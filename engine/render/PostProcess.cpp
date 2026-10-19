#include "render/PostProcess.h"

#include "render/OffscreenImage.h"
#include "render/ShaderPath.h"

#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/ComputePipeline.h"
#include "rhi/Device.h"
#include "rhi/Pipeline.h"
#include "rhi/DescriptorSet.h"

#include <glm/glm.hpp>
#include <glm/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace render {

namespace {

// 编辑器背景下交换链的清屏色（线性值；交换链是 SRGB，
// 写出去会被硬件编码成 sRGB，0.02 → 约 0.16，是很稳的深灰）
constexpr float kUiClear[4] = {0.020f, 0.022f, 0.028f, 1.0f};

// 创建 32³ 的 3D 颜色 LUT 图（仅 SAMPLED + TRANSFER_DST，布局迁移由调用方做）。
// 不用 rhi::Image —— 那是 2D-only 封装。
inline void createLutImage3D(rhi::Device& device, uint32_t size, VkFormat format,
                             VkImage& image, VkDeviceMemory& memory,
                             VkImageView& view) {
    VkDevice dev = device.get();

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = format;
    ci.extent = {size, size, size};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, nullptr, &image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &memory));
    VK_CHECK(vkBindImageMemory(dev, image, memory, 0));

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &view));
}

// 自动曝光三步共用的推常量（32B，各步的字段含义见各自着色器的注释）：
//   降采样：v[0..1] = 1/亮度图宽高 ; v[4] = 每纹素每轴采样数
//           v[5] = log2 下限 ; v[6] = log2 上限
//   直方图：v[0] = 图宽 ; v[1] = 图高 ; v[2] = 箱数 ; v[4..5] = log2 范围
//   归约　：v[0] = 箱数 ; v[1] = log2 下限 ; v[2] = log2 上限
struct AePush {
    float v[8];
};
static_assert(sizeof(AePush) == 32, "AePush 必须与 GLSL 的 push_constant 一致");

} // namespace

// 由"输出分辨率"与缩放比算出"场景渲染分辨率"（R1-4 DRS）。
// RenderTarget 建 HdrTarget 时与 PostProcess 内部都用它 —— 必须逐位一致。
VkExtent2D scaledRenderExtent(VkExtent2D out, float scale) {
    // 只缩不放：>1 夹到 1（放大渲染没有意义，只会白白变慢）。
    if (!(scale > 0.0f)) scale = 1.0f;
    if (scale > 1.0f) scale = 1.0f;
    if (scale < 0.25f) scale = 0.25f;

    uint32_t w = static_cast<uint32_t>(
        std::lround(static_cast<double>(out.width) * scale));
    uint32_t h = static_cast<uint32_t>(
        std::lround(static_cast<double>(out.height) * scale));

    // 下限 16（避免极小图 / 除零），上限不超过输出。
    if (w < 16u) w = 16u;
    if (h < 16u) h = 16u;
    if (out.width >= 16u && w > out.width) w = out.width;
    if (out.height >= 16u && h > out.height) h = out.height;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    return {w, h};
}

PostProcess::PostProcess(rhi::Device& device, VkFormat swapchainFormat,
                         VkFormat hdrFormat, uint32_t framesInFlight)
    : m_device(device), m_swapFormat(swapchainFormat), m_hdrFormat(hdrFormat),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    createSetLayoutAndSampler();
    createRenderPasses();
    createPipelines();
    // 自动曝光的资源尺寸固定，与场景分辨率无关 → 构造函数里一次建好
    createAeResources();
    // 色彩分级 LUT（32³）同理：固定尺寸，构造时建好，参数变化时才重烘
    createLutResources();
}

PostProcess::~PostProcess() {
    m_device.waitIdle();
    destroyDescriptors();
    destroyBloom();
    destroySwapFramebuffers();
    destroySceneOutputFrames();
    destroyFxaaFrames();
    destroyAeResources();
    destroyLutResources();
    m_uploadPool.reset();

    VkDevice dev = m_device.get();
    m_avgPipeline.reset();
    m_histPipeline.reset();
    m_lumPipeline.reset();
    m_fxaaOffscreenPipeline.reset();
    m_fxaaPipeline.reset();
    m_postOffscreenPipeline.reset();
    m_postPipeline.reset();
    m_blurPipeline.reset();
    m_brightPipeline.reset();

    if (m_aeSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_aeSetLayout, nullptr);
        m_aeSetLayout = VK_NULL_HANDLE;
    }
    if (m_lumRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_lumRenderPass, nullptr);
        m_lumRenderPass = VK_NULL_HANDLE;
    }
    if (m_offscreenRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_offscreenRenderPass, nullptr);
        m_offscreenRenderPass = VK_NULL_HANDLE;
    }
    if (m_bloomRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_bloomRenderPass, nullptr);
        m_bloomRenderPass = VK_NULL_HANDLE;
    }
    if (m_postRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_postRenderPass, nullptr);
        m_postRenderPass = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------- 资源创建

void PostProcess::createSetLayoutAndSampler() {
    // 后处理所有 Pass 的输入都是"一张图 + 一个线性采样器"
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    m_setLayout = rhi::makeSetLayout(m_device, {binding});

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.compareEnable = VK_FALSE;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_sampler));

    // ---- 自动曝光测量链的 set layout（全 COMPUTE 阶段）----
    //   0 = 64×64 块平均 log2 亮度图（histogram.comp 采样）
    //   1 = 128 箱直方图（histogram.comp 写 / average.comp 读）
    //   2 = 结果缓冲（average.comp 写 / CPU 回读）
    {
        VkDescriptorSetLayoutBinding b[3]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[2].binding = 2;
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[2].descriptorCount = 1;
        b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        m_aeSetLayout = rhi::makeSetLayout(
            m_device, {b[0], b[1], b[2]});
    }
}

