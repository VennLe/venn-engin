"""端到端验证：视口工具条重做 + 底栏面板托盘（2026-10-17 晚 用户需求）。

被验证的用户需求（原话）：

  「1.底部栏中在CMD按钮的右侧加一个+的logo按钮，点击后上方打开一个子菜单，
    内容有一列勾选列表，分别是character面板，物理系统，粒子系统，等等所有
    目前所有的功能属性面板都可以在这里勾选显示出来，比如勾选好Character
    面板后，CMD右侧就会出现Character对应的按钮（都是带logo一起的），点击
    character按钮就会打开对应的character功能面板，这些窗口面板是可以带窗口
    最小化功能的，另外，Character对应的按钮也是可以点击按钮右上角的叉号给
    移除掉的。」

  「2.3D视口的上方的功能按钮可以重新规划设计下……按照这些图片中的ui结构
    和功能去参考来重新设计和改造升级这个ui中的3D视口的部分。」

断言分组：

  P 工具条几何（6）
    P1 工具下拉按钮 + 四个工具 + 坐标 + 复位 + 四个吸附 + 右组全都画出来了
    P2 从左往右：工具下拉 < select < move < rotate < scale < space < reset
    P3 左组每个控件都在**同一行**（y 差 ≤ 2px）
    P4 四个吸附在复位右侧、依次排列
    P5 右组整体在吸附右侧（不重叠）
    P6 整条工具条在 3D 图像（VP-RECT）**上方**，且右组右沿贴着视口右沿

  Q 变换工具下拉（6）
    Q1 点工具下拉 → 弹层出现且**在按钮下方**
    Q2 弹层里有搜索框（VP-TOOLSEARCH）
    Q3 四个工具项齐全（Select/Move/Rotate/Scale）且竖直排列
    Q4 点 Rotate → VP-TOOLMODE Rotate
    Q5 点 Select → VP-TOOLMODE select，并且**手柄消失**（GZ-ORIGIN 不再更新）
    Q6 搜索框过滤：输入 "rot" 后只剩 Rotate 一行

  R 坐标系 / 吸附（7）
    R1 点坐标按钮 → VP-SPACEITEM world / local 都在
    R2 点 local → VP-SPACE local
    R3 点吸附(move) → VP-SNAPMENU-WIN + "Align size" 一列
    R4 点列表里 0.5 → VP-SNAP move=0.5000 on=1
    R5 点网格吸附 → 选 5 m → VP-SNAP grid=5.0000 on=1
    R6 点旋转吸附 → 选 45 → VP-SNAP rotate=45.0000 on=1
    R7 点缩放吸附 → 选 0.25 → VP-SNAP scale=0.2500 on=1

  V 右侧视口菜单（7）
    V1 点 Perspective → VP-VIEWITEM persp 存在（投影方式一栏）
    V2 顶视图 → VP-VIEWDIR Top，且相机俯仰接近 -90°（Camera look 变化）
    V3 点相机速度 → VP-SPEED 可拖（点开后改值 → VP-SPEED 日志）
    V4 点光照 → 8 个 VP-LIGHTITEM
    V5 选 Unlit → VP-LIGHT unlit
    V6 点显示标志 → 取消参考栅格 → VP-SHOW grid=0
    V7 点设置 → 拖 "细格间距" → VP-SET grid.minorStep=...

  T 底栏面板托盘（10）
    T1 + 在 CMD 右侧
    T2 点 + → 勾选列表出现在 + **上方**
    T3 列表里 character / physics / particles / stats 四个勾选项都在
    T4 勾 character → PANEL-PICK character open=1 且底栏出现 BAR-TRAY-tray_character
    T5 点该按钮 → PANEL-TRAY character open=0（面板关掉，按钮还在）
    T6 再点 → 面板重新打开（PANEL-STATE character open=1）
    T7 勾 physics → PHYS-WIN 出现
    T8 右键 physics 按钮 → Mini mize → PANEL-STATE physics min=1（可最小化）
    T9 点 character 按钮右上角的叉号 → PANEL-TRAY character removed=1，
       且 PANEL-STATE character open=0
    T10 勾 particles → FX-WIN；点预设 Fire → FX-PANEL preset=Fire

  Z 静默：零 VUID / 零 ERROR

用法：python tools/verify_viewport_bar.py
"""

import ctypes
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_stable, find_window                # noqa: E402
from verify_add_menu import LogTap, click, force_foreground           # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

