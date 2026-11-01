"""端到端验证：脚本导演 API（C6：playSequence / showDialogue / playAnim / openUI）。

==============================================================================
这个脚本验什么 / 不验什么
==============================================================================
`Game.exe --selftest script` 的 C13~C15 与 C20 已经验了**接线**：
  · 内建函数把参数原样送到宿主动作上
  · 宿主说"播不了 / 加载不了"时脚本拿到 false 且报错（不静默成功）
  · 路径参数传数字时报类型错，不会变成"去读一个叫 3 的资产"
  · `openUI("")` 是合法用法（关掉当前那块），不是失败

但自检里宿主是 `MockHost` —— 它只会**记录**，不会真的播。所以本脚本验的是
另外半条链：**宿主那边是不是真的动了**。

第一段（演出三件，`cutscene_demo` 基底）：
  ① `playSequence` → `SEQ-CUE path=... dur=8.000`（宿主真的加载并起播）
  ② `showDialogue` → `DLG-START ... node=l1 ... src=cue`（对话真的起来了）
  ③ `playAnim` → `RUNTIME-SWITCH-ANIM ... to=Run`（**组件真的被改了**）
  ④ 三条的**顺序** = 脚本里的语句顺序（同一个 `on start` 块内）
  ⑤ 三个返回值全是 true（脚本打完 `DIR-RESULT` 才算走完，中途报错会中止）
  ⑥ 播完让位：`SEQ-CUE-END`（点播是插播，不是抢占）

第二段（`openUI`，C6 第七件）——**另起一次进程**：
  ⑦ `openUI("ui/docs/hud_demo.ui.json")` → `UIDOC-OPEN ... ok=1 widgets=N src=script`
  ⑧ `openUI("")` → `UIDOC-OPEN path='' (closed) had=1 src=script`
     ⚠ `had=1` 是这条判据的全部重量：它证明"关之前那块**真的在**"。
       `had=0` 时这条日志与"启动时压根没加载过任何文档"长得一模一样。
  ⑨ `openUI("ui/docs/nope.ui.json")` → `ok=0 ... (keeping the current one)`，
     且 `UI-RESULT-SHOULD-NOT-REACH` **不出现**（失败真的中止了脚本）。
  ⑩ 失败之后 `UIDOC-STATS widgets=N` 里的 N 与 ⑦ 的 N **相同** ——
     证明"保住的是原来那块"，而不是"关成空白了"（后者 widgets 会变 0）。

⚠ 为什么 openUI 必须单独起一次进程：它最后那条**故意失败**，而运行期错误会
  **中止整个脚本** —— 混在第一段里会让 `DIR-RESULT` 永远打不出来，①~⑤ 全红，
  而真正的原因只是"我把一条会失败的语句排在前面"。

⚠⚠ ③ 为什么非要一条新锚点（`RUNTIME-SWITCH-ANIM`）：
  `playAnim` 最典型的失败模式是**改了个没人读的字段** ——
  `AnimatorComponent::defaultClip` 只在 `!appliedDefaultClip` 时补一次
  （`AnimatorSystem.cpp` 那个分支），只写它等于什么都没发生。
  而这种 bug 的特征是"代码越看越对、日志全绿、画面不动"。
  采样层的 `ANIM-FADE` 也靠不住（它只看 `SkinnedMeshComponent` 的
  `fadeFrom*`，硬切时压根不打）。所以必须在**写入点**留一行锚点。

⚠ 不验像素：全部以"引擎侧的日志"为判据。要看画面请看
  `docs/dialogue.md` / `docs/cutscene_sequencer.md` 里的手动步骤。

==============================================================================
场景怎么造（不新增固定资产）
==============================================================================
以 `cutscene_demo.json` 为基底（它有带 animator+骨架的角色和
`CutsceneCam`），在 `build/bin/` 里生成两份**探针场景**（每段一份）：

  · `Director` 的 `sequencer.enabled` 改成 false —— 否则它会自己播，
    日志里会混进第二段 `SEQ-CUE` 与 `DLG-START`，断言分不清是谁起的。
  · **那个带 animator 的角色**（不按名字找，2026-10-29 换过资产）加一个
    `script` 组件指向该段的探针脚本。

⚠ 探针场景与脚本都写在 `build/bin/` 下（**不是** `assets/`）：Game.exe 的
  工作目录是 build/bin，`--scene` 与资产路径都相对它。跑完删掉，别污染仓库。

用法：
    python tools/verify_director_api.py
"""

