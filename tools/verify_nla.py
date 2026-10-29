#!/usr/bin/env python3
"""端到端验证：NLA 时间轴（R9 / #337）

==============================================================================
这一条在验什么
==============================================================================
NLA 把一个 Action 变成**时间轴上的片段**（strip）：有起止时刻、可以错开、
可以叠、可以缩放。它跟 `AnimatorComponent::layers`（按骨覆盖的当前姿态）
**不是**同一个东西 —— 语义差别写在 `animation/NlaTimeline.h` 的开头，
`NlaPanel.h` 的开头写的是"这个差别在 UI 上长什么样"。

本脚本两半，重要性差得很远：

  **"什么都没变"**（占一半以上）
    · 空 `nlaTracks` 时 `pose` 必须**逐位**不变（`nla_empty_bitwise`）；
    · 播放头落在所有 strip 之外时同样逐位不变
      （`nla_no_active_strip_bitwise`）；
    · 权重 0 / 静音 / 遮罩一根骨都没命中 → 全都"这条不生效"，
      而不是"全身覆盖"。

  **"时间映射是可手算的"**
    `sourceTime` 是这一层唯一的新数学（span / speed / loop /
    clipStart..clipEnd 五个变量折成一个时刻）。所有断言都挑在能口算的点上
    （0.25 / 0.5 / 0.75）—— "看起来在动"完全不能证明映射是对的：
    一段匀速直线在错误的映射下照样匀速。

==============================================================================
分组
==============================================================================
  A 自检 `nla_*`（纯数值、不开窗）+ 点名五条底线 + `ANIM-TOTAL fail=0`
  B 面板与目标：`NLA-OPEN` / `NLA-TARGET` / 画布宽度 > 0
  C 结构编辑：加轨 + 两条 strip，摘要每次变、`NLA-CANVAS strips=2`
  D 拖动与量化：`move`/`resize` 落到 **1 ms** 量化后的值，负起点被夹到 0
  E 运行时：`ANIM-NLA applied=1 skipped=0`（真进了 `AnimatorSystem`）
  F 静音 / 独占：`trackmute` 之后同一帧 `applied=0`
  G 跨进程存读：存盘后文件里出现 `nlaTracks`，新进程读回来**摘要一致**
  H UI 锚点 + 零 VUID + 零 `[ERROR]`

==============================================================================
两个刻意的取舍
==============================================================================
① **不做像素判据。**
   "上下半身分别动"这件事已经由 `verify_anim_layers.py` 用上下带的像素统计
   验过了 —— 而 NLA 的混合**走的就是同一个 `blendPoseMasked`**（遮罩语义
   一个字没改，见 NlaTimeline.cpp 的注释）。再拍一遍截图只是把同一个断言
   写成更慢、更脆的版本。NLA 新增的风险全在**时间映射与包络**上，那是数值。
② **存读往返用两个进程。**
   同进程里"存了再读"很容易命中的是内存里的旧对象，看起来"读回来了"
   其实什么都没读。换一个进程重新打开才算数。

用法：
    python tools/verify_nla.py
    python tools/verify_nla.py --only a
    python tools/verify_nla.py --dump b.log
"""

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")

sys.path.insert(0, HERE)
from verify_add_menu import LogTap                      # noqa: E402

