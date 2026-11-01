# -*- coding: utf-8 -*-
"""否定测试：`verify_frame_pacing` 新判据的**算式**能不能真的红。

不启动编辑器，只喂合成帧序列，回答三件事：
  ① A3（p90 / 中位 ≤ 1.8）在"这台机器现在的噪声"下是绿的，
     在"20% 的帧掉到 100 ms"（真塌陷）下必须红；
  ② A4（尖刺 < 10% 的帧）在 6.7% 周期性尖刺下是绿的，
     在 20% 下必须红；
  ③ A5（静止 vs 移动的尖刺数差 ≤ 2）在两边一致时绿，
     在"移动多出 6 个"（玩法把帧拖垮）时必须红。

⚠ 这是**判据形式**的自检，不是产品自检 —— 产品的部分由用例本身跑。
写它的理由：这三条阈值都是从实测数据上定的，如果它们其实"怎么都能过"，
那和没写一样。
"""
import random
import sys

sys.path.insert(0, "tools")

from verify_frame_pacing import spike_stats                        # noqa: E402


def frames(dts):
    return [{"dt": d, "pos": (0.0, 0.0, 0.0), "anim": 0.0, "f": i}
            for i, d in enumerate(dts)]


def healthy(n=64, base=0.0285, spike_every=15, spike=0.065, seed=1):
    """本机现状：中位 28.5 ms + 每 15 帧一次 65 ms 停顿。"""
    rnd = random.Random(seed)
    out = []
    for i in range(n):
        if spike_every and i % spike_every == spike_every - 1:
            out.append(spike)
        else:
            out.append(base * rnd.uniform(0.88, 1.14))
    return out


def p90_over_med(dts):
    s = sorted(dts)
    med = s[len(s) // 2]
    p90 = s[min(len(s) - 1, int(0.90 * len(s)))]
    return p90 / med


def a4_ok(dts):
    n_sp, _, _ = spike_stats(frames(dts))
    return n_sp <= max(1, int(0.10 * len(dts)))


def a5_ok(idle, moved):
    i_n, _, _ = spike_stats(frames(idle))
    n_sp, _, _ = spike_stats(frames(moved))
    return abs(i_n - n_sp) <= 2, i_n, n_sp


def main():
    fails = []

    # ---- ① A3 ----
    ok_now = p90_over_med(healthy()) <= 1.8
    collapse = [0.10 if i % 5 == 0 else 0.0285 for i in range(64)]
    red_collapse = p90_over_med(collapse) > 1.8
    print("A3  本机噪声下比值=%.2f（应绿：%s）"
          % (p90_over_med(healthy()), ok_now))
    print("   20%% 帧塌陷到 100ms 比值=%.2f（应红：%s）"
          % (p90_over_med(collapse), red_collapse))
    if not ok_now:
        fails.append("A3 在本机噪声下就红（阈值不合理）")
    if not red_collapse:
        fails.append("A3 抓不住 20% 塌陷")

    # ---- ② A4 ----
    ok_a4 = a4_ok(healthy())
    red_a4 = not a4_ok(collapse)
    print("A4  本机噪声尖刺=%.1f%%（应绿：%s）"
          % (100.0 * spike_stats(frames(healthy()))[0] / 64.0, ok_a4))
    print("   20%% 塌陷尖刺占比=%.1f%%（应红：%s）"
          % (100.0 * spike_stats(frames(collapse))[0] / 64.0, red_a4))
    if not ok_a4:
        fails.append("A4 在本机噪声下就红")
    if not red_a4:
        fails.append("A4 抓不住 20% 塌陷")

    # ---- ③ A5 ----
    idle = healthy(seed=2)
    same, i_n, m_n = a5_ok(idle, healthy(seed=3))
    # 玩法把帧拖垮：移动组每 5 帧一个 65 ms 停顿
    bad_move = healthy(spike_every=5, seed=4)
    diff, i_n2, m_n2 = a5_ok(idle, bad_move)
    print("A5  两边一致（静止 %d / 移动 %d）应绿：%s" % (i_n, m_n, same))
    print("   移动组每 5 帧卡一次（静止 %d / 移动 %d）应红：%s"
          % (i_n2, m_n2, not diff))
    if not same:
        fails.append("A5 在两边一致时误红")
    if diff:
        fails.append("A5 抓不住'移动侧卡顿变密'")

    if fails:
        print("\n结果: FAIL: " + "; ".join(fails))
        return 1
    print("\n结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
