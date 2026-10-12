"""一次性诊断：看运行态鼠标视角到底卡在哪个门。

PLAYER-STATE 现在带 miv（viewportHovered）/mdx/mdy（io.MouseDelta）。
跑法：python tools/_probe_mouse.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_player as V                                        # noqa: E402
from capture_window import user32                                # noqa: E402
from verify_add_menu import (kill_stale_editors,                 # noqa: E402
                             open_menu_and_click, wait_rect)
from verify_asset_drag import client_to_screen, force_foreground  # noqa: E402


def dump(tap, tag, n=8):
    with tap.lock:
        lines = [l.rstrip() for l in tap.lines if "PLAYER-STATE" in l]
    print("---- %s (%d 条) ----" % (tag, len(lines)))
    for l in lines[-n:]:
        print("   ", l)


def main():
    kill_stale_editors()
    proc, hwnd, tap = V.launch(V.DEFAULT_EXE, {}, "1600x900")
    if not hwnd:
        print("启动失败")
        return 1
    try:
        vp = wait_rect(tap, "VP-RECT", 8.0)
        print("VP-RECT =", vp)
        mx, my = V.center(vp)
        user32.SetCursorPos(*client_to_screen(hwnd, mx, my))
        time.sleep(0.5)
        ok = open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                                 "VP-ADD-FirstPerson", "First-Person Player")
        print("创建第一人称玩家:", ok)

        V.tap_key(hwnd, V.VK_SPACE)
        st, n = V.wait_new_state(tap, 0, 8.0)
        print("Run 后首条:", st.group(0) if st else None)
        dump(tap, "进入 Run 后")

        # 把逻辑光标放回视口中心，等 hover 落定
        V.mouse_set_client(hwnd, mx, my)
        force_foreground(hwnd)
        time.sleep(1.5)
        dump(tap, "光标放回视口中心后")

        print(">>> 注入 6 次 PostMessage(WM_MOUSEMOVE, +40, 0)")
        for i in range(6):
            V.mouse_move_rel(hwnd, 40, 0)
            time.sleep(0.6)
        time.sleep(0.8)
        dump(tap, "注入水平位移后", 12)

        print(">>> 再注入 6 次 PostMessage(WM_MOUSEMOVE, 0, -40)")
        for i in range(6):
            V.mouse_move_rel(hwnd, 0, -40)
            time.sleep(0.6)
        time.sleep(0.8)
        dump(tap, "注入垂直位移后", 12)
        return 0
    finally:
        V.close(proc, hwnd)


if __name__ == "__main__":
    sys.exit(main())
