#include "render/PostProcess.h"

#include "render/OffscreenImage.h"
#include "render/ShaderPath.h"

#include "rhi/Device.h"
#include "rhi/Pipeline.h"
#include "rhi/DescriptorSet.h"

#include <cstring>

namespace render {

namespace {

// 编辑器背景下交换链的清屏色（线性值；交换链是 SRGB，
// 写出去会被硬件编码成 sRGB，0.02 → 约 0.16，是很稳的深灰）
constexpr float kUiClear[4] = {0.020f, 0.022f, 0.028f, 1.0f};

} // namespace

PostProcess::PostProcess(rhi::Device& device, VkFormat swapchainFormat,
                         VkFormat hdrFormat, uint32_t framesInFlight)
    : m_device(device), m_swapFormat(swapchainFormat), m_hdrFormat(hdrFormat),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    createSetLayoutAndSampler();
    createRenderPasses();
    createPipelines();
}

PostProcess::~PostProcess() {
    m_device.waitIdle();
    destroyDescriptors();
    destroyBloom();
    destroySwapFramebuffers();
    destroySceneOutputFrames();

    VkDevice dev = m_device.get();
    m_postOffscreenPipeline.reset();
    m_postPipeline.reset();
    m_blurPipeline.reset();
    m_brightPipeline.reset();

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
    VkRenderPass renderPass, const char* fragSpv, uint32_t setCount) {
    rhi::PipelineDesc desc;
    desc.vertSpv = vkutil::readFile(resolveShaderPath("fullscreen.vert.spv"));
    desc.fragSpv = vkutil::readFile(resolveShaderPath(fragSpv));
    // 无顶点缓冲：顶点由 fullscreen.vert 里的 gl_VertexID 生成
    for (uint32_t i = 0; i < setCount; ++i) desc.setLayouts.push_back(m_setLayout);
    desc.pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    desc.pushConstant.offset = 0;
    desc.pushConstant.size = sizeof(float) * 4;
    desc.renderPass = renderPass;
    desc.colorAttachmentCount = 1;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.cullMode = VK_CULL_MODE_NONE;
    return std::make_unique<rhi::Pipeline>(m_device, desc);
}

void PostProcess::createPipelines() {
    m_brightPipeline =
        makeFullscreenPipeline(m_bloomRenderPass, "bloom_bright.frag.spv", 1);
    m_blurPipeline =
        makeFullscreenPipeline(m_bloomRenderPass, "bloom_blur.frag.spv", 1);
    m_postPipeline =
        makeFullscreenPipeline(m_postRenderPass, "post.frag.spv", 2);
    // 离屏目标那条管线（编辑器视口模式才用得上）
    m_postOffscreenPipeline =
        makeFullscreenPipeline(m_offscreenRenderPass, "post.frag.spv", 2);
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
        createImage2D(m_device, m_sceneExtent.width, m_sceneExtent.height,
                      m_swapFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, f.image,
                      f.memory, f.view);

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_offscreenRenderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &f.view;
        fbci.width = m_sceneExtent.width;
        fbci.height = m_sceneExtent.height;
        fbci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr, &f.fb));
    }
}

void PostProcess::destroyDescriptors() { m_descriptors.reset(); }

void PostProcess::buildDescriptorSets(
    const std::vector<VkImageView>& hdrColorViews) {
    // 输入集数量 = 帧数 × 3（HDR / bloomA / bloomB），留足余量
    const uint32_t count = static_cast<uint32_t>(m_bloom.size()) * 3 + 8;
    m_descriptors =
        std::make_unique<rhi::DescriptorAllocator>(m_device, count, 4, count);

    m_hdrSets.resize(m_bloom.size());
    m_bloomASets.resize(m_bloom.size());
    m_bloomBSets.resize(m_bloom.size());

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
    const VkExtent2D oldSceneExtent = m_sceneExtent;

    // 旧描述符集引用旧的 VkImageView，整池丢弃最省心
    destroyDescriptors();
    destroyBloom();
    destroySwapFramebuffers();

    m_swapExtent = swapchainExtent;

    m_splitOutput = viewportExtent.width > 0 && viewportExtent.height > 0;
    m_sceneExtent = m_splitOutput ? viewportExtent : swapchainExtent;

    // 视口极端小的时候（面板被拖到几乎没有）也保证至少 1×1，
    // 否则 createImage2D 会拿到非法 extent
    if (m_sceneExtent.width == 0) m_sceneExtent.width = 1;
    if (m_sceneExtent.height == 0) m_sceneExtent.height = 1;

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
    const bool sceneOutputReusable =
        wasSplit == m_splitOutput && !m_sceneOutput.empty() &&
        m_sceneOutput.size() == m_framesInFlight &&
        oldSceneExtent.width == m_sceneExtent.width &&
        oldSceneExtent.height == m_sceneExtent.height;

    if (!sceneOutputReusable) {
        destroySceneOutputFrames();
        if (m_splitOutput) createSceneOutputFrames();
    }

    createBloomFrames();
    createSwapFramebuffers(swapchainViews);
    buildDescriptorSets(hdrColorViews);
}

