"""端到端验证：把 Content 面板里的**纹理图片**用鼠标拖到 Inspector 的材质槽。

和 verify_asset_drag.py 是同一套路子（ImGui 的拖拽只能靠真实鼠标按下-移动-
松开触发，没法用开关"模拟"）：

  1. 带 MYVK_LOG_RECTS=1 启动编辑器，它会往 stdout 打两类矩形：
       CB-CELL <文件名>   Content 面板里每个格子的矩形
       MAT-SLOT <槽名>    Inspector 材质区里每一行纹理槽的矩形
     两者坐标都是客户区物理像素，和 PrintWindow 截出来的图 1:1。
  2. 从日志里取 "CB-CELL Cube_BaseColor.png" 和 "MAT-SLOT albedo"；
  3. SetCursorPos + mouse_event 做一次真实的拖拽；
  4. 断言日志里出现 material slot 'albedo' <- ... (srgb=1)。

顺带覆盖一个很容易写错、又很难肉眼发现的地方：**同一张图绑到 albedo
（sRGB）和 normal（线性）时必须是两份不同的 GPU 纹理**。所以脚本会把同
一张 PNG 分别拖到 albedo 和 normal 上，然后断言两条日志的 srgb 标志不同
（1 和 0）—— 如果共用了一个缓存键，第二条会直接命中缓存拿回 sRGB 那张，
日志里就会出现两次 srgb=1。

用法：
    python tools/verify_material_drag.py
    python tools/verify_material_drag.py --asset Cube_BaseColor.png --out shot.png
"""

import argparse
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32   # noqa: E402
# 前台/坐标辅助统一走 verify_asset_drag，别各写一份 —— 重复实现的版本会
# 落后于那边的修复（比如 attach-thread-input），于是同一个坑要踩两遍。
from verify_asset_drag import client_to_screen, force_foreground, move_to  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = r"E:\code\cpp\_venn_verify"

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

RECT_RE = re.compile(
    r"(CB-CELL [^=]+|MAT-SLOT [^=]+"
    r"|VP-RECT|HIER-RECT row0|INS-RECT)"
    r"=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)"
)
BIND_RE = re.compile(r"material slot '([^']*)' <- (\S+) \(srgb=(\d)\)")


class LogTap:
    """后台读子进程 stdout，边打边解析矩形与绑定记录。"""

    def __init__(self, proc):
        self.proc = proc
        self.rects = {}
        self.binds = []          # [(slot, rel, srgb), ...]
        self.lines = []
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._pump, daemon=True)
        self.t.start()

    def _pump(self):
        for raw in self.proc.stdout:
            line = raw.strip()
            with self.lock:
                self.lines.append(line)
                m = RECT_RE.search(line)
                if m:
                    self.rects[m.group(1)] = tuple(
                        int(m.group(i)) for i in range(2, 6))
                b = BIND_RE.search(line)
                if b:
                    self.binds.append((b.group(1), b.group(2), int(b.group(3))))

    def rect(self, tag):
        with self.lock:
            return self.rects.get(tag)

    def wait_rect(self, tag, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            r = self.rect(tag)
            if r:
                return r
            time.sleep(0.1)
        return None

    def wait_bind(self, slot, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for b in self.binds:
                    if b[0] == slot:
                        return b
            time.sleep(0.1)
        return None


def press():
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)


def release():
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)


