#!/usr/bin/env python3
"""端到端验证：骨骼曲线编辑器（S-D / #337）

==============================================================================
这一条在验什么
==============================================================================
S-D 的任务书只有一句话："把 `AnimTrack` 换成 `Curve`" —— 也就是让骨骼轨道
终于能画出来、能拖手柄、能设缓动。这件事有两个方向，本脚本两边都钉：

  **"什么都没变"**（比"变了"重要得多）
    · 没有缓动的轨道，采样必须与 `assets::sampleTrack` **逐位**相同；
    · 没改就写回，`changed` 必须是 0；
    · 曲线里少一帧时必须**整条拒绝**（曲线只改形状，不增删关键帧）。

  **"改了的那一处确实变了"**
    · 缓动让同一时刻的值变成手算得出的那个数（smoothstep）；
    · 插值从 linear 换到 bezier 时摘要变、而**帧数一个都不变**；
    · 存盘后文件里真的多了 `keyEase`，再读回来摘要一致。

==============================================================================
分组
==============================================================================
  A 自检 32 条 `curvebake_*`（纯数值、不开窗）+ `ANIM-TOTAL fail=0` +
    点名四条底线（默认逐位、旋转 Linear 逐位、写回 no-op、帧数不等拒绝）
  B 打开片段：tracks / 通道数 / 时长 / readOnly=0，且**第一帧**的
    `xcheck` 就有样本（"绿灯挂在没跑上"是这类读数最危险的形态）
  C 七个通道挨个切：rotation 的 `skipped=63`（运行时走 slerp，画布上没有
    那条线）、translation 的 `skipped=0` —— 这两条差值是 ⑤ 的可见证据
  D 缓动改值：t=0.125 处 0.125 → 0.078125（= smoothstep(0.25) × 0.5，
    手算得出的数），且**端点逐位不动**
  E 插值 T：linear → bezier → constant，摘要每次都变，`keys` 恒为 6
  F 手柄：`handle:0:vector` 存的是**类型**（不是换算后的数值），拖 out
    手柄后 x 变大 —— 与相机面板"V 换算并写回"正好相反（约定 ④）
  G 存盘往返：文件里出现 `keyEase` + `"interp": "bezier"`，跨进程摘要一致
  H 只读护栏：文件打不开时 Save 被拒（reason=read-only）
  I UI 锚点（15+）+ 零 VUID + 零 `[ERROR]` + 注入脚本 applied/failed

==============================================================================
两个刻意的取舍
==============================================================================
① **不用鼠标拖**。注入脚本走的是 `act*` + `beginDrag/actDragKeysPx/endDrag`
   那一族 —— 和鼠标拖动**同一条路**（像素→值只有一个换算函数）。走同一条路
   验的是真行为；"鼠标按不按得到那个点"由 `BONEC-CANVAS` / `BONEC-HANDLE-OUT`
   的矩形锚点证明（它们在，说明画布与手柄真的画在屏幕上）。
② **像素增量一律归一化到 `BONEC-MAP`**。浮动窗口尺寸会被记住，"拖 40px =
   多少秒"写死会在另一台机器上错。所以断言一律"先读 MAP，再算期望"。

用法：
    python tools/verify_bone_curves.py
    python tools/verify_bone_curves.py --only d
    python tools/verify_bone_curves.py --keep
    python tools/verify_bone_curves.py --dump a.log
"""

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")

sys.path.insert(0, HERE)
from verify_add_menu import LogTap                      # noqa: E402

