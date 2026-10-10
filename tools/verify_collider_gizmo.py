"""端到端验证：用 W / E / R 手柄调整**碰撞框**（而不是物体）。

用户要的形态（原文）：
    "碰撞框也可以通过 w，e，r 去调整，但创建时默认是刚好包裹住物体的"

三件事必须同时成立，缺一个就不算做到：

  1. **W/E/R 三模式都能作用在碰撞框上**：`gizmo begin ... target=collider`
     / `gizmo end: ... target=collider`。
  2. **只改碰撞框，不动物体**：这正是"单独调碰撞体"的意义所在。断言分两层 ——
     · 拖拽日志里的数值（`pos= / rot= / scale=` 是碰撞组件自己的分量）
     · Ctrl+S 存盘后读 JSON：实体的 `transform` 一个字节都没变，
       而 `collision` 的 position / rotation / scale 各自被改过。
     第二层是关键的：日志只能证明"手柄以为自己在改碰撞体"，JSON 才能证明
     "物体的变换真的没被动过"。
  3. **创建时默认刚好包裹住物体**：日志 `collision: fit ... ratio ≈ 1`。

为什么每种模式各起一个进程：
    手柄的 W/E/R 快捷键在"右键飞行时"会**让位**给相机（见
    EditorApp::onUpdate 里 `flying` 那段）。同一个窗口里连着按 R → 拖拽
    → 按 E，一旦按键时序和 ImGui 的输入帧错开，就会出现"模式没切过去，
    后面的拖拽抓空"。用 `MYVK_EDITOR_GIZMO=move|rotate|scale` 启动钩子把
    模式**在启动时就定好**，每个模式独占一次进程，互不干扰、也不依赖按键
    时序 —— 顺带让"某个模式挂掉"不会连累另外两个。

    每个模式内部又各自做一遍"拖拽 → Ctrl+S → 读 JSON"，于是"只改碰撞体、
    不动物体"这件事在三种模式下都被独立证明了一次。

用法：
    python tools/verify_collider_gizmo.py
"""

import argparse
import json
import math
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

WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
VK_CONTROL = 0x11
SC_CTRL = 0x1D
VK_S, SC_S = 0x53, 0x1F

BEGIN_RE = re.compile(
    # target= 在行尾（产品日志为兼容 verify_gizmo_scale.py 的旧次序）
    r"gizmo begin: handle=(\S+) mode=(\S+) space=(\S+) .*target=(\S+)")
END_RE = re.compile(
    r"gizmo end: handle=(\S+) '([^']*)' "
    r"pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"rot=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"scale=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) target=(\S+)")
FIT_RE = re.compile(
    r"collision: fit '([^']*)' shape=(\S+) "
    r"mesh=\(([\d.]+),([\d.]+),([\d.]+)\) "
    r"collider=\(([\d.]+),([\d.]+),([\d.]+)\) "
    r"ratio=\(([\d.-]+),([\d.-]+),([\d.-]+)\)")
AUTO_RE = re.compile(
    r"Editor auto-collider \((\S+)\): '([^']*)' points=(\d+) edges=(\d+) "
    r"radius=([\d.]+) halfHeight=([\d.]+)")
SAVED_RE = re.compile(r"Scene saved: '([^']+)'")

# 手柄给模式起的名字（GizmoController::modeName）：平移叫 "Move" 不是
# "Translate"；给手柄起的名字（handleName）是 "X" 不是 "AxisX"。
MODE_SPECS = [
    # (钩子值, 期望 modeName, 探针标签, 期望 handle, 是否切向拖, 说明)
    ("move",   "Move",   "GZ-AXIS-X", "X", False,
     "W（平移）—— 拖 X 轴，只改碰撞体 position.x"),
    ("scale",  "Scale",  "GZ-AXIS-X", "X", False,
     "R（缩放）—— 拖 X 轴，只改碰撞体 scale.x"),
    ("rotate", "Rotate", "GZ-RING-X", "X", True,
     "E（旋转）—— 切向拖 X 环，只改碰撞体 rotation.x"),
]


# ------------------------------------------------------------------ 场景

