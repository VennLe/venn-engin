"""端到端验证：视口选择方式（Tab 径向菜单 / 点选 / 框选 / 套索 / 刷选）。

被验证的用户需求（2026-11-04 原话）：
    "模式类型中的选择这项移除掉，然后当鼠标在视口范围中时，鼠标按下 tab 键
     不动时可以打开一个圆盘选项，分别有普通选择，框选，套索选择，刷选，然后
     鼠标移动到哪个区域对应的选项就直接切换到对应的选择方式。"

分组与分工：

  A 左栏 + 工具条下拉
     A1 只剩 9 行模式（landscape…water）
     A2 **没有** select 行 —— "移除"这件事本身要有反证，否则哪天被加回来
        脚本会静静地继续绿
     A2b 左栏**没有**选择方式列表（2026-11-04 已搬到视口工具条的下拉里）
     A4 点视口左上那颗下拉 → 四种选择方式齐全（SELMODE-ITEM ...）
     A5/A6 点一行真的换方式（VP-SELMODE box / pick），并把起始状态还原

  B Tab 圆盘
     B1 视口里按住 Tab → SEL-RADIAL open
     B2 鼠标甩到上/右/下/左 → SELMODE pick / box / lasso / brush
        （四个方向都验，因为扇区映射写反一个"看起来也能用"）
     B3 松手 → SEL-RADIAL close

  C 三种手势**真选一次**（判据是日志里的 n，不是"菜单打开了"）
     C1 框选：拖着横跨视口 → BOX-SELECT n>0
     C2 套索：画一个大圈 → LASSO-SELECT n>0
     C3 刷选：按住横扫过角色 → BRUSH-SELECT n>0
     C4 点选：点视口空白 → 选中被清空

  D 日志健康：零 Vulkan 校验错误 / 零 ERROR

⚠ 这条用例踩在**真鼠标 + 真键盘**上：`keybd_event(VK_TAB)` 打给前台窗口，
  鼠标事件打给光标底下最上层的窗口。所以每次发事件前都抢一次前台，
  且跑的时候别碰鼠标。圆盘那条尤其怕"鼠标预热"没做 —— 上一动作常把光标
  甩出客户区，第一次 SetCursorPos 之后立刻按 Tab 会被判成"不在视口"。

用法：
    python tools/verify_selection_modes.py            # 全套
    python tools/verify_selection_modes.py B          # 只跑 B 组
"""

import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import user32                                   # noqa: E402
from verify_asset_drag import client_to_screen, move_to             # noqa: E402
from verify_nav_gizmo import (MOUSEEVENTF_LEFTDOWN,                 # noqa: E402
                              MOUSEEVENTF_LEFTUP)
from verify_toolkit_round2 import (EXE, ROOT, center, check, count,  # noqa: E402
                                   launch, rect_of, shutdown)

OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
SCENE = os.path.join(ROOT, "templates", "third_person", "assets", "scenes",
                     "main.json")

VK_TAB = 0x09

fails = []


def check_(ok, msg):
    """`verify_toolkit_round2.check` 已经把失败收集到它自己的列表里，
    这里再记一份用于收尾打印 —— 两边的 `fails` 不是同一个对象。"""
    if not ok:
        fails.append(msg)
    return check(ok, msg)


# ------------------------------------------------------------------ 输入

def ensure_foreground(hwnd):
    """抢前台。`SetForegroundWindow` 会被前台锁拒绝，先用一下 ALT 解锁。"""
    if user32.GetForegroundWindow() == hwnd:
        return True
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.25)
    if user32.GetForegroundWindow() != hwnd:
        user32.SwitchToThisWindow(hwnd, True)
        time.sleep(0.25)
    return user32.GetForegroundWindow() == hwnd


def mouse_to(hwnd, x, y):
    user32.SetCursorPos(*client_to_screen(hwnd, int(x), int(y)))


def click(hwnd, x, y):
    ensure_foreground(hwnd)
    # 预热两次：第一次 MOVE 常被"刚激活窗口"吞掉（MEMORY 里那条）
    mouse_to(hwnd, x, y)
    time.sleep(0.30)
    mouse_to(hwnd, x, y)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.10)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.45)


def drag_path(hwnd, pts, steps_per_seg=10):
    """按住左键依次走过 pts（客户区坐标），最后松开。"""
    ensure_foreground(hwnd)
    x0, y0 = pts[0]
    mouse_to(hwnd, x0, y0)
    time.sleep(0.30)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.18)
    for i in range(1, len(pts)):
        px, py = pts[i - 1]
        cx, cy = pts[i]
        for k in range(1, steps_per_seg + 1):
            mouse_to(hwnd, px + (cx - px) * k / steps_per_seg,
                     py + (cy - py) * k / steps_per_seg)
            time.sleep(0.030)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.7)


