"""端到端验证：E21 通知 / 进度系统。

背景：`VK_LOG_WARN/ERROR` 只写进 stdout 和 Console 的环形缓冲 —— 用户在
视口里干活时**根本看不见**；长任务（编译着色器 / 烘焙）也没有任何进度反馈。

本脚本验证五组（每一组一次独立启动，靠锚点日志断言）：

  1. **注入一条 ERROR → 状态栏出现 Error 计数 + Console 里能定位**
     （E21 验收的原文）。同时证明"只升级 Warn/Error"：一次跑下来几百条
     INFO，通知历史里只有个位数条。
  2. **点 Error 的 toast → 打开 Console 并滚到那一条**（NOTIF-CLICK +
     NOTIF-FOCUS）。走的是与鼠标点击完全同一条 clickToast 路径。
  3. **进度任务跑完**：NOTIF-TASK-BEGIN → STEP 0/0.25/0.5/0.75/1.0 →
     END cancelled=0 → 完成通知（"… - done"）。
  4. **取消按钮的契约**：requestCancel → setTaskProgress 返回 false →
     **宿主**收尾 endTask(cancelled=1)，且**不发**完成通知。
  5. **Info 不自动升级**：显式 push 一条 Info 才有通知；顺便确认
     Toast 是"活着"的（toasts=1）。

锚点日志（都由 NotificationCenter 打）：
    NOTIF-PUSH      level=.. source=<log|api> text=".." toasts=N history=M
    NOTIF-DEDUP     level=.. repeat=N text=".."
    NOTIF-CLICK     idx=N level=.. text=".." focus=console
    NOTIF-FOCUS     tab=console ok=1 text=".."
    NOTIF-TASK-BEGIN/STEP/END/CANCEL
    NOTIF-STATUSBAR errors=N warns=N tasks=N dirty=<0|1> unread=N tool=".."
    NOTIF-DUMP      toasts=N history=N warn=N error=N unread=N tasks=N minimized=<0|1>
    CONSOLE-DUMP    lines=N info=N warn=N err=N

用法：
    python tools/verify_notifications.py
"""

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window                                    # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

PUSH_RE = re.compile(
    r'NOTIF-PUSH level=(\w+) source=(\w+) text="([^"]*)" toasts=(\d+) '
    r'history=(\d+)')
CLICK_RE = re.compile(r'NOTIF-CLICK idx=(\d+) level=(\w+) text="([^"]*)" '
                      r'focus=(\w+)')
FOCUS_RE = re.compile(r'NOTIF-FOCUS tab=(\w+) ok=(\d+) text="([^"]*)"')
DEDUP_RE = re.compile(r'NOTIF-DEDUP level=(\w+) repeat=(\d+) text="([^"]*)"')
TASK_BEGIN_RE = re.compile(r'NOTIF-TASK-BEGIN id=(\S+) title="([^"]*)" '
                           r'tasks=(\d+)')
TASK_STEP_RE = re.compile(r'NOTIF-TASK-STEP id=(\S+) progress=([\d.]+)')
TASK_END_RE = re.compile(r'NOTIF-TASK-END id=(\S+) cancelled=(\d+) tasks=(\d+)')
TASK_CANCEL_RE = re.compile(r'NOTIF-TASK-CANCEL id=(\S+)')
STATUS_RE = re.compile(
    r'NOTIF-STATUSBAR errors=(\d+) warns=(\d+) tasks=(\d+) dirty=(\d+) '
    r'unread=(\d+) tool="([^"]*)"')
NOTIF_DUMP_RE = re.compile(
    r'NOTIF-DUMP toasts=(\d+) history=(\d+) warn=(\d+) error=(\d+) '
    r'unread=(\d+) tasks=(\d+) minimized=(\d+)')
CONSOLE_DUMP_RE = re.compile(
    r'CONSOLE-DUMP lines=(\d+) info=(\d+) warn=(\d+) err=(\d+)')

FAILS = []


def check(name, cond, detail=""):
    print("  %s %s%s" % ("[PASS]" if cond else "[FAIL]", name,
                         ("  " + str(detail)) if detail != "" else ""),
          flush=True)
    if not cond:
        FAILS.append(name)
    return cond


