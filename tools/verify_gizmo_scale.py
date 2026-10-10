"""端到端验证：缩放（Scale）手柄的"拖哪根轴，就沿哪个方向长"。

背景 —— 用户报告：
    "在 3d 视图中，切换到 Scale 调整物体大小时，拖拽物体的 y 轴怎么是 z 轴
     方向在拉伸，拖动 z 轴是 y 轴在拉伸"

根因：`TransformComponent::scale` 是**局部**的（M = T·R·S），而手柄拖的是
**世界**里的方向。物体一转，两套轴就不再重合 —— 绕 X 转 +90° 之后，物体的
局部 Y 正指着世界 Z。旧代码把拖拽倍率直接写进 `scale[手柄下标]`，于是
"拖绿轴（世界 Y）"改的是 local Y = 世界 Z，物体会**竖着**长；"拖蓝轴
（世界 Z）"改的是 local Z = 世界 -Y，物体会**横着**长。看起来就是两根轴
换了位置。

这个脚本断言的**不是**"拖 Y 改 scale.y"，而是真正该成立的不变量：

    物体在世界里长大的方向  ≈  被拖动的那根手柄的世界方向

长大方向 = Σ (Δscale[i] · 局部轴 i 在世界里的方向)，两个量都能从日志拿到：
    gizmo begin: handle=<轴> mode=Scale space=<世界/局部>
                 axis=(世界方向) localX=(..) localY=(..) localZ=(..)
    gizmo end:   handle=<轴> '<名字>' ... scale=(x,y,z)

用例：
    A. 未旋转的物体 + 世界坐标系 —— 三根轴各拖一次（老行为，必须不许坏）
    B. 绕 X 转 90° 的物体 + 世界坐标系 —— 就是用户报的那个场景
    C. 绕 X 转 90° 的物体 + 局部坐标系 —— 拖局部轴必须只改那一维
    D. 未旋转的物体 + 平面手柄（XY）—— 只改平面内两维，法线那一维不动

用法：
    python tools/verify_gizmo_scale.py
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                       # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402
from verify_asset_drag import force_foreground, move_to              # noqa: E402
from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# 手柄的屏幕长度约 90px。
#   ⚠ 抓取点必须落在**轴端的小方块**上（t = 1.0），不能取轴线的中间某处：
#     平面方片画在 0.30~0.62 的位置，而透视投影下"62% 的世界长度"在屏幕上
#     会超出 62% 的屏幕长度 —— 取 0.75 时实测正好压进 XZ 方片的命中区里，
#     hitTest 的判定顺序是 平面 → 单轴，于是拖"Y 轴"抓到了"XZ 平面"。
#     轴端在 1.0L，离方片够远，且正好是轴命中判定的终点（距离=0）。
GRAB_T = 1.0
DRAG_REACH = 1.4

BEGIN_RE = re.compile(
    r"gizmo begin: handle=(\S+) mode=(\S+) space=(\S+) "
    r"axis=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"localX=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"localY=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"localZ=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")
END_RE = re.compile(
    r"gizmo end: handle=(\S+) '([^']*)' "
    r"pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"rot=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"scale=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")


# ------------------------------------------------------------------ 工具

def vec3(m, i0):
    return (float(m.group(i0)), float(m.group(i0 + 1)), float(m.group(i0 + 2)))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def norm(v):
    n = math.sqrt(sum(x * x for x in v))
    return tuple(x / n for x in v) if n > 1e-9 else (0.0, 0.0, 0.0)


def last_match(tap, rx):
    with tap.lock:
        for line in reversed(tap.lines):
            m = rx.search(line)
            if m:
                return m
    return None


def n_matches(tap, rx):
    with tap.lock:
        return sum(1 for l in tap.lines if rx.search(l))


def wait_rect(tap, tag, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r:
            return r
        time.sleep(0.15)
    return None


def drag(hwnd, src, dst, steps=14):
    move_to(hwnd, src[0], src[1])
    time.sleep(0.35)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.20)
    for k in range(1, steps + 1):
        move_to(hwnd, src[0] + (dst[0] - src[0]) * k / steps,
                src[1] + (dst[1] - src[1]) * k / steps)
        time.sleep(0.035)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.55)


def center(rect):
    return ((rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5)


def launch(exe, scene, entity, space, size, gizmo="scale"):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_GIZMO"] = gizmo
    env["MYVK_EDITOR_SPACE"] = space
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_SELECT"] = entity
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor")
    if not hwnd:
        proc.terminate()
        return None, None, None
    user32.ShowWindow(hwnd, 9)
    time.sleep(2.0)
    tw, th = (int(v) for v in size.lower().split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(1.0)
    return proc, tap, hwnd


def stop(proc):
    if not proc:
        return
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


# ------------------------------------------------------------------ 主流程

def make_scene(path):
    """未旋转的 PlainCube 在原点；RotCube 在 (3,0,0) 且绕 X 转 +90°。

    转 +90° 之后它的局部 Y ≡ 世界 +Z、局部 Z ≡ 世界 -Y —— 正是能把
    "拖 Y 却在 Z 方向长"这个 bug 稳定复现出来的最小配置。
    """
    scene = {
        "version": 1,
        "generator": "verify_gizmo_scale",
        "entities": [
            {"name": "PlainCube",
             "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                           "scale": [1, 1, 1]},
             "mesh": {"kind": "builtin", "shape": "cube", "size": 2.0,
                      "name": "cube"}},
            {"name": "RotCube",
             "transform": {"position": [3, 0, 0],
                           "rotation": [math.pi / 2, 0, 0],
                           "scale": [1, 1, 1]},
             "mesh": {"kind": "builtin", "shape": "cube", "size": 2.0,
                      "name": "cube"}},
        ],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, indent=2)


def run_case(exe, scene, entity, space, size, label, cases, fails):
    """一个用例：起编辑器 → 依次拖动手柄 → 逐条断言。"""
    print("\n=== %s（%s / %s 坐标系）===" % (label, entity, space))
    proc, tap, hwnd = launch(exe, scene, entity, space, size)
    if not hwnd:
        print("  起不来编辑器")
        fails.append(label + ": no window")
        return
    try:
        origin = wait_rect(tap, "GZ-ORIGIN", 8.0)
        tips = {a: wait_rect(tap, "GZ-AXIS-" + a, 8.0) for a in "XYZ"}
        planes = {k: tap.last_rect("GZ-PLANE-" + k) for k in
                  ("YZ", "XZ", "XY")}
        if not origin or any(v is None for v in tips.values()):
            print("  手柄没出现：", origin, tips)
            with tap.lock:
                print("\n".join("    " + l for l in tap.lines[-12:]))
            fails.append(label + ": no gizmo")
            return

        ox, oy = center(origin)
        prev = (1.0, 1.0, 1.0)      # 场景里两颗立方体的 scale 都是 1

        def do_drag(name, grab, expect_handle):
            """拖一次，返回 (begin_match, scale, delta)；抓错手柄返回 None。"""
            nonlocal prev
            grab = (float(grab[0]), float(grab[1]))
            axis_dir = (grab[0] - ox, grab[1] - oy)
            ln = math.hypot(*axis_dir)
            ux, uy = axis_dir[0] / ln, axis_dir[1] / ln
            dst = (grab[0] + ux * ln * DRAG_REACH,
                   grab[1] + uy * ln * DRAG_REACH)
            n0 = n_matches(tap, BEGIN_RE)
            drag(hwnd, grab, dst)
            time.sleep(0.4)
            if n_matches(tap, BEGIN_RE) == n0:
                print("  [%s] 没抓到任何手柄" % name)
                fails.append("%s/%s: not grabbed" % (label, name))
                return None
            bm = last_match(tap, BEGIN_RE)
            em = last_match(tap, END_RE)
            if not em:
                fails.append("%s/%s: no end log" % (label, name))
                return None
            # ⚠ 先更新基准值再做断言：抓错手柄时物体**已经**被改了，
            #   不认账的话下一个轴的 Δ 会把这个意外改动算进去，
            #   于是后面每一条都跟着假失败（踩过）。
            sc = vec3(em, 9)      # scale=(a,b,c) 是 END_RE 的第 9/10/11 组
            d = tuple(sc[i] - prev[i] for i in range(3))
            prev = sc
            if bm.group(1) != expect_handle:
                print("  [%s] 抓错了手柄：期望 %s，实际 %s（Δ=(%+.3f,%+.3f,%+.3f)）"
                      % (name, expect_handle, bm.group(1), d[0], d[1], d[2]))
                fails.append("%s/%s: hit %s" % (label, name, bm.group(1)))
                return None
            if bm.group(2) != "Scale":
                fails.append("%s/%s: mode=%s" % (label, name, bm.group(2)))
                return None
            return bm, sc, d

        def growth_dir(bm, d):
            L = [vec3(bm, 7), vec3(bm, 10), vec3(bm, 13)]   # localX/Y/Z
            g = tuple(sum(d[i] * L[i][k] for i in range(3)) for k in range(3))
            return norm(g)

        def assert_along(name, bm, d, tol=0.98):
            """核心断言：物体在世界里长大的方向 ≈ 手柄的世界方向。"""
            axis = norm(vec3(bm, 4))
            c = abs(dot(growth_dir(bm, d), axis))
            changed = max(abs(x) for x in d)
            ok = changed > 1e-3 and c >= tol
            print("  [%s] Δscale=(%+.3f,%+.3f,%+.3f)  长大方向 %s  vs  手柄方向 %s"
                  "  |cos|=%.4f  -> %s"
                  % (name, d[0], d[1], d[2],
                     tuple(round(x, 3) for x in growth_dir(bm, d)),
                     tuple(round(x, 3) for x in axis), c,
                     "OK" if ok else "错位 !!"))
            if not ok:
                fails.append("%s/%s: growth |cos|=%.4f" % (label, name, c))
            return ok

        def assert_in_plane(name, bm, d, tol=0.05):
            """平面手柄：长大方向必须**垂直**于平面法线（也就是躺在平面里）。"""
            n = norm(vec3(bm, 4))            # 平面手柄的 axis = 平面法线
            c = abs(dot(growth_dir(bm, d), n))
            changed = max(abs(x) for x in d)
            ok = changed > 1e-3 and c <= tol
            print("  [%s] 长大方向 %s · 平面法线 %s  = %.4f（应在平面内，≈0）-> %s"
                  % (name, tuple(round(x, 3) for x in growth_dir(bm, d)),
                     tuple(round(x, 3) for x in n), c, "OK" if ok else "错位 !!"))
            if not ok:
                fails.append("%s/%s: out of plane %.4f" % (label, name, c))
            return ok

        for c in cases:
            if c["kind"] == "axis":
                a = c["axis"]
                t = center(tips[a])
                grab = (ox + (t[0] - ox) * GRAB_T, oy + (t[1] - oy) * GRAB_T)
                res = do_drag(c["name"], grab, a)
                if not res:
                    continue
                bm, sc, d = res
                assert_along(c["name"], bm, d)
                if c.get("expect_component") is not None:
                    want = c["expect_component"]
                    hit = max(range(3), key=lambda i: abs(d[i]))
                    ok = hit == want and abs(d[want]) > 1e-3
                    print("     期望只改 scale.%s，实际最大增量在 scale.%s -> %s"
                          % ("XYZ"[want], "XYZ"[hit], "OK" if ok else "错位 !!"))
                    if not ok:
                        fails.append("%s/%s: component %s != %s"
                                     % (label, c["name"], "XYZ"[hit],
                                        "XYZ"[want]))
            else:   # plane
                rect = planes.get(c["plane"])
                if not rect:
                    print("  [%s] 平面方片日志缺失" % c["name"])
                    fails.append("%s/%s: no plane rect" % (label, c["name"]))
                    continue
                res = do_drag(c["name"], center(rect), c["plane"])
                if not res:
                    continue
                bm, sc, d = res
                print("  [%s] 抓到 handle=%s  Δscale=(%+.3f,%+.3f,%+.3f)"
                      % (c["name"], bm.group(1), d[0], d[1], d[2]))
                assert_in_plane(c["name"], bm, d)
                if c.get("frozen_component") is not None:
                    fz = c["frozen_component"]
                    ok = abs(d[fz]) < 1e-3
                    print("     法线那一维 scale.%s 必须不动（Δ=%+.4f）-> %s"
                          % ("XYZ"[fz], d[fz], "OK" if ok else "错位 !!"))
                    if not ok:
                        fails.append("%s/%s: normal axis moved"
                                     % (label, c["name"]))
    finally:
        stop(proc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    scene = os.path.join(tempfile.gettempdir(), "venn_gizmo_scale_probe.json")
    make_scene(scene)
    print("测试场景：", scene)

    fails = []
    kill_stale_editors()

    # ---- A. 未旋转 + 世界：老行为不许坏 ----
    run_case(exe, scene, "PlainCube", "world", args.size, "A 未旋转/世界", [
        {"kind": "axis", "axis": "X", "name": "X 轴", "expect_component": 0},
        {"kind": "axis", "axis": "Y", "name": "Y 轴", "expect_component": 1},
        {"kind": "axis", "axis": "Z", "name": "Z 轴", "expect_component": 2},
    ], fails)

    kill_stale_editors()

    # ---- B. 旋转 90° + 世界：用户报的场景 ----
    run_case(exe, scene, "RotCube", "world", args.size, "B 旋转90°/世界", [
        {"kind": "axis", "axis": "X", "name": "X 轴"},
        {"kind": "axis", "axis": "Y", "name": "Y 轴"},
        {"kind": "axis", "axis": "Z", "name": "Z 轴"},
    ], fails)

    kill_stale_editors()

    # ---- C. 旋转 90° + 局部：拖局部轴只改那一维 ----
    run_case(exe, scene, "RotCube", "local", args.size, "C 旋转90°/局部", [
        {"kind": "axis", "axis": "X", "name": "局部 X", "expect_component": 0},
        {"kind": "axis", "axis": "Y", "name": "局部 Y", "expect_component": 1},
        {"kind": "axis", "axis": "Z", "name": "局部 Z", "expect_component": 2},
    ], fails)

    kill_stale_editors()

    # ---- D. 平面手柄：只碰平面内两维 ----
    run_case(exe, scene, "PlainCube", "world", args.size, "D 平面手柄", [
        {"kind": "plane", "plane": "XY", "name": "XY 方片",
         "frozen_component": 2},
    ], fails)

    print()
    if fails:
        print("失败项：")
        for f in fails:
            print("  -", f)
        print("\nRESULT: FAIL")
        return 4
    print("全部断言 PASS —— 拖哪根轴，物体就沿那个方向长")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
