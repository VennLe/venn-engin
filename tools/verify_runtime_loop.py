"""端到端验证：G03 —— `GameLoop` 固定步长调度器 + 帧管线。

这一例回答的问题只有一个：**"运行时的仿真，和编辑器里调出来的那个，是不是
同一件事"**。它拆成四组，从便宜到昂贵：

  A. **自检（不起窗口）** `Game.exe --selftest determinism`
     三条断言，全部是"确定性"的直接前提：
       A4 累加器跑 300 帧 dt=1/60 → **恰好** 300 步
          （多了 = 追赶逻辑失控，少了 = 累计误差吃掉步数）
       A5 累加器路径与 `stepFixed` 直调路径 **FNV-1a 位哈希相同**
          （这才是"逐位"，不是"差值小于 eps"）
       A6 `timeScale=0` → 一步都不推进、位置零漂移

  B. **真实主循环**（`MYVK_RUNTIME=1 --frames N`，真起窗口 / Vulkan）
     验收条款原话：「日志有 `GAME-LOOP tick=.. fixed=.. var=.. render=..`」，
     所以 B 组断的就是那行的**字段语义**，不只"出现了"：
       B3 render == tick      —— 一次 tick 一次呈现（漏画 = 黑屏；
                                 多画 = 描述符/帧资源被提前复用）
       B4 var == tick         —— 变量更新每 tick 一次
       B5 sim ≈ fixed / 60    —— 仿真的总时长必须与固定步数自洽
                                 （≈1.5% 容差；这不是"放宽阈值"，而是
                                   `fixedTimeStep` 是 float 1/60）
       B6 SUMMARY 的 ticks == 最后一条 GAME-LOOP 的 tick（统计没各算各的）
       B7 跑满 `--frames` 指定的帧数（`--frames` 是自终止口子，
          跑不满就说明有人提前 `invalidate()` 了窗口）

  C. **跨进程逐字符可复现**（同一份 `--frames` + `MYVK_FIXED_DT` 跑两次）
     `PHYS-BODY` 行序列必须**逐字符**相同。物理是最容易"看着对、其实错"的
     子系统 —— 墙钟 dt 抖动会让"看起来一样"掩盖大量不确定性，所以这里用
     `MYVK_FIXED_DT` 把 dt 钉成常量，再要求两次输出一个字都不差。

  D. **跨程序逐位一致：编辑器 Play vs Game.exe** ★本用例的核心★
     同一场景、同一 `MYVK_FIXED_DT`、同一 `MYVK_PHYS_DUMP` 采样节拍，
     两边的 `PHYS-BODY` 行**逐字符相同**。这是 `docs/ship_roadmap.md`
     §15 风险 #10（"编辑器里调好的手感，出包后不一样"）的**结构性**对策：
     G01/G03 把 `buildPhysicsWorld` / 运动学同步 / 写回实体全部提到
     `engine/physics/ScenePhysics.cpp`，两边共用同一份。
     ⚠ 这条断言的价值在于：如果将来有人把编辑器那边"顺手改一下"（比如
       在编辑器侧多乘一个 dt、或者写回时用编辑场景而不是运行态场景），
       它会立刻变红 —— 而其它几十条 GUI 用例都不会。

⚠ 采样只在**共同帧**上比（`f=20/40/60/...`）。两个程序的启动帧数不一定
  对齐（编辑器要先进 Play），所以 D 组取交集：交集空 = FAIL（说明连采样
  都没跑起来），交集非空则要求**每一行**逐字符相同。
  这正是"量测三坑"的第②条：比"两来源的数字"必须先**对齐采样窗口**。

用法：
    python tools/verify_runtime_loop.py
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_DIR = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN_DIR, "Editor.exe")
GAME_EXE = os.path.join(BIN_DIR, "Game.exe")

SCENE = "scenes/sandbox.json"
FIXED_DT = "0.0166666675"     # 与 1/60 的双精度最近值，两边必须喂同一串字符
SMOKE_FRAMES = 130            # > 2 个 60 帧日志周期，好让 B6 有得比
DET_FRAMES = 120              # 确定性对照用（20 的整数倍，采样 6 帧）
DET_DUMP_EVERY = 20
MIN_COMMON_DUMPS = 3          # 共同采样帧的下限（少于它就说明没跑起来）

LOOP_RE = re.compile(
    r"GAME-LOOP tick=(\d+) fixed=(\d+) var=(\d+) render=(\d+) "
    r"acc=([-\d.]+) alpha=([-\d.]+) sim=([\d.]+)s scale=([\d.]+) "
    r"dropped=(\d+)")
SUMMARY_RE = re.compile(
    r"GAME-LOOP-SUMMARY ticks=(\d+) fixed=(\d+) var=(\d+) render=(\d+) "
    r"sim=([\d.]+)s")
DET_RE = re.compile(r"GAME-DET (\S+) .*?result=(\w+)")
BODY_RE = re.compile(r"PHYS-BODY .*$", re.M)


def clean_env(**extra):
    """**剥掉全部 `MYVK_*`** 再叠 extra。

    为什么要剥：跑回归的人 shell 里可能挂着 `MYVK_FRAMES` / `MYVK_PHYS` 之类
    的调试变量，它们会悄悄改变被测行为（`MYVK_FRAMES` 甚至会把 `--frames`
    覆盖掉）。测试的输入必须是**自述**的。
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env.update({k: str(v) for k, v in extra.items()})
    return env


