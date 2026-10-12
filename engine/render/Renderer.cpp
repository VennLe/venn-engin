#include "render/Renderer.h"

#include "core/Window.h"
#include "core/Logger.h"
#include "scene/Scene.h"
#include "ecs/Components.h"
#include "assets/Mesh.h"
#include "assets/Material.h"
#include "assets/Texture.h"
#include "assets/Model.h"
#include "assets/Skeleton.h"
#include "render/ShaderPath.h"
#include "render/SSAOPass.h"
#include "render/FrameGraph.h"

// 编辑器视口要把它自己的离屏输出图注册成 ImGui 纹理
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// set 0 的 binding 数：0=FrameUBO 1=阴影图 2..5=灯/簇/索引/遥测
// 6=骨骼矩阵 7=辐照度 8=预过滤 9=BRDF表 10=天空立方体
// 从环境变量读 "r,g,b"（三个 float）。解析失败就当没设置 ——
// 与 envBool 同样的态度：宁可退回默认值，也不要让错别字悄悄改行为。
bool envVec3(const char* name, glm::vec3& out) {
    const char* s = std::getenv(name);
    if (!s) return false;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (std::sscanf(s, "%f,%f,%f", &r, &g, &b) != 3) return false;
    out = glm::vec3(r, g, b);
    return true;
}

// 从环境变量读一个不小于 minValue 的整数
bool envUint(const char* name, uint32_t minValue, uint32_t& out) {
    const char* s = std::getenv(name);
    if (!s) return false;
    const int v = std::atoi(s);
    if (v < static_cast<int>(minValue)) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

// 从环境变量读一个非负浮点（屏幕空间效果的数值参数覆盖用）。
// 与 envBool 同样的态度：写错就退回默认值，不让错别字悄悄改行为。
bool envFloat(const char* name, float& out) {
    const char* s = std::getenv(name);
    if (!s) return false;
    const float v = static_cast<float>(std::atof(s));
    if (!(v >= 0.0f)) return false;  // NaN / 负数都拒绝
    out = v;
    return true;
}

constexpr uint32_t kGlobalSetBindings = 15;

// "当前没有绑定任何前向变体管线"的哨兵。
// ⚠ 不能用 0：0 是**合法掩码**（三个关键字全关，也是最常见的材质）。
//    第一个物体恰好是全关时会被误判成"已经绑过了"而跳过绑定 ——
//    结果是拿天空盒/栅格管线残留的状态去画几何。
constexpr KeywordMask kBindsNothing = 0xFFFFFFFFu;

// 把局部 AABB 经世界矩阵变换后重新求包围盒（8 个角点变换 + 取 min/max）。
// 物体带旋转时，变换后的角点不再轴对齐，所以必须重新收紧一次外包盒 ——
// 直接把 lo/hi 乘矩阵会得到错误的（可能更小甚至反转的）盒子。
void transformAabb(const glm::mat4& m, const glm::vec3& lo, const glm::vec3& hi,
                   glm::vec3& outLo, glm::vec3& outHi) {
    const glm::vec3 corners[8] = {
        {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {lo.x, hi.y, lo.z},
        {hi.x, hi.y, lo.z}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z},
        {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z},
    };
    outLo = glm::vec3(1e30f);
    outHi = glm::vec3(-1e30f);
    for (const glm::vec3& c : corners) {
        const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
        outLo = glm::min(outLo, w);
        outHi = glm::max(outHi, w);
    }
}

} // namespace

Renderer::Renderer(core::Window& window) : m_window(window) {
    // 物体级视锥剔除开关。默认开；MYVK_FRUSTUM_CULL=0 可关闭 ——
    // 自动化验证靠它做"开关两次画面必须一致"的对照。
    m_frustumCullingEnabled = envBool("MYVK_FRUSTUM_CULL", true);

    // LOD 开关。默认开；MYVK_LOD=0 退回"全部 LOD0"，同样是对照组。
    m_lodEnabled = envBool("MYVK_LOD", true);
    // 全局偏差与强制层级：验证脚本靠它们做"偏差真的把分界推过去了"
    // 和"某一级长什么样"的断言。
    if (const char* s = std::getenv("MYVK_LOD_BIAS")) {
        const float v = static_cast<float>(std::atof(s));
        if (v > 0.0f) m_lodBias = v;
    }
    if (const char* s = std::getenv("MYVK_LOD_FORCE")) {
        m_forcedLodLevel = std::atoi(s);
    }

    // IBL / 天空盒开关。两个都默认开。
    //   MYVK_IBL=0 → pbr.frag 回退到旧的程序化天空梯度近似
    //   MYVK_SKY=0 → 不画天空盒背景（露出 HDR 清屏色）
    // 分开是为了让验证脚本能分别隔离"新背景 pass"与"新 IBL 数学"。
    m_iblEnabled = envBool("MYVK_IBL", true);
    m_skyEnabled = envBool("MYVK_SKY", true);

    // ---- 级联阴影（CSM）----
    //   MYVK_SHADOW=0          → 完全关掉阴影（shader 里 shadowParams.w = 0）
    //   MYVK_SHADOW_CASCADES=n → 1..4。**1 就是旧单张图的行为**，
    //                            验证脚本靠它做"CSM 改善的是哪一段"的对照
    //   MYVK_SHADOW_DIST=米    → 最远阴影距离（距离分级的硬边界）
    //   MYVK_SHADOW_LAMBDA=f   → 级联分割的 log/uniform 混合系数
    //   MYVK_SHADOW_RES=像素   → 每级的分辨率
    m_shadowsEnabled = envBool("MYVK_SHADOW", true);
    if (const char* s = std::getenv("MYVK_SHADOW_CASCADES")) {
        m_shadowCascades = std::min(std::max(std::atoi(s), 1), 4);
    }
    if (const char* s = std::getenv("MYVK_SHADOW_DIST")) {
        const float v = static_cast<float>(std::atof(s));
        if (v > 0.0f) m_shadowDistance = v;
    }
    if (const char* s = std::getenv("MYVK_SHADOW_LAMBDA")) {
        const float v = static_cast<float>(std::atof(s));
        m_shadowLambda = std::min(std::max(v, 0.0f), 1.0f);
    }
    if (const char* s = std::getenv("MYVK_SHADOW_RES")) {
        const int v = std::atoi(s);
        if (v >= 256) m_shadowResolution = static_cast<uint32_t>(v);
    }

    // ---- 局部光源阴影（点光 / 聚光灯）----
    //   MYVK_LOCAL_SHADOW=0      → 关掉（图仍渲染，片元不再采样）
    //   MYVK_LOCAL_SHADOW_LIGHTS=n → 本帧最多几盏灯投（按到相机距离由近到远取）
    //   MYVK_LOCAL_SHADOW_RES=px  → 每层分辨率（改它要重建资源，init 时）
    //   MYVK_LOCAL_SHADOW_BIAS=a,b,c → 常数(米) / 斜率(米) / PCF 半径(纹素)
    m_localShadowsEnabled = envBool("MYVK_LOCAL_SHADOW", true);
    if (const char* s = std::getenv("MYVK_LOCAL_SHADOW_LIGHTS")) {
        m_localShadowMaxLights = static_cast<uint32_t>(std::max(std::atoi(s), 0));
    }
    if (const char* s = std::getenv("MYVK_LOCAL_SHADOW_BIAS")) {
        float a = m_localShadowBias.x, b = m_localShadowBias.y,
              c = m_localShadowBias.z;
        if (std::sscanf(s, "%f,%f,%f", &a, &b, &c) >= 1) {
            m_localShadowBias.x = a;
            m_localShadowBias.y = b;
            m_localShadowBias.z = c;
        }
    }

    // ---- Shader 变体 / 关键字（见 render/ShaderKeywords.h）----
    //   MYVK_SHADER_FORCE_MASK=n → 无视材质，所有绘制项都用掩码 n（0..7）。
    //     逐个变体做对照实验用：比如 =4（只开 NORMAL_MAP）能看出法线贴图
    //     到底改了多少像素，=1 能看出双面是否真的画出了背面。
    //     默认 -1 = 不强制，按材质推导。
    //   MYVK_SHADER_VARIANTS=1 在 recordFrame 里读，决定打不打每 30 帧
    //     那条 SHADER-VARIANTS 自检日志（必须在前向 Pass 之后打，
    //     否则管线切换次数还没累加出来）。
    if (const char* s = std::getenv("MYVK_SHADER_FORCE_MASK")) {
        const int v = std::atoi(s);
        if (v >= 0 && v <= static_cast<int>(kKeywordAll)) {
            m_forcedKeywordMask = v;
        }
    }

    // ---- 屏幕空间效果（SSAO / 雾 / SSGI / SSR / God rays）----
    // 全部默认关：像素级回归依赖"什么都不开 = 基线逐位不变"。
    // 环境变量只做初值，运行时由 Engine 面板实时切（零重建）。
    m_screenSpace.ssao = envBool("MYVK_SSAO", false);
    m_screenSpace.fog = envBool("MYVK_FOG", false);
    m_screenSpace.ssgi = envBool("MYVK_SSGI", false);
    m_screenSpace.ssr = envBool("MYVK_SSR", false);
    m_screenSpace.godrays = envBool("MYVK_GODRAYS", false);
    envFloat("MYVK_SSAO_RADIUS", m_screenSpace.ssaoRadius);
    envFloat("MYVK_SSAO_STRENGTH", m_screenSpace.ssaoStrength);
    envFloat("MYVK_FOG_DENSITY", m_screenSpace.fogDensity);
    envFloat("MYVK_FOG_FALLOFF", m_screenSpace.fogHeightFalloff);
    envFloat("MYVK_SSGI_STRENGTH", m_screenSpace.ssgiStrength);
    envFloat("MYVK_SSGI_RADIUS", m_screenSpace.ssgiRadius);
    envFloat("MYVK_SSR_STRENGTH", m_screenSpace.ssrStrength);
    envFloat("MYVK_GODRAYS_STRENGTH", m_screenSpace.godraysStrength);

    // ---- 纹理能力钩子（默认全维持旧行为：无 mip、设备默认各向异性）----
    m_texMipmaps = envBool("MYVK_MIPMAP", false);
    m_texStream = envBool("MYVK_TEXSTREAM", false);
    // <0 = 不覆盖（用设备上限）；0-8 覆盖（1 = 强制各向同性）
    m_anisoOverride = -1.0f;
    float aniso = 0.0f;
    if (envFloat("MYVK_ANISO", aniso) && aniso > 0.0f)
        m_anisoOverride = aniso;
}

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
        // SSAO 的 R8 图 / FrameGraph 的工作图被 Post 的 framebuffer 与
        // 描述符（按视图句柄）引用 → 先于 HdrTarget（其深度图被这两个
        // 模块的描述符引用）销毁。
        m_frameGraph.reset();
        m_ssao.reset();
        m_hdrTarget.reset();

        m_depthPrePipelines.clear();
        m_pipelineSky.reset();
        m_gridPipeline.reset();
        m_forwardPipelines.clear();

        m_lighting.reset();

        m_shadowPass.reset();
        // 局部阴影图也被 set 0 的描述符（binding 12）引用 → 同样早于
        // m_descriptors 释放
        m_localShadowPass.reset();
        // 环境图被 set 0 的描述符引用 → 必须早于 m_descriptors 释放
        m_environment.reset();
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
    ctx.anisotropyOverride = m_anisoOverride;
    ctx.generateMipmaps = m_texMipmaps;
    ctx.streamMips = m_texStream;
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
    // GPU instancing / 合批：默认关（像素级回归的基线路径）。开着时
    // 渲染结果必须与关着逐像素一致 —— 验证脚本的核心断言。
    m_instancingEnabled = envBool("MYVK_INSTANCING", false);

    // 8) 后处理（Bloom + Tonemap，并持有交换链的 render pass）
    createPostProcess();

    // 8.5) 屏幕空间效果（SSAO 在预通道与前向之间跑，FrameGraph 在
    //      前向与后处理之间跑 —— 都要在第一次 drawFrame 前建好）。
    //      放在 createPostProcess 之后：renderExtent() 依赖 m_post。
    createScreenSpace();

    // 9) 分簇光照的 GPU 数据 + compute 管线
    createClusteredLighting();

    // 9.5) 天空盒 + IBL 环境贴图（烘焙一次）。
    //      必须在 createPipelines 之前？不 —— 天空管线只依赖 HdrTarget
    //      的 render pass。但它必须在 writeGlobalDescriptors 之前，
    //      因为 set 0 的 binding 7..10 要绑到这些图上。
    createEnvironment();

    // 10) 渲染管线（必须晚于 HdrTarget —— 它要 HDR 的 render pass）
    createPipelines();

    // 10.5) shader 热重载：注册监听名单（要在管线创建之后 —— 记录初始
    //       mtime，保证启动后第一轮 poll 不把"本来就在的文件"当变化）
    watchShaderFiles();

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
    // set 0：binding 0 = FrameUBO，binding 1 = 级联阴影图，
    //        binding 2/3/4 = 灯/簇/灯索引 SSBO，binding 5 = 遥测，
    //        binding 6 = 骨骼矩阵 SSBO（蒙皮网格，VERTEX 阶段），
    //        7..10 = IBL 三件套 + 天空立方体，
    //        11 = 局部光源阴影矩阵 SSBO，12 = 局部光源阴影图
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

    // 骨骼矩阵 SSBO：可变长度数组（所有蒙皮模型的关节矩阵总和）。
    // VERTEX 阶段访问（pbr.vert / shadow.vert 的 set0/binding6）。
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    // 7..10：IBL 三件套 + 天空立方体（全在片元阶段用）。
    // 挤在 set 0 的理由与灯数据一样：Vulkan 只保证至少 4 个 set 可同时
    // 绑定，而 set 1..6 已经被六张贴图占满。它们本来就是"每帧全局"资源。
    for (uint32_t b = 7; b <= 10; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    // 11：局部光源的**逐层**阴影矩阵（见 render::LightShadowRecord）。
    // VERTEX 阶段要给 shadow.vert 用来变换顶点（depth-only Pass），
    // FRAGMENT 阶段要给 pbr.frag 用来把片元反投回光源空间求参考深度。
    // 同一块 buffer 被两个阶段读，是因为矩阵只存这一份 —— 渲染与采样
    // 不可能用到不同矩阵。
    bindings[11].binding = 11;
    bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                              VK_SHADER_STAGE_FRAGMENT_BIT;

    // 12：局部光源阴影图（2D array 深度图，逐层 = 一盏灯的一个面 / 一整盏聚光）
    bindings[12].binding = 12;
    bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[12].descriptorCount = 1;
    bindings[12].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // 13：GPU instancing 的每实例 model 矩阵（见 buildOpaqueBatches）。
    // 只在顶点阶段读；缓冲可能因扩容换对象 —— 扩容处必须重写本 binding。
    bindings[13].binding = 13;
    bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    // 14：SSAO 结果图（R8 遮蔽图，见 render/SSAOPass.h）。关闭时绑
    // 缺省白图 —— 采样恒 1.0，pbr.frag 的 ambient 乘法逐位不变。
    // 布局上永远存在这个 binding（与 7..10 的 IBL 图同一待遇），
    // 着色器有没有用到它由各自编译单元决定。
    bindings[14].binding = 14;
    bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[14].descriptorCount = 1;
    bindings[14].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    m_globalSetLayout = rhi::makeSetLayout(
        *m_device, std::vector<VkDescriptorSetLayoutBinding>(bindings,
                                                             bindings + kGlobalSetBindings));

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
        *m_device, m_globalSetLayout, MAX_FRAMES_IN_FLIGHT, m_shadowResolution);
    m_shadowPass->setCascadeCount(m_shadowCascades);

    // 局部光源阴影的分辨率也走环境变量（与级联同款，便于验证脚本在
    // 不改代码的前提下单独调整这一路）。
    uint32_t localRes = render::LocalShadowPass::kDefaultResolution;
    envUint("MYVK_LOCAL_SHADOW_RES", 128, localRes);
    m_localShadowResolution = localRes;
    m_localShadowPass = std::make_unique<render::LocalShadowPass>(
        *m_device, m_globalSetLayout, MAX_FRAMES_IN_FLIGHT,
        m_localShadowResolution);

    m_localShadowMatrices.assign(render::kMaxLocalShadowLayers, glm::mat4(0.0f));
    m_localShadowLayerLight.assign(render::kMaxLocalShadowLayers, -1);
    m_localShadowNear.assign(render::kMaxLocalShadowLayers, 0.0f);
    m_localShadowFar.assign(render::kMaxLocalShadowLayers, 0.0f);
}