import argparse
import json
import os
import re
import subprocess
import sys
from safe_fs import safe_remove, safe_rmtree               # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "bin")
GAME_EXE = os.path.join(BIN, "Game.exe")

BASE_SCENE = "assets/scenes/cutscene_demo.json"
PROBE_SCENE = "assets/scenes/_probe_director.json"
PROBE_SCRIPT = "scripts/_verify_director.vks"
# 第二段（openUI）自己的探针：场景同名不同后缀，脚本也是。
PROBE_SCENE_UI = "assets/scenes/_probe_director_ui.json"
PROBE_SCRIPT_UI = "scripts/_verify_director_ui.vks"

# 700 帧：opening 是 8 秒，`kMaxStep` 把每帧推进夹到 0.1 s，所以最快也要
# 80 帧才走完；按 24 fps 算 8 秒 ≈ 200 帧，700 帧留够三倍余量。
# ⚠ 用 `--frames`（**不是** `--max-frames`）让进程自己退出。
FRAMES = 700
# UI 段只要跑到 `UIDOC-BOUNDS` 的第二次节流采样（第 60 帧）之后，
# 才能断言"失败之后那块还在"（⑤ → ⑩ 用的是 `UIDOC-STATS` 的 widgets 数）。
FRAMES_UI = 180

SEQ_PATH = "sequences/opening.seq.json"
DLG_ID = "ch01_open_01"
# ⚠ 只用这份资产**真有的** clip。`anime_girl.vmesh` 里是 `Idle|Walk` 两段
#   （各重复一次，源自 FBX 的两个同名 take）—— 没有 `Run`。
#   第一版写了 "Run"，脚本报错、三条断言全红；最后的定位靠的是
#   `RUNTIME-SWITCH-ANIM-MISS ... available=[Idle|Walk]` 那一行锚点。
#   这就是"失败要说清是哪种失败、并列出可选项"的用处。
ANIM_CLIP = "Idle"

# ⚠⚠ `.vks` 的 `let` **只能写在顶层**，不能写进 `on start` 块里
#   （第一版写成块内 `let a = ...`，编译直接失败：
#    `line 2: let is only allowed at the top level`）。
#   导致整个脚本 `not runnable` → 三条内建一条都没调到 → 六条断言全红，
#   而日志里**只有一行 WARN**，看起来像"特性没接线"。
#
# 这里索性不接返回值：三个内建**失败都会让脚本中止**（运行期错误），
# 所以 `DIR-RESULT` 出现就等价于"三条全成功"，比逐个比 bool 更短也更严格。
PROBE_VKS = """on start {
    playSequence("%s")
    showDialogue("%s")
    playAnim("%s")
    print("DIR-RESULT")
}
""" % (SEQ_PATH, DLG_ID, ANIM_CLIP)

# `openUI` 段（C6 第七件）。
#
# ⚠ 顺序是判据的一部分，别随手调：
#   ① 开一块            → ok=1 widgets=N
#   ② 关掉              → closed **had=1**（证明①真的开上了）
#   ③ 再开回来          → ok=1（这样⑨失败时才有"当前那块"可保）
#   ④ `UI-RESULT`       → 前三条**全返回 true** 才会走到这里
#   ⑤ 开一份不存在的    → ok=0，**并且脚本在这里中止**
#   ⑥ 再 print 一句     → 永远不该出现（证明失败真的不是"静默 false"）
#
# ⚠ `.vks` 的 `let` 只能写在顶层（见上面 PROBE_VKS 的注释），这里同样不接
#   返回值：三条**失败都会中止脚本**，所以 `UI-RESULT` 出现 = 三条都成功。
UI_DOC = "ui/docs/hud_demo.ui.json"
UI_DOC_BAD = "ui/docs/nope.ui.json"
PROBE_UI_VKS = """on start {
    openUI("%s")
    openUI("")
    openUI("%s")
    print("UI-RESULT")
    openUI("%s")
    print("UI-RESULT-SHOULD-NOT-REACH")
}
""" % (UI_DOC, UI_DOC, UI_DOC_BAD)

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ---------------------------------------------------------------- 造探针

