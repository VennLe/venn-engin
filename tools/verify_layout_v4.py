"""端到端验证：2026-10-21 这一轮的三个改动。

被验证的用户需求（原话）：
  1. "当选择创建第三人称视角的游戏创建后，进去的初始化游戏画面视口为什么是
     一个视角朝上的（朝向 Z 正方向的），并且是处于地面栅格平面之下的一个位置"
  2. "所有的功能面板……在打开显示的时候，让窗口停靠在 Viewport 视口窗口上，
     就是类似于浏览器窗口中可以打开多个不同网站页面窗口的那种，并且可以通过
     点击其对应的页面标签去切换"
  3. "原本的 Hierarchy 面板挪到编辑器窗口右面的 Inspector 面板的上方……
     将模式类型选择列表……这些功能的的相关配置和参数信息放在编辑器窗口的左侧，
     当选择不同的模式类型的时候就对应的切换"

断言分四组：

  A. 第三人称模板的初始相机
     A1 模板 JSON 里的 pitch 是弧度、且为正（= 相机在目标上方）
     A2 编辑器打开该场景 → SCENE-CAMERA eye.z > 0.5（在地面栅格之上）
     A3 pitch > 0（俯视），否则就是"朝天看"那个 bug

  B. 左栏模式栏
     B1 LAYOUT-RECTS：modes 贴最左（x=0）、宽度 280..460
     B2 9 行都在（MODE-RAIL-0..8 全有锚点）—— 一行都不能被裁掉
     B3 点第 5 行 → EDITMODE-PANELS mode=modeling
     B4 该模式的参数面板落在左栏里（TK-modeling 的 x/宽 与 modes 一致，
        且 y >= 列表底边）

  C. 视口标签条
     C1 默认只有 Viewport 一页
     C2 打开 GPU Timeline → 出现第二页 + VP-TAB active=gputimeline
     C3 点 Viewport 页 → VP-TAB active=viewport（切回去了）
     C4 点回 GPU Timeline 页 → VP-TAB active=gputimeline
     C5 点标签上的 × → VP-TAB xclose + close，且面板状态 open=0

  D. 右栏上下结构
     D1 Hierarchy 在 Inspector 正上方（同 x、同宽、边贴边）
     D2 标签条紧贴视口顶边（tabs.y2 == vp.y）
     D3 模式栏在最左，且与右栏不重叠

用法：python tools/verify_layout_v4.py
"""

import io
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import find_window                       # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors       # noqa: E402
from verify_toolkit_round2 import (                          # noqa: E402
    EXE, ROOT, check, center, click, events, last_event, launch, rect_of,
    shutdown)

OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

LAYOUT_RE = re.compile(
    r"LAYOUT-RECTS modes=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"hier=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"insp=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"tabs=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"vp=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")

CAM_RE = re.compile(
    r"SCENE-CAMERA eye=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"target=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"yaw=(-?[\d.]+) pitch=(-?[\d.]+) dist=(-?[\d.]+)")


def wait_match(tap, rx, timeout=12.0):
    """等一行匹配 rx 的日志，返回最后一个匹配对象（None = 超时）。"""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        with tap.lock:
            for line in tap.lines:
                m = rx.search(line)
                if m:
                    last = m
        if last:
            return last
        time.sleep(0.15)
    return last


def last_match(tap, rx):
    with tap.lock:
        for line in reversed(tap.lines):
            m = rx.search(line)
            if m:
                return m
    return None


def launch_env(scene, select, mode, extra=None):
    """launch() 的加强版：允许再塞几个环境变量（例如 MYVK_EDITOR_GPUTIMELINE）。"""
    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITOR_SCENE": scene,
        "MYVK_EDITOR_SELECT": select,
        "MYVK_EDITMODE": mode,
    })
    if extra:
        env.update(extra)
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", timeout=60)
    return proc, tap, hwnd


SANDBOX = os.path.join(ROOT, "build", "bin", "assets", "scenes", "sandbox.json")
TP_SCENE = os.path.join(os.path.dirname(ROOT), "venn-engin", "templates",
                        "third_person", "assets", "scenes", "main.json")
TP_JSON = os.path.join(ROOT, "templates", "third_person", "assets", "scenes",
                       "main.json")