void Renderer::createEnvironment() {
    render::EnvSettings es;

    // 天空模型：默认值与 pbr.frag 的 kFallbackZenith/Ground 一致，
    // 这样"IBL 开/关"两次渲染的天空是同一个，画面差异只来自着色数学。
    // 验证脚本用这两个变量来构造**可判定的因果实验**：
    // 把天顶设成纯红、地面设成纯蓝，一个金属镜面球的上半必然偏红、
    // 下半必然偏蓝 —— 这同时证明了天空生成、面号映射、预过滤、
    // BRDF 表整条链路都在真的生效（任一处错都会让颜色对不上）。
    envVec3("MYVK_SKY_ZENITH", es.zenith);
    envVec3("MYVK_SKY_GROUND", es.ground);

    // 尺寸/质量覆盖（缩小可以让验证跑得快，放大用于目视）
    envUint("MYVK_SKY_RES", 16, es.skySize);
    envUint("MYVK_IRRADIANCE_RES", 8, es.irradianceSize);
    envUint("MYVK_PREFILTER_RES", 16, es.prefilterSize);
    envUint("MYVK_PREFILTER_MIPS", 1, es.prefilterMips);
    envUint("MYVK_LUT_RES", 16, es.lutSize);
    envUint("MYVK_IBL_SAMPLES", 16, es.prefilterSamples);
    envUint("MYVK_BRDF_SAMPLES", 16, es.brdfSamples);

    m_environment = std::make_unique<render::Environment>(
        *m_device, *m_commandPool, es);
    m_environment->build();

    if (!m_environment->valid()) {
        VK_LOG_ERROR("Environment: build failed, falling back to the "
                     "procedural sky approximation");
        m_environment.reset();
        return;
    }

    const render::Environment::Stats& st = m_environment->stats();
    VK_LOG_INFO("Environment: sky=%u irr=%u pref=%ux%u mips lut=%u",
                st.skySize, st.irradianceSize, st.prefilterSize,
                st.prefilterMips, st.lutSize);

    if (std::getenv("MYVK_ENV_DBG")) {
        const render::Environment::Probe& p = m_environment->probe();
        // 面序：0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z。
        // up = +Z（天顶方向）、side = +Y（水平）、down = -Z（地面方向）。
        // up/side/down 的亮度必须**严格递减** —— 这条断言在 C++ 侧就能
        // 抓到"天空梯度落错轴"（Z-up 引擎里写成 dir.y 的经典 bug）。
        VK_LOG_INFO(
            "ENV-DBG sky=%u irr=%u pref=%u mips=%u lut=%u",
            st.skySize, st.irradianceSize, st.prefilterSize,
            st.prefilterMips, st.lutSize);
        VK_LOG_INFO(
            "ENV-READBACK up=(%.4f,%.4f,%.4f) side=(%.4f,%.4f,%.4f) "
            "down=(%.4f,%.4f,%.4f) lutA10=%.4f lutB10=%.4f "
            "lutA55=%.4f lutB55=%.4f",
            static_cast<double>(p.face[4].x), static_cast<double>(p.face[4].y),
            static_cast<double>(p.face[4].z), static_cast<double>(p.face[2].x),
            static_cast<double>(p.face[2].y), static_cast<double>(p.face[2].z),
            static_cast<double>(p.face[5].x), static_cast<double>(p.face[5].y),
            static_cast<double>(p.face[5].z),
            static_cast<double>(p.lutA_at_ndotv1_rough0),
            static_cast<double>(p.lutB_at_ndotv1_rough0),
            static_cast<double>(p.lutA_at_half),
            static_cast<double>(p.lutB_at_half));
    }
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
    // FXAA 默认关（保持既有画面基线；像素级回归依赖它关着）。
    // Render 面板的 checkbox 运行时可切（资源常驻，切换零重建）。
    m_post->settings().fxaa = envBool("MYVK_FXAA", false);

    std::vector<VkImageView> hdrViews;
    hdrViews.reserve(m_hdrTarget->framesInFlight());
    for (uint32_t i = 0; i < m_hdrTarget->framesInFlight(); ++i)
        hdrViews.push_back(m_hdrTarget->colorView(i));  // = resolve 视图

    m_post->recreate(m_swapchain->views(), m_swapchain->extent(), hdrViews,
                     viewportActive() ? VkExtent2D{m_viewportWidth,
                                                     m_viewportHeight}
                                        : VkExtent2D{0, 0});
}

// ---------------------------------------------------------------- 屏幕空间效果（SSAO + FrameGraph 链）

void Renderer::createScreenSpace() {
    if (!m_ssao)
        m_ssao = std::make_unique<render::SSAOPass>(*m_device,
                                                    MAX_FRAMES_IN_FLIGHT);
    if (!m_frameGraph)
        m_frameGraph = std::make_unique<render::FrameGraph>(
            *m_device, m_hdrTarget->colorFormat(), MAX_FRAMES_IN_FLIGHT);
    recreateScreenSpace();
}

void Renderer::recreateScreenSpace() {
    const VkExtent2D ext = renderExtent();
    const VkSampleCountFlagBits samples = m_hdrTarget->samples();
    const VkFormat depthFormat = m_hdrTarget->depthFormat();

    m_ssao->recreate(ext, samples, depthFormat);
    m_frameGraph->recreate(ext, samples, depthFormat);

    // 深度可采样视图每次 HdrTarget 重建后都会换 → 全部重喂
    std::vector<VkImageView> depthViews;
    depthViews.reserve(m_hdrTarget->framesInFlight());
    for (uint32_t i = 0; i < m_hdrTarget->framesInFlight(); ++i)
        depthViews.push_back(m_hdrTarget->depthSampledView(i));
    m_ssao->setDepthViews(depthViews);
    m_frameGraph->setDepthViews(depthViews);
}

std::vector<VkImageView> Renderer::postInputViews() const {
    std::vector<VkImageView> views;
    views.reserve(m_hdrTarget->framesInFlight());

    // 链上有 pass 激活 → PostProcess 采样链的输出工作图；
    // 全关 → 直接采前向 resolve 的 HDR 图（与引入链之前一致）。
    if (m_frameGraph && render::FrameGraph::chainActive(m_screenSpace)) {
        for (uint32_t i = 0; i < m_hdrTarget->framesInFlight(); ++i)
            views.push_back(m_frameGraph->outputView(i, m_screenSpace));
        return views;
    }
    for (uint32_t i = 0; i < m_hdrTarget->framesInFlight(); ++i)
        views.push_back(m_hdrTarget->colorView(i));
    return views;
}

void Renderer::rewirePostInput() {
    m_device->waitIdle();
    m_post->recreate(m_swapchain->views(), m_swapchain->extent(),
                     postInputViews(),
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

// ---------------------------------------------------------------- 角色详情窗口预览离屏链

void Renderer::setPreviewSize(uint32_t width, uint32_t height) {
    if (width > 0 && width < 16) width = 16;
    if (height > 0 && height < 16) height = 16;
    if (width == m_previewWidth && height == m_previewHeight) return;
    m_previewWidth = width;
    m_previewHeight = height;
    m_previewDirty = true;
}

uint64_t Renderer::previewTextureId() const {
    if (m_previewTextureSets.empty()) return 0;
    const VkDescriptorSet ds =
        m_previewTextureSets[m_currentFrame % m_previewTextureSets.size()];
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ds));
}

