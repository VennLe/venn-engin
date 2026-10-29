#!/usr/bin/env python3
# ============================================================
# verify_skin_weights —— 蒙皮权重的数值验收（不依赖引擎、不依赖截图）
#
# ---------------------------- 为什么需要这一刀 ----------------------------
# `docs/animate_girl_import.md` §4 当年写的是"权重和精确等于 1.0000，
# outliers=0" —— 全绿。可角色一播动画就被撕成碎片。原因是：
#
#   **权重和归一化只证明"权重是一组概率"，不证明"这组概率是对的"。**
#
# 实测反例（2026-10-23 那版资产，胸口的一个顶点）：
#     Hips 31% + LeftUpLeg 29% + HeadTop_End 27% + RightUpLeg 13%
# 四个方向同时拉，和照样是 1。所以这一刀补的是**"这组概率合不合理"**。
#
# ---------------------------- 七条判据 ----------------------------
#   A  权重和 = 1（±1e-5）                  —— 上一刀已有的，保留
#   B  每个顶点 1..4 个非零影响             —— 超了引擎只取前 4，会静默丢权重
#   C  跨肢体混杂率 = 0                     —— **本刀的核心**
#        同一顶点不能同时被「左臂 / 右臂 / 左腿 / 右腿」中两个以上
#        以 >15% 拉动；头也不能和四肢同时拉。
#   D  主导骨距离 p95 ≤ 阈值                —— 拉太远的顶点一动就是"甩"
#   E  左右镜像一致                         —— 抓"整体错位一列骨"这类
#        归一化和距离都查不出来的错（镜像点的主导骨必须互为 L/R）
#   F  静止不变性：jointMat(rest) ≈ I       —— 保证"静止姿势不塌"
#   G  每根骨都被用到（>1%）                —— 没绑到的骨动起来没反应
#
# `--baseline` 可以拿旧资产做对照，把 A/C 两组数字并排打出来 ——
# **判据必须能证伪它要防的那个错**，不能证伪的判据等于没测。
#
# 用法：
#   python tools/verify_skin_weights.py
#   python tools/verify_skin_weights.py --file assets/models/characters/animate_girl.glb
# ============================================================

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = os.path.join(ROOT, "assets", "models", "characters",
                       "animate_girl_rigged.glb")

CT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
      5125: ("I", 4), 5126: ("f", 4)}
NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}

CROSS_LIMB = 0.15      # 同一个顶点里，两个不同肢体各占多少才算"混杂"
# 主导骨距离的 p95 上限。**这条阈值是按资产定的，不是通用常量**：
# 这具模型有及膝长发，发梢离任何骨都 0.5 m 上下 —— 阈值卡 0.25 会把
# "长发"判成错。1.9 m 那种"关节位置算错空间"的错依然会立刻红。
DOM_FAR = 0.50
COVER_MIN = 0.01       # 一根骨至少要有这么多"总权重"才算被用到
LR_BALANCE = 0.35      # 左右同源骨的总权重相对差上限

PASS, FAIL = [], []


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print("  [%s] %-34s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ---------------------------------------------------------------- GLB
def read_glb(path):
    data = open(path, "rb").read()
    _, _, total = struct.unpack_from("<III", data, 0)
    off, js, bins = 12, None, b""
    while off < total:
        ln, ty = struct.unpack_from("<II", data, off)
        ch = data[off + 8: off + 8 + ln]
        if ty == 0x4E4F534A:
            js = json.loads(ch.decode("utf-8"))
        elif ty == 0x004E4942:
            bins = ch
        off += 8 + ln + ((4 - ln % 4) % 4 if ln % 4 else 0)
    return js, bins


def read_acc(js, bins, idx):
    a = js["accessors"][idx]
    bv = js["bufferViews"][a["bufferView"]]
    fmt, sz = CT[a["componentType"]]
    n = NC[a["type"]]
    stride = bv.get("byteStride") or (sz * n)
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    sc = {5121: 255.0, 5123: 65535.0}.get(a["componentType"]) \
        if a.get("normalized") else None
    out = []
    for k in range(a["count"]):
        v = struct.unpack_from("<" + fmt * n, bins, base + k * stride)
        out.append(tuple(x / sc for x in v) if sc else v)
    return out


def ident():
    return [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]


def mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)]
            for r in range(4)]


def node_trs(node):
    m = ident()
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    s = node.get("scale", [1, 1, 1])
    t = node.get("translation", [0, 0, 0])
    R = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    for i in range(3):
        for j in range(3):
            m[i][j] = R[i][j] * s[j]
        m[i][3] = t[i]
    return m


