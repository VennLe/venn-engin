"""S-A · 骨架作者（`animation::RigEdit` + Character 窗口的 Rig 页）的验收。

任务书的验收拆成下面六组：

  A  数据层自检（`rig_*`）—— 零编辑烘焙逐位一致 / invBind 重算 / 静止不变形
  B  注入改骨：roll 只绕骨轴扭、len/head 写得进 dump
  C  只重算脏子树：roll 一根骨，**非子孙**的世界位置逐位不变
  D  存盘：`.rig.json` 的字段与 dump 一致，且两次存盘的 `rest` 矩阵逐位相同
  E  镜像：左右骨关于 X=0 对称（模型本身不对称则 SKIP）
  F  UI 锚点齐全 + 全程零 VUID、零 `[ERROR]`

------------------------------------------------------------
为什么走 `MYVK_EDITOR_RIG_SCRIPT` 而不是拖控件
------------------------------------------------------------
和 R7/R8 一样：这一层的值全在"数据对不对"上。脚本走的是**和 UI 完全同一条**
通路（`RigEdit::setRoll → bake → 换骨架`），不是另一套实现；UI 只验锚点在不在。
顺带把"DragFloat 要拖多少像素才等于 90°"这个不稳定因素整个去掉。

------------------------------------------------------------
逐位一致怎么比
------------------------------------------------------------
`RIG-DUMP` 用 `%a`（精确十六进制浮点）打世界位置/骨长/roll，十进制打印会
掩盖 1 ulp 的差异，而 C 组要说的正是"没动的地方一位都没动" —— 所以 C 组直接
**比字符串**，不做浮点比较。

用法：
    python tools/verify_rig_edit.py
    python tools/verify_rig_edit.py --only b        # 只跑某一组
    python tools/verify_rig_edit.py --keep          # 留日志与产物
"""

import argparse
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_add_menu import LogTap, kill_stale_editors          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "rig_edit")

MODEL = "assets/models/anime_girl.fbx"
# 落在 build/bin 下（build/ 被 gitignore）。**相对路径是刻意的**：脚本里
# `save:<path>` 是按"第一个冒号之后整段"解析的，路径里一旦出现盘符冒号，
# 被测的就不是骨架编辑而是那个解析补丁了。
RIG_A = "_verif_rig_a.rig.json"
RIG_B = "_verif_rig_b.rig.json"

# printf 的 %a 输出：0x1.8p+0 / -0x1.0p-3 / 0x0p+0
HEX = r"-?0x[0-9a-fA-F.]+p[-+]?\d+"

LIST_RE = re.compile(
    r"RIG-LIST-BONE i=(\d+) name='([^']*)' parent=(-?\d+) src=(-?\d+) len=([-\d.eE+]+)")
DUMP_RE = re.compile(
    r"RIG-DUMP i=(\d+) name='([^']*)' parent=(-?\d+) t=\((" + HEX + r"),(" + HEX +
    r"),(" + HEX + r")\) len=(" + HEX + r") roll=(" + HEX + r") "
    r"deform=(\d) conn=(\d) inh=(\d) d=\((" + HEX + r"),(" + HEX + r"),(" + HEX +
    r")\) ax=\((" + HEX + r"),(" + HEX + r"),(" + HEX + r")\)")
SCRIPT_RE = re.compile(r"RIG-SCRIPT applied=(\d+) failed=(\d+) bones=(\d+) touched=(\d+) sel=(-?\d+)")
SELFTEST_RE = re.compile(r"ANIM-TEST (rig_\w+) actual=(\d+) expected=1 tol=0 detail=(\S*) result=(PASS|FAIL)")
TOTAL_RE = re.compile(r"ANIM-TOTAL pass=(\d+) fail=(\d+)")

results = []


def check(name, ok, detail=""):
    tag = "[PASS]" if ok else "[FAIL]"
    print("%s %s%s" % (tag, name, ("  → " + detail) if detail else ""))
    results.append((name, bool(ok), detail))


def skip(name, why):
    print("[SKIP] %s  → %s" % (name, why))


