#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""回归：顶点格式（112 B，§13）—— uv2 / morphBase / tangent 三个通道。

------------------------------ 这一刀为什么没有截图判据 ------------------------------
老规矩是新功能做"改前 / 改后"两轮截图、要求 `max diff == 0`。这一次用不上，
**不是偷懒，是截图在这里根本没有分辨力**：

  · 三个新通道**没有任何着色器读**（`pbr.vert` 只声明 location 0..5）。
  · 老六个属性的字节在重烤前后**逐个相同**（下面 E 组就是这件事的证据）。
  → 渲染出来的每一帧必然一样。一张"必然一样"的图不能证明任何事，
    它只能证明"编辑器还能启动"。

所以这一刀的判据全部钉在**字节**上：文件结构（A/B）、确定性（C）、
运行期解析出来的统计量与文件里的统计量一致（D）、老属性零变更（E）。
比截图更强的一点：截图只能发现"看得见的变化"，而格式手术最危险的失败模式
（字段偏移错一格、reserved 里带进脏数据、焊接键漏了字段）全都**看不见**。

运行：python tools/verify_vertex_format.py
结尾一定是 `结果: PASS` / `结果: FAIL: ...`。
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
ASSETC = os.path.join(ROOT, "build", "bin", "AssetC.exe")
EDITOR = os.path.join(ROOT, "build", "bin", "Editor.exe")
WORK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "vfmt")
# 一次性备份的旧格式文件（§13 手术前的产物）。没有就跳过 E 组。
BAK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "vmesh_bak")

STRIDE = 112
MESH_VER = 2

# 重烤前后的"账"必须完全一致。数字一旦变了，说明烤制参数用错了
# （例如 default_character 少了 --strip-skin，joints 会从 23 变成 52）。
#   submeshes, vertices, indices, joints, clips
#
# ⚠ `anime_girl` 的 clips 从 **4 变成 2**（2026-10-28，#329）：
#   它的 FBX 里有 4 个 AnimationStack（`Rig|Idle` ×2 + `Rig|Walk` ×2 ——
#   同一动作被导出了两遍），归一化掉 `|` 前缀后名字是 [Idle,Walk,Idle,Walk]。
#   导入期现在按"同名 + 同时长 → 重复导出 → 丢弃"去重（`assetio/ClipDedup.h`），
#   于是 `--info` 报 clips=2。**几何一个字节没动**：重烤前后 submeshes/vertices/
#   indices/joints/materials 全等，sourceHash 也全等，只有动画段少了一段重复。
BASE = {
    "models/anime_girl.vmesh":                   (7, 11338, 65712, 21, 2),
    "models/characters/alicia.vmesh":            (20, 21310, 95394, 128, 4),
    "models/characters/animate_girl.vmesh":      (7, 25626, 101781, 65, 4),
    "models/characters/default_character.vmesh": (2, 35440, 165960, 23, 4),
    "models/thirdparty/samba_autorig.vmesh":     (2, 35440, 165960, 23, 4),
    "models/thirdparty/samba_dancing.vmesh":     (2, 35440, 165960, 52, 2),
}

# 运行期探针只用这三个：一个"没有 UV2"、一个"有 UV2"、一个最大的。
RUNTIME_CASES = [
    "models/anime_girl.fbx",
    "models/characters/animate_girl.fbx",
    "models/characters/alicia.fbx",
]

fails: list[str] = []


def log(msg: str) -> None:
    print(msg, flush=True)


def check(name: str, ok: bool, detail: str = "") -> bool:
    log("CHECK %s %s%s" % (name, "OK" if ok else "FAIL", (" (%s)" % detail) if detail else ""))
    if not ok:
        fails.append("%s %s" % (name, detail))
    return ok


def run(cmd: list[str], env_extra: dict | None = None, timeout: int = 300):
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


