"""端到端验证：碰撞框线框的 **Show Wireframe 勾选框**。

用户要的形态（原文）：
    "物体的碰撞体检测线框在属性栏中设置一个勾选框可以切换它的隐藏和
     显示状态，正常默认是勾选显示状态的。"

断言：
  1. 默认**显示**：`COLLIDER-SIG ... show=1`，视口里有碰撞框线框
     （COLLIDER 矩形区域内有金色/青色线框像素）；
  2. Inspector ▸ Collision ▸ Show Wireframe 取消勾选 →
     `COLLIDER-SIG ... show=0`，线框像素消失（看不见的东西也不该
     再响应点击 —— hitSelectedWireframe 同步跳过）；
  3. Ctrl+S 存盘 → JSON 里 `collision.showWireframe=false`（状态持久化）；
  4. 重新勾上 → show=1，线框像素回来。

注意：W/E/R 编辑中的碰撞体**强制显示**（哪怕勾没了）—— 这是刻意的：
调整时看不见自己在动什么太奇怪。所以脚本先点掉 "Edit with W/E/R"
（MYVK_EDITOR_COLLIDER 钩子创建完会自动打开编辑态）。

用法：
    python tools/verify_collider_show.py
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
from verify_add_menu import LogTap, kill_stale_editors, wait_rect     # noqa: E402
from verify_asset_drag import client_to_screen, force_foreground      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

SIG_RE = re.compile(
    r"COLLIDER-SIG '([^']*)' shape=(\S+) points=(\d+) edges=(\d+) "
    r"solid=(\d) show=(\d)")
SAVED_RE = re.compile(r"Scene saved: '(.+)'")

VK_S = 0x53
SC_S = 0x1F
VK_CONTROL = 0x11
SC_CTRL = 0x1D

WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101

MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004

# 线框用色（ColliderView.cpp）：空闲青 / 选中金黄 / 编辑橙。
# ⚠ ImGui 的字节色经 **sRGB 交换链**会被当线性值再做一次 sRGB 编码
#   （坑点 #34 同源）：金 (255,202,74) 实测上屏是 (245,219,142) ——
#   b 通道 74→142 正好是 0.29^(1/2.4) 曲线。判据用编码后的实测值。
WIREFRAME_COLORS = [
    (255, 202, 74), (255, 148, 40), (96, 208, 196),      # 理论值
    (245, 219, 142), (234, 208, 137),                    # 金色编码后实测
    (239, 213, 105),                                     # 橙色编码后
    (169, 240, 235), (150, 236, 224),                    # 青色编码后
    # ⚠ 不要加"金线混红方块"的中间色 (~206,176,124)：它和红方块受光面
    #   (~237,160,149) 在 ±40 容差里互相重叠，线框隐藏后方块表面会假阳性。
    #   金线编码色与红色面靠 b 通道 (142 vs 149±) 天然分离，够用。
]


def save_scene(hwnd, tap, timeout=6.0):
    """Ctrl+S（真实 Ctrl + PostMessage 带扫描码的 S —— 坑点 #57）。"""
    force_foreground(hwnd)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0, 0)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_S, (SC_S << 16) | 1)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_S, (SC_S << 16) | 0xC0000001)
    user32.keybd_event(VK_CONTROL, SC_CTRL, 0x0002, 0)
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for line in tap.lines:
                m = SAVED_RE.search(line)
                if m:
                    return m.group(1)
        time.sleep(0.1)
    return None


def make_scene(path):
    scene = {
        "version": 1,
        "generator": "verify_collider_show",
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
                         "name": "show_test_cube"},
                "material": {"kind": "procedural", "name": "show_test_mat",
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


def click(hwnd, cx, cy):
    force_foreground(hwnd)
    sx, sy = client_to_screen(hwnd, cx, cy)
    user32.SetCursorPos(sx, sy)
    time.sleep(0.25)
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.12)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.5)


def center(rect):
    return (int((rect[0] + rect[2]) * 0.5), int((rect[1] + rect[3]) * 0.5))


def wait_match(pattern, tap, timeout=4.0):
    end = time.time() + timeout
    while time.time() < end:
        with tap.lock:
            for line in tap.lines:
                m = pattern.search(line)
                if m:
                    return m
        time.sleep(0.1)
    return None


