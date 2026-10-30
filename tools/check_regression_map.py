"""自检 `run_regression.py` 的**变更影响分析表**（不跑 GUI，秒级）。

这张表（`GLOBAL_GLOBS` / `TRIGGERS` / `IGNORE_GLOBS`）是"只跑关联用例"的
唯一依据，而它刚好是最容易悄悄腐化的东西：加个新用例忘了登记 → 那一例
退化成"无条件重跑"（安全，但白跑）；把横切文件写进 `TRIGGERS` → `--narrow`
下会**假跳过**（不安全）。本脚本把这两类问题都变成红字。

检查项：
  A. 影响分析的行为分支（用合成快照，不碰仓库文件）
  B. 快照覆盖范围：该进的进、不该进的不进
  C. 映射覆盖率：哪些用例没登记（会无条件重跑）
  D. **死映射**：`TRIGGERS` 里写了不存在、且不带通配的文件路径
  E. **全横切登记**：某用例的 globs 全落在 `GLOBAL_GLOBS` 里 → 等于没登记，
     应移出 `TRIGGERS`（否则 `--narrow` 下会假跳过）

用法：python tools/check_regression_map.py
"""

import importlib.util
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)

spec = importlib.util.spec_from_file_location(
    "rr", os.path.join(TOOLS, "run_regression.py"))
rr = importlib.util.module_from_spec(spec)
sys.modules["rr"] = rr
spec.loader.exec_module(rr)

fails = []


def mk(*paths, mt=100):
    """(路径...) → 合成快照（size|mtime 全同，只用来当"没变"的基准）。"""
    return {p: [100, mt] for p in paths}


OLD = mk("assets/shaders/decal.frag",
         "engine/render/DecalBuffer.cpp",
         "engine/render/Renderer.cpp",
         "engine/render/RenderTypes.h",
         "assets/shaders/pbr.frag",
         "CMakeLists.txt",
         "docs/render_roadmap.md",
         "editor/InspectorPanel.cpp",
         "assets/scenes/demo.json",
         "engine/render/Foo.cpp",
         # M1（G01/G05）：运行时目标与出厂配置 —— 让下面对它们的两条 fixture
         # 是"改了"而不是"新增"（新增会顺带触发"未登记点名"，那是另一回事）。
         "runtime/GameLoop.cpp",
         "assets/game.json",
         # 阶段六/七（2026-10-22）：自研二进制资产链的两个**新顶层目录**。
         # 同样先放进基准，让下面的 fixture 是"改了"而不是"新增"。
         "assetio/MeshFormat.cpp",
         "assetc/main.cpp",
         # §13（顶点格式 112 B）：让下面的 fixture 是"改了"而不是"新增"。
         # 新增会触发"未登记点名"，那验证的是另一条分支。
         "assetio/Vertex.h",
         # §16（两骨 IK，2026-10-25 第八刀）：同理 —— 让下面那两条 IK 的
         # fixture 是"改了"而不是"新增"。
         "engine/animation/TwoBoneIK.cpp",
         "engine/animation/AnimatorSystem.cpp",
         # B1（动画层 / 骨骼遮罩，2026-10-26）：同上 —— 先放进基准，让下面的
         # fixture 是"改了"而不是"新增"。
         "engine/animation/BoneMask.cpp",
         # B2（动画事件 / Notify，2026-10-26）：同上。
         "engine/animation/AnimEvent.cpp",
         # G18（预制体，2026-10-26 #315）：同上 —— 让下面那条 fixture 是
         # "改了"而不是"新增"（新增走的是另一条分支：未登记点名）。
         "engine/scene/Prefab.cpp",
         "engine/assets/Mesh.h")


def with_change(*paths, drop=()):
    d = dict(OLD)
    for p in paths:
        d[p] = [100, 999]      # mtime 变了 = 内容改了
    for p in drop:
        d.pop(p, None)
    return d


def run(name, old, new, narrow=False, want=None, want_unmapped=()):
    """want=None → 期望全量；want=set(...) → 期望**恰好**这些用例。"""
    imp = rr.impact_analysis(old, new, narrow=narrow)
    got = imp["affected"]
    ok = (got is None) if want is None else (got == set(want))
    for u in want_unmapped:
        ok = ok and (u in imp["unmapped"])
    print("  %-40s %s  affected=%s"
          % (name, "OK " if ok else "FAIL",
             "全量" if got is None else sorted(got)))
    if not ok:
        fails.append(name)
        print("       want=%s got=%s unmapped=%s global=%s"
              % ("全量" if want is None else sorted(want),
                 "全量" if got is None else sorted(got),
                 imp["unmapped"], imp["globalh"]))


print("[A] 影响分析行为分支")
run("无变更 → 谁都不跑", OLD, dict(OLD), want=())
run("decal.frag + DecalBuffer.cpp", OLD,
    with_change("assets/shaders/decal.frag", "engine/render/DecalBuffer.cpp"),
    want={"verify_decals", "verify_collision_play"})   # 后者是资源泄漏门
run("Renderer.cpp（横切）→ 全量", OLD,
    with_change("engine/render/Renderer.cpp"), want=None)
run("Renderer.cpp + --narrow → 无关联", OLD,
    with_change("engine/render/Renderer.cpp"), narrow=True, want=())
run("docs/*.md → 忽略", OLD,
    with_change("docs/render_roadmap.md"), want=())
run("CMakeLists.txt（横切，且后缀是 .txt）", OLD,
    with_change("CMakeLists.txt"), want=None)
run("pbr.frag（横切）→ 全量", OLD,
    with_change("assets/shaders/pbr.frag"), want=None)
run("新增**未登记**的 render 文件 → 全量 + 点名", OLD,
    with_change("engine/render/BrandNew.cpp"), want=None,
    want_unmapped=("engine/render/BrandNew.cpp",))
run("改一个已登记的 render 模块（ShadowPass）", OLD,
    with_change("engine/render/ShadowPass.cpp"),
    # ⚠ 2026-10-18 起 ShadowPass 也被 verify_character_import 登记（它要读
    #   `skc->jointBase` 往 push constant 里填蒙皮槽）→ 跟着同步 fixture。
    want={"verify_csm", "verify_shadow_hardening", "verify_collision_play",
          "verify_character_import"})
