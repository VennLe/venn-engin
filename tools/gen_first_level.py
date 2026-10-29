"""生成 W3 的"第一个真实关卡"（docs/world_scale_roadmap.md §8）。

**为什么生成而不是手写**：300 多个实体，手写一定出错（写错一个下标就整片
错位），而且改一次布局要重排几百行。生成器只有几十行，改布局改一个数就行。

三条纪律：
  1. **确定性**：固定种子（`random.Random(SEED)`）+ 所有遍历都走有序容器，
     同一份脚本跑两次必须逐字节相同。否则"场景往返逐字段相等"没法判。
  2. **命名有规则**：`Crate_<stack>_<i>_<j>_<k>`，用例直接按名字断言。
  3. **材质全部走 `.vmat`**：300+ 物体只引用 8 个母材质 —— 这是本阶段的核心
     验收动作（改一次 `.vmat`，全关卡都变）。

用法：
    python tools/gen_first_level.py            # 写到 assets/scenes/levels/
    python tools/gen_first_level.py --check    # 只校验已生成的文件是否与脚本一致
"""

import argparse
import json
import os
import random
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_REL = os.path.join("assets", "scenes", "levels", "first_level.json")
OUT_ABS = os.path.join(ROOT, OUT_REL)

SEED = 20261019

# ---- 8 个母材质（assets/materials/*.vmat）----
M_GROUND = "materials/ground.vmat"
M_CRATE = "materials/crate.vmat"
M_WALL = "materials/wall.vmat"
M_ROOF = "materials/roof.vmat"
M_METAL = "materials/metal.vmat"
M_GLASS = "materials/glass.vmat"
M_SKIN = "materials/skin.vmat"
M_CLOTH = "materials/cloth.vmat"

LEVEL_SIZE = 200.0       # 米
CRATE = 1.0              # 木箱边长（米）


def prim(kind, size=1.0, radius=0.5, segments=48, rings=24):
    """图元参数的**完整**形式。

    ⚠ 五个字段一个都不能省：写端（`primitiveParamsToJson`）总是把它们全写出来，
    省掉哪个，"生成 → 加载 → dump"就会多出哪个字段，往返比较永远差几条。
    """
    return {"kind": kind, "size": size, "radius": radius,
            "segments": segments, "rings": rings}


def vmat(path):
    """材质引用块。id 与 name 都取 `materials/xxx`，与 `.vmat` 文件里的 id 一致
    —— 改 .vmat 时两个引用会命中同一个 cached 材质（W2-1）。"""
    base = path[:-len(".vmat")]
    return {"kind": "vmat", "path": path, "name": base, "id": base}


def box(name, pos, size, mat, scale=None, shadow=True):
    """用单位立方体 + scale 摆一个盒子。

    ⚠ 用 scale 而不是给每个尺寸单独生成网格：图元网格只有一份，300 个盒子
      共享它 → 一个 draw call 的事。尺寸写进 primitive 会让每个盒子各自
      生成一份几何（顶点数乘 300）。
    """
    return {
        "name": name,
        "transform": {
            "position": [round(p, 4) for p in pos],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [round(s, 4) for s in (scale or size)],
        },
        "parent": -1,
        "visible": True,
        "castShadow": shadow,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                 "name": "cube"},
        "primitive": prim("cube", size=1.0),
        "material": mat,
    }


