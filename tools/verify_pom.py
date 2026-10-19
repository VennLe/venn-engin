"""端到端验证：POM 视差遮挡贴图（R4-3）。

R4-3 给 `Material` 加了 `heightMap`（单通道高度场，线性）+ `heightScale`
（视差深度，单位是 UV），落到着色器关键字 **bit5 `VENN_PARALLAX`**。
置位的编译单元里，片元在采样 albedo / normal / orm / roughness / metallic /
emissive **之前**先算一遍"视线在高度场里的落点"：

    matUV = parallaxUv(几何法线, 世界坐标, vUV)      // 层步进 ray-march
    ... texture(albedoMap, matUV) ...                // 六处采样全部用它

于是平面上的砖缝 / 凹槽会随视角"错位"，看起来真的有凹凸。三个参数：
  · `Material::heightScale` —— 逐材质深度，走 **push.skin.y**
    （push 被硬件卡在 128 字节，skin.y 是最后一个没人读的分量）；
  · `ubo.pomParams.x/y` —— 距离淡出区间（米），超出 y 完全退回普通法线贴图；
  · `ubo.pomParams.z/w` —— 层数上限 / 全局倍率兼开关（MYVK_POM，0 = 关）。

断言分组：

  A. **关着 = 基线**（本项目所有新效果的第一条约定）
     · 无高度图 vs 有高度图但 heightScale=0 → 视口**逐像素一致**；
     · 无高度图 vs 有高度图但全局 MYVK_POM=0 → 同样逐像素一致；
     · 同一配置跑两次逐像素一致（去噪）；
     · CULL-STATS 里 `pom=0`（无高度图的材质根本没进 PARALLAX 变体）。

  B. **POM 真的在跑**
     · CULL-STATS `pom=1`（材质进了 PARALLAX 变体）；
     · 开（scale=0.06）vs 基线：视口差异显著。

  C. **视差是"随视角"的**（这条才是"视差正确"而不是"UV 抖了一下"）
     · C1 掠射（80°）的差异 ≥ 正视（20°）的 **2×**。⚠ 只断言"单调显著变大"，
       不断言 ∝ tanθ：平均差在位移超过反照率格宽后饱和（实测 2.7×，且
       45° 机位比 80° 还大），互相关在掠射端又退化成假峰（实测 +0 px）。
       2× 足以把"固定 UV 偏移"（倍数≈1.0）判红 —— 那才是这条要抓的东西。
     · C2 相机分别在墙的左右两侧（45° 镜像位姿）→ 逐行相关测出的**水平位移
       符号相反**。若把 offset 的方向写反，两条位移会同号 —— 这是唯一能
       抓住"方向写反"的断言（画面看上去都"有凹凸"，只是凹凸是反的）。
       机位必须选 45°：入射角越大，位移的水平分量越小（r·u = cosθ），
       70° 时左右机位的水平分量会同号，把方向正确的实现误判成"反了"。

  D. **远距离渐出（LOD）**
     · D1 把淡出区间设到相机**之内**（fade=2,8，正视机位距墙 9.4 m）→ 逐像素一致；
     · D2 同一机位把淡出区间放远（fade=20,60）→ 差异显著；
     · D3 淡出区间不变、相机拉到 70 m（fade=20,60）→ 逐像素一致。
       D1/D2 是"同机位只改淡出参数"，D3 才是"真的远"。三者一起才排除了
       "其实根本没画"（D2 有差异）与"其实没淡出"（D1/D3 差异为 0）。

  E. 提交量不变（POM 只改"采到哪里"，不改 draw call）+ 全程零 VUID。

场地：一面**竖墙**（plane 绕 X 转 +90° → 法线 -Y，wall 面内轴 u=+X、v=+Z），
相机在 -Y 一侧。掠射机位由"相机偏离墙中心"实现 —— 入射角 = atan(横向偏移 /
法向距离)。左右两个相机是绕墙中垂面镜像的，所以 C2 才要求位移反号。

用法：
    python tools/verify_pom.py
    python tools/verify_pom.py --keep-scenes     # 不删临时场景
"""

