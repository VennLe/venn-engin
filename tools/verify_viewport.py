"""端到端验证：E14 视口四角浮层 + 相机书签 + 浮层显隐（docs/editor_roadmap.md）。

交付物（见 editor/ViewportPanel.cpp 的 "E14 浮层" 一段）：

  · 四角分工：左上 = 视图模式条 / 右上 = 统计 HUD /
              左下 = 相机信息（pos·look·FOV·Sens·Grid + 9 个书签）/
              右下 = 导航 gizmo（E14 起从原左下角搬过来）；
  · 两层显隐：`m_showOverlay`（工具栏 HUD 勾选框 / MYVK_EDITOR_NO_HUD）是总闸，
    总闸之上四块各自一个开关（`EditorContext::ViewportOverlay`），
    经右键空白处的 "Viewport Display" 菜单切换并随偏好落盘；
  · 统计 HUD 逐行开关（`StatsHudItems` 10 项）；
  · Shift 层快捷键：Shift+1..8 切视图模式 / Ctrl+Shift+1..9 存书签 /
    Alt+Shift+1..9 跳书签（数字键本体已被 Blender 视图方向与 Alt+面板显隐占满）。

断言分组：

  V0 UI 存在性    VP-MODEBAR modes=8 current=lit；modebar / stats / cam 三块
                  的矩形日志都在；NAV-BALL+X 出现（导航 gizmo 在画）。
  V4 模式条快捷键 Shift+2/5/6/1 → unlit / metallic / wireframe / lit
                  （⚠ Shift+N 切的是**模式索引 N-1**，不是"第 N 个按钮"）。
  V1 NO_HUD 确定性 同参数两次抓图，**整窗**（15 个面板 + 视口）逐像素一致。
  V2 全关 == NO_HUD 四个浮层开关全关（走偏好文件）→ 视口内部与 NO_HUD
                  逐像素一致。**这条是 E14 的核心不变量**：总闸之外的四块
                  开关必须把各自的绘制完全关掉，一块都不许漏。
                  （只比视口内部：总闸关 vs 开会让工具栏的 HUD 勾选框
                    换个高亮色，那是 1560 个像素的差异，与浮层无关。）
  V3 改造前基线  与 E14 改造前抓的**整窗**参考图（tools/vp_nohud_baseline.png）
                  逐像素一致 —— 证明这轮重构连面板边框都没碰。
                  ⚠ **E21 起把底部状态栏那一条（`kStatusH = 26` px）排除在外**：
                  状态栏新增了四个常驻项（选中数 / 保存状态 / 后台任务数 /
                  WARN-ERROR 计数），是**按设计**的变化。所以拆成两条：
                  V3 = 状态栏以上整窗逐像素 0 差；V3b = **全部**差异都落在
                  那一条内、且**非零**（有人把 E21 的状态栏改回去 → V3b 变红）。
                  **不重采基线** —— 重采等于把当前二进制的像素无条件盖成
                  "参考"，V3 就退化成"自己跟自己比"，失去意义。
  V4 模式条快捷键 Shift+2 → unlit、Shift+5 → wireframe、Shift+1 → 回 lit。
  V5 书签往返     Ctrl+Shift+1 存槽 1 → 按 3（右视图，改朝角）→ Ctrl+Shift+2
                  存槽 2 → 断言两槽不同（证明按下 3 真的改了相机）→
                  Alt+Shift+1 跳槽 1 → 断言 yaw/pitch 与槽 1 存时一致。
  V6 Display 菜单 右键视口空白处 → VP-DISPLAY-WIN（菜单开了）→ 点
                  "View mode bar" → VP-OVERLAY modeBar=0 且其余三块仍是 1 →
                  点 "Show all overlays" → 四块全回 1。
  V7 偏好落盘往返 正常窗口里关掉 modeBar+stats，优雅关窗（onShutdown 落盘）→
                  偏好 JSON 里 vp.overlay.modeBar/stats == 0 且 vp.stat.* 齐全 →
                  第二程带着这份偏好启动 → VP-OVERLAY 生效、关掉的两块不画。
  V8 零验证层错误 全部会话无 VUID / VALIDATION / ERROR。

⚠ **E21 起的一条隐性耦合**：这些会话里只要出现 `VK_LOG_WARN/ERROR`，右上角就会
  多一条 toast（NotifyCenter 按设计就是要弹的），于是 **V1b / V3 的逐像素比对会
  多出一块差异**。真出现这种情况，先看是不是"渲染/脚本真的报了警告" ——
  那是被测对象的问题，让它先红，别去动阈值。

用法：
    python tools/verify_viewport.py
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground     # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "_venn_verify", "e14")
BASELINE = os.path.join(ROOT, "tools", "vp_nohud_baseline.png")

# 和 capture_nohud.py 抓基线时**完全一样**的窗口几何。
WIN_W, WIN_H = 1600, 900

# 视口内部的一块保守矩形。**不用日志里的 VP-RECT 去裁图** ——
# `logRect` 是按 tag 去抖的（同 tag 只在变化 >4px 时重打一行），而视口首次
# 布局用的是"启动那一刻的窗口尺寸"：先起小窗再 SetWindowPos 的话，首帧会
# 打一行小矩形，之后布局收敛到另一处；日志里留下的是**哪一条取决于
# resize 与首帧的先后**，实测同一份二进制能拿到 (310,184)-(1238,776) 或
# (310,184)-(1260,832) 两种首值（残留证据见 _venn_verify/e14/nohud_baseline.png：
# 那张"参考图"就是按偏高的首值裁的，多带了 56 行工具栏）。
# 结论：像素比对一律用**稳定锚点** ——
#   · 全窗口图（15 个面板都算上，最强）；
#   · 或下面这块必定落在视口内的内部矩形（用于"总闸关 vs 四开关关"这种
#     不能整窗比对、只关心 3D 画面的场景）。
VP_INNER = (330, 200, 1200, 750)

WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CLOSE = 0x0010

VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12
SC_SHIFT = 0x2A
SC_CTRL = 0x1D
SC_ALT = 0x38

# ⚠ GLFW 的 WM_KEYDOWN 是**按扫描码**映射键值的，lParam 里扫描码为 0 会被
# 映射成 GLFW_KEY_UNKNOWN（verify_add_menu 里踩过）。美式键盘布局：
# 主键盘 1..9 → 0x02..0x0A。
SCAN = {0x31: 0x02, 0x32: 0x03, 0x33: 0x04, 0x34: 0x05, 0x35: 0x06,
        0x36: 0x07, 0x37: 0x08, 0x38: 0x09, 0x39: 0x0A}
VK_DIGIT = {i: 0x31 + (i - 1) for i in range(1, 10)}

MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010

VP_RECT_RE = re.compile(r"VP-RECT=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)")
SAVE_RE = re.compile(r"VP-BOOK save slot=(\d+) pos=([-\d.,]+) yaw=([-\d.]+) "
                     r"pitch=([-\d.]+)")
RECALL_RE = re.compile(r"VP-BOOK recall slot=(\d+) yaw=([-\d.]+) "
                       r"pitch=([-\d.]+)")


# ---------------------------------------------------------------- 日志小工具

def tap_count(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def tap_lines(tap):
    with tap.lock:
        return list(tap.lines)


def wait_count(tap, needle, n=1, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        if tap_count(tap, needle) >= n:
            return True
        time.sleep(0.15)
    return False


def last_match(lines, rx):
    out = None
    for l in lines:
        m = rx.search(l)
        if m:
            out = m
    return out


OVERLAY_RE = re.compile(
    r"VP-OVERLAY master=(\d) modeBar=(\d) stats=(\d) camera=(\d) "
    r"navGizmo=(\d)")


def overlay_state(tap):
    """最近一条 VP-OVERLAY 的状态（总闸 + 四个浮层开关）。

    这一行**只在状态变化时**打（ViewportPanel::draw 第 6 步）—— 显隐不能靠
    "矩形日志还在不在打"来判断：`logRect` 对同一矩形只打一条，不是逐帧刷的。
    """
    with tap.lock:
        m = None
        for l in tap.lines:
            mm = OVERLAY_RE.search(l)
            if mm:
                m = mm
    if not m:
        return None
    keys = ["master", "modeBar", "stats", "camera", "navGizmo"]
    return {k: int(m.group(i + 1)) for i, k in enumerate(keys)}


def wait_overlay(tap, pred, timeout=4.0):
    end = time.time() + timeout
    while time.time() < end:
        st = overlay_state(tap)
        if st and pred(st):
            return st
        time.sleep(0.15)
    return overlay_state(tap)


# ---------------------------------------------------------------- 输入注入

def post_key(hwnd, vk):
    """直投窗口的按键 —— 不依赖前台焦点，但要求带扫描码。"""
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (SCAN[vk] << 16) | 1)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (SCAN[vk] << 16) | 0xC0000001)
    time.sleep(0.30)


def press_combo(hwnd, vk, ctrl=False, shift=False, alt=False):
    """修饰键必须是**真实**键盘事件 —— GLFW 的 getMods() 走 GetKeyState，
    PostMessage 出来的修饰键在那里看不见（verify_add_menu 的 Shift+A 同款
    组合：修饰键真实 + 主键 PostMessage）。"""
    if not force_foreground(hwnd):
        print("  WARN: 编辑器未成为前台窗口，修饰键注入可能失效")
    if ctrl:
        user32.keybd_event(VK_CONTROL, SC_CTRL, 0, 0)
    if alt:
        user32.keybd_event(VK_MENU, SC_ALT, 0, 0)
    if shift:
        user32.keybd_event(VK_SHIFT, SC_SHIFT, 0, 0)
    time.sleep(0.15)
    post_key(hwnd, vk)
    if shift:
        user32.keybd_event(VK_SHIFT, SC_SHIFT, 0x0002, 0)
    if alt:
        user32.keybd_event(VK_MENU, SC_ALT, 0x0002, 0)
    if ctrl:
        user32.keybd_event(VK_CONTROL, SC_CTRL, 0x0002, 0)
    time.sleep(0.35)


def press_until(tap, hwnd, vk, needle, tries=4, **mods):
    """按一次不够就再按一次，直到日志里出现期望行。

    注入按键在这台机器上是**固有 flaky** 的（记忆里那条"注入的第一次事件
    被 Windows 拿去做窗口激活"）：实测同一段 Shift+2 / Shift+5 / Shift+1
    里中间那一下会丢。断言要的是"这个快捷键真的能用"，而不是"这一次
    PostMessage 没被系统吃掉"——所以按到生效为止，把次数记进详情。
    """
    for i in range(tries):
        if i == 0 or not tap.has_line(needle):
            press_combo(hwnd, vk, **mods)
        if tap.has_line(needle):
            return i + 1
        time.sleep(0.5)
    return 0


def click_client(hwnd, cx, cy):
    # 真实鼠标事件（mouse_event）只进**前台窗口**队列 —— 不先把编辑器拉到
    # 前台，点下去会落在别的窗口上，而且不报错（只表现为"菜单没弹"）。
    force_foreground(hwnd)
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.25)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.55)


def right_click_client(hwnd, cx, cy):
    """右键**快按快放**（不移动）—— 视口把它当"没拖拽的右键"，松手弹菜单；
    按住拖才是驾驶相机。"""
    force_foreground(hwnd)
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.30)
    user32.mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0)
    time.sleep(0.10)
    user32.mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0)
    time.sleep(0.70)


def click_rect(hwnd, rect):
    click_client(hwnd, (rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5)


# ---------------------------------------------------------------- 会话

class Session:
    def __init__(self, tap, hwnd, proc, lines, errs, vp_rect, img):
        self.tap = tap
        self.hwnd = hwnd
        self.proc = proc
        self.lines = lines
        self.errs = errs
        self.vp_rect = vp_rect
        self.img = img


def launch(exe, env_extra, actions=None, shot=None, window=(WIN_W, WIN_H),
           settle=3.0, ready_needle="VP-RECT modebar=", ready_n=1):
    """起一轮编辑器 → 摆窗口 → 等就绪 → （可选）跑动作 → 抓图 → 收尾。

    ⚠ 窗口在 onInit **之前**就建好了（GLFW create window 是初始化第一步），
    "找到窗口"远不等于"已经跑了几帧" —— 所以必须等**日志里的就绪锚点**，
    不能只看窗口句柄。`ready_n=1` 就够：`logRect` 是按 tag 去抖的，
    同一个矩形只会打一条。
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env.update(env_extra)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    hwnd = None
    img = None
    vp_rect = None
    try:
        for _ in range(60):
            hwnd = find_window("Venn Editor", timeout=2.0)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            return Session(tap, None, proc, tap_lines(tap), ["未找到编辑器窗口"],
                           None, None)

        user32.ShowWindow(hwnd, 9)
        user32.SetForegroundWindow(hwnd)
        time.sleep(1.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, window[0], window[1],
                            0x0002 | 0x0004)
        time.sleep(settle)
        if ready_needle:
            wait_count(tap, ready_needle, ready_n, timeout=25.0)
        time.sleep(0.8)

        if actions:
            actions(tap, hwnd)
            time.sleep(0.6)

        img, _ = capture_client(hwnd)
        if img is not None and shot:
            img.save(shot)

        lines = tap_lines(tap)
        for l in lines:
            m = VP_RECT_RE.search(l)
            if m:
                vp_rect = tuple(int(m.group(i)) for i in range(1, 5))

        errs = [l for l in lines
                if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                    or "Error:" in l)]
        return Session(tap, hwnd, proc, lines, errs, vp_rect, img)
    finally:
        if proc.poll() is None:
            if hwnd:
                user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
                try:
                    proc.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    pass
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    proc.kill()


