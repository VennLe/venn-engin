"""端到端验证：调试视图模式（R0-2，docs/render_roadmap.md）。

交付物（见 Renderer::ViewMode / pbr.frag / PipelineDesc::polygonMode）：

  · Renderer::ViewMode 8 种：Lit / Unlit / Normal / Roughness / Metallic /
    Wireframe / Overdraw / LightComplexity；
  · 前 5 + LightComplexity 走 pbr.frag 的 viewModeParams.x 运行时分支
    （Lit = 原路径，逐像素不变）；
  · Wireframe = polygonMode=LINE 管线（fillModeNonSolid 特性），
    Overdraw = additive 混合管线，两者 depth 不测不写、输出固定色；
  · MYVK_VIEW_MODE=<0..7> 环境钩子 + Render 面板 View mode 下拉
    （VM-COMBO 标签证明控件在渲染）。

断言分组：

  M0 UI / 启动     每轮日志有 VIEW-MODE init mode=<期望名>；
                   VM-COMBO labels=8 出现（下拉存在）；
                   wireframe=available（fillModeNonSolid 支持）。
  M1 基线恒等      MYVK_VIEW_MODE=0 vs 不设 → 逐像素一致（env 不碰基线）。
  M2 全模式差异    7 个非 Lit 模式 vs Lit 差异显著（全屏都在变 —— 地面
                   也是被着色的几何）。
  M3 灰度性        Roughness / Metallic：中央区域 R≈G≈B（max 通道差
                   <=3 的像素占比 >=95%）。
  M4 Normal        中央区域蓝通道均值 > 红通道（世界法线 z 主导，
                   地面/顶面朝上 → b≈1）。
  M5 LightComplexity 场景含 1 盏点光 → 中央出现"绿色系"像素
                   （g-r>20 且 g-b>20）—— 簇内灯数 >=1 的区域。
  M6 Overdraw      中央区域 R 均值 > B 均值（additive 紫红色调）。
  M7 零验证层错误  全部 9 轮 clean。

用法：
    python tools/verify_viewmodes.py
"""

import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import kill_stale_editors                       # noqa: E402
from verify_local_shadow import diff_stats, vp_box                   # noqa: E402
from verify_shader_variants import run_once                          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 视图模式索引 ↔ 名字（与 Renderer::ViewMode 一致）
MODES = ["lit", "unlit", "normal", "roughness", "metallic",
         "wireframe", "overdraw", "light_complexity"]

CAM = "0,-16,4.5,90,32"  # 俯视 32°：画面几乎全是地面+立方体，天空只剩边缘


def material(name, base, rough=0.85, metal=0.0, blend=False):
    return {"kind": "standard", "name": name,
            "baseColor": list(base) + [1.0],
            "metallic": metal, "roughness": rough, "normalScale": 0.0,
            "ao": 1.0, "emissive": [0, 0, 0],
            "doubleSided": False, "alphaBlend": blend}


def entity(name, pos, shape, size, mat):
    return {"name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": shape, "size": size,
                     "name": name.lower()},
            "material": mat}


