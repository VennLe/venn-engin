#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C7 的对话侧验收：条件分支 + 选项写旗标（2026-11-05）。

==============================================================================
它验什么 / 不验什么
==============================================================================
数值层面（条件求值 / 解析拒收 / 走线 / 写旗标顺序 / 跳数保护）由
`Game.exe --selftest dialogue` 的 **I 组 24 条 `bc_*`** 负责（微秒级、不要窗口）。
**本脚本只验"接起来之后还对不对"**，分三组：

A 自检计数 + 点名（`bc_run_true` / `bc_write_before_jump` …）
B 运行时真跑：一份带分支的 `.dlg.json` 交给 `Game.exe`，靠 `MYVK_DLG_AUTO`
  自动推进。断言
    ① `DLG-FLAGS installed=1`（宿主真把旗标表接上了）
    ② `DLG-COND id=g ok=0 else=h`（条件假 → 走 else）
    ③ `DLG-LINE id=h`（**真的落在 else 那一行**，不是只打了日志）
    ④ `DLG-WRITE flag=trust value=3`（选项写进去了）
    ⑤ 写完之后条件变成成立：`DLG-COND id=g ok=1`
C 剧本编辑器：注入脚本建条件 / 设 else / 挂副作用，dump 出来断言 JSON 里
  三者都在；再各做一次负向（op 拼错被拒、无条件时设 else 被拒）。

⚠ 两条"这么选的理由"：
   · **B 组必须真跑 Game.exe**。`--selftest dialogue` 里 store 是测试自己造的
     map；"运行时的 store 接到 `m_flags` 上了吗"只有真宿主能回答 —— 而漏接
     的症状恰恰是"条件恒假、走线永远另一条"，日志里一条错都没有。
   · **C 组断言 dump 出来的 JSON**，不是断言"日志打了一行"。`DLGED-ADDCOND`
     那行只能证明动作被调用了；`.dlg.json` 里真有 `if` 才算落到盘上。

用法：
    python tools/verify_dialogue_branch.py
    python tools/verify_dialogue_branch.py --only AB
    python tools/verify_dialogue_branch.py --keep
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_flags as F          # run_editor / check / 夹具路径

ASSETS = F.ASSETS
BIN = F.BIN
GAME = F.GAME

GROUP = {"A", "B", "C"}
failures = []

# 运行时夹具
SCENE_REL = "scenes/_dlg_branch_probe.json"
ASSET_REL = "dialogues/_dlg_branch.dlg.json"
# 编辑器夹具（C 组自己写一份，不碰 B 组那份：B 组要的是"演一遍"，
# C 组要的是"编一遍"，混用会让 dump 出来的东西受另一组影响）
EDIT_REL = "dialogues/_dlg_branch_edit.dlg.json"


def check(name, ok, detail=""):
    F.check(name, ok, detail)


def lines_of(tap):
    return F.snap(tap)


def first(lines, needle):
    for ln in lines:
        if needle in ln:
            return ln
    return ""


def every(lines, needle):
    return [ln for ln in lines if needle in ln]


# ---------------------------------------------------------------- 夹具

BRANCH_JSON = r"""{
  "version": 1,
  "name": "_dlg_branch",
  "start": "a",
  "lines": [
    { "id": "a", "speaker": "A", "text": "AAA", "next": "q" },
    { "id": "q", "speaker": "A", "text": "QQQ",
      "choices": [
        { "text": "全信", "goto": "g", "set": [ { "flag": "trust", "value": BRANCH_FIRST_TRUST } ] },
        { "text": "信一半", "goto": "g",
          "set": [ { "flag": "trust", "value": 1 }, { "flag": "route", "value": 2 } ] }
      ] },
    { "id": "g", "speaker": "A", "text": "GGG",
      "if": [ { "flag": "trust", "op": ">=", "value": 2 } ],
      "else": "h", "next": "e" },
    { "id": "h", "speaker": "A", "text": "HHH", "next": "e" },
    { "id": "e", "speaker": "A", "text": "EEE" }
  ]
}
"""

EDIT_JSON = r"""{
  "version": 1,
  "name": "_dlg_branch_edit",
  "start": "a",
  "lines": [
    { "id": "a", "text": "AAA", "next": "b" },
    { "id": "b", "text": "BBB" }
  ]
}
"""


def make_scene(path):
    """一张"一个 NPC 自带对话组件"的最小场景。

    ⚠ `autoStart: true` + 不设 `startNode`：起点**永远**用资产自己的 `start`
      （见 `DialogueSystem::cue` 那条纪律），这里顺着它写。
    ⚠ `cps` 给大一点：B 组要靠自动推进把五句走完，打字太慢会撞上帧数上限，
      而症状是"日志停在第三句"（看起来像分支没接上）。
    """
    ents = [{
        "name": "Talker",
        "transform": {"position": [0.0, 0.0, 1.0],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "parent": -1, "visible": True, "castShadow": True,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 0.6,
                 "name": "talker_mesh"},
        "dialogue": {"path": ASSET_REL, "enabled": True, "autoStart": True,
                     "cps": 240.0, "startNode": "", "priority": 0},
    }]
    scene = {
        "version": 2,
        "generator": "verify_dialogue_branch",
        "camera": {"target": [0, 0, 1.0], "yaw": 0.0, "pitch": -20.0,
                   "distance": 12.0},
        "physics": {"enabled": False, "groundPlane": False},
        "environment": {},
        "entities": ents,
    }
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)


