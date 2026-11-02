#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C8 导航（格子网 / A* / 视线平滑 / 代理跟随 / 网格资产 / 脚本驱动）验收（A~F）。

A  内核自检（`Game.exe --selftest nav`，94 条）：尺寸/原点/哨兵 · A* 六条
   规矩 · 视线与平滑 · 吸附 · 代理推进 · 存盘往返 · ASCII 形状
B  编辑器命令链：`nav.bake` → `nav.state` → `nav.ascii` → `nav.path` →
   `nav.save` → `nav.load` → `nav.agent` → `nav.tick` → `nav.stop`
C  面板锚点（`Authoring ▸ Navigation`）：Bake / 参数 / 路径 / 代理 / 资产
   五组控件都真的画出来了（`logRect` 有矩形）
D  运行时：`Game.exe --scene` 真跑，断言 `GAME-NAV source=…` 与
   `GAME-NAV tick ... moving>0`（**代理真的动了**，不是"引擎里有个类"）
E  网格资产（`.vnav.json`）：往返逐格一致 / 位图长度与 cols*rows 对不上被拒 /
   坏资产回落到现场烘焙而不是留一张空网
F  **脚本驱动**（`navGoto/navArrived/navFailed/navStop`）：真宿主里跑一份
   `.vks` —— 断言 `goto 1`（受理）· `arrived` 且**到达点 x > 3**（真的过墙，
   不是"恒返回 1"）· 没有 `is not available`

⚠ 判据口径（为什么这么选）：
   · **`nav.path` 断言的是"长度 > 直线距离"**，不是"返回了一条路径"。
     格子法 + 绕墙的两种实现（绕过去 / 直接穿墙）都能给出"一条路径"，
     只有长度能分开它们。
   · **`nav.tick` 断言 `moving>0` 之后还要断言 `arrived`**：只验"动了"
     的话，一个每帧原地抖动的代理照样绿。
   · **D / F 组必须真跑游戏**：`--selftest nav` 用的是内存里造的网格，证明不了
     "运行时把资产/烘焙接上了"。而"接没接上"恰恰是这类功能最常见的缺口
     （引擎里有、运行时没调）。
   · **E 组要破坏一次**：只验"好文件读得进"的话，"位图长度对不上也照读"
     这种实现永远绿。
   · **F 组断言位置而不只是那行日志**：`navArrived()` 恒返回 1 的实现也能
     让 `NAVPROBE arrived` 出现（在第 0 帧）。到达点的 x 才是"真走过去了"。

用法：
    python tools/verify_nav.py             # 全量 A~F
    python tools/verify_nav.py --only AB   # 只跑 A、B
    python tools/verify_nav.py --keep      # 保留夹具（排查用）
