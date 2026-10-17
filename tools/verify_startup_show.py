"""端到端验证：启动时**不闪白** + 缓冲两秒再露面。

用户原文（2026-10-17）：
    "编辑器窗口启动打开的时候，为什么会闪白一下，能否优化一下，可以让它
     这个启动过程缓冲两秒再直接打开完整的窗口。"

闪白的根因（不是"渲染慢"）：窗口一创建就 `glfwShowWindow`，而 Vulkan 第一帧
还没呈现 → 系统拿窗口类的 `COLOR_WINDOW`（纯白）刷客户区 → 白闪。
两条改动配合：
  ① `Window` 构造里不再立刻 show；首帧**呈现之后**才 `show()`，并且等满两秒
     （见 Application.cpp 里 kStartupShowDelay 那段）。
  ② 兜底：窗口类背景刷换成深灰（`SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND,
     …)`），万一系统还是要先刷一次客户区，刷的也是深灰而不是白。

断言（都能机器判，不靠"看着好像没闪"）：
  A1 起跑 0.8s 时窗口**还没露面**（IsWindowVisible = 0）—— "缓冲"的硬证据
  A2 露面时刻落在 1.6s ~ 4.5s（用户要的是"约两秒"，不是 0 也不是 8）
  A3 露面后连拍 8 张（约 0.9s），**每一张**都不是白的（平均亮度 < 180）
     —— 有白闪的话头几张必然是白的
  A4 日志里有 "Window shown (first frame is on screen)"

⚠ 必须设 MYVK_STARTUP_DELAY=1 才跑得出缓冲：脚本要设 MYVK_NO_WINDOW_SAVE
  （否则退出时会写 imgui.ini，把用户的布局存档覆盖掉），而那个变量会把
  缓冲跳过。所以加了 MYVK_STARTUP_DELAY 这个显式口子。

用法：
    python tools/verify_startup_show.py
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors                # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

WHITE_MIN = 180.0     # 平均亮度超过它 = 判"白"
EARLY_PROBE = 0.8     # A1 的检查时刻（秒）
MIN_SHOW = 1.6        # A2 下限
MAX_SHOW = 4.5        # A2 上限


def mean_luma(img):
    px = img.convert("RGB").resize((120, 80))
    w, h = px.size
    data = px.load()
    tot = 0.0
    for y in range(h):
        for x in range(w):
            r, g, b = data[x, y]
            tot += 0.299 * r + 0.587 * g + 0.114 * b
    return tot / float(w * h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件:", exe)
        return 2
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()

    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"   # 不写 imgui.ini（别覆盖用户布局）
    env["MYVK_STARTUP_DELAY"] = "1"    # 但**强制**保留那两秒缓冲 ← 本用例的靶子

    t0 = time.time()
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    print("launched pid", proc.pid)

    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails.append(name)

    try:
        # ⚠ 别"先 find_window 再 sleep 0.8s 看可见性"：find_window 自己会花掉
        #   一两秒（枚举顶层窗口 + 进程刚起时它可能还没建好窗口），于是"0.8s
        #   那一刻"的采样实际发生在 3s —— 第一版就是这么误报的（探针打印的
        #   时间戳反而是最好的线索）。改成**从一开始就连续轮询**，把"首次可见
        #   的时刻"当成观测量，A1/A2 都从它推。
        hwnd = None
        first_vis_t = None
        deadline = t0 + 12.0
        while time.time() < deadline:
            if hwnd is None:
                hwnd = find_window("Venn Editor")
            now = time.time() - t0
            if hwnd and user32.IsWindowVisible(hwnd):
                first_vis_t = now
                break
            time.sleep(0.02)
        if not hwnd:
            print("没找到窗口")
            return 2

        check("A1 起跑 %.1fs 时窗口还没露面（缓冲生效）" % EARLY_PROBE,
              first_vis_t is not None and first_vis_t > EARLY_PROBE,
              "首次可见 @%.2fs（要求 > %.1fs）" % (first_vis_t or -1.0,
                                                  EARLY_PROBE))

        check("A2 露面时刻 ≈ 缓冲两秒", first_vis_t is not None and
              MIN_SHOW <= first_vis_t <= MAX_SHOW,
              "%.2fs（要求 %.1f~%.1f；含 Vulkan 初始化，故 >2.0 属正常）"
              % (first_vis_t or -1.0, MIN_SHOW, MAX_SHOW))

        # ---- A3：露面后连拍，不能有白帧 ----
        user32.SetWindowPos(hwnd, 0, 0, 0, 1100, 800, 0x0002 | 0x0004)
        worst = -1.0
        n = 8
        for k in range(n):
            time.sleep(0.11)
            img, _ = capture_client(hwnd)
            img.save(os.path.join(OUT_DIR, "startup_show_%02d.png" % k))
            lu = mean_luma(img)
            if lu > worst:
                worst = lu
        check("A3 露面后 %d 张连拍都没有白帧（不闪白）" % n,
              worst < WHITE_MIN, "最高平均亮度 %.1f（白帧阈值 %.0f）"
              % (worst, WHITE_MIN))

        # ---- A4：日志自述 ----
        time.sleep(0.3)
        with tap.lock:
            logged = any("Window shown" in l for l in tap.lines)
        check("A4 日志有 'Window shown (first frame is on screen)'", logged)

        print("截图 →", OUT_DIR)
    finally:
        if args.keep:
            print("--keep：留窗口 pid", proc.pid)
        else:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    if fails:
        print("结果: FAIL: " + ", ".join(fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
