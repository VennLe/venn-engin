#!/usr/bin/env python3
# 临时截图助手：把一个模型导入并拍 N 张。用法：
#   python tools/_shot.py <模型相对路径> <cam> <tag> [帧数]
# cam = "x,y,z,yaw,pitch"
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client                              # noqa: E402
from verify_player import close, launch                                # noqa: E402
from verify_add_menu import kill_stale_editors                         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT = os.path.join(os.path.dirname(ROOT), "_venn_verify", "probe_agirl")


def main():
    model = sys.argv[1]
    cam = sys.argv[2] if len(sys.argv) > 2 else "2.4,-2.4,1.2,45,12"
    tag = sys.argv[3] if len(sys.argv) > 3 else "shot"
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    os.makedirs(OUT, exist_ok=True)
    kill_stale_editors()
    tmp = tempfile.mkdtemp(prefix="shot_")
    env = {"MYVK_EDITOR_SCENE": os.path.join(tmp, "s.json").replace("\\", "/"),
           "MYVK_EDITOR_IMPORT": model,
           "MYVK_EDITOR_CAM": cam}
    proc, hwnd, tap = launch(EXE, env, "1280x1000")
    if not hwnd:
        print("[FAIL] 起不来")
        return
    try:
        end = time.time() + 25
        while time.time() < end and not tap.has_line("MODEL-INSTANCE"):
            time.sleep(0.15)
        time.sleep(2.5)
        with tap.lock:
            lines = list(tap.lines)
        for ln in lines:
            if any(k in ln for k in ("MODEL-INSTANCE", "ANIM-SKIN", "[ERROR]")):
                print("  " + ln.strip())
        for i in range(n):
            img, ok = capture_client(hwnd)
            if ok:
                p = os.path.join(OUT, "%s_%d.png" % (tag, i))
                img.save(p)
                print("shot -> %s" % p)
            time.sleep(0.4)
    finally:
        close(proc, hwnd)


if __name__ == "__main__":
    main()
