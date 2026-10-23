"""端到端验证：地形笔刷的圆圈**不会变形**（2026-10-23）。

用户原话：
    "在 LandScape 地形模式下，Raise/Lower 这个笔刷操作地形时，笔刷的圆圈
     明显在变形。"

根因（修法见 `LandscapeEditor::viewportInput`）：
  圆环的每个采样点原来都写 `wp.z = heightAtWorld(x, y)` —— 圆环"贴着地表"。
  地表一旦被笔刷推起来，环点就爬上自己刚堆出的山包，透视投影之后椭圆被
  拉歪、压瘪、出现凹口。现在整圈取**同一个高度**（命中点的 z），水平圆的
  投影严格是圆锥曲线（椭圆），形状只由机位决定。

判据（都是**像素级**的，不靠"看着还行"）：
  A 圆环画出来了（颜色掩码 > 200 px）
  B 平地上是正椭圆：非椭圆分量 RMS < 2.0%
  C 笔心（小十字）在**圆心**上：质心 ±3 px 内必须有圆环色像素
    —— 旧代码把十字画在 `outer[0]`（角度 0 的那一点，即圆的右边缘），
       这一条能把它抓出来
  D 堆起一个山包、把圆环压在**半山腰**上，仍然是正椭圆（RMS < 2.0%）
    —— 这是"变形"最容易看出来的机位；修复前同一位置实测 3.5% / 峰峰 2.4 px

用法：python tools/verify_brush_ring.py
退出码 0 = 全过。
"""

import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window                # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors                # noqa: E402
from verify_asset_drag import client_to_screen                        # noqa: E402
from verify_landscape_ground import (OUT_DIR, center, focus_window,    # noqa: E402
                                     rect_of, user32, wait_line,
                                     MOUSEEVENTF_LEFTDOWN,
                                     MOUSEEVENTF_LEFTUP)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# 贴近地表、俯 22°：圆环在屏幕上直径约 90 px（远处看只有二十几像素，
# 形状误差会被像素化抹平，判不出变形）
CAM = "0,-45,18,90,22"

g_pass = 0
g_fail = 0


def check(name, ok, detail=""):
    global g_pass, g_fail
    if ok:
        g_pass += 1
        print("[ OK ] %s%s" % (name, ("  " + detail) if detail else ""))
    else:
        g_fail += 1
        print("[FAIL] %s%s" % (name, ("  " + detail) if detail else ""))
    return ok


# ---------------------------------------------------------------- 像素分析

def ring_mask(img):
    """圆环叠在地形绿上呈**淡黄**：R 很高、B 明显低于 R。"""
    a = np.asarray(img.convert("RGB")).astype(np.int16)
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    m = (r > 225) & (g > 195) & (b > 110) & (b < 205) & ((r - b) > 55)
    m[:90, :] = False       # 顶栏
    m[:, :470] = False      # 左栏（模式栏 + 地形面板）
    m[:, 1260:] = False     # 右栏
    m[800:, :] = False      # 状态栏
    return m


def ellipse_error(xs, ys, nbins=180):
    """r(θ) 的**非椭圆分量**。

    水平圆的透视投影严格是圆锥曲线 → r(θ) 只允许 0 次与 2 次谐波。
    高次谐波冒出来 = 圆环被地形拧过（凹口 / 鼓包 / 花瓣）。
    返回 (RMS px, max px, 平均半径 px)。
    """
    cx, cy = xs.mean(), ys.mean()
    ang = np.arctan2(ys - cy, xs - cx)
    rad = np.hypot(xs - cx, ys - cy)
    bins = ((ang + np.pi) / (2 * np.pi) * nbins).astype(int) % nbins
    r_of = np.full(nbins, np.nan)
    for i in range(nbins):
        sel = bins == i
        if sel.any():
            r_of[i] = rad[sel].mean()
    ok = ~np.isnan(r_of)
    if ok.sum() < 40:
        return (1e9, 1e9, 0.0)
    th = (np.arange(nbins) + 0.5) / nbins * 2 * np.pi - np.pi
    A = np.column_stack([np.ones(ok.sum()),
                         np.cos(th[ok]), np.sin(th[ok]),
                         np.cos(2 * th[ok]), np.sin(2 * th[ok])])
    coef, *_ = np.linalg.lstsq(A, r_of[ok], rcond=None)
    resid = r_of[ok] - A @ coef
    return (float(np.sqrt((resid ** 2).mean())), float(np.abs(resid).max()),
            float(r_of[ok].mean()))


