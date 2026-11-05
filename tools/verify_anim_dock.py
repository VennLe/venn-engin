"""端到端验证：底部动画坞（AnimationDock）。

被验证的用户需求（2026-11-05 原话）：
    "对于角色的动画，骨骼，还有运镜的相关的面板，在用户操作过程中应该是和
     场景内容是对照的。比如在做动画时，得能对照着场景中的动画去操作，而不是
     单一对着面板去做。关于动画的这块，面板只需要一个就够了，能否浓缩到一个
     操作面板。你可以尝试动画的面板放置在整个编辑器的底部，类似 ue5 和
     blender 中的那种操作方式一样。"

改造前：摄影表 / 曲线 / 骨骼曲线 / NLA / 时间轴 / 运镜 六张是**视口标签页**，
切过去视口整块不画 —— "对着场景调动画"在布局上不成立。
改造后：六张住进底部一条通栏，3D 视口始终完整可见（见 editor/AnimationDock.h）。

分组：

  A 坞出现 + 视口让位（纯日志，MYVK_FRAMES 跑完就退）
     A1 打开摄影表 → `ANIMDOCK open=1 tab=dope`
     A2 六个标签锚点齐全（顺序 = 动作 → 骨骼 → 片段 → 镜头）
     A3 内容矩形（ANIMDOCK-BODY）在坞里、在标尺下方
     A4 视口与坞**不重叠**（这是"对照"的几何前提）
     A5 标签条上**没有** dope —— 它已不是视口标签页

  B 全关 → 坞消失、视口长回来（同一台机器上与 A 对比高度）

  C 点标签换页（真鼠标）
     C1 点「运镜」→ `ANIMDOCK open=1 tab=cine`
     C2 运镜面板的控件锚点出现在坞的矩形里

  D 传输条：播放 / 暂停（真鼠标）
     D1 点 ▶ → `ANIMDOCK play=1`
     D2 再点 → `ANIMDOCK play=0`

  E 标尺拖播放头：点标尺右半，播放头矩形右移（判据是两个矩形，不是像素）

  F 日志健康：零 VUID / 零 [ERROR]

用法：
    python tools/verify_anim_dock.py          # 全套
    python tools/verify_anim_dock.py A        # 只跑 A 组
"""

import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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

TABS = ["dope", "curveed", "bonecurv", "nla", "timelineed", "cine"]


def check_(ok, msg, detail=""):
    """本文件自己的断言器：带 detail（失败时最有用的就是这几个数）。

    ⚠ 不直接用 `verify_toolkit_round2.check`：它只有 (ok, msg) 两个参数，
      而这个用例的判据全是矩形，失败时不打印矩形基本查不动。
    """
    print("    %-74s %s" % (msg, "PASS" if ok else "FAIL"))
    if detail:
        print("         %s" % detail)
    if not ok:
        fails.append(msg)
    return ok


# ------------------------------------------------------------------ 进程

class Session:
    """一程编辑器：自己写布局文件、自己按 PID 找窗口、退出时收干净。

    ⚠ 窗口必须按 **PID** 找（不是 `find_window("Venn Editor")`）：桌面上
      可能同时开着另一个 Editor，按标题会拿到别人的窗口 —— 截出来的是旧
      画面、点下去打的是别人的编辑器（本项目踩过）。
    """

    def __init__(self, opened, frames=None):
        self.tmp = tempfile.mkdtemp(prefix="venn_animdock_")
        self.layout = os.path.join(self.tmp, "layout.json")
        with open(self.layout, "w", encoding="utf-8") as f:
            json.dump({"version": 2,
                       "panels": [{"id": i, "open": (i in opened)}
                                  for i in TABS]}, f)
        self.lines = []
        self.proc = None
        self.hwnd = 0
        self.frames = frames

    def __enter__(self):
        env = dict(os.environ)
        env.update({
            "MYVK_LOG_RECTS": "1",
            "MYVK_NO_WINDOW_SAVE": "1",
            "MYVK_EDITOR_NO_GRID": "1",
            "MYVK_EDITOR_SCENE": SCENE,
            "MYVK_EDITOR_LAYOUT": self.layout,
        })
        if self.frames:
            env["MYVK_FRAMES"] = str(self.frames)
        self.log = open(os.path.join(self.tmp, "editor.log"), "w",
                        encoding="utf-8", errors="replace")
        self.proc = subprocess.Popen([EXE], cwd=BIN, env=env,
                                     stdout=self.log,
                                     stderr=subprocess.STDOUT)
        if self.frames:
            try:
                self.proc.wait(timeout=180)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        else:
            self.hwnd = self._window_of_pid(self.proc.pid)
            time.sleep(3.2)
        self.log.close()
        with open(self.log.name, encoding="utf-8", errors="replace") as f:
            self.lines = f.read().replace("\x00", "").splitlines()
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

    # ---- 读日志 ----
    def rect(self, tag, fresh=True):
        """取锚点矩形。fresh=True 时先等一帧（重试 6 次）。"""
        deadline = time.time() + (6.0 if fresh else 0.0)
        while True:
            r = self._last_rect(tag)
            if r or time.time() > deadline:
                return r
            time.sleep(0.2)

    def _last_rect(self, tag):
        pat = re.compile(re.escape(tag) +
                         r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)")
        for line in reversed(self.lines):
            m = pat.search(line)
            if m:
                return tuple(float(m.group(i)) for i in range(1, 5))
        return None

    def text(self):
        return "\n".join(self.lines)

    def events(self, needle):
        return sum(1 for l in self.lines if needle in l)

    def refresh(self):
        """交互中重新读日志（进程还活着，日志文件持续增长）。"""
        try:
            with open(self.log.name, encoding="utf-8", errors="replace") as f:
                self.lines = f.read().replace("\x00", "").splitlines()
        except OSError:
            pass

    # ---- 输入 ----
    def click(self, x, y, settle=1.1):
        """合成点击（客户区坐标）。两次 SetCursorPos 是**预热** ——
        上一动作常把光标甩出客户区，第一次移动会被当成"不在窗口里"。"""
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

    def click_tab(self, tile, tag):
        """点标签并等到目标页生效；矩形每次重新取（首次绘制后字号可能变，
        标签位置会整体右移 —— 实测 ~20px，用旧坐标会点错到隔壁）。"""
        for _ in range(4):
            self.refresh()
            r = self.rect("ANIMDOCK-TAB-%s" % tag, fresh=False)
            if not r:
                time.sleep(0.5)
                continue
            cx, cy = center(r)
            self.click(cx, cy)
            if self.events("ANIMDOCK open=1 tab=%s" % tag):
                return True
        return False


