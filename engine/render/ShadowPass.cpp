#include "render/ShadowPass.h"

#include "rhi/Device.h"
#include "rhi/Pipeline.h"

#include "render/RenderTypes.h"
#include "render/ShaderPath.h"

#include "scene/Scene.h"
#include "ecs/Components.h"
#include "assets/Mesh.h"

#include <cmath>

namespace render {

ShadowPass::ShadowPass(rhi::Device& device,
                       VkDescriptorSetLayout frameSetLayout,
                       uint32_t framesInFlight, uint32_t resolution)
    : m_device(device), m_frameSetLayout(frameSetLayout),
      m_resolution(resolution) {
    createResource();

    // 每个 frame-in-flight 一张深度图，避免跨帧的写/读数据竞争
    m_frames.resize(framesInFlight > 0 ? framesInFlight : 1);
    for (auto& f : m_frames) createFrame(f);

    createPipeline();
}

ShadowPass::~ShadowPass() {
    for (auto& f : m_frames) destroyFrame(f);
    m_frames.clear();

    VkDevice dev = m_device.get();
    m_pipeline.reset();  // 需要 device 仍有效

    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
}

void ShadowPass::createResource() {
    VkDevice dev = m_device.get();

    m_depthFormat = m_device.findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
         VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);

    // ---- 比较采样器 ----
    // compareEnable=TRUE 时 texture() 在硬件里直接做深度比较，
    // 返回 0/1（是否被遮挡），省去着色器中的手工判断
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // 阴影图范围外视为"无遮挡" → border 取不透明白
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &m_sampler));

    // ---- depth-only RenderPass（各帧共用，只描述格式）----
    VkAttachmentDescription depth{};
    depth.format = m_depthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{
        0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;  // 只写深度
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[2]{};
    // 外部 → 子过程：等该帧此前对贴图的采样读完
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // 子过程 → 外部：深度写完后主 Pass 才能采样（跨 render pass 同步）
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &depth;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &m_renderPass));
}

void ShadowPass::createFrame(Frame& f) {
    VkDevice dev = m_device.get();

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = m_depthFormat;
    ci.extent = {m_resolution, m_resolution, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, nullptr, &f.image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, f.image, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &f.memory));
    VK_CHECK(vkBindImageMemory(dev, f.image, f.memory, 0));

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = f.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = m_depthFormat;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &f.view));

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = m_renderPass;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &f.view;
    fbci.width = m_resolution;
    fbci.height = m_resolution;
    fbci.layers = 1;
    VK_CHECK(vkCreateFramebuffer(dev, &fbci, nullptr, &f.framebuffer));
}

void ShadowPass::destroyFrame(Frame& f) {
    VkDevice dev = m_device.get();
    if (f.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(dev, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
    }
    if (f.view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, f.view, nullptr);
        f.view = VK_NULL_HANDLE;
    }
    if (f.image != VK_NULL_HANDLE) {
        vkDestroyImage(dev, f.image, nullptr);
        f.image = VK_NULL_HANDLE;
    }
    if (f.memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, f.memory, nullptr);
        f.memory = VK_NULL_HANDLE;
    }
}

void ShadowPass::createPipeline() {
    rhi::PipelineDesc desc;
    desc.vertSpv = vkutil::readFile(resolveShaderPath("shadow.vert.spv"));
    desc.fragSpv = vkutil::readFile(resolveShaderPath("shadow.frag.spv"));
    desc.vertexBindings.push_back(assets::Vertex::bindingDescription());
    desc.vertexAttributes = assets::Vertex::attributeDescriptions();
    desc.setLayouts = {m_frameSetLayout};
    desc.pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    desc.pushConstant.offset = 0;
    desc.pushConstant.size = sizeof(PushConstants);
    desc.renderPass = m_renderPass;
    desc.colorAttachmentCount = 0;  // depth-only
    desc.depthTest = true;
    desc.depthWrite = true;
    // 只渲染正面：剔除背面能显著减少 shadow acne
    desc.cullMode = VK_CULL_MODE_FRONT_BIT;
    desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // 深度偏移：把深度沿法线推开一点，抵消自阴影条纹
    desc.depthBiasEnable = true;
    desc.depthBiasConstant = 1.25f;
    desc.depthBiasSlope = 1.75f;

    m_pipeline = std::make_unique<rhi::Pipeline>(m_device, desc);
}

void ShadowPass::updateLightMatrix(const glm::vec3& direction,
                                   const glm::vec3& center, float radius) {
    const glm::vec3 d = glm::normalize(direction);  // 传播方向：光 → 场景

    // 光源放在场景中心逆着光方向的位置
    const float distance = radius * 2.0f;
    const glm::vec3 lightPos = center - d * distance;

    glm::vec3 up(0.0f, 0.0f, 1.0f);  // Z-up
    // 光方向接近垂直时 lookAt 会退化（up 与视线共线），换用另一个轴
    if (std::abs(glm::dot(d, up)) > 0.99f) {
        up = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    const glm::mat4 view = glm::lookAt(lightPos, center, up);
    // 正交范围 = 场景包围球；近远平面紧贴包围球以提升深度精度。
    //
    // ⚠ 深度必须是 **0→1（Zero-to-One）** 约定，不能用 glm::ortho：
    //   · Vulkan 的 clip/NDC 深度范围是 [0, 1]，glm::ortho 给的是
    //     OpenGL 约定 [-1, 1]；
    //   · 若用 GL 约定，阴影图**写入**的深度是 ndc_z∈[-1,1] 被裁剪后
    //     的一半（z<0 的几何整段被裁掉），而 pbr.frag **采样**时算出的
    //     参考深度又是另一套映射 —— 两侧不在同一深度空间，整个覆盖区
    //     会被判成"被遮挡"，屏幕上就是一块边界笔直的暗色四边形。
    //   · glm::orthoRH_ZO 把近平面映到 0、远平面映到 1，与 Vulkan 一致；
    //     相应地 pbr.frag 里对 proj.z **不再**做 *0.5+0.5（只有 xy 需要）。
    const glm::mat4 proj = glm::orthoRH_ZO(-radius, radius, -radius, radius,
                                           distance - radius, distance + radius);

    m_lightSpace = proj * view;
}

void ShadowPass::render(VkCommandBuffer cmd, scene::Scene& scene,
                        VkDescriptorSet frameSet, uint32_t frameIndex) {
    if (m_frames.empty()) return;
    Frame& f = m_frames[frameIndex % m_frames.size()];

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_renderPass;
    rp.framebuffer = f.framebuffer;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {m_resolution, m_resolution};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->get());

    VkViewport vp{};
    vp.width = static_cast<float>(m_resolution);
    vp.height = static_cast<float>(m_resolution);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = {m_resolution, m_resolution};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    if (frameSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeline->layout(), 0, 1, &frameSet, 0,
                                nullptr);
    }

    // 只渲染投射阴影的物体（VisibilityComponent::castShadow）
    scene.forEachShadowCaster([&](ecs::Entity e, ecs::MeshComponent& mc) {
        if (!mc.mesh || !mc.mesh->valid()) return;

        PushConstants push{};
        push.model = scene.worldMatrix(e);
        vkCmdPushConstants(cmd, m_pipeline->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(PushConstants), &push);
        mc.mesh->draw(cmd);
    });

    vkCmdEndRenderPass(cmd);
}

} // namespace render
