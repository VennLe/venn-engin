"""把 Alicia Solid（VRM，二次元少女）导入引擎，并烘出 4 段基础动画。

用法：
    blender --background --factory-startup --python tools/blender_import_alicia.py -- \
        --vrm  <AliciaSolid.vrm> \
        --out  assets/models/characters/alicia.fbx
    build/bin/AssetC.exe assets/models/characters/alicia.fbx \
        -o assets/models/characters        # → alicia.vmesh

为什么不用现成的 animate_girl 脚本：
  · 那个资产是**无权重**的静态网格，最花时间的是逐壳绑骨；Alicia 自带
    99.8% 顶点的蒙皮权重，这一整段可以跳过。
  · 那个资产的 65 骨 Mixamo 骨架是"借"来的，两边 rest 朝向差异巨大。

⚠ **不能做 Mixamo retarget**（本脚本第一版的做法，已放弃）
  Alicia 是 VRM，Blender 的 glTF 导入器重建骨轴时用启发式：`LeftArm` 的
  tail 被编到**头顶正上方**（head z=1.271 / tail z=1.480），而真正的手在
  x=-0.497 处、水平方向 0.43 m 外。`bone.matrix_local` 的旋转与网格 bind
  的朝向彻底脱节 —— 于是任何"把源骨的世界旋转搬过来"的做法（Copy
  Rotation WORLD、rest-delta 换算、约束 + nla.bake）都会绕错轴：自检
  `err=0.00000`、画面却是"双手举过头顶"或"身体扭成一团"。

  → 改成**程序化关键帧**：所有旋转都用**世界轴**显式写出（`RX/RY/RZ`），
    方向靠实测标定（`LeftArm` 绕世界 Y 转 +75° 是举手，所以下垂是 −72°；
    腿沿 −Z，迈步是 RX(±25°)）。每根骨给一个世界 delta，再按 FK 递推成
    pose basis。这条换算路径的每个轴向都在 `blender_probe_alicia_axes.py` 里
    用"顶点实际位移"验证过。

产物：`alicia.fbx`（12 个网格 + 128 骨 + Idle/Walk/Run/Jump 四段），
      交给 AssetC 烤成 `.vmesh`（`joints=128 clips=4 skinVerts=100%`）。
详见 `docs/alicia_import.md`。
"""

import bpy
import math
import os
import sys

from mathutils import Matrix, Vector

# ------------------------------------------------------------ 参数

VRM = "E:/code/cpp/_venn_tmp/dl/AliciaSolid.vrm"
OUT = "assets/models/characters/alicia.fbx"

if "--" in sys.argv:
    argv = sys.argv[sys.argv.index("--") + 1:]
    for k, attr in (("--vrm", "VRM"), ("--out", "OUT")):
        if k in argv:
            globals()[attr] = argv[argv.index(k) + 1]

FPS = 30                # 关键帧采样率。clip 的实际长度写死在 CLIP_DEFS 里，
                        # 与帧率解耦（30 fps 只是让 2.0 s 的 Idle 有 60 帧）。

# 会被程序化姿态驱动的骨（价值在**名字**：pose_* 函数按这些名字查骨）。
# 表里左列是 Mixamo 的写法（历史的），右列是 Alicia/VRM 里的实际骨名；
# 除 Spine2 之外两边就是"去掉 mixamorig: 前缀"。
# Spine 链：Mixamo 三节 (Spine/Spine1/Spine2) 对 VRM 的 spine/chest/upperChest，
# 而 Alicia 的四节里 Spine2 在 VRM 语义中未使用，所以 Spine2 → Spine3。
BONE_MAP = {
    "Hips": "Hips",
    "Spine": "Spine",
    "Spine1": "Spine1",
    "Spine2": "Spine3",
    "Neck": "Neck",
    "Head": "Head",
    "LeftShoulder": "LeftShoulder",
    "LeftArm": "LeftArm",
    "LeftForeArm": "LeftForeArm",
    "LeftHand": "LeftHand",
    "RightShoulder": "RightShoulder",
    "RightArm": "RightArm",
    "RightForeArm": "RightForeArm",
    "RightHand": "RightHand",
    "LeftUpLeg": "LeftUpLeg",
    "LeftLeg": "LeftLeg",
    "LeftFoot": "LeftFoot",
    "LeftToeBase": "LeftToeBase",
    "RightUpLeg": "RightUpLeg",
    "RightLeg": "RightLeg",
    "RightFoot": "RightFoot",
    "RightToeBase": "RightToeBase",
}
# 手指：两边同名，直接补全
for side in ("Left", "Right"):
    for f in ("Thumb", "Index", "Middle", "Ring", "Pinky"):
        for i in (1, 2, 3):
            n = "%sHand%s%d" % (side, f, i)
            BONE_MAP[n] = n