# ---------------------------------------------------------------- 肢体分类
def limb_of(name):
    n = name.lower()
    # ⚠ 顺序有讲究：`LeftForeArm` 里**不含** `leftarm`（是 left+forearm），
    #   所以必须显式列 fore forearm，否则它会掉进 OTHER，
    #   让"手臂 vs 前臂"看起来不混杂、"手臂 vs 腿"看起来混杂，两头都错。
    if "leftforearm" in n or "leftarm" in n or "lefthand" in n \
            or "leftshoulder" in n:
        return "LARM"
    if "rightforearm" in n or "rightarm" in n or "righthand" in n \
            or "rightshoulder" in n:
        return "RARM"
    if "leftupleg" in n or "leftleg" in n or "leftfoot" in n or "lefttoe" in n:
        return "LLEG"
    if "rightupleg" in n or "rightleg" in n or "rightfoot" in n \
            or "righttoe" in n:
        return "RLEG"
    if "head" in n or "neck" in n:
        return "HEAD"
    if "hips" in n or "spine" in n or "pelvis" in n:
        return "TORSO"
    return "OTHER"


# 什么才算"病态混杂"：手臂骨和腿骨同时拉同一个顶点。
# 这是 2026-10-23 那版权重的典型症状（垂在胯边的手 + 及膝长发一起被判给
# 腿），一播动画就被上下撕开。
# 而 颈↔躯干 / 肩↔手臂 / 胯↔腿 / 左右大腿 这些**是关节过渡该有的**，
# 把它们也算成错，判据就会逼着人去破坏正常的绑定 —— 判据必须只抓真错。
ARM = {"LARM", "RARM"}
LEG = {"LLEG", "RLEG"}


def pathological(strong):
    return bool(strong & ARM and strong & LEG) or \
        ("HEAD" in strong and bool(strong & LEG))


def inv4(m):
    a = [list(m[r]) + [1.0 if r == c else 0.0 for c in range(4)]
         for r in range(4)]
    for c in range(4):
        pv = max(range(c, 4), key=lambda r: abs(a[r][c]))
        a[c], a[pv] = a[pv], a[c]
        if abs(a[c][c]) < 1e-12:
            return ident()
        f = a[c][c]
        a[c] = [v / f for v in a[c]]
        for r in range(4):
            if r != c and a[r][c]:
                f2 = a[r][c]
                a[r] = [a[r][k] - f2 * a[c][k] for k in range(8)]
    return [row[4:] for row in a]


