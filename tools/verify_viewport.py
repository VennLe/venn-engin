"""端到端验证：E14 视口浮层 + 相机书签 + 浮层显隐（docs/editor_roadmap.md）。

交付物（见 editor/ViewportPanel.cpp 的 "E14 浮层" 一段）：

  · 浮层分工：左上 = 视图模式条 / 右上 = 统计 HUD（**默认折叠隐藏**，
              可点开）/ 右下 = 导航 gizmo。
              左下角**曾经**是相机浮层（pos·look·FOV·Sens·Grid + 9 个书签）；
              2026-10-16 按用户要求**并入 Inspector → World → Camera**
              （两处编辑同一个相机对象本来就是重复），所以视口里不再有它。
  · 两层显隐：`m_showOverlay`（工具栏 HUD 勾选框 / MYVK_EDITOR_NO_HUD）是总闸，
    总闸之上三块各自一个开关（`EditorContext::ViewportOverlay`），
    经右键空白处的 "Viewport Display" 菜单切换并随偏好落盘；
  · 统计 HUD 逐行开关（`StatsHudItems` 10 项）+ **会话内**折叠态；
  · Shift 层快捷键：Shift+1..8 切视图模式 / Ctrl+Shift+1..9 存书签 /
    Alt+Shift+1..9 跳书签（数字键本体已被 Blender 视图方向与 Alt+面板显隐占满）。
    ⚠ 书签的**快捷键**留在视口（ViewportPanel 的按键处理），只有 Inspector
      里那排书签按钮是新增的入口。

断言分组：

  V0 UI 存在性    VP-MODEBAR modes=8 current=lit；modebar 矩形日志在；
                  NAV-BALL+X 出现（导航 gizmo 在画）；
                  **默认状态** VP-OVERLAY master=1 modeBar=1 stats=0 navGizmo=1
                  （stats 默认关是 2026-10-16 的既定行为，不是回归）。
  V1 NO_HUD 确定性 同参数两次抓图，**整窗**（15 个面板 + 视口）逐像素一致。
  V2 全关 == NO_HUD 三个浮层开关全关（走偏好文件）→ 视口内部与 NO_HUD
                  逐像素一致。**这条是 E14 的核心不变量**：总闸之外的三块
                  开关必须把各自的绘制完全关掉，一块都不许漏。
                  （只比视口内部：总闸关 vs 开会让工具栏的 HUD 勾选框
                    换个高亮色，那是 1560 个像素的差异，与浮层无关。）
  ⚠ V1/V2/V3 这三条都**逐像素**，因此都用 `launch_converged`：布局（分栏根节点
    ratio=0 = 按窗口自适应）有首帧竞态，实测 6 次启动里 1 次左栏差 4px —— 不等
    收敛就会拿两块不同几何的图去比。详见 `CONVERGED_VP` 处的注释。
  ⚠ 逐像素会话还要求**没有控件被 hover**：`launch` 会把窗口摆到 (0,0) 并把光标
    停到屏幕右下角（`park_cursor`）。实测视口工具栏那对 `World / Local` 单选钮
    一旦被 hover，外圈会亮一档 → 凭空多出 348px、最大差 17 的差异。
  V3 布局骨架    与 tools/vp_nohud_baseline.png（E14 时期抓的参考图）比**结构**
                  而不是逐像素：提取"面板垂直分隔线"的 x 位置，要求条数相同、
                  逐条对齐、每对差 ≤ SKEL_TOL(8) px。
                  ⚠ **为什么从"整窗逐像素"降级成结构断言（2026-10-16）**：
                  参考图是 E14 时期抓的，此后 E15~E21、E11.1（Inspector 图标
                  导航条）、以及 Render/Physics 两个页签的内容都是**有意的 UI
                  变更** —— 整窗像素当然对不上（实测右面板 76% 像素差、视口 52%）。
                  实测归因（**不是**渲染回归）：
                    · 骨架只差 3~5px —— 左面板宽了 ~5px、右面板窄 ~3px →
                      视口宽 945→938（宽高比变 0.7%）→ 栅格这类**高频内容**
                      整片错位，于是"差 52%"看着吓人；
                    · **天空/色调逐位不变**（三通道均值差 < 0.2）→ 曝光与
                      tonemap 根本没动。这正是 V3b 要守的东西。
                  **不重采基线**（纪律）：重采等于把当前二进制的像素无条件盖成
                  "参考"，从此只能验"它还是它自己"。结构断言不需要重采 ——
                  "面板边界在哪"是人能一眼判断对错的东西，±8px 也足够严。
  V3b 天空/色调   参考图与本轮在**天空区域**（视口上方一块固定矩形，
                  `SKY_BOX`）的三通道均值差 < 1.0 —— 曝光 / tonemap / IBL
                  没有被误改。（实测 0.12~0.19，阈值留了约 5 倍余量。）
  V4 模式条快捷键 Shift+2 → unlit、Shift+5 → wireframe、Shift+1 → 回 lit。
  V5 书签往返     Ctrl+Shift+1 存槽 1 → 按 3（右视图，改朝角）→ Ctrl+Shift+2
                  存槽 2 → 断言两槽不同（证明按下 3 真的改了相机）→
                  Alt+Shift+1 跳槽 1 → 断言 yaw/pitch 与槽 1 存时一致。
  V6 Display 菜单 右键视口空白处 → VP-DISPLAY-WIN（菜单开了）→ 点
                  "View mode bar" → VP-OVERLAY modeBar=0 且 navGizmo 仍是 1、
                  stats 仍是它本来的默认 0 → 点 "Show all overlays" → 三块全 1。
  V7 偏好落盘往返 正常窗口里把 modeBar 关掉、stats 打开，优雅关窗（onShutdown
                  落盘）→ 偏好 JSON 里 vp.overlay.modeBar==0 / stats==1 且
                  vp.stat.* 齐全 → 第二程带着这份偏好启动 → VP-OVERLAY 生效、
                  两块各自按存下来的值画或不画。
                  （特意做成一关一开：落盘的是**值**而不是"关过"这件事，
                    a 一个方向往返证明不了这一点。）
  V8 零验证层错误 全部会话无 VUID / VALIDATION / ERROR。

⚠ **E21 起的一条隐性耦合**：这些会话里只要出现 `VK_LOG_WARN/ERROR`，右上角就会
  多一条 toast（NotifyCenter 按设计就是要弹的），于是 **V1b / V3 的逐像素比对会
  多出一块差异**。真出现这种情况，先看是不是"渲染/脚本真的报了警告" ——
  那是被测对象的问题，让它先红，别去动阈值。

用法：
    python tools/verify_viewport.py
"""

