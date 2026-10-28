"""端到端验证：地形/植被编辑器的**参数与操作功能**升级（2026-10-20 第二轮）。

被验证的用户需求（原话）：
  "这个实现效果是否有些过于粗糙了，能否优化一下其中的详细参数和操作功能"

本轮新增的能力，逐条对应断言：
  A. 地形
     A- 新建地形坐在栅格地面上（2026-10-21 追加）
        A-1 谷底严格贴地 / A-2 默认起伏不再悬空 / A-3 LS-CREATE-SEAT
     A0 笔刷落点 == 画面隆起位置，且操作实时（2026-10-21 追加）
        A0a 离线：转置错误一定被抓到（判据 = 转置后整片的**最大高差**，
            不是"峰值被挪动多少" —— 后者是数据巧合，见断言处注释）
        A0b 刷新全部走内存直传（src=memory，零 src=disk）
        A0c 单次刷新耗时峰值 < 12 ms
        A0d/A0e/A0f 拖拽期间持续刷新、但不写盘（抬笔才写）
     A1 工具 8 个（Raise / Smooth / Flatten / Ramp / Noise / Erode /
        Paint layer / Carve）—— 按钮网格里 Ramp 排视觉第 4 格，而它的
        枚举值是 7，断言一律按视觉序号取锚点、按 `name=` 判成败
     A2 一笔结束产生一条撤销记录（LS-UNDO-PUSH），且高度图真的变了
     A3 Ctrl+Z 把磁盘高度图**精确**还原到刷之前（逐点差 = 0）
     A4 Ctrl+Shift+Z 重做回刷之后
     A5 Noise 工具真的在扰动表面（逐点绝对差之和变大）
     A6 Erode 工具真的搬运了物质
     A7 整块操作（Smooth all）可用且改了高度图
     A8 形状雕刻：按物体轮廓削地形，且**不**写出挖穿遮罩（拓扑完整）
     A9 Raise 收尾是锥形（山峰真的长高），不是一块平顶
        ⚠ 判据只看**世界高度**（top × maxHeight）：落盘时
          normalizeHeightRange 会把归一化值收回 [0,1] 并放大 maxHeight。
  B. 植被
     B1 种类从 3 种变 5 种（+Flower / Rock）
     B2 Rock 撒点成功
     B3 Flower 撒点成功
     B4 撒点日志带坡度/高度过滤统计（skippedSlope / skippedHeight）
  C. 全程零 Vulkan 验证层报错 / 零 ERROR

⚠ 两个吃过亏的地方（都写进注释了，别再踩）：
  1. **点击前必须校验坐标在客户区内**。曾经 `rect_of` 拿到一个被滚动条挤到
     窗口外的按钮矩形（y=1065，客户区才 614 高），点下去落到桌面上，
     **焦点从编辑器被抢走** —— 之后所有点击全部失效，表现为"后半段全 FAIL"，
     而根因在前半段一个看不见的按钮上。
  2. **笔刷落点必须避开浮动面板**。面板位置是 `DisplaySize * 0.30`，窗口一
     变小它就会压到视口左侧 —— 落点在面板里时画不出任何笔画，日志一条都没有。

用法：python tools/verify_world_tools.py
"""

import ctypes
import ctypes.wintypes as wintypes
import json
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from capture_window import capture_client, find_window  # noqa: E402
from verify_add_menu import LogTap, click, kill_stale_editors  # noqa: E402
from verify_asset_drag import client_to_screen  # noqa: E402
from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP  # noqa: E402

MOUSEEVENTF_WHEEL = 0x0800

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
ASSETS = os.path.join(ROOT, "build", "bin", "assets")
HEIGHT = os.path.join(ASSETS, "terrain", "landscape", "height.r32")

user32 = ctypes.windll.user32

RECT_ANY = r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)"
BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"

VK_Z = 0x5A
VK_CONTROL = 0x11
VK_SHIFT = 0x10
VK_OEM_PERIOD = 0xBE   # `.` —— 快捷键里用来加笔刷强度

fails = []


def check(ok, msg):
    print("    %-72s %s" % (msg, "PASS" if ok else "FAIL"))
    if not ok:
        fails.append(msg)
    return ok


