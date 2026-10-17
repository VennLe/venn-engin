"""端到端验证：底部通栏 + Content 抽屉（2026-10-17 布局改造）。

被验证的用户需求（原话）：
  · 原本的 Content 面板移除掉，"7 Entries | assets" 与底部栏合并成一行；
  · 底部栏左侧依次是「内容 / 输出日志 / CMD」，各自带一个清晰可见的 Logo；
  · 点「内容」→ Content 面板从底栏往上弹出，可拖上边缘改高度；
  · 抽屉右上角 "Dock in layout"：勾上就固定在布局里，默认**不勾**；
  · 不勾时，抽屉打开状态下点面板外任意处 → 立即关闭；
  · 播放条（Run/Play/Pause/Stop + 时间轴）的 UI 全部移除。

断言分组：
  A. 底栏形状（日志矩形，确定性）
     A1 通栏 + 高 32；
     A2 三个按钮都在底栏内、从左往右、每个左侧有彩色 Logo 像素；
     A3 资源统计靠右（BAR-STATS 的右边缘贴近窗口右缘）。
  B. 抽屉开合
     B1 默认关着（没有 CONTENT-DRAWER 矩形）；
     B2 点 Content → 抽屉出现，默认高 320、底边 == 底栏上沿；
     B3 再点 Content → 关（日志 open=0）。
  C. 点外部关闭（未停靠）
     C1 打开后点视口上半部 → closed=outside；
     C2 停靠后点同一处 → **不**关（dock 时是布局的一部分）。
  D. 停靠语义
     D1 勾 Dock → dock=1，且视口高度减少 ≈ 抽屉高度（真的占了布局）；
  E. 拖拽高度
     E1 从抽屉上边缘往上拖 120px → 高度增加 ≈ 120（±8）。
  F. 另外两个按钮
     F1 点 Output Log → OUTPUT-LOG open=1 且 CONSOLE-PANEL 矩形出现；
     F2 点 CMD → CMD-PALETTE open 日志出现。

全程零 Vulkan 验证层报错。

用法：python tools/verify_bottom_bar.py
"""

import ctypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, capture_stable, find_window  # noqa: E402
from verify_add_menu import LogTap, click, force_foreground         # noqa: E402
from verify_asset_drag import move_to, press, release               # noqa: E402
from verify_local_shadow import wait_rect                          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
user32 = ctypes.windll.user32

BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"

fails = []


