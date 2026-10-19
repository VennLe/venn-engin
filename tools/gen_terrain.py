#!/usr/bin/env python3
"""生成地形演示资产（W4-1）。

产物（全部确定：同一个 SEED 两次生成逐字节相同）：

    assets/terrain/hills/height.r32     32 位浮点高度，列主序，归一化 0..1
    assets/terrain/hills/splat.rgba8    4 层权重（R=grass G=dirt B=rock A=snow）
    assets/terrain/hills/terrain.vterrain

为什么高度是裸 .r32 而不是 16 位 PNG：16 位只有 65536 级，60 m 高度范围下
量化步长接近 1 mm，而"高度图 ↔ 物理"的验收判据是 1 cm —— 光量化就吃掉
10%。32 位浮点 257² 只有 264 KB。

为什么权重在一张 RGBA8 里：见 docs/world_scale_roadmap.md D5。

用法：
    python tools/gen_terrain.py            # 写资产
    python tools/gen_terrain.py --check    # 只统计不写（打印高度直方图）
"""

import argparse
import math
import os
import struct

SEED = 20261019
COLS = 257          # 采样点数（不是格数）
ROWS = 257
MPP = 2.0           # 米 / 采样间隔  → 512 m × 512 m
MAX_HEIGHT = 60.0   # 归一化高度 1.0 对应的米数

# 与 engine/terrain/Terrain.h 的 kChunkCells 一致：块边界必须落在采样点上
CHUNK_CELLS = 32

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT_DIR = os.path.join(REPO, "assets", "terrain", "hills")


# ---------------------------------------------------------------- 噪声
def _hash2(ix, iy):
    """整数格点 → [0,1) 的确定性伪随机。不用 random 是为了跨平台逐位一致。"""
    h = (ix * 374761393 + iy * 668265263 + SEED * 2246822519) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    h = h ^ (h >> 16)
    return (h & 0xFFFFFF) / float(0x1000000)


def _smooth(t):
    # smoothstep（3t²-2t³）：线性插值的格点接缝会留下明显的"菱形网格"，
    # 平滑后噪声看起来才像地形而不是数学作业。
    return t * t * (3.0 - 2.0 * t)


def value_noise(x, y):
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    v00 = _hash2(ix, iy)
    v10 = _hash2(ix + 1, iy)
    v01 = _hash2(ix, iy + 1)
    v11 = _hash2(ix + 1, iy + 1)
    sx, sy = _smooth(fx), _smooth(fy)
    a = v00 + (v10 - v00) * sx
    b = v01 + (v11 - v01) * sx
    return a + (b - a) * sy


def fbm(x, y, octaves=5, lacunarity=2.0, gain=0.5):
    total, amp, freq, norm = 0.0, 1.0, 1.0, 0.0
    for _ in range(octaves):
        total += value_noise(x * freq, y * freq) * amp
        norm += amp
        amp *= gain
        freq *= lacunarity
    return total / norm


def smoothstep(edge0, edge1, x):
    if edge1 <= edge0:
        return 0.0 if x < edge0 else 1.0
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def gauss(x, y, cx, cy, r, amp):
    d2 = (x - cx) ** 2 + (y - cy) ** 2
    return amp * math.exp(-d2 / (2.0 * r * r))


# ---------------------------------------------------------------- 高度
def height_at(xm, ym):
    """世界坐标（米，原点在中心）→ 归一化高度 [0,1]。"""
    # 三座山 + 一条缓谷：纯 fbm 会得到"处处差不多高"的毛毯，看不出地形。
    hills = (gauss(xm, ym, -110.0, 95.0, 78.0, 0.70) +
             gauss(xm, ym, 135.0, -65.0, 95.0, 0.58) +
             gauss(xm, ym, 40.0, 165.0, 70.0, 0.42) +
             gauss(xm, ym, -160.0, -140.0, 85.0, 0.30))
    # 两级细节：低频给"山脊/谷地"，高频给"石头与起伏"。
    # 单级 fbm 出来的地形像毛毯 —— 每个尺度都有能量才像山。
    detail = ((fbm(xm / 150.0, ym / 150.0) - 0.5) * 0.34 +
              (fbm(xm / 41.0, ym / 41.0, octaves=3) - 0.5) * 0.14)
    h = 0.14 + hills + detail

    # 原点周围 60 m 压平成一块台地：第一个真实关卡（W3）的木箱堆与房子
    # 都摆在原点附近，坡地上摆箱子会一半悬空。
    flat = gauss(xm, ym, 0.0, 0.0, 42.0, 1.0)
    h = h * (1.0 - 0.85 * flat) + 0.22 * 0.85 * flat

    return max(0.0, min(1.0, h))


