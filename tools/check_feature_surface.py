#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""特性↔UI 覆盖自检（docs/editor_architecture.md 第 9 节的强制机制）。

## 为什么需要它

本项目的 158 个 `MYVK_*` 环境变量里，一度有约 60% 在编辑器里**点不到**
（SSAO / SSGI / SSR / God rays / SSS / 阴影级联硬化…）。后果是：
功能在引擎里"有"，但美术/TA 用不上 —— 按 `docs/editor_architecture.md`
的定义，这**不算交付**。

这个脚本把"每个特性必须有 UI 面"从**人的记性**变成**机器的检查**：
不跑 GUI、秒级、可进 CI。

## 检查项

| 组 | 检查 | 失败意味着 |
|---|---|---|
| A | 登记项字段完整（id/name/roadmap/panel 非空） | 登记不规范 |
| B | 每个特性写了文档（tooltip/说明） | 用户看不懂 |
| C | `ui` 里的 token 能在 `editor/` 里 grep 到 | **该特性在编辑器里没有入口** ★ |
| D | 每个 `MYVK_*` 都被登记，或属于"测试/诊断专用"白名单 | **裸 env 漏网**（只有环境变量能开） |
| E | `test` 指向的用例在 `run_regression.py::DEFAULT` 里 | 没有回归保护 |
| F | `gpu` 段名在 `GpuTimer` 的段名表里 | 成本不可见（原则 P7） |

用法：
    python tools/check_feature_surface.py            # 全部检查
    python tools/check_feature_surface.py -v         # 打印通过的项
    python tools/check_feature_surface.py --coverage # 只打覆盖率表

