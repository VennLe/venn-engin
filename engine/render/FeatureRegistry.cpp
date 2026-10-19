// ============================================================
// render/FeatureRegistry 实现
//
// 结构：
//   ① kFeatures[] —— 纯静态元数据表（本文件最大的部分）
//   ② 运行时绑定表（id → 地址 + 捕获的默认值 + 是否已应用 env）
//   ③ applyEnv()  —— 逐 EnvMode 的解析（必须与 Renderer.cpp 原实现逐位等价）
// ============================================================

#include "render/FeatureRegistry.h"

#include "core/Logger.h"
#include "render/ShaderKeywords.h"   // kKeywordAll（var.forceMask 的合法上界）

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace render {

// ============================================================
// ① 元数据表
//
// 命名约定：param id = "<feature 短名>.<字段名>"，全小驼峰。
//   · 同时是 UI 的稳定 ImGui ID（PushID(feature) + ##param）
//   · 不与 env 名强绑定（env 名单独写，因为历史 env 名不规则）
//
// tier 的划分口径：
//   Basic    = 不看到它这个特性就没法用（总开关 + 主参数）
//   Advanced = 调优/排查时要（半径、偏置、质量、分辨率）
//   Debug    = 验证钩子、诊断、极端取值（普通用户永远不会去动）
// ============================================================

// ---- 视锥 / 遮挡 / LOD ----
static const ParamDesc kParamsCull[] = {
    PBool("cull.frustum", "Frustum culling", "MYVK_FRUSTUM_CULL",
          ParamTier::Basic,
          "视锥外的物体不提交。关掉 = 全量提交（验证剔除没漏画的对照组）。"),
};
static const ParamDesc kParamsHiz[] = {
    PBool("hiz.enabled", "Hi-Z occlusion", "MYVK_HIZ", ParamTier::Basic,
          "在视锥剔除之上再剔掉\"在画面里但被挡住\"的物体。依赖深度图，两帧延迟。"),
    PFloat("hiz.bias", "Depth bias (NDC)", "MYVK_HIZ_BIAS", 0.0f, 0.05f,
           ParamTier::Advanced,
           "遮挡判定的 NDC 深度偏置：越大越保守、越少剔。", 0.0f),
};
static const ParamDesc kParamsLod[] = {
    PBool("lod.enabled", "LOD", "MYVK_LOD", ParamTier::Basic,
          "关掉 = 全部用 LOD0（对照\"低模没把画面画坏\"）。"),
    PFloatPos("lod.bias", "LOD bias", "MYVK_LOD_BIAS", 0.1f, 4.0f,
              ParamTier::Advanced,
              "全局偏差：>1 更早切低模、<1 更晚切（更保细节）。"),
    PInt("lod.force", "Force level", "MYVK_LOD_FORCE", -1.0f, 8.0f,
         ParamTier::Debug,
         "-1 = 按距离自动；>=0 = 所有物体强制用该层级（看\"某一级长什么样\"）。"),
};

// ---- 天空 / IBL / 表面 ----
static const ParamDesc kParamsSky[] = {
    PBool("sky.enabled", "Skybox", "MYVK_SKY", ParamTier::Basic,
          "背景画不画天空盒。关掉露出 HDR 清屏色。"),
    PBool("sky.ibl", "IBL", "MYVK_IBL", ParamTier::Basic,
          "环境光走 IBL 还是旧的程序化天际梯度近似。与 Sky 分开是为了"
          "把\"背景变了\"和\"物体着色变了\"两件事各自归因。"),
};
static const ParamDesc kParamsSss[] = {
    PFloat("sss.curvature", "Curvature scale", "MYVK_SSS", 0.0f, 24.0f,
           ParamTier::Basic,
           "屏幕空间法线导数 → LUT 曲率轴的映射倍率。= 0 让 LUT 退化成"
           " Lambert（关掉 SSS 的对照组）。只影响 ShadingModel=Skin。"),
};
static const ParamDesc kParamsPom[] = {
    PFloat("pom.coverage", "Height scale", "MYVK_POM", 0.0f, 3.0f,
           ParamTier::Basic,
           "全局倍率兼开关，乘在材质的 heightScale 上。= 0 整段短路。"),
    mkParam("pom.fade", "Fade range (m)", "MYVK_POM_FADE", ParamKind::Float,
            ParamTier::Advanced, 0.0f, 80.0f, EnvMode::FloatPair,
            "距离淡出区间：超出终点完全退回法线贴图（env 写 \"起点,终点\"）。"),
    PFloat("pom.maxLayers", "Max layers", "MYVK_POM_LAYERS", 4.0f, 128.0f,
           ParamTier::Advanced, "步进的最大层数（典型 16~64）。", 4.0f),
};

// ---- 级联阴影 ----
static const ParamDesc kParamsShadow[] = {
    PBool("shadow.enabled", "Shadows", "MYVK_SHADOW", ParamTier::Basic,
          "总开关。关掉时着色器里 shadowParams.w = 0。"),
    mkParam("shadow.cascades", "Cascades", "MYVK_SHADOW_CASCADES",
            ParamKind::Int, ParamTier::Basic, 1.0f, 4.0f, EnvMode::IntRaw,
            "1 = 旧单张图的行为。切 1↔4 一眼看出 CSM 改善的是哪一段。",
            kNoMin, kNoMax, 1.0f, 4.0f),
    PFloatPos("shadow.distance", "Max distance (m)", "MYVK_SHADOW_DIST",
              5.0f, 200.0f, ParamTier::Basic,
              "最远阴影距离（距离分级的硬边界）。"),
    mkParam("shadow.lambda", "Split lambda", "MYVK_SHADOW_LAMBDA",
            ParamKind::Float, ParamTier::Advanced, 0.0f, 1.0f,
            EnvMode::FloatAnySign,
            "级联分割的 log/uniform 混合系数：0 = 均匀、1 = 对数。",
            kNoMin, kNoMax, 0.0f, 1.0f),
    mkParam("shadow.resolution", "Resolution (px)", "MYVK_SHADOW_RES",
            ParamKind::Int, ParamTier::Advanced, 512.0f, 4096.0f,
            EnvMode::IntRaw,
            "每级分辨率（4 级各一张 → 显存是它的 4 倍）。", 256.0f),
    PUFloat("shadow.fadeStart", "Fade start", 0.0f, 1.0f, ParamTier::Advanced,
            "阴影尾端的淡出起点（占本级跨度的比例）。"),
    // ---- 阴影硬化三件套（R3-1）----
    PBoolAto("shadow.texelSnap", "Texel snap", "MYVK_SHADOW_SNAP",
             ParamTier::Advanced,
             "球心光空间纹素量化（R3-1 ③）。默认开 = 既有行为。"),
    PFloatClamp("shadow.blend", "Cascade blend", "MYVK_SHADOW_BLEND", 0.0f,
                0.5f, ParamTier::Advanced, 0.0f, 0.5f,
                "级联间混合带宽（R3-1 ①），占本级跨度的比例。0 = 关。"),
    PFloatClamp("shadow.harden", "Contact harden", "MYVK_SHADOW_HARDEN", 0.0f,
                1.0f, ParamTier::Advanced, 0.0f, 1.0f,
                "接触硬化强度（R3-1 ②）。0 = 关。"),
};

// ---- 局部光源阴影 ----
static const ParamDesc kParamsLocalShadow[] = {
    PBool("lshadow.enabled", "Local shadows", "MYVK_LOCAL_SHADOW",
          ParamTier::Basic,
          "点光/聚光灯投影。图仍渲染，关掉只是片元不再采样。"),
    mkParam("lshadow.maxLights", "Max lamps", "MYVK_LOCAL_SHADOW_LIGHTS",
            ParamKind::Int, ParamTier::Basic, 0.0f, 12.0f, EnvMode::IntRaw,
            "本帧最多几盏灯投影（按到相机距离由近到远取）。",
            kNoMin, kNoMax, 0.0f, 1.0e9f),
    mkParam("lshadow.resolution", "Layer size (px)", "MYVK_LOCAL_SHADOW_RES",
            ParamKind::Int, ParamTier::Advanced, 512.0f, 4096.0f,
            EnvMode::IntRaw,
            "每层分辨率。⚠ 改它需要重建资源（初始化时生效）。", 128.0f),
    PVec3("lshadow.bias", "Bias (const, slope, PCF)", "MYVK_LOCAL_SHADOW_BIAS",
          ParamTier::Advanced, 0.0f, 1.0f,
          "三个分量：常数偏移(米) / 斜率偏移(米) / PCF 半径(纹素)。"
          "env 可以只写前一到两个（未写的保持原值）。"),
};

// ---- 光源修饰（R3-3）----
static const ParamDesc kParamsLightMod[] = {
    PBool("lm.ies", "IES profile", "MYVK_IES", ParamTier::Basic,
          "按 .ies 光域网重分布强度。关着时逐灯修饰记录全零。"),
    PBool("lm.cookie", "Light function (cookie)", "MYVK_COOKIE",
          ParamTier::Basic, "投影贴图调制（如\"窗框光\"）。"),
    PBool("lm.areaLight", "Rect area light", "MYVK_AREA_LIGHT",
          ParamTier::Basic, "矩形区域光（按面积积分）。"),
};

// ---- 屏幕空间：SSAO / 雾 / SSGI / SSR / God rays ----
static const ParamDesc kParamsSsao[] = {
    PBool("ssao.enabled", "SSAO", "MYVK_SSAO", ParamTier::Basic,
          "屏幕空间环境光遮蔽。结果经 set0/binding14 进 pbr.frag。"),
    PFloat("ssao.radius", "Radius (m)", "MYVK_SSAO_RADIUS", 0.05f, 3.0f,
           ParamTier::Basic, "采样半径（米）。"),
    PFloat("ssao.strength", "Strength", "MYVK_SSAO_STRENGTH", 0.0f, 2.0f,
           ParamTier::Basic, "遮蔽强度（1 = 标准）。"),
    PUFloat("ssao.bias", "Depth bias (m)", 0.0f, 0.2f, ParamTier::Advanced,
            "防自遮蔽条纹的深度偏置（米）。无 env。"),
};
static const ParamDesc kParamsHeightFog[] = {
    PBool("hfog.enabled", "Height fog", "MYVK_FOG", ParamTier::Basic,
          "解析式指数高度雾（按世界高度衰减），走 FrameUBO.fogParams，"
          "每帧生效、零重建。与下面的 Froxel 体积雾是**两条独立路径**。"),
    PFloat("hfog.density", "Density", "MYVK_FOG_DENSITY", 0.0f, 0.2f,
           ParamTier::Basic, "水平方向的基础密度。"),
    PFloat("hfog.falloff", "Height falloff (1/m)", "MYVK_FOG_FALLOFF", 0.0f,
           0.5f, ParamTier::Advanced, "越高越稀的衰减系数。"),
};
static const ParamDesc kParamsVfog[] = {
    PBool("vfog.enabled", "Volumetric fog (froxel)", "MYVK_VFOG",
          ParamTier::Basic,
          "逐 froxel 沿视线积分内散射/消光。有体积散射与局部形状，"
          "与解析高度雾不同。"),
    PFloat("vfog.density", "Density", "MYVK_VFOG_DENSITY", 0.0f, 0.3f,
           ParamTier::Basic, "基础密度（1/米）。"),
    PFloat("vfog.albedo", "Albedo", "MYVK_VFOG_ALBEDO", 0.0f, 1.0f,
           ParamTier::Advanced, "散射/消光比（1 = 纯散射）。"),
    PFloat("vfog.falloff", "Height falloff", "MYVK_VFOG_FALLOFF", 0.0f, 0.5f,
           ParamTier::Advanced, "高度衰减（1/米）。"),
    PFloat("vfog.blob", "Blob strength", "MYVK_VFOG_BLOB", 0.0f, 6.0f,
           ParamTier::Advanced, "局部雾团强度（0 = 均匀介质）。"),
    PFloat("vfog.phaseG", "Phase g", "MYVK_VFOG_PHASE_G", 0.0f, 0.95f,
           ParamTier::Advanced,
           "HG 相位各向异性：>0 前向散射 → 灯/太阳附近出现光柱。"),
};
static const ParamDesc kParamsSsgi[] = {
    PBool("ssgi.enabled", "SSGI", "MYVK_SSGI", ParamTier::Basic,
          "屏幕空间全局光（加色注入的间接光近似）。"),
    PFloat("ssgi.strength", "Strength", "MYVK_SSGI_STRENGTH", 0.0f, 2.0f,
           ParamTier::Basic, "间接光注入强度。"),
    PFloat("ssgi.radius", "Radius (m)", "MYVK_SSGI_RADIUS", 0.1f, 5.0f,
           ParamTier::Advanced, "采样半径（米），决定\"弹光\"的邻域范围。"),
};
static const ParamDesc kParamsSsr[] = {
    PBool("ssr.enabled", "SSR", "MYVK_SSR", ParamTier::Basic,
          "屏幕空间反射。"),
    PFloat("ssr.strength", "Strength", "MYVK_SSR_STRENGTH", 0.0f, 1.5f,
           ParamTier::Basic, "反射叠加强度（菲涅尔再乘一层）。", 0.0f),
};
static const ParamDesc kParamsGodrays[] = {
    PBool("godrays.enabled", "God rays", "MYVK_GODRAYS", ParamTier::Basic,
          "屏幕空间径向体积光（太阳光柱）。"),
    PFloat("godrays.strength", "Strength", "MYVK_GODRAYS_STRENGTH", 0.0f,
           1.5f, ParamTier::Basic, "光柱强度。", 0.0f),
};
static const ParamDesc kParamsDecals[] = {
    PBool("decals.enabled", "Decals (DBuffer)", "MYVK_DECALS",
          ParamTier::Basic,
          "延迟贴花：盒体体积写三张 DBuffer（反照率/世界法线/粗糙度），"
          "前向按 coverage 混合。关着时一个 pass 都不录。"),
};
static const ParamDesc kParamsProbe[] = {
    PBool("probe.enabled", "Reflection Probes", "MYVK_PROBES",
          ParamTier::Basic,
          "反射探针（R5-1）：场景里的盒体从中心烘一张立方体贴图，"
          "盒内表面的镜面反射从它取 —— 补上 SSR 按定义做不到的"
          "「屏幕外的东西」。关着时一次捕获都不录、资源也不分配。"),
    PFloat("probe.intensity", "Intensity", "MYVK_PROBE_INTENSITY", 0.0f, 4.0f,
           ParamTier::Basic, "反射强度倍率（乘在探针的镜面项上）。"),
    PInt("probe.captureSize", "Capture size (px)", "MYVK_PROBE_SIZE", 16, 1024,
         ParamTier::Advanced,
         "每个立方体面的捕获分辨率。换它会重建整套探针资源，"
         "验证脚本用小值把捕获压到毫秒级。"),
    PInt("probe.autoFrames", "Auto recapture (frames)", "MYVK_PROBE_AF", 0, 600,
         ParamTier::Advanced,
         "定期重捕周期（帧）。**0 = 关**（默认）：只在探针数据变了 / "
         "按了 Capture 时烘。静态场景定期重捕的产物与上一份逐位相同，"
         "纯属白烧带宽；动态场景（移动的物体 / 灯）才给它一个非零值。"),
};

// ---- R6-1 GPU 粒子 ----
//
// 粒子本体（发射器）是 ECS 组件 —— 发射率 / 寿命 / 形状 / 曲线 / 颜色
// 全在组件里，走 Inspector 的粒子分区。这里绑的是**全局**设置，两者
// 正交：最终尺寸 = 组件曲线 × fx.sizeScale，最终亮度 = 组件颜色 ×
// fx.intensity。
//
// 为什么不把组件的参数也搬进 Engine 面板：它们**每个发射器都不一样**
//（火焰是加色 + 上飘，水花是 Alpha + 下坠），放进全局面板就变成
// "一改全改"。反过来容量 / 模拟频率这类**装机参数**（MYVK_FX_CAPACITY /
// _EMITTERS / _HZ）故意不接 UI —— 它们决定显存布局，改一次要 waitIdle
// 重建整套资源。但仍然必须在 check_feature_surface.py 里登记，否则
// D 组会把它们报成"裸 env 漏网"（与探针的 MAX / PREFILTER_* 同一个取舍）。
static const ParamDesc kParamsFx[] = {
    PBool("fx.enabled", "GPU Particles", "MYVK_FX", ParamTier::Basic,
          "GPU 粒子（R6-1）：发射器的出生 / 老化 / 死亡 / 积分 / 曲线"
          "全在 compute 里算，紧凑化后由**一次间接绘制**画完（实例数由"
          "GPU 自己数，CPU 全程不知道有几颗粒子）。关着时一个 pass 都不录、"
          "显存也不分配 —— 没有粒子组件的场景与引入本模块之前逐位一致。"),
    PFloat("fx.sizeScale", "Size scale", "MYVK_FX_SIZE", 0.0f, 4.0f,
           ParamTier::Basic, "全局尺寸倍率（乘在每颗粒子的尺寸上）。"),
    PFloat("fx.softness", "Softness", "MYVK_FX_SOFTNESS", 0.0f, 4.0f,
           ParamTier::Basic,
           "软粒子淡化距离的全局倍率（× 各发射器的 Soft distance）。"
           "**0 = 关**：只剩硬遮挡，粒子贴到墙上的地方会露出方形的切边；"
           "调大则粒子在离表面越近时越透明，烟 / 水花贴地看起来才对。"),
    PFloat("fx.intensity", "Intensity", "MYVK_FX_INTENSITY", 0.0f, 8.0f,
           ParamTier::Basic,
           "亮度倍率（乘在颜色上）。加色混合的火花 / 火焰靠它调到想要的"
           "曝光 —— 它直接改变泛光的触发量。"),
    PInt("fx.freezeFrame", "Freeze frame", "MYVK_FX_FREEZE", 0, 100000,
         ParamTier::Debug,
         "冻结帧：> 0 时模拟推进到这一帧就停住（不再派发 compute），"
         "画面随之完全确定。**0 = 不冻结**（正常使用）。自动化验证需要"
         "「同一配置两次运行逐像素一致」，而实时模拟的帧号在抓拍时无法"
         "对齐 —— 这个开关就是那个对齐点（与 MYVK_TAA_PHASE 冻结抖动"
         "相位是同一条取舍）。"),
};

// ---- TAA ----
static const ParamDesc kParamsTaa[] = {
    PBool("taa.enabled", "TAA", "MYVK_TAA", ParamTier::Basic,
          "时域抗锯齿：帧首给投影加 Halton(2,3) 子像素抖动，多跑一个"
          " resolve pass。是屏幕空间链的**头**。"),
    PFloat("taa.blend", "Blend", "MYVK_TAA_BLEND", 0.01f, 1.0f,
           ParamTier::Advanced,
           "当前帧权重：越小越稳、越容易留拖影（0.1 ≈ 10 帧收敛）。"),
    PFloat("taa.clamp", "Neighbour clamp sigma", "MYVK_TAA_CLAMP", 0.0f, 4.0f,
           ParamTier::Advanced,
           "邻域方差钳制：>2 更保历史（易鬼影）、<0.5 更激进杀鬼影（会闪）。"),
};

// ---- 后处理（曝光 / tonemap / bloom / FXAA）----
static const char* const kTonemapNames[] = {"ACES", "Reinhard",
                                            "Clamp (raw HDR)", nullptr};
static const ParamDesc kParamsPost[] = {
    PFloat("post.exposure", "Exposure", "MYVK_EXPOSURE", 0.05f, 4.0f,
           ParamTier::Basic,
           "tonemap 之前的手动曝光（自动曝光关闭时生效）。"),
    PEnum("post.tonemap", "Tonemap", nullptr, ParamTier::Basic, kTonemapNames,
          "0 = ACES、1 = Reinhard、2 = Clamp(raw HDR)。"),
    PUFloat("post.bloomThreshold", "Bloom threshold", 0.0f, 4.0f,
            ParamTier::Basic, "亮部阈值：>1 只让真正的高光发光。"),
    PUFloat("post.bloomKnee", "Bloom knee", 0.01f, 2.0f, ParamTier::Advanced,
            "软过渡带宽。"),
    PUFloat("post.bloomStrength", "Bloom strength", 0.0f, 2.0f,
            ParamTier::Basic, "bloom 叠加强度（0 = 关闭）。"),
    PBool("post.fxaa", "FXAA", "MYVK_FXAA", ParamTier::Basic,
          "tonemap 之后的快速抗锯齿，补 MSAA 处理不了的贴图边缘锯齿。"),
};
static const ParamDesc kParamsAutoExp[] = {
    PBool("ae.enabled", "Auto exposure", "MYVK_AUTOEXP", ParamTier::Basic,
          "直方图 eye adaptation。开启后用自适应曝光替代 Exposure 滑条"
          "（滑条值保留为起始/回退值）。"),
    PFloat("ae.key", "Key (middle grey)", "MYVK_AE_KEY", 0.01f, 1.0f,
           ParamTier::Basic, "目标中间灰：让\"平均亮度 × 曝光\"落在这里。"),
    PFloat("ae.speed", "Adapt speed (1/s)", "MYVK_AE_SPEED", 0.1f, 12.0f,
           ParamTier::Advanced, "越大收敛越快、越容易\"呼吸\"。"),
    PFloatAny("ae.bias", "EV bias", "MYVK_AE_BIAS", -4.0f, 4.0f,
              ParamTier::Basic, "+1 = 亮一档。后处理体积可按位置覆盖。"),
    PFloat("ae.min", "Exposure min", "MYVK_AE_MIN", 0.005f, 1.0f,
           ParamTier::Advanced, "夹紧下界（防对着纯亮场景发散）。"),
    PFloat("ae.max", "Exposure max", "MYVK_AE_MAX", 1.0f, 16.0f,
           ParamTier::Advanced, "夹紧上界（防对着纯黑场景发散）。"),
};
static const ParamDesc kParamsColorGrading[] = {
    PBool("cg.enabled", "Color grading (LUT)", "MYVK_CG", ParamTier::Basic,
          "lift/gamma/gain/contrast/saturation 在 CPU 侧烘成 32³ LUT，"
          "合成里一次三线性采样（tonemap 后、暗角前）。"),
    PFloat("cg.blend", "LUT blend", "MYVK_CG_BLEND", 0.0f, 1.0f,
           ParamTier::Basic, "LUT 混合权重。"),
    PColor3("cg.lift", "Lift", "MYVK_CG_LIFT", ParamTier::Basic,
            "阴影抬升（可负）。"),
    PColor3("cg.gamma", "Gamma", "MYVK_CG_GAMMA", ParamTier::Basic,
            "中间调 gamma。"),
    PColor3("cg.gain", "Gain", "MYVK_CG_GAIN", ParamTier::Basic,
            "高光增益。"),
    // ⚠ 这两个**有** env（verify_color_lut 的 4 个用例都靠它们做"改一个
    //   参数、其余不动"的对照）。曾经被我当成"纯 UI 参数"漏掉 env，
    //   检查脚本的 H 组（注册表接管度）当场点名 —— 这条注释是那次事故的
    //   标记，别再把它们改回 PUFloat。
    PFloat("cg.contrast", "Contrast", "MYVK_CG_CONTRAST", 0.2f, 3.0f,
           ParamTier::Basic, "绕 0.5 的对比度。"),
    PFloat("cg.saturation", "Saturation", "MYVK_CG_SATURATION", 0.0f, 3.0f,
           ParamTier::Basic, "绕亮度的饱和度。"),
};
static const ParamDesc kParamsDrs[] = {
    PBool("drs.enabled", "Dynamic resolution", "MYVK_DRS", ParamTier::Basic,
          "场景按 renderScale 缩小渲染，合成时边缘感知升采样回全分辨率。"
          "关着时 renderScale 恒 1.0 → 逐像素回到基线。"),
    PBool("drs.auto", "Auto (by GPU time)", "MYVK_DRS_AUTO", ParamTier::Basic,
          "按 GPU 帧耗时自动调缩放比；关掉则用手动 renderScale。"),
    // ⚠ 必须是 NN（拒绝负数 / NaN）再夹紧 —— 原代码是
    //   `envFloat(...)` 然后钳到 [0.25, 1.0]，负数会被**拒绝**保持默认，
    //   而不是夹到 0.25（PFloatClamp 的语义），两者结果不同。
    PFloatClampNN("drs.scale", "Render scale", "MYVK_RENDER_SCALE", 0.25f, 1.0f,
                  ParamTier::Basic, 0.25f, 1.0f,
                  "手动缩放比（自动模式下由控制器更新）。"),
    PFloat("drs.targetMs", "Target GPU ms", "MYVK_DRS_TARGET_MS", 4.0f, 33.0f,
           ParamTier::Advanced, "自动模式的调节目标。"),
    PFloat("drs.minScale", "Min scale", "MYVK_DRS_MIN", 0.25f, 1.0f,
           ParamTier::Advanced, "缩放夹紧下界。"),
    PFloat("drs.maxScale", "Max scale", "MYVK_DRS_MAX", 0.25f, 1.0f,
           ParamTier::Advanced, "缩放夹紧上界。"),
    PFloat("drs.sharpness", "Sharpen", "MYVK_DRS_SHARP", 0.0f, 1.5f,
           ParamTier::Advanced, "升采样的边缘锐度（0 = 纯双线性）。"),
};

// ---- 相机级后处理 ----
static const ParamDesc kParamsDof[] = {
    PBool("dof.enabled", "DoF", "MYVK_DOF", ParamTier::Basic,
          "景深（深度驱动的散景模糊）。"),
    PFloat("dof.focus", "Focus (m)", "MYVK_DOF_FOCUS", 0.1f, 60.0f,
           ParamTier::Basic, "对焦距离：此距离处完全清晰。"),
    PFloat("dof.aperture", "Aperture", "MYVK_DOF_APERTURE", 0.0f, 8.0f,
           ParamTier::Basic, "光圈：离焦量 → CoC 的比例系数。"),
    PFloat("dof.maxRadius", "Max radius (px)", "MYVK_DOF_RADIUS", 1.0f, 40.0f,
           ParamTier::Advanced, "CoC 上限（像素）。"),
};
static const ParamDesc kParamsMotionBlur[] = {
    PBool("mb.enabled", "Motion blur", "MYVK_MOTIONBLUR", ParamTier::Basic,
          "相机运动模糊（屏幕空间重投影速度）。"),
    PFloat("mb.strength", "Strength", "MYVK_MB_STRENGTH", 0.0f, 4.0f,
           ParamTier::Basic, "速度缩放（>1 更夸张）。"),
    mkParam("mb.samples", "Samples", "MYVK_MB_SAMPLES", ParamKind::Int,
            ParamTier::Advanced, 2.0f, 16.0f, EnvMode::IntFromFloat,
            "沿速度方向的采样数（2..16）。"),
    PFloat("mb.simYaw", "Sim yaw (deg)", "MYVK_MB_SIM_YAW", 0.0f, 360.0f,
           ParamTier::Debug,
           "验证钩子：静态相机下人为制造偏航角速度（正常使用 = 0）。"),
};
static const ParamDesc kParamsChroma[] = {
    PBool("chroma.enabled", "Chromatic aberration", "MYVK_CHROMA",
          ParamTier::Basic, "径向 RGB 分离。"),
    PFloat("chroma.strength", "Chroma", "MYVK_CHROMA_STRENGTH", 0.0f, 3.0f,
           ParamTier::Basic, "边缘处的色散（占屏幕半径的比例，%）。"),
};
static const ParamDesc kParamsVignette[] = {
    PUBool("vig.enabled", "Vignette", ParamTier::Basic,
           "暗角。引入本模块前就在合成 push 里，默认 0.25。"),
    PFloat("vig.strength", "Vignette strength", "MYVK_VIGNETTE_STRENGTH", 0.0f,
           1.0f, ParamTier::Basic, ""),
    PFloatPos("vig.power", "Falloff power", "MYVK_VIGNETTE_POWER", 0.25f, 4.0f,
              ParamTier::Advanced, ">1 把暗角更压向边缘。"),
};

// ---- 渲染路径（管线级开关）----
static const ParamDesc kParamsRendering[] = {
    PBool("rp.depthPrepass", "Depth prepass", "MYVK_DEPTH_PREPASS",
          ParamTier::Basic,
          "前向之前先跑一遍深度。关掉会让 Hi-Z/MDI 的输入失效"
          "（MDI 打开时会自动带上）。"),
    PBool("rp.clusterCull", "Cluster culling", "MYVK_CLUSTER_CULL",
          ParamTier::Basic,
          "分簇光照里按簇剔除灯：一簇内一盏灯都不影响就不写进那张簇的灯表。"),
    PBool("rp.instancing", "GPU instancing", "MYVK_INSTANCING",
          ParamTier::Basic,
          "合批提交。开着的结果必须与关着逐像素一致（回归核心断言）。"),
    PBool("rp.mdi", "Multi-draw indirect", "MYVK_MDI", ParamTier::Advanced,
          "把\"一批一 draw\"压成一次 vkCmdDrawIndexedIndirect。"
          "打开会自动带上遮挡剔除 + 合批 + 深度预通道。"),
    PBool("rp.bindless", "Bindless textures", "MYVK_BINDLESS",
          ParamTier::Advanced,
          "贴图进 set1 的运行时数组，逐 draw 只 push 材质页号。"
          "⚠ 切换会重建整组前向管线。"),
    PBool("rp.gizmos", "Light gizmos", "MYVK_LIGHT_GIZMOS", ParamTier::Advanced,
          "每盏灯画一个自发光小球（渲染器的调试设施，不是场景资产）。"),
    PBool("rp.extraView", "Extra render target", "MYVK_EXTRA_VIEW",
          ParamTier::Debug, "额外建一个离屏目标（通用 RT 演示/验证）。"),
};

// ---- 纹理能力 ----
static const ParamDesc kParamsTexture[] = {
    PBool("tex.mipmaps", "Generate mipmaps", "MYVK_MIPMAP", ParamTier::Advanced,
          "RGBA8 纹理生成 mip 链。"),
    PBool("tex.stream", "DDS mip streaming", "MYVK_TEXSTREAM",
          ParamTier::Debug, "DDS 的 mip 分帧补传。"),
    PFloatPos("tex.aniso", "Anisotropy override", "MYVK_ANISO", 0.0f, 16.0f,
              ParamTier::Advanced,
              "0 = 用设备上限；1 = 强制各向同性。"),
};

// ---- 变体 / 验证钩子 ----
static const ParamDesc kParamsVariants[] = {
    PInt("var.forceMask", "Force keyword mask", "MYVK_SHADER_FORCE_MASK", -1.0f,
         static_cast<float>(kKeywordAll), ParamTier::Debug,
         "-1 = 按材质推导；>=0 = 无视材质，所有绘制项都用掩码 n。"
         "逐个变体做对照实验用。",
         -1.0f, static_cast<float>(kKeywordAll)),
    PInt("var.taaPhase", "Freeze TAA phase", "MYVK_TAA_PHASE", -1.0f, 7.0f,
         ParamTier::Debug,
         "-1 = 不冻结；0..7 = 冻结到该 Halton 相位（配合 MYVK_TAA=0 与"
         " MSAA=1 可离线平均出参考图）。",
         -1.0f, 7.0f),
};

// ---- 视口栅格参数（E02/E09 进注册表，由生成器出 UI）----
// 开关**不在这一组**：它由视口右上角浮层 / View 菜单控制，且每帧由
// 编辑器按"编辑态"同步（Play 期间强制关）。这里只有"调起来才知道合
// 不合适"的细节参数，全部纯 UI（无 env）。
static const ParamDesc kParamsGrid[] = {
    PUFloat("grid.minorStep", "Minor step (m)", 0.1f, 10.0f,
            ParamTier::Basic, "细格间距（米）。"),
    PUFloat("grid.majorStep", "Major (m)", 1.0f, 100.0f, ParamTier::Basic,
            "粗格间距（米）。"),
    PUFloat("grid.extent", "Extent (m)", 5.0f, 500.0f, ParamTier::Advanced,
            "栅格半径（米）。"),
    PUFloat("grid.alpha", "Alpha", 0.0f, 1.0f, ParamTier::Advanced,
            "线不透明度。"),
    PUFloat("grid.lineWidthPx", "Line width (px)", 0.5f, 5.0f,
            ParamTier::Advanced, "线宽（像素）。"),
    PUFloat("grid.fadeStart", "Edge fade start (m)", 0.0f, 300.0f,
            ParamTier::Advanced, "距栅格中心开始淡出的距离（不是距相机）。"),
    PUFloat("grid.fadeEnd", "Edge fade end (m)", 5.0f, 300.0f,
            ParamTier::Advanced, "完全淡出的距离（被 extent 与远平面夹住）。"),
};

// ---- 游戏 UI / HUD（U0，见 docs/ui_hud_roadmap.md）----
// 这是**玩家看的** UI（血条 / 准星 / 伤害数字 / 暂停菜单），与
// engine/ui/（编辑器的 ImGui）是两条通道，互不共享资源，所以参数
// 也单独一组：关掉 HUD 不该动到任何后处理参数。
//
// ⚠ 默认值的设计约束（每次加参数都要过一遍）：
//   · ui.enabled=true 但**没有 UI 数据** → 零绘制、不进任何 pass；
//   · ui.hudInEditor=false → 编辑器截图逐位不变（几十条基线）。
//   所以"打开这个特性"与"改变现有画面"是两件事。
static const ParamDesc kParamsUi2[] = {
    PBool("ui.enabled", "Game UI", "MYVK_UI", ParamTier::Basic,
          "运行时 UI/HUD 总开关。关掉 = 一个像素都不画（与没有这条通道时逐位相同）。"),
    PBool("ui.hudInEditor", "HUD in editor", "MYVK_UI_HUD_IN_EDITOR",
          ParamTier::Advanced,
          "在编辑器视口里也画 HUD（预览用）。默认关：打开会让编辑器截图像素整体漂。"),
    PFloat("ui.scale", "UI scale", "MYVK_UI_SCALE", 0.5f, 3.0f,
           ParamTier::Basic,
           "逻辑像素 → 输出像素的额外倍率。生效值 = min(W/1920,H/1080) × 本值。"),
    PBool("ui.debugBounds", "Show UI bounds", "MYVK_UI_DEBUG_BOUNDS",
          ParamTier::Debug,
          "把每个控件的最终矩形画成描边并打 UI2-BOUNDS 锚点（自动化读布局用）。"),
    PFloat("ui.fontSize", "Font size", "MYVK_UI_FONT_SIZE", 8.0f, 64.0f,
           ParamTier::Basic,
           "默认字号（逻辑像素）。SDF 字体任意字号锐利，不需要重新烘焙。"),
};

// ============================================================
#define ARRAY(a) a, static_cast<int>(sizeof(a) / sizeof((a)[0]))

const FeatureDesc kFeatures[] = {
    // ---- 剔除 / LOD ----
    {"cull", "Frustum Culling", "Culling", nullptr, nullptr,
     "verify_frustum_cull", ARRAY(kParamsCull)},
    {"hiz", "Hi-Z Occlusion Culling", "Culling", "R2-1", nullptr, "verify_hiz",
     ARRAY(kParamsHiz)},
    {"lod", "Level of Detail", "Culling", nullptr, nullptr, "verify_lod",
     ARRAY(kParamsLod)},

    // ---- 天空 / 表面 ----
    {"sky", "Skybox + IBL", "Sky", nullptr, nullptr, "verify_ibl",
     ARRAY(kParamsSky)},
    {"sss", "Pre-integrated SSS", "Surface", "R4-2", nullptr, "verify_sss",
     ARRAY(kParamsSss)},
    {"pom", "Parallax Occlusion Mapping", "Surface", "R4-3", nullptr,
     "verify_pom", ARRAY(kParamsPom)},

    // ---- 阴影 ----
    {"shadow", "Cascaded Shadow Maps", "Shadows", "R3-1", "shadows",
     "verify_csm", ARRAY(kParamsShadow)},
    {"lshadow", "Local Light Shadows", "Shadows", nullptr, "local_sh",
     "verify_local_shadow", ARRAY(kParamsLocalShadow)},

    // ---- 光源修饰 ----
    {"lm", "Light Modifiers", "Lights", "R3-3", nullptr,
     "verify_light_modifiers", ARRAY(kParamsLightMod)},

    // ---- 屏幕空间 ----
    {"ssao", "SSAO", "Scene", nullptr, "ssao", "verify_screenspace",
     ARRAY(kParamsSsao)},
    {"hfog", "Height Fog", "Scene", nullptr, nullptr, "verify_screenspace",
     ARRAY(kParamsHeightFog)},
    {"vfog", "Volumetric Fog", "Scene", "R3-2", "framegraph",
     "verify_volumetric_fog", ARRAY(kParamsVfog)},
    {"ssgi", "SSGI", "Scene", nullptr, "framegraph", "verify_screenspace",
     ARRAY(kParamsSsgi)},
    {"ssr", "Screen Space Reflections", "Scene", nullptr, "framegraph",
     "verify_screenspace", ARRAY(kParamsSsr)},
    {"godrays", "God Rays", "Scene", nullptr, "framegraph",
     "verify_screenspace", ARRAY(kParamsGodrays)},
    {"decals", "Deferred Decals (DBuffer)", "Scene", "R4-4", "decals",
     "verify_decals", ARRAY(kParamsDecals)},
    {"probe", "Reflection Probes", "Scene", "R5-1", nullptr,
     "verify_reflection_probe", ARRAY(kParamsProbe)},
    // GPU 粒子（R6-1）。gpu 段 = "particles"（模拟 compute ×2 + 间接绘制
    // 挤在同一个时戳段里 —— 它们是"粒子这一件事"的两半，分开计时只会
    // 让两个数都小到读不出来）。
    {"fx", "GPU Particles", "Scene", "R6-1", "particles", "verify_particles",
     ARRAY(kParamsFx)},

    // ---- 时域 ----
    {"taa", "Temporal AA", "Post", "R1-1", "framegraph", "verify_taa",
     ARRAY(kParamsTaa)},

    // ---- 后处理 ----
    // 本组跨好几个用例（tonemap/bloom/exposure 在 verify_fxaa，
    // 相机特效另算）—— 这里填主用例，其余见各 ParamDesc 的 tooltip。
    {"post", "Tone Mapping + Bloom", "Post", nullptr, "post", "verify_fxaa",
     ARRAY(kParamsPost)},
    {"ae", "Auto Exposure", "Post", "R1-2", "post", "verify_auto_exposure",
     ARRAY(kParamsAutoExp)},
    {"cg", "Color Grading LUT", "Post", "R1-3", "post", "verify_color_lut",
     ARRAY(kParamsColorGrading)},
    {"drs", "Dynamic Resolution", "Post", "R1-4", "post", "verify_drs",
     ARRAY(kParamsDrs)},

    // ---- 相机效果 ----
    {"dof", "Depth of Field", "Camera", nullptr, "framegraph", "verify_camfx",
     ARRAY(kParamsDof)},
    {"mb", "Motion Blur", "Camera", nullptr, "framegraph", "verify_camfx",
     ARRAY(kParamsMotionBlur)},
    {"chroma", "Chromatic Aberration", "Camera", nullptr, "post",
     "verify_camfx", ARRAY(kParamsChroma)},
    {"vig", "Vignette", "Camera", nullptr, "post", "verify_camfx",
     ARRAY(kParamsVignette)},

    // ---- 渲染路径 / 纹理 / 变体 ----
    // 本组是"渲染路径"这一类开关的集合（深度预通道 / 簇剔除 / 合批 /
    // MDI / bindless / gizmo / 额外目标），每个参数各有一条用例；
    // 这里填覆盖参数最多的那条，具体看 ParamDesc 的 tooltip。
    {"rp", "Rendering Path", "Rendering", nullptr, nullptr,
     "verify_instancing",
     ARRAY(kParamsRendering)},
    {"tex", "Texture Capabilities", "Rendering", "R2-3", nullptr,
     "verify_textures", ARRAY(kParamsTexture)},
    {"var", "Shader Variants", "Rendering", "R4-1", nullptr,
     "verify_shader_variants", ARRAY(kParamsVariants)},

    // ---- 视口 ----
    {"grid", "Viewport Grid", "Viewport", nullptr, nullptr, nullptr,
     ARRAY(kParamsGrid)},

    // ---- 游戏 UI / HUD（U0）----
    // test 指向 verify_ui2（tools/verify_ui2.py）：E 组会检查它确实在
    // run_regression.py 的 DEFAULT 里 —— 登记了特性却没回归是会报红的。
    {"ui2", "Game UI / HUD", "UI", "U0", nullptr, "verify_ui2",
     ARRAY(kParamsUi2)},
};

const int kFeatureCount =
    static_cast<int>(sizeof(kFeatures) / sizeof(kFeatures[0]));

#undef ARRAY

// ============================================================
// ② 运行时绑定表
// ============================================================

namespace {

struct Binding {
    std::string id;
    void* ptr = nullptr;
    void* aux = nullptr;     // FloatPair 的第二个字段
    ParamKind kind = ParamKind::Float;
    bool applied = false;    // env 是否已经应用过（分阶段调用用）
    // 文件层（RenderSettingsIO）是否已经处理过这条绑定。**必须与 applied
    // 分开**：两者都分阶段调用，而 env 永远要后写（测试钩子优先级最高）。
    bool settingsApplied = false;
    // 绑定那一刻捕获的"默认值"（此时成员刚默认初始化完、env 还没读）
    bool dBool = false;
    int dInt = 0;
    float dFloat = 0.0f;
    float dColor[3] = {0.0f, 0.0f, 0.0f};
};

std::vector<Binding> g_bindings;

const ParamDesc* g_allParams[kFeatureCount * 32];
int g_allParamCount = 0;

// 懒建：把所有 FeatureDesc 的 ParamDesc 摊平成一个数组，便于按 id 查找。
void buildParamIndex() {
    if (g_allParamCount > 0) return;
    for (int i = 0; i < kFeatureCount; ++i) {
        const FeatureDesc& f = kFeatures[i];
        for (int j = 0; j < f.paramCount; ++j) {
            if (g_allParamCount < static_cast<int>(sizeof(g_allParams) /
                                                  sizeof(g_allParams[0]))) {
                g_allParams[g_allParamCount++] = &f.params[j];
            }
        }
    }
}

// ---- 逐 EnvMode 解析（必须与 Renderer.cpp 原实现逐位等价）----

// 与 Renderer.cpp 的 envBool 同一份逻辑
bool parseEnvBool(const char* s) {
    if (s[0] == '0' || s[0] == 'f' || s[0] == 'F') return false;
    if (s[0] == 'o' && (s[1] == 'f' || s[1] == 'F')) return false;
    return true;
}

// 返回是否写入了值
bool applyEnv(const ParamDesc& d, const char* s, Binding& b) {
    switch (d.envMode) {
        case EnvMode::None:
            return false;

        case EnvMode::Bool: {
            *static_cast<bool*>(b.ptr) = parseEnvBool(s);
            return true;
        }

        case EnvMode::IntBool: {
            *static_cast<int*>(b.ptr) = (std::atoi(s) != 0) ? 1 : 0;
            return true;
        }

        case EnvMode::FloatNonNeg:
        case EnvMode::FloatPositive:
        case EnvMode::FloatAnySign: {
            const float v = static_cast<float>(std::atof(s));
            if (d.envMode == EnvMode::FloatNonNeg && !(v >= 0.0f)) return false;
            if (d.envMode == EnvMode::FloatPositive && !(v > 0.0f)) return false;
            if (v < d.minAccept || v > d.maxAccept) return false;
            float out = v;
            if (d.clampLo < d.clampHi) {
                out = out < d.clampLo ? d.clampLo
                                      : (out > d.clampHi ? d.clampHi : out);
            }
            *static_cast<float*>(b.ptr) = out;
            return true;
        }

        case EnvMode::IntRaw: {
            const float raw = static_cast<float>(std::atoi(s));
            if (raw < d.minAccept || raw > d.maxAccept) return false;
            float out = raw;
            if (d.clampLo < d.clampHi) {
                out = out < d.clampLo ? d.clampLo
                                      : (out > d.clampHi ? d.clampHi : out);
            }
            // 目标都是 32 位有符号/无符号整数；写入范围已由 clamp/minAccept
            // 保证落在 [0, 2^31)，所以位模式对两者都一样。
            *static_cast<int*>(b.ptr) = static_cast<int>(out);
            return true;
        }

        case EnvMode::IntFromFloat: {
            const float v = static_cast<float>(std::atof(s));
            if (!(v >= 0.0f)) return false;
            if (v < d.minAccept || v > d.maxAccept) return false;
            *static_cast<int*>(b.ptr) = static_cast<int>(v);
            return true;
        }

        case EnvMode::Vec3: {
            float r = 0.0f, g = 0.0f, bl = 0.0f;
            if (std::sscanf(s, "%f,%f,%f", &r, &g, &bl) != 3) return false;
            float* p = static_cast<float*>(b.ptr);
            p[0] = r; p[1] = g; p[2] = bl;
            return true;
        }

        case EnvMode::Vec3Partial: {
            // 原代码：`sscanf(...) >= 1` 就整组赋值，未解析到的分量保持原值。
            float r = 0.0f, g = 0.0f, bl = 0.0f;
            const int n = std::sscanf(s, "%f,%f,%f", &r, &g, &bl);
            if (n < 1) return false;
            float* p = static_cast<float*>(b.ptr);
            if (n >= 1) p[0] = r;
            if (n >= 2) p[1] = g;
            if (n >= 3) p[2] = bl;
            return true;
        }

        case EnvMode::FloatPair: {
            // MYVK_POM_FADE="10,26"：要求 a >= 0 且 b > a（与原代码一致）
            float a = 0.0f, c = 0.0f;
            if (std::sscanf(s, "%f,%f", &a, &c) != 2) return false;
            if (!(a >= 0.0f) || !(c > a)) return false;
            *static_cast<float*>(b.ptr) = a;
            if (b.aux) *static_cast<float*>(b.aux) = c;
            return true;
        }
    }
    return false;
}

} // namespace

// ============================================================
// 绑定 / 查找
// ============================================================

void bindParamAux(const char* id, void* ptr, void* aux, ParamKind kind) {
    buildParamIndex();
    Binding b;
    b.id = id;
    b.ptr = ptr;
    b.aux = aux;
    b.kind = kind;
    b.applied = false;

    // 捕获默认值（此刻成员刚默认初始化完、env 还没读）
    switch (kind) {
        case ParamKind::Bool:  b.dBool = *static_cast<bool*>(ptr); break;
        case ParamKind::Int:
        case ParamKind::Enum:  b.dInt = *static_cast<int*>(ptr); break;
        case ParamKind::Float: b.dFloat = *static_cast<float*>(ptr); break;
        case ParamKind::Color3:
        case ParamKind::Vec3: {
            const float* p = static_cast<float*>(ptr);
            b.dColor[0] = p[0]; b.dColor[1] = p[1]; b.dColor[2] = p[2];
            break;
        }
    }
    // 覆盖重复绑定（同一个 id 重绑时以后者为准，避免同一个地址出现两条）
    for (Binding& old : g_bindings) {
        if (old.id == b.id) { old = b; return; }
    }
    g_bindings.push_back(std::move(b));
}

void bindParam(const char* id, void* ptr, ParamKind kind) {
    bindParamAux(id, ptr, nullptr, kind);
}

int bindingCount() { return static_cast<int>(g_bindings.size()); }

void popBindingsAfter(int mark) {
    if (mark < 0) mark = 0;
    if (mark < static_cast<int>(g_bindings.size())) {
        g_bindings.resize(static_cast<size_t>(mark));
    }
}

const ParamDesc* findParam(const char* id) {
    buildParamIndex();
    for (int i = 0; i < g_allParamCount; ++i) {
        if (std::strcmp(g_allParams[i]->id, id) == 0) return g_allParams[i];
    }
    return nullptr;
}

const FeatureDesc* findFeature(const char* id) {
    for (int i = 0; i < kFeatureCount; ++i) {
        if (std::strcmp(kFeatures[i].id, id) == 0) return &kFeatures[i];
    }
    return nullptr;
}

const FeatureDesc* ownerFeature(const char* paramId) {
    for (int i = 0; i < kFeatureCount; ++i) {
        for (int j = 0; j < kFeatures[i].paramCount; ++j) {
            if (std::strcmp(kFeatures[i].params[j].id, paramId) == 0) {
                return &kFeatures[i];
            }
        }
    }
    return nullptr;
}

void* paramPtr(const char* id) {
    for (const Binding& b : g_bindings) {
        if (b.id == id) return b.ptr;
    }
    return nullptr;
}

// ============================================================
// ③-b 文件层覆盖用到的绑定视图（E03）
// ============================================================

namespace {

BindingView viewOf(const Binding& b) {
    BindingView v;
    v.id = b.id;
    v.kind = b.kind;
    v.ptr = b.ptr;
    v.aux = b.aux;
    v.dBool = b.dBool;
    v.dInt = b.dInt;
    v.dFloat = b.dFloat;
    v.dColor[0] = b.dColor[0];
    v.dColor[1] = b.dColor[1];
    v.dColor[2] = b.dColor[2];
    return v;
}

} // namespace

int takeUnappliedBindings(std::vector<BindingView>& out) {
    int n = 0;
    for (Binding& b : g_bindings) {
        if (b.settingsApplied) continue;
        b.settingsApplied = true;
        out.push_back(viewOf(b));
        ++n;
    }
    return n;
}

int collectBindings(std::vector<BindingView>& out) {
    for (const Binding& b : g_bindings) out.push_back(viewOf(b));
    return static_cast<int>(g_bindings.size());
}

void resetSettingsApplied() {
    for (Binding& b : g_bindings) b.settingsApplied = false;
}

// ============================================================
// ③ env 应用 / 复位
// ============================================================

int loadEnvOverrides() {
    int n = 0;
    for (Binding& b : g_bindings) {
        if (b.applied) continue;
        b.applied = true;    // 无论有没有 env，都标记为已处理（只处理一次）
        const ParamDesc* d = findParam(b.id.c_str());
        if (!d || !d->env) continue;
        const char* s = std::getenv(d->env);
        if (!s) continue;
        if (applyEnv(*d, s, b)) ++n;
    }
    return n;
}

bool resetParam(const char* id) {
    for (Binding& b : g_bindings) {
        if (b.id != id) continue;
        switch (b.kind) {
            case ParamKind::Bool:  *static_cast<bool*>(b.ptr) = b.dBool; break;
            case ParamKind::Int:
            case ParamKind::Enum:  *static_cast<int*>(b.ptr) = b.dInt; break;
            case ParamKind::Float: *static_cast<float*>(b.ptr) = b.dFloat; break;
            case ParamKind::Color3:
            case ParamKind::Vec3: {
                float* p = static_cast<float*>(b.ptr);
                p[0] = b.dColor[0]; p[1] = b.dColor[1]; p[2] = b.dColor[2];
                break;
            }
        }
        return true;
    }
    return false;
}

void resetToDefaults() {
    for (Binding& b : g_bindings) {
        switch (b.kind) {
            case ParamKind::Bool:  *static_cast<bool*>(b.ptr) = b.dBool; break;
            case ParamKind::Int:
            case ParamKind::Enum:  *static_cast<int*>(b.ptr) = b.dInt; break;
            case ParamKind::Float: *static_cast<float*>(b.ptr) = b.dFloat; break;
            case ParamKind::Color3:
            case ParamKind::Vec3: {
                float* p = static_cast<float*>(b.ptr);
                p[0] = b.dColor[0]; p[1] = b.dColor[1]; p[2] = b.dColor[2];
                break;
            }
        }
    }
}

// ============================================================
// ④ 自检
// ============================================================

int unboundParamCount(const char** outIds, int maxOut) {
    buildParamIndex();
    int n = 0;
    for (int i = 0; i < g_allParamCount; ++i) {
        const char* id = g_allParams[i]->id;
        bool bound = false;
        for (const Binding& b : g_bindings) {
            if (b.id == id) { bound = true; break; }
        }
        if (!bound) {
            if (outIds && n < maxOut) outIds[n] = id;
            ++n;
        }
    }
    return n;
}

void logRegistrySelfCheck() {
    const char* ids[24];
    const int n = unboundParamCount(ids, 24);
    VK_LOG_INFO("FEATURES: %d groups, %d params registered, %d bound, "
                "%d unbound",
                kFeatureCount, g_allParamCount, bindingCount(), n);
    if (n > 0) {
        std::string list;
        for (int i = 0; i < n && i < 24; ++i) {
            if (i) list += ", ";
            list += ids[i];
        }
        VK_LOG_WARN("FEATURES-UNBOUND (%d): %s%s", n, list.c_str(),
                    n > 24 ? " ..." : "");
    }
}

} // namespace render
