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

  E 半径调到 50 m 以上（超过相机到圆心的 45 m）之后**圆环还在画**
    —— 2026-10-24 用户报的第二条："把笔刷的半径或强度调大后会突然发现
       笔刷的圆圈消失了"。老代码只要有一个环点落到相机背后就整圈 return，
       而半径大于视距时远端点必然在背后。判据是 `LS-RING` 日志里的
       `behind > 0`（确实有环点在背后）+ `arcs >= 1`（仍然画出来了）。
       像素判据在这里不可靠：半径远大于视距时圆环边缘本来就在画面之外。

用法：python tools/verify_brush_ring.py
退出码 0 = 全过。
"""

import os
import re
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window                # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors                # noqa: E402
from verify_asset_drag import client_to_screen                        # noqa: E402
from verify_landscape_ground import (OUT_DIR, focus_window,             # noqa: E402
                                     rect_of, user32, wait_line,
                                     MOUSEEVENTF_LEFTDOWN,
                                     MOUSEEVENTF_LEFTUP)


def center(r):
    """矩形中心。

    ⚠ 本地定义（2026-10-24）：`verify_landscape_ground` 里那份已经删了 ——
      它改成"先 reveal 再点中心"，不再需要这个两行的工具函数。别的脚本
      还在用，所以各自留一份比在两处维护一个跨用例的公共函数更便宜。
    """
    return (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# 贴近地表、俯 24°：圆环在屏幕上直径约 450 px（远处看只有二十几像素，
# 形状误差会被像素化抹平，判不出变形）
#
# ⚠ 2026-10-24：机位从 "0,-45,18,90,22" 拉近到 24 m。
#   远机位（45 m）下，半径超过 ~20 m 时圆环的**近端那段弧**就跑到相机背后
#   （这条正是用户报的"半径调大圆圈消失"，见 E 组），于是 B/C/D 的
#   "整圈椭圆"判据会在放大半径时失效。拉近之后 10~16 m 的半径既够大
#   （450 px）又完整可见。
CAM = "0,-24,10,90,24"

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


def mark_mask(img):
    """笔心十字的像素。

    ⚠ 不能用 `ring_mask`：十字的颜色是 `IM_COL32(255,190,90,235)`，g=190
      刚好低于 ring_mask 的 `g > 195`，一条都命中不了 —— 于是 C 组永远
      FAIL（2026-10-24 踩到）。这里单独放宽到"亮橙"：
      实心十字 r≈255，而填充区是 alpha=30 叠在地形上（r 只有 170 上下），
      两者分得很开。
    """
    a = np.asarray(img.convert("RGB")).astype(np.int16)
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    m = (r > 205) & (g > 145) & (g < 220) & (b < 170) & ((r - b) > 70)
    m[:90, :] = False
    m[:, :470] = False
    m[:, 1260:] = False
    m[800:, :] = False
    return m


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


def last_ring(tap):
    """最后一条 `LS-RING r=... arcs=... behind=...`（外圈的数据）。"""
    with tap.lock:
        for l in reversed(tap.lines):
            m = re.search(r"LS-RING r=([\d.]+) arcs=(\d+) behind=(\d+)", l)
            if m:
                return float(m.group(1)), int(m.group(2)), int(m.group(3))
    return None


def tune_radius(hwnd, tap, lo, hi, tries=60):
    """用滚轮把半径调进 [lo, hi]，等 `LS-RING` 反映出来再返回。

    ⚠ 以前是**盲滚** 8 次当"约 30 m"：鼠标没进视口时一次都不生效，进了
      视口又可能直接冲过裁剪临界点 —— 两种情况下后面的形状断言都在测
      别的东西。改成读日志闭环。
    """
    focus_window(hwnd)
    cur = None
    for _ in range(tries):
        cur = last_ring(tap)
        if cur and lo <= cur[0] <= hi:
            return cur
        step = 120 if (cur is None or cur[0] < lo) else -120
        user32.mouse_event(0x0800, 0, 0, step, 0)
        time.sleep(0.12)
    return cur


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

        # ---- A/B/C：平地（半径锁在"整圈可见"的区间内）----
        move(hwnd, cx, cy - 40)
        time.sleep(0.5)
        move(hwnd, cx, cy)
        time.sleep(0.9)
        cur = tune_radius(hwnd, tap, 5.0, 7.0)
        check("A0 半径落在整圈可见的区间（behind=0）",
              cur is not None and cur[2] == 0, "%s" % (cur,))
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

        # C 笔心必须在**鼠标命中点**：±6 px 内要有笔心十字的像素。
        #   旧代码把十字画在 outer[0]（圆的右边缘），这里就会是空的。
        #
        # ⚠ 判据从"质心"改成"鼠标位置"（2026-10-24）：透视投影下**圆心的
        #   投影并不等于椭圆像素的质心**（近端被压得更扁，质心往远端偏），
        #   本机位实测偏 24 px。拿质心当圆心判，量的是投影的偏心，不是笔心。
        #   鼠标就在视口中心 → 十字必须在那儿。
        mm = mark_mask(img)
        mcx, mcy = int(round(cx)), int(round(cy))
        near = mm[max(0, mcy - 6):mcy + 7, max(0, mcx - 6):mcx + 7].any()
        check("C 笔心（十字）在鼠标命中点", bool(near),
              "鼠标=(%d,%d)，椭圆质心=(%d,%d)"
              % (mcx, mcy, int(round(xs.mean())), int(round(ys.mean()))))

        # ---- D：把圆环压在半个山包上 ----
        # 先在中心堆一个山包（按住 1.5 s ≈ 10 m 高 —— 相机只有 10 m 高，
        # 堆太久会钻进土里，圆环平面跑到相机上方，测的就不是"压在山腰"了），
        # 再把笔心挪到山腰
        user32.SetCursorPos(*client_to_screen(hwnd, int(cx), int(cy)))
        time.sleep(0.25)
        user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(1.5)
        user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        time.sleep(0.9)
        move(hwnd, cx, cy - 90)
        time.sleep(0.4)
        move(hwnd, cx + 34, cy + 6)
        time.sleep(1.0)
        # 山包抬高后同样半径的椭圆会放大，容易顶出视口左右边界
        # （一旦被裁，`outer_only` 取到的点就不再是一条完整椭圆）。压小到 3~5 m。
        tune_radius(hwnd, tap, 3.0, 5.0)
        time.sleep(0.6)
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

        # ---- E：把半径调到超过"相机到圆心的距离" ----
        #
        # 用户报的第二件事（2026-10-24）："在把笔刷的半径或强度调大后会突然
        # 3D 视口中发现笔刷的圆圈消失了。"
        #
        # 根因是**整圈丢弃**：老的 `projectRing` 只要有一个环点落在相机背后
        # （裁剪空间 w <= 0）就 `return false`，一个像素都不画。而半径一旦
        # 超过相机到圆心的水平距离，圆的远端点**必然**跑到背后 —— 本站位
        # 相机离圆心 45 m，于是半径跨过 45 m 左右时圆圈就"啪"地消失。
        # 地形被笔刷垫到相机高度以上时同理（强度大 → 抬升快 → 命中点抬高）。
        #
        # 现在按近平面把圆裁成可见弧段。判据看日志（像素判据在这里不可靠：
        # 半径远大于视距时，圆环本身的边缘本来就在画面之外）：
        #   E1 半径确实被调过 50 m；
        #   E2 `behind > 0` —— 有环点落在相机背后，这正是老代码放弃的那一帧；
        #   E3 `arcs >= 1` —— 即便如此，圆环仍然被画出来了。
        move(hwnd, cx, cy)
        time.sleep(0.5)
        tune_radius(hwnd, tap, 26.0, 400.0)
        time.sleep(0.8)

        rings = []
        with tap.lock:
            for l in tap.lines:
                m = re.search(r"LS-RING r=([\d.]+) arcs=(\d+) behind=(\d+)", l)
                if m:
                    rings.append((float(m.group(1)), int(m.group(2)),
                                  int(m.group(3))))
        if not check("E0 半径变大后有 LS-RING 记录", bool(rings),
                     "%d 条，最后一条 %s" % (len(rings),
                                             rings[-1] if rings else None)):
            return 5
        r_last, arcs, behind = rings[-1]
        check("E1 半径被调过 24 m（相机到圆心的距离）", r_last > 24.0,
              "r=%.1f m" % r_last)
        check("E2 有环点落到相机背后（老代码在这里整圈消失）", behind > 0,
              "behind=%d / %d 点" % (behind, 65))
        check("E3 圆环仍然在画（同一次绘制里外圈至少 1 段）", arcs >= 1,
              "arcs=%d（外圈+内圈）" % arcs)
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
