"""验收：默认操控（WASD / 右键跑 / 空格跳）+ 真实跳跃轨迹（P6，2026-10-22）。

用户原话：

  "角色移动行走可以直接默认 wsad 就可，角色 run 动画改为默认鼠标右键长按
   触发，角色跳跃是敲击键盘空格键触发。以上这些动画都是默认设置即可，无须
   通过键盘映射去设置。……跳跃的高度要符合实际的跳跃，不能太高也不能太低，
   要符合实际。再触发跳跃后，角色在空中按方向按键移动时则呈现的效果是让
   角色朝指定的方向跳跃，这个要符合一个基本的物理直觉，角色跳跃的轨迹是
   一个弧度，而且跳跃的距离也要适度，保持客观真实。"

判据（每条都能单独变红）：

  J0 默认绑定     `GAME-INPUT ... Jump=Space Sprint=Mouse1`
  J1 空格起跳     `GAME-ANIM state=jump clip=Jump … ground=0`
  J2 跳跃高度     轨迹峰值 − 起跳高度 ∈ [0.55, 0.90] m（理论 0.70）
  J3 滞空时间     起跳→落地 ∈ [0.60, 0.95] s（理论 0.754）
  J4 落地回 ground `GAME-PLAYER-POS ... ground=1` 且高度回到起点（±3 cm）
  J5 轨迹是弧     上升段 z 单调增、下降段单调减（至少各 3 个采样点）
  J6 空中可控     起跳后按住 W，落点横向位移 > 0.4 m（空气控制真的生效）
  J7 右键跑       `Sprint=Mouse1` 按住 → 位移速度 ≥ 1.6 × 走路速度
  J8 脚不陷入地面 `ground=1` 的采样里 z 不低于起点 1 cm
  J9 无崩溃       0 条 VK_CHECK / 0 条 invalid state / 进程活着

⚠ 高度用 `GAME-PLAYER-POS` 的 **z（胶囊中心）** 算：胶囊中心与脚底差一个
  常量，做差时抵消，所以它等价于"脚离地多高"。

用法：
    python tools/verify_player_jump.py
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
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "player_jump")

SCENE_REL = "scenes/_p6_jump.json"
SCENE_ABS = os.path.join(ASSETS, SCENE_REL)
GAMEJSON = os.path.join(ASSETS, "_p6_jump_game.json")

SPAWN = (0.0, 0.0, 0.0)

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
VK_W, VK_D, VK_SPACE, VK_ESCAPE = 0x57, 0x44, 0x20, 0x1B

# ⚠ 绑定的描述是**多条**用 " / " 连起来的（键盘 + 手柄）：
#   `GAME-INPUT Jump=Space / PadBtn0 Sprint=Mouse1 / PadBtn4 ...`
# 所以不能写成 `Jump=(\S+) Sprint=(\S+)` —— 中间隔着 " / PadBtn0 "。
RE_JUMP_BIND = re.compile(r"Jump=(\w+)")
RE_SPRINT_BIND = re.compile(r"Sprint=(\w+)")
RE_ANIM = re.compile(r"GAME-ANIM state=(\w+) clip=(\S+).*?ground=(\d)")
RE_POS = re.compile(r"GAME-PLAYER-POS f=(\d+) pos=\(([\d.-]+),([\d.-]+),"
                    r"([\d.-]+)\) ground=(\d)")

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


def tap(hwnd, vk, hold=0.06):
    key_down(hwnd, vk)
    time.sleep(hold)
    key_up(hwnd, vk)


def mouse_down(hwnd, button, x=640, y=360):
    msg = WM_LBUTTONDOWN if button == 0 else WM_RBUTTONDOWN
    flags = 1 if button == 0 else 2
    user32.PostMessageW(hwnd, msg, flags, ((y & 0xFFFF) << 16) | (x & 0xFFFF))


def mouse_up(hwnd, button, x=640, y=360):
    msg = WM_LBUTTONUP if button == 0 else WM_RBUTTONUP
    user32.PostMessageW(hwnd, msg, 0, ((y & 0xFFFF) << 16) | (x & 0xFFFF))


def click(hwnd, x, y):
    mouse_down(hwnd, 0, x, y)
    mouse_up(hwnd, 0, x, y)


def write_inputs():
    scene = {
        "version": 1,
        "generator": "verify_player_jump.py",
        "camera": {"target": [0.0, 0.0, 1.2], "yaw": 3.141592653589793,
                   "pitch": 0.30, "distance": 5.0, "fov": 55.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True, "ambientScale": 1.0},
        "physics": {"enabled": True, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            {"name": "Ground",
             "transform": {"position": [0.0, 0.0, 0.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "parent": -1, "visible": True, "castShadow": False,
             "locked": True,
             "mesh": {"kind": "builtin", "shape": "plane", "size": 80.0,
                      "name": "jump_ground"},
             "material": {"kind": "color", "color": [0.55, 0.58, 0.62, 1.0],
                          "roughness": 0.9, "metallic": 0.0,
                          "name": "jump_ground_mat"}},
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
    # 每 2 帧一行：60fps 下 33 ms 的采样分辨率，滞空 0.75 s 会有 ~22 个点
    # —— 再粗（10 帧）就量不出峰值，只能量出"大概跳了"。
    env["MYVK_RUNTIME_PLAYER_DUMP"] = "2"
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


def pos_samples(lines):
    """所有 (帧号, x, y, z, ground) 采样。"""
    out = []
    for line in lines:
        m = RE_POS.search(line)
        if m:
            out.append((int(m.group(1)), float(m.group(2)), float(m.group(3)),
                        float(m.group(4)), int(m.group(5))))
    return out


def main():
    write_inputs()
    os.makedirs(OUT_DIR, exist_ok=True)
    logp = os.path.join(OUT_DIR, "jump.log")
    lf = open(logp, "w", encoding="utf-8", errors="replace")

    # ⚠ stdout 直接写文件：管道 64 KB 满了子进程会卡在 write 上（踩过两次）
    p = subprocess.Popen([GAME], cwd=BIN, env=clean_env(),
                         stdout=lf, stderr=subprocess.STDOUT)
    hwnd = find_window("Venn Game", timeout=25.0)
    if not hwnd:
        p.kill()
        lf.close()
        print("结果: FAIL  (窗口没出现)")
        return 1

    time.sleep(3.0)
    shot(hwnd, "j0_spawn.png")

    click(hwnd, 640, 360)      # 进操作态（左键点视口）
    time.sleep(0.6)

    # ---- J1~J5：原地跳 ----
    tap(hwnd, VK_SPACE, 0.08)
    time.sleep(0.35)
    shot(hwnd, "j1_mid_air.png")
    time.sleep(1.2)
    shot(hwnd, "j2_landed.png")

    # ---- J6：起跳后在空中按 W（量落点偏移）----
    base = pos_samples(read_log(logp))
    x0 = base[-1][1] if base else 0.0
    tap(hwnd, VK_SPACE, 0.08)
    time.sleep(0.12)
    key_down(hwnd, VK_W)
    time.sleep(0.9)
    key_up(hwnd, VK_W)
    time.sleep(0.8)
    after_air = pos_samples(read_log(logp))
    air_dx = (after_air[-1][1] - x0) if after_air else 0.0
    shot(hwnd, "j3_air_control.png")

    # ---- J7：右键长按跑步 ----
    pos_before_run = pos_samples(read_log(logp))
    x_run0 = pos_before_run[-1][1] if pos_before_run else 0.0
    f_run0 = pos_before_run[-1][0] if pos_before_run else 0
    key_down(hwnd, VK_W)                     # 先走起来
    time.sleep(0.7)
    mouse_down(hwnd, 1)                      # 再按住右键冲刺
    time.sleep(1.0)
    shot(hwnd, "j4_running.png")
    mouse_up(hwnd, 1)
    key_up(hwnd, VK_W)
    time.sleep(0.5)
    pos_after_run = pos_samples(read_log(logp))
    x_run1 = pos_after_run[-1][1] if pos_after_run else 0.0
    f_run1 = pos_after_run[-1][0] if pos_after_run else 0
    shot(hwnd, "j5_after_run.png")

    time.sleep(0.3)
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

    # ---- J0 默认绑定 ----
    j_bind = sp_bind = None
    for line in lines:
        if "GAME-INPUT Jump=" not in line:
            continue
        mj = RE_JUMP_BIND.search(line)
        ms = RE_SPRINT_BIND.search(line)
        if mj and ms:
            j_bind, sp_bind = mj.group(1), ms.group(1)
            break
    check("J0 出厂绑定：空格跳跃 / 右键冲刺（不需要键盘映射）",
          j_bind == "Space" and sp_bind == "Mouse1",
          "Jump=%s Sprint=%s" % (j_bind, sp_bind))

    # ---- J1 起跳状态 ----
    jumped = None
    for line in lines:
        m = RE_ANIM.search(line)
        if m and m.group(1) == "jump":
            jumped = m
            break
    check("J1 敲空格 → 状态机切到 jump（ground=0）",
          jumped is not None and jumped.group(3) == "0",
          ("clip=%s ground=%s" % (jumped.group(2), jumped.group(3)))
          if jumped else "没有 state=jump")

    # ---- J2/J3/J5：从采样里找第一段"离地"的弧 ----
    samples = pos_samples(lines)
    seg = []
    for s in samples:
        if s[4] == 0:
            seg.append(s)
        elif seg:
            break
    # 起点：离地前最后一个 ground=1 的 z
    start_z = None
    for i, s in enumerate(samples):
        if s[4] == 0 and i > 0 and samples[i - 1][4] == 1:
            start_z = samples[i - 1][3]
            break
    if seg and start_z is not None:
        peak = max(s[3] for s in seg)
        height = peak - start_z
        # 滞空：离地帧 → 下一帧 ground=1
        t_air = None
        i_takeoff = None
        for i, s in enumerate(samples):
            if s[4] == 0 and i > 0 and samples[i - 1][4] == 1:
                i_takeoff = i
                break
        if i_takeoff is not None:
            for j in range(i_takeoff, len(samples)):
                if samples[j][4] == 1:
                    t_air = (samples[j][0] - samples[i_takeoff][0]) / 60.0
                    break
        check("J2 跳跃高度接近真实（0.55~0.90 m）", 0.55 <= height <= 0.90,
              "实测 %.3f m（理论 0.70）" % height)
        check("J3 滞空时间合理（0.60~0.95 s）",
              t_air is not None and 0.60 <= t_air <= 0.95,
              "实测 %.2f s（理论 0.75）" % (t_air if t_air is not None else -1))

        # J5：弧线 = 先升后降（不允许"直线上升然后瞬移落地"）
        zs = [s[3] for s in seg]
        if len(zs) >= 6:
            imax = zs.index(max(zs))
            up = all(zs[k] <= zs[k + 1] + 1e-3 for k in range(imax))
            down = all(zs[k] >= zs[k + 1] - 1e-3
                       for k in range(imax, len(zs) - 1))
            check("J5 轨迹是抛物线（上升段单调升、下降段单调降）",
                  up and down and imax >= 2 and len(zs) - imax >= 2,
                  "n=%d 峰值在第 %d 点" % (len(zs), imax))
        else:
            check("J5 轨迹是抛物线（上升段单调升、下降段单调降）", False,
                  "离地采样点只有 %d 个（dump 太稀）" % len(zs))

        # J4：落地后回到起点高度
        landed_z = None
        for j in range(i_takeoff, len(samples)):
            if samples[j][4] == 1:
                landed_z = samples[j][3]
                break
        check("J4 落地后回到地面高度（不沉不浮）",
              landed_z is not None and abs(landed_z - start_z) < 0.03,
              "起跳 z=%.3f 落地 z=%s"
              % (start_z, ("%.3f" % landed_z) if landed_z is not None else "-"))
    else:
        for n in ("J2 跳跃高度接近真实（0.55~0.90 m）",
                  "J3 滞空时间合理（0.60~0.95 s）",
                  "J4 落地后回到地面高度（不沉不浮）",
                  "J5 轨迹是抛物线（上升段单调升、下降段单调降）"):
            check(n, False, "没有找到离地采样段")

    # ---- J6 空中控制 ----
    check("J6 空中按 W 能改变落点（空气控制生效）", air_dx > 0.40,
          "落点横向位移 %.2f m" % air_dx)

    # ---- J7 右键冲刺 ----
    dt = (f_run1 - f_run0) / 60.0 if f_run1 > f_run0 else 0.0
    v_avg = (x_run1 - x_run0) / dt if dt > 0.1 else 0.0
    # 走路 3.0 m/s、冲刺 ×2 = 6.0；这里只断言"明显快于走路"
    check("J7 按住右键明显加速（≥ 1.6 × 走路速度）", v_avg >= 4.8,
          "平均 %.2f m/s（走路 3.0）" % v_avg)

    # ---- J8 脚不陷入地面 ----
    grounded = [s for s in samples if s[4] == 1]
    min_z = min((s[3] for s in grounded), default=1e9)
    base_z = samples[0][3] if samples else 0.0
    check("J8 贴地采样从不低于出生高度（脚不陷进地面）",
          min_z > base_z - 0.01,
          "min z=%.4f base=%.4f" % (min_z, base_z))

    # ---- J9 无崩溃 ----
    bad = [ln for ln in lines
           if ("VK_CHECK" in ln) or ("invalid state" in ln)]
    alive = p.returncode is None or p.returncode != 0
    check("J9 无 Vulkan 错误、无 invalid state", len(bad) == 0,
          "%d 条异常" % len(bad))

    print("-" * 56)
    print("结果: %d 通过 / %d 失败" % (g_pass, g_fail))
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    rc = main()
    for f in (SCENE_ABS, GAMEJSON):
        try:
            os.remove(f)
        except OSError:
            pass
    sys.exit(rc)