void PostProcess::createRenderPasses() {
    VkDevice dev = m_device.get();

    // ---- Bloom RenderPass：单颜色附件，进出都是"可采样" ----
    {
        VkAttachmentDescription color{};
        color.format = m_hdrFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // 全屏覆盖，无需保留
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &color;
        ci.subpassCount = 1;
        ci.pSubpasses = &subpass;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(dev, &ci, nullptr, &m_bloomRenderPass));
    }

    // ---- 亮度图 RenderPass（自动曝光 R1-2）----
    // 附件格式与 bloom 图相同（m_hdrFormat）→ 不必赌"R32F 能否当颜色附件"。
    // 与 bloom 的唯一实质差别是**同步语义面向 compute**：
    //   本 pass 的输入是"上一次的 compute 读"（WAR），输出要给 compute
    //   （直方图）读（RAW）。两条都写进 subpass 依赖后，整条测量链
    //   **不需要任何显式 VkImage 屏障** —— 这也正是把它做成片元 pass
    //   的原因（compute 读 HdrTarget 的图是没有依赖声明的）。
    {
        VkAttachmentDescription color{};
        color.format = m_hdrFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // 全屏覆盖
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // 每次全量覆盖 → 进 UNDEFINED（任意旧布局都合法，省掉首帧
        // "图刚创建还是 UNDEFINED"的初始化难题）
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{0,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &color;
        ci.subpassCount = 1;
        ci.pSubpasses = &subpass;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(dev, &ci, nullptr, &m_lumRenderPass));
    }

    // 交换链 Pass 与离屏 Pass 的附件描述只差 finalLayout，
    // 描述体本来就只有一处区别 → 用一个 lambda 生成两份，避免抄错。
    auto makeOutputPass = [&](VkImageLayout finalLayout, VkRenderPass& out) {
        VkAttachmentDescription color{};
        color.format = m_swapFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // 全屏覆盖，清一次最省事
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = finalLayout;

        VkAttachmentReference colorRef{0,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        if (finalLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            // 这张图下一站是被采样（ImGui 视口）→ 必须让写对片元阶段可见
            deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        } else {
            deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            deps[1].dstAccessMask = 0;
        }

        VkRenderPassCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &color;
        ci.subpassCount = 1;
        ci.pSubpasses = &subpass;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(dev, &ci, nullptr, &out));
    };

    makeOutputPass(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, m_postRenderPass);
    makeOutputPass(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   m_offscreenRenderPass);
}

std::unique_ptr<rhi::Pipeline> PostProcess::makeFullscreenPipeline(
    VkRenderPass renderPass, const char* fragSpv, uint32_t setCount,
    uint32_t pushSize) {
    rhi::PipelineDesc desc;
    desc.vertSpv = vkutil::readFile(resolveShaderPath("fullscreen.vert.spv"));
    desc.fragSpv = vkutil::readFile(resolveShaderPath(fragSpv));
    // 无顶点缓冲：顶点由 fullscreen.vert 里的 gl_VertexID 生成
    for (uint32_t i = 0; i < setCount; ++i) desc.setLayouts.push_back(m_setLayout);
    desc.pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    desc.pushConstant.offset = 0;
    desc.pushConstant.size = pushSize;
    desc.renderPass = renderPass;
    desc.colorAttachmentCount = 1;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.cullMode = VK_CULL_MODE_NONE;
    return std::make_unique<rhi::Pipeline>(m_device, desc);
}

void PostProcess::createPipelines() {
    // bloom / fxaa 的推常量是 1 个 vec4（16B）；合成（post.frag）是 4 个
    // vec4（64B：params + 相机效果 + 色彩分级 LUT + 升采样）。
    // 合成的 set 数 = 3（HDR / bloom / LUT）—— 三者共用 m_setLayout
    // （都是"一张图 + 一个采样器"的形状）。
    m_brightPipeline =
        makeFullscreenPipeline(m_bloomRenderPass, "bloom_bright.frag.spv", 1, 16);
    m_blurPipeline =
        makeFullscreenPipeline(m_bloomRenderPass, "bloom_blur.frag.spv", 1, 16);
    m_postPipeline =
        makeFullscreenPipeline(m_postRenderPass, "post.frag.spv", 3, 64);
    // 离屏目标那条管线（编辑器视口模式才用得上）
    m_postOffscreenPipeline =
        makeFullscreenPipeline(m_offscreenRenderPass, "post.frag.spv", 3, 64);
    // FXAA 两条：与合成的双管线结构对偶 —— 输出目标决定绑哪条 RenderPass。
    // fxaa.frag.spv 由 CMake 与其它 shader 一起编译（普通文件，无关键字）。
    m_fxaaPipeline =
        makeFullscreenPipeline(m_postRenderPass, "fxaa.frag.spv", 1, 16);
    m_fxaaOffscreenPipeline =
        makeFullscreenPipeline(m_offscreenRenderPass, "fxaa.frag.spv", 1, 16);

    // ---- 自动曝光测量链（R1-2）----
    // 第一步是片元降采样（挂在专用的 m_lumRenderPass 上，见其依赖说明），
    // 后两步是纯 compute。
    m_lumPipeline =
        makeFullscreenPipeline(m_lumRenderPass, "fg_lum_downsample.frag.spv", 1,
                               sizeof(AePush));
    {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.offset = 0;
        pc.size = sizeof(AePush);
        const std::vector<VkDescriptorSetLayout> sets{m_aeSetLayout};
        m_histPipeline = std::make_unique<rhi::ComputePipeline>(
            m_device, vkutil::readFile(resolveShaderPath("fg_lum_histogram.comp.spv")),
            sets, &pc);
        m_avgPipeline = std::make_unique<rhi::ComputePipeline>(
            m_device, vkutil::readFile(resolveShaderPath("fg_lum_average.comp.spv")),
            sets, &pc);
    }
}

void PostProcess::destroyBloom() {
    for (auto& b : m_bloom) {
        VkDevice dev = m_device.get();
        if (b.aFb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev, b.aFb, nullptr);
            b.aFb = VK_NULL_HANDLE;
        }
        if (b.bFb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev, b.bFb, nullptr);
            b.bFb = VK_NULL_HANDLE;
        }
        destroyImage2D(m_device, b.aImage, b.aMemory, b.aView);
        destroyImage2D(m_device, b.bImage, b.bMemory, b.bView);
    }
    m_bloom.clear();
}

void PostProcess::createBloomFrames() {
    m_bloom.resize(m_framesInFlight);

    for (auto& b : m_bloom) {
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;

        createImage2D(m_device, m_bloomExtent.width, m_bloomExtent.height,
                      m_hdrFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, b.aImage,
                      b.aMemory, b.aView);
        createImage2D(m_device, m_bloomExtent.width, m_bloomExtent.height,
                      m_hdrFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, b.bImage,
                      b.bMemory, b.bView);

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_bloomRenderPass;
        fbci.attachmentCount = 1;
        fbci.width = m_bloomExtent.width;
        fbci.height = m_bloomExtent.height;
        fbci.layers = 1;

        fbci.pAttachments = &b.aView;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr, &b.aFb));
        fbci.pAttachments = &b.bView;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr, &b.bFb));
    }
}

void PostProcess::destroySwapFramebuffers() {
    for (auto fb : m_swapFramebuffers) {
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(m_device.get(), fb, nullptr);
    }
    m_swapFramebuffers.clear();
}

void PostProcess::createSwapFramebuffers(const std::vector<VkImageView>& views) {
    m_swapFramebuffers.resize(views.size());
    for (size_t i = 0; i < views.size(); ++i) {
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_postRenderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &views[i];
        fbci.width = m_swapExtent.width;
        fbci.height = m_swapExtent.height;
        fbci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr,
                                     &m_swapFramebuffers[i]));
    }
}

void PostProcess::destroySceneOutputFrames() {
    for (auto& f : m_sceneOutput) {
        if (f.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device.get(), f.fb, nullptr);
            f.fb = VK_NULL_HANDLE;
        }
        destroyImage2D(m_device, f.image, f.memory, f.view);
    }
    m_sceneOutput.clear();
}

