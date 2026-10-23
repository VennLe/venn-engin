"""Blender 4.5 —— 把 Animate_Girl 的**静态网格**绑到 Mixamo 骨架上，并合入四段动画。

为什么需要这一步（2026-10-23 实测的资产真相）：
    `E:/dev/UE-resources/Animate_Girl/animate-girl.fbx`
        → 27394 顶点 / 1181 个独立壳 / **0 顶点组、0 骨骼**，纯静态雕塑。
    `resources/{Standing Idle,Walking,Fast Run,Jump}.fbx`
        → 每份 65 根 `mixamorig:*` 骨 + 370 条 AnimCurve、**没有网格**。
    缺的那一层就是**蒙皮权重**。引擎侧不补（A9 retarget 明确后置），
    所以在这里补，用 Blender 的骨热（bone heat）。

三个必须踩过去的技术坑（都实测过，注释里保留原因）：

1) **Mixamo 的 rest 姿势是坏的**：bind pose 整体比网格高 0.95 m
   （Hips rest z=1.96 / pose z=1.00）。rest 与网格不重叠 → 骨热
   "failed to find solution" → **0 个顶点有权重**，而且不报错、只是空。
   解法：`pose.armature_apply()` 把当前姿势烘成 rest，再清掉那条已失效的 action。

2) **整块网格上骨热必失败**：1181 个断开的壳（发丝、睫毛、衣服片）会让
   线性系统无解。实测**逐壳绑定 123/123 全成功**，于是流程是
   "清理 → separate LOOSE → 逐个绑 → join"。

3) **animation_apply 之后原来那条 action 就废了**：rest 一变，F-curve 的
   局部旋转就不是原来那件事了。所以四段 clip 一律**从源文件重新导入 +
   Copy Transforms 约束 + visual keying 烘帧**（世界空间搬运，与 rest 无关）。

产出：
    assets/models/characters/animate_girl.fbx   （网格 + 65 骨 + 4 段 clip）
    然后 `AssetC animate_girl.glb -o assets/models/characters/` → .vmesh

四段 clip 的名字**必须**是 Idle / Walk / Run / Jump：
    `engine/animation/Locomotion.h` 的 LocomotionConfig 逐字查这四个名字
    （大小写敏感）。对不上 = 角色永远站着不动，日志只有一行 WARN。

用法（后台跑，不开窗口）：
    "D:/blender/blender.exe" --background --factory-startup \
        --python tools/blender_import_animategirl.py
    # 可选参数（写在 `--` 之后）：--out <glb 路径>
"""

import sys

import bpy
from mathutils import Vector

# ------------------------------------------------------------ 参数

SRC_DIR = "E:/dev/UE-resources/Animate_Girl"
MESH_FBX = SRC_DIR + "/animate-girl.fbx"
RES_DIR = SRC_DIR + "/resources"

# (clip 名, 源文件)。顺序即导出顺序，便于日志对齐。
CLIPS = [
    ("Idle", RES_DIR + "/Standing Idle.fbx"),
    ("Walk", RES_DIR + "/Walking.fbx"),
    ("Run", RES_DIR + "/Fast Run.fbx"),
    ("Jump", RES_DIR + "/Jump.fbx"),
]

OUT = "assets/models/characters/animate_girl.fbx"
if "--" in sys.argv:
    argv = sys.argv[sys.argv.index("--") + 1:]
    if "--out" in argv:
        OUT = argv[argv.index("--out") + 1]

FPS = 30                          # Mixamo 一律 30 fps；不定死会把秒数算错
WELD = 1.0e-4                     # 焊接阈值（米）


def log(msg):
    print("AGIRL %s" % msg)


# ------------------------------------------------------------ 几何工具

def obj_bounds(objs):
    """一组对象的**世界空间**包围盒（看真实几何，不看 object.scale）。"""
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    dg = bpy.context.evaluated_depsgraph_get()
    for ob in objs:
        if ob.type != "MESH":
            continue
        ev = ob.evaluated_get(dg)
        me = ev.to_mesh()
        mw = ev.matrix_world
        for v in me.vertices:
            p = mw @ v.co
            for i in range(3):
                lo[i] = min(lo[i], p[i])
                hi[i] = max(hi[i], p[i])
        ev.to_mesh_clear()
    return lo, hi


def pose_bounds(arm):
    """骨架**当前姿势**的世界包围盒（不能用 rest：rest 是坏的）。"""
    lo = Vector((1e9, 1e9, 1e9))
    hi = Vector((-1e9, -1e9, -1e9))
    for p in arm.pose.bones:
        for q in (arm.matrix_world @ p.head, arm.matrix_world @ p.tail):
            for i in range(3):
                lo[i] = min(lo[i], q[i])
                hi[i] = max(hi[i], q[i])
    return lo, hi