run("editor/InspectorPanel.cpp → 面板块全家族", OLD,
    with_change("editor/InspectorPanel.cpp"),
    # ⚠ 这里**必须**跟着 run_regression.py 的 TRIGGERS 同步长：
    #   网格参数（mesh_params）/ 分区折叠与搜索（inspector）/
    #   左侧页签导航条（inspector_tabs）/ 粒子面板 12 模块（particles）/
    #   材质槽小方块（material_chips —— 2026-10-16 补进 DEFAULT 时登记的）/
    #   Animation 页签的 INS-ANIM-* 锚点（character_import —— 2026-10-18）。
    #   UI 分区（ui2 —— 2026-10-18：Inspector 的 Render 页新增 "UI" 分组，
    #   `ui.*` 五个参数的控件就长在这里）。
    #   中文化体检（verify_i18n_coverage —— 2026-10-18：它的取值域是 editor/**，
    #   Inspector 是字面量最密的一个文件，所以必然会命中）。
    #   Tool 页签的 Audio Source 段（verify_audio —— G14-7：`INS-AUDIO-HEADER`
    #   与 `INS-AUDIO-PREVIEW` 两个锚点就画在这个文件里）。
    #   2026-10-16 加 E11.1 页签时，这条 fixture 先红了 —— 那是它在正常工作。
    #   材质实例（W1-3 —— 2026-10-18：`INS-MAT-INSTANCE` / `INS-MAT-MAKEUNIQUE`
    #   就长在 `drawMaterialSection` 里）。
    want={"verify_mesh_params", "verify_inspector",
          "verify_inspector_tabs", "verify_particles",
          "verify_material_chips", "verify_character_import",
          "verify_ui2", "verify_i18n_coverage", "verify_audio",
          "verify_material_instances",
          # W4-4（2026-10-19）：Terrain 段（`INS-TERRAIN-*` 十个锚点）
          # 就长在 `drawTerrainSection` 里。
          "verify_terrain_editor",
          # S5（2026-10-19）：Inspector 的 Script 段
          # （`INS-SCRIPT-STATE` / `INS-SCRIPT-RESET`）就长在这个文件的
          # `drawScript()` 里 —— 脚本的"实例状态"那一行是 S5 才加的，
          # 所以这条 fixture 必须跟着长一项。
          "verify_script_editor",
          # 2026-10-24：地形层材质槽多了一个"从材质仓库挑"的下拉
          # （`drawTerrainSection` 里的 `##terlayer_pick`）。
          # 改这个文件本来就该重跑材质仓库那条用例 —— 它验的正是
          # "属性栏里也能绑定对应的材质"。
          "verify_material_library",
          # 2026-10-24：GameStart 升级成"出生参数"，Inspector 新增
          # `drawGameStartSection`（`INS-GAMESTART-HEADER/MODE/SPAWN/ENABLED`）。
          # 那段是**编辑期唯一**能改视角模式（第一/第三人称）的落点，
          # 所以 verify_gamestart_edit 的 [1b] 组必须跟着它跑。
          "verify_gamestart_edit",
          # 2026-10-24（第六轮）：Layer materials 上方新增"材质包目录 + 档位
          # + Refresh"两行（`INS-MATSRC-DIR/TIER/REFRESH/STATUS`）。
          # 这条用例走的就是那几个控件，改 `drawMaterialSourceRow` 必重跑。
          "verify_material_import",
          # 2026-10-25（A1）：Material 段的 shadingModel 下拉多了第三档
          # "Toon"（`kShading[] = {"Default","Skin","Toon"}`）—— 改这个文件
          # 当然该重跑"着色模型 → 变体掩码 → 像素"那条用例。
          "verify_toon_shading",
          # 2026-10-25（P0-2/B）：Material 段新增 Render Queue / Sort Order
          # 两个输入框（紧跟 Double Sided 之后）。改这个文件当然该重跑
          # "渲染顺序 → 谁画在上面"那条用例。
          "verify_render_queue",
          # 2026-10-25（§15）：Animation 页签新增 Morph 段（每根滑条一个
          # `SliderFloat`，标签取自 `MorphRig::channels()[c].name`）。
          # 滑条与 `morph.set` 命令走同一个 `assets::setMorphWeight`，
          # 所以改这里要重跑 GPU 形变那条像素用例。
          "verify_morph_gpu",
          # 2026-10-25（§16 第八刀）：同一个 `drawTabAnimation` 末尾又多了
          # 两段 —— `Two Bone IK`（Enabled / Target / 只读链解析 / 三个骨名 /
          # Weight / Pole mode / Pole vector）与 `Bone Attachment`
          # （host / bone / Offset / Local rot）。它们改的是**组件字段**，
          # 而 verify_ik 的 K1/K2 正是"组件字段 → 求解 → 像素"那条链的判据。
          "verify_ik",
          # 2026-10-26（A4）：Material 段的「Toon/NPR」块新增两行 ——
          # `Hair Aniso`（`INS-NPR-ANISO`）与 `Ring Shift`
          # （`INS-NPR-ANISO-SHIFT`）。这两个滑条写的是材质的
          # `anisoStrength` / `anisoShift`，而 verify_hair_aniso 验的正是
          # "这两个字段 → 材质页 spare.z/w → Kajiya-Kay 环"整条链。
          "verify_hair_aniso",
          # 2026-10-26（A3）：同一段 NPR 之后又多了三行 —— `Eye Unlit`
          # （`INS-NPR-EYE`）/ `Iris Depth`（`INS-NPR-IRIS`）/ `Catchlight`
          # （`INS-NPR-SPARK`）。它们写的是 `eyeStrength` / `eyeIrisDepth` /
          # `eyeCatchlight`，而 verify_eye_shading 验的正是"这三个字段 →
          # 材质页 spare2.xyz → Unlit 底/视差/眼神光"整条链。
          "verify_eye_shading",
          # 2026-10-26（B1）：`drawTabAnimation` 里新增整段
          # `Animation Layers  (upper / lower body)`（八个锚点：LAYERS 段 /
          # ADD / HDR / CLIP / WEIGHT / MASKMODE / MASKROOTS / REMOVE）。
          # 它改的是 `AnimatorComponent::layers`，而 verify_anim_layers 的
          # ⑨ 组就是"这段 UI 真的画出来了"，另一头（写入 → 存盘 → 读回）
          # 也在同一条链上。改这个文件当然该重跑它。
          "verify_anim_layers",
          # 2026-10-26（B2）：同一页又多了 `Animation Events  (footstep / hit /
          # sound)` 整段（八个锚点：EVENTS 段 / ADD / TRACK / CLIP / TIME /
          # KIND / PAYLOAD / REMOVE）。verify_anim_events 的 ⑥ 组就是
          # "这段 UI 真的画出来了"，⑤ 组验写入 → 存盘 → 读回。
          "verify_anim_events",
          # 2026-10-26（B3）：这一页的 `Root motion` 下拉**解除了 BeginDisabled**
          # （它在 A1 之后一直是灰的占位），下面还多了根骨输入框
          # （`INS-ANIM-ROOTBONE`，只在 mode != Off 时画）。也就是说"这个
          # 控件从此会真的改到引擎状态" —— 改这一页必须重跑根运动用例。
          "verify_root_motion",
          # 2026-10-26（C1）：Inspector 多了一整个 `sequence` 页签
          # （`INS-SEQ-HEADER/PATH/TIME/PREVIEW` 四个锚点：路径框 / 时间滑条 /
          # Preview 按钮 / 只读事件树）。verify_sequencer 的 ⑦ 组验的正是
          # "这些控件真的画出来了"，而拖动滑条走的是 `tick(sc, 0.0f)` +
          # `applyPoseToSceneCamera` —— 改这个文件必须重跑时间轴那条。
          "verify_sequencer",
          # 2026-10-26（C6 第三步）：Inspector 又多了第 13 个页签 `dialogue`
          # （`INS-DLG-HEADER/PATH/START` 三个锚点 + `INS-DLG-ASSET` 那一行
          # 结构化摘要），Add Component 菜单里也多了 `Dialogue` 项。
          # verify_dialogue_editor 验的就是"这一页真的画出来了 + 资产真的
          # 解析出 7 行台词"，改这个文件当然该重跑它。
          "verify_dialogue_editor",
          # 2026-10-27（#310）：这一页多了 "Edit script..." 按钮
          # （`INS-DLG-EDIT` 锚点）—— 剧本编辑器面板的三个入口之一，
          # 它把 `requestDialogueEditor(path)` 推给 ctx。
          # 改这里当然该重跑剧本编辑器那条（它验的第一件事就是这个入口）。
          "verify_dialogue_script",
          # 2026-10-27（#311）：`sequence` 页签多了 "Edit timeline..." 按钮
          # （`INS-SEQ-EDIT` 锚点）—— 时间轴编辑器面板的三个入口之一，
          # 它把 `requestTimelineEditor(path)` 推给 ctx。
          # 改这里当然该重跑时间轴编辑器那条（它验的第一件事就是这个入口）。
          "verify_timeline_editor",
          # 2026-10-26（#315）：头部多了一整条 prefab 栏（`INSP-PREFAB` 锚点，
          # Apply / Revert 两个小按钮）—— 它改的是 `PrefabLinkComponent` 的
          # 覆盖表（Apply 写回资产 / Revert 清覆盖），而 verify_prefab 的
          # "Apply 到底生效没有""Revert 清了几条"读的正是这里。
          "verify_prefab",
          # 2026-10-27（#316）：Transform 段最前面多了一个分叉 —— 多选时改走
          # `drawTransformMulti`（每个字段聚合一次，混合值显示 `—`／写 0·1，
          # 改动走 `batchAdd` / `batchMul` 并压成**一条** `MultiTransformEditCommand`）。
          # verify_multiselect 的 B 组断的就是 `INSP-MULTI ... pos=mixed` 那几个
          # 聚合结果，改这里当然该重跑它。
          "verify_multiselect",
          # 2026-10-27（J5）：Physics 页签末尾多了整段 `Joint`
          # （`INS-JOINT-HEADER/ADD/ENABLED/KIND/TARGET/LIMIT/MOTOR` 七个锚点）。
          # 它是**作者侧唯一**能配关节的地方 —— verify_joints 的 D 组验的就是
          # 这段画出来没有、以及"没有关节时是 Add、有了之后才是字段"这个分叉。
          "verify_joints"})