void PostProcess::createSceneOutputFrames() {
    m_sceneOutput.resize(m_framesInFlight);

    for (auto& f : m_sceneOutput) {
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
        // 离屏输出图是**全分辨率**的（合成 pass 已把缩放的场景升采样回来）
        createImage2D(m_device, m_outExtent.width, m_outExtent.height,
                      m_swapFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, f.image,
                      f.memory, f.view);

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_offscreenRenderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &f.view;
        fbci.width = m_outExtent.width;
        fbci.height = m_outExtent.height;
        fbci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr, &f.fb));
    }
}

void PostProcess::destroyFxaaFrames() {
    for (auto& f : m_fxaa) {
        if (f.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device.get(), f.fb, nullptr);
            f.fb = VK_NULL_HANDLE;
        }
        destroyImage2D(m_device, f.image, f.memory, f.view);
    }
    m_fxaa.clear();
}

void PostProcess::createFxaaFrames() {
    m_fxaa.resize(m_framesInFlight);

    for (auto& f : m_fxaa) {
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
        // FXAA 跑在**升采样之后**的全分辨率上（合成 → fxaa 中间图 →
        // FXAA 输出）：中间图与输出图同尺寸 = 输出分辨率。
        createImage2D(m_device, m_outExtent.width, m_outExtent.height,
                      m_swapFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, f.image,
                      f.memory, f.view);

        // 合成的目标：无论模式 A/B，fxaa 中间图都要被 FXAA pass 采样，
        // finalLayout 必须是 SHADER_READ_ONLY → 绑 m_offscreenRenderPass
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_offscreenRenderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &f.view;
        fbci.width = m_outExtent.width;
        fbci.height = m_outExtent.height;
        fbci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr, &f.fb));
    }
}

void PostProcess::destroyDescriptors() { m_descriptors.reset(); }

void PostProcess::buildDescriptorSets(
    const std::vector<VkImageView>& hdrColorViews) {
    // 输入集数量 = 帧数 × 4（HDR / bloomA / bloomB / fxaa）+ 自动曝光
    // （帧数）+ 余量。
    const uint32_t count = static_cast<uint32_t>(m_bloom.size()) * 4 + 8;
    // 第 5 个参数是 STORAGE_BUFFER 容量（自动曝光的 bins / result 用）。
    // 主池也预留 —— 那些集就是从这个池里分配的。
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        m_device, count, 4, count, count);

    m_hdrSets.resize(m_bloom.size());
    m_bloomASets.resize(m_bloom.size());
    m_bloomBSets.resize(m_bloom.size());
    m_fxaaSets.resize(m_bloom.size());

    for (size_t i = 0; i < m_bloom.size(); ++i) {
        m_hdrSets[i] = m_descriptors->allocate(m_setLayout);
        m_descriptors->writeTexture(m_hdrSets[i], 0, m_sampler,
                                    hdrColorViews[i]);

        m_bloomASets[i] = m_descriptors->allocate(m_setLayout);
        m_descriptors->writeTexture(m_bloomASets[i], 0, m_sampler,
                                    m_bloom[i].aView);

        m_bloomBSets[i] = m_descriptors->allocate(m_setLayout);
        m_descriptors->writeTexture(m_bloomBSets[i], 0, m_sampler,
                                    m_bloom[i].bView);

        if (!m_fxaa.empty()) {
            m_fxaaSets[i] = m_descriptors->allocate(m_setLayout);
            m_descriptors->writeTexture(m_fxaaSets[i], 0, m_sampler,
                                        m_fxaa[i].view);
        }
    }

    // 自动曝光的集引用的是**固定尺寸**的亮度图与缓冲（不随 resize 变），
    // 但它们必须跟着描述符池重建 —— 池一丢，旧集全失效。
    buildAeDescriptorSets();

    // 色彩分级 LUT 的集（set 2）同理：图是常驻的，但集要跟着池重建。
    m_lutSet = VK_NULL_HANDLE;
    if (m_lutView != VK_NULL_HANDLE) {
        m_lutSet = m_descriptors->allocate(m_setLayout);
        m_descriptors->writeTexture(m_lutSet, 0, m_sampler, m_lutView);
    }
}

// ---------------------------------------------------------------- 生命周期

VkImageView PostProcess::sceneOutputView(uint32_t frameIndex) const {
    if (m_sceneOutput.empty()) return VK_NULL_HANDLE;
    return m_sceneOutput[frameIndex % m_sceneOutput.size()].view;
}

void PostProcess::recreate(const std::vector<VkImageView>& swapchainViews,
                           VkExtent2D swapchainExtent,
                           const std::vector<VkImageView>& hdrColorViews,
                           VkExtent2D viewportExtent) {
    m_device.waitIdle();

    const bool wasSplit = m_splitOutput;
    const VkExtent2D oldOutExtent = m_outExtent;

    // 旧描述符集引用旧的 VkImageView，整池丢弃最省心
    destroyDescriptors();
    destroyBloom();
    destroySwapFramebuffers();

    m_swapExtent = swapchainExtent;

    m_splitOutput = viewportExtent.width > 0 && viewportExtent.height > 0;
    // 输出分辨率（全分辨率）：模式 B = 视口尺寸，模式 A = 交换链尺寸。
    m_outExtent = m_splitOutput ? viewportExtent : swapchainExtent;
    // 面板被拖到几乎没有时也保证至少 1×1，否则 createImage2D 会拿到非法 extent
    if (m_outExtent.width == 0) m_outExtent.width = 1;
    if (m_outExtent.height == 0) m_outExtent.height = 1;

    // 场景渲染分辨率 = 输出 × renderScale（DRS）。HdrTarget 那侧由
    // RenderTarget 用**同一个** scaledRenderExtent() 算 —— 两处必须一致。
    m_sceneExtent = scaledRenderExtent(m_outExtent, m_settings.renderScale);

    m_bloomExtent = {
        m_sceneExtent.width > kBloomDownscale
            ? m_sceneExtent.width / kBloomDownscale : 1,
        m_sceneExtent.height > kBloomDownscale
            ? m_sceneExtent.height / kBloomDownscale : 1,
    };

    // ---- 离屏输出图：只在"确实需要换图"时重建 ----
    // 交换链 resize（用户拉窗口）会走到这里，但编辑器视口的离屏分辨率
    // 并没有变。若跟着一起换 VkImageView，编辑器那边已经注册好的
    // ImGui 纹理就会指向被销毁的描述符集 —— 而那一帧的 ImGui 绘制数据
    // 早就构建完了，改不回来。保持图像不变 = 纹理 ID 稳定。
    // ⚠ 判据用**输出分辨率** m_outExtent（离屏图是全分辨率的）：DRS
    //   改变 m_sceneExtent 不该动到它 —— 那会把 ImTextureID 白换一次。
    const bool sceneOutputReusable =
        wasSplit == m_splitOutput && !m_sceneOutput.empty() &&
        m_sceneOutput.size() == m_framesInFlight &&
        oldOutExtent.width == m_outExtent.width &&
        oldOutExtent.height == m_outExtent.height;

    if (!sceneOutputReusable) {
        destroySceneOutputFrames();
        if (m_splitOutput) createSceneOutputFrames();
    }

    createBloomFrames();
    // fxaa 中间图没有 ImGui 纹理引用（sceneOutput 才有，要保 ID 稳定），
    // 可以直接整组换 —— 但必须先 destroy：recreate 会在 resize / 热重载 /
    // MSAA 切换时反复执行，不销毁就重建会把旧 image/memory/view/fb
    // 整组漏掉（实测 verify_collision_play 退出时报 24 个 leaked objects，
    // 就是窗口 resize 两次 × 每帧一组）。
    destroyFxaaFrames();
    createFxaaFrames();
    createSwapFramebuffers(swapchainViews);
    buildDescriptorSets(hdrColorViews);
}

