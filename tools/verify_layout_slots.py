#!/usr/bin/env python3
"""E29-1 验收：分栏树的"槽位 → 面板"映射从编译期常量变成**运行时可写**。

为什么要单独一组用例：
    改造前，哪个面板在哪一格由 `PanelDesc::layoutSlot` 这张常量表决定，
    改错了编译器会发现（少一个 case 就画不出来）。E29-1 之后它是**数据**，
    错误形态跟着变成静默的：搬丢、搬成两份、落盘读回来没生效 —— 三种都
    可能"看起来正常"。

验什么：
  A  `Editor.exe` + `MYVK_LAYOUT_TEST=1`：33 条纯数值断言（默认树 / 定位 /
     搬家 / 交换 / 矩形跟随）+ **分组存在性**（只数总数的话，"整组被删掉"
     也能靠别组凑够）。
  B  **落盘生效**：写一份布局文件，`slots` 把 Inspector 放到右栏上半、
     Hierarchy 放到下半 → 启动编辑器 → `LAYOUT-RECTS` 里 insp 必须**在上**。
  C  **对照**：同一份文件**删掉 slots 段** → hier 在上（默认树），且日志里
     没有 `LAYOUT-SLOTS`。没有 C 就分不清"是 slots 生效了"还是"本来就那样"。

不验什么：
  * 拖拽 —— 见 `tools/verify_layout_drag.py`（E29-2）。
  * 标签组的视觉（E29-3）。

用法：
    python tools/verify_layout_slots.py
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window                                # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")

FIXTURE = os.path.join(BIN, "layout_slots_fixture.json")

# 与 engine 侧 SplitLayout::buildDefault 对齐的三节点树：
#   0: H  modes | 1
#   1: H  viewport | 2
#   2: V  hierarchy / inspector
LAYOUT_RE = re.compile(
    r"LAYOUT-RECTS modes=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"hier=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"insp=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return bool(ok)


def base_layout():
    """一份最小的合法布局文件（version=2）。"""
    return {
        "version": 2,
        "panels": [],
        "workspace": "level",
        "split": [{"ratio": 0.0, "collapsed": False, "saved": 0.3},
                  {"ratio": 0.0, "collapsed": False, "saved": 0.5},
                  {"ratio": 0.42, "collapsed": False, "saved": 0.42}],
        "viewportLayout": 0,
        "viewMode": 0,
        "maximized": "",
        "inspTab": "object",
        "recent": [],
    }


def write_fixture(layout):
    with open(FIXTURE, "w", encoding="utf-8") as f:
        json.dump(layout, f, ensure_ascii=False, indent=2)
    return FIXTURE


# ------------------------------------------------------------------ A 自检
def group_selftest():
    env = dict(os.environ)
    env["MYVK_LAYOUT_TEST"] = "1"
    env.pop("MYVK_LOG_DT", None)
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    try:
        out, _ = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=10)
    out = (out or "")

    total = re.search(r"LAYOUT-TOTAL pass=(\d+) fail=(\d+)", out)
    fails = re.findall(r"LAYOUT-TEST (\S+).*result=FAIL", out)
    ok = total is not None and total.group(2) == "0" and int(total.group(1)) >= 30
    check("A1 布局自检全绿（>=30 条）", ok,
          (total.group(0) if total else "没有 LAYOUT-TOTAL 行") +
          ("；FAIL=%s" % ",".join(fails) if fails else ""))

    # 分组存在性：每组挑一条"只有这组才有"的断言当锚点
    for group, first in (("default", "default_node_count"),
                         ("locate", "locate_inspector"),
                         ("move", "move_cross_leaf"),
                         ("swap", "swap_ok"),
                         ("rect", "rect_follows_swap")):
        check("A2 分组 %s 有断言（%s）" % (group, first),
              ("LAYOUT-TEST %s " % first) in out,
              "锚点%s" % ("" if first in out else "缺"))


# ------------------------------------------------------- B/C 落盘 → 矩形
def run_editor_with_layout(timeout_s=45.0):
    """起编辑器读 fixture，抓 LAYOUT-RECTS 与 LAYOUT-SLOTS。返回 (rect, log)。"""
    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    # 显式路径 → 自动化运行下**照读**（见 EditorLayoutIO.h 的两条硬规则）
    env["MYVK_EDITOR_LAYOUT"] = FIXTURE
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    rect = None
    try:
        hwnd = find_window("Venn Editor", timeout=timeout_s)
        if not hwnd:
            return None, "窗口没出现"
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            # LogTap 只提供 last_rect / has_line，没有按正则取行的接口 →
            # 自己扫一遍（⚠ 要拿锁，读的是后台线程在追加的 list）
            with tap.lock:
                lines = list(tap.lines)
            for line in lines:
                if "LAYOUT-RECTS" not in line:
                    continue
                m = LAYOUT_RE.search(line)
                if m:
                    rect = tuple(float(m.group(i)) for i in range(1, 13))
                    break
            if rect:
                break
            time.sleep(0.25)
        with tap.lock:
            log = "\n".join(tap.lines)
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except Exception:
            proc.kill()
    return rect, log


def group_persist():
    # ---- B：slots 把 Inspector 换到上半 ----
    lay = base_layout()
    lay["slots"] = [
        {"a": ["modes"], "b": []},
        {"a": ["viewport"], "b": []},
        {"a": ["inspector"], "b": ["hierarchy"]},
    ]
    write_fixture(lay)
    rect, log = run_editor_with_layout()
    if rect is None:
        check("B1 读到 LAYOUT-RECTS", False, str(log)[:80])
        return
    mx, my, mw, mh = rect[0:4]
    hx, hy, hw, hh = rect[4:8]
    ix, iy, iw, ih = rect[8:12]
    # applied 数的是**文件里指定且成功解析的条目数**（这份 fixture 指定了 4 个：
    # 模式栏 / 视口 / Inspector / Hierarchy），不是"换了几个"。
    # 写成 4 而不是"日志里出现 LAYOUT-SLOTS"是刻意的：漏掉任何一个 id
    # （比如 kSlotModes 那个合成槽位 —— 它没有同名 PanelDesc，第一版就掉到
    #  applied=3）都会让这条红。
    applied = re.search(r"LAYOUT-SLOTS applied=(\d+)", log)
    check("B1 文件里 4 个槽位全部解析成功（applied=4）",
          applied is not None and applied.group(1) == "4",
          "actual=%s" % (applied.group(1) if applied else "缺"))
    check("B2 slots 生效：Inspector 被换到右栏上半（insp.y < hier.y）",
          iy < hy - 1.0,
          "insp.y=%.0f hier.y=%.0f" % (iy, hy))
    check("B3 互换后两格仍然同宽同列（是换面板，不是换格子）",
          abs(ix - hx) < 1.0 and abs(iw - hw) < 1.0,
          "insp=(%.0f,%.0f) hier=(%.0f,%.0f)" % (ix, iw, hx, hw))

    # ---- C 对照：删掉 slots → 默认树（hierarchy 在上）----
    lay2 = base_layout()
    write_fixture(lay2)
    rect2, log2 = run_editor_with_layout()
    if rect2 is None:
        check("C1 对照组读到 LAYOUT-RECTS", False, str(log2)[:80])
        return
    hy2 = rect2[5]
    iy2 = rect2[9]
    check("C1 对照：没有 slots 段时 hierarchy 仍在上面（默认树）",
          hy2 < iy2 - 1.0,
          "hier.y=%.0f insp.y=%.0f" % (hy2, iy2))
    check("C2 对照：默认树不触发 LAYOUT-SLOTS（说明 B 的证据来自 slots）",
          "LAYOUT-SLOTS" not in log2,
          "锚点%s" % ("误报" if "LAYOUT-SLOTS" in log2 else "无"))


def main():
    kill_stale_editors()
    if not os.path.exists(EDITOR):
        print("找不到可执行文件:", EDITOR)
        return 2
    try:
        print("== A 布局自检 ==")
        group_selftest()
        print("== B 落盘生效 ==")
        group_persist()
    finally:
        if os.path.exists(FIXTURE):
            os.remove(FIXTURE)

    fails = [n for n, ok, _ in results if not ok]
    if fails:
        print("结果: FAIL: " + ", ".join(fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
