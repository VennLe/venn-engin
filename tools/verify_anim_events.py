#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""B2 动画事件（Notify）的**端到端**验收。

数值自检（`verify_anim_runtime.py` 里的 `ev_*` 那 28 条）已经证明了边界语义
—— 半开区间、跨圈拆分、换 clip 归零、帧率无关。本脚本补的是它证明不了的
三件事：

  ① **真跑起来**（编辑器 Play，真实 dt）：`ANIM-EVENT` 锚点真的出现，且
     kind / t 与写进场景的配置**逐条一致**。
  ② **落盘往返**：`scene.save` 之后 JSON 里 `animator.events` 还在、字段
     一个不少；同时**不能**出现 `eventPrev` / `eventClip`（运行时字段）。
  ③ **UI 面**：Inspector 的 Animation Events 段与事件行真的画出来了。

判据只用 **kind + t**，不用"个数"：个数与"Walk 这一段有多长"绑定，而时长
是资产里烤出来的数（换 FBX 就变）。序列内容才是"有没有按边界发"的判据。

⚠ 场景从 `verify_anim_layers` 的模板复用（`MYVK_EDITOR_IMPORT` 烤的 9 实体
  场景）。**别手写 `skinned` 实体** —— 手写的那个在视口里一帧都不画，而且
  日志会骗你说一切正常（那条坑记在 `verify_anim_layers.py` 文件头）。

运行：python tools/verify_anim_events.py
结尾一定是 `结果: PASS` / `结果: FAIL: ...`。
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import find_window, user32                               # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect            # noqa: E402
from verify_anim_layers import (CAM, DEFAULT_EXE, OUT_DIR, TPL, _base_env,  # noqa: E402
                                _close, bake_template)

# 事件表：三件事落在 0.2 / 0.5 / 0.8 秒。三个都要比 Walk 的时长小。
#
# ⚠ 为什么不用 0.0：`t=0` 会在**每次回绕**时发一次，于是"第一圈"与"第二圈"
#   的首条事件落在同一帧附近，序列切分变得依赖具体帧号。避开端点之后，
#   序列就是规整的 ABCABCABC，`seq[0:3] == seq[3:6]` 直接就是"两圈一致"。
EVENTS = [(0.2, "EvA", "a"), (0.5, "EvB", "b"), (0.8, "EvC", "c")]
# 越界事件：写在 5.0 秒（远大于 Walk 的时长）→ **永远不发**。
# 这条是否定测试：它与 EVENTS 在同一张表里，只有它不出现才说明
# "t >= duration 被丢掉了"，而不是"整张表都没生效"。
OUT_OF_RANGE = (5.0, "EvLate", "")

NEEDLE = "ANIM-EVENT entity="
EV_RE = re.compile(r"ANIM-EVENT entity=(\d+)\s+clip=(\S+)\s+layer=\[([^\]]*)\]\s+"
                   r"kind=(\S+)\s+t=([\d.]+)\s+payload=\[([^\]]*)\]")

results: list[tuple[str, bool, str]] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    results.append((name, ok, detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))


# ============================================================
# 变体：模板上只改 `animator.events`（与 B1 改 layers 同一条路）
# ============================================================

def _root_of(doc: dict) -> dict:
    for e in doc["entities"]:
        if e.get("skinned"):
            return e
    raise SystemExit("[FAIL] 模板里没有蒙皮实体（先跑一次 verify_anim_layers）")


def build_variant(tag: str, events=None) -> str:
    """`events` = [(t, kind, payload), ...]；None = 完全不给事件表。"""
    with open(TPL, encoding="utf-8") as f:
        doc = json.load(f)
    root = _root_of(doc)
    root["skinned"]["clipName"] = "Walk"
    root["skinned"]["time"] = 0.0
    root["skinned"]["playing"] = False   # 权威在 animator 那一份
    root["skinned"]["loop"] = True

    ent = {
        "defaultClip": "Walk",
        "controllerPath": "",
        "time": 0.0,
        "speed": 1.0,
        "playing": True,     # ⚠ 与 B1 相反：事件要时间真的在走
        "loop": True,
        "rootMotion": 0,
    }
    if events is not None:
        ent["events"] = [{
            "clip": "Walk",
            "events": [{"t": t, "kind": k, "payload": p} if p else
                       {"t": t, "kind": k} for (t, k, p) in events],
        }]
    root["animator"] = ent
    doc["generator"] = "verify_anim_events"
    path = os.path.join(OUT_DIR, "anim_events_%s.json" % tag)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False)
    return path


