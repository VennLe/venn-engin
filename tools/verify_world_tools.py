"""端到端验证：地形/植被编辑器的**参数与操作功能**升级（2026-10-20 第二轮）。

被验证的用户需求（原话）：
  "这个实现效果是否有些过于粗糙了，能否优化一下其中的详细参数和操作功能"

本轮新增的能力，逐条对应断言：
  A. 地形
     A1 工具从 4 个变 6 个（Raise / Smooth / Flatten / Noise / Erode / Paint）
     A2 一笔结束产生一条撤销记录（LS-UNDO-PUSH），且高度图真的变了
     A3 Ctrl+Z 把磁盘高度图**精确**还原到刷之前（逐点差 = 0）
     A4 Ctrl+Shift+Z 重做回刷之后
     A5 Noise 工具真的在扰动表面（逐点绝对差之和变大）
     A6 Erode 工具真的搬运了物质
     A7 整块操作（Smooth all）可用且改了高度图
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


def read_heights(path):
    """height.r32 → float 列表（列主序）。文件不存在返回 None。"""
    if not os.path.exists(path):
        return None
    raw = open(path, "rb").read()
    n = len(raw) // 4
    return list(struct.unpack("<%df" % n, raw[: n * 4]))


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

        bp = brush_point(tap)
        print("    落点 = (%.0f, %.0f)" % (bp[0], bp[1]))
        bx, by = bp

        # ================================================== A 地形
        print("\n[A] 地形：工具 / 撤销 / 重做 / 噪声 / 侵蚀 / 整块操作")

        tool_rects = [rect_of(tap, "LS-TOOL-%d" % i, 3.0) for i in range(6)]
        check(all(tool_rects), "A1 六个工具按钮齐全（缺: %s）"
              % ([i for i, r in enumerate(tool_rects) if not r] or "无"))

        # A1b：滚轮调半径（`[` `]` 是 OEM 键，某些键盘布局下收不到，
        #      所以滚轮才是主入口 —— 这条断言保证它真的能调）
        focus_window(hwnd)
        user32.SetCursorPos(*client_to_screen(hwnd, bx, by))
        time.sleep(0.35)
        for _ in range(3):
            user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, 120, 0)
            time.sleep(0.2)
        time.sleep(0.45)
        check(count(tap, "LS-BRUSH radius=") > 0,
              "A1b 滚轮调半径生效（LS-BRUSH radius=）")

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

        # A5：噪声笔刷
        click_rect(hwnd, rect_of(tap, "LS-TOOL-3", 3.0), "Noise 按钮")
        check(count(tap, "LS-TOOL 3") > 0, "A5a 切到 Noise 工具")
        hn0 = read_heights(HEIGHT)
        drag(hwnd, (bx, by), (bx + 30.0, by - 20.0))
        hn1 = read_heights(HEIGHT)
        d_noise = absdiff(hn0, hn1)
        check(d_noise > 1.0e-3, "A5b 噪声扰动生效 (absdiff=%.5f)" % d_noise)

        # A6：侵蚀工具
        click_rect(hwnd, rect_of(tap, "LS-TOOL-4", 3.0), "Erode 按钮")
        check(count(tap, "LS-TOOL 4") > 0, "A6a 切到 Erode 工具")
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
        shoot(hwnd, "_wt_landscape_tools.png")

        # ================================================== B 植被
        print("\n[B] 植被：5 种类型 / 色差 / 过滤")
        click_rect(hwnd, rect_of(tap, "TB-EDITMODE"), "编辑模式下拉")
        fo = rect_of(tap, "TB-EDITMODE-ITEM foliage", 3.0)
        clicked = click_rect(hwnd, fo, "Foliage 菜单项")
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
        shoot(hwnd, "_wt_foliage_kinds.png")

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
