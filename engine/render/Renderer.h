#pragma once
// ============================================================
// render/Renderer —— 一帧的组织者
//
// Pass 编排（Clustered Forward / 分簇前向）：
//
//   ShadowPass（方向光**级联**阴影图，depth-only 2048² × 4 层 2D array；
//               每一级一次 depth-only render pass，各级各自做裁剪空间剔除）
//     → LocalShadowPass（点光 / 聚光灯阴影，depth-only 1024² × 12 层 2D array；
//               每层一次 depth-only render pass，含"未使用层只清不画"）
//       → DepthPrePass（不透明几何只写深度，MSAA 深度附件）
//         → ClusterBuild（compute：按 tile × 深度层切簇，把灯分到簇里）
//           → ForwardPass（【天空盒】→ 不透明 →【地平面栅格】→ 透明，
//                        输出到 MSAA HDR，再 resolve）
//             → PostProcess（Bloom 亮部提取 → 高斯 H/V → 合成 + Tonemap）
//               → 交换链（SRGB，硬件做线性→sRGB 编码；ImGui 画在同一 Pass）
//
// 两套阴影为什么必须是**两张图**：级联是正交投影、覆盖相机视锥切片，
// 分辨率要 2048² 才够；局部光是一盏灯一个锥体（点光 6 个 90° 面），
// 12 层已经吃满预算，只能给 1024²。硬塞进同一张 array 两边都得将就。
//
// 天空盒为什么排在最前面：它是一张全屏背景，任何几何都该盖住它。
// 它自己不写深度、也不过深度测试（depthTest/depthWrite 都关），所以
// 深度预通道铺好的深度原封不动地留给不透明与透明几何。排到最后
// 就不行了 —— 那时"背景"会盖掉地平面栅格（栅格同样不写深度，
// 它的像素深度还停在 1.0，正好落进"天空该画"的判据里）。
//
// 地平面栅格为什么插在"不透明"与"透明"之间：它是个半透明的参考层，
// 但**必须吃深度测试**（墙/箱子该挡住它），所以不能画在 ImGui 图层上；
// 而排在透明物体之前，玻璃之类的透明物才能正确地叠在它上面。
// 它自己不写深度（depthWrite=false），否则会把后面的透明物挡掉。
//
// 为什么深度预通道要**独立成一个 RenderPass**：
//   分簇的 compute 必须插在"深度写完"和"前向绘制"之间，而 compute
//   不能在一个 RenderPass 内部执行 —— 所以深度预通道与前向通道必须
//   分成两个 RenderPass，中间留给 compute。
//
// 为什么深度预通道值得做：
//   前向 Pass 的片元着色器现在要遍历"本簇的灯列表"做完整 BRDF，代价
//   比过去的单方向光高得多。先用 depth-only 管线把最近深度铺一遍，
//   主 Pass 靠它做 early-Z，被遮挡的像素根本不会进入灯循环 —— 这是
//   分簇光照能跑起来的另一半原因（分簇把"每像素处理几盏灯"降下来，
//   深度预通道把"要多少像素着色"降下来）。
//
// 描述符布局（与着色器一一对应）：
//   set 0  binding 0 : FrameUBO（每帧一份）
//          binding 1 : shadowMap（sampler2DArrayShadow，第 i 层 = 第 i 级级联）
//          binding 2 : lights SSBO（LightInstance[]）
//          binding 3 : clusters SSBO（uvec2[]）
//          binding 4 : lightIndices SSBO（uint[]）
//          binding 5 : clusterStats SSBO（uint[4]，遥测）
//          binding 6 : jointMatrices SSBO（蒙皮，VERTEX 阶段）
//          binding 7 : irradianceMap（samplerCube，IBL 漫反射）
//          binding 8 : prefilterMap（samplerCube，IBL 镜面预过滤）
//          binding 9 : brdfLut（sampler2D，split-sum 第二项）
//          binding 10: skyMap（samplerCube，天空盒背景）
//          binding 11: lightShadows SSBO（LightShadowRecord[]，逐层的局部光矩阵）
//          binding 12: localShadowMap（sampler2DArrayShadow，第 i 层 = 第 i 个局部阴影层）
//   set 1            : albedo 贴图
//   set 2            : normal 贴图
//   set 3            : ORM 贴图（R=AO G=Roughness B=Metallic）
//
// 为什么阴影图/灯数据都挤进 set 0：Vulkan 规范只保证至少 4 个
// descriptor set 可同时绑定，独立开 set 4/5 会突破这个下限。set 0
// 本就是"每帧全局"资源，放这里语义也正好。
// ============================================================

#include "rhi/VulkanCommon.h"
#include "rhi/Instance.h"
#include "rhi/Device.h"
#include "rhi/Swapchain.h"
#include "rhi/CommandPool.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Pipeline.h"

#include "render/FrameResources.h"
#include "render/Frustum.h"
#include "render/GpuTimer.h"
#include "render/RenderTypes.h"
#include "render/ScreenSpaceSettings.h"
#include "render/CameraEffects.h"
#include "render/RenderTarget.h"
#include "render/ShadowPass.h"
#include "render/LocalShadowPass.h"
#include "render/ShaderKeywords.h"
#include "render/ShaderHotReload.h"
#include "render/VariantPipelines.h"
#include "render/HdrTarget.h"
#include "render/PostProcess.h"
#include "render/ClusteredLighting.h"
#include "render/Environment.h"
#include "render/HiZOcclusion.h"
#include "render/MdiDraw.h"
#include "render/Bindless.h"

#include "ui/ImGuiManager.h"
#include "assets/Texture.h"
#include "assets/Material.h"
#include "assets/Lod.h"
#include "ecs/Entity.h"
#include "scene/Light.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {
class Window;
}

namespace scene {
class Scene;
}

namespace assets {
class Mesh;
}

