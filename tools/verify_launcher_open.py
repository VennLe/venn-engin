#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_launcher_open.py —— 启动器 M3 验收：拉起编辑器 + 生命周期

路线图 §7-M3 的验收条款：
  「创建 → 自动拉起 → 等到 `Venn Editor` 窗口 → 断言标题含工程名、
    `EDITOR-READY` 行里的实体数 = 模板实体数。」

这个脚本把 M1（窗口）+ M2（创建）+ M3（拉起）串成**一条用户路径**：
  新建向导 → 建出工程 → **自动**进编辑器 → 启动器收进任务栏 → 编辑器接管。

⚠ 2026-10-22 用户把这一步的交互改了：第三步点「创建」**直接进编辑器**，
  不再有"停在收尾页、再点 Open editor"这一手。所以 O1 现在断言的是
  "**没人点任何东西**，创建成功之后编辑器自己起来了"。

覆盖（O1~O10）：
  O1  点「创建」之后日志**自动**出现 `LAUNCHER-SPAWN ok pid=`
  O1b 收尾页 `LAUNCH-DONE` 仍然画出来（spawn 失败时它是唯一的重试入口）
  O1c `LAUNCHER-OPEN kind=editor` 恰好一次（多一次 = 又走了点按钮的老路）
  O2  拉起之后启动器**收进任务栏**（`LAUNCHER-MINIMIZED` + IsIconic）——
      2026-10-19 用户改的契约：启动器常驻，编辑器关掉之后还要在
  O2b 启动器进程还活着（以前这里断言的是"它自己退出了"，已反转）
  O2c 编辑器退出之后启动器仍在且仍是最小化
  O2d 从任务栏点回来 → `LAUNCHER-RESTORED`
  O3  `Venn Editor` 窗口出现
  O4  窗口标题 == `Venn Editor - <工程名>`（标题栏真的挂上了工程名）
  O5  日志出现 `EDITOR-PROJECT name=<工程名>`
  O6  **CWD == 工程根**（`EDITOR-CWD path=` 逐字相等）—— D5 的核心判据
  O7  场景被自动打开，实体数 == 模板 entities + 1（场景太阳）
  O8  子进程继承了 stdout（否则上面所有"编辑器日志"断言都会静默失败）
  O9  编辑器正常关窗，rc == 0
  O10 没有任何 "cannot chdir" / "is not a project" 的 WARN

⚠ O8 是**前提断言**，不是装饰：`CreateProcessW` 不给 std 句柄的话，编辑器
  的整条日志会静默消失，那时 O5~O7 全变成"看不见"而不是"不成立" ——
  必须能区分这两种情况（本项目对"静默 None"有过明确纪律）。

用法：
    python tools/verify_launcher_open.py
