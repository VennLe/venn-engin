# ============================================================
# export_test_fbx.py —— 生成 FBX 测试资产（供 FBX 加载器验证）
#   1) 骨骼网格：二次元女孩（复用 blender_make_girl.py 的构建），带 walk 动画
#   2) 静态网格：一个立方体（无骨骼）
# 输出到 assets/models/ 下。
# ============================================================
import bpy
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "blender_make_girl.py"), encoding="utf-8").read()
g = {"__name__": "notmain", "__file__": "blender_make_girl.py"}
exec(compile(src, "girl", "exec"), g)

OUT_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "assets", "models"))
os.makedirs(OUT_DIR, exist_ok=True)

# ---- 1) 骨骼网格 FBX ----
bpy.ops.wm.read_factory_settings(use_empty=True)
ob = g["build_character"]()
arm = g["build_armature"]()
bpy.ops.object.select_all(action="DESELECT")
ob.select_set(True)
arm.select_set(True)
bpy.context.view_layer.objects.active = arm
bpy.ops.object.parent_set(type="ARMATURE_AUTO")
g["anim_walk"]()
g["anim_idle"]()
g["push_to_nla"](arm)

# blender_make_girl.py 的角色是沿 **+Y** 站立构建的（为 glTF Y-up 导出
# 设计）。FBX 走 Blender 原生 Z-up 声明，导出前把 armature 对象绕 X 转
# +90°（+Y → +Z），让角色沿 +Z 站立 —— 与文件声明的 Z-up 一致。
# （网格已通过 ARMATURE_AUTO 挂到 armature 下，会跟随对象变换，且
#  armature 修改器不会重复施加该旋转。）
import math
arm.rotation_euler = (math.radians(90.0), 0.0, 0.0)
bpy.context.view_layer.update()

girl_fbx = os.path.join(OUT_DIR, "anime_girl.fbx")
bpy.ops.export_scene.fbx(
    filepath=girl_fbx, use_selection=False,
    object_types={"ARMATURE", "MESH"}, bake_anim=True, add_leaf_bones=False)
print("[fbx] 骨骼网格已导出:", girl_fbx)

# ---- 2) 静态网格 FBX（立方体）----
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0, 0, 1))
cube = bpy.context.active_object
cube.name = "TestCube"
cube_fbx = os.path.join(OUT_DIR, "test_cube.fbx")
bpy.ops.export_scene.fbx(
    filepath=cube_fbx, use_selection=False,
    object_types={"MESH"}, bake_anim=False)
print("[fbx] 静态网格已导出:", cube_fbx)
