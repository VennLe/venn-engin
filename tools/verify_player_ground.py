"""验收：脚踩在地形表面上，不陷到地面以下（P6，2026-10-22）。

用户原话：

  "角色在踩在地形的地面时，应该是脚部踩在上面的，不允许脚底踩到地形
   地面的下方。"

判据（每条都能单独变红）：

  G0 地形真的进了物理     `PHYS-TERRAIN source=terrain/hills/terrain.vterrain`
  G1 探针在跑            `GAME-FEET ... feetZ=... surfZ=... gap=...`
  G2 站着不陷            稳定后每一帧 gap ∈ [-0.02, +0.30]
  G3 走路不陷            按住 W 1.5 s，全程 gap ≥ -0.02
  G4 跑步（下坡更快）不陷 右键 + W 2.0 s，全程 gap ≥ -0.02
  G5 确实在坡上（不是平地代跑）surfZ 的极差 ≥ 0.02 m
  G6 跳跃落地不陷        落地后 0.5 s 内 gap ≥ -0.02
  G7 无崩溃              0 条 VK_CHECK / 0 条 invalid state

为什么用 gap 而不是"绝对高度"：`feetZ` 是**实体原点**（= 脚底），`surfZ`
是地形高度场在 (x,y) 处的插值（与贴地用的是同一个 `physics::heightAt`）。
两者的差就是"脚离地面多少"，和场景整体被抬高多少无关。

⚠ 容差 2 cm：贴地吸附是每帧比较 + 抬升，帧与帧之间会有一两毫米的抖动；
  真陷进去是**几厘米到几十厘米**（用户报的现象是整只脚没了），2 cm 足够
  把"抖动"和"故障"分开。

用法：
    python tools/verify_player_ground.py
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32       # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
ASSETS = os.path.join(BIN, "assets")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "player_ground")

SCENE_REL = "scenes/levels/_p6_ground.json"
SCENE_ABS = os.path.join(ASSETS, SCENE_REL)
GAMEJSON = os.path.join(ASSETS, "_p6_ground_game.json")

# 出生点：hills 地形在原点附近的地表约 17.3 m（terrain_probe 里的箱子
# z≈17.58 就是压在这个高度上）。给 1.2 m 余量让它自己落下来 —— 顺便验证
# "落到地形上"这件事本身。
SPAWN = (0.0, 3.0, 18.5)

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
VK_W, VK_SPACE = 0x57, 0x20

RE_TERRAIN = re.compile(r"PHYS-TERRAIN source=(\S+)")
RE_FEET = re.compile(r"GAME-FEET f=(\d+) feetZ=([\d.-]+) surfZ=([\d.-]+) "
                     r"gap=([\d.+-]+)")
RE_POS = re.compile(r"GAME-PLAYER-POS f=(\d+) pos=\(([\d.-]+),([\d.-]+),"
                    r"([\d.-]+)\) ground=(\d)")

GAP_MIN = -0.02          # 低于这个就是"陷进去了"
GAP_MAX = 0.30           # 高于这个就是"浮空了"（吸附没生效）

g_pass = 0
g_fail = 0


def check(name, ok, detail=""):
    global g_pass, g_fail
    if ok:
        g_pass += 1
        print("[ OK ] %s%s" % (name, ("  " + detail) if detail else ""))
    else:
        g_fail += 1
        print("[FAIL] %s%s" % (name, ("  " + detail) if detail else ""))
    return ok


def _sc(vk):
    return user32.MapVirtualKeyW(vk, 0) & 0xFF


def key_down(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (_sc(vk) << 16) | 1)


def key_up(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (_sc(vk) << 16) | 0xC0000001)


def click(hwnd, x, y):
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, ((y & 0xFFFF) << 16) | (x & 0xFFFF))
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, ((y & 0xFFFF) << 16) | (x & 0xFFFF))


def mouse_down(hwnd, button, x=640, y=360):
    msg = WM_LBUTTONDOWN if button == 0 else WM_RBUTTONDOWN
    user32.PostMessageW(hwnd, msg, 1 if button == 0 else 2,
                        ((y & 0xFFFF) << 16) | (x & 0xFFFF))


def mouse_up(hwnd, button, x=640, y=360):
    msg = WM_LBUTTONUP if button == 0 else WM_RBUTTONUP
    user32.PostMessageW(hwnd, msg, 0, ((y & 0xFFFF) << 16) | (x & 0xFFFF))


def tap(hwnd, vk, hold=0.08):
    key_down(hwnd, vk)
    time.sleep(hold)
    key_up(hwnd, vk)


def write_inputs():
    scene = {
        "version": 1,
        "generator": "verify_player_ground.py",
        "camera": {"target": [0.0, 3.0, 18.0], "yaw": 3.141592653589793,
                   "pitch": 0.25, "distance": 6.0, "fov": 55.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": True, "groundPlane": False,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            {"name": "Terrain",
             "transform": {"position": [0.0, 0.0, 0.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "parent": -1, "visible": True, "castShadow": False,
             "locked": True, "mesh": None, "material": None,
             "terrain": {"source": "terrain/hills/terrain.vterrain",
                         "visible": True, "heightScale": 1.0,
                         "layers": ["", "", "", ""]}},
            {"name": "GameStart",
             "transform": {"position": list(SPAWN),
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "parent": -1, "visible": True, "castShadow": False,
             "gameStart": {"yaw": 0.0, "enabled": True}},
        ],
    }
    os.makedirs(os.path.dirname(SCENE_ABS), exist_ok=True)
    with open(SCENE_ABS, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)

    gj = {
        "scene": SCENE_REL,
        "title": "Venn Game",
        "width": 1280, "height": 720,
        "maxFps": 60, "startState": "playing", "language": "zh",
        "player": {"model": "models/characters/default_character.vmesh"},
    }
    with open(GAMEJSON, "w", encoding="utf-8") as f:
        json.dump(gj, f, ensure_ascii=False, indent=2)


def clean_env():
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_RUNTIME_GAME_JSON"] = GAMEJSON
    # 每 2 帧一行（60fps 下 33 ms 一个采样点）
    env["MYVK_RUNTIME_PLAYER_DUMP"] = "2"
    return env


def shot(hwnd, name):
    os.makedirs(OUT_DIR, exist_ok=True)
    try:
        capture_client(hwnd)[0].save(os.path.join(OUT_DIR, name))
    except Exception as ex:                                   # noqa: BLE001
        print("  截图失败 %s: %s" % (name, ex))


def read_log(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def feet_samples(lines):
    """[(帧号, feetZ, surfZ, gap)]"""
    out = []
    for line in lines:
        m = RE_FEET.search(line)
        if m:
            out.append((int(m.group(1)), float(m.group(2)), float(m.group(3)),
                        float(m.group(4))))
    return out


def seg(stamps, f0, f1):
    return [s for s in stamps if f0 <= s[0] <= f1]


def main():
    write_inputs()
    os.makedirs(OUT_DIR, exist_ok=True)
    logp = os.path.join(OUT_DIR, "ground.log")
    lf = open(logp, "w", encoding="utf-8", errors="replace")

    # ⚠ stdout 直接写文件：管道 64 KB 满了子进程会卡在 write 上
    p = subprocess.Popen([GAME], cwd=BIN, env=clean_env(),
                         stdout=lf, stderr=subprocess.STDOUT)
    hwnd = find_window("Venn Game", timeout=25.0)
    if not hwnd:
        p.kill()
        lf.close()
        print("结果: FAIL  (窗口没出现)")
        return 1

    try:
        time.sleep(3.0)             # 落地 + 稳定
        shot(hwnd, "g0_spawn.png")
        click(hwnd, 640, 360)       # 进操作态（左键点视口）
        # ⚠ 点击之前玩家是冻结的（鼠标没进操作态），出生点又在离地 1.4 m 的
        #   空中 —— 落地发生在**点击之后**。所以"站着不陷"要等落稳再采样，
        #   否则窗口里混进自由落体段（第一版就这么误报过一次）。
        time.sleep(1.8)
        settle = feet_samples(read_log(logp))
        f_settle = settle[-1][0] if settle else 0
        time.sleep(0.7)

        still = feet_samples(read_log(logp))
        f_still_end = still[-1][0] if still else f_settle
        # ---- 走 ----
        key_down(hwnd, VK_W)
        time.sleep(1.5)
        key_up(hwnd, VK_W)
        time.sleep(0.4)
        shot(hwnd, "g1_walk.png")
        walk = feet_samples(read_log(logp))
        f_walk_end = walk[-1][0] if walk else f_still_end

        # ---- 跑（右键 + W）----
        key_down(hwnd, VK_W)
        time.sleep(0.5)
        mouse_down(hwnd, 1)
        time.sleep(2.0)
        shot(hwnd, "g2_run.png")
        mouse_up(hwnd, 1)
        key_up(hwnd, VK_W)
        time.sleep(0.6)
        run = feet_samples(read_log(logp))
        f_run_end = run[-1][0] if run else f_walk_end

        # ---- 跳一下再落地 ----
        tap(hwnd, VK_SPACE, 0.08)
        time.sleep(1.4)
        shot(hwnd, "g3_after_jump.png")
        jump = feet_samples(read_log(logp))
    finally:
        time.sleep(0.2)
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        lf.flush()
        lf.close()

    lines = read_log(logp)
    if not lines:
        print("结果: FAIL  (日志是空的)")
        return 1

    # ---- G0 地形进物理 ----
    m_t = None
    for line in lines:
        m = RE_TERRAIN.search(line)
        if m and "terrain.vterrain" in m.group(1):
            m_t = m
            break
    if not check("G0 地形进了物理（PHYS-TERRAIN）", m_t is not None,
                 m_t.group(1) if m_t else "没有 PHYS-TERRAIN"):
        return 2

    # ---- G1 探针在跑 ----
    allf = jump or run or walk or still
    if not check("G1 脚底探针在跑（GAME-FEET）", bool(allf),
                 "%d 个采样点" % len(allf)):
        return 3

    def gaps(samples):
        return [s[3] for s in samples]

    def report(tag, samples):
        g = gaps(samples)
        if not g:
            return "无采样"
        return ("n=%d gap∈[%+.4f, %+.4f] surfZ∈[%.3f, %.3f]"
                % (len(g), min(g), max(g),
                   min(s[2] for s in samples), max(s[2] for s in samples)))

    # ---- G2 站着不陷 ----
    tail = seg(still, f_settle, f_still_end)
    g2 = gaps(tail)
    check("G2 站着时脚踩在地表上（不陷、不浮）",
          bool(g2) and min(g2) >= GAP_MIN and max(g2) <= GAP_MAX,
          report("still", tail))

    # ---- G3 走路不陷 ----
    walkseg = seg(walk, f_still_end, f_walk_end)
    g3 = gaps(walkseg)
    check("G3 走路全程脚底不低于地表 2 cm",
          bool(g3) and min(g3) >= GAP_MIN,
          report("walk", walkseg))

    # ---- G4 跑步不陷 ----
    runseg = seg(run, f_walk_end, f_run_end)
    g4 = gaps(runseg)
    check("G4 跑步（含下坡）全程脚底不低于地表 2 cm",
          bool(g4) and min(g4) >= GAP_MIN,
          report("run", runseg))

    # ---- G5 确实在坡上 ----
    allseg = seg(allf, 0, 1 << 62)
    surf = [s[2] for s in allseg]
    span = (max(surf) - min(surf)) if surf else 0.0
    check("G5 走的这段路确实有起伏（不是平地代跑）", span >= 0.02,
          "surfZ 极差 %.4f m" % span)

    # ---- G6 跳跃落地不陷 ----
    jseg = seg(jump, f_run_end, 1 << 62)
    g6 = gaps(jseg)
    check("G6 跳跃落地后脚仍在地表之上", bool(g6) and min(g6) >= GAP_MIN,
          report("jump", jseg))

    # ---- G7 无崩溃 ----
    bad = [ln for ln in lines
           if ("VK_CHECK" in ln) or ("invalid state" in ln)]
    check("G7 全程无 Vulkan 错误、无 invalid state", len(bad) == 0,
          "%d 条异常" % len(bad))

    print("-" * 56)
    print("结果: %d 通过 / %d 失败" % (g_pass, g_fail))
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
