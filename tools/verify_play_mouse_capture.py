"""端到端验证：编辑器 Play 的**鼠标两态**（P8，2026-10-23）。

用户原话：
    "游戏启动运行后，必须要鼠标左键在 3d 视口中点击后才能开始正常和游戏
     画面内的角色开始交互操作，并且此时鼠标是隐藏不显示的。按下 Esc 键，
     此时就正常显示鼠标。"

拆成可以断言的四件事：
  ① 进 Play **不自动**接管（光标可见、键鼠不响应）
  ② 左键点视口 → 接管（`EDIT-MOUSE captured=1`，且**系统层面的光标真的隐藏**）
  ③ 接管之后 WASD 能动、视角能转（否则"接管"是个空动作）
  ④ Esc → 交还（`captured=0`，光标重新可见），并且可以再次接管

判据：
  M0 编辑器启动、拿到 VP-RECT
  M1 创建第三人称角色 + Space 进 Play → `EDIT-MOUSE captured=0`
  M2 未接管时按住 W 0.7 s → 角色**不动**（< 0.05 m）
  M3 未接管时系统光标可见（GetCursorInfo 的 CURSOR_SHOWING 位）
  M4 左键点视口 → `EDIT-MOUSE captured=1`
  M5 接管后系统光标**隐藏**（GetCursorInfo 不再报 CURSOR_SHOWING）
  M6 接管后按住 W → 角色真的走（> 0.5 m）
  M7 接管后鼠标位移 → yaw 变化（视角归游戏）
  M8 按 Esc → `EDIT-MOUSE captured=0` 且光标重新可见
  M9 再点一次视口 → 又能接管（两态可反复，不是一次性）
  M10 全程无 Vulkan 错误、进程正常退出

为什么必须查 GetCursorInfo 而不是只看日志：
  "captured=1" 只证明**我们想**隐藏光标；真正把它藏起来的是
  `glfwSetInputMode(GLFW_CURSOR_DISABLED)`。这两步之间完全可以断开
  （例如窗口失去焦点时 GLFW 会自己恢复光标）。系统层的显示位是唯一
  不可辩驳的证据。

用法：
    python tools/verify_play_mouse_capture.py
"""

import argparse
import ctypes
import os
import re
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import click as real_click, wait_rect                       # noqa: E402
from verify_asset_drag import client_to_screen                        # noqa: E402
# 复用玩家验收里的注入工具（同一套 PostMessage 语义，别写第二份）
from verify_player import (STATE_RE, all_states, center, click_client,  # noqa: E402
                           close, hold_key, launch, mouse_move_rel,
                           mouse_set_client, tap_key, VK_ESCAPE, VK_SPACE,
                           VK_W, wait_new_state)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "play_mouse")

# EDIT-MOUSE 锚点（EditorApp::updatePlayMouse）
RE_MOUSE = re.compile(r"EDIT-MOUSE captured=(\d)")

# ---- 系统光标可见性 ----------------------------------------------------
# GetCursorInfo 的 flags 里 CURSOR_SHOWING = 0x0001。GLFW 的 DISABLED 模式
# 走 `ShowCursor(FALSE)` + `SetCursor(NULL)`，所以 SHOWING 位会消失。
CURSOR_SHOWING = 0x0001


class CURSORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD),
                ("flags", wintypes.DWORD),
                ("hCursor", wintypes.HANDLE),
                ("ptScreenPos", wintypes.POINT)]


def cursor_showing():
    """系统层：光标此刻是否真的可见。取不到就返回 None（判据按失败算）。"""
    ci = CURSORINFO()
    ci.cbSize = ctypes.sizeof(CURSORINFO)
    if not user32.GetCursorInfo(ctypes.byref(ci)):
        return None
    return (ci.flags & CURSOR_SHOWING) != 0


def last_mouse_state(tap):
    """最近一条 EDIT-MOUSE 的 captured 值（没有返回 None）。"""
    with tap.lock:
        lines = list(tap.lines)
    for ln in reversed(lines):
        m = RE_MOUSE.search(ln)
        if m:
            return int(m.group(1))
    return None