def drag(hwnd, src, dst, label):
    """从 src(client 坐标) 真实拖到 dst。返回 True 表示走完了流程。"""
    sx = (src[0] + src[2]) // 2
    sy = (src[1] + src[3]) // 2
    tx = (dst[0] + dst[2]) // 2
    ty = (dst[1] + dst[3]) // 2
    print("[%s] src=%d,%d -> dst=%d,%d" % (label, sx, sy, tx, ty))

    force_foreground(hwnd)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.25)
    move_to(hwnd, sx, sy)
    time.sleep(0.3)
    press()
    time.sleep(0.25)

    # 先原地挪几像素越过 ImGui 的拖拽阈值（默认 6px），拖源才会"点火"
    for dx in (4, 9, 14):
        move_to(hwnd, sx + dx, sy + dx // 2)
        time.sleep(0.18)

    steps = 8
    for i in range(1, steps + 1):
        move_to(hwnd, sx + (tx - sx) * i // steps,
                sy + (ty - sy) * i // steps)
        time.sleep(0.14)

    time.sleep(0.3)
    release()
    time.sleep(1.2)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--asset", default="Cube_BaseColor.png")
    ap.add_argument("--content-dir", default="assets/models/Cube")
    ap.add_argument("--select", default="Ground")
    ap.add_argument("--out", default=os.path.join(OUT_DIR, "material_drag.png"))
    # 材质槽在 Inspector 靠下的位置，窗口要够高才在屏幕内
    ap.add_argument("--size", default="1600x1220")
    ap.add_argument("--env", action="append", default=[])
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    child_env = dict(os.environ)
    child_env["MYVK_LOG_RECTS"] = "1"
    child_env["MYVK_NO_WINDOW_SAVE"] = "1"
    child_env["MYVK_CONTENT_DIR"] = args.content_dir
    child_env["MYVK_EDITOR_SELECT"] = args.select
    for kv in args.env:
        k, v = kv.split("=", 1)
        child_env[k] = v

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=child_env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap(proc)
    print("launched pid", proc.pid)

    failures = []
    try:
        hwnd = find_window("Venn Editor")
        if not hwnd:
            print("未找到窗口")
            return 2
        user32.ShowWindow(hwnd, 9)
        user32.SetForegroundWindow(hwnd)
        time.sleep(2.0)

        # ---- 摆成固定尺寸（ImGui 会重排并重打矩形）----
        tw, th = (int(v) for v in args.size.lower().split("x"))
        user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
        time.sleep(2.5)

        cellTag = "CB-CELL " + args.asset
        cell = tap.wait_rect(cellTag, 15.0)
        slotA = tap.wait_rect("MAT-SLOT albedo", 15.0)
        if not cell:
            print("拿不到 Content 格子：", cellTag)
            print("现有条目：", [t[len("CB-CELL "):] for t in list(tap.rects)
                                 if t.startswith("CB-CELL")])
            return 3
        if not slotA:
            print("拿不到 MAT-SLOT albedo 矩形 —— 材质区可能在窗口外")
            print("现有 MAT-SLOT：", [t for t in tap.rects
                                      if t.startswith("MAT-SLOT")])
            img, _ = capture_client(hwnd)
            img.save(args.out)
            return 3

        print("cell=%s slot(albedo)=%s" % (cell, slotA))

        # ---------- 1) 拖到 albedo（sRGB 颜色贴图，srgb=1）----------
        drag(hwnd, cell, slotA, "albedo")
        got = tap.wait_bind("albedo")
        if got and got[2] == 1:
            print("PASS  albedo <- %s (srgb=%d)" % (got[1], got[2]))
        else:
            failures.append("albedo 绑定失败/色彩空间不对: %s" % (got,))
            print("FAIL  albedo:", got)

        time.sleep(0.4)

        # ---------- 2) 同一张图拖到 normal（线性数据贴图，srgb=0）----------
        # 重新读一次矩形：槽位内容变了（多了名字 + Clear），行高会变
        slotN = tap.wait_rect("MAT-SLOT normal", 8.0)
        cell = tap.wait_rect(cellTag, 5.0)
        if slotN:
            drag(hwnd, cell, slotN, "normal")
            gotN = tap.wait_bind("normal")
            if gotN and gotN[2] == 0:
                print("PASS  normal <- %s (srgb=%d)" % (gotN[1], gotN[2]))
            else:
                failures.append("normal 绑定失败/色彩空间不对: %s" % (gotN,))
                print("FAIL  normal:", gotN)
        else:
            failures.append("拿不到 MAT-SLOT normal")

        # ---------- 3) 关键断言：同一张图不能共用缓存键 ----------
        with tap.lock:
            binds = list(tap.binds)
        srgb_of = {}
        for slot, rel, s in binds:
            srgb_of.setdefault(rel, set()).add(s)
        shared_wrong = [rel for rel, ss in srgb_of.items() if len(ss) < 2
                        and len([b for b in binds if b[1] == rel]) >= 2]
        if shared_wrong:
            failures.append("同一张图绑到不同色彩空间却用了同一个缓存键: %s"
                            % shared_wrong)
            print("FAIL  sRGB 缓存键没有分开:", shared_wrong)
        else:
            print("PASS  同一张图在 albedo/normal 上是两份不同色彩空间的纹理")

        time.sleep(0.8)
        move_to(hwnd, 60, 60)   # 挪开光标，别挡住截图
        time.sleep(0.4)
        img, ok = capture_client(hwnd)
        img.save(args.out)
        print("PrintWindow ok=%s size=%s -> %s" % (ok, img.size, args.out))

        print()
        print("绑定记录：")
        for b in binds:
            print("   slot=%-7s rel=%-28s srgb=%d" % b)
        if failures:
            print("\n最近 30 行日志：")
            for line in tap.lines[-30:]:
                print("   ", line)
            print("\nRESULT: FAIL")
            for f in failures:
                print("  -", f)
            return 4
        print("\nRESULT: PASS")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
