#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P3-3 牌堆（牌库 / 手牌 / 弃牌堆 / 消耗区 / 结算区）验收（A~F）。

A  内核自检 M 组（`--selftest gameplay` 里的 24 组 80 条：uid 身份 / 队首抽牌 /
   洗回计数 / 抽干 / 手牌上限不烧牌 / Limbo 语义 / 同区重排差一位 / 存档往返 /
   uid 不复用 / 定义表不随 clear 消失）
B  Cards 面板锚点 + 命令流程（ensure / deck / draw / play / resolve / state）
C  Inspector ▸ Gameplay 页签的 Cards 段（读端数值 + 手牌列表锚点）
D  存档往返（场景里有 `cards` 块 → 读进来 → `scene.save` 写回去，
   **含 Limbo 里的牌**）
E  手牌上限 / 弃手 / 往返一致性
F  P3-1 配表进运行时：`assets/data/cards.csv` → `CardDef` → 效果真的落到实体上
   （unknown-def 拒绝搬牌 / 出包闭包必须带 data/ / 表头 11 列）

⚠ 判据口径（为什么这么选）：
   · **`cards.play` 之后断言 `discard=0`**。这是整个 P3-3 最贵的一条：
     如果打出的牌直接进弃牌堆，那么在结算里读"弃牌堆数量"的效果会先看到
     它自己（"弃牌越多伤害越高"多算一张）。而只验"手上少了一张"的话，
     两种实现都是绿的。
   · **`got` 与 `draw` 都断言**：`got` 是"进了几张手牌"，`draw` 是"牌库
     还剩几张"。只验 `got` 的话，"手牌满了继续抽并烧掉溢出的牌"照样绿。
   · 存档往返**两边都断言**：读端 `SCENE-CARDS ... ok=1`，写端 JSON 里
     `piles.draw/hand/limbo` 的长度。只验一边的话，
     "读得出来但存回去把 Limbo 丢了"完全测不到。

用法：
    python tools/verify_cards.py            # 全量 A~F
    python tools/verify_cards.py --only AB  # 只跑 A、B
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_flags as F          # 复用 run_editor / snap / check / 夹具路径

ASSETS = F.ASSETS
SRC_SCENE = F.SRC_SCENE

CD_PROBE_SCENE_REL = "scenes/_cards_probe.json"


def check(name, ok, detail=""):
    F.check(name, ok, detail)


def lines_of(tap):
    return F.snap(tap)


# ---------------------------------------------------------------- 夹具

# 11 张牌的牌库（uid 1..11）+ 2 张手牌（uid 12/13）。
# 用途：读端能验"抽牌堆 11 / 手牌 2"，而 `cards.deck` 会把它清成 5/4/1/1。
def _card(uid, def_id):
    return {"u": uid, "d": def_id}


CD_ENTITY = {
    "name": "CardHolder",
    "transform": {"position": [0.0, 0.0, 0.0],
                  "rotation": [0.0, 0.0, 0.0],
                  "scale": [1.0, 1.0, 1.0]},
    "gameplay": {
        "attrs": [
            {"n": "hp", "b": 100.0, "min": 0.0, "max": 100.0},
        ],
    },
    "cards": {
        "label": "Probe deck",
        "handLimit": 10,
        "nextUid": 14,
        # 洗回次数与出手次数是**可统计的玩法数据**，必须能存回来。
        "reshuffles": 2,
        "plays": 7,
        "piles": {
            "draw": [
                _card(1, "strike"), _card(2, "strike"), _card(3, "strike"),
                _card(4, "strike"), _card(5, "strike"),
                _card(6, "defend"), _card(7, "defend"),
                _card(8, "defend"), _card(9, "defend"),
                _card(10, "bash"), _card(11, "slimed"),
            ],
            "hand": [_card(12, "strike"), _card(13, "defend")],
        },
    },
}


CD_TARGET = {
    "name": "CardTargetDummy",
    "transform": {"position": [3.0, 0.0, 0.0],
                  "rotation": [0.0, 0.0, 0.0],
                  "scale": [1.0, 1.0, 1.0]},
    "gameplay": {
        "attrs": [
            {"n": "hp", "b": 100.0, "min": 0.0, "max": 100.0},
        ],
    },
}


