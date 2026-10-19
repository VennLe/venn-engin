#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线生成游戏 UI 的 **SDF 字体图集**（U1）。

## 为什么是"离线 + 提交产物"，而不是运行时读系统字体

现在编辑器 ImGui 的中文是从 `C:/Windows/Fonts/*.ttf` **合并**进来的
（`engine/ui/ImGuiManager.cpp`）。那条路有两个硬问题：

1. **打包即失效**：玩家机器上（尤其 Linux/macOS）没有那些路径 → 中文全方块；
2. **许可证**：`simhei/simsun/Deng` 都是不可再分发的商业字体，随游戏发出去
   = 侵权。

所以字形必须在**构建期**离线性化：TTF → SDF 距离场 → R8 图集 + JSON，
产物提交进 `assets/ui/fonts/`。运行时只读这两个文件。

字体源必须是 **OFL**（可再分发）。默认按这个顺序找：
  1. `--font` 参数
  2. `third_party/fonts/NotoSansSC-VF.ttf`（若仓库里放了）
  3. `C:/Windows/Fonts/NotoSansSC-VF.ttf`（本机装了 Noto Sans SC）

⚠ 只把 **图集**（PNG+JSON）当交付物提交，不提交 17 MB 的 VF 字体本体 ——
  图集本身就是"字体数据的图形化子集"，而 OFL 明确允许嵌入/派生出位图。

## 算法

每个字形：
  · 用 Pillow 以 `size` 号字渲染到 `ascent+descent` 高的格子里（左基线对齐）
  · 二值化（alpha >= 128）→ **精确欧氏距离变换**（Felzenszwalb 两遍 1D）
  · 有符号距离：形内为正、形外为负 → 归一化 `0.5 + d/(2*spread)` 到 0..1
    （0.5 = 轮廓线，这正是 shader 里 `smoothstep(0.5-w, 0.5+w, d)` 的约定）
  · shelf packing 到 2048² 的单页图集，每格留 `pad` 的边（否则相邻字形的
    距离场会在格子边界上互相污染 → 字边缘出现"相邻字的影子"）

## 用法

    python tools/gen_sdf_atlas.py                 # 默认参数
    python tools/gen_sdf_atlas.py --size 48 --spread 6
    python tools/gen_sdf_atlas.py --check         # 只打印统计，不写文件

产物：
    assets/ui/fonts/noto_sc_sdf.png    R8（单通道）
    assets/ui/fonts/noto_sc_sdf.json   字形表 / 度量 / 图集尺寸
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "ui", "fonts")
DICT_SRC = os.path.join(ROOT, "engine", "i18n", "ZhTable.cpp")   # U7 起词典在这里

# 词典之外还想收的字符：
#   · ASCII 0x20~0x7E（英文界面 / 数字 / 伤害数字）
#   · 中文标点（否则中文句子里的逗号句号是方块）
#   · 数字与单位（伤害数字、血量 "100/100"、"%"）
EXTRA_CHARS = ("，。！？：；、“”‘’（）《》【】…—·×÷°"
               "0123456789%+-./:<>[]()#@&*=")

FONT_CANDIDATES = [
    os.path.join(ROOT, "third_party", "fonts", "NotoSansSC-VF.ttf"),
    os.path.join(ROOT, "third_party", "fonts", "NotoSansSC-Regular.otf"),
    "C:/Windows/Fonts/NotoSansSC-VF.ttf",
    "C:/Windows/Fonts/NotoSansSC-Regular.otf",
]


# ============================================================
# 精确欧氏距离变换（Felzenszwalb & Huttenlocher, 1D 下包络）
# ============================================================

def _edt_1d(f: np.ndarray) -> np.ndarray:
    """一维平方距离变换：f 里 0 是种子，其余是 INF。"""
    n = f.shape[0]
    d = np.empty(n, dtype=np.float64)
    v = np.zeros(n, dtype=np.int32)      # 下包络的抛物线顶点位置
    z = np.zeros(n + 1, dtype=np.float64)  # 各段的分界
    k = 0
    v[0] = 0
    z[0] = -np.inf
    z[1] = np.inf
    for q in range(1, n):
        while True:
            s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.0 * q - 2.0 * v[k])
            if s <= z[k]:
                k -= 1
                if k < 0:
                    k = 0
                    break
                continue
            break
        k += 1
        v[k] = q
        z[k] = s
        z[k + 1] = np.inf
    k = 0
    for q in range(n):
        while z[k + 1] < q:
            k += 1
        dq = q - v[k]
        d[q] = dq * dq + f[v[k]]
    return d


