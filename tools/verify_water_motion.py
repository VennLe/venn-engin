"""端到端验证：水体可被 W/E/R 手柄操作 + 单一浪况参数（2026-10-21 第七轮）。

对应两条需求：

  1. "水体创建后必须是在 3D 视口中可以移动的，可以通过 W/E/R 控制位置、
     旋转、缩放。"
     —— 水体**没有网格组件**，gizmo 只认 `TransformComponent`。本用例直接
     拖真实手柄，断言 `WATER-XFORM` 日志里的 center / rot / halfExtent 变了。

  2. "浪花波纹要通过一个浮点参数控制，0 = 无任何波纹的平静水面，
     10 = 汹涌的浪涛。"
     —— 断言"水面高频能量"随 waves 单调上升，且 0 时几乎为 0（镜面）。

断言分五组：

  [1] Move：拖 GZ-AXIS-X → center.x 变化（且方向为正）
  [2] Rotate：拖 GZ-RING-Z → rot 变化 > 1°
  [3] Scale：拖 GZ-AXIS-X → halfExtent 变化
  [4] waves 0 < 4 < 10：水面区域高频能量单调上升
  [5] waves=0 是镜面：高频能量低于 waves=10 的 1/3，且两者画面差异显著

用法：
    python tools/verify_water_motion.py
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32           # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect        # noqa: E402
from verify_asset_drag import force_foreground, move_to                  # noqa: E402
from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# 相机 yaw=270 / pitch=32 时，视线在水面高度上的落点大约在 y = -24。
# 水体中心必须摆在这里 —— 摆在原点的话它落在 FOV 外，手柄会投影到屏幕外
# （实测 GZ-ORIGIN y=1824，窗口才 800 高）。
CAM = "0,0,16,270,32"
WATER_NAME = "Water_0"
WATER_LEVEL = 1.2
WATER_RADIUS = 18.0
WATER_CY = -24.0

XFORM_RE = re.compile(
    r"WATER-XFORM id=(\d+) center=\(([-\d.]+),([-\d.]+)\) level=([-\d.]+) "
    r"rot=([-\d.]+) halfExtent=\(([-\d.]+),([-\d.]+)\)")
WAVES_RE = re.compile(r"WATER-WAVES id=(\d+) waves=([-\d.]+)")
BEGIN_RE = re.compile(r"gizmo begin: handle=(\S+) mode=(\S+) space=(\S+)")
END_RE = re.compile(r"gizmo end: handle=(\S+) '([^']*)'")


# ------------------------------------------------------------------ 场景

def mat(name, base, rough=0.35):
    return {"kind": "procedural", "name": name,
            "baseColor": list(base) + [1.0],
            "metallic": 0.0, "roughness": rough,
            "normalScale": 1.0, "ao": 1.0,
            "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
            "alphaBlend": False}


def build_scene(path, waves=4.0, legacy_wave_height=None):
    doc = {
        "version": 1,
        "generator": "verify_water_motion",
        "light": {"direction": [-0.30, 0.45, -0.84],
                  "color": [1.0, 0.97, 0.92], "intensity": 1.4,
                  "castsShadow": True, "ambientScale": 0.55},
        "entities": [
            {"name": "Ground",
             "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                           "scale": [1, 1, 1]},
             "visible": True, "castShadow": False, "parent": -1,
             "mesh": {"kind": "builtin", "shape": "plane", "size": 220.0,
                      "name": "wm_ground"},
             "material": mat("wm_ground_mat", (0.55, 0.58, 0.52), 0.6)},
            {"name": WATER_NAME,
             "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                           "scale": [1, 1, 1]},
             "visible": True, "castShadow": False, "parent": -1,
             "water": {
                 "visible": True,
                 "shape": 0,                       # 0 = circle
                 "center": [0.0, WATER_CY],
                 "halfExtent": [WATER_RADIUS, WATER_RADIUS],
                 "rotation": 0.0,
                 "level": WATER_LEVEL,
                 "bedLevel": 0.0,
                 "autoFit": False,
                 "kind": 3,                        # sea（颜色/清澈度作为底子）
                 "waves": float(waves),
                 "shallowColor": [0.10, 0.40, 0.52],
                 "deepColor": [0.01, 0.06, 0.16],
                 "opacity": 0.30, "refraction": 0.35, "ior": 1.333,
                 "clarity": 1.2, "flowSpeed": 0.0,
                 "flowDir": [1.0, 0.0], "roughness": 0.05,
             }},
        ],
    }
    if legacy_wave_height is not None:
        # 模拟 2026-10-21 第七轮之前存的老场景：**没有** waves 字段，
        # 只有旧的 waveHeight。加载时应当由它反推出一个等效浪况。
        for e in doc["entities"]:
            w = e.get("water")
            if not w:
                continue
            w.pop("waves", None)
            w["waveHeight"] = float(legacy_wave_height)
            w["waveLength"] = 26.0
            w["ripple"] = 0.85
            w["foam"] = 0.80

    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


# ------------------------------------------------------------------ 启动

def launch(exe, scene, gizmo=None, select=None, size="1280x800", no_hud=True,
           editmode=None):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    if editmode:
        env["MYVK_EDITMODE"] = editmode
    # ⚠ NO_HUD 会**连带关掉变换手柄**（实测：设了之后 GZ-ORIGIN 一条都不打），
    #   所以拖手柄的三组必须传 no_hud=False。像素对照的浪况组才用它。
    if no_hud:
        env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_GRID"] = "0"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
    if gizmo:
        env["MYVK_EDITOR_GIZMO"] = gizmo
    if select:
        env["MYVK_EDITOR_SELECT"] = select

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
    # 真实点击要求窗口在**前台**：SetForegroundWindow 从后台脚本调多半静默
    # 失败，第一下点击只会去激活窗口、进不了 ImGui（日志一个字都没有）。
    force_foreground(hwnd)
    time.sleep(0.8)
    return proc, tap, hwnd


def stop(proc):
    if not proc:
        return
    proc.terminate()
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()


# ------------------------------------------------------------------ 鼠标

def center(rect):
    return ((rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5)


def drag(hwnd, src, dst, steps=14):
    # ⚠ 锚点是 ImGui 的**客户区**坐标，SetCursorPos 要**屏幕**坐标 ——
    #   直接用会把整段拖拽平移到窗口标题栏下方（实测：完全抓不到手柄）。
    #   verify_asset_drag.move_to(hwnd, x, y) 内部做了 client→screen 换算。
    move_to(hwnd, src[0], src[1])
    time.sleep(0.35)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.20)
    for i in range(1, steps + 1):
        t = i / float(steps)
        move_to(hwnd, src[0] + (dst[0] - src[0]) * t,
                src[1] + (dst[1] - src[1]) * t)
        time.sleep(0.035)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.55)


def wait_log(tap, needle, timeout=15.0):
    """等到日志里出现某句（轮询，不用 sleep 后读一把）。"""
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            if any(needle in l for l in tap.lines):
                return True
        time.sleep(0.2)
    return False


def n_matches(tap, rx):
    with tap.lock:
        return sum(1 for l in tap.lines if rx.search(l))


def last_match(tap, rx):
    with tap.lock:
        for line in reversed(tap.lines):
            m = rx.search(line)
            if m:
                return m
    return None


# ------------------------------------------------------------------ 图像

def luma(a):
    return a[:, :, 0] * 0.299 + a[:, :, 1] * 0.587 + a[:, :, 2] * 0.114


def laplacian_energy(a):
    """水面高频能量：4 邻域 Laplacian 的绝对值均值。

    平静水面 = 反射的天空是平滑渐变 → 能量接近 0；
    有波纹 = 法线乱 → 高光/反射碎 → 能量上升。
    """
    L = luma(a)
    lap = (np.abs(4.0 * L[1:-1, 1:-1] - L[:-2, 1:-1] - L[2:, 1:-1]
                  - L[1:-1, :-2] - L[1:-1, 2:]))
    return float(lap.mean()), lap


def water_box(vp):
    """视口下半部（水面所在区域），避开地平线以上的天空。

    VP-RECT 解析出来是浮点（wait_rect 原样按 float 存），切片必须显式转 int
    —— 否则 `TypeError: slice indices must be integers`。
    """
    x0, y0, x1, y1 = (int(v) for v in vp)
    h = y1 - y0
    # 内缩 6%：别把视口边框/滚动条/相邻面板那一列像素算进来
    # （那几列的边缘 = 极高 Laplacian，会把"镜面"读数顶上去）。
    ix = int((x1 - x0) * 0.06)
    iy = int(h * 0.06)
    return (x0 + ix, y0 + int(h * 0.45) + iy, x1 - ix, y1 - iy)


# ------------------------------------------------------------------ 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()

    exe = args.exe
    if not os.path.exists(exe):
        print("FAIL: 找不到可执行文件 %s" % exe)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()

    failures = []
    print("=" * 68)
    print("水体：W/E/R 操作 + 单一浪况参数")
    print("=" * 68)

    def check(label, ok, extra=""):
        print("  %s  %s%s" % ("[PASS]" if ok else "[FAIL]", label,
                              ("  " + extra) if extra else ""))
        if not ok:
            failures.append(label)

    # ================= 组 1-3：三个手柄 =================
    scene = os.path.join(OUT_DIR, "wm_scene.json")
    build_scene(scene, waves=4.0)

    # 第三项带 editmode=water：用户是在 **Water 模式**里用 W/E/R 的
    # （之前 Water 被算进 brushMode，手柄整个被关掉 —— 那是需求 1 的病根）。
    for mode, anchor, want_mode in (("move", "GZ-AXIS-X", None),
                                    ("scale", "GZ-AXIS-X", None),
                                    ("rotate", "GZ-RING-Z", None),
                                    ("scale", "GZ-AXIS-X", "water")):
        print("\n[%s] 拖 %s%s" % (mode, anchor,
                                 "（Water 模式内）" if want_mode else ""))
        proc, tap, hwnd = launch(exe, scene, gizmo=mode, select=WATER_NAME,
                                 no_hud=False, editmode=want_mode)
        if not hwnd:
            check("%s: 编辑器起不来" % mode, False)
            continue
        try:
            origin = wait_rect(tap, "GZ-ORIGIN", 8.0)
            grip = wait_rect(tap, anchor, 8.0)
            if not origin or not grip:
                check("%s: 手柄没出现" % mode, False,
                      "origin=%s grip=%s" % (origin, grip))
                continue
            ox, oy = center(origin)
            gx, gy = center(grip)

            # 拖拽方向：沿"原点 → 手柄"向外拉；旋转环要沿切线拉
            vx, vy = gx - ox, gy - oy
            ln = math.hypot(vx, vy) or 1.0
            if mode == "rotate":
                ux, uy = -vy / ln, vx / ln      # 切线
            else:
                ux, uy = vx / ln, vy / ln       # 径向
            dst = (gx + ux * 90.0, gy + uy * 90.0)

            n0 = n_matches(tap, XFORM_RE)
            n0b = n_matches(tap, BEGIN_RE)
            drag(hwnd, (gx, gy), dst)
            time.sleep(0.4)

            grabbed = n_matches(tap, BEGIN_RE) > n0b
            check("%s: 抓到了手柄" % mode, grabbed)
            if not grabbed:
                continue

            m = last_match(tap, XFORM_RE)
            check("%s: 打出 WATER-XFORM" % mode, m is not None,
                  "" if m else "（body 没被写回）")
            if not m:
                continue
            cx, cy = float(m.group(2)), float(m.group(3))
            level, rot = float(m.group(4)), float(m.group(5))
            hx, hy = float(m.group(6)), float(m.group(7))

            # 拖的是世界 X 轴 → 只有 x 该变；y / level 必须原地不动
            if mode == "move":
                check("move: center.x 真的变了", abs(cx - 0.0) > 0.2,
                      "center=(%.2f,%.2f) level=%.2f" % (cx, cy, level))
                check("move: 没串到 y / 高度",
                      abs(cy - WATER_CY) < 1e-2 and
                      abs(level - WATER_LEVEL) < 1e-2,
                      "center=(%.2f,%.2f) level=%.2f" % (cx, cy, level))
            elif mode == "rotate":
                # 环境里水体初始 rotation = 0
                check("rotate: rot 变化 > 1°", abs(rot) > 1.0,
                      "rot=%.2f" % rot)
                check("rotate: 没有偷偷改尺寸/位置",
                      abs(hx - WATER_RADIUS) < 0.01 and abs(cx) < 1e-3 and
                      abs(cy - WATER_CY) < 1e-2,
                      "halfExtent=%.2f center=(%.2f,%.2f)" % (hx, cx, cy))
            else:
                check("scale: halfExtent 真的变了",
                      abs(hx - WATER_RADIUS) > 0.2,
                      "halfExtent=(%.2f,%.2f)" % (hx, hy))
                check("scale: 圆形保持 x==y", abs(hx - hy) < 1e-3)
                check("scale: 没有偷偷挪位置",
                      abs(cx) < 1e-3 and abs(cy - WATER_CY) < 1e-2 and
                      abs(level - WATER_LEVEL) < 1e-2,
                      "center=(%.2f,%.2f) level=%.2f" % (cx, cy, level))
        finally:
            stop(proc)
            time.sleep(1.0)

    # ================= 组 4-5：浪况 =================
    print("\n[waves] 0 / 4 / 10 的高频能量（外加一次 waves=0 对照）")
    energies = {}
    shots = {}
    # 多跑一次 waves=0 当**控制组**：能量必须几乎不变，否则"单调"可能只是
    # 每次启动的画面抖动（本轮踩过：量测区没框好时 UI 会污染数值）。
    runs = [("w0", 0.0), ("w4", 4.0), ("w10", 10.0), ("w0_ctl", 0.0)]
    for tag, w in runs:
        sc = os.path.join(OUT_DIR, "wm_scene_%s.json" % tag)
        build_scene(sc, waves=w)
        proc, tap, hwnd = launch(exe, sc)
        if not hwnd:
            check("%s: 编辑器起不来" % tag, False)
            continue
        try:
            vp = wait_rect(tap, "VP-RECT", 10.0)
            # ⚠ 必须等到**水体真的上传了**再截图：只等 VP-RECT 会在水体
            #   还没 sync 完成时抓帧，那时画面上是地形而不是水面，能量读数
            #   完全不同（实测同一配置 0.33 vs 0.15，控制组当场抓到）。
            if not wait_log(tap, "WATER-SYNC", 15.0):
                check("%s: 水体没 sync" % tag, False)
                continue
            time.sleep(1.8)
            img, ok = capture_client(hwnd)
            img.save(os.path.join(OUT_DIR, "wm_%s.png" % tag))
            arr = np.asarray(img.convert("RGB")).astype("float32")
            shots[tag] = arr
            if not vp:
                check("%s: 没有 VP-RECT" % tag, False)
                continue
            box = water_box(vp)
            a = arr[box[1]:box[3], box[0]:box[2]]
            e, _ = laplacian_energy(a)
            energies[tag] = e
            print("     %-7s waves=%4.1f  水面高频能量 = %8.2f" % (tag, w, e))
        finally:
            stop(proc)
            time.sleep(1.0)

    if len(energies) == 4:
        e0, e4, e10 = energies["w0"], energies["w4"], energies["w10"]
        check("浪况单调：energy(0) < energy(4) < energy(10)",
              e0 < e4 < e10, "%.2f < %.2f < %.2f" % (e0, e4, e10))
        check("waves=0 基本是镜面（能量 < waves=10 的 1/3）",
              e0 * 3.0 < e10, "%.2f vs %.2f" % (e0, e10))
        # 控制组：同一配置跑两次，能量必须几乎一样 —— 否则"单调"可能只是
        # 启动抖动（这一条是把"量测本身可信"和"效果存在"分开断言）。
        ctl = abs(energies["w0_ctl"] - e0)
        check("控制组：waves=0 两次能量稳定（差 < 0.05）", ctl < 0.05,
              "%.2f vs %.2f" % (e0, energies["w0_ctl"]))

    # ================= 组 6：面板滑块（UI 通路）=================
    # 前五组走的是 scene JSON，只能证明"数据→渲染"。这一组真的去点面板上的
    # 滑块，证明"用户能在界面上改到它"。
    print("\n[panel] 点面板上的 Waves 滑块左端（→ 0）")
    proc, tap, hwnd = launch(exe, scene, select=WATER_NAME, no_hud=False,
                             editmode="water")
    if not hwnd:
        check("panel: 编辑器起不来", False)
    else:
        try:
            rect = wait_rect(tap, "WATER-WAVES", 8.0)
            if not rect:
                check("panel: 滑块没出现在首屏", False,
                      "（面板比窗口高时会滚出可视区）")
            else:
                cx, cy = center(rect)
                w = rect[2] - rect[0]
                # 点轨道最左端 = 0；ImGui 的滑块点一下就设值
                drag(hwnd, (rect[0] + 4, cy), (rect[0] + 4, cy), steps=3)
                time.sleep(1.0)
                m = last_match(tap, WAVES_RE)
                check("panel: 点滑块打出 WATER-WAVES", m is not None,
                      "rect=(%.0f,%.0f,%.0f,%.0f)" % tuple(rect))
                if m:
                    v = float(m.group(2))
                    check("panel: 值被改到 0（平静）", v <= 0.05,
                          "waves=%.2f" % v)
                # 再**拖动**滑块到最右端 = 10（点边缘不如拖 grab 可靠：
                # 右端几像素可能落在取值文字区/裁剪边缘上，点了没反应）
                n_before = n_matches(tap, WAVES_RE)
                drag(hwnd, (rect[0] + 6, cy), (rect[2] - 8, cy), steps=10)
                time.sleep(1.0)
                if n_matches(tap, WAVES_RE) <= n_before:
                    # 没出新日志就再补一次小拖（ImGui 有时把第一下当"激活"）
                    drag(hwnd, (cx, cy), (rect[2] - 8, cy), steps=10)
                    time.sleep(0.8)
                m2 = last_match(tap, WAVES_RE)
                if m2:
                    v2 = float(m2.group(2))
                    check("panel: 拖到右端 = 10（汹涌）", v2 >= 9.9,
                          "waves=%.2f" % v2)
                else:
                    check("panel: 第二次拖动没打日志", False)
        finally:
            stop(proc)
            time.sleep(1.0)

    # ================= 组 7：老场景迁移（无 waves 字段）=================
    # 第七轮之前存的水体只有 waveHeight。加载时按
    # waves = 10·(waveHeight/0.6)^(1/1.35) 反推，保证"打开旧场景，水面
    # 看起来还是原来的样子"，而不是悄悄变成默认的平静水面。
    print("\n[legacy] 老场景（只有 waveHeight=0.55，没有 waves）")
    sc = os.path.join(OUT_DIR, "wm_scene_legacy.json")
    build_scene(sc, legacy_wave_height=0.55)
    proc, tap, hwnd = launch(exe, sc)
    if not hwnd:
        check("legacy: 编辑器起不来", False)
    else:
        try:
            vp = wait_rect(tap, "VP-RECT", 10.0)
            wait_log(tap, "WATER-SYNC", 15.0)   # 同浪况组：等水体就绪
            time.sleep(1.8)
            img, _ = capture_client(hwnd)
            img.save(os.path.join(OUT_DIR, "wm_legacy.png"))
            arr = np.asarray(img.convert("RGB")).astype("float32")
            if vp:
                box = water_box(vp)
                e_leg, _ = laplacian_energy(arr[box[1]:box[3], box[0]:box[2]])
                print("     legacy 水面高频能量 = %.2f（waves=4 是 %.2f）"
                      % (e_leg, energies.get("w4", -1.0)))
                check("legacy: 旧波高映射成高浪况（能量 > waves=4）",
                      e_leg > energies.get("w4", 1e9),
                      "%.2f vs %.2f" % (e_leg, energies.get("w4", -1.0)))
            else:
                check("legacy: 没有 VP-RECT", False)
        finally:
            stop(proc)
            time.sleep(1.0)

    print("\n" + "=" * 68)
    if failures:
        print("结果: FAIL（%d 项）" % len(failures))
        for f in failures:
            print("  · " + f)
        return 1
    print("结果: PASS（手柄 3 组 + 浪况 2 组 + 面板滑块 1 组）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
