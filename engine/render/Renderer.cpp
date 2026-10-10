#include "render/Renderer.h"

#include "core/Window.h"
#include "core/Logger.h"
#include "scene/Scene.h"
#include "ecs/Components.h"
#include "assets/Mesh.h"
#include "assets/Material.h"
#include "assets/Texture.h"
#include "render/ShaderPath.h"

// 编辑器视口要把它自己的离屏输出图注册成 ImGui 纹理
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <utility>

namespace render {

namespace {

// 从环境变量读 MSAA 采样数（默认 4x）
VkSampleCountFlagBits sampleCountFromEnv() {
    const char* s = std::getenv("MYVK_MSAA");
    if (!s) return VK_SAMPLE_COUNT_4_BIT;
    const int n = std::atoi(s);
    switch (n) {
        case 1: return VK_SAMPLE_COUNT_1_BIT;
        case 2: return VK_SAMPLE_COUNT_2_BIT;
        case 8: return VK_SAMPLE_COUNT_8_BIT;
        case 4: return VK_SAMPLE_COUNT_4_BIT;
        default: return VK_SAMPLE_COUNT_4_BIT;
    }
}

// 从环境变量读布尔开关。只有明确写了 0 / false / off 才算关 ——
// 这样"设了但写成别的值"和"没设"都退化成默认值，不会悄悄改行为。
bool envBool(const char* name, bool defaultValue) {
    const char* s = std::getenv(name);
    if (!s) return defaultValue;
    if (s[0] == '0' || s[0] == 'f' || s[0] == 'F') return false;
    if (s[0] == 'o' && (s[1] == 'f' || s[1] == 'F')) return false;
    return true;
}

constexpr uint32_t kGlobalSetBindings = 6;

} // namespace

Renderer::Renderer(core::Window& window) : m_window(window) {}

Renderer::~Renderer() {
    if (m_device) {
        m_device->waitIdle();

        // 析构顺序要点：
        //   ImGui 的管线绑定在 Post 的 render pass 上 → ImGui 先走
        //   视口纹理是 ImGui 后端分配的 descriptor set → 必须早于 ImGui 关闭
        //   Post 的描述符集引用 HDR 的 resolve image view → Post 先于 HdrTarget
        //   ClusteredLighting 持有 SSBO，被 set 0 描述符引用 → 先于描述符池
        //   ShadowPass / 缺省贴图持有 VkPipeline / VkImage → 都要早于 Device
        if (m_imgui) {
            destroyViewportTextures();
            m_imgui->shutdown();
            m_imgui.reset();
        }
        m_post.reset();
        m_hdrTarget.reset();

        m_pipelineDepthPrePassDouble.reset();
        m_pipelineDepthPrePass.reset();
        m_gridPipeline.reset();
        m_pipelineTransparentDouble.reset();
        m_pipelineTransparent.reset();
        m_pipelineDoubleSided.reset();
        m_pipeline.reset();

        m_lighting.reset();

        m_shadowPass.reset();
        m_lightGizmoMesh.reset();
        m_defaultFlatNormal.reset();
        m_defaultWhite.reset();
        m_defaultBlack.reset();

        // 每帧资源（栅栏/信号量/UBO/描述符集）
        for (auto& f : m_frames) {
            if (f.inFlightFence != VK_NULL_HANDLE)
                vkDestroyFence(m_device->get(), f.inFlightFence, nullptr);
            if (f.imageAvailable != VK_NULL_HANDLE)
                vkDestroySemaphore(m_device->get(), f.imageAvailable, nullptr);
            f.globalUBO.reset();
        }
        m_frames.clear();

        destroyRenderFinishedSemaphores();
        m_descriptors.reset();

        // 关键顺序：Swapchain 必须先于 Surface 销毁
        m_swapchain.reset();

        if (m_textureSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device->get(), m_textureSetLayout,
                                         nullptr);
            m_textureSetLayout = VK_NULL_HANDLE;
        }
        if (m_globalSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device->get(), m_globalSetLayout,
                                         nullptr);
            m_globalSetLayout = VK_NULL_HANDLE;
        }
        if (m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance->get(), m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }
    }
    // 其余 RAII 成员（CommandPool/Device/Instance）按声明逆序自动析构
}

assets::TextureContext Renderer::textureContext() {
    assets::TextureContext ctx;
    ctx.device = m_device.get();
    ctx.cmdPool = m_commandPool.get();
    ctx.descriptors = m_descriptors.get();
    ctx.textureSetLayout = m_textureSetLayout;
    ctx.maxAnisotropy = m_device->maxSamplerAnisotropy();
    return ctx;
}

void Renderer::init() {
    // 1) Instance + Surface + Device
    m_instance = std::make_unique<rhi::Instance>(true);
    m_surface = m_window.createSurface(m_instance->get());
    m_device = std::make_unique<rhi::Device>(m_instance->get(), m_surface);

    // 2) Swapchain + CommandPool
    int w = 0, h = 0;
    m_window.framebufferSize(&w, &h);
    m_swapchain = std::make_unique<rhi::Swapchain>(
        *m_device, m_surface, static_cast<uint32_t>(w),
        static_cast<uint32_t>(h));
    m_commandPool = std::make_unique<rhi::CommandPool>(*m_device);

    // 3) Descriptor 布局 + 池
    createDescriptorLayouts();
    // 每个纹理独占一个 descriptor set（albedo/normal/orm 各一个），
    // 所以 sampler 描述符要按"纹理总数"预留；灯数据用了 4 个 storage buffer
    // × 每帧一份，所以 SSBO 也要按帧数留。
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        *m_device, 256, 32, 256, 64);

    // 4) 缺省贴图（材质槽为空时绑定，保证描述符始终有效）
    createDefaultTextures();

    // 5) 灯光可视化用的单位球（Renderer 自建，不进 AssetManager）
    createLightGizmoMesh();

    // 6) 阴影 Pass（要在 createFrameResources 之前，因为后者的
    //    set 0 需要绑定阴影贴图视图）
    createShadowPass();

    // 7) HDR + MSAA 离屏目标（Forward Pass 的输出）
    m_sampleCount = sampleCountFromEnv();
    createHdrTarget();

    // 自动化测试钩子：让验证脚本能固定住会改变画面的开关，
    // 从而做"剔除前/后""MSAA 开/关"的像素级对比。
    m_depthPrePassEnabled = envBool("MYVK_DEPTH_PREPASS", true);
    m_clusterCullingEnabled = envBool("MYVK_CLUSTER_CULL", true);
    m_showLightGizmos = envBool("MYVK_LIGHT_GIZMOS", true);

    // 8) 后处理（Bloom + Tonemap，并持有交换链的 render pass）
    createPostProcess();

    // 9) 分簇光照的 GPU 数据 + compute 管线
    createClusteredLighting();

    // 10) 渲染管线（必须晚于 HdrTarget —— 它要 HDR 的 render pass）
    createPipelines();

    // 11) 每帧资源（命令缓冲/同步原语/UBO）
    createFrameResources();

    // 12) 把 set 0 的 6 个 binding 全部写上
    writeGlobalDescriptors();

    // 13) 每交换链图像一个"渲染完成"信号量
    createRenderFinishedSemaphores();

    // 14) ImGui —— 挂在 Post 的 render pass 上（最外层、直接写交换链）
    m_imgui = std::make_unique<ui::ImGuiManager>();
    m_imgui->init(m_window.handle(), *m_instance, *m_device,
                  m_post->renderPass(), m_swapchain->imageCount(),
                  m_swapchain->imageCount());

    // 15) 编辑器视口纹理（要在 ImGui 初始化之后才能注册）
    createViewportTextures();

    const ClusterStats& cs = m_lighting->stats();
    VK_LOG_INFO(
        "Renderer initialized (GPU: %s, MSAA %ux, shadow %u, HDR %ux%u, "
        "clusters %ux%ux%u = %u)",
        m_device->properties().deviceName,
        static_cast<unsigned>(m_sampleCount), m_shadowPass->resolution(),
        m_hdrTarget->extent().width, m_hdrTarget->extent().height, cs.clusterX,
        cs.clusterY, cs.clusterZ, cs.clusterCount);
}