FRAMES = 120

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ------------------------------------------------------------------ 夹具
#
# ⚠ 面板**不依赖骨架**（它编辑的是独立的 `EditClip`），所以这里手写一份
#   片段就够 —— 不需要开 Character 窗口、不需要模型。骨头名字用真实的
#   `upperarm.L`，是为了让日志读起来像人话。
CLIP = {
    "format": "venn.animclip",
    "version": 1,
    "name": "bcurvetest",
    "baseClip": "",
    "duration": 1.0,
    "loop": True,
    "tracks": [
        # rotation：z 分量从 0 → 30° → 60°，另三个分量恒定（画布上是直线）
        {"joint": "upperarm.L", "path": "rotation",
         "times": [0.0, 0.5, 1.0],
         "values": [[0.0, 0.0, 0.0, 1.0],
                    [0.0, 0.0, 0.258819, 0.9659258],
                    [0.0, 0.0, 0.5, 0.8660254]]},
        # translation：x 从 0 → 0.5 → 1.0（画布上是一条 45° 直线）
        {"joint": "upperarm.L", "path": "translation",
         "times": [0.0, 0.5, 1.0],
         "values": [[0.0, 0.0, 0.0], [0.5, 0.0, 0.0], [1.0, 0.0, 0.0]]},
    ],
}
# tracks 顺序 × 分量顺序 → 通道编号
#   0..3 = rotation.x/y/z/w   4..6 = translation.x/y/z
CHAN_TRANS_X = 4
EXPECT_TRACKS = 2
EXPECT_KEYS = 6
EXPECT_CHANS = 7
EXPECT_DUR = 1.0


def write_clip(path, doc=None):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc if doc is not None else CLIP, f, indent=2)


# ------------------------------------------------------------------ 启动
def run_editor(extra_env, wait_s=120.0):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"     # 不设它 ui::logRect 直接 return
    env["MYVK_GRID"] = "0"
    env["MYVK_FRAMES"] = str(FRAMES)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    for k, v in extra_env.items():
        if v is not None:
            env[k] = v

    # 上一轮崩掉的实例会抢窗口标题 → 先清一遍（同 verify_timeline_editor）
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
OPEN_RE = re.compile(
    r"BONEC-OPEN path=(\S+) ok=(\d+) tracks=(\d+) keys=(\d+) dur=([\d.]+) "
    r"chans=(\d+) readOnly=(\d+)")
VIEW_RE = re.compile(r"BONEC-VIEW t=\[([-\d.]+),([-\d.]+)\] v=\[([-\d.]+),([-\d.]+)\] ch=(\S+)")
CHECK_RE = re.compile(
    r"BONEC-CHECK ok=(\d+) tracks=(\d+) keys=(\d+) dur=([\d.]+) ch=(\S*) "
    r"xcheck=([\d.eE+-]+) samples=(\d+) skipped=(\d+) err=(.*)$")
MAP_RE = re.compile(
    r"BONEC-MAP t0=([-\d.]+) t1=([-\d.]+) v0=([-\d.]+) v1=([-\d.]+) "
    r"pxmin=([-\d.]+),([-\d.]+) pxmax=([-\d.]+),([-\d.]+)")
SAMPLE_RE = re.compile(r"BONEC-SAMPLE t=([-\d.]+) v=([-\d.]+) ch=(\S+)")
DIGEST_RE = re.compile(r"BONEC-DIGEST ch=(\S+) n=(\d+) d=(\d+)")
CHAN_RE = re.compile(r"BONEC-CHANNEL i=(\d+) id=(\S+) keys=(\d+)")
INTERP_RE = re.compile(r"BONEC-INTERP i=(\d+) interp=(\w+) ch=(\S+)")
EASE_RE = re.compile(r"BONEC-EASE i=(\d+) mode=(\w+) shape=(\w+) ch=(\S+)")
HT_RE = re.compile(r"BONEC-HANDLETYPE i=(\d+) type=(\w+) ch=(\S+)")
DRAGH_RE = re.compile(r"BONEC-DRAG-END kind=handle i=(\d+) side=(\w+) x=([-\d.]+) y=([-\d.]+)")
SAVE_RE = re.compile(r"BONEC-SAVE path=(\S+) tracks=(\d+) keys=(\d+) force=(\d)")
BLOCK_RE = re.compile(r"BONEC-SAVE-BLOCKED path=(\S+) reason=([\w-]+)")
SCRIPT_RE = re.compile(
    r"BONEC-SCRIPT applied=(\d+) failed=(\d+) chan=(\d+) id=(\S+) tracks=(\d+) "
    r"keys=(\d+) sel=(\d+) dirty=(\d+)")


def last(regex, text):
    hits = [m for m in (regex.search(l) for l in text.splitlines()) if m]
    return hits[-1] if hits else None


def all_of(regex, text):
    return [m for m in (regex.search(l) for l in text.splitlines()) if m]