def run(exe, args=(), env_extra=None, timeout=240):
    p = subprocess.run([exe] + list(args), cwd=BIN_DIR,
                       env=clean_env(**(env_extra or {})),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", errors="replace")


def body_lines(text):
    """抽出 PHYS-BODY 行（去掉前面的 `[INFO ] ` 前缀，只留锚点之后）。"""
    return [m.group(0).strip() for m in BODY_RE.finditer(text)]


def main():
    for exe in (EDITOR_EXE, GAME_EXE):
        if not os.path.isfile(exe):
            print("找不到可执行文件:", exe)
            return 2

    # GUI 用例同时只能有一个在跑：先清场（会无差别杀 Editor.exe）
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from verify_add_menu import kill_stale_editors
        kill_stale_editors()
    except Exception as e:                                    # noqa: BLE001
        print("  （清场跳过：%s）" % e)
    subprocess.run(["taskkill", "/F", "/IM", "Game.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s%s" % ("OK  " if ok else "FAIL", name,
                              ("  <- " + detail) if detail else ""))
        if not ok:
            fails.append(name)

    print("=" * 68)
    print("G03 · GameLoop 固定步长 + 帧管线")
    print("  Game.exe   : %s" % GAME_EXE)
    print("  Editor.exe : %s" % EDITOR_EXE)
    print("=" * 68)

    # ---------------------------------------------------------- A. 自检
    print("\n--- A. 自检：--selftest determinism（无窗口）---")
    rc, txt = run(GAME_EXE, ["--selftest", "determinism"])
    check("A1 退出码 0", rc == 0, "rc=%d" % rc)

    m = re.search(r"GAME-DET-TOTAL pass=(\d+) fail=(\d+)", txt)
    if not m:
        check("A2 出现 GAME-DET-TOTAL", False, "没找到")
    else:
        np_, nf = int(m.group(1)), int(m.group(2))
        check("A2 GAME-DET-TOTAL pass=3 fail=0", np_ == 3 and nf == 0,
              "pass=%d fail=%d" % (np_, nf))

    det = DET_RE.findall(txt)
    bad = [n for n, r in det if r != "PASS"]
    check("A3 三条 GAME-DET 都 result=PASS", len(det) == 3 and not bad,
          "找到 %d 条，失败: %s" % (len(det), bad) if bad
          else "%d 条" % len(det))

    ms = re.search(r"GAME-DET steps accel=(\d+) expect=(\d+)", txt)
    check("A4 累加器 300 帧 dt=1/60 → 恰好 300 步",
          bool(ms) and ms.group(1) == ms.group(2) == "300",
          ms.group(0) if ms else "没找到 GAME-DET steps")

    mh = re.search(r"GAME-DET accel_hash=(\w+) ref_hash=(\w+)", txt)
    check("A5 累加器路径与 stepFixed 路径逐位相同",
          bool(mh) and mh.group(1) == mh.group(2),
          mh.group(0) if mh else "没找到 GAME-DET accel_hash")

    mf = re.search(r"GAME-DET frozen steps=(\d+) moved=(\S+)", txt)
    check("A6 timeScale=0 → 零推进、零漂移",
          bool(mf) and mf.group(1) == "0" and mf.group(2) == "0",
          mf.group(0) if mf else "没找到 GAME-DET frozen")

    # ---------------------------------------------------------- B. 主循环
    print("\n--- B. 真实主循环（MYVK_RUNTIME=1 --frames %d）---"
          % SMOKE_FRAMES)
    rc, txt = run(GAME_EXE, ["--frames", str(SMOKE_FRAMES), "--scene", SCENE],
                  env_extra={"MYVK_RUNTIME": "1"})
    check("B1 退出码 0 且日志有 GAME-EXIT code=0",
          rc == 0 and "GAME-EXIT code=0" in txt, "rc=%d" % rc)

    loops = LOOP_RE.findall(txt)
    check("B2 GAME-LOOP 行的 9 个字段齐全（tick/fixed/var/render/acc/"
          "alpha/sim/scale/dropped）", bool(loops),
          "%d 行" % len(loops) if loops else "一行都没有")

    if loops:
        tick, fixed, var, render, _acc, _al, sim, _sc, _dr = loops[-1]
        tick, fixed, var = int(tick), int(fixed), int(var)
        render, sim = int(render), float(sim)

        check("B3 render == tick（一次 tick 一次呈现）", render == tick,
              "tick=%d render=%d" % (tick, render))
        check("B4 var == tick（每 tick 一次变量更新）", var == tick,
              "tick=%d var=%d" % (tick, var))

        # sim 是"累计被消化的固定步时长"。用 --frames 跑满后它应当与
        # fixed * fixedTimeStep 自洽。容差 1.5%：float 的 1/60 与
        # 0.0166666675 之间有固有误差，不是靠放宽阈值糊过去的。
        want_sim = fixed * float(FIXED_DT)
        drift = abs(sim - want_sim) / want_sim if want_sim > 0 else 1.0
        check("B5 sim 与固定步数自洽（|Δ| < 1.5%）", fixed > 0 and drift < 0.015,
              "sim=%.3fs fixed*dt=%.3fs 偏差 %.3f%%"
              % (sim, want_sim, drift * 100.0))

        msum = SUMMARY_RE.search(txt)
        check("B6 GAME-LOOP-SUMMARY 的 ticks 与末条 GAME-LOOP 一致",
              bool(msum) and int(msum.group(1)) == tick,
              msum.group(0) if msum else "没找到 SUMMARY")

        check("B7 跑满 --frames %d" % SMOKE_FRAMES, tick == SMOKE_FRAMES,
              "tick=%d" % tick)

    # ---------------------------------------------------------- C. 跨进程
    print("\n--- C. 跨进程逐字符可复现（dt 固定）---")
    det_env = {"MYVK_FIXED_DT": FIXED_DT,
               "MYVK_PHYS_DUMP": str(DET_DUMP_EVERY)}
    det_args = ["--frames", str(DET_FRAMES), "--scene", SCENE]
    rc1, t1 = run(GAME_EXE, det_args, env_extra=det_env)
    rc2, t2 = run(GAME_EXE, det_args, env_extra=det_env)
    b1, b2 = body_lines(t1), body_lines(t2)
    check("C0 两次运行都退出码 0", rc1 == 0 and rc2 == 0,
          "rc=%d / %d" % (rc1, rc2))
    check("C1 两次的 PHYS-BODY 行数一致", len(b1) == len(b2),
          "%d vs %d" % (len(b1), len(b2)))
    check("C2 两次的 PHYS-BODY 逐字符相同", b1 == b2 and bool(b1),
          "%d 行" % len(b1) if b1 == b2 else
          ("首处不同: %s" % next((a for a, b in zip(b1, b2) if a != b), "?")))

    # ---------------------------------------------------------- D. 跨程序
    print("\n--- D. 编辑器 Play vs Game.exe（同一场景，逐位一致）---")
    rc3, t3 = run(EDITOR_EXE, [], env_extra={
        "MYVK_NO_WINDOW_SAVE": "1",        # 别覆盖用户的布局存档
        "MYVK_EDITOR_SCENE": SCENE,
        "MYVK_EDITOR_PLAY": "1",           # 启动即 Play（物理才走）
        "MYVK_FRAMES": str(DET_FRAMES),
        "MYVK_FIXED_DT": FIXED_DT,
        "MYVK_PHYS_DUMP": str(DET_DUMP_EVERY),
    })
    ed = body_lines(t3)
    check("D0 编辑器退出码 0", rc3 == 0, "rc=%d" % rc3)

    # 只比**共同采样帧**（量测三坑②：先对齐采样窗口）
    game_set, ed_set = set(b1), set(ed)
    common = sorted(game_set & ed_set,
                    key=lambda s: (int(re.search(r"f=(\d+)", s).group(1)),
                                   s))
    n_common = len({re.search(r"f=(\d+)", s).group(1) for s in common})
    check("D1 两边有 ≥%d 个共同采样帧" % MIN_COMMON_DUMPS,
          n_common >= MIN_COMMON_DUMPS,
          "共同帧 %d 个（game %d 行 / editor %d 行）"
          % (n_common, len(b1), len(ed)))

    # 逐字符相交：交集大小必须等于两边的较小者。少一行就说明"同一个 f、
    # 同一个 name，但数字不一样" —— 那正是手感漂移的样子。
    check("D2 共同的每一行逐字符相同", common and len(common) == len(ed),
          "%d/%d 行相同" % (len(common), len(ed)) if common
          else "没有可比的行")
    if len(common) != len(ed) and ed:
        miss = sorted(ed_set - game_set)[:3]
        print("      编辑器侧对不上的行: %s" % miss)

    # ---------------------------------------------------------- 结论
    print("\n" + "=" * 68)
    if fails:
        print("结果: FAIL: %d 项未通过 -> %s" % (len(fails), fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
