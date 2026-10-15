"""跑一轮回归：逐个启动 GUI 验证脚本，记录退出码与结果行。

GUI 测试要独占窗口，只能串行。而且**必须以退出码为准** —— 这批脚本的
结果行格式不统一（`RESULT: PASS` / `全部断言 PASS` / `全部 PASS`），
只靠文本匹配会漏判。失败时把该脚本的日志尾部打出来，省得再跑一遍。

=================================================================
增量：两道闸，**只跑本次改动真正关联到的用例**
=================================================================

全套 39 例要 30 分钟以上，其中绝大多数是几十~两百秒的 GUI 用例。问题是
"改了某一块渲染功能"以后，本来只需要重跑相关的几例。

第一道闸（**用例脚本**）：该用例的 .py + 它递归 import 的本地 tools 模块
没变 → 它的"判定逻辑"没变。

第二道闸（**变更影响分析**）：把仓库里的**源码**拍一份快照存进缓存，下次
运行时 diff 出"自上次回归以来改过哪些文件"，再按下面的表把文件映射成
**关联用例集**；只有落在这个集合里的用例才重跑。

    ┌─ 变更文件 ─┬─ 命中 GLOBAL_GLOBS ──→ 全量重跑（横切，安全兜底）
    │            ├─ 命中 TRIGGERS      ──→ 只重跑它登记的那些用例
    │            ├─ 命中 IGNORE        ──→ 无视（文档/记忆/编译辅助脚本）
    │            └─ 谁都没命中          ──→ ⚠ 全量重跑 + 点名（提示补表）

⚠ **"未登记"一律按全量处理**，绝不静默跳过 —— 这张映射表的价值是"把
  已经确认过归属的文件从全量里摘出来"，不是"猜哪些不重要"。所以它可以
  慢慢补：补得越多越快，没补的部分退化成今天的行为，**不会漏**。

⚠ **横切文件（GLOBAL_GLOBS）一改就全跑**：`CMakeLists.txt`、`engine/rhi/**`、
  `engine/render/Renderer.cpp`（所有 pass 的接线中枢）、`RenderTypes.h`
  （FrameUBO/set0 契约）、`pbr.frag`（前向主着色器）等等。文件粒度的映射
  看不出"Renderer.cpp 里改的是哪一段"，所以只能保守。想在"确实只动了一个
  特性的代码"时跳过这层，用 `--narrow`（见下）。

⚠ **新增文件（`+`）也会参与分析**：没人登记它就是"未登记" → 全跑。所以加
  新的 .cpp/.frag 时顺手补一行 TRIGGERS。

⚠ 快照**只覆盖构建输入**（源码/着色器/模型），**不含运行时数据**：
  `assets/scenes/**`、`assets/scripts/**` 是编辑器运行时自己会写的目录，
  放进快照会导致"每轮都算变了"。

用法：
    python tools/run_regression.py                      # 跑默认全套（只重跑关联用例）
    python tools/run_regression.py --explain            # 只看影响分析，不跑
    python tools/run_regression.py --all                # 无视一切缓存，全跑
    python tools/run_regression.py --narrow             # 横切文件不触发全量（作者显式断言）
    python tools/run_regression.py bvh player add_menu  # 只跑指定的几个（总是跑）
    python tools/run_regression.py --tail 60 bvh        # 只看尾部行数
    python tools/run_regression.py --no-cache           # 不读也不写缓存
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
# 单测完整日志的落盘目录（和截图产物放一起）
LOG_DIR = os.path.join(os.path.dirname(ROOT), "_venn_verify", "_regression")
CACHE_PATH = os.path.join(LOG_DIR, "_cache.json")
# 产物目录：exe + 被拷过去的 shader/材质/模型/场景
BIN_DIR = os.path.join(ROOT, "build", "bin")
TOOLS_DIR = os.path.join(ROOT, "tools")

DEFAULT = [
    "verify_frustum_cull", "verify_bvh", "verify_lod", "verify_ibl",
    "verify_csm", "verify_local_shadow", "verify_shadow_hardening",
    "verify_volumetric_fog", "verify_light_modifiers",
    "verify_shader_variants",
    "verify_shader_hotreload", "verify_fxaa", "verify_instancing",
    # R4-1：AlphaMode::Mask（alpha 裁切）+ 抖动裁切
    "verify_alpha_mask",
    # R4-2：预积分 SSS（ShadingModel::Skin，bit4 VENN_SHADING_SKIN）
    "verify_sss",
    "verify_nav_gizmo",
    "verify_character_window", "verify_add_menu", "verify_mesh_params",
    "verify_collider_gizmo", "verify_player", "verify_collision_play",
    "verify_drop_to_floor", "verify_material_roundtrip",
    "verify_primitive_roundtrip", "verify_screenspace", "verify_textures",
    # 多相机 / 通用 RenderTarget + 相机特效（DoF / 运动模糊 / 色差 / 暗角 /
    # 后处理体积）
    "verify_camfx",
    "verify_gpu_timing",
    "verify_viewmodes",
    "verify_taa",
    "verify_auto_exposure",
    "verify_color_lut",
    "verify_drs",
    # Hi-Z 遮挡剔除（R2-1）：深度金字塔 + AABB 遮挡测试 + 两帧延迟回读
    "verify_hiz",
    # Multi-draw indirect + GPU 紧凑化（R2-2）：间接命令 + GPU 写 draw 参数
    "verify_mdi",
    # Bindless / descriptor indexing（R2-3）：纹理数组 + 材质页，per-draw
    # 只 push 页号、零描述符重绑（开关都必须逐像素回到基线）
    "verify_bindless",
    # POM 视差遮挡贴图（R4-3）：heightMap 槽 + 切线空间层步进 ray-march，
    # bit5 VENN_PARALLAX（heightScale=0 / 全局开关 = 逐像素回到基线）
    "verify_pom",
    # 延迟贴花 / DBuffer（R4-4）：深度预通道 → 盒体 pass 写三张 DBuffer
    # （反照率 / 世界法线 / 粗糙度-金属度）→ 前向按 coverage 混合。
    # MYVK_DECALS 默认关；关 / enabled=false / opacity=0 / 盒体悬空
    # 四条都必须逐像素回到基线。
    "verify_decals",
    # 反射探针（R5-1）：探针盒 → 6 面相机捕获 → 逐面拷进立方体 → GGX 预过滤
    # （roughness = mip）→ 片元落在盒内时替换 IBL 的预过滤项。MYVK_PROBES
    # 默认关；关 / enabled=false 两条都必须逐像素回到基线；场地是"封闭屋子 +
    # 镜面球 + 相机背后的自发光面板"，球心那个像素直接读"探针有没有烘进
    # 相机看不到的那面墙"。
    "verify_reflection_probe",
    # GPU 粒子系统（R6-1）：compute 模拟（spawn/kill/固定步长积分/曲线/噪声）
    # + 计数与可选桶排序 + 两个 indirect draw（加色 / Alpha）+ 软粒子。
    # MYVK_FX 默认关；关 / 开了但无发射器两条都必须逐像素回到基线；
    # 冻结帧（MYVK_FX_FREEZE）同配置两次逐像素一致是"确定性"的硬证据；
    # 面板结构与 12 个模块的字段清单走 FX-PARAMS 断言。
    "verify_particles",
    # ---- 阶段 C：核心面板（E11 / E13 / E14）----
    # E11 Inspector：分区折叠**跨实体**保持 / 组件搜索过滤 / Copy-Paste-Reset
    # 往返（POD 字节快照经可撤销命令写回）
    "verify_inspector",
    # E13 ContentBrowser：类型过滤矩阵 / 列表与四档缩略图尺寸 / 拖资产到
    # 视口（贴地）与 Hierarchy（实例化）两条落点
    "verify_content_browser",
    # E14 Viewport：四角浮层（模式条 / 统计 HUD / 相机信息 + 9 书签 / 导航球）、
    # NO_HUD 两次逐像素一致 + 与改造前参考图一致、四浮层全关 == NO_HUD、
    # Shift+数字 快捷键、Viewport Display 右键菜单显隐、偏好落盘跨进程往返
    "verify_viewport",
    # ---- 阶段 D：交互理念 ----
    # E18 命令注册表 + 命令面板：注册表规模 / Ctrl+Shift+P 打开 /
    # 模糊搜索（query=ssr 榜首是 feature.toggle.ssr）/ 执行真生效
    # （CMD-RUN + FTR-UI）/ 无匹配给"最接近"建议
    "verify_command_palette",
    # E19 撤销覆盖设置类改动：改一次进历史 / 500ms 合并（depthDelta=0）/
    # undo 回旧值 redo 回新值 / Float 与 Bool 两类 kind 都接进来了
    "verify_undo_settings",
    # E21 通知 / 进度：注入 ERROR → 状态栏 Error 计数 + Console 能定位（点
    # toast → NOTIF-CLICK/NOTIF-FOCUS）/ 进度任务跑完与取消 / Info 不自动升级
    "verify_notifications",
    # ---- 阶段 E：生产力 ----
    # E25 GPU 时戳时间轴：时间轴总长 == GPU-TIMING 帧总耗时（< 5%）/ 60 帧
    # 历史封顶 / 点段联动 Console 过滤 / 归入 Rendering 工作区 / 采样纯读
    "verify_gpu_timeline",
]

# 结果行：兼容四种写法。取**最后一条**，因为有的脚本会先打一堆中间断言。
OK_RE = re.compile(r"(?:RESULT:\s*PASS|全部断言 PASS|^全部 PASS\b|结果:\s*PASS)")
BAD_RE = re.compile(r"(?:RESULT:\s*FAIL|断言[^\n]*FAIL|结果:\s*FAIL)")
# 资源泄漏：验证层在 vkDestroyDevice() 时点名没被销毁的子对象。
LEAK_RE = re.compile(r"leaked objects that have not been destroyed")

# ============================================================
# 变更影响分析：监视哪些目录 / 文件怎么映射到用例
# ============================================================

# 快照覆盖的**构建输入**（相对 ROOT）。只放"改了会影响 build/bin 产物"的
# 东西；运行时数据（assets/scenes、assets/scripts）**不能**进来 ——
# 编辑器自己会写它们，进了快照就每轮都判成"变了"。
WATCH_ROOTS = [
    "engine",
    "editor",
    "assets/shaders",
    "assets/models",
    "assets/textures",
    "CMakeLists.txt",
    "Makefile",
    "make.cmd",
]

# 一改就必须全量重跑的**横切**文件。
# 判据：它被几乎所有用例间接依赖，或者它是"多处必须逐字对齐"的契约来源。
GLOBAL_GLOBS = [
    # 构建系统：变体表 / shader 编译列表 / 关键字 -D 全在这里生成
    "CMakeLists.txt", "Makefile", "make.cmd", "cmake/**",
    # 基础设施层：所有 pass 都压在它上面
    "engine/core/**", "engine/rhi/**",
    # FrameUBO / set0 描述符 / 变体位序 —— 三处逐字对齐的契约
    "engine/render/RenderTypes.h",
    "engine/render/ShaderKeywords.*",
    "engine/render/ShaderPath.*",
    "engine/render/VariantPipelines.*",
    "engine/render/FrameGraph.*",
    "engine/render/FrameResources.h",
    # 接线中枢：所有 pass 的录制 / 描述符写入 / 统计都在这里，
    # 文件粒度看不出改的是哪一段 → 只能保守。想跳过用 --narrow。
    "engine/render/Renderer.cpp", "engine/render/Renderer.h",
    # 特性注册表（E01）：**每个**特性的 env 初值都从这里的 applyEnv()
    # 走一遍。改一条解析分支 = 改某个特性在测试里的启动值 → 横切。
    # （往 kFeatures[] 加新参数其实不影响既有行为，但文件粒度分不出来，
    #   按"宁可多跑"处理；想精确跳过用 --narrow。）
    "engine/render/FeatureRegistry.*",
    # 场景 / ECS 容器：几乎每个用例的场地都由它搭
    "engine/scene/Scene.h", "engine/scene/Scene.cpp",
    "engine/scene/Transform.h", "engine/ecs/**", "engine/ui/**",
    # 资产层：Mesh / Texture / Material / AssetManager 是公共入口
    "engine/assets/**",
    # 物理：player / collision / drop_to_floor 三个用例，但 Scene 也用它
    "engine/physics/**",
    # 共享着色器片段：被全部 .frag/.vert include
    "assets/shaders/common.glsl",
    "assets/shaders/space.glsl",
    # 前向主着色器：所有不透明物体都走它
    "assets/shaders/pbr.frag",
    "assets/shaders/pbr.vert",
    "assets/shaders/fullscreen.vert",
    "assets/shaders/depth_only.frag",
    # ⚠ `assets/models/**` 与 `assets/textures/**` **故意不在这里**：
    #   它们只被个别用例读（模型 → character_window / lod；贴图 → sss 的
    #   LUT、ibl 的 brdf LUT…），放进横切清单会把所有图形用例一起拖下水。
    #   留在"未登记"状态的效果一样安全（没登记 → 全量 + 点名），
    #   而且还多一层提示：看到名字就知道该往 TRIGGERS 里补哪一条。
]

# 改了**不影响任何用例**的东西（文档 / 记忆 / 编译辅助脚本）。
IGNORE_GLOBS = [
    "docs/**",
    ".workbuddy/**",
    ".github/**",
    "assets/shaders/compile.bat",
    "assets/shaders/compile.sh",
]
IGNORE_SUFFIXES = (".md", ".spv", ".json.bak", ".orig", ".rej")

# 渲染层的**逐个模块** glob（= `engine/render/` 下非横切的全部 .cpp/.h）。
#
# ⚠ 刻意**不写** `engine/render/**`：那样会把"新增的、还没人认识的 render
#   文件"也判成"已登记"，从而**绕过**"未登记 → 全量"这条兜底 —— 新模块
#   本该触发全量，结果只跑了一个用例。逐个列出来就没有这个洞：
#   新加模块仍是"未登记" → 全量（安全）。
# `check_regression_map.py` 的 [F] 项会自动核对"engine/render 下的每个文件
# 要么在 GLOBAL_GLOBS、要么在这里"，所以漏列会被抓出来。
RENDER_MODULES = [
    "engine/fx/ParticleSystem.*",
    "engine/render/Bindless.*",
    "engine/render/CameraEffects.h",
    "engine/render/ClusteredLighting.*",
    "engine/render/DecalBuffer.*",
    "engine/render/Environment.*",
    "engine/render/Frustum.*",
    "engine/render/GpuTimer.*",
    "engine/render/HdrTarget.*",
    "engine/render/HiZOcclusion.*",
    "engine/render/LightTextureLibrary.*",
    "engine/render/LocalShadowPass.*",
    "engine/render/MdiDraw.*",
    "engine/render/OffscreenImage.h",
    "engine/render/PostProcess.*",
    "engine/render/ReflectionProbe.*",
    "engine/render/RenderTarget.*",
    "engine/render/SSAOPass.*",
    "engine/render/ScreenSpaceSettings.h",
    "engine/render/ShaderHotReload.*",
    "engine/render/ShadowCull.h",
    "engine/render/ShadowPass.*",
    "engine/render/VolumetricFog.*",
]

# 用例 ← 触发它的源码/着色器。
#
# ⚠ 只登记"我确认归属"的模块。没登记的用例 = 无条件重跑（安全退化），
#   所以这张表可以增量补，补漏不会漏测。`--explain` 会把"没登记的用例"
#   和"没登记的文件"都点名列出来，照着补即可。
#
# ⚠ **不要登记横切文件**：命中 GLOBAL_GLOBS 的文件永远先按"全量"处理，
#   写在这里也是死代码；更糟的是 `--narrow` 下它会变成一条"看着有归属、
#   其实没覆盖"的假映射，把该跑的用例静默跳过。所以下面这几例
#   **故意不登记**（它们的实现主体全在横切文件里）→ 无条件重跑：
#     verify_lod          —— LOD 选择在 Renderer.cpp/Renderer.h
#     verify_instancing   —— instancing 是 Renderer + pbr.vert 的路径
#     verify_alpha_mask   —— Mask 走 push.skin.w + pbr.frag
#     verify_pom          —— 位移在 pbr.frag::parallaxUv
#     verify_textures     —— Texture/AssetManager 在 engine/assets/**
#     verify_shader_variants —— 变体表由 ShaderKeywords/CMakeLists 生成
TRIGGERS = {
    # ---- 剔除 / LOD / 批次 ----
    "verify_frustum_cull":   ["engine/render/Frustum.*", "engine/scene/Bvh.*"],
    "verify_bvh":            ["engine/scene/Bvh.*"],
    "verify_mdi":            ["engine/render/MdiDraw.*",
                              "assets/shaders/mdi_compact.comp"],
    "verify_hiz":            ["engine/render/HiZOcclusion.*",
                              "assets/shaders/hiz_build.comp",
                              "assets/shaders/hiz_downsample.comp",
                              "assets/shaders/hiz_test.glsl",
                              "assets/shaders/occlusion_test.comp"],
    "verify_bindless":       ["engine/render/Bindless.*"],
    # ---- 阴影 ----
    "verify_csm":            ["engine/render/ShadowPass.*",
                              "engine/render/ShadowCull.h",
                              "assets/shaders/shadow.vert",
                              "assets/shaders/shadow.frag"],
    "verify_local_shadow":   ["engine/render/LocalShadowPass.*"],
    "verify_shadow_hardening": ["engine/render/ShadowPass.*",
                                "assets/shaders/shadow.vert"],
    # ---- 环境 / IBL / 雾 ----
    "verify_ibl":            ["engine/render/Environment.*",
                              "assets/shaders/ibl_brdf.comp",
                              "assets/shaders/ibl_irradiance.comp",
                              "assets/shaders/ibl_prefilter.comp",
                              "assets/shaders/sky_cube.comp"],
    "verify_volumetric_fog": ["engine/render/VolumetricFog.*",
                              "assets/shaders/fog_inject.comp",
                              "assets/shaders/fog_integrate.comp"],
    "verify_light_modifiers": ["engine/render/LightTextureLibrary.*",
                               "engine/render/ClusteredLighting.*"],
    # ---- 屏幕空间 / 后处理链 ----
    "verify_screenspace":    ["engine/render/SSAOPass.*",
                              "engine/render/PostProcess.*",
                              "assets/shaders/ssao.frag",
                              "assets/shaders/ssao_blur.frag",
                              "assets/shaders/fg_ssgi.frag",
                              "assets/shaders/fg_ssr.frag",
                              "assets/shaders/fg_godrays.frag"],
    "verify_camfx":          ["engine/render/CameraEffects.h",
                              "engine/render/PostProcess.*",
                              "assets/shaders/fg_dof.frag",
                              "assets/shaders/fg_motionblur.frag",
                              "assets/shaders/fg_godrays.frag"],
    "verify_taa":            ["assets/shaders/fg_taa.frag",
                              "engine/render/PostProcess.*"],
    "verify_fxaa":           ["assets/shaders/fxaa.frag"],
    "verify_auto_exposure":  ["engine/render/PostProcess.*",
                              "assets/shaders/fg_lum_average.comp",
                              "assets/shaders/fg_lum_downsample.frag",
                              "assets/shaders/fg_lum_histogram.comp"],
    "verify_color_lut":      ["engine/render/PostProcess.*",
                              "assets/shaders/post.frag"],
    "verify_drs":            ["engine/render/PostProcess.*",
                              "engine/render/HdrTarget.*",
                              "engine/render/RenderTarget.*",
                              "engine/render/OffscreenImage.h"],
    "verify_viewmodes":      ["engine/render/ScreenSpaceSettings.h"],
    # ---- 变体 / 热重载 / 时戳 ----
    # （verify_shader_variants 不登记：它的实现主体是 ShaderKeywords.* /
    #   VariantPipelines.* / CMakeLists.txt，全是横切文件 → 无条件重跑）
    "verify_shader_hotreload": ["engine/render/ShaderHotReload.*"],
    "verify_gpu_timing":     ["engine/render/GpuTimer.*"],
    # ---- 材质特性 ----
    "verify_sss":            ["assets/textures/sss_lut*"],
    "verify_decals":         ["assets/shaders/decal.frag",
                              "assets/shaders/decal.vert",
                              "engine/render/DecalBuffer.*",
                              "engine/render/ScreenSpaceSettings.h"],
    # R5-1 反射探针：捕获/预过滤/盒投影整链都在 ReflectionProbe.*，片元侧的
    # 混合在 pbr.frag（已在横切表里）；组件与序列化不在 TRIGGERS 里是因为
    # 它们落在 SceneSerializer / Components.h —— 那两个本来就是横切。
    "verify_reflection_probe": ["engine/render/ReflectionProbe.*"],
    # R6-1 粒子系统：模拟/计数/绘制三条着色器 + 资源与描述符池全在
    # ParticleSystem.*；组件与序列化落在 Components.h / SceneSerializer.cpp
    # （本身就是横切，不登记）；面板在 InspectorPanel.* —— 那是横切文件，
    # 所以面板那一组（D/E）实际上是无条件重跑，这是安全的退化。
    "verify_particles":      ["engine/fx/ParticleSystem.*",
                              "assets/shaders/particle_sim.comp",
                              "assets/shaders/particle_count.comp",
                              "assets/shaders/particle_common.glsl",
                              "assets/shaders/particle.vert",
                              "assets/shaders/particle.frag"],
    # ---- 编辑器 / UI ----
    "verify_nav_gizmo":      ["editor/ViewportPanel.*",
                              "editor/GizmoController.*",
                              "engine/scene/Camera.*"],
    "verify_collider_gizmo": ["editor/ColliderView.*",
                              "editor/GizmoController.*"],
    "verify_add_menu":       ["editor/Toolbar.*", "editor/PrimitiveBuilder.*",
                              # E04/E05：View ▸ Panel / Window ▸ Panel 这些菜单项
                              # 是**从面板表生成**的，工作区子菜单也从预设生成 ——
                              # 表或预设改一个字，这一例的菜单断言就可能变。
                              "editor/PanelRegistry.*", "editor/Workspace.*"],
    "verify_mesh_params":    ["editor/InspectorPanel.*"],
    "verify_character_window": ["editor/CharacterWindow.*", "assets/models/**"],
    "verify_material_roundtrip": ["engine/scene/SceneSerializer.*"],
    "verify_primitive_roundtrip": ["engine/scene/SceneSerializer.*"],
    "verify_player":         ["editor/EditorScene.*"],
    # ⚠ 这一例同时是**资源泄漏门**：全套里只有它会让编辑器**优雅退出**，
    #   因而也只有它的日志里会出现 `vkDestroyDevice(): ... leaked objects`
    #   （其余脚本都是强杀进程，泄漏报告根本不会产生）。
    #   所以它必须跟着**整个渲染层**跑 —— 任何 render 模块少了一个
    #   vkDestroy*（image/view/memory/framebuffer…）只有它能发现。
    #   R4-4 的 DecalBuffer 析构漏调 destroyFrames()、漏掉 20 个对象，
    #   就是这么抓到的。挂 `RENDER_MODULES`（逐个模块，不是 `engine/render/**`）
    #   是**放宽**方向（多跑），不存在假跳过风险。
    "verify_collision_play":  ["editor/EditorScene.*"] + RENDER_MODULES,
    "verify_drop_to_floor":  ["editor/EditorScene.*"],
    # ---- 阶段 C：核心面板 ----
    # E11：分区折叠表 / 关键词表 / POD 快照全在 InspectorPanel 里
    "verify_inspector":      ["editor/InspectorPanel.*"],
    # E13：过滤 / 面包屑 / 视图模式 / 缩略图档位在 ContentBrowser，
    #      缓存淘汰在 ThumbnailCache，拖放载荷定义在 EditorDragDrop.h
    "verify_content_browser": ["editor/ContentBrowser.*",
                               "editor/ThumbnailCache.*",
                               "editor/EditorDragDrop.h"],
    # E14：四角浮层 / 书签 / 显隐全在 ViewportPanel；相机 yaw/pitch 存取走
    #      引擎的 scene::Camera（书签存的就是它）
    "verify_viewport":       ["editor/ViewportPanel.*",
                              "engine/scene/Camera.*"],
    # ---- 阶段 D：交互理念 ----
    # E18：注册表本体 + 模糊匹配 + 命令面板 UI 全在新文件 CommandRegistry；
    #      命令的**登记**（从特性/面板/视图模式/工作区/控制台五路派生）在
    #      EditorApp::registerEditorCommands。这里只登记前者 —— EditorApp.cpp
    #      未登记（改它 → 全量），是安全的保守行为。
    "verify_command_palette": ["editor/CommandRegistry.*"],
    # E19：设置类改动的撤销 —— 落地在 Command.h/.cpp（ParamEditCommand）+
    # FeatureUIGen（IsItemActivated/IsItemDeactivatedAfterEdit 钩子）。
    #       EditorApp::onFeatureParamUndo 是"宿主侧合并"的实现，但
    #       EditorApp.cpp 未登记（改它 → 全量），保守正确。
    "verify_undo_settings": ["editor/Command.*"],
    # E21：通知 / 进度全在本文件对（Toast / 历史 / 任务条 / 落盘 / 状态栏锚点）。
    #       宿主接线在 EditorApp.cpp（未登记 → 全量）；新增的 Logger hook 通道
    #       在 engine/core/Logger.h —— 它落在 GLOBAL_GLOBS 里（engine/core/**），
    #       所以那一侧一改本来就全量，不必也不该写进 TRIGGERS（写了会在
    #       --narrow 下假跳过）。
    "verify_notifications": ["editor/NotificationCenter.*"],
    # ---- 阶段 E：生产力 ----
    # E25：时间轴本体在新文件；Console 的 setSearchFilter（T4d 的
    #       CONSOLE-FILTER 锚点）也在本用例的断言链上，一并登记。
    #       宿主接线（采样/钩子/面板表一行/工作区清单）在 EditorApp.cpp 与
    #       Workspace.cpp —— 两者都未登记（改它们 → 全量），保守正确。
    "verify_gpu_timeline": ["editor/GpuTimelinePanel.*",
                            "editor/ConsolePanel.*"],
}


def _stat_sig(path):
    try:
        st = os.stat(path)
    except OSError:
        return b"<gone>"
    return ("%d|%d" % (st.st_size, st.st_mtime_ns)).encode()


def _stat_pair(path):
    """快照里存的 (大小, mtime_ns)。mtime 会让"只 touch 没改内容"也算变更
    —— 这是**故意**的（保守方向），比漏报安全。"""
    try:
        st = os.stat(path)
    except OSError:
        return None
    return [st.st_size, st.st_mtime_ns]


_LOCAL_IMPORT_RE = re.compile(r"^\s*(?:from|import)\s+([A-Za-z_]\w*)", re.M)


def _local_deps(path, seen):
    """递归收集该脚本 import 的**本地** tools 模块（`from verify_x import ...`）。

    只看本地：`numpy` / `PIL` 之类在 tools/ 下没有同名 .py，自然被过滤掉。
    """
    try:
        with open(path, "r", encoding="utf-8") as fh:
            src = fh.read()
    except OSError:
        return
    for mod in _LOCAL_IMPORT_RE.findall(src):
        sub = os.path.join(TOOLS_DIR, mod + ".py")
        if os.path.exists(sub) and sub not in seen:
            seen.add(sub)
            _local_deps(sub, seen)


def test_fingerprint(name):
    """**单个用例**的指纹：该脚本 + 它（递归）import 的本地 tools 模块。

    为什么要分到这一层：修一个脚本（比如给某个 flaky 断言加重试）不该让
    另外 30 多个用例白跑一遍。
    `run_regression.py` 自己**不参与任何用例的指纹**：它只是"跑法"
    （跳过策略 / 输出格式 / 影响分析表），改了它不影响任何用例的判定结果。
    """
    files = {os.path.join(TOOLS_DIR, name + ".py")}
    _local_deps(os.path.join(TOOLS_DIR, name + ".py"), files)
    h = hashlib.sha1()
    for f in sorted(files):
        h.update(os.path.basename(f).encode())
        h.update(b"|")
        h.update(_stat_sig(f))
        h.update(b"\n")
    return h.hexdigest()


# ---------- 变更影响分析 ----------

_GLOB_CACHE = {}


def _glob_re(pat):
    """把 glob 编译成正则。支持 `**`（跨目录）与 `*`（段内）。

    只用这两种 —— 够表达这张表里的所有条目，也避免 fnmatch 那些
    "`*` 也能跨 `/`" 的意外行为。
    """
    if pat in _GLOB_CACHE:
        return _GLOB_CACHE[pat]
    out, i, n = [], 0, len(pat)
    while i < n:
        c = pat[i]
        if c == "*":
            if pat.startswith("**", i):
                i += 2
                if i < n and pat[i] == "/":
                    i += 1          # `**/` 连零层目录一起匹配
                out.append(".*")
                continue
            out.append("[^/]*")
        elif c == "?":
            out.append("[^/]")
        else:
            out.append(re.escape(c))
        i += 1
    rx = re.compile("^" + "".join(out) + "$")
    _GLOB_CACHE[pat] = rx
    return rx


def _match_any(globs, rel):
    return any(_glob_re(g).match(rel) for g in globs)


def source_snapshot():
    """构建输入文件的 (相对路径 → (大小, mtime_ns))。

    只走 WATCH_ROOTS。文件本身（CMakeLists.txt）与目录都支持。
    """
    snap = {}
    for root in WATCH_ROOTS:
        full = os.path.join(ROOT, root)
        if os.path.isfile(full):
            pair = _stat_pair(full)
            if pair:
                snap[root.replace("\\", "/")] = pair
            continue
        if not os.path.isdir(full):
            continue
        for dirpath, dirnames, filenames in os.walk(full):
            dirnames[:] = [d for d in dirnames if d not in (".git", "build")]
            for fn in filenames:
                p = os.path.join(dirpath, fn)
                rel = os.path.relpath(p, ROOT).replace("\\", "/")
                pair = _stat_pair(p)
                if pair:
                    snap[rel] = pair
    return snap


def diff_snapshot(old, new):
    """→ (新增, 修改, 删除) 三个已排序的相对路径列表。old=None 视为全新。"""
    if not old:
        return sorted(new), [], []
    added = sorted(k for k in new if k not in old)
    removed = sorted(k for k in old if k not in new)
    changed = sorted(k for k in new if k in old and new[k] != old[k])
    return added, changed, removed


def impact_analysis(old_snap, new_snap, narrow=False):
    """把"变更文件集"映射成"该跑的用例集"。

    返回 dict：
      affected : set[str] | None   None = 全量重跑
      reason   : str               给 --explain 与人看的一句话
      lines    : list[str]         逐文件的判定（已格式化）
      unmapped : list[str]         没登记的文件（会触发全量）
      globalh  : list[str]         命中的横切文件
      no_trigger : list[str]       没有 TRIGGERS 登记的用例（无条件重跑）
    """
    added, changed, removed = diff_snapshot(old_snap, new_snap)
    hits = [("+", f) for f in added] + [("~", f) for f in changed] \
        + [("-", f) for f in removed]

    no_trigger = [n for n in DEFAULT if not TRIGGERS.get(n)]

    # 没有基准快照（首次跑 / 缓存格式升级）→ 不做逐文件判定。
    # 否则会把"整个仓库都是新增"当成两千条未登记变更打出来，没有信息量。
    if not old_snap:
        return {
            "affected": None,
            "reason": "没有基准快照（首次分析 / 缓存刚重建）",
            "lines": [], "unmapped": [], "globalh": [], "ignored": [],
            "no_trigger": no_trigger, "hits": [],
        }

    affected, lines = set(), []
    unmapped, globalh, ignored = [], [], []

    for tag, f in hits:
        # ⚠ 顺序很重要：**先判横切**。否则 `IGNORE_SUFFIXES` 里的通配后缀
        #   会把 `CMakeLists.txt` 这类"名字像文档、其实是构建输入"的文件
        #   一起吞掉（`.txt` 就踩过这个坑：CMakeLists.txt 被当成文档忽略 →
        #   改了构建配置却什么都不跑）。
        if _match_any(GLOBAL_GLOBS, f):
            globalh.append(f)
            lines.append("  %s %-52s ⚠ 横切文件 → 全量" % (tag, f))
            continue
        if _match_any(IGNORE_GLOBS, f) or f.endswith(IGNORE_SUFFIXES):
            ignored.append(f)
            lines.append("  %s %-52s 忽略（文档/辅助脚本）" % (tag, f))
            continue
        cases = [c for c, gl in TRIGGERS.items() if _match_any(gl, f)]
        if cases:
            affected.update(cases)
            lines.append("  %s %-52s → %s"
                         % (tag, f, ", ".join(sorted(cases))))
        else:
            unmapped.append(f)
            lines.append("  %s %-52s ⚠ 未登记 → 全量（请补 TRIGGERS）"
                         % (tag, f))

    no_trigger = [n for n in DEFAULT if not TRIGGERS.get(n)]

    if not hits:
        reason = "自上次回归以来源码没有任何变更"
    elif unmapped:
        reason = "%d 个文件没登记归属（保守起见全量）" % len(unmapped)
        affected = None
    elif globalh and not narrow:
        reason = "%d 个横切文件变了（文件粒度看不出改的是哪一段）" % len(globalh)
        affected = None
    elif globalh and narrow:
        reason = "只有横切文件变了，--narrow 下按已登记映射处理"
    else:
        reason = "变更全部落在已登记的映射里"

    return {
        "affected": affected,
        "reason": reason,
        "lines": lines,
        "unmapped": unmapped,
        "globalh": globalh,
        "ignored": ignored,
        "no_trigger": no_trigger,
        "hits": hits,
    }


def stale_build_warning(hits):
    """源码比 Editor.exe 还新 → 大概率忘了重建。早提醒，省下整轮白跑。"""
    exe = os.path.join(BIN_DIR, "Editor.exe")
    try:
        exe_t = os.stat(exe).st_mtime
    except OSError:
        return "找不到 %s（是不是还没构建过？）" % exe
    newest, newest_f = 0.0, ""
    for _tag, f in hits:
        p = os.path.join(ROOT, f.replace("/", os.sep))
        try:
            t = os.stat(p).st_mtime
        except OSError:
            continue
        if t > newest:
            newest, newest_f = t, f
    if newest > exe_t + 1.0:
        return ("`%s` 比 Editor.exe 还新 → 大概率**没重新构建**，"
                "这一轮跑的是旧二进制" % newest_f)
    return ""


def load_cache():
    try:
        with open(CACHE_PATH, "r", encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or "src" not in data:
        return None        # 旧格式（bin 指纹）→ 视为没有缓存
    return data


def save_cache(src_snap, passed, tests):
    os.makedirs(LOG_DIR, exist_ok=True)
    tmp = CACHE_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump({"src": src_snap, "passed": sorted(passed), "tests": tests},
                  fh, indent=1)
    os.replace(tmp, CACHE_PATH)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tests", nargs="*", help="只跑这些（可省 verify_ 前缀）")
    ap.add_argument("--tail", type=int, default=25,
                    help="失败时打印日志尾部行数（默认 25）")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="两个用例之间额外等待的秒数（默认 2）")
    ap.add_argument("--all", "-a", action="store_true",
                    help="无视缓存，全部重跑")
    ap.add_argument("--no-cache", action="store_true",
                    help="不读也不写缓存")
    ap.add_argument("--explain", action="store_true",
                    help="只打印变更影响分析，不实际跑")
    ap.add_argument("--narrow", action="store_true",
                    help="横切文件不触发全量（作者显式断言其它特性不受影响）")
    args = ap.parse_args()

    explicit = bool(args.tests)
    names = args.tests or DEFAULT
    names = [n if n.startswith("verify_") else "verify_" + n for n in names]

    use_cache = not args.no_cache
    cache = load_cache() if use_cache else None
    old_snap = (cache or {}).get("src")
    cur_snap = source_snapshot()

    # ---- 变更影响分析 ----
    imp = impact_analysis(old_snap, cur_snap, narrow=args.narrow)
    show = use_cache and not explicit

    if show or args.explain:
        print("== 变更影响分析 ==")
        if not old_snap:
            print("  （没有可用缓存 → 首次分析，视为全量）")
        elif imp["hits"]:
            print("  变更 %d 个文件（+新增 / ~修改 / -删除）：" % len(imp["hits"]))
            for ln in imp["lines"]:
                print(ln)
        else:
            print("  源码快照与上次一致（无变更）")
        # ⚠ "没登记 TRIGGERS"的用例只在本轮**确实有源码变更**时才无条件重跑。
        #   完全没有变更时它们也不可能受影响（"没变 ⇒ 结果不变"），
        #   所以那一轮可以直接一例都不跑 —— 这是"再跑一遍确认"的常见用法。
        if imp["hits"] and imp["no_trigger"]:
            print("  ⚠ 没登记 TRIGGERS、本轮无条件重跑：%s"
                  % ", ".join(imp["no_trigger"]))
        if imp["affected"] is None:
            print("  → 全量重跑（%s）" % imp["reason"])
        else:
            print("  → 关联用例 %d 个：%s"
                  % (len(imp["affected"]),
                     ", ".join(sorted(imp["affected"])) or "（无）"))
            print("  → 依据：%s" % imp["reason"])
        warn = stale_build_warning(imp["hits"])
        if warn:
            print("  ⚠ %s" % warn)
        print("")

    if args.explain:
        return 0

    # ---- 决定每个用例跳不跳 ----
    # affected is None → 全量（谁都不跳）
    passed_prev, tests_prev = set(), {}
    if cache:
        passed_prev = set(cache.get("passed") or [])
        tests_prev = cache.get("tests") or {}

    cached_ok = set()
    if use_cache and not args.all:
        for n in names:
            if n not in passed_prev:
                continue
            if tests_prev.get(n) != test_fingerprint(n):
                continue          # 用例自己的脚本变了
            if n in imp["no_trigger"] and imp["hits"]:
                continue          # 没登记归属：**有变更时**保守重跑
            if imp["affected"] is not None and n not in imp["affected"]:
                cached_ok.add(n)  # 本次改动跟它无关

    rows, skipped, ran = [], [], 0
    for n in names:
        path = os.path.join(TOOLS_DIR, n + ".py")
        if not os.path.exists(path):
            rows.append((n, None, "脚本不存在", ""))
            print("%-26s %s" % (n, "脚本不存在"))
            continue
        # 明确点名的不跳（"我就要看这个"）
        if (not explicit) and n in cached_ok:
            skipped.append(n)
            print("%-26s %s" % (n, "SKIP  (与本轮改动无关，上次 PASS)"))
            continue
        if ran:   # 让上一个编辑器进程彻底退场、前台焦点回稳
            time.sleep(args.settle)
        t0 = time.time()
        ran += 1
        p = subprocess.run([PY, path], cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True,
                           encoding="utf-8", errors="replace")
        out = p.stdout or ""
        ok = False
        for line in out.splitlines():
            if BAD_RE.search(line):
                ok = False
            elif OK_RE.search(line):
                ok = True
        mark = "PASS" if (p.returncode == 0 and ok) else "FAIL"
        rows.append((n, p.returncode, mark, out))
        # 每个用例的**完整**输出都落盘。失败时只打尾部 N 行，光靠它没法定位
        # 问题（比如"手柄目标是碰撞体"挂了，但尾部看到的已经是几行之后的连锁
        # 失败，看不到它前一刻的起点断言）。日志留在 _venn_verify/_regression/。
        os.makedirs(LOG_DIR, exist_ok=True)
        with open(os.path.join(LOG_DIR, n + ".log"), "w",
                  encoding="utf-8") as fh:
            fh.write(out)
        print("%-26s %-4s rc=%-3d %5.1fs" % (n, mark, p.returncode,
                                             time.time() - t0))
        if mark == "FAIL":
            tail = out.splitlines()[-args.tail:]
            for line in tail:
                print("      | " + line)
            print("      * 完整日志: %s"
                  % os.path.join(LOG_DIR, n + ".log"))

    # ---- 缓存回写：本轮跑过的用例按结果更新，没跑到的沿用旧的 ----
    # 只有 PASS 才进缓存（FAIL 会被丢弃 → 下次一定重跑，这正是我们要的）。
    # 快照存**本轮开始前**采集的那一份：它代表"这一轮测的是哪个源码状态"。
    if use_cache:
        keep = set(passed_prev)
        keep_tests = dict(tests_prev)
        if not explicit:
            # 非显式跑（整份 DEFAULT）：顺手清掉已经从清单里移除的用例
            keep &= set(names)
            keep_tests = {k: v for k, v in keep_tests.items() if k in keep}
        for n, _rc, mark, _out in rows:
            if mark == "PASS":
                keep.add(n)
                keep_tests[n] = test_fingerprint(n)
            else:
                keep.discard(n)
                keep_tests.pop(n, None)
        save_cache(cur_snap, keep, keep_tests)

    bad = [r for r in rows if r[2] != "PASS"]
    total = len(rows) + len(skipped)
    done = total - len(bad)
    print("\n%d/%d 通过" % (done, total))
    if skipped:
        print("（其中 %d 例与本轮改动无关，沿用上次 PASS 未重跑：%s）"
              % (len(skipped), ", ".join(skipped)))

    # ---- 横切检查：资源泄漏 ----
    # "谁的日志里有就算谁的" —— 这是**搭车**式检查：只有**优雅退出**的用例
    # 日志里才会出现 `vkDestroyDevice(): ... leaked objects`（强杀进程根本
    # 不产生报告）。所以不做全局扫描，只扫本轮真跑过的用例。
    # verify_collision_play 挂 `engine/render/**` 就是为保证"任何渲染层改动
    # 都至少有一个优雅退出的用例跑过"。
    leaks = [n for n, _rc, _mk, out in rows if out and LEAK_RE.search(out)]
    if leaks:
        print("\n== 横切检查 ==")
        print("  ✗ 资源泄漏：%s 的日志里有 vkDestroyDevice 泄漏报告"
              % ", ".join(leaks))
        print("    （一般是某个 pass 的析构漏了 destroyFrames()/vkDestroy*；"
              "搜日志里的 leaked objects 那行）")
    if bad:
        print("失败：" + ", ".join(r[0] for r in bad))
    return 1 if (bad or leaks) else 0


if __name__ == "__main__":
    sys.exit(main())
