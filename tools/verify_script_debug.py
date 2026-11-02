#!/usr/bin/env python3
# ============================================================
# verify_script_debug —— 脚本断点调试（C8）
#
# ---------------------------- 这一刀在验什么 ----------------------------
# 2026-11-02 之前：`grep -rn breakpoint` 全仓 **0 命中**。`.vks` 有实例表
# （Scripts 面板能看 vars），但"停在第几行、那一刻是什么状态"只能靠
# `print` 往 Console 刷 —— 而 `print` 恰恰看不见"没走到的那一行"。
#
# 这一刀验的是**断点真的把执行摁住了**，不是"有个面板"：
#
#   A. 面板锚点（编辑态）+ 空态提示
#   B. 断点**校验**：在注释行 / 空行上点断点是**拒收**的
#      ⚠ 这条比"能加断点"重要：静默接受一个永远不会命中的断点，
#        表现成"断点功能坏了"，而日志里一条错都没有。
#   C. 命中：停在第 13 行 → `SCR-PAUSE line=13` + `SCR-STATE state=Paused`
#      + `SCR-AT-VAR`（**协程里的状态**，这正是 print 看不见的那一半）
#   D. 继续：`script.cont` 之后
#      ① 日志里有 `SCR-RESUME`；
#      ② 下一帧**又停在同一条断点上**（断了还在）；
#      ③ `ticks` **变大了** —— 证明"继续"真的让那条语句执行了一次，
#         而不是只把暂停位清掉（后者会让脚本永远停在原地）。
#      ⚠ ③ 是这一组唯一能区分"执行一次"与"执行零次/两次"的判据。
#   E. 单步：`script.step` 之后停在**下一条语句**（13 → 14）。行号必须变，
#      不变就是"单步 = 继续"。
#   F. 阴性对照：Play 但**不加断点** → 零条 `SCR-PAUSE`
#      ⚠ 少了它，前面几条"有暂停"可能来自别的东西（比如熔断）。
#   G. 真鼠标点源码行 → 真的加上了一条断点（"界面点不到 = 没交付"）
#
# 夹具：把 `verify_flags.write_probe()` 那份脚本拿来用 —— 它有 5 条语句
# 落在 5 / 8 / 9 / 13 / 14 行，正好够"停在 13、单步到 14"。
#
# 用法：python tools/verify_script_debug.py [--only ABCDEFG]
# 结尾一定是 `结果: PASS` / `结果: FAIL: ...`。
# ============================================================

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import click, kill_stale_editors, wait_rect     # noqa: E402
from verify_asset_drag import force_foreground                       # noqa: E402

# 复用旗标那条验收的夹具与启动器：同一份 `.vks`、同一套 `run_editor`
# （含"命令按帧排队"的支持）。不重复写第二份 —— 两份夹具迟早会分叉。
import verify_flags as F                                             # noqa: E402

PROBE_VKS_REL = F.PROBE_VKS_REL
PROBE_SCENE_REL = F.PROBE_SCENE_REL
# 行号**从夹具源码算出来**，不写死：
# ⚠ 第一版写死成 13/14/5 是因为我数了 4 行头注释，实际有 5 行 —— 于是
#   `script.bp add ... 13` 落在 `on update {` 这行（钩子头不是语句），
#   被校验拒收，日志是 "line 13 has no statement"。写死行号等于把夹具的
#   注释行数复制了一份到测试里。
_PROBE_SRC_LINES = F.PROBE_SRC.splitlines()
LINE_LET = _PROBE_SRC_LINES.index("let ticks = 0") + 1
LINE_TICK = _PROBE_SRC_LINES.index("    ticks = ticks + 1") + 1
LINE_FLAG = _PROBE_SRC_LINES.index('    setFlag("ticks", ticks)') + 1

fails = []


def check(name, ok, detail=""):
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                        ("  (%s)" % detail) if detail else ""))
    if not ok:
        fails.append(name)


def lines_of(tap):
    return F.snap(tap)


def count(lines, needle):
    return sum(1 for l in lines if needle in l)


def wait_line(tap, needle, secs=45.0):
    end = time.time() + secs
    while time.time() < end:
        if any(needle in l for l in lines_of(tap)):
            return True
        time.sleep(0.2)
    return False


# ---------------------------------------------------------------- A + B