退出码：0 = 全绿；1 = 有 FAIL（C 组与 D 组是"必须清零"的欠账）。
"""

from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EDITOR_DIR = os.path.join(ROOT, "editor")
REG_SRC = os.path.join(ROOT, "tools", "run_regression.py")
TIMER_SRC = os.path.join(ROOT, "engine", "render", "GpuTimer.cpp")
# E01 落地后的**运行时**单一事实源；H 组拿它和本文件的离线表对账
FEATURE_REGISTRY_SRC = os.path.join(ROOT, "engine", "render",
                                    "FeatureRegistry.cpp")

# ============================================================
# 特性登记表
#
# ⚠ 这就是 docs/editor_architecture.md 里 `FeatureDesc` 的"离线投影"：
#   在真正的 FeatureRegistry.h 落地之前，这份表就是那份注册表。
#   落地后，本脚本改为解析 FeatureRegistry.h（结构一一对应）。
#
# 字段：
#   id      稳定标识（与 FeatureDesc::id 对齐）
#   name    显示名
#   roadmap 对应路线图项（"R4-4" / "—"）
#   scope   Editor / Engine / Scene / Object / Camera（设置层级）
#   panel   面板/分区（"Engine ▸ Scene/Ambient"）
#   ui      **UI 存在性证据**：能在 editor/ 里 grep 到的唯一字符串。
#           它是"这个特性的界面真的存在"的机器可验证凭据 ——
#           改 UI 代码时如果把这些 token 删了，C 组会立刻报红。
#   env     该特性拥有的环境变量（完整名，不含 MYVK_ 前缀也写全）
#   test    回归用例名（run_regression.py::DEFAULT 里的键）
#   gpu     GpuTimer 段名（有独立计时才有值，否则 None）
# ============================================================
FEATURES = [
    # ---------------- R0 工具先行 ----------------
    dict(id="gpu_timing", name="GPU 时戳剖析", roadmap="R0-1", scope="Editor",
         panel="Stats ▸ GPU timing", ui=["ms/frame", "GPU:"],
         env=["MYVK_GPU_TIMER", "MYVK_GPU_TIMING"],
         test="verify_gpu_timing", gpu=None),
    dict(id="view_modes", name="调试视图模式", roadmap="R0-2", scope="Editor",
         panel="Engine ▸ Viewport", ui=["View mode", "VIEW-MODE"],
         env=["MYVK_VIEW_MODE"], test="verify_viewmodes", gpu=None),

    # ---------------- R1 时域地基 ----------------
    dict(id="taa", name="TAA 时域抗锯齿", roadmap="R1-1", scope="Engine",
         panel="Engine ▸ Rendering", ui=["taa."],
         env=["MYVK_TAA", "MYVK_TAA_BLEND", "MYVK_TAA_CLAMP", "MYVK_TAA_PHASE"],
         test="verify_taa", gpu="post"),
    dict(id="auto_exposure", name="自动曝光（直方图 eye adaptation）",
         roadmap="R1-2", scope="Engine", panel="Engine ▸ Post Processing",
         ui=["ae."],
         env=["MYVK_AUTOEXP", "MYVK_EXPOSURE", "MYVK_AE_KEY", "MYVK_AE_SPEED",
              "MYVK_AE_BIAS", "MYVK_AE_MIN", "MYVK_AE_MAX", "MYVK_AE_SIM_LUM"],
         test="verify_auto_exposure", gpu="post"),
    dict(id="color_lut", name="色彩分级 LUT（32³）", roadmap="R1-3",
         scope="Engine", panel="Engine ▸ Post Processing",
         ui=["cg."],
         env=["MYVK_CG", "MYVK_CG_BLEND", "MYVK_CG_LIFT", "MYVK_CG_GAMMA",
              "MYVK_CG_GAIN", "MYVK_CG_CONTRAST", "MYVK_CG_SATURATION",
              "MYVK_LUT_RES"],
         test="verify_color_lut", gpu="post"),
    dict(id="drs", name="动态分辨率 / TSR 式升采样", roadmap="R1-4",
         scope="Engine", panel="Engine ▸ Post Processing",
         ui=["drs."],
         env=["MYVK_DRS", "MYVK_DRS_AUTO", "MYVK_DRS_TARGET_MS",
              "MYVK_DRS_MIN", "MYVK_DRS_MAX", "MYVK_DRS_SHARP",
              "MYVK_RENDER_SCALE"],
         test="verify_drs", gpu="post"),

    # ---------------- R2 可见性 / GPU-driven ----------------
    dict(id="hiz", name="Hi-Z 遮挡剔除（两帧延迟）", roadmap="R2-1",
         scope="Engine", panel="Engine ▸ Clustered Forward",
         ui=["hiz."],
         env=["MYVK_HIZ", "MYVK_HIZ_BIAS"], test="verify_hiz", gpu="depth"),
    dict(id="mdi", name="Multi-draw indirect + GPU 剔除", roadmap="R2-2",
         scope="Engine", panel="Engine ▸ Clustered Forward",
         ui=["rp.mdi"],
         env=["MYVK_MDI", "MYVK_DRAW_BATCH"], test="verify_mdi", gpu="forward"),
    dict(id="bindless", name="Bindless / descriptor indexing", roadmap="R2-3",
         scope="Engine", panel="Engine ▸ Clustered Forward",
         ui=["rp.bindless"], env=["MYVK_BINDLESS"],
         test="verify_bindless", gpu=None),

    # ---------------- R3 光影质量 ----------------
    dict(id="shadow_cascade", name="阴影级联 + 硬化三件套", roadmap="R3-1",
         scope="Scene", panel="Engine ▸ Shadows",
         ui=["shadow."],
         env=["MYVK_SHADOW", "MYVK_SHADOW_CASCADES", "MYVK_SHADOW_DIST",
              "MYVK_SHADOW_RES", "MYVK_SHADOW_SNAP", "MYVK_SHADOW_LAMBDA",
              "MYVK_SHADOW_HARDEN", "MYVK_SHADOW_BLEND"],
         test="verify_csm", gpu="shadows"),
    dict(id="local_shadow", name="局部阴影（点/聚光灯）", roadmap="R3-1",
         scope="Scene", panel="Inspector ▸ Light",
         ui=["Casts Shadow", "lshadow."],
         env=["MYVK_LOCAL_SHADOW", "MYVK_LOCAL_SHADOW_BIAS",
              "MYVK_LOCAL_SHADOW_RES", "MYVK_LOCAL_SHADOW_LIGHTS"],
         test="verify_local_shadow", gpu="local_sh"),
    dict(id="volumetric_fog", name="Froxel 体积雾", roadmap="R3-2",
         scope="Scene", panel="Engine ▸ Atmosphere",
         ui=["vfog."],
         env=["MYVK_VFOG", "MYVK_VFOG_DENSITY", "MYVK_VFOG_ALBEDO",
              "MYVK_VFOG_FALLOFF", "MYVK_VFOG_BLOB", "MYVK_VFOG_PHASE_G"],
         test="verify_volumetric_fog", gpu="framegraph"),
    dict(id="light_modifiers", name="区域光 / IES / light function",
         roadmap="R3-3", scope="Object", panel="Inspector ▸ Light",
         ui=["IES", "AREA", "lm."],
         env=["MYVK_AREA_LIGHT", "MYVK_IES", "MYVK_IES_FILE", "MYVK_COOKIE"],
         test="verify_light_modifiers", gpu=None),

    # ---------------- R4 材质表现力 ----------------
    dict(id="alpha_mask", name="AlphaMode::Mask + 抖动裁切", roadmap="R4-1",
         scope="Object", panel="Inspector ▸ Material",
         ui=["Alpha Mode", "Alpha Cutoff"],
         # 注：MYVK_CUTOFF_DBG 是诊断 dump，不属于本特性的用户旋钮，
         #     所以不登记在这里，只会在 D 组以"建议进 Debug 档"出现。
         env=[], test="verify_alpha_mask", gpu=None),
    dict(id="sss", name="预积分 SSS（皮肤）", roadmap="R4-2", scope="Object",
         panel="Inspector ▸ Material",
         ui=["sss."],
         env=["MYVK_SSS"], test="verify_sss", gpu=None),
    dict(id="pom", name="POM 视差遮挡贴图", roadmap="R4-3", scope="Object",
         panel="Inspector ▸ Material ▸ Height",
         ui=["Height Scale", "POM", "pom."],
         env=["MYVK_POM", "MYVK_POM_LAYERS", "MYVK_POM_FADE"],
         test="verify_pom", gpu=None),
    dict(id="decals", name="延迟贴花（DBuffer）", roadmap="R4-4", scope="Engine",
         panel="Inspector ▸ Decal",
         ui=["Decal", "decals."],
         env=["MYVK_DECALS"], test="verify_decals", gpu="decals"),
    dict(id="probe", name="反射探针（立方体捕获）", roadmap="R5-1", scope="Scene",
         panel="Inspector ▸ Reflection Probe",
         ui=["Reflection Probe", "probe."],
         # MAX / PREFILTER_* / SAMPLES 是"装机参数"（决定显存与烘焙时长），
         # 按 FeatureRegistry 的取舍**故意不接 UI**：它们和盒子分辨率一样
         # 属于"换一次要重建整套资源"的量级，放进面板只会让人误点。
         # 但仍然必须在这里登记 —— 否则 D 组会把它们报成"裸 env 漏网"。
         env=["MYVK_PROBES", "MYVK_PROBE_INTENSITY", "MYVK_PROBE_SIZE",
              "MYVK_PROBE_AF", "MYVK_PROBE_MAX", "MYVK_PROBE_PREFILTER_RES",
              "MYVK_PROBE_PREFILTER_MIPS", "MYVK_PROBE_SAMPLES"],
         test="verify_reflection_probe", gpu=None),

    # ---------------- R6 大件效果 ----------------
    # GPU 粒子系统。UI 面是 Inspector 里那个**模块化面板**：12 个折叠模块
    # （Playback / Emission / Shape / Velocity / Limit Velocity / Force /
    # Colour / Size / Rotation / Noise / Renderer / Soft Particles），每个
    # 模块带启用勾选框与"恢复默认"。两条 ui token 都在 InspectorPanel.*
    # 里：段名 + 一键展开按钮的矩形日志（后者是自动化点击的落点）。
    dict(id="fx", name="GPU 粒子系统", roadmap="R6-1", scope="Object",
         panel="Inspector ▸ Particle Emitter",
         ui=["Particle Emitter", "INS-FX-EXPAND-ALL"],
         env=["MYVK_FX", "MYVK_FX_CAPACITY", "MYVK_FX_EMITTERS", "MYVK_FX_HZ",
              "MYVK_FX_SIZE", "MYVK_FX_SOFTNESS", "MYVK_FX_INTENSITY",
              "MYVK_FX_FREEZE", "MYVK_FX_DBG"],
         test="verify_particles", gpu="particles"),

    # ---------------- 屏幕空间效果（ScreenSpaceSettings）----------------
    # ⚠ 这四组一度**完全没有 UI** —— 是 E1 阶段要清零的头号欠账。
    dict(id="ssao", name="SSAO 环境光遮蔽", roadmap="—", scope="Scene",
         panel="Engine ▸ Scene/Ambient",
         ui=["ssao."],
         env=["MYVK_SSAO", "MYVK_SSAO_RADIUS", "MYVK_SSAO_STRENGTH"],
         test="verify_screenspace", gpu="ssao"),
    dict(id="ssgi", name="SSGI-lite 屏幕空间全局光", roadmap="—",
         scope="Scene", panel="Engine ▸ Scene/Global Illumination",
         ui=["ssgi."],
         env=["MYVK_SSGI", "MYVK_SSGI_RADIUS", "MYVK_SSGI_STRENGTH"],
         test="verify_screenspace", gpu="framegraph"),
    dict(id="ssr", name="SSR 屏幕空间反射", roadmap="—", scope="Scene",
         panel="Engine ▸ Scene/Reflections",
         ui=["ssr."],
         env=["MYVK_SSR", "MYVK_SSR_STRENGTH"],
         test="verify_screenspace", gpu="framegraph"),
    dict(id="godrays", name="God rays 径向体积光", roadmap="—", scope="Scene",
         panel="Engine ▸ Scene/Atmosphere",
         ui=["godrays."],
         env=["MYVK_GODRAYS", "MYVK_GODRAYS_STRENGTH"],
         test="verify_screenspace", gpu="post"),

    # ---------------- 环境 / 光照基础设施 ----------------
    dict(id="sky", name="天空盒 + 动态天空", roadmap="—", scope="Scene",
         panel="Engine ▸ Sky", ui=["sky."],
         env=["MYVK_SKY", "MYVK_SKY_ZENITH", "MYVK_SKY_GROUND",
              "MYVK_SKY_RES"],
         test="verify_ibl", gpu=None),
    dict(id="ibl", name="IBL（辐照度 / 预过滤 / BRDF LUT）", roadmap="—",
         scope="Scene", panel="Engine ▸ Sky", ui=["sky.ibl"],
         env=["MYVK_IBL", "MYVK_IBL_SAMPLES", "MYVK_PREFILTER_RES",
              "MYVK_PREFILTER_MIPS", "MYVK_IRRADIANCE_RES", "MYVK_BRDF_SAMPLES"],
         test="verify_ibl", gpu=None),
    dict(id="height_fog", name="解析式高度雾", roadmap="—", scope="Scene",
         panel="Engine ▸ Atmosphere", ui=["hfog."],
         env=["MYVK_FOG", "MYVK_FOG_DENSITY", "MYVK_FOG_FALLOFF"],
         test="verify_screenspace", gpu=None),

    # ---------------- 后处理 / 相机效果 ----------------
    dict(id="post_tonemap", name="色调映射 / Bloom / FXAA / 曝光", roadmap="—",
         scope="Engine", panel="Engine ▸ Post Processing",
         ui=["post."],
         env=["MYVK_FXAA", "MYVK_MSAA", "MYVK_MIPMAP", "MYVK_ANISO"],
         test="verify_fxaa", gpu="post"),
    dict(id="camera_effects",
         name="相机特效（DoF / 运动模糊 / 色差 / 暗角）", roadmap="—",
         scope="Camera", panel="Engine ▸ Camera Effects",
         ui=["dof."],
         env=["MYVK_DOF", "MYVK_DOF_FOCUS", "MYVK_DOF_APERTURE",
              "MYVK_DOF_RADIUS", "MYVK_MOTIONBLUR", "MYVK_MB_STRENGTH",
              "MYVK_MB_SAMPLES", "MYVK_MB_SIM_YAW", "MYVK_CHROMA",
              "MYVK_CHROMA_STRENGTH", "MYVK_VIGNETTE_STRENGTH",
              "MYVK_VIGNETTE_POWER", "MYVK_VIGNETTE_OFF"],
         test="verify_camfx", gpu="post"),

    # ---------------- 剔除 / 场景 ----------------
    dict(id="culling", name="视锥 / 簇 / 深度预通道剔除", roadmap="—",
         scope="Engine", panel="Engine ▸ Clustered Forward",
         ui=["cull.", "rp.clusterCull"],
         env=["MYVK_FRUSTUM_CULL", "MYVK_CLUSTER_CULL", "MYVK_DEPTH_PREPASS",
              "MYVK_MAX_LIGHTS_PER_CLUSTER", "MYVK_TILE_SIZE",
              "MYVK_CLUSTER_SLICES"],
         test="verify_frustum_cull", gpu="cluster"),
    dict(id="lod", name="LOD 选择", roadmap="—", scope="Engine",
         panel="Engine ▸ Rendering", ui=["lod."],
         env=["MYVK_LOD", "MYVK_LOD_BIAS", "MYVK_LOD_FORCE"],
         test="verify_lod", gpu=None),
    dict(id="instancing", name="GPU Instancing", roadmap="—", scope="Engine",
         panel="Engine ▸ Clustered Forward", ui=["rp.instancing"],
         env=["MYVK_INSTANCING", "MYVK_TEXSTREAM"],
         test="verify_instancing", gpu="forward"),
    dict(id="grid", name="视口栅格", roadmap="—", scope="Editor",
         panel="Engine ▸ Viewport grid", ui=["grid.minorStep"],
         env=["MYVK_GRID", "MYVK_LIGHT_GIZMOS"], test="verify_screenspace",
         gpu=None),
    dict(id="multi_camera", name="多相机 / RenderTarget", roadmap="—",
         scope="Camera", panel="Engine ▸ Cameras", ui=["View camera"],
         env=["MYVK_VIEW_CAM"], test="verify_camfx", gpu="offscreen"),
    dict(id="shader_runtime", name="着色器热重载 / 变体", roadmap="—",
         scope="Editor", panel="Preferences ▸ Shaders",
         ui=["Shader"],
         env=["MYVK_SHADER_HOTRELOAD", "MYVK_SHADER_VARIANTS",
              "MYVK_SHADER_FORCE_MASK"],
         test="verify_shader_hotreload", gpu=None),

    # ---------------- 游戏 UI / HUD（U0，docs/ui_hud_roadmap.md）----------------
    # C 组的 ui token 用的是**参数 id**：FeatureRegistry.cpp 也在
    # _collect_editor_sources() 的输入里（registry 是"面板长什么样"的
    # 元数据源），所以参数 id 本身就是"界面上点得到"的机器证据。
    dict(id="ui2", name="游戏 UI / HUD", roadmap="U0", scope="Engine",
         panel="Engine/Inspector ▸ UI",
         ui=["ui.hudInEditor", "ui.debugBounds"],
         env=["MYVK_UI", "MYVK_UI_HUD_IN_EDITOR", "MYVK_UI_SCALE",
              "MYVK_UI_DEBUG_BOUNDS", "MYVK_UI_FONT_SIZE"],
         test="verify_ui2", gpu=None),
]

# ============================================================
# 环境变量白名单：**合法地**没有 UI 的那些
#
# 三类，都必须有理由：
#   ① 测试/自动化注入 —— 由脚本在启动时设，运行时不需要 UI
#   ② 路径/构建期 —— 启动参数语义
#   ③ 诊断 dump（`*_DBG`）—— 排查用，理想上应进 Debug 档，但不算欠账
#
# ⚠ 加进白名单 = 显式断言"这个开关用户不需要在界面上摸到"。
#   加之前先想清楚：它真的不需要吗？（`MYVK_MSAA` 就不该在白名单里）
# ============================================================
ALLOW_PREFIXES = (
    "MYVK_EDITOR_",     # ① 编辑器自动化注入（CAM/SCENE/SELECT/PLAY/SPACE…）
    # ① 运行时（G 系列）的开关。设计上与 `MYVK_EDITOR_` 完全对称：
    #    它们只在 **Game.exe** 里存在，而 Game.exe **没有界面**（M1 阶段
    #    连窗口 UI 都还没有，G15 才有游戏内 UI），所以"界面上摸不到"是
    #    它的本质而不是欠账。带前缀放行 = 新增一个运行时开关不必每次来
    #    白名单补一行，同时"裸 env 漏网"仍然会被抓住（例如有人写了个
    #    `MYVK_SCENE` 就没前缀了）。
    "MYVK_RUNTIME_",
    # 启动器的**测试初值**钩子（`MYVK_LAUNCHER_TEST_ROOT/NAME/RES/QUALITY/
    # CODE_EXTERNAL`）：四步向导仍然要一步不少地真点，它们只填初值。
    # 与 `MYVK_EDITOR_` 同一条理由 —— 被测对象是启动器窗口，不是这些变量。
    "MYVK_LAUNCHER_TEST_",
    # 脚本引擎的诊断/配额（`MYVK_VKS_TRACE` 追踪、`MYVK_VKS_QUOTA` 指令数
    # 上限）。用户面对的是 Scripts 面板（S5）与控制台 `script.*` 命令，
    # 这两条是"排查脚本死循环"时的 CI 钩子。
    "MYVK_VKS_",
)
ALLOW_EXACT = {
    # ① 测试 / CI 基础
    "MYVK_FRAMES", "MYVK_HIDE_UI", "MYVK_NO_WINDOW_SAVE", "MYVK_LOG_RECTS",
    "MYVK_VALIDATION", "MYVK_ENGINE_PANEL", "MYVK_EXTRA_VIEW",
    # ① `MYVK_RUNTIME=1`：运行时的 verbose 开关（每 60 帧一行 GAME-LOOP）。
    #    刻意**不**带下划线后缀（ship_roadmap 的验收条款就是按这个名字写的），
    #    所以它不能被上面的前缀规则吃掉，必须在这里显式登记。
    "MYVK_RUNTIME",
    # ① `MYVK_FIXED_DT=<秒>`：把每帧 dt 钉成常量，用于**确定性对照**
    #    （编辑器 Play 与 Game.exe 跑同一段固定步长序列，位置才能逐位比较，
    #    见 tools/verify_runtime_determinism.py）。纯测试钩子，同 `MYVK_FRAMES`。
    "MYVK_FIXED_DT",
    # `MYVK_UI_ASSET_DIR`：ui2 资产目录覆盖（开发期用）。正式包走 exeDir，
    # 这条只是"从别处跑二进制也能找到图集"的兜底 —— 纯测试/开发钩子。
    "MYVK_UI_ASSET_DIR",
    # `MYVK_LANG=zh|en`：运行时界面语言的**测试覆盖**（U7 验收条款点名的名字）。
    # 正规入口是 `--language` / game.json 的 `language`（RuntimeConfig 已解析），
    # 这条只是"不改配置文件也能 A/B 截图"的注入点 —— 与 `MYVK_FRAMES` 同类，
    # 所以刻意**不**改成 `MYVK_RUNTIME_*`（改了路线图的验收命令就跑不通）。
    "MYVK_LANG",
    # `MYVK_STARTUP_DELAY=1` 强制保留启动缓冲（正常启动本来就有 2s 缓冲，
    #  界面无开关；它只用来抵消 `MYVK_NO_WINDOW_SAVE` 顺带跳过缓冲的副作用，
    #  让 `verify_startup_show` 可测且不写 imgui.ini）→ 纯测试钩子，同 `MYVK_FRAMES`。
    "MYVK_STARTUP_DELAY",
    # ① 测试钩子：为了构造确定性对照而存在的"注入点"，
    #    源码里明确写着"正常使用不设即无影响" → 不需要 UI。
    "MYVK_FG_SUN_UV",
    # ① 物理（PHYS-1，2026-10-16）：
    #    · `MYVK_PHYS`      只是覆盖**场景里的开关**做 A/B（源码原话：
    #      "场景里的开关是权威；MYVK_PHYS=0/1 只是给自动化做 A/B 的强制覆盖"）
    #      —— 用户要碰的那个开关在 Inspector ▸ Physics ▸ `Simulate Physics`，
    #      有 UI，所以这里放行的是"覆盖手段"，不是"唯一入口"。
    #    · `MYVK_PHYS_TEST` 是**无窗口无 Vulkan** 的自检入口（editor/main.cpp
    #      在 app.run() 前就 return）—— 纯 CI/测试用，界面上不存在也不该存在。
    #      与 `MYVK_FRAMES` 同类。
    "MYVK_PHYS", "MYVK_PHYS_TEST",
    # ② E03 设置层级（2026-10-17）：
    #    · `MYVK_RENDER_SETTINGS` / `MYVK_RENDER_PREFS` 是**路径覆盖**，
    #      不是功能开关 —— 用户改设置靠的是 Engine 面板的
    #      `Export current` / `Reload from disk`（editor/EditorApp.cpp
    #      的 drawEngineSettingsIO），文件默认落在 `config/render.json`。
    #      这两个变量只给"自动化把文件指到临时目录"用（同 `MYVK_EDITOR_LAYOUT`）。
    #    · `MYVK_RENDER_SETTINGS_TEST` 是无窗口无 Vulkan 的自检入口
    #      （editor/main.cpp 在 app.run() 前就 return），与 `MYVK_PHYS_TEST`
    #      同类：CI 专用，界面上不存在也不该存在。
    "MYVK_RENDER_SETTINGS", "MYVK_RENDER_PREFS", "MYVK_RENDER_SETTINGS_TEST",
    # ① 骨骼动画自检（角色链 A0-3/A0-5，2026-10-18）：
    #    `MYVK_ANIM_TEST` 是**无窗口无 Vulkan** 的自检入口（editor/main.cpp
    #    在 app.run() 前就 return），与 `MYVK_PHYS_TEST` 同类：CI 专用，
    #    界面上不存在也不该存在。用户面对的动画开关在
    #    Inspector ▸ Animation（clip / 时间轴 / Loop / Playing），有 UI。
    "MYVK_ANIM_TEST",
    # ① 游戏 UI 自检（U0，2026-10-18）：`MYVK_UI2_TEST` 是无窗口无 Vulkan
    #    的自检入口（editor/main.cpp 在 app.run() 前就 return），与
    #    `MYVK_ANIM_TEST` / `MYVK_PHYS_TEST` 同类：CI 专用，界面上不存在
    #    也不该存在。用户面对的 UI 开关是 Engine/Inspector ▸ UI 的
    #    `ui.enabled` / `ui.hudInEditor`（有 UI 面）。
    "MYVK_UI2_TEST",
    # ② 路径 / 构建期
    "MYVK_ASSET_DIR", "MYVK_CONTENT_DIR", "MYVK_SHADER_DIR",
    "MYVK_CHARACTER_MODEL",
    # ③ 启动器（2026-10-19）：
    #    · `MYVK_LAUNCHER_PAGE` —— 自动化从哪一页开始（向导第几步 / 主界面）。
    #      纯测试钩子：正常使用总是从 Idle 页开始。
    #    · `MYVK_LAUNCHER_RECENT` —— 最近列表文件的**路径覆盖**（同
    #      `MYVK_RENDER_SETTINGS`）：用户改不了"最近工程存哪"，那是
    #      `%LOCALAPPDATA%/Venn/launcher/recent.json`，不需要界面开关。
    #    · `MYVK_TEMPLATE_DIR` —— 模板目录覆盖（开发期从别处跑二进制）。
    "MYVK_LAUNCHER_PAGE", "MYVK_LAUNCHER_RECENT", "MYVK_TEMPLATE_DIR",
    # ③ 无窗口自检入口（与 `MYVK_PHYS_TEST` / `MYVK_ANIM_TEST` 同类）：
    #    `MYVK_PROJECT_TEST` = 工程层 112 条；`MYVK_QUALITY_TEST` = E26 的
    #    15 条档位表断言。都在 app.run() 之前 return，界面上不存在。
    "MYVK_PROJECT_TEST", "MYVK_QUALITY_TEST",
}
# ③ 诊断 dump（`*_DBG`）。`*_DUMP` 同义：只打日志、不改行为，
#    与 `MYVK_PHYS_DUMP=<N>`（每 N 帧一行 `PHYS-BODY`）是同一类。
ALLOW_SUFFIX = ("_DBG", "_DUMP")

# C 组：这几种 scope 的"没有 UI"算 **FAIL**（用户直接面对的）
# 其余（Editor 内部工具类）算 **WARN**，不阻塞。
FAIL_SCOPES = {"Engine", "Scene", "Object", "Camera"}

# ============================================================
# 检查实现
# ============================================================


def _read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def _collect_editor_sources():
    """editor/ 下所有 .cpp/.h 的内容拼一起，供 token grep。

    E09 之后还拼上 FeatureRegistry.cpp：Engine 面板的控件由 FeatureUIGen
    从注册表生成，所以"注册表里登记了参数 id"本身就是 UI 存在的凭据
    （ui token 可以写 "ssr." 这种参数 id 前缀）。
    """
    parts = []
    for root, _dirs, files in os.walk(EDITOR_DIR):
        for fn in files:
            if fn.endswith((".cpp", ".h")):
                parts.append(_read(os.path.join(root, fn)))
    parts.append(_read(FEATURE_REGISTRY_SRC))
    return "\n".join(parts)


def _collect_registered_envs():
    """登记表里声明过的全部 env（去 MYVK_ 前缀统一成带前缀形式）。"""
    out = set()
    for f in FEATURES:
        for e in f.get("env") or ():
            out.add(e if e.startswith("MYVK_") else "MYVK_" + e)
    return out


def _scan_repo_envs():
    """扫全仓真实用到的 MYVK_*（源码 + 着色器 + 工具）。

    ⚠ 过滤掉以下划线下标结尾的名字：那是注释/文档里的**通配符写法**
      （如 `MYVK_VFOG_*`），不是真实变量，否则会被误判成"裸 env 漏网"。
    """
    pat = re.compile(r"MYVK_[A-Z_0-9]+")
    found = {}
    # ⚠ 必须排除**本文件自己**：它的登记表里写了一堆 env 名字，
    #   扫进去会让"每个 env 都出现在仓库里"，G 组（陈旧登记）永远无法报红。
    me = os.path.relpath(os.path.abspath(__file__), ROOT).replace("\\", "/")
    for sub in ("engine", "editor", "assets", "tools"):
        base = os.path.join(ROOT, sub)
        for root, _dirs, files in os.walk(base):
            for fn in files:
                if not fn.endswith((".cpp", ".h", ".frag", ".vert", ".comp",
                                    ".glsl", ".py", ".json")):
                    continue
                p = os.path.join(root, fn)
                rel = os.path.relpath(p, ROOT).replace("\\", "/")
                if rel == me:
                    continue
                for m in pat.findall(_read(p)):
                    if m.endswith("_"):
                        continue        # 通配符写法（`MYVK_XXX_*`），跳过
                    found.setdefault(m, set()).add(rel)
    return found


def _registry_envs():
    """FeatureRegistry.cpp 里登记过的全部 env（运行时 SSOT）。

    E01 之前这里返回空集 —— 那时还没有注册表，H 组会自动跳过。
    """
    src = _read(FEATURE_REGISTRY_SRC)
    if not src:
        return set()
    return set(re.findall(r'"(MYVK_[A-Z0-9_]+)"', src))


def _regression_cases():
    src = _read(REG_SRC)
    m = re.search(r"^DEFAULT\s*=\s*\[(.*?)^\]", src, re.S | re.M)
    if not m:
        return set()
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def _gpu_sections():
    src = _read(TIMER_SRC)
    m = re.search(r"kNames\[kSectionCount\]\s*=\s*\{(.*?)\}", src, re.S)
    if not m:
        return set()
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def main():
    ap = argparse.ArgumentParser(description="特性↔UI 覆盖自检")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="打印通过的项")
    ap.add_argument("--coverage", action="store_true",
                    help="只打覆盖率表")
    args = ap.parse_args()

    ed_src = _collect_editor_sources()
    reg_cases = _regression_cases()
    gpu_secs = _gpu_sections()
    registered = _collect_registered_envs()
    repo_envs = _scan_repo_envs()

    fails, warns, oks = [], [], []

    # ---------------- A/B/C：逐特性 ----------------
    missing_ui, bad_field, no_doc = [], [], []
    for f in FEATURES:
        fid = f["id"]
        # A：字段完整
        for key in ("id", "name", "roadmap", "panel"):
            if not f.get(key):
                bad_field.append("%s.%s 为空" % (fid, key))
        # B：文档（panel 里必须给出可读的分区描述 ≥ 6 字符）
        if len(f.get("panel") or "") < 6:
            no_doc.append(fid)
        # C：UI 存在性 ★
        toks = f.get("ui") or []
        if not toks:
            missing_ui.append(f)
        else:
            hit = [t for t in toks if t in ed_src]
            if not hit:
                missing_ui.append(f)
            else:
                oks.append("C %-16s UI 证据: %s" % (fid, ", ".join(hit)))

    for m in missing_ui:
        msg = "%-16s %-28s 面板=%s" % (m["id"], m["name"], m["panel"])
        if m.get("scope") in FAIL_SCOPES:
            fails.append("C 无 UI 面: " + msg)
        else:
            warns.append("C 无 UI 面(编辑器内部工具): " + msg)

    for b in bad_field:
        fails.append("A 登记字段不完整: " + b)
    for d in no_doc:
        warns.append("B 面板/分区描述过短: " + d)

    # ---------------- D：裸 env 漏网 ----------------
    # ⚠ 计数必须用**集合差**，不能用 len(reg) + len(allow) 做减法 ——
    #   两类会重叠（同一个变量既可能被登记、又可能命中白名单前缀），
    #   相减会算出负数。下面三个集合两两不相交。
    reg_set = {e for e in repo_envs if e in registered}
    allow_set = {e for e in repo_envs
                 if e.startswith(ALLOW_PREFIXES) or e in ALLOW_EXACT
                 or e.endswith(ALLOW_SUFFIX)}
    diag_set = {e for e in allow_set if e.endswith(ALLOW_SUFFIX)}

    orphans = []
    for env, where in sorted(repo_envs.items()):
        if env in reg_set or env in allow_set:
            continue
        orphans.append((env, sorted(where)))
    for env, where in orphans:
        fails.append("D 裸 env 未登记: %-28s %s" % (env, ", ".join(where[:2])))
    for env in sorted(diag_set):
        warns.append("D 诊断项未登记(建议进 Debug 档): " + env)

    # ---------------- G：登记了但仓库里不存在的 env（陈旧登记）----------------
    for f in FEATURES:
        for e in f.get("env") or ():
            full = e if e.startswith("MYVK_") else "MYVK_" + e
            if full not in repo_envs:
                warns.append("G 登记了但仓库中不存在（陈旧登记）: %s → %s"
                             % (f["id"], full))

    # ---------------- H：注册表接管度（E01 迁移进度）----------------
    # ⚠ 这是**进度表**而不是失败项：E01 明确只搬"构造函数 + 后处理"两处，
    #   剩下的（MSAA / 环境烘焙分辨率 / 分簇参数…）都存在函数内的局部
    #   变量里，要先提升成成员才能绑进注册表 —— 那是 E10 / E17 的工作。
    #   所以这里只报数、不判 FAIL，但报出来的每一条就是 E10 的输入。
    reg_envs = _registry_envs()
    not_migrated = []
    have_registry = bool(reg_envs)
    if have_registry:
        for f in FEATURES:
            for e in f.get("env") or ():
                full = e if e.startswith("MYVK_") else "MYVK_" + e
                if full not in reg_envs:
                    not_migrated.append((f["id"], full))

    # ---------------- E：回归保护 ----------------
    for f in FEATURES:
        t = f.get("test")
        if not t:
            warns.append("E 未指定回归用例: " + f["id"])
        elif t not in reg_cases:
            fails.append("E 回归用例不在 DEFAULT 里: %-16s → %s" % (f["id"], t))
        else:
            oks.append("E %-16s 回归用例 %s ✓" % (f["id"], t))

    # ---------------- F：GPU 成本可见 ----------------
    for f in FEATURES:
        g = f.get("gpu")
        if g and g not in gpu_secs:
            fails.append("F GPU 段名不存在: %-16s → %s（GpuTimer 段名表里没有）"
                         % (f["id"], g))
        elif g:
            oks.append("F %-16s GPU 段 %s ✓" % (f["id"], g))

    # ---------------- 覆盖率表 ----------------
    total = len(FEATURES)
    with_ui = total - len(missing_ui)
    env_total = len(repo_envs)
    env_orphan = len(orphans)

    print("=" * 72)
    print("特性 ↔ UI 覆盖  ·  docs/editor_architecture.md 第 9 节")
    print("=" * 72)
    print("特性总数            : %d" % total)
    print("有 UI 面            : %d  (%.0f%%)" % (with_ui, 100.0 * with_ui / total))
    print("无 UI 面            : %d  ← E1 阶段要清零" % len(missing_ui))
    print("-" * 72)
    print("环境变量总数        : %d" % env_total)
    print("已登记（有 UI/归属）: %d" % len(reg_set))
    print("白名单（测试/路径/诊断）: %d" % len(allow_set))
    print("裸 env 漏网         : %d  ← 必须清零" % env_orphan)
    print("=" * 72)
    if have_registry:
        declared = {e if e.startswith("MYVK_") else "MYVK_" + e
                    for f in FEATURES for e in (f.get("env") or ())}
        taken = len(declared) - len(not_migrated)
        print("注册表已接管的 env  : %d / %d  (%.0f%%)  ← FeatureRegistry.cpp"
              % (taken, len(declared),
                 100.0 * taken / len(declared) if declared else 0.0))
        print("尚未接管（只剩 env）: %d  → E10 / E17 的输入" % len(not_migrated))
    else:
        print("注册表              : 未落地（H 组跳过）")
    print("=" * 72)
    print("⚠ 粒度说明：本检查是**特性级**（这个特性在编辑器里有没有入口）。")
    print("  '有 UI 面' 不等于'每个参数都有 UI' —— 例如天空盒有开关、")
    print("  但天顶色/分辨率可能仍只有 env。参数级覆盖要等 FeatureRegistry")
    print("  落地后按 ParamDesc 逐项校验（架构文档 9.2 节）。")

    if not_migrated and (args.verbose or args.coverage):
        print("\n【尚未接管的 env（H 组）】—— 只有环境变量能设，注册表还没有它的位置：")
        by_feature = {}
        for fid, env in not_migrated:
            by_feature.setdefault(fid, []).append(env)
        for fid in sorted(by_feature):
            print("  %-16s %s" % (fid, ", ".join(sorted(by_feature[fid]))))

    if missing_ui and not args.coverage:
        print("\n【无 UI 面的特性】—— 用户/美术在编辑器里点不到：")
        for m in missing_ui:
            tag = "FAIL" if m.get("scope") in FAIL_SCOPES else "warn"
            print("  [%s] %-16s %-30s 应落在: %s"
                  % (tag, m["id"], m["name"], m["panel"]))
        print("\n  ⚠ 处置：要么补 UI（推荐，见架构文档第 11 节的补全清单），")
        print("          要么把该特性的 role 降级并在登记表里写明理由。")

    if args.verbose:
        print("\n【通过项】")
        for o in oks:
            print("  " + o)

    if warns and not args.coverage:
        print("\n【警告 %d 条】" % len(warns))
        for w in warns:
            print("  " + w)

    if fails:
        print("\n【失败 %d 条】" % len(fails))
        for x in fails:
            print("  " + x)
        print("\n结果: FAIL")
        return 1

    print("\n结果: PASS（%d 个特性全部有 UI 面；无裸 env 漏网）" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
