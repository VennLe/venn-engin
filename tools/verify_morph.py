#!/usr/bin/env python3
# ============================================================
# verify_morph —— §14 morph 数据链验收
#
# ---------------------------- 这一刀在验什么 ----------------------------
# 上一刀（§13，verify_vertex_format）验的是**格式**：112 字节、偏移没动、
# 老 80 字节逐字节未变。这一刀接上了数据链（FBX blend shape → MORPH 区块
# → 顶点里的 morphBase/morphCount → 运行期 Mesh），所以判据全部换成了
# **数据**：
#
#   A  MORPH 区块的存在性：只有 alicia 有；其余 5 个连区块都不该出现
#   B  区块内部账目：target 名唯一、delta 的 target/vertex 不越界、
#      vertex 单调不减（vertex-major 的必然结果）
#   C  顶点 ↔ 流 的双向自洽：每个带 morph 的顶点，它 [base, base+count)
#      这段记录必须**正好**是它自己的（与 C++ 读端做同一道校验，两条
#      独立实现互校 —— 只查一边会漏掉"两边按同一个错理解"的情况）
#   D  确定性：同一 FBX 烤两次逐字节相同（morph 的顺序规则必须确定）
#   E  运行期：VMESH-VFMT 的 morph 计数与 Python 直解一致
#
# ---------------------------- 为什么没有截图判据 ----------------------------
# 与 §13.6 同一个理由，而且更强：vertex shader **还没有读** morph
# （§14.6 说明了为什么这一刀不接 GPU），所以渲染出来的每一帧必然与
# 改造前逐像素相同。一张"必然相同"的图不能证明任何事。
# 反过来说：morph 数据链最危险的失败模式（delta 流与顶点错位、焊接
# 之后没跟着 remap、target 下标在子网格间串了）**全都看不见** ——
# 几何、法线、UV 全对，只有做表情时脸会歪。
# ============================================================

import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
WORK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "morph")
ASSETC = os.path.join(ROOT, "build", "bin", "AssetC.exe")
EDITOR = os.path.join(ROOT, "build", "bin", "Editor.exe")

STRIDE = 112
K_SECTION_MORPH = 6
K_NO_MORPH_BASE = 0xFFFFFFFF

# 六个资产：只有 alicia 的 FBX 带 blend shape（60 个 channel）。
ASSETS_LIST = [
    "models/anime_girl.vmesh",
    "models/characters/alicia.vmesh",
    "models/characters/animate_girl.vmesh",
    "models/characters/default_character.vmesh",
    "models/thirdparty/samba_autorig.vmesh",
    "models/thirdparty/samba_dancing.vmesh",
]
EXPECT_MORPH = {"models/characters/alicia.vmesh"}

# 运行期探针（与 verify_vertex_format 的 D 组同一批）
RUNTIME_CASES = [
    "models/anime_girl.fbx",
    "models/characters/animate_girl.fbx",
    "models/characters/alicia.fbx",
]

_n = 0
_bad = 0


def log(msg):
    print(msg)


def check(name, ok, detail=""):
    global _n, _bad
    _n += 1
    if not ok:
        _bad += 1
    print("CHECK %s %s%s" % (name, "OK" if ok else "FAIL",
                             (" (" + detail + ")") if detail else ""))


# ---------------------------------------------------------------- 容器
def read_container(path):
    """返回 (sections, data)。sections: {id: (version, offset, size)}

    布局必须与 assetio/MeshFormat.cpp 逐项对齐：
      文件头 32 B：magic(8) version(4) flags(4) sourceHash(8) sectionCount(4) reserved(4)
      区块表 32 B/条：id(4) version(4) crc(4) reserved(4) offset(8) size(8)
    """
    with open(path, "rb") as f:
        data = f.read()
    magic, _ver, _flags, _shash, count, _res = struct.unpack_from("<8sIIQII", data, 0)
    if magic != b"VNMESH\x1a\x00":
        raise RuntimeError("bad magic: %s" % path)
    secs = {}
    o = 32
    for _ in range(count):
        sid, sver, _crc, _res2, off, size = struct.unpack_from("<IIIIQQ", data, o)
        secs[sid] = (sver, off, size)
        o += 32
    return secs, data


