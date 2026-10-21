"""端到端验证：水体编辑模式（2026-10-21 第五轮）。

被验证的用户需求（原话）：
  "游戏中的对于水体的部分还没有相关的处理，你可以一起把它放到编辑器左侧的
   模式列表中去，然后和地形植被编辑一样，水这部分的参数面板你可以放到下方的
   属性参数面板中。
   a.水体水质的呈现效果，还有水面涟漪和波浪细纹，这方面的参数你可以搜集一下
     相关的算法，尽可能保证真实细腻感；
   b.水这部分的参数可以这样，可以通过圆形或矩形去控制水体的范围大小，然后类似
     用鼠标在指定区域去拖拽可生出对应的水体，参数面板可以实时控制水体的范围
     大小，水体的颜色，水的类型。可以控制水体的颜色，水体颜色可以通过自己设置
     也可以默认给定几中水体类型，比如山泉水，湖水，池塘水，海水，河水，等等去
     控制，不同类型的水，其透明度，折射度，流速，水面波纹，涟漪都不一样。"

--------------------------------------------------------------------------
断言分组
--------------------------------------------------------------------------
  A. 模式与面板
     A1 左栏出现 `MODE-RAIL-ITEM water`，且它的中心点落在模式栏窗口内
        （10 行的列表如果被裁掉最后一行，用户根本点不到 —— 这是"加了
         模式但用不了"的典型形态）
     A2 切过去之后 `EDITMODE-PANELS mode=water panel=water open=1`
     A3 面板锚点 WATER-PANEL / WATER-ADD 都在
  B. 渲染通道
     B1 `WATER-INIT` —— 通道建好（管线 + 描述符集）
     B2 `WATER-TARGETS` —— 帧缓冲/颜色拷贝图绑好了
  C. 建一块水体（点面板按钮）
     C1 `WATER-CREATE ... shape=circle src=button`
     C2 `WATER-SYNC bodies=1`
     C3 `WATER-MESH shape=circle verts>0`（网格真的生成了）
     C4 视口像素变化 > 3%（水面真画进画面了，不是只写了个组件）
  D. 水质预设真的有差异
     D1 新建下拉切到 "Sea water" → `WATER-NEW-KIND kind=sea`
     D2 新建下拉切到 "Rectangle" → `WATER-NEW-SHAPE shape=rect`
     D3 再建一块 → `WATER-CREATE ... shape=rect kind=sea`
        （同一个预设走通了"下拉 → 组件 → 渲染"整条链）
  E. 参数实时生效
     E1 拖半径滑块 → `WATER-SIZE ... radius=` 且值变了
     E2 水位滑块 → `WATER-LEVEL ... level=`
  F. 删除
     F1 `WATER-DELETE`，随后 `WATER-SYNC bodies=` 归零
  G. 日志健康：零 VUID / 零 ERROR

--------------------------------------------------------------------------
为什么"建水体"走面板按钮而不是视口拖拽
--------------------------------------------------------------------------
两条路径都实现了（拖拽是主交互，按钮是它的可脚本化等价入口），但拖拽
在自动化里要精确控制按下/移动/松开三段的时序与视口坐标，误触一次就会
落到"点一下松手 = 取消"的分支上。所以断言链走按钮，拖拽只做一次冒烟
（松手后必须有 WATER-DRAG-END 或 WATER-DRAG-CANCEL，不允许什么都不打）。

用法：python tools/verify_water.py
"""

import ctypes
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window           # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors           # noqa: E402
from verify_asset_drag import client_to_screen                   # noqa: E402
import verify_toolkit_modes as vtm                               # noqa: E402

EXE = vtm.EXE
OUT_DIR = vtm.OUT_DIR

user32 = ctypes.windll.user32
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


