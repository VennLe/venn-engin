"""端到端验证：时间轴轨道编辑器（#311）—— 关键帧真的能排、能拖、能存回。

==============================================================================
这一条在验什么
==============================================================================
在 #311 之前，`sequence` 页签只有 4 个控件（路径 / 时间滑条 / Preview /
只读事件树）。**运镜的正文仍然要手写 `.seq.json`** —— 想让镜头从 A 飞到 B，
作者得自己算 yaw 的弧度、手写五个浮点数。本脚本钉的是"轨道上也能排镜头"：

  A 打开：窗口画出来了（9 个控件锚点）+ 资产真解析了（5 帧 / 4 事件 / 8 秒）
  B 改一帧 + 加一帧 + 存到临时文件：文件里数值对、**度→弧度**对、ease 在、
    `"//"` 注释原样保住（这一条是"存盘不丢东西"最硬的那条）
  C 拖动重排：把第 0 帧拖到 7.0s（越过中间三帧）→ 松手后数组按 t 升序，
    而且是**同一帧被移动**（不是覆盖邻居）—— 判据是它的 pos 跟着它走
  D 打开一份坏文件 → 只读 + Save 被拒 + 坏文件没被截断
  E 空手新建 + 存盘：一帧骨架、duration=5、能加载（不是"黑屏 + no camera"）
  F 存到**场景正在用的那份** → 场景里的组件真的重读了（reloaded=1）

==============================================================================
三个刻意的取舍
==============================================================================
① **注入的是 ASCII 文本**。`MYVK_EDITOR_TLED_SCRIPT` 走 `std::getenv`，
   而 Windows 的窄环境块按 ANSI 代码页转换 —— 中文进去会变成 GBK 字节，
   于是断言测到的是"编码没配好"而不是"编辑器对不对"。中文是否原样，
   改用**更硬的**判据：把原文件里的中文往返一遍逐字节比对（B5）。

② **不用鼠标点按钮**。控件位置随内容高度漂移，而注入脚本调的 `act*` 正是
   按钮调的那一批函数（见 TimelineEditorWindow.h 的"动作"一节）——
   走同一条路，验的是真行为。矩形锚点仍然验一遍：它证明"面板真的画在
   屏幕上"，而不是只跑了逻辑。

③ **C 用例故意不点鼠标拖**，而是用 `settime` 调同一个 `actSetTime` ——
   它和拖拽走的是**同一个重排函数**（`reorderKey`）。拖动本身的手感
   （命中半径、松手才排）不在这条用例的射程内，那是人工验收的事；
   但"改完 t 数组还按升序、且身份没串"是能自动验的，也正是最容易写错的那一段。

用法：
    python tools/verify_timeline_editor.py
    python tools/verify_timeline_editor.py --only b     # 只跑某个用例
    python tools/verify_timeline_editor.py --keep       # 留下临时目录
"""

import argparse
import hashlib
import json
import math
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

# 被编辑的资产（相对 assets/）。5 帧、4 事件、duration=8。
ASSET = "sequences/opening.seq.json"
ASSET_ABS = os.path.join(BIN, "assets", ASSET)
SCENE = "assets/scenes/cutscene_demo.json"   # Director 上挂着这份过场

EXPECT_KEYS = 5
EXPECT_EVENTS = 4
EXPECT_DUR = 8.0

# 让编辑器自己退出（不认这个变量就得 kill，而"日志截到哪一帧"随机器浮动）
FRAMES = 150

# 原资产里那一大段 `"//"` 说明里的特征词（B5 拿它验"注释没被抹掉"）
COMMENT_MARK = "yaw"

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def run_editor(extra_env, wait_s=90.0):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"     # 不设它 ui::logRect 直接 return
    env["MYVK_GRID"] = "0"
    env["MYVK_FRAMES"] = str(FRAMES)
    # 自动化运行期间布局**不落盘**（MYVK_LOG_RECTS 存在即拒写）——
    # 否则这一轮的窗口位置会被下一个用例继承，位置断言互相干扰。
    for k, v in extra_env.items():
        if v is not None:
            env[k] = v

    # 上一轮崩掉的实例会抢窗口标题 → 先清一遍（与 verify_sequencer 同纪律）
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


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def join(p):
    """给注入脚本用的路径：统一正斜杠。C++ 侧两种都吃，但日志断言里
    反斜杠容易看花眼。"""
    return p.replace("\\", "/")


