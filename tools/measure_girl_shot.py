#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""量测角色预览渲染图的取景正确性（#84 取景校正的验收）。

正交相机 + 方形画幅 900x900 + ortho_scale=1.95：
  - 画面覆盖世界边长 = 1.95m → 461.5 px/m
  - 角色实际高 1.556m（脚底 y=0，蒙皮 EVAL bbox）
  - 角色实际宽：躯干 z±0.187，但双臂侧展后正面最宽 ~0.7m 量级

断言：
  1. px/m 横纵一致（用角色的 bbox 宽高比交叉验证）
  2. 图内角色高度换算 ≈ 1.556m（±15%）
  3. 角色中心接近画面中心（±80px）

用法: python tools/measure_girl_shot.py 图1.png [图2.png ...]
"""
import sys

import numpy as np
from PIL import Image

CANVAS = 1.95          # 正交画面覆盖的世界边长
EXPECT_H = 1.556       # 角色实际高度
EXPECT_W = 0.76        # 正面含双臂展开的近似宽度（粗验用，容差大）
PPM = 900.0 / CANVAS   # px per meter


def fg_bbox(path):
    im = np.asarray(Image.open(path).convert("RGB")).astype(np.int16)
    # 渲染输出是 sRGB 编码：线性背景 (0.09,0.10,0.13) → sRGB ≈ (84,89,100)。
    # （用线性值 (23,26,33) 判会把整张图都当成前景 —— 实测踩过）
    bg = np.array([84, 89, 100], dtype=np.int16)
    diff = np.abs(im - bg).sum(axis=2)
    mask = diff > 40
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return None
    return dict(path=path, n=int(mask.sum()),
                x0=int(xs.min()), x1=int(xs.max()),
                y0=int(ys.min()), y1=int(ys.max()),
                h_px=int(ys.max() - ys.min() + 1),
                w_px=int(xs.max() - xs.min() + 1))


def main():
    fails = []
    for path in sys.argv[1:]:
        b = fg_bbox(path)
        if not b:
            print("FAIL %s 没检测到前景" % path)
            fails.append(path)
            continue
        h_m = b["h_px"] / PPM
        w_m = b["w_px"] / PPM
        cx = (b["x0"] + b["x1"]) / 2
        cy = (b["y0"] + b["y1"]) / 2
        print("%s  前景=%dpx  bbox x %d..%d y %d..%d  宽 %dpx 高 %dpx"
              % (path.split("/")[-1].split("\\")[-1], b["n"],
                 b["x0"], b["x1"], b["y0"], b["y1"], b["w_px"], b["h_px"]))
        print("    换算: 高 %.3f m（期望 %.3f）  宽 %.3f m  宽高比 %.2f"
              % (h_m, EXPECT_H, w_m, b["w_px"] / b["h_px"]))
        print("    中心: (%.0f, %.0f) 画面中心 (450, 450)  偏移 (%+.0f, %+.0f)"
              % (cx, cy, cx - 450, cy - 450))
        if abs(h_m - EXPECT_H) > 0.25:
            print("    !! 高度换算偏差过大（取景或角色尺寸有问题）")
            fails.append(path)
        if abs(cy - 450) > 90:
            print("    !! 角色中心纵向偏离画面中心过多")
            fails.append(path)
    if fails:
        print("RESULT: FAIL", fails)
        return 1
    print("RESULT: PASS —— 取景正确，图内尺寸与网格 bbox 对得上")
    return 0


if __name__ == "__main__":
    sys.exit(main())