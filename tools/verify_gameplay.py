#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P2 玩法状态层验收（A~F）。

A  内核自检（`--selftest gameplay`，G~L 六组 111 条 + P3-3 的 M 组 80 条 = 191）
B  面板锚点 + 六条只读命令（state / order / advance / seed / roundtrip / rebuild）
C  Inspector ▸ Gameplay 页签（属性 / 状态 / 伤害预览的锚点与数值）
D  伤害明细五步（`gameplay.hit` → GP-DAMAGE + 5 条 GP-DAMAGE-STEP）
E  存档往返（场景里有 `gameplay` 块 → 读进来 → 再 `scene.save` 写回去）
F  脚本动词（`getAttr` / `addStatus` / `damage` / `tickRound` 在 Play 里真的接通）

⚠ 判据口径（为什么这么选）：
   · 属性值一律断言**具体数字**。只断言"页签在"的话，`buildAttributes`
     少挂一条状态修正器也照样绿 —— 而那正是这套东西最容易错的地方。
   · 伤害**读明细**（五步）而不是读最终血量：只读血量的话"护甲没生效"
     和"易伤没生效"可以给出完全一样的数（这是 Stats.h 文件头第 4 条）。
   · 存档往返必须**两边都断言**：读端（`SCENE-GAMEPLAY ... ok=1`）与
     写端（存回去的文件里真的有 `attrs`/`statuses`）。只验一边的话，
     "读得出来但存回去丢了"完全测不到。
   · F 组读**脚本自己的 print**：动词的返回值只有脚本看得到，用日志转述
     会把"宿主没接"误判成"接了但没打日志"。

用法：
    python tools/verify_gameplay.py            # 全量 A~F
    python tools/verify_gameplay.py --only AB  # 只跑 A、B