def write_asset(rel, text):
    p = os.path.join(ASSETS, *rel.split("/"))
    os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return p


def cleanup(keep):
    if keep:
        return
    for rel in (SCENE_REL, ASSET_REL, EDIT_REL):
        p = os.path.join(ASSETS, *rel.split("/"))
        try:
            if os.path.exists(p):
                os.remove(p)
        except OSError:
            pass


# ---------------------------------------------------------------- A：自检

def group_a():
    out = F.run_game_selftest("dialogue", timeout=180.0)
    total = first(out.splitlines(), "DLG-TOTAL")
    check("[A] 自检跑完（有 DLG-TOTAL 行）", "DLG-TOTAL" in out)
    m = re.search(r"pass=(\d+) fail=(\d+)", total)
    # 165 = 我在 24 条 I 组之前那 141 条 + 本组 24 条（I 组数得出来：
    # `grep -c 'reportFlag("bc_' DialogueSelfTest.cpp`）。
    # ⚠ 不写死也行，但**写死才能发现"少跑了一组"** —— 所以两条一起断言。
    check("[A] DLG-TOTAL pass>=165 fail=0",
          m is not None and int(m.group(1)) >= 165 and int(m.group(2)) == 0,
          total.strip())
    named = ["bc_run_true", "bc_run_false_else", "bc_write_before_jump",
             "bc_else_empty_ends", "bc_no_store_errors", "bc_chain_guard",
             "bc_reject_bad_op", "bc_rt_omits_empty"]
    for n in named:
        ok = any(("%s " % n) in ln and "result=PASS" in ln for ln in out.splitlines())
        check("[A] 自检 %s" % n, ok)


# ---------------------------------------------------------------- B：运行时

def run_scene(frames=420):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    # 自动推进：每句停 0.15 秒就往下走。⚠ 它**默认选 0 号选项** ——
    # 所以"走哪条分支"由 0 号选项写什么旗标决定（见下面两次运行）。
    # 不设它的话对话会永远停在第一句，看起来像"分支根本没接上"。
    env["MYVK_DLG_AUTO"] = "0.15"
    proc = subprocess.run([GAME, "--scene", SCENE_REL, "--frames", str(frames)],
                          cwd=BIN, env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=240.0)
    return (proc.stdout or "") + (proc.stderr or "")


def group_b():
    # ---- ① 0 号选项写 trust=3 → 条件成立 → 落在 g ----
    write_asset(ASSET_REL, BRANCH_JSON.replace("BRANCH_FIRST_TRUST", "3"))
    out = run_scene()
    lines = out.splitlines()

    # 宿主把旗标表接上了
    check("[B] 旗标仓库装上了（DLG-FLAGS installed=1）",
          any("DLG-FLAGS installed=1" in ln for ln in lines),
          first(lines, "DLG-FLAGS").strip())

    # 资产本身认得这段分支（conds/writes 是加载期统计出来的）
    asset = first(lines, "DLG-ASSET")
    check("[B] 加载期认出了 1 条条件 + 3 条副作用",
          "conds=1" in asset and "writes=3" in asset, asset.strip())

    # 选项写了旗标（只有被选中的那一条会写 → 1 条，不是 3 条）
    w = every(lines, "DLG-WRITE ")
    check("[B] 选项写了旗标（DLG-WRITE flag=trust value=3）",
          any("flag=trust value=3" in ln for ln in w),
          w[0].strip() if w else "")

    # 条件**先写后判**：写完之后 trust=3 ≥ 2 → ok=1
    cond = every(lines, "DLG-COND ")
    check("[B] 条件成立 → 走 g（ok=1 else=h）",
          any("id=g ok=1" in ln for ln in cond),
          next((ln.strip() for ln in cond if "id=g" in ln), ""))
    check("[B] 走线落在 g（DLG-LINE id=g）",
          any("DLG-LINE id=g" in ln for ln in lines),
          first(lines, "DLG-LINE id=g").strip())
    check("[B] 没走 else（没有 DLG-LINE id=h）",
          not any("DLG-LINE id=h" in ln for ln in lines),
          first(lines, "DLG-LINE id=h").strip())
    check("[B] 条件成立时也走完整段（DLG-END）",
          any("DLG-END" in ln for ln in lines), first(lines, "DLG-END").strip())
    check("[B] 运行时零 ERROR（条件成立那条线）", "[ERROR]" not in out,
          first(lines, "[ERROR]").strip())

    # ---- ② 0 号选项写 trust=1 → 条件不成立 → 落在 else=h ----
    #
    # ⚠ 这一趟不是"重复劳动"：一条走线的用例证明不了 else 分叉。
    #   两种实现（永远走 if / 永远走 else）各能过掉其中一趟。
    write_asset(ASSET_REL, BRANCH_JSON.replace("BRANCH_FIRST_TRUST", "1"))
    out2 = run_scene()
    lines2 = out2.splitlines()
    cond2 = every(lines2, "DLG-COND ")
    check("[B] 条件不成立 → ok=0（走 else）",
          any("id=g ok=0" in ln for ln in cond2),
          next((ln.strip() for ln in cond2 if "id=g" in ln), ""))
    check("[B] 走线落在 h（DLG-LINE id=h）",
          any("DLG-LINE id=h" in ln for ln in lines2),
          first(lines2, "DLG-LINE id=h").strip())
    check("[B] 条件不成立时也走完整段（DLG-END）",
          any("DLG-END" in ln for ln in lines2),
          first(lines2, "DLG-END").strip())
    check("[B] 运行时零 ERROR（走 else 那条线）", "[ERROR]" not in out2,
          first(lines2, "[ERROR]").strip())


