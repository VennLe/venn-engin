#include "render/FrameGraph.h"

#include "render/CameraEffects.h"
#include "render/OffscreenImage.h"
#include "render/ScreenSpaceSettings.h"
#include "render/ShaderPath.h"

#include "core/Logger.h"

#include "rhi/Device.h"
#include "rhi/Pipeline.h"
#include "rhi/DescriptorSet.h"

#include <array>
#include <cstdlib>
#include <cstring>

namespace render {

namespace {

// ---- 链注册表（加效果 = 这里加一行 + 着色器 + 设置 + UI）----
// 顺序即执行顺序：SSGI 先注入间接光，SSR 反射的是"已含 SSGI"的
// 画面（反射里也能看到 color bleeding），god rays 最后叠光柱。
struct PassSpec {
    const char* name;       // 日志 / 调试用
    const char* fragSpv;    // 单采样形态
    const char* fragSpvMsaa;// MSAA 深度形态
};

constexpr PassSpec kPasses[FrameGraph::kMaxPasses] = {
    {"SSGI", "fg_ssgi.frag.spv", "fg_ssgi.msaa.frag.spv"},
    {"SSR", "fg_ssr.frag.spv", "fg_ssr.msaa.frag.spv"},
    {"GODRAYS", "fg_godrays.frag.spv", "fg_godrays.msaa.frag.spv"},
    // 镜头效果排最后：作用于"最终的画面"（含反射 / 光柱）。
    {"DOF", "fg_dof.frag.spv", "fg_dof.msaa.frag.spv"},
    {"MOTIONBLUR", "fg_motionblur.frag.spv", "fg_motionblur.msaa.frag.spv"},
};
static_assert(sizeof(kPasses) / sizeof(kPasses[0]) == FrameGraph::kMaxPasses,
              "kPasses 长度必须与 FrameGraph::kMaxPasses 一致");

// 各 pass 的推常量（必须与对应 .frag 的 push_constant 块逐字段一致）
struct SsgiPush {
    glm::mat4 invProj;
    glm::vec4 projCoefs;
    glm::vec4 params;  // x=半径 y=强度 z=表面偏置 w=未用
};
static_assert(sizeof(SsgiPush) == 96, "");

struct SsrPush {
    glm::mat4 invProj;
    glm::vec4 projCoefs;
    glm::vec4 params;  // x=强度 y=厚度 z=步长 w=最大距离
    glm::vec4 camera;  // x=zn y=zf
};
static_assert(sizeof(SsrPush) == 112, "");

struct GodraysPush {
    glm::vec4 sun;      // xy=屏幕uv z=可见 w=未用
    glm::vec4 params;   // x=强度 y=衰减 z=步进密度 w=未用
    glm::vec4 sunColor; // rgb=光色×强度
};
static_assert(sizeof(GodraysPush) == 48, "");

// 景深：只需要 zn/zf 线性化深度 + 对焦参数
struct DofPush {
    glm::vec4 cam;      // x=zn y=zf z=对焦距离 w=光圈
    glm::vec4 params;   // x=最大半径(像素) y=texelX z=texelY w=未用
};
static_assert(sizeof(DofPush) == 32, "");

// 运动模糊：只传投影系数（重建视图点）+ 一个复投影矩阵
struct MotionBlurPush {
    glm::vec4 cam;          // x=P00 y=P11 z=zn w=zf
    glm::mat4 clipFromView; // prevViewProj × inverse(view)
    glm::vec4 params;       // x=强度 y=采样数 z=最大速度(uv) w=未用
};
static_assert(sizeof(MotionBlurPush) == 96, "");

// TAA：与运动模糊共用同一条重投影链（clipFromView），多一个邻域钳制系数
struct TaaPush {
    glm::vec4 cam;          // x=P00 y=P11 z=zn w=zf（未抖动）
    glm::mat4 clipFromView; // prevViewProj（未抖动）× inverse(view)
    glm::vec4 params;       // x=blend y=历史有效 z=texelX w=texelY
    glm::vec4 clampP;       // x=方差钳制系数(sigma)
};
static_assert(sizeof(TaaPush) == 112, "");

} // namespace

bool FrameGraph::chainActive(const ScreenSpaceSettings& s,
                             const CameraEffectSettings& fx) {
    return s.taa || s.ssgi || s.ssr || s.godrays || fx.dof || fx.motionBlur;
}

int FrameGraph::passCount(const ScreenSpaceSettings& s,
                          const CameraEffectSettings& fx) {
    int n = 0;
    if (s.ssgi) ++n;
    if (s.ssr) ++n;
    if (s.godrays) ++n;
    if (fx.dof) ++n;
    if (fx.motionBlur) ++n;
    return n;
}

FrameGraph::FrameGraph(rhi::Device& device, VkFormat sceneFormat,
                       uint32_t framesInFlight)
    : m_device(device), m_sceneFormat(sceneFormat),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    m_setLayout = rhi::makeSetLayout(m_device, {binding});

