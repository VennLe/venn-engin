"""验收：程序化生成的 idle / walk / run / jump 四段动画（P3）。

两段验收，缺一不可：

**A. 离线数值（不用显卡）** —— 生成器自带的探针（`PROCANIM-PROBE`）
   之所以存在，是因为"一段姿势数据是不是在走路"在静帧里看不出来：
     · 前向位移范围小 → 腿没摆起来
     · 抬升太小     → 脚在地上拖
     · 摆动相前向速度为负 → **月球漫步**（方向反了，范围照样是正的）
     · 相位不对     → 每一步的"脚最前"不在 p≈0
   这四条全部由生成器在烘焙时自己算好打出来，脚本只做门限判断。

**B. 运行时真的播得出来** —— 起 `Game.exe`，按 W / Shift+W / 空格，
   从日志里看 `GAME-ANIM state=` 是不是依次出现了 idle / walk / run / jump，
   并且**没有** `GAME-ANIM-MISSING`（缺片段就说明 clip 名字对不上）。

用法：
    python tools/verify_procanim.py
    python tools/verify_procanim.py --no-runtime     # 只跑离线数值
"""

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from safe_fs import safe_remove                                   # noqa: E402
from capture_window import capture_client, find_window, user32     # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
ASSETC = os.path.join(BIN, "AssetC.exe")
GAME = os.path.join(BIN, "Game.exe")
ASSETS = os.path.join(BIN, "assets")
WORK = os.path.join(os.path.dirname(ROOT), "_venn_verify", "procanim")

SRC = os.path.join(ASSETS, "models", "thirdparty", "samba_dancing.vmesh")
# 产物落到资产目录里：运行时只认资产根下的相对路径
OUT_REL = "models/thirdparty/_procanim_test.vmesh"
OUT = os.path.join(ASSETS, OUT_REL.replace("/", os.sep))
SCENE_NAME = "_p3_procanim.json"
SCENE_PATH = os.path.join(ASSETS, "scenes", SCENE_NAME)
TITLE = "_p3_procanim"

RE_PROBE = re.compile(
    r"ASSETC PROCANIM-PROBE walkFootFwd=([\d.]+) walkFootLift=([\d.]+) "
    r"runFootFwd=([\d.]+) runFootLift=([\d.]+) walkHandFwd=([\d.]+) "
    r"idleDrift=([\d.]+) walkFootFwdMaxPhase=([\d.]+) walkSwingFwdVel=(-?[\d.]+) "
    r"walkHandLat=([\d.]+) runHandLat=([\d.]+) runHandFwd=([\d.]+)")
RE_SUM = re.compile(r"ASSETC PROCANIM ok=1 clips=(\d+) tracks=(\d+) keys=(\d+)")
RE_CLIP = re.compile(r"ASSETC PROCANIM-CLIP (\S+) dur=([\d.]+)")
RE_INFO_CLIP = re.compile(r'ASSETC\s+clip "([^"]+)" ([\d.]+) s\s+tracks=(\d+)')
RE_ANIM = re.compile(r"GAME-ANIM state=(\w+) clip=(\S+)")
RE_MISS = re.compile(r"GAME-ANIM-MISSING state=(\w+) clip='([^']*)'")

VK_W, VK_SPACE = 0x57, 0x20
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
RE_SPRINT_BIND = re.compile(r"Sprint=(\w+)")


def _sc(vk):
    """VK → 扫描码（MAPVK_VK_TO_VSC）。现算，不再维护第二张字典表。"""
    return user32.MapVirtualKeyW(vk, 0) & 0xFF

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


def run_assetc(args, timeout=300):
    p = subprocess.run([ASSETC] + list(args), cwd=BIN,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=timeout)
    return p.returncode, p.stdout.splitlines()


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env.update({k: str(v) for k, v in extra.items()})
    return env