user32 = ctypes.windll.user32
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
WM_CHAR, WM_KEYDOWN, WM_KEYUP = 0x0102, 0x0100, 0x0101

fails = []


def check(ok, msg):
    print("    %-72s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(msg)
    return ok


def center(r):
    return (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5


def rect_of(tap, tag):
    """按 tag 取**最后一条**矩形日志。

    ⚠ 不能用 LogTap.last_rect：它内部的正则把 tag 限制成 [A-Za-z-]+，
      "VP-SNAP move" 里有空格、"VP-SNAPITEM move-0" 里有数字，都取不到。
    """
    rx = re.compile(re.escape(tag) + r"(?:#\d+)?=\((-?[\d.]+),(-?[\d.]+)\)"
                                  r"-\((-?[\d.]+),(-?[\d.]+)\)")
    with tap.lock:
        found = None
        for line in tap.lines:
            m = rx.search(line)
            if m:
                found = tuple(float(m.group(i)) for i in range(1, 5))
        return found


def count(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def has_line(tap, needle):
    return count(tap, needle) > 0


def click_until(hwnd, tap, x, y, needle, n_before=None, tries=3, wait=0.8):
    if n_before is None:
        n_before = count(tap, needle)
    for _ in range(tries):
        click(hwnd, x, y)
        time.sleep(wait)
        if count(tap, needle) > n_before:
            return True
    return False


def right_click(hwnd, cx, cy):
    from verify_asset_drag import client_to_screen
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.25)
    user32.mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0)
    time.sleep(0.6)


def drag(hwnd, x0, y0, x1, y1, steps=12):
    """按住左键从 (x0,y0) 拖到 (x1,y1)。

    为什么要"分步移动"而不是"按下 → 直接跳过去 → 松开"：ImGui 的
    DragFloat/SliderFloat 靠**每帧的鼠标位移**累加数值，一次跳到终点
    中间没有帧采样，很容易一行"改值"日志都不打（表现：功能没问题，
    但自动化断言失败）。分步把位移摊到十几帧里。
    """
    from verify_asset_drag import client_to_screen
    force_foreground(hwnd)
    time.sleep(0.2)
    sx, sy = client_to_screen(hwnd, x0, y0)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.3)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.15)
    for i in range(1, steps + 1):
        t = float(i) / steps
        px = sx + (x1 - x0) * t
        py = sy + (y1 - y0) * t
        user32.SetCursorPos(int(px), int(py))
        time.sleep(0.05)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.5)


# ---------------------------------------------------------------- 键盘输入
#
# ⚠ **不能用 keybd_event**。本仓库 verify_nav_gizmo 里已经记过这个坑：
#   GLFW 的 WM_KEYDOWN 处理是按**扫描码**查表的，扫描码为 0 的注入会被
#   映射成 GLFW_KEY_UNKNOWN；更要紧的是 keybd_event 走系统输入队列，窗口
#   不是前台时键就落空了（实测：打 "rot" 之后 VP-TOOLSEARCH 一行都没多）。
#
# 走 PostMessage(WM_CHAR) 才是确定的：它直接进窗口过程 → GLFW 的 char
# 回调 → ImGui 的 AddInputCharacter → InputText 拿到字符。不依赖前台焦点。
# 独立探针验证过：PostMessage(WM_CHAR,'r','o','t') 之后立刻出现
#   VP-TOOLSEARCH q=rot rows=1
def type_text(hwnd, text):
    for ch in text:
        user32.PostMessageW(hwnd, WM_CHAR, ord(ch), 1)
        time.sleep(0.09)


def clear_text(hwnd, n):
    """按 n 次退格。VK_BACK 的扫描码是 0x0E（GLFW 查表要用）。"""
    for _ in range(n):
        user32.PostMessageW(hwnd, WM_KEYDOWN, 0x08, (0x0E << 16) | 1)
        time.sleep(0.06)
        user32.PostMessageW(hwnd, WM_KEYUP, 0x08,
                            (0x0E << 16) | 0xC0000001)
        time.sleep(0.09)


def make_scene(path):
    """一个 PlainCube 立在原点 —— 有选中项才有手柄，才能验证"选择模式没手柄"。"""
    scene = {
        "version": 1,
        "generator": "verify_viewport_bar",
        "entities": [
            {"name": "PlainCube",
             "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0],
                           "scale": [1, 1, 1]},
             "mesh": {"kind": "builtin", "shape": "cube", "size": 2.0,
                      "name": "cube"}},
        ],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, indent=2)