    // TAA 的输入集是**一个 set 三个 binding**（本帧色 / 深度 / 历史），
    // 与其它 pass 的"源图 set + 深度 set"两集结构不同 —— 单独一个 layout。
    // 深度那份按 MSAA 与否绑 2D 或 2DMS 视图：描述符类型相同
    // （COMBINED_IMAGE_SAMPLER），差异只在 view 的维度，一个 layout 够用
    // （与 m_setLayout 同时服务两种深度形态同理）。
    {
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        m_taaSetLayout = rhi::makeSetLayout(m_device, {b[0], b[1], b[2]});
    }

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_sampler));

    // 深度采样器禁过滤（理由见 SSAOPass 同款注释）
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_depthSampler));

    // ---- 共享链 renderpass（与 PostProcess 的 bloom RP 同款语义）----
    VkAttachmentDescription color{};
    color.format = m_sceneFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // 全屏覆盖
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
    VK_CHECK(vkCreateRenderPass(m_device.get(), &ci, nullptr,
                                &m_chainRenderPass));
}

FrameGraph::~FrameGraph() {
    m_device.waitIdle();
    destroyWorkImages();
    destroyTaaImages();
    for (auto& p0 : m_pipelines) {
        for (auto& p : p0) p.reset();
    }
    for (auto& p : m_taaPipelines) p.reset();
    m_descriptors.reset();

    VkDevice dev = m_device.get();
    if (m_chainRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_chainRenderPass, nullptr);
        m_chainRenderPass = VK_NULL_HANDLE;
    }
    if (m_depthSampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_depthSampler, nullptr);
        m_depthSampler = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_taaSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_taaSetLayout, nullptr);
        m_taaSetLayout = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
}

void FrameGraph::recreate(VkExtent2D extent, VkSampleCountFlagBits samples,
                          VkFormat depthFormat) {
    (void)depthFormat;
    m_device.waitIdle();

    destroyWorkImages();
    destroyTaaImages();
    m_descriptors.reset();

    m_extent = extent;
    m_msaa = samples != VK_SAMPLE_COUNT_1_BIT;
    if (m_extent.width == 0) m_extent.width = 1;
    if (m_extent.height == 0) m_extent.height = 1;

    createWorkImages();
    createTaaImages();
    createPipelines();
    buildDescriptorSets();
    m_ready = true;
}

void FrameGraph::invalidateTaaHistory() {
    for (size_t i = 0; i < m_taaValid.size(); ++i) m_taaValid[i] = false;
}

void FrameGraph::setDepthViews(const std::vector<VkImageView>& views) {
    if (!m_descriptors) return;
    for (size_t i = 0; i < m_depthSets.size() && i < views.size(); ++i) {
        m_descriptors->writeTexture(
            m_depthSets[i], 0, m_depthSampler, views[i],
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    }
    // TAA 也要读同一张深度（binding 1）
    for (size_t i = 0; i < m_taaSets.size() && i < views.size(); ++i) {
        m_descriptors->writeTexture(
            m_taaSets[i], 1, m_depthSampler, views[i],
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    }
}

void FrameGraph::createWorkImages() {
    m_work.resize(m_framesInFlight);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT;
    for (uint32_t fi = 0; fi < m_work.size(); ++fi) {
        for (int k = 0; k < 2; ++k) {
            WorkImage& w = m_work[fi][k];
            createImage2D(m_device, m_extent.width, m_extent.height,
                          m_sceneFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                          w.image, w.memory, w.view);

            VkFramebufferCreateInfo fbci{};
            fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbci.renderPass = m_chainRenderPass;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &w.view;
            fbci.width = m_extent.width;
            fbci.height = m_extent.height;
            fbci.layers = 1;
            VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr,
                                         &w.fb));
        }
    }
}

