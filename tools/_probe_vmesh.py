"""临时探针：解析 .vmesh 的子网格目录，打印每个子网格的材质、包围盒、顶点数。

用途：确认"脸"到底是哪几个子网格、在局部空间的哪一侧（Y 正负），从而判断
模型的正面是 -Y 还是 +Y —— 朝向补偿 +90° 只对了一半的话（180° 错），
"人往前走、脸朝后"就是必然的。

格式见 assetio/MeshFormat.cpp 的 writeMeshFile。
"""
import struct
import sys

PATH = sys.argv[1] if len(sys.argv) > 1 else "assets/models/characters/alicia.vmesh"

buf = open(PATH, "rb").read()
assert buf[:8] == b"VNMESH\x1a\x00", "不是 vmesh"
ver, flags, src_hash, sec_count, _res = struct.unpack_from("<IIQII", buf, 8)
print("version=%d flags=0x%x sections=%d bytes=%d" % (ver, flags, sec_count, len(buf)))

secs = {}
for i in range(sec_count):
    e = 32 + i * 32
    sid, sver, crc, _r, off, size = struct.unpack_from("<IIIIQQ", buf, e)
    secs[sid] = (off, size)

# ---- STR（字符串表）----
strs = []
off, size = secs[5]
n = struct.unpack_from("<I", buf, off)[0]
p = off + 4
for _ in range(n):
    ln = struct.unpack_from("<I", buf, p)[0]
    p += 4
    strs.append(buf[p:p + ln].decode("utf-8", "replace"))
    p += ln

# ---- MAT（材质）----
mats = []
off, size = secs.get(4, (0, 0))
if size:
    _v, cnt = struct.unpack_from("<II", buf, off)
    p = off + 8
    for _ in range(cnt):
        ni, ai, ari, nni, nri = struct.unpack_from("<IIIII", buf, p)
        p += 20
        base = struct.unpack_from("<4f", buf, p)
        p += 16
        rough, metal = struct.unpack_from("<2f", buf, p)
        p += 8
        emis = struct.unpack_from("<3f", buf, p)
        p += 12
        ds = struct.unpack_from("<I", buf, p)[0]
        p += 4
        mats.append(dict(name=strs[ni], albedo=strs[ai], normal=strs[nni],
                         base=base, rough=rough, metal=metal, emissive=emis,
                         doubleSided=bool(ds)))

# ---- MESH（子网格目录）----
off, size = secs[1]
_v, sub_n, vsize, idx16 = struct.unpack_from("<IIII", buf, off)
print("submeshes=%d vertexSize=%d index16=%d" % (sub_n, vsize, idx16))
print()
print("%-3s %-22s %7s %8s  %-30s %-30s %s"
      % ("#", "name", "verts", "mat", "lo", "hi", "center"))
rows = []
for i in range(sub_n):
    e = off + 16 + i * 112
    name, mat, vc, ic, ofsV, ofsI = struct.unpack_from("<IiiIII", buf, e)
    lo = struct.unpack_from("<3f", buf, e + 24)
    hi = struct.unpack_from("<3f", buf, e + 36)
    rows.append((i, strs[name], vc, ic, mat, lo, hi))

for i, nm, vc, ic, mat, lo, hi in rows:
    mn = mats[mat]["name"] if 0 <= mat < len(mats) else "?"
    c = [(lo[k] + hi[k]) * 0.5 for k in range(3)]
    print("%-3d %-22s %7d %8s  (%6.3f,%6.3f,%6.3f)  (%6.3f,%6.3f,%6.3f)  "
          "(%6.3f,%6.3f,%6.3f)"
          % (i, nm, vc, mn, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
             c[0], c[1], c[2]))

print()
print("材质表（doubleSided 决定背面剔除；地形/角色都看这个）")
for i, m in enumerate(mats):
    print("  [%2d] %-22s ds=%-5s base=%s rough=%.3f metal=%.3f albedo=%s"
          % (i, m["name"], m["doubleSided"],
             tuple(round(x, 3) for x in m["base"]), m["rough"], m["metal"],
             m["albedo"] or "-"))
