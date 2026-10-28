#!/usr/bin/env python3
"""E29-2 / E29-3 验收：面板标题栏拖拽（交换 / 并成标签组）。

为什么要单独一组用例：
    E29-1 把"哪个面板在哪一格"变成了**数据**，于是"拖了没反应""拖反了"
    "拖完面板裂成两块"都不会崩、也不会报错 —— 全是静默的。
    纯数值自检（`MYVK_LAYOUT_TEST=1` 的 F 组）能证明状态机对，但证明不了
    **鼠标事件真的喂进了那个状态机**（`IsMouseClicked` 没接上、标题栏高度
    算错、坐标不是客户区坐标……都只在真窗口里暴露）。

验什么：
  A  自检 F/G 两组存在且全绿（`drag_hit_title` / `drag_drop_swapped` /
     `merge_applied` / `tabstrip_pill_count`）
  B  **真拖 · 交换**：按住 Inspector 标题栏拖到模式栏中部松手
     （模式栏不能当标签成员 → 中部落点退回"整格交换"）
     → 日志必须出现 `LAYOUT-DRAG from=inspector to=moderail`
     → 拖后的 `LAYOUT-RECTS` 里两格矩形**整体互换**（不是"面板画歪了"）
  C  **对照**：同样按住标题栏但**不移动**就松开（一次点击）
     → 不许出现 `LAYOUT-DRAG`，矩形一个都不许变
     没有 C 就分不清"B 组是拖拽生效"还是"启动时就那样"。
  D  **真拖 · 合并**（E29-3）：把 Hierarchy 拖到 Inspector 的**正中**
     → `LAYOUT-MERGE from=hierarchy to=inspector size=2`
     → 两枚标签锚点 `LAYOUT-TAB-hierarchy` / `LAYOUT-TAB-inspector` 都在
     → 点 Inspector 标签 → `LAYOUT-TABCLICK`，矩形换人显示
     → 双击 Hierarchy 标签 → `LAYOUT-TABHOME`，右栏恢复上下两格
     没有 D 就证明不了"合并之后还能出来"——而那是这条路唯一的单向门。

不验什么：
  * 落盘：自动化运行下布局**写一律拒绝**（见 EditorLayoutIO.h），所以
    "拖完的文件里有没有 slots"只能手测。
  * 标签条**颜色**：D 组只验"锚点存在 + 点得中"。

  E  **真拖·根分隔条**：把「Modes | 视口」那条线往右拖 → 左栏变宽、视口变窄；
     再往左拖 600px 必须停在下限（~300px，不是旧的 460）；最后往右还能变回来。
     守的是 2026-10-28 用户报的「视口往左拖扩不开、Modes 一动不动」——
     根因是 `minA` 恒等于默认宽度 460，`clamp` 之后一动都不动。
     （2026-10-28 #317 之后默认宽度也是 300 = 下限，所以 E1 先往**右**拖；
       判据没变：两个方向都动、停在下限、不塌成 0。）

用法：
    python tools/verify_layout_drag.py
"""

import ctypes
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window                                # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402
from verify_asset_drag import force_foreground                       # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")

user32 = ctypes.windll.user32

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
MK_LBUTTON = 0x0001

LAYOUT_RE = re.compile(
    r"LAYOUT-RECTS modes=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"hier=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"insp=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
    return bool(ok)


# ------------------------------------------------------------------ 鼠标注入
def _lparam(x, y):
    return (int(x) & 0xFFFF) | ((int(y) & 0xFFFF) << 16)


def _client_to_screen(hwnd, x, y):
    class POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]
    pt = POINT(int(x), int(y))
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y


def drag(hwnd, x0, y0, x1, y1, steps=14, hold=0.05):
    """按住左键从 (x0,y0) 拖到 (x1,y1)（客户区坐标）。

    两条通道同时走：
      · `PostMessage(WM_*)` —— GLFW 的 win32 后端就是靠这几条消息把
        `MouseDown` / `MousePos` 喂给 ImGui 的，不需要窗口在前台；
      · `SetCursorPos` —— 顺带把**真实**光标也挪过去。少了它，Windows 会给
        窗口补发 WM_MOUSELEAVE，ImGui 那一帧的 MousePos 变成 -FLT_MAX
        （`verify_player.py` 的注释里记过这条坑）。
    """
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, _lparam(x0, y0))
    user32.SetCursorPos(*_client_to_screen(hwnd, x0, y0))
    # ⚠ 起手要"预热"两次移动 + 0.35s：`IsMouseClicked` 只在**按下那一帧**
    #   判命中，而 ImGui 的 MousePos 要靠 WM_MOUSEMOVE 喂进来。上一个动作
    #   常把光标甩到客户区外（拖完 600px 之后在 x=340 或干脆出界），此时
    #   窗口会补发 WM_MOUSELEAVE、MousePos 变 -FLT_MAX，第一次 MOVE 那一帧
    #   往往还没落定 —— 表现是"这一组时红时绿，换个会话就好了"。
    #   实测：B1 / E1 各红过一次，都是这个相位问题（判定逻辑没问题）。
    time.sleep(0.18)
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, _lparam(x0, y0))
    time.sleep(0.22)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, _lparam(x0, y0))
    time.sleep(0.15)
    for i in range(1, steps + 1):
        px = x0 + (x1 - x0) * i / steps
        py = y0 + (y1 - y0) * i / steps
        user32.PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, _lparam(px, py))
        user32.SetCursorPos(*_client_to_screen(hwnd, int(px), int(py)))
        time.sleep(hold)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, _lparam(x1, y1))
    time.sleep(0.45)