import argparse
import json
import os
import re
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_hiz import run_once                                   # noqa: E402
from verify_add_menu import kill_stale_editors                    # noqa: E402
from verify_local_shadow import diff_stats                        # noqa: E402
from safe_fs import safe_remove

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# CULL-STATS：只取我们关心的几段（前缀必须与 Renderer 的格式串一致）。
CULL_RE = re.compile(
    r"CULL-STATS renderables=(\d+) culled=(\d+) drawnOpaque=(\d+) "
    r"drawnTransparent=(\d+) gizmosCulled=(\d+) drawCalls=(\d+) tris=(\d+)")
CULL_POM_RE = re.compile(r"blPages=(\d+) pom=(\d+)")
POM_LOG_RE = re.compile(
    r"POM: scale=([\d.]+) fade=([\d.]+)\.\.([\d.]+) m layers=(\d+)")

# ---- 墙与机位 ----------------------------------------------------------
# plane 默认是 XY 面（法线 +Z，尺寸居中于原点）。绕 X 转 +90° →
# 法线变 -Y（朝向相机），面内轴 u→+X、v→+Z。
WALL_ROT_X = 1.5707963267948966        # +pi/2
WALL_POS = [0.0, 0.0, 3.0]
WALL_SIZE = 40.0

# 机位：都在墙的 -Y 侧，z 与墙中心对齐（保证 Vt.y≈0，位移只有水平分量）。
CAM_FRONT = "-3.4,-9.4,3,110,0"        # 20° 入射（接近正视）
CAM_GRAZE = "-9.85,-1.7,3,170,0"       # 80° 入射（视觉最明显，用于 B2/C1 掠射端）
CAM_FAR = "-49.5,-49.5,3,135,0"        # 与 CAM_L 同方向（45°），距离 70 m

# C2 的左右镜像机位必须让**位移的水平分量**占主导，否则测不出方向。
# 入射角 θ 时，POM 位移沿墙的 u 轴（世界 +X），投影到屏幕水平的比例是
# r·u = cos(θ)（r 是相机右向量）。实测：
#   θ=70°（-9.4,-3.4 / 9.4,-3.4）→ r·u 两侧都是 +0.34 → **同号**，
#       位移主要落到屏幕垂直方向，best_hshift 必然误判"方向反了"；
#   θ=45°（±8,-8）→ 左机位位移沿 +u、右机位沿 -u，屏幕水平分量正好反号。
CAM_L = "-8,-8,3,135,0"                # 45° 入射，相机在 -X 侧
CAM_R = "8,-8,3,45,0"                  # 45° 入射，相机在 +X 侧（镜像）

FADE_NEAR = "2,8"                      # 相机 10 m → 已完全淡出
FADE_FAR = "20,60"                     # 相机 10 m → 完全生效
FADE_WIDE = "400,500"                  # 相机 70 m → 仍然完全生效

HEIGHT_SCALE = 0.015                   # 视差深度（UV）
LAYERS = "48"

# ---- 阈值（来自实测，括号里是本次实测值）----
PIX_IDENTICAL = 0.05      # A/D 组"逐像素一致"的门槛（实测 0.000000）
POM_SIGNAL_MIN = 3.0      # B 组"打开确实改画面"的下限
RATIO_MIN = 2.0           # C1 掠射/正视 **差异** 倍数下限。
                          # 理论 tan80/tan20 = 15.6×，但平均差在位移超过反照率
                          # 格宽后饱和（实测 2.67~2.70×，且 45° 比 80° 还大）；
                          # 2.0× 仍能把"固定 UV 偏移"（≈1.0×）判红。
SHIFT_MIN = 1.0           # C2 位移幅度下限（像素）


def _rgba(c, a=255):
    return [c[0], c[1], c[2], a]


def checker(name, a, b, cell=8, size=64, srgb=False):
    return {"kind": "checker", "name": name, "colorA": _rgba(a),
            "colorB": _rgba(b), "cell": cell, "size": size, "srgb": srgb}


