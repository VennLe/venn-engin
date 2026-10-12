"""Blender 4.5 —— 程序化生成一个二次元风格女孩角色（单体网格 + 骨架 + walk）。

为什么是"程序化建模"而不是拼图元：
    用户明确要求"必须是一个完整的整体，不能是简单的几个几何体拼凑"。
    所以躯干/头/颈是**一条连续的放样曲面**（沿 Y 轴一圈圈截面缝出来的），
    四肢也是沿路径的渐细放样管，头发是带"露脸开口"的壳 —— 视觉上是一个
    连续的角色，而不是圆柱+球拼起来的积木。

朝向与单位（对齐引擎，见 MEMORY.md「模型导入 = 根节点 + 子网格」）：
    Blender Y-up → glTF Y-up → 引擎在节点树根部预乘"绕 X +90°"，
    即 (x,y,z) → (x,-z,y)。所以在 Blender 里：
      · 脚底在 y = 0        → 引擎里 z = 0（刚好着地）
      · 角色面朝 **+X**     → 引擎里仍面朝 +X（引擎的转身就是绕 Z 转）
      · 身高 1.55m（≈6.5 头身，典型二次元比例）

动画：两个 action
    Walk —— 24 帧（24fps = 1 秒）循环走位；帧 25 与帧 1 同姿态（无缝）
    Idle —— 站立微摆（呼吸 + 重心偏移），停下时播这个

用法（后台跑，不开窗口）：
    "D:/blender/blender.exe" --background --factory-startup \
        --python tools/blender_make_girl.py -- --out assets/models/anime_girl.glb
"""

import math
import os
import sys

import bmesh  # noqa: F401  （保留：法线重算用得到）
import bpy
from mathutils import Matrix, Quaternion, Vector

# ------------------------------------------------------------ 参数

OUT = "assets/models/anime_girl.glb"
if "--" in sys.argv:
    argv = sys.argv[sys.argv.index("--") + 1:]
    if "--out" in argv:
        OUT = argv[argv.index("--out") + 1]

FPS = 24
WALK_FRAMES = 24
SEGS = 16                      # 放样截面的边数
SUBSURF = 1                    # 细分级别（让放样面看起来是" sculpt" 过的）

MAT_SKIN, MAT_HAIR, MAT_CLOTH, MAT_EYE, MAT_IRIS, MAT_SHOE, MAT_LASH = range(7)

# 放样环表： (y, 半宽 rx, 半深 rz, 截面"方"度) —— 越大越接近圆角矩形
BODY_RINGS = [
    (0.855, 0.132, 0.108, 2.6),   # 胯
    (0.905, 0.128, 0.104, 2.7),
    (0.955, 0.118, 0.099, 2.8),   # 腰
    (1.005, 0.110, 0.094, 2.8),
    (1.055, 0.119, 0.099, 2.7),   # 胸下
    (1.110, 0.128, 0.104, 2.6),   # 胸
    (1.160, 0.136, 0.102, 2.5),
    (1.205, 0.140, 0.096, 2.4),   # 肩
    (1.243, 0.120, 0.086, 2.4),
    (1.262, 0.062, 0.062, 2.2),   # 颈根
    (1.290, 0.052, 0.054, 2.1),   # 颈
    (1.318, 0.056, 0.060, 2.1),
    (1.340, 0.074, 0.080, 2.2),   # 下巴
    (1.372, 0.099, 0.104, 2.3),
    (1.408, 0.112, 0.118, 2.4),   # 脸颊（二次元大头）
    (1.448, 0.112, 0.119, 2.4),
    (1.482, 0.100, 0.107, 2.3),   # 额头
    (1.516, 0.072, 0.078, 2.2),
    (1.540, 0.036, 0.040, 2.1),
    (1.556, 0.008, 0.009, 2.0),   # 头顶
]

# 四肢：路径点 (x, y, z) + 半径（沿路径线性插值半径）。
# ⚠ **必须是三维路径**：早期版本这里只写 (x, y)，z 被丢成 0 —— 于是两条手臂
#   和两条腿全都长在了"前后"方向而不是"左右"，正面看是一条窄片、侧面看成一团。
#   （数值画像里一眼看穿：躯干段 x 方向 ±0.23 比 z 方向 ±0.10 还宽。）
#   z 就是"左右"：角色面朝 +X，上 +Y，左 -Z / 右 +Z。
ARM_PATH = [(0.010, 1.205, 0.115), (0.032, 1.075, 0.142),   # 肩→肘
            (0.046, 0.945, 0.152), (0.056, 0.820, 0.157),
            (0.061, 0.735, 0.158), (0.063, 0.680, 0.158)]   # 腕→手
ARM_RADII = [0.052, 0.045, 0.037, 0.033, 0.027, 0.021]

LEG_PATH = [(0.000, 0.870, 0.062), (0.006, 0.700, 0.066),
            (0.008, 0.520, 0.070), (0.004, 0.330, 0.072),
            (0.000, 0.150, 0.073), (0.000, 0.075, 0.073)]   # 髋→膝→踝
