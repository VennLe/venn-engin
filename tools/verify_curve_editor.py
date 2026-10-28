#!/usr/bin/env python3
"""端到端验证：曲线编辑器（Graph Editor，R6 / #318）

==============================================================================
这一条在验什么
==============================================================================
R5 把"值怎么随时间变"收敛成了 `animation::Curve`，但入口只有时间轴编辑器里
4 个拖条。**看不见曲线就调不出手感** —— 本脚本钉的是"曲线能画出来、能拖、
拖完数据真的变了"：

  A 打开：面板画出来（9 个控件锚点）+ 资产真解析了（5 帧 / 8 秒）+
    **画布投影 ≡ 运行时采样**（`CURVED-CHECK xcheck` 要 < 1e-4）
  B 拖关键帧：像素增量 → 秒/值的换算对得上 `CURVED-MAP`；存下来别的东西
    一点没动（同一帧的 pos.y/z 逐位不变）
  C 拖手柄：同一采样点**单调**变化（两次 -30px 各推一步，方向一致）
  D 切插值（T）：linear → step 后同一时刻的采样差 > 0.5（step 是"保持前帧"）
  E 切手柄类型（V）：vector 的手柄必须是 (±1, ±1)（= 直线），auto_clamped
    在局部极值处必须压平成 0 —— 这是两种类型**语义**上的差别，不是"数值变了"
  F 框选 + 批量拖：三帧整体平移，**相对间距逐帧不变**，未选中的两帧一点没动
  G 存盘：文件里 ease / 手柄对得上，`"//"` 注释原样保住（存盘不丢东西）
  H 磁盘冲突：打开之后文件被别人改过 → Save 被拒（reason=stale）→
    Save anyway 才写得进去（两个编辑器开同一份资产时唯一的护栏）
  I 布局持久化：关掉重开，面板自己回来（面板表 → 落盘 → 读回）

==============================================================================
三个刻意的取舍
==============================================================================
① **不用鼠标拖**。注入脚本走的是 `act*` + `beginDrag/actDragKeysPx/endDrag`
   那一族 —— 和鼠标拖动**同一条路**（像素→值只有一个换算函数）。走同一条路
   验的是真行为；而"鼠标能不能真的按到那个点"由 `CURVED-CANVAS` / `CURVED-KEY`
   的矩形锚点证明（它们在，说明画布真的画在屏幕上）。

② **像素增量归一化到 `CURVED-MAP`**。浮动窗口的尺寸会被 imgui.ini 记住，
   "拖 60px = 0.65 秒"这种写死的期望在另一台机器上就错。所以断言一律
   "先读 MAP，再算期望"。

③ **`boxsel` 用绘图区比例**，不是像素：同一份脚本在任何窗口尺寸下圈到
   同样多帧。

用法：
    python tools/verify_curve_editor.py
    python tools/verify_curve_editor.py --only b     # 只跑某个用例
    python tools/verify_curve_editor.py --keep       # 留下临时目录
    python tools/verify_curve_editor.py --dump x.log # 把 A 用例完整日志写出
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "build", "bin")
EDITOR_EXE = os.path.join(BIN, "Editor.exe")

sys.path.insert(0, HERE)
from verify_add_menu import LogTap                      # noqa: E402

# 被编辑的资产（相对 assets/）。5 帧、4 事件、duration=8。
ASSET = "sequences/opening.seq.json"
ASSET_ABS = os.path.join(BIN, "assets", ASSET)

EXPECT_KEYS = 5
EXPECT_DUR = 8.0
# 原资产五帧的时间（断言"拖之前是这个数"）
ORIG_T = [0.0, 2.5, 4.0, 6.0, 8.0]

# 让编辑器自己退出（不认这个变量就得 kill，而"日志截到哪一帧"随机器浮动）
FRAMES = 150

# 原资产里那一大段 `"//"` 说明（G 用例拿它验"注释没被抹掉"）
COMMENT_KEY = "//"
COMMENT_MARK = "yaw / pitch 单位是"

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


# ------------------------------------------------------------------ 启动
def run_editor(extra_env, wait_s=120.0, ready_needle="", on_ready=None):
    """起一程编辑器，返回完整日志。

    只有 H（磁盘冲突）需要 `ready_needle` / `on_ready`：那条用例得在编辑器
    **还活着**的时候从外面改文件，`communicate()` 一等到底就没缝插进去。所以
    改成 `LogTap` 泵管道 —— `ready_needle` 出现即回调，回调返回后再等进程自己
    退（`MYVK_FRAMES` 会兜底）。

    ⚠ `text=True` 不能省：`LogTap._pump` 里 `raw.strip()` 拿到的若是 bytes，
      `has_line("CURVED-...")` 永远 False → 闸门永远等不到，H 用例会以
      "reason=stale 没出现"的形式失败，而根因在编码上。
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"     # 不设它 ui::logRect 直接 return
    env["MYVK_GRID"] = "0"
    env["MYVK_FRAMES"] = str(FRAMES)
    env["MYVK_NO_WINDOW_SAVE"] = "1"   # 自动化期间不写 imgui.ini / 布局
    for k, v in extra_env.items():
        if v is not None:
            env[k] = v

    # 上一轮崩掉的实例会抢窗口标题 → 先清一遍（与 verify_timeline_editor 同纪律）
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

    if ready_needle:
        if wait_line(tap, ready_needle, wait_s):
            if on_ready:
                try:
                    on_ready()
                except Exception as exc:      # 回调炸了要留证据，不能静默
                    print("[WARN] on_ready 抛异常：%r" % (exc,))
        else:
            print("[WARN] 等不到闸门 %r（%ds）" % (ready_needle, int(wait_s)))

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
    """给注入脚本用的路径：统一正斜杠。"""
    return p.replace("\\", "/")