def make_scene(path):
    """1m 立方体，中心 z=0.5；相机摆成 3/4 视角（三根轴在屏幕上互相错开，
    旋转环才投影成真正的椭圆，脚本才点得准环上的点）。"""
    scene = {
        "version": 1,
        "generator": "verify_collider_gizmo",
        "camera": {"target": [0, 0, 0.5], "yaw": math.radians(135),
                   "pitch": math.radians(20), "distance": 8.0},
        "entities": [
            {
                "name": "GizmoCube",
                "transform": {"position": [0.0, 0.0, 0.5],
                              "rotation": [0.0, 0.0, 0.0],
                              "scale": [1.0, 1.0, 1.0]},
                "parent": -1, "visible": True, "castShadow": True,
                "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                         "name": "gizmo_test_cube"},
                "material": {"kind": "procedural", "name": "gizmo_test_mat",
                             "baseColor": [0.85, 0.45, 0.30, 1.0],
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
    user32.SetCursorPos(*client_to_screen(hwnd, cx, cy))


def center(rect):
    return ((rect[0] + rect[2]) * 0.5, (rect[1] + rect[3]) * 0.5)


def drag(hwnd, src, dst, steps=12):
    force_foreground(hwnd)
    move(hwnd, src[0], src[1])
    time.sleep(0.35)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.2)
    for k in range(1, steps + 1):
        move(hwnd, src[0] + (dst[0] - src[0]) * k / steps,
             src[1] + (dst[1] - src[1]) * k / steps)
        time.sleep(0.035)
    time.sleep(0.15)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.6)


def press_ctrl_s(hwnd):
    """Ctrl+S 存盘。Ctrl 必须是真实键盘事件（GLFW 用 GetKeyState 读修饰键）。"""
    for _ in range(5):
        if force_foreground(hwnd):
            break
        time.sleep(0.2)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0, 0)
    time.sleep(0.12)
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_S, (SC_S << 16) | 1)
    time.sleep(0.06)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_S, (SC_S << 16) | 0xC0000001)
    time.sleep(0.12)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0x0002, 0)
    time.sleep(0.9)


# ------------------------------------------------------------------ 日志

def last_match(tap, rx):
    with tap.lock:
        found = None
        for line in tap.lines:
            m = rx.search(line)
            if m:
                found = m
        return found