LEG_RADII = [0.090, 0.076, 0.059, 0.052, 0.041, 0.033]

# ------------------------------------------------------------ 网格累积器

VERTS = []
FACES = []
FACE_MAT = []


def add_ring(center, rx, rz, segs, power=2.0, z_offset=0.0, x_offset=0.0):
    """生成一圈顶点（截面是超椭圆：power 越大越"方"）。"""
    cx, cy, cz = center
    base = len(VERTS)
    for i in range(segs):
        t = 2.0 * math.pi * i / segs
        c, s = math.cos(t), math.sin(t)
        # 超椭圆：|x/rx|^p + |z/rz|^p = 1
        k = 2.0 / power
        ux = math.copysign(abs(c) ** k, c)
        uz = math.copysign(abs(s) ** k, s)
        VERTS.append(Vector((cx + ux * rx + x_offset,
                             cy,
                             cz + uz * rz + z_offset)))
    return base


def stitch(ring_a, ring_b, mat, skip=None):
    """缝合相邻两圈（四边形）。skip(theta_index) 返回 True 表示这一格不建面。"""
    segs = SEGS
    for i in range(segs):
        if skip is not None and skip(i):
            continue
        j = (i + 1) % segs
        FACES.append((ring_a + i, ring_a + j, ring_b + j, ring_b + i))
        FACE_MAT.append(mat)


def cap(ring, mat, flip=False):
    """封口（三角扇）。"""
    c = len(VERTS)
    # 用环心的平均值当封口中心
    cx = sum((VERTS[ring + i] for i in range(SEGS)), Vector((0, 0, 0))) / SEGS
    VERTS.append(cx)
    for i in range(SEGS):
        j = (i + 1) % SEGS
        if flip:
            FACES.append((c, ring + j, ring + i))
        else:
            FACES.append((c, ring + i, ring + j))
        FACE_MAT.append(mat)


def loft(rings_spec, mat, cap_bottom=True, cap_top=True, face_window=None):
    """沿一串 (y, rx, rz, power) 放样出一条闭合管道。

    face_window=(y_low, y_high, theta_lo, theta_hi)：这段高度区间里朝向
    theta 的那一块**不建面** —— 头发用它掏出"露脸"的开口。
    """
    prev = None
    for (y, rx, rz, power) in rings_spec:
        r = add_ring((0.0, y, 0.0), rx, rz, SEGS, power)
        if prev is None:
            if cap_bottom:
                cap(r, mat, flip=True)
        else:
            py = rings_spec[[x[0] for x in rings_spec].index(y) - 1][0]
            skip = None
            if face_window:
                y_lo, y_hi, th_lo, th_hi = face_window
                if y_lo <= y <= y_hi:
                    def skip(i, th_lo=th_lo, th_hi=th_hi):
                        if i >= th_lo and i <= th_hi:
                            return True
                        return False
            stitch(prev, r, mat, skip)
        prev = r
    if cap_top and prev is not None:
        cap(prev, mat)


def loft_path(path3, radii, mat, power=2.0, caps=True):
    """沿三维路径 (x, y, z) 放样一条管（四肢用）。"""
    rings = []
    for p, r in zip(path3, radii):
        rings.append(add_ring(tuple(p), r, r * 0.92, SEGS, power))
    for a, b in zip(rings[:-1], rings[1:]):
        stitch(a, b, mat)
    if caps:
        cap(rings[0], mat, flip=True)
        cap(rings[-1], mat)


def add_uv_sphere(center, radius, mat, squash=(1.0, 1.0, 1.0), segs=12, rings=8):
    """加一个椭球（眼睛/高光用）。"""
    start = len(VERTS)
    for r in range(rings + 1):
        phi = math.pi * r / rings
        for s in range(segs):
            th = 2.0 * math.pi * s / segs
            x = math.sin(phi) * math.cos(th) * squash[0]
            y = math.cos(phi) * squash[1]
            z = math.sin(phi) * math.sin(th) * squash[2]
            VERTS.append(Vector((center[0] + x * radius,
                                 center[1] + y * radius,
                                 center[2] + z * radius)))
    for r in range(rings):
        for s in range(segs):
            s2 = (s + 1) % segs
            FACES.append((start + r * segs + s, start + r * segs + s2,
                          start + (r + 1) * segs + s2, start + (r + 1) * segs + s))
            FACE_MAT.append(mat)


# ------------------------------------------------------------ 造型

def build_body():
    loft(BODY_RINGS, MAT_SKIN, cap_bottom=True, cap_top=True)


def build_limbs():
    for sgn in (1.0, -1.0):
        # 手臂：沿三维路径镜像（z 取左右）
        path = [(x, y, sgn * z) for (x, y, z) in ARM_PATH]
        loft_path(path, ARM_RADII, MAT_SKIN)
        hx, hy, hz = path[-1]
        add_uv_sphere((hx, hy - 0.030, hz), 1.0, MAT_SKIN,
                      squash=(0.026, 0.046, 0.020), segs=10, rings=6)

        lpath = [(x, y, sgn * z) for (x, y, z) in LEG_PATH]
        loft_path(lpath, LEG_RADII, MAT_SKIN)
        # 鞋：脚踝往前伸出的楔形（二次元小皮鞋），脚底严格 y=0
        fx, fy, fz = lpath[-1]
        add_uv_sphere((fx + 0.030, 0.036, fz), 1.0, MAT_SHOE,
                      squash=(0.042, 0.036, 0.040), segs=10, rings=6)


