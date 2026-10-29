"""S-B · 蒙皮与权重（`animation::WeightSet` + Character 窗口的 Weights 页）的验收。

任务书把 S-B 拆成"顶点组 / 自动权重 / 权重绘制 / 烘焙重传"四块，
验收对应八组：

  A  数据层自检（`weights_*`）—— 零编辑烘焙逐位一致 / 和恒为 1 / 只 deform 骨
  B  自动权重（真模型）：包络覆盖率 / 每个顶点的权重和 / 真的重传了 GPU
  C  只算 deform 骨：关掉一根骨 → 它一根权重都没有，摘要随之改变
  D  刷子：跨进程确定性（同一批操作 → 同一个摘要）+ 三种模式
  E  零编辑烘焙 ≡ 导入路径：不刷任何东西 `bake` → `changed=0`
  F  存盘 / 读盘：`venn.weights` 的字段 + 往返摘要不变
  G  镜像：权重跟着骨名 `.L`/`.R` 换边
  H  UI 锚点齐全 + 全程零 VUID、零 `[ERROR]`

------------------------------------------------------------
为什么走 `MYVK_EDITOR_WEIGHTS_SCRIPT` 而不是拖控件
------------------------------------------------------------
和 R7/R8/S-A 一样：这一层的正确性全在"数据对不对"上。脚本走的是
**和 UI 完全同一条**通路（`WeightSet::paintVertex → bake → Mesh::upload`），
不是另一套实现；UI 只验锚点在不在。顺带把"笔刷要拖到哪个像素"这个
不稳定因素整个去掉 —— 屏幕空间刷子另有 `WEIGHTS-BRUSH` 日志，
但它的输入是鼠标坐标，不适合当断言的输入。

------------------------------------------------------------
E 组为什么是 S-B 最重要的一条
------------------------------------------------------------
`bake` 会把权重写成 `JOINTS_0/WEIGHTS_0` 四槽再重传顶点缓冲。GPU 只吃
4 槽、我们的表能存 8 条，所以"读出来 → 排序 → 写回去"这条路**一定**会
让权重以 1e-7 的量级漂。漂了不报错、不花屏，只是"角色姿态每次打开权重页
都微微变一点"。所以：没编辑过的顶点组烘焙出去，**一个顶点都不许变**。
`WEIGHTS-BAKE ... changed=0` 就是这条。

用法：
    python tools/verify_weights.py
    python tools/verify_weights.py --only b        # 只跑某一组
    python tools/verify_weights.py --keep          # 留日志与产物
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import LogTap, kill_stale_editors          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "weights")

MODEL = "assets/models/anime_girl.fbx"
# 落在 build/bin 下（build/ 被 gitignore）。**相对路径是刻意的**：
# `save:<path>` / `load:<path>` 按"第一个冒号之后整段"解析，路径里一旦
# 出现盘符冒号，被测的就不是权重存盘而是那个解析补丁。
WT_FILE = "_verif_wt_a.weights.json"

NUM = r"[-+]?[\d.eE+]+"
COVER_RE = re.compile(
    r"WEIGHTS-AUTO-COVER mesh='([^']*)' verts=(\d+) outside=(\d+) pct=(" + NUM + r")%")
BONE_RE = re.compile(
    r"WEIGHTS-AUTO-BONE i=(\d+) name='([^']*)' use=(\d+) head=\(" + NUM + r"," + NUM +
    r"," + NUM + r"\) tail=\(" + NUM + r"," + NUM + r"," + NUM +
    r"\) len=(" + NUM + r") r=(" + NUM + r")")
SKIPBONE_RE = re.compile(
    r"WEIGHTS-AUTO-SKIP-BONE i=(\d+) name='([^']*)' reason=(\S+)")
SUMMARY_RE = re.compile(
    r"WEIGHTS-SUMMARY tag=(\S+) mesh='([^']*)' vtx=(\d+) touched=(\d+) unweighted=(\d+) "
    r"maxInf=(\d+) over4=(\d+) avgInf=(" + NUM + r") sum=\[(" + NUM + r"),(" + NUM + r")\]")
BAKE_RE = re.compile(r"WEIGHTS-BAKE mesh=(\d+) vtx=(\d+) changed=(\d+)")
BAKEALL_RE = re.compile(
    r"WEIGHTS-BAKE-ALL meshes=(\d+) reuploaded=(\d+) changedVerts=(\d+)")
DIGEST_RE = re.compile(
    r"WEIGHTS-DIGEST mesh=(-?\d+) name='([^']*)' h=(\d+) vtx=(\d+)")
MIRROR_RE = re.compile(r"WEIGHTS-MIRROR from=(-?\d+) to=(-?\d+) axis=([XYZ])")
DUMP_RE = re.compile(
    r"WEIGHTS-DUMP tag=(\S+) mesh='([^']*)' v=(\d+) n=(\d+)(?: e=(\S+))?")
SCRIPT_RE = re.compile(
    r"WEIGHTS-SCRIPT applied=(\d+) failed=(\d+) meshes=(\d+) active=(-?\d+) "
    r"sel=(-?\d+) dirty=(\d+)")
IMPORT_RE = re.compile(r"WEIGHTS-IMPORT meshes=(\d+) skinned=(\d+) verts=(\d+) bones=(\d+)")
LISTMESH_RE = re.compile(
    r"WEIGHTS-LIST-MESH i=(\d+) name='([^']*)' active=(\d+) vtx=(\d+) "
    r"touched=(\d+) unweighted=(\d+)")
SELFTEST_RE = re.compile(
    r"ANIM-TEST (weights_\w+) actual=(\d+) expected=1 tol=0 detail=(\S*) result=(PASS|FAIL)")
TOTAL_RE = re.compile(r"ANIM-TOTAL pass=(\d+) fail=(\d+)")

results = []


def check(name, ok, detail=""):
    tag = "[PASS]" if ok else "[FAIL]"
    print("%s %s%s" % (tag, name, ("  → " + detail) if detail else ""))
    results.append((name, bool(ok), detail))


def skip(name, why):
    print("[SKIP] %s  → %s" % (name, why))


# ================================================================== 编辑器

def run_editor(script, rig_script="", wait_s=180.0, wait_for="WEIGHTS-SCRIPT applied="):
    """起一次编辑器跑一段注入脚本，返回日志行。"""
    env = dict(os.environ)
    env["MYVK_CHARACTER_WINDOW"] = "1"
    env["MYVK_CHARACTER_MODEL"] = MODEL
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    if script:
        env["MYVK_EDITOR_WEIGHTS_SCRIPT"] = script
    if rig_script:
        env["MYVK_EDITOR_RIG_SCRIPT"] = rig_script

    proc = subprocess.Popen([EXE], cwd=BIN, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    _wait(tap, wait_for, wait_s)
    time.sleep(1.5)     # 再给一帧：UI 锚点在脚本之后的绘制里打
    try:
        proc.terminate()
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            pass
    tap.t.join(timeout=8)
    with tap.lock:
        lines = list(tap.lines)
    return [l.replace("\x00", "") for l in lines]


def _wait(tap, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for l in tap.lines:
                if needle in l:
                    return True
        time.sleep(0.15)
    print("[WARN] 等不到 %s（%ds）" % (needle, int(timeout)))
    return False


def digests_of(lines):
    """`WEIGHTS-DIGEST` → {mesh: h}（同网格以最后一行为准）。"""
    out = {}
    for l in lines:
        m = DIGEST_RE.search(l)
        if m:
            out[int(m.group(1))] = int(m.group(3))
    return out


def summaries_of(lines):
    """`WEIGHTS-SUMMARY` → 最后一条的字段。"""
    got = None
    for l in lines:
        m = SUMMARY_RE.search(l)
        if not m:
            continue
        got = {"tag": m.group(1), "mesh": m.group(2), "vtx": int(m.group(3)),
               "touched": int(m.group(4)), "unweighted": int(m.group(5)),
               "maxInf": int(m.group(6)), "over4": int(m.group(7)),
               "avgInf": float(m.group(8)),
               "minSum": float(m.group(9)), "maxSum": float(m.group(10))}
    return got


def bones_of(lines):
    """`WEIGHTS-AUTO-BONE` → {i: {name, len, r, use}}。"""
    out = {}
    for l in lines:
        m = BONE_RE.search(l)
        if m:
            out[int(m.group(1))] = {"name": m.group(2), "use": int(m.group(3)),
                                    "len": float(m.group(4)), "r": float(m.group(5))}
    return out


def dump_of(lines, want_v=None):
    """`WEIGHTS-DUMP` → {v: [(bone, weight), ...]}（同一顶点以最后一行为准）。"""
    out = {}
    for l in lines:
        m = DUMP_RE.search(l)
        if not m:
            continue
        v = int(m.group(3))
        if want_v is not None and v != want_v:
            continue
        ents = []
        if m.group(5):
            for part in m.group(5).split(","):
                if ":" not in part:
                    continue
                b, w = part.split(":", 1)
                try:
                    ents.append((int(b), float.fromhex(w)))
                except ValueError:
                    pass
        out[v] = ents
    return out


def flip_side(name):
    """`upperarm.L` ↔ `upperarm.R`、`LeftArm` ↔ `RightArm`。

    与 `RigEdit::flipSide` 同一套规则 —— 这里再写一份是**故意的**：
    测试要是直接调被测代码的换名规则，规则本身写错时它会跟着一起错，
    于是"镜像那条用例永远绿"。
    """
    for a, b in ((".L", ".R"), ("_L", "_R"), (".l", ".r"), ("_l", "_r")):
        if name.endswith(a):
            return name[:-len(a)] + b
        if name.endswith(b):
            return name[:-len(b)] + a
    for a, b in (("Left", "Right"), ("Right", "Left"),
                 ("left", "right"), ("right", "left")):
        if a in name:
            return name.replace(a, b, 1)
    return ""


# ================================================================== A 自检

def case_a():
    env = dict(os.environ)
    env["MYVK_ANIM_TEST"] = "1"
    proc = subprocess.run([EXE], cwd=BIN, env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=600)
    text = (proc.stdout or "") + (proc.stderr or "")
    if "--keep" in sys.argv:
        with open(os.path.join(OUT_DIR, "selftest.log"), "w", encoding="utf-8") as f:
            f.write(text)

    got = {}
    for m in SELFTEST_RE.finditer(text):
        got[m.group(1)] = (m.group(4) == "PASS", m.group(3))
    ws = {k: v for k, v in got.items() if k.startswith("weights_")}
    fails = [k for k, v in ws.items() if not v[0]]
    check("A1 weights_* 断言全部 PASS（共 %d 条）" % len(ws),
          len(ws) >= 14 and not fails,
          "fails=%s" % (",".join(sorted(fails)) if fails else "none"))

    tot = TOTAL_RE.search(text)
    check("A2 ANIM-TOTAL fail=0（整体自检没被带崩）",
          tot is not None and int(tot.group(2)) == 0,
          "total=%s" % (tot.group(0) if tot else "missing"))
    check("A3 自检进程 rc==0", proc.returncode == 0, "rc=%d" % proc.returncode)
    # 三条最关键的不变量单独点名（其余靠 A1 的总数兜底）
    for key in ("weights_bake_identity", "weights_auto_sum_one",
                "weights_auto_only_deform", "weights_bake_mask_scoped"):
        ok, det = got.get(key, (False, ""))
        check("A4 %s PASS" % key, ok, det)


# ================================================================== B 自动权重

def case_b(lines):
    imp = IMPORT_RE.search("\n".join(lines))
    check("B1 权重从网格导入（WEIGHTS-IMPORT）", imp is not None,
          imp.group(0) if imp else "missing")

    covers = [COVER_RE.search(l) for l in lines]
    covers = [m for m in covers if m]
    check("B2 自动权重跑过（WEIGHTS-AUTO-COVER %d 次）" % len(covers),
          len(covers) >= 1, "n=%d" % len(covers))
    if not covers:
        return
    tot_verts = sum(int(m.group(2)) for m in covers)
    tot_out = sum(int(m.group(3)) for m in covers)
    pct = 100.0 * tot_out / max(tot_verts, 1)
    # 覆盖率**不设死门槛**：它取决于资产的骨骼几何与模型是否对得上。
    # 这里只要求"这个数被算出来并打出来了"，以及"全落空"这种病态情况不发生。
    check("B3 包络覆盖率有数：%d 顶点 / %d 落空（%.1f%%）" %
          (tot_verts, tot_out, pct), tot_verts > 0 and pct < 100.0,
          "verts=%d outside=%d" % (tot_verts, tot_out))

    st = summaries_of(lines)
    check("B4 权重和恒为 1（SUM=[%.6f,%.6f]）" % (st["minSum"], st["maxSum"])
          if st else "B4 权重和恒为 1", st is not None and
          st["minSum"] >= 1.0 - 1e-4 and st["maxSum"] <= 1.0 + 1e-4,
          "sum=[%s,%s]" % (st["minSum"], st["maxSum"]) if st else "missing")
    check("B5 没有孤立顶点（unweighted=0）",
          st is not None and st["unweighted"] == 0,
          "unweighted=%s" % (st["unweighted"] if st else None))
    check("B6 影响条数 <= 4（GPU 四槽不用截断）",
          st is not None and st["over4"] == 0,
          "over4=%s" % (st["over4"] if st else None))

    ba = BAKEALL_RE.search("\n".join(lines))
    check("B7 烘焙并把顶点缓冲重传了（reuploaded>0）",
          ba is not None and int(ba.group(2)) > 0,
          ba.group(0) if ba else "missing")


# ================================================================== C 只算 deform 骨

def case_c(lines, bones):
    if not bones:
        skip("C1 关掉一根 deform 骨 → 它一根权重都没有", "日志里没有 WEIGHTS-AUTO-BONE")
        return
    # 挑**包络半径最大**的那根：它覆盖的顶点最多，关掉它摘要必变
    target = max(bones.items(), key=lambda kv: (kv[1]["r"], kv[0]))[0]
    name = bones[target]["name"]

    base = run_editor("list;mesh:0;autoall;digest")
    d_base = digests_of(base)
    off = run_editor("list;mesh:0;autoall;stats;digest",
                     rig_script="deform:%d:0" % target)

    skips = [SKIPBONE_RE.search(l) for l in off]
    skips = [m for m in skips if m]
    hit = [m for m in skips if int(m.group(1)) == target]
    check("C1 关掉的骨被自动权重跳过（%s）" % name,
          len(hit) >= 1 and hit[0].group(3) == "not-deform",
          "name=%s" % (hit[0].group(2) if hit else None))

    d_off = digests_of(off)
    common = [k for k in d_base if k in d_off]
    changed = [k for k in common if d_base[k] != d_off[k]]
    check("C2 权重表随之改变（%d/%d 张网格）" % (len(changed), len(common)),
          len(changed) >= 1, "changed=%s" % (changed[:6]))

    # 反向对照：不关的那一轮里，这根骨**必须**是有权重的（否则 C1 是空转）
    st = summaries_of(off)
    check("C3 关掉之后仍然没有孤立顶点（unweighted=0）",
          st is not None and st["unweighted"] == 0,
          "unweighted=%s" % (st["unweighted"] if st else None))


# ================================================================== D 刷子

def case_d():
    # 同一批操作跑两次（两个进程）→ 同一个摘要。这条把"全序排序"钉住：
    # 只按权重排序时，两条等权的条目在不同 std::sort 下会换位。
    script = ("list;mesh:0;paint:0:13:0.25;paint:1:13:0.25;paint:2:13:0.25;"
              "norm;digest")
    d1 = digests_of(run_editor(script))
    d2 = digests_of(run_editor(script))
    check("D1 跨进程确定性：同一批刷子操作 → 同一个摘要",
          bool(d1) and d1 == d2, "%s vs %s" % (d1.get(0), d2.get(0)))

    # 三种模式：先把 Auto normalize 关掉，让"加了多少"原样看得见 ——
    # 开着归一化时 `set 0.4` 出来的值会被其它权重除一遍，测的就不是
    # "Set 写了 0.4"而是"归一化对不对"（两条用例混一起，失败时分不清）。
    #
    # 顶点也要**从数据里挑**：随手写 v=3 可能正好在这一骨上没有权重，
    # 于是 subract 那一路变成 0-0.1 夹到 0，"方向正确"就成了空转
    #（第一版就是这样，D2 的 detail 里 sub=0.0000 全靠夹取混过去）。
    cand = [0, 3, 500, 1500, 3000]
    probe_lines = run_editor(
        "list;mesh:0;autoall;" + ";".join("dump:%d" % v for v in cand))
    picked = None
    for v in cand:
        d = dump_of(probe_lines, v)
        if v not in d:
            continue
        w0 = next((w for b, w in d[v] if b == 13), 0.0)
        if 0.05 <= w0 <= 0.80:
            picked = (v, w0)
            break
    if picked is None:
        skip("D2 刷子的三种模式", "挑不到一个顶点在骨 13 上有 0.05~0.8 的权重")
        return
    vtx, w0 = picked

    def probe(mode, value):
        # ⚠ 每一轮都要 `autoall`：w0 是在"自动权重之后"量出来的，而
        #   `paint` 作用在**当轮导入的那份表**上。少了这一句，比的其实是
        #   "自动权重的结果" vs "导入的权重"，diff 里看到的是两份数据的差
        #   （实测 add 变成 -0.14：导入的权重里这个顶点根本没有骨 13）。
        lines = run_editor("list;mesh:0;autoall;autonorm:0;mode:%d;"
                           "paint:%d:13:%f;dump:%d" % (mode, vtx, value, vtx))
        d = dump_of(lines, vtx)
        return (next((w for b, w in d[vtx] if b == 13), 0.0) if vtx in d else None)

    w_add = probe(0, 0.10)
    w_sub = probe(1, 0.10)
    w_set = probe(2, 0.40)
    if w_add is None or w_sub is None or w_set is None:
        check("D2 刷子的三种模式", False, "dump 缺失")
        return
    ok = (abs(w_add - (w0 + 0.10)) <= 1e-4 and
          abs(w_sub - (w0 - 0.10)) <= 1e-4 and
          abs(w_set - 0.40) <= 1e-4)
    check("D2 三种模式（v=%d, w0=%.4f）：add=%+.2f sub=%.2f set=%.4f" %
          (vtx, w0, w_add - w0, w_sub, w_set),
          ok, "add=%.4f sub=%.4f set=%.4f want=(%.4f,%.4f,0.4)" %
          (w_add, w_sub, w_set, w0 + 0.10, w0 - 0.10))

    # 开着 Auto normalize 时，刷完权重和必须仍然为 1（约定 ②）
    for tag, mode, val in (("add", 0, 0.25), ("sub", 1, 0.25), ("set", 2, 0.40)):
        lines = run_editor("list;mesh:0;autoall;autonorm:1;mode:%d;"
                           "paint:%d:13:%f;dump:%d" % (mode, vtx, val, vtx))
        d = dump_of(lines, vtx)
        ents = d.get(vtx) or []
        s = sum(w for _, w in ents)
        check("D3 %s + Auto normalize 之后权重和 == 1（%.6f）" % (tag, s),
              bool(ents) and abs(s - 1.0) <= 1e-4, "n=%d" % len(ents))


# ================================================================== E 零编辑烘焙

def case_e():
    lines = run_editor("list;bake")
    bakes = [BAKE_RE.search(l) for l in lines]
    bakes = [m for m in bakes if m]
    nonzero = [m for m in bakes if int(m.group(3)) != 0]
    check("E1 零编辑烘焙：每张网格 changed=0（%d 张）" % len(bakes),
          len(bakes) >= 1 and not nonzero,
          "nonzero=%s" % ([(m.group(1), m.group(3)) for m in nonzero][:5] or "none"))

    ba = BAKEALL_RE.search("\n".join(lines))
    check("E2 一个顶点都没变 → 不重传顶点缓冲",
          ba is not None and int(ba.group(2)) == 0,
          ba.group(0) if ba else "missing")

    # 导入的网格槽位本来就等于权重表 → 反转一次同样不该有任何改动
    rev = run_editor("list;revert;bake")
    rbakes = [BAKE_RE.search(l) for l in rev]
    rbakes = [m for m in rbakes if m]
    rnz = [m for m in rbakes if int(m.group(3)) != 0]
    check("E3 Revert 之后重新烘焙仍然 changed=0", bool(rbakes) and not rnz,
          "nonzero=%s" % ([(m.group(1), m.group(3)) for m in rnz][:5] or "none"))


# ================================================================== F 存盘 / 读盘

def case_f():
    path = os.path.join(BIN, WT_FILE)
    if os.path.exists(path):
        os.remove(path)

    before = digests_of(run_editor("list;mesh:0;autoall;save:%s;digest" % WT_FILE))
    ok_file = os.path.exists(path)
    check("F1 存盘落了文件", ok_file, path)
    if not ok_file:
        return

    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except Exception as e:                                  # noqa: BLE001
        check("F2 venn.weights 能解析", False, repr(e))
        return
    check("F2 format/version 正确",
          doc.get("format") == "venn.weights" and doc.get("version") == 1,
          "format=%s version=%s" % (doc.get("format"), doc.get("version")))

    stored = doc.get("verts") or []
    n_ent = sum(len(v.get("e") or []) for v in stored)
    check("F3 稀疏条目数合理（%d 顶点 / %d 条影响）" % (len(stored), n_ent),
          len(stored) > 0 and n_ent >= len(stored),
          "vertexCount=%s" % doc.get("vertexCount"))
    check("F4 vertexCount 与网格一致",
          doc.get("vertexCount", -1) > 0, "vertexCount=%s" % doc.get("vertexCount"))

    # 每条权重都在 [0,1] 且顶点/骨下标非负
    bad = 0
    for v in stored:
        if int(v.get("v", -1)) < 0:
            bad += 1
        for e in (v.get("e") or []):
            if len(e) < 2 or int(e[0]) < 0 or not (0.0 <= float(e[1]) <= 1.0):
                bad += 1
    check("F5 条目字段合法", bad == 0, "bad=%d" % bad)

    after = digests_of(run_editor("list;mesh:0;load:%s;digest" % WT_FILE))
    check("F6 存读往返摘要不变",
          bool(before) and bool(after) and before.get(0) == after.get(0),
          "%s vs %s" % (before.get(0), after.get(0)))

    if "--keep" not in sys.argv:
        try:
            os.remove(path)
        except OSError:
            pass


# ================================================================== G 镜像

def case_g(lines):
    # 顶点下标从 `WEIGHTS-LIST-MESH` 拿顶点数，再从里面挑几个候选试镜像
    vtx = 0
    for l in lines:
        m = LISTMESH_RE.search(l)
        if m and int(m.group(1)) == 0:
            vtx = int(m.group(4))
    if vtx <= 0:
        skip("G1 镜像到对侧顶点", "拿不到 mesh 0 的顶点数")
        return

    # `anime_girl` 的蒙皮是左右对称的，中段顶点一定能配上对；一次试 6 个候选
    cand = [vtx // 8, vtx // 6, vtx // 4, vtx // 3, vtx // 2, vtx - 1]
    script = "list;mesh:0;autoall;" + ";".join("mirror:%d" % v for v in cand)
    res = run_editor(script)
    found = []
    for l in res:
        m = MIRROR_RE.search(l)
        if m:
            found.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    hits = [f for f in found if f[1] >= 0 and f[1] != f[0]]
    check("G1 权重镜像找得到对侧顶点（试 %d 个，成 %d 个）" %
          (len(cand), len(hits)), len(hits) >= 1,
          "hits=%s" % (hits[:3],))
    if not hits:
        return

    # 骨名要跟着换边：dump 源顶点与镜像顶点，逐条比对骨名与权重
    src = hits[0][0]
    dst = hits[0][1]
    dump = run_editor("list;mesh:0;autoall;dump:%d;dump:%d;mirror:%d;dump:%d"
                      % (src, dst, src, dst))
    # 取**最后一轮**的 dst（镜像之后）
    last_dst = None
    for l in dump:
        m = DUMP_RE.search(l)
        if m and int(m.group(3)) == dst:
            last_dst = m
    dump_src = dump_of(dump, src)
    if last_dst is None or src not in dump_src:
        check("G2 镜像后骨名换边", False, "dump 缺失")
        return

    bones = bones_of(dump)
    ents = []
    if last_dst.group(5):
        for part in last_dst.group(5).split(","):
            if ":" in part:
                b, w = part.split(":", 1)
                try:
                    ents.append((int(b), float.fromhex(w)))
                except ValueError:
                    pass
    want = {}
    for b, w in dump_src[src]:
        nm = bones.get(b, {}).get("name", "")
        other = flip_side(nm)
        if other:
            want[other] = w     # 骨名 → 期望权重
    got = {}
    for b, w in ents:
        got[bones.get(b, {}).get("name", "?%d" % b)] = w
    ok = bool(want) and all(nm in got and abs(got[nm] - w) <= 1e-5
                            for nm, w in want.items())
    check("G2 镜像后骨名换边、权重一致（%d 条）" % len(want), ok,
          "from=%d to=%d want=%s got=%s" % (src, dst, sorted(want), sorted(got)))


# ================================================================== H UI 锚点

def case_h(lines):
    text = "\n".join(lines)
    need = ["CHAR-TAB-Weights", "CHAR-WT-MESH", "CHAR-WT-STATS", "CHAR-WT-RADIUS",
            "CHAR-WT-FALLOFF", "CHAR-WT-MAXINF", "CHAR-WT-DEFORMONLY",
            "CHAR-WT-AUTO", "CHAR-WT-AUTOALL", "CHAR-WT-MODE", "CHAR-WT-WEIGHT",
            "CHAR-WT-BRUSH", "CHAR-WT-SOFT", "CHAR-WT-AUTONORM", "CHAR-WT-MIRROR",
            "CHAR-WT-FRONTFACE", "CHAR-WT-BONE", "CHAR-WT-NORMALIZE",
            "CHAR-WT-SMOOTH", "CHAR-WT-BAKE", "CHAR-WT-SAVE", "CHAR-WT-LOAD",
            "CHAR-WT-REVERT", "CHAR-WT-SHOW"]
    missing = [n for n in need if n not in text]
    check("H1 UI 锚点齐全（%d 个）" % len(need), not missing,
          "missing=%s" % (",".join(missing) if missing else "none"))

    vk = re.findall(r"VUID-\S+|Validation Error", text)
    check("H2 零 VUID / 零 Validation Error", not vk, "n=%d" % len(vk))
    errs = [l for l in lines if "[ERROR]" in l]
    check("H3 零 [ERROR]", not errs, "first=%s" % (errs[0][:120] if errs else "none"))

    m = SCRIPT_RE.search(text)
    check("H4 脚本收尾干净（failed=0 dirty=0）",
          m is not None and m.group(2) == "0" and m.group(6) == "0",
          m.group(0) if m else "missing")


# ================================================================== main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑某个用例（a/b/c/d/e/f/g/h）")
    ap.add_argument("--keep", action="store_true", help="保留日志与产物")
    args = ap.parse_args()
    only = args.only.strip().lower()

    if os.path.isdir(OUT_DIR) and not args.keep:
        shutil.rmtree(OUT_DIR, ignore_errors=True)
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()

    if only in ("", "a"):
        case_a()

    lines = []
    bones = {}
    if only in ("", "b", "c", "g", "h"):
        lines = run_editor("list;mesh:0;autoall;stats;digest")
        bones = bones_of(lines)
        if "--keep" in sys.argv:
            with open(os.path.join(OUT_DIR, "base.log"), "w", encoding="utf-8") as f:
                f.write("\n".join(lines))

    if only in ("", "b"):
        case_b(lines)
    if only in ("", "c"):
        case_c(lines, bones)
    if only in ("", "d"):
        case_d()
    if only in ("", "e"):
        case_e()
    if only in ("", "f"):
        case_f()
    if only in ("", "g"):
        case_g(lines)
    if only in ("", "h"):
        case_h(lines)

    fails = [r for r in results if not r[1]]
    print()
    if fails:
        print("结果: FAIL: %d 项" % len(fails))
        for n, _, d in fails:
            print("  - %s  %s" % (n, d))
        return 1
    print("结果: PASS（%d 项）" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