# ---------------------------------------------------------------- .vmesh 只读解析
# 布局必须与 assetio/MeshFormat.cpp 逐项对齐：
#   文件头 32 B：magic(8) version(4) flags(4) sourceHash(8) sectionCount(4) reserved(4)
#   区块表 32 B/条：id(4) version(4) crc(4) reserved(4) offset(8) size(8)
#   MESH 载荷：version(4) count(4) stride(4) idx16(4) 然后 count × 112 B 目录
#             目录：nameStr(4) material(4) vc(4) ic(4) ofsV(4) ofsI(4)
#                   lo(12) hi(12) mat(64)
def parse_vmesh(path: str):
    data = open(path, "rb").read()
    magic, _ver, _flags, _shash, nsec, _res = struct.unpack_from("<8sIIQII", data, 0)
    if magic != b"VNMESH\x1a\x00":
        raise ValueError("bad magic %r" % magic)
    secs = {}
    for i in range(nsec):
        sid, sver, _crc, _r, off, size = struct.unpack_from("<IIIIQQ", data, 32 + i * 32)
        secs[sid] = (sver, off, size)
    if 1 not in secs:
        raise ValueError("no MESH section")
    _sver, off, _size = secs[1]
    mver, count, stride, idx16 = struct.unpack_from("<IIII", data, off)
    subs = []
    for i in range(count):
        p = off + 16 + i * 112
        _name, _mat, vc, ic, ofsV, ofsI = struct.unpack_from("<IIIIII", data, p)
        subs.append((vc, ic, off + ofsV, ofsI))
    return data, mver, stride, subs, bool(idx16)


def vertex_stats(path: str):
    """返回 dict：stride / 各新通道的非零顶点数 / morph 自洽计数。

    ⚠ 第 88 字节那个 u32 在 §13 叫 `reserved0`（断言"必须是 0"），
      §14 起是 `morphCount`（该顶点在 delta 流里的记录条数）。
      同一个偏移、不同的判据 —— 所以这里不再数"非零即脏"，
      而是数"base 与 count 是否自洽"。
    """
    data, mver, stride, subs, _i16 = parse_vmesh(path)
    n_uv2 = n_tan = n_morph = n_cnt = n_bad = total = 0
    for vc, _ic, base, _ofsI in subs:
        for k in range(vc):
            o = base + k * stride
            u0, u1 = struct.unpack_from("<ff", data, o + 80)
            morph, mcount = struct.unpack_from("<II", data, o + 88)
            t = struct.unpack_from("<4f", data, o + 96)
            total += 1
            if u0 != 0.0 or u1 != 0.0:
                n_uv2 += 1
            if t[3] != 0.0 or t[0] != 0.0 or t[1] != 0.0 or t[2] != 0.0:
                n_tan += 1
            if morph != 0xFFFFFFFF:
                n_morph += 1
            if mcount != 0:
                n_cnt += 1
            # 矛盾状态：有 base 没 count / 有 count 没 base。
            if (morph == 0xFFFFFFFF) != (mcount == 0):
                n_bad += 1
    return {
        "stride": stride, "meshVersion": mver, "total": total,
        "uv2": n_uv2, "tangent": n_tan, "morph": n_morph,
        "morphCount": n_cnt, "morphBad": n_bad,
        "submeshes": len(subs),
    }


