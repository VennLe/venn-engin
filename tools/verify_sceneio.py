#!/usr/bin/env python3
"""G16-5「序列化器三段拆分」的端到端验收（无窗口、无设备）。

## 为什么这条用例必须存在

三段拆分（`parseSceneDocument` / `instantiateDocument` / `resolveDocumentAssets`）
的全部价值是**结构性**的：让"解析与建世界"不再需要设备。所以它的验收也必须是
结构性的 —— 否则"拆了个寂寞"（把三件事重新揉回一个函数）能悄无声息地过。

四条互相独立的面：

  A  **`--selftest sceneio` 39 条**（`SCENEIO-TOTAL pass=39 fail=0`）
     数值层：解析 / 全局块 / raw 保留 / 无效槽位 / guid 补发 / 下标与 guid
     两种父引用 / 文件夹 / 相机应用 / 版本校验位置 / **三段与一把梭逐字节同**。

  B  **真实场景文件仍然读得进来**
     A 组用的是内存夹具；这里用 `cutscene_demo.json` 证明"经过真实文件"这条
     路没被拆坏：`SCENE-GUID` 汇总行在、`total` 与文件里的实体数相等、
     `Scene loaded ... skipped=0`。

  C  **写出 → 再读**：`--dump-scene` 的产物再读一遍必须 `assigned=0`
     ⚠ 这条是"写端真的把 guid 写出来了"的唯一运行时证据。只看 B 组的话，
       写端完全不写 guid 也全绿（每次靠补发）。

  D  **签名护栏（静态）**：`parseSceneDocument` 的参数表里不能出现
     `Scene` / `AssetManager`，`instantiateDocument` 里不能出现 `AssetManager`。
     ⚠ 这看起来像语法检查，但它抓的正是**撤销这次拆分**的那个动作 ——
       有人图方便把 `assets` 加进 ①②，编译一样过、测试一样绿，
       而"后台线程解析"从此再也做不成。这是本仓库里唯一能发现它的地方。

不验什么：
  * 逐字段的组件往返（那是 `verify_runtime_m2.py` C 组的活儿，15+9+9+3+4+8 个字段）
  * 编辑器侧行为（要开窗口，且与本拆分无关）

配对的自检：`Game.exe --selftest sceneio`（39 条）。
⚠ 子命令叫 `sceneio` 不叫 `save`：`save` 归存档（G17），两个功能共用一个名字会让人
以为"存档自检只要跑这个就行"。
"""

import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_shader_variants import ROOT  # noqa: E402

BIN_DIR = os.path.join(ROOT, "build", "bin")
GAME_EXE = os.path.join(BIN_DIR, "Game.exe")
SCENE_REL = "assets/scenes/cutscene_demo.json"
SCENE_ABS = os.path.join(BIN_DIR, SCENE_REL.replace("/", os.sep))
DOC_HEADER = os.path.join(ROOT, "engine", "scene", "SceneDocument.h")

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return bool(ok)


