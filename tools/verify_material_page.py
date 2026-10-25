"""端到端验证：材质页 32 B → 96 B 的四个 A 类语义槽（ramp / matcap / detail）。

背景：`anime_gap_audit.md` §8.2 列过三条 GPU 通道的战况 —— push 常量恰好
128 B、纹理 set 恰好 8 个、非 bindless 路径在结构上封死。于是二次元需要的
"同像素叠加"那几层（明暗带 / 球面高光 / 面部 SDF / 贴花）**只能**进 bindless
的材质页表。这一刀把页表从 32 B 扩到 96 B，并打通其中三个真正被着色器用到
的槽（SDF 只通到"乘法调制"，完整算法要 UV1 与头部空间，见 §11.9）。

本脚本验的是"通道真的通到了像素上"，分两类判据：

  A. **恒等类（最硬的那条）**：缺省槽绑白图 + 恒等用法，所以
       没挂贴图  ==  逐像素等于基线（max diff == 0，不是"近似相等"）
     这一组刻意不测"把功能关掉"，而是测"**开着**一个不改变结果的配置"：
       · ramp 白图          → band × 1.0 = band
       · detail 白图 + 强度 0.5 → albedo *= mix(1, 1, 0.5) = 1
       · matcap 黑图 + 强度 0.9 → color += 0 × 0.9 = 0
     三条都**真的进了分支**（不是被参数跳过），所以它们同时验证了
     "采样路径正确"和"数学上是恒等"。这比"把功能关掉再看"强，因为
     关掉时那段代码根本没执行。

  B. **行为类**：换一张非中性贴图，画面必须朝可预测的方向动
       · ramp 中灰 → **受光区**被乘小 → 受光区平均亮度下降
       · ramp 全黑 → band 恒 0 → 整球落在暗部色上 → 方差塌掉
       · matcap 全白 + 强度 0.5 → 加项恒正 → 偏亮
       · detail 棋盘 → albedo 被调制 → 与基线不同

     ⚠ 这一组**刻意不用"整幅平均亮度"**当判据。踩过的坑：中灰 ramp 实测
       让全画面平均亮度**上升** 13% —— 因为 ramp 一边压暗受光档、一边把
       最黑的交界环从 0 抬起来，净效果是"对比度塌缩"而不是"整体变暗"。
       所以判据钉在受光区这个固定像素集合上，因果才是单向的。

⚠ 必须显式开 `MYVK_BINDLESS=1`：页表只在 bindless 路径里存在。这不是
  偷懒，是 §8.2 的结构事实 —— 非 bindless 的那份 `.spv` 连 set 1 都没有。
  所以脚本同时断言日志里出现 `Bindless: ready`，否则后面所有判据都会
  以一种很难懂的方式失败（"贴图挂了但画面不动"）。

⚠ 只换材质、不换机位/几何：判定用的是"同一块像素区域跨运行比较"，
  机位或网格一变，差异就不再只由材质决定。

用法：
    python tools/verify_material_page.py [--exe path]
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_shader_variants import (                                    # noqa: E402
    OUT_DIR, ROOT, entity, run_once, write_scene)
# 复用 toon 验收里的机位与采样区：那套数字是**量出来的**（球心与视口中心
# 重合、球直径约 380 px），换一套就要重新标定，没有理由重来一次。
from verify_toon_shading import (                                       # noqa: E402
    BALL_POS, BALL_SIZE, CAM, LIGHT_DIR, SUN, center_patch, luma,
    material_model, to_np_rgb)

DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
SCENE_DIR = os.path.join(OUT_DIR, "material_page")

# 变体表：(tag, 说明, 材质上额外的键)
# 名字必须各不相同 —— 程序化贴图按 **name** 在 AssetManager 里缓存，
# 同名不同色会让后一个变体悄悄拿到前一个的图（症状是"判据全绿但画面没变"）。
VARIANTS = [
    ("ref", "参考：只开着色模型，一个语义槽都不挂", {}),
    ("ramp_white", "ramp 全白 → band × 1 → 必须逐像素等于参考", {
        "rampMap": {"kind": "solid", "name": "mp_ramp_w",
                    "color": [255, 255, 255, 255]},
        "rampSteps": 0.0,
    }),
    ("ramp_gray", "ramp 中灰 → 受光档被压低 → 整体变暗", {
        "rampMap": {"kind": "solid", "name": "mp_ramp_g",
                    "color": [128, 128, 128, 255]},
        "rampSteps": 0.0,
    }),
    ("ramp_black", "ramp 全黑 → band 恒 0 → 只剩暗部色，画面被压平", {
        "rampMap": {"kind": "solid", "name": "mp_ramp_b",
                    "color": [0, 0, 0, 255]},
        "rampSteps": 0.0,
    }),
    ("detail_white", "detail 白图 + 强度 0.5 → 恒等（走的是采样路径）", {
        "detailMap": {"kind": "solid", "name": "mp_det_w",
                      "color": [255, 255, 255, 255]},
        "detailStrength": 0.5,
        "detailTiling": 1.0,
    }),
    ("detail_checker", "detail 棋盘 → albedo 被调制 → 与基线不同", {
        "detailMap": {"kind": "checker", "name": "mp_det_c",
                      "colorA": [40, 40, 40, 255], "colorB": [255, 255, 255, 255],
                      "cell": 32, "size": 256, "srgb": False},
        "detailStrength": 1.0,
        "detailTiling": 4.0,
    }),
    ("matcap_black", "matcap 黑图 + 强度 0.9 → 加项为 0 → 恒等（走的是采样路径）", {
        "matcapMap": {"kind": "solid", "name": "mp_mc_b",
                      "color": [0, 0, 0, 255]},
        "matcapStrength": 0.9,
    }),
    ("matcap_white", "matcap 全白 + 强度 0.5 → 加项恒正 → 偏亮", {
        "matcapMap": {"kind": "solid", "name": "mp_mc_w",
                      "color": [255, 255, 255, 255]},
        "matcapStrength": 0.5,
    }),
]


def build_scene(path, tag, extra):
    """一颗球、无机无地面：像素统计不被网格/地面干扰。"""
    mat = material_model("mp_ball_mat_" + tag, "toon")
    mat.update(extra)
    ents = [entity("Ball", BALL_POS,
                   {"shape": "sphere", "size": BALL_SIZE, "name": "mp_ball"},
                   mat)]
    return write_scene(path, ents, "verify_material_page: " + tag, sun=SUN,
                       light_dir=LIGHT_DIR)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        print("FAIL: exe not found: %s" % args.exe)
        return 1
    os.makedirs(SCENE_DIR, exist_ok=True)

    env = {"MYVK_BINDLESS": "1"}   # 页表只在 bindless 路径存在（见文件头）

    shots = {}
    log_all = []
    errors = []
    vp_ref = None

    for tag, desc, extra in VARIANTS:
        scene = os.path.join(SCENE_DIR, "scene_%s.json" % tag)
        shot = os.path.join(SCENE_DIR, "shot_%s.png" % tag)
        build_scene(scene, tag, extra)
        print("[run] %-15s %s" % (tag, desc))
        lines, vp, img, errs = run_once(args.exe, scene, env, shot, cam=CAM)
        log_all.extend(lines)
        if errs:
            errors.extend(["%s: %s" % (tag, e) for e in errs[:4]])
        if img is None:
            print("       (no screenshot)")
            continue
        if tag == "ref":
            vp_ref = vp
        shots[tag] = to_np_rgb(img)

    checks = []

    def check(name, ok, detail):
        checks.append((name, bool(ok), detail))

    # ---------------- 0. 基本前提 ----------------
    check("shots_available", len(shots) == len(VARIANTS),
          "captured=%d/%d" % (len(shots), len(VARIANTS)))

    sizes = {t: a.shape for t, a in shots.items()}
    check("shots_same_size", len(set(sizes.values())) <= 1,
          "sizes=%s" % (sorted(set(str(s) for s in sizes.values())),))

    check("no_validation_errors", not errors, "; ".join(errors[:3]))

    # 没有这一条，后面所有判据失败都会显得像"贴图没生效"
    bindless_ready = any("Bindless: ready" in l for l in log_all)
    check("bindless_ready", bindless_ready,
          "日志里没有 'Bindless: ready' —— 页表根本没建起来")

    # 材质确实路由到了 Toon 变体（bit6），否则 ramp 那段代码不在编译单元里
    toon_routed = any("shading-toon" in l for l in log_all)
    check("toon_variant_routed", toon_routed,
          "没看到 [shading-toon] 的变体日志")

    if "ref" not in shots:
        print("\nFAIL: 没有参考图，后续判据无法计算")
        return 1

    ref_img = shots["ref"]
    ref_patch = center_patch(ref_img, vp_ref)
    ref_lum = float(luma(ref_patch).mean())

    def patch(tag):
        return center_patch(shots[tag], vp_ref)

    def mean_lum(tag):
        return float(luma(patch(tag)).mean())

    def std_lum(tag):
        return float(luma(patch(tag)).std())

    # 受光区掩码：按**参考图**的亮度取上四分位。
    # ⚠ 必须用参考图的掩码固定住像素集合，不能对每个变体各自算 percentile
    #   —— 否则"变暗"会把掩码自己也改掉，测的是另一个集合。
    ref_patch_lum = luma(ref_patch)
    lit_mask = ref_patch_lum > np.percentile(ref_patch_lum, 75)

    def lit_mean(tag):
        return float(luma(patch(tag))[lit_mask].mean())

    ref_lit_mean = float(ref_patch_lum[lit_mask].mean())

    def max_diff(tag):
        a, b = patch(tag), ref_patch
        if a.shape != b.shape:
            return 999.0
        return float(np.abs(a - b).max())

    # ---------------- A. 恒等类 ----------------
    # 判据是**恰好 0**，不是"小于阈值"：ramp 白图是 band × 1.0、
    # detail 是 mix(1, 1, 0.5)、matcap 是 +0 × 0.9 —— 三者在浮点里
    # 都是精确恒等，任何非零值都说明采样出来的不是纯白/纯黑，
    # 或者混进了一个本该被消掉的项。
    for tag in ("ramp_white", "detail_white", "matcap_black"):
        if tag in shots:
            d = max_diff(tag)
            check("%s_is_identity" % tag, d == 0.0,
                  "max diff=%.3f（必须恰好 0）" % d)

    # ---------------- B. 行为类 ----------------
    # ⚠ 这里**不**断言"变亮/变暗"的方向，因为 toonDirect 里 band 同时是
    #   "走亮档"的开关：band→0 意味着全部落在暗部色上，而暗部色
    #   (albedo×0.45,0.49,0.62) 未必比"亮档 × 阴影"的平均值更暗 ——
    #   实测就是反的（见文件头"乘法不是替换"）。方向断言会写成
    #   依赖打光方式的脆弱判据；换成下面两类真正因果相关的量：
    #     · 中灰 ramp → 受光档被乘小 → 平均亮度必然降
    #     · 全黑 ramp → band 恒 0 → 整个球一个颜色 → 方差塌掉
    # ⚠ 这里**不**用"整幅平均亮度"的涨落当判据。踩过的坑：中灰 ramp
    #   实测让平均亮度**上升** 13%，因为 ramp 同时做了两件事 ——
    #   受光档被乘小（压暗亮部），而 `mix(dark, lit·shadow≈0, band)` 里
    #   band 变小意味着不再等于 0（把最黑的交界环抬成 0.61×暗部色）。
    #   一升一降，净效果是**对比度塌缩**而不是整体变暗。
    #   所以判据必须钉在"受光区"这个像素集合上：那里 band≈1，乘 ramp
    #   之后必然变暗，因果是单向的。
    if "ramp_gray" in shots:
        m = lit_mean("ramp_gray")
        check("ramp_gray_dims_lit_region", m < ref_lit_mean * 0.95,
              "受光区 mean=%.2f ref=%.2f (%.2fx)"
              % (m, ref_lit_mean, m / max(ref_lit_mean, 1e-6)))
    if "ramp_black" in shots:
        s, sref = std_lum("ramp_black"), std_lum("ref")
        check("ramp_black_flattens", s < sref * 0.5,
              "std=%.2f ref_std=%.2f（band 恒 0 → 单一暗部色）" % (s, sref))
    if "matcap_white" in shots:
        m = mean_lum("matcap_white")
        check("matcap_white_brighter", m > ref_lum + 2.0,
              "mean=%.2f ref=%.2f (+%.2f)" % (m, ref_lum, m - ref_lum))
    if "detail_checker" in shots:
        d = float(np.abs(patch("detail_checker") - ref_patch).mean())
        check("detail_checker_changes", d > 1.0,
              "mean abs diff=%.2f（棋盘调制 albedo，必须 > 1.0）" % d)

    # ---------------- 结果 ----------------
    print("\n=== verify_material_page ===")
    failed = 0
    for name, ok, detail in checks:
        print("%s %-28s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failed += 1
    print("=== %d/%d PASS ===" % (len(checks) - failed, len(checks)))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
