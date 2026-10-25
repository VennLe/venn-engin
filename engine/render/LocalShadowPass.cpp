#include "render/LocalShadowPass.h"

#include "rhi/Device.h"
#include "rhi/Pipeline.h"

#include "render/ShadowCull.h"
#include "render/ShaderPath.h"

#include "ecs/Components.h"
#include "assets/Mesh.h"
#include "scene/Light.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace render {

namespace {

// 透视投影的立方体面 FOV：正好 90°（π/2）时 6 个面严丝合缝地拼满球面。
constexpr float kCubeFaceFov = 1.5707963267948966f;

// 近平面取 range 的固定比例（再夹到 [0.1, 2.0] 米）。
//
// 为什么不取一个很小的固定值（比如 0.05）：透视投影的 NDC 深度是
// **双曲**分布，far/near 越大，越靠近远平面的地方两个只差几米的表面在
// NDC 上就几乎贴在一起。实测 range=32 用 near=0.05 时，离灯 15m 的
// 遮挡体和 20m 的地面在 NDC 上只差 7.5e-4 —— 任何合理的深度偏移都会
// 把阴影整个推没（聚光灯的阴影就是这么消失的）。
// 取 range/20 后同一个场景的 NDC 差涨到 2.8e-2，还顺带把深度缓冲的
// 有效位用在了真正有几何的那一段上。
//
// 代价：离灯太近（<near）的物体不投影。对"吊在房间中央的灯"来说，
// 那个半径 1~2 米的球里本来也不该有东西。
constexpr float kLightNearRatio = 0.05f;
constexpr float kLightNearMin = 0.10f;
constexpr float kLightNearMax = 2.00f;

// 聚光灯的锥体在近平面上的截面是一个圆，而透视视锥的截面是正方形。
// 要让圆完整落进正方形，半边长必须 >= 圆半径（相切即可），这里留 2%
// 余量避免圆锥边缘的像素正好压在裁剪面上闪烁。
constexpr float kSpotFovMargin = 1.02f;

// 由 range 推出的近 / 远平面（两处共用，保证 near<far 且都在合理范围）
void lightPlanes(float range, float* outNear, float* outFar) {
    const float far = std::max(range, 0.1f);
    float near = range * kLightNearRatio;
    near = std::min(std::max(near, kLightNearMin), kLightNearMax);
    // 兜底：range 很小时不能让 near 顶到 far 上
    near = std::min(near, far * 0.5f);
    *outNear = near;
    *outFar = far;
}

} // namespace

