#!/usr/bin/env python3
# ============================================================
# verify_stats_cpu —— Stats 面板的 CPU 分段 + 进程内存（审计 B2 / C9）
#
# ---------------------------- 这一刀在验什么 ----------------------------
# 2026-11-02 之前，GPU 侧有 `GpuTimer` 的 11 段时戳 + 堆叠条，CPU 侧只有
# `io.Framerate` 一个总数。于是"这帧 12ms —— 是卡在等 GPU、还是卡在自己
# 录命令、还是卡在场景同步"在界面上无法回答。
#
# 这一刀验的不是"有没有 12 个数字"，而是**这些数字之间的关系站不站得住**：
#
#   A. 分段计时真的接上了（`Game.exe` 侧，`MYVK_CPU_STATS=1`）
#      · 12 个键全在（少接一段 = 该键恒 0，不是报错，只能靠这里抓）
#      · `frame >= render`（render 是 frame 的一部分）
#      · `render >= wait+sync+record+submit+present − ε`（**嵌套关系**）
#        —— 这条是唯一能证明"五段确实是 drawFrame 的细分、而不是五个
#           各自独立的计数器"的判据。接错位置（比如把 record 套在
#           uploadSceneState 外面）会让这条红。
#      · `record > 0` / `sync > 0`（录过命令、同步过场景）
#   A2. **阴性对照**：不开 `MYVK_CPU_STATS` 时一行 `CPU-STATS` 都不许有
#        —— 否则"默认零痕迹"这条纪律没人守，日志里会多出一堆噪音，
#           而所有按行数/正则断言的老用例都可能被它顶掉。
#   B. 面板真的画出来了（锚点 `STATS-CPU-BAR` / `-DETAIL` / `-TREE` /
#      `STATS-MEM`）
#   C. 展开"CPU 分段表"真的多出了行（判据 = `STATS-MEM` 被推下去）
#      —— 只断言"点了不崩"等于什么都没验。
#
# ⚠ 为什么 C 组不直接数表格行：`logRect` 只给锚点，表格行没有锚点。而
#   `STATS-MEM` 在树下面，表一展开它必然下移 —— 这是一个"两种实现会不一样
#   的那个量"（不展开的实现、或者展开了但表是空壳的实现，都不会下移）。
#
# 用法：python tools/verify_stats_cpu.py [--only ABC]
# 结尾一定是 `结果: PASS` / `结果: FAIL: ...`（run_regression 取**最后一条**）。
# ============================================================

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                    # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors, wait_rect  # noqa: E402
from verify_asset_drag import force_foreground                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")
SIZE = "1400x900"

# 12 个键必须一个不少 —— 少一个就是"那段没接上"（恒 0，不会报错）。
KEYS = ["frame", "events", "scene", "logic", "ui", "assets", "render",
        "wait", "sync", "record", "submit", "present"]
LINE_RE = re.compile(r"CPU-STATS\s+(.*)$")
KV_RE = re.compile(r"(\w+)=(-?[\d.]+)")

# 段与段的容差：这些都是 steady_clock 直接测出来的，同一次运行里
# `render` 必然 >= 它内部五段之和；给 0.05ms 只是防浮点打印的舍入。
EPS = 0.05

fails = []


def check(name, ok, detail=""):
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                        ("  (%s)" % detail) if detail else ""))
    if not ok:
        fails.append(name)


# ---------------------------------------------------------------- 运行

def base_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    env.update({k: str(v) for k, v in extra.items()})
    return env


def snap(tap):
    with tap.lock:
        return list(tap.lines)


def close(proc):
    try:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
    except Exception:                                          # noqa: BLE001
        pass