def radial(hwnd, tap, cx, cy, dx, dy):
    """在 (cx,cy) 按住 Tab → 移到 (cx+dx, cy+dy) → 松开。

    返回 True = 这次真的打开过圆盘（`SEL-RADIAL open` 涨了一行）。
    """
    ensure_foreground(hwnd)
    mouse_to(hwnd, cx, cy)
    time.sleep(0.32)
    mouse_to(hwnd, cx, cy)
    time.sleep(0.18)
    n0 = count(tap, "SEL-RADIAL open")
    user32.keybd_event(VK_TAB, 0, 0, 0)
    time.sleep(0.28)
    if dx or dy:
        mouse_to(hwnd, cx + dx, cy + dy)
        time.sleep(0.42)
    user32.keybd_event(VK_TAB, 0, 2, 0)
    time.sleep(0.45)
    return count(tap, "SEL-RADIAL open") > n0


def vp(tap):
    r = rect_of(tap, "VP-RECT", 10.0)
    return r


def vp_center(r):
    return center(r)


# ------------------------------------------------------------------ 分组

def part_a(hwnd, tap):
    print("\n== A · 左栏 / 工具条下拉 ==")
    check_(rect_of(tap, "MODE-RAIL-ITEM landscape", 6.0) is not None,
           "A1a 模式列表有 landscape")
    check_(rect_of(tap, "MODE-RAIL-ITEM water", 6.0) is not None,
           "A1b 模式列表有 water（9 行的最后一行）")
    check_(rect_of(tap, "MODE-RAIL-ITEM select", 0.8) is None,
           "A2 模式列表**没有** select 行（已移除）")
    # 2026-11-04 二次调整：选择方式列表从**左栏**搬到了视口工具条的下拉里。
    # 这条是反向证据 —— 左栏如果还留着，用户会看到两套单选、且两边不同步。
    check_(rect_of(tap, "SELMODE-ITEM pick", 0.6) is None,
           "A2b 左栏**不再**有选择方式列表（已搬到工具条下拉）")

    btn = rect_of(tap, "VP-TOOLBTN", 3.0)
    if not check_(btn is not None, "A3 视口工具条左上那颗下拉按钮存在"):
        return
    click(hwnd, *center(btn))
    time.sleep(0.7)
    missing = [m for m in ("pick", "box", "lasso", "brush")
               if rect_of(tap, "SELMODE-ITEM " + m, 3.0) is None]
    check_(not missing, "A4 下拉里有四种选择方式（缺: %s）" % (missing or "无"))

    # 点"框选" → VP-SELMODE box（这条证明列表**能点**，不是只画了个样子）
    row = rect_of(tap, "SELMODE-ITEM box", 2.0)
    if row:
        n0 = count(tap, "VP-SELMODE box")
        click(hwnd, *center(row))
        time.sleep(0.6)
        check_(count(tap, "VP-SELMODE box") > n0,
               "A5 点下拉里的 Box Select → VP-SELMODE box")
    else:
        check_(False, "A5 拿不到 Box Select 那行的矩形")

    # 还原成 pick：B 组的四个方向是**按顺序**验的，起始必须是 pick，
    # 否则第一个 box 扇区会因为"值没变"不打日志（B2 假红）。
    btn = rect_of(tap, "VP-TOOLBTN", 2.0)
    if btn:
        click(hwnd, *center(btn))
        time.sleep(0.6)
        row = rect_of(tap, "SELMODE-ITEM pick", 2.0)
        if row:
            n0 = count(tap, "VP-SELMODE pick")
            click(hwnd, *center(row))
            time.sleep(0.6)
            check_(count(tap, "VP-SELMODE pick") > n0,
                   "A6 点回 Select → VP-SELMODE pick（起始状态已还原）")
        else:
            check_(False, "A6 拿不到 Select 那行的矩形")
    # 保险：弹层还开着的话收掉，别让 B 组的按键被它吃掉
    user32.keybd_event(0x1B, 0, 0, 0)
    user32.keybd_event(0x1B, 0, 2, 0)
    time.sleep(0.3)