// ---------------------------------------------------------------- 录制

void PostProcess::drawFullscreen(VkCommandBuffer cmd, VkRenderPass renderPass,
                                 rhi::Pipeline& pipeline, VkFramebuffer fb,
                                 VkExtent2D extent, const VkDescriptorSet* sets,
                                 uint32_t setCount, const void* push,
                                 uint32_t pushSize) {
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = renderPass;
    rp.framebuffer = fb;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.get());

    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.layout(), 0, setCount, sets, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       pushSize, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

void PostProcess::recordFullscreenPass(
    VkCommandBuffer cmd, VkFramebuffer fb, VkRenderPass rp,
    rhi::Pipeline& pipeline, VkExtent2D targetExtent,
    const VkDescriptorSet* sets, uint32_t setCount, const void* push,
    uint32_t pushSize,
    const std::function<void(VkCommandBuffer)>* overlay) {
    VkClearValue clear{};
    clear.color = {{kUiClear[0], kUiClear[1], kUiClear[2], kUiClear[3]}};

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = targetExtent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.get());

    VkViewport vp{};
    vp.width = static_cast<float>(targetExtent.width);
    vp.height = static_cast<float>(targetExtent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = targetExtent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.layout(), 0, setCount, sets, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       pushSize, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // overlay（ImGui）**必须**在这个正在进行的 RenderPass 内绘制：
    // 它的管线绑定在本 RenderPass 上，而重开一个 Pass 又会把刚合成好的
    // 画面清掉（loadOp=CLEAR）。所以只能在这里、vkCmdEndRenderPass 之前画。
    if (overlay && *overlay) (*overlay)(cmd);

    vkCmdEndRenderPass(cmd);
}

void PostProcess::recordComposite(
    VkCommandBuffer cmd, VkFramebuffer fb, VkRenderPass rp,
    rhi::Pipeline& pipeline, VkExtent2D targetExtent, uint32_t frameIndex,
    const std::function<void(VkCommandBuffer)>* overlay) {
    const uint32_t fi =
        frameIndex % static_cast<uint32_t>(m_bloom.size());

    // set 2 = 色彩分级 LUT（单张、帧间恒定；关着时仍要绑一个合法集，
    // 但着色器用 push 里的开关把它整段跳过 → 逐像素回到基线）。
    VkDescriptorSet sets[3] = {m_hdrSets[fi], m_bloomASets[fi], m_lutSet};
    // 64B 推常量：vec4 params（曝光 / 强度 / tonemap / 暗角）
    //            + vec4 camFx（色差 / 暗角幂 / 保留）
    //            + vec4 lut（缩放 / 偏置 / 开关 / 混合权重）
    //            + vec4 ups（源纹素 x,y / 升采样开关 / 锐度）
    // 曝光用 effectiveExposure()：自动曝光开着时是自适应值，
    // 关着时**精确等于** m_settings.exposure（逐像素回到基线）。
    const float post[8] = {effectiveExposure(), m_settings.bloomStrength,
                           static_cast<float>(m_settings.tonemapMode),
                           m_cameraFx.vignette,
                           m_cameraFx.chroma, m_cameraFx.vignettePower, 0.0f,
                           0.0f};
    // LUT 采样坐标：c*(N-1)/N + 1/(2N)。恒等 LUT 在仿射函数上可被硬件
    // 三线性精确重建 → 开着但参数默认时画面几乎不变（回归对照组）。
    const float lutScale =
        static_cast<float>(kLutSize - 1) / static_cast<float>(kLutSize);
    const float lutOffset = 0.5f / static_cast<float>(kLutSize);
    const float lut[4] = {lutScale, lutOffset,
                          m_settings.colorGrading ? 1.0f : 0.0f,
                          m_settings.cgLutBlend};
    // 升采样：只有 renderScale < 1 时开。源纹素用**场景渲染分辨率**的
    // 倒数（边缘感知核的步长）。scale == 1 时整段关闭 → 逐像素不变。
    const bool up = m_settings.renderScale < 0.999f;
    const float upv[4] = {
        up && m_sceneExtent.width ? 1.0f / static_cast<float>(m_sceneExtent.width)
                                  : 0.0f,
        up && m_sceneExtent.height
            ? 1.0f / static_cast<float>(m_sceneExtent.height) : 0.0f,
        up ? 1.0f : 0.0f, m_settings.drsSharpness};
    float pc[16];
    std::memcpy(pc, post, sizeof(post));
    std::memcpy(pc + 8, lut, sizeof(lut));
    std::memcpy(pc + 12, upv, sizeof(upv));
    recordFullscreenPass(cmd, fb, rp, pipeline, targetExtent, sets, 3, pc,
                         sizeof(pc), overlay);
}

