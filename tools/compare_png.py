"""比较两张截图的差异（用于验证场景序列化前后渲染是否一致）。

用法：
    python tools/compare_png.py a.png b.png
"""

import sys

from PIL import Image, ImageChops, ImageStat


def main():
    if len(sys.argv) < 3:
        print("usage: compare_png.py <a.png> <b.png>")
        return 1

    a = Image.open(sys.argv[1]).convert("RGB")
    b = Image.open(sys.argv[2]).convert("RGB")
    if a.size != b.size:
        print("size mismatch:", a.size, b.size)
        return 1

    diff = ImageChops.difference(a, b)
    stat = ImageStat.Stat(diff)
    mean = sum(stat.mean) / 3.0
    extrema = max(max(ch) for ch in stat.extrema)

    # 逐像素统计"明显不同"的比例（任一通道差 > 16）
    gray = diff.convert("L")
    hist = gray.histogram()
    total = a.size[0] * a.size[1]
    changed = sum(hist[17:])
    print("size          : %s" % (a.size,))
    print("mean abs diff : %.3f / 255" % mean)
    print("max abs diff  : %d" % extrema)
    print("pixels >16    : %d / %d (%.3f%%)" % (changed, total,
                                                100.0 * changed / total))

    # 差异像素的包围盒：若只落在一小块（动画中的物体），
    # 就说明差异来自动画相位而非序列化本身
    mask = gray.point(lambda v: 255 if v > 16 else 0)
    bbox = mask.getbbox()
    print("diff bbox     : %s" % (bbox,))
    if bbox:
        bw = bbox[2] - bbox[0]
        bh = bbox[3] - bbox[1]
        print("bbox coverage : %dx%d (%.1f%% of frame area)"
              % (bw, bh, 100.0 * bw * bh / total))

    # 粗网格差异分布（8x6），一眼看出差异集中在哪些区域
    gx, gy = 8, 6
    print("diff grid (>16% of cell pixels):")
    for r in range(gy):
        row = []
        for c in range(gx):
            x0 = a.size[0] * c // gx
            x1 = a.size[0] * (c + 1) // gx
            y0 = a.size[1] * r // gy
            y1 = a.size[1] * (r + 1) // gy
            cell = gray.crop((x0, y0, x1, y1))
            ch = cell.histogram()
            ct = (x1 - x0) * (y1 - y0)
            pct = 100.0 * sum(ch[17:]) / ct
            row.append("%5.1f" % pct)
        print("   " + " ".join(row))
    return 0


if __name__ == "__main__":
    sys.exit(main())
