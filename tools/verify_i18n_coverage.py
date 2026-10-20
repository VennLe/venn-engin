#!/usr/bin/env python3
"""verify_i18n_coverage.py —— 中文化覆盖率体检

背景：`editor/Localization.cpp` 的 kZh[] 是**唯一**的中文来源，调用点永远写英文、
靠 `TR()` 在显示层替换。缺译文只会"某处还是英文"，不会崩 —— 所以没人会注意到，
只能靠扫描兜住。这个工具就是那把尺子。

三类体检：

  A 组 · 参数表（engine/render/FeatureRegistry.cpp）
     `PBool/PFloat/PInt/mkParam` 的 label、tooltip，`kFeatures[]` 的 name/group。
     这些是"属性面板里那一串英文"的真正来源 —— 它们不在任何 ImGui 调用里，
     普通 grep 抓不到，是最容易漏的一块。

  B 组 · 界面字面量（editor/*.cpp，engine/ui/*.cpp）
     ImGui 调用里的字符串字面量，**没有**被 `TR()` 包裹的。

  C 组 · 孤儿译文（kZh[] 里有 en 键，但源码里已经没人用它）
     只报告、不判红（历史遗留 + 少量动态用法），用来防止词典无限膨胀。

用法：
    python tools/verify_i18n_coverage.py            # 只跑门禁，输出汇总
    python tools/verify_i18n_coverage.py --list     # 打全量未覆盖清单
    python tools/verify_i18n_coverage.py --list-group A   # 只看某组
退出码：0 = 全绿；1 = 有未覆盖项（门禁失败）。
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FEATURE_REGISTRY = os.path.join(ROOT, "engine", "render", "FeatureRegistry.cpp")
# 词典本体。U7 起在 engine/i18n/（运行时要共用同一份）——
# `editor/Localization.cpp` 只是转发壳，里面**没有**词条了。
LOCALIZATION = os.path.join(ROOT, "engine", "i18n", "ZhTable.cpp")

# ---- 扫描范围 -------------------------------------------------------------
# 这些目录里出现的界面字面量都算"要翻给人看"。
UI_DIRS = [os.path.join(ROOT, "editor"),
           os.path.join(ROOT, "engine", "ui"),
           # 运行时 HUD / 暂停菜单的文案（U8）：`运行` 里的字面量也是给人看的
           os.path.join(ROOT, "runtime", "ui")]

# ImGui 函数 → 哪个参数是"给用户看的文字"（0 = 第 1 个参数）。
# 没列的函数默认取第 1 个（绝大多数 ImGui 控件的 label/fmt 都在第一位）。
ARG_INDEX = {
    "TextColored": 1,
    "TextDisabled": 0,
    "SetTooltip": 0,
    "BeginTable": 1,
    # hint / 提示文本是第 2 个参数（第 1 个是稳定 ID "##x"，不许翻）
    "InputTextWithHint": 1,
    "Image": -1,          # 纯资源，不对人
    "ImageButton": -1,
    "PushID": -1,         # 稳定 ID，**绝不能翻**
    "PopID": -1,
    "PushStyleColor": -1,
    "PushStyleVar": -1,
    "SetNextWindowSize": -1,
    "SetNextWindowPos": -1,
    "SetCursorPos": -1,
    "SetCursorPosX": -1,
    "SetCursorPosY": -1,
    "Dummy": -1,
    "InvisibleButton": -1,
    "SameLine": -1,
    "Spacing": -1,
    "Separator": -1,
    "NewLine": -1,
    "Indent": -1,
    "Unindent": -1,
    "GetContentRegionAvail": -1,
    "GetCursorPosX": -1,
    "GetCursorPosY": -1,
    "GetItemRectMin": -1,
    "GetItemRectMax": -1,
    "IsItemHovered": -1,
    "IsItemActive": -1,
    "IsItemClicked": -1,
    "GetWindowWidth": -1,
    # 度量串：它们度量的是**别人画出来的文字**，自己不是文案。
    # 若对应绘制点翻了，度量点要跟着用同一译文（两处必须同源），
    # 但那是调用点的责任，不是"给这个串加条译文"能解决的。
    "CalcTextSize": -1,
    "CalcTextSizeA": -1,
    "GetTextLineHeight": -1,
    "GetFrameHeight": -1,
    "ProgressBar": 0,
}

# 这些"文字"其实是路径 / 资源名 / 着色器关键字 / 内部 id，翻了对不上任何东西。
SKIP_PREFIX = ("assets/", "../", "##", "#", "shaders/", "config/", "scenes/",
               "textures/", "fonts/", "ui/", "models/")
SKIP_EXACT = {
    "", " ", "%s", "%zu", "%d", "%f", "%.2f", "%.3f", "%.1f", "%.0f",
    "##", "###", "ID", "OK", "X", "Y", "Z", "-", "+", "/", ":", "|",
    "UTF-8", "GLSL", "Vulkan", "MSVC", "Clang", "GCC", "NaN", "inf",
}
# 纯标识符样式的串（全小写 + 点 / 下划线，如 "ssr.strength"）；不翻。
ID_LIKE = re.compile(r"^[a-z0-9_.\-:/]+$")

# 格式占位符：%[-+ 0#]*[0-9]*(\.[0-9]+)?[diouxXeEfgGscp%]
FMT_SPEC = re.compile(r"%(?:[-+ 0#]*)(?:[0-9]+|\*)?(?:\.(?:[0-9]+|\*))?"
                      r"(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGscp%]")


def is_format_string(s):
    return "%" in s


def format_has_text(s):
    """格式串里"去掉占位符之后"还有没有真正要翻的文字。

    `%s` / `%-12s %s` / `%.3f ms` 的前两个不翻最后一个要翻 ——
    判据就是剥掉占位符后还剩几个字母。纯占位的格式串翻不翻都一样，
    给它加词条只会让词典虚胖，还会掩盖真正的漏翻。
    """
    stripped = FMT_SPEC.sub(" ", s)
    return len([c for c in stripped if c.isascii() and c.isalpha()]) >= 2

CALL_RE = re.compile(r"\bImGui::([A-Za-z_][A-Za-z0-9_]*)\s*\(")
STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
TR_RE = re.compile(r'\bTR\s*\(\s*"((?:[^"\\]|\\.)*)"')
TRID_RE = re.compile(r'\btrId\s*\(\s*"((?:[^"\\]|\\.)*)"')

# 参数表宏：第 2 个字符串参数是 label。
PARAM_MACROS = ("PBool", "PFloat", "PFloatPos", "PInt", "PEnum", "mkParam",
                "mkEnumParam", "PVec3", "PColor3")
MACRO_RE = re.compile(r"\b(" + "|".join(PARAM_MACROS) + r")\s*\(")
FEATURE_ENTRY_RE = re.compile(r'\{\s*"([a-z0-9_]+)"\s*,\s*"([^"]*)"\s*,'
                              r'\s*"([^"]*)"')
ENUM_ARRAY_RE = re.compile(r"const char\* const kEnum\w+\[\]\s*=\s*\{(.*?)\}")


def read(path):
    # newline="" = 不转换换行符。**必须这样读**：本仓库里有的文件是 CRLF、
    # 有的是 LF（历史原因），统一转换会让 --fix 把整个文件重写成另一种风格
    # → diff 变成 5000 行，真正的改动全部淹没。
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def collect_ui_files():
    files = []
    for d in UI_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".cpp"):
                files.append(os.path.join(d, fn))
    return files


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def balanced_args(text, open_paren):
    """从 '(' 位置开始，返回括号内全文（跳过字符串与注释）。"""
    depth = 0
    i = open_paren
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:i]
        i += 1
    return text[open_paren + 1:]


def looks_like_ui_text(s):
    if s in SKIP_EXACT:
        return False
    if any(s.startswith(p) for p in SKIP_PREFIX):
        return False
    if ID_LIKE.match(s):
        return False
    if "%" in s:
        return False          # 格式串本身：占位符不翻，内容另判
    letters = [c for c in s if c.isalpha()]
    if len(letters) < 2:
        return False
    if not any(c.isascii() and c.isalpha() for c in s):
        return False          # 已经是中文了
    # 必须有空格或以大写开头的英文单词才像界面文字
    if " " not in s and not (s[0].isupper() and len(s) > 2):
        return False
    return True


def needs_translation(s):
    """这个串要不要有人给它一条译文。

    · 普通界面文字 → 要；
    · 格式串 → 看剥掉占位符后还剩不剩文字（`%s` 不翻，`%zu entities` 要翻）。
    """
    if looks_like_ui_text(s):
        return True
    return is_format_string(s) and format_has_text(s)


def collect_translated(texts):
    """源码里被 TR()/trId() 包裹的字面量。"""
    out = set()
    for t in texts:
        for m in TR_RE.finditer(t):
            out.add(m.group(1))
        for m in TRID_RE.finditer(t):
            out.add(m.group(1))
    return out


def collect_dict():
    """kZh[] 里的 en 键（按 C++ 语义解码）。

    ⚠ 必须 c_unescape：源码里写的是 `\\n` 两个字符，而 `tr()` 收到的是
      编译后的真换行。不解码的话，**所有带换行的长 tooltip 都查不到**
      （短词没有转义，所以能对上 —— 这种"部分能中"最容易骗人：
      看着工具在减少，其实长条目一条都没生效）。
    """
    text = read(LOCALIZATION)
    out = set()
    for m in re.finditer(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"', text):
        out.add(c_unescape(m.group(1)))
    return out


def collect_dict_dups():
    """kZh[] 里重复的 en 键。

    `zhMap()` 是 `unordered_map::emplace` 还是 `= `？两种行为不同：
    重复键要么被静默忽略、要么后者覆盖前者 —— 无论哪种，**有一条永远读不到**。
    危险的是两份译文不一致时（改了上面那条、界面还是旧词），根本没有编译错误。
    这里只报告，删哪条由人定。
    """
    text = read(LOCALIZATION)
    seen, dups = set(), []
    for m in re.finditer(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"', text):
        k = c_unescape(m.group(1))
        if k in seen:
            dups.append(k)
        seen.add(k)
    return dups


def scan_combo_items():
    """A4'：被 `TrItems<N>(kArr)` 接进 Combo 的选项数组。

    两类问题都会报：
      · 数组元素在词典里没有 → 下拉框里那一项还是英文；
      · `N` 与真实元素数不一致 → 写小了漏翻最后一项，写大了越界读
        （那个是 UB，比"漏翻"严重得多）。
    """
    found = []
    for path in collect_ui_files():
        text = read(path)
        # ⚠ 同一个名字可能在多个函数里各定义一份 `kShapes[]`（生命周期不同）。
        #   按"离调用点最近的那份定义"取，否则会拿错长度 → 假报 N 不符。
        arrays = []   # [(name, start, elems, line)]
        for m in ITEMS_ARRAY_RE.finditer(text):
            elems = [s.group(1) for s in STR_RE.finditer(m.group(2))]
            arrays.append((m.group(1), m.start(), elems,
                           line_of(text, m.start())))
        for m in TRITEMS_RE.finditer(text):
            n = int(m.group(1))
            name = m.group(2)
            cands = [a for a in arrays if a[0] == name and a[1] < m.start()]
            if not cands:
                continue
            _n, _off, elems, ln = max(cands, key=lambda a: a[1])
            if n != len(elems):
                found.append(("<N 与元素数不符: %s N=%d 实际=%d>"
                              % (name, n, len(elems)),
                              os.path.relpath(path, ROOT), ln, name))
            for e in elems:
                found.append((e, os.path.relpath(path, ROOT), ln, name))
    return found


def scan_param_table():
    """A 组：参数表里的 label / tooltip / 特性名 / 分组名。"""
    text = read(FEATURE_REGISTRY)
    labels, tooltips = [], []
    for m in MACRO_RE.finditer(text):
        args = balanced_args(text, m.end() - 1)
        strs = [s.group(1) for s in STR_RE.finditer(args)]
        ln = line_of(text, m.start())
        # mkParam 的第 2 个字符串才是 label；P* 宏也是第 2 个。
        if len(strs) >= 2:
            labels.append((strs[1], ln, m.group(1)))
        # tooltip 约定是**最后一个**中文串（env 名是第 3 个）
        for s in reversed(strs):
            if any(ord(c) > 0x2E7F for c in s):
                tooltips.append((s, ln, m.group(1)))
                break
    names = []
    for m in FEATURE_ENTRY_RE.finditer(text):
        names.append((m.group(2), line_of(text, m.start()), "FeatureDesc.name"))
        names.append((m.group(3), line_of(text, m.start()), "FeatureDesc.group"))
    enums = []
    for m in ENUM_ARRAY_RE.finditer(text):
        for s in STR_RE.finditer(m.group(1)):
            enums.append((s.group(1), line_of(text, m.start()),
                          "enumNames"))
    return labels, tooltips, names, enums


def c_unescape(s):
    """按 C++ 语义还原转义序列（只处理常见的几种）。

    为什么必须还原：`tr()` 的 key 是**编译后**的字符串。源码里写成
    `"line1\\n" "line2"`，编译后是 "line1\nline2"（真换行）——
    如果拿源码里的两字符 `\\n` 当 key，词典里那条永远查不到。
    """
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            out.append({"n": "\n", "t": "\t", "r": "\r", "\\": "\\",
                        '"': '"', "0": "\0"}.get(c, "\\" + c))
            i += 2
            continue
        out.append(s[i])
        i += 1
    return "".join(out)


def string_groups(part):
    """把参数片段切成"字面量组"，返回 [(起始, 结束, 解码后文本)]。

    ⚠ 相邻字面量（`"a" "b"`）在 C++ 里是**一个**字符串，必须合成一组：
      · 翻译要拿完整句子当 key（拆开翻会把句子切碎，译文对不上）；
      · 改写只能在**组首**插 `TRL(`、**组尾**插 `)`，中间插就语法错误。
    """
    groups = []
    i, n = 0, len(part)
    while i < n:
        if part[i] != '"':
            i += 1
            continue
        start = i
        j = i + 1
        while j < n:
            if part[j] == "\\":
                j += 2
                continue
            if part[j] == '"':
                j += 1
                break
            j += 1
        raw = part[start + 1:j - 1]
        end = j
        # 吞并紧邻的后续字面量（中间只允许空白）
        while True:
            k = end
            while k < n and part[k] in " \t\r\n":
                k += 1
            if k >= n or part[k] != '"':
                break
            k2 = k + 1
            while k2 < n:
                if part[k2] == "\\":
                    k2 += 2
                    continue
                if part[k2] == '"':
                    k2 += 1
                    break
                k2 += 1
            raw += part[k + 1:k2 - 1]
            end = k2
        groups.append((start, end, c_unescape(raw)))
        i = end
    return groups


def split_top_level(args):
    """按顶层逗号切参数，返回 [(起始偏移, 文本片段)]。

    ⚠ 必须跳过字符串与嵌套括号 —— `ImVec2(-1.0f, 0.0f)` 里的逗号不是
      参数分隔符。切错了偏移量，自动改写就会把 `TRL(` 插到错误的位置。
    """
    parts, depth, cur, start, i = [], 0, "", 0, 0
    n = len(args)
    while i < n:
        c = args[i]
        if c == '"':
            cur += c
            i += 1
            while i < n:
                cur += args[i]
                if args[i] == "\\" and i + 1 < n:
                    cur += args[i + 1]
                    i += 2
                    continue
                if args[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            parts.append((start, cur))
            cur = ""
            i += 1
            start = i
            continue
        cur += c
        i += 1
    parts.append((start, cur))
    return parts


def scan_ui_literals():
    """B 组：ImGui 调用里没被 TR() 包裹的界面字面量。

    返回 (字面量, 相对路径, 行号, 函数名, 起始偏移, 结束偏移) —— 偏移是
    **含引号**的绝对位置，`--fix` 靠它做原地改写。
    """
    hits = []
    for path in collect_ui_files():
        text = read(path)
        for m in CALL_RE.finditer(text):
            fn = m.group(1)
            idx = ARG_INDEX.get(fn, 0)
            if idx < 0:
                continue
            args = balanced_args(text, m.end() - 1)
            args_start = m.end()
            parts = split_top_level(args)
            if idx >= len(parts):
                continue
            off, part = parts[idx]
            # ⚠ 逐个"字面量组"判定：`cond ? TRL("Open") : "Open / Run"`
            #   两个分支都要翻，只判第一个会静默漏掉另一半。
            for gstart, gend, gtext in string_groups(part):
                if not needs_translation(gtext):
                    continue
                prefix = part[:gstart].rstrip()
                if prefix.endswith("TR(") or prefix.endswith("TRL(") or \
                        prefix.endswith("trId("):
                    continue
                hits.append((gtext, os.path.relpath(path, ROOT),
                             line_of(text, m.start()), fn,
                             args_start + off + gstart,
                             args_start + off + gend))
    return hits


def c_escape(s):
    """反向：把解码后的文本写成 C++ 字符串字面量内容（不含外层引号）。"""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        else:
            out.append(ch)
    return "".join(out)


# Combo 的 items 数组：`static const char* kXxx[] = {"a", "b"};`
ITEMS_ARRAY_RE = re.compile(
    r"(?:static\s+)?const\s+char\s*\*\s*(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;",
    re.S)
TRITEMS_RE = re.compile(r"TrItems\s*<\s*(\d+)\s*>\s*\(\s*(\w+)\s*\)")

WRAP_CALL_RE = re.compile(r"\b(TR|TRL|trId)\s*\(")


def collect_wrapped_keys():
    """所有被 TR()/TRL()/trId() 包裹的 key（已按字面量组解码）。

    ⚠ 这是本工具最重要的一组：`TR()` 包了、词典里却没有 → 中文界面下
      屏幕上仍然显示英文。B 组（"未包 TR"）归零并不代表翻完了。

    ⚠ `TRL` 的口径是**按第一个 `##` 切开、只翻前半截**（见 trLabel），
      所以这里查的 key 也要剥掉后缀 —— 否则 `Duration##fx` 会被当成
      独立词条，词典里就得为每个后缀各写一条（几十条纯重复）。
    """
    keys = []
    for path in collect_ui_files() + [FEATURE_REGISTRY]:
        text = read(path)
        for m in WRAP_CALL_RE.finditer(text):
            fn = m.group(1)
            args = balanced_args(text, m.end() - 1)
            groups = string_groups(args)
            if not groups:
                continue
            key = groups[0][2]
            if fn == "TRL":
                cut = key.find("##")
                if cut >= 0:
                    key = key[:cut]
            keys.append(key)
    return keys


FMT_CALL_RE = re.compile(r"\b(sn?printf)\s*\(")

# 有意保留英文的格式串（D 组白名单）。**精确匹配，不做子串**。
#
# ⚠ 这里用子串匹配过一次，结果灾难：白名单里放了 "%s" 和 "GB"，于是
#   "view mode   %s"、"selected: %s"、"VRAM  %.1f GB" 全被当成"已豁免"
#   静默放过 —— 命中数从 24 掉到 11，看着像"快修完了"，实际是把还漏的
#   一批藏起来了。**豁免条目必须整串相等**才算数。
#
# 判据只有一条：翻成中文会让它变得更难用。符合的只有两类：
#   ① 纯 ID / 锚点（"CB-TYPE %s"、"##toast%zu"）—— 回归脚本按字符串匹配；
#   ② 纯数值 / 变量转储（"%s = %d"）—— 里面没有"给人读的英文词"。
# ⚠ 单位（KB / GB / m/s / FPS）**不在豁免之列**：那些行里真正要翻的是
#   标签（"VRAM"、"view mode"），单位跟着标签一起写进中文串即可。
FMT_KEEP = (
    # --- 纯 ID / 自动化锚点 ---
    "slot:@%p",
    "##toast%zu",
    "builtin/cube/s%.4f", "builtin/plane/s%.4f",
    "builtin/sphere/r%.4f/g%d/n%d", "builtin/cylinder/r%.4f/g%d",
    "CB-TYPE %s", "CB-ADDITEM %s", "CB-TREE-NODE %s",
    # W4-4：地形段 4 个层槽的锚点。数字后缀是"第几层"，翻成中文反而让
    # 回归脚本抓不到（`verify_terrain_editor.py` 按 LAYER0..3 逐个等）。
    "INS-TERRAIN-LAYER%d",
    "BAR-BTN-%s", "BAR-TRAY-%s", "BAR-TRAY-X-%s", "BAR-PICK-%s", "tray_%s",
    "TB-ADD-%s", "TB-SELMODE-ITEM %s", "TB-PLAY-%s",
    "VP-TOOLITEM %s", "VP-SNAPITEM %s-%d", "VP-VIEWDIRITEM %s",
    "VP-LAYOUTITEM %s", "VP-LIGHTITEM %d", "VP-SETITEM %s",
    "FX-ROW %d", "FX-BURST-%d", "FX-PRESET %d",
    "SPLIT-%d", "##split_preset_%d",
    "NOTIF-DUMP toasts=%d history=%d warn=%d error=%d "
    "unread=%d tasks=%d minimized=%d",     # con->addCommand("notif.dump") 的开发期转储
    # W4-5/W5-0：地形笔刷与植被物种的 `logRect` 锚点 + 材质/网格资产 key。
    # 这些是**回归脚本按名字逐个等的稳定标识**，翻成中文脚本就抓不到了；
    # 而且它们从不出现在屏幕上（只进日志），本来就不该翻。
    "LS-TOOL-%d", "FO-KIND-%d", "TB-EDITMODE-ITEM %s",
    "foliage/mesh/%s", "foliage/mat/%s/%d",
    # 2026-10-20 晚：六个工具面板（ToolkitEditors.cpp）的锚点 / 资产 key /
    # 自动生成的实体名。同样"从不上屏"：锚点只进日志，key 只进 AssetManager，
    # 实体名是 `Chunk 010203` / `Brush Sphere 01` 这类**数据**（用户可在层级里
    # 改），翻了反而让回归脚本与存档里的名字对不上。
    "TK-%s", "BR-SHAPE-%d", "PCG-KIND-%d",
    "toolkit/paint/%u/%d", "toolkit/model/%u/%d", "toolkit/mat/%s",
    "Chunk %02d%02d%02d", "Chunk %02d", "Brush %s %02d", "PCG %s %03d",
    # 2026-10-20 夜：Modeling 的选区层级按钮锚点 + Voronoi 的网格 key。
    # 和上面同一类 —— 锚点只进日志（脚本按 MD-SELMODE-2 抓 Face 按钮），
    # key 只进 AssetManager，翻成中文两边都立刻断。
    "MD-SELMODE-%d", "toolkit/fracture/%u/%d/%d",
    # --- 纯 ID 拼接（## 之后是稳定 ID，前面是已经翻过的变量）---
    "%s###panel_%s", "%s##seg%d", "%s##speed", "%s##%s", "%s##ws_%s",
    "%s##recent%zu", "%s %s##hist%zu", "%s##vpl%d",
    # --- 键位和弦：Ctrl/Alt/Shift 是键名，翻译反而看不懂 ---
    "Ctrl+Alt+%d",
    # --- 纯数值 / 变量转储 ---
    "%s = %s", "%s = %d (%s)", "%s = %d", "%s = %.4f",
    "f%-6d %.2fs", "k%u  %s  x%u\n",
    "%.1f KB", "%.1f MB",          # 只有单位，没有标签
    "%s", "%d", "%u", "%.3f", "(%.2f,%.2f,%.2f)",
)


def scan_bare_formats():
    """D 组：`snprintf/sprintf` 的格式串本身是英文且没包 TR()。

    为什么单列一组：这类字面量**不在 ImGui 调用参数里**，B 组的 CALL_RE
    一条都扫不到，而屏幕上可能是整行英文。清单里真实漏过的例子：
        std::snprintf(label, sizeof(label), "tasks %d", n);
        ImGui::TextColored(..., TR("%s"), label);      // ← 看着像翻过了
        std::snprintf(buf, ..., "Move %s: %+.3f m%s", ...);  // 视口变换读数

    ⚠ 判据用 needs_translation()（格式串走 format_has_text，剥掉 %d/%s
      之后还要剩 ≥2 个字母才算"有英文"），否则 "%s" / "%d" 会刷一片假阳性。

    返回 (缺译文的命中列表, 检查总数)。
    """
    hits, total = [], 0
    for path in collect_ui_files():
        text = read(path)
        for m in FMT_CALL_RE.finditer(text):
            fn = m.group(1)
            args = balanced_args(text, m.end() - 1)
            parts = split_top_level(args)
            # snprintf(dst, size, fmt, ...) → fmt 是第 3 个；sprintf(dst, fmt, ...) 是第 2 个
            idx = 2 if fn == "snprintf" else 1
            if idx >= len(parts):
                continue
            off, part = parts[idx]
            groups = string_groups(part)
            if not groups:
                continue                    # 格式串是变量，静态扫不到
            total += 1
            gstart, _gend, decoded = groups[0]
            # 已经被 TR(...) 包住？（口径同 B 组：看前一个非空字符是不是 '('）
            head = part[:gstart].rstrip()
            if head.endswith("TR(") or head.endswith("TRL("):
                continue
            if any(c > "\x7f" for c in decoded):
                continue                    # 本来就是中文原文
            if not needs_translation(decoded):
                continue
            if decoded in FMT_KEEP:
                continue
            if decoded.lstrip().startswith("{"):
                continue                    # JSON 模板（内容浏览器的资源描述）
            hits.append((decoded, path, line_of(text, m.start()), fn))
    return hits, total


WIDGET_CALL_RE = re.compile(
    r"\bImGui::(?:DragFloat[234]?|SliderFloat[234]?|InputFloat[234]?|"
    r"DragInt[234]?|SliderInt[234]?|InputInt[234]?|SliderAngle|"
    r"ProgressBar|DragScalar|SliderScalar)\s*\(")

# 单字母 / 符号单位：needs_translation() 要求"剥掉占位符后还剩 ≥2 个字母"，
# 于是 "%.3f m"、"%.2f s" 会被判成"没有英文"而漏掉 —— 但它们恰恰是屏幕上
# 最扎眼的英文（`位置 3.000 m`）。这条正则专门兜住单位。
UNIT_FMT = re.compile(
    r"^%[-+ 0#0-9.*hlLzjt]*[dfgGeE]\s*(?:m|s|ms|cm|rad|kg|Hz|deg)(?:/s|/s2|/m3)?$")


def scan_widget_formats():
    """E 组：DragFloat / SliderFloat 的**显示格式串**没被 TR() 包裹。

    这是第三类盲区，前面两种扫法都看不见：
      · B 组只扫"界面字面量"（label），格式串不是 label；
      · D 组只扫 snprintf/sprintf 的格式串，这里是 ImGui 自己 vsnprintf。
    真实漏过的例子：`SliderFloat(TRL("Yaw"), &yaw, -180, 180, "%.1f deg")`
    —— 控件标签已经中文了，右边数字后面还挂着 `35.5 deg`。

    怎么定位格式串：它是**最后一个"整体就是字符串字面量"的参数**（min/max
    之后），所以从参数尾部往前找第一个纯字面量参数即可。找不到就跳过
    （有些控件用 ImGui 的默认格式）。

    返回 (缺译文的命中列表, 检查总数)。
    """
    hits, total = [], 0
    for path in collect_ui_files():
        text = read(path)
        for m in WIDGET_CALL_RE.finditer(text):
            args = balanced_args(text, m.end() - 1)
            parts = split_top_level(args)
            for off, part in reversed(parts):
                groups = string_groups(part)
                if not groups:
                    continue
                gstart, gend, decoded = groups[0]
                # 参数里必须只有这一个字面量，否则不是单纯的格式串参数
                if part[:gstart].strip() or part[gend:].strip():
                    continue
                total += 1
                if part[:gstart].rstrip().endswith("TR("):
                    break
                if any(c > "\x7f" for c in decoded):
                    break
                if not (needs_translation(decoded) or
                        UNIT_FMT.match(decoded.strip())):
                    break
                hits.append((decoded, path, line_of(text, m.start()), "imgui"))
                break
    return hits, total


def scan_placeholder_drift():
    """F 组：译文里的 printf 占位符序列必须与英文原文**完全一致**。

    这是**崩溃级**问题，不是"少了几个字"：`{"%d items", "%s 项"}` 会让
    `snprintf` 拿整数去当 `const char*` 解引用（或反之），轻则乱码重则崩。
    而且英文模式不经过词典，**只有切到中文才会崩** —— 最容易漏测的一类。

    只比"占位符序列"（类型字符 + 长度修饰），不比宽度：宽度改了不致命
    （`%.1f` → `%.2f` 只是显示不同），但**类型改了会崩**。这里按严格相等比，
    宁可让人确认一遍。

    返回 [(en, zh, en_specs, zh_specs)]。
    """
    text = read(LOCALIZATION)
    start = text.index("const Entry kZh[] = {")
    bad = []
    for m in re.finditer(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}',
                         text[start:]):
        en = c_unescape(m.group(1))
        zh = c_unescape(m.group(2))
        a, b = FMT_SPEC.findall(en), FMT_SPEC.findall(zh)
        if (a or b) and a != b:
            bad.append((en, zh, a, b))
    return bad


def apply_fix(hits):
    """把命中的字面量原地包成 `TRL("...")`。

    ⚠ 从后往前替换：先改后面的偏移，前面的偏移才仍然有效。
    """
    by_file = {}
    for _lit, rel, _ln, _fn, s, e in hits:
        by_file.setdefault(rel, []).append((s, e))
    total = 0
    for rel, spans in by_file.items():
        path = os.path.join(ROOT, rel)
        text = read(path)
        for s, e in sorted(spans, reverse=True):
            text = text[:s] + "TRL(" + text[s:e] + ")" + text[e:]
            total += 1
        write(path, text)
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--list-group", default="")
    ap.add_argument("--fix", action="store_true",
                    help="把 B 组的命中项原地包成 TRL(...)（改完要重编译）")
    ap.add_argument("--dump-keys", action="store_true",
                    help="按 C++ 转义列出所有缺译文的 key（贴进 kZh[] 用）")
    args = ap.parse_args()

    dict_en = collect_dict()
    labels, tooltips, names, enums = scan_param_table()
    ui_hits = scan_ui_literals()

    all_src = [read(p) for p in collect_ui_files()]
    wrapped = collect_translated(all_src + [read(FEATURE_REGISTRY)])

    if args.fix:
        n = apply_fix(ui_hits)
        print("已把 %d 处字面量包成 TRL(...)。重跑一次确认 B 组归零。" % n)
        ui_hits = scan_ui_literals()

    def report(title, items, key):
        miss = [it for it in items if key(it) not in dict_en]
        print("%-38s 共 %3d，缺译文 %3d（%.1f%%）"
              % (title, len(items), len(miss),
                 100.0 * (len(items) - len(miss)) / max(1, len(items))))
        return miss

    def is_cjk(s):
        return any(ord(c) > 0x2E7F for c in s)

    print("=" * 78)
    print("中文化覆盖率体检（engine/i18n/ZhTable.cpp 词典 %d 条）" % len(dict_en))
    print("=" * 78)

    miss_labels = report("A1 · 参数 label（英文原文 + 词典）", labels,
                         lambda x: x[0])
    # ⚠ tooltip 的口径不同：它**本来就是中文原文**（不是"英文待翻"），
    #   所以判据是"含不含 CJK"，不是"在不在词典里"。
    miss_tips = [t for t in tooltips if not is_cjk(t[0])]
    print("%-38s 共 %3d，仍是英文 %3d"
          % ("A2 · 参数 tooltip（应为中文原文）", len(tooltips),
             len(miss_tips)))
    miss_names = report("A3 · 特性名 / 分组名", names, lambda x: x[0])
    combo_items = scan_combo_items()
    # 纯缩写（S/M/L/XL、2x/4x）不要求译文：翻成中文反而更长更难看。
    combo_need = [(t, p, l, a) for (t, p, l, a) in combo_items
                  if looks_like_ui_text(t) or t.startswith("<N ")]
    miss_enums = [it for it in combo_need if it[0] not in dict_en]
    print("%-38s 共 %3d，缺译文 %3d"
          % ("A4 · Combo 选项数组（TrItems）", len(combo_items),
             len(miss_enums)))
    miss_ui = report("B  · 界面字面量未包 TR()", ui_hits, lambda x: x[0])

    # B2：包了 TR 但词典里没有 → 屏幕上仍然是英文。**这才是"没翻完"的真身。**
    wrapped_keys = collect_wrapped_keys()
    uniq_wrapped = []
    seen_k = set()
    for k in wrapped_keys:
        if k in seen_k:
            continue
        seen_k.add(k)
        if not needs_translation(k):
            continue
        if k in dict_en:
            continue
        uniq_wrapped.append((k, 0, "TR/TRL"))
    print("%-38s 共 %3d，缺译文 %3d"
          % ("B2 · 已包 TR 但词典缺失", len(seen_k), len(uniq_wrapped)))

    # ---- C：孤儿词条（**只报告，不判红**）----
    #
    # 词典只增不减的话迟早变成一片"以前的界面"坟场，而且没人敢删。判据是
    # 反过来的：kZh[] 里存在、但源码里**连这个字面量都找不到** → 必然是死条目。
    #
    # ⚠ 为什么不用"不在 TR() 里"来判？因为有一批 TR 的入参是**变量**：
    #     TR(kWorkspaces[i].title)、TR(m_ctx->status().c_str())、TR(handleName())
    #   扫描器看不见它们，会把 "Level"/"Ready"/"screen" 误报成孤儿。
    #   改成"整个文件里有没有这个字面量"就干净了 —— 死条目连字符串都不存在。
    all_lits = set()
    for t in all_src:
        for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', t):
            all_lits.add(c_unescape(m.group(1)))
    orphans = sorted(k for k in dict_en
                     if k not in wrapped_keys and k not in all_lits
                     and needs_translation(k))
    dups = collect_dict_dups()
    print("%-38s 共 %3d，疑似死条目 %3d，重复键 %d"
          % ("C · 词典孤儿（源码已无此字面量）", len(dict_en), len(orphans),
             len(dups)))

    # ---- D：裸 snprintf 格式串（**判红**）----
    #
    # 这是整整一大类盲区：`snprintf(buf, n, "tasks %d", v)` 之后调
    # `Text(TR("%s"), buf)` —— 字面量不在 ImGui 调用里，B 组一条都看不见，
    # 屏幕上却是实打实的英文（"tasks %d"、变换读数全是这么漏的）。
    bare_hits, bare_total = scan_bare_formats()
    print("%-38s 共 %3d，仍是英文 %3d"
          % ("D · snprintf 格式串未包 TR()", bare_total, len(bare_hits)))

    widget_hits, widget_total = scan_widget_formats()
    print("%-38s 共 %3d，仍是英文 %3d"
          % ("E · 控件显示格式串未包 TR()", widget_total, len(widget_hits)))

    drift = scan_placeholder_drift()
    print("%-38s 占位符不一致 %3d"
          % ("F · 译文占位符漂移（崩溃级）", len(drift)))

    if args.list or args.list_group:
        want = args.list_group.upper()
        groups = [
            ("A1", "参数 label（属性面板里那一串）", miss_labels),
            ("A2", "参数 tooltip", miss_tips),
            ("A3", "特性名 / 分组名", miss_names),
            ("A4", "枚举选项名", miss_enums),
            ("B", "界面字面量（未包 TR）", miss_ui),
            ("D", "snprintf 格式串（未包 TR）", bare_hits),
            ("E", "控件显示格式串（未包 TR）", widget_hits),
        ]
        for gid, gname, items in groups:
            if want and want != gid:
                continue
            if not items:
                continue
            print()
            print("-" * 78)
            print("[%s] %s —— %d 项" % (gid, gname, len(items)))
            print("-" * 78)
            for it in items:
                if gid == "A4":
                    print("  %-40s %s:%s" % (it[0][:40], it[1], it[2]))
                elif gid == "B":
                    print("  %-46s %s:%d  %s" % (it[0][:46], it[1], it[2], it[3]))
                elif gid in ("D", "E"):
                    print("  %-40s %s:%d  %s()" % (it[0][:40], it[1], it[2], it[3]))
                else:
                    print("  L%-5d %-22s %s" % (it[1], it[2], it[0][:60]))

    if args.list_group.upper() == "F" and drift:
        print()
        print("-" * 78)
        print("[F] 译文占位符漂移 —— %d 项（切到中文会崩/乱码）" % len(drift))
        print("-" * 78)
        for en, zh, a, b in drift:
            print("  EN %s\n     %s\n     ZH %s\n     %s"
                  % (en[:70], a, zh[:70], b))

    if args.list_group.upper() == "C" and (orphans or dups):
        print()
        print("-" * 78)
        print("[C] 词典孤儿 %d 项 / 重复键 %d 项（可删，删前确认没有 TR(变量) 在用）"
              % (len(orphans), len(dups)))
        print("-" * 78)
        for k in orphans:
            print("  孤儿  %s" % k[:70])
        for k in dups:
            print("  重复  %s" % k[:70])

    total = (len(miss_labels) + len(miss_tips) + len(miss_names) +
             len(miss_enums) + len(miss_ui) + len(uniq_wrapped) +
             len(bare_hits) + len(widget_hits) + len(drift))

    if args.dump_keys:
        print()
        print("// ---- 以下按组列出缺译文的 key（可直接贴进 kZh[]）----")
        groups = [
            ("A1 参数 label", miss_labels),
            ("A3 特性名 / 分组名", miss_names),
            ("B 界面字面量（未包 TR）", miss_ui),
            ("B2 已包 TR 但缺译文", uniq_wrapped),
        ]
        for gname, items in groups:
            if not items:
                continue
            print()
            print("    // ===== %s =====" % gname)
            seen = set()
            for it in items:
                k = it[0]
                if k in seen:
                    continue
                seen.add(k)
                print('    {"%s", ""},' % c_escape(k))
    print()
    print("=" * 78)
    if total == 0:
        print("结果: PASS")
    else:
        print("结果: FAIL:（%d 项缺中文）" % total)
        print("  A1=%d A2=%d A3=%d A4=%d B=%d B2=%d D=%d E=%d F=%d"
              % (len(miss_labels), len(miss_tips), len(miss_names),
                 len(miss_enums), len(miss_ui), len(uniq_wrapped),
                 len(bare_hits), len(widget_hits), len(drift)))
    print("=" * 78)
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
