#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/verify_terrain_physics.py —— 地形是"真的地面"吗（第十轮）

被验证的用户需求（原话）：
  "生成的地形地面就是真实的地面，物理特性上不允许说有任何物体时能陷入
   其中的，包括 GameStart。"

在此之前地形**只**是渲染物：
  · `PhysicsWorld` 里没有它；
  · `CharacterDesc::ground` 只在自检里被赋值过；
  · 模板地形在原点处是 3.35 m（归一化基准取的是全局最低点），而 GameStart
    写的是 z=0 —— 一开局人的脚就在土里 3.35 m。
于是"新工程一进去，玩家埋在地里；掉下来的箱子穿到地面以下"。

断言（A/B/C 三组）：
  A. 地形真的接进了物理
     A1 `PHYS-TERRAIN source=... terrains=1`（读到了场景里的 .vterrain）
     A2 `PHYS-TERRAIN attach ... proxyBody=`（求解器拿到了代表体）
  B. 玩家站在地面上，不埋不浮
     B1 `GAME-PLAYER attached`
     B2 稳态 `GAME-PLAYER-POS ... ground=1`
     B3 玩家脚底 z ≈ 出生点处的地形高度（模板已把出生点整成 0）
        —— 这一步同时验了"模板地形原点 = 0 m"这个数据修复
  C. 刚体不会陷进地形
     C1 平地上落下的箱子：静止后 z ≈ 地形高度 + 半高（±6 cm）
     C2 斜坡上落下的箱子：静止后 z ≈ **该点地形高度** + 半高
        （如果地形碰撞没接上，它会停在 z = 半高 —— 也就是穿到地下）
     C3 两个箱子的底面都不低于地表 5 cm 以上
  D. 日志健康：零 VUID / 零 ERROR

地形高度用 Python **重算一遍**（与 `physics::heightAt` 同一个双线性公式），
不是"读 logs 里吐的数字"—— 否则物理侧算错了也验不出来。

