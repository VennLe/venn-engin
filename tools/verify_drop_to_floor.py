"""端到端验证：数字键 0「落地」+ 自带地面「上锁」（2026-10-10 用户需求）。

三件在命令行里模拟不了、只能真输入真点击的事：

  A. **守卫**：选择不是"在 3D 视口里左键点中"的（这里用 MYVK_EDITOR_SELECT
     模拟层级树选中）→ 按 0 **必须什么都不做**，只在状态栏提示一句。
     断言日志：drop-to-floor: skipped（且**没有**真正落地的那行）。

  B. **正例**：镜头摆好让悬空立方体正好在视口中心 → 左键点中它 → 按 0
     → 它应该竖直落到地面：x / y 一点不动，底部贴到 z = 0。
     测试场景里立方体是 1m 边长、中心在 z = 3 → 底部在 2.5
     → dz 必须是 -2.5，落点 z 必须是 0.5，bottom 必须是 0。
     断言日志：pick: selected 'FloatingCube' (from viewport)
              drop-to-floor: 'FloatingCube' dz=-2.5000 pos=(0,0,3)->(0,0,0.5)

  C. **地面锁定**：
     · 启动即选中 Ground（模拟层级树选中）→ 断言不给变换手柄
       （gizmo: 'Ground' is locked -> no transform handle）
     · 在视口里点**只有地面**的地方 → 断言什么都没选中（点选跳过了地面）

用法：
    python tools/verify_drop_to_floor.py
    python tools/verify_drop_to_floor.py --keep-scene     # 保留临时场景文件
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32  # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")

RECT_RE = re.compile(
    r"([A-Za-z-]+)=\((-?[\d.]+),(-?[\d.]+)\)-\((-?[\d.]+),(-?[\d.]+)\)")
DROP_RE = re.compile(
    r"drop-to-floor: '([^']*)' dz=(-?[\d.]+) pos=\((-?[\d.]+),(-?[\d.]+),"
    r"(-?[\d.]+)\)->\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) bottom=(-?[\d.]+)")

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101

VK_0 = 0x30
SC_0 = 0x0B          # 主键盘区 '0' 的扫描码（GLFW 按 lParam 扫描码映射）


class LogTap:
    def __init__(self, proc):
        self.lines = []
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._pump, args=(proc,), daemon=True)
        self.t.start()

    def _pump(self, proc):
        for raw in proc.stdout:
            with self.lock:
                self.lines.append(raw.strip())

    def has(self, needle):
        with self.lock:
            return any(needle in l for l in self.lines)

    def last_rect(self, tag):
        needle = tag + "=("
        with self.lock:
            found = None
            for line in self.lines:
                if needle in line:
                    m = RECT_RE.search(line)
                    if m:
                        found = tuple(float(m.group(i)) for i in range(2, 6))
            return found

    def drops(self):
        """所有"真的落地了"的日志（已解析成 dict）"""
        out = []
        with self.lock:
            for line in self.lines:
                m = DROP_RE.search(line)
                if m:
                    out.append({
                        "name": m.group(1),
                        "dz": float(m.group(2)),
                        "before": tuple(float(m.group(i)) for i in (3, 4, 5)),
                        "after": tuple(float(m.group(i)) for i in (6, 7, 8)),
                        "bottom": float(m.group(9)),
                    })
        return out

    def wait(self, needle, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.has(needle):
                return True
            time.sleep(0.1)
        return False


def click(hwnd, cx, cy):
    force_foreground(hwnd)
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.3)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.5)


def press_zero(hwnd):
    """按数字键 0。先试真实键盘事件（绝大多数情况够用），不生效再 PostMessage
    直投窗口 —— 后者不依赖前台焦点，但按 GLFW 的规矩必须带扫描码。"""
    force_foreground(hwnd)
    user32.keybd_event(VK_0, SC_0, 0, 0)
    time.sleep(0.08)
    user32.keybd_event(VK_0, SC_0, 0x0002, 0)
    time.sleep(0.35)


def post_zero(hwnd):
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_0, (SC_0 << 16) | 1)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_0, (SC_0 << 16) | 0xC0000001)
    time.sleep(0.35)


def press_zero_until(hwnd, tap, needle, tries=4):
    """按 0 直到出现期望的日志（两种注入方式轮着来）"""
    for i in range(tries):
        press_zero(hwnd)
        if tap.wait(needle, 1.2):
            return True
        post_zero(hwnd)
        if tap.wait(needle, 1.2):
            return True
        print("    retry: 数字键 0 未生效（第 %d 次）" % (i + 1))
    return False


def _save(hwnd, path):
    """截一张客户区图存档（人工复核用）"""
    img, _ = capture_client(hwnd)
    img.save(path)
    print("  截图:", path)


def kill_stale_editors():
    subprocess.run(["taskkill", "/F", "/IM", "Editor.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)


def make_test_scene(path):
    """一个悬空立方体：1m 边长、中心 (0,0,3) → 底部在 z = 2.5"""
    scene = {
        "version": 1,
        "generator": "verify_drop_to_floor",
        "camera": {"target": [0, 0, 0.5], "yaw": 0.62, "pitch": 0.30,
                   "distance": 9.0},
        "entities": [{
            "name": "FloatingCube",
            "transform": {"position": [0.0, 0.0, 3.0],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1,
            "visible": True,
            "castShadow": True,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "drop_test_cube"},
            "material": {"kind": "procedural", "name": "drop_test_mat",
                         "baseColor": [0.85, 0.35, 0.30, 1.0],
                         "metallic": 0.0, "roughness": 0.55,
                         "normalScale": 0.0, "ao": 1.0,
                         "emissive": [0.0, 0.0, 0.0],
                         "doubleSided": False, "alphaBlend": False},
        }],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, indent=2)
    return path


def launch(exe, extra_env, size):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env.update(extra_env)
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap(proc)
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
    time.sleep(1.0)
    if proc.poll() is None:
        proc.kill()
    proc.wait(timeout=10)
    time.sleep(0.6)


def viewport_center(tap, exe, hwnd):
    """视口矩形中心（每帧重打，取最后一条）"""
    rect = None
    for _ in range(30):
        rect = tap.last_rect("VP-RECT")
        if rect:
            break
        time.sleep(0.2)
    if not rect:
        return None
    return ((rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5, rect)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "..", "_venn_verify"))
    ap.add_argument("--keep-scene", action="store_true")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    out = os.path.abspath(args.out_dir)
    os.makedirs(out, exist_ok=True)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1

    tmpdir = tempfile.mkdtemp(prefix="venn_drop_")
    scene_path = make_test_scene(os.path.join(tmpdir, "floating_cube.json"))
    print("测试场景:", scene_path)

    fails = []
    kill_stale_editors()

    try:
        # ------------------------------------------------------------------
        # A. 守卫：不是视口点中的选中 → 按 0 不该动它
        # ------------------------------------------------------------------
        print("\n[A] 守卫：层级树选中（非视口点选）时按 0")
        proc, hwnd, tap = launch(exe, {"MYVK_EDITOR_SCENE": scene_path,
                                       "MYVK_EDITOR_SELECT": "FloatingCube",
                                       "MYVK_EDITOR_CAM": "0,-8,3,90,0"},
                                 args.size)
        if not hwnd:
            print("  未找到窗口"); return 2
        press_zero_until(hwnd, tap, "drop-to-floor: skipped")
        skipped = tap.has("drop-to-floor: skipped")
        dropped = len(tap.drops()) > 0
        print("  skipped=%s  dropped=%s" % (skipped, dropped))
        if not skipped:
            fails.append("A: 没等到 'drop-to-floor: skipped'（守卫没生效）")
        if dropped:
            fails.append("A: 非视口选中竟然落地了（守卫失效）")
        close(proc, hwnd)

        # ------------------------------------------------------------------
        # B. 正例：视口左键点中 → 按 0 → 竖直落地
        # ------------------------------------------------------------------
        print("\n[B] 正例：视口左键点中悬空立方体 → 按 0")
        kill_stale_editors()
        proc, hwnd, tap = launch(exe, {"MYVK_EDITOR_SCENE": scene_path,
                                       "MYVK_EDITOR_CAM": "0,-8,3,90,0"},
                                 args.size)
        if not hwnd:
            print("  未找到窗口"); return 2
        vc = viewport_center(tap, exe, hwnd)
        if not vc:
            print("  拿不到 VP-RECT"); return 2
        cx, cy, rect = vc
        print("  视口矩形=(%.0f,%.0f)-(%.0f,%.0f) 点击中心=(%.0f,%.0f)"
              % (rect + (cx, cy)))
        # 相机 (0,-8,3) 平视 +Y → (0,0,3) 的立方体正好在视口中心
        click(hwnd, cx, cy)
        got_pick = tap.wait("pick: selected 'FloatingCube' (from viewport)", 2.0)
        print("  pick 命中:", got_pick)
        if not got_pick:
            fails.append("B: 视口左键没点中 FloatingCube（pick 日志缺失）")
        else:
            press_zero_until(hwnd, tap, "drop-to-floor: 'FloatingCube' dz=")
            drops = tap.drops()
            if not drops:
                fails.append("B: 按 0 之后没有落地日志")
            else:
                d = drops[-1]
                print("  dz=%.4f  before=%s  after=%s  bottom=%.4f"
                      % (d["dz"], d["before"], d["after"], d["bottom"]))
                # 断言 1：只动 z —— x / y 必须一模一样
                if abs(d["after"][0] - d["before"][0]) > 1e-6 or \
                   abs(d["after"][1] - d["before"][1]) > 1e-6:
                    fails.append("B: x / y 被改动了（应当一点不动）")
                # 断言 2：底部贴到 z = 0
                if abs(d["bottom"]) > 1e-3:
                    fails.append("B: 底部没贴到 z=0（bottom=%.4f）" % d["bottom"])
                # 断言 3：立方体中心 3.0 - 0.5 = 0.5
                if abs(d["after"][2] - 0.5) > 1e-3:
                    fails.append("B: 落点 z 应为 0.5，实际 %.4f" % d["after"][2])
                # 断言 4：dz = -2.5
                if abs(d["dz"] + 2.5) > 1e-3:
                    fails.append("B: dz 应为 -2.5，实际 %.4f" % d["dz"])
        _save(hwnd, os.path.join(out, "drop_to_floor.png"))
        close(proc, hwnd)

        # ------------------------------------------------------------------
        # C. 地面锁定：不给手柄 + 点选跳过
        # ------------------------------------------------------------------
        print("\n[C] 地面锁定：选中不给手柄 / 视口点不到")
        kill_stale_editors()
        proc, hwnd, tap = launch(exe, {"MYVK_EDITOR_SELECT": "Ground",
                                       "MYVK_EDITOR_CAM": "0,-8,4,90,10"},
                                 args.size)
        if not hwnd:
            print("  未找到窗口"); return 2
        locked = tap.wait("gizmo: 'Ground' is locked -> no transform handle", 3.0)
        print("  不给手柄:", locked)
        if not locked:
            fails.append("C: 选中 Ground 时仍给了变换手柄（锁定没生效）")
        # 先存档：此刻 Ground 是选中的（Inspector 能拍到 Locked 勾 + 视口里没手柄）
        _save(hwnd, os.path.join(out, "ground_locked.png"))

        vc = viewport_center(tap, exe, hwnd)
        if not vc:
            print("  拿不到 VP-RECT"); return 2
        cx, cy, rect = vc
        # 视口下方 82% 处：只有地面（俯角 10°，那块区域离相机很近）
        gx = rect[0] + (rect[2] - rect[0]) * 0.5
        gy = rect[1] + (rect[3] - rect[1]) * 0.82
        click(hwnd, gx, gy)
        nothing = tap.wait("pick: nothing hit -> selection cleared", 2.0)
        hit_ground = tap.has("pick: selected 'Ground'")
        print("  点空地 → 无选中:", nothing, " 选中了地面:", hit_ground)
        if not nothing:
            fails.append("C: 点空地没出现 'nothing hit'（点选没跳过地面？）")
        if hit_ground:
            fails.append("C: 地面被点选中了（锁定失效）")
        close(proc, hwnd)

    finally:
        kill_stale_editors()
        if not args.keep_scene:
            try:
                os.remove(scene_path)
                os.rmdir(tmpdir)
            except OSError:
                pass

    print("\n================ 结果 ================")
    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("全部 PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