# ------------------------------------------------------------------ A
def case_a():
    print("\n=== A 坞出现 / 视口让位 ===")
    with Session({"dope"}, frames=150) as s:
        txt = s.text()
        check_("ANIMDOCK open=1 tab=dope" in txt,
               "A1 打开摄影表 → 坞出现在底部")
        dock = s.rect("ANIMDOCK-FULL", fresh=False)
        body = s.rect("ANIMDOCK-BODY", fresh=False)
        ruler = s.rect("ANIMDOCK-RULER", fresh=False)
        check_(bool(dock) and bool(body) and bool(ruler),
               "A2 坞 / 标尺 / 内容三块矩形都有")
        if dock and body and ruler:
            ok = (body[1] >= ruler[3] - 1.0 and body[3] <= dock[3] + 1.0 and
                  abs(body[0] - dock[0]) <= 1.0 and
                  abs(body[2] - dock[2]) <= 1.0)
            check_(ok, "A3 内容矩形在标尺下方、与坞同宽同底",
                   "dock=%s ruler=%s body=%s" % (dock, ruler, body))
        miss = [t for t in TABS if not s.rect("ANIMDOCK-TAB-%s" % t, False)]
        check_(not miss, "A4 六个标签锚点齐全", "missing=%s" % (miss,))
        # 视口：ViewportPanel 自己打的锚点叫 VP-RECT
        vp = s.rect("VP-RECT", fresh=False)
        if vp and dock:
            check_(vp[3] <= dock[1] + 2.0,
                   "A5 视口矩形不与坞重叠（上下对照的几何前提）",
                   "vp=%s dock=%s" % (vp, dock))
        else:
            check_(False, "A5 视口矩形不与坞重叠", "vp=%s dock=%s" % (vp, dock))
        check_(s.events("VTAB-dope") == 0,
               "A6 摄影表已不在视口标签条上",
               "VTAB-dope 出现 %d 次" % s.events("VTAB-dope"))
        bad = [l for l in s.lines if re.search(BAD_RE, l)]
        check_(not bad, "A7 零 Vulkan 校验错误 / 零 [ERROR]（%d 条）" % len(bad),
               "%s" % (bad[:2],))
        return vp[3] - vp[1] if vp else 0.0


# ------------------------------------------------------------------ B
def case_b(h_with_dock):
    print("\n=== B 全关 → 坞消失、视口长回来 ===")
    with Session(set(), frames=150) as s:
        txt = s.text()
        check_("ANIMDOCK open=0" in txt or "ANIMDOCK " not in txt,
               "B1 六张全关 → 坞不出现")
        vp = s.rect("VP-RECT", fresh=False)
        check_(bool(vp), "B2 拿到视口矩形（%s）" % (vp,))
        if vp and h_with_dock:
            h = vp[3] - vp[1]
            check_(h > h_with_dock + 60.0,
                   "B3 关掉坞之后视口变高（%.0f → %.0f）" % (h_with_dock, h))


