#!/usr/bin/env python3
"""端到端验证：动画摄影表（Dope Sheet，R8 / #320）

==============================================================================
这一条在验什么
==============================================================================
R6 把"某一条通道的值怎么变"画成了曲线，但作者读节奏时看的是**另一件事**：
哪一帧有键、这些键分布得匀不匀。曲线图上十根竖线挤在一起是看不出来的。
本脚本钉的是"这张表真的能读、能批量改，而且改完的数字对得上":

  A 打开：通道树画出 3 根骨 + 3 条通道行（6 行）/ 15 帧 / 3.0 秒；
    **折叠**一根骨后摘要行接管它的帧（行数 6→5，`selrow` 仍能选中那 10 帧）
    —— 摘要行铺点只在折叠时发生，"展开了还叠一层"就是同一个帧画两遍
  B 框选 10 帧一起挪 0.25 秒 → **时间戳整体偏移且相对间距逐帧逐位不变**
    （验收第 1 条）；没被框到的 5 帧一个字节都没动；存盘也是这个数
  C 镜像 → 每帧落到 `2·pivot − t` 上（逐位），**间距序列正好反序**
    （= 前后半段对称）；镜像两次回到原样，落盘的 hips 轨道与原始逐位相同
    （验收第 2 条）
  D 关键帧类型：设成 extreme 之后只有 `types` 变了，`times` / `values`
    **逐位不动** —— 所以求值结果不可能变（求解器层面的等价由 C++ 自检
    `dope_types_do_not_change_sampling` 钉，这里钉的是"落盘不改数据"）
  E 缩放 / 平移：`zoom` 绕播放头、跨度减半而**锚点的相对位置不变**；
    `pan` 只挪 t0 不改跨度；每颗菱形的 px 与 `px0 + (t−t0)·pps` 一致
  F UI 锚点齐全（窗口 / 画布 / 工具栏按钮）+ 全程零 VUID、零 `[ERROR]`
  G 对齐循环长度：首帧是相位锚点、**逐位**不动；末帧精确落在 1.5（不是
    1.4999999，否则循环点每转一圈差一丁点）；中间九帧的相位差 < 1e-6

==============================================================================
四个刻意的取舍
==============================================================================
① **夹具是脚本现造的**，不是仓库里的资产。`engine/assets/**` 在回归的
   `GLOBAL_GLOBS` 里（动一下要重跑半个回归），而且"能被框选成恰好 10 帧"
   这种要求本身就很刻意 —— 现造一份、测完就扔，不给资产库添垃圾。

② **时间戳全是 2⁻⁴ 的整数倍（0.0625 的倍数）+ 位移取 0.25**，于是
   `(t+d)−t` 与 `d` 在 float32 上**逐位相等**，`==` 才敢直接用。
   真实的 1/30 秒时刻做不到这一点（见 C++ 侧 `dope_move_shift_bitwise`
   那段注释），用容差又会把"累加实现"这种真 bug 放过去。

③ **不用鼠标拖**。批量操作走 `act*` 一族（与鼠标拖动同一条实现），
   "鼠标能不能按到那颗菱形"由 `DOPE-CANVAS` / `DOPE-KEY px` 的推导一致证明。

④ **px 的判据用同一批 `DOPE-DUMP` 头里的 t0/pps**，不跨批猜。
   脚本里的 `zoom` / `pan` 会立刻改视图，但本帧的菱形还画在上一帧的换算率
   上 —— 跨批配错的表现是"px 差几十像素"这种假失败。

用法：
    python tools/verify_dope_sheet.py
    python tools/verify_dope_sheet.py --only b     # 只跑某个用例
    python tools/verify_dope_sheet.py --keep       # 留下临时目录
    python tools/verify_dope_sheet.py --dump x.log # 把 A 用例完整日志写出
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")

sys.path.insert(0, HERE)
from verify_add_menu import LogTap                      # noqa: E402

# ---------------------------------------------------------------- 夹具
# 三根骨：hips 10 帧（要被框选 + 挪 + 镜像的那一组）、spine 3 帧、head 2 帧。
# spine / head 刻意放在 2.0 秒之后：`selrect:0:1.125` 就**只会**圈到 hips，
# "没被选中一个字节都没动"这条才有意义。
HIPS_T = [0.0, 0.125, 0.25, 0.375, 0.4375, 0.5, 0.75, 0.875, 1.0, 1.125]
SPINE_T = [2.0, 2.5, 3.0]
HEAD_T = [2.0, 3.0]
EXPECT_KEYS = len(HIPS_T) + len(SPINE_T) + len(HEAD_T)      # 15
EXPECT_DUR = 3.0
# hips 的时间范围：`selrect:0:1.125`、`mirror:0.5625`（= 这段的中心）
HIPS_LO, HIPS_HI = HIPS_T[0], HIPS_T[-1]
HIPS_MID = (HIPS_LO + HIPS_HI) * 0.5                        # 0.5625

# 让编辑器自己退出（不认这个变量就得 kill，而"日志截到哪一帧"随机器浮动）
FRAMES = 150

BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def f32(x):
    """最近的 float32。判据要的是"逐位相等"，不是"差得少"。"""
    return struct.unpack("f", struct.pack("f", x))[0]


def quat(i):
    """一个可区分的四元数。**值本身不参与任何摄影表判据**，只用来验
    "没被顺手改掉"。分量取 2⁻³ 的整数倍 → JSON 往返也是逐位精确的。"""
    return [0.0, 0.0, f32(i * 0.125), 1.0]


def make_fixture(path, name="dope_fixture"):
    tracks = [
        {"joint": "hips", "path": "rotation",
         "times": HIPS_T, "values": [quat(i) for i in range(len(HIPS_T))]},
        {"joint": "spine", "path": "rotation",
         "times": SPINE_T, "values": [quat(i) for i in range(len(SPINE_T))]},
        {"joint": "head", "path": "rotation",
         "times": HEAD_T, "values": [quat(i) for i in range(len(HEAD_T))]},
    ]
    doc = {"format": "venn.animclip", "version": 1, "name": name,
           "baseClip": "", "duration": EXPECT_DUR, "loop": True,
           "tracks": tracks}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
    return doc


# ------------------------------------------------------------------ 启动
def run_editor(extra_env, wait_s=180.0):
    """起一程编辑器，返回完整日志。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"        # 不设它 ui::logRect 直接 return
    env["MYVK_GRID"] = "0"
    env["MYVK_FRAMES"] = str(FRAMES)
    env["MYVK_NO_WINDOW_SAVE"] = "1"   # 自动化期间不写 imgui.ini / 布局
    for k, v in extra_env.items():
        if v is not None:
            env[k] = v

    # 上一轮崩掉的实例会抢窗口标题 → 先清一遍（与 verify_curve_editor 同纪律）
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