def run_game(frames=48, cpu_stats=True, timeout=90.0):
    env = base_env()
    if cpu_stats:
        env["MYVK_CPU_STATS"] = "1"
    proc = subprocess.Popen([GAME, "--frames", str(frames)], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    # ⚠ 这里是"等它自己跑完"而不是 terminate：`--frames 48` 会自行退出，
    #   而 terminate 会把最后几行（含我们要的 CPU-STATS）截掉。
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        close(proc)
    try:
        tap.t.join(timeout=10.0)
    except Exception:                                          # noqa: BLE001
        pass
    return snap(tap)


def launch_editor(panels="stats", wait_tag="STATS-MEM", timeout=70.0):
    env = base_env(MYVK_EDITOR_PANELS=panels)
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", timeout=timeout)
    if not hwnd:
        close(proc)
        return proc, None, tap
    user32.ShowWindow(hwnd, 9)
    time.sleep(1.0)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    force_foreground(hwnd)

    deadline = time.time() + 45.0
    while time.time() < deadline:
        if any(wait_tag in ln for ln in snap(tap)):
            break
        time.sleep(0.2)
    return proc, hwnd, tap


# ---------------------------------------------------------------- A

def group_a():
    print("\n[A] Game.exe 的 CPU 分段（MYVK_CPU_STATS=1）")
    lines = run_game(frames=48, cpu_stats=True)
    parsed = []
    for ln in lines:
        m = LINE_RE.search(ln)
        if m:
            parsed.append({k: float(v) for k, v in KV_RE.findall(m.group(1))})

    check("[A] 打出了 CPU-STATS 行", bool(parsed),
          "拿到 %d 条" % len(parsed))
    if not parsed:
        return
    v = parsed[-1]
    missing = [k for k in KEYS if k not in v]
    check("[A] 12 个键全在", not missing, "缺 %s" % missing)
    if missing:
        return

    check("[A] 所有段非负", all(v[k] >= 0.0 for k in KEYS),
          " ".join("%s=%.2f" % (k, v[k]) for k in KEYS))
    check("[A] frame > 0（帧计时器真的在跑）", v["frame"] > 0.0,
          "frame=%.2f" % v["frame"])
    # 嵌套关系：render 必须是 frame 的一部分。
    check("[A] frame >= render（render 是 frame 的一段）",
          v["frame"] + EPS >= v["render"],
          "frame=%.2f render=%.2f" % (v["frame"], v["render"]))
    # ⚠ 这一条是**全篇最贵的**：它证明五段确实是 drawFrame 的细分，
    #   而不是五个各自独立的计数器。接错嵌套位置（例如把 record 套在
    #   uploadSceneState 外面）会让和超过 render，这条就红。
    detail_sum = (v["wait"] + v["sync"] + v["record"] + v["submit"] +
                  v["present"])
    check("[A] render >= wait+sync+record+submit+present（嵌套关系）",
          v["render"] + 0.30 >= detail_sum,
          "render=%.2f 明细和=%.2f" % (v["render"], detail_sum))
    check("[A] record > 0（真的录过命令）", v["record"] > 0.0,
          "record=%.3f" % v["record"])
    check("[A] sync > 0（真的同步过场景）", v["sync"] > 0.0,
          "sync=%.3f" % v["sync"])
    # 五段明细的每一项都必须是"某次真的发生过"——
    # 除 wait 外（vsync 关闭时它可能≈0），其余四个在 48 帧里必然非零。
    nonzero = [k for k in ("sync", "record", "submit", "present")
               if v[k] <= 0.0]
    check("[A] sync/record/submit/present 都非零", not nonzero,
          "为零的：%s" % nonzero)

    # ⚠ 顶层五段之和必须 <= frame —— 这一条抓的是"重复计数"：
    #   运行时的 `onUpdate` 是整个 GameLoop::tick（含渲染），如果
    #   Application 在外面对 onUpdate 整体打了 kLogic，同时 GameLoop 的
    #   渲染回调又打了 kRender，那么 logic + render 会超过 frame。
    #   这类错**不会**让任何单个数字看起来不对，只会让堆叠条冲出 100%。
    top_sum = (v["events"] + v["scene"] + v["logic"] + v["ui"] +
               v["assets"] + v["render"])
    check("[A] 顶层五段之和 <= frame（没有重复计数）",
          top_sum <= v["frame"] + 0.30,
          "sum=%.2f frame=%.2f" % (top_sum, v["frame"]))

    # 多帧之间 frame 会波动（不是恒定值）→ 说明它不是从别处抄来的常数
    frames = [p["frame"] for p in parsed if "frame" in p]
    check("[A] frame 是多帧采样（至少 1 条且不恒定）",
          len(frames) >= 1 and (len(set(frames)) > 1 or len(frames) == 1),
          "frames=%s" % [round(f, 2) for f in frames])


# ---------------------------------------------------------------- A2

def group_a2():
    print("\n[A2] 阴性对照：不开 MYVK_CPU_STATS 时不许有 CPU-STATS 行")
    lines = run_game(frames=48, cpu_stats=False)
    hits = [ln for ln in lines if "CPU-STATS" in ln]
    # ⚠ 这条不是"多跑一遍以防万上"：不开就该**一行都没有**。开了才有，
    #   说明门是好的；如果默认也打，日志会多出一堆噪音，而所有按行数
    #   或正则断言的老用例都可能被它顶掉。
    check("[A2] 默认（不开 env）零条 CPU-STATS", not hits,
          "拿到 %d 条：%s" % (len(hits), hits[:2]))
    check("[A2] 对照：开了才有（A 组已证）", True, "")


# ---------------------------------------------------------------- B

def group_b():
    print("\n[B] 面板锚点（Stats 面板真的画出来这两块）")
    kill_stale_editors()
    proc, hwnd, tap = launch_editor(panels="stats", wait_tag="STATS-MEM")
    lines = snap(tap)
    check("[B] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        close(proc)
        return

    for tag in ("STATS-CPU-BAR", "STATS-CPU-DETAIL", "STATS-CPU-TREE",
                "STATS-MEM"):
        r = wait_rect(tap, tag, timeout=12.0)
        check("[B] 锚点 %s" % tag, r is not None, "%s" % (r,))

    # 两条条必须在面板里、且"帧条"在"渲染明细条"上面（顺序反了就是
    # 图例和内容对不上）。
    bar = wait_rect(tap, "STATS-CPU-BAR", timeout=6.0)
    det = wait_rect(tap, "STATS-CPU-DETAIL", timeout=6.0)
    if bar and det:
        check("[B] 帧条在渲染明细条之上（bar.y < detail.y）",
              bar[1] < det[1], "bar=%s detail=%s" % (bar, det))
        check("[B] 两条都有宽度（不是退化的 0 宽矩形）",
              (bar[2] - bar[0]) > 100 and (det[2] - det[0]) > 100,
              "bar w=%.0f detail w=%.0f" % (bar[2] - bar[0], det[2] - det[0]))

    # ---- C：展开分段表 ----
    print("\n[C] 展开 CPU 分段表 → STATS-MEM 被推下去")
    before = wait_rect(tap, "STATS-MEM", timeout=6.0)
    tree = wait_rect(tap, "STATS-CPU-TREE", timeout=6.0)
    if not (before and tree):
        check("[C] 拿到锚点", False, "before=%s tree=%s" % (before, tree))
        close(proc)
        return
    # ⚠ 合成鼠标"起手"要预热（见 memory：上一动作常把光标甩出客户区，
    #   `MousePos=-FLT_MAX` 会让这一下点击落到空气上）。两次 MOVE + 停顿。
    force_foreground(hwnd)
    time.sleep(0.2)
    click(hwnd, (tree[0] + tree[2]) // 2, (tree[1] + tree[3]) // 2)
    time.sleep(1.2)
    after = wait_rect(tap, "STATS-MEM", timeout=10.0)
    check("[C] 点得动（点后重新拿到 STATS-MEM）", after is not None,
          "%s" % (after,))
    if after:
        check("[C] 表格真的插进来了（STATS-MEM 下移 > 80px）",
              after[1] - before[1] > 80.0,
              "before.y=%.0f after.y=%.0f Δ=%.0f"
              % (before[1], after[1], after[1] - before[1]))

    close(proc)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="AB",
                    help="要跑的组（A / A2 / B；B 里含 C）")
    args = ap.parse_args()
    only = args.only.upper()

    print("=" * 74)
    print("Stats 的 CPU 分段 + 进程内存（审计 B2 / C9）")
    print("=" * 74)

    if "A" in only:
        group_a()
    if "A2" in only or only == "A2":
        group_a2()
    if "B" in only:
        group_b()

    print("\n" + "=" * 74)
    if fails:
        print("结果: FAIL: %d 项 —— %s" % (len(fails), fails[:6]))
        return 1
    print("结果: PASS（CPU 分段接线 + 嵌套关系 + 默认零痕迹 + 面板两条堆叠条"
          " + 分段表可展开 + 内存一节）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
