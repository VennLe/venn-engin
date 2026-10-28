"""端到端验证：关节（J5）—— 从组件 → 序列化 → 求解器 → 界面的整条链路。

背景：`physics/Joints.h` 里球窝/铰链/滑动/距离/焊死全都实现好了，但编辑器
里既加不上组件、也配不了另一端，于是门 / 摆锤 / 链条 / 布娃娃这些最常用的
物理玩法一个都做不出来。这一轮补上的是**作者侧那一半**：

    ecs::JointComponent → 场景 JSON → buildSceneWorld() → physics::Joint

并且补了三个"看得见"的入口：Inspector 的 Joint 段、Physics 面板的关节总览、
以及 Play 之后真正被约束住的行为。

------------------------------------------------------------
为什么断言要这么设计（而不是"有日志就算过"）
------------------------------------------------------------
关节最恶劣的失败形态是**静默**：日志一行不报、程序不崩、物体照常下落，
只是"没被拉住"。所以不能断言"配置存在"，必须断言"运动学上确实被约束了"。
判据取的是**到铰点的距离**：

    Arm      起点 (1,0,4)，铰点 (0,0,4) —— 到铰点距离恒为 1.0（刚性连杆）
    FreeFall 同一高度起跳、没有关节 —— 到同一个铰点的距离会一路涨到 5.7

两条同时成立才算数：前者证明"约束生效"，后者证明"这个判据能区分有没有
约束"（否则把判据写成"距离 < 100"也能过，等于没测）。这就是测试纪律里
要求的**对照区** —— 没有对照区的断言，不知道它在防什么。

------------------------------------------------------------
覆盖
------------------------------------------------------------
  A. 铰链真的拉住一个动态体（含自由落体对照）
  B. 四种坏配置全被拦下：目标悬空 / 没选目标 / 对面没刚体 / 关掉的静默不建
  C. 存盘往返：File ▸ Save As 出来的文件里 joint 块逐字段不变（真字节）
  D. 界面：Inspector 的 Joint 段 + Physics 面板的关节总览，且点一行能选中
  E. 四种类型的**运动学判据**：Ball / Fixed / Slider / Distance，每种都配一个
     同几何、同初值的无约束对照（判据分不开的两者等于没测）
  F. 脚本 API：`joint` / `jointAnchors` / `jointAxis` / `jointKind` /
     `jointTarget` 在**编辑器 Play** 里真的改到组件与求解器，且拼错类型名
     会报错（不是静默失败）
  G. 视口拾取：真鼠标点两下按钮 + 点两下视口，用日志里的 `world` / `local`
     独立复算 `world = pos + scale·local`（换算写错就红），外加 Esc 只退拾取
     不清选中
  H. 3D 调试绘制：`RENDER joint-gizmos n=… verts=…` 真的算出来了，且**竖直
     连杆**也在（漏了参考轴换向那条分支就只剩 3 个几何体）；默认关

------------------------------------------------------------
否定测试（必须做，结果记在文档里）
------------------------------------------------------------
    python tools/verify_joints.py --negate

`--negate` 把夹具里 Arm 的 joint 块整个删掉（模拟"组件没生效 / 序列化漏了
这一块"）。A 组必须变红，且红在"到铰点距离恒为 1.0"和"末态停在正下方"
这两条上 —— 如果 `--negate` 也能过，说明 A 组断言是空转的，比不测更糟。

用法：
    python tools/verify_joints.py
    python tools/verify_joints.py --keep      # 保留夹具与日志（排查用）
"""

import argparse
import json
import math
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_window import find_window, user32                        # noqa: E402
from verify_add_menu import esc, kill_stale_editors                   # noqa: E402
from verify_asset_drag import force_foreground, wheel_at              # noqa: E402
from verify_character_import import click_tag                         # noqa: E402
from verify_menu_wiring import LogTap, click, do_menu, wait_rect      # noqa: E402
from safe_fs import safe_remove                                       # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ROOT, "build", "bin", "Editor.exe")
OUT_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify")

# ---------------------------------------------------------------- guid
#
# 16 位小写 hex，**必须补满** —— `parseGuidHex` 长度不等于 16 直接判非法，
# 于是"存一次再读"之后实体变成无身份、被随机补发新 guid，关节就悬空了。
# 这三个值本身没有含义，只要非 0（0 = kInvalidGuid）且互不相同。
G_PIVOT = "a1b2c3d4e5f00001"
G_ARM = "a1b2c3d4e5f00002"
G_FREE = "a1b2c3d4e5f00003"
G_GHOST = "a1b2c3d4e5f00004"
G_BAD = "00000000deadbeef"        # 合法格式，但场景里不存在
G_ORPHAN = "a1b2c3d4e5f00011"
G_NOTARGET = "a1b2c3d4e5f00012"
G_NOBODY = "a1b2c3d4e5f00013"
G_OFF = "a1b2c3d4e5f00014"
# E 组（四种类型的运动学判据）：每种一条关节体 + 一个对照
G_K_BALL = "a1b2c3d4e5f00021"
G_K_BALLPIVOT = "a1b2c3d4e5f00022"
G_K_FIXED = "a1b2c3d4e5f00023"
G_K_FIXEDPIVOT = "a1b2c3d4e5f00024"
G_K_SLIDE = "a1b2c3d4e5f00025"
G_K_SLIDEPIVOT = "a1b2c3d4e5f00026"
G_K_SLIDEFREE = "a1b2c3d4e5f00027"
G_K_DIST = "a1b2c3d4e5f00028"
G_K_DISTPIVOT = "a1b2c3d4e5f00029"
G_K_DISTFREE = "a1b2c3d4e5f0003a"
# F 组（脚本 API）
G_S_BALL = "a1b2c3d4e5f00031"
G_S_PIVOT = "a1b2c3d4e5f00032"
G_S_FREE = "a1b2c3d4e5f00033"
G_S_BAD = "a1b2c3d4e5f00034"
# H 组（3D 调试绘制）：**竖直连杆**专用夹具
G_V_PIVOT = "a1b2c3d4e5f00041"
G_V_ROD = "a1b2c3d4e5f00042"

# 铰点（Arm 的 anchorSelf 换算到世界）—— 断言"到铰点距离"时用。
ANCHOR = (0.0, 0.0, 4.0)

BODY_RE = re.compile(
    r"PHYS-BODY f=(\d+) name='([^']*)' pos=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"vel=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) awake=(\d)")
BUILT_RE = re.compile(
    r"PHYS-JOINT built a=(\d+) b=(\d+) kind=(\d+) limit=(\d+) motor=(\d+)")
# ⚠ `target=N` 这一段**只在 no-rigid-body 那条里出现**（另外两条打到这里时
#   目标还没解析出来，没得可打）。正则不留这个可选项，"对面没刚体"那条就
#   一条也匹配不上 —— 表现为"三种坏法只报了两个"，而真因在正则而不在引擎。
SKIP_RE = re.compile(
    r"PHYS-JOINT skip entity=(\d+)(?: target=\d+)? reason=([a-z-]+)")
# 拾取一行把"世界"和"局部"都打了出来 —— 这是唯一能在测试侧**独立复算**
# 那条换算的入口（见 G 组的判据）。
PICK_RE = re.compile(
    r"JOINT-PICK apply=(\w+) entity=(\d+) space=(\d+) source=([a-z-]+) "
    r"world=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) "
    r"local=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) changed=(\d)")


# ---------------------------------------------------------------- 夹具

def _mat(name, color):
    return {"kind": "procedural", "name": name, "baseColor": color,
            "metallic": 0.0, "roughness": 0.6, "normalScale": 0.0, "ao": 1.0,
            "emissive": [0.0, 0.0, 0.0], "doubleSided": False,
            "alphaBlend": False}


def cube(name, guid, pos, scale, color, body=None):
    e = {"name": name, "guid": guid,
         "transform": {"position": list(pos), "rotation": [0.0, 0.0, 0.0],
                       "scale": list(scale)},
         "parent": -1, "visible": True, "castShadow": True,
         "mesh": {"kind": "builtin", "shape": "cube", "size": 1.0,
                  "name": name.lower() + "_mesh"},
         "material": _mat(name.lower() + "_mat", color)}
    if body:
        e["rigidbody"] = body
    return e


def dyn_box(**kw):
    """动态盒。±0.5 的盒子 × scale 就是物理盒（`scaleShapeForWorld` 里
    盒体走 `halfExtents *= scale`，所以视觉和碰撞体是对齐的）。

    `angularDamping` 故意调大：无阻尼的单摆会一直摆，末态位置取决于采样
    相位，断言只能写"动过" —— 那是弱判据。有阻尼它才会收敛到**唯一**的
    稳定平衡位置（铰点正下方 1 m 处），于是"末态 = (0,0,3)"这种精确断言
    才成立，也才真的能区分"被铰链拉住"和"自由落体"。
    """
    b = {"bodyType": 2, "shape": 2, "halfExtents": [0.5, 0.5, 0.5],
         "density": 500.0, "useGravity": True, "allowSleep": True,
         "linearDamping": 0.2, "angularDamping": 1.5}
    b.update(kw)
    return b


def hinge(target_guid, anchor_self=(-1.0, 0.0, 0.0),
          anchor_target=(0.0, 0.0, -16.0), axis=(0.0, 1.0, 0.0), **kw):
    j = {"kind": 2, "target": target_guid,
         "anchorSelf": list(anchor_self), "anchorTarget": list(anchor_target),
         "axis": list(axis), "distance": 1.0,
         "useLimit": False, "lowerLimit": -1.2, "upperLimit": 1.2,
         "useMotor": False, "motorSpeed": 0.0, "maxMotorForce": 0.0,
         "enabled": True}
    j.update(kw)
    return j