# ------------------------------------------------------------------ 日志解析
OPEN_RE = re.compile(
    r"CURVED-OPEN path=(\S+) ok=(\d+) keys=(\d+) dur=([\d.]+) readOnly=(\d)")
CHECK_RE = re.compile(
    r"CURVED-CHECK ok=(\d+) keys=(\d+) dur=([\d.]+) warns=(\d+) xcheck=([\d.eE+-]+) "
    r"where=(\S*) err=(.*)$")
MAP_RE = re.compile(
    r"CURVED-MAP t0=([-\d.]+) t1=([-\d.]+) v0=([-\d.]+) v1=([-\d.]+) "
    r"px0=([-\d.]+) px1=([-\d.]+) py0=([-\d.]+) py1=([-\d.]+) ch=(\S+)")
KEY_RE = re.compile(
    r"CURVED-KEY i=(\d+) t=([-\d.]+) v=([-\d.]+) px=([-\d.]+) py=([-\d.]+) sel=(\d)")
SAMPLE_RE = re.compile(r"CURVED-SAMPLE t=([-\d.]+) v=([-\d.]+) ch=(\S+)")
DRAGKEY_RE = re.compile(
    r"CURVED-DRAG-END kind=key i=(-?\d+) t=([-\d.]+) v=([-\d.]+) n=(\d+) ch=(\S+)")
DRAGH_RE = re.compile(
    r"CURVED-DRAG-END kind=handle i=(\d+) side=(\w+) x=([-\d.]+) y=([-\d.]+) ch=(\S+)")
SETEASE_RE = re.compile(
    r"CURVED-SETEASE i=(\d+) ease=(\w+) interp=(\w+) easeMode=(\w+) shape=(\w+)")
SETHT_RE = re.compile(
    r"CURVED-SETHANDLETYPE i=(\d+) type=(\w+) ch=(\S+) out=([-\d.]+),([-\d.]+) "
    r"in=([-\d.]+),([-\d.]+)")
BOXRE = re.compile(r"CURVED-BOXSEL n=(\d+) primary=(-?\d+)")
SAVE_RE = re.compile(r"CURVED-SAVE path=(\S+) keys=(\d+) force=(\d)")
BLOCK_RE = re.compile(r"CURVED-SAVE-BLOCKED path=(\S+) reason=(\S+)")
SCRIPT_RE = re.compile(r"CURVED-SCRIPT applied=(\d+) failed=(\d+)")


def last(regex, text):
    hits = [m for m in (regex.search(l) for l in text.splitlines()) if m]
    return hits[-1] if hits else None


def all_of(regex, text):
    return [m for m in (regex.search(l) for l in text.splitlines()) if m]


def groups(m):
    return m.groups() if m else "none"


def map_of(text):
    m = last(MAP_RE, text)
    if not m:
        return None
    g = [float(m.group(i)) for i in range(1, 9)]
    return {"t0": g[0], "t1": g[1], "v0": g[2], "v1": g[3],
            "px0": g[4], "px1": g[5], "py0": g[6], "py1": g[7],
            "ch": m.group(9)}


def px_per_sec(mp):
    return (mp["px1"] - mp["px0"]) / max(mp["t1"] - mp["t0"], 1e-6)


def px_per_unit(mp):
    return (mp["py1"] - mp["py0"]) / max(mp["v1"] - mp["v0"], 1e-6)


