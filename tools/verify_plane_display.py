"""端到端验证：Shift+A 添加的 **Plane** 不再"灰白半透明、若影若现"。

根因（2026-10-11 修复）：Plane 的几何面正好在 z=0，与编辑器自带 Ground
的顶面**完全共面** → z-fighting：两块面逐像素抢深度，Ground 的灰色
(105,108,114) 透出来，看起来就是"灰白半透明"且随视角"若影若现"。

修法：落地时给 Plane 抬 1cm（视觉上仍然贴地，深度上分离），并把默认
材质调成与 Cube 一致的 (200,200,205)。

断言：
  1. Shift+A → Mesh ▸ Plane 创建成功；
  2. Ctrl+S 后读场景 JSON：Plane 的 position.z ≈ 0.01（共面消除的
     确定性证据 —— 不再与 Ground 打架）；
  3. 截图目视：Plane 是一块均匀的浅灰色实面（不再斑驳）。

用法：
    python tools/verify_plane_display.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import (LogTap, kill_stale_editors, open_menu_and_click,
                             wait_rect)                               # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

ADDED_RE = re.compile(r"ViewportPanel: added 'Plane' via Shift\+A menu")
SAVED_RE = re.compile(r"Scene saved: '(.+)'")

WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101

VK_S = 0x53
SC_S = 0x1F
VK_CONTROL = 0x11
SC_CTRL = 0x1D


def save_scene(hwnd, tap, timeout=6.0):
    """Ctrl+S（真实 Ctrl + PostMessage 带扫描码的 S —— 坑点 #57）。"""
    force_foreground(hwnd)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0, 0)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_S, (SC_S << 16) | 1)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_S, (SC_S << 16) | 0xC0000001)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0x0002, 0)
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for line in tap.lines:
                m = SAVED_RE.search(line)
                if m:
                    return m.group(1)
        time.sleep(0.1)
    return None


def launch(exe, extra_env, size):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update(extra_env)
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
        return None, None, tap
    user32.ShowWindow(hwnd, 9)
    time.sleep(1.5)
    tw, th = (int(v) for v in size.lower().split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)
    return proc, hwnd, tap


def close(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


def center(rect):
    return (int((rect[0] + rect[2]) * 0.5), int((rect[1] + rect[3]) * 0.5))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    args = ap.parse_args()

    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="plane_disp_")
    scene_path = os.path.join(tmpdir, "plane.json").replace("\\", "/")
    out_png = os.path.join(OUT_DIR, "plane_display.png")

    fails = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if detail else ""))
        if not ok:
            fails.append(name)

    # 机位：从 -Y 上方看原点（Plane 会加在鼠标落点附近的原点）
    proc, hwnd, tap = launch(args.exe, {
        "MYVK_EDITOR_SCENE": scene_path,          # 不存在 → 当前文档是它
        "MYVK_EDITOR_CAM": "0,-7,4,90,25",        # 从 -Y 看 +Z 俯视
    }, args.size)
    if not hwnd:
        print("编辑器没起来")
        return 2
    try:
        print("[1] Shift+A → Mesh ▸ Plane")
        vp = wait_rect(tap, "VP-RECT", 8.0)
        if not vp:
            print("拿不到 VP-RECT")
            return 3
        mx, my = center(vp)
        # Shift+A 要求视口 hovered —— 先把鼠标放进视口
        sx, sy = client_to_screen(hwnd, mx, my)
        user32.SetCursorPos(sx, sy)
        time.sleep(0.5)
        ok = open_menu_and_click(tap, hwnd, mx, my,
                                 "VP-ADD-Mesh", "VP-ADD-Plane", "Plane")
        check("Shift+A 加 Plane 成功", ok)

        end = time.time() + 5.0
        added = False
        while time.time() < end and not added:
            with tap.lock:
                added = any(ADDED_RE.search(l) for l in tap.lines)
            time.sleep(0.1)
        check("创建日志（added 'Plane'）", added)

        print("[2] Ctrl+S → 断言落点 z = 0.01（与 Ground 分离）")
        saved = save_scene(hwnd, tap)
        check("存盘成功", saved is not None, str(saved))
        if saved:
            with open(saved, encoding="utf-8") as f:
                doc = json.load(f)
            plane = None
            for ent in doc.get("entities", []):
                if ent.get("name") == "Plane":
                    plane = ent
            check("场景里有名为 Plane 的实体", plane is not None)
            if plane:
                z = plane["transform"]["position"][2]
                check("Plane 落点 z ≈ 0.01（不再与 Ground 共面）",
                      abs(z - 0.01) < 1e-3, "z=%.4f" % z)

        print("[3] 截图目视")
        force_foreground(hwnd)
        time.sleep(0.5)
        capture_client(hwnd)[0].save(out_png)
        print("  截图:", out_png)

        print()
        if fails:
            print("RESULT: FAIL")
            for f in fails:
                print("  -", f)
            return 4
        print("RESULT: PASS")
        return 0
    finally:
        close(proc, hwnd)


if __name__ == "__main__":
    sys.exit(main())
