#!/usr/bin/env python3
# ============================================================
# verify_levels —— 关卡流送（G19）
#
# ---------------------------- 这一刀在验什么 ----------------------------
# 2026-11-02 之前：`grep -rn "LevelStreaming\|LevelManifest"` 全仓 **0 命中**。
# 编辑器只认"当前打开的那一个场景文件"，游戏只能一次加载整个世界。
# 后果不是"慢"，是**做不了**大世界：几千个实体全在内存里、全进 BVH、
# 全参与剔除，而且没有"走近才加载"这件最基础的事。
#
# 这一刀分两组验，判据都不落在"有个面板"上：
#
#   A. 清单内核（`--selftest project` 的 LEVEL 组 31 条）
#      解析 / 校验（缺 name、缺 path、重名、只有 center 没有 halfExtent
#      一律整份拒收）/ 往返 / 距离 / 滞回。
#   B. 面板锚点 + `level.list` 的行数（编辑态）
#   C. **additive 加载不破坏已有实体**（G19 唯一会"静默吃掉数据"的地方）
#      ① 主场景的实体数在加载前后**不变**（`world=` 前后相等）；
#      ② 每张关卡的 `entities=` 等于它场景文件里的实体数；
#      ③ 日志里有 `SCENE-STREAM load`（走的是叠加那条路，不是整份替换）。
#   D. 按位置流送：进盒加载 / 出盒卸载
#   E. **常驻关卡永不自动卸载**（viewer 跑到天边它也还在）
#   F. **滞回**：加载盒 10 米、卸载盒 12.5 米 —— 11 米处**不许**卸
#      ⚠ 这一条是流送最容易写错的地方：两个盒写成同一个，玩家在边界上
#        左右晃就是"加载→卸载→加载"，每次都重新解析场景文件。
#   G. 真鼠标点 `LVL-LOAD` → 那张关卡真的进了世界
#
# ⚠ 夹具（1 份清单 + 3 张场景）写在 `build/bin/assets/` 下，跑完全部删掉。
#   三张场景是**从 `script_demo.json` 派生**的（手写 JSON 少一个字段就
#   静默丢实体），且**清掉 guid** —— 三份共用同一批 guid 会撞车，
#   撞车本身不中断加载（会 reassign），但会让"哪个实体来自哪张关卡"变浑。
#
# 用法：python tools/verify_levels.py [--only ABCDEFG]
# 结尾一定是 `结果: PASS` / `结果: FAIL: ...`。
# ============================================================

import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import kill_stale_editors, wait_rect             # noqa: E402
from verify_asset_drag import force_foreground                        # noqa: E402
from verify_add_menu import click                                     # noqa: E402

import verify_flags as F                                              # noqa: E402

BIN = F.BIN
ASSETS = F.ASSETS
SRC_SCENE = F.SRC_SCENE

LVL_MANIFEST = os.path.join(ASSETS, "levels", "_probe.vlevels")
LVL_MANIFEST_REL = "levels/_probe.vlevels"
LVL_SCENES = {
    "hub": ("scenes/_lvl_hub.json", 3, True, None),
    "arena": ("scenes/_lvl_arena.json", 2, False, ([0, 0, 0], [10, 10, 10])),
    "far": ("scenes/_lvl_far.json", 2, False, ([200, 0, 0], [10, 10, 10])),
}

fails = []


def check(name, ok, detail=""):
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                        ("  (%s)" % detail) if detail else ""))
    if not ok:
        fails.append(name)


def snap(tap):
    return F.snap(tap)


# ---------------------------------------------------------------- 夹具