namespace render {

class SSAOPass;
class VolumetricFog;
class LightTextureLibrary;
class FrameGraph;
struct FrameGraphInput;

// 地平面参考栅格（编辑器视口的坐标系参照物）。
// 一个专门的 pass：在 y = 0 平面上画一个解析式抗锯齿栅格，
// 顶点由 gl_VertexIndex 生成（没有顶点缓冲），图案在片元里按像素算。
//
// 为什么不画在 ImGui 的 draw list 上（那样确实更省事）：
//   ImGui 的图层永远压在 3D 图之上，栅格会**穿过墙和物体**露出来。
//   放进 3D pass 才能吃到深度测试 —— 墙挡住的那些格线就不该出现。
struct GridSettings {
    // 半边长（米）：栅格覆盖 [-extent, extent]²
    float extent = 60.0f;
    // 最小格 / 主格（米）。用户要求"最小基本单位 1 m"，主格取 10 m
    float minorStep = 1.0f;
    float majorStep = 10.0f;
    // 平面高度：正好在地板所在平面上（Z-up：地平面是 z=0；靠着色器里的
    // 深度偏移压 z-fighting）
    float planeOffset = 0.0f;
    // 线色与总不透明度（线性空间 —— 这个 pass 输出到 HDR 目标，不做 gamma）
    glm::vec3 color{0.72f, 0.76f, 0.82f};
    float alpha = 0.55f;
    // 最小格线的相对强度（主格固定 1.0）
    float minorStrength = 0.55f;
    // 线宽（像素）与**边界淡出**范围（米）——
    // 注意 fadeStart / fadeEnd 是"距栅格中心"的距离，**不是**距相机的距离：
    // 栅格是固定不动的一块地，它的样子不该因为相机挪动就变（按相机距离
    // 淡出等于让一圈"没有栅格的环"跟着相机跑，飞行时非常显眼）。
    // 实际用的终点还会被 extent 与远平面夹一次，见 Renderer::recordFrame。
    float lineWidthPx = 1.4f;
    float fadeStart = 45.0f;
    float fadeEnd = 60.0f;
};

// R4-2：皮肤预积分散射的曲率缩放默认值。
// 把"屏幕空间法线导数（无量纲）"映射到 SSS LUT 的曲率轴 0..1。
// 8.0 是实测值：脸颊这种大平面几乎为 0，轮廓/鼻梁/耳缘落到 0.4~1.0。
// 环境变量 MYVK_SSS 可以覆盖它（=0 时关掉 SSS，作对照组）。
static constexpr float kSssDefaultCurvatureScale = 8.0f;

class Renderer {
public:
    explicit Renderer(core::Window& window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void init();  // 创建全部 Vulkan 资源
    void drawFrame(scene::Scene& scene);

    // 资源上下文（供 AssetManager 创建 GPU 资源）
    assets::TextureContext textureContext();

    rhi::Device& device() { return *m_device; }
    ui::ImGuiManager& ui() { return *m_imgui; }

    // 后处理参数（ImGui 面板直接改）
    PostSettings& postSettings() { return m_post->settings(); }

    // 自动曝光（R1-2）的当前值 / 开关（面板显示用）
    float autoExposureValue() const {
        return m_post ? m_post->autoExposureValue() : 1.0f;
    }
    bool autoExposureEnabled() const {
        return m_post && m_post->settings().autoExposure;
    }

    // ============================================================
    // 相机级后处理效果（景深 / 运动模糊 / 色差 / 暗角）
    //
    // 这是**基础值**（ImGui 面板改的就是它）。每帧会叠加"后处理体积"
    // （scene.postVolumes()）在相机位置处的覆盖值，得到生效值
    // （effectiveCameraEffects()）再下发：
    //   · dof / motionBlur → FrameGraph 链（前向后，读深度与相机矩阵）
    //   · chromatic / vignette → PostProcess 合成（tonemap 后，显示域）
    // 全部默认关（暗角默认 0.25 是引入本模块前的既有值）。
    // ============================================================
    CameraEffectSettings& cameraEffects() { return m_cameraFx; }
    const CameraEffectSettings& cameraEffects() const { return m_cameraFx; }
    // 本帧叠加体积覆盖后的生效值
    const CameraEffectSettings& effectiveCameraEffects() const {
        return m_effectiveCameraFx;
    }

    // ============================================================
    // 屏幕空间效果（SSAO / 高度雾 / SSGI / SSR / God rays）
    //
    // 设置对象被三个消费者共享：
    //   · SSAOPass（前向前，结果经 set0/binding14 进 pbr.frag）
    //   · FrameGraph（前向后：SSGI → SSR → God rays，输出图接管
    //     PostProcess 的输入）
    //   · pbr.frag（高度雾参数走 FrameUBO.fogParams）
    // 切开关**零资源重建**：ssao/链的激活状态变化在 drawFrame 开头
    // 被检测，必要时重写一次描述符 / 重建一次 PostProcess 输入。
    // ============================================================
    ScreenSpaceSettings& screenSpaceSettings() { return m_screenSpace; }
    const ScreenSpaceSettings& screenSpaceSettings() const {
        return m_screenSpace;
    }

    // ============================================================
    // Hi-Z 遮挡剔除（R2-1）
    //
    // 视锥剔除只问"在不在画面里"；这一项补上"在画面里但被更近的几何
    // 盖住了"。开启后每帧在**前向 pass 之后**建一张 Hi-Z 深度金字塔、
    // 把本帧的候选 AABB 逐个与它比较，结果回读到 CPU，**延迟两帧**
    // 作用到绘制列表（被判定完全遮挡的实体不进不透明/透明列表）。
    //
    // 默认**关**：关闭时不录任何 compute、不剔任何东西 → 逐像素回到基线。
    // 打开时也只剔"整块都躲在更近几何后面"的物体（判据见
    // occlusion_test.comp 顶部的一串保守条件）→ 画面不该有可见变化。
    //
    // 环境变量：MYVK_HIZ=1 开、MYVK_HIZ_BIAS 覆盖深度偏置(NDC)、
    //           MYVK_HIZ_DBG=1 打诊断日志。
    // ============================================================
    bool& occlusionCullingEnabled() { return m_occlusionCullingEnabled; }
    bool occlusionCullingEnabled() const { return m_occlusionCullingEnabled; }
    const render::HiZStats& hizStats() const {
        static const render::HiZStats kEmpty{};
        return m_hiz ? m_hiz->stats() : kEmpty;
    }
    float& hizDepthBias() { return m_hizBiasNdc; }

    // ============================================================
    // Multi-draw indirect + GPU 紧凑化（R2-2，见 render/MdiDraw.h）
    //
    // 依赖 R2-1：打开 MDI 会自动带上遮挡剔除（Hi-Z）、合批与深度预通道
    // —— 前两个是它的数据来源与前提，第三个是"本帧深度"的来源。
    // 打开后前向 pass 的不透明几何改走**每组一次** vkCmdDrawIndexedIndirect
    // （组 = 同管线 + 同材质），提交量从 O(物体数) 降到 O(材质数)。
    //
    // 默认**关**：关闭时零 compute、零间接命令 → 逐像素回到基线。
    // 环境变量：MYVK_MDI=1 开、MYVK_MDI_DBG=1 打每帧 MDI-STATS。
    // ============================================================
    bool& mdiEnabled() { return m_mdiEnabled; }
    bool mdiEnabled() const { return m_mdiEnabled; }
    const render::MdiStats& mdiStats() const {
        static const render::MdiStats kEmpty{};
        return m_mdi ? m_mdi->stats() : kEmpty;
    }

    // ============================================================
    // Bindless / descriptor indexing（R2-3，见 render/Bindless.h）
    //
    // 打开后材质不再各占 set 1..6：所有贴图进 set 1 binding 0 的运行时
    // 数组，材质页号 per-draw push 进 push 常量 —— 一个 draw 只绑 2 个
    // 描述符集（set 0 / set 1），且**纹理数量完全不影响绑定次数**。
    //
    // 默认**关**：关闭时 set 布局、.spv、push 内容全部走旧路径 →
    // 逐像素回到基线（这是所有新效果的对照约定）。
    //
    // 硬前提：设备支持 descriptor indexing 的整套特性（Device::supportsBindless）。
    // 设备不支持时即使 MYVK_BINDLESS=1 也不会打开，只打一行日志。
    // 环境变量：MYVK_BINDLESS=1 开、MYVK_BINDLESS_DBG=1 打每帧统计。
    // ============================================================
    bool bindlessEnabled() const { return m_bindlessEnabled; }
    // 运行时切换（编辑器勾选框）。代价 = 一次 waitIdle + 整组前向管线重建：
    // set 1 的布局与片元 .spv 都变了，而这两者绑在管线对象上，改不了。
    // 关闭时会把 m_bindless 整个放掉（回到"每材质 6 个 set"的旧路径）。
    void setBindlessEnabled(bool on);
    // 设备是否**有能力**开（与 bindlessEnabled() 不同：后者是"当前开着"）
    bool bindlessSupported() const {
        return m_device && m_device->supportsBindless();
    }
    const render::BindlessStats& bindlessStats() const {
        static const render::BindlessStats kEmpty{};
        return m_bindless ? m_bindless->stats() : kEmpty;
    }

    // 分簇光照（灯列表 / 簇 / 统计）
    ClusteredLighting& clusteredLighting() { return *m_lighting; }

    // MSAA 采样数。改动会重建 HdrTarget 与前向管线（代价：一次 waitIdle）
    VkSampleCountFlagBits sampleCount() const { return m_sampleCount; }
    void setSampleCount(VkSampleCountFlagBits samples);

    // ---- shader 热重载（见 render/ShaderHotReload.h）----
    // watchShaderFiles 注册要监听的 .spv（init 尾部调一次）；drawFrame
    // 每帧调 pollShaderReload 轮询，发现变化就 waitIdle + 重跑
    // createPipelines（变体族缓存清掉后下一帧按需从磁盘重读）。
    // MYVK_SHADER_HOTRELOAD=0 可关（像素级回归需要绝对稳定时用）。
    void watchShaderFiles();
    void pollShaderReload();
    // 热重载专用：重建主链 + 预览链的 PostProcess（管线随 shader 变）
    void rebuildPostChains();

    // 深度预通道开关（关掉后主 Pass 的 early-Z 失效，仅用于对比验证）
    bool& depthPrePassEnabled() { return m_depthPrePassEnabled; }

    // 分簇剔除开关（关掉后每簇塞满灯表 —— 用于验证"剔除不漏光"）
    bool& clusterCullingEnabled() { return m_clusterCullingEnabled; }

    // ============================================================
    // 物体级视锥剔除
    //
    // 每帧用**主相机**的视锥（见 m_sceneFrustum）把场景里可见的网格实体
    // 筛一遍：世界空间 AABB 完全落在视锥外的，不进入深度预通道 / 不透明 /
    // 透明任何一条几何提交路径。
    //
    // 关掉它就退回"全量提交"，用于对比验证剔除是否漏画（开关本身就是
    // 回归测试的手段：同一场景开关两次，画面必须一模一样）。
    // ============================================================
    bool& frustumCullingEnabled() { return m_frustumCullingEnabled; }
    bool frustumCullingEnabled() const { return m_frustumCullingEnabled; }

    // ============================================================
    // LOD（细节层级）
    //
    // 有 LOD 链的网格（见 assets/Lod.h）每帧按"屏幕占高比"选一层画：
    // 占得越小用越低的模。选择带迟滞（kLodHysteresis），物体正好骑在
    // 阈值上时不会每帧来回跳。
    //
    // 这里是**全局**开关与偏差 —— 单物体的覆盖以后可以走 ECS 组件；
    // 现在先把"能不能真的少画"这件事立起来。
    //   * enabled 关掉 = 全部退回 LOD0，用于对比验证"低模没画坏画面"。
    //   * bias > 1 = 更早切低模（数字越大越省、画质越低）。
    //   * forcedLevel >= 0 = 无视距离，所有物体都锁在这一级（调试用：
    //     一眼看出某一级长什么样）。
    // ============================================================
    bool& lodEnabled() { return m_lodEnabled; }
    bool lodEnabled() const { return m_lodEnabled; }
    float& lodBias() { return m_lodBias; }
    int& forcedLodLevel() { return m_forcedLodLevel; }

    // ============================================================
    // 天空盒 + IBL（见 render/Environment.h）
    //
    // 环境贴图在 init 时烘焙一次，四张图（天空立方体 / 辐照度 /
    // 预过滤 / BRDF 表）加起来不到 2 MB。这里的两个开关只决定
    // **怎么用**它们：
    //   * ibl：开 = pbr.frag 走真 IBL（split-sum）；关 = 回退到旧的
    //     程序化天空梯度近似。两者画面必须**不同**（近似就是近似），
    //     但都要在物理上说得通 —— 这是对照实验的判据。
    //   * sky：开 = 画天空盒背景；关 = 露出 HDR 原先的清屏色。
    //     用来隔离"新加的背景 pass"与"新的 IBL 数学"两件事。
    // ============================================================
    bool& iblEnabled() { return m_iblEnabled; }
    bool iblEnabled() const { return m_iblEnabled; }
    bool& skyEnabled() { return m_skyEnabled; }
    bool skyEnabled() const { return m_skyEnabled; }
    const render::Environment* environment() const { return m_environment.get(); }
    bool environmentReady() const {
        return m_environment != nullptr && m_environment->valid();
    }

    // ============================================================
    // 级联方向光阴影（CSM，见 render/ShadowPass.h）
    //
    //   * cascades：1..4。**1 = 退回单张正交图**（和旧行为等价），
    //     用来做"CSM 到底改善了哪一段"的对照实验。
    //   * distance：最远阴影距离（米）。超出它的像素完全没有阴影，
    //     并在最后 (1 - fadeStart) 比例的距离里平滑淡出 ——
    //     这就是"阴影距离分级"的硬边界。会被自动夹到相机远平面以内。
    //   * lambda：级联分割的 log / uniform 混合系数（0=全均匀 1=全对数）。
    //   * fadeStart：淡出起点占最大距离的比例（0.85 = 最后 15% 淡出）。
    //   * bias：（常数偏移, 斜率偏移, PCF 核半径(纹素), 远级联半径增长）
    // ============================================================
    bool& shadowsEnabled() { return m_shadowsEnabled; }
    bool shadowsEnabled() const { return m_shadowsEnabled; }
    uint32_t& shadowCascades() { return m_shadowCascades; }
    uint32_t shadowCascades() const { return m_shadowCascades; }
    float& shadowDistance() { return m_shadowDistance; }
    float& shadowLambda() { return m_shadowLambda; }
    float& shadowFadeStart() { return m_shadowFadeStart; }
    glm::vec4& shadowBias() { return m_shadowBias; }
    const glm::vec4& shadowBias() const { return m_shadowBias; }
    // ---- 阴影硬化三件套（R3-1）----
    bool& shadowTexelSnap() { return m_shadowTexelSnap; }
    float& shadowBlend() { return m_shadowBlend; }
    float& shadowHarden() { return m_shadowHarden; }
    const render::ShadowPass* shadowPass() const { return m_shadowPass.get(); }

    // ============================================================
    // 局部光源阴影（点光 / 聚光灯，见 render/LocalShadowPass.h）
    //
    // 与级联方向光**并列**的第二套阴影通道，共用同一张
    // 1024² × 12 层的 2D array 深度图：
    //   * 聚光灯占 1 层（沿锥轴一次透视投影）；
    //   * 点光占 6 层（立方体 6 面各一次 90° 透视投影）。
    //
    //   * enabled：总开关。关掉后 depth-only Pass 仍在跑（保证那张图
    //     始终处于可采样布局），只是片元不再采样 → localShadowParams.w = 0。
    //   * maxLights：本帧最多几盏灯参与投影。层数硬上限是 12 层，这个
    //     软上限用来做"少投几盏"的对照实验（灯按到相机的距离由近到远排，
    //     预算用完就轮不到，宁可不投也不要抢掉近处的）。
    //   * bias：（常数深度偏移, 斜率深度偏移, PCF 核半径(纹素)）
    // 分配结果通过灯表的 cosOuterType.z（基准层号）告知片元侧。
    // ============================================================
    bool& localShadowsEnabled() { return m_localShadowsEnabled; }
    bool localShadowsEnabled() const { return m_localShadowsEnabled; }
    uint32_t& localShadowMaxLights() { return m_localShadowMaxLights; }
    glm::vec4& localShadowBias() { return m_localShadowBias; }
    const glm::vec4& localShadowBias() const { return m_localShadowBias; }
    const render::LocalShadowPass* localShadowPass() const {
        return m_localShadowPass.get();
    }
    // 本帧实际用掉的层数（0 = 没有任何局部光源投影）
    uint32_t localShadowLayersUsed() const { return m_localShadowLayersUsed; }

    // 本帧渲染统计（drawFrame 开头清零，供 Stats 面板显示剔除效果）。
    // 计数口径是"提交"而非"像素"：triangles 是提交的三角形数（不是实际
    // 着色数）——它衡量的是 CPU 提交开销，正是视锥剔除要压下去的东西。
    struct RenderStats {
        uint32_t renderables = 0;       // 索引里的网格实体总数（剔除前）
        uint32_t culledFrustum = 0;     // 被视锥剔除的数量
        // Hi-Z 遮挡剔除（R2-1）：在视锥**内**、但被更近几何完全挡住的
        // 数量（延迟两帧回读的结果）。开启遮挡剔除时 <= 视锥候选数。
        uint32_t culledOcclusion = 0;
        uint32_t drawnOpaque = 0;       // 实际提交的不透明物体数
        uint32_t drawnTransparent = 0;  // 实际提交的透明物体数
        uint32_t gizmosCulled = 0;      // 被剔除的灯可视化小球数
        uint32_t drawCalls = 0;         // 提交的绘制命令总数（含深度预通道）
        uint32_t triangles = 0;         // 提交的三角形总数
        // GPU instancing：不透明列表聚合出的批数（items - batches = 省下的
        // draw call）。instancing 关闭时与 drawnOpaque 相等。
        uint32_t opaqueBatches = 0;
        // Multi-draw indirect（R2-2）：本帧真正发出的 vkCmdDrawIndexedIndirect
        // 次数（= 同管线同材质的组数）与 GPU 紧凑化后画出的实例数。
        // 组数 vs drawnOpaque 就是"CPU 提交量从 O(物体数) 降到 O(材质数)"。
        uint32_t mdiSubmissions = 0;
        uint32_t mdiBatches = 0;
        uint32_t mdiInstances = 0;
        // Bindless（R2-3）：已注册进纹理数组的张数、本帧用掉的材质页数。
        // 关着时全 0（旧路径一张纹理一个 set，没有"数组下标"这个概念）。
        uint32_t bindlessTextures = 0;
        uint32_t bindlessPages = 0;
        // 本帧视锥查询实际访过的 BVH 节点数。渲染统计里最有用的一条：
        // 它和 renderables 一比就知道树有没有建歪（退化成全遍历时
        // 会逼近 nodeCount）。
        uint32_t bvhNodeVisits = 0;

        // ---- LOD（见 assets/Lod.h）----
        // 可见物体里"有 LOD 链"的数量。没链的（立方体/平面这种已经最简的
        // 网格）不计入，所以 lodEntities <= drawnOpaque + drawnTransparent。
        uint32_t lodEntities = 0;
        // 各层级各画了多少个（下标 = 层级，0 = 高模）
        uint32_t lodLevelCounts[assets::kMaxLodLevels] = {0, 0, 0, 0};
        // 如果**全部**用 LOD0 会提交多少三角形。和 triangles 一比就是
        // 这套 LOD 到底省了多少 —— 验证脚本的核心断言之一。
        uint32_t trianglesNoLod = 0;
        // 层级切换次数（本帧 + 累计）。静止场景里累计值应当很快不再增长；
        // 一直涨说明迟滞没生效、物体在阈值上抖动。
        uint32_t lodSwitches = 0;

        // ---- Shader 变体（关键字系统，见 render/ShaderKeywords.h）----
        // 本帧各变体各画了多少个，下标 = 关键字掩码（0..7）。
        // 全 0 的项不在日志里打印，否则一行 8 段全是 0 根本读不动。
        uint32_t variantCounts[1u << kKeywordCount] = {};
        // 因掩码变化而重绑管线的次数。和 drawCalls 一比就知道变体切换
        // 带来的绑定开销有多大（同掩码相邻的物体不会重绑）。
        uint32_t variantSwitches = 0;
    };
    const RenderStats& renderStats() const { return m_stats; }

    // GPU 时戳剖析（R0-1）：各 pass 耗时（毫秒）。nullptr / !valid() =
    // 设备不支持时戳；lastTotalMs() < 0 = 本帧还没有读回数据。
    const GpuTimer* gpuTimer() const { return m_gpuTimer.get(); }

    // ============================================================
    // 调试视图模式（R0-2，docs/render_roadmap.md）
    //
    //   Lit             默认，原渲染路径（逐像素不变）
    //   Unlit           纯反照率（无光照/阴影）
    //   Normal          世界空间法线可视化
    //   Roughness / Metallic  材质参数灰度图
    //   Wireframe       LINE 光栅（需 fillModeNonSolid 特性），固定绿色
    //   Overdraw        additive 累加（重叠越多越亮），depth 不测试
    //   LightComplexity 簇内灯数热度图
    //
    // 前 5 种与 LightComplexity 走 pbr.frag 的 viewModeParams.x 分支
    // （运行时 uniform，UE 同款取舍）；Wireframe/Overdraw 额外换管线。
    // 阴影 pass / 深度预通道 / 分簇 compute 不受影响 —— 只覆盖前向
    // 片元输出。初始值可用 MYVK_VIEW_MODE=<0..7> 设置。
    // ============================================================
    enum class ViewMode : uint32_t {
        Lit = 0,
        Unlit,
        Normal,
        Roughness,
        Metallic,
        Wireframe,
        Overdraw,
        LightComplexity,
    };
    static const char* viewModeName(ViewMode m);
    ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode m);
    // Wireframe 依赖 fillModeNonSolid 特性；不支持的设备上返回 false，
    // setViewMode(Wireframe) 会回退 Lit。
    bool wireframeAvailable() const { return m_fillModeNonSolid; }

