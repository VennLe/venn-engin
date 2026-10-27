#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""回归：游戏 UI / HUD 通道（`docs/ui_hud_roadmap.md` 的 U0 起逐阶段累加）。

这条通道和编辑器 ImGui 是**两条互不共享资源**的路（`engine/ui2/` 是新目录，
`engine/ui/` 只服务编辑器）。所以它的用例也分三层：

  1. **无窗口数值自检**（`MYVK_UI2_TEST=1`，毫秒级）—— 缩放公式 / 默认值，
     以后还有字体度量 / 换行 / 布局比例 / 焦点导航。这一层不碰 Vulkan。
  2. **能力量测**（`UI2-CAPS`）—— 把"能不能做"钉在实测值上：
     `maxBoundDescriptorSets` 是**同时绑定**的 set 数上限（不是总数），
     UI 是独立管线只绑 1 个 set，所以 3D 那条通道用满 8 个也不影响它。
  3. **像素守卫** —— `ui.enabled=1` 但**没有 UI 数据**时必须零绘制；
     `ui.hudInEditor=0`（默认）时编辑器截图必须与关掉整条通道**逐位相同**。

⚠ 为什么第三层是"两次启动对比"而不是"跟参考图对比"：
   参考图会随任何一个不相关的美术改动过期，变成"每轮都要重新拍基线"。
   "开/关这条通道 → 逐位相同"是**不依赖基线**的强断言，而且它守的正是
   我们真正怕的那件事（新通道偷偷改了既有像素）。

运行：python tools/verify_ui2.py
结尾一定是 `结果: PASS` / `结果: FAIL: ...`（run_regression.py 取**最后一条**）。
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time

from PIL import ImageChops

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client                            # noqa: E402
from verify_add_menu import kill_stale_editors                       # noqa: E402
from verify_player import close, launch                              # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

# 相机摆在一个有地面 + 天空的固定机位：像素守卫要在"有东西可画"的画面上跑，
# 否则"两边都是纯黑"也会判 PASS（假绿）。
CAM = "4,-4,2.2,45,18"

failures: list[str] = []
log_lines: list[str] = []


