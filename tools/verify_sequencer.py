"""端到端验证：过场时间轴（C1）。

==============================================================================
这个脚本验什么 / 不验什么
==============================================================================
数值层面（解析 / 采样 / yaw 最短弧 / step 硬切 / 半开区间 / 跨圈 / 首帧 /
夹步长 / 循环 / 编辑态）由 `Game.exe --selftest sequence` 的 48 条 `SEQ-TEST`
负责 —— 那边是纯函数，几毫秒跑完。**本脚本只验"接到一起之后还对不对"**：

  ① 序列化读侧：`SCENE-SEQUENCER ok=1`（场景里真建出了组件）
  ② 资产加载：`SEQ-ASSET name=opening dur=8.000 loop=0 keys=5 events=4`
  ③ **事件按时刻发、顺序对**：4 条 `SEQ-EVENT` 的 t 与 kind 逐条比对。
     ⚠ 这是唯一能证明"半开区间 + 跨圈两段"在真实帧率下也对的证据 ——
     自检里帧长是钉死的，这里 dt 是浮动的。
  ④ **相机真被推动**：第一帧 `CAM-POSE` 落在**第一帧关键帧**上。
     ⚠ 演示场景里相机实体的 transform 刻意写在**原点**，与第一帧关键帧
       (5.5,-1.2,1.9) 不同 → 这条只可能来自时间轴。要是两处写同一个数，
       这条断言等于什么都没测。
  ⑤ **硬切是瞬间的**：恰好有一次跳变，且它至少是"最大平滑帧位移"的 4 倍。
     只有 `Ease::Step` 会这样；平滑插值永远给不出这种倍率。
     ⚠ 判据必须是**倍率**，不能写绝对米数：单帧位移 = 速度 × dt，而 dt 在
       真实运行时浮动（实测 24 fps 上下，还能被 `kMaxStep` 夹到 0.10 s）。
       实测平滑段最大 0.32 m/帧、硬切 2.10 m/帧 —— 用"< 0.15 m"这种绝对值
       会稳定误报（我第一版就是这么写的）。
  ⑥ **运镜走到底、并收敛到末关键帧**：最后一帧落在 (6.5,0,2.4)，且收尾三帧
     的位移是**递减到 0**（不是骤停、不是跳走）。
     ⚠ "播完撒手"（`hasPose() == false`）**在这里验不了** —— `CAM-POSE` 只在
       姿态**变化**时打印（`CameraSystem` 里有 `m_lastLogged != buf` 去重），
       "停住不动"= 没有日志行，不是"位移为 0 的行"。所以这条派给
       `Game.exe --selftest sequence` 的 `tick_finished_no_pose`；
       本脚本只钉"相机确实走完了全程并在末帧收敛"。
       顺带一提：正因为去重，**最后一条 `CAM-POSE` 就是静息姿态**。
  ⑦ UI 面：Inspector 的 Sequence 页签真的画出了路径框 / 时间滑条 / 预览按钮
  ⑧ 序列化**写**侧：`scene.save` 写回的 JSON 里有 `sequencer.path`，
     而 `playhead` / `started` / `finished` / `assetLoaded` 一个都没有
     （运行时字段落盘会让"下次读档"接着上次的播放头，那是存档系统的活）。

⚠ 不验像素：运镜的正确性用相机坐标判定，比"看着对"精确得多，也不会被
  阴影 / TAA / 取景框这些老问题污染。要看画面请用
  `python tools/make_sequencer_demo.py` 生成场景后手动进 Play。

==============================================================================
一个必须先排掉的坑
==============================================================================
**场景里没有 `CameraComponent` 实体时，运镜会被静默丢弃** ——
`CameraSystem::update` 在"没有活跃相机"时直接 return。
症状极其误导：`SEQ-EVENT` 一条不少、`SEQ-ASSET` 正常加载，**画面纹丝不动**。
所以演示场景里显式放了一台 `CutsceneCam`；本脚本 ④⑤⑥ 全部依赖它。

用法：
    python tools/verify_sequencer.py [--exe path]
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect    # noqa: E402
from verify_shader_variants import ROOT                             # noqa: E402

EDITOR_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
GAME_EXE = os.path.join(ROOT, "build", "bin", "Game.exe")

SCENE = "assets/scenes/cutscene_demo.json"

# 资产的真实内容（改 `assets/sequences/opening.seq.json` 时要同步改这里）
SEQ_NAME = "opening"
SEQ_DUR = 8.0
SEQ_KEYS = 5
EXPECTED_EVENTS = [
    (0.0, "Fade"),
    (1.2, "Footstep"),
    (4.0, "Line"),
    (6.5, "Music"),
]
FIRST_KEY_POS = (5.500, -1.200, 1.900)
LAST_KEY_POS = (6.500, 0.000, 2.400)

# 硬切那一下的阈值。判据用**倍率**：单帧位移 = 速度 × dt，dt 浮动 →
# 绝对值阈值一定会误报（实测平滑段 0.32 m/帧、硬切 2.10 m/帧 ≈ 6.5×）。
CUT_MIN_JUMP = 1.0          # "这算一次跳变"的绝对门槛（防止把噪声当硬切）
CUT_MIN_RATIO = 4.0         # 跳变 / 最大平滑帧位移 的下限
CUT_MAX_COUNT = 1           # opening.seq.json 里只有一个 step 关键帧
TRAVEL_MIN = 4.0
STOP_EPS = 0.08             # 收尾三帧的位移上限（实测 0.012，留 5 倍余量）

# 帧数：过场 8 秒 + 前面 ~66 帧加载/编管线。
# ⚠⚠ 这个数**以前是假的**：因为参数名写错成 `--max-frames`，引擎根本没看到
#   它（`applyCli` 静默忽略未知参数），进程一直跑，靠 `communicate(timeout=45)`
#   到点 kill —— 实际跑到 1000 多帧，260 这个数从来没被真正用上。改成正确的
#   `--frames` 之后它才生效，于是 260 帧**根本没跑完 8 秒**：③ 事件缺尾巴、
#   ⑥ 相机没走到底，两条一起红。
# ⚠ 取 700 而不是"算出来的 330"：fps 是浮的（`maxFps=60` 上限，实测 20~40），
#   60 fps 下 8 秒就是 480 帧 + 启动 66 帧。宁可多跑几秒，也不能卡在边界上。
MAX_FRAMES = 700

POS_RE = re.compile(r"CAM-POSE name=(\S+) pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)")
EVENT_RE = re.compile(r"SEQ-EVENT seq=(\S+) kind=(\S+) t=([\d.]+) payload=\[([^\]]*)\]")

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ---------------------------------------------------------------- 运行时那一组

def run_game(scene, wait_s=45.0, timeout=90.0):
    """跑一次 Game.exe，收完整 stdout。

    ⚠ 必须用 `communicate()`（或专门的排空线程）收输出：`Popen(stdout=PIPE)`
      却没人读，64 KB 管道满的那一刻进程会**永久阻塞**——症状是"跑到一半
      不动了"，而不是报错。
    ⚠ 用 `--frames` 让进程**自己退出**，别靠 `kill()`。靠 kill 的时候
      "日志截到哪一帧"取决于墙钟，而 8 秒的过场能不能跑完取决于当时 fps ——
      于是同一份代码有时截到第 6 秒（末帧断言全红）、有时截到第 9 秒（全绿）。
      帧数一钉，日志窗口就固定了。
    ⚠⚠ 参数名是 `--frames`，**不是** `--max-frames`。`RuntimeConfig::applyCli`
      末尾对未知参数**不报错**（注释写明"快捷方式里塞进来的东西不该让游戏
      起不来"），所以拼错一个名字完全静默：进程照常跑、日志照常打、断言照常
      过 —— 只不过它其实一直没退出，是 `communicate(timeout=45)` 到点后
      **被 kill** 的。本脚本第一版就写成 `--max-frames`，于是"不靠 kill"那句
      注释是假的：真跑起来是"靠 45 秒超时 + kill，恰好 kill 前已经越过第 260
      帧"。已改成 `--frames`。
    """
    env = dict(os.environ)
    env["MYVK_LOG_DT"] = ""          # 不要逐帧探针，日志已经够长
    argv = [GAME_EXE, "--scene", scene, "--trace-states",
            "--frames", str(MAX_FRAMES)]
    proc = subprocess.Popen(argv, cwd=os.path.dirname(GAME_EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=timeout)
    text = (out or b"").decode("utf-8", "replace").replace("\x00", "")
    return text


def group_runtime():
    txt = run_game(SCENE)
    lines = txt.splitlines()

    # ① 序列化读侧
    check("① 场景加载建出了 SequencerComponent",
          "SCENE-SEQUENCER" in txt and "ok=1" in txt,
          "found=%s" % ("SCENE-SEQUENCER" in txt))

    # ② 资产加载
    want = "SEQ-ASSET name=%s dur=%.3f" % (SEQ_NAME, SEQ_DUR)
    has_asset = want in txt and ("keys=%d" % SEQ_KEYS) in txt
    check("② 资产解析成功（dur / keys / events）", has_asset,
          "want=%s" % want)

    # ③ 事件：逐条比 t 与 kind（顺序也必须一致）
    fired = [(float(m.group(3)), m.group(2))
             for m in (EVENT_RE.search(l) for l in lines) if m]
    same_len = len(fired) == len(EXPECTED_EVENTS)
    order_ok = same_len and all(
        abs(a[0] - b[0]) < 0.01 and a[1] == b[1]
        for a, b in zip(fired, EXPECTED_EVENTS))
    check("③ 四条事件按时刻与顺序发出", order_ok,
          "fired=%s" % (fired,))

    # ④ 相机第一帧落在第一关键帧上（场景里相机实体在原点 → 只能来自时间轴）
    poses = [(float(m.group(2)), float(m.group(3)), float(m.group(4)))
             for m in (POS_RE.search(l) for l in lines) if m]
    if not poses:
        check("④ 第一帧相机 = 第一关键帧", False, "没有 CAM-POSE（没相机实体？）")
        check("⑤ 硬切是瞬间的（且总路程够长）", False, "没有 CAM-POSE")
        check("⑥ 运镜走到底、收敛到末关键帧", False, "没有 CAM-POSE")
        return
    p0 = poses[0]
    d0 = max(abs(p0[i] - FIRST_KEY_POS[i]) for i in range(3))
    check("④ 第一帧相机 = 第一关键帧", d0 < 0.05, "d=%.4f pos=%s" % (d0, p0))

    # ⑤ 硬切：最大的那一跳 vs 其余帧（倍率判据，见文件头）
    jumps = [max(abs(a[i] - b[i]) for i in range(3))
             for a, b in zip(poses, poses[1:])]
    big = [j for j in jumps if j >= CUT_MIN_JUMP]
    rest = [j for j in jumps if j < CUT_MIN_JUMP]
    top = max(big) if big else 0.0
    flat = max(rest) if rest else 0.0
    ratio = (top / flat) if flat > 0 else float("inf")
    travel = sum(jumps)
    check("⑤ 硬切是瞬间的（且总路程够长）",
          len(big) == CUT_MAX_COUNT and ratio >= CUT_MIN_RATIO
          and travel > TRAVEL_MIN,
          "cuts=%d max_jump=%.3f max_other=%.3f ratio=%.2fx travel=%.2f" %
          (len(big), top, flat, ratio, travel))

    # ⑥ 走到底 + 收尾收敛（"撒手"验不了，见文件头）
    pend = poses[-1]
    d_end = max(abs(pend[i] - LAST_KEY_POS[i]) for i in range(3))
    tail = jumps[-3:] if len(jumps) >= 3 else jumps
    settled = (not tail) or max(tail) < STOP_EPS
    check("⑥ 运镜走到底、收敛到末关键帧", d_end < 0.02 and settled,
          "d=%.4f tail_max=%.5f pos=%s" %
          (d_end, max(tail) if tail else -1.0, pend))


# ---------------------------------------------------------------- 编辑器那一组

def run_editor(scene, select, insp_tab, console_cmd=None, wait_s=25.0):
    env = dict(os.environ)
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_SELECT"] = select
    env["MYVK_EDITOR_INSP_TAB"] = insp_tab
    # ⚠ 面板**默认全关**（`g_open[]` 出厂全 0）—— 不显式打开 Inspector，
    #   `drawTabSequence` 一帧都不会跑，四个 `INS-SEQ-*` 锚点全都不会出现。
    #   症状极具误导性：编辑器正常启动、场景正常加载、日志里啥错也没有。
    env["MYVK_EDITOR_PANELS"] = "inspector"
    # ⚠⚠ `logRect()` 在**没设 `MYVK_LOG_RECTS` 时直接 return** —— 不设这个变量
    #   编辑器一切正常，但一个 `*-RECT` 锚点都不会出现。症状是"⑦ 四件套
    #   全 False，可 ⑧ 的落盘却是好的"，而且 `wait_rect` 会白等满 25 秒。
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_GRID"] = "0"
    if console_cmd:
        env["MYVK_EDITOR_CONSOLE_CMD"] = console_cmd
    kill_stale_editors()
    proc = subprocess.Popen([EDITOR_EXE], cwd=os.path.dirname(EDITOR_EXE),
                            env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            raise SystemExit("[FAIL] 编辑器窗口没出现")
        user32.ShowWindow(hwnd, 9)
        wait_rect(tap, "VP-RECT", 25.0)
        # 等锚点出现（选区 + 页签切换要几帧）
        end = time.time() + wait_s
        while time.time() < end:
            with tap.lock:
                if any("INS-SEQ-PATH" in l for l in tap.lines):
                    break
            time.sleep(0.25)
        time.sleep(1.0)
        with tap.lock:
            return list(tap.lines)
    finally:
        try:
            proc.kill()
            proc.wait(timeout=10)
        except Exception:
            pass
        if hwnd:
            try:
                user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
            except Exception:
                pass
        time.sleep(0.5)


def group_editor():
    # ⚠ `scene.save` **不吃路径参数**（实现体是 `saveSceneNow()`，落在
    #   `m_ctx->scenePath()` 上）。所以"写侧往返"只能这么做：先把场景**拷一份**
    #   到临时文件、让编辑器打开那一份、`scene.save` 原地写回、再读回来断言。
    #   第一版我写的是 `scene.save <临时路径>`，结果它把**演示场景本身**
    #   覆盖了，而临时文件从来没出现过 —— 报错信息("save 文件不存在")
    #   看起来像保存失败，其实保存得很好，只是落到了别处。
    work = os.path.join(os.path.dirname(ROOT), "_venn_verify", "sequence")
    os.makedirs(work, exist_ok=True)
    # 每次唯一的名字，而不是"先删再写"：删文件要走 `safe_fs`，而且固定名字
    # 一旦这次 save 失败，会读到**上一轮的成品** → 假 PASS。
    tmp = os.path.join(work, "roundtrip_%d.json" % int(time.time()))
    src = os.path.join(ROOT, SCENE)
    if not os.path.isfile(src):
        check("⑦ Inspector 画出 Sequence 页签四件套", False, "缺场景 %s" % src)
        check("⑧ scene.save 保留 sequencer、且不写运行时字段", False,
              "缺场景 %s" % src)
        return
    shutil.copyfile(src, tmp)

    lines = run_editor(tmp, "Director", "sequence", console_cmd="scene.save")
    txt = "\n".join(lines)

    has_header = "INS-SEQ-HEADER" in txt
    has_path = "INS-SEQ-PATH" in txt
    has_time = "INS-SEQ-TIME" in txt
    has_prev = "INS-SEQ-PREVIEW" in txt
    check("⑦ Inspector 画出 Sequence 页签四件套",
          has_header and has_path and has_time and has_prev,
          "header=%s path=%s time=%s preview=%s" %
          (has_header, has_path, has_time, has_prev))

    # ⑧ 写侧：路径写进去了，运行时字段一个都没写
    if not os.path.isfile(tmp):
        check("⑧ scene.save 保留 sequencer、且不写运行时字段", False,
              "save 文件不存在: %s" % tmp)
        return
    with open(tmp, encoding="utf-8") as f:
        doc = json.load(f)
    sq = None
    for e in doc.get("entities", []):
        if isinstance(e.get("sequencer"), dict):
            sq = e["sequencer"]
            break
    if not sq:
        check("⑧ scene.save 保留 sequencer、且不写运行时字段", False,
              "存档里没有 sequencer 块")
        return
    runtime_only = [k for k in ("playhead", "started", "finished",
                               "assetLoaded", "prevTime", "loadFailedLogged")
                    if k in sq]
    check("⑧ scene.save 保留 sequencer、且不写运行时字段",
          sq.get("path", "") != "" and not runtime_only,
          "path=%s leaked=%s" % (sq.get("path"), runtime_only or "none"))


def group_selftest():
    """`Game.exe --selftest sequence` 的数值自检（无窗口、无 Vulkan，几百毫秒）。

    ⚠⚠ 这一组是 2026-10-28（R5）才补的，补之前有一段**假绿**：
      `SequenceSelfTest.cpp` 里的 73 条 `SEQ-TEST` 一直没人消费 —— 也就是说
      "采样算错了"只在有人手动跑一遍时才会被发现。写路径（`sampleCamera`
      的插值）刚被统一到 `animation::Curve`，如果连数值侧都不自动跑，
      这一层就是**红着的闸门**（等于没有闸门）。

    断三件事：
      1. 进程 rc == 0（自检有失败项时返回项数）；
      2. `SEQ-TOTAL fail=0`；
      3. **每一条 R5 新增断言都出现过** —— 防止哪条被 `#if 0` 掉之后脚本还绿。
    """
    env = dict(os.environ)
    argv = [GAME_EXE, "--selftest", "sequence"]
    proc = subprocess.run(argv, cwd=os.path.dirname(GAME_EXE), env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=120)
    txt = (proc.stdout or b"").decode("utf-8", "replace")
    check("A0 · 过场数值自检 rc=0", proc.returncode == 0,
          "rc=%d" % proc.returncode)

    m = re.search(r"SEQ-TOTAL\s+pass=(\d+)\s+fail=(\d+)", txt)
    check("A0 · SEQ-TOTAL fail=0", bool(m) and m.group(2) == "0",
          m.group(0) if m else "没有 SEQ-TOTAL 行")

    want = [
        "seq_sample_legacy_bitwise",      # 三个老模式逐位不变（迁移锚）
        "seq_step_keeps_a_time",          # step 是"选择"不是插值
        "seq_bezier_default_quarter_x",   # bezier 落点对
        "seq_bezier_handle_changes_shape",# 手柄真的改曲线（且方向对）
        "seq_bezier_handle_clamped",      # 写反的 x 被钳回
        "seq_bezier_roundtrip_handles",   # 手柄跟着资产往返
    ]
    missing = [w for w in want if w not in txt]
    check("A0 · R5 六条曲线断言都在", not missing,
          "missing=%s" % ",".join(missing))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=EDITOR_EXE)
    ap.add_argument("--skip-editor", action="store_true")
    args = ap.parse_args()

    print("== 过场时间轴（C1）端到端 ==")
    group_selftest()
    group_runtime()
    if not args.skip_editor:
        group_editor()

    bad = [r for r in results if not r[1]]
    print("-" * 66)
    for name, ok, detail in results:
        print("%s  %s" % ("PASS" if ok else "FAIL", name))
    if bad:
        print("结果: FAIL: %d 项未通过" % len(bad))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