def build_hair():
    """头发 = 贴着头骨的一顶壳（只在**脸的高度**掏出开口）+ 刘海 + 侧发绺 + 双马尾。"""
    # 1) 头发壳：比头骨大 9mm，只覆盖 y ∈ [1.312, 1.556]
    #    脸的高度段（1.335~1.525）把正前方 ±67° 留空；头顶与后脑照常包住。
    #    ⚠ 开口必须**按高度限定**：早期版本不限定，整条缝从头顶一路开到
    #    后颈，看起来就是个开了槽的头盔。
    face_lo, face_hi = 14, 2          # 环向索引 14,15,0,1,2（绕一圈回到正面）
    prev = None
    for (y, rx, rz, power) in BODY_RINGS:
        if y < 1.312:
            continue
        r = add_ring((0.0, y, 0.0), rx + 0.009, rz + 0.010, SEGS, power)
        if prev is not None:
            opening = 1.335 <= y <= 1.525
            stitch(prev, r, MAT_HAIR,
                   lambda i, o=opening: o and (i >= face_lo or i <= face_hi))
        prev = r
    cap(prev, MAT_HAIR)

    # 2) 刘海：额前一排短绺（每绺是一根渐细的小管，z 往左右散开）
    for k in range(5):
        t = (k - 2) / 2.0                      # -1..1
        z = t * 0.072
        x0 = 0.100 - abs(t) * 0.016
        path = [(x0, 1.492 - abs(t) * 0.010, z),
                (x0 + 0.022, 1.458, z * 1.08),
                (x0 + 0.030, 1.418 - abs(t) * 0.008, z * 1.10)]
        loft_path(path, [0.026, 0.022, 0.010], MAT_HAIR, power=1.6)

    # 3) 侧发绺：沿脸颊两侧下垂（在"左右"方向 z 上偏移）
    for sgn in (1.0, -1.0):
        path = [(0.082, 1.505, sgn * 0.092), (0.096, 1.432, sgn * 0.104),
                (0.102, 1.362, sgn * 0.108), (0.100, 1.300, sgn * 0.104)]
        loft_path(path, [0.030, 0.028, 0.024, 0.016], MAT_HAIR, power=1.7)

    # 4) 双马尾：从后脑两侧甩出去，带一点弧度（x 为负 = 脑后）
    for sgn in (1.0, -1.0):
        path = [(-0.035, 1.472, sgn * 0.070), (-0.082, 1.448, sgn * 0.112),
                (-0.112, 1.372, sgn * 0.142), (-0.124, 1.252, sgn * 0.150),
                (-0.117, 1.132, sgn * 0.140), (-0.104, 1.062, sgn * 0.124)]
        loft_path(path, [0.044, 0.050, 0.046, 0.039, 0.028, 0.012],
                  MAT_HAIR, power=1.8)


def build_face():
    """二次元的灵魂：大眼睛 + 睫毛。

    眼球要**探出脸面**才有存在感：头骨在眼睛高度的 rx≈0.112，眼球中心
    若放在 0.098 就整个埋进脸里（早期版本就是这样，渲出来只剩两个小点）。
    """
    for sgn in (1.0, -1.0):
        ex, ey, ez = 0.099, 1.424, sgn * 0.049
        # 眼白（略微外凸的椭球）
        add_uv_sphere((ex, ey, ez), 1.0, MAT_EYE, squash=(0.026, 0.042, 0.034),
                      segs=14, rings=10)
        # 虹膜 + 瞳孔（往脸面方向再推一点）
        add_uv_sphere((ex + 0.017, ey, ez), 1.0, MAT_IRIS,
                      squash=(0.013, 0.026, 0.023), segs=14, rings=10)
        add_uv_sphere((ex + 0.026, ey, ez), 1.0, MAT_IRIS,
                      squash=(0.006, 0.012, 0.011), segs=10, rings=6)
        # 高光：二次元眼睛的关键一点
        add_uv_sphere((ex + 0.024, ey + 0.015, ez - sgn * 0.010), 1.0,
                      MAT_EYE, squash=(0.006, 0.010, 0.010), segs=8, rings=5)
        # 睫毛：眼睛上方一条压扁的深色椭球，二次元感的一半在这儿
        add_uv_sphere((ex + 0.012, ey + 0.040, ez), 1.0, MAT_LASH,
                      squash=(0.016, 0.011, 0.040), segs=12, rings=6)


