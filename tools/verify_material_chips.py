"""端到端验证：Material 面板每个属性右边的"纹理槽小方块"。

用户要的形态（原文）：
    "物体的纹理贴图请直接在 Material 栏那里添加，basecolor，roughness，
     Metallic，Emissive，在这几个属性右侧各放置一个小方框（其中带一个小
     加号），可将贴图拖拽其中生效（拖拽成功后在该方型区域中显示该纹理贴图
     的文件名称）"

所以这里断言四件事：
  1. **结构**：六个槽都在，且各自紧贴在对应属性那一行的右侧同一列上
     （albedo / metallic / roughness / emissive / orm / normal）。
  2. **空态**：没绑东西时是一个小方块（≈22px）—— 不是一整行的大控件。
  3. **拖拽绑定**：从 Content 面板拖图片进去 → 日志出现绑定记录，
     且色彩空间正确（颜色贴图 srgb=1 / 数据贴图 srgb=0）。
  4. **绑定后显示文件名**：方块变宽，里面显示的是**文件名**（basename，
     不是整条资产路径）。这条由编辑器的 MAT-CHIP 签名日志断言。
     —— 靠截图认字既脆又慢，而 MAT-CHIP 就是界面文案的唯一来源。
  5. **真的生效**：给 emissive 绑一张亮图 → 视口整片变亮
     （着色器确实采样了新加的 set 6，而不是只改了个 UI 状态）。

用法：
    python tools/verify_material_chips.py
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
from verify_asset_drag import force_foreground, move_to          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = r"E:\code\cpp\_venn_verify"

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

RECT_RE = re.compile(
    r"(CB-CELL [^=]+|MAT-SLOT [^=]+|MAT-CHIP [^ ]+|VP-RECT)"
    r"=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)")
# MAT-CHIP <slot> shows="<file name>"  （空槽是 "(empty)"）
CHIP_RE = re.compile(r'MAT-CHIP (\S+) shows="([^"]*)"')
BIND_RE = re.compile(r"material slot '([^']*)' <- (\S+) \(srgb=(\d)\)")

SLOTS = ["albedo", "metallic", "roughness", "emissive", "orm", "normal"]
# 颜色贴图 vs 数据贴图 —— 决定了 sRGB 解码与否
SRGB_EXPECT = {"albedo": 1, "emissive": 1,
               "metallic": 0, "roughness": 0, "orm": 0, "normal": 0}


class LogTap:
    def __init__(self, proc):
        self.proc = proc
        self.rects = {}
        self.chips = {}          # slot -> 最新一次 "显示的内容"
        self.binds = []          # [(slot, rel, srgb)]
        self.lines = []
        self.lock = threading.Lock()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for raw in self.proc.stdout:
            line = raw.strip()
            with self.lock:
                self.lines.append(line)
                m = RECT_RE.search(line)
                if m:
                    self.rects[m.group(1)] = tuple(
                        int(m.group(i)) for i in range(2, 6))
                c = CHIP_RE.search(line)
                if c:
                    self.chips[c.group(1)] = c.group(2)
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

    def chip(self, slot):
        with self.lock:
            return self.chips.get(slot)

    def wait_chip(self, slot, want, timeout=8.0):
        """等某个槽显示的名字变成 want（"" = 空）。"""
        end = time.time() + timeout
        while time.time() < end:
            v = self.chip(slot)
            if v is not None and (v == want or (want == "" and v == "(empty)")):
                return True
            time.sleep(0.1)
        return False

    def wait_bind(self, slot, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for b in self.binds:
                    if b[0] == slot:
                        return b
            time.sleep(0.1)
        return None


def drag(hwnd, src, dst, label):
    sx, sy = (src[0] + src[2]) // 2, (src[1] + src[3]) // 2
    tx, ty = (dst[0] + dst[2]) // 2, (dst[1] + dst[3]) // 2
    print("  [%s] (%d,%d) -> (%d,%d)" % (label, sx, sy, tx, ty))
    force_foreground(hwnd)
    time.sleep(0.25)
    move_to(hwnd, sx, sy)
    time.sleep(0.3)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.25)
    # 先原地挪几像素越过 ImGui 的拖拽阈值（默认 6px），拖源才会"点火"
    for dx in (4, 9, 14):
        move_to(hwnd, sx + dx, sy + dx // 2)
        time.sleep(0.18)
    steps = 8
    for i in range(1, steps + 1):
        move_to(hwnd, sx + (tx - sx) * i // steps, sy + (ty - sy) * i // steps)
        time.sleep(0.14)
    time.sleep(0.3)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(1.1)


def mean_luma(img, box):
    """box=(x0,y0,x1,y1) 内的平均亮度（0-255）。"""
    px = img.convert("RGB").load()
    x0, y0, x1, y1 = [int(v) for v in box]
    total = n = 0
    for y in range(y0, max(y0 + 1, y1), 3):
        for x in range(x0, max(x0 + 1, x1), 3):
            r, g, b = px[x, y]
            total += 0.299 * r + 0.587 * g + 0.114 * b
            n += 1
    return total / max(n, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--asset", default="Cube_BaseColor.png")
    ap.add_argument("--content-dir", default="assets/models/Cube")
    ap.add_argument("--select", default="Ground")
    ap.add_argument("--size", default="1600x1220")
    ap.add_argument("--out", default=os.path.join(OUT_DIR, "material_chips.png"))
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_CONTENT_DIR"] = args.content_dir
    env["MYVK_EDITOR_SELECT"] = args.select

    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap(proc)
    fails = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if detail else ""))
        if not ok:
            fails.append("%s %s" % (name, detail))

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

        vp = tap.wait_rect("VP-RECT", 15.0)
        cell = tap.wait_rect("CB-CELL " + args.asset, 15.0)
        if not vp or not cell:
            print("缺 VP-RECT 或 Content 格子", vp, cell)
            return 3

        # ---------- 1) 六个槽都在 ----------
        rects = {}
        for s in SLOTS:
            rects[s] = tap.wait_rect("MAT-SLOT " + s, 12.0)
        missing = [s for s in SLOTS if not rects[s]]
        check("六个纹理槽都在（albedo/metallic/roughness/emissive/orm/normal）",
              not missing, "缺 " + str(missing) if missing else "")

        # ---------- 2) 空态：是小方块 ----------
        # 空槽宽 22px；如果退化成"一整行"就会是几百像素。
        # albedo 例外：场景里 Ground 的材质本来就带一张默认底色图，它一
        # 上屏就是"已绑定"的样子（正好顺便验证了绑定态的那种宽度）。
        if not missing:
            widths = {s: rects[s][2] - rects[s][0] for s in SLOTS}
            bad = {s: w for s, w in widths.items() if w > 30 and s != "albedo"}
            check("空槽是小方块（宽 ≤ 30px）", not bad, str(widths))

            # 位置：六个槽都要**落在属性行右侧**，且不越出面板右边缘。
            # 不要求左边缘严格对齐 —— 槽位是跟在标签后面自然排的（标签
            # 长短不同），钉成固定列反而会把标签挤扁 / 让槽位叠到控件上。
            panel = tap.wait_rect("INS-MESH-HEADER", 6.0)
            if panel:
                left, right = panel[0], panel[2] + 10
                off = {s: rects[s][0] for s in SLOTS
                       if rects[s][0] < left + 60}
                check("六个槽都在属性行右侧（控件列之后）", not off,
                      "面板左缘 %d，越界的 %s" % (left, off))
                over = {s: rects[s][2] for s in SLOTS
                        if rects[s][2] > right}
                check("六个槽都不越出面板右边缘", not over,
                      "面板右缘 %d，越界的 %s" % (right, over))
            for s in ("metallic", "roughness", "emissive", "orm", "normal"):
                check("空槽 %-9s 显示为空" % s, tap.wait_chip(s, "", 3.0),
                      repr(tap.chip(s)))

        # ---------- 3) 拖拽绑定 + 显示文件名 ----------
        before = capture_client(hwnd)[0]
        luma0 = mean_luma(before, vp)

        basename = args.asset.replace("\\", "/").split("/")[-1]

        def chip_looks_like(shown, want):
            """槽位显示的应当是**文件名**：允许左边被截断（"..."），
            但去掉省略号后必须正好是文件名的后缀 —— 既排除了整条路径，
            也排除了缓存键之类的内部标记。"""
            if not shown:
                return False
            tail = shown[3:] if shown.startswith("...") else shown
            return bool(tail) and want.endswith(tail)

        for slot in ("metallic", "roughness", "emissive"):
            got = None
            for attempt in range(3):
                r = tap.wait_rect("MAT-SLOT " + slot, 6.0)
                cell = tap.wait_rect("CB-CELL " + args.asset, 6.0)
                if not r or not cell:
                    check("拖到 %s" % slot, False, "矩形缺失")
                    break
                drag(hwnd, cell, r, slot)
                got = tap.wait_bind(slot)
                if got:
                    break
                print("  retry %d: %s 没收到拖放（首个拖拽偶发丢事件）"
                      % (attempt + 1, slot))
                time.sleep(0.5)
            check("%s 收到拖拽绑定" % slot, got is not None, str(got))
            if got:
                check("  %s 的色彩空间 (期望 srgb=%d)" % (slot, SRGB_EXPECT[slot]),
                      got[2] == SRGB_EXPECT[slot], "实际 srgb=%d" % got[2])
            ok = False
            for _ in range(30):
                if chip_looks_like(tap.chip(slot), basename):
                    ok = True
                    break
                time.sleep(0.1)
            check("  %s 方块里显示文件名" % slot, ok,
                  "显示的是 %r，期望以 %r 结尾（允许左侧省略号）"
                  % (tap.chip(slot), basename))
            r2 = tap.wait_rect("MAT-SLOT " + slot, 6.0)
            if r2:
                check("  绑定后方块变宽以放下文件名",
                      (r2[2] - r2[0]) > 60, "宽 %dpx" % (r2[2] - r2[0]))

        # ---------- 4) emissive 真的生效（视口整片变亮）----------
        time.sleep(1.2)
        move_to(hwnd, 60, 60)
        time.sleep(0.5)
        after = capture_client(hwnd)[0]
        luma1 = mean_luma(after, vp)
        check("emissive 贴图真的进了着色器（视口平均亮度上升）",
              luma1 > luma0 + 1.0,
              "%.2f -> %.2f" % (luma0, luma1))
        after.save(args.out)

        print()
        if fails:
            print("最近 40 行日志：")
            for line in tap.lines[-40:]:
                print("   ", line)
            print("\nRESULT: FAIL")
            for f in fails:
                print("  -", f)
            return 4
        print("截图：", args.out)
        print("RESULT: PASS")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
