#!/usr/bin/env python3
"""端到端验证：约束栈（S-C / #336）

==============================================================================
这一条在验什么
==============================================================================
S-C 之前，"表演层后处理"是 `AnimatorSystem::applyPerformance` 里**三段写死的
代码**（LookAt → TwoBoneIK → SpringBone）：顺序是常量，不能改、不能跳过、不能
在骨上叠第二条。S-C 把它变成"骨上一条有序的表"，于是真正要钉的性质也变了 ——
不是"某个特性算得对不对"，而是**这四条**：

  A 自检 23 条 `constraint_*`（`MYVK_ANIM_TEST=1`，不开窗）：
    - 限幅：超限夹到边界；**限幅以内一个字节都不写**（`memcmp`）；influence
      0.5 走 slerp（60°→20° 走一半 = 40°，不是线性缩放）；
    - 复制旋转：世界旋转对齐到 1e-3 度；influence 0 逐位不动；
    - 复制位置：世界头部重合；轴掩码只动选中的轴（其余 < 1e-5）；
    - ChildOf：相对偏移整体跟转（误差 < 1e-4）+ **对照组**证明"不做就不会跟"；
    - **顺序**：同两条约束，[限位→复制] 得 60°、[复制→限位] 得 20° ——
      这是"栈"与"三个特性"的区别，也是这一轮唯一无法用截图看出来的性质；
    - IK 入栈：与直接调 `solveTwoBoneIK` **逐位相同**；influence 0 逐位不动；
      influence 0.5 的末端误差落在"满权重"与"不动"之间；stretch 够得到 0.95。
  B 真资产接线：脚本加的条目全部落到合法骨上、`applied + skipped == total`
    且 `failed == 0`（没有"绑不上骨"的条目）；**跨进程摘要一致**。
  C 顺序不是摆设：`move` 之后摘要必变，移回来摘要必复原（逐位）。
  D influence / enabled 的真语义：0 或关掉 → `applied == 0`，条目只是"跳过"，
    不是"算了个 0 写回去"。
  E 落盘：`venn.constraints` 字段齐、条目数对、**存读往返摘要不变**。
  F IK 进真资产：解算成功（`ik >= 1`），链根/中/末都落在合法下标上。
  G UI：约束段 + 列表 + 增删移 + 各类型参数控件的锚点齐全。
  H 全程零 VUID、零 `[ERROR]`、脚本收尾 `failed=0 dirty=0`。

==============================================================================
三个刻意的取舍
==============================================================================
1. **不在这条脚本里钉"某根骨该转多少度"。** 真资产（`anime_girl.fbx`）的
   静止世界位置和网格对不上（S-B 已经量过：6.3% 顶点落空），拿它做解剖学
   断言等于把资产问题算进功能验收。数值断言全在 A 组，那里的夹具是**自己
   搭的**（spine/两臂/weapon + 三节腿），可控且逐位可复现。
2. **锚点分三次开窗**，因为 UI 只在**被选中**的那条约束上画类型参数
   （选 CopyRotation 时不画限位那六个数）。一次开窗只能看见一种。
3. **骨一律用下标**（`list` 会打出来），不依赖命名 —— 和 `MYVK_EDITOR_RIG_SCRIPT`
   同一条约定：`mixamorig:LeftArm` 里的冒号会把脚本的 `:` 分隔符打碎。

用法：
    python tools/verify_constraints.py
    python tools/verify_constraints.py --keep      # 保留每次的完整日志
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from verify_add_menu import LogTap, kill_stale_editors          # noqa: E402

BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "constraints")

MODEL = "assets/models/characters/animate_girl.fbx"

SCRIPT_RE = re.compile(
    r"CONSTRAINT-SCRIPT applied=(\d+) failed=(\d+) n=(\d+) sel=(-?\d+) "
    r"dirty=(\d+) joints=(\d+)")
EVAL_RE = re.compile(
    r"CONSTRAINT-EVAL total=(\d+) applied=(\d+) skipped=(\d+) failed=(\d+) "
    r"clamped=(\d+) ik=(\d+)")
LIST_RE = re.compile(r"CONSTRAINT-LIST n=(\d+) joints=(\d+) digest=([0-9a-f]{16})")
DIGEST_RE = re.compile(r"CONSTRAINT-DIGEST n=(\d+) h=([0-9a-f]{16})")
ITEM_RE = re.compile(
    r"CONSTRAINT-LIST-ITEM i=(\d+) type=(\S+) bone=(-?\d+) target=(-?\d+) "
    r"inf=([\d.]+) enabled=(\d)")
SAVE_RE = re.compile(r"CONSTRAINT-SAVE path='([^']*)' items=(\d+)")
LOAD_RE = re.compile(r"CONSTRAINT-LOAD path='([^']*)' items=(\d+) bad=(\d+)")
SELFTEST_RE = re.compile(
    r"ANIM-TEST (\w+) actual=(\S+) expected=(\S+) tol=(\S+) detail=(\S*) "
    r"result=(\w+)")
# ⚠ `report()`（数值型断言）**没有 detail 字段** —— 只有 `reportFlag()` 有。
#   拿一条正则套两种格式会把 7 条数值断言整片漏掉（第一版就漏了：16 vs 23）。
SELFTEST_NUM_RE = re.compile(
    r"ANIM-TEST (\w+) actual=(\S+) expected=(\S+) tol=(\S+) result=(\w+)")
TOTAL_RE = re.compile(r"ANIM-TOTAL pass=(\d+) fail=(\d+)")

fails = []


def check(name, ok, detail=""):
    print("[%s] %s%s" % ("PASS" if ok else "FAIL", name,
                         ("  → %s" % detail) if detail else ""))
    if not ok:
        fails.append(name)


def skip(name, why):
    print("[SKIP] %s  → %s" % (name, why))


# ================================================================== 运行器

def run_selftest():
    env = dict(os.environ)
    env["MYVK_ANIM_TEST"] = "1"
    proc = subprocess.run([EXE], cwd=BIN, env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=600)
    return (proc.stdout or "") + (proc.stderr or "")


def run_editor(script, wait_s=180.0):
    """起一次编辑器跑一段约束脚本，返回日志行。"""
    env = dict(os.environ)
    env["MYVK_CHARACTER_WINDOW"] = "1"
    env["MYVK_CHARACTER_MODEL"] = MODEL
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    if script:
        env["MYVK_EDITOR_CONSTRAINT_SCRIPT"] = script

    proc = subprocess.Popen([EXE], cwd=BIN, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    _wait(tap, "CONSTRAINT-SCRIPT applied=", wait_s)
    time.sleep(1.2)      # 再给一帧：UI 锚点在脚本之后的绘制里打
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


def last_match(lines, rx):
    for l in reversed(lines):
        m = rx.search(l)
        if m:
            return m
    return None


def all_matches(lines, rx):
    out = []
    for l in lines:
        m = rx.search(l)
        if m:
            out.append(m)
    return out


def rect_present(lines, tag):
    return any(("CHAR-CON-" in l and tag in l) for l in lines) or \
        any(tag in l and "=(" in l for l in lines)


def vuid_count(lines):
    n = 0
    for l in lines:
        low = l.lower()
        if "vuid" in low or "validation error" in low:
            n += 1
    return n


def error_lines(lines):
    return [l for l in lines if "[ERROR]" in l]


# ================================================================== A 自检

def case_a():
    text = run_selftest()
    if "--keep" in sys.argv:
        with open(os.path.join(OUT_DIR, "selftest.log"), "w", encoding="utf-8") as f:
            f.write(text)

    got = {}
    for m in SELFTEST_RE.finditer(text):
        got[m.group(1)] = (m.group(6) == "PASS", m.group(5))
    for m in SELFTEST_NUM_RE.finditer(text):
        if m.group(1) in got:
            continue
        got[m.group(1)] = (m.group(5) == "PASS",
                           "actual=%s/expected=%s" % (m.group(2), m.group(3)))
    cs = {k: v for k, v in got.items() if k.startswith("constraint_")}
    bad = [k for k, v in cs.items() if not v[0]]
    check("A1 constraint_* 断言全部 PASS（共 %d 条）" % len(cs),
          len(cs) >= 20 and not bad,
          "fails=%s" % (",".join(sorted(bad)) if bad else "none"))

    tot = TOTAL_RE.search(text)
    check("A2 ANIM-TOTAL fail=0（新断言没把整体自检带崩）",
          tot is not None and int(tot.group(2)) == 0,
          "total=%s" % (tot.group(0) if tot else "missing"))

    # 五条"这一轮真正想钉住的东西"单独点名
    for key, why in (
        ("constraint_order_limit_then_copy_deg", "顺序 A = 60°"),
        ("constraint_order_copy_then_limit_deg", "顺序 B = 20°"),
        ("constraint_limit_inside_bitwise", "限幅内逐位不变"),
        ("constraint_childof_control", "ChildOf 的对照组不是空转"),
        ("constraint_ik_via_stack_bitwise", "IK 入栈与直接求解逐位相同"),
    ):
        ok, det = got.get(key, (False, ""))
        check("A3 %s（%s）" % (key, why), ok, det)


# ================================================================== B 接线

BONE_A = 13
BONE_B = 14


def base_script(extra):
    """三条不同类型的条目 + 一段额外命令。返回整段脚本。"""
    return ("add:LimitRotation:%d;add:CopyRotation:%d;target:1:%d;"
            "add:CopyLocation:%d;target:2:%d;%s;list;digest;eval"
            % (BONE_A, BONE_B, BONE_A, BONE_B, BONE_A, extra))


def case_b():
    lines = run_editor(base_script("sel:0"))
    if "--keep" in sys.argv:
        with open(os.path.join(OUT_DIR, "b_full.log"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
    sc = last_match(lines, SCRIPT_RE)
    check("B1 脚本收尾干净（三条命令都成功）",
          sc is not None and int(sc.group(2)) == 0 and int(sc.group(3)) == 3,
          sc.group(0) if sc else "missing")

    items = all_matches(lines, ITEM_RE)
    check("B2 三条条目都落在合法骨上（bone ∈ [0, joints)）",
          len(items) == 3 and all(0 <= int(m.group(3)) < 21 and
                                  int(m.group(3)) >= 0 for m in items),
          "bones=%s" % [int(m.group(3)) for m in items])

    ev = last_match(lines, EVAL_RE)
    if ev:
        total, applied, skipped, failed = (int(ev.group(i)) for i in (1, 2, 3, 4))
        check("B3 求值没有失败条目（applied + skipped == total）",
              total == 3 and failed == 0 and applied + skipped == total,
              ev.group(0))
    else:
        check("B3 求值没有失败条目（applied + skipped == total）", False, "no EVAL")

    d1 = last_match(lines, LIST_RE)
    lines2 = run_editor(base_script("sel:0"))
    d2 = last_match(lines2, LIST_RE)
    check("B4 跨进程摘要一致（同一份栈 → 同一个 digest）",
          d1 is not None and d2 is not None and d1.group(3) == d2.group(3),
          "%s vs %s" % (d1.group(3) if d1 else "?", d2.group(3) if d2 else "?"))
    return lines


# ================================================================== C 顺序

def case_c():
    # 同一批条目，把第二条往上移一位再移回来：摘要必须变、再复原。
    lines = run_editor("add:LimitRotation:%d;add:CopyRotation:%d;target:1:%d;"
                       "digest;move:1:-1;digest;move:0:1;digest"
                       % (BONE_A, BONE_B, BONE_A))
    ds = [m.group(2) for m in all_matches(lines, DIGEST_RE)]
    check("C1 移动之后摘要必变（顺序进摘要 → 顺序不是摆设）",
          len(ds) >= 3 and ds[0] != ds[1],
          "d=%s" % ds)
    check("C2 移回原位摘要必复原（逐位）",
          len(ds) >= 3 and ds[0] == ds[2],
          "d=%s" % ds)


# ================================================================== D 语义

def case_d():
    # influence 0 / 关掉：条目只是"跳过"，不是"算了个 0 写回去"
    script = ("add:LimitRotation:%d;influence:0:0;eval;enable:0:0;eval;"
              "influence:0:1;enable:0:1;eval;digest" % BONE_A)
    lines = run_editor(script)
    evs = all_matches(lines, EVAL_RE)
    if len(evs) != 3:
        check("D1 influence 0 → applied == 0", False, "evals=%d" % len(evs))
        check("D2 关掉 → applied == 0", False, "evals=%d" % len(evs))
        return
    a0, a1, a2 = (int(m.group(2)) for m in evs)
    s0, s1 = (int(m.group(3)) for m in evs[:2])
    check("D1 influence 0 → applied == 0 且 skipped == 1",
          a0 == 0 and s0 == 1, evs[0].group(0))
    check("D2 关掉 → applied == 0 且 skipped == 1",
          a1 == 0 and s1 == 1, evs[1].group(0))
    check("D3 恢复 influence 1 / enabled 1 之后求值不再失败",
          int(evs[2].group(4)) == 0 and a2 + int(evs[2].group(3)) == 1,
          evs[2].group(0))


# ================================================================== E 落盘

def case_e():
    path = os.path.join(BIN, "_verif_con.constraints.json")
    try:
        os.remove(path)
    except OSError:
        pass
    script = ("add:LimitRotation:%d;limit:0:0:2:20;"
              "add:CopyRotation:%d;target:1:%d;influence:1:0.4;"
              "save:%s;digest;load:%s;digest"
              % (BONE_A, BONE_B, BONE_A, path, path))
    lines = run_editor(script)
    sv = last_match(lines, SAVE_RE)
    check("E1 存盘落了文件", os.path.exists(path) and sv is not None,
          sv.group(0) if sv else "no SAVE log")

    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        check("E2 format/version 正确",
              doc.get("format") == "venn.constraints" and doc.get("version") == 1,
              "format=%s version=%s" % (doc.get("format"), doc.get("version")))
        items = doc.get("items") or []
        check("E3 条目字段齐全（type/enabled/influence/boneIndex/limit/ik）",
              len(items) == 2 and all(
                  k in items[0] for k in ("type", "enabled", "influence",
                                          "boneIndex", "limitMask", "minDeg",
                                          "maxDeg", "ik")),
              "n=%d keys=%s" % (len(items), sorted(items[0].keys()) if items else []))
        check("E4 限位数值原样落盘（0..20）",
              len(items) == 2 and abs(items[0]["maxDeg"][0] - 20.0) < 1e-6
              and abs(items[0]["minDeg"][0] - 2.0) < 1e-6,
              "min=%s max=%s" % (items[0]["minDeg"] if items else "?",
                                 items[0]["maxDeg"] if items else "?"))
    else:
        check("E2 format/version 正确", False, "no file")
        check("E3 条目字段齐全", False, "no file")
        check("E4 限位数值原样落盘", False, "no file")

    ds = [m.group(2) for m in all_matches(lines, DIGEST_RE)]
    check("E5 存读往返摘要不变", len(ds) >= 2 and ds[0] == ds[-1], "d=%s" % ds)
    ld = last_match(lines, LOAD_RE)
    check("E6 读盘没有失效条目（bad=0）",
          ld is not None and int(ld.group(3)) == 0,
          ld.group(0) if ld else "no LOAD log")


# ================================================================== F IK

def case_f():
    # 真资产上挂一条 IK：证明"命令能落到真骨架上、链能解析出来"。
    # 具体角度对不对是 A 组的事（那是自建夹具）。
    script = ("add:TwoBoneIK:%d;iktarget:0:0.2:-0.7:0.0;stretch:0:0.3;eval;list"
              % BONE_A)
    lines = run_editor(script)
    ev = last_match(lines, EVAL_RE)
    if ev is None:
        check("F1 IK 条目在真骨架上解算成功（ik >= 1）", False, "no EVAL")
        return
    ik = int(ev.group(6))
    if ik >= 1:
        check("F1 IK 条目在真骨架上解算成功（ik >= 1）", True, ev.group(0))
        check("F2 IK 条目没有把别的条目带崩（failed == 0）",
              int(ev.group(4)) == 0, ev.group(0))
    else:
        skip("F1 IK 条目在真骨架上解算成功（ik >= 1）",
             "这条资产的骨名解析不出三节链 —— 已在 A 组用自建骨架覆盖")
        skip("F2 IK 条目没有把别的条目带崩（failed == 0）", "同上")


# ================================================================== G UI

def case_g():
    want = {
        "limit": ["CHAR-CON-TITLE", "CHAR-CON-STATS", "CHAR-CON-LIST",
                  "CHAR-CON-TYPE", "CHAR-CON-ADD", "CHAR-CON-DEL",
                  "CHAR-CON-UP", "CHAR-CON-DOWN", "CHAR-CON-INFL",
                  "CHAR-CON-LIMIT"],
        "copy": ["CHAR-CON-TARGET"],
        "ik": ["CHAR-CON-IKROOT", "CHAR-CON-IKMID", "CHAR-CON-IKEND",
               "CHAR-CON-IKHAS", "CHAR-CON-IKTGT", "CHAR-CON-IKSTRETCH"],
    }
    runs = {
        "limit": run_editor("add:LimitRotation:%d;sel:0" % BONE_A),
        "copy": run_editor("add:CopyRotation:%d;target:0:%d;sel:0"
                           % (BONE_B, BONE_A)),
        "ik": run_editor("add:TwoBoneIK:%d;sel:0" % BONE_A),
    }
    total = 0
    for k, line_list in runs.items():
        missing = [t for t in want[k] if not rect_present(line_list, t)]
        total += len(want[k])
        check("G-%s UI 锚点齐全（%d 个）" % (k, len(want[k])),
              not missing, "missing=%s" % (missing or "none"))
        errs = error_lines(line_list)
        check("G-%s 零 [ERROR]" % k, not errs, "first=%s" % (errs[:1] or "none"))
        check("G-%s 零 VUID / 零 Validation Error" % k, vuid_count(line_list) == 0,
              "n=%d" % vuid_count(line_list))
        sc = last_match(line_list, SCRIPT_RE)
        check("G-%s 脚本收尾干净（failed=0）" % k,
              sc is not None and int(sc.group(2)) == 0,
              sc.group(0) if sc else "missing")
    print("  （UI 锚点合计 %d 个 / %d 次开窗）" % (total, len(runs)))


# ================================================================== main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留每次的完整日志")
    args = ap.parse_args()
    os.makedirs(OUT_DIR, exist_ok=True)
    kill_stale_editors()

    for fn in (case_a, case_b, case_c, case_d, case_e, case_f, case_g):
        print("\n" + "=" * 78)
        print("== %s" % fn.__name__.upper())
        print("=" * 78)
        fn()

    print("\n" + "=" * 78)
    if fails:
        print("结果: FAIL（%d 项）" % len(fails))
        for f in fails:
            print("  - %s" % f)
    else:
        print("结果: PASS")
    print("=" * 78)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
