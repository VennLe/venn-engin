"""端到端验证：E25 GPU 时戳时间轴（docs/editor_roadmap.md §6）。

背景：Stats 面板（E15）已经画了"**本帧** 10 段按占比铺一条"的堆叠条，
回答"谁最贵"。但它回答不了"**抖不抖**" —— 这一帧的 forward 突然涨了
3ms，是偶发还是常态？那需要纵向的 60 帧时间轴。本任务就是补这一半。

交付物：
  · editor/GpuTimelinePanel.h/.cpp —— 甘特图（悬停读名称/ms/占比、
    点击某段）+ 最近 60 帧堆叠（旧在上）+ 段明细表 + 点段联动 Console；
  · 每帧一次纯读采样（GpuTimer 只存最近一帧，历史由面板自己攒）；
  · 归入 Rendering 工作区（Workspace.cpp 的 kRenderOpen）；
  · MYVK_EDITOR_GPU_TIMELINE=1 → 每 30 帧一条 GPU-TIMELINE 快照。

断言分组（每组一次独立启动）：

  T1 面板归属   env 钩子能开面板；Rendering 工作区把它打开；注册表里
                多出它的 Toggle 命令（可发现性 = E18 的既有能力自动生效）。
  T2 时间轴总长 **验收原文**：时间轴总长与 GPU-TIMING 日志的帧总耗时
                一致（误差 < 5%）。本面板的"总长"= 10 段之和（甘特图
                能画的部分），与 GpuTimer::lastTotalMs()（同一条
                GPU-TIMING 日志里的 frame）之间只差"段与段之间的空隙"。
                所以断言拆成三条：seg_sum ≈ last、gap/last < 5%、
                面板报的 last 与 GPU-TIMING 的 frame 同量级。
  T3 历史与统计  frames 涨到 60 封顶（"最近 60 帧"）；avg/max 合理；
                hot 是合法段名且 > 0 —— 证明纵向视图有真实数据。
  T4 点段联动    点 forward → CLICK seg=forward index=6 + CONSOLE-FILTER
                text=forward + 选中态保持（sel=forward）；换 ssao 再验一次
                （index=4），证明段名→下标是查表不是硬编码。
  T5 采样纯读    多跑"每 30 帧一条快照 + 每帧一次采样"后，视口逐像素
                一致 —— 采样只读 GpuTimer，不碰任何渲染资源。
  T6 零验证层错误 全部轮次无 VUID / VALIDATION / [ERROR]。

用法：
    python tools/verify_gpu_timeline.py
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_stable, find_window                    # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect         # noqa: E402
from verify_local_shadow import diff_stats, vp_box                        # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify", "e25")

TIMELINE_RE = re.compile(
    r"GPU-TIMELINE frames=(\d+) last=([\d.]+) seg_sum=([\d.]+) gap=([\d.]+) "
    r"avg=([\d.]+) max=([\d.]+) hot=(\S+):([\d.]+) sel=(\S+)")
TIMING_RE = re.compile(r"GPU-TIMING frame=([\d.]+)\b")
CLICK_RE = re.compile(r"GPU-TIMELINE-CLICK seg=(\S+) index=(\d+) ms=([\d.]+)")
FILTER_RE = re.compile(r"CONSOLE-FILTER text=(\S+)")
PANEL_RE = re.compile(r"PANEL-STATE gputimeline=(open|closed)")
REGISTRY_RE = re.compile(r"CMD-REGISTRY count=(\d+)")

# GpuTimer::Section 的顺序（GpuTimer.h）。CLICK 的 index 必须与它对上。
SECTIONS = ["shadows", "local_sh", "depth", "decals", "ssao",
            "cluster", "forward", "particles", "framegraph", "offscreen",
            "post"]

FAILS = []


def check(name, cond, detail=""):
    print("  %s %s%s" % ("[PASS]" if cond else "[FAIL]", name,
                         ("  " + str(detail)) if detail != "" else ""),
          flush=True)
    if not cond:
        FAILS.append(name)
    return cond


def launch(exe, env_extra, wait_for=(), timeout=60.0, shot=None, wait_fn=None):
    """起编辑器跑一轮，等 wait_for 全部出现（wait_fn 可选，作用于日志快照），
    返回 dict。

    MYVK_FRAMES 让进程自己跑够帧再退（比"等日志条数"更确定：GPU-TIMELINE
    的历史要满 60 帧才封顶，靠时间猜会偶发少几帧）。
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env.update(env_extra)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", 20.0)
    if hwnd and shot:
        # 截图前把窗口恢复成固定尺寸并置前 —— PrintWindow 对"最小化 /
        # 尺寸异常"的窗口会返回未合成的空白帧（与 verify_shader_variants
        # 的 run_once 同一套准备动作）。
        try:
            from capture_window import user32
            user32.ShowWindow(hwnd, 9)                      # SW_RESTORE
            time.sleep(1.2)
            user32.SetWindowPos(hwnd, 0, 0, 0, 1280, 800,
                                0x0002 | 0x0004)            # NOMOVE|NOZORDER
            time.sleep(2.0)
        except Exception as e:                                      # noqa: BLE001
            print("  WARN: 窗口准备失败 %s" % e)
    markers = [wait_for] if isinstance(wait_for, str) else list(wait_for)
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            snap = list(tap.lines)
        hit = all(any(mk in l for l in snap) for mk in markers) if markers \
            else True
        if hit and (wait_fn is None or wait_fn(snap)):
            break
        if proc.poll() is not None:
            break
        time.sleep(0.25)
    time.sleep(0.8)

    vp = None
    img = None
    try:
        vp = wait_rect(tap, "VP-RECT", 3.0)
        if shot and hwnd:
            img = capture_stable(hwnd)
            if img is not None:
                img.save(shot)
    except Exception as e:                                          # noqa: BLE001
        print("  WARN: 截图失败 %s" % e)

    try:
        from capture_window import user32
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    except Exception:                                               # noqa: BLE001
        pass
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=6)
        except subprocess.TimeoutExpired:
            proc.kill()

    with tap.lock:
        lines = list(tap.lines)
    errs = [l for l in lines
            if ("VUID" in l or "VALIDATION" in l or "[ERROR]" in l
                or "Error:" in l)]
    return {"lines": lines, "vp": vp, "img": img, "errs": errs}


