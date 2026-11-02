#!/usr/bin/env python3
# ============================================================
# verify_post_volume —— 后处理体积（Post-Process Volume）的端到端验收
#
# ---------------------------- 这一刀在验什么 ----------------------------
# `scene::PostVolume` 的**内核**从 R1-2/R1-3 起就在（AABB + 优先级 +
# 过渡带 + kNoOverride），落盘与 `Renderer` 求值也都通了。真正缺的是
# "开发者能不能在编辑器里放一个出来"——2026-11-02 之前 `editor/` 里
# `PostVolume` **零命中**。
#
# 所以这一刀验的不是算法，而是**四条链路是否真的接上了**：
#
#   ① 场景 JSON 的 `postVolumes` 能读进场景（`PV-LIST`）
#   ② 求值按"盒内 = 1 / 盒外 = 0"分开（`PV-EVAL` 两组坐标）
#   ③ **落盘往返不丢字段** —— 这条当场抓到一个真 bug：`cgLutBlend` /
#      `autoExposureBias` 只进了结构体、没进 toJson/fromJson，
#      设了 LUT 混合 0.7 存盘再读回来变 0.0，而且**一句报错都没有**
#   ④ 面板控件真的画出来了（锚点全在），且"未覆盖"的项**不画滑条**
#      —— 这是 `kNoOverride` 语义在 UI 上的唯一可见证据
#
# ---------------------------- 为什么不做像素判据 ----------------------------
# 后处理体积的效果是"景深/暗角/调色"，在 1400x900 的一张静帧里，
# 景深焦点从 5 m 改到 6 m 可能只有几十个像素的模糊半径差 —— 低于噪声。
# 判据放在**数值**层（权重、混合值、JSON 字段），像素留给 `verify_camera`
# / `verify_postfx` 那两条已经稳定的链。
#
# 用法：python tools/verify_post_volume.py
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
import safe_fs                                                   # noqa: E402

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

def volume(name, lo, hi, **kw):
    """一份 `postVolumeToJson` 形状的体积（键名与序列化器逐字一致）。

    ⚠ 键名写错不会报错 —— `j.value("x", default)` 对缺失键是静默的，
      于是"夹具写错"会表现成"产品丢了字段"。所以这里只覆盖**已知键**，
      并且 A 组专门断言读回来的值。
    """
    v = {
        "name": name,
        "boundsMin": list(lo),
        "boundsMax": list(hi),
        "priority": 1.0,
        "blendDistance": 1.0,
        "dofFocus": -1.0,
        "dofAperture": -1.0,
        "dofMaxRadius": -1.0,
        "motionBlurStrength": -1.0,
        "chromaStrength": -1.0,
        "vignetteStrength": -1.0,
        "dof": -1,
        "motionBlur": -1,
        "chromatic": -1,
        "vignette": -1,
        "cgLutBlend": -1.0,
        "autoExposureBias": -1.0,
    }
    v.update(kw)
    return v


