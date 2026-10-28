"""端到端验证：顶栏"选择模式"改编辑模式下拉 + 地形 / 植被编辑器（2026-10-20）。

被验证的用户需求（原话）：
  1) "引擎编辑器中的选择模式那里的下拉列表应该是 <UE 截图> 这样的，选择
     不同类型的模式就会打开不同的编辑器。"
  2) "对于游戏中的地形和植被系统可以优先制作出来"

断言分组：
  A. 模式切换的入口（2026-10-21 改过）
     A0 顶栏那颗"模式下拉"已经**删掉**（TB-EDITMODE 不该再出现）
     A0b 顶栏也不该再有 "+ Add"（搬到视口工具条了，锚点 TB-ADD 换了位置）
     A1 左栏模式栏出 10 行（MODE-RAIL-ITEM select…water 全在）
     A3 点左栏 Modeling → 已实现，模式真的切过去 + 面板自动打开
       （2026-10-20 晚六个模式全部落地；逐模式动作见 verify_toolkit_modes.py）
  B. 地形编辑器
     B1 点 Landscape → EDITMODE landscape + EDITMODE-PANELS landscape=1
     B2 自动建地形：LS-CREATE / LS-TERRAIN，height.r32 真的落盘
     B3 视口里拖一笔 → LS-STROKE-BEGIN / LS-STROKE-END
     B4 磁盘上的高度图真的变了（读 height.r32 算 max，必须比刷之前高）
  C. 植被编辑器
     C1 点 Foliage → EDITMODE foliage + FO-PANEL
     C2 视口里点一下 → FOLIAGE-SPAWN + FOLIAGE-COUNT total>0
     C3 撒点落在 Foliage 分组下（层级里能看到，存盘不会丢）
  D. 回到选择模式
     D1 点 Select → EDITMODE select，两个编辑器面板都关掉
  E. 全程零 Vulkan 验证层报错 / 零 ERROR

用法：python tools/verify_edit_modes.py
"""

import ctypes
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
# 运行时的资产根是 build/bin/assets（exe 同级），地形就写在这里
ASSETS = os.path.join(ROOT, "build", "bin", "assets")
HEIGHT = os.path.join(ASSETS, "terrain", "landscape", "height.r32")

user32 = ctypes.windll.user32

RECT_ANY = r"=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)"
BAD_RE = r"VUID-|\[ERROR\]|Validation Error|leaked|SEGV"

fails = []


def check(ok, msg):
    print("    %-70s %s" % (msg, "PASS" if ok else "FAIL"))
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