def wait_mouse_state(tap, want, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        v = last_mouse_state(tap)
        if v == want:
            return True
        time.sleep(0.1)
    return last_mouse_state(tap) == want


def make_check():
    fails = []

    def check(name, ok, detail=""):
        print(("PASS  " if ok else "FAIL  ") + name + ("  " + detail if detail else ""))
        sys.stdout.flush()
        if not ok:
            fails.append(name)
        return ok

    return check, fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    args = ap.parse_args()
    os.makedirs(OUT_DIR, exist_ok=True)

    check, fails = make_check()
    if not os.path.exists(args.exe):
        check("编辑器可执行文件存在", False, args.exe)
        print("RESULT: FAIL")
        return 2

    proc, hwnd, tap = launch(args.exe, {}, args.size)
    try:
        vp = wait_rect(tap, "VP-RECT", 10.0)
        check("M0 编辑器启动并报出视口矩形", bool(vp),
              "VP-RECT=%s" % (vp,))
        if not vp:
            return 3

        # ---- 创建第三人称角色（走 Add 菜单，与用户操作同路径）----
        mx, my = center(vp)
        sx, sy = client_to_screen(hwnd, mx, my)
        user32.SetCursorPos(sx, sy)
        time.sleep(0.4)
        from verify_add_menu import open_menu_and_click
        ok = open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                                 "VP-ADD-ThirdPerson",
                                 "Third-Person Player")
        check("M1a Shift+A ▸ Player ▸ 第三人称创建成功", ok)
        if not ok:
            return 4
        time.sleep(0.8)

        # ---- 进 Play：**不该**自动接管 ----
        tap_key(hwnd, VK_SPACE)       # Space（编辑态 = Run）
        ok0 = wait_mouse_state(tap, 0, 8.0)
        check("M1b 进 Play 时鼠标可见、键鼠未接管（captured=0）", ok0,
              "最近一条=%s" % last_mouse_state(tap))
        show0 = cursor_showing()
        check("M2 未接管时系统光标可见（CURSOR_SHOWING）", show0 is True,
              "GetCursorInfo flags 显示=%s" % show0)

        # ---- 未接管：按 W 角色不动 ----
        st_a, na = wait_new_state(tap, 0, 5.0)
        moved = 0.0
        if st_a:
            hold_key(hwnd, VK_W, 0.8)
            st_b, nb = wait_new_state(tap, na - 1, 5.0)
            if st_b:
                moved = max(abs(float(st_b.group(3)) - float(st_a.group(3))),
                            abs(float(st_b.group(4)) - float(st_a.group(4))))
        check("M3 未接管时按住 W 角色不动（交互被冻结）", moved < 0.05,
              "位移 %.3f m" % moved)

        # ---- 左键点视口 → 接管 ----
        real_click(hwnd, mx, my)
        ok1 = wait_mouse_state(tap, 1, 6.0)
        check("M4 左键点视口 → 接管（EDIT-MOUSE captured=1）", ok1,
              "最近一条=%s" % last_mouse_state(tap))
        # 系统层：光标真的没了
        time.sleep(0.4)
        show1 = cursor_showing()
        check("M5 接管后系统光标隐藏（不再 CURSOR_SHOWING）", show1 is False,
              "GetCursorInfo flags 显示=%s" % show1)
        img = capture_client(hwnd)[0]
        if img is not None:
            img.save(os.path.join(OUT_DIR, "m5_captured.png"))

        # ---- 接管后：W 能走 ----
        st_c = all_states(tap)
        nc = len(st_c) - 1
        st_c0 = st_c[-1] if st_c else None
        hold_key(hwnd, VK_W, 0.9)
        st_d, nd = wait_new_state(tap, max(nc, 0), 5.0)
        walked = 0.0
        if st_c0 and st_d:
            walked = ((float(st_d.group(3)) - float(st_c0.group(3))) ** 2 +
                      (float(st_d.group(4)) - float(st_c0.group(4))) ** 2) ** 0.5
        check("M6 接管后按住 W 角色真的走起来", walked > 0.4,
              "位移 %.3f m" % walked)

        # ---- 接管后：鼠标转视角 ----
        cx, cy = center(vp)
        mouse_set_client(hwnd, cx, cy)
        time.sleep(0.3)
        st_e = all_states(tap)
        ne = len(st_e) - 1
        yaw0 = float(st_e[-1].group(6)) if st_e else 0.0
        turned = 0.0
        for _ in range(6):
            mouse_move_rel(hwnd, 40, 0)
            time.sleep(0.12)
        time.sleep(0.5)
        st_f, nf = wait_new_state(tap, max(ne, 0), 5.0)
        if st_f:
            turned = abs(float(st_f.group(6)) - yaw0)
        check("M7 接管后鼠标位移能转视角（yaw 变化）", turned > 0.05,
              "dyaw=%.3f rad" % turned)

        # ---- Esc → 交还 ----
        tap_key(hwnd, VK_ESCAPE)
        ok2 = wait_mouse_state(tap, 0, 6.0)
        check("M8 按 Esc → 交还鼠标（EDIT-MOUSE captured=0）", ok2,
              "最近一条=%s" % last_mouse_state(tap))
        time.sleep(0.5)
        show2 = cursor_showing()
        check("M9 交还后系统光标重新可见（CURSOR_SHOWING）", show2 is True,
              "GetCursorInfo flags 显示=%s" % show2)
        img2 = capture_client(hwnd)[0]
        if img2 is not None:
            img2.save(os.path.join(OUT_DIR, "m9_released.png"))

        # ---- 再点一次：两态可反复 ----
        sx2, sy2 = client_to_screen(hwnd, mx, my)
        user32.SetCursorPos(sx2, sy2)
        time.sleep(0.3)
        real_click(hwnd, mx, my)
        ok3 = wait_mouse_state(tap, 1, 6.0)
        check("M10 再次点视口又能接管（两态可反复）", ok3,
              "最近一条=%s" % last_mouse_state(tap))
        # 收尾：交还，免得关窗口时光标还是隐藏的
        tap_key(hwnd, VK_ESCAPE)
        wait_mouse_state(tap, 0, 4.0)

        # ---- 崩溃检查 ----
        with tap.lock:
            logs = list(tap.lines)
        bad = [ln for ln in logs
               if ("VK_CHECK" in ln) or ("invalid state" in ln)]
        alive = proc.poll() is None
        check("M11 全程无 Vulkan 错误、编辑器没有崩溃", len(bad) == 0 and alive,
              "%d 条异常 alive=%s" % (len(bad), alive))
    finally:
        # 落盘：失败时要能从日志里看出卡在哪一格（PLAYCLICK / EDIT-MOUSE）
        with tap.lock:
            dump = list(tap.lines)
        try:
            with open(os.path.join(OUT_DIR, "editor.log"), "w",
                      encoding="utf-8", errors="replace") as f:
                f.write("\n".join(dump))
        except OSError:
            pass
        close(proc, hwnd)

    print()
    if fails:
        print("RESULT: FAIL")
        for f in fails:
            print("  -", f)
        return 6
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