def click_until_sig(hwnd, tap, checkbox, want, tries=4):
    """点击勾选框直到 COLLIDER-SIG 的 show 位变成 want。

    ⚠ 这台机器的鼠标在 SetCursorPos 之后 0.35s 内会被平滑漂走（坑点
    #32），点击偶发落空 —— 落空就再点一次，最多 tries 次。
    """
    for _ in range(tries):
        click(hwnd, *center(checkbox))
        end = time.time() + 4.0
        while time.time() < end:
            with tap.lock:
                for line in tap.lines:
                    m = SIG_RE.search(line)
                    if m and m.group(6) == want:
                        return m
            time.sleep(0.1)
    return None


def click_until_line(hwnd, tap, rect, needle, tries=4):
    """点击 rect 直到日志里出现含 needle 的行（toggle 类勾选框的回执）。"""
    for _ in range(tries):
        click(hwnd, *center(rect))
        end = time.time() + 3.0
        while time.time() < end:
            with tap.lock:
                for line in tap.lines:
                    if needle in line:
                        return line
            time.sleep(0.1)
    return None


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
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(0.6)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


def wireframe_pixel_count(img, box):
    """box=(x0,y0,x1,y1) 内的线框像素数（任一线框色 ±40 容差）。"""
    x0, y0, x1, y1 = [int(v) for v in box]
    x0 = max(0, x0)
    y0 = max(0, y0)
    n = 0
    px = img.load()
    for y in range(y0, min(y1, img.height)):
        for x in range(x0, min(x1, img.width)):
            r, g, b = px[x, y][:3]
            for (cr, cg, cb) in WIREFRAME_COLORS:
                if (abs(r - cr) < 40 and abs(g - cg) < 40
                        and abs(b - cb) < 40):
                    n += 1
                    break
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    # ⚠ 窗口要够高：Collision 段展开后 Show Wireframe 勾选框贴近底部，
    #   900px 的窗口会把它压出屏幕外（点击落空且无报错）。
    ap.add_argument("--size", default="1600x1200")
    args = ap.parse_args()

    kill_stale_editors()
    os.makedirs(OUT_DIR, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="collider_show_")
    scene_path = make_scene(os.path.join(tmpdir, "show.json"))
    out_png = os.path.join(OUT_DIR, "collider_show.png")
    out_png_off = os.path.join(OUT_DIR, "collider_show_off.png")

    fails = []

    def check(name, ok, detail=""):
        print("%s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  (%s)" % detail) if detail else ""))
        if not ok:
            fails.append(name)

    proc, hwnd, tap = launch(args.exe, {
        "MYVK_EDITOR_SCENE": scene_path,
        "MYVK_EDITOR_SELECT": "ColliderCube",
        "MYVK_EDITOR_COLLIDER": "hull",
        # ⚠ 必须关栅格（本文件是**逐像素**判据）。地面栅格的线色
        #   `GridSettings.color = (0.72,0.76,0.82)` 上屏后落在浅青一带，
        #   在 ±40 容差里和 WIREFRAME_COLORS 的青色**撞色**；而判据用的
        #   矩形是 `COLLIDER <名字>` 外扩 8px 的，正好把那圈地面线框进来。
        #   实测：栅格开着时"线框已隐藏"仍剩 176 个假阳性像素（阈值 <40）
        #   → 红；关掉后 0。这**不是**线框没藏住 —— 用改动前的旧栅格参数
        #   （extent 60 / fade 45→60）复跑，数字一模一样是 176，属既有缺陷。
        "MYVK_GRID": "0",
    }, args.size)
    if not hwnd:
        print("编辑器没起来")
        return 2
    try:
        # ---- 1) 默认显示 ----
        print("[1] 默认状态（勾选 = 显示）")
        sig = None
        end = time.time() + 8.0
        while time.time() < end and sig is None:
            with tap.lock:
                for line in tap.lines:
                    m = SIG_RE.search(line)
                    if m:
                        sig = m
            time.sleep(0.1)
        check("碰撞框已创建（COLLIDER-SIG）", sig is not None)
        if sig:
            check("  默认 show=1（勾选显示状态）", sig.group(6) == "1",
                  "show=%s" % sig.group(6))
        coll = wait_rect(tap, "COLLIDER ColliderCube", 6.0)
        check("线框矩形探针（COLLIDER ColliderCube）", coll is not None,
              str(coll))

        shot = capture_client(hwnd)[0]
        shot.save(out_png)
        if coll:
            box = (coll[0] - 8, coll[1] - 8, coll[2] + 8, coll[3] + 8)
            n = wireframe_pixel_count(shot, box)
            # <40：残留的是红方块上的太阳高光（暖白色，与编码金在容差内
            # 撞色）+ 抗锯齿边缘 —— 实测 ~15 个；线框本身 ~3000 个。
            check("  视口里确实有线框像素", n > 300, "pixels=%d" % n)

        # ---- 2) 展开 Collision 段 → 关编辑 → 关线框 ----
        print("[2] Inspector ▸ Collision ▸ 取消 Show Wireframe")
        header = wait_rect(tap, "INS-COLLISION-HEADER", 6.0)
        if header:
            click(hwnd, *center(header))
            time.sleep(0.6)
        edit_cb = wait_rect(tap, "INS-COLLIDER-EDIT", 5.0)
        check("Edit with W/E/R 勾选框（INS-COLLIDER-EDIT）",
              edit_cb is not None, str(edit_cb))
        if edit_cb:
            # 关掉编辑（线框才允许隐藏）。编辑态日志回执确认切到位 ——
            # 单次点击落空的话编辑态强制显示线框，像素断言会误报。
            got = click_until_line(hwnd, tap, edit_cb,
                                   "INS-COLLIDER-EDIT-SET 0")
            check("  编辑态已关闭（INS-COLLIDER-EDIT-SET 0）",
                  got is not None, str(got))
            time.sleep(0.3)
        show_cb = wait_rect(tap, "INS-COLLIDER-SHOW", 5.0)
        check("Show Wireframe 勾选框（INS-COLLIDER-SHOW）",
              show_cb is not None, str(show_cb))
        if not show_cb:
            capture_client(hwnd)[0].save(
                out_png.replace(".png", "_no_checkbox.png"))
            return 5

        sig0 = wait_match(SIG_RE, tap, 1.0)
        sig2 = click_until_sig(hwnd, tap, show_cb, "0")
        check("取消勾选 → COLLIDER-SIG show=0", sig2 is not None,
              sig2.group(0) if sig2 else "点击 4 次都没等到")

        time.sleep(0.5)
        shot2 = capture_client(hwnd)[0]
        shot2.save(out_png_off)
        if coll:
            box = (coll[0] - 8, coll[1] - 8, coll[2] + 8, coll[3] + 8)
            n2 = wireframe_pixel_count(shot2, box)
            check("  线框像素消失", n2 < 40, "pixels=%d" % n2)  # 高光/AA 残留 ~15

        # ---- 3) 持久化 ----
        print("[3] Ctrl+S → JSON")
        saved = save_scene(hwnd, tap)
        check("存盘成功", saved is not None)
        if saved:
            with open(saved, encoding="utf-8") as f:
                doc = json.load(f)
            col = None
            for ent in doc.get("entities", []):
                if ent.get("name") == "ColliderCube":
                    col = ent.get("collision")
            check("collision.showWireframe=false 落盘",
                  col is not None and col.get("showWireframe") is False,
                  str(col))

        # ---- 4) 勾回来 ----
        print("[4] 重新勾上 → 线框回来")
        sig3 = click_until_sig(hwnd, tap, show_cb, "1")
        check("重新勾选 → COLLIDER-SIG show=1", sig3 is not None,
              sig3.group(0) if sig3 else "点击 4 次都没等到")
        time.sleep(0.5)
        shot3 = capture_client(hwnd)[0]
        shot3.save(out_png.replace(".png", "_back.png"))
        if coll:
            box = (coll[0] - 8, coll[1] - 8, coll[2] + 8, coll[3] + 8)
            n3 = wireframe_pixel_count(shot3, box)
            check("  线框像素回来", n3 > 300, "pixels=%d" % n3)

        print()
        if fails:
            print("最近 40 行日志：")
            with tap.lock:
                for line in tap.lines[-40:]:
                    print("   ", line)
            print("RESULT: FAIL")
            for f in fails:
                print("  -", f)
            return 6
        print("截图:", out_png, "/", out_png_off)
        print("RESULT: PASS")
        return 0
    finally:
        close(proc, hwnd)


if __name__ == "__main__":
    sys.exit(main())