def log(msg):
    print("ALICIA %s" % msg)


def rot3(m):
    """取 3x3 旋转（去掉缩放/平移）。"""
    return m.to_3x3().normalized()


def select_only(objs, active=None):
    for o in bpy.data.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = active or (objs[0] if objs else None)


def topo_order(arm):
    """父在子前。"""
    out, seen = [], set()

    def walk(b):
        if b.name in seen:
            return
        if b.parent is not None:
            walk(b.parent)
        seen.add(b.name)
        out.append(b.name)

    for b in arm.data.bones:
        walk(b)
    return out


# ------------------------------------------------------------ 1. 目标骨架

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc.render.fps = FPS

log("import %s" % VRM)
bpy.ops.import_scene.gltf(filepath=VRM)

arm = [o for o in bpy.data.objects if o.type == "ARMATURE"][0]
arm.name = "ALICIA"

# VRM 里带一个不蒙皮的辅助网格（2x2 的地面/包围盒），会把包围盒算错，删掉
junk = [o for o in bpy.data.objects if o.type == "MESH"
        and not any(m.type == "ARMATURE" for m in o.modifiers)]
for o in junk:
    log("DROP unskinned mesh %s (%d verts)" % (o.name, len(o.data.vertices)))
    bpy.data.objects.remove(o, do_unlink=True)
meshes = [o for o in bpy.data.objects if o.type == "MESH"]
log("meshes=%d bones=%d" % (len(meshes), len(arm.data.bones)))

# ------------------------------------------------------------ 2. rest 基准

arm_mw_r = rot3(arm.matrix_world)
order = topo_order(arm)

dst_rest_local = {}     # armature 空间 rest **旋转**（3×3，喂姿态函数用）
rest4 = {}              # armature 空间 rest **完整矩阵**（4×4，含骨头位置）
for b in arm.data.bones:
    dst_rest_local[b.name] = rot3(b.matrix_local)
    rest4[b.name] = b.matrix_local.copy()

# 目标骨 → 父骨（用于自顶向下推 basis）
parent_of = {b.name: (b.parent.name if b.parent else None)
             for b in arm.data.bones}

hips_rest_world_pos = (arm.matrix_world @ arm.data.bones["Hips"].head_local)
log("hips rest world = (%.3f, %.3f, %.3f)"
    % (hips_rest_world_pos.x, hips_rest_world_pos.y, hips_rest_world_pos.z))

I3 = Matrix.Identity(3)

if arm.animation_data is None:
    arm.animation_data_create()
arm.data.pose_position = "POSE"

# 目标骨全部先归零（rest），没被 mapped 到的骨保持静止
for n in order:
    pb = arm.pose.bones[n]
    pb.rotation_mode = "QUATERNION"
    pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    pb.location = (0.0, 0.0, 0.0)