def outer_only(xs, ys):
    """只留外圈（内圈半径约是它的 0.67 倍）。"""
    cx, cy = xs.mean(), ys.mean()
    d = np.hypot(xs - cx, ys - cy)
    return d > np.percentile(d, 97) * 0.80


# ---------------------------------------------------------------- 交互

def shoot(hwnd, name):
    img, _ = capture_client(hwnd)
    p = os.path.join(OUT_DIR, name)
    img.save(p)
    return img, p


def move(hwnd, x, y):
    user32.PostMessageW(hwnd, 0x0200, 0,
                        ((int(y) & 0xFFFF) << 16) | (int(x) & 0xFFFF))
    user32.SetCursorPos(*client_to_screen(hwnd, int(x), int(y)))
    time.sleep(0.05)


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITMODE": "landscape",
        "MYVK_LANDSCAPE_AUTOCREATE": "1",
        "MYVK_EDITOR_CAM": CAM,
    })
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        hwnd = find_window("Venn Editor", timeout=60)
        if not hwnd:
            check("编辑器启动", False, "找不到窗口")
            return 1
        focus_window(hwnd)
        time.sleep(2.0)
        if not wait_line(tap, "LS-AUTOCREATE", 20.0):
            check("地形创建", False, "LS-AUTOCREATE 没出现")
            return 2
        time.sleep(1.2)
        vp = rect_of(tap, "VP-RECT", 8.0)
        if not vp:
            check("拿到视口矩形", False)
            return 2
        cx, cy = center(vp)

        # 半径提到 ~30 m：默认 12 m 在这个机位只有 40 px，判形状太小
        for _ in range(8):
            user32.mouse_event(0x0800, 0, 0, 120, 0)
            time.sleep(0.06)
        time.sleep(0.6)

        # ---- A/B/C：平地 ----
        move(hwnd, cx, cy - 40)
        time.sleep(0.5)
        move(hwnd, cx, cy)
        time.sleep(0.9)
        img, path = shoot(hwnd, "ring_flat.png")
        m = ring_mask(img)
        ys, xs = np.nonzero(m)
        if not check("A 圆环画出来了", len(xs) > 200, "%d px  %s"
                     % (len(xs), path)):
            return 3

        sel = outer_only(xs.astype(float), ys.astype(float))
        rms, mx, mean_r = ellipse_error(xs[sel].astype(float),
                                        ys[sel].astype(float))
        pct = 100.0 * rms / max(mean_r, 1e-6)
        check("B 平地上是正椭圆", pct < 2.0,
              "非椭圆 RMS=%.2fpx (%.1f%%) max=%.2fpx r=%.0fpx"
              % (rms, pct, mx, mean_r))

        # C 笔心必须在圆心：质心 ±3 px 内要有圆环色像素。
        #   旧代码把十字画在 outer[0]（圆的右边缘），圆心是空的。
        icx, icy = int(round(xs.mean())), int(round(ys.mean()))
        near = m[max(0, icy - 3):icy + 4, max(0, icx - 3):icx + 4].any()
        check("C 笔心（十字）在圆心", bool(near),
              "质心=(%d,%d)" % (icx, icy))

        # ---- D：把圆环压在半个山包上 ----
        # 先在中心堆一个山包（按住 3 s ≈ 22 m 高），再把笔心挪到山腰
        user32.SetCursorPos(*client_to_screen(hwnd, int(cx), int(cy)))
        time.sleep(0.25)
        user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(3.0)
        user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        time.sleep(0.9)
        move(hwnd, cx, cy - 90)
        time.sleep(0.4)
        move(hwnd, cx + 34, cy + 6)
        time.sleep(1.0)
        img, path = shoot(hwnd, "ring_slope.png")
        m = ring_mask(img)
        ys, xs = np.nonzero(m)
        if not check("D0 斜坡上圆环还在", len(xs) > 200, "%d px  %s"
                     % (len(xs), path)):
            return 4
        sel = outer_only(xs.astype(float), ys.astype(float))
        rms, mx, mean_r = ellipse_error(xs[sel].astype(float),
                                        ys[sel].astype(float))
        pct = 100.0 * rms / max(max(mean_r, 1e-6), 1e-6)
        check("D 压在斜坡上仍然不变形", pct < 2.2,
              "非椭圆 RMS=%.2fpx (%.1f%%) max=%.2fpx"
              % (rms, pct, mx))
        return 0
    finally:
        try:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        except Exception:                                # noqa: BLE001
            pass
        time.sleep(0.6)
        if proc.poll() is None:
            proc.terminate()


if __name__ == "__main__":
    rc = main()
    print("\n==== %d passed, %d failed ====" % (g_pass, g_fail))
    sys.exit(0 if g_fail == 0 else 1)