def launch_and_close_gracefully(exe, env_extra, actions=None, timeout=25.0):
    """**非**自动化运行（不设 MYVK_NO_WINDOW_SAVE / MYVK_FRAMES）：
    这样 onShutdown 里的 savePrefsToDisk() 才会真的写盘。
    动作跑完后发 WM_CLOSE，等进程自己退出（退出码 0 = 走了 onShutdown）。"""
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env.update(env_extra)

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        hwnd = None
        for _ in range(60):
            hwnd = find_window("Venn Editor", timeout=2.0)
            if hwnd:
                break
            time.sleep(0.5)
        if not hwnd:
            return None, ["未找到编辑器窗口"]
        user32.ShowWindow(hwnd, 9)
        user32.SetForegroundWindow(hwnd)
        time.sleep(1.0)
        user32.SetWindowPos(hwnd, 0, 0, 0, WIN_W, WIN_H, 0x0002 | 0x0004)
        time.sleep(3.0)
        wait_count(tap, "VP-RECT modebar=", 2, timeout=25.0)
        time.sleep(0.8)
        if actions:
            actions(tap, hwnd)
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.terminate()
            return None, ["优雅关闭超时（没走 onShutdown？）"]
        lines = tap_lines(tap)
        errs = [l for l in lines
                if ("VUID" in l or "VALIDATION" in l or " ERROR" in l
                    or "Error:" in l)]
        return tap, errs
    finally:
        if proc.poll() is None:
            proc.kill()