def keys_of(text):
    """每个下标**最后一次**出现的那一行（拖动之后状态会重打）。"""
    out = {}
    for m in all_of(KEY_RE, text):
        out[int(m.group(1))] = {"t": float(m.group(2)), "v": float(m.group(3)),
                                "px": float(m.group(4)), "py": float(m.group(5)),
                                "sel": int(m.group(6))}
    return out


def samples_of(text):
    return [{"t": float(m.group(1)), "v": float(m.group(2)), "ch": m.group(3)}
            for m in all_of(SAMPLE_RE, text)]


def dump_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


# ------------------------------------------------------------------ 临时目录
class Ctx:
    def __init__(self, tmp):
        self.tmp = tmp
        self.copy_n = 0

    def asset(self, tag):
        """每次都从原资产复制一份新的 —— 用例之间不互相污染。"""
        self.copy_n += 1
        dst = join(os.path.join(self.tmp, "%s.seq.json" % tag))
        shutil.copyfile(ASSET_ABS, dst)
        return dst

    def out(self, tag):
        return join(os.path.join(self.tmp, "%s.dump.json" % tag))


# ================================================================== A 打开
def case_a(dump_path=""):
    # ⚠ 必须带一个脚本：`CURVED-SCRIPT applied=` 只在 runInjectedScript() 里打，
    #   而那个函数在 env 没设时直接 return。A9 断言的就是"注入通路跑通、没有一个
    #   token 失败"，空脚本时这行永远不存在 —— 那不是产品的问题，是这条用例的问题。
    text = run_editor({
        "MYVK_EDITOR_CURVED": ASSET,
        "MYVK_EDITOR_CURVED_SCRIPT": "check;sample:4.0",
    })
    if dump_path:
        with open(dump_path, "w", encoding="utf-8") as f:
            f.write(text)

    m = last(OPEN_RE, text)
    check("A1 CURVED-OPEN ok=1 keys=%d dur=%.2f readOnly=0" % (EXPECT_KEYS, EXPECT_DUR),
          m is not None and m.group(2) == "1" and
          int(m.group(3)) == EXPECT_KEYS and
          abs(float(m.group(4)) - EXPECT_DUR) < 1e-3 and m.group(5) == "0",
          "got=%s" % (groups(m),))

    check("A2 走的是 MYVK_EDITOR_CURVED 那条入口（reason=env）",
          "PANEL curveed open=1 reason=env" in text, "")

    m = last(CHECK_RE, text)
    xcheck = float(m.group(5)) if m else 9.9
    check("A3 CURVED-CHECK ok=1（序列化后能原样解析回去）",
          m is not None and m.group(1) == "1" and int(m.group(2)) == EXPECT_KEYS,
          "got=%s" % (groups(m),))
    # ⚠ 这条是 R6 的核心：画布画的是 `evaluateCurve`，运行时走 `sampleCamera`。
    #   两者数学等价、**浮点不逐位**（glm::mix 是 a*(1-w)+b*w，evalSegment 是
    #   a+(b-a)*w），所以判据是 1e-4 而不是 0。真分叉的话会差到 1e-1 量级。
    check("A4 画布投影 ≡ 运行时采样（最差 %.2e < 1e-4）" % xcheck, xcheck < 1e-4,
          "xcheck=%.3e where=%s" % (xcheck, m.group(6) if m else "?"))

    need = ["CURVED-WIN", "CURVED-CANVAS", "CURVED-PATH", "CURVED-SAVE",
            "CURVED-CHANNEL", "CURVED-HEAD", "CURVED-FIT", "CURVED-PROP-T",
            "CURVED-PROP-EASE"]
    missing = [t for t in need if t not in text]
    check("A5 九个控件锚点都在 —— 面板真的画在屏幕上", not missing,
          "missing=%s" % missing)

    ks = keys_of(text)
    ok_keys = len(ks) == EXPECT_KEYS and all(
        abs(ks[i]["t"] - ORIG_T[i]) < 1e-3 for i in range(EXPECT_KEYS))
    check("A6 五帧都画出来了，t = %s" % ORIG_T, ok_keys,
          "got=%s" % {i: ks[i]["t"] for i in sorted(ks)})

    mp = map_of(text)
    ok_map = (mp is not None and mp["px1"] > mp["px0"] + 100.0 and
              mp["py1"] > mp["py0"] + 50.0 and mp["t0"] < 0.0 and
              mp["t1"] > EXPECT_DUR and mp["ch"] == "position.x")
    check("A7 画布映射正常（时间轴两端留了外推余量，纵轴倒置）", ok_map,
          "%s" % (mp,))

    # 关键帧点必须落在绘图区里（不然拖不到）
    if mp and ks:
        inside = all(mp["px0"] - 1 <= ks[i]["px"] <= mp["px1"] + 1 and
                     mp["py0"] - 1 <= ks[i]["py"] <= mp["py1"] + 1 for i in ks)
        check("A8 五个关键帧点都落在绘图区内（可拖）", inside,
              "keys=%s" % {i: (ks[i]["px"], ks[i]["py"]) for i in sorted(ks)})

    m = last(SCRIPT_RE, text)
    check("A9 脚本收尾干净（failed=0）", m is not None and m.group(2) == "0",
          "got=%s" % (groups(m),))