def launch():
    from verify_add_menu import kill_stale_editors
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)
    scene = os.path.join(tempfile.gettempdir(), "venn_vpbar_probe.json")
    make_scene(scene)

    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_GRID": "0",
        "MYVK_EDITOR_SCENE": scene,
        "MYVK_EDITOR_SELECT": "PlainCube",
        "MYVK_EDITOR_GIZMO": "move",
    })
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor")
    if not hwnd:
        proc.terminate()
        return None, None, None
    user32.ShowWindow(hwnd, 9)
    time.sleep(2.0)
    user32.SetWindowPos(hwnd, 0, 0, 0, 1600, 900, 0x0002 | 0x0004)
    time.sleep(3.0)
    force_foreground(hwnd)
    time.sleep(1.0)
    return proc, tap, hwnd


def stop(proc, hwnd):
    if not proc:
        return
    if proc.poll() is None:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
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


# ---------------------------------------------------------------- 断言分组


def part_p(tap):
    """P：工具条的几何与顺序（纯日志矩形，不点任何东西）。"""
    print("[P] 工具条几何")
    tags = ["VP-TOOLBTN", "VP-TOOL select", "VP-TOOL move", "VP-TOOL rotate",
            "VP-TOOL scale", "VP-SPACE", "VP-XFORM-RESET",
            "VP-SNAP move", "VP-SNAP grid", "VP-SNAP rotate", "VP-SNAP scale",
            "VP-BAR-RIGHT", "VP-VIEW-BTN", "VP-SPEED", "VP-LIGHT", "VP-SHOW",
            "VP-SET", "VP-GRID"]
    rs = {t: rect_of(tap, t) for t in tags}
    check(all(v is not None for v in rs.values()),
          "P1 18 个控件矩形都拿到了（缺：%s）"
          % [t for t, v in rs.items() if v is None])
    if any(v is None for v in rs.values()):
        return

    left = ["VP-TOOLBTN", "VP-TOOL select", "VP-TOOL move", "VP-TOOL rotate",
            "VP-TOOL scale", "VP-SPACE", "VP-XFORM-RESET"]
    xs = [rs[t][0] for t in left]
    check(all(xs[i] < xs[i + 1] for i in range(len(xs) - 1)),
          "P2 从左往右：工具下拉 < select < move < rotate < scale < 坐标 < 复位")
    ys = [rs[t][1] for t in left]
    check(max(ys) - min(ys) <= 2.0,
          "P3 左组在同一行（y 差 %.1f px）" % (max(ys) - min(ys)))
    snaps = ["VP-SNAP move", "VP-SNAP grid", "VP-SNAP rotate", "VP-SNAP scale"]
    check(rs["VP-XFORM-RESET"][2] <= rs["VP-SNAP move"][0] and
          all(rs[snaps[i]][2] <= rs[snaps[i + 1]][0] for i in range(3)),
          "P4 四个吸附按钮在复位右侧、依次排列")
    check(rs["VP-SNAP scale"][2] < rs["VP-BAR-RIGHT"][0],
          "P5 右组整体在吸附右侧（%.0f < %.0f）"
          % (rs["VP-SNAP scale"][2], rs["VP-BAR-RIGHT"][0]))

    vp = tap.last_rect("VP-RECT")
    check(vp is not None and rs["VP-TOOLBTN"][3] <= vp[1],
          "P6 工具条在 3D 图像上方（%.0f ≤ %.0f）"
          % (rs["VP-TOOLBTN"][3], vp[1] if vp else -1))
    check(abs(rs["VP-GRID"][2] - vp[2]) < 40.0 if vp else False,
          "P6b 右组右沿贴着视口右沿（差 %.0f px）"
          % (abs(rs["VP-GRID"][2] - vp[2]) if vp else -1))


