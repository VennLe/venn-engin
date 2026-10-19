#!/usr/bin/env python3
"""生成 W4 的地形探针场景（docs/world_scale_roadmap.md §9）。

场景内容：
  · 一块 `terrain/hills/terrain.vterrain`（512 m × 512 m，起伏 0..52 m）
  · 台地上 6 个木箱 + 3 处坡上的石墩（用来验证"地形收到阴影"与"坡度法线"）
  · 一台俯瞰的相机

**为什么生成而不是手写**：木箱/石墩的 z 坐标必须落在高度场上（否则它们
要么悬空、要么埋进山里）。高度来自 `height.r32` 的双线性插值 —— 手算
6 个点就会错，而且改一次地形就得全部重算。

三条纪律（与 gen_first_level.py 相同）：
  1. 确定性：无随机数（全部显式坐标），同一份脚本跑两次逐字节相同
  2. 材质只引用 `.vmat`
  3. 与写端的规范化形状一致：mesh 写全 5 字段、material 块写 kind/path/name/id

用法：
    python tools/gen_terrain_probe.py
"""

import argparse
import json
import math
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TERRAIN_REL = os.path.join("assets", "terrain", "hills")
OUT_REL = os.path.join("assets", "scenes", "levels", "terrain_probe.json")
OUT_ABS = os.path.join(ROOT, OUT_REL)

VT_META = "terrain/hills/terrain.vterrain"
M_CRATE = "materials/crate.vmat"
M_WALL = "materials/wall.vmat"


# ---------------------------------------------------------------- 高度场
class Heightfield:
    """与 physics/Heightfield.cpp 的 heightAt **逐行同构**的双线性插值。

    ⚠ 这里多写一份是为了让生成器不依赖 C++。一旦两侧的插值约定漂移，
      生成出来的箱子就会悬空/陷入几十厘米 —— 而那是"看起来差不多"的错误。
    """

    def __init__(self, path, cols, rows, mpp, max_height):
        with open(path, "rb") as f:
            raw = f.read()
        assert len(raw) == cols * rows * 4, "height.r32 尺寸不对"
        self.h = struct.unpack("<%df" % (cols * rows), raw)
        self.cols, self.rows = cols, rows
        self.mpp, self.max_h = mpp, max_height
        self.ox = -0.5 * (cols - 1) * mpp
        self.oy = -0.5 * (rows - 1) * mpp

    def raw(self, c, r):
        c = min(max(c, 0), self.cols - 1)
        r = min(max(r, 0), self.rows - 1)
        return self.h[c * self.rows + r]

    def at(self, x, y):
        gc = min(max((x - self.ox) / self.mpp, 0.0), float(self.cols - 1))
        gr = min(max((y - self.oy) / self.mpp, 0.0), float(self.rows - 1))
        c0, r0 = int(math.floor(gc)), int(math.floor(gr))
        fc, fr = gc - c0, gr - r0
        if c0 >= self.cols - 1:
            c0, fc = self.cols - 2, 1.0
        if r0 >= self.rows - 1:
            r0, fr = self.rows - 2, 1.0
        h00, h10 = self.raw(c0, r0), self.raw(c0 + 1, r0)
        h01, h11 = self.raw(c0, r0 + 1), self.raw(c0 + 1, r0 + 1)
        a = h00 + (h10 - h00) * fc
        b = h01 + (h11 - h01) * fc
        return (a + (b - a) * fr) * self.max_h

    def slope_deg(self, x, y):
        """中心差分算法线 → 与水平面夹角（度）。用来确认箱子没摆在陡壁上。"""
        d = self.mpp * 0.5
        hx = (self.at(x + d, y) - self.at(x - d, y)) / (2.0 * d)
        hy = (self.at(x, y + d) - self.at(x, y - d)) / (2.0 * d)
        return math.degrees(math.atan(math.hypot(hx, hy)))


def prim(kind, size=1.0, radius=0.5, segments=48, rings=24):
    return {"kind": kind, "size": size, "radius": radius,
            "segments": segments, "rings": rings}


def vmat(path):
    base = path[: -len(".vmat")]
    return {"kind": "vmat", "path": path, "name": base, "id": base}


