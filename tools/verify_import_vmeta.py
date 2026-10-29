"""端到端验证：导入设置 sidecar `.vmeta`（W2-2，见 docs/world_scale_roadmap.md §7）。

断言的六件事：

  A 组  首次导入生成 sidecar
        `models/characters/animate_girl_rigged.glb.vmeta` 出现，`materials` 里有 7 条、
        条条等于 `模型路径#glTF材质名`，`sourceHash` 是 `fnv1a:0x` 开头。
        ⚠ 文件名是**追加** `.vmeta` 而不是替换扩展名 —— 否则 `.glb` 与 `.fbx`
          会抢同一个 sidecar（见 B3）。

  B 组  源没变 → 什么都不做
        B1 第二次加载日志 `changed=0 forced=0`
        B2 sidecar 的 mtime **逐位不变**（真的没重写）
        B3 `.glb` 与 `.fbx` 各有各的 sidecar（不互相覆盖）

  C 组  映射表说话
        手改 `.vmeta`：`Skin → custom/skin_v9` → `ASSET-MAT-ID` 的 id 跟着变。
        ⚠ 这条是 W2-2 的**目的**：材质身份记在 sidecar 里，不是按下标推的。

  D 组  源变了 → 按名字重建映射
        把 `sourceHash` 改成假值（等价于"源文件被改过"）+ 塞一条源里不存在的
        `Ghost` →
        D1 日志 `changed=1`
        D2 恰好一条 `ASSET-MAT-REMAP-FAIL model=... name=Ghost`
        D3 重写后的 sidecar 里 `Ghost` 被删掉（不留永久假警报）
        D4 已有的映射**保留**（`Skin` 仍是 `custom/skin_v9`）——
           "重导出后用户的材质覆盖还挂在原来那个材质上"

  E 组  改了 import 参数 → 强制重导
        `.vmeta` 的 `import.scale` 1.0 → 0.5 → 日志 `forced=1`，签名被刷新。

  F 组  导入参数真的生效（不是只写进文件）
        `import.scale` 0.5 时 FBX 的归一化半径 = 1.0 时的一半。
        ⚠ 用 FBX 路径验：它在加载末尾会打 `FBX normalized: ... radius=`，
          不需要开窗口就能量。

用法：
    python tools/verify_import_vmeta.py
"""

import json
import os
import re
import struct
import subprocess
import sys
from safe_fs import safe_remove

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
SCENES = os.path.join(BIN, "assets", "scenes")
MODELS = os.path.join(BIN, "assets", "models")

GLB_PROBE = "scenes/runtime_test/mat_model_probe.json"
FBX_PROBE_REL = "scenes/_vmeta_fbx_probe.json"

GLB_REL = "models/characters/animate_girl_rigged.glb"
FBX_REL = "models/characters/animate_girl.fbx"
# sidecar 的路径是**资产路径直接追加 `.vmeta`**（见下面的 A 组注释）。
GLB_VMETA = os.path.join(MODELS, "characters", "animate_girl_rigged.glb.vmeta")
FBX_VMETA = os.path.join(MODELS, "characters", "animate_girl.fbx.vmeta")

def _glb_material_names(path):
    """从 .glb 的 JSON chunk 里读材质名，外加"第 0 个子网格用的是哪一条"。

    ⚠ 别在这里钉死名字表。原来是 `["Skin","Hair","Cloth",...]` —— 那是
    `blender_make_girl.py` 那批资产的命名。换成 FAB 角色之后材质叫
    `材质` / `材质.001..006`，写死表会让 A2/D4 一起红，而"红"的原因和
    这条用例要验的东西（映射表 = 模型路径#材质名）毫无关系。
    """
    data = open(path, "rb").read()
    _, _, total = struct.unpack_from("<III", data, 0)
    off = 12
    while off < total:
        ln, ty = struct.unpack_from("<II", data, off)
        if ty == 0x4E4F534A:
            js = json.loads(data[off + 8: off + 8 + ln].decode("utf-8"))
            names = [m.get("name", "") for m in js.get("materials", [])]
            prim = (js.get("meshes") or [{}])[0].get("primitives") or [{}]
            mi = prim[0].get("material")
            first = names[mi] if isinstance(mi, int) and mi < len(names) else ""
            return names, first
        off += 8 + ln + ((4 - ln % 4) % 4 if ln % 4 else 0)
    return [], ""