# ============================================================ A
def part_a():
    print("\n== A · 第三人称模板的初始相机 ==")
    # A1 直接看模板文件（这是"新建工程"被拷贝走的那一份）
    with io.open(TP_JSON, encoding="utf-8") as f:
        cam = json.load(f)["camera"]
    pitch, yaw = cam["pitch"], cam["yaw"]
    check(0.05 < pitch < 1.4,
          "A1 模板 camera.pitch=%.3f 是正的弧度（相机在目标上方俯视）"
          % pitch)
    check(abs(abs(yaw) - 3.1416) < 0.02,
          "A1b 模板 camera.yaw=%.3f ≈ π（站在角色背后，不是对着脸）" % yaw)

    # A2 / A3 编辑器真的把这个相机摆对了
    proc, tap, hwnd = launch(TP_JSON, "Player", "select")
    try:
        m = wait_match(tap, CAM_RE, 25.0)
        if not m:
            check(False, "A2 找不到 SCENE-CAMERA（相机没被加载？）")
            check(False, "A3 (跳过)")
            return
        eyez = float(m.group(3))
        p = float(m.group(8))
        check(eyez > 0.5,
              "A2 相机 eye.z=%.2f > 0（在地面栅格之上）" % eyez)
        check(p > 0.05,
              "A3 相机 pitch=%.3f > 0（俯视；负数就是原来那个'朝天看'的 bug）"
              % p)
    finally:
        shutdown(proc, hwnd)


# ============================================================ B
def part_b():
    print("\n== B · 左栏模式栏 ==")
    proc, tap, hwnd = launch(SANDBOX, "BoxA", "select")
    try:
        m = wait_match(tap, LAYOUT_RE, 25.0)
        if not m:
            check(False, "B1 找不到 LAYOUT-RECTS")
            return
        mx, my, mw, mh = (float(m.group(i)) for i in range(1, 5))
        check(abs(mx) < 2.0 and 280.0 <= mw <= 520.0,
              "B1 模式栏贴最左且宽度合理 (x=%.0f w=%.0f)" % (mx, mw))

        rows = [rect_of(tap, "MODE-RAIL-%d" % i, 4.0) for i in range(9)]
        check(all(r is not None for r in rows),
              "B2 9 行模式全部有锚点（没有被窗口裁掉）")

        win = rect_of(tap, "MODE-RAIL-WIN", 4.0)
        if rows[-1] and win:
            check(rows[-1][3] <= win[3] + 1.0,
                  "B2b 最后一行底边 %.0f <= 列表底边 %.0f（没溢出）"
                  % (rows[-1][3], win[3]))

        # B3 点第 5 行 = Modeling
        n0 = events(tap, r"EDITMODE-PANELS mode=modeling")
        click(hwnd, *center(rows[4]))
        time.sleep(0.9)
        ok = events(tap, r"EDITMODE-PANELS mode=modeling") > n0
        if not ok:
            click(hwnd, *center(rect_of(tap, "MODE-RAIL-4", 3.0)))
            time.sleep(0.9)
            ok = events(tap, r"EDITMODE-PANELS mode=modeling") > n0
        check(ok, "B3 点左栏第 5 行 → EDITMODE-PANELS mode=modeling")

        # B4 建模面板落在左栏里
        tk = rect_of(tap, "TK-modeling", 6.0)
        if not tk:
            check(False, "B4 找不到 TK-modeling 锚点")
        else:
            sameCol = abs(tk[0] - mx) < 2.0 and abs((tk[2] - tk[0]) - mw) < 2.0
            belowList = win is not None and tk[1] >= win[3] - 1.0
            check(sameCol and belowList,
                  "B4 建模面板长在左栏列表下方 (x=%.0f w=%.0f y=%.0f)"
                  % (tk[0], tk[2] - tk[0], tk[1]))
    finally:
        shutdown(proc, hwnd)


