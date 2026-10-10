"""端到端验证：在选中物体上按**鼠标右键**弹出的「添加碰撞体」菜单。

用户要的形态（原文）：
    "当选中一个物体时，并且在该物体上按下鼠标右键打开一个菜单，其中有：
     添加碰撞体功能 …… 有两种碰撞体结构，一个是按照物体几何结构，根据当前
     业内最优秀的算法找到一个，另一种就是直接给一个胶囊体 …… 创建时默认是
     刚好包裹住物体的"

所以这里断言：

  1. **右键点物体 → 弹出菜单**：`VP-OBJ-OPEN seq=N` + `VP-OBJ-WIN` 矩形。
     同时验证"右键拖拽 ≠ 右键点击"：按下与松开之间移动超过 6px **不能**
     弹菜单（否则右键转视角会一路弹菜单）。
  2. **右键点空白 → 不弹菜单**（菜单只对"选中物体上的右键"响应）。
  3. **菜单里有两个碰撞体结构**：Add Collision Body ▸
     `Convex Hull (QuickHull)` / `Capsule`，两个 item 矩形都在。
  4. **凸包 = 按几何结构求得**：点 Convex Hull 后日志出现
     `collision: added convex collider ... points>=4 edges>=6`
     （QuickHull 至少一个四面体；立方体就是 8 点 18 边）。
  5. **胶囊**：点 Capsule 后 `points=0 radius>0`。
  6. **创建时默认刚好包裹住物体**：
     · 凸包 —— `collision: fit 'name' mesh=(...) collider=(...) ratio=(...)`
       三个 ratio 都必须 ≈ 1（凸包是网格顶点的子集，AABB 只会更小一点点）
     · 胶囊 —— 三个 ratio 都必须 ≥ 1（半径取的是外接圆，要**盖住** AABB）
  7. **可视化的碰撞框**：`COLLIDER <name>` 矩形（ImGui 线框探针）出现，
     且 `COLLIDER-SIG` 报出 shape / points / edges / solid。

用法：
    python tools/verify_collider_menu.py
    python tools/verify_collider_menu.py --keep-scene    # 保留临时场景
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32        # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors               # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground     # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010

ADD_RE = re.compile(
    r"collision: added (convex|capsule) collider to '([^']*)' "
    r"points=(\d+) edges=(\d+) radius=([\d.]+) halfHeight=([\d.]+) "
    r"pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")
FIT_RE = re.compile(
    r"collision: fit '([^']*)' shape=(\S+) "
    r"mesh=\(([\d.]+),([\d.]+),([\d.]+)\) "
    r"collider=\(([\d.]+),([\d.]+),([\d.]+)\) "
    r"ratio=\(([\d.-]+),([\d.-]+),([\d.-]+)\)")
SIG_RE = re.compile(
    r"COLLIDER-SIG '([^']*)' shape=(\S+) points=(\d+) edges=(\d+) solid=(\d)")
OPEN_RE = re.compile(r"VP-OBJ-OPEN seq=(\d+) at=\((-?\d+),(-?\d+)\)")


# ------------------------------------------------------------------ 场景

def make_scene(path):
    """一个 1m 立方体（中心抬到 z=0.5，正好坐在地面上）。
    相机从 -Y 平视立方体中心。"""
    scene = {
        "version": 1,
        "generator": "verify_collider_menu",
        "camera": {"target": [0, 0, 0.5], "yaw": 1.5707963, "pitch": 0.0,
                   "distance": 8.0},
        "entities": [
            {
                "name": "ColliderCube",
                "transform": {"position": [0.0, 0.0, 0.5],
                              "rotation": [0.0, 0.0, 0.0],
                              "scale": [1.0, 1.0, 1.0]},
                "parent": -1, "visible": True, "castShadow": True,
                "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                         "name": "menu_test_cube"},
                "material": {"kind": "procedural", "name": "menu_test_mat",
                             "baseColor": [0.85, 0.35, 0.30, 1.0],
                             "metallic": 0.0, "roughness": 0.55,
                             "normalScale": 0.0, "ao": 1.0,
                             "emissive": [0, 0, 0],
                             "doubleSided": False, "alphaBlend": False},
            },
        ],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, indent=2)
    return path


# ------------------------------------------------------------------ 输入

def move(hwnd, cx, cy):
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)


def click_left(hwnd, cx, cy):
    force_foreground(hwnd)
    move(hwnd, cx, cy)
    time.sleep(0.25)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.5)


def right_click(hwnd, cx, cy, drift=0):
    """右键"点击"：按下与松开之间只移动 drift 像素（<6 才算点击）。"""
    force_foreground(hwnd)
    move(hwnd, cx, cy)
    time.sleep(0.25)
    user32.mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    if drift:
        for i in range(1, 5):
            move(hwnd, cx + drift * i // 4, cy)
            time.sleep(0.05)
    user32.mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0)
    time.sleep(0.6)


def hover(hwnd, cx, cy, wait=0.6):
    force_foreground(hwnd)
    move(hwnd, cx, cy)
    time.sleep(wait)


def center(rect):
    return (int((rect[0] + rect[2]) * 0.5), int((rect[1] + rect[3]) * 0.5))


# ------------------------------------------------------------------ 主流程

def launch(exe, extra_env, size):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update(extra_env)
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = None
    for _ in range(40):
        hwnd = find_window("Venn Editor")
        if hwnd:
            break
        time.sleep(0.25)
    if not hwnd:
        proc.kill()
        return None, None, tap
    user32.ShowWindow(hwnd, 9)
    time.sleep(1.5)
    tw, th = (int(v) for v in size.lower().split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)
    return proc, hwnd, tap


def close(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


def last_match(pattern, tap):
    with tap.lock:
        found = None
        for line in tap.lines:
            m = pattern.search(line)
            if m:
                found = m
        return found


def wait_match(pattern, tap, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            found = None
            for line in tap.lines:
                m = pattern.search(line)
                if m:
                    found = m
            if found:
                return found
        time.sleep(0.1)
    return None


def wait_open_seq(tap, seq, timeout=6.0):
    """等待"第 seq 次打开菜单"的日志。"""
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for line in tap.lines:
                m = OPEN_RE.search(line)
                if m and int(m.group(1)) == seq:
                    return m
        time.sleep(0.1)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    ap.add_argument("--entity", default="ColliderCube")
    ap.add_argument("--keep-scene", action="store_true")
    ap.add_argument("--out", default=os.path.join(OUT_DIR,
                                                  "collider_menu.png"))
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    tmpdir = tempfile.mkdtemp(prefix="venn_collmenu_")
    scene_path = make_scene(os.path.join(tmpdir, "collider_menu.json"))
    print("测试场景:", scene_path)

    fails = []
    notes = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if (detail and not ok) else ""))
        if not ok:
            fails.append("%s %s" % (name, detail))

    kill_stale_editors()
    proc, hwnd, tap = launch(exe, {
        "MYVK_EDITOR_SCENE": scene_path,
        "MYVK_EDITOR_SELECT": args.entity,
        "MYVK_EDITOR_GIZMO": "move",
        # 从 -Y 平视立方体中心（世界是 Z-up；yaw=90° → forward = +Y）
        "MYVK_EDITOR_CAM": "0,-6,0.5,90,0",
    }, args.size)
    if not hwnd:
        print("未找到窗口")
        return 2

    try:
        # ---------- 0) 定位：手柄原点 = 选中物体的屏幕位置 ----------
        def object_screen(timeout=12.0):
            """当前帧选中物体（手柄原点）的屏幕位置。

            相机一被右键拖拽转过，物体在屏幕上的位置就变了，所以每次
            右键**之前**都要重新取一次 —— 拿旧坐标去点必然落空。
            logRect 只在矩形真的动了才重打一行，所以"没动"时 last_rect
            返回的仍然是最新值。
            """
            end = time.time() + timeout
            r = None
            while time.time() < end:
                r = tap.last_rect("GZ-ORIGIN")
                if r:
                    return center(r)
                time.sleep(0.1)
            return center(r) if r else None

        def open_count():
            with tap.lock:
                return sum(1 for l in tap.lines if "VP-OBJ-OPEN" in l)

        ox, oy = object_screen()
        if ox is None:
            print("拿不到 GZ-ORIGIN（选中物体没有手柄？）")
            return 3
        vp = tap.last_rect("VP-RECT")
        print("物体屏幕位置 = (%d,%d)" % (ox, oy))

        # ---------- 1) 守卫：右键点**空白处**不弹菜单 ----------
        print("\n[0] 守卫")
        n0 = open_count()
        if vp:
            right_click(hwnd, int(vp[2] - 30), int(vp[3] - 30))
            check("右键点空白处不弹菜单", open_count() == n0)
            move(hwnd, 5, 5)
            time.sleep(0.3)

        # ---------- 2) 正例：右键点物体 → 弹出菜单 ----------
        print("\n[1] 右键点在选中的物体上 → 弹出菜单")
        ox, oy = object_screen()
        n0 = open_count()
        right_click(hwnd, ox, oy)
        op = wait_open_seq(tap, 1, 6.0)
        check("右键点物体弹出菜单（VP-OBJ-OPEN seq=1）", op is not None)
        win = tap.last_rect("VP-OBJ-WIN")
        check("菜单窗口矩形（VP-OBJ-WIN）", win is not None, str(win))
        if not win:
            capture_client(hwnd)[0].save(
                args.out.replace(".png", "_no_menu.png"))
            print("物体屏幕位置 = (%d,%d)  VP=%s" % (ox, oy, vp))
            return 4
        capture_client(hwnd)[0].save(args.out.replace(".png", "_open.png"))

        # ---------- 3) 菜单项结构 ----------
        print("\n[2] 菜单里有两个碰撞体结构")
        add_rect = tap.last_rect("VP-OBJ-Add")
        check("菜单含 Add Collision Body 子菜单项", add_rect is not None,
              str(add_rect))
        if add_rect:
            hover(hwnd, *center(add_rect), wait=0.7)
            # 子菜单可能因为 hover 延迟没展开，最多试 3 次
            for _ in range(3):
                if tap.last_rect("VP-OBJ-AddConvex"):
                    break
                time.sleep(0.4)
        convex_rect = tap.last_rect("VP-OBJ-AddConvex")
        capsule_rect = tap.last_rect("VP-OBJ-AddCapsule")
        check("子菜单含 Convex Hull (QuickHull)", convex_rect is not None,
              str(convex_rect))
        check("子菜单含 Capsule", capsule_rect is not None, str(capsule_rect))
        rem_rect = tap.last_rect("VP-OBJ-Remove")
        edit_rect = tap.last_rect("VP-OBJ-EditCollider")
        check("菜单含 Remove Collision Body", rem_rect is not None)
        check("菜单含 Edit Collider with W/E/R", edit_rect is not None)

        if not convex_rect or not capsule_rect:
            capture_client(hwnd)[0].save(
                args.out.replace(".png", "_submenu_fail.png"))
            return 5

        capture_client(hwnd)[0].save(args.out.replace(".png", "_submenu.png"))

        # ---------- 4) 凸包：按几何结构（QuickHull） ----------
        print("\n[3] 添加凸包碰撞体（按物体几何结构）")
        click_left(hwnd, *center(convex_rect))
        add = None
        end = time.time() + 8.0
        while time.time() < end and add is None:
            with tap.lock:
                for line in tap.lines:
                    m = ADD_RE.search(line)
                    if m and m.group(1) == "convex":
                        add = m
            time.sleep(0.1)
        check("凸包碰撞体创建成功（collision: added convex collider）",
              add is not None, add.group(0) if add else "没等到日志")
        if add:
            pts, edges = int(add.group(3)), int(add.group(4))
            check("凸包顶点 >= 4（QuickHull 至少一个四面体）", pts >= 4,
                  "points=%d" % pts)
            check("凸包边数 >= 6", edges >= 6, "edges=%d" % edges)
            check("凸包对立方体给出 8 点 18 边", (pts, edges) == (8, 18),
                  "points=%d edges=%d" % (pts, edges))

        def find_fit(shape, timeout=5.0):
            end = time.time() + timeout
            while time.time() < end:
                with tap.lock:
                    for line in tap.lines:
                        m = FIT_RE.search(line)
                        if m and m.group(2) == shape:
                            return m
                time.sleep(0.1)
            return None

        fit = find_fit("convex")
        check("凸包 AABB 与网格 AABB 一并报出（collision: fit）",
              fit is not None)
        if fit:
            ratios = [float(fit.group(i)) for i in (9, 10, 11)]
            tight = all(0.90 <= r <= 1.02 for r in ratios)
            check("创建时**刚好包裹住物体**（凸包 ratio ≈ 1）", tight,
                  "ratio=(%.4f,%.4f,%.4f)" % tuple(ratios))

        def wait_sig(shape, timeout=5.0):
            end = time.time() + timeout
            while time.time() < end:
                with tap.lock:
                    for line in tap.lines:
                        m = SIG_RE.search(line)
                        if m and m.group(2) == shape:
                            return m
                time.sleep(0.1)
            return None

        sig = wait_sig("convex")
        check("可视化的碰撞框在渲染（COLLIDER-SIG）", sig is not None,
              sig.group(0) if sig else "没等到日志")
        if sig:
            check("  碰撞框形状 = convex", sig.group(2) == "convex",
                  sig.group(2))
            if add:
                check("  碰撞框顶点数与创建一致",
                      int(sig.group(3)) == int(add.group(3)),
                      "sig=%s add=%s" % (sig.group(3), add.group(3)))
            check("  碰撞框默认 solid", sig.group(5) == "1", sig.group(5))
        coll_rect = tap.last_rect("COLLIDER " + args.entity)
        check("碰撞框矩形探针（COLLIDER %s）" % args.entity,
              coll_rect is not None, str(coll_rect))

        print("  截图:", args.out)
        capture_client(hwnd)[0].save(args.out)

        # ---------- 5) 胶囊 ----------
        print("\n[4] 添加胶囊碰撞体")
        ox, oy = object_screen()
        right_click(hwnd, ox, oy)
        op2 = wait_open_seq(tap, 2, 6.0)
        check("第二次右键仍能弹出菜单（seq=2）", op2 is not None)
        add_rect = tap.last_rect("VP-OBJ-Add")
        if add_rect:
            hover(hwnd, *center(add_rect), wait=0.7)
        capsule_rect = None
        for _ in range(4):
            capsule_rect = tap.last_rect("VP-OBJ-AddCapsule")
            if capsule_rect:
                break
            time.sleep(0.3)
        if not capsule_rect:
            check("第二次菜单里 Capsule 项可用", False, "矩形丢失")
            return 6
        click_left(hwnd, *center(capsule_rect))

        cap = None
        end = time.time() + 8.0
        while time.time() < end and cap is None:
            with tap.lock:
                for line in tap.lines:
                    m = ADD_RE.search(line)
                    if m and m.group(1) == "capsule":
                        cap = m
            time.sleep(0.1)
        check("胶囊碰撞体创建成功（collision: added capsule collider）",
              cap is not None, cap.group(0) if cap else "没等到日志")
        if cap:
            check("  胶囊没有凸包顶点（points=0）", int(cap.group(3)) == 0,
                  "points=%s" % cap.group(3))
            check("  胶囊半径 > 0", float(cap.group(5)) > 0.0,
                  "radius=%s" % cap.group(5))
            check("  立方体（1m）胶囊半径 = 外接圆 √0.5 ≈ 0.7071",
                  abs(float(cap.group(5)) - 0.70710678) < 0.01,
                  "radius=%s" % cap.group(5))

        cap_fit = find_fit("capsule") if cap else None
        check("胶囊 AABB 与网格 AABB 一并报出", cap_fit is not None)
        if cap_fit:
            ratios = [float(cap_fit.group(i)) for i in (9, 10, 11)]
            covers = all(r >= 0.999 for r in ratios)
            check("胶囊完整**盖住**物体 AABB（ratio >= 1）", covers,
                  "ratio=(%.4f,%.4f,%.4f)" % tuple(ratios))
            notes.append("capsule ratio=(%.4f,%.4f,%.4f)" % tuple(ratios))

        sig2 = wait_sig("capsule")
        check("  碰撞框切到 shape=capsule",
              sig2 is not None and sig2.group(2) == "capsule",
              sig2.group(0) if sig2 else "没等到日志")
        if sig2:
            check("  胶囊碰撞框边数 = 0（凸包边为空）",
                  int(sig2.group(4)) == 0, sig2.group(4))
        capture_client(hwnd)[0].save(args.out.replace(".png", "_capsule.png"))

        # ---------- 6) 守卫（放最后：它会转动相机）----------
        print("\n[5] 守卫：右键**拖拽**（位移 > 6px）不弹菜单")
        ox, oy = object_screen()
        n0 = open_count()
        right_click(hwnd, ox, oy, drift=40)
        time.sleep(0.6)
        check("右键拖拽（>6px）不弹菜单（右键转视角不弹菜单）",
              open_count() == n0, "拖拽也弹了菜单")

        print()
        if fails:
            print("最近 60 行日志：")
            for line in tap.lines[-60:]:
                print("   ", line)
            print("\nRESULT: FAIL")
            for f in fails:
                print("  -", f)
            return 7
        for n in notes:
            print("note:", n)
        if not args.keep_scene:
            try:
                os.remove(scene_path)
                os.rmdir(tmpdir)
            except OSError:
                pass
        print("截图：", args.out)
        print("RESULT: PASS")
        return 0
    finally:
        close(proc, hwnd)


if __name__ == "__main__":
    sys.exit(main())
