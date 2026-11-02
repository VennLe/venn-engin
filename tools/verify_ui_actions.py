#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""端到端验证：UI 文档的按钮**真的能点**（`UiWidget::action`，2026-11-02）。

背景：在 2026-11-02 之前 `.ui.json` 里的按钮是**纯装饰** —— `drawDocument`
会把它画出来，但没有任何命中测试，`UiWidget` 里也没有 `action` 这个字段
（`grep -rn action engine/ui2/` → 0）。唯一能点的 `UiButton` 服务的是暂停
菜单那条**硬编码**路径，跟文档层完全不通。这个脚本守的就是"作者摆出来的
按钮点得动"这件事，从**四个不同的入口**各验一遍：

  A. **无窗口自检**（`Editor.exe` + `MYVK_UI2_TEST=1`，毫秒级）
     解析（`flag:/toggle:/sound:/quit/写错`）、命中测试（最上面的赢 / 面板不
     可交互 / disabled 不命中 / 没配 action 的 Button 仍可交互）、按下松开
     （同一控件才算点击 / 拖出去不算 / 注入点击）、条件可见（隐藏=不产出记录
     / 设计视图忽略 visibleBind）、序列化往返、坏动作只降级。
     ⚠ 用 `pass >= 109` 当下限而不是精确值（新增断言不该让脚本变红），
       同时钉住 13 个**关键用例名** —— 删光断言仍 fail=0 这个洞必须堵住。

  B. **注入点击**（`MYVK_RUNTIME_UICLICK` 参考像素，一次启动跑完 9 下）
     每个动词走一遍，并把三种"看起来一样"的情况分开：
     · `flag:`   → `UIDOC-ACTION kind=flag key=menuStart value=1.0000`
     · `toggle:` → 连点两次必须 `value=1` 然后 `value=0`（**不是**两次都 1）
     · `sound:`  → `kind=sound clip=audio/click.wav`
     · 没配 action 的按钮点两次 → `UIDOC-NOACTION` **只有一条**（不刷屏）
     · `enabled=false` 的按钮 → `hit=0`，且 `shouldNotFire` 一次都不出现
     · 点空白 → `UIDOC-CLICK hit=0 x= y=`（"点歪了"与"按钮没画"必须分得开）

  C. **条件可见**（同一次启动）
     `visibleBind` 的那个面板在旗标置起来**之前不进** `UIDOC-BOUNDS`，
     之后进 —— 判据是 `UIDOC-STATS widgets=7 skipped=1` 变成 `9 / 0`。
     ⚠ 不能只断言"最后有 panel_secret"：那样"永远显示"的实现也会绿。

  D. **quit 动词**（另起一次）
     `UIDOC-ACTION kind=quit` → `GAME-STATE ... shutting down`。
     对照组是 B/C 那次（点遍所有按钮但没点 quit）—— 它必须**没有**这一条，
     否则这条断言只是在量"每次跑完都会打一行"。

  E. **真鼠标**（起窗口，SetCursorPos + mouse_event）
     ⚠⚠ 这一组守的是**另一条代码路径**：注入点击走排队，不看 `m_mouseCaptured`；
       真鼠标要先过 `tickUi` 的"点击视口 → 进入操作态"。加文档交互之前那里
       是**无条件捕获**的，于是"注入能点、鼠标点不动"—— 只有这一组能抓到。
     判据是一对：点按钮 → `GAME-MOUSE captured=0 reason=ui-doc id=btn_flag`；
     点空白 → `GAME-MOUSE captured=1 reason=click`（照旧进入操作态）。

用法：
    python tools/verify_ui_actions.py             # 五组全跑
    python tools/verify_ui_actions.py --only AB   # 只跑指定组

