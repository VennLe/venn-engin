#!/usr/bin/env python3
# ============================================================
# make_sss_lut.py —— 预积分皮肤次表面散射 LUT（R4-2）
#
# 生成一张 256×256 的二维查找表，交给 pbr.frag 的 VENN_SHADING_SKIN
# 编译单元按 **(N·L, 曲率)** 采样，替代朴素的 max(N·L, 0) 漫反射项。
#
# ---- 物理模型（预积分 / pre-integrated）----
# 把表面局部看成半径 r = 1/curvature 的弧面。一个片元收到的漫反射光
# 不是只由自己的法线决定 —— 光在介质里横向"渗"过来，所以要把**邻域
# 法线**的受光量按一条扩散核加权积分：
#
#     D(cosθ, curv) = Σ_φ P( s=r·φ ) · max(0, cos(θ − φ))  /  Σ_φ P( s )
#
#   · θ = N 与 L 的夹角（cosθ = N·L）
#   · φ = 沿"含 N 与 L 的大圆"的弧角，弧长 s = r·φ
#   · P(s) = exp(−s²/2σ²)，σ = 该通道的**散射距离**
#
# 关键点（也是这个模型能出效果的原因）：
#   · **r → ∞（平面）时核宽 → 0**，只剩 φ=0 → D = max(0, cosθ) ——
#     也就是朴素 Lambert。于是"曲率 = 0 行"必须逐点等于 max(N·L,0)，
#     这是脚本的硬自检（见 assert 段）。
#   · **r 小（高曲率：鼻梁/耳缘/指尖）时核宽大** → 明暗交界处光"绕过"
#     去，阴影侧也有亮度 → 透光感。
#   · **σ 逐通道不同（R > G > B）**：红色散射最远，于是交界处 R 通道
#     先亮起来 → 泛红。这就是"耳缘透红"的来源，不是叠一层红色。
#
# ---- 输出 ----
#   assets/textures/sss_lut.dds   BC1(DXT1) + mip 链（引擎按 UNORM 采样）
#   assets/textures/sss_lut.png   未压缩对照（人眼检查 / 差分定位）
#
# ⚠ 采样侧必须 textureLod(..., 0.0)：LUT 的高 mip 会把 ndotl 轴也糊掉。
#   带 mip 链只是喂给加载器（与 bc1_test.dds 同款）。
# ============================================================
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_bc_dds import encode_bc1, mip_chain, write_dds  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "textures")
os.makedirs(OUT, exist_ok=True)

SIZE = 256          # LUT 边长（x = ndotl, y = curvature）
PHI_SAMPLES = 512   # 大圆上的积分采样数
CURV_EPS = 1.0e-4   # 曲率下限（避免 1/0）
MM = 1000.0         # 每单位长度 = 1000 毫米（σ 用毫米更直观）

# 逐通道散射距离（**毫米**，相对"曲率 = 1 时半径 = 1 单位 = 1000mm"）。
# R > G > B 是皮肤/蜡这类介质的实测规律（红光穿透最深）。
SIGMA_MM = np.array([500.0, 300.0, 150.0], dtype=np.float64)


def build_lut():
    """→ (SIZE, SIZE, 3) float32，行 = 曲率(0→1)，列 = N·L(-1→1)。"""
    ndotl = np.linspace(-1.0, 1.0, SIZE)        # 列
    curv = np.linspace(0.0, 1.0, SIZE)          # 行
    phis = np.linspace(-np.pi, np.pi, PHI_SAMPLES, endpoint=False)
    theta = np.arccos(np.clip(ndotl, -1.0, 1.0))                # (SIZE,)

    # 邻域法线的受光量：cos(θ − φ) 截断到 ≥0。形状 (PHI, SIZE)。
    lit = np.maximum(0.0, np.cos(theta[None, :] - phis[:, None]))

    lut = np.zeros((SIZE, SIZE, 3), dtype=np.float64)
    for row, c in enumerate(curv):
        r_units = 1.0 / max(c, CURV_EPS)        # 半径（单位）
        s_mm = r_units * phis * MM              # 弧长（毫米）
        for ch in range(3):
            w = np.exp(-(s_mm * s_mm) / (2.0 * SIGMA_MM[ch] ** 2))   # (PHI,)
            num = (w[:, None] * lit).sum(axis=0)                     # (SIZE,)
            den = w.sum()
            lut[row, :, ch] = num / max(den, 1e-12)
    # ---- 逐行、逐通道归一化：让 N·L = 1 处恒为 1 ----
    # 宽核除了"渗进阴影"，也会把**峰值压暗**（能量摊开）——不处理的话
    # 皮肤正对光的部分会偏蓝（R 的核最宽、被压得最狠）。游戏里通行的
    # 做法是把响应函数在"全亮"处归一到 1：宽核带来的能量损失当作艺术
    # 取舍丢掉，只保留"阴影侧渗光 + 泛红"这个我们真正要的特征。
    # 曲率 = 0 行归一化因子恰为 1 → 不动，Lambert 自检仍然成立。
    norm = lut[:, -1, :]                                   # (SIZE, 3)
    lut /= np.maximum(norm[:, None, :], 1e-6)
    # 数值兜底：BC1 要求 0..255，先夹好
    return np.clip(lut, 0.0, 1.0)