"""

import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_flags as F          # 复用 run_editor / snap / check / 夹具路径

BIN = F.BIN
ASSETS = F.ASSETS
SRC_SCENE = F.SRC_SCENE

GP_PROBE_SCENE_REL = "scenes/_gp_probe.json"
GP_PROBE_VKS_REL = "scripts/_gp_probe.vks"


def check(name, ok, detail=""):
    F.check(name, ok, detail)


def lines_of(tap):
    return F.snap(tap)


# ---------------------------------------------------------------- 夹具

# 一个"数值齐全"的单位。**属性与状态都写满**：
#   hp 100/100、attack 100、armor 100、shield 0、vulnerable 0
#   + 一层中毒（3 回合、-3 攻击）
# 于是"读档之后的值"每一个都能对上具体数字：
#   attack = 100 - 3 = 97     armor=100 → 物理减伤 50%
GP_ENTITY = {
    "name": "GpProbe",
    "transform": {"position": [0.0, 0.0, 0.0],
                  "rotation": [0.0, 0.0, 0.0],
                  "scale": [1.0, 1.0, 1.0]},
    "gameplay": {
        "speed": 150.0,
        "gauge": 250.0,
        "team": 1,
        "attrs": [
            {"n": "hp", "b": 100.0, "min": 0.0, "max": 100.0},
            {"n": "attack", "b": 100.0, "min": 0.0, "max": 1000000.0},
            {"n": "armor", "b": 100.0, "min": 0.0, "max": 1000000.0},
            {"n": "shield", "b": 0.0, "min": 0.0, "max": 1000000.0},
            {"n": "vulnerable", "b": 0.0, "min": -1000.0, "max": 1000.0},
        ],
        "statuses": [
            # ⚠ `dot` 三件套（dot / dotAttr / dotEl）**必须写**：只写 `mods`
            #   的话 `dotPerTurn` 是 0，`tickTurn()` 一条 dot 都不产 ——
            #   症状是"回合末结算"那一组全绿（dots=0 也是合法输出），
            #   而 F 组脚本的 `tickRound()` 会返回 0。这条是踩过的。
            {"id": "poison", "name": "Poison", "turns": 3, "stacks": 1,
             "max": 5, "rule": "stack",
             "dot": 5.0, "dotAttr": "hp", "dotEl": "poison",
             "mods": [{"a": "attack", "op": "flat", "v": -3.0}]},
        ],
    },
}

# 第二个单位：只为了"队列里不止一个人"，并且速度不同（100 vs 150 → 150 先动）。
GP_ENTITY_B = {
    "name": "GpSlow",
    "transform": {"position": [2.0, 0.0, 0.0],
                  "rotation": [0.0, 0.0, 0.0],
                  "scale": [1.0, 1.0, 1.0]},
    "gameplay": {
        "speed": 100.0,
        "gauge": 0.0,
        "team": 0,
        "attrs": [
            {"n": "hp", "b": 50.0, "min": 0.0, "max": 50.0},
            {"n": "attack", "b": 10.0, "min": 0.0, "max": 1000000.0},
        ],
    },
}

GP_PROBE_SRC = """\
# _gp_probe.vks —— P2 验收夹具（**不是资产**，跑完由 verify_gameplay.py 删掉）
#
# 只验一件事：四个玩法动词在 Play 里真的接通了，且返回值可信。
# 每一行 print 都是一个独立的判据（行号就是断言顺序）。
on start {
    # ① 属性：夹具里 attack = 100 - 3(毒) = 97
    print(getAttr("attack"))
    # ② 加一层毒 → 总层数 2
    print(addStatus("poison"))
    # ③ 加完之后 attack = 100 - 6 = 94
    print(getAttr("attack"))
    # ④ 打一下：armor=100 → 减伤 50%，所以 100 点只掉 50
    print(damage(100, "physical"))
    # ⑤ 回合末结算：一条毒 → 返回 1
    print(tickRound())
    # ⑥ 打完之后 hp = 100 - 50 - dot
    print(getAttr("hp"))
}
"""


def write_probe():
    """夹具：两个带 `gameplay` 块的实体。

    ⚠ 外壳字段（transform / camera / light / physics）从 `script_demo.json`
    派生：手写容易少一个字段，而少字段的后果是**静默丢掉这个实体**
    （表现成"一个单位都没有"）。实体内容则是我们自己的。
    """
    os.makedirs(os.path.dirname(F.PROBE_VKS), exist_ok=True)
    with open(os.path.join(ASSETS, GP_PROBE_VKS_REL), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(GP_PROBE_SRC)

    with open(SRC_SCENE, "r", encoding="utf-8") as f:
        demo = json.load(f)

    a = json.loads(json.dumps(GP_ENTITY))
    b = json.loads(json.dumps(GP_ENTITY_B))
    scene = {
        "version": demo.get("version", 1),
        "generator": "verify_gameplay.py",
        "camera": demo.get("camera", {}),
        "light": demo.get("light", {}),
        "physics": demo.get("physics", {}),
        "entities": [a, b],
    }
    # ⚠ 落 **assets 根**（`--scene` 收的是相对 assets 的路径）。写到
    #   `build/bin/scenes/` 的话编辑器**不报错**、只开个空场景。
    out = os.path.join(ASSETS, GP_PROBE_SCENE_REL)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)


def write_script_probe():
    """F 组专用的夹具：把脚本挂到 GpProbe 上。"""
    with open(os.path.join(ASSETS, GP_PROBE_VKS_REL), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(GP_PROBE_SRC)
    path = os.path.join(ASSETS, GP_PROBE_SCENE_REL)
    with open(path, "r", encoding="utf-8") as f:
        scene = json.load(f)
    for e in scene.get("entities", []):
        if e.get("name") == "GpProbe":
            e["script"] = {"path": GP_PROBE_VKS_REL}
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)


def remove_probe():
    for p in (os.path.join(ASSETS, GP_PROBE_SCENE_REL),
              os.path.join(ASSETS, GP_PROBE_VKS_REL)):
        try:
            if os.path.isfile(p):
                os.remove(p)
        except Exception:                                      # noqa: BLE001
            pass


# ---------------------------------------------------------------- A

def group_a():
    print("\n[A] 内核自检（G~L 六组）")
    out = F.run_game_selftest("gameplay")

    # ⚠ 不能 `startswith`：这一行带 `[INFO ] ` 前缀（`VK_LOG_INFO` 的输出）。
    #   本仓踩过"日志前缀变了导致判据整组红"的坑，所以按**包含**匹配。
    m = [l for l in out.splitlines() if "GAMEPLAY-TOTAL " in l]
    check("[A] GAMEPLAY-TOTAL 出现", len(m) == 1, m[:1])
    if m:
        # 210 条，实测分布：G 属性 13 / H 状态 19 / I 伤害 18 / J 随机 12 /
        # K 回合 38 / L 持久化 11 / **M 牌堆 80**（P3-3）/
        # **N 配表 19**（P3-1，解析器纯函数，实现住在 `gameplay/CardTable.cpp`）。
        # M 与 N 的关键项由 `verify_cards.py` 的 A/F 组逐条点名，那边才是主人。
        # ⚠ 加断言时必须同步改这个数 —— 不然"少跑了一组"看不出来。
        # ⚠ 也别凭印象写分布（第一版写的 13/12/15/13/28/8 只有 89 条，
        #   而 `pass=111` 照样绿 —— 两个数字各错各的，互不校验）。
        #   数法：`Game.exe --selftest gameplay | grep GAMEPLAY-TEST |
        #         sed -E 's/.*GAMEPLAY-TEST ([A-Z])[0-9].*/\1/' | sort | uniq -c`
        check("[A] 全通过（210 条）", "pass=210 fail=0" in m[0], m[0])
        check("[A] 失败数为 0", "fail=0" in m[0], m[0])

    # 反向判据：各组各自的行数必须都出现过（漏接一组会少一整块）。
    # ⚠ M 组属于 `verify_cards.py`（它才是牌堆的主人），这里只要求前六组。
    for g in ("G1.", "H1.", "I1.", "J1.", "K1.", "L1."):
        n = sum(1 for l in out.splitlines() if "GAMEPLAY-TEST " + g in l)
        check("[A] %s* 组有断言" % g[:2], n > 0, "n=%d" % n)

    # 这两条是"已经踩过的坑"的回归钉：满血 consume 必须扣得动
    # （探斜率写法在满血时斜率=0，症状是"打人完全没伤害"）。
    check("[A] L1 满血扣减（consume 的反向探针）",
          any("L1.consume-taken" in l and "result=PASS" in l
              for l in out.splitlines()),
          [l for l in out.splitlines() if "L1.consume-taken" in l][:1])
    # 状态快照往返（走 restore 而不是 add）
    check("[A] L2 快照往返（restore 不重算层数）",
          any("L2.restore-stacks" in l and "result=PASS" in l
              for l in out.splitlines()),
          [l for l in out.splitlines() if "L2.restore-stacks" in l][:1])


# ---------------------------------------------------------------- B

def group_b():
    print("\n[B] 面板锚点 + 六条只读命令")
    write_probe()
    cmd = ("gameplay.state; gameplay.order 4; gameplay.advance 3.0; "
           "gameplay.endturn; gameplay.rebuild; gameplay.seed 424242; "
           "gameplay.roundtrip; gameplay.tick")
    proc, hwnd, tap = F.run_editor(panel="gameplay", scene=GP_PROBE_SCENE_REL,
                                   console_cmd=cmd,
                                   wait_tag="GP-RECT",
                                   wait_for="GP-TICK-CMD",
                                   wait_secs=70.0)
    check("[B] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)
    joined = "\n".join(lines)

    for tag in ("GP-RECT", "GP-STATE", "GP-ORDER", "GP-ADVANCE", "GP-TICK",
                "GP-ROUNDTRIP", "GP-SEEDAPPLY", "GP-DT"):
        check("[B] 锚点 %s 有矩形" % tag, tag + "=" in joined)

    # 两个单位都要在队列里（夹具写了两个）。
    check("[B] 队列里两个单位", "units=2 queued=2" in joined,
          [l for l in lines if "GP-STATE" in l][:2])

    # 顺序条：150 速（带 250 初始槽）对 100 速。
    #   手算时刻 5.00 / 10.00 / 11.67 / 18.33 —— 名字序列是
    #   `GpProbe > GpSlow > GpProbe > GpProbe`。
    # ⚠ 第 3、4 位**都是** GpProbe 不是重复 bug：1.5 倍速本来就会在某些位置
    #   连出两次（比例 5:3）。判据钉的是"具体序列"而不是"交替"—— 真按交替
    #   去修，等于把 1.5 倍速抹平成 1:1。
    orders = [l for l in lines if "GP-ORDER order=" in l]
    check("[B] 顺序条行数 > 0", len(orders) >= 1, orders[:2])
    if orders:
        body = orders[0].split("order=", 1)[1]
        seq = re.findall(r"([^ >]+)\(([\d.]+)\)", body)
        names = [s[0] for s in seq]
        times = [float(s[1]) for s in seq]
        check("[B] 顺序条四位", len(seq) == 4, seq)
        check("[B] 名字序列 = P,S,P,P（1.5 倍速的正确 CTB）",
              names == ["GpProbe", "GpSlow", "GpProbe", "GpProbe"], names)
        # 时刻必须递增：同一人多次出现时，若印的是"原始队列剩余量"，
        # 会得到 5.00 / 10.00 / 5.00 / 5.00（时间倒流）。
        check("[B] 时刻单调递增", all(times[i] <= times[i + 1]
                                      for i in range(len(times) - 1)), times)
        check("[B] 时刻数值对得上", len(times) == 4 and
              abs(times[0] - 5.0) < 0.02 and abs(times[1] - 10.0) < 0.02 and
              abs(times[2] - 11.67) < 0.02 and abs(times[3] - 18.33) < 0.02,
              times)

    # 重建之后队列还是 2 个（重建走的是组件里那份 speed/gauge）
    check("[B] rebuild 之后仍是 2 个", "GP-REBUILD" in joined and
          any("n=2" in l for l in lines if "GP-QUEUE" in l),
          [l for l in lines if "GP-QUEUE" in l][-2:])

    # 种子真的换了（0 会被拒，424242 应该生效）
    check("[B] 种子生效", "GP-SEED seed=424242" in joined,
          [l for l in lines if "GP-SEED" in l][:2])

    # 队列存档往返：ok=1 且 same=1（顺序完全一致）
    check("[B] 队列往返 ok=1 same=1",
          "GP-ROUNDTRIP roundtrip ok=1 same=1" in joined,
          [l for l in lines if "GP-ROUNDTRIP" in l][:2])

    # 回合末：夹具里有一条毒 → dots=1
    check("[B] 回合末结算了一条 dot", "GP-TICK-CMD tick units=2 dots=1" in joined,
          [l for l in lines if "GP-TICK-CMD" in l][:2])

    remove_probe()


# ---------------------------------------------------------------- C

def group_c():
    print("\n[C] Inspector ▸ Gameplay 页签（数值要对得上）")
    write_probe()
    # ⚠ **不能在这里打伤害**：console 命令是在同一个 tick 里跑完的，`hit`
    #   会先把 hp 扣到 50，等 Gameplay 页签第一帧绘制时 `INS-GP-STATS` 就
    #   直接印 50 —— 于是"hp=100/100（读进来的白值没被算错）"这条判据
    #   永远测不到。伤害是 D 组的事，C 组只验读数。
    cmd = "entity.select GpProbe; gameplay.state"
    proc, hwnd, tap = F.run_editor(
        panel="inspector", scene=GP_PROBE_SCENE_REL, console_cmd=cmd,
        wait_tag="INS-GP-HEADER", wait_for="INS-GP-DMGPREV",
        wait_secs=70.0,
        env_extra={"MYVK_EDITOR_INSP_TAB": "gameplay"})
    check("[C] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)
    joined = "\n".join(lines)

    for tag in ("INS-GP-HEADER", "INS-GP-STATS", "INS-GP-ATTRS",
                "INS-GP-STATUSES", "INS-GP-PRESET-poison",
                "INS-GP-PRESET-shield", "INS-GP-PRESET-vuln",
                "INS-GP-DMGPREV"):
        check("[C] 锚点 %s 有矩形" % tag, tag + "=" in joined)

    # ⚠ 数值判据：attack = 100 - 3（毒 1 层）= 97。少挂一条状态修正器
    #   会得到 100，多挂一条会得到 94 —— 三个数互不相同，所以钉得住。
    stats = [l for l in lines if "INS-GP-STATS entity=" in l]
    check("[C] 数值行打了", len(stats) >= 1, stats[:2])
    if stats:
        check("[C] hp=100/100", "hp=100.0/100" in stats[0], stats[0])
        check("[C] attack=97（毒挂上了）", "atk=97.0" in stats[0], stats[0])
        check("[C] armor=100", "armor=100.0" in stats[0], stats[0])

    # 伤害预览：100 点物理 vs armor 100 → 减伤 50% → 50。
    # 五步明细必须都在（步数固定，脚本才能按下标读）。
    prev = [l for l in lines if "INS-GP-DMGPREV entity=" in l]
    check("[C] 预览行打了", len(prev) >= 1, prev[:2])
    if prev:
        check("[C] 预览是五步", "steps=5" in prev[0], prev[0])
        check("[C] 预览进公式 100", "-> armor 50.0" in prev[0], prev[0])

    # 选中生效（否则上面那些全是"某个实体"的数，不是 GpProbe 的）
    check("[C] entity.select 命中了唯一一个",
          "ENTITY-SELECT name=GpProbe" in joined and "candidates=1" in joined,
          [l for l in lines if "ENTITY-SELECT" in l][:2])

    remove_probe()


# ---------------------------------------------------------------- D

def group_d():
    print("\n[D] 伤害五步明细（只读最终血量测不出护甲有没有生效）")
    write_probe()
    cmd = "entity.select GpProbe; gameplay.hit 100 physical"
    proc, hwnd, tap = F.run_editor(
        panel="gameplay", scene=GP_PROBE_SCENE_REL, console_cmd=cmd,
        wait_tag="GP-RECT", wait_for="GP-HIT",
        wait_secs=70.0)
    check("[D] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)
    joined = "\n".join(lines)

    dmg = [l for l in lines if "GP-DAMAGE " in l]
    steps = [l for l in lines if "GP-DAMAGE-STEP " in l]
    check("[D] 伤害行打了", len(dmg) == 1, dmg[:2])
    check("[D] 明细恰好五步", len(steps) == 5, "got=%d" % len(steps))

    # 五步的 label 顺序固定：crit → armor/抗性 → vuln → shield → hp
    if len(steps) == 5:
        check("[D] 第 1 步是暴击判定", "no-crit" in steps[0], steps[0])
        check("[D] 第 2 步是护甲（减伤 50%）", "red=50.0%" in steps[1], steps[1])
        check("[D] 第 3 步是易伤（0%）", "vuln=+0.0%" in steps[2], steps[2])
        check("[D] 第 4 步是护盾（吸收 0）", "absorbed 0.0" in steps[3], steps[3])
        check("[D] 第 5 步是扣血（100→50）", "hp 100.0 -> 50.0" in steps[4], steps[4])

    # 最终 dealt 必须是 50（100 点被 armor 挡了一半）。
    # ⚠ 这一条与上面第 5 步是**两条不同**的判据：一个读明细、一个读汇总，
    #   汇总那条能抓住"明细对了但汇总算错"（确实是两个独立的量）。
    check("[D] dealt=50", "armor 50.0 -> vuln 50.0 -> shield 0.0 -> hp 50.0" in joined,
          dmg[:1])

    remove_probe()


# ---------------------------------------------------------------- E

def group_e():
    print("\n[E] 存档往返（读端 + 写端两边都要断言）")
    write_probe()
    path = os.path.join(ASSETS, GP_PROBE_SCENE_REL)
    # ⚠ 不带 `gameplay.tick`：夹具里加了 shield 之后，dot 会被护盾吸收，
    #   hp 就不是一条能只看护甲的直伤结果了 —— 存档往返不需要顺手测
    #   "护盾吃不吃 dot"（那是 H 组的事），混在一起判据会互相污染。
    cmd = "entity.select GpProbe; gameplay.hit 30; gameplay.addstatus shield; " \
          "scene.save"
    proc, hwnd, tap = F.run_editor(
        panel="gameplay", scene=GP_PROBE_SCENE_REL, console_cmd=cmd,
        wait_tag="GP-RECT", wait_for="saved:",
        wait_secs=70.0)
    check("[E] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)
    joined = "\n".join(lines)

    # 读端：场景里那个 gameplay 块被认出来了。
    # ⚠ 不能断言"恰好两行"：`scene.save` 之后编辑器会重建场景，日志会再打
    #   一遍（第二遍里 statuses 已经是 2 了）。所以要按 **entity 去重**数
    #   "有几个单位被认出来"，而不是数行数。
    loads = [l for l in lines if "SCENE-GAMEPLAY entity=" in l]
    by_entity = {}
    for l in loads:
        mm = re.search(r"entity=(\d+) attrs=(\d+) statuses=(\d+)", l)
        if mm:
            by_entity.setdefault(int(mm.group(1)), []).append(l)
    check("[E] 读端认出了两个单位", len(by_entity) == 2, sorted(by_entity.keys()))
    check("[E] 两个单位都 ok=1",
          bool(by_entity) and all(any("ok=1" in x for x in v)
                                 for v in by_entity.values()),
          [v[0] for v in by_entity.values()])
    check("[E] 读端 attrs/statuses 都读到了",
          any("attrs=5 statuses=1" in l and "ok=1" in l for l in loads),
          loads[:3])

    # 写端：存回去的文件里真的有 attrs / statuses（**不是**只写了 speed）
    with open(path, "r", encoding="utf-8") as f:
        saved = json.load(f)
    probe = None
    for e in saved.get("entities", []):
        if e.get("name") == "GpProbe":
            probe = e
            break
    check("[E] 存回去的文件里有 GpProbe", probe is not None, "")
    if probe is not None:
        gp = probe.get("gameplay") or {}
        check("[E] 写端有 gameplay 块", bool(gp), list(gp.keys()))
        attrs = {a.get("n"): a for a in gp.get("attrs", []) if isinstance(a, dict)}
        check("[E] 写端 5 条属性", len(attrs) == 5, list(attrs.keys()))
        # ⚠ 这一条是整组的关键：**打掉的血必须被存下来**。
        #   30 点物理打 armor=100 → 减伤 50% → 只掉 15 → hp = 100 - 15 = **85**。
        #   （上一版这里写的期望是 70，那是漏算护甲 —— 100-30 是"没有护甲"
        #    的结果，正好和"护甲没生效"这个 bug 的数值一样，不能用。）
        #   存成"原始白值"的话这里是 100；存成"算好的值"会把毒的修正也算
        #   进去 → 别的数。85 只有一个来源。
        hp = attrs.get("hp", {})
        check("[E] hp 存成 85（当前血量，且护甲真的生效了）",
              abs(float(hp.get("b", -1)) - 85.0) < 0.01, hp)
        # 上下限必须一起存：只存 base 会让读档之后闪避率变 350%
        check("[E] hp 的 max 也存了", abs(float(hp.get("max", -1)) - 100.0) < 0.01, hp)
        # 状态存**完整定义**（不只是 id）：调完平衡老存档不该变味
        sts = gp.get("statuses") or []
        ids = [s.get("id") for s in sts if isinstance(s, dict)]
        check("[E] 状态列表存下来了", len(sts) >= 1, ids)
        check("[E] 中毒带完整 mods",
              any(s.get("id") == "poison" and s.get("mods") for s in sts),
              ids)
        # gauge 也要存（不存的话读档后所有人行动条清零 → 敌人总是先手）
        check("[E] gauge 存了", "gauge" in gp, list(gp.keys()))

    remove_probe()


# ---------------------------------------------------------------- F

def group_f():
    print("\n[F] 脚本动词（Play 里四个动词真的接通）")
    write_probe()
    write_script_probe()
    # @150：脚本跑过 on start 之后再取状态，避免"命令先于脚本"。
    cmd = "@150 gameplay.state; @150 gameplay.order 2"
    # ⚠ `print` 默认只进编辑器面板（用户脚本可能每帧打印，落日志会淹掉），
    #   要落日志必须开 `MYVK_LOG_SCRIPT_PRINT=1`。而 `base_env()` 会**清掉
    #   所有 `MYVK_*`**，所以只能走 `env_extra` 传 —— 直接改 os.environ
    #   等于没设。踩过。
    proc, hwnd, tap = F.run_editor(
        panel="gameplay", scene=GP_PROBE_SCENE_REL, console_cmd=cmd,
        play=True, wait_tag="GP-RECT", wait_for="GP-ORDER",
        wait_secs=80.0,
        env_extra={"MYVK_LOG_SCRIPT_PRINT": "1"})
    check("[F] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)
    joined = "\n".join(lines)

    # 脚本 print 出来的数（见 GP_PROBE_SRC 的行号注释）
    # ⚠ 只从 `VKS-PRINT` 行取：屏幕/日志里还有别的数字（坐标、耗时），
    #   全日志扫数字会被它们碰巧满足。也**不能按子串包含**判值
    #   （`97` 会被 `197` 满足）。
    vals = []
    for l in lines:
        if "VKS-PRINT " not in l:
            continue
        t = l.rsplit(": ", 1)[-1].strip()
        if re.fullmatch(r"-?\d+(\.\d+)?", t):
            vals.append(t)
    check("[F] 脚本打了至少 6 个数", len(vals) >= 6, vals[:8])
    if len(vals) >= 6:
        # ① attack = 100 - 3（一层毒）
        check("[F] getAttr(attack)=97", vals[0].startswith("97"), vals[0])
        # ② addStatus 返回总层数 2
        check("[F] addStatus(poison)=2", vals[1].startswith("2"), vals[1])
        # ③ 两层毒 → 94
        check("[F] getAttr(attack)=94", vals[2].startswith("94"), vals[2])
        # ④ damage(100, physical)：armor 100 → 只掉 50
        check("[F] damage(100)=50（护甲挡了一半）", vals[3].startswith("50"), vals[3])
        # ⑤ tickRound：**一个状态实例结算一条 dot**，与层数无关。
        #    poison 是 1 个实例、stacks=2 → 返回 **1**（不是 2）。
        #    ⚠ 这条最容易写成 2（"两层毒 → 两条"）：写成 2 就等于把判据
        #      绑到"按层展开"的实现上，而层数是伤害的乘数、不是条数。
        check("[F] tickRound()=1（实例数，不是层数）", vals[4].startswith("1"), vals[4])
        # ⑥ hp：100 - 50(直伤，armor 减半) - 10(毒：5/层 × 2 层) = 40。
        #    ⚠ 这里 dot 的 element 是 poison，走 `resist_poison`（不存在 → 0%），
        #      所以**不吃**护甲减半 —— 与直伤那 50 不是同一条算法。
        check("[F] 最终 hp=40", vals[5].startswith("40"), vals[5])

    # 反向判据：不许出现 "not available"（宿主没接的真实症状）
    check("[F] 没有 not-available 报错",
          "not available" not in joined,
          [l for l in lines if "not available" in l][:2])

    remove_probe()


# ---------------------------------------------------------------- main

def main():
    only = None
    for a in sys.argv[1:]:
        if a.startswith("--only"):
            only = a.split("=", 1)[1] if "=" in a else sys.argv[sys.argv.index(a) + 1]

    groups = [("A", group_a), ("B", group_b), ("C", group_c),
              ("D", group_d), ("E", group_e), ("F", group_f)]
    for name, fn in groups:
        if only and name not in only:
            continue
        fn()

    total = len(F.fails)
    print("\n" + "=" * 70)
    if total == 0:
        print("结果: PASS")
    else:
        print("结果: FAIL: %d 条" % total)
        for n in F.fails:
            print("  - %s" % n)
    print("=" * 70)
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
