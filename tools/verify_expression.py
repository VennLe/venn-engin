#!/usr/bin/env python3
# ============================================================
# verify_expression —— B6 表情预设 + 眨眼/口型 的端到端验收
#
# ---------------------------- 这一刀在验什么 ----------------------------
# `--selftest expression` 已经把**数值内核**验穿了（眨眼曲线、解析、往返、
# 驱动只写自有通道）。但内核对不对，与"用户点得到、场景存得下"是两件事：
#
#   ① 场景 JSON 里的 `expression` 块能不能读成组件（`SCENE-EXPR ok=1`）
#   ② Inspector 上有没有那一段（锚点全在 = 控件真的画出来了）
#   ③ 控制台命令与面板是不是同一条路（`EXPR-SET`）
#   ④ **进 Play 之后驱动真的在跑**（owned>0、blinkCount>0）
#   ⑤ 存盘只写非默认值（老资产不许长出脏键）
#
# ④ 是这一刀不能省的：内核自检里是我手动调 `tickExpressions`，
#    "Play 每帧会不会调它"在那条路径上一行都没测到。
#
# ---------------------------- 为什么不做像素判据 ----------------------------
# 表情的价值在"眼睛能不能看出这张脸在笑"，而一套 blend shape 里单根滑条
# 推到 0.6 通常只改几十个像素 —— 那种量级在 1080p 里低于噪声，判据会变成
# 掷骰子。所以这里钉的是**数据**（权重写没写、写了哪几根），像素留给
# `verify_morph_gpu` 那条已经稳定的链去守。
#
# 用法：python tools/verify_expression.py
# ============================================================

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                   # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors           # noqa: E402
from verify_asset_drag import force_foreground                   # noqa: E402
import safe_fs                                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")
SCENE_DIR = os.path.join(BIN, "assets", "scenes")
SIZE = "1400x900"

MODEL = "models/characters/alicia.fbx"
PRESET = "expressions/alicia_demo.expr.json"
ENTITY = "Face"

_n = 0
_bad = 0


def check(name, ok, detail=""):
    global _n, _bad
    _n += 1
    if not ok:
        _bad += 1
    print("CHECK %s %s%s" % (name, "OK" if ok else "FAIL",
                             (" (" + detail + ")") if detail else ""))


# ---------------------------------------------------------------- 夹具