void FrameGraph::destroyWorkImages() {
    for (auto& frame : m_work) {
        for (int k = 0; k < 2; ++k) {
            WorkImage& w = frame[k];
            if (w.fb != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(m_device.get(), w.fb, nullptr);
                w.fb = VK_NULL_HANDLE;
            }
            destroyImage2D(m_device, w.image, w.memory, w.view);
        }
    }
    m_work.clear();
    for (auto& v : m_workSets) v.clear();
    m_hdrSets.clear();
    m_depthSets.clear();
    m_ready = false;
}

void FrameGraph::createTaaImages() {
    m_taaOut.resize(m_framesInFlight);
    m_taaHist.resize(m_framesInFlight);
    m_taaValid.assign(m_framesInFlight, false);  // 新图 = 没有历史

    // out：写目标（COLOR_ATTACHMENT）+ 被链/PostProcess 采样 + 当 copy 源；
    // hist：只当 copy 目标 + 被 TAA 采样 —— 少一个 COLOR_ATTACHMENT 位。
    const VkImageUsageFlags outUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags histUsage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    for (uint32_t fi = 0; fi < m_framesInFlight; ++fi) {
        createImage2D(m_device, m_extent.width, m_extent.height, m_sceneFormat,
                      outUsage, VK_IMAGE_ASPECT_COLOR_BIT, m_taaOut[fi].image,
                      m_taaOut[fi].memory, m_taaOut[fi].view);

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_chainRenderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &m_taaOut[fi].view;
        fbci.width = m_extent.width;
        fbci.height = m_extent.height;
        fbci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fbci, nullptr,
                                     &m_taaOut[fi].fb));

        createImage2D(m_device, m_extent.width, m_extent.height, m_sceneFormat,
                      histUsage, VK_IMAGE_ASPECT_COLOR_BIT, m_taaHist[fi].image,
                      m_taaHist[fi].memory, m_taaHist[fi].view);
    }
}

void FrameGraph::destroyTaaImages() {
    for (TaaImage& t : m_taaOut) {
        if (t.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device.get(), t.fb, nullptr);
            t.fb = VK_NULL_HANDLE;
        }
        destroyImage2D(m_device, t.image, t.memory, t.view);
    }
    for (TaaImage& t : m_taaHist) {
        destroyImage2D(m_device, t.image, t.memory, t.view);
    }
    m_taaOut.clear();
    m_taaHist.clear();
    m_taaValid.clear();
    m_taaSets.clear();
}

void FrameGraph::createPipelines() {
    for (uint32_t p = 0; p < kMaxPasses; ++p) {
        for (uint32_t msaa = 0; msaa < 2; ++msaa) {
            rhi::PipelineDesc desc;
            desc.vertSpv =
                vkutil::readFile(resolveShaderPath("fullscreen.vert.spv"));
            desc.fragSpv = vkutil::readFile(resolveShaderPath(
                msaa ? kPasses[p].fragSpvMsaa : kPasses[p].fragSpv));
            // 每个链 pass 恰好两个输入：源图 + 深度图
            desc.setLayouts.push_back(m_setLayout);
            desc.setLayouts.push_back(m_setLayout);
            desc.pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            desc.pushConstant.offset = 0;
            desc.pushConstant.size = 128;  // 覆盖各 pass 的实际推常量
            desc.renderPass = m_chainRenderPass;
            desc.colorAttachmentCount = 1;
            desc.depthTest = false;
            desc.depthWrite = false;
            desc.cullMode = VK_CULL_MODE_NONE;
            m_pipelines[p][msaa] =
                std::make_unique<rhi::Pipeline>(m_device, desc);
        }
    }

    // TAA 预 pass：一个 set（3 binding：本帧色 / 深度 / 历史）。
    for (uint32_t msaa = 0; msaa < 2; ++msaa) {
        rhi::PipelineDesc desc;
        desc.vertSpv =
            vkutil::readFile(resolveShaderPath("fullscreen.vert.spv"));
        desc.fragSpv = vkutil::readFile(resolveShaderPath(
            msaa ? "fg_taa.msaa.frag.spv" : "fg_taa.frag.spv"));
        desc.setLayouts.push_back(m_taaSetLayout);
        desc.pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        desc.pushConstant.offset = 0;
        desc.pushConstant.size = static_cast<uint32_t>(sizeof(TaaPush));
        desc.renderPass = m_chainRenderPass;
        desc.colorAttachmentCount = 1;
        desc.depthTest = false;
        desc.depthWrite = false;
        desc.cullMode = VK_CULL_MODE_NONE;
        m_taaPipelines[msaa] =
            std::make_unique<rhi::Pipeline>(m_device, desc);
    }
}

