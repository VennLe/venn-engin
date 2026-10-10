"""端到端验证：视口左下角的 Blender 风格导航球 + 数字键视图切换。

覆盖两条交互路径（都是纯 UI 行为，命令行模拟不了，只能真输入）：

  1. **点击导航球的轴端小球** → 相机吸附到对应正视图
     （点绿色 Y+ 球 → Top 视图：pitch ≈ +89°）
  2. **数字键 1 / 3 / 7**（主键盘上排）→ Front / Right / Top
     （Blender 小键盘那一套，Ctrl+ = 反向）

数值断言靠编辑器的 NAV-CAM 日志（MYVK_LOG_RECTS=1 时，yaw/pitch 变化
超过 0.02 rad 就打一行）。

用法：
    python tools/verify_nav_gizmo.py
    python tools/verify_nav_gizmo.py --out out.png
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
from capture_window import capture_client, find_window, user32   # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground, move_to  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

NAV_RE = re.compile(r"NAV-CAM yaw=(-?[\d.]+) pitch=(-?[\d.]+)")
# tag 里含 '+'（NAV-BALL+Z），字符类必须带上它
RECT_RE = re.compile(
    r"([A-Za-z0-9+#-]+)"
    r"=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)")


class LogTap:
    def __init__(self, proc):
        self.proc = proc
        self.lines = []
        self.cams = []          # [(yaw, pitch), ...] 按出现顺序
        self.rects = {}         # tag -> (x0, y0, x1, y1)
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._pump, daemon=True)
        self.t.start()

    def _pump(self):
        for raw in self.proc.stdout:
            line = raw.strip()
            with self.lock:
                self.lines.append(line)
                m = NAV_RE.search(line)
                if m:
                    self.cams.append((float(m.group(1)), float(m.group(2))))
                r = RECT_RE.search(line)
                if r:
                    self.rects[r.group(1)] = tuple(
                        int(r.group(i)) for i in range(2, 6))

    def last_cam(self):
        with self.lock:
            return self.cams[-1] if self.cams else None

    def rect(self, tag):
        with self.lock:
            return self.rects.get(tag)

    def wait_rect(self, tag, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            r = self.rect(tag)
            if r:
                return r
            time.sleep(0.15)
        return None


WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101


def post_key(hwnd, vk, scan):
    """直接向窗口投递按键 —— 不依赖前台焦点。

    ⚠ 必须带上**真实扫描码**：GLFW 的 WM_KEYDOWN 处理是按扫描码查表的，
    lParam 高字里扫描码为 0 会被映射成 GLFW_KEY_UNKNOWN，键就"发不出去"。
    用 keybd_event(vk, 0, ...) 正是这个坑 —— 编辑器收不到任何键，测试静默
    失败（独立探针验证过：换成 PostMessage + 扫描码后 1/3/7 立刻生效）。
    """
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (scan << 16) | 1)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (scan << 16) | 0xC0000001)


VK_1, VK_3, VK_7 = 0x31, 0x33, 0x37
SC_1, SC_3, SC_7 = 0x02, 0x04, 0x08      # 美式键盘扫描码


def click(hwnd, cx, cy):
    # 每次点击前都拉一次前台：SetCursorPos + mouse_event 是走**系统输入队列**
    # 的，窗口不是前台时点击会落到别人身上（PostMessage 的按键不受影响，
    # 所以"按键能用、点击失效"这种症状很容易被误判成功能坏了 —— 实测踩过）。
    force_foreground(hwnd)
    time.sleep(0.2)
    move_to(hwnd, cx, cy)
    time.sleep(0.35)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.6)


def angdiff(a, b):
    d = (a - b) % 6.2831853
    if d > 3.1415927:
        d -= 6.2831853
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    # 验证产物一律写到项目外的 _venn_verify/，别往仓库里丢截图
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(ROOT), "_venn_verify", "nav_gizmo.png"))
    ap.add_argument("--size", default="1600x900")
    ap.add_argument("--env", action="append", default=[])
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    child_env = dict(os.environ)
    child_env["MYVK_LOG_RECTS"] = "1"
    child_env["MYVK_NO_WINDOW_SAVE"] = "1"
    for kv in args.env:
        k, v = kv.split("=", 1)
        child_env[k] = v

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=child_env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap(proc)
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

        if not force_foreground(hwnd):
            print("WARN: 窗口不在前台，输入可能不达")

        time.sleep(1.0)
        base = tap.last_cam()
        print("初始相机:", base)

        # ---- 导航球 Z+ 小球的中心 ----
        # 编辑器直接把每个正轴小球的中心打成了矩形（NAV-BALL+Z），坐标就是
        # 客户区物理像素，直接用，不用去截图里猜。
        #
        # 这里以前是"扫描左下区域的偏蓝像素"，结果误命中了 Content 面板里
        # 的蓝色像素，点了个空 —— 四个断言全 FAIL 而且看起来像功能坏了。
        ball = tap.wait_rect("NAV-BALL+Z", 10.0)
        if not ball:
            print("没拿到 NAV-BALL+Z 矩形（导航球没绘制？）")
            return 3
        gx = (ball[0] + ball[2]) // 2
        gy = (ball[1] + ball[3]) // 2
        print("Z+ 球（俯视）中心 client=(%d, %d)" % (gx, gy))

        # ---- 1. 点击 Z+ 球 → Top 视图（pitch ≈ +89° = 1.5533）----
        click(hwnd, gx, gy)
        time.sleep(0.8)
        cam = tap.last_cam()
        print("点击 Z+ 后相机:", cam)
        if cam and abs(cam[1] - 1.5533) < 0.03:
            print("断言 A (点击 Z+ -> Top, pitch≈1.5533): PASS")
        else:
            print("断言 A: FAIL")
            fails.append("A: click Z+ -> pitch=%s" % (cam,))

        # ---- 2. 数字键 1 → Front（yaw ≈ +90° = 1.5708, pitch ≈ 0）----
        post_key(hwnd, VK_1, SC_1)
        time.sleep(0.8)
        cam = tap.last_cam()
        print("按 1 后相机:", cam)
        if cam and abs(angdiff(cam[0], 1.5708)) < 0.03 and abs(cam[1]) < 0.03:
            print("断言 B (按 1 -> Front): PASS")
        else:
            print("断言 B: FAIL")
            fails.append("B: key1 -> %s" % (cam,))

        # ---- 3. 数字键 3 → Right（yaw ≈ 0, pitch ≈ 0）----
        post_key(hwnd, VK_3, SC_3)
        time.sleep(0.8)
        cam = tap.last_cam()
        print("按 3 后相机:", cam)
        if cam and abs(angdiff(cam[0], 0.0)) < 0.03 and abs(cam[1]) < 0.03:
            print("断言 C (按 3 -> Right): PASS")
        else:
            print("断言 C: FAIL")
            fails.append("C: key3 -> %s" % (cam,))

        # ---- 4. 数字键 7 → Top（pitch ≈ +1.5533）----
        post_key(hwnd, VK_7, SC_7)
        time.sleep(0.8)
        cam = tap.last_cam()
        print("按 7 后相机:", cam)
        if cam and abs(cam[1] - 1.5533) < 0.03:
            print("断言 D (按 7 -> Top): PASS")
        else:
            print("断言 D: FAIL")
            fails.append("D: key7 -> %s" % (cam,))

        # 回到 Front 视图截图存档（画面特征明显：栅格水平线 + 导航球 X 朝右）
        post_key(hwnd, VK_1, SC_1)
        time.sleep(1.0)
        vp = tap.rect("VP-RECT")
        if vp:
            move_to(hwnd, (vp[0] + vp[2]) // 2, vp[1] + 40)   # 挪开，别挡 tooltip
        time.sleep(0.6)
        img, ok = capture_client(hwnd)
        img.save(args.out)
        print("截图 ok=%s -> %s" % (ok, args.out))

        print()
        if fails:
            print("失败项：", fails)
            return 4
        print("全部断言 PASS")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
