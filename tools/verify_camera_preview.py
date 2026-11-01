"""编辑器视口"以选中相机预览"验收（#150 尾巴之一）。

    python tools/verify_camera_preview.py

==============================================================================
这一条补的是什么
==============================================================================
运镜体系（`CameraComponent`）到这一版为止只有 `Game.exe` 侧接线：运行时由
`CameraSystem` 把活跃相机的机位 / 镜头写进 `scene::Camera`。**编辑器视口是
自由相机**（不经过 `CameraSystem`），所以在编辑器里点开一台相机，看到的是
自由相机那一套 —— 景深、快门、离轴、焦距全部看不到效果，只能进 Play 才知道
镜头长什么样。

现在补上了：Cinematography 面板 ▸ **Look through selected camera**。

==============================================================================
四条判据，各钉一种失败模式
==============================================================================
A2  **点得到**：按钮有矩形 ≠ 连上了。点完必须出现 `CINE-PREVIEW on=1`，
    而且 guid 是**选中那一台**的（不是"列表里第一台"）。
A3  **机位真的换了**：`CINE-PREVIEW-LENS` 报的 pos/yaw/pitch 必须等于夹具里
    那台相机的世界变换 —— 不是自由相机的位置。
    ⚠ 这一条的落点是 `scene.camera()` 的**实测值**，也就是渲染真正用的那个
      对象。写成"面板状态栏里显示了相机名"就变成了在断言 UI 文案。
A4  **镜头参数接上了**：fov 必须由 `focal=35 / sensor=24` 算出来（37.85°），
    而不是组件里那个被覆盖的 `fov=50`；dof / 快门同理。
    ⚠ 这条同时是"两处换算不许分叉"的看门人 —— 编辑器这一路和
      `RuntimeApp::applyCameraLensEffects` 是两份代码（runtime 不认识
      editor），一旦谁改了换算，这里立刻红。
A5  **退出后原样还回去**：`CINE-PREVIEW on=0` + `CINE-PREVIEW-LENS restored=1`
    且 `dof=0`（进入前是关的）。
    ⚠ 不复原的话，退出预览之后整个编辑器视口会永远带着那台相机的浅景深 ——
      用户的感受是"关掉预览之后变得模糊了"，重启才恢复。

==============================================================================
**故意不测**"预览态下鼠标拖不动相机"（`ViewportPanel::handleNavigation` 里
那行 `if (m_ctx.previewing()) return;`）
==============================================================================
第一版写了这一条，判据是"右键拖 60 px 之后不许出现新的 `CINE-PREVIEW-LENS`
行"。**否定测试（把那行门禁注释掉重编重跑）它照样绿** —— 于是这条断言被删掉了。

原因很直接：`applyPreviewCamera()` 每帧都把相机重设成实体姿态，所以
`scene.camera()` 的**帧末值**恒等于实体姿态，`CINE-PREVIEW-LENS` 的签名永远
不变 —— 拖不拖都一样。也就是说"相机位置没变"这个量**区分不了**门禁在不在。

真正会变的只有"onImGui 那一帧渲染用的中间值"，而它渲染完就被下一帧抹掉；
能抓住它的只有"拖拽过程中连拍多帧、看有没有一帧偏了"，那是概率判据
（记忆里那条"20%~40% 踏空等于没有断言"）。所以这一条**不设判据**，只在
代码里保留门禁并写清理由（手感：不关的话是"推一下被弹回去"）。

> 一条永远为真的断言比没有断言更糟：它占着一个 PASS，让人以为这里测过了。

==============================================================================
为什么用夹具场景而不是拿现成场景
==============================================================================
现成场景里的相机参数是给人看的（随时会被调），而这个用例要断言**具体数字**
（pos=(4,-2,2) / yaw=0 / fov=37.85）。夹具写进 `build/bin/assets/scenes/`，
用**相对名**（`--scene` 打不开时不报错，见记忆里那条）。

用法：
    python tools/verify_camera_preview.py
"""

import json
import math
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit                                        # noqa: E402
from capture_window import find_window, user32                    # noqa: E402
from safe_fs import safe_remove                                   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")

SCENE_NAME = "scenes/_cine_preview_fixture.json"     # 相对 assets/
SCENE_PATH = os.path.join(BIN, "assets", "scenes", "_cine_preview_fixture.json")
OUT = os.path.join(os.path.dirname(ROOT), "_venn_verify", "cine_preview")

GUID_CAM = "c1b0000000000001"
CAM_NAME = "PrevCam"

