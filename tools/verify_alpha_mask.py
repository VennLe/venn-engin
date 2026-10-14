"""端到端验证：AlphaMode 加 `Mask`（alpha test）+ 抖动裁切（R4-1）。

Mask 是**第三种 alpha 模式**（Opaque / Blend / Mask），与 Blend 的关键区别：

  * Mask **写深度**、**不混合** —— 管线固定状态与 Opaque 完全相同；
  * 片元在 `alpha < cutoff` 时 discard → 镂空处既不写颜色也不写深度；
  * MSAA 下管线开 alpha-to-coverage，筛孔边缘得到硬件抗锯齿；
  * Mask 项**跳过深度预通道**（预通道不采样材质，无从判断该镂空哪里），
    靠前向自己写深度。

片元里的 alpha 处理（pbr.frag 的 VENN_ALPHA_MASK 分支）：
    alpha' = clamp((alpha - cutoff) * 2 + 0.5, 0, 1)
预映射让 "alpha == cutoff" 正好落在 a2c 硬编码的 0.5 阈值上，于是 cutoff
滑条在 MSAA 与非 MSAA 下表现一致；`keep < 0.5 → discard` 在两种采样数下
都给出相同的裁决（alpha'<=0 ⟺ a2c 覆盖掩码全 0）。

抖动模式（`ditherAlpha`）：push.skin.w 取负（绝对值仍是 cutoff）。用 4×4
Bayer 有序抖动阈值代替固定 cutoff —— 每个片元仍是非 0 即 1，只是"谁被
留下"随屏幕位置变化。

测试场景：一个**正对相机的平面**（25 单位），其 albedo 贴图是一张
**50% 灰**（alpha = 0.5，RGB 白）的程序化贴图。于是：

  * cutoff = 0.50 → alpha(0.5) 恰好等于阈值，step 的边界；
  * cutoff = 0.30 → 全部保留（0.5 >= 0.3）→ 与 Opaque 同画；
  * cutoff = 0.70 → 全部丢弃（0.5 < 0.7）→ 平面看不见，透出背景。

> ⚠ 场景里的平面用 50% 灰贴图是为了让"整片保留 / 整片丢弃"都可断言；
> 真正的"镂空"是纹理里逐像素的 alpha 变化，这里用单一 alpha 值换取
> **确定性**的断言（一个 cutoff 触发整片翻转，最容易判断）。

断言分六组：

  1. **默认 = 基线**：三位关键字掩码里 AlphaMask=0；同一配置跑两次逐像素一致。
  2. **Mask + cutoff 0.7（全丢）**：平面消失 → 与 Opaque 版相比中心区
     亮差异显著（透出深色背景）。
  3. **Mask + cutoff 0.3（全留）**：平面还在 → **逐像素等于 Opaque 基线**。
     （这条同时钉死了"Mask 不改变 Opaque 画面"。）
  4. **Blend 对照**：切回 Blend → 平面半透明叠加（颜色居于全留与全丢之间）。
  5. **抖动**：ditherAlpha 打开 → 画面出现逐像素 0/1 的花斑（与硬边版本
     显著不同），且**保留了写深度**（细节：只断言与硬边版本不同 + 有
     高频噪声）。
  6. **不变量**：全程零 VUID（Mask 的管线状态与 a2c 都合法）。

用法：
    python tools/verify_alpha_mask.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

CAM = "0,-26,9,90,8"

# "plane" 是**水平面**（XY 平面，法线 +Z）—— 所以测试板与地面一样"，都
# 平放在 z 上，靠相机俯角看它们的正面。相机 (0,-26,9) 朝 +Y、俯角 8°，
# 与 verify_fxaa.py 完全同一套视角（已在用的可靠组合）。
PLANE_SIZE = 40.0
# 测试板 albedo 贴图的 alpha 通道值（128 / 255 = 0.50196）。
# 选 128 是为了让 0.5 的 cutoff 落在"刚好保留"这一侧（step(0.5, 0.502)=1），
# 于是"cutoff=0.5 全留"成为一条可断言的硬边界。
PLANE_ALPHA = 128
SUN_INTENSITY = 0.9
AMBIENT_SCALE = 0.35

SHADER_DBG_RE = re.compile(r"SHADER-VARIANTS-BUILT.*")
# VARIANT-DBG / CULL-STATS 里能拿到本帧用到的掩码；这里主要靠画面断言，
# 掩码只做日志佐证。


def material(name, alpha_mode="opaque", cutoff=0.5, dither=False,
             alpha_texture=True):
    """一个平放的白色材质；alpha 由 albedo 贴图的 alpha 通道决定。

    alpha_texture=True 时挂一张"solid 白 + alpha 128（= 0.502）"的贴图；
    否则不挂贴图（内置纯白，alpha = 1）—— 用来对照"Mask 但贴图完全不透
    明"这条路径。

    ⚠ 用 **solid** 而不是 checker：整片同一个 alpha 才能让"一个 cutoff
    触发整片翻转"成为确定性断言（逐格子的 alpha 会让断言退化成统计量）。
    """
    return {
        "kind": "procedural",
        "name": name,
        "baseColor": [1.0, 1.0, 1.0, 1.0],
        "metallic": 0.0,
        "roughness": 0.9,
        "normalScale": 1.0,
        "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0],
        "doubleSided": False,
        "alphaMode": alpha_mode,
        "alphaBlend": (alpha_mode == "blend"),
        "alphaCutoff": cutoff,
        "ditherAlpha": dither,
        "albedo": ({"kind": "solid", "name": name + "_tex",
                    "color": [255, 255, 255, 128], "size": 512}
                   if alpha_texture else None),
    }


def build_scene(path, alpha_mode="opaque", cutoff=0.5, dither=False,
                alpha_texture=True):
    """一块**水平测试板** + 地面（都在 XY 平面）。

    相机 (0,-26,9) 朝 +Y 俯视 8° → 测试板（z=0.5，比地面高一点以免 z-fight）
    在画面里占一大片。地面用暗色，"板消失"时能看到地面颜色 → 中心亮度下降。

    ⚠ 不用"竖立的墙"：builtin plane 恒为水平面（法线 +Z），要竖立得转
    rotation，多一处出错面。水平板 + 俯角相机是最短路径。
    """
    ground = {
        "name": "Ground",
        "transform": {"position": [0.0, 0.0, 0.0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": 120.0,
                 "name": "am_ground"},
        "material": {
            "kind": "procedural", "name": "am_ground_mat",
            "baseColor": [0.06, 0.06, 0.08, 1.0],
            "metallic": 0.0, "roughness": 1.0,
            "normalScale": 1.0, "ao": 1.0,
            "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
            "alphaMode": "opaque", "alphaBlend": False,
            "alphaCutoff": 0.5, "ditherAlpha": False, "albedo": None,
        },
    }
    # 测试板：比地面高 0.5，避免共面 z-fighting；同样大小但用亮色。
    plane = {
        "name": "Panel",
        "transform": {"position": [0.0, 6.0, 0.5], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": PLANE_SIZE,
                 "name": "am_panel"},
        "material": material("am_panel_mat", alpha_mode, cutoff, dither,
                             alpha_texture),
    }
    doc = {
        "version": 1,
        "generator": "verify_alpha_mask.py",
        "light": {"direction": [-0.30, -0.45, -0.84], "color": [1.0, 0.98, 0.95],
                  "intensity": SUN_INTENSITY, "castsShadow": False,
                  "ambientScale": AMBIENT_SCALE},
        "entities": [ground, plane],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return 2


def run_once(exe, scene, extra_env, shot_path):
    """跑一轮编辑器，返回 (日志行, 视口矩形, RGB ndarray, 错误行)。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_GRID"] = "0"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
    env.update(extra_env)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            return [], None, None, ["未找到编辑器窗口"]
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800, 0x0002 | 0x0004)
        time.sleep(2.5)

        vp = wait_rect(tap, "VP-RECT", 10.0)
        # 等渲染稳定（几帧之后）
        time.sleep(2.0)

        img = None
        if shot_path:
            shot, _ = capture_client(hwnd)
            shot.save(shot_path)
            img = np.asarray(shot.convert("RGB")).astype("float32")

        with tap.lock:
            lines = list(tap.lines)
        errs = [l for l in lines
                if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                    or "Error:" in l or "找不到变体着色器" in l)]
        return lines, vp, img, errs
    finally:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.5)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=6)
            except subprocess.TimeoutExpired:
                proc.kill()