    // ============================================================
    // Shader 变体 / 关键字（见 render/ShaderKeywords.h）
    //
    // 每个绘制项按下式得到一个关键字掩码，绘制时拿掩码去变体族里取管线：
    //     掩码 = keywordsOf(material)      ← 材质特性（双面 / 透明 / 有法线图）
    // 队列分类（不透明 / 透明）也由同一个掩码推导 —— "半透明"只有一处定义。
    //
    //   · forcedKeywordMask >= 0 时**无视材质**，所有绘制项都用这个掩码。
    //     用来逐个变体做对照实验（例如强制关掉 NORMAL_MAP，看画面差在哪；
    //     或强制打开 DOUBLE_SIDED，看背面是否真的画出来）。
    //     -1 = 不强制（默认）。可用 MYVK_SHADER_FORCE_MASK 环境变量设初值。
    //   · 变体族本身：已建了几个变体、命中率（验证脚本靠它证明缓存生效）。
    // ============================================================
    int& forcedKeywordMask() { return m_forcedKeywordMask; }
    int forcedKeywordMask() const { return m_forcedKeywordMask; }
    const VariantPipelines& forwardPipelines() const {
        return m_forwardPipelines;
    }
    const VariantPipelines& depthPrePipelines() const {
        return m_depthPrePipelines;
    }

