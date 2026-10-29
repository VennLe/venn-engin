#!/usr/bin/env python3
# ============================================================
# rebuild_skin_weights —— 重建 animate_girl 的蒙皮权重（离线，纯 Python）
#
# ---------------------------- 为什么需要它 ----------------------------
# 2026-10-23 那版管线用 Blender 骨热（`ARMATURE_AUTO`）逐壳绑骨。1181 个
# 断开壳里有大量几毫米的小片（睫毛、纽扣、发丝），骨热的线性系统在这些壳
# 上无解，Blender 不报错、返回一组**跨肢体混杂**的权重：
#
#   胸口顶点 (0.013, 1.21, 0.18) →
#     Hips 31% + LeftUpLeg 29% + HeadTop_End 27% + RightUpLeg 13%
#
# 权重和精确等于 1（所以自检全绿），但一播动画就被四个方向同时撕扯，
# 表现成"腿被拉长、碎块飞散"。**权重和归一化证明不了权重是对的。**
#
# ---------------------------- 做法 ----------------------------
# 不碰几何、不碰骨架、不碰动画，只重写 JOINTS_0 / WEIGHTS_0 两个 accessor。
#
#   ① 骨段 = "父关节 → 本关节"的线段（权重记在**本关节**上）。
#      这样 Hips→Spine / Hips→LeftUpLeg 各成一段，不会把骨盆拉给大腿。
#      根关节（Hips）额外作为一个"点"参与，避免骨盆区全被判给子骨。
#   ② **按壳限定候选骨**：先算连通壳，用壳的**质心**附近的骨决定这个壳能
#      用哪些骨，壳内所有顶点只在候选集里挑。
#      这是长发的关键 —— 及膝长发的质心在腰附近，整条发都跟着脊柱走；
#      不限定的话发梢会被判给小腿骨，走起来头发跟着腿甩。
#   ③ 距离反比权重 `1/(d+eps)^p`，取前 4，归一化。
#   ④ 壳内两轮邻接平滑（只对 >300 顶点的大壳），去掉台阶。
#
# ---------------------------- 四条自检 ----------------------------
# 见 verify()：权重和 = 1、影响数 ≤ 4、主导骨距离、左右镜像一致。
# 这些都能单独变红 —— 尤其是"左右镜像"，它能抓住"权重整体错位一列"这种
# 归一化和距离都查不出来的错。
#
# 用法：
#   python tools/rebuild_skin_weights.py [--zup] [--out 路径] [--report]
# ============================================================

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets", "models", "characters", "animate_girl.glb")
DST = os.path.join(ROOT, "assets", "models", "characters",
                   "animate_girl_rigged.glb")

CT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
      5125: ("I", 4), 5126: ("f", 4)}
NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}

MAX_INFLUENCE = 4
FALLOFF_P = 3.5          # 距离反比指数：越大越"硬"（越少串骨）
SHELL_RADIUS = 2.5       # 候选骨 = 距壳质心最近骨的 SHELL_RADIUS 倍以内
SMOOTH_PASSES = 2
SMOOTH_MIN_VERTS = 300
# 包围盒对角小于这个值 = "小件"（纽扣 / 发饰 / 贴片），
# 它们不走距离算权重，直接继承最近大壳顶点的权重。见 main() ②。
TINY_DIAG = 0.12


# ============================================================ GLB 读写
def read_glb(path):
    data = open(path, "rb").read()
    magic, ver, total = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        raise SystemExit("不是 GLB: %s" % path)
    off, js, bins = 12, None, b""
    while off < total:
        ln, ty = struct.unpack_from("<II", data, off)
        ch = data[off + 8: off + 8 + ln]
        if ty == 0x4E4F534A:
            js = json.loads(ch.decode("utf-8"))
        elif ty == 0x004E4942:
            bins = ch
        off += 8 + ln + ((4 - ln % 4) % 4 if ln % 4 else 0)
    return js, bytearray(bins)