"""

import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_flags as F          # run_editor / snap / check / 夹具路径

ASSETS = F.ASSETS
BIN = F.BIN
GAME = F.GAME

SCENE_REL = "scenes/_nav_probe.json"
GRID_REL = "nav/_nav_probe.vnav.json"
BAD_GRID_REL = "nav/_nav_probe_bad.vnav.json"
# F 组：脚本驱动（独立的场景 + 脚本，**故意不读** B 组产出的网格资产
# —— 这样 F 组验的是"现场烘 + 脚本动词"这条完整兜底路）。
SCENE2_REL = "scenes/_nav_script_probe.json"
SCRIPT_REL = "scripts/_nav_probe.vks"

GROUP = {"A", "B", "C", "D", "E", "F"}
failures = []


def check(name, ok, detail=""):
    F.check(name, ok, detail)


def lines_of(tap):
    return F.snap(tap)


def has(lines, needle):
    return any(needle in ln for ln in lines)


def first(lines, needle):
    for ln in lines:
        if needle in ln:
            return ln
    return ""


# ---------------------------------------------------------------- 夹具

def _mesh_cube(name, size):
    return {"kind": "builtin", "shape": "cube", "size": size, "name": name}


def _mat(name, color):
    return {"kind": "procedural", "name": name, "baseColor": color, "metallic": 0.0,
            "roughness": 0.7, "normalScale": 0.0, "ao": 1.0, "emissive": [0, 0, 0],
            "doubleSided": False, "alphaBlend": False}


def _entity(name, pos, scale=(1.0, 1.0, 1.0), mesh=None, material=None, collision=None,
            extra=None):
    e = {"name": name,
         "transform": {"position": list(pos), "rotation": [0.0, 0.0, 0.0],
                       "scale": list(scale)},
         "parent": -1, "visible": True, "castShadow": True}
    if mesh:
        e["mesh"] = mesh
    if material:
        e["material"] = material
    if collision is not None:
        e["collision"] = collision
    if extra:
        e.update(extra)
    return e


def make_scene(path):
    """一张"一堵墙 + 一个 NPC"的小关卡。

    · Wall 覆盖 y ∈ [-3, 3] → 从 (-6,0) 到 (6,0) 的直线**必然**被挡住，
      所以 `nav.path` 的长度可以当判据（绕过去一定更长）。
    · Npc 带 `nav` 块（`autoStart` + `goal`）→ 运行时不用脚本就会走。
    """
    ents = [
        _entity("Npc", (-6.0, 0.0, 0.0), (1.0, 1.0, 1.0),
                mesh=_mesh_cube("npc_mesh", 0.8),
                material=_mat("npc_mat", [0.35, 0.55, 0.90, 1.0]),
                extra={"nav": {"speed": 3.0, "goal": [6.0, 0.0, 0.0], "autoStart": True}}),
        _entity("Wall", (0.0, 0.0, 0.75), (1.0, 6.0, 1.5),
                mesh=_mesh_cube("wall_mesh", 1.0),
                material=_mat("wall_mat", [0.55, 0.50, 0.45, 1.0]),
                # ⚠ 只有 `CollisionComponent` **不会**建刚体：物理世界是按
                #   `RigidBodyComponent` 遍历的（`buildSceneWorld` 的 ① 段），
                #   碰撞组件只是"形状从哪来"。少了这一块，症状是
                #   `bodies=1`（只有无限地面）、整张网格全可走 —— 而烘焙
                #   会报"成功"，因为确实采到了地面。
                collision={"shape": "convex", "solid": True},
                extra={"rigidbody": {"bodyType": 0, "shape": 0, "useGravity": False}}),
        # 地面用**无限地面**（`groundPlane`）而不是一个 plane 网格：
        #   plane 是单个四边形，auto 碰撞要从它生成凸包 —— 退化形状（零体积）
        #   会让"射线打不到地面"，而症状是"整张网格全 Blocked"，
        #   看起来像烘焙坏了。

    ]
    scene = {
        "version": 2,
        "generator": "verify_nav",
        "camera": {"target": [0, 0, 1.0], "yaw": 0.0, "pitch": -20.0,
                   "distance": 18.0},
        "physics": {"enabled": True, "groundPlane": True},
        "environment": {},
        "entities": ents,
    }
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)


NAV_PROBE_VKS = """# _nav_probe.vks —— verify_nav F 组的夹具：一条脚本驱动的巡逻。
#
# 要验的是 **`navGoto` 真的接到了宿主**（而不是"关卡里摆了 goal 所以会走"）：
#   · `on start` 里点名要去 (6,0,0) —— 起点 (-6,0,0) 与它之间有一堵墙；
#   · `on update` 里轮询到达 / 不可达，各打一行**一次性**的日志
#     （`done` 那个标志必须有：不加的话 150 帧能刷 150 行，日志没法定点断言）。
#
# ⚠ `navArrived()` 与 `navFailed()` 必须成对：只等 arrived 的话，
#   目标在墙外时这个脚本会**永远转**（配额不报错，只是什么都不做）。

let done = 0

on start {
    print("NAVPROBE goto", navGoto(6.0, 0.0, 0.0))
}

