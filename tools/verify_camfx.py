"""端到端验证：多相机 / 通用 RenderTarget + 相机特效（DoF / 运动模糊 / 色差 / 暗角 / 后处理体积）。

背景：这一批的硬约定和屏幕空间效果完全一致 —— **默认关 = 基线逐像素不变**。
每个效果必须满足对照实验：开着时画面显著不同、关着时与基线逐位一致。

落点（见 render/CameraEffects.h / FrameGraph.h / RenderTarget.h）：

  · 景深 DoF / 运动模糊 MB  → FrameGraph 链的最后两个 pass（前向之后，
    要读深度与相机矩阵）
  · 色差 / 暗角             → PostProcess 合成（tonemap 后的显示域）
  · 后处理体积              → 每帧按相机位置求值，叠加到上面的基础设置
  · 多相机 / 通用 RT        → 相机由 Scene 持有（camera(i)），
                              离屏目标由 Renderer 的 m_targets 持有

断言分组：

  A. 景深
     A1 开/关差异显著（MSAA=4）；A2 MSAA=1 形态同样显著；
     A3 MYVK_DOF_RADIUS 极小 → 逐像素恒等（"不对焦区就不碰"的短路路径）。
  B. 运动模糊
     B1 **相机静止 + 开着 → 与关着逐像素一致**（速度恒 0 短路，
        这是"关着 = 基线"之外的第二条恒等保证）；
     B2 人为角速度（MYVK_MB_SIM_YAW）→ 显著差异；
     B3 强度单调：0.5 < 2.0 的差异像素数递增（强度真的在起作用）；
     B4 MSAA=1 形态同样工作。
  C. 色差
     C1 开/关差异显著，且差异**集中在画面外围**（径向分离的必然结果：
        中心处 RGB 三个采样点几乎重合）。中心区差异像素数必须远小于外圈。
  D. 暗角
     D1 强度 0 → 0.6 差异显著，且**四角平均亮度下降**（方向性断言）。
  E. 后处理体积
     E1 相机在体积内 → 与"基础设置直接设成同一值"**逐像素一致**
        （体积求值正确的强证据）；
     E2 相机在体积外 → 回到基线（与无体积场景一致）。
  F. 基线 / 零开销
     F1 全关时 FRAMEGRAPH 日志不出现（链零 pass）；
     F2 全关基线 <= 显式关掉默认暗角（证明默认 0.25 的暗角确实在基线里）。
  G. 多相机 / 通用 RenderTarget
     G1 两台相机（场景 JSON）→ MYVK_VIEW_CAM=0/1 画面显著不同；
     G2 MYVK_EXTRA_VIEW=1 → RT-TARGETS count=3，零 VUID。

全程零 Vulkan 验证层报错。用法：
    python tools/verify_camfx.py
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import kill_stale_editors                       # noqa: E402
from verify_local_shadow import diff_stats, region_diff, vp_box      # noqa: E402
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


# 三个深度层次分明的物体 + 一大片地面：DoF 要有"近/中/远"才看得出模糊梯度，
# 色差/暗角需要画面铺满（地面从天到地占满整屏，径向效果才有外围可看）。
def build_scene(path, extra=None):
    ents = [entity("Ground", (0, 0, 0), "plane", 60.0,
                   material("cfx_ground", (0.38, 0.36, 0.33))),
            entity("NearCube", (4.0, 4.0, 2.0), "cube", 4.0,
                   material("cfx_near", (0.90, 0.36, 0.20))),
            entity("MidCube", (0.0, 0.0, 2.5), "cube", 4.0,
                   material("cfx_mid", (0.85, 0.80, 0.30))),
            entity("FarCube", (-9.0, -22.0, 3.0), "cube", 5.0,
                   material("cfx_far", (0.22, 0.48, 0.88)))]
    doc = {"version": 1, "generator": "verify_camfx.py",
           "camera": {"target": [0.0, 0.0, 2.0], "yaw": 90.0, "pitch": 8.0,
                      "distance": 14.0, "fov": 60.0},
           "light": {"direction": [0.35, 0.25, -1.0],
                     "color": [1.0, 0.97, 0.92], "intensity": 1.1}}
    if extra:
        doc.update(extra)
    doc["entities"] = ents
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


CAM = "0,-14,3.2,90,8"

BASE_ENV = {
    "MYVK_LIGHT_GIZMOS": "0",
    "MYVK_EDITOR_NO_HUD": "1",
    "MYVK_FRUSTUM_DBG": "0",
}

# 所有相机关效开关的"全关"基线（暗角例外：它默认 0.25，是引入本模块前的既有值）
OFF = {
    "MYVK_DOF": "0", "MYVK_MOTIONBLUR": "0", "MYVK_CHROMA": "0",
    "MYVK_MB_SIM_YAW": "0",
}


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
    scene = os.path.join(OUT_DIR, "camfx.json")
    build_scene(scene)

    def run(tag, on=None, env_extra=None, sc=None, cam=CAM, shot=True):
        env = dict(os.environ)
        env.update(BASE_ENV)
        env.update(OFF)
        env["MYVK_FG_DBG"] = "1"
        if on:
            for k, v in on.items():
                env[k] = str(v)
        if env_extra:
            env.update(env_extra)
        return run_once(exe, sc or scene, env,
                        (os.path.join(OUT_DIR, "cfx_%s.png" % tag)
                         if shot else None), cam=cam)

    def diff(name, vpA, imgA, vpB, imgB, min_n, min_mx=8):
        if imgA is None or imgB is None or vpA is None or vpB is None:
            check(name, False, "截图/视口缺失")
            return 0, 0, 0.0
        a, b = vp_box(vpA), vp_box(vpB)
        box = (max(a[0], b[0]), max(a[1], b[1]),
               min(a[2], b[2]), min(a[3], b[3]))
        n, mx, mean, _ = diff_stats(imgA, imgB, box)
        print("     %s：差异像素=%d 最大差=%d 平均差=%.2f" % (name, n, mx, mean))
        check(name, n > min_n and mx > min_mx, "n=%d max=%d" % (n, mx))
        return n, mx, mean

    def same(name, vpA, imgA, vpB, imgB):
        """逐像素恒等断言：最大差必须 <= 1（8bit 量化下允许 1 个 LSB）。"""
        if imgA is None or imgB is None or vpA is None or vpB is None:
            check(name, False, "截图/视口缺失")
            return
        a, b = vp_box(vpA), vp_box(vpB)
        box = (max(a[0], b[0]), max(a[1], b[1]),
               min(a[2], b[2]), min(a[3], b[3]))
        n, mx, mean, _ = diff_stats(imgA, imgB, box)
        # 只看 >18 的"可见差异"，另外再单独断言最大差 <= 1
        if imgA.size != imgB.size:
            check(name, False, "尺寸不一致")
            return
        import numpy as np
        pa = np.asarray(imgA.convert("RGB"), dtype=np.int16)
        pb = np.asarray(imgB.convert("RGB"), dtype=np.int16)
        x0, y0, x1, y1 = box
        sub = np.abs(pa[y0:y1 + 1, x0:x1 + 1] - pb[y0:y1 + 1, x0:x1 + 1])
        mxall = int(sub.max())
        print("     %s：视口内最大差=%d（>18 的像素=%d）" % (name, mxall, n))
        check(name, mxall <= 1, "max=%d n=%d" % (mxall, n))

    # ================================================================
    # A. 景深
    # ================================================================
    _, vpA, imgA, eA = run("base")
    _, vpB, imgB, eB = run("dof", on={"MYVK_DOF": "1"},
                           env_extra={"MYVK_DOF_FOCUS": "6",
                                      "MYVK_DOF_APERTURE": "1.6",
                                      "MYVK_DOF_RADIUS": "14"})
    diff("A1 景深 开/关差异显著", vpA, imgA, vpB, imgB, 3000)
    check("A1 景深轮零 VUID / 零 Fatal", not (eA or eB), (eA + eB)[:2])

    _, vpC, imgC, eC = run("base_m1", env_extra={"MYVK_MSAA": "1"})
    _, vpD, imgD, eD = run("dof_m1", on={"MYVK_DOF": "1"},
                           env_extra={"MYVK_MSAA": "1",
                                      "MYVK_DOF_FOCUS": "6",
                                      "MYVK_DOF_APERTURE": "1.6",
                                      "MYVK_DOF_RADIUS": "14"})
    diff("A2 景深 开/关差异显著（MSAA=1 单采样深度形态）",
         vpC, imgC, vpD, imgD, 3000)
    check("A2 景深 MSAA=1 轮零 VUID / 零 Fatal", not (eC or eD), (eC + eD)[:2])

    # 半径上限压到 0.5 px → 每个像素的 CoC 半径都 < 0.75 → 走"完全不碰"短路
    _, vpE, imgE, eE = run("dof_eps", on={"MYVK_DOF": "1"},
                           env_extra={"MYVK_DOF_FOCUS": "6",
                                      "MYVK_DOF_APERTURE": "1.6",
                                      "MYVK_DOF_RADIUS": "0.5"})
    same("A3 景深半径≈0 → 逐像素不变（对焦区短路路径）",
         vpA, imgA, vpE, imgE)
    check("A3 景深半径≈0 轮零 VUID / 零 Fatal", not eE, eE[:2])

    # ================================================================
    # B. 运动模糊
    # ================================================================
    # B1：静态相机 + MB 开着 → 重投影速度恒 0 → 逐像素与关着一致。
    _, vpF, imgF, eF = run("mb_static", on={"MYVK_MOTIONBLUR": "1"})
    same("B1 相机静止 + 运动模糊开着 → 逐像素一致（速度 0 短路）",
         vpA, imgA, vpF, imgF)
    check("B1 静态运动模糊轮零 VUID / 零 Fatal", not eF, eF[:2])

    # B2/B3：人为角速度（静态相机下制造可复现的相机运动）
    _, vpG, imgG, eG = run("mb_sim", on={"MYVK_MOTIONBLUR": "1"},
                           env_extra={"MYVK_MB_SIM_YAW": "1.5",
                                      "MYVK_MB_STRENGTH": "1.0"})
    nG, mxG, meanG = diff("B2 人为角速度 → 运动模糊差异显著",
                          vpA, imgA, vpG, imgG, 3000)
    check("B2 运动模糊轮零 VUID / 零 Fatal", not (eG), eG[:2])

    _, vpH, imgH, eH = run("mb_sim_low", on={"MYVK_MOTIONBLUR": "1"},
                           env_extra={"MYVK_MB_SIM_YAW": "1.5",
                                      "MYVK_MB_STRENGTH": "0.4"})
    nH, mxH, meanH = diff("B3 强度 0.4 → 差异（对照组）",
                          vpA, imgA, vpH, imgH, 500)
    # ⚠ 判据用**平均差**而不是">18 的像素数"：0.4 与 1.0 都会把同一批
    #   边缘像素推动到阈值之上，计数会饱和（实测都等于 7240）。平均差是
    #   线性量，才反映"强度真的在缩放速度"。
    print("     B3 单调性：strength 0.4 → mean=%.3f，1.0 → mean=%.3f"
          % (meanH, meanG))
    check("B3 运动模糊强度单调（0.4 的平均差 < 1.0）", meanH < meanG,
          "mean(0.4)=%.3f mean(1.0)=%.3f" % (meanH, meanG))
    check("B3 低强度轮零 VUID / 零 Fatal", not eH, eH[:2])

    _, vpI, imgI, eI = run("mb_sim_m1", on={"MYVK_MOTIONBLUR": "1"},
                           env_extra={"MYVK_MSAA": "1",
                                      "MYVK_MB_SIM_YAW": "1.5"})
    diff("B4 运动模糊（MSAA=1 单采样深度形态）", vpC, imgC, vpI, imgI, 3000)
    check("B4 运动模糊 MSAA=1 轮零 VUID / 零 Fatal", not eI, eI[:2])

    # ================================================================
    # C. 色差
    # ================================================================
    _, vpJ, imgJ, eJ = run("chroma", on={"MYVK_CHROMA": "1"},
                           env_extra={"MYVK_CHROMA_STRENGTH": "3.0"})
    diff("C1 色差 开/关差异显著", vpA, imgA, vpJ, imgJ, 1000)
    check("C1 色差轮零 VUID / 零 Fatal", not eJ, eJ[:2])

    # 径向分离：中心区几乎不动，外圈差异明显。
    if vpA and vpJ and imgA is not None and imgJ is not None:
        x0, y0, x1, y1 = vp_box(vpA)
        cx = (x0 + x1) // 2
        cy = (y0 + y1) // 2
        r = min(x1 - x0, y1 - y0) // 6          # 中心 1/3 半径的方框
        inner = (cx - r, cy - r, cx + r, cy + r)
        outer = (x0, y0, x1, y1)
        ni, _, _, _ = diff_stats(imgA, imgJ, inner)
        no, _, _, _ = diff_stats(imgA, imgJ, outer)
        print("     C1 色差分布：中心区差异=%d，整视口=%d" % (ni, no))
        check("C1 色差差异集中在画面外围（中心区远小于整幅）",
              ni * 4 < no, "inner=%d outer=%d" % (ni, no))

    # ================================================================
    # D. 暗角
    # ================================================================
    _, vpK, imgK, eK = run("vig_off", on={"MYVK_VIGNETTE_STRENGTH": "0.0"})
    _, vpL, imgL, eL = run("vig_on", on={"MYVK_VIGNETTE_STRENGTH": "0.6"})
    diff("D1 暗角 0 → 0.6 差异显著", vpK, imgK, vpL, imgL, 3000)
    check("D1 暗角轮零 VUID / 零 Fatal", not (eK or eL), (eK + eL)[:2])

    # 方向性：四角必须**变暗**。
    if vpK and vpL and imgK is not None and imgL is not None:
        x0, y0, x1, y1 = vp_box(vpK)
        w = x1 - x0
        h = y1 - y0
        corners = [(x0, y0, x0 + w // 5, y0 + h // 5),
                   (x1 - w // 5, y0, x1, y0 + h // 5),
                   (x0, y1 - h // 5, x0 + w // 5, y1),
                   (x1 - w // 5, y1 - h // 5, x1, y1)]
        dark = 0
        for cbox in corners:
            d = region_diff(imgL, imgK, cbox)   # 正值期望（L 更暗 → 差为正；此处用绝对值）
            # region_diff 是平均绝对差，方向要单独看均值
            import numpy as np
            pk = np.asarray(imgK.convert("RGB"), dtype=np.float64)
            pl = np.asarray(imgL.convert("RGB"), dtype=np.float64)
            aa = pk[cbox[1]:cbox[3], cbox[0]:cbox[2]].mean()
            bb = pl[cbox[1]:cbox[3], cbox[0]:cbox[2]].mean()
            if bb < aa - 1.0:
                dark += 1
        print("     D1 四角变暗计数：%d/4" % dark)
        check("D1 暗角方向性：四角平均亮度下降", dark == 4, "%d/4" % dark)

    # ================================================================
    # E. 后处理体积
    # ================================================================
    # 体积罩住相机（相机 eye=(0,-14,3.2)），覆盖 vignetteStrength = 0.6。
    vol_scene = os.path.join(OUT_DIR, "camfx_vol.json")
    build_scene(vol_scene, extra={
        "postVolumes": [{
            "name": "VigVolume",
            "boundsMin": [-20, -26, -5], "boundsMax": [20, 20, 25],
            "priority": 1.0, "blendDistance": 2.0,
            "vignetteStrength": 0.6, "vignette": 1,
        }]})
    _, vpM, imgM, eM = run("vol_on", sc=vol_scene)
    same("E1 相机在体积内 → 与基础值直接设成同一值逐像素一致",
         vpL, imgL, vpM, imgM)
    check("E1 体积生效轮零 VUID / 零 Fatal", not eM, eM[:2])

    # 体积挪到相机到不了的地方 → 求值权重 0 → 回到基线
    vol_far = os.path.join(OUT_DIR, "camfx_vol_far.json")
    build_scene(vol_far, extra={
        "postVolumes": [{
            "name": "FarVolume",
            "boundsMin": [200, 200, 200], "boundsMax": [210, 210, 210],
            "priority": 1.0, "blendDistance": 2.0,
            "vignetteStrength": 0.6, "vignette": 1,
        }]})
    _, vpN, imgN, eN = run("vol_far", sc=vol_far)
    same("E2 相机在体积外 → 回到基线（与无体积场景逐像素一致）",
         vpA, imgA, vpN, imgN)
    check("E2 体积不在场轮零 VUID / 零 Fatal", not eN, eN[:2])

    # ================================================================
    # F. 基线 / 零开销
    # ================================================================
    lF, _, _, eF2 = run("all_off_dbg", shot=False)
    fg = [l for l in lF if "FRAMEGRAPH" in l]
    check("F1 全关时 FRAMEGRAPH 日志不出现（链零 pass、零开销）",
          not fg and not eF2, (fg + eF2)[:2])

    # F2：默认暗角在基线里 —— 显式关掉它应当产生差异
    _, vpO, imgO, eO = run("vig_default_off", on={"MYVK_VIGNETTE_OFF": "1"})
    diff("F2 默认暗角(0.25)确实在基线里（关掉它画面有差异）",
         vpA, imgA, vpO, imgO, 500)
    check("F2 关暗角轮零 VUID / 零 Fatal", not eO, eO[:2])

    # ================================================================
    # G. 多相机 / 通用 RenderTarget
    # ================================================================
    cam_scene = os.path.join(OUT_DIR, "camfx_cams.json")
    # camera = 0 号（由顶层 camera 字段钉住）；cameras[0] = 1 号（从对面看）
    build_scene(cam_scene, extra={
        "cameras": [{"target": [0.0, 0.0, 2.0], "yaw": 270.0, "pitch": -12.0,
                     "distance": 20.0, "fov": 60.0}]})
    _, vpP, imgP, eP = run("cam0", sc=cam_scene)
    lQ, vpQ, imgQ, eQ = run("cam1", sc=cam_scene,
                            env_extra={"MYVK_VIEW_CAM": "1"})
    diff("G1 多相机：VIEW_CAM=0 → 1 画面显著不同", vpP, imgP, vpQ, imgQ, 3000)
    check("G1 多相机轮零 VUID / 零 Fatal", not (eP or eQ), (eP + eQ)[:2])

    lR, _, _, eR = run("extra_view", env_extra={"MYVK_EXTRA_VIEW": "1"},
                       shot=False)
    rt = [l for l in lR if "RT-TARGETS" in l]
    ok_rt = bool(rt) and "count=2" in rt[0] and "extra=1" in rt[0]
    print("     G2 RT-TARGETS 日志：%s" % (rt[0].strip() if rt else "未出现"))
    check("G2 通用 RenderTarget：EXTRA_VIEW=1 → 多出 1 个目标且零报错",
          ok_rt and not eR, (rt[:1] + eR)[:2])

    # 对照组：不开 EXTRA_VIEW 时只有 main 一个目标（"新增视角只付一次
    # createRenderTarget()" 的量化证据）。
    lS, _, _, eS = run("no_extra", shot=False)
    rt2 = [l for l in lS if "RT-TARGETS" in l]
    ok_rt2 = bool(rt2) and "count=1" in rt2[0] and "extra=0" in rt2[0]
    print("     G2 对照 RT-TARGETS：%s" % (rt2[0].strip() if rt2 else "未出现"))
    check("G2 对照：不开 EXTRA_VIEW → 只有 main", ok_rt2 and not eS,
          (rt2[:1] + eS)[:2])

    # 汇总行：run_regression.py 靠它判定 ok
    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