def join(p):
    """给注入脚本 / 环境变量用的路径：统一正斜杠（`DOPE-OPEN path=\\S+`
    在带反斜杠的 JSON 转义里会读成两截）。"""
    return os.path.abspath(p).replace("\\", "/")


# ------------------------------------------------------------------ 日志解析
OPEN_RE = re.compile(
    r"DOPE-OPEN path=(\S+) ok=1 tracks=(\d+) keys=(\d+) dur=([\d.]+)")
SEL_RE = re.compile(r"DOPE-SEL n=(\d+)(?: t=\[([-\d.]+),([-\d.]+)\])?")
MOVE_RE = re.compile(
    r"DOPE-MOVE n=(\d+) delta=([-\d.]+) merged=(\d+) dropped=(\d+) "
    r"t=\[([-\d.]+),([-\d.]+)\]")
MIRROR_RE = re.compile(
    r"DOPE-MIRROR n=(\d+) pivot=([-\d.]+) merged=(\d+) t=\[([-\d.]+),([-\d.]+)\]")
ALIGN_RE = re.compile(
    r"DOPE-ALIGN n=(\d+) target=([-\d.]+) scale=([-\d.]+) first=([-\d.]+) "
    r"last=([-\d.]+)")
TYPE_RE = re.compile(r"DOPE-TYPE n=(\d+) type=(\w+)")
VIEW_RE = re.compile(
    r"DOPE-VIEW t0=([-\d.]+) t1=([-\d.]+) px0=([-\d.]+) px1=([-\d.]+) "
    r"pps=([-\d.]+) rowH=([-\d.]+) rows=(\d+) scroll=([-\d.]+) treeW=([-\d.]+)")
SUMMARY_RE = re.compile(r"DOPE-SUMMARY rows=(\d+) keys=(\d+) sel=(\d+) dur=([\d.]+)")
DUMP_RE = re.compile(
    r"DOPE-DUMP n=(\d+) px0=([-\d.]+) px1=([-\d.]+) t0=([-\d.]+) "
    r"t1=([-\d.]+) pps=([-\d.]+)")
KEY_RE = re.compile(
    r"DOPE-KEY track=(\d+) joint='([^']*)' path=(\w+) i=(\d+) t=(\S+) "
    r"type=(\w+) sel=(\d) px=([-\d.]+)")