def box(name, pos, scale, mat, shadow=True):
    return {
        "name": name,
        "transform": {
            "position": [round(p, 4) for p in pos],
            "rotation": [0.0, 0.0, 0.0],
            "scale": [round(s, 4) for s in scale],
        },
        "parent": -1,
        "visible": True,
        "castShadow": shadow,
        "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0, "name": "cube"},
        "primitive": prim("cube", size=1.0),
        "material": mat,
    }


def build():
    hf = Heightfield(os.path.join(TERRAIN_REL, "height.r32"), 257, 257, 2.0, 60.0)

    ents = []
    # ---- 地形本体 ----
    # transform 写单位阵：地形已经以原点为中心（.vterrain 里的采样点
    # (0,0) 对应世界 (-size/2, -size/2)，见 TerrainAsset::heightfield）。
    ents.append({
        "name": "Terrain",
        "transform": {"position": [0.0, 0.0, 0.0],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "parent": -1,
        "visible": True,
        "castShadow": False,
        "locked": True,
        # ⚠ `mesh` / `material` 显式写 null：地形**没有网格组件**，而序列化
        #   器对"没有网格的实体"就是吐 `"mesh": null, "material": null`
        #   （`SceneSerializer.cpp` 里那两行是无条件的）。不写这两个键，
        #   `--dump-scene` 的往返就会永远差这一处 —— 与 first_level.json 里
        #   灯 / 角色根实体同一个写法。
        "mesh": None,
        "material": None,
        "terrain": {
            "source": VT_META,
            "visible": True,
            "heightScale": 1.0,
            "layers": ["", "", "", ""],
        },
    })

    # ---- 台地上的木箱（z 落在高度场上）----
    # 台地中心 0.22*0.85 ≈ 0.19 → 约 11 m 高，所以箱子在 11 m 附近。
    crate_spots = [(-2.0, -2.0), (2.2, -1.6), (-1.4, 2.4), (2.6, 2.8),
                   (0.4, 5.2), (-0.6, -5.4)]
    for i, (x, y) in enumerate(crate_spots):
        z = hf.at(x, y)
        ents.append(box("TerrainCrate_%d" % i, (x, y, z + 0.5),
                        (1.0, 1.0, 1.0), vmat(M_CRATE)))

    # ---- 坡上的石墩：验证"陡坡上是岩石层、箱子收到地形投影" ----
    pillars = [(-34.0, -26.0), (38.0, 20.0), (12.0, 46.0)]
    for i, (x, y) in enumerate(pillars):
        z = hf.at(x, y)
        ents.append(box("TerrainRock_%d" % i, (x, y, z + 1.1),
                        (1.6, 1.6, 2.2), vmat(M_WALL)))

    # ---- 相机：从台地外侧俯瞰 ----
    # target 放在台地顶面略上方，distance 让整块地形与两座山都进画。
    camera = {
        "target": [0.0, 0.0, round(hf.at(0.0, 0.0) + 3.0, 3)],
        "yaw": 0.62,
        "pitch": 0.30,
        "distance": 52.0,
        "fov": 55.0,
    }
    light = {
        "direction": [0.45, 0.70, -0.62],
        "color": [1.0, 0.96, 0.9],
        "intensity": 3.2,
        "castsShadow": True,
        "ambientScale": 1.0,
    }
    doc = {
        "version": 1,
        "generator": "gen_terrain_probe.py (W4): terrain/hills 512m, "
                     "6 crates + 3 rocks on the heightfield",
        "camera": camera,
        "light": light,
        "entities": ents,
    }

    # 自检：箱子不能落在陡坡上（>25° 就会看起来"斜插进山里"）
    for i, (x, y) in enumerate(crate_spots):
        s = hf.slope_deg(x, y)
        if s > 25.0:
            print("[warn] crate %d at (%.1f,%.1f) slope=%.1f deg" % (i, x, y, s))
    print("terrain height at origin = %.2f m" % hf.at(0.0, 0.0))
    print("camera target z = %.2f" % camera["target"][2])
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
    print("written ->", OUT_REL, "entities=", len(doc["entities"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