def build_cloth():
    """连衣裙：从腰往下张开的裙摆（罩在胯/大腿外面）。"""
    skirt = [
        (1.010, 0.112, 0.098, 2.8),
        (0.975, 0.116, 0.102, 2.8),
        (0.930, 0.132, 0.116, 2.7),
        (0.880, 0.156, 0.138, 2.6),
        (0.830, 0.180, 0.158, 2.5),
        (0.792, 0.196, 0.172, 2.4),
        (0.775, 0.198, 0.174, 2.4),
    ]
    prev = None
    for (y, rx, rz, power) in skirt:
        r = add_ring((0.0, y, 0.0), rx, rz, SEGS, power)
        if prev is not None:
            stitch(prev, r, MAT_CLOTH)
        prev = r
    cap(prev, MAT_CLOTH)


def build_character():
    build_body()
    build_cloth()
    build_limbs()
    build_hair()
    build_face()
    me = bpy.data.meshes.new("AnimeGirl")
    me.from_pydata([tuple(v) for v in VERTS], [], FACES)
    me.update()
    ob = bpy.data.objects.new("AnimeGirl", me)
    bpy.context.collection.objects.link(ob)

    mats = [
        ("Skin", (0.98, 0.80, 0.72), 0.65, 0.0),
        ("Hair", (0.36, 0.22, 0.42), 0.42, 0.0),
        ("Cloth", (0.92, 0.36, 0.42), 0.72, 0.0),
        ("EyeWhite", (0.98, 0.98, 1.00), 0.25, 0.0),
        ("Iris", (0.20, 0.55, 0.85), 0.15, 0.0),
        ("Shoe", (0.22, 0.16, 0.20), 0.55, 0.0),
        ("Lash", (0.08, 0.05, 0.12), 0.35, 0.0),
    ]
    for (name, col, rough, metal) in mats:
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        bsdf = m.node_tree.nodes["Principled BSDF"]
        bsdf.inputs["Base Color"].default_value = (*col, 1.0)
        bsdf.inputs["Roughness"].default_value = rough
        bsdf.inputs["Metallic"].default_value = metal
        me.materials.append(m)
    for poly, mi in zip(me.polygons, FACE_MAT):
        poly.material_index = mi
        poly.use_smooth = True

    sub = ob.modifiers.new("Subdivision", "SUBSURF")
    sub.levels = SUBSURF
    sub.render_levels = SUBSURF
    return ob


# ------------------------------------------------------------ 骨架

BONES = [
    # (name, head, tail, parent, 绕自身轴的附加旋转)
    ("hips",        (0.0, 0.870, 0.0), (0.0, 0.960, 0.0), None),
    ("spine",       (0.0, 0.955, 0.0), (0.0, 1.055, 0.0), "hips"),
    ("chest",       (0.0, 1.055, 0.0), (0.0, 1.190, 0.0), "spine"),
    ("neck",        (0.0, 1.190, 0.0), (0.0, 1.320, 0.0), "chest"),
    ("head",        (0.0, 1.315, 0.0), (0.0, 1.470, 0.0), "neck"),
    # 右臂（+Z 侧）
    ("shoulder.R",  (0.030, 1.205, 0.115), (0.130, 1.200, 0.115), "chest"),
    ("upperarm.R",  (0.132, 1.200, 0.118), (0.181, 0.945, 0.118), "chest"),
    ("lowerarm.R",  (0.181, 0.945, 0.118), (0.203, 0.735, 0.118), "upperarm.R"),
    ("hand.R",      (0.203, 0.735, 0.118), (0.208, 0.660, 0.118), "lowerarm.R"),
    # 左臂
    ("shoulder.L",  (0.030, 1.205, -0.115), (0.130, 1.200, -0.115), "chest"),
    ("upperarm.L",  (0.132, 1.200, -0.118), (0.181, 0.945, -0.118), "chest"),
    ("lowerarm.L",  (0.181, 0.945, -0.118), (0.203, 0.735, -0.118), "upperarm.L"),
    ("hand.L",      (0.203, 0.735, -0.118), (0.208, 0.660, -0.118), "lowerarm.L"),
    # 右腿
    ("thigh.R",     (0.000, 0.870, 0.062), (0.000, 0.520, 0.070), "hips"),
    ("shin.R",      (0.000, 0.520, 0.070), (0.000, 0.100, 0.073), "thigh.R"),
    ("foot.R",      (0.000, 0.100, 0.073), (0.075, 0.030, 0.073), "shin.R"),
    ("toe.R",       (0.075, 0.030, 0.073), (0.115, 0.022, 0.073), "foot.R"),
    # 左腿
    ("thigh.L",     (0.000, 0.870, -0.062), (0.000, 0.520, -0.070), "hips"),
    ("shin.L",      (0.000, 0.520, -0.070), (0.000, 0.100, -0.073), "thigh.L"),
    ("foot.L",      (0.000, 0.100, -0.073), (0.075, 0.030, -0.073), "shin.L"),
    ("toe.L",       (0.075, 0.030, -0.073), (0.115, 0.022, -0.073), "foot.L"),
]


def build_armature():
    data = bpy.data.armatures.new("Rig")
    arm = bpy.data.objects.new("Rig", data)
    bpy.context.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    for (name, head, tail, parent) in BONES:
        b = data.edit_bones.new(name)
        b.head = Vector(head)
        b.tail = Vector(tail)
        if parent:
            b.parent = data.edit_bones[parent]
            b.use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")
    arm.show_in_front = True
    arm.data.display_type = "OCTAHEDRAL"
    return arm


