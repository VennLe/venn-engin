"""端到端验证：普通选择模式的**轮廓高光**（选中描边）。

被验证的用户需求（2026-11-05 原话）：
    "当选择方式是普通选择时，鼠标点击场景中的物体应该要呈现出轮廓边缘高光
     的效果。"

实现（两段，都不新增渲染目标）：
  ① 深度预通道把选中物体的几何缓冲 alpha 写成**负的视图距离**
     （`depth_only.frag` + `Renderer::setSelectionOutline`）；
  ② 后处理合成做一次"负值向外扩张"，把选中像素的边缘染成暖橙
     （`post.frag` 的 selectionOutline 段）。
为什么不用"反向外壳"（把网格再画一遍沿法线外扩）：硬边网格的法线逐面分裂，
外扩时棱角处裂开（实测是虚线描边），薄片网格更是没有背面可画 —— 这两条都
在改造过程中真实撞到过（见 Renderer.h 里 setSelectionOutline 的注释）。

分组：
  A 日志判据（硬判据，像素可能有抗锯齿抖动，日志没有）
     A1 初始没有选中 → 不该有 `ids=1`
     A2 点中场景物体 → `SEL-OUTLINE ids=1 mode=pick edit=1`
     A3 点天空（空白）→ 回到 `ids=0`
  B 像素判据（"有没有真的画出来"）
     B1 选中后视口内新增暖橙像素 ≥ 120（描边的线宽是 2px，一圈也就几百个）
     B2 取消选中后回落到基线（相差 < 25%）—— 证明那些像素是"选中态带来的"
     B3 对照区（画面右上角的天空）显著变化像素 < 1% —— 证明不是整屏变色
  C 日志健康：零 VUID / 零 [ERROR]

⚠ 为什么一定要有 B3：`SEL-OUTLINE ids=1` 只证明"编辑器把 id 交给了渲染器"，
  证明不了渲染器真的画了线。反过来只测像素也证明不了"只有普通选择才有" ——
  两边都要。

用法：
    python tools/verify_selection_outline.py
    python tools/verify_selection_outline.py A     # 只跑日志组
"""

import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image                                               # noqa: E402

from capture_window import _looks_blank, capture_client             # noqa: E402
from verify_asset_drag import client_to_screen                      # noqa: E402
from verify_toolkit_round2 import (BAD_RE, EXE, ROOT, center,        # noqa: E402
                                   shutdown)

OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
SCENE = "scenes/anime_town.json"
BIN = os.path.dirname(EXE)

user32 = ctypes.windll.user32
WM_CLOSE = 0x0010
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

fails = []


def check_(ok, msg, detail=""):
    print("    %-74s %s" % (msg, "PASS" if ok else "FAIL"))
    if detail:
        print("         %s" % detail)
    if not ok:
        fails.append(msg)
    return ok


# ------------------------------------------------------------------ 像素

def warm_count(img, box):
    """数"暖橙"像素。描边色 1.0/0.46/0.12 在 tonemap 之后被 sRGB 编码成
    大约 (255,181,95)，所以判据是"红高、绿中、蓝低"，而不是精确匹配
    某个三元组（抗锯齿与 MSAA 会让每个像素都略有不同）。"""
    c = img.crop(box).convert("RGB")
    n = 0
    for p in c.getdata():
        if p[0] > 195 and 105 < p[1] < 215 and p[2] < 135 and p[0] - p[2] > 95:
            n += 1
    return n


def diff_ratio(a, b, box, thr=18):
    """显著变化像素占比（不用整图平均差 —— 平均差会被大片相同背景稀释）。"""
    ca = list(a.crop(box).convert("RGB").getdata())
    cb = list(b.crop(box).convert("RGB").getdata())
    n = 0
    for x, y in zip(ca, cb):
        if abs(x[0] - y[0]) + abs(x[1] - y[1]) + abs(x[2] - y[2]) > thr:
            n += 1
    return 100.0 * n / max(len(ca), 1)