"""

import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402


def launcher_expected_size():
    """启动器的期望客户区尺寸 —— 与 `LauncherApp::onInit` 逐字同一条公式。

        winW = min(1400, workW * 0.94)
        winH = min(880,  workH * 0.90)

    （常量见 `launcher/LauncherApp.cpp` 的 kWindowW/kWindowH/kWinWPct/kWinHPct。）
    ⚠ 这是**唯一**该复写这个公式的地方：把 1400×880 写死在断言里，1366×768
      的笔记本上必红 —— 那里会被工作区夹小，而夹取本身也是要验的行为。
    """
    import ctypes

    class RECT(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                    ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

    r = RECT()
    # SPI_GETWORKAREA = 0x0030（不含任务栏）
    ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(r), 0)
    ww, wh = r.right - r.left, r.bottom - r.top
    want = (min(1400, int(ww * 0.94)), min(880, int(wh * 0.90)))
    return want, (ww, wh)


def main():
    if not os.path.isfile(kit.LAUNCHER) or not os.path.isfile(kit.EDITOR):
        kit.log("结果: FAIL: Launcher.exe / Editor.exe 不存在（先 make build）")
        return 1

    work = os.path.join(kit.WORK_ROOT, "launcher_open")
    work = kit.fresh_dir(work)
    os.makedirs(work, exist_ok=True)
    recent_path = os.path.join(work, "recent.json")
    name = "OpenGame"
    root = os.path.join(work, name)
    kit.log("PROBE work=%s" % work)

    # 期望的实体数来自**模板**（输入），不是来自运行结果（输出）。
    with open(os.path.join(kit.TEMPLATES, "third_person", "assets", "scenes",
                           "main.json"), encoding="utf-8") as f:
        tmpl_scene = json.load(f)
    want_entities = len(tmpl_scene.get("entities", [])) + 1   # + 场景太阳

    fails = []
    proc, tap, hwnd = kit.launch(
        recent_path=recent_path,
        env_extra={
            "MYVK_LAUNCHER_PAGE": "new",
            "MYVK_LAUNCHER_TEST_ROOT": work,
            "MYVK_LAUNCHER_TEST_NAME": name,
        })
    if not hwnd:
        proc.kill()
        kit.log("结果: FAIL: 启动器窗口没出现")
        return 1

    editor_hwnd = 0
    try:
        # ---- O0 窗口尺寸 = min(1400×880, 工作区 × 0.94 / 0.90) ----
        #
        # ⚠ 量的是**客户区**（`GetClientRect`），不是外框：外框含标题栏和
        #   边框，会随 DPI 缩放变化，拿它做断言在 125%/150% 缩放下必红。
        #
        # ⚠ 2026-10-29 改判据：2026-10-27 用户第 1 条把版式从 1080×720
        #   改成 **1400×880**（见 LauncherApp.cpp 的 kWindowW/H），这条断言
        #   一直停在 1080×720 上红着。现在按**同一条公式**算期望值：写死
        #   1400×880 会在 1366×768 的笔记本上必红（那里会被工作区夹到
        #   1284×691），而"夹取逻辑本身"也是要验的东西。
        want_wh, work_wh = launcher_expected_size()
        cw, ch = kit.client_size(hwnd)
        if (cw, ch) != want_wh:
            fails.append("O0 启动器客户区 %dx%d，期望 %dx%d（工作区 %dx%d）"
                         % (cw, ch, want_wh[0], want_wh[1], work_wh[0],
                            work_wh[1]))
        else:
            kit.log("CHECK O0 launcher client size %dx%d OK（工作区 %dx%d）"
                    % (cw, ch, work_wh[0], work_wh[1]))

        # ---- 走完向导（与 M2 同一条路径，这里不重复断言，只推进）----
        if not kit.click_anchor(hwnd, tap, "LAUNCH-TYPE-1"):
            fails.append("向导 Step1 点不到第三人称卡片")
            return kit.finish(fails)
        if not kit.click_anchor(hwnd, tap, "LAUNCH-BTN-next"):
            fails.append("向导 Step1 点不到下一步")
            return kit.finish(fails)
        tap.wait_rect("LAUNCH-INPUT-root", timeout=15.0)
        if not kit.click_anchor(hwnd, tap, "LAUNCH-BTN-next"):
            fails.append("向导 Step2 点不到下一步")
            return kit.finish(fails)
        tap.wait_rect("LAUNCH-COMBO-res", timeout=15.0)
        if not kit.click_anchor(hwnd, tap, "LAUNCH-BTN-create"):
            fails.append("向导 Step3 点不到创建")
            return kit.finish(fails)

        # ---- O1 创建成功 → **自动**进编辑器（2026-10-22 需求 A）----
        #
        # 用户原话："当操作到第三步的时候点击创建的时候，就直接进入引擎编辑器
        # 即可，不需要其他的操作。"
        #
        # 所以这里**不再点** `LAUNCH-BTN-doneOpen`。`commitWizard()` 在创建
        # 成功的那一帧就 `requestSpawn()`，下一帧 `onUpdate` 里拉子进程并
        # 收进任务栏（见 LauncherApp.cpp）。
        #
        # ⚠ 三条断言缺一条，"契约被改回去"这件事就会**静默通过**：
        #   ① SPAWN ok  —— 子进程真的起来了；
        #   ② LAUNCH-DONE 仍然画出来 —— 收尾页不是被删掉了，只是**没人需要
        #      点**。spawn 失败时它还是唯一的重试入口，删了就没有兜底；
        #   ③ `LAUNCHER-OPEN kind=editor` 只出现一次 —— 两次就意味着又走了
        #      "点 Open editor" 那条老路（一次来自 commitWizard，一次来自
        #      按钮）。
        if not tap.wait_line("LAUNCHER-SPAWN ok", timeout=45.0):
            fails.append("O1 创建之后没有自动拉起编辑器（没有 LAUNCHER-SPAWN ok）")
        else:
            for l in tap.text().splitlines():
                if "LAUNCHER-SPAWN ok" in l:
                    kit.log("CHECK O1 %s" % l.split("] ", 1)[-1])
                    break

        # ⚠ 顺序说明：`LAUNCH-DONE` 的锚点行是在**下一帧的 onImGui** 才打的
        #   （创建那一帧 `switch(m_page)` 走的还是 Step3），而 `LAUNCHER-SPAWN`
        #   在同一帧的 `onUpdate` 里更早 —— 所以这条必须**排在 O1 之后**等。
        if not tap.wait_rect("LAUNCH-DONE", timeout=15.0):
            fails.append("O1b 收尾页 LAUNCH-DONE 没画出来"
                         "（spawn 失败时它是唯一的重试入口，不能删）")
        else:
            kit.log("CHECK O1b done page still drawn (fallback entry) OK")

        n_open = tap.count_line("LAUNCHER-OPEN kind=editor")
        if n_open != 1:
            fails.append("O1c LAUNCHER-OPEN kind=editor 出现 %d 次，期望 1"
                         "（多出来说明还在走「点 Open editor」的老路）" % n_open)
        else:
            kit.log("CHECK O1c spawned exactly once, no manual click OK")

        # ---- O2 拉起之后启动器**收进任务栏**，不退出 ----
        # 契约在 2026-10-19 被用户改反了：以前是"一个工程一个进程，拉起即
        # 退出"；现在是"编辑器关掉之后启动器还要在"（换工程不用重新找图标）。
        # ⚠ 因此**不能**再用 `proc.wait()` 断言它退出 —— 那样会把新契约
        #   当成回归。判据拆成三条：日志 / 进程还活着 / Windows 真的收进任务栏。
        if not tap.wait_line("LAUNCHER-MINIMIZED", timeout=30.0):
            fails.append("O2 没有 LAUNCHER-MINIMIZED（拉起后没被最小化）")
        else:
            kit.log("CHECK O2 LAUNCHER-MINIMIZED OK")
        time.sleep(1.5)
        if proc.poll() is not None:
            fails.append("O2b 拉起之后启动器退出了（rc=%s），应当常驻任务栏"
                         % proc.returncode)
        elif not kit.is_iconic(hwnd):
            fails.append("O2b 启动器进程还在，但窗口没最小化（IsIconic=false）")
        else:
            kit.log("CHECK O2b launcher alive + iconic OK")

        # ---- O3/O4 编辑器窗口 + 标题 ----
        editor_hwnd, title = kit.wait_title(kit.EDITOR_TITLE, timeout=90.0)
        if not editor_hwnd:
            fails.append("O3 等不到 Venn Editor 窗口")
            kit.log("=== 日志尾部 ===")
            kit.log("\n".join(tap.text().splitlines()[-25:]))
            return kit.finish(fails)
        want_title = "%s - %s" % (kit.EDITOR_TITLE, name)
        if title != want_title:
            fails.append("O4 标题=%r，期望 %r" % (title, want_title))
        else:
            kit.log("CHECK O4 title=%r OK" % title)

        # ---- O5 EDITOR-PROJECT ----
        if not tap.wait_line("EDITOR-PROJECT name=%s" % name, timeout=20.0):
            fails.append("O5 日志里没有 EDITOR-PROJECT name=%s" % name)
        else:
            kit.log("CHECK O5 EDITOR-PROJECT name=%s OK" % name)

        # ---- O6 CWD == 工程根（D5）----
        cwd_line = None
        end = time.time() + 20.0
        while time.time() < end and not cwd_line:
            for l in tap.text().splitlines():
                if "EDITOR-CWD path=" in l:
                    cwd_line = l.split("EDITOR-CWD path=", 1)[1].strip()
            time.sleep(0.2)
        if not cwd_line:
            fails.append("O6 日志里没有 EDITOR-CWD（无法证明 CWD 契约）")
        elif kit.project_key(cwd_line) != kit.project_key(root):
            fails.append("O6 CWD=%s，期望工程根 %s（lpCurrentDirectory 没生效）"
                         % (cwd_line, root))
        else:
            kit.log("CHECK O6 cwd=%s OK" % cwd_line)

        # ---- O7 场景实体数 ----
        got = None
        end = time.time() + 25.0
        while time.time() < end and got is None:
            for l in tap.text().splitlines():
                if "auto-opened scene" in l and "entities)" in l:
                    try:
                        got = int(l.rsplit("(", 1)[1].split(" ")[0])
                    except (IndexError, ValueError):
                        pass
            time.sleep(0.2)
        if got is None:
            fails.append("O7 没有 'auto-opened scene ... (N entities)' 行")
        elif got != want_entities:
            fails.append("O7 实体数 %s，期望 %s（模板 %s + 太阳 1）"
                         % (got, want_entities, want_entities - 1))
        else:
            kit.log("CHECK O7 entities=%s OK" % got)

        # ---- O8 子进程确实继承了 stdout ----
        # 判据：日志里必须同时出现**两个进程**的行（启动器的 LAUNCHER-*
        # 与编辑器的 EDITOR-*）。只有启动器那部分 = 句柄没传下去。
        both = tap.has_line("LAUNCHER-SPAWN") and tap.has_line("EDITOR-PROJECT")
        if not both:
            fails.append("O8 子进程没继承 stdout（日志里凑不齐两个进程的行）")
        else:
            kit.log("CHECK O8 child stdout inherited OK")

        # ---- O10 没有加载失败的 WARN ----
        bad = [l for l in tap.text().splitlines()
               if "cannot chdir" in l or "is not a project" in l]
        if bad:
            fails.append("O10 出现加载失败 WARN: %s" % bad[:2])
        else:
            kit.log("CHECK O10 no project-load warnings OK")

        kit.capture(editor_hwnd, os.path.join(work, "editor_opened.png"))

        # ---- O9 正常关窗 + 退出码 ----
        # ⚠ 编辑器是**孙进程**：`proc` 是启动器，早就退出了。要拿编辑器的
        #   退出码必须用日志里那个 pid（`LAUNCHER-SPAWN ok pid=`）。
        pid = kit.parse_spawn_pid(tap.text())
        if not pid:
            fails.append("O9 日志里没有 LAUNCHER-SPAWN 的 pid，无法断言退出码")
        else:
            kit.close_window(editor_hwnd)
            rc = kit.wait_pid_exit(pid, timeout_ms=90000)
            if rc is None:
                fails.append("O9 编辑器 pid=%d 在 WM_CLOSE 之后没有退出" % pid)
            elif rc != 0:
                fails.append("O9 编辑器退出码 rc=%d，期望 0" % rc)
            else:
                kit.log("CHECK O9 editor pid=%d exit rc=0 OK" % pid)
            if not tap.has_line("Application shutdown"):
                fails.append("O9 日志里没有 'Application shutdown'"
                             "（onShutdown 没跑）")
            else:
                kit.log("CHECK O9 onShutdown ran OK")

        # ---- O2c/O2d 编辑器退出之后启动器仍在，且能点回前台 ----
        time.sleep(1.5)
        if proc.poll() is not None:
            fails.append("O2c 编辑器退出后启动器也没了（rc=%s）" % proc.returncode)
        elif not kit.is_iconic(hwnd):
            fails.append("O2c 编辑器退出后启动器窗口不在最小化状态")
        else:
            kit.log("CHECK O2c launcher survived editor exit OK")
            kit.restore_window(hwnd)
            if not tap.wait_line("LAUNCHER-RESTORED", timeout=10.0):
                fails.append("O2d 从任务栏恢复后没出现 LAUNCHER-RESTORED")
            else:
                kit.log("CHECK O2d restore from taskbar OK")

        with open(os.path.join(work, "open.log"), "w", encoding="utf-8") as f:
            f.write(tap.text())

    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)
        if editor_hwnd:
            kit.close_window(editor_hwnd)
            time.sleep(1.0)

    return kit.finish(fails)


if __name__ == "__main__":
    sys.exit(main())