def build_probe(scene_rel, script_rel, vks):
    """生成探针场景 + 探针脚本。返回 (场景绝对路径, 脚本绝对路径)。"""
    with open(os.path.join(BIN, BASE_SCENE), encoding="utf-8") as f:
        scene = json.load(f)

    hit_dir = hit_girl = False
    for e in scene.get("entities", []):
        name = e.get("name", "")
        if name == "Director" and "sequencer" in e:
            e["sequencer"]["enabled"] = False
            hit_dir = True
        # 角色实体：**不钉名字**。2026-10-29 演示角色从 `anime_girl.fbx`
        # 换成了 `animate_girl_rigged.glb`，实体名跟着变成资产名 ——
        # 而这条用例真正要的只是"那个带 animator 的角色"。按名字找的话，
        # 每换一次资产这里就要跟着改一次。
        if not hit_girl and e.get("animator"):
            e["script"] = {"path": script_rel, "enabled": True,
                           "timeScale": 1.0}
            hit_girl = True

    if not hit_dir or not hit_girl:
        raise SystemExit(
            "基底场景结构变了：Director=%s 带 animator 的角色=%s。"
            "本脚本依赖这两个实体（见文件头'场景怎么造'）" % (hit_dir, hit_girl))

    scene_path = os.path.join(BIN, scene_rel)
    os.makedirs(os.path.dirname(scene_path), exist_ok=True)
    with open(scene_path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)

    script_path = os.path.join(BIN, "assets", script_rel)
    os.makedirs(os.path.dirname(script_path), exist_ok=True)
    with open(script_path, "w", encoding="utf-8") as f:
        f.write(vks)

    return scene_path, script_path


def cleanup(paths):
    for p in paths:
        try:
            safe_remove(p)
        except OSError:
            pass


# ---------------------------------------------------------------- 跑

def run_game(scene_rel, wait_s=90.0, frames=None):
    """跑一次 Game.exe，收完整 stdout。

    ⚠ 必须 `communicate()`：`Popen(stdout=PIPE)` 却没人读，64 KB 管道满了
      进程会**永久阻塞**（症状是"跑到一半不动"，不是报错）。
    """
    env = dict(os.environ)
    env["MYVK_LOG_DT"] = ""      # 不要逐帧探针
    argv = [GAME_EXE, "--scene", scene_rel, "--trace-states",
            "--frames", str(frames or FRAMES)]
    proc = subprocess.Popen(argv, cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=30)
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


# ---------------------------------------------------------------- 判定

CUE_RE = re.compile(r"SEQ-CUE path=(\S+) dur=([\d.]+) keys=(\d+) events=(\d+)")
DLG_RE = re.compile(r"DLG-START asset=(\S+) path=\S+ node=(\S+) cps=[\d.]+ "
                    r"host=\d+ src=(\S+)")
SWITCH_RE = re.compile(r"RUNTIME-SWITCH-ANIM entity=\d+ from=\[([^\]]*)\] "
                       r"to=(\S+)")
# `openUI` 段的三条锚点
UI_OPEN_RE = re.compile(r"UIDOC-OPEN path=(\S+) ok=1 widgets=(\d+) name=\S+ "
                        r"ref=\d+x\d+ src=(\S+)")
UI_CLOSE_RE = re.compile(r"UIDOC-OPEN path='' \(closed\) had=(\d+) src=(\S+)")
UI_BAD_RE = re.compile(r"UIDOC-OPEN path=(\S+) ok=0 err=.*"
                       r"\(keeping the current one\)")
UI_STATS_RE = re.compile(r"UIDOC-STATS widgets=(\d+) drawn=(\d+)")


def first_index(lines, pred):
    for i, l in enumerate(lines):
        if pred(l):
            return i
    return -1