void Renderer::destroyPreviewTextures() {
    for (auto ds : m_previewTextureSets) {
        if (ds != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(ds);
    }
    m_previewTextureSets.clear();
    m_previewTextureViews.clear();
}

void Renderer::createPreviewTextures() {
    if (!m_previewPost || !m_previewPost->offscreen() || !m_imgui) {
        destroyPreviewTextures();
        return;
    }
    std::vector<VkImageView> views;
    const uint32_t n = m_previewHdrTarget->framesInFlight();
    views.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
        views.push_back(m_previewPost->sceneOutputView(i));
    if (views == m_previewTextureViews && !views.empty()) return;
    destroyPreviewTextures();
    m_previewTextureSets.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i) {
        if (views[i] == VK_NULL_HANDLE) continue;
        m_previewTextureSets[i] = ImGui_ImplVulkan_AddTexture(
            views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    m_previewTextureViews = std::move(views);
}

void Renderer::applyPreviewSize() {
    m_previewDirty = false;
    if (!m_device) return;
    m_device->waitIdle();

    // 懒创建预览离屏链（第一次打开角色窗口时才建，省去常驻开销）
    if (!m_previewHdrTarget) {
        m_previewHdrTarget = std::make_unique<render::HdrTarget>(
            *m_device, MAX_FRAMES_IN_FLIGHT, m_sampleCount);
    }
    if (!m_previewPost) {
        m_previewPost = std::make_unique<render::PostProcess>(
            *m_device, m_swapchain->format(), m_previewHdrTarget->colorFormat(),
            MAX_FRAMES_IN_FLIGHT);
    }

    const VkExtent2D ext{m_previewWidth, m_previewHeight};
    m_previewHdrTarget->recreate(ext.width, ext.height);

    std::vector<VkImageView> hdrViews;
    hdrViews.reserve(m_previewHdrTarget->framesInFlight());
    for (uint32_t i = 0; i < m_previewHdrTarget->framesInFlight(); ++i)
        hdrViews.push_back(m_previewHdrTarget->colorView(i));

    // 预览链用离屏输出模式（viewportExtent 非零 → 合成写离屏图）
    m_previewPost->recreate(m_swapchain->views(), m_swapchain->extent(),
                            hdrViews, ext);

    createPreviewTextures();
    VK_LOG_INFO("Preview render size -> %ux%u (offscreen)", ext.width,
                ext.height);
}

// ---------------------------------------------------------------- 预览渲染

// 把指定场景的相机 / 灯 / 蒙皮数据上传到本帧全局缓冲。
// drawFrame 与 drawPreview 共用 —— 只是各自针对不同场景各调一次。
void Renderer::uploadSceneState(scene::Scene& scene, VkExtent2D extent,
                                bool resetSkinning) {
    auto& frame = m_frames[m_currentFrame];

    // ---- FrameUBO 的前半段（相机 / 光）----
    FrameUBO ubo{};
    ubo.view = scene.camera().viewMatrix();
    ubo.proj = scene.camera().projMatrix(
        static_cast<float>(extent.width) / static_cast<float>(extent.height));
    glm::vec3 ld = glm::normalize(scene.light().direction);
    ubo.lightDir = glm::vec4(ld, scene.light().intensity);
    ubo.lightColor = glm::vec4(scene.light().color, 1.0f);
    ubo.camPos = glm::vec4(scene.camera().position(), 1.0f);

    // 近远平面**必须从相机读**，不能写死常量：级联分割点、切片角点的
    // NDC 换算、以及分簇的对数切片都假设"这里的 near/far"与
    // `camera().projMatrix()` 用的是同一组值。默认都是 0.1 / 200，
    // 但场景可以改，写死就会悄悄错位。
    const float kNear = scene.camera().nearPlane();
    const float kFar = scene.camera().farPlane();

    // ---- 级联阴影：由**相机视锥切片**算 N 级光源空间矩阵 ----
    // 注意这里不再需要"场景投影体包围球"了 —— 那是单张图时代的做法
    // （一张图盖住整个场景）。CSM 每级只覆盖一个视锥切片，包围球由切片
    // 自己定，与场景里有什么无关。顺带解决了老做法的两个老毛病：
    // 场景里一加个远处的大物体，近处那级的精度就跟着掉；以及"没有投影体"
    // 时要靠 camera.target() 兜底。
    m_shadowPass->setCascadeCount(m_shadowCascades);
    m_shadowPass->updateCascades(ubo.view, ubo.proj, kNear, kFar, ld,
                                 m_shadowDistance, m_shadowLambda);

    const uint32_t cascades = m_shadowPass->cascadeCount();
    for (uint32_t c = 0; c < render::kShadowCascadeMax; ++c) {
        ubo.lightSpace[c] = m_shadowPass->lightSpace(c);
    }
    // 关闭时 w=0：着色器直接短路返回"完全受光"，连级联都不用选。
    ubo.shadowParams = glm::vec4(
        static_cast<float>(cascades),
        m_shadowPass->maxDistance(),   // 已经夹到相机远平面以内
        m_shadowFadeStart,
        m_shadowsEnabled ? 1.0f : 0.0f);
    ubo.cascadeSplits = glm::vec4(
        m_shadowPass->split(0), m_shadowPass->split(1),
        m_shadowPass->split(2), m_shadowPass->split(3));
    ubo.shadowBias = m_shadowBias;

    // 诊断：把每级的分割点与级联数打出来。级联分割是这个功能里最容易
    // "看着没问题其实糊了"的部分（半径算大了 → 精度全浪费）。
    // 和 LOD-STATS 一样 30 帧一条，否则每帧一条会把日志刷爆。
    static const bool shadowDbg = std::getenv("MYVK_SHADOW_DBG") != nullptr;
    static uint32_t shadowDbgTick = 0;
    if (resetSkinning && shadowDbg && (shadowDbgTick++ % 30) == 1) {
        VK_LOG_INFO("SHADOW-DBG cascades=%u dist=%.1f lambda=%.2f "
                    "splits=%.2f/%.2f/%.2f/%.2f bias=(%.4f,%.4f,%.1f,%.2f) "
                    "enabled=%d",
                    cascades, static_cast<double>(m_shadowPass->maxDistance()),
                    static_cast<double>(m_shadowLambda),
                    static_cast<double>(m_shadowPass->split(0)),
                    static_cast<double>(m_shadowPass->split(1)),
                    static_cast<double>(m_shadowPass->split(2)),
                    static_cast<double>(m_shadowPass->split(3)),
                    static_cast<double>(m_shadowBias.x),
                    static_cast<double>(m_shadowBias.y),
                    static_cast<double>(m_shadowBias.z),
                    static_cast<double>(m_shadowBias.w),
                    m_shadowsEnabled ? 1 : 0);
    }

    // ---- 局部光源 ----
    scene.collectLights(m_lightScratch);
    // 层分配必须夹在"收集"与"上传"之间：分配结果（层号）写回
    // m_lightScratch 的 cosOuterType.z，随灯表一起送到片元侧。
    //
    // **只有主场景才做分配**。预览场景（resetSkinning=false）不渲染局部
    // 阴影图，若它也去改写 binding11 的矩阵与灯表层号，就会把主场景刚
    // 录好的那套覆盖掉——GPU 在提交后才读这块 buffer，覆盖即错。
    // 不分配时灯表里全是 z=-1，片元查不到层号自然不采样，正是想要的。
    if (resetSkinning) {
        prepareLocalShadows(scene, glm::vec3(ubo.camPos));
    }
    m_lighting->uploadLights(m_currentFrame, m_lightScratch);

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
    // IBL 开关与预过滤 mip 上限。环境烘不出来时强制走旧路径 ——
    // 否则着色器会去采一张还没写过的图（全黑），金属会突然变死色。
    const bool envOk = m_environment && m_environment->valid();
    ubo.envParams = glm::vec4(
        (m_iblEnabled && envOk) ? 1.0f : 0.0f,
        envOk ? m_environment->prefilterMaxLod() : 0.0f, 0.0f, 0.0f);
    // 局部光源阴影参数。w = 总开关 —— 关掉后片元完全不采样那张图，
    // 于是"关阴影"与"开着但没灯"两条路径的像素必须一致。
    ubo.localShadowParams =
        glm::vec4(m_localShadowBias.x, m_localShadowBias.y, m_localShadowBias.z,
                  m_localShadowsEnabled ? 1.0f : 0.0f);

    // ---- 屏幕空间效果的公共输入（主场景专用）----
    // SSAO / FrameGraph 推常量的源头 + 雾参数。预览场景不做屏幕空间
    // 效果：m_* 缓存量不被预览相机覆盖，fogParams.z=0 让着色器短路。
    if (resetSkinning) {
        m_viewProj = ubo.proj * ubo.view;
        m_invProj = glm::inverse(ubo.proj);
        // 透视系数直接抄 proj：P00 / P11（含 Y 翻转）/ P22 / P23。
        // RH 透视 clip.w = -viewZ → 视图点 v 的屏幕 uv =
        // (P00·vx, P11·vy)/(-vz) · 0.5 + 0.5，与深度重建严格互逆。
        m_projCoefs = glm::vec4(ubo.proj[0][0], ubo.proj[1][1],
                                ubo.proj[2][2], ubo.proj[3][2]);
        m_camNear = kNear;
        m_camFar = kFar;
    }
    // 雾参数进 FrameUBO（pbr.frag 尾部的高度雾块）。x=密度 y=高度衰减
    // z=开关 —— 关着时 z=0，着色器整块短路，基线逐位不变。
    if (resetSkinning && m_screenSpace.fog) {
        ubo.fogParams = glm::vec4(m_screenSpace.fogDensity,
                                  m_screenSpace.fogHeightFalloff, 1.0f, 0.0f);
    } else {
        ubo.fogParams = glm::vec4(0.0f);
    }
    std::memcpy(frame.uboMapped, &ubo, sizeof(FrameUBO));

    // ---- 相机视锥 + 本帧绘制列表（物体级视锥剔除在这里发生）----
    // 每个场景用自己的相机算视锥，所以必须先于该场景的几何录制。
    m_sceneFrustum = extractFrustum(ubo.proj * ubo.view);
    m_sceneFrustumValid = true;
    // 重置蒙皮的调用方就是"主场景"（预览场景传 false 追加骨骼矩阵），
    // 借用它来区分统计口径：预览的物体数不该算进主场景的剔除统计。
    buildDrawLists(scene, scene.camera().position(), resetSkinning);

    // ---- 蒙皮 ----
    updateSkinning(scene, resetSkinning);
}

// 构建本帧绘制列表：视锥剔除 + 世界矩阵缓存 + 透明排序，一次做完。
//
// 剔除只对"有包围盒"的网格生效：顶点为空 / 包围盒未知的网格保守保留，
// 宁可多画也不能漏画（漏画是肉眼可见的 bug）。
void Renderer::buildDrawLists(scene::Scene& scene, const glm::vec3& camPos,
                              bool isMain) {
    m_opaqueDraws.clear();
    m_transparentDraws.clear();
    // 实例 SSBO 槽游标：主场景从头排（照骨骼 SSBO 的 resetOffset 模式），
    // 预览场景紧跟在主场景的槽后面继续累加 —— 同一块 buffer 互不覆盖。
    if (isMain) m_instanceOffset = 0;

    // ---- 1) 空间索引 ----
    // 本帧只取一次：每次调用都要把全场景的世界矩阵 + 8 个角点算一遍，
    // 在每个 pass 里各调一次就是重复付这份钱。剔除与编辑器的射线拾取
    // 共用同一棵（见 Scene::spatialIndex）。
    scene::Bvh& bvh = scene.spatialIndex();
    if (isMain) m_stats.renderables = static_cast<uint32_t>(bvh.leafCount());

    // ---- 2) 候选集：开剔除就向 BVH 要"视锥内"的实体 ----
    // 这是 BVH 相对老版线性扫描的价值所在：节点包围盒一旦落在视锥外，
    // 整棵子树的实体连"6 个平面测试"都不用做。
    // 关剔除走 collectAll（全量），用于对比验证剔除有没有漏画。
    m_visibleScratch.clear();
    if (m_frustumCullingEnabled && m_sceneFrustumValid) {
        bvh.query(m_sceneFrustum, m_visibleScratch);
        if (isMain) {
            m_stats.bvhNodeVisits = bvh.lastQueryNodeVisits();
            m_stats.culledFrustum =
                m_stats.renderables > m_visibleScratch.size()
                    ? m_stats.renderables -
                          static_cast<uint32_t>(m_visibleScratch.size())
                    : 0;
        }
    } else {
        bvh.collectAll(m_visibleScratch);
    }

    // ---- 自检（MYVK_BVH_DBG=1）：BVH 的结果必须与"逐个实体直接测视锥"
    //      一致。任何一个实体被判可见而 BVH 漏掉，就是树建错了
    //      （节点包围盒没包住自己的成员）—— 那是会漏画的硬错误。
    if (isMain && std::getenv("MYVK_BVH_DBG") && m_sceneFrustumValid) {
        std::unordered_map<uint32_t, char> inBvh;
        for (ecs::Entity e : m_visibleScratch) inBvh[e.id] = 1;

        uint32_t brute = 0, missing = 0, extra = 0;
        scene.forEachRenderable([&](ecs::Entity e, ecs::MeshComponent& mc,
                                    ecs::MaterialComponent&) {
            if (!mc.mesh || !mc.mesh->valid() || !mc.mesh->hasBounds()) return;
            glm::vec3 wlo, whi;
            transformAabb(scene.worldMatrix(e), mc.mesh->boundsMin(),
                          mc.mesh->boundsMax(), wlo, whi);
            const bool vis = m_sceneFrustum.intersects(wlo, whi);
            const bool inList = inBvh.find(e.id) != inBvh.end();
            if (vis) ++brute;
            if (vis && !inList) ++missing;
            if (!vis && inList) ++extra;
        });
        VK_LOG_INFO("BVH-DBG brute=%u bvh=%llu missing=%u extra=%u", brute,
                    static_cast<unsigned long long>(m_visibleScratch.size()),
                    missing, extra);
    }

    // ---- 3) 建绘制列表（只遍历候选集）----
    //
    // LOD 的屏幕占高比要用相机竖直半 FOV；每帧算一次就够。
    // 注意：**相对视口高度**的比值，所以与视口宽高比无关，只要 fovY。
    const float tanHalfFovY =
        std::tan(glm::radians(scene.camera().fov()) * 0.5f);
    const float invTanHalf = tanHalfFovY > 1e-4f ? 1.0f / tanHalfFovY : 0.0f;

    ecs::World& w = scene.world();
    for (ecs::Entity e : m_visibleScratch) {
        auto* mc = w.get<ecs::MeshComponent>(e);
        if (!mc || !mc->mesh || !mc->mesh->valid()) continue;

        // 世界矩阵只算一次（三条提交路径共用）。Scene::worldMatrix 要沿
        // 父链递归累乘，原本每条路径各算一遍是纯粹的重复开销。
        const glm::mat4 model = scene.worldMatrix(e);

        // ---- LOD 选层 ----
        const assets::Mesh* mesh = mc->mesh;
        int lodLevel = 0;
        const assets::LodChain* chain = mesh->lodChain();
        const bool lodActive =
            chain && chain->valid() && (m_lodEnabled || m_forcedLodLevel >= 0);
        if (lodActive) {
            size_t lv;
            if (m_forcedLodLevel >= 0) {
                // 强制层级：跳过距离计算，但仍然把它记进选择表 —— 这样
                // 从"强制"切回"自动"时不会因为表里的旧值乱跳一次。
                lv = std::min<size_t>(static_cast<size_t>(m_forcedLodLevel),
                                      chain->levelCount() - 1);
            } else {
                // 屏幕占高比 = 物体投影高度 / 视口高度。
                // 用世界 AABB 的**半对角线**当包围球半径：对包围盒来说是
                // 保守上界（真正的最优球要另外算），宁可把它估大一点 ——
                // 估小了会让远处的物体提前掉到低模。
                glm::vec3 wlo, whi;
                transformAabb(model, mesh->boundsMin(), mesh->boundsMax(),
                              wlo, whi);
                const glm::vec3 center = (wlo + whi) * 0.5f;
                const float radius = glm::length(whi - wlo) * 0.5f;
                const float dist =
                    std::max(glm::length(center - camPos), 1e-4f);
                const float ratio = radius * invTanHalf / dist;

                const auto sit = m_lodSelected.find(e.id);
                const int cur =
                    sit == m_lodSelected.end() ? -1 : static_cast<int>(sit->second);
                lv = chain->selectLevel(ratio, m_lodBias, cur);
                if (isMain && std::getenv("MYVK_LOD_DBG") &&
                    cur != static_cast<int>(lv)) {
                    const std::string* nm = w.name(e);
                    VK_LOG_INFO("LOD-SWITCH '%s' %d->%d ratio=%.4f dist=%.2f",
                                nm ? nm->c_str() : "?",
                                cur, static_cast<int>(lv), ratio, dist);
                }
            }
            if (m_lodSelected[e.id] != static_cast<uint8_t>(lv)) {
                ++m_lodSwitchTotal;
                m_lodSelected[e.id] = static_cast<uint8_t>(lv);
            }
            lodLevel = static_cast<int>(lv);
            mesh = chain->level(lv).mesh ? chain->level(lv).mesh : mesh;
            if (isMain) {
                ++m_stats.lodEntities;
                m_stats.lodLevelCounts[lv] =
                    m_stats.lodLevelCounts[lv] + 1;
            }
        }

        DrawItem it;
        it.mesh = mesh;
        it.lodLevel = lodLevel;
        const auto* matc = w.get<ecs::MaterialComponent>(e);
        it.material = matc ? matc->material : nullptr;
        it.model = model;
        // 无 LOD 的物体也要参与"如果全用 0 级会提交多少"的对照
        it.baseTriangles = static_cast<uint32_t>(
            mc->mesh->indices().size() / 3);

        // ---- Shader 变体：材质特性 → 关键字掩码 ----
        // 掩码在这里算一次，深度预通道与前向两条路径共用（见 DrawItem::keywords）。
        // 空材质（只有深度预通道会画）保持"全关"。
        const assets::Material* mat = it.material;
        it.keywords = mat ? effectiveKeywords(*mat) : kKeywordNone;

        // ---- 队列分类也由掩码推导 ----
        // 原来是再判一次 mat->alphaMode —— 那样"什么算半透明"就有两处
        // 定义（掩码里一处、这里一处），改关键字时必然漏掉一处。
        // 现在 ALPHA_BLEND 位是唯一判据。
        if (hasKeyword(it.keywords, kKeywordAlphaBlend)) {
            const glm::vec3 d = glm::vec3(model[3]) - camPos;
            it.viewDepthSq = glm::dot(d, d);
            m_transparentDraws.push_back(it);
        } else {
            // 注意：材质为空也算不透明（深度预通道一直允许空材质）。
            // 前向不透明路径会再跳过空材质的项。
            m_opaqueDraws.push_back(it);
        }
    }

    if (isMain) {
        // GPU instancing 合批要在统计之前 —— opaqueBatches 也进统计。
        buildOpaqueBatches();

        m_stats.drawnOpaque = static_cast<uint32_t>(m_opaqueDraws.size());
        m_stats.drawnTransparent =
            static_cast<uint32_t>(m_transparentDraws.size());
        m_stats.opaqueBatches = m_instancingEnabled
            ? static_cast<uint32_t>(m_opaqueBatches.size())
            : m_stats.drawnOpaque;

        // 提交量统计：只算真正会提交前向绘制命令的项。
        // 空材质的项只有深度预通道会画，不计入前向的 draw call。
        uint64_t tris = 0;
        uint64_t trisNoLod = 0;
        uint32_t calls = 0;
        for (const DrawItem& it : m_opaqueDraws) {
            if (!it.material) continue;
            ++calls;
            tris += it.mesh->indices().size() / 3;
            trisNoLod += it.baseTriangles;
        }
        for (const DrawItem& it : m_transparentDraws) {
            ++calls;
            tris += it.mesh->indices().size() / 3;
            trisNoLod += it.baseTriangles;
        }
        m_stats.drawCalls = calls;
        m_stats.triangles =
            tris > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(tris);
        m_stats.trianglesNoLod = trisNoLod > 0xFFFFFFFFull
            ? 0xFFFFFFFFu : static_cast<uint32_t>(trisNoLod);
        m_stats.lodSwitches = m_lodSwitchTotal;

        // ---- Shader 变体统计 ----
        // 口径与前向真的提交了哪些绘制对齐：不透明里跳掉空材质项
        // （它们只有深度预通道会画），透明项全部计入。
        for (uint32_t i = 0; i <= kKeywordAll; ++i) {
            m_stats.variantCounts[i] = 0;
        }
        for (const DrawItem& it : m_opaqueDraws) {
            if (!it.material) continue;
            ++m_stats.variantCounts[it.keywords & kKeywordAll];
        }
        for (const DrawItem& it : m_transparentDraws) {
            ++m_stats.variantCounts[it.keywords & kKeywordAll];
        }
        // ⚠ SHADER-VARIANTS 自检日志**不在这里**打。buildDrawLists 跑在前向
        //   Pass **之前**，而 variantSwitches 是前向 Pass 里累加的 ——
        //   在这儿读它永远是"这一帧还没开始画"的 0。日志挪到
        //   recordFrame 的前向 Pass 之后。

        // 自检（MYVK_LOD_DBG=1）：每 30 帧把"各层级各几个 + 实际/假设三角形
        // 数"打出来（和 CULL-STATS 同频，否则每帧一条会把日志刷爆）。
        // 验证脚本靠它断言低模真的被选上了、而且省下的量符合预期。
        static const bool lodDbg = std::getenv("MYVK_LOD_DBG") != nullptr;
        static uint32_t lodTick = 0;
        if (lodDbg && (lodTick++ % 30) == 1) {
            VK_LOG_INFO("LOD-STATS ents=%u l0=%u l1=%u l2=%u l3=%u "
                        "tris=%u noLod=%u switches=%u forced=%d bias=%.2f",
                        m_stats.lodEntities, m_stats.lodLevelCounts[0],
                        m_stats.lodLevelCounts[1], m_stats.lodLevelCounts[2],
                        m_stats.lodLevelCounts[3], m_stats.triangles,
                        m_stats.trianglesNoLod, m_stats.lodSwitches,
                        m_forcedLodLevel,
                        static_cast<double>(m_lodBias));
        }

        // 自检（MYVK_DRAW_BATCH=1）：合批效果体温计。items - batches =
        // 省掉的 draw call；batches 一直等于 items 就说明场景里没有
        // 同网格+同材质的相邻物（或合批没生效）。
        static const bool batchDbg = std::getenv("MYVK_DRAW_BATCH") != nullptr;
        static uint32_t batchTick = 0;
        if (batchDbg && (batchTick++ % 30) == 1) {
            VK_LOG_INFO("DRAW-BATCH items=%u batches=%u saved=%u "
                        "transparent=%u instancing=%d",
                        m_stats.drawnOpaque, m_stats.opaqueBatches,
                        m_stats.drawnOpaque - m_stats.opaqueBatches,
                        m_stats.drawnTransparent,
                        m_instancingEnabled ? 1 : 0);
        }
    }

    // 透明物体：远 → 近。混合必须按这个顺序，否则 alpha 累加会错。
    std::sort(m_transparentDraws.begin(), m_transparentDraws.end(),
              [](const DrawItem& a, const DrawItem& b) {
                  return a.viewDepthSq > b.viewDepthSq;
              });
}

// ============================================================
// GPU instancing / 合批
// ============================================================
void Renderer::buildOpaqueBatches() {
    m_opaqueBatches.clear();
    if (!m_instancingEnabled) return;  // 基线路径：逐项提交，不动 SSBO

    // ---- 排序：同 (掩码, 材质, 网格) 挨在一起 ----
    // stable_sort 保住视锥查询的原顺序：同一场景逐帧的批划分一致，
    // 调试时"第 N 批是谁"不会跳。不透明的绘制顺序本来就不影响最终
    // 像素（深度测试裁决），排序只改变提交分组，不改画面。
    std::stable_sort(m_opaqueDraws.begin(), m_opaqueDraws.end(),
                     [](const DrawItem& a, const DrawItem& b) {
                         if (a.keywords != b.keywords)
                             return a.keywords < b.keywords;
                         if (a.material != b.material)
                             return a.material < b.material;
                         return a.mesh < b.mesh;
                     });

    auto& frame = m_frames[m_currentFrame];

    // ---- 容量：本场景的实例槽不够就扩（稀有路径，照 jointMatrices）----
    const uint32_t need =
        m_instanceOffset + static_cast<uint32_t>(m_opaqueDraws.size());
    if (need > frame.instanceCapacity) {
        const uint32_t newCap =
            std::max(frame.instanceCapacity * 2, need + 64);
        frame.instanceModels = std::make_unique<rhi::Buffer>(
            *m_device, newCap * sizeof(render::InstanceData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        frame.instanceMapped = frame.instanceModels->map();
        frame.instanceCapacity = newCap;
        // buffer 换了对象 → binding 13 必须重写（照 jointMatrices）
        writeGlobalDescriptors();
    }
    auto* dst = static_cast<render::InstanceData*>(frame.instanceMapped) +
                m_instanceOffset;

    // ---- 相邻同键聚合：一批 = 一次管线/材质状态 + N 个实例 ----
    // 蒙皮：jointBase 挂在 **mesh** 上（m_skinJointBase），同 mesh 的
    // 实体共享同一份关节矩阵 —— 同 mesh 同材质的批内自然一致，不破坏。
    size_t i = 0;
    while (i < m_opaqueDraws.size()) {
        size_t j = i;
        while (j + 1 < m_opaqueDraws.size() &&
               m_opaqueDraws[j + 1].keywords == m_opaqueDraws[i].keywords &&
               m_opaqueDraws[j + 1].material == m_opaqueDraws[i].material &&
               m_opaqueDraws[j + 1].mesh == m_opaqueDraws[i].mesh) {
            ++j;
        }
        DrawBatch batch;
        batch.mesh = m_opaqueDraws[i].mesh;
        batch.material = m_opaqueDraws[i].material;
        batch.keywords = m_opaqueDraws[i].keywords;
        batch.firstInstance = m_instanceOffset;
        batch.count = static_cast<uint32_t>(j - i + 1);
        for (size_t k = i; k <= j; ++k)
            dst[k - i].model = m_opaqueDraws[k].model;
        dst += batch.count;
        m_instanceOffset += batch.count;
        m_opaqueBatches.push_back(batch);
        i = j + 1;
    }
}

// 把 scene 的几何录制进指定的 HdrTarget（阴影 pass + 深度预通道 +
// 分簇 compute + 前向）。注意：阴影贴图 / 分簇 / UBO 这些全局缓冲已经在
// uploadSceneState 里针对**这个场景**上传过了，这里只负责录制几何。
void Renderer::recordSceneInto(VkCommandBuffer cmd, scene::Scene& scene,
                               class HdrTarget& hdr, bool isPreview) {
    auto& frame = m_frames[m_currentFrame];

    // Pass 1：级联阴影贴图（每个级联一次 depth-only pass）。
    // 预览场景**跳过**：阴影图是全局一张，主场景的采样命令已经录制在先，
    // 预览再写入就是无同步的写/写竞争。预览光照极简（一盏方向光），
    // 没有阴影完全可接受。
    if (!isPreview) {
        recordShadowPasses(cmd, scene, frame.globalUBOSet);
        // 局部光源阴影（点光 / 聚光灯）与级联并列，同样只在主场景渲染。
        // 它排在级联之后：两者互不依赖，但"全部阴影图先写、前向再采样"
        // 这个大顺序必须保持（跨 render pass 的写→读依赖在各自的
        // render pass 里已经声明了）。
        recordLocalShadowPasses(cmd, scene, frame.globalUBOSet);
    }

    // Pass 2：深度预通道（复用现有实现，但用传入的 hdr 目标）
    if (m_depthPrePipelines.initialized()) {
        VkClearValue clear{};
        clear.depthStencil = {1.0f, 0};
        const VkExtent2D ext = hdr.extent();
        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = hdr.depthPrePass();
        rp.framebuffer = hdr.depthFramebuffer(m_currentFrame);
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = ext;
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{};
        vp.width = static_cast<float>(ext.width);
        vp.height = static_cast<float>(ext.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = ext;
        vkCmdSetScissor(cmd, 0, 1, &sc);

        if (m_depthPrePassEnabled) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_depthPrePipelines.layout(), 0, 1,
                                    &frame.globalUBOSet, 0, nullptr);
            recordOpaqueDepthGeometry(cmd);
        }
        vkCmdEndRenderPass(cmd);
    }

    // Pass 3：分簇剔除（compute）
    // 预览场景**跳过**：分簇缓冲是按主视口尺寸建的，预览分辨率不同会错位；
    // 且预览的 compute 会与"主前向 Pass 已录制的读取"构成未同步竞争。
    // 预览场景只有方向光（无点光 → 片元不遍历灯表），跳过完全等价。
    if (!isPreview) {
        m_lighting->record(cmd, m_currentFrame, m_clusterCullingEnabled,
                           frame.globalUBOSet);
    }

    // Pass 4：前向渲染 → HDR → resolve
    VkClearValue clears[2]{};
    clears[0].color = {{0.020f, 0.024f, 0.040f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    const VkExtent2D hdrExtent = hdr.extent();
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = hdr.renderPass();
    rp.framebuffer = hdr.framebuffer(m_currentFrame);
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = hdrExtent;
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.width = static_cast<float>(hdrExtent.width);
    vp.height = static_cast<float>(hdrExtent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{};
    sc.extent = hdrExtent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_forwardPipelines.layout(), 0, 1,
                            &frame.globalUBOSet, 0, nullptr);

    // 天空盒背景（预览视口也画 —— 角色窗口里有个天空好看得多，
    // 而且它不依赖任何 compute/阴影资源，预览场景可以直接用）
    recordSkyBackground(cmd);

    // 不透明：按关键字掩码取变体管线。掩码没变就不重绑 —— 同材质的
    // 相邻物体（场景里最常见的情况）只付一次 vkCmdBindPipeline。
    KeywordMask boundMask = kBindsNothing;
    for (const DrawItem& it : m_opaqueDraws) {
        if (!it.material) continue;  // 空材质只有深度预通道会画
        bindForwardVariant(cmd, it.keywords, boundMask,
                           m_stats.variantSwitches);
        drawMesh(cmd, m_forwardPipelines.layout(), it.model, *it.material,
                 *it.mesh);
    }

    // 灯光可视化（预览里也给点光小球，帮助判断光照）
    if (m_showLightGizmos && m_lightGizmoMesh && !m_lightScratch.empty()) {
        assets::Material gizmoMat;
        gizmoMat.baseColorFactor = glm::vec4(0.0f);
        gizmoMat.metallic = 0.0f;
        gizmoMat.roughness = 1.0f;
        gizmoMat.normalScale = 0.0f;
        gizmoMat.ao = 1.0f;
        // 小球材质是临时构造的：自发光、不透明、不双面、没法线图 →
        // 常态就是掩码 0。走 effectiveKeywords 而不是直接写 0，是为了
        // 让"强制掩码"的对照实验也能一致地管到它。
        const KeywordMask gizmoKw = effectiveKeywords(gizmoMat);
        for (const auto& L : m_lightScratch) {
            const glm::vec3 p(L.positionRange);
            const float r =
                m_lightGizmoScale * (0.6f + 0.4f * L.positionRange.w / 8.0f);
            glm::mat4 model = glm::translate(glm::mat4(1.0f), p);
            model = glm::scale(model, glm::vec3(r));
            const glm::vec3 c =
                glm::vec3(L.colorIntensity) * (L.colorIntensity.a * 0.35f);
            gizmoMat.emissive = glm::clamp(c, glm::vec3(0.0f), glm::vec3(6.0f));
            bindForwardVariant(cmd, gizmoKw, boundMask,
                               m_stats.variantSwitches);
            drawMesh(cmd, m_forwardPipelines.layout(), model, gizmoMat,
                     *m_lightGizmoMesh);
        }
    }

    // 透明（绘制列表已按到相机距离降序排好，远 → 近）
    for (const DrawItem& it : m_transparentDraws) {
        bindForwardVariant(cmd, it.keywords, boundMask,
                           m_stats.variantSwitches);
        drawMesh(cmd, m_forwardPipelines.layout(), it.model, *it.material,
                 *it.mesh);
    }
    vkCmdEndRenderPass(cmd);
}

void Renderer::recordPreview(VkCommandBuffer cmd) {
    if (!m_previewScene) return;
    if (!previewActive()) return;
    // applyPreviewSize 会**懒创建**预览离屏链（第一次打开角色窗口时才建），
    // 所以"目标还不存在"不能作为提前 return 的条件 —— 否则永远建不起来。
    if (m_previewDirty || !m_previewHdrTarget || !m_previewPost)
        applyPreviewSize();
    if (!m_previewHdrTarget || !m_previewPost) return;

    // 上传预览场景的相机 / 灯 / 蒙皮到全局缓冲。
    // 注意：FrameUBO / 灯数据会被覆盖（预览场景录制紧跟主场景之后，
    // 主场景几何命令已录制完毕，drawMesh 的 push constant 是即时值，安全）。
    // 但**蒙皮矩阵**走追加模式（resetSkinning=false），主场景的骨骼矩阵
    // 保留在 SSBO 前半段，预览场景追加在后半段 —— 互不覆盖。
    uploadSceneState(*m_previewScene,
                     VkExtent2D{m_previewWidth, m_previewHeight}, false);
    recordSceneInto(cmd, *m_previewScene, *m_previewHdrTarget,
                    /*isPreview=*/true);

    // 后处理：合成写预览离屏图（不做 ImGui overlay）
    const auto overlay = [](VkCommandBuffer) {};
    m_previewPost->renderScene(cmd, m_currentFrame, m_currentFrame, overlay);
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
    // HdrTarget 的深度视图换了 → SSAO/FrameGraph 的描述符要重喂
    recreateScreenSpace();

    m_post->recreate(m_swapchain->views(), m_swapchain->extent(),
                     postInputViews(),
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
                                    VkDescriptorSetLayout texSet,
                                    bool instancing) {
    // 顶点着色器按 instancing 开关选：合批版从实例 SSBO（binding 13）
    // 按 gl_InstanceIndex 取 model，普通版从 push.model 取。两个 pass
    // （深度预通道 + 前向）必须用同一份，否则深度与着色的顶点变换路径
    // 不同，浮点误差会让 early-Z 的深度对不上。
    desc.vertSpv = vkutil::readFile(resolveShaderPath(
        instancing ? "pbr.inst.vert.spv" : "pbr.vert.spv"));
    // ⚠ 这里**故意不读**片元着色器。
    // 片元阶段由关键字决定：每个变体读自己那份 pbr.k<掩码>.frag.spv
    // （见 VariantPipelines::get）。单体 pbr.frag.spv 已经不存在了 ——
    // 曾经在这里读它，结果是启动即 "Failed to open file: pbr.frag.spv"。
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
    //
    // 走 SingleSpv：所有掩码共用 depth_only.frag.spv，只有固定状态随
    // 掩码变。它不输出颜色，ALPHA_BLEND / NORMAL_MAP 对它没有任何意义，
    // 所以 stateMask 只留 DOUBLE_SIDED（双面材质要两面都写深度，否则
    // 从背面看向双面板时预通道留不下深度、主 Pass 的 early-Z 白铺）。
    {
        rhi::PipelineDesc desc;
        // 深度预通道的顶点阶段与前向**必须同一份**（instancing 同开同关），
        // 否则两条路径的顶点变换不一致，early-Z 会因浮点误差闪烁。
        desc.vertSpv = vkutil::readFile(resolveShaderPath(
            m_instancingEnabled ? "pbr.inst.vert.spv" : "pbr.vert.spv"));
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
        desc.cullMode = VK_CULL_MODE_BACK_BIT;  // 基线；掩码 1 会改成 NONE
        desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        m_depthPrePipelines.init(*m_device, desc, "depth_only.frag.spv",
                                 render::ShaderPermutation::SingleSpv,
                                 render::kKeywordDoubleSided);
    }

    // ---- 前向不透明 / 透明：一族变体，按关键字掩码惰性创建 ----
    // 过去这里是 4 条手写管线 + 后面三处 if 阶梯；现在只有一份"基线描述"，
    // 掩码负责两件事：
    //   · 换 .spv（NORMAL_MAP 少一次采样 + cotangent frame，
    //     DOUBLE_SIDED 多一次 gl_FrontFacing 法线翻转）
    //   · 改固定状态（DOUBLE_SIDED → cullMode=NONE，
    //     ALPHA_BLEND → blendEnable=true + depthWrite=false）
    // 见 VariantPipelines::applyKeywordState。
    //
    // 基线仍是"不透明 + 背面剔除 + 写深度"：掩码 0 编出来的结果与引入
    // 关键字之前逐像素一致（pbr.frag 里三个宏默认都是 0）。
    //
    // depthWrite 基线保持 true：深度预通道已经写过同样的值，重写一次代价
    // 极小，但换来"关掉深度预通道也完全正确" —— 不必为开关维护两套管线。
    {
        rhi::PipelineDesc desc;
        fillForwardPipelineDesc(desc, *m_hdrTarget, m_globalSetLayout,
                                m_textureSetLayout, m_instancingEnabled);
        desc.cullMode = VK_CULL_MODE_BACK_BIT;
        m_forwardPipelines.init(*m_device, desc, "pbr.frag",
                                render::ShaderPermutation::PerMask,
                                render::kKeywordDoubleSided |
                                    render::kKeywordAlphaBlend);
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

    // ---- 天空盒背景（全屏三角形 3 顶点，无顶点缓冲）----
    // 与栅格管线受同一条约束：set 布局与推常量范围必须与主前向管线
    // **逐字段一致**。set 0 是整个 Pass 一次性绑定的，中途出现一个不
    // 兼容的 layout 会让后面所有 draw 报 VUID-...-08600（详见上面栅格
    // 那段）。所以这里照样声明 7 个 set + 128 字节推常量，尽管
    // sky.vert 既不读贴图也不读推常量。
    //
    // depthTest / depthWrite 都关：
    //   · 不测深度 → 背景一定画得出来，不受深度预通道结果影响
    //   · 不写深度 → 后面几何的 early-Z 仍然吃深度预通道的成果
    {
        rhi::PipelineDesc desc;
        desc.vertSpv = vkutil::readFile(resolveShaderPath("sky.vert.spv"));
        desc.fragSpv = vkutil::readFile(resolveShaderPath("sky.frag.spv"));
        desc.setLayouts = {m_globalSetLayout, m_textureSetLayout,
                           m_textureSetLayout, m_textureSetLayout,
                           m_textureSetLayout, m_textureSetLayout,
                           m_textureSetLayout};
        desc.pushConstant.stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        desc.pushConstant.offset = 0;
        desc.pushConstant.size = sizeof(PushConstants);  // 128，与主管线一致
        desc.renderPass = m_hdrTarget->renderPass();
        desc.samples = m_hdrTarget->samples();
        desc.depthTest = false;
        desc.depthWrite = false;
        desc.cullMode = VK_CULL_MODE_NONE;
        m_pipelineSky = std::make_unique<rhi::Pipeline>(*m_device, desc);
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

        // 骨骼矩阵 SSBO：初始 256 个 mat4（够几个角色），不够时 updateSkinning
        // 里按需重建。HOST_VISIBLE + 持久映射。
        const uint32_t kInitialJoints = 256;
        f.jointMatrices = std::make_unique<rhi::Buffer>(
            *m_device, kInitialJoints * sizeof(glm::mat4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        f.jointMapped = f.jointMatrices->map();
        f.jointCapacity = kInitialJoints;

        // 局部光源阴影矩阵 SSBO（set 0 binding 11）：固定 kMaxLocalShadowLayers
        // 条 LightShadowRecord，容量恒定，常驻映射。
        f.lightShadows = std::make_unique<rhi::Buffer>(
            *m_device,
            render::kMaxLocalShadowLayers * sizeof(render::LightShadowRecord),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        f.lightShadowsMapped = f.lightShadows->map();

        // 每实例 model SSBO（set 0 binding 13）：初始 256 个实例，不够时
        // buildOpaqueBatches 里按需重建（照 jointMatrices 的模式）。
        const uint32_t kInitialInstances = 256;
        f.instanceModels = std::make_unique<rhi::Buffer>(
            *m_device, kInitialInstances * sizeof(render::InstanceData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        f.instanceMapped = f.instanceModels->map();
        f.instanceCapacity = kInitialInstances;

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
        // binding 6：骨骼矩阵 SSBO（蒙皮网格，VERTEX 阶段）
        m_descriptors->writeBuffer(
            f.globalUBOSet, 6, f.jointMatrices->get(),
            f.jointMatrices->size(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        // binding 7..10：IBL 三件套 + 天空立方体。
        // 这几张图**所有帧共用**（init 时烘一次就不动了），所以每个
        // 帧的 set 写的是同一批视图 —— 描述符集本身必须按帧分开，
        // 因为 set 0 里还有每帧不同的 UBO / 阴影图 / SSBO。
        if (m_environment && m_environment->valid()) {
            m_descriptors->writeTexture(f.globalUBOSet, 7,
                                        m_environment->cubeSampler(),
                                        m_environment->irradianceView());
            m_descriptors->writeTexture(f.globalUBOSet, 8,
                                        m_environment->cubeSampler(),
                                        m_environment->prefilterView());
            m_descriptors->writeTexture(f.globalUBOSet, 9,
                                        m_environment->lutSampler(),
                                        m_environment->brdfLutView());
            m_descriptors->writeTexture(f.globalUBOSet, 10,
                                        m_environment->cubeSampler(),
                                        m_environment->skyView());
        } else {
            // 环境烘不出来也不该让整个管线失效：退回到缺省白/黑贴图，
            // 描述符始终有效，着色器那边由 envParams.x = 0 走旧路径。
            m_descriptors->writeTexture(f.globalUBOSet, 7,
                                        m_defaultWhite->sampler(),
                                        m_defaultWhite->view());
            m_descriptors->writeTexture(f.globalUBOSet, 8,
                                        m_defaultWhite->sampler(),
                                        m_defaultWhite->view());
            m_descriptors->writeTexture(f.globalUBOSet, 9,
                                        m_defaultWhite->sampler(),
                                        m_defaultWhite->view());
            m_descriptors->writeTexture(f.globalUBOSet, 10,
                                        m_defaultBlack->sampler(),
                                        m_defaultBlack->view());
        }

        // binding 11：局部光源阴影矩阵（逐层一条 LightShadowRecord）。
        // 每帧 in-flight 一块，因为它的内容每帧都会被 prepareLocalShadows 重写。
        m_descriptors->writeBuffer(
            f.globalUBOSet, 11, f.lightShadows->get(), f.lightShadows->size(),
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        // binding 12：局部光源阴影图。同样必须用 DEPTH_STENCIL_READ_ONLY
        // （depth-only attachment 写完后停在 READ_ONLY，采样侧不能按
        //  DEPTH_ATTACHMENT 声明，否则验证层报 layout 不匹配）。
        m_descriptors->writeTexture(
            f.globalUBOSet, 12, m_localShadowPass->sampler(),
            m_localShadowPass->view(i),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

        // binding 13：每实例 model 矩阵（instancing 合批）。扩容换了
        // buffer 对象时 writeGlobalDescriptors 会被重调，这里跟着刷新。
        m_descriptors->writeBuffer(
            f.globalUBOSet, 13, f.instanceModels->get(),
            f.instanceModels->size(), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        // binding 14：SSAO 遮蔽图。开关关着 / SSAO 未就绪时绑缺省
        // 白图 → 采样恒 1.0，pbr.frag 的环境光乘法逐位回到基线。
        if (m_ssao && m_ssao->ready() && m_screenSpace.ssao) {
            m_descriptors->writeTexture(f.globalUBOSet, 14,
                                        m_ssao->sampler(),
                                        m_ssao->resultView(i));
        } else {
            m_descriptors->writeTexture(f.globalUBOSet, 14,
                                        m_defaultWhite->sampler(),
                                        m_defaultWhite->view());
        }
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

    // HdrTarget 整个换新 → 深度视图全换，SSAO/FrameGraph 重建 + 重喂
    recreateScreenSpace();

    // PostProcess 采样的是 resolve 视图 / 链输出（随 HdrTarget 一起换了）
    m_post->recreate(m_swapchain->views(), m_swapchain->extent(),
                     postInputViews(),
                     viewportActive()
                         ? VkExtent2D{m_viewportWidth, m_viewportHeight}
                         : VkExtent2D{0, 0});
    // SSAO 的 R8 图重建 → binding 14 里的 resultView 句柄已换，必须刷新
    writeGlobalDescriptors();
    createViewportTextures();

    VK_LOG_INFO("MSAA sample count -> %ux", static_cast<unsigned>(m_sampleCount));
}

// ---------------------------------------------------------------- shader 热重载

void Renderer::watchShaderFiles() {
    // 监听名单 = createPipelines() 与 PostProcess 能重建的全部：
    //   · pbr 主族（顶点 + 8 个关键字变体）、深度预通道、栅格、天空
    //     → createPipelines() 重跑（waitIdle 后整组重建 + 变体缓存清空）
    //   · fullscreen/bloom/post/fxaa → PostProcess::recreate
    //     （rebuildPostChains，主链 + 预览链一起）
    // 不在名单里的：shadow/cluster/IBL compute（各自的 Pass 类持有管线，
    // 没有现成的重建入口 —— 需要时给对应类加一个 recreate 再注册进来）。
    m_shaderReload.watch("pbr.vert.spv");
    if (m_instancingEnabled) m_shaderReload.watch("pbr.inst.vert.spv");
    for (uint32_t m = 0; m <= render::kKeywordAll; ++m)
        m_shaderReload.watch(render::variantShaderFile("pbr.frag", m));
    m_shaderReload.watch("depth_only.frag.spv");
    m_shaderReload.watch("grid.vert.spv");
    m_shaderReload.watch("grid.frag.spv");
    m_shaderReload.watch("sky.vert.spv");
    m_shaderReload.watch("sky.frag.spv");
    m_shaderReload.watch("fullscreen.vert.spv");
    m_shaderReload.watch("bloom_bright.frag.spv");
    m_shaderReload.watch("bloom_blur.frag.spv");
    m_shaderReload.watch("post.frag.spv");
    m_shaderReload.watch("fxaa.frag.spv");
    // 屏幕空间效果（SSAO / FrameGraph 链）：单采样 + MSAA 双形态都要听。
    // dirty 命中它们时 pollShaderReload 会重跑两个模块的 createPipelines
    //（只重建管线，图与描述符不动 —— 最轻的路径）。
    m_shaderReload.watch("ssao.frag.spv");
    m_shaderReload.watch("ssao.msaa.frag.spv");
    m_shaderReload.watch("ssao_blur.frag.spv");
    m_shaderReload.watch("ssao_blur.msaa.frag.spv");
    m_shaderReload.watch("fg_ssgi.frag.spv");
    m_shaderReload.watch("fg_ssgi.msaa.frag.spv");
    m_shaderReload.watch("fg_ssr.frag.spv");
    m_shaderReload.watch("fg_ssr.msaa.frag.spv");
    m_shaderReload.watch("fg_godrays.frag.spv");
    m_shaderReload.watch("fg_godrays.msaa.frag.spv");
}

void Renderer::pollShaderReload() {
    // 默认开：热重载是开发工作流的一部分（对齐脚本热重载"常开无开关"）。
    // 像素级回归需要画面绝对稳定时设 MYVK_SHADER_HOTRELOAD=0 —— 本项目
    // 的回归用例都不改 .spv，开着也不影响它们，但显式可关更保险。
    static const bool enabled = envBool("MYVK_SHADER_HOTRELOAD", true);
    if (!enabled) return;

    if (++m_shaderReloadTick < 15) return;  // 60fps ≈ 0.25s 一轮
    m_shaderReloadTick = 0;

    if (m_shaderReload.poll() == 0) return;
    const std::vector<std::string>& dirty = m_shaderReload.dirty();
    std::string names;
    for (size_t i = 0; i < dirty.size(); ++i) {
        if (!names.empty()) names += ", ";
        names += dirty[i];
    }

    // 重建时机：GPU 上的命令必须全部跑完 —— 旧管线/布局即将销毁。
    // 照 setSampleCount 的先例 waitIdle。变体族 init() 会清惰性缓存，
    // 下一帧 get() 按需从磁盘重读 —— 新 .spv 自然生效（SingleSpv 与
    // PerMask 两条路径都是 get 未命中时 loadSpv，都会拿到新字节码）。
    m_device->waitIdle();
    try {
        createPipelines();
        rebuildPostChains();
        // 屏幕空间效果的 .spv 变了 → 只重建管线（图/描述符/深度视图
        // 全都不动）。挂在同一个 try 里：残缺 .spv 一并走"重载失败"日志。
        bool screenspaceDirty = false;
        for (const std::string& d : dirty) {
            if (d.rfind("ssao", 0) == 0 || d.rfind("fg_", 0) == 0)
                screenspaceDirty = true;
        }
        if (screenspaceDirty) {
            if (m_ssao && m_ssao->ready()) m_ssao->createPipelines();
            if (m_frameGraph) m_frameGraph->createPipelines();
        }
    } catch (const std::exception& e) {
        // 典型来源：外部工具写 .spv 写了一半就被轮询扫到（残缺文件）。
        // 旧管线已被部分替换，继续跑可能 VK_ERROR —— 日志里明说，
        // 别让人对着花屏猜。
        VK_LOG_ERROR("SHADER-RELOAD FAILED [%s]: %s —— 旧管线已被部分替换，"
                     "若画面异常请重启编辑器或等下一次正确重载",
                     names.c_str(), e.what());
        return;
    }
    VK_LOG_INFO("SHADER-RELOAD ok files=%u [%s] 管线已重建",
                static_cast<unsigned>(dirty.size()), names.c_str());
}

// 后处理链的整组重建（热重载专用；setSampleCount 有自己的段落，不共用
// —— 那条路径不重建预览链是有意保持的既有行为，改它超出本轮范围）。
void Renderer::rebuildPostChains() {
    // 输入视图按当前接线取：链激活 → 链输出；全关 → HDR resolve。
    m_post->recreate(m_swapchain->views(), m_swapchain->extent(),
                     postInputViews(),
                     viewportActive()
                         ? VkExtent2D{m_viewportWidth, m_viewportHeight}
                         : VkExtent2D{0, 0});
    createViewportTextures();

    // 预览链是懒创建的 —— 还没打开角色窗口时压根不存在，跳过
    if (m_previewPost && m_previewHdrTarget) {
        std::vector<VkImageView> pv;
        pv.reserve(m_previewHdrTarget->framesInFlight());
        for (uint32_t i = 0; i < m_previewHdrTarget->framesInFlight(); ++i)
            pv.push_back(m_previewHdrTarget->colorView(i));
        m_previewPost->recreate(m_swapchain->views(), m_swapchain->extent(),
                                pv, VkExtent2D{m_previewWidth,
                                               m_previewHeight});
        createPreviewTextures();
    }
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
    // HdrTarget 的深度视图换了 → SSAO/FrameGraph 重建 + 重喂
    recreateScreenSpace();

    // 后处理：交换链视图 + 场景输入视图都变了 → 重建 framebuffer 与描述符
    m_post->recreate(m_swapchain->views(), ext, postInputViews(),
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

void Renderer::recordSkyBackground(VkCommandBuffer cmd) {
    if (!m_skyEnabled || !m_pipelineSky) return;
    if (!m_environment || !m_environment->valid()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_pipelineSky->get());
    // 顶点由 gl_VertexIndex 现算（一个覆盖全屏的大三角形），无顶点缓冲
    vkCmdDraw(cmd, 3, 1, 0, 0);
    ++m_stats.drawCalls;
}

void Renderer::recordShadowPasses(VkCommandBuffer cmd, scene::Scene& scene,
                                  VkDescriptorSet frameSet) {
    if (!m_shadowPass) return;
    // 关掉阴影时**仍然渲染**深度图：一来省掉"开关一变就要重分配资源"，
    // 二来保证界面上随时切回来都是热的。真正的开关在片元着色器里
    // （shadowParams.w），所以关掉时那些采样根本不发生。
    // 若想连渲染开销也省掉，把 m_shadowCascades 设成 1 也远不够 ——
    // 这里刻意不提供"完全不渲染"的分支：那会让 shadowMap 停留在
    // UNDEFINED 布局而被采样（验证层会报）。
    const uint32_t n = m_shadowPass->cascadeCount();
    for (uint32_t c = 0; c < n; ++c) {
        m_shadowPass->render(cmd, scene, frameSet, m_currentFrame, c,
                             &m_skinJointBase);
    }

    // 自检（MYVK_SHADOW_DBG=1）：每级"考虑了多少 / 实际提交了多少"。
    // 这两个数一比就是每级裁剪空间剔除的效果 —— 没有它的话，投影体的
    // 提交量会直接变成全场景 × 级联数。30 帧一条，与 SHADOW-DBG 同频。
    static const bool dbg = std::getenv("MYVK_SHADOW_DBG") != nullptr;
    static uint32_t tick = 0;
    if (dbg && (tick++ % 30) == 1) {
        uint32_t sub = 0, con = 0;
        for (uint32_t c = 0; c < n; ++c) {
            sub += m_shadowPass->submitted(c);
            con += m_shadowPass->considered(c);
        }
        VK_LOG_INFO("SHADOW-STATS cascades=%u submitted=%u considered=%u "
                    "c%d=%u/%u c%d=%u/%u c%d=%u/%u c%d=%u/%u",
                    n, sub, con, 0u, m_shadowPass->submitted(0),
                    m_shadowPass->considered(0), 1u,
                    m_shadowPass->submitted(1), m_shadowPass->considered(1),
                    2u, m_shadowPass->submitted(2),
                    m_shadowPass->considered(2), 3u,
                    m_shadowPass->submitted(3), m_shadowPass->considered(3));
    }
}

// ---------------------------------------------------------- 局部光源阴影

void Renderer::prepareLocalShadows(scene::Scene& scene,
                                   const glm::vec3& camPos) {
    (void)scene;

    const uint32_t total = render::kMaxLocalShadowLayers;
    for (uint32_t i = 0; i < total; ++i) {
        m_localShadowLayerLight[i] = -1;
        m_localShadowMatrices[i] = glm::mat4(0.0f);
        m_localShadowNear[i] = 0.0f;
        m_localShadowFar[i] = 0.0f;
    }
    m_localShadowLayersUsed = 0;

    // ---- 1) 候选：要投影的局部光源 ----
    m_localShadowCandidates.clear();
    for (uint32_t i = 0; i < m_lightScratch.size(); ++i) {
        const scene::LightInstance& li = m_lightScratch[i];
        // castsShadow 来自组件（见 ecs::PointLightComponent::castsShadow）。
        // 关掉总开关时直接不分配 —— 省掉推导矩阵与渲染的开销，
        // 片元那边因为层号是 -1 本来就什么都不做。
        if (!m_localShadowsEnabled || !li.castsShadow()) continue;
        const int type = static_cast<int>(li.cosOuterType.y + 0.5f);
        uint32_t layers = 0;
        if (type == static_cast<int>(scene::LightInstance::Type::Point)) {
            layers = render::kPointLightFaces;
        } else if (type ==
                   static_cast<int>(scene::LightInstance::Type::Spot)) {
            layers = render::kSpotLightFaces;
        } else {
            continue;  // Directional：场景级太阳走 CSM，实体级方向光不投影
        }
        const glm::vec3 p(li.positionRange);
        const glm::vec3 d = p - camPos;
        m_localShadowCandidates.push_back(
            {i, glm::dot(d, d), layers});
    }

    // ---- 2) 近的优先：层预算用完时宁可不投远处的 ----
    std::sort(m_localShadowCandidates.begin(), m_localShadowCandidates.end(),
              [](const LocalShadowCandidate& a, const LocalShadowCandidate& b) {
                  return a.distSq < b.distSq;
              });

    // ---- 3) 贪心分配连续层段 ----
    uint32_t next = 0;
    uint32_t assigned = 0;
    for (const LocalShadowCandidate& c : m_localShadowCandidates) {
        if (assigned >= m_localShadowMaxLights) break;
        if (next + c.layers > total) break;  // 预算耗尽（点光要 6 层，常常卡在这）
        glm::mat4 mats[render::kPointLightFaces];
        float nearP = 0.0f, farP = 0.0f;
        const uint32_t got = render::buildLocalLightMatrices(
            m_lightScratch[c.lightIndex], mats, render::kPointLightFaces, &nearP,
            &farP);
        if (got != c.layers) continue;  // 类型与层数不符（不该发生）
        for (uint32_t k = 0; k < got; ++k) {
            m_localShadowMatrices[next + k] = mats[k];
            m_localShadowLayerLight[next + k] = static_cast<int>(c.lightIndex);
            m_localShadowNear[next + k] = nearP;
            m_localShadowFar[next + k] = farP;
        }
        // 基准层号写回灯表 → 随 uploadLights 一起进 set0/binding2，
        // 片元侧据此算出"点光的哪个面"。
        m_lightScratch[c.lightIndex].setShadowLayer(static_cast<int>(next));
        next += got;
        ++assigned;
    }
    m_localShadowLayersUsed = next;

    // ---- 4) 写进本帧的 SSBO（逐层一条记录）----
    auto& frame = m_frames[m_currentFrame];
    auto* rec = static_cast<render::LightShadowRecord*>(frame.lightShadowsMapped);
    for (uint32_t i = 0; i < total; ++i) {
        rec[i].matrix = m_localShadowMatrices[i];
        const int light = m_localShadowLayerLight[i];
        // x/y = 该层的 near / far（片元侧用它把世界偏移换算成 NDC）
        // z = 所属光源下标（调试/dump 用），w = 该层是否有效
        rec[i].params =
            glm::vec4(m_localShadowNear[i], m_localShadowFar[i],
                      static_cast<float>(light), light >= 0 ? 1.0f : 0.0f);
    }

    // ---- 自检（MYVK_SHADOW_DBG=1）----
    // 层分配是"看着没问题其实全给了同一盏灯"的高发区，所以把候选数、
    // 实际分配数、用掉的层数打成一行。
    static const bool dbg = std::getenv("MYVK_SHADOW_DBG") != nullptr;
    static uint32_t tick = 0;
    if (dbg && (tick++ % 30) == 1) {
        VK_LOG_INFO("LOCAL-SHADOW-DBG candidates=%u assigned=%u layers=%u/%u "
                    "maxLights=%u enabled=%d",
                    static_cast<unsigned>(m_localShadowCandidates.size()),
                    assigned, m_localShadowLayersUsed, total,
                    m_localShadowMaxLights, m_localShadowsEnabled ? 1 : 0);
    }
}

void Renderer::recordLocalShadowPasses(VkCommandBuffer cmd,
                                       scene::Scene& scene,
                                       VkDescriptorSet frameSet) {
    if (!m_localShadowPass) return;

    const uint32_t total = render::kMaxLocalShadowLayers;
    for (uint32_t layer = 0; layer < total; ++layer) {
        const bool used = m_localShadowLayerLight[layer] >= 0;
        // used == false 时传 nullptr：pass 内部只清深度（保证整张 array
        // 始终处于可采样的 READ_ONLY 布局）。**不能跳过这一步**。
        m_localShadowPass->render(cmd, scene, frameSet, m_currentFrame, layer,
                                  used ? &m_localShadowMatrices[layer] : nullptr,
                                  &m_skinJointBase);
    }

    static const bool dbg = std::getenv("MYVK_SHADOW_DBG") != nullptr;
    static uint32_t tick = 0;
    if (dbg && (tick++ % 30) == 1) {
        uint32_t sub = 0, con = 0;
        for (uint32_t l = 0; l < total; ++l) {
            sub += m_localShadowPass->submitted(l);
            con += m_localShadowPass->considered(l);
        }
        VK_LOG_INFO("LOCAL-SHADOW-STATS layers=%u used=%u submitted=%u "
                    "considered=%u",
                    total, m_localShadowLayersUsed, sub, con);
    }
}

// "当前没有绑定任何前向变体管线"的哨兵见文件开头（kBindsNothing）。

render::KeywordMask
Renderer::effectiveKeywords(const assets::Material& mat) const {
    if (m_forcedKeywordMask >= 0) {
        return static_cast<render::KeywordMask>(m_forcedKeywordMask) &
               render::kKeywordAll;
    }
    return render::keywordsOf(mat);
}

void Renderer::bindForwardVariant(VkCommandBuffer cmd, render::KeywordMask kw,
                                  render::KeywordMask& bound,
                                  uint32_t& switches) {
    if (kw == bound) return;
    bound = kw;
    ++switches;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_forwardPipelines.get(kw).get());
}

void Renderer::drawMesh(VkCommandBuffer cmd, VkPipelineLayout layout,
                        const glm::mat4& model, const assets::Material& mat,
                        const assets::Mesh& mesh, uint32_t instanceCount,
                        uint32_t firstInstance) {
    PushConstants push{};
    // instancing 合批时 VS 从实例 SSBO 按 gl_InstanceIndex 取 model，
    // 这里的 push.model 闲置（仍照推 —— push 布局必须恒定，见
    // createPipelines 里栅格管线那段"布局兼容"的说明）。
    push.model = model;
    push.baseColorFactor = mat.baseColorFactor;
    push.pbr = glm::vec4(mat.metallic, mat.roughness, mat.normalScale, mat.ao);
    push.emissive = glm::vec4(mat.emissive, 1.0f);
    // 蒙皮定位：查 mesh 是否属于某个蒙皮模型。jointBase<0 = 静态网格，
    // 着色器按 weights 和 == 0 走原路径（skin.z 只是防御性冗余）。
    const auto it = m_skinJointBase.find(&mesh);
    if (it != m_skinJointBase.end() && it->second >= 0) {
        push.skin = glm::vec4(static_cast<float>(it->second), 0.0f, 1.0f, 0.0f);
    } else {
        push.skin = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
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

    mesh.drawInstanced(cmd, instanceCount, firstInstance);
}

void Renderer::updateSkinning(scene::Scene& scene, bool resetOffset) {
    auto& frame = m_frames[m_currentFrame];
    if (resetOffset) {
        m_skinJointBase.clear();
        m_skinJointOffset = 0;
    }

    // 收集所有骨骼网格体，算关节矩阵，写入 SSBO。
    // jointBase 顺序累加；每个模型的 mesh 都映射到它的 jointBase。
    uint32_t offset = m_skinJointOffset;
    std::vector<glm::mat4> scratch;

    scene.world().each<ecs::SkinnedMeshComponent>(
        [&](ecs::Entity e, ecs::SkinnedMeshComponent& sk) {
            if (!sk.model || !sk.model->skeleton) return;
            const assets::Skeleton& skel = *sk.model->skeleton;
            const int n = skel.jointCount();
            if (n <= 0) return;

            // 采样：clipName 空 → 静止姿势；否则找 clip 采样
            const assets::AnimationClip* clip =
                sk.clipName.empty() ? nullptr : skel.findClip(sk.clipName);
            skel.sample(clip, sk.time, scratch);

            // 容量不足则重建 buffer（稀有路径）
            if (offset + static_cast<uint32_t>(n) > frame.jointCapacity) {
                const uint32_t newCap =
                    std::max(frame.jointCapacity * 2,
                             offset + static_cast<uint32_t>(n) + 64);
                frame.jointMatrices = std::make_unique<rhi::Buffer>(
                    *m_device, newCap * sizeof(glm::mat4),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                frame.jointMapped = frame.jointMatrices->map();
                frame.jointCapacity = newCap;
                // 描述符要重写（buffer 换了）
                writeGlobalDescriptors();
            }

            std::memcpy(static_cast<glm::mat4*>(frame.jointMapped) + offset,
                        scratch.data(), n * sizeof(glm::mat4));

            // 记录 jointBase + 把该模型所有 mesh 映射过去
            sk.jointBase = static_cast<int>(offset);
            for (const auto& sub : sk.model->subMeshes)
                if (sub.mesh) m_skinJointBase[sub.mesh] = sk.jointBase;

            offset += static_cast<uint32_t>(n);
        });

    m_skinJointOffset = offset;
}

// 深度预通道的几何提交：只画**不透明**几何（透明物体若参与预通道会把
// 后面本该被混合出来的东西挡掉；灯小球也不参与，它不该遮挡任何东西）。
// 遍历的是 buildDrawLists 已经过视锥剔除的列表 —— 主场景与预览场景共用。
void Renderer::recordOpaqueDepthGeometry(VkCommandBuffer cmd) {
    // 深度预通道只有 DOUBLE_SIDED 一个关键字有意义（占位：ALPHA_BLEND /
    // NORMAL_MAP 对只写深度的管线没有影响，见 createPipelines 里的说明）。
    // 掩码不变就不重绑；kBindsNothing 是无效哨兵（掩码只有 0..7）。
    KeywordMask boundMask = kBindsNothing;

    if (m_instancingEnabled) {
        // 合批路径：与前向 Pass 遍历**同一个** m_opaqueBatches ——
        // 槽序一致，实例 SSBO 只需写一份。空材质的批也要画（预通道
        // 允许空材质，前向才跳过它们）。
        for (const DrawBatch& b : m_opaqueBatches) {
            const KeywordMask kw = b.keywords;
            if (kw != boundMask) {
                boundMask = kw;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_depthPrePipelines.get(kw).get());
            }
            PushConstants push{};
            // model 由实例 SSBO 提供（pbr.inst.vert 不读 push.model）；
            // skin 照旧 —— jointBase 挂在 mesh 上，批内自然一致。
            const auto jt = m_skinJointBase.find(b.mesh);
            if (jt != m_skinJointBase.end() && jt->second >= 0) {
                push.skin = glm::vec4(static_cast<float>(jt->second), 0.0f,
                                      1.0f, 0.0f);
            }
            vkCmdPushConstants(cmd, m_depthPrePipelines.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(PushConstants), &push);
            b.mesh->drawInstanced(cmd, b.count, b.firstInstance);
        }
        return;
    }

    for (const DrawItem& it : m_opaqueDraws) {
        const KeywordMask kw = it.keywords;
        if (kw != boundMask) {
            boundMask = kw;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_depthPrePipelines.get(kw).get());
        }
        PushConstants push{};
        push.model = it.model;
        // push.skin 必须和前向路径一致：蒙皮顶点在着色器里是按顶点属性
        // 的 weights 之和判分支的，push.skin.x 只负责把该网格的关节矩阵
        // 起点（jointBase）告诉着色器。这里不填的话，多个骨骼网格体同场
        // 时预通道会用错关节 → 深度与前向对不上。
        const auto jt = m_skinJointBase.find(it.mesh);
        if (jt != m_skinJointBase.end() && jt->second >= 0) {
            push.skin = glm::vec4(static_cast<float>(jt->second), 0.0f, 1.0f,
                                  0.0f);
        }
        vkCmdPushConstants(cmd, m_depthPrePipelines.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants),
                           &push);
        it.mesh->draw(cmd);
    }
}

void Renderer::recordDepthPrePass(VkCommandBuffer cmd, scene::Scene& scene) {
    // 注意：这个 RenderPass **无条件**执行，即使预通道被关掉。
    // 前向通道的深度附件是 LOAD 语义（复用这里的产物），深度内容完全
    // 由这里产生。关掉预通道只意味着"这里不画几何"——前向通道退回自己
    // 做深度测试、失去 early-Z —— 但**清深度这一步不能省**，否则前向
    // 通道会拿上一帧残留的深度做遮挡测试，画面直接废掉。
    // 变体族是惰性创建的，"有没有建过管线"不再能当就绪判据
    // （原来判 m_pipelineDepthPrePass 是否为空）。改成问族对象自己。
    if (!m_depthPrePipelines.initialized()) return;

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
        // 几何统一走本帧绘制列表（已经过视锥剔除 + 世界矩阵已缓存）
        const VkDescriptorSet globalSet = m_frames[m_currentFrame].globalUBOSet;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_depthPrePipelines.layout(), 0, 1,
                                &globalSet, 0, nullptr);
        recordOpaqueDepthGeometry(cmd);
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

    // ================= Pass 1：级联阴影贴图（depth-only × N 级）=========
    // 必须排在主 Pass 之前：主 Pass 要采样它产出的深度图。
    // 每级一次独立的 render pass，framebuffer 挂的是该级的单独一层。
    recordShadowPasses(cmd, scene, frame.globalUBOSet);

    // ========= Pass 1.5：局部光源阴影贴图（depth-only × 12 层）=========
    // 未使用的层只清深度（见 renderLocalShadowPasses）。同样必须先于
    // 主 Pass —— 前向的片元要采样它。
    recordLocalShadowPasses(cmd, scene, frame.globalUBOSet);

    // ================= Pass 2：深度预通道（depth-only，MSAA）=========
    // 不透明几何先铺一遍深度，主 Pass 靠它做 early-Z。
    // 分簇的 compute 不能出现在 RenderPass 内部，所以这里必须收尾。
    recordDepthPrePass(cmd, scene);

    // ========= Pass 2.5：SSAO（读 MSAA 深度 → R8 遮蔽图 → 去噪）=====
    // 关闭时一个 pass 都不录：binding 14 指向缺省白图，pbr.frag 的
    // 环境光乘法恒 1.0，基线逐位不变。
    if (m_screenSpace.ssao && m_ssao && m_ssao->ready()) {
        m_ssao->render(cmd, m_currentFrame, m_screenSpace, m_invProj,
                       m_projCoefs, m_camNear, m_camFar);
    }

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
                            m_forwardPipelines.layout(), 0, 1,
                            &frame.globalUBOSet, 0, nullptr);

    // ---------------- 天空盒背景 ----------------
    // 排在最前：它铺满整屏，任何几何都该盖住它。它自己不测深度、
    // 不写深度，所以深度预通道铺好的结果原封不动留给后面。
    recordSkyBackground(cmd);

    // ---------------- 不透明 ----------------
    // 遍历本帧绘制列表（已剔除视锥外的物体，世界矩阵也已缓存）。
    // 按关键字掩码取变体管线；掩码没变就不重绑 —— 同一个材质的相邻
    // 物体（场景里最常见的情况）只付一次 vkCmdBindPipeline。
    KeywordMask boundMask = kBindsNothing;
    if (m_instancingEnabled) {
        // 合批路径：同 (掩码, 材质, 网格) 的一批只付一次状态切换，
        // vkCmdDrawIndexedInstanced 展开 —— 每实例 model 从 SSBO 读。
        for (const DrawBatch& b : m_opaqueBatches) {
            if (!b.material) continue;  // 空材质只有深度预通道会画
            bindForwardVariant(cmd, b.keywords, boundMask,
                               m_stats.variantSwitches);
            drawMesh(cmd, m_forwardPipelines.layout(), glm::mat4{1.0f},
                     *b.material, *b.mesh, b.count, b.firstInstance);
        }
    } else {
        for (const DrawItem& it : m_opaqueDraws) {
            if (!it.material) continue;  // 空材质只有深度预通道会画
            bindForwardVariant(cmd, it.keywords, boundMask,
                               m_stats.variantSwitches);
            drawMesh(cmd, m_forwardPipelines.layout(), it.model, *it.material,
                     *it.mesh);
        }
    }

    // ---------------- 灯光可视化 ----------------
    // 每盏灯画一颗自发光小球：颜色 = 灯色 × 强度。既是调试手段，
    // 也顺便证明 HDR + Bloom 在新管线里依然工作（小球会晕开）。
    if (m_showLightGizmos && m_lightGizmoMesh && !m_lightScratch.empty()) {
        assets::Material gizmoMat;
        gizmoMat.baseColorFactor = glm::vec4(0.0f);
        gizmoMat.metallic = 0.0f;
        gizmoMat.roughness = 1.0f;
        gizmoMat.normalScale = 0.0f;
        gizmoMat.ao = 1.0f;
        // 小球材质是临时构造的：自发光、不透明、不双面、没法线图 →
        // 常态就是掩码 0。走 effectiveKeywords 而不是直接写 0，是为了
        // 让"强制掩码"的对照实验也能一致地管到它。
        const KeywordMask gizmoKw = effectiveKeywords(gizmoMat);

        for (const auto& L : m_lightScratch) {
            const glm::vec3 p(L.positionRange);
            // 半径随影响范围稍作变化，便于一眼看出灯的强弱
            const float r =
                m_lightGizmoScale * (0.6f + 0.4f * L.positionRange.w / 8.0f);

            // 视锥剔除：灯可能远在相机背后（室内场景外还有灯很常见），
            // 小球没必要提交。灯天然就是"中心 + 半径"，用球体测试即可。
            if (m_frustumCullingEnabled && m_sceneFrustumValid &&
                !m_sceneFrustum.intersectsSphere(p, r)) {
                ++m_stats.gizmosCulled;
                continue;
            }

            glm::mat4 model = glm::translate(glm::mat4(1.0f), p);
            model = glm::scale(model, glm::vec3(r));

            // 让小球自己发光；强度过高会过曝成一团白，这里压到 0.35
            const glm::vec3 c = glm::vec3(L.colorIntensity) *
                                (L.colorIntensity.a * 0.35f);
            gizmoMat.emissive = glm::clamp(c, glm::vec3(0.0f),
                                           glm::vec3(6.0f));

            bindForwardVariant(cmd, gizmoKw, boundMask,
                               m_stats.variantSwitches);
            drawMesh(cmd, m_forwardPipelines.layout(), model, gizmoMat,
                     *m_lightGizmoMesh);
            ++m_stats.drawCalls;
            m_stats.triangles += static_cast<uint32_t>(
                m_lightGizmoMesh->indices().size() / 3);
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

    // ⚠ 栅格管线刚把管线绑定改掉了 → 变体绑定的缓存作废。
    // 不重置的话，第一个透明物体若掩码恰好与"栅格之前绑的那个"相同，
    // 就会跳过重绑，直接拿栅格管线去画几何（栅格没有顶点缓冲，
    // 结果是一堆乱画的三角形）。这是"缓存绑定状态"最容易踩的坑。
    boundMask = kBindsNothing;

    // ---------------- 透明（按到相机距离降序）----------------
    // 同一 subpass 内图元按提交顺序光栅化，所以绘制列表的排序顺序就是混合
    // 顺序（列表在 buildDrawLists 里已按距离降序排好，也顺带做过视锥剔除）。
    // 不排序的话，近处的透明物先画就会被远处的"盖"上一层。
    if (!m_transparentDraws.empty()) {
        for (const DrawItem& it : m_transparentDraws) {
            bindForwardVariant(cmd, it.keywords, boundMask,
                               m_stats.variantSwitches);
            drawMesh(cmd, m_forwardPipelines.layout(), it.model,
                     *it.material, *it.mesh);
        }
    }

    vkCmdEndRenderPass(cmd);

    // ========= Pass 4.5：FrameGraph 链（SSGI → SSR → God rays）======
    // 全关时零 pass —— PostProcess 直接采样 HDR，逐位回到基线。
    // 太阳投影（god rays 的光柱源）：把方向光沿 -lightDir 推远成世界
    // 空间一个点再投到屏幕。投影 w>0（相机前方）且 uv 在放宽的屏内
    // （[-0.15, 1.15]，光柱中心允许在屏外一点）才算可见。
    if (render::FrameGraph::chainActive(m_screenSpace) && m_frameGraph) {
        const glm::vec3 camPos = scene.camera().position();
        const glm::vec3 ld = glm::normalize(scene.light().direction);
        const glm::vec4 clip =
            m_viewProj * glm::vec4(camPos - ld * 500.0f, 1.0f);

        render::FrameGraphInput in{};
        in.invProj = m_invProj;
        in.projCoefs = m_projCoefs;
        in.camera = glm::vec4(m_camNear, m_camFar, 0.0f, 0.0f);
        in.sunColor = glm::vec4(glm::vec3(scene.light().color) *
                                    scene.light().intensity,
                                1.0f);
        if (clip.w > 0.0f) {
            const glm::vec2 uv = glm::vec2(clip.x, clip.y) / clip.w * 0.5f +
                                 0.5f;
            if (uv.x > -0.15f && uv.x < 1.15f && uv.y > -0.15f &&
                uv.y < 1.15f) {
                in.sunUv = uv;
                in.sunVisible = 1.0f;
            }
        }
        // 自动化验证钩子：编辑器的轨道相机重摆让"把太阳摆进画面"在
        // 脚本侧很难稳定复现 —— 允许从环境变量直接指定太阳屏幕坐标
        // （与 MYVK_TILE_SIZE 等钩子同一惯例，正常使用不设即无影响）。
        if (const char* s = std::getenv("MYVK_FG_SUN_UV")) {
            float u = 0.0f, v = 0.0f;
            if (std::sscanf(s, "%f,%f", &u, &v) == 2) {
                in.sunUv = glm::vec2(u, v);
                in.sunVisible = 1.0f;
            }
        }
        m_frameGraph->execute(cmd, m_currentFrame,
                              m_hdrTarget->colorView(m_currentFrame),
                              m_screenSpace, in);

        // 诊断（MYVK_FG_DBG）：太阳投影结果。godrays 没画出来时先看这行
        static const bool sunDbg = std::getenv("MYVK_FG_DBG") != nullptr;
        static uint32_t sunTick = 0;
        if (sunDbg && (sunTick++ % 30) == 1) {
            const glm::vec3 cp = scene.camera().position();
            VK_LOG_INFO("FG-SUN visible=%.0f uv=(%.3f,%.3f) cam=(%.1f,%.1f,"
                        "%.1f) clip=(%.1f,%.1f,%.1f,%.1f)",
                        in.sunVisible, static_cast<double>(in.sunUv.x),
                        static_cast<double>(in.sunUv.y),
                        static_cast<double>(cp.x), static_cast<double>(cp.y),
                        static_cast<double>(cp.z),
                        static_cast<double>(clip.x), static_cast<double>(clip.y),
                        static_cast<double>(clip.z), static_cast<double>(clip.w));
        }
    }

    // ---------------- Shader 变体自检（MYVK_SHADER_VARIANTS=1）----------------
    // 位置必须在**前向 Pass 之后**：buildDrawLists 只算出"各变体该画几个"，
    // 真正的管线切换次数是刚才那个循环里累加的。放在 buildDrawLists 里读
    // 会读到 m_stats 刚清零的 0（踩过）。
    //
    // 格式刻意做成"key=value + 逗号分隔"，验证脚本直接正则截取：
    //   第 1 条 = 数字（建了几个变体 / 各画几个 / 切换几次 / 强制掩码）
    //   第 2 条 = 掩码到关键字名字的映射（读日志的人不用翻头文件）
    // 每 30 帧一条，和 CULL-STATS / LOD-STATS 同频。
    static const bool varDbg =
        std::getenv("MYVK_SHADER_VARIANTS") != nullptr;
    static uint32_t varTick = 0;
    if (varDbg && (varTick++ % 30) == 1) {
        const std::string draws =
            variantTable(m_stats.variantCounts, 1u << kKeywordCount);
        VK_LOG_INFO("SHADER-VARIANTS fwd_created=%u depth_created=%u "
                    "switches=%u forced=%d draws=%s",
                    m_forwardPipelines.created(),
                    m_depthPrePipelines.created(), m_stats.variantSwitches,
                    m_forcedKeywordMask, draws.c_str());
        VK_LOG_INFO("SHADER-VARIANTS-BUILT fwd %s",
                    m_forwardPipelines.createdList().c_str());
        VK_LOG_INFO("SHADER-VARIANTS-BUILT depth %s",
                    m_depthPrePipelines.createdList().c_str());
    }

    // ============ 角色详情窗口预览场景（同一条命令缓冲内）============
    // **必须排在 ImGui 之前**：ImGui 的 UI 数据里已经引用了预览离屏图
    // （ImGui::Image 贴图），预览渲染若在其后，ImGui 采样时那张图还在
    // UNDEFINED 布局（验证层报 VUID-09600）。先渲染 → 图转到
    // SHADER_READ_ONLY → ImGui 再采样。
    recordPreview(cmd);

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

    // ---- 屏幕空间效果接线检测 ----
    // 每帧只比对开关位，变化才付代价：
    //   · FrameGraph 链激活翻转 → PostProcess 的输入视图换源（链输出
    //     或 HDR 本尊），必须 waitIdle 后重建它的 framebuffer/描述符；
    //   · SSAO 开关翻转 → binding 14 从白图换到遮蔽图（或反之），
    //     重写 set 0 即可，无资源重建。
    const bool chainWanted = render::FrameGraph::chainActive(m_screenSpace);
    if (chainWanted != m_chainWired) {
        m_chainWired = chainWanted;
        rewirePostInput();
    }
    if (m_screenSpace.ssao != m_ssaoWired) {
        m_ssaoWired = m_screenSpace.ssao;
        writeGlobalDescriptors();
    }

    // ---- shader 热重载：外部构建器更新了 .spv → 帧边界重建管线 ----
    // 放在 fence 等待之前：waitIdle 保证重建时 GPU 上没有任何命令在跑。
    pollShaderReload();

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

    // ---- 本帧渲染统计清零（buildDrawLists 会重新累积）----
    m_stats = RenderStats{};

    // ---- 场景状态上传 ----
    // 阴影正交范围 / 局部灯 / FrameUBO（相机与光照）/ **相机视锥 + 本帧
    // 绘制列表**（物体级视锥剔除就在这里发生）/ 蒙皮。
    // 主场景与角色窗口的预览场景共用这一条路径，区别只有渲染分辨率与
    // resetSkinning（true = 主场景：重置骨骼 SSBO 偏移，并累积剔除统计）。
    uploadSceneState(scene, renderExtent(), true);

    // 录制命令
    recordFrame(frame.commandBuffer, imageIndex, scene);

    // ---- 剔除统计（自动化验证用；MYVK_FRUSTUM_DBG=1 打开）----
    // 放在这里而不是 buildDrawLists 里：灯小球的剔除发生在 recordFrame
    // 内部，只有到这里本帧的统计才完整。
    {
        static const bool dbg = std::getenv("MYVK_FRUSTUM_DBG") != nullptr;
        static uint32_t tick = 0;
        if (dbg && (tick++ % 30) == 1) {
            const scene::Bvh& bvh = scene.spatialIndex();
            const scene::BvhAabb rb = bvh.rootBounds();
            VK_LOG_INFO(
                "CULL-STATS renderables=%u culled=%u drawnOpaque=%u "
                "drawnTransparent=%u gizmosCulled=%u drawCalls=%u tris=%u "
                "enabled=%d nodes=%llu depth=%llu visits=%u "
                "root=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) "
                "lod=%u/%u/%u/%u lodEnts=%u noLod=%u lodSw=%u",
                m_stats.renderables, m_stats.culledFrustum,
                m_stats.drawnOpaque, m_stats.drawnTransparent,
                m_stats.gizmosCulled, m_stats.drawCalls, m_stats.triangles,
                m_frustumCullingEnabled ? 1 : 0,
                static_cast<unsigned long long>(bvh.nodeCount()),
                static_cast<unsigned long long>(bvh.depth()),
                bvh.lastQueryNodeVisits(), static_cast<double>(rb.min.x),
                static_cast<double>(rb.min.y), static_cast<double>(rb.min.z),
                static_cast<double>(rb.max.x), static_cast<double>(rb.max.y),
                static_cast<double>(rb.max.z),
                m_stats.lodLevelCounts[0], m_stats.lodLevelCounts[1],
                m_stats.lodLevelCounts[2], m_stats.lodLevelCounts[3],
                m_stats.lodEntities, m_stats.trianglesNoLod,
                m_stats.lodSwitches);
        }
    }

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
