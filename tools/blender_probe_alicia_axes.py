"""骨轴标定探针：量出 Alicia 的骨局部轴与网格的真实关系。

**为什么留着**：`blender_import_alicia.py` 里那些硬编码角度（下垂 RY(−72°)、
迈步 RX(±25°)）不是猜的，是这里量出来的。删掉这个脚本，那些数字就变成
"无从复核的魔数"了。换下一个 VRM 资产时也直接复用它。

**为什么需要**：rest-delta 版的 basis 自检 err 全 0，姿势却把手举过头顶。
怀疑 Blender 的 glTF 导入器重建的 matrix_local 与网格 bind 脱节。
这里直接量：给 LeftArm 绕局部某轴转 45°，看**手附近的顶点**实际跑到哪 ——
用"顶点实际位移"当判据，而不是看矩阵算得对不对。

结论（Alicia 实测）：
  · 骨 y_axis 指向头顶（tail 被导到 head 正上方），与网格 bind 方向无关
  · 绕**世界** Y 转 +75° → 手臂抬起，所以下垂是 RY(−72°)
  · 腿沿 −Z → 迈步是 RX(±25°)，正值朝 +Y（前）
  · 手臂前后摆 = RX(swing) @ RY(drop)（先下垂，再摆）

用法：
    blender --background --factory-startup \
        --python tools/blender_probe_alicia_axes.py
"""
import bpy
import math
from mathutils import Vector, Quaternion

VRM = "E:/code/cpp/_venn_tmp/dl/AliciaSolid.vrm"

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
bpy.ops.import_scene.gltf(filepath=VRM)
arm = [o for o in bpy.data.objects if o.type == "ARMATURE"][0]
meshes = [o for o in bpy.data.objects if o.type == "MESH"
          and any(m.type == "ARMATURE" for m in o.modifiers)]

bo = arm.data.bones["LeftArm"]
print("PROBE LeftArm x_axis=(%.3f,%.3f,%.3f) y_axis=(%.3f,%.3f,%.3f) "
      "z_axis=(%.3f,%.3f,%.3f)"
      % (*bo.x_axis, *bo.y_axis, *bo.z_axis))
print("PROBE LeftArm head=(%.3f,%.3f,%.3f) tail=(%.3f,%.3f,%.3f) len=%.3f"
      % (*bo.head_local, *bo.tail_local, bo.length))

hand_head = arm.matrix_world @ arm.data.bones["LeftHand"].head_local
print("PROBE LeftHand rest head world=(%.3f,%.3f,%.3f)" % (*hand_head,))


def hand_vertex():
    """找离 LeftHand 骨最近的那个顶点（索引 + 世界坐标）。"""
    best, bi, bm = 1e9, -1, None
    for m in meshes:
        for v in m.data.vertices:
            w = m.matrix_world @ v.co
            d = (w - hand_head).length
            if d < best:
                best, bi, bm = d, v.index, m
    return bm, bi, best


bm, bi, dist = hand_vertex()
print("PROBE nearest vert mesh=%s idx=%d dist=%.4f" % (bm.name, bi, dist))


def deformed_pos(m, idx):
    dg = bpy.context.evaluated_depsgraph_get()
    ev = m.evaluated_get(dg)
    me = ev.to_mesh()
    p = ev.matrix_world @ me.vertices[idx].co
    ev.to_mesh_clear()
    return p


p0 = deformed_pos(bm, bi)
print("PROBE rest vert=(%.4f,%.4f,%.4f)" % (*p0,))

pb = arm.pose.bones["LeftArm"]
pb.rotation_mode = "QUATERNION"

for axis_name, axis in (("X", (1, 0, 0)), ("Y", (0, 1, 0)), ("Z", (0, 0, 1))):
    pb.rotation_quaternion = Quaternion(Vector(axis), math.radians(45.0))
    bpy.context.view_layer.update()
    p1 = deformed_pos(bm, bi)
    delta = p1 - p0
    # 骨本身的朝向变化
    ay = (arm.matrix_world.to_3x3() @ arm.pose.bones["LeftArm"].y_axis).normalized()
    print("PROBE local%s45  vert=(%.4f,%.4f,%.4f) moved=%.4f boneY=(%.2f,%.2f,%.2f)"
          % (axis_name, p1.x, p1.y, p1.z, delta.length, ay.x, ay.y, ay.z))

pb.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
bpy.context.view_layer.update()

# 再测：绕**世界** Y 轴转（这是"手臂下垂"该有的旋转）
from mathutils import Matrix
Ri = (arm.matrix_world.to_3x3().normalized()).inverted()
Rw = Matrix.Rotation(math.radians(75.0), 3, "Y") @ arm.matrix_world.to_3x3().normalized()
local = Ri @ Rw @ (arm.matrix_world.to_3x3().normalized())
rest_local = arm.data.bones["LeftArm"].matrix_local.to_3x3().normalized()
basis = rest_local.inverted() @ local @ rest_local
pb.rotation_quaternion = basis.to_quaternion()
bpy.context.view_layer.update()
p2 = deformed_pos(bm, bi)
ay = (arm.matrix_world.to_3x3() @ arm.pose.bones["LeftArm"].y_axis).normalized()
print("PROBE worldY75  vert=(%.4f,%.4f,%.4f) moved=%.4f boneY=(%.2f,%.2f,%.2f)"
      % (p2.x, p2.y, p2.z, (p2 - p0).length, ay.x, ay.y, ay.z))