import argparse
import atexit
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


# ------------------------------------------------------------------ 环境钉死
#
# ⚠ **为什么 NO_HUD / 全关这两组会话必须钉死布局与偏好**（2026-10-16 补）：
#
#   编辑器在没有 MYVK_EDITOR_LAYOUT / MYVK_EDITOR_PREFS_PATH 时，会读写
#   `build/bin/config/editor_layout.json` —— 一个**没有被版本控制**的运行时
#   文件（build/ 在 .gitignore 里）。用户拖一下分栏、切一下 Inspector 页签、
#   跑一次别的验证脚本，它就会变；而它一变，**每一块面板的边界都会跟着动**，
#   于是"整窗逐像素"的比对结果完全取决于"上一个人把分栏拖到哪"。
#
#   实测证据：把这一条钉死之前，同一份二进制在不同分栏比例下与旧基线的差异
#   在 526741 / 639043 两个值之间跳（差 11 万个像素），而两次连续抓图之间
#   永远是 0 差（V1b 一直 PASS）—— 说明抖动全部来自那个外部文件。
#
#   所以：这一条把这些会话的布局/偏好指向一个**不存在的临时路径**（= 出厂
#   默认，且 MYVK_NO_WINDOW_SAVE=1 保证它永远不被写出来）。参考图也因此
#   变成了"从仓库本身就能复现"的东西，而不是"复现某台机器上的鼠标位置"。
def pinned_env(td, extra=None):
    """把布局与偏好都钉到**不存在的临时文件**上（开局即出厂默认）。

    `td` 是一个临时目录。传进来的 `extra` 可以覆盖（比如 NO_HUD 或某个
    自定义偏好文件 —— 后者会替换掉这里钉的偏好路径，由调用方自己保证
    那份偏好是完整的）。
    """
    env = {
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_LAYOUT": os.path.join(td, "pinned_layout.json"),
        "MYVK_EDITOR_PREFS_PATH": os.path.join(td, "pinned_prefs.json"),
    }
    if extra:
        env.update(extra)
    return env

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