class Session:
    """常驻 Game.exe + 后台排空 stdout（不排空会写满管道死锁）。"""

    def __init__(self, args, env):
        self.lines = []
        self._lock = threading.Lock()
        self.proc = subprocess.Popen(
            [GAME] + args, cwd=BIN, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()
        self.hwnd = 0

    def _pump(self):
        for line in self.proc.stdout:
            with self._lock:
                self.lines.append(line.rstrip("\r\n"))

    def text(self):
        with self._lock:
            return list(self.lines)

    def state_seq(self):
        """状态机**按日志顺序**走过的状态序列。

        ⚠ 刻意**不**提供"状态集合"那种接口：B2/B3/B4 三条如果都读集合，它们
        判的是同一个量（"三个状态名都出现过"），三条会一起红、一起绿 ——
        那等于只写了一条断言。按顺序比才能分开"根本没切过去"和"切过去了
        但顺序不对"（比如冲刺太快、walk 被直接跳过）。
        """
        out = []
        for x in self.text():
            m = RE_ANIM.search(x)
            if m:
                out.append(m.group(1))
        return out

    def missing(self):
        return {m.group(1) for m in (RE_MISS.search(x) for x in self.text())
                if m}

    def sprint_bind(self):
        """从 `GAME-INPUT … Sprint=Mouse1 / PadBtn4` 里把冲刺的**主**绑定读回来。

        ⚠ 读它而不是写死"按右键"：出厂绑定改过一次（10-22 从 LShift 换成
        鼠标右键），写死的夹具就是这么烂掉的 —— 而且烂掉时的现象是"run 切
        不过去"，看起来像动画的锅。
        """
        for x in self.text():
            m = RE_SPRINT_BIND.search(x)
            if m:
                return m.group(1)
        return ""

    def stop(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=8)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


def key(hwnd, vk, down):
    """发一个键消息。

    ⚠⚠ lParam **必须带扫描码**（`(扫描码 << 16) | 重复次数`）。
    GLFW 的 Win32 后端在 `keyboardProc` 里是从 `(lParam >> 16) & 0x1FF`
    取扫描码、再查 `keycodes[scancode]` 得到 GLFW 键值的；lParam 传 0
    → 扫描码 0 → 查到 `GLFW_KEY_UNKNOWN` → `_glfwInputKey` 头一行就
    `return`，**键盘回调一次都不会进**。

    这种失败特别能骗人：窗口活着、画面正常、日志里一条错都没有，
    只有"状态机永远停在 idle"。它和"键绑错了"长得一模一样，区别只在
    把绑定改成别的键也照样不动。2026-10-30 本用例 B2/B3/B4 三红就是它
    （旧实现只 `PostMessageW(hwnd, msg, vk, 0)`）。
    """
    if down:
        user32.PostMessageW(hwnd, WM_KEYDOWN, vk, (_sc(vk) << 16) | 1)
    else:
        user32.PostMessageW(hwnd, WM_KEYUP, vk, (_sc(vk) << 16) | 0xC0000001)


def mouse(hwnd, button, down, x=480, y=300):
    """按/放鼠标键。`button` 用 GLFW 的编号（0=左 1=右 2=中）。

    ⚠ 鼠标消息走 `PostMessage` 是**可靠**的（GLFW 的 mouseProc 只看
    wParam 的键位，不像 Shift 那样会被"防粘住"补丁撤销），所以冲刺
    不需要 `keybd_event` + 抢前台的组合拳。
    """
    if button == 0:
        msg, flag = (WM_LBUTTONDOWN, 1) if down else (WM_LBUTTONUP, 0)
    else:
        msg, flag = (WM_RBUTTONDOWN, 2) if down else (WM_RBUTTONUP, 0)
    user32.PostMessageW(hwnd, msg, flag, ((y & 0xFFFF) << 16) | (x & 0xFFFF))


def make_scene():
    scene = {
        "version": 1,
        "generator": "verify_procanim.py",
        "camera": {"target": [0.0, 0.0, 1.2], "yaw": 3.141592653589793,
                   "pitch": 0.28, "distance": 4.5, "fov": 55.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True,
                  "ambientScale": 1.0},
        "physics": {"enabled": True, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            {"name": "Ground",
             "transform": {"position": [0.0, 0.0, 0.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "parent": -1, "visible": True, "castShadow": False, "locked": True,
             "mesh": {"kind": "builtin", "shape": "plane", "size": 120.0,
                      "name": "p3_ground"},
             "material": {"kind": "vmat", "path": "materials/ground.vmat",
                          "name": "materials/ground",
                          "id": "materials/ground"}},
            {"name": "GameStart",
             "transform": {"position": [0.0, 0.0, 0.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "parent": -1, "visible": True, "castShadow": False,
             "gameStart": {"yaw": 0.0, "enabled": True}},
        ],
    }
    os.makedirs(os.path.dirname(SCENE_PATH), exist_ok=True)
    with open(SCENE_PATH, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-runtime", action="store_true")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    os.makedirs(WORK, exist_ok=True)
    print("== 程序化四段动画：验收 ==")

    if not os.path.exists(SRC):
        print("[FAIL] 找不到 %s" % SRC)
        return 1

    # ---- A. 离线：剥骨架 → 自动绑骨 → 生成动画 ----
    rc, lines = run_assetc(["--strip-skin", "--autorig", "--procanim",
                            SRC, "-o", OUT])
    if not check("A0 一键跑通（剥骨架→绑骨→四段动画）", rc == 0,
                 "" if rc == 0 else (lines[-1] if lines else "")):
        print("\n结果: FAIL")
        return 1

    probe = None
    summary = None
    clips = []
    for ln in lines:
        m = RE_PROBE.search(ln)
        if m:
            probe = [float(g) for g in m.groups()]
        m = RE_SUM.search(ln)
        if m:
            summary = [int(g) for g in m.groups()]
        m = RE_CLIP.search(ln)
        if m:
            clips.append((m.group(1), float(m.group(2))))

    check("A1 生成 4 段、每段都有轨道与关键帧",
          summary is not None and summary[0] == 4 and summary[2] > 500,
          "" if summary is None else
          "clips=%d tracks=%d keys=%d" % tuple(summary))

    names = [c[0] for c in clips]
    want = ["Idle", "Walk", "Run", "Jump"]
    check("A2 四段名字齐全（就是运行时状态机找的那四个）",
          all(w in names for w in want), ",".join(names))
    durs = dict(clips)
    check("A3 时长在合理区间（idle 最长、run 最短）",
          durs.get("Idle", 0) > durs.get("Walk", 0) > durs.get("Run", 0) > 0.3
          and durs.get("Jump", 0) > 0.5,
          " ".join("%s=%.2fs" % d for d in clips))

    if probe is None:
        check("A4 探针数值可用", False, "没有 PROCANIM-PROBE")
    else:
        (wf, wl, rf, rl, wh, drift, phase, swingv, wlat, rlat, rh) = probe
        # 门限来源：真人（1.8 m）步幅 0.6~1.0 m、脚离地 3~20 cm、
        # 摆动相前向速度 ~0.5 m/s；跑起来幅度更大。
        check("A4 走：脚的前向位移像走路（0.4~1.3 m）", 0.4 <= wf <= 1.3,
              "%.3f m" % wf)
        check("A5 走：脚真的离地（2~25 cm）", 0.02 <= wl <= 0.25, "%.3f m" % wl)
        check("A6 走：摆动相脚向前飞（不是月球漫步）", swingv > 0.1,
              "摆动相平均前向速度 %+.3f m/s" % swingv)
        check("A7 走：脚最前的一刻在周期起点（相位约定成立）",
              min(phase, 1.0 - phase) <= 0.15, "p=%.3f" % phase)
        check("A8 跑：幅度明显大于走", rf > wf * 1.05 and rl > wl,
              "跑 %.2f m / 抬 %.2f m vs 走 %.2f m / 抬 %.2f m" %
              (rf, rl, wf, wl))
        check("A9 手臂在摆", wh > 0.15, "手的前向位移 %.3f m" % wh)
        check("A10 idle 不会自己飘（根位移 < 3 cm）", 0.0 < drift < 0.03,
              "%.4f m" % drift)
        # 手臂横向。T-pose 指尖在 0.74 m 处，压平到 80° 后手应落在 0.28 m 附近
        # （肩 0.19 + 上臂 0.55 × cos80°）。超了就是"张开翅膀"那种撇手 ——
        # 从斜后上方看截图几乎分不出来，只有这个数能抓住。
        check("A12 手臂是垂着摆的，没往两边撇（横向 < 0.32 m）",
              wlat < 0.32 and rlat < 0.32,
              "走 %.3f m / 跑 %.3f m" % (wlat, rlat))

    # ---- A11 片段真的写进文件、且能被读回来 ----
    # 每条轨道要求的最低数不是同一个：Idle 只让少数关节动（站着不能乱晃），
    # 生成器只给"真的在变"的关节建轨道，所以它天然比 Walk/Run 少。
    rc, lines = run_assetc(["--info", OUT])
    info_clips = {}
    for ln in lines:
        m = RE_INFO_CLIP.search(ln)
        if m:
            info_clips[m.group(1)] = (float(m.group(2)), int(m.group(3)))
    min_tracks = {"Idle": 6, "Walk": 10, "Run": 12, "Jump": 10}
    ok_info = (len(info_clips) == 4 and
               all(n in info_clips and info_clips[n][0] > 0.3 and
                   info_clips[n][1] >= min_tracks[n] for n in want))
    check("A11 四段都落进了 .vmesh 并能读回（名字/时长/轨道数）", ok_info,
          " ".join("%s=%.2fs/%dtrk(≥%d)" % (k, v[0], v[1], min_tracks[k])
                   for k, v in sorted(info_clips.items())))

    # ---- B. 运行时四态切换 ----
    if not args.no_runtime:
        make_scene()
        env = clean_env(
            MYVK_RUNTIME_TITLE=TITLE,
            MYVK_RUNTIME_SCENE="scenes/" + SCENE_NAME,
            MYVK_RUNTIME_WIDTH=960, MYVK_RUNTIME_HEIGHT=600,
            MYVK_RUNTIME_AUDIO="0",
            MYVK_RUNTIME_PLAYER_MODEL=OUT_REL,
            MYVK_RUNTIME_PLAYER_CLIP="Idle",
        )
        s = Session(["--fps", "60"], env)
        if not s.hwnd:
            s.hwnd = find_window(TITLE, 25.0)
        if not s.hwnd:
            check("B0 游戏窗口出现", False, "标题前缀 '%s'" % TITLE)
            s.stop()
            return 1
        time.sleep(2.0)
        img_idle = capture_client(s.hwnd)[0]
        img_idle.save(os.path.join(WORK, "b_idle.png"))

        # ⚠⚠ 先左键点一下视口 —— 运行时的"操作态"是**点出来的**。
        #
        # 不点的话：键事件照样进引擎（GLFW 回调、`Input` 里 `g_keyDown[W]`
        # 都是 true），但 `PlayerController` 不收移动意图，位置一毫米都不动，
        # 状态机一直停在 `state=idle speed=0.00`。
        #
        # 这个失败形态最难查 —— 日志里**一条错都没有**，看起来就像"W 没送到"。
        # 2026-10-30 实测：同一份脚本，加这一行之前 ①PostMessage W / ④抢前台
        # +keybd_event 全都不动；加这一行之后同一条 PostMessage 立刻 x=4.96。
        # 位置见 `verify_player_jump.py` 的同一句 `click(hwnd, 640, 360)`。
        mouse(s.hwnd, 0, True)
        mouse(s.hwnd, 0, False)
        time.sleep(0.15)

        key(s.hwnd, VK_W, True)              # 走
        time.sleep(1.2)
        capture_client(s.hwnd)[0].save(os.path.join(WORK, "b_walk.png"))

        # 冲刺 → 跑。⚠ 出厂绑定是**鼠标右键**，不是 Shift：
        # 2026-10-22 用户要求"run 动画一按住 Shift 就切"之后，Sprint 的
        # 键盘位被整个摘掉了（`InputMap::makeDefault` 现在只绑 Mouse1 +
        # PadBtn4）。本用例当时没跟着改，于是 B3 一直按着一个不存在的键。
        sprint_bind = s.sprint_bind()
        mouse(s.hwnd, 1, True)
        time.sleep(1.2)
        capture_client(s.hwnd)[0].save(os.path.join(WORK, "b_run.png"))
        mouse(s.hwnd, 1, False)
        time.sleep(0.2)
        key(s.hwnd, VK_W, False)

        key(s.hwnd, VK_SPACE, True)          # 跳
        time.sleep(0.1)
        key(s.hwnd, VK_SPACE, False)
        time.sleep(0.6)
        capture_client(s.hwnd)[0].save(os.path.join(WORK, "b_jump.png"))
        time.sleep(0.4)
        s.stop()

        with open(os.path.join(WORK, "log.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(s.text()))

        # 按**顺序**判：走 → 跑 → 跳 必须依次发生过。集合判定分不开
        # "三个都出现过"和"跳完才走"，见 `state_seq()` 的注释。
        seq = s.state_seq()

        def first(name):
            return seq.index(name) if name in seq else -1

        i_idle, i_walk, i_run, i_jump = (first("idle"), first("walk"),
                                         first("run"), first("jump"))
        tail = "seq=%s" % ("→".join(seq[:8]) or "-")
        check("B1 静置时切到 idle", i_idle >= 0, tail)
        check("B2 按 W 切到 walk（在 idle 之后）", i_walk > i_idle >= 0, tail)
        # 先把"用例的按法还成立吗"单独判一条：它红了就是夹具陈旧，
        # 不是 run 动画坏了 —— 这两种红要能一眼分开。
        check("B3a 冲刺绑定仍是鼠标右键（用例按法依赖它）",
              sprint_bind == "Mouse1", "Sprint=%s" % (sprint_bind or "-"))
        check("B3 按住冲刺键切到 run（在 walk 之后，出厂 %s）" % (sprint_bind or "?"),
              i_run > i_walk >= 0, tail)
        check("B4 空格切到 jump（在 run 之后）", i_jump > i_run >= 0, tail)
        check("B5 没有缺片段（clip 名字与生成器一致）", not s.missing(),
              "missing=%s" % (",".join(sorted(s.missing())) or "-"))

        sc = os.path.getsize(os.path.join(WORK, "b_walk.png"))
        check("B6 画面有内容（walk 截图非空）", sc > 20000, "%d bytes" % sc)

    print("\n产物: %s" % WORK)
    print("结果: %s  (%d 通过 / %d 失败)" %
          ("PASS" if g_fail == 0 else "FAIL", g_pass, g_fail))

    if not args.keep:
        for p in (OUT, SCENE_PATH):
            if os.path.exists(p):
                safe_remove(p)
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