def joint(kind, target, anchor_self=(-1.0, 0.0, 0.0),
          anchor_target=(0.0, 0.0, -16.0), axis=(0.0, 1.0, 0.0), **kw):
    """任意类型的关节块。`hinge()` 是它的 kind=2 特例，语义完全一致 ——
    之所以留着 `hinge()` 这个名字，是因为 A/B/C/D 四组的夹具和断言都按
    "铰链"写的，改名只会让 diff 变大。"""
    j = hinge(target, anchor_self, anchor_target, axis, **kw)
    j["kind"] = kind
    return j


def static_sphere(name, guid, pos):
    """静态支点（铰点挂在这里）。用球是为了"无论从哪个方向看都只有 0.2 m"，
    接触面积最小 —— 支点就算离关节体很近也不会蹭出接触力。"""
    return {"name": name, "guid": guid,
            "transform": {"position": list(pos),
                          "rotation": [0.0, 0.0, 0.0],
                          "scale": [1.0, 1.0, 1.0]},
            "parent": -1, "visible": True, "castShadow": True,
            "rigidbody": {"bodyType": 0, "shape": 1, "radius": 0.2,
                          "density": 500.0, "useGravity": False,
                          "allowSleep": True}}


def make_hinge_scene(negate=False):
    """A 组夹具：一根横着的 Arm，用铰链吊在一个**很远**的静态支点上。

    为什么支点摆在 (0,0,20) 而不是铰点旁边：静态支点也有碰撞体，摆在铰点
    附近的话，Arm 一边绕铰点转一边和支点蹭，接触力会污染轨迹（典型的
    "关节看起来有点软"）。把支点丢到 16 m 外、只靠 `anchorTarget` 把铰点
    搬回来，就完全没有接触干扰 —— 这也是 `localAnchor` 用局部坐标的意义。

    为什么 Arm 起点是**横着**的：竖直下垂是稳定平衡，摆在那里不动，
    "关节生效"和"关节没生效但也没被推"分不开。横着放才有明确的运动。
    """
    pivot = {"name": "Pivot", "guid": G_PIVOT,
             "transform": {"position": [0.0, 0.0, 20.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "parent": -1, "visible": True, "castShadow": True,
             "rigidbody": {"bodyType": 0, "shape": 1, "radius": 0.2,
                           "density": 500.0, "useGravity": False,
                           "allowSleep": True}}
    arm = cube("Arm", G_ARM, (1.0, 0.0, 4.0), (2.0, 0.2, 0.2),
               [0.85, 0.45, 0.30, 1.0], dyn_box())
    if not negate:
        arm["joint"] = hinge(G_PIVOT)
    free = cube("FreeFall", G_FREE, (1.0, 4.0, 4.0), (2.0, 0.2, 0.2),
                [0.30, 0.55, 0.85, 1.0], dyn_box())
    return {"version": 1, "generator": "verify_joints",
            "camera": {"target": [0.0, 0.0, 3.0], "yaw": 2.2, "pitch": 0.3,
                       "distance": 14.0},
            "physics": {"enabled": True, "groundPlane": True},
            "entities": [pivot, arm, free]}


def make_vertical_joint_scene():
    """H 组夹具：两端锚点**在一条竖线上**（差分方向 z 分量 = ±1）。

    为什么专门造这么一份：可视化里的连杆是"线段 → 拉长的立方体"，要构造
    一组正交基，而做法是 `cross(ref, z)`。参考轴若还是取世界 Z（也就是
    连杆方向本身），叉乘是**零向量**，归一化后整条连杆塌成零尺寸 ——
    画面上"竖直的绳子"凭空消失，而且**不报错**。摆锤的绳、起重机吊钩的
    缆、门的合页轴，全是竖直的。

    判据就是几何体条数：两端球 + 连杆 + 铰链轴 = **4**；漏了换参考轴那条
    分支就只有 3（连杆没生成）。这是"两种实现会不一样的那个量"。
    """
    pivot = {"name": "VPivot", "guid": G_V_PIVOT,
             "transform": {"position": [0.0, 0.0, 8.0],
                           "rotation": [0.0, 0.0, 0.0],
                           "scale": [1.0, 1.0, 1.0]},
             "parent": -1, "visible": True, "castShadow": True,
             "rigidbody": {"bodyType": 0, "shape": 1, "radius": 0.2,
                           "density": 500.0, "useGravity": False,
                           "allowSleep": True}}
    # 竖直的杆：中心 (0,0,6)、高 2 m（z ∈ [5,7]），铰点在杆顶 (0,0,6)+0 ——
    # 锚点都取 0，世界位置上 pa=(0,0,6)、pb=(0,0,8)，差 = 纯 +Z。
    rod = cube("VRod", G_V_ROD, (0.0, 0.0, 6.0), (0.4, 0.4, 2.0),
               [0.85, 0.45, 0.30, 1.0], dyn_box(angularDamping=0.4))
    rod["joint"] = joint(2, G_V_PIVOT, anchor_self=(0.0, 0.0, 0.0),
                         anchor_target=(0.0, 0.0, 0.0), axis=(0.0, 1.0, 0.0))
    return {"version": 1, "generator": "verify_joints",
            "camera": {"target": [0.0, 0.0, 6.0], "yaw": 2.2, "pitch": 0.15,
                       "distance": 12.0},
            "physics": {"enabled": True, "groundPlane": True},
            "entities": [pivot, rod]}


def make_broken_scene():
    """B 组夹具：四种配错的关节，一种配对的关掉。

    四种坏法**各自对应 buildSceneWorld 里的一条 WARN 分支**，一条都不能少 ——
    这四条是"用户照着 Inspector 乱填"能到达的全部状态，漏掉哪条，那种填法
    就会静默失败（物体照常下落，用户以为关节坏了）。
    """
    ents = [
        # 静态支点：给 Off 那条当合法目标
        {"name": "Pivot", "guid": G_PIVOT,
         "transform": {"position": [0.0, 0.0, 20.0],
                       "rotation": [0.0, 0.0, 0.0],
                       "scale": [1.0, 1.0, 1.0]},
         "parent": -1, "visible": True, "castShadow": True,
         "rigidbody": {"bodyType": 0, "shape": 1, "radius": 0.2,
                       "density": 500.0, "useGravity": False,
                       "allowSleep": True}},
        # 只有网格、**没有刚体** —— NoBody 那条指的就是它
        cube("Ghost", G_GHOST, (0.0, 20.0, 4.0), (1.0, 1.0, 1.0),
             [0.7, 0.7, 0.2, 1.0]),
    ]

    orphan = cube("Orphan", G_ORPHAN, (0.0, 0.0, 6.0), (1.0, 1.0, 1.0),
                  [0.9, 0.4, 0.3, 1.0], dyn_box())
    orphan["joint"] = hinge(G_BAD)                      # → target-missing
    notarget = cube("NoTarget", G_NOTARGET, (3.0, 0.0, 6.0), (1.0, 1.0, 1.0),
                    [0.9, 0.6, 0.3, 1.0], dyn_box())
    notarget["joint"] = hinge("")                       # → no-target
    nobody = cube("NoBody", G_NOBODY, (6.0, 0.0, 6.0), (1.0, 1.0, 1.0),
                  [0.6, 0.4, 0.9, 1.0], dyn_box())
    nobody["joint"] = hinge(G_GHOST)                    # → no-rigid-body
    off = cube("Off", G_OFF, (9.0, 0.0, 6.0), (1.0, 1.0, 1.0),
               [0.4, 0.8, 0.6, 1.0], dyn_box())
    off["joint"] = hinge(G_PIVOT, enabled=False)        # → 静默，不算 skip

    ents += [orphan, notarget, nobody, off]
    return {"version": 1, "generator": "verify_joints_broken",
            "camera": {"target": [4.0, 0.0, 3.0], "yaw": 2.2, "pitch": 0.25,
                       "distance": 16.0},
            "physics": {"enabled": True, "groundPlane": True},
            "entities": ents}


def write_scene(path, scene):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(scene, f, indent=2, ensure_ascii=False)
        f.write("\n")
    return path


# ------------------------------------------------ 夹具：四种类型的运动学判据
#
# 一条关节"生效"和"配错了"在画面上极其相似（都可能在动、也都可能不动），
# 所以每一种都要有**一个对照物**：同一场景里放一个几何完全相同、初速度完全
# 相同、就是没有关节的物体。判据必须能把这两者分开 —— 分不开的判据等于
# 没测（这正是 A 组拿 FreeFall 当对照的理由，这里只是把它推广到四种类型）。
#
# 四种类型各自"应当能做什么"，决定了判据只能这么写：
#
#   Ball      锚点锁死、**朝向自由**。朝向不进 PHYS-BODY（只有 pos/vel），
#             所以把锚点从质心挪开 1 m：重力绕锚点产生力矩 → 它会**摆下来**。
#             "摆下来"这件事只能由转动产生，于是它就是"转动自由度活着"的
#             可观测替身。对照取 **Fixed**（同样锁位置、但朝向也锁 → 摆不动）：
#             两者位置判据完全相同（到锚点恒 1.0），差别只在"能不能转"，
#             所以这条对照是**唯一**能把 Ball 和 Fixed 分开的判据。
#
#   Slider    只留轴向。**必须给一个横向初速度**下去 —— 否则"x 不动"是
#             自由落体也满足的废话（竖直下落本来就不改 x）。给了 2 m/s 的
#             +X 初速度之后：有约束的 x 恒为 0，对照的 x 会跑出 1 m 以上。
#
#   Distance  两点间距恒定、切向自由。距离判据本身在"无约束"下也会**不**成立
#             （对照物直线下落 → 距离从 2 m 涨到 5.8 m），所以它自带对照；
#             再加一条"它真的摆下来了"证明切向没被锁死。
#
# 三个轴向（y=0 / 8 / 16 / 24）分开摆，互不接触 —— 竖直下落的物体不会
# 横着串门。
K_BALL_REF = (0.0, 0.0, 4.0)      # Ball 的铰点（世界）
K_FIXED_REF = (0.0, 8.0, 4.0)     # Fixed 的铰点（世界）
K_SLIDE_REF = (0.0, 16.0, 6.0)    # Slider 的 B 端锚点（A 端会沿 Z 走开）
K_DIST_REF = (0.0, 24.0, 6.0)     # Distance 的 B 端锚点
K_DISTFREE_REF = (0.0, 32.0, 6.0)  # 对照物旁边那个"虚拟锚点"


def make_kinematic_scene():
    """E 组夹具：Ball / Fixed / Slider / Distance + 各自的对照。"""
    arm = (2.0, 0.2, 0.2)          # 横着的细长盒：重心在体心，锚点在一端
    ents = [
        static_sphere("BallPivot", G_K_BALLPIVOT, (0.0, 0.0, 20.0)),
        static_sphere("FixedPivot", G_K_FIXEDPIVOT, (0.0, 8.0, 20.0)),
        static_sphere("SlidePivot", G_K_SLIDEPIVOT, (0.0, 16.0, 20.0)),
        static_sphere("DistPivot", G_K_DISTPIVOT, (0.0, 24.0, 20.0)),
    ]

    # ---- Ball：锚点在 -X 端 1 m 处，起始横着放（和 A 组同一手法：
    #      竖直下垂是稳定平衡，摆在那里不动就分不出"生效"与"没被推"）。
    ball = cube("KBall", G_K_BALL, (1.0, 0.0, 4.0), arm,
                [0.85, 0.45, 0.30, 1.0], dyn_box())
    ball["joint"] = joint(1, G_K_BALLPIVOT, anchor_self=(-1.0, 0.0, 0.0),
                          anchor_target=(0.0, 0.0, -16.0))

    # ---- Fixed 对照：几何、初值、锚点全同，只有 kind 不同。
    fixed = cube("KFixed", G_K_FIXED, (1.0, 8.0, 4.0), arm,
                 [0.45, 0.45, 0.85, 1.0], dyn_box())
    fixed["joint"] = joint(0, G_K_FIXEDPIVOT, anchor_self=(-1.0, 0.0, 0.0),
                           anchor_target=(0.0, 0.0, -16.0))

    # ---- Slider：竖直滑块 + 2 m/s 的 +X 初速度（横向探针）。
    #      轴 (0,0,1) 是 **A 的局部空间**；A 的旋转是单位四元数 → 就是世界 +Z。
    slider = cube("KSlide", G_K_SLIDE, (0.0, 16.0, 6.0), (1.0, 1.0, 1.0),
                  [0.35, 0.75, 0.55, 1.0],
                  dyn_box(initialVelocity=[2.0, 0.0, 0.0]))
    slider["joint"] = joint(3, G_K_SLIDEPIVOT, anchor_self=(0.0, 0.0, 0.0),
                            anchor_target=(0.0, 0.0, -14.0),
                            axis=(0.0, 0.0, 1.0))
    slide_free = cube("KSlideFree", G_K_SLIDEFREE, (4.0, 16.0, 6.0),
                      (1.0, 1.0, 1.0), [0.75, 0.55, 0.35, 1.0],
                      dyn_box(initialVelocity=[2.0, 0.0, 0.0]))

    # ---- Distance：从 (2,24,6) 起跳、连到 (0,24,6)，绳长 2 m。
    #      它必须**摆下来**（切向自由），而不是原地不动 —— 原地不动那是 Fixed。
    dist = cube("KDist", G_K_DIST, (2.0, 24.0, 6.0), (1.0, 1.0, 1.0),
                [0.85, 0.75, 0.30, 1.0], dyn_box())
    dist["joint"] = joint(4, G_K_DISTPIVOT, anchor_self=(0.0, 0.0, 0.0),
                          anchor_target=(0.0, 0.0, -14.0), distance=2.0)
    dist_free = cube("KDistFree", G_K_DISTFREE, (2.0, 32.0, 6.0),
                     (1.0, 1.0, 1.0), [0.55, 0.75, 0.85, 1.0], dyn_box())

    ents += [ball, fixed, slider, slide_free, dist, dist_free]
    return {"version": 1, "generator": "verify_joints_kinematic",
            "camera": {"target": [0.0, 12.0, 3.0], "yaw": 2.2, "pitch": 0.3,
                       "distance": 40.0},
            "physics": {"enabled": True, "groundPlane": True},
            "entities": ents}


# ------------------------------------------------ 夹具：脚本 API（J5-4）
#
# 脚本只能**改**关节，不能凭空造（它没能力给实体加刚体/组件）。所以夹具
# 先给 KSBall 挂一个"有组件、但没选另一端"的关节 —— 这正是 Inspector 里
# 刚点完 Add Joint 的状态，也是用户写脚本时最可能面对的状态。
#
# 起始 kind 故意配 Fixed（0）而不是 Ball：脚本调完 `joint(..., "ball")` 之后
# `jointKind()` 从 "Fixed" 变成 "Ball"，这一条**只有真的写进组件**才会变。
# 若夹具本来就是 Ball，"变没变"看不出来，判据就空了。
#
# ⚠ 为什么等到 `frame > 3` 才动手，而不是在 `on start` 里：编辑器一帧内的
#   顺序是 **脚本 → 玩家 → 物理**，而物理世界是"进 Play 的第一帧才建"。
#   在 `on start` 里改关节，那时 `m_physics` 还是空的 → `rebuildJoints`
#   只能返回 0（组件改了、求解器还没建，紧接着 buildSceneWorld 才把它建出来）。
#   等几帧，测的正好是 J5-4 真正要保证的那件事：**运行途中**改关节能当场
#   重建求解器里的约束，而不重建刚体（不把全场的速度和位置撞回初始值）。
#
# ⚠⚠ `let` 只允许出现在**顶层**（这是脚本语言的硬规矩，写在钩子里直接编译
#    失败："let is only allowed at the top level"）。所以 `armed` 声明在
#    顶层，钩子里只用赋值；其它变量一律不声明，直接就地调用。
#    踩过：第一版写成 `let b = self()` 放在 `on update` 里 → 整个脚本
#    "not runnable"，表现是**什么日志都没有**，看着像"脚本没跑"。
S_SCRIPT = """\
# 由 verify_joints.py（F 组）生成：脚本配关节
let armed = 0
on update {
    if (armed == 0 && frame > 3) {
        armed = 1
        print("SCRIPT-JOINT before", jointKind(self()))
        print("SCRIPT-JOINT set", joint(self(), find("KSBallPivot"), "ball"))
        print("SCRIPT-JOINT kind", jointKind(self()))
        print("SCRIPT-JOINT target", name(jointTarget(self())))
        jointAnchors(self(), vec3(-1, 0, 0), vec3(0, 0, -16))
        print("SCRIPT-JOINT anchors", true)
        print("SCRIPT-JOINT axis", jointAxis(self(), vec3(0, 1, 0)))
    }
}
"""

# 故意拼错类型名。**错误的形状**也要测：`joint()` 失败时会挂一条错误，
# 引擎侧必须同时打一条 WARN（`EDITOR-JOINT set: unknown kind`）。
# 静默失败是最恶劣的 —— 用户只看到脚本"什么都没做"。
#
# ⚠ 这里**故意不写 `print(joint(...))`**：失败时错误挂在语句上，语句里
#   自带的 print 根本不会执行 —— 写成那样会让人误以为"返回值测过了"。
#
# ⚠ KSBad 的关节块**不选目标**：它只要一个 `JointComponent` 让脚本能走到
#   "解析类型名"那一步。给了目标的话，Play 建世界时会先把这条关节建起来，
#   再被脚本判为拼错 —— 两条日志混在一起，反而不好读。
S_BAD_SCRIPT = """\
let armed = 0
on update {
    if (armed == 0 && frame > 3) {
        armed = 1
        joint(self(), find("KSBallPivot"), "bogus")
    }
}
"""


def make_script_scene(script, bad_script):
    ents = [static_sphere("KSBallPivot", G_S_PIVOT, (0.0, 0.0, 20.0))]
    ball = cube("KSBall", G_S_BALL, (1.0, 0.0, 4.0), (2.0, 0.2, 0.2),
                [0.85, 0.45, 0.30, 1.0], dyn_box())
    # 有组件、没选目标（= 刚点完 Add Joint 的样子），kind 先填 Fixed
    ball["joint"] = joint(0, "", anchor_self=(-1.0, 0.0, 0.0),
                          anchor_target=(0.0, 0.0, -16.0))
    ball["script"] = {"path": script, "enabled": True, "timeScale": 1.0}
    free = cube("KSFree", G_S_FREE, (1.0, 6.0, 4.0), (2.0, 0.2, 0.2),
                [0.30, 0.55, 0.85, 1.0], dyn_box())
    bad = cube("KSBad", G_S_BAD, (1.0, 12.0, 4.0), (0.4, 0.4, 0.4),
               [0.70, 0.40, 0.70, 1.0], dyn_box())
    bad["joint"] = joint(1, "")
    bad["script"] = {"path": bad_script, "enabled": True, "timeScale": 1.0}
    ents += [ball, free, bad]
    return {"version": 1, "generator": "verify_joints_script",
            "camera": {"target": [0.0, 6.0, 3.0], "yaw": 2.2, "pitch": 0.3,
                       "distance": 24.0},
            "physics": {"enabled": True, "groundPlane": True},
            "entities": ents}


# ---------------------------------------------------------------- 运行

def _base_env(exe):
    env = dict(os.environ)
    env["MYVK_NO_WINDOW_SAVE"] = "1"
    env["MYVK_EDITOR_CB_NO_DIALOG"] = "1"
    return env


def run_headless(exe, scene, frames, extra=None, timeout=240):
    """跑满 frames 帧自动退出，返回日志行。**不开窗交互** —— 物理断言
    全部来自日志，不需要窗口在屏幕上（但引擎仍然要建窗口，这是 Vulkan
    的既有形态）。"""
    env = _base_env(exe)
    env["MYVK_EDITOR_SCENE"] = scene
    env["MYVK_FRAMES"] = str(frames)
    env["MYVK_LOG_RECTS"] = "0"
    if extra:
        env.update(extra)
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
    time.sleep(0.4)
    with tap.lock:
        return list(tap.lines)


def launch_interactive(exe, scene, extra=None, size="1600x900"):
    """开一个可以点东西的编辑器。返回 (proc, tap, hwnd)。"""
    env = _base_env(exe)
    env["MYVK_LOG_RECTS"] = "1"
    if scene:
        env["MYVK_EDITOR_SCENE"] = scene
    if extra:
        env.update(extra)
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace",
                            bufsize=1)
    tap = LogTap()
    tap.start(proc)
    hwnd = find_window("Venn Editor", 60.0)
    if not hwnd:
        proc.kill()
        return proc, tap, None
    user32.ShowWindow(hwnd, 9)
    time.sleep(2.0)
    tw, th = (int(v) for v in size.lower().split("x"))
    user32.SetWindowPos(hwnd, 0, 0, 0, tw, th, 0x0002 | 0x0004)
    time.sleep(2.0)
    force_foreground(hwnd)
    time.sleep(0.8)
    return proc, tap, hwnd


def scroll_inspector_to(hwnd, tap, tag, tries=14):
    """把 Inspector 里的 `tag` 滚到**真的落在面板可视带里**，返回最新矩形。

    ⚠ 为什么不复用 `verify_character_import.scroll_tag_into_view`（2026-10-27
      实测，一条断言浪费了一轮）：它把鼠标停在
      `(客户区宽-100, 客户区高*0.5)` 去滚。这个落点在本机布局里正好压在
      Inspector 头部「搜索框 / 分隔条」那一条**死区**上 ——
      滚 44 次、换四个 x 坐标，目标矩形一格都没动（始终 y=2144）：

          INS-JOINT-PICK-SELF=(1427,2144)-(1555,2174)   ← y 远超客户区高 844

      它只在 `force_foreground` 失败时返回 None，所以外层看到的是"拿得到
      矩形、点下去毫无反应"—— 前置断言 PASS、功能看着像坏了。
      可靠落点是**面板自身矩形**的中轴、离底边 60px（实测每格 90px）。
    """
    panel = wait_rect(tap, "INS-PANEL", 6.0)
    if not panel:
        return tap.last_rect(tag)
    px = int((panel[0] + panel[2]) * 0.5)
    py = int(panel[3] - 60)
    lo = panel[1] + 24.0
    hi = panel[3] - 24.0
    r = tap.last_rect(tag)
    for _ in range(tries):
        if not r:
            return None
        cy = (r[1] + r[3]) * 0.5
        if lo <= cy <= hi:
            return r
        # wheel_at 正数 = 向下滚（内容上移）
        wheel_at(hwnd, px, py, 3 if cy > hi else -3)
        r = tap.last_rect(tag)
    return r


def shutdown(proc, tap, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)      # WM_CLOSE
        time.sleep(0.8)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    time.sleep(0.3)
    with tap.lock:
        return list(tap.lines)


# ---------------------------------------------------------------- 解析

def _engine_lines(lines):
    """只留引擎自己的日志行，剔掉 `NOTIF-PUSH` 的回声。

    ⚠ 这个坑踩过：引擎把每一条 WARN 同时推成一条通知（`NOTIF-PUSH …
      text="PHYS-JOINT skip …"`），于是同一条 WARN 在日志里出现**两次**。
      不筛的话"三种坏法各报一条"会变成"每种两条"，而且排序后看起来像
      "少了一种" —— 假红的方向还特别像真 bug。
    """
    return [l for l in lines if "NOTIF-PUSH" not in l]


def body_samples(lines, name):
    """[(frame, (x,y,z)), ...]，按日志顺序（= 时间顺序）。"""
    out = []
    for line in _engine_lines(lines):
        m = BODY_RE.search(line)
        if m and m.group(2) == name:
            out.append((int(m.group(1)),
                        (float(m.group(3)), float(m.group(4)),
                         float(m.group(5)))))
    return out


def dist(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def spread(vals):
    return (max(vals) - min(vals)) if vals else 0.0


def no_gpu_errors(lines):
    return not any(("VUID" in l) or l.lstrip().upper().startswith("[ERROR")
                   or "DEVICE_LOST" in l for l in lines)


# ---------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out-dir", default=OUT_DIR)
    ap.add_argument("--keep", action="store_true", help="保留夹具与日志")
    ap.add_argument("--negate", action="store_true",
                    help="否定测试：删掉 Arm 的 joint 块，A 组必须变红")
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("找不到可执行文件，请先构建：", exe)
        return 1
    out_dir = os.path.abspath(args.out_dir)
    os.makedirs(out_dir, exist_ok=True)

    # 同一时刻只允许一个 GUI 用例（引擎的窗口/显卡状态是全局的）
    kill_stale_editors()

    tmpdir = tempfile.mkdtemp(prefix="venn_joints_")
    fails = []

    def check(name, ok, detail=""):
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                               ("  → %s" % detail) if (detail and not ok) else ""))
        if not ok:
            fails.append("%s | %s" % (name, detail))

    try:
        # ============================================================ A
        print("[A] 铰链真的拉住一个动态体（FreeFall 作对照）")
        scene_a = write_scene(os.path.join(tmpdir, "hinge.json"),
                              make_hinge_scene(negate=args.negate))
        lines = run_headless(exe, scene_a, frames=900,
                             extra={"MYVK_EDITOR_PLAY": "1",
                                    "MYVK_PHYS_DUMP": "40"})
        if args.keep:
            with open(os.path.join(out_dir, "joints_A.log"), "w",
                      encoding="utf-8") as f:
                f.write("\n".join(lines))

        check("A0 进入了 Play（runtime scene）",
              any("PLAY in viewport" in l for l in lines), "没有 PLAY 日志")

        built = [BUILT_RE.search(l) for l in _engine_lines(lines)]
        built = [m for m in built if m]
        if args.negate:
            # 否定测试下这里本来就该是 0 行 —— 不算失败，继续看运动学断言
            print("  note: 否定测试模式，PHYS-JOINT built 行数 = %d" % len(built))
        else:
            check("A1 恰好建起一条关节", len(built) == 1, "built=%d" % len(built))
            if built:
                check("A2 类型是 Hinge（kind=2）", built[0].group(3) == "2",
                      "kind=%s" % built[0].group(3))
                check("A3 两端不是同一个刚体",
                      built[0].group(1) != built[0].group(2),
                      "a=%s b=%s" % (built[0].group(1), built[0].group(2)))

        arm = body_samples(lines, "Arm")
        free = body_samples(lines, "FreeFall")
        check("A4 Arm 的轨迹有采样（≥5 个点）", len(arm) >= 5,
              "n=%d" % len(arm))
        check("A5 FreeFall 对照也有采样", len(free) >= 5, "n=%d" % len(free))
        if len(arm) >= 5 and len(free) >= 5:
            d_arm = [dist(p, ANCHOR) for _, p in arm]
            d_free = [dist(p, ANCHOR) for _, p in free]
            # —— 核心判据：到铰点的距离是否被锁住 ——
            check("A6 Arm 到铰点的距离全程 ≈ 1.0 m（连杆被锁住）",
                  max(d_arm) < 1.05 and spread(d_arm) < 0.05,
                  "max=%.4f spread=%.4f" % (max(d_arm), spread(d_arm)))
            # —— 对照：同一个判据在无约束的物体上必须发散 ——
            #
            # ⚠ 这里原本还有一条 `spread(d_free) > 0.8`（要求"看得到下落过程"）。
            #   它是**依赖采样节奏**的，已于 2026-10-27 去掉：自由落体 4 m 只要
            #   0.9 s，而 `MYVK_PHYS_DUMP` 每 40 步才打一行 —— 第一次采样时它
            #   往往已经趴在地上了（实测 f=40 时 z=0.08），spread 只剩 0.009。
            #   换句话说那条子句测的是"dump 间隔够不够密"，不是"关节有没有
            #   生效"。真正的区分力在"Arm 到锚点恒 1.0"与"FreeFall 到同一个锚点
            #   涨到 5.7"的对比上，两者相差 4.6 m，与采样相位无关。
            check("A7 对照 FreeFall 到同一铰点的距离涨到 > 5 m（判据能区分）",
                  max(d_free) > 5.0,
                  "max=%.4f" % max(d_free))

            xs = [p[0] for _, p in arm]
            check("A8 Arm 沿 X 真的摆动了（≥0.8 m 的行程）",
                  spread(xs) > 0.8, "x∈[%.3f,%.3f]" % (min(xs), max(xs)))
            fx = [p[0] for _, p in free]
            check("A9 对照 FreeFall 只做直线下落（X 不动）",
                  spread(fx) < 0.02, "x∈[%.3f,%.3f]" % (min(fx), max(fx)))

            last = arm[-1][1]
            check("A10 Arm 停在铰点正下方 (0,0,3) m 附近",
                  abs(last[0]) < 0.35 and 2.85 < last[2] < 3.25,
                  "last=(%.3f,%.3f,%.3f)" % last)

        check("A11 日志里没有 VUID / ERROR / DEVICE_LOST", no_gpu_errors(lines),
              "有错误行")
        print()

        # ============================================================ B
        print("[B] 四种坏配置全被拦下，关掉的静默不建")
        scene_b = write_scene(os.path.join(tmpdir, "broken.json"),
                              make_broken_scene())
        lines_b = run_headless(exe, scene_b, frames=600,
                               extra={"MYVK_EDITOR_PLAY": "1",
                                      "MYVK_PHYS_DUMP": "20"})
        if args.keep:
            with open(os.path.join(out_dir, "joints_B.log"), "w",
                      encoding="utf-8") as f:
                f.write("\n".join(lines_b))

        built_b = [BUILT_RE.search(l) for l in _engine_lines(lines_b)]
        built_b = [m for m in built_b if m]
        check("B1 一条都没建起来（built 行数为 0）", len(built_b) == 0,
              "built=%d" % len(built_b))

        skips = [SKIP_RE.search(l) for l in _engine_lines(lines_b)]
        skips = [m for m in skips if m]
        reasons = sorted(m.group(2) for m in skips)
        check("B2 三种坏法各报一条 WARN",
              reasons == ["no-rigid-body", "no-target", "target-missing"],
              "reasons=%s" % reasons)
        check("B3 汇总行说 skipped=3",
              any("PHYS-JOINT skipped=3" in l for l in _engine_lines(lines_b)),
              "没有 skipped=3 汇总行")

        # 关掉的那条：既不 built 也不 warn（它是**故意**的，不是错误）。
        # 判据是"三条 skip 而不是四条"——B2 已经锁死了，这里再锁行为面：
        # 它必须照样掉下去（关掉 = 不建 = 不受约束，不是"冻住"）。
        #
        # ⚠ 判据不写"z 变化量 > N"：采样从第 20 帧才开始，那时它已经掉了
        #   一大截，变化量取决于采样相位。写"落到地面"才与相位无关 ——
        #   它从 6 m 起跳，能到 0.5 m 就一定是自由下落。
        off = body_samples(lines_b, "Off")
        check("B4 Off 有轨迹采样", len(off) >= 3, "n=%d" % len(off))
        if off:
            zs = [p[2] for _, p in off]
            check("B5 关掉的关节不会拴住物体（从 6 m 落到地面）",
                  min(zs) < 1.0, "z∈[%.3f,%.3f]" % (min(zs), max(zs)))
        check("B6 日志里没有 VUID / ERROR / DEVICE_LOST",
              no_gpu_errors(lines_b), "有错误行")
        print()

        # ============================================================ C
        print("[C] 存盘往返：Save As 出来的 joint 块逐字段不变")
        scene_c = write_scene(os.path.join(tmpdir, "roundtrip_in.json"),
                              make_hinge_scene())
        rt_out = os.path.join(tmpdir, "roundtrip_out.json")
        proc, tap, hwnd = launch_interactive(
            exe, scene_c, extra={"MYVK_EDITOR_SAVEAS_PATH": rt_out})
        lines_c = []
        if not hwnd:
            check("C0 编辑器窗口起来了", False, "没找到窗口")
        else:
            ok = do_menu(tap, hwnd, "MENU-FILE", "MENU-SAVEAS",
                         expect="SAVEAS abs=")
            check("C1 File ▸ Save As 真的写了文件", ok,
                  "菜单项没反应（SAVEAS 日志没出现）")
            lines_c = shutdown(proc, tap, hwnd)
            if args.keep:
                with open(os.path.join(out_dir, "joints_C.log"), "w",
                          encoding="utf-8") as f:
                    f.write("\n".join(lines_c))
            check("C2 目标文件存在", os.path.exists(rt_out), rt_out)
            if os.path.exists(rt_out):
                src = {e["name"]: e for e in json.load(
                    open(scene_c, encoding="utf-8"))["entities"]}
                dst = {e["name"]: e for e in json.load(
                    open(rt_out, encoding="utf-8"))["entities"]}
                check("C3 存盘后 Arm / Pivot / FreeFall 都还在",
                      {"Arm", "Pivot", "FreeFall"} <= set(dst), str(list(dst)))
                j_src = src["Arm"].get("joint")
                j_dst = dst.get("Arm", {}).get("joint")
                check("C4 Arm 的 joint 块还在", j_dst is not None, "没有 joint")
                if j_dst:
                    check("C5 键集合一致（没有字段被漏写）",
                          set(j_src) == set(j_dst),
                          "少了 %s / 多了 %s" % (
                              sorted(set(j_src) - set(j_dst)),
                              sorted(set(j_dst) - set(j_src))))
                    check("C6 target 的 guid 一字不差（16 位 hex）",
                          j_dst.get("target") == G_PIVOT,
                          "target=%r" % j_dst.get("target"))
                    check("C7 实体 guid 也被保住了",
                          dst["Arm"].get("guid") == G_ARM,
                          "guid=%r" % dst["Arm"].get("guid"))
                    bad = []
                    for k, v in j_src.items():
                        w = j_dst.get(k, "<缺>")
                        if isinstance(v, float):
                            if not isinstance(w, (int, float)) or \
                                    abs(float(w) - v) > 1e-5:
                                bad.append("%s: %r→%r" % (k, v, w))
                        elif v != w:
                            bad.append("%s: %r→%r" % (k, v, w))
                    check("C8 全部 %d 个字段值都一致" % len(j_src), not bad,
                          "; ".join(bad))
        print()

        # ============================================================ D
        print("[D] 界面：Inspector 的 Joint 段 + Physics 面板的关节总览")
        # 起点故意选 **Pivot**（一个没有关节的实体）：先断言"没有关节时显示
        # Add"，再点面板那一行切到 Arm，断言"有关节时显示字段"。两段在同一个
        # 进程里按时间先后出现 → 这个对照不需要第二次启动，也就不会因为
        # 两次启动的环境差异而误判。
        proc, tap, hwnd = launch_interactive(
            exe, scene_c, extra={"MYVK_EDITOR_SELECT": "Pivot",
                                 "MYVK_EDITOR_PANELS": "physics"})
        lines_d = []
        if not hwnd:
            check("D0 编辑器窗口起来了", False, "没找到窗口")
        else:
            # 面板总览：不点任何东西就该有
            r = wait_rect(tap, "PHYS-JOINTS-COUNT", 6.0)
            check("D1 Physics 面板画出了关节总览", r is not None,
                  "PHYS-JOINTS-COUNT 锚点没出现")
            row = wait_rect(tap, "PHYS-JOINT-ROW-0", 6.0)
            check("D2 列出了那一条关节", row is not None,
                  "PHYS-JOINT-ROW-0 没出现")
            with tap.lock:
                d_lines = list(tap.lines)
            m = re.search(r"PHYS-JOINTS authored=(\d+) bad=(\d+) off=(\d+)",
                          "\n".join(d_lines))
            check("D3 统计行说 authored=1 bad=0 off=0",
                  m is not None and m.groups() == ("1", "0", "0"),
                  m.group(0) if m else "没有 PHYS-JOINTS 统计行")
            m2 = re.search(r"PHYS-JOINT-LIST i=0 .*state=(\w+)",
                           "\n".join(d_lines))
            check("D4 这一条被判成 state=ok（目标有效）",
                  m2 is not None and m2.group(1) == "ok",
                  m2.group(0) if m2 else "没有 PHYS-JOINT-LIST 行")

            # 切到 Inspector 的 Physics 页签（默认页签是 All，里面没有 Joint 段）
            nav = wait_rect(tap, "INS-NAV physics", 5.0)
            check("D5 Inspector 有 Physics 页签锚点", nav is not None)
            if nav:
                click(hwnd, (nav[0] + nav[2]) * 0.5, (nav[1] + nav[3]) * 0.5)
                time.sleep(0.8)
            # 此时选中的是 Pivot（没有关节）→ 只该有 Add 按钮
            add = wait_rect(tap, "INS-JOINT-ADD", 5.0)
            check("D6 没有关节的实体显示 Add Joint", add is not None,
                  "INS-JOINT-ADD 锚点没出现")

            # 点面板那一行 → 选中 Arm → Inspector 该换成关节字段
            if row:
                click(hwnd, (row[0] + row[2]) * 0.5, (row[1] + row[3]) * 0.5)
                time.sleep(1.0)
            hdr = wait_rect(tap, "INS-JOINT-HEADER", 5.0)
            check("D7 点面板的行 → Inspector 换成有 joint 的那一段",
                  hdr is not None, "INS-JOINT-HEADER 锚点没出现")
            with tap.lock:
                d_lines2 = list(tap.lines)
            check("D8 点行真的改变了选择（PHYS-JOINTS pick）",
                  any("PHYS-JOINTS pick entity=" in l for l in d_lines2),
                  "没有 pick 日志")
            for tag, label in (("INS-JOINT-KIND", "类型下拉"),
                               ("INS-JOINT-TARGET", "另一端下拉")):
                check("D9 %s 画出来了（%s）" % (label, tag),
                      any(tag + "=" in l for l in d_lines2), "锚点缺失")
            # 铰链才有的限位/马达两段
            for tag, label in (("INS-JOINT-LIMIT", "限位"),
                               ("INS-JOINT-MOTOR", "马达")):
                check("D10 %s 只对铰链/滑动显示（%s）" % (label, tag),
                      any(tag + "=" in l for l in d_lines2), "锚点缺失")
            # 先后顺序即"对照"：Add 在切选择之前就出现过，字段在之后。
            # ⚠ 不能拿 `INS-JOINT-HEADER` 当"有关节"的判据 —— 分区标题是
            #   **无条件**画的（没有关节时它下面写一句"没有关节"+Add 按钮），
            #   于是切换前后都有 HEADER，比出来的先后是反的。`INS-JOINT-KIND`
            #   只有真的存在 JointComponent 才会画，这才是分界点。
            i_add = next((i for i, l in enumerate(d_lines2)
                          if "INS-JOINT-ADD=" in l), -1)
            i_kind = next((i for i, l in enumerate(d_lines2)
                           if "INS-JOINT-KIND=" in l), -1)
            check("D11 Add 出现在关节字段之前（说明是选择切换引起的）",
                  0 <= i_add < i_kind, "i_add=%d i_kind=%d" % (i_add, i_kind))
            check("D12 日志里没有 VUID / ERROR / DEVICE_LOST",
                  no_gpu_errors(d_lines2), "有错误行")
            lines_d = shutdown(proc, tap, hwnd)
            if args.keep:
                with open(os.path.join(out_dir, "joints_D.log"), "w",
                          encoding="utf-8") as f:
                    f.write("\n".join(lines_d))
        print()

        # ============================================================ E
        print("[E] 四种类型的运动学判据（每一种都配同几何的无约束对照）")
        scene_e = write_scene(os.path.join(tmpdir, "kinematic.json"),
                              make_kinematic_scene())
        lines_e = run_headless(exe, scene_e, frames=900,
                               extra={"MYVK_EDITOR_PLAY": "1",
                                      "MYVK_PHYS_DUMP": "20"})
        if args.keep:
            with open(os.path.join(out_dir, "joints_E.log"), "w",
                      encoding="utf-8") as f:
                f.write("\n".join(lines_e))

        built_e = [BUILT_RE.search(l) for l in _engine_lines(lines_e)]
        built_e = [m for m in built_e if m]
        check("E0 四条关节都建起来了（Ball/Fixed/Slider/Distance）",
              len(built_e) == 4, "built=%d" % len(built_e))
        kinds = sorted(m.group(3) for m in built_e)
        check("E1 四种 kind 各一条（0=Fixed 1=Ball 3=Slider 4=Distance）",
              kinds == ["0", "1", "3", "4"], "kinds=%s" % kinds)

        # ---- Ball：锚点锁死 + 朝向自由 ----
        #
        # "位置锁死"这一半：锚点在体心外 1 m，所以质心到铰点的距离必须恒为
        # 1.0（= 连杆没被拉长）。
        # "朝向自由"这一半：日志里只有 pos/vel，看不到四元数。但 Ball 允许
        # 绕铰点转 —— 重力绕铰点产生力矩 → 它会从"横着"摆到"垂着"，质心从
        # (1,0,4) 走到 (0,0,3)。**只能由转动产生这个位移**，于是末态位置就是
        # "转动自由度活着"的可观测替身。
        kball = body_samples(lines_e, "KBall")
        check("E2 KBall 有轨迹采样（≥5 个点）", len(kball) >= 5,
              "n=%d" % len(kball))
        if len(kball) >= 5:
            dball = [dist(p, K_BALL_REF) for _, p in kball]
            check("E3 Ball 的锚点被锁死（质心到铰点恒为 1.0 m）",
                  max(dball) < 1.05 and spread(dball) < 0.06,
                  "max=%.4f spread=%.4f" % (max(dball), spread(dball)))
            last = kball[-1][1]
            check("E4 Ball 转了 90° 摆到铰点正下方（朝向自由）",
                  abs(last[0]) < 0.35 and 2.85 < last[2] < 3.25,
                  "last=(%.3f,%.3f,%.3f)" % last)

        # ---- Fixed 对照：同一几何、同一锚点，只有 kind 不同 ----
        # 它必须**一动不动**。这一条是 E4 的对照：两者位置判据（E3）完全
        # 相同，差别只在"能不能转"—— 所以 E4 + E5 才构成 Ball 与 Fixed 的
        # 完整分界。少了 E5，"Ball 生效了"和"Ball 退化成 Fixed 了"分不开。
        kfixed = body_samples(lines_e, "KFixed")
        check("E5 KFixed 对照有轨迹采样", len(kfixed) >= 5, "n=%d" % len(kfixed))
        if len(kfixed) >= 5:
            off = max(dist(p, (1.0, 8.0, 4.0)) for _, p in kfixed)
            check("E6 Fixed 对照焊在原地（朝向锁死 → 摆不下来）",
                  off < 0.15, "max=%.4f" % off)
            dfixed = [dist(p, K_FIXED_REF) for _, p in kfixed]
            check("E7 Fixed 的连杆长度也不变（与 E3 同一条判据）",
                  max(dfixed) < 1.05 and spread(dfixed) < 0.06,
                  "max=%.4f spread=%.4f" % (max(dfixed), spread(dfixed)))

        # ---- Slider：只沿轴动 ----
        #
        # ⚠ 探针必须是**横向初速度**：竖直下落本来就不改 x，用"x 不动"当
        #   判据的话，没有关节的物体也一样过 —— 那是一条空转断言。夹具给了
        #   2 m/s 的 +X 初速度，于是"有约束 vs 无约束"才真的不一样。
        kslide = body_samples(lines_e, "KSlide")
        check("E8 KSlide 有轨迹采样", len(kslide) >= 5, "n=%d" % len(kslide))
        if len(kslide) >= 5:
            xs = [p[0] for _, p in kslide]
            zs = [p[2] for _, p in kslide]
            check("E9 Slider 横向被锁住（2 m/s 的 +X 初速度被吃掉，x 不动）",
                  spread(xs) < 0.06 and max(abs(x) for x in xs) < 0.06,
                  "x∈[%.4f,%.4f]" % (min(xs), max(xs)))
            check("E10 Slider 轴向自由（照样落到地面）", min(zs) < 1.0,
                  "z∈[%.3f,%.3f]" % (min(zs), max(zs)))
        ksf = body_samples(lines_e, "KSlideFree")
        check("E11 KSlideFree 对照有轨迹采样", len(ksf) >= 5, "n=%d" % len(ksf))
        if len(ksf) >= 5:
            # ⚠ 判据用**绝对位移**（起点 x=4.0 是夹具定的），不用 spread：
            #   这块受地面摩擦，2 m/s 只够滑 1.9 m 左右就停下；一旦第一次采样
            #   落得晚一点，spread 就从 1.39 变成 0.99 —— 实测两次跑出来过
            #   两种值。跟采样相位无关的写法才是"位移"，即 `末态 x − 起点 x`。
            xs = [p[0] for _, p in ksf]
            moved = max(xs) - 4.0
            check("E12 对照沿 X 跑掉了（同一个初速度，无约束 → 位移 > 1 m）",
                  moved > 1.0, "x∈[%.3f,%.3f] moved=%.3f" % (min(xs), max(xs),
                                                            moved))

        # ---- Distance：两点间距恒定、切向自由 ----
        kdist = body_samples(lines_e, "KDist")
        check("E13 KDist 有轨迹采样", len(kdist) >= 5, "n=%d" % len(kdist))
        if len(kdist) >= 5:
            dd = [dist(p, K_DIST_REF) for _, p in kdist]
            check("E14 Distance 的绳长恒为 2.0 m",
                  max(dd) < 2.05 and spread(dd) < 0.06,
                  "max=%.4f spread=%.4f" % (max(dd), spread(dd)))
            last = kdist[-1][1]
            # 从 (2,24,6) 摆到 (0,24,4) —— 原地不动说明切向被锁死（= 退化成
            # Fixed）。这一条是 Distance 与 Fixed 的分界。
            #
            # ⚠ 阈值给到 0.7 而不是 0.35：这条绳子的阻尼来自线速度阻尼，
            #   不像铰链那样有强角阻尼，900 帧后还在 ±0.4 m 的小幅摆动
            #   （实测末态 x=-0.377）。判据要的是"从 x=2 摆到了 x≈0"，
            #   不是"收敛到毫米级" —— 把阈值卡在 ±0.35 会随相位偶发变红。
            check("E15 Distance 切向自由（摆到锚点正下方 2 m）",
                  abs(last[0]) < 0.7 and 3.6 < last[2] < 4.4,
                  "last=(%.3f,%.3f,%.3f)" % last)
        kdf = body_samples(lines_e, "KDistFree")
        check("E16 KDistFree 对照有轨迹采样", len(kdf) >= 5, "n=%d" % len(kdf))
        if len(kdf) >= 5:
            ddf = [dist(p, K_DISTFREE_REF) for _, p in kdf]
            check("E17 对照的'绳长'不恒定（2 m → 5 m 以上，判据能区分）",
                  max(ddf) > 4.5 and spread(ddf) > 1.5,
                  "max=%.3f spread=%.3f" % (max(ddf), spread(ddf)))

        check("E18 日志里没有 VUID / ERROR / DEVICE_LOST", no_gpu_errors(lines_e),
              "有错误行")
        print()

        # ============================================================ F
        print("[F] 脚本 API：joint / jointAnchors / jointKind / jointTarget")
        script = os.path.join(tmpdir, "joint_api.vks")
        bad_script = os.path.join(tmpdir, "joint_bad.vks")
        for path, body in ((script, S_SCRIPT), (bad_script, S_BAD_SCRIPT)):
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(body)
        scene_f = write_scene(os.path.join(tmpdir, "script.json"),
                              make_script_scene(
                                  script.replace("\\", "\\\\"),
                                  bad_script.replace("\\", "\\\\")))
        lines_f = run_headless(exe, scene_f, frames=600,
                               extra={"MYVK_EDITOR_PLAY": "1",
                                      "MYVK_PHYS_DUMP": "20",
                                      "MYVK_LOG_SCRIPT_PRINT": "1"})
        if args.keep:
            with open(os.path.join(out_dir, "joints_F.log"), "w",
                      encoding="utf-8") as f:
                f.write("\n".join(lines_f))

        joined_f = "\n".join(_engine_lines(lines_f))
        check("F0 脚本真的跑起来了（有 SCRIPT-JOINT 打印）",
              "SCRIPT-JOINT before" in joined_f, "没有脚本输出")

        def script_value(tag):
            m = re.search(r"SCRIPT-JOINT %s (\S+)" % tag, joined_f)
            return m.group(1) if m else None

        check("F1 配之前组件里是 Fixed（夹具初值，说明 F3 的变化不可能是巧合）",
              script_value("before") == "Fixed", script_value("before"))
        check("F2 joint(ball, pivot, 'ball') 返回 true",
              script_value("set") == "true", script_value("set"))
        check("F3 jointKind() 变成 Ball（脚本真的写进了组件）",
              script_value("kind") == "Ball", script_value("kind"))
        check("F4 jointTarget() 指到 KSBallPivot",
              (script_value("target") or "").startswith("KSBallPivot"),
              script_value("target"))
        check("F5 jointAnchors() 也通了（vec3 形式）",
              script_value("anchors") == "true", script_value("anchors"))
        # ⚠ 这一条锁的是**签名**，不只是行为：`jointAnchors` / `jointAxis` 一度
        #   在内建表里声明成 7 个 / 4 个参数（六个标量拆开写），于是按 vec3 写的
        #   脚本**编译期**就报 "expects 7 argument(s), got 3"，整份脚本 not
        #   runnable —— 一行运行日志都不会有，看着像"脚本根本没跑"。
        #   内建表的 arity 就是公开签名，必须与实现一起改。
        check("F5b jointAxis() 也通了（vec3 形式）",
              script_value("axis") == "true", script_value("axis"))

        m = re.search(r"EDITOR-JOINT set self=(\d+) other=(\d+) kind=Ball "
                      r"built=(\d+)", joined_f)
        check("F6 编辑器侧执行了 set，且当场重建了关节（built=1）",
              m is not None and m.group(3) == "1",
              m.group(0) if m else "没有 EDITOR-JOINT set 行")
        check("F7 求解器侧收到了这条关节（PHYS-JOINT built kind=1）",
              any(x.group(3) == "1" for x in
                  [BUILT_RE.search(l) for l in _engine_lines(lines_f)]
                  if x is not None),
              "没有 kind=1 的 PHYS-JOINT built 行")
        check("F8 没有 'not available in this host'（宿主真的把动作接上了）",
              "not available in this host" not in joined_f,
              "脚本报了这个宿主不支持关节")

        # 失败路径：拼错的类型名必须**报错**，不能"认不出就当 Hinge"
        #
        # ⚠ 判据不能写"print 出 joint() == false"：`joint()` 失败时会把错误挂到
        #   **那条语句**上，于是语句自带的 `print` 根本不会执行（实测日志里
        #   一条 SCRIPT-JOINT bad 都没有）。可观测的是错误本身 + 实例状态 ——
        #   这也正是设计意图：配错关节是**程序错误**，不该被当成返回值糊过去。
        check("F9 拼错的类型名让那个脚本实例进入 Failed（错误没被吞掉）",
              re.search(r"SCR-STATE entity=\d+ name=KSBad state=Failed",
                        joined_f) is not None,
              "KSBad 没有进入 Failed 状态")
        check("F10 引擎侧为它打了一条 WARN（不是静默失败）",
              "EDITOR-JOINT set: unknown kind 'bogus'" in joined_f,
              "没有 unknown kind 的 WARN")
        check("F11 脚本侧拿到了带原因的运行时错误",
              "check the kind name" in joined_f,
              "错误信息里没有提示怎么改")

        # 关节真的约束住了？—— 同一个物理判据，证明改的是**求解器**而不只是组件
        ksb = body_samples(lines_f, "KSBall")
        ksfree = body_samples(lines_f, "KSFree")
        check("F12 KSBall / KSFree 都有轨迹采样",
              len(ksb) >= 3 and len(ksfree) >= 3,
              "n=%d/%d" % (len(ksb), len(ksfree)))
        if len(ksb) >= 3 and len(ksfree) >= 3:
            # ⚠ 判据只看**末态**：脚本是在 Play 跑起来之后才配关节的，在那之前
            #   KSBall 已经自由落了一小段；球窝把它拽回球面之后还会摆几下。
            #   所以"全程恒 1.0"这种判据在这里不成立（它只对夹具一开头就配好
            #   的 E 组成立），能说明问题的是"最后被稳稳拉住在球面上"。
            d_last = dist(ksb[-1][1], K_BALL_REF)
            check("F13 脚本配出来的 Ball 真的拉住了（末态到铰点 ≈1.0 m）",
                  0.90 < d_last < 1.10, "last=%.4f" % d_last)
            zs = min(p[2] for _, p in ksfree)
            check("F14 对照 KSFree 落到了地面（说明物理真的在跑）", zs < 1.0,
                  "min z=%.3f" % zs)
        check("F15 日志里没有 VUID / ERROR / DEVICE_LOST", no_gpu_errors(lines_f),
              "有错误行")
        print()

        # ============================================================ G
        print("[G] 视口里点两下拾取锚点（真鼠标 + 独立复算换算）")
        #
        # 组目标是那条**最容易写错又最难看错**的换算：命中点是世界坐标，写进
        # `anchorSelf` 之前必须过**本实体**世界矩阵的逆，写进 `anchorTarget`
        # 之前必须过**target 实体**的逆。写反了（或忘了逆）的症状是"锚点点上去
        # 位置看着对，一按 Play 物体被拽到十万八千里"。
        #
        # 判据不是"点了有日志"，而是：从日志里取出 `world` 与 `local`，用夹具里
        # **已知**的变换（Arm：pos=(1,0,4) scale=(2,0.2,0.2)；Pivot：pos=(0,0,20)
        # 无缩放）独立算一遍 `world = M * local`，两者必须在 1e-3 内一致。
        # 这条也是"两种实现会不一样的那个量"：只做平移不做缩放的实现、或者
        # 拿错实体的矩阵的实现，都会在这里红。
        #
        # ⚠ 不设 `MYVK_EDITOR_PANELS`：把中间格子换成别的面板之后，视口从第 3
        #   帧起就不再 `draw()`，`VP-RECT` 变成**陈旧值** —— 按它算坐标点下去
        #   永远 `hovered=0`，会看起来像"拾取功能坏了"（这条坑排查了很久）。
        proc, tap, hwnd = launch_interactive(
            exe, scene_a, extra={"MYVK_EDITOR_SELECT": "Arm"})
        lines_g = []
        if not hwnd:
            check("G0 编辑器窗口起来了", False, "没找到窗口")
        else:
            nav = wait_rect(tap, "INS-NAV physics", 8.0)
            check("G1 Inspector 有 Physics 页签锚点", nav is not None)
            if nav:
                click(hwnd, (nav[0] + nav[2]) * 0.5, (nav[1] + nav[3]) * 0.5)
                time.sleep(0.9)

            # ⚠ `logRect` 对**被裁掉**的控件照样打日志（它只去重、不判可见性），
            #   而 Joint 段长在 Physics 页的末尾 —— 实测锚点在 y=2144，比客户区
            #   高出一千多像素。直接按矩形中心点下去 = 点在客户区外，什么都不会
            #   发生（而且前置断言"按钮存在"还会 PASS）。
            #   所以：**先滚，再确认它真的落在面板可视带里**，才允许点。
            vp0 = wait_rect(tap, "VP-RECT", 8.0)
            scr = scroll_inspector_to(hwnd, tap, "INS-JOINT-PICK-SELF")
            panel = wait_rect(tap, "INS-PANEL", 6.0)
            vis = (scr is not None and panel is not None and
                   panel[1] + 24.0 <= (scr[1] + scr[3]) * 0.5 <= panel[3] - 24.0)
            check("G2 滚动后 'Pick in viewport' 真的落在可视区里",
                  vis and vp0 is not None,
                  "rect=%s panel=%s" % (scr, panel))
            ok = vis and click_tag(hwnd, tap, "INS-JOINT-PICK-SELF")
            time.sleep(0.7)
            with tap.lock:
                g1 = list(tap.lines)
            check("G3 点按钮进入拾取态（JOINT-PICK begin=self）",
                  ok and any("JOINT-PICK begin=self entity=" in l for l in g1),
                  "按钮点了但没进拾取态")

            vp = wait_rect(tap, "VP-RECT", 8.0)
            check("G4 视口画出了矩形（VP-RECT）", vp is not None)
            if vp:
                w = vp[2] - vp[0]
                h = vp[3] - vp[1]
                # 两下点在不同的位置：第一下取 A 端，第二下取 B 端。
                click(hwnd, vp[0] + w * 0.35, vp[1] + h * 0.40)
                time.sleep(0.9)
                click(hwnd, vp[0] + w * 0.62, vp[1] + h * 0.58)
                time.sleep(0.9)
            lines_g = shutdown(proc, tap, hwnd)
            if args.keep:
                with open(os.path.join(out_dir, "joints_G.log"), "w",
                          encoding="utf-8") as f:
                    f.write("\n".join(lines_g))

            picks = [PICK_RE.search(l) for l in _engine_lines(lines_g)]
            picks = [m for m in picks if m]
            ends = {m.group(1): m for m in picks}
            check("G5 两下点击各写了一个锚点（self + target）",
                  {"self", "target"} <= set(ends), "picks=%s" % sorted(ends))

            # —— 独立复算：world 必须等于「实体变换 × local」——
            ARM_T = (1.0, 0.0, 4.0)
            ARM_S = (2.0, 0.2, 0.2)
            PIVOT_T = (0.0, 0.0, 20.0)

            def recompute(m, pos, scale):
                world = [float(m.group(i)) for i in (5, 6, 7)]
                local = [float(m.group(i)) for i in (8, 9, 10)]
                want = [pos[i] + scale[i] * local[i] for i in range(3)]
                return max(abs(world[i] - want[i]) for i in range(3))

            if "self" in ends:
                m = ends["self"]
                check("G6 self 端写的是**本实体**局部坐标（world = pos + scale·local）",
                      recompute(m, ARM_T, ARM_S) < 1e-3 and m.group(2) == m.group(3),
                      "空间对不上（space=%s entity=%s）" % (m.group(3),
                                                          m.group(2)))
            if "target" in ends:
                m = ends["target"]
                check("G7 target 端写的是**target 实体**局部坐标（本端缩放不参与）",
                      recompute(m, PIVOT_T, (1.0, 1.0, 1.0)) < 1e-3,
                      "空间对不上")
            check("G8 点空也要能用（source 是 surface 或 camera-plane）",
                  all(m.group(4) in ("surface", "camera-plane") for m in picks)
                  and len(picks) >= 2,
                  "sources=%s" % [m.group(4) for m in picks])
            check("G9 点完 A 端自动接着点 B 端（next=target → next=none）",
                  any("JOINT-PICK next=target" in l for l in lines_g) and
                  any("JOINT-PICK next=none" in l for l in lines_g),
                  "advance 日志不完整")
            check("G10 日志里没有 VUID / ERROR / DEVICE_LOST",
                  no_gpu_errors(lines_g), "有错误行")

        # ---- Esc 只退拾取、不清选中 ----
        # 单独起一个进程：上一步已经把锚点写脏了，而这一条要的是"进拾取态 →
        # Esc → 选中还在"。混在一起会让两种状态互相干扰。
        proc, tap, hwnd = launch_interactive(
            exe, scene_e, extra={"MYVK_EDITOR_SELECT": "KBall"})
        lines_g2 = []
        if not hwnd:
            check("G11 第二个窗口起来了", False, "没找到窗口")
        else:
            nav = wait_rect(tap, "INS-NAV physics", 8.0)
            if nav:
                click(hwnd, (nav[0] + nav[2]) * 0.5, (nav[1] + nav[3]) * 0.5)
                time.sleep(0.9)
            vp1 = wait_rect(tap, "VP-RECT", 8.0)
            s2 = scroll_inspector_to(hwnd, tap, "INS-JOINT-PICK-SELF")
            panel2 = wait_rect(tap, "INS-PANEL", 6.0)
            inband = (s2 is not None and panel2 is not None and
                      panel2[1] + 24.0 <= (s2[1] + s2[3]) * 0.5
                      <= panel2[3] - 24.0)
            if inband:
                click_tag(hwnd, tap, "INS-JOINT-PICK-SELF")
                time.sleep(0.6)
            with tap.lock:
                g_mid = list(tap.lines)
            # ⚠ 前置：没进拾取态的话，G12/G13 就是两条空转的断言
            #   （"没有 esc 取消日志"会被当成实现坏了，其实是没进去）。
            check("G11 第二个窗口真的进了拾取态（否则 G12/G13 空转）",
                  inband and any("JOINT-PICK begin=self entity=" in l
                                 for l in g_mid),
                  "inband=%s vp=%s" % (inband, vp1))
            esc(hwnd)
            time.sleep(0.6)
            # ⚠ `logRect` 是**按矩形去重**的：不滚这一下，cancel 之后
            #   INS-JOINT-KIND 不会重新打，"选中还在"与"只是没再打印"
            #   就分不开了（上一版 G13 正是这么红的）。
            if panel2:
                wheel_at(hwnd, int((panel2[0] + panel2[2]) * 0.5),
                         int(panel2[3] - 60), -2)
                time.sleep(0.6)
            with tap.lock:
                g2 = list(tap.lines)
            lines_g2 = shutdown(proc, tap, hwnd)
            # shutdown 返回的是**全量**（含 g2 那一段），别把它俩相加 ——
            # 相加会让同一行出现两次，`i_kind > i_cancel` 这种下标比较就乱了。
            allg2 = lines_g2 or g2
            i_cancel = next((i for i, l in enumerate(allg2)
                             if "JOINT-PICK cancel reason=esc" in l), -1)
            check("G12 Esc 退出拾取（cancel reason=esc）", i_cancel >= 0,
                  "没有 esc 取消日志")
            # 选中若被顺手清掉，Inspector 就会换成 Scene/Camera，关节字段消失。
            # 判据 = "cancel 之后仍在画 INS-JOINT-KIND"。
            i_kind = next((i for i, l in enumerate(allg2) if i > i_cancel and
                           "INS-JOINT-KIND=" in l), -1)
            check("G13 Esc 没有顺手清选中（Inspector 还停在关节字段上）",
                  i_cancel >= 0 and i_kind > i_cancel,
                  "i_cancel=%d i_kind=%d" % (i_cancel, i_kind))
            check("G14 日志里没有 VUID / ERROR / DEVICE_LOST",
                  no_gpu_errors(allg2), "有错误行")
        print()

        # ============================================================ H
        print("[H] 3D 调试绘制（竖直连杆 + 默认关）")
        #
        # 这一组盯的是 `collectJointGizmos` 里那条最容易漏的分支：线段 →
        # 拉长立方体时要挑一个**与线段方向不平行**的参考轴。竖直连杆
        # （摆锤的绳、吊钩的缆）方向就是世界 Z，参考轴仍取 Z 的话叉乘是
        # 零向量 → 连杆塌成零尺寸、整条"绳子"消失，而且**不报错**。
        # 夹具的锚点差是纯 +Z，几何体条数因此是判据：4 = 两球 + 连杆 + 轴，
        # 3 = 连杆没生成。
        scene_h = write_scene(os.path.join(tmpdir, "vertical.json"),
                              make_vertical_joint_scene())
        proc, tap, hwnd = launch_interactive(
            exe, scene_h, extra={"MYVK_JOINT_GIZMOS": "1"})
        lines_h = []
        if not hwnd:
            check("H1 窗口起来了", False, "没找到窗口")
        else:
            time.sleep(1.5)
            lines_h = shutdown(proc, tap, hwnd)
            if args.keep:
                with open(os.path.join(out_dir, "joints_H.log"), "w",
                          encoding="utf-8") as f:
                    f.write("\n".join(lines_h))

            gz = [l for l in _engine_lines(lines_h) if "RENDER joint-gizmos" in l]
            check("H1 打开可视化后真的算出了几何（RENDER joint-gizmos）",
                  len(gz) >= 1, "没有 joint-gizmos 行")
            last = gz[-1] if gz else ""
            check("H2 一条关节、两端都配好了（n=1 dangling=0 missing-body=0）",
                  "n=1 " in last and "dangling=0" in last and
                  "missing-body=0" in last, last)
            # ⚠ 必须是**等于 4**而不是"≥3"：漏了参考轴换向只剩 3，
            #   大于 4 说明多画了（比如把轴也画给了 Ball，那种"看起来更多"
            #   同样是错的）。
            check("H3 竖直连杆也在（两球 + 连杆 + 轴 = 4 个几何体）",
                  last.endswith("verts=4"), last)
            check("H4 日志里没有 VUID / ERROR / DEVICE_LOST",
                  no_gpu_errors(lines_h), "有错误行")
            # 反向对照：G 组那次**没**设 MYVK_JOINT_GIZMOS，同一个场景
            # （hinge.json，同样有一条关节）必须一行都没有。这条守的是
            # "默认开" —— 默认开的话，用户每帧都在为一堆球付片元。
            neg = [l for l in _engine_lines(lines_g) if
                   "RENDER joint-gizmos" in l]
            check("H5 默认关（不设 MYVK_JOINT_GIZMOS 时一行都不打）",
                  bool(lines_g) and not neg,
                  "G 组日志为空" if not lines_g
                  else "默认就画了：%s" % (neg[:1] or ""))
        print()

        # ========================================================== 收尾
        if fails:
            print("结果: FAIL: %d 条" % len(fails))
            for f in fails:
                print("  -", f)
            tail = (lines_d or lines_b or lines)[-40:]
            print("\n最近 40 行日志：")
            for line in tail:
                print("   ", line)
            return 4
        print("结果: PASS")
        return 0
    finally:
        kill_stale_editors()
        if not args.keep:
            try:
                for f in os.listdir(tmpdir):
                    safe_remove(os.path.join(tmpdir, f))
                os.rmdir(tmpdir)
            except OSError:
                pass
        else:
            print("夹具保留在：", tmpdir)


if __name__ == "__main__":
    sys.exit(main())
