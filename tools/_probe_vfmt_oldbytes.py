#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一次性比对：旧 .vmesh(80B 顶点) vs 新 .vmesh(112B 顶点)。

判据只有一条：对同一个子网格的第 k 个顶点，新文件的**前 80 字节**
必须与旧文件逐字节相同。这是"§13 那次手术没有动老属性"的唯一直接证据
（字节数变了，任何偏移错位都会在这里露出来）。

不是正式用例（依赖一次性备份），结论写进 docs/anime_girl_gap 的 §13。
"""
import os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BAK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "vmesh_bak")

CASES = [
    ("assets/models/characters/animate_girl.vmesh", "assets_models_anime_girl.vmesh"),
    ("assets/models/characters/alicia.vmesh", "assets_models_characters_alicia.vmesh"),
    ("assets/models/characters/animate_girl.vmesh",
     "assets_models_characters_animate_girl.vmesh"),
    ("assets/models/characters/default_character.vmesh",
     "assets_models_characters_default_character.vmesh"),
    ("assets/models/thirdparty/samba_autorig.vmesh",
     "assets_models_thirdparty_samba_autorig.vmesh"),
    ("assets/models/thirdparty/samba_dancing.vmesh",
     "assets_models_thirdparty_samba_dancing.vmesh"),
]


def read_mesh(path):
    """返回 (stride, [(vc, ic, ofsV, verts_bytes)], payload_base)。"""
    data = open(path, "rb").read()
    magic, ver, flags, shash, nsec, _res = struct.unpack_from("<8sIIQII", data, 0)
    assert magic == b"VNMESH\x1a\x00", magic
    secs = {}
    for i in range(nsec):
        sid, sver, crc, _r, off, size = struct.unpack_from("<IIIIQQ", data, 32 + i * 32)
        secs[sid] = (sver, off, size)
    sver, off, size = secs[1]
    p = off
    mver, count, stride, idx16 = struct.unpack_from("<IIII", data, p)
    p += 16
    subs = []
    for i in range(count):
        _name, _mat, vc, ic, ofsV, ofsI = struct.unpack_from("<IIIIII", data, p + i * 112)
        subs.append((vc, ic, ofsV))
    return mver, stride, off, subs, data


ok = True
for rel, bak in CASES:
    new = os.path.join(ROOT, rel)
    old = os.path.join(BAK, bak)
    if not (os.path.isfile(new) and os.path.isfile(old)):
        print("SKIP %s（缺备份）" % rel)
        continue
    nver, nstride, noff, nsubs, ndata = read_mesh(new)
    over, ostride, ooff, osubs, odata = read_mesh(old)
    line = "%-52s old=%dB/%dsub new=%dB/v%d/%dsub " % (
        os.path.basename(rel), ostride, len(osubs), nstride, nver, len(nsubs))
    if len(osubs) != len(nsubs):
        print(line + "FAIL submesh 数不同")
        ok = False
        continue
    bad = 0
    vtot = 0
    for (ovc, _oic, oov), (nvc, _nic, nov) in zip(osubs, nsubs):
        if ovc != nvc:
            print(line + "FAIL 子网格顶点数 %d != %d" % (ovc, nvc))
            ok = False
            bad = -1
            break
        vtot += ovc
        ob = odata[ooff + oov: ooff + oov + ovc * ostride]
        nb = ndata[noff + nov: noff + nov + nvc * nstride]
        for k in range(ovc):
            if ob[k * 80:(k + 1) * 80] != nb[k * nstride:k * nstride + 80]:
                bad += 1
    if bad < 0:
        continue
    if bad:
        print(line + "FAIL %d 个顶点的前 80 字节不同" % bad)
        ok = False
    else:
        print(line + "OK  %d 顶点 × 80B 逐字节相同" % vtot)

sys.exit(0 if ok else 1)
