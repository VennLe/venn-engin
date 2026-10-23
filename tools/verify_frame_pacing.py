"""端到端验证：Play 态的**帧节奏**（2026-10-23）。

用户原话：
    "游戏运行后，角色在移动时尤其是鼠标移动转换视角时，明显感觉角色动作在
     卡顿，步伐也变得僵硬了起来。"

实测到的根因（不是动画播放器的错）：
  逐帧探针（`MYVK_LOG_DT=1`）显示**位置与动画相位都是干净的** ——
  角色每帧位移恒定（速度 cv 0.7%），动画播放头严格按 dt 推进。
  真正脏的是**时基**：主循环在 60 Hz 的屏上跑到 **470~490 fps**
  （dt 均值 2.1 ms、标准差 0.63 ms = **30% 抖动**），原因是呈现模式选了
  MAILBOX（不锁帧）。渲染帧与显示刷新不是整数倍关系 → 合成器每次取的
  "最新一帧"距上次刷新的时间长短不一 → 匀速运动（转视角时整个视野都在
  动）推进得不均匀 = 肉眼看到的"一顿一顿"。

修法：`core::vsyncEnabled()` + `rhi/Swapchain` 默认 FIFO（见
`engine/core/RunMode.h`）。自动化仍可 `MYVK_VSYNC=0` 跑满。

断言（两组对照，同一台机器同一次会话跑完）：
  0  日志里出现 `Swapchain present mode = FIFO (vsync=1)`
  A  锁帧后：dt 均值 > 5 ms、**相对抖动 sd/mean < 5%**、零尖刺（dt>2×均值）
     —— 修复前同一条判据实测 30% 抖动、6% 的帧是尖刺
  B  动画相位与位移仍然严格跟着 dt 走（ΔanimT/dt ≈ 1、速度 cv < 1%）
  C  对照：`MYVK_VSYNC=0` 时确实不锁（帧率 > 200 fps）—— 证明开关是活的，
     不是"碰巧慢了"

用法：python tools/verify_frame_pacing.py     （约 40 秒）
"""

import math
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import user32                                    # noqa: E402
from verify_add_menu import kill_stale_editors, open_menu_and_click, wait_rect  # noqa: E402
from verify_asset_drag import client_to_screen                        # noqa: E402
from verify_player import (LogTap, VK_SPACE, VK_W, SC, WM_KEYDOWN,    # noqa: E402
                           WM_KEYUP, _captured, _mouse_pos, capture_ready,
                           center, close, launch, mouse_move_rel,
                           mouse_set_client, tap_key)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

DT_RE = re.compile(
    r"FRAME-DT f=(\d+) dt=([\d.]+) play=(\d) cap=(\d) "
    r"pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) animT=(-?[\d.]+)")

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


def rows(tap):
    with tap.lock:
        lines = list(tap.lines)
    out = []
    for line in lines:
        m = DT_RE.search(line)
        if m:
            out.append({
                "f": int(m.group(1)), "dt": float(m.group(2)),
                "pos": (float(m.group(5)), float(m.group(6)), float(m.group(7))),
                "anim": float(m.group(8)),
            })
    return out


def mean_sd(xs):
    n = len(xs)
    if n == 0:
        return (0.0, 0.0, 0.0)
    m = sum(xs) / n
    return (m, math.sqrt(sum((x - m) ** 2 for x in xs) / n), max(xs))


def hold_down(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (SC[vk] << 16) | 1)


def hold_up(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (SC[vk] << 16) | 0xC0000001)


def enter_play(exe, vsync, with_mouse):
    """起一个编辑器进程 → 加第三人称角色 → Space 进 Play → 点视口接管。

    返回 (proc, hwnd, tap, vp, base)；失败返回 (None, ...)。
    """
    kill_stale_editors()
    _captured[0] = False
    _mouse_pos[0] = None
    env = {"MYVK_LOG_DT": "1"}
    if vsync is not None:
        env["MYVK_VSYNC"] = "1" if vsync else "0"
    proc, hwnd, tap = launch(exe, env, "1600x900")
    if not hwnd:
        return None, None, tap, None, 0
    vp = wait_rect(tap, "VP-RECT", 8.0)
    if not vp:
        return proc, hwnd, tap, None, 0
    mx, my = center(vp)
    user32.SetCursorPos(*client_to_screen(hwnd, mx, my))
    time.sleep(0.5)
    if not open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                               "VP-ADD-ThirdPerson", "Third-Person Player"):
        return proc, hwnd, tap, None, 0
    time.sleep(0.6)
    tap_key(hwnd, VK_SPACE)
    time.sleep(2.0)
    cx, cy = center(vp)
    mouse_set_client(hwnd, cx, cy)
    if not capture_ready(tap, hwnd, vp):
        return proc, hwnd, tap, None, 0
    time.sleep(0.8)
    return proc, hwnd, tap, vp, len(rows(tap))