def build():
    rnd = random.Random(SEED)
    ents = []

    # ---- 地面：一片 200×200 的 plane（不是 40×40 块 tile）----
    # roadmap §8 给了两个选项。选整片：tile 会把 draw call 乘 1600，而地形的
    # 真正做法是 W4 的高度场，这里只是"能站住的地面"。
    ents.append({
        "name": "Ground",
        "transform": {"position": [0.0, 0.0, 0.0],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "parent": -1, "visible": True, "castShadow": False, "locked": True,
        "mesh": {"kind": "builtin", "shape": "plane", "size": LEVEL_SIZE,
                 "name": "level_ground"},
        "material": vmat(M_GROUND),
    })

    # ---- 6 个木箱堆：每堆 4×4 底 × 3 层 = 48 ----
    # ⚠ 层与层之间**略小于 1.0**（0.98）：完全贴合会让相邻面共面，阴影贴图
    #   的深度打架，看起来是一层脏点。
    # ⚠ 摆得比"200×200 米"想象的紧：默认机位（distance 26）下才看得清。
    #   摆到 ±70 时木箱只占屏幕几个像素 —— 那时候"改母材质画面变不变"
    #   根本测不出来（实测 sig=0.25%），而问题是**机位**不是材质。
    stacks = [(-16.0, -16.0), (16.0, -16.0), (-16.0, 16.0),
              (16.0, 16.0), (0.0, -24.0), (0.0, 24.0),
              (-32.0, 0.0), (32.0, 0.0)]
    for si, (sx, sy) in enumerate(stacks):
        for lay in range(3):
            for i in range(4):
                for j in range(4):
                    # 每一层少放几箱（确定性：按种子决定跳过哪些）
                    if lay > 0 and rnd.random() < 0.18:
                        continue
                    x = sx + (i - 1.5) * 1.06 + rnd.uniform(-0.03, 0.03)
                    y = sy + (j - 1.5) * 1.06 + rnd.uniform(-0.03, 0.03)
                    z = CRATE * 0.5 + lay * 0.98
                    ents.append(box("Crate_%d_%d_%d_%d" % (si, lay, i, j),
                                    (x, y, z), (CRATE, CRATE, CRATE),
                                    vmat(M_CRATE)))

    # ---- 一栋房（8×8×3 的室内 + 门洞 + 窗 + 屋顶）----
    hx, hy = -30.0, -30.0
    T = 0.3          # 墙厚
    H = 3.0          # 层高
    DOOR_W, DOOR_H = 1.6, 2.2

    # 地面板（室内地板，抬 2 cm 防止与地平面共面）
    ents.append(box("House_Floor", (hx, hy, 0.02), (8.0, 8.0, 0.04),
                    vmat(M_WALL)))

    # 南北墙（整面）
    for nm, (px, py), sc in (("House_Wall_N", (hx, hy + 4.0 - T * 0.5), (8.0, T, H)),
                             ("House_Wall_S", (hx, hy - 4.0 + T * 0.5), (8.0, T, H))):
        ents.append(box(nm, (px, py, H * 0.5), sc, vmat(M_WALL)))

    # 东墙（整面）
    ents.append(box("House_Wall_E", (hx + 4.0 - T * 0.5, hy, H * 0.5),
                    (T, 8.0, H), vmat(M_WALL)))

    # 西墙：留一个门洞 → 左段 + 右段 + 门楣
    side_w = (8.0 - DOOR_W) * 0.5
    ents.append(box("House_Wall_W_L",
                    (hx - 4.0 + T * 0.5, hy - (DOOR_W * 0.5 + side_w * 0.5),
                     H * 0.5),
                    (T, side_w, H), vmat(M_WALL)))
    ents.append(box("House_Wall_W_R",
                    (hx - 4.0 + T * 0.5, hy + (DOOR_W * 0.5 + side_w * 0.5),
                     H * 0.5),
                    (T, side_w, H), vmat(M_WALL)))
    ents.append(box("House_Lintel", (hx - 4.0 + T * 0.5, hy,
                                     (H + DOOR_H) * 0.5),
                    (T, DOOR_W, H - DOOR_H), vmat(M_WALL)))

    # 屋顶（比墙外扩 0.3 m，做成"压顶"）
    ents.append(box("House_Roof", (hx, hy, H + 0.15), (8.6, 8.6, 0.3),
                    vmat(M_ROOF)))

    # 两扇窗（玻璃；贴在南墙上，法线朝 -Y）
    for k, wy in enumerate((-2.0, 2.0)):
        ents.append(box("House_Window_%d" % k,
                        (hx - 2.0 + 4.0 * k, hy - 4.0 + 0.02, 1.8),
                        (1.6, 0.06, 1.2), vmat(M_GLASS), shadow=False))

    # 门框（金属，两根立柱）
    for k, dy in enumerate((-DOOR_W * 0.5, DOOR_W * 0.5)):
        ents.append(box("House_DoorFrame_%d" % k,
                        (hx - 4.0 + T * 0.5, hy + dy, DOOR_H * 0.5),
                        (0.34, 0.12, DOOR_H), vmat(M_METAL)))

    # ---- 室内点光（没有网格，所以只有 pointLight 块）----
    ents.append({
        "name": "House_Lamp",
        "transform": {"position": [hx, hy, 2.6],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "parent": -1, "visible": True, "castShadow": False,
        "mesh": None,
        "material": None,
        "pointLight": {"color": [1.0, 0.86, 0.68], "intensity": 8.0,
                       "range": 12.0, "enabled": True, "castsShadow": False},
    })

    # ---- 8 根金属路灯柱（沿中轴）----
    for k in range(8):
        y = -21.0 + k * 6.0
        ents.append(box("Pillar_%d" % k, (0.0, y, 1.5), (0.3, 0.3, 3.0),
                        vmat(M_METAL)))

    # ---- 两具"假人"道具：用掉 skin / cloth 两个母材质 ----
    for k, px in enumerate((-8.0, 8.0)):
        ents.append(box("Mannequin_%d_Body" % k, (px, 8.0, 0.9),
                        (0.6, 0.35, 1.4), vmat(M_CLOTH)))
        ents.append({
            "name": "Mannequin_%d_Head" % k,
            "transform": {"position": [px, 8.0, 1.9],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1, "visible": True, "castShadow": True,
            # ⚠ `mesh` 块与 `primitive` 块是**两套**东西（前者描述"哪份几何"，
            #   后者是编辑器回改用的意图）。球体的细分参数在 mesh 块里也要写，
            #   写端 `meshSourceToJson` 会原样吐回来（segments/rings > 0 时）。
            "mesh": {"kind": "builtin", "shape": "sphere", "size": 0.5,
                     "segments": 48, "rings": 24, "name": "sphere"},
            "primitive": prim("sphere", size=0.5, radius=0.25),
            "material": vmat(M_SKIN),
        })

    # ---- 角色（animate_girl_rigged.glb，播 Walk）----
    # ⚠ 蒙皮实体**没有 mesh 块**：模型根实体只带 `skinned`，几何由它自己
    #   加载（`skinnedFromJson` 里 `am.loadGLTF`）。给个 `mesh` 反而会让
    #   序列化出两条互相打架的来源。
    #
    # 换掉了原来那个 1181 个断壳的低模 `models/anime_girl.glb`
    # （2026-10-29）。新资产是 `E:\dev\UE-resources\Animate_Girl` 那个
    # 27394 顶点的雕塑 + Mixamo 65 骨骨架，由 `tools/rebuild_skin_weights.py`
    # 重写过权重并把根节点转成 Z-up。**`_rigged` 这个后缀不是可选的**：
    # 同目录的 `animate_girl.glb` 是没修权重、没转轴的半成品，喂进来会是
    # 一个躺在地上、四肢被拉长的角色。
    #
    # 四段 clip 名与 `LocomotionConfig` 逐字对齐（Idle/Walk/Run/Jump，
    # 大小写敏感），所以 `clipName: Walk` 这条不用改。
    ents.append({
        "name": "Player",
        "transform": {"position": [0.0, 0.0, 0.0],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "parent": -1, "visible": True, "castShadow": True,
        "mesh": None,
        "material": None,
        "skinned": {"kind": "gltf",
                    "path": "models/characters/animate_girl_rigged.glb",
                    "modelKey": "models/characters/animate_girl_rigged.glb",
                    "clipName": "Walk", "time": 0.0, "speed": 1.0,
                    "playing": True, "loop": True},
    })

    return ents


def build_scene():
    """返回完整的场景文档。

    太阳放在顶层 `light` 字段（场景级，不占实体）；给一个偏低的角度，
    让地面上的木箱有长影子 —— 影子是"这个关卡真的在跑 CSM"的唯一肉眼证据。
    """
    return {
        "version": 1,
        "generator": ("gen_first_level.py (W3): %d entities, all 8 .vmat "
                      "materials, seed=%d" % (0, SEED)),
        # ⚠ yaw / pitch 的单位是**弧度**：`Camera::setPitch` 直接
        #   `glm::clamp(p, -1.5533, 1.5533)`。写成度数（-18.0）会被夹到
        #   -89°，而且**只有把 dump 出来的数跟源文件比才看得出来** ——
        #   画面上只是"相机角度怪怪的"。
        # 机位：站在原点上方偏后，俯角 -16°，能同时看见木箱堆、灯柱和房子。
        # 改这个就等于改"验收时看到什么"，配合 D 组（改母材质看像素差）。
        # ⚠⚠ pitch 的符号是**反的**：负值把相机放到地面**下方** —— 地面的
        #   法线朝 +Z、单面渲染，于是整片地面被背面剔除，画面里物体全在
        #   "浮空"，而且**没有任何日志**提示相机在地下（只有截图看得出）。
        #   但 `applyCamera` 里 setPitch 之后 setTarget 会反推位置，所以
        #   真正的方向由 target 与 distance 决定；+0.28 才是"从上方俯视"。
        "camera": {"target": [0.0, 0.0, 1.2],
                   "yaw": 0.7853982, "pitch": 0.2792527,
                   "distance": 26.0, "fov": 55.0},
        # ⚠ 这里**不写** `physics` 块：写端只写"与默认值不同"的那几项
        #   （`SceneSerializer` 刻意如此，免得两份文件的 diff 被一整套默认
        #   物理参数撑爆）。生成器写进去、写端又省略 → 往返比较永远差一项。
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True, "ambientScale": 1.0},
        "entities": build(),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="只校验磁盘上的文件是否与脚本生成的结果一致")
    args = ap.parse_args()

    doc = build_scene()
    doc["generator"] = ("gen_first_level.py (W3): %d entities, all 8 .vmat "
                        "materials, seed=%d" % (len(doc["entities"]), SEED))
    text = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"

    if args.check:
        if not os.path.exists(OUT_ABS):
            print("缺少 %s" % OUT_REL)
            return 1
        with open(OUT_ABS, encoding="utf-8") as f:
            cur = f.read()
        if cur != text:
            print("结果: FAIL  %s 与生成器不一致（改了布局没重新生成？）"
                  % OUT_REL)
            return 1
        print("结果: PASS  %s 与生成器一致（%d 个实体）"
              % (OUT_REL, len(doc["entities"])))
        return 0

    os.makedirs(os.path.dirname(OUT_ABS), exist_ok=True)
    with open(OUT_ABS, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    names = [e["name"] for e in doc["entities"]]
    dup = len(names) - len(set(names))
    mats = {}
    for e in doc["entities"]:
        m = e.get("material")
        if isinstance(m, dict) and m.get("kind") == "vmat":
            mats[m["id"]] = mats.get(m["id"], 0) + 1
    print("写出 %s" % OUT_REL)
    print("  实体 %d 个（重名 %d 个）" % (len(names), dup))
    print("  材质引用：%s" % json.dumps(mats, ensure_ascii=False, sort_keys=True))
    return 0 if dup == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
