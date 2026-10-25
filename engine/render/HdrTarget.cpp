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

    // ---- 1) 几何预通道：深度 + 几何缓冲（法线/线性深度）----
    //
    // 附件排布（括号里是索引）：
    //   深度 (0)  ·  几何 (1)  ·  [几何 resolve (2)，仅 MSAA]
    // clearValueCount 因此是 2：只有 (0)(1) 需要清。(2) 是 DONT_CARE 的
    // resolve 目标，规范只要求 clearValueCount 覆盖到"最高清屏附件索引"。
    {
        VkAttachmentDescription att[3]{};

        // (0) 深度
        att[0].format = m_depthFormat;
        att[0].samples = m_samples;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // 必须 STORE —— 前向通道要用 LOAD 复用这份深度
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // finalLayout = DEPTH_READ_ONLY：预通道与前向之间可能插着
        // SSAO pass，它要把这张深度图当**纹理**采样。渲染pass结束
        // 时自动转到 READ_ONLY 布局，前向的 initialLayout 与之衔接。
        att[0].finalLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;

        // (1) 几何缓冲
        att[1].format = kGeometryFormat;
        att[1].samples = m_samples;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (msaa) {
            // 多采样那张只是 resolve 的源，之后没人读 → DONT_CARE
            att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else {
            // 无 MSAA：它本身就是最终可被采样的那张
            att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        uint32_t attachmentCount = 2;
        if (msaa) {
            // (2) resolve 目标：单采样、被后处理采样
            att[2].format = kGeometryFormat;
            att[2].samples = VK_SAMPLE_COUNT_1_BIT;
            att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            att[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            attachmentCount = 3;
        }

        VkAttachmentReference depthRef{
            0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference geoRef{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference geoResolveRef{
            2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &geoRef;
        subpass.pResolveAttachments = msaa ? &geoResolveRef : nullptr;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency deps[2]{};
        // 外部 → 子过程：等该帧此前对同一深度图/几何图的使用结束。
        // 几何图多一类读者：**上一帧的后处理**把它当纹理采样（描边）。
        // 少了 FRAGMENT_SHADER / SHADER_READ 这一对，就是"上一帧还在读、
        // 这一帧已经在写"的 WAR 竞争 —— 表现是偶发的一帧描边乱闪。
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // 子过程 → 外部：深度写"交出去"。读者有两类：
        //   · 前向通道继续当附件用（READ/WRITE，老语义）
        //   · 屏幕空间效果 / 后处理把它们当纹理采样（FRAGMENT + SHADER_READ）
        //     没有这条，SSAO 与描边读到的都是未同步内容
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = attachmentCount;
        ci.pAttachments = att;
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
    // 布局流：预通道停在 DEPTH_READ_ONLY（SSAO 可能已采样过）→ 本
    // pass 开头自动升到 DEPTH_ATTACHMENT → 结束时转回 DEPTH_READ_ONLY
    // （前向之后的屏幕空间效果继续采样它）。storeOp 因此必须是 STORE
    // —— 过去"这一帧之后没人再读深度"的假设已被屏幕空间效果打破。
    attaches[1].format = m_depthFormat;
    attaches[1].samples = m_samples;
    attaches[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attaches[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attaches[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attaches[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attaches[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    attaches[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;

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
    // 子过程 → 外部：颜色（含 resolve）写完，后处理才能采样；
    // 深度同理 —— 屏幕空间效果（SSR/SSGI/god rays）在前向之后把
    // 深度图当纹理采样，深度写必须对片元阶段可见。
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
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

    // resolve 图：既是颜色附件（MSAA 关时），也是被采样的那张。
    // TRANSFER_SRC 是给反射探针捕获（R5-1）用的：捕获要把这一面
    // vkCmdCopyImage 进立方体贴图的某一层。usage 只是"允许"，不改变
    // 布局与带宽 —— 不开探针的运行一个字节的差异都没有。
    createImage2D(m_device, m_extent.width, m_extent.height, kColorFormat,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, f.resolveImage, f.resolveMemory,
                  f.resolveView);

    // 深度图：附件身份之外还要当**纹理**被屏幕空间效果采样
    // （sampler2DMS）→ usage 加 SAMPLED_BIT。多采样图像按规范
    // 只能以 2DMS 形式取样，不能当普通 sampler2D 过滤采样 ——
    // OffscreenImage.h 顶部注释说的是后者，这里用的是前者。
    createImage2D(m_device, m_extent.width, m_extent.height, m_depthFormat,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT, f.depthImage, f.depthMemory,
                  f.depthView, m_samples);

    // 纹理身份视图：与附件视图同图不同 view（view 只是引用，
    // 布局由 renderpass 的附件描述统一迁移）
    {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = f.depthImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_depthFormat;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(m_device.get(), &vi, nullptr,
                                   &f.depthSampledView));
    }

    // 几何缓冲（法线 + 线性深度）。
    //   MSAA 开：一张多采样（只当 resolve 的源，storeOp=DONT_CARE）
    //            + 一张单采样（resolve 目标，后处理采样这张）
    //   MSAA 关：只建单采样那张（它同时是颜色附件）
    // 采样数与 renderpass 的附件描述必须一致，否则 vkCreateFramebuffer
    // 会直接报 VUID。
    if (msaaEnabled()) {
        createImage2D(m_device, m_extent.width, m_extent.height, kGeometryFormat,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, f.geoImage, f.geoMemory,
                      f.geoView, m_samples);
    }
    createImage2D(m_device, m_extent.width, m_extent.height, kGeometryFormat,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, f.geoResolveImage,
                  f.geoResolveMemory, f.geoResolveView,
                  VK_SAMPLE_COUNT_1_BIT);

    // 深度预通道 framebuffer（深度 + 几何 + [几何 resolve]）
    {
        VkImageView views[3]{};
        views[0] = f.depthView;
        uint32_t count = 0;
        if (msaaEnabled()) {
            views[1] = f.geoView;
            views[2] = f.geoResolveView;
            count = 3;
        } else {
            views[1] = f.geoResolveView;
            count = 2;
        }

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_depthPrePass;
        fbci.attachmentCount = count;
        fbci.pAttachments = views;
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
    destroyImage2D(m_device, f.geoResolveImage, f.geoResolveMemory,
                   f.geoResolveView);
    destroyImage2D(m_device, f.geoImage, f.geoMemory, f.geoView);
    destroyImage2D(m_device, f.depthImage, f.depthMemory, f.depthView);
    if (f.depthSampledView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device.get(), f.depthSampledView, nullptr);
        f.depthSampledView = VK_NULL_HANDLE;
    }
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
