"""生成 `assets/scenes/anime_town.json` —— 引擎启动时打开的那张"二次元小镇"。

## 为什么是脚本而不是手写 JSON

场景里有 60+ 个实体（10 栋房子 × 每栋 6 件、树、路、地）。手写一是不
可能对齐，二是"把房子整体往右挪 2 m 再高一点"这种调整要做几十次编辑。
脚本里改一个数就重生成，等于给场景留了一份**可复算的源**。

## 风格上的三个决定

1. **屋顶用两块斜板拼人字顶**，不是一个旋转过的方盒。
   方盒转 45° 在俯视时是菱形、平视时是"斜的方块"，不像屋顶；两块斜板
   才有屋脊线和出檐。角度 30°~38° 之间，太陡像哥特、太缓像棚屋。

2. **墙面与屋顶的配色走"低饱和 + 高明度"**。
   二次元街景的通病是照真实色卡选，出来一片脏。这里用的是粉/米/浅蓝/
   薄荷绿这类高亮色，屋顶单点深色（砖红/藏青/墨绿）压住画面。

3. **每栋房子独立随机化，但用固定种子**。
   随机会让每次重生成都变样（"改完发现房子位置变了"），固定种子则
   既有错落感又可复现。

用法：
    python tools/make_anime_town.py           # 写 assets/scenes/anime_town.json
    python tools/make_anime_town.py --check   # 只打印统计，不写文件
"""

import argparse
import json
import math
import os
import random
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "scenes", "anime_town.json")

# 固定种子：错落感要，但不能"每次生成都不一样"。
SEED = 20261101


def mat(name, color, rough=0.62, metal=0.0, emissive=None):
    return {
        "kind": "procedural",
        "name": name,
        "baseColor": [color[0], color[1], color[2], 1.0],
        "metallic": metal,
        "roughness": rough,
        "normalScale": 0.0,
        "ao": 1.0,
        "emissive": list(emissive) if emissive else [0.0, 0.0, 0.0],
        "doubleSided": False,
        "alphaBlend": False,
    }


def ent(name, pos, scale=(1.0, 1.0, 1.0), rot=(0.0, 0.0, 0.0), mesh=None,
        material=None, parent=-1):
    e = {
        "name": name,
        "transform": {
            "position": [float(pos[0]), float(pos[1]), float(pos[2])],
            "rotation": [float(rot[0]), float(rot[1]), float(rot[2])],
            "scale": [float(scale[0]), float(scale[1]), float(scale[2])],
        },
        "parent": parent,
        "visible": True,
        "castShadow": True,
    }
    if mesh:
        e["mesh"] = mesh
    if material:
        e["material"] = material
    return e


def box_mesh(name, size=1.0):
    return {"kind": "builtin", "shape": "cube", "size": size, "name": name}


def plane_mesh(name, size):
    return {"kind": "builtin", "shape": "plane", "size": size, "name": name}


# ---------------------------------------------------------------- 调色板
# 墙面：高亮低饱和。挑 6 个轮换，避免整条街一个色。
#
# ⚠ 这些值**压过一次**（第一版 0.8~0.96，上屏全糊成纯白）。原因是场景里
#   没有强光源时自动曝光会把画面整体提亮，高亮底色直接顶到 1.0 ——
#   于是"米白 / 樱粉 / 浅蓝"在截图里是三块一样的白。
#   低于 0.8 才能让色相在 tonemap 之后还活着。
WALLS = [
    (0.74, 0.68, 0.60),   # 米白
    (0.80, 0.62, 0.65),   # 樱粉
    (0.60, 0.70, 0.82),   # 浅蓝
    (0.66, 0.78, 0.66),   # 薄荷
    (0.82, 0.75, 0.54),   # 奶油黄
    (0.70, 0.64, 0.78),   # 藕荷
]
ROOFS = [
    (0.62, 0.24, 0.22),   # 砖红
    (0.20, 0.26, 0.42),   # 藏青
    (0.24, 0.36, 0.30),   # 墨绿
    (0.46, 0.30, 0.20),   # 栗褐
    (0.34, 0.22, 0.38),   # 紫檀
]
TRIM = (0.55, 0.44, 0.34)          # 窗框 / 门框：木色
GLASS = (1.00, 0.86, 0.55)         # 窗玻璃：自发光暖黄（二次元的"灯"）
GROUND = (0.34, 0.48, 0.28)        # 草地
ROAD = (0.42, 0.40, 0.38)