# 2026-10-28（#317）：分栏 → 视口几何。
# `editor/SplitLayout.*` 管的是"左栏多宽"，而视口的左右边界就是它算出来的：
#   · verify_viewport      —— CONVERGED_VP / V1d2 / V3 骨架全跟着分栏走；
#   · verify_viewport_bar  —— 工具条右组放不放得下（7/7 vs 3/7）也是它；
#   · verify_layout_drag   —— 根分隔条的拖动夹取（minA）；
#   · verify_layout_slots  —— 叶子面板组 / 读盘恢复；
#   · 另外三条是"按宽度排版"的历史 owner（底栏 / 三栏内容 / 材质 chip），
#     它们读到的是面板矩形，所以一并钉住。
# 判据：verify_viewport 与 verify_viewport_bar **一定**在（本轮就是给它们
# 补的映射 —— 以前改分栏不会触发视口用例，属于漏网）。
run("editor/SplitLayout.cpp → 分栏与视口几何", OLD,
    with_change("editor/SplitLayout.cpp"),
    want={"verify_bottom_bar", "verify_content_three_pane", "verify_i18n_coverage",
          "verify_layout_drag", "verify_layout_slots", "verify_material_chips",
          "verify_viewport", "verify_viewport_bar"})
# 2026-10-27（#316）：选择集本体（`editor/EditorContext.*`）。
# 期望是**并集**，而且这份并集本身就是这份 fixture 的价值：`EditorContext`
# 是十几个用例共用的中枢，改它的"选择"语义会被一长串名字一起拖下水。
# 判据是"verify_multiselect 一定在"（它是这一层新加的 owner），但把整张表
# 钉住能顺手证明另一件事：这套改动**没有**把选择语义的连带面悄悄扩大。
# ⚠ 里面那条 verify_i18n_coverage 不是噪声 —— 它扫 `editor/**` 的界面字面量，
#   而这个文件确实带 `TR()` 词条。
run("editor/EditorContext.cpp → 选择集（多选）", OLD,
    with_change("editor/EditorContext.cpp"),
    want={"verify_audio", "verify_char_locomotion", "verify_editor_play_terrain",
          "verify_i18n_coverage", "verify_material_import",
          "verify_multiselect", "verify_play_mouse_capture",
          "verify_player_feel", "verify_prefab", "verify_script_editor",
          "verify_ui_editor", "verify_viewport_bar",
          # 2026-10-27（G16-4）：`m_selectionGuid` 的快照与 `validateSelection`
          # 的接回顺序都落在这个文件里。选中接错**不会报错**，只会让属性面板
          # 显示另一个同名物体 —— 所以改这里必须重跑 identity 那条用例。
          "verify_guid",
          # 2026-10-27（P2）：全选 / 反选（`selectAll` / `invertSelection`）
          # 就在这个文件里，而菜单接线用例正是靠 `SELECT all n=` /
          # `SELECT invert n=` 两行日志判断"点了真的发生了"。
          "verify_menu_wiring",
          # 2026-10-27（J5-1）：视口拾取锚点的**状态本体**就在这个文件里
          # （`AnchorPick` 三态 + `cancelAnchorPick` / `advanceAnchorPick`），
          # 而且三个选择入口都要顺手取消拾取 —— 取消漏一处，表现是
          # "点了别的物体之后拾取还挂着，下一次点视口把锚点写到新物体上"。
          # verify_joints 的 G 组断的就是这条链路。
          "verify_joints"})
# 2026-10-27（#316）：批量数学（`editor/MultiEditMath.h`）—— 一个**纯函数**头，
# 只被 GizmoController 与 InspectorPanel 两处共用，所以 owner 应该**只有**
# verify_multiselect（外加扫字面量的 i18n 覆盖面）。
# 这条 fixture 钉的是"新加的纯函数不许偷偷挂到别人名下"：哪天有人把它
# include 进第三条链，这里会红，提醒想清楚那条链到底要不要验数值。
run("editor/MultiEditMath.h → 批量数学", OLD,
    with_change("editor/MultiEditMath.h"),
    want={"verify_multiselect", "verify_i18n_coverage"})
# 2026-10-25（§16 第八刀）：两骨 IK 的求解器本体。
# 期望里**必然**带 verify_anim_runtime / verify_character_import /
# verify_player_character —— 它们三个都登记了 `engine/animation/**` 整层。
# 判据是"自己那条一定在"，不是"只有自己那条"（与 AutoRig / ProcAnim 同理）：
# 换个角度说，`verify_ik` 单独挂在这三个名字旁边，价值在于**标明归属** ——
# 一眼能看出"改求解器是为了验 IK 的像素"，而不是"又要跑四条动画用例"。
# 2026-10-29（#336 S-C）：再加一条 verify_constraints —— 约束栈里的 IK 是
# 这个 solver 的**薄包装**，改余弦定理/极向量必须重验"入栈后与直接调用逐位
# 相同"（`constraint_ik_via_stack_bitwise`）。这条不是"顺手带上的"：它钉的是
# 两条路径只有一份实现。
run("engine/animation/TwoBoneIK.cpp → 两骨 IK", OLD,
    with_change("engine/animation/TwoBoneIK.cpp"),
    want={"verify_ik", "verify_anim_runtime", "verify_character_import",
          "verify_player_character", "verify_constraints"})
