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

VK_W, VK_SHIFT, VK_SPACE = 0x57, 0x10, 0x20
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
KEYEVENTF_KEYUP = 0x0002

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

    def states(self):
        return {m.group(1) for m in (RE_ANIM.search(x) for x in self.text())
                if m}

    def missing(self):
        return {m.group(1) for m in (RE_MISS.search(x) for x in self.text())
                if m}

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
    user32.PostMessageW(hwnd, WM_KEYDOWN if down else WM_KEYUP, vk, 0)


def modifier(vk, down, scan=0x2A):
    """按/放**修饰键**（Shift / Win）必须走真实键事件，不能 PostMessage。

    为什么：GLFW 在每次 `glfwPollEvents` 的结尾都有一段"防 Shift 粘住"的补丁
    （win32_window.c 里 `GetKeyState(vk) & 0x8000` 那段）—— 物理上没按着就把
    Shift 强制 release。PostMessage 只改消息队列、不改物理键状态，于是合成出来
    的按下**在同一次轮询里就被自己撤销了**，一帧都留不下。表现就是"Shift 好像
    根本没按"，动画状态机的 run 永远切不过去（W / 空格这类普通键不受影响）。
    """
    user32.keybd_event(vk, scan, 0 if down else KEYEVENTF_KEYUP, None)


def focus(hwnd):
    """真实键事件只送到**前台**窗口，所以按修饰键之前先把它拉到前台。"""
    user32.ShowWindow(hwnd, 9)               # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.15)


def clear_modifiers():
    """清掉系统层残留的 Shift。

    上一轮漏放 Shift 会让下一次启动的 GLFW 认为"开局就按着冲刺"，日志里凭空
    冒出 run —— 这会把"跑通了"和"被污染了"混成一种现象。
    """
    for _ in range(3):
        for vk in (VK_SHIFT, 0xA0, 0xA1):
            user32.keybd_event(vk, 0x2A, KEYEVENTF_KEYUP, None)
        time.sleep(0.05)


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
        # 起窗口**之前**先把系统层的 Shift 清干净：万一上一轮漏放，
        # GLFW 会在建窗口时认为"开局就按着冲刺"，run 会凭空出现。
        clear_modifiers()
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
        img_idle = s.shot() if hasattr(s, "shot") else None
        img_idle = capture_client(s.hwnd)[0]
        img_idle.save(os.path.join(WORK, "b_idle.png"))

        key(s.hwnd, VK_W, True)              # 走（普通键，PostMessage 够用）
        time.sleep(1.2)
        capture_client(s.hwnd)[0].save(os.path.join(WORK, "b_walk.png"))
        focus(s.hwnd)                        # 真实键事件只认前台窗口
        modifier(VK_SHIFT, True)             # 冲刺 → 跑
        time.sleep(1.2)
        capture_client(s.hwnd)[0].save(os.path.join(WORK, "b_run.png"))
        modifier(VK_SHIFT, False)
        time.sleep(0.2)
        key(s.hwnd, VK_W, False)

        key(s.hwnd, VK_SPACE, True)          # 跳
        time.sleep(0.1)
        key(s.hwnd, VK_SPACE, False)
        time.sleep(0.6)
        capture_client(s.hwnd)[0].save(os.path.join(WORK, "b_jump.png"))
        time.sleep(0.4)
        s.stop()
        clear_modifiers()                    # 别把 Shift 留给下一个用例

        with open(os.path.join(WORK, "log.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(s.text()))

        st = s.states()
        check("B1 静置时切到 idle", "idle" in st, "states=%s" % (",".join(sorted(st)) or "-"))
        check("B2 按 W 切到 walk", "walk" in st, "states=%s" % (",".join(sorted(st)) or "-"))
        check("B3 按住 Shift 切到 run", "run" in st, "states=%s" % (",".join(sorted(st)) or "-"))
        check("B4 空格切到 jump", "jump" in st, "states=%s" % (",".join(sorted(st)) or "-"))
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
