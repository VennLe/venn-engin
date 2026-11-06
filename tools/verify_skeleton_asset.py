"""验收：骨架独立成资产（对标 UE 的 Skeleton 资产 `SK_Mannequin`）。

用户原话（2026-11-06）：

    "把角色模型的动作，姿势，骨骼绑定，动画完全按照 ue5 中的风格进行改造，
     参考 …/Content/Characters/Mannequins 这个其中的结构"

UE 那边的结构与本工程的对应关系：

    SK_Mannequin.uasset       ←→  assets/characters/mannequin/Rigs/SK_Mannequin.vskel
    SKM_Manny / SKM_Quinn     ←→  assets/models/characters/alicia.vmesh（引用上面那份）
    Meshes/ Rigs/ Anims/      ←→  assets/characters/mannequin/{Meshes,Rigs,Anims}
    socket（weapon_r/hand_r） ←→  Skeleton::sockets

判据全部走**离线工具**（`AssetC`），不需要 GPU、不需要开窗 —— 这一层
本来就该在构建机上跑得飞快：

  S0 出厂资产在位        alicia.vskel（128 骨/3 挂点）+ SK_Mannequin.vskel
                         （35 骨/3 挂点），且 alicia.vmesh 里写着引用
  S1 提取可用            `--extract-skel` 对一份 vmesh 副本能产出骨架资产，
                         产出的文件 `--check` 通过
  S2 导出不动模型        只导骨架时 vmesh **一个字节都不改**（`vmesh untouched`）
                         —— 这条防的是"顺手重烤把动画又压了一遍"
  S3 引用写进 vmesh      `--skel-ref` 之后 sections 6→7、`skelRef=` 有值、
                         `--check` 仍然通过
  S4 坏文件被拒          CRC 坏一个字节 → `--check` 非零退出（而不是崩，
                         也不是"读出一具空骨架"）
  S5 UE 目录结构         `assets/characters/mannequin/{Meshes,Rigs,Anims}` 三个目录
  S6 `--info` 按扩展名分流  `.vskel` 走 SKELINFO，不是 MESH 那套输出

用法：
    python tools/verify_skeleton_asset.py
    python tools/verify_skeleton_asset.py --keep     # 保留临时文件
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_fs import safe_remove, safe_rmtree                          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
ASSETC = os.path.join(BIN, "AssetC.exe")
ASSETS = os.path.join(BIN, "assets")
WORK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "skeleton_asset")

REL_CHAR = "models/characters/alicia.vmesh"
REL_SKEL = "models/characters/alicia.vskel"
REL_MQ_RIG = "characters/mannequin/Rigs/SK_Mannequin.vskel"

RE_SKELINFO = re.compile(r"joints=(\d+) topo=(\d+) sockets=(\d+) engineXform=(\d)")
RE_DIGEST = re.compile(r"boneDigest=([0-9a-f]{16})")
RE_SOCKET = re.compile(r'socket "([^"]+)" bone="([^"]+)" index=(-?\d+)')
RE_SKELREF = re.compile(r"skelRef=(\S+)")
RE_SECTION = re.compile(r"version=(\d+) flags=0x([0-9a-f]+) sections=(\d+)")
RE_EXTRACT = re.compile(r"ASSETC SKEL OK (\S+) joints=(\d+) sockets=(\d+) "
                        r"digest=([0-9a-f]{16})")


def run(args, timeout=120):
    """跑 AssetC，返回 (returncode, stdout)。"""
    p = subprocess.run([ASSETC] + args, cwd=BIN, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout)
    return p.returncode, p.stdout


def info_of(path):
    """`--info` 一次，返回 (rc, 行列表)。"""
    rc, out = run(["--info", path])
    return rc, out.splitlines()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留临时文件")
    args = ap.parse_args()

    if os.path.isdir(WORK):
        safe_rmtree(WORK)
    os.makedirs(WORK, exist_ok=True)

    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print("[ %s ] %s  %s" % (" OK " if ok else "FAIL", name, detail))
        return ok

    char = os.path.join(ASSETS, REL_CHAR.replace("/", os.sep))
    skel = os.path.join(ASSETS, REL_SKEL.replace("/", os.sep))
    mq_rig = os.path.join(ASSETS, REL_MQ_RIG.replace("/", os.sep))

    # ---------------- S0 出厂资产在位 ----------------
    rc_a, lines_a = info_of(skel)
    m_a = next((m for m in (RE_SKELINFO.search(l) for l in lines_a) if m), None)
    d_a = next((m for m in (RE_DIGEST.search(l) for l in lines_a) if m), None)
    rc_m, lines_m = info_of(mq_rig)
    m_m = next((m for m in (RE_SKELINFO.search(l) for l in lines_m) if m), None)
    rc_c, lines_c = info_of(char)
    m_ref = next((m for m in (RE_SKELREF.search(l) for l in lines_c) if m), None)
    check("S0 出厂骨架资产在位（alicia 128 骨 / SK_Mannequin 35 骨 / 都有 3 挂点）",
          rc_a == 0 and rc_m == 0 and m_a is not None and m_m is not None
          and int(m_a.group(1)) == 128 and int(m_a.group(3)) == 3
          and int(m_m.group(1)) == 35 and int(m_m.group(3)) == 3,
          "alicia=j%s/s%s mq=j%s/s%s" %
          (m_a.group(1) if m_a else "?",
           m_a.group(3) if m_a else "?",
           m_m.group(1) if m_m else "?",
           m_m.group(3) if m_m else "?"))
    check("S0b alicia.vmesh 里写着骨架引用（skelRef=%s）" % REL_SKEL,
          m_ref is not None and m_ref.group(1) == REL_SKEL,
          m_ref.group(0) if m_ref else "没有 skelRef=")
    alicia_digest = d_a.group(1) if d_a else ""

    # ---------------- S1 提取可用 ----------------
    work_char = os.path.join(WORK, "alicia.vmesh")
    shutil.copy2(char, work_char)
    size_before = os.path.getsize(work_char)
    rc, out = run([work_char, "--extract-skel", os.path.join(WORK, "extracted.vskel")])
    m_x = RE_EXTRACT.search(out)
    check("S1 `--extract-skel` 从 vmesh 产出骨架资产",
          rc == 0 and m_x is not None and int(m_x.group(2)) == 128
          and int(m_x.group(3)) == 3,
          m_x.group(0)[:100] if m_x else out.strip().splitlines()[-1:])
    rc_chk, out_chk = run(["--check", os.path.join(WORK, "extracted.vskel")])
    check("S1b 产出的 .vskel 能过 --check", rc_chk == 0, out_chk.strip()[:100])

    # ---------------- S2 导出不动模型 ----------------
    #
    # 这是实测踩出来的：`writeMeshFile` 对同一份数据再跑一遍**不逐字节
    # 幂等**（动画抽帧会在已经抽过的曲线上再掉几帧）。所以"只导骨架"这条
    # 路必须**不落盘** vmesh。
    size_after = os.path.getsize(work_char)
    check("S2 只导骨架时 vmesh 不改写（字节数不变）",
          size_before == size_after and "vmesh untouched" in out,
          "%d -> %d" % (size_before, size_after))

    # ---------------- S3 引用写进 vmesh ----------------
    rc, out = run([work_char, "--skel-ref", REL_SKEL])
    rc_i, lines_i = info_of(work_char)
    m_sec = next((m for m in (RE_SECTION.search(l) for l in lines_i) if m), None)
    m_ref2 = next((m for m in (RE_SKELREF.search(l) for l in lines_i) if m), None)
    check("S3 `--skel-ref` 写进 SKELREF 区块（sections 6→7，skelRef 有值）",
          rc == 0 and m_sec is not None and int(m_sec.group(3)) == 7
          and m_ref2 is not None and m_ref2.group(1) == REL_SKEL,
          "sections=%s ref=%s" % (m_sec.group(3) if m_sec else "?",
                                  m_ref2.group(1) if m_ref2 else "-"))
    rc_chk, out_chk = run(["--check", work_char])
    check("S3b 写入引用后 mesh 仍然 --check 通过", rc_chk == 0,
          out_chk.strip()[:100])

    # ---------------- S4 坏文件被拒 ----------------
    bad = os.path.join(WORK, "corrupt.vskel")
    shutil.copy2(os.path.join(WORK, "extracted.vskel"), bad)
    with open(bad, "r+b") as f:
        f.seek(os.path.getsize(bad) // 2)
        b = f.read(1)
        f.seek(os.path.getsize(bad) // 2)
        f.write(bytes([b[0] ^ 0xFF]))
    rc_bad, out_bad = run(["--check", bad])
    check("S4 坏一个字节 → --check 非零退出（CRC 抓到）",
          rc_bad != 0 and "crc" in out_bad.lower(),
          out_bad.strip()[:110])

    # ---------------- S5 UE 目录结构 ----------------
    missing_dirs = [d for d in ("Meshes", "Rigs", "Anims")
                    if not os.path.isdir(os.path.join(ASSETS, "characters",
                                                      "mannequin", d))]
    check("S5 目录按 UE 的 Mannequins 结构（Meshes/Rigs/Anims）",
          not missing_dirs, "missing=%s" % (",".join(missing_dirs) or "none"))

    # ---------------- S6 --info 按扩展名分流 ----------------
    # 同一条 `--info` 命令，两种文件必须走两套输出 —— 否则目录批处理
    # （`AssetC --info assets -r`）会把 .vskel 当网格去解，报一堆假错。
    check("S6 `--info` 对 .vskel 走 SKELINFO（不对网格格式做体检）",
          any("ASSETC SKELINFO" in l for l in lines_a)
          and not any("vfmt" in l for l in lines_a),
          "lines=%d" % len(lines_a))

    # 挂点落点（S0 的细化）：UE 骨名 -> 下标必须解析得出来
    socks = RE_SOCKET.findall("\n".join(lines_m))
    want = {"weapon_r": "hand_r", "weapon_l": "hand_l", "eye": "head"}
    ok_sock = all(any(n == k and b == v and int(i) >= 0 for n, b, i in socks)
                  for k, v in want.items())
    check("S7 SK_Mannequin 的挂点落在 UE 骨上（weapon_r/hand_r、weapon_l/hand_l、eye/head）",
          ok_sock, "sockets=%s" % (",".join("%s->%s" % (n, b) for n, b, _ in socks) or "-"))

    passed = sum(1 for r in results if r)
    failed = len(results) - passed
    print()
    if not args.keep:
        safe_rmtree(WORK)
    else:
        print("  临时文件保留在: %s" % WORK)
    if alicia_digest:
        print("  alicia.vskel digest=%s" % alicia_digest)
    print("结果: %s  (%d 通过 / %d 失败)" %
          ("PASS" if failed == 0 else "FAIL", passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
