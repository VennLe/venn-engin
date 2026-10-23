"""一次性探针：Play 态"边走边转视角"的逐帧时基。

用户报告：
    "游戏运行后，角色在移动时尤其是鼠标移动转换视角时，明显感觉角色动作
     在卡顿，步伐也变得僵硬了起来。"

要分辨的三种可能（只看帧率数字只能看到第一种）：
  ① 帧率抖（dt 有尖刺）      → FRAME-DT 的 dt 列
  ② 位移抖（速度不连续）      → FRAME-DT 的 pos 列一阶差分
  ③ 动画相位抖（播放头乱跳）  → FRAME-DT 的 animT 列一阶差分

用法：
    python tools/_probe_frame_dt.py [--mode third|first] [--move-burst 1]
"""

import argparse
import math
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import user32                                    # noqa: E402
from verify_add_menu import kill_stale_editors, wait_rect, open_menu_and_click  # noqa: E402
from verify_asset_drag import client_to_screen                        # noqa: E402
from verify_player import (LogTap, VK_SPACE, VK_W, SC, WM_KEYDOWN,    # noqa: E402
                           WM_KEYUP, _captured, _mouse_pos, capture_ready,
                           click_client, close, center, launch,
                           mouse_move_rel, mouse_set_client, tap_key)

DT_RE = re.compile(
    r"FRAME-DT f=(\d+) dt=([\d.]+) play=(\d) cap=(\d) "
    r"pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) animT=(-?[\d.]+)")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")


def parse(tap):
    with tap.lock:
        lines = list(tap.lines)
    out = []
    for line in lines:
        m = DT_RE.search(line)
        if m:
            out.append((int(m.group(1)), float(m.group(2)),
                        int(m.group(3)), int(m.group(4)),
                        float(m.group(5)), float(m.group(6)), float(m.group(7)),
                        float(m.group(8))))
    return out


def stats(xs):
    if not xs:
        return (0.0, 0.0, 0.0)
    n = len(xs)
    mean = sum(xs) / n
    var = sum((x - mean) ** 2 for x in xs) / n
    return (mean, math.sqrt(var), max(xs))


def hold_down(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (SC[vk] << 16) | 1)


