#include "render/SSAOPass.h"

#include "render/OffscreenImage.h"
#include "render/ScreenSpaceSettings.h"
#include "render/ShaderPath.h"

#include "rhi/Device.h"
#include "rhi/Pipeline.h"
#include "rhi/DescriptorSet.h"

namespace render {

SSAOPass::SSAOPass(rhi::Device& device, uint32_t framesInFlight)
    : m_device(device), m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    // ---- 输入描述符布局：单 combined image sampler（与 PostProcess 同款）----
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    m_setLayout = rhi::makeSetLayout(m_device, {binding});

    // AO 结果图给 pbr.frag 用线性采样（放大的柔和过渡比像素块好看）
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_sampler));

    // 深度图采样器：shader 里全部走 texelFetch / texture 但不过滤，
    // NEAREST 明确"不插值深度"（插值两个不同表面的深度会造出
    // 不存在的中间深度，污染可见性判断）。
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_depthSampler));

    createRenderPasses();
}

SSAOPass::~SSAOPass() {
    m_device.waitIdle();
    destroyFrames();
    m_ssaoPipeline.reset();
    m_blurPipeline.reset();
    m_descriptors.reset();

    VkDevice dev = m_device.get();
    if (m_blurRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_blurRenderPass, nullptr);
        m_blurRenderPass = VK_NULL_HANDLE;
    }
    if (m_ssaoRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_ssaoRenderPass, nullptr);
        m_ssaoRenderPass = VK_NULL_HANDLE;
    }
    if (m_depthSampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_depthSampler, nullptr);
        m_depthSampler = VK_NULL_HANDLE;
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

void SSAOPass::createRenderPasses() {
    // 与 PostProcess 的 bloom RenderPass 同款：单 R8 附件，
    // UNDEFINED 进 / SHADER_READ_ONLY 出（initialLayout=UNDEFINED +
    // DONT_CARE 意味着"旧内容随便丢"—— 全屏覆盖每帧重写，
    // 也顺带免掉了上一帧残留布局的跟踪）。
    auto makePass = [&](VkRenderPass& out) {
        VkAttachmentDescription color{};
        color.format = VK_FORMAT_R8_UNORM;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{0,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency deps[2]{};
        // 外部 → 子过程：等上一步对本附件的采样读完（WAR）
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // 子过程 → 外部：写完转 SHADER_READ_ONLY 且对片元可见
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
        VK_CHECK(vkCreateRenderPass(m_device.get(), &ci, nullptr, &out));
    };
    makePass(m_ssaoRenderPass);
    makePass(m_blurRenderPass);
}

void SSAOPass::recreate(VkExtent2D extent, VkSampleCountFlagBits samples,
                        VkFormat depthFormat) {
    (void)depthFormat;  // 深度图由 HdrTarget 持有；此参数保留给断言/日志
    m_device.waitIdle();

    destroyFrames();
    m_descriptors.reset();

    m_extent = extent;
    m_msaa = samples != VK_SAMPLE_COUNT_1_BIT;
    if (m_extent.width == 0) m_extent.width = 1;
    if (m_extent.height == 0) m_extent.height = 1;

    createFrames();
    createPipelines();
    buildDescriptorSets();
    m_ready = true;
}

void SSAOPass::setDepthViews(const std::vector<VkImageView>& views) {
    // 深度视图随 HdrTarget 重建而变 —— 重写两处引用它的描述符
    if (!m_descriptors) return;
    for (size_t i = 0; i < m_frames.size() && i < views.size(); ++i) {
        if (m_depthSets.size() > i) {
            m_descriptors->writeTexture(
                m_depthSets[i], 0, m_depthSampler, views[i],
                VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        }
        if (m_blurDepthSets.size() > i) {
            m_descriptors->writeTexture(
                m_blurDepthSets[i], 0, m_depthSampler, views[i],
                VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        }
    }
}

void SSAOPass::createFrames() {
    m_frames.resize(m_framesInFlight);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT;
    for (auto& f : m_frames) {
        createImage2D(m_device, m_extent.width, m_extent.height,
                      VK_FORMAT_R8_UNORM, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                      f.rawImage, f.rawMemory, f.rawView);
        createImage2D(m_device, m_extent.width, m_extent.height,
                      VK_FORMAT_R8_UNORM, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                      f.finalImage, f.finalMemory, f.finalView);

        auto makeFb = [&](VkRenderPass rp, VkImageView view,
                          VkFramebuffer& out) {
            VkFramebufferCreateInfo fbci{};
            fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbci.renderPass = rp;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &view;
            fbci.width = m_extent.width;
            fbci.height = m_extent.height;
            fbci.layers = 1;
            VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr,
                                         &out));
        };
        makeFb(m_ssaoRenderPass, f.rawView, f.rawFb);
        makeFb(m_blurRenderPass, f.finalView, f.finalFb);
    }
}

void SSAOPass::destroyFrames() {
    for (auto& f : m_frames) {
        if (f.rawFb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device.get(), f.rawFb, nullptr);
            f.rawFb = VK_NULL_HANDLE;
        }
        if (f.finalFb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device.get(), f.finalFb, nullptr);
            f.finalFb = VK_NULL_HANDLE;
        }
        destroyImage2D(m_device, f.rawImage, f.rawMemory, f.rawView);
        destroyImage2D(m_device, f.finalImage, f.finalMemory, f.finalView);
    }
    m_frames.clear();
    m_depthSets.clear();
    m_blurRawSets.clear();
    m_blurDepthSets.clear();
    m_ready = false;
}