def write_probe():
    with open(SRC_SCENE, "r", encoding="utf-8") as f:
        demo = json.load(f)

    scene = {
        "version": demo.get("version", 1),
        "generator": "verify_cards.py",
        "camera": demo.get("camera", {}),
        "light": demo.get("light", {}),
        "physics": demo.get("physics", {}),
        # ⚠ 第二个实体是给 P3-1 的 F 组用的：`target=enemy` 的牌（打击/重击）
        #   需要一个**别人**当目标。场上只有持牌者自己时，Enemy 那条目标解析
        #   会落空 → 效果被算成 missed → "这张牌打出去一点伤害都没有"，
        #   而那看起来像伤害公式坏了，不像目标选错了。
        "entities": [json.loads(json.dumps(CD_ENTITY)),
                     json.loads(json.dumps(CD_TARGET))],
    }
    # ⚠ 落 **assets 根**（`--scene` 收的是相对 assets 的路径）。写到
    #   `build/bin/scenes/` 的话编辑器**不报错**、只开个空场景。
    out = os.path.join(ASSETS, CD_PROBE_SCENE_REL)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)


def remove_probe():
    p = os.path.join(ASSETS, CD_PROBE_SCENE_REL)
    try:
        if os.path.isfile(p):
            os.remove(p)
    except Exception:                                          # noqa: BLE001
        pass


# ---------------------------------------------------------------- A

def group_a():
    print("\n[A] 内核自检 M 组（牌堆 24 组 80 条）")
    out = F.run_game_selftest("gameplay")

    # ⚠ 按**包含**匹配：这一行带 `[INFO ] ` 前缀，`startswith` 会对不上
    #   （P2 的 A 组就是在这里红过一次）。
    m = [l for l in out.splitlines() if "GAMEPLAY-TOTAL " in l]
    check("[A] GAMEPLAY-TOTAL 出现", len(m) == 1, m[:1])
    if m:
        # 243 条 = P2 的 111 + P3-3 的 M 组 80 + P3-1 的 N 组 19 + P3-4 的 O/P 组 33。
        # 实测分布：G 13 / H 19 / I 18 / J 12 / K 38 / L 11 / M 80 / N 19 /
        #           **O 26**（伤害数字层）/ **P 7**（卡牌表现模板）。
        # ⚠ 加断言时必须同步改这个数 —— 不然"少跑了一组"看不出来。
        check("[A] 全通过（243 条）", "pass=243 fail=0" in m[0], m[0])
        check("[A] 失败数为 0", "fail=0" in m[0], m[0])

    # M 组的关键几条单独点出来：整组红的时候能立刻知道是哪一类。
    for key, why in (
        ("M1.distinct-uids", "身份用 uid 不是下标"),
        ("M3.reshuffled", "牌库空 → 弃牌洗回"),
        ("M5.left-in-draw", "手牌满不烧牌"),
        ("M6.discard-not-yet", "打出的牌不进弃牌堆（Limbo）"),
        ("M8.exhaust", "消耗卡进消耗区"),
        ("M9.play-rejected", "诅咒打不出去"),
        ("M10.at3", "同区重排不差一位"),
        ("M14.no-collision", "读档重算 nextUid"),
        ("M17.permutation", "洗牌只改顺序不改集合"),
        ("M18.defs-kept", "clear 不清定义表"),
        ("M24.no-reuse", "uid 永不复用"),
    ):
        hit = [l for l in out.splitlines() if key + " " in l]
        check("[A] %s（%s）" % (key, why),
              len(hit) == 1 and "result=PASS" in hit[0], hit[:1])

    # P3-4 的 O/P 组点名。挑的是"错了会静默"的四条：合并 / 确定性 /
    # 背后丢弃 / 牌堆动作不出粒子。
    for key, why in (
        ("O1.merge-one", "同点同类合并成一条"),
        ("O7.text-damage", "合并后的文本是 `37 x3`"),
        ("O8.drift-deterministic", "漂移确定（像素判据不 flaky）"),
        ("O9.behind-dropped", "相机背后的点被丢弃（不贴边）"),
        ("P2.pile-moves-silent", "抽/弃/消耗不在世界里出东西"),
        ("P3.crit-style", "暴击换了飘字样式"),
    ):
        hit = [l for l in out.splitlines() if key + " " in l]
        check("[A] %s（%s）" % (key, why),
              len(hit) == 1 and "result=PASS" in hit[0], hit[:1])

    # P3-1 的 N 组（配表解析）同样点名。⚠ 这几条挑的是"错了会静默"的：
    #   位置语义（N3b）/ 往返（N10）/ 缺列不半份（N14）/ 坏效果整行跳过（N16）。
    for key, why in (
        ("N3.effect-empty-fields", "空 target 的占位写法"),
        ("N3b.colon-shift-trap", "多一个冒号会把数字挪到 type 上"),
        ("N10.effects-roundtrip", "效果串 格式→解析 逐字段相等"),
        ("N14.table-no-id", "缺 id 列时绝不返回半份数据"),
        ("N16.table-bad-fx", "效果写错 → 整行跳过（不是空效果）"),
        ("N17.table-space-id", "id 带空格被拒绝"),
    ):
        hit = [l for l in out.splitlines() if key + " " in l]
        check("[A] %s（%s）" % (key, why),
              len(hit) == 1 and "result=PASS" in hit[0], hit[:1])