# ------------------------------------------------------------------ 日志解析
OPEN_RE = re.compile(
    r"TLED-OPEN path=(\S+) ok=(\d+) keys=(\d+) events=(\d+) dur=([\d.]+) "
    r"readOnly=(\d+)")
CHECK_RE = re.compile(
    r"TLED-CHECK ok=(\d+) keys=(\d+) events=(\d+) dur=([\d.]+) warns=(\d+) "
    r"err=(.*)$")
BLOCK_RE = re.compile(
    r"TLED-SAVE-BLOCKED path=(\S+) reason=(\S+)(?: err=(.*))?$")
SAVE_RE = re.compile(r"SEQ-SAVE path=(\S+) full=(\S+) keys=(\d+) events=(\d+)")
SCENE_RE = re.compile(
    r"TLED-SCENE-RELOAD path=(\S+) reloaded=(\d+) skipped=(\d+)")
DRAG_RE = re.compile(
    r"TLED-DRAG-END track=(\w+) from=(\d+) to=(\d+) t=([-\d.]+)")
REORDER_RE = re.compile(
    r"TLED-REORDER track=(\w+) from=(\d+) to=(\d+) t=([-\d.]+)")
NEW_RE = re.compile(r"TLED-NEW keys=(\d+) events=(\d+) dur=([\d.]+)")


def last(regex, text):
    hits = [m for m in (regex.search(l) for l in text.splitlines()) if m]
    return hits[-1] if hits else None


def groups(m):
    return m.groups() if m else "none"


# ------------------------------------------------------------------ 临时目录
class Ctx:
    def __init__(self, tmp):
        self.tmp = tmp
        self.b_out = join(os.path.join(tmp, "b_edited.seq.json"))
        self.c_out = join(os.path.join(tmp, "c_moved.seq.json"))
        self.d_bad = join(os.path.join(tmp, "d_broken.seq.json"))
        self.e_out = join(os.path.join(tmp, "e_new.seq.json"))


# ------------------------------------------------------------------ A 打开
def case_a(dump_path=""):
    text = run_editor({"MYVK_EDITOR_TLED": ASSET})
    if dump_path:
        with open(dump_path, "w", encoding="utf-8") as f:
            f.write(text)

    m = last(OPEN_RE, text)
    check("A1 TLED-OPEN ok=1 keys=%d events=%d dur=%.2f readOnly=0" %
          (EXPECT_KEYS, EXPECT_EVENTS, EXPECT_DUR),
          m is not None and m.group(2) == "1" and
          int(m.group(3)) == EXPECT_KEYS and
          int(m.group(4)) == EXPECT_EVENTS and
          abs(float(m.group(5)) - EXPECT_DUR) < 1e-3 and m.group(6) == "0",
          "got=%s" % (groups(m),))

    check("A2 走的是 Inspector 那条请求通道（reason=edit-request）",
          "open=1 reason=edit-request" in text, "")

    m = last(CHECK_RE, text)
    check("A3 TLED-CHECK ok=1（序列化后能原样解析回去）",
          m is not None and m.group(1) == "1" and
          int(m.group(2)) == EXPECT_KEYS,
          "got=%s" % (groups(m),))

    need = ["TLED-WIN", "TLED-PATH", "TLED-DUR", "TLED-HEAD", "TLED-CAPTURE",
            "TLED-ADDKEY", "TLED-ADDEVENT", "TLED-TRACKS", "TLED-VALID"]
    missing = [t for t in need if t not in text]
    check("A4 九个控件锚点都在 —— 面板真的画在屏幕上", not missing,
          "missing=%s" % missing)

    check("A5 轨道区自绘出来了（InvisibleButton 的矩形有记录）",
          "TLED-TRACKS" in text, "")