void PostProcess::renderScene(
    VkCommandBuffer cmd, uint32_t imageIndex, uint32_t frameIndex,
    const std::function<void(VkCommandBuffer)>& overlay,
    const GameUiDrawFn& gameUi) {
    if (m_bloom.empty() || m_swapFramebuffers.empty()) return;

    // ---- 游戏 UI 的两个包装（顺序是契约：gameUi 先画，ImGui 后画）----
    //
    // ⚠ 为什么 gameUi 要拿到 **VkRenderPass**：游戏 UI 的管线按 render pass
    //   懒建/缓存 —— 两个输出 pass 的 subpass dependency 不同，管线**不兼容**
    //   （见 ui2/UiRenderer.h 文件头）。所以调用方必须把"当前在哪个 pass 里"
    //   传下去，否则会在模式 B 里用错管线（VUID-vkCmdDraw-renderPass-02684）。
    //
    // · bothA 给**模式 A**（交换链）：gameUi + ImGui 都画在这个 Pass 里；
    // · gameB 给**模式 B**（离屏图）：只画 gameUi，**绝不**画 ImGui ——
    //   那张图会被 ImGui 自己当纹理贴进视口，把面板画进去就是无限套娃。
    // · 没有 gameUi 时 bothPtr 就是原始 overlay 引用 → 与引入前逐位相同。
    std::function<void(VkCommandBuffer)> bothA;
    std::function<void(VkCommandBuffer)> gameB;
    if (gameUi) {
        bothA = [this, &gameUi, &overlay](VkCommandBuffer c) {
            gameUi(c, m_postRenderPass);
            if (overlay) overlay(c);
        };
        gameB = [this, &gameUi](VkCommandBuffer c) {
            gameUi(c, m_offscreenRenderPass);
        };
    }
    const std::function<void(VkCommandBuffer)>* bothPtr =
        gameUi ? &bothA : &overlay;
    const std::function<void(VkCommandBuffer)>* gameBPtr =
        gameUi ? &gameB : nullptr;

    const uint32_t fi = frameIndex % static_cast<uint32_t>(m_bloom.size());
    BloomFrame& bf = m_bloom[fi];

    // ---- 自动曝光（R1-2）：先量本帧亮度 ----
    // 位置必须在**亮部提取之前**：亮部提取的阈值判定也要乘曝光，
    // 而且它读的是同一张场景色。量完 → bins 归约 → 结果缓冲等 CPU
    // 两帧后回读（本帧的曝光用的是上一次回读的值）。
    if (m_settings.autoExposure) recordAutoExposure(cmd, fi);

    const bool bloomOk = fi < m_hdrSets.size() && fi < m_bloomASets.size() &&
                         fi < m_bloomBSets.size();

    const float texelX = 1.0f / static_cast<float>(m_bloomExtent.width);
    const float texelY = 1.0f / static_cast<float>(m_bloomExtent.height);

    if (bloomOk) {
        // 1) 亮部提取：HDR（全分辨率） → bloomA（半分辨率）
        const float bright[4] = {m_settings.bloomThreshold, m_settings.bloomKnee,
                                 effectiveExposure(), 0.0f};
        drawFullscreen(cmd, m_bloomRenderPass, *m_brightPipeline, bf.aFb,
                       m_bloomExtent, &m_hdrSets[fi], 1, bright, sizeof(bright));

        // 2) 水平模糊：bloomA → bloomB
        const float blurH[4] = {texelX, texelY, 1.0f, 0.0f};
        drawFullscreen(cmd, m_bloomRenderPass, *m_blurPipeline, bf.bFb,
                       m_bloomExtent, &m_bloomASets[fi], 1, blurH, sizeof(blurH));

        // 3) 垂直模糊：bloomB → bloomA（结果留在 bloomA）
        const float blurV[4] = {texelX, texelY, 0.0f, 1.0f};
        drawFullscreen(cmd, m_bloomRenderPass, *m_blurPipeline, bf.aFb,
                       m_bloomExtent, &m_bloomBSets[fi], 1, blurV, sizeof(blurV));
    }

    if (!bloomOk) return;

    // ---- FXAA 开启：合成 → fxaa 中间图 → FXAA → 最终目标 ----
    // 为什么插在 tonemap 之后：FXAA 的判定阈值定义在显示域（LDR），
    // HDR 值域上对比度判断完全失效（fxaa.frag 头注释详述）。
    // 为什么 UI 必须在 FXAA 之后：让 FXAA 模糊 ImGui 面板文字是不可
    // 接受的 —— 两种模式下 overlay 都在 FXAA 写出的那个 Pass 里（模式 B
    // 的 ImGui 本来就在下一个 Pass；模式 A 内嵌进 FXAA Pass）。
    if (m_settings.fxaa && fi < m_fxaaSets.size() &&
        fi < m_fxaa.size()) {
        const float texelX2 =
            1.0f / static_cast<float>(m_outExtent.width);
        const float texelY2 =
            1.0f / static_cast<float>(m_outExtent.height);
        const float fxaaPush[4] = {texelX2, texelY2, 0.0f, 0.0f};

        // 合成到 fxaa 中间图。统一走离屏管线 —— 两种模式下这张图的
        // finalLayout 都是 SHADER_READ_ONLY（下一站是 FXAA 的输入）。
        recordComposite(cmd, m_fxaa[fi].fb, m_offscreenRenderPass,
                        *m_postOffscreenPipeline, m_outExtent, frameIndex,
                        nullptr);

        if (m_splitOutput && fi < m_sceneOutput.size()) {
            // 模式 B：FXAA → 离屏图（ImGui 采样的那张），ImGui 照旧另跑。
            // 游戏 UI 画在**这个** Pass 里 —— 它写出的就是视口看到的那张图，
            // 而且排在 FXAA 之后（HUD 不该被抗锯齿糊掉）。
            recordFullscreenPass(cmd, m_sceneOutput[fi].fb,
                                 m_offscreenRenderPass,
                                 *m_fxaaOffscreenPipeline, m_outExtent,
                                 &m_fxaaSets[fi], 1, fxaaPush, sizeof(fxaaPush),
                                 gameBPtr);

            // FXAA pass 的 finalLayout 只保证"布局正确"，写入对
            // ImGui 片元采样可见还要靠这条显式屏障。
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = m_sceneOutput[fi].image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            return;
        }

        // 模式 A：FXAA 直写交换链，UI overlay 内嵌在同一 Pass 内 ——
        // ImGui 管线绑在 m_postRenderPass 上，此 Pass 附件描述一致，
        // 管线可直接用；UI 因此落在抗锯齿之后的图层上。
        recordFullscreenPass(cmd, m_swapFramebuffers[imageIndex],
                             m_postRenderPass, *m_fxaaPipeline, m_outExtent,
                             &m_fxaaSets[fi], 1, fxaaPush, sizeof(fxaaPush),
                             bothPtr);
        return;
    }

    if (m_splitOutput && fi < m_sceneOutput.size()) {
        // ---- 模式 B：合成到离屏图（不画 ImGui），ImGui 由 renderUiToSwapchain 负责 ----
        // 游戏 UI 例外：它画进离屏图本身（= 编辑器视口里能看到血条）。
        // 这就是 `ui.hudInEditor` 那个开关的落点。
        recordComposite(cmd, m_sceneOutput[fi].fb, m_offscreenRenderPass,
                        *m_postOffscreenPipeline, m_outExtent, frameIndex,
                        gameBPtr);

        // RenderPass 的 finalLayout 已把图转到 SHADER_READ_ONLY，但那只
        // 保证"布局正确"，不保证"写入对后续读取可见"。这里补一条显式内存
        // 屏障，让紧接着的 UI Pass 在片元阶段采样时能看到本 Pass 的写入。
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_sceneOutput[fi].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        return;
    }

    // ---- 模式 A：合成到交换链，UI 画在同一个 Pass 内 ----
    recordComposite(cmd, m_swapFramebuffers[imageIndex], m_postRenderPass,
                    *m_postPipeline, m_outExtent, frameIndex, bothPtr);
}