# `AnimatorSystem.*` 比上面多一个 owner：三层（LookAt → IK → 弹簧骨）的
# **施加顺序**与"perform 结束时推进弹簧骨"都在这里，`verify_player_feel`
# 的平滑过渡判据读的也是这条链 → 改它必须连它一起跑。
run("engine/animation/AnimatorSystem.cpp → 三层顺序", OLD,
    with_change("engine/animation/AnimatorSystem.cpp"),
    # ⚠ 2026-10-26（B1）：加了 verify_anim_layers —— 附加层的采样 / 权重 /
    #   淡入推进就写在 AnimatorSystem.cpp（`applyLayers`），而那张表的
    #   TRIGGERS 里登记了它。这一条正是"加一条用例必须同步 fixture"的样子。
    want={"verify_ik", "verify_anim_runtime", "verify_character_import",
          "verify_player_character", "verify_player_feel",
          "verify_anim_layers",
          # B2：`dispatchEvents` 的调用点也在这里（底座在 tick 里、附加层在
          # applyLayers 里），所以改这个文件同样要重跑事件用例。
          "verify_anim_events",
          # B3：`applyRootMotion` 的调用点也在这里（表演层之后、poseToJoints
          # 之前），`resolveRootBone` / 钳位 / 跨圈丢弃全在这一段里。
          "verify_root_motion",
          # R9：`applyNla` 的调用点也在这里，而且**位置是语义** —— 它必须
          # 在底座之后、`applyLayers` 之前。挪到后面（比如表演层之后）的
          # 症状是"拖上片段，角色的头发不再跟着抖"；挪到前面则"一有 strip
          # 角色就再也攻击不了"。两种都只有那条用例的上下带判据抓得住。
          "verify_nla"})
run("assets/shaders/fg_taa.frag → taa", OLD,
    with_change("assets/shaders/fg_taa.frag"), want={"verify_taa"})
# 2026-10-26（B1）：遮罩解算 + 分层混合的原语（`engine/animation/BoneMask.*`）。
# 期望里除自己之外必然带三条 —— 它们的 TRIGGERS 都是 `engine/animation/**`
# 整层（换姿势就等于换所有动画用例的输入）。与 verify_ik 那条同理：
# `verify_anim_layers` 出现在这里，价值在于**标明归属**。
run("engine/animation/BoneMask.cpp → 遮罩原语", OLD,
    with_change("engine/animation/BoneMask.cpp"),
    want={"verify_anim_layers", "verify_anim_runtime",
          "verify_character_import", "verify_player_character"})
# 2026-10-26（B2）：事件边界判定（`engine/animation/AnimEvent.*`）。
# 期望里带 verify_anim_runtime（28 条 `ev_*` 数值断言就在它的自检里）与
# verify_anim_events（端到端那份）。⚠ 别把 `BoneMask.cpp` 与它写成同一条 ——
# 两者的 owner 不同，合并之后"改遮罩却要跑事件用例"会变成假信号。
run("engine/animation/AnimEvent.cpp → 事件边界", OLD,
    with_change("engine/animation/AnimEvent.cpp"),
    want={"verify_anim_events", "verify_anim_runtime",
          "verify_character_import", "verify_player_character"})
# 2026-10-26（#315，G18）：预制体本体（`engine/scene/Prefab.*`）。
# 这一层的语义（资产读写 / 实例化 / 覆盖 / 同步 / Apply / Revert）只被
# `--selftest prefab` 那 35 条和 verify_prefab 两处覆盖，所以期望是**恰好
# 一条** —— 哪天有人往第二条用例上挂 `engine/scene/Prefab.*`，这条会红，
# 提醒把两边的断言分工写清楚（数值 vs 端到端）。
# ⚠ 别在这里顺带断言 SceneSerializer：它被八条用例共享（见上面那些
#   `verify_*` 的 SceneSerializer 登记），拿它当输入只会得到一大片无关名字。
run("engine/scene/Prefab.cpp → 预制体", OLD,
    with_change("engine/scene/Prefab.cpp"), want={"verify_prefab"})
# 2026-10-26（C1）：过场时间轴的解析 / 采样 / 播放器本体
# （`engine/sequence/**`）。这一层只有 verify_sequencer 一条用例登记，
# 所以期望是**恰好一条**（别的 fixture 都是"自己那条一定在"）—— 哪天有人
# 把 `engine/sequence/**` 挂到第二条用例上，这条会跟着红，提醒去同步。
# ⚠ 数值侧（48 条 `SEQ-TEST`）住在 `SequenceSelfTest.cpp`，同属一层，
#   跑 `--selftest sequence` 才执行 —— 端到端那条**不能**替代它。
# 2026-10-27（#311）起是**两条**：`SequenceAsset` 从这一版开始能**写回**
# （`toJson` / `saveSequence`），而时间轴编辑器每次改动都靠"序列化 → 丢回
# `parseSequence`"做校验 —— 存盘格式错一个字段，编辑器那条会立刻红。
# 2026-10-28（#318）起是**三条**：曲线编辑器读的是同一份 `.seq.json` 的
# ease / 手柄字段，而且 `saveSequence` 写回之后它要能原样读回（G 用例比对
# 落盘 JSON）—— 换算表与序列化口径任何一边变了，画布画出来的东西就不再
# 等于运行时采样的那一份。
# ⚠ 数值侧（`rt_*` 那 9 条往返断言）住在 `SequenceSelfTest.cpp`，跑
#   `--selftest sequence` 才执行 —— 端到端那条**不能**替代它。
# 2026-10-30 起是**四条**：多机位 marker（`shots` 数组）的解析 / 排序 /
# 区间语义 / 写回往返全在这个文件里，`verify_multi_cam` 的 A 组
# （`seq_shot_*` 24 条）和 B 组（存盘往返）都读它。
run("engine/sequence/SequenceAsset.cpp → 过场时间轴 × 轨道编辑器", OLD,
    with_change("engine/sequence/SequenceAsset.cpp"),
    want={"verify_sequencer", "verify_timeline_editor", "verify_curve_editor",
          "verify_multi_cam"})
# 2026-10-26（C2 第二步）：对话层（`engine/dialogue/**`）。
# ⚠ 数值侧（97 条 `DLG-TEST`）住在 `DialogueSelfTest.cpp`，跑
#   `--selftest dialogue` 才执行 —— 端到端那条**不能**替代它。
# ⚠ 这里挑 `DialogueSystem.cpp`（调度层）而不是 `DialogueAsset.cpp`：前者同时
#   牵动"时间轴 cue"和"组件驱动"两条路，是这一层真正的接线中枢。
# ⚠ 别改成 `DialoguePlayer.cpp` 去测"只挂一条" —— 它同理，但用调度层能顺带
#   证明"改了调度就必须重跑端到端"这件真正想钉的事。
# ⚠ 2026-10-26（C6 第三步）起是**两条**：`verify_dialogue_editor` 的页签懒加载
#   走的也是 `DialogueAsset::load`，解析口径一变 `INS-DLG-ASSET` 那行的
#   行数/选项数就变。哪天有人把编辑期那条摘掉，这条会红 —— 那是它在工作。
run("engine/dialogue/DialogueSystem.cpp → 对话 × 对话页签", OLD,
    with_change("engine/dialogue/DialogueSystem.cpp"),
    want={"verify_dialogue", "verify_dialogue_editor"})
# 2026-10-27（#310）：剧本编辑器的**写回**（`toJson` / `saveDialogue`）。
# 期望恰好三条 —— 这一层现在被三张网兜着：
#   · `verify_dialogue`        解析口径（存出来的东西必须还能被它读回）
#   · `verify_dialogue_editor` Inspector 页签那一行 `INS-DLG-ASSET`（行数/选项数）
#   · `verify_dialogue_script` 编辑器面板自己（B/D 两组直接比对落盘 JSON）
# 哪天有人把 `engine/dialogue/**` 从某条名下摘掉，这条会红 —— 那是它在工作。
run("engine/dialogue/DialogueAsset.cpp → 对话 × 两个编辑器入口", OLD,
    with_change("engine/dialogue/DialogueAsset.cpp"),
    want={"verify_dialogue", "verify_dialogue_editor", "verify_dialogue_script"})