ZOOM_RE = re.compile(r"DOPE-ZOOM factor=([-\d.]+) t0=([-\d.]+) t1=([-\d.]+)")
PAN_RE = re.compile(r"DOPE-PAN dt=([-\d.]+) t0=([-\d.]+) t1=([-\d.]+)")
SAVE_RE = re.compile(r"DOPE-SAVE path=(\S+) tracks=(\d+) keys=(\d+)")
COLLAPSE_RE = re.compile(r"DOPE-COLLAPSE joint='([^']*)' collapsed=(\d)")
SCRIPT_RE = re.compile(r"DOPE-SCRIPT applied=(\d+) failed=(\d+) keys=(\d+) sel=(\d+)")

def last(regex, text):
    hits = [m for m in (regex.search(l) for l in text.splitlines()) if m]
    return hits[-1] if hits else None


def all_of(regex, text):
    return [m for m in (regex.search(l) for l in text.splitlines()) if m]


def groups(m):
    return m.groups() if m else "none"


def rects(text, tag):
    """`TAG=(x0,y0)-(x1,y1) center=(cx,cy)` → [(x0,y0,x1,y1)]。

    ⚠ 用 `search` 不用 `match`：日志行前面还有时间戳/级别前缀。tag 也按
    整词匹配，否则 `DOPE-TYPE` 会被 `DOPE-TYPE-XXX` 之类吃掉。
    """
    rx = re.compile(r"\b" + re.escape(tag) +
                    r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)")
    out = []
    for line in text.splitlines():
        m = rx.search(line)
        if m:
            out.append(tuple(float(m.group(i)) for i in range(1, 5)))
    return out


def parse_t(s):
    """`%a`（C 的 hex float）→ float。`0x1p-3`、`0x1.8p+0` 都认。"""
    return float.fromhex(s) if "0x" in s or "0X" in s.lower() else float(s)


def batches(text):
    """按 `DOPE-DUMP` 切批 → [{"h": 头, "keys": [...]}]。

    ⚠ 必须按**行序**切，不能对全部 DOPE-KEY 取 last：一次 `move` 前后两次
    `dumpkeys` 才是"变没变"的证据，混在一起就只剩"最后是什么"。
    """
    out = []
    cur = None
    for line in text.splitlines():
        m = DUMP_RE.search(line)
        if m:
            cur = {"h": {"n": int(m.group(1)), "px0": float(m.group(2)),
                         "px1": float(m.group(3)), "t0": float(m.group(4)),
                         "t1": float(m.group(5)), "pps": float(m.group(6))},
                   "keys": []}
            out.append(cur)
            continue
        k = KEY_RE.search(line)
        if k and cur is not None:
            cur["keys"].append({
                "track": int(k.group(1)), "joint": k.group(2), "path": k.group(3),
                "i": int(k.group(4)), "t": parse_t(k.group(5)),
                "type": k.group(6), "sel": int(k.group(7)),
                "px": float(k.group(8)),
            })
    return out


def by_track(keys, joint):
    """某根骨的帧，按 (track, i) 的原始顺序。"""
    return [k for k in keys if k["joint"] == joint]


def times_of(keys, joint):
    return [k["t"] for k in by_track(keys, joint)]


def gaps(ts):
    return [f32(ts[i + 1] - ts[i]) for i in range(len(ts) - 1)]


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def track_of(doc, joint, path="rotation"):
    for t in doc.get("tracks", []):
        if t.get("joint") == joint and t.get("path") == path:
            return t
    return None


def same_floats(a, b, tol=0.0):
    """逐位（tol=0）或逐位到给定容差地比两串浮点。长度不同直接 False。"""
    a = [f32(float(x)) for x in a]
    b = [f32(float(x)) for x in b]
    if len(a) != len(b):
        return False
    return all(abs(x - y) <= tol for x, y in zip(a, b))