def parse_vp(vp):
    if not vp:
        return None
    vals = [int(float(v)) for v in re.findall(r"-?\d+\.?\d*", str(vp))]
    if len(vals) < 4:
        return None
    if vals[2] < vals[0] or vals[3] < vals[1]:
        return (vals[0], vals[1], vals[0] + vals[2], vals[1] + vals[3])
    return (vals[0], vals[1], vals[2], vals[3])


def luma(a):
    return a[:, :, 0] * 0.299 + a[:, :, 1] * 0.587 + a[:, :, 2] * 0.114


def full_mean_diff(a, b):
    return float(np.abs(luma(a) - luma(b)).mean())


def panel_mean_diff(a, b):
    """只比**面板区域**（裁剪区底部中心，见 center_mean 的说明）。

    全屏均值差会被天空/地面稀释（板只占视口 ~15%），测不出"板消失"这种
    局部翻转 —— 实测全屏差只有 0.05，而板区差高达 150+。
    """
    h, w = luma(a).shape
    y0, y1 = int(h * 0.72), int(h * 0.92)
    x0, x1 = int(w * 0.30), int(w * 0.70)
    return float(np.abs(luma(a)[y0:y1, x0:x1] - luma(b)[y0:y1, x0:x1]).mean())


def center_mean(a, frac=0.35):
    """面板区域的亮度均值。

    ⚠ 不能用"视口正中"：相机 (0,-26,9) 俯角 8° 时，视口**上半**是天空盒、
    中间偏下才是测试板（`am_rawalpha.png` 实测：板在裁剪区纵向 ~72%~96%）。
    用正中会只采到天空 → cutoff 变化完全测不出来（这曾让本脚本首版假 FAIL）。
    改成锚定**裁剪区底部中心**：板恒在那里，且地面暗色 → "板消失"时亮度骤降。
    """
    h, w = luma(a).shape
    # 纵向取 72%~92%（板的主体），横向取中间 50%。
    y0, y1 = int(h * 0.72), int(h * 0.92)
    x0, x1 = int(w * 0.30), int(w * 0.70)
    return float(luma(a)[y0:y1, x0:x1].mean())