# V3：面板垂直分隔线的位置容差。实测同一份二进制在不同分栏比例下会差 3~5px
# （面板最小宽度随内容变），8px 既容得下这个，又能抓住"真的动过布局"。
SKEL_TOL = 8
# V3b：取视口**上方**一块固定矩形（一定落在天空里，见文件头）；坐标系同样是
# 窗口客户区。
SKY_BOX = (195, 240, 600, 900)
SKY_TOL = 1.0

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
    r"VP-OVERLAY master=(\d) modeBar=(\d) stats=(\d) navGizmo=(\d)")

# 日志里 VP-OVERLAY 的键顺序（与 ViewportPanel::draw 第 6 步的格式串一致）。
# ⚠ 2026-10-16 起**没有 camera** 这一项了：相机浮层已并入 Inspector。
OVERLAY_KEYS = ["master", "modeBar", "stats", "navGizmo"]


def overlay_state(tap):
    """最近一条 VP-OVERLAY 的状态（总闸 + 三个浮层开关）。

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
    return {k: int(m.group(i + 1)) for i, k in enumerate(OVERLAY_KEYS)}


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


def park_cursor():
    """把物理光标停到屏幕右下角（= 一定在 1600×900 的窗口之外）。

    ⚠ **为什么必须停**：ImGui 的控件有 hover 高亮。窗口一开，光标"正好落在
    哪个控件上"完全取决于**用户上一秒把鼠标推到哪** —— 那不是被测代码的状态，
    却会进截图。实测：视口工具栏那对 `World / Local` 单选钮，被 hover 时**外圈
    会亮一档**（内圈圆点一模一样），于是 V1b"两次抓图逐像素一致"会凭空多出
    **348 个像素、最大差 17** 的差异（差异形状是一个标准圆环 ≈ 控件边框）。

    与 `pinned_env`（钉布局/偏好）、`MYVK_EDITOR_NO_HUD`、`MYVK_GRID=0` 是同一类措施：
    把"环境"钉死，让像素只反映被测代码。调用点：窗口摆好之后、以及**抓图之前**。
    """
    user32.SetCursorPos(user32.GetSystemMetrics(0) - 4,
                        user32.GetSystemMetrics(1) - 4)


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
        # 窗口摆到 (0,0)（原来的 SWP_NOMOVE 会让它留在 GLFW 的默认位置，那样
        # "屏幕右下角在不在窗口外"就不确定了）→ 再把光标停到右下角，保证开局
        # 没有任何控件被 hover。
        user32.SetWindowPos(hwnd, 0, 0, 0, window[0], window[1], 0x0004)
        park_cursor()
        time.sleep(settle)
        if ready_needle:
            wait_count(tap, ready_needle, ready_n, timeout=25.0)
        time.sleep(0.8)

        if actions:
            actions(tap, hwnd)
            time.sleep(0.6)

        # ⚠ 抓图前**再停一次**：`actions` 里的点击会把光标留在刚点过的控件上。
        park_cursor()
        time.sleep(0.2)
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


# ------------------------------------------------------------------ 布局收敛
#
# ⚠ **首帧布局竞态（2026-10-16 实测）**：同一份二进制连起 6 次，其中 **1 次**
#   左栏宽度差 4px —— `VP-RECT=(310,184,1238,776)` / 骨架 `[304,1246,1572]`
#   变成了 `VP-RECT=(305,184,1241,776)` / 骨架 `[300,1249,1568,1575]`。
#   根因：分栏的根节点 ratio = 0.0 = **按窗口自适应**（见 `SplitLayout.h` 的
#   "工作区预设里 ratio 写 0 的那些节点"），而本 harness 是"先起窗、再
#   SetWindowPos"—— 首帧布局用的是**启动那一刻**的尺寸，偶尔就停在没收敛的
#   那一支上。
#
#   危害：凡是"逐像素比对"的会话，会变成拿**两块不同几何**的图去比 —— 差异
#   全部来自那 4px 位移（视口宽 928→936，宽高比变 → 栅格整片错位），真实回归
#   反而被淹掉。V2c 就是因此从 PASS 翻成 130564px 差异的（**不是浮层的问题**）。
#
#   对策：要逐像素比的会话一律用 `launch_converged`，等到几何收敛再抓图。
CONVERGED_VP = (310, 184, 1238, 776)
LAYOUT_TRIES = 4


def launch_converged(exe, env_extra, **kw):
    """启动到"布局已收敛"（`VP-RECT == CONVERGED_VP`）为止，最多重试 LAYOUT_TRIES 次。

    最后一次即使没收敛也**照常返回** —— 让上层断言如实报红，不在这里吞掉。
    """
    s = None
    for i in range(LAYOUT_TRIES):
        s = launch(exe, env_extra, **kw)
        if s.vp_rect == CONVERGED_VP:
            if i:
                print(f"  （第 {i + 1} 次启动才收敛到 {CONVERGED_VP}）")
            return s
    print(f"  WARN: {LAYOUT_TRIES} 次启动都没收敛到 {CONVERGED_VP}，"
          f"最后一次 VP-RECT={s.vp_rect if s else None}")
    return s


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


def panel_skeleton(img, y0=100, y1=780):
    """返回"面板垂直分隔线"的 x 位置（升序）。

    判据：整列在 [y0,y1) 上**几乎同色**（std < 2）且与左右邻域的均值有明显跳变。
    这比逐像素比对稳得多 —— 面板里的**内容**怎么改都不影响"边界画在哪"，
    而它恰好是布局回归真正会让变的东西。相邻候选合并到 6px 以内（分隔线
    本身可能有 1~2px 的双线）。
    """
    import numpy as np
    a = np.asarray(img.convert("RGB"), dtype=np.int32)[y0:y1]
    lum = a.mean(axis=2)
    colstd = lum.std(axis=0)
    colmean = lum.mean(axis=0)
    cand = []
    for x in range(2, len(colstd) - 2):
        if colstd[x] >= 2.0:
            continue
        left = colmean[max(0, x - 6):x - 1].mean()
        right = colmean[x + 2:x + 7].mean()
        if abs(colmean[x] - left) > 6 or abs(colmean[x] - right) > 6:
            cand.append(x)
    groups = []
    for x in cand:
        if groups and x - groups[-1][-1] <= 6:
            groups[-1].append(x)
        else:
            groups.append([x])
    return [int(round(float(np.mean(g)))) for g in groups]


def region_mean(img, box):
    """矩形区域的三通道均值（box = (y0, y1, x0, x1)）。"""
    import numpy as np
    y0, y1, x0, x1 = box
    a = np.asarray(img.convert("RGB"), dtype=np.float64)[y0:y1, x0:x1]
    return a.reshape(-1, 3).mean(axis=0)


# ---------------------------------------------------------------- 偏好文件

# ⚠ 版本号必须与 `editor/EditorLayoutIO.cpp` 的 `kPrefsVersion` 一致（现在是 2）。
#   写成 1 的后果不是"字段没生效"而是**整份存档被丢弃**：版本门
#   （EditorLayoutIO.cpp 里"老 schema → 整份作废"那一段）会让引擎直接回到
#   出厂默认，于是 V2 那一程的浮层全开着，V2a/V2b/V2c 一起红，而且看起来
#   像是"偏好不生效"的诡异 bug。
#   2026-10-17 全量回归就是踩在这里：`write_prefs` 还写 1，而 kPrefsVersion
#   早就升到 2 了。这条断言（V2a 要求三个开关都是 0）恰好是唯一能抓到它的
#   地方 —— 所以别为了让 V2 变绿去改断言，要改的是这里的版本号。
PREFS_VERSION = 2


def write_prefs(path, overrides):
    """写一份最小偏好：只给要覆盖的 key，其余靠出厂值。"""
    doc = {"version": PREFS_VERSION,
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

    # 全流程共用的"钉死用"临时目录：所有 GUI 会话都把布局/偏好指到这里面
    # **不存在**的文件上（= 出厂默认），见上面 pinned_env 的说明。
    pin_dir = tempfile.mkdtemp(prefix="vp_verify_")
    atexit.register(shutil.rmtree, pin_dir, True)

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

    # ⚠ 这一程也要钉布局/偏好：`VP-OVERLAY` 的**默认值**断言（V0d/V4e）读的是
    # 开局那一条日志，而开局状态来自落盘的偏好文件 —— 不钉死就会读到上一次
    # 真实运行（或别的脚本）留下的 `build/bin/config/editor_prefs.json`。
    s = launch(exe, pinned_env(pin_dir), actions=act_shortcuts,
               shot=os.path.join(OUT_DIR, "vp_overlays.png"))
    errs_all["快捷键/书签"] = s.errs
    lines = s.lines

    check("V0a 模式条 8 格（VP-MODEBAR modes=8 current=lit）",
          s.tap.has_line("VP-MODEBAR modes=8 current=lit"),
          "缺 VP-MODEBAR modes=8 current=lit")
    check("V0b VP-RECT modebar 矩形日志在（浮层在画）",
          s.tap.last_rect("VP-RECT modebar") is not None)
    check("V0c 导航 gizmo 在画（NAV-BALL+X）",
          s.tap.last_rect("NAV-BALL+X") is not None)
    # 默认状态：stats **默认关**（2026-10-16 用户要求"右上角数据面板默认可折叠
    # 隐藏"，折叠态默认就是看不到）。所以这里断言的是"默认长什么样"，而不是
    # "四块都在" —— 后者现在是**错的期望**。
    ov_default = overlay_state(s.tap)
    check("V0d 默认 VP-OVERLAY：master/modeBar/navGizmo 开、stats 关",
          ov_default is not None and ov_default["master"] == 1 and
          ov_default["modeBar"] == 1 and ov_default["stats"] == 0 and
          ov_default["navGizmo"] == 1, f"{ov_default}")
    check("V0e stats 默认关 → VP-RECT stats 不该出现",
          not s.tap.has_line("VP-RECT stats="))

    check("V4a Shift+2 → unlit",
          s.tap.has_line("VP-MODEBAR key=2 mode=unlit"))
    check("V4b Shift+5 → metallic（= 模式索引 4）",
          s.tap.has_line("VP-MODEBAR key=5 mode=metallic"))
    check("V4c Shift+6 → wireframe（= 模式索引 5）",
          s.tap.has_line("VP-MODEBAR key=6 mode=wireframe"))
    check("V4d Shift+1 → 回 lit",
          s.tap.has_line("VP-MODEBAR key=1 mode=lit"))
    ov0 = overlay_state(s.tap)
    check("V4e 启动时浮层状态 = 默认（modeBar/navGizmo 开、stats 关）",
          ov0 is not None and ov0["master"] == 1 and ov0["modeBar"] == 1 and
          ov0["navGizmo"] == 1 and ov0["stats"] == 0,
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
    # ⚠ 这两程就是"整窗逐像素"的基准，**必须**钉死布局/偏好（见 pinned_env）。
    nohud_env = pinned_env(pin_dir, {"MYVK_EDITOR_NO_HUD": "1"})
    s1 = launch_converged(exe, nohud_env,
                          shot=os.path.join(OUT_DIR, "vp_nohud_a.png"),
                          ready_needle=None, settle=4.0)
    s2 = launch_converged(exe, nohud_env,
                          shot=os.path.join(OUT_DIR, "vp_nohud_b.png"),
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
        # ---- V3：布局骨架（**不是**逐像素）。见文件头"为什么降级"。----
        sk_ref = panel_skeleton(base)
        sk_cur = panel_skeleton(s1.img)
        ok_len = len(sk_ref) == len(sk_cur) and len(sk_ref) > 0
        worst = 0
        if ok_len:
            worst = max(abs(a - b) for a, b in zip(sk_ref, sk_cur))
        check("V3 面板骨架与参考图一致（分隔线条数相同、每对差 ≤ %dpx）"
              % SKEL_TOL,
              ok_len and worst <= SKEL_TOL,
              f"参考 {sk_ref} / 本轮 {sk_cur}"
              + (f" / 最大差 {worst}px" if ok_len else " / 条数不同"))
        # ---- V3b：天空 / 色调没被误改（曝光与 tonemap 的守卫）----
        if (base.size == s1.img.size and SKY_BOX[1] <= H
                and SKY_BOX[3] <= W):
            m_ref = region_mean(base, SKY_BOX)
            m_cur = region_mean(s1.img, SKY_BOX)
            dmax = float(abs(m_ref - m_cur).max())
            check("V3b 天空/色调逐位不变（三通道均值差 < %.1f）" % SKY_TOL,
                  dmax < SKY_TOL,
                  f"参考 {m_ref.round(2)} / 本轮 {m_cur.round(2)}"
                  f" / 最大通道差 {dmax:.3f}")
        else:
            check("V3b 天空/色调逐位不变", False,
                  f"尺寸不同 {base.size} vs {s1.img.size}")
    else:
        check("V3 面板骨架与参考图一致", False,
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
    # 三个浮层开关全关（走偏好文件）→ 视口内部必须与 NO_HUD 逐像素一致。
    with tempfile.TemporaryDirectory() as td:
        p_all_off = os.path.join(td, "all_off.json")
        write_prefs(p_all_off, {
            "vp.overlay.modeBar": 0, "vp.overlay.stats": 0,
            "vp.overlay.navGizmo": 0})
        # 只覆盖偏好（那份"三开关全关"），布局仍钉在不存在处（= 出厂默认），
        # 否则这一程的面板边界会和 s1（NO_HUD）不一样 → 视口内部裁的就不是
        # 同一块像素了。
        s3 = launch_converged(exe, pinned_env(td, {
                                  "MYVK_EDITOR_PREFS_PATH": p_all_off}),
                              shot=os.path.join(OUT_DIR, "vp_alloff.png"),
                              ready_needle=None, settle=4.0)
        errs_all["全浮层关"] = s3.errs
        st3 = overlay_state(s3.tap)
        check("V2a 三个开关都是 0（VP-OVERLAY），总闸仍开",
              st3 is not None and st3["master"] == 1 and
              all(st3[k] == 0 for k in ("modeBar", "stats", "navGizmo")),
              f"{st3}")
        for tag in ("VP-RECT modebar", "VP-RECT stats"):
            check(f"V2b 关掉后 {tag} 不再出现",
                  not s3.tap.has_line(tag + "="))
        check("V2b 关掉后 NAV-BALL 不再出现",
              not s3.tap.has_line("NAV-BALL+X"))
        if s3.img is not None and s1.img is not None:
            # 只比视口内部：整窗比对会带上工具栏那个 HUD 勾选框的高亮差
            # （总闸关 vs 总闸开，1560 像素，与浮层无关）。
            d = diff_stats(crop(s3.img, VP_INNER), crop(s1.img, VP_INNER))
            check("V2c 三浮层全关 == NO_HUD（视口内部逐像素）",
                  d is not None and d["pixels"] == 0, f"{d}")
        else:
            check("V2c 三浮层全关 == NO_HUD（视口内部逐像素）", False,
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
        r_gz = tap.last_rect("VP-DISPLAY-ITEM navGizmo")
        r_all = tap.last_rect("VP-DISPLAY-ITEM all")
        # ⚠ 相机浮层的复选项已随浮层一起删掉（见文件头），菜单里没有
        #   "VP-DISPLAY-ITEM camera" 了 —— 不要再断言它。
        v6["items"] = all(r is not None for r in (r_mb, r_st, r_gz, r_all))
        if not (v6["menu"] and v6["items"]):
            return

        click_rect(hwnd, r_mb)               # 关掉 View mode bar
        v6["after_off"] = wait_overlay(
            tap, lambda st: st["modeBar"] == 0, timeout=4.0)
        click_rect(hwnd, r_all)              # Show all overlays
        v6["after_all"] = wait_overlay(
            tap, lambda st: all(st[k] == 1
                                for k in ("modeBar", "stats", "navGizmo")),
            timeout=4.0)

    s4 = launch(exe, pinned_env(pin_dir), actions=act_display_menu,
                shot=os.path.join(OUT_DIR, "vp_display_menu.png"),
                ready_needle="VP-RECT modebar=")
    errs_all["Display 菜单"] = s4.errs

    check("V6a 右键视口空白处弹出 Viewport Display 菜单", v6.get("menu"))
    check("V6b 菜单里三块浮层开关 + Show all 的矩形都打出来了",
          v6.get("items"))
    off_st = v6.get("after_off")
    check("V6c 点掉 View mode bar → VP-OVERLAY modeBar=0",
          off_st is not None and off_st["modeBar"] == 0, f"{off_st}")
    check("V6d 只关了这一块：navGizmo 仍是 1、stats 保持它的默认 0",
          off_st is not None and off_st["navGizmo"] == 1 and
          off_st["stats"] == 0, f"{off_st}")
    all_st = v6.get("after_all")
    check("V6e Show all overlays → 三块全回来（stats 也被打开）",
          all_st is not None and
          all(all_st[k] == 1 for k in ("modeBar", "stats", "navGizmo")),
          f"{all_st}")

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
            # 一关一开：modeBar 1→0、stats 0→1（stats 的默认值就是 0，
            # 见 V0d）。这样落盘的两个值都**不等于默认**，第二程才能证明
            # "存下来的是值本身"，而不是"瞎猫碰上默认值"。
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
            check("V7d vp.overlay.modeBar 落盘为 0（点了一下关掉）",
                  prefs.get("vp.overlay.modeBar") == 0,
                  f"modeBar={prefs.get('vp.overlay.modeBar')}")
            check("V7e vp.overlay.stats 落盘为 1（点了一下打开）",
                  prefs.get("vp.overlay.stats") == 1,
                  f"stats={prefs.get('vp.overlay.stats')}")
            check("V7f vp.overlay.navGizmo 保持 1（没被误关）",
                  prefs.get("vp.overlay.navGizmo") == 1,
                  f"navGizmo={prefs.get('vp.overlay.navGizmo')}")
            stats_keys = [k for k in prefs if k.startswith("vp.stat.")]
            check("V7g 10 条 vp.stat.* 行开关都在盘里",
                  len(stats_keys) == 10, f"{len(stats_keys)} 条")

            # ---- 第二程：带着这份偏好启动，必须生效 ----
            if os.path.exists(prefs_path):
                # 带着刚写出来的偏好跑 —— 布局仍钉在不存在处（出厂默认）。
                s5 = launch(exe, pinned_env(td, {
                                  "MYVK_EDITOR_PREFS_PATH": prefs_path}),
                            shot=os.path.join(OUT_DIR, "vp_prefs_reload.png"),
                            ready_needle="VP-RECT modebar=")
                errs_all["偏好读回"] = s5.errs
                check("V7h 第二程读回了偏好（日志有 'Editor prefs: loaded'）",
                      s5.tap.has_line("Editor prefs: loaded"))
                st5 = overlay_state(s5.tap)
                check("V7i 第二程 VP-OVERLAY：modeBar 关、stats/navGizmo 开",
                      st5 is not None and st5["master"] == 1 and
                      st5["modeBar"] == 0 and st5["stats"] == 1 and
                      st5["navGizmo"] == 1, f"{st5}")
                check("V7j 第二程：关掉的不画、打开的画",
                      not s5.tap.has_line("VP-RECT modebar=") and
                      s5.tap.has_line("VP-RECT stats=") and
                      s5.tap.last_rect("NAV-BALL+X") is not None)
            else:
                check("V7h/i 第二程读回", False, "偏好文件没写出来")
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