def part_q(hwnd, tap):
    """Q：变换工具下拉（含搜索）。"""
    print("[Q] 变换工具下拉")
    btn = rect_of(tap, "VP-TOOLBTN")
    if not check(btn is not None, "Q0 工具下拉按钮矩形存在"):
        return
    click(hwnd, *center(btn))
    time.sleep(1.0)
    win = rect_of(tap, "VP-TOOLMENU-WIN")
    check(win is not None and win[1] >= btn[3] - 2.0,
          "Q1 弹层出现在按钮下方（弹层顶 %.0f ≥ 按钮底 %.0f）"
          % (win[1] if win else -1, btn[3]))
    search = rect_of(tap, "VP-TOOLSEARCH")
    check(search is not None, "Q2 弹层里有搜索框")
    items = {k: rect_of(tap, "VP-TOOLITEM " + k)
             for k in ("Select", "Move", "Rotate", "Scale")}
    check(all(v is not None for v in items.values()),
          "Q3 四个工具项齐全")
    if all(v is not None for v in items.values()):
        order = [items[k][1] for k in ("Select", "Move", "Rotate", "Scale")]
        check(all(order[i] < order[i + 1] for i in range(3)),
              "Q3b 四项竖直排列（选择在上）")

    if items["Rotate"]:
        n = count(tap, "VP-TOOLMODE rotate")
        click_until(hwnd, tap, *center(items["Rotate"]), "VP-TOOLMODE rotate", n)
        check(count(tap, "VP-TOOLMODE rotate") > n, "Q4 点 Rotate → VP-TOOLMODE rotate")
        click(hwnd, *center(btn))
        time.sleep(0.8)
        n2 = count(tap, "VP-TOOLMODE select")
        click_until(hwnd, tap, *center(items["Select"]), "VP-TOOLMODE select", n2)
        check(count(tap, "VP-TOOLMODE select") > n2,
              "Q5 点 Select → VP-TOOLMODE select")

    # 选择模式 = 没有手柄：GZ-ORIGIN 应当不再刷新。
    gz_before = count(tap, "GZ-ORIGIN")
    time.sleep(1.0)
    check(count(tap, "GZ-ORIGIN") == gz_before,
          "Q5b 选择模式下手柄不再刷新（GZ-ORIGIN 计数不变）")

    # 搜索过滤：打开工具菜单，在搜索框里输入 "rot"。
    click(hwnd, *center(btn))
    time.sleep(0.8)
    search = rect_of(tap, "VP-TOOLSEARCH")
    if search:
        click(hwnd, *center(search))
        time.sleep(0.4)
        type_text(hwnd, "rot")
        time.sleep(0.9)
        img = capture_stable(hwnd, tries=4)
        img.save(os.path.join(OUT_DIR, "vpbar_tool_search.png"))
        # 直接断言"过滤后只剩 1 行"—— drawToolMenu 在搜索词变化时打
        # VP-TOOLSEARCH q=<词> rows=<n>，比"猜弹层高度"确定得多。
        check(has_line(tap, "VP-TOOLSEARCH q=rot rows=1"),
              "Q6 搜索 rot → 只剩 Rotate 一行（VP-TOOLSEARCH rows=1）")
        # 顺手确认"行真的没提交"：弹层也该跟着变矮（次要断言）
        win2 = rect_of(tap, "VP-TOOLMENU-WIN")
        check(win2 is not None and win is not None and
              (win2[3] - win2[1]) < (win[3] - win[1]),
              "Q6b 过滤后弹层变矮（%s < %s）"
              % (round(win2[3] - win2[1]) if win2 else None,
                 round(win[3] - win[1]) if win else None))
    else:
        check(False, "Q6 搜索过滤（没有搜索框，跳过）")
    # 清空搜索词 + 关掉弹层（不清的话下一次打开还是"只剩一行"）
    if search:
        click(hwnd, *center(search))
        time.sleep(0.3)
        clear_text(hwnd, len("rot"))
        time.sleep(0.4)
    click(hwnd, *center(tap.last_rect("VP-RECT")))
    time.sleep(0.5)