def timings(lines):
    """所有 GPU-TIMELINE 快照 → list[dict]。"""
    out = []
    for l in lines:
        m = TIMELINE_RE.search(l)
        if not m:
            continue
        out.append({
            "frames": int(m.group(1)), "last": float(m.group(2)),
            "seg_sum": float(m.group(3)), "gap": float(m.group(4)),
            "avg": float(m.group(5)), "max": float(m.group(6)),
            "hot": m.group(7), "hot_ms": float(m.group(8)), "sel": m.group(9),
        })
    return out


def gpu_frames(lines):
    """GPU-TIMING 的 frame 值（Renderer 每 30 帧一条）。"""
    return [float(m.group(1)) for m in
            (TIMING_RE.search(l) for l in lines) if m]


def rel_diff(a, b):
    d = max(abs(a), abs(b), 1e-9)
    return abs(a - b) / d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()

    # ================================================ 轮 A：主轮
    # 开面板 + 时间轴快照 + GPU-TIMING（T2 要比的两侧都在这一轮里）
    a = launch(args.exe,
               {"MYVK_GPU_TIMING": "1", "MYVK_EDITOR_GPU_TIMELINE": "1",
                "MYVK_EDITOR_GPUTIMELINE": "1"},
               "GPU-TIMELINE frames=60")
    A = timings(a["lines"])

    print("\n== T1 面板归属与可发现性 ==")
    panel_states = PANEL_RE.findall("\n".join(a["lines"]))
    check("T1a env 钩子 MYVK_EDITOR_GPUTIMELINE=1 打开了面板",
          "open" in panel_states,
          "PANEL-STATE 序列 = %s" % panel_states)
    reg = [int(m.group(1)) for m in
           (REGISTRY_RE.search(l) for l in a["lines"]) if m]
    check("T1b 命令注册表包含新面板（count >= 71）",
          bool(reg) and reg[-1] >= 71, "CMD-REGISTRY count=%s" % (
              reg[-1] if reg else "?"))

    print("\n== T2 时间轴总长 == GPU-TIMING 帧总耗时（验收原文）==")
    check("T2a 拿到 GPU-TIMELINE 快照 >= 2 条", len(A) >= 2,
          "实际 %d 条" % len(A))
    check("T2b 拿到 GPU-TIMING 日志 >= 2 条", len(gpu_frames(a["lines"])) >= 2,
          "实际 %d 条" % len(gpu_frames(a["lines"])))
    if A:
        worst_seg = max(rel_diff(t["seg_sum"], t["last"]) for t in A)
        check("T2c 甘特图可画部分（seg_sum）≈ 帧总长（last），误差 < 5%",
              worst_seg < 0.05,
              "最大相对差 %.2f%%（例: last=%.3f seg_sum=%.3f）" % (
                  worst_seg * 100.0, A[-1]["last"], A[-1]["seg_sum"]))
        neg = [t for t in A if t["gap"] < 0.0]
        worst_gap = max(t["gap"] / max(t["last"], 1e-9) for t in A)
        check("T2d 空隙 gap >= 0 且 gap/last < 5%（段时间戳覆盖了整帧）",
              not neg and worst_gap < 0.05,
              "最大 gap 占比 %.2f%%，负 gap %d 条" % (worst_gap * 100.0,
                                                  len(neg)))
    else:
        for nm in "cd":
            check("T2%s （依赖 T2a）" % nm, False, "没有快照")

    gf = gpu_frames(a["lines"])
    if A and gf:
        # ⚠⚠ 只比**同源窗口**的样本。
        #   GPU-TIMING 每 30 帧才写一条；而面板的第一条快照在 `frames=7` 就
        #   写了 —— 那一格正是启动期（管线/着色器首次编译），实测 11.3~11.6ms，
        #   而稳态只有 ~8.5ms。样本只有 4 条时，这个离群点能把中位拽高 ~10%
        #   （2026-10-16 全量回归实测：面板中位 9.503 vs GPU-TIMING 中位 8.535
        #   = 10.19% FAIL，而两侧报的其实是同一个量）。
        #   修法：丢掉 frames < 30 的启动样本（GPU-TIMING 压根不采这一段），
        #   阈值**仍是 5%** —— 这是把两侧的采样窗口对齐，不是放宽标准。
        #   三轮实测（2026-10-16，剔除启动样本后）：0.40% / 0.22% / 0.33%。
        steady = [t for t in A if t["frames"] >= 30]
        if len(steady) < 2:
            steady = A
        panel_med = statistics.median(t["last"] for t in steady)
        timing_med = statistics.median(gf)
        d = rel_diff(panel_med, timing_med)
        check("T2e 面板报的总长与 GPU-TIMING 的 frame 同量级（< 5%，同源窗口）",
              d < 0.05,
              "面板中位 %.3f ms（%d 条稳态样本）vs GPU-TIMING 中位 %.3f ms，"
              "差 %.2f%%" % (panel_med, len(steady), timing_med, d * 100.0))
    else:
        check("T2e 面板报的总长与 GPU-TIMING 的 frame 同量级", False,
              "缺一侧数据")

    print("\n== T3 历史深度与统计 ==")
    if A:
        fr = [t["frames"] for t in A]
        check("T3a frames 单调不减", all(fr[i] <= fr[i + 1]
                                        for i in range(len(fr) - 1)),
              "序列 " + ",".join(str(x) for x in fr))
        check("T3b 历史涨到 60 封顶（路线图：最近 60 帧）", fr[-1] == 60,
              "最终 frames=%d" % fr[-1])
        last = A[-1]
        check("T3c avg > 0 且 max >= avg", last["avg"] > 0
              and last["max"] >= last["avg"] - 1e-6,
              "avg=%.3f max=%.3f" % (last["avg"], last["max"]))
        check("T3d hot 是合法段名且耗时 > 0",
              last["hot"] in SECTIONS and last["hot_ms"] > 0.0,
              "hot=%s:%.3f" % (last["hot"], last["hot_ms"]))
        check("T3e 未点击时 sel 为空占位", last["sel"] == "-",
              "sel=%s" % last["sel"])
    else:
        for nm in "abcde":
            check("T3%s （依赖 T2a）" % nm, False, "没有快照")

    # ================================================ 轮 B：Rendering 工作区
    print("\n== T1c Rendering 工作区归属 ==")
    b = launch(args.exe,
               {"MYVK_EDITOR_WORKSPACE": "rendering"},
               "PANEL-STATE gputimeline=open")
    b_states = PANEL_RE.findall("\n".join(b["lines"]))
    check("T1c 切到 Rendering 工作区后 GPU 时间轴是开的",
          "open" in b_states, "PANEL-STATE 序列 = %s" % b_states)
    check("T1d 工作区切换日志出现（证明真的切了）",
          any("WORKSPACE rendering" in l for l in b["lines"]),
          "WORKSPACE rendering" if any("WORKSPACE rendering" in l
                                       for l in b["lines"]) else "缺失")

    # ================================================ 轮 C / D：点段联动
    print("\n== T4 点段联动 Console ==")
    for want, want_idx in (("forward", 6), ("depth", 2)):
        c = launch(args.exe,
                   {"MYVK_EDITOR_GPU_TIMELINE": "1",
                    "MYVK_EDITOR_GPUTIMELINE": "1",
                    "MYVK_EDITOR_GPUTIMELINE_CLICK": want},
                   "CONSOLE-FILTER text=%s" % want)
        clicks = [m for m in (CLICK_RE.search(l) for l in c["lines"]) if m]
        check("T4a[%s] 出现 GPU-TIMELINE-CLICK" % want, len(clicks) >= 1,
              clicks[0].group(0) if clicks else "缺失")
        if clicks:
            check("T4b[%s] 段名 → 下标查表正确（index=%d）" % (want, want_idx),
                  clicks[0].group(1) == want
                  and int(clicks[0].group(2)) == want_idx,
                  "seg=%s index=%s" % (clicks[0].group(1), clicks[0].group(2)))
            check("T4c[%s] 被点段耗时 > 0" % want,
                  float(clicks[0].group(3)) > 0.0,
                  "%s ms" % clicks[0].group(3))
        else:
            check("T4b[%s] 段名 → 下标查表正确" % want, False, "无 CLICK")
            check("T4c[%s] 被点段耗时 > 0" % want, False, "无 CLICK")
        filt = [m.group(1) for m in
                (FILTER_RE.search(l) for l in c["lines"]) if m]
        check("T4d[%s] Console 过滤框被填上段名" % want, filt and filt[0] == want,
              "CONSOLE-FILTER text=%s" % (filt[0] if filt else "?"))
        # 选中态要**保持**到后续帧（不是点完就忘）
        sel_keep = [t["sel"] for t in timings(c["lines"])]
        check("T4e[%s] 选中态在后续快照里保持" % want,
              any(s == want for s in sel_keep),
              "sel 序列 = %s" % (sel_keep[-3:] if sel_keep else "无快照"))

    # ================================================ 轮 E：采样纯读（像素）
    print("\n== T5 采样不影响画面 ==")
    # 两轮都不开面板（不给 MYVK_EDITOR_GPUTIMELINE），差别只有"有没有每帧
    # 采样 + 每 30 帧一条快照"。等 3 条 GPU-TIMING（≈90 帧）而不是等第一条
    # —— 让两轮的帧数量级接近，画面差异不会来自"还没进入稳态"。
    e1 = launch(args.exe, {"MYVK_GPU_TIMING": "1"},
                wait_fn=lambda ls: sum(1 for l in ls
                                       if "GPU-TIMING frame=" in l) >= 3,
                shot=os.path.join(OUT_DIR, "e25_plain.png"))
    e2 = launch(args.exe,
                {"MYVK_GPU_TIMING": "1", "MYVK_EDITOR_GPU_TIMELINE": "1"},
                "GPU-TIMELINE frames=60",
                shot=os.path.join(OUT_DIR, "e25_snap.png"))
    if e1["img"] is not None and e2["img"] is not None and e1["vp"]:
        n, mx, mean, _ = diff_stats(e1["img"], e2["img"], vp_box(e1["vp"]))
        check("T5 多跑采样+快照后视口逐像素一致", mx == 0,
              "差 %d 像素，最大差 %d，均值 %.4f" % (n, mx, mean))
    else:
        check("T5 多跑采样+快照后视口逐像素一致", False,
              "截图缺失 plain=%s snap=%s" % (e1["img"] is not None,
                                            e2["img"] is not None))

    # ================================================ T6 验证层
    print("\n== T6 零验证层错误 ==")
    for tag, r in (("主轮", a), ("Rendering 工作区", b), ("像素对照(plain)", e1),
                   ("像素对照(snap)", e2)):
        check("T6 无 Vulkan 验证层报错 / [ERROR]（%s）" % tag, not r["errs"],
              "clean" if not r["errs"] else "; ".join(r["errs"][:2]))

    print()
    if FAILS:
        print("失败项：")
        for f in FAILS:
            print("  - %s" % f)
        print("\nRESULT: FAIL（%d 项）" % len(FAILS))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
