"""端到端验证：地形与视口栅格共面 + 形状雕刻 + 坡道笔刷 + 力度上限（2026-10-20）。

被验证的用户需求（原话）：
  1) "地形模式下的地面应该要和原来栅格化的地面保持同一平面才是"
  2) "地形模式下的笔刷种类还不够丰富，不支持挖洞的模式"
  3) "笔刷的力度区间可以调更大一下"

断言分组：
  A. 共面
     A1 新建地形的高度图**四条边**全是 0（栅格画在 z=0，地形最低点必须
        落在同一个平面上，否则地形边缘要么悬空、要么插到栅格下面）
     A2 中心仍有起伏，但**幅度受限**（2026-10-21 改）：
        旧断言是 max > 0.30，那是"默认地形能顶到 1.0"时代的期望。
        用户后来报的正是这个 —— 地形一起手就是几十米高，把模板场景
        （Ground/Player/Pickup 全坐在 z≈0）整个埋掉。现在默认起伏被
        归一化到 [0, 0.12]，判据反过来：有起伏（> 0.05）但不喧宾夺主
        （<= 0.25）。
  B. 形状雕刻（原挖洞，2026-10-21 重做）
     B1 面板里有 8 个工具（LS-TOOL-0..7），Carve 是第 8 个
     B2 选中 Carve 后拖一笔 → LS-STROKE-BEGIN / END
     B3 走的是形状压印（LS-CARVE 日志），地表真的被压下去
     B4 **不再写挖穿遮罩**：holes.r8 要么不存在，要么全 255。
        这是本组最关键的断言 —— 用户明确要求"不能破坏地面拓扑"。
  C. 坡道（Ramp）
     C1 选 Ramp（LS-TOOL-3）后拖一笔 → 高度图 sum 变化
     C2 变化是**局部**的：绝大部分采样点没动（坡道不是全局平整）
  D. 力度区间
     D1 Shift + 滚轮向上 → LS-BRUSH strength= 日志出现
     D2 最终强度 > 1.0（证明上限已从 1.0 放到 16.0）
  E. 日志健康：零 VUID / 零 ERROR

用法：python tools/verify_landscape_ground.py
"""

import ctypes
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
ASSETS = os.path.join(ROOT, "build", "bin", "assets")
TDIR = os.path.join(ASSETS, "terrain", "landscape")
HEIGHT = os.path.join(TDIR, "height.r32")
HOLES = os.path.join(TDIR, "holes.r8")
DESC = os.path.join(TDIR, "terrain.vterrain")

user32 = ctypes.windll.user32

RECT_ANY = r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)"
BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"

MOUSEEVENTF_WHEEL = 0x0800
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


def last_float(tap, needle, field, timeout=6.0, since=0):
    """从最后一条含 needle 的日志里抓 `field=数字`。抓不到返回 None。

    `since` 用来跳过本组动作之前的历史行 —— 比如强度那组必须先拿到
    "开始之前的强度"，再判断滚轮之后有没有变大。
    """
    pat = re.compile(re.escape(field) + r"=(-?[\d.]+)")
    deadline = time.time() + timeout
    got = None
    while time.time() < deadline:
        with tap.lock:
            hits = [l for l in tap.lines[:] if needle in l]
        if len(hits) > since:
            m = pat.search(hits[-1])
            if m:
                got = float(m.group(1))
        if got is not None:
            return got
        time.sleep(0.1)
    return got


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


