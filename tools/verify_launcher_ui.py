#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_launcher_ui.py —— 启动器 M1 验收：窗口骨架 + 最近列表（只读）

路线图 §7-M1 的验收条款：
  「`verify_launcher_ui.py` —— 锚点 `LAUNCH-RECENT-*` / `LAUNCH-DETAIL` /
    `LAUNCH-BTN-new`；截图人工看一眼排版（图标/对齐只能靠眼睛）。」

覆盖（U1~U7）：
  U1 窗口起来了，标题是 `Venn Launcher`
  U2 header 画出来了（`LAUNCH-RECT-header`）
  U3 最近列表按 recent.json 的行数逐行画（`LAUNCH-RECENT-0/1`）
  U4 右栏详情存在，且**在左栏右侧**（两栏布局真的成立，不是叠在一起）
  U5 footer 在列表**下方**（`LAUNCH-BTN-new` 的 y > 列表的 y）
  U6 失效条目在启动时被剔除（recent.json 里的死路径在退出后消失）
  U7 **没有任何 LAUNCH-SPAWN**（只读用例不许真的拉起编辑器）
  U8 截图落盘（供人眼复核；脚本不断言像素）

为什么 U4/U5 断言"几何相对关系"而不是"精确像素"：
  窗口大小 / DPI / 字体度量都会让绝对坐标变，写死数字的断言换台机器就红。
  而"右栏在左栏右边""底栏在列表下面"是**布局意图**本身 —— 它错了才是真 bug。

