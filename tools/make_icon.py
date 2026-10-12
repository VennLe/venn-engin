"""从 design/venn_logo_source.png 重建图标资产（可复现，坑点 #55 配套）。

背景（2026-10-11 用户反馈"图标分辨率低、看着糊"）：

    旧流程把 1024 的设计源图先抠成 **512** 的透明主档（venn_icon.png），
    再从这512 生成 7 档 ICO（16/24/32/48/64/128/256）。于是 256 那一档是
    "1024 → 512 → 256" **两次**降采样，细节在半路就丢了；而且 7 档盖不住
    125%/175% 这类缩放（SM_CXICON 会变成 40/56，最近邻只拿到 32 或 64，
    再由 GDI 拉伸 → 糊）。

    本脚本改成：**一次**抠图出 1024 透明主档 → 从它生成 17 档 ICO，
    每一档都直接对应一种常见 DPI（16/20/24/28/32/40/48/56/64/72/80/96/
    112/128/160/192/256）。小尺寸额外做一次轻度 UnsharpMask 抵消
    LANCZOS 的软化（这层描边很粗，缩到 16px 本来就容易糊成一团）。

抠图管线（沿用 MEMORY.md 记的要点，顺序不能换）：
    1. 抹水印（右下角"AI生成"）——先按行取左侧背景色把它盖掉，
       否则它会被当成角色的一部分留下来；
    2. 连续 alpha 估计：暗描边 / 饱和肤色 = 实，**中性浅灰** = 虚；
    3. 从图像边界洪泛"确定是背景"的连通域 → 只有外面的背景和地面阴影
       会被抠掉；眼睛/牙齿这种"中性浅灰但在封闭轮廓里"的会被保留；
    4. 腐蚀 1px：去掉抠图边缘那圈浅色镶边。

用法：
    python tools/make_icon.py            # 写 assets/icons/{venn_icon.png,venn.ico}
    python tools/make_icon.py --preview  # 额外输出一张抠图结果拼版便于目视
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "design", "venn_logo_source.png")
OUT_PNG = os.path.join(ROOT, "assets", "icons", "venn_icon.png")
OUT_ICO = os.path.join(ROOT, "assets", "icons", "venn.ico")

# 覆盖 100%~250% 全部常见缩放：任务栏/标题栏/资源管理器都会命中精确档位
ICO_SIZES = [16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128,
             160, 192, 256]


def remove_watermark(rgb):
    """抹掉右下角的图库水印：按行取其左侧背景色整段盖掉。

    不能只在洪泛时把它当背景 —— 水印是**略深**的中性灰，比"确定背景"的
    阈值暗，会被判成前景而在抠图后留下一块脏影。
    """
    h, w, _ = rgb.shape
    x0, x1 = int(w * 0.66), int(w * 0.995)
    y0, y1 = int(h * 0.895), int(h * 0.995)
    for y in range(y0, y1):
        strip = rgb[y, max(0, x0 - 70):max(1, x0 - 8)].astype(np.float32)
        fill = np.median(strip.reshape(-1, 3), axis=0)
        rgb[y, x0:x1] = fill.astype(rgb.dtype)
    return rgb


def alpha_mask(rgb):
    """抠出角色 = "从边界洪泛掉所有非轮廓的连通域"。

    屏障 = **暗且有彩度**的像素 —— 这张图里角色描边是藏青 (48,58,75)
    （sat≈23~33），而地面阴影是**中性灰** (114,110,111)（sat≈2~6）。
    只按"暗"判会把阴影也当屏障，它自带一圈暗色外沿 → 闭合成一块，
    洪泛进不去，阴影就跟着角色一起留了下来（实测踩过）。

    于是从图像边界洪泛"非屏障"像素：
    · 背景 / 地面阴影：与边界连通 → 冲掉；
    · 奶黄身体、眼白、牙齿：虽然浅，但被**闭合**的藏青描边围着，
      从边界够不着 → 天然保住（不需要"再填内腔"这种易错的补救）；
    · 二值 mask 的锯齿交给后面逐档 LANCZOS 缩放去抗锯齿，比在 1024 上
      猜"软 alpha"稳得多。
    """
    a = rgb.astype(np.float32)
    mn = a.min(axis=2)
    sat = a.max(axis=2) - mn
    outline = (mn <= 150.0) & (sat >= 18.0)
    # 屏障外扩 1px：闭合轮廓的抗锯齿缝可能有 1px 漏点，涨一点堵住。
    # ⚠ **只能涨 1px** —— 涨 2px 会把描边外侧的背景像素一起圈进来，
    #   成图上就是一圈浅色镶边（实测踩过）。
    barrier = np.asarray(
        Image.fromarray((outline * 255).astype(np.uint8), "L")
        .filter(ImageFilter.MaxFilter(3))) > 127
    outside = flood_outside(~barrier)
    # 再向内腐蚀 1px，把描边外侧那一圈"与背景混色"的抗锯齿像素削掉，
    # 镶边才不会跟着进ICO（小尺寸下这一圈就是"发白的一圈毛边"）。
    opaque = np.asarray(
        Image.fromarray(((~outside) * 255).astype(np.uint8), "L")
        .filter(ImageFilter.MinFilter(3))) > 127
    alpha = np.where(opaque, 255, 0).astype(np.uint8)
    return alpha


def flood_outside(mask):
    """从图像边界洪泛出"外部背景"连通域（mask=True 表示确定是背景）。

    这一步是眼白/牙齿能活下来的原因：它们同样是中性浅灰，但被深色嘴线/
    轮廓**封闭**在里面，从边界洪泛够不着。
    """
    from collections import deque

    h, w = mask.shape
    seen = np.zeros_like(mask)
    dq = deque()
    for x in range(w):
        for y in (0, h - 1):
            if mask[y, x] and not seen[y, x]:
                seen[y, x] = True
                dq.append((y, x))
    for y in range(h):
        for x in (0, w - 1):
            if mask[y, x] and not seen[y, x]:
                seen[y, x] = True
                dq.append((y, x))
    while dq:
        y, x = dq.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] \
                    and not seen[ny, nx]:
                seen[ny, nx] = True
                dq.append((ny, nx))
    return seen


def cutout():
    img = Image.open(SRC).convert("RGB")
    rgb = remove_watermark(np.array(img))
    alpha = alpha_mask(rgb)
    out = Image.fromarray(np.dstack([rgb, alpha]), "RGBA")

    # 裁到内容包围盒再留 2% 余量：ICO 各档都是满幅缩小，留白会让小尺寸
    # 的有效像素更少（16px 档尤其吃亏）。
    bbox = out.getbbox()
    if bbox:
        pad = int(max(out.size) * 0.02)
        out = out.crop((max(0, bbox[0] - pad), max(0, bbox[1] - pad),
                        min(out.width, bbox[2] + pad),
                        min(out.height, bbox[3] + pad)))
    side = max(out.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(out, ((side - out.width) // 2, (side - out.height) // 2))
    return canvas


def resize_premultiplied(img, size):
    """按预乘 alpha 缩放。

    直接对 RGBA 缩放会让透明像素的 RGB（这里是黑）渗进边缘 → 一圈黑边。
    """
    w, h = img.size
    scale = size / float(max(w, h))
    sw, sh = max(1, int(round(w * scale))), max(1, int(round(h * scale)))
    a = np.asarray(img, np.float32) / 255.0
    rgb = a[:, :, :3] * a[:, :, 3:4]
    im_a = Image.fromarray((a[:, :, 3] * 255).astype(np.uint8), "L")
    im_rgb = Image.fromarray((rgb * 255).astype(np.uint8), "RGB")
    im_rgb = im_rgb.resize((sw, sh), Image.LANCZOS)
    im_a = im_a.resize((sw, sh), Image.LANCZOS)
    out = np.zeros((sh, sw, 4), np.float32)
    out[:, :, :3] = np.asarray(im_rgb, np.float32) / 255.0
    out[:, :, 3] = np.asarray(im_a, np.float32) / 255.0
    a3 = np.clip(out[:, :, 3:4], 1e-6, 1.0)
    straight = np.clip(out[:, :, :3] / a3, 0.0, 1.0)
    res = np.dstack([straight, out[:, :, 3]])
    frame = Image.fromarray((res * 255).astype(np.uint8), "RGBA")
    # 小尺寸补一次锐化：粗描边缩到 16/24px 时 LANCZOS 会把它糊掉
    if size <= 48:
        frame = frame.filter(
            ImageFilter.UnsharpMask(radius=0.55, percent=75, threshold=2))
    return frame


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()

    master = cutout()
    master.save(OUT_PNG)
    print("主档: %s %s" % (OUT_PNG, master.size))

    frames = [resize_premultiplied(master, s) for s in ICO_SIZES]
    frames[-1].save(OUT_ICO, format="ICO",
                    sizes=[(s, s) for s in ICO_SIZES],
                    append_images=frames[:-1])
    print("ICO:  %s %d 档 %s" % (OUT_ICO, len(ICO_SIZES), ICO_SIZES))

    chk = Image.open(OUT_ICO)
    got = sorted(chk.info.get("sizes", []))
    ok = got == sorted([(s, s) for s in ICO_SIZES])
    print("ICO 自检: %s (%s)" % ("OK" if ok else "缺档", got))
    if not ok:
        return 1

    if args.preview:
        # 拼版：16/32/48/128/256 各一档 + 主档缩略，目视检查有没有糊/镶边
        tiles = [resize_premultiplied(master, s)
                 for s in (16, 32, 48, 128, 256)]
        pad = 12
        W = sum(t.width for t in tiles) + pad * (len(tiles) + 1)
        H = max(t.height for t in tiles) + pad * 2
        sheet = Image.new("RGBA", (W, H), (48, 50, 56, 255))
        x = pad
        for t in tiles:
            sheet.paste(t, (x, pad + (H - 2 * pad - t.height) // 2), t)
            x += t.width + pad
        out = os.path.join(os.path.dirname(ROOT), "_venn_verify",
                           "icon_sizes_preview.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        sheet.save(out)
        print("预览: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())