def wait_line(tap, needle, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if count(tap, needle) > 0:
            return True
        time.sleep(0.15)
    return False


def focus_window(hwnd):
    """把编辑器窗口拉到前台。Windows 只把鼠标消息投给前台窗口 ——
    没焦点时症状是"界面看着正常、日志一条不动"，极难往这儿想。"""
    if user32.GetForegroundWindow() == hwnd:
        return
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    if user32.GetForegroundWindow() != hwnd:
        user32.SwitchToThisWindow(hwnd, True)
        time.sleep(0.3)


_click_raw = click


def click(hwnd, x, y, *args, **kwargs):
    focus_window(hwnd)
    return _click_raw(hwnd, x, y, *args, **kwargs)


def drag(hwnd, src, dst, steps=16):
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
    time.sleep(0.8)


def shift_wheel(hwnd, pos, notches=20):
    """在 pos 处按住 Shift 滚滚轮 notches 格（正 = 向上）。

    ⚠ 必须真的按住 Shift：地形那支靠 `io.KeyShift` 分流，没按下就是调半径
      而不是调强度 —— 断言会看起来像"调强度没实现"。
    """
    focus_window(hwnd)
    user32.SetCursorPos(*client_to_screen(hwnd, pos[0], pos[1]))
    time.sleep(0.3)
    user32.keybd_event(VK_SHIFT, 0, 0, 0)
    time.sleep(0.15)
    for _ in range(notches):
        user32.mouse_event(MOUSEEVENTF_WHEEL, 0, 0, 120, 0)
        time.sleep(0.06)
    time.sleep(0.2)
    user32.keybd_event(VK_SHIFT, 0, 2, 0)
    time.sleep(0.5)


def read_grid():
    """读地形描述 + 高度图。返回 (cols, rows, [float]) 或 None。"""
    if not (os.path.exists(DESC) and os.path.exists(HEIGHT)):
        return None
    with open(DESC, "r", encoding="utf-8") as f:
        desc = json.load(f)
    cols, rows = int(desc["cols"]), int(desc["rows"])
    raw = open(HEIGHT, "rb").read()
    n = len(raw) // 4
    vals = list(struct.unpack("<%df" % n, raw[: n * 4]))
    if n != cols * rows:
        return None
    return cols, rows, vals


def border_stats(cols, rows, vals):
    """四条边上的 (min, max)。

    高度图是**列主序**：`heights[col*rows + row]`（与 terrain.vterrain 的
    cols/rows 对应，见 engine/terrain/Terrain.h）。写成行主序会让"边界"
    选到一条斜线上 —— 那样共面断言永远过不了，而地形看着又是对的。
    """
    edge = []
    for c in range(cols):
        edge.append(vals[c * rows + 0])
        edge.append(vals[c * rows + (rows - 1)])
    for r in range(rows):
        edge.append(vals[0 * rows + r])
        edge.append(vals[(cols - 1) * rows + r])
    return min(edge), max(edge)


def shoot(hwnd, name):
    try:
        img, _ = capture_client(hwnd)
    except Exception as e:                      # noqa: BLE001
        print("      ! 截图失败(%s): %s" % (name, e))
        return
    if img is None or img.size[0] <= 0 or img.size[1] <= 0:
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
    # 每次从零开始：地形由编辑器自己建，脚本只断言结果。
    # ⚠ holes.r8 也要删 —— 留着旧文件的话"挖洞没生效"会被旧的洞掩盖。
    for f in ("terrain.vterrain", "height.r32", "splat.rgba8", "holes.r8"):
        p = os.path.join(TDIR, f)
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
    try:
        hwnd = find_window("Venn Editor", timeout=60)
        if not hwnd:
            print("找不到编辑器窗口")
            return 1
        time.sleep(2.0)

        if not wait_line(tap, "LS-TERRAIN path=terrain/landscape", 12.0):
            check(False, "地形没被建出来（LS-TERRAIN 缺失）")
            return 1
        time.sleep(0.6)

        # ================================================== A 共面
        print("\n[A] 地形与视口栅格共面")
        g = read_grid()
        if not g:
            check(False, "读不到地形资产（%s / %s）" % (DESC, HEIGHT))
            return 1
        cols, rows, vals = g
        bmin, bmax = border_stats(cols, rows, vals)
        vmax = max(vals)
        print("      grid=%dx%d  border min=%.6f max=%.6f  interior max=%.4f"
              % (cols, rows, bmin, bmax, vmax))
        # 栅格平面在 z=0（GridSettings::planeOffset），地形高度 = height*maxHeight。
        # 所以"共面"的判据是边界上的归一化高度恒为 0 —— 允许 1e-4 的余量
        # （float 归一化 + smoothstep 的末端残差）。
        check(abs(bmin) <= 1.0e-4 and abs(bmax) <= 1.0e-4,
              "A1 四条边全为 0（min=%.6f max=%.6f）" % (bmin, bmax))
        # 上界 0.25 来自用户报的"地形把初始场景盖住了"：模板场景的实体都在
        # z≈0..1，地形一起手就几十米高的话，一进编辑器看到的是一片山坡。
        check(0.05 < vmax <= 0.25,
              "A2 中心有起伏但不喧宾夺主（0.05 < max=%.4f <= 0.25）" % vmax)
        shoot(hwnd, "landscape_coplanar.png")

        vp = rect_of(tap, "VP-RECT", 6.0)
        if not vp:
            check(False, "拿不到视口矩形（VP-RECT）")
            return 1
        # 笔心：视口左侧偏下（右侧被地形面板盖住），这块屏幕位置对着地面。
        bx = vp[0] + (vp[2] - vp[0]) * 0.16
        by = vp[1] + (vp[3] - vp[1]) * 0.72

        # ================================================== B 形状雕刻
        print("\n[B] 形状雕刻笔刷（原挖洞）")
        tools = {}
        for i in range(8):
            tools[i] = rect_of(tap, "LS-TOOL-%d" % i, 2.0)
        missing = [i for i in range(8) if not tools[i]]
        check(not missing, "B1 8 个工具按钮齐全（缺: %s）" % (missing or "无"))

        carve_btn = tools.get(7)
        if not carve_btn:
            check(False, "找不到 Carve 工具按钮（LS-TOOL-7）")
        else:
            click(hwnd, *center(carve_btn))
            time.sleep(0.5)
            check(count(tap, "index=7 name=Carve") > 0,
                  "B1b 选中 Carve 工具（index=7 name=Carve）")

            before_carve = read_grid()
            n_stroke = count(tap, "LS-STROKE-BEGIN")
            n_carve = count(tap, "LS-CARVE shape=")
            drag(hwnd, (bx, by), (bx + 46.0, by - 18.0))
            time.sleep(1.2)
            check(count(tap, "LS-STROKE-BEGIN") > n_stroke,
                  "B2 雕刻那一笔落下（LS-STROKE-BEGIN 递增）")
            # 形状压印 vs 改遮罩：看日志有没有 LS-CARVE。被压下去的采样点
            # 也要真的有，否则"日志对了但地形没动"会漏过去。
            check(count(tap, "LS-CARVE shape=") > n_carve,
                  "B3 走形状压印（LS-CARVE，不是改挖洞遮罩）")
            after_carve = read_grid()
            drop = 0
            if before_carve and after_carve:
                drop = sum(1 for a, b in zip(before_carve[2], after_carve[2])
                           if b < a - 1.0e-4)
            print("      · 雕刻后 %d 个采样点变低" % drop)
            # 阈值 50：笔刷半径 12 m / mpp 2.0 → 半径 6 个采样点，扫过一小段
            # 就有几百个。50 足以区分"真的压下去了"和"数值噪声"。
            check(drop > 50,
                  "B3b 地表被压出凹腔（%d 个采样点变低）" % drop)

            # B4 是本组最重要的一条：用户要求"不能破坏地面拓扑结构"。
            # 形状雕刻只写高度图，**一行 holes 也不该写**。所以判据是
            # "holes.r8 要么不存在，要么全 255（无挖穿）"。
            intact = True
            if os.path.exists(HOLES):
                raw = open(HOLES, "rb").read()
                intact = bool(raw) and all(v == 255 for v in raw)
            check(intact, "B4 没有挖穿遮罩（holes.r8 不存在或全 255）")
            shoot(hwnd, "landscape_carve.png")

        # ================================================== C 坡道
        print("\n[C] 坡道笔刷")
        ramp_btn = tools.get(3)
        before_ramp = read_grid()
        if not ramp_btn:
            check(False, "找不到 Ramp 工具按钮（LS-TOOL-3）")
        else:
            click(hwnd, *center(ramp_btn))
            time.sleep(0.5)
            check(count(tap, "index=3 name=Ramp") > 0,
                  "C0 选中 Ramp 工具（index=3 name=Ramp）")
            drag(hwnd, (bx + 10.0, by + 6.0), (bx + 70.0, by + 6.0), steps=20)
            time.sleep(1.0)
            after_ramp = read_grid()
            if before_ramp and after_ramp:
                changed = sum(1 for a, b in zip(before_ramp[2], after_ramp[2])
                              if abs(a - b) > 1.0e-6)
                total = len(after_ramp[2])
                check(changed > 0, "C1 坡道改了高度（%d 个采样点变化）" % changed)
                # 坡道是**局部**的：整块地形 6.6 万个点，一笔只该动一小片。
                check(changed < total * 0.25,
                      "C2 改动是局部的（%d/%d = %.2f%%）"
                      % (changed, total, changed * 100.0 / total))
            else:
                check(False, "C1 读不到高度图（before/after 为空）")
            shoot(hwnd, "landscape_ramp.png")

        # ================================================== D 力度上限
        print("\n[D] 强度区间")
        before_n = count(tap, "LS-BRUSH strength=")
        m = re.search(r"strength=([\d.]+)", lines_with(tap, "LS-BRUSH strength=")[-1]
                      ) if before_n else None
        start = float(m.group(1)) if m else None
        print("      起始强度 = %s" % ("%.2f" % start if start else "(尚无日志)"))

        shift_wheel(hwnd, (bx, by), notches=22)
        time.sleep(0.6)
        after_n = count(tap, "LS-BRUSH strength=")
        check(after_n > before_n, "D1 Shift+滚轮 产生了 strength 日志（%d 条）"
              % (after_n - before_n))
        cur = last_float(tap, "LS-BRUSH strength=", "strength")
        check(cur is not None and cur > 1.0,
              "D2 强度可以超过 1.0（现在 %.2f，旧上限是 1.00）" % (cur or -1))
        check(cur is not None and cur <= 16.0 + 1.0e-4,
              "D3 强度不越界（<= 16.00，现在 %.2f）" % (cur or -1))

        # ================================================== E 日志健康
        print("\n[E] 日志健康")
        with tap.lock:
            bad = [l for l in tap.lines if re.search(BAD_RE, l)]
        check(not bad, "E1 无 Vulkan 验证层 / ERROR（%d 条）" % len(bad))
        for l in bad[:5]:
            print("      !", l)

        print("\n截图：%s" % OUT_DIR)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except Exception:                       # noqa: BLE001
            proc.kill()

    print("\n==== %s ====" % ("ALL PASS" if not fails else "FAILED %d" % len(fails)))
    for f in fails:
        print("  -", f)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