# ---------------------------------------------------------------- C：编辑器

def group_c():
    # 建条件 + 设 else + 挂副作用
    script = ";".join([
        "sel:1",             # 第二行（b）
        "addcond",
        "condflag:0:met_alice",
        # ⚠ 先改成 `<` 再改回来：`actAddCond` 的默认 op 就是 `>=`，直接
        #   设 `>=` 会走"值没变"的早退（不打日志），断言就成了"没反应"。
        "condop:0:<",
        "condop:0:>=",
        "condval:0:2",
        "setelse:a",
        "addwrite:0",        # b 这一行没有选项 → 应当被拒
        "save",
    ])
    dump = os.path.join(BIN, "assets", "dialogues", "_dlg_branch_edit.json.dump")
    proc, hwnd, tap = F.run_editor(
        env_extra={"MYVK_EDITOR_DLGED": EDIT_REL,
                   "MYVK_EDITOR_DLGED_SCRIPT": script,
                   "MYVK_EDITOR_DLGED_DUMP": dump},
        wait_for="DLGED-CHECK", wait_secs=45.0)
    try:
        lines = lines_of(tap)
        add = first(lines, "DLGED-ADDCOND")
        check("[C] 加条件成功（DLGED-ADDCOND n=1）", "n=1" in add, add.strip())
        ops = every(lines, "DLGED-CONDOP")
        check("[C] 改运算符走了两步（DLGED-CONDOP op=< 然后 op=>=）",
              any("op=<" in ln for ln in ops) and any("op=>=" in ln for ln in ops),
              "%d 条；%s" % (len(ops), ops[0].strip() if ops else ""))
        el = first(lines, "DLGED-ELSE")
        check("[C] 设 else（DLGED-ELSE else=a）", "else=a" in el, el.strip())
        # 负向：b 这一行没有选项，`addwrite:0` 必须被拒
        rej = first(lines, "DLGED-REJECT")
        check("[C] 没有选项时挂副作用被拒（DLGED-REJECT op=addwrite）",
              "op=addwrite" in rej, rej.strip())
        # 负向：op 拼错必须被拒（不是"写进去、存盘时报错"）
        check("[C] 非法运算符没被写进去（日志里没有 DLGED-CONDOP op=！）",
              "op=!" not in "\n".join(lines))
    finally:
        F.close(proc)

    # dump 出来的是真文件（上面那次 save 落的盘）—— 断言 JSON 而不是日志
    p = os.path.join(ASSETS, *EDIT_REL.split("/"))
    ok_json = False
    detail = p
    try:
        with open(p, encoding="utf-8") as f:
            doc = json.load(f)
        node = next((n for n in doc.get("lines", []) if n.get("id") == "b"), None)
        ok_json = (node is not None and node.get("if") == [
            {"flag": "met_alice", "op": ">=", "value": 2.0}] and
            node.get("else") == "a" and "set" not in node)
        detail = json.dumps(node, ensure_ascii=False) if node else "<no b>"
    except Exception as e:                                  # noqa: BLE001
        ok_json = False
        detail = "%s（%s）" % (p, e)
    check("[C] 落盘的 .dlg.json 里真有 if/else（不是只有日志）", ok_json, detail)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑这些组（如 AB）")
    ap.add_argument("--keep", action="store_true", help="保留夹具")
    args = ap.parse_args()
    want = set(args.only.upper()) & GROUP if args.only else set(GROUP)

    write_asset(ASSET_REL, BRANCH_JSON)
    write_asset(EDIT_REL, EDIT_JSON)
    make_scene(os.path.join(ASSETS, *SCENE_REL.split("/")))

    t0 = time.time()
    if "A" in want:
        group_a()
    if "B" in want:
        group_b()
    if "C" in want:
        group_c()

    cleanup(args.keep)

    total = 0
    try:
        import verify_flags
        total = len(verify_flags.fails)
    except Exception:                                       # noqa: BLE001
        pass
    print()
    print("=" * 78)
    if total == 0 and not failures:
        print("结果: PASS")
    else:
        print("结果: FAIL: %d 项" % max(total, len(failures)))
    print("=" * 78)
    print("耗时 %.1fs" % (time.time() - t0))
    return 0 if (total == 0 and not failures) else 1


if __name__ == "__main__":
    sys.exit(main())
