"""一次性探针：目视确认 Shader 变体系统的两处 UI 是否正确渲染。

  · Stats 面板的 "Shader vars: N fwd / M depth built, K switches" 一行，
    以及下面那条"各掩码画了几个"的摊开列表；
  · Inspector 材质区的 "Shader Variant: k5 (double-sided|normal-map)" 一行
    （需要先点选一个有材质的物体）。

因为 Stats 面板默认关闭、也没有自动化开关，这个脚本配合两处临时改动使用：

  1. `editor/EditorContext.h` 里 `PanelVisibility::stats = true`（跑完还原）；
  2. 把 `build/bin/imgui.ini` 移开（跑完可还原）—— ImGui 会把每个窗口的
     Pos/Size 存进去，`Stats` 曾经被存在 `Pos=1228,336`，直接落在客户区
     之外，截图上只剩每行开头几个字（排查了一轮才发现不是排版问题）。

写法上刻意复用 verify_bvh 的 click_client —— 它带"第一次注入点击会被窗口
激活吃掉"的前置知识，这里同样先做一次预热点击。

用法：
    python tools/_probe_ui_variants.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32      # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors, wait_rect   # noqa: E402
from verify_bvh import click_client                                 # noqa: E402
from verify_asset_drag import force_foreground                      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT = os.path.join(ROOT, "..", "_venn_verify")

# 场景刻意让**被点中的那个物体**（Ground，视口中心那一下正好打在它上面）
# 带上 双面 + 法线图 → 掩码 = 1|4 = 5，Inspector 那行才看得出关键字组合。
# 另外两件用来让 Stats 面板的掩码列表不止一项。
SCENE = os.path.join(OUT, "ui_variants_scene.json")
CAM = "0,-20,3,90,0"


def _mat(name, base, double_sided=False, alpha_blend=False, normal=False):
    m = {
        "kind": "procedural", "name": name,
        "baseColor": list(base) + [0.35 if alpha_blend else 1.0],
        "metallic": 0.0, "roughness": 0.55, "normalScale": 1.0, "ao": 1.0,
        "emissive": [0.0, 0.0, 0.0],
        "doubleSided": double_sided, "alphaBlend": alpha_blend,
    }
    if normal:
        m["normal"] = {"kind": "checker", "name": "ui_normal_checker",
                       "colorA": [128, 128, 255, 255],
                       "colorB": [220, 128, 255, 255],
                       "cell": 24, "size": 256}
    return m


def _ent(name, pos, shape, size, mat, rot=(0, 0, 0)):
    return {"name": name,
            "transform": {"position": list(pos), "rotation": list(rot),
                          "scale": [1, 1, 1]},
            "visible": True, "castShadow": True, "parent": -1,
            "mesh": {"kind": "builtin", "shape": shape, "size": size,
                     "name": name.lower()},
            "material": mat}


def write_scene(path):
    import json
    import math
    half = math.pi * 0.5
    doc = {
        "version": 1, "generator": "_probe_ui_variants.py",
        "light": {"direction": [-0.35, -0.5, -0.79],
                  "color": [1.0, 0.97, 0.92], "intensity": 0.9,
                  "castsShadow": True, "ambientScale": 0.30},
        "entities": [
            # 掩码 5 = double-sided | normal-map（视口中心那一下打中的就是它）
            _ent("Ground", (0, 0, 0), "plane", 80.0,
                 _mat("ui_ground_mat", (0.72, 0.72, 0.76),
                      double_sided=True, normal=True)),
            # 掩码 0
            _ent("CubePlain", (-8, 6, 2), "cube", 4.0,
                 _mat("ui_cube_plain_mat", (0.85, 0.45, 0.30))),
            # 掩码 2 = alpha-blend
            _ent("Glass", (8, 6, 4), "plane", 10.0,
                 _mat("ui_glass_mat", (0.9, 0.95, 1.0), alpha_blend=True),
                 rot=(half, 0, 0)),
        ],
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


def main():
    kill_stale_editors()
    os.makedirs(OUT, exist_ok=True)
    write_scene(SCENE)

    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_LIGHT_GIZMOS"] = "0"
    env["MYVK_SHADER_VARIANTS"] = "1"
    env["MYVK_FRUSTUM_DBG"] = "1"
    env["MYVK_EDITOR_SCENE"] = SCENE
    env["MYVK_EDITOR_CAM"] = CAM

    proc = __import__("subprocess").Popen(
        [EXE], cwd=os.path.dirname(EXE), env=env,
        stdout=__import__("subprocess").PIPE,
        stderr=__import__("subprocess").STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1)
    tap = LogTap()
    tap.start(proc)

    hwnd = None
    for _ in range(40):
        hwnd = find_window("Venn Editor")
        if hwnd:
            break
        time.sleep(0.25)
    if not hwnd:
        print("未找到编辑器窗口")
        return 1

    try:
        user32.ShowWindow(hwnd, 9)
        time.sleep(1.5)
        # 尽量高：Inspector 的材质区很长（Base Color → 到 Shader Variant 共 10 行），
        # 面板不够高的话最后那行会被自己的裁剪区切掉。
        user32.SetWindowPos(hwnd, 0, 0, 0, 1500, 1000, 0x0002 | 0x0004)
        time.sleep(2.5)
        force_foreground(hwnd)
        # ⚠ 必须在 ShowWindow/SetWindowPos **之后**再清 imgui.ini 类存档带来的
        #   面板位置 —— 但 ImGui 读到 ini 是在初始化时，所以这个脚本要在
        #   "build/bin/imgui.ini 已被移开"的前提下跑（见文件头注释）。

        vp = wait_rect(tap, "VP-RECT", 10.0)
        print("VP-RECT =", vp)
        if not vp:
            return 2
        vx0, vy0, vx1, vy1 = vp

        # 预热点击（第一次注入点击会被窗口激活吃掉）
        click_client(hwnd, vx0 + (vx1 - vx0) * 0.94, vy0 + (vy1 - vy0) * 0.94)
        time.sleep(1.0)

        # 点视口选地面 -> Inspector 出现材质区。
        # ⚠ 视口矩形会随面板布局（imgui.ini）在两次启动间漂移几十像素，
        #   固定一个比例点会时中时不中（实测 (0.50,0.70) 有时落在天空上
        #   → `pick: nothing hit`）。所以按候选点依次试，命中就停。
        picked = None
        for fy in (0.70, 0.80, 0.62, 0.86, 0.90, 0.55):
            for fx in (0.50, 0.35, 0.65):
                click_client(hwnd, vx0 + (vx1 - vx0) * fx,
                             vy0 + (vy1 - vy0) * fy)
                time.sleep(0.8)
                with tap.lock:
                    hits = [l for l in tap.lines if "pick: selected" in l]
                if hits:
                    picked = hits[-1].strip()
                    break
            if picked:
                break
        time.sleep(0.8)

        with tap.lock:
            picks = [l for l in tap.lines if "pick:" in l]
            vars_ = [l for l in tap.lines if "SHADER-VARIANTS " in l]
        print("pick 日志 :", picks[-1].strip() if picks else "(无)")
        print("变体日志 :", vars_[-1].strip() if vars_ else "(无)")

        img, _ = capture_client(hwnd)
        path = os.path.join(OUT, "ui_variants.png")
        img.save(path)
        print("截图 ->", path, img.size)
    finally:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.5)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=6)
            except Exception:
                proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
