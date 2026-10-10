"""端到端验证：视口 Shift+A 添加菜单 + 视口正下方播放条。

覆盖：
  1. 布局断言（日志矩形，确定性）：
     - TR-BAR 紧贴 VP-RECT 底边（播放条在视口正下方）
     - TR-BAR 高度 ≈ 34px（"刚刚好一行"）
     - 彩色按钮组（Run/Play/Pause/Stop）水平居中于 TR-BAR
  2. Shift+A 菜单：
     - 打开菜单（VP-ADD-OPEN 日志 = 打开成功）
     - 键盘导航激活 Mesh > Cube（ImGui 已启用 NavEnableKeyboard：
       Down 移动项 / Right 展开子菜单 / Enter 激活 —— 不猜像素坐标）
     - 断言：added 'Cube' 日志 + 视口像素变化
     - 键盘导航激活 Light > Sunlight：断言 added 'Sunlight' 日志
  3. 截图存档（人工复核）：菜单弹出 / 子菜单展开 / 添加后的 Inspector

用法：
    python tools/verify_add_menu.py
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32  # noqa: E402
from verify_asset_drag import force_foreground, move_to  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

# tag 字符类必须含小写：VP-ADD-Mesh / VP-ADD-Cube 这些 tag 带小写字母，
# 只写 [A-Z-] 会静默失配（转储看得见、正则抓不到，排查半天）。
# `#\d+` 是"第几次打开弹出菜单"的序号 —— 菜单弹出物的矩形都带它，见
# ViewportPanel::drawAddMenu 里那段注释。
RECT_RE = re.compile(
    r"([A-Za-z-]+(?:#\d+)?)"
    r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)")

WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101

VK_SHIFT = 0x10
VK_A = 0x41
VK_DOWN = 0x28
VK_RIGHT = 0x27
VK_RETURN = 0x0D
VK_ESCAPE = 0x1B

# 美式键盘布局扫描码。⚠ GLFW 的 WM_KEYDOWN 是**按扫描码**映射键值的，
# lParam 里扫描码为 0 会被映射成 GLFW_KEY_UNKNOWN（实测踩过）。
SC = {VK_A: 0x1E, VK_DOWN: 0x50, VK_RIGHT: 0x4D, VK_RETURN: 0x1C,
      VK_ESCAPE: 0x01}


def post_key(hwnd, vk):
    """直接向窗口投递按键 —— 不依赖前台焦点。"""
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (SC[vk] << 16) | 1)
    time.sleep(0.04)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (SC[vk] << 16) | 0xC0000001)
    time.sleep(0.25)


def press_shift_a(hwnd):
    """混合注入（实测最稳组合）：
    - Shift 必须是真实键盘事件 —— GLFW 的 getMods() 用 GetKeyState 读
      修饰键，PostMessage 的 Shift 在那里看不见；
    - 而真实事件只进**前台窗口**队列 —— 所以先 ALT-trick 把编辑器拉到
      前台并确认成功（失败打印警告，靠外层 3 次 retry 兜底）；
    - A 用 PostMessage 直投编辑器窗口 —— GLFW 按 lParam 扫描码映射键值
      （scancode 0 必须带上），且不依赖前台焦点。
    全真实 keybd_event(Shift+A) 实测反而不稳（脚本后台运行时 ALT-trick
    偶发被前台策略拒绝，A 落到别的窗口）。"""
    ok = False
    for _ in range(5):
        if force_foreground(hwnd):
            ok = True
            break
        time.sleep(0.2)
    if not ok:
        print("  WARN: 编辑器未成为前台窗口，Shift 注入可能失效")
    user32.keybd_event(VK_SHIFT, 0x2A, 0, 0)          # Shift down（真实）
    time.sleep(0.10)
    post_key(hwnd, VK_A)                              # A（PostMessage 直投）
    user32.keybd_event(VK_SHIFT, 0x2A, 0x0002, 0)     # Shift up
    time.sleep(0.5)


def wait_rect(tap, tag, timeout=6.0):
    """等待某个矩形日志出现（菜单/子菜单要在屏幕上出现才有）"""
    end = time.time() + timeout
    r = None
    while time.time() < end:
        r = tap.last_rect(tag)
        if r:
            return r
        time.sleep(0.15)
    return None


def open_menu_and_click(tap, hwnd, mx, my, parent_tag, item_tag, item_name):
    """打开菜单 → 按日志矩形点击父项展开子菜单 → 按日志矩形点击子项。
    编辑器会把弹出窗口 / 每个菜单项的真实矩形打到日志（VP-ADD-*），
    因此完全不猜字体行高。返回是否成功。"""
    needle = "added '%s' via Shift+A menu" % item_name
    if tap.has_line(needle):
        return True

    for attempt in range(3):
        press_shift_a(hwnd)
        time.sleep(0.3)
        if not tap.has_line("VP-ADD-OPEN"):
            print("  retry: Shift+A 未触发（第 %d 次）" % (attempt + 1))
            continue
        win = wait_rect(tap, "VP-ADD-WIN", 3.0)
        parent = wait_rect(tap, parent_tag, 2.0) if win else None
        if not win:
            print("  retry: 弹窗矩形未出现（第 %d 次）" % (attempt + 1))
        elif not parent:
            print("  retry: 父项 %s 矩形未出现（第 %d 次）"
                  % (parent_tag, attempt + 1))
        if not win or not parent:
            with tap.lock:
                dbg = [l for l in tap.lines if "VP-ADD" in l]
            print("  VP-ADD 日志:", dbg[-6:] if dbg else "(none)")
            esc(hwnd)
            continue
        # 悬停父项 → 子菜单展开（矩形日志随之出现）
        move_to(hwnd, (parent[0] + parent[2]) * 0.5,
                (parent[1] + parent[3]) * 0.5)
        item = wait_rect(tap, item_tag, 3.0)
        if not item:
            print("  retry: 子项矩形未出现（第 %d 次）" % (attempt + 1))
            esc(hwnd)
            continue
        click(hwnd, (item[0] + item[2]) * 0.5, (item[1] + item[3]) * 0.5)
        time.sleep(0.6)
        if tap.has_line(needle):
            return True
        esc(hwnd)
    return tap.has_line(needle)


def esc(hwnd):
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, (SC[VK_ESCAPE] << 16) | 1)
    time.sleep(0.04)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_ESCAPE, (SC[VK_ESCAPE] << 16) | 0xC0000001)
    time.sleep(0.3)


class LogTap:
    def __init__(self):
        self.lines = []
        self.lock = threading.Lock()

    def start(self, proc):
        self.t = threading.Thread(target=self._pump, args=(proc,), daemon=True)
        self.t.start()

    def _pump(self, proc):
        for raw in proc.stdout:
            with self.lock:
                self.lines.append(raw.strip())

    def last_rect(self, tag):
        """返回 (x0, y0, x1, y1)，找不到返回 None。

        tag 按**前缀**匹配，`#<序号>` 后缀可有可无；取最后一条 = 最新一次的
        真实坐标。菜单弹出物的 tag 带 `#<第几次打开>` —— 关掉这个后缀去精确
        比对，会命中上一次打开留下的陈旧矩形，然后脚本会在子菜单展开之前
        就点下去（这就是"第一次加 Cube 成功、接着加 Sphere 必失败"的根因）。
        """
        rx = re.compile(r"\b" + re.escape(tag) + r"(?:#\d+)?=\(")
        with self.lock:
            found = None
            for line in self.lines:
                if rx.search(line):
                    m = RECT_RE.search(line)
                    if m:
                        found = tuple(float(m.group(i)) for i in range(2, 6))
            return found

    def has_line(self, needle):
        with self.lock:
            return any(needle in l for l in self.lines)


def click(hwnd, cx, cy):
    from verify_asset_drag import client_to_screen
    from verify_nav_gizmo import MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.25)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.5)


def region_diff(img_a, img_b, box):
    """box=(x0,y0,x1,y1) 内的平均绝对像素差（0-255）；越界自动裁剪"""
    x0, y0, x1, y1 = [int(v) for v in box]
    W, H = img_a.size
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W - 1), min(y1, H - 1)
    pa, pb = img_a.load(), img_b.load()
    total, n = 0, 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            a, b = pa[x, y], pb[x, y]
            total += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
            n += 1
    return total / max(n, 1) / 3.0


def kill_stale_editors():
    """之前脚本崩溃可能留下孤儿 Editor.exe —— find_window 会摸到旧窗口，
    坐标全错。开跑前清场。

    ⚠ taskkill 只是"请求"，返回时窗口往往还活着几帧。如果不等它真的消失就
    放行，紧接着的 find_window 会摸到**那具尸体**：之后 SetWindowPos / 鼠标
    事件全发给旧窗口，新开的那份反而收不到。实测症状：脚本单独跑 100% 过，
    连着跑就偶发"拖拽不生效 + 截图里布局是乱的"（因为坐标换算用的是旧窗口
    的原点，鼠标实际落在了分隔条上，顺手把分栏比例拖到了 0.93）。

    所以这里多等一步：**枚举不到 "Venn Editor" 顶层窗口**才返回。
    """
    subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 6.0
    while time.time() < end:
        # find_window 的 timeout 只是"再试一轮"的上限，给 0 会一次都不枚举
        if not find_window("Venn Editor", 0.35):
            break
        time.sleep(0.2)
    time.sleep(0.6)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "..", "_venn_verify"))
    ap.add_argument("--size", default="1600x900")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    out = os.path.abspath(args.out_dir)
    os.makedirs(out, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    kill_stale_editors()

    child_env = dict(os.environ)
    child_env["MYVK_LOG_RECTS"] = "1"
    child_env["MYVK_NO_WINDOW_SAVE"] = "1"

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=child_env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    print("launched pid", proc.pid)

    fails = []
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        time.sleep(2.0)
        tw, th = (int(v) for v in args.size.lower().split("x"))
        user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
        time.sleep(2.5)
        force_foreground(hwnd)
        time.sleep(1.0)

        vp = tap.last_rect("VP-RECT")
        tr = tap.last_rect("TR-BAR")
        print("VP-RECT:", vp)
        print("TR-BAR :", tr)
        if not vp or not tr:
            print("缺 VP-RECT / TR-BAR 日志")
            return 3

        # 窗口尺寸 sanity check：偶尔 Window Manager 不吃 resize，
        # 坐标会整体错位 —— 检测到就重试一次。
        crect = wt.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(crect))
        if abs(crect.right - tw) > 60 or abs(crect.bottom - th) > 60:
            print("WARN: 客户区 %dx%d 不符，重试 resize"
                  % (crect.right, crect.bottom))
            user32.ShowWindow(hwnd, 9)
            time.sleep(0.5)
            user32.SetWindowPos(hwnd, 0, 100, 100, tw, th, 0x0004)
            time.sleep(2.0)
            vp = tap.last_rect("VP-RECT")
            tr = tap.last_rect("TR-BAR")
            print("retry VP-RECT:", vp, " TR-BAR:", tr)

        # ---- 断言 1：播放条紧贴视口底边、高度 ≈ 34 ----
        # （VP-RECT 是视口窗口内的 3D 图像矩形；图像下方还有 ~8px 的
        #   窗口内边距，所以允许 10px 容差）
        if abs(tr[1] - vp[3]) <= 10.0:
            print("断言 1a (TR-BAR 紧贴视口底边): PASS")
        else:
            print("断言 1a: FAIL")
            fails.append("1a tr.y0=%.0f vs vp.y1=%.0f" % (tr[1], vp[3]))
        if abs((tr[3] - tr[1]) - 34.0) <= 5.0:
            print("断言 1b (TR-BAR 高度≈34): PASS")
        else:
            print("断言 1b: FAIL")
            fails.append("1b h=%.0f" % (tr[3] - tr[1]))

        # ---- 断言 2：整组彩色按钮在播放条内且水平居中 ----
        img, _ = capture_client(hwnd)
        px = img.load()
        W, H = img.size
        colored = [(x, y) for y in range(int(tr[1]) + 2, min(int(tr[3]) - 2, H))
                   for x in range(0, W)
                   if max(px[x, y][:3]) - min(px[x, y][:3]) > 45]
        if colored:
            xs = [p[0] for p in colored]
            row_cx = (min(xs) + max(xs)) * 0.5
            tr_cx = (tr[0] + tr[2]) * 0.5
            ok = abs(row_cx - tr_cx) < 40.0
            print("断言 2 (按钮组居中, 组中心 x=%.0f, 条中心 x=%.0f): %s"
                  % (row_cx, tr_cx, "PASS" if ok else "FAIL"))
            if not ok:
                fails.append("2 row-center x=%.0f vs %.0f" % (row_cx, tr_cx))
        else:
            print("断言 2: FAIL（没找到彩色按钮）")
            fails.append("2 no colored buttons")

        img, _ = capture_client(hwnd)
        img.save(os.path.join(out, "addmenu_00_full.png"))

        # ---- Shift+A → Mesh > Cube（按日志矩形精确点击）----
        mx, my = int((vp[0] + vp[2]) * 0.5), int((vp[1] + vp[3]) * 0.5)
        move_to(hwnd, mx, my)
        time.sleep(0.5)
        before = capture_client(hwnd)[0]

        ok_cube = open_menu_and_click(tap, hwnd, mx, my,
                                      "VP-ADD-Mesh", "VP-ADD-Cube", "Cube")
        after = capture_client(hwnd)[0]

        img, _ = capture_client(hwnd)
        img.save(os.path.join(out, "addmenu_01_after_cube.png"))

        ok_log = tap.has_line("added 'Cube' via Shift+A menu")
        d = region_diff(before, after, vp)
        print("视口平均像素差 after Cube: %.2f, added'Cube': %s (click ok=%s)"
              % (d, ok_log, ok_cube))
        if ok_log and d > 0.5:
            print("断言 3 (Mesh>Cube 落进场景): PASS")
        else:
            print("断言 3: FAIL")
            fails.append("3 cube log=%s diff=%.2f" % (ok_log, d))

        # ---- Shift+A → Light > Sunlight ----
        ok_sun = open_menu_and_click(tap, hwnd, mx, my,
                                     "VP-ADD-Light", "VP-ADD-Sunlight",
                                     "Sunlight")

        move_to(hwnd, W - 200, H - 60)   # 光标挪开
        time.sleep(0.5)
        img, _ = capture_client(hwnd)
        img.save(os.path.join(out, "addmenu_02_final.png"))

        ok_sun = tap.has_line("added 'Sunlight' via Shift+A menu")
        print("断言 4 (Light>Sunlight 配置太阳): %s"
              % ("PASS" if ok_sun else "FAIL"))
        if not ok_sun:
            fails.append("4 sunlight log missing")

        print()
        if fails:
            print("失败项：", fails)
            return 4
        print("全部断言 PASS（截图见 %s/addmenu_*.png）" % out)
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