    // 灯光可视化小球
    bool& lightGizmosEnabled() { return m_showLightGizmos; }
    float& lightGizmoScale() { return m_lightGizmoScale; }

    // ============================================================
    // 地平面参考栅格（编辑器视口用）
    //
    // 开关由**编辑器**每帧写进来（编辑态 + 用户勾选），默认关 ——
    // 引擎本身不该平白多出一个编辑器辅助物。
    // 具体参数（格距 / 颜色 / 淡出）在 gridSettings() 里改。
    // ============================================================
    bool& gridEnabled() { return m_gridEnabled; }
    bool gridEnabled() const { return m_gridEnabled; }
    GridSettings& gridSettings() { return m_grid; }
    const GridSettings& gridSettings() const { return m_grid; }

    // 是否启用了阴影（ImGui 上做显示用）
    bool shadowEnabled() const { return m_shadowPass != nullptr; }

    // ============================================================
    // 编辑器视口支持
    //
    // 把 3D 渲染从"直写交换链"改到一张 w×h 的离屏图，由编辑器用
    // ImGui::Image() 画在 Viewport 面板里。传 (0,0) 关闭，恢复原行为。
    //
    // 这是一次真正的"渲染分辨率解耦"：HdrTarget / 分簇缓冲 / Bloom 链
    // 全部跟着 renderExtent() 走，与交换链尺寸无关。
    // ============================================================
    void setViewportSize(uint32_t width, uint32_t height);
    bool viewportActive() const {
        return m_viewportWidth > 0 && m_viewportHeight > 0;
    }
    // 3D 场景的渲染分辨率（视口模式 = 视口尺寸；否则 = 交换链尺寸）
    VkExtent2D renderExtent() const;
    // ImGui::Image() 用的纹理句柄（实体是 VkDescriptorSet 的整型化形式）
    uint64_t viewportTextureId() const;

    // ============================================================
    // 通用 RenderTarget API（见 render/RenderTarget.h）
    //
    // 每个 RenderTarget = 一套 HdrTarget + PostProcess + ImGui 输出纹理，
    // 自管尺寸与生命周期。主视口与角色预览都是它的实例（0 号 / 1 号）。
    // 新增离屏视角（小地图 / 反射探针 / 画中画）= createRenderTarget()
    // + 绑场景与相机，不再改动主链代码。
    // ============================================================
    size_t renderTargetCount() const { return m_targets.size(); }
    render::RenderTarget* renderTarget(size_t index) {
        return index < m_targets.size() ? m_targets[index].get() : nullptr;
    }
    render::RenderTarget* mainRenderTarget() { return m_mainTarget; }
    render::RenderTarget* previewRenderTarget() { return m_previewTarget; }
    // 追加一个离屏目标（初始尺寸 0 = 未激活），返回其下标
    size_t createRenderTarget();
    uint64_t renderTargetTextureId(size_t index) const;

    // 多相机：主视口用 scene.camera(index) 渲染（0 = 默认相机）。
    // 相机由场景持有（Scene::camera(i) / addCamera，见 scene/Scene.h）。
    void setViewCameraIndex(size_t index) { m_viewCameraIndex = index; }
    size_t viewCameraIndex() const { return m_viewCameraIndex; }

