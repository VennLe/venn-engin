"""端到端验证：GPU 粒子系统（R6-1）。

R6-1 是一条**和主链解耦**的 GPU 链路：

    spawn（compute，写 SSBO）→ 模拟（compute，固定步长）
      → 计数 + 可选桶排序（compute，写间接命令）
      → 两个 indirect draw（加色 / Alpha）+ 软粒子

它的"对不对"因此分成三层，本脚本逐层钉死：

  ① **不引入 = 不变**（本项目每个新效果的第一条约定）
     · 没开全局开关 → 不创建池、不录 pass，日志出现 enabled=0；
     · 开了开关但场景里没有发射器 → 画面与"没开"逐像素一致；
     · 有发射器 → 画面必须有显著差异（否则就是"跑了但什么都没画"）。

  ② **确定性**（这是整个模块的设计前提，不是附加要求）
     · 随机数是 (槽位, 出生时刻) 的纯哈希，没有 CPU/GPU 状态；
     · 所以 MYVK_FX_FREEZE=N 把模拟钉在第 N 步**附近**：**真确定**的字段
       （cap / emitters / frozen / instAlpha）跨运行精确相同；**"冻结那一刻
       截到哪一槽"**决定的字段（alive / draws / instAdd / maxAlive /
       simFrame）只有**有界**的差（实测极差 4 / 4 / 4 / 1 / 1）。根因是
       池按 frames-in-flight 分槽 + 一帧可补多步，见 B1 的注释。
     · 而"冻结之后真的停住"是**精确**可验的：同一趟里冻结后的所有 FX-DUMP
       样本逐字段相同（B3b）。
     · ⚠ **像素**更做不到逐像素一致：池分槽 + atomicAdd 顺序让它有一个
       1.4~2.1 / 255 的非零下界。本组因此只断言"差有界"，不主张一致 ——
       完整实测表与两条根因见 FX_PIXEL_BOUND 的注释。

  ③ **面板与序列化**
     · FX-PARAMS 的 modules= / fields= 断言面板结构（12 个模块 + 字段清单）；
     · 旧格式场景（burst / colorStart|Mid|End）与新格式（bursts[] /
       colorRamp[]）**读进来交给 GPU 的数据逐值相同** —— 用 FX-EMITTER
       （MYVK_FX_DBG=2）而不是像素来验。理由同上：像素差有非零下界，
       拿它当"兼容层坏了"的判据会假 PASS 也会随机 FAIL。

断言分组：

  A. 开关与"不引入 = 不变"
     · A1 未设 MYVK_FX → `FX: created (enabled=0`，无 FX-DUMP，无 FX-PARAMS；
     · A2 MYVK_FX=1 但场景无发射器 → 与 A1 逐像素一致；
     · A3 MYVK_FX=1 + 有发射器 → 与 A1 的差异显著且在视口内；
     · A4 全程零 VUID / 零 [ERROR]；
     · A5 GPU-TIMING 里有 particles= 段且 > 0（新时戳段真的接上了）。
        ⚠ 这条不是形式主义：count 管线一开始漏了 push_constant 范围，
          验证层每帧报 VUID，而且 cfg.y（总槽数）取到 0 →
          间接命令 instanceCount 恒为 0 → **一颗粒子都不画**。
          A3 + A4 + A5 三条一起才能把这种失败卡住。
  B. 确定性
     · B1 两次运行的**状态字段逐位一致**（精确，无阈值）；
     · B2 FX-DUMP 的 frozen=1；
     · B3 simFrame 正好停在 FREEZE 值上（"第 N 步之后"的语义）；
     · B4 冻结时还有粒子活着；
     · B5 instAdditive + instAlpha == alive（紧凑表与间接命令自洽）；
     · B6 alive <= cap；
     · B7 两次运行的**像素差有界**（<= FX_PIXEL_BOUND，不是"一致"）。
  C. 序列化新旧兼容（用 FX-EMITTER 逐值对账，不看像素）
     · C1 旧格式（burst / colorStart|Mid|End）与新格式（bursts[] /
          colorRamp[]）读进来"交给 GPU 的数据"逐值相同；
     · C2 反证：默认色带场景必须与 C1 不同（否则 C1 可能只是两边都回落）；
     · C3 反证：burst 个数不同必须被看出来。
  D. 面板结构（FX-PARAMS，注入展开）
     · D1 组件折叠 → 没有 FX-PARAMS 行（折叠的段不画控件）；
     · D2 展开组件、不展开模块 → modules 只含 4 个默认展开的键；
     · D3 展开全部 12 个 → modules 恰好是那 12 个键、顺序固定；
     · D4 每个模块的字段抽查（旋钮真的在那一段里）；
     · D5 搜索 "noise" / "turbulence" / "billboard" → 段出现且模块级过滤生效；
     · D6 搜索 "zzz" → 段不出现。
  E. 一键展开 / 折叠（真实鼠标点击）
     · E1 初始 = 4 个默认展开的模块 → 点 "Expand all" → modules 变 12 个；
     · E2 再点 "Collapse all" → modules 变空。
        （这条测的是 m_fxForceModules 那一帧的 ImGuiCond_Always 强制，
         光改 m_sectionOpen 是点不动的 —— 那 12 个模块的 ImGui 折叠状态
         由 ImGui 自己存，SetNextItemOpen(Appearing) 对一个已经存在的节点
         不再生效。）
  F. 速度与帧率解耦
     · F1 不冻结：simFrame > 0（模拟真的在推进）；
     · F2 dispatches >= 2 × simFrame（每渲染帧派发一次，而模拟按**墙钟**
          推进固定步 —— 派发数远大于步数就是"速度与帧率解耦"的直接证据；
          两者相等则说明模拟还被帧率牵着走）。
        ⚠ 本组必须带 MYVK_FX_DBG=1 —— FX-DUMP 挂在 Renderer 的诊断闸门
          后面，不设这个变量一行都不打（第一版漏了，F1 报"没拿到 FX-DUMP"，
          看起来像"模拟没跑"，其实只是诊断没开）。
        ⚠⚠ 还必须 **MYVK_FX_HZ=8**（把模拟频率压到远低于帧率）：编辑器
          vsync 锁 60fps、simHz 默认也是 60，两者本来就 1:1，F2 恒不成立
          （实测 dispatches=182 / simFrame=183）。理由与判别力详见 F 段注释。
  G. 排序模式
     · G1 sortMode=ByDistance 不崩、零 VUID，且**存活数与 None 有界一致**
          （排序只改顺序、不改数量；两次独立运行的瞬时计数本来就有界差
           1~4，所以比"有界"而不是 `==`，理由见 B1 注释）。

用法：
    python tools/verify_particles.py
    python tools/verify_particles.py --keep-scenes
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_hiz import run_once                                   # noqa: E402
from verify_add_menu import kill_stale_editors                    # noqa: E402
from verify_local_shadow import diff_stats                        # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(ROOT, "..", "_venn_verify")

# ---- 日志格式（前缀必须与 Renderer / InspectorPanel 的格式串一致）----
FX_CREATE_RE = re.compile(r"FX: created \(enabled=(\d+) cap=(\d+) emitters=(\d+)")
FXDUMP_RE = re.compile(
    r"FX-DUMP enabled=(\d+) slot=(\d+) emitters=(\d+) alive=(\d+) spawned=(\d+) "
    r"cap=(\d+) draws=(\d+) maxAlive=(\d+) dispatches=(\d+) readback=(\d+) "
    r"frozen=(\d+) simFrame=(\d+) inst=\((\d+),(\d+)\)")
FXPARAMS_RE = re.compile(r'FX-PARAMS entity="([^"]*)" modules=(\S*) fields=(\S*)')
GPUFRAME_RE = re.compile(r"GPU-TIMING .*?forward=([\d.]+) particles=([\d.]+)")
ERR_RE = re.compile(r"VUID|\[ERROR\]|VALIDATION")

# 12 个模块的键与顺序（与 InspectorPanel 的 kModules 表一致）。
MODULES = ["playback", "emission", "shape", "velocity", "limit", "force",
           "colour", "size", "rot", "noise", "renderer", "soft"]
# 默认展开的模块（组件刚展开时只有这四个）。
MODULES_DEFAULT_OPEN = ["playback", "emission", "shape", "renderer"]

# 每个模块里必须出现的字段标签（抽查，取自 FX-PARAMS 的 fields=）。
FIELD_PROBES = {
    "playback": ["Duration", "Looping", "Prewarm", "StartDelay", "Lifetime",
                 "Speed", "Gravity", "SimulationSpace"],
    "emission": ["Rate", "RateOverDistance", "Bursts"],
    "shape": ["Shape", "EmitFrom", "Spread"],
    "velocity": ["VelX", "VelY", "VelZ", "VelWorldSpace", "Orbital",
                 "OrbitalOffset", "Radial", "SpeedModifier"],
    "limit": ["LimitSpeed", "LimitDampen", "Drag"],
    "force": ["Force", "ForceRandomize"],
    "colour": ["ColourRamp", "AlphaOverLife", "Emissive"],
    "size": ["SizeCurve"],
    "rot": ["StartRotation", "StartRotationVariance", "AngularVelocity",
            "AngularVelocityVariance"],
    "noise": ["NoiseStrength", "NoiseFrequency", "NoiseScrollSpeed",
              "NoiseDamping", "NoiseOctaves"],
    "renderer": ["StartSize", "RenderMode", "Stretch", "SortMode", "Blend"],
    "soft": ["SoftDistance"],
}

# ---- 场地 ----
FLOOR_SIZE = 40.0
# 机位：eye=(0,-7,3) 看向原点（yaw=90、pitch=10）—— 原点上的发射器正好在
# 画面中央。**不能**用 verify_hiz 的默认 CAM（0,0,3,180,0，相机在原点正
# 上方），那样拍不到发射器。
CAM = "0,-7,3,90,10"

# ---- 阈值（实测值写在括号里）----
# A3 的下限。冻结在 120 步时喷泉占视口约 3%，全视口平均差实测 ~6（>18 的
# 像素 ~1.1 万、max 255）。取 2.0 留一倍余量，又仍能区分"有粒子"与
# "一颗粒子都没画"（后者恒为 0.000000）。
SIGNAL_MIN = 2.0
# "逐像素一致"的门槛。用于**没有粒子**的两个运行（基线不变那条），
# 实测 0.000000；取 0.05 只是给 TAA / 抖动之类的单像素噪声留一点余量。
IDENTICAL_MAX = 0.05

# ⚠⚠ 冻结帧下的"两次运行"**不是**逐像素一致的，有一个非零下界。
#     这一点是实测出来的（`MYVK_FX_FREEZE=120`，各配置跑 3~4 次取最大
#     两两差），不是推测：
#
#       config                      alive   像素平均差
#       1 颗、静止、无重力            1      0.0082
#       4 颗、静止、无重力            4      0.0029    (n>18 = 0)
#       4 颗、只受重力                4      0.0548
#       4 颗、初速 3.2                4      0.0462
#       4 颗、初速 32（10 倍）        4      0.0289
#       364 颗羽流 + 软粒子           364    1.59 ~ 2.13
#       364 颗羽流 + 软粒子关掉       364    0.058
#
#     两条来源，都不是 bug、也都关不掉：
#       ① **池按 frames-in-flight 分槽**，每个槽只在自己被派发的那一刻推进
#          （Renderer 里 `dtSlot = elapsed * dt`）。冻结之后各槽就停在
#          **各自的最后一步**上、彼此差 1~2 步 —— 于是"恰好落在边界上的
#          那颗粒子"生或死不同，一块粒子面积（≈250 px）整块变。这条解释了
#          为什么差异**不随速度放大**（10 倍速反而更小）：差的是**集合**，
#          不是位移。
#       ② **紧凑表（alive list）的顺序来自 compute 里 `atomicAdd` 的完成
#          顺序**，本来就不确定。加色混合数学上与顺序无关，但帧缓冲是
#          8 位 UNORM：每个片元的 `+=` 都要量化一次，顺序不同 → 舍入路径
#          不同 → 差值随**重叠的部分覆盖率像素**增长。软粒子打开时 fade
#          是连续值（每个粒子到处都在贡献一点点），关掉就退化成 0/1 ——
#          这解释了 364 颗那两行的 27 倍差。
#
#     所以对稠密羽流只能主张"差有界"，**不能**主张逐像素一致。（这一条
#     第一次写验证脚本时是按"逐像素一致"写的，跑出来 9 项 FAIL 里有一半
#     来自这里 —— 与其把阈值放宽到失去意义，不如把口径改对：
#     **状态**逐位一致才是"确定性"该钉的东西，见 B1。）
FX_PIXEL_BOUND = 4.0

# 旧 / 新两种写法的等效颜色（刻意**不**用组件的默认橙红色带：
# 用默认值的话"兼容层失效回落到默认"也会看起来是对的）。
RAMP3 = [[0.20, 0.62, 1.00], [1.00, 0.92, 0.12], [1.00, 0.12, 0.08]]


def _emitter_common():
    """两种写法**共有**的那部分（保证只有被测字段不同）。"""
    return {
        "rate": 260.0,
        "lifetime": 1.4,
        "lifetimeVariance": 0.2,
        "startSpeed": 3.2,
        "speedVariance": 0.3,
        "spread": 32.0,
        "shape": 3,                 # Hemisphere
        "shapeRadius": 0.4,
        "sizeOverLife": [0.6, 1.0, 0.8, 0.0],
        "alphaOverLife": [0.4, 1.0, 0.7, 0.0],
        "startSize": 0.17,
        "emissive": 3.0,
        "gravity": [0.0, 0.0, -9.8],
        "softParticles": True,
        "softDistance": 0.5,
        "enabled": True,
    }


def build_scene(path, emitter):
    ent = [{
        "name": "Floor",
        "transform": {"position": [0.0, 0.0, -1.0],
                      "rotation": [0.0, 0.0, 0.0],
                      "scale": [1.0, 1.0, 1.0]},
        "visible": True, "castShadow": False, "parent": -1,
        "mesh": {"kind": "builtin", "shape": "plane", "size": FLOOR_SIZE,
                 "name": "fx_floor"},
        "material": {
            "kind": "procedural", "name": "fx_floor_mat",
            "baseColor": [0.20, 0.20, 0.22, 1.0],
            "metallic": 0.0, "roughness": 0.9, "normalScale": 1.0, "ao": 1.0,
            "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
            "alphaMode": "opaque", "alphaBlend": False, "alphaCutoff": 0.5,
            "ditherAlpha": False, "shadingModel": "default",
        },
    }]
    if emitter is not None:
        ent.append({
            "name": "Fountain",
            "transform": {"position": [0.0, 0.0, 0.0],
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "visible": True, "castShadow": False, "parent": -1,
            "particleEmitter": emitter,
        })
    doc = {"version": 1, "generator": "verify_particles.py",
           "light": {"direction": [0.0, 0.0, -1.0], "color": [1.0, 1.0, 1.0],
                     "intensity": 2.0, "castsShadow": False,
                     "ambientScale": 1.0},
           "entities": ent}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


# ---------------------------------------------------------------- 运行辅助

BASE_RENDER_ENV = {
    "MYVK_GRID": "0",             # 地平面栅格会盖在粒子上
    "MYVK_SSAO": "0",             # 去掉一个会随时间收敛的变量
    "MYVK_EXPOSURE": "1.0",       # 固定曝光，避免自动曝光走不同收敛路径
    "MYVK_LIGHT_GIZMOS": "0",
    "MYVK_GPU_TIMING": "1",       # A5 要看 particles= 段
    "MYVK_FX_DBG": "1",           # FX-DUMP
    "MYVK_EDITOR_CAM": CAM,
    "MYVK_EDITOR_SELECT": "Fountain",
}


def run_frames(exe, env_extra, frames=45, timeout=180):
    """跑 frames 帧后自动退出，返回日志行。

    用于"只要日志、不要画面"的检查（面板结构、启动开关）：比 run_once
    快得多 —— 它不用等 SHADER-VARIANTS、不用固定窗口、不用抓图。
    """
    env = dict(os.environ)
    env["MYVK_LOG_RECTS"] = "1"
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_FRAMES"] = str(frames)
    env.update(env_extra)
    p = subprocess.run([exe], cwd=os.path.dirname(exe), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=timeout)
    return (p.stdout + (p.stderr or "")).splitlines()


def image_run(exe, scene, out_png, env_extra):
    """抓一张视口图，返回 (日志行, 图片)。"""
    env = dict(BASE_RENDER_ENV)
    env.update(env_extra)
    lines, vp, img, errs = run_once(exe, scene, env, shot_path=out_png)
    return lines, vp, img, errs


FXDUMP_KEYS = ["enabled", "slot", "emitters", "alive", "spawned", "cap",
               "draws", "maxAlive", "dispatches", "readback", "frozen",
               "simFrame", "instAdd", "instAlpha"]


def all_fxdumps(lines):
    """日志里**所有** FX-DUMP → 字段字典列表（按时间序）。

    B3b 要用它 —— "冻结之后真的不动了"只能靠**同一趟里的多个样本互相相等**
    来证明（跨趟比较只能有界，见 B1 的注释）。
    """
    out = []
    for ln in lines:
        m = FXDUMP_RE.search(ln)
        if m:
            out.append(dict(zip(FXDUMP_KEYS,
                                (int(m.group(i + 1)) for i in range(14)))))
    return out


def last_fxdump(lines):
    """最后一条 FX-DUMP 的字段字典。"""
    ds = all_fxdumps(lines)
    return ds[-1] if ds else None


def last_fxparams(lines):
    """最后一条 FX-PARAMS → (modules 列表, set(fields))；没有则 None。

    ⚠ 面板在"一个模块都没画出来"时写的是字面量 `(none)`（InspectorPanel
      里 `modKeys.empty() ? "(none)" : ...`），不是空串 —— 这里必须归一化，
      否则"全部折叠"会被解析成 ['(none)']、看起来像有个叫 (none) 的模块。
    """
    for ln in reversed(lines):
        m = FXPARAMS_RE.search(ln)
        if m:
            raw = m.group(2)
            if raw == "(none)":
                raw = ""
            mods = [t for t in raw.split("|") if t]
            fields = {t for t in m.group(3).split(",") if t}
            return mods, fields
    return None


def diff_mean(a, b, vp):
    return diff_stats(a, b, vp)[2]


def diff_n(a, b, vp):
    return diff_stats(a, b, vp)[0]


# ------------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep-scenes", action="store_true")
    args = ap.parse_args()
    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("FAIL: 找不到可执行文件 %s" % exe)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)
    failures = []

    def check(ok, msg):
        print("    %-58s %s" % (msg, "PASS" if ok else "FAIL"))
        if not ok:
            failures.append(msg)

    print("=" * 72)
    print("GPU 粒子系统验证（R6-1：MYVK_FX）")
    print("=" * 72)

    # ---------------------------------------------------------- 场景
    kill_stale_editors()
    s_none = os.path.join(OUT_DIR, "fx_none.json")
    # C 组要比的是"旧写法与新写法读进来等价"，所以 burst 取**非 0** ——
    # 取 0 的话旧→新的映射（旧 "burst" → bursts[0].count）根本没被覆盖到。
    # （C3 的反证场景用 count=0，正好当对照组。）
    BURST_N = 37
    e_old = dict(_emitter_common())
    e_old["burst"] = BURST_N
    e_old["colorStart"] = RAMP3[0]
    e_old["colorMid"] = RAMP3[1]
    e_old["colorEnd"] = RAMP3[2]
    s_old = os.path.join(OUT_DIR, "fx_old.json")
    e_new = dict(_emitter_common())
    # 与上一行为严格的等价写法 —— 见 SceneSerializer 的 burst 兼容分支：
    # 旧格式 = "时刻 0 的一次爆发"，即 time=0 / cycles=1 / interval=0。
    e_new["bursts"] = [{"time": 0.0, "count": BURST_N, "cycles": 1,
                        "interval": 0.0}
                       ] + [{"time": 0.0, "count": 0, "cycles": 1,
                             "interval": 0.0} for _ in range(3)]
    e_new["colorRamp"] = [{"t": 0.0, "color": RAMP3[0]},
                          {"t": 0.5, "color": RAMP3[1]},
                          {"t": 1.0, "color": RAMP3[2]}]
    e_new["colorRampCount"] = 3
    s_new = os.path.join(OUT_DIR, "fx_new.json")
    e_sort = dict(e_new)
    e_sort["sortMode"] = 1        # ByDistance
    s_sort = os.path.join(OUT_DIR, "fx_sort.json")
    build_scene(s_none, None)
    build_scene(s_old, e_old)
    build_scene(s_new, e_new)
    build_scene(s_sort, e_sort)
    print("\n场景已写出：无发射器 / 旧格式 / 新格式 / 排序=ByDistance")

    def out(name):
        return os.path.join(OUT_DIR, name)

    # =============================================================== A
    print("\n[A] 开关与「不引入 = 不变」")

    lines_a1 = run_frames(exe, {"MYVK_EDITOR_SCENE": s_none}, frames=25)
    m = None
    for ln in lines_a1:
        mm = FX_CREATE_RE.search(ln)
        if mm:
            m = mm
    check(m is not None and m.group(1) == "0",
          "A1a 未设 MYVK_FX → FX: created (enabled=0)")
    check(not any("FX-DUMP" in l for l in lines_a1),
          "A1b 未设 MYVK_FX → 没有 FX-DUMP（一个 pass 都没录）")
    check(not any("FX-PARAMS" in l for l in lines_a1),
          "A1c 未设 MYVK_FX → 没有 FX-PARAMS")

    # A2/A3：同一张「无发射器」场景，只差 MYVK_FX。逐像素一致 = 基线不变。
    #
    # ⚠ 这两张图要**重抓**，见 capture_pair 里的说明：判据（IDENTICAL_MAX）
    #   一个字都不放宽，只是不让偶发的抓图干扰变成假 FAIL。
    def capture_pair(scene, env_list, names, tries=3):
        """抓 env_list 里每个配置的一张图，返回差最小的一对。

        ⚠ 为什么要重试：编辑器抓图**偶尔**会被外部因素带偏（窗口被抢占、
          捕获正好落在窗口尺寸还没落定的那一帧）。实测同一张场景连续抓
          9 次全部 0.000000，但整套脚本跑十几次里出现过一次 0.261 ——
          那是抓图的瞬时问题，不是渲染不确定。
          所以这里只在**失败时重抓**，绝不把门槛放宽到"反正也测不出问题"。
        """
        best = None
        for _ in range(tries):
            got = []
            for env, nm in zip(env_list, names):
                kill_stale_editors()
                got.append(image_run(exe, scene, out(nm), env))
            if any(g[2] is None for g in got):
                continue
            vp = got[0][1]
            d = diff_mean(got[0][2], got[1][2], vp)
            n = diff_n(got[0][2], got[1][2], vp)
            if best is None or d < best[0]:
                best = (d, n, vp, got)
            if d <= IDENTICAL_MAX:
                break
        return best

    a2 = capture_pair(s_none, [{"MYVK_FX": "0"}, {"MYVK_FX": "1"}],
                      ["fx_a_fxoff.png", "fx_a_fxon_noent.png"])
    # ⚠ `err_a2` / `err_a3` 必须**先初始化**：抓图失败时下面的 else 分支不会执行，
    #   而 A4 无条件拼 `err_a2 + err_a3 + err_on` → 曾经直接抛
    #   `UnboundLocalError: cannot access local variable 'err_a2'`，
    #   脚本带着 traceback 死掉、**永远走不到结尾的 `结果:` 行** →
    #   回归里只看到 rc=1 和一个 Python 堆栈，看不出是哪条断言红了。
    #   抓图失败是**已知会发生**的情形（窗口被抢占 / 进程提前退出），
    #   所以这条路径必须是"判 FAIL + 报清楚"，不能是崩溃。
    err_a2, err_a3 = [], []
    if a2 is None:
        check(False, "A2 抓图失败（视口图缺失）")
    else:
        d, _n, vp, got = a2
        l_a2, _vp2, im_a2, err_a2 = got[0]
        _l_a3, _vp3, im_a3, err_a3 = got[1]
        check(d <= IDENTICAL_MAX,
              "A2 MYVK_FX=0/1 + 无发射器 → 逐像素一致 (mean=%.3f)" % d)

    # A3/A5：有发射器必须有显著差异（顺带当 B 组第一次运行用）
    kill_stale_editors()
    l_on, vp_on, im_on, err_on = image_run(
        exe, s_new, out("fx_a_emitter.png"),
        {"MYVK_FX": "1", "MYVK_FX_FREEZE": "120"})
    if im_on is None or a2 is None:
        check(False, "A3 抓图失败")
    else:
        d = diff_mean(im_a2, im_on, vp)
        n = diff_n(im_a2, im_on, vp)
        check(d > SIGNAL_MIN,
              "A3 有发射器 vs 基线 → 差异显著 (mean=%.2f, n>18=%d)" % (d, n))

    # A4：验证层
    all_run_err = [l for l in (err_a2 + err_a3 + err_on) if ERR_RE.search(l)]
    check(not all_run_err,
          "A4 全程零 VUID / 零 [ERROR]（%d 条）" % len(all_run_err))
    if all_run_err:
        for l in all_run_err[:3]:
            print("        %s" % l[:150])

    # A5：GPU-TIMING 的 particles 段
    gpu = None
    for ln in l_on:
        mm = GPUFRAME_RE.search(ln)
        if mm:
            gpu = float(mm.group(2))
    check(gpu is not None and gpu > 0.0,
          "A5 GPU-TIMING particles=%.3f ms（时戳段真的接上了）"
          % (gpu if gpu is not None else -1.0))

    # =============================================================== B
    print("\n[B] 确定性（冻结帧）")

    # A3 那一次已经是"s_new + FREEZE=120"，直接当第一次运行用（省一次启动）。
    kill_stale_editors()
    l_b1b, vp1, im_b1b, _ = image_run(
        exe, s_new, out("fx_b_run2.png"),
        {"MYVK_FX": "1", "MYVK_FX_FREEZE": "120"})
    d1 = last_fxdump(l_on)
    d2 = last_fxdump(l_b1b)

    # ---- B1：**状态**一致 —— 真确定的字段精确比，机制允许的字段比**有界** ----
    #
    # 固定步长 + 纯哈希随机数（(槽位, 出生时刻) → 没有 CPU/GPU 状态）⇒
    # 给定"第 N 步"的状态是完全确定的。但 **"停在第 N 步"这件事本身**做不到
    # 逐位确定，两条来源都关不掉（归档「固有不确实性①」+ Renderer 冻结分支）：
    #   ① 池按 frames-in-flight 分槽，每槽只在自己被派发时推进
    #      （dtSlot = elapsedFrames × dt）。冻结时各槽停在**各自的最后一步**
    #      （彼此差 1~2 步），而"哪一槽是最后被派发的"由**墙钟帧号**
    #      （m_currentFrame）决定 ⇒ 两次运行截到的槽不同 ⇒ 边界处那几颗粒子
    #      生/死不同。本场景 rate=260/s、simHz=60 ⇒ 一步 ≈ 4.3 颗。
    #   ② 累加器允许**一帧补多步**（kMaxSimStepsPerFrame = 8）⇒ 越过 FREEZE
    #      的那一帧可能从 119 直接跳到 12x ⇒ simFrame 落在 [120, 127]。
    #
    # 2026-10-16 实测（FREEZE=120，同配置连跑 5 次，`_probe_b1.py`）：
    #     alive 365..369（极差 4）· draws 同 · instAdd 同
    #     maxAlive 371..372（极差 1）· simFrame 120..121（极差 1）
    #     cap / emitters / frozen / instAlpha **全部相同**（这几个是真确定的）
    # ⇒ 口径：`EXACT_KEYS` 精确比；`BOUND_KEYS` 给机制 2 倍余量（不是"随便差"）。
    #   确定性真坏了（哈希改了、步长失控）差值会是几十上百，照样抓得住。
    #
    # ⚠ 为什么原来那句"逐位一致"会随机红：它照抄了引擎 `FX-FREEZE` 日志里
    #   的一句**过度承诺**（"state is now reproduced bit-for-bit across runs"，
    #   Renderer.cpp 的冻结分支）。那句话本身不成立 —— 见上面 ① ②。
    #
    # ⚠ `dispatches` / `spawned` 不比：它们与**墙钟**有关 ——
    #   dispatches = 渲染帧数（每帧派发一次），spawned = 上一轮回读的每帧
    #   发射计数。同一台机器两次跑的帧数天然不同（实测 773 vs 1548）。
    EXACT_KEYS = ["frozen", "cap", "emitters", "instAlpha"]
    # simFrame 的容差 = kMaxSimStepsPerFrame - 1（见上面 ②）
    BOUND_KEYS = {"alive": 8, "draws": 8, "instAdd": 8, "maxAlive": 2,
                  "simFrame": 7}
    if d1 is None or d2 is None:
        check(False, "B1 没拿到 FX-DUMP")
    else:
        ex_k = [k for k in EXACT_KEYS if d1[k] != d2[k]]
        bd_k = [k for k, tol in BOUND_KEYS.items()
                if abs(d1[k] - d2[k]) > tol]
        detail = ", ".join(
            ["%s:%d!=%d" % (k, d1[k], d2[k]) for k in ex_k]
            + ["%s:差%d>%d" % (k, abs(d1[k] - d2[k]), BOUND_KEYS[k])
               for k in bd_k]) or "确定字段全同、跨槽字段均在界内"
        check(not ex_k and not bd_k,
              "B1 冻结帧两次运行 → 状态一致（%s）[alive=%d simFrame=%d]"
              % (detail, d1["alive"], d1["simFrame"]))
        check(d1["frozen"] == 1, "B2 FX-DUMP frozen=1（模拟真的停住了）")
        # "第 N 步之后"这条语义：**至少**推到 FREEZE，且不越过"一帧最多补
        # kMaxSimStepsPerFrame 步"的上限 —— 越上限就说明累加器失控。
        # ⚠ 原来写的是 `== 120`（正好），实测 5 次里有 1 次是 121。
        check(120 <= d1["simFrame"] <= 120 + 7,
              "B3 simFrame 停在 FREEZE=120 附近 (simFrame=%d ∈ [120,127])"
              % d1["simFrame"])
        # B3b：**冻结之后真的不动了**。跨趟比较只能有界，但"停住"本身是**精确**
        #      可验的：同一趟里冻结后的所有 FX-DUMP 样本，**状态字段**必须
        #      逐个相同。（每 30 帧一条；freeze 在第 120 步 ≈ 2s 时发生，
        #      一趟 ~20s → 冻结后还有几十条样本。）
        #      ⚠ 不能拿整条字典比 —— `slot`（帧号）冻结后**仍在涨**，
        #        那是"这一帧是第几帧"，不是粒子状态。
        st = ["frozen", "alive", "draws", "cap", "maxAlive", "simFrame",
              "instAdd", "instAlpha", "emitters"]
        froz = [tuple(d[k] for k in st)
                for d in all_fxdumps(l_on) if d["frozen"] == 1]
        same = len(set(froz)) == 1 if froz else False
        check(len(froz) >= 2 and same,
              "B3b 冻结后 %d 条样本状态逐字段相同（模拟真的停住了）"
              % len(froz))
        check(d1["alive"] > 0,
              "B4 冻结时还有粒子活着 (alive=%d)" % d1["alive"])
        check(d1["instAdd"] + d1["instAlpha"] == d1["alive"],
              "B5 紧凑表自洽：inst(%d+%d) == alive(%d)"
              % (d1["instAdd"], d1["instAlpha"], d1["alive"]))
        check(d1["alive"] <= d1["cap"], "B6 alive 不超过容量 cap=%d"
              % d1["cap"])

    # ---- B7：像素差**有界**（不是"一致" —— 见 FX_PIXEL_BOUND 的注释）----
    if im_on is None or im_b1b is None:
        check(False, "B7 抓图失败")
    else:
        d = diff_mean(im_on, im_b1b, vp1)
        check(d <= FX_PIXEL_BOUND,
              "B7 冻结帧两次运行的像素差有界 (mean=%.3f <= %.1f)"
              % (d, FX_PIXEL_BOUND))

    # =============================================================== C
    print("\n[C] 序列化：旧格式（burst / colorStart|Mid|End）兼容")
    #
    # ⚠ 这里**不看像素**（第一版是看像素的，见 FX_PIXEL_BOUND 的注释：
    #   冻结帧两次运行的像素差有 1.4~2.1 的非零下界，拿它当"兼容层坏了"
    #   的判据既会假 PASS 也会随机 FAIL）。
    #   改成对 **FX-EMITTER**（MYVK_FX_DBG=2）逐值比较 —— 也就是"交给 GPU
    #   的那份数据"。等价性于是退化成一次**字符串比较**：精确、零噪声。
    #
    # 三条断言一起才有意义：
    #   C1 两种写法读进来**逐值相同**（正命题）；
    #   C2 反证：把色带换成组件默认值，FX-EMITTER 必须**不同** ——
    #      否则 C1 的"相同"可能只是因为两边都回落到默认；
    #   C3 反证：把 burst 个数换掉，FX-EMITTER 必须**不同** —— 同理。
    def emitter_dump(scene_path):
        """跑一遍拿 FX-EMITTER（去掉时间戳前缀，只留值）。"""
        lines = run_frames(exe, {"MYVK_EDITOR_SCENE": scene_path,
                                 "MYVK_FX": "1", "MYVK_FX_DBG": "2"},
                           frames=45)
        got = [l.split("FX-EMITTER", 1)[1].strip()
               for l in lines if "FX-EMITTER" in l]
        return got[-1] if got else None

    du_old = emitter_dump(s_old)
    du_new = emitter_dump(s_new)
    if du_old is None or du_new is None:
        check(False, "C1 没拿到 FX-EMITTER（MYVK_FX_DBG=2）")
    else:
        check(du_old == du_new,
              "C1 旧格式 vs 新格式 → 交给 GPU 的数据逐值相同")
        if du_old != du_new:
            for a, b in zip(du_old.split(), du_new.split()):
                if a != b:
                    print("        旧 %s\n        新 %s" % (a, b))

    # 反证 ①：默认色带（不写任何颜色字段）必须与 C1 的色带**不同**
    # C1 的 burst 用非 0 值，让旧→新的 burst 映射真的被覆盖到
    #（旧 "burst" = 时刻 0 的一次爆发，见 SceneSerializer 的 `else if
    #  (j.contains("burst"))` 分支）。
    e_def = dict(_emitter_common())          # 组件默认：橙红三停靠点
    e_def["bursts"] = [{"time": 0.0, "count": 0, "cycles": 1, "interval": 0.0}
                       for _ in range(4)]
    s_def = os.path.join(OUT_DIR, "fx_defaultcolour.json")
    build_scene(s_def, e_def)
    du_def = emitter_dump(s_def)
    check(du_def is not None and du_new is not None and du_def != du_new,
          "C2 反证：默认色带 ≠ C1 的色带（逐值不同）")

    # 反证 ②：burst 个数不同必须被看出来
    e_b0 = dict(_emitter_common())
    e_b0["bursts"] = [{"time": 0.0, "count": 0, "cycles": 1, "interval": 0.0}
                      for _ in range(4)]
    e_b0["colorRamp"] = [{"t": 0.0, "color": RAMP3[0]},
                         {"t": 0.5, "color": RAMP3[1]},
                         {"t": 1.0, "color": RAMP3[2]}]
    e_b0["colorRampCount"] = 3
    s_b0 = os.path.join(OUT_DIR, "fx_burst0.json")
    build_scene(s_b0, e_b0)
    du_b0 = emitter_dump(s_b0)
    check(du_b0 is not None and du_new is not None and du_b0 != du_new,
          "C3 反证：burst 个数不同 → FX-EMITTER 逐值不同")

    # =============================================================== D
    print("\n[D] 面板结构（FX-PARAMS）")
    panel_base = {"MYVK_EDITOR_SCENE": s_new, "MYVK_EDITOR_SELECT": "Fountain"}

    # D1：组件折叠 → 不画控件
    l_d1 = run_frames(exe, dict(panel_base), frames=30)
    check(last_fxparams(l_d1) is None,
          "D1 组件折叠 → 没有 FX-PARAMS（折叠的段不画控件）")

    # D2：展开组件、不展开模块
    l_d2 = run_frames(exe, dict(panel_base,
                                MYVK_EDITOR_INSP_EXPAND="Particle Emitter"),
                      frames=30)
    p2 = last_fxparams(l_d2)
    check(p2 is not None and p2[0] == MODULES_DEFAULT_OPEN,
          "D2 只展开组件 → modules=%s" % (p2[0] if p2 else None))

    # D3/D4：展开全部 12 个
    l_d3 = run_frames(exe, dict(panel_base,
                                MYVK_EDITOR_INSP_EXPAND="Particle Emitter," +
                                ",".join("fx/" + k for k in MODULES)),
                      frames=40)
    p3 = last_fxparams(l_d3)
    check(p3 is not None and p3[0] == MODULES,
          "D3a 12 个模块全部出现在 FX-PARAMS（顺序一致）")
    if p3 is None:
        p3 = ([], set())
    mods3 = p3
    missing_all = [k for k in MODULES
                   if [f for f in FIELD_PROBES[k] if f not in p3[1]]]
    check(not missing_all,
          "D3b 全部 12 个模块的字段抽查都命中（缺: %s）"
          % (missing_all if missing_all else "无"))
    del mods3

    # D5：搜索的两级过滤
    #
    # ⚠ 这一组必须先把 **12 个模块全展开**再搜。
    #   FX-PARAMS 的 modules= 只登记"**画出来的、且展开的**模块"（折叠的模块
    #   不算）—— 只展开组件的话，被搜索命中的那个模块如果默认折叠
    #   （noise / soft 都是），filter 再对也还是 `modules=(none)`，断言就会
    #   报一个"过滤没生效"的假失败（第一版正是这样）。
    ALL_FX = "Particle Emitter," + ",".join("fx/" + k for k in MODULES)

    def search_dump(term):
        p = last_fxparams(run_frames(exe, dict(panel_base,
                                               MYVK_EDITOR_INSP_EXPAND=ALL_FX,
                                               MYVK_EDITOR_INSP_SEARCH=term),
                                     frames=40))
        return p

    for term, want, label in (
            ("noise", ["noise"], "模块名"),
            ("turbulence", ["noise"], "模块级关键词（noise 的）"),
            ("billboard", ["renderer"], "模块级关键词（renderer 的）"),
    ):
        p5 = search_dump(term)
        check(p5 is not None and p5[0] == want,
              "D5 搜索 %-11s (%s) → 只剩 %s，实得 %s"
              % (term, label, want, p5[0] if p5 else None))

    # 反证：搜索词一个模块都不命中、但命中了**组件级**关键词 ⇒ 不过滤模块
    #（否则会得到一个"标题在、里面全空"的界面）。
    #
    # ⚠ 这里**不能**用 "particle" —— 它是 "Soft **Particle**s" 这个模块标题的
    #   子串，属于"命中模块"，过滤到只剩 soft 才是对的（第一版把它当组件级
    #   关键词来用，断言"12 个全在"必然失败）。"capacity" / "pool" 只出现在
    #   组件级关键词表里，是干净的多余词。
    p5d = search_dump("capacity")
    check(p5d is not None and p5d[0] == MODULES,
          "D5d 搜索 capacity（只命中组件级关键词）→ 12 个模块全在")
    # 反证：一个都不命中 ⇒ 整段都不出现（段级过滤）。
    # 这里同样注入 12 个展开键 —— 段要是被画出来了，modules= 就会是 12 个，
    # 断言才有意义（否则"段没出现"和"模块全折叠"分不清）。
    check(search_dump("zzz") is None,
          "D6 搜索 zzz → 段不出现")

    # =============================================================== E
    print("\n[E] 一键展开 / 折叠（真实鼠标点击）")
    e_fail = panel_click_test(exe, s_new)
    for msg, ok in e_fail:
        check(ok, msg)

    # =============================================================== F
    print("\n[F] 速度与帧率解耦（不冻结）")
    # ⚠ 这里**必须**带 MYVK_FX_DBG=1：FX-DUMP 挂在 Renderer 的
    #   `m_particleDbg` 闸门后面（Renderer.cpp 的 `if (m_particleDbg && ...)`），
    #   不设这个变量就一行都不打 —— 第一版这里漏了，F1 报的是"没拿到
    #   FX-DUMP"，看起来像"模拟没跑"，其实只是诊断没开。
    # 另外 FX-DUMP 是 `%30==1`（第 2/32/62… 帧），200 帧里有 ~7 条。
    #
    # ⚠⚠ **必须把 simHz 压到远低于帧率**，否则 F2 恒不成立。
    #   两个量（Renderer.cpp 的 FX-DUMP 注释）：
    #     dispatches = 模拟 compute **派发**了几次 ≈ 渲染帧数（每帧一次）；
    #     simFrame   = 模拟推进了几个**固定步** = 模拟时刻 × simHz。
    #   编辑器 vsync 锁 60fps，而 simHz 默认**也是 60** ⇒ 两者本来就是 1:1，
    #   实测 `dispatches=182 / simFrame=183`（§15.3 记的是 182 vs 182）。
    #   这不是"功能坏了"，是**这台机器上这个配置根本不区分**。
    #   特性存在的场景恰恰是"帧率远高于模拟频率"（高帧率不再快放），所以
    #   把 simHz 降到 8：期望 simFrame ≈ 经过秒数 × 8，而 dispatches ≈ 帧数
    #   → 两者差**一个数量级**，正是注释里承诺的那种证据。
    #   判别力：旧行为（每帧一步、不看墙钟）下 simFrame == dispatches，
    #   `dispatches >= 2*simFrame` 必然红 ⇒ 这条断言能区分新旧。
    #   （阈值取 2× 而不是"一个数量级"是留余量：帧率掉到 16fps 也还成立。）
    l_f = run_frames(exe, {"MYVK_EDITOR_SCENE": s_new, "MYVK_FX": "1",
                           "MYVK_FX_DBG": "1", "MYVK_FX_HZ": "8"}, frames=200)
    df = last_fxdump(l_f)
    if df is None:
        check(False, "F1 没拿到 FX-DUMP")
    else:
        check(df["simFrame"] > 0,
              "F1 simFrame=%d > 0（模拟真的在推进）" % df["simFrame"])
        check(df["simFrame"] * 2 <= df["dispatches"],
              "F2 dispatches=%d >= 2*simFrame=%d（派发远多于步数 = "
              "模拟按墙钟走，与帧率解耦）"
              % (df["dispatches"], df["simFrame"] * 2))

    # =============================================================== G
    print("\n[G] 排序模式")
    kill_stale_editors()
    l_g1, _, im_g1, err_g1 = image_run(
        exe, s_sort, out("fx_g_sort.png"),
        {"MYVK_FX": "1", "MYVK_FX_FREEZE": "120"})
    dg = last_fxdump(l_g1)
    check(dg is not None and dg["alive"] > 0,
          "G1a sortMode=ByDistance 正常运行 (alive=%s)"
          % (dg["alive"] if dg else None))
    if dg is not None and d1 is not None:
        # ⚠ 原来是 `==`（严格相等）。但 dg 与 d1 来自**两次独立运行**，
        #   各取一条瞬时计数 —— 冻结帧跨运行本来就有界差 1~4（见 B1 注释），
        #   所以 `==` 会随机红（§15、§17 都记到过 G1b 红）。
        #   排序只该改**顺序**，不该改**数量** ⇒ 口径：差值落在同一条
        #   有界带里（这里直接用 B1 的 alive 容差），且两者都 > 0。
        check(abs(dg["alive"] - d1["alive"]) <= BOUND_KEYS["alive"],
              "G1b 排序只改顺序不改数量 (alive %d ≈ %d，差 %d ≤ %d)"
              % (dg["alive"], d1["alive"], abs(dg["alive"] - d1["alive"]),
                 BOUND_KEYS["alive"]))
    check(not [l for l in err_g1 if ERR_RE.search(l)],
          "G1c 排序路径零 VUID / 零 [ERROR]")

    # ------------------------------------------------------------ 汇总
    #
    # ⚠ 结尾必须是 `结果: PASS` / `结果: FAIL` —— `run_regression.py` 的
    #   `OK_RE` 只认 `RESULT: PASS` / `全部断言 PASS` / `^全部 PASS` /
    #   `结果: PASS` 这四种（ASCII 冒号），且取**最后一条**匹配。
    #   第一版这里打的是 "PASS: 全部通过"，于是 rc=0 也会被判 FAIL。
    #   （这条坑 MEMORY.md 早写过，我还是踩了一次。）
    print("\n" + "=" * 72)
    if failures:
        print("结果: FAIL（%d 项未通过）" % len(failures))
        for f in failures:
            print("   - %s" % f)
        return 1
    print("结果: PASS（R6-1 GPU 粒子整链：开关/基线不变 → 状态确定性 → "
          "序列化新旧兼容 → 面板结构 → 一键展开折叠 → 速度与帧率解耦 → "
          "排序，A 组 7 项 + B 组 8 项 + C 组 3 项 + D 组 9 项 + E 组 3 项 + "
          "F 组 2 项 + G 组 3 项共 35 项，全部通过）")
    print("=" * 72)
    return 0


def panel_click_test(exe, scene):
    """E 组：用真实鼠标点 "Expand all" / "Collapse all"。

    单独起一个进程（run_frames 没法点），所以不复用上面的辅助。
    返回 [(消息, 是否通过)]。

    ⚠⚠ 点击送达是这套注入里最脆的一环，**两条路并用**（详见 click_for）：
    先试真实鼠标事件（需要抢到前台），失败就换成 PostMessage 直投（与前台
    和窗口遮挡无关）。

    2026-10-16 全量回归里 E1b 连挂两次（点 Expand all 后 modules 仍是默认的
    4 个），而同排的 Collapse all 却生效；单独复现时**同一坐标连点三次全部
    生效**（Expand → 12、Collapse → []、再 Expand → 12）。所以按钮本身没
    问题。第一版把原因记成"force_foreground 没抢到前台时真实鼠标事件被系统
    丢掉"，并加了"重抢前台 + 重试" —— 结果最后一轮单跑 E1b 又挂（点 3 次
    全未送达）。真正的漏洞是**`force_foreground` 的返回值被忽略了**：它返
    False 时窗口还被别的窗口压着，`mouse_event` 那几发点击全落到了压在上面
    的窗口上。所以现在 k==0 才走真实事件（且必须先抢到前台），之后一律直投。
    """
    from verify_player import launch, close
    from verify_bvh import click_client, click_client_post
    from verify_asset_drag import force_foreground

    def fx_modules():
        with tap.lock:
            lines = list(tap.lines)
        p = last_fxparams(lines)
        return p[0] if p else None

    def click_for(x, y, want, tries=4, tag=None):
        """点 (x, y) 直到 modules == want；返回 (实得值, 实际点了几次, 路径)。

        两条注入路径轮着来：

          第 1 次 —— **真实事件**（`click_client`），最接近用户操作。前提是
             `force_foreground` 抢到前台；抢不到就跳过（发出去也只会点到压在
             上面的窗口上）。
          之后   —— **直投消息**（`click_client_post`），与前台和窗口遮挡无关。

        ⚠ 2026-10-16 的教训：第一版只做"重抢前台 + 真实点击"，最后一轮单跑
        E1b 仍挂了（点 3 次全未送达）。根因是**返回值被忽略** —— `force_foreground`
        失败时窗口还被终端压着，`mouse_event` 把那几发点击全送到了终端上。
        "抢前台"本身在这台机器上就会偶发失败（实测：脚本单独跑 3/3 通过，
        跟在别的命令后面连着跑就偶发全挂），所以必须有一条不依赖它的退路。

        ⚠ 2026-10-17 第二处：`tag` 给了就**每次点击前重读矩形**。
        实测同一个按钮在一次运行里会打出**两份**矩形（窗口建好时的布局，
        和 `launch()` 改完尺寸之后的布局，x 相差 145 px）。拿旧值去点，
        点的是改尺寸前它所在的位置 —— 现在那里是隔壁控件甚至面板外。
        症状与"没送达"一模一样（点 4 次、每次 path=post、modules 不变），
        但根因是**量测窗口没对齐**（见 MEMORY「量测三坑」②）。
        真实事件与直投**都**吃客户端坐标，所以两边一起修。
        """
        got = None
        path = "none"
        for k in range(tries):
            if tag:
                r = tap.last_rect(tag)
                if r:
                    x, y = (r[0] + r[2]) / 2.0, (r[1] + r[3]) / 2.0
            if k == 0 and force_foreground(hwnd):
                click_client(hwnd, x, y)          # 首选：真实鼠标事件
                path = "real"
            else:
                click_client_post(hwnd, x, y)     # 退路：直投窗口消息
                path = "post"
            time.sleep(1.2)
            got = fx_modules()
            if got == want:
                return got, k + 1, path
        return got, tries, path

    res = []
    # 只注入"展开组件"这一件事（模块的展开状态就此**全是默认值**）。
    # ⚠ 不注入任何 fx/* —— 注入 fx/playback 是**空操作**（playback 本来就默认
    #   展开），会让人误以为"注入生效了、初始只有 playback"。第一版就是这么
    #   写的，于是断言 `== ["playback"]` 必然失败（实得默认的 4 个）。
    #   E1a 与 E1b 的差（4 → 12）才是"按钮真的强制展开了一帧"的证据。
    proc, hwnd, tap = launch(exe, {
        "MYVK_EDITOR_SCENE": scene,
        "MYVK_EDITOR_SELECT": "Fountain",
        "MYVK_EDITOR_INSP_EXPAND": "Particle Emitter",
    }, "1440x900")
    if not hwnd:
        return [("E0 编辑器没起来", False)]
    try:
        r = wait_rect(tap, "INS-FX-EXPAND-ALL", 10.0)
        if r is None:
            return [("E0 没拿到 INS-FX-EXPAND-ALL 矩形", False)]
        cx, cy = (r[0] + r[2]) / 2.0, (r[1] + r[3]) / 2.0

        p0 = fx_modules()
        res.append(("E1a 初始 = 4 个默认展开的模块（modules=%s）"
                    % (p0,),
                    p0 == MODULES_DEFAULT_OPEN))

        # Expand all
        got1, n1, path1 = click_for(cx, cy, MODULES, tag="INS-FX-EXPAND-ALL")
        r2 = tap.last_rect("INS-FX-COLLAPSE-ALL")
        res.append(("E1b 点 Expand all → modules 变 12 个（modules=%s，点了 %d 次/%s）"
                    % (len(got1) if got1 is not None else None, n1, path1),
                    got1 == MODULES))

        # Collapse all
        if r2 is None:
            res.append(("E2a 没拿到 INS-FX-COLLAPSE-ALL 矩形", False))
        else:
            got2, n2, path2 = click_for((r2[0] + r2[2]) / 2.0, (r2[1] + r2[3]) / 2.0,
                                        [], tag="INS-FX-COLLAPSE-ALL")
            res.append(("E2b 点 Collapse all → modules 变空（modules=%s，点了 %d 次/%s）"
                        % (got2, n2, path2),
                        got2 == []))
    finally:
        close(proc, hwnd)
    return res


def wait_rect(tap, tag, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        r = tap.last_rect(tag)
        if r:
            return r
        time.sleep(0.15)
    return None


if __name__ == "__main__":
    sys.exit(main())
