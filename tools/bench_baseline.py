# -*- coding: utf-8 -*-
"""性能基线采集（P1）—— 给 `docs/perf_baseline.md` 那张表提供数据。

为什么要有这个脚本（而不是随手看一眼 HUD）：
  · HUD 的 FPS 是**瞬时值**，同一次运行里 34~55 fps 之间跳；两个会话之间能差
    60%（2026-11-01 实测）。口报的数字没法当基准。
  · 回归用的 `build/bin/Editor.exe` 是 **Debug**：`/RTC1` + 全部断言 + 没有内联
    → 它测出来的帧时间**不能**用来判断"引擎快不快"，只能判断"逻辑对不对"。
    性能基线必须跑 `build-rel/`（`make perf-build`）。
  · 所以这里只做一件事：**同一份配置跑固定时长，取中位数**，并且把"取哪一段、
    怎么去预热"写死在代码里，下次改引擎时两边量法一致才可比。

量法（三条都是踩过的坑，别改）：
  1. **丢掉预热帧**（默认前 14 帧）。程序的起手几帧被 `maxFrameTime` 钳到 100 ms，
     混进来会把中位数往右拖。
  2. **用中位数 + p90，不用平均**。这台机器的 DWM 每 ~15 帧偷一次 55~73 ms，
     平均值被那几下决定。
  3. **GPU 时间取 `GPU-TIMING` 那条日志**（引擎自己打的时戳和），不是
     "1/fps"。1/fps 里含垂直同步与呈现排队。

用法：
    python tools/bench_baseline.py                      # 默认 2 场景 × 3 分辨率
    python tools/bench_baseline.py --sizes 1600x900
    python tools/bench_baseline.py --scenes assets/scenes/anime_demo.json
    python tools/bench_baseline.py --json rel.json --exe build-rel/bin/Editor.exe

⚠ 它是**采集器**不是断言器：帧率随机器状态浮动，把它塞进回归只会随机红。
   （判据是"和 `docs/perf_baseline.md` 里的上一版比，掉了没掉"，那是人看的。）
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from verify_add_menu import kill_stale_editors                      # noqa: E402
from verify_player import LogTap, close, launch                      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build-rel", "bin", "Editor.exe")
DEFAULT_SCENES = ["assets/scenes/anime_demo.json", "assets/scenes/sandbox.json"]
DEFAULT_SIZES = ["1280x720", "1600x900", "1920x1080"]

DT_RE = re.compile(r"FRAME-DT f=(\d+) dt=([\d.]+) ")
GPU_RE = re.compile(r"GPU-TIMING frame=([\d.]+) .*?forward=([\d.]+) .*?post=([\d.]+)")
# 段名=毫秒。引擎那条日志是 "GPU-TIMING frame=.. shadows=.. local_sh=.. ..."，
# 段名表在 `GpuTimer` 里；这里不认名单，来什么收什么（加段不用改脚本）。
SEG_RE = re.compile(r"(\w+)=([\d.]+)")


def pct(xs, p):
    """第 p 百分位（最近秩法，样本少时也不会插值出假精度）。"""
    if not xs:
        return 0.0
    s = sorted(xs)
    i = min(len(s) - 1, max(0, int(round(p * (len(s) - 1)))))
    return s[i]


def collect(tap):
    with tap.lock:
        lines = list(tap.lines)
    dts, gpu, segs = [], [], []
    for line in lines:
        m = DT_RE.search(line)
        if m:
            dts.append(float(m.group(2)))
        if "GPU-TIMING" in line and "frame=" in line:
            kv = dict((k, float(v)) for k, v in SEG_RE.findall(line))
            segs.append(kv)
            gpu.append(kv.get("frame", 0.0))
    return dts, gpu, segs


def bench_one(exe, scene, size, frames, warmup, timeout):
    """跑一份配置，返回指标 dict（拿不到样本时 ok=False）。"""
    kill_stale_editors()
    env = {
        "MYVK_EDITOR_SCENE": scene,
        "MYVK_LOG_DT": "1",          # FRAME-DT ...（每帧一行）
        "MYVK_GPU_TIMING": "1",      # GPU-TIMING ...（每 30 帧一行）
        "MYVK_NO_WINDOW_SAVE": "1",  # 不写 imgui.ini
        "MYVK_VALIDATION": "0",      # 校验层会吃掉 20~30% 时间
    }
    proc, hwnd, tap = launch(exe, env, size)
    rec = {"scene": scene, "size": size, "ok": False}
    if not hwnd:
        rec["error"] = "no window"
        return rec
    try:
        # 预热：等场景加载 + 着色器变体编译 + 首帧分配都过去。
        time.sleep(3.0)
        t0 = time.time()
        while time.time() - t0 < timeout:
            dts, _, _ = collect(tap)
            if len(dts) >= frames:
                break
            time.sleep(0.5)
        dts, gpu, segs = collect(tap)
        used = dts[max(0, warmup):]
        if len(used) < 30:
            rec["error"] = "只有 %d 个帧样本" % len(used)
            return rec
        med = statistics.median(used)
        rec.update({
            "ok": True,
            "n": len(used),
            "cpu_med_ms": med * 1000.0,
            "cpu_p90_ms": pct(used, 0.90) * 1000.0,
            "fps": 1.0 / med if med > 0 else 0.0,
            "gpu_med_ms": statistics.median(gpu) if gpu else -1.0,
            "gpu_max_ms": max(gpu) if gpu else -1.0,
            "gpu_n": len(gpu),
            "segments": {k: statistics.median([d.get(k, 0.0) for d in segs])
                         for k in (segs[0] if segs else {})},
        })
    finally:
        close(proc, hwnd)
    return rec


def bench_game_one(exe_dir, scene, size, frames, warmup, timeout):
    """跑 `Game.exe`（真正的运行时负载），返回指标 dict。

    与编辑器那条路的区别（都是刻意的）：
      · **分辨率走 `--width/--height`**，不靠 SetWindowPos —— 运行时自己建窗口，
        改窗口大小还要等它重建交换链，多一步就多一处漂移；
      · **`MYVK_RUNTIME_MAXFPS=0`** 关掉 60 帧上限。开着上限量出来的全是 16.6 ms，
        什么也测不出来（第一次跑就撞上了：maxFps=60 → 每份配置都 13.0 ms）；
      · **`MYVK_FRAMES` 让它自己退出**，不用找窗口、不用发 WM_CLOSE。
    """
    game = os.path.join(exe_dir, "Game.exe")
    w, h = (int(v) for v in size.lower().split("x"))
    env = dict(os.environ)
    env.update({
        "MYVK_RUNTIME_MAXFPS": "0",
        "MYVK_GPU_TIMING": "1",
        "MYVK_VALIDATION": "0",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_FRAMES": str(frames + warmup),
    })
    rec = {"scene": scene, "size": size, "mode": "game", "ok": False}
    if not os.path.isfile(game):
        rec["error"] = "没有 Game.exe"
        return rec
    t0 = time.time()
    proc = subprocess.Popen([game, "--scene", scene,
                             "--width", str(w), "--height", str(h)],
                            cwd=exe_dir, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        rec["error"] = "超时未退出"
        return rec
    elapsed = time.time() - t0
    dts, gpu, segs = collect(tap)
    if len(gpu) < 3:
        rec["error"] = "只有 %d 条 GPU-TIMING" % len(gpu)
        return rec
    used = gpu[max(0, warmup // 30):]
    med = statistics.median(used)
    rec.update({
        "ok": True,
        "n": len(used),
        "gpu_med_ms": med,
        "gpu_max_ms": max(used),
        "gpu_n": len(used),
        "segments": {k: statistics.median([d.get(k, 0.0) for d in segs[-len(used):]])
                     for k in (segs[0] if segs else {})},
        # 帧率只能估：每行 GPU-TIMING = 30 帧，除以墙钟时间。含加载与退出。
        "fps": (len(gpu) * 30.0) / elapsed if elapsed > 0 else 0.0,
        "cpu_med_ms": -1.0,
        "cpu_p90_ms": -1.0,
        "wall_s": elapsed,
    })
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--mode", default="editor", choices=["editor", "game"],
                    help="editor=视口空闲；game=Game.exe 真负载（不封顶）")
    ap.add_argument("--scenes", default=",".join(DEFAULT_SCENES),
                    help="逗号分隔；相对资产根的路径")
    ap.add_argument("--sizes", default=",".join(DEFAULT_SIZES))
    ap.add_argument("--frames", type=int, default=300,
                    help="每份配置至少采这么多帧（含预热帧）")
    ap.add_argument("--warmup", type=int, default=14,
                    help="丢掉前 N 帧（起手被 maxFrameTime 钳到 100 ms）")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    exe = args.exe if os.path.isabs(args.exe) else os.path.join(ROOT, args.exe)
    if not os.path.isfile(exe):
        print("✗ 找不到 %s" % exe)
        print("  先跑 `make perf-build`（产物在 build-rel/bin/，与回归用的 build/ 分开）")
        print("结果: FAIL: 基线二进制不存在")
        return 1

    scenes = [s.strip() for s in args.scenes.split(",") if s.strip()]
    sizes = [s.strip() for s in args.sizes.split(",") if s.strip()]

    print("exe  = %s" % exe)
    print("mtime= %s" % time.strftime("%Y-%m-%d %H:%M:%S",
                                      time.localtime(os.path.getmtime(exe))))
    print("mode = %s   采 %d 帧/配置（丢前 %d）"
          % (args.mode, args.frames, args.warmup))
    print("")

    rows = []
    for scene in scenes:
        for size in sizes:
            print("  ... %-34s %-11s" % (os.path.basename(scene), size), end="", flush=True)
            if args.mode == "game":
                r = bench_game_one(os.path.dirname(exe), scene, size,
                                   args.frames, args.warmup, args.timeout)
            else:
                r = bench_one(exe, scene, size, args.frames, args.warmup, args.timeout)
            r["mode"] = args.mode
            rows.append(r)
            if r["ok"]:
                print("\r  ok  %-34s %-11s n=%-3d gpu %.1f ms  ~%.0f fps"
                      % (os.path.basename(scene), size, r["n"],
                         r["gpu_med_ms"], r["fps"]))
            else:
                print("\r  ✗   %-34s %-11s %s"
                      % (os.path.basename(scene), size, r.get("error", "?")))

    ok = [r for r in rows if r["ok"]]
    print("")
    if ok:
        if args.mode == "game":
            print("| 场景 | 分辨率 | GPU 样本 | GPU 中位 (ms) | GPU max (ms) | 墙钟 fps |")
            print("|---|---|---|---|---|---|")
            for r in ok:
                print("| `%s` | %s | %d | %.1f | %.1f | %.0f |"
                      % (os.path.basename(r["scene"]), r["size"], r["n"],
                         r["gpu_med_ms"], r["gpu_max_ms"], r["fps"]))
        else:
            print("| 场景 | 分辨率 | 帧数 | CPU 中位 (ms) | fps | CPU p90 (ms) | GPU 中位 (ms) | GPU max (ms) |")
            print("|---|---|---|---|---|---|---|---|")
            for r in ok:
                print("| `%s` | %s | %d | %.1f | %.1f | %.1f | %.1f | %.1f |"
                      % (os.path.basename(r["scene"]), r["size"], r["n"],
                         r["cpu_med_ms"], r["fps"], r["cpu_p90_ms"],
                         r["gpu_med_ms"], r["gpu_max_ms"]))

    # 分段（贵在哪一段）：P2~P10 的取舍全看这张。只印非零段，按耗时降序。
    seg_rows = [r for r in ok if r.get("segments")]
    if seg_rows:
        print("")
        print("GPU 分段中位（ms）：")
        for r in seg_rows:
            seg = dict(r["segments"])
            seg.pop("frame", None)
            hot = sorted(((v, k) for k, v in seg.items() if v >= 0.01), reverse=True)
            print("  %-22s %-10s  %s" % (os.path.basename(r["scene"]), r["size"],
                  "  ".join("%s=%.2f" % (k, v) for v, k in hot)))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"exe": exe, "mode": args.mode, "rows": rows},
                      f, ensure_ascii=False, indent=2)
        print("\nJSON → %s" % args.json)

    if len(ok) < len(rows):
        print("结果: FAIL: %d/%d 份配置没采到样本" % (len(rows) - len(ok), len(rows)))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
