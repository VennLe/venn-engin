#pragma once
// ============================================================
// render/Renderer —— 一帧的组织者
//
// Pass 编排（Clustered Forward / 分簇前向）：
//
//   ShadowPass（方向光阴影图，depth-only 2048²）
//     → DepthPrePass（不透明几何只写深度，MSAA 深度附件）
//       → ClusterBuild（compute：按 tile × 深度层切簇，把灯分到簇里）
//         → ForwardPass（不透明 →【地平面栅格】→ 透明，输出到 MSAA HDR，再 resolve）
//           → PostProcess（Bloom 亮部提取 → 高斯 H/V → 合成 + Tonemap）
//             → 交换链（SRGB，硬件做线性→sRGB 编码；ImGui 画在同一 Pass）
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
//          binding 1 : shadowMap（sampler2DShadow）
//          binding 2 : lights SSBO（LightInstance[]）
//          binding 3 : clusters SSBO（uvec2[]）
//          binding 4 : lightIndices SSBO（uint[]）
//          binding 5 : clusterStats SSBO（uint[4]，遥测）
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
#include "render/RenderTypes.h"
#include "render/ShadowPass.h"
#include "render/HdrTarget.h"
#include "render/PostProcess.h"
#include "render/ClusteredLighting.h"

#include "ui/ImGuiManager.h"
#include "assets/Texture.h"
#include "assets/Material.h"
#include "ecs/Entity.h"
#include "scene/Light.h"

#include <memory>
#include <string>
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

    // 分簇光照（灯列表 / 簇 / 统计）
    ClusteredLighting& clusteredLighting() { return *m_lighting; }

    // MSAA 采样数。改动会重建 HdrTarget 与前向管线（代价：一次 waitIdle）
    VkSampleCountFlagBits sampleCount() const { return m_sampleCount; }
    void setSampleCount(VkSampleCountFlagBits samples);

    // 深度预通道开关（关掉后主 Pass 的 early-Z 失效，仅用于对比验证）
    bool& depthPrePassEnabled() { return m_depthPrePassEnabled; }

    // 分簇剔除开关（关掉后每簇塞满灯表 —— 用于验证"剔除不漏光"）
    bool& clusterCullingEnabled() { return m_clusterCullingEnabled; }

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

private:
    void createFrameResources();
    void createDescriptorLayouts();
    void createDefaultTextures();
    void createLightGizmoMesh();
    void createShadowPass();
    void createHdrTarget();
    void createPostProcess();
    void createClusteredLighting();
    void createPipelines();
    void writeGlobalDescriptors();
    // 每张交换链图像一个"渲染完成"信号量（避免与呈现操作复用冲突）
    void createRenderFinishedSemaphores();
    void destroyRenderFinishedSemaphores();
    void recreateSwapchain();
    // 应用新的视口尺寸：重建 HdrTarget / 分簇 / 后处理 / ImGui 纹理
    void applyViewportSize();
    void createViewportTextures();
    void destroyViewportTextures();
    void recordFrame(VkCommandBuffer cmd, uint32_t imageIndex,
                     scene::Scene& scene);
    // 录制深度预通道（返回是否真的录了内容）
    void recordDepthPrePass(VkCommandBuffer cmd, scene::Scene& scene);
    // 单个网格的绘制（含 doubleSided 管线切换的调用方负责）
    void drawMesh(VkCommandBuffer cmd, VkPipelineLayout layout,
                  const glm::mat4& model, const assets::Material& mat,
                  const assets::Mesh& mesh);

    core::Window& m_window;

    std::unique_ptr<rhi::Instance> m_instance;
    std::unique_ptr<rhi::Device> m_device;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    std::unique_ptr<rhi::Swapchain> m_swapchain;
    std::unique_ptr<rhi::CommandPool> m_commandPool;

    // ---- 前向渲染管线（都绑在 HdrTarget 的 render pass 上）----
    std::unique_ptr<rhi::Pipeline> m_pipeline;                     // 不透明
    std::unique_ptr<rhi::Pipeline> m_pipelineDoubleSided;          // 不透明双面
    std::unique_ptr<rhi::Pipeline> m_pipelineTransparent;          // 透明混合
    std::unique_ptr<rhi::Pipeline> m_pipelineTransparentDouble;    // 透明双面

    // ---- 深度预通道管线（绑在 HdrTarget 的 depthPrePass 上）----
    std::unique_ptr<rhi::Pipeline> m_pipelineDepthPrePass;
    std::unique_ptr<rhi::Pipeline> m_pipelineDepthPrePassDouble;

    // ---- 地平面栅格管线（前向 Pass 内，三角形带 4 顶点，无顶点缓冲）----
    std::unique_ptr<rhi::Pipeline> m_gridPipeline;

    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;
    VkDescriptorSetLayout m_globalSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    std::vector<FrameResources> m_frames;
    std::vector<VkSemaphore> m_renderFinished;  // 每交换链图像一个
    std::unique_ptr<render::ShadowPass> m_shadowPass;
    std::unique_ptr<render::HdrTarget> m_hdrTarget;
    std::unique_ptr<render::PostProcess> m_post;
    std::unique_ptr<render::ClusteredLighting> m_lighting;
    std::unique_ptr<ui::ImGuiManager> m_imgui;

    // 缺省贴图：材质未指定某张纹理时绑定它们，保证描述符始终有效
    std::unique_ptr<assets::Texture> m_defaultWhite;       // (1,1,1,1)
    std::unique_ptr<assets::Texture> m_defaultFlatNormal;  // (0.5,0.5,1)
    std::unique_ptr<assets::Texture> m_defaultBlack;       // (0,0,0,1)

    // 灯光可视化：每盏灯画一个小自发光球（由 Renderer 自己建网格，
    // 不经过 AssetManager —— 它是渲染器的调试设施，不是场景资产）
    std::unique_ptr<assets::Mesh> m_lightGizmoMesh;

    // 每帧复用的灯列表（避免每帧分配）
    std::vector<scene::LightInstance> m_lightScratch;
    // 透明物体排序用的临时列表：(实体, 到相机的距离平方)
    std::vector<std::pair<ecs::Entity, float>> m_transparentScratch;

    VkSampleCountFlagBits m_sampleCount = VK_SAMPLE_COUNT_4_BIT;
    bool m_depthPrePassEnabled = true;
    bool m_clusterCullingEnabled = true;
    bool m_showLightGizmos = true;
    float m_lightGizmoScale = 0.05f;

    // 地平面栅格（编辑器每帧写 gridEnabled；默认关）
    bool m_gridEnabled = false;
    GridSettings m_grid;

    // ---- 编辑器视口（0,0 = 关闭）----
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    bool m_viewportDirty = false;
    // 每帧一张 ImGui 纹理（引用该帧的离屏输出图）
    std::vector<VkDescriptorSet> m_viewportTextureSets;
    // 注册这些纹理时引用的离屏 image view。
    // 重建时先比对：视图没变就**不**注销重注册 —— 否则本帧已经构建好的
    // ImGui 绘制数据会指向刚被释放的描述符集（编辑器 resize 时的经典坑）。
    std::vector<VkImageView> m_viewportTextureViews;

    uint32_t m_currentFrame = 0;
    bool m_framebufferResized = false;
};

} // namespace render