def wait_event(tap, pattern, timeout=10.0):
    """等一条**事件**行（跳过 logRect 锚点行）。"""
    rx = re.compile(pattern)
    rect_like = re.compile(re.escape(pattern.split("=")[0]) + r"=\(")
    deadline = time.time() + timeout
    while time.time() < deadline:
        with tap.lock:
            for line in reversed(tap.lines):
                if rx.search(line) and not rect_like.search(line):
                    return line
        time.sleep(0.12)
    return None


def num(line, key):
    m = re.search(r"%s=(-?\d+)" % key, line or "")
    return int(m.group(1)) if m else None


def fnum(line, key):
    m = re.search(r"%s=(-?\d+(?:\.\d+)?)" % key, line or "")
    return float(m.group(1)) if m else None


def launch(extra=None):
    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)
    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITOR_SCENE": "scenes/sandbox.json",
        "MYVK_EDITMODE": "water",
    })
    if extra:
        env.update(extra)
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    return proc, tap


def stop(proc):
    try:
        proc.terminate()
        proc.wait(timeout=8)
    except Exception:                            # noqa: BLE001
        pass
    kill_stale_editors()


def viewport_pixels(hwnd, tap):
    """抓一帧，返回 (整图, 视口矩形)。视口矩形取 VP-RECT 锚点。"""
    rect = vtm.rect_of(tap, "VP-RECT", 4.0)
    shot, _ = capture_client(hwnd)
    return shot, rect


def diff_ratio(a, b, box):
    """两张图的差异像素比例（只统计 box 内）。

    ⚠ 边界取**两张图尺寸的交集**：截的是客户区，窗口在两次抓图之间被
      系统调过尺寸（或 DPI 变化）时两张图大小不同，照第一张的坐标去索引
      第二张会直接 IndexError。
    """
    if a is None or b is None or not box:
        return 0.0
    w = min(a.size[0], b.size[0])
    h = min(a.size[1], b.size[1])
    x0, y0, x1, y1 = [int(v) for v in box]
    x0 = max(x0, 0)
    y0 = max(y0, 0)
    x1 = min(x1, w)
    y1 = min(y1, h)
    if x1 <= x0 or y1 <= y0:
        return 0.0
    pa = a.load()
    pb = b.load()
    changed = 0
    total = 0
    step = 2      # 隔行抽样：1088×600 全量比对太慢，2 像素步长足够
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            total += 1
            ca = pa[x, y]
            cb = pb[x, y]
            if (abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) +
                    abs(ca[2] - cb[2])) > 18:
                changed += 1
    return changed / total if total else 0.0


def combo_pick(hwnd, rect, index, tap, pattern):
    """（已弃用）点开 ImGui Combo 再按行高猜第 index 项。

    2026-10-21：面板里的形状 / 水质选择器改成了**一排按钮**（见
    WaterEditor::drawKindButtons 的注释），锚点带值（`WATER-KIND sea`），
    不再需要猜弹出层的行高。这个函数留着是为了"如果以后又出现 Combo"时
    有个参考，但水体面板已经不调它了。
    """
    before = vtm.events(tap, pattern)
    for row_h in (22.0, 24.0, 20.0, 26.0, 28.0):
        vtm.click(hwnd, vtm.center(rect)[0], vtm.center(rect)[1])
        time.sleep(0.45)
        y = rect[3] + 8.0 + row_h * (index + 0.5)
        vtm.click(hwnd, vtm.center(rect)[0], y)
        time.sleep(0.55)
        if vtm.events(tap, pattern) > before:
            return True
        # 没中：下拉可能还开着（点空了会关掉），关掉重来
        vtm.click(hwnd, vtm.center(rect)[0], vtm.center(rect)[1])
        time.sleep(0.25)
        vtm.click(hwnd, vtm.center(rect)[0], vtm.center(rect)[1])
        time.sleep(0.25)
    return False


