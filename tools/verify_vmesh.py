#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""回归：离线资产链（FBX → `.vmesh` → 运行时加载）。

这条链有三段，每一段都有**独立的失败模式**，所以分开验：

  A. 离线编译   `AssetC` 能不能烤、烤得对不对（顶点/ACMR/关键帧的量化）
  B. 往返       烤出来的文件能不能被读回、CRC/截断能不能被拒
  C. 运行时     编辑器是不是**真的走了 .vmesh**（而不是悄悄回退 FBX），
                以及缓存过期时会不会自动回退
  E. 贴图容错   `.vmesh` 里的贴图路径失效时，模型还能不能照常渲染（阶段五 3.6）
  D. 引擎内自检 `MYVK_ASSET_TEST=1` 的 44 条断言

（E 排在 D 之前是执行顺序，不是编号顺序 —— 字母沿用需求文档里的组名，
  不想因为插了一组就把 D 改名，那样和历史日志对不上。）

⚠ C 段刻意**不做截图比对**：模型实体是 autoPlay 的，截图结果取决于
   wall clock（两个进程不可能落在同一帧），那样的断言只会随机红。
  几何正确性由 `MYVK_ASSET_TEST=1` 的逐字节顶点比较覆盖，那条才是
   真正的证据（见 engine/assets/AssetSelfTest.cpp）。

运行：python tools/verify_vmesh.py
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
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EDITOR = os.path.join(ROOT, "build", "bin", "Editor.exe")
ASSETC = os.path.join(ROOT, "build", "bin", "AssetC.exe")
WORK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "vmesh")

MODEL_REL = "models/characters/animate_girl.fbx"
VMESH_REL = "models/characters/animate_girl.vmesh"

fails: list[str] = []


def log(msg: str) -> None:
    print(msg, flush=True)


def check(name: str, ok: bool, detail: str = "") -> bool:
    log("CHECK %s %s%s" % (name, "OK" if ok else "FAIL", (" (%s)" % detail) if detail else ""))
    if not ok:
        fails.append("%s %s" % (name, detail))
    return ok


