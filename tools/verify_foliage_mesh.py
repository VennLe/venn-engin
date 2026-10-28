#!/usr/bin/env python3
"""植被网格与随机区间验收（2026-10-28）。

为什么要单独一组：
    用户报了两件事：
      ① 「生成的 grass / tree / flower / bush / rock 都太粗糙了」
      ② 「除了默认模型，这几个应该也允许用户自己从本地项目资源目录拖模型
          绑定，然后按算法随机放置；偏移量区间、旋转量区间、缩放区间也要能设」

    这两件事都**不会崩、也不会报错**：
      · 网格退化成粗糙版本 —— 画面还是绿的，只是像一撮毛；
      · 拖进来的模型没生效 —— 撒出来仍是程序化草，用户以为拖拽没反应；
      · 区间没生效 —— 还是随机撒，只是范围不对。
    所以判据必须落在**数字**上：三角数、绑定的资产路径、实测缩放/偏移。

验什么：
  A  内置五种网格的**三角数**（精细化前：草 6 / 花 10 / 灌木 ~80 / 树 32 / 石头 108）
     → 现在分别是 72 / 72 / 512 / 830 / 252 量级，断言给的是"不许退回旧版"的下限。
  B  自定义网格绑定：`MYVK_FOLIAGE_MESH=models/box01.glb` 起一个会话 →
     `FOLIAGE-ENV mesh=…` + `FOLIAGE-TYPE-MESH src=…` + 撒点日志 `mesh=…` 三处一致，
     且 placed>0（模型真的被当成实例网格用上了）。
  C  随机区间：`MYVK_FOLIAGE_SCALE=2,2` + `MYVK_FOLIAGE_OFFSET=6,6,0` →
     撒点日志里 `scale=2.00..2.00`（区间被**精确**读取并应用）、
     `maxOff > 11`（6 m 偏移把实例推到了 10 m 笔刷圆之外）。
  D  对照（与 A 同一个默认会话）：`scale=0.70..1.50` 有跨度、`maxOff <= 10.5`。
     ⚠ 没有 D 就分不清 C 组那两条是"设置生效"还是"本来就那样"。

不验什么：
  * 旋转区间：日志里没有单株欧拉角（要读场景实体），只用肉眼/截图看；
    代码路径与缩放/偏移是**同一个 rnd() 调用序列**（见 samplePlacement），
    缩放与偏移两条都验住了，旋转按同一函数顺带保证。
  * 拖拽手势本身：`FO-MESH` 槽的拖放走 ImGui 的 AcceptDragDropPayload，
    B 组用环境钩子设同一个字段（`TypeSettings::meshPath`）绕开鼠标合成 ——
    "拖到槽里"这一步由 `verify_asset_drag` 那套 CB→视口 拖拽间接覆盖。

用法：
    python tools/verify_foliage_mesh.py
"""

import ctypes
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window                              # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors             # noqa: E402
from verify_asset_drag import force_foreground                     # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EDITOR = os.path.join(BIN, "Editor.exe")

user32 = ctypes.windll.user32
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
MK_LBUTTON = 0x0001

RECT_RE = re.compile(r"\b([A-Z][A-Z0-9_-]*(?: [a-z_]+)?)=\(([-\d.]+),([-\d.]+)\)-"
                     r"\(([-\d.]+),([-\d.]+)\)")

results = []


def check(ok, msg):
    results.append((msg, bool(ok)))
    print("    %-64s %s" % (msg, "PASS" if ok else "FAIL"))
    return bool(ok)


# ------------------------------------------------------------------ 小工具
def count(tap, needle):
    with tap.lock:
        return sum(1 for l in tap.lines if needle in l)


def lines_with(tap, needle):
    with tap.lock:
        return [l for l in tap.lines if needle in l]


def rect_of(tap, tag, timeout=6.0):
    end = time.time() + timeout
    pat = re.compile(re.escape(tag) + r"=\(([-\d.]+),([-\d.]+)\)-\(([-\d.]+),([-\d.]+)\)")
    while time.time() < end:
        with tap.lock:
            for l in tap.lines:
                m = pat.search(l)
                if m:
                    return tuple(float(m.group(i)) for i in range(1, 5))
        time.sleep(0.1)
    return None


def _lparam(x, y):
    return (int(x) & 0xFFFF) | ((int(y) & 0xFFFF) << 16)


def _c2s(hwnd, x, y):
    class POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]
    pt = POINT(int(x), int(y))
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y


def click(hwnd, x, y):
    lp = _lparam(x, y)
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp)
    user32.SetCursorPos(*_c2s(hwnd, x, y))
    time.sleep(0.18)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp)
    time.sleep(0.10)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)
    time.sleep(0.55)


def click_rect(hwnd, r, what):
    if not r:
        print("      ! %s 没有矩形锚点，跳过" % what)
        return False
    click(hwnd, (r[0] + r[2]) * 0.5, (r[1] + r[3]) * 0.5)
    return True