# 2026-10-27（#310）：剧本编辑器面板本体。
# 期望两条：除了自己那条，还有 verify_i18n_coverage —— 它扫描 `editor/*.cpp`
# 里的界面字面量，而新面板一口气加了 40 条 TR 词条（缺了就是"面板一半英文"）。
# ⚠ 这条**必须**留着：新加面板最容易漏的就是词条，而漏了不崩。
run("editor/DialogueEditorWindow.cpp → 剧本编辑器 × 中文化", OLD,
    with_change("editor/DialogueEditorWindow.cpp"),
    want={"verify_dialogue_script", "verify_i18n_coverage"})
# 2026-10-27（#311）：时间轴编辑器面板本体。
# 期望两条：除了自己那条，还有 verify_i18n_coverage —— 它扫描 `editor/*.cpp`
# 里的界面字面量，而新面板一口气加了 60 条 TR 词条（缺了就是"面板一半英文"）。
# ⚠ 这条**必须**留着：新加面板最容易漏的就是词条，而漏了不崩。
# 2026-10-30：多机位 marker 的**第三轨**（机位）`@Track::Shots`）
# 和注入脚本 verbs（`addshot` / `setshotcam` / `setshotblend` /
# `setshotlabel` / `sel:shots`）都在这个文件里 —— `verify_multi_cam`
# 的 B 组就是驱动它们。所以现在是**三条**。
run("editor/TimelineEditorWindow.cpp → 时间轴编辑器 × 中文化", OLD,
    with_change("editor/TimelineEditorWindow.cpp"),
    want={"verify_timeline_editor", "verify_i18n_coverage", "verify_multi_cam"})
# 2026-10-28（#318）：曲线编辑器面板本体。
# 期望两条：除了自己那条，还有 verify_i18n_coverage —— 它扫描 `editor/*.cpp`
# 里的界面字面量，而新面板一口气加了 19 条 TR 词条（缺了就是"面板一半英文"）。
# ⚠ 这条**必须**留着：新加面板最容易漏的就是词条，而漏了不崩。
# ⚠ 期望里**没有** verify_timeline_editor：两个编辑器共享的是数据
#   （`SequenceAsset`），不是面板代码 —— 把面板本体挂到对方名下会让
#   "改曲线画布却要跑时间轴那六次启动"变成假信号。共享的那份数据由上面
#   `SequenceAsset.cpp` 那条 fixture 负责。
run("editor/CurveEditorPanel.cpp → 曲线编辑器 × 中文化", OLD,
    with_change("editor/CurveEditorPanel.cpp"),
    want={"verify_curve_editor", "verify_i18n_coverage"})
# 2026-10-28（#320）：摄影表面板本体。
# 期望两条：除了自己那条，还有 verify_i18n_coverage —— 它扫描 `editor/*.cpp`
# 里的界面字面量，而新面板一口气加了 18 条 TR 词条（缺了就是"面板一半英文"）。
# ⚠ 期望里**没有** verify_curve_editor / verify_pose_mode：三者共享的是数据
#   （`ClipEdit`）与宿主（`CharacterWindow`），不是面板代码 —— 把面板本体挂
#   到对方名下会让"改摄影表却要跑曲线那九次启动"变成假信号。共享的那两份
#   由下面 `ClipEdit.cpp` 与 `CharacterWindow.cpp` 的 fixture 负责。
run("editor/DopeSheetWindow.cpp → 摄影表 × 中文化", OLD,
    with_change("editor/DopeSheetWindow.cpp"),
    want={"verify_dope_sheet", "verify_i18n_coverage"})
# 2026-10-29（#334）：骨架编辑的数据层（`engine/animation/RigEdit.*`）。
# 实际受影响的是四条：自己 + 三条按 `engine/animation/**` 整层登记的动画用例
# （`verify_anim_runtime` / `verify_character_import` / `verify_player_character`
# —— 它们采样的是同一具骨架，改烘焙就等于改它们的输入）。
# ⚠ 期望里**没有** verify_pose_mode / verify_dope_sheet：那两条登记的是
#   `engine/animation/ClipEdit.*` 这个**具体文件**，不是整层 —— 把 RigEdit
#   塞进去会让"改骨架却要跑摄影表"变成假信号。这条 fixture 的价值正是把
#   这个边界写下来。
run("engine/animation/RigEdit.cpp → 骨架编辑 × 动画输入", OLD,
    with_change("engine/animation/RigEdit.cpp"),
    want={"verify_rig_edit", "verify_anim_runtime", "verify_character_import",
          "verify_player_character"})
# 2026-10-29（#335）：蒙皮权重本体（`engine/animation/WeightSet.*`）。
# 期望形状与上面 RigEdit 那条**一模一样**：`engine/animation/**` 整层被三条
# 动画用例登记着（`verify_anim_runtime` / `verify_character_import` /
# `verify_player_character`），所以改权重一定会进它们的行列。
# ⚠ 这条 fixture 的价值在把 `verify_weights` 标明归属：改的是"顶点组 / 自动
#   权重 / 烘焙"，验的是 Weights 页那 41 项 —— 而不是"又要跑一遍角色导入"。
run("engine/animation/WeightSet.cpp → 蒙皮权重 × 角色链", OLD,
    with_change("engine/animation/WeightSet.cpp"),
    want={"verify_weights", "verify_anim_runtime", "verify_character_import",
          "verify_player_character"})
# 2026-10-29（#336 S-C）：约束栈的求值核心（`engine/animation/Constraint.*`）。
# 它同时是"数据层 + 求值"两份职责，所以两边都得跟着跑：
#   · `verify_constraints` —— 自己那条（23 条 `constraint_*` + 8 组端到端）
#   · `engine/animation/**` 整层被四条动画用例登记着（角色链的采样输入）
# ⚠ `verify_rig_edit` 刻意**不**在这里：RigEdit 不读约束，两者只在
#   CharacterWindow 的 UI 上相邻 —— 登记进来会让"改约束"白跑一遍骨架编辑。
run("engine/animation/Constraint.cpp → 约束栈 × 角色链", OLD,
    with_change("engine/animation/Constraint.cpp"),
    want={"verify_constraints", "verify_anim_runtime", "verify_character_import",
          "verify_player_character"})
# 2026-10-29（#337 S-D）：骨骼曲线的投影/写回层（`engine/animation/CurveBake.*`）。
# 这一条的**形状很特别** —— 它同时挂在三张网里：
#   · `verify_bone_curves` —— 自己那条（32 条 `curvebake_*` + 9 组端到端）
#   · `engine/animation/**` 整层 → 三条动画用例（采样输入换了）
#   · `verify_curve_editor` **刻意不在**：相机曲线面板根本不知道 CurveBake
#     存在（两者只是共用 `animation::Curve`）。登记进来会让"改骨骼曲线"白跑
#     一遍过场相机的 73 项。
run("engine/animation/CurveBake.cpp → 骨骼曲线 × 角色链", OLD,
    with_change("engine/animation/CurveBake.cpp"),
    want={"verify_bone_curves", "verify_anim_runtime", "verify_character_import",
          "verify_player_character"})
# 2026-10-29（R9）：NLA 求值器（`engine/animation/NlaTimeline.*`）。
# 形状与上面两条一样（自己那条 + `engine/animation/**` 整层的三条），
# 价值在于把 `verify_nla` **标明归属**：改时间映射是为了验时间轴，
# 而不是"又要跑四条动画用例"。
# ⚠ `verify_anim_layers` **不在**并集里，这是刻意的：层登记的是
#   `engine/animation/BoneMask.*` 那一个文件，不是整层。而 NLA 的遮罩
#   走的是同一个 `buildBoneMask` / `blendPoseMasked` —— 真改了遮罩语义，
#   `BoneMask.*` 那条 fixture 会把层拉进来，两条用例各管各的一半。
run("engine/animation/NlaTimeline.cpp → NLA × 角色链", OLD,
    with_change("engine/animation/NlaTimeline.cpp"),
    want={"verify_nla", "verify_anim_runtime", "verify_character_import",
          "verify_player_character"})