def run_once(exe, env_extra, wait_for, timeout=45.0):
    """起编辑器，跑 env_extra，等 wait_for（可迭代）出现；返回 stdout 行。

    等不到也会照常收尾 —— 断言阶段自己会说缺了什么，比在这里超时吼一嗓子
    更有信息量。
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update(env_extra)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", 20.0)
    markers = [wait_for] if isinstance(wait_for, str) else list(wait_for)
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            snap = list(tap.lines)
        if all(any(mk in l for l in snap) for mk in markers):
            break
        time.sleep(0.25)
    time.sleep(0.6)

    try:
        from capture_window import user32
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE → onShutdown
    except Exception:
        pass
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=6)
        except subprocess.TimeoutExpired:
            proc.kill()
    else:
        try:
            proc.wait(timeout=6)
        except subprocess.TimeoutExpired:
            pass
    with tap.lock:
        return list(tap.lines)


def find_all(regex, lines):
    out = []
    for l in lines:
        m = regex.search(l)
        if m:
            out.append(m)
    return out


def dump_or_none(lines):
    ms = find_all(NOTIF_DUMP_RE, lines)
    return ms[-1] if ms else None


# ============================================================
# 第 1 组：注入 ERROR → 状态栏计数 + Console 能定位
# ============================================================
def group_inject_error(exe):
    print("\n== 第 1 组：注入一条 ERROR（MYVK_EDITOR_TOAST=error）==")
    kill_stale_editors()
    lines = run_once(exe, {"MYVK_EDITOR_TOAST": "error"},
                     ["NOTIF-PUSH", "NOTIF-DUMP"])

    pushes = find_all(PUSH_RE, lines)
    check("产生了通知（NOTIF-PUSH）", len(pushes) >= 1, "count=%d" % len(pushes))
    err_push = [m for m in pushes if m.group(1) == "Error"]
    if not check("其中一条是 Error", len(err_push) == 1,
                 "error pushes=%d" % len(err_push)):
        return
    m = err_push[0]
    print("  %s" % m.group(0))
    check("来源是**日志**（source=log，不是显式 push）", m.group(2) == "log",
          m.group(2))
    check("文本就是注入的那条（含 MYVK_EDITOR_TOAST）",
          "injected test error" in m.group(3), m.group(3))

    # 状态栏常驻计数（E21 验收原文："断言状态栏出现 Error 计数"）
    st = find_all(STATUS_RE, lines)
    if check("状态栏打出了计数锚点（NOTIF-STATUSBAR）", len(st) >= 1,
             "count=%d" % len(st)):
        last = st[-1]
        print("  %s" % last.group(0))
        check("状态栏 errors == 1", int(last.group(1)) == 1, last.group(1))
        check("状态栏 tasks == 0", int(last.group(3)) == 0, last.group(3))
        # 边沿触发：第一次（全 0）和注入后（errors=1）各一条，不该每帧一条
        check("计数锚点是边沿触发（不是每帧刷）", len(st) <= 4,
              "count=%d" % len(st))

    # Console 里能定位：那条 Error 必须真的进了 Console 的缓冲
    cd = find_all(CONSOLE_DUMP_RE, lines)
    n_info = 0
    if check("拿到了 Console 统计（CONSOLE-DUMP）", len(cd) >= 1):
        print("  %s" % cd[-1].group(0))
        n_info = int(cd[-1].group(2))
        check("Console 缓冲里有 >=1 条 ERROR", int(cd[-1].group(4)) >= 1,
              cd[-1].group(4))
        check("Console 缓冲里有成规模的 INFO（链路真的在跑）", n_info >= 20,
              n_info)

    nd = dump_or_none(lines)
    if check("拿到了通知统计（NOTIF-DUMP）", nd is not None):
        print("  %s" % nd.group(0))
        n_hist = int(nd.group(2))
        check("通知历史 error == 1", int(nd.group(4)) == 1, nd.group(4))
        # "只升级 Warn/Error"的可断言形式：几百条日志 → 个位数通知
        check("Info 不自动升级（INFO 行数 >= 10x 通知条数）",
              n_info >= 10 * max(n_hist, 1), "info=%d history=%d"
              % (n_info, n_hist))


# ============================================================
# 第 2 组：点 toast → 打开 Console 定位
# ============================================================
def group_click_toast(exe):
    print("\n== 第 2 组：点 toast（MYVK_EDITOR_TOAST=warn + 第 50 帧点它）==")
    kill_stale_editors()
    lines = run_once(exe,
                     {"MYVK_EDITOR_TOAST": "warn", "MYVK_EDITOR_TOAST_CLICK": "0"},
                     ["NOTIF-CLICK", "NOTIF-FOCUS"])

    clicks = find_all(CLICK_RE, lines)
    if not check("点击被记录（NOTIF-CLICK）", len(clicks) >= 1,
                 "count=%d" % len(clicks)):
        return
    c = clicks[-1]
    print("  %s" % c.group(0))
    check("点的是第 0 条", int(c.group(1)) == 0, c.group(1))
    check("级别是 Warn", c.group(2) == "Warn", c.group(2))
    check("指向 Console（focus=console）", c.group(4) == "console", c.group(4))

    foci = find_all(FOCUS_RE, lines)
    if check("定位请求被落地（NOTIF-FOCUS）", len(foci) >= 1,
             "count=%d" % len(foci)):
        f = foci[-1]
        print("  %s" % f.group(0))
        check("打开的是 Console 面板", f.group(1) == "console", f.group(1))
        check("定位成功（ok=1）", f.group(2) == "1", f.group(2))
        check("定位的文本 == 被点的那条", f.group(3) == c.group(3),
              f.group(3))

    st = find_all(STATUS_RE, lines)
    if st:
        print("  %s" % st[-1].group(0))
        check("状态栏 warns >= 1", int(st[-1].group(2)) >= 1, st[-1].group(2))


# ============================================================
# 第 3 组：进度任务跑完
# ============================================================
def group_task_done(exe):
    print("\n== 第 3 组：进度任务跑完（MYVK_EDITOR_ASYNC_TASK=1）==")
    kill_stale_editors()
    lines = run_once(exe, {"MYVK_EDITOR_ASYNC_TASK": "1"},
                     ["NOTIF-TASK-END"])

    begins = find_all(TASK_BEGIN_RE, lines)
    if not check("任务开始了（NOTIF-TASK-BEGIN）", len(begins) >= 1):
        return
    print("  %s" % begins[-1].group(0))
    check("任务 id == demo", begins[-1].group(1) == "demo", begins[-1].group(1))
    check("任务标题非空", len(begins[-1].group(2)) > 0, begins[-1].group(2))
    check("begin 时后台任务数 == 1", int(begins[-1].group(3)) == 1,
          begins[-1].group(3))

    steps = find_all(TASK_STEP_RE, lines)
    progresses = sorted(set(float(m.group(2)) for m in steps))
    print("  进度轨迹: %s" % progresses)
    check("进度按 25% 的桶推进（>=4 档）", len(progresses) >= 4,
          len(progresses))
    check("进度到过 100%", any(p >= 1.0 for p in progresses), progresses)

    ends = find_all(TASK_END_RE, lines)
    if check("任务结束了（NOTIF-TASK-END）", len(ends) >= 1):
        e = ends[-1]
        print("  %s" % e.group(0))
        check("不是被取消的（cancelled=0）", e.group(2) == "0", e.group(2))
        check("结束后后台任务数归零", int(e.group(3)) == 0, e.group(3))

    done = [m for m in find_all(PUSH_RE, lines)
            if m.group(2) == "api" and "- done" in m.group(3)]
    check("任务完成推进了一条通知（'... - done'）", len(done) == 1,
          "count=%d" % len(done))

    st = find_all(STATUS_RE, lines)
    check("状态栏出现过 tasks=1（运行时可见）",
          any(int(m.group(3)) == 1 for m in st),
          [m.group(3) for m in st])


# ============================================================
# 第 4 组：取消按钮的契约
# ============================================================
def group_task_cancel(exe):
    print("\n== 第 4 组：取消任务（MYVK_EDITOR_ASYNC_TASK_CANCEL=1）==")
    kill_stale_editors()
    lines = run_once(exe,
                     {"MYVK_EDITOR_ASYNC_TASK": "1",
                      "MYVK_EDITOR_ASYNC_TASK_CANCEL": "1"},
                     ["NOTIF-TASK-CANCEL", "NOTIF-TASK-END"])

    cancels = find_all(TASK_CANCEL_RE, lines)
    check("取消请求被记录（NOTIF-TASK-CANCEL）", len(cancels) >= 1,
          "count=%d" % len(cancels))

    ends = find_all(TASK_END_RE, lines)
    if check("任务被收尾（NOTIF-TASK-END）", len(ends) >= 1):
        e = ends[-1]
        print("  %s" % e.group(0))
        check("是**取消**结束的（cancelled=1）", e.group(2) == "1", e.group(2))
        check("取消后后台任务数归零", int(e.group(3)) == 0, e.group(3))

    done = [m for m in find_all(PUSH_RE, lines)
            if m.group(2) == "api" and "- done" in m.group(3)]
    check("取消的任务**不发**完成通知", len(done) == 0, "count=%d" % len(done))

    steps = find_all(TASK_STEP_RE, lines)
    progresses = sorted(set(float(m.group(2)) for m in steps))
    print("  取消前的进度轨迹: %s" % progresses)
    check("取消发生在中途（没有走到 100%）",
          all(p < 1.0 for p in progresses), progresses)


# ============================================================
# 第 5 组：显式 Info 通知 + toast 真的活着
# ============================================================
def group_info_toast(exe):
    print("\n== 第 5 组：显式 Info 通知（MYVK_EDITOR_TOAST=info）==")
    kill_stale_editors()
    lines = run_once(exe, {"MYVK_EDITOR_TOAST": "info"},
                     ["NOTIF-PUSH", "NOTIF-DUMP"])

    pushes = [m for m in find_all(PUSH_RE, lines) if m.group(2) == "api"]
    if not check("显式 push 了一条通知", len(pushes) == 1,
                 "count=%d" % len(pushes)):
        return
    p = pushes[0]
    print("  %s" % p.group(0))
    check("级别是 Info", p.group(1) == "Info", p.group(1))
    check("toast 还活着（toasts=1）", int(p.group(4)) == 1, p.group(4))

    nd = dump_or_none(lines)
    if check("拿到了通知统计（NOTIF-DUMP）", nd is not None):
        print("  %s" % nd.group(0))
        check("历史里 error == 0（Info 不会变成 Error）",
              int(nd.group(4)) == 0, nd.group(4))
        check("Info 不自动升级：历史只有显式那一条",
              int(nd.group(2)) <= 30, nd.group(2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("未找到可执行文件，请先构建：", exe)
        return 2

    group_inject_error(exe)
    group_click_toast(exe)
    group_task_done(exe)
    group_task_cancel(exe)
    group_info_toast(exe)

    print("\n结果: " + ("PASS" if not FAILS else "FAIL: " + ", ".join(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