def map_of(text):
    m = last(MAP_RE, text)
    if not m:
        return None
    g = [float(m.group(i)) for i in range(1, 9)]
    return {"t0": g[0], "t1": g[1], "v0": g[2], "v1": g[3],
            "pxmin": (g[4], g[5]), "pxmax": (g[6], g[7])}


# ------------------------------------------------------------------ A 自检
# `reportFlag()`：detail 在 result **之前**
SELFTEST_RE = re.compile(
    r"ANIM-TEST (\w+) actual=(\S+) expected=(\S+) tol=(\S+) detail=(.*) result=(\w+)")
# ⚠ `report()`（数值型断言）**没有 detail 字段** —— 只有 `reportFlag()` 有。
#   用一个正则套两种会静默漏掉一半（S-C 那轮就这么栽过一次；这一轮又栽了
#   一次，症状是 32 条只数到 6 条 —— 数到的正好全是 report() 那几条）。
SELFTEST_NUM_RE = re.compile(
    r"ANIM-TEST (\w+) actual=(\S+) expected=(\S+) tol=(\S+) result=(\w+)")


def case_a(log=None):
    print("\n=== A 自检（curvebake_*）===")
    if log is None:
        log = run_editor({"MYVK_ANIM_TEST": "1"})
    passed = {}
    for line in log.splitlines():
        m = SELFTEST_RE.search(line)
        if m:
            passed[m.group(1)] = m.group(6)      # detail 在前，result 在第 6 组
            continue
        m = SELFTEST_NUM_RE.search(line)
        if m:
            passed[m.group(1)] = m.group(5)
    cur = {k: v for k, v in passed.items() if k.startswith("curvebake_")}
    check("A1 curvebake_* 共 32 条", len(cur) == 32,
          "n=%d" % len(cur))
    check("A2 全部 PASS", all(v == "PASS" for v in cur.values()),
          ",".join("%s=%s" % (k, v) for k, v in cur.items() if v != "PASS") or "ok")
    total = re.search(r"ANIM-TOTAL pass=(\d+) fail=(\d+)", log)
    check("A3 ANIM-TOTAL fail=0", bool(total) and total.group(2) == "0",
          total.group(0) if total else "没打到 ANIM-TOTAL")

    # 点名四条底线（少了它们，"没有回归"这句话就不成立）
    must = ["curvebake_default_bitwise", "curvebake_rotation_linear_bitwise",
            "curvebake_writeback_identical", "curvebake_writeback_refuse",
            "curvebake_roundtrip_digest", "curvebake_json_roundtrip"]
    miss = [k for k in must if passed.get(k) != "PASS"]
    check("A4 点名 6 条底线", not miss, "缺/红: %s" % (miss or "无"))
    return log


# ------------------------------------------------------------------ B 打开
def case_b(tmp):
    print("\n=== B 打开片段 ===")
    path = os.path.join(BIN, "_verify_bonec.anim.json")
    write_clip(path)
    log = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec.anim.json"})
    m = last(OPEN_RE, log)
    ok = (m and m.group(2) == "1" and int(m.group(3)) == EXPECT_TRACKS
          and int(m.group(4)) == EXPECT_KEYS and int(m.group(6)) == EXPECT_CHANS
          and m.group(7) == "0"
          and abs(float(m.group(5)) - EXPECT_DUR) < 1e-6)
    check("B1 打开（tracks=2 keys=6 chans=7 readOnly=0）", bool(ok), m.group(0) if m else "没打到 BONEC-OPEN")

    v = last(VIEW_RE, log)
    check("B2 视图铺满时间轴（0..1s）",
          bool(v) and abs(float(v.group(1))) < 1e-6 and abs(float(v.group(2)) - 1.0) < 1e-6,
          v.group(0) if v else "没打到 BONEC-VIEW")

    # ⚠ 这一条盯的是"绿灯挂在没跑上"：`open()` 曾经在 `rebuildCurve()` 之前
    #   就调 revalidate，于是第一帧 xcheck=0 而 samples=0 —— 看着像满分，
    #   实际一个样本都没比。
    c = last(CHECK_RE, log)
    check("B3 首帧 CHECK 真有样本（samples>0）",
          bool(c) and int(c.group(7)) > 0,
          c.group(0) if c else "没打到 BONEC-CHECK")
    return log