void Renderer::createDescriptorLayouts() {
    // set 0：binding 0 = FrameUBO，binding 1 = 阴影贴图，
    //        binding 2/3/4 = 灯/簇/灯索引 SSBO，binding 5 = 遥测
    VkDescriptorSetLayoutBinding bindings[kGlobalSetBindings]{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    // COMPUTE 也要用：分簇的 compute 着色器要从同一份 UBO 读相机矩阵
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    for (uint32_t b = 2; b <= 4; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT |
                                 VK_SHADER_STAGE_COMPUTE_BIT;
    }

    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    m_globalSetLayout = rhi::makeSetLayout(
        *m_device, std::vector<VkDescriptorSetLayoutBinding>(bindings,
                                                             bindings + 6));

    // set 1/2/3 共用：单纹理采样器（binding 0，frag）
    VkDescriptorSetLayoutBinding texBinding{};
    texBinding.binding = 0;
    texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texBinding.descriptorCount = 1;
    texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    m_textureSetLayout = rhi::makeSetLayout(*m_device, {texBinding});
}

void Renderer::createDefaultTextures() {
    assets::TextureContext ctx = textureContext();

    // ⚠ 这两张都是 **数据贴图**，必须走 UNORM（srgb=false）。
    // 若按颜色贴图建成 SRGB，硬件采样时会做 sRGB→线性解码：
    //   平坦法线 (128,128,255) → 解码后 (0.216,0.216,1.0)
    //   → nSample = (x,y,z)*2-1 = (-0.568, -0.568, 1.0)
    //   即"平坦法线"其实被整体扳了约 39°，于是地平面这类大面片的着色
    //   会随切线基（TBN）的退化与否整块变色，屏幕上就是一道笔直的硬边。
    // 颜色贴图（albedo / emissive）才需要 srgb=true。

    // 白色：albedo 与 ORM 的中性值（乘上因子即等于因子本身）
    uint8_t white[4] = {255, 255, 255, 255};
    m_defaultWhite = std::make_unique<assets::Texture>();
    m_defaultWhite->makeSolid(ctx, white, 4, /*srgb=*/false);

    // 平坦法线：切线空间法线的"无扰动"值 (0, 0, 1) → 编码后 (0.5, 0.5, 1.0)
    uint8_t flatN[4] = {128, 128, 255, 255};
    m_defaultFlatNormal = std::make_unique<assets::Texture>();
    m_defaultFlatNormal->makeSolid(ctx, flatN, 4, /*srgb=*/false);

    // 黑色：emissive 的中性值。自发光在着色器里是**加法**
    // （emissive = factor + map），没绑图时加 0 才是"不发光"。
    // 用白色当默认的话，所有没绑自发光图的材质会整片发白光。
    uint8_t black[4] = {0, 0, 0, 255};
    m_defaultBlack = std::make_unique<assets::Texture>();
    m_defaultBlack->makeSolid(ctx, black, 4, /*srgb=*/false);
}

void Renderer::createLightGizmoMesh() {
    // 低模球即可：只是把小光源的位置画成一颗自发光小球
    m_lightGizmoMesh = std::make_unique<assets::Mesh>();
    m_lightGizmoMesh->upload(*m_device, *m_commandPool,
                             assets::makeSphereVertices(1.0f, 12, 8),
                             assets::makeSphereIndices(12, 8));
}

void Renderer::createShadowPass() {
    m_shadowPass = std::make_unique<render::ShadowPass>(
        *m_device, m_globalSetLayout, MAX_FRAMES_IN_FLIGHT);
}

void Renderer::createHdrTarget() {
    m_hdrTarget = std::make_unique<render::HdrTarget>(*m_device,
                                                      MAX_FRAMES_IN_FLIGHT,
                                                      m_sampleCount);
    // 设备可能不支持请求的采样数 → 以 HdrTarget 实际选中的为准
    m_sampleCount = m_hdrTarget->samples();

    // 视口模式下 3D 渲染分辨率由编辑器指定，与交换链尺寸脱钩
    VkExtent2D ext = m_swapchain->extent();
    if (m_viewportWidth > 0 && m_viewportHeight > 0) {
        ext = {m_viewportWidth, m_viewportHeight};
    }
    m_hdrTarget->recreate(ext.width, ext.height);
}

VkExtent2D Renderer::renderExtent() const {
    if (m_post) return m_post->sceneExtent();
    if (m_viewportWidth > 0 && m_viewportHeight > 0)
        return {m_viewportWidth, m_viewportHeight};
    return m_swapchain ? m_swapchain->extent() : VkExtent2D{1, 1};
}

void Renderer::createPostProcess() {
    m_post = std::make_unique<render::PostProcess>(
        *m_device, m_swapchain->format(), m_hdrTarget->colorFormat(),
        MAX_FRAMES_IN_FLIGHT);

    std::vector<VkImageView> hdrViews;
    hdrViews.reserve(m_hdrTarget->framesInFlight());
    for (uint32_t i = 0; i < m_hdrTarget->framesInFlight(); ++i)
        hdrViews.push_back(m_hdrTarget->colorView(i));  // = resolve 视图

    m_post->recreate(m_swapchain->views(), m_swapchain->extent(), hdrViews,
                     viewportActive() ? VkExtent2D{m_viewportWidth,
                                                     m_viewportHeight}
                                        : VkExtent2D{0, 0});
}

void Renderer::createClusteredLighting() {
    m_lighting = std::make_unique<render::ClusteredLighting>(
        *m_device, *m_commandPool, m_globalSetLayout, MAX_FRAMES_IN_FLIGHT);

    // 自动化测试钩子：允许从环境变量覆盖分簇参数。
    // maxLightsPerCluster 尤其重要 —— "关剔除"的自检要求 cap ≥ 灯数，
    // 否则灯表会被截断，对照实验就失去意义。
    render::ClusterConfig& cfg = m_lighting->editableConfig();
    if (const char* s = std::getenv("MYVK_TILE_SIZE")) {
        const int v = std::atoi(s);
        if (v >= 4) cfg.tileSize = static_cast<uint32_t>(v);
    }
    if (const char* s = std::getenv("MYVK_CLUSTER_SLICES")) {
        const int v = std::atoi(s);
        if (v >= 1) cfg.depthSlices = static_cast<uint32_t>(v);
    }
    if (const char* s = std::getenv("MYVK_MAX_LIGHTS_PER_CLUSTER")) {
        const int v = std::atoi(s);
        if (v >= 1) cfg.maxLightsPerCluster = static_cast<uint32_t>(v);
    }

    const VkExtent2D ext = renderExtent();
    m_lighting->recreate(ext.width, ext.height);
}

// ---------------------------------------------------------------- 编辑器视口

void Renderer::setViewportSize(uint32_t width, uint32_t height) {
    // 面板被折叠/拖到极小时给个下限，避免 0 尺寸图像
    if (width > 0 && width < 16) width = 16;
    if (height > 0 && height < 16) height = 16;
    if (width == m_viewportWidth && height == m_viewportHeight) return;
    m_viewportWidth = width;
    m_viewportHeight = height;
    m_viewportDirty = true;
}

uint64_t Renderer::viewportTextureId() const {
    if (m_viewportTextureSets.empty()) return 0;
    // 用"本帧将渲染进去的那张图"—— ImGui 的 UI 在 drawFrame 之前构建，
    // 此时 m_currentFrame 正是 drawFrame 要用的下标；命令缓冲里
    // 先写这张图、再被 ImGui 采样，前后有栅栏保证不冲突。
    const VkDescriptorSet ds =
        m_viewportTextureSets[m_currentFrame % m_viewportTextureSets.size()];
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ds));
}