# ================================================================== B 拖关键帧
def case_b(ctx):
    asset = ctx.asset("b")
    dump = ctx.out("b")
    text = run_editor({
        "MYVK_EDITOR_CURVED": asset,
        "MYVK_EDITOR_CURVED_DUMP": dump,
        # ① 第 1 帧（t=2.5）往右拖 60px；② 第 2 帧（t=4.0）往上拖 40px。
        "MYVK_EDITOR_CURVED_SCRIPT":
            "chan:position.x;sel:1;dragkey:1:60:0;sel:2;dragkey:2:0:-40;dump",
    })
    mp = map_of(text)
    if not check("B0 拿到画布映射", mp is not None, "%s" % (mp,)):
        return
    pps, ppu = px_per_sec(mp), px_per_unit(mp)

    m = last(DRAGKEY_RE, text)
    check("B1 拖完打了 CURVED-DRAG-END kind=key", m is not None,
          "got=%s" % (groups(m),))

    ks = keys_of(text)
    want_t1 = ORIG_T[1] + 60.0 / pps
    check("B2 第 1 帧的 t 跟着像素走（%.3f → 期望 %.3f）" % (ks.get(1, {}).get("t", -1),
                                                            want_t1),
          abs(ks.get(1, {}).get("t", -1) - want_t1) < 0.02,
          "dx=60px @ %.1f px/s" % pps)

    want_v2 = 1.6 + 40.0 / ppu     # pos.x[2] 原本 1.6；往上拖 = 值变大
    check("B3 第 2 帧的 value 跟着像素走（期望 %.3f）" % want_v2,
          abs(ks.get(2, {}).get("v", -1) - want_v2) < 0.03,
          "got=%.4f  py_per_unit=%.2f" % (ks.get(2, {}).get("v", -1), ppu))

    data = dump_json(dump)
    cams = data["camera"]
    check("B4 存盘/导出里 t 真的变了（2.5 → %.3f）" % want_t1,
          len(cams) == EXPECT_KEYS and abs(cams[1]["t"] - want_t1) < 0.02,
          "t=%.4f" % cams[1]["t"])
    # ⚠ 只动被拖的那个标量：同一帧的 y / z / yaw / pitch / fov 必须**逐位**不变。
    orig = dump_json(ASSET_ABS)["camera"]
    same = (abs(cams[1]["pos"][1] - orig[1]["pos"][1]) < 1e-7 and
            abs(cams[1]["pos"][2] - orig[1]["pos"][2]) < 1e-7 and
            abs(cams[1]["yaw"] - orig[1]["yaw"]) < 1e-7 and
            abs(cams[1]["pitch"] - orig[1]["pitch"]) < 1e-7 and
            abs(cams[1]["fov"] - orig[1]["fov"]) < 1e-7)
    check("B5 只改了 position.x，同一帧的其余字段逐位不变", same,
          "pos=%s" % (cams[1]["pos"],))

    same_other = all(abs(cams[i]["pos"][0] - orig[i]["pos"][0]) < 1e-7
                     for i in (0, 3, 4))
    check("B6 没被选中的帧一个都没动", same_other,
          "%s" % [cams[i]["pos"][0] for i in (0, 3, 4)])