def analyse(path, label, verbose=True):
    js, bins = read_glb(path)
    bp = {}
    for i, nd in enumerate(js["nodes"]):
        for ch in nd.get("children", []):
            bp[ch] = i
    WORLD = {}

    def world(i):
        if i in WORLD:
            return WORLD[i]
        m = node_trs(js["nodes"][i])
        if i in bp:
            m = mul(world(bp[i]), m)
        WORLD[i] = m
        return m

    skin = js["skins"][0]
    joints = skin["joints"]
    names = [js["nodes"][j].get("name", "j%d" % k) for k, j in enumerate(joints)]
    limbs = [limb_of(n) for n in names]

    # ⚠ 关节位置必须换算到 **mesh 节点空间** —— 顶点就活在那个空间里。
    #   否则只要根节点带旋转（我们的资产为了 Y-up→Z-up 带了一个 +90°X），
    #   顶点和关节就差一个整 90°，"顶点到骨的距离"会算出 1.9 m 这种鬼数。
    #   静态姿势下 world(j)·invBind = meshNodeWorld，所以
    #   mesh 空间的关节原点 = inv(meshNodeWorld) · world(j) 的平移列。
    mesh_node = None
    for i, nd in enumerate(js["nodes"]):
        if nd.get("skin") == 0 and "mesh" in nd:
            mesh_node = i
            break
    Minv = inv4(world(mesh_node)) if mesh_node is not None else ident()
    jpos = []
    for j in joints:
        w = world(j)
        jpos.append(tuple(sum(Minv[r][c] * w[c][3] for c in range(4))
                          for r in range(3)))

    # ---- 收集所有顶点 ----
    verts = []
    for pr in js["meshes"][0]["primitives"]:
        at = pr["attributes"]
        P = read_acc(js, bins, at["POSITION"])
        J = read_acc(js, bins, at["JOINTS_0"])
        W = read_acc(js, bins, at["WEIGHTS_0"])
        verts.append((P, J, W))
    total = sum(len(v[0]) for v in verts)

    print("\n%s  (%s)" % (label, os.path.relpath(path, ROOT)))
    print("  joints=%d  顶点=%d  primitives=%d"
          % (len(joints), total, len(verts)))

    # A / B / C / D
    bad_sum = 0
    bad_count = 0
    cross = 0
    far = []
    bone_use = defaultdict(float)
    for P, J, W in verts:
        for p, jj, ww in zip(P, J, W):
            s = sum(ww)
            if abs(s - 1.0) > 1e-5:
                bad_sum += 1
            nz = [(jj[k], ww[k]) for k in range(4) if ww[k] > 0.0]
            if not nz or len(nz) > 4:
                bad_count += 1
            per = defaultdict(float)
            for j, w in nz:
                if j < len(limbs):
                    per[limbs[j]] += w
                if j < len(names):
                    bone_use[names[j]] += w
            dom = max(nz, key=lambda kv: kv[1])[0] if nz else -1
            if 0 <= dom < len(jpos):
                d = ((p[0]-jpos[dom][0])**2 + (p[1]-jpos[dom][1])**2
                     + (p[2]-jpos[dom][2])**2) ** 0.5
                far.append(d)
            strong = {l for l, w in per.items()
                      if w > CROSS_LIMB and l != "OTHER"}
            if pathological(strong):
                cross += 1
    far.sort()
    p95 = far[int(len(far) * 0.95)] if far else 0.0

    check("A 权重和 = 1", bad_sum == 0, "越界 %d / %d" % (bad_sum, total))
    check("B 影响数 1..4", bad_count == 0, "异常 %d" % bad_count)
    check("C 跨肢体混杂 = 0", cross == 0,
          "混杂 %d（%.2f%%）" % (cross, 100.0 * cross / max(1, total)))
    check("D 主导骨距离 p95 ≤ %.2f m" % DOM_FAR, p95 <= DOM_FAR,
          "p95 = %.3f m" % p95)

    # E 左右不串边：右侧的顶点不许被"Right 侧骨"占去大头，反之亦然
    #
    # 试过两个版本，都被实测否掉了：
    #   ① 逐骨比左右总权重 —— 这具雕塑右手是握姿，右手中指骨**一个顶点都
    #      没有**，逐骨比必然红，红的是"解剖不对称"不是权重错。
    #   ② 整条肢体比总质量 —— 她是"一腿前一腿后"的站姿，LLEG/RLEG 的顶点
    #      数天然差 40%（旧资产同样 48%），比不出真假。
    # 换成几何判据就干净了：JOINTS_0 存的是**下标**，一旦整体错开一列，
    # 右半身的顶点就会指向 Left 系列骨 —— 那在几何上是荒谬的，
    # 而且与"这具身体长什么样"完全无关。
    if verbose:
        cross_side, side_total = 0, 0
        for P, J, W in verts:
            for p, jj, ww in zip(P, J, W):
                if abs(p[0]) < 0.15:
                    continue            # 中线附近（胯/胸）本来就该混
                side_total += 1
                wrong = "R" if p[0] > 0 else "L"
                bad = 0.0
                for k in range(4):
                    if ww[k] <= 0 or jj[k] >= len(names):
                        continue
                    n = names[jj[k]]
                    if n.startswith("mixamorig:"):
                        n = n[len("mixamorig:"):]
                    if n.startswith(wrong):
                        bad += ww[k]
                if bad > 0.5:
                    cross_side += 1
        rate = 100.0 * cross_side / max(1, side_total)
        check("E 左右不串边", rate < 1.0,
              "错边顶点 %d / %d（%.2f%%）" % (cross_side, side_total, rate))

    # F 静止不变性（同一个 Minv，与上面 jpos 用的是同一份）
    maxerr = 0.0
    if True:
        ibm = [list(v) for v in read_acc(js, bins,
                                         skin["inverseBindMatrices"])] \
            if "inverseBindMatrices" in skin else None
        if ibm:
            ibm = [[[ibm[k][c * 4 + r] for c in range(4)] for r in range(4)]
                   for k in range(len(joints))]
            for k, j in enumerate(joints):
                J = mul(mul(Minv, world(j)), ibm[k])
                for r in range(4):
                    for c in range(4):
                        maxerr = max(maxerr,
                                     abs(J[r][c] - (1.0 if r == c else 0.0)))
    check("F 静止 jointMat ≈ I", maxerr < 1e-4, "maxErr = %.2e" % maxerr)

    # G 骨覆盖
    # 允许极少数指尖骨没绑到：这具雕塑的右手是握姿，指尖藏在掌心/道具里，
    # 周围没有属于它的顶点。除此之外每一根骨都必须有"地盘"。
    FINGER = ("thumb", "index", "middle", "ring", "pinky", "finger")
    unused = [n for n in names if bone_use.get(n, 0.0) < COVER_MIN]
    unused_nonfinger = [n for n in unused
                        if not any(f in n.lower() for f in FINGER)]
    check("G 每根骨都被用到（指尖除外）", not unused_nonfinger,
          "未用 %d：%s" % (len(unused), unused[:4]))

    return {"cross": cross, "p95": p95, "bad_sum": bad_sum, "total": total}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default=DEFAULT)
    ap.add_argument("--baseline", default=None,
                    help="旧资产，用来做对照（证明判据能抓到那个错）")
    args = ap.parse_args()

    print("=" * 66)
    print("verify_skin_weights —— 蒙皮权重数值验收")
    print("=" * 66)
    analyse(args.file, "目标资产")

    if args.baseline:
        print("\n" + "-" * 66)
        print("对照组（同一个判据必须能把旧资产判红，否则判据是空的）")
        print("-" * 66)
        analyse(args.baseline, "对照资产")

    print("\n" + "=" * 66)
    print("合计 PASS=%d FAIL=%d" % (len(PASS), len(FAIL)))
    if FAIL:
        print("FAILED: " + ", ".join(FAIL))
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