# ------------------------------------------------------------------ B 编辑+存盘
def case_b(ctx):
    # 原资产的五帧在 t = 0 / 2.5(ease=step) / 4.0 / 6.0 / 8.0。
    #
    #   ① sel:camera:1 是 **2.5 那一帧（ease=step）** → 挪到 3.0。
    #      用它验"改 t 不丢作者设的 ease"（B6）。选 4.0 那一帧是不行的：
    #      它的 ease 本来就是 smooth，"没被改"和"被改成默认值"看起来一样。
    #   ② sel:camera:2 是 4.0 那一帧 → 挪到 4.5、改 pos / yaw / fov。
    #   ③ addkey 打在 **3.5**（真空档）。
    #      ⚠ 不能打 6.0 —— 那里本来就有一帧，`actAddKey` 会正确地拒绝它
    #      （"这个时刻已经有关键帧了"），于是帧数不变、B1/B7 变成假阳性。
    #      改之前这两个断言看着是过的，其实什么都没测。
    script = ";".join([
        "sel:camera:1",
        "settime:3.0",
        "sel:camera:2",
        "settime:4.5",
        "setpos:7,8,9",
        "setyaw:90",
        "setfov:30",
        "head:3.5",
        "addkey",
        "dump",
        "saveas:" + ctx.b_out,
    ])
    text = run_editor({"MYVK_EDITOR_TLED": ASSET,
                       "MYVK_EDITOR_TLED_SCRIPT": script})

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

    keys = doc.get("camera", [])
    check("B1 存出来的文件是合法 JSON，且有 %d 帧 + 1 新帧" % EXPECT_KEYS,
          len(keys) == EXPECT_KEYS + 1, "keys=%d" % len(keys))

    # ⚠ 加了 6.0s 那一帧之后，第 2 帧仍然是 t=4.5 的那个 —— 这里故意
    #   不按下标找，而是**按 t 找**：按下标找的话，一旦重排写错了，
    #   断言会跟着错到一起，什么都测不出来。
    k45 = [k for k in keys if abs(float(k.get("t", -1)) - 4.5) < 1e-3]
    check("B2 settime:4.5 真的把它挪到了 4.5s",
          len(k45) == 1, "t 列表=%s" % [k.get("t") for k in keys])

    if len(k45) != 1:
        return
    k = k45[0]
    pos = k.get("pos", [])
    check("B3 pos 改对了（7,8,9）",
          len(pos) == 3 and all(abs(pos[i] - v) < 1e-4
                                for i, v in enumerate((7, 8, 9))),
          "pos=%r" % (pos,))
    # ② 度→弧度：这是全窗口最容易写错的一处换算。
    #    90° 必须落成 pi/2 ≈ 1.5708，写成 90 或 1.5708*180 都是错的。
    yaw = float(k.get("yaw", 0))
    check("B4 setyaw:90 → 文件里是 %.4f 弧度（不是 90，也不是 5156）" %
          (math.pi / 2),
          abs(yaw - math.pi / 2) < 1e-3, "yaw=%r" % yaw)
    check("B5 setfov:30 生效", abs(float(k.get("fov", 0)) - 30.0) < 1e-4,
          "fov=%r" % k.get("fov"))
    # 原来在 2.5s 那一帧的 ease 是 step —— 挪到 3.0 之后必须还是 step。
    # ⚠ 这是"编辑器不会悄悄把作者的缓动改成默认值"的唯一一条断言。
    k30 = [x for x in keys if abs(float(x.get("t", -1)) - 3.0) < 1e-3]
    check("B6 改时间没丢掉那一帧原来的 ease（2.5s 的 step 跟到了 3.0s）",
          len(k30) == 1 and k30[0].get("ease") == "step",
          "ease=%r" % (k30[0].get("ease") if k30 else None))

    check("B7 新加的 3.5s 那一帧在数组里",
          any(abs(float(x.get("t", -1)) - 3.5) < 1e-3 for x in keys),
          "t 列表=%s" % [x.get("t") for x in keys])

    # 数组必须按 t 升序（这是 `sampleCamera` 的前提，见 SequenceAsset.h ②）
    ts = [float(x.get("t", 0)) for x in keys]
    check("B8 存出来的 camera 数组按 t 升序", ts == sorted(ts), "ts=%s" % ts)

    # ① 作者的 `"//"` 说明必须原样保住 —— 不保住的话，作者第一次按 Save
    #    就无声失去了这份格式唯一的一份说明。
    check("B9 \"//\" 注释原样落盘（没被编辑器抹掉）",
          doc.get("//") is not None and COMMENT_MARK in doc.get("//", ""),
          "comment=%r" % (str(doc.get("//"))[:40]))

    # duration 是显式写出的（不是按内容推算）
    check("B10 duration 显式写出且没变（8.0）",
          abs(float(doc.get("duration", 0)) - EXPECT_DUR) < 1e-4,
          "duration=%r" % doc.get("duration"))

    check("B11 中文 payload 原样（没有 \\u 转义）",
          "\\u" not in raw, "esc=%s" % ("\\u" in raw))

    m = last(CHECK_RE, text)
    check("B12 存盘后校验仍 ok=1", m is not None and m.group(1) == "1",
          "got=%s" % (groups(m),))