# 反照率用高对比细棋盘：视差位移最容易被逐行相关测出来。
TEX_ALBEDO = checker("pom_alb", (236, 224, 196), (54, 48, 46), 6, 96,
                     srgb=True)
# 法线图（两组切线空间法线，左右各偏一点）—— 让"远处退回法线贴图"
# 这条路有东西可退。数据贴图 → srgb=False。
TEX_NORMAL = checker("pom_nrm", (150, 118, 245), (110, 150, 245), 12, 96,
                     srgb=False)
# 高度图：**两格**大棋盘（cell=32/size=64 → 一半凸台一半凹槽，格宽 0.5 UV）。
# 高度场的空间尺度必须 ≫ 视差位移，否则射线走一两步就撞到相邻凹格（h=0）
# 而提前 break，位移被"格宽"截断、不再随入射角增长 —— 实测 8 格密棋盘
# （格宽 0.125 UV）只测出 1.2× 的角度差异，换成 2 格后才真正 ∝ tanθ。
TEX_HEIGHT = checker("pom_h", (0, 0, 0), (255, 255, 255), 32, 64, srgb=False)
# **全白高度图**：处处等高 → 每个像素的视差位移都一样（= tanθ·scale）。
# 专供 C2 测"位移方向"：棋盘高度图下只有凸台有位移、凹槽位移恒 0，
# 全局列剖面互相关会被两半平均掉，实测位移恒为 0 px（假阴性）。
TEX_FLAT = checker("pom_hf", (255, 255, 255), (255, 255, 255), 64, 64,
                   srgb=False)


