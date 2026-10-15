"""端到端验证：GPU 时戳剖析（R0-1，docs/render_roadmap.md）。

交付物（见 render/GpuTimer.h / Renderer.cpp / EditorApp.cpp）：

  · render::GpuTimer —— VK_QUERY_TYPE_TIMESTAMP 查询池（每帧 slot =
    18 个 query：9 段 × begin/end），fence 之后读回换算毫秒；
  · Renderer::recordFrame 各 pass 打点（shadows / local_sh / depth /
    ssao / cluster / forward / framegraph / offscreen / post）；
  · MYVK_GPU_TIMING=1 时每 30 帧一条 GPU-TIMING 日志（key=value 格式）；
  · 编辑器统计面板的 GPU 耗时瀑布。

断言分组：

  T1 剖析产出      GPU-TIMER ready 日志出现（池创建成功）；
                   ≥2 条 GPU-TIMING；每条 frame 有限且 (0, 200)ms；
                   常开段 shadows/depth/cluster/forward/post ≥ 0 且有限，
                   其中 forward 与 post 至少各有一条 > 0（前向与后处理
                   永远在跑，量出来是 0 只能是读数坏了）。
  T2 条件段归零    默认场景（SSAO 关 / 链关 / 无离屏窗口）→
                   ssao、framegraph、offscreen 三段 ≈ 0（< 0.05ms）——
                   关闭的 pass 计为 0 是"瀑布可读"的关键性质。
  T3 像素零影响    时戳开（默认）vs MYVK_GPU_TIMER=0 → 视口逐像素一致
                   （时戳只写 query pool，不触碰渲染资源）。
  T4 零验证层错误  两轮日志均无 VUID / validation / ERROR。

全程零 Vulkan 验证层报错。用法：
    python tools/verify_gpu_timing.py
"""

import argparse
import json
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import kill_stale_editors                       # noqa: E402
from verify_local_shadow import diff_stats, vp_box                   # noqa: E402
from verify_shader_variants import run_once                          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

SECTIONS = ["shadows", "local_sh", "depth", "decals", "ssao", "cluster",
            "forward", "particles", "framegraph", "offscreen", "post"]
# 常开段（默认场景里这些 pass 一定在录命令）
ALWAYS_ON = ["shadows", "depth", "cluster", "forward", "post"]
# 默认场景里关闭/不存在的段（run_once 设了 MYVK_EDITOR_NO_HUD=1，
# 预览窗口不开 → offscreen 也是空的；默认场景没有粒子发射器 →
# particles 只打相邻两个时戳，读数 ≈0）
INACTIVE = ["ssao", "framegraph", "offscreen", "particles"]

TIMING_RE = re.compile(r"GPU-TIMING\s+(.*)")


def parse_timing(line):
    """'frame=6.84 shadows=1.20 ...' → dict（float）。解析失败返回 None。"""
    m = TIMING_RE.search(line)
    if not m:
        return None
    out = {}
    for kv in m.group(1).split():
        if "=" not in kv:
            return None
        k, v = kv.split("=", 1)
        try:
            out[k] = float(v)
        except ValueError:
            return None
    return out


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