# ------------------------------------------------------------------ C 拖动重排
def case_c(ctx):
    """把第 0 帧从 0.0s 拖到 7.0s（越过中间三帧），存盘看数组。

    判据分两层：
      ① 数组按 t 升序（`sampleCamera` 的前提）
      ② **是同一帧被移动**，不是"覆盖了邻居" —— 用 pos 当身份：
         第 0 帧是 [5.5,-1.2,1.90]，唯一。它必须出现在 t=7.0 那一格上。
    """
    script = ";".join([
        "sel:camera:0",
        "settime:7.0",
        "dump",
        "saveas:" + ctx.c_out,
    ])
    text = run_editor({"MYVK_EDITOR_TLED": ASSET,
                       "MYVK_EDITOR_TLED_SCRIPT": script})

    if not os.path.isfile(ctx.c_out):
        check("C1 存出来的文件存在", False, ctx.c_out)
        return
    with open(ctx.c_out, "r", encoding="utf-8") as f:
        doc = json.load(f)
    keys = doc.get("camera", [])

    check("C1 帧数没变（%d，移动不是增删）" % EXPECT_KEYS,
          len(keys) == EXPECT_KEYS, "keys=%d" % len(keys))

    ts = [float(k.get("t", 0)) for k in keys]
    check("C2 重排之后仍按 t 升序", ts == sorted(ts), "ts=%s" % ts)

    moved = [k for k in keys if abs(float(k.get("t", -1)) - 7.0) < 1e-3]
    ok_identity = False
    if len(moved) == 1:
        p = moved[0].get("pos", [])
        ok_identity = (len(p) == 3 and abs(p[0] - 5.5) < 1e-4 and
                       abs(p[1] + 1.2) < 1e-4 and abs(p[2] - 1.90) < 1e-4)
    check("C3 拖过去的是**同一帧**（pos 跟着走：[5.5,-1.2,1.90]）",
          ok_identity,
          "t=7 那一帧的 pos=%r" % (moved[0].get("pos") if moved else None))

    check("C4 原来 0.0s 那一格空了（没有留下残影）",
          not any(abs(t) < 1e-4 for t in ts), "ts=%s" % ts)

    # ⚠ 这里断言的是 `TLED-REORDER` 而不是 `TLED-DRAG-END`：后者只在
    #   鼠标松开时打，而 `settime` 走的是同一条重排函数、没有那一次松手。
    #   两条路径共用 `reorderKey`，所以断言在 REORDER 上才是对的。
    # 插入位置是算出来的，不是"最后一个"：取出第 0 项之后数组是
    #   [2.5, 4.0, 6.0, 8.0]，7.0 落在 6.0 之后、8.0 之前 → 下标 3。
    # ⚠ 这里原本写成 `EXPECT_KEYS - 1`（=4），是"拖到最右边"的想当然 ——
    #   而 8.0 那一帧还在，7.0 只能到 3。断言写错会让人以为重排错了。
    m = last(REORDER_RE, text)
    check("C5 重排走了 reorderKey（TLED-REORDER from=0 to=3）",
          m is not None and m.group(1) == "camera" and
          m.group(2) == "0" and int(m.group(3)) == 3,
          "got=%s" % (groups(m),))


# ------------------------------------------------------------------ D 坏文件
def case_d(ctx):
    # 一份 `ease` 写错的资产：`parseSequence` 会在第 0 帧直接失败，
    # 此时 cameraKeys 里可能已经有前几帧的残片（取决于错误位置）。
    with open(ctx.d_bad, "w", encoding="utf-8") as f:
        f.write('{\n  "duration": 3.0,\n  "camera": [\n'
                '    { "t": 0.0, "pos": [0,0,3], "ease": "bouncy" },\n'
                '    { "t": 1.0, "pos": [0,0,3] }\n'
                '  ]\n}\n')
    before = sha(ctx.d_bad)
    text = run_editor({"MYVK_EDITOR_TLED": ctx.d_bad,
                       "MYVK_EDITOR_TLED_SCRIPT": "addkey;save"})

    m = last(OPEN_RE, text)
    check("D1 打开坏文件：ok=0 readOnly=1",
          m is not None and m.group(2) == "0" and m.group(6) == "1",
          "got=%s" % (groups(m),))

    m = last(BLOCK_RE, text)
    check("D2 Save 被拒（reason=read-only）",
          m is not None and m.group(2) == "read-only",
          "got=%s" % (groups(m),))

    check("D3 坏文件一个字节都没动（没被截断成残片）",
          before == sha(ctx.d_bad), "")


