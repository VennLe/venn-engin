# ============================================================
# export_clip_dup_fbx.py —— 造一份**极小**的"take 名重复"FBX 夹具
#
# 为什么需要它
# ------------------------------------------------------------------
# `verify_clip_dedup` 验的是 `assetio/ClipDedup.h`：Blender 用
# `bake_anim_use_all_actions` 导 FBX 时，会把每个 action 既当成
# AnimationStack 导一遍、又把 NLA 轨道再导一遍，于是
# `Rig|Idle` 出现两次、`Rig|Walk` 出现两次。剥掉 `Rig|` 前缀之后
# 名字撞车 → `findClip()` 逐字取第一个 → 第二段永远播不到；更糟的是
# 角色窗口里两行同名条目 + 同帧同 ID → ImGui 弹模态告警，之后**所有
# 鼠标点击都被它吃掉**。
#
# 这份夹具原来的载体是那具已经退役的粗糙角色（5.8 MB）。为了两个
# 撞名的 take 把一整具人物签进仓库不划算，所以这里造一个
# 「1 根骨 + 1 个方块 + 2 个 action」的最小复现：它只需要**撞名**这个
# 性质，不需要好看、不需要多骨。
#
# 输出：assets/models/test_clip_dup.fbx（与 test_cube.fbx 同级的测试资产）
#
# 用法：
#   "D:/blender/blender.exe" --background --factory-startup \
#       --python tools/export_clip_dup_fbx.py
# ============================================================
import os
import sys

import bpy

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "models", "test_clip_dup.fbx")
if "--" in sys.argv:
    argv = sys.argv[sys.argv.index("--") + 1:]
    if "--out" in argv:
        OUT = argv[argv.index("--out") + 1]

FRAMES = 24          # 两个 action 同长，撞名才判得了"同名 + 同时长"
AXIS = (0.0, 0.0, 0.25)


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # ---- 1) 一根骨的骨架 ----
    arm_data = bpy.data.armatures.new("Rig")
    arm = bpy.data.objects.new("Rig", arm_data)
    bpy.context.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    bone = arm_data.edit_bones.new("Root")
    bone.head = (0.0, 0.0, 0.0)
    bone.tail = (0.0, 0.0, 0.5)
    bpy.ops.object.mode_set(mode="OBJECT")

    # ---- 2) 一个方块，自动权重挂到骨架上 ----
    bpy.ops.mesh.primitive_cube_add(size=0.4, location=(0.0, 0.0, 0.5))
    mesh = bpy.context.active_object
    mesh.name = "DupMesh"
    bpy.ops.object.select_all(action="DESELECT")
    mesh.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")

    # ---- 3) 两个同长的 action ----
    for name, dz in (("Idle", 0.10), ("Walk", 0.25)):
        act = bpy.data.actions.new(name)
        ad = arm.animation_data_create()
        ad.action = act
        pb = arm.pose.bones["Root"]
        for f, k in ((1, 0.0), (FRAMES + 1, dz)):
            pb.location = (0.0, 0.0, k)
            pb.keyframe_insert("location", frame=f,
                               group=pb.name)
        ad.action = None
        print("[clipdup] action %s: frames=%d..%d"
              % (name, 1, FRAMES + 1))

    # ---- 4) 推进 NLA —— 与旧版的做法一致，导出器会把每个 action
    #         再导一遍，于是 4 个栈 / 2 个名字。
    #         ⚠ 顺序必须是 Idle → Walk：`verify_clip_dedup` 的 C 组钉的是
    #         `FBX clips count=2 names=[Idle,Walk]`（去重保留**首次出现**的
    #         那一条，所以 NLA 轨道顺序直接决定输出顺序）。
    ad = arm.animation_data_create()
    ad.action = None
    for name in ("Idle", "Walk"):
        act = bpy.data.actions[name]
        track = ad.nla_tracks.new()
        track.name = name
        strip = track.strips.new(name, 1, act)
        try:
            if hasattr(strip, "action_slot"):
                slots = [s for s in act.slots
                         if s.target_id_type == "OBJECT"]
                if slots:
                    strip.action_slot = slots[0]
        except Exception as e:                      # 老版本没有 slot
            print("[clipdup] slot 绑定跳过:", e)
        strip.frame_start = 1
        strip.frame_end = FRAMES + 1
        strip.action_frame_start = float(act.frame_range[0])
        strip.action_frame_end = float(act.frame_range[1])
        strip.blend_type = "REPLACE"
        strip.extrapolation = "NOTHING"

    # ---- 5) 导出 ----
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    bpy.ops.export_scene.fbx(
        filepath=OUT, use_selection=False,
        object_types={"ARMATURE", "MESH"}, bake_anim=True,
        add_leaf_bones=False)
    print("[clipdup] 已导出: %s (%d bytes)"
          % (OUT, os.path.getsize(OUT)))


main()
