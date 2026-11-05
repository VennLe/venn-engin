"""一次性诊断：第一人称眼位 / 换视角 / 藏身体（2026-11-06）。"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify_player as V
from capture_window import user32
from verify_add_menu import kill_stale_editors, open_menu_and_click, wait_rect
from verify_asset_drag import client_to_screen, force_foreground

VK_V, SC_V = 0x56, 0x2F

def dump(tap, tag, key, n=6):
    # ⚠ 过滤是**大小写敏感**的子串匹配。之前用 "view" 抓视角日志，结果
    # "EDIT-VIEW switched ..." 里一个**小写** view 都没有 —— 整行被静默
    # 过滤掉，看起来像"日志没打"。要用就传 "VIEW"。
    with tap.lock:
        lines = [l.rstrip() for l in tap.lines if key in l]
    print("---- %s (%d) ----" % (tag, len(lines)))
    for l in lines[-n:]:
        print("   ", l)

def main():
    kill_stale_editors()
    proc, hwnd, tap = V.launch(V.DEFAULT_EXE, {}, "1600x900")
    if not hwnd:
        print("启动失败"); return 1
    try:
        vp = wait_rect(tap, "VP-RECT", 8.0)
        mx, my = V.center(vp)
        user32.SetCursorPos(*client_to_screen(hwnd, mx, my))
        time.sleep(0.5)
        print("创建第一人称玩家:",
              open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                                  "VP-ADD-FirstPerson", "First-Person Player"))
        time.sleep(0.6)
        V.tap_key(hwnd, V.VK_SPACE)
        time.sleep(2.0)
        V.mouse_set_client(hwnd, mx, my)
        force_foreground(hwnd)
        print("capture_ready:", V.capture_ready(tap, hwnd, vp))
        time.sleep(1.0)
        dump(tap, "PLAYER-STATE", "PLAYER-STATE", 3)
        dump(tap, "VIEW", "VIEW", 6)

        print(">>> V 键（切第三人称）")
        user32.PostMessageW(hwnd, V.WM_KEYDOWN, VK_V, (SC_V << 16) | 1)
        time.sleep(0.08)
        user32.PostMessageW(hwnd, V.WM_KEYUP, VK_V, (SC_V << 16) | 0xC0000001)
        time.sleep(1.2)
        dump(tap, "PLAYER-STATE", "PLAYER-STATE", 3)
        dump(tap, "VIEW", "VIEW", 6)

        print(">>> V 键（切回第一人称）")
        user32.PostMessageW(hwnd, V.WM_KEYDOWN, VK_V, (SC_V << 16) | 1)
        time.sleep(0.08)
        user32.PostMessageW(hwnd, V.WM_KEYUP, VK_V, (SC_V << 16) | 0xC0000001)
        time.sleep(1.2)
        dump(tap, "VIEW", "VIEW", 6)
        dump(tap, "PLAYER-STATE", "PLAYER-STATE", 3)
        return 0
    finally:
        V.close(proc, hwnd)

sys.exit(main())
