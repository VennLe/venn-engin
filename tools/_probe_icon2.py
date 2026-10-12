import ctypes, os, subprocess, sys, time
from ctypes import wintypes as wt

u = ctypes.WinDLL("user32", use_last_error=True)
g = ctypes.WinDLL("gdi32", use_last_error=True)

class _HB(ctypes.c_void_p):
    pass

wt.HBITMAP = _HB

class ICONINFO(ctypes.Structure):
    _fields_ = [("fIcon", wt.BOOL), ("xHotspot", wt.DWORD), ("yHotspot", wt.DWORD),
                ("hbmMask", wt.HBITMAP), ("hbmColor", wt.HBITMAP)]

class BITMAP(ctypes.Structure):
    _fields_ = [("bmType", ctypes.c_long), ("bmWidth", ctypes.c_long),
                ("bmHeight", ctypes.c_long), ("bmWidthBytes", ctypes.c_long),
                ("bmPlanes", ctypes.c_ushort), ("bmBitsPixel", ctypes.c_ushort),
                ("bmBits", ctypes.c_void_p)]

g.GetObjectW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
g.GetObjectW.restype = ctypes.c_int
u.GetIconInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
u.GetIconInfo.restype = wt.BOOL
u.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                           ctypes.c_size_t, ctypes.c_size_t]
u.SendMessageW.restype = ctypes.c_void_p
u.GetClassLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]
u.GetClassLongPtrW.restype = ctypes.c_void_p

def sz(h):
    if not h:
        return None
    ii = ICONINFO()
    if not u.GetIconInfo(h, ctypes.byref(ii)):
        return "GetIconInfo-fail"
    bm = BITMAP()
    g.GetObjectW(ii.hbmColor, ctypes.sizeof(bm), ctypes.byref(bm))
    return (bm.bmWidth, bm.bmHeight)

WM_GETICON = 0x7F
WM_SETICON = 0x80

exe = r"E:/code/cpp/venn-engin/build/bin/Editor.exe"
p = subprocess.Popen([exe], cwd=os.path.dirname(exe))
hwnd = 0
for _ in range(120):
    hwnd = u.FindWindowW(None, "Venn Editor")
    if hwnd:
        break
    time.sleep(0.2)
if not hwnd:
    print("no window")
    p.terminate()
    sys.exit(1)
time.sleep(3)

big_h = u.SendMessageW(hwnd, WM_GETICON, 1, 0)
gc = u.GetClassLongPtrW(hwnd, -14)
print(f"WM_GETICON/BIG handle=0x{big_h or 0:x}  size={sz(big_h)}")
print(f"GCLP_HICON      handle=0x{gc or 0:x}  size={sz(gc)}")
print("同一个句柄?", big_h == gc)

# 实验 1：显式把类图标(48)塞回 ICON_BIG，立即读回
u.SendMessageW(hwnd, WM_SETICON, 1, gc)
b2 = u.SendMessageW(hwnd, WM_GETICON, 1, 0)
print(f"重设 ICON_BIG=48 后立即读 -> {sz(b2)}")

# 实验 2：连续采样，看是否又被改回
for i in range(6):
    time.sleep(1.0)
    b3 = u.SendMessageW(hwnd, WM_GETICON, 1, 0)
    print(f"  +{i+1}s ICON_BIG -> {sz(b3)} (handle=0x{b3 or 0:x})")

p.terminate()
try:
    p.wait(timeout=5)
except Exception:
    p.kill()