# ================================================================== A 打开 + 通道树
def case_a(tmp, dump_path=""):
    asset = join(os.path.join(tmp, "a.anim.json"))
    make_fixture(asset)
    # dumpkeys → 15 帧的 px；collapse+selrow → 摘要行接管折叠骨的帧
    text = run_editor({
        "MYVK_EDITOR_DOPE": asset,
        "MYVK_EDITOR_DOPE_SCRIPT": "dumpkeys;collapse:hips;selrow:hips",
    })
    if dump_path:
        with open(dump_path, "w", encoding="utf-8") as f:
            f.write(text)

    m = last(OPEN_RE, text)
    check("A1 DOPE-OPEN ok=1 tracks=3 keys=%d dur=%.1f" % (EXPECT_KEYS, EXPECT_DUR),
          m is not None and int(m.group(2)) == 3 and
          int(m.group(3)) == EXPECT_KEYS and
          abs(float(m.group(4)) - EXPECT_DUR) < 1e-3,
          "got=%s" % (groups(m),))

    check("A2 走的是 MYVK_EDITOR_DOPE 那条入口（reason=env）",
          "PANEL dope open=1 reason=env" in text, "")

    # ⚠ 用**第一条** DOPE-VIEW：折叠之后的行数变了，最后一条记的是 5 行。
    #   要钉的是"打开那一刻画布长什么样"，取最后一条就成了"折叠后还剩几行"。
    v = (all_of(VIEW_RE, text) or [None])[0]
    ok_v = (v is not None and float(v.group(4)) > float(v.group(3)) + 200.0 and
            float(v.group(5)) > 0.0 and int(v.group(7)) == 6 and
            120.0 <= float(v.group(9)) <= 240.0)
    check("A3 画布几何正常（6 行 = 3 摘要 + 3 通道；树宽落在 [120,240]）", ok_v,
          "view=%s" % (groups(v),))

    s0 = all_of(SUMMARY_RE, text)
    first_sum = s0[0] if s0 else None
    check("A4 打开时：rows=6 keys=%d sel=0 dur=%.1f" % (EXPECT_KEYS, EXPECT_DUR),
          first_sum is not None and int(first_sum.group(1)) == 6 and
          int(first_sum.group(2)) == EXPECT_KEYS and int(first_sum.group(3)) == 0 and
          abs(float(first_sum.group(4)) - EXPECT_DUR) < 1e-3,
          "got=%s" % (groups(first_sum),))

    bs = batches(text)
    if not check("A5 拿到一批 dumpkeys（%d 批）" % len(bs), len(bs) == 1,
                 "%d 批" % len(bs)):
        return
    keys = bs[0]["keys"]
    check("A6 十五帧都铺出来了（hips 10 / spine 3 / head 2）",
          len(keys) == EXPECT_KEYS and same_floats(times_of(keys, "hips"), HIPS_T) and
          same_floats(times_of(keys, "spine"), SPINE_T) and
          same_floats(times_of(keys, "head"), HEAD_T),
          "n=%d hips=%s" % (len(keys), times_of(keys, "hips")))

    h = bs[0]["h"]
    inside = all(h["px0"] - 1.0 <= k["px"] <= h["px1"] + 1.0 for k in keys)
    check("A7 十五颗菱形都落在画布横向范围内（点得到）", inside,
          "px0=%.1f px1=%.1f px=%s" % (h["px0"], h["px1"],
                                       [round(k["px"], 1) for k in keys]))

    m = last(COLLAPSE_RE, text)
    check("A8 collapse:hips → 折叠生效", m is not None and m.group(1) == "hips" and
          m.group(2) == "1", "got=%s" % (groups(m),))

    sums = all_of(SUMMARY_RE, text)
    last_sum = sums[-1] if sums else None
    check("A9 折叠后行数 6→5，且摘要行的 10 帧被 selrow 选中（sel=10）",
          last_sum is not None and int(last_sum.group(1)) == 5 and
          int(last_sum.group(2)) == EXPECT_KEYS and int(last_sum.group(3)) == 10,
          "got=%s" % (groups(last_sum),))

    m = last(SEL_RE, text)
    check("A10 DOPE-SEL n=10 t=[0.0000,1.1250]（= 没有被折叠吃掉的帧）",
          m is not None and m.group(1) == "10" and
          m.group(2) is not None and abs(float(m.group(2)) - HIPS_LO) < 1e-3 and
          abs(float(m.group(3)) - HIPS_HI) < 1e-3, "got=%s" % (groups(m),))

    m = last(SCRIPT_RE, text)
    check("A11 脚本收尾干净（applied=3 failed=0 sel=10）",
          m is not None and m.group(1) == "3" and m.group(2) == "0" and
          m.group(4) == "10", "got=%s" % (groups(m),))
    return text


