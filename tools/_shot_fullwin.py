#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时截图：连**非客户区**（标题栏/边框）一起抓，用来看顶部白条。
    python tools/_shot_fullwin.py launcher
    python tools/_shot_fullwin.py editor
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import ImageGrab  # noqa: E402
import launcher_kit  # noqa: E402

OUT = os.path.join(os.path.dirname(launcher_kit.REPO), "_venn_verify", "ui")
user32 = ctypes.windll.user32


def window_rect(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "launcher"
    os.makedirs(OUT, exist_ok=True)
    if which == "launcher":
        proc, tap, hwnd = launcher_kit.launch(env_extra={})
    else:
        import subprocess
        env = launcher_kit.clean_env({})
        proc = subprocess.Popen([launcher_kit.EDITOR], cwd=launcher_kit.BIN, env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        hwnd = 0
        for _ in range(200):
            hwnd = launcher_kit.find_window(launcher_kit.EDITOR_TITLE, timeout=0.3)
            if hwnd:
                break
            time.sleep(0.2)
    if not hwnd:
        print("[FAIL] 窗口没出现")
        return
    launcher_kit.focus_window(hwnd)
    time.sleep(2.5)
    box = window_rect(hwnd)
    print("winrect", box, "client", launcher_kit.client_size(hwnd))
    img = ImageGrab.grab(bbox=box, all_screens=True)
    p = os.path.join(OUT, "full_%s.png" % which)
    img.save(p)
    print("shot ->", p, img.size)
    launcher_kit.shutdown(proc, hwnd)


if __name__ == "__main__":
    main()