# ------------------------------------------------------------------ 进程

class Session:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="venn_selout_")
        self.lines = []
        self.proc = None
        self.hwnd = 0

    def __enter__(self):
        env = dict(os.environ)
        env.update({
            "MYVK_LOG_RECTS": "1",
            "MYVK_NO_WINDOW_SAVE": "1",
            "MYVK_EDITOR_NO_GRID": "1",
            "MYVK_EDITOR_SCENE": SCENE,
        })
        self.log = open(os.path.join(self.tmp, "editor.log"), "w",
                        encoding="utf-8", errors="replace")
        self.proc = subprocess.Popen([EXE], cwd=BIN, env=env,
                                     stdout=self.log, stderr=subprocess.STDOUT)
        self.hwnd = self._window_of_pid(self.proc.pid)
        time.sleep(3.4)
        return self

    def __exit__(self, *a):
        if self.proc and self.proc.poll() is None:
            shutdown(self.proc, self.hwnd or 0)
        return False

    @staticmethod
    def _window_of_pid(pid, timeout=60.0):
        end = time.time() + timeout
        while time.time() < end:
            found = []

            @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
            def _enum(hwnd, _lp):
                if not user32.IsWindowVisible(hwnd):
                    return True
                p = wt.DWORD(0)
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
                if p.value == pid and user32.GetWindowTextLengthW(hwnd) > 0:
                    found.append(hwnd)
                return True

            user32.EnumWindows(_enum, 0)
            if found:
                return found[0]
            time.sleep(0.25)
        return 0

    def refresh(self):
        try:
            with open(self.log.name, encoding="utf-8", errors="replace") as f:
                self.lines = f.read().replace("\x00", "").splitlines()
        except OSError:
            pass

    def rect(self, tag):
        self.refresh()
        pat = re.compile(re.escape(tag) +
                         r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)")
        for line in reversed(self.lines):
            m = pat.search(line)
            if m:
                return tuple(float(m.group(i)) for i in range(1, 5))
        return None

    def last_sel(self):
        self.refresh()
        for line in reversed(self.lines):
            if "SEL-OUTLINE" in line:
                m = re.search(r"ids=(\d+) mode=(\w+) edit=(\d)", line)
                if m:
                    return int(m.group(1)), m.group(2), m.group(3)
        return None

    def grab(self, name):
        for _ in range(14):
            img, _ok = capture_client(self.hwnd)
            if img.size[0] > 16 and not _looks_blank(img):
                img.save(os.path.join(OUT_DIR, name))
                return img
            time.sleep(0.4)
        return None

    def click(self, x, y, settle=1.2):
        user32.keybd_event(0x12, 0, 0, 0)
        user32.keybd_event(0x12, 0, 2, 0)
        user32.SetForegroundWindow(self.hwnd)
        time.sleep(0.3)
        sx, sy = client_to_screen(self.hwnd, int(x), int(y))
        user32.SetCursorPos(sx, sy)
        time.sleep(0.35)
        user32.SetCursorPos(sx, sy)
        time.sleep(0.35)
        user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.09)
        user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
        time.sleep(settle)
        self.refresh()


