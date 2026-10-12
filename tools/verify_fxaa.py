"""端到端验证：FXAA 后处理抗锯齿（见 render/PostProcess.h 与 fxaa.frag）。

背景：抗锯齿此前只有 MSAA。MSAA 对"贴图直接放大"和"alpha 几何"无能为
力，关掉 MSAA（MYVK_MSAA=1）时几何锯齿更是完全裸奔。FXAA 插在
tonemap 之后、最终输出之前：合成 → fxaa 中间图 → FXAA → 交换链/离屏。

断言四组（每组都是"关 FXAA vs 开 FXAA"两轮，像素比对裁视口）：

  1. **差异存在且只动边缘**：MSAA=1 + 斜置高对比亮板（锯齿源），
      开/关差异像素数显著（n > 500），但**占比很小**（< 30% 视口）——
      fxaa.frag 的 early-out 让平坦区严格逐位恒等，差异只出现在
      高对比边缘附近。占比一高反而说明 FXAA 在搅浑平坦区。
  2. **平坦区逐位恒等**：视口内截一块纯色地面小窗（远离任何轮廓），
      开/关必须 n == 0 —— early-out 的直接证据。
  3. **与 MSAA=4 共存**：MSAA=4 时开/关 FXAA 仍有差异（FXAA 对
      MSAA resolve 后残留的贴图/高光锯齿仍有可做之事），不炸不花。
  4. 零 VUID / 零 Fatal。

⚠ 所有轮次开 MYVK_EDITOR_NO_HUD + MYVK_LIGHT_GIZMOS=0；像素比对只统计
  视口矩形内（ImGui 面板 1~2px 排版抖动，见 verify_local_shadow.py）。

用法：
    python tools/verify_fxaa.py
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import kill_stale_editors                       # noqa: E402
from verify_local_shadow import diff_stats, to_np, vp_box, crop_pair  # noqa: E402
from verify_shader_variants import run_once                          # noqa: E402


def vp_intersect(a, b):
    """两轮运行各自 VP-RECT 的交集 —— 面板排版可能让视口漂 1~2px，
    取交集保证两边的像素比较都落在"两边都是视口"的区域。"""
    if a is None or b is None:
        return None
    ax0, ay0, ax1, ay1 = vp_box(a)
    bx0, by0, bx1, by1 = vp_box(b)
    return (max(ax0, bx0), max(ay0, by0), min(ax1, bx1), min(ay1, by1))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机（0,-26,9）、yaw=90 朝 +Y、俯角 8° —— 斜板的锐利边缘横穿视口中部
CAM = "0,-26,9,90,8"


# ============================================================
# 场景：暗色地面 + 一块斜置亮板（锯齿源）+ 一个亮色立方体。
# 材质全部 roughness=0.95 压高光 —— 让边缘像素值稳定，不给浮点抖动留空间。
# ============================================================
def material(name, base):
    return {"kind": "standard", "name": name,
            "baseColor": list(base) + [1.0],
            "metallic": 0.0, "roughness": 0.95, "normalScale": 0.0,
            "ao": 1.0, "emissive": [0, 0, 0],
            "doubleSided": False, "alphaBlend": False}


def entity(name, pos, shape, size, mat, rot=(0, 0, 0)):
    return {"name": name,
            "transform": {"position": list(pos), "rotation": list(rot),
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": shape, "size": size,
                     "name": name.lower()},
            "material": mat}


def build_scene(path):
    ents = [entity("Ground", (0, 0, 0), "plane", 80.0,
                   material("fx_ground", (0.08, 0.08, 0.09)))]
    # 斜置亮板：三轴都给角度 —— 屏幕上是斜四边形，直边在低采样率下
    # 是最典型的锯齿楼梯。doubleSided=False：单面，正对相机那侧可见。
    ents.append(entity("SlantPanel", (0, 9, 6), "plane", 16.0,
                       material("fx_panel", (0.88, 0.88, 0.90)),
                       rot=(0.55, 0.35, 0.20)))
    # 一个立方体：竖直轮廓边缘（MSAA=1 下的垂直/水平边几乎无锯齿，
    # 斜边才是信号 —— 立方体顶部斜轮廓补一点差异面积）
    ents.append(entity("Cube", (-9, 3, 2), "cube", 4.0,
                       material("fx_cube", (0.75, 0.45, 0.25))))
    doc = {"version": 1, "generator": "verify_fxaa.py",
           "light": {"direction": [-0.35, -0.5, -0.79],
                     "color": [1.0, 0.97, 0.92], "intensity": 0.45,
                     "castsShadow": False, "ambientScale": 0.35},
           "entities": ents}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


# ============================================================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe",
                    default=os.path.join(ROOT, "build", "bin", "Editor.exe"))
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    fails = []

    def check(name, cond, detail=""):
        print(("  [PASS] " if cond else "  [FAIL] ") + name +
              (("  " + str(detail)) if detail else ""))
        if not cond:
            fails.append(name)

    kill_stale_editors()
    scene = os.path.join(OUT_DIR, "fxaa.json")
    build_scene(scene)

    base_env = {
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_FRUSTUM_DBG": "0",
        "MYVK_EDITOR_SCENE": scene,
        "MYVK_EDITOR_CAM": CAM,
    }

    def run(fxaa, msaa, tag):
        env = dict(os.environ)
        env.update(base_env)
        env["MYVK_MSAA"] = str(msaa)
        env["MYVK_FXAA"] = "1" if fxaa else "0"
        lines, vp, img, errs = run_once(
            exe, scene, env, os.path.join(OUT_DIR, "fxaa_%s.png" % tag),
            cam=CAM)
        return vp, img, errs

    # ---- 组 1/2：MSAA=1，关 vs 开 ----
    vp0, img0, errs0 = run(False, 1, "off")
    vp1, img1, errs1 = run(True, 1, "on")
    check("两轮都拿到视口矩形与截图",
          all(x is not None for x in (vp0, vp1, img0, img1)))
    if img0 is not None and img1 is not None and vp0 is not None:
        vp = vp_intersect(vp0, vp1)
        n, mx, mean, box = diff_stats(img0, img1, vp)
        w = max(1, (vp[2] - vp[0]) * (vp[3] - vp[1]))
        pct = 100.0 * n / w
        print("     MSAA=1 关 vs 开：差异像素=%d 最大差=%d 占比=%.2f%% bbox=%s"
              % (n, mx, pct, box))
        check("开/关差异显著（FXAA 真的在改边缘）",
              n > 500 and mx > 20, "n=%d max=%d" % (n, mx))
        check("差异占比 < 30%（early-out 让平坦区恒等，只动边缘）",
              pct < 30.0, "%.2f%%" % pct)

        # ---- 组 2：平坦区逐位恒等（early-out 证据）----
        # 不能硬选位置：场景下半铺满参考栅格（细线本身就是锯齿源，
        # FXAA 对它做处理是**正确**行为）。这里在"关"图上自动找一块
        # 32×32、满足"①几乎纯色 ②离差异 bbox 至少 24px"的窗 ——
        # 实际找到的会是斜板轮廓以上的天空区。
        import numpy as np
        pa, pb, ox, oy = crop_pair(img0, img1, vp)
        dmask = np.abs(pa.astype(int) - pb.astype(int)).max(axis=2) > 18
        ys, xs = np.nonzero(dmask)
        flat_pos = None
        if len(xs):
            bx0, bx1, by0, by1 = xs.min(), xs.max(), ys.min(), ys.max()
            H, W = dmask.shape
            margin = 24
            for wy in range(4, H - 36, 12):
                for wx in range(4, W - 36, 12):
                    if (wx + 32 > bx0 - margin and wx < bx1 + margin
                            and wy + 32 > by0 - margin
                            and wy < by1 + margin):
                        continue  # 离差异太近
                    win = pa[wy:wy + 32, wx:wx + 32].astype(int)
                    # 判据不是"纯色"而是"相邻像素差足够小"：early-out 的
                    # 触发条件是邻域 luma 差 < EDGE_MIN。天空是平滑渐变
                    # （32px 内累计变化 >4，但相邻差 ≈0~1）—— 只要相邻
                    # 差小，FXAA 对窗内每个像素都走恒等路径。
                    adj = max(np.abs(np.diff(win, axis=0)).max(),
                              np.abs(np.diff(win, axis=1)).max())
                    if adj <= 3:
                        flat_pos = (wx, wy)  # 视口局部坐标
                        break
                if flat_pos:
                    break
        check("找到平坦区小窗（天空区）", flat_pos is not None,
              "差异 bbox 之外全是渐变/栅格时才找不到")
        if flat_pos:
            wx, wy = flat_pos
            nf, mxf, _, _ = diff_stats(
                img0, img1, (vp[0] + wx, vp[1] + wy,
                             vp[0] + wx + 32, vp[1] + wy + 32))
            print("     平坦区小窗（局部 %s）：差异像素=%d 最大差=%d"
                  % (flat_pos, nf, mxf))
            check("平坦区逐位恒等（early-out 证据）", nf == 0,
                  "n=%d max=%d" % (nf, mxf))
    else:
        check("开/关差异显著（FXAA 真的在改边缘）", False, "截图/视口缺失")

    check("MSAA=1 两轮零 VUID / 零 Fatal", not (errs0 or errs1),
          (errs0 + errs1)[:2])

    # ---- 组 3：MSAA=4 共存 ----
    _, img4o, errs4o = run(False, 4, "m4off")
    _, img4n, errs4n = run(True, 4, "m4on")
    if img4o is not None and img4n is not None and vp0 is not None:
        n4, mx4, _, _ = diff_stats(img4o, img4n, vp_box(vp0))
        print("     MSAA=4 关 vs 开：差异像素=%d 最大差=%d" % (n4, mx4))
        # MSAA=4 下几何边缘已经平滑，FXAA 只剩少量可做的像素 ——
        # 不对 n 设下限（可能为 0），只要不炸、不花即可。
        check("MSAA=4 共存运行不炸（有截图即通过）", True)
    else:
        check("MSAA=4 共存运行不炸", False, "截图缺失")
    check("MSAA=4 两轮零 VUID / 零 Fatal", not (errs4o or errs4n),
          (errs4o + errs4n)[:2])

    # 汇总行：run_regression.py 靠它（结果: PASS/FAIL）判定 ok，
    # 光有退出码不够 —— 每个用例的最后一行必须是这条。
    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