# ---------------------------------------------------------------- B

def group_b():
    print("\n[B] Cards 面板 + 命令流程")
    write_probe()
    # 一条命令串把整条流程走完。**顺序就是语义**：
    #   state（读端） → deck（清+5/4/1/1） → draw 5 → play 0 → resolve → state
    cmd = ("entity.select CardHolder; "
           "cards.state; cards.detail; "
           "cards.deck; cards.draw 5; "
           "cards.play 0; cards.resolve; "
           "cards.state")
    proc, hwnd, tap = F.run_editor(
        panel="cards", scene=CD_PROBE_SCENE_REL, console_cmd=cmd,
        wait_tag="CP-RECT", wait_for="GP-CARDS-CMD name=state",
        wait_secs=70.0)
    check("[B] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)
    remove_probe()

    states = [l for l in lines if "GP-CARDS-CMD name=state" in l]
    decks = [l for l in lines if "GP-CARDS-CMD name=deck" in l]
    draws = [l for l in lines if "GP-CARDS-CMD name=draw" in l]
    plays = [l for l in lines if "GP-CARDS-CMD name=play" in l]
    resolves = [l for l in lines if "GP-CARDS-CMD name=resolve" in l]
    details = [l for l in lines if "CARDS-PILE entity=" in l]

    check("[B] state 打了两条（读端 + 流程后）", len(states) >= 2, states[:3])
    if states:
        # ① 读端：夹具里的 11 + 2 张被完整读进来。
        first = states[0]
        check("[B] 读端 draw=11 hand=2",
              "draw=11" in first and "hand=2" in first, first)
        check("[B] 读端 reshuffle=2（洗回次数是存档数据）",
              "reshuffle=2" in first, first)
        check("[B] 读端 plays=7（出手次数是存档数据）",
              "plays=7" in first, first)
        check("[B] 读端 uid=14（nextUid 被读回来）", "uid=14" in first, first)

    check("[B] 逐张列表有 13 条（11 + 2）", len(details) == 13, details[:3])
    if details:
        # ⚠ **这条在 P3-1 之后反过来了**（记账过时，不是实现坏了）：
        #   老期望是"读进来那一刻还没载入配表 → known=0"，因为那时定义表
        #   只由 `cards.defs` 手动灌。P3-1 让 `EditorApp::gp()` 在**换场景**
        #   时就自动 `loadCardDefs()` + `setCardDefs()`，于是第一次 `cards.state`
        #   读到的已经是 known=1。
        #   现在这条验的是**顺序**：灌定义必须发生在"读牌堆明细"之前 ——
        #   反过来的话（先读后灌）这里仍然是 known=0，而 F 组看不出来
        #   （F 组是手动 `cards.defs`，绕过了自动装载那条路）。
        check("[B] 读进来就是 known=1（P3-1：定义表在首读之前已灌好）",
              all("known=1" in l for l in details), details[:2])

    check("[B] deck 清成 11 张", len(decks) == 1 and "draw=11" in decks[0],
          decks[:1])
    check("[B] draw 抽到 5 张", len(draws) == 1 and "got=5" in draws[0],
          draws[:1])
    if draws:
        # 牌库 11 - 5 = 6，手牌 5。
        check("[B] 抽完之后 draw=6 hand=5",
              "draw=6" in draws[0] and "hand=5" in draws[0], draws[0])
        check("[B] 没触发洗回（牌库还够）", "reshuffled=0" in draws[0], draws[0])

    # ⚠ 整组最关键的一条。
    check("[B] play 之后进 Limbo 且**没进弃牌堆**",
          len(plays) == 1 and "ok=1" in plays[0] and "limbo=1" in plays[0]
          and "discard=0" in plays[0], plays[:1])
    check("[B] resolve 之后才进弃牌堆",
          len(resolves) == 1 and "ok=1" in resolves[0] and "discard=1" in resolves[0]
          and "limbo=0" in resolves[0], resolves[:1])

    if len(states) >= 2:
        last = states[-1]
        # ⚠ 字段名以 `GameplayRuntime::cardLine()` 为准：是 `discard=` 不是
        #   `disc=`（`CardZones::dump()` 才用 `disc=`，两者形状不同）。
        check("[B] 最终 draw=6 hand=4 discard=1",
              "draw=6" in last and "hand=4" in last and "discard=1" in last, last)


# ---------------------------------------------------------------- C

def group_c():
    print("\n[C] Inspector ▸ Gameplay 页签的 Cards 段")
    write_probe()
    # ⚠ 不先 play/draw：console 命令在**同一个 tick** 跑完，页签第一帧绘制时
    #   数值已经变过 —— 那时"读进来的 draw=11 hand=2"这条判据就永远测不到
    #   （P2 的 C 组踩过一模一样的坑）。
    cmd = "entity.select CardHolder; cards.state"
    proc, hwnd, tap = F.run_editor(
        panel="inspector", scene=CD_PROBE_SCENE_REL, console_cmd=cmd,
        wait_tag="INS-CARD-STATS", wait_for="INS-CARD-STATS",
        wait_secs=70.0,
        env_extra={"MYVK_EDITOR_INSP_TAB": "gameplay"})
    check("[C] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)
    remove_probe()

    stats = [l for l in lines if "INS-CARD-STATS entity=" in l]
    check("[C] 牌堆读数行出现了", len(stats) >= 1, stats[:2])
    if stats:
        s = stats[0]
        check("[C] 读端 draw=11 hand=2 discard=0",
              "draw=11" in s and "hand=2" in s and "discard=0" in s, s)
        check("[C] limbo=0（夹具里没有结算中的牌）", "limbo=0" in s, s)
        check("[C] uid=14（nextUid 读对了）", "uid=14" in s, s)

    # 手牌列表的锚点：`INS-CARD-HAND` 用 logRect 记，只要有手牌就会有一条。
    hand_rects = [l for l in lines if "RECT INS-CARD-HAND" in l or "INS-CARD-HAND" in l]
    check("[C] 手牌列表锚点存在", len(hand_rects) >= 1, hand_rects[:2])


# ---------------------------------------------------------------- D

def group_d():
    print("\n[D] 存档往返（含 Limbo 里的牌）")
    write_probe()
    path = os.path.join(ASSETS, CD_PROBE_SCENE_REL)
    # play 一张留它在 Limbo，然后立刻存盘 —— 存进去的必须有 limbo 区。
    cmd = ("entity.select CardHolder; "
           "cards.deck; cards.draw 3; cards.play 0; scene.save")
    proc, hwnd, tap = F.run_editor(
        panel="cards", scene=CD_PROBE_SCENE_REL, console_cmd=cmd,
        wait_tag="CP-RECT", wait_for="saved:",
        wait_secs=70.0)
    check("[D] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)

    loads = [l for l in lines if "SCENE-CARDS entity=" in l]
    check("[D] 读端认出了 cards 块", len(loads) >= 1, loads[:3])
    check("[D] 读端 ok=1 且读到 draw=11 hand=2",
          any("ok=1" in l and "draw=11" in l and "hand=2" in l for l in loads),
          loads[:3])

    # 写端：文件里真的有 `cards` 块，而且三个区都在。
    doc = None
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except Exception as e:                                     # noqa: BLE001
        check("[D] 能读回场景文件", False, str(e))
    if doc:
        ents = [e for e in doc.get("entities", []) if e.get("name") == "CardHolder"]
        check("[D] CardHolder 还在场景里", len(ents) == 1, len(ents))
        if ents:
            cards = ents[0].get("cards", {})
            piles = cards.get("piles", {})
            draw = piles.get("draw", [])
            hand = piles.get("hand", [])
            limbo = piles.get("limbo", [])
            discard = piles.get("discard", [])
            # deck 造 11 张 → 抽 3 → 打 1：draw 8 / hand 2 / limbo 1 / discard 0
            check("[D] 写端 draw 8 张", len(draw) == 8, len(draw))
            check("[D] 写端 hand 2 张", len(hand) == 2, len(hand))
            # ⚠ 这一条是 P3-3 存档的核心：结算中的牌**必须**能存回来，
            #   否则"打出一张牌的中途存档"会让那张牌凭空消失。
            check("[D] 写端 limbo 1 张（结算中的牌也存）", len(limbo) == 1, len(limbo))
            check("[D] 写端 discard 0 张（还没结算完）", len(discard) == 0, len(discard))
            check("[D] 写端 handLimit=10", cards.get("handLimit") == 10,
                  cards.get("handLimit"))
            # uid 必须原样存住：只用下标的话读档之后"第 0 张"是另一张牌。
            uids = [c.get("u") for c in draw]
            check("[D] 写端每张牌都有 uid", all(isinstance(u, int) and u > 0 for u in uids),
                  uids[:4])
            check("[D] 写端 nextUid 抬到 15（deck 重造后 1..11，抽走后仍是 12）",
                  cards.get("nextUid", 0) >= 12, cards.get("nextUid"))

    remove_probe()


# ---------------------------------------------------------------- E

def group_e():
    print("\n[E] 手牌上限 / 弃手 / 往返一致")
    write_probe()
    # deck = 11 张。抽 100 张：手牌上限 10 → got=10，牌库剩 1（**不烧牌**）。
    cmd = ("entity.select CardHolder; "
           "cards.deck; cards.draw 100; "
           "cards.discardhand; "
           "cards.draw 5; cards.roundtrip")
    proc, hwnd, tap = F.run_editor(
        panel="cards", scene=CD_PROBE_SCENE_REL, console_cmd=cmd,
        wait_tag="CP-RECT", wait_for="GP-CARDS-CMD name=roundtrip",
        wait_secs=70.0)
    check("[E] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)
    remove_probe()

    draws = [l for l in lines if "GP-CARDS-CMD name=draw" in l]
    disc = [l for l in lines if "GP-CARDS-CMD name=discardhand" in l]
    rts = [l for l in lines if "GP-CARDS-CMD name=roundtrip" in l]

    check("[E] draw 打了两条", len(draws) >= 2, draws[:3])
    if draws:
        # ⚠ 上限 10 → got=10（不是 11，也不是 100）。
        check("[E] 抽 100 只进 10 张（手牌上限）", "got=10" in draws[0], draws[0])
        # ⚠ 剩下 1 张**留在牌库**，没被"抽了就丢"烧掉。
        check("[E] 溢出的 1 张留在牌库（不烧牌）", "draw=1" in draws[0], draws[0])
        check("[E] 手牌正好 10 张", "hand=10" in draws[0], draws[0])
    if len(draws) >= 2:
        check("[E] 弃手之后能再抽 5 张", "got=5" in draws[1], draws[1])
        # ⚠ 这一条是**副产品判据**（原本只想验"弃手后还能抽"）：弃手时牌库
        #   只剩 1 张、弃牌堆 10 张，所以抽 5 张必然触发一次洗回。
        #   留着它是因为"洗回"在真实对局里每两三回合就发生一次，
        #   而它一旦坏掉的表现是"中盘之后抽牌效果全部哑火"，最难定位。
        check("[E] 牌库不够时自动洗回（reshuffled=1）",
              "reshuffled=1" in draws[1], draws[1])

    check("[E] 弃手牌弃了 10 张", len(disc) == 1 and "n=10" in disc[0], disc[:1])

    check("[E] 文本往返一致", len(rts) == 1 and "ok=1" in rts[0] and "same=1" in rts[0],
          rts[:1])


# ---------------------------------------------------------------- F

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARDS_CSV = os.path.join(ROOT, "assets", "data", "cards.csv")


def group_f():
    print("\n[F] 配表进运行时（P3-1：配表 → CardDef → 卡牌效果）")
    write_probe()
    # 顺序就是语义，每一步都**复位**再走（`cards.reset` 不清定义表）：
    #
    #   ① defstable  —— 配表 7 张进内存 + 灌进场景所有牌堆
    #   ② detail     —— 每张牌 `known=1`（"灌进去了"只有这里看得见）
    #   ③ quickdraw  —— 只在 CSV 里有的牌，`draw::2` 效果必须真的抽 2 张
    #   ④ bash       —— `dmg=8.00` + `exhaust=1`（数值与消耗旗标都来自表）
    #   ⑤ slimed     —— `unplayable` 从表里读出来 → 打不出去
    #   ⑥ nosuchcard —— 定义查不到 → **不搬牌**，`reason=unknown-def`
    cmd = ("entity.select CardHolder; "
           "cards.defstable; cards.detail; "
           "cards.reset; cards.add quickdraw 1; cards.add strike 3; "
           "cards.draw 1; cards.cast 0; "
           "cards.reset; cards.add bash 1; cards.draw 1; cards.cast 0; "
           "cards.reset; cards.add slimed 1; cards.draw 1; cards.cast 0; "
           "cards.reset; cards.add nosuchcard 1; cards.draw 1; cards.cast 0; "
           "cards.state")
    proc, hwnd, tap = F.run_editor(
        panel="cards", scene=CD_PROBE_SCENE_REL, console_cmd=cmd,
        wait_tag="CP-RECT", wait_for="GP-CARDS-CMD name=state",
        wait_secs=80.0)
    check("[F] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        remove_probe()
        return

    lines = lines_of(tap)
    F.close(proc)
    remove_probe()

    tabs = [l for l in lines if "GP-CARDS-CMD name=defstable" in l]
    # ⚠ **必须排掉 `NOTIF-PUSH` 镜像行**：每条 WARN 都会被推成一条
    #   `NOTIF-PUSH ... text="GP-CARD-CAST ..."`，它包含同样的字样。
    #   不排的话"有 N 条日志"会变成 2N，而 `casts[i]` 会指向镜像行 ——
    #   症状是断言红了但读起来数字完全对得上。
    casts = [l for l in lines
             if "GP-CARD-CAST " in l and "NOTIF-PUSH" not in l]
    piles = [l for l in lines if "CARDS-PILE entity=" in l]
    states = [l for l in lines if "GP-CARDS-CMD name=state" in l]

    # ① 配表读到了 7 张（strike/defend/bash/slimed/cleave/quickdraw/focus）。
    check("[F] 配表被找到（found=1）", len(tabs) == 1 and "found=1" in tabs[0], tabs[:1])
    if tabs:
        check("[F] 配表 7 张定义", "defs=7" in tabs[0], tabs[0])
        check("[F] 配表没有坏行（skipped=0 errors=0）",
              "skipped=0" in tabs[0] and "errors=0" in tabs[0], tabs[0])
        # ⚠ `decks=1`：定义要灌进**牌堆**，不只是读进内存。只验 `defs=7`
        #   的话，"读了但没灌"照样绿 —— 而那种状态每张牌都是 `known=0`。
        check("[F] 定义灌进了 1 个牌堆（decks=1）", "decks=1" in tabs[0], tabs[0])

    # ② 灌进去之后每张牌都认得出来（`known=1`）。
    known0 = [l for l in piles if "known=0" in l]
    known1 = [l for l in piles if "known=1" in l]
    check("[F] 牌堆里的牌全都 known=1", len(known1) >= 13 and not known0,
          "known1=%d known0=%d" % (len(known1), len(known0)))

    check("[F] 四次 cast 都打了日志", len(casts) == 4, casts[:5])
    if len(casts) == 4:
        # ③ quickdraw：只在 CSV 里存在 → 走通"表 → 效果"整条链。
        #   `draw=2` 是**抽到的张数**（不是牌库剩余），牌库起始 3 张足够。
        check("[F] quickdraw 打出并抽了 2 张（draw=2）",
              "def=quickdraw" in casts[0] and "ok=1" in casts[0]
              and "draw=2" in casts[0], casts[0])
        # ④ bash：8 点伤害 + 消耗（两个值都在 CSV 里）。
        check("[F] bash 造成 8 点伤害（dmg=8.00）",
              "def=bash" in casts[1] and "dmg=8.00" in casts[1], casts[1])
        check("[F] bash 进了消耗区（exhaust=1，表里的旗标）",
              "exhaust=1" in casts[1], casts[1])
        # ⑤ slimed：`unplayable=1` 来自表 → 打不出去，**牌留在手上**。
        check("[F] slimed 打不出去（reason=unplayable）",
              "def=slimed" in casts[2] and "reason=unplayable" in casts[2],
              casts[2])
        check("[F] slimed 没离开手牌（hand=1 limbo=0）",
              "hand=1" in casts[2] and "limbo=0" in casts[2], casts[2])
        # ⑥ 定义查不到 **不搬牌**：搬了再静默丢弃 = "打出去什么都没发生"，
        #   而那与"配表没生效"完全一样（P3-1 最贵的一类 bug）。
        check("[F] 未知卡拒绝打出（reason=unknown-def）",
              "def=nosuchcard" in casts[3] and "reason=unknown-def" in casts[3],
              casts[3])
        check("[F] 未知卡没被搬进 Limbo（hand=1 limbo=0）",
              "hand=1" in casts[3] and "limbo=0" in casts[3], casts[3])

    # ⑦ 面板/命令层面的最终状态：四次复位后手牌里只剩那张未知卡。
    if states:
        check("[F] 收尾 state 里手牌 1 张", "hand=1" in states[-1], states[-1])

    # ---- ⑧ 出包必须带上配表（静态探针）----
    # 配表是"按**目录**扫盘"读的，没有任何 JSON 字符串指向 `.csv` →
    # 引用闭包必然把它当垃圾剪掉。剪掉的症状是"出包之后所有卡牌变未知卡"，
    # 而开发机上永远复现不了（`assets/data` 就在旁边）。
    try:
        with open(os.path.join(ROOT, "engine", "project", "ProjectBuild.cpp"),
                  "r", encoding="utf-8") as f:
            pb = f.read()
    except Exception as e:                                     # noqa: BLE001
        pb = ""
        check("[F] 能读 ProjectBuild.cpp", False, str(e))
    check("[F] 闭包 alwaysInclude 含 data/（防被当垃圾剪掉）",
          '"data/"' in pb, "" if '"data/"' in pb else "missing")
    check("[F] kEngineAssetDirs 含 assets/data",
          '"assets/data"' in pb, "" if '"assets/data"' in pb else "missing")

    # ---- ⑨ 配表本体：11 列 + 7 行（列名/字段缺一个就是静默少一项）----
    try:
        import csv as _csv
        with open(CARDS_CSV, "r", encoding="utf-8-sig", newline="") as f:
            rows = list(_csv.reader(f))
    except Exception as e:                                     # noqa: BLE001
        rows = []
        check("[F] 能读 cards.csv", False, str(e))
    if rows:
        check("[F] 表头 11 列", len(rows[0]) == 11, rows[0])
        want = ["id", "#note", "name", "kind", "target", "cost", "exhaust",
                "unplayable", "upgradedCost", "text", "effects"]
        check("[F] 表头就是 CardTable 认的那 11 个名字", rows[0] == want, rows[0])
        body = [r for r in rows[1:] if r and r[0].strip()]
        check("[F] 表体 7 张卡", len(body) == 7, len(body))
        ids = [r[0].strip() for r in body]
        check("[F] id 不重复", len(set(ids)) == len(ids), ids)


# ---------------------------------------------------------------- G

# P3-4 模板表里引用的 .veffect。**这份清单必须与 `engine/gameplay/CardFx.cpp`
# 的表逐项一致** —— 所以这里不手抄，而是从源码里正则读出来。
ROOT = F.ROOT
CARDFX_CPP = os.path.join(ROOT, "engine", "gameplay", "CardFx.cpp")
FX_DIR = os.path.join(ROOT, "assets", "fx")
BATTLE_VKS = os.path.join(ROOT, "templates", "card_battler", "assets",
                          "scripts", "battle.vks")


def group_g():
    """P3-4：卡牌表现模板 —— 表里写的资产必须真的在，模板脚本必须有入口。

    ⚠ 这一组**全是静态检查**，但它拦的是最贵的那类静默失败：
      "表里填了 `fx/card_play.veffect`，文件却不在" → 出牌没有粒子，
      日志一行错都没有（`EffectService::play` 找不到资产就返回 0）。
    """
    print("\n[G] 卡牌表现模板（P3-4）")

    try:
        with open(CARDFX_CPP, "r", encoding="utf-8") as f:
            src = f.read()
    except Exception as e:                                     # noqa: BLE001
        check("[G] 能读 CardFx.cpp", False, str(e))
        return

    import re as _re
    refs = _re.findall(r'"(fx/[A-Za-z0-9_]+\.veffect)"', src)
    check("[G] 模板表里至少有 2 个特效引用", len(refs) >= 2, refs)
    # ⚠ **不查"引用不重复"**：Hit/Crit 共用 `card_hit`、Shield/Buff 共用
    #   `card_buff` 是刻意的（同一个动作的轻重差异在飘字样式上，不在粒子上）。
    #   查重复会把"复用"判成"抄错"。
    check("[G] 引用的 .veffect 名字都在 assets/fx 下（拼写检查）",
          all(r.startswith("fx/") and ".." not in r for r in refs), refs)

    for rel in sorted(set(refs)):
        p = os.path.join(ROOT, "assets", rel)
        exists = os.path.isfile(p)
        check("[G] %s 存在" % rel, exists, "" if exists else "missing %s" % p)
        if not exists:
            continue
        try:
            with open(p, "r", encoding="utf-8") as f:
                eff = json.load(f)
        except Exception as e:                                 # noqa: BLE001
            check("[G] %s 是合法 JSON" % rel, False, str(e))
            continue
        parts = eff.get("particles") or []
        # 只有一层粒子也不够：`bursts` 里的 count 是 0 = 什么都不喷。
        # 两种驱动都算"能喷出东西"：bursts（一次性爆发）与 rate（持续型，
        # 见 heal_glow 的注释）。⚠ 只认 bursts 会把"持续型"判成红 —— 那是我
        # 的判据过窄，不是资产坏了（第一次跑就是这么红的）。
        emits = any(
            (b.get("count", 0) or 0) > 0
            for layer in parts
            for b in (layer.get("bursts") or []))
        emits = emits or any((layer.get("rate", 0.0) or 0.0) > 0.0
                             for layer in parts)
        check("[G] %s 有粒子层（particles 非空）" % rel, len(parts) >= 1,
              "layers=%d" % len(parts))
        check("[G] %s 真的会喷东西（burst count 或 rate > 0）" % rel, emits, "")

    # ---- 副本新鲜度 ----
    # ⚠⚠ 运行时读的是 `build/bin/assets/fx/`，不是仓库里那份。改了仓库不
    #   `make build` 的症状是：JSON 检查全绿、运行时 `FX-PLAY failed`（整条
    #   特效不播）。这一轮就是这么踩到的 —— `card_buff.veffect` 里有个
    #   未转义的 ASCII 双引号，仓库那份改好了、副本还是坏的。
    stale = []
    for rel in sorted(set(refs)):
        src = os.path.join(ROOT, "assets", rel)
        dst = os.path.join(F.BIN, "assets", rel)
        if not os.path.isfile(dst):
            stale.append("%s (缺副本)" % rel)
        elif os.path.getmtime(dst) < os.path.getmtime(src):
            stale.append("%s (副本比源旧)" % rel)
    check("[G] build/bin 里的 .veffect 副本是新烤的（改完要 make build）",
          not stale, stale)

    # ---- 模板脚本必须真的调用这两个动词 ----
    # ⚠ "引擎支持了但模板没接" = 出包的游戏里按右键什么也不发生，
    #   而所有自检全绿（它们测的是引擎，不是内容）。
    try:
        with open(BATTLE_VKS, "r", encoding="utf-8") as f:
            vks = f.read()
    except Exception as e:                                     # noqa: BLE001
        check("[G] 能读卡牌模板脚本", False, str(e))
        return
    check("[G] 卡牌模板脚本调用了 castCard（出包有入口）",
          "castCard(" in vks, "" if "castCard(" in vks else "missing")
    check("[G] 卡牌模板脚本调用了 drawCards",
          "drawCards(" in vks, "" if "drawCards(" in vks else "missing")

    # ---- 模板脚本**真的能编译**（不是只看字符串）----
    #
    # ⚠⚠ 这一条是 2026-11-04 补的，因为踩到了：模板里写着
    #   `if (pressed(...)) { let dots = tickRound() }`，而这个语言的 `let`
    #   **只允许在顶层** → 整份脚本 `not runnable` → 卡牌模板从诞生起
    #   脚本一行都没跑过（GAME-READY 都没打），而所有自检全绿：
    #   K33 只验 manifest / 场景存在，`--selftest script` 跑的是内联源码。
    #
    # 做法与 `verify_fx` 一致：把模板的场景 + 脚本拷进 `build/bin/assets`
    # （运行时只认 exeDir/assets），跑 60 帧，看有没有 `not runnable`。
    _run_template_script()


def _run_template_script():
    import shutil
    import subprocess

    vks_rel = "scripts/_card_probe.vks"
    scene_rel = "scenes/_card_probe.json"
    dst_vks = os.path.join(F.BIN, "assets", vks_rel)
    dst_scene = os.path.join(F.BIN, "assets", scene_rel)
    src_vks = os.path.join(ROOT, "templates", "card_battler", "assets",
                           "scripts", "battle.vks")
    src_scene = os.path.join(ROOT, "templates", "card_battler", "assets",
                             "scenes", "main.json")
    try:
        os.makedirs(os.path.dirname(dst_vks), exist_ok=True)
        shutil.copyfile(src_vks, dst_vks)
        with open(src_scene, "r", encoding="utf-8") as f:
            sc = json.load(f)
        for e in sc.get("entities", []):
            if isinstance(e.get("script"), dict):
                e["script"]["path"] = vks_rel
        with open(dst_scene, "w", encoding="utf-8", newline="\n") as f:
            json.dump(sc, f, ensure_ascii=False, indent=1)

        env = dict(os.environ)
        env.setdefault("MYVK_RUNTIME_AUDIO", "off")
        exe = os.path.join(F.BIN, "Game.exe")
        r = subprocess.run([exe, "--scene", scene_rel, "--frames", "60",
                            "--audio-null", "--windowed", "--fps", "0"],
                           cwd=F.BIN, capture_output=True, timeout=300,
                           env=env)
        out = (r.stdout or b"").decode("utf-8", "replace")
    except Exception as e:                                     # noqa: BLE001
        check("[G] 模板脚本能跑起来（冒烟）", False, str(e))
        return
    finally:
        for pth in (dst_vks, dst_scene):
            try:
                if os.path.isfile(pth):
                    os.remove(pth)
            except OSError:
                pass

    bad = [l for l in out.splitlines() if "not runnable" in l]
    check("[G] 模板脚本能编译（没有 `not runnable`）", not bad, bad[:1])
    # 正向锚点：`on start` 真的执行了。只看"没有错误"是不够的 ——
    # 脚本被禁用 / 路径写错时同样一行错都没有。
    check("[G] 模板脚本的 on start 真的执行了（GAME-READY）",
          "GAME-READY" in out,
          "" if "GAME-READY" in out else "log 里没有 GAME-READY")


# ---------------------------------------------------------------- main

GROUPS = {"A": group_a, "B": group_b, "C": group_c, "D": group_d, "E": group_e,
          "F": group_f, "G": group_g}


def main():
    only = None
    for a in sys.argv[1:]:
        if a.startswith("--only"):
            only = a.split("=", 1)[1] if "=" in a else sys.argv[sys.argv.index(a) + 1]

    for name, fn in GROUPS.items():
        if only and name not in only:
            continue
        fn()

    total = len(F.fails)
    print("\n" + "=" * 78)
    if total == 0:
        print("结果: PASS")
    else:
        print("结果: FAIL: %d 条" % total)
        for n in F.fails:
            print("  - %s" % n)
    print("=" * 78)
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