def wait_rect(tap, tag, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r:
            return r
        time.sleep(0.15)
    return None


def count(tap, rx):
    with tap.lock:
        return sum(1 for l in tap.lines if rx.search(l))


def try_drag(tap, hwnd, probe, perp, attempts=3):
    """从 probe 探针的中心拖一次手柄，最多重试 attempts 次。

    为什么要重试：每个进程的**第一次**左键按下有时会被 Windows 拿去
    "激活窗口"（不传给 GLFW），于是手柄收不到 mouse-down，日志里一个
    gizmo begin 都没有 —— 看上去就像"抓空了"。重试一次基本必中，而且
    每次都重新读一遍探针坐标，顺带免疫"上一次拖拽把相机/手柄挪了位置"。
    """
    for i in range(attempts):
        origin = tap.last_rect("GZ-ORIGIN")
        grab = wait_rect(tap, probe, 4.0)
        if not origin or not grab:
            time.sleep(0.4)
            continue
        ox, oy = center(origin)
        gx, gy = center(grab)
        ax, ay = gx - ox, gy - oy
        ln = math.hypot(ax, ay) or 1.0
        ux, uy = ax / ln, ay / ln
        if perp:
            ux, uy = -uy, ux            # 旋转环要切向拖才转得动
        reach = 150.0 if perp else max(60.0, ln * 1.4)
        n0 = count(tap, BEGIN_RE)
        drag(hwnd, (gx, gy), (gx + ux * reach, gy + uy * reach))
        time.sleep(0.5)
        if count(tap, BEGIN_RE) > n0:
            # 等到落地日志（旋转环偶尔要比平移多等一两帧）
            end = time.time() + 3.0
            while time.time() < end:
                if last_match(tap, END_RE):
                    break
                time.sleep(0.15)
            return (last_match(tap, BEGIN_RE), last_match(tap, END_RE),
                    (ox, oy, gx, gy))
        time.sleep(0.6)
    return None, None, None


def launch(exe, scene, size, gizmo, extra=None):
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_EDITOR_SELECT"] = "GizmoCube"
    env["MYVK_EDITOR_COLLIDER"] = "hull"
    env["MYVK_EDITOR_GIZMO"] = gizmo
    # eye = target + angleDir * distance（见 Camera::angleDir）
    # yaw=135° pitch=20° → 3/4 视角，三根轴在屏幕上互相错开
    env["MYVK_EDITOR_CAM"] = "-5.3,-5.3,3.2,135,20"
    if extra:
        env.update(extra)
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
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


# ------------------------------------------------------------------ 单模式

def run_mode(exe, scene_path, size, spec, out_png, check):
    """跑一个模式：启动 → 起手断言 → 拖一次 → 存盘 → 读 JSON。

    check 是外部传进来的断言函数（把失败收集到同一个列表里）。
    返回一个 dict：本次的拖拽前后数据（供末尾打印）。
    """
    hook, exp_mode, probe, exp_handle, perp, title = spec
    print("\n[%s]" % title)

    proc, hwnd, tap = launch(exe, scene_path, size, hook)
    if not hwnd:
        check("  %s：窗口起来了" % hook, False, "没找到窗口")
        return None
    result = {}
    try:
        # ---- 起手：钩子已建好凸包，且"刚好包裹住物体" ----
        auto = None
        end = time.time() + 8.0
        while time.time() < end and auto is None:
            auto = last_match(tap, AUTO_RE)
            time.sleep(0.15)
        check("  起点：钩子给选中项建好了凸包（Editor auto-collider）",
              auto is not None and auto.group(1) == "hull",
              auto.group(0) if auto else "没等到日志")
        if auto:
            check("    立方体 → 8 点 18 边",
                  (int(auto.group(3)), int(auto.group(4))) == (8, 18),
                  "points=%s edges=%s" % (auto.group(3), auto.group(4)))

        fit = None
        end = time.time() + 5.0
        while time.time() < end and fit is None:
            fit = last_match(tap, FIT_RE)
            time.sleep(0.15)
        if fit:
            ratios = [float(fit.group(i)) for i in (9, 10, 11)]
            check("  创建时刚好包裹住物体（collision: fit ratio ≈ 1）",
                  all(0.90 <= r <= 1.02 for r in ratios),
                  "ratio=(%.4f,%.4f,%.4f)" % tuple(ratios))
        else:
            check("  创建时刚好包裹住物体（collision: fit ratio ≈ 1）",
                  False, "没等到 'collision: fit' 日志")

        # ---- 探针 ----
        origin = wait_rect(tap, "GZ-ORIGIN", 10.0)
        grab_rect = wait_rect(tap, probe, 10.0)
        if not origin or not grab_rect:
            check("  手柄探针出现（GZ-ORIGIN + %s）" % probe, False,
                  "origin=%s probe=%s" % (origin, grab_rect))
            return None
        # ---- 拖拽（带重试，免疫每个进程第一次点击被拿去激活窗口）----
        bm, em, hit = try_drag(tap, hwnd, probe, perp)
        grabbed = bm is not None
        check("  抓到碰撞体手柄（gizmo begin）", grabbed,
              "重试 3 次都没抓到（探针 %s）" % probe)
        if not grabbed:
            capture_client(hwnd)[0].save(out_png.replace(".png", "_miss.png"))
            return None
        ox, oy, gx, gy = hit
        print("  手柄原点=(%.0f,%.0f)  抓取点(%s)=(%.0f,%.0f)"
              % (ox, oy, probe, gx, gy))

        check("  手柄目标是碰撞体（target=collider）",
              bm.group(4) == "collider", bm.group(0))
        check("    mode = %s" % exp_mode, bm.group(2) == exp_mode, bm.group(2))
        check("    handle = %s" % exp_handle, bm.group(1) == exp_handle,
              bm.group(1))
        check("  拖拽有落地日志（gizmo end, target=collider）",
              em is not None and em.group(12) == "collider",
              em.group(0) if em else "只抓到 begin，没等到 end")

        pos = rot = scl = None
        if em:
            pos = [float(em.group(i)) for i in (3, 4, 5)]
            rot = [float(em.group(i)) for i in (6, 7, 8)]
            scl = [float(em.group(i)) for i in (9, 10, 11)]
            result.update(pos=pos, rot=rot, scale=scl)
            if hook == "move":
                check("    只改碰撞体 position.x（y/z 不动）",
                      abs(pos[0]) > 1e-3 and abs(pos[1]) < 1e-4 and
                      abs(pos[2]) < 1e-4, "pos=(%.4f,%.4f,%.4f)" % tuple(pos))
            elif hook == "scale":
                check("    只改碰撞体 scale.x（y/z 保持 1）",
                      abs(scl[0] - 1.0) > 1e-3 and abs(scl[1] - 1.0) < 1e-4 and
                      abs(scl[2] - 1.0) < 1e-4,
                      "scale=(%.4f,%.4f,%.4f)" % tuple(scl))
            elif hook == "rotate":
                check("    碰撞体 rotation.x 真的转了（|rot.x| > 0）",
                      abs(rot[0]) > 1e-3, "rot=(%.4f,%.4f,%.4f)" % tuple(rot))

        capture_client(hwnd)[0].save(out_png)

        # ---- 存盘：物体 Transform 必须一个字节都没变 ----
        n0 = count(tap, SAVED_RE)
        press_ctrl_s(hwnd)
        for _ in range(30):
            if count(tap, SAVED_RE) > n0:
                break
            time.sleep(0.2)
        saved = last_match(tap, SAVED_RE)
        check("  Ctrl+S 触发存盘", saved is not None and count(tap, SAVED_RE) > n0,
              saved.group(0) if saved else "没有 'Scene saved' 日志")
        if saved and count(tap, SAVED_RE) > n0:
            path = saved.group(1)
            time.sleep(0.5)
            with open(path, "r", encoding="utf-8") as f:
                doc = json.load(f)
            ent = next((e for e in doc.get("entities", [])
                        if e.get("name") == "GizmoCube"), None)
            if not ent:
                check("  存盘文件里有 GizmoCube", False, path)
            else:
                tr = ent.get("transform", {})
                pl = [round(v, 5) for v in tr.get("position", [])]
                rl = [round(v, 5) for v in tr.get("rotation", [])]
                sl = [round(v, 5) for v in tr.get("scale", [])]
                check("  **物体的 transform.position 没被碰过**",
                      pl == [0.0, 0.0, 0.5], "position=%s" % pl)
                check("  **物体的 transform.rotation 没被碰过**",
                      rl == [0.0, 0.0, 0.0], "rotation=%s" % rl)
                check("  **物体的 transform.scale 没被碰过**",
                      sl == [1.0, 1.0, 1.0], "scale=%s" % sl)

                col = ent.get("collision")
                check("  存盘文件里有 collision 段", isinstance(col, dict))
                if isinstance(col, dict):
                    check("    collision.shape = convex",
                          col.get("shape") == "convex", str(col.get("shape")))
                    cp = [float(v) for v in col.get("position", [0, 0, 0])]
                    cr = [float(v) for v in col.get("rotation", [0, 0, 0])]
                    cs = [float(v) for v in col.get("scale", [1, 1, 1])]
                    result.update(col_pos=cp, col_rot=cr, col_scale=cs)
                    if hook == "move":
                        check("    collision.position.x 被平移改过（物体没动）",
                              abs(cp[0]) > 1e-3, "position=%s" % cp)
                    elif hook == "scale":
                        check("    collision.scale.x 被缩放改过（物体没动）",
                              abs(cs[0] - 1.0) > 1e-3, "scale=%s" % cs)
                    elif hook == "rotate":
                        check("    collision.rotation.x 被旋转改过（物体没动）",
                              abs(cr[0]) > 1e-3, "rotation=%s" % cr)
                    check("    凸包顶点不进 JSON（派生数据，按 mesh 重建）",
                          "hullPoints" not in col and "points" not in col,
                          str(sorted(col.keys())))
        return result
    finally:
        close(proc, hwnd)


# ------------------------------------------------------------------ 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--size", default="1600x900")
    ap.add_argument("--out", default=os.path.join(OUT_DIR,
                                                  "collider_gizmo.png"))
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    tmpdir = tempfile.mkdtemp(prefix="venn_collgizmo_")
    scene_path = make_scene(os.path.join(tmpdir, "collider_gizmo.json"))
    print("测试场景:", scene_path)

    fails = []
    results = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if (detail and not ok) else ""))
        if not ok:
            fails.append("%s | %s" % (name, detail))

    kill_stale_editors()
    try:
        for spec in MODE_SPECS:
            hook = spec[0]
            out_png = args.out.replace(".png", "_%s.png" % hook)
            r = run_mode(exe, scene_path, args.size, spec, out_png, check)
            if r:
                results.append((hook, r))
            capture_client_path = out_png
            print("  截图:", capture_client_path)

        print()
        for hook, r in results:
            print("  [%s] pos=%s rot=%s scale=%s" %
                  (hook, r.get("pos"), r.get("rot"), r.get("scale")))
            print("        collision pos=%s rot=%s scale=%s" %
                  (r.get("col_pos"), r.get("col_rot"), r.get("col_scale")))

        if fails:
            print("\nRESULT: FAIL")
            for f in fails:
                print("  -", f)
            return 4
        print("RESULT: PASS")
        return 0
    finally:
        try:
            os.remove(scene_path)
            os.rmdir(tmpdir)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