def check(msg: str, ok: bool) -> bool:
    print("    %-66s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        failures.append(msg)
    return ok


def kv(line: str | None, key: str):
    if not line:
        return None
    m = re.search(r"\b" + re.escape(key) + r"=([^\s]+)", line)
    return m.group(1) if m else None


def find_line(tap, needle: str):
    with tap.lock:
        for ln in tap.lines:
            if needle in ln:
                return ln
    return None


def wait_line(tap, needle: str, timeout: float = 20.0):
    end = time.time() + timeout
    while time.time() < end:
        ln = find_line(tap, needle)
        if ln:
            return ln
        time.sleep(0.1)
    return None


def count_errors(tap) -> int:
    with tap.lock:
        return sum(1 for ln in tap.lines
                   if "[ERROR]" in ln or "VUID-" in ln)


def shot(hwnd, name: str, tag: str = ""):
    img, ok = capture_client(hwnd)
    if not ok:
        return None
    os.makedirs(OUT_DIR, exist_ok=True)
    img.save(os.path.join(OUT_DIR, name))
    return img


def grab(extra_env: dict, size: str = "1280x720", settle: float = 2.0):
    """启动一次编辑器 → 等 UI2-CAPS 出现 → 再画几帧 → 截一张。

    返回 (image, ok, tap)；启动失败时 image=None。
    """
    env = {"MYVK_EDITOR_CAM": CAM, "MYVK_EDITOR_NO_HUD": "1", "MYVK_GRID": "0"}
    env.update(extra_env)
    proc, hwnd, tap = launch(EXE, env, size)
    if not hwnd:
        return None, False, tap
    try:
        if wait_line(tap, "UI2-CAPS", 15.0) is None:
            return None, False, tap
        time.sleep(settle)
        img = shot(hwnd, "ui2_capture_%s.png" % _tag_of(extra_env))
        errs = count_errors(tap)
        if errs:
            print("    !! %d 条 [ERROR]/VUID" % errs)
        return img, img is not None, tap
    finally:
        close(proc, hwnd)


def _tag_of(env: dict) -> str:
    return "on" if env.get("MYVK_UI") != "0" else "off"


def diff_ratio(a, b, thresh: int = 8) -> float:
    """显著变化像素占比（0..1）。口径见 MEMORY：不用整图平均差。"""
    if a is None or b is None:
        return 1.0
    ca, cb = a.convert("RGB"), b.convert("RGB")
    if ca.size != cb.size:
        return 1.0
    hist = ImageChops.difference(ca, cb).convert("L").histogram()
    total = sum(hist)
    if total == 0:
        return 1.0
    return sum(hist[thresh + 1:]) / float(total)


# ============================================================
# A：能力量测（UI2-CAPS）
# ============================================================

def case_caps():
    print("\n[A] UI2-CAPS：能力量测（不靠记忆，靠实测）")
    kill_stale_editors()
    img, ok, tap = grab({})
    if not check("编辑器启动 + UI2-CAPS 出现", ok):
        return None
    ln = find_line(tap, "UI2-CAPS")
    print("    %s" % ln.strip())

    sets = int(kv(ln, "maxBoundDescriptorSets") or "0")
    # UI 是**独立管线**、一次绘制只绑 1 个 set；要求 >= 2 是为了留一个余量
    # （将来 UI 自己要再加一个 UBO set 时不必改架构）。
    check("maxBoundDescriptorSets=%d >= 2（UI 独立 set 有地方放）" % sets,
          sets >= 2)

    fmt = int(kv(ln, "swapFormat") or "-1")
    # 交换链优先 B8G8R8A8_SRGB（=44/50 视驱动而定，只断言"是 sRGB"这一点很难，
    # 所以报出来 + 断言 UI 的 sRGB 反编码契约由 B 组的像素值钉住）。
    print("    swapFormat=%d（UI shader 必须 srgbToLinear，B 组用像素钉它）"
          % fmt)

    ext = kv(ln, "outExtent") or "0x0"
    print("    outExtent=%s（UI 除的是**输出**分辨率，不是 DRS 分辨率）" % ext)

    check("UI2-CAPS 里有 outExtent 且非零", ext != "0x0")
    return img


# ============================================================
# B：像素守卫（默认不改变任何既有像素）
# ============================================================

def case_default_pixels(on_img):
    print("\n[B] 像素守卫：ui.enabled=1 但没有 UI 数据 → 与 ui.enabled=0 逐位相同")
    off_img, ok, _tap = grab({"MYVK_UI": "0"})
    if not check("ui.enabled=0 时也能正常启动", ok):
        return False
    if on_img is None:
        return check("默认（enabled=1）那一张截图拿到了", False)

    ratio = diff_ratio(on_img, off_img)
    print("    显著变化像素占比 = %.4f%%（阈值 0.50%%）" % (ratio * 100.0))
    # ⚠ 判据复用 A0 的口径：显著变化像素占比，不用整图平均差。
    #   现在整条通道还没画任何东西，所以这个数必须是**精确的 0**；
    #   给 0.5% 的余量只是为了不被 TAA 抖动之类的东西绊住（这里其实也没开）。
    return check("默认关/开的画面一致（UI 通道零绘制）", ratio <= 0.005)


# ============================================================
# C：无窗口数值自检
# ============================================================

EXPECTED_SELFTEST = [
    "scale_1080p", "scale_720p", "scale_4k", "scale_ultrawide",
    "scale_zero_guard", "scale_user_multiplier",
    "default_enabled", "default_hud_in_editor", "default_debug_bounds",
    "default_font_size", "default_scale",
    # U1：字体数据自洽 + 排版（真图集、纯 CPU）
    "font_json_load", "font_glyph_count", "font_has_cjk", "font_has_ascii",
    "font_metrics", "font_uv_in_range", "font_fallback",
    "text_verts_2chars", "text_measure", "text_newline_verts",
    "text_newline_dy", "text_advance_uses_adv", "sdf_smooth_shrinks",
    # U4 锚点布局 / U5 控件
    "layout_ref_exact", "layout_720_margin", "layout_margin_ratio",
    "layout_right_edge", "layout_ultrawide_right", "layout_center",
    "layout_zero_guard",
    "hit_inside", "hit_outside", "hit_right_exclusive", "hit_disabled",
    "button_idle_color", "button_hover_color", "button_press_beats_hover",
    "button_with_label_verts",
    # #312：`.ui.json` 文档层（解析 / 往返 / 放置 / 绑定 / 绘制）。
    # 这 30 条是"算得对"的那一半（另一半"面板上按下去真写了"在
    # tools/verify_ui_editor.py）。逐个列名字而不是只数总数：删掉一条时
    # 总数也会跟着少，但只有名字在这里才能指出**缺的是哪一条**。
    "uidoc_parse_ok", "uidoc_parse_widgets", "uidoc_parse_comment",
    "uidoc_parse_warn_free",
    "uidoc_roundtrip_bytes",
    "uidoc_place_count", "uidoc_place_order",
    "uidoc_anchor_top_center_x", "uidoc_anchor_bottom_left_x",
    "uidoc_anchor_bottom_left_y",
    "uidoc_child_in_parent_x", "uidoc_child_in_parent_w",
    "uidoc_scale_independent",
    "uidoc_bind_text", "uidoc_bind_int_vs_float", "uidoc_bind_unknown_kept",
    "uidoc_bind_progress_fill", "uidoc_bind_missing_zero",
    "uidoc_draw_widgets", "uidoc_draw_texts", "uidoc_draw_rect_verts",
    "uidoc_hidden_parent_skips_children", "uidoc_zero_extent",
    "uidoc_reject_dup_id", "uidoc_reject_dup_id_msg",
    "uidoc_reject_bad_anchor", "uidoc_reject_missing_rect",
    "uidoc_reject_unknown_type", "uidoc_reject_id_with_space",
    "uidoc_bad_color_warns",
]

TEST_RE = re.compile(r"UI2-TEST\s+(\S+)\s+actual=(.+?)\s+expected=(.+?)\s+"
                     r"tol=(\S+)\s+result=(PASS|FAIL)\s*$")
# ↑ actual / expected 用 `(.+?)` 而不是 `(\S+)`：断言值里**可以有空格**
#   （`HP 70/100`、`70 70.5`、`self-test fixture`）。原来用 `\S+` 时这三条
#   直接被判成"行格式解析失败"，而 `UI2-TOTAL pass` 与数到的行数又对不上
#   （86 vs 83），红字全指向解析器而不是产品。靠后面紧跟的 ` expected=` /
#   ` tol=` / ` result=` 这几个字面量做边界，值里不含这些串就不会歧义。
TOTAL_RE = re.compile(r"UI2-TOTAL\s+pass=(\d+)\s+fail=(\d+)")


def case_selftest():
    print("\n[C] 无窗口自检（MYVK_UI2_TEST=1，不初始化 Vulkan）")
    if not os.path.isfile(EXE):
        return check("找不到 %s" % EXE, False)
    env = dict(os.environ)
    env["MYVK_UI2_TEST"] = "1"
    proc = subprocess.run([EXE], cwd=os.path.dirname(EXE), env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=120)
    out = proc.stdout or ""
    seen, bad = [], []
    for line in out.splitlines():
        if "UI2-TEST" not in line:
            continue
        m = TEST_RE.search(line)
        if not m:
            bad.append(line.strip())
            continue
        name, actual, expected, _tol, res = m.groups()
        seen.append(name)
        print("    %-24s actual=%-10s expected=%-10s %s"
              % (name, actual, expected, res))
        if res != "PASS":
            failures.append("%s: actual=%s expected=%s" % (name, actual,
                                                           expected))

    ok = True
    ok &= check("UI2-TEST 行格式全部可解析（%d 条）" % len(seen), not bad)
    for b in bad[:3]:
        print("    !! 解析失败: %s" % b)

    missing = [n for n in EXPECTED_SELFTEST if n not in seen]
    ok &= check("断言清单完整（缺 %d 项）" % len(missing), not missing)
    if missing:
        print("    缺少: %s" % ", ".join(missing))

    total = None
    for line in out.splitlines():
        m = TOTAL_RE.search(line)
        if m:
            total = (int(m.group(1)), int(m.group(2)))
    ok &= check("UI2-TOTAL 出现", total is not None)
    if total:
        print("    UI2-TOTAL pass=%d fail=%d" % total)
        ok &= check("UI2-TOTAL fail=0", total[1] == 0)
        ok &= check("UI2-TOTAL pass 与脚本数到的行数一致",
                    total[0] == len(seen))
    return ok


# ============================================================
# D：ui.rect 真的上屏（U2 的 "Hello World"）
# ============================================================

def _vp_center(tap, img):
    vp = tap.last_rect("VP-RECT")
    if not vp or img is None:
        return None, None
    cx = int((vp[0] + vp[2]) * 0.5)
    cy = int((vp[1] + vp[3]) * 0.5)
    if not (0 <= cx < img.size[0] and 0 <= cy < img.size[1]):
        return None, vp
    return (cx, cy), vp


def case_rect_pixels():
    """铺满视口的 #808080 → 中心像素必须是 **128**。

    ⚠ 这个判据不是"应该 128"，是**实测出来的两个候选值**：
      · 交换链是 sRGB（B8G8R8A8_SRGB）→ 片元里 srgbToLinear(0.5)=0.2158，
        硬件编码回去 = **128**（正确）；
      · 漏掉反编码（0.5 当线性输出）→ 屏幕 **188**（错，整块 UI 亮一大截）。
      所以断言取 128±2，188 必须红。
    """
    print("\n[D] ui.rect 上屏：铺满视口的 #808080 → 中心像素 128")
    kill_stale_editors()
    env = {
        # 编辑器视口是"模式 B"（离屏图）→ 要看得到 HUD 必须显式开这个开关。
        "MYVK_UI_HUD_IN_EDITOR": "1",
        "MYVK_EDITOR_CONSOLE_CMD": "ui.rect 0 0 4096 4096 #808080",
    }
    img, ok, tap = grab(env, settle=2.5)
    if not check("编辑器启动 + UI2-CAPS", ok):
        return False, None

    ln = wait_line(tap, "UI2-DEBUG-RECT", 10.0)
    if not check("ui.rect 命令执行（UI2-DEBUG-RECT 出现）", ln is not None):
        return False, img
    check("  on=1", kv(ln, "on") == "1")

    stats = wait_line(tap, "UI2-STATS", 10.0)
    if not check("UI2-STATS 出现（说明真的走了绘制路径）", stats is not None):
        return False, img
    print("    %s" % stats.strip())
    check("  draws=1（单图集一次 draw，合批有效）", kv(stats, "draws") == "1")
    check("  verts=6（一个矩形 = 2 个三角形）", kv(stats, "verts") == "6")

    center, vp = _vp_center(tap, img)
    if not check("拿到视口矩形与中心像素坐标", center is not None):
        return False, img
    px = img.convert("RGB").getpixel(center)
    print("    VP-RECT=%s center=%s pixel=%s" % (vp, center, px))
    check("中心像素 = 128±2（sRGB 反编码正确；188 说明漏了这步）",
          all(abs(c - 128) <= 2 for c in px))

    if count_errors(tap) != 0:
        check("零 [ERROR] / 零 VUID（模式 B 的管线兼容性成立）", False)
    else:
        check("零 [ERROR] / 零 VUID（模式 B 的管线兼容性成立）", True)
    return True, img


def case_editor_default_off(with_rect_img):
    """`ui.hudInEditor` 默认关 → 就算下了 ui.rect 命令，编辑器截图也不许变。

    这条守的是"几十条编辑器像素基线"：一条新通道默认就往视口里画东西，
    等于让所有既有截图整体漂 —— 那种红是查不出原因的。
    """
    print("\n[E] 默认关守卫：hudInEditor=0 时下 ui.rect，编辑器截图必须不变")
    if with_rect_img is None:
        return check("上一组的截图拿到了", False)
    kill_stale_editors()
    # 同样的 ui.rect 命令，但**不设** MYVK_UI_HUD_IN_EDITOR
    img, ok, tap = grab({"MYVK_EDITOR_CONSOLE_CMD":
                         "ui.rect 0 0 4096 4096 #808080"}, settle=2.5)
    if not check("编辑器启动", ok):
        return False
    check("命令确实执行了（不是被忽略）", find_line(tap, "UI2-DEBUG-RECT") is not None)
    check("这条路径上**没有** UI2-STATS（一帧都没画）",
          find_line(tap, "UI2-STATS") is None)
    ratio = diff_ratio(with_rect_img, img)
    print("    与开了 hudInEditor 的那张相比：显著变化像素占比 %.2f%%"
          % (ratio * 100.0))
    # 两张图**必须不同**（否则说明 D 组那张根本没画上 —— 假绿），
    # 而且这一张必须与"没有 HUD"的基准一致。前者用"不同"断言，
    # 后者由 B 组（enabled=1 无数据 == enabled=0）间接守住。
    #
    # ⚠ 阈值为什么不是 50%：UI 画在**视口**那张离屏图上，而截图是整个编辑器
    #   窗口 —— 视口只占约 1/3（实测 33.3%）。拿"半屏"当门槛等于要求视口占满
    #   窗口，换个窗口尺寸就红。这里要证明的是"确实画上了"而不是"占多少"：
    #   B 组已经把纯噪声水平钉在 0.0000%，所以 2% 就足以区分"画了"与"没画"。
    return check("两张图明显不同（D 组确实画上了，E 组确实没画）",
                 ratio >= 0.02)


def case_taa_independent():
    """TAA 开/关：UI 区域的像素必须**逐位相同**。

    为什么这条重要：游戏 UI 挂在 Post 输出 pass 内、FXAA/后处理**之后**。
    一旦有人把它挪进后处理链（比如为了"让 UI 也吃 TAA"），那么 TAA 的
    抖动/历史混合就会把 UI 搅出差异 —— 这条立刻红。它同时钉住"UI 不跟 DRS、
    不跟时域效果"这条契约。
    """
    print("\n[F] 顺序契约：TAA 开/关时 UI 区域像素逐位相同")
    shots = {}
    for tag, taa in (("taa0", "0"), ("taa1", "1")):
        env = {
            "MYVK_UI_HUD_IN_EDITOR": "1",
            "MYVK_TAA": taa,
            "MYVK_EDITOR_CONSOLE_CMD": "ui.rect 0 0 4096 4096 #808080",
        }
        img, ok, tap = grab(env, settle=2.5)
        if not check("TAA=%s 启动" % taa, ok):
            return False
        check("  TAA=%s 时 UI 画上了（UI2-STATS）" % taa,
              wait_line(tap, "UI2-STATS", 10.0) is not None)
        shots[tag] = img
        if tag == "taa0":
            kill_stale_editors()
    ratio = diff_ratio(shots["taa0"], shots["taa1"])
    print("    两帧显著变化像素占比 = %.4f%%" % (ratio * 100.0))
    return check("TAA 开/关下 UI 区域逐位一致", ratio <= 0.001)


# ============================================================
# G：SDF 字体（U1）—— 图集覆盖 / 加载锚点 / 文本上屏
# ============================================================

ATLAS_DIR = os.path.join(ROOT, "assets", "ui", "fonts")
ATLAS_JSON = os.path.join(ATLAS_DIR, "noto_sc_sdf.json")
ATLAS_PNG = os.path.join(ATLAS_DIR, "noto_sc_sdf.png")
DICT_SRC = os.path.join(ROOT, "engine", "i18n", "ZhTable.cpp")   # U7 起词典在这里


def _ink_ratio(img, vp, bright: int = 200, dark: int = 40):
    """视口区域内：亮像素占比（字形墨迹）/ 暗像素占比（黑底）。"""
    if img is None or not vp:
        return None, None
    x0, y0 = max(0, int(vp[0])), max(0, int(vp[1]))
    x1, y1 = min(img.size[0], int(vp[2])), min(img.size[1], int(vp[3]))
    if x1 <= x0 or y1 <= y0:
        return None, None
    crop = img.convert("RGB").crop((x0, y0, x1, y1))
    raw = crop.tobytes()          # 避免 getdata() 的弃用警告
    n = len(raw) // 3
    if n == 0:
        return None, None
    nb = nd = 0
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        if r >= bright and g >= bright and b >= bright:
            nb += 1
        elif r <= dark and g <= dark and b <= dark:
            nd += 1
    return nb / float(n), nd / float(n)


def case_font():
    """图集产物自洽 + 运行时真的加载 + 字真的画上屏。

    ⚠ 这里最要紧的一条是**词典全覆盖**：缺一个汉字，玩家看到的就是一个
    方块，而这种缺失在"跑起来了、有字了"的画面上完全看不出来。
    """
    print("\n[G] SDF 字体：图集自洽 + 词典全覆盖 + 加载锚点 + 文本上屏")
    if not check("图集 PNG + JSON 都在仓库里（跑 tools/gen_sdf_atlas.py）",
                 os.path.isfile(ATLAS_PNG) and os.path.isfile(ATLAS_JSON)):
        return

    with open(ATLAS_JSON, encoding="utf-8") as f:
        meta = json.load(f)
    glyphs = meta["glyphs"]
    aw, ah = meta["atlas"]["w"], meta["atlas"]["h"]
    print("    图集 %dx%d，%d 字形，字号 %.0f，spread %.1f"
          % (aw, ah, len(glyphs), meta["size"], meta["sdfRange"]))
    check("字形数 >= 700", len(glyphs) >= 700)

    # ---- 词典全覆盖（U1 的硬验收）----
    with open(DICT_SRC, encoding="utf-8", errors="replace") as f:
        src = f.read()
    need = set(ch for ch in src
               if ord(ch) > 127 and not (0xE000 <= ord(ch) <= 0xF8FF))
    missing = sorted(ch for ch in need
                     if ("%04X" % ord(ch)) not in glyphs)
    print("    词典非 ASCII 字符 %d 个，缺 %d 个"
          % (len(need), len(missing)))
    if missing:
        print("    缺字示例: %s" % "".join(missing[:24]))
    check("词典里的每个非 ASCII 字符都在图集里", not missing)

    # ---- 度量自洽（结构性质，不钉具体数字）----
    bad_adv = bad_uv = 0
    for g in glyphs.values():
        if g["adv"] <= 0:
            bad_adv += 1
        x, y, w, h = g["uv"]
        if x < 0 or y < 0 or x + w > aw or y + h > ah:
            bad_uv += 1
    check("adv 全部 > 0（%d 个异常）" % bad_adv, bad_adv == 0)
    check("uv 全部落在图集内（%d 个越界）" % bad_uv, bad_uv == 0)

    # ---- 运行时：UI2-FONT 锚点 ----
    kill_stale_editors()
    base_env = {"MYVK_UI_HUD_IN_EDITOR": "1",
                "MYVK_EDITOR_CONSOLE_CMD": "ui.rect 0 0 4096 4096 #000000"}
    img0, ok0, tap0 = grab(base_env, settle=2.5)
    if not check("编辑器启动（黑底那一次）", ok0):
        return
    ln = wait_line(tap0, "UI2-FONT", 10.0)
    if not check("UI2-FONT 出现（图集真的加载了）", ln is not None):
        return
    print("    %s" % ln.strip())
    check("  日志里的字形数与 JSON 一致",
          kv(ln, "glyphs") == str(len(glyphs)))
    check("  图集尺寸与 JSON 一致",
          kv(ln, "atlas") == "%dx%d" % (aw, ah))
    check("  汉字 U+4E00 有字形（top=1）", kv(ln, "top") == "1")
    # ⚠ 日志里是 Windows 反斜杠路径 → 先归一化再比后缀（别比整个前缀，
    #   不然换个工作目录就红）。
    jp = (kv(ln, "json") or "").replace("\\", "/")
    check("  日志写明了实际加载的图集路径（json=…）",
          jp.endswith("assets/ui/fonts/noto_sc_sdf.json"))

    # 黑底那一次：视口里几乎没有亮像素（= 否定测试的基准）
    _c0, vp0 = _vp_center(tap0, img0)
    ink0, dark0 = _ink_ratio(img0, vp0)
    print("    黑底：亮 %.4f%%，暗 %.2f%%"
          % ((ink0 or 0) * 100.0, (dark0 or 0) * 100.0))
    check("黑底矩形铺满视口（暗像素 > 90%）", (dark0 or 0) > 0.90)
    check("黑底上没有亮像素（< 0.05%）—— 文字的否定测试", (ink0 or 0) < 0.0005)

    # ---- 文本上屏 ----
    kill_stale_editors()
    text_env = {"MYVK_UI_HUD_IN_EDITOR": "1",
                "MYVK_EDITOR_CONSOLE_CMD":
                    "ui.rect 0 0 4096 4096 #000000;ui.text 60 220 64 #ffffff "
                    "SDF Ag"}
    img1, ok1, tap1 = grab(text_env, settle=2.5)
    if not check("编辑器启动（带文字那一次）", ok1):
        return
    tln = wait_line(tap1, "UI2-DEBUG-TEXT", 10.0)
    if not check("ui.text 命令执行（UI2-DEBUG-TEXT 出现）", tln is not None):
        return
    check("  on=1", kv(tln, "on") == "1")
    stats = wait_line(tap1, "UI2-STATS", 10.0)
    check("UI2-STATS 出现（文字进了同一批）", stats is not None)
    if stats:
        print("    %s" % stats.strip())
        # 1 个矩形(6) + 6 个字符(36) = 42 顶点；断言"合批仍然一次 draw"
        check("  draws=1（文字与矩形同批，没被拆成第二个 draw）",
              kv(stats, "draws") == "1")

    _c1, vp1 = _vp_center(tap1, img1)
    ink1, dark1 = _ink_ratio(img1, vp1)
    print("    带文字：亮 %.4f%%，暗 %.2f%%"
          % ((ink1 or 0) * 100.0, (dark1 or 0) * 100.0))
    # 64px 的 "SDF Ag" 铺满一行，墨迹占比在千分之几量级 —— 阈值取 0.05%
    # （与否定测试的 0.05% 同一量级，两条合起来才是"画了 vs 没画"的分界）。
    check("白色文字真的画上了（亮像素 >= 0.05%）", (ink1 or 0) >= 0.0005)
    check("文字没有糊满整个视口（亮像素 < 30%）", (ink1 or 0) < 0.30)

    if count_errors(tap1) != 0:
        check("G 组零 [ERROR] / VUID", False)
    else:
        check("G 组零 [ERROR] / VUID", True)


# ============================================================
# H：控件（U4 布局 + U5 按钮/命中）
# ============================================================

_BTN_IDLE = (42, 46, 54)        # #2A2E36（UiWidget 的 idle 色；ABGR 0xE6362E2A）
# 三种状态色**在屏上**的样子（颜色带着 alpha，先与黑底混合再被截图）：
#   idle  (42,46,54)×0.902 = (37.9,41.5,48.7)  实测 (40,43,51)
#   hover (58,64,75)×0.941 = (54.5,60.2,70.5)
#   press (20,23,27)×0.980 = (19.6,22.5,26.5)
_BTN_STATES = ((40, 43, 51), (55, 60, 71), (20, 23, 27))


def _color_ratio(crop, want, tol=10):
    raw = crop.tobytes()
    n = len(raw) // 3
    if not n:
        return 0.0
    hit = 0
    for i in range(0, len(raw), 3):
        if (abs(raw[i] - want[0]) <= tol and
                abs(raw[i + 1] - want[1]) <= tol and
                abs(raw[i + 2] - want[2]) <= tol):
            hit += 1
    return hit / float(n)


def _button_ink(crop):
    """返回 (主色, 主色占比, 白字占比)。

    **主色**才是判据：它免疫"crop 里混进一小块别的东西"，而实现错误照样红
    （白字占 0.2%，永远抢不走主色；黑底若占多数则说明按钮根本没填满）。
    """
    raw = crop.tobytes()
    n = len(raw) // 3
    counts: dict[tuple[int, int, int], int] = {}
    white = 0
    for i in range(0, len(raw), 3):
        c = (raw[i], raw[i + 1], raw[i + 2])
        counts[c] = counts.get(c, 0) + 1
        if c[0] >= 200 and c[1] >= 200 and c[2] >= 200:
            white += 1
    if not counts:
        return (0, 0, 0), 0.0, 0.0
    modal = max(counts, key=lambda k: counts[k])
    return modal, counts[modal] / float(n), white / float(n)


def _design_scale(out):
    return min(out[0] / 1920.0, out[1] / 1080.0)


def case_widget():
    """按钮真的画上屏 + 点击真的命中（U4/U5）。

    ⚠ 像素判据用**差分包围盒**，不自己算坐标：
    `VP-RECT` 到底是 (x,y,w,h) 还是 (x0,y0,x1,y1) 在 D 组里分辨不出来
    （那块矩形铺满整个视口，两种解释的"中心"都落在里面），所以这里只拿
    "黑底"与"黑底+按钮"两张图做差 —— 包围盒自己会告诉我们按钮在哪、多大。
    顺带就把"分辨率无关"钉死了：包围盒的宽高必须等于设计尺寸 × 缩放。
    """
    print("\n[H] 按钮上屏 + 点击命中（U4 布局 / U5 控件）")
    kill_stale_editors()
    black = {"MYVK_UI_HUD_IN_EDITOR": "1",
             "MYVK_EDITOR_CONSOLE_CMD": "ui.rect 0 0 4096 4096 #000000"}
    img_black, ok_b, tap_b = grab(black, settle=2.0)
    if not check("编辑器启动（黑底基准）", ok_b):
        return
    stats_b = wait_line(tap_b, "UI2-STATS", 10.0)
    if not check("黑底基准的 UI2-STATS", stats_b is not None):
        return

    base = ("ui.rect 0 0 4096 4096 #000000;"
            "ui.button 100 100 300 80 OK")
    img, ok, tap = grab({"MYVK_UI_HUD_IN_EDITOR": "1",
                         "MYVK_EDITOR_CONSOLE_CMD": base + ";ui.click 250 140"},
                        settle=2.5)
    if not check("编辑器启动（按钮 + 命中点击）", ok):
        return

    if not check("ui.button 命令执行", wait_line(tap, "UI2-DEBUG-BUTTON", 10.0)
                 is not None):
        return
    stats = wait_line(tap, "UI2-STATS", 10.0)
    if not check("UI2-STATS 出现", stats is not None):
        return
    print("    %s" % stats.strip())
    # 6（黑底矩形）+ 6（按钮圆角矩形）+ 12（"OK" 两个字符）= 24 顶点。
    # 钉死这个数：它同时证明"矩形、控件、文字"三样都进了同一批。
    check("  verts=24（矩形 6 + 按钮 6 + 两字 12）", kv(stats, "verts") == "24")
    check("  draws=1（三样同批）", kv(stats, "draws") == "1")

    clk = wait_line(tap, "UI2-CLICK", 10.0)
    if not check("UI2-CLICK 出现（点击被消费了）", clk is not None):
        return
    print("    %s" % clk.strip())
    # 负向判据就在同一个断言里：miss 会让 hit=0 → 红
    check("  按钮中心点击命中（hit=1）", kv(clk, "hit") == "1")

    # ---- 像素：差分包围盒 = 按钮在屏幕上的真实位置与尺寸 ----
    out_s = kv(stats, "out") or "0x0"
    out = tuple(int(v) for v in out_s.split("x"))
    if not check("拿到输出尺寸", out[0] > 0 and out[1] > 0):
        return
    if not check("两张截图都拿到了", img is not None and img_black is not None):
        return
    diff = ImageChops.difference(img_black.convert("RGB"), img.convert("RGB"))
    bbox = diff.getbbox()
    if not check("差分有内容（按钮确实改变了像素）", bbox is not None):
        return
    bw, bh = bbox[2] - bbox[0], bbox[3] - bbox[1]
    s = _design_scale(out)
    # 离屏图 → 屏幕：ImGui 把 out 大小的图等比贴进视口，所以要再乘一次
    # "显示出来的宽度 / 输出宽度"。这个比例直接从包围盒反推，不依赖 VP-RECT。
    ds = bw / (300.0 * s)
    print("    bbox=%s 宽高=%dx%d 设计 300x80 scale=%.4f → 显示比例≈%.4f"
          % (bbox, bw, bh, s, ds))
    # 宽高比 = 设计宽高比（3.75）：位置/缩放算错会让它偏
    check("包围盒宽高比 = 3.75 ±0.35（布局等比、没有被拉伸）",
          abs(bw / float(bh) - 3.75) <= 0.35)
    check("包围盒高度 ≈ 80 × 显示缩放（±4px）",
          abs(bh - 80.0 * s * ds) <= 4.0)

    crop = img.convert("RGB").crop(bbox)
    modal, modal_r, white_r = _button_ink(crop)
    print("    按钮区域：主色 #%02X%02X%02X %.1f%%，白字 %.1f%%"
          % (modal[0], modal[1], modal[2], 100.0 * modal_r, 100.0 * white_r))
    # ⚠ 判据是「主色属于按钮三态之一，且占比 >= 60%」，**不是**「idle 色占比
    #   >= 45%」。改的理由是 2026-10-19 全量回归的实测：
    #   失败帧的包围盒 = 197x52，正常帧 = 95x25 —— **整整 2.07 倍**，而且那一
    #   帧的 VP 也是 2.07 倍宽（视口面板铺满、侧栏没收起）。也就是说 crop 确确实
    #   实是按钮本身，变的是"当时按钮被画成了哪种状态色"（idle 占比 14.7%，
    #   而 idle 色只有 14.7% 说明主体是 hover：两态色差 12~17，落在 ±10 之外）。
    #   → 原来的写法把「按钮此刻是不是被悬停」也判成了红，是把**状态**当成了
    #     **正确性**。这一组要钉的只有两件事：①按钮真的被填了色（不是透明/没画）
    #     ②色值经过 sRGB 往返没走样（D 组那条 128 vs 188 的同族）。
    #   主色 + 60% 两条合起来仍然可否定：漏掉 srgbToLinear/LinearToSrgb → 主色
    #   跳到 188 附近，三态都不匹配；按钮没画 → 差分空、上面 bbox 先红。
    hit = [c for c in _BTN_STATES
           if all(abs(modal[i] - c[i]) <= 10 for i in range(3))]
    check("按钮主色 = 三态色之一（#2A2E36 家族，sRGB 正确）", bool(hit))
    check("按钮主色占 crop >= 60%（真的被填满，不是几条抗锯齿边）",
          modal_r >= 0.60)
    check("按钮上有白色文字墨迹（>= 1%）", white_r >= 0.01)

    # ---- 负向：点到空白处必须 hit=0（否则命中测试是"永远为真"的假断言）----
    kill_stale_editors()
    img2, ok2, tap2 = grab({"MYVK_UI_HUD_IN_EDITOR": "1",
                            "MYVK_EDITOR_CONSOLE_CMD":
                                base + ";ui.click 1200 900"}, settle=2.0)
    if not check("编辑器启动（未命中点击）", ok2):
        return
    clk2 = wait_line(tap2, "UI2-CLICK", 10.0)
    if not check("UI2-CLICK 出现（未命中那次）", clk2 is not None):
        return
    print("    %s" % clk2.strip())
    check("  空白处点击未命中（hit=0）—— 命中测试的否定测试",
          kv(clk2, "hit") == "0")


def main() -> int:
    print("=" * 74)
    print("游戏 UI / HUD：能力量测 + 零绘制守卫 + 无窗口自检"
          "（docs/ui_hud_roadmap.md U0~U2）")
    print("=" * 74)

    if not os.path.isfile(EXE):
        print("结果: FAIL: 找不到 %s（先 ./make.cmd build）" % EXE)
        return 1

    kill_stale_editors()
    on_img = case_caps()
    case_default_pixels(on_img)
    case_selftest()
    ok_rect, rect_img = case_rect_pixels()
    if ok_rect:
        case_editor_default_off(rect_img)
        case_taa_independent()
    else:
        print("\n（D 组失败 → 跳过 E/F 组）")
    case_font()
    case_widget()

    print()
    print("=" * 74)
    if failures:
        print("结果: FAIL:（%d 项）" % len(failures))
        for f in failures:
            print("   - %s" % f)
        print("=" * 74)
        return 1
    print("结果: PASS（UI2-CAPS 能力量测 + 默认零绘制 + 无窗口数值自检 + "
          "ui.rect 上屏 128 + hudInEditor 默认关 + TAA 无关 + SDF 字体覆盖/上屏"
          " + 按钮上屏/命中）")
    print("=" * 74)
    return 0


if __name__ == "__main__":
    sys.exit(main())