void PostProcess::renderUiToSwapchain(
    VkCommandBuffer cmd, uint32_t imageIndex,
    const std::function<void(VkCommandBuffer)>& overlay) {
    if (!overlay || m_swapFramebuffers.empty()) return;

    // 这条路径只在"场景 → 离屏图"（编辑器视口）时被调用：
    // 交换链上除了 UI 什么都没有，所以这里用 loadOp=CLEAR 的 Pass 清一次
    // 编辑器底色，再让 ImGui 画全部面板（含 Viewport 里的 3D 贴图）。
    VkClearValue clear{};
    clear.color = {{kUiClear[0], kUiClear[1], kUiClear[2], kUiClear[3]}};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_postRenderPass;
    rp.framebuffer = m_swapFramebuffers[imageIndex];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = m_swapExtent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    overlay(cmd);
    vkCmdEndRenderPass(cmd);
}

// ============================================================
// 自动曝光（R1-2）
// ============================================================

void PostProcess::createAeResources() {
    m_lum.resize(m_framesInFlight);
    m_aeBins.resize(m_framesInFlight);
    m_aeResult.resize(m_framesInFlight);
    m_aeResultMapped.assign(m_framesInFlight, nullptr);

    for (auto& f : m_lum) {
        const VkImageUsageFlags usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        createImage2D(m_device, kLumWidth, kLumHeight, m_hdrFormat, usage,
                      VK_IMAGE_ASPECT_COLOR_BIT, f.image, f.memory, f.view);

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_lumRenderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &f.view;
        fbci.width = kLumWidth;
        fbci.height = kLumHeight;
        fbci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr, &f.fb));
    }

    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        // bins：纯 GPU 随机访问（每帧 vkCmdFillBuffer 清零）
        m_aeBins[i] = std::make_unique<rhi::Buffer>(
            m_device, kAeBins * sizeof(uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        // result：compute 写、CPU 回读 → HOST_VISIBLE|COHERENT 持久映射
        m_aeResult[i] = std::make_unique<rhi::Buffer>(
            m_device, 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        float* p = static_cast<float*>(m_aeResult[i]->map());
        m_aeResultMapped[i] = p;
        if (p) std::memset(p, 0, 4 * sizeof(float));
    }
}

void PostProcess::destroyAeResources() {
    for (auto& f : m_lum) {
        if (f.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device.get(), f.fb, nullptr);
            f.fb = VK_NULL_HANDLE;
        }
        destroyImage2D(m_device, f.image, f.memory, f.view);
    }
    m_lum.clear();
    m_aeBins.clear();
    m_aeResultMapped.assign(m_aeResult.size(), nullptr);
    m_aeResult.clear();
    m_aeSets.clear();
}

void PostProcess::buildAeDescriptorSets() {
    if (m_lum.empty() || m_aeBins.empty() || m_aeResult.empty()) return;
    m_aeSets.resize(m_framesInFlight, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        m_aeSets[i] = m_descriptors->allocate(m_aeSetLayout);
        m_descriptors->writeTexture(m_aeSets[i], 0, m_sampler, m_lum[i].view);
        m_descriptors->writeBuffer(m_aeSets[i], 1, m_aeBins[i]->get(),
                                   VK_WHOLE_SIZE,
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        m_descriptors->writeBuffer(m_aeSets[i], 2, m_aeResult[i]->get(),
                                   VK_WHOLE_SIZE,
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
}

void PostProcess::recordAutoExposure(VkCommandBuffer cmd, uint32_t fi) {
    if (fi >= m_lum.size() || fi >= m_aeSets.size() || fi >= m_hdrSets.size() ||
        !m_lumPipeline || !m_histPipeline || !m_avgPipeline) {
        return;
    }

    // ---- 1) 片元降采样：场景色 → 64×64 块平均 log2 亮度 ----
    // 输入用 m_hdrSets[fi]（= PostProcess 当前采样的场景色，可能是 HDR
    // 本尊，也可能是 FrameGraph 链的输出图）—— 与合成读的是同一张。
    AePush p{};
    p.v[0] = 1.0f / static_cast<float>(kLumWidth);
    p.v[1] = 1.0f / static_cast<float>(kLumHeight);
    p.v[2] = 0.0f;
    p.v[3] = 0.0f;
    p.v[4] = static_cast<float>(kLumSamplesPerTexel);
    p.v[5] = kAeLogLo;
    p.v[6] = kAeLogHi;
    p.v[7] = 0.0f;
    drawFullscreen(cmd, m_lumRenderPass, *m_lumPipeline, m_lum[fi].fb,
                   VkExtent2D{kLumWidth, kLumHeight}, &m_hdrSets[fi], 1, p.v,
                   sizeof(p.v));

    // ---- 2) 清 bins（TRANSFER → COMPUTE）----
    vkCmdFillBuffer(cmd, m_aeBins[fi]->get(), 0, VK_WHOLE_SIZE, 0u);
    {
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = m_aeBins[fi]->get();
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 1, &b, 0, nullptr);
    }

    // ---- 3) 直方图（写 bins）----
    AePush hp{};
    hp.v[0] = static_cast<float>(kLumWidth);
    hp.v[1] = static_cast<float>(kLumHeight);
    hp.v[2] = static_cast<float>(kAeBins);
    hp.v[3] = 0.0f;
    hp.v[4] = kAeLogLo;
    hp.v[5] = kAeLogHi;
    hp.v[6] = 0.0f;
    hp.v[7] = 0.0f;
    const uint32_t gx = (kLumWidth + 15u) / 16u;
    const uint32_t gy = (kLumHeight + 15u) / 16u;
    m_histPipeline->dispatch(cmd, gx, gy, 1, &m_aeSets[fi], 1, hp.v,
                             sizeof(hp.v));

    // ---- 4) bins → 平均亮度（COMPUTE → COMPUTE）----
    {
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = m_aeBins[fi]->get();
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 1, &b, 0, nullptr);
    }
    AePush ap{};
    ap.v[0] = static_cast<float>(kAeBins);
    ap.v[1] = kAeLogLo;
    ap.v[2] = kAeLogHi;
    ap.v[3] = 0.0f;
    ap.v[4] = 0.0f;
    ap.v[5] = 0.0f;
    ap.v[6] = 0.0f;
    ap.v[7] = 0.0f;
    m_avgPipeline->dispatch(cmd, 1, 1, 1, &m_aeSets[fi], 1, ap.v, sizeof(ap.v));

    // ---- 5) 结果缓冲 → 主机可见（COMPUTE → HOST）----
    {
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = m_aeResult[fi]->get();
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &b,
                             0, nullptr);
    }
}

float PostProcess::autoExposureAvgLum() const {
    return std::exp2(m_aeAvgLog2);
}

// ---------------------------------------------------------------- 色彩分级 LUT