    // ============================================================
    // 角色详情窗口的独立预览渲染目标（UE5 Persona 的预览视口）
    //
    // 主视口与预览视口要**同时**显示、尺寸不同、相机不同、场景不同 ——
    // 预览就是一个绑定了"预览场景 + 其相机"的 RenderTarget（1 号）。
    // drawFrame() 内主场景录制完毕后，继续把预览场景渲染到它。
    // ============================================================
    void setPreviewSize(uint32_t width, uint32_t height);
    bool previewActive() const {
        return m_previewWidth > 0 && m_previewHeight > 0;
    }
    uint64_t previewTextureId() const;
    // 设置 / 清除"本帧要额外渲染的预览场景"（角色详情窗口持有）。
    void setPreviewScene(scene::Scene* scene) { m_previewScene = scene; }
    // 额外视角（验证 / 演示用）：MYVK_EXTRA_VIEW=1 时建一个 2 号目标，
    // 用主场景的 camera(1) 渲染，证明"通用 RT + 多相机"确实开箱可用。
    bool extraViewEnabled() const { return m_extraTarget != nullptr; }

private:
    void createFrameResources();
    void createDescriptorLayouts();
    void createDefaultTextures();
    void createLightGizmoMesh();
    void createShadowPass();
    void createEnvironment();
    // 主视口离屏目标（0 号）：建 HdrTarget + PostProcess + 离屏纹理。
    // 取代过去分开的 createHdrTarget / createPostProcess —— 两者本就是
    // "一个离屏目标"的两半，现在合到 RenderTarget 里一起建。
    void createMainTarget();
    // 每帧把"后处理体积覆盖"叠加到基础相机效果上，得到生效值并下发到
    // PostProcess（色差 / 暗角）。FrameGraph 的 DoF / 运动模糊直接读
    // effectiveCameraEffects()。
    void updateCameraEffects(scene::Scene& scene);
    // 录制一个"非主"离屏目标（预览 / 额外视角）：上传它绑定的场景状态 →
    // 录几何 → 跑后处理。与主视口的差别只有"跳过阴影与分簇 compute"。
    void recordOffscreenTarget(VkCommandBuffer cmd,
                               render::RenderTarget& target);
    // 屏幕空间效果模块的创建 / 重建（SSAOPass + FrameGraph）。
    // recreate 版在"渲染分辨率或 MSAA 变了"的所有路径上调用
    // （applyViewportSize / recreateSwapchain / setSampleCount）。
    void createScreenSpace();
    void recreateScreenSpace();
    // 0 号目标重建后，把 PostProcess 的输入重接到"当前应有的源"：
    // 链激活 → FrameGraph 的输出工作图；否则 → HDR resolve 视图。
    // ⚠ 必须在 recreateScreenSpace() **之后**调用 —— 链激活时
    //   postInputViews() 返回的是 FrameGraph 的工作图，而那批图的句柄
    //   要等 FrameGraph 按新尺寸重建后才更新；先取就会把**即将销毁**的
    //   旧视图写进描述符（验证层报 invalid VkImageView）。
    void rewireMainTargetChainInput();
    // PostProcess 应该采样的输入视图：链激活时 = FrameGraph 的输出
    // 工作图，否则 = HDR resolve 视图（基线路径，逐像素不变）。
    std::vector<VkImageView> postInputViews() const;
    // 链激活状态翻转后重接 PostProcess 输入（内部含 waitIdle）
    void rewirePostInput();
    void createClusteredLighting();
    void createPipelines();
    void writeGlobalDescriptors();
    // 给**指定** set 0 写全部 15 个 binding。frameIndex 决定阴影图 / SSBO /
    // SSAO 图用哪一份（每帧在途各一套，必须对应）；ubo 是该 set 的 binding 0。
    // writeGlobalDescriptors 用它对"主链 set"与"各离屏目标的 set"各写一遍 ——
    // 两者只差 binding 0（各目标自己的相机 UBO），其余完全共享。
    void writeGlobalSet(VkDescriptorSet set, uint32_t frameIndex, VkBuffer ubo);
    // 懒建"某离屏目标本帧的相机 UBO 槽"（建 buffer + set 0 + 写全部 binding）。
    // slot = 离屏目标序号（0 起）。返回槽结构。
    FrameResources::OffscreenUbo& ensureOffscreenUbo(uint32_t slot);
    // 每张交换链图像一个"渲染完成"信号量（避免与呈现操作复用冲突）
    void createRenderFinishedSemaphores();
    void destroyRenderFinishedSemaphores();
    void recreateSwapchain();
    // 应用新的视口尺寸：重建 HdrTarget / 分簇 / 后处理 / ImGui 纹理
    void applyViewportSize();
    // DRS 的缩放比变化后重建一整套随分辨率走的资源（HdrTarget / 屏幕空间
    // 链 / 分簇 / PostProcess）。与 applyViewportSize 同一套序列，只是
    // 尺寸没变、变的是 PostSettings::renderScale。
    void applyRenderScale();
    // 懒创建 1 号目标（角色预览，第一次打开角色窗口时才建）
    render::RenderTarget* ensurePreviewTarget();
    // 预览离屏链的尺寸应用（独立于主视口）
    void applyPreviewSize();
    // 把指定场景的相机 / 灯 / 蒙皮数据上传到本帧全局缓冲。
    // extent 指定该场景的渲染分辨率（主视口用 renderExtent()，
    // 预览视口用预览尺寸）。resetSkinning 为 true 时清空蒙皮映射与
    // SSBO 偏移（主场景），false 时追加（预览场景紧接主场景之后）。
    // camOverride 非空时用它代替 scene.camera()（多相机：主视口可选
    // camera(i)，离屏目标用自己的相机）。
    // offscreenSlot >= 0 时把相机 UBO 写进"该离屏目标的本帧槽"而不是
    // 主链 UBO（见 FrameResources::OffscreenUbo 的注释），并把本场景要用的
    // set 0 记进 m_sceneGlobalSet 供紧随其后的 recordSceneInto 使用。
    void uploadSceneState(scene::Scene& scene, VkExtent2D extent,
                          bool resetSkinning,
                          const scene::Camera* camOverride = nullptr,
                          int offscreenSlot = -1);
    // 把 scene 的几何录制进指定 HdrTarget（阴影 + 深度预通道 + 分簇 + 前向）。
    // isPreview=true 时跳过阴影 pass 与分簇 compute（预览与主场景共享这些
    // 全局缓冲，且主场景的采样命令已录制在先，避免无同步竞争）。
    // globalSet = 该场景要绑的 set 0（主链或某个离屏目标的）。
    void recordSceneInto(VkCommandBuffer cmd, scene::Scene& scene,
                         class HdrTarget& hdr, bool isPreview,
                         VkDescriptorSet globalSet);
    // 在同一个命令缓冲内录制预览场景（主场景之后调用）。
    void recordPreview(VkCommandBuffer cmd);
    void recordFrame(VkCommandBuffer cmd, uint32_t imageIndex,
                     scene::Scene& scene);
    // 录制深度预通道（返回是否真的录了内容）
    void recordDepthPrePass(VkCommandBuffer cmd, scene::Scene& scene);
    // 单个网格的绘制（含 doubleSided 管线切换的调用方负责）。
    // instanceCount/firstInstance 走 instancing 合批：VS 从实例 SSBO 读
    // model，push 里的 model 字段闲置（仍照推，保持 push 布局恒定）。
    void drawMesh(VkCommandBuffer cmd, VkPipelineLayout layout,
                  const glm::mat4& model, const assets::Material& mat,
                  const assets::Mesh& mesh, uint32_t instanceCount = 1,
                  uint32_t firstInstance = 0);
    // set 1..6（albedo / normal / orm / roughness / metallic / emissive）的
    // 绑定，缺省时用中性贴图。drawMesh 与 MDI 路径共用 —— 两处的贴图
    // 顺序/缺省规则必须完全一致，否则同一材质会在两条路径下长得不一样。
    void bindMaterialTextures(VkCommandBuffer cmd, VkPipelineLayout layout,
                              const assets::Material& mat);
    // 不透明合批：排序 + 聚合 + 写实例 SSBO（buildDrawLists 尾部调用）。
    void buildOpaqueBatches();
    // 蒙皮：遍历场景里的 SkinnedMeshComponent，采样关节矩阵写入骨骼 SSBO，
    // 并建立 mesh→jointBase 映射供 drawMesh 查。每帧在 recordFrame 前调用。
    // resetOffset=true 时清空映射与偏移（主场景每帧第一次调用）；false 时
    // 追加写入（预览场景紧跟在主场景之后，共享同一 SSBO，互不覆盖）。
    void updateSkinning(scene::Scene& scene, bool resetOffset);

    // ------------------------------------------------------------
    // 一帧内要提交给**某一个场景**的绘制项。
    //
    // 建表时一次性把两件事算完：
    //   1) 视锥剔除（世界 AABB vs 视锥的六平面测试）
    //   2) 世界矩阵缓存 —— Scene::worldMatrix(e) 要沿父链递归累乘，
    //      而深度预通道 / 不透明 / 透明三条路径原本各算一次，同一个实体
    //      一帧要算 3~4 遍。缓存下来是纯赚。
    // 之后所有几何提交路径都遍历这两张表，不再碰 ECS。
    // ------------------------------------------------------------
    struct DrawItem {
        const assets::Mesh* mesh = nullptr;
        const assets::Material* material = nullptr;  // 可能为空（走缺省材质）
        glm::mat4 model{1.0f};
        float viewDepthSq = 0.0f;  // 到相机的距离平方（透明排序用）
        // LOD 层级（0 = 高模）。仅用于统计/调试，绘制路径不读它。
        int lodLevel = 0;
        // 该实体**原始**网格（LOD0）的三角形数。统计"不用 LOD 会提交多少"
        // 时要拿它做对照 —— 此时 mesh 指向的已经是低模了。
        uint32_t baseTriangles = 0;
        // 本项的 Shader 变体掩码，由 keywordsOf(*material) 一次性算好。
        // 建表时算而不是绘制时算：同一个材质在一帧里会被深度预通道、
        // 前向两条路径各取一次掩码，算两遍既费又可能不一致。
        // 材质为空（只有深度预通道会画）时保持 kKeywordNone。
        KeywordMask keywords = kKeywordNone;
        // 该项在 Hi-Z 候选表（m_hizAabbScratch）里的下标；-1 = 没进候选表
        // （网格没有包围盒）。R2-2 要按**批序**重排候选 AABB，所以必须
        // 能从这里找回它，不能只靠"两个表的顺序恰好一样"。
        int32_t hizSlot = -1;
        // 该项是否**不参与深度预通道**（R4-1）。
        //
        // Mask 材质的前向片元会 discard，但深度预通道用的是 depth_only.frag
        // （不采样材质 → 无从判断该不该丢）。若让 Mask 项进预通道，镂空处
        // 会先被写进深度，前向的 mask 片元再被 early-Z 挡掉 → 镂空"透光"
        // 直接失效，而且 alpha-to-coverage 的软边也被已经写死的深度覆盖。
        // 所以 Mask 项**跳过预通道**，靠前向自己写深度（代价是丢掉这一批的
        // early-Z，可接受；等做"带材质采样的 depth prepass 变体"再补回来）。
        bool skipDepthPrepass = false;
    };

    // 构建本帧绘制列表。isMain 为 true 时累积 RenderStats（预览场景是
    // 附带渲染，不该把它的大小算进主场景的剔除统计里）。
    // 前提：m_sceneFrustum 已经按**本场景**的相机更新过（见 uploadSceneState）。
    // 走 Scene::spatialIndex()（BVH）取视锥内的候选集，不再线性扫全场景。
    void buildDrawLists(scene::Scene& scene, const glm::vec3& camPos,
                        bool isMain);

    // 深度预通道的几何提交（遍历本帧不透明绘制列表）。
    // 调用方负责 RenderPass 的 begin/end 与 set 0 的绑定。
    void recordOpaqueDepthGeometry(VkCommandBuffer cmd);

    // ------------------------------------------------------------
    // Shader 变体的绑定（前向 Pass 用）
    // ------------------------------------------------------------
    // 材质 → **生效**的关键字掩码：默认就是 keywordsOf(mat)，
    // forcedKeywordMask >= 0 时无视材质直接用它（对照实验用）。
    KeywordMask effectiveKeywords(const assets::Material& mat) const;
    // 绑定 kw 对应的前向变体管线；与 bound 相同则跳过（同掩码相邻的
    // 物体不该重绑）。每次真的重绑会把 switches 加一。
    void bindForwardVariant(VkCommandBuffer cmd, KeywordMask kw,
                            KeywordMask& bound, uint32_t& switches);

    // 天空盒背景（全屏三角形）。前提：set 0 已绑定，且处于前向 Pass 内、
    // 几何提交之前。调用后管线状态是"天空管线"，调用方接着自己绑下一个。
    void recordSkyBackground(VkCommandBuffer cmd);

    // 级联阴影的几何提交：**每一级一次** depth-only render pass。
    // 前提：本帧的 FrameUBO 已经上传过（updateCascades 就在那里调的）。
    // 预览场景不要调（阴影图是全局一张，主场景的采样命令已经录制在先）。
    void recordShadowPasses(VkCommandBuffer cmd, scene::Scene& scene,
                            VkDescriptorSet frameSet);