GLTF_MAT_NAMES, MAT_KEY = _glb_material_names(
    os.path.join(ROOT, "assets", "models", "characters",
                 "animate_girl_rigged.glb"))
# C/D 组"手改映射"用的那条：第 0 个子网格的材质名（id 里的 `/mat0`）。
MAT_KEY = MAT_KEY or (GLTF_MAT_NAMES[0] if GLTF_MAT_NAMES else "Skin")

VMETA_RE = re.compile(r"ASSET-VMETA path=(\S+) hash=(\S+) changed=(\d+) "
                      r"forced=(\d+) mats=(\d+)")
MAT_ID_RE = re.compile(r"ASSET-MAT-ID model=(\S+) key=(\S+) id=(\S+)")
REMAP_RE = re.compile(r"ASSET-MAT-REMAP-FAIL model=(\S+) name=(\S+)")
RADIUS_RE = re.compile(r"FBX normalized:.*radius=([\d.]+)")

FBX_PROBE = {
    "version": 1,
    "generator": "verify_import_vmeta: FBX submesh 0, used only to read the "
                 "normalized radius under different import.scale values",
    "entities": [
        {
            "name": "GirlFbx",
            "transform": {"position": [0.0, 0.0, 0.0],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1, "visible": True, "castShadow": True,
            "mesh": {"kind": "fbx", "path": FBX_REL,
                     "modelKey": FBX_REL, "subMeshIndex": 0},
        },
    ],
}

g_pass = 0
g_fail = 0


def check(name, ok, detail=""):
    global g_pass, g_fail
    if ok:
        g_pass += 1
        print("[ OK ] %s%s" % (name, ("  " + detail) if detail else ""))
    else:
        g_fail += 1
        print("[FAIL] %s%s" % (name, ("  " + detail) if detail else ""))


def run(args, timeout=180):
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("MYVK_") and k != "MYVK_ASSET_DIR":
            env.pop(k, None)
    p = subprocess.run([GAME] + list(args), cwd=BIN, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=timeout)
    return p.returncode, p.stdout.splitlines()


def run_probe(scene_rel, frames=3):
    return run(["--scene", scene_rel, "--frames", str(frames)])[1]


def read_meta(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_meta(path, doc):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)


def find(regex, lines):
    return [m for m in (regex.search(l) for l in lines) if m]


def vmeta_line(lines):
    got = find(VMETA_RE, lines)
    return got[-1] if got else None


def cleanup():
    for p in (GLB_VMETA, FBX_VMETA,
              os.path.join(SCENES, "_vmeta_fbx_probe.json")):
        try:
            safe_remove(p)
        except OSError:
            pass


# ====================================================================

def group_a():
    print("\n== A 组：首次导入生成 sidecar ==")
    cleanup()
    lines = run_probe(GLB_PROBE)
    check("A1 首次加载后 sidecar 出现", os.path.exists(GLB_VMETA), GLB_VMETA)
    if not os.path.exists(GLB_VMETA):
        return
    doc = read_meta(GLB_VMETA)
    mats = doc.get("materials") or {}
    ok = (len(mats) == 7
          and all(mats.get(n) == GLB_REL + "#" + n for n in GLTF_MAT_NAMES))
    check("A2 映射表 7 条，条条 = 模型路径#材质名", ok,
          json.dumps(mats, ensure_ascii=False, sort_keys=True))
    check("A3 sourceHash 形状是 fnv1a:0x...",
          re.fullmatch(r"fnv1a:0x[0-9A-F]{16}", doc.get("sourceHash", ""))
          is not None, doc.get("sourceHash", ""))
    ids = find(MAT_ID_RE, lines)
    check("A4 7 条 ASSET-MAT-ID 与映射一致",
          len(ids) == 7 and all(m.group(3) == mats.get(m.group(3).split("#")[-1])
                                for m in ids),
          "%d 条" % len(ids))


def group_b():
    print("\n== B 组：源没变就什么都不做 ==")
    if not os.path.exists(GLB_VMETA):
        check("B1 源没变 → changed=0", False, "缺少 sidecar（A 组没跑成）")
        return
    before = os.stat(GLB_VMETA).st_mtime_ns
    lines = run_probe(GLB_PROBE)
    m = vmeta_line(lines)
    check("B1 第二次加载 changed=0 forced=0",
          bool(m) and m.group(3) == "0" and m.group(4) == "0",
          m.group(0).split("] ")[-1] if m else "none")
    after = os.stat(GLB_VMETA).st_mtime_ns
    # ⚠ mtime 比"内容相同"更严格：顺手证明**没写盘**，而不是"写了但内容一样"
    check("B2 sidecar 没被重写（mtime 逐位不变）", before == after,
          "%d -> %d" % (before, after))

    # B3：同一模型的 .fbx 必须有自己的 sidecar，不能和 .glb 抢
    with open(os.path.join(SCENES, "_vmeta_fbx_probe.json"), "w",
              encoding="utf-8") as f:
        json.dump(FBX_PROBE, f, indent=2, ensure_ascii=False)
    run_probe(FBX_PROBE_REL, frames=3)
    check("B3 .glb 与 .fbx 各有各的 sidecar（不互相覆盖）",
          os.path.exists(FBX_VMETA) and os.path.exists(GLB_VMETA),
          "%s / %s" % (os.path.exists(GLB_VMETA), os.path.exists(FBX_VMETA)))
    if os.path.exists(FBX_VMETA):
        glb_mats = read_meta(GLB_VMETA).get("materials") or {}
        fbx_mats = read_meta(FBX_VMETA).get("materials") or {}
        check("B3b 两份映射互不相干（都指向自己的源）",
              all(v.startswith(GLB_REL + "#") for v in glb_mats.values())
              and all(v.startswith(FBX_REL + "#") for v in fbx_mats.values()),
              "glb=%d fbx=%d" % (len(glb_mats), len(fbx_mats)))


def group_c():
    print("\n== C 组：映射表决定 id ==")
    if not os.path.exists(GLB_VMETA):
        check("C1 手改映射 → ASSET-MAT-ID 跟着改", False, "缺少 sidecar")
        return
    doc = read_meta(GLB_VMETA)
    doc.setdefault("materials", {})[MAT_KEY] = "custom/skin_v9"
    write_meta(GLB_VMETA, doc)
    lines = run_probe(GLB_PROBE)
    ids = find(MAT_ID_RE, lines)
    skin = [m for m in ids if m.group(2).endswith("/mat0")]
    check("C1 手改映射 → ASSET-MAT-ID 跟着改",
          bool(skin) and skin[0].group(3) == "custom/skin_v9",
          skin[0].group(3) if skin else "none")


def group_d():
    print("\n== D 组：源变了 → 按名字重建映射 ==")
    if not os.path.exists(GLB_VMETA):
        check("D1 源变了 → changed=1", False, "缺少 sidecar")
        return
    doc = read_meta(GLB_VMETA)
    # 改 hash 等价于"源文件被改过"（不去动仓库里的模型）
    doc["sourceHash"] = "fnv1a:0xDEADBEEFDEADBEEF"
    doc.setdefault("materials", {})["Ghost"] = GLB_REL + "#Ghost"
    write_meta(GLB_VMETA, doc)

    lines = run_probe(GLB_PROBE)
    m = vmeta_line(lines)
    check("D1 源变了 → changed=1", bool(m) and m.group(3) == "1",
          m.group(0).split("] ")[-1] if m else "none")
    remap = find(REMAP_RE, lines)
    check("D2 源里没了的名字 → ASSET-MAT-REMAP-FAIL",
          len(remap) == 1 and remap[0].group(2) == "Ghost",
          "%d 条: %s" % (len(remap),
                        remap[0].group(0) if remap else "none"))
    after = read_meta(GLB_VMETA)
    check("D3 陈旧的 Ghost 映射被删掉（不留永久假警报）",
          "Ghost" not in (after.get("materials") or {}),
          json.dumps(sorted(after.get("materials") or {}), ensure_ascii=False))
    check("D4 已有映射保留（%s 仍是 custom/skin_v9）" % MAT_KEY,
          (after.get("materials") or {}).get(MAT_KEY) == "custom/skin_v9",
          (after.get("materials") or {}).get(MAT_KEY, "none"))
    check("D5 记下的 hash 被刷成当前值",
          after.get("sourceHash", "").startswith("fnv1a:0x")
          and after.get("sourceHash") != "fnv1a:0xDEADBEEFDEADBEEF",
          after.get("sourceHash", "none"))


def group_e():
    print("\n== E 组：改了 import 参数 → 强制重导 ==")
    if not os.path.exists(GLB_VMETA):
        check("E1 改 import.scale → forced=1", False, "缺少 sidecar")
        return
    doc = read_meta(GLB_VMETA)
    doc["import"]["scale"] = 0.5
    write_meta(GLB_VMETA, doc)
    lines = run_probe(GLB_PROBE)
    m = vmeta_line(lines)
    check("E1 改 import.scale → forced=1", bool(m) and m.group(4) == "1",
          m.group(0).split("] ")[-1] if m else "none")
    sig0 = read_meta(GLB_VMETA).get("appliedImportSig", "")
    check("E2 应用签名被刷新（下次不会再判成被改过）",
          sig0.startswith("fnv1a:0x") and sig0 != "",
          sig0)
    lines = run_probe(GLB_PROBE)
    m = vmeta_line(lines)
    check("E3 再加载一次 forced 回到 0",
          bool(m) and m.group(4) == "0",
          m.group(0).split("] ")[-1] if m else "none")


def group_f():
    print("\n== F 组：import.scale 真的生效 ==")
    if not os.path.exists(FBX_VMETA):
        # B3 可能没建出来（.fbx 探针没跑过）→ 这里自己跑一遍
        with open(os.path.join(SCENES, "_vmeta_fbx_probe.json"), "w",
                  encoding="utf-8") as f:
            json.dump(FBX_PROBE, f, indent=2, ensure_ascii=False)
        run_probe(FBX_PROBE_REL)

    def radius_at(scale):
        doc = read_meta(FBX_VMETA) if os.path.exists(FBX_VMETA) else {}
        doc.setdefault("import", {})["scale"] = scale
        write_meta(FBX_VMETA, doc)
        lines = run_probe(FBX_PROBE_REL)
        got = find(RADIUS_RE, lines)
        return float(got[-1].group(1)) if got else 0.0

    r1 = radius_at(1.0)
    r_half = radius_at(0.5)
    # ⚠ 判据要"接近一半"，不能只判"变了"：只判不同的话，任何无关抖动都过。
    ok = r1 > 0.0 and abs(r_half / r1 - 0.5) < 0.05
    check("F1 import.scale=0.5 → 归一化半径约为 1.0 时的一半",
          ok, "r1=%.4f r0.5=%.4f 比值=%.3f"
              % (r1, r_half, (r_half / r1) if r1 else 0.0))


def main():
    if not os.path.exists(GAME):
        print("找不到可执行文件，请先构建：", GAME)
        return 1
    try:
        group_a()
        group_b()
        group_c()
        group_d()
        group_e()
        group_f()
    finally:
        cleanup()

    print("\n结果: %s  (pass=%d fail=%d)"
          % ("PASS" if g_fail == 0 else "FAIL: %d 项" % g_fail, g_pass, g_fail))
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
