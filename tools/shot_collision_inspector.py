"""截取 Inspector 里的 Collision 段落（展开状态），供人工复核。

Collision 段默认是折叠的，而且排在 Material 之后 —— 默认 900px 高的窗口里
它正好落在可视区下沿。这里用一个更高的窗口 + 必要时滚轮下滚，把它推进
可视区，点开标题栏，再截图。

用法：
    python tools/shot_collision_inspector.py
    python tools/shot_collision_inspector.py --shape capsule
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground     # noqa: E402
from verify_collider_gizmo import make_scene                         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_WHEEL = 0x0800


def move(hwnd, cx, cy):
    user32.SetCursorPos(*client_to_screen(hwnd, cx, cy))


def click_left(hwnd, cx, cy):
    force_foreground(hwnd)
    move(hwnd, cx, cy)
    time.sleep(0.3)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.08)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.4)


def wheel(hwnd, cx, cy, notches=-3):
    force_foreground(hwnd)
    move(hwnd, cx, cy)
    time.sleep(0.25)
    for _ in range(abs(notches)):
        user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0,
                           120 if notches > 0 else -120, 0)
        time.sleep(0.12)
    time.sleep(0.4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x1200")
    ap.add_argument("--shape", default="hull", choices=["hull", "capsule"])
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)
    out = args.out or os.path.join(OUT_DIR,
                                   "collision_inspector_%s.png" % args.shape)

    tmpdir = tempfile.mkdtemp(prefix="venn_colui_")
    scene = make_scene(os.path.join(tmpdir, "ui.json"))

    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_SELECT"] = "GizmoCube"
    env["MYVK_EDITOR_COLLIDER"] = args.shape
    env["MYVK_EDITOR_CAM"] = "-5.3,-5.3,3.2,135,20"

    kill_stale_editors()
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    for _ in range(40):
        hwnd = find_window("Venn Editor")
        if hwnd:
            break
        time.sleep(0.25)
    if not hwnd:
        proc.kill()
        print("没找到窗口")
        return 2
    try:
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.2)
        tw, th = (int(v) for v in args.size.lower().split("x"))
        user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
        force_foreground(hwnd)
        time.sleep(2.5)

        # Inspector 右栏的中心 x（视口右缘到窗口右缘之间）
        vp = tap.last_rect("VP-RECT")
        if vp:
            insp_x = int((vp[2] + tw) * 0.5)
        else:
            insp_x = int(tw * 0.9)
        insp_y = int(th * 0.5)

        # 把 Collision 标题栏推进可视区（越界就滚一下）
        for attempt in range(6):
            hdr = tap.last_rect("INS-COLLISION-HEADER")
            if hdr and 0 < hdr[1] < th - 40 and hdr[3] < th - 10:
                break
            wheel(hwnd, insp_x, insp_y, -3)

        hdr = tap.last_rect("INS-COLLISION-HEADER")
        if hdr:
            cx = int((hdr[0] + hdr[2]) * 0.5)
            cy = int((hdr[1] + hdr[3]) * 0.5)
            print("Collision 标题栏 = (%d,%d)" % (cx, cy))
            click_left(hwnd, cx, cy)
        else:
            print("拿不到 INS-COLLISION-HEADER（Inspector 里没有碰撞组件？）")

        time.sleep(0.6)
        capture_client(hwnd)[0].save(out)
        print("截图：", out)
        return 0
    finally:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.6)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
        try:
            os.remove(scene)
            os.rmdir(tmpdir)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
