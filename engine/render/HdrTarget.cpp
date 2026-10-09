#include "render/HdrTarget.h"

#include "render/OffscreenImage.h"
#include "rhi/Device.h"

#include <algorithm>

namespace render {

namespace {

// 从设备限制里挑一个真正受支持的采样数（颜色 + 深度都要支持）。
// 桌面 Vulkan 上 4x 几乎是必然可用的，但显式查询一次比赌更划算 ——
// 采样数与附件不匹配时 vkCreateRenderPass 会直接失败。
VkSampleCountFlagBits pickSampleCount(rhi::Device& device,
                                      VkSampleCountFlagBits want) {
    const VkPhysicalDeviceLimits& lim = device.properties().limits;
    const VkSampleCountFlags common = lim.framebufferColorSampleCounts &
                                      lim.framebufferDepthSampleCounts;

    if (common & want) return want;

    // 逐级降档：8 → 4 → 2 → 1
    const VkSampleCountFlagBits ladder[] = {
        VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_4_BIT,
        VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_1_BIT};
    for (VkSampleCountFlagBits c : ladder) {
        if (common & c) return c;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

} // namespace

HdrTarget::HdrTarget(rhi::Device& device, uint32_t framesInFlight,
                     VkSampleCountFlagBits samples)
    : m_device(device) {
    m_samples = pickSampleCount(device, samples);
    createRenderPasses();
    m_frames.resize(framesInFlight > 0 ? framesInFlight : 1);
}

HdrTarget::~HdrTarget() {
    m_device.waitIdle();
    destroyAllFrames();

    VkDevice dev = m_device.get();
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    if (m_depthPrePass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_depthPrePass, nullptr);
        m_depthPrePass = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------- RenderPass

void HdrTarget::createRenderPasses() {
    VkDevice dev = m_device.get();

    m_depthFormat = m_device.findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
         VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);

    const bool msaa = msaaEnabled();

    // ---- 1) 深度预通道：只有一个多采样深度附件 ----
    {
        VkAttachmentDescription depth{};
        depth.format = m_depthFormat;
        depth.samples = m_samples;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // 必须 STORE —— 前向通道要用 LOAD 复用这份深度
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef{
            0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 0;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency deps[2]{};
        // 外部 → 子过程：等该帧此前对同一深度图的使用结束
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // 子过程 → 外部：把深度写"交出去"，供后面的前向通道读取
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &depth;
        ci.subpassCount = 1;
        ci.pSubpasses = &subpass;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(dev, &ci, nullptr, &m_depthPrePass));
    }

    // ---- 2) 前向通道 ----
    //   MSAA 开：att0 = 多采样颜色，att1 = 多采样深度，att2 = 单采样 resolve
    //   MSAA 关：att0 = 单采样颜色（直接就是 resolve 图），att1 = 深度
    VkAttachmentDescription attaches[3]{};

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    if (msaa) {
        // 多采样颜色：内容在 resolve 之后就没用了 → DONT_CARE
        attaches[0].format = kColorFormat;
        attaches[0].samples = m_samples;
        attaches[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attaches[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attaches[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attaches[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attaches[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attaches[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        // 无 MSAA：颜色附件本身就是最终可采样图 → STORE + SHADER_READ_ONLY
        attaches[0].format = kColorFormat;
        attaches[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attaches[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attaches[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attaches[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attaches[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attaches[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attaches[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // 深度：LOAD —— 直接复用深度预通道的结果，不重新清。
    // storeOp 用 DONT_CARE：这一帧之后没人再读深度
    attaches[1].format = m_depthFormat;
    attaches[1].samples = m_samples;
    attaches[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attaches[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attaches[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attaches[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attaches[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attaches[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    uint32_t attachmentCount = 2;
    if (msaa) {
        // resolve 目标：单采样、可被采样
        attaches[2].format = kColorFormat;
        attaches[2].samples = VK_SAMPLE_COUNT_1_BIT;
        attaches[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attaches[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attaches[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attaches[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attaches[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attaches[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachmentCount = 3;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pResolveAttachments = msaa ? &resolveRef : nullptr;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[2]{};
    // 外部 → 子过程：
    //   * 等前向通道上一次被 PostProcess 采样读完（resolve 图的 WAR 依赖）
    //   * 等深度预通道把深度写完（跨 RenderPass 的 RAW 依赖）
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // 子过程 → 外部：颜色（含 resolve）写完，后处理才能采样
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = attachmentCount;
    ci.pAttachments = attaches;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(dev, &ci, nullptr, &m_renderPass));
}

// ---------------------------------------------------------------- 图像

void HdrTarget::createFrame(Frame& f) {
    if (msaaEnabled()) {
        // 多采样颜色：只作为附件，永远不被采样 → 不需要 SAMPLED_BIT
        createImage2D(m_device, m_extent.width, m_extent.height, kColorFormat,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, f.msaaColorImage,
                      f.msaaColorMemory, f.msaaColorView, m_samples);
    }

    // resolve 图：既是颜色附件（MSAA 关时），也是被采样的那张
    createImage2D(m_device, m_extent.width, m_extent.height, kColorFormat,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, f.resolveImage, f.resolveMemory,
                  f.resolveView);

    createImage2D(m_device, m_extent.width, m_extent.height, m_depthFormat,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT, f.depthImage, f.depthMemory,
                  f.depthView, m_samples);

    // 深度预通道 framebuffer（只有深度）
    {
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_depthPrePass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &f.depthView;
        fbci.width = m_extent.width;
        fbci.height = m_extent.height;
        fbci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr, &f.depthFb));
    }

    // 前向通道 framebuffer
    {
        // MSAA 关时颜色附件直接就是 resolve 图
        VkImageView colors[3] = {
            msaaEnabled() ? f.msaaColorView : f.resolveView, f.depthView,
            f.resolveView};
        const uint32_t count = msaaEnabled() ? 3u : 2u;
        (void)colors;

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_renderPass;
        fbci.attachmentCount = count;
        fbci.pAttachments = colors;
        fbci.width = m_extent.width;
        fbci.height = m_extent.height;
        fbci.layers = 1;
        VK_CHECK(
            vkCreateFramebuffer(m_device.get(), &fbci, nullptr, &f.forwardFb));
    }
}

void HdrTarget::destroyFrame(Frame& f) {
    VkDevice dev = m_device.get();
    if (f.forwardFb != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(dev, f.forwardFb, nullptr);
        f.forwardFb = VK_NULL_HANDLE;
    }
    if (f.depthFb != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(dev, f.depthFb, nullptr);
        f.depthFb = VK_NULL_HANDLE;
    }
    destroyImage2D(m_device, f.depthImage, f.depthMemory, f.depthView);
    destroyImage2D(m_device, f.resolveImage, f.resolveMemory, f.resolveView);
    destroyImage2D(m_device, f.msaaColorImage, f.msaaColorMemory,
                   f.msaaColorView);
}

void HdrTarget::destroyAllFrames() {
    for (auto& f : m_frames) destroyFrame(f);
}

void HdrTarget::recreate(uint32_t width, uint32_t height) {
    m_device.waitIdle();
    destroyAllFrames();
    m_extent = {std::max(1u, width), std::max(1u, height)};

    for (auto& f : m_frames) createFrame(f);
}

} // namespace render
