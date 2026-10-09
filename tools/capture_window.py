"""捕获窗口截图（可用于验证渲染是否正常）。

特性：
  * 用 PrintWindow(PW_RENDERFULLCONTENT) 直接抓窗口内容，不受其它窗口遮挡影响
  * 进程声明 DPI 感知，避免 150% 缩放下的坐标偏移
  * 顺带做一次窗口缩放（触发交换链重建），再截图

用法：
    python tools/capture_window.py [--no-resize] [--out 输出.png]
    python tools/capture_window.py --env MYVK_HIDE_UI=1 --env MYVK_CLUSTER_CULL=0

    # 抓编辑器画面（默认目标就是它）
    python tools/capture_window.py --exe build/bin/Editor.exe \
        --title "Venn Editor" --out editor_screenshot.png

    # 抓"编辑器 + 指定场景 + 选中某个物体"（用来截图验证手柄 / 层级图标）
    python tools/capture_window.py --exe build/bin/Editor.exe \
        --title "Venn Editor" --size 1600x900 \
        --env MYVK_EDITOR_SCENE=E:/code/cpp/_venn_verify/verify_scene.json \
        --env MYVK_EDITOR_SELECT=Crate --out gizmo_move.png
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time
from collections import Counter

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
DEFAULT_TITLE = "Venn Editor"

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

try:  # per-monitor DPI 感知
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
        ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
        ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
        ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD),
    ]


def find_window(title, timeout=20.0):
    """按标题**前缀**找顶层窗口。

    窗口标题现在带后缀（如 "Venn Editor"），
    FindWindowW 要求精确匹配会找不到，所以枚举所有顶层窗口做 startswith。
    """
    end = time.time() + timeout
    while time.time() < end:
        found = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def _enum(hwnd, _lparam):
            length = user32.GetWindowTextLengthW(hwnd)
            if length > 0:
                buf = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, buf, length + 1)
                if buf.value.startswith(title) and user32.IsWindowVisible(hwnd):
                    found.append(hwnd)
            return True

        user32.EnumWindows(_enum, 0)
        if found:
            return found[0]
        time.sleep(0.3)
    return 0


def capture_client(hwnd):
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    w, h = rect.right, rect.bottom

    hdc_win = user32.GetDC(hwnd)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_win)
    hbm = gdi32.CreateCompatibleBitmap(hdc_win, w, h)
    gdi32.SelectObject(hdc_mem, hbm)

    PW_CLIENTONLY, PW_RENDERFULLCONTENT = 0x1, 0x2
    ok = user32.PrintWindow(hwnd, hdc_mem,
                            PW_RENDERFULLCONTENT | PW_CLIENTONLY)

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h  # 负数 = 自上而下
    bi.biPlanes = 1
    bi.biBitCount = 32

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc_mem, hbm, 0, h, buf, ctypes.byref(bi), 0)

    gdi32.DeleteObject(hbm)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(hwnd, hdc_win)

    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1)
    return img.convert("RGB"), bool(ok)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-resize", action="store_true",
                    help="跳过窗口缩放测试")
    ap.add_argument("--size", default="",
                    help="把窗口摆成固定尺寸再截图，形如 1600x900。"
                         "审阅布局时用（截图工具默认的压测序列最后停在 1280x720）")
    ap.add_argument("--out", default=os.path.join(ROOT, "sandbox_screenshot.png"))
    ap.add_argument("--wait", type=float, default=4.0, help="启动后等待秒数")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="给子进程追加环境变量，可重复（如 --env MYVK_CLUSTER_CULL=0）")
    ap.add_argument("--exe", default=DEFAULT_EXE,
                    help="要启动的可执行文件（默认 build/bin/Editor.exe）")
    ap.add_argument("--title", default=DEFAULT_TITLE,
                    help="窗口标题前缀（默认 Venn Editor）")
    ap.add_argument("--args", default="",
                    help="追加给子进程的命令行参数，按空格切分。"
                         "抓 Play 态画面用 --env MYVK_EDITOR_PLAY=1（游戏跑在编辑器视口里，"
                         "没有独立的游戏窗口）："
                         "--exe build/bin/Editor.exe --env MYVK_EDITOR_PLAY=1 "
                         "--title \"Venn Editor\"")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    title = args.title

    if not os.path.exists(exe):
        print("未找到可执行文件，请先构建：", exe)
        return 1

    child_env = dict(os.environ)
    # 本工具会故意缩放窗口做压测，还会强杀进程 —— 别让这次运行把
    # "上次关闭时的窗口几何"存档覆写掉（编辑器下次启动就恢复成压测尺寸了）。
    child_env.setdefault("MYVK_NO_WINDOW_SAVE", "1")
    for kv in args.env:
        if "=" not in kv:
            print("--env 需要 KEY=VALUE 形式：", kv)
            return 1
        k, v = kv.split("=", 1)
        child_env[k] = v

    cmd = [exe] + (args.args.split() if args.args else [])
    proc = subprocess.Popen(cmd, cwd=os.path.dirname(exe), env=child_env)
    print("launched pid", proc.pid, "cmd", cmd, flush=True)

    hwnd = find_window(title)
    if not hwnd:
        print("未找到窗口，退出")
        proc.terminate()
        return 2

    user32.ShowWindow(hwnd, 9)   # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    time.sleep(args.wait)

    if args.size:
        try:
            tw, th = (int(v) for v in args.size.lower().split("x"))
        except Exception:
            print("--size 需要 WxH 形式，如 1600x900：", args.size)
            proc.terminate()
            return 1
        user32.ShowWindow(hwnd, 9)   # SW_RESTORE（从最大化还原，否则改不了尺寸）
        time.sleep(0.5)
        flags = 0x0002 | 0x0004      # NOMOVE | NOZORDER
        user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, flags)
        print("resize ->", tw, th, flush=True)
        time.sleep(2.0)
    elif not args.no_resize:
        # 缩放若干次，触发交换链重建（顺带做健壮性验证）
        flags = 0x0002 | 0x0004  # NOMOVE | NOZORDER
        for (tw, th) in [(1024, 640), (1440, 810), (1280, 720)]:
            user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, flags)
            print("resize ->", tw, th, flush=True)
            time.sleep(2.0)

    img, ok = capture_client(hwnd)
    img.save(args.out)
    print("PrintWindow ok=%s size=%s -> %s" % (ok, img.size, args.out))

    cnt = Counter()
    for y in range(0, img.height, 6):
        for x in range(0, img.width, 6):
            cnt[img.getpixel((x, y))] += 1
    total = sum(cnt.values())
    print("主要颜色占比：")
    for c, n in cnt.most_common(8):
        print("    %-18s %.1f%%" % (str(c), 100.0 * n / total))

    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