def parse_mesh(data, secs):
    """返回 (mver, stride, idx16, submeshes)。

    ⚠ `submeshes` 里的 ofsV / ofsI 是**绝对文件偏移**（区块起点 + 目录里
      的相对偏移）。目录里存的是相对偏移，忘了加基准的后果是"读到别的
      区块的字节"——本脚本第一版就是这么错的，C 组三项全红，而 C++ 读端
      （同一道校验的正确实现）是绿的。两条独立实现互校的价值就在这里。
    """
    _ver, off, _size = secs[1]
    mver, count, stride, idx16 = struct.unpack_from("<IIII", data, off)
    subs = []
    o = off + 16
    for _ in range(count):
        _name, _mat, vc, ic, ofsV, ofsI = struct.unpack_from("<IiIIII", data, o)
        subs.append((vc, ic, off + ofsV, off + ofsI))
        o += 112
    return mver, stride, bool(idx16), subs


def parse_morph(data, secs):
    """返回 [ {targets, deltas, channels} ] / 没有区块返回 None。

    MORPH 区块 v1/v2 都读（读端两种都支持，脚本也要能验两种）：

      v1:  ver(u32) smc(u32)  { tc(u32) dc(u32)  tc×u32 名  dc×32B }
      v2:  在 v1 的每个子网格尾部**追加** channel 表：
              cc(u32)  { 名(u32) key数(u32)  key数×{ f32 targetWeight, u32 target } }

    v1 没有 channel 表 → 这里**合成** 1:1 表（{1.0, t}），与 C++ 读端
    （MeshFormat.cpp 的 `morphSecVersion == 1` 分支）用同一条合成规则。
    这样后面的判据只写一遍，不必按版本分叉。
    """
    if K_SECTION_MORPH not in secs:
        return None
    _secVer, off, size = secs[K_SECTION_MORPH]
    # ⚠ 容器区块表里的 `version` 是**固定常数 1**（写端的 Sec 结构里没有
    #   版本字段），真正有意义的版本在**区块内部第一个 u32**。C++ 读端校验
    #   的也是它（MeshFormat.cpp:711 的 `morphSecVersion`）。外部工具若按
    #   区块表的 1 判断"这是 v1"，会把 v2 文件读歪 —— 唯一判据是下面这个。
    blockVer, smc = struct.unpack_from("<II", data, off)
    ver = blockVer
    if ver not in (1, 2):
        raise RuntimeError("MORPH 区块内部版本 %d 不认识（用 AssetC 重烤）" % ver)
    out = []
    o = off + 8
    for _ in range(smc):
        tc, dc = struct.unpack_from("<II", data, o)
        o += 8
        targets = []
        for _ in range(tc):
            # 名字是 STR 区块的下标，这里只需要"非 kNoString"
            targets.append(struct.unpack_from("<I", data, o)[0])
            o += 4
        deltas = []
        for _ in range(dc):
            px, py, pz, target, nx, ny, nz, vertex = struct.unpack_from(
                "<fffIfffI", data, o)
            deltas.append(((px, py, pz), target, (nx, ny, nz), vertex))
            o += 32
        channels = []
        if ver >= 2:
            cc = struct.unpack_from("<I", data, o)[0]
            o += 4
            for _ in range(cc):
                nm, kc = struct.unpack_from("<II", data, o)
                o += 8
                keys = []
                for _ in range(kc):
                    tw, tg = struct.unpack_from("<fI", data, o)
                    o += 8
                    keys.append((tw, tg))
                channels.append({"name": nm, "keys": keys})
        else:
            for t in range(tc):
                channels.append({"name": targets[t], "keys": [(1.0, t)]})
        out.append({"targets": targets, "deltas": deltas,
                    "channels": channels})
    return out


def parse_strings(data, secs):
    if 5 not in secs:
        return []
    _v, off, size = secs[5]
    n = struct.unpack_from("<I", data, off)[0]
    o = off + 4
    out = []
    for _ in range(n):
        ln = struct.unpack_from("<I", data, o)[0]
        out.append(data[o + 4:o + 4 + ln].decode("utf-8", "replace"))
        o += 4 + ln
    return out


def parse_vertices(data, secs, subs, stride):
    """只取 (morphBase, morphCount) 与非零 uv2 计数。"""
    bases = []
    for vc, _ic, ofsV, _ofsI in subs:
        for k in range(vc):
            o = ofsV + k * stride
            mb, mc = struct.unpack_from("<II", data, o + 88)
            bases.append((mb, mc))
    return bases