def count(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def lines_with(tap, needle):
    with tap.lock:
        return [l for l in tap.lines if needle in l]


def mark(tap):
    """记下当前日志条数 —— 之后用 since() 只统计"这一段操作"产生的行。

    为什么不能全局 count：A0d 要证明"拖拽期间一次盘都不写"，而画一笔前后
    还有撤销/重做各自的一次落盘，混在一起数就永远对不上。
    """
    with tap.lock:
        return len(tap.lines)


def since(tap, idx, needle):
    with tap.lock:
        return [l for l in tap.lines[idx:] if needle in l]


def rect_of(tap, tag, timeout=6.0):
    deadline = time.time() + timeout
    pat = re.compile(re.escape(tag) + RECT_ANY)
    while time.time() < deadline:
        with tap.lock:
            for line in reversed(tap.lines):
                m = pat.search(line)
                if m:
                    return tuple(float(m.group(i)) for i in range(1, 5))
        time.sleep(0.12)
    return None


def center(r):
    return (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5


def client_size(hwnd):
    rc = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    return rc.right, rc.bottom


def focus_window(hwnd):
    """把编辑器抢到前台。

    ⚠ 这一条是整份脚本的地基。Windows 只会把鼠标消息送给**前台**窗口的
      命中区域；编辑器没有焦点时，所有 `click()` 都石沉大海 —— 而截图用的是
      PrintWindow（被遮住也能拍到），所以画面上一切正常，只有日志一条不动。
      表现极具误导性：连"点顶栏下拉"都失效，看起来像 UI 坏了。

    单纯 SetForegroundWindow 会被前台锁拒绝（返回后 GetForegroundWindow
    仍不是它）。先敲一下 ALT 解除锁，再 SetForegroundWindow；还不行就用
    老 API SwitchToThisWindow 兜底。
    """
    if user32.GetForegroundWindow() == hwnd:
        return True
    user32.keybd_event(0x12, 0, 0, 0)     # ALT down
    user32.keybd_event(0x12, 0, 2, 0)     # ALT up
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.35)
    if user32.GetForegroundWindow() != hwnd:
        user32.SwitchToThisWindow(hwnd, True)
        time.sleep(0.35)
    ok = user32.GetForegroundWindow() == hwnd
    if not ok:
        print("      ! 无法把编辑器切到前台，后续点击可能全部失效")
    return ok


def click_rect(hwnd, r, name="widget"):
    """点一个矩形中心 —— 但**先校验它在客户区内**（见文件头第 1 条）。"""
    if not r:
        print("      ! %s 没有矩形锚点，跳过" % name)
        return False
    cx, cy = center(r)
    cw, ch = client_size(hwnd)
    if not (2.0 <= cx <= cw - 2.0 and 2.0 <= cy <= ch - 2.0):
        print("      ! %s 在客户区外 (%.0f,%.0f) / 客户区 %dx%d，跳过点击"
              % (name, cx, cy, cw, ch))
        return False
    focus_window(hwnd)
    click(hwnd, cx, cy)
    time.sleep(0.45)
    return True


def wait_line(tap, needle, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if count(tap, needle) > 0:
            return True
        time.sleep(0.15)
    return False


def brush_point(tap):
    """视口里一个"既在地形上、又没被浮动面板盖住"的落点。"""
    vp = rect_of(tap, "VP-RECT")
    if not vp:
        return None
    pnl = rect_of(tap, "LS-PANEL", 1.0) or rect_of(tap, "FO-PANEL", 1.0)
    bx = vp[0] + 60.0
    if pnl and (pnl[0] - vp[0]) > 90.0:
        bx = min(bx, pnl[0] - 80.0)
    bx = max(bx, vp[0] + 24.0)
    by = vp[1] + (vp[3] - vp[1]) * 0.60
    return bx, by


def drag(hwnd, src, dst, steps=14):
    focus_window(hwnd)
    user32.SetCursorPos(*client_to_screen(hwnd, src[0], src[1]))
    time.sleep(0.3)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.2)
    for k in range(1, steps + 1):
        x = src[0] + (dst[0] - src[0]) * k / steps
        y = src[1] + (dst[1] - src[1]) * k / steps
        user32.SetCursorPos(*client_to_screen(hwnd, x, y))
        time.sleep(0.045)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.9)