# ================================================================== B 框选 10 帧一起挪
def case_b(tmp):
    asset = join(os.path.join(tmp, "b.anim.json"))
    make_fixture(asset)
    # 框选 hips 那 10 帧（t∈[0,1.125]）→ 整体右移 0.25 秒 → 存盘
    text = run_editor({
        "MYVK_EDITOR_DOPE": asset,
        "MYVK_EDITOR_DOPE_SCRIPT":
            "selrect:0:1.125;dumpkeys;move:0.25;dumpkeys;save;dumpkeys",
    })

    m = last(SEL_RE, text)
    check("B1 框到 10 帧（t=[0.0000,1.1250]）",
          m is not None and m.group(1) == "10" and
          m.group(2) is not None and abs(float(m.group(3)) - HIPS_HI) < 1e-3,
          "got=%s" % (groups(m),))

    mm = last(MOVE_RE, text)
    check("B2 DOPE-MOVE n=10 delta=0.250000 merged=0 dropped=0",
          mm is not None and mm.group(1) == "10" and
          abs(float(mm.group(2)) - 0.25) < 1e-6 and mm.group(3) == "0" and
          mm.group(4) == "0", "got=%s" % (groups(mm),))

    bs = batches(text)
    if not check("B3 三批 dumpkeys（挪前 / 挪后 / 存盘后）", len(bs) == 3,
                 "%d 批" % len(bs)):
        return
    before, after = times_of(bs[0]["keys"], "hips"), times_of(bs[1]["keys"], "hips")
    want = [f32(t + 0.25) for t in HIPS_T]

    # ⚠ 这条就是任务书的验收第 1 条前半句：整体偏移，**逐位**。
    check("B4 十帧整体 +0.25 秒（逐位相等）", same_floats(after, want),
          "got=%s" % (after,))

    # …后半句：相对间距逐帧不变。逐个累加的实现会在这里漂。
    check("B5 相对间距逐帧不变（9 个 gap 逐位相等）",
          same_floats(gaps(after), gaps(HIPS_T)),
          "before=%s after=%s" % (gaps(HIPS_T), gaps(after)))

    others_before = [(k["joint"], k["i"], k["t"]) for k in bs[0]["keys"]
                     if k["joint"] != "hips"]
    others_after = [(k["joint"], k["i"], k["t"]) for k in bs[1]["keys"]
                    if k["joint"] != "hips"]
    check("B6 没被框到的 5 帧逐位没动", others_before == others_after,
          "%s" % (others_after,))

    moved_sel = [k for k in bs[1]["keys"] if k["joint"] == "hips" and k["sel"] == 1]
    check("B7 选择集被改写成新的下标（挪完仍是这 10 帧选着）",
          len(moved_sel) == 10 and
          all(k["sel"] == 0 for k in bs[1]["keys"] if k["joint"] != "hips"),
          "sel=%d" % len(moved_sel))

    doc = load_json(asset)
    tr = track_of(doc, "hips")
    want_vals = [quat(i) for i in range(len(HIPS_T))]
    check("B8 存盘的是平移后的数（duration 仍是 %.1f）" % EXPECT_DUR,
          tr is not None and same_floats(tr["times"], want) and
          abs(float(doc["duration"]) - EXPECT_DUR) < 1e-6,
          "times=%s" % (tr["times"] if tr else None,))
    check("B9 存盘没顺手改值（values 与夹具逐位相同）",
          tr is not None and tr["values"] == want_vals, "")

    m = last(SCRIPT_RE, text)
    check("B10 脚本收尾干净（applied=6 failed=0）",
          m is not None and m.group(1) == "6" and m.group(2) == "0",
          "got=%s" % (groups(m),))

    m = last(SAVE_RE, text)
    check("B11 存盘写的是全部轨道与帧（tracks=3 keys=%d）" % EXPECT_KEYS,
          m is not None and m.group(2) == "3" and int(m.group(3)) == EXPECT_KEYS,
          "got=%s" % (groups(m),))