def _derive_scene(src_entities, count, out_path):
    """从 script_demo 派生一张只含 N 个实体的场景。

    ⚠ 清掉 `guid` / `parentGuid`：三张关卡共用同一批 guid 会在叠加加载时
      撞车（不中断，但会让"这实体来自哪张关卡"变浑）。清掉后由
      `create()` 随机补发，三份天然互不相同。
    ⚠ 也清掉 `script` 组件：夹具里不需要脚本，留着只会让 Play 里多一堆
      与本题无关的 tick。
    """
    picked = []
    for e in src_entities:
        if len(picked) >= count:
            break
        if not isinstance(e, dict):
            continue
        ent = json.loads(json.dumps(e))
        ent.pop("guid", None)
        ent.pop("parentGuid", None)
        ent.pop("parent", None)
        ent.pop("script", None)
        picked.append(ent)
    while len(picked) < count:            # 源场景实体不够时补空壳
        picked.append({"name": "Filler%d" % len(picked),
                       "transform": {"position": [0, 0, 0],
                                     "rotation": [0, 0, 0],
                                     "scale": [1, 1, 1]}})
    scene = {"version": 2, "generator": "verify_levels.py",
             "entities": picked}
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)


def write_fixtures():
    os.makedirs(os.path.dirname(LVL_MANIFEST), exist_ok=True)
    with open(SRC_SCENE, "r", encoding="utf-8") as f:
        demo = json.load(f)
    src = [e for e in demo.get("entities", []) if isinstance(e, dict)]

    levels = []
    for name, (rel, n, always, box) in LVL_SCENES.items():
        _derive_scene(src, n, os.path.join(ASSETS, rel))
        entry = {"name": name, "path": rel}
        if always:
            entry["alwaysLoaded"] = True
        if box:
            entry["center"] = box[0]
            entry["halfExtent"] = box[1]
        levels.append(entry)

    manifest = {"name": "_probe", "levels": levels}
    with open(LVL_MANIFEST, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)


def remove_fixtures():
    paths = [LVL_MANIFEST]
    for rel, _n, _a, _b in LVL_SCENES.values():
        paths.append(os.path.join(ASSETS, rel))
    for p in paths:
        try:
            if os.path.isfile(p):
                os.remove(p)
        except Exception:                                          # noqa: BLE001
            pass


# ---------------------------------------------------------------- A

def group_a():
    print("\n[A] 清单内核（解析 / 校验 / 距离 / 滞回，31 条）")
    out = F.run_game_selftest("project")
    m = re.search(r"LEVEL-TEST total pass=(\d+) fail=(\d+)", out)
    check("[A] 打出 LEVEL-TEST total", m is not None, "")
    if not m:
        return
    p, f = int(m.group(1)), int(m.group(2))
    check("[A] 31 条全过", p >= 31 and f == 0, "pass=%d fail=%d" % (p, f))

    names = re.findall(r"LEVEL-TEST (?:PASS|FAIL) (.+)", out)
    need = ["L1 parse-ok", "L1b level-count", "L2 reject-no-name",
            "L2b reject-no-path", "L2c reject-duplicate",
            "L2d reject-half-box", "L3 roundtrip-count",
            "L4b inside-is-zero", "L4c outside-to-face",
            "L4d outside-diagonal", "L5c outside-not-wanted",
            "L6b buffer-not-unloaded", "L6c outside-unloads",
            "L6e scale-clamped", "L8b default-enabled"]
    missing = [n for n in need if n not in names]
    check("[A] 关键用例都在", not missing, "缺 %s" % missing)
    total = re.search(r"PROJECT-ALL-TOTAL failures=(\d+)", out)
    check("[A] 工程自检总计 0 失败",
          total is not None and total.group(1) == "0",
          total.group(0) if total else "no total line")


# ---------------------------------------------------------------- B