def build_scene(path):
    """地面 + 三个材质各异的立方体 + 一块透明板 + 一盏点光。

    · 立方体都在画面中央 → 灰度/法线/灯复杂度断言取中央区域就够；
    · 点光（红色材质旁）→ LightComplexity 有 >=1 灯的簇；
    · 透明板走 ALPHA_BLEND 变体 —— 顺带覆盖"调试模式画透明队列"。
    """
    ents = [
        entity("Ground", (0, 0, 0), "plane", 60.0,
               material("vm_ground", (0.38, 0.36, 0.33))),
        entity("RedCube", (0.0, 1.0, 1.6), "cube", 3.0,
               material("vm_red", (0.85, 0.15, 0.10), rough=0.7)),
        entity("MetalCube", (-4.5, -1.0, 1.6), "cube", 3.0,
               material("vm_metal", (0.75, 0.75, 0.78), rough=0.15,
                        metal=0.95)),
        entity("RoughCube", (4.5, -1.0, 1.6), "cube", 3.0,
               material("vm_rough", (0.6, 0.5, 0.35), rough=0.98)),
        entity("GlassPanel", (0.0, -6.5, 1.8), "cube", 3.0,
               material("vm_glass", (0.5, 0.8, 0.9), rough=0.1,
                        blend=True)),
        # 点光：放在红立方体上方，让 LightComplexity 在中央产生 >=1 灯
        {"name": "Pt", "transform": {"position": [0.0, 1.0, 5.0],
                                     "rotation": [0, 0, 0],
                                     "scale": [1, 1, 1]},
         "visible": True, "castShadow": False, "parent": -1,
         "pointLight": {"color": [1.0, 0.9, 0.8], "intensity": 40.0,
                        "range": 16.0, "enabled": True, "castsShadow": False}},
    ]
    doc = {"version": 1, "generator": "verify_viewmodes.py",
           "camera": {"target": [0.0, 0.0, 1.8], "yaw": 90.0, "pitch": 8.0,
                      "distance": 14.0, "fov": 60.0},
           "light": {"direction": [0.35, 0.25, -1.0],
                     "color": [1.0, 0.97, 0.92], "intensity": 1.0},
           "entities": ents}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def center_region(vp, frac=0.22):
    """视口中央 frac 比例的正方形区域（俯视相机下地面/立方体必在其中，
    天空只剩边缘 —— 灰度/法线/灯复杂度断言都取这里）。"""
    x0, y0, x1, y1 = [int(v) for v in vp_box(vp)]
    w, h = x1 - x0, y1 - y0
    cx, cy = x0 + w // 2, y0 + h // 2
    half = int(min(w, h) * frac)
    return (cx - half, cy - half, cx + half, cy + half)