def part_b(hwnd, tap):
    print("\n== B · Tab 径向菜单 ==")
    r = vp(tap)
    if not r:
        check_(False, "B0 拿不到 VP-RECT")
        return None
    cx, cy = vp_center(r)
    # 四个方向各来一次；角度半径 70px 稳稳落在扇区中线上（外圈 104 / 内圈 34）。
    # ⚠ 顺序从 box 开始，**不是** pick：`setSelectionMode` 对"相同值"直接
    #   return（不打日志），而启动时本来就是 pick —— 先测 pick 的话
    #   `SELMODE pick` 一行都不会出现，B2 会假红。
    cases = [("box", 70, 0), ("lasso", 0, 70),
             ("brush", -70, 0), ("pick", 0, -70)]
    opened = 0
    for mid, dx, dy in cases:
        n0 = count(tap, "SELMODE " + mid)
        ok = radial(hwnd, tap, cx, cy, dx, dy)
        if ok:
            opened += 1
        got = count(tap, "SELMODE " + mid) > n0
        check_(ok, "B1 按住 Tab 打开圆盘（%s）" % mid)
        check_(got, "B2 甩到 (%+d,%+d) → SELMODE %s" % (dx, dy, mid))
        time.sleep(0.2)
    check_(opened == 4, "B3 四次都打开过圆盘 (opened=%d/4)" % opened)
    check_(count(tap, "SEL-RADIAL close") >= 4,
           "B4 每次松手都关闭 (close=%d)" % count(tap, "SEL-RADIAL close"))
    return (cx, cy)


def part_c(hwnd, tap, c):
    print("\n== C · 三种手势真的选中了东西 ==")
    r = vp(tap)
    if not r:
        check_(False, "C0 拿不到 VP-RECT")
        return
    cx, cy = c
    x0, y0, x1, y1 = r[0], r[1], r[2], r[3]

    # ---- C1 框选：左上 → 右下，横跨整个视口 ----
    radial(hwnd, tap, cx, cy, 70, 0)      # 右 = 框选
    time.sleep(0.2)
    n0 = count(tap, "BOX-SELECT")
    drag_path(hwnd, [(x0 + (x1 - x0) * 0.05, y0 + (y1 - y0) * 0.06),
                     (x1 - (x1 - x0) * 0.05, y1 - (y1 - y0) * 0.06)],
              steps_per_seg=14)
    time.sleep(0.4)
    box_line = ""
    if count(tap, "BOX-SELECT") > n0:
        box_line = last_line(tap, "BOX-SELECT")
    nbox = _n_of(box_line)
    check_(count(tap, "BOX-SELECT") > n0, "C1 框选产生了 BOX-SELECT")
    check_(nbox > 0, "C1b 框选选中了物体 (n=%d)" % nbox)

    # C1c：**再框同一片一次**。这条钉的是一个已经踩过的坑：结算时如果
    # "跳过已选中的"，第二次全员命中却全员被跳过 → found 空 → 清空选中，
    # 用户看到的是"再框一次，选中全没了"。同时也是 C2 的前置条件：
    # 进套索时选择集里正躺着 n=96 个东西。
    n1 = count(tap, "BOX-SELECT")
    drag_path(hwnd, [(x0 + (x1 - x0) * 0.05, y0 + (y1 - y0) * 0.06),
                     (x1 - (x1 - x0) * 0.05, y1 - (y1 - y0) * 0.06)],
              steps_per_seg=14)
    time.sleep(0.4)
    nbox2 = _n_of(last_line(tap, "BOX-SELECT")) if count(tap, "BOX-SELECT") > n1 else 0
    check_(nbox2 > 0, "C1c 连框同一片两次仍选中 (n=%d)" % nbox2)

    # ---- C2 套索：绕视口中心画一个大圈 ----
    radial(hwnd, tap, cx, cy, 0, 70)      # 下 = 套索
    time.sleep(0.2)
    n0 = count(tap, "LASSO-SELECT")
    rad = min(x1 - x0, y1 - y0) * 0.38
    pts = []
    for i in range(19):                   # 18 段 + 回到起点
        a = (i / 18.0) * math.tau - math.pi / 2.0
        pts.append((cx + math.cos(a) * rad, cy + math.sin(a) * rad))
    drag_path(hwnd, pts, steps_per_seg=3)
    time.sleep(0.4)
    lasso_line = last_line(tap, "LASSO-SELECT") if count(tap, "LASSO-SELECT") > n0 else ""
    nlasso = _n_of(lasso_line)
    check_(count(tap, "LASSO-SELECT") > n0, "C2 套索产生了 LASSO-SELECT")
    check_(nlasso > 0, "C2b 套索圈住了物体 (n=%d)" % nlasso)

    # ---- C3 刷选：按住横扫过角色 ----
    radial(hwnd, tap, cx, cy, -70, 0)     # 左 = 刷选
    time.sleep(0.2)
    n0 = count(tap, "BRUSH-SELECT")
    drag_path(hwnd, [(x0 + (x1 - x0) * 0.12, cy),
                     (x1 - (x1 - x0) * 0.12, cy)],
              steps_per_seg=18)
    time.sleep(0.4)
    brush_line = last_line(tap, "BRUSH-SELECT") if count(tap, "BRUSH-SELECT") > n0 else ""
    nbrush = _n_of(brush_line)
    check_(count(tap, "BRUSH-SELECT") > n0, "C3 刷选产生了 BRUSH-SELECT")
    check_(nbrush > 0, "C3b 刷子扫到了物体 (n=%d)" % nbrush)

    # ---- C4 回到点选，点空白 → 清空 ----
    radial(hwnd, tap, cx, cy, 0, -70)     # 上 = 点选
    time.sleep(0.2)
    before = last_line(tap, "SELMODE pick")
    click(hwnd, x0 + (x1 - x0) * 0.04, y0 + (y1 - y0) * 0.06)
    time.sleep(0.4)
    check_(bool(before), "C4 能切回 pick（SELMODE pick 出现过）")
    # 点空白在 pick 模式下会先记一笔"可能要拉框"，松手判定 <4px 才清空
    check_(count(tap, "pick: nothing hit -> selection cleared") > 0,
           "C5 点空白清空了选中")

    # ---- C6 天空区域拖框 = 一个都选不中 ----
    #
    # 用户要求"所有的选择类型只能对天空 / 地面 / 云层 / 星空之外的物体生效"。
    # 天空 / 云层 / 星空是 `SkyParams` 的程序化绘制，**没有实体**，所以正确
    # 行为是 n=0；哪天有人"给天空挂一个实体"，这条会立刻红。
    # 取景：视口最上面 2%~10% 的一条横带（第三人称模板的机位在那儿只有天）。
    radial(hwnd, tap, cx, cy, 70, 0)      # 右 = 框选
    time.sleep(0.2)
    n0 = count(tap, "BOX-SELECT")
    drag_path(hwnd, [(x0 + (x1 - x0) * 0.25, y0 + (y1 - y0) * 0.02),
                     (x1 - (x1 - x0) * 0.25, y0 + (y1 - y0) * 0.10)],
              steps_per_seg=10)
    time.sleep(0.4)
    sky_line = last_line(tap, "BOX-SELECT") if count(tap, "BOX-SELECT") > n0 else ""
    nsky = _n_of(sky_line)
    check_(count(tap, "BOX-SELECT") > n0, "C6 天空区域能正常拉框（BOX-SELECT 出现）")
    check_(nsky == 0, "C6b 天空区域框选 = 0 个（天空 / 云 / 星空不是物体）(n=%d)"
           % nsky)


