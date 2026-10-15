"""抓 "NO_HUD 视口像素" 参考图（E14 逐像素回归的基线）。

用法：
    python _venn_verify/e14/capture_nohud.py <out.png> [额外 env=value ...]

流程刻意分两段：**先摆窗口尺寸、再读日志里的 VP-RECT**。
反过来的话读到的是首帧（默认窗口大小）的矩形，摆完窗口后就过期了 ——
拿过期矩形去裁图会裁到面板边框上，看起来"什么都没变"其实裁错了地方。
"""
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
from capture_window import capture_client, find_window, user32  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
VP_RE = re.compile(r"VP-RECT=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)")


def main():
    out = sys.argv[1]
    extra = dict(kv.split("=", 1) for kv in sys.argv[2:])

    exe = os.path.join(ROOT, "build", "bin", "Editor.exe")
    env = dict(os.environ)
    env.update({
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_FRAMES": "150",
    })
    env.update(extra)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    vp = {}
    lock = threading.Lock()

    def pump():
        for line in proc.stdout:
            m = VP_RE.search(line)
            if m:
                with lock:
                    vp["r"] = tuple(int(m.group(i)) for i in range(1, 5))

    threading.Thread(target=pump, daemon=True).start()

    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        user32.SetForegroundWindow(hwnd)
        time.sleep(1.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(3.0)          # 等重排后的 VP-RECT 重新打点
        with lock:
            rect = vp.get("r")
        if not rect:
            print("拿不到 VP-RECT")
            return 3
        img, ok = capture_client(hwnd)
        img.crop(rect).save(out)
        print("VP-RECT=%s  capture_ok=%s  -> %s" % (rect, ok, out))
        return 0
    finally:
        proc.terminate()


if __name__ == "__main__":
    sys.exit(main())