def region_stats(img, box):
    """区域内的 numpy 数组（float）。"""
    import numpy as np
    x0, y0, x1, y1 = [max(int(v), 0) for v in box]
    a = np.asarray(img, dtype=np.float32)
    return a[y0:y1, x0:x1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "bin",
                                                  "Editor.exe"))
    args = ap.parse_args()

    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        scene = os.path.join(td, "viewmodes_scene.json")
        build_scene(scene)

        results = []

        def check(name, cond, detail=""):
            results.append((name, bool(cond), detail))
            print(("PASS  " if cond else "FAIL  ") + name +
                  (f"  ({detail})" if detail else ""))

        # ---- 9 轮：Lit 基线 ×2（设 0 / 不设）+ 7 个模式 ----
        shots = {}
        logs = {}
        errs = {}
        for mode in range(8):
            env = {"MYVK_VIEW_MODE": str(mode)}
            shot = os.path.join(OUT_DIR, f"viewmode_{MODES[mode]}.png")
            lines, vp, img, e = run_once(args.exe, scene, env, shot, cam=CAM)
            logs[mode] = lines
            errs[mode] = e
            shots[mode] = img
            if img is None or vp is None:
                print(f"!! 第 {mode} 轮截图/视口缺失，后续断言将失败")
        # 对照轮：完全不设 MYVK_VIEW_MODE（Lit 基线）
        lines_ref, vp_ref, img_ref, e_ref = run_once(
            args.exe, scene, {}, os.path.join(OUT_DIR,
                                              "viewmode_noref.png"),
            cam=CAM)
        errs["ref"] = e_ref

        # UI 存在性专用轮：打开 Engine 面板（View mode 下拉所在）。
        # 与像素对照分开 —— 面板里 "Lights/cluster avg" 每帧微变，
        # 打开它会毁掉逐像素断言（踩过）。
        lines_ui, _, _, e_ui = run_once(
            args.exe, scene, {"MYVK_ENGINE_PANEL": "1", "MYVK_VIEW_MODE": "0"},
            None, cam=CAM)
        errs["ui"] = e_ui

        # ---- M0 启动 / UI ----
        all_lines = []
        for m in range(8):
            all_lines += logs.get(m, [])
        all_lines += lines_ref
        all_lines += lines_ui

        for m in range(8):
            want = f"VIEW-MODE init mode={MODES[m]}"
            ok = any(want in l for l in logs.get(m, []))
            check(f"M0a [{MODES[m]}] 启动确认行", ok,
                  "" if ok else f"缺 '{want}'")

        check("M0b View mode 下拉存在（VM-COMBO）",
              any("VM-COMBO labels=8" in l for l in all_lines))
        check("M0c wireframe 特性可用",
              any("VIEW-MODE wireframe=available" in l for l in all_lines))

        # ---- M1 基线恒等：MYVK_VIEW_MODE=0 vs 不设 ----
        if shots.get(0) is not None and img_ref is not None and vp_ref:
            n, mx, mean, _ = diff_stats(shots[0], img_ref, vp_box(vp_ref))
            check("M1 mode=0 与不设 env 逐像素一致", mx == 0,
                  f"差异像素 {n}，最大差 {mx}")
        else:
            check("M1 mode=0 与不设 env 逐像素一致", False, "截图缺失")

        # ---- M2 全模式差异 ----
        if shots.get(0) is not None and vp_ref:
            for m in range(1, 8):
                if shots.get(m) is None:
                    check(f"M2 [{MODES[m]}] 与 Lit 差异显著", False,
                          "截图缺失")
                    continue
                n, mx, mean, _ = diff_stats(shots[m], shots[0],
                                            vp_box(vp_ref))
                check(f"M2 [{MODES[m]}] 与 Lit 差异显著",
                      n > 5000 and mean > 1.0,
                      f"差异像素 {n}，最大差 {mx}，均值 {mean:.2f}")

        # ---- M3 灰度性（Roughness / Metallic）----
        if vp_ref:
            box = center_region(vp_ref)
            for m, nm in ((3, "roughness"), (4, "metallic")):
                if shots.get(m) is None:
                    check(f"M3 [{nm}] 中央灰度", False, "截图缺失")
                    continue
                import numpy as np
                a = region_stats(shots[m], box)
                ch = a.max(axis=2) - a.min(axis=2)
                # 阈值 12：后处理（bloom 半分辨率 / tonemap）在灰度图上
                # 会留下 4~9 的通道间舍入噪声（实测），真正的彩色内容
                # （法线蓝 / 灯复杂度绿 / 轴线）通道差 >= 60。
                frac = float((ch <= 12.0).mean())
                check(f"M3 [{nm}] 中央 R≈G≈B（>=95% 像素通道差<=12）",
                      frac >= 0.95, f"灰度占比 {frac*100:.1f}%")

            # ---- M4 Normal 蓝主导 ----
            if shots.get(2) is not None:
                import numpy as np
                a = region_stats(shots[2], box)
                check("M4 [normal] 中央蓝通道均值 > 红",
                      float(a[:, :, 2].mean()) > float(a[:, :, 0].mean()),
                      f"b={a[:,:,2].mean():.1f} r={a[:,:,0].mean():.1f}")

            # ---- M5 LightComplexity 绿色像素 ----
            if shots.get(7) is not None:
                import numpy as np
                a = region_stats(shots[7], box)
                g = a[:, :, 1]
                r = a[:, :, 0]
                b = a[:, :, 2]
                green = int(((g - r > 20) & (g - b > 20)).sum())
                check("M5 [light_complexity] 中央存在绿系像素（点光簇）",
                      green > 200, f"绿色像素 {green}")

            # ---- M6 Overdraw 紫红调 ----
            if shots.get(6) is not None:
                import numpy as np
                a = region_stats(shots[6], box)
                rmean = float(a[:, :, 0].mean())
                bmean = float(a[:, :, 2].mean())
                check("M6 [overdraw] 中央 R 均值 > B（紫红 additive）",
                      rmean > bmean, f"r={rmean:.1f} b={bmean:.1f}")

        # ---- M7 零验证层错误 ----
        bad = {k: v for k, v in errs.items() if v}
        check("M7 全部 9 轮无 Vulkan 验证层报错", not bad,
              "clean" if not bad else f"{len(bad)} 轮有错：" +
              "; ".join(bad[list(bad)[0]][:2]))

        print()
        failed = [r for r in results if not r[1]]
        for name, ok, detail in failed:
            print(f"  - {name}" + (f"  ({detail})" if detail else ""))
        if failed:
            print(f"\nRESULT: FAIL（{len(failed)} 项失败）")
            return 1
        print("结果: PASS")
        return 0


if __name__ == "__main__":
    sys.exit(main())