用法：python tools/verify_terrain_physics.py
"""

import json
import math
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_kit as kit  # noqa: E402

WORK = os.path.join(kit.WORK_ROOT, "terrain_physics")
KIND = "third_person"
PROJECT_NAME = "VennTerrainPhys"   # 窗口标题以 "Venn" 开头（find_window 靠它）
FRAMES = 360          # 6 秒 @60fps：从 8~12 m 落下足够稳


# ---------------------------------------------------------------- 高度场复算

def load_heightfield(vterrain_path):
    with open(vterrain_path, encoding="utf-8") as f:
        desc = json.load(f)
    base = os.path.dirname(vterrain_path)
    with open(os.path.join(base, desc["height"]), "rb") as f:
        raw = f.read()
    n = len(raw) // 4
    heights = struct.unpack("<%df" % n, raw)
    return {
        "cols": desc["cols"],
        "rows": desc["rows"],
        "mpp": desc["metersPerPixel"],
        "maxh": desc["maxHeight"],
        "origin": (-0.5 * (desc["cols"] - 1) * desc["metersPerPixel"],
                   -0.5 * (desc["rows"] - 1) * desc["metersPerPixel"]),
        "heights": heights,
    }


def height_at(hf, x, y):
    """逐字复刻 `physics::heightAt`（列主序 + 双线性 + 边缘钳制）。"""
    cols, rows = hf["cols"], hf["rows"]

    def raw(c, r):
        c = max(0, min(c, cols - 1))
        r = max(0, min(r, rows - 1))
        return hf["heights"][c * rows + r]

    gc = max(0.0, min((x - hf["origin"][0]) / hf["mpp"], cols - 1.0))
    gr = max(0.0, min((y - hf["origin"][1]) / hf["mpp"], rows - 1.0))
    c0, r0 = int(math.floor(gc)), int(math.floor(gr))
    fc, fr = gc - c0, gr - r0
    if c0 >= cols - 1:
        c0, fc = cols - 2, 1.0
    if r0 >= rows - 1:
        r0, fr = rows - 2, 1.0
    c1, r1 = c0 + 1, r0 + 1
    a = raw(c0, r0) + (raw(c1, r0) - raw(c0, r0)) * fc
    c = raw(c0, r1) + (raw(c1, r1) - raw(c0, r1)) * fc
    return (a + (c - a) * fr) * hf["maxh"]


def pick_slope_spot(hf, lo=1.2, hi=2.4):
    """找一个"地形明显高出地面 0 m、但脚下够平"的点放箱子。

    · 高度要在 [lo, hi]：太矮分不出"停在地形上"还是"停在 z=0 平面"；
    · 局部坡度要小：箱子落在陡坡上会滚走，断言会变成"箱子不在原点"。
    """
    best = None
    step = 8          # 米
    for ix in range(-60, 61):
        for iy in range(-60, 61):
            x, y = ix * step, iy * step
            if abs(x) < 24 and abs(y) < 24:
                continue          # 离出生点远一点，避免和"压平区"混在一起
            h = height_at(hf, x, y)
            if not (lo <= h <= hi):
                continue
            d = 4.0
            slope = max(abs(height_at(hf, x + d, y) - height_at(hf, x - d, y)),
                        abs(height_at(hf, x, y + d) - height_at(hf, x, y - d)))
            slope /= (2.0 * d)
            if slope > 0.06:      # < 3.4°
                continue
            if best is None or slope < best[2]:
                best = (x, y, slope, h)
    return best


# ---------------------------------------------------------------- 场景

def build_scene(hf, slope_xy):
    """模板场景 + 两个落体箱子（平地 / 斜坡）。"""
    scene = {
        "version": 1,
        "generator": "verify_terrain_physics.py",
        "camera": {"target": [0.0, 0.0, 1.2], "yaw": 3.141592653589793,
                   "pitch": 0.35, "distance": 4.5, "fov": 55.0},
        "physics": {"enabled": True, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4},
        "entities": [
            {
                "name": "Terrain",
                "transform": {"position": [0.0, 0.0, 0.0],
                              "rotation": [0.0, 0.0, 0.0],
                              "scale": [1.0, 1.0, 1.0]},
                "parent": -1, "visible": True, "castShadow": False,
                "locked": True,
                "terrain": {"source": "terrain/ground/terrain.vterrain",
                            "visible": True, "heightScale": 1.0,
                            "layers": ["", "", "", ""]},
            },
            {
                "name": "GameStart",
                "transform": {"position": [0.0, 0.0, 0.0],
                              "rotation": [0.0, 0.0, 0.0],
                              "scale": [1.0, 1.0, 1.0]},
                "parent": -1, "visible": True, "castShadow": False,
                "gameStart": {"yaw": 0.0, "enabled": True},
            },
        ],
    }

    def box(name, x, y, z, color):
        return {
            "name": name,
            "transform": {"position": [x, y, z],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1, "visible": True, "castShadow": True,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "tp_" + name},
            "primitive": {"kind": "box", "size": 1.0},
            "material": {"kind": "procedural", "name": "tp_" + name + "_mat",
                         "baseColor": color, "metallic": 0.0, "roughness": 0.6,
                         "normalScale": 0.0, "ao": 1.0,
                         "emissive": [0.0, 0.0, 0.0],
                         "doubleSided": False, "alphaBlend": False},
            "rigidbody": {"bodyType": 2, "shape": 2,
                          "halfExtents": [0.5, 0.5, 0.5],
                          "density": 400.0, "useGravity": True,
                          "allowSleep": True},
        }

    # ⚠ 不能放在 (0,0)：那是玩家出生点，箱子会**落在人头顶上**
    #   （实测停在 z=2.295 = 胶囊顶 1.80 + 半高 0.5），断言的"停在地面"
    #   就变成了"停在玩家身上"。挪到出生点东南 6 m 的平地上。
    scene["entities"].append(box("BoxFlat", 6.0, 0.0, 8.0,
                                 [0.82, 0.36, 0.26, 1.0]))
    scene["entities"].append(box("BoxSlope", slope_xy[0], slope_xy[1], 12.0,
                                 [0.32, 0.48, 0.78, 1.0]))
    return scene


# ---------------------------------------------------------------- 主流程

def substitute(root, name, kind):
    """把模板里的 `__PROJECT_NAME__` / `__TEMPLATE_ID__` 换掉（同创建流程）。"""
    for dirpath, _dirs, files in os.walk(root):
        for fn in files:
            if not fn.endswith((".vks", ".json", ".md")):
                continue
            p = os.path.join(dirpath, fn)
            with open(p, encoding="utf-8", newline="") as f:
                text = f.read()
            out = text.replace("__PROJECT_NAME__", name).replace(
                "__TEMPLATE_ID__", kind)
            if out != text:
                with open(p, "w", encoding="utf-8", newline="\n") as f:
                    f.write(out)


def fnum(line, key):
    m = re.search(r"%s=\((-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)\)"
                  % key, line or "")
    return (float(m.group(1)), float(m.group(2)), float(m.group(3))) if m else None


def main():
    fails = []
    if not os.path.isfile(kit.GAME):
        kit.log("结果: FAIL: Game.exe 不存在（先 make build）")
        return 1

    root = os.path.join(WORK, KIND)
    kit.make_project(root, KIND, name=PROJECT_NAME)
    # ⚠ 占位符必须替换：`game.json` 的 title 是 `__PROJECT_NAME__`，
    #   窗口标题就用它 —— 而 `find_window` 是按**前缀**找窗口的，
    #   不替换的话 `find_window("Venn")` 永远找不到（截图会失败）。
    substitute(root, PROJECT_NAME, KIND)
    vterrain = os.path.join(root, "assets", "terrain", "ground",
                            "terrain.vterrain")
    hf = load_heightfield(vterrain)
    kit.log("PROBE terrain %dx%d mpp=%.1f maxh=%.4f origin=(%.1f,%.1f) "
            "h(0,0)=%.4f"
            % (hf["cols"], hf["rows"], hf["mpp"], hf["maxh"], hf["origin"][0],
               hf["origin"][1], height_at(hf, 0.0, 0.0)))

    # ---- 出生点必须正好在地面上（模板数据修复的硬证据）----
    h0 = height_at(hf, 0.0, 0.0)
    if abs(h0) > 0.02:
        fails.append("A0 模板地形在原点处不是 0 m（%.4f）—— GameStart 会陷进去"
                     % h0)
    else:
        kit.log("CHECK A0 出生点处地形高度 = %.4f m（GameStart 站在 (0,0,0)）OK"
                % h0)

    spot = pick_slope_spot(hf)
    if spot is None:
        fails.append("A0b 没找到合适的斜坡测试点（地形太平？）")
        slope_xy = (60.0, 60.0)
        h_slope = height_at(hf, 60.0, 60.0)
    else:
        slope_xy = (spot[0], spot[1])
        h_slope = spot[3]
        kit.log("PROBE slope spot=(%.0f,%.0f) h=%.3f slope=%.4f"
                % (spot[0], spot[1], spot[3], spot[2]))

    scene_path = os.path.join(root, "assets", "scenes", "terrain_phys.json")
    with open(scene_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(build_scene(hf, slope_xy), f, ensure_ascii=False, indent=2)
        f.write("\n")

    env = kit.clean_env({
        "MYVK_ASSET_DIR": os.path.join(root, "assets"),
        "MYVK_RUNTIME_SCENE": "scenes/terrain_phys.json",
        "MYVK_RUNTIME_START_STATE": "playing",
        "MYVK_PHYS_DUMP": "60",
        "MYVK_RUNTIME_PLAYER_DUMP": "60",
        "MYVK_FRAMES": str(FRAMES),
        "MYVK_NO_WINDOW_SAVE": "1",
    })
    proc = subprocess.Popen([kit.GAME], cwd=root, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = kit.LogTap()
    tap.start(proc)

    hwnd = kit.find_window("Venn", timeout=60.0)
    if not hwnd:
        proc.kill()
        kit.log("结果: FAIL: 游戏窗口没出现")
        return 1
    kit.focus_window(hwnd)

    try:
        # 跑满 FRAMES 帧自己退出（MYVK_FRAMES）
        try:
            proc.wait(timeout=180)
        except subprocess.TimeoutExpired:
            proc.kill()
            fails.append("D0 运行时没有在 %d 帧后自己退出" % FRAMES)
    finally:
        try:
            kit.capture(hwnd, os.path.join(WORK, "terrain_physics.png"))
        except Exception:
            pass
        kit.close_window(hwnd)

    txt = tap.text()
    with open(os.path.join(WORK, "terrain_physics.log"), "w",
              encoding="utf-8") as f:
        f.write(txt)

    # ---- A. 地形接进物理 ----
    if not re.search(r"PHYS-TERRAIN source=terrain/ground/terrain\.vterrain",
                     txt):
        fails.append("A1 没有读到场景里的地形（无 PHYS-TERRAIN source=…）")
    else:
        m = re.search(r"PHYS-TERRAIN source=.*?terrains=(\d+)", txt)
        kit.log("CHECK A1 地形已接进物理（terrains=%s）OK"
                % (m.group(1) if m else "?"))
    if "PHYS-TERRAIN attach" not in txt:
        fails.append("A2 求解器没拿到地形代表体（无 PHYS-TERRAIN attach）")
    else:
        kit.log("CHECK A2 求解器代表体已创建 OK")

    # ---- B. 玩家站得住 ----
    if "GAME-PLAYER attached" not in txt:
        fails.append("B1 玩家没有被 PlayerController 接管")
    else:
        kit.log("CHECK B1 玩家已接管 OK")

    pos_lines = re.findall(r"GAME-PLAYER-POS f=\d+ pos=\([^)]*\) ground=\d",
                           txt)
    if not pos_lines:
        fails.append("B2 没有 GAME-PLAYER-POS 采样（运行时没进仿真？）")
    else:
        last = pos_lines[-1]
        p = fnum(last.replace("ground=", "pos="), "pos") or fnum(last, "pos")
        grounded = re.search(r"ground=(\d)", last)
        if not grounded or grounded.group(1) != "1":
            fails.append("B2 稳态时玩家没有贴地（%s）" % last.strip())
        else:
            kit.log("CHECK B2 稳态贴地 OK（%s）" % last.strip())
        if p:
            feet = p[2] - 0.90     # 胶囊 halfHeight 0.55 + radius 0.35
            if abs(feet - h0) > 0.08:
                fails.append("B3 玩家脚底 %.3f m 与地形高度 %.3f m 差得太多"
                             % (feet, h0))
            else:
                kit.log("CHECK B3 玩家脚底 %.3f m ≈ 地形 %.3f m OK"
                        % (feet, h0))

    # ---- C. 刚体不陷入地形 ----
    def last_box(name):
        hits = re.findall(r"PHYS-BODY f=\d+ name='%s' pos=\(([^)]*)\)" % name,
                          txt)
        if not hits:
            return None
        return [float(v) for v in hits[-1].split(",")]

    h_flat = height_at(hf, 6.0, 0.0)
    for box_name, expect_h in (("BoxFlat", h_flat), ("BoxSlope", h_slope)):
        p = last_box(box_name)
        if p is None:
            fails.append("C1 %s 没有 PHYS-BODY 采样" % box_name)
            continue
        bottom = p[2] - 0.5
        want = expect_h + 0.5
        if abs(p[2] - want) > 0.06:
            fails.append("C %s 静止高度 z=%.3f，期望 %.3f（地形 %.3f + 半高 0.5）"
                         % (box_name, p[2], want, expect_h))
        else:
            kit.log("CHECK C %s 停在地形上 z=%.3f（地形 %.3f + 0.5）OK"
                    % (box_name, p[2], expect_h))
        if bottom < expect_h - 0.05:
            fails.append("C %s 底面 %.3f m 陷进地形（地表 %.3f m）%.3f m"
                         % (box_name, bottom, expect_h, expect_h - bottom))
        else:
            kit.log("CHECK C %s 底面 %.3f m ≥ 地表 %.3f m（没陷进去）OK"
                    % (box_name, bottom, expect_h))

    # ---- D. 日志健康 ----
    bad = [l for l in txt.splitlines()
           if "VUID-" in l or "[ERROR]" in l or "Validation Error" in l]
    if bad:
        fails.append("D1 有 %d 条 VUID/ERROR，首条: %s" % (len(bad), bad[0][:160]))
    else:
        kit.log("CHECK D1 零 VUID / 零 ERROR OK")

    return kit.finish(fails, keep_msg="截图：%s"
                      % os.path.join(WORK, "terrain_physics.png"))


if __name__ == "__main__":
    sys.exit(main())