# ------------------------------------------------------------ 动画

def rot_axis(pb, axis, angle):
    """给 pose bone 一个**世界/骨架空间**的轴角旋转。

    直接写 rotation_euler 会踩到"骨骼自身朝向"的坑：脚本里所有摆动都
    想绕世界 Z（角色面朝 +X，摆动平面是 XY），但 bone 的局部轴未必是
    世界轴。先把世界旋转换算到骨骼静止坐标系里：
        q_local = q_rest⁻¹ · q_world
    """
    q_world = Quaternion(Vector(axis).normalized(), angle)
    q_rest = pb.bone.matrix_local.to_quaternion()
    pb.rotation_mode = "QUATERNION"
    pb.rotation_quaternion = (q_rest.inverted() @ q_world)


def key(pb, frame, loc=None):
    if loc is not None:
        pb.location = Vector(loc)
        pb.keyframe_insert(data_path="location", frame=frame)
    pb.keyframe_insert(data_path="rotation_quaternion", frame=frame)


def pose_at(frame, thigh, knee, ankle, arm, elbow, bob, twist, lean):
    """写一个 walk 关键帧。角度单位 rad。"""
    P = bpy.data.objects["Rig"].pose.bones
    rot_axis(P["thigh.R"], (0, 0, 1), thigh)
    rot_axis(P["thigh.L"], (0, 0, 1), -thigh)
    rot_axis(P["shin.R"], (0, 0, 1), knee)
    rot_axis(P["shin.L"], (0, 0, 1), max(0.0, -knee))
    rot_axis(P["foot.R"], (0, 0, 1), ankle)
    rot_axis(P["foot.L"], (0, 0, 1), -ankle * 0.6)
    rot_axis(P["upperarm.R"], (0, 0, 1), -arm)
    rot_axis(P["upperarm.L"], (0, 0, 1), arm)
    rot_axis(P["lowerarm.R"], (0, 1, 0), elbow)
    rot_axis(P["lowerarm.L"], (0, 1, 0), -elbow)
    rot_axis(P["hips"], (0, 0, 1), lean)
    P["hips"].location = (0.0, bob, 0.0)
    rot_axis(P["chest"], (0, 1, 0), twist)
    for name in ("thigh.R", "thigh.L", "shin.R", "shin.L", "foot.R", "foot.L",
                 "upperarm.R", "upperarm.L", "lowerarm.R", "lowerarm.L",
                 "hips", "chest"):
        key(P[name], frame)


def new_action(rig, name):
    """给骨架挂一个**全新独立**的 action。

    不新建就会出这种坑：walk 和 idle 的关键帧都被 `keyframe_insert` 写进
    当前挂着的那一个 action 里，后一次改名把前一次整个顶掉（实测：导出的
    glb 里只剩一条叫Idle 的动画，通道里混着走位的关键帧）。
    Blender 4.4+ 的 action 带 slot，赋过去之后还要指明用哪个 slot。
    """
    act = bpy.data.actions.new(name)
    try:
        act.slots.new(id_type="OBJECT", name=rig.name)
    except (AttributeError, TypeError):
        pass
    ad = rig.animation_data_create()
    ad.action = act
    try:
        ad.action_slot = act.slots[0]
    except (AttributeError, IndexError, TypeError):
        pass
    return act


def anim_walk():
    rig = bpy.data.objects["Rig"]
    new_action(rig, "Walk")
    P = rig.pose.bones
    #   frame:  接触(迈出) → 过渡 → 接触(反向) → 过渡 → 回到起点
    keys = [
        # frame, thigh,  knee, ankle,  arm, elbow,  bob,  twist,  lean
        (1,  0.46, 0.10, -0.16, 0.40, 0.34, 0.018,  0.10, 0.03),
        (7,  0.10, 0.30,  0.10, 0.06, 0.22, -0.026, 0.02, 0.02),
        (13, -0.46, 0.10,  0.16, -0.40, 0.34, 0.018, -0.10, 0.03),
        (19, -0.10, 0.30, -0.10, -0.06, 0.22, -0.026, -0.02, 0.02),
        (25, 0.46, 0.10, -0.16, 0.40, 0.34, 0.018,  0.10, 0.03),
    ]
    for (f, *vals) in keys:
        pose_at(f, *vals)
    # 循环感：给每条 F-Curve 加 Cycles 修改器（导出时按帧采样，循环本身
    # 由导出后的 clip 长度决定，但加了更保险）
    act = rig.animation_data.action
    for fc in _all_fcurves(act):
        # 显式 LINEAR：导出器默认给 STEP，60fps 下播起来一卡一卡的
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"
        if not any(m.type == "CYCLES" for m in fc.modifiers):
            fc.modifiers.new("CYCLES")


