"""临时驱动脚本：启动编辑器 → 可选点几下 → 截图 → 关掉。

用途只有一个：把 SSR 粗糙度 / 夜晚天空的改动**看一眼**（含 A/B）。不用它当
判据、不进回归 —— 真正的闸门是 `verify_screenspace`（SSR pass）、
`verify_quality_presets`（参数面）、`verify_i18n_coverage`（词条）。
配套说明见 `docs/sky_night_reflection_2026-11-05.md`。

⚠ 窗口必须按 **PID** 找：桌面上可能同时开着另一个 Editor，按标题
  `find_window("Venn Editor")` 会拿到**别人**的窗口 —— 截出来的图
  是旧画面、`WM_CLOSE` 关掉的是别人的编辑器（踩过）。

⚠ 想直接开成夜晚**不用点 UI**：`MYVK_SKY_PRESET=night`（Renderer 早就有）。
  点「Starry night」按钮只是为了证明**按钮写进去的就是新值**。

用法：
    python tools/_probe_sky_night.py <tag>                       # 只截图
    python tools/_probe_sky_night.py <tag> --clicks "x,y;x,y"    # 依次点
    python tools/_probe_sky_night.py <tag> --set K=V K=V         # 覆盖环境变量
例：
    python tools/_probe_sky_night.py night --set MYVK_SKY_PRESET=night \\
        MYVK_EDITOR_NO_GRID=1
"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import (_looks_blank, capture_client, user32)  # noqa: E402
from verify_asset_drag import client_to_screen                      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT = os.path.abspath(os.path.join(ROOT, "..", "_venn_verify"))
SCENE = "scenes/anime_town.json"

WM_CLOSE = 0x0010
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


def window_of_pid(pid, timeout=45.0):
    """按 PID 找该进程的可见顶层窗口。"""
    end = time.time() + timeout
    while time.time() < end:
        found = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def _enum(hwnd, _lp):
            if not user32.IsWindowVisible(hwnd):
                return True
            p = wt.DWORD(0)
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value == pid and user32.GetWindowTextLengthW(hwnd) > 0:
                found.append(hwnd)
            return True

        user32.EnumWindows(_enum, 0)
        if found:
            return found[0]
        time.sleep(0.3)
    return 0


def grab(hwnd, name, tries=14):
    for _ in range(tries):
        img, ok = capture_client(hwnd)
        if img.size[0] > 16 and not _looks_blank(img):
            p = os.path.join(OUT, name)
            img.save(p)
            print("saved", p, img.size)
            return img
        time.sleep(0.5)
    print("!! blank frames for", name)
    return None


def main():
    argv = sys.argv[1:]
    tag = argv[0] if argv else "a"
    clicks = []
    if "--clicks" in argv:
        spec = argv[argv.index("--clicks") + 1]
        for part in spec.split(";"):
            if part.strip():
                x, _, y = part.partition(",")
                clicks.append((int(x), int(y)))
    overrides = {}
    if "--set" in argv:
        for item in argv[argv.index("--set") + 1:]:
            if item.startswith("--"):
                break
            k, _, v = item.partition("=")
            overrides[k] = v

    env = dict(os.environ)
    env.update({"MYVK_LOG_RECTS": "1", "MYVK_EDITOR_SCENE": SCENE})
    env.update(overrides)
    print("override:", overrides, "clicks:", clicks)

    log_path = os.path.join(OUT, "night_%s_log.txt" % tag)
    logf = open(log_path, "w", encoding="utf-8", errors="replace")
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=logf, stderr=subprocess.STDOUT)
    hwnd = window_of_pid(proc.pid)
    print("pid", proc.pid, "hwnd", hwnd)
    if not hwnd:
        proc.kill()
        logf.close()
        return 1
    time.sleep(3.5)
    grad = grab(hwnd, "night_%s_launch.png" % tag)
    del grad

    if clicks:
        user32.keybd_event(0x12, 0, 0, 0)
        user32.keybd_event(0x12, 0, 2, 0)
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.3)
    for i, click in enumerate(clicks):
        sx, sy = client_to_screen(hwnd, click[0], click[1])
        user32.SetCursorPos(sx, sy)
        time.sleep(0.4)
        user32.SetCursorPos(sx, sy)
        time.sleep(0.4)
        user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.09)
        user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        time.sleep(1.4)
        grab(hwnd, "night_%s_c%d.png" % (tag, i))
    if clicks:
        sx, sy = client_to_screen(hwnd, clicks[-1][0], clicks[-1][1])
        user32.SetCursorPos(sx, sy + 300)   # 移开，避免 hover 高亮
        time.sleep(1.2)
        grab(hwnd, "night_%s_final.png" % tag)

    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    try:
        proc.wait(timeout=15)
    except Exception:  # noqa: BLE001
        proc.kill()
    logf.close()
    with open(log_path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    print("log lines:", text.count("\n"))
    for line in text.splitlines():
        if "SKY-PANEL-PRESET" in line or "ERROR" in line or "VUID" in line:
            print("   |", line[:150])
    return 0


if __name__ == "__main__":
    sys.exit(main())