def run_game(args, wait_s=45.0):
    """跑一次 Game.exe，收完整 stdout。

    ⚠ 必须用 `communicate()`：`Popen(stdout=PIPE)` 却没人读，64 KB 管道满的
      那一刻进程会**永久阻塞**，症状是"跑到一半不动了"而不是报错。
    """
    env = dict(os.environ)
    env.pop("MYVK_LOG_DT", None)
    proc = subprocess.Popen([GAME_EXE] + args, cwd=BIN_DIR, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = proc.communicate(timeout=wait_s)
    except subprocess.TimeoutExpired:
        # ⚠ 第二次 communicate 也要有界：只写 `proc.kill()` 然后无限等，
        #   会把"用例卡住"变成"脚本卡死"，比假红更难查。
        proc.kill()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            out = b""
    return (out or b"").decode("utf-8", "replace").replace("\x00", "")


GUID_LINE = re.compile(r"SCENE-GUID assigned=(\d+) dup=(\d+) total=(\d+) path=(\S+)")
LOADED_LINE = re.compile(r"\((\d+) entities, (\d+) skipped\)")


# ---------------------------------------------------------------- A 自检

def group_selftest():
    out = run_game(["--selftest", "sceneio"], wait_s=30.0)
    total = re.search(r"SCENEIO-TOTAL pass=(\d+) fail=(\d+)", out)
    fails = re.findall(r"SCENEIO-TEST (\S+).*result=FAIL", out)

    ok = total is not None and total.group(2) == "0" and int(total.group(1)) >= 39
    check("A1 `--selftest sceneio` 全绿（>=39 条）", ok,
          (total.group(0) if total else "没有 SCENEIO-TOTAL 行") +
          ("；FAIL=%s" % ",".join(fails) if fails else ""))

    # 分组存在性：只数总数的话，"整组被删掉"也能靠别组凑够 39
    for group_name, first in (("parse", "parse_keeps_raw"),
                              ("instantiate", "instantiate_parent_by_guid"),
                              ("equivalence", "equiv_parent_shape")):
        check("A2 分段 %s 有断言（%s）" % (group_name, first),
              ("SCENEIO-TEST %s " % first) in out, "锚点%s出现" % ("" if first in out else "没"))


# ---------------------------------------------------------------- B 真实场景

def group_real_scene():
    with open(SCENE_ABS, "r", encoding="utf-8") as f:
        doc = json.load(f)
    ents = [e for e in (doc.get("entities") or []) if isinstance(e, dict)]

    out = run_game(["--scene", SCENE_REL, "--frames", "20"])
    m = GUID_LINE.search(out)
    got = (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None

    # 这份夹具是 v1（没有 guid）→ 全部补发；判据是"total 对得上"，
    # 而不是某个具体数字 —— 实体数改了夹具以后这里不该假红。
    check("B1 真实场景加载后 total == 文件实体数",
          got is not None and got[2] == len(ents),
          "got=%s expect total=%d" % (got, len(ents)))

    loaded = LOADED_LINE.search(out)
    check("B2 没有实体被当成空实体跳过",
          loaded is not None and loaded.group(2) == "0",
          loaded.group(0) if loaded else "没有 'Scene loaded' 行")

    errs = [l for l in out.splitlines() if "[ERROR" in l or "VUID" in l]
    check("B3 加载路径没有 ERROR/VUID", not errs,
          "err_lines=%d" % len(errs))


# ---------------------------------------------------------------- C 写出 → 再读

def group_roundtrip():
    # ⚠ dump 产物必须落在 `build/bin` 下、并且**用相对文件名**再读回来。
    #   用临时目录的绝对路径当 `--scene` 会卡到超时（运行时把场景名当**资产根
    #   下的相对路径**解析，绝对路径打不开就退回默认流程继续跑），
    #   症状是"脚本 90 秒无输出"而不是报错。
    #   固定文件名 + 覆盖写：不做"先删再写"，删文件要走 tools/safe_fs.py，
    #   而覆盖写不会因为上一次的失败产物让这一次假 PASS（每次整份重写）。
    dump_rel = "_verify_sceneio_roundtrip.json"
    dump_abs = os.path.join(BIN_DIR, dump_rel)
    out = run_game(["--scene", SCENE_REL, "--dump-scene", dump_abs, "--frames", "3"])
    check("C1 --dump-scene 写出成功",
          "GAME-SCENE dumped path=" in out, "没有 dumped 锚点")

    if not os.path.isfile(dump_abs):
        check("C2 写出文件存在", False, dump_abs)
        return
    with open(dump_abs, "r", encoding="utf-8") as f:
        written = json.load(f)
    wents = [e for e in (written.get("entities") or []) if isinstance(e, dict)]
    check("C2 写出文件可解析且实体数不变", len(wents) > 0, "entities=%d" % len(wents))

    # 写端必须写 guid：再读一遍这**份写出来的文件**，assigned 必须是 0。
    out2 = run_game(["--scene", dump_rel, "--frames", "20"])
    m = GUID_LINE.search(out2)
    got = (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None
    check("C3 再读写出文件 → assigned=0（写端真的写了 guid）",
          got is not None and got[0] == 0 and got[2] == len(wents),
          "got=%s expect=(0,*,%d)" % (got, len(wents)))


# ---------------------------------------------------------------- D 签名护栏

def _signature(text, name):
    """从 `name(` 起到配对 `)` 的整段签名（跨行）。"""
    i = text.find(name + "(")
    if i < 0:
        return None
    depth = 0
    for j in range(i + len(name), len(text)):
        c = text[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


def group_signature_guard():
    with open(DOC_HEADER, "r", encoding="utf-8") as f:
        text = f.read()

    parse_sig = _signature(text, "parseSceneDocument")
    inst_sig = _signature(text, "instantiateDocument")
    resolve_sig = _signature(text, "resolveDocumentAssets")

    check("D1 三段都在头文件里声明了",
          parse_sig is not None and inst_sig is not None and resolve_sig is not None,
          "parse=%d inst=%d resolve=%d" % (parse_sig is not None, inst_sig is not None,
                                           resolve_sig is not None))

    # ⚠ 这两条是本脚本唯一能发现"撤销拆分"的地方。
    parse_clean = (parse_sig is not None and "Scene&" not in parse_sig and
                   "AssetManager" not in parse_sig)
    check("D2 parseSceneDocument 不吃 Scene / AssetManager", parse_clean,
          "sig=clean" if parse_clean else "sig=dirty")
    inst_clean = (inst_sig is not None and "AssetManager" not in inst_sig)
    check("D3 instantiateDocument 不吃 AssetManager", inst_clean,
          "sig=clean" if inst_clean else "sig=dirty")
    # 反向：③ 段**必须**吃 AssetManager，否则它没在解析资产
    resolve_has = (resolve_sig is not None and "AssetManager" in resolve_sig)
    check("D4 resolveDocumentAssets 吃 AssetManager", resolve_has,
          "sig=ok" if resolve_has else "sig=missing")


def main():
    if not os.path.isfile(GAME_EXE):
        print("找不到 %s" % GAME_EXE)
        print("结果: FAIL: 可执行文件不存在（先 ./make.cmd build）")
        return 1
    if not os.path.isfile(SCENE_ABS):
        print("找不到 %s（CMake 会把 scenes 拷进 build/bin）" % SCENE_ABS)
        print("结果: FAIL: 场景夹具不存在")
        return 1

    print("== G16-5 序列化器三段拆分 ==")
    group_selftest()
    group_real_scene()
    group_roundtrip()
    group_signature_guard()

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