def wait_line(tap, needle, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if count(tap, needle) > 0:
            return True
        time.sleep(0.15)
    return False


def wait_re(tap, pattern, timeout=8.0):
    """等某条日志匹配正则。

    ⚠ `EDITMODE-PANELS` 那行中间还夹着 `landscape=0 foliage=0`，
      想同时断言 mode 与 panel 只能用正则，子串一定匹配不上。
    """
    deadline = time.time() + timeout
    rx = re.compile(pattern)
    while time.time() < deadline:
        with tap.lock:
            if any(rx.search(l) for l in tap.lines):
                return True
        time.sleep(0.15)
    return False


def focus_window(hwnd):
    """把编辑器窗口拉到前台，否则所有点击都是空气。

    Windows 只把鼠标消息投递给**前台**窗口；而截图走 PrintWindow，
    被别的窗口挡住也能拍到完整画面 —— 于是症状是"界面看着完全正常、
    日志一条不动、后半段全 FAIL"，极难往"没焦点"上想。
    单纯 SetForegroundWindow 会被前台锁拒绝（返回后 GetForegroundWindow
    仍不是它），所以要先用一下 ALT 解除锁；还不行就用 SwitchToThisWindow 兜底。
    """
    if user32.GetForegroundWindow() == hwnd:
        return
    user32.keybd_event(0x12, 0, 0, 0)
    user32.keybd_event(0x12, 0, 2, 0)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    if user32.GetForegroundWindow() != hwnd:
        user32.SwitchToThisWindow(hwnd, True)
        time.sleep(0.3)


def switch_mode(tap, hwnd, mode_id):
    """切编辑模式：点**左栏模式栏**那一行。

    ⚠ 2026-10-21 顶栏那颗下拉删掉了（用户：「模式类型的那个下拉框可以去掉了，
      因为是冗余的」）。切换入口现在是左栏 ModeRail 的 10 行单选 —— 它一直
      可见，不用先点开弹层。锚点 `MODE-RAIL-ITEM <id>` 是按 id 打的，
      顺序变了也不会错位。
    """
    r = rect_of(tap, "MODE-RAIL-ITEM " + mode_id, 4.0)
    if not r:
        return False
    click(hwnd, *center(r))
    time.sleep(1.0)
    return True


def drag(hwnd, src, dst, steps=16):
    """按住左键从 src 拖到 dst（都是客户区坐标）。"""
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


# 把所有 `click(...)` 统一包一层前台聚焦：这个脚本里有十来处点击，
# 逐个改容易漏，而漏掉一处就足以让那段断言"看起来像 UI 坏了"。
_click_raw = click


def click(hwnd, x, y, *args, **kwargs):
    focus_window(hwnd)
    return _click_raw(hwnd, x, y, *args, **kwargs)


def height_stats(path):
    """读 height.r32（列主序 float）→ (min, max, sum, size)。文件不存在返回 None。

    ⚠ 判"有没有抬高"不能只看 max：笔刷在视口中段刷一笔，几乎一定刷不到
    全局最高点（初始正弦地形的最值在别的角落），于是"真的改了"和"完全没
    改"在 max 上看不出区别。sum 是单调的 —— 抬升必然让整片区域的和变大，
    而这个和是 257² 个数的累加，1e-4 的分辨率足够。
    """
    if not os.path.exists(path):
        return None
    raw = open(path, "rb").read()
    n = len(raw) // 4
    vals = struct.unpack("<%df" % n, raw[: n * 4])
    return min(vals), max(vals), sum(vals), n


def shoot(hwnd, name):
    """截图；拿不到图就跳过。

    capture_client 偶尔返回空图（窗口正在重建 swapchain / 刚被 resize），
    以前这里直接 img.save() 抛 ValueError: cannot write empty image，
    整个脚本在断言之后崩掉 —— C/D/E 三组根本跑不到。
    """
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
    # 每次从零开始：地形资产由编辑器自己建，脚本只断言结果
    terrain_dir = os.path.join(ASSETS, "terrain", "landscape")
    for f in ("terrain.vterrain", "height.r32", "splat.rgba8", "holes.r8"):
        p = os.path.join(terrain_dir, f)
        if os.path.exists(p):
            os.remove(p)

    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        # 自动化走 env 起模式，避开"点下拉"这条含 UI 坐标的路径；
        # 下拉本身在 A 组单独点一遍验证。
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

        # ================================================== A 模式入口
        print("\n[A] 模式切换入口")
        # 顶栏那颗下拉必须**不存在**了 —— 删掉的东西被谁加回来，这里第一个红。
        bar = rect_of(tap, "TB-BAR", 4.0)
        tb = rect_of(tap, "TB-EDITMODE", 1.2)
        check(tb is None, "A0 顶栏的模式下拉已删除（TB-EDITMODE 不存在）")

        # "+ Add" 搬到视口工具条：x 必须落在视口那一列里，而且 y 在标签条
        # 下沿那条工具条上（比顶栏低 60px 左右）。
        add = rect_of(tap, "TB-ADD", 4.0)
        vp = rect_of(tap, "VP-RECT", 6.0)
        if add and vp and bar:
            check(add[0] >= vp[0] - 1.0 and add[3] < vp[3] and add[1] > bar[3],
                  "A0b \"+ Add\" 在视口工具条上 (add=(%.0f,%.0f) vp.x=%.0f bar.h=%.0f)"
                  % (add[0], add[1], vp[0], bar[3]))
        else:
            check(False, "A0b 拿不到 TB-ADD / VP-RECT / TB-BAR")

        # ⚠ 这一串必须跟 `editor::EditMode` 的枚举顺序 / `editModeId()` 对齐。
        #   原来是 9 个（到 pcg 为止），2026-10-21 加了 Water 之后漏更新 ——
        #   于是"模式栏齐全"这个断言从来没看过 water 那一行。
        ids = ["select", "landscape", "foliage", "mesh_paint", "modeling",
               "fracture", "brush_edit", "animation", "pcg", "water"]
        rects = {}
        for i in ids:
            rects[i] = rect_of(tap, "MODE-RAIL-ITEM " + i, 2.0)
        missing = [i for i in ids if not rects[i]]
        check(not missing, "A1 左栏 10 行模式齐全（缺: %s）" % (missing or "无"))
        shoot(hwnd, "editmode_rail.png")

        # 点 Modeling：2026-10-20 晚这一项已经落地（六个模式全部实现），
        # 所以这里断言的是"真的切过去了 + 面板开出来了"。
        # ⚠ 不能数 "EDITMODE " 这个子串：日志里的 "MODE-RAIL ..." 之类
        #   也可能含它。真正的"切换"只有 EditorContext::setEditMode 打的那行
        #   "EDITMODE <id> implemented=N"，行首是 `] `。
        if rects.get("modeling"):
            mode_switch = re.compile(r"\] EDITMODE [a-z_]+ implemented=")
            with tap.lock:
                sw_before = sum(1 for l in tap.lines if mode_switch.search(l))
            click(hwnd, *center(rects["modeling"]))
            time.sleep(1.0)
            with tap.lock:
                sw_after = sum(1 for l in tap.lines if mode_switch.search(l))
            check(sw_after > sw_before,
                  "A3a 模式真的切过去了 (切换次数 %d → %d)"
                  % (sw_before, sw_after))
            check(count(tap, "EDITMODE modeling implemented=1") > 0,
                  "A3b Modeling 标记为已实现（implemented=1）")
            check(wait_re(
                tap, r"EDITMODE-PANELS mode=modeling\b.*panel=modeling open=1",
                6.0), "A3c Modeling 面板自动打开")

        # ================================================== B 地形
        print("\n[B] 地形编辑器")
        check(switch_mode(tap, hwnd, "landscape"), "B0 点左栏切到 Landscape")
        time.sleep(0.2)
        check(count(tap, "EDITMODE landscape") > 0, "B1a EDITMODE landscape")
        check(count(tap, "EDITMODE-PANELS mode=landscape landscape=1") > 0,
              "B1b 地形面板自动打开")
        check(count(tap, "LS-CREATE path=terrain/landscape") > 0,
              "B2a 自动建出地形资产（LS-CREATE）")
        check(wait_line(tap, "LS-TERRAIN path=terrain/landscape", 6.0),
              "B2b 地形被加载（LS-TERRAIN）")
        check(os.path.exists(HEIGHT), "B2c height.r32 真的落盘")

        before = height_stats(HEIGHT)
        vp = rect_of(tap, "VP-RECT")
        # 笔心：视口左侧偏下（右侧被地形面板盖住）。这块屏幕位置对着地面。
        bx = vp[0] + (vp[2] - vp[0]) * 0.16
        by = vp[1] + (vp[3] - vp[1]) * 0.72
        drag(hwnd, (bx, by), (bx + 40.0, by - 26.0))
        time.sleep(1.0)
        check(count(tap, "LS-STROKE-BEGIN") > 0, "B3a 笔刷落下（LS-STROKE-BEGIN）")
        check(count(tap, "LS-STROKE-END") > 0, "B3b 笔刷抬起（LS-STROKE-END）")
        after = height_stats(HEIGHT)
        if before and after:
            check(after[2] > before[2] + 1.0e-6,
                  "B4 磁盘高度图被抬高 (sum %.3f → %.3f)"
                  % (before[2], after[2]))
            # 抬升是**有界**的：一笔抬高不应该把整块地形推到 1.0（那说明
            # 归一化被破坏，渲染侧会整体塌成平面）
            check(after[1] <= 1.0 + 1.0e-6,
                  "B4b 高度仍在 0..1 归一化范围内 (max %.4f)" % after[1])
        else:
            check(False, "B4 读不到高度图（before=%s after=%s）"
                  % (before, after))
        shoot(hwnd, "landscape_after_paint.png")

        # ================================================== C 植被
        print("\n[C] 植被编辑器")
        check(switch_mode(tap, hwnd, "foliage"), "C0 点左栏切到 Foliage")
        time.sleep(0.5)
        check(count(tap, "EDITMODE foliage") > 0, "C1a EDITMODE foliage")
        check(count(tap, "EDITMODE-PANELS mode=foliage") > 0,
              "C1b 植被面板自动打开")
        check(count(tap, "FO-PANEL") > 0, "C1c FO-PANEL 矩形已上报")

        # 先切到"树"（5 m，远看也看得见），再撒
        tree_btn = rect_of(tap, "FO-KIND-2", 3.0)
        if tree_btn:
            click(hwnd, *center(tree_btn))
            time.sleep(0.5)
        check(count(tap, "FOLIAGE-KIND tree") > 0, "C2a 切到 Tree 类型")

        vp = rect_of(tap, "VP-RECT")
        px = vp[0] + (vp[2] - vp[0]) * 0.14
        py = vp[1] + (vp[3] - vp[1]) * 0.60
        click(hwnd, px, py)
        time.sleep(1.2)
        check(count(tap, "FOLIAGE-SPAWN kind=tree") > 0,
              "C2b 撒点成功（FOLIAGE-SPAWN kind=tree）")
        cnt = lines_with(tap, "FOLIAGE-COUNT total=")
        total = 0
        if cnt:
            m = re.search(r"total=(\d+)", cnt[-1])
            total = int(m.group(1)) if m else 0
        check(total > 0, "C2c 实例计数 > 0 (total=%d)" % total)
        check(count(tap, "FOLIAGE-ROOT created") > 0,
              "C3 实例挂在 Foliage 分组下")
        shoot(hwnd, "foliage_after_scatter.png")

        # ================================================== D 回到选择
        print("\n[D] 回到选择模式")
        check(switch_mode(tap, hwnd, "select"), "D0 点左栏切回 Select")
        time.sleep(0.2)
        check(count(tap, "EDITMODE select") > 0, "D1a EDITMODE select")
        check(wait_re(tap, r"EDITMODE-PANELS mode=select .*landscape=0 foliage=0",
                      5.0), "D1b 两个编辑器面板都关掉")
        shoot(hwnd, "back_to_select.png")

        # ================================================== E 无报错
        print("\n[E] 日志健康")
        with tap.lock:
            bad = [l for l in tap.lines if re.search(BAD_RE, l)]
        check(not bad, "E1 无 Vulkan 验证层 / ERROR（%d 条）" % len(bad))
        if bad:
            for l in bad[:5]:
                print("      !", l)

        print("\n截图：%s" % OUT_DIR)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except Exception:
            proc.kill()

    print("\n==== %s ====" % ("ALL PASS" if not fails else "FAILED %d" % len(fails)))
    for f in fails:
        print("  -", f)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
