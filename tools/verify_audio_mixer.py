#!/usr/bin/env python3
# ============================================================
# verify_audio_mixer —— 音频混音台（A2）的端到端验收
#
# ---------------------------- 这一刀在验什么 ----------------------------
# 音频**内核**从 G14 起就完整：自研 Mixer + WASAPI/Null 双后端 + 3D 空间化
# + ducking + 流式音乐 + 语音总线，`Game.exe --selftest audio` 能在无声卡
# 机器上把它们全断言一遍。
#
# 但 2026-11-02 之前，编辑器里：
#     grep -rn "setMasterGain\|setMuted\|setDuckDepth" editor/  →  0 命中
# 也就是说 Master / 静音 / duck 深度对开发者**不可达**（只能改 C++ 重编译），
# 而"语音总线电平"（B6 口型的唯一输入）压根没有任何界面可看。
#
# 所以这一刀验的不是混音算法，而是**入口有没有真的接上**：
#
#   ① 旋钮能改，而且**读回来是改后的值**（A 组）
#      —— 只验"命令收到了"是不够的：`AUDIO-CMD knob=music value=0.30`
#         在"参数没落到 AudioSystem"时照样打出来。所以另立 `audio.state`
#         读回末态，而且读的是 `busGain()` —— 与面板推子同一个访问器。
#   ② 面板控件真的画出来了（B 组，锚点全在）
#   ③ **点面板上的按钮真的能改**（C 组）—— "界面点不到 = 没交付"
#   ④ 老路径 `audio.bus master` 与 `setMasterGain` 不许分叉（D 组）
#      —— 这条抓的是本轮顺手修的真 bug：以前 `setBusGain(Master)` 写
#         `m_busGain[0]` 而不写 `m_master`，两个真值各对一半，
#         于是"命令改了、推子读不到"。
#   ⑤ 没有声卡（null 后端）时面板照常画、不报错（E 组）
#
# ---------------------------- 为什么不做像素判据 ----------------------------
# 音频没有像素。电平表是**弧长**，只能看"有没有在动"；能断言的量全在
# 数值层（增益 / 声部数 / RMS / 串号）。判据放在日志与读回上。
#
# 用法：python tools/verify_audio_mixer.py
# ============================================================

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                   # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors, wait_rect  # noqa: E402
from verify_asset_drag import force_foreground                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
SCENE_DIR = os.path.join(BIN, "assets", "scenes")
SIZE = "1400x900"

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