# ------------------------------------------------------------ 姿态定义
#
# ⚠ 为什么不用 Mixamo retarget
#   实测（tools/blender_probe_alicia_axes.py）：Alicia 是 VRM，Blender 的 glTF
#   导入器重建骨轴时用启发式，LeftArm 的 tail 被编到**头顶正上方**
#   （head z=1.271 / tail z=1.480，而真正的手在 x=-0.497 处水平 0.43 m
#   外）。bone.matrix_local 的旋转与网格绑定的朝向彻底脱节，任何"从源骨
#   搬世界旋转"的做法都会让手臂绕错轴 —— 这就是前面 rest-delta 自检
#   err=0、姿势却把手举过头顶的原因。
#
#   所以这里改成**程序化关键帧**：所有旋转都用**世界轴**显式写出，方向由
#   实测标定：
#       · 左臂沿 -X、右臂沿 +X（T-pose），下垂 = RY(∓72°)
#       · 腿沿 -Z，迈步 = RX(±25°)，正值向前(+Y)
#       · 手臂前后摆 = RX(swing) @ RY(drop)（先下垂再摆）
#   每根骨给一个世界 delta，再按 FK 递推成 basis —— 这条换算路径在
#   tools/blender_probe_alicia_axes.py 里验证过（worldY75 的手臂确实绕对轴）。

from mathutils import Matrix, Quaternion


def RX(d):
    return Matrix.Rotation(math.radians(d), 3, "X")


def RY(d):
    return Matrix.Rotation(math.radians(d), 3, "Y")


def RZ(d):
    return Matrix.Rotation(math.radians(d), 3, "Z")


I3 = Matrix.Identity(3)


def pose_idle(t):
    """2.0 s 循环：呼吸 + 极轻的重心浮动。"""
    p = 2.0 * math.pi * t
    br = math.sin(p)
    return {
        "Hips": (RZ(0), 0.006 * br),
        "Spine": (RX(1.2 + 0.8 * br), 0.0),
        "Spine1": (RX(0.8 + 0.5 * br), 0.0),
        "Spine3": (RX(0.5), 0.0),
        "Neck": (RX(-0.8), 0.0),
        "Head": (RX(-1.5 - 0.6 * br), 0.0),
        "LeftShoulder": (RY(-2.0), 0.0),
        "RightShoulder": (RY(2.0), 0.0),
        "LeftArm": (RY(-70.0 + 1.2 * br), 0.0),
        "RightArm": (RY(70.0 - 1.2 * br), 0.0),
        "LeftForeArm": (RY(-10.0), 0.0),
        "RightForeArm": (RY(10.0), 0.0),
        "LeftHand": (RY(-3.0), 0.0),
        "RightHand": (RY(3.0), 0.0),
        "LeftUpLeg": (RX(0.0), 0.0),
        "RightUpLeg": (RX(0.0), 0.0),
    }


def pose_walk(t):
    """1.0 s 循环：对侧摆臂 + 双膝先后弯。"""
    p = 2.0 * math.pi * t
    sw = math.sin(p)
    return {
        # 重心：每步下沉一次（2 倍频），并左右轻摆
        "Hips": (RZ(3.0 * sw) @ RX(1.5), -0.012 + 0.010 * abs(math.sin(2 * p))),
        "Spine": (RZ(-2.0 * sw) @ RX(2.0), 0.0),
        "Spine1": (RX(1.5), 0.0),
        "Spine3": (RZ(-1.5 * sw), 0.0),
        "Neck": (RX(-1.5), 0.0),
        "Head": (RZ(1.5 * sw) @ RX(-1.0), 0.0),
        "LeftShoulder": (RY(-2.0), 0.0),
        "RightShoulder": (RY(2.0), 0.0),
        # 手臂：下垂 70°，再绕世界 X 反相摆 22°
        "LeftArm": (RX(-22.0 * sw) @ RY(-70.0), 0.0),
        "RightArm": (RX(22.0 * sw) @ RY(70.0), 0.0),
        "LeftForeArm": (RX(-10.0 - 8.0 * max(0.0, -sw)) @ RY(-10.0), 0.0),
        "RightForeArm": (RX(-10.0 - 8.0 * max(0.0, sw)) @ RY(10.0), 0.0),
        # 腿：前后 25°；膝盖只向后弯（负值）
        "LeftUpLeg": (RX(25.0 * sw), 0.0),
        "RightUpLeg": (RX(-25.0 * sw), 0.0),
        "LeftLeg": (RX(-32.0 * max(0.0, math.sin(p + 2.4))), 0.0),
        "RightLeg": (RX(-32.0 * max(0.0, math.sin(p + 2.4 + math.pi))), 0.0),
        "LeftFoot": (RX(12.0 * max(0.0, -sw)), 0.0),
        "RightFoot": (RX(12.0 * max(0.0, sw)), 0.0),
    }