# ---------------------------------------------------------------- 像素比对

def crop(img, rect):
    return img.convert("RGB").crop(tuple(int(v) for v in rect))


def diff_stats(a, b):
    import numpy as np
    x = np.asarray(a, dtype=np.int16)
    y = np.asarray(b, dtype=np.int16)
    if x.shape != y.shape:
        return None
    d = np.abs(x - y)
    per = d.max(axis=2)
    return {"pixels": int((per > 0).sum()), "max": int(per.max()),
            "mean": float(d.mean())}


# ---------------------------------------------------------------- 偏好文件

def write_prefs(path, overrides):
    """写一份最小偏好：只给要覆盖的 key，其余靠出厂值。"""
    doc = {"version": 1,
           "prefs": {k: float(v) for k, v in overrides.items()}}
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)


def read_prefs(path):
    with open(path, "r", encoding="utf-8") as f:
        j = json.load(f)
    return j.get("prefs", {})


# ---------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)

    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)

    results = []

    def check(name, cond, detail=""):
        results.append((name, bool(cond), detail))
        print(("PASS  " if cond else "FAIL  ") + name +
              (f"  ({detail})" if detail else ""))

    errs_all = {}

    # ============================================================ V0 / V4 / V5
    # 一次会话里把"存在性 + 快捷键 + 书签"全跑完（都只需要一次启动）
    def act_shortcuts(tap, hwnd):
        # ⚠ Shift+N 映射的是**模式索引 N-1**：Shift+2 → unlit(1)、
        #   Shift+5 → metallic(4)、Shift+6 → wireframe(5)。别按"第几个按钮"想。
        press_until(tap, hwnd, VK_DIGIT[2], "VP-MODEBAR key=2 mode=unlit",
                    shift=True)
        press_until(tap, hwnd, VK_DIGIT[5], "VP-MODEBAR key=5 mode=metallic",
                    shift=True)
        press_until(tap, hwnd, VK_DIGIT[6], "VP-MODEBAR key=6 mode=wireframe",
                    shift=True)
        press_until(tap, hwnd, VK_DIGIT[1], "VP-MODEBAR key=1 mode=lit",
                    shift=True)
        press_until(tap, hwnd, VK_DIGIT[1], "VP-BOOK save slot=1",
                    ctrl=True, shift=True)
        # 中间按一次**不带修饰键**的 3 = Blender 的"右视图"：把朝角改掉，
        # 后面的书签跳转才有可验证的效果（否则 yaw/pitch 本来就一样）。
        # 判据用 NAV-CAM（导航球每帧打一行相机姿态，变化 >0.02 rad 才打）。
        base = tap_count(tap, "NAV-CAM")
        for _ in range(4):
            press_combo(hwnd, VK_DIGIT[3])
            if tap_count(tap, "NAV-CAM") > base:
                break
            time.sleep(0.4)
        time.sleep(0.5)
        press_until(tap, hwnd, VK_DIGIT[2], "VP-BOOK save slot=2",
                    ctrl=True, shift=True)
        press_until(tap, hwnd, VK_DIGIT[1], "VP-BOOK recall slot=1",
                    alt=True, shift=True)

    s = launch(exe, {}, actions=act_shortcuts,
               shot=os.path.join(OUT_DIR, "vp_overlays.png"))
    errs_all["快捷键/书签"] = s.errs
    lines = s.lines

    check("V0a 模式条 8 格（VP-MODEBAR modes=8 current=lit）",
          s.tap.has_line("VP-MODEBAR modes=8 current=lit"),
          "缺 VP-MODEBAR modes=8 current=lit")
    for tag in ("VP-RECT modebar", "VP-RECT stats", "VP-RECT cam"):
        check(f"V0b {tag} 矩形日志在（浮层在画）",
              s.tap.last_rect(tag) is not None)
    check("V0c 导航 gizmo 在画（NAV-BALL+X）",
          s.tap.last_rect("NAV-BALL+X") is not None)

    check("V4a Shift+2 → unlit",
          s.tap.has_line("VP-MODEBAR key=2 mode=unlit"))
    check("V4b Shift+5 → metallic（= 模式索引 4）",
          s.tap.has_line("VP-MODEBAR key=5 mode=metallic"))
    check("V4c Shift+6 → wireframe（= 模式索引 5）",
          s.tap.has_line("VP-MODEBAR key=6 mode=wireframe"))
    check("V4d Shift+1 → 回 lit",
          s.tap.has_line("VP-MODEBAR key=1 mode=lit"))
    ov0 = overlay_state(s.tap)
    check("V4e 启动时四个浮层都在（VP-OVERLAY）",
          ov0 is not None and ov0["master"] == 1 and
          all(ov0[k] == 1 for k in ("modeBar", "stats", "camera", "navGizmo")),
          f"{ov0}")

    saves = {}
    recs = {}
    for l in lines:
        m = SAVE_RE.search(l)
        if m:
            saves[int(m.group(1))] = (float(m.group(3)), float(m.group(4)))
        m = RECALL_RE.search(l)
        if m:
            recs[int(m.group(1))] = (float(m.group(2)), float(m.group(3)))

    check("V5a Ctrl+Shift+1 / +2 各存了一条书签",
          1 in saves and 2 in saves, f"saves={sorted(saves)}")
    if 1 in saves and 2 in saves:
        d = max(abs(saves[1][0] - saves[2][0]),
                abs(saves[1][1] - saves[2][1]))
        check("V5b 中间按下 3（右视图）确实改了相机（两槽不同）", d > 0.05,
              f"槽1 {saves[1]} vs 槽2 {saves[2]}，差 {d:.4f}")
    check("V5c Alt+Shift+1 跳回槽 1", 1 in recs, f"recalls={sorted(recs)}")
    if 1 in saves and 1 in recs:
        dy = abs(saves[1][0] - recs[1][0])
        dp = abs(saves[1][1] - recs[1][1])
        check("V5d 跳回后 yaw/pitch 与存时一致", dy < 1e-3 and dp < 1e-3,
              f"dyaw={dy:.6f} dpitch={dp:.6f}")

    # ============================================================ V1 / V3
    nohud_env = {"MYVK_EDITOR_NO_HUD": "1", "MYVK_NO_WINDOW_SAVE": "1"}
    s1 = launch(exe, nohud_env, shot=os.path.join(OUT_DIR, "vp_nohud_a.png"),
                ready_needle=None, settle=4.0)
    s2 = launch(exe, nohud_env, shot=os.path.join(OUT_DIR, "vp_nohud_b.png"),
                ready_needle=None, settle=4.0)
    errs_all["NO_HUD A"] = s1.errs
    errs_all["NO_HUD B"] = s2.errs
    check("V1a NO_HUD 会话确实没画浮层（无 VP-RECT modebar）",
          not s1.tap.has_line("VP-RECT modebar="))

    if s1.img is not None and s2.img is not None:
        # 整窗比对：15 个面板 + 视口一起比，比"只裁视口"更强，而且**不依赖
        # VP-RECT**（那个 tag 的首值取决于 resize 与首帧的先后，见文件头）。
        d = diff_stats(s1.img, s2.img)
        check("V1b 两次 NO_HUD **整窗**逐像素一致",
              d is not None and d["pixels"] == 0, f"{d}" if d else "尺寸不同")
    else:
        check("V1b 两次 NO_HUD 整窗逐像素一致", False, "截图缺失")

    if s1.img is not None and os.path.exists(BASELINE):
        from PIL import Image
        base = Image.open(BASELINE)
        W, H = s1.img.size
        # E21：状态栏底部那一条**按设计**跟参考图不同了 —— 它新增了四个常驻项
        # （选中数 / `* unsaved` / `tasks N` / `errors N`+`warns N`）。
        # V3 的意图是"连面板边框都没动"，所以把状态栏那一条（客户端底部
        # `kStatusH = 26` px）切掉再整窗比 —— **不重采基线**：重采等于把
        # 当前二进制的像素无条件盖成"参考"，那 V3 就退化成"自己跟自己比"。
        BAND = 26
        cut = H - BAND
        d = diff_stats(crop(base, (0, 0, W, cut)), crop(s1.img, (0, 0, W, cut)))
        check("V3 与 E14 改造前参考图**整窗**逐像素一致（除状态栏那一条）",
              d is not None and d["pixels"] == 0,
              f"参考 {base.size} / 本轮 {s1.img.size}，状态栏以上差 {d}")
        # 差异必须**全部**落在状态栏那一条里（新增的计数不该往外溢），
        # 而且必须是**非零**的（有人把 E21 的状态栏改回去，这条要响）。
        d_bottom = diff_stats(crop(base, (0, cut, W, H)),
                              crop(s1.img, (0, cut, W, H)))
        d_all = diff_stats(base, s1.img)
        check("V3b 差异全部落在状态栏那一条内（E21 新增的常驻计数）",
              d_all is not None and d_bottom is not None and
              d_bottom["pixels"] == d_all["pixels"] and d_bottom["pixels"] > 0,
              f"全窗 {d_all} / 底条 {d_bottom}")
    else:
        check("V3 与 E14 改造前参考图一致", False,
              f"缺参考图 {BASELINE} 或本轮截图")

    # 视口内部矩形必须落在日志报的视口里（布局真变了要让这条先响，
    # 而不是让后面的像素断言给出一个看不懂的差异）
    if s1.vp_rect:
        x0, y0, x1, y1 = s1.vp_rect
        check("V1d VP_INNER 落在视口矩形内",
              x0 < VP_INNER[0] and VP_INNER[2] < x1 and
              y0 < VP_INNER[1] and VP_INNER[3] < y1,
              f"VP-RECT={s1.vp_rect} VP_INNER={VP_INNER}")

    # ============================================================ V2
    # 四个浮层开关全关（走偏好文件）→ 视口内部必须与 NO_HUD 逐像素一致。
    with tempfile.TemporaryDirectory() as td:
        p_all_off = os.path.join(td, "all_off.json")
        write_prefs(p_all_off, {
            "vp.overlay.modeBar": 0, "vp.overlay.stats": 0,
            "vp.overlay.camera": 0, "vp.overlay.navGizmo": 0})
        s3 = launch(exe, {"MYVK_NO_WINDOW_SAVE": "1",
                          "MYVK_EDITOR_PREFS_PATH": p_all_off},
                    shot=os.path.join(OUT_DIR, "vp_alloff.png"),
                    ready_needle=None, settle=4.0)
        errs_all["全浮层关"] = s3.errs
        st3 = overlay_state(s3.tap)
        check("V2a 四个开关都是 0（VP-OVERLAY），总闸仍开",
              st3 is not None and st3["master"] == 1 and
              all(st3[k] == 0 for k in ("modeBar", "stats", "camera",
                                        "navGizmo")), f"{st3}")
        for tag in ("VP-RECT modebar", "VP-RECT stats", "VP-RECT cam"):
            check(f"V2b 关掉后 {tag} 不再出现",
                  not s3.tap.has_line(tag + "="))
        check("V2b 关掉后 NAV-BALL 不再出现",
              not s3.tap.has_line("NAV-BALL+X"))
        if s3.img is not None and s1.img is not None:
            # 只比视口内部：整窗比对会带上工具栏那个 HUD 勾选框的高亮差
            # （总闸关 vs 总闸开，1560 像素，与浮层无关）。
            d = diff_stats(crop(s3.img, VP_INNER), crop(s1.img, VP_INNER))
            check("V2c 四浮层全关 == NO_HUD（视口内部逐像素）",
                  d is not None and d["pixels"] == 0, f"{d}")
        else:
            check("V2c 四浮层全关 == NO_HUD（视口内部逐像素）", False,
                  "截图缺失")

    # ============================================================ V6
    # 显隐判定用 `VP-OVERLAY`（只在状态变化时打一行）。**不用**"矩形日志还在
    # 不在打"—— `logRect` 对同一矩形只打一条（按 tag + 4px 去抖），不会逐帧刷。
    v6 = {}

    def act_display_menu(tap, hwnd):
        rect = tap.last_rect("VP-RECT")
        if not rect:
            return
        # 点在视口靠上的位置（天空），一定不是"按在选中物体上"→ 弹的是
        # Viewport Display 菜单而不是物体菜单。
        cx = int((rect[0] + rect[2]) * 0.5)
        cy = int(rect[1] + (rect[3] - rect[1]) * 0.12)
        right_click_client(hwnd, cx, cy)
        v6["menu"] = tap.last_rect("VP-DISPLAY-WIN") is not None
        r_mb = tap.last_rect("VP-DISPLAY-ITEM modeBar")
        r_st = tap.last_rect("VP-DISPLAY-ITEM stats")
        r_cm = tap.last_rect("VP-DISPLAY-ITEM camera")
        r_gz = tap.last_rect("VP-DISPLAY-ITEM navGizmo")
        r_all = tap.last_rect("VP-DISPLAY-ITEM all")
        v6["items"] = all(r is not None
                          for r in (r_mb, r_st, r_cm, r_gz, r_all))
        if not (v6["menu"] and v6["items"]):
            return

        click_rect(hwnd, r_mb)               # 关掉 View mode bar
        v6["after_off"] = wait_overlay(
            tap, lambda st: st["modeBar"] == 0, timeout=4.0)
        click_rect(hwnd, r_all)              # Show all overlays
        v6["after_all"] = wait_overlay(
            tap, lambda st: all(st[k] == 1
                                for k in ("modeBar", "stats", "camera",
                                          "navGizmo")), timeout=4.0)

    s4 = launch(exe, {"MYVK_NO_WINDOW_SAVE": "1"}, actions=act_display_menu,
                shot=os.path.join(OUT_DIR, "vp_display_menu.png"),
                ready_needle="VP-RECT modebar=")
    errs_all["Display 菜单"] = s4.errs

    check("V6a 右键视口空白处弹出 Viewport Display 菜单", v6.get("menu"))
    check("V6b 菜单里四个浮层开关 + Show all 的矩形都打出来了",
          v6.get("items"))
    off_st = v6.get("after_off")
    check("V6c 点掉 View mode bar → VP-OVERLAY modeBar=0",
          off_st is not None and off_st["modeBar"] == 0, f"{off_st}")
    check("V6d 只关了这一块，其余三块不受影响",
          off_st is not None and off_st["stats"] == 1 and
          off_st["camera"] == 1 and off_st["navGizmo"] == 1, f"{off_st}")
    all_st = v6.get("after_all")
    check("V6e Show all overlays → 四块全回来",
          all_st is not None and
          all(all_st[k] == 1 for k in ("modeBar", "stats", "camera",
                                       "navGizmo")), f"{all_st}")

    # ============================================================ V7
    with tempfile.TemporaryDirectory() as td:
        prefs_path = os.path.join(td, "editor_prefs.json")
        layout_path = os.path.join(td, "editor_layout.json")

        # 会写盘的非自动化运行会顺带把窗口几何写进 build/bin/editor_window.ini
        # 和 imgui.ini —— 先把它们备份出来，跑完还原，别污染用户状态。
        bin_dir = os.path.dirname(os.path.abspath(exe))
        saved_cfg = {}
        for f in ("editor_window.ini", "imgui.ini"):
            p = os.path.join(bin_dir, f)
            if os.path.exists(p):
                saved_cfg[p] = open(p, "rb").read()

        def act_write(tap, hwnd):
            rect = tap.last_rect("VP-RECT")
            if not rect:
                return
            cx = int((rect[0] + rect[2]) * 0.5)
            cy = int(rect[1] + (rect[3] - rect[1]) * 0.12)
            right_click_client(hwnd, cx, cy)
            if not tap.last_rect("VP-DISPLAY-WIN"):
                return
            for item in ("VP-DISPLAY-ITEM modeBar", "VP-DISPLAY-ITEM stats"):
                r = tap.last_rect(item)
                if r:
                    click_rect(hwnd, r)
                time.sleep(0.6)

        try:
            tap_w, errs_w = launch_and_close_gracefully(
                exe, {"MYVK_EDITOR_LAYOUT": layout_path,
                      "MYVK_EDITOR_PREFS_PATH": prefs_path},
                actions=act_write)
            errs_all["偏好写盘"] = errs_w
            check("V7a 非自动化运行走了 onShutdown（进程自己退出）",
                  tap_w is not None, "" if tap_w else "; ".join(errs_w[:2]))
            check("V7b 日志有 'Editor prefs: saved'",
                  tap_w is not None and tap_w.has_line("Editor prefs: saved"))
            check("V7c 偏好文件已生成", os.path.exists(prefs_path), prefs_path)

            prefs = {}
            if os.path.exists(prefs_path):
                prefs = read_prefs(prefs_path)
            check("V7d vp.overlay.modeBar / stats 落盘为 0",
                  prefs.get("vp.overlay.modeBar") == 0 and
                  prefs.get("vp.overlay.stats") == 0,
                  f"modeBar={prefs.get('vp.overlay.modeBar')} "
                  f"stats={prefs.get('vp.overlay.stats')}")
            check("V7e vp.overlay.camera / navGizmo 保持 1（没被误关）",
                  prefs.get("vp.overlay.camera") == 1 and
                  prefs.get("vp.overlay.navGizmo") == 1,
                  f"camera={prefs.get('vp.overlay.camera')} "
                  f"navGizmo={prefs.get('vp.overlay.navGizmo')}")
            stats_keys = [k for k in prefs if k.startswith("vp.stat.")]
            check("V7f 10 条 vp.stat.* 行开关都在盘里",
                  len(stats_keys) == 10, f"{len(stats_keys)} 条")

            # ---- 第二程：带着这份偏好启动，必须生效 ----
            if os.path.exists(prefs_path):
                s5 = launch(exe, {"MYVK_NO_WINDOW_SAVE": "1",
                                  "MYVK_EDITOR_PREFS_PATH": prefs_path},
                            shot=os.path.join(OUT_DIR, "vp_prefs_reload.png"),
                            ready_needle="VP-RECT cam=")
                errs_all["偏好读回"] = s5.errs
                check("V7g 第二程读回了偏好（日志有 'Editor prefs: loaded'）",
                      s5.tap.has_line("Editor prefs: loaded"))
                st5 = overlay_state(s5.tap)
                check("V7h 第二程 VP-OVERLAY：modeBar/stats 关，camera/navGizmo 开",
                      st5 is not None and st5["master"] == 1 and
                      st5["modeBar"] == 0 and st5["stats"] == 0 and
                      st5["camera"] == 1 and st5["navGizmo"] == 1, f"{st5}")
                check("V7i 第二程：关掉的两块不画、留着的两块照画",
                      not s5.tap.has_line("VP-RECT modebar=") and
                      not s5.tap.has_line("VP-RECT stats=") and
                      s5.tap.has_line("VP-RECT cam=") and
                      s5.tap.last_rect("NAV-BALL+X") is not None)
            else:
                check("V7g/h 第二程读回", False, "偏好文件没写出来")
        finally:
            for p, data in saved_cfg.items():
                with open(p, "wb") as f:
                    f.write(data)

    # ============================================================ V8
    bad = {k: v for k, v in errs_all.items() if v}
    check("V8 全部会话无 Vulkan 验证层报错", not bad,
          "clean" if not bad else "; ".join(
              f"{k}: {v[0][:90]}" for k, v in list(bad.items())[:3]))

    print()
    failed = [r for r in results if not r[1]]
    for name, ok, detail in failed:
        print(f"  - {name}" + (f"  ({detail})" if detail else ""))
    if failed:
        print(f"\nRESULT: FAIL（{len(failed)} 项失败）")
        return 1
    print(f"结果: PASS（{len(results)} 项断言全过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