def select_only(objs, active=None):
    for o in bpy.data.objects:
        o.select_set(False)
    for o in objs:
        o.select_set(True)
    if active is not None:
        bpy.context.view_layer.objects.active = active


def import_fbx(path):
    """导入一个 FBX，返回**本次新增**的对象（导入盘里可能有同名对象）。"""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path)
    return [o for o in bpy.data.objects if o not in before]


# ------------------------------------------------------------ 主流程

def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.fps = FPS
    sc.render.fps_base = 1.0
    log("BEGIN src=%s out=%s fps=%d" % (SRC_DIR, OUT, FPS))

    # ---- 1. 网格：导入 → 烘变换 → 脚底 z=0、水平居中 ----
    new = import_fbx(MESH_FBX)
    meshes = [o for o in new if o.type == "MESH"]
    if not meshes:
        log("FAIL no mesh in %s" % MESH_FBX)
        return 2
    mesh = max(meshes, key=lambda o: len(o.data.vertices))
    for o in new:
        if o is not mesh and o.type in {"CAMERA", "LIGHT"}:
            bpy.data.objects.remove(o, do_unlink=True)
    log("MESH obj=%s verts=%d materials=%d" %
        (mesh.name, len(mesh.data.vertices), len(mesh.data.materials)))

    bpy.context.view_layer.objects.active = mesh
    select_only([mesh], mesh)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    mlo, mhi = obj_bounds([mesh])
    height = mhi.z - mlo.z
    off = Vector((-(mlo.x + mhi.x) * 0.5, -(mlo.y + mhi.y) * 0.5, -mlo.z))
    for v in mesh.data.vertices:
        v.co += off
    mlo, mhi = obj_bounds([mesh])
    log("MESH height=%.3f armspan=%.3f depth=%.3f z=%.3f..%.3f"
        % (mhi.z - mlo.z, mhi.x - mlo.x, mhi.y - mlo.y, mlo.z, mhi.z))

    # ---- 2. 骨架：导入 → 按**姿势**缩放到同高并对位 ----
    arm_new = import_fbx(CLIPS[0][1])
    arms = [o for o in arm_new if o.type == "ARMATURE"]
    if not arms:
        log("FAIL no armature in %s" % CLIPS[0][1])
        return 2
    arm = arms[0]
    log("RIG obj=%s bones=%d root=%s"
        % (arm.name, len(arm.data.bones),
           [b.name for b in arm.data.bones if b.parent is None]))

    sc.frame_set(1)
    bpy.context.view_layer.update()
    plo, phi = pose_bounds(arm)
    s = height / max(1e-6, phi.z - plo.z)
    # ⚠ 必须**乘**在原有 scale 上：Mixamo 的 FBX 是厘米制，导入器把 0.01
    #   挂在 armature 的 object scale 上；直接赋值会把这个 0.01 一起覆盖掉，
    #   骨架放大 100 倍 → 骨热一个顶点都找不到。
    arm.scale = tuple(c * s for c in arm.scale)
    select_only([arm], arm)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

    plo, phi = pose_bounds(arm)
    d = Vector((-(plo.x + phi.x) * 0.5, -(plo.y + phi.y) * 0.5, mlo.z - plo.z))
    arm.location += d
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    plo, phi = pose_bounds(arm)
    log("RIG scaled x%.4f aligned -> pose z=%.3f..%.3f (mesh %.3f..%.3f)"
        % (s, plo.z, phi.z, mlo.z, mhi.z))

    # ---- 3. 把当前姿势烘成 rest（坑 1）----
    if arm.animation_data:
        arm.animation_data.action = None
    arm.data.pose_position = "POSE"
    bpy.context.view_layer.update()
    bpy.ops.object.mode_set(mode="POSE")
    bpy.ops.pose.select_all(action="SELECT")
    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode="OBJECT")
    rh = (arm.matrix_world @ arm.data.bones["mixamorig:Hips"].head_local).z
    rf = (arm.matrix_world @ arm.data.bones["mixamorig:LeftFoot"].head_local).z
    log("REST applied hipsZ=%.3f footZ=%.3f (mesh 脚底 %.3f)" % (rh, rf, mlo.z))

    # ---- 4. 网格清理 + 逐壳绑骨（坑 2）----
    select_only([mesh], mesh)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.remove_doubles(threshold=WELD)
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.mesh.separate(type="LOOSE")
    bpy.ops.object.mode_set(mode="OBJECT")

    parts = [o for o in bpy.data.objects if o.type == "MESH"]
    parts.sort(key=lambda o: -len(o.data.vertices))
    total = sum(len(o.data.vertices) for o in parts)
    log("BIND parts=%d verts=%d" % (len(parts), total))
    bound = 0
    for ob in parts:
        select_only([ob, arm], arm)
        try:
            bpy.ops.object.parent_set(type="ARMATURE_AUTO")
        except Exception as exc:                       # noqa: BLE001
            log("WARN part %s bind failed: %s" % (ob.name, exc))
            continue
        if any(v.groups for v in ob.data.vertices):
            bound += 1
    log("BIND shell ok=%d/%d" % (bound, len(parts)))
    if bound == 0:
        log("FAIL not a single shell got weights")
        return 3

    # 合回一个对象（join 会按名字合并顶点组）
    select_only(parts, parts[0])
    bpy.ops.object.join()
    mesh = bpy.context.view_layer.objects.active
    mesh.name = "Yuzuha_Body"
    weighted = sum(1 for v in mesh.data.vertices if v.groups)
    log("BIND joined verts=%d weighted=%d (%.1f%%) groups=%d"
        % (len(mesh.data.vertices), weighted,
           100.0 * weighted / max(1, len(mesh.data.vertices)),
           len(mesh.vertex_groups)))

    # ---- 5. 四段 clip：源骨架 → Copy Transforms → visual keying 烘帧（坑 3）----
    if arm.animation_data is None:
        arm.animation_data_create()
    arm.animation_data.action = None

    baked = []
    for name, path in CLIPS:
        src_new = import_fbx(path)
        src_arms = [o for o in src_new if o.type == "ARMATURE"]
        if not src_arms:
            log("FAIL no armature in %s" % path)
            return 4
        src = src_arms[0]
        if src.animation_data is None or src.animation_data.action is None:
            log("FAIL no action in %s" % path)
            return 4
        src_act = src.animation_data.action
        fr0, fr1 = int(round(src_act.frame_range[0])), int(round(src_act.frame_range[1]))
        sc.frame_start, sc.frame_end = fr0, fr1
        sc.frame_set(fr0)

        # ⚠ 源骨架先把**同一套均匀缩放**叠上去（Mixamo 的 0.01 厘米制要保留，
        #   所以是乘不是赋值）。不叠的话，源的世界姿势比目标小 1.11 倍，
        #   搬过去的人物会矮一截、脚离地、腿压扁。
        src.scale = tuple(c * s for c in src.scale)
        bpy.context.view_layer.update()

        # ⚠ 用 **位置 + 旋转分开拷**，不要用 Copy Transforms：
        #   Copy Transforms 连**缩放**一起搬，而源 object scale 是 0.01 →
        #   目标每根骨被缩到 1/100，网格立刻炸成"星形"，但日志一切正常。
        for pb in arm.pose.bones:
            cl = pb.constraints.new("COPY_LOCATION")
            cl.name = "AGIRL_RETARGET"
            cl.target = src
            cl.subtarget = pb.name
            cl.target_space = "WORLD"
            cl.owner_space = "WORLD"
            cr = pb.constraints.new("COPY_ROTATION")
            cr.name = "AGIRL_RETARGET_R"
            cr.target = src
            cr.subtarget = pb.name
            cr.target_space = "WORLD"
            cr.owner_space = "WORLD"
            cr.mix_mode = "REPLACE"

        act = bpy.data.actions.new(name)
        act.use_fake_user = True
        arm.animation_data.action = act
        try:
            if hasattr(arm.animation_data, "action_slot") and act.slots:
                arm.animation_data.action_slot = act.slots[0]
        except Exception:                              # noqa: BLE001
            pass

        select_only([arm], arm)
        bpy.ops.object.mode_set(mode="POSE")
        bpy.ops.pose.select_all(action="SELECT")
        try:
            bpy.ops.nla.bake(frame_start=fr0, frame_end=fr1, step=1,
                             only_selected=False, visual_keying=True,
                             clear_constraints=True, clear_parents=False,
                             use_current_action=True, bake_types={"POSE"})
        except Exception as exc:                       # noqa: BLE001
            log("FAIL bake %s: %s" % (name, exc))
            return 5
        bpy.ops.object.mode_set(mode="OBJECT")
        arm.animation_data.action = act
        act.name = name

        # ---- 砍掉非根骨的平移 + 全部缩放 ----
        #
        # ⚠ 这一步不是"优化"，是**正确性**。Mixamo 的 clip 给每根骨都打了
        #   location 关键帧（值 = 它在源骨架里的绝对位置，厘米制）。这些
        #   平移在 Blender 里回读是对的（两边都乘了 0.01），但经过
        #   `AssetC`（ufbx）之后单位约定不一致 → 偏移被放大 ~100 倍 →
        #   引擎里整个人**炸成星形**（四肢从骨盆向外飞，网格拉成面条），
        #   而日志/断言全都正常。实测就是这个症状。
        #
        #   人形骨架是**刚性**的：骨长不变，非根骨的世界平移完全由父骨的
        #   旋转决定。所以只保留根骨（Hips）的平移、砍掉全部缩放，动画
        #   一个像素都不变，还顺带把 clip 数据量砍掉 2/3。
        root_name = 'pose.bones["mixamorig:Hips"]'
        dropped_loc = dropped_scale = 0
        for fc in list(act.fcurves):
            dp = fc.data_path
            if dp.endswith(".scale"):
                act.fcurves.remove(fc)
                dropped_scale += 1
            elif dp.endswith(".location") and not dp.startswith(root_name):
                act.fcurves.remove(fc)
                dropped_loc += 1
        # 通道删掉之后把姿势里的残值归零，否则导出时还带着最后一帧的偏移
        for pb in arm.pose.bones:
            if pb.name != "mixamorig:Hips":
                pb.location = (0.0, 0.0, 0.0)
            pb.scale = (1.0, 1.0, 1.0)

        # 清掉这次导入的源对象与它自己的 action
        for o in src_new:
            bpy.data.objects.remove(o, do_unlink=True)
        if src_act.users == 0:
            bpy.data.actions.remove(src_act)

        bf = act.frame_range
        baked.append((name, bf[0], bf[1]))
        log("CLIP %-5s frames=%.0f..%.0f dur=%.3fs fcurves=%d "
            "dropped_loc=%d dropped_scale=%d"
            % (name, bf[0], bf[1], (bf[1] - bf[0]) / FPS, len(act.fcurves),
               dropped_loc, dropped_scale))

    # ---- 6. 导出 FBX ----
    # ⚠ 只能出 FBX：`AssetC` 的离线编译器只认 `.fbx → .vmesh`
    #   （`assetc/main.cpp` 的 `isFbx` 闸门；glb 直接 SKIP not_fbx）。
    #
    # ⚠ clip 名会被 FBX 导出器加上 `<armature对象名>|` 前缀。而引擎侧
    #   `Skeleton::findClip` 是**逐字**比较（`Locomotion` 查 Idle/Walk/Run/Jump），
    #   带前缀 = 一个都找不到 = 角色永远站着。
    #   实测「All Actions」模式出的是 `Armature|Idle`，改不了；换成
    #   **NLA 轨**模式后 take 名取的是**轨名**，于是把轨名写成裸 clip 名。
    keep = {"Idle", "Walk", "Run", "Jump"}
    for a in list(bpy.data.actions):
        if a.name in keep:
            a.use_fake_user = True
            continue
        # 源骨架留下的一次性 action（名字里带 mixamo.com 的那批）必须清掉，
        # 否则会被当成额外的 clip 一起导出去（实测会多出 5 段垃圾）。
        a.use_fake_user = False
        bpy.data.actions.remove(a)

    ad = arm.animation_data
    ad.action = None
    for t in list(ad.nla_tracks):
        ad.nla_tracks.remove(t)
    for name in ["Idle", "Walk", "Run", "Jump"]:
        act = bpy.data.actions[name]
        track = ad.nla_tracks.new()
        track.name = name
        track.strips.new(name, int(round(act.frame_range[0])), act)
    sc.frame_set(1)

    select_only([mesh, arm], arm)
    if OUT.lower().endswith(".glb") or OUT.lower().endswith(".gltf"):
        # glTF 出口：clip 名直接取 action 名（没有 `<armature>|` 前缀），
        # 而且引擎有原生 glTF 加载器 —— 这条路上不需要 AssetC。
        bpy.ops.export_scene.gltf(
            filepath=bpy.path.abspath(OUT),
            export_format="GLB",
            use_selection=True,
            export_yup=True,
            export_apply=False,              # 带蒙皮时应用变换会丢权重
            export_animations=True,
            export_animation_mode="ACTIONS",
            export_bake_animation=False,
            export_optimize_animation_size=True,
            export_anim_single_armature=True,
            export_skins=True,
            export_def_bones=False,
            export_rest_position_armature=True,
            export_materials="EXPORT",
            export_cameras=False,
            export_lights=False,
        )
        log("WROTE %s (gltf)" % OUT)
        log("DONE clips=%s" % ",".join(n for n, _, _ in baked))
        return 0

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
        bake_anim_use_all_actions=False,
        bake_anim_use_nla_strips=True,   # ← take 名 = 轨名 = 裸 clip 名
        bake_anim_force_startend_keying=False,
        bake_anim_step=1.0,
        bake_anim_simplify_factor=0.0,
        path_mode="AUTO",
        embed_textures=False,
    )
    log("WROTE %s" % OUT)
    log("DONE clips=%s" % ",".join(n for n, _, _ in baked))
    return 0


if __name__ == "__main__":
    code = main()
    if code:
        log("EXIT %d" % code)
    sys.exit(code)