def pose_run(t):
    """0.6 s 循环：大摆幅 + 前倾 + 屈肘。"""
    p = 2.0 * math.pi * t
    sw = math.sin(p)
    return {
        "Hips": (RZ(5.0 * sw) @ RX(4.0), -0.02 + 0.045 * abs(math.sin(2 * p))),
        "Spine": (RZ(-3.5 * sw) @ RX(9.0), 0.0),
        "Spine1": (RX(6.0), 0.0),
        "Spine3": (RZ(-2.0 * sw) @ RX(3.0), 0.0),
        "Neck": (RX(-6.0), 0.0),
        "Head": (RX(-6.0), 0.0),
        "LeftShoulder": (RY(-4.0), 0.0),
        "RightShoulder": (RY(4.0), 0.0),
        # 跑步：肘弯 85°，上臂前后 42°
        "LeftArm": (RX(-42.0 * sw) @ RY(-58.0), 0.0),
        "RightArm": (RX(42.0 * sw) @ RY(58.0), 0.0),
        "LeftForeArm": (RX(-85.0) @ RY(-8.0), 0.0),
        "RightForeArm": (RX(-85.0) @ RY(8.0), 0.0),
        "LeftUpLeg": (RX(42.0 * sw), 0.0),
        "RightUpLeg": (RX(-42.0 * sw), 0.0),
        "LeftLeg": (RX(-75.0 * max(0.0, math.sin(p + 2.0))), 0.0),
        "RightLeg": (RX(-75.0 * max(0.0, math.sin(p + 2.0 + math.pi))), 0.0),
        "LeftFoot": (RX(18.0 * max(0.0, -sw)), 0.0),
        "RightFoot": (RX(18.0 * max(0.0, sw)), 0.0),
    }


def pose_jump(t):
    """1.0 s 单次：蹲 → 起跳 → 空中收腿 → 落地缓冲。"""
    # 分段插值：0-0.25 蹲 / 0.25-0.45 起 / 0.45-0.72 空中 / 0.72-1 落地
    if t < 0.25:
        u = t / 0.25
        crouch = u * u
        dz = -0.16 * crouch
        thigh, knee = 42.0 * crouch, 68.0 * crouch
        lift = 0.0
    elif t < 0.45:
        u = (t - 0.25) / 0.20
        crouch = 1.0 - u
        dz = -0.16 * crouch + 0.34 * u
        thigh, knee = 42.0 * crouch * (1 - u), 68.0 * crouch * (1 - u)
        lift = 1.0
    elif t < 0.72:
        u = (t - 0.45) / 0.27
        dz = 0.34 - 0.10 * (u * u)          # 上抛后缓落
        thigh, knee = 38.0 * math.sin(u * math.pi), 60.0 * math.sin(u * math.pi)
        lift = 0.0
    else:
        u = (t - 0.72) / 0.28
        dz = 0.24 * (1.0 - u) - 0.10 * math.sin(u * math.pi)
        thigh, knee = 30.0 * (1.0 - u), 55.0 * (1.0 - u)
        lift = 0.0
    return {
        "Hips": (RX(6.0 * max(0.0, 1.0 - 2.0 * abs(t - 0.125))), dz),
        "Spine": (RX(9.0 if t < 0.45 else 3.0), 0.0),
        "Spine1": (RX(5.0), 0.0),
        "Neck": (RX(-4.0), 0.0),
        "Head": (RX(-4.0), 0.0),
        # 起跳/空中手臂上摆
        "LeftArm": (RX(-55.0 * lift) @ RY(-70.0 + 30.0 * lift), 0.0),
        "RightArm": (RX(-55.0 * lift) @ RY(70.0 - 30.0 * lift), 0.0),
        "LeftForeArm": (RX(-25.0 * lift) @ RY(-10.0), 0.0),
        "RightForeArm": (RX(-25.0 * lift) @ RY(10.0), 0.0),
        "LeftUpLeg": (RX(thigh), 0.0),
        "RightUpLeg": (RX(thigh * 0.85), 0.0),
        "LeftLeg": (RX(-knee), 0.0),
        "RightLeg": (RX(-knee * 0.9), 0.0),
        "LeftFoot": (RX(18.0 * lift), 0.0),
        "RightFoot": (RX(18.0 * lift), 0.0),
    }