# ================================================================== C 拖手柄
def case_c(ctx):
    asset = ctx.asset("c")
    dump = ctx.out("c")
    text = run_editor({
        "MYVK_EDITOR_CURVED": asset,
        "MYVK_EDITOR_CURVED_DUMP": dump,
        # 同一根 out 手柄推两次（每次 -30px = 往上），中间各采样一次。
        "MYVK_EDITOR_CURVED_SCRIPT":
            "chan:position.x;sel:0;setease:bezier;sample:1.0;"
            "draghandle:0:out:0:-30;sample:1.0;"
            "draghandle:0:out:0:-30;sample:1.0;dump",
    })
    ss = samples_of(text)
    if not check("C0 三次采样都在（%d 条）" % len(ss),
                 len(ss) == 3 and all(abs(s["t"] - 1.0) < 1e-4 for s in ss),
                 "%s" % ss):
        return
    v0, v1, v2 = ss[0]["v"], ss[1]["v"], ss[2]["v"]
    # 这一段 pos.x 是 5.5 → 3.0（下降）。手柄往上抬 = 前段更"鼓" → 采样值**变大**。
    check("C1 第一次拖手柄：同一采样点变大（%.4f → %.4f）" % (v0, v1), v1 > v0 + 0.05,
          "delta=%.4f" % (v1 - v0))
    check("C2 第二次同方向：继续变大（%.4f → %.4f）" % (v1, v2), v2 > v1 + 0.05,
          "delta=%.4f" % (v2 - v1))
    check("C3 单调（两步同号且都不为零）", (v1 - v0) * (v2 - v1) > 0,
          "")

    m = last(DRAGH_RE, text)
    check("C4 打了 CURVED-DRAG-END kind=handle side=out", m is not None and m.group(2) == "out",
          "got=%s" % (groups(m),))

    cams = dump_json(dump)["camera"]
    hy = cams[0]["outHandle"][1]
    check("C5 手柄值写进了资产（outHandle.y = %.3f ≠ 0）" % hy, hy < -0.05,
          "outHandle=%s" % (cams[0]["outHandle"],))
    check("C6 另一根手柄没被顺手改掉（inHandle 仍是默认）",
          abs(cams[0]["inHandle"][0] + 1.0 / 3.0) < 0.02 and
          abs(cams[0]["inHandle"][1]) < 1e-6,
          "inHandle=%s" % (cams[0]["inHandle"],))


# ================================================================== D 切插值
def case_d(ctx):
    asset = ctx.asset("d")
    dump = ctx.out("d")
    text = run_editor({
        "MYVK_EDITOR_CURVED": asset,
        "MYVK_EDITOR_CURVED_DUMP": dump,
        # 第 1 帧（t=2.5，原本就是 step）先改成 linear，再改回 step，各采一次。
        "MYVK_EDITOR_CURVED_SCRIPT":
            "chan:position.x;sel:1;setease:linear;sample:3.75;"
            "setease:step;sample:3.75;cycleease;dump",
    })
    ss = samples_of(text)
    if not check("D0 两次采样都在", len(ss) == 2, "%s" % ss):
        return
    lin, step = ss[0]["v"], ss[1]["v"]
    check("D1 linear 段：t=3.75 时已经走到 %.3f" % lin,
          abs(lin - (3.0 + (1.6 - 3.0) * ((3.75 - 2.5) / 1.5))) < 0.01,
          "got=%.4f" % lin)
    check("D2 step 段：整段保持前一帧（3.0），差 %.3f > 0.5" % abs(lin - step),
          abs(step - 3.0) < 0.01 and abs(lin - step) > 0.5,
          "lin=%.4f step=%.4f" % (lin, step))

    # ⚠ 不能用 last()：脚本最后一步是 cycleease（step → bezier），最后一条
    #   CURVED-SETEASE 记的是 bezier 那一档。要断言的是**step 那一档**的映射。
    hits = all_of(SETEASE_RE, text)
    step_hit = None
    for h in hits:
        if h.group(2) == "step":
            step_hit = h
    check("D3 step 映射到曲线核心的 interp=constant",
          step_hit is not None and step_hit.group(3) == "constant",
          "eases=%s" % [(h.group(2), h.group(3)) for h in hits])

    # T 从 step 按一下 → bezier（循环表 linear → smooth → step → bezier）
    check("D4 T 从 step 循环到 bezier",
          len(hits) >= 3 and hits[-1].group(2) == "bezier" and
          hits[-1].group(3) == "bezier",
          "got=%s" % ([h.group(2) for h in hits],))


