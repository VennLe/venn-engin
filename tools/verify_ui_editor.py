"""端到端验证：UI 编辑器（#312）—— 控件树能改、锚点能换、绑定能驱动。

==============================================================================
这一条在验什么
==============================================================================
在 #312 之前，"界面上有什么"只存在于 C++ 里：血条坐标写死在 `Hud.cpp`、
暂停菜单写死在 `PauseMenu.cpp`。这份脚本钉的是"界面变成一份资产"：

  A 打开：面板画出来了（13 个控件锚点）+ 文档真解析了（7 个控件）
  B 改数值 + 存盘：位置 / 尺寸 / 颜色 / 圆角 / 字号 / 对齐都落到 JSON 里，
    而且**注释原样保住、中文没有 \\u 转义**（"存盘不丢东西"最硬的三条）
  C 换锚点**保屏幕位置**：bottom-right → top-right 之后 rect 不变、x/y 重算
    —— 这是"拖锚点"真正要保证的语义（不然控件会瞬间跳走一个自己的宽度）
  D 打开坏文件 → 只读 + Save 被拒 + 坏文件 1 字节没动
  E 空手新建 → 三控件骨架能存能加载（第一次做界面的那条路）
  F 运行时按文档画：`Game.exe --ui-doc` 真的读了它，几何与锚点对得上
  G 数据绑定：脚本 damage(30) → 文档里的血条填充缩到 70%（量几何）
  H 截图 diff：改一个控件位置 → 画布区域显著变化，且**不是整图平移**

==============================================================================
三个刻意的取舍
==============================================================================
① **注入的是 ASCII 文本**。`MYVK_EDITOR_UIED_SCRIPT` 走 `std::getenv`，
   Windows 的窄环境块按 ANSI 代码页转换 —— 中文进去会变成 GBK 字节，断言
   测到的就是"编码没配好"而不是"编辑器对不对"。中文是否原样，改用**更硬的**
   判据：把原文件里的中文（"//" 注释里那句）往返一遍逐字节比对（B6/B7）。

② **不用鼠标点按钮**。注入脚本调的 `act*` 正是按钮调的那一批函数（见
   UiEditorWindow.h 的"动作"一节），走同一条路，验的是真行为。矩形锚点仍然
   验一遍：它证明面板真的画在屏幕上，而不是只跑了逻辑。

③ **H 用自校准的像素判据**（沿用 `verify_hud` 的结论）：先同配置跑两遍求
   "基线噪声"，再跑改动版；判据是"显著变化像素占比 > 0.5% 且 > 10× 基线"，
   外加"包围盒内的变化密度 < 15%"（证明是**零星几处**改动，不是整块重排；
   这里不用包围盒面积 —— 改一个控件会同时动画布、右栏数字、校验条三处，
   包围盒必然覆盖大半个窗口）。

用法：
    python tools/verify_ui_editor.py
    python tools/verify_ui_editor.py --only a    # 只跑某个用例
    python tools/verify_ui_editor.py --only fg   # 运行时 + 绑定
    python tools/verify_ui_editor.py --keep      # 留下临时目录
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")
GAME_EXE = os.path.join(BIN, "Game.exe")

# 被编辑的资产（相对 assets/）。4 个顶层控件 + 3 个子控件 = 7。
ASSET = "ui/docs/hud_demo.ui.json"
ASSET_ABS = os.path.join(BIN, "assets", ASSET)
EXPECT_WIDGETS = 7

# HUD 夹具（借用 verify_hud 的那一份：第 60 帧 damage(30)）
SCENE_DAMAGE = "scenes/runtime_test/hud_damage_scene.json"

FRAMES = 150          # 编辑器跑满就退（不认这个变量就得 kill，日志会截错帧）
GAME_FRAMES = 220     # 运行时：留够 3 个 60 帧节流窗口

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def run_editor(extra_env, wait_s=120.0):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"     # 不设它 ui::logRect 直接 return
    env["MYVK_GRID"] = "0"
    env["MYVK_FRAMES"] = str(FRAMES)
    for k, v in extra_env.items():
        if v is not None:
            env[k] = v

    try:
        subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                       capture_output=True, timeout=10)
    except Exception:
        pass

    proc = subprocess.Popen([EDITOR_EXE], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=20)
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


def run_game(args, wait_s=150.0):
    env = dict(os.environ)
    try:
        subprocess.run(["taskkill", "/F", "/IM", "Game.exe"],
                       capture_output=True, timeout=10)
    except Exception:
        pass
    proc = subprocess.Popen([GAME_EXE] + args, cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=20)
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def join(p):
    return p.replace("\\", "/")


# ------------------------------------------------------------------ 日志解析
OPEN_RE = re.compile(r"UIED-OPEN path=(\S+) ok=(\d+) widgets=(\d+) readOnly=(\d+)")
CHECK_RE = re.compile(r"UIED-CHECK ok=(\d+) widgets=(\d+) warns=(\d+) err=(.*)$")
BLOCK_RE = re.compile(r"UIED-SAVE-BLOCKED path=(\S+) reason=([\w-]+)")
# ↑ `reason=(\S+)` 会被紧随其后的 NOTIF-PUSH 回显行截走：那行是
#   `text="UIED-SAVE-BLOCKED … reason=read-only"`，`\S+` 一路吃到最后那个
#   引号，断言拿到 `read-only"` 就永远不等。改 `[\w-]+` 只吃单词字符和连字符。
SAVE_RE = re.compile(r"UIDOC-SAVE path=(\S+) full=(\S+) widgets=(\d+)")
NEW_RE = re.compile(r"UIED-NEW widgets=(\d+) ref=(\d+)x(\d+)")
ANCHOR_RE = re.compile(r"UIED-ANCHOR id=(\S+) from=(\S+) to=(\S+) x=([-\d.]+) "
                       r"y=([-\d.]+) keep=(\d)")
RECT_RE = re.compile(r"UIED-([A-Z-]+)=\(([-\d.]+),([-\d.]+)\)-"
                     r"\(([-\d.]+),([-\d.]+)\)")
LOAD_RE = re.compile(r"UIDOC-LOAD path=(\S+) ok=(\d+) widgets=(\d+)")
BOUNDS_RE = re.compile(r"UIDOC-BOUNDS id=(\S+) type=(\S+) x=([-\d.]+) "
                       r"y=([-\d.]+) w=([-\d.]+) h=([-\d.]+) fill=([-\d.]+)")
STATS_RE = re.compile(r"UIDOC-STATS widgets=(\d+) drawn=(\d+) skipped=(\d+) "
                      r"texts=(\d+) out=(\d+)x(\d+) bindings=(\d+)")


def last(regex, text):
    hits = [m for m in (regex.search(l) for l in text.splitlines()) if m]
    return hits[-1] if hits else None


def all_of(regex, text):
    return [m for m in (regex.search(l) for l in text.splitlines()) if m]


def groups(m):
    return m.groups() if m else "none"


def rects(text):
    d = {}
    for m in RECT_RE.finditer(text):
        d.setdefault(m.group(1), []).append(
            (float(m.group(2)), float(m.group(3)),
             float(m.group(4)), float(m.group(5))))
    return d


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def find_child(doc, wid):
    """在全树里按 id 找控件（不按路径 —— 路径写错会让断言跟着错）。"""
    stack = list(doc.get("widgets", []))
    while stack:
        w = stack.pop()
        if w.get("id") == wid:
            return w
        stack.extend(w.get("children", []))
    return None


def anchor_offset(anchor):
    """锚点 → (ax, ay)，取值 -1/0/+1，与 `ui2::UiAnchor` 的九宫格一致。

    0 号 `top-left` 是默认值，`toJson` 会省掉这个字段，所以缺省必须当 0 算。
    """
    return {"top-left": (-1, -1), "top-center": (0, -1), "top-right": (1, -1),
            "center-left": (-1, 0), "center": (0, 0), "center-right": (1, 0),
            "bottom-left": (-1, 1), "bottom-center": (0, 1),
            "bottom-right": (1, 1)}.get(anchor, (-1, -1))


def screen_rect(doc, rect, anchor, parent=None):
    """把文档里的 `rect`（锚点相关）换算成画布上的**屏幕矩形**。

    `rect` 里的 x/y 是"距锚点的那一侧有多远"，不是屏幕坐标：
      * 左锚  x 从父矩形左边量；右锚 x 从右边量；中锚 x 从中心量
      * 上锚  y 从父矩形顶边量；下锚 y 从底边量；中锚 y 从中心量
    所以换锚点要"屏幕位置不变"时，文档里的 rect **必须变**（x 从 24 变成
    992 之类），而屏幕矩形不变。把两者混为一谈的断言会误判实现。

    这里只用来算"期望的屏幕矩形"，不参与生成断言 —— 参考值来自原始资产
    摆出来的位置，注释见 case_c。
    """
    pw, ph = (parent if parent else (doc.get("canvas", [1920, 1080]))[:2])
    x, y, w, h = rect
    ax, ay = anchor_offset(anchor)
    sx = { -1: x, 0: pw * 0.5 + x - w * 0.5, 1: pw - x - w }[ax]
    sy = { -1: y, 0: ph * 0.5 + y - h * 0.5, 1: ph - y - h }[ay]
    return (sx, sy, w, h)


# ------------------------------------------------------------------ A 打开
def case_a(dump_path=""):
    text = run_editor({"MYVK_EDITOR_UIED": ASSET})
    if dump_path:
        with open(dump_path, "w", encoding="utf-8") as f:
            f.write(text)

    m = last(OPEN_RE, text)
    check("A1 UIED-OPEN ok=1 widgets=%d readOnly=0" % EXPECT_WIDGETS,
          m is not None and m.group(2) == "1" and
          int(m.group(3)) == EXPECT_WIDGETS and m.group(4) == "0",
          "got=%s" % (groups(m),))

    check("A2 走的是 ctx 那条请求通道（reason=edit-request）",
          "open=1 reason=edit-request" in text, "")

    m = last(CHECK_RE, text)
    check("A3 UIED-CHECK ok=1（序列化后能原样解析回去）",
          m is not None and m.group(1) == "1" and
          int(m.group(2)) == EXPECT_WIDGETS,
          "got=%s" % (groups(m),))

    need = ["UIED-WIN", "UIED-PATH", "UIED-SAVE-BTN", "UIED-NEW-BTN",
            "UIED-TREE", "UIED-CANVAS", "UIED-INSPECTOR", "UIED-ID",
            "UIED-ANCHOR-GRID", "UIED-POS", "UIED-SIZE", "UIED-COLOR",
            "UIED-VALID"]
    missing = [t for t in need if t not in text]
    check("A4 十三个控件锚点都在 —— 面板真的画在屏幕上", not missing,
          "missing=%s" % missing)

    # 画布必须是 16:9（参考比例 1920x1080）：拉伸的设计画布会让"看起来居中"
    # 的控件在运行时偏掉 —— 而那种偏差在截图里完全看不出来。
    r = rects(text).get("CANVAS")
    if r:
        x0, y0, x1, y1 = r[0]
        ar = (x1 - x0) / max(1.0, (y1 - y0))
        check("A5 设计画布保持 16:9（实测 %.3f）" % ar, abs(ar - 16.0 / 9.0) < 0.02,
              "%.3fx%.3f" % (x1 - x0, y1 - y0))
    else:
        check("A5 设计画布保持 16:9", False, "没有 UIED-CANVAS")

    # 三栏不能互相压：树在画布左边、属性栏在画布右边（x 严格递增）
    t = rects(text).get("TREE")
    c = rects(text).get("CANVAS")
    i = rects(text).get("INSPECTOR")
    ok = bool(t and c and i) and t[0][2] <= c[0][0] and c[0][2] <= i[0][0]
    check("A6 三栏从左到右不重叠", ok,
          "tree=%s canvas=%s insp=%s" % (t, c, i))


# ------------------------------------------------------------------ B 编辑+存盘
def case_b(ctx):
    script = ";".join([
        "sel:hud/hpbar",
        "setpos:64,42",
        "setsize:280,20",
        "setcolor:#00FF00FF",
        "setradius:4",
        "sel:hud/hpnum",
        "setalign:center",
        "setfont:22",
        "dump",
        "saveas:" + ctx.b_out,
    ])
    text = run_editor({"MYVK_EDITOR_UIED": ASSET,
                       "MYVK_EDITOR_UIED_SCRIPT": script})

    if not os.path.isfile(ctx.b_out):
        check("B1 存出来的文件存在", False, ctx.b_out)
        return
    with open(ctx.b_out, "r", encoding="utf-8") as f:
        raw = f.read()
    try:
        doc = json.loads(raw)
    except Exception as e:
        check("B1 存出来的文件是合法 JSON", False, str(e))
        return

    bar = find_child(doc, "hpbar")
    check("B1 控件还在（按 id 找，不按路径）", bar is not None, "")
    if not bar:
        return

    rect = bar.get("rect", [])
    check("B2 setpos/setsize 落盘（64,42,280,20）",
          len(rect) == 4 and all(abs(rect[i] - v) < 0.01
                                 for i, v in enumerate((64, 42, 280, 20))),
          "rect=%r" % (rect,))
    check("B3 setcolor 落盘为 #00FF00FF（大写十六进制）",
          bar.get("color") == "#00FF00FF", "color=%r" % bar.get("color"))
    check("B4 setradius 落盘（4）", abs(float(bar.get("radius", -1)) - 4) < 1e-4,
          "radius=%r" % bar.get("radius"))

    num = find_child(doc, "hpnum")
    check("B5 文本绑定表达式原样保住（含 {}）",
          num is not None and "{health}" in num.get("text", ""),
          "text=%r" % (num.get("text") if num else None))
    check("B6 setalign/setfont 落盘（center / 22）",
          num is not None and num.get("align") == "center" and
          abs(float(num.get("size", 0)) - 22.0) < 1e-4,
          "align=%r size=%r" % (num.get("align") if num else None,
                                num.get("size") if num else None))

    # "//" 里那句中文必须逐字保住：在 ANSI 环境块里注入中文是注定坏的，
    # 所以中文只用**文件往返**来验（见头注释 ①）。
    check("B7 \"//\" 注释原样落盘（中文没被抹掉）",
          doc.get("//") is not None and "运行时绑定" in doc.get("//", ""),
          "comment=%r" % (str(doc.get("//"))[:40],))
    check("B8 中文没有 \\u 转义（ensure_ascii=false）", "\\u" not in raw,
          "esc=%s" % ("\\u" in raw))

    m = last(CHECK_RE, text)
    check("B9 存盘后校验仍 ok=1", m is not None and m.group(1) == "1",
          "got=%s" % (groups(m),))
    check("B10 存盘走了临时文件 + rename（UIDOC-SAVE 打到目标路径）",
          last(SAVE_RE, text) is not None,
          "got=%s" % (groups(last(SAVE_RE, text)),))


# ------------------------------------------------------------------ C 换锚点
def case_c(ctx):
    """`sel:resume;setanchor:top-right` —— 换锚点必须**保持屏幕位置不变**。

    原来的摆放：anchor=bottom-right，rect=[24,24,240,64]（距右下角各 24），
    屏幕矩形 = [1920-24-240, 1080-24-64, 240, 64] = [1656, 992, 240, 64]。

    换成 top-right 之后，宽度一样，所以"距右边 24"这个 x 恰好不用动，
    而 y 必须从"距底边 24"改成"距顶边 992" —— 文档里 rect 变成
    [24, 992, 240, 64]，屏幕矩形仍是 [1656, 992, 240, 64]。

    ⚠ 所以判据是**屏幕矩形**不变，不是文档 rect 不变。只改 anchor 不重算
    x/y 的实现会让控件从底部飞到顶部（rect 留在 [24,24]，屏幕 y 变成 24），
    C2 会抓到；文档 rect 那个值也一起断言，因为"y=992 而不是 24"正是
    逆运算算对了的直接证据。
    """
    script = ";".join([
        "sel:resume",
        "setanchor:top-right",
        "dump",
        "saveas:" + ctx.c_out,
    ])
    text = run_editor({"MYVK_EDITOR_UIED": ASSET,
                       "MYVK_EDITOR_UIED_SCRIPT": script})

    if not os.path.isfile(ctx.c_out):
        check("C1 存出来的文件存在", False, ctx.c_out)
        return
    doc = load_json(ctx.c_out)
    btn = find_child(doc, "resume")
    check("C1 锚点换成了 top-right", btn is not None and
          btn.get("anchor") == "top-right",
          "anchor=%r" % (btn.get("anchor") if btn else None))
    if not btn:
        return
    rect = [float(v) for v in btn.get("rect", [])]
    check("C2 文档 rect 重算成 24,992,240,64（y 从距底改为距顶）",
          len(rect) == 4 and all(abs(rect[i] - v) < 0.01
                                 for i, v in enumerate((24, 992, 240, 64))),
          "rect=%r" % (rect,))
    scr = screen_rect(doc, rect, btn.get("anchor")) if len(rect) == 4 else ()
    check("C3 屏幕矩形一点没动（1656,992,240,64）",
          len(scr) == 4 and all(abs(scr[i] - v) < 0.01
                                for i, v in enumerate((1656, 992, 240, 64))),
          "screen=%r" % (scr,))
    m = last(ANCHOR_RE, text)
    check("C4 打了 UIED-ANCHOR ... keep=1（记账，不只改数据）",
          m is not None and m.group(6) == "1" and
          m.group(2) == "bottom-right" and m.group(3) == "top-right" and
          abs(float(m.group(4)) - 24.0) < 0.01 and
          abs(float(m.group(5)) - 992.0) < 0.01,
          "got=%s" % (groups(m),))


# ------------------------------------------------------------------ D 坏文件
def case_d(ctx):
    # `type` 写错的资产：`parseUiDocument` 会在第一个控件直接失败。
    with open(ctx.d_bad, "w", encoding="utf-8") as f:
        f.write('{\n  "version": 1,\n  "name": "broken",\n  "widgets": [\n'
                '    { "id": "a", "type": "slider", "rect": [0,0,10,10] },\n'
                '    { "id": "b", "type": "text", "rect": [0,0,10,10] }\n'
                '  ]\n}\n')
    before = sha(ctx.d_bad)
    text = run_editor({"MYVK_EDITOR_UIED": ctx.d_bad,
                       "MYVK_EDITOR_UIED_SCRIPT": "add:panel;save"})

    m = last(OPEN_RE, text)
    check("D1 打开坏文件：ok=0 readOnly=1",
          m is not None and m.group(2) == "0" and m.group(4) == "1",
          "got=%s" % (groups(m),))

    m = last(BLOCK_RE, text)
    check("D2 Save 被拒（reason=read-only）",
          m is not None and m.group(2) == "read-only",
          "got=%s" % (groups(m),))

    check("D3 坏文件一个字节都没动（没被截成残片）",
          before == sha(ctx.d_bad), "")


# ------------------------------------------------------------------ E 从零新建
def case_e(ctx):
    text = run_editor({
        "MYVK_EDITOR_UIED": "",     # 空串 = 只请求开窗，不加载资产
        "MYVK_EDITOR_UIED_SCRIPT": "new;dump;saveas:" + ctx.e_out,
    })

    m = last(NEW_RE, text)
    check("E1 空手开窗 + New：3 个控件 / 参考分辨率 1920x1080",
          m is not None and int(m.group(1)) == 3 and m.group(2) == "1920" and
          m.group(3) == "1080",
          "got=%s" % (groups(m),))
    check("E2 空手开窗时路径是 (none)", "(none)" in text, "")

    if not os.path.isfile(ctx.e_out):
        check("E3 New 存出来的文件存在", False, ctx.e_out)
        return
    doc = load_json(ctx.e_out)
    hud = find_child(doc, "hud")
    check("E3 骨架里有面板 hud（nine）", hud is not None and
          hud.get("type") == "nine",
          "type=%r" % (hud.get("type") if hud else None))
    if not hud:
        return
    check("E4 骨架的子控件是 2 个（血条 + 数字）",
          len(hud.get("children", [])) == 2,
          "children=%d" % len(hud.get("children", [])))
    bar = find_child(doc, "hpbar")
    check("E5 骨架的血条自带绑定（bind=health / bindMax=maxHealth）",
          bar is not None and bar.get("bind") == "health" and
          bar.get("bindMax") == "maxHealth",
          "bind=%r" % (bar.get("bind") if bar else None))
    check("E6 骨架的锚点在左下（bottom-left）",
          hud.get("anchor") == "bottom-left", "anchor=%r" % hud.get("anchor"))

    m = last(CHECK_RE, text)
    check("E7 刚建出来的骨架就是可加载的（check ok=1）",
          m is not None and m.group(1) == "1", "got=%s" % (groups(m),))


# ------------------------------------------------------------------ F/G 运行时
def case_fg(ctx):
    """运行时真的拿这份文档画，而且绑定真的跟着脚本走。

    判据分两层：
      ① `UIDOC-BOUNDS` 的几何与锚点对得上（24/74 这些数在设计稿里）
      ② 脚本 `damage(30)` 之后，hpbar 的填充宽度 = 槽宽的 70%（**量几何**，
         不量颜色 —— 颜色会被 sRGB/混合/背景干扰，宽度不会）
    """
    text = run_game([
        "--scene", SCENE_DAMAGE,
        "--start-state", "playing",
        "--ui-doc", ASSET,
        "--title", "Venn UiDocTest",
        "--frames", str(GAME_FRAMES),
    ])

    m = last(LOAD_RE, text)
    check("F1 UIDOC-LOAD ok=1 widgets=%d" % EXPECT_WIDGETS,
          m is not None and m.group(2) == "1" and
          int(m.group(3)) == EXPECT_WIDGETS,
          "got=%s" % (groups(m),))

    stats = all_of(STATS_RE, text)
    check("F2 打了 UIDOC-STATS（文档真的进了绘制路径）", len(stats) > 0,
          "n=%d" % len(stats))
    if not stats:
        return
    out_w = int(stats[-1].group(5))
    out_h = int(stats[-1].group(6))
    scale = min(out_w / 1920.0, out_h / 1080.0)

    # drawn / texts 是 drawDocument 真返回的计数（曾经写死成 drawn=1，那份
    # 日志只能证明"进了绘制函数"，证明不了"画了 7 个"）。
    n_w = int(stats[-1].group(1))
    n_drawn = int(stats[-1].group(2))
    n_skip = int(stats[-1].group(3))
    n_text = int(stats[-1].group(4))
    check("F2b 7 个控件全画了、3 段文字都出了（drawn=%d texts=%d skipped=%d）"
          % (n_drawn, n_text, n_skip),
          n_drawn == EXPECT_WIDGETS and n_text == 3 and n_skip == 0 and
          n_drawn + n_skip == n_w,
          "widgets=%d drawn=%d skipped=%d texts=%d" %
          (n_w, n_drawn, n_skip, n_text))

    bounds = {}
    for b in all_of(BOUNDS_RE, text):
        bounds.setdefault(b.group(1), []).append(b)
    missing = [w for w in ("hud", "hpbar", "hpnum", "title", "resume", "topline")
               if w not in bounds]
    check("F3 六个顶层/子控件都出了几何锚点", not missing,
          "missing=%s" % missing)
    if "hud" not in bounds:
        return

    hud = bounds["hud"][0]
    x, y = float(hud.group(3)), float(hud.group(4))
    # 左下锚：x = 24×scale，y = (1080-24-74)×scale
    ex = 24.0 * scale
    ey = (1080.0 - 24.0 - 74.0) * scale
    check("F4 hud 贴在左下（x≈%.1f y≈%.1f）" % (ex, ey),
          abs(x - ex) <= 2.0 and abs(y - ey) <= 2.0, "got=(%.1f,%.1f)" % (x, y))

    # 子控件在父矩形里：hpbar 的 x = hud.x + 10×scale
    bar = bounds["hpbar"][0]
    check("F5 子控件锚点是相对父矩形（hpbar.x = hud.x + 10×scale）",
          abs(float(bar.group(3)) - (x + 10.0 * scale)) <= 2.0,
          "hpbar.x=%s hud.x=%.1f scale=%.3f" % (bar.group(3), x, scale))

    # 顶部 4px 装饰条：宽度 = 全屏，高度 = 4×scale
    top = bounds["topline"][0]
    check("F6 顶层控件按屏幕锚点铺满宽度（topline.w ≈ 屏宽）",
          abs(float(top.group(5)) - out_w) <= 2.0,
          "w=%s out_w=%d" % (top.group(5), out_w))

    # ---- G 绑定：脚本 damage(30) → health 70 → fill = 70% × 槽宽 ----
    fills = [(float(b.group(5)), float(b.group(7))) for b in bounds["hpbar"]]
    hurt = [f for f in fills if abs(f[1] - f[0] * 0.7) <= 2.0]
    check("G1 扣血 30 后血条填充 = 70% 槽宽（量几何）", len(hurt) > 0,
          "fill/槽宽: %s" % ", ".join("%.1f/%.1f" % f for f in fills))
    check("G2 血条随脚本变化 → 绑定真的通了（不是画了个静态条）",
          len(set(round(f[1], 1) for f in fills)) > 1,
          "fill 序列: %s" % [round(f[1], 1) for f in fills])
    check("G3 绑定表里有 HUD 那几个键（health/maxHealth/hint…）",
          int(stats[-1].group(7)) >= 5, "bindings=%s" % stats[-1].group(7))


# ------------------------------------------------------------------ H 截图 diff
def capture(out_path, extra_env, wait_s=15.0):
    """拍一张编辑器窗口截图。

    两处必须显式指定，否则 H 组必然假失败：

    ① `--no-resize`。不传的话 capture_window 会依次把窗口改成
       1024x640 / 1440x810 / 1280x720 试三次 —— 每张截图的客户区尺寸
       都不一样，`diff_ratio` 的第一句 `a.size != b.size` 直接返回 1.0，
       H2 基线噪声变成 100%，看起来像"编辑器不稳定"，实际是工具在动窗口。
    ② **不设 `MYVK_FRAMES`**（连空串也不设）。空串走 `std::atoi` 得 0，
       语义是"不限帧"碰巧能活，但这是靠 atoi 的巧合；显式 pop 掉才是
       "按默认配置跑"，而且父进程 shell 里万一残留了 `MYVK_FRAMES=1`，
       这一行能把截图从"窗口早退、PrintWindow 全黑"里救回来。
    """
    cmd = [sys.executable, os.path.join(HERE, "capture_window.py"),
           "--exe", EDITOR_EXE, "--title", "Venn Editor",
           "--wait", "6", "--no-resize", "--out", out_path]
    for k, v in extra_env.items():
        cmd += ["--env", "%s=%s" % (k, v)]
    env = dict(os.environ)
    env.pop("MYVK_FRAMES", None)
    try:
        subprocess.run(cmd, cwd=BIN, env=env, capture_output=True, timeout=90)
    except Exception:
        pass
    return os.path.isfile(out_path)


def diff_ratio(a_path, b_path):
    """显著变化像素占比 + 变化包围盒 + 包围盒内的变化密度（复用 verify_hud 判据）。

    ⚠ 不用整图平均差：UI 控件只占几个百分点，平均差会被稀释到没有判别力。

    三个返回值：
      sig     显著变化像素占**全图**的比例
      bbox    变化像素的包围盒 (w, h)
      density **包围盒内部**真正变了的像素占比

    为什么要 density 而不是"包围盒占全图多少"：在编辑器里改一个控件位置，
    会同时改三处 —— 画布里的控件、右栏的 X/Y 数字、校验条的文字。三点散在
    窗口的对角，包围盒**必然**覆盖大半个窗口（实测 1140x565）。所以拿包围盒
    面积当判据只会误伤。真正能区分"局部改动"和"整图重排"的是密度：整块重排
    时包围盒里几乎每个像素都变（密度近 1），零星几处改动时密度只有几个百分点。
    """
    from PIL import Image, ImageChops
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        return 1.0, (a.size[0], a.size[1]), 1.0
    d = ImageChops.difference(a, b).convert("L")
    total = a.size[0] * a.size[1]
    sig = sum(d.histogram()[9:]) / float(total)
    mask = d.point(lambda v: 255 if v > 8 else 0)
    bbox = mask.getbbox()
    if not bbox:
        return sig, (0, 0), 0.0
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    changed = mask.crop(bbox).histogram()[255]      # 阈值后只有 0 / 255
    return sig, (w, h), changed / float(max(1, w * h))


def case_h(ctx):
    base = {"MYVK_EDITOR_UIED": ASSET, "MYVK_GRID": "0"}
    ok = (capture(ctx.h_a, base) and
          capture(ctx.h_b, base) and
          capture(ctx.h_c, dict(base, MYVK_EDITOR_UIED_SCRIPT=
                                "sel:hud/hpbar;setpos:200,20")))
    if not ok:
        check("H1 三张截图都拍到了", False, "missing")
        return
    check("H1 三张截图都拍到了（同配置两遍 + 改动一遍）", True, "")

    noise, _, n_dense = diff_ratio(ctx.h_a, ctx.h_b)
    moved, bbox, density = diff_ratio(ctx.h_a, ctx.h_c)
    check("H2 同配置两遍的基线噪声很小（%.4f%% < 0.05%%）" % (noise * 100.0),
          noise < 0.0005, "noise=%.5f" % noise)
    check("H3 改一个控件位置 → 显著变化像素 > 0.5%%（实测 %.3f%%）"
          % (moved * 100.0), moved > 0.005, "moved=%.5f" % moved)
    # 分母用 1e-9 兜底；噪声恰好是 0（两次截图逐像素相同）时别把
    # 2.47e7 这个数打印出来 —— 读起来像溢出，其实是"基线为 0"。
    ratio = moved / max(noise, 1e-9)
    ratio_txt = "inf" if noise == 0 else "%.1f" % ratio
    check("H4 变化量是基线的 %s 倍（判据 >= 10）" % ratio_txt,
          moved >= 10.0 * max(noise, 1e-9),
          "ratio=%s" % ("inf（基线为 0）" if noise == 0 else ratio_txt))
    # 局部改动 vs 整图重排：看包围盒里的**密度**，不看包围盒多大（见 diff_ratio）
    check("H5 变化是零星的（包围盒内密度 %.1f%% < 15%%）" % (density * 100.0),
          density < 0.15, "bbox=%sx%s density=%.4f" % (bbox[0], bbox[1], density))
    # 兜底：万一整块画布都重排了，density 也会很高 —— 这里再给一个宽松上限
    check("H6 变化没有铺满整窗（包围盒 %.1f%% < 75%%）"
          % (bbox[0] * bbox[1] / float(1600 * 900) * 100.0),
          bbox[0] * bbox[1] < 0.75 * 1600 * 900,
          "bbox=%sx%s" % bbox)


# ------------------------------------------------------------------ 临时目录
class Ctx:
    def __init__(self, tmp):
        self.tmp = tmp
        self.b_out = join(os.path.join(tmp, "b_edited.ui.json"))
        self.c_out = join(os.path.join(tmp, "c_anchor.ui.json"))
        self.d_bad = join(os.path.join(tmp, "d_broken.ui.json"))
        self.e_out = join(os.path.join(tmp, "e_new.ui.json"))
        self.h_a = os.path.join(tmp, "h_a.png")
        self.h_b = os.path.join(tmp, "h_b.png")
        self.h_c = os.path.join(tmp, "h_c.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑某个用例（a/b/c/d/e/fg/h）")
    ap.add_argument("--keep", action="store_true", help="保留临时目录")
    ap.add_argument("--dump", default="", help="把 A 用例的完整日志写到这个文件")
    args = ap.parse_args()

    if not os.path.isfile(EDITOR_EXE):
        print("找不到 %s —— 先编译 VennEditor" % EDITOR_EXE)
        return 2

    tmp = tempfile.mkdtemp(prefix="venn_uied_")
    ctx = Ctx(tmp)
    only = args.only.lower()
    try:
        if only in ("", "a"):
            case_a(args.dump)
        if only in ("", "b"):
            case_b(ctx)
        if only in ("", "c"):
            case_c(ctx)
        if only in ("", "d"):
            case_d(ctx)
        if only in ("", "e"):
            case_e(ctx)
        if only in ("", "f", "g", "fg"):
            case_fg(ctx)
        if only in ("", "h"):
            case_h(ctx)
    finally:
        if args.keep:
            print("临时目录：%s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
        for exe in ("Editor.exe", "Game.exe"):
            try:
                subprocess.run(["taskkill", "/F", "/IM", exe],
                               capture_output=True, timeout=10)
            except Exception:
                pass

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    # ⚠ 最后一行必须是 `结果: PASS` / `结果: FAIL` —— run_regression.py 的
    #   OK_RE 只认那几种写法。只打 "42/42 PASS" 会 rc=0 却被判成 FAIL。
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