# ------------------------------------------------------------------ 日志读取
def latest_rect(tap):
    """最后一条 LAYOUT-RECTS → (modes, hier, insp)，每条是 (x,y,w,h)。"""
    with tap.lock:
        lines = list(tap.lines)
    for line in reversed(lines):
        if "LAYOUT-RECTS" not in line:
            continue
        m = LAYOUT_RE.search(line)
        if m:
            v = [float(m.group(i)) for i in range(1, 13)]
            return (tuple(v[0:4]), tuple(v[4:8]), tuple(v[8:12]))
    return None


def rect_count(tap):
    with tap.lock:
        return sum(1 for l in tap.lines if "LAYOUT-RECTS" in l)


def wait_new_rect(tap, after, timeout=8.0):
    """等第 after+1 条以后的 LAYOUT-RECTS（避免拿到拖动前的陈旧矩形）。"""
    end = time.time() + timeout
    while time.time() < end:
        if rect_count(tap) > after:
            r = latest_rect(tap)
            if r:
                return r
        time.sleep(0.12)
    return None


def click(hwnd, x, y):
    """单击一个点（客户区坐标）。"""
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, _lparam(x, y))
    user32.SetCursorPos(*_client_to_screen(hwnd, int(x), int(y)))
    time.sleep(0.12)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, _lparam(x, y))
    time.sleep(0.10)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, _lparam(x, y))
    time.sleep(0.40)


def double_click(hwnd, x, y):
    """双击。

    ⚠ ImGui 判双击用的是 `MouseDoubleClickTime`（默认 **0.30 s**）：两次
      `WM_LBUTTONDOWN` 的间隔必须小于它。`drag()` 里那些 0.15 s 的 sleep
      放在这里就**永远判不出双击** —— 分开写一个函数，别复用 drag 的节奏。
    """
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, _lparam(x, y))
    user32.SetCursorPos(*_client_to_screen(hwnd, int(x), int(y)))
    time.sleep(0.12)
    for _ in range(2):
        user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, _lparam(x, y))
        time.sleep(0.03)
        user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, _lparam(x, y))
        time.sleep(0.06)
    time.sleep(0.45)


TAG_RE = re.compile(
    r"(LAYOUT-TAB-[\w-]+)=\(([-\d.]+),([-\d.]+)\)-\(([-\d.]+),([-\d.]+)\)")
# 通用锚点：`SPLIT-0=(..)-(..)` / `VP-RECT=(..)-(..)` 这类。
# ⚠ `TAG_RE` 只认标签条，因为 D 组只关心它；E 组要看分隔条与视口，
#   所以另写一条。分开写比给 TAG_RE 加一堆可选前缀干净。
ANY_RE = re.compile(
    r"\b([A-Z][A-Z0-9_-]*)=\(([-\d.]+),([-\d.]+)\)-\(([-\d.]+),([-\d.]+)\)")


def tag_rects(tap):
    """所有 `TAG=(x0,y0)-(x1,y1)` 锚点（后出现的覆盖先出现的）。"""
    out = {}
    with tap.lock:
        for line in tap.lines:
            for m in TAG_RE.finditer(line):
                out[m.group(1)] = tuple(float(m.group(i)) for i in range(2, 6))
    return out