void FrameGraph::buildDescriptorSets() {
    const uint32_t n = m_framesInFlight;
    // 每帧：hdr / work0 / work1 / depth / taa 共 5 个 set
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        m_device, n * 5 + 8, 0, n * 5 + 8);

    m_hdrSets.resize(n);
    m_workSets[0].resize(n);
    m_workSets[1].resize(n);
    m_depthSets.resize(n);
    m_taaSets.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        m_hdrSets[i] = m_descriptors->allocate(m_setLayout);
        m_workSets[0][i] = m_descriptors->allocate(m_setLayout);
        m_workSets[1][i] = m_descriptors->allocate(m_setLayout);
        m_depthSets[i] = m_descriptors->allocate(m_setLayout);
        m_taaSets[i] = m_descriptors->allocate(m_taaSetLayout);

        // 工作图视图是自己的 → 这里就能写；HDR / 深度视图是外面喂的
        m_descriptors->writeTexture(m_workSets[0][i], 0, m_sampler,
                                    m_work[i][0].view);
        m_descriptors->writeTexture(m_workSets[1][i], 0, m_sampler,
                                    m_work[i][1].view);
        // TAA 的 binding 2 = 历史图。视图恒定（不 ping-pong），只写一次；
        // binding 0（本帧 HDR）每帧重写，binding 1（深度）由 setDepthViews 喂。
        m_descriptors->writeTexture(m_taaSets[i], 2, m_sampler,
                                    m_taaHist[i].view);
    }
}