# ================================================================== C 镜像
def case_c(tmp):
    asset = join(os.path.join(tmp, "c.anim.json"))
    make_fixture(asset)
    # 镜像 → dump → 再镜像（应当回到原样）→ dump → 存盘
    text = run_editor({
        "MYVK_EDITOR_DOPE": asset,
        "MYVK_EDITOR_DOPE_SCRIPT":
            "selrect:0:1.125;mirror:0.5625;dumpkeys;mirror:0.5625;dumpkeys;save",
    })

    mm = last(MIRROR_RE, text)
    check("C1 DOPE-MIRROR n=10 pivot=0.562500 merged=0",
          mm is not None and mm.group(1) == "10" and
          abs(float(mm.group(2)) - HIPS_MID) < 1e-6 and mm.group(3) == "0",
          "got=%s" % (groups(mm),))

    bs = batches(text)
    if not check("C2 两批 dumpkeys（镜像后 / 再镜像后）", len(bs) == 2,
                 "%d 批" % len(bs)):
        return
    mir = times_of(bs[0]["keys"], "hips")

    # 每帧落到 2·pivot − t 上（逐位）。C++ 侧用 double 算中间值就是为了这条
    # 能用 `==` 而不是容差。
    want = sorted(f32(2.0 * HIPS_MID - t) for t in HIPS_T)
    check("C3 十帧都落到 2·0.5625 − t 上（逐位）", same_floats(mir, want),
          "got=%s want=%s" % (mir, want))

    # "前后半段对称"在节奏上的说法：间距序列正好反序。
    check("C4 间距序列反序（前后半段对称）",
          same_floats(gaps(mir), list(reversed(gaps(HIPS_T)))),
          "before=%s after=%s" % (gaps(HIPS_T), gaps(mir)))

    check("C5 镜像真的动了数据（不是恒等）",
          not same_floats(mir, HIPS_T), "%s" % (mir,))

    back = times_of(bs[1]["keys"], "hips")
    check("C6 镜像两次回到原样（逐位）", same_floats(back, HIPS_T),
          "got=%s" % (back,))

    others = [(k["joint"], k["i"], k["t"]) for k in bs[0]["keys"] if k["joint"] != "hips"]
    want_others = [("spine", i, t) for i, t in enumerate(SPINE_T)] + \
                  [("head", i, t) for i, t in enumerate(HEAD_T)]
    check("C7 没被选中的 spine / head 逐位没动", others == want_others,
          "%s" % (others,))

    doc = load_json(asset)
    tr = track_of(doc, "hips")
    check("C8 存盘的 hips 与原始逐位相同（来回一趟没丢东西）",
          tr is not None and same_floats(tr["times"], HIPS_T),
          "times=%s" % (tr["times"] if tr else None,))
    check("C9 存盘没顺手写 `types` 字段（全默认就不写）",
          tr is not None and "types" not in tr, "%s" % (sorted(tr) if tr else None))

    m = last(SCRIPT_RE, text)
    check("C10 脚本收尾干净（applied=6 failed=0）",
          m is not None and m.group(1) == "6" and m.group(2) == "0",
          "got=%s" % (groups(m),))


# ================================================================== D 关键帧类型
def case_d(tmp):
    asset = join(os.path.join(tmp, "d.anim.json"))
    fix = make_fixture(asset)
    # 设类型 → dump → 存盘 → 重新读盘 → dump（验落盘往返）
    text = run_editor({
        "MYVK_EDITOR_DOPE": asset,
        "MYVK_EDITOR_DOPE_SCRIPT":
            "selrect:0:1.125;type:extreme;dumpkeys;save;reload;dumpkeys",
    })

    m = last(TYPE_RE, text)
    check("D1 DOPE-TYPE n=10 type=extreme",
          m is not None and m.group(1) == "10" and m.group(2) == "extreme",
          "got=%s" % (groups(m),))

    bs = batches(text)
    if not check("D2 两批 dumpkeys（设类型后 / 重新读盘后）", len(bs) == 2,
                 "%d 批" % len(bs)):
        return
    for tag, b in (("设类型后", bs[0]), ("读盘后", bs[1])):
        hips = [k for k in b["keys"] if k["joint"] == "hips"]
        others = [k for k in b["keys"] if k["joint"] != "hips"]
        check("D3 %s：hips 十帧 type=extreme，其余五帧还是 key" % tag,
              len(hips) == 10 and all(k["type"] == "extreme" for k in hips) and
              len(others) == 5 and all(k["type"] == "key" for k in others),
              "hips=%s others=%s" % ([k["type"] for k in hips],
                                     [k["type"] for k in others]))

    doc = load_json(asset)
    tr = track_of(doc, "hips")
    check("D4 落盘写了 types=[1]×10", tr is not None and tr.get("types") == [1] * 10,
          "types=%s" % (tr.get("types") if tr else None,))

    # ⚠ 这条是"类型不影响求值"的**落盘一半**：求值只读 times / values，
    #   它俩逐位不动 → 采样结果不可能变。真正跑一遍求解器的等价性在
    #   C++ 自检 `dope_types_do_not_change_sampling` / `dope_types_independent`。
    check("D5 times / values 与夹具逐位相同（类型不改数据）",
          tr is not None and same_floats(tr["times"], fix["tracks"][0]["times"]) and
          tr["values"] == fix["tracks"][0]["values"], "")
    check("D6 spine / head 没有 types 字段（只写非默认）",
          all("types" not in track_of(doc, j) for j in ("spine", "head")),
          "")
    check("D7 duration / loop / name 没被顺手改掉",
          abs(float(doc["duration"]) - EXPECT_DUR) < 1e-6 and
          doc["loop"] is True and doc["name"] == "dope_fixture", "")

    m = last(SCRIPT_RE, text)
    check("D8 脚本收尾干净（applied=6 failed=0）",
          m is not None and m.group(1) == "6" and m.group(2) == "0",
          "got=%s" % (groups(m),))


