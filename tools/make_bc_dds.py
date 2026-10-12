#!/usr/bin/env python3
# ============================================================
# make_bc_dds.py —— 生成 BC 压缩 DDS 测试资产（引擎验证用）
#
# Pillow 只能"读" BC，不能"写" BC，所以这里用 numpy 手写一个
# 快速 BC1/BC3 编码器：
#   - 端点 = 块内像素逐通道 min/max（快速近似，验证足够）
#   - 端点先 565 量化再展开 8bit 插值，贴近硬件行为
#   - 强制 c0(565) > c1(565)，避开 3-color punchthrough 模式
# mip 链由 PIL LANCZOS 逐级减半离线生成（BC 不能运行时 blit）。
#
# 输出：
#   assets/textures/bc1_test.dds   (DXT1, 带 mip 链)
#   assets/textures/bc3_test.dds   (DXT5, 带 mip 链)
#   assets/textures/bc1_test.png   (未压缩对照，同一张图)
# ============================================================
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "textures")
os.makedirs(OUT, exist_ok=True)


# ------------------------------------------------------------
# 程序化测试图：细棋盘（mipmap/压缩差异在远处最明显）+ 对角色渐变
# ------------------------------------------------------------
def make_test_image(size=512):
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    cell = 4  # 高频棋盘
    board = ((x // cell).astype(np.int32) + (y // cell).astype(np.int32)) % 2
    grad = (x / size)  # 横向明暗渐变
    r = np.where(board == 0, 230.0, 30.0) * (0.55 + 0.45 * grad)
    g = np.where(board == 0, 225.0, 40.0) * (0.55 + 0.45 * grad)
    b = np.where(board == 0, 210.0, 70.0) * (0.55 + 0.45 * grad)
    img = np.dstack([r, g, b, np.full_like(r, 255.0)])
    return Image.fromarray(img.astype(np.uint8), "RGBA")


# ------------------------------------------------------------
# BC1 编码（numpy 向量化）
# 输入: (H, W, 4) uint8, H/W 为 4 的倍数
# 输出: (H//4 * W//4 * 8,) uint8
# ------------------------------------------------------------
def encode_bc1(rgba):
    h, w, _ = rgba.shape
    # 小 mip（<4px）也占一个 4x4 块：pad 到 4 倍数（DDS 大小公式
    # ceil(w/4)*ceil(h/4) 与 pad 后一致，pad 区不会被采样）
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if ph != h or pw != w:
        rgba = np.pad(rgba, ((0, ph - h), (0, pw - w), (0, 0)))
        h, w = ph, pw
    rgb = rgba[..., :3].astype(np.int32)
    # 切成 4x4 块：Bh x Bw x 4 x 4 x 3
    blocks = rgb.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4)
    flat = blocks.reshape(h // 4, w // 4, 16, 3)          # 每块 16 像素

    # 端点：块内逐通道 min / max
    lo = flat.min(axis=2)                                  # (Bh, Bw, 3)
    hi = flat.max(axis=2)

    def to565(c):
        r5 = (c[..., 0] * 31 + 127) // 255
        g6 = (c[..., 1] * 63 + 127) // 255
        b5 = (c[..., 2] * 31 + 127) // 255
        return r5, g6, b5

    def expand(r5, g6, b5):
        r8 = (r5 << 3) | (r5 >> 2)
        g8 = (g6 << 2) | (g6 >> 4)
        b8 = (b5 << 3) | (b5 >> 2)
        return np.stack([r8, g8, b8], axis=-1)

    # 保证 c0(565 值) > c1(565 值)（4-color 模式；相等时给 c0 进 1）
    r0, g0, b0 = to565(hi)
    r1, g1, b1 = to565(lo)
    v0 = (r0 << 11) | (g0 << 5) | b0
    v1 = (r1 << 11) | (g1 << 5) | b1
    need = v0 <= v1
    r0s, g0s, b0s = r0.copy(), g0.copy(), b0.copy()
    r1s, g1s, b1s = r1.copy(), g1.copy(), b1.copy()
    r0[need], r1[need] = r1s[need], r0s[need]
    g0[need], g1[need] = g1s[need], g0s[need]
    b0[need], b1[need] = b1s[need], b0s[need]
    v0 = (r0 << 11) | (g0 << 5) | b0
    v1 = (r1 << 11) | (g1 << 5) | b1
    # 全同色块（v0 == v1）会触发 3-color 模式 → c0 在 565 上进 1（g 段
    # 溢出进位到 r）。⚠ 只对 same 块生效 —— 端点分量是全图共享的数组，
    # 无脑 +1 会把非 same 块的调色板与已编码的 v0 撑错位。
    same = v0 == v1
    if same.any():
        g0n = np.where(same, g0 + 1, g0)             # 进位预备
        carry = g0n > 63
        r0n = np.where(carry, (r0 + 1) % 32, r0)
        g0n = np.where(carry, g0n % 64, g0n)
        v0 = np.where(same, (r0n << 11) | (g0n << 5) | b0, v0)
        r0, g0 = r0n, g0n                            # b0 不动

    c0 = expand(r0, g0, b0)   # (Bh, Bw, 3) 8bit 端点
    c1 = expand(r1, g1, b1)

    # 4 色调色板（按 spec 在展开后的 8bit 上插值）
    pal0 = c0
    pal1 = c1
    pal2 = (2 * c0 + c1) // 3
    pal3 = (c0 + 2 * c1) // 3

    # 每像素选最近调色板项 → 2bit 索引
    px = flat[:, :, :, None, :]               # (Bh, Bw, 16, 1, 3)
    pal = np.stack([pal0, pal1, pal2, pal3], axis=2)   # (Bh, Bw, 4, 3)
    dist = ((px - pal[:, :, None, :, :]) ** 2).sum(-1) # (Bh, Bw, 16, 4)
    idx = dist.argmin(axis=-1).astype(np.uint64)       # (Bh, Bw, 16)

    # 打包 2bit 索引：texel i 的位段在 [2i, 2i+1]
    shifts = (np.arange(16, dtype=np.uint64) * 2)
    indices = (idx << shifts).sum(axis=2)          # (Bh, Bw)

    v0u = v0.astype(np.uint64)
    v1u = v1.astype(np.uint64)
    words = (v0u | (v1u << np.uint64(16)) | (indices << np.uint64(32)))
    return words.view(np.uint16).reshape(h // 4, w // 4, 4)


# ------------------------------------------------------------
# BC3 = BC1 颜色 + BC4 alpha（alpha 端点 a0 < a1 = 8 线性插值）
# 输出: (H//4 * W//4 * 16,) uint8（alpha 块在前，颜色块在后）
# ------------------------------------------------------------
def encode_bc3(rgba):
    h, w, _ = rgba.shape
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if ph != h or pw != w:
        rgba = np.pad(rgba, ((0, ph - h), (0, pw - w), (0, 0)))
        h, w = ph, pw
    color = encode_bc1(rgba)                       # (Bh, Bw, 4) uint16

    alpha = rgba[..., 3].astype(np.int32)
    blocks_a = alpha.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3)
    fa = blocks_a.reshape(h // 4, w // 4, 16)

    a0 = fa.min(axis=2)                            # 低端点
    a1 = fa.max(axis=2)                            # 高端点
    # a0 < a1 → 8 级插值（spec: a0 > a1 时是 6 级 + 两个特殊值，避开）
    same = a0 >= a1
    a1 = np.where(same, a1 + 1, a1)                # 防相等（+1 封顶 255 以下）

    vals = np.arange(8, dtype=np.int32)
    pal = ((a1 - a0)[..., None] * vals // 7) + a0[..., None]   # (Bh, Bw, 8)
    # 8 个调色板值
    d = (fa[:, :, :, None] - pal[:, :, None, :]) ** 2           # (Bh,Bw,16,8)
    idx = d.argmin(axis=-1).astype(np.uint64)                   # (Bh, Bw, 16)

    shifts = (np.arange(16, dtype=np.uint64) * 3)
    bits = (idx << shifts).sum(axis=2)             # 48bit
    lo48 = (bits & ((1 << 48) - 1)).astype(np.uint64)
    hi16 = (bits >> np.uint64(48)).astype(np.uint64)

    a0u = a0.astype(np.uint16)
    a1u = a1.astype(np.uint16)
    alpha_words = np.empty((h // 4, w // 4, 4), dtype=np.uint16)
    alpha_words[..., 0] = a0u | (a1u << np.uint64(8)).astype(np.uint16)
    packed = np.stack(
        [lo48 & np.uint64(0xFFFF),
         (lo48 >> np.uint64(16)) & np.uint64(0xFFFF),
         (lo48 >> np.uint64(32)) & np.uint64(0xFFFF)],
        axis=-1).astype(np.uint16)                 # (Bh, Bw, 3)
    alpha_words[..., 1:4] = packed

    out = np.empty((h // 4, w // 4, 8), dtype=np.uint16)
    out[..., :4] = alpha_words
    out[..., 4:] = color
    return out


# ------------------------------------------------------------
# DDS 文件组装（老式头 + FourCC，带 mip 链）
# ------------------------------------------------------------
def write_dds(path, mips_data, w, h, fourcc):
    n = len(mips_data)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # CAPS|H|W|PIXELFORMAT|MIPCOUNT|LINEARSIZE
    pflags = 0x4                                          # DDPF_FOURCC
    header = struct.pack(
        "<7I44x32s5I",     # size,flags,h,w,pitch,depth,mipcount,res[11],ddspf(32B),caps,caps2-4,reserved2
        124, flags, h, w, 0, 0, n,
        struct.pack("<8I", 32, pflags, fourcc, 0, 0, 0, 0, 0),
        0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)              # TEXTURE|MIPMAP|COMPLEX
    body = b"".join(mip.tobytes() for mip in mips_data)
    with open(path, "wb") as f:
        f.write(struct.pack("<I", 0x20534444))
        f.write(header)
        f.write(body)
    print(f"wrote {path} ({w}x{h} levels={n} fmt={fourcc:#x})")


def mip_chain(img):
    """PIL LANCZOS 逐级减半到 1x1"""
    out = [img]
    while out[-1].size[0] > 1 or out[-1].size[1] > 1:
        w = max(1, out[-1].size[0] // 2)
        h = max(1, out[-1].size[1] // 2)
        out.append(out[-1].resize((w, h), Image.LANCZOS))
    return out


# ------------------------------------------------------------
# BC1 解码（自检用）：按 spec 还原 4 色调色板 + 2bit 索引
# ------------------------------------------------------------
def decode_bc1(words, w, h):
    """words: (Bh, Bw, 4) uint16 → (h, w, 4) uint8（pad 区为 0）"""
    bh, bw, _ = words.shape
    ph, pw = bh * 4, bw * 4

    def expand(v):     # 565 → 8bit 三元组
        r = ((v >> np.uint16(11)) & np.uint16(31)).astype(np.int32)
        g = ((v >> np.uint16(5)) & np.uint16(63)).astype(np.int32)
        b = (v & np.uint16(31)).astype(np.int32)
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4),
                         (b << 3) | (b >> 2)], axis=-1)

    c0 = expand(words[..., 0])   # (Bh,Bw,3)
    c1 = expand(words[..., 1])
    pal = np.stack([c0, c1, (2 * c0 + c1) // 3, (c0 + 2 * c1) // 3],
                   axis=2)       # (Bh,Bw,4,3)

    bits = np.stack([words[..., 2].astype(np.uint64),
                     words[..., 3].astype(np.uint64)], axis=-1)
    bits = (bits[:, :, 0] | (bits[:, :, 1] << np.uint64(16)))  # (Bh,Bw) 32bit
    texel = np.arange(16, dtype=np.uint64)
    idx = ((bits[..., None] >> (texel * np.uint64(2))) & np.uint64(3)).astype(np.int32)  # (Bh,Bw,16)

    ii = np.arange(bh)[:, None, None]
    jj = np.arange(bw)[None, :, None]
    out = pal[ii, jj, idx]                    # (Bh,Bw,16,3)
    # 布局重排：块内 texel 16 = (ty,tx) 行序，必须拆开并与块列维
    # 交换（encode 的 transpose 的逆操作），否则块顺序全错位
    img = out.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4)
    return img.reshape(ph, pw, 3)[:h, :w]


def decode_bc1_packed(mip_u16, w, h):
    return decode_bc1(mip_u16, w, h)


def main():
    img = make_test_image(512)
    img.save(os.path.join(OUT, "bc1_test.png"))
    chain = mip_chain(img)

    # BC1：每 mip 独立编码（4x4 块，尺寸均为 4 倍数）
    bc1_mips = []
    bc3_mips = []
    for m in chain:
        a = np.asarray(m, dtype=np.uint8)
        bc1_mips.append(encode_bc1(a))
        bc3_mips.append(encode_bc3(a))

    write_dds(os.path.join(OUT, "bc1_test.dds"), bc1_mips, 512, 512,
              0x31545844)                                    # DXT1
    write_dds(os.path.join(OUT, "bc3_test.dds"), bc3_mips, 512, 512,
              0x35545844)                                    # DXT5

    # 自检：内置 BC1 解码器回读 mip0，与原图比 PSNR
    dec = decode_bc1(bc1_mips[0], 512, 512).astype(np.float32)
    ref = np.asarray(img.convert("RGB"), dtype=np.float32)
    mse = ((dec - ref) ** 2).mean()
    psnr = 10 * np.log10(255 * 255 / max(mse, 1e-9))
    print(f"BC1 roundtrip PSNR = {psnr:.1f} dB")
    if psnr < 22.0:
        print("PSNR 太低，编码器可能有 bug", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