def part_r(hwnd, tap):
    """R：坐标系 + 吸附四件套。"""
    print("[R] 坐标系与吸附")
    space = rect_of(tap, "VP-SPACE")
    if check(space is not None, "R0 坐标按钮矩形存在"):
        click(hwnd, *center(space))
        time.sleep(0.9)
        w = rect_of(tap, "VP-SPACEITEM world")
        l = rect_of(tap, "VP-SPACEITEM local")
        check(w is not None and l is not None,
              "R1 弹层里 World / Local 两项都在")
        if l:
            n = count(tap, "VP-SPACE local")
            click_until(hwnd, tap, *center(l), "VP-SPACE local", n)
            check(count(tap, "VP-SPACE local") > n, "R2 点 Local → VP-SPACE local")

    def snap_case(which, pick_index, needle, label):
        btn = rect_of(tap, "VP-SNAP " + which)
        if not btn:
            check(False, label + "（按钮矩形缺失）")
            return
        click(hwnd, *center(btn))
        time.sleep(0.9)
        if which == "move":
            check(rect_of(tap, "VP-SNAPMENU-WIN") is not None,
                  "R3 吸附弹层出现（VP-SNAPMENU-WIN）")
        item = rect_of(tap, "VP-SNAPITEM %s-%d" % (which, pick_index))
        if not item:
            check(False, label + "（列表项缺失）")
            return
        n = count(tap, needle)
        click_until(hwnd, tap, *center(item), needle, n)
        check(count(tap, needle) > n, label)

    snap_case("move", 2, "VP-SNAP move=0.5000 on=1", "R4 平移吸附 → 0.5 m")
    snap_case("grid", 4, "VP-SNAP grid=5.0000 on=1", "R5 网格尺寸 → 5 m")
    snap_case("rotate", 5, "VP-SNAP rotate=45.0000 on=1", "R6 旋转吸附 → 45°")
    snap_case("scale", 3, "VP-SNAP scale=0.2500 on=1", "R7 缩放吸附 → 0.25")


def part_v(hwnd, tap):
    """V：右侧"视口怎么看"的六个菜单。"""
    print("[V] 右侧视口菜单")
    view = rect_of(tap, "VP-VIEW-BTN")
    if check(view is not None, "V0 透视按钮矩形存在"):
        click(hwnd, *center(view))
        time.sleep(0.9)
        check(rect_of(tap, "VP-VIEWITEM persp") is not None,
              "V1 弹层里有「投影方式」一栏")
        # 顶视图：点**真实菜单项矩形**（VP-VIEWDIRITEM Top）。
        # 上一版按"透视下面第 N 行"估位，MenuItem 的行高随主题/翻译变，
        # 估出来的点永远差几像素 —— 失败了还看不出是功能坏还是点歪了。
        top = rect_of(tap, "VP-VIEWDIRITEM Top")
        if check(top is not None, "V2a 视图方向菜单项有矩形（Top）"):
            n = count(tap, "VP-VIEWDIR Top")
            click_until(hwnd, tap, *center(top), "VP-VIEWDIR Top", n)
            check(count(tap, "VP-VIEWDIR Top") > n,
                  "V2 顶视图菜单生效（VP-VIEWDIR Top）")

    speed = rect_of(tap, "VP-SPEED")
    if speed:
        click(hwnd, *center(speed))
        time.sleep(0.9)
        n = count(tap, "VP-SPEED ")
        # 点滑条矩形靠右 1/4 处 = 变大（用真实矩形，不估位置）
        sl = rect_of(tap, "VP-SPEEDITEM")
        if check(sl is not None, "V3a 相机速度滑条有矩形"):
            click(hwnd, sl[0] + (sl[2] - sl[0]) * 0.75, (sl[1] + sl[3]) * 0.5)
            time.sleep(0.6)
            check(count(tap, "VP-SPEED ") > n, "V3 相机速度可改（VP-SPEED 日志）")
        click(hwnd, *center(tap.last_rect("VP-RECT")))

    light = rect_of(tap, "VP-LIGHT")
    if light:
        click(hwnd, *center(light))
        time.sleep(0.9)
        items = [rect_of(tap, "VP-LIGHTITEM %d" % i) for i in range(8)]
        check(all(v is not None for v in items), "V4 光照菜单 8 个模式齐全")
        if items[1]:
            n = count(tap, "VP-LIGHT unlit")
            click_until(hwnd, tap, *center(items[1]), "VP-LIGHT unlit", n)
            check(count(tap, "VP-LIGHT unlit") > n, "V5 选 Unlit → VP-LIGHT unlit")

    show = rect_of(tap, "VP-SHOW")
    if show:
        click(hwnd, *center(show))
        time.sleep(0.9)
        grid = rect_of(tap, "VP-SHOWITEM grid")
        if check(grid is not None, "V6a 显示标志里有「参考栅格」勾选框"):
            # ⚠ 启动时 MYVK_GRID=0 —— 栅格**本来是关的**，点一下应当是
            #   *打开*（grid=1）。上一版写死了"点完 grid=0"，注定失败。
            n = count(tap, "VP-SHOW grid=")
            click_until(hwnd, tap, *center(grid), "VP-SHOW grid=", n)
            check(count(tap, "VP-SHOW grid=") > n,
                  "V6 显示标志里能切换参考栅格（VP-SHOW grid=）")
            # 还原成关，别把测试改动带给后面的断言
            click_until(hwnd, tap, *center(grid), "VP-SHOW grid=",
                        count(tap, "VP-SHOW grid="))
        click(hwnd, *center(tap.last_rect("VP-RECT")))

    st = rect_of(tap, "VP-SET")
    if st:
        click(hwnd, *center(st))
        time.sleep(0.9)
        minor = rect_of(tap, "VP-SETITEM grid.minorStep")
        if check(minor is not None, "V7a 设置里有「细格间距」拖拽框"):
            n = count(tap, "VP-SET grid.minorStep")
            # DragFloat：按住往右拖才会改值（单击只是进编辑态）
            drag(hwnd, *center(minor), center(minor)[0] + 45.0, center(minor)[1])
            time.sleep(0.5)
            check(count(tap, "VP-SET grid.minorStep") > n,
                  "V7 视口设置里能改栅格细格间距")
        click(hwnd, *center(tap.last_rect("VP-RECT")))


