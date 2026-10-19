#!/usr/bin/env python3
"""生成 S6 的示例关卡 `assets/scenes/levels/script_demo.json`
（docs/script_roadmap.md §7 S6）。

场景内容（**全部逻辑在脚本里，一行 C++ 都不改**）：

    Ground           60 m 平地（physics.groundPlane 撑住玩家）
    Player           带 character + camera 的角色，站在原点
    Patrol_A/B/C     3 个巡逻兵   → assets/scripts/enemy_patrol.vks
    Door_A / Door_B  2 扇定时门   → assets/scripts/door_timer.vks
    Pickup_0..4      5 个拾取物   → assets/scripts/pickup.vks
    Director         记分板+计时   → assets/scripts/hud_counter.vks

**为什么生成而不是手写**：拾取物要围成一个半径 1.2 m 的环（脚本里的
拾取半径是 1.5 m），巡逻兵要摆在同一条 X 轴上、彼此 y 错开 4~5 m 才不会
互相穿过；这些坐标改一次就得全部重算，而算错了**不会报错** —— 只会表现成
"捡了几个就没动静了"。

⚠ 与路线图原文的一处差异（必须记账）：原文写"1 个地形"。这里用的是
   **平地**而不是 `terrain/hills`。原因：`engine/terrain` 的高度场**没有
   接进物理**（`runtime/` 与 `editor/` 里搜不到 `Heightfield`），角色的
   胶囊会直接掉到 z=0，而 hills 地形在原点附近高 11~17 m —— 整关的玩法
   判定（距离/拾取/门）会全部失真。等 W5 之后地形物理接通，把 Ground
   换成地形实体即可，脚本一行都不用改。

三条纪律（与 gen_first_level.py / gen_terrain_probe.py 相同）：
  1. 确定性：无随机数，同一份脚本跑两次逐字节相同
  2. 材质只引用 `.vmat`
  3. 与写端的规范化形状一致：mesh 写 5 字段、material 块写 kind/path/name/id

用法：
    python tools/gen_script_demo.py
    python tools/gen_script_demo.py --check     # 只比对，不写盘
"""

import argparse
import json
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_REL = os.path.join("assets", "scenes", "levels", "script_demo.json")
OUT_ABS = os.path.join(ROOT, OUT_REL)

M_CRATE = "materials/crate.vmat"
M_WALL = "materials/wall.vmat"
M_METAL = "materials/metal.vmat"
M_GROUND = "materials/ground.vmat"
M_SKIN = "materials/skin.vmat"
M_CLOTH = "materials/cloth.vmat"


def prim(kind, size=1.0, radius=0.5, segments=48, rings=24):
    return {"kind": kind, "size": size, "radius": radius,
            "segments": segments, "rings": rings}


def vmat(path):
    base = path[: -len(".vmat")]
    return {"kind": "vmat", "path": path, "name": base, "id": base}


def mesh(shape, size=1.0, name=None):
    return {"kind": "builtin", "shape": shape, "size": size,
            "name": name or ("script_demo_" + shape)}


def ent(name, pos, msh, mat, prim_shape=None, rot=(0.0, 0.0, 0.0),
        scale=(1.0, 1.0, 1.0), shadow=True, locked=False):
    e = {
        "name": name,
        "transform": {
            "position": [round(v, 4) for v in pos],
            "rotation": [round(v, 4) for v in rot],
            "scale": [round(v, 4) for v in scale],
        },
        "parent": -1,
        "visible": True,
        "castShadow": shadow,
    }
    if locked:
        e["locked"] = True
    if msh is None:
        e["mesh"] = None
        e["material"] = None
    else:
        e["mesh"] = msh
        if prim_shape:
            e["primitive"] = prim(prim_shape)
        e["material"] = mat
    return e


def with_script(e, path, enabled=True, time_scale=1.0):
    e["script"] = {"path": path, "enabled": enabled, "timeScale": time_scale}
    return e