# 2026-10-28（#318）：曲线求值本体（`engine/animation/Curve.*`）。
# 期望是"自己那条一定在"的大并集：`engine/animation/**` 整层被四条动画用例
# 登记着（换求值 = 换它们的输入），而画布画的就是同一份采样。
# 这里挑 `Curve.cpp` 而不是别的动画文件，价值在于把 `verify_curve_editor`
# **标明归属** —— 一眼能看出"改求值是为了验画布"，而不是"又要跑四条动画用例"。
# ⚠ 并集里没有 verify_ik：它登记的是 `TwoBoneIK.*` 那一类具体文件，不是整层
#   （上面那条 fixture 可以对照）。verify_sequencer 在是因为过场采样也走这份
#   求值 —— 改它等于改过场播放器看到的值。
# 2026-10-29（#337 S-D）再加 `verify_bone_curves`：骨骼那条"没缓动就逐位
# 不变"的保证，正好压在 `evalSegment` 的 Linear 分支上 —— 只要有人把它改成
# `glm::mix(v0,v1,w)`（= `a*(1-w)+b*w`），`sampleTrack` 的 `a+(b-a)*u` 就对不上，
# 而**画面上一丝变化都没有**，只有那条断言会红。
run("engine/animation/Curve.cpp → 曲线求值 × 画布", OLD,
    with_change("engine/animation/Curve.cpp"),
    want={"verify_curve_editor", "verify_timeline_editor", "verify_sequencer",
          "verify_bone_curves", "verify_anim_runtime",
          "verify_character_import", "verify_player_character"})
# 2026-10-28（#319）：Pose Mode 的数据层（`engine/animation/ClipEdit.*`）。
# `engine/animation/**` 整层被几条动画用例登记着，所以 affected 是四条并集；
# 这条 fixture 的价值是把 `verify_pose_mode` **标明归属** —— 一眼能看出
# "改打帧实现是为了验姿势模式"。
# 2026-10-28（#320）起是**五条**：三个批量操作（move / mirror / align）与
# 关键帧类型表都长在这个文件里，摄影表那条用例直接调它们 —— 改成逐帧累加
# 实现会让"相对间距逐位不变"当场红，而那正是它存在的理由。
# 2026-10-29（#337 S-D）起是**六条**：缓动表（`EditTrack::ease`）就长在这个
# 文件里，`overlayEditPose` 的签名也从 `vector<AnimTrack>` 换成了
# `vector<ResolvedTrack>`。两个直接消费者都得跟着跑 —— 姿势模式（同一条叠加链）
# 与骨骼曲线（自己那条 9 组端到端）。
run("engine/animation/ClipEdit.cpp → 骨骼打帧 × 姿势模式", OLD,
    with_change("engine/animation/ClipEdit.cpp"),
    # 2026-10-30 加 verify_gait_cycle：生成器把结果写进 `EditClip` 再落盘，
    # 而那个脚本是**从盘上读回**来当独立 oracle 的 —— 存盘格式一变，
    # "逐帧烘焙真的有 49 个键"这条判据读到的就不是同一份东西了。
    want={"verify_pose_mode", "verify_dope_sheet", "verify_bone_curves",
          "verify_anim_runtime", "verify_character_import",
          "verify_player_character", "verify_gait_cycle"})
# `Pose.cpp` 是**全动画链的地基**（采样 / FK / 混合），ClipEdit 只是叠在它
# 上面的一层。⚠ 并集里没有 verify_curve_editor / verify_timeline_editor /
#   verify_sequencer —— 它们登记的是 `engine/animation/Curve.*` 与
#   `engine/sequence/**`，不是 `engine/animation/**` 整层；把它们写进来会红。
# 2026-10-29（R9）加 `verify_nla`：`smoothFade`（淡入淡出的形状）在 R9 搬到了
# `Pose.h/.cpp` —— 层的淡入与 strip 的淡入共用这一份。改它等于同时改两条
# 淡入曲线，两边都得重验（这条 fixture 出现的理由就是"搬家也得登记"）。
run("engine/animation/Pose.cpp → 采样地基 × 姿势模式", OLD,
    with_change("engine/animation/Pose.cpp"),
    want={"verify_pose_mode", "verify_anim_runtime", "verify_character_import",
          "verify_player_character", "verify_nla"})
# `SequencerSystem.cpp` 是 **C1 / C2 / C6 的交界**：
#   · C1 组件路径（playhead / 关键帧 / 事件）
#   · C2 `kind=Line` → 一次 `cue()`
#   · C6 `cue()` 本身（脚本点播，与组件共用同一份 `tick`）
# 所以它必须同时挂在三条用例名下 —— 期望恰好三条。哪天有人只留两条
# （比如嫌"改时间轴为什么要跑脚本点播"），这条会红。
# 2026-10-30 起是**四条**：`resolveShot()` 每帧把"这一帧该用哪台机位"
# 报给宿主（`shotIndex()` / `shotCameraGuid()` / `shotBlendSeconds()`），
# `verify_multi_cam` 的 C 组断言的 `SEQ-SHOT` 就是它打的。
run("engine/sequence/SequencerSystem.cpp → 时间轴 × 对话 × 脚本导演", OLD,
    with_change("engine/sequence/SequencerSystem.cpp"),
    want={"verify_sequencer", "verify_dialogue", "verify_director_api",
          "verify_multi_cam"})
run("删除 decal.frag → 也算变更", OLD,
    with_change(drop=("assets/shaders/decal.frag",)),
    want={"verify_decals"})
run("多特性并发 → 并集", OLD,
    with_change("assets/shaders/decal.frag", "assets/shaders/fxaa.frag",
                "engine/render/GpuTimer.cpp"),
    want={"verify_decals", "verify_fxaa", "verify_gpu_timing",
          "verify_collision_play"})
run("无基准快照 → 全量", None, OLD, want=None)
# M1：改 runtime 的任意一行 → 三条运行时用例**一起**跑。
# 只挑出一条 = 假跳过（另外两条会跑在坏掉的运行时上而"通过"）。
run("runtime/GameLoop.cpp → 三条运行时用例", OLD,
    with_change("runtime/GameLoop.cpp"),
    want={"verify_runtime_build", "verify_runtime_loop", "verify_runtime_states"})
# G05 的出厂配置被 states 的冷启动断言读；G14 起它的 `audio` 块也被
# `verify_audio` 的 B 组读（`AUDIO-CONFIG ... music=0.70` 就是
# "game.json 真的生效"的唯一证据）→ 两个 owner。
run("assets/game.json → 只跑 verify_runtime_states + verify_audio", OLD,
    with_change("assets/game.json"),
    want={"verify_runtime_states", "verify_audio"})