void FrameGraph::execute(VkCommandBuffer cmd, uint32_t frameIndex,
                         VkImageView hdrColorView,
                         const ScreenSpaceSettings& s,
                         const CameraEffectSettings& fx,
                         const FrameGraphInput& in) {
    if (!m_ready || m_work.empty()) return;
    const uint32_t fi = frameIndex % static_cast<uint32_t>(m_work.size());

    // ---- TAA 预 pass（链的头）----
    // 先跑它：后面的 SSGI/SSR/godrays/DoF/MB 都在"已经解析掉抖动"的画面上
    // 工作（UE 里 TAA 也在后处理链的最前面）。关着时一次不录。
    VkImageView chainSrc = hdrColorView;
    if (s.taa && !m_taaOut.empty() && m_taaPipelines[0]) {
        if (!m_taaWasOn) invalidateTaaHistory();  // 关→开：旧历史不可信
        chainSrc = recordTaaPass(cmd, fi, hdrColorView, s, in);
    }
    m_taaWasOn = s.taa;

    // 链源头描述符每帧重写：TAA 开着时是解析结果、否则是 HDR resolve 本尊；
    // 而且 HdrTarget 重建后视图会换（recreate 节奏与它未必同帧）。
    m_descriptors->writeTexture(m_hdrSets[fi], 0, m_sampler, chainSrc);

    const bool enabled[kMaxPasses] = {s.ssgi,   s.ssr,        s.godrays,
                                      fx.dof,   fx.motionBlur};
    const uint32_t flavor = m_msaa ? 1u : 0u;

    const float texelX = 1.0f / static_cast<float>(m_extent.width);
    const float texelY = 1.0f / static_cast<float>(m_extent.height);

    int passIdx = 0;  // 链上的序号（决定读写哪张工作图）
    for (uint32_t p = 0; p < kMaxPasses; ++p) {
        if (!enabled[p]) continue;

        // ---- 推常量（按 pass 拼装）----
        uint8_t push[128] = {};
        uint32_t pushSize = 0;
        switch (p) {
            case 0: {  // SSGI
                SsgiPush u{};
                u.invProj = in.invProj;
                u.projCoefs = in.projCoefs;
                u.params = glm::vec4(s.ssgiRadius, s.ssgiStrength, 0.05f, 0.0f);
                pushSize = sizeof(u);
                std::memcpy(push, &u, sizeof(u));
                break;
            }
            case 1: {  // SSR
                SsrPush u{};
                u.invProj = in.invProj;
                u.projCoefs = in.projCoefs;
                u.params = glm::vec4(s.ssrStrength, 0.35f, 0.35f, 30.0f);
                u.camera = in.camera;
                pushSize = sizeof(u);
                std::memcpy(push, &u, sizeof(u));
                break;
            }
            case 2: {  // GODRAYS
                GodraysPush u{};
                u.sun = glm::vec4(in.sunUv.x, in.sunUv.y, in.sunVisible, 0.0f);
                u.params = glm::vec4(s.godraysStrength, 0.93f, 1.0f, 0.0f);
                u.sunColor = in.sunColor;
                pushSize = sizeof(u);
                std::memcpy(push, &u, sizeof(u));
                break;
            }
            case 3: {  // DOF
                DofPush u{};
                u.cam = glm::vec4(in.camera.x, in.camera.y, fx.dofFocus,
                                  fx.dofAperture);
                u.params = glm::vec4(fx.dofMaxRadius, texelX, texelY, 0.0f);
                pushSize = sizeof(u);
                std::memcpy(push, &u, sizeof(u));
                break;
            }
            default: {  // MOTIONBLUR
                MotionBlurPush u{};
                u.cam = glm::vec4(in.projCoefs.x, in.projCoefs.y,
                                  in.camera.x, in.camera.y);
                u.clipFromView = in.clipFromView;
                int samples = fx.motionBlurSamples;
                if (samples < 2) samples = 2;
                if (samples > 16) samples = 16;
                u.params = glm::vec4(fx.motionBlurStrength,
                                     static_cast<float>(samples), 0.25f, 0.0f);
                pushSize = sizeof(u);
                std::memcpy(push, &u, sizeof(u));
                break;
            }
        }

        // ---- ping-pong 保守屏障（WAW / WAR 一网打尽）----
        // oldLayout=UNDEFINED：spec 允许它匹配任意实际布局（语义是
        // "旧内容不再需要"）。这里不需要旧内容（每 pass 全屏覆盖写），
        // 只需要内存依赖 —— 而且必须用 UNDEFINED：k=1 图在单 pass 链里
        // 永远不会被写（停在 UNDEFINED），renderpass 的 initialLayout=
        // UNDEFINED 也会把验证层的布局状态重置，用 SHADER_READ_ONLY 做
        // oldLayout 会每帧撞 09600。
        for (int k = 0; k < 2; ++k) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = m_work[fi][k].image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_SHADER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(
                cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);
        }

        // ---- 录制本 pass ----
        const WorkImage& target = m_work[fi][passIdx & 1];
        VkDescriptorSet srcSet =
            passIdx == 0 ? m_hdrSets[fi] : m_workSets[(passIdx - 1) & 1][fi];
        VkDescriptorSet sets[2] = {srcSet, m_depthSets[fi]};

        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = m_chainRenderPass;
        rpbi.framebuffer = target.fb;
        rpbi.renderArea.offset = {0, 0};
        rpbi.renderArea.extent = m_extent;
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        rhi::Pipeline& pl = *m_pipelines[p][flavor];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl.get());

        VkViewport vp{};
        vp.width = static_cast<float>(m_extent.width);
        vp.height = static_cast<float>(m_extent.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = m_extent;
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pl.layout(), 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(cmd, pl.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           pushSize, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);
        ++passIdx;
    }

    // 自检（MYVK_FG_DBG=1）：链上跑了哪些 pass。验证脚本用它断言
    // "开关组合 → 执行清单"，也用来确认全关时这条日志压根不出现。
    static const bool dbg = std::getenv("MYVK_FG_DBG") != nullptr;
    static uint32_t tick = 0;
    if (dbg && (tick++ % 30) == 1) {
        VK_LOG_INFO("FRAMEGRAPH passes=%d msaa=%d extent=%ux%u taa=%d hist=%d",
                    passCount(s, fx), m_msaa ? 1 : 0, m_extent.width,
                    m_extent.height, s.taa ? 1 : 0,
                    (s.taa && fi < m_taaValid.size() && m_taaValid[fi]) ? 1 : 0);
    }
}