# ---------------------------------------------------------------- 探针资产根
def prepare_runtime_root() -> str:
    """造一个独立资产根，只放探针要用的 fbx + 同名 .vmesh。

    ⚠ **必须**把 .vmesh 的 mtime 拨到 .fbx 之后。加载器判"缓存是否过期"
      用的是 mtime 比较；`copy2` 会保留原时间戳，而仓库里两者的先后
      关系是历史遗留的 —— 一旦 .fbx 更新，整个 D 组会静默退化成"测的是
      FBX 解析路径"，而 D2 统计照样对得上（因为两边数据一样）。
      这种"判据还在，被测对象换了"的失败最难发现，所以这里显式钉死。
    """
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    root = os.path.join(WORK, "root")
    os.makedirs(os.path.join(root, "scenes"))
    ents = []
    old = 1_000_000_000
    for i, rel in enumerate(RUNTIME_CASES):
        stem = os.path.splitext(rel)[0]
        d = os.path.join(root, os.path.dirname(rel))
        os.makedirs(d, exist_ok=True)
        for ext, mt in ((".fbx", old), (".vmesh", old + 100)):
            src = os.path.join(ASSETS, stem + ext)
            if not os.path.isfile(src):
                continue
            dst = os.path.join(d, os.path.basename(stem) + ext)
            shutil.copyfile(src, dst)
            os.utime(dst, (mt, mt))
        ents.append({
            "name": "M%d" % i,
            "transform": {"position": [i * 3.0, 0.0, 0.0],
                          "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]},
            "parent": -1, "visible": True, "castShadow": True,
            "mesh": {"kind": "fbx", "path": rel,
                     "modelKey": "%s#vfmt%d" % (rel, i), "subMeshIndex": 0},
        })
    with open(os.path.join(root, "scenes", "probe.json"), "w",
              encoding="utf-8") as f:
        json.dump({"version": 1, "generator": "verify_vertex_format 探针",
                   "entities": ents}, f, ensure_ascii=False, indent=2)
    return root


def main() -> int:
    for exe in (ASSETC, EDITOR):
        if not os.path.isfile(exe):
            log("SKIP 缺少 %s" % exe)
            return 0

    # ================= A. 磁盘上的六个资产都是新格式 =================
    for rel, want in BASE.items():
        path = os.path.join(ASSETS, rel)
        if not os.path.isfile(path):
            fails.append("A0 缺少资产 %s" % rel)
            log("CHECK A0 缺少资产 %s FAIL" % rel)
            continue
        code, out = run([ASSETC, "--info", path])
        m = re.search(r"vfmt meshVersion=(\d+) stride=(\d+)", out)
        tag = os.path.basename(rel)
        check("A1 %s stride=112 / meshVersion=2" % tag,
              bool(m) and int(m.group(2)) == STRIDE and int(m.group(1)) == MESH_VER,
              m.group(0) if m else "没有 vfmt 行")
        m2 = re.search(r"submeshes=(\d+) vertices=(\d+) indices=(\d+)", out)
        m3 = re.search(r"joints=(\d+) clips=(\d+)", out)
        got = (int(m2.group(1)), int(m2.group(2)), int(m2.group(3)),
               int(m3.group(1)), int(m3.group(2))) if (m2 and m3) else ()
        check("A2 %s 重烤前后账目一致" % tag, got == want,
              "%s vs 期望 %s" % (got, want))

    # ================= B. 结构不变量（直解字节）=================
    stats = {}
    for rel in BASE:
        path = os.path.join(ASSETS, rel)
        if os.path.isfile(path):
            stats[rel] = vertex_stats(path)

    check("B0 六个资产的 stride 都是 %d" % STRIDE,
          all(s["stride"] == STRIDE for s in stats.values()) and len(stats) == 6,
          "/".join(sorted({str(s["stride"]) for s in stats.values()})))
    # §14：第 88 字节的 `morphCount` 与 `morphBase` 必须严格自洽。
    # "有 base 没 count"（或反过来）在写端是不可能出现的 ——
    # `buildMorphStream` 两个字段一起写。这里红了说明写端有分支漏了。
    check("B1 morphBase / morphCount 严格自洽（不存在半份 morph 标记）",
          all(s["morphBad"] == 0 for s in stats.values()),
          ", ".join("%s=%d" % (os.path.basename(k), v["morphBad"])
                    for k, v in stats.items() if v["morphBad"]))
    # morph 的**正例**：只有 alicia 的 FBX 带 blend shape（60 个 channel）。
    # 其余五个必须全是哨兵 —— 这一条同时证明两件事：
    #   ① 没有 morph 的资产不会凭空长出 morph 标记；
    #   ② 有 morph 的资产确实把标记填进去了（不是解析器整体挂了）。
    with_morph = [k for k, v in stats.items() if v["morph"] > 0]
    only_alicia = with_morph == ["models/characters/alicia.vmesh"]
    al = stats.get("models/characters/alicia.vmesh", {})
    check("B2 只有 alicia 带 morph 标记，且 base/count 一一对应",
          only_alicia and al.get("morph", 0) == al.get("morphCount", -1)
          and al.get("morph", 0) > 0,
          "带 morph 的资产=[%s]；alicia base=%s count=%s" %
          (", ".join(os.path.basename(x) for x in with_morph),
           al.get("morph"), al.get("morphCount")))
    # 切线"全或无"：一个网格要么整体带切线层，要么整体没有。
    # 不做"必须全 0"的硬断言 —— 那样将来加一个带切线的资产就会红，
    # 而红的理由是"这刀还没写"，不是回归。
    check("B3 切线全或无（不存在半份切线层）",
          all(s["tangent"] in (0, s["total"]) for s in stats.values()),
          ", ".join("%s=%d/%d" % (os.path.basename(k), v["tangent"], v["total"])
                    for k, v in stats.items()))
    # UV2 的正例：animate_girl 的 FBX 真的带两套 UV（Blender 里开了第二层）。
    # 这条是"导入器确实读了 uv_sets[1]"的唯一正面证据 —— 其余五个资产
    # 都只有一层，全 0 分不清"没读"和"真没有"。
    ag = stats.get("models/characters/animate_girl.vmesh", {})
    check("B4 animate_girl 的 UV2 来自真实数据（全顶点非零）",
          ag.get("uv2", 0) == ag.get("total", -1) and ag.get("total", 0) > 0,
          "uv2=%s/%s" % (ag.get("uv2"), ag.get("total")))
    zeros = [k for k, v in stats.items()
             if k != "models/characters/animate_girl.vmesh" and v["uv2"] != 0]
    check("B5 只有单层 UV 的资产 uv2 保持全 0（缺省不影响）",
          not zeros, ", ".join(os.path.basename(z) for z in zeros))

    # ================= C. 确定性 =================
    src_fbx = os.path.join(ASSETS, "models", "anime_girl.fbx")
    if os.path.isfile(src_fbx):
        os.makedirs(WORK, exist_ok=True)
        a = os.path.join(WORK, "det_a.vmesh")
        b = os.path.join(WORK, "det_b.vmesh")
        for out in (a, b):
            run([ASSETC, src_fbx, "--asset-root", ASSETS, "-o", out])
        ha = hashlib.sha256(open(a, "rb").read()).hexdigest() if os.path.isfile(a) else "-"
        hb = hashlib.sha256(open(b, "rb").read()).hexdigest() if os.path.isfile(b) else "-"
        # 确定性仍然是硬要求：vmesh 是**签入仓库**的产物，同机同源两次
        # 编译不一致意味着"谁烤的谁负责"的追溯彻底失效。
        check("C1 同一输入烤两次逐字节相同", ha == hb and ha != "-",
              ha[:16] + " vs " + hb[:16])

    # ================= D. 运行期解析出的统计量与文件一致 =================
    root = prepare_runtime_root()
    code, out = run([EDITOR], {
        "MYVK_ASSET_DIR": root,
        "MYVK_EDITOR_SCENE": "scenes/probe.json",
        "MYVK_FRAMES": "60",
        "MYVK_NO_WINDOW_SAVE": "1",
    })
    if code != 0:
        log("EDITOR exit=%d" % code)
    fmt = re.findall(r"VMESH-VFMT stride=(\d+) verts=(\d+) uv2=(\d+) "
                     r"tangent=(\d+) morphVerts=(\d+) morphDeltas=(\d+) "
                     r"morphTargets=(\d+) morphBad=(\d+)", out)
    check("D1 三个探针模型都走 .vmesh 并打出 VFMT 行", len(fmt) == len(RUNTIME_CASES),
          "%d 行 / 期望 %d（loaded=%d, FBX 回退=%d）" %
          (len(fmt), len(RUNTIME_CASES), out.count("VMESH loaded"),
           out.count("FBX loaded")))
    check("D2 运行期 stride 全为 112",
          bool(fmt) and all(int(f[0]) == STRIDE for f in fmt),
          ",".join(f[0] for f in fmt))
    # 运行期读到的 uv2 计数必须与 Python 直解文件的计数相同 —— 两条
    # 独立路径（C++ 加载器 / Python 解析器）互为校验。只查一条会漏掉
    # "两边都错得一样"这种情况（比如两边都按错的偏移读）。
    pairs = []
    for (rel_fbx, f) in zip(RUNTIME_CASES, fmt):
        rel_vm = os.path.splitext(rel_fbx)[0] + ".vmesh"
        s = stats.get(rel_vm)
        if s is None:
            continue
        pairs.append((os.path.basename(rel_vm), int(f[1]), s["total"], int(f[2]), s["uv2"]))
    check("D3 运行期顶点数 / uv2 计数与文件解析一致",
          bool(pairs) and all(p[1] == p[2] and p[3] == p[4] for p in pairs),
          "; ".join("%s v%d/%d uv2 %d/%d" % p for p in pairs))
    # morph 也要两条路径互校。alicia 是唯一有 delta 的探针，所以这里
    # 能真正比"运行期读到的 delta 条数 == Python 直解 MORPH 区块的条数"。
    mpairs = []
    for (rel_fbx, f) in zip(RUNTIME_CASES, fmt):
        rel_vm = os.path.splitext(rel_fbx)[0] + ".vmesh"
        s = stats.get(rel_vm)
        if s is None:
            continue
        mpairs.append((os.path.basename(rel_vm), int(f[4]), s["morph"], int(f[7])))
    check("D4 运行期 morph 顶点数与文件解析一致，且无矛盾标记",
          bool(mpairs) and all(p[1] == p[2] and p[3] == 0 for p in mpairs),
          "; ".join("%s morphVerts %d/%d bad=%d" % p for p in mpairs))

    # ================= E. 老属性零变更（需要一次性备份）=================
    #
    # 依赖 `_venn_verify/vmesh_bak`（§13 手术前手动留的六个旧格式文件）。
    # 这个备份**不可再生**（当前代码只会写 112 B），所以没有就跳过而不是
    # 报失败 —— 判据依赖临时产物时必须能自己退场，否则它会在新机器上
    # 永远红，然后被人删掉。
    if not os.path.isdir(BAK):
        log("SKIP E 组（没有 %s，无法验证老属性零变更）" % BAK)
    else:
        mismatched, verified = [], 0
        for rel in BASE:
            new_path = os.path.join(ASSETS, rel)
            # 备份名 = 仓库相对路径（含 assets/ 前缀）把 `/` 换成 `_`
            old_path = os.path.join(BAK, ("assets/" + rel).replace("/", "_"))
            if not (os.path.isfile(new_path) and os.path.isfile(old_path)):
                continue
            o_data, _omv, o_stride, o_subs, _oi = parse_vmesh(old_path)
            n_data, _nmv, n_stride, n_subs, _ni = parse_vmesh(new_path)
            if len(o_subs) != len(n_subs):
                mismatched.append("%s: 子网格数" % os.path.basename(rel))
                continue
            bad = 0
            vtot = 0
            for (ovc, _oic, obase, _ooi), (nvc, _nic, nbase, _noi) in zip(o_subs, n_subs):
                if ovc != nvc:
                    bad = -1
                    break
                vtot += ovc
                for k in range(ovc):
                    o = o_data[obase + k * o_stride: obase + k * o_stride + 80]
                    n = n_data[nbase + k * n_stride: nbase + k * n_stride + 80]
                    if o != n:
                        bad += 1
            if bad:
                mismatched.append("%s: %d 个顶点" % (os.path.basename(rel), bad))
            else:
                verified += vtot
        # ⚠ `verified > 0` 是这条判据的一半。第一版只判 `not mismatched`，
        #   而备份文件名拼错时循环一次都没进 —— mismatched 为空 → 判 PASS，
        #   详情里写着"0 个顶点已验证"。**一个可以零证据通过的断言，
        #   比没有这条断言更危险**：它会让人以为这块被看着了。
        check("E1 老 80 字节逐字节未变（全部资产、全部顶点）",
              not mismatched and verified > 0,
              ("%d 个顶点已验证" % verified) if not mismatched
              else "; ".join(mismatched))

    log("")
    if fails:
        for f in fails:
            log("  ✗ %s" % f)
        log("结果: FAIL: %d 项" % len(fails))
        return 1
    log("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