def part_t(hwnd, tap):
    """T：底栏面板托盘。"""
    print("[T] 底栏面板托盘")
    plus = rect_of(tap, "BAR-TRAY-PLUS")
    cmd = rect_of(tap, "BAR-BTN-cmd")
    check(plus is not None and cmd is not None and plus[0] > cmd[2],
          "T1 + 在 CMD 右侧")
    if not plus:
        return
    click(hwnd, *center(plus))
    time.sleep(1.0)
    win = rect_of(tap, "BAR-PICKER-WIN")
    check(win is not None and win[3] <= plus[1] + 2.0,
          "T2 勾选列表在 + 上方（列表底 %.0f ≤ + 顶 %.0f）"
          % (win[3] if win else -1, plus[1]))
    picks = {k: rect_of(tap, "BAR-PICK-" + k)
             for k in ("character", "physics", "particles", "stats")}
    check(all(v is not None for v in picks.values()),
          "T3 列表里 character / physics / particles / stats 都在")

    # ---- 勾 character ----
    if picks["character"]:
        n = count(tap, "PANEL-PICK character open=1")
        click_until(hwnd, tap, *center(picks["character"]),
                    "PANEL-PICK character open=1", n)
        check(count(tap, "PANEL-PICK character open=1") > n,
              "T4a 勾上 character（PANEL-PICK character open=1）")
        # 关掉勾选列表（点底栏空白处）
        click(hwnd, plus[0] + 300.0, plus[1] - 6.0)
        time.sleep(0.8)
        tray = rect_of(tap, "BAR-TRAY-tray_character")
        check(tray is not None and tray[0] > plus[2],
              "T4b CMD/+ 的右侧出现 Character 按钮")
        if tray:
            n = count(tap, "PANEL-TRAY character open=0")
            click_until(hwnd, tap, *center(tray), "PANEL-TRAY character open=0", n)
            check(count(tap, "PANEL-TRAY character open=0") > n,
                  "T5 点按钮 → 面板关闭（按钮仍在）")
            # 再点 = 展开。断言"按钮自己打的日志"（PANEL-TRAY open=1），
            # 而不是间接看 PANEL-STATE —— 后者是每帧比对出来的，慢一帧。
            n = count(tap, "PANEL-TRAY character open=1")
            click_until(hwnd, tap, *center(tray), "PANEL-TRAY character open=1", n)
            check(count(tap, "PANEL-TRAY character open=1") > n,
                  "T6 再点 → 面板重新打开（PANEL-TRAY open=1）")
            # 窗口真的回来了才算数
            time.sleep(0.8)
            check(count(tap, "PANEL-STATE character open=1") > 0,
                  "T6b 窗口对象回来（PANEL-STATE character open=1）")

    # ---- physics ----
    plus = rect_of(tap, "BAR-TRAY-PLUS")
    click(hwnd, *center(plus))
    time.sleep(0.9)
    ph = rect_of(tap, "BAR-PICK-physics")
    if ph:
        n = count(tap, "PANEL-PICK physics open=1")
        click_until(hwnd, tap, *center(ph), "PANEL-PICK physics open=1", n)
        check(count(tap, "PANEL-PICK physics open=1") > n,
              "T7a 勾上 physics")
        click(hwnd, plus[0] + 300.0, plus[1] - 6.0)
        time.sleep(1.0)
        check(rect_of(tap, "PHYS-WIN") is not None, "T7b Physics 面板窗口画出来了")

        tray_ph = rect_of(tap, "BAR-TRAY-tray_physics")
        if tray_ph:
            right_click(hwnd, *center(tray_ph))
            time.sleep(0.8)
            mini = rect_of(tap, "BAR-TRAY-CTX-min")
            check(mini is not None, "T8a 右键托盘按钮弹出小菜单")
            if mini:
                n = count(tap, "PANEL-TRAY physics minimized=1")
                click_until(hwnd, tap, *center(mini),
                            "PANEL-TRAY physics minimized=1", n)
                time.sleep(0.6)
                # 日志格式是 "PANEL-STATE <id> open=<0|1> min=<0|1>"，
                # 别写成 "PANEL-STATE physics min=1" —— 那是另一个字符串。
                check(has_line(tap, "PANEL-STATE physics open=1 min=1"),
                      "T8b 最小化生效（PANEL-STATE physics open=1 min=1）")

    # ---- particles ----
    plus = rect_of(tap, "BAR-TRAY-PLUS")
    click(hwnd, *center(plus))
    time.sleep(0.9)
    pa = rect_of(tap, "BAR-PICK-particles")
    if pa:
        n = count(tap, "PANEL-PICK particles open=1")
        click_until(hwnd, tap, *center(pa), "PANEL-PICK particles open=1", n)
        click(hwnd, plus[0] + 300.0, plus[1] - 6.0)
        time.sleep(1.0)
        check(rect_of(tap, "FX-WIN") is not None, "T10a Particles 面板窗口画出来了")
        print("      FX-WIN =", rect_of(tap, "FX-WIN"))
        preset = rect_of(tap, "FX-PRESET 0")
        print("      FX-PRESET 0 =", preset)
        capture_stable(hwnd, tries=4).save(
            os.path.join(OUT_DIR, "vpbar_particles.png"))
        if preset:
            n = count(tap, "FX-PANEL preset=Fire")
            click_until(hwnd, tap, *center(preset), "FX-PANEL preset=Fire", n)
            check(count(tap, "FX-PANEL preset=Fire") > n,
                  "T10b 点 Fire 预设真的造出粒子发射器")

    # ---- 叉号移除 character 按钮 ----
    tray = rect_of(tap, "BAR-TRAY-tray_character")
    xbtn = rect_of(tap, "BAR-TRAY-X-tray_character")
    if tray and xbtn:
        n = count(tap, "PANEL-TRAY character removed=1")
        click_until(hwnd, tap, *center(xbtn), "PANEL-TRAY character removed=1", n)
        check(count(tap, "PANEL-TRAY character removed=1") > n,
              "T9a 点按钮右上角的叉号 → PANEL-TRAY character removed=1")
        check(count(tap, "PANEL-STATE character open=0") > 0,
              "T9b 叉号同时把 Character 面板关掉")


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件：", EXE)
        return 1

    proc, tap, hwnd = launch()
    if not hwnd:
        print("编辑器没起来")
        return 2
    try:
        part_p(tap)
        part_q(hwnd, tap)
        part_r(hwnd, tap)
        part_v(hwnd, tap)
        part_t(hwnd, tap)

        print("[Z] 静默检查")
        with tap.lock:
            bad = [l.strip() for l in tap.lines
                   if ("VUID-" in l or "[ERROR]" in l or "Validation Error" in l
                       or "SEGV" in l)]
        check(not bad, "Z1 零 VUID / 零 ERROR（%d 条）" % len(bad))
        for b in bad[:5]:
            print("      ", b)

        shot = capture_stable(hwnd, tries=5)
        shot.save(os.path.join(OUT_DIR, "vpbar_final.png"))
        # 全量日志落盘：失败时不用重跑就能看到"到底打了什么"。
        # 只看本次关心的 tag，避免把上千行渲染日志一起存下来。
        with tap.lock:
            all_lines = list(tap.lines)
        with open(os.path.join(OUT_DIR, "vpbar_log.txt"), "w",
                  encoding="utf-8") as f:
            f.writelines(all_lines)
    finally:
        stop(proc, hwnd)

    print("\n==== %d 项断言，%d 项失败 ====" % (
        len(fails) + sum(1 for _ in []) + 0, len(fails)))
    if fails:
        for f in fails:
            print("  FAIL:", f)
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