def press_event(tap, hwnd, tag, pattern, body_tag="WATER-PANEL", timeout=6.0,
                tries=3):
    """点锚点 `tag`，返回它**新打出**的匹配 `pattern` 的事件行（没有 = None）。

    比 vtm.act 多两件事：
      · 只认"点完之后新增的"事件行 —— `wait_event` 会把历史上同名的旧行
        当成答案，断言就成了永真；
      · 失败会重试：参数面板的滚动位置/焦点偶尔差一帧，物理点击就落空了。
        重试对"删除"这类操作是安全的（删成功后按钮就没了，不会误删第二个）。
    """
    for _ in range(tries):
        r = vtm.reveal(tap, hwnd, tag, body_tag)
        if not r:
            time.sleep(0.3)
            continue
        before = vtm.events(tap, pattern)
        vtm.click(hwnd, *vtm.center(r))
        deadline = time.time() + timeout
        while time.time() < deadline:
            if vtm.events(tap, pattern) > before:
                return wait_event(tap, pattern, 1.5)
            time.sleep(0.12)
    return None


def alive(tap, tag, dwell=1.2):
    """锚点是否**还在被重画** = 面板里现在真的存在这个控件。

    ⚠ 只看 `rect_of` 会拿到历史行：控件是条件渲染的（矩形水体没有"半径"
      滑块），条件关掉之后旧日志行还留着 —— 照着旧坐标点，点中的就是别的
      控件。判据必须换成"这一秒还在新增行吗"。
    """
    n0 = vtm.count(tap, tag)
    time.sleep(dwell)
    return vtm.count(tap, tag) > n0


def drag_slider(tap, hwnd, tag, dx, pattern, body_tag="WATER-PANEL"):
    """在滑块上按下 → 横向拖 dx 像素 → 松开，返回新打出的事件行。

    三个细节都是踩出来的：
      · 坐标必须过 `client_to_screen`：SetCursorPos 收的是**屏幕**坐标，
        直接喂锚点的客户区坐标会把鼠标点到别处（窗口不在屏幕原点），
        表现为"拖动毫无反应"（2026-10-21 E 组假失败的真因）。
      · **不要**"先点一下再按下"：两次点击间隔 < 双击阈值（0.3s）会被 ImGui
        当成双击 → SliderFloat 进入文本输入模式，之后拖动同样没反应。
      · 光标分几步移动：SliderFloat 靠鼠标位移累加，瞬移只算一次 delta。
    """
    r = vtm.reveal(tap, hwnd, tag, body_tag)
    if not r:
        return None
    cx, cy = vtm.center(r)
    before = vtm.events(tap, pattern)
    vtm.focus_window(hwnd)
    time.sleep(0.1)

    def move_to(x, y):
        sx, sy = client_to_screen(hwnd, x, y)
        user32.SetCursorPos(int(sx), int(sy))

    move_to(cx, cy)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    for i in range(1, 5):
        move_to(cx + dx * i / 4.0, cy)
        time.sleep(0.09)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if vtm.events(tap, pattern) > before:
            return wait_event(tap, pattern, 1.5)
        time.sleep(0.12)
    # 兜底：松手那一帧的 IsItemDeactivatedAfterEdit 可能比轮询晚到
    time.sleep(0.8)
    if vtm.events(tap, pattern) > before:
        return wait_event(tap, pattern, 1.0)
    return None


def drawn(tap, hwnd, tag, body_tag="WATER-PANEL"):
    """这个锚点**现在**还在画吗（而不是只留在历史日志里）。

    为什么不能只看 `rect_of`：`ui::logRect` 对"位置没变过的矩形"**不重复
    输出**（见 engine/ui/DebugRects.h 的去重），所以一个控件即使当前没画，
    它的旧行仍然在日志里 —— `rect_of` 会把旧矩形当答案，脚本照着旧坐标点，
    落点毫无意义（2026-10-21 实测：以为'新建'段已展开，其实已收起）。

    办法：让面板**轻微滚动一下再滚回来**（净位移 0）。当前画着的控件矩形
    都会变，于是各输出一次；没画的不会。数一下行数有没有涨即可。
    """
    body = vtm.rect_of(tap, body_tag, 2.0)
    if not body:
        return False
    n0 = vtm.count(tap, tag)
    cx, cy = vtm.center(body)
    vtm.wheel(hwnd, cx, cy, -1)
    time.sleep(0.45)
    vtm.wheel(hwnd, cx, cy, 1)
    time.sleep(0.45)
    return vtm.count(tap, tag) > n0