    // 建 LocalShadowPass（init 时一次；分辨率改动要重建 → 一次 waitIdle）
    void createLocalShadowPass();
    // 建局部光源阴影矩阵缓冲区（每帧 in-flight 一份，常驻映射）
    void createLocalShadowBuffers();
    // 每帧给"要投影的局部光源"分配层号，并把世界→光源裁剪矩阵写进
    // **本帧**的 lightShadows SSBO。
    //
    // ⚠ 必须在 m_lighting->uploadLights() **之前**调用：层号（写进
    //   LightInstance::cosOuterType.z）要随灯表一起送到片元侧，晚了就
    //   会出现"片上写的是上一帧的层号"。
    void prepareLocalShadows(scene::Scene& scene, const glm::vec3& camPos);

    // R3-3：把 m_lightModScratch（名字）解析成 m_lightExtScratch（层号）
    void resolveLightExt();
    // 逐层录制 depth-only pass：**所有层都要走一遍**（未使用的层只清深度），
    // 否则那张 array 会有一层停在 UNDEFINED 而被采样 → 验证层直接报错。
    void recordLocalShadowPasses(VkCommandBuffer cmd, scene::Scene& scene,
                                 VkDescriptorSet frameSet);

    core::Window& m_window;

    std::unique_ptr<rhi::Instance> m_instance;
    std::unique_ptr<rhi::Device> m_device;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    std::unique_ptr<rhi::Swapchain> m_swapchain;
    std::unique_ptr<rhi::CommandPool> m_commandPool;

    // ---- 前向渲染管线：按关键字掩码的**变体族**（都绑在 HdrTarget 的 render pass 上）----
    // 这里原来是 4 条手写成员（不透明 / 双面 / 透明 / 透明双面）＋ 三处
    // if 阶梯，再加一个材质特性就要变成 8 条。现在交给掩码自动派生：
    //   · 掩码决定**编哪份 .spv**（NORMAL_MAP 少一次采样 + 一次 cotangent
    //     frame；DOUBLE_SIDED 多一次 gl_FrontFacing 法线翻转）；
    //   · 掩码同时决定**管线固定状态**（DOUBLE_SIDED → 关剔除；
    //     ALPHA_BLEND → 开混合 + 不写深度）。两边由同一个掩码推导，
    //     不可能出现"编译期是双面、栅格化还在剔背面"这种错位。
    // 惰性创建、按掩码缓存，没用到过的组合不会白建。
    render::VariantPipelines m_forwardPipelines;

    // ---- 深度预通道管线（绑在 HdrTarget 的 depthPrePass 上）----
    // 它只有 DOUBLE_SIDED 关不关剔除这一个差别，而且不输出颜色 ——
    // ALPHA_BLEND / NORMAL_MAP 对它没有意义。所以走 SingleSpv：所有掩码
    // 共用同一份 depth_only.frag.spv，只有固定状态随掩码变，不为它多编
    // 8 份毫无区别的字节码。
    render::VariantPipelines m_depthPrePipelines;

    // ---- shader 热重载（见 watchShaderFiles / pollShaderReload）----
    render::ShaderHotReload m_shaderReload;
    // 帧计数节流：每 15 帧扫一次 mtime（60fps ≈ 0.25s，与脚本热重载
    // 的 0.5s 定时器同数量级；std::filesystem 的 stat 很便宜，但没必要
    // 每帧做）。
    uint32_t m_shaderReloadTick = 0;

    // ---- 地平面栅格管线（前向 Pass 内，三角形带 4 顶点，无顶点缓冲）----
    std::unique_ptr<rhi::Pipeline> m_gridPipeline;

    // ---- 天空盒背景管线（前向 Pass 内，全屏三角形 3 顶点，无顶点缓冲）----
    // 推常量范围与主前向管线一致，理由同栅格管线（见 createPipelines）。
    std::unique_ptr<rhi::Pipeline> m_pipelineSky;

    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;
    VkDescriptorSetLayout m_globalSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    std::vector<FrameResources> m_frames;
    std::vector<VkSemaphore> m_renderFinished;  // 每交换链图像一个
    std::unique_ptr<render::ShadowPass> m_shadowPass;
    std::unique_ptr<render::LocalShadowPass> m_localShadowPass;
    // ---- 通用离屏目标（见 render/RenderTarget.h）----
    // m_targets 拥有全部目标；下面三个是**非拥有**的便捷句柄：
    //   [0] 主视口（常驻）  [1] 角色预览（懒建）  [2] 额外视角（env）
    // 主链的 m_hdrTarget / m_post 也指向 m_targets[0] 内部 —— 保留成裸
    // 指针是**故意的**：Renderer 全文有近 60 处 m_hdrTarget->/m_post->
    // 调用，改成裸指针后这些调用点一行都不用动。
    std::vector<std::unique_ptr<render::RenderTarget>> m_targets;
    render::RenderTarget* m_mainTarget = nullptr;
    render::RenderTarget* m_previewTarget = nullptr;
    render::RenderTarget* m_extraTarget = nullptr;
    render::HdrTarget* m_hdrTarget = nullptr;
    render::PostProcess* m_post = nullptr;
    std::unique_ptr<render::ClusteredLighting> m_lighting;
    std::unique_ptr<render::Environment> m_environment;
    std::unique_ptr<ui::ImGuiManager> m_imgui;

    // ---- 屏幕空间效果（见 screenSpaceSettings() 的注释）----
    std::unique_ptr<render::SSAOPass> m_ssao;
    std::unique_ptr<render::VolumetricFog> m_vfog;
    // R3-3：光源修饰纹理库（cookie + IES 的 sampler2DArray）
    std::unique_ptr<render::LightTextureLibrary> m_lightTextures;
    std::unique_ptr<render::FrameGraph> m_frameGraph;

    // ---- Hi-Z 遮挡剔除（R2-1，见 occlusionCullingEnabled() 的注释）----
    std::unique_ptr<render::HiZOcclusion> m_hiz;
    bool m_occlusionCullingEnabled = false;
    float m_hizBiasNdc = 1.0e-3f;   // 遮挡判定的 NDC 深度偏置（保守）

    // ---- Multi-draw indirect（R2-2）----
    std::unique_ptr<render::MdiDraw> m_mdi;
    bool m_mdiEnabled = false;
    bool m_mdiDbg = false;
    // 每帧重建的草稿（避免每帧分配）：源目录 / 物体→批号 / 命令 / 提交组，
    // 以及按**批序**重排过的候选 AABB（与目录同下标）。
    std::vector<render::MdiInstance> m_mdiCatalogScratch;
    std::vector<uint32_t> m_mdiObjBatchScratch;
    std::vector<render::MdiDrawCommand> m_mdiCmdScratch;
    std::vector<render::MdiSubmitGroup> m_mdiGroupScratch;
    std::vector<render::HiZAabb> m_mdiAabbScratch;
    // 把本帧的绘制列表 + 批表翻译成 MDI 的四个缓冲（buildDrawLists 尾部调）。
    bool buildMdiFrameData();
    // 本帧 MDI 数据是否已就绪（buildDrawLists 里置位，recordFrame 里读）。
    // 之所以要存一份：buildDrawLists 跑在录制**之前**，到 recordFrame 里
    // 再判一次"目录/命令是否齐"就得重算，且两处的判据容易走偏。
    bool m_mdiStaged = false;
    bool m_hizDbg = false;

    // ---- Bindless / descriptor indexing（R2-3）----
    // m_bindless 非空 = 设备支持且当前开着（两者必须同时成立，别只看
    // m_bindlessEnabled()：设备不支持时后者会被 init 打回 false）。
    std::unique_ptr<render::BindlessTextures> m_bindless;
    bool m_bindlessEnabled = false;
    bool m_bindlessDbg = false;
    // 逐 draw 把材质页号压进 push.emissive.w（关闭时恒 1.0 = 旧值）。
    // 放在这里是为了让 drawMesh / MDI 两条提交路径共用同一个口径。
    float materialPageCode(const assets::Material& mat);
    // Mask 材质的 alpha 裁切阈值 → push.skin.w（R4-1）。非 Mask 材质恒
    // 返回 +0.5（旧的空值）；负号 = 抖动模式（见 pbr.frag 的说明）。
    // 同样放在这里让 drawMesh / MDI 共用。
    static float alphaCutoffLane(const assets::Material& mat);
    // set 1..6 用的布局：bindless 时是那一份"纹理数组 + 页表"的布局，
    // 否则是老的"单纹理"布局。管线描述的 setLayouts 与 grid/sky 的
    // 对齐都必须走它 —— 两处用了不同的对象就会踩"set 0 被中途打断"
    // 那条 VUID（见 createPipelines 里栅格那段说明）。
    VkDescriptorSetLayout pipelineTexSetLayout() const {
        return m_bindless ? m_bindless->setLayout() : m_textureSetLayout;
    }
    // 本帧候选 AABB（与上传给 GPU 的顺序一一对应）。回读时用它把 vis 的
    // 下标映射回实体 —— 顺序错了就会剔错物体，是这一块最容易踩的坑。
    std::vector<render::HiZAabb> m_hizAabbScratch;
    std::vector<ecs::Entity> m_hizSlotEntities[MAX_FRAMES_IN_FLIGHT];
    // 两帧前判定"被遮挡"的实体集合（buildDrawLists 按它过滤候选）。
    std::unordered_map<uint32_t, uint8_t> m_occludedSet;
    render::ScreenSpaceSettings m_screenSpace;
    // 接线状态缓存：与 m_screenSpace 的开关比对，变了才付重接的代价
    bool m_chainWired = false;   // FrameGraph 链 → PostProcess 输入
    bool m_ssaoWired = false;    // binding 14 = SSAO 结果图还是白图
    // 推常量/UBO 用的相机推导缓存（uploadSceneState 每帧填）
    glm::mat4 m_invProj{1.0f};        // inverse(proj)
    glm::vec4 m_projCoefs{0, 0, 0, 0};// P00/P11/P22/P23（解析投影用）
    glm::mat4 m_viewProj{1.0f};       // proj*view（太阳屏幕投影用）
    float m_camNear = 0.1f;
    float m_camFar = 200.0f;
    // uploadSceneState 算出的"本场景要绑的 set 0"：
    //   主视口 = 本帧的 globalUBOSet；
    //   离屏目标 = 它自己的相机 UBO 槽（见 FrameResources::OffscreenUbo）。
    // recordSceneInto 紧接着用它，避免两处各自重算一遍。
    VkDescriptorSet m_sceneGlobalSet = VK_NULL_HANDLE;

