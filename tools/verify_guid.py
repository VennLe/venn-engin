#!/usr/bin/env python3
"""G16 稳定 GUID 的端到端验收（无窗口、无设备）。

验什么：
  A  `Game.exe --selftest guid` 的 26 条不变量
     （World 层的分配/反查/冲突/销毁 + 场景往返 + 选择接回）
  B  **v1 老场景**（实体上没有 guid）→ 全部补发，`dup=0`
  C  **v2 新场景**（每个实体带 guid）→ `assigned=0`
     ⚠ 这条是"读端真的读了文件里的值"的唯一证据：只看 B 组的话，
        即便读端完全忽略 `guid` 字段、每次随机补发，B 组也会全绿。
  D  v2 场景里**写一个重复 guid** → `dup=1`
     （一个文件被手改过、或者两个场景合并过一次，就会出现这种文件）

不验什么：
  * 编辑器侧的行为（Hierarchy 右键、撤销后选中项）。那要开窗口，
    走的是 `verify_sequencer.py` 里那套 GUI 写法，本脚本刻意不碰 ——
    纯内存的东西不该为了"顺手"变成 40 秒的 GUI 用例。
  * guid 的**具体数值**。分配是随机的，任何"期望等于某个 16 位 hex"的断言
    都只会在下一次运行变成假红。这里只断言关系（唯一 / 命中 / 不复用）。

配对的自检：`Game.exe --selftest guid`（26 条，其中 `scene_guid_roundtrip`
与 `scene_guid_stable_twice` 覆盖"写端真的写了"）。
"""

import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_shader_variants import ROOT  # noqa: E402

GAME_EXE = os.path.join(ROOT, "build", "bin", "Game.exe")
SCENE_REL = "assets/scenes/cutscene_demo.json"
SCENE_ABS = os.path.join(ROOT, "build", "bin", SCENE_REL)

# v2 夹具落在这里：与 Game.exe 同一个工作目录，`--scene` 用裸文件名就能打开。
# ⚠ 刻意**固定文件名 + 覆盖写**，不做"先删再写" —— 删文件要走 tools/safe_fs.py，
#   而覆盖写没有那个顾虑，也不会因为上一次的失败产物让这一次假 PASS
#   （每次开头都会重写整个文件）。
V2_REL = "_verify_guid_v2.json"
DUP_REL = "_verify_guid_dup.json"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return bool(ok)


def run_game(args, wait_s=45.0, timeout=90.0):
    """跑一次 Game.exe，收完整 stdout。

    ⚠ 必须用 `communicate()`：`Popen(stdout=PIPE)` 却没人读，64 KB 管道满的
      那一刻进程会**永久阻塞**，症状是"跑到一半不动了"而不是报错。
    """
    env = dict(os.environ)
    env.pop("MYVK_LOG_DT", None)          # 不要逐帧探针，日志已经够长
    proc = subprocess.Popen([GAME_EXE] + args, cwd=os.path.join(ROOT, "build", "bin"),
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=timeout)
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


GUID_LINE = re.compile(
    r"SCENE-GUID assigned=(\d+) dup=(\d+) total=(\d+) path=(\S+)")


def parse_guid_line(text):
    """返回 (assigned, dup, total, path)，没找到返回 None。"""
    m = GUID_LINE.search(text)
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4))


def load_scene(text):
    """跑一次场景加载，返回 SCENE-GUID 的四元组 + 完整日志。"""
    out = run_game(["--scene", text, "--frames", "40"])
    return parse_guid_line(out), out


# ---------------------------------------------------------------- A 自检

def group_selftest():
    out = run_game(["--selftest", "guid"], wait_s=30.0)
    total = re.search(r"GUID-TOTAL pass=(\d+) fail=(\d+)", out)
    fails = re.findall(r"GUID-TEST (\S+).*result=FAIL", out)
    check("A1 `--selftest guid` 全绿",
          total is not None and total.group(2) == "0" and int(total.group(1)) >= 26,
          (total.group(0) if total else "没有 GUID-TOTAL 行") +
          ("；FAIL=%s" % ",".join(fails) if fails else ""))


