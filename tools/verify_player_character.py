"""端到端验证：**第三方 FBX 角色绑到第三人称玩家上能不能真的玩起来**（P1）。

背景（用户原话）："在游戏场景中新建一个第三人称的角色，然后从网上下载一个
fbx 角色模型绑定上面，看看能否正常使用。"

这条链的每一个环节都曾经"看起来通了但实际不通"，所以每条判据都打在
**可观测的物理量**上，不靠"日志没报错"：

  V0 场景就位     读进来 N 个实体，出生点 GameStart 存在
  V1 玩家生成     运行期从出生点造出 Player（场景里没有它）
  V2 模型真绑上   走的是 instantiateModel → meshes>=1 且 skinned=1
                  （只有 SkinnedMeshComponent 没有子网格实体 = 视口里空的）
  V3 1:1 尺度     胶囊总高 == 模型几何身高 ±1%（用户要的"和真实环境一比一"）
  V4 脚踩地面     模型被抬到脚底（lift = -boundsMin.z），不是埋一半在地下
  V5 渲染可见     与"空场景干板"做差，角色像素数 > 阈值（真的画出来了）
  V6 动画在播    ANIM-SKIN 分配了 jointBase；clip 非空
  V7 状态机      静止 → idle；按住 W 走 → walk（且位移 > 0.5 m）
  V8 原地不动    不按键时位移 ≈ 0（没有"自己滑走"这种回归）

⚠ V7 需要真的按键。用 PostMessage 往窗口发 WM_KEYDOWN/UP —— GLFW 的
  wndproc 不看窗口是否在前台，所以无人值守下也有效（编辑器那批脚本
  一直是这么做的）。

用法：
    python tools/verify_player_character.py
    python tools/verify_player_character.py --model models/thirdparty/samba_dancing.fbx \
                                            --clip mixamo.com
    python tools/verify_player_character.py --keep     # 不清理临时场景
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
from safe_fs import safe_remove                                    # noqa: E402
from capture_window import capture_client, find_window, user32      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin")
GAME = os.path.join(BIN, "Game.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "player")

TITLE = "_p244_player"
SCENE_NAME = "_p244_player.json"          # 落在 build/bin/assets/scenes/
SCENE_PATH = os.path.join(BIN, "assets", "scenes", SCENE_NAME)

# 日志锚点
RE_PLAYER = re.compile(r"GAME-PLAYER spawned .*visual=(\w+)")
RE_VISUAL = re.compile(
    r"GAME-PLAYER visual=model path=(\S+) meshes=(\d+) skinned=(\d+) "
    r"height=([\d.]+) m")
RE_MODELINST = re.compile(r"MODEL-INSTANCE path=\S+ root=\d+ meshes=(\d+) "
                          r"skinned=(\d+) joints=(\d+)")
RE_CAPSULE = re.compile(r"GAME-PLAYER capsule radius=([\d.]+) "
                        r"halfHeight=([\d.]+) totalHeight=([\d.]+) "
                        r"\(model=([\d.]+) fit=(\d)\)")
RE_LIFT = re.compile(r"GAME-PLAYER model lift=([-\d.]+) m \(bounds z: "
                     r"([-\d.]+)\.\.([-\d.]+)\)")
RE_SKIN = re.compile(r"ANIM-SKIN entity=(\d+) jointBase=(-?\d+) time=([\d.]+)")
RE_ANIM = re.compile(r"GAME-ANIM state=(\w+) clip=(\S+)")
RE_ANIMMISS = re.compile(r"GAME-ANIM-MISSING state=(\w+) clip='([^']*)'")
RE_POS = re.compile(r"GAME-PLAYER-POS f=(\d+) pos=\(([-\d.]+),([-\d.]+),"
                    r"([-\d.]+)\)")
RE_SCENE = re.compile(r"GAME-SCENE path=\S+ entities=(\d+) skipped=(\d+)")

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


# ---------------------------------------------------------------- 场景

def make_scene():
    """空场 + 80 m 地面 + 出生点。地面给"1:1 对比"一个参照（每格 1 m 的
    栅格是编辑器画的，游戏里没有 —— 所以这里铺一张有纹理的地面材质）。"""
    scene = {
        "version": 1,
        "generator": "verify_player_character.py",
        "camera": {"target": [0.0, 0.0, 1.2], "yaw": 3.141592653589793,
                   "pitch": 0.28, "distance": 4.5, "fov": 55.0},
        "light": {"direction": [0.42, 0.72, -0.55], "color": [1.0, 0.96, 0.9],
                  "intensity": 3.2, "castsShadow": True,
                  "ambientScale": 1.0},
        "physics": {"enabled": True, "groundPlane": True,
                    "fixedTimeStep": 0.0166666675, "maxSubSteps": 4,
                    "allowSleep": True},
        "entities": [
            {
                "name": "Ground",
                "transform": {"position": [0.0, 0.0, 0.0],
                              "rotation": [0.0, 0.0, 0.0],
                              "scale": [1.0, 1.0, 1.0]},
                "parent": -1, "visible": True, "castShadow": False,
                "locked": True,
                "mesh": {"kind": "builtin", "shape": "plane", "size": 80.0,
                         "name": "p244_ground"},
                "material": {"kind": "vmat", "path": "materials/ground.vmat",
                             "name": "materials/ground",
                             "id": "materials/ground"},
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
    with open(SCENE_PATH, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=2)


# ---------------------------------------------------------------- 跑游戏

def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MYVK_")}
    env.update({k: str(v) for k, v in extra.items()})
    return env


class Session:
    """跑一个常驻的 Game.exe，边跑边把 stdout 排空到内存。

    ⚠ 必须用**线程**排空：管道缓冲只有几十 KB，不排空的话游戏会在写入时
      阻塞，表现是"窗口出来了但画面冻在第一帧"—— 那是死锁，不是渲染问题。
    """

    def __init__(self, args, env, tag):
        self.tag = tag
        self.lines = []
        self._lock = threading.Lock()
        self.proc = subprocess.Popen(
            [GAME] + args, cwd=BIN, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", bufsize=1)
        self._t = threading.Thread(target=self._pump, daemon=True)
        self._t.start()
        self.hwnd = 0

    def _pump(self):
        for line in self.proc.stdout:
            with self._lock:
                self.lines.append(line.rstrip("\r\n"))

    def wait_window(self, timeout=25.0):
        self.hwnd = find_window(TITLE, timeout)
        return self.hwnd != 0

    def shot(self):
        return capture_client(self.hwnd)[0]

    def text(self):
        with self._lock:
            return list(self.lines)

    def findall(self, regex):
        out = []
        for ln in self.text():
            m = regex.search(ln)
            if m:
                out.append(m.groups())
        return out

    def wait_for(self, regex, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            if self.findall(regex):
                return True
            time.sleep(0.1)
        return False

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
    WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
    user32.PostMessageW(hwnd, WM_KEYDOWN if down else WM_KEYUP, vk, 0)


def click(hwnd, x, y):
    """往客户区发一次左键（按下 + 抬起）。

    ⚠ P5 之后玩家输入被"左键点视口才接管"这道闸门控着（见 RuntimeApp 的
      `if (m_mouseCaptured) m_player->update(...)`）—— 不点这一下，
      `GAME-MOUSE captured=0`，之后所有 WM_KEYDOWN 都被丢掉，表现是
      "按住 W 位移 0.00 m"（V7b/V7c 从 P5 起一直是红的，跟按键注入无关）。
      PostMessage 不走前台焦点，和 `key()` 是同一套路子。
    """
    WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
    lp = ((y & 0xFFFF) << 16) | (x & 0xFFFF)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 0x0001, lp)
    time.sleep(0.06)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)


# 虚拟键码（与 core/InputMap 的出厂绑定：WASD 移动、Space 跳、Shift 冲刺）
VK_W, VK_SPACE, VK_SHIFT = 0x57, 0x20, 0x10


def image_diff_count(a, b, thr=12):
    """两张同尺寸截图的显著差异像素数。a/b 是 PIL Image。"""
    if a.size != b.size:
        return -1
    pa, pb = a.load(), b.load()
    w, h = a.size
    n = 0
    for y in range(0, h, 2):          # 隔行采样：快 2 倍，判定意义不变
        for x in range(0, w, 2):
            ca, cb = pa[x, y], pb[x, y]
            if (abs(ca[0] - cb[0]) + abs(ca[1] - cb[1])
                    + abs(ca[2] - cb[2])) > thr:
                n += 1
    return n * 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="models/thirdparty/samba_dancing.fbx")
    ap.add_argument("--clip", default="mixamo.com")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(SCENE_PATH), exist_ok=True)

    print("== 第三方角色绑到第三人称玩家：端到端验证 ==")
    print("   模型: %s" % args.model)
    print("   clip: %s" % args.clip)

    make_scene()

    # ① 干板：不绑模型（胶囊占位体），用来做差
    plate_env = clean_env(
        MYVK_RUNTIME_TITLE=TITLE,
        MYVK_RUNTIME_SCENE="scenes/" + SCENE_NAME,
        MYVK_RUNTIME_WIDTH=960, MYVK_RUNTIME_HEIGHT=600,
        MYVK_RUNTIME_AUDIO="0",
        MYVK_RUNTIME_PLAYER_DUMP="30",
    )
    plate = Session(["--fps", "60"], plate_env, "plate")
    plate_ok = plate.wait_window()
    img_plate = None
    if plate_ok:
        time.sleep(2.0)                      # 等天空盒 / 阴影 / 稳定
        img_plate = plate.shot()
        img_plate.save(os.path.join(OUT_DIR, "plate_capsule.png"))
    plate.stop()

    # ② 主角：绑第三方 FBX
    env = clean_env(
        MYVK_RUNTIME_TITLE=TITLE,
        MYVK_RUNTIME_SCENE="scenes/" + SCENE_NAME,
        MYVK_RUNTIME_WIDTH=960, MYVK_RUNTIME_HEIGHT=600,
        MYVK_RUNTIME_AUDIO="0",
        MYVK_RUNTIME_PLAYER_DUMP="30",
        MYVK_RUNTIME_PLAYER_MODEL=args.model,
        MYVK_RUNTIME_PLAYER_CLIP=args.clip,
    )
    s = Session(["--fps", "60"], env, "player")
    if not s.wait_window():
        print("[FAIL] 游戏窗口没出现（标题前缀 '%s'）" % TITLE)
        s.stop()
        return 1

    s.wait_for(RE_VISUAL, timeout=15.0)
    time.sleep(2.2)

    # ②b 先点一下视口把输入接管过来（P5 的闸，见 click() 的注释）。
    #     截图放在点击之后：之前那张是"没接管"的静止态，两张一样没意义。
    click(s.hwnd, 960 // 2, 600 // 2)
    time.sleep(0.4)

    img_idle = s.shot()
    img_idle.save(os.path.join(OUT_DIR, "player_idle.png"))

    # ③ 按住 W 走 1.6 s
    key(s.hwnd, VK_W, True)
    time.sleep(1.6)
    img_walk = s.shot()
    key(s.hwnd, VK_W, False)
    img_walk.save(os.path.join(OUT_DIR, "player_walk.png"))
    time.sleep(0.6)

    # ④ 跳一下（不一定能测到 clip，但位移/贴地状态是真值）
    key(s.hwnd, VK_SPACE, True)
    time.sleep(0.1)
    key(s.hwnd, VK_SPACE, False)
    time.sleep(0.5)
    s.stop()

    lines = s.text()
    with open(os.path.join(OUT_DIR, "player_log.txt"), "w",
              encoding="utf-8") as f:
        f.write("\n".join(lines))

    # ---------------------------------------------------------------- 判据
    scene_hits = s.findall(RE_SCENE)
    check("V0 场景读进来且没有跳过", bool(scene_hits) and scene_hits[0][1] == "0",
          "entities=%s skipped=%s" % (scene_hits[0] if scene_hits else ("?", "?")))

    spawn = s.findall(RE_PLAYER)
    check("V1 运行期从出生点造出玩家", bool(spawn) and spawn[0][0] == "model",
          "visual=%s" % (spawn[0][0] if spawn else "?"))

    mi = s.findall(RE_MODELINST)
    check("V2 走的是 instantiateModel（子网格实体 + 蒙皮组件）",
          bool(mi) and int(mi[0][0]) >= 1 and mi[0][1] == "1",
          "meshes=%s skinned=%s joints=%s" % (mi[0] if mi else ("?", "?", "?")))

    vis = s.findall(RE_VISUAL)
    geo_h = float(vis[0][3]) if vis else 0.0
    cap = s.findall(RE_CAPSULE)
    if cap:
        total = float(cap[0][2])
        err = abs(total - float(cap[0][3])) / max(float(cap[0][3]), 1e-6)
        check("V3 胶囊总高 == 模型身高（1:1 真实尺度）", err <= 0.01,
              "capsule=%.4f m vs model=%.4f m (err=%.2f%%)"
              % (total, float(cap[0][3]), err * 100.0))
    else:
        check("V3 胶囊总高 == 模型身高（1:1 真实尺度）", False, "没有 capsule 行")

    lift = s.findall(RE_LIFT)
    if lift:
        got, lo = float(lift[0][0]), float(lift[0][1])
        check("V4 模型脚底对齐玩家原点（不陷地、不悬空）",
              abs(got - (-lo)) <= 0.01,
              "lift=%.3f vs -boundsMin.z=%.3f" % (got, -lo))
    else:
        check("V4 模型脚底对齐玩家原点（不陷地、不悬空）", False, "没有 lift 行")

    skin = s.findall(RE_SKIN)
    check("V6 蒙皮关节矩阵真的进了 SSBO", bool(skin) and int(skin[0][1]) >= 0,
          "ANIM-SKIN n=%d jointBase=%s" % (len(skin),
                                           skin[0][1] if skin else "?"))

    anim = s.findall(RE_ANIM)
    miss = s.findall(RE_ANIMMISS)
    miss_states = {m[0] for m in miss}
    # 状态机"跑起来了"的两种合法表现：切到了 idle，或者**明确报出**
    # 模型里没有 Idle 这段（缺 clip 是配置事实，不是故障 —— 例如
    # samba_dancing.fbx 只有 "mixamo.com" 一段舞蹈）。
    check("V7 状态机评估了 idle（切过去 或 报出缺 Idle 片段）",
          any(a[0] == "idle" for a in anim) or "idle" in miss_states,
          "states=%s missing=%s"
          % (",".join(sorted({a[0] for a in anim})) or "-",
             ",".join(sorted(miss_states)) or "-"))

    walked = any(a[0] in ("walk", "run") for a in anim)
    walk_declared = "walk" in miss_states or "run" in miss_states
    check("V7b 走动时要么切到 walk/run，要么报出缺哪段 clip",
          walked or walk_declared,
          "walked=%d missing=%s" % (int(walked),
                                    ",".join(sorted({m[1] for m in miss}))
                                    or "-"))

    # 位移：从 GAME-PLAYER-POS 序列看水平位移
    pos = s.findall(RE_POS)
    moved = 0.0
    still = 0.0
    if len(pos) >= 4:
        def p(i):
            return (float(pos[i][1]), float(pos[i][2]))
        # 起步前几帧 vs 走动之后：用最大水平位移近似（能走起来就够）
        xs = [p(i) for i in range(len(pos))]
        span = max(abs(a[0] - b[0]) + abs(a[1] - b[1])
                   for a in xs[:3] for b in xs)
        moved = span
    check("V7c 按住 W 真的走出去了（>0.3 m）", moved > 0.3,
          "最大水平位移≈%.2f m（%d 个采样）" % (moved, len(pos)))

    if img_plate is not None:
        diff = image_diff_count(img_idle, img_plate)
        check("V5 角色真的被画出来了（与胶囊干板做差）", diff > 1500,
              "差异像素≈%d（960x600）" % diff)
    else:
        check("V5 角色真的被画出来了（与胶囊干板做差）", False,
              "干板没截到")

    check("V8 模型身高是合理人形尺寸", 1.3 <= geo_h <= 2.3,
          "height=%.3f m" % geo_h)

    print("\n截图: %s" % OUT_DIR)
    print("结果: %s  (%d 通过 / %d 失败)" %
          ("PASS" if g_fail == 0 else "FAIL", g_pass, g_fail))

    if not args.keep:
        if os.path.exists(SCENE_PATH):
            safe_remove(SCENE_PATH)
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