def brush_point(tap):
    """视口里一个"既在地形上、又没被左栏面板盖住"的落点。"""
    vp = rect_of(tap, "VP-RECT")
    if not vp:
        return None
    pnl = rect_of(tap, "FO-PANEL", 1.0)
    bx = vp[0] + 60.0
    if pnl and (pnl[0] - vp[0]) > 90.0:
        bx = min(bx, pnl[0] - 80.0)
    bx = max(bx, vp[0] + 24.0)
    by = vp[1] + (vp[3] - vp[1]) * 0.60
    return bx, by


def launch(extra=None, wait_frames=3.2):
    """起一个编辑器：地形自动建、初始在 Landscape，之后由调用方切到 Foliage。"""
    env = dict(os.environ)
    env.update({
        "MYVK_LOG_RECTS": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_LIGHT_GIZMOS": "0",
        "MYVK_EDITMODE": "landscape",
        "MYVK_LANDSCAPE_AUTOCREATE": "1",
    })
    # 清掉可能从外部继承的植被钩子，避免"上一组的环境"漏到这一组
    for k in ("MYVK_FOLIAGE_KIND", "MYVK_FOLIAGE_SCALE", "MYVK_FOLIAGE_OFFSET",
              "MYVK_FOLIAGE_ROT", "MYVK_FOLIAGE_MESH"):
        env.pop(k, None)
    if extra:
        env.update(extra)
    proc = subprocess.Popen([EDITOR], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", timeout=60)
    if hwnd:
        force_foreground(hwnd)
    time.sleep(wait_frames)
    return proc, tap, hwnd


def stop(proc):
    try:
        proc.terminate()
        proc.wait(timeout=8)
    except Exception:
        proc.kill()


def enter_foliage(hwnd, tap):
    r = rect_of(tap, "MODE-RAIL-ITEM foliage", 6.0)
    if not click_rect(hwnd, r, "左栏 Foliage 行"):
        return False
    time.sleep(1.2)
    return count(tap, "EDITMODE foliage") > 0


def scatter(hwnd, tap, times=2):
    """在视口刷两笔（两点不同，避免"恰好落在地形外"的偶发）。"""
    pt = brush_point(tap)
    if not pt:
        return False
    click(hwnd, pt[0], pt[1])
    time.sleep(0.7)
    if times > 1:
        click(hwnd, pt[0] + 34.0, pt[1] - 22.0)
        time.sleep(0.7)
    return True


# ------------------------------------------------------------------ A/D 内置网格
def group_builtin():
    print("\n[A] 内置五种网格的三角数（精细化不许退回旧版）")
    proc, tap, hwnd = launch()
    try:
        if not hwnd:
            check(False, "A0 编辑器窗口出现")
            return
        if not enter_foliage(hwnd, tap):
            check(False, "A0 切到 Foliage 模式")
            return

        # 依次点五个类型：每切一次 drawMeshSlot 会 typeMesh() 一次，
        # 首次解析会打 `FOLIAGE-TYPE-MESH`（src=builtin tris=N）。
        ids = ["grass", "bush", "tree", "flower", "rock"]
        for i in range(5):
            click_rect(hwnd, rect_of(tap, "FO-KIND-%d" % i, 4.0), ids[i])
            time.sleep(0.5)

        # 下限取"旧版三角形数的 5~10 倍"：低于它就说明有人把形状改回去了。
        floor = {"grass": 60, "flower": 60, "bush": 400, "tree": 700, "rock": 200}
        for k in ids:
            got = None
            for l in lines_with(tap, "FOLIAGE-TYPE-MESH kind=" + k + " "):
                m = re.search(r"src=(\S+) tris=(\d+)", l)
                if m:
                    got = m
            if not got:
                check(False, "A1 %-6s 有 FOLIAGE-TYPE-MESH 日志（tris）" % k)
                continue
            tris = int(got.group(2))
            check(got.group(1) == "builtin" and tris >= floor[k],
                  "A1 %-6s 内置网格 tris=%d（下限 %d，src=%s）"
                  % (k, tris, floor[k], got.group(1)))

        # ---- D 对照：默认区间下的随机跨度 ----
        print("\n[D] 对照：默认随机区间")
        if not scatter(hwnd, tap):
            check(False, "D0 找到视口落点")
        spawn = lines_with(tap, "FOLIAGE-SPAWN kind=")
        if not spawn:
            check(False, "D1 撒点产生了 FOLIAGE-SPAWN")
        else:
            last = spawn[-1]
            m = re.search(r"scale=([\d.]+)\.\.([\d.]+)", last)
            o = re.search(r"maxOff=([\d.]+)", last)
            if m and o:
                lo, hi = float(m.group(1)), float(m.group(2))
                off = float(o.group(1))
                check(hi - lo > 0.15,
                      "D1 默认区间有随机跨度 scale=%.2f..%.2f" % (lo, hi))
                check(off <= 10.5,
                      "D2 默认没有偏移（maxOff=%.2f <= 笔刷半径 10）" % off)
            else:
                check(False, "D1 撒点日志带 scale/maxOff")
    finally:
        stop(proc)


# ------------------------------------------------------------------ B 自定义网格
def group_custom_mesh():
    print("\n[B] 自定义网格：把项目里的模型绑到植被类型上")
    model = "models/box01.glb"
    if not os.path.exists(os.path.join(BIN, "assets", model)):
        check(False, "B0 资产存在: %s" % model)
        return
    proc, tap, hwnd = launch({"MYVK_FOLIAGE_MESH": model,
                              "MYVK_FOLIAGE_KIND": "tree"})
    try:
        if not hwnd:
            check(False, "B0 编辑器窗口出现")
            return
        if not enter_foliage(hwnd, tap):
            check(False, "B0 切到 Foliage 模式")
            return
        # ⚠ `FOLIAGE-ENV` 是 applyEnvOverrides() 打的，它挂在 sync() 上，
        #   而 sync() 由**植被面板绘制**驱动 —— 所以必须先进 Foliage 模式
        #   才看得到这一行（第一版在进模式之前断言，白红了一次）。
        envline = lines_with(tap, "FOLIAGE-ENV ")
        check(bool(envline) and ("mesh=" + model) in envline[-1],
              "B1 钩子被读进来（FOLIAGE-ENV mesh=%s）" % model)
        tm = lines_with(tap, "FOLIAGE-TYPE-MESH kind=tree ")
        ok_src = bool(tm) and ("src=" + model) in tm[-1] and "tris=0 " not in tm[-1]
        check(ok_src, "B2 tree 用的是自定义网格（FOLIAGE-TYPE-MESH tris>0）")
        if tm:
            print("      ·", tm[-1].strip()[-110:])

        if not scatter(hwnd, tap):
            check(False, "B3 找到视口落点")
            return
        spawn = lines_with(tap, "FOLIAGE-SPAWN kind=tree")
        if spawn:
            last = spawn[-1]
            m = re.search(r"placed=(\d+)", last)
            placed = int(m.group(1)) if m else 0
            check(placed > 0 and ("mesh=" + model) in last,
                  "B3 撒点用的是自定义网格（placed=%d mesh=%s）" % (placed, model))
            print("      ·", last.strip()[-130:])
        else:
            check(False, "B3 撒点产生了 FOLIAGE-SPAWN kind=tree")
    finally:
        stop(proc)


# ------------------------------------------------------------------ C 随机区间
def group_ranges():
    print("\n[C] 随机区间：缩放固定 2.00、偏移 ±6 m")
    proc, tap, hwnd = launch({"MYVK_FOLIAGE_SCALE": "2.0,2.0",
                              "MYVK_FOLIAGE_OFFSET": "6,6,0",
                              "MYVK_FOLIAGE_ROT": "0,0,180"})
    try:
        if not hwnd:
            check(False, "C0 编辑器窗口出现")
            return
        if not enter_foliage(hwnd, tap):
            check(False, "C0 切到 Foliage 模式")
            return
        envline = lines_with(tap, "FOLIAGE-ENV ")
        ok_env = bool(envline) and "scale=2.00..2.00" in envline[-1] and \
            "offset=6.00,6.00,0.00" in envline[-1]
        check(ok_env, "C1 钩子被读进来（scale=2.00..2.00 offset=6,6,0）")
        if not scatter(hwnd, tap, times=3):
            check(False, "C2 找到视口落点")
            return
        spawn = [l for l in lines_with(tap, "FOLIAGE-SPAWN kind=") if "placed=0" not in l]
        if not spawn:
            check(False, "C2 撒点产生了实例")
            return
        last = spawn[-1]
        m = re.search(r"scale=([\d.]+)\.\.([\d.]+)", last)
        o = re.search(r"maxOff=([\d.]+)", last)
        if m:
            lo, hi = float(m.group(1)), float(m.group(2))
            # 区间是**常量** 2.0：实测必须精确等于它（上下浮动只允许浮点误差）。
            # 这条比"跨度大于某值"强得多 —— 它证明读的是**用户设的数**。
            check(abs(lo - 2.0) < 0.02 and abs(hi - 2.0) < 0.02,
                  "C2 缩放区间被精确应用（scale=%.2f..%.2f，设的是 2.0..2.0）"
                  % (lo, hi))
        else:
            check(False, "C2 撒点日志带 scale")
        if o:
            off = float(o.group(1))
            # 笔刷半径默认 10 m，偏移 ±6 → 必然会有点落在 10 m 之外。
            check(off > 11.0,
                  "C3 偏移区间生效（maxOff=%.2f > 11，笔刷本身只有 10 m）" % off)
        else:
            check(False, "C3 撒点日志带 maxOff")
        print("      ·", last.strip()[-130:])
    finally:
        stop(proc)


def main():
    if not os.path.exists(EDITOR):
        print("找不到可执行文件，请先构建：", EDITOR)
        return 1
    kill_stale_editors()
    try:
        group_builtin()
        group_custom_mesh()
        group_ranges()
    finally:
        kill_stale_editors()

    fails = [n for n, ok in results if not ok]
    print()
    if fails:
        print("失败 %d 项：" % len(fails))
        for f in fails:
            print("  -", f)
        print("结果: FAIL")
        return 1
    print("==== 全部断言 PASS（%d 条）====" % len(results))
    print("结果: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
