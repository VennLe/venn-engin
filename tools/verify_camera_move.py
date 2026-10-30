"""运镜体系（Cinematography）验收。

    python tools/verify_camera_move.py [--no-gui]

三组：

    A 组（内核，无窗口）  `Game.exe --selftest cameramove`
                          —— 动词增量 / 镜头光学 / 新通道隔离 / 欧拉闭环 /
                             路径弧长参数化 / 快门换算，共 69 条。
    B 组（面板，GUI）     运镜面板真的能打开、能选中相机、**点动词画面真的动**
                          （两次点击的 yaw 差 = ±15°，这两条一起才排除
                           "按钮有矩形但没连上"）。
    C 组（序列化回路）    场景 JSON 里的 `camera` / `cameraPath` 读回来后，
                          面板状态行 `CINE-STATE` 里的数字必须与文件一致
                          （focal=35 / shutter=180 / dof=1 / path≠0）。

⚠ 为什么每组都要有：A 组证明"算子算得对"，但算子对了不等于**界面点得到**
（记忆里那条"界面点不到 = 没交付"）；C 组证明"存下来的东西读得回来" ——
组件加了字段却忘记读回是这类功能最常见的一种"看起来做完了"。

用法：
    python tools/verify_camera_move.py             # 全跑
    python tools/verify_camera_move.py --no-gui    # 只跑 A 组（CI / 无桌面）
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit                                        # noqa: E402
from capture_window import find_window                             # noqa: E402
from safe_fs import safe_remove                                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")
OUT = os.path.join(os.path.dirname(ROOT), "_venn_verify", "cine")
SCENE_NAME = "scenes/_cine_fixture.json"   # resolveAssetPath 相对 assets/ 根
SCENE_PATH = os.path.join(BIN, "assets", "scenes", "_cine_fixture.json")

FAILS = []
PASSES = []


def check(what, ok, detail=""):
    if ok:
        PASSES.append(what)
        kit.log("  [PASS] %s%s" % (what, ("  — " + detail) if detail else ""))
    else:
        FAILS.append(what)
        kit.log("  [FAIL] %s%s" % (what, ("  — " + detail) if detail else ""))
    return ok


# ---------------------------------------------------------------- 夹具场景
#
# 三台实体：
#   CineCam   带 CameraComponent（焦距 35 / 快门 180° / 景深开 / 绑路径）
#   CamPath   带 CameraPathComponent（三个点，progress=0.25，speed=0）
#   Subject   被注视目标（lookTargetGuid 指它）
#
# ⚠ guid 必须是 **16 位 hex**（记忆里"hex 补满 16 位"）：少一位就是 0，
#   而 0 = "没绑"，于是 path/look 会在加载后静默失效。
GUID_CAM = "c1a0000000000001"
GUID_PATH = "c1a0000000000002"
GUID_SUBJ = "c1a0000000000003"


def write_fixture():
    os.makedirs(os.path.dirname(SCENE_PATH), exist_ok=True)
    doc = {
        "version": 1,
        "generator": "verify_camera_move.py",
        "camera": {"target": [0.0, 0.0, 1.5], "yaw": 0.0, "pitch": 0.25,
                   "distance": 8.0, "fov": 45.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.0, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": False, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            {"guid": GUID_CAM, "name": "CineCam", "parent": -1,
             "transform": {"position": [-6.0, 0.0, 3.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "visible": True, "castShadow": False,
             "camera": {"fov": 50.0, "priority": 10, "enabled": True,
                        # 镜头段：这一组是 C 组要逐项核对的东西
                        "focal": 35.0, "sensorHeight": 24.0,
                        "shutterAngle": 180.0,
                        "dofEnabled": True, "fStop": 2.0,
                        "focusDistance": 6.0,
                        "lookTargetGuid": GUID_SUBJ,
                        "pathGuid": GUID_PATH}},
            {"guid": GUID_PATH, "name": "CamPath", "parent": -1,
             "transform": {"position": [0.0, 0.0, 0.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "visible": True, "castShadow": False,
             "cameraPath": {"points": [-6.0, 0.0, 2.0,
                                       0.0, 3.0, 2.0,
                                       6.0, 0.0, 2.0],
                            "closed": False, "progress": 0.25,
                            "speed": 0.0, "lookAlong": True,
                            "yawOffsetDeg": 0.0}},
            {"guid": GUID_SUBJ, "name": "Subject", "parent": -1,
             "transform": {"position": [0.0, 0.0, 1.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "visible": True, "castShadow": False},
        ],
    }
    with open(SCENE_PATH, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)


# ---------------------------------------------------------------- A 组
TOTAL_RE = re.compile(r"CAMMOVE-TOTAL pass=(\d+) fail=(\d+)")


def run_selftest():
    kit.log("---- A 组：运镜内核自检 ----")
    proc = subprocess.run([GAME, "--selftest", "cameramove"], cwd=BIN,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=180)
    out = proc.stdout or ""
    m = None
    for line in out.splitlines():
        mm = TOTAL_RE.search(line)
        if mm:
            m = mm
    if not m:
        check("A0 自检跑出 CAMMOVE-TOTAL", False, "输出结尾没有总和行")
        return
    passed, failed = int(m.group(1)), int(m.group(2))
    check("A0 自检无失败项", failed == 0, "pass=%d fail=%d" % (passed, failed))
    check("A1 自检条目数 >= 69", passed >= 69, "pass=%d" % passed)

    # 各组代表条目必须**真的出现过**：只数总条数会让"某组整段没跑"看不出来。
    must = [
        "A1b Pan 推两次 = 30°（增量而非设值）",
        "A2e yaw=90° 后 Dolly 走 +Y（跟着相机转）",
        "B1 50mm/24mm → 26.99°",
        "C1 roll=0 时 viewMatrix 逐位等于 lookAt（老画面不动）",
        "D1 25 组 (yaw,pitch,roll) 写回实体后局部 +X 仍是视线",
        "D2 同理局部 +Z 仍是 tilt 后的 up（roll 没丢）",
        "E1a 弧长参数下每步位移均匀（离散度 < 3%）",
        "E1b 反向对照：按参数采样时位移极不均匀（> 100%）",
        "F1a 180° → 强度 1.0（电影标准 180 degree rule）",
        "F2 36 组 (yaw,pitch,roll) 写回实体后 roll 能原样读回来",
    ]
    missing = [s for s in must if s not in out]
    check("A2 六组代表条目都出现过", not missing,
          "缺=%s" % (missing or "-"))


# ---------------------------------------------------------------- B / C 组

VERB_RE = re.compile(
    r"CINE-VERB action=(\S+) amount=(-?[\d.]+) pos=\((-?[\d.]+),(-?[\d.]+),"
    r"(-?[\d.]+)\) yaw=(-?[\d.]+) pitch=(-?[\d.]+)")
STATE_RE = re.compile(
    r"CINE-STATE sel=(\S+) focal=(-?[\d.]+) fov=(-?[\d.]+) "
    r"fovEff=(-?[\d.]+) shift=\((-?[\d.]+),(-?[\d.]+)\) roll=(-?[\d.]+) "
    r"dof=(\d) fstop=(-?[\d.]+) shutter=(-?[\d.]+) look=(\S+) "
    r"damp=(-?[\d.]+) hand=(-?[\d.]+)/(-?[\d.]+) path=(\S+)")


def last_match(tap, rx):
    with tap.lock:
        for line in reversed(tap.lines):
            m = rx.search(line)
            if m:
                return m
    return None


def scroll_into_view(hwnd, tap, tag, win_tag, tries=14, step=120):
    """把 `tag` 滚进 `win_tag` 的可视竖直范围，返回最终矩形（失败 None）。

    ⚠ 为什么必须做：ImGui 的折叠区一多，面板内容就比停靠后的窗口高。滚出
      可视区的控件 **矩形照样会被 logRect 打出来**（它只按坐标去重，不判
      可见性）—— 照着那个坐标点，点到的是窗口外面/别的控件，而日志里
      一条错都不会有。
    """
    from capture_window import user32
    wr = tap.last_rect(win_tag)
    if not wr:
        return None
    cx = int((wr[0] + wr[2]) * 0.5)
    cy = int((wr[1] + wr[3]) * 0.5)
    for _ in range(tries):
        r = tap.last_rect(tag)
        if r and r[1] >= wr[1] + 4 and r[3] <= wr[3] - 4:
            return r
        kit.focus_window(hwnd)
        screen = kit.client_to_screen(hwnd, cx, cy)
        user32.SetCursorPos(screen[0], screen[1])
        time.sleep(0.05)
        # MOUSEEVENTF_WHEEL = 0x0800，负 delta = 向下滚
        user32.mouse_event(0x0800, 0, 0, -step, 0)
        time.sleep(0.35)
    return None


def run_gui():
    kit.log("---- B 组：运镜面板（GUI）----")
    write_fixture()

    env = kit.clean_env({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_PANELS": "cine",
        "MYVK_EDITOR_SCENE": SCENE_NAME,   # 相对名 + cwd=bin（绝对路径会打不开）
        "MYVK_GRID": "0",
        "MYVK_EDITOR_NO_HUD": "1",
    })
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)
    hwnd = find_window(kit.EDITOR_TITLE, 60.0)
    if not hwnd:
        check("B0 编辑器窗口出现", False)
        proc.kill()
        return
    try:
        from capture_window import user32
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 1000, 0x0002 | 0x0004)
        time.sleep(1.5)
        kit.focus_window(hwnd)
    except Exception as e:                                        # noqa: BLE001
        kit.log("WARN 窗口准备失败 %s" % e)

    try:
        rect = tap.wait_rect("CINE-WIN", 40.0)
        if not check("B1 运镜面板打开（CINE-WIN）", rect is not None,
                     "rect=%s" % (rect,)):
            return

        # ---- 选中相机（点列表里那一行）----
        n_state = tap.count_line("CINE-STATE")
        if not kit.click_anchor(hwnd, tap, "CINE-CAM-CineCam"):
            check("B3 点得到相机行（CINE-CAM-CineCam）", False)
            return
        ok = tap.count_line("CINE-STATE") > n_state
        deadline = time.time() + 8.0
        while not ok and time.time() < deadline:
            time.sleep(0.2)
            ok = tap.count_line("CINE-STATE") > n_state
        st = last_match(tap, STATE_RE)
        check("B3 点相机行后状态行出现且 sel=CineCam",
              ok and st is not None and st.group(1) == "CineCam",
              "st=%s" % (st.group(0)[:90] if st else "-"))

        # ---- 选中之后四段的锚点才画得出来（镜头/机位/路径/定稿都在
        #      `cur()` 非空的分支里）----
        anchors = ["CINE-FOCAL", "CINE-DOF", "CINE-SHUTTER", "CINE-LOOK",
                   "CINE-AMOUNT", "CINE-VERB-PAN", "CINE-VERB-ORBIT",
                   "CINE-BAKE"]
        got = {}
        for a in anchors:
            got[a] = tap.wait_rect(a, 6.0)
        missing_a = [a for a, r in got.items() if r is None]
        check("B2a 八个锚点都画出来了", not missing_a, "缺=%s" % (missing_a or "-"))
        # 水平越界 = 被面板裁掉（点了会点到别处）。竖直方向允许在折叠区
        # 下面 —— 那种只要滚一下就能点到，B7 会真的滚。
        horiz = {a: (r is not None and r[0] >= rect[0] - 4 and
                     r[2] <= rect[2] + 4) for a, r in got.items()}
        check("B2b 锚点横向都在面板内（没被裁掉）",
              all(horiz.values()),
              "越界=%s" % ([a for a, ok in horiz.items() if not ok] or "-"))

        # ---- C 组：序列化回路（同一份状态行，逐项对夹具里的值）----
        if st:
            focal = float(st.group(2))
            fov_eff = float(st.group(4))
            dof = int(st.group(8))
            shutter = float(st.group(10))
            look = st.group(11)
            path = st.group(15)
            check("C1 focal 读回 = 35", abs(focal - 35.0) < 0.01, "%.2f" % focal)
            check("C2 shutter 读回 = 180", abs(shutter - 180.0) < 0.01,
                  "%.1f" % shutter)
            check("C3 dofEnabled 读回 = 1", dof == 1, "%d" % dof)
            check("C4 pathGuid 读回（非 0）", path not in ("0", "-", "0x0"),
                  "path=%s" % path)
            check("C5 lookTargetGuid 读回（非 0）",
                  look not in ("0", "-", "0x0"), "look=%s" % look)
            # fov 应该由焦距算出来（35mm / 24mm 传感器 ≈ 37.8°），
            # 而不是文件里那个 50 —— 这条钉的是"焦距优先"真的生效了。
            # fovEff 应该由焦距算出来（35mm / 24mm 传感器 ≈ 37.8°），
            # 而不是文件里那个 50 —— 这条钉的是"焦距优先"真的生效了。
            # ⚠ 别拿 `fov` 那个字段判：装了定焦镜头之后它是**残留值**。
            check("C6 fovEff 由焦距算出（≈37.8°，不是文件里的 50）",
                  abs(fov_eff - 37.8) < 1.5, "fovEff=%.2f" % fov_eff)
        else:
            for tag in ("C1", "C2", "C3", "C4", "C5", "C6"):
                check("%s 状态行缺失无法核对" % tag, False)

        # ---- B4/B5：点动词，画面真的动（两次增量 + 符号）----
        n_verb = tap.count_line("CINE-VERB action=")
        if not kit.click_anchor(hwnd, tap, "CINE-VERB-PAN"):
            check("B4 点得到 Pan + 按钮", False)
            return
        deadline = time.time() + 8.0
        while tap.count_line("CINE-VERB action=") <= n_verb and time.time() < deadline:
            time.sleep(0.1)
        m1 = last_match(tap, VERB_RE)
        check("B4 点 Pan + 后打出一条 CINE-VERB",
              tap.count_line("CINE-VERB action=") > n_verb and m1 is not None,
              "n=%d" % tap.count_line("CINE-VERB action="))
        yaw1 = float(m1.group(6)) if m1 else 0.0
        amt1 = float(m1.group(2)) if m1 else 0.0
        check("B5 Pan + 的幅度 = +15°", abs(amt1 - 15.0) < 0.01,
              "amount=%.2f" % amt1)

        n_verb2 = tap.count_line("CINE-VERB action=")
        kit.click_anchor(hwnd, tap, "CINE-VERB-PAN-NEG")
        deadline = time.time() + 8.0
        while tap.count_line("CINE-VERB action=") <= n_verb2 and time.time() < deadline:
            time.sleep(0.1)
        m2 = last_match(tap, VERB_RE)
        yaw2 = float(m2.group(6)) if m2 else 0.0
        dy = yaw2 - yaw1
        while dy > 180.0:
            dy -= 360.0
        while dy < -180.0:
            dy += 360.0
        check("B6 Pan − 让 yaw 回去 15°（增量是相对当前姿态，不是绝对值）",
              abs(dy + 15.0) < 0.6, "dyaw=%.2f" % dy)

        # ---- B7：Bake 写回实体 ----
        #
        # ⚠ 必须先把按钮滚进可视区：面板内容比停靠后的窗口高，按钮在折叠区
        #   下面时**矩形照样存在**，点下去却点到别处（记忆里那条"有矩形却
        #   点不到"）。判据用"滚动之后矩形真的落进窗口 y 范围"来确认。
        n_bake = tap.count_line("CINE-BAKE pos=")
        if not scroll_into_view(hwnd, tap, "CINE-BAKE", "CINE-WIN"):
            check("B7a 能把 Bake 按钮滚进可视区", False)
        else:
            check("B7a 能把 Bake 按钮滚进可视区", True)
        kit.click_anchor(hwnd, tap, "CINE-BAKE")
        deadline = time.time() + 8.0
        while tap.count_line("CINE-BAKE pos=") <= n_bake and time.time() < deadline:
            time.sleep(0.1)
        check("B7 Bake 写回实体（CINE-BAKE pos= 落日志）",
              tap.count_line("CINE-BAKE pos=") > n_bake,
              "n=%d" % tap.count_line("CINE-BAKE pos="))

        # ---- B8：无验证层报错 ----
        bad = [x for x in tap.text() if "VUID" in x or "VALIDATION" in x]
        check("B8 无 VUID / VALIDATION 报错", not bad,
              "首条=%s" % (bad[0][:100] if bad else "-"))
    finally:
        try:
            kit.shutdown(proc, hwnd, wait=30.0)
        except Exception:                                         # noqa: BLE001
            proc.kill()
        # 夹具留在 build/bin（回归跑下一次会重写），但别让它进版本库
        if os.path.exists(SCENE_PATH):
            safe_remove(SCENE_PATH)


def run_runtime():
    """D 组：**运行时**才有的两条链路 —— 路径求值 + 镜头段接渲染效果。

    为什么要单独一组：`CameraSystem` 只在 Game.exe 里被 `update` 驱动，
    编辑器视口用的是自由相机。也就是说 B 组全绿也可能出现"进游戏路径不走、
    景深不生效"，而这两条恰好是这套体系里**只有运行时才成立**的部分。
    """
    kit.log("---- D 组：运行时（路径求值 + 镜头效果）----")
    if not os.path.exists(SCENE_PATH):
        write_fixture()
    env = kit.clean_env({
        "MYVK_RUNTIME_TRACE_STATES": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_GRID": "0",
    })
    proc = subprocess.Popen([GAME, "--scene", "scenes/_cine_fixture.json"],
                            cwd=BIN, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)
    try:
        ok_path = tap.wait_line("CAM-PATH", 40.0)
        check("D1 运行时路径求值（CAM-PATH 落日志）", bool(ok_path))
        # ⚠ CAM-LENS 是**变化才打**：第一帧一定会打一条（含 dof / shutter）
        m = None
        deadline = time.time() + 8.0
        while m is None and time.time() < deadline:
            m = last_match(tap, re.compile(
                r"CAM-LENS cam=(\S+) dof=(\d) focus=([\d.]+) ap=([\d.]+) "
                r"sh=([\d.]+) blur=([\d.]+)"))
            if m is None:
                time.sleep(0.2)
        check("D2 镜头段接到渲染效果（CAM-LENS）", m is not None)
        if m:
            check("D3 景深开（dof=1）", m.group(2) == "1", "dof=%s" % m.group(2))
            check("D4 快门 180° → 模糊强度 1.0",
                  abs(float(m.group(6)) - 1.0) < 0.01, "blur=%.2f" % float(m.group(6)))
    finally:
        # Game.exe 的窗口标题不是 kit 里的常量（那两条是 Launcher / Editor），
        # 所以直接关进程 —— 这组只关心日志，不需要走"正常关窗"那条路。
        try:
            proc.terminate()
            proc.wait(timeout=20)
        except Exception:                                         # noqa: BLE001
            proc.kill()
        if os.path.exists(SCENE_PATH):
            safe_remove(SCENE_PATH)


def main():
    no_gui = "--no-gui" in sys.argv
    os.makedirs(OUT, exist_ok=True)
    run_selftest()
    if not no_gui:
        run_gui()
        run_runtime()

    kit.log("")
    kit.log("通过 %d 项，失败 %d 项" % (len(PASSES), len(FAILS)))
    if FAILS:
        kit.log("结果: FAIL: " + "; ".join(FAILS))
        return 1
    kit.log("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