def reveal(hwnd, tap, tag, panel_tag="LS-PANEL", tries=10):
    """把浮层面板里的目标滚到**完全可见**，返回可点的中心坐标。

    ⚠ 这一条是 2026-10-21 加的，理由值得记住：锚点报的是 ImGui item 的
      **未裁剪**矩形。实测 `LS-TOOL-7 = (207,831)-(395,859)`，而面板底边是
      y=842 —— 按钮下沿有 17 px 在面板外面，中心 (301,845) 落在面板**下方**，
      点下去打到底部 dock 上，把视口标签从 Viewport 切成了 Console。
      之后所有视口操作（笔刷、撒点）全部落空，表现成"后半段整片 FAIL"，
      而根因只是最后一行按钮被裁掉了。

    返回 None 表示滚不到 —— 调用方据此跳过，而不是瞎点。
    """
    r = rect_of(tap, tag, 1.0)
    if not r:
        return None
    pnl = rect_of(tap, panel_tag, 1.0)
    if not pnl:
        return None
    for _ in range(tries):
        # 完整落在面板内（留 4 px 余量）才认为可点
        if r[1] >= pnl[1] + 4.0 and r[3] <= pnl[3] - 4.0:
            return ((r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5)
        # 目标在下方 → 向下滚（内容上移）
        down = r[3] > pnl[3] - 4.0
        cx = (pnl[0] + pnl[2]) * 0.5
        cy = (pnl[1] + pnl[3]) * 0.5
        focus_window(hwnd)
        user32.SetCursorPos(*client_to_screen(hwnd, cx, cy))
        time.sleep(0.15)
        user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, -120 if down else 120, 0)
        time.sleep(0.4)
        r = rect_of(tap, tag, 1.0) or r
        pnl = rect_of(tap, panel_tag, 1.0) or pnl
    if r[1] >= pnl[1] + 4.0 and r[3] <= pnl[3] - 4.0:
        return ((r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5)
    print("      ! %s 滚不到面板可见区内 (item=%s panel=%s)"
          % (tag, ["%.0f" % v for v in r], ["%.0f" % v for v in pnl]))
    return None


def hold_key(hwnd, vk, seconds=1.5):
    """按住一个键 —— 触发 ImGui 的按键重复（IsKeyPressed(key, repeat=true)）。

    为什么不用连发 N 次 down/up：ImGui 的 repeat 与 GLFW 的自动重复绑定，
    手动连发会被合并成一次按下。按住等系统自动重复才可靠。
    """
    focus_window(hwnd)
    user32.keybd_event(vk, 0, 0, 0)
    time.sleep(seconds)
    user32.keybd_event(vk, 0, 2, 0)
    time.sleep(0.3)


def read_heights(path):
    """height.r32 → float 列表（列主序）。文件不存在返回 None。"""
    if not os.path.exists(path):
        return None
    raw = open(path, "rb").read()
    n = len(raw) // 4
    return list(struct.unpack("<%df" % n, raw[: n * 4]))


def read_desc(path):
    """terrain.vterrain → dict。"""
    if not os.path.exists(path):
        return None
    try:
        return json.loads(open(path, "r", encoding="utf-8").read())
    except Exception:                       # noqa: BLE001
        return None


def transpose_probe(h, cols, rows, mpp):
    """量化"不转置上传"会把地形画成什么样。

    存储是**列主序**（`heights[col*rows + row]`，physics::Heightfield 约定），
    而 Vulkan 纹理按**行主序**解释（`texel(x,y) = data[y*w + x]`），
    terrain.vert 又用 `uv = ((col+0.5)/cols, (row+0.5)/rows)` 采样。
    于是原样上传 = 采样点 (col,row) 读到 `h[row*cols + col]` —— 整片地形
    绕对角线镜像。

    返回 (峰值CPU世界坐标, 峰值会被画到的世界坐标, 差异比例, 最大高差)。
    """
    n = cols * rows
    peak = max(range(n), key=lambda i: h[i])
    pc, pr = divmod(peak, rows)          # 列主序：i = col*rows + row
    cx, cy = (cols - 1) * 0.5, (rows - 1) * 0.5
    cpu_w = ((pc - cx) * mpp, (pr - cy) * mpp)
    # 行主序解释：同一个下标 i 变成 row*cols + col
    nc, nr = peak % cols, peak // cols
    naive_w = ((nc - cx) * mpp, (nr - cy) * mpp)
    mism = 0
    worst = 0.0
    for c in range(cols):
        for r in range(rows):
            d = abs(h[c * rows + r] - h[r * cols + c])
            if d > 1e-6:
                mism += 1
            if d > worst:
                worst = d
    return cpu_w, naive_w, mism / float(n), worst


def parse_float(line, key):
    m = re.search(r"%s=(-?[\d.]+)" % re.escape(key), line)
    return float(m.group(1)) if m else None


def absdiff(a, b):
    """逐点绝对差之和 —— 判断"表面被扰动过"比 sum 更可靠：
    噪声抬一半压一半，sum 可能几乎不变，但 absdiff 一定变大。"""
    if not a or not b or len(a) != len(b):
        return 0.0
    return sum(abs(x - y) for x, y in zip(a, b))


def key_combo(hwnd, vk, ctrl=True, shift=False):
    """发组合键。ImGui 的 io.KeyCtrl 来自 GLFW 的修饰键状态，
    所以必须真的按下/释放 Ctrl，不能只发 Z。"""
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.2)
    if ctrl:
        user32.keybd_event(VK_CONTROL, 0, 0, 0)
    if shift:
        user32.keybd_event(VK_SHIFT, 0, 0, 0)
    time.sleep(0.06)
    user32.keybd_event(vk, 0, 0, 0)
    time.sleep(0.08)
    user32.keybd_event(vk, 0, 2, 0)
    if shift:
        user32.keybd_event(VK_SHIFT, 0, 2, 0)
    if ctrl:
        user32.keybd_event(VK_CONTROL, 0, 2, 0)
    time.sleep(0.8)


def shoot(hwnd, name):
    try:
        img, _ = capture_client(hwnd)
    except Exception as e:                      # noqa: BLE001
        print("      ! 截图失败(%s): %s" % (name, e))
        return
    if img is None or getattr(img, "size", 0) == 0:
        print("      ! 截图为空，跳过: %s" % name)
        return
    try:
        img.save(os.path.join(OUT_DIR, name))
    except Exception as e:                      # noqa: BLE001
        print("      ! 截图保存失败(%s): %s" % (name, e))