const glm::vec3 kCubeFaceDir[6] = {
    {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
    {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
};
const glm::vec3 kCubeFaceUp[6] = {
    {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
    {0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
};

uint32_t buildLocalLightMatrices(const scene::LightInstance& li, glm::mat4* out,
                                 uint32_t maxOut, float* outNear,
                                 float* outFar) {
    if (!out || maxOut == 0) return 0;

    const glm::vec3 pos(li.positionRange);
    const float range = li.positionRange.w;
    if (!(range > 0.0f)) return 0;

    float near = 0.0f, far = 0.0f;
    lightPlanes(range, &near, &far);
    if (outNear) *outNear = near;
    if (outFar) *outFar = far;

    const int type = static_cast<int>(li.cosOuterType.y + 0.5f);

    if (type == static_cast<int>(scene::LightInstance::Type::Spot)) {
        // 外锥半角：CPU 侧只存了 cos(外锥)，这里反解回来。
        // 每帧每灯一次 acos，代价可以忽略。
        const float outer = std::acos(
            std::min(std::max(li.cosOuterType.x, -1.0f), 1.0f));
        const float fovY =
            2.0f * std::atan(std::tan(outer) * kSpotFovMargin);
        const glm::vec3 d = glm::normalize(glm::vec3(li.directionCosInner));
        // 视线与 up 共线时 lookAt 退化，换一个轴（与 ShadowPass 同款保护）
        glm::vec3 up(0.0f, 0.0f, 1.0f);
        if (std::abs(glm::dot(d, up)) > 0.99f) up = glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 view = glm::lookAt(pos, pos + d, up);
        const glm::mat4 proj =
            glm::perspectiveRH_ZO(fovY, 1.0f, near, far);
        out[0] = proj * view;
        return 1;
    }

    if (type == static_cast<int>(scene::LightInstance::Type::Point)) {
        const uint32_t n = std::min(maxOut, kPointLightFaces);
        const glm::mat4 proj =
            glm::perspectiveRH_ZO(kCubeFaceFov, 1.0f, near, far);
        for (uint32_t f = 0; f < n; ++f) {
            const glm::vec3 d = kCubeFaceDir[f];
            const glm::mat4 view = glm::lookAt(pos, pos + d, kCubeFaceUp[f]);
            out[f] = proj * view;
        }
        return n;
    }

    return 0;  // Directional：场景级太阳走 ShadowPass，实体级方向光不投影
}

LocalShadowPass::LocalShadowPass(rhi::Device& device,
                                 VkDescriptorSetLayout frameSetLayout,
                                 uint32_t framesInFlight, uint32_t resolution)
    : m_device(device), m_frameSetLayout(frameSetLayout),
      m_resolution(resolution > 0 ? resolution : kDefaultResolution) {
    createResource();

    m_frames.resize(framesInFlight > 0 ? framesInFlight : 1);
    for (auto& f : m_frames) createFrame(f);

    createPipeline();
}

LocalShadowPass::~LocalShadowPass() {
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

uint32_t LocalShadowPass::submitted(uint32_t layer) const {
    return layer < kMaxLayers ? m_submitted[layer] : 0;
}

uint32_t LocalShadowPass::considered(uint32_t layer) const {
    return layer < kMaxLayers ? m_considered[layer] : 0;
}

void LocalShadowPass::resetStats() {
    for (uint32_t i = 0; i < kMaxLayers; ++i) {
        m_submitted[i] = 0;
        m_considered[i] = 0;
    }
}

void LocalShadowPass::createResource() {
    VkDevice dev = m_device.get();

    m_depthFormat = m_device.findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
         VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);

    // 比较采样器：与级联阴影同款（硬件做深度比较）。见 ShadowPass 的注释。
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &m_sampler));

    // depth-only render pass（与 ShadowPass 结构相同，各帧共用）
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
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
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

void LocalShadowPass::createFrame(Frame& f) {
    VkDevice dev = m_device.get();

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = m_depthFormat;
    ci.extent = {m_resolution, m_resolution, 1};
    ci.mipLevels = 1;
    // 固定按上限分配：层数只在"场上同时有几盏灯投影"时才变，
    // 每变一次就重分配上百 MB 显然不划算。多出来的层每帧只清一遍。
    ci.arrayLayers = kMaxLayers;
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

    // 采样用：全部层一起（着色器按层号选）
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = f.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = m_depthFormat;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = kMaxLayers;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &f.arrayView));

    for (uint32_t c = 0; c < kMaxLayers; ++c) {
        VkImageViewCreateInfo lv = vi;
        lv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        lv.subresourceRange.baseArrayLayer = c;
        lv.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(dev, &lv, nullptr, &f.layerView[c]));

        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = m_renderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &f.layerView[c];
        fbci.width = m_resolution;
        fbci.height = m_resolution;
        fbci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(dev, &fbci, nullptr, &f.framebuffer[c]));
    }
}

void LocalShadowPass::destroyFrame(Frame& f) {
    VkDevice dev = m_device.get();
    for (uint32_t c = 0; c < kMaxLayers; ++c) {
        if (f.framebuffer[c] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev, f.framebuffer[c], nullptr);
            f.framebuffer[c] = VK_NULL_HANDLE;
        }
        if (f.layerView[c] != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, f.layerView[c], nullptr);
            f.layerView[c] = VK_NULL_HANDLE;
        }
    }
    if (f.arrayView != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, f.arrayView, nullptr);
        f.arrayView = VK_NULL_HANDLE;
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