void PostProcess::createLutResources() {
    if (m_uploadPool) return;
    m_uploadPool = std::make_unique<rhi::CommandPool>(m_device);

    // 优先 16F（暗部无量化台阶、绝大多数设备可三线性过滤），退到 8-bit
    // UNORM（规格保证可线性过滤）保证任何设备都能跑。
    m_lutFormat = m_device.findSupportedFormat(
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (m_lutFormat == VK_FORMAT_UNDEFINED) {
        m_lutFormat = VK_FORMAT_R8G8B8A8_UNORM;  // 兜底：几乎必然成功
    }
    createLutImage3D(m_device, kLutSize, m_lutFormat, m_lutImage, m_lutMemory,
                     m_lutView);
    // **立刻烘一次**（恒等 LUT）：这样图从构造完就停在 SHADER_READ_ONLY，
    // 不依赖"某个目标是否被调用过 beginFrame" —— 每个 RenderTarget 都有自己的
    // PostProcess，只有主目标会被驱动 beginFrame；若延迟到那时再烘，副目标的
    // LUT 图会一直是 UNDEFINED，而描述符声明的是 SHADER_READ_ONLY → 验证层报错。
    snapshotGrading();   // 让重烘判定的基准 = 当前参数
    m_lutDirty = false;
    bakeLut();
}

void PostProcess::destroyLutResources() {
    VkDevice dev = m_device.get();
    m_lutSet = VK_NULL_HANDLE;   // 集随描述符池一起销毁
    if (m_lutView != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, m_lutView, nullptr);
        m_lutView = VK_NULL_HANDLE;
    }
    if (m_lutImage != VK_NULL_HANDLE) {
        vkDestroyImage(dev, m_lutImage, nullptr);
        m_lutImage = VK_NULL_HANDLE;
    }
    if (m_lutMemory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, m_lutMemory, nullptr);
        m_lutMemory = VK_NULL_HANDLE;
    }
}

// lift / gamma / gain / contrast / saturation —— 从"整体"到"分级"。
// 顺序刻意固定，且与 UE Color Grading 的直觉一致：
//   1) lift   —— 抬阴影：c = c + lift*(1-c)（lift=0 恒等）
//   2) gamma  —— 中间调：c = pow(c, 1/gamma)（gamma=1 恒等）
//   3) gain   —— 高光增益：c = c * gain（gain=1 恒等）
//   4) contrast —— 绕中灰 0.5 缩放：c = (c-0.5)*k + 0.5
//   5) saturation —— 绕亮度插值：c = mix(lum, c, s)
// 全程作用在显示域 [0,1]（与 LUT 的定义域一致）。
static glm::vec3 applyGrading(const glm::vec3& in, const float lift[3],
                              const float gamma[3], const float gain[3],
                              float contrast, float saturation) {
    glm::vec3 c = in;
    c = c + glm::vec3(lift[0], lift[1], lift[2]) *
                (glm::vec3(1.0f) - c);                       // lift
    c = glm::vec3(std::pow(std::max(c.x, 0.0f), 1.0f / std::max(gamma[0], 1e-3f)),
                  std::pow(std::max(c.y, 0.0f), 1.0f / std::max(gamma[1], 1e-3f)),
                  std::pow(std::max(c.z, 0.0f), 1.0f / std::max(gamma[2], 1e-3f)));
    c *= glm::vec3(gain[0], gain[1], gain[2]);               // gain
    c = (c - 0.5f) * contrast + 0.5f;                        // contrast
    const float lum = glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    c = glm::mix(glm::vec3(lum), c, saturation);             // saturation
    return glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
}