void Renderer::destroyViewportTextures() {
    for (auto ds : m_viewportTextureSets) {
        if (ds != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(ds);
    }
    m_viewportTextureSets.clear();
    m_viewportTextureViews.clear();
}

void Renderer::createViewportTextures() {
    if (!m_post || !m_post->offscreen() || !m_imgui) {
        destroyViewportTextures();
        return;
    }

    // 先看这次的离屏视图和上次注册的是不是同一批。
    // 是 → 什么都不做，ImTextureID 保持不变（交换链 resize 走的正是这条路）。
    std::vector<VkImageView> views;
    const uint32_t n = m_hdrTarget->framesInFlight();
    views.reserve(n);
    for (uint32_t i = 0; i < n; ++i) views.push_back(m_post->sceneOutputView(i));

    if (views == m_viewportTextureViews && !views.empty()) return;

    destroyViewportTextures();

    m_viewportTextureSets.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i) {
        if (views[i] == VK_NULL_HANDLE) continue;
        m_viewportTextureSets[i] = ImGui_ImplVulkan_AddTexture(
            views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    m_viewportTextureViews = std::move(views);
}

void Renderer::applyViewportSize() {
    m_viewportDirty = false;
    if (!m_device) return;

    m_device->waitIdle();

    const VkExtent2D scene = viewportActive()
                                 ? VkExtent2D{m_viewportWidth, m_viewportHeight}
                                 : m_swapchain->extent();

    // HdrTarget 的 RenderPass/附件尺寸、分簇网格、后处理链三者都绑在
    // 渲染分辨率上 —— 只能整组重建。
    m_hdrTarget->recreate(scene.width, scene.height);
    m_lighting->recreate(scene.width, scene.height);

    std::vector<VkImageView> hdrViews;
    hdrViews.reserve(m_hdrTarget->framesInFlight());
    for (uint32_t i = 0; i < m_hdrTarget->framesInFlight(); ++i)
        hdrViews.push_back(m_hdrTarget->colorView(i));

    m_post->recreate(m_swapchain->views(), m_swapchain->extent(), hdrViews,
                     viewportActive()
                         ? VkExtent2D{m_viewportWidth, m_viewportHeight}
                         : VkExtent2D{0, 0});

    // 灯数据缓冲被重建 → set 0 的 binding 2/3/4/5 要重写
    writeGlobalDescriptors();

    // 离屏输出图的 image view 换了 → ImGui 纹理必须重新注册
    createViewportTextures();

    VK_LOG_INFO("Viewport render size -> %ux%u%s", scene.width, scene.height,
                viewportActive() ? " (offscreen)" : " (swapchain)");
}

// 全屏三角形 / 3D 管线共用的描述符与推常量设置
static void fillForwardPipelineDesc(rhi::PipelineDesc& desc,
                                    render::HdrTarget& hdr,
                                    VkDescriptorSetLayout globalSet,
                                    VkDescriptorSetLayout texSet) {
    desc.vertSpv = vkutil::readFile(resolveShaderPath("pbr.vert.spv"));
    desc.fragSpv = vkutil::readFile(resolveShaderPath("pbr.frag.spv"));
    desc.vertexBindings.push_back(assets::Vertex::bindingDescription());
    desc.vertexAttributes = assets::Vertex::attributeDescriptions();
    // set 0 = FrameUBO + 阴影图 + 灯数据，
    // set 1..6 = albedo / normal / orm / roughness / metallic / emissive
    desc.setLayouts = {globalSet, texSet, texSet, texSet,
                       texSet,   texSet, texSet};
    desc.pushConstant.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    desc.pushConstant.offset = 0;
    desc.pushConstant.size = sizeof(render::PushConstants);
    desc.renderPass = hdr.renderPass();
    desc.samples = hdr.samples();
    desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

void Renderer::createPipelines() {
    // ---- 深度预通道：只写深度，不输出颜色 ----
    // 顶点着色器**复用 pbr.vert**，保证与主 Pass 的顶点变换逐位一致
    // （否则主 Pass 的 LESS_OR_EQUAL 会因浮点误差出现深度闪烁）。
    {
        rhi::PipelineDesc desc;
        desc.vertSpv = vkutil::readFile(resolveShaderPath("pbr.vert.spv"));
        desc.fragSpv =
            vkutil::readFile(resolveShaderPath("depth_only.frag.spv"));
        desc.vertexBindings.push_back(assets::Vertex::bindingDescription());
        desc.vertexAttributes = assets::Vertex::attributeDescriptions();
        desc.setLayouts = {m_globalSetLayout};
        desc.pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        desc.pushConstant.offset = 0;
        desc.pushConstant.size = sizeof(PushConstants);
        desc.renderPass = m_hdrTarget->depthPrePass();
        desc.samples = m_hdrTarget->samples();
        desc.colorAttachmentCount = 0;  // depth-only
        desc.depthTest = true;
        desc.depthWrite = true;
        desc.cullMode = VK_CULL_MODE_BACK_BIT;
        desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        m_pipelineDepthPrePass = std::make_unique<rhi::Pipeline>(*m_device, desc);

        desc.cullMode = VK_CULL_MODE_NONE;
        m_pipelineDepthPrePassDouble =
            std::make_unique<rhi::Pipeline>(*m_device, desc);
    }

    // ---- 前向不透明 ----
    // depthWrite 保持 true：深度预通道已经写过同样的值，重写一次代价极小，
    // 但换来"关掉深度预通道也完全正确" —— 不必为开关维护两套管线。
    {
        rhi::PipelineDesc desc;
        fillForwardPipelineDesc(desc, *m_hdrTarget, m_globalSetLayout,
                                m_textureSetLayout);
        desc.cullMode = VK_CULL_MODE_BACK_BIT;
        m_pipeline = std::make_unique<rhi::Pipeline>(*m_device, desc);

        desc.cullMode = VK_CULL_MODE_NONE;
        m_pipelineDoubleSided = std::make_unique<rhi::Pipeline>(*m_device, desc);
    }

    // ---- 前向透明：alpha 混合 + 不写深度 ----
    // 不写深度是透明物体的铁律：写了就会把后面的透明物体挡掉
    {
        rhi::PipelineDesc desc;
        fillForwardPipelineDesc(desc, *m_hdrTarget, m_globalSetLayout,
                                m_textureSetLayout);
        desc.blendEnable = true;
        desc.depthWrite = false;
        desc.cullMode = VK_CULL_MODE_BACK_BIT;
        m_pipelineTransparent = std::make_unique<rhi::Pipeline>(*m_device, desc);

        desc.cullMode = VK_CULL_MODE_NONE;
        m_pipelineTransparentDouble =
            std::make_unique<rhi::Pipeline>(*m_device, desc);
    }

    // ---- 地平面参考栅格（编辑器视口）----
    // 与其它管线不同的三点：
    //   ① 没有顶点缓冲（顶点在 VS 里由 gl_VertexIndex 现算）—— 顶点布局留空
    //   ② 用 TRIANGLE_STRIP 画 4 个顶点
    //   ③ 推常量实际只写 48 字节（GridPushConstants），但**布局必须声明
    //      与主前向管线一模一样的 112 字节 range**，理由见下。
    // 描述符布局仍然给满 set 0..3：管线不声明 set 1/2/3，但命令缓冲里
    // 那三个 set 还绑着（上一个物体留下的），布局里留着最省事也最保险。
    // depthWrite=false —— 它只是层参考，不能挡住后面的透明物体。
    // cullMode=none —— 从地板下方往上看时也要可见。
    //
    // ⚠ push constant range 为什么不能缩成 48：Vulkan 的"管线布局兼容"
    // 判定把 **push constant range 的范围与阶段** 也算在内。set 0 是整个
    // Pass 一次性绑定的（见 recordFrame 开头），中间若出现一次与它不兼容
    // 的 vkCmdBindDescriptorSets，Vulkan 就认为 set 0 被"打断"了，
    // 后面所有 draw 都会报 VUID-...-08600。所以栅格的推常量 range 必须
    // 与主前向管线**逐字段相同**（offset 0 / size 112 / VERTEX|FRAGMENT），
    // 这样两者的 layout 对 set 0 天然兼容，不用重绑、也不会有副作用。
    // 声明 112 而只推 48 是合法的：着色器静态用到的 [0,48) 落在 range 内。
    {
        rhi::PipelineDesc desc;
        desc.vertSpv = vkutil::readFile(resolveShaderPath("grid.vert.spv"));
        desc.fragSpv = vkutil::readFile(resolveShaderPath("grid.frag.spv"));
        desc.setLayouts = {m_globalSetLayout, m_textureSetLayout,
                           m_textureSetLayout, m_textureSetLayout,
                           m_textureSetLayout, m_textureSetLayout,
                           m_textureSetLayout};
        desc.pushConstant.stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        desc.pushConstant.offset = 0;
        desc.pushConstant.size = sizeof(PushConstants);  // 112，故意与主管线一致
        desc.renderPass = m_hdrTarget->renderPass();
        desc.samples = m_hdrTarget->samples();
        desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        desc.depthTest = true;
        desc.depthWrite = false;
        desc.blendEnable = true;
        desc.cullMode = VK_CULL_MODE_NONE;
        m_gridPipeline = std::make_unique<rhi::Pipeline>(*m_device, desc);
    }
}

void Renderer::createFrameResources() {
    m_frames.resize(MAX_FRAMES_IN_FLIGHT);

    auto cmds = m_commandPool->allocate(MAX_FRAMES_IN_FLIGHT);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        auto& f = m_frames[i];
        f.commandBuffer = cmds[i];

        VkFenceCreateInfo fenceCI{};
        fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(m_device->get(), &fenceCI, nullptr,
                               &f.inFlightFence));

        VkSemaphoreCreateInfo semCI{};
        semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(m_device->get(), &semCI, nullptr,
                                   &f.imageAvailable));

        f.globalUBO = std::make_unique<rhi::Buffer>(
            *m_device, sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        f.uboMapped = f.globalUBO->map();

        f.globalUBOSet = m_descriptors->allocate(m_globalSetLayout);
    }
}

void Renderer::writeGlobalDescriptors() {
    for (uint32_t i = 0; i < m_frames.size(); ++i) {
        auto& f = m_frames[i];
        // binding 0：每帧 UBO
        m_descriptors->writeBuffer(f.globalUBOSet, 0, f.globalUBO->get(),
                                   sizeof(FrameUBO));
        // binding 1：该帧对应的阴影贴图（深度图必须用 DEPTH_STENCIL_READ_ONLY）
        m_descriptors->writeTexture(
            f.globalUBOSet, 1, m_shadowPass->sampler(), m_shadowPass->view(i),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

        // binding 2/3/4：灯 / 簇 / 灯索引
        m_descriptors->writeBuffer(
            f.globalUBOSet, 2, m_lighting->lightsBuffer(i),
            m_lighting->lightsBufferSize(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        m_descriptors->writeBuffer(
            f.globalUBOSet, 3, m_lighting->clustersBuffer(i),
            m_lighting->clustersBufferSize(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        m_descriptors->writeBuffer(
            f.globalUBOSet, 4, m_lighting->lightIndicesBuffer(i),
            m_lighting->lightIndicesBufferSize(),
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        // binding 5：遥测（只有 compute 用）
        m_descriptors->writeBuffer(
            f.globalUBOSet, 5, m_lighting->statsBuffer(i),
            m_lighting->statsBufferSize(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
}

void Renderer::createRenderFinishedSemaphores() {
    destroyRenderFinishedSemaphores();

    VkSemaphoreCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    m_renderFinished.resize(m_swapchain->imageCount(), VK_NULL_HANDLE);
    for (auto& sem : m_renderFinished) {
        VK_CHECK(vkCreateSemaphore(m_device->get(), &ci, nullptr, &sem));
    }
}

void Renderer::destroyRenderFinishedSemaphores() {
    if (!m_device) return;
    for (auto sem : m_renderFinished) {
        if (sem != VK_NULL_HANDLE)
            vkDestroySemaphore(m_device->get(), sem, nullptr);
    }
    m_renderFinished.clear();
}

void Renderer::setSampleCount(VkSampleCountFlagBits samples) {
    if (samples == m_sampleCount) return;

    m_device->waitIdle();
    m_sampleCount = samples;

    // HdrTarget 的 RenderPass 与附件采样数、以及所有前向管线的
    // rasterizationSamples 都绑在一起 —— 只能整组重建。
    const VkExtent2D ext = renderExtent();
    m_hdrTarget = std::make_unique<render::HdrTarget>(
        *m_device, MAX_FRAMES_IN_FLIGHT, samples);
    m_hdrTarget->recreate(ext.width, ext.height);
    m_sampleCount = m_hdrTarget->samples();  // 可能被设备降档

    createPipelines();

    // PostProcess 采样的是 resolve 视图，随 HdrTarget 一起换了
    std::vector<VkImageView> hdrViews;
    hdrViews.reserve(m_hdrTarget->framesInFlight());
    for (uint32_t i = 0; i < m_hdrTarget->framesInFlight(); ++i)
        hdrViews.push_back(m_hdrTarget->colorView(i));
    m_post->recreate(m_swapchain->views(), m_swapchain->extent(), hdrViews,
                     viewportActive()
                         ? VkExtent2D{m_viewportWidth, m_viewportHeight}
                         : VkExtent2D{0, 0});
    createViewportTextures();

    VK_LOG_INFO("MSAA sample count -> %ux", static_cast<unsigned>(m_sampleCount));
}

void Renderer::recreateSwapchain() {
    int w = 0, h = 0;
    m_window.framebufferSize(&w, &h);
    while (w == 0 || h == 0) {
        m_window.waitEvents();  // 窗口最小化，等恢复
        m_window.framebufferSize(&w, &h);
    }
    m_device->waitIdle();
    m_swapchain->recreate(static_cast<uint32_t>(w),
                          static_cast<uint32_t>(h));

    const VkExtent2D ext = m_swapchain->extent();

    // HDR/MSAA 离屏目标随"场景渲染分辨率"重建（其 image view 会变）。
    // 视口模式下这个分辨率与交换链无关，不能跟着交换链走。
    const VkExtent2D sceneExt = renderExtent();
    m_hdrTarget->recreate(sceneExt.width, sceneExt.height);

    // 簇网格与 SSBO 尺寸随分辨率变（gridX/gridY 由宽高算出）
    m_lighting->recreate(sceneExt.width, sceneExt.height);

    // 后处理：交换链视图 + HDR 视图都变了 → 重建 framebuffer 与描述符集
    std::vector<VkImageView> hdrViews;
    hdrViews.reserve(m_hdrTarget->framesInFlight());
    for (uint32_t i = 0; i < m_hdrTarget->framesInFlight(); ++i)
        hdrViews.push_back(m_hdrTarget->colorView(i));
    m_post->recreate(m_swapchain->views(), ext, hdrViews,
                     viewportActive()
                         ? VkExtent2D{m_viewportWidth, m_viewportHeight}
                         : VkExtent2D{0, 0});

    // 灯数据缓冲被重建了 → set 0 的 binding 2/3/4/5 必须重写
    writeGlobalDescriptors();

    createRenderFinishedSemaphores();

    // 离屏输出图的 image view 也变了 → 重注册 ImGui 纹理
    createViewportTextures();

    if (m_imgui) {
        // 图像数变化时同步 ImGui 后端
        m_imgui->setMinImageCount(m_swapchain->imageCount());
    }
}

// ---------------------------------------------------------------- 绘制辅助

void Renderer::drawMesh(VkCommandBuffer cmd, VkPipelineLayout layout,
                        const glm::mat4& model, const assets::Material& mat,
                        const assets::Mesh& mesh) {
    PushConstants push{};
    push.model = model;
    push.baseColorFactor = mat.baseColorFactor;
    push.pbr = glm::vec4(mat.metallic, mat.roughness, mat.normalScale, mat.ao);
    push.emissive = glm::vec4(mat.emissive, 1.0f);
    vkCmdPushConstants(cmd, layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &push);

    // set 1..6：albedo / normal / orm / roughness / metallic / emissive
    // （缺省时绑定中性贴图：白 / 平坦法线 / 白 / 白 / 白 / 黑）
    VkDescriptorSet texSets[kTextureSetCount] = {
        (mat.albedoMap ? mat.albedoMap : m_defaultWhite.get())->descriptorSet(),
        (mat.normalMap ? mat.normalMap : m_defaultFlatNormal.get())
            ->descriptorSet(),
        (mat.ormMap ? mat.ormMap : m_defaultWhite.get())->descriptorSet(),
        (mat.roughnessMap ? mat.roughnessMap : m_defaultWhite.get())
            ->descriptorSet(),
        (mat.metallicMap ? mat.metallicMap : m_defaultWhite.get())
            ->descriptorSet(),
        (mat.emissiveMap ? mat.emissiveMap : m_defaultBlack.get())
            ->descriptorSet(),
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1,
                            kTextureSetCount, texSets, 0, nullptr);

    mesh.draw(cmd);
}

void Renderer::recordDepthPrePass(VkCommandBuffer cmd, scene::Scene& scene) {
    // 注意：这个 RenderPass **无条件**执行，即使预通道被关掉。
    // 前向通道的深度附件是 LOAD 语义（复用这里的产物），深度内容完全
    // 由这里产生。关掉预通道只意味着"这里不画几何"——前向通道退回自己
    // 做深度测试、失去 early-Z —— 但**清深度这一步不能省**，否则前向
    // 通道会拿上一帧残留的深度做遮挡测试，画面直接废掉。
    if (!m_pipelineDepthPrePass) return;

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};  // 远平面

    const VkExtent2D ext = m_hdrTarget->extent();

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_hdrTarget->depthPrePass();
    rp.framebuffer = m_hdrTarget->depthFramebuffer(m_currentFrame);
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = ext;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(ext.width);
    viewport.height = static_cast<float>(ext.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = ext;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (m_depthPrePassEnabled) {
    // 深度预通道只画**不透明**几何：透明物体若参与预通道会把后面
    // 本该被混合出来的东西挡掉。灯小球也不参与（它不该遮挡任何东西）。
    const VkDescriptorSet globalSet = m_frames[m_currentFrame].globalUBOSet;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineDepthPrePass->layout(), 0, 1, &globalSet,
                            0, nullptr);

    bool boundDouble = false;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_pipelineDepthPrePass->get());

    scene.forEachRenderable([&](ecs::Entity e, ecs::MeshComponent& mc,
                                ecs::MaterialComponent& matc) {
        if (!mc.mesh || !mc.mesh->valid()) return;
        const assets::Material* mat = matc.material;
        if (mat && mat->alphaMode == assets::Material::AlphaMode::Blend) return;

        const bool doubleSided = mat ? mat->doubleSided : false;
        if (doubleSided != boundDouble) {
            boundDouble = doubleSided;
            vkCmdBindPipeline(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                (boundDouble ? m_pipelineDepthPrePassDouble
                             : m_pipelineDepthPrePass)
                    ->get());
        }

        PushConstants push{};
        push.model = scene.worldMatrix(e);
        vkCmdPushConstants(cmd, m_pipelineDepthPrePass->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants),
                           &push);
        mc.mesh->draw(cmd);
    });
    }  // if (m_depthPrePassEnabled)

    vkCmdEndRenderPass(cmd);
}

void Renderer::recordFrame(VkCommandBuffer cmd, uint32_t imageIndex,
                           scene::Scene& scene) {
    auto& frame = m_frames[m_currentFrame];

    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    // ================= Pass 1：阴影贴图（depth-only）=================
    // 必须排在主 Pass 之前：主 Pass 要采样它产出的深度图
    m_shadowPass->render(cmd, scene, frame.globalUBOSet, m_currentFrame);

    // ================= Pass 2：深度预通道（depth-only，MSAA）=========
    // 不透明几何先铺一遍深度，主 Pass 靠它做 early-Z。
    // 分簇的 compute 不能出现在 RenderPass 内部，所以这里必须收尾。
    recordDepthPrePass(cmd, scene);

    // ================= Pass 3：分簇剔除（compute）===================
    // 读完 UBO 里的相机参数，按 tile × 深度层把灯分到簇里，并插一个
    // buffer barrier 让 clusters/lightIndices 的写对片元阶段可见。
    m_lighting->record(cmd, m_currentFrame, m_clusterCullingEnabled,
                       frame.globalUBOSet);

    // ================= Pass 4：前向渲染 → MSAA HDR → resolve ========
    VkClearValue clears[2]{};
    clears[0].color = {{0.020f, 0.024f, 0.040f, 1.0f}};  // 线性 HDR 底色
    // 深度的 loadOp 是 LOAD（复用深度预通道的结果，包括它被关闭时的
    // "仅清深度"），所以这个清值实际不会被用到，留着只为占满数组。
    clears[1].depthStencil = {1.0f, 0};

    const VkExtent2D hdrExtent = m_hdrTarget->extent();

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_hdrTarget->renderPass();
    rp.framebuffer = m_hdrTarget->framebuffer(m_currentFrame);
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = hdrExtent;
    // 只有颜色（index 0）与深度（index 1）需要清值；
    // resolve 附件的 loadOp 是 DONT_CARE，不占用清值数组。
    rp.clearValueCount = 2;
    rp.pClearValues = clears;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // 动态视口 / 剪刀
    VkViewport viewport{};
    viewport.width = static_cast<float>(hdrExtent.width);
    viewport.height = static_cast<float>(hdrExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = hdrExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipeline->layout(), 0, 1, &frame.globalUBOSet, 0,
                            nullptr);

    // ---------------- 不透明 ----------------
    bool boundDouble = false;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->get());

    scene.forEachRenderable([&](ecs::Entity e, ecs::MeshComponent& mc,
                                ecs::MaterialComponent& matc) {
        if (!mc.mesh || !mc.mesh->valid() || !matc.material) return;
        const assets::Material& mat = *matc.material;
        if (mat.alphaMode == assets::Material::AlphaMode::Blend) return;

        // glTF 的 doubleSided 材质切到不剔除的管线
        if (mat.doubleSided != boundDouble) {
            boundDouble = mat.doubleSided;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              (boundDouble ? m_pipelineDoubleSided : m_pipeline)
                                  ->get());
        }
        drawMesh(cmd, m_pipeline->layout(), scene.worldMatrix(e), mat, *mc.mesh);
    });

    // ---------------- 灯光可视化 ----------------
    // 每盏灯画一颗自发光小球：颜色 = 灯色 × 强度。既是调试手段，
    // 也顺便证明 HDR + Bloom 在新管线里依然工作（小球会晕开）。
    if (m_showLightGizmos && m_lightGizmoMesh && !m_lightScratch.empty()) {
        if (boundDouble) {
            boundDouble = false;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_pipeline->get());
        }

        assets::Material gizmoMat;
        gizmoMat.baseColorFactor = glm::vec4(0.0f);
        gizmoMat.metallic = 0.0f;
        gizmoMat.roughness = 1.0f;
        gizmoMat.normalScale = 0.0f;
        gizmoMat.ao = 1.0f;

        for (const auto& L : m_lightScratch) {
            const glm::vec3 p(L.positionRange);
            // 半径随影响范围稍作变化，便于一眼看出灯的强弱
            const float r =
                m_lightGizmoScale * (0.6f + 0.4f * L.positionRange.w / 8.0f);
            glm::mat4 model = glm::translate(glm::mat4(1.0f), p);
            model = glm::scale(model, glm::vec3(r));

            // 让小球自己发光；强度过高会过曝成一团白，这里压到 0.35
            const glm::vec3 c = glm::vec3(L.colorIntensity) *
                                (L.colorIntensity.a * 0.35f);
            gizmoMat.emissive = glm::clamp(c, glm::vec3(0.0f),
                                           glm::vec3(6.0f));

            drawMesh(cmd, m_pipeline->layout(), model, gizmoMat,
                     *m_lightGizmoMesh);
        }
    }

    // ---------------- 地平面参考栅格（编辑器视口）----------------
    // 位置是关键：夹在"不透明"与"透明"之间。
    //   · 吃深度测试 → 墙、柱子、箱子都会正确挡住它（这也是它不做成
    //     ImGui 叠加层的原因）
    //   · 不写深度 + 排在透明之前 → 玻璃之类的透明物能正常叠在它上面
    // 顶点一个都不要：4 个 gl_VertexIndex 拼出铺满 extent 的四边形，
    // 格线图案全在片元里按像素算（见 assets/shaders/grid.frag）。
    if (m_gridEnabled && m_gridPipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_gridPipeline->get());

        // 淡出距离收在栅格自己的边界（extent）与远平面以内：
        //   · 超过 extent → 淡出在方块外面才走完，方块的边会留一条硬边；
        //   · 超过远平面 → "淡出"永远发生不了，地平线处会突然截断。
        // 这两个都是**距栅格中心**的距离（见 GridSettings 的注释）。
        const float far = scene.camera().farPlane();
        const float fadeEnd =
            std::min(std::min(m_grid.fadeEnd, m_grid.extent), far * 0.95f);

        GridPushConstants gpc;
        gpc.params = glm::vec4(m_grid.extent, glm::max(m_grid.minorStep, 1e-3f),
                               glm::max(m_grid.majorStep, 1e-3f),
                               m_grid.planeOffset);
        gpc.color = glm::vec4(m_grid.color, m_grid.alpha);
        gpc.fade = glm::vec4(std::min(m_grid.fadeStart, fadeEnd * 0.9f),
                             fadeEnd, glm::max(m_grid.lineWidthPx, 0.5f),
                             m_grid.minorStrength);

        vkCmdPushConstants(cmd, m_gridPipeline->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(gpc), &gpc);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }

    // ---------------- 透明（按到相机距离降序）----------------
    // 同一 subpass 内图元按提交顺序光栅化，所以这里的排序顺序就是混合顺序。
    // 不排序的话，近处的透明物先画就会被远处的"盖"上一层。
    m_transparentScratch.clear();
    const glm::vec3 camPos = scene.camera().position();
    scene.forEachRenderable([&](ecs::Entity e, ecs::MeshComponent& mc,
                                ecs::MaterialComponent& matc) {
        if (!mc.mesh || !mc.mesh->valid() || !matc.material) return;
        if (matc.material->alphaMode != assets::Material::AlphaMode::Blend)
            return;
        const glm::vec3 p = glm::vec3(scene.worldMatrix(e)[3]);
        m_transparentScratch.emplace_back(e, glm::dot(p - camPos, p - camPos));
    });

    if (!m_transparentScratch.empty()) {
        std::sort(m_transparentScratch.begin(), m_transparentScratch.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        boundDouble = false;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipelineTransparent->get());

        for (const auto& item : m_transparentScratch) {
            ecs::Entity e = item.first;
            const auto* mc = scene.world().get<ecs::MeshComponent>(e);
            const auto* matc = scene.world().get<ecs::MaterialComponent>(e);
            if (!mc || !matc || !mc->mesh || !matc->material) continue;

            const assets::Material& mat = *matc->material;
            if (mat.doubleSided != boundDouble) {
                boundDouble = mat.doubleSided;
                vkCmdBindPipeline(
                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    (boundDouble ? m_pipelineTransparentDouble
                                 : m_pipelineTransparent)
                        ->get());
            }
            drawMesh(cmd, m_pipelineTransparent->layout(),
                     scene.worldMatrix(e), mat, *mc->mesh);
        }
    }

    vkCmdEndRenderPass(cmd);

    // ============ Pass 5..N：Bloom + Tonemap → 输出 ==============
    // 模式 A（游戏）：合成直接写交换链，ImGui 在同一个 Pass 内绘制。
    // 模式 B（编辑器视口）：合成写离屏图；再单独一个 Pass 清交换链并画
    //   ImGui —— Viewport 面板用 ImGui::Image 把那张离屏图贴进子窗口。
    const auto overlay = [this](VkCommandBuffer c) { m_imgui->render(c); };
    m_post->renderScene(cmd, imageIndex, m_currentFrame, overlay);
    if (m_post->offscreen()) {
        m_post->renderUiToSwapchain(cmd, imageIndex, overlay);
    }

    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::drawFrame(scene::Scene& scene) {
    if (m_window.takeResizeFlag()) {
        recreateSwapchain();
    }
    // 编辑器拖动了视口面板 → 重建整套离屏链
    if (m_viewportDirty) {
        applyViewportSize();
    }
    // ImGui 面板可能改了分簇参数 → 重建簇缓冲
    if (m_lighting->needsRebuild()) {
        const VkExtent2D e = renderExtent();
        m_lighting->recreate(e.width, e.height);
        writeGlobalDescriptors();
    }

    auto& frame = m_frames[m_currentFrame];
    VkDevice dev = m_device->get();

    vkWaitForFences(dev, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);

    // 栅栏已过 → GPU 不再访问本帧的统计缓冲，可以安全回读
    m_lighting->beginFrame(m_currentFrame);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(
        dev, m_swapchain->get(), UINT64_MAX, frame.imageAvailable,
        VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("vkAcquireNextImageKHR failed");
    }

    vkResetFences(dev, 1, &frame.inFlightFence);

    // 该交换链图像专属的"渲染完成"信号量（防止与呈现复用冲突）
    VkSemaphore renderFinished = m_renderFinished[imageIndex];

    // ---- 场景包围盒 → 光源正交投影范围 ----
    // 两个要点：
    //   1) 用**网格自身的世界空间 AABB**（局部盒 × 世界矩阵），而不是实体
    //      原点。原点忽略物体尺寸/缩放，单个物体算出来的包围盒退化成一个
    //      点 → 半径只剩 2m，阴影贴图只覆盖原点周围一小块，屏幕上就会出现
    //      一块边界笔直的方形阴影区。
    //   2) 只统计**投射阴影**的物体。像 120×120 的地面这种"只接收不投射"
    //      的物体（castShadow=false）若参与，正交范围会被撑到 ±85m，
    //      深度精度全浪费在空地上。
    glm::vec3 bmin(1e30f), bmax(-1e30f);
    size_t casterCount = 0;
    scene.forEachShadowCaster([&](ecs::Entity e, ecs::MeshComponent& mc) {
        if (!mc.mesh || !mc.mesh->valid() || !mc.mesh->hasBounds()) return;
        ++casterCount;
        const glm::mat4 m = scene.worldMatrix(e);
        const glm::vec3 lo = mc.mesh->boundsMin();
        const glm::vec3 hi = mc.mesh->boundsMax();
        const glm::vec3 corners[8] = {
            {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {lo.x, hi.y, lo.z},
            {hi.x, hi.y, lo.z}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z},
            {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z},
        };
        for (const glm::vec3& c : corners) {
            const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
            bmin = glm::min(bmin, w);
            bmax = glm::max(bmax, w);
        }
    });
    const bool hasBounds = bmin.x <= bmax.x;
    // 没有投影物体时以相机焦点为中心给一块默认范围。用原点 + 2m 这种
    // 退化范围的话，视口里那条方形边界会正好落在镜头内。
    const glm::vec3 center =
        hasBounds ? (bmin + bmax) * 0.5f : scene.camera().target();
    const float radius =
        hasBounds ? glm::length(bmax - bmin) * 0.5f + 2.0f : 25.0f;

    if (std::getenv("MYVK_SHADOW_DBG")) {
        VK_LOG_INFO("SHADOW-DBG casters=%llu center=(%.2f,%.2f,%.2f) "
                    "radius=%.2f dir=(%.2f,%.2f,%.2f)",
                    static_cast<unsigned long long>(casterCount), center.x,
                    center.y, center.z, radius, scene.light().direction.x,
                    scene.light().direction.y, scene.light().direction.z);
    }

    m_shadowPass->updateLightMatrix(scene.light().direction, center, radius);

    // ---- 收集局部光源（点光 / 射灯）并上传 ----
    scene.collectLights(m_lightScratch);
    m_lighting->uploadLights(m_currentFrame, m_lightScratch);

    // ---- 更新 FrameUBO（相机 + 光照 + 分簇参数）----
    // 注意用的是 renderExtent 而**不是**交换链尺寸：编辑器视口下
    // 场景渲染分辨率是视口大小，投影矩阵的宽高比、分簇的屏幕尺寸、
    // invViewProj 都必须按它算，否则画面会拉伸、拾取射线也会偏。
    const VkExtent2D extent = renderExtent();
    FrameUBO ubo{};
    ubo.view = scene.camera().viewMatrix();
    ubo.proj = scene.camera().projMatrix(
        static_cast<float>(extent.width) / static_cast<float>(extent.height));
    glm::vec3 ld = glm::normalize(scene.light().direction);
    ubo.lightDir = glm::vec4(ld, scene.light().intensity);
    ubo.lightColor = glm::vec4(scene.light().color, 1.0f);
    ubo.camPos = glm::vec4(scene.camera().position(), 1.0f);
    ubo.lightSpace = m_shadowPass->lightSpaceMatrix();

    // 分簇：深度切片从"可见范围内最近的深度"起算。
    // 本引擎的 proj 由 glm::perspective 生成且未开 GLM_FORCE_DEPTH_ZERO_TO_ONE，
    // NDC z 覆盖 [-1,1]，而 Vulkan 只保留 [0,1] —— 真正可见的最近深度因此是
    // z_ndc = 0 处的 2·zn·zf/(zn+zf)，而不是 zn。用它当切片起点，
    // 才不会有若干层切片白白落在裁剪区外。
    constexpr float kNear = 0.1f;    // 与 scene::Camera 的 m_zNear 一致
    constexpr float kFar = 200.0f;   // 与 scene::Camera 的 m_zFar 一致
    const float clusterNear = 2.0f * kNear * kFar / (kNear + kFar);
    ubo.cameraParams =
        glm::vec4(kNear, kFar, clusterNear,
                  static_cast<float>(m_lighting->depthSlices()));
    ubo.clusterDims = glm::vec4(
        static_cast<float>(m_lighting->gridX()),
        static_cast<float>(m_lighting->gridY()),
        static_cast<float>(m_lighting->depthSlices()),
        static_cast<float>(m_lighting->config().tileSize));
    ubo.screenSize =
        glm::vec4(static_cast<float>(extent.width),
                  static_cast<float>(extent.height),
                  1.0f / static_cast<float>(extent.width),
                  1.0f / static_cast<float>(extent.height));
    ubo.lightParams =
        glm::vec4(static_cast<float>(m_lightScratch.size()),
                  scene.light().ambientScale,
                  m_clusterCullingEnabled ? 1.0f : 0.0f,
                  static_cast<float>(m_lighting->config().maxLightsPerCluster));
    ubo.invViewProj = glm::inverse(ubo.proj * ubo.view);
    std::memcpy(frame.uboMapped, &ubo, sizeof(FrameUBO));

    // 录制命令
    recordFrame(frame.commandBuffer, imageIndex, scene);

    // 提交
    VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &frame.imageAvailable;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &frame.commandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderFinished;
    VK_CHECK(vkQueueSubmit(m_device->graphicsQueue(), 1, &si,
                           frame.inFlightFence));

    // 呈现
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinished;
    pi.swapchainCount = 1;
    VkSwapchainKHR swap = m_swapchain->get();
    pi.pSwapchains = &swap;
    pi.pImageIndices = &imageIndex;

    VkResult pres = vkQueuePresentKHR(m_device->graphicsQueue(), &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    } else if (pres != VK_SUCCESS) {
        throw std::runtime_error("vkQueuePresentKHR failed");
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

} // namespace render