# ------------------------------------------------------------------ C 通道
def case_c():
    print("\n=== C 七个通道挨个切 ===")
    path = os.path.join(BIN, "_verify_bonec.anim.json")
    write_clip(path)
    # ⚠ `chan:0` 在"当前就是 0"时是幂等操作，**不打日志**（`actSetChannel`
    #   开头 `if (index == m_chan) return true`）。所以先把选中挪到最后一格，
    #   再回头挨个走一遍 —— 否则第一格永远数不到。
    order = [EXPECT_CHANS - 1] + list(range(EXPECT_CHANS))
    script = ";".join("chan:%d" % i for i in order) + ";check"
    log = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec.anim.json",
                      "MYVK_EDITOR_BONEC_SCRIPT": script})
    chans = all_of(CHAN_RE, log)
    ids = [m.group(2) for m in chans]
    # 脚本走 8 步（先把选中挪到末格，再 0..6 一遍），但**只有 7 个通道**。
    check("C1 七个通道都能选中",
          len(ids) == len(order) and len(set(ids)) == EXPECT_CHANS,
          "steps=%d uniq=%d ids=%s" % (len(ids), len(set(ids)), ids))

    # rotation（chan 0..3）：运行时那一段走 slerp → 63 个样本不可比
    # translation（chan 4..6）：全程走曲线 → 0 个跳过
    rot = [m for m in all_of(CHECK_RE, log) if "/rotation." in m.group(5)]
    tra = [m for m in all_of(CHECK_RE, log) if "/translation." in m.group(5)]
    rot_sk = [int(m.group(8)) for m in rot]
    tra_sk = [int(m.group(8)) for m in tra]
    check("C2 rotation 段被跳过（slerp）", bool(rot_sk) and all(s == 63 for s in rot_sk),
          "skipped=%s" % (rot_sk or "无"))
    check("C3 translation 段一个都没跳过", bool(tra_sk) and all(s == 0 for s in tra_sk),
          "skipped=%s" % (tra_sk or "无"))
    # 能比的那些必须**恰好相等**（不是"差得小"）
    worst = [float(m.group(6)) for m in all_of(CHECK_RE, log)]
    check("C4 画布 ≡ 运行时（xcheck 全 0）", bool(worst) and all(w == 0.0 for w in worst),
          "worst=%s" % (worst or "无"))
    return log


# ------------------------------------------------------------------ D 缓动
def case_d():
    print("\n=== D 缓动改值（手算）===")
    path = os.path.join(BIN, "_verify_bonec.anim.json")
    write_clip(path)
    # t=0.125 落在第一段（0 → 0.5）的 u=0.25 处；translation.x 的段值是 0 → 0.5
    #   线性：0.5 × 0.25      = 0.125
    #   smoothstep(0.25)      = 0.25² (3 − 2×0.25) = 0.15625
    #   缓动后：0.5 × 0.15625 = 0.078125
    script = ("chan:%d;head:0.125;ease:0:inout;shape:0:smooth;head:0.125;"
              "head:0;head:1;digest" % CHAN_TRANS_X)
    log = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec.anim.json",
                      "MYVK_EDITOR_BONEC_SCRIPT": script})
    smp = all_of(SAMPLE_RE, log)
    vals = [float(m.group(2)) for m in smp]
    check("D1 采样共 4 次（缓动前后 + 两个端点）", len(vals) == 4,
          "n=%d v=%s" % (len(vals), vals))
    if len(vals) == 4:
        check("D2 缓动前 = 手算线性值 0.125", abs(vals[0] - 0.125) < 1e-6, "v=%r" % vals[0])
        want = 0.5 * 0.15625
        check("D3 缓动后 = 手算 smoothstep 0.078125", abs(vals[1] - want) < 1e-6,
              "v=%r want=%r" % (vals[1], want))
        # ⚠ 端点值来自**最后一个关键帧**（translation.x 是 0 → 0.5 → 1.0），
        #   所以 t=1.0 处是 1.0。第一版写成 0.5，是把"第二帧"当成了末帧。
        check("D4 端点逐位不动（0 与 1.0）",
              vals[2] == 0.0 and abs(vals[3] - 1.0) < 1e-7,
              "t0=%r t1=%r" % (vals[2], vals[3]))

    e = last(EASE_RE, log)
    check("D5 日志记了 mode/shape", bool(e) and e.group(2) == "inout" and e.group(3) == "smooth",
          e.group(0) if e else "没打到 BONEC-EASE")
    return log


