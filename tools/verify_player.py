"""端到端验证：Shift+A ▸ **Player** 菜单（第一人称 / 第三人称）。

用户要的形态（原文要点）：
    "在add的菜单中，新加一个Player选项 …… 第一人称Player,第三人称Player。
     按wsad可以控制前后左右移动，通过鼠标可以控制视角的移动。"

断言（两种视角各起一个进程）：
  1. 菜单创建成功（`added 'First-Person Player'` / `'Third-Person Player'`）；
  2. Space 开始 Run → `PLAYER-STATE mode=...` 每 30 帧报状态；
  3. **W** 按住 → 玩家沿视线/相机朝向移动（第一人称 yaw=0 → x 增大；
     第三人称相机在角色 +X 侧 → W 远离相机 → x 减小）；
  4. **D** 按住 → 向右横移（yaw=0 → y 减小）；
  5. **鼠标**移动 → yaw/pitch 变化（两种视角都驱动 yaw；先等 miv=1，
     即视口悬停门控打开，再连打一组位移看累积量）；
  6. 第三人称：编辑态截图小人 + 运行态截图 walk 姿势。

用法：
    python tools/verify_player.py
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import (LogTap, kill_stale_editors, open_menu_and_click,
                             post_key, wait_rect)                     # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_MOUSEMOVE = 0x0200

VK_W, VK_A, VK_S, VK_D = 0x57, 0x41, 0x53, 0x44
VK_SPACE = 0x20
SC = {VK_W: 0x11, VK_A: 0x1E, VK_S: 0x1F, VK_D: 0x20, VK_SPACE: 0x39}

STATE_RE = re.compile(
    r"PLAYER-STATE '([^']*)' mode=(\S+) "
    r"pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"yaw=(-?[\d.]+) pitch=(-?[\d.]+)"
    r"(?: miv=(\d))?")


def hold_key(hwnd, vk, seconds):
    """按下不放（GLFW 收到 down → IsKeyDown 持续 true），之后松开。"""
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (SC[vk] << 16) | 1)
    time.sleep(seconds)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (SC[vk] << 16) | 0xC0000001)
    time.sleep(0.2)


def tap_key(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (SC[vk] << 16) | 1)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (SC[vk] << 16) | 0xC0000001)
    time.sleep(0.4)


def states(tap):
    with tap.lock:
        return [STATE_RE.search(l) for l in tap.lines]
    # unreachable


def all_states(tap):
    with tap.lock:
        out = []
        for line in tap.lines:
            m = STATE_RE.search(line)
            if m:
                out.append(m)
        return out


def wait_new_state(tap, count, timeout=6.0):
    """等到日志里出现第 count+1 条 PLAYER-STATE，返回最新一条。"""
    end = time.time() + timeout
    while time.time() < end:
        ss = all_states(tap)
        if len(ss) > count:
            return ss[-1], len(ss)
        time.sleep(0.1)
    ss = all_states(tap)
    return (ss[-1] if ss else None), len(ss)


def center(rect):
    return (int((rect[0] + rect[2]) * 0.5), int((rect[1] + rect[3]) * 0.5))


def _mouse_lparam(x, y):
    return ((int(y) & 0xFFFF) << 16) | (int(x) & 0xFFFF)


# 我们"逻辑上"维持的光标位置（客户端坐标）。ImGui 看到的位置由它决定，
# 与真实光标无关。
_mouse_pos = [None]


def mouse_set_client(hwnd, x, y):
    """把鼠标放到窗口客户端坐标 (x, y)。

    两件事一起做，缺一不可：

    1) **PostMessage(WM_MOUSEMOVE)** —— 这是让 ImGui 真正收到位移的手段。
       ImGui 的 GLFW 后端在 `bd->MouseWindow != nullptr`（光标已进入窗口）时
       **完全依赖光标位置回调**，不再每帧轮询 glfwGetCursorPos；而
       glfwGetCursorPos 返回的其实也是同一条消息更新出来的
       virtualCursorPos。走 GLFW 自己的消息循环（win32_window.c 的
       WM_MOUSEMOVE → _glfwInputCursorPos）与窗口焦点无关，也绕开了
       SetCursorPos / mouse_event 在这台机器上的节流与丢帧。

    2) **SetCursorPos** —— 把**真实**光标也挪到同一个地方。这一步不是为了
       位移，而是为了让真实光标留在窗口内：一旦真实光标在窗口外，Windows
       会不停地给窗口补发 WM_MOUSELEAVE（GLFW 的 TrackMouseEvent 一装上就
       立刻触发），ImGui 于是每隔一帧收到一个 MousePos = -FLT_MAX
       —— 表现就是 hover 时真时假（等于永远为假，鼠标视角直接不生效），
       而且 MouseDelta 在"位置无效"的那些帧会被清零。
    """
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, _mouse_lparam(x, y))
    user32.SetCursorPos(*client_to_screen(hwnd, int(x), int(y)))
    _mouse_pos[0] = (int(x), int(y))


def mouse_move_rel(hwnd, dx, dy):
    """在"逻辑光标"上相对移动，并把新位置投递给窗口。"""
    if _mouse_pos[0] is None:
        _mouse_pos[0] = (0, 0)
    x, y = _mouse_pos[0]
    mouse_set_client(hwnd, x + int(dx), y + int(dy))


def mouse_look_delta(tap, hwnd, count, dx, dy, field, timeout=3.0, tries=4):
    """注入一次相对鼠标位移，返回 PLAYER-STATE 第 field 列的变化量。

    返回 (delta, count)；delta 为 0.0 表示重试完都没看到变化。

    重试的原因：PLAYER-STATE 每 30 帧才一条，而一次位移只影响**一帧**的
    io.MouseDelta —— 探针采样点大概率错过那一帧。好在效果是**累积的**
    （yaw += delta*lookSpeed），所以多注几次总有一次能被采样到；每次都
    重新取基准，只关心"相对基准有没有变"。
    """
    for _ in range(tries):
        st_a, na = wait_new_state(tap, count, timeout)
        if st_a is not None:
            count = na
        mouse_move_rel(hwnd, dx, dy)
        time.sleep(0.5)
        st_b, nb = wait_new_state(tap, count, timeout)
        if st_b is None:
            continue
        count = nb
        if st_a is None:
            continue
        d = float(st_b.group(field)) - float(st_a.group(field))
        if abs(d) > 0.02:
            return d, count
    return 0.0, count


def latest_state(tap):
    """当前最后一条 PLAYER-STATE（没有就返回 (None, 0)）。"""
    ss = all_states(tap)
    return (ss[-1] if ss else None), len(ss)


def hover_ready(tap, hwnd, timeout=12.0):
    """等到探针报 miv=1 —— 即视口被悬停。

    鼠标视角是 viewportHovered 硬门控的（EditorApp::runPlayers 的
    `if (mouseInViewport)`）。而 ImGui 的 hover 要求 g.HoveredWindow 就是
    视口所在窗口：Shift+A 菜单刚关掉时鼠标还压在右侧面板上，或者还有
    残留的 popup / tooltip 挡着，此时 hover 是 false —— **注入多少位移
    都不会转视角**。这里一边轻轻抖动（给 ImGui 新的 MousePos 事件）一边
    轮询门控状态。
    """
    end = time.time() + timeout
    while time.time() < end:
        ss = all_states(tap)
        if ss and ss[-1].group(8) == "1":
            return True
        mouse_move_rel(hwnd, 3, 0)
        mouse_move_rel(hwnd, -3, 0)
        time.sleep(0.2)
    return False


def mouse_look_burst(tap, hwnd, dx, dy, field, n, settle=0.25):
    """连续注入 n 次同向位移，返回 PLAYER-STATE 第 field 列的累积变化量。

    为什么不是"注入一次断言一次"：见 mouse_look_delta 的说明 —— 单次注入
    要被 30 帧一次的探针采样到只有约 1/6 的命中率。视角旋转本身是累积量，
    连打 n 次之后比较"基准 → 现在"就稳得多。
    """
    st_a, _ = latest_state(tap)
    if st_a is None:
        return 0.0
    base = float(st_a.group(field))
    for _ in range(n):
        mouse_move_rel(hwnd, dx, dy)
        time.sleep(settle)
    time.sleep(1.0)  # 等探针打出一条落在注入之后的状态
    st_b, _ = latest_state(tap)
    if st_b is None:
        return 0.0
    return float(st_b.group(field)) - base


def launch(exe, extra_env, size):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update(extra_env)
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    for _ in range(40):
        hwnd = find_window("Venn Editor")
        if hwnd:
            break
        time.sleep(0.25)
    if not hwnd:
        proc.kill()
        return None, None, tap
    user32.ShowWindow(hwnd, 9)
    time.sleep(1.5)
    tw, th = (int(v) for v in size.lower().split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)
    return proc, hwnd, tap


def close(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_case(exe, size, mode, fails, check):
    """mode = 'first' | 'third'。返回 0 成功。"""
    label = ("First-Person Player", "first", "player_first") if mode == "first" \
        else ("Third-Person Player", "third", "player_third")
    item_name, mode_word, png_base = label

    kill_stale_editors()
    tmpdir = tempfile.mkdtemp(prefix="player_%s_" % mode)

    proc, hwnd, tap = launch(exe, {}, size)
    if not hwnd:
        check("[%s] 编辑器启动" % mode, False)
        return 2
    try:
        # ---- 1) 菜单创建 ----
        vp = wait_rect(tap, "VP-RECT", 8.0)
        if not vp:
            check("[%s] 拿到 VP-RECT" % mode, False)
            return 3
        mx, my = center(vp)
        # Shift+A 要求视口 hovered —— 先把鼠标放进视口
        sx, sy = client_to_screen(hwnd, mx, my)
        user32.SetCursorPos(sx, sy)
        time.sleep(0.5)
        ok = open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                                 "VP-ADD-%s" % ("FirstPerson" if mode == "first"
                                                else "ThirdPerson"),
                                 item_name)
        check("[%s] Shift+A ▸ Player ▸ %s 创建成功" % (mode, item_name), ok)
        if not ok:
            return 4

        if mode == "third":
            time.sleep(0.6)
            capture_client(hwnd)[0].save(
                os.path.join(OUT_DIR, png_base + "_edit.png"))

        # ---- 2) Space 开始 Run ----
        tap_key(hwnd, VK_SPACE)
        st0, n0 = wait_new_state(tap, 0, 8.0)
        check("[%s] Run 后报 PLAYER-STATE" % mode,
              st0 is not None and st0.group(2) == mode_word,
              st0.group(0) if st0 else "没等到")
        if not st0:
            return 5
        n0 = n0 - 1  # wait_new_state 返回的是"已有条数"，用条数做游标

        # ---- 3) W 前进 ----
        hold_key(hwnd, VK_W, 1.0)
        st1, n1 = wait_new_state(tap, n0, 6.0)
        if st1:
            dx = float(st1.group(3)) - float(st0.group(3))
            dy = float(st1.group(4)) - float(st0.group(4))
            if mode == "first":
                check("[%s] W 沿视线前进（x 增大 %.2f m）" % (mode, dx),
                      dx > 0.8, "dx=%.3f dy=%.3f" % (dx, dy))
            else:
                # 相机在角色 +X 侧 → W = 远离相机 = -X
                check("[%s] W 远离相机（x 减小 %.2f m）" % (mode, dx),
                      dx < -0.8, "dx=%.3f dy=%.3f" % (dx, dy))
            n0 = n1
        else:
            check("[%s] W 之后有新的状态日志" % mode, False)

        # ---- 4) D 横移（第一人称 yaw=0 → right=(0,-1) → y 减小）----
        st2 = None
        if mode == "first":
            hold_key(hwnd, VK_D, 0.8)
            st2, n2 = wait_new_state(tap, n0, 6.0)
            if st2:
                dy = float(st2.group(4)) - float(st1.group(4))
                check("[first] D 向右横移（y 减小 %.2f m）" % dy, dy < -0.4,
                      "dy=%.3f" % dy)
                n0 = n2

        # ---- 5) 鼠标转视角（yaw / pitch 方向）----
        # 前置条件：鼠标视角被 viewportHovered 硬门控，所以先把光标放回视口
        # 中心并**等门控真的打开**（探针里的 miv=1）；门控没开就直接判失败，
        # 免得后面三条断言一起报"0.000"、看不出真正原因。
        # 注入策略见 mouse_look_burst：单次注入很难被 30 帧一次的探针采样到，
        # 所以每个方向连打一组、断言累积变化。
        cx, cy = center(vp)
        mouse_set_client(hwnd, cx, cy)
        hoverOk = hover_ready(tap, hwnd)
        check("[%s] 视口悬停就绪（鼠标视角门控 miv=1）" % mode, hoverOk)
        if not hoverOk:
            return 6

        # 预热：冲掉 SetCursorPos / 首次进入留下的那一个大 delta（结果丢弃）
        mouse_look_burst(tap, hwnd, 20, 0, 6, 4)

        # 屏幕坐标 y 向下，dx=+40 = 鼠标右移；
        # 引擎里 yaw += MouseDelta.x * lookSpeed → 右移时 yaw 增大。
        dyaw = mouse_look_burst(tap, hwnd, 40, 0, 6, 6)
        check("[%s] 鼠标右移 → yaw 增大 %.3f rad" % (mode, dyaw),
              dyaw > 0.05, "dyaw=%.3f" % dyaw)

        # 语义约定：pc.pitch = 视线仰角，正 = 抬头。
        # dy=-40 = 鼠标上移；引擎里 pitch -= MouseDelta.y → 上移时 pitch 增大。
        dpitch = mouse_look_burst(tap, hwnd, 0, -40, 7, 6)
        check("[%s] 鼠标上移 → pitch 增大 %.3f（抬头）" % (mode, dpitch),
              dpitch > 0.05, "dpitch=%.3f" % dpitch)

        dpitch2 = mouse_look_burst(tap, hwnd, 0, 40, 7, 6)
        check("[%s] 鼠标下移 → pitch 减小 %.3f（低头）" % (mode, dpitch2),
              dpitch2 < -0.05, "dpitch=%.3f" % dpitch2)

        if mode == "third":
            # 边走边截一张 walk 姿势
            user32.PostMessageW(hwnd, WM_KEYDOWN, VK_W,
                                (SC[VK_W] << 16) | 1)
            time.sleep(0.7)
            capture_client(hwnd)[0].save(
                os.path.join(OUT_DIR, png_base + "_walk.png"))
            user32.PostMessageW(hwnd, WM_KEYUP, VK_W,
                                (SC[VK_W] << 16) | 0xC0000001)
            time.sleep(0.3)

        return 0
    finally:
        close(proc, hwnd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    ap.add_argument("--mode", default="both", choices=["first", "third",
                                                       "both"])
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    fails = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if detail else ""))
        if not ok:
            fails.append(name)

    modes = ["first", "third"] if args.mode == "both" else [args.mode]
    rc = 0
    for m in modes:
        print("########## case: %s" % m)
        rc = run_case(args.exe, args.size, m, fails, check)
        if rc != 0:
            fails.append("case %s 提前结束 (rc=%d)" % (m, rc))

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