# ================================================================== E 缩放 / 平移
def case_e(tmp):
    asset = join(os.path.join(tmp, "e.anim.json"))
    make_fixture(asset)
    # 先 dump（= 打开时 fitView 的视图），再缩放（绕播放头）、再平移
    text = run_editor({
        "MYVK_EDITOR_DOPE": asset,
        "MYVK_EDITOR_DOPE_SCRIPT": "dumpkeys;head:0.5;zoom:0.5;pan:0.5",
    })

    bs = batches(text)
    if not check("E1 拿到一批 dumpkeys", len(bs) == 1, "%d 批" % len(bs)):
        return
    h = bs[0]["h"]

    # 每颗菱形的 px 必须等于 px0 + (t−t0)·pps（用**同一批**头里的数）
    worst = 0.0
    for k in bs[0]["keys"]:
        want = h["px0"] + (k["t"] - h["t0"]) * h["pps"]
        worst = max(worst, abs(want - k["px"]))
    check("E2 十五颗菱形的 px ≡ px0+(t−t0)·pps（最差 %.3f px < 0.01）" % worst,
          worst < 0.01, "px0=%.1f pps=%.3f" % (h["px0"], h["pps"]))

    hz = last(ZOOM_RE, text)
    if not check("E3 打了 DOPE-ZOOM factor=0.5", hz is not None and
                 abs(float(hz.group(1)) - 0.5) < 1e-6, "got=%s" % (groups(hz),)):
        return

    span_before = h["t1"] - h["t0"]
    span_after = float(hz.group(3)) - float(hz.group(2))
    check("E4 缩放后跨度减半（%.4f → %.4f）" % (span_before, span_after),
          abs(span_after - span_before * 0.5) < 1e-2,
          "factor=0.5")

    # 绕播放头缩放的定义：播放头在窗口里的**相对位置**不变。
    # head 由脚本设成 0.5，两次都要落在同一个 frac 上。
    frac_before = (0.5 - h["t0"]) / span_before
    frac_after = (0.5 - float(hz.group(2))) / span_after
    check("E5 缩放锚点是播放头（frac %.4f → %.4f 不变）" % (frac_before, frac_after),
          abs(frac_before - frac_after) < 1e-3,
          "t0=%.4f → %.4f" % (h["t0"], float(hz.group(2))))

    hp = last(PAN_RE, text)
    if not check("E6 打了 DOPE-PAN dt=0.5", hp is not None, "got=%s" % (groups(hp),)):
        return
    check("E7 平移只挪 t0、不改跨度",
          abs((float(hp.group(3)) - float(hp.group(2))) - span_after) < 1e-3 and
          abs(float(hp.group(2)) - (float(hz.group(2)) + 0.5)) < 1e-3,
          "pan t0=%.4f t1=%.4f" % (float(hp.group(2)), float(hp.group(3))))

    v = last(VIEW_RE, text)
    check("E8 视图真的换了（脚本之后那帧的 DOPE-VIEW 与 pan 收尾一致）",
          v is not None and abs(float(v.group(1)) - float(hp.group(2))) < 1e-3 and
          abs(float(v.group(2)) - float(hp.group(3))) < 1e-3,
          "view=%s pan=%s" % (groups(v), groups(hp)))


