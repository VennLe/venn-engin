"""端到端验证：第三人称相机的三个老毛病（用户第七轮报的 B / C 两条）。

用户原话：
  · "角色相机视野在往上仰视和往下俯视的角度有些不正常，要符合人类生活中的
     现实场景。"
  · "角色在有障碍物或者墙体附近移动时会时常发现角色相机卡进场景物体中，
     比如墙体等障碍物，这样直接视野就变成无效的障碍物内了。关于这一点你
     可以借鉴 ue5 中的相关处理方案。"

对应到代码里是三件事，这个脚本逐条钉住：

  [1] **俯仰范围**按人的颈椎给（仰 55° / 俯 70°，上下不对称），且运行时与
      编辑器 Play **共用同一份常量**（`engine/scene/LookLimits.h`）。
      判据：`Game.exe --selftest player` 的 E 组；外加静态检查"两边都 include
      了 LookLimits.h、且不再各自写死角度"。
  [2] **弹簧臂 = 球扫掠 + 穿透优先**（UE SpringArmComponent 的思路）。
      判据：`Game.exe --selftest springarm` 的 S 组。其中 S3（半墙）是"单射线
      会漏、球扫掠能发现"的最小模型；S4/S5 是"宁可贴脸也不留在墙里"。
  [3] **地面钳制**：仰视到极限时相机不许沉到地表以下，且被顶高之后镜头
      仍然看着角色。判据：player 自检的 F 组 + 编辑器 Play 的贴墙场景。

编辑器 Play 那一段用**四面围起来的 2.4 m 盒子**当场景：默认臂长（按角色
身高算出来 5~6 m）无论朝哪个方向都会穿墙，所以只要相机的 x/y 还落在盒子
内部、"臂长 < 墙距"，就说明弹簧臂真的在工作。
（旧实现在这里 cam 会落在墙**外**，因为编辑器 Play 那条链一点碰撞都没有。）

用法：
    python tools/verify_third_person_camera.py
退出码 0 = 全过。
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import launcher_kit as kit                                          # noqa: E402
from capture_window import capture_stable, find_window              # noqa: E402
from verify_add_menu import LogTap, kill_stale_editors              # noqa: E402
from verify_landscape_ground import focus_window                    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
GAME = os.path.join(BIN, "Game.exe")
OUT = os.path.join(ROOT, "_venn_verify")
SCENES = os.path.join(BIN, "assets", "scenes")

# 盒子的半边长（墙的内表面到中心的距离）与墙厚
BOX_HALF = 1.20
WALL_THICK = 0.20
WALL_HEIGHT = 3.0

STATE_RE = re.compile(
    r"PLAYER-STATE.*pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\).*"
    r"yaw=([-\d.]+) pitch=([-\d.]+) arm=([-\d.]+) "
    r"cam=\(([-\d.]+),([-\d.]+),([-\d.]+)\)")

failures = []


def fail(msg):
    failures.append(msg)
    print("  ✗ " + msg)


def ok(msg):
    print("  ✓ " + msg)


# ---------------------------------------------------------------- 场景
def build_scene(path):
    """四面墙围成的盒子 + 地面 + GameStart。

    为什么不做成"一面墙 + 角色背对它"：相机默认朝角色的**正后方**，
    而"正后方"是哪个方向取决于模型朝向 / bodyYaw 的初值 —— 赌错方向这条
    用例就永远绿（相机在空地上，臂长自然不缩短）。四面都堵死，无论初始
    朝向如何都必须缩臂。
    """
    ents = [{
        "name": "Ground",
        "transform": {"position": [0.0, 0.0, 0.0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "parent": -1, "visible": True, "castShadow": False, "locked": True,
        "mesh": {"kind": "builtin", "shape": "plane", "size": 40.0,
                 "name": "sp_ground"},
        "material": {"kind": "color", "color": [0.55, 0.58, 0.62, 1.0],
                     "roughness": 0.9, "metallic": 0.0,
                     "name": "sp_ground_mat"},
    }, {
        "name": "GameStart",
        "transform": {"position": [0.0, 0.0, 0.0], "rotation": [0, 0, 0],
                      "scale": [1, 1, 1]},
        "parent": -1, "visible": True, "castShadow": False,
        "gameStart": {"yaw": 0.0, "enabled": True},
    }]

    # 四面墙：沿 X 两面（在 y=0 处横跨），沿 Y 两面
    walls = [
        ("WallXM", (-BOX_HALF, 0.0, WALL_HEIGHT * 0.5),
         (WALL_THICK, BOX_HALF * 2, WALL_HEIGHT)),
        ("WallXP", (BOX_HALF, 0.0, WALL_HEIGHT * 0.5),
         (WALL_THICK, BOX_HALF * 2, WALL_HEIGHT)),
        ("WallYM", (0.0, -BOX_HALF, WALL_HEIGHT * 0.5),
         (BOX_HALF * 2, WALL_THICK, WALL_HEIGHT)),
        ("WallYP", (0.0, BOX_HALF, WALL_HEIGHT * 0.5),
         (BOX_HALF * 2, WALL_THICK, WALL_HEIGHT)),
    ]
    for name, pos, scale in walls:
        ents.append({
            "name": name,
            "transform": {"position": list(pos), "rotation": [0, 0, 0],
                          "scale": list(scale)},
            "parent": -1, "visible": True, "castShadow": True,
            "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                     "name": "sp_" + name},
            "material": {"kind": "color", "color": [0.62, 0.60, 0.56, 1.0],
                         "roughness": 0.85, "metallic": 0.0,
                         "name": "sp_" + name + "_mat"},
        })

    doc = {
        "version": 1,
        "generator": "verify_third_person_camera.py",
        "camera": {"target": [0.0, 0.0, 1.2], "yaw": 3.14159265,
                   "pitch": 0.30, "distance": 5.0, "fov": 55.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.0, "castsShadow": False,
                  "ambientScale": 1.4},
        "physics": {"enabled": True, "groundPlane": True},
        "entities": ents,
    }
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
    return "scenes/" + os.path.basename(path)


# ---------------------------------------------------------------- 自检
def run_selftest(name, patterns):
    """跑一个无窗口自检，返回 {断言名: (是否 PASS, 明细)}。"""
    proc = subprocess.run([GAME, "--selftest", name], cwd=BIN,
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=180)
    out = proc.stdout + proc.stderr
    res = {}
    for tag in patterns:
        m = re.search(r"^\s*(\w+-TEST) (pass|FAIL) " + re.escape(tag) + r"(.*)$",
                      out, re.MULTILINE)
        if not m:
            res[tag] = (False, "没找到这条断言")
        else:
            res[tag] = (m.group(2) == "pass", m.group(3).strip(" —"))
    total = re.search(r"(\w+-TOTAL) pass=(\d+) fail=(\d+)", out)
    return res, (proc.returncode, total.group(0) if total else "无 TOTAL 行")


# ---------------------------------------------------------------- 主流程
def main():
    os.makedirs(OUT, exist_ok=True)

    # ---- [1] 俯仰范围（运行时）----
    print("[1] 俯仰范围：仰 55° / 俯 70°（上下不对称，按人的颈椎给）")
    res, (rc, total) = run_selftest(
        "player", ["E1 仰视上限", "E2 俯视上限", "E3 仰视比俯视窄",
                   "F1 仰视到极限时相机仍在地表之上",
                   "F2 反解的 pitch", "F3 臂长 =="])
    for tag, (good, detail) in res.items():
        (ok if good else fail)("%s — %s" % (tag, detail))
    if rc != 0:
        fail("player 自检退出码 %d（%s）" % (rc, total))

    # ---- [2] 弹簧臂（纯几何）----
    print("\n[2] 弹簧臂：球扫掠 + 穿透优先（UE SpringArm 的三条要点）")
    res, (rc, total) = run_selftest(
        "springarm", ["S2 正墙", "S3 半墙", "S4 墙比最小臂长还近时",
                      "S5 极窄空间：臂长止于硬下限", "S7d 用反解的"])
    for tag, (good, detail) in res.items():
        (ok if good else fail)("%s — %s" % (tag, detail))
    if rc != 0:
        fail("springarm 自检退出码 %d（%s）" % (rc, total))

    # ---- [3] 常量单一来源（静态检查）----
    #
    # 运行时与编辑器 Play 是两条链，pitch 的符号还相反。它们必须都从
    # LookLimits.h 取角度 —— 各自写死就等于"改一处忘了另一处"。
    print("\n[3] 俯仰常量只有一个来源（engine/scene/LookLimits.h）")
    hdr = os.path.join(ROOT, "engine", "scene", "LookLimits.h")
    if not os.path.exists(hdr):
        fail("LookLimits.h 不存在")
    else:
        src = open(hdr, encoding="utf-8").read()
        if "kLookUpDeg = 55" in src and "kLookDownDeg = 70" in src:
            ok("LookLimits.h: 仰 55° / 俯 70°")
        else:
            fail("LookLimits.h 的值不是 55/70")
    for rel, must in (("runtime/PlayerController.cpp",
                       ["scene/LookLimits.h", "scene::kLookUpRad",
                        "scene::kLookDownRad"]),
                      ("editor/EditorApp.cpp",
                       ["scene/LookLimits.h", "scene::kLookUpRad",
                        "scene::kLookDownRad"])):
        path = os.path.join(ROOT, rel)
        txt = open(path, encoding="utf-8").read()
        missing = [m for m in must if m not in txt]
        if missing:
            fail("%s 缺少 %s（又写死了一份？）" % (rel, ", ".join(missing)))
        else:
            ok("%s 使用共享常量" % rel)

    # ---- [4] 编辑器 Play 贴墙：相机不进墙 ----
    print("\n[4] 编辑器 Play：四面围死 2.4 m 盒子，相机不许落在墙里")
    scene_rel = build_scene(os.path.join(SCENES, "_verify_springarm.json"))
    kill_stale_editors()

    env = kit.clean_env({
        "MYVK_EDITOR_SCENE": scene_rel,
        "MYVK_EDITOR_PLAY": "1",
        "MYVK_NO_WINDOW_SAVE": "1",
        "MYVK_EDITOR_NO_HUD": "1",
        "MYVK_EDITOR_LANG": "zh",
    })
    proc = subprocess.Popen([EXE], cwd=BIN, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        hwnd = find_window("Venn Editor", timeout=60)
        if not hwnd:
            fail("编辑器窗口没出现")
            return 1
        focus_window(hwnd)
        time.sleep(11.0)

        last = None
        for line in tap.lines:
            m = STATE_RE.search(line)
            if m:
                last = m
        if not last:
            fail("没有 PLAYER-STATE 行（arm/cam 字段是不是被删了？）")
            for line in tap.lines[-15:]:
                print("     " + line.strip())
            return 1

        px, py, pz = (float(last.group(i)) for i in (1, 2, 3))
        pitch = float(last.group(5))
        arm = float(last.group(6))
        cx, cy, cz = (float(last.group(i)) for i in (7, 8, 9))
        print("    pos=(%.2f,%.2f,%.2f) pitch=%.3f arm=%.3f cam=(%.2f,%.2f,%.2f)"
              % (px, py, pz, pitch, arm, cx, cy, cz))

        # 墙的内表面在 ±1.20，相机中心必须还在里面
        if abs(cx) < BOX_HALF + 0.05 and abs(cy) < BOX_HALF + 0.05:
            ok("相机仍在盒子内部（|x|=%.2f |y|=%.2f < %.2f）"
               % (abs(cx), abs(cy), BOX_HALF))
        else:
            fail("相机落到墙外：cam=(%.2f,%.2f) 而墙在 ±%.2f"
                 % (cx, cy, BOX_HALF))
        if arm < BOX_HALF:
            ok("臂长被压到墙距以内（arm=%.2f < %.2f）" % (arm, BOX_HALF))
        else:
            fail("臂长没被压制：arm=%.2f（默认臂长是 5~6 m，说明碰撞没生效）"
                 % arm)
        if cz > -0.05:
            ok("相机没有沉到地面以下（cam.z=%.2f）" % cz)
        else:
            fail("相机在地面以下：cam.z=%.2f" % cz)
        # 俯仰范围：默认 17°，只要没被钳到边界就说明还在合理区间
        if -1.0 < pitch < 1.3:
            ok("俯仰落在人眼范围内（pitch=%.3f rad = %.1f°）"
               % (pitch, pitch * 57.29578))
        else:
            fail("俯仰超出范围：pitch=%.3f" % pitch)

        img = capture_stable(hwnd, tries=20, delay=0.4)
        if img:
            p = os.path.join(OUT, "_edit_springarm_box.png")
            img.save(p)
            print("    截图 %s" % p)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except Exception:
            pass

    print("\n" + "=" * 66)
    if failures:
        print("结果: FAIL（%d 项）" % len(failures))
        for f in failures:
            print("  · " + f)
        return 1
    print("结果: PASS（俯仰范围 / 弹簧臂 / 地面钳制 全部通过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