def check(ok, msg):
    print("    %-64s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(msg)
    return ok


def count(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def center(r):
    return (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5


def click_until(hwnd, tap, x, y, needle, n_before, tries=3, wait=1.0):
    """点 (x,y)，直到日志里的 needle 计数增加。

    ⚠ 为什么需要重试：`PrintWindow` 抓图 / `SetCursorPos` 之后，ImGui 有时
      要**两帧**才把鼠标位置与按下状态对齐（尤其窗口刚被 SetWindowPos
      挪过）。一次点击偶尔会"点空"—— 这是注入侧的抖动，不是被测功能的
      缺陷（实测：同一段流程连跑 3 次会有 1 次第一下点空）。
    """
    for _ in range(tries):
        click(hwnd, x, y)
        time.sleep(wait)
        if count(tap, needle) > n_before:
            return True
    return False


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1

    from verify_add_menu import kill_stale_editors
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITOR_NO_HUD": "1",
    })
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到编辑器窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(3.0)
        force_foreground(hwnd)
        time.sleep(0.8)

        crect = ctypes.wintypes.RECT() if hasattr(ctypes, "wintypes") else None
        from ctypes import wintypes as wt
        cr = wt.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(cr))
        W, H = cr.right, cr.bottom

        bar = wait_rect(tap, "BAR-BOTTOM", 10.0)
        if not bar:
            print("缺 BAR-BOTTOM 日志")
            return 3

        # ============================================================ A
        print("\n[A] 底栏形状")
        check(abs((bar[3] - bar[1]) - 32.0) <= 3.0,
              "A1a 底栏高度 ≈ 32 (h=%.0f)" % (bar[3] - bar[1]))
        check(bar[0] <= 2.0 and abs(bar[2] - W) <= 3.0,
              "A1b 底栏通栏 (x∈[%.0f,%.0f], W=%d)" % (bar[0], bar[2], W))

        btns = [("content", wait_rect(tap, "BAR-BTN-content", 5.0)),
                ("log", wait_rect(tap, "BAR-BTN-log", 5.0)),
                ("cmd", wait_rect(tap, "BAR-BTN-cmd", 5.0))]
        miss = [n for n, r in btns if not r]
        if miss:
            check(False, "A2a 三个按钮矩形齐全（缺 %s）" % miss)
        else:
            inside = all(r[1] >= bar[1] - 1 and r[3] <= bar[3] + 1
                         for _, r in btns)
            ordered = btns[0][1][0] < btns[1][1][0] < btns[2][1][0]
            check(inside and ordered,
                  "A2a 按钮都在底栏内、从左往右 (x=%.0f,%.0f,%.0f)"
                  % (btns[0][1][0], btns[1][1][0], btns[2][1][0]))

            img, _ = capture_client(hwnd)
            px = img.load()
            iw, ih = img.size
            icons_px = 0
            for _, r in btns:
                for y in range(int(r[1]), min(int(r[3]), ih)):
                    for x in range(int(r[0]), min(int(r[0]) + 22, iw)):
                        q = px[x, y][:3]
                        if max(q) - min(q) > 40:
                            icons_px += 1
            check(icons_px > 60,
                  "A2b 三个 Logo 各自有彩色像素 (彩色像素总数=%d)" % icons_px)

        stats = wait_rect(tap, "BAR-STATS", 5.0)
        if not stats:
            check(False, "A3 BAR-STATS 矩形")
        else:
            check(abs(stats[2] - (W - 12)) <= 40.0,
                  "A3 资源统计靠右 (x1=%.0f, W-12=%d)" % (stats[2], W - 12))

        # ============================================================ B
        print("\n[B] 抽屉开合")
        check(count(tap, "CONTENT-DRAWER=") == 0,
              "B1 默认关着（没有 CONTENT-DRAWER 矩形）")

        n_open = count(tap, "CONTENT-DRAWER open=1")
        ok_open = click_until(hwnd, tap, *center(btns[0][1]),
                              "CONTENT-DRAWER open=1", n_open)
        time.sleep(0.4)
        drawer = tap.last_rect("CONTENT-DRAWER")
        check(ok_open and drawer is not None,
              "B2a 点 Content → 抽屉打开（日志 open=1）")
        if drawer:
            check(abs((drawer[3] - drawer[1]) - 320.0) <= 4.0,
                  "B2b 默认高度 ≈ 320 (h=%.0f)" % (drawer[3] - drawer[1]))
            check(abs(drawer[3] - bar[1]) <= 2.0,
                  "B2c 抽屉底边贴着底栏上沿 (%.0f vs %.0f)"
                  % (drawer[3], bar[1]))
            check(drawer[0] <= 2.0 and abs(drawer[2] - W) <= 3.0,
                  "B2d 抽屉横跨整窗 (x∈[%.0f,%.0f])" % (drawer[0], drawer[2]))

        n_close = count(tap, "CONTENT-DRAWER open=0")
        ok_close = click_until(hwnd, tap, *center(btns[0][1]),
                               "CONTENT-DRAWER open=0", n_close)
        check(ok_close, "B3 再点 Content → 关（日志 open=0）")

        # ============================================================ C
        print("\n[C] 点外部关闭（未停靠）")
        vp0 = tap.last_rect("VP-RECT")
        click_until(hwnd, tap, *center(btns[0][1]),
                    "CONTENT-DRAWER open=1", count(tap, "CONTENT-DRAWER open=1"))
        time.sleep(0.4)
        n_out = count(tap, "CONTENT-DRAWER closed=outside")
        # 落点必须选在抽屉上沿之上 —— 抽屉是浮层，盖住视口下半部
        ok_c = click_until(hwnd, tap, (vp0[0] + vp0[2]) * 0.5, vp0[1] + 40,
                           "CONTENT-DRAWER closed=outside", n_out)
        check(ok_c, "C1 点视口上半部 → closed=outside")

        # ============================================================ D
        print("\n[D] 停靠语义")
        vp_before = tap.last_rect("VP-RECT")
        click_until(hwnd, tap, *center(btns[0][1]),
                    "CONTENT-DRAWER open=1", count(tap, "CONTENT-DRAWER open=1"))
        time.sleep(0.4)
        dock = wait_rect(tap, "CONTENT-DRAWER-DOCK", 5.0)
        if not dock:
            check(False, "D1 抽屉右上角有 Dock in layout")
        else:
            check(dock[2] > W - 220.0 and dock[1] < bar[1] - 250.0,
                  "D1 Dock 复选框在抽屉右上角 (x1=%.0f, y=%.0f)"
                  % (dock[2], dock[1]))
            n_dock = count(tap, "CONTENT-DRAWER dock=1")
            ok_dock = click_until(hwnd, tap, *center(dock),
                                  "CONTENT-DRAWER dock=1", n_dock, wait=1.2)
            check(ok_dock, "D2 勾 Dock → 日志 dock=1")
            vp_after = tap.last_rect("VP-RECT")
            if vp_before and vp_after:
                drop = (vp_before[3] - vp_before[1]) - (vp_after[3] - vp_after[1])
                check(abs(drop - 320.0) <= 10.0,
                      "D3 停靠后视口变矮 ≈ 320 (Δh=%.0f)" % drop)
            # 停靠时点外部**不**关
            n_out2 = count(tap, "CONTENT-DRAWER closed=outside")
            click(hwnd, (vp_before[0] + vp_before[2]) * 0.5, vp_before[1] + 30)
            time.sleep(1.0)
            check(count(tap, "CONTENT-DRAWER closed=outside") == n_out2,
                  "D4 停靠时点外部 → 不关闭")

        # ============================================================ E
        print("\n[E] 拖拽改高度")
        grip = wait_rect(tap, "CONTENT-DRAWER-GRIP", 5.0)
        d0 = tap.last_rect("CONTENT-DRAWER")
        if not grip or not d0:
            check(False, "E1 拿到把手 / 抽屉矩形")
        else:
            gx = (grip[0] + grip[2]) * 0.5
            gy = grip[1] + 2
            move_to(hwnd, gx, gy)
            time.sleep(0.25)
            press()
            time.sleep(0.2)
            for k in range(1, 9):
                move_to(hwnd, gx, gy - 15 * k)
                time.sleep(0.05)
            time.sleep(0.25)
            release()
            time.sleep(1.0)
            d1 = tap.last_rect("CONTENT-DRAWER")
            grow = (d1[3] - d1[1]) - (d0[3] - d0[1])
            check(abs(grow - 120.0) <= 10.0,
                  "E1 上拖 120px → 高度 +%.0f" % grow)

        # ============================================================ F
        print("\n[F] 输出日志 / CMD")
        n_log = count(tap, "OUTPUT-LOG open=1")
        ok_log = click_until(hwnd, tap, *center(btns[1][1]),
                             "OUTPUT-LOG open=1", n_log)
        check(ok_log, "F1a 点 Output Log → OUTPUT-LOG open=1")
        check(tap.last_rect("CONSOLE-PANEL") is not None,
              "F1b Console 面板真的画出来了（CONSOLE-PANEL 矩形）")

        n_cmd = count(tap, "CMD-PALETTE open=1")
        ok_cmd = click_until(hwnd, tap, *center(btns[2][1]),
                             "CMD-PALETTE open=1", n_cmd)
        check(ok_cmd, "F2 点 CMD → 命令面板打开")

        # ============================================================ G
        print("\n[G] 验证层静默")
        with tap.lock:
            bad = [l for l in tap.lines if __import__("re").search(BAD_RE, l)]
        check(not bad, "G1 零 VUID / ERROR (%d 条)" % len(bad))

        img, _ = capture_client(hwnd)
        if img:
            img.save(os.path.join(OUT_DIR, "bottom_bar_final.png"))

    finally:
        try:
            proc.kill()
        except Exception:
            pass
        time.sleep(0.6)

    print("\n" + "=" * 72)
    if fails:
        print("结果: FAIL（%d 项未通过）" % len(fails))
        for f in fails:
            print("   -", f)
        return 1
    print("结果: PASS（底栏 5 项 + 抽屉开合 5 项 + 点外部 1 项 + 停靠 4 项 + "
          "拖拽 1 项 + 按钮 3 项 + 静默 1 项）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