def _edt_2d(seed: np.ndarray) -> np.ndarray:
    """二维平方距离（seed=True 处距离为 0）。"""
    inf = np.float64(1e20)
    f = np.where(seed, 0.0, inf)
    for x in range(f.shape[0]):
        f[x, :] = _edt_1d(f[x, :])
    for y in range(f.shape[1]):
        f[:, y] = _edt_1d(f[:, y])
    return f


def signed_distance(alpha: np.ndarray, spread: float) -> np.ndarray:
    """alpha(0..255) → 归一化有符号距离（0..1，0.5 = 轮廓）。

    用 **2×2 超采样后的二值**，再按下采样后的分辨率算距离？不 ——
    这里直接对 alpha>=128 的二值图做 EDT。抗锯齿信息在 SDF 里没有意义
    （SDF 本身就是"把抗锯齿搬到采样端"），二值化反而更干净。
    """
    mask = alpha >= 128
    if not mask.any():
        return np.zeros_like(alpha, dtype=np.float32)
    d_out = np.sqrt(_edt_2d(~mask))   # 形内像素到最近"形外"像素
    d_in = np.sqrt(_edt_2d(mask))     # 形外像素到最近"形内"像素
    signed = np.where(mask, d_out, -d_in)
    # 归一化：±spread 映射到 [0,1] 的两端（0.5 = 边界）
    return np.clip(0.5 + signed / (2.0 * spread), 0.0, 1.0).astype(np.float32)


# ============================================================
# 字符集
# ============================================================

def collect_chars() -> list[str]:
    """ASCII + 中文标点 + 词典里出现的全部非 ASCII 字符。"""
    chars = set(EXTRA_CHARS)
    for c in range(0x20, 0x7F):
        chars.add(chr(c))
    try:
        src = open(DICT_SRC, encoding="utf-8", errors="replace").read()
    except OSError:
        print("!! 找不到 %s —— 只能生成 ASCII 部分" % DICT_SRC)
        src = ""
    for ch in src:
        # 只收"看起来是该显示的字符"：ASCII 之外、且不是控制字符/代理对
        if ord(ch) > 127 and not (0xE000 <= ord(ch) <= 0xF8FF):
            chars.add(ch)
    return sorted(chars)


# ============================================================
# 生成
# ============================================================

def pick_font(explicit: str | None) -> str:
    if explicit:
        return explicit
    for p in FONT_CANDIDATES:
        if p and os.path.isfile(p):
            return p
    raise SystemExit("找不到 OFL 字体（NotoSansSC）。用 --font 指定。")