def anim_idle():
    rig = bpy.data.objects["Rig"]
    new_action(rig, "Idle")
    P = rig.pose.bones
    for f, (breath, sway) in ((1, (0.0, 0.0)), (13, (0.012, 0.018)),
                              (25, (0.0, 0.0))):
        rot_axis(P["chest"], (0, 1, 0), sway)
        P["hips"].location = (0.0, breath, 0.0)
        rot_axis(P["upperarm.R"], (0, 1, 0), 0.04)
        rot_axis(P["upperarm.L"], (0, 1, 0), -0.04)
        for name in ("chest", "hips", "upperarm.R", "upperarm.L"):
            key(P[name], f)


def _all_fcurves(action):
    """兼容 4.4+ 的 slotted action。"""
    try:
        if hasattr(action, "layers") and len(action.layers):
            out = []
            for layer in action.layers:
                for strip in layer.strips:
                    for bag in strip.channelbags:
                        out.extend(bag.fcurves)
            if out:
                return out
    except Exception:
        pass
    try:
        return list(action.fcurves)
    except Exception:
        return []


def push_to_nla(arm):
    """把两个 action 都推进 NLA 轨道 —— 否则导不出两条动画。

    为什么：同一个骨架对象的 animation_data.action 只能挂**一个** action，
    后挂的 Idle 会把 Walk 顶掉；导出器的 ACTIONS 模式导的是"当前挂着
    那些"，于是只剩一条。推进 NLA（两条轨道）再用 NLA_TRACKS 模式导，
    一个 action = 一条 animation。
    """
    ad = arm.animation_data_create()
    names = [a.name for a in bpy.data.actions if a.name in ("Walk", "Idle")]
    ad.action = None
    for i, name in enumerate(names):
        act = bpy.data.actions[name]
        track = ad.nla_tracks.new()
        track.name = name
        strip = track.strips.new(name, 1, act)
        # Blender 4.4+ 的 slotted action：strip 必须指到具体 slot
        try:
            if hasattr(strip, "action_slot"):
                slots = [s for s in act.slots
                         if s.target_id_type == "OBJECT"]
                if slots:
                    strip.action_slot = slots[0]
        except Exception as e:      # 老版本没有 slot 概念
            print("[girl] slot 绑定跳过:", e)
        strip.frame_start = 1
        strip.frame_end = WALK_FRAMES + 1
        strip.action_frame_start = float(act.frame_range[0])
        strip.action_frame_end = float(act.frame_range[1])
        strip.blend_type = "REPLACE"
        strip.extrapolation = "NOTHING"
        print("[girl] NLA 轨道:", track.name,
              "%.3f..%.3f" % (strip.frame_start, strip.frame_end))
    return len(names)


# ------------------------------------------------------------ 预览渲染

def _look_at(ob, loc, target, up=(0.0, 1.0, 0.0)):
    """把物体摆成"从 loc 看向 target"。

    ⚠ **不能**用 `dir.to_track_quat('-Z', 'Y')`：它内部的"世界上方向"参照
    是 +Z，而本角色是沿 **+Y** 站立的（Y-up），于是相机会绕视线滚 90°，
    渲出来的人横躺着（实测踩过）。这里直接用基底向量拼旋转矩阵，
    up 显式给 +Y。
    """
    from mathutils import Matrix
    fwd = (Vector(target) - Vector(loc)).normalized()
    zc = -fwd                                    # 相机局部 +Z 朝后
    xc = Vector(up).cross(zc).normalized()       # 局部 +X = up × Z
    if xc.length < 1e-6:                         # 视线与 up 平行的退化情形
        xc = Vector((1.0, 0.0, 0.0))
    yc = zc.cross(xc)                            # 局部 +Y = Z × X
    m = Matrix((xc, yc, zc)).transposed().to_4x4()
    m.translation = Vector(loc)
    ob.matrix_world = m


