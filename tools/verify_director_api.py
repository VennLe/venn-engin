"""端到端验证：脚本导演 API（C6 第一批：playSequence / showDialogue / playAnim）。

==============================================================================
这个脚本验什么 / 不验什么
==============================================================================
`Game.exe --selftest script` 的 C13/C14/C15 已经验了**接线**：
  · 三条内建函数把参数原样送到宿主动作上
  · 宿主说"播不了"时脚本拿到 false 且报错（不静默成功）
  · 路径参数传数字时报类型错，不会变成"去读一个叫 3 的资产"

但自检里宿主是 `MockHost` —— 它只会**记录**，不会真的播。所以本脚本验的是
另外半条链：**宿主那边是不是真的动了**。

  ① `playSequence` → `SEQ-CUE path=... dur=8.000`（宿主真的加载并起播）
  ② `showDialogue` → `DLG-START ... node=l1 ... src=cue`（对话真的起来了）
  ③ `playAnim` → `RUNTIME-SWITCH-ANIM ... to=Run`（**组件真的被改了**）
  ④ 三条的**顺序** = 脚本里的语句顺序（同一个 `on start` 块内）
  ⑤ 三个返回值全是 true（脚本打完 `DIR-RESULT` 才算走完，中途报错会中止）
  ⑥ 播完让位：`SEQ-CUE-END`（点播是插播，不是抢占）

⚠⚠ ③ 为什么非要一条新锚点（`RUNTIME-SWITCH-ANIM`）：
  `playAnim` 最典型的失败模式是**改了个没人读的字段** ——
  `AnimatorComponent::defaultClip` 只在 `!appliedDefaultClip` 时补一次
  （`AnimatorSystem.cpp` 那个分支），只写它等于什么都没发生。
  而这种 bug 的特征是"代码越看越对、日志全绿、画面不动"。
  采样层的 `ANIM-FADE` 也靠不住（它只看 `SkinnedMeshComponent` 的
  `fadeFrom*`，硬切时压根不打）。所以必须在**写入点**留一行锚点。

⚠ 不验像素：三条全部以"引擎侧的日志"为判据。要看画面请看
  `docs/dialogue.md` / `docs/cutscene_sequencer.md` 里的手动步骤。

==============================================================================
场景怎么造（不新增固定资产）
==============================================================================
以 `cutscene_demo.json` 为基底（它有带 animator+骨架的角色和
`CutsceneCam`），在 `build/bin/` 里生成一份**探针场景**：

  · `Director` 的 `sequencer.enabled` 改成 false —— 否则它会自己播，
    日志里会混进第二段 `SEQ-CUE` 与 `DLG-START`，断言分不清是谁起的。
  · **那个带 animator 的角色**（不按名字找，2026-10-29 换过资产）加一个
    `script` 组件指向探针脚本。

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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "bin")
GAME_EXE = os.path.join(BIN, "Game.exe")

BASE_SCENE = "assets/scenes/cutscene_demo.json"
PROBE_SCENE = "assets/scenes/_probe_director.json"
PROBE_SCRIPT = "scripts/_verify_director.vks"

# 700 帧：opening 是 8 秒，`kMaxStep` 把每帧推进夹到 0.1 s，所以最快也要
# 80 帧才走完；按 24 fps 算 8 秒 ≈ 200 帧，700 帧留够三倍余量。
# ⚠ 用 `--frames`（**不是** `--max-frames`）让进程自己退出。
FRAMES = 700

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

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ---------------------------------------------------------------- 造探针

def build_probe():
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
            e["script"] = {"path": PROBE_SCRIPT, "enabled": True,
                           "timeScale": 1.0}
            hit_girl = True

    if not hit_dir or not hit_girl:
        raise SystemExit(
            "基底场景结构变了：Director=%s 带 animator 的角色=%s。"
            "本脚本依赖这两个实体（见文件头'场景怎么造'）" % (hit_dir, hit_girl))

    scene_path = os.path.join(BIN, PROBE_SCENE)
    os.makedirs(os.path.dirname(scene_path), exist_ok=True)
    with open(scene_path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)

    script_path = os.path.join(BIN, "assets", PROBE_SCRIPT)
    os.makedirs(os.path.dirname(script_path), exist_ok=True)
    with open(script_path, "w", encoding="utf-8") as f:
        f.write(PROBE_VKS)

    return scene_path, script_path


def cleanup(paths):
    for p in paths:
        try:
            os.remove(p)
        except OSError:
            pass


# ---------------------------------------------------------------- 跑

def run_game(scene_rel, wait_s=90.0):
    """跑一次 Game.exe，收完整 stdout。

    ⚠ 必须 `communicate()`：`Popen(stdout=PIPE)` 却没人读，64 KB 管道满了
      进程会**永久阻塞**（症状是"跑到一半不动"，不是报错）。
    """
    env = dict(os.environ)
    env["MYVK_LOG_DT"] = ""      # 不要逐帧探针
    argv = [GAME_EXE, "--scene", scene_rel, "--trace-states",
            "--frames", str(FRAMES)]
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

    print("== 脚本导演 API（C6 第一批）端到端 ==")
    if not os.path.isfile(GAME_EXE):
        check("0 找不到 Game.exe", False, GAME_EXE)
        return 1

    scene_path, script_path = build_probe()
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
