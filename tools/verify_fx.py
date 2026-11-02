#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P1 特效系统验收（A~F）。

A  三组内核自检（FX-ASSET / FX-POOL / FX-EVENT）
B  面板锚点 + fx.state / fx.play / fx.stopall（编辑态，不需要 Play）
C  池满被丢弃 —— "某些命中没火花"的唯一可观察证据
D  脚本动词真的接通（Play 里 playEffect 返回 id > 0）
E  动画事件 → 特效（P1-3 端到端，文档口径 `Hit|fx/hit_spark`）
F  顿帧 / 闪白的否定测试（真实 dt、取 max 不累加、90 帧内恢复）

⚠ 判据口径（为什么这么选）：
   · `FX-PLAY ... id=N` 里的 **id > 0** 才是"真的占到槽"的证据。
     "命令没报错"完全不能证明播了（资产缺失时也返回一个错误串）。
   · 池满必须断言 `dropped` 发生变化。画面里**永远**不会出现
     "这里本该有一个特效"的证据 —— 只有计数能说。
   · D 组判据是**脚本自己的 print**：`playEffect` 的返回值只有脚本
     看得到，用日志转述会把"宿主没接"误判成"接了但没打日志"。
   · E 组只放**一条**事件且 `loop=False`：循环时"多一帧 / 少一次 /
     每帧都发"这三种错误在序列里长得几乎一样。
   · F 组三个数缺一不可：`total`（抓累加）、`elapsed`（抓 scaled dt）、
     `frames`（抓 timeScale=0 的死锁）。顿帧的失败形态在画面上都像
     "有点卡"，只有这三个数能区分。

用法：
    python tools/verify_fx.py            # 全量 A~F
    python tools/verify_fx.py --only BC  # 只跑 B、C
"""

import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_flags as F          # 复用 run_editor / snap / check / 夹具路径

# ---------------------------------------------------------------- 常量

FX_HIT = "fx/hit_spark.veffect"
FX_SLASH = "fx/slash_a.veffect"

FX_PROBE_SCENE_REL = "scenes/_fx_probe.json"
FX_PROBE_VKS_REL = "scripts/_fx_probe.vks"

BIN = F.BIN
ASSETS = F.ASSETS
SRC_SCENE = F.SRC_SCENE


def check(name, ok, detail=""):
    F.check(name, ok, detail)


def lines_of(tap):
    return F.snap(tap)


# ---------------------------------------------------------------- 夹具

# ⚠ 脚本自己 `print` 出 `playEffect` 的返回值 —— 这是唯一能区分
#   "宿主动作没接"（返回 0 + `not available`）与"播了"的判据。
FX_PROBE_SRC = """\
# _fx_probe.vks —— P1 验收夹具（**不是资产**，跑完由 verify_fx.py 删掉）
#
# 只验一件事：脚本能不能让宿主出特效，且拿得到 id。
# ⚠ 不写 shake / hitstop：那两个没有返回值，判据只能靠日志，
#   会把"没接线"和"没打日志"混在一起。
let fxid = 0