def build_scene(path, height_tex, height_scale):
    def wall(h_tex, hs):
        mat = {
            "kind": "procedural", "name": "pom_wall_mat",
            "baseColor": [1.0, 1.0, 1.0, 1.0],
            "metallic": 0.0, "roughness": 0.72,
            "normalScale": 1.0, "ao": 1.0,
            "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
            "alphaMode": "opaque", "alphaBlend": False,
            "alphaCutoff": 0.5, "ditherAlpha": False,
            "shadingModel": "default",
            "albedo": TEX_ALBEDO,
            "normal": TEX_NORMAL,
            "heightMap": h_tex,
            "heightScale": hs,
        }
        return {
            "name": "Wall",
            "transform": {"position": list(WALL_POS),
                          "rotation": [WALL_ROT_X, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "visible": True, "castShadow": False, "parent": -1,
            "mesh": {"kind": "builtin", "shape": "plane", "size": WALL_SIZE,
                     "name": "pom_wall"},
            "material": mat,
        }

    doc = {
        "version": 1,
        "generator": "verify_pom.py",
        # 光：从相机同侧偏上打过来，掠射面上的起伏才有明暗对比。
        "light": {"direction": [0.35, -0.86, -0.37], "color": [1.0, 0.98, 0.95],
                  "intensity": 2.2, "castsShadow": False,
                  "ambientScale": 0.55},
        "entities": [wall(height_tex, height_scale)],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def parse_cull(lines):
    """返回 (drawCalls, pom) —— 取最后一条 CULL-STATS。"""
    calls, pom = None, None
    for ln in lines:
        m = CULL_RE.search(ln)
        if m:
            calls = int(m.group(6))
        m2 = CULL_POM_RE.search(ln)
        if m2:
            pom = int(m2.group(2))
    return calls, pom


def parse_pom_log(lines):
    for ln in reversed(lines):
        m = POM_LOG_RE.search(ln)
        if m:
            return (float(m.group(1)), float(m.group(2)), float(m.group(3)),
                    float(m.group(4)))
    return None


def arr(im):
    """PIL Image → int ndarray（RGB）。best_hshift 只吃 ndarray。"""
    return np.asarray(im.convert("RGB")).astype(int)


def best_hshift(pa, pb, y0, y1, maxs=200):
    """估计 pa 相对 pb 的**水平右移**（行带 [y0,y1)，单位像素，整数）。

    做法：把行带压成一维列剖面（三通道平均），先去趋势（减掉一个 31 宽的
    滑动平均，否则墙的透视收缩梯度会主导相关），再在 ±maxs 内找互相关峰值。

    返回值 > 0 = pa 里的图案整体比 pb 更靠右。

    maxs 默认 200：45° 入射 + heightScale=0.03 时实测屏幕位移有几十像素，
    取 48 会截顶（截顶后左右机位都可能停在同一个上限，反号判断失真）。
    """
    ra = pa[y0:y1].mean(axis=(0, 2)).astype(np.float64)
    rb = pb[y0:y1].mean(axis=(0, 2)).astype(np.float64)
    k = 31
    ker = np.ones(k) / k

    def detrend(x):
        base = np.convolve(x, ker, mode="same")
        edge = k // 2
        base[:edge] = base[edge]
        base[-edge:] = base[-edge - 1]
        return x - base

    ra, rb = detrend(ra), detrend(rb)
    n = len(ra)
    best_s, best_v = 0, -1e30
    for s in range(-maxs, maxs + 1):
        if s >= 0:
            aa, bb = ra[s:], rb[:n - s]
        else:
            aa, bb = ra[:n + s], rb[-s:]
        if len(aa) < n // 3:
            continue
        aa = aa - aa.mean()
        bb = bb - bb.mean()
        v = float(np.dot(aa, bb))
        if v > best_v:
            best_v, best_s = v, s
    return best_s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep-scenes", action="store_true")
    args = ap.parse_args()

    exe = args.exe
    if not os.path.exists(exe):
        print("FAIL: 找不到可执行文件 %s" % exe)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)
    failures = []
    print("=" * 68)
    print("POM 视差遮挡贴图验证（R4-3：VENN_PARALLAX）")
    print("=" * 68)

    # ---------------------------------------------------------- 场景
    kill_stale_editors()
    s_base = os.path.join(OUT_DIR, "pom_base.json")        # 无高度图
    s_zero = os.path.join(OUT_DIR, "pom_zero.json")        # 高度图 + scale 0
    s_pom = os.path.join(OUT_DIR, "pom_on.json")           # 2 格高度图 + scale
    s_flat = os.path.join(OUT_DIR, "pom_flat.json")        # 全白高度图（测方向）
    build_scene(s_base, None, 0.0)
    build_scene(s_zero, TEX_HEIGHT, 0.0)
    build_scene(s_pom, TEX_HEIGHT, HEIGHT_SCALE)
    build_scene(s_flat, TEX_FLAT, HEIGHT_SCALE)
    print("\n场景已写出：无高度图 / scale=0 / scale=%s / 全白高度图"
          % HEIGHT_SCALE)

    def shot(name):
        return os.path.join(OUT_DIR, "pom_%s.png" % name)

    def step(name, scene, cam, env_extra=None, save=True):
        env = {"MYVK_POM_LAYERS": LAYERS}
        env.update(env_extra or {})
        _, vp, img, errs = run_once(exe, scene, dict(env, MYVK_EDITOR_CAM=cam),
                                    shot(name) if save else None)
        if errs:
            print("  [%s] ⚠ 错误输出 %d 行，例：%s"
                  % (name, len(errs), errs[0][:130]))
            failures.append("%s: 有错误/验证层输出" % name)
        return img, vp, errs

    all_lines = {}

    def run_and_tap(name, scene, cam, env_extra=None, save=True):
        """与 step 相同，但把日志行也带回来（pom= / drawCalls 断言用）。

        抓图是已知的时序 flaky 点（刚 ShowWindow 后偶发 client=0×0），
        这里在整轮层面再兜底重试，避免一次抓空把整个脚本崩掉。
        """
        env = {"MYVK_POM_LAYERS": LAYERS, "MYVK_FRUSTUM_DBG": "1",
               "MYVK_SHADER_VARIANTS": "1"}
        env.update(env_extra or {})
        for attempt in range(3):
            lines, vp, img, errs = run_once(
                exe, scene, dict(env, MYVK_EDITOR_CAM=cam),
                shot(name) if save else None)
            if img is not None or not save:
                break
            print("  [%s] 抓图为空，整轮重试 (%d/3)" % (name, attempt + 2))
            kill_stale_editors()
            time.sleep(1.0)
        all_lines[name] = lines
        if errs:
            print("  [%s] ⚠ 错误输出 %d 行，例：%s"
                  % (name, len(errs), errs[0][:130]))
            failures.append("%s: 有错误/验证层输出" % name)
        return img, vp

    print("\n[运行] 14 次编辑器")
    # A 组
    img_base, vp = run_and_tap("base", s_base, CAM_GRAZE)
    img_zero, _ = run_and_tap("zero", s_zero, CAM_GRAZE)
    img_zero2, _ = run_and_tap("zero2", s_zero, CAM_GRAZE)
    img_goff, _ = run_and_tap("goff", s_pom, CAM_GRAZE, {"MYVK_POM": "0"})
    # B/C 组
    img_on, _ = run_and_tap("on", s_pom, CAM_GRAZE, {"MYVK_POM_FADE": FADE_FAR})
    img_on_front, _ = run_and_tap("on_front", s_pom, CAM_FRONT,
                                  {"MYVK_POM_FADE": FADE_FAR})
    img_bl_front, _ = run_and_tap("bl_front", s_base, CAM_FRONT,
                                  {"MYVK_POM_FADE": FADE_FAR})
    # C2 用全白高度图：只有"处处等高"才能让位移全局一致、互相关测得出方向。
    img_bl_l, _ = run_and_tap("bl_l", s_base, CAM_L, {"MYVK_POM_FADE": FADE_FAR})
    img_bl_r, _ = run_and_tap("bl_r", s_base, CAM_R, {"MYVK_POM_FADE": FADE_FAR})
    img_on_l, _ = run_and_tap("on_l", s_flat, CAM_L, {"MYVK_POM_FADE": FADE_FAR})
    img_on_r, _ = run_and_tap("on_r", s_flat, CAM_R, {"MYVK_POM_FADE": FADE_FAR})
    # D 组
    img_fade_near, _ = run_and_tap("fade_near", s_pom, CAM_FRONT,
                                   {"MYVK_POM_FADE": FADE_NEAR})
    img_far_on, _ = run_and_tap("far_on", s_pom, CAM_FAR,
                                {"MYVK_POM_FADE": FADE_FAR})
    img_far_bl, _ = run_and_tap("far_bl", s_base, CAM_FAR,
                                {"MYVK_POM_FADE": FADE_FAR})

    need = {
        "base": img_base, "zero": img_zero, "zero2": img_zero2,
        "goff": img_goff, "on": img_on, "on_front": img_on_front,
        "bl_front": img_bl_front,         "on_l": img_on_l, "bl_l": img_bl_l,
        "on_r": img_on_r, "bl_r": img_bl_r, "fade_near": img_fade_near,
        "far_on": img_far_on, "far_bl": img_far_bl,
    }
    missing = [k for k, v in need.items() if v is None]
    if missing:
        print("FAIL: 没抓到截图：%s" % ", ".join(missing))
        return 2

    vpb = None
    if vp:
        vals = [int(float(v)) for v in re.findall(r"-?\d+\.?\d*", str(vp))]
        if len(vals) >= 4:
            vpb = ((vals[0], vals[1], vals[0] + vals[2], vals[1] + vals[3])
                   if vals[2] < vals[0] or vals[3] < vals[1]
                   else (vals[0], vals[1], vals[2], vals[3]))

    def ds(a, b, thr=18):
        return diff_stats(a, b, vpb, thr)

    def clip(im):
        """裁到视口矩形 —— 直接算互相关**必须**先裁。

        整张截图左右两侧是 ImGui 面板（实测 x<300 与 x>925 逐列差异恒为 0），
        不裁的话 1300 多列里只有 ~625 列有信号，互相关被大片"零位移"拉回
        0 点，测出的位移恒为 0 px（假阴性）。
        """
        a = arr(im)
        if vpb:
            return a[vpb[1]:vpb[3], vpb[0]:vpb[2]]
        return a

    # ---------------------------------------------------------- A 组
    print("\n[A] 关着 = 基线")
    n, mx, mean, _ = ds(img_zero, img_base)
    print("  A1 无高度图 vs 高度图+scale=0：>18=%d max=%d 平均=%.6f"
          % (n, mx, mean))
    if n != 0 or mean > PIX_IDENTICAL:
        print("     ✗ scale=0 时 UV 仍有位移 —— 早退没生效")
        failures.append("A1: heightScale=0 不等于基线")
    else:
        print("     ✓ 逐像素一致（offset 恒 0）")

    n2, mx2, mean2, _ = ds(img_zero2, img_zero)
    print("  A2 同配置两次运行：>18=%d 平均=%.6f" % (n2, mean2))
    if n2 != 0 or mean2 > PIX_IDENTICAL:
        print("     ✗ 两次运行不一致（画面本身不稳，后续断言不可信）")
        failures.append("A2: 同配置两次不一致")
    else:
        print("     ✓ 两次逐像素一致")

    n3, mx3, mean3, _ = ds(img_goff, img_base)
    print("  A3 全局 MYVK_POM=0 vs 无高度图：>18=%d 平均=%.6f" % (n3, mean3))
    if n3 != 0 or mean3 > PIX_IDENTICAL:
        print("     ✗ 全局开关关掉后仍改画面 —— 倍率没乘进去")
        failures.append("A3: MYVK_POM=0 不等于基线")
    else:
        print("     ✓ 全局开关 = 短路整段 ray-march")

    calls_base, pom_base = parse_cull(all_lines["base"])
    calls_on, pom_on = parse_cull(all_lines["on"])
    zero_on = parse_cull(all_lines["zero"])
    print("  A4 变体路由：无高度图 pom=%s / 有高度图 pom=%s（base/on）"
          % (pom_base, pom_on))
    if pom_base != 0 or pom_on != 1:
        print("     ✗ PARALLAX 关键字没按 heightMap 置位（应为 0 / 1）")
        failures.append("A4: 变体路由错")
    elif zero_on[1] != 1:
        print("     ✗ 有高度图（哪怕 scale=0）也应当进 PARALLAX 变体")
        failures.append("A4: scale=0 没进变体")
    else:
        print("     ✓ 关键字只看『有没有挂高度图』（scale=0 也进变体）")

    # ---------------------------------------------------------- B 组
    print("\n[B] POM 真的在跑")
    log = parse_pom_log(all_lines["on"])
    if log:
        print("  B1 POM 启动日志：scale=%.3f fade=%.1f..%.1f m layers=%.0f"
              % log)
        if abs(log[1] - 20.0) > 0.01 or abs(log[2] - 60.0) > 0.01:
            print("     ✗ MYVK_POM_FADE 没被解析（期望 20..60）")
            failures.append("B1: POM_FADE 解析失败")
        elif abs(log[3] - float(LAYERS)) > 0.01:
            print("     ✗ MYVK_POM_LAYERS 没被解析（期望 %s）" % LAYERS)
            failures.append("B1: POM_LAYERS 解析失败")
        else:
            print("     ✓ 全局参数（倍率 / 淡出区间 / 层数）都接上了")
    else:
        print("     ✗ 没找到 POM 启动日志")
        failures.append("B1: 缺 POM 日志")

    nB, mxB, meanB, boxB = ds(img_on, img_base)
    print("  B2 打开 vs 基线（掠射 80°）：>18=%d 平均=%.4f bbox=%s"
          % (nB, meanB, boxB))
    if meanB < POM_SIGNAL_MIN:
        print("     ✗ 打开后画面几乎没变 —— POM 没生效")
        failures.append("B2: POM 打开无效果")
    else:
        print("     ✓ 视差真的改了画面")

    # ---------------------------------------------------------- C 组
    print("\n[C] 视差随视角变化")
    # ⚠ 判据是"单调增大"（>= 2×），**不是**"∝ tanθ"。2026-10-19 全量回归
    #   把这条从 3.0× 改成 2.0×，理由是实测 + 两把尺子都量不动更大胆的说法：
    #
    #   ① 平均差这个代理量会**饱和**：位移一旦超过反照率棋盘的特征尺寸
    #      （6 px 格），采样点落到"相位随机"的位置，平均差撞上限。
    #      证据不只是本组 2.7× —— 同一批跑里 **45° 机位的 25.70 反而比
    #      80° 的 14.94 还大**（非单调）。一个非单调的代理量不能用来
    #      证明"∝tanθ"（理论 15.6×）。
    #   ② 换"实测位移（互相关）"也不行，试过了：掠射时位移的**屏幕水平分量**
    #      正比于 sinθ 而不是 tanθ，而墙面又被极度透视压缩（C2 的注释原文：
    #      "机位必须选 45°，70° 时左右机位的水平分量会同号"）。实测
    #      80° 的互相关给出 **+0 px**（纹理周期 + 极度压缩 → 相关峰退化成
    #      一堆等高的假峰）。所以 20°/80° 这对机位本来就不适合量位移。
    #
    #   那 2× 还能抓住什么？抓的就是这条断言原本要抓的那件事：**offset 是不是
    #   随视角变**。若把 POM 退化成"一个固定的 UV 偏移"，两个角度的差异会
    #   只剩相机本身带来的投影差别（实测那种情况下倍数 ≈ 1.0），2× 照样红。
    d_front = ds(img_on_front, img_bl_front)[2]
    d_graze = ds(img_on, img_base)[2]
    ratio = d_graze / max(d_front, 1e-6)
    print("  C1 20° 入射差异=%.4f / 80° 入射差异=%.4f，倍数=%.2f×（下限 %.1f×）"
          % (d_front, d_graze, ratio, RATIO_MIN))
    if not (d_graze > POM_SIGNAL_MIN and ratio >= RATIO_MIN):
        print("     ✗ 掠射的差异没明显大于正视 —— offset 不随入射角变化"
              "（固定 UV 偏移的世界里倍数≈1.0）")
        failures.append("C1: 差异不随入射角增大")
    else:
        print("     ✓ 越掠射差异越大（offset 随视角变化，不是固定 UV 偏移）")

    h = clip(img_on_l).shape[0]
    y0, y1 = int(h * 0.35), int(h * 0.65)
    sh_l = best_hshift(clip(img_on_l), clip(img_bl_l), y0, y1)
    sh_r = best_hshift(clip(img_on_r), clip(img_bl_r), y0, y1)
    dl = ds(img_on_l, img_bl_l)[2]
    dr = ds(img_on_r, img_bl_r)[2]
    print("  C2 45° 左机位差异=%.4f 位移=%+d px；右机位差异=%.4f 位移=%+d px"
          % (dl, sh_l, dr, sh_r))
    if not (abs(sh_l) >= SHIFT_MIN and abs(sh_r) >= SHIFT_MIN):
        print("     ✗ 位移测不出来（幅度 < %.1f px）" % SHIFT_MIN)
        failures.append("C2: 位移幅度不足")
    elif sh_l * sh_r >= 0:
        print("     ✗ 左右镜像机位的位移**同号** —— offset 的方向写反了")
        failures.append("C2: 视差方向反了")
    else:
        print("     ✓ 左右镜像机位位移反号（方向正确，不是静位移）")

    # ---------------------------------------------------------- D 组
    print("\n[D] 远距离渐出（LOD）")
    # D1/D2 必须用**正视机位**（CAM_FRONT，法向距离 9.4 m）：掠射机位在
    # y=-1.7，正前方 1.7 m 就撞墙，墙的最近点远小于淡出终点 8 m，
    # 全屏根本淡不掉（实测差异 12.83，是场景选错、不是淡出没接）。
    nD1, _, mD1, _ = ds(img_fade_near, img_bl_front)
    print("  D1 正视 20°、淡出区间 2..8 m（相机 9.4 m）vs 同机位基线："
          ">18=%d 平均=%.6f" % (nD1, mD1))
    if nD1 != 0 or mD1 > PIX_IDENTICAL:
        print("     ✗ 淡出到 0 后仍改画面 —— 淡出没接进 offset")
        failures.append("D1: 淡出为 0 时没退回基线")
    else:
        print("     ✓ 完全淡出 = 逐像素退回普通法线贴图")

    nD2, _, mD2, _ = ds(img_on_front, img_bl_front)
    if mD2 < POM_SIGNAL_MIN:
        print("     ✗ 对照组（同机位、淡出区间放远）没有差异 → D1 是假阳性")
        failures.append("D2: 同机位放远淡出区间仍无差异")
    else:
        print("  D2 同机位淡出区间放远 → 差异 %.4f（对照，证明 D1 不是假阳性）"
              % mD2)

    nD3, _, mD3, _ = ds(img_far_on, img_far_bl)
    nFar, _, mFar, _ = ds(img_far_bl, img_bl_l)
    print("  D3 相机 70 m、淡出区间 20..60 → 差异：>18=%d 平均=%.6f"
          "（该距离下远/近基线差异 %.2f，说明墙确实还在画面里）"
          % (nD3, mD3, mFar))
    if nD3 != 0 or mD3 > PIX_IDENTICAL:
        print("     ✗ 超远距离仍改画面 —— 距离淡出失效")
        failures.append("D3: 远距离没淡出")
    elif mFar < 1.0:
        print("     ✗ 远处基线几乎与近处相同 —— 相机没真的拉远，D3 无意义")
        failures.append("D3: 远机位无效")
    else:
        print("     ✓ 超过淡出终点后逐像素退回法线贴图")

    # ---------------------------------------------------------- E 组
    print("\n[E] 提交量 & 验证层")
    print("  E1 drawCalls：基线=%s / 打开=%s" % (calls_base, calls_on))
    if calls_base is None or calls_on is None or calls_base != calls_on:
        print("     ✗ drawCalls 变了 —— POM 不该改提交量")
        failures.append("E1: drawCalls 变化")
    else:
        print("     ✓ 提交量不变（POM 只改采样坐标）")

    # ---------------------------------------------------------- 可视化
    try:
        a, b = clip(img_base), clip(img_on)
        dv = (b - a) * 4.0 + 128.0
        panels = [
            (a, "baseline (no height, 80 deg)"),
            (b, "POM on (80 deg)"),
            (np.clip(dv, 0, 255), "delta x4"),
            (clip(img_on_l), "flat H, 45 deg LEFT"),
            (clip(img_on_r), "flat H, 45 deg RIGHT"),
        ]
        imgs = [Image.fromarray(np.clip(p, 0, 255).astype(np.uint8))
                for p, _ in panels]
        k, lab = 1, 16
        cw = sum(p.width * k for p in imgs) + lab * (len(imgs) - 1)
        ch = max(p.height * k for p in imgs) + lab
        canvas = Image.new("RGB", (cw, ch), (26, 26, 30))
        drw, cx = ImageDraw.Draw(canvas), 0
        for p, (_, tag) in zip(imgs, panels):
            canvas.paste(p.resize((p.width * k, p.height * k), Image.NEAREST),
                         (cx, lab))
            drw.text((cx + 4, 3), tag, fill=(238, 238, 238))
            cx += p.width * k + lab
        cmp_path = os.path.join(OUT_DIR, "pom_compare.png")
        canvas.save(cmp_path)
        print("\n[可视化] 对比图: %s" % cmp_path)
    except Exception as ex:
        print("\n[可视化] 跳过（%s: %s）" % (type(ex).__name__, ex))

    if not args.keep_scenes:
        for p in (s_base, s_zero, s_pom):
            try:
                safe_remove(p)
            except OSError:
                pass

    print("\n" + "=" * 68)
    if failures:
        print("结果: FAIL（%d 项）" % len(failures))
        for f in failures:
            print("  · " + f)
        return 1
    print("结果: PASS（A 组 4 项 + B 组 2 项 + C 组 2 项 + "
          "D 组 3 项 + E 组 1 项，全部通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