# ============================================================
# 跑一轮 Play，收 ANIM-EVENT
#
# ⚠ 不复用 `verify_anim_layers.run_shot`：那个的等待条件写死成"截一张非空
#   视口图"，而这里要的是"凑够 N 条事件"。用固定 dt（`MYVK_FIXED_DT`）之后
#   仿真时间与墙钟解耦，只能按事件数等，不能按秒等 —— 1/120 的 dt 下动画
#   是半速的，等 2 秒只走了 1 秒的仿真。
# ============================================================

def run_play(scene: str, extra_env: dict, want: int = 6,
             timeout: float = 40.0) -> list[str]:
    env = _base_env()
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_CAM"] = CAM
    env["MYVK_EDITOR_PLAY"] = "1"
    env.update(extra_env)

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
        except Exception:
            pass
        raise SystemExit("[FAIL] 编辑器窗口没出现")
    try:
        user32.ShowWindow(hwnd, 9)
        wait_rect(tap, "VP-RECT", 20.0)
        end = time.time() + timeout
        while time.time() < end:
            with tap.lock:
                n = sum(1 for l in tap.lines if NEEDLE in l)
            if n >= want:
                break
            time.sleep(0.2)
        with tap.lock:
            lines = list(tap.lines)
    finally:
        _close(proc, hwnd)
    return lines


def parse_events(lines: list[str]) -> list[tuple[str, float, str, str, str]]:
    """→ [(kind, t, payload, clip, layer), ...]，保持日志顺序。"""
    out = []
    for l in lines:
        m = EV_RE.search(l)
        if not m:
            continue
        _ent, clip, layer, kind, t, payload = m.groups()
        out.append((kind, float(t), payload, clip, layer))
    return out


def seq_of(evs) -> list[tuple[str, float, str]]:
    """只留判据用的三元组。`t` 是**事件自身的时间** —— 这份日志能跨帧率
    比对，靠的就是它不是播放头时间（见 `AnimEvent.h`）。"""
    return [(k, round(t, 4), p) for (k, t, p, _c, _l) in evs]


# ============================================================

