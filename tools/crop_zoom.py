from PIL import Image
import sys

src = sys.argv[1]
out = sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
scale = float(sys.argv[7]) if len(sys.argv) > 7 else 2.0

im = Image.open(src).convert("RGB")
crop = im.crop((x0, y0, x1, y1))
w, h = crop.size
crop = crop.resize((int(w * scale), int(h * scale)), Image.NEAREST)
crop.save(out)
print("saved", out, crop.size)