def last_line(tap, needle):
    with tap.lock:
        for line in reversed(tap.lines):
            if needle in line:
                return line
    return ""


def _n_of(line):
    m = re.search(r"n=(\d+)", line)
    return int(m.group(1)) if m else 0


# ------------------------------------------------------------------ main

def main():
    only = sys.argv[1].upper() if len(sys.argv) > 1 else ""

    def want(tag):
        return not only or only == tag

    print("启动编辑器：%s" % SCENE)
    proc, tap, hwnd = launch(SCENE, "", "select")
    try:
        time.sleep(1.5)
        if want("A"):
            part_a(hwnd, tap)
        c = None
        if want("B"):
            c = part_b(hwnd, tap)
        if want("C"):
            if c is None:
                r = vp(tap)
                c = vp_center(r) if r else (640, 400)
            part_c(hwnd, tap, c)

        if want("D"):
            print("\n== D · 日志健康 ==")
            bad_re = re.compile(r"\[ERROR\]|VUID-|validation layer|SEGV|"
                                r"vkQueueSubmit.*-4")
            with tap.lock:
                bad = [l for l in tap.lines if bad_re.search(l)]
            check_(not bad, "D1 零 Vulkan 校验错误 / 零 ERROR（%d 条）"
                   % len(bad))
            for l in bad[:5]:
                print("    " + l.strip())
    finally:
        shutdown(proc, hwnd)

    print()
    if fails:
        for f in fails:
            print("CHECK FAIL " + f)
        print("结果: FAIL: %d 项不达标" % len(fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