FRAMES = 140
SRC_SCENE = os.path.join(ROOT, "assets", "scenes", "anime_demo.json")
# 探针场景放 build/：它是**生成物**，不该进 assets/（进了就会被人当资产引用）
PROBE = os.path.join(ROOT, "build", "_nla_probe.json")

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ------------------------------------------------------------------ 夹具
#
# 复用 `anime_demo.json` 里那个"有 skinned + animator + Walk 段"的实体。
#
# ⚠ 为什么不手写一个最小场景：`skinned` 块只建 `SkinnedMeshComponent`，
#   真正被渲染的是**每个 submesh 一个子实体**（由 `scene::instantiateModel`
#   建）。手写的实体在日志里一切正常、屏幕上什么都没有 —— 这个坑
#   `verify_anim_layers.py` 的文件头记着（第一版白查了三轮）。
#   本脚本不做像素判据，但"骨架里真的有 Walk 段"仍然要真的存在，
#   否则 `NLA-STAT` / `ANIM-NLA` 全是 0 而看起来像"功能没接线"。
# 一套"建出两条 strip + 改权重/包络"的脚本。
# ⚠ G 组（存读往返）**必须复用这一份** —— 第一版在存盘那个进程里只跑了
#   `save`，于是存下去的是**空** NLA，而"文件里有 nlaTracks"照样成立
#   （存的是别人之前写进去的）。断言全绿、什么都没验到。
# ⚠ 里面有两个 `digest`：D4 断的是"每改一次结构摘要都必须变"，
#   只有一个摘要的话那条断言只能证明"摘要不为空"。
SCRIPT_BUILD = (
    "addtrack;"
    "addstrip:Walk:0:1;"
    "addstrip:Walk:1:2;"
    "digest;stat;"
    "move:0.25:1.25;"
    "resize:0.1:0.9;"
    "select:0:1;weight:0.5;blendin:0.2;blendout:0.2;"
    "digest;stat")


def make_probe(dst=None):
    dst = dst or PROBE
    with open(SRC_SCENE, "r", encoding="utf-8") as f:
        doc = json.load(f)
    # 去掉可能残留的 NLA（源场景本来就没有，这里只是把纪律写明）
    for e in doc.get("entities", []):
        if "animator" in e:
            e["animator"].pop("nlaTracks", None)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return dst