def wait_tag(tap, tag, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tag_rects(tap).get(tag)
        if r:
            return r
        time.sleep(0.15)
    return None


def start_editor():
    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LOG_RECTS"] = "1"
    env.pop("MYVK_EDITOR_LAYOUT", None)   # 干净默认树
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    return proc, tap


def stop_editor(proc):
    try:
        proc.terminate()
        proc.wait(timeout=8)
    except Exception:
        proc.kill()


# ------------------------------------------------------------------ A 自检
def group_selftest():
    env = dict(os.environ)
    env["MYVK_LAYOUT_TEST"] = "1"
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    try:
        out, _ = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate(timeout=10)
    out = out or ""

    total = re.search(r"LAYOUT-TOTAL pass=(\d+) fail=(\d+)", out)
    fails = re.findall(r"LAYOUT-TEST (\S+).*result=FAIL", out)
    ok = total is not None and total.group(2) == "0" and int(total.group(1)) >= 90
    check("A1 布局自检全绿（>=90 条，含 F 拖拽 + G 标签组）", ok,
          (total.group(0) if total else "没有 LAYOUT-TOTAL 行") +
          ("；FAIL=%s" % ",".join(fails) if fails else ""))

    # F / G 组存在性：这几条只有"拖拽 / 标签组"才可能有
    for anchor in ("drag_hit_title", "drag_drop_swapped", "merge_applied",
                   "tabstrip_pill_count", "detach_restores_two_cells"):
        check("A2 断言 %s 存在" % anchor,
              ("LAYOUT-TEST %s " % anchor) in out,
              "锚点%s" % ("" if anchor in out else "缺"))


# ------------------------------------------------------- B 真拖 / C 对照
def group_drag():
    proc, tap = start_editor()
    try:
        hwnd = find_window("Venn Editor", timeout=45)
        if hwnd:
            # ⚠ 必须先把窗口提到前台：ImGui 的 GLFW 后端**只在窗口有焦点时**
            #   才更新 io.MousePos。没焦点时鼠标位置是上一帧的陈旧值，
            #   标题栏拖拽/分隔条悬停全部落空 —— 表现就是"这条用例时红时绿"。
            force_foreground(hwnd)
        if not hwnd:
            check("B0 编辑器窗口出现", False, "没找到 Venn Editor")
            return
        # 多等一会：首帧的 LAYOUT-RECTS 可能还是"初始化中"的比例
        time.sleep(3.0)
        before = latest_rect(tap)
        if not before:
            check("B0 读到 LAYOUT-RECTS", False, "日志里没有")
            return
        modes0, hier0, insp0 = before
        mx, my, mw, mh = modes0
        ix, iy, iw, ih = insp0

        # 起点 = Inspector 标题栏中心（标题栏高度约 24px，取 12）
        sx = ix + iw * 0.5
        sy = iy + 12.0
        # 终点 = 模式栏中部（落点用整格，不必是标题栏）
        tx = mx + mw * 0.5
        ty = my + mh * 0.5

        n0 = rect_count(tap)
        drag(hwnd, sx, sy, tx, ty)

        # ---- B1 日志：真的发生了一次交换 ----
        end = time.time() + 8.0
        while time.time() < end and not tap.has_line("LAYOUT-DRAG"):
            time.sleep(0.15)
        dragline = None
        with tap.lock:
            for l in tap.lines:
                if "LAYOUT-DRAG" in l:
                    dragline = l
        check("B1 拖拽产生了 LAYOUT-DRAG", dragline is not None,
              "锚点%s" % ("缺（鼠标事件没喂进去 / 抓手没命中标题栏）"
                          if dragline is None else "有"))
        if dragline:
            m = re.search(r"from=(\S+) to=(\S+)", dragline)
            got = (m.group(1), m.group(2)) if m else ("?", "?")
            check("B2 交换的是 inspector ↔ moderail", got == ("inspector", "moderail"),
                  "actual=%s->%s" % got)

        # ---- B3 矩形整体互换 ----
        after = wait_new_rect(tap, n0)
        if not after:
            check("B3 拖后读到新的 LAYOUT-RECTS", False, "没有新行")
            return
        modes1, hier1, insp1 = after
        check("B3 Inspector 抢到了最左那一格（insp.x≈0 且宽度≈原模式栏）",
              insp1[0] < 2.0 and abs(insp1[2] - mw) < 3.0,
              "insp=(%.0f,%.0f,%.0f,%.0f) 原modes宽=%.0f" %
              (insp1[0], insp1[1], insp1[2], insp1[3], mw))
        check("B4 模式栏挪到了 Inspector 原来的位置（两格整体互换）",
              abs(modes1[0] - ix) < 3.0 and abs(modes1[2] - iw) < 3.0,
              "modes=(%.0f,%.0f,%.0f,%.0f) 原insp=(%.0f,%.0f,%.0f,%.0f)" %
              (modes1[0], modes1[1], modes1[2], modes1[3], ix, iy, iw, ih))
        check("B5 Hierarchy 没被连累（它还在右栏上部）",
              abs(hier1[0] - hier0[0]) < 3.0 and abs(hier1[1] - hier0[1]) < 3.0,
              "hier=%.0f,%.0f → %.0f,%.0f" %
              (hier0[0], hier0[1], hier1[0], hier1[1]))
    finally:
        stop_editor(proc)

    # ---- C 对照：只点不拖 ----
    proc2, tap2 = start_editor()
    try:
        hwnd = find_window("Venn Editor", timeout=45)
        if hwnd:
            # ⚠ 必须先把窗口提到前台：ImGui 的 GLFW 后端**只在窗口有焦点时**
            #   才更新 io.MousePos。没焦点时鼠标位置是上一帧的陈旧值，
            #   标题栏拖拽/分隔条悬停全部落空 —— 表现就是"这条用例时红时绿"。
            force_foreground(hwnd)
        if not hwnd:
            check("C0 对照窗口出现", False, "没找到")
            return
        time.sleep(3.0)
        before = latest_rect(tap2)
        if not before:
            check("C0 对照读到 LAYOUT-RECTS", False, "没有")
            return
        modes0, hier0, insp0 = before
        n0 = rect_count(tap2)
        ix, iy, iw, ih = insp0
        # 同一个起点，但**原地按下再松开**（不移动）→ 一次普通点击
        drag(hwnd, ix + iw * 0.5, iy + 12.0, ix + iw * 0.5, iy + 12.0, steps=2,
             hold=0.05)
        time.sleep(1.2)
        check("C1 只点击不拖 → 没有 LAYOUT-DRAG", not tap2.has_line("LAYOUT-DRAG"),
              "锚点%s" % ("误报" if tap2.has_line("LAYOUT-DRAG") else "无"))
        after = latest_rect(tap2)
        check("C2 只点击不拖 → 矩形一个都没变",
              after is not None and after == before,
              "before=%s after=%s" % (before, after))
        _ = n0
    finally:
        stop_editor(proc2)


# --------------------------------------------------- D 合并 / E29-3
def group_tabs():
    proc, tap = start_editor()
    try:
        hwnd = find_window("Venn Editor", timeout=45)
        if hwnd:
            # ⚠ 必须先把窗口提到前台：ImGui 的 GLFW 后端**只在窗口有焦点时**
            #   才更新 io.MousePos。没焦点时鼠标位置是上一帧的陈旧值，
            #   标题栏拖拽/分隔条悬停全部落空 —— 表现就是"这条用例时红时绿"。
            force_foreground(hwnd)
        if not hwnd:
            check("D0 编辑器窗口出现", False, "没找到 Venn Editor")
            return
        time.sleep(3.0)
        before = latest_rect(tap)
        if not before:
            check("D0 读到 LAYOUT-RECTS", False, "日志里没有")
            return
        modes0, hier0, insp0 = before
        hx, hy, hw, hh = hier0
        ix, iy, iw, ih = insp0

        n0 = rect_count(tap)
        # Hierarchy 标题栏 → Inspector **正中**（中部 = 并入，不是交换）
        drag(hwnd, hx + hw * 0.5, hy + 12.0, ix + iw * 0.5, iy + ih * 0.5)

        # ---- D1 日志：真的合并了，且组里变成 2 个 ----
        end = time.time() + 8.0
        while time.time() < end and not tap.has_line("LAYOUT-MERGE"):
            time.sleep(0.15)
        mergeline = None
        with tap.lock:
            for l in tap.lines:
                if "LAYOUT-MERGE" in l:
                    mergeline = l
        check("D1 拖到格子中部产生了 LAYOUT-MERGE", mergeline is not None,
              "锚点%s（中部没判成 merge / 落点不是 inspector）" %
              ("缺" if mergeline is None else "有"))
        if not mergeline:
            return
        m = re.search(r"from=(\S+) to=(\S+) size=(\d+)", mergeline)
        got = (m.group(1), m.group(2), m.group(3)) if m else ("?", "?", "?")
        check("D2 合的是 hierarchy → inspector，组里 2 个",
              got == ("hierarchy", "inspector", "2"), "actual=%s" % (got,))

        # ---- D3/D4 矩形：整格归活动面板，非活动的被清零 ----
        after = wait_new_rect(tap, n0)
        if not after:
            check("D3 合并后读到新的 LAYOUT-RECTS", False, "没有新行")
            return
        modes1, hier1, insp1 = after
        check("D3 合并后 Hierarchy 占满右栏（x/w 跟原 Inspector 一致）",
              abs(hier1[0] - ix) < 3.0 and abs(hier1[2] - iw) < 3.0 and
              hier1[3] > hh + 10.0,
              "hier=(%.0f,%.0f,%.0f,%.0f) 原insp=(%.0f,%.0f,%.0f,%.0f)" %
              (hier1[0], hier1[1], hier1[2], hier1[3], ix, iy, iw, ih))
        check("D4 非活动标签不占矩形（Inspector 宽 0）", insp1[2] < 1.0,
              "insp=(%.0f,%.0f,%.0f,%.0f)" % insp1)

        # ---- D5 两枚标签都在 ----
        tab_h = wait_tag(tap, "LAYOUT-TAB-hierarchy", 6.0)
        tab_i = wait_tag(tap, "LAYOUT-TAB-inspector", 6.0)
        check("D5 两个标签锚点都在", bool(tab_h) and bool(tab_i),
              "hier=%s insp=%s" % (tab_h is not None, tab_i is not None))
        if not (tab_h and tab_i):
            return
        check("D6 两枚标签并排（inspector 在 hierarchy 左侧或右侧都可，但不能重叠）",
              abs(tab_h[0] - tab_i[0]) > 1.0 or abs(tab_h[2] - tab_i[2]) > 1.0,
              "hier x0=%.0f x1=%.0f | insp x0=%.0f x1=%.0f" %
              (tab_h[0], tab_h[2], tab_i[0], tab_i[2]))

        # ---- D7 点 Inspector 标签 → 换成它显示 ----
        n1 = rect_count(tap)
        click(hwnd, (tab_i[0] + tab_i[2]) * 0.5, (tab_i[1] + tab_i[3]) * 0.5)
        end = time.time() + 6.0
        while time.time() < end and not tap.has_line("LAYOUT-TABCLICK"):
            time.sleep(0.15)
        clickline = None
        with tap.lock:
            for l in tap.lines:
                if "LAYOUT-TABCLICK" in l:
                    clickline = l
        check("D7 点标签产生了 LAYOUT-TABCLICK panel=inspector",
              clickline is not None and "panel=inspector" in clickline,
              "anchor=%s" % (clickline.strip() if clickline else "缺"))
        after2 = wait_new_rect(tap, n1)
        if after2:
            modes2, hier2, insp2 = after2
            check("D8 切换后 Inspector 可见、Hierarchy 归零",
                  insp2[2] > 1.0 and hier2[2] < 1.0,
                  "insp=(%.0f,%.0f,%.0f,%.0f) hier.w=%.0f" %
                  (insp2[0], insp2[1], insp2[2], insp2[3], hier2[2]))
        else:
            check("D8 切换后读到新的 LAYOUT-RECTS", False, "没有新行")

        # ---- D9 双击 Hierarchy 标签 → 送回老家，右栏恢复两格 ----
        n2 = rect_count(tap)
        lab_h = wait_tag(tap, "LAYOUT-TAB-hierarchy", 4.0) or tab_h
        # 双击**可能要重试**：PostMessage 的两下点击偶尔会落进同一帧，
        # ImGui 只当成一次单击（这一条在别的用例里也遇到过）。
        home = None
        for _ in range(3):
            double_click(hwnd, (lab_h[0] + lab_h[2]) * 0.5,
                         (lab_h[1] + lab_h[3]) * 0.5)
            end = time.time() + 3.0
            while time.time() < end and not tap.has_line("LAYOUT-TABHOME"):
                time.sleep(0.15)
            if tap.has_line("LAYOUT-TABHOME"):
                with tap.lock:
                    for l in tap.lines:
                        if "LAYOUT-TABHOME" in l:
                            home = l.strip()
                break
            lab_h = wait_tag(tap, "LAYOUT-TAB-hierarchy", 2.0) or lab_h
        check("D9 双击标签产生了 LAYOUT-TABHOME panel=hierarchy",
              home is not None and "panel=hierarchy" in home,
              "anchor=%s" % (home if home else "缺（双击没被识别 / 双击没接上）"))

        after3 = wait_new_rect(tap, n2)
        if after3:
            modes3, hier3, insp3 = after3
            check("D10 拆回后右栏恢复上下两格（两块都可见）",
                  hier3[2] > 1.0 and hier3[3] > 1.0 and insp3[2] > 1.0 and
                  insp3[3] > 1.0,
                  "hier=(%.0f,%.0f,%.0f,%.0f) insp=(%.0f,%.0f,%.0f,%.0f)" %
                  (hier3 + insp3))
        else:
            check("D10 拆回后读到新的 LAYOUT-RECTS", False, "没有新行")
    finally:
        stop_editor(proc)


def any_rect(name, tap, timeout=8.0):
    """任意 `NAME=(x0,y0)-(x1,y1)` 锚点的**最后**一条（后出现的覆盖先出现的）。"""
    end = time.time() + timeout
    got = None
    while time.time() < end:
        with tap.lock:
            for line in tap.lines:
                for m in ANY_RE.finditer(line):
                    if m.group(1) == name:
                        got = tuple(float(m.group(i)) for i in range(2, 6))
        if got:
            return got
        time.sleep(0.15)
    return got


# ------------------------------------------------------- E 拖根分隔条：左栏可缩
def group_split():
    """E：把「Modes | 视口」那条分隔条往左拖，左栏必须跟着变窄、视口跟着变宽。

    守的是 2026-10-28 用户报的那条：

        「Viewport 视口往左边拖拽扩大区域的时候，无法继续扩大，并且在此过程中
          左边的 Modes 区域似乎是保持不动的。」

    根因：`SplitLayout` 里根节点的 `minA` 恒等于 460，而那**正好**是首帧算
    出来的默认宽度 —— 于是往下限方向拖时 `clamp(pixel, 460, ...)` 一动不动，
    看起来像"分隔条坏了"。修法是把下限改成按当前编辑模式问
    （`editModeRailMinWidth`：Landscape / Foliage / Select = 300）。

    ⚠ 判据必须是**两个方向都动**：
      E1 往右 160px → modes 变宽 ~160、viewport 变窄（不是"只有一个变"）
      E2 往左 600px → modes 停在下限 ~300（下限生效，**不许**塌成 0）
      E3 往右 200px → 又变宽（说明不是单向卡死，也不是"下限被误当成固定值"）

    ⚠⚠ 2026-10-28（#317）改过一次**起手方向**：默认左栏宽度也改成"按当前
      编辑模式取"（`editModeRailMinWidth`）之后，Select 下**开局就是 300**，
      也就是**已经贴着下限**了 —— 再往左拖本来就一格都不会动。所以 E1 改成
      先往右拖（有余量的方向），E2 再从 460 往左撞下限。判据一个字没松：
      "两个方向都动 + 停在下限 300 + 不塌成 0"原样保留。

    ⚠ E2 是这条用例的核心：修复前 modes 会停在 **460**，所以"宽 ≤ 320"
      在旧实现下必红；而"≥ 290"防的是另一个错法（有人干脆把 minA 设成 0，
      左栏被拖没，模式列表点都点不到）。
    """
    proc, tap = start_editor()
    try:
        hwnd = find_window("Venn Editor", timeout=45)
        if hwnd:
            # ⚠ 必须先把窗口提到前台：ImGui 的 GLFW 后端**只在窗口有焦点时**
            #   才更新 io.MousePos。没焦点时鼠标位置是上一帧的陈旧值，
            #   标题栏拖拽/分隔条悬停全部落空 —— 表现就是"这条用例时红时绿"。
            force_foreground(hwnd)
        if not hwnd:
            check("E0 编辑器窗口出现", False, "没找到 Venn Editor")
            return
        time.sleep(3.0)   # 等首帧比例收敛（与 B 组同一套理由）
        before = latest_rect(tap)
        if not before:
            check("E0 读到 LAYOUT-RECTS", False, "日志里没有")
            return
        modes0 = before[0]
        mw0 = modes0[2]

        sp = any_rect("SPLIT-0", tap, 6.0)
        if not sp:
            check("E0 读到 SPLIT-0 分隔条锚点", False, "日志里没有")
            return
        vp_before = any_rect("VP-RECT", tap, 6.0)
        line_x = (sp[0] + sp[2]) * 0.5
        line_y = (sp[1] + sp[3]) * 0.5

        # ---- E1 先往右 160（默认就贴着下限，往左没有余量）----
        n0 = rect_count(tap)
        drag(hwnd, line_x, line_y, line_x + 160.0, line_y)
        r1 = wait_new_rect(tap, n0)
        if not r1:
            check("E1 拖后读到新的 LAYOUT-RECTS", False, "没有新行")
            return
        mw1 = r1[0][2]
        check("E1 往右拖 160px → Modes 真的变宽了（修前是 0 变化）",
              mw1 - mw0 > 120.0,
              "modes %.0f → %.0f" % (mw0, mw1))
        # 同一时刻视口必须变窄：两个都变才算"分栏在动"（只有一个变说明
        # 拽到的是别的节点的线）。
        #
        # ⚠ 阈值不能写成"≈ 左栏吃掉的全部"：右栏（Hierarchy/Inspector）是
        #   **按比例**跟着 restW 走的（`mid.ratio` 是一个比例），所以左栏多吃
        #   的 160px 里有一部分从右栏来。判据只要"视口让出了增长量的一半以上"
        #   就足以区分"分栏在动"和"没动"。
        vp1 = any_rect("VP-RECT", tap, 6.0)
        vp_w_before = (vp_before[2] - vp_before[0]) if vp_before else -1.0
        vp_w_after = (vp1[2] - vp1[0]) if vp1 else -2.0
        check("E2 同时视口变窄了（同一块空间换人，不是凭空多出来）",
              vp1 is not None and
              (vp_w_before - vp_w_after) > 0.5 * (mw1 - mw0),
              "视口宽 %.0f → %.0f（左栏吃掉 %.0f）"
              % (vp_w_before, vp_w_after, mw1 - mw0))

        # ---- E2 继续往左 600：撞下限 ----
        sp2 = any_rect("SPLIT-0", tap, 4.0) or sp
        lx2 = (sp2[0] + sp2[2]) * 0.5
        n1 = rect_count(tap)
        drag(hwnd, lx2, line_y, lx2 - 600.0, line_y)
        r2 = wait_new_rect(tap, n1)
        if not r2:
            check("E2 撞下限后读到新的 LAYOUT-RECTS", False, "没有新行")
            return
        mw2 = r2[0][2]
        check("E3 左栏停在下限 ~300px（旧实现在这里停在 460）",
              290.0 <= mw2 <= 330.0, "modes=%.0f" % mw2)
        check("E4 左栏没有被拖没（下限不是 0）", mw2 >= 280.0,
              "modes=%.0f" % mw2)

        # ---- E5 往右 200：能变回来 ----
        # ⚠ 这一步在**贴着下限**的位置起手，是整组里最容易打空的一下：
        #   合成鼠标的 `IsMouseClicked` 要求"按下那一帧鼠标正好在线上的热区里"，
        #   而上一步刚把光标甩到客户区外（-300）。所以先把指针停在线上
        #   让它"热"一帧再拖；万一还是打空就重试一次（并在输出里注明）。
        ok5 = False
        used_retry = False
        mw3 = mw2
        for attempt in range(2):
            sp3 = any_rect("SPLIT-0", tap, 4.0)
            if not sp3:
                break
            lx3 = (sp3[0] + sp3[2]) * 0.5
            force_foreground(hwnd)
            user32.SetCursorPos(*_client_to_screen(hwnd, lx3, line_y))
            time.sleep(0.35)
            n2 = rect_count(tap)
            drag(hwnd, lx3, line_y, lx3 + 200.0, line_y)
            r3 = wait_new_rect(tap, n2)
            mw3 = r3[0][2] if r3 else mw2
            if mw3 - mw2 > 150.0:
                ok5 = True
                used_retry = attempt > 0
                break
        check("E5 再往右拖 200px 又变宽（不是单向卡死）", ok5,
              "modes %.0f → %.0f%s" % (mw2, mw3,
                                       "（第二次才命中）" if used_retry else ""))
    finally:
        stop_editor(proc)


def cursor_name():
    """当前系统光标的名字（按 `LoadCursor` 的标准句柄比对）。

    ⚠ 只能读到**真实**光标：`SetCursorPos` 之后要给窗口一两帧时间，
      GLFW 才会把 ImGui 请求的光标 `SetCursor` 上去。所以调用前先 hover
      并 sleep 0.4~0.5s（见 F 组的用法），否则读到的是上一个位置的光标。
    """
    class CURSORINFO(ctypes.Structure):
        _fields_ = [("cbSize", ctypes.c_ulong), ("flags", ctypes.c_ulong),
                    ("hCursor", ctypes.c_void_p),
                    ("ptScreenPos", ctypes.c_long * 2)]
    ci = CURSORINFO()
    ci.cbSize = ctypes.sizeof(ci)
    user32.GetCursorInfo(ctypes.byref(ci))
    names = {}
    for nm, idd in (("ARROW", 32512), ("IBEAM", 32513), ("WAIT", 32514),
                    ("CROSS", 32515), ("SIZEWE", 32644), ("SIZENS", 32645),
                    ("SIZEALL", 32646), ("HAND", 32649)):
        names[user32.LoadCursorW(None, ctypes.c_wchar_p(idd))] = nm
    return names.get(ci.hCursor, "other")


def hover(hwnd, x, y, settle=0.5):
    """把鼠标停在某点（不发按键），等光标切换稳定。"""
    user32.SetCursorPos(*_client_to_screen(hwnd, int(x), int(y)))
    time.sleep(0.1)
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, _lparam(x, y))
    time.sleep(settle)