# ------------------------------------------------------------------ E 插值
def case_e():
    print("\n=== E 插值 T（改形状，不改帧数）===")
    path = os.path.join(BIN, "_verify_bonec.anim.json")
    write_clip(path)
    script = ("chan:%d;digest;interp:0:bezier;digest;interp:0:constant;digest;"
              "interp:0:linear;digest" % CHAN_TRANS_X)
    log = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec.anim.json",
                      "MYVK_EDITOR_BONEC_SCRIPT": script})
    dg = [m.group(3) for m in all_of(DIGEST_RE, log)]
    ns = [int(m.group(2)) for m in all_of(DIGEST_RE, log)]
    check("E1 四次摘要", len(dg) == 4, "n=%d" % len(dg))
    check("E2 三种插值摘要两两不同", len(set(dg[:3])) == 3, "d=%s" % (dg or "无"))
    check("E3 回到 linear 摘要复原（可逆）", len(dg) == 4 and dg[0] == dg[3],
          "%s vs %s" % (dg[0] if dg else "?", dg[3] if dg else "?"))
    check("E4 帧数恒为 3（曲线不增删帧）", bool(ns) and all(n == 3 for n in ns),
          "n=%s" % (ns or "无"))
    ip = all_of(INTERP_RE, log)
    check("E5 日志记了 interp 名", len(ip) == 3 and
          [m.group(2) for m in ip] == ["bezier", "constant", "linear"],
          [m.group(2) for m in ip])

    s = last(SCRIPT_RE, log)
    check("E6 注入脚本零失败", bool(s) and s.group(2) == "0",
          s.group(0) if s else "没打到 BONEC-SCRIPT")
    return log


# ------------------------------------------------------------------ F 手柄
def case_f():
    print("\n=== F 手柄（存类型，不换算）===")
    path = os.path.join(BIN, "_verify_bonec.anim.json")
    write_clip(path)
    # vector 手柄：out 指向下一帧 → 段退化成直线。**类型要留在文件里**，
    # 不是"按规则算一遍数值然后写成 Free"（那正是相机面板的做法 —— 约定 ④）。
    script = "chan:%d;sel:0;interp:0:bezier;handle:0:vector;save" % CHAN_TRANS_X
    log = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec.anim.json",
                      "MYVK_EDITOR_BONEC_SCRIPT": script})
    ht = last(HT_RE, log)
    check("F1 handle 记为 vector", bool(ht) and ht.group(2) == "vector",
          ht.group(0) if ht else "没打到 BONEC-HANDLETYPE")

    doc = json.load(open(path, encoding="utf-8"))
    tr = doc["tracks"][1]
    eases = tr.get("keyEase") or []
    have = [e for e in eases if e.get("i") == 0]
    check("F2 文件里存下了 handle=vector", bool(have) and have[0].get("handle") == "vector",
          json.dumps(have[0] if have else {}, ensure_ascii=False))
    check("F3 文件里存下了 interp=bezier", bool(have) and have[0].get("interp") == "bezier",
          json.dumps(have[0] if have else {}, ensure_ascii=False))

    # 拖 out 手柄：像素增量归一化到 BONEC-MAP
    write_clip(path)
    script2 = "chan:%d;sel:0;interp:0:bezier;drag:out:40:0;save" % CHAN_TRANS_X
    log2 = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec.anim.json",
                       "MYVK_EDITOR_BONEC_SCRIPT": script2})
    mp = map_of(log2)
    dh = last(DRAGH_RE, log2)
    check("F4 拖 out 手柄有落点日志", bool(dh) and dh.group(2) == "out",
          dh.group(0) if dh else "没打到 BONEC-DRAG-END kind=handle")
    doc2 = json.load(open(path, encoding="utf-8"))
    e2 = [e for e in (doc2["tracks"][1].get("keyEase") or []) if e.get("i") == 0]
    if mp and e2:
        want = 40.0 / max((mp["pxmax"][0] - mp["pxmin"][0]) / (mp["t1"] - mp["t0"]), 1e-6)
        got = float(e2[0]["out"][0])
        # 起点是 1/3（默认手柄），拖 40px 之后应明显更大
        check("F5 手柄 x 按 MAP 换算（> 1/3）", got > 1.0 / 3.0 + 1e-3,
              "out.x=%.4f (+%.4f s)" % (got, got - 1.0 / 3.0))
        check("F6 拖过的手柄变成 free（Auto 不认你拖的值）",
              e2[0].get("handle") == "free", "handle=%s" % e2[0].get("handle"))
        check("F7 MAP 与拖动换算自洽（>0）", want > 0.0, "pxPerSec 推得 %.4f" % want)
    else:
        check("F5 手柄 x 按 MAP 换算", False, "缺 MAP 或 keyEase")
        check("F6 拖过的手柄变成 free", False, "缺 MAP 或 keyEase")
        check("F7 MAP 与拖动换算自洽", False, "缺 MAP 或 keyEase")
    return log2