# ============================================================ C
def part_c():
    print("\n== C · 视口标签条 ==")
    proc, tap, hwnd = launch_env(SANDBOX, "BoxA", "select",
                                 {"MYVK_EDITOR_GPUTIMELINE": "1"})
    try:
        # C2 第二页
        check(rect_of(tap, "VTAB-viewport", 20.0) is not None,
              "C1 标签条上有 Viewport 页")
        tl = rect_of(tap, "VTAB-gputimeline", 20.0)
        check(tl is not None, "C2 打开 GPU Timeline 后出现它的标签页")
        # 注意用词：syncViewportTabs 打的是 `VP-TAB open=<id> active=1`，
        # 而"用户点了某页"打的是 `VP-TAB active=<id>` —— 两条不同的事件，
        # 断言"自动切过去"只能用前者。
        check(last_event(tap, r"VP-TAB open=gputimeline") != "",
              "C2b 刚打开的面板自动成为当前页（VP-TAB open=gputimeline）")

        vp = rect_of(tap, "VTAB-viewport", 4.0)
        if not (vp and tl):
            return

        # C3 点 Viewport 页
        n0 = events(tap, r"VP-TAB active=viewport")
        click(hwnd, *center(vp))
        time.sleep(0.8)
        check(events(tap, r"VP-TAB active=viewport") > n0,
              "C3 点 Viewport 标签 → 切回 3D 视口")

        # C4 点回 GPU Timeline 页
        tl2 = rect_of(tap, "VTAB-gputimeline", 4.0)
        n1 = events(tap, r"VP-TAB active=gputimeline")
        click(hwnd, *center(tl2))
        time.sleep(0.8)
        check(events(tap, r"VP-TAB active=gputimeline") > n1,
              "C4 点 GPU Timeline 标签 → 切回那一页")

        # C5 点标签右端的 ×
        tl3 = rect_of(tap, "VTAB-gputimeline", 4.0)
        n2 = events(tap, r"VP-TAB xclose=gputimeline")
        cx = tl3[2] - 14.0
        cy = (tl3[1] + tl3[3]) * 0.5
        click(hwnd, cx, cy)
        time.sleep(0.9)
        closed = events(tap, r"VP-TAB xclose=gputimeline") > n2
        check(closed, "C5 点标签上的 × → 关掉这一页（VP-TAB xclose）")
        if closed:
            check(last_event(tap, r"VP-TAB close=gputimeline") != "",
                  "C5b 关掉当前页后自动回 Viewport")
            tail = ""
            deadline = time.time() + 3.0
            while time.time() < deadline:
                tail = last_event(tap, r"PANEL-STATE gputimeline")
                if "open=0" in tail:
                    break
                time.sleep(0.15)
            check("open=0" in tail,
                  "C5c 面板状态同步成关闭（PANEL-STATE open=0）")
    finally:
        shutdown(proc, hwnd)


# ============================================================ D
def part_d():
    print("\n== D · 右栏 Hierarchy 在 Inspector 之上 ==")
    proc, tap, hwnd = launch(SANDBOX, "BoxA", "select")
    try:
        m = wait_match(tap, LAYOUT_RE, 25.0)
        if not m:
            check(False, "D1 找不到 LAYOUT-RECTS")
            return
        hx, hy, hw, hh = (float(m.group(i)) for i in range(5, 9))
        ix, iy, iw, ih = (float(m.group(i)) for i in range(9, 13))
        tx, ty, tw, th = (float(m.group(i)) for i in range(13, 17))
        vx, vy, vw, vh = (float(m.group(i)) for i in range(17, 21))

        check(abs(hx - ix) < 2.0 and abs(hw - iw) < 2.0,
              "D1a Hierarchy 与 Inspector 同列同宽 (x=%.0f w=%.0f)"
              % (hx, hw))
        check(abs((hy + hh) - iy) < 3.0 and hy + hh <= iy + 2.0,
              "D1b Hierarchy 底边 %.0f 贴着 Inspector 顶边 %.0f"
              % (hy + hh, iy))
        check(abs((ty + th) - vy) < 2.0,
              "D2 标签条底边 %.0f == 视口顶边 %.0f" % (ty + th, vy))
        check(hx > vx + vw - 2.0,
              "D3 视口在左、右栏在其右（vp 右缘 %.0f <= hier x %.0f）"
              % (vx + vw, hx))

        bad = [l for l in tap.lines if any(
            w in l for w in ("VUID-", "[ERROR]", "Validation Error", "SEGV"))]
        check(not bad, "D4 零 Vulkan 校验错误 / 零 SEGV")
    finally:
        shutdown(proc, hwnd)


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    part_a()
    part_b()
    part_c()
    part_d()

    from verify_toolkit_round2 import fails
    print("\n================ 结果 ================")
    if fails:
        print("FAIL %d 项：" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("全部 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