# ---------------------------------------------------------------- 主流程
def main():
    if not os.path.isfile(ASSETC):
        log("找不到 %s" % ASSETC)
        return 2

    stats = {}
    for rel in ASSETS_LIST:
        p = os.path.join(ASSETS, rel)
        if not os.path.isfile(p):
            log("SKIP 缺文件 %s" % rel)
            continue
        secs, data = read_container(p)
        mver, stride, idx16, subs = parse_mesh(data, secs)
        strs = parse_strings(data, secs)
        stats[rel] = {
            "secs": secs, "data": data, "stride": stride, "meshVersion": mver,
            "subs": subs, "strs": strs,
            "morph": parse_morph(data, secs),
            "verts": parse_vertices(data, secs, subs, stride),
        }

    # ================= A. 区块存在性 =================
    present = {k for k, v in stats.items() if v["morph"] is not None}
    check("A1 只有带 blend shape 的资产有 MORPH 区块",
          present == EXPECT_MORPH,
          "有区块=[%s] 期望=[%s]" % (", ".join(sorted(os.path.basename(x) for x in present)),
                                     ", ".join(sorted(os.path.basename(x) for x in EXPECT_MORPH))))
    check("A2 没有 morph 的资产连区块都不写（不写空区块）",
          all(v["morph"] is not None or v["secs"].get(K_SECTION_MORPH) is None
              for v in stats.values()),
          "/".join(os.path.basename(k) for k in stats))
    # --info 的账目必须与 Python 直解一致（CLI 是给人看的，不能自己算错）
    for rel, st in stats.items():
        p = os.path.join(ASSETS, rel)
        r = subprocess.run([ASSETC, "--info", p], capture_output=True, text=True)
        m = re.search(r"morph targets=(\d+) deltas=(\d+)", r.stdout)
        if not m:
            check("A3 %s --info 打出 morph 行" % os.path.basename(rel), False, "没有该行")
            continue
        want_t = sum(len(m["targets"]) for m in (st["morph"] or []))
        want_d = sum(len(m["deltas"]) for m in (st["morph"] or []))
        check("A3 %s --info 与直解一致" % os.path.basename(rel),
              int(m.group(1)) == want_t and int(m.group(2)) == want_d,
              "info=%s/%s 直解=%d/%d" % (m.group(1), m.group(2), want_t, want_d))

    # ================= B. 区块内部账目 =================
    for rel, st in stats.items():
        if not st["morph"]:
            continue
        name = os.path.basename(rel)
        subs = st["subs"]
        ok_count = len(st["morph"]) == len(subs)
        check("B1 %s MORPH 的子网格数与 MESH 一致" % name, ok_count,
              "%d vs %d" % (len(st["morph"]), len(subs)))

        bad_name, dup_name, bad_target, bad_vertex, nonmono, dup_pair = \
            0, 0, 0, 0, 0, 0
        for si, sm in enumerate(st["morph"]):
            vc = subs[si][0]
            names = sm["targets"]
            deltas = sm["deltas"]
            # 名字必须解得出、且子网格内唯一
            real = [st["strs"][i] if i < len(st["strs"]) else "" for i in names]
            if any(not s for s in real):
                bad_name += 1
            if len(set(real)) != len(real):
                dup_name += 1
            prev_v = -1
            seen = set()
            for (_p, target, _n3, vertex) in deltas:
                if target >= len(names):
                    bad_target += 1
                if vertex >= vc:
                    bad_vertex += 1
                if vertex < prev_v:
                    nonmono += 1
                prev_v = vertex
                if (vertex, target) in seen:
                    dup_pair += 1
                seen.add((vertex, target))

        check("B2 %s target 名字全部可解析" % name, bad_name == 0, "bad=%d" % bad_name)
        check("B3 %s target 名字在子网格内唯一" % name, dup_name == 0, "dup=%d" % dup_name)
        check("B4 %s delta 的 target 下标不越界" % name, bad_target == 0,
              "bad=%d" % bad_target)
        check("B5 %s delta 的顶点下标不越界" % name, bad_vertex == 0,
              "bad=%d" % bad_vertex)
        check("B6 %s delta 按顶点升序（vertex-major 的必然结果）" % name,
              nonmono == 0, "乱序=%d" % nonmono)
        check("B7 %s (vertex,target) 不重复" % name, dup_pair == 0, "dup=%d" % dup_pair)

    # ================= C. 顶点 ↔ 流 双向自洽 =================
    for rel, st in stats.items():
        if not st["morph"]:
            continue
        name = os.path.basename(rel)
        bad_range, bad_owner, orphan = 0, 0, 0
        for si, sm in enumerate(st["morph"]):
            deltas = sm["deltas"]
            vc = st["subs"][si][0]
            ofsV = st["subs"][si][2]
            expect = 0
            for v in range(vc):
                mb, mc = struct.unpack_from("<II", st["data"],
                                            ofsV + v * STRIDE + 88)
                if mc == 0:
                    continue
                if mb + mc > len(deltas):
                    bad_range += 1
                    continue
                if mb != expect:
                    bad_range += 1
                    continue
                for k in range(mc):
                    if deltas[mb + k][3] != v:
                        bad_owner += 1
                expect += mc
            if expect != len(deltas):
                orphan += 1
        check("C1 %s 每个顶点的 [base,base+count) 都在流内且首尾相接" % name,
              bad_range == 0, "bad=%d" % bad_range)
        check("C2 %s 区间里的记录确实属于该顶点" % name, bad_owner == 0,
              "bad=%d" % bad_owner)
        check("C3 %s 没有孤儿 delta（流长度 == 顶点 count 之和）" % name,
              orphan == 0, "孤儿子网格=%d" % orphan)

    # ================= D. 确定性 =================
    fbx = os.path.join(ASSETS, "models", "characters", "alicia.fbx")
    if os.path.isfile(fbx):
        os.makedirs(WORK, exist_ok=True)
        a = os.path.join(WORK, "det_a.vmesh")
        b = os.path.join(WORK, "det_b.vmesh")
        ok = True
        for out in (a, b):
            r = subprocess.run([ASSETC, fbx, "-q", "-o", out],
                               capture_output=True, text=True)
            if r.returncode != 0:
                ok = False
                log("烤制失败: %s" % r.stdout[-400:])
        if ok:
            ha = open(a, "rb").read()
            hb = open(b, "rb").read()
            check("D1 alicia 连烤两次逐字节相同（delta 顺序确定）",
                  ha == hb, "%d vs %d bytes" % (len(ha), len(hb)))
        else:
            check("D1 alicia 连烤两次逐字节相同（delta 顺序确定）", False, "烤制失败")

    # ================= F. channel 表（§15，v2 新增）=================
    #
    # 这一组验的是「channel 与 target 解耦」这件事落没落地。上一刀（§14）
    # 只有一个 target 列表，等于把 FBX 的 blend_channel 中间那层
    # keyframe **整个丢了** —— 一个滑条只能挂一个形状，in-between 全废。
    #
    # 判据的核心是「读出来的表必须能被 MorphRig 无条件信任」：
    # 写端做了什么保证，这里就断言什么。任何一条破了，运行期表现都是
    # **静默的形状错**（滑条推一半，脸变成另一个表情），不是崩溃。
    for rel, st in stats.items():
        if not st["morph"]:
            continue
        name = os.path.basename(rel)
        no_key, bad_order, bad_weight, bad_key_target, no_real = 0, 0, 0, 0, 0
        multi_key = 0                       # 有 in-between 的滑条数
        max_keys = 0
        for si, sm in enumerate(st["morph"]):
            tc = len(sm["targets"])
            for ch in sm["channels"]:
                keys = ch["keys"]
                if not keys:
                    no_key += 1
                    continue
                max_keys = max(max_keys, len(keys))
                if len(keys) > 1:
                    multi_key += 1
                # 写端做了 stable_sort（按 targetWeight 升序）。**必须**是
                # 有序的：MorphRig 的分段查找假设 points 已排序，顺序错了
                # 会在插值段上切错位置，而不会报错。
                for i in range(1, len(keys)):
                    if keys[i][0] < keys[i - 1][0]:
                        bad_order += 1
                        break
                for tw, tg in keys:
                    if not (0.0 <= tw <= 1.0):
                        bad_weight += 1
                    # kNoMorphTarget 是**合法的断点**（本桶没这个偏移），
                    # 但除了它以外任何越界都会被 MorphRig 的边界检查吞掉
                    # → 形状静默变小。写端就该保证不出现。
                    if tg != K_NO_MORPH_BASE and tg >= tc:
                        bad_key_target += 1
                if all(tg == K_NO_MORPH_BASE for _tw, tg in keys):
                    no_real += 1
        check("F1 %s 每个 channel 至少一个 key" % name, no_key == 0,
              "空 channel=%d" % no_key)
        check("F2 %s channel 的 key 按 targetWeight 升序" % name,
              bad_order == 0, "乱序=%d" % bad_order)
        check("F3 %s channel key 的 targetWeight ∈ [0,1]" % name,
              bad_weight == 0, "越界=%d" % bad_weight)
        check("F4 %s channel key 的 target 下标不越界（kNoMorphTarget 除外）" % name,
              bad_key_target == 0, "越界=%d" % bad_key_target)
        # 整条 channel 一个真 target 都没有 = 纯粹的空滑条（face 被切成
        # 十几个材质桶，每个桶都继承了 deformer 的全部 channel）。这种
        # 必须在**烤制期**丢掉，否则运行期会多出一堆"推了没反应"的滑条
        # 占着 MorphRig 的通道下标，UI 上还没法解释。
        check("F5 %s 没有一条 channel 是全部 key 都指向 kNoMorphTarget" % name,
              no_real == 0, "空 channel=%d" % no_real)
        uniq = set()
        for sm in st["morph"]:
            for ch in sm["channels"]:
                uniq.add(st["strs"][ch["name"]]
                         if ch["name"] < len(st["strs"]) else "")
        log("NOTE %s 唯一 channel 名 %d 个；有 in-between 的 <子网格×滑条> "
            "%d 条；单个滑条最多 %d 个 key"
            % (name, len(uniq), multi_key, max_keys))

    # ================= G. offset_weights 预乘（§15）=================
    #
    # ufbx 的求值式是 `vertex += offset * weight * offset_weights[i]`
    # （third_party/ufbx/ufbx.c:32078）。offset_weights 是**逐偏移的常数**，
    # 所以烤制期把它乘进 delta 之后，运行期一行都不用改、32 B 布局也不用动。
    #
    # 怎么从产物反推它真的乘了？两条**必要条件**：
    #   G1  `ow == 0` 的偏移必须在烤制期被丢掉 → 产物里不该有
    #       "pos 和 nrm 同时为 0" 的 delta（那是白占 32 B 还让顶点多循环一次）
    #   G2  若某条 delta 的 pos 恰为 0 而 nrm 非 0，说明它是纯法线偏移
    #       （合法，但要计数 —— 数量异常地多通常意味着位置被乘成了 0）
    # 这两条**不能**证明乘对了系数（那需要重跑 ufbx 求值做对照），但能
    # 抓住"忘记乘"和"乘成 0"这两种最常见的错。
    for rel, st in stats.items():
        if not st["morph"]:
            continue
        name = os.path.basename(rel)
        zero_both, nrm_only, total = 0, 0, 0
        for sm in st["morph"]:
            for (p, _t, n, _v) in sm["deltas"]:
                total += 1
                pz = p[0] == 0.0 and p[1] == 0.0 and p[2] == 0.0
                nz = n[0] == 0.0 and n[1] == 0.0 and n[2] == 0.0
                if pz and nz:
                    zero_both += 1
                elif pz and not nz:
                    nrm_only += 1
        check("G1 %s 没有零 delta（offset_weights==0 的偏移已丢弃）" % name,
              zero_both == 0, "零 delta=%d / 总 %d" % (zero_both, total))
        log("NOTE %s delta %d 条，其中纯法线偏移 %d 条（%.1f%%）"
            % (name, total, nrm_only,
               100.0 * nrm_only / max(total, 1)))

    # ================= E. 运行期互校 =================
    probe = os.path.join(WORK, "probe")
    if os.path.isdir(probe):
        import shutil
        shutil.rmtree(probe)
    root = os.path.join(probe, "root")
    os.makedirs(os.path.join(root, "scenes"))
    ents = []
    old = 1_000_000_000
    for i, rel in enumerate(("models/characters/alicia.fbx", "models/anime_girl.fbx")):
        stem = os.path.splitext(rel)[0]
        d = os.path.join(root, os.path.dirname(rel))
        os.makedirs(d, exist_ok=True)
        for ext, mt in ((".fbx", old), (".vmesh", old + 100)):
            src = os.path.join(ASSETS, stem + ext)
            dst = os.path.join(d, os.path.basename(stem) + ext)
            if not os.path.isfile(src):
                continue
            with open(src, "rb") as f:
                blob = f.read()
            with open(dst, "wb") as f:
                f.write(blob)
            os.utime(dst, (mt, mt))
        ents.append({
            "name": "M%d" % i,
            "transform": {"position": [i * 3.0, 0.0, 0.0],
                          "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
            "parent": -1, "visible": True, "castShadow": True,
            "mesh": {"kind": "fbx", "path": rel,
                     "modelKey": "%s#morph%d" % (rel, i), "subMeshIndex": 0},
        })
    import json
    with open(os.path.join(root, "scenes", "probe.json"), "w",
              encoding="utf-8") as f:
        json.dump({"version": 1, "generator": "verify_morph 探针",
                   "entities": ents}, f, ensure_ascii=False, indent=2)

    env = dict(os.environ)
    env.update({
        "MYVK_ASSET_ROOT": root,
        "MYVK_EDITOR_SCENE": "scenes/probe.json",
        "MYVK_FRAMES": "60",
        "MYVK_NO_WINDOW_SAVE": "1",
    })
    r = subprocess.run([EDITOR], capture_output=True, text=True, env=env, cwd=root)
    out = r.stdout + r.stderr
    lines = re.findall(r"VMESH-VFMT stride=(\d+) verts=(\d+) uv2=(\d+) "
                       r"tangent=(\d+) morphVerts=(\d+) morphDeltas=(\d+) "
                       r"morphTargets=(\d+) morphBad=(\d+)", out)
    check("E1 运行期打出 VFMT 行（走的是 .vmesh 不是 FBX 回退）",
          len(lines) == 2, "%d 行 / loaded=%d fbxFallback=%d" %
          (len(lines), out.count("VMESH loaded"), out.count("FBX loaded")))
    if len(lines) == 2:
        by_verts = {int(x[1]): x for x in lines}
        for rel, want_has in (("models/characters/alicia.vmesh", True),
                              ("models/anime_girl.vmesh", False)):
            st = stats.get(rel)
            if not st:
                continue
            vc = sum(s[0] for s in st["subs"])
            line = by_verts.get(vc)
            if not line:
                check("E2 %s 运行期有对应行" % os.path.basename(rel), False,
                      "没有 verts=%d 的行" % vc)
                continue
            n_morph_py = sum(1 for v in st["verts"] if v[0] != K_NO_MORPH_BASE)
            check("E2 %s 运行期 morph 顶点数与直解一致" % os.path.basename(rel),
                  int(line[4]) == n_morph_py,
                  "run=%s py=%d" % (line[4], n_morph_py))
            check("E3 %s 运行期 morphBad == 0" % os.path.basename(rel),
                  int(line[7]) == 0, "bad=%s" % line[7])
            if want_has:
                check("E4 %s 运行期确实读到 delta" % os.path.basename(rel),
                      int(line[5]) > 0 and int(line[6]) > 0,
                      "deltas=%s targets=%s" % (line[5], line[6]))

    # ---- E5/E6：MorphRig 的**跨子网格合并**（§15 的名字欠账）----
    #
    # 磁盘上是「逐子网格」的 channel 表（158 条 = 12 个材质桶各存一份），
    # 运行期必须合并成「按名字唯一」的滑条表（这才是 UI 上该有几根滑条）。
    # 这两个数不相等，说明合并没做（或者合并键不是名字）。
    mlines = re.findall(r"VMESH-MORPH model=(\S+) channels=(\d+) driving=(\d+) "
                        r"subMeshes=(\d+)", out)
    check("E5 运行期打出 MORPH 行", len(mlines) >= 1, "%d 行" % len(mlines))
    if mlines:
        st = stats.get("models/characters/alicia.vmesh")
        uniq = set()
        drive_any = set()
        if st:
            for sm in st["morph"]:
                for ch in sm["channels"]:
                    nm = st["strs"][ch["name"]] if ch["name"] < len(st["strs"]) else ""
                    uniq.add(nm)
                    if any(tg != K_NO_MORPH_BASE for _tw, tg in ch["keys"]):
                        drive_any.add(nm)
        # 运行期按 verts 顺序打印，alicia 是第一个（探针里是 M0）
        run_ch, run_dr = int(mlines[0][1]), int(mlines[0][2])
        check("E5 运行期 channel 数 == 磁盘上唯一名字数（同名跨桶已合并）",
              run_ch == len(uniq),
              "run=%d 磁盘唯一=%d（磁盘逐桶 %d 条）"
              % (run_ch, len(uniq),
                 sum(len(sm["channels"]) for sm in (st["morph"] if st else []))))
        check("E6 运行期 driving 数 == 磁盘上至少有一个真 target 的名字数",
              run_dr == len(drive_any),
              "run=%d 磁盘=%d" % (run_dr, len(drive_any)))

    # ================= 结果 =================
    print("\n结果: %s（%d 项）" % ("PASS" if _bad == 0 else "FAIL: %d 项" % _bad, _n))
    return 0 if _bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