def write_scene(name, expression, morph_weights=None):
    """造一个"一张脸"的场景。

    `expression` 传 None = 不写这个块（用来验"老场景不建组件"）。
    """
    os.makedirs(SCENE_DIR, exist_ok=True)
    ent = {
        "name": ENTITY,
        "castShadow": True,
        "material": None,
        # mesh 留空、用 skinned 承载模型：与 `assets/scenes/anime_demo.json`
        # 同一个形状（模型由 skinned/morph 块的 kind+path 解析）。
        "mesh": None,
        "parent": -1,
        "visible": True,
        "transform": {"position": [0.0, 0.0, 0.0],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "skinned": {"kind": "fbx", "path": MODEL, "modelKey": MODEL,
                    "clipName": "", "speed": 1.0, "playing": False,
                    "loop": True, "time": 0.0},
        "morph": {"kind": "fbx", "path": MODEL, "modelKey": MODEL},
    }
    if morph_weights:
        ent["morph"]["weights"] = morph_weights
    if expression is not None:
        ent["expression"] = expression

    scene = {
        "version": 2,
        "generator": "verify_expression.py",
        "camera": {"target": [0.0, 0.0, 0.85], "yaw": -1.5708, "pitch": 0.02,
                   "distance": 1.9, "fov": 50.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": False},
        "entities": [ent],
    }
    path = os.path.join(SCENE_DIR, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)
    return "scenes/" + name


# ---------------------------------------------------------------- 运行

def snap(tap):
    with tap.lock:
        return list(tap.lines)


def run_editor(scene, console_cmd, extra_env=None, cmd_frame=70,
               wait_for="SCENE-EXPR", timeout=60.0):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_NO_WINDOW_SAVE"] = "1"     # 别把夹具窗口尺寸写回全局布局
    env["MYVK_EDITOR_NO_HUD"] = "1"      # HUD 的 FPS 每次不同，会污染断言之外的图
    env["MYVK_LOG_RECTS"] = "1"
    # ⚠⚠ Inspector **只画选中的实体**。不选中时所有 `INS-*` 锚点一条都不会出现,
    #   而日志里没有任何"你没选中"的提示 —— 这是 B 组第一版全红的唯一原因
    #   （8 个锚点 + 命令链路一起挂，看起来像"UI 没做"）。
    env["MYVK_EDITOR_SELECT"] = ENTITY
    # ⚠⚠ 还要钉住**页签**：Expression 段长在 `drawTabAnimation` 里（它与
    #   Morph Targets 一起，紧挨在 Animator 之前 —— 用户看完滑条就该看到
    #   "这套脸叫什么表情"）。而 `drawTabAll` **不是**融合视图，它只调
    #   transform / visibility / mesh / lights / script / sequence 那几段。
    #   不钉这一句，8 个 `INS-EXPR-*` 锚点一条都不会出现。
    env["MYVK_EDITOR_INSP_TAB"] = "animation"
    if console_cmd:
        env["MYVK_EDITOR_CONSOLE_CMD"] = console_cmd
        env["MYVK_EDITOR_CONSOLE_CMD_FRAME"] = str(cmd_frame)
    if extra_env:
        env.update(extra_env)

    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", timeout=timeout)
    if not hwnd:
        _close(proc)
        return [], []
    user32.ShowWindow(hwnd, 9)
    time.sleep(1.0)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    force_foreground(hwnd)

    # 等到关键锚点出现（场景读完了 / 组件建好了）
    deadline = time.time() + 40.0
    while time.time() < deadline:
        lines = snap(tap)
        if wait_for is None or any(wait_for in ln for ln in lines):
            break
        time.sleep(0.2)
    # 命令在第 N 帧执行；再留一点给 Play 里的驱动跑几秒
    time.sleep(6.0)
    lines = snap(tap)
    _close(proc)
    return lines, snap(tap)


def _close(proc):
    try:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
    except Exception:                                          # noqa: BLE001
        pass


def first(regex, lines):
    for ln in lines:
        m = regex.search(ln)
        if m:
            return m
    return None


def all_of(regex, lines):
    out = []
    for ln in lines:
        m = regex.search(ln)
        if m:
            out.append(m)
    return out


# ---------------------------------------------------------------- 正则

RE_TOTAL = re.compile(r"TOTAL pass=(\d+) fail=(\d+)")
RE_SCENE_EXPR = re.compile(
    r"SCENE-EXPR entity=\d+ preset=(\S+) current=(\S+) intensity=([\d.]+) "
    r"blink=(\d) lip=(\d) ok=(\d)")
RE_LIST = re.compile(r"EXPR-LIST entity=\d+ n=(\d+) (.*)$")
RE_SET = re.compile(r"EXPR-SET entity=\d+ name=\S+ preset=(\S+) intensity=([\d.]+)")
RE_SET_FAIL = re.compile(r"EXPR-SET ok=0 name=(\S+) err=(.*)$")
RE_STATE = re.compile(
    r"EXPR-STATE entity=\d+ preset=(\S+) intensity=([\d.]+) blink=(\d) "
    r"phase=(-?[\d.]+) timer=(-?[\d.]+) count=(\d+) lip=(\d) level=([\d.]+) "
    r"owned=(\d+)")
RE_BLINK_REQ = re.compile(r"EXPR-BLINK-REQ entity=\d+ enabled=(\d)")
ANCHORS = ("INS-EXPR-HEADER", "INS-EXPR-PATH", "INS-EXPR-PRESET",
           "INS-EXPR-INTENSITY", "INS-EXPR-BLINK", "INS-EXPR-BLINK-NOW",
           "INS-EXPR-LIP", "INS-EXPR-LEVEL")


# ---------------------------------------------------------------- 组 A

def group_a_selftest():
    print("\n[A] 数值内核（--selftest expression）")
    if not os.path.exists(GAME):
        check("A0 Game.exe 存在", False, GAME)
        return
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    try:
        out = subprocess.run([GAME, "--selftest", "expression"], cwd=BIN,
                             env=env, capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=120)
    except subprocess.TimeoutExpired:
        check("A1 自检跑完", False, "timeout")
        return
    m = RE_TOTAL.search(out.stdout)
    if not m:
        check("A1 自检打出了 TOTAL 行", False,
              (out.stdout or "")[-200:].replace("\n", " | "))
        return
    ok = int(m.group(1))
    bad = int(m.group(2))
    check("A1 自检全绿（fail=0）", bad == 0, "pass=%d fail=%d" % (ok, bad))
    check("A2 自检条数够（>= 30）", ok >= 30, "pass=%d" % ok)
    check("A3 退出码 = 0", out.returncode == 0, "rc=%d" % out.returncode)
    # 关键组的标题必须在输出里 —— 只数总数的话，"E 组整段没跑"也能凑够 30。
    for tag in ("[A] 眨眼曲线", "[B] 预设解析", "[C] 写回 / 读回往返",
                "[D] 与 rig 的适配度", "[E] 驱动器"):
        check("A4 有 %s 这一段" % tag, tag in out.stdout, "")


# ---------------------------------------------------------------- 组 B

def group_b_editor():
    print("\n[B] 编辑器：读场景 / UI 面 / 命令（不进 Play）")
    scene = write_scene("_expr_edit.json", {
        "preset": PRESET,
        "current": "calm",
        "intensity": 0.6,
        # ⚠ alicia 的通道名全是 `target_N`（美术没改名），所以眨眼通道
        #   必须显式指到一根**真实存在**的滑条上：默认的 "blink" 在这个 rig
        #   上找不到 → 驱动什么都不写（owned=0），而那不是 bug，是"名字对不上"。
        "blink": {"enabled": True, "interval": 0.2, "channel": "target_1"},
    })
    lines, _ = run_editor(
        scene,
        "expr.list %s; expr.state %s; expr.setpreset surprise %s; "
        "expr.state %s; expr.setpreset nope %s; expr.blink %s; scene.save"
        % (ENTITY, ENTITY, ENTITY, ENTITY, ENTITY, ENTITY))

    m = first(RE_SCENE_EXPR, lines)
    check("B1 场景里的 expression 块被读成组件",
          bool(m) and m.group(6) == "1",
          m.group(0) if m else "no SCENE-EXPR")
    check("B2 读到 preset 路径与当前表情",
          bool(m) and m.group(1) == PRESET and m.group(2) == "calm",
          ("%s / %s" % (m.group(1), m.group(2))) if m else "-")
    check("B3 强度读对了（0.60）",
          bool(m) and abs(float(m.group(3)) - 0.6) < 1e-6,
          m.group(3) if m else "-")
    check("B4 blink=1 / lip=0（与夹具一致）",
          bool(m) and m.group(4) == "1" and m.group(5) == "0",
          ("blink=%s lip=%s" % (m.group(4), m.group(5))) if m else "-")

    for a in ANCHORS:
        check("B5 锚点在：%s" % a, ('"%s"' % a) in "\n".join(lines) or
              a in "\n".join(lines), "")

    ml = first(RE_LIST, lines)
    names = ml.group(2).split() if ml else []
    check("B6 EXPR-LIST 列出 3 个预设", bool(ml) and ml.group(1) == "3",
          ("n=%s [%s]" % (ml.group(1), " ".join(names))) if ml else "no LIST")
    check("B7 预设名就是资产里的那三个",
          names == ["calm", "smile", "surprise"], " ".join(names))

    states = all_of(RE_STATE, lines)
    check("B8 EXPR-STATE 打了两次（切之前 / 切之后）", len(states) >= 2,
          "n=%d" % len(states))
    if len(states) >= 2:
        check("B9 第一次 state 是 calm",
              states[0].group(1) == "calm", states[0].group(1))
        check("B10 切完之后 state 是 surprise",
              states[1].group(1) == "surprise", states[1].group(1))

    ms = first(RE_SET, lines)
    check("B11 切预设打了 EXPR-SET（与面板同一条路）",
          bool(ms) and ms.group(1) == "surprise",
          ms.group(0) if ms else "no EXPR-SET")
    mf = first(RE_SET_FAIL, lines)
    check("B12 不存在的预设名**报错**（不是静默成功）",
          bool(mf) and "unknown preset" in mf.group(2),
          mf.group(2) if mf else "no ok=0")
    check("B13 expr.blink 记录了请求",
          bool(first(RE_BLINK_REQ, lines)), "")

    # ---- 存盘：只写非默认 ----
    path = os.path.join(SCENE_DIR, "_expr_edit.json")
    saved = None
    try:
        with open(path, encoding="utf-8") as f:
            saved = json.load(f)
    except Exception as e:                                     # noqa: BLE001
        check("B14 存盘可读", False, repr(e))
        return
    ent = next((e for e in saved.get("entities", [])
                if e.get("name") == ENTITY), None)
    ex = (ent or {}).get("expression")
    check("B15 存盘里还有 expression 块", isinstance(ex, dict),
          str(type(ex).__name__))
    if isinstance(ex, dict):
        check("B16 存的是切换后的表情（surprise）",
              ex.get("current") == "surprise", str(ex.get("current")))
        check("B17 强度落盘（0.6）",
              abs(float(ex.get("intensity", -1)) - 0.6) < 1e-6,
              str(ex.get("intensity")))
        check("B18 改过的 blink 落盘（interval=0.2）",
              abs(float(ex.get("blink", {}).get("interval", -1)) - 0.2) < 1e-6,
              str(ex.get("blink")))
        # lipSync 全程是默认值 → **不许**长出这个键（老资产一存就脏）
        check("B19 默认的 lipSync 不落盘", "lipSync" not in ex,
              str(sorted(ex.keys())))


# ---------------------------------------------------------------- 组 C

def group_c_play():
    print("\n[C] 进 Play：驱动真的每帧在跑")
    scene = write_scene("_expr_play.json", {
        "preset": PRESET,
        "current": "smile",
        "intensity": 1.0,
        "blink": {"enabled": True, "interval": 0.2, "channel": "target_1"},
    })
    lines, _ = run_editor(
        scene, "expr.state %s" % ENTITY,
        extra_env={"MYVK_EDITOR_PLAY": "1", "MYVK_EXPR_LOG": "1"},
        cmd_frame=140)

    states = all_of(RE_STATE, lines)
    check("C1 Play 里打出了 EXPR-STATE", len(states) >= 1,
          "n=%d" % len(states))
    if not states:
        return
    s = states[-1]
    check("C2 当前表情 = smile", s.group(1) == "smile", s.group(1))
    # 核心：驱动真的写了权重（owned>0）。edit 态下这里是 0 —— 因为驱动
    # 只在 Play 跑。所以这一条同时证明了"tick 接进了 Play"。
    owned = int(s.group(9))
    check("C3 驱动拥有通道 > 0（Play 里 tick 真的在跑）", owned > 0,
          "owned=%d" % owned)
    count = int(s.group(6))
    check("C4 自动眨眼跑过（interval=0.2，2 秒内至少 3 次）", count >= 3,
          "count=%d" % count)
    check("C5 blink=1 lip=0（配置没被 Play 改掉）",
          s.group(3) == "1" and s.group(7) == "0",
          "blink=%s lip=%s" % (s.group(3), s.group(7)))
    blinks = [ln for ln in lines if "EXPR-BLINK name=" in ln]
    check("C6 MYVK_EXPR_LOG 让自动眨眼打了日志", len(blinks) >= 1,
          "lines=%d" % len(blinks))


# ---------------------------------------------------------------- 组 D

def group_d_no_expression():
    print("\n[D] 老场景（没有 expression 块）不许建组件")
    scene = write_scene("_expr_none.json", None)
    lines, _ = run_editor(scene, "expr.state %s" % ENTITY,
                          wait_for="SCENE-MORPH", cmd_frame=80)
    check("D1 没有 SCENE-EXPR（不建组件）",
          first(RE_SCENE_EXPR, lines) is None, "")
    m = first(re.compile(r"EXPR-STATE entity=\d+ ok=0 reason=no-component"),
              lines)
    check("D2 expr.state 如实报没有组件", bool(m), "")


# ---------------------------------------------------------------- main

def main():
    print("=" * 72)
    print("B6 表情预设 + 眨眼/口型 验收")
    print("=" * 72)
    kill_stale_editors()
    try:
        group_a_selftest()
        group_b_editor()
        group_c_play()
        group_d_no_expression()
    finally:
        kill_stale_editors()
        for n in ("_expr_edit.json", "_expr_play.json", "_expr_none.json"):
            p = os.path.join(SCENE_DIR, n)
            if os.path.exists(p):
                safe_fs.safe_remove(p)

    print("\n%d 通过 / %d 失败" % (_n - _bad, _bad))
    print("\n结果: %s" % ("PASS" if _bad == 0 else "FAIL: %d 条" % _bad))
    return 0 if _bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