def main():
    # ⚠ `global` 必须在**任何**对 GAME_EXE 的读之前声明 —— 写在
    #   `add_argument(default=GAME_EXE)` 之后会直接 SyntaxError。
    global GAME_EXE

    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=GAME_EXE)
    ap.add_argument("--keep", action="store_true", help="不删探针文件（调试用）")
    args = ap.parse_args()

    GAME_EXE = args.exe

    print("== 脚本导演 API（C6：演出三件 + openUI）端到端 ==")
    if not os.path.isfile(GAME_EXE):
        check("0 找不到 Game.exe", False, GAME_EXE)
        return 1

    scene_path, script_path = build_probe(PROBE_SCENE, PROBE_SCRIPT, PROBE_VKS)
    try:
        text = run_game(PROBE_SCENE)
    finally:
        if not args.keep:
            cleanup([scene_path, script_path])

    # 调试开关：`DIRECTOR_LOG_DUMP=路径` 把整份日志落盘。
    # 排查"脚本根本没跑"这类问题时，看断言输出是没用的（全是空列表）。
    dump = os.environ.get("DIRECTOR_LOG_DUMP")
    if dump:
        with open(dump, "w", encoding="utf-8") as f:
            f.write(text)

    lines = text.splitlines()

    # ① 脚本起播了时间轴
    cues = [m for m in (CUE_RE.search(l) for l in lines) if m]
    check("① playSequence → SEQ-CUE（宿主真的加载并起播）",
          len(cues) == 1 and cues[0].group(1) == SEQ_PATH
          and float(cues[0].group(2)) > 7.0,
          "cues=%s" % [c.groups() for c in cues])

    # ② 脚本起对话
    dlgs = [m for m in (DLG_RE.search(l) for l in lines) if m]
    check("② showDialogue → DLG-START src=cue",
          len(dlgs) >= 1 and dlgs[0].group(1) == DLG_ID
          and dlgs[0].group(3) == "cue",
          "dlgs=%s" % [d.groups() for d in dlgs][:2])

    # ③ 宿主真的改了动画组件（这是本脚本的核心断言）
    swaps = [m for m in (SWITCH_RE.search(l) for l in lines) if m]
    check("③ playAnim → RUNTIME-SWITCH-ANIM to=%s" % ANIM_CLIP,
          len(swaps) == 1 and swaps[0].group(2) == ANIM_CLIP,
          "swaps=%s" % [s.groups() for s in swaps])

    # ④ 三条的顺序 = 脚本语句顺序（同一 on start 块）
    i_seq = first_index(lines, lambda l: "SEQ-CUE path=" in l)
    i_dlg = first_index(lines, lambda l: "DLG-START asset=" in l)
    i_anim = first_index(lines, lambda l: "RUNTIME-SWITCH-ANIM" in l)
    check("④ 顺序：SEQ-CUE → DLG-START → SWITCH-ANIM",
          0 <= i_seq < i_dlg < i_anim,
          "idx=(%d,%d,%d)" % (i_seq, i_dlg, i_anim))

    # ⑤ 三个返回值全 true（中途报错会让脚本中止 → 没有 DIR-RESULT）
    got_result = "DIR-RESULT" in text
    has_err = "GAME-SCRIPT runtime error" in text
    check("⑤ 三个内建都返回 true（脚本走到底）",
          got_result and not has_err,
          "result=%s runtime_error=%s" % (got_result, has_err))

    # ⑥ 播完让位
    check("⑥ 播完让位：SEQ-CUE-END",
          "SEQ-CUE-END" in text,
          "（opening 8 秒 / %d 帧）" % FRAMES)

    # ---------------- 第二段：openUI（C6 第七件）----------------
    #
    # ⚠ 必须**单独起一次进程**：它最后那条是**故意失败**的，而运行期错误会
    #   中止整个脚本 —— 混在第一段里会让 `DIR-RESULT` 永远打不出来，①~⑤
    #   全红，而真正的原因只是"我把一条会失败的语句排在了前面"。
    scene2, script2 = build_probe(PROBE_SCENE_UI, PROBE_SCRIPT_UI, PROBE_UI_VKS)
    try:
        text2 = run_game(PROBE_SCENE_UI, frames=FRAMES_UI)
    finally:
        if not args.keep:
            cleanup([scene2, script2])
    if dump:
        with open(dump + ".ui.log", "w", encoding="utf-8") as f:
            f.write(text2)
    lines2 = text2.splitlines()

    # ⑦ 宿主真的加载并显示了那块界面。
    # ⚠ 期望**恰好 2 条** `ok=1`：脚本里开了两次（①开、③关掉后再开回来）。
    #   写成 `>= 1` 的话"第三次开回来失败了"这条会漏过去，而 ⑧ 已经因为
    #   ②的 `had=1` 独立成立 —— 两个缺口正好互相掩盖。
    opens = [m for m in (UI_OPEN_RE.search(l) for l in lines2) if m]
    check("⑦ openUI(path) → UIDOC-OPEN ok=1 src=script（两次都开上）",
          len(opens) == 2 and opens[0].group(1) == UI_DOC
          and opens[0].group(3) == "script",
          "opens=%s" % [m.groups() for m in opens][:3])

    # ⑧ 关掉：`had=1` 是这条判据的全部重量。
    #   `had=0` 时这行与"启动时压根没加载过任何文档"长得一模一样 ——
    #   也就是说，"①到底有没有开上"只能靠这个数字证明。
    closes = [m for m in (UI_CLOSE_RE.search(l) for l in lines2) if m]
    check("⑧ openUI(\"\") → UIDOC-OPEN closed had=1 src=script",
          len(closes) == 1 and closes[0].group(1) == "1"
          and closes[0].group(2) == "script",
          "closes=%s" % [m.groups() for m in closes])

    # ⑨ 失败那条：宿主报了无法加载，**并且脚本在这里中止**。
    #   ⚠ 用 `UI-RESULT(?!-)` 而不是 `"UI-RESULT" in text`：
    #     `UI-RESULT-SHOULD-NOT-REACH` 里含子串 `UI-RESULT`，直接 `in` 判断
    #     会把"脚本跑到了不该到的那一句"读成"脚本正常走到底"—— 正好反了。
    reached = bool(re.search(r"UI-RESULT(?!-)", text2))
    leak = "UI-RESULT-SHOULD-NOT-REACH" in text2
    bads = [m for m in (UI_BAD_RE.search(l) for l in lines2) if m]
    check("⑨ 开一份不存在的：报 ok=0 并中止脚本（没跑到第二句 print）",
          reached and not leak and len(bads) == 1
          and bads[0].group(1) == UI_DOC_BAD,
          "reached=%s leak=%s bad=%s" % (reached, leak,
                                         [m.groups() for m in bads]))

    # ⑩ 失败之后**当前那块还在**。
    #   ⚠ 不能只看"有没有 UIDOC-STATS"：写成"先清空再尝试加载"的实现里，
    #     `m_uiDoc.valid = false` 会让整个采样块被跳过 —— 那确实"没有 STATS"，
    #     但症状是"屏幕空了"，与判据要的正好相反。所以要**比数字**：
    #     失败前最后一条 STATS 的 widgets 必须与失败后的相同。
    i_bad = first_index(lines2, lambda l: "UIDOC-OPEN" in l and "ok=0" in l)
    if i_bad >= 0:
        before = [m for m in (UI_STATS_RE.search(l) for l in lines2[:i_bad]) if m]
        after = [m for m in (UI_STATS_RE.search(l) for l in lines2[i_bad:]) if m]
    else:
        before, after = [], []
    want = before[-1].group(1) if before else None
    check("⑩ 失败之后当前那块还在（UIDOC-STATS widgets 不变）",
          len(after) >= 1 and int(after[-1].group(1)) > 0
          and (want is None or after[-1].group(1) == want),
          "before=%s after=%s" % (want, [m.group(1) for m in after][:4]))

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    # ⚠ 收尾行必须是 `结果: PASS` / `结果: FAIL` —— `run_regression.py` 的
    #   OK_RE 只认这句。只打 "6/6 PASS" 会 rc=0 却判 FAIL（上一例踩过）。
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
