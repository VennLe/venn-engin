"""验收：角色面板预览视口（P6，2026-10-22）。

用户原话：

  "角色面板中预览角色动画时，要可以预览动画播放的效果的，预览视口中要带
   栅格地面和 gizmo 标记。在这个预览视口中，可以清晰的看到角色的胶囊体，
   并且也可以通过鼠标去选中，和控制角色，胶囊体各自的位置，旋转和大小。"

判据（每条都能单独变红）：

  P0 角色面板打开且认领了模型（`PLAYER-DEFAULT bound`）
  P1 预览视口有矩形（`CHAR-PREVIEW`，非退化）
  P2 叠加层默认开（`preview overlay grid=1 capsule=1 gizmo=1`）
  P3 预览动画真的在动 —— 点 Walk 后隔 0.45 s 截两张，
     预览矩形内的像素差异占比 ≥ 0.4 %（**这条是"能不能预览动画"的唯一
     硬证据**：时间轴在走 ≠ 模型在动，后者只有像素能证明）
  P4 胶囊线框真的画出来了 —— 截图里"绿色占优"的像素 ≥ 120 个
  P5 鼠标点**线框**→ `CHAR-PREVIEW-PICK target=capsule`
  P6 鼠标点**身体**→ `CHAR-PREVIEW-PICK target=model`
     （P5/P6 的点击位置来自 `CHAR-PREVIEW-CAPSULE-SCREEN` 这个投影锚点，
      不猜坐标：胶囊中轴两端 + 半径像素都是引擎自己算出来的）
  P7 编辑器 Play 里空格能跳 → `EDIT-JUMP v0=3.70 expectH=0.70m`
  P8 无崩溃

用法：
    python tools/verify_char_preview.py
    python tools/verify_char_preview.py --keep
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32          # noqa: E402
from safe_fs import safe_remove                                        # noqa: E402
from verify_add_menu import (LogTap as _BaseLogTap, click,
                             kill_stale_editors, open_menu_and_click,
                             wait_rect)
from verify_asset_drag import client_to_screen, force_foreground       # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
ASSETS = os.path.join(BIN, "assets")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "char_preview")

SCENE_NAME = "_p6_preview.json"
SCENE_PATH = os.path.join(ASSETS, "scenes", SCENE_NAME)
SIZE = "1600x1000"

VK_SPACE, VK_ESCAPE = 0x20, 0x1B

RE_DEFAULT = re.compile(r"PLAYER-DEFAULT bound path=(\S+)")
RE_OVERLAY = re.compile(r"preview overlay grid=(\d) capsule=(\d) gizmo=(\d) "
                        r"capsuleR=([\d.]+) capsuleHH=([\d.]+)")
RE_PICK = re.compile(r"CHAR-PREVIEW-PICK target=(\w+)")
RE_CAP_SCR = re.compile(r"CHAR-PREVIEW-CAPSULE-SCREEN a=\((-?[\d.]+),(-?[\d.]+)\) "
                        r"b=\((-?[\d.]+),(-?[\d.]+)\) r=([\d.]+) "
                        r"off=\((-?[\d.]+),(-?[\d.]+)\)")
RE_JUMP = re.compile(r"EDIT-JUMP v0=([\d.]+) expectH=([\d.]+)m")
# Play 态鼠标接管（P8）：EditorApp::updatePlayMouse 在状态变化时打一条
RE_EDIT_MOUSE = re.compile(r"EDIT-MOUSE captured=1")


def make_scene():
    scene = {
        "version": 1,
        "generator": "verify_char_preview.py",
        "camera": {"target": [0.0, 0.0, 1.2], "yaw": 3.141592653589793,
                   "pitch": 0.30, "distance": 6.0, "fov": 55.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True,
                  "ambientScale": 1.0},
        "physics": {"enabled": True, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            {"name": "GameStart",
             "transform": {"position": [0.0, 0.0, 0.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "parent": -1, "visible": True, "castShadow": False,
             "gameStart": {"yaw": 0.0, "enabled": True}},
        ],
    }
    os.makedirs(os.path.dirname(SCENE_PATH), exist_ok=True)
    with open(SCENE_PATH, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)


class LogTap(_BaseLogTap):
    def snapshot(self):
        with self.lock:
            return list(self.lines)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = SCENE_PATH
    env.update({k: str(v) for k, v in extra.items()})
    return env


def launch():
    proc = subprocess.Popen([EXE], cwd=BIN, env=clean_env(),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    for _ in range(60):
        hwnd = find_window("Venn Editor")
        if hwnd:
            break
        if proc.poll() is not None:
            return proc, None, tap
        time.sleep(0.25)
    if not hwnd:
        return proc, None, tap
    user32.ShowWindow(hwnd, 9)
    time.sleep(1.2)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)
    return proc, hwnd, tap


def close(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(0.8)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


def center(r):
    return (int((r[0] + r[2]) * 0.5), int((r[1] + r[3]) * 0.5))


def _sc(vk):
    return user32.MapVirtualKeyW(vk, 0) & 0xFF


def tap_key(hwnd, vk, hold=0.06):
    user32.PostMessageW(hwnd, 0x0100, vk, (_sc(vk) << 16) | 1)
    time.sleep(hold)
    user32.PostMessageW(hwnd, 0x0101, vk, (_sc(vk) << 16) | 0xC0000001)
    time.sleep(0.15)


def wait_regex_after(tap, rx, offset, timeout=10.0):
    end = time.time() + timeout
    n = offset
    while time.time() < end:
        lines = tap.snapshot()
        for i in range(n, len(lines)):
            m = rx.search(lines[i])
            if m:
                return m, i + 1
        n = max(n, len(lines))
        time.sleep(0.1)
    return None, n


def last_regex(tap, rx):
    """日志里**最后一条**匹配（锚点可能被推拉视角刷了好几版）。"""
    hit = None
    for ln in tap.snapshot():
        m = rx.search(ln)
        if m:
            hit = m
    return hit


def wait_rect_visible(tap, tag, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r and (r[2] - r[0]) > 8.0 and (r[3] - r[1]) > 8.0:
            return r
        time.sleep(0.12)
    return None


def grab(hwnd):
    try:
        return capture_client(hwnd)[0].convert("RGB")
    except Exception as exc:                                # noqa: BLE001
        print("  截图失败:", exc)
        return None


def diff_ratio(a, b, rect, step=2):
    """两张图在 rect 内的"明显不同"像素占比。"""
    if a is None or b is None:
        return 0.0
    x0, y0, x1, y1 = (int(v) for v in rect)
    x0 = max(x0, 0)
    y0 = max(y0, 0)
    x1 = min(x1, a.width, b.width)
    y1 = min(y1, a.height, b.height)
    pa, pb = a.load(), b.load()
    total = 0
    diff = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            total += 1
            ca, cb = pa[x, y], pb[x, y]
            if (abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) +
                    abs(ca[2] - cb[2])) > 24:
                diff += 1
    return (diff / total) if total else 0.0


def green_pixels(img, rect, step=1):
    """rect 内"绿色占优"的像素数（胶囊线框 120,235,150）。"""
    if img is None:
        return 0
    x0, y0, x1, y1 = (int(v) for v in rect)
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, img.width), min(y1, img.height)
    px = img.load()
    n = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            r, g, b = px[x, y]
            if g > 150 and g > r + 45 and g > b + 25:
                n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留截图与日志")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print("[ %s ] %s  %s" % (" OK " if ok else "FAIL", name, detail))
        return ok

    kill_stale_editors()
    make_scene()

    proc, hwnd, tap = launch()
    if not hwnd:
        check("编辑器起得来", False, "exit=%s" % proc.poll())
        close(proc, None)
        return 1

    try:
        time.sleep(2.0)

        # ---- P0 建角色 ----
        vp = wait_rect(tap, "VP-RECT", 12.0)
        if not vp:
            check("拿到视口矩形", False)
            return 3
        mx, my = center(vp)
        user32.SetCursorPos(*client_to_screen(hwnd, mx, my))
        time.sleep(0.4)
        ok = open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                                 "VP-ADD-ThirdPerson", "Third-Person Player")
        if not check("P0a 菜单创建第三人称角色", ok):
            return 4
        m_def, off = wait_regex_after(tap, RE_DEFAULT, 0, 14.0)
        check("P0b 创建即装配默认角色", m_def is not None,
              m_def.group(0) if m_def else "没有 PLAYER-DEFAULT bound")

        # ---- P1 预览视口矩形 ----
        preview = wait_rect_visible(tap, "CHAR-PREVIEW", 14.0)
        if not check("P1 角色面板预览视口存在（CHAR-PREVIEW）",
                     preview is not None,
                     "rect=(%d,%d)-(%d,%d)" % tuple(int(v) for v in preview)
                     if preview else "没有矩形"):
            return 5

        # ---- P2 叠加层默认开 ----
        m_ov, off = wait_regex_after(tap, RE_OVERLAY, 0, 12.0)
        check("P2 预览默认带栅格地面 + 胶囊 + gizmo",
              m_ov is not None and m_ov.group(1) == "1"
              and m_ov.group(2) == "1" and m_ov.group(3) == "1",
              m_ov.group(0) if m_ov else "没有 preview overlay 行")
        if m_ov:
            check("P2b 胶囊尺寸由模型身高推出（r≈0.29 / hh≈0.61）",
                  0.20 <= float(m_ov.group(4)) <= 0.40
                  and 0.40 <= float(m_ov.group(5)) <= 0.80,
                  "r=%s hh=%s" % (m_ov.group(4), m_ov.group(5)))

        # ---- P3 预览动画真的在动 ----
        tab_anim = wait_rect_visible(tap, "CHAR-TAB-Animation", 8.0)
        if tab_anim:
            ax, ay = center(tab_anim)
            click(hwnd, ax, ay)
            time.sleep(0.3)
            click(hwnd, ax, ay)
            time.sleep(0.5)
        row = wait_rect_visible(tap, "CHAR-CLIP-Walk", 8.0)
        if row:
            rx, ry = center(row)
            click(hwnd, rx, ry)
            time.sleep(0.5)

        a = grab(hwnd)
        time.sleep(0.45)
        b = grab(hwnd)
        ratio = diff_ratio(a, b, preview)
        check("P3 预览里的模型真的在动（不是定格姿势）", ratio >= 0.004,
              "像素差异 %.3f%%（阈值 0.4%%）" % (ratio * 100.0))
        if a is not None:
            a.save(os.path.join(OUT_DIR, "p3_anim_a.png"))
        if b is not None:
            b.save(os.path.join(OUT_DIR, "p3_anim_b.png"))

        # ---- P4 胶囊线框可见 ----
        gcount = green_pixels(b, preview)
        check("P4 胶囊线框画在预览里（绿色像素）", gcount >= 120,
              "%d 个绿色像素（阈值 120）" % gcount)
        if b is not None:
            b.save(os.path.join(OUT_DIR, "p4_capsule.png"))

        # ---- P5 点胶囊**线框** → 选中胶囊 ----
        px0, py0, px1, py1 = (int(v) for v in preview)
        m_cap = last_regex(tap, RE_CAP_SCR)
        if not check("P5a 拿到胶囊的屏幕投影（锚点，不猜坐标）",
                     m_cap is not None,
                     m_cap.group(0) if m_cap else "没有 "
                     "CHAR-PREVIEW-CAPSULE-SCREEN"):
            return 6
        ax, ay = float(m_cap.group(1)), float(m_cap.group(2))
        bx, by = float(m_cap.group(3)), float(m_cap.group(4))
        rpx = float(m_cap.group(5))
        ox, oy = float(m_cap.group(6)), float(m_cap.group(7))
        # 左母线 = 中轴中点 − off；往内收 3 px 保证落在 9 px 判定带里
        cx_line = (ax + bx) * 0.5 - ox
        cy_line = (ay + by) * 0.5 - oy
        shrink = 3.0 / max(rpx, 1.0)
        cx_line += ox * shrink
        cy_line += oy * shrink
        off = len(tap.snapshot())
        click(hwnd, int(round(cx_line)), int(round(cy_line)))
        m_pick, _ = wait_regex_after(tap, RE_PICK, off, 4.0)
        check("P5b 鼠标点胶囊线框 → 选中胶囊",
              m_pick is not None and m_pick.group(1) == "capsule",
              (m_pick.group(0) if m_pick else "没有 CHAR-PREVIEW-PICK") +
              "  @(%d,%d) r=%.1f" % (int(cx_line), int(cy_line), rpx))
        g1 = grab(hwnd)
        if g1 is not None:
            g1.save(os.path.join(OUT_DIR, "p5_pick_capsule.png"))

        # ---- P6 点**身体**内部 → 选中模型 ----
        cx_body, cy_body = (ax + bx) * 0.5, (ay + by) * 0.5
        off = len(tap.snapshot())
        click(hwnd, int(round(cx_body)), int(round(cy_body)))
        m_pick2, _ = wait_regex_after(tap, RE_PICK, off, 4.0)
        check("P6 鼠标点身体 → 选中模型",
              m_pick2 is not None and m_pick2.group(1) == "model",
              (m_pick2.group(0) if m_pick2 else "没有 CHAR-PREVIEW-PICK") +
              "  @(%d,%d)" % (int(cx_body), int(cy_body)))
        g2 = grab(hwnd)
        if g2 is not None:
            g2.save(os.path.join(OUT_DIR, "p6_pick_model.png"))

        # ---- P7 编辑器 Play 空格跳 ----
        # 先把焦点还给视口（点 3D 视口标签），否则键盘事件可能落在面板里
        vtab = wait_rect_visible(tap, "VTAB-viewport", 6.0)
        if vtab:
            vx, vy = center(vtab)
            click(hwnd, vx, vy)
            time.sleep(0.6)
        off = len(tap.snapshot())
        tap_key(hwnd, VK_SPACE)          # 编辑态 Space = 进 Play
        time.sleep(2.0)

        # ---- P7a（P8，2026-10-23）：Play 态键鼠是两态的 ----
        # 进了 Play 不等于能操作 —— 必须先用左键在 3D 视口里点一下接管，
        # 否则下面的 Space 不归玩家（用户要求"点了才能交互"）。
        # 视口矩形沿用编辑态拿到的那份：Play 态只是少了工具栏，中心点必在
        # 视口内。
        ocx, ocy = center(vp)
        off = len(tap.snapshot())
        click(hwnd, ocx, ocy)
        m_cap, _ = wait_regex_after(tap, RE_EDIT_MOUSE, off, 6.0)
        check("P7a Play 态点视口接管鼠标（探针 captured=1）",
              m_cap is not None, m_cap.group(0) if m_cap else "没有 EDIT-MOUSE")

        tap_key(hwnd, VK_SPACE)          # Play 态 Space = 跳跃
        m_jump, _ = wait_regex_after(tap, RE_JUMP, off, 6.0)
        check("P7 编辑器 Play 里敲空格 → 角色起跳（v0=3.70 / 0.70 m）",
              m_jump is not None and abs(float(m_jump.group(1)) - 3.70) < 0.2,
              m_jump.group(0) if m_jump else "没有 EDIT-JUMP")
        shot = grab(hwnd)
        if shot is not None:
            shot.save(os.path.join(OUT_DIR, "p7_play_jump.png"))

        # 退出 Play（Esc 在 Play 里是退出全屏；用 Space 切暂停再切回编辑）
        tap_key(hwnd, VK_ESCAPE)
        time.sleep(0.3)

        # ---- P8 无崩溃 ----
        lines = tap.snapshot()
        bad = [ln for ln in lines
               if ("VK_CHECK" in ln) or ("invalid state" in ln)]
        check("P8 全程无 Vulkan 错误、进程活着",
              len(bad) == 0 and proc.poll() is None,
              "%d 条异常 exit=%s" % (len(bad), proc.poll()))
    finally:
        close(proc, hwnd)
        if not args.keep:
            for f in (SCENE_PATH,):
                safe_remove(f)

    print("-" * 56)
    ok = sum(1 for r in results if r)
    print("结果: %d 通过 / %d 失败" % (ok, len(results) - ok))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