// ---------------------------------------------------------------- 录制

void PostProcess::drawFullscreen(VkCommandBuffer cmd, rhi::Pipeline& pipeline,
                                 VkFramebuffer fb, VkExtent2D extent,
                                 const VkDescriptorSet* sets, uint32_t setCount,
                                 const float push[4]) {
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_bloomRenderPass;
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
                       sizeof(float) * 4, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

void PostProcess::recordComposite(
    VkCommandBuffer cmd, VkFramebuffer fb, VkRenderPass rp,
    rhi::Pipeline& pipeline, VkExtent2D targetExtent, uint32_t frameIndex,
    const std::function<void(VkCommandBuffer)>* overlay) {
    const uint32_t fi =
        frameIndex % static_cast<uint32_t>(m_bloom.size());

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

    VkDescriptorSet sets[2] = {m_hdrSets[fi], m_bloomASets[fi]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.layout(), 0, 2, sets, 0, nullptr);

    const float post[4] = {m_settings.exposure, m_settings.bloomStrength,
                           static_cast<float>(m_settings.tonemapMode),
                           m_settings.vignette};
    vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(float) * 4, post);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // overlay（ImGui）**必须**在这个正在进行的 RenderPass 内绘制：
    // 它的管线绑定在本 RenderPass 上，而重开一个 Pass 又会把刚合成好的
    // 画面清掉（loadOp=CLEAR）。所以只能在这里、vkCmdEndRenderPass 之前画。
    if (overlay && *overlay) (*overlay)(cmd);

    vkCmdEndRenderPass(cmd);
}

void PostProcess::renderScene(
    VkCommandBuffer cmd, uint32_t imageIndex, uint32_t frameIndex,
    const std::function<void(VkCommandBuffer)>& overlay) {
    if (m_bloom.empty() || m_swapFramebuffers.empty()) return;

    const uint32_t fi = frameIndex % static_cast<uint32_t>(m_bloom.size());
    BloomFrame& bf = m_bloom[fi];

    const bool bloomOk = fi < m_hdrSets.size() && fi < m_bloomASets.size() &&
                         fi < m_bloomBSets.size();

    const float texelX = 1.0f / static_cast<float>(m_bloomExtent.width);
    const float texelY = 1.0f / static_cast<float>(m_bloomExtent.height);

    if (bloomOk) {
        // 1) 亮部提取：HDR（全分辨率） → bloomA（半分辨率）
        const float bright[4] = {m_settings.bloomThreshold, m_settings.bloomKnee,
                                 m_settings.exposure, 0.0f};
        drawFullscreen(cmd, *m_brightPipeline, bf.aFb, m_bloomExtent,
                       &m_hdrSets[fi], 1, bright);

        // 2) 水平模糊：bloomA → bloomB
        const float blurH[4] = {texelX, texelY, 1.0f, 0.0f};
        drawFullscreen(cmd, *m_blurPipeline, bf.bFb, m_bloomExtent,
                       &m_bloomASets[fi], 1, blurH);

        // 3) 垂直模糊：bloomB → bloomA（结果留在 bloomA）
        const float blurV[4] = {texelX, texelY, 0.0f, 1.0f};
        drawFullscreen(cmd, *m_blurPipeline, bf.aFb, m_bloomExtent,
                       &m_bloomBSets[fi], 1, blurV);
    }

    if (!bloomOk) return;

    if (m_splitOutput && fi < m_sceneOutput.size()) {
        // ---- 模式 B：合成到离屏图（不画 UI），UI 由 renderUiToSwapchain 负责 ----
        recordComposite(cmd, m_sceneOutput[fi].fb, m_offscreenRenderPass,
                        *m_postOffscreenPipeline, m_sceneExtent, frameIndex,
                        nullptr);

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
                    *m_postPipeline, m_sceneExtent, frameIndex, &overlay);
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

} // namespace render