def build_heights():
    out = []
    half = (COLS - 1) * 0.5 * MPP
    # 列主序：先沿 Y 走完一列
    for c in range(COLS):
        xm = c * MPP - half
        for r in range(ROWS):
            ym = r * MPP - half
            out.append(height_at(xm, ym))
    return out


# ---------------------------------------------------------------- 权重
def build_splat(heights):
    """4 层权重。返回 bytes（RGBA8，列主序，与高度一一对应）。"""
    out = bytearray(COLS * ROWS * 4)
    half = (COLS - 1) * 0.5 * MPP

    def h_at(c, r):
        c = max(0, min(COLS - 1, c))
        r = max(0, min(ROWS - 1, r))
        return heights[c * ROWS + r]

    for c in range(COLS):
        for r in range(ROWS):
            i = c * ROWS + r
            h = heights[i]
            # 坡度：中心差分，单位是米/米（乘 maxHeight 才有物理意义）
            hx = (h_at(c + 1, r) - h_at(c - 1, r)) * MAX_HEIGHT / (2.0 * MPP)
            hy = (h_at(c, r + 1) - h_at(c, r - 1)) * MAX_HEIGHT / (2.0 * MPP)
            slope = math.sqrt(hx * hx + hy * hy)  # tan(坡角)
            steep = smoothstep(0.55, 1.60, slope)

            # 权重设计：**草是默认覆盖**，其余三层是"抢占"。
            # 最早的写法把 w_grass 写成 (1 - h/0.55)，于是任何 >0.55 的地方
            # 一棵草都没有 —— 台地（h≈0.3）上土占 57%，整片地形是一张
            # 土黄色毯子，看不出"4 层混合"这件事。改成"先算出抢占量，
            # 剩下的都给草"，低处才是绿的。
            w_rock = steep ** 1.1
            w_snow = smoothstep(0.70, 0.92, h) * (1.0 - 0.6 * steep)
            # 土只出现在"有点坡度"的地方（雨水冲刷带），缓地上没有
            w_dirt = (1.0 - w_rock) * (1.0 - w_snow) * \
                smoothstep(0.06, 0.45, slope) * 0.55
            w_grass = max(0.0, 1.0 - w_rock - w_snow - w_dirt)

            s = w_grass + w_dirt + w_rock + w_snow
            if s <= 1e-6:
                w_grass = 1.0
                s = 1.0
            q = [w_grass / s, w_dirt / s, w_rock / s, w_snow / s]
            # 量化到 8 位并保证**四路之和恰好 255**：shader 里直接
            # dot(splat, weights) 而不再归一化，和不为 1 会让 albedo 整体
            # 偏亮/偏暗（低亮度区尤其明显）。
            q255 = [int(round(v * 255.0)) for v in q]
            q255[3] = max(0, min(255, 255 - q255[0] - q255[1] - q255[2]))
            out[i * 4 + 0] = q255[0]
            out[i * 4 + 1] = q255[1]
            out[i * 4 + 2] = q255[2]
            out[i * 4 + 3] = q255[3]
    return bytes(out)


# ---------------------------------------------------------------- 写盘
def vterrain_json():
    return """{
  "type": "terrain",
  "version": 1,
  "name": "hills",
  "cols": %d,
  "rows": %d,
  "metersPerPixel": %.1f,
  "maxHeight": %.1f,
  "splatRepeat": 1.0,
  "layerTiling": 6.0,
  "height": "height.r32",
  "splat": "splat.rgba8",
  "layers": [
    "materials/terrain_grass",
    "materials/terrain_dirt",
    "materials/terrain_rock",
    "materials/terrain_snow"
  ]
}
""" % (COLS, ROWS, MPP, MAX_HEIGHT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只统计，不写文件")
    args = ap.parse_args()

    assert (COLS - 1) % CHUNK_CELLS == 0, "块边界必须落在采样点上"
    assert (ROWS - 1) % CHUNK_CELLS == 0

    heights = build_heights()
    splat = build_splat(heights)

    lo, hi = min(heights), max(heights)
    mean = sum(heights) / len(heights)
    print("heights: n=%d min=%.4f max=%.4f mean=%.4f" %
          (len(heights), lo, hi, mean))
    buckets = [0] * 8
    for h in heights:
        buckets[min(7, int(h * 8))] += 1
    print("histogram(8 桶):", buckets)

    if args.check:
        return 0

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "height.r32"), "wb") as f:
        f.write(struct.pack("<%df" % len(heights), *heights))
    with open(os.path.join(OUT_DIR, "splat.rgba8"), "wb") as f:
        f.write(splat)
    with open(os.path.join(OUT_DIR, "terrain.vterrain"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(vterrain_json())

    print("written ->", OUT_DIR)
    print("  height.r32   %d bytes" % (len(heights) * 4))
    print("  splat.rgba8  %d bytes" % len(splat))
    print("  terrain.vterrain")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