# ================================================================== G 对齐循环长度
def case_g(tmp):
    asset = join(os.path.join(tmp, "g.anim.json"))
    make_fixture(asset)
    # 把 hips 那 10 帧整体"拉/压"到 [0, 1.5]：首帧不动、末帧精确落在 1.5，
    # 中间每帧的**相位**（相对位置）不变。
    text = run_editor({
        "MYVK_EDITOR_DOPE": asset,
        "MYVK_EDITOR_DOPE_SCRIPT": "selrect:0:1.125;align:1.5;dumpkeys;save",
    })

    m = last(ALIGN_RE, text)
    span = HIPS_HI - HIPS_LO
    want_scale = (1.5 - HIPS_LO) / span
    check("G1 DOPE-ALIGN n=10 target=1.500000 scale=%.6f first=0 last=1.5"
          % want_scale,
          m is not None and m.group(1) == "10" and
          abs(float(m.group(2)) - 1.5) < 1e-6 and
          abs(float(m.group(3)) - want_scale) < 1e-4 and
          abs(float(m.group(4)) - HIPS_LO) < 1e-6 and
          abs(float(m.group(5)) - 1.5) < 1e-6,
          "got=%s" % (groups(m),))

    bs = batches(text)
    if not check("G2 拿到一批 dumpkeys", len(bs) == 1, "%d 批" % len(bs)):
        return
    ts = times_of(bs[0]["keys"], "hips")

    # 首帧是相位锚点，必须**逐位**不动（对齐只该改它右边的东西）
    check("G3 首帧原地不动（相位锚点）", ts and ts[0] == HIPS_T[0],
          "t0=%.6f" % (ts[0] if ts else -1))
    # 末帧走特判：`lo+(hi−lo)·scale` 的舍入可能差 1 ulp，而"对齐到 1.5 秒"
    # 必须精确 —— 否则循环点永远差一丁点，且每循环一次再差一点。
    check("G4 末帧精确落在 1.5（不是 1.4999999）", ts and ts[-1] == 1.5,
          "t_last=%.9f" % (ts[-1] if ts else -1))
    # 相位 = 相对位置。这才是"按相位对齐循环长度"这句话的内容。
    worst = 0.0
    if len(ts) == len(HIPS_T):
        for a, b in zip(HIPS_T, ts):
            fa = (a - HIPS_LO) / span
            fb = (b - ts[0]) / (ts[-1] - ts[0])
            worst = max(worst, abs(fa - fb))
    check("G5 中间九帧的相位不变（最差 %.2e < 1e-6）" % worst, worst < 1e-6,
          "worst=%.3e" % worst)

    doc = load_json(asset)
    tr = track_of(doc, "hips")
    check("G6 存盘与内存一致 / values 没被顺手改掉",
          tr is not None and same_floats(tr["times"], ts, tol=1e-6) and
          tr["values"] == [quat(i) for i in range(len(HIPS_T))], "")

    m = last(SCRIPT_RE, text)
    check("G7 脚本收尾干净（applied=4 failed=0）",
          m is not None and m.group(1) == "4" and m.group(2) == "0",
          "got=%s" % (groups(m),))


# ================================================================== F UI 锚点 + 零错误
def case_f(text):
    need = ["DOPE-WIN", "DOPE-CANVAS", "DOPE-PATH", "DOPE-SAVE", "DOPE-FIT",
            "DOPE-HEAD", "DOPE-TYPE", "DOPE-MIRROR", "DOPE-ALIGN", "DOPE-DEL"]
    missing = [t for t in need if not rects(text, t)]
    check("F1 十个控件锚点都在 —— 面板真的画在屏幕上", not missing,
          "missing=%s" % missing)

    win = rects(text, "DOPE-WIN")
    cv = rects(text, "DOPE-CANVAS")
    ok_geo = False
    if win and cv:
        w, c = win[-1], cv[-1]
        ok_geo = (w[2] - w[0] > 600.0 and w[3] - w[1] > 300.0 and
                  c[0] >= w[0] - 1.0 and c[2] <= w[2] + 1.0 and
                  c[1] >= w[1] - 1.0 and c[3] <= w[3] + 1.0)
    check("F2 窗口尺寸合理且画布在窗口内", ok_geo,
          "win=%s canvas=%s" % (win[-1] if win else None, cv[-1] if cv else None))

    bad = [l for l in text.splitlines()
           if re.search(BAD_RE, l)]
    check("F3 全程零 Vulkan 验证层报错 / 零 [ERROR]（%d 条）" % len(bad), not bad,
          "%s" % (bad[:3],))


# ================================================================== main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑某个用例（a/b/c/d/e/f/g）")
    ap.add_argument("--keep", action="store_true", help="保留临时目录")
    ap.add_argument("--dump", default="", help="把 A 用例的完整日志写到这个文件")
    args = ap.parse_args()

    if not os.path.isfile(EDITOR_EXE):
        print("找不到 %s —— 先编译 VennEditor" % EDITOR_EXE)
        return 2

    tmp = tempfile.mkdtemp(prefix="venn_dope_")
    only = args.only.lower()
    a_text = ""
    try:
        if only in ("", "a", "f"):
            a_text = case_a(tmp, args.dump) or ""
        if only in ("", "b"):
            case_b(tmp)
        if only in ("", "c"):
            case_c(tmp)
        if only in ("", "d"):
            case_d(tmp)
        if only in ("", "e"):
            case_e(tmp)
        if only in ("", "f"):
            case_f(a_text)
        if only in ("", "g"):
            case_g(tmp)
    finally:
        if args.keep:
            print("临时目录：%s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
        try:
            subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                           capture_output=True, timeout=10)
        except Exception:
            pass

    n_pass = sum(1 for _, ok, _ in results if ok)
    print("\n%d/%d PASS" % (n_pass, len(results)))
    # ⚠ 最后一行必须是 `结果: PASS` / `结果: FAIL: N 项` —— run_regression.py
    #   的 OK_RE 只认那几种写法。
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