# ================================================================== E 切手柄类型
def case_e(ctx):
    asset = ctx.asset("e")
    dump = ctx.out("e")
    text = run_editor({
        "MYVK_EDITOR_CURVED": asset,
        "MYVK_EDITOR_CURVED_DUMP": dump,
        # 第 2 帧（t=4.0, pos.x=1.6）在 1.6 → 2.2 这一段上是**局部极小值**
        # （前一帧 3.0 更大、后一帧 2.2 更大）→ AutoClamped 必须压平。
        "MYVK_EDITOR_CURVED_SCRIPT":
            "chan:position.x;sel:2;setease:bezier;sample:4.5;"
            "setht:vector;sample:4.5;setht:auto_clamped;sample:4.5;dump",
    })
    ss = samples_of(text)
    if not check("E0 三次采样都在", len(ss) == 3, "%s" % ss):
        return

    hts = all_of(SETHT_RE, text)
    if not check("E1 两次 CURVED-SETHANDLETYPE 都打了", len(hts) == 2,
                 "%s" % [h.groups() for h in hts]):
        return
    vec, clamp = hts[0], hts[1]
    check("E2 vector → 手柄 (±1, ±1)（指向另一端点 = 直线）",
          vec.group(2) == "vector" and
          abs(float(vec.group(4)) - 1.0) < 1e-6 and abs(float(vec.group(5)) - 1.0) < 1e-6 and
          abs(float(vec.group(6)) + 1.0) < 1e-6 and abs(float(vec.group(7)) + 1.0) < 1e-6,
          "out=%s,%s in=%s,%s" % (vec.group(4), vec.group(5), vec.group(6), vec.group(7)))
    check("E3 auto_clamped 在局部极值处压平（out.y = 0）",
          clamp.group(2) == "auto_clamped" and abs(float(clamp.group(5))) < 1e-6 and
          abs(float(clamp.group(7))) < 1e-6,
          "out=%s,%s in=%s,%s" % (clamp.group(4), clamp.group(5), clamp.group(6), clamp.group(7)))

    # vector 段 = 直线：t=4.5（u=0.25）应该是 1.6 + 0.6*0.25 = 1.75
    check("E4 vector 段的采样真的等于直线插值（%.4f ≈ 1.75）" % ss[1]["v"],
          abs(ss[1]["v"] - 1.75) < 0.01, "got=%.4f" % ss[1]["v"])
    # 压平 = 起步变慢：同一时刻的值应当**低于**直线插值（1.75），而不是高于。
    # 判据不写死具体数（那会把测试绑在贝塞尔求解器的实现上），只钉住
    # "落在 [起点, 直线] 之间、且离直线至少 0.03" —— 既排除"手柄根本没生效
    # （= 1.75）"，也排除"被算到区间外（过冲）"。
    check("E5 压平后起步更慢：%.4f 低于直线 %.4f" % (ss[2]["v"], ss[1]["v"]),
          1.60 < ss[2]["v"] < ss[1]["v"] - 0.03,
          "delta=%.4f（负 = 更滞后）" % (ss[2]["v"] - ss[1]["v"]))

    cams = dump_json(dump)["camera"]
    check("E6 最终落在资产里的是 auto_clamped 的数值（不是「类型」）",
          abs(cams[2]["outHandle"][1]) < 1e-6 and
          abs(cams[2]["outHandle"][0] - 1.0 / 3.0) < 1e-3,
          "outHandle=%s" % (cams[2]["outHandle"],))


# ================================================================== F 框选 + 批量
def case_f(ctx):
    asset = ctx.asset("f")
    dump = ctx.out("f")
    text = run_editor({
        "MYVK_EDITOR_CURVED": asset,
        "MYVK_EDITOR_CURVED_DUMP": dump,
        # 框住 t ∈ [≈2.1, ≈6.4] 那一段（= 第 1/2/3 帧），整体右移 24px
        "MYVK_EDITOR_CURVED_SCRIPT":
            "chan:position.x;boxsel:0.28:0.0:0.78:1.0;dragkey:1:24:0;dump",
    })
    m = last(BOXRE, text)
    check("F1 框选圈到 3 帧（2.5 / 4.0 / 6.0）", m is not None and m.group(1) == "3",
          "got=%s" % (groups(m),))

    mp = map_of(text)
    pps = px_per_sec(mp) if mp else 0.0
    want = 24.0 / pps if pps else 0.0
    cams = dump_json(dump)["camera"]
    d1 = cams[1]["t"] - ORIG_T[1]
    d2 = cams[2]["t"] - ORIG_T[2]
    d3 = cams[3]["t"] - ORIG_T[3]
    check("F2 三帧整体平移 %.3f s（期望 %.3f）" % (d1, want),
          abs(d1 - want) < 0.02 and abs(d2 - want) < 0.02 and abs(d3 - want) < 0.02,
          "d=%s" % [round(d1, 4), round(d2, 4), round(d3, 4)])
    # ⚠ 这条就是"批量平移"的定义：相对间距**逐帧**不变。逐个累加实现会漂。
    check("F3 相对间距逐帧不变（0.0 / 1.5 / 2.0 仍是 1.5 / 2.0）",
          abs((cams[2]["t"] - cams[1]["t"]) - 1.5) < 1e-3 and
          abs((cams[3]["t"] - cams[2]["t"]) - 2.0) < 1e-3,
          "gaps=%s" % [round(cams[2]["t"] - cams[1]["t"], 4),
                       round(cams[3]["t"] - cams[2]["t"], 4)])
    check("F4 没被框到的两帧原地不动",
          abs(cams[0]["t"]) < 1e-6 and abs(cams[4]["t"] - 8.0) < 1e-6,
          "t0=%.4f t4=%.4f" % (cams[0]["t"], cams[4]["t"]))