def group_b():
    print("\n[B] 面板锚点 + level.list（编辑态）")
    cmd = ("level.manifest %s; level.list; level.count; level.state"
           % LVL_MANIFEST_REL)
    write_fixtures()
    try:
        proc, hwnd, tap = F.run_editor(panel="levels", console_cmd=cmd,
                                       wait_tag="LVL-RECT",
                                       wait_for="LEVEL-STATE levels=",
                                       wait_secs=45.0)
        check("[B] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return
        for tag in ("LVL-RECT", "LVL-COUNT", "LVL-VIEWER", "LVL-RELOAD",
                    "LVL-LOAD", "LVL-UNLOAD", "LVL-UNLOADALL", "LVL-SRC"):
            r = wait_rect(tap, tag, timeout=12.0)
            check("[B] 锚点 %s" % tag, r is not None, "%s" % (r,))
        lines = snap(tap)
        F.close(proc)
        joined = "\n".join(lines)

        rows = [l for l in lines if "LEVEL-ROW " in l]
        check("[B] level.list 打出 3 行", len(rows) == 3,
              "rows=%d" % len(rows))
        check("[B] hub 标成常驻",
              any("name=hub loaded=0 always=1" in l for l in rows), rows[:2])
        # ⚠ `entities=` 是"**当前已加载**的实体数"，未加载时必然是 0 ——
        #   "清单指对了文件"要看**加载之后**的数量，那一条在 C 组
        #   （第一版把这两条写在这里，读到 entities=0 就红了，而其实是对的）。
        check("[B] 未加载时 entities=0（不是场景文件里的数量）",
              all("entities=0" in l for l in rows), rows[:2])
        check("[B] LEVEL-COUNT total=3",
              "LEVEL-COUNT total=3 loaded=0" in joined,
              [l for l in lines if "LEVEL-COUNT" in l][:3])
    finally:
        remove_fixtures()


# ---------------------------------------------------------------- C

def group_c():
    print("\n[C] additive 加载：不破坏已有实体、不动全局")
    write_fixtures()
    # ⚠ 判据是 `world=` **前后相等**：整份替换（`loadScene`）会把它变成
    #   副关卡的实体数，那是最容易犯的错，而"加载成功了"这条断言对它无效。
    cmd = ("level.manifest %s; level.state; "
           "level.load hub; level.state; "
           "level.load arena; level.state; level.list"
           % LVL_MANIFEST_REL)
    try:
        proc, hwnd, tap = F.run_editor(panel="levels", console_cmd=cmd,
                                       wait_tag="LVL-RECT",
                                       wait_for="LEVEL-COUNT total=3",
                                       wait_secs=50.0)
        check("[C] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return
        lines = snap(tap)
        F.close(proc)
        joined = "\n".join(lines)

        states = [l for l in lines if "LEVEL-STATE " in l]
        worlds = [int(m.group(1)) for m in
                  (re.search(r"world=(\d+)", l) for l in states) if m]
        check("[C] 打出了三次 LEVEL-STATE", len(states) >= 3,
              "n=%d" % len(states))
        # 编辑态 runtimeScene 的初始实体数（可能是 0）。加载两张关卡之后
        # 它应该**正好多出 5 个**（hub 3 + arena 2）。
        check("[C] 加载两张关卡后实体数 = 初始 + 5",
              len(worlds) >= 3 and worlds[-1] == worlds[0] + 5,
              "worlds=%s" % worlds[:6])
        # 反向：整份替换的实现在这里会红（worlds[-1] 会等于 2）
        check("[C] 不是整份替换（world 单调增长且没回落）",
              worlds == sorted(worlds), "worlds=%s" % worlds[:6])

        check("[C] 走的是 SCENE-STREAM（叠加）那条路",
              joined.count("SCENE-STREAM load") >= 2,
              joined.count("SCENE-STREAM load"))
        # ⚠ 判据只针对**流送的那两张关卡**，不能写成"整份日志里没有
        #   `Scene loaded`" —— 编辑器**启动**时会用 `loadScene` 打开主场景，
        #   那一行与流送无关（第一版就是这么红的：把开机日志算成了流送的错）。
        #   要看的是："`_lvl_` 这些副关卡有没有走整份替换那条路"。
        bad = [l for l in lines
               if "Scene loaded:" in l and "_lvl_" in l]
        check("[C] 副关卡没走整份替换（没有 Scene loaded: *_lvl_*）",
              not bad, bad[:2])

        rows = [l for l in lines if "LEVEL-ROW " in l]
        # ⚠ 这两条才是"清单指对了文件"的证据：指错文件（比如把 arena 的
        #   路径写在 hub 上）加载也会**成功**，只有实体数会不对。
        check("[C] 加载后 hub 有 3 个实体（与场景文件一致）",
              any("name=hub loaded=1" in l and "entities=3" in l for l in rows),
              rows[:3])
        check("[C] 加载后 arena 有 2 个实体",
              any("name=arena loaded=1" in l and "entities=2" in l
                  for l in rows), rows[:3])
        check("[C] 加载日志里 hub 报了 3 个实体",
              any("LEVELS load name=hub" in l and "entities=3" in l
                  for l in lines),
              [l for l in lines if "LEVELS load" in l][:3])
        check("[C] 最终 loaded=2",
              any("levels=3 loaded=2" in l for l in states), states[-2:])
        if rows:
            check("[C] 一行 LEVEL-ROW 都没漏", True, "")
    finally:
        remove_fixtures()


# ---------------------------------------------------------------- D/E/F

def group_def():
    print("\n[D/E/F] 按位置流送：进盒加载 / 出盒卸载 / 常驻 / 滞回")
    write_fixtures()
    # 顺序是判据的一部分（D 进 → F 缓冲带 → D 出 → E 天边）：
    #   0,0,0      → hub 常驻主动加载 + arena 进盒 → loaded=2
    #   11,0,0     → arena 在**卸载盒**（12.5）内 → 仍然 loaded=2
    #   13,0,0     → arena 出去了 → loaded=1
    #   200,0,0    → far 进盒 → loaded=2（hub 还在）
    #   9999,0,0   → hub 是常驻，**永不**卸载 → loaded=1
    cmd = ("level.manifest %s; "
           "level.stream 0 0 0; level.state; "
           "level.stream 11 0 0; level.state; "
           "level.stream 13 0 0; level.state; "
           "level.stream 200 0 0; level.state; "
           "level.stream 9999 0 0; level.state; level.list"
           % LVL_MANIFEST_REL)
    try:
        proc, hwnd, tap = F.run_editor(panel="levels", console_cmd=cmd,
                                       wait_tag="LVL-RECT",
                                       wait_for="LEVEL-COUNT total=3",
                                       wait_secs=55.0)
        check("[D] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return
        lines = snap(tap)
        F.close(proc)
        joined = "\n".join(lines)

        streams = [l for l in lines if "LEVEL-STREAM viewer=" in l]
        loaded = [int(m.group(1)) for m in
                  (re.search(r"loaded=(\d+)", l) for l in streams) if m]
        check("[D] 五次流送都跑了", len(streams) >= 5, "n=%d" % len(streams))
        # ⚠ 按**顺序**比整串，而不是"某处出现过某个数"
        check("[D] 加载序列 2→2→1→2→1", loaded[:5] == [2, 2, 1, 2, 1], loaded)

        # F：11 米处**不许**卸（加载盒 10 / 卸载盒 12.5）
        check("[F] 11 米（缓冲带内）没卸载",
              len(loaded) >= 2 and loaded[1] == 2, loaded[:3])
        check("[F] 13 米（缓冲带外）卸载了",
              len(loaded) >= 3 and loaded[2] == 1, loaded[:3])

        # E：常驻
        names_after = [l for l in lines if "LEVEL-ROW " in l]
        check("[E] 跑到 9999 米后 hub 仍然加载着",
              any("name=hub loaded=1" in l for l in names_after),
              names_after[-3:])
        check("[E] 跑到 9999 米后 arena / far 都卸了",
              any("name=arena loaded=0" in l for l in names_after) and
              any("name=far loaded=0" in l for l in names_after),
              names_after[-3:])
        # D：卸载是真把实体销毁了（不是只改标志位）
        # ⚠ 正则里必须带上中间的 `wanted=`：`stateLine()` 是
        #   `levels= loaded= wanted= entities= failed= viewer= world=`
        #   一整串，少写一段就永远匹配不上（而看起来像"卸载没生效"）。
        check("[D] 卸载后实体总数回到只剩常驻的 3 个",
              any(re.search(r"LEVEL-STATE levels=3 loaded=1 wanted=\d+ "
                            r"entities=3", l) for l in lines),
              [l for l in lines if "loaded=1" in l and "LEVEL-STATE" in l][-2:])
    finally:
        remove_fixtures()


# ---------------------------------------------------------------- G

def group_g():
    print("\n[G] 真鼠标点 LVL-LOAD → 那张关卡真的进了世界")
    write_fixtures()
    # 先选中 hub（`level.list` 只打印；选中靠点表格行）——
    # ⚠ 面板的 Load 需要"有选中"，所以 G 组要先点一下表格里那一行。
    cmd = ("level.manifest %s; level.list; level.state" % LVL_MANIFEST_REL)
    try:
        proc, hwnd, tap = F.run_editor(panel="levels", console_cmd=cmd,
                                       wait_tag="LVL-SRC",
                                       wait_for="LEVEL-COUNT total=3",
                                       wait_secs=45.0)
        check("[G] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return
        # ⚠ 必须点**带名字的**锚点：`LVL-LIST` 被 `logRect` 去重成一条，
        #   留哪一条取决于首帧布局是否已稳定（实测留的是**最后一行**）——
        #   点它等于随机选一张关卡，于是 `level.load far` 而不是 hub。
        r = wait_rect(tap, "LVL-ROW-hub", timeout=15.0)
        check("[G] 找到 hub 那一行的矩形", r is not None, "%s" % (r,))
        if r:
            force_foreground(hwnd)
            time.sleep(0.3)
            click(hwnd, (r[0] + r[2]) / 2.0, (r[1] + r[3]) / 2.0)
            time.sleep(0.8)

        rl = wait_rect(tap, "LVL-LOAD", timeout=10.0)
        check("[G] 找到 Load 按钮", rl is not None, "%s" % (rl,))
        hit = False
        if rl:
            cx = (rl[0] + rl[2]) / 2.0
            cy = (rl[1] + rl[3]) / 2.0
            force_foreground(hwnd)
            time.sleep(0.3)
            for _ in range(3):
                click(hwnd, cx, cy)
                end = time.time() + 2.5
                while time.time() < end:
                    if any("LEVELS-CMD load name=hub ok=1" in l
                           for l in snap(tap)):
                        hit = True
                        break
                    time.sleep(0.15)
                if hit:
                    break
        lines = snap(tap)
        F.close(proc)
        joined = "\n".join(lines)
        check("[G] 点击真的加载了 hub",
              "LEVELS-CMD load name=hub ok=1" in joined,
              [l for l in lines if "LEVELS-CMD" in l][:3])
        # 加码：真的进了世界（不是只改了标志位）
        check("[G] hub 的 3 个实体进了场景",
              any("LEVELS load name=hub" in l and "entities=3" in l
                  for l in lines),
              [l for l in lines if "LEVELS load" in l][:3])
    finally:
        remove_fixtures()


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="ABCDEFG")
    args = ap.parse_args()
    only = args.only.upper()

    print("=" * 74)
    print("关卡流送（G19）：清单内核 / 叠加加载 / 按位置流送 / 常驻 / 滞回")
    print("=" * 74)

    kill_stale_editors()
    try:
        if "A" in only:
            group_a()
        if "B" in only:
            group_b()
        if "C" in only:
            group_c()
        if "D" in only or "E" in only or "F" in only:
            group_def()
        if "G" in only:
            group_g()
    finally:
        remove_fixtures()

    print("\n" + "=" * 74)
    if fails:
        print("结果: FAIL: %d 项 —— %s" % (len(fails), fails[:6]))
        return 1
    print("结果: PASS（31 条内核 + 面板/命令 + 叠加不毁已有实体 + "
          "进盒加载/出盒卸载 + 常驻不卸 + 滞回 + 鼠标加载）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