# ---- 夹具里那台相机的"标准答案" ----
# 变换：position=(4,-2,2)，rotation.z = π（弧度！见 TransformComponent）。
#   Rz(π) 把局部 +X 转到世界 (-1,0,0)；而引擎的约定是"实体局部 +X = 视线"，
#   由 forward=(-cos·cos(yaw), +cos·sin(yaw), -sin(pitch)) 反解：
#     fwd=(-1,0,0) ⇒ yaw = atan2(0, 1) = 0，pitch = asin(0) = 0
#   ⚠ rotation 存的是**弧度**（`TransformComponent::rotation` 的注释），
#     写成 180 会转到别处 —— 而日志里只表现为 yaw 不是 0，看起来像"反解错了"。
CAM_POS = (4.0, -2.0, 2.0)
CAM_YAW = 0.0
CAM_PITCH = 0.0
CAM_FOCAL = 35.0
CAM_SENSOR = 24.0
# fovY = 2·atan(h/2f) = 2·atan(12/35) = 37.8498°
CAM_FOV = 2.0 * math.degrees(math.atan(CAM_SENSOR * 0.5 / CAM_FOCAL))
CAM_FSTOP = 2.0
CAM_FOCUS = 6.0
# aperture = 1.2 · (1.4/N) · (f/50)（见 CameraMove.cpp 的 fStopToDofAperture）
CAM_AP = 1.2 * (1.4 / CAM_FSTOP) * (CAM_FOCAL / 50.0)
CAM_SHUTTER = 180.0

PREVIEW_RE = re.compile(r"CINE-PREVIEW on=(\d)(?: guid=([0-9a-f]+) name=(\S+))?")
LENS_RE = re.compile(
    r"CINE-PREVIEW-LENS guid=([0-9a-f]+) name=(\S+) "
    r"pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) yaw=(-?[\d.]+) "
    r"pitch=(-?[\d.]+) fov=(-?[\d.]+) dof=(\d) fstop=(-?[\d.]+) "
    r"focus=(-?[\d.]+) ap=(-?[\d.]+) sh=(-?[\d.]+) blur=(-?[\d.]+) "
    r"pos2=(-?[\d.]+)")

PASSES = []
FAILS = []


def check(what, ok, detail=""):
    if ok:
        PASSES.append(what)
        kit.log("  [PASS] %s%s" % (what, ("  — " + detail) if detail else ""))
    else:
        FAILS.append(what)
        kit.log("  [FAIL] %s%s" % (what, ("  — " + detail) if detail else ""))
    return ok


def write_fixture():
    os.makedirs(os.path.dirname(SCENE_PATH), exist_ok=True)
    doc = {
        "version": 1,
        "generator": "verify_camera_preview.py",
        "camera": {"target": [0.0, 0.0, 1.5], "yaw": 0.0, "pitch": 0.25,
                   "distance": 8.0, "fov": 45.0},
        "light": {"direction": [0.42, 0.72, -0.55],
                  "color": [1.0, 0.96, 0.9], "intensity": 3.0,
                  "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": False, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            {"guid": GUID_CAM, "name": CAM_NAME, "parent": -1,
             "transform": {"position": list(CAM_POS),
                           "rotation": [0.0, 0.0, 3.14159265358979],
                           "scale": [1.0, 1.0, 1.0]},
             "visible": True, "castShadow": False,
             "camera": {"fov": 50.0,          # ⚠ 会被 focal 覆盖，正是 A4 的落点
                        "priority": 10, "enabled": True,
                        "focal": CAM_FOCAL, "sensorHeight": CAM_SENSOR,
                        "shutterAngle": CAM_SHUTTER,
                        "dofEnabled": True, "fStop": CAM_FSTOP,
                        "focusDistance": CAM_FOCUS}},
        ],
    }
    with open(SCENE_PATH, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)


def last_match(tap, rx):
    with tap.lock:
        for line in reversed(tap.lines):
            m = rx.search(line)
            if m:
                return m
    return None