# ================================================================== G 存盘
def case_g(ctx):
    asset = ctx.asset("g")
    dump = ctx.out("g")
    text = run_editor({
        "MYVK_EDITOR_CURVED": asset,
        "MYVK_EDITOR_CURVED_DUMP": dump,
        "MYVK_EDITOR_CURVED_SCRIPT":
            "chan:position.x;sel:0;setease:bezier;sethandle:out:0.25:0.8;save;dump",
    })
    m = last(SAVE_RE, text)
    check("G1 CURVED-SAVE keys=5 force=0", m is not None and m.group(2) == "5" and
          m.group(3) == "0", "got=%s" % (groups(m),))

    on_disk = dump_json(asset)
    cams = on_disk["camera"]
    check("G2 文件里 ease=bezier 且手柄是刚拖的那组",
          cams[0]["ease"] == "bezier" and
          abs(cams[0]["outHandle"][0] - 0.25) < 1e-6 and
          abs(cams[0]["outHandle"][1] - 0.8) < 1e-6,
          "ease=%s outHandle=%s" % (cams[0]["ease"], cams[0].get("outHandle")))
    check("G3 只有 bezier 段写手柄（其余四帧没有 outHandle 字段）",
          all("outHandle" not in cams[i] for i in (1, 2, 3, 4)),
          "%s" % [sorted(k for k in c if k.startswith("out")) for c in cams])

    orig = dump_json(ASSET_ABS)
    check("G4 `\"//\"` 注释原样保住（存盘不丢东西）",
          COMMENT_KEY in on_disk and COMMENT_MARK in on_disk[COMMENT_KEY] and
          on_disk[COMMENT_KEY] == orig[COMMENT_KEY], "")
    check("G5 事件与时长都没被顺手改掉",
          len(on_disk["events"]) == len(orig["events"]) and
          abs(on_disk["duration"] - orig["duration"]) < 1e-6,
          "events=%d dur=%.3f" % (len(on_disk["events"]), on_disk["duration"]))
    check("G6 dump 与文件一致（同一份资产）",
          json.dumps(dump_json(dump)["camera"], sort_keys=True) ==
          json.dumps(cams, sort_keys=True), "")


# ================================================================== H 磁盘冲突
def case_h(ctx):
    asset = ctx.asset("h")
    text = run_editor({
        "MYVK_EDITOR_CURVED": asset,
        # sleep 是给这条用例的闸门：脚本停下 2.5 秒，让 Python 从外面改文件
        "MYVK_EDITOR_CURVED_SCRIPT": "sleep:2500;save;saveforce;dump",
        "MYVK_EDITOR_CURVED_DUMP": ctx.out("h"),
    }, wait_s=180.0,
        ready_needle="CURVED-SLEEP ms=",
        on_ready=lambda: touch_outside(asset))
    blocks = all_of(BLOCK_RE, text)
    # 同一句警告会出现**两次**：一次是 VK_LOG_WARN 的原始行，一次是日志面板
    # 把这句话连引号一起回显（`..."reason=stale"`）。所以按"去掉尾引号后的
    # reason 集合"判，而不是数行数 —— 数行数的版本会在日志格式微调时假失败。
    reasons = set(b.group(2).rstrip('"') for b in blocks)
    check("H1 文件被外部改过 → Save 被拒（reason=stale，且只因为这一条）",
          reasons == {"stale"},
          "%s" % [b.groups() for b in blocks])
    m = last(SAVE_RE, text)
    check("H2 Save anyway（force=1）写得进去", m is not None and m.group(3) == "1",
          "got=%s" % (groups(m),))
    on_disk = dump_json(asset)
    check("H3 盘上是编辑器那一份（外面写的标记已经被覆盖掉）",
          "touched" not in json.dumps(on_disk, ensure_ascii=False) and
          len(on_disk["camera"]) == EXPECT_KEYS,
          "keys=%d" % len(on_disk["camera"]))


def touch_outside(path):
    """模拟"另一个编辑器把文件存了"。

    必须**真的重写一遍**，不能只 `touch`：`save()` 比的是
    `std::filesystem::last_write_time`，内容一样但时间戳没变的话，那条
    `reason=stale` 就永远不出现，用例会以"护栏没生效"的假象失败。

    写进去的 `"touched"` 是个**标记**，不是数据：编辑器 `saveforce` 之后
    它必须消失（H3 就是靠这一点确认盘上落的是编辑器那一份，而不是外面那份）。
    """
    st0 = os.stat(path)
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    doc["touched"] = "by-another-editor"
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
    st1 = os.stat(path)
    if st1.st_mtime_ns == st0.st_mtime_ns:
        # 理论上到不了这儿（写盘必然改 mtime），但真到了就别让用例静默跑偏。
        os.utime(path, ns=(st1.st_atime_ns, st0.st_mtime_ns + 10 ** 9))
        print("[WARN] 重写文件后 mtime 没变，已手工推后 1s")