def build_scene(path):
    """静态场景：地面 + 三个立方体。所有 pass 都有活干，画面确定性
    （静态相机 → 两轮截图可比）。"""
    ents = [entity("Ground", (0, 0, 0), "plane", 60.0,
                   material("gt_ground", (0.38, 0.36, 0.33))),
            entity("LeftCube", (-6.0, -2.0, 2.0), "cube", 4.0,
                   material("gt_left", (0.90, 0.36, 0.20))),
            entity("MidCube", (0.0, 2.0, 2.5), "cube", 4.0,
                   material("gt_mid", (0.85, 0.80, 0.30))),
            entity("FarCube", (-10.0, -24.0, 3.0), "cube", 5.0,
                   material("gt_far", (0.22, 0.48, 0.88)))]
    doc = {"version": 1, "generator": "verify_gpu_timing.py",
           "camera": {"target": [0.0, 0.0, 2.0], "yaw": 90.0, "pitch": 8.0,
                      "distance": 14.0, "fov": 60.0},
           "light": {"direction": [0.35, 0.25, -1.0],
                     "color": [1.0, 0.97, 0.92], "intensity": 1.1},
           "entities": ents}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "bin",
                                                  "Editor.exe"))
    args = ap.parse_args()

    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        scene = os.path.join(td, "gpu_timing_scene.json")
        build_scene(scene)

        results = []

        def check(name, cond, detail=""):
            results.append((name, bool(cond), detail))
            print(("PASS  " if cond else "FAIL  ") + name +
                  (f"  ({detail})" if detail else ""))

        # ---- 第 1 轮：时戳开（默认）+ 剖析日志 ----
        lines_a, vp_a, img_a, errs_a = run_once(
            args.exe, scene, {"MYVK_GPU_TIMING": "1"},
            os.path.join(OUT_DIR, "gpu_timing_on.png"))
        # ---- 第 2 轮：显式关掉时戳（像素对照的另一半）----
        lines_b, vp_b, img_b, errs_b = run_once(
            args.exe, scene, {"MYVK_GPU_TIMER": "0"},
            os.path.join(OUT_DIR, "gpu_timing_off.png"))

        # ============ T1 剖析产出 ============
        ready = [l for l in lines_a if "GPU-TIMER ready" in l]
        check("T1a 查询池创建成功（GPU-TIMER ready）", len(ready) >= 1,
              ready[0].strip() if ready else "无 ready 日志")

        timings = [t for t in (parse_timing(l) for l in lines_a) if t]
        check("T1b GPU-TIMING 日志 ≥2 条", len(timings) >= 2,
              f"实际 {len(timings)} 条")

        if timings:
            frames = [t.get("frame", -1) for t in timings]
            ok_frame = all(0.0 < f < 200.0 for f in frames)
            check("T1c 帧耗时有限且在 (0,200)ms", ok_frame,
                  "frame=" + ",".join(f"{f:.2f}" for f in frames[:6]))

            missing = [k for t in timings for k in SECTIONS if k not in t]
            check("T1d 每条日志 11 段齐全", not missing,
                  f"缺失 {sorted(set(missing))}" if missing else "11/11")

            bad = []
            for t in timings:
                for k in ALWAYS_ON:
                    if not (0.0 <= t.get(k, -1) < 200.0):
                        bad.append(k)
            check("T1e 常开段数值有限且 ≥0", not bad,
                  f"异常 {sorted(set(bad))}" if bad else
                  "/".join(ALWAYS_ON))

            fwd_pos = sum(1 for t in timings if t.get("forward", 0) > 0)
            post_pos = sum(1 for t in timings if t.get("post", 0) > 0)
            check("T1f forward 段 > 0（前向确实被计时）", fwd_pos >= 1,
                  f"{fwd_pos}/{len(timings)} 条 > 0")
            check("T1g post 段 > 0（后处理确实被计时）", post_pos >= 1,
                  f"{post_pos}/{len(timings)} 条 > 0")
        else:
            for nm in "cdefg":
                check(f"T1{nm} （依赖 T1b）", False, "没有可解析的日志")

        # ============ T2 条件段归零 ============
        if timings:
            worst = max(max(t.get(k, 99) for t in timings) for k in INACTIVE)
            check("T2 关闭的段 ≈ 0（ssao/framegraph/offscreen/particles < "
                  "0.05ms）",
                  worst < 0.05,
                  f"{len(INACTIVE)} 段最大值 {worst:.4f}ms")
        else:
            check("T2 关闭的段 ≈ 0", False, "没有可解析的日志")

        # ============ T3 像素零影响 ============
        if img_a is not None and img_b is not None and vp_a:
            n, mx, mean, _ = diff_stats(img_a, img_b, vp_box(vp_a))
            check("T3 时戳开/关 逐像素一致", mx == 0,
                  f"差异像素 {n}，最大差 {mx}，均值 {mean:.4f}")
        else:
            check("T3 时戳开/关 逐像素一致", False,
                  f"截图缺失 a={img_a is not None} b={img_b is not None}")

        # ============ T4 零验证层错误 ============
        check("T4a 时戳开一轮无 Vulkan 验证层报错", not errs_a,
              "clean" if not errs_a else "; ".join(errs_a[:3]))
        check("T4b 时戳关一轮无 Vulkan 验证层报错", not errs_b,
              "clean" if not errs_b else "; ".join(errs_b[:3]))

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