def main():
    print("== 编辑器视口：以选中相机预览 ==")
    if not os.path.isfile(EDITOR):
        check("A0 Editor.exe 存在", False, EDITOR)
        return 1
    os.makedirs(OUT, exist_ok=True)
    write_fixture()

    env = kit.clean_env({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_PANELS": "cine",
        "MYVK_EDITOR_SCENE": SCENE_NAME,
        "MYVK_GRID": "0",
        "MYVK_EDITOR_NO_HUD": "1",
    })
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)
    hwnd = find_window(kit.EDITOR_TITLE, 90.0)
    if not hwnd:
        check("A0 编辑器窗口出现", False)
        proc.kill()
        return 1

    try:
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 1000, 0x0002 | 0x0004)
        time.sleep(1.5)
        kit.focus_window(hwnd)

        if not check("A0 运镜面板打开（CINE-WIN）",
                     tap.wait_rect("CINE-WIN", 40.0) is not None):
            return 1

        # ---- 选中夹具里那台相机 ----
        row_tag = "CINE-CAM-" + CAM_NAME
        if not check("A1 相机行出现（%s）" % row_tag,
                     tap.wait_rect(row_tag, 20.0) is not None):
            return 1
        n_state = tap.count_line("CINE-STATE")
        if not kit.click_in_panel(hwnd, tap, row_tag, "CINE-WIN")[0]:
            check("A1b 点得到相机行", False)
            return 1
        deadline = time.time() + 8.0
        while tap.count_line("CINE-STATE") <= n_state and time.time() < deadline:
            time.sleep(0.2)
        check("A1b 点相机行 → 面板状态行有反应",
              tap.count_line("CINE-STATE") > n_state,
              "CINE-STATE +%d" % (tap.count_line("CINE-STATE") - n_state))

        # ---- A2 点 Look through ----
        btn = tap.wait_rect("CINE-PREVIEW-BTN", 10.0)
        if not check("A2 预览按钮有矩形（CINE-PREVIEW-BTN）", btn is not None):
            return 1
        if not kit.click_in_panel(hwnd, tap, "CINE-PREVIEW-BTN", "CINE-WIN")[0]:
            check("A2b 点得到预览按钮", False)
            return 1
        deadline = time.time() + 10.0
        m_prev = None
        while time.time() < deadline:
            m_prev = last_match(tap, PREVIEW_RE)
            if m_prev and m_prev.group(1) == "1":
                break
            time.sleep(0.15)
        check("A2b 点完真的进了预览（CINE-PREVIEW on=1）",
              bool(m_prev) and m_prev.group(1) == "1",
              m_prev.group(0) if m_prev else "没有 CINE-PREVIEW 行")
        check("A2c 锁的是选中的那一台（guid 对得上）",
              bool(m_prev) and m_prev.group(2) == GUID_CAM,
              "want guid=%s got=%s" %
              (GUID_CAM, m_prev.group(2) if m_prev else None))

        # ---- A3 / A4 机位与镜头 ----
        lens = None
        deadline = time.time() + 10.0
        while time.time() < deadline:
            lens = last_match(tap, LENS_RE)
            if lens:
                break
            time.sleep(0.15)
        if not check("A3 出现机位锚点（CINE-PREVIEW-LENS）", lens is not None):
            return 1
        pos = tuple(float(lens.group(i)) for i in (3, 4, 5))
        ok_pos = all(abs(pos[i] - CAM_POS[i]) < 1.0e-3 for i in range(3))
        check("A3b 相机位置 = 夹具里的世界位置（不是自由相机）", ok_pos,
              "pos=(%.3f,%.3f,%.3f) want=%s" % (pos + (CAM_POS,)))
        yaw, pitch = float(lens.group(6)), float(lens.group(7))
        check("A3c yaw/pitch 由实体世界变换反解（局部 +X = 视线）",
              abs(yaw - CAM_YAW) < 1.0e-3 and abs(pitch - CAM_PITCH) < 1.0e-3,
              "yaw=%.4f pitch=%.4f want=(%.3f,%.3f)" %
              (yaw, pitch, CAM_YAW, CAM_PITCH))
        fov = float(lens.group(8))
        check("A4 fov 由 focal 算出来（37.85°，不是组件里的 50）",
              abs(fov - CAM_FOV) < 0.02,
              "fov=%.3f want=%.3f" % (fov, CAM_FOV))
        dof, fstop, focus, ap = (lens.group(9), float(lens.group(10)),
                                 float(lens.group(11)), float(lens.group(12)))
        check("A4b 景深接上了（dof=1 / fstop / focus 都对）",
              dof == "1" and abs(fstop - CAM_FSTOP) < 0.01
              and abs(focus - CAM_FOCUS) < 0.01,
              "dof=%s fstop=%.2f focus=%.2f" % (dof, fstop, focus))
        check("A4c 光圈换算与运行时同一条（ap=1.2·(1.4/N)·(f/50)）",
              abs(ap - CAM_AP) < 0.01, "ap=%.4f want=%.4f" % (ap, CAM_AP))
        sh, blur = float(lens.group(13)), float(lens.group(14))
        check("A4d 快门接上了（180° → 强度 1.0）",
              abs(sh - CAM_SHUTTER) < 0.01 and abs(blur - 1.0) < 0.01,
              "sh=%.1f blur=%.2f" % (sh, blur))

        # ---- A5 退出还原 ----
        # ⚠ 这里原本还有一条"预览态下右键拖不动相机"，**已删除** ——
        #   否定测试（拆掉 `handleNavigation` 的门禁）证明它照样绿，
        #   原因见文件头"故意不测"那一节。
        if not kit.click_in_panel(hwnd, tap, "CINE-PREVIEW-BTN", "CINE-WIN")[0]:
            check("A5 点得到【退出预览】按钮", False)
        deadline = time.time() + 10.0
        m_off = None
        while time.time() < deadline:
            m_off = last_match(tap, PREVIEW_RE)
            if m_off and m_off.group(1) == "0":
                break
            time.sleep(0.15)
        check("A5 退出预览（CINE-PREVIEW on=0）",
              bool(m_off) and m_off.group(1) == "0",
              m_off.group(0) if m_off else "没有 on=0")
        restored = tap.has_line("CINE-PREVIEW-LENS restored=1 dof=0")
        check("A5b 退出时把进入前的镜头效果还回去了（dof 回到 0）",
              restored, "" if restored else "缺 restored=1 dof=0 那一行")
    finally:
        try:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        except Exception:                                        # noqa: BLE001
            pass
        try:
            proc.wait(timeout=40)
        except subprocess.TimeoutExpired:
            proc.kill()
        safe_remove(SCENE_PATH)

    print("-" * 66)
    for n in PASSES:
        print("PASS  %s" % n)
    for n in FAILS:
        print("FAIL  %s" % n)
    if FAILS:
        print("结果: FAIL: %d 项未通过" % len(FAILS))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