# ---------------------------------------------------------------- B/C/D 场景

def read_entities():
    with open(SCENE_ABS, "r", encoding="utf-8") as f:
        doc = json.load(f)
    ents = doc.get("entities") or []
    return doc, [e for e in ents if isinstance(e, dict)]


def write_variant(path_rel, mutate):
    """读源场景 → 交给 mutate 改 → 写到 build/bin 下的临时文件。

    返回 (实体数, 写出的文档)。
    """
    doc, ents = read_entities()
    mutate(doc, ents)
    with open(os.path.join(ROOT, "build", "bin", path_rel), "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
    return len(ents), doc


def group_scene():
    # ---- B：v1（源文件本身没有 guid，也要先确认这一点）----
    _, ents = read_entities()
    already = sum(1 for e in ents if isinstance(e.get("guid"), str))
    check("B0 源场景确实是 v1（没有 guid）", already == 0,
          "entities=%d with_guid=%d" % (len(ents), already))

    got, _ = load_scene(SCENE_REL)
    check("B1 v1 场景 → 全部补发、无重复",
          got is not None and got[0] == len(ents) and got[1] == 0 and got[2] == len(ents),
          "got=%s expect=(%d,0,%d)" % (got, len(ents), len(ents)))

    # ---- C：v2，每个实体一个互不相同的 guid ----
    def add_guids(doc, ents):
        doc["version"] = 2
        for i, e in enumerate(ents):
            e["guid"] = "%016x" % (0x1A2B3C4D00000000 + i)

    n, doc = write_variant(V2_REL, add_guids)
    got, out = load_scene(V2_REL)
    skipped = re.search(r"\((\d+) entities, (\d+) skipped\)", out)
    check("C1 v2 场景 → assigned=0（真的读了文件里的 guid）",
          got is not None and got[0] == 0 and got[1] == 0 and got[2] == n,
          "got=%s expect=(0,0,%d)" % (got, n))
    # 顺带钉住"没有实体被当成空实体跳过" —— 否则 total 会莫名其妙变小
    check("C2 v2 场景没有实体被跳过",
          skipped is not None and skipped.group(2) == "0",
          skipped.group(0) if skipped else "没有 'Scene loaded' 行")

    # 把 guid 全部换成小写 16 位之外的形式（大写）也不该丢 —— 手改文件常见
    def uppercase(doc, ents):
        doc["version"] = 2
        for i, e in enumerate(ents):
            e["guid"] = ("%016X" % (0x5E6F7A8B00000000 + i))

    write_variant(V2_REL, uppercase)
    got, _ = load_scene(V2_REL)
    check("C3 大写 hex 也照收（assigned=0）", got is not None and got[0] == 0,
          "got=%s" % (got,))

    # ---- D：把第 2 个实体的 guid 改成和第 1 个一样 ----
    def make_dup(doc, ents):
        doc["version"] = 2
        for i, e in enumerate(ents):
            e["guid"] = "%016x" % (0x1111111100000000 + i)
        if len(ents) >= 2:
            ents[1]["guid"] = ents[0]["guid"]

    n, _ = write_variant(DUP_REL, make_dup)
    got, out = load_scene(DUP_REL)
    check("D1 文件内重复 guid → dup=1 且不致命",
          got is not None and got[1] == 1 and got[2] == n,
          "got=%s expect dup=1 total=%d" % (got, n))
    check("D2 重复时打 SCENE-GUID-DUP 锚点",
          "SCENE-GUID-DUP" in out and "action=reassign" in out,
          "锚点%s出现" % ("" if "SCENE-GUID-DUP" in out else "没"))


def main():
    if not os.path.isfile(GAME_EXE):
        print("找不到 %s" % GAME_EXE)
        print("结果: FAIL: 可执行文件不存在（先 ./make.cmd build）")
        return 1

    print("== G16 稳定 GUID 端到端 ==")
    group_selftest()
    group_scene()

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