# ------------------------------------------------------------------ main
def main():
    argv = sys.argv[1:]
    only = (argv[0].upper() if argv else "")
    if not os.path.isfile(EXE):
        print("找不到 %s —— 先编译 VennEditor" % EXE)
        return 2

    print("\n=== A/B 选中 → 轮廓高光；取消 → 消失 ===")
    with Session() as s:
        check_(s.hwnd != 0, "A0 找到编辑器窗口")
        if not s.hwnd:
            return 2
        vp = s.rect("VP-RECT")
        check_(bool(vp), "A1 拿到视口矩形（%s）" % (vp,))
        if not vp:
            return 2

        # 视口中心偏下 = 地面上的房子/树；正中心往往是天空
        cx = (vp[0] + vp[2]) * 0.5
        cy = vp[1] + (vp[3] - vp[1]) * 0.72
        sky_x = vp[0] + (vp[2] - vp[0]) * 0.86
        sky_y = vp[1] + (vp[3] - vp[1]) * 0.18

        img0 = s.grab("selout_before.png")
        sel0 = s.last_sel()
        check_(sel0 is None or sel0[0] == 0,
               "A2 初始没有选中（不该有 ids=1）", "sel=%s" % (sel0,))

        # ---- 选中一个物体 ----
        # ⚠ 不能只点一个"看起来有东西"的坐标：地面本身是 **locked** 的
        #   （模板里就带 "locked": true），点它按设计不选中。这里按网格
        #   试探，命中就停 —— 判据仍然是"点中了场景物体"，只是不赌某一点。
        sel1 = None
        hit_pt = None
        for fy in (0.62, 0.72, 0.52, 0.80, 0.45):
            for fx in (0.50, 0.38, 0.62, 0.30, 0.70):
                px = vp[0] + (vp[2] - vp[0]) * fx
                py = vp[1] + (vp[3] - vp[1]) * fy
                s.click(px, py, settle=0.9)
                cur = s.last_sel()
                if cur and cur[0] >= 1:
                    sel1 = cur
                    hit_pt = (px, py)
                    break
            if sel1:
                break
        img1 = s.grab("selout_selected.png")
        check_(sel1 is not None and sel1[1] == "pick" and sel1[2] == "1",
               "A3 点中场景物体 → SEL-OUTLINE ids>=1 mode=pick",
               "sel=%s at=%s" % (sel1, hit_pt))

        # ---- 取消选中 ----
        s.click(sky_x, sky_y)
        sel2 = s.last_sel()
        img2 = s.grab("selout_deselected.png")
        check_(sel2 is not None and sel2[0] == 0,
               "A4 点天空 → 选中清空（ids=0）", "sel=%s" % (sel2,))

        if only in ("", "B"):
            if not (img0 and img1 and img2):
                check_(False, "B0 三张截图都拿到了")
            else:
                # 只比"视口内部"：UI 面板里也有暖色按钮
                box = (int(vp[0]) + 4, int(vp[1]) + 4,
                       int(vp[2]) - 4, int(vp[3]) - 4)
                w0 = warm_count(img0, box)
                w1 = warm_count(img1, box)
                w2 = warm_count(img2, box)
                check_(w1 >= w0 + 120,
                       "B1 选中后视口内新增暖橙像素（描边真的画出来了）",
                       "before=%d selected=%d (+%d)" % (w0, w1, w1 - w0))
                check_(w2 <= max(w0 * 1.25, w0 + 40.0),
                       "B2 取消选中后回落到基线（那些像素是选中态带来的）",
                       "before=%d after=%d" % (w0, w2))
                # 对照区：画面右上角的天空 + 右侧一小块，选中/取消不影响它
                cbox = (int(vp[2] - 0.22 * (vp[2] - vp[0])), int(vp[1] + 8),
                        int(vp[2] - 8), int(vp[1] + 0.25 * (vp[3] - vp[1])))
                ratio = diff_ratio(img0, img1, cbox)
                check_(ratio < 1.0,
                       "B3 对照区（右上天空）几乎没变（%.2f%% < 1%%）" % ratio,
                       "box=%s" % (cbox,))

        bad = [l for l in s.lines if re.search(BAD_RE, l)]
        check_(not bad, "C1 零 Vulkan 校验错误 / 零 [ERROR]（%d 条）" % len(bad),
               "%s" % (bad[:2],))

    print("\n%d 条失败" % len(fails))
    for f in fails:
        print("  - %s" % f)
    print("结果: %s" % ("PASS" if not fails else "FAIL: " + fails[0]))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