def main() -> int:
    ap = argparse.ArgumentParser(description="TTF → SDF 图集（R8）+ JSON")
    ap.add_argument("--font", default=None)
    ap.add_argument("--size", type=int, default=48, help="图集里的字号（像素）")
    ap.add_argument("--spread", type=float, default=6.0,
                    help="距离场半径（像素）：越大越能吃描边/外发光，图集也越大")
    ap.add_argument("--atlas", type=int, default=2048)
    # 高度可单独给：词典涨到 1059 个字符后 2048² 差一点点装不下（97.6% 处
    # 溢出），而 4096² 只用到 28% —— 多出来的空白既进 PNG（提交产物）也进
    # 显存。2048×3072 刚好装到 ~75%。图集尺寸由 JSON 里的 atlas.w/h 驱动，
    # 运行时**不假设 2 的幂**（Vulkan 的 sampled texture 本来就支持 NPOT）。
    ap.add_argument("--atlas-h", type=int, default=0,
                    help="图集高度；0 = 与 --atlas 相同（正方形）")
    ap.add_argument("--check", action="store_true", help="只统计，不写文件")
    args = ap.parse_args()

    font_path = pick_font(args.font)
    print("字体: %s" % font_path)
    try:
        font = ImageFont.truetype(font_path, args.size)
        try:
            font.set_variation_by_name("Regular")
        except Exception:
            pass    # 静态字体没有变体轴，忽略
    except OSError as e:
        print("!! 字体打不开: %s" % e)
        return 1

    ascent, descent = font.getmetrics()
    pad = int(math.ceil(args.spread)) + 2
    cell_h = ascent + descent + 2 * pad
    chars = collect_chars()
    print("字符数: %d（其中非 ASCII %d）"
          % (len(chars), sum(1 for c in chars if ord(c) > 127)))
    print("字号 %d，ascent=%d descent=%d，cell_h=%d，spread=%.1f"
          % (args.size, ascent, descent, cell_h, args.spread))

    # ---- 1) 逐字渲染 + 距离场 ----
    glyphs = {}
    packed = []          # (ch, sdf_array(w,h), adv, bearing_x)
    for ch in chars:
        try:
            adv = font.getlength(ch)
        except Exception:
            adv = float(args.size)
        advance = max(1, int(math.ceil(adv)))
        w = advance + 2 * pad
        h = cell_h

        img = Image.new("L", (w, h), 0)
        drw = ImageDraw.Draw(img)
        # anchor="ls" = 左基线。基线 y 放在 ascent+pad 处。
        drw.text((pad, pad + ascent), ch, font=font, fill=255, anchor="ls")
        alpha = np.array(img, dtype=np.uint8)
        if not alpha.any():
            # 空白字符（空格）：没有字形也要占一个格子，否则排版时查不到
            sdf = np.zeros((h, w), dtype=np.float32)
        else:
            sdf = signed_distance(alpha, args.spread)
        glyphs[ch] = {"adv": advance, "w": w, "h": h, "sdf": sdf}
        packed.append(ch)

    # ---- 2) shelf packing ----
    atlas_w = args.atlas
    atlas_h = args.atlas_h or args.atlas
    x = y = 0
    row_h = 0
    placed = {}
    for ch in packed:
        g = glyphs[ch]
        w, h = g["w"], g["h"]
        if x + w > atlas_w:
            x = 0
            y += row_h
            row_h = 0
        if y + h > atlas_h:
            print("!! 图集装不下：在 '%s' 处溢出（%.1f%% 已用）"
                  % (ch, 100.0 * (y * atlas_w) / (atlas_w * atlas_h)))
            return 1
        placed[ch] = (x, y)
        x += w
        row_h = max(row_h, h)

    used_h = y + row_h
    print("图集占用: %dx%d / %d（高度利用率 %.1f%%）"
          % (atlas_w, used_h, atlas_h, 100.0 * used_h / atlas_h))

    if args.check:
        return 0

    # ---- 3) 拼图 + 打包 ----
    atlas = np.zeros((atlas_h, atlas_w), dtype=np.float32)
    # 初始值 0 = "离轮廓无限远"？不 —— 0 表示"深在外部"，
    # 空区域采样出来是透明，正是我们要的。
    meta_glyphs = {}
    for ch, (gx, gy) in placed.items():
        g = glyphs[ch]
        sdf = g["sdf"]
        atlas[gy:gy + g["h"], gx:gx + g["w"]] = sdf
        meta_glyphs["%04X" % ord(ch)] = {
            "uv": [gx, gy, g["w"], g["h"]],
            "adv": g["adv"],
            # ⚠ bear.x 必须是 **-pad**，不能是 0：
            #   字形是画在 (pad, pad+ascent) 的（左基线），所以墨迹从格子左边
            #   往里 pad 才开始。若把格子左边缘直接对齐笔位，那 pad（=spread+2）
            #   就被当成"字间空白"重复计一次 → 48 号字上每个字宽多 16px，
            #   整句看起来字距散开。bear.x = -pad 让格子往左探出去，墨迹正好
            #   落在笔位上，推进仍按真实的 adv。
            "bear": [-pad, -(pad + ascent)],
        }

    os.makedirs(OUT_DIR, exist_ok=True)
    png_path = os.path.join(OUT_DIR, "noto_sc_sdf.png")
    json_path = os.path.join(OUT_DIR, "noto_sc_sdf.json")

    out = (np.clip(atlas, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    Image.fromarray(out, mode="L").save(png_path, optimize=True)

    meta = {
        "font": "Noto Sans SC",
        "license": "OFL-1.1",
        "size": args.size,
        "channels": 1,
        "sdfRange": args.spread,
        "atlas": {"w": atlas_w, "h": atlas_h},
        "metrics": {"ascent": ascent, "descent": descent, "lineGap": 0},
        "glyphCount": len(meta_glyphs),
        "glyphs": meta_glyphs,
    }
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, separators=(",", ":"))

    png_mb = os.path.getsize(png_path) / 1048576.0
    json_kb = os.path.getsize(json_path) / 1024.0
    print("写出: %s（%.2f MB）" % (png_path, png_mb))
    print("写出: %s（%.1f KB，%d 字形）" % (json_path, json_kb, len(meta_glyphs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