void SSAOPass::createPipelines() {
    // MSAA 深度 → sampler2DMS 版着色器；单采样 → 普通版。
    //（两份 .spv 都由 CMake 的 SHADER_MSAA_DEPTH_SHADERS 编出。）
    const char* ssaoFrag = m_msaa ? "ssao.msaa.frag.spv" : "ssao.frag.spv";
    const char* blurFrag =
        m_msaa ? "ssao_blur.msaa.frag.spv" : "ssao_blur.frag.spv";

    auto makePipeline = [&](const char* frag, uint32_t setCount) {
        rhi::PipelineDesc desc;
        desc.vertSpv = vkutil::readFile(resolveShaderPath("fullscreen.vert.spv"));
        desc.fragSpv = vkutil::readFile(resolveShaderPath(frag));
        for (uint32_t i = 0; i < setCount; ++i)
            desc.setLayouts.push_back(m_setLayout);
        // SSAO 推常量 96B、模糊 32B —— 统一声明 128B FRAGMENT，
        // 管线布局互不影响（各自独立，不与前向管线共享 set 0）。
        desc.pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        desc.pushConstant.offset = 0;
        desc.pushConstant.size = 128;
        desc.renderPass = setCount == 1 ? m_ssaoRenderPass : m_blurRenderPass;
        desc.colorAttachmentCount = 1;
        desc.depthTest = false;
        desc.depthWrite = false;
        desc.cullMode = VK_CULL_MODE_NONE;
        return std::make_unique<rhi::Pipeline>(m_device, desc);
    };

    m_ssaoPipeline = makePipeline(ssaoFrag, 1);
    m_blurPipeline = makePipeline(blurFrag, 2);
}

void SSAOPass::buildDescriptorSets() {
    const uint32_t n = static_cast<uint32_t>(m_frames.size());
    // 每帧 3 个集（ssao 深度 / blur 原始 / blur 深度）+ 余量
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        m_device, n * 3 + 8, 0, n * 3 + 8);

    m_depthSets.resize(n);
    m_blurRawSets.resize(n);
    m_blurDepthSets.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        m_depthSets[i] = m_descriptors->allocate(m_setLayout);
        m_blurRawSets[i] = m_descriptors->allocate(m_setLayout);
        m_blurDepthSets[i] = m_descriptors->allocate(m_setLayout);
        m_descriptors->writeTexture(m_blurRawSets[i], 0, m_sampler,
                                    m_frames[i].rawView);
    }
    // 深度视图由 setDepthViews 喂（HdrTarget 的视图在 recreate 后才稳）
}

void SSAOPass::recordPass(VkCommandBuffer cmd, VkRenderPass rp,
                          VkFramebuffer fb, VkExtent2D extent,
                          rhi::Pipeline& pipeline,
                          const VkDescriptorSet* sets, uint32_t setCount,
                          const void* push, uint32_t pushSize) {
    VkClearValue clear{};
    clear.color = {{1.0f, 1.0f, 1.0f, 1.0f}};  // DONT_CARE，值无所谓

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = extent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

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

void SSAOPass::render(VkCommandBuffer cmd, uint32_t frameIndex,
                      const ScreenSpaceSettings& s, const glm::mat4& invProj,
                      const glm::vec4& projCoefs, float zn, float zf) {
    if (!m_ready || m_frames.empty()) return;
    const uint32_t fi = frameIndex % static_cast<uint32_t>(m_frames.size());
    const AoFrame& f = m_frames[fi];

    // ---- 1) SSAO：深度 → R8 遮蔽图（带逐像素噪声旋转）----
    struct {
        glm::mat4 invProj;
        glm::vec4 projCoefs;
        glm::vec4 params;  // x=半径 y=强度 z=bias w=未用
    } ssaoPush{};
    ssaoPush.invProj = invProj;
    ssaoPush.projCoefs = projCoefs;
    ssaoPush.params = glm::vec4(s.ssaoRadius, s.ssaoStrength, s.ssaoBias, 0.0f);
    static_assert(sizeof(ssaoPush) == 96, "SSAO 推常量必须与着色器一致");
    recordPass(cmd, m_ssaoRenderPass, f.rawFb, m_extent, *m_ssaoPipeline,
               &m_depthSets[fi], 1, &ssaoPush, sizeof(ssaoPush));

    // ---- 2) 深度感知模糊：原始 → 去噪（pbr.frag 采的就是这张）----
    struct {
        glm::vec4 params;  // x=1/宽 y=1/高 z=深度衰减(1/m) w=未用
        glm::vec4 camera;  // x=zn y=zf
    } blurPush{};
    blurPush.params = glm::vec4(
        1.0f / static_cast<float>(m_extent.width),
        1.0f / static_cast<float>(m_extent.height), 0.5f, 0.0f);
    blurPush.camera = glm::vec4(zn, zf, 0.0f, 0.0f);
    static_assert(sizeof(blurPush) == 32, "模糊推常量必须与着色器一致");
    const VkDescriptorSet blurSets[2] = {m_blurRawSets[fi],
                                         m_blurDepthSets[fi]};
    recordPass(cmd, m_blurRenderPass, f.finalFb, m_extent, *m_blurPipeline,
               blurSets, 2, &blurPush, sizeof(blurPush));
}

VkImageView SSAOPass::resultView(uint32_t frameIndex) const {
    if (m_frames.empty()) return VK_NULL_HANDLE;
    return m_frames[frameIndex % m_frames.size()].finalView;
}

} // namespace render