def group_ab():
    print("\n[A/B] 面板锚点 + 断点校验（注释行必须被拒收）")
    cmd = ("script.bp add %s %d; "
           "script.bp add %s 1; "
           "script.bp list" % (PROBE_VKS_REL, LINE_TICK, PROBE_VKS_REL))
    F.write_probe()
    try:
        proc, hwnd, tap = F.run_editor(panel="scriptdbg", console_cmd=cmd,
                                       wait_tag="SCRDBG-RECT",
                                       wait_for="SCR-BP-TOTAL", wait_secs=45.0)
        check("[A] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return
        for tag in ("SCRDBG-RECT", "SCRDBG-STATE", "SCRDBG-CONT",
                    "SCRDBG-STEP", "SCRDBG-CLEARBP", "SCRDBG-BPCOUNT",
                    "SCRDBG-PATH", "SCRDBG-SRC"):
            r = wait_rect(tap, tag, timeout=12.0)
            check("[A] 锚点 %s" % tag, r is not None, "%s" % (r,))
        lines = lines_of(tap)
        F.close(proc)
        joined = "\n".join(lines)

        # ---- B：合法行收下 ----
        check("[B] 第 13 行断点被接受",
              "SCR-BP add path=%s line=%d total=1" % (PROBE_VKS_REL, LINE_TICK)
              in joined,
              [l for l in lines if "SCR-BP add" in l][:3])
        # ---- B：注释行拒收（这条最值钱，见文件头）----
        check("[B] 第 1 行（注释）被拒收",
              ("SCR-BP-ADD-FAIL path=%s line=1" % PROBE_VKS_REL) in joined,
              [l for l in lines if "SCR-BP-ADD-FAIL" in l][:3])
        check("[B] 拒收之后断点总数仍是 1",
              "SCR-BP-TOTAL count=1" in joined,
              [l for l in lines if "SCR-BP-TOTAL" in l][:3])
        if hwnd:
            pass
    finally:
        F.remove_probe()


# ---------------------------------------------------------------- C + D

def group_cd():
    print("\n[C/D] 断点命中 → 继续 → 再命中（且 ticks 真的涨了）")
    # 命令按帧排队：@40 加断点（同一帧末尾就会命中）→ @80 读状态 →
    # @120 继续 → @160 再读 → @200 清断点 + 继续 → @280 确认已恢复运行。
    cmd = ("@40 script.bp add %s %d; "
           "@80 script.at; "
           "@120 script.cont; "
           "@160 script.at; "
           "@200 script.bp clear; @200 script.cont; "
           "@280 script.at" % (PROBE_VKS_REL, LINE_TICK))
    F.write_probe()
    try:
        proc, hwnd, tap = F.run_editor(
            panel="scriptdbg", console_cmd=cmd, wait_tag="SCRDBG-RECT",
            wait_for="SCR-AT paused=0", play=True, scene=PROBE_SCENE_REL,
            wait_secs=70.0)
        check("[C] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return
        lines = lines_of(tap)
        F.close(proc)
        joined = "\n".join(lines)

        # ---- C：命中 ----
        check("[C] 打出了 SCR-PAUSE 且行号 = %d" % LINE_TICK,
              ("SCR-PAUSE entity=" in joined and
               ("line=%d" % LINE_TICK) in joined and
               PROBE_VKS_REL in joined),
              [l for l in lines if "SCR-PAUSE" in l][:3])
        # ⚠ `SCR-STATE` 只说 Paused，`SCR-PAUSE` 才有行号 —— 两个都要，
        #   否则"状态变了"与"停在对的那一行"混为一谈。
        check("[C] SCR-STATE 报了 Paused",
              any("state=Paused" in l and "SCR-STATE" in l for l in lines),
              [l for l in lines if "SCR-STATE" in l][-3:])
        at_lines = [l for l in lines if "SCR-AT paused=" in l]
        check("[C] script.at 报 paused=1 且停在 %d" % LINE_TICK,
              any(("paused=1" in l) and (("line=%d" % LINE_TICK) in l)
                  for l in at_lines),
              at_lines[:3])
        # ---- C：协程里的变量（print 看不见的那一半）----
        check("[C] script.at 吐出了暂停实例的变量",
              any("SCR-AT-VAR ticks=" in l for l in lines),
              [l for l in lines if "SCR-AT-VAR" in l][:4])

        # ---- D：继续 ----
        check("[D] 打出了 SCR-RESUME", "SCR-RESUME entity=" in joined,
              [l for l in lines if "SCR-RESUME" in l][:3])

        # ② 断了还在：继续之后下一帧应该又停在同一条断点上 → SCR-PAUSE 至少 2 条
        pauses = [l for l in lines if "SCR-PAUSE entity=" in l]
        check("[D] 继续之后又停在同一条断点上（>=2 次命中）",
              len(pauses) >= 2, "hits=%d" % len(pauses))

        # ③ ticks 涨了 —— 证明那条语句**真的执行了一次**
        ticks = [int(m.group(1)) for m in
                 (re.search(r"SCR-AT-VAR ticks=(\d+)", l) for l in lines)
                 if m]
        check("[D] 继续之后 ticks 变大了（语句真的执行了一次）",
              len(ticks) >= 2 and ticks[-1] > ticks[0],
              "ticks=%s" % ticks[:6])

        # 清断点 + 继续之后必须不再暂停
        check("[D] 清断点后恢复到 paused=0",
              "SCR-AT paused=0" in joined,
              at_lines[-2:])
    finally:
        F.remove_probe()


# ---------------------------------------------------------------- E

def group_e():
    print("\n[E] 单步：13 → 14（行号必须变）")
    cmd = ("@40 script.bp add %s %d; "
           "@80 script.at; "
           "@140 script.step; "
           "@200 script.at" % (PROBE_VKS_REL, LINE_TICK))
    F.write_probe()
    try:
        proc, hwnd, tap = F.run_editor(
            panel="scriptdbg", console_cmd=cmd, wait_tag="SCRDBG-RECT",
            wait_for="SCR-STEP-CMD", play=True, scene=PROBE_SCENE_REL,
            wait_secs=70.0)
        check("[E] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return
        time.sleep(1.2)
        lines = lines_of(tap)
        F.close(proc)
        joined = "\n".join(lines)

        check("[E] 单步命令真的执行了（stepped=1）",
              "SCR-STEP-CMD stepped=1" in joined,
              [l for l in lines if "SCR-STEP-CMD" in l][:2])
        check("[E] 打出了 SCR-STEP（放行了暂停实例）",
              "SCR-STEP entity=" in joined,
              [l for l in lines if "SCR-STEP entity" in l][:2])
        # ⚠ 核心判据：单步之后停的行号必须**变**。不变 = "单步 = 继续"，
        #   而那正好是最容易写错的那种实现（把 stepPause 与 pauseSkipLine
        #   搞反/顺序错）。
        pause_lines = [int(m.group(1)) for m in
                       (re.search(r"SCR-PAUSE entity=\d+ path=\S+ line=(\d+)", l)
                        for l in lines) if m]
        check("[E] 先停在 %d" % LINE_TICK,
              LINE_TICK in pause_lines, pause_lines[:6])
        check("[E] 单步之后停在 %d" % LINE_FLAG,
              LINE_FLAG in pause_lines, pause_lines[:6])
    finally:
        F.remove_probe()


# ---------------------------------------------------------------- F

def group_f():
    print("\n[F] 阴性对照：Play 但不加断点 → 零条 SCR-PAUSE")
    F.write_probe()
    try:
        proc, hwnd, tap = F.run_editor(
            panel="scriptdbg", console_cmd="@150 script.at",
            wait_tag="SCRDBG-RECT", wait_for="SCR-AT paused=0",
            play=True, scene=PROBE_SCENE_REL, wait_secs=60.0)
        check("[F] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return
        lines = lines_of(tap)
        F.close(proc)
        # ⚠ 没有这一组的话，C/D/E 的"有 SCR-PAUSE"可能来自别的机制
        #   （比如我把暂停打在了熔断路径上）。对照区必须是 0。
        check("[F] 没有断点时一条 SCR-PAUSE 都没有",
              count(lines, "SCR-PAUSE") == 0,
              count(lines, "SCR-PAUSE"))
        check("[F] 脚本确实在跑（有 SCR-STATE Running）",
              any("state=Running" in l for l in lines),
              [l for l in lines if "SCR-STATE" in l][:2])
        check("[F] 状态是 paused=0", "SCR-AT paused=0" in "\n".join(lines),
              [l for l in lines if "SCR-AT " in l][:2])
    finally:
        F.remove_probe()


# ---------------------------------------------------------------- G

MOUSEEVENTF_WHEEL = 0x0800


def wheel_at(hwnd, cx, cy, notches=1):
    """在 (cx,cy) 处滚 notches 格（正 = 向下滚 → 内容上移）。"""
    from verify_asset_drag import client_to_screen                  # noqa: PLC0415
    sx, sy = client_to_screen(hwnd, cx, cy)
    F.user32.SetCursorPos(int(sx), int(sy))
    time.sleep(0.15)
    for _ in range(abs(notches)):
        F.user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0,
                             -120 if notches > 0 else 120, 0)
        time.sleep(0.12)
    time.sleep(0.4)


def scroll_into_box(hwnd, tap, tag, box_tag, tries=10):
    """把 `tag` 那一行滚进 `box_tag` 那块可视区里。

    ⚠⚠ 这一步不是"顺手加的保险"，是必需的：`logRect` 打的是**逻辑坐标**，
      被 `BeginChild` 裁掉的行照样有一行矩形（`GetItemRectMin/Max` 不
      clamp），但鼠标点上去什么也点不到 —— 用例于是表现成"点了没反应"。
      实测：源码子窗口 `SCRDBG-SRC` 的 y 只到 475，而第 14 行在 537~562，
      **整行都在子窗口外面**，点了等于点在面板空白处。
      更阴的是当时的判据（"日志里出现过 total=1 的 add"）会被**命令自己**
      打的那一行满足 —— 于是"点不中"伪装成 PASS。
      修法两条一起做：这里先滚到可见；判据改成"命令碰的是别的行"。
    """
    box = wait_rect(tap, box_tag, timeout=10.0)
    if not box:
        return None
    r = None
    for _ in range(tries):
        r = wait_rect(tap, tag, timeout=3.0)
        if r and r[1] >= box[1] + 2 and r[3] <= box[3] - 2:
            return r
        wheel_at(hwnd, (box[0] + box[2]) * 0.5, (box[1] + box[3]) * 0.5, 1)
    return r


def group_g():
    print("\n[G] 真鼠标点源码行 → 加上断点 → 进 Play 真的停在那儿")
    F.write_probe()
    try:
        # `script.bp add/remove` 用的是 **`let ticks = 0`（第 6 行）**，
        # 不是第 14 行 —— 目的只是"把脚本资产加载进来"（`selectPath` 只认
        # 已加载的路径）。⚠ 必须挑**另一个行号**：如果命令也碰第 14 行，
        # 下面"点击加了断点"的判据就会被命令打的那一行满足，
        # 点击其实完全没生效也会绿（第一版正是这么骗过自己的）。
        cmd = ("script.bp add %s %d; script.bp remove %s %d; "
               "script.select %s; flags.set g_probe 1; script.bp list"
               % (PROBE_VKS_REL, LINE_LET, PROBE_VKS_REL, LINE_LET,
                  PROBE_VKS_REL))
        # ⚠⚠ 这一组**必须跑编辑态**，不能加 `play=True`：
        #   `EditorApp::drawPanels()` 里 `hosted = isEditing() ?
        #   viewportTabDrawnPanel() : -1` —— Play 期间内容区**永远**是
        #   3D 视口（P8），所有 `kSlotNone` 的标签页面板整块不派发。
        #   于是这张面板在 Play 态一帧都不画，`SCRDBG-*` 一条都没有，
        #   而看起来像"面板坏了"（第一版就是这么红的：8 个锚点全无）。
        #
        # ⚠⚠ 必须 `scene=PROBE_SCENE_REL`（编辑态就打开探针场景）：
        #   点 Play 跑的是**编辑态那个场景**。不传的话跑的是默认场景，
        #   里面没有挂着 `_flags_probe.vks` 的实体 → 脚本一帧都不 tick →
        #   断点永远不命中（日志是 `Runtime scripts: 0 executed`）。
        proc, hwnd, tap = F.run_editor(panel="scriptdbg", console_cmd=cmd,
                                       wait_tag="SCRDBG-BPCOUNT",
                                       wait_for="SCR-BP-TOTAL count=0",
                                       scene=PROBE_SCENE_REL, wait_secs=45.0)
        check("[G] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return
        # 点 `on update` 里那条语句，不是"第一条可打断行"：
        # 首条是 `let ticks = 0`（顶层相位，只跑一次），点它验不到
        # "这条断点在 Play 里真的会停"。
        tag = "SCRDBG-LINE-%d" % LINE_TICK
        r = scroll_into_box(hwnd, tap, tag, "SCRDBG-SRC")
        box = wait_rect(tap, "SCRDBG-SRC", timeout=8.0)
        check("[G] 源码行落在源码区可视范围内", bool(r and box and
                                                    r[1] >= box[1] and
                                                    r[3] <= box[3]),
              "line=%s box=%s" % (r, box))
        if not r:
            F.close(proc)
            return

        added = ("SCR-BP add path=%s line=%d total=1"
                 % (PROBE_VKS_REL, LINE_TICK))

        def added_now():
            return any(added in l for l in F.snap(tap))

        # ⚠ 点击次数必须是**奇数**：这一行是"切换"（再点一次就去掉），
        #   点成偶数的话进 Play 时断点已经没了 —— 而且日志里"曾经出现过
        #   一条 add"仍然成立，判据看不出这个错。
        #   所以先点一次、看有没有加上，没加上才再点，最多三次。
        cx = (r[0] + r[2]) / 2.0
        cy = (r[1] + r[3]) / 2.0
        force_foreground(hwnd)
        time.sleep(0.3)
        for _ in range(3):
            click(hwnd, cx, cy)
            end = time.time() + 2.5
            while time.time() < end:
                if added_now():
                    break
                time.sleep(0.15)
            if added_now():
                break
        lines = F.snap(tap)
        joined = "\n".join(lines)
        check("[G] 点击真的在第 %d 行加了一条断点" % LINE_TICK,
              added in joined, [l for l in lines if "SCR-BP" in l][:6])

        # ---- 加码：鼠标加的断点**不是摆设** ----
        # ⚠ 这一步是"界面点不到 = 没交付"的完整闭环：只断言日志里多了一行
        #   `SCR-BP add` 的话，一个"点了记下来但 tick 时从不查"的实现照样绿。
        rp = wait_rect(tap, "TB-PLAY-run", timeout=15.0)
        check("[G] 找到工具栏 Play 按钮", rp is not None, "%s" % (rp,))

        def played():
            return any("EDITOR-FLAG new-session" in l for l in F.snap(tap))

        # ⚠ 为什么是"点到进去为止"而不是像 D 组那样固定点两次：
        #   `TB-PLAY-run` 是**切换**按钮 —— 点两次若两次都命中，就是
        #   "进 Play 又立刻退出"，脚本只跑一两帧（浅色文本可能还没渲染），
        #   而两次都命中的情况下日志与点一次完全一样，看不出来。
        #   合成鼠标的"起手"又不保证第一次一定点中，所以只能**先看效果再决定
        #   要不要再点**：进了就停手。
        if rp:
            cx = (rp[0] + rp[2]) / 2.0
            cy = (rp[1] + rp[3]) / 2.0
            force_foreground(hwnd)
            time.sleep(0.3)
            for _ in range(3):
                click(hwnd, cx, cy)
                end = time.time() + 3.5
                while time.time() < end:
                    if played():
                        break
                    time.sleep(0.15)
                if played():
                    break
        hit = False
        deadline = time.time() + 45.0
        while time.time() < deadline:
            if any(("SCR-PAUSE entity=" in l) and
                   (("line=%d" % LINE_TICK) in l) for l in F.snap(tap)):
                hit = True
                break
            time.sleep(0.2)
        lines = F.snap(tap)
        F.close(proc)
        check("[G] 点 Play 真的进了运行态", played(),
              [l for l in lines if "new-session" in l][:2])
        check("[G] 进 Play 后停在这条鼠标加的断点上（line=%d）" % LINE_TICK,
              hit,
              [l for l in lines if "SCR-PAUSE" in l][:4])
        if not hit:
            # 只在这一条红的时候吐现场：整条链上唯一能区分"断点表没了"、
            # "脚本没跑"、"路径对不上"三种死法的就是这批行。
            # ⚠ 取**最后** 40 条：`SCRDBG-LINE-*` 每行每帧都有一条，
            #   取前 40 条会被编辑态的锚点全部占满，Play 之后的日志一条都看不见
            #   （第一版就是这样：明明有现场，看起来却像"Play 后什么都没发生"）。
            noisy = [x for x in lines
                     if ("SCR-" in x or "SCRDBG-STATE" in x or
                         "SCRDBG-BPCOUNT" in x or "scripts:" in x or
                         "script" in x.lower())]
            print("   ---- 现场（含 SCR / script 的行，后 40 条）----")
            for l in noisy[-40:]:
                print("   | " + l)
    finally:
        F.remove_probe()


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="ABCDEFG")
    args = ap.parse_args()
    only = args.only.upper()

    print("=" * 74)
    print("脚本断点调试（C8）：校验 / 命中 / 继续 / 单步 / 阴性对照 / 鼠标")
    print("=" * 74)

    kill_stale_editors()
    try:
        if "A" in only or "B" in only:
            group_ab()
        if "C" in only or "D" in only:
            group_cd()
        if "E" in only:
            group_e()
        if "F" in only:
            group_f()
        if "G" in only:
            group_g()
    finally:
        F.remove_probe()

    print("\n" + "=" * 74)
    if fails:
        print("结果: FAIL: %d 项 —— %s" % (len(fails), fails[:6]))
        return 1
    print("结果: PASS（断点校验 + 命中带行号 + 继续/单步真的前进 + "
          "无断点零暂停 + 鼠标点行加断点）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