# 阶段六/七（2026-10-22）：`assetio/**` 是**格式本体**——引擎与离线工具共用
# 同一份读写，改一处字段偏移两边一起坏，所以必须精确命中 verify_vmesh。
# 注意 `engine/assets/**`（运行时加载器 / 自检）是 GLOBAL，那条路径走全量，
# 不在这个 fixture 的期望里 —— 这也是"只登记非横切"的现场证明。
run("assetio/MeshFormat.cpp → verify_vmesh", OLD,
    with_change("assetio/MeshFormat.cpp"),
    # 2026-10-25（§13）：MESH 区块的 stride 与区块版本搬到 verify_vertex_format
    # 之后，这个文件变成**两个** owner。这不是"多跑一条以防万一"——
    # 它真的同时是格式总主（offsets/区块表，verify_vmesh 的账目判据）和
    # 顶点布局（stride/版本，verify_vertex_format 的 A1 判据）。
    # §14 又加了**第三个**：MORPH 区块的读写也在这里（verify_morph 的
    # A/B/C 三组）。三个 owner 不是设计失误 —— 这个文件就是"容器格式"
    # 本身，容器增加一个区块，账目、顶点布局、区块内部数据三者都会动。
    # §15 加了**第四个**：GPU 形变的 MORPH 区块 v2（channel 表 + offset_weights
    # 预乘）也落在这里。像素用例看起来"只关心渲染"，但它读的那份 .vmesh
    # 就是这里写出来的 —— 格式坏了它是第一个受害者（角色加载失败 = 视口空白）。
    want={"verify_vmesh", "verify_vertex_format", "verify_morph",
          "verify_morph_gpu"})
# §13 的正主：顶点布局本身的改动只该惊动顶点格式那条。但 `assetio/**`
# 被 verify_vmesh 整层登记着（格式总主），所以期望里必然带它 ——
# 判据是"自己那条一定在"，不是"只有自己那条"（与下面 AutoRig/ProcAnim 同理）。
run("assetio/Vertex.h → verify_vertex_format", OLD,
    with_change("assetio/Vertex.h"),
    want={"verify_vmesh", "verify_vertex_format"})
# `engine/assets/Mesh.h` 是属性表 / stride 的来源，属**横切**（`engine/assets/**`
# 登记为 GLOBAL）→ 期望全量。把它写成 fixture 是为了钉住"它确实横切"：
# 哪天有人把 engine/assets/** 从 GLOBAL 摘出来，这条会立刻红。
run("engine/assets/Mesh.h → 横切（全量）", OLD,
    with_change("engine/assets/Mesh.h"), want=None)
run("assetc/main.cpp → verify_vmesh", OLD,
    with_change("assetc/main.cpp"),
    # 2026-10-22 起 assetc 还挂了绑骨（--autorig）与程序化动画（--procanim）
    # 两组开关，它们各自有自己的验收脚本 → 三个 owner。
    # 2026-10-28（#329）再加 verify_clip_dedup：take 名去重后的
    # `FBX-CLIPDUP` 行是由 assetc/main.cpp **打印**的（assetio 不链 Logger，
    # 只产出文本），格式改了只有那条用例会红。
    want={"verify_vmesh", "verify_autorig", "verify_procanim",
          "verify_clip_dedup"})
# 2026-10-22：自动绑骨 / 程序化动画是两条**独立**的离线链，各自只被自己的
# 用例读。合成一条的话，"把 AutoRig 的比例表改坏"会让 procanim 也重跑，
# 反过来一样 —— 跑得多但没人对结果负责，等于没登记。
# ⚠ 期望里**必然**带 verify_vmesh：它登记了 `assetio/**` 与 `assetc/**`
#   整层（.vmesh 的字节布局是格式总主，绑骨/动画也在往里写 joints 与 clips）。
#   所以这里的判据是"自己那条一定在"，不是"只有自己那条"。
run("assetio/AutoRig.cpp → 自动绑骨", OLD,
    with_change("assetio/AutoRig.cpp"),
    want={"verify_autorig", "verify_vmesh"})
run("assetio/ProcAnim.cpp → 四段程序化动画", OLD,
    with_change("assetio/ProcAnim.cpp"),
    # 2026-10-22 P5 起它也是 verify_player_feel 的 owner：walk/run/idle 的
    # hips 垂直偏移决定"脚会不会陷进地面"，那条用例 R1 盯的就是这件事。
    # P6 再加一条 verify_player_jump：poseJump 的重写（双脚离地 / 0.75 s）
    # 和 Jump 的初速是同一件事的两半。
    want={"verify_procanim", "verify_vmesh", "verify_player_feel",
          "verify_player_jump"})
# 2026-10-22 编辑器闪退：离屏预览把主场景地形拆了（详见 verify_preview_terrain
# 的文档头）。这两个文件的 owner 就是那一条 —— 谁改预览的资源生命周期或
# 地形实例的销毁时机，都必须重跑它。
# ⚠ 期望是**并集**不是"只有我"：CharacterWindow 早已被角色导入 / 角色窗口 /
#   i18n 覆盖率三条占着（它带 TR() 字符串），RenderTarget 被碰撞 Play 与 DRS
#   占着。判据是"自己那条一定在"，不是"别人不该在"。
run("editor/CharacterWindow.cpp → 预览与地形并存", OLD,
    with_change("editor/CharacterWindow.cpp"),
    # 2026-10-22 P4 起它也是 verify_char_locomotion 的 owner（按键槽 / 绑键
    # 捕获 / `CHAR-KEYBIND` 锚点都长在这个文件里）。
    # P6 再加一条 verify_char_preview：预览叠加层（胶囊线框 / gizmo / 选中框）、
    # 鼠标点选、位置-旋转-大小控件条 —— 整块都在这个文件里。
    # R7（#319）再加 verify_pose_mode：姿势模式的面板、旋转环 gizmo、打帧、
    # 姿态库、以及 `MYVK_EDITOR_POSE_SCRIPT` 的注入点全长在这个文件里。
    # R8（#320）再加 verify_dope_sheet：`syncDopeSheet()` 与 "Dope sheet" 按钮
    # 也长在这里 —— 它决定"摄影表看的是哪一份 clip"，改错了绑定态整张表是空的。
    # S-A（#334）再加 verify_rig_edit：Rig 页、`MYVK_EDITOR_RIG_SCRIPT`、
    # 以及 `bake → 换 m_model->skeleton` 的接线都在这里。少了它，"改骨架
    # 却只跑角色窗口"会变成一条看不见的假跳过。
    # S-B（#335）再加 verify_weights：权重页、笔刷命中测试、`drawWeightsOverlay`、
    # 以及 `MYVK_EDITOR_WEIGHTS_SCRIPT` 的注入点全长在这里 —— 而且"重传顶点缓冲
    # 之前必须 `waitIdle()`"这条约定就在 `applyWeights()` 里，改错了是删除在飞缓冲
    # 的 VUID，只有 verify_weights 的 H 组会叫。
    # S-C（#336）再加 verify_constraints：约束段画在 Rig 页里，而且预览的
    # 采样链（`samplePoseInto`）里那一行 `m_constraints.evaluate(...)` 就在
    # 这个文件 —— 删掉它，约束在画面上就"完全不生效"，但所有数值断言还绿
    # （自检不走编辑器），只有 verify_constraints 的 B 组会红。
    want={"verify_character_import", "verify_character_window",
          "verify_i18n_coverage", "verify_preview_terrain",
          "verify_char_locomotion", "verify_char_preview",
          "verify_pose_mode", "verify_dope_sheet", "verify_rig_edit",
          "verify_weights", "verify_constraints",
          # GaitCycle（2026-10-30）：WALK CYCLE 那一段参数控件、生成按钮、
          # 以及 `gait:` 脚本命令都长在这里，而它与按钮共用
          # `CharacterWindow::generateWalk()` —— 接线断了就只剩自检那条
          # 纯函数路径还在绿。
          "verify_gait_cycle"})
run("engine/render/RenderTarget.cpp → 预览与地形并存", OLD,    with_change("engine/render/RenderTarget.cpp"),
    # 2026-10-25 加 verify_outline：描边的几何缓冲视图
    # （`HdrTarget::geometryView()`）就是在 RenderTarget::apply /
    # recreatePost 里跟着 recreate 传下去的 —— 漏传的表现是"第一帧有
    # 描边、resize 一次之后描边读到已释放的图"。
    want={"verify_collision_play", "verify_drs", "verify_preview_terrain",
          "verify_outline"})