def main():
    exe = DEFAULT_EXE
    if len(sys.argv) > 1:
        exe = sys.argv[1]
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    # =============================================== 组 A/B：锁帧（默认）
    print("\n[A] 锁垂直同步（默认行为）")
    proc, hwnd, tap, vp, base = enter_play(exe, True, True)
    if not proc or not hwnd:
        check("编辑器启动", False)
        return 2
    try:
        if vp is None:
            check("进 Play + 点视口接管", False)
            return 3
        with tap.lock:
            head = "\n".join(tap.lines[:400])
        check("A0 呈现模式日志 = FIFO(vsync=1)",
              "Swapchain present mode = FIFO (vsync=1)" in head,
              "（Swapchain.cpp 的策略）")

        # 边走边转视角 —— 正是用户描述的场景
        hold_down(hwnd, VK_W)
        sgn = 1
        end = time.time() + 2.6
        while time.time() < end:
            mouse_move_rel(hwnd, 26 * sgn, 0)
            sgn = -sgn
            time.sleep(0.045)
        # ⚠ 先取样本再松键：松键之后的几帧速度是 0，会把"速度 cv"这条
        #   判据一脚踹翻（实测 47%）。同理开头的几帧（按键还没生效）也要
        #   从"第一帧真的动起来"开始算。
        seg = rows(tap)[base:]
        hold_up(hwnd, VK_W)
        time.sleep(0.6)

        # 掐掉"还没动"的头部：第一条 v>0.1 的帧作为起点
        head = 0
        for i in range(1, len(seg)):
            q, p = seg[i], seg[i - 1]
            if math.dist(p["pos"], q["pos"]) / max(q["dt"], 1e-9) > 0.1:
                head = max(0, i - 1)
                break
        seg = seg[head:]
        if not check("A1 采到逐帧样本", len(seg) > 40, "%d 帧" % len(seg)):
            return 4
        dts = [r["dt"] for r in seg]
        m, sd, mx = mean_sd(dts)
        rel = sd / max(m, 1e-9)
        fps = 1.0 / max(m, 1e-9)
        spikes = sum(1 for d in dts if d > 2.0 * m)
        check("A2 帧被锁在刷新率上（不是跑满）", m > 0.005,
              "dt=%.2f ms → %.1f fps" % (m * 1000, fps))
        check("A3 帧步长抖动 < 8%（修复前实测 31%）", rel < 0.08,
              "sd/mean=%.1f%%  sd=%.2fms  max=%.2fms"
              % (rel * 100, sd * 1000, mx * 1000))
        check("A4 没有尖刺帧（dt > 2×均值）", spikes == 0,
              "%d/%d" % (spikes, len(dts)))

        spd, adv = [], []
        for p, q in zip(seg, seg[1:]):
            d3 = math.dist(p["pos"], q["pos"])
            spd.append(d3 / max(q["dt"], 1e-9))
            da = q["anim"] - p["anim"]
            if da > 0.0:          # 跨圈（anim 归零）丢掉
                adv.append(da / max(q["dt"], 1e-9))
        sm, ssd, _ = mean_sd(spd)
        am, asd, _ = mean_sd(adv)
        check("B1 位移速度恒定（cv < 1%）", sm > 0.1 and ssd / sm < 0.01,
              "v=%.3f m/s  cv=%.2f%%" % (sm, 100 * ssd / max(sm, 1e-9)))
        check("B2 动画相位严格按 dt 推进（比值 ≈ 1）",
              abs(am - 1.0) < 0.02, "ΔanimT/dt=%.4f  sd=%.4f" % (am, asd))
    finally:
        close(proc, hwnd)

    # =============================================== 组 C：对照（不锁）
    print("\n[C] 对照：MYVK_VSYNC=0 应当跑满")
    proc, hwnd, tap, vp, base = enter_play(exe, False, False)
    if not proc or not hwnd:
        check("编辑器启动（对照）", False)
        return 5
    try:
        if vp is None:
            check("进 Play（对照）", False)
            return 6
        hold_down(hwnd, VK_W)
        time.sleep(1.2)
        hold_up(hwnd, VK_W)
        time.sleep(0.4)
        dts = [r["dt"] for r in rows(tap)[base:]]
        if not check("C0 采到逐帧样本（对照）", len(dts) > 40,
                     "%d 帧" % len(dts)):
            return 7
        m, sd, mx = mean_sd(dts)
        fps = 1.0 / max(m, 1e-9)
        check("C1 不锁时确实更快（> 200 fps）", fps > 200.0,
              "%.0f fps（锁帧后应当 ≈ 显示器刷新率）" % fps)
        check("C2 不锁时抖动明显更大（这就是用户看到的卡顿来源）",
              sd / max(m, 1e-9) > 0.10, "sd/mean=%.1f%%"
              % (100 * sd / max(m, 1e-9)))
    finally:
        close(proc, hwnd)
    return 0


if __name__ == "__main__":
    rc = main()
    print("\n==== %d passed, %d failed ====" % (g_pass, g_fail))
    sys.exit(0 if g_fail == 0 else 1)
