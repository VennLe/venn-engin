#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时截图：启动器 idle 页 / 向导。用法：
    python tools/_shot_launcher.py idle
    python tools/_shot_launcher.py new
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit  # noqa: E402

OUT = os.path.join(os.path.dirname(launcher_kit.REPO), "_venn_verify", "ui")


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "idle"
    os.makedirs(OUT, exist_ok=True)
    env = {}
    if tag == "new":
        env["MYVK_LAUNCHER_PAGE"] = "new"
    proc, tap, hwnd = launcher_kit.launch(env_extra=env)
    if not hwnd:
        print("[FAIL] 起不来")
        return
    try:
        time.sleep(1.5)
        p = os.path.join(OUT, "launcher_%s.png" % tag)
        if launcher_kit.capture(hwnd, p):
            print("shot -> %s" % p)
        else:
            print("[FAIL] capture")
    finally:
        launcher_kit.shutdown(proc, hwnd)


if __name__ == "__main__":
    main()