VkImageView FrameGraph::outputView(uint32_t frameIndex,
                                   const ScreenSpaceSettings& s,
                                   const CameraEffectSettings& fx) const {
    const uint32_t fi =
        m_work.empty()
            ? 0u
            : frameIndex % static_cast<uint32_t>(m_work.size());
    const int n = passCount(s, fx);
    if (n > 0) {
        if (m_work.empty()) return VK_NULL_HANDLE;
        return m_work[fi][(n - 1) & 1].view;
    }
    // 只剩 TAA 时（链上没有其它 pass），输出就是 TAA 的解析结果。
    // m_taaOut 的视图跨帧恒定 —— PostProcess 的输入描述符只在 recreate
    // 时写一次，这条稳定性是硬要求。
    if (s.taa && fi < m_taaOut.size()) return m_taaOut[fi].view;
    return VK_NULL_HANDLE;
}

VkImageView FrameGraph::recordTaaPass(VkCommandBuffer cmd, uint32_t fi,
                                      VkImageView hdrColorView,
                                      const ScreenSpaceSettings& s,
                                      const FrameGraphInput& in) {
    if (fi >= m_taaOut.size() || !m_taaPipelines[0]) return hdrColorView;
    const bool histOk = fi < m_taaValid.size() && m_taaValid[fi];

    // ---- 1) 历史搬运：out（本轮上一次的解析结果）→ hist ----
    // 为什么必须有这一步：out 的视图**必须恒定**（PostProcess 的输入
    // 描述符只在 recreate 时写一次），所以 TAA 不能 ping-pong 输出图；
    // 历史只能从 out 拷一份过来。首轮 / 尺寸变化后历史无效，连搬运都省。
    if (histOk) {
        VkImageMemoryBarrier b[2]{};
        for (int i = 0; i < 2; ++i) {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        b[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[0].image = m_taaOut[fi].image;

        b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // 旧历史不再需要
        b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].srcAccessMask = 0;
        b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].image = m_taaHist[fi].image;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, b);

        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {m_extent.width, m_extent.height, 1};
        vkCmdCopyImage(cmd, m_taaOut[fi].image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_taaHist[fi].image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier b2{};
        b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b2.image = m_taaHist[fi].image;
        b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b2);
    } else {
        // 首轮 / 尺寸变化后：历史内容无效，跳过搬运 —— 但**布局迁移不能
        // 省**。描述符是按 SHADER_READ_ONLY_OPTIMAL 写的，而验证层对
        // "这个描述符会不会被采样"做的是 SPIR-V 静态判定（不看片元里的
        // if 分支），于是它会认定本帧要采这张仍停在 UNDEFINED 的图，
        // 直接报 VUID 09600。显式迁一次即可。
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_taaHist[fi].image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
    // 注意 m_taaOut 此刻停在 TRANSFER_SRC_OPTIMAL（或首轮的 UNDEFINED）：
    // 下面的 renderpass 用 initialLayout=UNDEFINED，会自动迁移到附件布局
    // （内容本就要被全屏覆盖，丢弃旧内容是明确语义）。

    // ---- 2) TAA 本体 ----
    m_descriptors->writeTexture(m_taaSets[fi], 0, m_sampler, hdrColorView);

    TaaPush u{};
    u.cam = glm::vec4(in.projCoefs.x, in.projCoefs.y, in.camera.x,
                      in.camera.y);
    u.clipFromView = in.clipFromView;
    float blend = s.taaBlend;
    if (blend < 0.01f) blend = 0.01f;
    if (blend > 1.0f) blend = 1.0f;
    float clampK = s.taaClamp;
    if (clampK < 0.25f) clampK = 0.25f;
    if (clampK > 4.0f) clampK = 4.0f;
    u.params = glm::vec4(blend, histOk ? 1.0f : 0.0f,
                         1.0f / static_cast<float>(m_extent.width),
                         1.0f / static_cast<float>(m_extent.height));
    u.clampP = glm::vec4(clampK, 0.0f, 0.0f, 0.0f);

    rhi::Pipeline& pl = *m_taaPipelines[m_msaa ? 1 : 0];
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = m_chainRenderPass;
    rpbi.framebuffer = m_taaOut[fi].fb;
    rpbi.renderArea.offset = {0, 0};
    rpbi.renderArea.extent = m_extent;
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl.get());

    VkViewport vp{};
    vp.width = static_cast<float>(m_extent.width);
    vp.height = static_cast<float>(m_extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{};
    sc.extent = m_extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl.layout(),
                            0, 1, &m_taaSets[fi], 0, nullptr);
    vkCmdPushConstants(cmd, pl.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<uint32_t>(sizeof(u)), &u);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    m_taaValid[fi] = true;
    return m_taaOut[fi].view;
}

} // namespace render