def open_section(tap, hwnd, header_tag, content_tag, body_tag="WATER-PANEL"):
    """展开一个折叠段（面板按用途折成 Shape / Water quality / Surface motion
    / New water body）。

    展开后把面板滚到底：新建段在面板最底部，展开后它的按钮会落到可视区
    之外（左栏参数区只有 ~400px 高），不滚到底就是"点了没反应"。
    """
    if drawn(tap, hwnd, content_tag, body_tag):
        return True
    h = vtm.reveal(tap, hwnd, header_tag, body_tag)
    if not h:
        return False
    vtm.click(hwnd, *vtm.center(h))
    time.sleep(0.5)
    body = vtm.rect_of(tap, body_tag, 2.0)
    if body:
        vtm.wheel(hwnd, vtm.center(body)[0], vtm.center(body)[1], -6)
        time.sleep(0.4)
    return drawn(tap, hwnd, content_tag, body_tag)


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1

    print("=" * 74)
    print("水体编辑模式：模式入口 / 渲染通道 / 预设 / 参数 / 删除")
    print("=" * 74)

    proc, tap = launch()
    try:
        hwnd = find_window("Venn Editor", timeout=60)
        if not hwnd:
            print("找不到编辑器窗口")
            return 1
        time.sleep(3.0)

        # ---------------- A 模式与面板 ----------------
        print("\n[A] 模式入口与面板")
        item = vtm.rect_of(tap, "MODE-RAIL-ITEM water", 6.0)
        vin = vtm.rect_of(tap, "MODE-RAIL-WIN", 4.0)
        vtm.check(bool(item), "A1a 左栏有 Water 模式行")
        if item and vin:
            vtm.check(vtm.inside(item, vin),
                      "A1b Water 行完整落在模式栏内（%s vs %s）" % (item, vin))
            print("      · 模式栏 %s  最后一行 %s" % (vin, item))

        vtm.check(vtm.switch_mode(tap, hwnd, "water"), "A1c 点击 Water 行")
        time.sleep(0.8)
        em = wait_event(tap, r"EDITMODE-PANELS mode=water", 8.0)
        vtm.check(bool(em), "A2 切到 water 模式（EDITMODE-PANELS mode=water）")
        if em:
            print("      ·", em.strip()[-110:])
            vtm.check("panel=water" in em and "open=1" in em,
                      "A2b 水体面板被打开（panel=water open=1）")

        panel = vtm.rect_of(tap, "WATER-PANEL", 6.0)
        add = vtm.rect_of(tap, "WATER-ADD", 6.0)
        vtm.check(bool(panel), "A3a WATER-PANEL 锚点存在")
        vtm.check(bool(add), "A3b WATER-ADD 按钮锚点存在")

        # ---------------- B 渲染通道 ----------------
        print("\n[B] 渲染通道")
        init = wait_event(tap, r"WATER-INIT ", 6.0)
        vtm.check(bool(init), "B1 水面通道已初始化（WATER-INIT）")
        tgt = wait_event(tap, r"WATER-TARGETS ", 6.0)
        vtm.check(bool(tgt), "B2 目标/帧缓冲已绑定（WATER-TARGETS）")
        for line in (init, tgt):
            if line:
                print("      ·", line.strip()[-110:])

        # 建水前的基线截图
        before, vpbox = viewport_pixels(hwnd, tap)

        # ---------------- C 建一块水体 ----------------
        print("\n[C] 建一块水体（面板按钮）")
        ok, logged = vtm.act(tap, hwnd, "WATER-ADD", "新建水体",
                             r"WATER-CREATE ", body_tag="WATER-PANEL")
        vtm.check(ok, "C0 点到 WATER-ADD")
        created = wait_event(tap, r"WATER-CREATE ", 8.0) if logged else None
        vtm.check(bool(created), "C1 WATER-CREATE 打出")
        if created:
            print("      ·", created.strip()[-150:])
            vtm.check("shape=circle" in created,
                      "C1b 默认形状是圆形（shape=circle）")
            vtm.check("src=button" in created, "C1c 来源是面板按钮（src=button）")
            vtm.check("kind=" in created, "C1d 带水质类型（kind=...）")

        sync = wait_event(tap, r"WATER-SYNC bodies=1", 8.0)
        vtm.check(bool(sync), "C2 渲染层收到 1 块水体（WATER-SYNC bodies=1）")

        mesh = wait_event(tap, r"WATER-MESH ", 8.0)
        vtm.check(bool(mesh), "C3 水面网格已生成（WATER-MESH）")
        if mesh:
            print("      ·", mesh.strip()[-130:])
            vtm.check(num(mesh, "tris") and num(mesh, "tris") > 100,
                      "C3b 三角形数合理（tris=%s）" % num(mesh, "tris"))

        time.sleep(1.2)
        after, _ = viewport_pixels(hwnd, tap)
        r = diff_ratio(before, after, vpbox)
        vtm.check(r > 0.03, "C4 水面真的画进视口（像素变化 %.1f%% > 3%%）"
                  % (r * 100.0))
        vtm.shoot(hwnd, "water_circle.png")

        # ---------------- D 预设与形状 ----------------
        print("\n[D] 水质预设 / 形状（面板按钮）")
        # 选中一块水体之后，"New water body" 段是收起的（它是"下一步动作"，
        # 不是参数）—— 先展开它，按钮才存在
        opened = open_section(tap, hwnd, "WATER-SEC new", "WATER-ADD")
        vtm.check(opened, "D0 展开'新建水体'段（WATER-SEC new → WATER-ADD）")

        sea = press_event(tap, hwnd, "WATER-NEW-KIND sea",
                          r"WATER-NEW-KIND kind=")
        vtm.check(bool(sea), "D1 点'海水'按钮就选中 sea water")
        if sea:
            print("      ·", sea.strip()[-110:])
            vtm.check("kind=sea" in sea, "D1b 预设是 sea")

        rect_btn = press_event(tap, hwnd, "WATER-NEW-SHAPE rect",
                               r"WATER-NEW-SHAPE shape=")
        vtm.check(bool(rect_btn), "D2 点'矩形'按钮就切到 rectangle")
        if rect_btn:
            print("      ·", rect_btn.strip()[-110:])
            vtm.check("shape=rect" in rect_btn, "D2b 形状是 rect")

        # 用刚选的模板再建一块 —— 走通"按钮 → 模板 → 组件 → 渲染"整条链
        created2 = press_event(tap, hwnd, "WATER-ADD", r"WATER-CREATE ")
        vtm.check(bool(created2), "D3 再建一块（WATER-CREATE）")
        if created2:
            print("      ·", created2.strip()[-150:])
            vtm.check("shape=rect" in created2 and "kind=sea" in created2,
                      "D3b 新水体用的是刚选的形状与水质（rect + sea）")
            mesh2 = wait_event(tap, r"WATER-MESH shape=rect", 8.0)
            vtm.check(bool(mesh2), "D3c 矩形网格生成了（WATER-MESH shape=rect）")
            if mesh2:
                print("      ·", mesh2.strip()[-130:])

        # 矩形水体：面板换成"半宽/半深"，拖一下要能改尺寸
        vtmm = vtm
        vtmm.check(drawn(tap, hwnd, "WATER-HALFX"),
                   "D4 矩形体出现半宽滑块（WATER-HALFX）")
        half_line = drag_slider(tap, hwnd, "WATER-HALFX", 60, r"WATER-SIZE ")
        vtmm.check(bool(half_line) and "halfExtent=" in (half_line or ""),
                   "D4b 拖半宽滑块打出矩形分支的 WATER-SIZE halfExtent=")
        if half_line:
            print("      ·", half_line.strip()[-110:])

        # ---------------- E 参数实时生效 ----------------
        print("\n[E] 参数面板（实时改范围 / 水位）")
        # E 组要拖"半径"，先把当前这块切回圆形（顺便验选中水体的形状按钮）
        back = press_event(tap, hwnd, "WATER-SHAPE circle", r"WATER-SHAPE id=")
        vtm.check(bool(back), "E0 切回圆形（WATER-SHAPE id=...）")
        if back:
            print("      ·", back.strip()[-110:])
        vtm.check(drawn(tap, hwnd, "WATER-RADIUS"),
                  "E0b 圆形体出现半径滑块（WATER-RADIUS）")

        size_line = drag_slider(tap, hwnd, "WATER-RADIUS", 130,
                                r"WATER-SIZE ")
        vtm.check(bool(size_line) and "radius=" in (size_line or ""),
                  "E1 拖半径滑块打出 WATER-SIZE radius=")
        if size_line:
            print("      ·", size_line.strip()[-110:])
            vtm.check(fnum(size_line, "radius") != 10.0,
                      "E1b 半径真的变了（radius=%s，初始 10.0）"
                      % fnum(size_line, "radius"))

        lvl_line = drag_slider(tap, hwnd, "WATER-LEVEL", 90,
                               r"WATER-LEVEL id=")
        vtm.check(bool(lvl_line), "E2 拖水位滑块打出 WATER-LEVEL")
        if lvl_line:
            print("      ·", lvl_line.strip()[-110:])
            vtm.check(fnum(lvl_line, "level") != 1.35,
                      "E2b 水位真的变了（level=%s，初始 1.35）"
                      % fnum(lvl_line, "level"))

        vtm.shoot(hwnd, "water_panel.png")

        # ---------------- F 删除 ----------------
        print("\n[F] 删除水体")
        def last_bodies():
            with tap.lock:
                for line in reversed(tap.lines):
                    if "WATER-SYNC bodies=" in line:
                        return num(line, "bodies")
            return None

        before_bodies = last_bodies()
        del_line = press_event(tap, hwnd, "WATER-DELETE",
                               r"WATER-DELETE id=")
        vtm.check(bool(del_line), "F1 删除打出 WATER-DELETE")
        if del_line:
            print("      ·", del_line.strip()[-110:])
        time.sleep(1.2)
        after_bodies = last_bodies()
        vtm.check(before_bodies is not None and after_bodies is not None and
                  after_bodies == before_bodies - 1,
                  "F2 渲染层少了一块水体（bodies %s → %s）"
                  % (before_bodies, after_bodies))

        # ---------------- G 日志健康 ----------------
        print("\n[G] 日志健康")
        bad = []
        with tap.lock:
            for line in tap.lines:
                if "VUID-" in line or "[ERROR" in line or "[ERROR]" in line:
                    bad.append(line.strip())
        vtm.check(not bad, "G1 零 VUID / 零 ERROR（%d 条）" % len(bad))
        for line in bad[:6]:
            print("      !", line[-140:])
    finally:
        stop(proc)

    print("\n截图：%s" % OUT_DIR)
    if vtm.fails:
        print("失败 %d 条：" % len(vtm.fails))
        for f in vtm.fails:
            print("  -", f)
        # ⚠ 结果行必须用 **ASCII 冒号**：run_regression 的 OK_RE/BAD_RE 只认
        #   `结果: PASS` / `结果: FAIL`。只写"全部通过。"会被框架判成 FAIL
        #   （本用例之前没进 DEFAULT，所以这个坑一直没暴露）。
        print("结果: FAIL（%d 项）" % len(vtm.fails))
        return 1
    print("全部通过。")
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
