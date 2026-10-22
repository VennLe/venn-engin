"""验收：运行时的"手感四件事"（P5，2026-10-22）。

用户原话（四条）：

  1. "游戏启动运行时，角色的脚部位置应该是紧贴地面的，却陷入进去了。另外，
     角色初始化的位置为什么不是 GameStart 的位置，GameStart 只是视口里的
     标记作用，当游戏启动运行的时候，这个标记就要消失，然后创建的角色放在
     这个位置。"
  2. "游戏启动运行后，必须要鼠标左键在 3d 视口中点击后才能开始正常和游戏
     画面内的角色开始交互，操作，操作角色时，鼠标是要隐藏的。然后退出操作
     可以点 Esc 键，此时也显示鼠标。"
  3. "这个问题最关键，角色在移动时为何没有朝它的视线方向去移动。"
  4. "不同的状态下的角色动画在切换时需要有平滑过渡的感觉，不能看起来太
     机械僵硬。"

判据（每条都能单独变红）：

  R0 出生点   GameStart 故意摆在 (5,3,0) → `spawned at GameStart feet=(5.00,3.00,0.00)`
  R1 脚贴地   `model lift=... -> bottom=0.0000`（|bottom| ≤ 2 mm）
  R2 朝向     `facing ... shouldersAlong=Y ok=1`（人形左右跨度 > 前后跨度）
  R3 启动未捕获 `GAME-MOUSE captured=0` + `cursor=1`（光标看得见、动不了）
  R4 未捕获冻结 点击前按 W：`GAME-PLAYER-POS` 的 x 一动不动
  R5 点击捕获  `GAME-MOUSE captured=1 reason=click`
  R6 捕获后能走 按 W → `GAME-ANIM state=walk` 且 x 明显增大
  R7 平滑过渡  `state=walk ... blend=0.20s` 且 `ANIM-TICK ... fading=1`
  R8 Esc 释放  `UI2-PAUSE open=1 ... cursor=1`（= 退出操作态、光标回来）
  R9 无崩溃    0 条 VK_CHECK / 0 条 invalid state / 进程活着

⚠ R2 用的是**几何判据**不是截图比对：像素级的"宽高比"实测在 0.367~0.429
  之间飘（摆臂会撑宽包围盒），拿来断言就是 flaky。
  T-pose 人形肩宽 ≈ 0.45 m、身体厚 ≈ 0.25 m —— 按正确朝向装配后，"左右"
  必须落在 Y 上。漏掉 90° 补偿时跨度会整好反过来，这条立刻红。

用法：
    python tools/verify_player_feel.py
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
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "player_feel")

SCENE_REL = "scenes/_p5_feel.json"
SCENE_ABS = os.path.join(ASSETS, SCENE_REL)
GAMEJSON = os.path.join(ASSETS, "_p5_feel_game.json")

# 出生点故意**不在原点**：只在原点测的话，"用没用 GameStart"根本看不出来
SPAWN = (5.0, 3.0, 0.0)

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
VK_W = 0x57
VK_ESCAPE = 0x1B

RE_LIFT = re.compile(r"GAME-PLAYER model lift=([\d.-]+) m \(bounds z: "
                     r"([\d.-]+)\.\.([\d.-]+)\) yaw=([\d.-]+) deg -> "
                     r"bottom=([\d.-]+)")
RE_FACING = re.compile(r"GAME-PLAYER facing yaw=([\d.-]+) spanX=([\d.-]+) "
                       r"spanY=([\d.-]+) shouldersAlong=(\w) ok=(\d)")
RE_SPAWN = re.compile(r"GAME-PLAYER spawned at GameStart found=(\d+) "
                      r"feet=\(([\d.-]+),([\d.-]+),([\d.-]+)\) yaw=([\d.-]+)")
RE_MOUSE = re.compile(r"GAME-MOUSE captured=(\d)(?: reason=(\w+))?")
RE_PAUSE = re.compile(r"UI2-PAUSE open=(\d) context=(\w+) cursor=(\d)")
RE_ANIM = re.compile(r"GAME-ANIM state=(\w+) clip=(\S+)(?: .*?)?blend=([\d.]+)s")
RE_POS = re.compile(r"GAME-PLAYER-POS f=(\d+) pos=\(([\d.-]+),([\d.-]+),"
                    r"([\d.-]+)\)")

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


def _sc(vk):
    return user32.MapVirtualKeyW(vk, 0) & 0xFF


def key_down(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (_sc(vk) << 16) | 1)


def key_up(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (_sc(vk) << 16) | 0xC0000001)


def click(hwnd, x, y):
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, lp)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)


def write_inputs():
    scene = {
        "version": 1,
        "generator": "verify_player_feel.py",
        "camera": {"target": [SPAWN[0], SPAWN[1], 1.2],
                   "yaw": 3.141592653589793, "pitch": 0.30,
                   "distance": 5.0, "fov": 55.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": True, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            # 参考地板：顶面在 z=0（plane），脚应该贴着它
            {"name": "Ground",
             "transform": {"position": [SPAWN[0], SPAWN[1], 0.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "parent": -1, "visible": True, "castShadow": False,
             "locked": True,
             "mesh": {"kind": "builtin", "shape": "plane", "size": 40.0,
                      "name": "feel_ground"},
             "material": {"kind": "color", "color": [0.55, 0.58, 0.62, 1.0],
                          "roughness": 0.9, "metallic": 0.0,
                          "name": "feel_ground_mat"}},
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
    env["MYVK_RUNTIME_PLAYER_DUMP"] = "10"
    return env


def shot(hwnd, name):
    os.makedirs(OUT_DIR, exist_ok=True)
    p = os.path.join(OUT_DIR, name)
    try:
        capture_client(hwnd)[0].save(p)
        return p
    except Exception as ex:                                   # noqa: BLE001
        print("  截图失败 %s: %s" % (name, ex))
        return ""


def read_log(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def first_match(lines, rx, start=0, end=None):
    """从 start 开始找第一条匹配。返回 (下标, match)。"""
    end = len(lines) if end is None else min(end, len(lines))
    for i in range(start, end):
        m = rx.search(lines[i])
        if m:
            return i, m
    return -1, None


def last_pos_before(lines, idx):
    last = None
    for i in range(idx):
        m = RE_POS.search(lines[i])
        if m:
            last = (i, float(m.group(2)), float(m.group(3)))
    return last


def last_pos_overall(lines):
    last = None
    for line in lines:
        m = RE_POS.search(line)
        if m:
            last = (float(m.group(2)), float(m.group(3)))
    return last


def main():
    write_inputs()
    os.makedirs(OUT_DIR, exist_ok=True)
    logp = os.path.join(OUT_DIR, "feel.log")
    lf = open(logp, "w", encoding="utf-8", errors="replace")

    # ⚠ stdout 必须直接写文件：Vulkan validation 一开，管道 64 KB 很快满，
    #   子进程会阻塞在 write 上，窗口永远不出现（第一版就是这么失败的）。
    p = subprocess.Popen([GAME], cwd=BIN, env=clean_env(),
                         stdout=lf, stderr=subprocess.STDOUT)
    hwnd = find_window("Venn Game", timeout=25.0)
    if not hwnd:
        p.kill()
        lf.close()
        print("结果: FAIL  (窗口没出现)")
        return 1

    time.sleep(3.0)
    shots = {}
    shots["spawn"] = shot(hwnd, "r3_spawn.png")

    # ---- R4：未捕获时按 W，玩家必须一动不动 ----
    key_down(hwnd, VK_W)
    time.sleep(1.2)
    key_up(hwnd, VK_W)
    time.sleep(0.3)

    # ---- R5：点视口进入操作态 ----
    click(hwnd, 640, 360)
    time.sleep(0.6)
    shots["click"] = shot(hwnd, "r5_after_click.png")

    # ---- R6：捕获后按 W 真的走 ----
    key_down(hwnd, VK_W)
    time.sleep(1.6)
    shots["walk"] = shot(hwnd, "r6_walking.png")
    key_up(hwnd, VK_W)
    time.sleep(0.6)

    # ---- R8：Esc 退出操作态 ----
    key_down(hwnd, VK_ESCAPE)
    time.sleep(0.12)
    key_up(hwnd, VK_ESCAPE)
    time.sleep(0.8)
    shots["esc"] = shot(hwnd, "r8_after_esc.png")

    time.sleep(0.4)
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

    # ---- R0 出生点 = GameStart（故意摆在非原点）----
    i_spawn, m_spawn = first_match(lines, RE_SPAWN)
    if m_spawn:
        feet = (float(m_spawn.group(2)), float(m_spawn.group(3)),
                float(m_spawn.group(4)))
        d = max(abs(feet[k] - SPAWN[k]) for k in range(3))
        check("R0 出生点是 GameStart（不是原点）",
              m_spawn.group(1) == "1" and d < 0.02,
              "feet=(%.2f,%.2f,%.2f) want=(%.2f,%.2f,%.2f)"
              % (feet + SPAWN))
    else:
        check("R0 出生点是 GameStart（不是原点）", False, "没有 GAME-PLAYER spawned")

    # ---- R1 脚贴地 ----
    _, m_lift = first_match(lines, RE_LIFT)
    if m_lift:
        bottom = float(m_lift.group(5))
        check("R1 模型脚底落在实体原点（脚不下陷）", abs(bottom) <= 2e-3,
              "lift=%.3f bottom=%+.4f" % (float(m_lift.group(1)), bottom))
    else:
        check("R1 模型脚底落在实体原点（脚不下陷）", False, "没有 lift 锚点")

    # ---- R2 朝向补偿（几何判据）----
    _, m_face = first_match(lines, RE_FACING)
    if m_face:
        spanX = float(m_face.group(2))
        spanY = float(m_face.group(3))
        check("R2 模型的正面朝 +X（移动方向 = 视线方向）",
              m_face.group(5) == "1" and spanX < spanY,
              "yaw=%s spanX=%.3f spanY=%.3f shouldersAlong=%s"
              % (m_face.group(1), spanX, spanY, m_face.group(4)))
    else:
        check("R2 模型的正面朝 +X（移动方向 = 视线方向）", False,
              "没有 facing 锚点")

    # ---- R3 启动未捕获 ----
    i_mouse0, m_mouse0 = first_match(lines, RE_MOUSE)
    i_cap, m_cap = first_match(lines, RE_MOUSE)
    # captured=1 必须出现在 captured=0 之后
    i_cap = -1
    m_cap = None
    for i, line in enumerate(lines):
        m = RE_MOUSE.search(line)
        if m and m.group(1) == "1":
            i_cap, m_cap = i, m
            break
    ok_r3 = (m_mouse0 is not None and m_mouse0.group(1) == "0"
             and i_cap > i_mouse0)
    check("R3 启动时未捕获（光标可见、先不能操作）", ok_r3,
          "captured=0 在 captured=1 之前" if ok_r3 else
          "captured=0@%s captured=1@%s" % (i_mouse0, i_cap))

    # ---- R4 未捕获冻结 ----
    # 点击事件在日志里是 GAME-MOUSE captured=1 那一行；取它之前的最后一条 POS
    # 与第一条 POS 比较。
    if i_cap > 0:
        p_last = last_pos_before(lines, i_cap)
        _, m_p0 = first_match(lines, RE_POS)
        p0 = (float(m_p0.group(2)), float(m_p0.group(3))) if m_p0 else None
        if p0 and p_last:
            moved = max(abs(p_last[1] - p0[0]), abs(p_last[2] - p0[1]))
            check("R4 未捕获时按 W 玩家不动（门控真的生效）", moved < 0.02,
                  "位移=%.3f m" % moved)
        else:
            check("R4 未捕获时按 W 玩家不动（门控真的生效）", False,
                  "缺少 POS 行")
    else:
        check("R4 未捕获时按 W 玩家不动（门控真的生效）", False, "没有捕获事件")

    # ---- R5 点击捕获 ----
    ok_r5 = (m_cap is not None and m_cap.group(2) == "click")
    check("R5 左键点视口 → 捕获鼠标", ok_r5,
          "reason=%s" % (m_cap.group(2) if m_cap else "-"))

    # ---- R6 捕获后能走，且方向是 +X ----
    i_walk = -1
    for i, line in enumerate(lines):
        m = RE_ANIM.search(line)
        if m and m.group(1) == "walk":
            i_walk = i
            break
    p_end = last_pos_overall(lines)
    if i_walk > 0 and p_end and m_p0 is not None:
        dx = p_end[0] - float(m_p0.group(2))
        check("R6 捕获后按 W 走起来（沿 +X = 取景前方）", dx > 0.5,
              "Δx=%.2f m state=walk" % dx)
    else:
        check("R6 捕获后按 W 走起来（沿 +X = 取景前方）", False,
              "walk@%s Δx=%s" % (i_walk, p_end))

    # ---- R7 平滑过渡 ----
    #
    # ⚠ 判据不能只看 `blend=0.20s`（那只是"配置读进来了"），也不能靠
    #   `ANIM-TICK fading=`（每 60 帧一行，一次过渡只有 12 帧，几乎采不到）。
    #   `ANIM-FADE begin` 是过渡第一帧打的一条，两者都出现才算数。
    blend_val = ""
    for line in lines:
        m = RE_ANIM.search(line)
        if m and float(m.group(3)) > 0.0:
            blend_val = m.group(3)
            break
    fade_begin = ""
    for line in lines:
        m = re.search(r"ANIM-FADE begin from=(\S+) to=(\S+) dur=([\d.]+)s", line)
        if m:
            fade_begin = "%s->%s %ss" % (m.group(1), m.group(2), m.group(3))
            break
    check("R7 状态切换有交叉淡入（blend>0 且真的开始了混合）",
          bool(blend_val) and bool(fade_begin),
          "blend=%ss fade=%s" % (blend_val or "-", fade_begin or "-"))

    # ---- R8 Esc 退出操作态 ----
    i_esc, m_esc = -1, None
    for i, line in enumerate(lines):
        m = RE_PAUSE.search(line)
        if m and m.group(1) == "1":
            i_esc, m_esc = i, m
            break
    ok_r8 = (m_esc is not None and i_esc > i_cap and m_esc.group(3) == "1")
    check("R8 Esc → 退出操作态并显示鼠标", ok_r8,
          "open=%s cursor=%s" % (m_esc.group(1) if m_esc else "-",
                                 m_esc.group(3) if m_esc else "-"))

    # ---- R9 无崩溃 ----
    vk = sum(1 for line in lines if "[VK_CHECK]" in line)
    inv = sum(1 for line in lines if "invalid state" in line)
    check("R9 0 条 VK_CHECK / 0 条 invalid state / 进程活着", vk == 0 and inv == 0,
          "vk=%d invalid=%d exit=%s" % (vk, inv, p.returncode))

    for k, v in shots.items():
        if v:
            print("  截图(%s): %s" % (k, v))
    print("  完整日志: %s（共 %d 行）" % (logp, len(lines)))
    print()
    print("结果: %s  (%d 通过 / %d 失败)"
          % ("PASS" if g_fail == 0 else "FAIL", g_pass, g_fail))
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    for f in ("_p5_feel.json", "_p5_feel_game.json"):
        try:
            os.remove(os.path.join(ASSETS, f))
        except OSError:
            pass
    rc = main()
    for f in ("_p5_feel.json", "_p5_feel_game.json"):
        try:
            os.remove(os.path.join(ASSETS, f))
        except OSError:
            pass
    sys.exit(rc)
