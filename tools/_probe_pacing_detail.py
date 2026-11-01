# -*- coding: utf-8 -*-
"""一次性探针：`verify_frame_pacing` 那 4 条红到底红在哪几帧。

要回答三件事（只看 sd/mean 答不出来）：
  ① 那个 100 ms 的尖刺落在**第几帧** —— 首帧预热，还是运动中随机掉帧？
  ② 除了它还有几帧异常，分布是"开头一簇"还是"散在中段"？
  ③ C 组（MYVK_VSYNC=0）拿到的 present mode 到底是 MAILBOX 还是回退 FIFO？

用法：python tools/_probe_pacing_detail.py
"""
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_frame_pacing as vfp                                   # noqa: E402


def dump_mode(tap, tag):
    with tap.lock:
        lines = [l for l in tap.lines if "present mode" in l]
    print("  [%s] present mode 日志：%s" % (tag, lines[-1].strip() if lines else "(无)"))


def report(seg, tag):
    dts = [r["dt"] for r in seg]
    if not dts:
        print("  [%s] 没有样本" % tag)
        return
    m, sd, mx = vfp.mean_sd(dts)
    srt = sorted(dts)
    med = srt[len(srt) // 2]
    p95 = srt[min(len(srt) - 1, int(0.95 * len(srt)))]
    spikes = [(i, d) for i, d in enumerate(dts) if d > 2.0 * med]
    print("  [%s] n=%d 均值=%.1fms(%.0f fps) 中位=%.1fms p95=%.1fms max=%.1fms "
          "p95/中位=%.2f  cv=%.0f%%"
          % (tag, len(dts), m * 1000, 1.0 / max(m, 1e-9), med * 1000, p95 * 1000,
             mx * 1000, p95 / max(med, 1e-9), 100 * sd / max(m, 1e-9)))
    print("      尖刺(>2×中位) %d 个：%s"
          % (len(spikes),
             ", ".join("idx%d=%.0fms" % (i, d * 1000) for i, d in spikes[:8]) or "（无）"))


def run(exe, vsync, tag, frames_head=14):
    print("\n===== %s（vsync=%s）=====" % (tag, vsync))
    proc, hwnd, tap, vp, base = vfp.enter_play(exe, vsync, True)
    if not vp:
        print("  ✗ enter_play 失败（vp=None）")
        return
    dump_mode(tap, tag)

    # ---- ① 静止 2 s（不按键、不动鼠标）：分离"环境/合成器"与"玩法逻辑" ----
    # 如果静止时也有每 0.5 s 一次的 70 ms 停顿，那它就不是"移动时卡顿"，
    # 而是这台机器的 DWM/驱动在偷时间 —— 拿它去要求引擎是没有意义的。
    idle_base = len(vfp.rows(tap))
    time.sleep(2.0)
    report(vfp.rows(tap)[idle_base:], tag + " 静止")

    # ---- ② 边走边转视角 2.6 s（用户描述的场景）----
    moved_base = len(vfp.rows(tap))
    vfp.hold_down(hwnd, vfp.VK_W)
    sgn = 1
    end = time.time() + 2.6
    while time.time() < end:
        vfp.mouse_move_rel(hwnd, 26 * sgn, 0)
        sgn = -sgn
        time.sleep(0.045)
    seg = vfp.rows(tap)[moved_base:]
    vfp.hold_up(hwnd, vfp.VK_W)
    time.sleep(0.6)
    # 与用例同一套"掐头"逻辑
    head = 0
    for i in range(1, len(seg)):
        if math.dist(seg[i - 1]["pos"], seg[i]["pos"]) / max(seg[i]["dt"], 1e-9) > 0.1:
            head = max(0, i - 1)
            break
    seg = seg[head:]
    report(seg, tag + " 移动")
    dts = [r["dt"] for r in seg]
    print("      前 %d 帧 dt(ms)：%s" % (frames_head,
          " ".join("%.1f" % (d * 1000) for d in dts[:frames_head])))
    # B2：逐帧 ΔanimT/dt 的分布
    ratios = []
    for i in range(1, len(seg)):
        d = seg[i]["anim"] - seg[i - 1]["anim"]
        ratios.append(d / max(seg[i]["dt"], 1e-9))
    rs = sorted(ratios)
    if rs:
        back = sum(1 for x in ratios if x < 0.0)
        print("      ΔanimT/dt：中位=%.3f  p10=%.3f  p90=%.3f  min=%.3f  max=%.3f  倒退帧=%d"
              % (rs[len(rs) // 2], rs[int(0.1 * len(rs))],
                 rs[int(0.9 * len(rs))], rs[0], rs[-1], back))
    try:
        vfp.close(proc, hwnd)
    except Exception as e:                                          # noqa: BLE001
        print("  close 异常：%r" % (e,))


def main():
    exe = vfp.DEFAULT_EXE
    # 顺序可对调：`--order ca` 先跑不锁帧再跑锁帧，用来排除"先跑的那组
    # 更轻"这种顺序假象（A 组先跑 → 温度/占用还没上来，看起来更快）。
    order = sys.argv[1] if len(sys.argv) > 1 else "--order=ac"
    if "ca" in order:
        run(exe, False, "C 不锁帧")
        run(exe, True, "A 锁帧")
    else:
        run(exe, True, "A 锁帧")
        run(exe, False, "C 不锁帧")
    return 0


if __name__ == "__main__":
    sys.exit(main())