void PostProcess::bakeLut() {
    if (m_lutImage == VK_NULL_HANDLE) return;

    const uint32_t n = kLutSize;
    const float denom = static_cast<float>(n - 1);
    const size_t texels = static_cast<size_t>(n) * n * n;

    // 先在 CPU 侧把整张 LUT 算好（含格式打包），再一次性 staging 上传。
    // 布局：紧密排布，x 最快变（Vulkan 的 buffer→image 拷贝约定）。
    std::vector<uint8_t> bytes;
    const bool is16f = (m_lutFormat == VK_FORMAT_R16G16B16A16_SFLOAT);
    m_lutIdentity = true;
    bytes.resize(texels * (is16f ? 8u : 4u));

    size_t idx = 0;
    for (uint32_t k = 0; k < n; ++k) {
        for (uint32_t j = 0; j < n; ++j) {
            for (uint32_t i = 0; i < n; ++i, ++idx) {
                const glm::vec3 in(static_cast<float>(i) / denom,
                                   static_cast<float>(j) / denom,
                                   static_cast<float>(k) / denom);
                const glm::vec3 out = applyGrading(
                    in, m_settings.cgLift, m_settings.cgGamma, m_settings.cgGain,
                    m_settings.cgContrast, m_settings.cgSaturation);
                // 恒等判定（阈值远小于 1/255，只有真正的恒等映射才通过）
                if (std::abs(out.x - in.x) > 1e-5f ||
                    std::abs(out.y - in.y) > 1e-5f ||
                    std::abs(out.z - in.z) > 1e-5f) {
                    m_lutIdentity = false;
                }
                if (is16f) {
                    // glm 只提供 packHalf2x16(vec2)；取低 16 位即单分量 half。
                    auto half1 = [](float v) {
                        return static_cast<uint16_t>(
                            glm::packHalf2x16(glm::vec2(v, 0.0f)) & 0xFFFFu);
                    };
                    const uint16_t hx = half1(out.x);
                    const uint16_t hy = half1(out.y);
                    const uint16_t hz = half1(out.z);
                    const uint16_t hw = half1(1.0f);
                    uint16_t* p = reinterpret_cast<uint16_t*>(bytes.data()) + idx * 4;
                    p[0] = hx; p[1] = hy; p[2] = hz; p[3] = hw;
                } else {
                    uint8_t* p = bytes.data() + idx * 4;
                    p[0] = static_cast<uint8_t>(std::lround(
                        glm::clamp(out.x, 0.0f, 1.0f) * 255.0f));
                    p[1] = static_cast<uint8_t>(std::lround(
                        glm::clamp(out.y, 0.0f, 1.0f) * 255.0f));
                    p[2] = static_cast<uint8_t>(std::lround(
                        glm::clamp(out.z, 0.0f, 1.0f) * 255.0f));
                    p[3] = 255;
                }
            }
        }
    }

    // staging：HOST_VISIBLE | COHERENT → GPU
    rhi::Buffer staging(m_device, bytes.size(),
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.writeData(bytes.data(), bytes.size());

    VkCommandBuffer cmd = m_uploadPool->beginOneTime();

    // UNDEFINED → TRANSFER_DST（丢弃旧内容；每次重烘都是全量覆盖）
    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = m_lutImage;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.layerCount = 1;
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;    // 紧密排布
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {n, n, n};
    vkCmdCopyBufferToImage(cmd, staging.get(), m_lutImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST → SHADER_READ_ONLY（此后永久停在这个布局）
    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toRead);

    m_uploadPool->endAndSubmit(cmd);

    snapshotGrading();
    m_lutDirty = false;
}

bool PostProcess::gradingChanged() const {
    for (int i = 0; i < 3; ++i) {
        if (m_settings.cgLift[i] != m_cgLift[i] ||
            m_settings.cgGamma[i] != m_cgGamma[i] ||
            m_settings.cgGain[i] != m_cgGain[i]) {
            return true;
        }
    }
    return m_settings.cgContrast != m_cgContrast ||
           m_settings.cgSaturation != m_cgSaturation;
}

void PostProcess::snapshotGrading() {
    for (int i = 0; i < 3; ++i) {
        m_cgLift[i] = m_settings.cgLift[i];
        m_cgGamma[i] = m_settings.cgGamma[i];
        m_cgGain[i] = m_settings.cgGain[i];
    }
    m_cgContrast = m_settings.cgContrast;
    m_cgSaturation = m_settings.cgSaturation;
}

void PostProcess::rebakeLutIfDirty() {
    if (!m_lutDirty && !gradingChanged()) return;
    bakeLut();
}

void PostProcess::beginFrame(uint32_t frameIndex, float dt) {
    // LUT 只在调节色参数变化时重烘（尺寸小，CPU 烘 + 一次性上传，代价可忽略）。
    // 放在这里：调用点在本帧任何命令录制之前、且上一帧的栅栏已过。
    rebakeLutIfDirty();

    if (!m_settings.autoExposure) {
        m_aeWasOn = false;   // 下次开启时重新以手动值为起点
        m_aeSamples = 0.0f;  // 关着时"没有测量"（日志断言靠它）
        return;
    }

    // 上升沿：以手动曝光为起点，避免从 1.0 突跳（也让"关→开"可复现）
    if (!m_aeWasOn) {
        m_aeValue = m_settings.exposure;
        m_aeWasOn = true;
    }

    const uint32_t fi = frameIndex % m_framesInFlight;
    float avgLog2 = 0.0f;
    float samples = 0.0f;

    if (m_aeSimLum > 0.0f) {
        // 回归钩子：亮度钉死（与场景内容解耦 → 收敛曲线确定可断言）
        avgLog2 = std::log2(std::max(m_aeSimLum, 1e-5f));
        samples = 1.0f;
    } else if (fi < m_aeResultMapped.size() && m_aeResultMapped[fi]) {
        // 时序：本调用点在 in-flight 栅栏之后 → 该 slot 上一轮写的
        // 结果一定已落盘（HOST_COHERENT，无需手动 invalidate）。
        const float* r = m_aeResultMapped[fi];
        avgLog2 = r[0];
        samples = r[1];
    }

    m_aeAvgLog2 = avgLog2;
    m_aeSamples = samples;
    if (!(samples > 0.0f)) return;  // 还没有有效测量（首帧 / 刚开启）

    const float avgLum = std::exp2(avgLog2);
    float target = m_settings.exposureKey / std::max(avgLum, 1e-5f);
    target *= std::exp2(m_settings.exposureBias);  // EV 偏置
    m_aeTargetRaw = target;
    target = std::min(std::max(target, m_settings.exposureMin),
                      m_settings.exposureMax);
    m_aeTarget = target;

    // 指数平滑：alpha = 1 - exp(-speed·dt)
    //   speed ≤ 0 → 直接跟上（无适应过程）。dt 夹紧避免首帧巨大跳变。
    const float clampedDt = std::min(std::max(dt, 0.0f), 0.25f);
    float alpha = m_settings.exposureSpeed > 0.0f
                      ? 1.0f - std::exp(-m_settings.exposureSpeed * clampedDt)
                      : 1.0f;
    alpha = std::min(std::max(alpha, 0.0f), 1.0f);
    m_aeValue += (target - m_aeValue) * alpha;
    m_aeValue = std::min(std::max(m_aeValue, m_settings.exposureMin),
                         m_settings.exposureMax);
}

// ---------------------------------------------------------------- 动态分辨率（DRS，R1-4）

bool PostProcess::updateDynamicResolution(float gpuFrameMs) {
    const float prev = m_settings.renderScale;

    if (!m_settings.dynamicRes) {
        // 关：强制回到 1.0（回到基线）。这里返回 true 会让调用方重建一次
        // —— 正好把刚才被缩放过的资源复原。
        if (m_settings.renderScale != 1.0f) m_settings.renderScale = 1.0f;
        m_drsSmoothedMs = 0.0f;
        m_drsLastMs = 0.0f;
        m_drsCooldown = 0;
        return m_settings.renderScale != prev;
    }

    if (!m_settings.drsAuto || gpuFrameMs <= 0.0f) {
        // 手动模式（或时戳不可用）：只做夹紧，不动用户给的值。
        float s = std::min(std::max(m_settings.renderScale, m_settings.drsMinScale),
                           m_settings.drsMaxScale);
        m_drsLastMs = gpuFrameMs;
        if (s != m_settings.renderScale) {
            m_settings.renderScale = s;
            return true;
        }
        return false;
    }

    m_drsLastMs = gpuFrameMs;
    // 指数平滑：单帧耗时噪声很大（尤其是时戳量化），必须平滑后再决策
    m_drsSmoothedMs = (m_drsSmoothedMs <= 0.0f)
                          ? gpuFrameMs
                          : m_drsSmoothedMs + (gpuFrameMs - m_drsSmoothedMs) * 0.15f;

    const float target = std::max(m_settings.drsTargetMs, 1e-3f);
    // 像素数 ∝ scale²，时间 ∝ 像素数 ⇒ 想要 time=target 需要
    // scale' = scale · sqrt(target / measured)。
    float desired = m_settings.renderScale;
    if (m_drsSmoothedMs > 1e-3f) {
        desired *= std::sqrt(target / m_drsSmoothedMs);
    }
    desired = std::min(std::max(desired, m_settings.drsMinScale),
                       m_settings.drsMaxScale);

    if (m_drsCooldown > 0) {
        --m_drsCooldown;
        return false;
    }

    // 死区：相对差 < 3% 不动（避免在目标附近来回抖）。
    if (std::fabs(desired - m_settings.renderScale) <
        0.03f * m_settings.renderScale) {
        return false;
    }

    // 量化到 1/16 步进 —— 只有量化值真的变了才付"重建一整套图"的代价。
    float q = std::round(desired * 16.0f) / 16.0f;
    q = std::min(std::max(q, m_settings.drsMinScale), m_settings.drsMaxScale);
    if (std::fabs(q - m_settings.renderScale) < (1.0f / 32.0f)) return false;

    m_settings.renderScale = q;
    m_drsCooldown = 12;  // 至少隔 12 帧才允许再变（重建很贵）
    return true;
}

} // namespace render