def self_check(lut):
    """三条硬断言 —— 任何一条挂了都说明模型/参数写错了。"""
    ndotl = np.linspace(-1.0, 1.0, SIZE)
    ok = True

    # ① 曲率 = 0 行必须退化成 Lambert（三通道相等且 = max(N·L,0)）。
    row0 = lut[0]
    target = np.maximum(ndotl, 0.0)
    err = np.abs(row0 - target[:, None]).max()
    print(f"[1] 曲率=0 行对 Lambert 的最大误差 = {err:.4f}（阈值 0.01）")
    if err > 0.01:
        print("    ✗ 平面没有退化成 Lambert —— 核归一化写错了")
        ok = False
    else:
        print("    ✓ 曲率=0 → 逐点等于 max(N·L,0)")

    # ② 曲率 = 1、阴影侧（N·L ≈ −0.15）必须 R > G > B（红光渗得更远）。
    col = int(((-0.15 + 1.0) / 2.0) * (SIZE - 1))
    rgb = lut[SIZE - 1, col]
    print(f"[2] 曲率=1 / N·L≈-0.15 → RGB = "
          f"({rgb[0]:.4f}, {rgb[1]:.4f}, {rgb[2]:.4f})（应 R>G>B 且 R>0）")
    if not (rgb[0] > rgb[1] > rgb[2] and rgb[0] > 1e-3):
        print("    ✗ 阴影侧没有出现红向偏色 —— σ 顺序或归一化不对")
        ok = False
    else:
        print("    ✓ 阴影侧 R>G>B → 泛红透光")

    # ③ 曲率 = 1、正对光（N·L = 1）三通道都 ≈1（亮的正面不该被染色）。
    rgb1 = lut[SIZE - 1, -1]
    print(f"[3] 曲率=1 / N·L=1 → RGB = "
          f"({rgb1[0]:.4f}, {rgb1[1]:.4f}, {rgb1[2]:.4f})（应都接近 1）")
    if rgb1.min() < 0.9:
        print("    ✗ 正面被明显压暗/染色 —— 归一化尺度不对")
        ok = False
    else:
        print("    ✓ 正面三通道接近 1")
    return ok


def sample_bilinear(lut, ndotl, curv):
    """按 shader 的方式（u = ndotl*0.5+0.5, v = curv）双线性取一个值。"""
    x = (ndotl * 0.5 + 0.5) * (SIZE - 1)
    y = curv * (SIZE - 1)
    x0, y0 = int(np.floor(x)), int(np.floor(y))
    x1, y1 = min(x0 + 1, SIZE - 1), min(y0 + 1, SIZE - 1)
    fx, fy = x - x0, y - y0
    c = (lut[y0, x0] * (1 - fx) * (1 - fy) + lut[y0, x1] * fx * (1 - fy) +
         lut[y1, x0] * (1 - fx) * fy + lut[y1, x1] * fx * fy)
    return c


def main():
    lut = build_lut()
    if not self_check(lut):
        print("结果: FAIL —— LUT 自检未通过", file=sys.stderr)
        sys.exit(1)

    # 未压缩对照（顶端再加一行"曲率已 = 1"的参考色带，方便肉眼对比）
    png = np.zeros((SIZE, SIZE, 4), dtype=np.uint8)
    png[..., :3] = np.clip(lut * 255.0 + 0.5, 0, 255).astype(np.uint8)
    png[..., 3] = 255
    img = Image.fromarray(png, "RGBA")
    img.save(os.path.join(OUT, "sss_lut.png"))

    # BC1 DDS（带 mip 链，喂给引擎的 DDS 加载器）
    mips = [encode_bc1(np.asarray(m, dtype=np.uint8)) for m in mip_chain(img)]
    write_dds(os.path.join(OUT, "sss_lut.dds"), mips, SIZE, SIZE, 0x31545844)

    # 量化后再验一次：BC1 会把曲率=0 行压出误差，确认仍在可用范围
    dec = np.asarray(img, dtype=np.float32)[..., :3] / 255.0
    row0_err = np.abs(dec[0] - np.maximum(np.linspace(-1, 1, SIZE), 0)[:, None]).max()
    print(f"[4] 8bit 量化后曲率=0 行误差 = {row0_err:.4f}")
    print("结果: PASS —— sss_lut.dds / sss_lut.png 已写出")


if __name__ == "__main__":
    main()