    // ---- 纹理能力（构造函数从环境变量读，textureContext() 下发）----
    bool m_texMipmaps = false;      // MYVK_MIPMAP=1：RGBA8 纹理生成 mip 链
    bool m_texStream = false;       // MYVK_TEXSTREAM=1：DDS mip 分帧补传
    float m_anisoOverride = -1.0f;  // MYVK_ANISO 覆盖；<0 = 设备上限

    // ---- 相机效果（景深 / 运动模糊 / 色差 / 暗角）+ 后处理体积 ----
    // 基础值（UI 改） / 生效值（每帧叠加体积覆盖后下发）
    render::CameraEffectSettings m_cameraFx;
    render::CameraEffectSettings m_effectiveCameraFx;
    // 主视口用哪台相机（scene.camera(index)）
    size_t m_viewCameraIndex = 0;
    // 重投影矩阵（运动模糊 + TAA 共用）：clipFromView =
    // prevViewProj × inverse(view)。把"视图空间点 → 上一帧屏幕 uv"整条链
    // 压成一个矩阵。**两侧都用未抖动的投影**（TAA 的抖动只进光栅化），
    // 于是相机静止时严格推出"速度 0"。
    //
    // ⚠ 这里修掉了引入 TAA 时发现的一个既有错误：以前算的是
    //   viewProj × inverse(view) —— 两者相乘恰好等于 proj 本身，
    //   于是运动模糊只对"投影变化"有反应，相机真正平移/旋转时反而不动
    //   （verify_camfx 的 B2/B3 之所以能过，是因为它们走 MYVK_MB_SIM_YAW
    //   钩子直接覆盖了这个矩阵）。正确形式必须把**上一帧的 viewProj**
    //   带进来。
    glm::mat4 m_clipFromView{1.0f};  // 本帧用（每帧在 uploadSceneState 里算）
    glm::mat4 m_prevViewProj{1.0f};  // 上一帧未抖动 viewProj
    glm::mat4 m_curViewProj{1.0f};   // 本帧未抖动 viewProj（帧末滚进 m_prev）
    bool m_hasPrevFrame = false;

    // ---- 自动曝光（R1-2）----
    // 每帧 dt 从 steady_clock 量（指数的适应速度用它换算 alpha）。
    // 首帧用一个 60fps 的估计值，避免 dt 巨大把曝光一次拉到底。
    std::chrono::steady_clock::time_point m_lastFrameTime{};
    float m_frameDt = 1.0f / 60.0f;
    // MYVK_AUTOEXP_DBG=1：每帧一行 AUTOEXP 日志（回归脚本读它画收敛曲线）
    bool m_autoExposureDbg = false;
    // MYVK_CG_DBG=1：每帧一行 LUT-INFO 日志（色彩分级回归读它）
    bool m_colorGradingDbg = false;
    // MYVK_DRS_DBG=1：每帧一行 DRS 日志（动态分辨率回归读它画缩放曲线）
    bool m_drsDbg = false;
    // 自动曝光的 EV 偏置：面板改的是"基础值"（存在 PostSettings 里），
    // 后处理体积可以按相机位置覆盖它。这两个成员做"回声检测" ——
    // 值 != 上一次写下去的值就说明面板动过 → 更新基础值。
    float m_aeBiasBase = 0.0f;
    float m_aeBiasApplied = 0.0f;
    // 色彩分级 LUT 混合权重同理：面板值 = 基础值，后处理体积可按位置覆盖。
    float m_cgBlendBase = 1.0f;
    float m_cgBlendApplied = 1.0f;
    // MYVK_MB_SIM_YAW=<度>：验证钩子 —— 静态相机下人为制造偏航角速度，
    // 让运动模糊的重投影路径可被端到端验证（正常使用不设 = 0）。
    float m_mbSimYaw = 0.0f;
    // ---- TAA 的抖动相位 ----
    // Halton(2,3) 序列的下标 —— **按帧槽（frame-in-flight）各持一个**！
    //
    // 为什么不能用一个全局计数器：历史图是按帧槽分开累积的（TAA 在
    // 帧 f 读的是 f-2 留在同槽里的结果），所以"槽 0 的帧号"永远是偶数、
    // "槽 1 的帧号"永远是奇数。若相位按帧号递增，槽 0 就只见过偶数相位、
    // 槽 1 只见过奇数相位 —— 两个槽收敛到**不同的** 4 相位平均，
    // 屏幕上以半帧率来回切这两张图，而且抓哪一张取决于抓拍瞬间的奇偶，
    // 跨运行不可复现（verify_taa 的"两次运行逐像素一致"就是这么挂的）。
    // 每槽各自 +1 之后，两个槽都遍历全部 8 个相位、收敛到同一张图。
    uint32_t m_taaPhase[2] = {0u, 0u};
    // MYVK_TAA_PHASE=<0..7>：验证钩子 —— **冻结**抖动相位（不随帧推进），
    // 且即使 TAA 关着也照加抖动。配合 8 次运行（相位 0..7，TAA=0，MSAA=1）
    // 可在 Python 侧离线平均出"8 次亚像素重采样"的参考图，再与 TAA 的
    // 收敛结果对照。正常使用不设 = -1（不冻结、TAA 关则无抖动）。
    int m_taaForcePhase = -1;
    // MYVK_EXTRA_VIEW=1：建一个额外离屏目标（2 号）做通用 RT 演示 / 验证
    bool m_extraViewWanted = false;

    // ---- 角色详情窗口的预览（1 号 RenderTarget）----
    uint32_t m_previewWidth = 0;
    uint32_t m_previewHeight = 0;
    // 面板拖动了尺寸 → 下一帧录制前应用（不能在 ImGui 构建期重建资源）
    bool m_previewDirty = false;
    // 本帧要额外渲染的预览场景（角色详情窗口持有，非空才录制预览）。
    scene::Scene* m_previewScene = nullptr;

    // 缺省贴图：材质未指定某张纹理时绑定它们，保证描述符始终有效
    std::unique_ptr<assets::Texture> m_defaultWhite;       // (1,1,1,1)
    std::unique_ptr<assets::Texture> m_defaultFlatNormal;  // (0.5,0.5,1)
    std::unique_ptr<assets::Texture> m_defaultBlack;       // (0,0,0,1)

    // R4-2：预积分皮肤散射 LUT（256²，set0/binding19）。
    // init 时从 assets/textures/sss_lut.dds 加载；缺失/损坏则程序化生成
    // 一张"Lambert 表"兜底（Skin 材质退化成 Default，绝不崩）。
    std::unique_ptr<assets::Texture> m_sssLut;
    // 曲率缩放（→ FrameUBO.sssParams.x）。MYVK_SSS=0 时置 0 = 关 SSS 对照组。
    float m_sssCurvatureScale = kSssDefaultCurvatureScale;

    // 灯光可视化：每盏灯画一个小自发光球（由 Renderer 自己建网格，
    // 不经过 AssetManager —— 它是渲染器的调试设施，不是场景资产）
    std::unique_ptr<assets::Mesh> m_lightGizmoMesh;

    // 每帧复用的灯列表（避免每帧分配）
    std::vector<scene::LightInstance> m_lightScratch;
    // R3-3：逐灯修饰（CPU 名字描述 → 解析成 GPU LightExtInstance）
    std::vector<scene::LightModifierDesc> m_lightModScratch;
    std::vector<scene::LightExtInstance> m_lightExtScratch;

    // ---- 物体级视锥剔除 ----
    // 本帧**当前场景**的相机视锥（uploadSceneState 里按该场景的相机重建；
    // 主场景用完后预览场景会覆盖它，但主场景的绘制列表此时已经建好）。
    Frustum m_sceneFrustum;
    bool m_sceneFrustumValid = false;
    bool m_frustumCullingEnabled = true;
    RenderStats m_stats;
    // ---- GPU 时戳剖析（R0-1）----
    // 每帧在命令缓冲里对各 pass 打 begin/end 时戳，fence 之后读回。
    // 默认开（开销 = 每帧 18 个 query 写，微秒级）；MYVK_GPU_TIMER=0 关。
    // 只写 query pool，不触碰渲染资源 —— 开关与画面无关。
    std::unique_ptr<GpuTimer> m_gpuTimer;
    bool m_gpuTiming = true;