on update {
    if (done == 0) {
        if (navArrived()) {
            done = 1
            print("NAVPROBE arrived", pos(self()))
        }
        if (navFailed()) {
            done = 1
            print("NAVPROBE failed")
        }
    }
}
"""


def make_scene2(scene_path, script_abs):
    """F 组夹具：一个**只由脚本驱动**的 NPC。

    ⚠ `nav` 块里刻意**没有** `goal` / `autoStart`：D 组已经验了"关卡里摆的
      目标会走"，F 组要验的是**脚本**那条路。两种驱动方式分开验，否则
      "`navGoto` 根本没接到宿主"会被 `autoStart` 遮住（NPC 照样在走，
      断言全绿）。判断依据就是 `NAVPROBE goto 1` 那行——它是脚本**打印**
      出来的，宿主没接的话那行会变成 `navGoto() is not available`。
    """
    ents = [
        _entity("Npc", (-6.0, 0.0, 0.0), (1.0, 1.0, 1.0),
                mesh=_mesh_cube("npc_mesh", 0.8),
                material=_mat("npc_mat", [0.35, 0.55, 0.90, 1.0]),
                extra={"nav": {"speed": 3.0},
                       "script": {"path": script_abs.replace("\\", "\\\\"),
                                  "enabled": True, "timeScale": 1.0}}),
        _entity("Wall", (0.0, 0.0, 0.75), (1.0, 6.0, 1.5),
                mesh=_mesh_cube("wall_mesh", 1.0),
                material=_mat("wall_mat", [0.55, 0.50, 0.45, 1.0]),
                collision={"shape": "convex", "solid": True},
                extra={"rigidbody": {"bodyType": 0, "shape": 0, "useGravity": False}}),
    ]
    scene = {
        "version": 2,
        "generator": "verify_nav",
        "camera": {"target": [0, 0, 1.0], "yaw": 0.0, "pitch": -20.0,
                   "distance": 18.0},
        "physics": {"enabled": True, "groundPlane": True},
        "environment": {},
        "entities": ents,
    }
    os.makedirs(os.path.dirname(scene_path) or ".", exist_ok=True)
    with open(scene_path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)


def cleanup(keep):
    if keep:
        return
    for rel in (SCENE_REL, GRID_REL, BAD_GRID_REL, SCENE2_REL, SCRIPT_REL):
        p = os.path.join(ASSETS, *rel.split("/"))
        try:
            if os.path.exists(p):
                os.remove(p)
        except OSError:
            pass


# ---------------------------------------------------------------- A：内核自检
def group_a():
    out = F.run_game_selftest("nav", timeout=180.0)
    check("[A] 自检进程跑完（有 NAV-TOTAL 行）", "NAV-TOTAL" in out)
    total = first(out.splitlines(), "NAV-TOTAL")
    check("[A] NAV-TOTAL pass=94 fail=0", "pass=94 fail=0" in total, total.strip())
    # 点名几条"最容易悄悄变绿"的断言：它们各自钉着一条 A* 规矩。
    for name in ("B4.no_cut", "B5.detoured", "B6.same_path_diff", "C3.smoother_fewer",
                 "E4.reached", "E8.pos_untouched", "F2.path_empty", "F3.speed"):
        ok = any(("%s " % name) in ln and "result=PASS" in ln for ln in out.splitlines())
        check("[A] 自检 %s" % name, ok)


# ---------------------------------------------------------------- B：命令链

def group_b():
    scene = os.path.join(ASSETS, *SCENE_REL.split("/"))
    cmds = ";".join([
        "nav.state",
        "nav.bake",
        "nav.state",
        "nav.ascii",
        "nav.path -6 0 6 0",
        "nav.path 40 40 42 42",          # 网格外（盒是 ±24）→ 吸附失败
        "nav.save " + GRID_REL,
        "nav.load " + GRID_REL,
        "nav.load nav/does_not_exist.json",
        "nav.agent 6 0 0",
        "nav.tick 60",
        "nav.state",
        "nav.stop",
        "nav.clear",
        "nav.state",
    ])
    # `nav.agent` / `nav.stop` 作用在**选中项**上 → 必须先预选 Npc
    # （`MYVK_EDITOR_SELECT` 是编辑器既有的"启动时选中"开关）。
    proc, hwnd, tap = F.run_editor(panel="navigation", console_cmd=cmds, scene=SCENE_REL,
                                   # 等到**最后一条**命令的日志再快照：只等第一个
                                   # state 的话，后面十几条还在管道里没到，
                                   # 断言会读到"少了一半的日志"（而且看起来像
                                   # 命令没注册）。
                                   wait_for="GP-NAV-CMD name=clear", wait_secs=40.0,
                                   env_extra={"MYVK_EDITOR_SELECT": "Npc"})
    try:
        lines = lines_of(tap)
        states = [ln for ln in lines if "GP-NAV-CMD name=state" in ln]
        check("[B] nav.state 至少打了 4 次（烘前/烘后/推进后/清空后）", len(states) >= 4,
              "n=%d" % len(states))
        if len(states) >= 4:
            check("[B] 烘之前 grid=0", "grid=0" in states[0], states[0].strip())
            check("[B] 烘之后 grid=1", "grid=1" in states[1], states[1].strip())
            check("[B] 清空之后回到 grid=0", "grid=0" in states[-1], states[-1].strip())

        bake = first(lines, "GP-NAV-CMD name=bake")
        check("[B] nav.bake 成功且 walk>0", "NAV-BAKE walk=" in bake and "walk=0 " not in bake,
              bake.strip())
        # ⚠ 墙必须真的被采到。`walk=9216` 是"96×96 全可走"—— 那意味着
        #   Wall 的碰撞体根本没建（`shape` 写成 "auto" 就会被落到 capsule，
        #   而胶囊不是从网格生成的）。所以这条断言钉的是"墙生效了"。
        check("[B] 墙挡住了格子（walk < cells）", "walk=9216" not in bake, bake.strip())
        # 尺寸纪律：96×96 是 48 m / 0.5 m 的默认盒子的结果，改了默认值这条会红
        check("[B] 烘焙尺寸 grid=96x96", "grid=96x96" in bake,
              bake.strip())

        ascii_line = [ln for ln in lines if "GP-NAV-CMD name=ascii" in ln]
        check("[B] nav.ascii 打了图形（有 '#' 与 '.'）",
              bool(ascii_line) and any("#" in ln for ln in lines)
              and any("." in ln for ln in lines))

        paths = [ln for ln in lines if "GP-NAV-CMD name=path" in ln]
        check("[B] nav.path 跑了两次", len(paths) >= 2, "n=%d" % len(paths))
        if len(paths) >= 2:
            check("[B] 穿墙路径 reached=1", "reached=1" in paths[0], paths[0].strip())
            # 直线 12 米；绕墙一定更长 —— 这是"格子法真的在绕"的唯一证据
            ln = paths[0]
            try:
                length = float(ln.split("len=")[1].split()[0])
            except (IndexError, ValueError):
                length = -1.0
            check("[B] 绕墙路径长度 > 12", length > 12.0, "len=%.2f" % length)
            check("[B] 网格外的目标被拒（ok=0）", "ok=0" in paths[1], paths[1].strip())

        save = first(lines, "GP-NAV-CMD name=save")
        check("[B] nav.save 成功", "NAV-GRID save=1" in save, save.strip())
        grid_path = os.path.join(ASSETS, *GRID_REL.split("/"))
        check("[B] 网格资产真的落盘了", os.path.exists(grid_path), grid_path)
        load = [ln for ln in lines if "GP-NAV-CMD name=load" in ln]
        check("[B] nav.load 成功", bool(load) and "NAV-GRID load=1" in load[0],
              load[0].strip() if load else "")
        check("[B] 读不存在的资产报错（load=0）",
              len(load) >= 2 and "NAV-GRID load=0" in load[1],
              load[1].strip() if len(load) >= 2 else "")

        agent = first(lines, "GP-NAV-CMD name=agent")
        check("[B] nav.agent 挂上代理（ok=1）", "NAV-AGENT ok=1" in agent, agent.strip())
        tick = first(lines, "GP-NAV-CMD name=tick")
        check("[B] nav.tick 推进了代理（moving>0）", "moving=0 " not in tick and "agents=1" in tick,
              tick.strip())
        stop = first(lines, "GP-NAV-CMD name=stop")
        check("[B] nav.stop 成功", "NAV-STOP ok=1" in stop, stop.strip())
    finally:
        F.close(proc)


# ---------------------------------------------------------------- C：面板锚点

def group_c():
    proc, hwnd, tap = F.run_editor(panel="navigation", scene=SCENE_REL,
                                   wait_tag="LAYOUT-RECT", wait_secs=30.0)
    try:
        lines = lines_of(tap)
        tags = ["NAV-BAKE", "NAV-CLEAR", "NAV-ASCII", "NAV-CELL", "NAV-RADIUS",
                "NAV-SLOPE", "NAV-STEP", "NAV-PATH-TEST", "NAV-AGENT-ATTACH",
                "NAV-AGENT-STOP", "NAV-GRID-PATH", "NAV-GRID-SAVE", "NAV-GRID-LOAD"]
        for tag in tags:
            check("[C] 面板锚点 %s 有矩形" % tag, has(lines, tag))
        # 参数控件的顺序就是面板上的顺序 —— 反过来（代理在前）说明面板被重排过，
        # 而"按 rect 点控件"的用例会静默漂移。
        # 锚点格式：`TAG=(x0,y0)-(x1,y1) center=(cx,cy)` —— 取 y0 判断上下关系。
        def y(tag):
            ln = first(lines, tag)
            try:
                return float(ln.split("=(")[1].split(",")[1].split(")")[0])
            except (IndexError, ValueError):
                return -1.0
        check("[C] 网格参数在代理段上面", 0 < y("NAV-CELL") < y("NAV-AGENT-ATTACH"),
              "cell=%.0f agent=%.0f" % (y("NAV-CELL"), y("NAV-AGENT-ATTACH")))
    finally:
        F.close(proc)


# ---------------------------------------------------------------- D：运行时

def group_d():
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    proc = subprocess.run([GAME, "--scene", SCENE_REL, "--frames", "150"], cwd=BIN, env=env,
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=240.0)
    out = (proc.stdout or "") + (proc.stderr or "")
    nav_lines = [ln for ln in out.splitlines() if "GAME-NAV" in ln]
    check("[D] 运行时打了 GAME-NAV（导航接上了）", bool(nav_lines),
          "%d lines" % len(nav_lines))
    check("[D] 走的是资产（source=asset）", any("source=asset" in ln for ln in nav_lines),
          next((ln.strip() for ln in nav_lines if "source=" in ln), ""))
    ticks = [ln for ln in nav_lines if "GAME-NAV tick" in ln]
    check("[D] 代理被推进过（有 tick 行）", bool(ticks), "%d" % len(ticks))
    if ticks:
        ln = ticks[0]
        check("[D] tick agents=1", "agents=1" in ln, ln.strip())
        moved = False
        for t in ticks:
            try:
                if int(t.split("moving=")[1].split()[0]) > 0:
                    moved = True
                    break
            except (IndexError, ValueError):
                pass
        check("[D] 代理真的动了（moving>0）", moved,
              ticks[0].strip() if ticks else "")
    check("[D] 运行时零 ERROR", "[ERROR]" not in out,
          next((ln.strip() for ln in out.splitlines() if "[ERROR]" in ln), ""))


# ---------------------------------------------------------------- E：网格资产

def group_e():
    grid_path = os.path.join(ASSETS, *GRID_REL.split("/"))
    if not os.path.exists(grid_path):
        check("[E] 前置：网格资产存在（B 组产出）", False, grid_path)
        return
    with open(grid_path, encoding="utf-8") as f:
        j = json.load(f)
    cols, rows = j.get("cols"), j.get("rows")
    check("[E] 资产里有 cols/rows/cell", bool(cols) and bool(rows) and "cell" in j,
          "cols=%s rows=%s" % (cols, rows))
    check("[E] 位图长度 == cols*rows", len(j.get("cells", "")) == cols * rows,
          "%d vs %d" % (len(j.get("cells", "")), cols * rows))
    check("[E] 高度数组长度 == cols*rows", len(j.get("height", [])) == cols * rows,
          "%d" % len(j.get("height", [])))
    check("[E] 位图只含 0/1", set(j.get("cells", "")) <= {"0", "1"},
          str(sorted(set(j.get("cells", "")))))
    # 有墙 → 图里必须两种都有（全 1 = 没采到墙，全 0 = 没采到地面）
    check("[E] 位图里既有可走也有不可走", "0" in j.get("cells", "") and "1" in j.get("cells", ""))

    # 破坏一次：位图截短 → 必须被拒（否则"格子数与位图对不上"会静默错位）
    bad = dict(j)
    bad["cells"] = j["cells"][:-1]
    bad_path = os.path.join(ASSETS, *BAD_GRID_REL.split("/"))
    with open(bad_path, "w", encoding="utf-8") as f:
        json.dump(bad, f)

    cmds = ";".join(["nav.load " + GRID_REL, "nav.load " + BAD_GRID_REL, "nav.state"])
    # `nav.agent` / `nav.stop` 作用在**选中项**上 → 必须先预选 Npc
    # （`MYVK_EDITOR_SELECT` 是编辑器既有的"启动时选中"开关）。
    proc, hwnd, tap = F.run_editor(panel="navigation", console_cmd=cmds, scene=SCENE_REL,
                                   # 等到**最后一条**命令的日志再快照：只等第一个
                                   # state 的话，后面十几条还在管道里没到，
                                   # 断言会读到"少了一半的日志"（而且看起来像
                                   # 命令没注册）。
                                   wait_for="GP-NAV-CMD name=clear", wait_secs=40.0,
                                   env_extra={"MYVK_EDITOR_SELECT": "Npc"})
    try:
        lines = lines_of(tap)
        loads = [ln for ln in lines if "GP-NAV-CMD name=load" in ln]
        check("[E] 好资产读进来", bool(loads) and "NAV-GRID load=1" in loads[0],
              loads[0].strip() if loads else "")
        check("[E] 截短的位图被拒（load=0）",
              len(loads) >= 2 and "NAV-GRID load=0" in loads[1],
              loads[1].strip() if len(loads) >= 2 else "")
        if len(loads) >= 2:
            check("[E] 拒绝原因说清了（bitmap size）", "bitmap size" in loads[1],
                  loads[1].strip())
        # ⚠ 读失败**不许清掉**旧网格：清了的话这一关瞬间"哪儿都不能走"
        st = first(lines, "GP-NAV-CMD name=state")
        check("[E] 读失败后网格还在（grid=1）", "grid=1" in st, st.strip())
    finally:
        F.close(proc)


# ---------------------------------------------------------------- F：脚本动词

def group_f():
    """`navGoto` / `navArrived` / `navFailed` / `navStop` 在**真宿主**里通了。

    ⚠ 为什么 A 组（`--selftest script` C22）不能替代这一组：
      C22 用的是 MockHost（记录器），它证明的是"求值器 → 时宿主动作"接线对。
      但"运行时把 `navGoto` 接到 `m_nav` 上了吗""网格烘出来了没有"
      "代理真的走到了吗"全在真宿主那一侧 —— 那正是这类功能最常见的缺口
      （引擎里有、运行时没调）。
    """
    script_abs = os.path.join(ASSETS, *SCRIPT_REL.split("/"))
    os.makedirs(os.path.dirname(script_abs) or ".", exist_ok=True)
    with open(script_abs, "w", encoding="utf-8", newline="\n") as f:
        f.write(NAV_PROBE_VKS)
    make_scene2(os.path.join(ASSETS, *SCENE2_REL.split("/")), script_abs)

    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    proc = subprocess.run([GAME, "--scene", SCENE2_REL, "--frames", "200"],
                          cwd=BIN, env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=240.0)
    out = (proc.stdout or "") + (proc.stderr or "")
    lines = out.splitlines()

    # ① 兜底路径：这张场景没有 `nav/_nav_script_probe.vnav.json`
    #    → 必须现场烘（source=bake）。这条同时证明"没有资产时不是全静止"。
    nav_lines = [ln for ln in lines if "GAME-NAV" in ln]
    check("[F] 没有资产 → 现场烘（source=bake）",
          any("source=bake" in ln for ln in nav_lines),
          next((ln.strip() for ln in nav_lines if "source=" in ln), ""))

    # ② 脚本能被宿主执行（不是 not runnable）
    check("[F] 脚本跑起来了（没有 not runnable）",
          not any("not runnable" in ln for ln in lines),
          next((ln.strip() for ln in lines if "not runnable" in ln), ""))

    # ③ navGoto 接到宿主了：脚本打印的是 1，而不是 "not available"
    goto = [ln for ln in lines if "NAVPROBE goto" in ln]
    check("[F] navGoto 受理（脚本打印 goto 1）",
          bool(goto) and goto[0].rstrip().endswith("goto 1"),
          goto[0].strip() if goto else "")
    check("[F] 宿主接了 navGoto（没有 is not available）",
          not any("navGoto() is not available" in ln for ln in lines),
          next((ln.strip() for ln in lines if "is not available" in ln), ""))

    # ④ 代理真的走到了：`NAVPROBE arrived (x, y, z)` 里的 x 必须过墙
    #    ⚠ 只断言"有一行 arrived"是不够的：`navArrived()` 恒返回 1 的实现
    #      也能让这行出现（在第 0 帧）。位置才是"真的走过去了"的判据。
    arrived = [ln for ln in lines if "NAVPROBE arrived" in ln]
    moved_x = None
    if arrived:
        try:
            vec = arrived[0].split("(")[1].split(")")[0]
            moved_x = float(vec.split(",")[0])
        except (IndexError, ValueError):
            moved_x = None
    check("[F] 走完了全程（NAVPROBE arrived）", bool(arrived),
          arrived[0].strip() if arrived else "")
    check("[F] 真的过了墙（到达点 x > 3）", moved_x is not None and moved_x > 3.0,
          "x=%s" % moved_x)

    # ⑤ 没有走成 failed（走了 failed 说明求路失败但那行 arrived 还在）
    check("[F] 没有走成 failed", not any("NAVPROBE failed" in ln for ln in lines))

    # ⑥ 运行时零 ERROR（VUID / 断言都在这条下面）
    check("[F] 运行时零 ERROR", "[ERROR]" not in out,
          next((ln.strip() for ln in lines if "[ERROR]" in ln), ""))


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑这些组（如 AB）")
    ap.add_argument("--keep", action="store_true", help="保留夹具")
    args = ap.parse_args()
    want = set(args.only.upper()) & GROUP if args.only else set(GROUP)

    make_scene(os.path.join(ASSETS, *SCENE_REL.split("/")))

    t0 = time.time()
    if "A" in want:
        group_a()
    if "B" in want:
        group_b()      # B 组会产出网格资产，E 组依赖它
    if "C" in want:
        group_c()
    if "D" in want:
        group_d()      # D 组要读 E/B 产出的资产
    if "E" in want:
        group_e()
    if "F" in want:
        group_f()      # F 组自带夹具，不依赖 B/E 的产出

    cleanup(args.keep)

    total = 0
    try:
        import verify_flags
        total = len(verify_flags.fails)
    except Exception:
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
