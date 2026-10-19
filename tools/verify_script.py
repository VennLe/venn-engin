"""端到端验证：脚本玩法（S6，见 docs/script_roadmap.md §7）。

**这个脚本本身是路线图 §8 那条总验收的机器化版本**：

    拿一个新关卡，从"设计玩法"到"能在编辑器里玩"，
    全程不改任何 .cpp、不重编译引擎。

被验的关卡 `assets/scenes/levels/script_demo.json` 由
`tools/gen_script_demo.py` 生成，玩法逻辑**全部**在
`assets/scripts/` 那四个 `.vks` 里。所以这里每一条断言失败，都意味着
"某个玩法必须回 C++ 才能实现"—— 那正是要防的事。

七组：

  G1 三个巡逻兵在动（**像素证据 + 对照区**）
  G2 玩家被巡逻兵撞到 → `HUD-STATS health` 下降
  G3 门按**时刻**开合（open→2.0s→close，不是按帧）
  G4 5 个拾取物 → `worldAttr("picked") == 5`
  G5 存盘往返：世界黑板保住、逐实体运行状态不保
  G6 热重载改速度 → 不重启、位置连续
  G7 `scriptbench` 帧时间 < 预算

⚠ 三个走过弯路的地方（写在这里免得下次再踩）：

  1. **像素断言必须有对照区**。两次启动之间窗口会偏 1~2 px，整图差异能冲到
     100%，于是"两张图不一样"永远成立，断言等于没写。这里比的是
     「巡逻区」与「天空区」的**显著变化像素占比**，天空必须 ≈0。
  2. **门要按时刻断言**。`DOOR-CLOSED t - DOOR-OPEN t` 是脚本自己的
     `time`（实例本地时间，kSelfTimed 自己累加），帧率怎么变它都是 2.0；
     按帧号断言则会在机器一慢就红。
  3. **`Player` 的朝向是 +X、相机挂在 -X 一侧**。第一版把巡逻兵摆在 -X，
     日志里一条错都没有，只有截图看得出"三个兵全在相机背后"。
     所以 G1 先断言画面里**确实有东西在动**，再谈动得对不对。
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window                # noqa: E402
from verify_add_menu import LogTap                                   # noqa: E402
from safe_fs import safe_rmtree                                      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, "build", "bin", "Game.exe")
# 运行时按**资产根**解析相对路径，所以从 build/bin 起跑
BIN_DIR = os.path.join(ROOT, "build", "bin")
DEMO_SCENE = "scenes/levels/script_demo.json"

OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

# 取样区（**按画面比例**，不写死像素 —— 窗口大小一变全错）
PATROL_BOX = (0.23, 0.31, 0.63, 0.43)    # 三个巡逻兵所在的那条带
SKY_BOX = (0.70, 0.08, 0.97, 0.28)       # 纯天空：任何"整图都在抖"都会在这里露馅

# 判据（自校准）：目标区显著变化像素 > 0.5%，且对照区 < 0.05%
PIX_TARGET_MIN = 0.005
PIX_CONTROL_MAX = 0.0005

# G7 的帧时间预算。
# ⚠ 这是 **Debug（/Od /Ob0）** 下的预算：本机实测约 92 ms/帧
#   （200 实例 × 100 语句 = 20000 条语句/帧，约 4.6 us/语句）。
#   它看守的是**算法级**退化（比如把 O(活跃) 写成 O(总数)），
#   不是绝对速度 —— 绝对速度在 Debug 里没有意义。
BENCH_BUDGET_MS = 200.0

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


# ---------------------------------------------------------------- 设施
def kill_stale_games():
    """清掉孤儿 Game.exe。

    ⚠ 与 `kill_stale_editors` 分开写：那个只杀 Editor.exe，而这里跑的是
      运行时。两个都杀会把别的用例的窗口一起带走。
    """
    subprocess.run(["taskkill", "/F", "/IM", "Game.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 6.0
    while time.time() < end:
        if not find_window("Venn", 0.35):
            break
        time.sleep(0.2)
    time.sleep(0.5)


def launch(args, extra=None, cwd=None):
    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_VALIDATION"] = "0"      # 验证层在 8 秒长跑里会刷几百行
    if extra:
        env.update(extra)
    proc = subprocess.Popen([GAME] + args, cwd=cwd or BIN_DIR, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    return proc, tap


def stop(proc):
    try:
        proc.terminate()
        proc.wait(timeout=10)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def wait_line(tap, needle, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        if tap.has_line(needle):
            return True
        time.sleep(0.1)
    return False


def sig_ratio(a, b, box_frac, thresh=8):
    """显著变化像素占比（阈值 8/255）。

    ⚠ 不用"整图平均差"：巡逻兵占屏不到 3%，整图平均会被地面拽平到
      零点几 —— 阈值怎么定都是错的。
    """
    from PIL import ImageChops
    if a is None or b is None:
        return 0.0
    w, h = a.size
    box = (int(box_frac[0] * w), int(box_frac[1] * h),
           int(box_frac[2] * w), int(box_frac[3] * h))
    ca = a.convert("RGB").crop(box)
    cb = b.convert("RGB").crop(box)
    hist = ImageChops.difference(ca, cb).convert("L").histogram()
    total = max(ca.size[0] * ca.size[1], 1)
    return sum(hist[thresh + 1:]) / float(total)


def door_pairs(tap):
    """→ {门名: [(open_t, close_t), …]}，按日志顺序配对。"""
    rx = re.compile(r"DOOR-(OPEN|CLOSED) (\w+) ([\d.eE+-]+)")
    out = {}
    for line in list(tap.lines):
        m = rx.search(line)
        if not m:
            continue
        kind, name, t = m.group(1), m.group(2), float(m.group(3))
        slot = out.setdefault(name, [])
        if kind == "OPEN":
            slot.append([t, None])
        elif slot and slot[-1][1] is None:
            slot[-1][1] = t
    return out


# ---------------------------------------------------------------- G1~G4
def run_play(exe_unused=None):
    """一次长跑里验 G1（像素）+ G2（扣血）+ G3（门）+ G4（拾取）。"""
    kill_stale_games()
    # 720 帧 ≈ 12 秒。下限是"巡逻兵第一次走到玩家身上"——home=+4、振幅 ±6、
    # 速度 2.5 m/s，第一次进入 1.6 m 半径大约在 5.8 秒（见 enemy_patrol.vks）。
    # ⚠ 不能卡着 6 秒起跑：`HUD-STATS` 每 60 帧才打一行，运气差的时候
    #   扣血那一刻正好落在两次采样中间（第一次跑就是这么偶发红的）。
    #   所以给足 12 秒，并且下面用**轮询**等"血真的掉了"，不用固定 sleep。
    proc, tap = launch(["--scene", DEMO_SCENE, "--frames", "720"])
    try:
        hwnd = find_window("Venn", timeout=30.0)
        if not hwnd:
            check("G1 启动示例关卡", False, "找不到窗口")
            return
        # 开局先确认"真的在跑"再截图 —— 否则后面全是拿两张一样的图比
        if not wait_line(tap, "GAME-SCRIPT precompiled", 25.0):
            check("G1 启动示例关卡", False, "脚本没预编译")
            return
        check("G1 启动示例关卡", True, "script_demo 加载完成")

        time.sleep(2.0)
        shot_a = capture_client(hwnd)[0]
        time.sleep(1.5)
        shot_b = capture_client(hwnd)[0]
        if shot_a is not None:
            shot_a.save(os.path.join(OUT_DIR, "script_demo_a.png"))
        if shot_b is not None:
            shot_b.save(os.path.join(OUT_DIR, "script_demo_b.png"))

        r_target = sig_ratio(shot_a, shot_b, PATROL_BOX)
        r_ctrl = sig_ratio(shot_a, shot_b, SKY_BOX)
        check("G1 三个巡逻兵在动（像素证据）", r_target > PIX_TARGET_MIN,
              "巡逻区 Δ=%.2f%% 阈值=%.1f%%" % (r_target * 100.0,
                                              PIX_TARGET_MIN * 100.0))
        # ⚠ 这条是"否定测试"的一半：没有它，上面那条在"整图都在抖"时也会绿
        check("G1b 对照区（天空）没变（排除整图抖动）",
              r_ctrl < PIX_CONTROL_MAX,
              "天空 Δ=%.3f%% 上限=%.2f%%" % (r_ctrl * 100.0,
                                            PIX_CONTROL_MAX * 100.0))

        # 长跑剩下的时间交给 G2/G3/G4 的日志
        hit = wait_line(tap, "PATROL-HIT", 20.0)
        check("G2 巡逻兵撞到玩家（PATROL-HIT）", hit, "")
        # ⚠ 轮询到"血真的掉了"为止，而不是 `sleep(1)` 之后读一把：
        #   `HUD-STATS` 是每 60 帧一行的节流日志，固定 sleep 会把
        #   "还没采样到"读成"血没掉"（第一次全量跑就这么红过一次）。
        deadline = time.time() + 12.0
        hp = []
        while time.time() < deadline:
            hp = []
            for line in list(tap.lines):
                m = re.search(r"HUD-STATS .*health=([\d.]+)", line)
                if m:
                    hp.append(float(m.group(1)))
            if any(h <= 90.0 for h in hp):
                break
            time.sleep(0.2)
        low = [h for h in hp if h <= 90.0]
        check("G2b HUD 血量真的下降了（HUD-STATS health<=90）", bool(low),
              "health 序列=%s（共 %d 行）" % (hp[-4:], len(hp)))

        pairs = door_pairs(tap)
        names = sorted(pairs.keys())
        ok_pairs = all(p and p[0][1] is not None for p in pairs.values())
        check("G3 两扇门都开过又关过", len(names) == 2 and ok_pairs,
              "doors=%s" % names)
        for want in ("Door_A", "Door_B"):
            ps = pairs.get(want) or []
            dt = (ps[0][1] - ps[0][0]) if ps and ps[0][1] is not None else -1.0
            # 2.0 s 是脚本里的 `wait(2.0)`。容差给到 [1.9, 2.2]：
            # 下界挡住"没等就关"，上界挡住"多等了一帧以上"。
            check("G3b %s 开着正好 2 秒（按时刻）" % want,
                  1.9 <= dt <= 2.2, "Δt=%.4fs" % dt)

        picks = []
        for line in list(tap.lines):
            m = re.search(r"PICKUP (\w+) (\d+)", line)
            if m:
                picks.append((m.group(1), int(m.group(2))))
        check("G4 5 个拾取物都被收走", len(picks) == 5,
              "picks=%s" % [p[1] for p in picks])
        check("G4b worldAttr(picked) 累加到 5",
              bool(picks) and picks[-1][1] == 5
              and wait_line(tap, "HUD-COUNT 5", 3.0),
              "last=%s" % (picks[-1] if picks else None,))
    finally:
        stop(proc)


# ---------------------------------------------------------------- G5
def run_save_roundtrip():
    """G5：跑完之后存盘 → 世界黑板进存档、逐实体运行状态不进。

    ⚠ 为什么不用 `--dump-scene`：那个是"读进来**立刻**再写一份"，
      而 `setWorldAttr` 只有在 Play 里跑起来才会发生 —— dump 出来的
      永远是空的，用它验 G5 会得到一条"看起来通过"的空断言。
    """
    kill_stale_games()
    tmp = tempfile.mkdtemp(prefix="venn_script_g5_")
    saved = os.path.join(tmp, "after.json")
    proc, tap = launch(["--scene", DEMO_SCENE, "--frames", "90"],
                       {"MYVK_RUNTIME_SAVE_SCENE": saved})
    try:
        ok = wait_line(tap, "GAME-SCENE-SAVED", 40.0)
    finally:
        stop(proc)
    if not ok or not os.path.isfile(saved):
        check("G5 退出时把运行态场景写了一份", False, "saved=%s" % saved)
        return
    check("G5 退出时把运行态场景写了一份", True, "%d bytes"
          % os.path.getsize(saved))

    text = open(saved, encoding="utf-8").read()
    doc = json.loads(text)
    sv = doc.get("scriptVars") or {}
    check("G5b 世界黑板进了存档（scriptVars.picked == 5）",
          abs(float(sv.get("picked", -1)) - 5.0) < 1e-6, "scriptVars=%s" % sv)
    # 逐实体变量（`setAttr(self(), "hp", …)` / `taken`）**不存盘**：
    # 序列化器对它们没有写端，所以全篇只该有**根**上那一个 scriptVars。
    check("G5c 逐实体运行状态没进存档（scriptVars 只出现一次）",
          text.count('"scriptVars"') == 1,
          "occurrences=%d" % text.count('"scriptVars"'))

    # ---- 读回来：黑板保住、逐实体运行状态不保 ----
    proc, tap = launch(["--scene", saved, "--frames", "150"])
    try:
        got = wait_line(tap, "PICKUP Pickup_0 6", 40.0)
        check("G5d 读回后 worldAttr 从 5 继续累加（6 而不是 1）", got, "")
        # 逐实体变量（`taken` 这个"已经捡过"的守卫）**没进存档** →
        # 5 个拾取物会**各自再被捡一次**，picked 从 5 一路涨到 10。
        #
        # ⚠ 这一条是实测后改的：路线图原文写的是"巡逻兵**位置**不保"，
        #   但实测发现位置**会**存（Transform 是普通场景数据，脚本每帧写的
        #   就是它）—— 保不住的是**脚本实例状态**。于是"巡逻兵的 home 变成
        #   了存档里的位置，它再也不会走回玩家身边"，`PATROL-HIT` 在重开
        #   之后一次都不出现（G5e 第一版就是这么红的）。
        #   所以判据换成同一件事的**可观测**形式：守卫变量清零 → 再捡一遍。
        last = None
        wait_line(tap, "PICKUP Pickup_4 10", 25.0)
        for line in list(tap.lines):
            m = re.search(r"PICKUP (\w+) (\d+)", line)
            if m:
                last = int(m.group(2))
        check("G5e 读回后实例变量被清零（拾取物又各捡一次 → 10）",
              last == 10, "last_picked=%s" % last)
    finally:
        stop(proc)
    safe_rmtree(tmp)


# ---------------------------------------------------------------- G6
HOT_V1 = """let dir = 1
let home = vec3(0, 0, 0)
let speed = 2.5
let beat = 0
on start {
    home = pos(self())
}
on update {
    p = pos(self())
    if (p.x > home.x + 6.0) { dir = -1 }
    if (p.x < home.x - 6.0) { dir = 1 }
    setPos(self(), vec3(p.x + dir * speed * dt, p.y, p.z))
    beat = beat + dt
    if (beat > 0.5) {
        beat = 0
        print("PATROL-POS", time, p.x)
    }
}
"""

# 只改**一个数字**：AST 形状不变 → `reattach` 应该成功（S-D7 第 3 条）
HOT_V2 = HOT_V1.replace("let speed = 2.5", "let speed = 1.25")

G6_SCENE = """{
  "version": 1,
  "generator": "verify_script hotreload fixture",
  "camera": {"target": [0.0, 0.0, 1.0], "yaw": 0.0, "pitch": -0.3,
             "distance": 12.0},
  "physics": {"enabled": false, "groundPlane": false,
              "fixedTimeStep": 0.0166666675, "maxSubSteps": 4},
  "entities": [
    {
      "name": "Walker",
      "transform": {"position": [0.0, 0.0, 1.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0]},
      "parent": -1, "visible": true, "castShadow": true,
      "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
               "name": "hot_cube"},
      "script": {"path": "%(script)s", "enabled": true, "timeScale": 1.0}
    }
  ]
}
"""


def run_hot_reload():
    """G6：改一个数字热重载 → 不重启、位置连续。"""
    kill_stale_games()
    tmp = tempfile.mkdtemp(prefix="venn_script_g6_")
    script = os.path.join(tmp, "walker.vks")
    with open(script, "w", encoding="utf-8", newline="\n") as f:
        f.write(HOT_V1)
    scene = os.path.join(tmp, "hot.json")
    with open(scene, "w", encoding="utf-8", newline="\n") as f:
        f.write(G6_SCENE % {"script": script.replace("\\", "\\\\")})

    proc, tap = launch(["--scene", scene, "--frames", "420"])
    try:
        if not wait_line(tap, "PATROL-POS", 30.0):
            check("G6 热重载夹具启动", False, "没有 PATROL-POS")
            return
        check("G6 热重载夹具启动", True, "")
        time.sleep(1.2)
        n_before = len([l for l in list(tap.lines) if "PATROL-POS" in l])
        with open(script, "w", encoding="utf-8", newline="\n") as f:
            f.write(HOT_V2)
        kept = wait_line(tap, "VKS-RELOAD-KEEP", 20.0)
        check("G6b 改一个数字 → 对账成功（VKS-RELOAD-KEEP）", kept, "")
        check("G6c 没走整体重启（没有 VKS-RELOAD-RESTART）",
              not any("VKS-RELOAD-RESTART" in l for l in list(tap.lines)), "")
        time.sleep(1.5)
        rows = []
        for line in list(tap.lines):
            m = re.search(r"PATROL-POS ([\d.eE+-]+) ([\d.eE+-]+)", line)
            if m:
                rows.append((float(m.group(1)), float(m.group(2))))
        # ① 时间单调：重启会把实例本地时间清零，序列里就会出现一次回落
        mono = all(rows[i][0] > rows[i - 1][0] for i in range(1, len(rows)))
        check("G6d 实例时间单调（没有重启清零）", len(rows) > 4 and mono,
              "n=%d t=%s" % (len(rows), [round(r[0], 2) for r in rows[:6]]))
        # ② 位置连续：相邻两条之间最多移动 speed*0.5 秒 = 1.25 m
        jump = 0.0
        for i in range(1, len(rows)):
            jump = max(jump, abs(rows[i][1] - rows[i - 1][1]))
        check("G6e 位置连续（相邻采样位移 <= 1.5 m）", rows and jump <= 1.5,
              "max_jump=%.3f m (n=%d, 热重载前 %d 行)"
              % (jump, len(rows), n_before))
    finally:
        stop(proc)
        safe_rmtree(tmp)


# ---------------------------------------------------------------- G7
def run_bench():
    """G7：200 实例 × 100 语句/帧 的帧时间 < 预算。"""
    proc, tap = launch(["--selftest", "scriptbench"])
    try:
        ok = wait_line(tap, "VKS-BENCH", 120.0)
    finally:
        stop(proc)
    if not ok:
        check("G7 scriptbench 打出 VKS-BENCH", False, "")
        return
    check("G7 scriptbench 打出 VKS-BENCH", True, "")
    ms = None
    inst = 0
    executed = 0
    for line in list(tap.lines):
        m = re.search(r"VKS-BENCH instances=(\d+) stmts=(\d+) ms=([\d.]+).*"
                      r"executed=(\d+)", line)
        if m:
            inst = int(m.group(1))
            ms = float(m.group(3))
            executed = int(m.group(4))
    check("G7b 200 个实例都真的执行了", inst == 200 and executed == 200,
          "instances=%d executed=%d" % (inst, executed))
    check("G7c 帧时间在预算内", ms is not None and ms < BENCH_BUDGET_MS,
          "ms=%.1f 预算=%.0f（Debug /Od）" % (ms if ms else -1.0,
                                           BENCH_BUDGET_MS))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="",
                    help="play / save / hot / bench（缺省全跑）")
    args = ap.parse_args()

    if not os.path.isfile(GAME):
        print("找不到 %s，请先 ./make.cmd build" % GAME)
        print("结果: FAIL: exe missing")
        return 1

    os.makedirs(OUT_DIR, exist_ok=True)
    try:
        if args.only in ("", "play"):
            run_play()
        if args.only in ("", "save"):
            run_save_roundtrip()
        if args.only in ("", "hot"):
            run_hot_reload()
        if args.only in ("", "bench"):
            run_bench()
    finally:
        kill_stale_games()

    print("")
    print("pass=%d fail=%d" % (g_pass, g_fail))
    if g_fail:
        print("结果: FAIL: %d 项" % g_fail)
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
