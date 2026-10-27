"""端到端验证：对话（C2 第二步 —— 组件 / 字幕框 / 时间轴接线）。

==============================================================================
这个脚本验什么 / 不验什么
==============================================================================
数值层面（解析 / 校验 / 打字机 / 播放头 / 选项 / 调度优先级）由
`Game.exe --selftest dialogue` 的 **97 条 `DLG-TEST`** 负责 —— 那边帧长钉死、
几毫秒跑完。**本脚本只验"接到一起之后还对不对"**，分两个用例：

【用例 A · 自动推进】`MYVK_DLG_AUTO=1.5`，进程靠 `--frames` 自己退出。
  ① 时间轴真的 `cue` 了对话：`SEQ-EVENT kind=Line t=4.0` → `DLG-START src=cue`
  ② 资产加载：`DLG-ASSET lines=7 start=l1`
  ③ **五句的顺序与字数**：l1/8 → l2/27 → q1/5(choices=2) → roof/14 → roof_end/12
     ⚠ 字数是**码点**（汉字算 1）。l2 的字节数是 79 —— 哪天有人把打字机改回
       按字节切，这条会从 27 变 79，而界面上只是"字变成一串问号"。
  ④ 打字机真的在动：`SUB-TYPE` 里存在 `0 < shown < total` 的采样点
  ⑤ 字幕框几何（用真实客户区尺寸算，不是抄日志）：
     等比缩放 / 水平居中 / 所有框**底边对齐**（框向上长高）/ 底边距 = 96·s
  ⑥ 选项**等正文出完才出现**：q1 先有一条 `SUB-BOX ... choices=0`，之后才有
     `choices=2`；两条 `SUB-CHOICE` 文本与资产一致、焦点在 0
  ⑦ 收尾：`DLG-CHOICE i=0 id=roof` → `DLG-END asset=... src=cue`

【用例 B · 按键推进】不设 `MYVK_DLG_AUTO`，往窗口里 PostMessage 真按键，
配 `MYVK_RUNTIME_TIMESCALE=0.5` 把打字窗口拉宽。
  ⑧ 两段式：打字中按一下 → `DLG-PRESS reveal`（**不是**"什么都没发生"）
  ⑨ 打字中按的那一下**不会顺手把选项选掉**（q1 的 reveal 早于任何 `DLG-CHOICE`）
  ⑩ ↓ 之后按空格 → 选中的是 **1 号**（走到 `sleep` 分支，不是 `roof`）
  ⑪ 换分支成功：日志里只有 `sleep`/`sleep_end`，没有 `roof`

⚠ 不验像素。字幕框"看着对不对"交给 `SUB-BOX` 的几何数字 —— 走查截图会被
  阴影 / TAA / 字体图集这些老问题污染，而且没法判"框是不是向上长高"。

==============================================================================
三个必须记住的坑
==============================================================================
**坑一：`--frames`，不是 `--max-frames`。**
`RuntimeConfig::applyCli` 末尾对未知参数**不报错**（"快捷方式里塞进来的东西
不该让游戏起不来"），所以名字拼错完全静默：进程照常跑、断言照常过 —— 只不过
它一直没退出，是 `communicate(timeout=45)` 到点后**被 kill** 的。
`verify_sequencer.py` 第一版就写成 `--max-frames`，"不靠 kill"那句注释是假的。

**坑二：不设 `MYVK_DLG_AUTO` 时，对话会永远停在第一句。**
这不是 bug —— 对话本来就该等玩家按。但"进程跑满 1000 帧一件事都没发生"，
看起来非常像"接线断了"。用例 B 必须自己按键。

**坑三：`DLG-END` 会打两条，不是重复。**
一条来自 `DialogueRunner::advance()`（只有 `steps=` / `last=`，在"判定说完"
的那一刻打），另一条来自 `DialogueSystem::noteLine()`（下一帧 `tick()` 补上
`asset=` / `src=`）。断言取**带 `src=`** 的那条 —— 只有它知道这段是时间轴起的
还是组件起的。

**坑四：`DLG-PRESS need-choice` 在运行时不可达，别拿它当判据。**
`RuntimeApp` 的分发是 `atChoice ? choose() : pressAdvance()`，而
`atChoice = p->atChoice() && p->fullyRevealed()`；`pressAdvance` 只在
`!atChoice` 时被调 —— 那时要么不在选项节点，要么正文没出完，两条路都只可能
返回 `Revealed` / `Moved`。**"正文出完 + 站在选项节点 + 调 pressAdvance"这个
组合只有外部直接调 API 才出现**，归数值侧
`Game.exe --selftest dialogue` 的 `player_q1_needchoice` 管。
（上一刀 ⑥ 那条"撒手"是同一类错误：断言了一个日志里永远不会有的行。
  判据要能失败，但也要**有可能成立**。）

**坑五：`PostMessageW` 的 down / up 之间必须真的停一下。**
两次调用在同一轮 `glfwPollEvents` 里一起被处理，GLFW 的 `window->keys[key]`
先置 PRESS 再置 RELEASE，引擎这一帧取边沿时已经是"没按下" → `wasPressed`
**永远 false**。症状极具误导性：按键像是完全没送到（`taps=8` 却一句都没走），
可窗口句柄是对的、`PostMessage` 也不报错。这里用 `hold=0.06`。

==============================================================================
两个"看起来该失败却过了"的判据（第一版踩过）
==============================================================================
· 字幕框宽度不能写死 `826.7`：那是 1280×720 客户区下 1240 设计宽 × (1280/1920)
  的结果。改成"框宽 / 客户区宽 = 设计宽 / 1920"，换分辨率也不红。
· `SUB-CHOICE i=0 focus=1` 不是"焦点反了"：日志打的是**第 i 项是不是焦点**，
  所以 i=0 得 1、i=1 得 0 恰好说明焦点在第一项。

用法：
    python tools/verify_dialogue.py [--skip-keys]
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors             # noqa: E402
from verify_shader_variants import ROOT                             # noqa: E402

GAME_EXE = os.path.join(ROOT, "build", "bin", "Game.exe")
SCENE = "assets/scenes/cutscene_demo.json"
GAME_JSON = os.path.join(ROOT, "assets", "game.json")

# 资产的真实内容（改 `assets/dialogues/ch01_open_01.dlg.json` 时要同步改这里）
DLG_NAME = "ch01_open_01"
DLG_PATH = "dialogues/ch01_open_01.dlg.json"
DLG_LINES = 7
DLG_START = "l1"
DLG_CPS = 24.0
# (id, speaker, 码点字数, 选项数) —— 字数按**码点**，不是字节
DLG_CHAIN = [
    ("l1", "千夏", 8, 0),
    ("l2", "千夏", 27, 0),
    ("q1", "千夏", 5, 2),
    ("roof", "千夏", 14, 0),
    ("roof_end", "我", 12, 0),
]
CHOICE_TEXT = ["去天台看看", "回教室再睡一会"]
CHOICE0_GOTO = "roof"          # choices[0] 指向的节点 id
CHOICE1_GOTO = "sleep"         # choices[1] 指向的节点 id

# 字幕框的设计尺寸（`runtime/ui/Subtitle.cpp` 的常量，参考 1920×1080）
SUB_REF_W = 1240.0
SUB_REF_SCREEN_W = 1920.0
SUB_BOTTOM_REF = 96.0

# 自动推进的节奏与帧数。
# ⚠ 帧数一钉，日志窗口就固定了：8 秒过场 + 4 秒之后 ~7.5 秒对话 ≈ 15.5 秒，
#   实测 ~39 fps → 660 帧刚好越过 `DLG-END`（520 帧时末句还在打字）。
AUTO_SEC = 1.5
FRAMES_AUTO = 700
FRAMES_KEYS = 1000

# 用例 B 的慢放倍率。理由：要按在"正文还没出完"的那段窗口里，而 `q1` 只有
# 5 个字（24 cps → 0.21 秒），真实速度下"看见锚点 → PostMessage 送达 → 下一帧
# 处理"这一趟有 20%~40% 概率踏空，测出来就是**偶发红**。
# 0.5 把它拉到 0.42 秒；ps：这个倍率也会把时间轴的播头一起放慢（`seq.tick`
# 吃的是 `vdt`），所以整轮从 ~16 秒变 ~24 秒，帧数预算跟着放宽。
TIME_SCALE_KEYS = 0.5

VK_SPACE = 0x20
VK_DOWN = 0x28
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202

START_RE = re.compile(r"DLG-START asset=(\S+) path=(\S+) node=(\S+) "
                      r"cps=([\d.]+) host=(\d+) src=(\S+)")
LINE_RE = re.compile(r"DLG-LINE id=(\S+) speaker=(\S+) chars=(\d+) shown=(\d+) "
                     r"choices=(\d+) asset=(\S+) src=(\S+)")
TYPE_RE = re.compile(r"SUB-TYPE id=(\S+) shown=(\d+) total=(\d+)")
BOX_RE = re.compile(r"SUB-BOX x=([-\d.]+) y=([-\d.]+) w=([-\d.]+) "
                    r"h=([-\d.]+) lines=(\d+) choices=(\d+) speaker=(\S+)")
CHC_RE = re.compile(r"SUB-CHOICE i=(\d+) focus=(\d+) "
                    r"rect=([-\d.]+),([-\d.]+),([-\d.]+),([-\d.]+) text=\[(.*)\]")
PRESS_REVEAL_RE = re.compile(r"DLG-PRESS reveal id=(\S+) chars=(\d+)/(\d+) src=(\S+)")
# ⚠ 这个锚点**故意不在断言里用**：`DLG-PRESS need-choice` 在运行时不可达
#   （见 group_keys 的说明），留着是为了排查时能在日志里认出它。
PRESS_CHOICE_RE = re.compile(r"DLG-PRESS need-choice id=(\S+) n=(\d+) src=(\S+)")
PICK_RE = re.compile(r"DLG-CHOICE i=(\d+) id=(\S+) asset=(\S+)")
# ⚠ `DLG-END` 有**两条**（见文件头坑三）。这条正则只认带 `src=` 的那条 ——
#   中间还夹着 `steps=` / `last=`，所以不能拿 "DLG-END asset=X src=cue" 做
#   子串匹配（第一版就是这么写的，永远不命中，看起来像"对话没结束"）。
END_RE = re.compile(r"DLG-END asset=(\S+) steps=(\d+) last=(\S+) src=(\S+)")
EVENT_RE = re.compile(r"SEQ-EVENT seq=(\S+) kind=(\S+) t=([\d.]+) payload=\[([^\]]*)\]")

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ---------------------------------------------------------------- 进程与窗口

KF_EXTENDED = 0x100
# 需要扩展位（lParam bit24 / HIWORD 的 0x100）的那批键。
# ⚠ 方向键在小键盘上**另有一套**（0x48/0x50/0x4B/0x4D vs 扩展的 0x148/...），
#   所以"按了方向键却没反应"这件事，只有把扩展位补上才分得清是"没送到"还是
#   "送成了小键盘的键"。
EXTENDED_VKS = (0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,   # PgUp/PgDn/End/Home/←↑→↓
                0x2D, 0x2E, 0x5B, 0x5C, 0x5D, 0x6F, 0x90, 0x91)   # Ins/Del/Win/Apps/NumLock


def _key_lparam(vk, up):
    """拼 PostMessage 的 lParam。

    ⚠⚠ **扩展位不能省。** GLFW 在 win32_window.c 里是这么取键的：
          scancode = HIWORD(lParam) & (KF_EXTENDED | 0xff);
          key      = _glfw.win32.keycodes[scancode];
      扩展键（方向键等）的真实扫描码是 `0x150` 这种带 0x100 的数；漏掉扩展位
      就变成 `0x50`，而 `keycodes[0x50]` 是小键盘的那个键 —— 于是
      `keyPressed(GLFW_KEY_DOWN)` 恒为 false。
      症状：空格（普通键）一路正常，唯独 ↑↓ 没反应，看起来像"运行时不读方向键"。
      `verify_char_locomotion` / `verify_player_jump` 只发字母和空格，所以这个
      坑一直在那儿没人踩到。
    """
    sc = user32.MapVirtualKeyW(vk, 0) & 0xFF
    if vk in EXTENDED_VKS:
        sc |= KF_EXTENDED
    lp = 1 | (sc << 16)                    # 重复次数 = 1
    if up:
        lp |= 0xC0000000                   # bit30 上一状态按下 + bit31 迁移
    return lp


def tap_key(hwnd, vk, hold=0.06):
    """按一下，`hold` 秒后松开。

    ⚠⚠ **必须真的按住一会儿**。第一版是 down 和 up 连着发、中间不 sleep ——
      两次 `PostMessageW` 在同一轮 `glfwPollEvents` 里一起被处理，GLFW 的
      `window->keys[key]` 先置 PRESS 再置 RELEASE，等引擎这一帧去取边沿时
      已经是"没按下"，于是 `wasPressed` **永远为 false**。
      症状极具误导性：日志里按键像是完全没送到（对话一句没走、`taps=8` 但
      `DLG-LINE` 只有 l1），可窗口句柄是对的、`PostMessageW` 也没报错。
      `verify_player_jump.py` 的 `tap(..., hold=0.06)` 就是这个缘故。

    ⚠ 但也不能像 `verify_char_locomotion.tap_key` 那样在末尾 `sleep(0.4)`：
      这里的判据恰恰是"按下的那一刻正文还没出完"，等一下就把打字窗口等过去
      了，`DLG-PRESS reveal` 永远不会出现。
    """
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, _key_lparam(vk, up=False))
    time.sleep(hold)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, _key_lparam(vk, up=True))


def click_viewport(hwnd, x=640, y=360):
    """点一下视口 —— 运行时的"操作态"（鼠标捕获 / WASD 生效）靠这一下进门。

    和 `verify_player_jump.py` 的 `click(hwnd, 640, 360)` 同一个理由。对话本身
    不要求捕获，但先把状态弄成"手已经放到键上"更接近真实玩家，也免得哪天
    输入门禁多加一层时这组用例莫名全红。
    """
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, lp)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)
    time.sleep(0.25)


def client_size(hwnd):
    """客户区尺寸；拿不到返回 (0, 0)。

    ⚠ 必须取**真实**客户区：字幕框的像素坐标是"设计值 × 客户区宽/1920"，
      拿日志里的绝对数字去比，换个分辨率就全红。
    """
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    return (int(rect.right), int(rect.bottom))


def run_case(env_extra, frames, script=None, wait_s=60.0, timeout=120.0):
    """跑一次 Game.exe，返回 (日志行, 客户区宽, 客户区高)。

    `script` 是回调 `fn(tap, hwnd)`，在进程运行期间于主线程执行 —— 用例 B
    靠它在正确的时刻按键。

    ⚠ 用 `LogTap`（排空线程）而不是 `communicate()`：要按键就得在主线程里等
      锚点出现，而 `Popen(stdout=PIPE)` 没人读的话，64 KB 管道一满进程就
      **永久阻塞**（症状是"跑到一半不动了"，不报错）。
    ⚠ 用 `--frames` 让进程**自己退出**，别靠 `kill()` —— 靠 kill 时"日志截到
      哪一帧"取决于墙钟，而 fps 是浮动的。
    """
    env = dict(os.environ)
    env["MYVK_LOG_DT"] = ""
    env.pop("MYVK_DLG_AUTO", None)
    env.update(env_extra)
    argv = [GAME_EXE, "--scene", SCENE, "--trace-states",
            "--frames", str(frames)]
    kill_stale_editors()
    proc = subprocess.Popen(argv, cwd=os.path.dirname(GAME_EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = 0
    try:
        # 总是找窗口：① 客户区尺寸是几何断言的依据；② 不让窗口露面时
        # `unfocusedFps=15` 会把帧率砍一半，帧数窗口就不好算了。
        hwnd = find_window("Venn Game", timeout=25.0)
        if hwnd:
            user32.ShowWindow(hwnd, 9)
        if script is not None:
            script(tap, hwnd)
        proc.wait(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=timeout)
    finally:
        w = h = 0
        if hwnd:
            w, h = client_size(hwnd)
        with tap.lock:
            lines = list(tap.lines)
        if hwnd:
            try:
                user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
            except Exception:
                pass
    return lines, w, h


def wait_anchor(tap, needle, timeout=30.0, poll=0.02):
    """等到**任意一行包含** `needle` 才返回 True。

    ⚠ 轮询间隔 20 ms 而不是 250 ms：`l1` 只有 8 个字、24 cps = 0.33 秒的
      打字窗口，反应慢一点就只能读到"已经出完了"，`DLG-PRESS reveal` 会缺席。
    """
    end = time.time() + timeout
    while time.time() < end:
        if tap.has_line(needle):
            return True
        time.sleep(poll)
    return False


# ---------------------------------------------------------------- 用例 A

def group_auto():
    lines, cw, ch = run_case({"MYVK_DLG_AUTO": str(AUTO_SEC)}, FRAMES_AUTO)
    txt = "\n".join(lines)

    # ① 时间轴 cue 到对话
    line_ev = [m for m in (EVENT_RE.search(l) for l in lines)
               if m and m.group(2) == "Line"]
    cue_ok = (len(line_ev) == 1
              and abs(float(line_ev[0].group(3)) - 4.0) < 0.01
              and line_ev[0].group(4) == DLG_NAME)
    starts = [m for m in (START_RE.search(l) for l in lines) if m]
    start = starts[0] if starts else None
    check("① 时间轴 kind=Line 真的 cue 了对话",
          cue_ok and start is not None and start.group(6) == "cue"
          and start.group(1) == DLG_NAME,
          "event=%s start=%s" %
          ((line_ev[0].group(3), line_ev[0].group(4)) if line_ev else None,
           start.groups() if start else None))

    # ② 资产加载
    check("② 资产解析（lines / start）",
          ("DLG-ASSET name=%s lines=%d start=%s" %
           (DLG_NAME, DLG_LINES, DLG_START)) in txt,
          "want lines=%d start=%s" % (DLG_LINES, DLG_START))

    # ③ 五句顺序与字数（码点）
    got = [(m.group(1), m.group(2), int(m.group(3)), int(m.group(5)))
           for m in (LINE_RE.search(l) for l in lines) if m]
    want = [(i, sp, n, c) for (i, sp, n, c) in DLG_CHAIN]
    chain_ok = got[:len(want)] == want
    check("③ 五句顺序 + 码点字数（l2 = 27 字 / 79 字节）", chain_ok,
          "got=%s" % (got,))

    # src 必须一律是 cue：这段是时间轴起的，不是实体上的 autoStart
    srcs = set(m.group(7) for m in (LINE_RE.search(l) for l in lines) if m)
    check("③b 全部台词 src=cue（不是组件 autoStart）",
          srcs == {"cue"}, "srcs=%s" % (sorted(srcs),))

    # ④ 打字机在动
    typed = {}
    for l in lines:
        m = TYPE_RE.search(l)
        if m:
            typed.setdefault(m.group(1), []).append(
                (int(m.group(2)), int(m.group(3))))
    partial = [(k, v) for k, v in typed.items()
               if any(0 < s < t for s, t in v)]
    mono = all(v == sorted(v) for _, v in typed.items())
    l2_total = [t for s, t in typed.get("l2", [])]
    check("④ 打字机 shown 递增且 total = 码点数",
          bool(partial) and mono and l2_total and set(l2_total) == {27},
          "partial=%s l2_total=%s mono=%s" %
          ([k for k, _ in partial], sorted(set(l2_total)), mono))

    # ⑤ 字幕框几何
    boxes = [(float(m.group(1)), float(m.group(2)), float(m.group(3)),
              float(m.group(4)), int(m.group(5)), int(m.group(6)))
             for m in (BOX_RE.search(l) for l in lines) if m]
    if not boxes:
        check("⑤ 字幕框几何（缩放 / 居中 / 底边对齐）", False, "零条 SUB-BOX")
    else:
        # 客户区拿不到就退回 game.json 的配置尺寸（并写进 detail，别静默）
        if cw < 100 or ch < 100:
            import json
            with open(GAME_JSON, encoding="utf-8") as f:
                gj = json.load(f)
            cw, ch = int(gj["width"]), int(gj["height"])
            src = "game.json"
        else:
            src = "client"
        bx, by, bw, bh = boxes[0][:4]
        s = bw / SUB_REF_W
        want_s = cw / SUB_REF_SCREEN_W
        bottom = [round(b[1] + b[3], 1) for b in boxes]
        gap_ok = abs((ch - (by + bh)) - SUB_BOTTOM_REF * s) < 1.5
        center_ok = abs(bx - (cw - bw) / 2.0) < 1.0
        flat_ok = (max(bottom) - min(bottom)) < 0.5
        # 框是**向上长高**的：底边钉死、顶边上移
        grows = max(b[3] for b in boxes) - min(b[3] for b in boxes) > 8.0
        check("⑤ 字幕框几何（缩放 / 居中 / 底边对齐）",
              abs(s - want_s) < 1e-3 and center_ok and flat_ok and gap_ok
              and grows,
              "size=%dx%d(%s) s=%.4f want=%.4f btm=%s gap_ok=%s "
              "center=%s flat=%s grows=%s" %
              (cw, ch, src, s, want_s, sorted(set(bottom)), gap_ok,
               center_ok, flat_ok, grows))

    # ⑥ 选项：等正文出完才出现 + 两条文本 + 焦点在 0
    q1_boxes = []
    seen_q1 = False
    for l in lines:
        m = BOX_RE.search(l)
        if not m:
            if LINE_RE.search(l) and LINE_RE.search(l).group(1) == "q1":
                seen_q1 = True
            continue
        if seen_q1:
            q1_boxes.append(int(m.group(6)))
    choices = [(int(m.group(1)), int(m.group(2)), m.group(7))
               for m in (CHC_RE.search(l) for l in lines) if m]
    texts = [c[2] for c in choices]
    focus0 = [c for c in choices if c[1] == 1]
    check("⑥ 选项在正文出完之后才出现（两条、焦点在 0）",
          q1_boxes[:1] == [0] and 2 in q1_boxes
          and texts == CHOICE_TEXT
          and len(focus0) == 1 and focus0[0][0] == 0,
          "q1_boxes=%s texts=%s focus_on=%s" %
          (q1_boxes, texts, [c[0] for c in focus0]))

    # ⑦ 收尾
    picks = [m for m in (PICK_RE.search(l) for l in lines) if m]
    ends = [m for m in (END_RE.search(l) for l in lines) if m]
    end_ok = (bool(ends) and ends[0].group(1) == DLG_NAME
              and ends[0].group(4) == "cue"
              and ends[0].group(3) == DLG_CHAIN[-1][0])
    check("⑦ 选完接 roof、说完 DLG-END",
          bool(picks) and picks[0].group(2) == CHOICE0_GOTO and end_ok,
          "picks=%s end=%s" %
          ([p.groups() for p in picks], ends[0].groups() if ends else None))


# ---------------------------------------------------------------- 用例 B

def wait_subbox_choices(tap, n=2, timeout=10.0):
    """等 `SUB-BOX ... choices=N` 出现。

    ⚠ 不能拿 `"choices=2"` 当子串去等：`DLG-LINE id=q1 ... choices=2` 也含这个
      子串，于是"刚进这一句"就被判成"选项面板出来了"，后面那一下按空格的
      语义就错了（本来想测"打字中按=补完"，结果在正文已出完的节点上按成了
      选定）。
    """
    needle = "SUB-BOX"
    tail = "choices=%d" % n
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for l in tap.lines:
                if needle in l and tail in l:
                    return True
        time.sleep(0.02)
    return False


def group_keys():
    """按键推进：两段式 / 焦点 / 换分支。

    ⚠⚠ **`DLG-PRESS need-choice` 在这里断言不到，因为它运行时不可达。**
      `RuntimeApp` 的分发是 `atChoice ? choose() : pressAdvance()`，而
      `atChoice = p->atChoice() && p->fullyRevealed()`；`pressAdvance` 只在
      `!atChoice` 时被调，那时要么不在选项节点（露 Revealed/Moved），要么正文
      没出完（露 Revealed）。所以"正文出完 + 站在选项节点 + 调 pressAdvance"
      这个组合只有**外部直接调 API** 才会出现 —— 由数值侧
      `Game.exe --selftest dialogue` 的 `player_q1_needchoice` 负责。
      （上一刀 ⑥ 那条"撒手"也是同一类错误：断言了一个日志里永远不会有的行。
        判据要能失败，但也要**有可能成立**。）

      于是这里改判两件**运行时真的会走**、而且以前没人验过的事：
        · 打字中按空格 **不会顺手把选项选掉**（q1 的 reveal 必须早于 CHOICE）
        · ↑↓ 真的改变了选中的下标（选中 1 号 → 走到 sleep 分支，不是 roof）
    """
    names = [
        "⑧ 打字中按空格 = 补完（DLG-PRESS reveal）",
        "⑨ 打字中按空格不会顺手选掉选项",
        "⑩ ↓ 移焦点后选中的是 1 号",
        "⑪ 换分支成功（走 sleep，不是 roof）",
    ]
    if not os.path.isfile(GAME_EXE):
        for n in names:
            check(n, False, "缺 Game.exe")
        return

    state = {"taps": 0}

    def script(tap, hwnd):
        if not hwnd:
            return
        click_viewport(hwnd)
        # ① 等到第一句出现，立刻按。l1 有 8 个字，`--time-scale 0.5` 下打字窗口
        #    0.67 秒 —— 够"看见锚点 → 按下去"这一趟。
        if not wait_anchor(tap, "DLG-LINE id=" + DLG_START, timeout=40.0):
            return
        tap_key(hwnd, VK_SPACE)
        state["taps"] += 1
        # ② 用 0.18 秒的节拍一路按到 q1。
        #    ⚠ 节拍必须**短于**最短的打字窗口，否则"q1 刚出现"这件事要等到下
        #      一拍才被自己发现，而那时 q1 可能已经出完了（它的窗口是全链最短的，
        #      5 个字）。
        end = time.time() + 30.0
        last = 0.0
        while time.time() < end:
            if tap.has_line("DLG-LINE id=q1"):
                break
            if time.time() - last > 0.18:
                tap_key(hwnd, VK_SPACE)
                last = time.time()
                state["taps"] += 1
            time.sleep(0.02)
        # ③ q1 刚出现 → 立刻按 → 正文还没出完 → 只能是 reveal
        tap_key(hwnd, VK_SPACE)
        state["taps"] += 1
        # ④ 等选项面板（正文出完 && atChoice）
        if not wait_subbox_choices(tap, 2, timeout=10.0):
            return
        time.sleep(0.3)
        # ⑤ ↓ 把焦点移到第二项（"回教室再睡一会"），再空格选定
        tap_key(hwnd, VK_DOWN)
        time.sleep(0.35)
        tap_key(hwnd, VK_SPACE)

    # ⚠ 变量名带 `MYVK_RUNTIME_` 前缀（和 `MYVK_RUNTIME_TRACE_STATES` 一族），
    #   命令行那个则是 `--time-scale`（带连字符）—— 两处都容易写错。
    env = {"MYVK_RUNTIME_TIMESCALE": str(TIME_SCALE_KEYS)}
    lines, _, _ = run_case(env, FRAMES_KEYS, script=script, wait_s=80.0)

    reveals = [(m.group(1), m.group(2), m.group(3))
               for m in (PRESS_REVEAL_RE.search(l) for l in lines) if m]
    check(names[0], bool(reveals)
          and all(int(a) == int(b) for _, a, b in reveals),
          "reveals=%s taps=%d" % (reveals, state["taps"]))

    # ⑨ reveal(q1) 的行号必须早于第一条 DLG-CHOICE
    idx_reveal = next((i for i, l in enumerate(lines)
                       if (PRESS_REVEAL_RE.search(l) or [None])[0]
                       and PRESS_REVEAL_RE.search(l).group(1) == "q1"), -1)
    idx_pick = next((i for i, l in enumerate(lines) if PICK_RE.search(l)), -1)
    check(names[1],
          idx_reveal >= 0 and idx_pick > idx_reveal,
          "reveal_q1@%d pick@%d" % (idx_reveal, idx_pick))

    picks = [m for m in (PICK_RE.search(l) for l in lines) if m]
    focus1 = [l for l in lines if CHC_RE.search(l)
              and CHC_RE.search(l).group(1) == "1"
              and CHC_RE.search(l).group(2) == "1"]
    check(names[2],
          bool(picks) and picks[0].group(1) == "1" and bool(focus1),
          "picks=%s focus1=%d" % ([p.groups() for p in picks], len(focus1)))

    line_ids = set(m.group(1) for m in (LINE_RE.search(l) for l in lines) if m)
    went = CHOICE0_GOTO not in line_ids and CHOICE1_GOTO in line_ids
    check(names[3], went, "ids=%s" % (sorted(line_ids),))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-keys", action="store_true",
                    help="只跑自动推进那一组（调字幕框时用，快一半）")
    ap.add_argument("--skip-auto", action="store_true",
                    help="只跑按键那一组（调输入时序时用）")
    args = ap.parse_args()

    print("== 对话（C2 第二步）端到端 ==")
    if not args.skip_auto:
        group_auto()
    if not args.skip_keys:
        group_keys()

    bad = [r for r in results if not r[1]]
    print("-" * 66)
    for name, ok, detail in results:
        print("%s  %s" % ("PASS" if ok else "FAIL", name))
    if bad:
        print("结果: FAIL: %d 项未通过" % len(bad))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