# ------------------------------------------------------------------ G 存读
def case_g():
    print("\n=== G 存盘往返（跨进程摘要）===")
    path = os.path.join(BIN, "_verify_bonec.anim.json")
    write_clip(path)
    script = ("chan:%d;sel:1;interp:1:bezier;ease:1:inout;shape:1:sine;"
              "head:0.75;digest;save" % CHAN_TRANS_X)
    log1 = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec.anim.json",
                       "MYVK_EDITOR_BONEC_SCRIPT": script})
    s1 = last(SAVE_RE, log1)
    check("G1 存盘成功（keys=6）", bool(s1) and s1.group(3) == str(EXPECT_KEYS),
          s1.group(0) if s1 else "没打到 BONEC-SAVE")
    d1 = last(DIGEST_RE, log1)
    check("G2 第一次运行有摘要", bool(d1), d1.group(0) if d1 else "无")

    # ⚠ 第二次运行**不注入改数据的脚本**：只读盘 + 打摘要。两个进程的摘要
    #   必须逐位相同 —— 这是"存下去的东西完整到能重建求值"的唯一证据。
    log2 = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec.anim.json",
                       "MYVK_EDITOR_BONEC_SCRIPT": "chan:%d;head:0.75;digest;check"
                       % CHAN_TRANS_X})
    d2 = last(DIGEST_RE, log2)
    check("G3 跨进程摘要一致", bool(d1) and bool(d2) and d1.group(3) == d2.group(3),
          "%s vs %s" % (d1.group(3) if d1 else "?", d2.group(3) if d2 else "?"))
    c2 = last(CHECK_RE, log2)
    check("G4 读回来的轨道画布 ≡ 运行时", bool(c2) and float(c2.group(6)) == 0.0,
          c2.group(0) if c2 else "没打到 BONEC-CHECK")

    doc = json.load(open(path, encoding="utf-8"))
    keys = [e for e in (doc["tracks"][1].get("keyEase") or []) if e.get("i") == 1]
    check("G5 ease / shape 落到文件里",
          bool(keys) and keys[0].get("easeMode") == "inout" and keys[0].get("shape") == "sine",
          json.dumps(keys[0] if keys else {}, ensure_ascii=False))
    # 没动过的那条旋转轨**一个 keyEase 字段都不该有**（稀疏写）
    check("G6 没编辑的轨道不写 keyEase", "keyEase" not in doc["tracks"][0],
          "tracks[0] keys=%s" % list(doc["tracks"][0].keys()))
    return log1


# ------------------------------------------------------------------ H 只读
def case_h():
    print("\n=== H 只读护栏 ===")
    bad = os.path.join(BIN, "_verify_bonec_bad.anim.json")
    with open(bad, "w", encoding="utf-8") as f:
        f.write('{"format":"not-a-clip","version":1,"tracks":[]}')
    log = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec_bad.anim.json",
                      "MYVK_EDITOR_BONEC_SCRIPT": "save"})
    m = last(OPEN_RE, log)
    check("H1 坏文件 → readOnly=1", bool(m) and m.group(7) == "1",
          m.group(0) if m else "没打到 BONEC-OPEN")
    b = last(BLOCK_RE, log)
    check("H2 Save 被拒（reason=read-only）", bool(b) and b.group(2) == "read-only",
          b.group(0) if b else "没打到 BONEC-SAVE-BLOCKED")
    s = last(SAVE_RE, log)
    check("H3 没有写出任何东西", s is None, s.group(0) if s else "ok（无 SAVE）")
    return log