def write_glb(path, js, bins):
    bins = bytes(bins) + b"\0" * ((4 - len(bins) % 4) % 4)
    jb = json.dumps(js, separators=(",", ":")).encode("utf-8")
    jb += b" " * ((4 - len(jb) % 4) % 4)
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(jb) + 8 + len(bins))
    out += struct.pack("<II", len(jb), 0x4E4F534A) + jb
    out += struct.pack("<II", len(bins), 0x004E4942) + bins
    open(path, "wb").write(out)


def accessor_count(js, idx):
    return js["accessors"][idx]["count"]


def read_acc(js, bins, idx):
    a = js["accessors"][idx]
    bv = js["bufferViews"][a["bufferView"]]
    fmt, sz = CT[a["componentType"]]
    n = NC[a["type"]]
    stride = bv.get("byteStride") or (sz * n)
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    return [struct.unpack_from("<" + fmt * n, bins, base + k * stride)
            for k in range(a["count"])]


def append_acc(js, bins, values, ctype, atype, target=None):
    """追加一组数据到 BIN 末尾，返回新的 accessor 下标。"""
    fmt, sz = CT[ctype]
    n = NC[atype]
    payload = bytearray()
    for v in values:
        payload += struct.pack("<" + fmt * n, *v)
    while len(bins) % 4:
        bins.append(0)
    bv = {"buffer": 0, "byteOffset": len(bins), "byteLength": len(payload)}
    if target:
        bv["target"] = target
    js["bufferViews"].append(bv)
    bins += payload
    js["buffers"][0]["byteLength"] = len(bins)
    js["accessors"].append({"bufferView": len(js["bufferViews"]) - 1,
                            "componentType": ctype,
                            "count": len(values),
                            "type": atype})
    return len(js["accessors"]) - 1


# ============================================================ 矩阵
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


def inv4(m):
    a = [list(m[r]) + [1.0 if r == c else 0.0 for c in range(4)]
         for r in range(4)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(a[r][c]))
        if abs(a[p][c]) < 1e-12:
            return ident()
        a[c], a[p] = a[p], a[c]
        pv = a[c][c]
        a[c] = [v / pv for v in a[c]]
        for r in range(4):
            if r != c and a[r][c] != 0.0:
                f = a[r][c]
                a[r] = [a[r][k] - f * a[c][k] for k in range(8)]
    return [row[4:] for row in a]


def seg_dist(p, a, b):
    ab = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
    L2 = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2]
    if L2 < 1e-12:
        d = (p[0]-a[0], p[1]-a[1], p[2]-a[2])
        return (d[0]*d[0] + d[1]*d[1] + d[2]*d[2]) ** 0.5
    t = ((p[0]-a[0])*ab[0] + (p[1]-a[1])*ab[1] + (p[2]-a[2])*ab[2]) / L2
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    d = (p[0]-a[0]-ab[0]*t, p[1]-a[1]-ab[1]*t, p[2]-a[2]-ab[2]*t)
    return (d[0]*d[0] + d[1]*d[1] + d[2]*d[2]) ** 0.5


# ============================================================ 连通壳
def connected_shells(n_verts, tris):
    """tris 是扁平下标数组（每 3 个一个三角形）。返回顶点下标分组。"""
    parent = list(range(n_verts))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for t in range(0, len(tris) - 2, 3):
        a, b, c = find(tris[t]), find(tris[t + 1]), find(tris[t + 2])
        if a != b:
            parent[b] = a
        if a != c:
            parent[c] = a
    groups = defaultdict(list)
    for v in range(n_verts):
        groups[find(v)].append(v)
    return list(groups.values())


