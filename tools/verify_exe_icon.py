#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""验证 Editor.exe 内嵌图标组（RT_GROUP_ICON=14）里各 DPI 档是否齐备。

用法: python tools/verify_exe_icon.py [Editor.exe]
输出 RESULT: PASS / FAIL，并列出每档 (w,h,bpp,bytes)。
"""
import os
import struct
import sys

RT_GROUP_ICON = 14
RT_ICON = 3


def read_image_resources(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[pe:pe + 4] == b"PE\0\0", "不是合法 PE"
    machine, nsec = struct.unpack_from("<HH", d, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    ddoff = opt + (112 if magic == 0x20B else 96)
    rsrc_rva, rsrc_size = struct.unpack_from("<II", d, ddoff + 2 * 8)
    if rsrc_rva == 0:
        return None, 0
    sec = opt + struct.unpack_from("<H", d, pe + 20)[0]
    sects = []
    for i in range(nsec):
        o = sec + i * 40
        _name = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vs, va, rs, ra = struct.unpack_from("<IIII", d, o + 8)
        sects.append((va, vs, ra, rs))

    def rva2off(rva):
        for va, vs, ra, rs in sects:
            if va <= rva < va + max(vs, rs):
                return ra + (rva - va)
        return None

    base = rva2off(rsrc_rva)

    def entries(off):
        n_named, n_id = struct.unpack_from("<HH", d, off + 12)
        out = []
        for i in range(n_named + n_id):
            nameid, offs = struct.unpack_from("<II", d, off + 16 + i * 8)
            out.append((nameid & 0x7FFFFFFF, bool(nameid & 0x80000000), offs))
        return out

    # 顶层：找 type 14 (group icon)
    groups = []
    for rtype, _named, roffs in entries(base):
        if roffs & 0x80000000:
            dnode = base + (roffs & 0x7FFFFFFF)
            for rid, _n2, ro2 in entries(dnode):
                if ro2 & 0x80000000:
                    lnode = base + (ro2 & 0x7FFFFFFF)
                    for lang, _n3, ro3 in entries(lnode):
                        if not (ro3 & 0x80000000):
                            data_rva, size = struct.unpack_from(
                                "<II", d, base + ro3)
                            doff = rva2off(data_rva)
                            blob = d[doff:doff + size]
                            if rtype == RT_GROUP_ICON:
                                groups.append(blob)
    return groups, len(entries(base))


def parse_group(blob):
    """GRPICONDIR: reserved,type,count -> count*GRPICONDIRENTRY"""
    res, typ, count = struct.unpack_from("<HHH", blob, 0)
    out = []
    for i in range(count):
        o = 6 + i * 14
        w, h, ncol, _res = struct.unpack_from("<BBBB", blob, o)
        nplanes, nbits, size, nid = struct.unpack_from("<HHIH", blob, o + 4)
        out.append(dict(w=256 if w == 0 else w,
                        h=256 if h == 0 else h,
                        bpp=nbits, size=size, id=nid))
    return out


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "build/bin/Editor.exe"
    if not os.path.isfile(exe):
        exe = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", exe)
    groups, ntypes = read_image_resources(exe)
    if not groups:
        print("FAIL: exe 内未找到 RT_GROUP_ICON 资源")
        return 1
    ent = parse_group(max(groups, key=len))
    sizes = sorted(x["w"] for x in ent)
    print("exe =", os.path.abspath(exe))
    print("图标组内含 %d 档:" % len(ent))
    for x in sorted(ent, key=lambda t: t["w"]):
        print("   %3dx%-3d %2dbpp %6d bytes  id=%d"
              % (x["w"], x["h"], x["bpp"], x["size"], x["id"]))

    want = [16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 160,
            192, 256]
    missing = [s for s in want if s not in sizes]
    # Windows 任务栏/标题栏常用档
    critical = [16, 20, 24, 32, 40, 48, 64, 256]
    miss_crit = [s for s in critical if s not in sizes]
    ok = not miss_crit and sizes == want
    print("sizes =", sizes)
    if missing:
        print("缺失档:", missing)
    if ok:
        print("RESULT: PASS —— 17 档全覆盖（100%~250% DPI），关键档齐备")
        return 0
    print("RESULT: FAIL —— 关键档缺失:", miss_crit)
    return 1


if __name__ == "__main__":
    sys.exit(main())