def render_preview(dirname):
    """渲 3 张 PNG：正面 / 四分之三侧 / 迈步中。

    没有这一步就没法目视验收 —— 角色好不好看、蒙皮有没有崩、动画姿势对不对，
    只能看图。而 headless 环境下 EEVEE 可能拿不到 GL 上下文，所以先试
    EEVEE，失败退回 Cycles CPU。
    """
    scene = bpy.context.scene
    os.makedirs(dirname, exist_ok=True)
    # ⚠ 画幅必须是**方形**。ortho_scale 的生效方向取决于 sensor fit AUTO：
    # 竖版画幅时它作用于与横版相反的轴（Blender 4.x 在 portrait 下按纵向
    # 覆盖 = ortho_scale × (长边/短边) 生效），560x900 下 1.72 实测导致
    # 横向 530px/m、纵向 254px/m —— 像素不是方的，量测全乱。方形画幅下
    # 两轴覆盖都 = ortho_scale，无歧义。
    scene.render.resolution_x = 900
    scene.render.resolution_y = 900
    scene.render.film_transparent = False
    # ⚠ Blender 4.x 默认 AgX 视图变换会把所有颜色压成"粉粉白白"，
    #   验收图必须用 Standard 才看得出配色对不对
    try:
        scene.view_settings.view_transform = "Standard"
        scene.view_settings.look = "None"
    except TypeError:
        pass
    scene.world = bpy.data.worlds.new("W")
    scene.world.use_nodes = True          # 不开这个 node_tree 是 None
    scene.world.node_tree.nodes["Background"].inputs[0].default_value = (
        0.09, 0.10, 0.13, 1.0)
    scene.world.node_tree.nodes["Background"].inputs[1].default_value = 1.0

    # 三点打光
    for (loc, energy, size) in (((2.4, 2.2, 2.6), 260, 2.5),
                                 ((-2.6, 1.7, 1.8), 90, 4.0),
                                 ((0.4, 2.6, -2.6), 150, 2.0)):
        ld = bpy.data.lights.new("L", type="AREA")
        ld.energy = energy
        ld.size = size
        lo = bpy.data.objects.new("L", ld)
        bpy.context.collection.objects.link(lo)
        _look_at(lo, loc, (0.0, 1.0, 0.0))

    cam_data = bpy.data.cameras.new("Cam")
    # 正交 + 明确取景：图里角色的比例可以直接拿尺子量，
    # 透视 + sensor fit 的组合会让"看起来矮"到底是真矮还是取景问题分不清。
    # 角色高 1.556m（脚底 y=0），方形画幅下 ortho_scale = 画面覆盖的
    # 世界边长；1.95 → 角色占 80%，四周留 10% 边距。
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = 1.95
    cam = bpy.data.objects.new("Cam", cam_data)
    bpy.context.collection.objects.link(cam)
    scene.camera = cam

    # 取景中心 = 角色几何中心（y 0..1.556 → 0.78），保证画面上下留白对称
    target = Vector((0.0, 0.78, 0.0))

    # ---- 参照物（只出现在 --preview 渲染里；glb 导出在此之前已完成）----
    # 1m 红柱（y 0..1）+ 平躺地面：取景正确性的"尺子"。验证完成后
    # 这里保留 —— 以后每次重渲都能一眼看出取景/贴地是否还对。
    # （量测验收在 tools/measure_girl_shot.py，柱子会被红色判据排除）
    bpy.ops.mesh.primitive_cube_add(size=1.0,
                                    location=(0.0, 0.5, 0.55))
    pole = bpy.context.active_object
    pole.name = "RefPole"
    pole.scale = (0.015, 0.5, 0.015)
    pm = bpy.data.materials.new("RefPoleMat")
    pm.use_nodes = True
    pb = pm.node_tree.nodes["Principled BSDF"]
    pb.inputs["Base Color"].default_value = (0.9, 0.1, 0.1, 1.0)
    pole.data.materials.append(pm)

    bpy.ops.mesh.primitive_plane_add(size=2.4, location=(0.0, 0.0, 0.0))
    ground = bpy.context.active_object
    ground.name = "RefGround"
    # plane 默认躺在 XY 平面（法线 +Z）—— Y-up 场景里要绕 X 转 90° 才平躺
    ground.rotation_euler = (1.5707963, 0.0, 0.0)
    gm2 = bpy.data.materials.new("RefGroundMat")
    gm2.use_nodes = True
    gb = gm2.node_tree.nodes["Principled BSDF"]
    gb.inputs["Base Color"].default_value = (0.75, 0.75, 0.78, 1.0)
    ground.data.materials.append(gm2)

    def shoot(name, campos, frame):
        scene.frame_set(frame)
        _look_at(cam, campos, target)
        scene.render.filepath = os.path.join(dirname, name)
        bpy.ops.render.render(write_still=True)
        print("[girl] 预览:", scene.render.filepath)

    try:
        scene.render.engine = "BLENDER_EEVEE_NEXT"
    except TypeError:
        scene.render.engine = "BLENDER_EEVEE"
    # 静息姿势：先把 NLA 静音，否则渲出来的是第 1 帧的迈步姿势，
    # 看基础结构会被腿的前后摆动干扰。
    ad = bpy.data.objects["Rig"].animation_data
    for t in ad.nla_tracks:
        t.mute = True
    # ⚠ NLA 静音 ≠ rest pose！anim_walk/anim_idle 用 rot_axis 直接转过
    #   pose bones 且从未复位 —— EVAL 用的就是这份**残留姿势**，渲染里
    #   下半身会被折叠成一团（实测 EVAL bbox y 0.776..1.531，腿全没了）。
    #   这里把每根 pose bone 的局部变换清零，才真正回到 rest。
    for pb in bpy.data.objects["Rig"].pose.bones:
        pb.matrix_basis = Matrix.Identity(4)
    # 再把 Armature 修改器关掉 → 渲染**未经蒙皮变形**的原始网格。
    # ⚠ 必须用 show_render：EEVEE 渲染只看这个开关，show_viewport 关了
    #   渲染照跑（实测 raw 图与 rest 图像素完全相同就是这么来的）。
    ob = bpy.data.objects["AnimeGirl"]
    arm_mods = [m for m in ob.modifiers if m.type == "ARMATURE"]
    for m in arm_mods:
        m.show_render = False
        m.show_viewport = False
    scene.frame_set(1)
    bpy.context.view_layer.update()
    shoot("girl_raw_front.png", (3.4, 0.80, 0.0), 1)
    shoot("girl_raw_side.png", (0.0, 0.80, 3.4), 1)
    for m in arm_mods:
        m.show_render = True
        m.show_viewport = True
    bpy.context.view_layer.update()
    shoot("girl_rest_front.png", (3.4, 0.80, 0.0), 1)
    shoot("girl_rest_side.png", (0.0, 0.80, 3.4), 1)
    for t in ad.nla_tracks:
        t.mute = False
    try:
        shoot("girl_front.png", (3.6, 0.80, 0.0), 1)
        shoot("girl_walk.png", (0.0, 0.95, 3.4), 7)
        shoot("girl_side.png", (0.0, 0.80, 3.6), 1)
        shoot("girl_back.png", (-3.0, 1.10, 1.9), 13)
        # ---- 诊断：顶视图 + 纯参照物图（定位取景/坐标歧义用）----
        # 顶视图：从 +Y 往下看，画面上 = 世界 +Z、画面右 = 世界 -X。
        # 柱子应是一个点/短棒位于画面下方偏右（z=+0.55 → 上偏？直接看）。
        shoot("diag_top.png", (0.0, 3.6, 0.0), 1)
        ob.hide_render = True
        for m in arm_mods:
            m.show_viewport = False
        shoot("diag_ref_only_front.png", (3.4, 0.80, 0.0), 1)
        ob.hide_render = False
        for m in arm_mods:
            m.show_viewport = True
    except RuntimeError as e:
        print("[girl] EEVEE 无头渲染失败（%s），退回 Cycles CPU" % e)
        scene.render.engine = "CYCLES"
        scene.cycles.samples = 24
        scene.cycles.device = "CPU"
        shoot("girl_front.png", (3.6, 0.80, 0.0), 1)
        shoot("girl_walk.png", (0.0, 0.95, 3.4), 7)
        shoot("girl_side.png", (0.0, 0.80, 3.6), 1)
        shoot("girl_back.png", (-3.0, 1.10, 1.9), 13)