def write_scene(name):
    """最小场景。混音台不读场景内容，但编辑器要有个场景才起得来。

    ⚠ 不要塞 AudioSourceComponent：编辑态**不跑**场景音频，塞了也不会
      出现在声源列表里，反而会让"列表为空"这条判据变得含糊。
    """
    os.makedirs(SCENE_DIR, exist_ok=True)
    scene = {
        "version": 2,
        "generator": "verify_audio_mixer.py",
        "camera": {"target": [0.0, 0.0, 1.0], "yaw": 0.610865,
                   "pitch": 0.314159, "distance": 8.0, "fov": 50.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": False},
        "entities": [],
    }
    path = os.path.join(SCENE_DIR, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)
    return "scenes/" + name


# ---------------------------------------------------------------- 运行

def snap(tap):
    with tap.lock:
        return list(tap.lines)


def close(proc):
    try:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
    except Exception:                                          # noqa: BLE001
        pass


def launch(scene, console_cmd=None, extra_env=None, cmd_frame=70,
           wait_for="AM-RECT", timeout=60.0):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_NO_HUD"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
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
        close(proc)
        return proc, None, tap
    user32.ShowWindow(hwnd, 9)
    time.sleep(1.0)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    force_foreground(hwnd)

    deadline = time.time() + 40.0
    while time.time() < deadline:
        lines = snap(tap)
        if wait_for is None or any(wait_for in ln for ln in lines):
            break
        time.sleep(0.2)
    time.sleep(3.0)
    return proc, hwnd, tap


def run_once(scene, console_cmd, extra_env=None, wait_for="AM-RECT"):
    proc, hwnd, tap = launch(scene, console_cmd, extra_env, wait_for=wait_for)
    lines = snap(tap)
    close(proc)
    return lines


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

RE_STATE = re.compile(
    r"AUDIO-STATE master=([-\d.]+) music=([-\d.]+) sfx=([-\d.]+) "
    r"voice=([-\d.]+) duck=([-\d.]+) mute=(\d+) running=(\d+)")
RE_CMD_KNOB = re.compile(r"AUDIO-CMD knob=(\w+) value=([-\d.]+)")
RE_UNKNOWN = re.compile(r"AUDIO-CMD gain=unknown knob=(\S+)")
RE_SRC_N = re.compile(r"AUDIO-SOURCES n=(\d+) playing=(\d+)")
RE_MIX = re.compile(r"AUDIO-MIX knob=(\w+) value=([-\d.]+)")

ANCHORS = ["AM-RECT", "AM-STATUS", "AM-FADER-MASTER", "AM-FADER-MUSIC",
           "AM-FADER-SFX", "AM-FADER-VOICE", "AM-MUTE", "AM-MUTEFOCUS",
           "AM-DUCK", "AM-STOPALL", "AM-BUS-MUSIC", "AM-BUS-SFX",
           "AM-BUS-VOICE", "AM-VOICE-LEVEL", "AM-VOICE-PLAYING",
           "AM-NOSRC", "AM-LIVE"]


# ---------------------------------------------------------------- 组 A

def group_a_knobs():
    print("\n[A] 旋钮：改得动 + 读回来是改后的值（不点 UI）")
    scene = write_scene("_am_min.json")
    lines = run_once(
        scene,
        "audio.state; "
        "audio.gain music 0.30; audio.state; "
        "audio.gain music 0.85; audio.mute 1; audio.state; "
        "audio.gain duck 0; audio.state; "
        "audio.gain bogus 0.5; "
        "audio.sources",
        wait_for="AUDIO-SOURCES")

    st = all_of(RE_STATE, lines)
    check("A1 四条 AUDIO-STATE 都打出来了", len(st) >= 4, "n=%d" % len(st))
    if len(st) >= 4:
        # 字段序：1 master / 2 music / 3 sfx / 4 voice / 5 duck / 6 mute
        check("A2 出厂 music=0.700（编辑器硬编码的那一份）",
              st[0].group(2) == "0.700", st[0].group(0))
        check("A3 改成 0.30 之后**读回** 0.300（不是只收到命令）",
              st[1].group(2) == "0.300", st[1].group(0))
        check("A4 再改成 0.85 读回 0.850", st[2].group(2) == "0.850",
              st[2].group(0))
        check("A5 mute 1 之后读回 mute=1", st[2].group(6) == "1",
              st[2].group(0))
        check("A6 duck 0 之后读回 duck=0.000", st[3].group(5) == "0.000",
              st[3].group(0))
        # 没动过的总线不许被顺手改掉 —— 一次 set 影响全场是很常见的实现事故
        check("A7 没动过的 sfx/voice 仍是 1.000",
              st[3].group(3) == "1.000" and st[3].group(4) == "1.000",
              st[3].group(0))
    else:
        for i in range(2, 8):
            check("A%d （缺 AUDIO-STATE，跳过）" % i, False, "no state")

    knobs = RE_CMD_KNOB.findall("\n".join(lines))
    check("A8 AUDIO-CMD 记到了 music 两次",
          sum(1 for k, _ in knobs if k == "music") == 2, str(knobs))
    check("A9 AUDIO-CMD 记到了 mute / duck",
          any(k == "mute" for k, _ in knobs) and any(k == "duck" for k, _ in knobs),
          str(knobs))

    mu = first(RE_UNKNOWN, lines)
    check("A10 认不出的旋钮要留痕（不静默忽略）", bool(mu),
          mu.group(0) if mu else "-")

    ms = first(RE_SRC_N, lines)
    check("A11 编辑态声源 n=0 / playing=0（编辑态不跑场景音频）",
          bool(ms) and ms.group(1) == "0" and ms.group(2) == "0",
          ms.group(0) if ms else "-")
    check("A12 零 [ERROR]", not any("[ERROR]" in ln for ln in lines),
          next((ln for ln in lines if "[ERROR]" in ln), ""))


# ---------------------------------------------------------------- 组 B

def group_b_ui():
    print("\n[B] 面板：锚点全在（看画没画，不点）")
    scene = write_scene("_am_ui.json")
    lines = run_once(scene, None, {"MYVK_EDITOR_PANELS": "audio"})
    text = "\n".join(lines)
    miss = [a for a in ANCHORS if a not in text]
    check("B1 全部 %d 个锚点都在" % len(ANCHORS), not miss,
          "missing=%s" % (",".join(miss) if miss else "-"))
    check("B2 零 [ERROR]（面板没把 ImGui 的 ID 栈搞坏）",
          not any("[ERROR]" in ln for ln in lines),
          next((ln for ln in lines if "[ERROR]" in ln), ""))
    # 面板打开时会打一条 AUDIO-MIX？不会 —— 只有动作才打。反过来验：
    # "没动过就没日志"，否则说明面板每帧都在推意图（推子会自己漂）。
    check("B3 没碰控件时**没有** AUDIO-MIX（面板不会每帧推意图）",
          not first(RE_MIX, lines), "")


# ---------------------------------------------------------------- 组 C

def group_c_button():
    print("\n[C] 点面板上的 Stop all 真的能改（不是只有命令能改）")
    scene = write_scene("_am_button.json")
    proc, hwnd, tap = launch(scene, None, {"MYVK_EDITOR_PANELS": "audio"})
    try:
        if not hwnd:
            check("C0 编辑器起来了", False, "no window")
            return
        r = wait_rect(tap, "AM-STOPALL", timeout=10.0)
        check("C1 Stop all 按钮有矩形", bool(r), str(r))
        if r:
            force_foreground(hwnd)
            click(hwnd, (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5)
        m = None
        deadline = time.time() + 6.0
        while time.time() < deadline and not m:
            m = first(RE_MIX, snap(tap))
            time.sleep(0.2)
        check("C2 点一下就有一条 AUDIO-MIX knob=stopall",
              bool(m) and m.group(1) == "stopall", m.group(0) if m else "-")
        check("C3 零 [ERROR]", not any("[ERROR]" in ln for ln in snap(tap)), "")
    finally:
        close(proc)


# ---------------------------------------------------------------- 组 D

def group_d_legacy_bus():
    print("\n[D] 老命令 audio.bus 与推子不许分叉（本轮修的真 bug）")
    scene = write_scene("_am_bus.json")
    lines = run_once(
        scene,
        "audio.state; audio.bus master 0.5; audio.state; "
        "audio.bus sfx 0.20; audio.state; audio.bus nosuch 0.5; audio.state",
        wait_for="AUDIO-STATE")

    st = all_of(RE_STATE, lines)
    check("D1 四条 AUDIO-STATE 都在", len(st) >= 3, "n=%d" % len(st))
    if len(st) >= 3:
        # ⚠ 这条就是那个 bug 的判据：修之前 `setBusGain(Master)` 只写
        #   `m_busGain[0]`，`busGain(Master)` 读 `m_master` → 这里会是 1.000。
        check("D2 audio.bus master 0.5 之后读回 master=0.500",
              st[1].group(1) == "0.500", st[1].group(0))
        check("D3 audio.bus sfx 0.20 之后读回 sfx=0.200",
              st[2].group(3) == "0.200", st[2].group(0))
        check("D4 master 没有被 sfx 那次顺手改掉",
              st[2].group(1) == "0.500", st[2].group(0))
        check("D5 认不出的总线不改任何东西",
              st[3].group(1) == "0.500" and st[3].group(3) == "0.200",
              st[3].group(0))
    else:
        for i in range(2, 6):
            check("D%d （缺 AUDIO-STATE，跳过）" % i, False, "no state")
    check("D6 认不出的总线要留痕",
          any("AUDIO-CMD bus=unknown" in ln for ln in lines), "")


# ---------------------------------------------------------------- 组 E

def group_e_no_device():
    print("\n[E] null 后端（没有声卡）时面板照画、不报错")
    scene = write_scene("_am_null.json")
    lines = run_once(scene, None,
                     {"MYVK_EDITOR_PANELS": "audio",
                      "MYVK_EDITOR_AUDIO_NULL": "1"})
    text = "\n".join(lines)
    check("E1 面板仍然画出来（AM-RECT / AM-FADER-MASTER 在）",
          "AM-RECT" in text and "AM-FADER-MASTER" in text, "")
    check("E2 设备侧打了一条 EDITOR-AUDIO（后端名可查）",
          any("EDITOR-AUDIO enabled=" in ln for ln in lines), "")
    check("E3 零 [ERROR]（设备不可用不是错误）",
          not any("[ERROR]" in ln for ln in lines),
          next((ln for ln in lines if "[ERROR]" in ln), ""))


# ---------------------------------------------------------------- main

def main():
    global _n, _bad
    print("=" * 60)
    print("verify_audio_mixer —— 混音台：旋钮 / 读回 / 面板 / 老路径")
    print("=" * 60)
    if not os.path.isfile(EDITOR):
        print("✗ 找不到 %s（先跑 `make build`）" % EDITOR)
        print("结果: FAIL: 编辑器不存在")
        return 1
    kill_stale_editors()

    group_a_knobs()
    group_b_ui()
    group_c_button()
    group_d_legacy_bus()
    group_e_no_device()

    print("\n" + "-" * 60)
    print("通过 %d / 失败 %d" % (_n - _bad, _bad))
    if _bad:
        print("结果: FAIL: %d 条断言没通过" % _bad)
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