CLIP_DEFS = [
    ("Idle", 2.0, True, pose_idle),
    ("Walk", 1.0, True, pose_walk),
    ("Run", 0.6, True, pose_run),
    ("Jump", 1.0, False, pose_jump),
]


def build_clip(name, dur, loop, fn):
    """把一个 pose 函数烤成 action。

    换算链（**这是第二版，第一版是错的**，见文件头的警告）：

        pose_world[n] = pose_world[父] · (L[父]⁻¹ · L[n]) · basis[n]
        ⇒ basis[n]   = (L[父]⁻¹ L[n])⁻¹ · pose_world[父]⁻¹ · pose_world[n]
        ⇒ basis[根]  = L[根]⁻¹ · pose_world[根]

    其中 `L[n] = bone.matrix_local`（**armature 空间的 4×4**，含平移）。

    `pose_world[n] = M[n] · L[n]`，而 `M[n]` 是"这块骨（连同整个子树）被搬动
    了多少"的刚体变换 —— 这才是蒙皮真正看到的东西（`W_t · W_rest⁻¹`）。

        M[n] = M[父] · (T(h) · R · T(h)⁻¹)      h = L[n] 的平移（骨头位置）

    也就是"先继承父骨的搬动，再绕**自己的头**转 R（世界轴）"。R 直接来自
    pose 函数，不需要任何 rest 朝向的对齐 —— 这是绕过"VRM 骨轴是启发式重建
    的、`matrix_local` 与网格 bind 脱节"那个坑的唯一干净办法。

    ⚠ 第一版写成 `A[n] = (A[父]·R_rest[父]⁻¹) · D · R_rest[n]`，把 rest **local**
      当世界矩阵用了（对非根骨两者差着整条父链）。结果 `basis` 恰好等于
      rest 旋转 —— 自检 `err=0`、引擎里播出来却是静态 T-pose。判据：烘焙后
      `LeftHand` 的头必须从 T-pose 的高度掉到胯部附近。
    """
    nframes = max(2, int(round(dur * FPS)))
    total = nframes + (1 if loop else 0)      # 循环 clip 补最后一帧 = 第一帧

    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    if arm.animation_data is None:
        arm.animation_data_create()
    arm.animation_data.action = act
    try:
        if hasattr(arm.animation_data, "action_slot") and act.slots:
            arm.animation_data.action_slot = act.slots[0]
    except Exception:                          # noqa: BLE001
        pass

    for f in range(total + 1):
        t = (f / float(nframes)) % 1.0 if loop else min(1.0, f / float(nframes))
        pose = fn(t)

        # ---- 1) 逐骨刚体搬动 M（世界轴旋转，绕自己的头）----
        M = {}
        for n in order:
            p = parent_of[n]
            d = pose.get(n)
            R = (d[0] if d else I3).to_4x4()
            h = rest4[n].to_translation()        # 骨头的 rest 位置（armature 空间）
            about = (Matrix.Translation(h) @ R @ Matrix.Translation(-h))
            M[n] = (M[p] @ about) if p else about
            if n == "Hips":
                # 位移写在 pose 的第二项（米，世界 Z），整体抬/沉
                dz = d[1] if d else 0.0
                M[n] = Matrix.Translation((0.0, 0.0, dz)) @ M[n]

        # ---- 2) 目标世界矩阵 → Blender 的 basis ----
        for n in order:
            p = parent_of[n]
            L4 = rest4[n]                        # 4×4 rest
            pw = M[n] @ L4                       # pose_world，armature 空间
            if p:
                rel = rest4[p].inverted() @ L4
                ppw = M[p] @ rest4[p]
                basis = rel.inverted() @ ppw.inverted() @ pw
            else:
                basis = L4.inverted() @ pw
            pb = arm.pose.bones[n]
            pb.matrix_basis = basis
            pb.keyframe_insert("location", frame=f)
            pb.keyframe_insert("rotation_quaternion", frame=f)

    # ---- 3) 自检：手到底在不在胯部（第一版就是这里没查才漏过去的）----
    if name == "Idle":
        arm.animation_data.action = act
        sc.frame_set(1)
        bpy.context.view_layer.update()
        hand = arm.pose.bones["LeftHand"]
        Lh = rest4["LeftHand"]
        got = hand.matrix @ Lh.inverted()      # 实测的 W_t·W_rest⁻¹
        want = M["LeftHand"]
        err = max(abs(got[r][c] - want[r][c])
                  for r in range(4) for c in range(4))
        # 位置判据（可证伪）：T-pose 时 LeftHand 的头在 z≈1.27，垂下去 ≈0.85
        log("SELF %s hand head=(%.3f, %.3f, %.3f) M_err=%.5f"
            % (name, hand.head.x, hand.head.y, hand.head.z, err))
        if hand.head.z > 1.10:
            log("SELF-FAIL 手还在肩膀高度（z=%.3f）—— 姿态没生效"
                % hand.head.z)
        if err > 1.0e-3:
            log("SELF-FAIL basis 反算不准（err=%.5f）" % err)

    for fc in act.fcurves:
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"
    log("CLIP %-5s frames=%d dur=%.2fs loop=%d fcurves=%d"
        % (name, total, dur, 1 if loop else 0, len(act.fcurves)))
    return (0, total)