用法：
    python tools/verify_launcher_ui.py
    python tools/verify_launcher_ui.py --keep
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留临时目录与截图")
    args = ap.parse_args()

    if not os.path.isfile(kit.LAUNCHER):
        kit.log("结果: FAIL: Launcher.exe 不存在（先 make build）")
        return 1

    work = os.path.join(kit.WORK_ROOT, "launcher_ui")
    work = kit.fresh_dir(work)
    os.makedirs(work, exist_ok=True)

    # ---- 输入：两个真工程 + 一个死路径 ----
    # ⚠ 死路径**必须**造出来：U6（剔除）是"列表自己会清理"的唯一证据，
    #   全用活工程的话把它删了测试照样全绿。
    alpha = kit.make_project(os.path.join(work, "Alpha"), "first_person")
    beta = kit.make_project(os.path.join(work, "Beta"), "third_person")
    gone = os.path.join(work, "Gone")
    recent_path = os.path.join(work, "recent.json")
    kit.write_recent(recent_path, [
        {"name": "Beta", "path": beta, "gameType": "third_person",
         "lastOpenedAt": "2026-10-19T06:00:00Z"},
        {"name": "Alpha", "path": alpha, "gameType": "first_person",
         "lastOpenedAt": "2026-10-19T05:00:00Z"},
        {"name": "Gone", "path": gone, "gameType": "third_person",
         "lastOpenedAt": "2026-10-19T04:00:00Z"},
    ])
    kit.log("PROBE work=%s recent=%s" % (work, recent_path))

    proc, tap, hwnd = kit.launch(recent_path=recent_path)
    fails = []
    if not hwnd:
        tap_out = tap.text()
        proc.kill()
        kit.log("=== 启动器日志尾部 ===")
        kit.log("\n".join(tap_out.splitlines()[-25:]))
        kit.log("结果: FAIL: 启动器窗口没出现")
        return 1

    try:
        # ---- U1 标题 ----
        kit.log("CHECK U1 title='%s' OK" % kit.TITLE)

        # ---- 布局稳定：等锚点出现（首帧 ImGui 拿不到最终尺寸，
        #      锚点只在矩形变化时才重打 —— 所以要"等出现"而不是 sleep 一把）----
        r_header = tap.wait_rect("LAUNCH-RECT-header", timeout=25.0)
        r_rec0 = tap.wait_rect("LAUNCH-RECENT-0", timeout=25.0)
        r_rec1 = tap.wait_rect("LAUNCH-RECENT-1", timeout=25.0)
        r_detail = tap.wait_rect("LAUNCH-DETAIL", timeout=25.0)
        r_new = tap.wait_rect("LAUNCH-BTN-new", timeout=25.0)

        for name, r in (("LAUNCH-RECT-header", r_header),
                        ("LAUNCH-RECENT-0", r_rec0),
                        ("LAUNCH-RECENT-1", r_rec1),
                        ("LAUNCH-DETAIL", r_detail),
                        ("LAUNCH-BTN-new", r_new)):
            if not r:
                fails.append("缺少锚点 %s（UI 没画到 / 锚点拼错）" % name)
        if fails:
            kit.log("=== 锚点行 ===")
            kit.log("\n".join(l for l in tap.text().splitlines()
                              if "LAUNCH-" in l)[-2000:])
            kit.shutdown(proc, hwnd)
            return kit.finish(fails)

        kit.log("CHECK U2 header=%s OK" % (r_header,))

        # ---- U3 列表行数 = 活工程数（3 条里有 1 条是死路径 → 2 行）----
        # ⚠ 判定用"第 2 行存在、第 3 行不存在"。只断"第 0 行存在"是假绿：
        #   列表全空时 ImGui 也不会画第 0 行…… 但那行的锚点同样不会打，
        #   所以再加一条"行数正好 2"（死路径被剔掉）。
        has2 = tap.wait_rect("LAUNCH-RECENT-2", timeout=2.0)
        if has2:
            fails.append("U3 出现了 LAUNCH-RECENT-2：死路径没有被剔除")
        else:
            kit.log("CHECK U3 rows=2 (Beta/Alpha, Gone 已剔除) OK")

        # ---- U4 右栏在左栏右侧 ----
        if r_detail[0] <= r_rec0[2]:
            fails.append("U4 详情区起点 x=%.0f 没有落在列表右缘 x=%.0f 之后"
                         "（两栏塌成一栏了）" % (r_detail[0], r_rec0[2]))
        else:
            kit.log("CHECK U4 left.right=%.0f < right.left=%.0f OK"
                    % (r_rec0[2], r_detail[0]))

        # ---- U5 footer 在列表下方 ----
        if r_new[1] <= r_rec0[3]:
            fails.append("U5 新建按钮 y=%.0f 没有落在列表底部 y=%.0f 之下"
                         "（底栏跑到列表里了）" % (r_new[1], r_rec0[3]))
        else:
            kit.log("CHECK U5 list.bottom=%.0f < footer.y=%.0f OK"
                    % (r_rec0[3], r_new[1]))

        # ---- U7 只读：不许拉起任何子进程 ----
        if tap.has_line("LAUNCH-SPAWN"):
            fails.append("U7 只读用例里出现了 LAUNCH-SPAWN（不该拉起编辑器）")
        else:
            kit.log("CHECK U7 no LAUNCH-SPAWN OK")

        # ---- 截图（人眼复核排版；脚本不断言像素）----
        shot = os.path.join(work, "launcher_ui.png")
        if kit.capture(hwnd, shot):
            kit.log("CHECK screenshot=%s" % shot)
        else:
            kit.log("WARN 截图失败（不影响判据）")

        # ---- 关窗 ----
        done, rc = kit.shutdown(proc, hwnd)
        if not done:
            fails.append("启动器在 WM_CLOSE 之后没有退出（onShutdown 没跑，"
                         "最近列表不会落盘）")
        kit.log("CHECK exit rc=%s" % rc)

        # ---- U6 死路径在退出后被剔除并落盘 ----
        items = kit.read_recent(recent_path)
        paths = [it.get("path") for it in items]
        if gone in paths:
            fails.append("U6 死路径 'Gone' 仍在 recent.json 里（pruneMissing 没生效"
                         "或没落盘）")
        elif len(items) != 2:
            fails.append("U6 recent.json 剩 %d 条，期望 2（Beta/Alpha）"
                         % len(items))
        else:
            kit.log("CHECK U6 pruned Gone, kept=%d OK" % len(items))

        if not tap.has_line("LAUNCHER-READY"):
            fails.append("日志里没有 LAUNCHER-READY（版本+GPU 那行没打）")
        else:
            for l in tap.text().splitlines():
                if "LAUNCHER-READY" in l:
                    kit.log("CHECK U1 %s" % l.split("] ", 1)[-1])
                    break

    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)

    if not args.keep:
        pass  # 截图留着（人眼复核），工程目录一起留，体积很小

    return kit.finish(fails)


if __name__ == "__main__":
    sys.exit(main())