def corner_logs(tap):
    with tap.lock:
        return [l for l in tap.lines if "LAYOUT-CORNER-" in l]


# ------------------------------------------- F 角落抓手：角上合并 / 边上调大小
def group_corner():
    """F：两条分隔条的**交点**是"合并"手势，其余线段是"调大小"手势。

    守的是 2026-10-28 用户提的规则（原话）：

        「当鼠标放在两个面板的角的交界时…拖拽时显示成一个加号，这个时候
          往左拖拽意味着左边区域面板被合并到左边，反之亦然。而在除了角落
          的其他交接处位置，则是正常地调整面板区域的显示面积大小。」

    为什么不能只验"能不能合并"：
      · 合并这条路径藏在 `movePanel` 里，而"角落到底抢不抢得到悬停"取决于
        `m_splitHoverTaken` 的置位顺序 —— 顺序错了的表现是"角上拖一下，
        线跟着动了、面板没合"，日志里**两行都有**，光看有没有 MERGE 会漏;
      · 光标是这条需求的一半（用户要的就是"显示成一个加号"）。`SIZEALL`
        是 Windows 的四向箭头（看着就是一个加号），而线上必须是 `SIZEWE`/
        `SIZENS` —— 这两个值一对，才说明"角落和边被切开了"。

    判据：
      F1 角点锚点的中心 == (竖线 x, 横线 y)（差 < 3px）
      F2 悬停角落 → 光标 SIZEALL          ← 用户要的"加号"
      F3 悬停线上（离角 60px）→ 光标 SIZENS  ← 对照组：边上没有被抢走
      F4 线上往下拖 80px → 横线真的移动了，而且**没有** LAYOUT-CORNER 日志
      F5 角上往下拖 → LAYOUT-CORNER-MERGE from/to = hierarchy/inspector
      F6 合并后右栏只剩一格（另一个矩形收成 0）
      F7 角上往左拖 → 走 Swap（视口不能当标签成员，退回整格交换）
    """
    proc, tap = start_editor()
    try:
        hwnd = find_window("Venn Editor", timeout=45)
        if hwnd:
            force_foreground(hwnd)
        if not hwnd:
            check("F0 编辑器窗口出现", False, "没找到 Venn Editor")
            return
        time.sleep(3.0)

        # ---- F1 角点几何：必须正好落在两条线的交点上 ----
        sp_v = any_rect("SPLIT-1", tap, 6.0)     # 视口 | 右栏（竖线）
        sp_h = any_rect("SPLIT-2", tap, 6.0)     # Hierarchy / Inspector（横线）
        cr = any_rect("SPLIT-CORNER-0", tap, 6.0)
        if not (sp_v and sp_h and cr):
            check("F1 读到角点锚点 SPLIT-CORNER-0", False,
                  "split1=%s split2=%s corner=%s" % (sp_v, sp_h, cr))
            return
        vx = (sp_v[0] + sp_v[2]) * 0.5
        hy = (sp_h[1] + sp_h[3]) * 0.5
        ccx = (cr[0] + cr[2]) * 0.5
        ccy = (cr[1] + cr[3]) * 0.5
        check("F1 角点正好在两条分隔线的交点上（差 < 3px）",
              abs(ccx - vx) < 3.0 and abs(ccy - hy) < 3.0,
              "corner=(%.1f,%.1f) 交点=(%.1f,%.1f)" % (ccx, ccy, vx, hy))

        # ---- F2 光标：角上是"加号" ----
        hover(hwnd, ccx, ccy)
        cur_corner = cursor_name()
        check("F2 悬停角落 → 光标是加号（SIZEALL）", cur_corner == "SIZEALL",
              "actual=%s" % cur_corner)

        # ---- F3 对照：线上还是双箭头 ----
        hover(hwnd, (sp_h[0] + sp_h[2]) * 0.5, hy)
        cur_line = cursor_name()
        check("F3 悬停线上（离角 60px 以外）→ 仍是 NS 双箭头（没被角落抢走）",
              cur_line == "SIZENS", "actual=%s" % cur_line)

        # ---- F4 对照：线上拖 = 调大小，不许产生角落日志 ----
        before_corner = len(corner_logs(tap))
        n0 = rect_count(tap)
        drag(hwnd, (sp_h[0] + sp_h[2]) * 0.5, hy,
             (sp_h[0] + sp_h[2]) * 0.5, hy + 80.0)
        r1 = wait_new_rect(tap, n0)
        sp_h2 = any_rect("SPLIT-2", tap, 4.0)
        new_y = (sp_h2[1] + sp_h2[3]) * 0.5 if sp_h2 else hy
        check("F4 线上往下拖 80px → 横线真的往下走了（在调大小）",
              r1 is not None and (new_y - hy) > 40.0,
              "横线 y %.0f → %.0f（新矩形 %s）" % (hy, new_y, r1 is not None))
        check("F4b 这一下没有触发角落手势（边上 ≠ 角上）",
              len(corner_logs(tap)) == before_corner,
              "角落日志 %d → %d" % (before_corner, len(corner_logs(tap))))

        # ---- F5 角上往下拖 = 合并 ----
        # 重读角点：F4 把横线挪了，角点跟着走（日志是去重的，会打新值）。
        cr2 = any_rect("SPLIT-CORNER-0", tap, 4.0) or cr
        ccx2 = (cr2[0] + cr2[2]) * 0.5
        ccy2 = (cr2[1] + cr2[3]) * 0.5
        n1 = rect_count(tap)
        drag(hwnd, ccx2, ccy2, ccx2, ccy2 + 110.0)
        wait_new_rect(tap, n1)
        logs = corner_logs(tap)
        line = logs[-1] if logs else ""
        m = re.search(r"LAYOUT-CORNER-MERGE from=(\S+) to=(\S+) size=(\d+)", line)
        got = (m.group(1), m.group(2), int(m.group(3))) if m else None
        check("F5 角上往下拖 → 两个上下相邻的面板并成标签组",
              got is not None and set(got[:2]) == {"hierarchy", "inspector"} and
              got[2] >= 2,
              "日志=%s" % (line.strip()[-70:] if line else "没有角落日志"))

        # ---- F6 合并后右栏只剩一格 ----
        after = latest_rect(tap)
        if after:
            _, hier1, insp1 = after
            empty = (hier1[2] < 1.0) or (insp1[2] < 1.0)
            big = max(hier1[2], insp1[2])
            check("F6 右栏收成一格（另一格宽度归零，整列被吃掉）",
                  empty and big > 250.0,
                  "hier.w=%.0f insp.w=%.0f" % (hier1[2], insp1[2]))
        else:
            check("F6 右栏收成一格", False, "没读到 LAYOUT-RECTS")
    finally:
        stop_editor(proc)

    # ---- F7 反方向：往左拖 = 与视口交换（视口不能当标签成员）----
    # ⚠ 必须换一次会话：F5 已经把右栏合成一格，横线没了，角点也就不存在
    #   （这是正确行为 —— 没有横线的地方没有"角"）。
    proc2, tap2 = start_editor()
    try:
        hwnd = find_window("Venn Editor", timeout=45)
        if hwnd:
            force_foreground(hwnd)
        if not hwnd:
            check("F7 第二个会话窗口出现", False, "没找到")
            return
        time.sleep(3.0)
        cr3 = any_rect("SPLIT-CORNER-0", tap2, 6.0)
        if not cr3:
            check("F7 读到角点锚点", False, "日志里没有 SPLIT-CORNER-0")
            return
        ccx3 = (cr3[0] + cr3[2]) * 0.5
        ccy3 = (cr3[1] + cr3[3]) * 0.5
        n2 = rect_count(tap2)
        drag(hwnd, ccx3, ccy3, ccx3 - 110.0, ccy3)
        wait_new_rect(tap2, n2)
        logs = corner_logs(tap2)
        line = logs[-1] if logs else ""
        # 视口不能当标签成员 → 落到"整格交换"。这条**不是**妥协就算了：
        # 它证明兜底分支还在工作（有人为了"让合并总能成功"去放开视口的话，
        # 这里会变成 MERGE，而视口被并进去后画面上就少了一块 3D 视图）。
        m = re.search(r"LAYOUT-CORNER-DRAG from=(\S+) to=(\S+)", line)
        got = (m.group(1), m.group(2)) if m else None
        check("F7 角上往左拖 → 视口格整格交换（视口不能当标签成员）",
              got is not None and "viewport" in got,
              "日志=%s" % (line.strip()[-70:] if line else "没有角落日志"))
    finally:
        stop_editor(proc2)


def main():
    kill_stale_editors()
    if not os.path.exists(EDITOR):
        print("找不到可执行文件:", EDITOR)
        return 2
    try:
        print("== A 自检（F 组拖拽状态机）==")
        group_selftest()
        print("== B 真拖(交换) + C 对照 ==")
        group_drag()
        print("== D 真拖(合并) + 标签切换 / 拆回 ==")
        group_tabs()
        print("== E 真拖(根分隔条)：左栏可缩 / 视口可变宽 ==")
        group_split()
        print("== F 角落抓手：角上=合并 / 边上=调大小 ==")
        group_corner()
    finally:
        kill_stale_editors()

    fails = [n for n, ok, _ in results if not ok]
    if fails:
        print("结果: FAIL: " + ", ".join(fails))
        return 1
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