def run(cmd: list[str], env_extra: dict | None = None, timeout: int = 240):
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def prepare() -> str:
    """造一个**独立资产根**，不碰仓库里的 assets/。

    为什么不直接把 .vmesh 放进 assets/models：
      那会让**所有**其它用例悄悄改成走二进制路径（它们的历史基线是
      FBX 解析出来的）。测试之间的相互影响，比测试本身更容易埋雷。
    """
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    root = os.path.join(WORK, "root")
    os.makedirs(os.path.join(root, "models"), exist_ok=True)
    os.makedirs(os.path.join(root, "scenes"), exist_ok=True)

    src = os.path.join(ROOT, "assets", "models", "characters", "animate_girl.fbx")
    if not os.path.isfile(src):
        log("SKIP 缺少 %s" % src)
        sys.exit(0)
    # ⚠ `MODEL_REL` 带一层 `characters/`，这一层必须显式建出来，
    #   否则 copy2 抛 WinError 3。
    dst = os.path.join(root, MODEL_REL)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)

    scene = {
        "version": 1,
        "generator": "verify_vmesh: 一个引用 FBX 的模型实体，用来验证运行时"
                     "优先读同名 .vmesh（并能在缓存过期时回退）。",
        "entities": [
            {
                "name": "Girl",
                "transform": {
                    "position": [0.0, 0.0, 0.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                },
                "parent": -1,
                "visible": True,
                "castShadow": True,
                "mesh": {
                    "kind": "fbx",
                    "path": MODEL_REL,
                    "modelKey": MODEL_REL,
                    "subMeshIndex": 0,
                },
            }
        ],
    }
    with open(os.path.join(root, "scenes", "probe.json"), "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)
    return root


def run_editor(root: str, frames: int = 90) -> str:
    code, out = run(
        [EDITOR],
        {
            "MYVK_ASSET_DIR": root,
            "MYVK_EDITOR_SCENE": "scenes/probe.json",
            "MYVK_FRAMES": str(frames),
            "MYVK_NO_WINDOW_SAVE": "1",
        },
    )
    if code != 0:
        log("EDITOR exit=%d" % code)
    return out


# ---------------- .vmesh 的 Python 侧最小解析（E 组打补丁用）----------------
# 布局必须与 assetio/MeshFormat.cpp 逐项对齐：
#   文件头 32 B：magic(8) version(4) flags(4) sourceHash(8) sectionCount(4) reserved(4)
#   区块表 32 B/条：id(4) version(4) crc(4) reserved(4) offset(8) size(8)
#   MAT 载荷：version(4) count(4) 然后 count × 60 B
#             5 个字符串下标(20) + baseColor(16) + rough(4) + metal(4) + emissive(12) + doubleSided(4)
#   STR 载荷：count(4) 然后 count 条 "u32 长度 + 字节"
# 只读这三样：改 MAT 的下标、重算 CRC。其余字段一概不碰。

def _sections(data: bytes) -> dict:
    count = struct.unpack_from("<I", data, 24)[0]
    out = {}
    for i in range(count):
        e = 32 + i * 32
        sid, _ver, crc, _res, off, size = struct.unpack_from("<IIIIQQ", data, e)
        out[sid] = (e, off, size, crc)
    return out


def _strings(data: bytes, off: int, size: int) -> list[str]:
    n = struct.unpack_from("<I", data, off)[0]
    p = off + 4
    out = []
    for _ in range(n):
        ln = struct.unpack_from("<I", data, p)[0]
        p += 4
        out.append(data[p:p + ln].decode("utf-8", "replace"))
        p += ln
    return out


def break_texture_paths(vmesh: str) -> list[str]:
    """把每条材质的 `albedoPath` / `albedoRaw` 都指向**它自己的名字**。

    名字一定非空，也一定不是真文件（没有扩展名）→ 运行期必然解析失败 ——
    这正是"贴图路径失效"的现场（阶段五 3.6 要守的那条容错）。

    ⚠ 改完**重算 MAT 区块的 CRC32**，让文件仍然是**合法**的：这一组验的是
      "贴图没了还能不能渲染"，不是"文件坏了能不能拒"（后者归 B/E 之外的
      坏文件断言管）。用损坏文件冒充缺贴图，测出来的是另一条分支。
    """
    data = bytearray(open(vmesh, "rb").read())
    secs = _sections(data)
    if 4 not in secs or 5 not in secs:
        return []
    mat_e, mat_off, mat_size, _crc = secs[4]
    strs = _strings(data, secs[5][1], secs[5][2])
    nmat = struct.unpack_from("<I", data, mat_off + 4)[0]
    names = []
    for m in range(nmat):
        base = mat_off + 8 + m * 60
        name_idx = struct.unpack_from("<I", data, base)[0]
        names.append(strs[name_idx] if name_idx < len(strs) else "?")
        struct.pack_into("<I", data, base + 4, name_idx)   # albedoPath
        struct.pack_into("<I", data, base + 8, name_idx)   # albedoRaw
    struct.pack_into("<I", data, mat_e + 8,
                     zlib.crc32(bytes(data[mat_off:mat_off + mat_size])))
    with open(vmesh, "wb") as f:
        f.write(data)
    return names


def parse_assetc(out: str) -> dict:
    """从 AssetC 的输出里抽数字。抽不到就是 None —— 由 check 报出来。"""
    d: dict = {}
    m = re.search(r"vertices (\d+) -> (\d+)\s+indices (\d+) -> (\d+)\s+acmr "
                  r"([\d.]+) -> ([\d.]+)", out)
    if m:
        d["vin"], d["vout"] = int(m.group(1)), int(m.group(2))
        d["iin"], d["iout"] = int(m.group(3)), int(m.group(4))
        d["acmr_in"], d["acmr_out"] = float(m.group(5)), float(m.group(6))
    m = re.search(r"keys (\d+) -> (\d+)", out)
    if m:
        d["keys_in"], d["keys_out"] = int(m.group(1)), int(m.group(2))
    m = re.search(r"submeshes=(\d+) joints=(\d+) clips=(\d+) materials=(\d+)", out)
    if m:
        d["submeshes"] = int(m.group(1))
        d["joints"] = int(m.group(2))
        d["clips"] = int(m.group(3))
        d["materials"] = int(m.group(4))
    m = re.search(r"geometry ([\d.]+ MB|[\d.]+ KB|[\d]+ B) -> file ([\d.]+ MB|[\d.]+ KB|[\d]+ B) \(([\d.]+)x\)", out)
    if m:
        d["ratio"] = float(m.group(3))
    return d


def main() -> int:
    for exe in (EDITOR, ASSETC):
        if not os.path.isfile(exe):
            log("SKIP 找不到 %s（先构建）" % exe)
            return 0

    root = prepare()
    fbx = os.path.join(root, MODEL_REL)
    vmesh = os.path.join(root, VMESH_REL)

    # ---------------- A. 离线编译 ----------------
    code, out = run([ASSETC, fbx, "--asset-root", root, "--scale", "1.0"])
    if not check("A1 AssetC 退出码 0", code == 0, "rc=%d" % code):
        log(out[-2000:])
        return finish()

    st = parse_assetc(out)
    check("A2 烤出 .vmesh", os.path.isfile(vmesh))
    check("A3 子网格/骨骼/动画都在",
          st.get("submeshes", 0) > 0 and st.get("joints", 0) > 0 and st.get("clips", 0) > 0,
          "sub=%s joints=%s clips=%s" % (st.get("submeshes"), st.get("joints"), st.get("clips")))
    # 焊接必须显著减少顶点（FBX 的"每面角一个顶点"是主要浪费）
    weld_ok = st.get("vin", 0) > 0 and st.get("vout", 0) < st["vin"] * 0.5
    check("A4 顶点焊接生效（至少砍掉一半）", weld_ok,
          "%s -> %s" % (st.get("vin"), st.get("vout")))
    # 索引总数不能变（少一个就是模型破洞）
    check("A5 三角形数不变", st.get("iin") == st.get("iout"),
          "%s -> %s" % (st.get("iin"), st.get("iout")))
    # 缓存重排必须让 ACMR 下降（或至少不升）
    check("A6 ACMR 不升（重排有效）",
          st.get("acmr_out", 9.0) <= st.get("acmr_in", 0.0) + 1e-4,
          "%s -> %s" % (st.get("acmr_in"), st.get("acmr_out")))
    # 动画抽帧：必须有明显下降，但不是"删光"
    check("A7 动画关键帧被压缩", 0 < st.get("keys_out", 0) < st.get("keys_in", 0),
          "%s -> %s" % (st.get("keys_in"), st.get("keys_out")))
    check("A8 压缩比 > 1.5x", st.get("ratio", 0.0) > 1.5,
          "%.2fx" % st.get("ratio", 0.0))

    # ---------------- B. 工具自洽 ----------------
    code, out = run([ASSETC, "--info", vmesh])
    m = re.search(r"version=(\d+) flags=0x([0-9a-f]+) sections=(\d+)", out)
    check("B1 --info 头部可读", bool(m) and m.group(1) == "1",
          m.group(0) if m else "no header line")
    m2 = re.search(r"submeshes=(\d+) vertices=(\d+) indices=(\d+) index16=(\d)", out)
    info_ok = bool(m2) and int(m2.group(1)) == st.get("submeshes") and int(m2.group(2)) == st.get("vout")
    check("B2 --info 计数与写盘一致", info_ok, m2.group(0) if m2 else "no counts")

    code, out = run([ASSETC, "--check", vmesh])
    check("B3 --check 通过", code == 0 and "ASSETC OK" in out, out.strip().splitlines()[-1] if out.strip() else "")

    # 确定性：同一输入烤两次必须逐字节相同（否则构建不可复现）
    h1 = hashlib.sha256(open(vmesh, "rb").read()).hexdigest()
    run([ASSETC, fbx, "--asset-root", root, "-o", vmesh])
    h2 = hashlib.sha256(open(vmesh, "rb").read()).hexdigest()
    check("B4 两次编译逐字节相同", h1 == h2, h1[:16] + " vs " + h2[:16])

    # 关掉优化 → 顶点数应当回到原始值（证明开关真的接上了，而不是装饰）
    raw_out = os.path.join(WORK, "raw.vmesh")
    code, out = run([ASSETC, fbx, "--asset-root", root, "--no-weld",
                     "--no-optimize", "--no-anim-reduce", "-o", raw_out])
    raw = parse_assetc(out)
    check("B5 --no-weld 真的不焊", raw.get("vout") == raw.get("vin") and raw.get("vin", 0) > 0,
          "%s -> %s" % (raw.get("vin"), raw.get("vout")))
    check("B6 --no-anim-reduce 真的不抽帧", raw.get("keys_out") == raw.get("keys_in"),
          "%s -> %s" % (raw.get("keys_in"), raw.get("keys_out")))
    check("B7 优化后的文件更小", os.path.getsize(vmesh) < os.path.getsize(raw_out),
          "%d vs %d" % (os.path.getsize(vmesh), os.path.getsize(raw_out)))

    # ---------------- C. 运行时 ----------------
    out = run_editor(root)
    baseline_tex_warn = out.count("VMESH-TEX-MISSING")
    check("C1 运行时走的是 .vmesh", "VMESH loaded" in out,
          "见 C2 日志" if "VMESH loaded" in out else "没看到 VMESH loaded")
    check("C2 没有回退到 FBX 解析", "FBX loaded" not in out,
          "" if "FBX loaded" not in out else "出现了 FBX loaded")
    m = re.search(r"VMESH loaded: .*?\((\d+) submeshes, (\w+), (\d+) joints, "
                  r"(\d+) clips, radius=([\d.]+), src=([^,]+), ([\d.]+) ms\)", out)
    if m:
        check("C3 子网格数一致", int(m.group(1)) == st.get("submeshes"),
              "%s / %s" % (m.group(1), st.get("submeshes")))
        check("C4 蒙皮判定一致", m.group(2) == "skinned", m.group(2))
        check("C5 骨骼数一致", int(m.group(3)) == st.get("joints"),
              "%s / %s" % (m.group(3), st.get("joints")))
        check("C6 动画片段数一致", int(m.group(4)) == st.get("clips"),
              "%s / %s" % (m.group(4), st.get("clips")))
        vmesh_ms = float(m.group(7))
    else:
        fails.append("C3 缺少 VMESH loaded 行")
        vmesh_ms = -1.0

    # 回退：把 .vmesh 挪走 → 必须回到 FBX（而不是报错/空白）
    os.rename(vmesh, vmesh + ".bak")
    out = run_editor(root)
    check("C7 缓存缺失时回退 FBX", "FBX loaded" in out and "VMESH loaded" not in out)
    m = re.search(r"FBX loaded: .*\((\d+) submeshes, (\w+), (\d+) joints, ([\d.]+) ms\)", out)
    fbx_ms = float(m.group(4)) if m else -1.0
    if m:
        check("C8 回退路径的子网格数一致", int(m.group(1)) == st.get("submeshes"),
              "%s / %s" % (m.group(1), st.get("submeshes")))
    os.rename(vmesh + ".bak", vmesh)

    # 过期判据：把源文件改新 → 必须回退 FBX，并打出 STALE
    now = os.path.getmtime(vmesh)
    os.utime(fbx, (now + 10, now + 10))
    out = run_editor(root)
    check("C9 源比缓存新时回退 FBX", "MESH-BIN-STALE" in out and "FBX loaded" in out)
    check("C10 过期时不再读 .vmesh", "VMESH loaded" not in out)

    # 加载耗时（同机同时段，只是量级对比；不作为硬门槛）
    if vmesh_ms > 0 and fbx_ms > 0:
        check("C11 .vmesh 加载更快", vmesh_ms < fbx_ms,
              "%.1f ms vs %.1f ms (%.1fx)" % (vmesh_ms, fbx_ms, fbx_ms / vmesh_ms))

    # ---------------- E. 贴图路径容错（阶段五 3.6）----------------
    # 现场：`.vmesh` 里记着贴图路径，但贴图被人删了 / 拷到别的机器上丢了。
    # 期望：模型照常加载、走默认材质、打 WARN，**不许**崩、不许整模型失败。
    #
    # ⚠ 必须先把 C9 改过的 mtime 还原 —— 那一步故意把 .fbx 拨到".vmesh"之后，
    #   不还原的话下面每次都会走 STALE 回退 FBX，E 组就在测另一条路。
    old = os.path.getmtime(vmesh) - 100
    os.utime(fbx, (old, old))
    os.utime(vmesh, (old + 50, old + 50))

    names = break_texture_paths(vmesh)
    check("E1 打补丁后文件仍然合法（--check 通过）",
          bool(names) and run([ASSETC, "--check", vmesh])[0] == 0,
          "materials=%d" % len(names))
    out = run_editor(root)
    check("E2 贴图缺失时模型照常加载", "VMESH loaded" in out and "FBX loaded" not in out)
    check("E3 打出了 VMESH-TEX-MISSING",
          out.count("VMESH-TEX-MISSING") > baseline_tex_warn,
          "%d -> %d 条" % (baseline_tex_warn, out.count("VMESH-TEX-MISSING")))
    check("E4 没有 Vulkan 错误", "VK_ERROR" not in out and "VK_CHECK" not in out,
          "" if "VK_ERROR" not in out else "出现 VK_ERROR")
    # ⚠ 这里**不能**写成 `check(..., (x in out, "want=.."))` —— 那样第二个参数
    #   是个**元组**，恒为真，断言等于没写。先算成变量再传。
    sub_kept = ("(%d submeshes" % st.get("submeshes", -1)) in out
    check("E5 几何不受影响（子网格数不变）", sub_kept,
          "want=%s" % st.get("submeshes"))

    # ---------------- D. 引擎内自检 ----------------
    code, out = run([EDITOR], {"MYVK_ASSET_TEST": "1"})
    total = re.search(r"ASSET-TOTAL pass=(\d+) fail=(\d+)", out)
    check("D1 引擎内资产自检 fail=0", bool(total) and total.group(2) == "0",
          total.group(0) if total else "no ASSET-TOTAL")
    if total:
        check("D2 自检条数足够（>= 35）", int(total.group(1)) >= 35,
              "pass=%s" % total.group(1))
    check("D3 没有跳过（模型真的被导入过）", "SKIP" not in out)

    return finish()


def finish() -> int:
    if fails:
        log("")
        for f in fails:
            log("  ✗ %s" % f)
        log("结果: FAIL: %d 项" % len(fails))
        return 1
    log("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
