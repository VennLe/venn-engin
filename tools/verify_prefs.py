"""端到端验证：Edit > Preferences 大窗口（Blender 风格）。

MYVK_EDITOR_PREFS=1 启动时编辑器自动打开 Preferences（onInit 钩子），
脚本截一张客户区图存档，供人工核对布局：
  - 左侧 168px 分类栏（Interface / Viewport / Editing / Keymap / Scene）
  - 右侧内容页 + 底部说明
  - 窗口 920x620 居中

用法：
    python tools/verify_prefs.py
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32  # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")


def main():
    exe = os.path.abspath(DEFAULT_EXE)
    out = os.path.abspath(os.path.join(ROOT, "..", "_venn_verify"))
    os.makedirs(out, exist_ok=True)

    kill_stale_editors()

    env = dict(os.environ)
    env["MYVK_EDITOR_PREFS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    print("launched pid", proc.pid)
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
        time.sleep(2.5)
        for line in tap.lines:
            if "PREFS" in line:
                print("  ", line)
        win = tap.last_rect("PREFS-WIN")
        img, _ = capture_client(hwnd)
        W, H = img.size
        img.save(os.path.join(out, "preferences.png"))
        print("截图已存:", os.path.join(out, "preferences.png"),
              "尺寸", img.size)
        if not win:
            print("未拿到 PREFS-WIN 日志")
            return 3
        cx = (win[0] + win[2]) * 0.5
        cy = (win[1] + win[3]) * 0.5
        ok = abs(cx - W * 0.5) < 30.0 and abs(cy - H * 0.5) < 30.0
        print("窗口中心 (%.0f,%.0f) vs 客户区中心 (%.0f,%.0f): %s"
              % (cx, cy, W * 0.5, H * 0.5, "PASS" if ok else "FAIL"))
        return 0 if ok else 4
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