def main():
    if not os.path.exists(EXE):
        print("找不到可执行文件，请先构建：", EXE)
        return 1

    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)
    terrain_dir = os.path.join(ASSETS, "terrain", "landscape")
    for f in ("terrain.vterrain", "height.r32", "splat.rgba8"):
        p = os.path.join(terrain_dir, f)
        if os.path.exists(p):
            os.remove(p)

    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITMODE": "landscape",
        "MYVK_LANDSCAPE_AUTOCREATE": "1",
    })
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    try:
        hwnd = find_window("Venn Editor", timeout=60)
        if not hwnd:
            print("找不到编辑器窗口")
            return 1
        time.sleep(2.0)
        focus_window(hwnd)
        if not wait_line(tap, "LS-TERRAIN path=terrain/landscape", 12.0):
            print("地形没有自动建出来，后续断言没有意义")
            return 1
        time.sleep(0.8)

        # ================================================ A- 新建地形坐落
        #
        # 用户原话："每次通过地编生成的地形，为什么运行游戏时发现地形是在原
        # 栅格地面的正上方的，还把原本的游戏初始化场景给覆盖住了。"
        #
        # 根因是默认高度的**基线**是 0.46（maxHeight=60 → 整片面挂在 +27.6 m），
        # 而初始场景里的 Ground / Player / Pickup 都坐在 z≈0..1。现在默认
        # 起伏被归一化到 [0, 0.12]，谷底严格贴地。
        #
        # ⚠ 必须在这里读 —— 后面几组会刷地形，读数就不是"新建后原样"了。
        print("\n[A-] 新建地形是否坐在栅格地面上")
        hd = read_desc(os.path.join(terrain_dir, "terrain.vterrain"))
        h0 = read_heights(HEIGHT)
        if hd and h0:
            mx, mn = max(h0), min(h0)
            max_m = mx * hd["maxHeight"]
            print("      · 归一化 %.4f..%.4f → 实际 %.2f..%.2f m（maxHeight=%.0f）"
                  % (mn, mx, mn * hd["maxHeight"], max_m, hd["maxHeight"]))
            check(mn <= 0.002,
                  "A-1 谷底贴地（min=%.4f ≈ 0）" % mn)
            check(mx <= 0.25,
                  "A-2 默认起伏不再悬空（峰值 %.2f m <= 0.25×maxHeight=%.1f m）"
                  % (max_m, 0.25 * hd["maxHeight"]))
        else:
            check(False, "A-1/A-2 读不到新建地形的高度图")
        check(count(tap, "LS-CREATE-SEAT") > 0,
              "A-3 地形实体坐在地平面上（LS-CREATE-SEAT）")

        bp = brush_point(tap)
        print("    落点 = (%.0f, %.0f)" % (bp[0], bp[1]))
        bx, by = bp

        # ================================================== A 地形
        print("\n[A] 地形：工具 / 撤销 / 重做 / 噪声 / 侵蚀 / 整块操作")

        # A1：工具按钮网格（8 个）
        # ⚠ 断言锚点用 `LS-TOOL-<视觉序号>`，而日志里打的是**枚举值**。
        #   两者顺序不一样（网格把 Ramp 排在视觉第 4 格，枚举里它是 7）。
        #   早先这里按"序号 == 枚举值"写死了，加了 Ramp 之后 A5/A6 就
        #   一直 FAIL —— 断言的其实是"第 4 格是 Noise"，实际那格是 Ramp。
        tool_rects = [rect_of(tap, "LS-TOOL-%d" % i, 3.0) for i in range(8)]
        check(all(tool_rects), "A1 八个工具按钮齐全（缺: %s）"
              % ([i for i, r in enumerate(tool_rects) if not r] or "无"))

        # A1b：滚轮调半径（`[` `]` 是 OEM 键，某些键盘布局下收不到，
        #      所以滚轮才是主入口 —— 这条断言保证它真的能调）
        #
        # 这里必须允许重试：滚轮分支的前置条件是 `onTerrain`（射线打到
        # 地形上）。刚自动建完地形的那一两帧里相机可能还在落位，射线打空
        # 时滚轮被视口拿去**推拉相机**，一条 LS-BRUSH 都不会有 —— 于是
        # "功能好的、断言先判了死刑"。实测手工探测一次就出 5 条。
        focus_window(hwnd)
        user32.SetCursorPos(*client_to_screen(hwnd, bx, by))
        time.sleep(0.45)
        for _attempt in range(3):
            for _ in range(3):
                user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, 120, 0)
                time.sleep(0.2)
            time.sleep(0.45)
            if count(tap, "LS-BRUSH radius=") > 0:
                break
        check(count(tap, "LS-BRUSH radius=") > 0,
              "A1b 滚轮调半径生效（LS-BRUSH radius=）")
        if count(tap, "LS-BRUSH strength=") > 0:
            print("      ! 收到的却是 strength= —— Shift 被当作按下")

        # A2：一笔 → 撤销记录
        h0 = read_heights(HEIGHT)
        drag(hwnd, (bx, by), (bx + 46.0, by - 30.0))
        time.sleep(0.8)
        h1 = read_heights(HEIGHT)
        check(count(tap, "LS-UNDO-PUSH") > 0, "A2a 一笔产生撤销记录（LS-UNDO-PUSH）")
        check(h0 is not None and h1 is not None and absdiff(h0, h1) > 1.0e-4,
              "A2b 高度图真的变了 (absdiff=%.4f)" % absdiff(h0, h1))

        # A3：Ctrl+Z 精确还原
        key_combo(hwnd, VK_Z)
        h2 = read_heights(HEIGHT)
        check(count(tap, "LS-UNDO depth=0") > 0, "A3a 撤销栈清空（LS-UNDO depth=0）")
        check(h2 is not None and absdiff(h0, h2) < 1.0e-5,
              "A3b 磁盘高度图精确还原 (absdiff=%.8f)" % absdiff(h0, h2))

        # A4：Ctrl+Shift+Z 重做
        key_combo(hwnd, VK_Z, ctrl=True, shift=True)
        h3 = read_heights(HEIGHT)
        check(count(tap, "LS-REDO depth=") > 0, "A4a 重做日志（LS-REDO）")
        check(h3 is not None and absdiff(h1, h3) < 1.0e-5,
              "A4b 重做回到刷之后的状态 (absdiff=%.8f)" % absdiff(h1, h3))

        # ============================================ A0 笔刷落点 == 生效位置
        #
        # 用户原话："鼠标操作的地区和实际生效的位置是有不同的"
        #
        # 根因不是射线、不是矩阵，是**上传时的存储序**：高度图列主序，
        # 纹理行主序，原样上传 → 整片地形绕对角线镜像。所以这里分两层验：
        #   ① 离线：这份地形资产本身足够非对称，转置错误一定会被抓到
        #      （不够非对称的话，就算代码错了画面也"看起来正常"）；
        #   ② 在线：拖一笔之后，刷新必须走内存直传、且耗时是毫秒级。
        print("\n[A0] 笔刷落点与实时性")
        desc = read_desc(os.path.join(terrain_dir, "terrain.vterrain"))
        hh = read_heights(HEIGHT)
        if desc and hh:
            cpu_w, naive_w, ratio, worst = transpose_probe(
                hh, desc["cols"], desc["rows"], desc["metersPerPixel"])
            gap = ((cpu_w[0] - naive_w[0]) ** 2 +
                   (cpu_w[1] - naive_w[1]) ** 2) ** 0.5
            worst_m = worst * desc["maxHeight"]
            print("      · 地形 %dx%d  峰值 CPU=(%.1f,%.1f) 不转置会画到 "
                  "(%.1f,%.1f)  峰值偏差 %.0f m  差异采样 %.1f%%  "
                  "最大高差 %.1f m"
                  % (desc["cols"], desc["rows"], cpu_w[0], cpu_w[1],
                     naive_w[0], naive_w[1], gap, ratio * 100.0, worst_m))
            # ⚠ 判据别用"峰值会被挪动 > 20 m"：那依赖"默认地形的最高点离
            #   对角线够远"，是**数据巧合** —— 默认噪声一改（2026-10-21 起的
            #   [0,0.12] 归一化）峰值就靠近对角线了，实测只偏 17 m，于是
            #   "转置 bug 会被抓到"这个真结论被一个假 FAIL 盖住。
            #   改成直接量"转置之后整片地形差多少米"（最大高差），它与峰值
            #   位置无关，正好就是转置 bug 的可见后果。
            check(ratio > 0.5 and worst_m > 2.0,
                  "A0a 转置错误必被抓到 (差异 %.0f%%, 最大高差 %.1f m)"
                  % (ratio * 100.0, worst_m))
        else:
            check(False, "A0a 读不到地形资产，无法做离线断言")

        # 在线证据：TERRAIN-REFRESH 的 src / lastMs / maxMs
        refr = lines_with(tap, "TERRAIN-REFRESH")
        mem = [l for l in refr if "src=memory" in l]
        dsk = [l for l in refr if "src=disk" in l]
        check(len(refr) > 0 and len(mem) == len(refr),
              "A0b 刷新全部走内存直传 (memory=%d disk=%d)"
              % (len(mem), len(dsk)))
        if refr:
            last = parse_float(refr[-1], "lastMs") or 0.0
            peak = parse_float(refr[-1], "maxMs") or 0.0
            print("      · 最近一次刷新 lastMs=%.2f  maxMs=%.2f" % (last, peak))
            check(peak < 12.0, "A0c 单次刷新耗时峰值 %.2f ms < 12 ms" % peak)

        # 拖拽期间不落盘：单独画一笔长笔划，只统计这一段产生的日志。
        # 一笔 30 步 ≈ 1.5 s → 按 80 ms 节流应有十几次刷新；
        # 修复前每次刷新前都先 saveAssets（写 264 KB 再读回），所以
        # LS-SAVE 会跟 TERRAIN-REFRESH 一样多。
        m0 = mark(tap)
        drag(hwnd, (bx - 40.0, by + 24.0), (bx + 30.0, by - 40.0), steps=30)
        time.sleep(0.9)
        seg_refr = since(tap, m0, "TERRAIN-REFRESH")
        seg_save = since(tap, m0, "LS-SAVE ms=")
        print("      · 一笔 30 步：刷新 %d 次，落盘 %d 次"
              % (len(seg_refr), len(seg_save)))
        check(len(seg_refr) >= 3,
              "A0d 拖拽期间持续刷新 (%d 次)" % len(seg_refr))
        check(len(seg_save) <= 1,
              "A0e 拖拽期间不写盘，抬笔才写 (落盘 %d 次)" % len(seg_save))
        segms = [parse_float(l, "lastMs") or 0.0 for l in seg_refr]
        if segms:
            check(max(segms) < 12.0,
                  "A0f 期间单次刷新最慢 %.2f ms < 12 ms" % max(segms))



        # A5：噪声笔刷（视觉第 5 格 = LS-TOOL-4；枚举值是 3，别混用）
        click_rect(hwnd, rect_of(tap, "LS-TOOL-4", 3.0), "Noise 按钮")
        check(count(tap, "name=Noise") > 0, "A5a 切到 Noise 工具")
        hn0 = read_heights(HEIGHT)
        drag(hwnd, (bx, by), (bx + 30.0, by - 20.0))
        hn1 = read_heights(HEIGHT)
        d_noise = absdiff(hn0, hn1)
        check(d_noise > 1.0e-3, "A5b 噪声扰动生效 (absdiff=%.5f)" % d_noise)

        # A6：侵蚀工具（视觉第 6 格 = LS-TOOL-5；枚举值是 4）
        click_rect(hwnd, rect_of(tap, "LS-TOOL-5", 3.0), "Erode 按钮")
        check(count(tap, "name=Erode") > 0, "A6a 切到 Erode 工具")
        he0 = read_heights(HEIGHT)
        drag(hwnd, (bx, by), (bx + 26.0, by - 18.0))
        he1 = read_heights(HEIGHT)
        d_erode = absdiff(he0, he1)
        check(d_erode > 1.0e-4, "A6b 侵蚀搬运了物质 (absdiff=%.5f)" % d_erode)

        # A7：整块平滑
        clicked = click_rect(hwnd, rect_of(tap, "LS-WHOLE-SMOOTH", 3.0),
                             "Smooth all 按钮")
        time.sleep(0.8)
        check(clicked and count(tap, "LS-WHOLE action=smooth") > 0,
              "A7a Smooth all 执行（LS-WHOLE action=smooth）")
        hs = read_heights(HEIGHT)
        check(absdiff(he1, hs) > 1.0e-5, "A7b 整块平滑改到了高度图")


        # ================================================== B 植被
        print("\n[B] 植被：5 种类型 / 色差 / 过滤")
        # 2026-10-21：顶栏下拉删掉了，切模式改点左栏模式栏那一行。
        fo = rect_of(tap, "MODE-RAIL-ITEM foliage", 4.0)
        clicked = click_rect(hwnd, fo, "左栏 Foliage 行")
        time.sleep(1.6)
        check(clicked and count(tap, "EDITMODE foliage") > 0,
              "B0 进入 Foliage 模式")

        kinds = [rect_of(tap, "FO-KIND-%d" % i, 3.0) for i in range(5)]
        check(all(kinds), "B1 五种植物按钮齐全（缺: %s）"
              % ([i for i, r in enumerate(kinds) if not r] or "无"))

        # B2：石头
        click_rect(hwnd, kinds[4], "Rock 按钮")
        check(count(tap, "FOLIAGE-KIND rock") > 0, "B2a 切到 Rock")
        bp2 = brush_point(tap)
        focus_window(hwnd)
        click(hwnd, bp2[0], bp2[1])
        time.sleep(1.2)
        check(count(tap, "FOLIAGE-SPAWN kind=rock") > 0, "B2b 石头撒点成功")
        rock_lines = lines_with(tap, "FOLIAGE-COUNT")
        rock_total = 0
        if rock_lines:
            m = re.findall(r"rock=(\d+)", rock_lines[-1])
            rock_total = int(m[-1]) if m else 0
        check(rock_total > 0, "B2c 石头计数 > 0 (rock=%d)" % rock_total)

        # B3：花
        click_rect(hwnd, kinds[3], "Flower 按钮")
        check(count(tap, "FOLIAGE-KIND flower") > 0, "B3a 切到 Flower")
        focus_window(hwnd)
        click(hwnd, bp2[0] + 26.0, bp2[1] - 12.0)
        time.sleep(1.2)
        check(count(tap, "FOLIAGE-SPAWN kind=flower") > 0, "B3b 花撒点成功")

        # B4：过滤统计进了日志（0 也算 —— 关键字段存在）
        spawn = lines_with(tap, "FOLIAGE-SPAWN kind=rock")
        has_fields = bool(spawn) and "skippedSlope=" in spawn[-1] and \
            "skippedHeight=" in spawn[-1]
        check(has_fields, "B4 撒点日志带坡度/高度过滤统计")
        if spawn:
            print("      ·", spawn[-1].strip()[-130:])

        cnt = lines_with(tap, "FOLIAGE-COUNT total=")
        total = 0
        if cnt:
            m = re.search(r"total=(\d+)", cnt[-1])
            total = int(m.group(1)) if m else 0
        check(0 < total <= 6000, "B5 实例总数正常 (total=%d)" % total)

        # ---------------------------------------------------- B6~B10 面板重做
        #
        # 2026-10-28：Foliage 面板按 UE5 重做（工具行 / 类型列表带眼睛 /
        # 每类型参数 / Reapply）。这一段的判据全部落在**新控件真的接通了**
        # 上：点了工具 → 日志变→ 视口行为跟着变，而不是"按钮画出来了"。
        #
        # ⚠ 类型列表和工具行在面板顶部（默认可见）；Reapply 在面板下方，
        #   要先 `reveal` 滚进来 —— 被裁掉的控件"有矩形却点不到"。
        tools = [rect_of(tap, "FO-TOOL-%d" % i, 3.0) for i in range(4)]
        check(all(tools), "B6 工具行四格齐全（Paint/Erase/Single/Select，缺 %s）"
              % ([i for i, r in enumerate(tools) if not r] or "无"))

        # B7 Single：点一株（不是撒一片）
        click_rect(hwnd, tools[2], "Single 工具")
        check(count(tap, "FOLIAGE-TOOL single") > 0, "B7a 切到 Single 工具")
        bp3 = brush_point(tap)
        focus_window(hwnd)
        click(hwnd, bp3[0], bp3[1])
        time.sleep(0.9)
        single = lines_with(tap, "FOLIAGE-SINGLE")
        ok_single = bool(single) and "placed=1" in single[-1]
        check(ok_single, "B7b 一次单击只放 1 株（FOLIAGE-SINGLE placed=1）")
        if single:
            print("      ·", single[-1].strip()[-120:])

        # B8 Erase：真的删掉了
        click_rect(hwnd, tools[1], "Erase 工具")
        check(count(tap, "FOLIAGE-TOOL erase") > 0, "B8a 切到 Erase 工具")
        focus_window(hwnd)
        drag(hwnd, (bp3[0] - 20.0, bp3[1]), (bp3[0] + 20.0, bp3[1]), steps=10)
        time.sleep(0.9)
        erased = [l for l in lines_with(tap, "FOLIAGE-ERASE")
                  if "removed=0" not in l]
        check(bool(erased), "B8b 擦除真的删了实例（FOLIAGE-ERASE removed>0）")
        if erased:
            print("      ·", erased[-1].strip()[-120:])

        # B9 眼睛：可见性开关（不是删除）
        vis = rect_of(tap, "FO-VIS-3", 3.0)
        # ⚠ `before_total` 必须**当场重读**，不能沿用 B5 那个值：
        #   B8 的擦除把株数改了，而 `recount(false)` 是**不打日志**的
        #   （只在条数变化时打）。沿用旧值会把"擦除让 18 变 5"算到"隐藏"
        #   头上 —— 上一版就是这么假红的（18 → 5）。
        cnt_prev = lines_with(tap, "FOLIAGE-COUNT total=")
        before_total = total
        if cnt_prev:
            m = re.search(r"total=(\d+)", cnt_prev[-1])
            before_total = int(m.group(1)) if m else total
        if vis:
            click_rect(hwnd, vis, "Flower 眼睛")
            check(count(tap, "FOLIAGE-VIS flower=0") > 0,
                  "B9a 点眼睛 → FOLIAGE-VIS flower=0")
            cnt2 = lines_with(tap, "FOLIAGE-COUNT total=")
            t2 = before_total
            if cnt2:
                m = re.search(r"total=(\d+)", cnt2[-1])
                t2 = int(m.group(1)) if m else t2
            check(t2 == before_total,
                  "B9b 只是隐藏，实例数没变（%d → %d）" % (before_total, t2))
            click_rect(hwnd, vis, "Flower 眼睛（点回来）")
            check(count(tap, "FOLIAGE-VIS flower=1") > 0,
                  "B9c 再点一次恢复显示 → FOLIAGE-VIS flower=1")
        else:
            check(False, "B9 找到 FO-VIS-3 锚点")

        # B10 Reapply：把参数重新套到已有实例上（UE 的招牌按钮）
        pt = reveal(hwnd, tap, "FO-REAPPLY", panel_tag="FO-PANEL", tries=14)
        if pt:
            focus_window(hwnd)
            click(hwnd, pt[0], pt[1])
            time.sleep(1.0)
            check(count(tap, "FOLIAGE-REAPPLY kind=") > 0,
                  "B10 Reapply 执行并写了日志（FOLIAGE-REAPPLY kind=…）")
            rp = lines_with(tap, "FOLIAGE-REAPPLY kind=")
            if rp:
                print("      ·", rp[-1].strip()[-120:])
        else:
            check(False, "B10 FO-REAPPLY 滚不进可视区（面板太窄/滚轮没到）")

        shoot(hwnd, "_wt_foliage_kinds.png")
        # ============================================== A8 形状雕刻（原"挖洞"）
        #
        # 用户原话："挖洞不是把原地面中间剖开一个缺口，不能破坏拓扑；最好是
        # 选一个物体、在地面里拖动它贯穿表面，用这个物体的轮廓给地形形变。"
        #
        # 所以断言分两头：① 真的在按形状削地形（LS-CARVE + 高度下降）；
        # ② **没有**写出挖穿遮罩（holes.r8 不存在或全 255）—— 拓扑完整。
        print("\n[A8] 形状雕刻：以物体轮廓压出凹腔，不破拓扑")
        # ⚠ 这一组跑在植被组之后 —— 那时**地形面板是关着的**，LS-TOOL-*
        #   锚点全是上一段留下的旧坐标，照着旧坐标点会点进植被面板里
        #   （表现为"切不到 Carve、拖动也不改地形"）。所以先切回地形模式。
        ls_rail = rect_of(tap, "MODE-RAIL-ITEM landscape", 4.0)
        m_ls = mark(tap)
        click_rect(hwnd, ls_rail, "左栏 Landscape 行")
        time.sleep(1.2)
        check(len(since(tap, m_ls, "EDITMODE landscape")) > 0,
              "A8-0 切回 Landscape 模式")
        # 雕刻点就取 A2 抬过的那个位置 —— 那里地表一定高于 0。默认地形的
        # 谷底高度是 0，"往下压"压到 0 就停住了，在谷底上压等于什么都没发生。
        scx, scy = bx, by

        # 先在雕刻点堆一个土包再压。为什么必须这么绕：默认地形的谷底高度
        # 是 **0**（见 A-1），而"往下压"能压到的最低就是 0 —— 在谷底上压
        # 等于什么都没发生，断言会误报成"功能没生效"。
        pos_r = reveal(hwnd, tap, "LS-TOOL-0")
        if pos_r:
            focus_window(hwnd)
            click(hwnd, pos_r[0], pos_r[1])
            time.sleep(0.4)
        hold_key(hwnd, VK_OEM_PERIOD, 0.5)      # 抬升速率 ∝ strength，先加一点
        for _ in range(3):
            drag(hwnd, (scx, scy), (scx + 3.0, scy + 3.0), steps=5)
        mound = read_heights(HEIGHT) or [0.0]
        print("      · 先在雕刻点堆了个土包：顶点 %.3f 归一化（=%.1f m）"
              % (max(mound), max(mound) * 60.0))
        check(max(mound) > 0.16,
              "A8-1 土包已成型（顶点 %.3f > 默认峰值 0.12）" % max(mound))

        pos = reveal(hwnd, tap, "LS-TOOL-7")
        if pos:
            focus_window(hwnd)
            click(hwnd, pos[0], pos[1])
            time.sleep(0.5)
        check(bool(pos) and count(tap, "name=Carve") > 0,
              "A8a 切到 Carve 工具（点 %s）"
              % (["%.0f" % v for v in pos] if pos else "无锚点"))

        before = read_heights(HEIGHT)
        drag(hwnd, (scx, scy), (scx + 70.0, scy), steps=16)
        after = read_heights(HEIGHT)
        check(count(tap, "LS-CARVE shape=") > 0, "A8b 走形状压印（LS-CARVE）")
        check(count(tap, "mode=carve") > 0, "A8c 默认下压（mode=carve）")
        drop = sum(1 for a, b in zip(before, after) if b < a - 1.0e-4)
        car = lines_with(tap, "LS-CARVE shape=")
        if car:
            print("      · %s" % car[-1].strip()[-120:])
        print("      · 压前顶点 %.3f / 压后顶点 %.3f / absdiff %.5f"
              % (max(before or [0.0]), max(after or [0.0]),
                 absdiff(before, after)))
        check(drop > 200, "A8d 地表被压下去（%d 个采样点变低）" % drop)

        hole_path = os.path.join(terrain_dir, "holes.r8")
        intact = True
        if os.path.exists(hole_path):
            raw_h = open(hole_path, "rb").read()
            intact = bool(raw_h) and all(v == 255 for v in raw_h)
        check(intact, "A8e 没有挖穿遮罩（地面拓扑完整）")

        m0 = mark(tap)
        focus_window(hwnd)
        user32.keybd_event(VK_SHIFT, 0, 0, 0)
        time.sleep(0.1)
        drag(hwnd, (scx, scy - 70.0), (scx, scy - 70.0), steps=3)
        user32.keybd_event(VK_SHIFT, 0, 2, 0)
        time.sleep(0.3)
        check(len(since(tap, m0, "mode=mound")) > 0,
              "A8f Shift 反向堆出同轮廓的隆起")

        # ============================================== A9 Raise 不再收尾成平顶
        #
        # 用户原话："抬升最后收尾的时候总会成一个小平面 —— 想刷山峰，结果
        # 变成了类似火山的结构。"
        #
        # 根因是 `h = clamp(h, 0, 1)`：笔心先撞顶、外圈继续涨 → 必然平顶。
        # 现在允许超过 1.0（kMaxNorm=8，且越接近上限越慢），所以：
        #   ① 顶点能突破 1.0 —— ⚠ 这是**内存里**的事实；落盘时会被
        #      normalizeHeightRange 收回 [0,1] 并放大 maxHeight（见下面的
        #      长注释），所以这条要用**世界高度**来断言；
        #   ② 与顶点同高的采样点很少（锥形收尾），而不是一大片平台。
        print("\n[A9] Raise 收尾是锥形，不是平台")
        # 面板刚才被 A8 滚下去了，Raise 在第一行 —— 按同一套逻辑滚回来。
        pos0 = reveal(hwnd, tap, "LS-TOOL-0")
        if pos0:
            focus_window(hwnd)
            click(hwnd, pos0[0], pos0[1])
            time.sleep(0.5)
        check(bool(pos0) and count(tap, "name=Raise") > 0, "A9a 切回 Raise")

        # 抬升速率 ∝ strength，而默认 0.35 太慢。用 `.` 把强度顶一点再刷，
        # 但**不能顶到上限** —— 顶满会直接撞上 kMaxNorm 的渐近线，测到的
        # 就是"饱和态"，那本来就应该是一块平台，断言变得没有意义。
        hold_key(hwnd, VK_OEM_PERIOD, 0.4)
        st = [parse_float(l, "strength") for l in lines_with(tap, "LS-BRUSH strength=")]
        st = [v for v in st if v]
        if st:
            print("      · 强度提到 %.2f" % st[-1])

        # ⚠ 这里**不能**写死"刷 10 笔"，理由（2026-10-21 实测踩到）：
        #   抬升量 ∝ dt，而引擎把 dt 钳在 0.05 s；机器慢到 <20 fps 时
        #   真实间隔 0.1 s 只算 0.05 s，累积速度直接腰斩。
        #   于是同一份 Raise 代码，上一跑 top=1.56、这一跑 top=0.63，
        #   看起来像回归，其实是断言在测机器速度。
        #   改成"刷到达标就停"：结论与帧率无关，只影响刷几次。
        #   次数上限 30：满速下最多涨 30*0.575s*0.18/s ≈ 3.1，
        #   仍低于 A9b2 的饱和阈值 4.0，不会退化成测平台。
        #
        # ⚠⚠ 2026-10-22 起判据只能看**世界高度**，不能看磁盘上的归一化值：
        #   `saveAssets` 落盘前会先 `terrain::normalizeHeightRange` 把高度
        #   收敛回 [0,1]、并把 `maxHeight` 同乘放大（世界高度不变）——
        #   那是 .r32 的资产契约。结果就是磁盘上的 `top` **恒为 1.000**，
        #   旧的 `top > 1.02` 再也不可能成立（实测 1.000 = 数据契约，
        #   不是功能回归）。用户看到的"山长高了没有"始终是
        #   `top * maxHeight` 这个米数，所以判据、break 条件、上限全部换成米。
        d0 = read_desc(os.path.join(terrain_dir, "terrain.vterrain"))
        m0 = d0["maxHeight"] if d0 else 60.0
        strokes = 0
        top = 0.0
        world = 0.0
        hr = []
        for _ in range(30):
            drag(hwnd, (bx - 30.0, by - 30.0), (bx - 27.0, by - 27.0), steps=5)
            strokes += 1
            hr = read_heights(HEIGHT)
            dn = read_desc(os.path.join(terrain_dir, "terrain.vterrain"))
            top = max(hr) if hr else 0.0
            world = top * (dn["maxHeight"] if dn else m0)
            if world > 1.10 * m0:
                break
        flat = sum(1 for v in hr if v > top - 0.02) if hr else 0
        print("      · 刷了 %d 笔  顶点 %.3f 归一化 · 世界 %.1f m"
              "（起始 maxHeight %.1f m）  与顶点同高(±0.02)的采样点 %d"
              % (strokes, top, world, m0, flat))
        check(world > 1.02 * m0,
              "A9b 抬升突破了旧的 1.0 天花板（世界 %.1f m > 起始 %.1f m × 1.02）"
              % (world, m0))
        check(world < 4.0 * m0,
              "A9b2 没撞上限，测的不是饱和态（%.1f m < 4 × %.1f m）"
              % (world, m0))
        check(flat < 60,
              "A9c 收尾是锥形不是平台（同高采样点 %d < 60）" % flat)
        shoot(hwnd, "_wt_landscape_tools.png")

        # ================================================== C 无报错
        print("\n[C] 日志健康")
        with tap.lock:
            bad = [l for l in tap.lines if re.search(BAD_RE, l)]
        check(not bad, "C1 无 Vulkan 验证层 / ERROR（%d 条）" % len(bad))
        for l in bad[:6]:
            print("      !", l.strip()[:160])

        print("\n截图：%s" % OUT_DIR)
    finally:
        try:
            proc.terminate()
        except Exception:                       # noqa: BLE001
            pass

    if fails:
        print("\n失败 %d 项：" % len(fails))
        for f in fails:
            print("  -", f)
        return 1
    print("\n全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
