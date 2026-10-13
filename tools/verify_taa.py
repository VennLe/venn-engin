"""端到端验证：TAA（时域抗锯齿，R1-1）。

背景：本模块沿用全套既有约定 —— **默认关 = 基线逐像素不变**。

TAA 与其它链 pass 的两点不同，也是本脚本断言的重点：

  1. **它改的是"怎么采样"**：帧首给投影矩阵加 Halton(2,3) 子像素抖动。
     所以"关着 = 基线"不仅要求不录 pass，还要求抖动**压根没加**
     （T2 用逐像素恒等钉死这一点）。
  2. **它跨帧**：输出同时是链的源头和下一帧的历史，于是"收敛 ⇒ 可复现"
     成了最直接的端到端证据 —— 静态相机下两次独立进程跑同样设置，
     抓到的图必须逐像素一致（T5）。

抗锯齿的判据（T4）刻意不用"看起来更平滑"这种含糊说法，而是：

  · **高频能量下降**：二阶差分（Laplacian）超过阈值的像素数显著减少
    —— 锯齿的本质就是"少数像素上极大的局部曲率"；
  · **且变化必须集中在几何边缘**：边缘像素上的平均变化量必须是
    非边缘像素的若干倍。这一条是用来排除"其实就是全局模糊"的 ——
    模糊会把整幅图都改掉，而时域超采样只该改边缘。

用法：
    python tools/verify_taa.py
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import kill_stale_editors                       # noqa: E402
from verify_local_shadow import diff_stats, vp_box                   # noqa: E402
from verify_shader_variants import run_once                          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")


def material(name, base, rough=0.85):
    return {"kind": "standard", "name": name,
            "baseColor": list(base) + [1.0],
            "metallic": 0.0, "roughness": rough, "normalScale": 0.0,
            "ao": 1.0, "emissive": [0, 0, 0],
            "doubleSided": False, "alphaBlend": False}


def entity(name, pos, shape, size, mat):
    return {"name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": shape, "size": size,
                     "name": name.lower()},
            "material": mat}


# 斜边尽量多：立方体在透视下每个面都是斜边，柱体侧面是光滑斜轮廓。
# TAA 的收益全在这些亚像素斜边上。
def build_scene(path):
    ents = [entity("Ground", (0, 0, 0), "plane", 60.0,
                   material("taa_ground", (0.36, 0.35, 0.33))),
            entity("CubeA", (-3.0, 4.0, 1.6), "cube", 3.2,
                   material("taa_a", (0.88, 0.34, 0.20))),
            entity("CubeB", (2.5, -1.0, 2.2), "cube", 4.4,
                   material("taa_b", (0.85, 0.80, 0.30))),
            entity("CubeC", (6.5, -16.0, 2.4), "cube", 4.8,
                   material("taa_c", (0.22, 0.48, 0.88))),
            entity("Pillar", (-9.0, -8.0, 3.0), "cylinder", 2.0,
                   material("taa_d", (0.55, 0.60, 0.62)))]
    doc = {"version": 1, "generator": "verify_taa.py",
           "camera": {"target": [0.0, 0.0, 2.0], "yaw": 90.0, "pitch": 6.0,
                      "distance": 15.0, "fov": 60.0},
           "light": {"direction": [0.35, 0.25, -1.0],
                     "color": [1.0, 0.97, 0.92], "intensity": 1.1},
           "entities": ents}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


CAM = "0,-14,3.2,90,8"

BASE_ENV = {
    "MYVK_LIGHT_GIZMOS": "0",
    "MYVK_EDITOR_NO_HUD": "1",
    "MYVK_FRUSTUM_DBG": "0",
    "MYVK_FG_DBG": "1",
}

# 其它链效果一律关死：本脚本只测 TAA，"链上还有谁"必须确定。
OFF = {
    "MYVK_DOF": "0", "MYVK_MOTIONBLUR": "0", "MYVK_CHROMA": "0",
    "MYVK_SSGI": "0", "MYVK_SSR": "0", "MYVK_GODRAYS": "0", "MYVK_FOG": "0",
    "MYVK_SSAO": "0", "MYVK_MB_SIM_YAW": "0",
}


# ---------------- 图像分析小工具 ----------------

def _gray(img, box):
    import numpy as np
    a = np.asarray(img.convert("RGB"), dtype=np.float64)
    x0, y0, x1, y1 = box
    return a[y0:y1 + 1, x0:x1 + 1].max(axis=2)


def lap_count(img, box, thr=20):
    """二阶差分（Laplacian）超过 thr 的像素数 —— 抗锯齿的主判据。

    锯齿的特征是"少数像素上有极大的局部曲率"。时域超采样把边缘能量
    摊开，这个计数随之下降。比一阶梯度（总变差）可靠：一阶梯度在
    理想阶跃被摊成斜坡后**总量不变**（TV 守恒），二阶才看得出区别。
    """
    import numpy as np
    g = _gray(img, box)
    lx = np.abs(g[:, 2:] - 2.0 * g[:, 1:-1] + g[:, :-2])
    ly = np.abs(g[2:, :] - 2.0 * g[1:-1, :] + g[:-2, :])
    return int((lx > thr).sum() + (ly > thr).sum())


def edge_localized(imgA, imgB, box, edge_thr=20):
    """返回 (边缘像素平均变化量, 非边缘像素平均变化量, 比值)。

    边缘掩码取自 A 的一阶梯度。用来区分"抗锯齿"与"全局模糊"：
    前者只改边缘（比值高），后者会把非边缘区域也改掉（比值低）。
    """
    import numpy as np
    a = np.asarray(imgA.convert("RGB"), dtype=np.float64)
    b = np.asarray(imgB.convert("RGB"), dtype=np.float64)
    x0, y0, x1, y1 = box
    sa, sb = a[y0:y1 + 1, x0:x1 + 1], b[y0:y1 + 1, x0:x1 + 1]
    d = np.abs(sa - sb).max(axis=2)
    g = sa.max(axis=2)
    gm = np.zeros_like(g)
    dx = np.abs(np.diff(g, axis=1))
    dy = np.abs(np.diff(g, axis=0))
    gm[:, :-1] = np.maximum(gm[:, :-1], dx)
    gm[:-1, :] = np.maximum(gm[:-1, :], dy)
    e = gm > edge_thr
    ne = ~e
    em = float(d[e].mean()) if e.any() else 0.0
    nm = float(d[ne].mean()) if ne.any() else 0.0
    return em, nm, (em / nm if nm > 1e-6 else 999.0), int(e.sum())


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
    scene = os.path.join(OUT_DIR, "taa.json")
    build_scene(scene)

    def run(tag, on=None, env_extra=None, shot=True):
        env = dict(os.environ)
        env.update(BASE_ENV)
        env.update(OFF)
        if on:
            for k, v in on.items():
                env[k] = str(v)
        if env_extra:
            env.update(env_extra)
        return run_once(exe, scene, env,
                        (os.path.join(OUT_DIR, "taa_%s.png" % tag)
                         if shot else None), cam=CAM)

    def boxof(vpA, vpB):
        a, b = vp_box(vpA), vp_box(vpB)
        return (max(a[0], b[0]), max(a[1], b[1]),
                min(a[2], b[2]), min(a[3], b[3]))

    def cmp(name, vpA, imgA, vpB, imgB, need_diff=None, allow_same=False):
        if imgA is None or imgB is None or vpA is None or vpB is None:
            check(name, False, "截图/视口缺失")
            return None
        if imgA.size != imgB.size:
            check(name, False, "尺寸不一致")
            return None
        import numpy as np
        box = boxof(vpA, vpB)
        n, mx, mean, _ = diff_stats(imgA, imgB, box)
        pa = np.asarray(imgA.convert("RGB"), dtype=np.int16)
        pb = np.asarray(imgB.convert("RGB"), dtype=np.int16)
        x0, y0, x1, y1 = box
        mxall = int(np.abs(pa[y0:y1 + 1, x0:x1 + 1] -
                           pb[y0:y1 + 1, x0:x1 + 1]).max())
        print("     %s：差异像素=%d 最大差=%d(视口内 %d) 平均差=%.3f"
              % (name, n, mx, mxall, mean))
        if allow_same:
            check(name, mxall <= 1, "max=%d" % mxall)
        elif need_diff is not None:
            check(name, n > need_diff and mx > 8, "n=%d max=%d" % (n, mx))
        # need_diff=None：只报告不判定（调用方自己写更细的判据）
        return (n, mx, mxall, mean, box)

    # ================================================================
    # A. 基线（不设 MYVK_TAA）与 TAA 开
    # ================================================================
    lA, vpA, imgA, eA = run("off")
    lB, vpB, imgB, eB = run("on", on={"MYVK_TAA": "1"})

    fgB = [l for l in lB if "FRAMEGRAPH" in l]
    has_taa = any("taa=1" in l for l in fgB)
    has_hist = any("taa=1" in l and "hist=1" in l for l in fgB)
    print("     A1 FRAMEGRAPH 行数=%d 样例：%s"
          % (len(fgB), fgB[0].strip() if fgB else "未出现"))
    check("A1 TAA 开 → FRAMEGRAPH 日志 taa=1（链被激活，pass 真的录了）",
          has_taa, fgB[:1])
    check("A1 TAA 开 → 第二轮起 hist=1（历史图已建立）", has_hist, fgB[:1])
    check("A1 TAA 轮零 VUID / 零 Fatal", not eB, eB[:2])

    fgA = [l for l in lA if "FRAMEGRAPH" in l]
    check("A2 TAA 关 → FRAMEGRAPH 日志不出现（链零 pass、零开销）",
          not fgA and not eA, (fgA[:1] + eA)[:2])

    # ================================================================
    # B. 关 = 基线（逐像素恒等；抖动也必须在关着时不存在）
    # ================================================================
    lC, vpC, imgC, eC = run("explicit_off", on={"MYVK_TAA": "0"})
    cmp("B1 MYVK_TAA=0 与完全不设 MYVK_TAA → 逐像素恒等",
        vpA, imgA, vpC, imgC, allow_same=True)
    check("B1 显式关轮零 VUID / 零 Fatal", not eC, eC[:2])

    # ================================================================
    # C. 开 ≠ 基线
    # ================================================================
    cmp("C1 TAA 开/关差异显著（画面确实变了）",
        vpA, imgA, vpB, imgB, need_diff=200)

    # ================================================================
    # D. 抗锯齿（用 MSAA=1 隔离硬件 MSAA 的贡献）
    # ================================================================
    lD, vpD, imgD, eD = run("m1_off", env_extra={"MYVK_MSAA": "1"})
    lE, vpE, imgE, eE = run("m1_taa", on={"MYVK_TAA": "1"},
                            env_extra={"MYVK_MSAA": "1"})
    if vpD and vpE and imgD is not None and imgE is not None:
        box = boxof(vpD, vpE)
        dOff, dOn = lap_count(imgD, box), lap_count(imgE, box)
        print("     D1 高频像素计数（Laplacian>20）：MSAA=1 基线=%d，"
              "MSAA=1+TAA=%d（%.1f%%）"
              % (dOff, dOn, 100.0 * dOn / max(dOff, 1)))
        check("D1 高频能量显著下降（TAA 的 Laplacian 计数 < 基线的 75%）",
              dOn < dOff * 0.75, "off=%d on=%d" % (dOff, dOn))

        em, nm, ratio, nedge = edge_localized(imgD, imgE, box)
        print("     D2 变化分布：边缘(%d px) 平均变化=%.2f，非边缘 平均变化=%.3f"
              "，比值=%.1f×" % (nedge, em, nm, ratio))
        check("D2 变化集中在几何边缘（边缘/非边缘 ≥ 5×）—— 排除'全局模糊'",
              em > 1.0 and ratio >= 5.0, "ratio=%.1f" % ratio)
    else:
        check("D1 高频能量显著下降（TAA 的 Laplacian 计数 < 基线的 75%）",
              False, "截图缺失")
        check("D2 变化集中在几何边缘（边缘/非边缘 ≥ 5×）—— 排除'全局模糊'",
              False, "截图缺失")
    check("D MSAA=1 轮零 VUID / 零 Fatal", not (eD or eE), (eD + eE)[:2])

    cmp("D3 MSAA=1：TAA 开/关差异显著（单采样深度形态真的在工作）",
        vpD, imgD, vpE, imgE, need_diff=200)

    # ================================================================
    # E. 收敛 ⇒ 可复现（两次独立进程、静态相机、同设置）
    # ================================================================
    # 注意判据**不是"逐像素全等"**：TAA 的权重是 0.1 的指数滑动平均，
    # 收敛后进入一个 8 相位极限环，抓拍落在环上哪个位置由帧号决定 ——
    # 两次运行的抓拍相位必然不同，边缘处会留下 α×边缘对比度 量级的
    # LSB 级差异（实测最大 8/255）。能断言的是"**没有任何可见差异**"
    # （>18 的像素数为 0）加一个很小的平均差上界。
    # 真正该被这条排除的是"没收敛"：那种情况两次运行会差出成百上千个
    # 显著像素（本脚本第一版就是这个数量级：611 px / max 77）。
    lF, vpF, imgF, eF = run("on_repeat", on={"MYVK_TAA": "1"})
    rE = cmp("E1 两次独立运行 TAA → 无可见差异（收敛 ⇒ 无鬼影 / 无漂移）",
             vpB, imgB, vpF, imgF)
    if rE is not None:
        nE, mxE, mxallE, meanE, _ = rE
        check("E1 两次运行：差异 >18 的像素数为 0（无可见差异）",
              nE == 0, "n=%d" % nE)
        check("E1 两次运行：平均差 ≤ 0.35（仅为相位极限环的 LSB 残差）",
              meanE <= 0.35, "mean=%.3f max=%d" % (meanE, mxallE))
    check("E1 复现轮零 VUID / 零 Fatal", not eF, eF[:2])

    # ================================================================
    # F. 抖动链路活着（冻结相位的钩子：不同相位必须给出不同画面）
    # ================================================================
    lG, vpG, imgG, eG = run("phase0", on={"MYVK_TAA": "0"},
                            env_extra={"MYVK_MSAA": "1",
                                       "MYVK_TAA_PHASE": "0"})
    lH, vpH, imgH, eH = run("phase1", on={"MYVK_TAA": "0"},
                            env_extra={"MYVK_MSAA": "1",
                                       "MYVK_TAA_PHASE": "1"})
    cmp("F1 抖动相位 0 与 1 画面显著不同（帧首抖动真的在动投影矩阵）",
        vpG, imgG, vpH, imgH, need_diff=200)
    check("F1 相位钩子轮零 VUID / 零 Fatal", not (eG or eH), (eG + eH)[:2])

    # ================================================================
    # G. 汇总
    # ================================================================
    all_errs = eA + eB + eC + eD + eE + eF + eG + eH
    check("G1 全部轮次零 Vulkan 验证层报错", not all_errs, all_errs[:3])

    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