made = []
for name, dur, loop, fn in CLIP_DEFS:
    rng = build_clip(name, dur, loop, fn)
    made.append((name, rng, loop))

# ------------------------------------------------------------ 3. 导出 FBX

arm.animation_data.action = None
for t in list(arm.animation_data.nla_tracks):
    arm.animation_data.nla_tracks.remove(t)
# 动画用 `bake_anim_use_all_actions`，**不用 NLA strips**。这不是口味问题，
# 是唯一能同时满足下面两条的组合（四种都实测过，只有最后一种同时成立）：
#
#   ① 节点基础变换必须是 rest。FBX 的"节点基础变换"= 导出那一刻**评估出来
#      的 pose**。不复位就导 → 基础变换带的是最后一个 clip 的末帧姿态，而
#      AssetC 正是拿它当 `restLocal` / `invBind` 的来源 → 静止姿势 == 那个
#      clip 的一帧 → 播它时 `W_t·W_rest⁻¹ ≡ I`，画面是**一动不动的 T-pose**，
#      日志却全绿（播放头在走、clip 名对、关节数对）。
#   ② 曲线必须是真的。导出器是按"当前姿势"采样基础变换的，所以只要把姿势
#      复位成 rest，任何"跟着当前姿势烤"的模式都会烤出四段静止轨。
#
#   · NLA strips + 复位            → 四段 clip 全静止（AssetC keys 0）
#   · NLA strips + `use_nla=False` → 同上
#   · NLA strips 静音 / 挪到帧 100+ → 导出器直接跳过，一个 take 都不出
#   · **all-actions + 复位**       → ✅ rest 干净 + 曲线完整（keys 3684）
#     代价：take 名变成 `ALICIA|Idle`。载入器 FbxLoader 会把 `|` 前缀剥掉
#     （Unity / UE / glTF 转换器的通行做法），引擎侧仍然是裸名。
arm.animation_data.action = None
for t in list(arm.animation_data.nla_tracks):
    arm.animation_data.nla_tracks.remove(t)