# 2026-10-22 P4：移动状态机抽成 `engine/animation/Locomotion.*`，编辑器 Play
# 与 Game.exe 共用同一份。它错了的表现是"腿不动"或"停下不播 idle"——只有
# verify_char_locomotion 看得见，所以它必须是 owner 之一。
# ⚠ 期望里必然带 verify_anim_runtime / verify_character_import /
#   verify_player_character：它们都登记了 `engine/animation/**` 整层。
#   判据仍是"自己那条一定在"，不是"只有自己那条"。
run("engine/animation/Locomotion.cpp → 默认角色移动", OLD,
    with_change("engine/animation/Locomotion.cpp"),
    # 2026-10-22 P5 起多一条 owner：crossfade 的 `blendSeconds` 与"切 clip 时
    # 记下旧播放头"都长在这个文件里，verify_player_feel 的 R7 盯的就是它。
    # P6 再加 verify_player_jump：jump 态的进入/退出与落地回 idle 也在这台
    # 状态机里（跳跃物理在别处，但"什么时候算跳"在这儿）。
    # 2026-10-30 A4 再加 verify_gait_cycle：步幅匹配把"移动速度 → 播放速率"
    # 写进 `sk.speed` 的那段代码就在本文件第 6 节，而那条链的验收（真实资产
    # 上的自然步速 + UI 锚点）住在 gait 用例里。
    want={"verify_char_locomotion", "verify_anim_runtime",
          "verify_character_import", "verify_gait_cycle",
          "verify_player_character", "verify_player_feel",
          "verify_player_jump"})
# 2026-10-23 P11：默认角色换成 Alicia Solid（128 骨 / 4 段 clip，见
# docs/alicia_import.md）。**`kDefaultCharacterPath` 这个常量本身长在
# editor/EditorScene.cpp 里** —— "换默认角色"完全可以只改这一个文件、一点
# 都不碰 assets/models/characters/，所以这个文件的 owner 必须包含
# verify_char_locomotion（它的 C0/C1c 就是拿这个常量去对资产的）。
# ⚠ 期望是并集：同一文件还管 createPrimitiveOnGround 的落点（verify_player /
#   verify_collision_play / verify_drop_to_floor / verify_model_scale）和
#   一批 TR() 词条（verify_i18n_coverage）。
run("editor/EditorScene.cpp → 默认角色装配", OLD,
    with_change("editor/EditorScene.cpp"),
    want={"verify_char_locomotion", "verify_character_import",
          "verify_collision_play", "verify_drop_to_floor",
          # 2026-10-24：`spawnPlayerAtGameStart` 住在这个文件里 —— 编辑器 Play
          # 时才生成玩家实体（PLAYER-SPAWN 锚点），
          # verify_editor_play_terrain 的 T1b 就是断它。
          "verify_editor_play_terrain",
          "verify_i18n_coverage", "verify_model_scale", "verify_player",
          "verify_player_feel"})
# ⚠ 按键翻译表 `engine/ui/KeyCodes.*` **故意不做夹具**：`engine/ui/**` 在
#   `GLOBAL_GLOBS` 里，改它走全量，登记进 TRIGGERS 反而会在 --narrow 下
#   假跳过（check_regression_map 的 [E] 段会点名）。这里留一条反向断言，
#   把这个事实钉住 —— 哪天有人把 KeyCodes 从横切里摘出来，得知道要补映射。
run("engine/ui/KeyCodes.cpp → 横切（本来就全量，不该登记）", OLD,
    with_change("engine/ui/KeyCodes.cpp"),
    want=None)

print("\n[B] 快照覆盖范围（真实扫描仓库）")
s = rr.source_snapshot()
print("  条目数 = %d" % len(s))
bad = [k for k in s if k.startswith(("assets/scenes/", "assets/scripts/",
                                     "tools/", "docs/", ".workbuddy/",
                                     "build/"))]
print("  ⚠ 不该进快照的条目：%s" % (bad if bad else "（无）"))
if bad:
    fails.append("快照混进运行时数据")
for probe in ["engine/render/DecalBuffer.cpp", "assets/shaders/decal.frag",
              "editor/InspectorPanel.cpp", "CMakeLists.txt"]:
    if probe not in s:
        print("  ⚠ 快照缺 %s" % probe)
        fails.append("快照缺 " + probe)

print("\n[C] 映射覆盖率")
no_trig = rr.impact_analysis(OLD, dict(OLD))["no_trigger"]
print("  已登记 %d 例 / 共 %d 例" % (len([n for n in rr.DEFAULT
                                            if n in rr.TRIGGERS]),
                                     len(rr.DEFAULT)))
print("  无条件重跑（没登记 TRIGGERS）：%s" % (", ".join(no_trig) or "（无）"))
missing = [n for n in rr.DEFAULT if not os.path.exists(
    os.path.join(TOOLS, n + ".py"))]
if missing:
    print("  ⚠ DEFAULT 里有脚本不存在：%s" % missing)
    fails.append("DEFAULT 列了不存在的脚本")

print("\n[D] 死映射（写了不存在、又不带通配的路径）")
dead = []
for c, gl in rr.TRIGGERS.items():
    for g in gl:
        if any(ch in g for ch in "*?"):
            continue
        if not os.path.exists(os.path.join(REPO, g.replace("/", os.sep))):
            dead.append("%s: %s" % (c, g))
print("  %s" % (dead if dead else "（无）"))
if dead:
    fails.append("死映射")

print("\n[E] 全横切登记（globs 全落在 GLOBAL_GLOBS → --narrow 下会假跳过）")
fully_dead, redundant = [], []
for c, gl in rr.TRIGGERS.items():
    n_global = 0
    for g in gl:
        probe = g.replace("**", "probe/leaf").replace("*", "z").replace("?", "z")
        if rr._match_any(rr.GLOBAL_GLOBS, probe):
            n_global += 1
            redundant.append("%s: %s" % (c, g))
    if n_global == len(gl):
        fully_dead.append(c)
if redundant:
    print("  冗余的横切 glob（只是多余，无害）：")
    for r in redundant:
        print("      " + r)
print("  整条都指向横切文件的用例：%s" % (", ".join(fully_dead) or "（无）"))
if fully_dead:
    fails.append("全横切登记: " + ", ".join(fully_dead))

print("\n[F] RENDER_MODULES 完整性"
      "（engine/render 下每个文件要么在 GLOBAL_GLOBS、要么在 RENDER_MODULES）")
rnd = os.path.join(REPO, "engine", "render")
uncovered = []
for fn in sorted(os.listdir(rnd)):
    rel = "engine/render/" + fn
    if rr._match_any(rr.GLOBAL_GLOBS, rel):
        continue
    if rr._match_any(rr.RENDER_MODULES, rel):
        continue
    uncovered.append(rel)
print("  没被覆盖的 render 文件：%s"
      % (uncovered if uncovered else "（无 —— 新模块会被这里抓出来）"))
if uncovered:
    fails.append("RENDER_MODULES 漏列: " + ", ".join(uncovered))
leakgate = rr.TRIGGERS.get("verify_collision_play", [])
missing = [g for g in rr.RENDER_MODULES if g not in leakgate]
if missing:
    print("  ⚠ 泄漏门没挂上这些模块：%s" % missing)
    fails.append("泄漏门漏挂模块")

print("\n结果: %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
sys.exit(1 if fails else 0)