# ------------------------------------------------------------------ C/D/E
def case_cde():
    print("\n=== C/D/E 交互：换页 / 播放 / 拖播放头 ===")
    with Session({"dope"}) as s:
        check_(s.hwnd != 0, "C0 找到编辑器窗口")
        if not s.hwnd:
            return
        # ---- C 换到「运镜」 ----
        ok = s.click_tab(None, "cine")
        check_(ok, "C1 点「运镜」标签 → tab=cine")
        # 换页后运镜面板的控件应该落在坞里
        time.sleep(0.8)
        s.refresh()
        anchor = None
        for t in ("CINE-FOCAL", "CINE-FSTOP", "CINE-AMOUNT", "CINE-DOF"):
            anchor = s.rect(t, fresh=False)
            if anchor:
                break
        body = s.rect("ANIMDOCK-BODY", fresh=False)
        if anchor and body:
            inside = (anchor[0] >= body[0] - 2 and anchor[2] <= body[2] + 2 and
                      anchor[1] >= body[1] - 2 and anchor[3] <= body[3] + 2)
            check_(inside, "C2 运镜面板**画在坞的内容区里**（不是浮在视口上）",
                   "anchor=%s body=%s" % (anchor, body))
        else:
            check_(False, "C2 运镜面板画在坞的内容区里",
                   "anchor=%s body=%s" % (anchor, body))

        # ---- D 播放 / 暂停 ----
        t0 = s.events("ANIMDOCK play=1")
        tr = s.rect("ANIMDOCK-TRANSPORT", fresh=False)
        if tr:
            # 传输条五个按钮各 26~30px，播暂是第 3 颗
            played = False
            for dx in (26 + 2 + 26 + 2 + 15,):
                s.click(tr[0] + dx, center(tr)[1])
                s.refresh()
                if s.events("ANIMDOCK play=") > t0:
                    played = True
                    break
            check_(played, "D1 点 ▶ → 打出 ANIMDOCK play=…")
            check_(s.events("ANIMDOCK play=1") >= 1, "D2 状态是播放中（play=1）")
            n0 = s.events("ANIMDOCK play=")
            s.click(tr[0] + 26 + 2 + 26 + 2 + 15, center(tr)[1])
            s.refresh()
            check_(s.events("ANIMDOCK play=") > n0, "D3 再点一次 → 暂停")
            # 暂停后不该再动时间（对照：暂停前那次推进过）
            check_(s.events("ANIMDOCK play=0") >= 1, "D4 暂停打的是 play=0")
        else:
            check_(False, "D1 点 ▶ → 打出 ANIMDOCK play=…", "无传输条矩形")

        # ---- E 拖播放头 ----
        ruler = s.rect("ANIMDOCK-RULER", fresh=False)
        head0 = s.rect("ANIMDOCK-PLAYHEAD", fresh=False)
        if ruler and head0:
            x_mid = ruler[0] + (ruler[2] - ruler[0]) * 0.5
            x_far = ruler[0] + (ruler[2] - ruler[0]) * 0.85
            s.click(x_mid, center(ruler)[1])
            head1 = s.rect("ANIMDOCK-PLAYHEAD", fresh=True)
            # 往右拖：分两次点击，第二次在更右边 —— 只要"播放头跟着走"
            s.click(x_far, center(ruler)[1])
            head2 = s.rect("ANIMDOCK-PLAYHEAD", fresh=True)
            ok = head1 and head2 and head1[0] > head0[0] + 20.0 and \
                head2[0] > head1[0] + 20.0
            check_(ok, "E1 点标尺 → 播放头跟着右移（时间轴真的能拖）",
                   "h0=%.0f h1=%.0f h2=%.0f" %
                   (head0[0], head1[0] if head1 else -1,
                    head2[0] if head2 else -1))
        else:
            check_(False, "E1 点标尺 → 播放头跟着右移",
                   "ruler=%s head=%s" % (ruler, head0))

        bad = [l for l in s.lines if re.search(BAD_RE, l)]
        check_(not bad, "F1 交互全程零 VUID / 零 [ERROR]（%d 条）" % len(bad),
               "%s" % (bad[:2],))


# ------------------------------------------------------------------ main
def main():
    argv = sys.argv[1:]
    only = (argv[0].upper() if argv else "")
    if not os.path.isfile(EXE):
        print("找不到 %s —— 先编译 VennEditor" % EXE)
        return 2

    if only in ("", "A", "B"):
        h = case_a()
        if only in ("", "B"):
            case_b(h)
    if only in ("", "C", "D", "E"):
        case_cde()

    print("\n%d 条失败" % len(fails))
    for f in fails:
        print("  - %s" % f)
    print("结果: %s" % ("PASS" if not fails else "FAIL: " + fails[0]))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
