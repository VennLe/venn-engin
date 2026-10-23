"""验收：**创建自带角色 + WASD 走路动画 + 静止 8 秒重放 idle + 按键触发动画**（P4）。

用户原话（2026-10-22，两段）：

    "当创建角色时，请默认用你刚才的那套角色模型，骨骼绑定，还有动画，
     然后请给我添加角色移动的映射，按 wsad 可以让角色前后左右移动，这个
     移动时是要结合角色的行走动画的，角色停在原地如果不动长达 8 秒以上就
     触发 idle 动画，此后如果一直没移动，则每隔 8 秒继续触发。"

    "角色如果有其他的动画，可以在其对应的角色动画面板中通过添加键盘映射去
     关联对应的按键去实现动画触发。还有，角色面板这块可以也放到视口窗口上的
     标签栏一起，这样也可以通过标签直接切换。"

判据（每一条都能单独变红）：

  C0 默认角色资产：128 骨 + Idle/Walk/Run/Jump 四段 + 全顶点有权重
     （AssetC --info；**体检的是真·默认角色** alicia.vmesh）
  C1 Shift+A ▸ Player ▸ Third-Person Player 后打 `PLAYER-DEFAULT bound`
     —— 模型/骨骼/动画一次装好，不再是一具空胶囊
  C2 角色窗口**自动认领**场上那个模型（`adopted bound model`）
  C3 角色面板在**视口标签栏**上（`VTAB-character` 有非退化矩形）
  C4 动画页签里每段 clip 都有按键槽（`CHAR-CLIP-KEY-Jump`）
  C5 点按键槽 + 按 K → `CHAR-KEYBIND clip=Jump key=75`
  C5b Play 态点视口接管鼠标（`EDIT-MOUSE captured=1`，两态门控）
  C5c 切回 Viewport 标签后主视口**画面真的在变**（不是 T-pose）
     —— 这一条专治"createPlayer 自动弹出 Character 预览窗口，把 T-pose
        盖在主视口上，让人误判动画坏了"
  C6 进 Play（Space）后按 W → `EDIT-ANIM state=walk clip=Walk`
     —— 这就是"移动要结合行走动画"
  C7 松开 W → 回到 `EDIT-ANIM state=idle`
  C8 静止不动 → `EDIT-ANIM-IDLE retrigger=1 idle≈8.0s`
  C9 Play 里按 K → `EDIT-ANIM-KEY-TRIGGER clip=Jump key=75`
     （同时证明 animKeys 走完整套 JSON 存/读：Play 的运行态是**重新
       反序列化**出来的副本，组件没存/没读就没有这条）
  C10 全程 0 条 `[VK_CHECK]`，进程活着

用法：
    python tools/verify_char_locomotion.py
    python tools/verify_char_locomotion.py --keep     # 保留截图与日志
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import capture_client, find_window, user32          # noqa: E402
from safe_fs import safe_remove                                        # noqa: E402
from verify_add_menu import (LogTap as _BaseLogTap, click,
                             kill_stale_editors, open_menu_and_click,
                             wait_rect)
from verify_asset_drag import client_to_screen, force_foreground

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
EXE = os.path.join(BIN, "Editor.exe")
ASSETC = os.path.join(BIN, "AssetC.exe")
ASSETS = os.path.join(BIN, "assets")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "char_locomotion")

SCENE_NAME = "_p4_char_loco.json"
SCENE_PATH = os.path.join(ASSETS, "scenes", SCENE_NAME)
DEFAULT_CHAR = os.path.join(ASSETS, "models", "characters", "alicia.vmesh")
# 默认角色的身份指纹（C0 用）。改默认角色时**这两行必须一起改** —— 否则
# C0 会继续对旧文件做体检、然后在"默认角色坏了"的情况下报绿。
DEFAULT_JOINTS = 128
DEFAULT_HEIGHT = 1.652
SIZE = "1600x1000"

VK_K = 0x4B
VK_W = 0x57
VK_SPACE = 0x20

RE_DEFAULT = re.compile(
    r"PLAYER-DEFAULT bound path=(\S+) meshes=(\d+) skinned=(\d+) joints=(\d+) "
    r"clips=(\d+) height=([\d.]+) capsule=([\d.]+)/([\d.]+)")
RE_ADOPT = re.compile(r"CharacterWindow: adopted bound model '([^']*)' "
                      r"\(joints=(\d+) clips=(\d+)\)")
RE_KINDBIND = re.compile(r"CHAR-KEYBIND clip=(\S+) key=(\d+)")
RE_ANIM = re.compile(r"EDIT-ANIM state=(\w+) clip=(\S+)")
RE_IDLE = re.compile(r"EDIT-ANIM-IDLE retrigger=(\d+) idle=([\d.]+)s")
RE_KEYTRIG = re.compile(r"EDIT-ANIM-KEY-TRIGGER clip=(\S+) key=(\d+)")
# Play 态鼠标接管（P8，2026-10-23）：EditorApp::updatePlayMouse 的锚点。
# 没有它就没有"点了才能交互"—— 下面的 W 会一帧都推不动。
RE_EDIT_MOUSE = re.compile(r"EDIT-MOUSE captured=(\d)")
RE_INFO_CLIP = re.compile(r'clip "([^"]+)" ([\d.]+) s')


# ---------------------------------------------------------------- 场景

def make_scene():
    """只有出生点的空场景。

    为什么**不带地形**：这条用例要跑 8 秒以上的 Play，地形会把每帧时间拉长
    （地形是 25 万面 + 4 层材质），8 秒 play time 可能要等三十秒真实时间。
    角色的装配与动画都与地形无关 —— 地形那条链已经有
    `verify_preview_terrain.py` 守着。
    """
    scene = {
        "version": 1,
        "generator": "verify_char_locomotion.py",
        "camera": {"target": [0.0, 0.0, 1.2], "yaw": 3.141592653589793,
                   "pitch": 0.30, "distance": 6.0, "fov": 55.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True,
                  "ambientScale": 1.0},
        "physics": {"enabled": True, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
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
    os.makedirs(os.path.dirname(SCENE_PATH), exist_ok=True)
    with open(SCENE_PATH, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)


# ---------------------------------------------------------------- 日志泵

class LogTap(_BaseLogTap):
    def snapshot(self):
        with self.lock:
            return list(self.lines)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_SCENE"] = SCENE_PATH
    env.update({k: str(v) for k, v in extra.items()})
    return env


def launch():
    proc = subprocess.Popen([EXE], cwd=BIN, env=clean_env(),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)

    hwnd = None
    for _ in range(60):
        hwnd = find_window("Venn Editor")
        if hwnd:
            break
        if proc.poll() is not None:
            return proc, None, tap
        time.sleep(0.25)
    if not hwnd:
        return proc, None, tap

    user32.ShowWindow(hwnd, 9)
    time.sleep(1.2)
    tw, th = (int(v) for v in SIZE.split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.5)
    force_foreground(hwnd)
    time.sleep(0.8)
    return proc, hwnd, tap


def close(proc, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.8)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()


# ---------------------------------------------------------------- 小工具

def center(rect):
    return (int((rect[0] + rect[2]) * 0.5), int((rect[1] + rect[3]) * 0.5))


def grab(hwnd):
    try:
        return capture_client(hwnd)[0].convert("RGB")
    except Exception as exc:                                # noqa: BLE001
        print("  截图失败:", exc)
        return None


def diff_ratio(a, b, rect, step=2):
    """两张图在 rect 内的"明显不同"像素占比（同 verify_char_preview）。"""
    if a is None or b is None:
        return 0.0
    x0, y0, x1, y1 = (int(v) for v in rect)
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, a.width, b.width), min(y1, a.height, b.height)
    pa, pb = a.load(), b.load()
    total = diff = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            total += 1
            ca, cb = pa[x, y], pb[x, y]
            if (abs(ca[0] - cb[0]) + abs(ca[1] - cb[1])
                    + abs(ca[2] - cb[2])) > 24:
                diff += 1
    return (diff / total) if total else 0.0


# ---- 键盘注入 ----
#
# `verify_player` 的 tap_key / hold_key 走的是它自己那张**扫描码字典**（SC），
# 只覆盖它用到的几个键；K 不在里面（实测 KeyError: 75）。这里改成
# `MapVirtualKey` 现算 —— 任何 VK 都能发，不用再维护第二张表。
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101


def _sc(vk):
    sc = user32.MapVirtualKeyW(vk, 0)      # MAPVK_VK_TO_VSC
    return sc & 0xFF


def tap_key(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (_sc(vk) << 16) | 1)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (_sc(vk) << 16) | 0xC0000001)
    time.sleep(0.4)


def key_down(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (_sc(vk) << 16) | 1)


def key_up(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (_sc(vk) << 16) | 0xC0000001)


def hold_key(hwnd, vk, seconds):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (_sc(vk) << 16) | 1)
    time.sleep(seconds)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, (_sc(vk) << 16) | 0xC0000001)
    time.sleep(0.2)


def wait_regex_after(tap, rx, offset, timeout=8.0):
    """从第 `offset` 行开始扫，返回 (match, 命中行的下一行号)。

    ⚠ 为什么不用"第 N 个匹配"：那条路要先把整份日志重新 grep 一遍再按下标
    取，而日志是**并发追加**的 —— 两次 grep 之间行数会变，下标就对不上了
    （实测表现：明明日志里有 `state=idle`，用例却报"没有回到 idle"）。
    按行号偏移走是单调的，不会错位。
    """
    end = time.time() + timeout
    n = offset
    while time.time() < end:
        lines = tap.snapshot()
        for i in range(n, len(lines)):
            m = rx.search(lines[i])
            if m:
                return m, i + 1
        n = max(n, len(lines))
        time.sleep(0.1)
    return None, n


def wait_anim_state(tap, want, offset, timeout=8.0):
    """等到 state 属于 `want` 的 EDIT-ANIM 行，返回 (match, 下一行号)。"""
    end = time.time() + timeout
    n = offset
    while time.time() < end:
        lines = tap.snapshot()
        for i in range(n, len(lines)):
            m = RE_ANIM.search(lines[i])
            if m and m.group(1) in want:
                return m, i + 1
        n = max(n, len(lines))
        time.sleep(0.1)
    return None, n


def wait_rect_visible(tap, tag, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r and (r[2] - r[0]) > 2.0 and (r[3] - r[1]) > 2.0:
            return r
        time.sleep(0.12)
    return None


# ---------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留截图与日志")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print("[ %s ] %s  %s" % (" OK " if ok else "FAIL", name, detail))
        return ok

    kill_stale_editors()
    make_scene()

    # ---- C0 默认角色资产 ----
    # ⚠ 体检的是**真·默认角色**（`editor/EditorScene.cpp::kDefaultCharacterPath`），
    #   不是随便一个能加载的角色。历史上这里写过 `default_character.vmesh`
    #   （23 骨兜底资产），换默认角色后忘了改 → C0 一直绿，但测的是旧文件。
    p = subprocess.run([ASSETC, "--info", DEFAULT_CHAR],
                       cwd=BIN, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", timeout=120)
    info = p.stdout.splitlines()
    joints = [l for l in info if "joints=" in l and "clips=" in l]
    clips = [m.group(1) for m in (RE_INFO_CLIP.search(l) for l in info) if m]
    n_joints = int(re.search(r"joints=(\d+)", joints[0]).group(1)) if joints else 0
    hm = [re.search(r"height\(Z\)=([\d.]+)", l) for l in info]
    height = float(next(m.group(1) for m in hm if m)) if any(hm) else 0.0
    skin = [l for l in info if "skinVerts=" in l]
    skin_txt = skin[0].replace("ASSETC", "").strip() if skin else "?"
    skin_ok = bool(skin) and re.search(r"skinVerts=(\d+) / (\d+)", skin[0]) \
        and re.search(r"skinVerts=(\d+) / (\d+)", skin[0]).group(1) \
        == re.search(r"skinVerts=(\d+) / (\d+)", skin[0]).group(2)
    check("C0 默认角色资产（%d 骨 + 四段动画 + 全顶点有权重）" % DEFAULT_JOINTS,
          p.returncode == 0 and n_joints == DEFAULT_JOINTS
          and set(clips) >= {"Idle", "Walk", "Run", "Jump"} and skin_ok,
          "joints=%d clips=%s %s" % (n_joints, ",".join(clips), skin_txt))
    check("C0b 默认角色身高对得上（%.3f m）" % DEFAULT_HEIGHT,
          abs(height - DEFAULT_HEIGHT) < 0.01, "height(Z)=%.3f" % height)
    if p.returncode != 0:
        print("  AssetC --info 失败:", info[-3:])

    proc, hwnd, tap = launch()
    if not hwnd:
        check("编辑器起得来", False, "exit=%s" % proc.poll())
        close(proc, None)
        return 1

    try:
        time.sleep(2.0)

        # ---- C1 创建第三人称角色 ----
        vp = wait_rect(tap, "VP-RECT", 10.0)
        if not vp:
            check("拿到视口矩形", False)
            return 3
        mx, my = center(vp)
        user32.SetCursorPos(*client_to_screen(hwnd, mx, my))
        time.sleep(0.4)
        ok = open_menu_and_click(tap, hwnd, mx, my, "VP-ADD-Player",
                                 "VP-ADD-ThirdPerson", "Third-Person Player")
        if not check("C1a 菜单创建第三人称角色", ok):
            return 4

        m, _ = wait_regex_after(tap, RE_DEFAULT, 0, 12.0)
        check("C1b 创建即装配默认角色（模型+骨骼+动画）", m is not None,
              m.group(0)[:120] if m else "没有 PLAYER-DEFAULT bound")
        if m:
            # 骨架数必须和 C0 一致：路径、蒙皮、clip 数全对但 joints 是另一
            # 个数 = 加载到的其实不是 C0 体检的那份资产（同名不同版本）。
            check("C1c 装的是蒙皮模型且 clip 齐（skinned=1 / clips=4 / "
                  "joints=%d）" % DEFAULT_JOINTS,
                  m.group(1) in (DEFAULT_CHAR.replace(os.sep, "/"),
                                 "models/characters/alicia.vmesh")
                  and m.group(3) == "1" and m.group(5) == "4"
                  and int(m.group(4)) == DEFAULT_JOINTS,
                  "path=%s skinned=%s joints=%s clips=%s height=%s" %
                  (m.group(1), m.group(3), m.group(4), m.group(5), m.group(6)))

        # ---- C2 角色窗口认领场上的模型 ----
        m, _ = wait_regex_after(tap, RE_ADOPT, 0, 12.0)
        check("C2 角色窗口自动认领场上的模型", m is not None,
              m.group(0) if m else "没有 adopted bound model")

        # ---- C3 角色面板在视口标签栏上 ----
        tabc = wait_rect_visible(tap, "VTAB-character", 8.0)
        check("C3 角色面板在视口标签栏（可通过标签切换）", tabc is not None,
              "rect=(%d,%d)-(%d,%d)" % tuple(int(v) for v in tabc)
              if tabc else "没有 VTAB-character 矩形")

        # ---- C4 动画页签：每段 clip 都有按键槽 ----
        tab_anim = wait_rect_visible(tap, "CHAR-TAB-Animation", 8.0)
        if not check("C4a 拿到 Animation 页签", tab_anim is not None):
            return 5
        ax, ay = center(tab_anim)
        click(hwnd, ax, ay)
        time.sleep(0.3)
        click(hwnd, ax, ay)     # 第一次点击只给焦点
        time.sleep(0.5)

        key_slot = wait_rect_visible(tap, "CHAR-CLIP-KEY-Jump", 8.0)
        check("C4b 每条 clip 行尾有按键槽（CHAR-CLIP-KEY-Jump）",
              key_slot is not None)

        # ---- C5 点按键槽 + 按 K 完成绑定 ----
        bound = None
        if key_slot:
            kx, ky = center(key_slot)
            click(hwnd, kx, ky)
            time.sleep(0.5)
            tap_key(hwnd, VK_K)
            bound, _ = wait_regex_after(tap, RE_KINDBIND, 0, 6.0)
        check("C5 按键槽 + 按 K → Jump 绑到 K（GLFW 75）",
              bound is not None and bound.group(1) == "Jump"
              and bound.group(2) == "75",
              bound.group(0) if bound else "没有 CHAR-KEYBIND")

        shot1 = os.path.join(OUT_DIR, "c5_bound.png")
        try:
            capture_client(hwnd)[0].save(shot1)
        except Exception as exc:            # noqa: BLE001
            print("  截图失败:", exc)

        # ---- C6/C7 进 Play：W 走 → 走路动画，松开 → idle ----
        time.sleep(0.5)
        tap_key(hwnd, VK_SPACE)             # Space = Play
        seen_play = False
        end = time.time() + 8.0
        while time.time() < end:
            if any("PLAY" in l or "Runtime" in l
                   for l in tap.snapshot()[-400:]):
                seen_play = True
                break
            time.sleep(0.1)
        time.sleep(1.5)                     # 让状态机先落一次 idle

        # ---- P8（2026-10-23）：Play 态键鼠是两态的，先点视口接管 ----
        # 用户要求"必须先用左键在 3D 视口里点一下才开始和角色交互"。所以
        # 任何"进 Play 后注入 WASD 再断言动画"的用例都得先补这一下，否则下面
        # 的 W 一帧都推不动（C6 会莫名其妙变成"一直 idle"）。
        # ⚠ 偏移必须在**点击之前**取：click() 内部会 sleep 0.5 s，锚点在
        #   那半秒里就打完了 —— 点完再取 offset 等于"从锚点之后开始找"，
        #   永远等不到（第一版就栽在这儿，日志里明明有那一条）。
        off_cap = len(tap.snapshot())
        click(hwnd, mx, my)
        m_cap, _ = wait_regex_after(tap, RE_EDIT_MOUSE, off_cap, 8.0)
        check("C5b Play 态点视口接管鼠标（两态门控）", m_cap is not None,
              m_cap.group(0) if m_cap else "没有 EDIT-MOUSE captured=1")

        # ---- C5c 主视口里那个人到底动没动（T-pose 陷阱的正式判据）----
        # `createPlayer` 成功后会**自动打开 Character 预览窗口**。那是一份
        # 独立渲染的静止副本（编辑态 / clip 空 / play=0），而且它盖在视口
        # 上面 —— 于是"截主视口的图"实际拍到的是预览窗口里那个 T-pose，
        # 让人误判"引擎里的动画坏了"。这一条把"必须切回 Viewport 标签再
        # 截图 + 画面必须真的在变"钉成判据：
        #   · 切标签：标签栏有 `VTAB-viewport` 锚点，点它（不猜坐标）
        #   · 判动：间隔 0.45 s 两帧的像素差异（静止 = 0%，Idle 有呼吸摆动）
        vpt = wait_rect_visible(tap, "VTAB-viewport", 8.0)
        if check("C5c0 拿到 Viewport 标签（能把预览窗口挤下去）",
                 vpt is not None):
            click(hwnd, *center(vpt))
            time.sleep(0.9)                 # 等自动布局切完再截
        a1 = grab(hwnd)
        time.sleep(1.0)                     # ⚠ 1.0 s 不是随便取的：Idle 的
        #   呼吸/重心浮动幅度很小，0.45 s 只有 ~0.03%（跟 tooltip 的文字刷新
        #   同量级，分不开）。1.0 s 实测 0.5~0.6%，T-pose（静止）实测 ≤0.05%。
        a2 = grab(hwnd)
        ratio = diff_ratio(a1, a2, vp)
        check("C5c 主视口里角色真的在动（不是预览窗口里的 T-pose）",
              ratio >= 0.0015,
              "视口内像素差异 %.3f%%（阈值 0.15%%，静止实测 ≤0.05%%）"
              % (ratio * 100.0))
        for img, nm in ((a1, "c5c_viewport_a.png"), (a2, "c5c_viewport_b.png")):
            if img is not None:
                img.save(os.path.join(OUT_DIR, nm))
        # 切过标签后鼠标接管已经丢了（点在标签上 = 点在视口外），补一次
        click(hwnd, mx, my)
        time.sleep(0.4)

        # 行号偏移（不是"第几个匹配"），理由见 wait_regex_after
        off = len(tap.snapshot())
        # ⚠ 走路截图必须在**按住 W 的当中**拍：`hold_key` 是阻塞的、
        #   返回时已经松开，那时人已经回到 idle 了（第一版就栽在这儿 ——
        #   名字叫 walk 的截图里其实站着不动）。所以这里手动 down / up。
        key_down(hwnd, VK_W)
        walk, off = wait_anim_state(tap, {"walk"}, off, 8.0)
        time.sleep(0.4)                     # 让动画真的迈出两步再拍
        try:
            capture_client(hwnd)[0].save(
                os.path.join(OUT_DIR, "c6_walk_in_scene.png"))
        except Exception as exc:            # noqa: BLE001
            print("  截图失败:", exc)

        # 走到 3D 视口标签看一眼：动画是不是真的在**场景里**那个人身上
        vp_tab = wait_rect_visible(tap, "VTAB-viewport", 4.0)
        if vp_tab:
            click(hwnd, *center(vp_tab))
            time.sleep(0.6)
            try:
                capture_client(hwnd)[0].save(
                    os.path.join(OUT_DIR, "c6_walk_in_viewport.png"))
            except Exception as exc:        # noqa: BLE001
                print("  截图失败:", exc)
        key_up(hwnd, VK_W)

        check("C6 按 W 走起来 → 播 Walk（移动结合行走动画）",
              walk is not None,
              walk.group(0) if walk else "没有 EDIT-ANIM state=walk")
        check("C6b 确实进过 Play（否则上面这条是空跑）", seen_play)

        # 松开后再等：速度归零 → 回 idle
        idle_after, off = wait_anim_state(tap, {"idle"}, off, 8.0)
        check("C7 松开后回到 idle", idle_after is not None,
              idle_after.group(0) if idle_after else "没有回到 idle")

        # ---- C8 静止 8 秒 → 重放一次 idle ----
        # `idle=` 是**累计 dt**（play time），不是墙钟 —— 编辑器掉帧时
        # 真实耗时会超过 8 秒，所以等待窗口给 30 s，判据只看日志里的数值。
        rt, off = wait_regex_after(tap, RE_IDLE, off, 30.0)
        idle_sec = float(rt.group(2)) if rt else 0.0
        check("C8 静止满 8 秒重放 idle（retrigger=1, idle≈8s）",
              rt is not None and rt.group(1) == "1"
              and 7.9 <= idle_sec <= 8.9,
              rt.group(0) if rt else "没有 EDIT-ANIM-IDLE")

        # ---- C9 Play 里按 K → 触发 Jump（顺带证明 animKeys 存读闭环）----
        tap_key(hwnd, VK_K)
        trig, off = wait_regex_after(tap, RE_KEYTRIG, off, 8.0)
        check("C9 Play 里按 K 触发 Jump（animKeys 存读闭环）",
              trig is not None and trig.group(1) == "Jump"
              and trig.group(2) == "75",
              trig.group(0) if trig else "没有 EDIT-ANIM-KEY-TRIGGER")

        # ---- C10 没有设备丢失 ----
        lines = tap.snapshot()
        vk = [l for l in lines if "[VK_CHECK]" in l]
        bad = [l for l in lines if "invalid state" in l]
        alive = proc.poll() is None
        check("C10 0 条 VK_CHECK / 0 条 invalid state / 进程活着",
              not vk and not bad and alive,
              "vk=%d invalid=%d exit=%s" % (len(vk), len(bad), proc.poll()))

        shot2 = os.path.join(OUT_DIR, "c9_inplay.png")
        try:
            capture_client(hwnd)[0].save(shot2)
            print("  截图:", shot2)
        except Exception as exc:            # noqa: BLE001
            print("  截图失败:", exc)

        with open(os.path.join(OUT_DIR, "p4_full.log"), "w",
                  encoding="utf-8") as f:
            f.write("\n".join(lines))
        print("  完整日志: %s（共 %d 行）"
              % (os.path.join(OUT_DIR, "p4_full.log"), len(lines)))
        print("  EDIT-ANIM 行数:",
              len([l for l in lines if "EDIT-ANIM" in l]))
    finally:
        close(proc, hwnd)
        if not args.keep:
            safe_remove(SCENE_PATH)

    npass = sum(1 for r in results if r)
    print("\n结果: %s  (%d 通过 / %d 失败)"
          % ("PASS" if npass == len(results) else "FAIL",
             npass, len(results) - npass))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