on start {
    fxid = playEffect("fx/hit_spark.veffect")
    print(fxid)
}
"""


def write_probe():
    """夹具：一个挂脚本的实体（从 `script_demo.json` 派生）。

    ⚠ 场景**必须派生**而不是手写：实体块里少一个字段加载就静默丢实体，
    表现成"脚本一行都没跑"（与 verify_flags 同一个坑）。
    """
    os.makedirs(os.path.dirname(F.PROBE_VKS), exist_ok=True)
    with open(F.PROBE_VKS.replace("_flags_probe", "_fx_probe"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(FX_PROBE_SRC)

    with open(SRC_SCENE, "r", encoding="utf-8") as f:
        demo = json.load(f)
    probe = None
    for e in demo.get("entities", []):
        if isinstance(e, dict) and isinstance(e.get("script"), dict):
            probe = json.loads(json.dumps(e))
            break
    if probe is None:
        raise RuntimeError("script_demo.json 里找不到带 script 的实体")
    probe["name"] = "FxProbe"
    probe["script"]["path"] = FX_PROBE_VKS_REL
    probe["transform"]["position"] = [0.0, 0.0, 0.0]

    scene = {
        "version": demo.get("version", 1),
        "generator": "verify_fx.py",
        "camera": demo.get("camera", {}),
        "light": demo.get("light", {}),
        "physics": demo.get("physics", {}),
        "entities": [probe],
    }
    # ⚠⚠ 夹具落 **assets 根**而不是 `build/bin/` 根：`--scene` 收的是
    #   相对 assets 的路径（`MYVK_EDITOR_SCENE=scenes/xxx.json`，cwd=BIN）。
    #   写到 `build/bin/scenes/` 的话编辑器**不会报错**，只会开个空场景
    #   → 表现成"脚本一行都没跑"（与 verify_flags 同一个坑）。
    out = os.path.join(ASSETS, FX_PROBE_SCENE_REL)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)


def remove_probe():
    for p in (os.path.join(ASSETS, FX_PROBE_SCENE_REL),
              os.path.join(ASSETS, FX_PROBE_VKS_REL)):
        try:
            if os.path.isfile(p):
                os.remove(p)
        except Exception:                                      # noqa: BLE001
            pass


# ---------------------------------------------------------------- A

def group_a():
    print("\n[A] 三组内核自检（资产解析 / 池调度 / 事件路由）")
    out = F.run_game_selftest("project")

    for tag, want in (("FX-ASSET", 18), ("FX-POOL", 12), ("FX-EVENT", 11)):
        m = [l for l in out.splitlines() if l.startswith(tag + "-TOTAL")]
        check("[A] %s-TOTAL 出现" % tag, len(m) == 1, m[:1])
        if m:
            check("[A] %s 全通过（%d 条）" % (tag, want),
                  ("pass=%d fail=0" % want) in m[0], m[0])
    check("[A] 工程自检总闸失败数为 0",
          "PROJECT-ALL-TOTAL failures=0" in out,
          [l for l in out.splitlines() if "PROJECT-ALL-TOTAL" in l][:1])

    # 反向判据：四组自检的行数必须都出现过（漏接一组会少一整块）
    for tag in ("FX-ASSET PASS", "FX-POOL PASS", "FX-EVENT PASS"):
        n = sum(1 for l in out.splitlines() if l.startswith(tag))
        check("[A] %s 行数 > 0" % tag, n > 0, "n=%d" % n)


# ---------------------------------------------------------------- B

def group_b():
    print("\n[B] 面板锚点 + fx.state / fx.play / fx.stopall")
    # ⚠ 等**最后一条命令自己的日志**，不等面板锚点 —— 锚点第 3~4 帧就有，
    #   命令要等第 40 帧（panel_gap_review 的 A4 那一轮栽过）。
    cmd = ("fx.state; fx.play %s; fx.state; fx.stopall" % FX_HIT)
    proc, hwnd, tap = F.run_editor(panel="fx", console_cmd=cmd,
                                   wait_tag="FX-RECT",
                                   wait_for="FX-STOPALL active=",
                                   wait_secs=60.0)
    check("[B] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        return

    lines = lines_of(tap)
    F.close(proc)
    joined = "\n".join(lines)

    # 面板真的画了（8 个锚点里的 4 个关键位）
    for tag in ("FX-RECT", "FX-STATE", "FX-PLAY-hit_spark", "FX-STOPALL"):
        check("[B] 锚点 %s 有矩形" % tag, tag + "=" in joined)

    # 池真的建了宿主（8 = 默认预算）
    check("[B] 池建了 8 个宿主", "FX-POOL hosts=8" in joined,
          [l for l in lines if "FX-POOL" in l][:2])

    # 第一次 fx.state：active=0（还没播）
    states = [l for l in lines if "FX-STATE fx budget=" in l]
    check("[B] fx.state 至少两条", len(states) >= 2, states[:2])

    # fx.play 真的占到槽：id > 0
    plays = [l for l in lines if "FX-PLAY path=" in l]
    check("[B] fx.play 打了一行", len(plays) == 1, plays[:2])
    if plays:
        check("[B] fx.play 拿到 id=1", "id=1" in plays[0], plays[0])
        # ⚠ 这一条是对**动画事件**那条路的旁证：`active=1` 说明宿主已被占。
        check("[B] fx.play 之后至少 1 个槽在用", "active=1" in plays[0], plays[0])

    # fx.stopall 之后必须归零
    stops = [l for l in lines if "FX-STOPALL active=" in l]
    check("[B] fx.stopall 把活跃数清零", any("active=0" in l for l in stops),
          stops[:2])


# ---------------------------------------------------------------- C

def group_c():
    print("\n[C] 池满 → 新的被丢弃（确定性规则）")
    # ⚠ 同一帧连发 11 次：每层占一个槽、池只有 8 个 → 恰好 8 成功 3 丢弃。
    #   分开帧发就测不出丢弃（第一波 0.5 秒内就回收了）。
    plays = "; ".join(["fx.play %s" % FX_HIT] * 11)
    cmd = "@40 %s; @60 fx.state" % plays
    proc, hwnd, tap = F.run_editor(panel="fx", console_cmd=cmd,
                                   wait_tag="FX-RECT",
                                   wait_for="FX-STATE fx budget=",
                                   wait_secs=70.0)
    check("[C] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        return

    lines = lines_of(tap)
    F.close(proc)
    joined = "\n".join(lines)

    ok_plays = [l for l in lines if "FX-PLAY path=" in l and "id=0" not in l]
    bad_plays = [l for l in lines if "FX-PLAY path=" in l and "id=0" in l]
    check("[C] 8 次占到槽", len(ok_plays) == 8,
          "got=%d" % len(ok_plays))
    check("[C] 3 次被丢弃", len(bad_plays) == 3,
          "got=%d" % len(bad_plays))

    # dropped 计数必须真的动 —— 这是面板上唯一能看出"少了特效"的地方
    check("[C] dropped 计数变成 3", "dropped=3" in joined,
          [l for l in lines if "dropped=" in l][-2:])


# ---------------------------------------------------------------- D

def group_d():
    print("\n[D] 脚本动词接通（Play 里 playEffect 返回 id > 0）")
    write_probe()
    try:
        # 进 Play 才会跑脚本的 on start。`MYVK_LOG_SCRIPT_PRINT=1` 把
        # 脚本的 print 落进日志 —— 判据就是那行数字。
        # ⚠ `MYVK_LOG_FX=1`：脚本走的是 `EffectService::play` 那条路，
        #   而它默认不打日志（每次命中一行会淹掉日志）。必须显式打开，
        #   否则判据变成"没打日志"= 看起来像"没播"。
        proc, hwnd, tap = F.run_editor(
            panel="fx", console_cmd=None, wait_tag="FX-RECT",
            wait_for="FX-POOL hosts=", play=True,
            scene=FX_PROBE_SCENE_REL, wait_secs=70.0,
            env_extra={"MYVK_LOG_FX": "1"})
        check("[D] 编辑器窗口起来了", bool(hwnd))
        if not hwnd:
            F.close(proc)
            return

        # 给 on start 一点时间
        deadline = time.time() + 30.0
        hit = False
        while time.time() < deadline:
            if any("FX-PLAY path=fx/hit_spark.veffect id=" in l
                   for l in lines_of(tap)):
                hit = True
                break
            time.sleep(0.2)
        lines = lines_of(tap)
        F.close(proc)
        joined = "\n".join(lines)

        # ⚠ 反向判据：宿主没接时脚本会报这句，而**不崩不中断**，
        #   剧本静默走 else（A4 的 setFlag 同一个教训）。
        check("[D] 没有 'not available in this host'",
              "playEffect() is not available" not in joined,
              [l for l in lines if "not available" in l][:2])
        check("[D] 脚本调 playEffect 真的播了", hit,
              [l for l in lines if "FX-PLAY" in l][:3])
        # id > 0 才算真的占到槽。`src=service` 把"脚本触发的"与
        # "控制台 fx.play 打的"区分开（两者都在 FX-PLAY 这个前缀下）。
        svc_plays = [l for l in lines if "FX-PLAY path=fx/hit_spark.veffect" in l
                     and "src=service" in l]
        check("[D] 服务侧记录了这次播放", len(svc_plays) >= 1, svc_plays[:3])
        check("[D] 脚本拿到的 id > 0",
              any("id=0" not in l for l in svc_plays), svc_plays[:3])
    finally:
        remove_probe()


# ---------------------------------------------------------------- E

def build_anim_variant(tag, events, loop=False, dest_dir=None, file_name=None):
    """带蒙皮实体的场景变体：`animator` 段加上事件表。

    ⚠⚠ **不能手写 `skinned` 实体**：手写的那个在视口里一像素都不画，而日志
      会骗你说一切正常（`verify_anim_layers.py` 文件头的坑 1）。所以复用
      `verify_anim_layers` 用 `MYVK_EDITOR_IMPORT` 烤好的 9 实体模板，
      只改 `animator` 一段 —— 与 B1/B2 同一条路。

    `dest_dir` / `file_name`：G 组要喂 `Game.exe --scene <相对 assets 的路径>`，
    只能落在 `build/bin/assets/scenes/` 下（`_venn_verify` 在资产根外面，
    `--scene` 找不到**不会报错**，只会开个空场景）。
    """
    from verify_anim_layers import OUT_DIR, TPL, ensure_template

    ensure_template()
    with open(TPL, encoding="utf-8") as f:
        doc = json.load(f)
    # ... 以下照旧，只是输出目录可覆盖

    root = None
    for e in doc.get("entities", []):
        if isinstance(e, dict) and e.get("skinned"):
            root = e
            break
    if root is None:
        raise RuntimeError("模板里没有蒙皮实体（先跑一次 verify_anim_layers）")

    # 底座与 animator 两份都要对齐：`skinned` 那份管渲染，`animator` 那份是
    # **权威**（`playing` 只在 animator 上）。
    root["skinned"]["clipName"] = "Walk"
    root["skinned"]["time"] = 0.0
    root["skinned"]["playing"] = False
    root["skinned"]["loop"] = loop
    root["animator"] = {
        "defaultClip": "Walk",
        "controllerPath": "",
        "time": 0.0,
        "speed": 1.0,
        "playing": True,      # ⚠ 事件要时间真的在走
        "loop": loop,
        "rootMotion": 0,
        "events": [{
            "clip": "Walk",
            "events": [{"t": t, "kind": k, "payload": p} if p else
                       {"t": t, "kind": k} for (t, k, p) in events],
        }],
    }
    doc["generator"] = "verify_fx"
    out_dir = dest_dir or OUT_DIR
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, file_name or ("fx_anim_%s.json" % tag))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False)
    return path


def run_anim_play(scene, extra_env=None, want=1, timeout=45.0):
    """跑一轮 Play，收日志（判据是 `ANIM-EVENT` 与 `FX-*` 两类行）。"""
    from verify_anim_layers import CAM, DEFAULT_EXE, _base_env, _close
    import subprocess
    from capture_window import find_window, user32
    from verify_add_menu import LogTap, kill_stale_editors, wait_rect

    env = _base_env()
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
    env["MYVK_EDITOR_PLAY"] = "1"
    env["MYVK_LOG_FX"] = "1"
    if extra_env:
        env.update({k: str(v) for k, v in extra_env.items()})

    kill_stale_editors()
    proc = subprocess.Popen([DEFAULT_EXE], cwd=os.path.dirname(DEFAULT_EXE),
                            env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor")
    if not hwnd:
        try:
            proc.kill()
        except Exception:                                  # noqa: BLE001
            pass
        raise RuntimeError("编辑器窗口没出现")
    try:
        user32.ShowWindow(hwnd, 9)
        wait_rect(tap, "VP-RECT", 20.0)
        end = time.time() + timeout
        while time.time() < end:
            with tap.lock:
                n = sum(1 for l in tap.lines if "ANIM-EVENT entity=" in l)
            if n >= want:
                break
            time.sleep(0.2)
        # 事件发出来之后特效还要几帧才回收，多等一点让 FX-* 落齐
        time.sleep(1.2)
        with tap.lock:
            return list(tap.lines)
    finally:
        _close(proc, hwnd)


def group_e():
    print("\n[E] 动画事件 → 特效（P1-3 端到端：文档口径 `Hit|fx/xxx`）")
    # 只放**一条**事件、`loop=False`：这样"恰好一次"才是个有意义的数。
    # 循环的话判据只能写成"每圈一次"，而"多一帧 / 少一次 / 每帧都发"
    # 这三种错误在循环里长得几乎一样。
    scene = build_anim_variant(
        "hit", [(0.2, "Hit", FX_HIT)], loop=False)
    lines = run_anim_play(scene, {"MYVK_FIXED_DT": "0.0166667"}, want=1)
    joined = "\n".join(lines)

    anim = [l for l in lines if "ANIM-EVENT entity=" in l and "kind=Hit" in l]
    check("[E] 动画事件发了且只发一次", len(anim) == 1,
          "n=%d %s" % (len(anim), anim[:2]))

    # ⚠ 关键判据：路由**认** `Hit` 这个文档口径。
    #   不认的时候不会崩、不会报错，只会打一行 "FX-EVENT ignored ...
    #   unknown kind: 'Hit'" 然后什么都不发生 —— 那行是唯一证据。
    check("[E] `Hit` 没有被当成未知事件",
          "unknown kind" not in joined and "FX-EVENT ignored" not in joined,
          [l for l in lines if "FX-EVENT ignored" in l][:2])

    disp = [l for l in lines if "FX-DISPATCH" in l]
    check("[E] 分派恰好发生一次且只出 1 个特效",
          len(disp) == 1 and "fx=1 " in disp[0] and "total=1 " in disp[0],
          disp[:2])

    plays = [l for l in lines if "FX-PLAY path=fx/hit_spark.veffect" in l]
    check("[E] 特效真的占到了宿主槽（id>0）",
          len(plays) >= 1 and "id=0" not in plays[0], plays[:2])


# ---------------------------------------------------------------- F

def group_f():
    print("\n[F] 顿帧 / 闪白：真实 dt 与「不累加」的否定测试（P1-5）")
    # 同一帧连发三次 hitstop(0.15)：
    #   · 取 max → `end total=0.150`
    #   · 累加   → `end total=0.450`（5 连击变成 0.45 秒的"卡"）
    #
    # ⚠ 用 **0.15** 而不是设计值 0.05~0.09：0.03 秒的顿帧在 30 fps 下
    #   一帧就结束了（`frames=1`），那样 `elapsed`/`frames` 两个数都退化成
    #   "跑了一帧"，测不出"跨帧用真实 dt 递减"。跨多帧之后，用 scaled dt
    #   的实现会把 0.15 拖成 0.15/0.06 = 2.5 秒 → `elapsed` 立刻红。
    cmd = ("@40 fx.hitstop 0.15; @40 fx.hitstop 0.15; @40 fx.hitstop 0.15; "
           "@40 fx.flash 0.7 0.12; @40 fx.shake 0.4")
    proc, hwnd, tap = F.run_editor(panel="fx", console_cmd=cmd,
                                   wait_tag="FX-RECT",
                                   wait_for="FX-FLASH end",
                                   play=True, wait_secs=60.0)
    check("[F] 编辑器窗口起来了", bool(hwnd))
    if not hwnd:
        F.close(proc)
        return

    lines = lines_of(tap)
    F.close(proc)
    joined = "\n".join(lines)

    begins = [l for l in lines if "FX-HITSTOP begin=" in l]
    check("[F] 顿帧被触发", len(begins) >= 1, begins[:3])

    ends = [l for l in lines if "FX-HITSTOP end " in l]
    check("[F] 顿帧会结束（不是死锁）", len(ends) >= 1, ends[:2])
    if ends:
        m = re.search(r"total=([\d.]+) elapsed=([\d.]+) frames=(\d+) reqs=(\d+)",
                      ends[0])
        check("[F] FX-HITSTOP end 行可解析", m is not None, ends[0])
        if m:
            total, elapsed, frames, reqs = (float(m.group(1)),
                                            float(m.group(2)),
                                            int(m.group(3)),
                                            int(m.group(4)))
            # 取 max 不累加：三次 0.15 必须是 0.15，不是 0.45
            check("[F] 三次请求取 max（total=0.150，不是 0.450）",
                  abs(total - 0.15) < 1e-3, "total=%.3f reqs=%d" % (total, reqs))
            # 真实 dt：用 scaled dt 的话 0.15/0.06 = 2.5 秒才走完
            check("[F] 计时器用真实 dt（elapsed < 0.60）",
                  elapsed < 0.60, "elapsed=%.3f" % elapsed)
            # 90 帧内恢复 —— 这条是"退化成 timeScale=0"的死锁护栏。
            # 下界 2：顿帧真的跨了帧（一帧就结束的话这条功能没意义）。
            check("[F] 90 帧内恢复且真的跨帧", 2 <= frames <= 90,
                  "frames=%d" % frames)

    fbeg = [l for l in lines if "FX-FLASH begin=" in l]
    check("[F] 闪白被触发（0.120 秒）",
          any("begin=0.120" in l for l in fbeg), fbeg[:2])
    check("[F] 闪白会结束（不永远盖屏）", "FX-FLASH end" in joined,
          [l for l in lines if "FX-FLASH end" in l][:2])
    check("[F] 震屏被触发", "FX-SHAKE trauma=0.400" in joined,
          [l for l in lines if "FX-SHAKE" in l][:2])


# ---------------------------------------------------------------- G

def group_g():
    print("\n[G] 出包运行时：Game.exe 里动画事件也出特效（P1 最后一环）")
    # ⚠ 这条是 P1 收口时**补的**：编辑器 Play 通了不等于游戏里有特效。
    #   运行时的分派是**另一处代码**（`RuntimeApp::dispatchAnimEventsThisFrame`），
    #   而"出包后打怪没刀光"这种情况在编辑器里怎么点都看不出来。
    rel = "scenes/_fx_rt_probe.json"
    path = build_anim_variant("rt", [(0.2, "Hit", FX_HIT)], loop=False,
                              dest_dir=os.path.join(ASSETS, "scenes"),
                              file_name="_fx_rt_probe.json")
    try:
        from verify_audio import run as run_game
        # `frames=150`：0.2 秒的事件在 60fps 下约第 12 帧发；给足余量让
        # 池推进与闪白结束都走完（不然 `FX-RT-FLASH end` 可能还没打）。
        rc, lines = run_game(F.GAME, ["--scene", rel], frames=150,
                             env_extra={"MYVK_LOG_FX": "1"}, timeout=180)
        joined = "\n".join(lines)

        check("[G] Game.exe 起来了", rc == 0, "rc=%d" % rc)

        anim = [l for l in lines if "ANIM-EVENT entity=" in l and "kind=Hit" in l]
        check("[G] 运行时里动画事件发了", len(anim) >= 1, anim[:2])

        # ⚠ 反向判据：场景没加载成功时不会有任何 ANIM-EVENT，
        #   而 `--scene` 打不开**不报错**（只会开个空场景）—— rc 是 0。
        check("[G] 场景真的加载了（不是空场景）", len(anim) >= 1,
              "n=%d rc=%d" % (len(anim), rc))

        disp = [l for l in lines if "FX-RT-DISPATCH" in l]
        check("[G] 运行时侧分派了一次且 fx=1",
              len(disp) == 1 and "total=1 " in disp[0] and "fx=1 " in disp[0],
              disp[:2])

        plays = [l for l in lines if "FX-PLAY path=fx/hit_spark.veffect" in l]
        check("[G] 运行时真的占到宿主槽（id>0）",
              len(plays) >= 1 and "id=0" not in plays[0], plays[:2])

        # `.veffect` 里的 hitstop / flash 在运行时也必须走通 —— 这是
        # "编辑器手感对、出包变样"最容易漏的一半。
        check("[G] 运行时顿帧也走真实 dt",
              any("FX-RT-HITSTOP end" in l for l in lines),
              [l for l in lines if "FX-RT-HITSTOP" in l][:2])
        check("[G] 运行时闪白会结束",
              any("FX-RT-FLASH end" in l for l in lines),
              [l for l in lines if "FX-RT-FLASH" in l][:2])
    finally:
        from safe_fs import safe_remove
        safe_remove(path)


# ---------------------------------------------------------------- main

GROUPS = {"A": group_a, "B": group_b, "C": group_c, "D": group_d,
          "E": group_e, "F": group_f, "G": group_g}


def main():
    only = None
    argv = sys.argv[1:]
    for i, a in enumerate(argv):
        if a == "--only" and i + 1 < len(argv):
            only = argv[i + 1].upper()

    print("=" * 60)
    print("P1 特效系统验收")
    print("=" * 60)

    for key, fn in GROUPS.items():
        if only and key not in only:
            continue
        fn()

    print("\n" + "=" * 60)
    if not F.fails:
        print("结果: PASS")
    else:
        print("结果: FAIL: %d 条断言未通过" % len(F.fails))
    print("=" * 60)
    return 0 if not F.fails else 1


if __name__ == "__main__":
    sys.exit(main())
