#!/usr/bin/env python3
# 一次性探针：从 alicia.vmesh 的 MORPH 区块里取真实 delta，画成三格图。
#
# 为什么需要它：morph 这一刀没有像素判据（着色器还不读，§14.1），所以
# "数据对不对"只能靠字节断言。但**字节自洽证明不了内容不是一堆 0** ——
# 一个全 0 的 delta 流同样能通过 verify_morph 的 A/B/C 全部 25 项。
# 这个探针看的是内容：每个 target 影响多少顶点、位移有多大、动的是脸还是全身。
#
# 用法：python tools/_probe_morph_stats.py
# 产物：../_venn_verify/morph_delta.png（仓库外，不进版本库）
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VMESH = os.path.join(ROOT, "assets", "models", "characters", "alicia.vmesh")
OUT = os.path.join(os.path.dirname(ROOT), "_venn_verify", "morph_delta.png")


def read_container(path):
    data = open(path, "rb").read()
    magic, _v, _f, _h, n, _r = struct.unpack_from("<8sIIQII", data, 0)
    assert magic == b"VNMESH\x1a\x00"
    secs = {}
    for i in range(n):
        sid, _sv, _crc, _rr, off, size = struct.unpack_from("<IIIIQQ", data, 32 + i * 32)
        secs[sid] = (off, size)
    return data, secs


def main():
    data, secs = read_container(VMESH)
    if 6 not in secs:
        print("没有 MORPH 区块")
        return 1
    off, _size = secs[6]

    # MESH：拿顶点的静止位置（第 3 格要用）
    moff, _ms = secs[1]
    _mv, smc, stride, _i16 = struct.unpack_from("<IIII", data, moff)
    subs = []
    o = moff + 16
    for _ in range(smc):
        _nm, _mat, vc, ic, ofsV, ofsI = struct.unpack_from("<IiIIII", data, o)
        subs.append((vc, moff + ofsV))
        o += 112

    # MORPH：每个子网格一份 (names, deltas)
    _ver, nsub = struct.unpack_from("<II", data, off)
    o = off + 8
    deltas_all = []      # (sub, target, pos(3), nrm(3), vertex)
    for si in range(nsub):
        tc, dc = struct.unpack_from("<II", data, o)
        o += 8
        o += 4 * tc          # 名字索引（这里不解析）
        for _ in range(dc):
            px, py, pz, t, nx, ny, nz, v = struct.unpack_from("<fffIfffI", data, o)
            deltas_all.append((si, t, (px, py, pz), (nx, ny, nz), v))
            o += 32

    d = np.array([[x[2][0], x[2][1], x[2][2]] for x in deltas_all], dtype=np.float64)
    lens = np.linalg.norm(d, axis=1)

    agg = {}
    for si, t, p, _n, _v in deltas_all:
        agg.setdefault((si, t), []).append(p)
    keys = sorted(agg)

    W, H = 1500, 560
    img = Image.new("RGB", (W, H), (16, 18, 22))
    dr = ImageDraw.Draw(img)

    # ---- 第 1 格：每个 (子网格,target) 的 delta 条数 ----
    x0, y0, w1, h1 = 40, 70, 620, 420
    dr.rectangle([x0, y0, x0 + w1, y0 + h1], outline=(60, 66, 76))
    counts = [len(agg[k]) for k in keys]
    mx = max(counts) if counts else 1
    bw = w1 / max(len(counts), 1)
    for i, c in enumerate(counts):
        h = h1 * c / mx
        col = (90, 170, 240) if keys[i][0] == 0 else (
            (240, 170, 90) if keys[i][0] == 1 else (150, 220, 150))
        dr.rectangle([x0 + i * bw + 0.5, y0 + h1 - h, x0 + (i + 1) * bw - 0.5,
                      y0 + h1], fill=col)
    dr.text((x0, y0 - 26), "deltas per (submesh, target)   max=%d  n=%d"
            % (mx, len(counts)), fill=(220, 226, 236))

    # ---- 第 2 格：位移长度直方图 ----
    x2, w2 = 720, 380
    dr.rectangle([x2, y0, x2 + w2, y0 + h1], outline=(60, 66, 76))
    nz = lens[lens > 0]
    if len(nz):
        lo, hi = np.log10(nz.min()), np.log10(nz.max())
        hist, _edges = np.histogram(nz, bins=36, range=(10 ** lo, 10 ** hi))
        mh = max(hist.max(), 1)
        bw2 = w2 / len(hist)
        for i, v in enumerate(hist):
            h = h1 * v / mh
            if h > 0.3:
                dr.rectangle([x2 + i * bw2 + 0.5, y0 + h1 - h,
                              x2 + (i + 1) * bw2 - 0.5, y0 + h1],
                             fill=(200, 130, 220))
    dr.text((x2, y0 - 26), "|posDelta| histogram  min=%.2e max=%.2e zero=%d"
            % (nz.min(), nz.max(), int((lens == 0).sum())), fill=(220, 226, 236))

    # ---- 第 3 格：受影响顶点的前视投影（(x,z) 当正面）----
    x3, w3 = 1140, 320
    dr.rectangle([x3, y0, x3 + w3, y0 + h1], outline=(60, 66, 76))
    pts = []
    for vc, ofsV in subs:
        for k in range(vc):
            px, py, pz = struct.unpack_from("<fff", data, ofsV + k * stride)
            pts.append((px, pz))
    pts = np.array(pts)
    # ⚠ 顶点下标是**子网格内局部**的，跨子网格不可比 —— 所以按子网格分别取
    hit_by_sub = {}
    for si, _t, _p, _n, v in deltas_all:
        hit_by_sub.setdefault(si, set()).add(v)
    lo = pts.min(axis=0)
    hi = pts.max(axis=0)
    span = np.maximum(hi - lo, 1e-6)

    def proj(p):
        u = (p[0] - lo[0]) / span[0]
        v = (p[1] - lo[1]) / span[1]
        return x3 + 10 + u * (w3 - 20), y0 + h1 - 10 - v * (h1 - 20)

    for p in pts[::3]:
        a, b = proj(p)
        dr.point((a, b), fill=(52, 58, 68))
    for si, vset in hit_by_sub.items():
        if si >= len(subs):
            continue
        vc, ofsV = subs[si]
        for k in vset:
            if k >= vc:
                continue
            px, py, pz = struct.unpack_from("<fff", data, ofsV + int(k) * stride)
            a, b = proj((px, pz))
            dr.point((a, b), fill=(255, 120, 120))
    dr.text((x3, y0 - 26), "affected verts (front view, red)", fill=(220, 226, 236))

    dr.text((20, 18), "alicia.vmesh  MORPH: %d deltas / %d (submesh,target) pairs / "
            "%d affected verts" % (len(deltas_all), len(keys),
                                   sum(len(s) for s in hit_by_sub.values())),
            fill=(240, 244, 250))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img.save(OUT)
    print("saved", OUT, img.size)
    print("deltas=%d  pairs=%d" % (len(deltas_all), len(keys)))
    print("|posDelta| min=%.4e max=%.4e mean=%.4e  zeros=%d"
          % (nz.min(), nz.max(), nz.mean(), int((lens == 0).sum())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