def main() -> int:
    print("=" * 72)
    print("B2 动画事件 / Notify（编辑器 Play + 固定 dt）")
    print("=" * 72)

    if not os.path.isfile(DEFAULT_EXE):
        print("结果: FAIL: 找不到 %s（先 ./make.cmd build）" % DEFAULT_EXE)
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.isfile(TPL):
        print("     模板不在，先烤一份（约 20 秒）…")
        bake_template(DEFAULT_EXE)

    # ---- 1) 事件真的发了，且与配置逐条一致 ----
    v60 = build_variant("v60", EVENTS)
    ev60 = parse_events(run_play(v60, {"MYVK_FIXED_DT": "0.0166667"}, want=6))
    s60 = seq_of(ev60)

    want = [(k, t, p) for (t, k, p) in EVENTS]
    head_ok = len(s60) >= 3 and s60[:3] == [(k, round(t, 4), p)
                                            for (k, t, p) in want]
    check("① ANIM-EVENT 出现且 kind/t/payload 与配置逐条一致", head_ok,
          "n=%d head=%s" % (len(s60), s60[:3]))

    # ⚠ 判据的口径：`t=` 必须是**事件时间**，不是播放头。
    # 若有人把它改成播放头时间，这条立刻红 —— 而"序列跨帧率一致"会跟着失效。
    times_ok = len(s60) >= 3 and all(
        abs(s60[i][1] - want[i][1]) < 1.0e-4 for i in range(3))
    check("① t 是事件自身的时间（不是播放头）", times_ok,
          "got=%s" % [x[1] for x in s60[:3]])

    # ---- 2) 两圈序列逐条相同 ----
    lap_ok = len(s60) >= 6 and s60[0:3] == s60[3:6]
    check("② 两圈事件序列逐条相同", lap_ok,
          "lap0=%s lap1=%s" % (s60[0:3], s60[3:6]))

    # ---- 3) 帧率无关：1/60 与 1/120 的前 6 条相同 ----
    v120 = build_variant("v120", EVENTS)
    ev120 = parse_events(run_play(v120, {"MYVK_FIXED_DT": "0.0083333"}, want=6))
    s120 = seq_of(ev120)
    fps_ok = len(s60) >= 6 and len(s120) >= 6 and s60[:6] == s120[:6]
    check("③ 帧率无关：60fps 与 120fps 前 6 条相同", fps_ok,
          "n60=%d n120=%d" % (len(s60), len(s120)))

    # ---- 4) 否定测试 ----
    # 4a 越界事件（t=5.0 > 时长）一条都不许发，而**同表里的正常事件照发**。
    #     ⚠ 这两个必须同时成立：只有"EvLate 不出现"的话，整张表没生效也满足。
    v_late = build_variant("late", EVENTS + [OUT_OF_RANGE])
    s_late = seq_of(parse_events(
        run_play(v_late, {"MYVK_FIXED_DT": "0.0166667"}, want=6)))
    kinds_late = [k for (k, _t, _p) in s_late]
    check("④ 越界事件（t=5.0）一条都不发，同表其它事件照发",
          "EvLate" not in kinds_late and "EvA" in kinds_late,
          "kinds=%s" % kinds_late[:8])

    # 4b 没有事件表 = 一条都不发（回归基线：B2 之前的场景行为不变）。
    v_none = build_variant("none", None)
    lines_none = run_play(v_none, {"MYVK_FIXED_DT": "0.0166667"}, want=6,
                          timeout=12.0)
    n_none = len(parse_events(lines_none))
    check("④ 没有事件表 → 0 条（老场景逐位不变）", n_none == 0,
          "n=%d" % n_none)

    # ---- 5) 落盘往返：scene.save 之后 events 还在，运行时字段不在 ----
    # 走的就是编辑器自己的 `scene.save`（与烤模板同一个控制台命令）——
    # 不另写一条"把 JSON 读出来再写回去"的路，那种路测的是脚本自己。
    v_save = build_variant("save", EVENTS)
    run_play(v_save, {"MYVK_FIXED_DT": "0.0166667",
                      "MYVK_EDITOR_CONSOLE_CMD": "scene.save"}, want=4,
             timeout=30.0)
    saved_ok = False
    detail = "scene.save 没落盘"
    if os.path.isfile(v_save):
        with open(v_save, encoding="utf-8") as f:
            txt = f.read()
        doc = json.loads(txt)
        root = _root_of(doc)
        trs = (root.get("animator") or {}).get("events") or []
        got = []
        if trs and trs[0].get("clip") == "Walk":
            for e in trs[0].get("events") or []:
                got.append((round(float(e.get("t", -1)), 4), e.get("kind"),
                            e.get("payload", "")))
        want_saved = [(round(t, 4), k, p) for (t, k, p) in EVENTS]
        no_runtime = ("eventPrev" not in txt) and ("eventClip" not in txt)
        saved_ok = got == want_saved and no_runtime
        detail = "saved=%s noRuntime=%s" % (got, no_runtime)
    check("⑤ scene.save 往返：events 逐字段保留且不写运行时字段",
          saved_ok, detail)

    # ---- 6) UI 面 ----
    # 用 MYVK_EDITOR_SELECT 预选角色（Inspector 绑到它），**不点按钮** ——
    # 长面板深处的按钮可能落在裁剪区外，"假点击"比不点更糟。
    env_ui = {
        "MYVK_EDITOR_PANELS": "inspector",
        "MYVK_EDITOR_INSP_TAB": "animation",
        "MYVK_EDITOR_SELECT": "anime_girl",
        # 编辑态（非 Play）：面板要能在**不跑**的时候编辑，这才是作者工作流
        "MYVK_EDITOR_PLAY": "0",
    }
    lines_ui = run_play(build_variant("ui", EVENTS), env_ui, want=1,
                        timeout=15.0)
    txt_ui = "\n".join(lines_ui)
    need = ["INS-ANIM-EVENTS", "INS-EV-ADD", "INS-EV-TRACK", "INS-EV-TIME",
            "INS-EV-KIND", "INS-EV-PAYLOAD"]
    missing = [t for t in need if t not in txt_ui]
    tags = sorted({ln.split("RECT ")[1].split()[0]
                   for ln in lines_ui if "RECT " in ln})
    check("⑥ Inspector 画出事件段与事件行", not missing,
          "missing=%s tags=%d" % (missing, len(tags)))

    # ---- 汇总 ----
    bad = [r for r in results if not r[1]]
    print("-" * 66)
    for name, ok, _ in results:
        print("%-5s %s" % ("PASS" if ok else "FAIL", name))
    print("结果: " + ("PASS" if not bad else "FAIL: %d 组未通过" % len(bad)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