def add_house(out, idx, x, y, w, d, h, rot_z, wall, roof, rng):
    """一栋二层小屋：墙体 + 人字顶 + 门 + 四扇窗。"""
    p = "House%02d" % idx
    ang = py = rot_z

    out.append(ent(p + "_wall", (x, y, h * 0.5), (w, d, h), (0.0, 0.0, ang),
                   box_mesh(p + "_wall_mesh"),
                   mat(p + "_wall_mat", wall, rough=0.75)))

    # ---- 人字顶：两块斜板 ----
    eave = 0.28                         # 出檐
    roof_h = w * rng.uniform(0.30, 0.42)  # 屋脊高 ≈ 半个进深的 60%~85%
    half = w * 0.5 + eave
    slope = math.atan2(roof_h, half)     # 坡面与水平面的夹角
    slab_len = math.hypot(half, roof_h) + 0.05
    slab_thk = 0.16

    # 绕 Z 旋转后，房子的局部 X 轴指向 (cos, sin)。两块斜板的中心要
    # 沿这个方向偏移，否则房子一转屋顶就和墙错开。
    ux, uy = math.cos(ang), math.sin(ang)

    for side in (-1.0, 1.0):
        # 局部 X 偏移：坡面中心在 ±half/2
        off = side * half * 0.5
        cx = x + ux * off
        cy = y + uy * off
        cz = h + roof_h * 0.5
        # 绕局部 Y 轴倾斜，让**靠屋脊那一端**抬起。
        #   绕 Y 转 θ：局部 X 轴 (1,0,0) → (cosθ, 0, -sinθ)
        #   θ > 0 时 +X 端**向下**，θ < 0 时 +X 端向上。
        #   · side = -1：坡面在 -X 侧、屋脊在 +X 端 → +X 端要抬 → θ = -slope
        #   · side = +1：坡面在 +X 侧、屋脊在 -X 端 → -X 端要抬 → θ = +slope
        # 也就是 θ = side * slope。
        #
        # ⚠ 第一版写的是 `-side * slope`，结果两片屋顶各朝外翻 → 拼成一个
        #   V 字，远看像"屋顶从中间塌下去、两端翘起来"。同一处符号错还会
        #   让屋脊线消失（两片不再在上方相接），一眼就能看出来。
        out.append(ent(
            "%s_roof_%s" % (p, "L" if side < 0 else "R"),
            (cx, cy, cz),
            (slab_len, d + eave * 2.0, slab_thk),
            (0.0, side * slope, ang),
            box_mesh("%s_roof_mesh" % p, 1.0),
            mat("%s_roof_mat" % p, roof, rough=0.68)))

    # ---- 门（朝 +Y 的正面）----
    door_w, door_h = 0.72, 1.55
    door_off = -w * 0.22                    # 偏一侧，别正中
    dx = x + ux * door_off - uy * (d * 0.5 + 0.03)
    dy = y + uy * door_off + ux * (d * 0.5 + 0.03)
    out.append(ent(p + "_door", (dx, dy, door_h * 0.5),
                   (door_w, 0.08, door_h), (0.0, 0.0, ang),
                   box_mesh(p + "_door_mesh"),
                   mat(p + "_door_mat", TRIM, rough=0.6)))

    # ---- 窗：一楼两扇、二楼两扇 ----
    win_w, win_h = 0.62, 0.72
    for row, wz in enumerate((h * 0.30, h * 0.72)):
        for col, sign in enumerate((-1.0, 1.0)):
            if row == 0 and sign < 0:
                continue                    # 一楼左侧让给门
            wx = x + ux * (sign * w * 0.26) - uy * (d * 0.5 + 0.04)
            wy = y + uy * (sign * w * 0.26) + ux * (d * 0.5 + 0.04)
            out.append(ent(
                "%s_win_%d%d" % (p, row, col),
                (wx, wy, wz), (win_w, 0.06, win_h), (0.0, 0.0, ang),
                box_mesh("%s_win_mesh" % p),
                mat("%s_win_mat" % p, GLASS, rough=0.25,
                    emissive=(0.30, 0.24, 0.12))))

    # ---- 烟囱（七成房子有）----
    if rng.random() < 0.7:
        chx = x + ux * (w * 0.28) - uy * (d * 0.18)
        chy = y + uy * (w * 0.28) + ux * (d * 0.18)
        out.append(ent(p + "_chimney", (chx, chy, h + roof_h * 0.75),
                       (0.30, 0.30, roof_h * 1.15), (0.0, 0.0, ang),
                       box_mesh(p + "_chimney_mesh"),
                       mat(p + "_chimney_mat", roof, rough=0.8)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只统计，不写文件")
    args = ap.parse_args()

    rng = random.Random(SEED)
    out = []

    # ---------------- 地面 ----------------
    out.append(ent("Ground", (0.0, 0.0, 0.0), (1.0, 1.0, 1.0),
                   mesh=plane_mesh("anime_town_ground", 260.0),
                   material=mat("anime_town_ground_mat", GROUND, rough=0.92)))

    # ---------------- 主路（一块块方砖拼，比一张长条更像路）----------------
    for i in range(26):
        t = i / 25.0
        # 沿 +Y 从 -26 铺到 +26，左右微摆做出"弯"的感觉
        px = math.sin(t * math.pi * 1.4) * 2.2
        py = -26.0 + t * 52.0
        out.append(ent("Road%02d" % i, (px, py, 0.012),
                       (3.2, 1.9, 0.02), (0.0, 0.0, math.cos(t * 5.0) * 0.06),
                       box_mesh("anime_town_road_mesh"),
                       mat("anime_town_road_mat", ROAD, rough=0.95)))

    # ---------------- 房子：沿路两侧错落排 ----------------
    # 手排而不是纯随机：位置刻意疏密不均（有的挨着、有的空出一块种树），
    # 高度也刻意参差 —— "错落有致"靠的就是这两条。
    plots = [
        # (x,     y,     w,    d,    h,    转角)
        (-7.2, -19.0, 3.6, 3.2, 3.4, 0.10),
        (-8.4, -12.0, 2.9, 3.6, 2.7, -0.06),
        (-6.6,  -6.0, 4.2, 3.0, 4.2, 0.04),
        (-9.0,   0.5, 3.1, 3.1, 3.0, 0.14),
        (-6.9,   7.0, 3.8, 3.4, 3.8, -0.10),
        (-8.2,  14.0, 3.3, 3.0, 2.9, 0.07),
        (7.0,  -16.0, 3.4, 3.3, 3.2, -0.12),
        (8.8,   -9.5, 4.5, 3.5, 4.6, 0.05),
        (6.4,   -3.0, 3.0, 3.6, 2.8, 0.11),
        (9.4,    3.5, 3.7, 3.2, 3.6, -0.04),
        (6.8,   10.0, 4.0, 3.4, 4.0, 0.08),
        (8.6,   17.5, 3.2, 3.0, 3.1, -0.09),
    ]
    for i, (x, y, w, d, h, rz) in enumerate(plots):
        add_house(out, i, x, y, w, d, h, rz,
                  WALLS[i % len(WALLS)], ROOFS[i % len(ROOFS)], rng)

    # ---------------- 树：填在空档里 ----------------
    tree_spots = [
        (-11.5, -15.5), (-12.0, -3.0), (-11.0, 10.5), (-3.2, -22.0),
        (11.6, -12.5), (12.4, 0.0), (11.2, 13.0), (3.0, 21.5),
        (-3.5, 3.0), (3.8, -8.0), (-4.0, 18.0), (4.2, 12.5),
    ]
    for i, (tx, ty) in enumerate(tree_spots):
        s = rng.uniform(2.2, 3.4)
        out.append(ent("Tree%02d" % i, (tx, ty, 0.0), (s, s, s),
                       (0.0, 0.0, rng.uniform(0.0, 6.28)),
                       {"kind": "builtin", "shape": "tree", "size": 1.0,
                        "name": "anime_town_tree%02d_mesh" % i},
                       mat("anime_town_tree%d_mat" % i,
                           (0.30, 0.48, 0.26), rough=0.85)))

    # 灌木：贴着房子根部，打破"房子直接插在草地上"的生硬感
    for i, (x, y, w, d, h, rz) in enumerate(plots):
        for k in range(2):
            s = rng.uniform(0.55, 0.85)
            out.append(ent(
                "Bush%02d_%d" % (i, k),
                (x + rng.uniform(-w * 0.7, w * 0.7),
                 y + rng.uniform(-d * 0.7, d * 0.7), s * 0.4),
                (s, s, s), (0.0, 0.0, rng.uniform(0.0, 6.28)),
                {"kind": "builtin", "shape": "rock", "size": 1.0,
                 "name": "anime_town_bush%02d_%d_mesh" % (i, k)},
                mat("anime_town_bush%d_%d_mat" % (i, k),
                    (0.34, 0.52, 0.28), rough=0.9)))

    doc = {
        "version": 1,
        "generator": "make_anime_town.py —— 二次元小镇（启动场景）",
        "camera": {
            # 街道纵深方向是 +Y，所以 yaw 取 90°（正对 +Y），退到 -Y 一侧
            # 26 m、抬高 8 m 略俯视 —— 一眼能看到整条街、两侧房子和天。
            "target": [0.0, 2.0, 3.0],
            "yaw": 1.5707963,
            "pitch": 0.30,
            "distance": 30.0,
        },
        "physics": {"enabled": True, "groundPlane": True},
        # 晴天 + 略多云。数值与 SkyParams::clearDay() 同源，只把云量调高
        # 一点 —— 空场上云的占比更大，少了会显得天空很空。
        "environment": {
            "zenith": [0.16, 0.36, 0.82],
            "horizon": [0.60, 0.76, 0.93],
            "ground": [0.40, 0.48, 0.36],
            "cloudColor": [1.0, 1.0, 1.0],
            "cloudCoverage": 0.52,
            "cloudThickness": 0.94,
            "cloudScale": 1.25,
            "cloudHeight": 900.0,
            "cloudSpeed": 0.012,
            "cloudShadow": 0.58,
            "cloudEdgeSharpness": 0.60,
            "exposure": 0.92,
            "starAmount": 0.0,
            "starBrightness": 0.0,
            "sunDirection": [0.522, -0.326, 0.788],
            "sunIntensity": 1.0,
            "sunAngularRadius": 0.017,
            "horizonGlow": 0.30,
        },
        "entities": out,
    }

    print("实体数 = %d（房子 %d 栋、树 %d、灌木 %d）"
          % (len(out), len(plots), len(tree_spots), len(plots) * 2))
    if args.check:
        return 0

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
    print("已写出 %s（%.1f KB）" % (OUT, os.path.getsize(OUT) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