# ------------------------------------------------------------------ 启动
def run_editor(extra_env, wait_s=180.0):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"     # 不设它 ui::logRect 直接 return
    env["MYVK_GRID"] = "0"
    env["MYVK_FRAMES"] = str(FRAMES)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    for k, v in extra_env.items():
        if v is not None:
            env[k] = v

    # 上一轮崩掉的实例会抢窗口标题 → 先清一遍
    try:
        subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                       capture_output=True, timeout=10)
    except Exception:
        pass

    proc = subprocess.Popen([EDITOR_EXE], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        proc.wait(timeout=wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=20)
    tap.t.join(timeout=10)
    with tap.lock:
        lines = list(tap.lines)
    return "\n".join(lines).replace("\x00", "")


# ------------------------------------------------------------------ 解析
OPEN_RE = re.compile(r"NLA-OPEN target=(.*) follow=(\d+)")
TARGET_RE = re.compile(r"NLA-TARGET src=(\S+) ent=(.*)$")
TRK_ADD_RE = re.compile(r"NLA-TRK added=1 n=(\d+) sel=(-?\d+)")
TRK_EDIT_RE = re.compile(r"NLA-TRK trk=(\d+) (\w+)=(\d+)")
STRIP_ADD_RE = re.compile(
    r"NLA-STRIP added=1 trk=(-?\d+) strip=(-?\d+) start=([-\d.]+) "
    r"end=([-\d.]+)")
MOVE_RE = re.compile(
    r"NLA-MOVE trk=(-?\d+) strip=(-?\d+) start=([-\d.]+) end=([-\d.]+) "
    r"why=(\S+)")
STAT_RE = re.compile(
    r"NLA-STAT tracks=(\d+) strips=(\d+) t=([-\d.]+) dur=([-\d.]+) "
    r"sel=(-?\d+)/(-?\d+) playing=(\d+) digest=([0-9a-f]+)")
DIGEST_RE = re.compile(r"NLA-DIGEST digest=([0-9a-f]+)")
CANVAS_RE = re.compile(
    r"NLA-CANVAS tracks=(\d+) strips=(\d+) px=([-\d.]+) scroll=([-\d.]+) "
    r"w=([-\d.]+)")
TIME_RE = re.compile(r"NLA-TIME t=([-\d.]+) src=(\w+)")
PLAY_RE = re.compile(r"NLA-PLAY playing=(\d+)")
SAVE_RE = re.compile(
    r"NLA-SAVE path=(.*) ok=(\d+) entities=(\d+) err=(.*)$")
SCRIPT_RE = re.compile(r"NLA-SCRIPT applied=(\d+) failed=(\d+)")
# 运行时汇总（AnimatorSystem）
ANIM_NLA_RE = re.compile(
    r"ANIM-NLA configured=(\d+) strips=(\d+) applied=(\d+) skipped=(\d+) "
    r"problem=(.*)$")


def last(regex, text):
    hits = [m for m in (regex.search(l) for l in text.splitlines()) if m]
    return hits[-1] if hits else None


def all_of(regex, text):
    return [m for m in (regex.search(l) for l in text.splitlines()) if m]


def stat_of(text):
    m = last(STAT_RE, text)
    if not m:
        return None
    return {"tracks": int(m.group(1)), "strips": int(m.group(2)),
            "t": float(m.group(3)), "dur": float(m.group(4)),
            "selT": int(m.group(5)), "selS": int(m.group(6)),
            "playing": int(m.group(7)), "digest": m.group(8)}


def anim_nla(text):
    """最后一个 `ANIM-NLA` —— 只看**最后**一行：前面几帧是脚本还没跑之前的
    状态（`configured=0`），拿它当判据会得出"功能没生效"的错误结论。"""
    m = last(ANIM_NLA_RE, text)
    if not m:
        return None
    return {"configured": int(m.group(1)), "strips": int(m.group(2)),
            "applied": int(m.group(3)), "skipped": int(m.group(4)),
            "problem": m.group(5)}


# ------------------------------------------------------------------ A 自检
# `reportFlag()`：detail 在 result **之前**
SELFTEST_RE = re.compile(
    r"ANIM-TEST (\w+) actual=(\S+) expected=(\S+) tol=(\S+) detail=(.*) "
    r"result=(\w+)")
# ⚠ `report()`（数值型）**没有 detail 字段**。一个正则套两种会静默漏一半
#   —— S-C 与 S-D 两轮各栽过一次，这里一开始就拆开。
SELFTEST_NUM_RE = re.compile(
    r"ANIM-TEST (\w+) actual=(\S+) expected=(\S+) tol=(\S+) result=(\w+)")


def case_a(log=None):
    print("\n=== A 自检（nla_*）===")
    if log is None:
        log = run_editor({"MYVK_ANIM_TEST": "1"})
    passed = {}
    for line in log.splitlines():
        m = SELFTEST_RE.search(line)
        if m:
            passed[m.group(1)] = m.group(6)
            continue
        m = SELFTEST_NUM_RE.search(line)
        if m:
            passed[m.group(1)] = m.group(5)
    cur = {k: v for k, v in passed.items() if k.startswith("nla_")}
    check("A1 nla_* 共 42 条", len(cur) == 42, "n=%d" % len(cur))
    check("A2 全部 PASS", all(v == "PASS" for v in cur.values()),
          ",".join("%s=%s" % (k, v) for k, v in cur.items() if v != "PASS")
          or "ok")
    total = re.search(r"ANIM-TOTAL pass=(\d+) fail=(\d+)", log)
    check("A3 ANIM-TOTAL fail=0", bool(total) and total.group(2) == "0",
          total.group(0) if total else "no ANIM-TOTAL")
    # 点名五条底线：它们红了就等于"没有 NLA 时行为也变了"
    for name in ("nla_empty_bitwise", "nla_no_active_strip_bitwise",
                 "nla_replace_w1_bitwise", "nla_w0_bitwise",
                 "nla_order_top_wins"):
        check("A4 %s" % name, passed.get(name) == "PASS",
              passed.get(name, "missing"))
    return log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑某一组（a/b/...）")
    ap.add_argument("--dump", default="", help="把最后一个日志写到文件")
    args = ap.parse_args()
    only = args.only.strip().lower()

    def want(g):
        return (not only) or (g in only)

    make_probe()

    # ---------------- A ----------------
    if want("a"):
        case_a()

    # ---------------- B ----------------
    log_b = ""
    if want("b") or want("c") or want("d") or want("e") or want("f"):
        print("\n=== B 面板与目标 ===")
        log_b = run_editor({"MYVK_EDITOR_SCENE": PROBE,
                            "MYVK_EDITOR_NLA": "1",
                            "MYVK_EDITOR_NLA_SCRIPT": SCRIPT_BUILD})

        m = last(OPEN_RE, log_b)
        check("B1 NLA-OPEN 出现", bool(m), m.group(0) if m else "missing")
        tg = last(TARGET_RE, log_b)
        check("B2 目标解析到实体", bool(tg) and "#" in tg.group(2),
              tg.group(0) if tg else "missing")
        cv = last(CANVAS_RE, log_b)
        check("B3 画布宽度 > 0", bool(cv) and float(cv.group(5)) > 100.0,
              cv.group(0) if cv else "missing")
        sc = last(SCRIPT_RE, log_b)
        check("B4 注入脚本全成功",
              bool(sc) and int(sc.group(2)) == 0 and int(sc.group(1)) >= 9,
              sc.group(0) if sc else "missing")

        # ---------------- C ----------------
        print("\n=== C 结构编辑 ===")
        ta = last(TRK_ADD_RE, log_b)
        check("C1 加轨", bool(ta) and int(ta.group(1)) == 1,
              ta.group(0) if ta else "missing")
        adds = all_of(STRIP_ADD_RE, log_b)
        check("C2 加了两条 strip", len(adds) == 2, "n=%d" % len(adds))
        st = stat_of(log_b)
        check("C3 NLA-STAT strips=2", bool(st) and st["strips"] == 2,
              st and json.dumps(st))
        # 第二条 strip 的默认起点 = 第一条的末尾（"不重叠"是本面板的默认）
        if len(adds) == 2:
            s2 = float(adds[1].group(3))
            e1 = float(adds[0].group(4))
            check("C4 新 strip 接在末尾", abs(s2 - e1) < 1e-3,
                  "prev_end=%.3f next_start=%.3f" % (e1, s2))
        cv2 = last(CANVAS_RE, log_b)
        check("C5 画布上看得到两条", bool(cv2) and int(cv2.group(2)) == 2,
              cv2.group(0) if cv2 else "missing")

        # ---------------- D ----------------
        print("\n=== D 拖动与量化 ===")
        mv = last(MOVE_RE, log_b)
        d1 = False
        detail1 = "no move log"
        for m in all_of(MOVE_RE, log_b):
            if m.group(5) == "move":
                d1 = abs(float(m.group(3)) - 0.250) < 1e-6
                detail1 = "start=%s end=%s" % (m.group(3), m.group(4))
                break
        check("D1 move 起点 = 0.250", d1, detail1)
        # 两次 move/resize 的 why 都能看见（脚本用的是同一条 moveStrip 通路）
        whys = [m.group(5) for m in all_of(MOVE_RE, log_b)]
        check("D2 move 与 resize 走同一条路",
              ("move" in whys) and ("resize" in whys), ",".join(whys) or "none")
        # resize:0.1:0.9 之后起点必须正好是 0.100（量化后逐位相等）
        d3 = False
        detail3 = "no resize log"
        for m in all_of(MOVE_RE, log_b):
            if m.group(5) == "resize":
                d3 = abs(float(m.group(3)) - 0.100) < 1e-6
                detail3 = "start=%s" % m.group(3)
        check("D3 resize 起点 = 0.100", d3, detail3)
        # 摘要：每改一次结构都必须变（strips 顺序、时间、权重都吃进摘要）
        digs = [m.group(1) for m in all_of(DIGEST_RE, log_b)]
        check("D4 摘要每次改都变", len(digs) >= 2 and len(set(digs)) == len(digs),
              ",".join(digs) or "none")

        # ---------------- E ----------------
        print("\n=== E 运行时 ===")
        an = anim_nla(log_b)
        check("E1 运行时真的收到 NLA", bool(an) and an["configured"] >= 1,
              an and json.dumps(an))
        # 播放头停在 0（编辑器非 Play 不推进）→ [0.1,0.9] 那条在区间内、
        # [1,2] 那条在区间外。所以 applied=1、skipped=0。
        check("E2 区间内 1 条、区间外不算跳过",
              bool(an) and an["applied"] == 1 and an["skipped"] == 0,
              an and json.dumps(an))

        # ---------------- F ----------------
        print("\n=== F 静音 / 独占 ===")
        log_f = run_editor({"MYVK_EDITOR_SCENE": PROBE,
                            "MYVK_EDITOR_NLA": "1",
                            "MYVK_EDITOR_NLA_SCRIPT":
                                SCRIPT_BUILD +
                                ";select:0:0;trackmute:0:1;stat"})
        # 静音之后同一帧 `ANIM-NLA applied` 必须掉到 0
        # ⚠ 取**最后一行**：脚本跑之前那几帧仍是 1，用 any() 会永远成立
        anf = anim_nla(log_f)
        check("F1 静音轨 → applied=0", bool(anf) and anf["applied"] == 0,
              anf and json.dumps(anf))
        tm = last(TRK_EDIT_RE, log_f)
        check("F2 静音状态落到了轨上",
              bool(tm) and tm.group(2) == "muted" and tm.group(3) == "1",
              tm.group(0) if tm else "missing")

        # ---------------- G ----------------
        print("\n=== G 跨进程存读 ===")
        dig_b = (stat_of(log_b) or {}).get("digest")
        log_s = run_editor({"MYVK_EDITOR_SCENE": PROBE,
                            "MYVK_EDITOR_NLA": "1",
                            "MYVK_EDITOR_NLA_SCRIPT": SCRIPT_BUILD + ";save"})
        sv = last(SAVE_RE, log_s)
        check("G1 存盘成功", bool(sv) and sv.group(2) == "1",
              sv.group(0) if sv else "missing")
        with open(PROBE, "r", encoding="utf-8") as f:
            raw = f.read()
        check("G2 文件里出现 nlaTracks", '"nlaTracks"' in raw,
              "len=%d" % len(raw))
        check("G3 文件里出现 strip 的字段",
              '"strips"' in raw and '"clip"' in raw, "ok")
        # 第二个进程重新打开
        log_r = run_editor({"MYVK_EDITOR_SCENE": PROBE,
                            "MYVK_EDITOR_NLA": "1",
                            "MYVK_EDITOR_NLA_SCRIPT": "stat"})
        st_r = stat_of(log_r)
        check("G4 跨进程摘要一致",
              bool(st_r) and bool(dig_b) and st_r["digest"] == dig_b,
              "before=%s after=%s" % (dig_b, st_r and st_r["digest"]))
        check("G5 跨进程 strips 数一致",
              bool(st_r) and st_r["strips"] == 2,
              st_r and json.dumps(st_r))

    # ---------------- H ----------------
    if want("h"):
        print("\n=== H UI 锚点 ===")
        log_h = run_editor({"MYVK_EDITOR_SCENE": PROBE,
                            "MYVK_EDITOR_NLA": "1",
                            "MYVK_EDITOR_NLA_SCRIPT": SCRIPT_BUILD})
        need = ["NLA-OPEN", "NLA-TARGET", "NLA-TRK", "NLA-STRIP", "NLA-SEL",
                "NLA-STAT", "NLA-DIGEST", "NLA-CANVAS", "NLA-SCRIPT",
                "ANIM-NLA"]
        miss = [a for a in need if (a + " ") not in log_h]
        check("H1 %d 个锚点齐全" % len(need), not miss,
              "missing=" + ",".join(miss) if miss else "ok")
        check("H2 零 VUID",
              "VUID" not in log_h,
              "; ".join(l for l in log_h.splitlines() if "VUID" in l)[:400])
        errs = [l for l in log_h.splitlines()
                if "[ERROR]" in l or "VK_LOG_ERROR" in l]
        check("H3 零 [ERROR]", not errs, "; ".join(errs[:3]))
        tot = last(SCRIPT_RE, log_h)
        check("H4 脚本 failed=0", bool(tot) and int(tot.group(2)) == 0,
              tot.group(0) if tot else "missing")

    # ---------------- 汇总 ----------------
    if args.dump:
        with open(args.dump, "w", encoding="utf-8") as f:
            f.write(log_b or "")
        print("log → %s" % args.dump)

    ok = sum(1 for _, o, _ in results if o)
    bad = len(results) - ok
    print("\n结果: %d 通过 / %d 失败" % (ok, bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