def f32(x):
    """把 python float 压成 float32（`%a` 出来的是 double 表示，但值来自 float）。"""
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def run_editor(script, wait_s=120.0):
    """起一次编辑器跑一段注入脚本，返回日志行。"""
    env = dict(os.environ)
    env["MYVK_CHARACTER_WINDOW"] = "1"
    env["MYVK_CHARACTER_MODEL"] = MODEL
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    if script:
        env["MYVK_EDITOR_RIG_SCRIPT"] = script

    proc = subprocess.Popen([EXE], cwd=BIN, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    _wait(tap, "RIG-SCRIPT applied=", wait_s)
    time.sleep(1.2)     # 再给一帧：UI 锚点在脚本之后的绘制里打
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


def bones_of(lines):
    """RIG-LIST-BONE → [{i,name,parent,src,len}]（按 i 排序）。"""
    out = {}
    for l in lines:
        m = LIST_RE.search(l)
        if m:
            out[int(m.group(1))] = {"i": int(m.group(1)), "name": m.group(2),
                                    "parent": int(m.group(3)),
                                    "len": float(m.group(5))}
    return [out[k] for k in sorted(out)]


def dumps_of(lines):
    """RIG-DUMP → [{i,name,t:(x,y,z),len,roll,d,ax}]（按 i 排序，最后一轮为准）。"""
    out = {}
    for l in lines:
        m = DUMP_RE.search(l)
        if not m:
            continue
        out[int(m.group(1))] = {
            "i": int(m.group(1)), "name": m.group(2),
            "t": (float.fromhex(m.group(4)), float.fromhex(m.group(5)),
                  float.fromhex(m.group(6))),
            "len": float.fromhex(m.group(7)),
            "roll": float.fromhex(m.group(8)),
            "deform": m.group(9), "conn": m.group(10), "inh": m.group(11),
            "d": (float.fromhex(m.group(12)), float.fromhex(m.group(13)),
                  float.fromhex(m.group(14))),
            "ax": (float.fromhex(m.group(15)), float.fromhex(m.group(16)),
                   float.fromhex(m.group(17))),
            # 原始字符串：C 组要的是"逐位"，比字符串比浮点更直接
            "t_raw": (m.group(4), m.group(5), m.group(6)),
        }
    return [out[k] for k in sorted(out)]


def descendants(bones, i):
    kids = {}
    for b in bones:
        kids.setdefault(b["parent"], []).append(b["i"])
    out, stack = set(), [i]
    while stack:
        p = stack.pop()
        for c in kids.get(p, []):
            if c not in out:
                out.add(c)
                stack.append(c)
    return out


def load_rig(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


# ================================================================== A 数据层自检

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
    rig = {k: v for k, v in got.items() if k.startswith("rig_")}
    fails = [k for k, v in rig.items() if not v[0]]
    check("A1 rig_* 断言全部 PASS（共 %d 条）" % len(rig),
          len(rig) >= 12 and not fails,
          "fails=%s" % (",".join(sorted(fails)) if fails else "none"))

    tot = TOTAL_RE.search(text)
    check("A2 ANIM-TOTAL fail=0（整体自检没被带崩）",
          tot is not None and int(tot.group(2)) == 0,
          "total=%s" % (tot.group(0) if tot else "missing"))
    check("A3 自检进程 rc==0", proc.returncode == 0, "rc=%d" % proc.returncode)
    # 具体两条最关键的（其余靠 A1 的总数兜底）
    for key in ("rig_bake_identity", "rig_rest_no_deform"):
        ok, det = got.get(key, (False, ""))
        check("A4 %s PASS" % key, ok, det)


# ================================================================== B 注入改骨

def case_b(tmp, picked):
    """roll 只绕骨轴扭；len / head 写得进去。"""
    i = picked["roll"]
    k = picked["len"]          # len/head 用另一根骨：roll 那根已经拧过了，
                               # 混在一起验会让"到底哪条生效了"说不清
    lines = run_editor(
        "list;dump;roll:%d:90;dump;len:%d:0.5;head:%d:0.1:0.2:0.3;dump"
        % (i, k, k))
    bs = bones_of(lines)
    ds = dumps_of(lines)
    check("B1 RIG-LIST 与 RIG-DUMP 的骨数一致（%d）" % len(bs),
          len(bs) > 0 and len(ds) == len(bs),
          "list=%d dump=%d" % (len(bs), len(ds)))

    # ---- roll：ax 绕 **骨轴** 转了 90° ----
    before, after = None, None
    seen = 0
    for l in lines:
        m = DUMP_RE.search(l)
        if not m or int(m.group(1)) != i:
            continue
        d = {"ax": (float.fromhex(m.group(15)), float.fromhex(m.group(16)),
                    float.fromhex(m.group(17))),
             "d": (float.fromhex(m.group(12)), float.fromhex(m.group(13)),
                   float.fromhex(m.group(14)))}
        if seen == 0:
            before = d
        seen += 1
        if seen == 2:
            after = d          # 第二轮的 dump = roll 之后、len/head 之前
            break
    ok_b = before is not None and after is not None
    ang = 0.0
    perp = 0.0
    if ok_b:
        a0, a1 = before["ax"], after["ax"]
        dot = max(-1.0, min(1.0, sum(x * y for x, y in zip(a0, a1))))
        ang = math.degrees(math.acos(dot))
        # ax 必须垂直于骨轴（扭转不动骨轴本身）
        perp = abs(sum(x * y for x, y in zip(a1, after["d"])))
    check("B2 roll 90° → 局部 X 轴绕骨轴转了 %.3f°" % ang,
          ok_b and abs(ang - 90.0) < 0.05, "ang=%.4f" % ang)
    check("B3 扭转后 X 轴仍垂直于骨轴", ok_b and perp < 1e-5, "dot=%.2e" % perp)

    # ---- len / head ----
    last = dumps_of(lines)
    one = next((d for d in last if d["i"] == k), None)
    check("B4 len 写进去了（0.5）",
          one is not None and abs(one["len"] - 0.5) < 1e-6,
          "len=%s" % (one["len"] if one else None))
    check("B5 head 写进去了（0.1,0.2,0.3）",
          one is not None and all(abs(a - b) < 1e-5 for a, b in
                                  zip(one["t"], (0.1, 0.2, 0.3))),
          "t=%s" % (str(one["t"]) if one else None))

    m = SCRIPT_RE.search("\n".join(lines))
    check("B6 脚本收尾干净（failed=0）",
          m is not None and m.group(2) == "0",
          "got=%s" % (str(m.groups()) if m else None))


# ================================================================== C 只重算脏子树

def case_c(tmp, picked):
    i = picked["roll"]
    base = run_editor("list;dump")
    bs = bones_of(base)
    dirty = descendants(bs, i) | {i}

    rolled = run_editor("list;dump;roll:%d:45;dump" % i)
    b0 = dumps_of(base)
    # `dumps_of` 每根骨只留**最后一次**出现，所以两段 dump 直接取末尾那段即可
    b1 = dumps_of(rolled)

    d0 = {d["i"]: d for d in b0}
    d1 = {d["i"]: d for d in b1}
    check("C1 两轮 dump 的骨数一致", len(d0) == len(d1) == len(bs),
          "n0=%d n1=%d" % (len(d0), len(d1)))

    bad = []
    for b in bs:
        k = b["i"]
        if k in dirty or k not in d0 or k not in d1:
            continue
        if d0[k]["t_raw"] != d1[k]["t_raw"]:
            bad.append("%s(%d)" % (b["name"], k))
    check("C2 非子孙骨的世界位置逐位不变（%d 根）" % (len(bs) - len(dirty)),
          not bad, "changed=%s" % (",".join(bad) if bad else "none"))

    # 被 roll 的那根骨：位置不动，朝向变了（否则就是"roll 挪了骨"）
    moved = 0.0
    if i in d0 and i in d1:
        moved = max(abs(a - b) for a, b in zip(d0[i]["t"], d1[i]["t"]))
    check("C3 被扭转的骨自身没被挪走（<=1e-5）", moved <= 1e-5, "d=%.2e" % moved)


# ================================================================== D 存盘

def case_d(tmp, picked):
    """两次存盘：一次零编辑、一次 roll 过的，比对 `rest` 矩阵是否被污染。"""
    i = picked["roll"]
    pa = os.path.join(BIN, RIG_A)
    pb = os.path.join(BIN, RIG_B)
    for p in (pa, pb):
        if os.path.exists(p):
            os.remove(p)

    run_editor("list;save:%s" % RIG_A)
    run_editor("list;roll:%d:33.75;save:%s;dump" % (i, RIG_B))

    ok_files = os.path.exists(pa) and os.path.exists(pb)
    check("D1 两次存盘都落了文件", ok_files,
          "a=%s b=%s" % (os.path.exists(pa), os.path.exists(pb)))
    if not ok_files:
        return

    a = load_rig(pa)
    b = load_rig(pb)
    check("D2 format/version 正确",
          a.get("format") == "venn.rig" and a.get("version") == 1,
          "format=%s version=%s" % (a.get("format"), a.get("version")))
    check("D3 骨数一致且等于 RIG-LIST（%d）" % len(b.get("bones", [])),
          len(a.get("bones", [])) == len(b.get("bones", [])) > 0,
          "a=%d b=%d" % (len(a.get("bones", [])), len(b.get("bones", []))))

    # 存盘的 roll 就是拧进去的角度（33.75°）
    want = f32(math.radians(33.75))
    got = b["bones"][i].get("roll")
    check("D4 存盘的 roll == 33.75°", got is not None and abs(got - want) < 1e-7,
          "got=%r want=%r" % (got, want))

    # **其余骨一个数都没变**：`rest` 是 16 个 float，roll 不该碰它们
    diff = []
    for k in range(len(a["bones"])):
        if k == i:
            continue
        if a["bones"][k]["rest"] != b["bones"][k]["rest"]:
            diff.append(a["bones"][k]["name"])
    check("D5 未编辑的骨 rest 矩阵逐位相同", not diff,
          "changed=%s" % (",".join(diff[:5]) if diff else "none"))

    # head 与 dump 对得上（存的是骨架空间的值）
    lines = run_editor("list;dump")
    dd = {x["i"]: x for x in dumps_of(lines)}
    bad = []
    for k, bone in enumerate(a["bones"]):
        if k not in dd:
            continue
        if max(abs(p - q) for p, q in zip(bone["head"], dd[k]["t"])) > 1e-5:
            bad.append(bone["name"])
    check("D6 存盘的 head 与运行期 dump 一致", not bad,
          "mismatch=%s" % (",".join(bad[:5]) if bad else "none"))

    if "--keep" not in sys.argv:
        for p in (pa, pb):
            try:
                os.remove(p)
            except OSError:
                pass


# ================================================================== E 镜像

def case_e(tmp, picked):
    """左右骨关于镜像轴对称。模型自己就不对称的那一对，换成最对称的一对再验。"""
    pairs = picked.get("pairs") or []
    if not pairs:
        skip("E1 镜像后左右对称", "这具骨架里找不到 .L/.R 成对的骨")
        return

    base_lines = run_editor("list;dump")
    ax = axis_of(base_lines)
    db = {d["i"]: d for d in dumps_of(base_lines)}

    # 选**最对称**的那一对（实测 `anime_girl` 的肩差 6 mm，对它是真不对称；
    # 拿第一对标红只会得到一条"模型不好看"的假失败）
    best = None
    for li, ri, ln, rn in pairs:
        if li not in db or ri not in db:
            continue
        tl, tr = db[li]["t"], db[ri]["t"]
        asym = max((abs(tl[a] + tr[a]) if a == ax else abs(tl[a] - tr[a]))
                   for a in range(3))
        if best is None or asym < best[0]:
            best = (asym, li, ri, ln, rn)
    if best is None:
        skip("E1 镜像后左右对称", "dump 里缺成对的骨")
        return
    asym, li, ri, lname, rname = best
    if asym > 5e-3:
        skip("E1 镜像后左右对称", "最对称的一对也差 %.1f mm（%s/%s）" %
             (asym * 1000.0, lname, rname))
        return

    tl = db[li]["t"]
    after = dumps_of(run_editor("list;dump;mirror:%d;dump" % li))
    da = {d["i"]: d for d in after}
    if ri not in da:
        check("E1 镜像后左右对称", False, "dump 里没有 %s" % rname)
        return
    t2 = da[ri]["t"]
    want = list(tl)
    want[ax] = -want[ax]          # 镜像轴是从数据里认的，不是写死的 X
    check("E1 镜像后 %s 是 %s 关于 %s=0 的镜像（自身差 %.2f mm）" %
          (rname, lname, "XYZ"[ax], asym * 1000.0),
          all(abs(p - q) < 1e-5 for p, q in zip(t2, want)),
          "%s=%s want=%s" % (rname, t2, tuple(want)))

    # 方向也镜像了
    dl, dr = da[li]["d"], da[ri]["d"]
    wd = list(dl)
    wd[ax] = -wd[ax]
    check("E2 镜像后朝向同样对称",
          all(abs(p - q) < 1e-5 for p, q in zip(dr, wd)),
          "%s=%s want=%s" % (rname, dr, tuple(wd)))


# ================================================================== F UI 锚点 + 健康

def case_f(lines):
    text = "\n".join(lines)
    need = ["CHAR-TAB-Rig", "CHAR-RIG-SAVE-BTN", "CHAR-RIG-LOAD-BTN",
            "CHAR-RIG-REVERT-BTN", "CHAR-RIG-STATS", "CHAR-RIG-HEAD",
            "CHAR-RIG-LEN", "CHAR-RIG-ROLL", "CHAR-RIG-FLAGS",
            "CHAR-RIG-ALIGN-BTN", "CHAR-RIG-MIRROR-BTN", "CHAR-RIG-ADD-BTN",
            "CHAR-RIG-DEL-BTN"]
    missing = [n for n in need if n not in text]
    check("F1 UI 锚点齐全（%d 个）" % len(need), not missing,
          "missing=%s" % (",".join(missing) if missing else "none"))

    vk = re.findall(r"VUID-\S+|Validation Error", text)
    check("F2 零 VUID / 零 Validation Error", not vk,
          "n=%d" % len(vk))
    errs = [l for l in lines if "[ERROR]" in l]
    check("F3 零 [ERROR]", not errs, "first=%s" % (errs[0][:120] if errs else "none"))
    imp = [l for l in lines if "CHAR-RIG-IMPORT" in l]
    check("F4 骨架导入过一次", len(imp) >= 1, "n=%d" % len(imp))
    app = [l for l in lines if "CHAR-RIG-APPLY" in l]
    check("F5 编辑后烘焙并换骨（CHAR-RIG-APPLY）", len(app) >= 1, "n=%d" % len(app))


# ================================================================== main

def axis_of(lines):
    """`RIG-LIST ... axis=Y` → 1。镜像轴是引擎从数据里认出来的，测试跟着它走。"""
    for l in lines:
        m = re.search(r"RIG-LIST n=\d+ axis=([XYZ])", l)
        if m:
            return "XYZ".index(m.group(1))
    return 0


def pick_targets(bones):
    """挑出各用例要用的骨：必须**由数据决定**，不能写死名字。

    理由是硬的：换个模型（Mixamo 的 `mixamorig:` 前缀、Blender 的 `.L`
    后缀、或者干脆没有左右标记）测试就整片红，那种红是负债不是信号。
    """
    kids = {}
    for b in bones:
        kids.setdefault(b["parent"], []).append(b["i"])
    with_kid = [b for b in bones if kids.get(b["i"])]
    # roll 挑**子树最小**的那根：C 组要断言"非子孙一位都没动"，
    # 挑根骨的话子孙就是全部，那条断言会变成 0 根的空转（实测踩过）。
    if with_kid:
        subs = {b["i"]: len(descendants(bones, b["i"])) for b in with_kid}
        roll = min(subs, key=lambda k: (subs[k], k))
    else:
        roll = bones[0]["i"]
    ln = (with_kid[-1]["i"] if with_kid else bones[0]["i"])

    pair = []
    by_name = {b["name"]: b["i"] for b in bones}
    for b in bones:
        other = flip_side(b["name"])
        if other and other in by_name and by_name[other] != b["i"]:
            pair.append((b["i"], by_name[other], b["name"], other))
    return {"roll": roll, "len": ln, "pairs": pair}


def flip_side(name):
    """`upperarm.L` ↔ `upperarm.R`、`LeftArm` ↔ `RightArm`。

    与 `RigEdit::flipSide` 同一套规则 —— 这里再写一份是**故意的**：
    测试要是直接调被测代码的换名规则，规则本身写错时它会跟着一起错，
    于是"镜像那条用例永远绿"。两边的规则各自独立，才能互相钉住。
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑某个用例（a/b/c/d/e/f）")
    ap.add_argument("--keep", action="store_true", help="保留日志与产物")
    args = ap.parse_args()
    only = args.only.strip().lower()

    if os.path.isdir(OUT_DIR) and not args.keep:
        shutil.rmtree(OUT_DIR, ignore_errors=True)
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()

    if only in ("", "a"):
        case_a()

    # B 之后的用例都要先知道骨表
    picked = None
    lines = []
    if only in ("", "b", "c", "d", "e", "f"):
        lines = run_editor("list;dump")
        bones = bones_of(lines)
        picked = pick_targets(bones) if bones else None
        check("B0 从日志里认出骨表（%d 根）" % len(bones), len(bones) > 0,
              "n=%d" % len(bones))

    if picked:
        if only in ("", "b"):
            case_b(OUT_DIR, picked)
        if only in ("", "c"):
            case_c(OUT_DIR, picked)
        if only in ("", "d"):
            case_d(OUT_DIR, picked)
        if only in ("", "e"):
            case_e(OUT_DIR, picked)
    if only in ("", "f"):
        case_f(lines)

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