void LocalShadowPass::createPipeline() {
    rhi::PipelineDesc desc;
    // 与级联阴影**共用同一个顶点着色器**：它靠 push.which.y 决定从
    // FrameUBO.lightSpace[] 还是 binding11 的 records[] 取矩阵。
    desc.vertSpv = vkutil::readFile(resolveShaderPath("shadow.vert.spv"));
    desc.fragSpv = vkutil::readFile(resolveShaderPath("shadow.frag.spv"));
    desc.vertexBindings.push_back(assets::Vertex::bindingDescription());
    // 同 ShadowPass：shadow.vert 不读 location 5，别多喂。
    // 阴影读 pos/normal/uv/joints/weights + morph（§15）；不读顶点色。
    desc.vertexAttributes = assets::Vertex::attributeDescriptionsShadow();
    desc.setLayouts = {m_frameSetLayout};
    desc.pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    desc.pushConstant.offset = 0;
    desc.pushConstant.size = sizeof(ShadowPushConstants);
    desc.renderPass = m_renderPass;
    desc.colorAttachmentCount = 0;  // depth-only
    desc.depthTest = true;
    desc.depthWrite = true;
    // 只渲染正面（与级联阴影一致）：剔除背面能显著减少 shadow acne
    desc.cullMode = VK_CULL_MODE_FRONT_BIT;
    desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // 深度偏移主要靠**片元侧的世界单位偏移**（见 pbr.frag::ndcBiasFromWorld
    // 与本文件 lightPlanes 的注释）：光栅化阶段的 depthBias 单位是"最小
    // 可分辨深度"，对浮点深度格式约等于 2⁻²³ —— 在这里几乎等于没加，
    // 强行调大反而在近处过偏。这里只留一丁点兜底。
    desc.depthBiasEnable = true;
    desc.depthBiasConstant = 1.0f;
    desc.depthBiasSlope = 1.5f;

    m_pipeline = std::make_unique<rhi::Pipeline>(m_device, desc);
}

void LocalShadowPass::render(VkCommandBuffer cmd, scene::Scene& scene,
                             VkDescriptorSet frameSet, uint32_t frameIndex,
                             uint32_t layer, const glm::mat4* clip,
                             MorphSlotFn morphSlot) {
    if (m_frames.empty() || layer >= kMaxLayers) return;
    Frame& f = m_frames[frameIndex % m_frames.size()];

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_renderPass;
    rp.framebuffer = f.framebuffer[layer];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {m_resolution, m_resolution};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // 未使用的层：清完就走。**不能省** —— 所有层共用一个 array 视图被采样，
    // 留下一层停在 UNDEFINED 会让验证层直接报 IMAGE_LAYOUT 错。
    if (!clip) {
        m_considered[layer] = 0;
        m_submitted[layer] = 0;
        vkCmdEndRenderPass(cmd);
        return;
    }

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

    uint32_t considered = 0;
    uint32_t submitted = 0;

    scene.forEachShadowCaster([&](ecs::Entity e, ecs::MeshComponent& mc) {
        if (!mc.mesh || !mc.mesh->valid()) return;
        ++considered;

        const glm::mat4 world = scene.worldMatrix(e);
        // 每层各剔一次：点光的一层只覆盖 90° 的一个面，绝大多数投影体
        // 都在这个面之外。不剔的话提交量就是 全场景 × 层数。
        if (mc.mesh->hasBounds() &&
            !visibleInLightClip((*clip) * world, mc.mesh->boundsMin(),
                                mc.mesh->boundsMax())) {
            return;
        }
        ++submitted;

        ShadowPushConstants push{};
        push.model = world;
        // which.y = 1 → shadow.vert 从 binding11 的 records[layer] 取矩阵
        push.which = glm::vec4(static_cast<float>(layer), 1.0f, 0.0f, 0.0f);
        // 蒙皮网格：jointBase 必须与主前向路径一致，否则阴影是绑定姿势。
        // 沿父链取本实体的组件值（不能按 const Mesh* 查表 —— 同 FBX 的
        // 多个实例 Mesh 指针相同，会互相覆盖）。
        if (const auto* skc =
                scene.findInAncestors<ecs::SkinnedMeshComponent>(e)) {
            if (skc->jointBase >= 0) {
                push.skin = glm::vec4(static_cast<float>(skc->jointBase), 0.0f,
                                      1.0f, 0.0f);
            }
        }
        // morph（§15）：与级联阴影同一条路 —— push.which.z 带槽位。
        std::uint32_t slot = 0;
        if (morphSlot && mc.mesh->morphTargetCount() > 0)
            slot = morphSlot(scene, e, *mc.mesh);
        push.which.z = static_cast<float>(slot);
        vkCmdPushConstants(cmd, m_pipeline->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(ShadowPushConstants), &push);
        mc.mesh->drawInstanced(cmd, 1, slot);
    });

    m_considered[layer] = considered;
    m_submitted[layer] = submitted;

    vkCmdEndRenderPass(cmd);
}

} // namespace render