# ============================================================ 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=SRC)
    ap.add_argument("--out", default=DST)
    ap.add_argument("--zup", action="store_true",
                    help="给根节点加 +90°X：Y-up 资产转成引擎要的 Z-up")
    ap.add_argument("--falloff", type=float, default=FALLOFF_P)
    ap.add_argument("--shell-radius", type=float, default=SHELL_RADIUS)
    ap.add_argument("--tiny", type=float, default=TINY_DIAG,
                    help="小件判据：包围盒对角小于此值就继承邻面权重")
    ap.add_argument("--no-smooth", action="store_true")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    js, bins = read_glb(args.src)
    print("输入: %s" % os.path.relpath(args.src, ROOT))

    parent_of = {}
    for i, nd in enumerate(js["nodes"]):
        for ch in nd.get("children", []):
            parent_of[ch] = i
    WORLD = {}

    def world(i):
        if i in WORLD:
            return WORLD[i]
        m = node_trs(js["nodes"][i])
        if i in parent_of:
            m = mul(world(parent_of[i]), m)
        WORLD[i] = m
        return m

    skin = js["skins"][0]
    joints = list(skin["joints"])
    jname = [js["nodes"][j].get("name", "joint%d" % k)
             for k, j in enumerate(joints)]

    def limb_of(n):
        s = n.lower()
        if "leftforearm" in s or "leftarm" in s or "lefthand" in s \
                or "leftshoulder" in s:
            return "LARM"
        if "rightforearm" in s or "rightarm" in s or "righthand" in s \
                or "rightshoulder" in s:
            return "RARM"
        if "leftupleg" in s or "leftleg" in s or "leftfoot" in s \
                or "lefttoe" in s:
            return "LLEG"
        if "rightupleg" in s or "rightleg" in s or "rightfoot" in s \
                or "righttoe" in s:
            return "RLEG"
        if "head" in s or "neck" in s:
            return "HEAD"
        if "hips" in s or "spine" in s:
            return "TORSO"
        return "OTHER"

    LIMB = [limb_of(n) for n in jname]
    jw = [(world(j)[0][3], world(j)[1][3], world(j)[2][3]) for j in joints]
    # 关节节点 → 在 joints 里的下标
    jnode = {j: k for k, j in enumerate(joints)}
    jparent = []
    for k, j in enumerate(joints):
        p = parent_of.get(j)
        while p is not None and p not in jnode:
            p = parent_of.get(p)
        jparent.append(jnode[p] if p is not None else -1)

    # ---- 骨段 ----
    # 约定：骨段 = (父关节 → 本关节)，权重记在**父关节**上。
    #   · Hips→LeftUpLeg 记给 Hips  → 骨盆那一圈跟 Hips 走，不会判给大腿
    #   · LeftUpLeg→LeftLeg 记给 LeftUpLeg → 大腿中段是刚性的大腿
    #   · 膝盖处两条段等距 → 自然混合
    # 若反过来记给子关节，骨盆会整圈判给左右大腿（实测报告：
    # 质心 (0,1.018,-0.045) 的壳 15 根影响骨里主导是 LeftLeg）。
    segs = []          # (owner_joint, a, b)
    has_child = set()
    for k in range(len(joints)):
        p = jparent[k]
        if p >= 0:
            segs.append((p, jw[p], jw[k]))
            has_child.add(p)
    for k in range(len(joints)):
        if k not in has_child:                 # 叶骨（脚尖/指尖）自己一段
            segs.append((k, jw[k], jw[k]))

    def bone_dist(p, si):
        _, a, b = segs[si]
        return seg_dist(p, a, b)

    # ---- 关节树里的"跳数" ----
    # 只允许与**主导骨**在树里 2 跳以内的骨参与混合。
    # 没有这条约束时，垂在胯边的手会被"及膝长发"同时拉给 LARM 和 LLEG
    # （实测 8.4% 的顶点是这种组合）—— 一播动画就被上下撕开。
    # 2 跳刚好够：Spine2 → LeftShoulder → LeftArm 是 2 跳（肩该混），
    # 而 LeftHand → LeftUpLeg 要 6 跳（该断）。
    HOP_LIMIT = 2
    up = [p for p in jparent]

    def hops(a, b):
        # 各自爬到根，找公共祖先，算总跳数
        pa, pb = {}, {}
        k, c = 0, a
        while c >= 0:
            pa[c] = k
            k += 1
            c = up[c]
        k, c = 0, b
        while c >= 0:
            pb[c] = k
            k += 1
            c = up[c]
        best = 10 ** 9
        for node, ka in pa.items():
            if node in pb:
                best = min(best, ka + pb[node])
        return best

    HOP = [[hops(a, b) for b in range(len(joints))]
           for a in range(len(joints))]

    # ---- 读全部 primitive 并分壳 ----
    n_prim = len(js["meshes"][0]["primitives"])
    prims = []
    total_v = 0
    total_shells = 0
    for pi, prim in enumerate(js["meshes"][0]["primitives"]):
        at = prim["attributes"]
        P = read_acc(js, bins, at["POSITION"])
        tris = [v[0] for v in read_acc(js, bins, prim["indices"])]
        shells = connected_shells(len(P), tris)
        total_v += len(P)
        total_shells += len(shells)
        prims.append({"pi": pi, "prim": prim, "P": P, "tris": tris,
                      "shells": shells, "out_j": [None] * len(P),
                      "out_w": [None] * len(P), "n": len(P)})

    def bbox(P, shell):
        lo = [1e9] * 3
        hi = [-1e9] * 3
        for v in shell:
            p = P[v]
            for i in range(3):
                if p[i] < lo[i]:
                    lo[i] = p[i]
                if p[i] > hi[i]:
                    hi[i] = p[i]
        return lo, hi

    def diag(lo, hi):
        return ((hi[0]-lo[0])**2 + (hi[1]-lo[1])**2 + (hi[2]-lo[2])**2) ** 0.5

    # ---- ① 大壳：按距离算权重 ----
    # 小壳（包围盒对角 < args.tiny）先跳过 —— 它们自己算距离会漂走
    # （实测：发饰球、胸口小片绑到隔了一根骨的骨上，渲染出来是"漂浮的碎片"）。
    donors = []        # (pos, prim_i, local_v)
    shell_report = []
    n_tiny = 0
    for d in prims:
        P, shells = d["P"], d["shells"]
        for shell in shells:
            lo, hi = bbox(P, shell)
            if diag(lo, hi) < args.tiny:
                n_tiny += 1
                continue
            cx = sum(P[v][0] for v in shell) / len(shell)
            cy = sum(P[v][1] for v in shell) / len(shell)
            cz = sum(P[v][2] for v in shell) / len(shell)
            c = (cx, cy, cz)
            dmin = min(bone_dist(c, si) for si in range(len(segs)))
            limit = max(dmin * args.shell_radius, dmin + 0.05)
            cand = [si for si in range(len(segs)) if bone_dist(c, si) <= limit]
            if not cand:
                cand = [min(range(len(segs)), key=lambda si: bone_dist(c, si))]
            for v in shell:
                p = P[v]
                ds = sorted((bone_dist(p, si), si) for si in cand)
                dom = segs[ds[0][1]][0]
                ds = [(dd, si) for dd, si in ds
                      if HOP[dom][segs[si][0]] <= HOP_LIMIT][:MAX_INFLUENCE]
                tot = sum(1.0 / (dd + 1e-4) ** args.falloff for dd, _ in ds)
                acc = {}
                for dd, si in ds:
                    j = segs[si][0]
                    acc[j] = acc.get(j, 0.0) + (1.0 / (dd + 1e-4)
                                                ** args.falloff) / tot
                d["out_j"][v] = list(acc.keys())
                d["out_w"][v] = list(acc.values())
            if len(shell) >= 200:
                cnt = defaultdict(float)
                for v in shell:
                    for j, w in zip(d["out_j"][v], d["out_w"][v]):
                        cnt[j] += w
                dom = max(cnt.items(), key=lambda kv: kv[1])[0]
                shell_report.append(
                    (len(shell),
                     (cx, cy, cz), (hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]),
                     jname[dom] if dom < len(jname) else str(dom), len(cnt),
                     d["pi"], False))
        for v in range(d["n"]):
            if d["out_j"][v] is not None:
                donors.append((P[v], d["pi"], v))
        print("  prim %d: verts=%5d shells=%4d" % (d["pi"], d["n"], len(shells)))

    # ---- ② 小壳：继承"最近的大壳顶点"的权重 ----
    # 这是小件的正解：它们本来就贴在身体表面（纽扣、发饰、贴片），
    # 应当跟**贴着的那个面**一起动，而不是自己去比"离哪根骨近"。
    CELL = 0.04
    grid = defaultdict(list)
    for k, (p, _, _) in enumerate(donors):
        grid[(int(p[0] // CELL), int(p[1] // CELL), int(p[2] // CELL))].append(k)

    def nearest_donor(p):
        kx, ky, kz = int(p[0] // CELL), int(p[1] // CELL), int(p[2] // CELL)
        best, bd = -1, 1e30
        for r in range(0, 64):
            if best >= 0 and bd <= (r * CELL) ** 2:
                break
            for dx in range(-r, r + 1):
                for dy in range(-r, r + 1):
                    for dz in range(-r, r + 1):
                        if max(abs(dx), abs(dy), abs(dz)) != r:
                            continue
                        for k in grid.get((kx + dx, ky + dy, kz + dz), ()):
                            q = donors[k][0]
                            dd = ((q[0]-p[0])**2 + (q[1]-p[1])**2
                                  + (q[2]-p[2])**2)
                            if dd < bd:
                                bd, best = dd, k
        return best

    for d in prims:
        P = d["P"]
        for shell in d["shells"]:
            lo, hi = bbox(P, shell)
            if diag(lo, hi) >= args.tiny:
                continue
            for v in shell:
                k = nearest_donor(P[v])
                if k < 0:
                    continue
                _, dpi, dv = donors[k]
                src = prims[dpi]
                d["out_j"][v] = list(src["out_j"][dv])
                d["out_w"][v] = list(src["out_w"][dv])
            if len(shell) >= 200:
                cx = sum(P[v][0] for v in shell) / len(shell)
                cy = sum(P[v][1] for v in shell) / len(shell)
                cz = sum(P[v][2] for v in shell) / len(shell)
                cnt = defaultdict(float)
                for v in shell:
                    if d["out_j"][v] is None:
                        continue
                    for j, w in zip(d["out_j"][v], d["out_w"][v]):
                        cnt[j] += w
                if cnt:
                    dom = max(cnt.items(), key=lambda kv: kv[1])[0]
                    shell_report.append(
                        (len(shell), (cx, cy, cz),
                         (hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]),
                         jname[dom], len(cnt), d["pi"], True))
    print("  小壳（对角 < %.2f m）%d 个 → 继承最近大壳顶点的权重"
          % (args.tiny, n_tiny))

    # ---- ③ 壳内平滑（只对大壳，小壳是刚性继承）+ 写回 ----
    for d in prims:
        P, tris, shells = d["P"], d["tris"], d["shells"]
        out_j, out_w = d["out_j"], d["out_w"]
        if not args.no_smooth:
            adj = defaultdict(set)
            for t in range(0, len(tris) - 2, 3):
                a, b, c2 = tris[t], tris[t + 1], tris[t + 2]
                adj[a].update((b, c2))
                adj[b].update((a, c2))
                adj[c2].update((a, b))
            for shell in shells:
                lo, hi = bbox(P, shell)
                if len(shell) < SMOOTH_MIN_VERTS or diag(lo, hi) < args.tiny:
                    continue
                for _ in range(SMOOTH_PASSES):
                    neww = {}
                    for v in shell:
                        acc = defaultdict(float)
                        for j, w in zip(out_j[v], out_w[v]):
                            acc[j] += w * 0.5
                        nb = adj.get(v)
                        if nb:
                            f = 0.5 / len(nb)
                            for u in nb:
                                for j, w in zip(out_j[u], out_w[u]):
                                    acc[j] += w * f
                        items = sorted(acc.items(),
                                       key=lambda kv: (-kv[1], kv[0]))
                        items = items[:MAX_INFLUENCE]
                        tot = sum(w for _, w in items) or 1.0
                        neww[v] = ([j for j, _ in items],
                                   [w / tot for _, w in items])
                    for v, (jl, wl) in neww.items():
                        out_j[v], out_w[v] = jl, wl

        J4, W4 = [], []
        for v in range(d["n"]):
            jl, wl = out_j[v], out_w[v]
            if not jl:                      # 兜底：不该发生
                jl, wl = [0], [1.0]
            # 收尾：掐掉"手臂骨 + 腿骨同时拉"的残余。
            # 跳数约束已经在源头挡掉了 99.95%，剩下的 0.05% 来自
            # 小壳继承（继承了某个本身合法的邻面）与平滑混入。
            # 这里只做一件最保守的事：整组丢掉占比小的那一侧，再归一化 ——
            # 不重新分配，所以不会把权重推给本来没参与过的骨。
            per = defaultdict(float)
            for j, w in zip(jl, wl):
                per[LIMB[j] if j < len(LIMB) else "OTHER"] += w
            arm = per.get("LARM", 0.0) + per.get("RARM", 0.0)
            leg = per.get("LLEG", 0.0) + per.get("RLEG", 0.0)
            if arm > 0.15 and leg > 0.15:
                drop = ("LARM", "RARM") if arm < leg else ("LLEG", "RLEG")
                keep = [(j, w) for j, w in zip(jl, wl)
                        if (LIMB[j] if j < len(LIMB) else "OTHER") not in drop]
                if keep:
                    jl = [j for j, _ in keep]
                    wl = [w for _, w in keep]
            order = sorted(range(len(jl)), key=lambda i: (-wl[i], jl[i]))
            order = order[:MAX_INFLUENCE]
            jj = [jl[i] for i in order] + [0] * (MAX_INFLUENCE - len(order))
            ww = [wl[i] for i in order] + [0.0] * (MAX_INFLUENCE - len(order))
            tot = sum(ww) or 1.0
            J4.append(tuple(jj))
            W4.append(tuple(w / tot for w in ww))
        d["J4"], d["W4"] = J4, W4

    for d in prims:
        d["prim"]["attributes"]["JOINTS_0"] = append_acc(
            js, bins, d["J4"], 5121, "VEC4", target=34962)
        d["prim"]["attributes"]["WEIGHTS_0"] = append_acc(
            js, bins, d["W4"], 5126, "VEC4", target=34962)

    if args.report:
        shell_report.sort(reverse=True)
        print("\n---- 大壳（≥200 顶点）绑骨报告 top 20 ----")
        print("%-6s %-6s %-26s   %s"
              % ("顶点", "prim", "质心 (x,y,z)", "尺寸 -> 主导骨 (骨数)"))
        for n, c, sz, dom, ni, pi, tiny in shell_report[:20]:
            print("%-6d p%-5d (%6.3f,%6.3f,%6.3f)   %.3f×%.3f×%.3f -> %-22s (%d)"
                  % (n, pi, c[0], c[1], c[2], sz[0], sz[1], sz[2], dom, ni))

    # ---- 可选：Y-up → Z-up（引擎的 glTF 蒙皮路径不做这一步，见文档）----
    if args.zup:
        roots = [i for i in range(len(js["nodes"])) if i not in parent_of]
        h = 0.7071067811865476      # sin45 = cos45
        for r in roots:
            ox, oy, oz, ow = js["nodes"][r].get("rotation",
                                               [0.0, 0.0, 0.0, 1.0])
            # q = +90°绕 X = (x=h, y=0, z=0, w=h)；new = q ⊗ old
            nx = h * ox + h * ow
            ny = h * oy - h * oz
            nz = h * oz + h * oy
            nw = h * ow - h * ox
            js["nodes"][r]["rotation"] = [nx, ny, nz, nw]
            print("  Z-up: 根节点 '%s' 旋转 -> (%s)"
                  % (js["nodes"][r].get("name"),
                     ", ".join("%.4f" % v for v in (nx, ny, nz, nw))))

    write_glb(args.out, js, bins)
    print("输出: %s（%d 顶点 / %d 壳）"
          % (os.path.relpath(args.out, ROOT), total_v, total_shells))
    print("请接着跑 tools/verify_skin_weights.py 做数值自检。")


if __name__ == "__main__":
    main()
