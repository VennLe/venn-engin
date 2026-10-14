"""端到端验证：预积分 SSS（皮肤次表面散射，R4-2）。

R4-2 给 `Material` 加了 `ShadingModel{Default, Skin}`，落到着色器关键字
**bit4 `VENN_SHADING_SKIN`**。置位的编译单元里，方向光/局部光源的**漫反射**
项不再乘 `NdotL`，而是乘一张 256×256 预积分 LUT 的采样结果：

    vec3 d = skinDiffuse(N, L)     // = texture(sssLut, 纹素中心(u=NdotL*0.5+0.5, v=curv))
    直接光 = (kD * albedo/π * d + specular * NdotL) * radiance * shadow

LUT 的第二维 `curv`（曲率）**不是**美术参数，而是从几何法线的屏幕导数现算：

    curv = clamp(length(fwidth(geomN)) * ubo.sssParams.x, 0, 1)

`ubo.sssParams.x` 是唯一的尺度旋钮（默认 `kSssDefaultCurvatureScale = 8`，
环境变量 `MYVK_SSS=f` 可覆盖，`=0` 即"曲率恒 0 → 退化成 Lambert"的对照组）。

LUT 的物理模型与自检在 `tools/make_sss_lut.py` 里（那里单测**数值**）。
本脚本测的是**接线**：LUT 有没有被采样到、生效方向对不对、Default 路径
有没有被污染、以及"平坦表面不该泛红"这条边界不变量。

断言分四组（A 组是纯数值，不需要跑引擎）：

  A1. LUT 曲率=0 行逐点等于 `max(N·L, 0)`（平面退化成 Lambert，误差 < 0.01）。
  A2. LUT 曲率=1、阴影侧（N·L≈0）**R > G > B** 且 R−B > 0.05（泛红透光）。
  A3. LUT 曲率=1、正对光（N·L=1）三通道都 ≥ 0.9（亮的正面不被染色/压暗）。
  A4. 固定 N·L 时，R 通道随曲率**单调不减**且在 0→1 之间有可见增量
      （曲率轴真的在起作用，不是只读过一行）。

  B1. **Default 材质对 `MYVK_SSS` 完全不敏感**：同一个 default 球，
      `MYVK_SSS=0` 与 `=16` 两次渲染在球区**逐像素一致** → 证明 Default
      变体压根没引用 binding19 / sssParams 这条代码路径。
  B2. **Skin 材质对 `MYVK_SSS` 敏感**：同一个 skin 球，`=0` vs `=16` 在球区
      出现显著差异 → 证明 LUT 真的被采样进光照了。
  B3. **生效方向是"泛红"而不是"整体提亮/变蓝"**：B2 的差异里，**红通道的
      增量 > 蓝通道增量的 5 倍**（LUT 里 σ_R > σ_B，红光渗得最远）。
  B4. **零回归不变量**：`MYVK_SSS=0`（曲率恒 0）时，Skin 球与 Default 球
      在球区差异应只有 **LUT 量化噪声**（实测 0.20/255）—— 关着就是基线，
      符合项目"默认关 = 基线逐位不变"。
      ⚠ 这条同时是**采样边界不变量**的回归测试，见下。
  B5. **曲率轴单调**：`MYVK_SSS=4` 的差异必须**严格小于** `=16` 的 1/3
      （刻度真的在改变曲率，且 16 还没把整张球压到饱和）。

  ⚠ 关于 B4 为什么能当"采样边界"的回归测试：
  纹理采样器是 **REPEAT** 地址模式，而 LUT 的两条轴端点恰好会取到 u=1
  （正对光）与 v=0（曲率 0）。若把 `(u,v)` 直接当纹理坐标用，边界上的双线性
  会跟"绕到另一侧"的纹素各取一半 —— 曲率 0 的行会掺进曲率 1 的红边
  （曾实测 v=0 读出 0.179，而表里其实是 0.098），皮肤上所有平坦区域无端泛红。
  `skinDiffuse` 用 `(x*(N-1)+0.5)/N` 把坐标钉到**纹素中心**后不再跨界。
  scale=0 时**整个球**的曲率恒为 0 → 修复前 B4 会读到 5.6，修复后 0.20，
  差 27 倍，因此 B4 的阈值本身就是这条不变量的守卫。

  ⚠ 球区（region）不是硬编码的屏幕比例，而是 **"有球画面 − 无球画面"**
  的亮度差掩膜现算出来的，再往里腐蚀 3 像素躲开轮廓抗锯齿：

    mask = erode( luma(d0) - luma(ground_only) > 5 , 3 )

  这样球摆在画面哪里都不用改断言；而且 mask 来自 Default 基线，
  B1..B5 五组用的是**同一个像素集合**，可比。

场地为什么用**球**而不是板：`curv` 来自 `fwidth(几何法线)`，**平面法线恒定
→ fwidth = 0 → 曲率恒 0 → LUT 退化成 Lambert**，板上永远测不出 SSS。
球（以及真实的鼻梁/耳缘/指尖）法线随屏幕位置变化，才有非零曲率。

关于刻度：默认刻度 8 是按**细几何**（鼻梁那种十几像素宽的特征）调的；一个
占画面 ~200px 的球其曲率只有 ~0.1 量级，默认刻度下差异偏小、断言会飘。
实测球区平均亮度差随刻度为 1→0.03 / 2→0.13 / 4→0.51 / 8→1.92 / 16→6.41，
`≥24` 后曲率被 clamp 到 1 而**饱和**（24/32/48/64 逐位相同）。
所以测试取 **16**：既远高于噪声地板（0.20），又还没进饱和平台，能真实
经过 LUT 的曲率轴。刻度只是把机制放大到可稳定测量的量级，不改变
"关着等于基线"这条不变量（B4 仍用 0 对照）。

用法：
    python tools/verify_sss.py
    python tools/verify_sss.py --with-default-scale   # 额外打印引擎默认刻度
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402
from make_sss_lut import build_lut, SIZE                             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 与 verify_alpha_mask.py / verify_fxaa.py 同一套已验证的视角。
CAM = "0,-26,9,90,8"

BALL_POS = (0.0, 7.0, 2.6)
BALL_RADIUS = 2.0
SKIN_BASE = [0.85, 0.66, 0.60]

# 强**侧**光：-direction 是"射向光源"的向量。这里 X 分量占绝对优势 →
# 球的一半亮一半暗，明暗交界（= 最该出现"渗光泛红"的地方）正对相机。
SUN_DIR = [-0.92, -0.20, -0.34]
SUN_INTENSITY = 1.6
AMBIENT_SCALE = 0.22

SSS_OFF = "0"
SSS_ON = "16"        # 放大刻度（见文件头说明）
SSS_MID = "4"        # 曲率轴单调性用
SSS_DEFAULT = "8"    # 引擎默认值，只打印不判

# ---- 阈值（全部来自实测，括号里是本次实测值）----
PIX_IDENTICAL = 0.05   # B1 Default 两次渲染：逐像素一致（实测 0.000000）
BASELINE_MAX = 0.60    # B4 Skin(=0) vs Default：只允许 LUT 量化噪声（实测 0.2041；
                       #     若采样边界退化，这里会读到 ~5.6，差 ~9 倍）
SKIN_SIGNAL_MIN = 1.5  # B2 Skin 开/关差异下限（实测 6.4108）
RED_MARGIN_MIN = 1.0   # B3 ΔR−ΔB 下限（实测 +12.5040）


# ---------------------------------------------------------------- 场景
def material(name, shading="default"):
    return {
        "kind": "procedural",
        "name": name,
        "baseColor": list(SKIN_BASE) + [1.0],
        "metallic": 0.0,
        "roughness": 0.6,
        "normalScale": 1.0,
        "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0],
        "doubleSided": False,
        "alphaMode": "opaque",
        "alphaBlend": False,
        "alphaCutoff": 0.5,
        "ditherAlpha": False,
        "shadingModel": shading,
        "albedo": None,
    }


def build_scene(path, shading="default", with_ball=True):
    ground = {
        "name": "Ground",
        "transform": {"position": [0.0, 0.0, 0.0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": 120.0,
                 "name": "sss_ground"},
        "material": {
            "kind": "procedural", "name": "sss_ground_mat",
            "baseColor": [0.05, 0.05, 0.06, 1.0],
            "metallic": 0.0, "roughness": 1.0,
            "normalScale": 1.0, "ao": 1.0,
            "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
            "alphaMode": "opaque", "alphaBlend": False,
            "alphaCutoff": 0.5, "ditherAlpha": False, "albedo": None,
        },
    }
    ents = [ground]
    if with_ball:
        # 用较多细分，法线在球面上平滑变化 → fwidth(N) 反映真实曲率。
        ents.append({
            "name": "SkinBall",
            "transform": {"position": list(BALL_POS), "rotation": [0, 0, 0],
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": False, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "sphere",
                     "size": BALL_RADIUS, "name": "sss_ball",
                     "segments": 64, "rings": 32},
            "material": material("sss_ball_mat", shading),
        })
    doc = {
        "version": 1,
        "generator": "verify_sss.py",
        "light": {"direction": list(SUN_DIR), "color": [1.0, 0.98, 0.95],
                  "intensity": SUN_INTENSITY, "castsShadow": False,
                  "ambientScale": AMBIENT_SCALE},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return len(ents)


# ---------------------------------------------------------------- 运行
def run_once(exe, scene, extra_env, shot_path):
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


def erode(mask, it=3):
    m = mask
    for _ in range(it):
        m = (m & np.roll(m, 1, 0) & np.roll(m, -1, 0)
             & np.roll(m, 1, 1) & np.roll(m, -1, 1))
    return m


# ---------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--with-default-scale", action="store_true",
                    help="额外跑一轮 MYVK_SSS=8（引擎默认刻度）作参考，只打印不判")
    args = ap.parse_args()

    exe = args.exe
    if not os.path.exists(exe):
        print("FAIL: 找不到可执行文件 %s" % exe)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)
    failures = []
    print("=" * 68)
    print("SSS 验证（R4-2：预积分皮肤散射 LUT）")
    print("=" * 68)

    # ---------------------------------------------------------- A 组：LUT 数值
    print("\n[A] LUT 数值（make_sss_lut.build_lut）")
    lut = build_lut()
    ndotl = np.linspace(-1.0, 1.0, SIZE)

    row0 = lut[0]
    target = np.maximum(ndotl, 0.0)
    err0 = float(np.abs(row0 - target[:, None]).max())
    print("  A1 曲率=0 行 vs max(N·L,0) 最大误差 = %.5f（阈值 0.01）" % err0)
    if err0 > 0.01:
        print("     ✗ 平面没有退化成 Lambert —— 核归一化写错了")
        failures.append("A1: 曲率0 未退化成 Lambert")
    else:
        print("     ✓ 曲率=0 → 逐点等于 max(N·L,0)")

    col0 = int(((0.0 + 1.0) / 2.0) * (SIZE - 1))          # N·L ≈ 0
    rgb0 = lut[SIZE - 1, col0]
    rb0 = float(rgb0[0] - rgb0[2])
    print("  A2 曲率=1 / N·L=0 → RGB = (%.4f, %.4f, %.4f)，R−B = %.4f"
          % (rgb0[0], rgb0[1], rgb0[2], rb0))
    if not (rgb0[0] > rgb0[1] > rgb0[2] and rb0 > 0.05):
        print("     ✗ 阴影侧没有出现红向偏色")
        failures.append("A2: 阴影侧未泛红")
    else:
        print("     ✓ R>G>B 且 R−B>0.05 → 泛红透光")

    rgb1 = lut[SIZE - 1, -1]
    print("  A3 曲率=1 / N·L=1 → RGB = (%.4f, %.4f, %.4f)"
          % (rgb1[0], rgb1[1], rgb1[2]))
    if float(rgb1.min()) < 0.9:
        print("     ✗ 正面被压暗/染色 —— 逐行归一化失效")
        failures.append("A3: 正对光被染色")
    else:
        print("     ✓ 正面三通道接近 1")

    # A4：固定 N·L，R 随曲率单调不减 + 有可见增量
    colx = int((0.3 + 1.0) / 2.0 * (SIZE - 1))            # N·L ≈ 0.3
    curvs = [0, 64, 128, 192, 255]
    series = [float(lut[c, colx, 0]) for c in curvs]
    mono = all(series[i] <= series[i + 1] + 1e-6 for i in range(len(series) - 1))
    gain = series[-1] - series[0]
    print("  A4 N·L=0.3，曲率 0→1 的 R = %s，增量 %.4f"
          % (" → ".join("%.3f" % v for v in series), gain))
    if not (mono and gain > 0.05):
        print("     ✗ 曲率轴没在起作用（非单调或增量太小）")
        failures.append("A4: 曲率轴失效")
    else:
        print("     ✓ 单调不减且增量 %.3f > 0.05" % gain)

    # ---------------------------------------------------------- 场景
    kill_stale_editors()

    n_ground = build_scene(os.path.join(OUT_DIR, "sss_scene_ground.json"),
                           with_ball=False)
    for tag, sh in (("default", "default"), ("skin", "skin")):
        build_scene(os.path.join(OUT_DIR, "sss_scene_%s.json" % tag),
                    shading=sh)
    print("\n场景已写出（实体数 无球=%d，有球=2）" % n_ground)

    def shot(name):
        return os.path.join(OUT_DIR, "sss_%s.png" % name)

    def step(name, scene, sss):
        _, vp, img, errs = run_once(exe, scene, {"MYVK_SSS": sss}, shot(name))
        if errs:
            print("  [%s] ⚠ 错误输出 %d 行，例：%s"
                  % (name, len(errs), errs[0][:120]))
            failures.append("%s: 有错误/验证层输出" % name)
        return img, vp

    print("\n[运行] 6 次（ground / default×2 / skin×3）")
    img_g, vp = step("ground", os.path.join(OUT_DIR,
                                            "sss_scene_ground.json"), SSS_ON)
    img_d0, _ = step("d0", os.path.join(OUT_DIR, "sss_scene_default.json"),
                     SSS_OFF)
    img_d16, _ = step("d16", os.path.join(OUT_DIR, "sss_scene_default.json"),
                      SSS_ON)
    img_s0, _ = step("s0", os.path.join(OUT_DIR, "sss_scene_skin.json"),
                     SSS_OFF)
    img_s16, _ = step("s16", os.path.join(OUT_DIR, "sss_scene_skin.json"),
                      SSS_ON)
    img_s4, _ = step("s4", os.path.join(OUT_DIR, "sss_scene_skin.json"),
                     SSS_MID)

    if img_g is None or img_d0 is None:
        print("FAIL: 没抓到截图")
        return 2

    vpb = parse_vp(vp)
    if vpb:
        x0, y0, x1, y1 = vpb
    else:
        x0, y0, x1, y1 = 268, 118, 790, 628

    def crop(im):
        return im[y0:y1, x0:x1]

    g, d0, d16 = crop(img_g), crop(img_d0), crop(img_d16)
    s0, s16, s4 = crop(img_s0), crop(img_s16), crop(img_s4)

    # ---- 球区掩膜：有球 − 无球 ----
    mask = (luma(d0) - luma(g)) > 5.0
    mask = erode(mask, 3)
    npx = int(mask.sum())
    print("\n[掩膜] 球区像素 = %d（占裁剪区 %.2f%%）"
          % (npx, 100.0 * npx / max(mask.size, 1)))
    if npx < 500:
        print("FAIL: 球区太小（<500 px），场景/相机没对上")
        return 2

    def md(a, b):
        """球区平均亮照差（全通道）。"""
        return float(np.abs(a[mask] - b[mask]).mean())

    def cd(a, b):
        """球区逐通道平均**有符号**增量。"""
        return (a[mask] - b[mask]).mean(axis=0)

    # ---- B1：Default 不读 sssParams ----
    print("\n[B1] Default 球：MYVK_SSS=0 vs =%s（应逐像素一致）" % SSS_ON)
    d_b1 = md(d0, d16)
    print("     球区平均亮度差 = %.6f（阈值 %.3f）" % (d_b1, PIX_IDENTICAL))
    if d_b1 > PIX_IDENTICAL:
        print("     ✗ Default 被 sssParams 影响 —— binding19/UBO 泄漏进 Default 路径")
        failures.append("B1: Default 对 MYVK_SSS 敏感")
    else:
        print("     ✓ Default 完全不引用 SSS 代码路径")

    # ---- B2：Skin 敏感 ----
    print("\n[B2] Skin 球：MYVK_SSS=0 vs =%s（应显著不同）" % SSS_ON)
    d_b2 = md(s0, s16)
    print("     球区平均亮度差 = %.4f（阈值 %.2f）" % (d_b2, SKIN_SIGNAL_MIN))
    if d_b2 < SKIN_SIGNAL_MIN:
        print("     ✗ Skin 没反应 —— LUT 没被采样进光照")
        failures.append("B2: Skin 对 MYVK_SSS 不敏感")
    else:
        print("     ✓ Skin 的漫反射由 LUT 驱动")

    # ---- B3：泛红方向 ----
    print("\n[B3] Skin 差异的通道方向：红通道增益应远大于蓝通道")
    dc = cd(s16, s0)
    ratio = dc[0] / max(dc[2], 1e-6)
    print("     逐通道平均增量 ΔRGB = (%+.4f, %+.4f, %+.4f)"
          % (dc[0], dc[1], dc[2]))
    print("     ΔR−ΔB = %+.4f（阈值 %.2f）；ΔR/ΔB = %.1f×（要求 > 5×）"
          % (dc[0] - dc[2], RED_MARGIN_MIN, ratio))
    if dc[0] - dc[2] <= RED_MARGIN_MIN or ratio <= 5.0:
        print("     ✗ 红通道没有远大于蓝通道 —— 方向不对（可能整体提亮/偏蓝）")
        failures.append("B3: SSS 未表现为泛红")
    else:
        print("     ✓ 红光渗得最远 → 泛红透光")

    # ---- B4：关着 = 基线（同时守卫采样边界不变量）----
    print("\n[B4] 零回归：MYVK_SSS=0 时 Skin 球应只剩 LUT 量化噪声")
    d_b4 = md(s0, d0)
    print("     球区平均亮度差 = %.4f（阈值 %.2f；采样边界若退化会读到 ~5.6）"
          % (d_b4, BASELINE_MAX))
    if d_b4 > BASELINE_MAX:
        print("     ✗ Skin(曲率0) 与 Default 不等价 —— 关着也改变了基线；")
        print("       若数值很大，优先怀疑 LUT 采样坐标落在 REPEAT 边界上")
        failures.append("B4: SSS 关闭时 Skin != Default")
    else:
        print("     ✓ 关闭时 Skin 逐像素等于 Default（噪声 %.2f ≪ 信号 %.2f）"
              % (d_b4, d_b2))

    # ---- B5：曲率轴单调 ----
    print("\n[B5] 曲率轴单调：MYVK_SSS=%s 的差异应严格小于 =%s 的 1/3"
          % (SSS_MID, SSS_ON))
    d_b5 = md(s4, s0)
    print("     球区平均亮度差 = %.4f（信号 %.4f；要求 0 < x < %.4f）"
          % (d_b5, d_b2, d_b2 / 3.0))
    if not (d_b5 > PIX_IDENTICAL and d_b5 < d_b2 / 3.0):
        print("     ✗ 刻度没有单调地改变曲率（要么全无响应，要么已饱和）")
        failures.append("B5: 曲率轴非单调/饱和")
    else:
        print("     ✓ 刻度 %s 介于「关闭」与 %s 之间，曲率轴真的在动"
              % (SSS_MID, SSS_ON))

    # ---- 参考输出（不判）----
    if args.with_default_scale:
        img_s8, _ = step("s8", os.path.join(OUT_DIR, "sss_scene_skin.json"),
                         SSS_DEFAULT)
        print("\n[参考] Skin 球 MYVK_SSS=%s（引擎默认刻度）"
              " 与 =0 的平均亮度差 = %.4f（球区）"
              % (SSS_DEFAULT, md(crop(img_s8), s0)))

    # ---------------------------------------------------------- 可视化对比图
    # 路线的验收原文是"皮肤在强侧光下鼻梁/耳缘**泛红透光**"——那是视觉判据，
    # 上面那些数字只是它的代理。所以顺手拼一张肉眼可查的对比图：
    # Default / Skin(曲率0) / Skin(曲率16) 三块并排，外加一张放大 6 倍的差分，
    # 一眼能看出增益集中在**明暗交界线**且**偏红**。
    try:
        ys, xs = np.nonzero(mask)
        pad = 12
        bx0 = max(int(xs.min()) - pad, 0)
        bx1 = min(int(xs.max()) + pad, mask.shape[1])
        by0 = max(int(ys.min()) - pad, 0)
        by1 = min(int(ys.max()) + pad, mask.shape[0])
        rgb = lambda im: Image.fromarray(  # noqa: E731
            im[by0:by1, bx0:bx1].astype("uint8"))
        panels = [(rgb(d0), "Default"),
                  (rgb(s0), "Skin  sss=0"),
                  (rgb(s16), "Skin  sss=%s" % SSS_ON)]
        dv = (s16[by0:by1, bx0:bx1] - s0[by0:by1, bx0:bx1]) * 6.0 + 128.0
        panels.append((Image.fromarray(np.clip(dv, 0, 255).astype("uint8")),
                       "delta x6  (Skin %s - 0)" % SSS_ON))
        k, lab = 3, 16
        cw = sum(p.width * k for p, _ in panels) + lab * (len(panels) - 1)
        ch = max(p.height * k for p, _ in panels) + lab
        canvas = Image.new("RGB", (cw, ch), (26, 26, 30))
        drw, cx = ImageDraw.Draw(canvas), 0
        for p, tag in panels:
            canvas.paste(p.resize((p.width * k, p.height * k), Image.NEAREST),
                         (cx, lab))
            drw.text((cx + 4, 3), tag, fill=(238, 238, 238))
            cx += p.width * k + lab
        cmp_path = os.path.join(OUT_DIR, "sss_compare.png")
        canvas.save(cmp_path)
        print("\n[可视化] 对比图（球区放大 3 倍）: %s" % cmp_path)
    except Exception as ex:          # 拼图失败不影响验收结论
        print("\n[可视化] 跳过（%s: %s）" % (type(ex).__name__, ex))

    # ---------------------------------------------------------- 结论
    print("\n" + "=" * 68)
    if failures:
        print("结果: FAIL（%d 项）" % len(failures))
        for f in failures:
            print("  · " + f)
        return 1
    print("结果: PASS（A 组 4 项数值 + B 组 5 项渲染，全部通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
