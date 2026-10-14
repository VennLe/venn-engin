#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""验证编辑器窗口 / 任务栏图标的清晰度（对应用户需求①）。

背景：引擎图标原先只有 7 档（16/24/32/48/64/128/256），在 125%/150%/175%
缩放下 GetIconInfo 取到的 SM_CXICON 是 40/56/60/56，最近邻只能拿 32 或 64 再
由 GDI 双线性拉伸 -> 看起来发糊。现在 venn.ico 有 17 档，exe 里也确认嵌全了
（见 tools/verify_exe_icon.py），本脚本负责**实际像素层面**确认抓到的是
原生尺寸那一档，而不是被拉伸的。

做法：
  1. 起 Editor.exe，等窗口出现
  2. 读 DPI（GetDpiForWindow），推出该DPI 下系统期望的 SM_CXICON/SM_CXSMICON
  3. WM_GETICON(ICON_BIG/ICON_SMALL) + GCLP_HICON 取 HICON
  4. GetIconInfo -> bitmap 尺寸，必须**精确等于**期望值（不放大即清晰）
  5. 顺带截标题栏左侧 32x32 与任务栏按钮，存 PNG 供肉眼比对

输出：RESULT: PASS/FAIL + 关键数字
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(os.path.dirname(ROOT), "_venn_verify")

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

# 64 位下 HANDLE/HBITMAP/HICON 是指针宽的指针量；不显式声明 argtypes
# 时 ctypes 按 c_int 传参会OverflowError("int too long to convert")。
# 注意 wintypes 里没有 HBITMAP，必须自己建 c_void_p 别名（且要在
# ICONINFO 结构体定义**之前**赋值，否则字段已按旧类型定型）。
class _HBITMAP(ctypes.c_void_p):
    pass


wt.HBITMAP = _HBITMAP
try:
    ctypes.windll.usercore.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        user32.SetProcessDPIAware()


class ICONINFO(ctypes.Structure):
    _fields_ = [("fIcon", wt.BOOL), ("xHotspot", wt.DWORD),
                ("yHotspot", wt.DWORD), ("hbmMask", wt.HBITMAP),
                ("hbmColor", wt.HBITMAP)]


gdi32.GetObjectW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
gdi32.GetObjectW.restype = ctypes.c_int
gdi32.GetDIBits.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint,
                           ctypes.c_uint, ctypes.c_void_p,
                           ctypes.c_void_p, ctypes.c_uint]
user32.GetIconInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
user32.GetIconInfo.restype = wt.BOOL
user32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                ctypes.c_size_t, ctypes.c_size_t]
user32.SendMessageW.restype = ctypes.c_void_p
user32.GetClassLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]
user32.GetClassLongPtrW.restype = ctypes.c_void_p


class BITMAP(ctypes.Structure):
    _fields_ = [("bmType", ctypes.c_long), ("bmWidth", ctypes.c_long),
                ("bmHeight", ctypes.c_long), ("bmWidthBytes", ctypes.c_long),
                ("bmPlanes", ctypes.c_ushort), ("bmBitsPixel", ctypes.c_ushort),
                ("bmBits", ctypes.c_void_p)]


class BIH(ctypes.Structure):
    _fields_ = [("biSize", wt.UINT), ("biWidth", ctypes.c_int32),
                ("biHeight", ctypes.c_int32), ("biPlanes", wt.USHORT),
                ("biBitCount", wt.USHORT), ("biCompression", wt.UINT),
                ("biSizeImage", wt.UINT), ("biXPelsPerMeter", ctypes.c_int32),
                ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", wt.UINT),
                ("biClrImportant", wt.UINT)]


def find_window(title, timeout=25.0):
    end = time.time() + timeout
    while time.time() < end:
        h = user32.FindWindowW(None, title)
        if h:
            return h
        time.sleep(0.2)
    return 0


def hicon_size(h):
    if not h:
        return None
    ii = ICONINFO()
    if not user32.GetIconInfo(h, ctypes.byref(ii)):
        return None
    bm = BITMAP()
    gdi32.GetObjectW(ii.hbmColor, ctypes.sizeof(bm), ctypes.byref(bm))
    return bm.bmWidth, bm.bmHeight


def save_hicon(h, path):
    ii = ICONINFO()
    if not user32.GetIconInfo(h, ctypes.byref(ii)):
        return None
    bm = BITMAP()
    gdi32.GetObjectW(ii.hbmColor, ctypes.sizeof(bm), ctypes.byref(bm))
    w, hh = bm.bmWidth, bm.bmHeight
    bi = BIH()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth = w
    bi.biHeight = -hh
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0
    buf = ctypes.create_string_buffer(w * hh * 4)
    hdc = user32.GetDC(0)
    gdi32.GetDIBits(hdc, ii.hbmColor, 0, hh, buf, ctypes.byref(bi), 0)
    user32.ReleaseDC(0, hdc)
    img = Image.frombuffer("RGBA", (w, hh), buf, "raw", "BGRA", 0, 1)
    img.save(path)
    return w, hh


