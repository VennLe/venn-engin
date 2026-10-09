from PIL import Image
import sys

for p in sys.argv[1:]:
    im = Image.open(p).convert("RGBA")
    px = im.load()
    w, h = im.size
    # 采样若干点，统计 R/G/B 均值与极值
    sr = sg = sb = 0
    nr = ng = nb = 0
    mn = (255, 255, 255)
    mx = (0, 0, 0)
    for y in range(0, h, max(1, h // 16)):
        for x in range(0, w, max(1, w // 16)):
            r, g, b, a = px[x, y]
            sr += r; sg += g; sb += b; nr += 1
            mn = (min(mn[0], r), min(mn[1], g), min(mn[2], b))
            mx = (max(mx[0], r), max(mx[1], g), max(mx[2], b))
    print("%s  %dx%d  sample=%d" % (p, w, h, nr))
    print("   mean R=%d G=%d B=%d" % (sr // nr, sg // nr, sb // nr))
    print("   min=%s  max=%s" % (mn, mx))
    print("   pixel(0,0)=%s  pixel(w/2,h/2)=%s" % (px[0, 0], px[w // 2, h // 2]))