# ------------------------------------------------------------ 主流程

def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    scene.frame_start = 1
    scene.frame_end = WALK_FRAMES

    ob = build_character()
    arm = build_armature()

    bpy.ops.object.select_all(action="DESELECT")
    ob.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")
    print("[girl] 蒙皮权重完成（automatic weights）")

    # 校验：脚底必须严格贴 y=0
    ys = [v.co.y for v in ob.data.vertices]
    print("[girl] 顶点 %d / 面 %d / 骨骼 %d" %
          (len(ob.data.vertices), len(ob.data.polygons), len(arm.data.bones)))
    print("[girl] y 范围 %.4f .. %.4f（脚底应为 0.0000）" % (min(ys), max(ys)))
    names = ["Skin", "Hair", "Cloth", "EyeW", "Iris", "Shoe", "Lash"]
    for mi, nm in enumerate(names):
        pts = [ob.data.vertices[p.vertices[0]].co
               for p, m in zip(ob.data.polygons, FACE_MAT) if m == mi]
        if pts:
            print("[girl]   %-6s y %.3f..%.3f  x %.3f..%.3f  z %.3f..%.3f" % (
                nm, min(p.y for p in pts), max(p.y for p in pts),
                min(p.x for p in pts), max(p.x for p in pts),
                min(p.z for p in pts), max(p.z for p in pts)))

    anim_walk()
    print("[girl] Walk action 完成")
    anim_idle()
    print("[girl] Idle action 完成")
    n_nla = push_to_nla(arm)
    print("[girl] 推进 NLA 的 action 数: %d" % n_nla)

    out = OUT if os.path.isabs(OUT) else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), OUT)
    os.makedirs(os.path.dirname(out), exist_ok=True)

    kwargs = dict(
        filepath=out, export_format="GLB", use_selection=False,
        export_animations=True, export_skins=True,
        export_yup=True, export_frame_range=True, export_force_sampling=True,
        # ⚠ 必须用 NLA_TRACKS：同一个对象的 action 只能挂一个，
        #   Walk 会被后挂的 Idle 顶掉，只剩一条动画。
        export_animation_mode="NLA_TRACKS", export_nla_strips=True,
        export_materials="EXPORT", export_texcoords=True, export_normals=True,
    )
    # 不同 Blender 小版本的导出参数有增删，逐个降级重试
    while True:
        try:
            bpy.ops.export_scene.gltf(**kwargs)
            break
        except TypeError as e:
            bad = str(e)
            removed = None
            for k in list(kwargs):
                if k in bad and k not in ("filepath", "export_format"):
                    removed = k
                    break
            if not removed:
                print("[girl] 导出失败:", e)
                raise
            print("[girl] 该版本不支持参数 %s，去掉重试" % removed)
            kwargs.pop(removed)

    print("[girl] 已导出:", out, os.path.getsize(out), "bytes")

    if "--preview" in (argv if "--" in sys.argv else []):
        prev = os.path.join(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "..", "_venn_verify")
        render_preview(os.path.normpath(prev))


if __name__ == "__main__":
    main()