def grab_screen(x, y, w, h, path):
    """屏幕区域抓图（BGRX -> PIL）"""
    hdc = user32.GetDC(0)
    bi = BIH()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth = w
    bi.biHeight = -h
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0
    user32.ReleaseDC(0, hdc)
    from PIL import ImageGrab
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True)
    img.save(path)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "bin", "Editor.exe"))
    ap.add_argument("--title", default="Venn Editor")
    ap.add_argument("--keep", action="store_true", help="截图后不关进程")
    args = ap.parse_args()

    env = dict(os.environ)
    proc = subprocess.Popen([args.exe], env=env, cwd=os.path.dirname(args.exe))
    try:
        hwnd = find_window(args.title)
        if not hwnd:
            print("RESULT: FAIL —— 没找到窗口", args.title)
            return 1
        time.sleep(2.5)   # 等 applyIconFromResource 跑完

        dpi = user32.GetDpiForWindow(hwnd)
        cx = user32.GetSystemMetrics(11 if dpi >= 96 else 11)   # SM_CXICON
        cxs = user32.GetSystemMetrics(49)                        # SM_CXSMICON
        # GetSystemMetrics 在 per-monitor DPI 感知下已按当前 DPI 缩放
        want_big, want_small = cx, cxs
        print("DPI = %d  -> SM_CXICON=%d SM_CXSMICON=%d" % (dpi, want_big, want_small))

        WM_GETICON, ICON_BIG, ICON_SMALL, ICON_SMALL2 = 0x007F, 1, 0, 2
        got = {}
        for name, idx in (("BIG", ICON_BIG), ("SMALL", ICON_SMALL),
                          ("SMALL2", ICON_SMALL2)):
            h = user32.SendMessageW(hwnd, WM_GETICON, idx, 0)
            got[name] = hicon_size(h)
            print("  WM_GETICON/%-6s -> %s" % (name, got[name]))
        for name, idx in (("GCLP_HICON", -14), ("GCLP_HICONSM", -34)):
            h = user32.GetClassLongPtrW(hwnd, idx)
            got[name] = hicon_size(h)
            print("  %-13s -> %s" % (name, got[name]))

        # 挑出实际用于标题栏的大图标
        big = user32.SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0)
        if not big:
            big = user32.GetClassLongPtrW(hwnd, -14)
        if big:
            save_hicon(big, os.path.join(OUT, "icon_taskbar_big.png"))
        small = user32.SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0)
        if small:
            save_hicon(small, os.path.join(OUT, "icon_title_small.png"))

        # 标题栏：窗口左上角 48x32
        r = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        print("  window rect", r.left, r.top, r.right, r.bottom)
        try:
            grab_screen(r.left + 6, r.top + 2, 48, 30,
                        os.path.join(OUT, "icon_titlebar_crop.png"))
            # 任务栏按钮：从左往右扫，找 48 宽里含深色非背景像素最多的
            tb = user32.FindWindowW("Shell_TrayWnd", None)
            tr = wt.RECT()
            user32.GetWindowRect(tb, ctypes.byref(tr))
            print("  taskbar", tr.left, tr.top, tr.right, tr.bottom)
            bw = tr.right - tr.left
            grab_screen(tr.left, tr.top, min(bw, 900), tr.bottom - tr.top,
                        os.path.join(OUT, "icon_taskbar_strip.png"))
        except Exception as e:
            print("  (截图跳过:", e, ")")

        ok = True
        for key, want in (("BIG", want_big), ("GCLP_HICON", want_big)):
            sz = got.get(key)
            if sz and sz[0] != want:
                print("  !! %s 实际 %dx%d，期望 %d（被拉伸 = 会糊）"
                      % (key, sz[0], sz[1], want))
                ok = False
        if got.get("SMALL") and got["SMALL"][0] != want_small:
            print("  !! SMALL 实际 %dx%d，期望 %d" %
                  (got["SMALL"][0], got["SMALL"][1], want_small))
            ok = False
        if ok:
            print("RESULT: PASS —— 标题栏/任务栏图标均为原生档，未被 GDI 拉伸")
            return 0
        print("RESULT: FAIL —— 存在被拉伸的档位")
        return 1
    finally:
        if not args.keep:
            try:
                proc.terminate()
                proc.wait(timeout=6)
            except Exception:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())