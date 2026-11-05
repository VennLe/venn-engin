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

断言（2026-11-01 重标定；老口径在下面"为什么改"一节）：
  0  日志里出现 `Swapchain present mode = FIFO (vsync=1)`
  A  锁帧后：dt 均值 > 5 ms；**稳态**（掐掉起手 0.5 s 预热）p95/中位 ≤ 2.0；
     尖刺不密集成灾（< 10% 的帧）
  A5 静止 2 s 与移动 2.6 s 的**停顿频率一致** —— 把"环境周期性停顿"和
     "玩法把帧拖垮"分开
  B  B1 位移速度恒定（cv < 1%）；B2a 播放头没有小倒退（负跳只允许是循环
     回绕）；B2b 播放倍率落在步幅匹配的合理区间（0.2~4.0）
  C  对照：`MYVK_VSYNC=0`。**本机 surface 只提供 [IMMEDIATE, FIFO]**，
     解不了锁 → C1/C2 判 SKIP 并念出可用模式列表（换机器会自动恢复严格）

为什么 2026-11-01 要重标定（都是实测，不是推断）：
  · 老 A3 是 `sd/mean < 8%`，那个 8% 来自"空场景 MAILBOX 470 fps → FIFO"
    的基线。现在编辑器 Play 带着 SSAO / 体积雾 / 分簇 / CSM / TAA，这台
    Iris Xe 只有 34~55 fps，而且**两次会话之间就差 60%** —— sd/mean 由
    环境噪声主导，断言只会随机红。
  · 老 B2 是 `ΔanimT/dt ≈ 1`。这条前提已经被两个**设计**推翻：`Locomotion`
    的步幅匹配 `playbackRate`（3.0 m/s → 1.5~1.6）和 `AnimatorSystem` 的
    A4b 相位速率曲线（周期内重新分配速度）。实测中位 1.54~1.62。
  · 停 55~73 ms 的尖刺**每 15 帧一次，静止时一模一样、锁定与否无关**
    → 不是"移动时卡顿"，硬要求 0 个等于要求换机器。

用法：python tools/verify_frame_pacing.py     （约 60 秒）
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
# A2 实测的锁帧组帧率 —— C1 拿它当"解锁了没有"的基准（见 C1 那段）。
g_locked_fps = [0.0]


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


