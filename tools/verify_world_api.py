"""端到端验证：脚本世界控制 API（C6 第二批：setFlag / getFlag / spawn / loadScene）。

==============================================================================
这个脚本验什么 / 不验什么
==============================================================================
`Game.exe --selftest script` 的 C16~C19 已经验了**接线**：
  · setFlag/getFlag 写读往返、bool→1、未设过=0、覆盖不累加
  · spawn 两种形态的位置都原样到宿主、返回的句柄可以当场 setPos/pos
  · spawn 失败 / loadScene 被拒绝时脚本拿到 false **并且报错**

但自检里宿主是 `MockHost` —— 它只记录，不会真的刷实体、也不会真的换场景。
所以本脚本验另外半条链：**宿主那边真的动了**。

  ① `setFlag("affection", 42)` → `RUNTIME-FLAG set name=affection value=42.0000`
  ② `spawn("models/anime_girl.fbx", vec3(...))` → `RUNTIME-SPAWN ... skinned=1`
     （真的走了 FBX 装配，不是"回了个句柄"）
  ③ `loadScene("scenes/..._b.json")` → `RUNTIME-SCENE-REQ ...`（请求受理）
  ④ 下一帧真切换 → `RUNTIME-SCENE-SWITCH path=... entities=N`（N > 0）
  ⑤ **旗标跨场景保留**：新场景的脚本 `setFlag("seen_ch2", getFlag("affection"))`
     打出 `value=42.0000`。这一条是本脚本真正的靶心 —— 它同时证明了
     "旧场景的旗标没被清"与"新场景的脚本真的跑起来了"。
  ⑥ 五条的**顺序** = 脚本里的语句顺序 + 换场景"下一帧生效"的语义

⚠ 不验像素。要看画面请看 `docs/script_api.md` 的手动步骤。

==============================================================================
场景怎么造（不新增固定资产）
==============================================================================
以 `cutscene_demo.json` 为基底生成**两份**探针场景（都在 `build/bin/` 下）：

  场景 A：`anime_girl` 挂探针脚本 A（做旗标 / 刷实体 / 换场景）
  场景 B：`anime_girl` 挂探针脚本 B（读旗标 + 打一行标记）

两份都把 `Director.sequencer.enabled` 关掉 —— 否则它自己会播 opening，
日志里混进 `SEQ-CUE` / `DLG-START`，排查时容易看错是谁起的。

⚠⚠ `.vks` 的 `let` **只能写在顶层**，不能写进 `on start` 块里
   （第一版写成块内 `let a = ...` → 编译直接失败：
    `line 2: let is only allowed at the top level`，而症状是六条断言全红、
    `runtime_error=False`，因为**编译错不是运行错**）。
   所以这里全程不接返回值：四个内建**失败都会让脚本中止**，
   `WORLD-A-DONE` 出现就等价于"全部成功"。

用法：
    python tools/verify_world_api.py
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
SCENE_A_REL = "scenes/_probe_world_a.json"
SCENE_B_REL = "scenes/_probe_world_b.json"
SCRIPT_A_REL = "scripts/_verify_world_a.vks"
SCRIPT_B_REL = "scripts/_verify_world_b.vks"

# 120 帧 ≈ 2 秒。这条链的动作全在第一帧（on start）+ 第二帧（换场景生效），
# 留 120 是给"模型加载慢"的余量（anime_girl 是蒙皮 FBX，Debug 下装配要几十 ms）。
# ⚠ 用 `--frames`（**不是** `--max-frames`）：未知参数被 `applyCli` 静默忽略，
#   拼错也能"跑过"——那一轮的日志里一条锚点都没有，只能靠断言全红发现。
FRAMES = 120

MODEL = "models/anime_girl.fbx"   # 基底场景里 anime_girl 用的就是这个
FLAG_VALUE = 42

# ⚠ `spawn` 的位置用**常量 vec3**，不用 `pos(player())`：探针场景里没有
#   角色控制器（cutscene_demo 是过场场景），取玩家会拿到无效句柄。
PROBE_A_VKS = """on start {
    setFlag("affection", %d)
    spawn("%s", vec3(2, 3, 0))
    loadScene("%s")
    print("WORLD-A-DONE")
}
""" % (FLAG_VALUE, MODEL, SCENE_B_REL)

# 新场景的脚本：把**旧场景**设的旗标抄到新键上，再打一行。
# 抄写而不是直接打印，是为了让断言只依赖我们自己控制的 `%.4f` 格式
# （`print(数字)` 走的是 `toStr()`，两者的位数不保证一致）。
PROBE_B_VKS = """on start {
    setFlag("seen_ch2", getFlag("affection"))
    print("WORLD-B-DONE")
}
"""

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ---------------------------------------------------------------- 造探针

def build_probes():
    """生成两份探针场景 + 两个探针脚本。返回待清理的绝对路径列表。"""
    with open(os.path.join(BIN, BASE_SCENE), encoding="utf-8") as f:
        base = json.load(f)

    def make(script_rel, out_rel):
        scene = json.loads(json.dumps(base))   # 深拷贝：两份互不影响
        hit_dir = hit_girl = False
        for e in scene.get("entities", []):
            name = e.get("name", "")
            if name == "Director" and "sequencer" in e:
                e["sequencer"]["enabled"] = False
                hit_dir = True
            if name == "anime_girl":
                e["script"] = {"path": script_rel, "enabled": True,
                               "timeScale": 1.0}
                hit_girl = True
        if not hit_dir or not hit_girl:
            raise SystemExit(
                "基底场景结构变了：Director=%s anime_girl=%s。"
                "本脚本依赖这两个实体（见文件头'场景怎么造'）"
                % (hit_dir, hit_girl))
        path = os.path.join(BIN, "assets", out_rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(scene, f, ensure_ascii=False, indent=1)
        return path

    paths = [make(SCRIPT_A_REL, SCENE_A_REL), make(SCRIPT_B_REL, SCENE_B_REL)]

    for rel, text in ((SCRIPT_A_REL, PROBE_A_VKS), (SCRIPT_B_REL, PROBE_B_VKS)):
        p = os.path.join(BIN, "assets", rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        paths.append(p)
    return paths


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

FLAG_RE = re.compile(r"RUNTIME-FLAG set name=(\S+) value=(-?[\d.]+) total=(\d+)")
SPAWN_RE = re.compile(r"RUNTIME-SPAWN entity=(\d+) path=(\S+) "
                      r"pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\) skinned=(\d+)")
REQ_RE = re.compile(r"RUNTIME-SCENE-REQ path=(\S+) \(applies at the next frame")
SWITCH_RE = re.compile(r"RUNTIME-SCENE-SWITCH path=(\S+) entities=(\d+) "
                       r"skipped=(\d+) bodies=(\d+) scripts=(\d+) player=(\d) "
                       r"count=(\d)")


def first_index(lines, regex):
    for i, l in enumerate(lines):
        if regex.search(l):
            return i
    return -1


def main():
    # ⚠ `global` 必须是 main 的**第一件事**：写在 `add_argument(default=...)`
    #   之后会直接 SyntaxError（verify_director_api.py 踩过一次）。
    global GAME_EXE

    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=GAME_EXE)
    ap.add_argument("--dump", default="", help="把完整 stdout 落一份盘（排查用）")
    ap.add_argument("--keep", action="store_true", help="不删探针文件（调试用）")
    args = ap.parse_args()

    GAME_EXE = args.exe

    print("== 脚本世界控制 API（C6 第二批）端到端 ==")
    if not os.path.isfile(GAME_EXE):
        check("0 找不到 Game.exe", False, GAME_EXE)
        print("\n0/1 PASS")
        print("结果: FAIL: exe missing")
        return 1

    if not os.path.isfile(os.path.join(BIN, BASE_SCENE)):
        check("0 找不到基底场景", False, BASE_SCENE)
        print("\n0/1 PASS")
        print("结果: FAIL: base scene missing")
        return 1

    probes = build_probes()
    try:
        text = run_game(SCENE_A_REL)
    finally:
        if args.keep:
            print("  （探针保留：%s）" % ", ".join(probes))
        else:
            cleanup(probes)

    if args.dump:
        with open(args.dump, "w", encoding="utf-8") as f:
            f.write(text)

    lines = text.splitlines()

    # ① 旗标：写进宿主
    flag = FLAG_RE.search(text)
    check("① setFlag → RUNTIME-FLAG",
          bool(flag) and flag.group(1) == "affection" and
          abs(float(flag.group(2)) - FLAG_VALUE) < 1e-3,
          "got=%s" % (str(flag.groups()) if flag else "（没有这一行）"))

    # ② 刷实体：真的走了 FBX 装配（skinned=1 说明进的是 Model 那一路，
    #    而不是 .obj 的单网格路径）
    sp = SPAWN_RE.search(text)
    check("② spawn → RUNTIME-SPAWN（skinned=1，走的 FBX 装配）",
          bool(sp) and sp.group(2) == MODEL and sp.group(6) == "1",
          "got=%s" % (str(sp.groups()) if sp else "（没有这一行）"))

    # ③ 请求受理
    req = REQ_RE.search(text)
    check("③ loadScene → RUNTIME-SCENE-REQ（受理，不当场切）",
          bool(req) and req.group(1).endswith("_probe_world_b.json"),
          "got=%s" % (req.group(1) if req else "（没有这一行）"))

    # ④ 下一帧真切换
    sw = SWITCH_RE.search(text)
    check("④ RUNTIME-SCENE-SWITCH（entities>0，世界真的换了）",
          bool(sw) and int(sw.group(2)) > 0,
          "got=%s" % (str(sw.groups()) if sw else "（没有这一行）"))

    # ⑤ 旗标跨场景保留：新场景的脚本把旧旗标抄到新键上
    flag2 = [m for m in FLAG_RE.finditer(text) if m.group(1) == "seen_ch2"]
    check("⑤ 旗标跨场景保留（新场景读到旧场景的 affection=42）",
          len(flag2) == 1 and abs(float(flag2[0].group(2)) - FLAG_VALUE) < 1e-3,
          "got=%s" % (str(flag2[0].groups()) if flag2 else "（没有这一行）"))

    # ⑥ 顺序：旗标 → 刷实体 → 受理 → 切换 → 新场景的旗标
    #    ⚠ 这一条同时钉住了"换场景下一帧生效"：如果切换是当帧做的，
    #      `spawn` 的锚点会被它自己的清世界吞掉（或者干脆没有这一行）。
    i1 = first_index(lines, FLAG_RE)
    i2 = first_index(lines, SPAWN_RE)
    i3 = first_index(lines, REQ_RE)
    i4 = first_index(lines, SWITCH_RE)
    i5 = first_index(lines, re.compile(r"RUNTIME-FLAG set name=seen_ch2"))
    ordered = -1 not in (i1, i2, i3, i4, i5) and i1 < i2 < i3 < i4 < i5
    check("⑥ 五步顺序 = 脚本语句顺序 + 换场景下一帧生效",
          ordered, "idx=(%d,%d,%d,%d,%d)" % (i1, i2, i3, i4, i5))

    # ⑦ 没有失败路径（失败也会留下锚点，别让它们混在"全绿"里）
    bad = [l for l in lines
           if "RUNTIME-SPAWN-FAIL" in l or "RUNTIME-SCENE-REQ rejected" in l
           or "RUNTIME-SCENE-SWITCH failed" in l
           or "GAME-SCRIPT runtime error" in l
           or "GAME-SCRIPT not runnable" in l]
    check("⑦ 没有失败锚点 / 脚本运行期错误", not bad,
          "bad=%d %s" % (len(bad), bad[0][:90] if bad else ""))

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    # ⚠ 收尾行必须是 `结果: PASS` / `结果: FAIL` —— `run_regression.py` 的
    #   OK_RE 只认这句。只打 "7/7 PASS" 会 rc=0 却判 FAIL（上一例踩过）。
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
