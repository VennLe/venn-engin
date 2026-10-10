"""诊断探针：缩放模式下，拖 X / Y / Z 三根轴，各自到底改了 scale 的哪一个分量？

背景（用户报告）：
    "切换到 Scale 调整物体大小时，拖拽物体的 y 轴怎么是 z 轴方向在拉伸，
     拖 z 轴是 y 轴在拉伸"

这个脚本不猜坐标：手柄的轴端点位置由编辑器自己打在日志里
（GZ-ORIGIN / GZ-AXIS-X / GZ-AXIS-Y / GZ-AXIS-Z），脚本只做三件事：
    1. Shift+A 加一个 Cube（加完自动选中）
    2. 依次沿每根轴的屏幕方向拖动鼠标
    3. 读 `gizmo begin:` / `gizmo end:` 两行日志，判断
       "拖第 i 根轴 → scale[i] 变化" 这个不变量成不成立

输出三行结论，直接指出哪根轴串了。
"""

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                       # noqa: E402
from verify_add_menu import (LogTap, kill_stale_editors,             # noqa: E402
                             open_menu_and_click)
from verify_asset_drag import force_foreground, move_to              # noqa: E402
from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

BEGIN_RE = re.compile(
    r"gizmo begin: handle=(\S+) mode=(\S+) space=(\S+) "
    r"axis=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")
END_RE = re.compile(
    r"gizmo end: handle=(\S+) '([^']*)' pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"rot=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"scale=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")


def wait_rect(tap, tag, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r:
            return r
        time.sleep(0.15)
    return None


def n_begin(tap):
    with tap.lock:
        return sum(1 for l in tap.lines if "gizmo begin:" in l)


def last_match(tap, rx):
    with tap.lock:
        for line in reversed(tap.lines):
            m = rx.search(line)
            if m:
                return m
    return None


def count_matches(tap, rx):
    with tap.lock:
        return sum(1 for l in tap.lines if rx.search(l))


def drag(hwnd, src, dx, dy, steps=14):
    """从 src（客户区坐标）按住左键，沿 (dx, dy) 分步拖出去再松手。"""
    move_to(hwnd, src[0], src[1])
    time.sleep(0.35)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.20)
    for k in range(1, steps + 1):
        move_to(hwnd, src[0] + dx * k / steps, src[1] + dy * k / steps)
        time.sleep(0.035)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    ap.add_argument("--reach", type=float, default=1.7,
                    help="拖动距离 = 手柄屏幕长度 x 该倍数")
    ap.add_argument("--cam", default="",
                    help="可选的 MYVK_EDITOR_CAM 覆盖（默认用编辑器自带视角）")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件：", exe)
        return 1

    kill_stale_editors()

    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_GIZMO"] = "scale"
    if args.cam:
        env["MYVK_EDITOR_CAM"] = args.cam

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        tw, th = (int(v) for v in args.size.lower().split("x"))
        user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
        time.sleep(2.5)
        force_foreground(hwnd)
        time.sleep(1.0)

        vp = tap.last_rect("VP-RECT")
        if not vp:
            print("没有 VP-RECT 日志，无法定位视口")
            return 3
        mx = int((vp[0] + vp[2]) * 0.5)
        my = int((vp[1] + vp[3]) * 0.5)
        move_to(hwnd, mx, my)
        time.sleep(0.5)

        # ---- 加一个 Cube（加完自动选中，手柄随之上屏）----
        if not open_menu_and_click(tap, hwnd, mx, my,
                                   "VP-ADD-Mesh", "VP-ADD-Cube", "Cube"):
            print("Shift+A 加 Cube 失败")
            return 4
        move_to(hwnd, mx + 60, my + 40)
        time.sleep(0.6)

        origin = wait_rect(tap, "GZ-ORIGIN", 6.0)
        tips = {a: wait_rect(tap, "GZ-AXIS-" + a, 6.0) for a in "XYZ"}
        print("GZ-ORIGIN  :", origin)
        for a in "XYZ":
            print("GZ-AXIS-%s : %s" % (a, tips[a]))
        if not origin or any(t is None for t in tips.values()):
            print("手柄矩形日志不全，无法继续")
            return 5

        ox = (origin[0] + origin[2]) * 0.5
        oy = (origin[1] + origin[3]) * 0.5

        # 手柄的屏幕长度 ≈ 90px；用最长的那个 tip 距离当参考
        dists = {}
        for a in "XYZ":
            tx = (tips[a][0] + tips[a][2]) * 0.5
            ty = (tips[a][1] + tips[a][3]) * 0.5
            dists[a] = ((tx - ox) ** 2 + (ty - oy) ** 2) ** 0.5
        print("三根轴的屏幕长度(px):", {a: round(v, 1) for a, v in dists.items()})

        base_scale = (1.0, 1.0, 1.0)   # Cube 刚加出来是 (1,1,1)
        results = {}
        for a in "XYZ":
            tx = (tips[a][0] + tips[a][2]) * 0.5
            ty = (tips[a][1] + tips[a][3]) * 0.5
            ux, uy = tx - ox, ty - oy
            ln = max((ux * ux + uy * uy) ** 0.5, 1e-3)
            ux, uy = ux / ln, uy / ln

            # ⚠ 起点不能是手柄中心 —— 中心那个"整体缩放"方块的命中半径
            # 有 10px，从原点按下去抓到的一律是它（第一次跑就是这么错的：
            # 三根轴测出来全都是等比缩放）。从轴线的 3/4 处下手。
            sx = ox + ux * ln * 0.75
            sy = oy + uy * ln * 0.75
            reach = ln * args.reach
            dx, dy = ux * reach, uy * reach

            n0 = n_begin(tap)
            drag(hwnd, (sx, sy), dx, dy)
            time.sleep(0.4)

            if n_begin(tap) == n0:
                print("轴 %s：没有产生拖拽（没抓到手柄？）" % a)
                results[a] = None
                continue
            m = last_match(tap, END_RE)
            if not m:
                print("轴 %s：有 begin 但没有 end 日志" % a)
                results[a] = None
                continue
            bm = last_match(tap, BEGIN_RE)
            scale = tuple(float(m.group(i)) for i in (9, 10, 11))
            # 相对上一次的增量，取绝对值最大者 = "实际被改的是哪一维"
            deltas = [scale[i] - base_scale[i] for i in range(3)]
            print("轴 %s: 抓到 handle=%s  拖后 scale=(%.3f, %.3f, %.3f)"
                  "  Δ=(%+.3f, %+.3f, %+.3f)"
                  % (a, bm.group(1) if bm else "?",
                     scale[0], scale[1], scale[2],
                     deltas[0], deltas[1], deltas[2]))
            results[a] = deltas
            base_scale = scale

        print()
        idx = {"X": 0, "Y": 1, "Z": 2}
        ok = True
        for a in "XYZ":
            d = results.get(a)
            if d is None:
                ok = False
                continue
            hit = max(range(3), key=lambda i: abs(d[i]))
            good = (hit == idx[a]) and abs(d[idx[a]]) > 1e-3
            print("轴 %s 的拖拽实际改变了 scale.%s  -> %s"
                  % (a, "XYZ"[hit], "OK" if good else "错位 !!"))
            ok = ok and good
        print()
        print("结论：", "三根轴一一对应，没有问题" if ok else "存在轴错位")
        return 0 if ok else 6
    finally:
        # 顺带把读到的日志转储一份，方便人工核对
        with tap.lock:
            dump = [l for l in tap.lines
                    if l.startswith("gizmo ") or "GZ-" in l]
        print("\n---- 相关日志 ----")
        for l in dump[-40:]:
            print(" ", l)
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
