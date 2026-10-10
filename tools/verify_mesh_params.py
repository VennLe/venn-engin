"""端到端验证：Inspector 里 Mesh / Light 的**差异化参数** + 鼠标拖拽调值。

对应需求："添加 mesh 或 light 后，右侧应该展示该资产特有的属性参数，不同
mesh / 不同 light 的参数不同，并且可以用鼠标拖拽调值（像 Blender）。"

难点在于"不同资产参数不同"这件事很难用截图证明 —— 截一张图只能看出"有个
面板"，看不出"Sphere 比 Cube 多两个控件"。所以面板里埋了一行**签名日志**
（只在内容变化时打，不刷屏）：

    MESH-PARAMS  kind=Sphere labels=Radius,Segments,Rings
    LIGHT-PARAMS type=Spot   labels=Color,Intensity,Range,Direction,InnerCone,
                                    OuterCone,Enabled

脚本据此做**结构性断言**：三种图元 / 三种灯的 labels 集合必须互不相同，且
与组件里真实存在的字段一一对应。

拖拽走真实鼠标：每个参数控件的矩形由 logRect 打出（MESH-PARAM Size /
MESH-PARAM Segments ...），脚本按矩形中心按下并水平拖动，然后断言：

  · 日志出现 `primitive rebuild: 'Cube' cube size=... -> N verts (key ...)`
  · 新值 != 旧值
  · 缓存键从 builtin/cube/s1.0000 变成 builtin/cube/s1.8xxx

最后一条最关键 —— 它证明**几何真的被重新生成了**，而不只是数字变了。
Sphere 的 Segments 拖两次，用两次的顶点数对比，直接证明"参数 -> 几何"。

用法：
    python tools/verify_mesh_params.py
"""

import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32   # noqa: E402
from verify_add_menu import (LogTap, click, esc, kill_stale_editors,  # noqa: E402
                             press_shift_a)
from verify_asset_drag import force_foreground, move_to           # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = r"E:\code\cpp\_venn_verify"

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

# 分组： 1 名字 / 2 shape / 3 size / 4 radius / 5 seg / 6 rings / 7 verts / 8 key
REBUILD_RE = re.compile(
    r"primitive rebuild: '([^']*)' (\S+) size=([\d.]+) radius=([\d.]+) "
    r"seg=(-?\d+) rings=(-?\d+) -> (\d+) verts \(key (\S+)\)")


def find_line(tap, pattern, timeout=6.0, where=None):
    """在已收集的日志里找**最后一条**匹配的行，返回 re.Match 或 None。

    where: 可选的额外谓词，用来在同一类日志里挑出特定实体（Cube / Sphere）。
    """
    rx = re.compile(pattern)
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for line in reversed(tap.lines):
                m = rx.search(line)
                if m and (where is None or where(m)):
                    return m
        time.sleep(0.1)
    return None


def is_shape(shape):
    """REBUILD_RE 的 group(2) 是 MeshSource 的 shape 名（cube / sphere / ...）"""
    return lambda m: m.group(2) == shape