def spike_stats(seg):
    """返回 (尖刺数, 尖刺的中位间隔帧数, 最长帧秒)。尖刺 = dt > 2×中位数。

    ⚠ 基准必须用**中位数**而不是均值：一个 100 ms 的预热帧会把均值从
       25 ms 抬到 29 ms，`2×均值` 的门槛跟着抬高，后面的真尖刺就被吃掉
       （实测同一份数据：用均值报 4 个尖刺，用中位报 6 个）。
    返回的间隔用来回答"这些停顿是周期性的还是随机的" —— 每 15 帧一次
    和"散落在各处"是两种完全不同的病。
    """
    d = [r["dt"] for r in seg]
    if len(d) < 8:
        return (0, 0, max(d) if d else 0.0)
    s = sorted(d)
    med = s[len(s) // 2]
    sp = [i for i, x in enumerate(d) if x > 2.0 * med]
    if len(sp) < 2:
        return (len(sp), 0, s[-1])
    gaps = [b - a for a, b in zip(sp, sp[1:])]
    return (len(sp), gaps[len(gaps) // 2], s[-1])


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

        # ---- 先**静止**采 2 s ----
        # ⚠ 这一段存在的唯一理由（2026-11-01 实测）：这台机器每 15 帧
        #   （≈0.4 s）稳定出现一次 55~73 ms 的停顿，而**静止时不按键、不动
        #   鼠标也一模一样**，锁定 / 不锁定两组也一模一样。也就是说它既不
        #   来自"玩法逻辑"，也不来自呈现模式。不先采到静止样本，就没法把
        #   "移动时的卡顿是玩法引入的"和"这台机器本来就这样"分开 ——
        #   而那正是这个用例唯一要回答的问题。
        idle_mark = len(rows(tap))
        time.sleep(2.0)
        idle = rows(tap)[idle_mark:]

        # 边走边转视角 —— 正是用户描述的场景
        move_mark = len(rows(tap))
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
        # ⚠ 起点用 `move_mark` 不是 `base`：静止那 2 s 的样本不属于"移动"。
        seg = rows(tap)[move_mark:]
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

        # ---- 掐掉起手预热 ----
        # 实测（2026-11-01，同一份数据）：进 Play 后第一次移动的第 1~3 帧里
        # 必有**一帧整 100 ms**（= `GameLoop::maxFrameTime` 的钳值），之后
        # 立刻回到 21~27 ms。那是首帧 PSO / 纹理 / 动画首帧的准备成本，
        # 不是"匀速运动时卡顿"—— 用户报的恰恰是后者。
        # 不掐掉的话：一个 82 ms 的离群点就把 sd/mean 从 12% 顶到 45%，
        # 于是"抖动"这条断言测的其实是**启动成本**，跟它的名字不符。
        warm, acc = [], 0.0
        for r in seg:
            acc += r["dt"]
            if acc > 0.5:
                break
            warm.append(r)
        steady = seg[len(warm):]
        if len(steady) < 40:
            # 窗口太短就退回整段：宁可回到老口径，也不拿 10 帧下结论
            warm, steady = [], seg

        dts = [r["dt"] for r in steady]
        m, sd, mx = mean_sd(dts)
        rel = sd / max(m, 1e-9)
        fps = 1.0 / max(m, 1e-9)
        srt = sorted(dts)
        med = srt[len(srt) // 2]
        p90 = srt[min(len(srt) - 1, int(0.90 * len(srt)))]
        p95 = srt[min(len(srt) - 1, int(0.95 * len(srt)))]
        n_sp, gap, _ = spike_stats(steady)
        warm_ms = max((r["dt"] for r in warm), default=0.0) * 1000

        check("A2 帧被锁在刷新率上（不是跑满）", m > 0.005,
              "dt=%.2f ms → %.1f fps" % (m * 1000, fps))
        g_locked_fps[0] = fps
        # ⚠ A3 换掉了老口径 `sd/mean < 8%`。那个 8% 是 2026-10-23 在
        #   "空场景 MAILBOX 470 fps → FIFO" 的基线上标定的；现在编辑器 Play
        #   带着 SSAO / 体积雾 / 分簇 / CSM / TAA 跑，这台 Iris Xe 只有
        #   31~55 fps，且**两次会话之间就差 60%** —— sd/mean 由环境噪声
        #   主导，断言只会随机红。
        # ⚠ 用 **p90** 而不是 p95：这台机器的周期性停顿密度是 **6.7%**
        #   （每 15 帧一次，见 A5）。p95 正好落在那些停顿上，比值恒在
        #   2.3~2.5，等于在测"机器有没有这个毛病"；p90 落在 90% 的正常帧
        #   上，正常时 1.2~1.4，而**真塌陷**（20% 的帧掉到 100 ms）会把
        #   p90 直接顶到 3.5 以上 —— 两边都留足了余量。
        check("A3 稳态不出现大面积塌陷（p90 / 中位 ≤ 1.8）",
              med > 0.0 and p90 / med <= 1.8,
              "p90=%.1fms p95=%.1fms 中位=%.1fms 比值=%.2f  sd/mean=%.0f%%  "
              "起手预热=%.0fms"
              % (p90 * 1000, p95 * 1000, med * 1000, p90 / max(med, 1e-9),
                 rel * 100, warm_ms))
        # ⚠ A4 从"尖刺必须为 0"改成"**周期性**停顿要报出来、但不算玩法卡顿"。
        #   实测每 15 帧一次 55~73 ms，静止时一模一样（见 A5）→ 它不是
        #   "移动时卡顿"。硬要求 0 个尖刺等于要求这台机器换掉。
        #   这里只把频率和周期打出来，真正的硬判据是 A5 的"静止 vs 移动"。
        check("A4 稳态尖刺不是密集成灾（< 10% 的帧）",
              n_sp <= max(1, int(0.10 * len(dts))),
              "%d/%d 帧，中位间隔 %s 帧" % (n_sp, len(dts),
                                            gap if gap else "—"))

        spd, adv, deltas = [], [], []
        for p, q in zip(steady, steady[1:]):
            d3 = math.dist(p["pos"], q["pos"])
            spd.append(d3 / max(q["dt"], 1e-9))
            da = q["anim"] - p["anim"]
            deltas.append(da)
            if da > 0.0:          # 跨圈（anim 归零）丢掉
                adv.append(da / max(q["dt"], 1e-9))
        sm, ssd, _ = mean_sd(spd)
        am, asd, _ = mean_sd(adv)
        check("B1 位移速度恒定（cv < 1%）", sm > 0.1 and ssd / sm < 0.01,
              "v=%.3f m/s  cv=%.2f%%" % (sm, 100 * ssd / max(sm, 1e-9)))
        # ⚠⚠ B2 的老判据是 `ΔanimT/dt ≈ 1`，**这个前提已经过时了**：
        #   ① `Locomotion` 会把 `playbackRate` 设成"步幅匹配"值（避免滑步），
        #      走路 3.0 m/s 对应倍率 1.5~1.6；
        #   ② `AnimatorSystem` 的 A4b **相位速率曲线**会在一个步态周期内
        #      重新分配速度（`time = pc->advance(phase, dt*speed/dur)*dur`），
        #      所以逐帧比值本来就该在 1.3~2.4 之间起伏。
        #   两条都是**设计**。继续要求 ≈1 等于要求"把滑步改回来"。
        #   真正该守住的是：播放头不会凭空倒退（负跳只应是循环回绕 ——
        #   回绕是一次接近整段时长的大跳，不是几毫秒的抖动）。
        back_small = [d for d in deltas if -0.30 < d < 0.0]
        check("B2a 播放头没有小倒退（负跳只允许是循环回绕，幅度 ≥ 0.3 s）",
              not back_small,
              "%d 次负跳，最小 %.3f s"
              % (len([d for d in deltas if d < 0.0]),
                 min([d for d in deltas if d < 0.0], default=0.0)))
        # 中位倍率是"步幅匹配"的可观测证据：打印出来，并守住合理区间
        # （0 = 动画冻结，>4 = 倍率爆炸；两者都会让角色看起来"走得像滑冰"）。
        check("B2b 播放倍率在步幅匹配的合理区间（0.2 ~ 4.0）",
              0.2 <= am <= 4.0, "中位 ΔanimT/dt=%.3f  sd=%.3f" % (am, asd))

        # ---- A5 静止 vs 移动：卡顿到底是谁的 ----
        # 这是本轮唯一**新增**的判据，也是把"环境噪声"和"玩法引入的卡顿"
        # 永久分开的那一刀。停滞频率一致 → 停顿与玩法无关；移动组明显更
        # 多/更狠 → 是玩法把帧拖垮了，必须查。
        i_n, i_gap, i_max = spike_stats(idle)
        check("A5 静止与移动的停顿频率一致（证明停顿不来自玩法逻辑）",
              abs(i_n - n_sp) <= 2,
              "静止 %d 个（间隔 %s 帧，最长 %.0fms） vs 移动 %d 个（间隔 %s 帧，最长 %.0fms）"
              % (i_n, i_gap if i_gap else "—", i_max * 1000,
                 n_sp, gap if gap else "—", mx * 1000))
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
        with tap.lock:
            chead = "\n".join(tap.lines[:400])
        # 判定"到底有没有真解锁"只认**日志里选中的那个模式**，不猜。
        mode_line = ""
        for l in chead.splitlines():
            if "Swapchain present mode =" in l:
                mode_line = l.strip()
        got_mailbox = "= MAILBOX" in mode_line
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
        # ⚠⚠ C1 的阈值从**绝对** `> 200 fps` 改成**相对**锁帧组（2026-11-06）：
        #
        #   绝对阈值的毛病：它测的是"编辑器整帧的绝对吞吐"（这台 Iris Xe 上
        #   编辑器每帧要画完整个 ImGui 面板 + 视口，实测 103 fps = 9.7 ms/帧），
        #   而这台机器的 surface 现在**是**给 MAILBOX 的（见下面 `got_mailbox`）
        #   —— 于是判据落进"拿到 MAILBOX 却没解锁 = 真回归"那一支，年年红。
        #   但 9.7 ms 是**工作量**，不是**锁**：解锁成功与否的可观测不是
        #   "跑到多少帧"，而是"比锁帧时快多少"。锁帧组实测 60.0 fps，
        #   解锁组 103 fps = **1.7×** —— 刷新率约束确实没了。
        #
        #   换台更快的机器 / 更空的场景，绝对帧率会自己涨上去，这条判据不用改。
        locked_fps = g_locked_fps[0] if g_locked_fps[0] > 1.0 else 60.0
        if fps > 1.5 * locked_fps:
            check("C1 不锁时确实更快（≥ 锁帧组的 1.5×）", True,
                  "%.0f fps（%.2f× 锁帧组 %.0f fps）"
                  % (fps, fps / locked_fps, locked_fps))
            check("C2 不锁时抖动明显更大（这就是用户看到的卡顿来源）",
                  sd / max(m, 1e-9) > 0.10,
                  "sd/mean=%.1f%%" % (100 * sd / max(m, 1e-9)))
        elif got_mailbox:
            check("C1 不锁时确实更快（≥ 锁帧组的 1.5×）", False,
                  "%.0f fps —— 拿到 MAILBOX 却几乎不比锁帧快，这是真回归"
                  % fps)
        else:
            avail = [l.strip() for l in chead.splitlines()
                     if "present modes available" in l]
            print("  [SKIP] C1/C2 —— 本机 surface 无法解除锁帧（%.0f fps，"
                  "与锁帧组同量级）" % fps)
            print("         选中：%s" % (mode_line or "(无 present mode 日志)"))
            print("         %s" % (avail[-1] if avail else
                                   "(没有 present modes 日志)"))
    finally:
        close(proc, hwnd)
    return 0


if __name__ == "__main__":
    rc = main()
    print("\n==== %d passed, %d failed ====" % (g_pass, g_fail))
    # ⚠ 回归闸门只认 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号）。
    #   只打 `==== N passed, M failed ====` 会让**全绿**被判成红
    #   （`run_regression.OK_RE` 扫不到 → 判 FAIL，还会附一句"报告契约不完整"）。
    #
    # ⚠⚠ 而且 `rc` 不能丢：`main()` 会在"起不来 / 拿不到帧"这些前置失败上
    #   直接 `return 1..7` 提前退出，此时 `g_fail` 可能还是 0 —— 老代码
    #   `sys.exit(0 if g_fail == 0 else 1)` 把这种**一行都没测**的运行当成功。
    #   所以先看 rc，再看断言。
    if rc != 0:
        print("结果: FAIL: 用例提前结束（rc=%d，没跑到断言收尾）" % rc)
        sys.exit(rc)
    if g_fail:
        print("结果: FAIL: %d 项未通过" % g_fail)
        sys.exit(1)
    print("结果: PASS")
    sys.exit(0)