# ------------------------------------------------------------------ I UI
ANCHORS = [
    "BONEC-WIN", "BONEC-PATH", "BONEC-SAVE", "BONEC-CHANNEL", "BONEC-HEAD",
    "BONEC-FIT", "BONEC-CANVAS", "BONEC-MAP", "BONEC-CURSOR",
    "BONEC-INSP-T", "BONEC-INSP-V", "BONEC-INSP-INTERP", "BONEC-INSP-EASE",
    "BONEC-INSP-SHAPE", "BONEC-INSP-HANDLE", "BONEC-INSP-OUT", "BONEC-INSP-IN",
    "BONEC-HANDLE-OUT", "BONEC-HANDLE-IN", "BONEC-CHECK",
]


def case_i(log=None):
    print("\n=== I UI 锚点 / 干净日志 ===")
    path = os.path.join(BIN, "_verify_bonec.anim.json")
    write_clip(path)
    if log is None:
        log = run_editor({"MYVK_EDITOR_BONEC": "_verify_bonec.anim.json",
                          "MYVK_EDITOR_BONEC_SCRIPT":
                              "chan:%d;sel:0;interp:0:bezier" % CHAN_TRANS_X})
    # ⚠ `logRect` 的锚点长成 `BONEC-WIN=(x,y)-(x,y)`，而我自己打的那些是
    #   `BONEC-MAP t0=...`（空格分隔）。只查 `NAME=` 会漏掉后一类。
    miss = [a for a in ANCHORS
            if (a + "=") not in log and (a + " ") not in log]
    check("I1 %d 个锚点齐全" % len(ANCHORS), not miss, "缺: %s" % (miss or "无"))

    vuid = [l for l in log.splitlines() if "VUID" in l]
    check("I2 零 VUID", not vuid, vuid[0] if vuid else "ok")
    errs = [l for l in log.splitlines() if "[ERROR]" in l]
    check("I3 零 [ERROR]", not errs, errs[0] if errs else "ok")

    s = last(SCRIPT_RE, log)
    check("I4 注入脚本 applied>0 / failed=0",
          bool(s) and int(s.group(1)) > 0 and s.group(2) == "0",
          s.group(0) if s else "没打到 BONEC-SCRIPT")
    check("I5 改完之后 dirty=1", bool(s) and s.group(8) == "1",
          s.group(0) if s else "无")
    return log


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑某一组（a/b/c/...）")
    ap.add_argument("--keep", action="store_true", help="留下临时文件")
    ap.add_argument("--dump", default="", help="把 A 组日志写到这个文件")
    args = ap.parse_args()

    if not os.path.exists(EDITOR_EXE):
        print("找不到 %s —— 先 build" % EDITOR_EXE)
        return 2

    print("=" * 78)
    print("骨骼曲线编辑器（S-D）端到端验证")
    print("=" * 78)

    only = args.only.lower()
    a_log = case_a() if not only or only == "a" else None
    if a_log and args.dump:
        with open(args.dump, "w", encoding="utf-8") as f:
            f.write(a_log)
        print("  （A 组日志 → %s）" % args.dump)

    if not only or only == "b":
        case_b(None)
    if not only or only == "c":
        case_c()
    if not only or only == "d":
        case_d()
    if not only or only == "e":
        case_e()
    if not only or only == "f":
        case_f()
    if not only or only == "g":
        case_g()
    if not only or only == "h":
        case_h()
    if not only or only == "i":
        case_i()

    if not args.keep:
        for name in ("_verify_bonec.anim.json", "_verify_bonec_bad.anim.json"):
            p = os.path.join(BIN, name)
            if os.path.exists(p):
                try:
                    os.remove(p)
                except OSError:
                    pass

    npass = sum(1 for _, ok, _ in results if ok)
    nfail = len(results) - npass
    print()
    print("=" * 78)
    if nfail == 0:
        print("结果: PASS（%d 项）" % len(results))
    else:
        print("结果: FAIL（%d/%d 项失败）" % (nfail, len(results)))
        for name, ok, detail in results:
            if not ok:
                print("   ✗ %s  %s" % (name, detail))
    print("=" * 78)
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