def wait_rect(tap, tag, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r:
            return r
        time.sleep(0.15)
    return None


def menu_seq(tap):
    """当前日志里出现过的最大菜单序号（0 = 还没打开过）。

    编辑器每次 Shift+A 打开菜单都会 ++ 序号，并把序号拼进所有弹出物矩形的
    tag（VP-ADD-Mesh#3）。这样"这一次打开"和"上一次打开"在日志里是可区分
    的 —— 否则 logRect 的按矩形去抖会让第二次打开一行都不打，脚本只能读到
    陈旧坐标。
    """
    rx = re.compile(r"VP-ADD-OPEN seq=(\d+)")
    with tap.lock:
        nums = [int(m.group(1)) for m in (rx.search(l) for l in tap.lines) if m]
    return max(nums) if nums else 0


def add_via_menu(tap, hwnd, vx, vy, parent, item, item_name):
    """视口里 Shift+A → <parent> → <item>，全程按**本次序号**限定矩形。

    父项 tag  : VP-ADD-Mesh#<seq>
    子项 tag  : VP-ADD-Cube#<seq>
    序号保证读到的是这一次打开菜单的真实坐标，不可能是上一轮留下的。
    """
    needle = "added '%s' via Shift+A menu" % item_name
    if tap.has_line(needle):
        return True

    for attempt in range(4):
        base = menu_seq(tap)
        press_shift_a(hwnd)

        # 等这一次的打开序号出现
        seq, end = None, time.time() + 3.0
        while time.time() < end:
            n = menu_seq(tap)
            if n > base:
                seq = n
                break
            time.sleep(0.1)
        if seq is None:
            continue

        win = wait_rect(tap, "VP-ADD-WIN#%d" % seq, 2.0)
        par = wait_rect(tap, "VP-ADD-%s#%d" % (parent, seq), 2.0)
        if not win or not par:
            esc(hwnd)
            continue

        # 悬停父项 → 子菜单展开 → 本次的子项矩形才会出现
        move_to(hwnd, (par[0] + par[2]) * 0.5, (par[1] + par[3]) * 0.5)
        it = wait_rect(tap, "VP-ADD-%s#%d" % (item, seq), 3.0)
        if not it:
            esc(hwnd)
            continue

        click(hwnd, (it[0] + it[2]) * 0.5, (it[1] + it[3]) * 0.5)
        time.sleep(0.6)
        if tap.has_line(needle):
            return True
        esc(hwnd)
        move_to(hwnd, vx, vy)
        time.sleep(0.3)

    return False


def drag_h(hwnd, rect, dx, steps=10, label=""):
    """在 rect 中心按下，水平拖 dx 像素。

    DragFloat/DragInt 是按**逐帧鼠标位移**累积改值的，所以要分多步慢慢移：
    一次性跳过去的话中间帧没有 MouseDelta，控件可能只吃到很小一部分。
    """
    cx = (rect[0] + rect[2]) * 0.5
    cy = (rect[1] + rect[3]) * 0.5
    print("  drag %-16s at (%.0f,%.0f) dx=%+d" % (label, cx, cy, dx))

    force_foreground(hwnd)
    time.sleep(0.2)
    move_to(hwnd, cx, cy)
    time.sleep(0.25)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.2)
    for i in range(1, steps + 1):        # 第一步就超过 ImGui 的 6px 拖拽阈值
        move_to(hwnd, cx + dx * i // steps, cy)
        time.sleep(0.09)
    time.sleep(0.25)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.7)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out", default=os.path.join(OUT_DIR, "mesh_params.png"))
    ap.add_argument("--size", default="1600x900")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

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
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        print("  %s  %s%s" % ("PASS" if ok else "FAIL", name,
                              ("  [%s]" % detail) if detail else ""))
        if not ok:
            fails.append("%s  %s" % (name, detail))

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
        time.sleep(0.8)

        vp = tap.last_rect("VP-RECT")
        if not vp:
            print("缺 VP-RECT")
            return 3
        vx, vy = int((vp[0] + vp[2]) * 0.5), int((vp[1] + vp[3]) * 0.5)

        # ==================== 1. Cube ====================
        print("\n[1] Shift+A -> Mesh > Cube")
        move_to(hwnd, vx, vy)
        time.sleep(0.4)
        if not add_via_menu(tap, hwnd, vx, vy, "Mesh", "Cube", "Cube"):
            print("Shift+A 菜单没走通，中止")
            return 4
        check("添加 Cube", True)

        # Mesh 段默认折叠 —— 展开它才有参数可读
        hdr = wait_rect(tap, "INS-MESH-HEADER", 6.0)
        if hdr:
            click(hwnd, (hdr[0] + hdr[2]) * 0.5, (hdr[1] + hdr[3]) * 0.5)
            time.sleep(0.6)
        else:
            check("找到 Mesh 标题栏", False, "INS-MESH-HEADER 没出现")

        m = find_line(tap, r"MESH-PARAMS kind=(\S+) labels=(\S+)", 6.0)
        check("Cube 参数签名", bool(m) and m.group(1) == "Cube",
              m.group(0) if m else "没有 MESH-PARAMS 日志")
        check("Cube 只有 Size 一项可调",
              bool(m) and m.group(2) == "Size",
              "labels=%s" % (m.group(2) if m else "?"))

        sizeRect = wait_rect(tap, "MESH-PARAM Size", 6.0)
        if not sizeRect:
            with tap.lock:
                dump = [l for l in tap.lines if "MESH-PARAM" in l]
            check("拿到 Size 控件矩形", False, "现有: %s" % dump[-3:])
        else:
            check("拿到 Size 控件矩形", True, str(sizeRect))
            move_to(hwnd, vx, vy)
            time.sleep(0.2)
            drag_h(hwnd, sizeRect, +90, label="Size")

            m2 = find_line(tap, REBUILD_RE.pattern, 6.0, where=is_shape("cube"))
            if not m2:
                check("拖 Size 后几何重建", False, "没有 rebuild 日志")
            else:
                check("拖 Size 后几何重建", True, m2.group(0))
                size = float(m2.group(3))
                check("Size 值真的变了 (原 1.000)", size > 1.05,
                      "size=%.4f" % size)
                check("缓存键随参数变化",
                      "s1.0000" not in m2.group(8), "key=%s" % m2.group(8))

            move_to(hwnd, 40, 40)
            time.sleep(0.5)
            shot = os.path.join(os.path.dirname(args.out), "mesh_params_cube.png")
            capture_client(hwnd)[0].save(shot)
            print("  截图: %s" % shot)

        # ==================== 2. Sphere ====================
        print("\n[2] Shift+A -> Mesh > Sphere")
        move_to(hwnd, vx, vy)
        time.sleep(0.4)
        if not add_via_menu(tap, hwnd, vx, vy, "Mesh", "Sphere", "Sphere"):
            check("添加 Sphere", False, "菜单没走通")
        else:
            check("添加 Sphere", True)

        m = find_line(tap, r"MESH-PARAMS kind=Sphere labels=(\S+)", 6.0)
        check("Sphere 参数签名", bool(m), m.group(0) if m else "没有日志")
        check("Sphere 比 Cube 多出 Segments / Rings",
              bool(m) and set(m.group(1).split(",")) ==
              {"Radius", "Segments", "Rings"},
              "labels=%s" % (m.group(1) if m else "?"))

        segRect = wait_rect(tap, "MESH-PARAM Segments", 6.0)
        if not segRect:
            check("拿到 Segments 控件矩形", False, "缺失")
        else:
            check("拿到 Segments 控件矩形", True, str(segRect))
            drag_h(hwnd, segRect, +70, label="Segments#1")
            r1 = find_line(tap, REBUILD_RE.pattern, 6.0, where=is_shape("sphere"))
            if not r1:
                check("拖 Segments 后几何重建", False, "没有 rebuild 日志")
            else:
                check("拖 Segments 后几何重建", True, r1.group(0))
                seg1, verts1 = int(r1.group(5)), int(r1.group(7))
                check("Segments 变了 (默认 48)", seg1 != 48, "seg=%d" % seg1)
                check("缓存键带上细分参数",
                      ("g%d" % seg1) in r1.group(8), "key=%s" % r1.group(8))

                # 再拖一次：证明"顶点数确实跟着参数走"（几何真的重建了）
                drag_h(hwnd, segRect, +70, label="Segments#2")
                r2 = find_line(tap, REBUILD_RE.pattern, 6.0,
                               where=is_shape("sphere"))
                if not r2:
                    check("两次拖拽生成不同几何", False, "第二次没有 rebuild")
                else:
                    seg2, verts2 = int(r2.group(5)), int(r2.group(7))
                    check("两次拖拽生成不同几何",
                          seg2 != seg1 and verts2 != verts1,
                          "seg %d->%d   verts %d->%d"
                          % (seg1, seg2, verts1, verts2))

            move_to(hwnd, 40, 40)
            time.sleep(0.5)
            shot = os.path.join(os.path.dirname(args.out),
                                "mesh_params_sphere.png")
            capture_client(hwnd)[0].save(shot)
            print("  截图: %s" % shot)

        # ==================== 3. 三种灯 ====================
        print("\n[3] Shift+A -> Light > Point / Spot / Directional")
        want = [
            ("Point Light", "PointLight", "Point",
             {"Color", "Intensity", "Range", "Enabled"}),
            ("Directional Light", "DirectionalLight", "Directional",
             {"Direction", "Color", "Intensity", "Ambient", "CastsShadow"}),
            ("Spot Light", "SpotLight", "Spot",
             {"Color", "Intensity", "Range", "Direction",
              "InnerCone", "OuterCone", "Enabled"}),
        ]
        light_sets = {}
        for name, tag, typename, expect in want:
            move_to(hwnd, vx, vy)
            time.sleep(0.4)
            if not add_via_menu(tap, hwnd, vx, vy, "Light", tag, name):
                check("添加 %s" % name, False, "菜单没走通")
                continue
            check("添加 %s" % name, True)
            mm = find_line(tap,
                           r"LIGHT-PARAMS type=%s labels=(\S+)" % typename, 6.0)
            if not mm:
                check("%s 参数签名" % typename, False, "没有 LIGHT-PARAMS 日志")
                continue
            got = set(mm.group(1).split(","))
            light_sets[typename] = got
            check("%s 参数字段正确" % typename, got == expect, "got=%s" % sorted(got))

        if len(light_sets) == 3:
            uniq = {frozenset(v) for v in light_sets.values()}
            check("三种灯的参数集合互不相同", len(uniq) == 3,
                  " ; ".join("%s=%d项" % (k, len(v))
                             for k, v in light_sets.items()))

        # ==================== 4. 截图 ====================
        move_to(hwnd, 40, 40)
        time.sleep(0.5)
        img, ok = capture_client(hwnd)
        img.save(args.out)
        print("\n截图 ok=%s size=%s -> %s" % (ok, img.size, args.out))

        print("\n---- 检查项 ----")
        for name, ok, _ in checks:
            print("  %s  %s" % ("PASS" if ok else "FAIL", name))
        if fails:
            print("\nRESULT: FAIL")
            for f in fails:
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