for name, _rng, _loop in made:
    bpy.data.actions[name].use_fake_user = True

for pb in arm.pose.bones:
    pb.matrix_basis = Matrix.Identity(4)
sc.frame_set(1)
bpy.context.view_layer.update()
_mh = arm.pose.bones["LeftHand"]
log("POSE-RESET LeftHand head=(%.3f, %.3f, %.3f) 应为 (−0.497, −0.008, 1.264)"
    % (_mh.head.x, _mh.head.y, _mh.head.z))

# ------------------------------------------------------------ 材质压平
#
# ⚠ 为什么必须做这一步：VRM 的材质是 **MToon**，glTF 导入器把它重建成
#   `Emission + Light Path + Transparent BSDF + Mix Shader` 一堆节点，**没有
#   Principled BSDF**。FBX 导出器只认 Principled 的 Base Color 链路 ——
#   MToon 它认不出来，于是导出时**一张贴图都不带**：
#   FBX 里搜不到任何 `.png` 字符串，`AssetC --info` 十二个材质全是
#   `albedo=-`，角色在引擎里渲染成**纯白石膏像**。
#   （这里被坑过一次：以为贴图没落盘，其实落了，是引用根本没进 FBX。）
def find_image(nt, depth=0):
    """在节点树里找第一张图（含嵌套 node group）。"""
    for node in nt.nodes:
        if node.type == "TEX_IMAGE" and node.image:
            return node.image
    if depth < 3:
        for node in nt.nodes:
            if node.type == "GROUP" and node.node_tree:
                img = find_image(node.node_tree, depth + 1)
                if img:
                    return img
    return None


_flat = 0
for mat in bpy.data.materials:
    if not mat.use_nodes or not mat.node_tree:
        continue
    nt = mat.node_tree
    img = find_image(nt)
    # 兜底：按名字最长前缀匹配（`Alicia_body_wear` → `Alicia_body`）
    if img is None:
        cand = [i for i in bpy.data.images if mat.name.startswith(i.name)]
        if cand:
            img = max(cand, key=lambda i: len(i.name))
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    out.location = (320, 0)
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.location = (0, 0)
    bsdf.inputs["Roughness"].default_value = 0.7
    bsdf.inputs["Metallic"].default_value = 0.0
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    if img is not None:
        img.colorspace_settings.name = "sRGB"
        t = nt.nodes.new("ShaderNodeTexImage")
        t.location = (-360, 0)
        t.image = img
        nt.links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
        # 睫毛 / 发丝 / 制服镂空靠贴图 alpha 抠形状，不接就变实心块
        nt.links.new(t.outputs["Alpha"], bsdf.inputs["Alpha"])
        if any(k in mat.name for k in ("trans", "mastuge", "eye")):
            # Blender 4.2+ 删了 `shadow_method`、`blend_method` 也改了语义，
            # 这里只做"尽力而为"，拿不到就跳过 —— 导出器读的是 Principled
            # 的 Alpha 链路，属性名不匹配不影响 FBX。
            for attr, val in (("blend_method", "CLIP"),
                              ("shadow_method", "CLIP"),
                              ("surface_render_method", "DITHERED")):
                if hasattr(mat, attr):
                    try:
                        setattr(mat, attr, val)
                    except Exception:      # noqa: BLE001
                        pass
        _flat += 1