# ================================================================== I 布局持久化
def case_i(ctx):
    """关掉重开，面板自己回来。

    ⚠ 这一条**不能**带 MYVK_FRAMES / MYVK_NO_WINDOW_SAVE：那两个一存在，
      `saveEditorLayout()` 直接 return（见 EditorLayoutIO.cpp），落盘永远
      不发生，于是测的就成了"一个不写盘的功能为什么不写盘"。
    """
    layout = os.path.join(ctx.tmp, "editor_layout.json")
    prefs = os.path.join(ctx.tmp, "editor_prefs.json")
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_GRID"] = "0"
    env["MYVK_EDITOR_LAYOUT"] = layout
    env["MYVK_EDITOR_PREFS_PATH"] = prefs
    env["MYVK_EDITOR_CURVED"] = ASSET

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
        hwnd = wait_window("Venn Editor", 60.0)
        if not hwnd:
            check("I1 编辑器窗口起来了", False, "未找到窗口")
            return
        ok = wait_line(tap, "CURVED-WIN=", 40.0)
        check("I1 第一程：面板画出来了", ok, "")
        time.sleep(1.0)
        # 优雅关闭（走 onShutdown → 落盘）
        from capture_window import user32
        user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
        try:
            proc.wait(timeout=40)
        except subprocess.TimeoutExpired:
            proc.kill()
            check("I2 优雅退出", False, "超时（没走 onShutdown？）")
            return
    finally:
        if proc.poll() is None:
            proc.kill()

    check("I2 退出码 0（走了 onShutdown）", proc.returncode == 0,
          "rc=%s" % proc.returncode)
    if not check("I3 布局文件写出来了", os.path.exists(layout), layout):
        return
    with open(layout, "r", encoding="utf-8") as f:
        doc = json.load(f)
    panels = {p["id"]: p["open"] for p in doc.get("panels", [])}
    check("I4 布局里记着 curveed 是打开的", panels.get("curveed") is True,
          "curveed=%s" % panels.get("curveed"))

    # ---- 第二程：只带布局，不带 MYVK_EDITOR_CURVED ----
    text = run_editor({"MYVK_EDITOR_LAYOUT": layout})
    check("I5 第二程：面板自己回来了（CURVED-WIN 又出现）",
          "CURVED-WIN=" in text, "")
    check("I6 第二程：空手开窗（path=(none)，等作者填路径）",
          "CURVED-OPEN path=(none) ok=0" in text, "")


def wait_window(prefix, timeout):
    from capture_window import find_window
    t0 = time.time()
    while time.time() - t0 < timeout:
        hwnd = find_window(prefix, timeout=1.0)
        if hwnd:
            return hwnd
        time.sleep(0.5)
    return None


def wait_line(tap, needle, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if tap.has_line(needle):
            return True
        time.sleep(0.05)
    return False


# ================================================================== main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只跑某个用例（a/b/c/d/e/f/g/h/i）")
    ap.add_argument("--keep", action="store_true", help="保留临时目录")
    ap.add_argument("--dump", default="", help="把 A 用例的完整日志写到这个文件")
    args = ap.parse_args()

    if not os.path.isfile(EDITOR_EXE):
        print("找不到 %s —— 先编译 VennEditor" % EDITOR_EXE)
        return 2

    tmp = tempfile.mkdtemp(prefix="venn_curved_")
    ctx = Ctx(tmp)
    only = args.only.lower()
    try:
        if only in ("", "a"):
            case_a(args.dump)
        if only in ("", "b"):
            case_b(ctx)
        if only in ("", "c"):
            case_c(ctx)
        if only in ("", "d"):
            case_d(ctx)
        if only in ("", "e"):
            case_e(ctx)
        if only in ("", "f"):
            case_f(ctx)
        if only in ("", "g"):
            case_g(ctx)
        if only in ("", "h"):
            case_h(ctx)
        if only in ("", "i"):
            case_i(ctx)
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
    # ⚠ 最后一行必须是 `结果: PASS` / `结果: FAIL` —— run_regression.py 的
    #   OK_RE 只认那几种写法。
    if n_pass == len(results):
        print("结果: PASS")
        return 0
    print("结果: FAIL: %d 项" % (len(results) - n_pass))
    return 1


if __name__ == "__main__":
    sys.exit(main())
