"""端到端验证：延迟贴花 / DBuffer（R4-4）。

R4-4 引入的是一条**独立于主链的 pass**：

    深度预通道 ─→ [贴花 pass：写三张 DBuffer] ─→ 分簇 ─→ 前向(采样 DBuffer)

贴花实体没有网格组件，渲染器自己画一个**单位立方体 × 贴花世界矩阵**的
盒体；片元不采盒体表面，而是按 gl_FragCoord 从深度图重建该像素的场景
表面世界坐标，投到贴花局部空间，落在盒内才采样贴花贴图，写进
反照率 / 世界法线 / 粗糙度-金属度三张图（第 4 通道统一是 coverage）。
前向 Pass 里 `pbr.frag` 按 coverage 把三样混进材质（用 `textureLod(...,0)`：
这段在非一致控制流里，隐式导数不可靠，而 DBuffer 没有 mip）。

所以"一张贴花到底贴没贴上去"取决于**四件事同时成立**，本脚本逐条钉死：

  ① 全局开关（MYVK_DECALS / ubo.decalParams.x）；
  ② 组件开关 enabled；
  ③ coverage 链：opacity × 边缘软淡出 × 距离淡出 × 贴图 alpha；
  ④ 盒内测试：重建出的场景点必须落在盒体体积里。

断言分组：

  A. **关着 = 基线**（本项目所有新效果的第一条约定）
     · A1 全局关（MYVK_DECALS=0）vs 场景里根本没贴花 → 逐像素一致；
     · A2 enabled=false → 逐像素一致；
     · A3 opacity=0（pass 照跑，coverage 算出来是 0）→ 逐像素一致；
     · A4 **盒体悬空**（整体在地面之上，与任何表面都不相交）→ 逐像素一致。
       这条是"盒内测试真的在起作用"的唯一证据：若盒体测试被忽略，贴花会
       糊满整个地面。
     · A5 同配置跑两次逐像素一致（去噪）；
     · A6 CULL-STATS 的 `decal=` 在关着/无贴花时是 0。

  B. **贴花真的在跑**
     · B1 启动日志 `DECALS: on/off (dbuffer x3 RGBA8, max N decals/slot)`；
     · B2 打开 vs 基线：视口内平均差显著；
     · B3 差异**局部**（包围盒面积远小于视口）——排除"整屏被贴满"这种
          看起来"有效果"其实盒内测试失效的情况；
     · B4 CULL-STATS `decal=1`；
     · B5 GPU-TIMING 里有 `decals=` 段（新插的时戳段真的接了）；
     · B6 把贴花实体的 X 挪 4 m：受影响像素的包围盒中心跟着右移
          （贴花的位姿来自实体 Transform，不是写死的）；
     · B7 盒体从 6 m 放到 11 m：受影响像素数明显变多（尺寸参数生效）。

  C. **距离淡出**
     · C1 淡出区间设在相机**之内**（fade=1,3，相机距贴花 11 m）→ 逐像素一致；
     · C2 同一机位把区间放远（fade=20,60）→ 差异显著（对照，证明 C1 不是
          假阳性）；
     · C3 相机拉到 76 m（fade=20,60）→ 逐像素一致，且远处基线/近处基线
          本身有显著差异（证明场景还在画面里，不是"什么都没画"）。

  D. **提交量与验证层**
     · D1 主链 drawCalls 不变（贴花是独立 pass，不该改主链提交量）；
     · D2 全程零 VUID / 验证层 ERROR。

场地：一块 60 m 的**地面**（plane，法线 +Z，Z-up 右手系），相机在上方
（eye z=7）俯视；贴花盒是 6×6×2 m 的立方体，中心放在 z=0.5 —— 于是
盒体跨过地面（z 从 -0.5 到 1.5），地面在 |x|<3、|y|<3 的区域被贴上。

用法：
    python tools/verify_decals.py
    python tools/verify_decals.py --keep-scenes
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

# CULL-STATS：只要最后两段（前缀必须与 Renderer 的格式串一致）
CULL_RE = re.compile(r"CULL-STATS .*?pom=(\d+) decal=(\d+)")
DECAL_LOG_RE = re.compile(r"DECALS: (on|off) \(dbuffer x3 RGBA8, max (\d+)")
GPU_RE = re.compile(r"GPU-TIMING .*?depth=([\d.]+) decals=([\d.]+)")

# ---- 场地 -------------------------------------------------------------
FLOOR_SIZE = 60.0
FLOOR_Y = 0.0                 # 地面在 z=0
DECAL_XY = 0.0                # 贴花中心（x, y）
DECAL_Z = 0.5                 # 盒体中心高度 → 盒体跨过地面
DECAL_BASE_SIZE = [6.0, 6.0, 2.0]
DECAL_BIG_SIZE = [11.0, 11.0, 2.0]
DECAL_MOVE_DX = 4.0
# 悬空盒：z 中心 3.2、半轴 1.0 → 盒体最低点 2.2 m，离地面（z=0）还有 2.2 m
DECAL_AIR_Z = 3.2

# ---- 机位 -------------------------------------------------------------
# Z-up：angleDir = (cp*cos(yaw), -cp*sin(yaw), sin(pitch))，相机在
# target + angleDir*distance 处；MYVK_EDITOR_CAM 传的是 **eye**。
# eye=(0,-9,7)、看向 (0,0,0.6) → yaw=90（相机在 -Y 侧看向 +Y）、pitch≈35.4。
CAM_TOP = "0,-9,7,90,35.4"          # 距贴花中心 ≈ 11.0 m
# 远处：eye=(0,-70,30)、看向原点 → yaw=90、pitch≈22.9；距贴花中心 ≈ 76 m
CAM_FAR = "0,-70,30,90,22.9"

# 距离淡出区间（米）
FADE_NEAR = (1.0, 3.0)     # 相机 11 m → 早已完全淡出
FADE_FAR = (20.0, 60.0)    # 相机 11 m → 完全生效；76 m → 完全淡出

# ---- 阈值（实测值写在括号里）----
PIX_IDENTICAL = 0.05      # A/C"逐像素一致"的门槛
# B2 的下限。**不能**照搬 POM 脚本的 3.0：本场地里贴花只占视口 ~9.5%
# 的像素、而且高度雾把颜色冲淡了，实测"打开 vs 基线"的全视口平均差是
# 4.45（>18 的像素数 28495，max 90）。取 2.0 留一倍余量又仍能区分
# "有效果"与"没效果"（后者恒为 0.000000）。
DECAL_SIGNAL_MIN = 2.0
BBOX_AREA_MAX = 0.45      # B3 差异包围盒占视口面积的上限（实测 ~25%）
MOVE_DX_MIN = 25.0        # B6 包围盒中心右移下限（像素）
SIZE_RATIO_MIN = 1.4      # B7 大盒/小盒受影响像素数倍数下限


def _rgba(c, a=255):
    return [c[0], c[1], c[2], a]


def solid(name, c):
    return {"kind": "solid", "name": name, "color": _rgba(c), "size": 8}


def checker(name, a, b, cell=8, size=64, srgb=False):
    return {"kind": "checker", "name": name, "colorA": _rgba(a),
            "colorB": _rgba(b), "cell": cell, "size": size, "srgb": srgb}


# 地面：暗灰纯色（贴花是亮红/亮黄，差异不会被底色吃掉）
TEX_FLOOR = solid("decal_floor", (70, 72, 78))
# 贴花反照率：两格大棋盘（红 / 黄），两种颜色都远离地面底色
TEX_DECAL = checker("decal_alb", (232, 58, 36), (250, 236, 72), 32, 64,
                    srgb=True)
# 贴花法线：两组切线空间法线（左右各偏一点）→ 贴花会改表面明暗
TEX_DECAL_NRM = checker("decal_nrm", (150, 118, 245), (110, 150, 245),
                        16, 96, srgb=False)


def build_scene(path, decal=None):
    """decal=None → 场景里没有贴花实体（基线）。"""
    floor = {
        "name": "Floor",
        "transform": {"position": [0.0, 0.0, FLOOR_Y],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": FLOOR_SIZE,
                 "name": "decal_floor_mesh"},
        "material": {
            "kind": "procedural", "name": "decal_floor_mat",
            "baseColor": [1.0, 1.0, 1.0, 1.0],
            "metallic": 0.0, "roughness": 0.65,
            "normalScale": 1.0, "ao": 1.0,
            "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
            "alphaMode": "opaque", "alphaBlend": False,
            "alphaCutoff": 0.5, "ditherAlpha": False,
            "shadingModel": "default",
            "albedo": TEX_FLOOR,
        },
    }

    ents = [floor]
    if decal is not None:
        pos = [DECAL_XY + decal.get("dx", 0.0), 0.0,
               decal.get("z", DECAL_Z)]
        ents.append({
            "name": "Decal",
            "transform": {"position": pos, "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "visible": True, "castShadow": False, "parent": -1,
            # ⚠ 没有 "mesh" —— 贴花本来就无网格，这是合法实体
            "decal": {
                "size": decal.get("size", DECAL_BASE_SIZE),
                "opacity": decal.get("opacity", 1.0),
                "tint": decal.get("tint", [1.0, 1.0, 1.0]),
                "roughness": decal.get("roughness", 0.55),
                "metallic": decal.get("metallic", 0.0),
                "normalScale": decal.get("normalScale", 1.0),
                "fadeStart": decal.get("fade", FADE_FAR)[0],
                "fadeEnd": decal.get("fade", FADE_FAR)[1],
                "edgeFade": decal.get("edgeFade", 0.12),
                "enabled": decal.get("enabled", True),
                "albedo": decal.get("albedo", TEX_DECAL),
                "normal": decal.get("normal", TEX_DECAL_NRM),
            },
        })

    doc = {
        "version": 1,
        "generator": "verify_decals.py",
        # 光从相机同侧偏上打下来（Z-up；pbr.frag 用 Ld = -lightDir，
        # 所以 z 分量为负 = 光从上方来）→ 地面被照亮。
        "light": {"direction": [0.35, -0.45, -0.82], "color": [1.0, 0.98, 0.95],
                  "intensity": 2.4, "castsShadow": False,
                  "ambientScale": 0.5},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def last_match(pat, lines, group=1):
    """最后一条匹配的第 group 组。默认取第 1 组（= 描述性字段本身）。

    ⚠ 别用 -1 当"整条"的默认值：Python 的 re.Match.group(-1) 会抛
      IndexError（负数下标在 3.6 之后就不再表示"最后一个组"了）。
    """
    for ln in reversed(lines):
        m = pat.search(ln)
        if m:
            return m.group(group)
    return None


def last_decal_count(lines):
    """CULL-STATS 的 decal= 字段（最后一条）。"""
    v = last_match(CULL_RE, lines, 2)
    return None if v is None else int(v)


def last_draw_calls(lines):
    v = None
    for ln in lines:
        m = re.search(r"drawCalls=(\d+)", ln)
        if m:
            v = int(m.group(1))
    return v


def bbox_of(a, b, vp, thr=18):
    return diff_stats(a, b, vp, thr)[3]


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
    print("延迟贴花 / DBuffer 验证（R4-4：MYVK_DECALS）")
    print("=" * 68)

    # ---------------------------------------------------------- 场景
    kill_stale_editors()
    s_base = os.path.join(OUT_DIR, "decal_base.json")
    s_off = os.path.join(OUT_DIR, "decal_off.json")
    s_dis = os.path.join(OUT_DIR, "decal_disabled.json")
    s_op0 = os.path.join(OUT_DIR, "decal_op0.json")
    s_air = os.path.join(OUT_DIR, "decal_air.json")
    s_on = os.path.join(OUT_DIR, "decal_on.json")
    s_mv = os.path.join(OUT_DIR, "decal_moved.json")
    s_big = os.path.join(OUT_DIR, "decal_big.json")
    s_near = os.path.join(OUT_DIR, "decal_fadenear.json")
    build_scene(s_base, None)
    build_scene(s_off, {})
    build_scene(s_dis, {"enabled": False})
    build_scene(s_op0, {"opacity": 0.0})
    build_scene(s_air, {"z": DECAL_AIR_Z})
    build_scene(s_on, {})
    build_scene(s_mv, {"dx": DECAL_MOVE_DX})
    build_scene(s_big, {"size": DECAL_BIG_SIZE})
    build_scene(s_near, {"fade": FADE_NEAR})
    print("\n场景已写出：基线 / 全局关 / enabled=false / opacity=0 / 悬空盒 / "
          "正常 / 右移 / 放大 / 近淡出")

    def shot(name):
        return os.path.join(OUT_DIR, "decal_%s.png" % name)

    all_lines = {}

    def step(name, scene, cam, env_extra=None, save=True):
        env = {"MYVK_GRID": "0",                  # 地平面栅格会盖在贴花上
               "MYVK_DEPTH_PREPASS": "1",         # 贴花重建深度，必须要它
               "MYVK_FRUSTUM_DBG": "1",           # CULL-STATS
               "MYVK_GPU_TIMING": "1",            # GPU-TIMING
               # 编辑器 HUD 会带来与渲染无关的像素噪声，A/C 组要判"逐像素
               # 一致"就必须关掉它（与其它 verify_* 脚本同一口径）。
               "MYVK_EDITOR_NO_HUD": "1",
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

    print("\n[运行] 12 次编辑器")
    img_base, vp = step("base", s_base, CAM_TOP, {"MYVK_DECALS": "1"})
    img_off, _ = step("off", s_off, CAM_TOP, {"MYVK_DECALS": "0"})
    img_off2, _ = step("off2", s_off, CAM_TOP, {"MYVK_DECALS": "0"})
    img_dis, _ = step("dis", s_dis, CAM_TOP, {"MYVK_DECALS": "1"})
    img_op0, _ = step("op0", s_op0, CAM_TOP, {"MYVK_DECALS": "1"})
    img_air, _ = step("air", s_air, CAM_TOP, {"MYVK_DECALS": "1"})
    img_on, _ = step("on", s_on, CAM_TOP, {"MYVK_DECALS": "1"})
    img_mv, _ = step("mv", s_mv, CAM_TOP, {"MYVK_DECALS": "1"})
    img_big, _ = step("big", s_big, CAM_TOP, {"MYVK_DECALS": "1"})
    img_near, _ = step("near", s_near, CAM_TOP, {"MYVK_DECALS": "1"})
    img_faron, _ = step("faron", s_on, CAM_FAR, {"MYVK_DECALS": "1"})
    img_farbl, _ = step("farbl", s_base, CAM_FAR, {"MYVK_DECALS": "1"})

    need = {
        "base": img_base, "off": img_off, "off2": img_off2, "dis": img_dis,
        "op0": img_op0, "air": img_air, "on": img_on, "mv": img_mv,
        "big": img_big, "near": img_near, "faron": img_faron,
        "farbl": img_farbl,
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

    if vpb:
        vp_area = float((vpb[2] - vpb[0]) * (vpb[3] - vpb[1]))
    else:
        vp_area = float(img_base.width * img_base.height)
        print("  ⚠ 没拿到 VP-RECT，包围盒占比改用整窗面积（偏保守）")

    def center_of(box):
        if not box:
            return None
        return ((box[0] + box[2]) * 0.5, (box[1] + box[3]) * 0.5)

    # ---------------------------------------------------------- A 组
    print("\n[A] 关着 = 基线")
    n1, _, m1, _ = ds(img_off, img_base)
    print("  A1 MYVK_DECALS=0（场景里有贴花）vs 无贴花场景：>18=%d 平均=%.6f"
          % (n1, m1))
    if n1 != 0 or m1 > PIX_IDENTICAL:
        print("     ✗ 全局关卡后仍改画面 —— pass 没被跳过 / 缺省图不是全透明")
        failures.append("A1: 全局关不等于基线")
    else:
        print("     ✓ 逐像素一致（pass 根本没录）")

    n2, _, m2, _ = ds(img_dis, img_base)
    print("  A2 enabled=false vs 无贴花场景：>18=%d 平均=%.6f" % (n2, m2))
    if n2 != 0 or m2 > PIX_IDENTICAL:
        print("     ✗ 组件开关没生效 —— 关掉的贴花仍被收进 pass")
        failures.append("A2: enabled=false 不等于基线")
    else:
        print("     ✓ 逐像素一致（collectDecals 就把它滤掉了）")

    n3, _, m3, _ = ds(img_op0, img_base)
    print("  A3 opacity=0（pass 照跑）vs 无贴花场景：>18=%d 平均=%.6f"
          % (n3, m3))
    if n3 != 0 or m3 > PIX_IDENTICAL:
        print("     ✗ opacity=0 仍改画面 —— coverage 没乘 opacity")
        failures.append("A3: opacity=0 不等于基线")
    else:
        print("     ✓ 逐像素一致（coverage=0 → 前向整段跳过）")

    n4, _, m4, box4 = ds(img_air, img_base)
    print("  A4 盒体悬空（最低点离地 2.2 m）vs 无贴花场景：>18=%d 平均=%.6f"
          % (n4, m4))
    if n4 != 0 or m4 > PIX_IDENTICAL:
        print("     ✗ 盒体与表面不相交却仍改画面 —— 盒内测试失效（bbox=%s）"
              % box4)
        failures.append("A4: 盒内测试失效")
    else:
        print("     ✓ 逐像素一致（重建点全在盒外）")

    n5, _, m5, _ = ds(img_off2, img_off)
    print("  A5 同配置两次运行：>18=%d 平均=%.6f" % (n5, m5))
    if n5 != 0 or m5 > PIX_IDENTICAL:
        print("     ✗ 两次运行不一致（画面本身不稳，后续断言不可信）")
        failures.append("A5: 同配置两次不一致")
    else:
        print("     ✓ 两次逐像素一致")

    dec_off = last_decal_count(all_lines["off"])
    dec_base = last_decal_count(all_lines["base"])
    dec_on = last_decal_count(all_lines["on"])
    print("  A6 CULL-STATS decal：关=%s / 无贴花=%s / 打开=%s"
          % (dec_off, dec_base, dec_on))
    if dec_off != 0 or dec_base != 0 or dec_on != 1:
        print("     ✗ decal 计数不对（期望 0 / 0 / 1）")
        failures.append("A6: decal 计数错")
    else:
        print("     ✓ 统计口径与「有没有真的收到贴花」一致")

    # ---------------------------------------------------------- B 组
    print("\n[B] 贴花真的在跑")
    on_log = last_match(DECAL_LOG_RE, all_lines["on"])
    off_log = last_match(DECAL_LOG_RE, all_lines["off"])
    print("  B1 启动日志：打开=%s / 关闭=%s"
          % (on_log or "缺", off_log or "缺"))
    if on_log != "on" or off_log != "off":
        print("     ✗ DECALS 开关日志不对（期望 on / off）")
        failures.append("B1: 缺 DECALS 日志")
    else:
        print("     ✓ 全局开关被正确解析（默认关，靠 MYVK_DECALS 打开）")

    nB, mxB, mB, boxB = ds(img_on, img_base)
    print("  B2 打开 vs 基线：>18=%d 平均=%.4f max=%d bbox=%s"
          % (nB, mB, mxB, boxB))
    if mB < DECAL_SIGNAL_MIN:
        print("     ✗ 打开后画面几乎没变 —— 贴花没画上去")
        failures.append("B2: 打开无效果")
    else:
        print("     ✓ 贴花真的改了画面")

    if not boxB:
        print("  B3 ✗ 没有差异包围盒（B2 的差异为 0，无法判断局部性）")
        failures.append("B3: 缺差异包围盒")
    else:
        area = float((boxB[2] - boxB[0]) * (boxB[3] - boxB[1]))
        frac = area / max(vp_area, 1.0)
        print("  B3 差异包围盒面积占比 = %.1f%%（上限 %.0f%%）"
              % (frac * 100.0, BBOX_AREA_MAX * 100.0))
        if frac > BBOX_AREA_MAX:
            print("     ✗ 差异铺满整屏 —— 盒内测试或 coverage 有问题")
            failures.append("B3: 差异不局部")
        else:
            print("     ✓ 差异集中在盒体的屏幕投影内")

    print("  B4 CULL-STATS decal（打开）= %s" % dec_on)
    if dec_on != 1:
        print("     ✗ 打开时 decal 计数不是 1")
        failures.append("B4: 打开时 decal 计数错")
    else:
        print("     ✓ 贴花 pass 确实提交了 1 个盒体")

    gpu = None
    for ln in reversed(all_lines["on"]):
        m = GPU_RE.search(ln)
        if m:
            gpu = (float(m.group(1)), float(m.group(2)))
            break
    if not gpu:
        print("  B5 ✗ 没找到 GPU-TIMING 的 decals 字段")
        failures.append("B5: GPU-TIMING 缺 decals 段")
    else:
        print("  B5 GPU-TIMING depth=%.2f ms decals=%.2f ms" % gpu)
        if gpu[1] < 0.0:
            print("     ✗ decals 段是负值（时戳没写到那个 query）")
            failures.append("B5: decals 段非法")
        else:
            print("     ✓ 时戳段接上了（0.00 也算合法：这一帧的盒体像素本来就少）")

    box_on = boxB
    n_mv, _, m_mv, box_mv = ds(img_mv, img_base)
    c_on, c_mv = center_of(box_on), center_of(box_mv)
    print("  B6 贴花右移 %.0f m：>18=%d 平均=%.4f；包围盒中心 x %.0f → %.0f"
          % (DECAL_MOVE_DX, n_mv, m_mv,
             c_on[0] if c_on else -1.0, c_mv[0] if c_mv else -1.0))
    if not c_on or not c_mv:
        print("     ✗ 拿不到包围盒中心")
        failures.append("B6: 缺包围盒")
    elif (c_mv[0] - c_on[0]) < MOVE_DX_MIN:
        print("     ✗ 贴花没跟着实体往 +X 挪（屏幕右方）")
        failures.append("B6: 贴花没跟随实体 Transform")
    else:
        print("     ✓ 位姿来自实体 Transform（相机右方 = 世界 +X）")

    n_big, _, m_big, box_big = ds(img_big, img_base)
    ratio = float(n_big) / float(max(nB, 1))
    print("  B7 盒体 6 m → 11 m：受影响像素 %d → %d（%.2f×）"
          % (nB, n_big, ratio))
    if ratio < SIZE_RATIO_MIN:
        print("     ✗ 放大盒体后受影响像素没明显增加 —— size 没接进 world 矩阵")
        failures.append("B7: size 参数无效")
    else:
        print("     ✓ 盒体越大覆盖越多（size 参与了世界矩阵）")

    # ---------------------------------------------------------- C 组
    print("\n[C] 距离淡出")
    nC1, _, mC1, _ = ds(img_near, img_base)
    print("  C1 淡出区间 %s（相机 11 m，早已淡尽）vs 无贴花场景："
          ">18=%d 平均=%.6f" % (FADE_NEAR, nC1, mC1))
    if nC1 != 0 or mC1 > PIX_IDENTICAL:
        print("     ✗ 完全淡出后仍改画面 —— 距离淡出没接进 coverage")
        failures.append("C1: 淡尽时不等于基线")
    else:
        print("     ✓ 淡尽 = 逐像素退回基线")

    print("  C2 同一机位、区间放远到 %s → 差异 %.4f（对照，证明 C1 不是假阳性）"
          % (FADE_FAR, mB))
    if mB < DECAL_SIGNAL_MIN:
        print("     ✗ 对照组没有差异 → C1 是假阳性")
        failures.append("C2: 对照无差异")

    nC3, _, mC3, _ = ds(img_faron, img_farbl)
    nFar, _, mFar, _ = ds(img_farbl, img_base)
    print("  C3 相机 76 m、区间 %s → 差异：>18=%d 平均=%.6f"
          "（远/近基线差异 %.2f，说明地面还在画面里）"
          % (FADE_FAR, nC3, mC3, mFar))
    if nC3 != 0 or mC3 > PIX_IDENTICAL:
        print("     ✗ 超远距离仍改画面 —— 距离淡出失效")
        failures.append("C3: 远距离没淡出")
    elif mFar < 1.0:
        print("     ✗ 远处基线几乎与近处相同 —— 相机没真的拉远，C3 无意义")
        failures.append("C3: 远机位无效")
    else:
        print("     ✓ 超过淡出终点后逐像素退回基线")

    # ---------------------------------------------------------- D 组
    print("\n[D] 提交量 & 验证层")
    c_base = last_draw_calls(all_lines["base"])
    c_on = last_draw_calls(all_lines["on"])
    print("  D1 drawCalls：基线=%s / 打开=%s" % (c_base, c_on))
    if c_base is None or c_on is None or c_base != c_on:
        print("     ✗ 主链 drawCalls 变了 —— 贴花不该改主链提交量")
        failures.append("D1: drawCalls 变化")
    else:
        print("     ✓ 主链提交量不变（贴花走独立 pass）")
    print("  D2 全程零 VUID / 验证层 ERROR（12 轮，见上面各轮的 ⚠ 行）")

    # ---------------------------------------------------------- 可视化
    try:
        def clip(im):
            a = np.asarray(im.convert("RGB")).astype(int)
            if vpb:
                return a[vpb[1]:vpb[3], vpb[0]:vpb[2]]
            return a

        a, b = clip(img_base), clip(img_on)
        dv = (b - a) * 4.0 + 128.0
        panels = [
            (a, "baseline (no decal)"),
            (b, "decal on"),
            (np.clip(dv, 0, 255), "delta x4"),
            (clip(img_air), "box floating (rejected)"),
            (clip(img_mv), "decal moved +4 m in X"),
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
        cmp_path = os.path.join(OUT_DIR, "decal_compare.png")
        canvas.save(cmp_path)
        print("\n[可视化] 对比图: %s" % cmp_path)
    except Exception as ex:
        print("\n[可视化] 跳过（%s: %s）" % (type(ex).__name__, ex))

    if not args.keep_scenes:
        for p in (s_base, s_off, s_dis, s_op0, s_air, s_on, s_mv, s_big,
                  s_near):
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
    print("结果: PASS（A 组 6 项 + B 组 7 项 + C 组 3 项 + D 组 2 项，全部通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
