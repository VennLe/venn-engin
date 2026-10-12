"""端到端验证：屏幕空间效果全家桶（SSAO / 高度雾 / FrameGraph 链）。

背景：这批效果的硬约定是"默认全关 = 基线逐像素不变"。每个效果
必须满足对照实验：开着时画面**显著不同**、关着时与基线逐位一致。
接线语义（见 render/ScreenSpaceSettings.h / FrameGraph.h）：

  · SSAO      深度预通道后、前向前；pbr.frag binding 14 乘进环境光
  · 高度雾    FrameUBO.fogParams → pbr.frag 尾部混合（雾色 = 天空立方体）
  · SSGI/SSR/GODRAYS  FrameGraph 链（前向后、PostProcess 前）；
              全关时链零 pass，PostProcess 直接采 HDR

断言六组：

  1. **SSAO 开/关差异显著**（MSAA=4，走 sampler2DMS 深度形态）：
      贴地立方体根部的环境光遮蔽让地面角落变暗 → 差异像素显著。
  2. SSAO 同样显著（MSAA=1，走 sampler2D 单采样形态）——
      两种着色器形态都要真的在干活。
  3. **高度雾开/关差异显著**：密度拉大（MYVK_FOG_DENSITY=0.15），
      远处物体明显淡入天空色 → 差异像素占比大（雾盖整片远景）。
  4. **God rays 开/关差异显著**：相机对准太阳（光柱源在画内），
      径向光柱叠加 → 差异显著；MYVK_FG_DBG 日志出现 passes=1。
  5. **链全开（SSGI+SSR+GODRAYS）**：FRAMEGRAPH passes=3 日志出现、
      画面有差异、不炸不花。
  6. **全关零链开销**：MYVK_FG_DBG=1 跑全关轮，日志**不得出现**
      FRAMEGRAPH 行（0 pass 时不录也不打）——"全关零开销"的证据。

每轮零 VUID / 零 Fatal。用法：
    python tools/verify_screenspace.py
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import kill_stale_editors                       # noqa: E402
from verify_local_shadow import diff_stats, vp_box                  # noqa: E402
from verify_shader_variants import run_once                         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")


# 近景构图：立方体贴地（AO 信号源）+ 一根柱子（雾的纵深参照）。
def material(name, base, rough=0.9):
    return {"kind": "standard", "name": name,
            "baseColor": list(base) + [1.0],
            "metallic": 0.0, "roughness": rough, "normalScale": 0.0,
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
    ents = [entity("Ground", (0, 0, 0), "plane", 60.0,
                   material("ssp_ground", (0.35, 0.33, 0.30))),
            # 贴地立方体：根部一圈 AO 最浓的位置
            entity("AoCube", (-2.5, 2.5, 2), "cube", 4.0,
                   material("ssp_cube", (0.72, 0.42, 0.25))),
            # 高柱：雾的近/远参照（顶远底近）。size 只吃标量 ——
            # 拉长用 scale（z×6），贴地摆 z=半高×scale=6。
            entity("FogPillar", (6, -6, 6), "cube", 2.0,
                   material("ssp_pillar", (0.25, 0.35, 0.55)),
                   rot=(0, 0, 0))]
    doc = {"version": 1, "generator": "verify_screenspace.py",
           # 光近乎垂直向下 → 太阳（-direction 方向）在天顶附近，
           # 相机仰视天顶就能让光柱源稳定居中（不依赖 yaw 约定）。
           "light": {"direction": [0.1, 0.0, -1.0],
                     "color": [1.0, 0.97, 0.92], "intensity": 0.45,
                     "castsShadow": False, "ambientScale": 0.4},
           "entities": ents}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


# 近景相机：立方体占画面左中，柱子右侧远景
CAM = "0,-14,3.2,90,8"
# 太阳在天顶附近（光源 direction=[0.1,0,-1] → 太阳方向 ≈ (0.1,0,1)），
# 相机仰视：pitch = -84°（⚠ pitch 是俯角，正 = 向下）、yaw ≈ 6°。
SUN_CAM = "0,-4,1.5,6,-84"

BASE_ENV = {
    "MYVK_LIGHT_GIZMOS": "0",
    "MYVK_EDITOR_NO_HUD": "1",
    "MYVK_FRUSTUM_DBG": "0",
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
    scene = os.path.join(OUT_DIR, "screenspace.json")
    build_scene(scene)

    def run(tag, cam, on=None, env_extra=None):
        """on: None=全关；dict=要开的开关。返回 (lines, vp, img, errs)。"""
        env = dict(os.environ)
        env.update(BASE_ENV)
        env["MYVK_EDITOR_SCENE"] = scene
        env["MYVK_EDITOR_CAM"] = cam
        for k in ("MYVK_SSAO", "MYVK_FOG", "MYVK_SSGI", "MYVK_SSR",
                  "MYVK_GODRAYS"):
            env[k] = "0"
        env["MYVK_FG_DBG"] = "1"  # 组 6 靠它断言"全关不打 FRAMEGRAPH"
        if on:
            for k, v in on.items():
                env[k] = str(v)
        if env_extra:
            env.update(env_extra)
        return run_once(exe, scene, env, os.path.join(OUT_DIR, "ssp_%s.png"
                                                      % tag), cam=cam)

    def diff(name, vpA, imgA, vpB, imgB, min_n, min_mx=8):
        if imgA is None or imgB is None or vpA is None or vpB is None:
            check(name, False, "截图/视口缺失")
            return
        a, b = vp_box(vpA), vp_box(vpB)
        box = (max(a[0], b[0]), max(a[1], b[1]),
               min(a[2], b[2]), min(a[3], b[3]))
        n, mx, mean, _ = diff_stats(imgA, imgB, box)
        print("     %s：差异像素=%d 最大差=%d" % (name, n, mx))
        check(name, n > min_n and mx > min_mx, "n=%d max=%d" % (n, mx))

    # ---- 组 1：SSAO（MSAA=4 → sampler2DMS 深度形态）----
    _, vpA, imgA, eA = run("base_m4", CAM)
    _, vpB, imgB, eB = run("ssao_m4", CAM, on={"MYVK_SSAO": "1"})
    diff("SSAO 开/关差异显著（MSAA=4）", vpA, imgA, vpB, imgB, 500)
    check("SSAO MSAA=4 轮零 VUID / 零 Fatal", not (eA or eB), (eA + eB)[:2])

    # ---- 组 2：SSAO（MSAA=1 → sampler2D 单采样形态）----
    _, vpC, imgC, eC = run("base_m1", CAM, env_extra={"MYVK_MSAA": "1"})
    _, vpD, imgD, eD = run("ssao_m1", CAM, on={"MYVK_SSAO": "1"},
                           env_extra={"MYVK_MSAA": "1"})
    diff("SSAO 开/关差异显著（MSAA=1）", vpC, imgC, vpD, imgD, 500)
    check("SSAO MSAA=1 轮零 VUID / 零 Fatal", not (eC or eD), (eC + eD)[:2])

    # ---- 组 3：高度雾（密度拉大让远处明显泛白）----
    _, vpE, imgE, eE = run("fog_base", CAM)
    _, vpF, imgF, eF = run("fog_on", CAM, on={"MYVK_FOG": "1"},
                           env_extra={"MYVK_FOG_DENSITY": "0.15"})
    diff("高度雾 开/关差异显著", vpE, imgE, vpF, imgF, 5000)
    check("雾轮零 VUID / 零 Fatal", not (eE or eF), (eE + eF)[:2])

    # ---- 组 4：God rays（太阳居中 + 拉高强度）----
    # 编辑器的轨道相机重摆让"把太阳摆进画面"的机位很难稳定复现，
    # 用 MYVK_FG_SUN_UV 钩子直接指定太阳屏幕坐标（0.5,0.5 = 居中）。
    # 默认强度 0.45 的光柱叠在亮天空上 8bit 差只有 5~30/255，信噪
    # 不够 —— 用 MYVK_GODRAYS_STRENGTH 钩子拉到 1.5（该钩子与
    # MYVK_SSAO_STRENGTH 同一惯例，正常使用不设即无影响）。
    _, vpG, imgG, eG = run("gr_base", CAM)
    lg, vpH, imgH, eH = run(
        "gr_on", CAM, on={"MYVK_GODRAYS": "1"},
        env_extra={"MYVK_FG_SUN_UV": "0.5,0.5",
                   "MYVK_GODRAYS_STRENGTH": "1.5"})
    diff("God rays 开/关差异显著（太阳在画内）",
         vpG, imgG, vpH, imgH, 500)
    gr_lines = [l for l in lg if "FRAMEGRAPH passes=1" in l]
    check("FRAMEGRAPH 日志确认 godrays 单 pass 执行", bool(gr_lines),
          "FRAMEGRAPH passes=1 未出现")
    check("God rays 轮零 VUID / 零 Fatal", not (eG or eH), (eG + eH)[:2])

    # ---- 组 5：链全开（SSGI + SSR + GODRAYS = 3 pass）----
    _, vpI, imgI, eI = run("all_base", CAM)
    li, vpJ, imgJ, eJ = run("all_on", CAM, on={
        "MYVK_SSGI": "1", "MYVK_SSR": "1", "MYVK_GODRAYS": "1"})
    all_lines = [l for l in li if "FRAMEGRAPH" in l]
    check("FRAMEGRAPH 日志确认三 pass 执行",
          any("passes=3" in l for l in all_lines), all_lines[:1])
    diff("链全开 画面有差异且不炸", vpI, imgI, vpJ, imgJ, 100)
    check("链全开轮零 VUID / 零 Fatal", not (eI or eJ), (eI + eJ)[:2])

    # ---- 组 6：全关零链开销（FRAMEGRAPH 行不得出现）----
    lk, _, _, eK = run("all_off_dbg", CAM)
    fg_off = [l for l in lk if "FRAMEGRAPH" in l]
    check("全关时 FRAMEGRAPH 日志不出现（链零 pass）",
          not fg_off and not eK, (fg_off + eK)[:2])

    # 汇总行：run_regression.py 靠它判定 ok
    print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