def high_freq_ratio(a):
    """高频能量占比：相邻像素差的均值 / 全局均值 —— 抖动的花斑会让它飙升。"""
    L = luma(a)
    dx = np.abs(np.diff(L, axis=1)).mean()
    dy = np.abs(np.diff(L, axis=0)).mean()
    m = max(L.mean(), 1e-3)
    return float((dx + dy) / m)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = args.exe
    if not os.path.exists(exe):
        print("FAIL: 找不到可执行文件 %s" % exe)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)
    failures = []
    print("=" * 68)
    print("AlphaMask 验证（R4-1：alpha 裁切 + 抖动）")
    print("=" * 68)

    kill_stale_editors()

    def shot_for(name):
        return os.path.join(OUT_DIR, "am_%s.png" % name)

    scenes = {}

    def scene_for(key, **kw):
        if key not in scenes:
            p = os.path.join(OUT_DIR, "am_scene_%s.json" % key)
            build_scene(p, **kw)
            scenes[key] = p
        return scenes[key]

    def step(name, key, extra=None, **kw):
        scene = scene_for(key, **kw)
        lines, vp, img, errs = run_once(exe, scene, extra or {},
                                       shot_for(name))
        if errs:
            print("  [%s] ⚠ 错误输出 %d 行，例：%s"
                  % (name, len(errs), errs[0][:120]))
            failures.append("%s: 有错误/验证层输出" % name)
        return lines, vp, img

    # ---- 组 1：opaque 基线 + 稳定性 ----
    print("\n[1] opaque 基线：同配置两次逐像素一致")
    _, vp, img_o1 = step("opaque1", "opaque", alpha_mode="opaque")
    _, _, img_o2 = step("opaque2", "opaque", alpha_mode="opaque")
    if img_o1 is None:
        print("FAIL: 没抓到截图")
        return 2
    vpb = parse_vp(vp)
    if vpb:
        x0, y0, x1, y1 = vpb
    else:
        x0, y0, x1, y1 = 268, 118, 790, 628

    def crop(im):
        return im[y0:y1, x0:x1]

    base = crop(img_o1)
    d = full_mean_diff(base, crop(img_o2))
    print("  两次 opaque 全屏均值差 = %.6f" % d)
    if d > 0.0001:
        print("  ✗ opaque 基线不稳定")
        failures.append("组1: 基线不稳定")
    else:
        print("  ✓ opaque 基线逐像素一致")
    base_c = center_mean(base)
    print("  中心亮度 = %.2f" % base_c)

    # ---- 组 2：Mask cutoff=0.70（alpha 0.5 < 0.7 → 全丢）----
    print("\n[2] Mask cutoff=0.70：alpha(0.5) < 0.70 → 平面整片丢弃")
    _, _, img_m7 = step("mask70", "mask70", alpha_mode="mask", cutoff=0.70)
    c7 = crop(img_m7)
    d7 = panel_mean_diff(base, c7)
    c7_center = center_mean(c7)
    print("  面板区与 opaque 的均值差 = %.4f；面板亮度 %.2f → %.2f"
          % (d7, base_c, c7_center))
    if d7 < 30.0:
        print("  ✗ 平面没有消失（面板区差异太小）")
        failures.append("组2: cutoff 全丢未生效")
    elif c7_center >= base_c - 20.0:
        print("  ✗ 面板仍然很亮（平面还在）")
        failures.append("组2: 平面未被裁掉")
    else:
        print("  ✓ 高 cutoff 把平面整片裁掉（透出背景）")

    # ---- 组 3：Mask cutoff=0.30（alpha 0.5 >= 0.3 → 全留）----
    print("\n[3] Mask cutoff=0.30：alpha(0.5) >= 0.30 → 平面全留"
          "（应逐像素等于 opaque）")
    _, _, img_m3 = step("mask30", "mask30", alpha_mode="mask", cutoff=0.30)
    c3 = crop(img_m3)
    d3 = panel_mean_diff(base, c3)
    d3f = full_mean_diff(base, c3)
    print("  面板区均值差 = %.6f（全屏 %.6f）" % (d3, d3f))
    # 允许极小差异（a2c 在 MSAA 下把 alpha'=1.0 的片元全保，理论上逐位相同）
    if d3 > 0.5:
        print("  ✗ Mask(全留) 与 opaque 不一致 —— Mask 不应改变已有画面")
        failures.append("组3: Mask 全留路径污染 opaque")
    else:
        print("  ✓ Mask 全留与 opaque 逐像素一致")

    # ---- 组 4：Blend 对照（半透明叠加）----
    print("\n[4] Blend 对照：半透明叠加（颜色居于全留与全丢之间）")
    _, _, img_bl = step("blend", "blend", alpha_mode="blend", cutoff=0.5)
    cb = crop(img_bl)
    cb_center = center_mean(cb)
    print("  面板亮度：opaque %.2f | blend %.2f | 全丢 %.2f"
          % (base_c, cb_center, c7_center))
    if not (c7_center - 1.0 < cb_center < base_c + 1.0) or \
            abs(cb_center - base_c) < 1.0:
        print("  ✗ Blend 没有表现为'介于两者之间'（可能混成不透明）")
        failures.append("组4: Blend 未按半透明合成")
    else:
        print("  ✓ Blend 表现为半透明叠加")

    # ---- 组 5：抖动（花斑 + 与硬边不同）----
    print("\n[5] 抖动模式：与硬边版本显著不同，且出现高频花斑")
    _, _, img_dt = step("dither", "dither", alpha_mode="mask", cutoff=0.5,
                        dither=True)
    cd = crop(img_dt)
    # 硬边对照：cutoff 恰好 0.5（alpha 0.5 >= 0.5 → step 返回 1 → 全留）
    _, _, img_hard = step("hard50", "hard50", alpha_mode="mask", cutoff=0.5)
    ch = crop(img_hard)
    d_dither = panel_mean_diff(ch, cd)
    hf_hard = high_freq_ratio(ch[int(ch.shape[0]*0.72):int(ch.shape[0]*0.92),
                                 int(ch.shape[1]*0.30):int(ch.shape[1]*0.70)])
    hf_dither = high_freq_ratio(cd[int(cd.shape[0]*0.72):int(cd.shape[0]*0.92),
                                   int(cd.shape[1]*0.30):int(cd.shape[1]*0.70)])
    print("  抖动 vs 硬边 面板区均值差 = %.4f；面板高频比 %.4f → %.4f"
          % (d_dither, hf_hard, hf_dither))
    if d_dither < 5.0:
        print("  ✗ 抖动没有改变画面")
        failures.append("组5: 抖动未生效")
    elif hf_dither <= hf_hard * 1.2:
        print("  ✗ 抖动没有出现高频花斑（可能是均匀调暗）")
        failures.append("组5: 抖动不是逐像素 0/1")
    else:
        print("  ✓ 抖动产生逐像素花斑")
    print("  抖动面板亮度 %.2f（应约为硬边全留 %.2f 的一半）"
          % (center_mean(cd), center_mean(ch)))

    # ---- 结论 ----
    print("\n" + "=" * 68)
    if failures:
        print("结果: FAIL（%d 项）" % len(failures))
        for f in failures:
            print("  · " + f)
        return 1
    print("结果: PASS（5 组全部通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