log("FLATTEN materials=%d/%d 带贴图（MToon → Principled）"
    % (_flat, len([m for m in bpy.data.materials if m.use_nodes])))

# ------------------------------------------------------------ 关于"抠形"（已放弃）
#
# VRM 的脸 / 睫毛 / 发丝是"卡片 + 贴图 alpha 抠形状"，理论上应该在离线阶段
# 把透明面删掉 —— 因为 `assetio::SourceMaterial` **不带 alphaMode**
# （`.vmesh` 里没有这个字段），导入的模型一律 Opaque，半透明层会渲染成实心。
#
# 实测两次，都不对，结论是**这条路走不通**：
#   ① 对所有"贴图里带透明"的材质一律抠 → 身体网格的**头部整块被抠掉**，
#      角色变成一个没有头的洞。原因：`Alicia_body` 贴图在头部那块本来就
#      是透明的（原设计里那张脸由 face 层盖住），抠完就露底。
#   ② 只抠名字含 face/mastuge/hair_trans 的叠层 → 得到"白脸 + 一个红嘴巴"。
#      原因：`Alicia_face.png` 有 **97% 像素完全不透明**，它就是脸本身
#      （肤色底 + 很淡的五官）；真正的眼睛在 `Alicia_eye` / `Alicia_eye_white`
#      的独立网格上，而那两块网格**在脸皮的里面**，靠 MToon 的描画顺序盖
#      上去 —— Opaque 之后被脸皮整片挡住，抠不抠都看不见。
#
# 所以正确的修法不是删面，是给 `.vmesh` 加 **alphaMode / 描画顺序**（要
# bump 版本 + 动载入器 + 改着色器变体，回归面很大，不在"换默认角色"这一刀
# 的范围里）。已知遗留已记在 docs/alicia_import.md §6。
# 贴图落盘：VRM 的图是内嵌的（packed），FBX 的 COPY 模式需要一个真文件。
# 落在**系统临时目录**，不进资产树 —— 资产树里只留 FBX 的 COPY 目录
# `<name>.fbm/`（那才是可移植的相对路径），两套图并存只会让人分不清
# 哪套才是真正被引用的。
import tempfile
import shutil

texdir = tempfile.mkdtemp(prefix="alicia_tex_")
for img in bpy.data.images:
    if img.name == "Render Result" or img.size[0] == 0:
        continue
    dst = os.path.join(texdir, "%s.png" % img.name.replace("/", "_"))
    try:
        img.filepath_raw = dst
        img.file_format = "PNG"
        img.save()
    except Exception as ex:
        log("TEX skip %s (%s)" % (img.name, ex))
log("textures -> %s（导出后删除）" % texdir)

sc.frame_set(1)
select_only(meshes + [arm], arm)
bpy.ops.export_scene.fbx(
    filepath=bpy.path.abspath(OUT),
    use_selection=True,
    object_types={"MESH", "ARMATURE"},
    apply_scale_options="FBX_SCALE_ALL",
    axis_forward="-Z",
    axis_up="Y",
    use_mesh_modifiers=False,        # 蒙皮修改器不能被烘掉
    mesh_smooth_type="FACE",
    use_tspace=False,
    add_leaf_bones=False,
    bake_anim=True,
    bake_anim_use_all_actions=True,   # take 名 = "<armature>|<action>"（载入器剥前缀）
    bake_anim_use_nla_strips=False,
    bake_anim_force_startend_keying=False,
    bake_anim_step=1.0,
    bake_anim_simplify_factor=0.0,
    path_mode="COPY",
    embed_textures=False,            # 内嵌的话 ufbx 只给 blob，AssetC 只认磁盘文件
)

size = os.path.getsize(bpy.path.abspath(OUT)) if os.path.exists(bpy.path.abspath(OUT)) else 0
log("WROTE %s (%.2f MB) clips=%s"
    % (OUT, size / 1048576.0, ",".join(m[0] for m in made)))
shutil.rmtree(texdir, ignore_errors=True)
log("DONE")