def write_scene(name, volumes=None):
    """一个"看得见东西"的最小场景；`volumes=None` = 老场景（不写这个键）。"""
    os.makedirs(SCENE_DIR, exist_ok=True)
    scene = {
        "version": 2,
        "generator": "verify_post_volume.py",
        "camera": {"target": [0.0, 0.0, 1.0], "yaw": 0.610865,
                   "pitch": 0.314159, "distance": 16.0, "fov": 50.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": False},
        "entities": [{
            "name": "Box",
            "parent": -1,
            "visible": True,
            "castShadow": True,
            "transform": {"position": [0.0, 0.0, 0.0],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "mesh": {"kind": "builtin", "shape": "cube", "size": 2.0,
                     "name": "pv_box"},
            "material": {"kind": "procedural", "name": "pv_mat",
                         "baseColor": [0.7, 0.7, 0.72, 1.0],
                         "metallic": 0.0, "roughness": 0.6,
                         "normalScale": 0.0, "ao": 1.0},
        }],
    }
    if volumes is not None:
        scene["postVolumes"] = volumes
    path = os.path.join(SCENE_DIR, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=1)
    return "scenes/" + name


# ---------------------------------------------------------------- 运行

def snap(tap):
    with tap.lock:
        return list(tap.lines)


def launch(scene, console_cmd=None, extra_env=None, cmd_frame=70,
           wait_for="PV-", timeout=60.0):
    """起编辑器，等到关键日志出现；返回 (proc, hwnd, tap)。"""
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

    # 等场景读完 + 命令跑完（命令在第 70 帧）
    deadline = time.time() + 40.0
    while time.time() < deadline:
        lines = snap(tap)
        if wait_for is None or any(wait_for in ln for ln in lines):
            break
        time.sleep(0.2)
    time.sleep(3.0)
    return proc, hwnd, tap


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


def run_once(scene, console_cmd, extra_env=None, wait_for="PV-"):
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

RE_LIST_N = re.compile(r"PV-LIST n=(\d+)")
RE_LIST_ROW = re.compile(r"PV-LIST i=(\d+) (.*)")
RE_EVAL = re.compile(
    r"PV-EVAL cam=\(([-\d.]+),([-\d.]+),([-\d.]+)\) total=(\d+) active=(\d+) "
    r"dof=(-?\d+) focus=([-\d.]+) ap=([-\d.]+) maxr=([-\d.]+) "
    r"mb=(-?\d+) mbstr=([-\d.]+) chr=(-?\d+) chrstr=([-\d.]+) "
    r"vig=(-?\d+) vigstr=([-\d.]+) lut=([-\d.]+) exp=([-\d.]+)")
RE_CREATE = re.compile(r"PV-CREATE name=(\S+) count=(\d+)")
RE_DELETE = re.compile(r"PV-DELETE name=(\S+) count=(\d+)")

ANCHORS = ["PV-RECT", "PV-NEW", "PV-DUP", "PV-DEL", "PV-ITEM-0",
           "PV-NAME", "PV-MIN", "PV-MAX", "PV-PRIORITY", "PV-BLEND", "PV-LIVE"]


# ---------------------------------------------------------------- 组 A

def group_a_roundtrip():
    print("\n[A] 读场景 / 求值 / 落盘往返（不点 UI）")
    scene = write_scene("_pv_roundtrip.json", [
        # 覆盖值故意取"非默认"：全默认的话，字段丢没丢分不出来。
        volume("room", (-5.0, -5.0, -5.0), (5.0, 5.0, 5.0),
               priority=2.0, blendDistance=1.0,
               dof=1, dofFocus=5.0, dofAperture=2.8, dofMaxRadius=8.0,
               cgLutBlend=0.7, autoExposureBias=-0.5),
    ])
    lines = run_once(
        scene,
        "pv.list; pv.eval 0 0 0; pv.eval 100 0 0; scene.save; "
        "pv.eval; pv.add AtCam; pv.list; pv.remove 0; pv.list")

    ml = first(RE_LIST_N, lines)
    check("A1 场景里的 postVolumes 读成 1 个", bool(ml) and ml.group(1) == "1",
          ml.group(0) if ml else "no PV-LIST")

    rows = all_of(RE_LIST_ROW, lines)
    # ⚠ 列表行用的是 `postVolumeDescribe()` 的输出，名字以 `name=` 打头
    #   （与 `PV-CREATE name=` 同一套写法）。第一版按行首匹配名字，
    #   A2/A15 假红 —— 行内容其实一直是对的。
    check("A2 列表里有 room，且覆盖值全读对",
          any("name=room" in m.group(2) and "focus=5.00" in m.group(2)
              and "lut=0.70" in m.group(2) and "exp=-0.50" in m.group(2)
              for m in rows),
          rows[0].group(2) if rows else "no row")

    evals = all_of(RE_EVAL, lines)
    check("A3 PV-EVAL 至少 3 次（盒内 / 盒外 / 相机）", len(evals) >= 3,
          "n=%d" % len(evals))
    if len(evals) >= 2:
        inside, outside = evals[0], evals[1]
        # 盒内：权重 1 → 覆盖值原样采用
        check("A4 原点在盒内：active=1 且 dof=1",
              inside.group(5) == "1" and inside.group(6) == "1",
              "active=%s dof=%s" % (inside.group(5), inside.group(6)))
        check("A5 盒内焦距 = 5.0（不是 kNoOverride）",
              abs(float(inside.group(7)) - 5.0) < 1e-3, inside.group(7))
        # 这一条是**新增字段**的判据：旧 toJson 丢这两个键，这里会读到 -1
        check("A6 盒内 LUT 混合 = 0.70（cgLutBlend 落盘了）",
              abs(float(inside.group(16)) - 0.7) < 1e-3, inside.group(16))
        check("A7 盒内曝光偏置 = -0.50（autoExposureBias 落盘了）",
              abs(float(inside.group(17)) + 0.5) < 1e-3, inside.group(17))
        # 盒外：权重 0 → 一个字段都不该被覆盖
        check("A8 100 m 外：active=0", outside.group(5) == "0", outside.group(5))
        check("A9 盒外全部回到 kNoOverride（dof=-1 / focus=-1 / lut=-1）",
              outside.group(6) == "-1" and outside.group(7) == "-1.000"
              and outside.group(16) == "-1.000",
              "dof=%s focus=%s lut=%s" % (outside.group(6), outside.group(7),
                                          outside.group(16)))

    # ---- 存盘 JSON：字段必须真的写进去 ----
    path = os.path.join(SCENE_DIR, "_pv_roundtrip.json")
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        vols = doc.get("postVolumes") or []
        v0 = vols[0] if vols else {}
        check("A10 存盘后 postVolumes 还在（1 个）", len(vols) == 1,
              "n=%d" % len(vols))
        check("A11 cgLutBlend 写进了 JSON（这条抓的是真 bug）",
              abs(float(v0.get("cgLutBlend", -99)) - 0.7) < 1e-3,
              str(v0.get("cgLutBlend")))
        check("A12 autoExposureBias 写进了 JSON",
              abs(float(v0.get("autoExposureBias", -99)) + 0.5) < 1e-3,
              str(v0.get("autoExposureBias")))
        check("A13 几何保真（min/max）",
              v0.get("boundsMin") == [-5.0, -5.0, -5.0]
              and v0.get("boundsMax") == [5.0, 5.0, 5.0],
              "%s / %s" % (v0.get("boundsMin"), v0.get("boundsMax")))
    except Exception as e:                                     # noqa: BLE001
        check("A10 读回转储的场景", False, repr(e))

    # ---- pv.add 的默认位置 = 相机（这条证明"建完立刻能看见"）----
    if len(evals) >= 3:
        cam = [float(evals[2].group(i)) for i in (1, 2, 3)]
        mc = first(RE_CREATE, lines)
        check("A14 pv.add 建出了 AtCam", bool(mc) and mc.group(1) == "AtCam",
              mc.group(0) if mc else "no PV-CREATE")
        # 第二条 PV-LIST 里找 AtCam 的 min/max
        lists = all_of(RE_LIST_N, lines)
        rows2 = [m.group(2) for m in all_of(RE_LIST_ROW, lines)
                 if "name=AtCam" in m.group(2)]
        # 只要**最后一条** AtCam 行（前面那次列表里没有它）
        if rows2:
            mmin = re.search(r"min=\(([-\d.]+),([-\d.]+),([-\d.]+)\)", rows2[0])
            if mmin:
                got = [float(mmin.group(i)) for i in (1, 2, 3)]
                expect = [c - 4.0 for c in cam]
                check("A15 新体积以相机为圆心、半边长 4 m",
                      all(abs(a - b) < 0.05 for a, b in zip(got, expect)),
                      "min=%s expect=%s" % (got, [round(e, 2) for e in expect]))
        else:
            check("A15 新体积的 min 出现在列表里", False, "no AtCam row")

        md = first(RE_DELETE, lines)
        check("A16 pv.remove 0 删掉了 room",
              bool(md) and md.group(1) == "room", md.group(0) if md else "-")
        check("A17 删完只剩 1 个（count 递减）",
              bool(md) and md.group(2) == "1", md.group(2) if md else "-")
        check("A18 最后一次列表 n=1",
              bool(lists) and lists[-1].group(1) == "1",
              lists[-1].group(1) if lists else "-")


# ---------------------------------------------------------------- 组 B

def group_b_ui():
    print("\n[B] 面板：锚点 / 覆盖语义（不点，只看画没画）")
    scene = write_scene("_pv_ui.json", [
        volume("room", (-5.0, -5.0, -5.0), (5.0, 5.0, 5.0),
               dof=1, dofFocus=5.0, cgLutBlend=0.7),
    ])
    lines = run_once(scene, None, {"MYVK_EDITOR_PANELS": "postvol"},
                     wait_for="PV-RECT")
    text = "\n".join(lines)

    for a in ANCHORS:
        check("B1 锚点在：%s" % a, ('"%s"' % a) in text or a in text, "")

    # 覆盖语义：勾上的项**有滑条**，没勾的项**只有勾选框**
    check("B2 勾上的 dof 有滑条（PV-OVR-DOF-FOCUS-VAL）",
          "PV-OVR-DOF-FOCUS-VAL" in text, "")
    check("B3 勾上的 LUT 有滑条（PV-OVR-CG-LUT-VAL）",
          "PV-OVR-CG-LUT-VAL" in text, "")
    check("B4 dof 三态是下拉（PV-OVR-DOF-TRI）", "PV-OVR-DOF-TRI" in text, "")
    check("B5 未覆盖的 motionBlur **没有**强度滑条（只看得到勾选框）",
          "PV-OVR-MOTION-STR-CHECK" in text
          and "PV-OVR-MOTION-STR-VAL" not in text,
          "check=%s val=%s" % ("PV-OVR-MOTION-STR-CHECK" in text,
                               "PV-OVR-MOTION-STR-VAL" in text))
    check("B6 未覆盖的 vignette 同样没有强度滑条",
          "PV-OVR-VIGNETTE-STR-CHECK" in text
          and "PV-OVR-VIGNETTE-STR-VAL" not in text, "")
    check("B7 零 [ERROR]（面板没把 ImGui 的 ID 栈搞坏）",
          not any("[ERROR]" in ln for ln in lines),
          next((ln for ln in lines if "[ERROR]" in ln), ""))


# ---------------------------------------------------------------- 组 C

def group_c_button():
    print("\n[C] 面板按钮真的能建（不是只有命令能建）")
    scene = write_scene("_pv_button.json", [
        volume("room", (-5.0, -5.0, -5.0), (5.0, 5.0, 5.0), dof=1,
               dofFocus=5.0),
    ])
    proc, hwnd, tap = launch(scene, None,
                             {"MYVK_EDITOR_PANELS": "postvol"},
                             wait_for="PV-RECT")
    try:
        if not hwnd:
            check("C0 编辑器起来了", False, "no window")
            return
        r = wait_rect(tap, "PV-NEW", timeout=10.0)
        check("C1 New volume 按钮有矩形", bool(r), str(r))
        if r:
            force_foreground(hwnd)
            click(hwnd, (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5)
        # 等结构变更的日志
        mc = None
        deadline = time.time() + 6.0
        while time.time() < deadline and not mc:
            mc = first(RE_CREATE, snap(tap))
            time.sleep(0.2)
        check("C2 点一下就建了一个（PV-CREATE count=2）",
              bool(mc) and mc.group(2) == "2", mc.group(0) if mc else "-")
        check("C3 新条目出现在列表里（PV-ITEM-1）",
              "PV-ITEM-1" in "\n".join(snap(tap)), "")
        check("C4 零 [ERROR]", not any("[ERROR]" in ln for ln in snap(tap)),
              "")
    finally:
        close(proc)


# ---------------------------------------------------------------- 组 D

def group_d_legacy():
    print("\n[D] 老场景（没有 postVolumes 键）不能出事")
    scene = write_scene("_pv_legacy.json", None)
    lines = run_once(scene, "pv.list; pv.eval 0 0 0",
                     {"MYVK_EDITOR_PANELS": "postvol"},
                     wait_for="PV-RECT")
    text = "\n".join(lines)
    ml = first(RE_LIST_N, lines)
    check("D1 pv.list n=0（不凭空造体积）", bool(ml) and ml.group(1) == "0",
          ml.group(0) if ml else "-")
    me = first(RE_EVAL, lines)
    check("D2 求值不崩，且所有字段都是 kNoOverride",
          bool(me) and me.group(5) == "0" and me.group(6) == "-1",
          me.group(0) if me else "-")
    check("D3 面板照样画出来（PV-RECT / PV-NEW 在）",
          "PV-RECT" in text and "PV-NEW" in text, "")
    check("D4 空场景不报错误（提示语由面板给）",
          not any("[ERROR]" in ln for ln in lines), "")


# ---------------------------------------------------------------- main

def main():
    global _n, _bad
    print("=" * 60)
    print("verify_post_volume —— 后处理体积：入口 / 求值 / 落盘 / UI")
    print("=" * 60)
    if not os.path.isfile(EDITOR):
        print("✗ 找不到 %s（先跑 `make build`）" % EDITOR)
        print("结果: FAIL: 编辑器不存在")
        return 1
    kill_stale_editors()

    group_a_roundtrip()
    group_b_ui()
    group_c_button()
    group_d_legacy()

    print("\n" + "-" * 60)
    print("通过 %d / 失败 %d" % (_n - _bad, _bad))
    if _bad:
        print("结果: FAIL: %d 条断言没通过" % _bad)
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