# ------------------------------------------------------------------ E 从零新建
def case_e(ctx):
    """空手开窗 → New → 存盘。这是"第一次排一段过场"最可能走的那条路，
    也是最容易漏测的一条：面板从空表起步，那一帧骨架是**代码生成**的。"""
    text = run_editor({
        # 空串 = 只请求开窗、不加载资产（等价于从 View 菜单点开）
        "MYVK_EDITOR_TLED": "",
        "MYVK_EDITOR_TLED_SCRIPT": "new;dump;saveas:" + ctx.e_out,
    })

    check("E1 空手开窗（path=(none)）",
          "(none)" in text and "reason=edit-request" in text, "")

    m = last(NEW_RE, text)
    check("E2 TLED-NEW keys=1 events=0 dur=5.00",
          m is not None and m.group(1) == "1" and m.group(2) == "0" and
          abs(float(m.group(3)) - 5.0) < 1e-3,
          "got=%s" % (groups(m),))

    if not os.path.isfile(ctx.e_out):
        check("E3 New 存出来的文件存在", False, ctx.e_out)
        return
    with open(ctx.e_out, "r", encoding="utf-8") as f:
        doc = json.load(f)
    keys = doc.get("camera", [])
    check("E3 骨架是一帧、起点 t=0", len(keys) == 1 and
          abs(float(keys[0].get("t", -1))) < 1e-4,
          "keys=%d" % len(keys))
    check("E4 骨架的 duration 是 5.0（不是 0 —— 写 0 会退回\"按内容推算\"）",
          abs(float(doc.get("duration", 0)) - 5.0) < 1e-4,
          "duration=%r" % doc.get("duration"))

    m = last(CHECK_RE, text)
    check("E5 刚建出来的骨架就是可加载的（check ok=1）",
          m is not None and m.group(1) == "1", "got=%s" % (groups(m),))


# ------------------------------------------------------------------ F 场景刷新
def case_f(ctx):
    """存到**场景正在用的那份**路径 → 场景里的组件要重新读。

    不刷的话症状很怪：窗口里是新运镜、Save 成功、进 Play 还是旧的
    （`SequencerComponent::assetLoaded` 是"读过一次就别再读"）。
    ⚠ 这一条会临时改写 `assets/sequences/opening.seq.json`，
      跑完在 finally 里原样写回并校验哈希。
    """
    with open(ASSET_ABS, "rb") as f:
        original = f.read()
    before = hashlib.sha1(original).hexdigest()
    try:
        text = run_editor({
            "MYVK_EDITOR_TLED": ASSET,
            "MYVK_EDITOR_TLED_SCRIPT": "sel:camera:0;setyaw:12.5;save",
            "MYVK_EDITOR_SCENE": SCENE,
        })
        m = last(SCENE_RE, text)
        check("F1 存盘后场景组件重读了（reloaded>=1）",
              m is not None and int(m.group(2)) >= 1,
              "got=%s" % (groups(m),))
        # 正在播的那一段不该被打断（编辑态没在播，所以 skipped 应为 0）
        check("F2 没在播的那一份不被跳过（skipped=0）",
              m is not None and int(m.group(3)) == 0,
              "got=%s" % (groups(m),))
        check("F3 真的写进去了（SEQ-SAVE 打到这条路径）",
              last(SAVE_RE, text) is not None,
              "got=%s" % (groups(last(SAVE_RE, text)),))
    finally:
        with open(ASSET_ABS, "wb") as f:
            f.write(original)
    check("F4 资产已原样还原（哈希一致）", before == sha(ASSET_ABS), "")


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑某个用例（a/b/c/d/e/f）")
    ap.add_argument("--keep", action="store_true", help="保留临时目录")
    ap.add_argument("--dump", default="", help="把 A 用例的完整日志写到这个文件")
    args = ap.parse_args()

    if not os.path.isfile(EDITOR_EXE):
        print("找不到 %s —— 先编译 VennEditor" % EDITOR_EXE)
        return 2

    tmp = tempfile.mkdtemp(prefix="venn_tled_")
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
        if only in ("", "f"):
            case_f(ctx)
    finally:
        if args.keep:
            print("临时目录：%s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
        try:
            subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                           capture_output=True, timeout=10)
        except Exception:
            pass

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    # ⚠ 最后一行必须是 `结果: PASS` / `结果: FAIL` —— run_regression.py 的
    #   OK_RE 只认那几种写法。只打 "6/6 PASS" 会 rc=0 却被判成 FAIL。
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