def build():
    ents = []

    # ---- 地面 ----
    ents.append(ent("Ground", (0.0, 0.0, 0.0), mesh("plane", 60.0, "demo_ground"),
                    vmat(M_GROUND), locked=True, shadow=False))

    # ---- 玩家 ----
    # ⚠ 实体原点是**脚底**（见 runtime_test/player_scene.json 的注释）。
    # `character` 段是 `player()` 内建的**唯一**判据：场景里第一个带
    # CharacterControllerComponent 的实体就是"玩家"。
    player = ent("Player", (0.0, 0.0, 0.0), mesh("cylinder", 1.0, "demo_player"),
                 vmat(M_SKIN), prim_shape="cylinder")
    player["primitive"] = {"kind": "cylinder", "size": 0.9, "radius": 0.35,
                           "segments": 16, "rings": 1}
    player["character"] = {
        "radius": 0.35, "halfHeight": 0.55, "stepHeight": 0.35,
        "slopeLimitDeg": 46.0, "groundSnap": True, "groundSnapDistance": 0.3,
        "gravityScale": 1.0, "maxSpeed": 4.5, "sprintMultiplier": 2.0,
        "jumpSpeed": 5.0, "acceleration": 40.0, "airControl": 0.25,
        "turnSpeed": 12.0, "pushStrength": 1.0, "enabled": True,
    }
    # 跟随相机。distance 拉到 18 m 是为了让**三个巡逻兵一起进画** ——
    # G1 的像素断言比的是"两帧之间画面变了多少"，机位太近只会看到一个。
    # ⚠ 玩家的朝向是 +X，相机在 **-X 一侧**（弹簧臂挂在身后）：摆在 -X 的
    #   东西全部在相机背后（第一次生成时三个巡逻兵就是这么"消失"的，
    #   日志里一条错都没有 —— 只有截图看得出）。
    player["camera"] = {
        "fov": 55.0, "priority": 10, "enabled": True,
        "targetOffset": [0.0, 0.0, 1.0],
        "distance": 18.0, "minDistance": 1.0,
        "collideWithWorld": False,
        "shakeAmplitude": 0.0, "shakeFrequency": 24.0,
    }
    ents.append(player)

    # ---- 3 个巡逻兵 ----
    # 三个都沿 X 巡逻（±6 m），y 各自错开 5 m：既能同时进画，又不会互相穿模。
    # Patrol_A 的 y=0 与玩家同轴，且 home 的 x=+4 —— 它一个来回就会**穿过**
    # 玩家（|x| < 1.6 的窗口），这正是 G2（撞到就扣血）需要的因果链。
    patrols = [("Patrol_A", 4.0, 0.0), ("Patrol_B", 6.0, 5.0),
               ("Patrol_C", 6.0, -5.0)]
    for name, x, y in patrols:
        e = ent(name, (x, y, 0.6), mesh("cube", 1.0, "demo_patrol"),
                vmat(M_CRATE), prim_shape="cube",
                scale=(0.8, 0.8, 1.2))
        with_script(e, "scripts/enemy_patrol.vks")
        ents.append(e)

    # ---- 2 扇定时门 ----
    # 都摆在玩家 3 m 以内（脚本的开门半径）→ 一进 Play 就开、2 秒后关。
    # 按**时刻**断言比按帧断言稳：帧率一变，帧号就全错了。
    doors = [("Door_A", 2.0, 0.0, 0.0), ("Door_B", 0.0, 2.5, 90.0)]
    for name, x, y, rz in doors:
        e = ent(name, (x, y, 1.0), mesh("cube", 1.0, "demo_door"),
                vmat(M_METAL), prim_shape="cube",
                rot=(0.0, 0.0, rz), scale=(0.2, 1.6, 2.0))
        with_script(e, "scripts/door_timer.vks")
        ents.append(e)

    # ---- 5 个拾取物：半径 1.2 m 的环 ----
    # 脚本的拾取半径是 1.5 m，所以玩家**不用动**就能全捡完 —— 这是刻意的：
    # 自动化没法可靠地"走两步"，而这一条要验的是 worldAttr 累加，不是走路。
    for i in range(5):
        a = math.radians(-90.0 + i * 72.0)
        x, y = round(1.2 * math.cos(a), 4), round(1.2 * math.sin(a), 4)
        e = ent("Pickup_%d" % i, (x, y, 0.4), mesh("sphere", 0.4, "demo_pickup"),
                vmat(M_CLOTH), prim_shape="sphere",
                scale=(0.4, 0.4, 0.4))
        with_script(e, "scripts/pickup.vks")
        ents.append(e)

    # ---- 记分板 / 计时挑战 ----
    director = ent("Director", (0.0, -3.0, 0.0), None, None)
    with_script(director, "scripts/hud_counter.vks")
    ents.append(director)

    doc = {
        "version": 1,
        "generator": "gen_script_demo.py (S6): 1 ground + player + 3 patrols "
                     "+ 2 doors + 5 pickups + 1 director, all logic in .vks",
        "camera": {
            "target": [0.0, 0.0, 1.0],
            "yaw": 0.0,
            "pitch": -18.0,
            "distance": 18.0,
            "fov": 55.0,
        },
        "light": {
            "direction": [0.42, 0.66, -0.62],
            "color": [1.0, 0.96, 0.9],
            "intensity": 3.2,
            "castsShadow": True,
            "ambientScale": 1.0,
        },
        "physics": {
            "enabled": True,
            "groundPlane": True,
            "fixedTimeStep": 0.0166666675,
            "maxSubSteps": 4,
            "allowSleep": True,
        },
        "entities": ents,
    }
    return doc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    doc = build()
    text = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
    if args.check:
        with open(OUT_ABS, encoding="utf-8") as f:
            same = f.read() == text
        print("up-to-date" if same else "DIFFERS")
        return 0 if same else 1
    os.makedirs(os.path.dirname(OUT_ABS), exist_ok=True)
    with open(OUT_ABS, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("written -> %s entities=%d" % (OUT_REL, len(doc["entities"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