    // ---- 调试视图模式（R0-2）----
    ViewMode m_viewMode = ViewMode::Lit;
    bool m_fillModeNonSolid = false;  // 物理设备特性（wireframe 前提）
    // Wireframe / Overdraw 的独立管线（与变体族共用 layout，frag 都是
    // pbr.k0 —— 输出固定色，材质差异只剩顶点变换）。
    std::unique_ptr<rhi::Pipeline> m_wireframePipeline;
    std::unique_ptr<rhi::Pipeline> m_overdrawPipeline;
    // 这两种模式的几何绘制改走它；其余返回 null（着色器分支即可）
    rhi::Pipeline* viewModeDebugPipeline();
    // 本帧的绘制列表（buildDrawLists 填充）。透明表已按到相机距离降序排好。
    std::vector<DrawItem> m_opaqueDraws;
    std::vector<DrawItem> m_transparentDraws;

    // ============================================================
    // GPU instancing / 合批（MYVK_INSTANCING=1，见 buildOpaqueBatches）
    //
    // 不透明列表按 (关键字掩码, 材质, 网格) 稳定排序后，相邻同键项聚合成
    // 一批：每批只付一次 vkCmdBindPipeline + 材质 push + 纹理 set 绑定，
    // 用 vkCmdDrawIndexedInstanced(count) 展开 —— 每实例的 model 矩阵放在
    // set 0 binding 13 的 SSBO 里，pbr.inst.vert 用 gl_InstanceIndex 读
    // （firstInstance 即全局槽基址，批与批之间天然不重叠）。
    //
    // 槽序 = 批展开的绘制序，**深度预通道与前向 Pass 共用同一份**
    // （两个 pass 遍历同一个 m_opaqueBatches，SSBO 只写一次）。
    // 主场景与预览场景的槽在 buffer 里连续排布：buildDrawLists 每场景
    // 调一次，isMain 时把游标清零再累加（与骨骼 SSBO 的 jointOffset 同款）。
    //
    // 透明列表**不合批**：它必须按到相机的距离排序（混合语义），
    // 排序与批序互斥 —— 透明物体逐个提交，走原路径。
    // ============================================================
    struct DrawBatch {
        const assets::Mesh* mesh = nullptr;
        const assets::Material* material = nullptr;  // 可空（只进深度预通道）
        KeywordMask keywords = kKeywordNone;
        uint32_t firstInstance = 0;  // 全局 SSBO 槽基址
        uint32_t count = 0;          // 实例数
    };
    std::vector<DrawBatch> m_opaqueBatches;
    bool m_instancingEnabled = false;
    // 本帧已写入实例 SSBO 的槽总数（主场景 + 预览场景累加）。
    uint32_t m_instanceOffset = 0;
    // BVH 视锥查询的结果（避免每帧分配）。装的是候选实体的句柄，
    // 建表时再按句柄回查 Mesh / Material 组件。
    std::vector<ecs::Entity> m_visibleScratch;

    // ---- LOD ----
    bool m_lodEnabled = true;
    float m_lodBias = 1.0f;
    int m_forcedLodLevel = -1;
    // 每个实体上一帧选的层级（迟滞要用）。用实体 id 做键；实体销毁后条目
    // 会留到下次被同 id 复用时覆盖 —— 最坏情况是新物体第一帧少一次迟滞，
    // 没有任何可见影响。切换次数单独累计，供验证脚本判断"稳不稳"。
    std::unordered_map<uint32_t, uint8_t> m_lodSelected;
    uint32_t m_lodSwitchTotal = 0;

    // 蒙皮：mesh 指针 → 骨骼 SSBO 里的 jointBase（updateSkinning 每帧填）。
    // drawMesh 用它把 push.skin.x 设为该 mesh 的关节矩阵起始下标。
    std::unordered_map<const assets::Mesh*, int> m_skinJointBase;
    // 当前帧已写入骨骼 SSBO 的关节矩阵总数（主场景 + 预览场景累加）。
    uint32_t m_skinJointOffset = 0;

    VkSampleCountFlagBits m_sampleCount = VK_SAMPLE_COUNT_4_BIT;
    bool m_depthPrePassEnabled = true;
    bool m_clusterCullingEnabled = true;
    // Shader 变体强制覆盖（-1 = 按材质推导）。见 forcedKeywordMask()。
    int m_forcedKeywordMask = -1;
    bool m_iblEnabled = true;
    bool m_skyEnabled = true;
    bool m_showLightGizmos = true;
    float m_lightGizmoScale = 0.05f;

    // ---- 级联阴影（CSM）----
    bool m_shadowsEnabled = true;
    // 每级的分辨率（4 级各一张，所以显存是它的 4 倍：2048² × 4 字节 × 4 层
    // × 2 帧 ≈ 134 MB。`MYVK_SHADOW_RES` 可下调）
    uint32_t m_shadowResolution = render::ShadowPass::kDefaultResolution;
    uint32_t m_shadowCascades = render::ShadowPass::kMaxCascades;
    float m_shadowDistance = 90.0f;
    float m_shadowLambda = 0.6f;
    float m_shadowFadeStart = 0.85f;
    // (常数深度偏移, 斜率深度偏移, PCF 核半径(纹素), 远级联半径增长)
    glm::vec4 m_shadowBias{0.0009f, 0.0032f, 1.0f, 0.55f};

    // ---- 阴影硬化三件套（R3-1）----
    // 三个都默认取"不改变基线"的一侧：snap=开（既有行为）、blend/harden=0
    // （分支完全跳过）。验证脚本逐一打开做对照。
    bool m_shadowTexelSnap = true;    // ③ 球心光空间纹素量化
    float m_shadowBlend = 0.0f;       // ① 级联间混合带宽（占本级跨度比例）
    float m_shadowHarden = 0.0f;      // ② 接触硬化强度

    // ---- 光源修饰（R3-3）----
    // 三个总开关默认**关**：关时逐灯修饰记录全为零 → 着色器走原始路径，
    // 与基线逐位一致。组件上的名字只是"想用什么"，真正的开关在这里。
    bool m_iesEnabled = false;         // IES 光域网
    bool m_cookieEnabled = false;      // light function 投影贴图
    bool m_areaLightEnabled = false;   // 矩形区域光
    // MYVK_IES_FILE 的原始串（库建好后解析加载）
    std::string m_iesFileSpec;

    // ---- 局部光源阴影（点光 / 聚光灯）----
    bool m_localShadowsEnabled = true;
    uint32_t m_localShadowResolution = render::LocalShadowPass::kDefaultResolution;
    // 本帧最多几盏灯参与投影（层数硬上限由 kMaxLocalShadowLayers 决定）
    uint32_t m_localShadowMaxLights = 4;
    // (常数深度偏移, 斜率深度偏移, PCF 核半径(纹素), 开关 ← 每帧写)
    // ⚠ 前两个是**世界单位（米）**，不是 NDC 偏移。透视投影的 NDC 深度是
    //    双曲分布，同一个 NDC 偏移在远平面附近对应好几米、在近处只有几毫米，
    //    所以只有按米给、再由片元按参考深度换算（用每层的 near/far），
    //    这个参数才有可调性。0.03 / 0.15 对 1024² 的层大致等于"一个纹素
    //    跨度的深度误差 + 一点余量"。
    glm::vec4 m_localShadowBias{0.030f, 0.150f, 1.2f, 0.0f};
    // 本帧实际用掉的层数（prepareLocalShadows 填）
    uint32_t m_localShadowLayersUsed = 0;
    // 逐层的工作区（避免每帧分配）：
    //   m_localShadowMatrices[i]   = 第 i 层的世界 → 光源裁剪矩阵
    //   m_localShadowLayerLight[i] = 第 i 层对应的 m_lightScratch 下标，-1 = 未用
    //   m_localShadowNear/Far[i]   = 第 i 层投影的近 / 远平面（米）。
    //     片元侧要靠它把"世界单位的深度偏移"换算成 NDC —— 透视投影的
    //     深度是双曲的，没有这对数值就给不出正确的偏移（踩过）。
    std::vector<glm::mat4> m_localShadowMatrices;
    std::vector<int> m_localShadowLayerLight;
    std::vector<float> m_localShadowNear;
    std::vector<float> m_localShadowFar;
    // 本帧的层分配候选（避免每帧分配）。按到相机距离由近到远排 ——
    // 层预算用完时 "近的优先" 必须是一条硬规则，否则移动相机时
    // 会看到"哪盏灯投影"在跳。
    struct LocalShadowCandidate {
        uint32_t lightIndex = 0;  // m_lightScratch 下标
        float distSq = 0.0f;      // 到相机的距离平方（排序键）
        uint32_t layers = 0;      // 该灯要占几层（点光 6 / 聚光 1）
    };
    std::vector<LocalShadowCandidate> m_localShadowCandidates;

    // 地平面栅格（编辑器每帧写 gridEnabled；默认关）
    bool m_gridEnabled = false;
    GridSettings m_grid;

    // ---- 编辑器视口（0,0 = 关闭；尺寸/纹理都在 m_targets[0] 里）----
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    bool m_viewportDirty = false;

    uint32_t m_currentFrame = 0;
    bool m_framebufferResized = false;
};

} // namespace render
