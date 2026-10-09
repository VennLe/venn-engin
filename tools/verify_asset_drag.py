"""端到端验证：把 Content 资源浏览器里的**网格模型**用鼠标拖进场景视口。

为什么需要这么一个脚本：ImGui 的拖拽（BeginDragDropSource / Target）只能靠
真实的鼠标按下-移动-松开才能触发，没法用命令行开关"模拟"一次。而这个功能
（需求 3）恰好是纯交互的，所以只能做真·UI 自动化：

  1. 带 MYVK_LOG_RECTS=1 启动编辑器（它会把 Content 每个格子的矩形、
     视口图像的矩形都写进 stdout，坐标就是客户区物理像素，和 PrintWindow
     截出来的图 1:1）；
  2. 从日志里取 "CB-CELL box01.glb" 和 "VP-RECT" 的矩形；
  3. 把窗口摆成固定尺寸（1600x900）后重新取一次 —— 缩放会让 ImGui 重排，
     旧的坐标就失效了；
  4. SetCursorPos + mouse_event 做一次真实的拖拽：按下 → 移动过 ImGui 的
     drag threshold → 移进视口 → 松开；
  5. 截图存档，并从日志里抓 alignImportToGround 那一行做**数值断言**：
     最终底部必须正好等于 0（栅格面）。

用法：
    python tools/verify_asset_drag.py
    python tools/verify_asset_drag.py --out out.png --asset box01.glb
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import threading
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

RECT_RE = re.compile(
    r"(CB-CELL [^=]+|VP-RECT|HIER-RECT row0)"
    r"=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)"
)
ALIGN_RE = re.compile(r"alignImportToGround: box\.min\.y=(-?[\d.]+) -> bottom=(-?[\d.]+)")


class LogTap:
    """后台读子进程 stdout，边打边解析出需要的矩形。"""

    def __init__(self, proc):
        self.proc = proc
        self.rects = {}          # tag -> (x0, y0, x1, y1)
        self.lines = []
        self.align = None        # (min_y, bottom_y)
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._pump, daemon=True)
        self.t.start()

    def _pump(self):
        for raw in self.proc.stdout:
            line = raw.strip()
            with self.lock:
                self.lines.append(line)
                m = RECT_RE.search(line)
                if m:
                    tag = m.group(1)
                    self.rects[tag] = tuple(int(m.group(i)) for i in range(2, 6))
                a = ALIGN_RE.search(line)
                if a:
                    self.align = (float(a.group(1)), float(a.group(2)))

    def rect(self, tag):
        with self.lock:
            return self.rects.get(tag)

    def wait_rect(self, tag, timeout=30.0):
        end = time.time() + timeout
        while time.time() < end:
            r = self.rect(tag)
            if r:
                return r
            time.sleep(0.1)
        return None


def client_to_screen(hwnd, cx, cy):
    pt = wt.POINT(int(cx), int(cy))
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y


def move_to(hwnd, cx, cy):
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)


def press():
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)


def release():
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


VK_MENU = 0x12


def force_foreground(hwnd):
    """把窗口弄到前台并**确认**成功。

    SetForegroundWindow 有一条硬规则：只有当前前台进程（或被它启动的进程）
    才允许改前台窗口。从后台脚本里直接调多半会**静默失败**，而窗口一旦不是
    活动窗口，ImGui 的拖拽就完全收不到输入（鼠标消息还是会来，但键盘/焦点
    相关的路径不对，实测就是"点了没反应"）。
    办法：先按下再松开一次 ALT —— 系统就认为"用户在操作"，随后的
    SetForegroundWindow 才会被放行。这是 Windows 上很老的绕法，但有效。
    """
    if user32.GetForegroundWindow() == hwnd:
        return True
    user32.keybd_event(VK_MENU, 0, 0, 0)          # ALT down
    user32.ShowWindow(hwnd, 9)                    # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    user32.keybd_event(VK_MENU, 0, 0x0002, 0)     # ALT up (KEYEVENTF_KEYUP)
    time.sleep(0.4)
    return user32.GetForegroundWindow() == hwnd


def cursor_pos():
    pt = wt.POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    return (pt.x, pt.y)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--asset", default="box01.glb",
                    help="Content 里要拖的那个模型文件名")
    ap.add_argument("--out", default=os.path.join(ROOT, "verify_asset_drag.png"))
    ap.add_argument("--debug-shot", default="",
                    help="额外存一张拖到一半的截图，用来判断拖拽有没有点火")
    ap.add_argument("--size", default="1600x900")
    ap.add_argument("--env", action="append", default=[])
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    child_env = dict(os.environ)
    child_env["MYVK_LOG_RECTS"] = "1"
    child_env["MYVK_NO_WINDOW_SAVE"] = "1"
    for kv in args.env:
        k, v = kv.split("=", 1)
        child_env[k] = v

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=child_env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap(proc)
    print("launched pid", proc.pid)

    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        user32.SetForegroundWindow(hwnd)
        time.sleep(2.0)

        # ---- 先摆成固定尺寸再读坐标（ImGui 重排会重打矩形）----
        tw, th = (int(v) for v in args.size.lower().split("x"))
        user32.ShowWindow(hwnd, 9)
        time.sleep(0.5)
        user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
        time.sleep(2.5)

        vp = tap.wait_rect("VP-RECT", 10.0)
        cell = tap.wait_rect("CB-CELL " + args.asset, 15.0)
        if not vp or not cell:
            print("拿不到坐标：VP-RECT=%s CB-CELL=%s" % (vp, cell))
            print("Content 里现有条目：",
                  [t[len("CB-CELL "):] for t in list(tap.rects) if t.startswith("CB-CELL")])
            return 3

        sx = (cell[0] + cell[2]) // 2
        sy = (cell[1] + cell[3]) // 2
        # 落点选视口下半部、偏左一点 —— 既不压在手柄/浮层上，也和原点明显不同，
        # 这样"拖到哪儿就落在哪儿"也能一并看出来
        tx = int(vp[0] + (vp[2] - vp[0]) * 0.34)
        ty = int(vp[1] + (vp[3] - vp[1]) * 0.74)

        print("source(client)=%d,%d  target(client)=%d,%d  vp=%s" % (sx, sy, tx, ty, vp))

        print("foreground ok=%s" % force_foreground(hwnd))
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.3)
        move_to(hwnd, sx, sy)
        print("cursor now =", cursor_pos(), " want(client) =", (sx, sy))
        time.sleep(0.30)
        press()
        time.sleep(0.25)

        # 先原地挪几像素越过 ImGui 的拖拽阈值（默认 6px），拖源才会"点火"
        for dx in (4, 9, 14):
            move_to(hwnd, sx + dx, sy + dx // 2)
            time.sleep(0.18)

        # 分几步移进视口（中间停一下，让 ImGui 每帧都有机会处理）
        steps = 6
        for i in range(1, steps + 1):
            mx = sx + (tx - sx) * i // steps
            my = sy + (ty - sy) * i // steps
            move_to(hwnd, mx, my)
            time.sleep(0.16)

        time.sleep(0.35)
        if args.debug_shot:
            mid, _ = capture_client(hwnd)
            mid.save(args.debug_shot)
            print("拖拽中间态截图 ->", args.debug_shot)

        release()
        time.sleep(1.6)
        move_to(hwnd, vp[0] + 20, vp[1] + 20)   # 把光标挪开，免得挡住视线

        img, ok = capture_client(hwnd)
        img.save(args.out)
        print("PrintWindow ok=%s size=%s -> %s" % (ok, img.size, args.out))

        if tap.align:
            miny, bottom = tap.align
            print("alignImportToGround: box.min.y=%.4f bottom=%.4f" % (miny, bottom))
            print("贴地断言: %s (|bottom| < 1e-3)"
                  % ("PASS" if abs(bottom) < 1e-3 else "FAIL"))
        else:
            print("没有出现 alignImportToGround 日志 —— 拖拽大概率没生效")
            print("最近 25 行日志：")
            for line in tap.lines[-25:]:
                print("   ", line)
            return 4

        imported = [l for l in tap.lines if "Imported" in l or "imported" in l]
        for line in imported[-3:]:
            print("   ", line)
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