⚠ 结尾一定是 `结果: PASS` / `结果: FAIL: ...`（回归脚本取**最后一条**）。
"""

import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import find_window, user32                     # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors             # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground   # noqa: E402
from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_DIR = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN_DIR, "Editor.exe")
GAME = os.path.join(BIN_DIR, "Game.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

# 夹具：源在 assets/（跟工程一起走），运行时读的是 build/bin 下那份。
# ⚠ 新加资产**不会自动进 build/bin**（增量构建只拷"变了的"），所以这里
#   每次运行都同步一次 —— 少了这一步，"改了夹具没效果"会被读成"产品坏了"。
DOC_SRC = os.path.join(ROOT, "assets", "ui", "docs", "action_demo.ui.json")
DOC_REL = "ui/docs/action_demo.ui.json"

# ⚠ 窗口标题必须**唯一**：`find_window` 是前缀匹配，运行时的默认标题
#   "Venn" 会同时命中 "Venn Editor"。
TITLE = "VennUIActionTest"

# 点空白用的参考像素（夹具里所有控件都在 x<940 / y<640 之内）
BLANK_REF = (1500.0, 950.0)

# 必须存在的自检用例名 —— 少了任何一条就说明"这一块被删了"
REQUIRED_TESTS = [
    "uiint_parse_flag", "uiint_parse_toggle", "uiint_parse_sound",
    "uiint_parse_quit", "uiint_parse_bad_verb", "uiint_parse_bad_number",
    "uiint_hit_button", "uiint_hit_disabled", "uiint_hit_panel_not_interactive",
    "uiint_hit_button_without_action", "uiint_click_same_widget",
    "uiint_click_drag_out", "uiint_click_injected",
    "uiint_visbind_hidden", "uiint_visbind_shown",
    "uiint_roundtrip_write", "uiint_bad_action_warns_not_fails",
]

TEST_RE = re.compile(r"UI2-TEST\s+(\S+)\s+actual=(.+?)\s+expected=(.+?)\s+"
                     r"tol=(\S+)\s+result=(PASS|FAIL)\s*$")
TOTAL_RE = re.compile(r"UI2-TOTAL\s+pass=(\d+)\s+fail=(\d+)")

CLICK1_RE = re.compile(
    r"UIDOC-CLICK hit=1 id=(\S+) x=([\d.-]+) y=([\d.-]+) action=(\S+)")
CLICK0_RE = re.compile(r"UIDOC-CLICK hit=0 x=([\d.-]+) y=([\d.-]+)")
DRAGOUT_RE = re.compile(r"UIDOC-CLICK hit=0 id=(\S+) reason=drag-out")
ACTION_RE = re.compile(r"UIDOC-ACTION (.*)$")
FLAG_RE = re.compile(r"kind=flag key=(\S+) value=([\d.-]+)")
TOGGLE_RE = re.compile(r"kind=toggle key=(\S+) value=([\d.-]+)")
SOUND_RE = re.compile(r"kind=sound clip=(\S+) ok=(\d)")
QUIT_RE = re.compile(r"kind=quit")
NOACTION_RE = re.compile(r"UIDOC-NOACTION id=(\S+)")
STATS_RE = re.compile(r"UIDOC-STATS widgets=(\d+) drawn=(\d+) skipped=(\d+) "
                      r"texts=(\d+) out=(\d+)x(\d+) bindings=(\d+)")
BOUNDS_RE = re.compile(r"UIDOC-BOUNDS id=(\S+) type=(\S+) x=([\d.-]+) "
                       r"y=([\d.-]+) w=([\d.-]+) h=([\d.-]+) fill=([\d.-]+)")
MOUSE_RE = re.compile(r"GAME-MOUSE captured=(\d) reason=(\S+)(?: id=(\S+))?")
SHUTDOWN = "GAME-STATE stack empty / quit requested"

fails = []


def check(name, ok, detail=""):
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                        ("  (%s)" % detail) if detail else ""))
    if not ok:
        fails.append(name)


# ============================================================
# 公共
# ============================================================

def clean_env(**extra):
    """剥掉全部 MYVK_* 再叠 extra —— 测试的输入必须自述。"""
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update({k: str(v) for k, v in extra.items()})
    return env


def sync_fixture():
    """把夹具同步到 build/bin（运行时的资产根）。"""
    dst_dir = os.path.join(BIN_DIR, "assets", "ui", "docs")
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, os.path.basename(DOC_SRC))
    shutil.copy2(DOC_SRC, dst)
    return dst


def load_widgets():
    """夹具里的顶层控件（id → 定义）。

    ⚠ 点击坐标**从夹具里读**，不在脚本里抄一份：抄一份之后改夹具的 rect
      而忘了改脚本，症状是"点了没反应"，而日志里 `hit=0` 与"按钮没画"
      长得一样。读出来就不可能不同步。
    """
    with open(DOC_SRC, encoding="utf-8") as f:
        doc = json.load(f)
    out = {}
    for w in doc["widgets"]:
        out[w["id"]] = w
    return doc, out


def center(w):
    """顶层控件的中心（参考像素）。"""
    x, y, ww, hh = w["rect"]
    return (x + ww / 2.0, y + hh / 2.0)


def run_frames(clicks, frames, timeout=240):
    """跑满 frames 帧自退。clicks 是**参考像素**。"""
    env = clean_env(MYVK_RUNTIME_UI_DOC=DOC_REL,
                    MYVK_RUNTIME_UICLICK=";".join("%g,%g" % c for c in clicks))
    p = subprocess.run([GAME, "--frames", str(frames),
                        "--start-state", "playing", "--title", TITLE],
                       cwd=BIN_DIR, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout)
    return p.returncode, p.stdout.splitlines()


def grep(lines, regex):
    return [m for m in (regex.search(l) for l in lines) if m]


def lines_with(lines, needle):
    return [l for l in lines if needle in l]


def idx_of(lines, needle, start=0):
    for i in range(start, len(lines)):
        if needle in lines[i]:
            return i
    return -1


def client_size(hwnd):
    r = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return max(1, r.right - r.left), max(1, r.bottom - r.top)


def real_click(hwnd, cx, cy):
    """合成一次真实左键点击（客户区坐标）。

    ⚠ 起手要预热：上一次动作（ShowWindow / 别的脚本）常把光标甩到客户区
      之外，第一下 SetCursorPos 会被吞掉，点击就落在别处。两次 MOVE +
      每步一点等待，是这里唯一稳定的写法。
    ⚠ 必须用 `mouse_event`，不用 `PostMessage`：验证过 PostMessage 的
      左键在 GLFW 下 0/3 命中（见 MEMORY「点菜单必须 mouse_event」）。
    """
    sx, sy = client_to_screen(hwnd, cx, cy)
    for _ in range(2):
        user32.SetCursorPos(sx, sy)
        time.sleep(0.18)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.16)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.45)


def launch_window():
    proc = subprocess.Popen([GAME, "--title", TITLE, "--start-state", "playing"],
                            cwd=BIN_DIR,
                            env=clean_env(MYVK_RUNTIME_UI_DOC=DOC_REL),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window(TITLE, timeout=25.0)
    if not hwnd:
        proc.kill()
        return proc, None, tap
    user32.ShowWindow(hwnd, 9)
    # 摆到 (0,0) + 固定尺寸：两次启动的客户区差几十像素，点击坐标就全漂了
    user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 760, 0x0002 | 0x0004)
    time.sleep(2.0)
    return proc, hwnd, tap


def close_window(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.5)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


def wait_for(tap, regex, pred, timeout=15.0, start=0):
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            raw = list(tap.lines)
        hits = [m for m in (regex.search(l) for l in raw[start:])
                if m and pred(m)]
        if hits:
            return hits[-1]
        time.sleep(0.15)
    return None


# ============================================================
# A. 无窗口自检
# ============================================================

def case_a():
    print("\n[A] 无窗口自检：Editor.exe + MYVK_UI2_TEST=1")
    env = dict(os.environ)
    env["MYVK_UI2_TEST"] = "1"
    p = subprocess.run([EDITOR], cwd=BIN_DIR, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=180)
    lines = (p.stdout or "").splitlines()
    check("[A] 退出码 0", p.returncode == 0, "rc=%d" % p.returncode)

    total = None
    for l in lines:
        m = TOTAL_RE.search(l)
        if m:
            total = (int(m.group(1)), int(m.group(2)))
    check("[A] 打出 UI2-TOTAL", total is not None, str(total))
    if not total:
        return
    check("[A] fail=0", total[1] == 0, "fail=%d" % total[1])
    # 下限而不是精确值；新加断言不该让脚本变红
    check("[A] pass >= 109", total[0] >= 109, "pass=%d" % total[0])

    seen = {}
    for l in lines:
        m = TEST_RE.search(l)
        if m and m.group(1).startswith("uiint_"):
            seen[m.group(1)] = m.group(5)
    bad = [k for k, v in seen.items() if v != "PASS"]
    check("[A] 交互用例条数 >= 23", len(seen) >= 23, "n=%d" % len(seen))
    check("[A] 交互用例全 PASS", not bad, str(bad))
    missing = [n for n in REQUIRED_TESTS if n not in seen]
    check("[A] 关键用例都在（缺 %d）" % len(missing), not missing,
          str(missing))


# ============================================================
# B + C. 注入点击：动词全覆盖 + 条件可见
# ============================================================

def case_bc(widgets):
    print("\n[B] 注入点击：五种动词 / 禁用 / 点歪 / NOACTION 不刷屏")

    clicks = [
        center(widgets["btn_flag"]),      # 1 flag:
        center(widgets["btn_toggle"]),    # 2 toggle: → 1
        center(widgets["btn_toggle"]),    # 3 toggle: → 0
        center(widgets["btn_none"]),      # 4 没配 action
        center(widgets["btn_secret"]),    # 5 flag:secret=1（C 组用）
        BLANK_REF,                        # 6 点空白
        center(widgets["btn_none"]),      # 7 再点一次没配 action 的
        center(widgets["btn_sound"]),     # 8 sound:
        center(widgets["btn_disabled"]),  # 9 enabled=false
    ]
    # 9 下点击落在第 20/50/80/... 帧（每 30 帧一发）→ 最后一发在 260 帧，
    # 再加一次 `UIDOC-STATS` 的 60 帧节流采样（300）留余量。
    rc, lines = run_frames(clicks, 340)
    check("[B] 夹具跑完（rc=0）", rc == 0, "rc=%d" % rc)

    stats = grep(lines, STATS_RE)
    check("[B] 打出 UIDOC-STATS", len(stats) > 0, "n=%d" % len(stats))
    if not stats:
        return lines
    outW, outH = int(stats[0].group(5)), int(stats[0].group(6))
    scale = outW / 1920.0
    print("    out=%dx%d scale=%.4f" % (outW, outH, scale))

    # ---- 夹具与脚本同源守卫 ----
    # 夹具里 btn_flag 的 rect 是参考像素，输出像素必须是 ref*scale。
    # 差超过 1px 说明"夹具改了 / scale 口径变了"，后面的坐标断言都不再可信。
    bf = [m for m in grep(lines, BOUNDS_RE) if m.group(1) == "btn_flag"]
    if bf:
        exp = widgets["btn_flag"]["rect"]
        got = tuple(float(bf[0].group(i)) for i in (3, 4, 5, 6))
        ok = (abs(got[0] - exp[0] * scale) <= 1.0 and
              abs(got[1] - exp[1] * scale) <= 1.0 and
              abs(got[2] - exp[2] * scale) <= 1.0 and
              abs(got[3] - exp[3] * scale) <= 1.0)
        check("[B] btn_flag 的屏幕矩形 = rect×scale（夹具口径一致）", ok,
              "got=(%.1f,%.1f,%.1fx%.1f) expect=(%.1f,%.1f,%.1fx%.1f)"
              % (got + (exp[0] * scale, exp[1] * scale, exp[2] * scale,
                        exp[3] * scale)))
    else:
        check("[B] 打出 btn_flag 的 UIDOC-BOUNDS", False)

    hits1 = grep(lines, CLICK1_RE)
    hits0 = grep(lines, CLICK0_RE)
    print("    点击命中 %d 次 / 未命中 %d 次" % (len(hits1), len(hits0)))

    # ---- B1 flag: ----
    f1 = [m for m in hits1 if m.group(1) == "btn_flag"]
    check("[B1] 点 btn_flag → hit=1（命中测试通）", len(f1) == 1,
          str([m.group(1) for m in hits1]))
    check("[B1] action 原文经日志传出（action=flag:menuStart=1）",
          bool(f1) and f1[0].group(4) == "flag:menuStart=1",
          f1[0].group(4) if f1 else "没等到")
    fa = [m for m in grep(lines, FLAG_RE) if m.group(1) == "menuStart"]
    check("[B1] UIDOC-ACTION kind=flag key=menuStart value=1.0000",
          bool(fa) and abs(float(fa[0].group(2)) - 1.0) < 1e-6,
          fa[0].group(0) if fa else "没等到")

    # ---- B2 toggle: 连点两次必须回 0 ----
    tg = [m for m in grep(lines, TOGGLE_RE) if m.group(1) == "musicOn"]
    seq = [m.group(2) for m in tg]
    check("[B2] toggle 连点两次 = 1 → 0（不是两次都 1）",
          seq == ["1", "0"], "seq=%s" % seq)

    # ---- B3 sound: ----
    sd = grep(lines, SOUND_RE)
    check("[B3] sound: → kind=sound clip=audio/click.wav",
          bool(sd) and sd[0].group(1) == "audio/click.wav",
          sd[0].group(0) if sd else "没等到")
    # ⚠ 不断言 ok=1：没有声卡的机器上 `playOneShot` 恒返回 false（正常工况，
    #   见 AudioSystem::playOneShot）。这里守的是"派发到了 sound 分支"。
    if sd:
        print("    （本机声卡 ok=%s —— 不判：无声卡恒 0 是正常工况）"
              % sd[0].group(2))

    # ---- B4 禁用控件不命中 ----
    dis = center(widgets["btn_disabled"])
    expd = (dis[0] * scale, dis[1] * scale)
    on_dis = [m for m in hits0
              if abs(float(m.group(1)) - expd[0]) <= 2.0 and
              abs(float(m.group(2)) - expd[1]) <= 2.0]
    check("[B4] enabled=false 的按钮 hit=0（禁用仍命中最糟）",
          len(on_dis) == 1, "n=%d expect=1" % len(on_dis))
    check("[B4] 禁用控件没触发动作（shouldNotFire 一次都没有）",
          not lines_with(lines, "shouldNotFire"))

    # ---- B5 点空白也要留痕 ----
    blank = [m for m in hits0
             if abs(float(m.group(1)) - BLANK_REF[0] * scale) <= 2.0 and
             abs(float(m.group(2)) - BLANK_REF[1] * scale) <= 2.0]
    check("[B5] 点空白 → UIDOC-CLICK hit=0 且坐标对得上（「点歪了」可分）",
          len(blank) == 1,
          "(%s,%s) expect=(%.1f,%.1f)"
          % (blank[0].group(1), blank[0].group(2),
             BLANK_REF[0] * scale, BLANK_REF[1] * scale) if blank else "没等到")

    # ---- B6 NOACTION 只打一次 ----
    na = grep(lines, NOACTION_RE)
    check("[B6] 没配 action 的按钮点了两次 → UIDOC-NOACTION 只有一条",
          len(na) == 1 and na[0].group(1) == "btn_none",
          "%d 条 %s" % (len(na), [m.group(1) for m in na]))

    # ---- C 条件可见 ----
    print("\n[C] 条件可见（visibleBind）：旗标置起来之前不进 bounds")
    i_secret = idx_of(lines, "key=secret")
    check("[C] 找到 flag:secret 那条动作", i_secret >= 0)
    if i_secret >= 0:
        before = [l for l in lines[:i_secret] if "UIDOC-BOUNDS" in l]
        after = [l for l in lines[i_secret:] if "UIDOC-BOUNDS" in l]
        check("[C] 置旗标**之前** panel_secret 不在 bounds 里",
              bool(before) and not any("id=panel_secret" in l for l in before),
              "before 有 %d 条 bounds" % len(before))
        check("[C] 置旗标**之后** panel_secret 出现在 bounds 里",
              any("id=panel_secret" in l for l in after) and
              any("id=secret_text" in l for l in after),
              "after 有 %d 条 bounds" % len(after))
        # 判据数值：7/1 → 9/0（widgets 数变了才说明"记录真的产出了"）
        sb = [m for m in grep(lines[:i_secret], STATS_RE)]
        sa = [m for m in grep(lines[i_secret:], STATS_RE)]
        check("[C] widgets 7→9、skipped 1→0（隐藏控件真的不产记录）",
              bool(sb) and bool(sa) and sb[-1].group(1) == "7" and
              sb[-1].group(3) == "1" and sa[-1].group(1) == "9" and
              sa[-1].group(3) == "0",
              "%s/%s → %s/%s" % (sb[-1].group(1) if sb else "?",
                                 sb[-1].group(3) if sb else "?",
                                 sa[-1].group(1) if sa else "?",
                                 sa[-1].group(3) if sa else "?"))
    return lines


# ============================================================
# D. quit 动词（另起一次，因为它会关掉窗口）
# ============================================================

def case_d(widgets, main_lines):
    print("\n[D] quit 动词：点一下就真的退出")
    rc, lines = run_frames([center(widgets["btn_quit"])], 400)
    check("[D] 跑起来（rc=0）", rc == 0, "rc=%d" % rc)
    check("[D] UIDOC-ACTION kind=quit",
          bool(grep(lines, QUIT_RE)), str(grep(lines, QUIT_RE)))
    check("[D] 真的走到关窗口那一步（GAME-STATE shutting down）",
          bool(lines_with(lines, SHUTDOWN)),
          "有 %d 条" % len(lines_with(lines, SHUTDOWN)))
    # 对照：B/C 那次点遍了别的按钮，必须**没有**这一条
    check("[D] 对照：没点 quit 的那次没有 shutting down",
          main_lines is not None and not lines_with(main_lines, SHUTDOWN),
          "对照组 %d 条" % (len(lines_with(main_lines, SHUTDOWN))
                            if main_lines else -1))


# ============================================================
# E. 真鼠标（另一条代码路径）
# ============================================================

def case_e():
    print("\n[E] 真鼠标：点按钮不吃成「进入操作态」，点空白照旧吃")
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)
    proc, hwnd, tap = launch_window()
    if not hwnd:
        check("[E] 运行时窗口起来", False)
        return
    try:
        b0 = wait_for(tap, BOUNDS_RE, lambda m: m.group(1) == "btn_flag",
                      timeout=20.0)
        st = wait_for(tap, STATS_RE, lambda m: True, timeout=20.0)
        check("[E] 打出 btn_flag 的 bounds 与 STATS", b0 is not None and
              st is not None)
        if not b0 or not st:
            return
        outW, outH = int(st.group(5)), int(st.group(6))
        cw, ch = client_size(hwnd)
        print("    out=%dx%d client=%dx%d" % (outW, outH, cw, ch))

        def to_client(ox, oy):
            # 输出像素 → 客户区像素（合成鼠标只能给客户区坐标）
            return ox * cw / float(outW), oy * ch / float(outH)

        bx, by, bw, bh = (float(b0.group(i)) for i in (3, 4, 5, 6))
        ccx, ccy = to_client(bx + bw * 0.5, by + bh * 0.5)

        force_foreground(hwnd)
        n0 = len(lines_with(list(tap.lines), "UIDOC-CLICK"))
        real_click(hwnd, ccx, ccy)

        hit = wait_for(tap, CLICK1_RE, lambda m: m.group(1) == "btn_flag",
                       timeout=10.0, start=n0)
        check("[E] 真鼠标点中 btn_flag（hit=1）", hit is not None,
              hit.group(0) if hit else "没等到")
        act = wait_for(tap, FLAG_RE, lambda m: m.group(1) == "menuStart",
                       timeout=6.0)
        check("[E] 真鼠标点出 flag:menuStart", act is not None,
              act.group(0) if act else "没等到")

        # ⚠⚠ 这一条是 2026-11-02 修的那条 bug 的唯一判据：加文档交互之前
        #    ①b 无条件捕获鼠标，于是"注入能点、鼠标点不动"。
        mo = wait_for(tap, MOUSE_RE, lambda m: m.group(3) == "btn_flag",
                      timeout=6.0)
        check("[E] 点按钮不进入操作态（GAME-MOUSE captured=0 reason=ui-doc）",
              mo is not None and mo.group(1) == "0",
              mo.group(0) if mo else "没等到")

        # ---- 对照：点空白仍然进入操作态 ----
        n1 = len(lines_with(list(tap.lines), "GAME-MOUSE"))
        real_click(hwnd, cw * 0.75, ch * 0.85)
        cap = wait_for(tap, MOUSE_RE, lambda m: m.group(2) == "click",
                       timeout=10.0, start=n1)
        check("[E] 对照：点空白仍然 captured=1 reason=click",
              cap is not None and cap.group(1) == "1",
              cap.group(0) if cap else "没等到")
    finally:
        close_window(proc, hwnd)


# ============================================================

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="ABCDE",
                    help="只跑指定组，如 --only AB（默认 ABCDE）")
    args = ap.parse_args()
    only = args.only.upper()

    for exe in (EDITOR, GAME):
        if not os.path.isfile(exe):
            print("结果: FAIL: 找不到 %s（先 ./make.cmd build）" % exe)
            return 4
    if not os.path.isfile(DOC_SRC):
        print("结果: FAIL: 找不到夹具 %s" % DOC_SRC)
        return 4
    sync_fixture()
    doc, widgets = load_widgets()

    main_lines = None
    if "A" in only:
        case_a()
    if "B" in only or "C" in only:
        main_lines = case_bc(widgets)
    if "D" in only:
        case_d(widgets, main_lines)
    if "E" in only:
        case_e()

    print()
    if fails:
        print("结果: FAIL:（%d 项）" % len(fails))
        for f in fails:
            print("   -", f)
        return 6
    print("结果: PASS（UI 动作解析 + 命中 + 鼠标/注入两条路径 + "
          "条件可见 + quit）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