def hold_up(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (SC[vk] << 16) | 0xC0000001)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--mode", default="third", choices=["third", "first"])
    ap.add_argument("--size", default="1600x900")
    # 呈现模式：1 = FIFO（锁垂直同步，现在的默认），0 = MAILBOX（跑满）
    ap.add_argument("--vsync", default="1")
    args = ap.parse_args()

    kill_stale_editors()
    _captured[0] = False
    _mouse_pos[0] = None

    env = {"MYVK_LOG_DT": "1", "MYVK_VSYNC": args.vsync}
    proc, hwnd, tap = launch(args.exe, env, args.size)
    if not hwnd:
        print("启动失败")
        return 2
    try:
        vp = wait_rect(tap, "VP-RECT", 8.0)
        if not vp:
            print("拿不到 VP-RECT")
            return 3
        mx, my = center(vp)
        user32.SetCursorPos(*client_to_screen(hwnd, mx, my))
        time.sleep(0.5)
        item = "VP-ADD-%s" % ("ThirdPerson" if args.mode == "third"
                              else "FirstPerson")
        ok = open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player", item,
                                 "Third-Person Player" if args.mode == "third"
                                 else "First-Person Player")
        print("菜单创建:", ok)
        time.sleep(0.6)
        tap_key(hwnd, VK_SPACE)
        time.sleep(2.0)
        cx, cy = center(vp)
        mouse_set_client(hwnd, cx, cy)
        cap = capture_ready(tap, hwnd, vp)
        print("接管 cap=", cap)
        if not cap:
            return 4
        time.sleep(0.8)

        base = len(parse(tap))
        print("基准帧号:", base)

        # ---- 阶段 A：只按 W，不动鼠标 ----
        hold_down(hwnd, VK_W)
        time.sleep(1.6)
        # ---- 阶段 B：W + 持续鼠标横向移动（左→右来回扫）----
        mark = len(parse(tap))
        t_end = time.time() + 2.4
        sgn = 1
        while time.time() < t_end:
            mouse_move_rel(hwnd, 26 * sgn, 0)
            sgn = -sgn
            time.sleep(0.045)
        time.sleep(0.4)
        mark2 = len(parse(tap))
        hold_up(hwnd, VK_W)
        time.sleep(1.0)

        rows = parse(tap)
        print("采到帧数:", len(rows))

        a = rows[base:mark]        # 纯移动
        b = rows[mark:mark2]       # 移动 + 转视角

        for name, seg in (("A 只按 W", a), ("B W+转视角", b)):
            if len(seg) < 20:
                print("%s: 样本不足 (%d)" % (name, len(seg)))
                continue
            dts = [r[1] for r in seg[1:]]
            m, sd, mx_ = stats(dts)
            # 位移一阶差分（速度）
            spd = []
            for p, q in zip(seg, seg[1:]):
                d = math.dist(p[4:7], q[4:7])
                spd.append(d / max(q[1], 1e-6))
            sm, ssd, smx = stats(spd)
            # 动画相位一阶差分
            adv = []
            for p, q in zip(seg, seg[1:]):
                da = q[7] - p[7]
                if da < -0.5:      # 跨圈
                    continue
                adv.append(da)
            am, asd, amx = stats(adv)
            print("")
            print("%s  n=%d" % (name, len(seg)))
            print("  dt(ms)   mean=%.2f sd=%.2f max=%.2f  (%.1f fps)"
                  % (m * 1000, sd * 1000, mx_ * 1000, 1.0 / max(m, 1e-6)))
            print("  速度(m/s) mean=%.3f sd=%.3f max=%.3f"
                  % (sm, ssd, smx))
            print("  animT/d  mean=%.5f sd=%.5f  (应≈1.0)"
                  % (am if am else 0.0, asd))
            # 抖动指标：速度的相对离散度（%）
            if sm > 0.01:
                print("  速度抖动 cv=%.1f%%" % (100.0 * ssd / sm))
            # dt 与 animT 步长的相关性（1.0 = 完全按 dt 推进）
            if len(dts) == len(adv) and len(dts) > 5:
                ma = sum(dts) / len(dts)
                madv = sum(adv) / len(adv)
                cov = sum((x - ma) * (y - madv) for x, y in zip(dts, adv))
                va = sum((x - ma) ** 2 for x in dts) ** 0.5
                vb = sum((y - madv) ** 2 for y in adv) ** 0.5
                if va > 0 and vb > 0:
                    print("  corr(dt, ΔanimT)=%.3f" % (cov / (va * vb)))
            # 尖刺：dt > 2×mean
            spikes = [d for d in dts if d > 2 * m]
            print("  尖刺 dt>2×mean: %d/%d" % (len(spikes), len(dts)))
            # 最差的几帧（速度偏离均值最大）
            worst = sorted(range(len(spd)), key=lambda i: -abs(spd[i] - sm))[:6]
            print("  速度最差帧: %s" % ", ".join(
                "#%d v=%.2f" % (i, spd[i]) for i in worst))
            print("  前 8 帧 v: %s" % " ".join("%.2f" % v for v in spd[:8]))
        # 动画状态切换（真正的"卡顿"来源之一）
        with tap.lock:
            lines = list(tap.lines)
        sw = [l for l in lines if "EDIT-ANIM state=" in l or "-ANIM-FADE" in l]
        print("")
        print("动画状态切换 %d 条:" % len(sw))
        for l in sw[:12]:
            print("   ", l.strip()[:130])
        return 0
    finally:
        close(proc, hwnd)


if __name__ == "__main__":
    sys.exit(main())
