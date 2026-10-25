#include "render/ShadowPass.h"

#include "rhi/Device.h"
#include "rhi/Pipeline.h"

#include "render/ShadowCull.h"
#include "render/ShaderPath.h"

#include "scene/Scene.h"
#include "ecs/Components.h"
#include "assets/Mesh.h"

#include <algorithm>
#include <cmath>

namespace render {

namespace {

// 线性视深度 → NDC z。glm::perspective 且**未**定义
// GLM_FORCE_DEPTH_ZERO_TO_ONE，所以 NDC z ∈ [-1, 1]。
// 与 pbr.frag / cluster_build.comp 的 ndcToDepth 互为逆运算：
//   ndcToDepth(z, n, f) = 2nf / ((n+f) - z(f-n))
float depthToNdc(float d, float n, float f) {
    return (d * (f + n) - 2.0f * f * n) / (d * (f - n));
}

} // namespace

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

void ShadowPass::setCascadeCount(uint32_t n) {
    m_cascadeCount = std::min(std::max(n, 1u), kMaxCascades);
}

uint32_t ShadowPass::submitted(uint32_t cascade) const {
    return cascade < kMaxCascades ? m_submitted[cascade] : 0;
}

uint32_t ShadowPass::considered(uint32_t cascade) const {
    return cascade < kMaxCascades ? m_considered[cascade] : 0;
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
    // 返回 0/1（是否被遮挡），省去着色器中的手工判断。
    // 同一把采样器数组与非数组都适用。
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
    // 每一级级联都走一次这个小 render pass，只是 framebuffer 挂的层不同。
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
    // 一层 = 一级级联。固定按 kMaxCascades 分配，级联数改小时只是不用它们，
    // 不必重建资源（改一次级联数就重分配几十 MB 实在太浪费）。
    ci.arrayLayers = kMaxCascades;
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

    // 采样用视图：4 层一起（着色器按层号选级联）
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = f.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = m_depthFormat;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = kMaxCascades;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &f.arrayView));

    // 渲染用视图：每层一个 2D 视图。
    // ⚠ 不能直接把 arrayView 挂进 framebuffer 就完事 —— 那会变成
    // "layered rendering"，需要 multiview / gl_Layer 把每个实例送到不同层。
    for (uint32_t c = 0; c < kMaxCascades; ++c) {
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

void ShadowPass::destroyFrame(Frame& f) {
    VkDevice dev = m_device.get();
    for (uint32_t c = 0; c < kMaxCascades; ++c) {
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

void ShadowPass::createPipeline() {
    rhi::PipelineDesc desc;
    desc.vertSpv = vkutil::readFile(resolveShaderPath("shadow.vert.spv"));
    desc.fragSpv = vkutil::readFile(resolveShaderPath("shadow.frag.spv"));
    desc.vertexBindings.push_back(assets::Vertex::bindingDescription());
    // 只喂 0..4：shadow.vert 不读顶点色（location 5）。多声明一个不会被读的
    // 属性会触发校验层 "Vertex attribute at location 5 not consumed by
    // vertex shader"。
    // 阴影读 pos/normal/uv/joints/weights + morph（§15）；不读顶点色。
    desc.vertexAttributes = assets::Vertex::attributeDescriptionsShadow();
    desc.setLayouts = {m_frameSetLayout};
    desc.pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    desc.pushConstant.offset = 0;
    // ⚠ 这里是 **ShadowPushConstants（96B）**，不是主前向的 128B
    // PushConstants。两者不同是允许的：本 Pass 在**自己的 render pass 内
    // 自行绑定 set 0**（不像前向 Pass 那样"绑一次 set 0 再切一堆管线"），
    // 绑定时和绘制时用的是同一个管线布局对象，不可能不兼容。
    // 反过来说：**不要**把这个 Pass 的绘制搬进前向 Pass 的 render pass 里，
    // 那会立刻踩到 VUID-...-08600。
    desc.pushConstant.size = sizeof(ShadowPushConstants);
    desc.renderPass = m_renderPass;
    desc.colorAttachmentCount = 0;  // depth-only
    desc.depthTest = true;
    desc.depthWrite = true;
    // 只渲染正面：剔除背面能显著减少 shadow acne
    desc.cullMode = VK_CULL_MODE_FRONT_BIT;
    desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // 深度偏移：把深度沿法线推开一点，抵消自阴影条纹。
    // 级联把正交深度范围拉长（远大于单张图时的 2r），所以这两个值比
    // 单级联时代略大。
    desc.depthBiasEnable = true;
    desc.depthBiasConstant = 1.6f;
    desc.depthBiasSlope = 2.2f;

    m_pipeline = std::make_unique<rhi::Pipeline>(m_device, desc);
}

void ShadowPass::updateCascades(const glm::mat4& camView,
                                const glm::mat4& camProj,
                                float camNear, float camFar,
                                const glm::vec3& lightDir, float maxDistance,
                                float lambda) {
    // 相机远平面之外的级联毫无意义（那里根本没有可见像素），
    // 直接夹住 —— 不夹的话远级联的半径会被白白撑大，近处的精度跟着掉。
    m_maxDistance = std::min(std::max(maxDistance, 1.0f),
                             camFar > 1.0f ? camFar : 1.0f);
    const uint32_t n = m_cascadeCount;

    // ---- 1) 分割点：log 与 uniform 的插值（PSSM 的 practical scheme）----
    //   纯对数：近处极密、远处极疏，但第 0 级会小到几乎没有意义；
    //   纯均匀：远处精度不够。lambda 默认 0.6 略偏均匀。
    const float nf = camNear > 1e-4f ? camNear : 1e-4f;
    float splits[kMaxCascades + 1];
    splits[0] = nf;
    for (uint32_t i = 1; i <= n; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(n);
        const float logS = nf * std::pow(m_maxDistance / nf, f);
        const float uniS = nf + (m_maxDistance - nf) * f;
        splits[i] = lambda * logS + (1.0f - lambda) * uniS;
    }
    for (uint32_t i = 0; i < n; ++i) m_splits[i] = splits[i + 1];
    // 级联数不足 4 时把多余分量填成最后一级的值：着色器会按级联数截断，
    // 不会读到它们，但填满能避免调试时看到一堆 0 而误判。
    for (uint32_t i = n; i < kMaxCascades; ++i) m_splits[i] = splits[n];

    const glm::mat4 invViewProj = glm::inverse(camProj * camView);
    const glm::vec3 d = glm::normalize(lightDir);

    // 光方向接近垂直时 lookAt 会退化（up 与视线共线），换用另一个轴
    glm::vec3 up(0.0f, 0.0f, 1.0f);  // Z-up
    if (std::abs(glm::dot(d, up)) > 0.99f) up = glm::vec3(0.0f, 1.0f, 0.0f);

    for (uint32_t c = 0; c < n; ++c) {
        // ---- 2) 视锥切片的 8 个世界角点 ----
        const float z0 = depthToNdc(splits[c], nf, camFar);
        const float z1 = depthToNdc(splits[c + 1], nf, camFar);
        glm::vec3 corners[8];
        int k = 0;
        for (int zi = 0; zi < 2; ++zi) {
            const float zn = (zi == 0) ? z0 : z1;
            for (int yi = 0; yi < 2; ++yi) {
                for (int xi = 0; xi < 2; ++xi) {
                    const glm::vec4 p =
                        invViewProj * glm::vec4((xi == 0) ? -1.0f : 1.0f,
                                                (yi == 0) ? -1.0f : 1.0f,
                                                zn, 1.0f);
                    corners[k++] = glm::vec3(p) / p.w;
                }
            }
        }

        // ---- 3) 包围球 ----
        // 用球而不是盒：球的半径与光方向无关，光一转也不变 → 阴影边缘
        // 不会随光照角度"呼吸"。盒的半径会随方向变，必然抖。
        glm::vec3 mid(0.0f);
        for (const glm::vec3& p : corners) mid += p;
        mid /= 8.0f;
        float radius = 0.0f;
        for (const glm::vec3& p : corners) {
            radius = glm::max(radius, glm::length(p - mid));
        }
        // 退化保护：切片极小或数值异常时，给一个下限免得 texel 归一化除零
        radius = glm::max(radius, 0.05f);

        // ---- 4) 光空间视图 + 纹素量化 ----
        const float texel = 2.0f * radius / static_cast<float>(m_resolution);

        // 量化必须在**光空间里对球心的世界位置**做，不能对 `view * mid`
        // 做 —— `mid` 是 lookAt 的 target，lookAt 之后它必然落在视轴原点，
        // `view*mid == (0,0,-dist)` 恒成立，量化一个常量等于什么都没做
        // （这正是 2026-10-13 发现的 bug：开关看起来接线了、日志也在打，
        // 但阴影边缘照样连续滑动）。
        //
        // 正确做法：先用"只含旋转"的光空间基把球心变换到光轴坐标系
        // （Rx = 视矩阵的左上 3x3 的转置，即世界→光轴方向），对它的
        // xy 量化到纹素网格，得到量化后的**世界位置** center，再用它重建
        // lookAt。这样"相机在纹素内移动"时，center 逐位不变 → 整个
        // lightSpace 矩阵逐位不变 → 阴影落点相对世界完全不动。
        glm::vec3 center = mid;
        if (m_texelSnapping) {
            // 光轴基（与 lookAt 内部一致：z=视线方向，x=up×z，y=z×x）
            const glm::vec3 lz = d;
            const glm::vec3 lx = glm::normalize(glm::cross(up, lz));
            const glm::vec3 ly = glm::cross(lz, lx);
            // 世界 → 光轴坐标系（点乘即投影）
            const float ax = glm::dot(mid, lx);
            const float ay = glm::dot(mid, ly);
            const float sx = std::floor(ax / texel) * texel;
            const float sy = std::floor(ay / texel) * texel;
            center = mid + lx * (sx - ax) + ly * (sy - ay);
        }
        const glm::vec3 eye = center - d * (radius + kDepthPadding);
        const glm::mat4 view = glm::lookAt(eye, center, up);
        // 近平面取 0：正交投影的深度是线性的，牺牲的那点精度可以忽略，
        // 换来的是"光源到切片之间的所有遮挡体都进深度图"。
        const glm::mat4 proj = glm::orthoRH_ZO(
            -radius, radius, -radius, radius, 0.0f,
            2.0f * (radius + kDepthPadding));
        m_lightSpace[c] = proj * view;
    }
    for (uint32_t c = n; c < kMaxCascades; ++c) {
        m_lightSpace[c] = m_lightSpace[n - 1];
    }
}

void ShadowPass::render(VkCommandBuffer cmd, scene::Scene& scene,
                        VkDescriptorSet frameSet, uint32_t frameIndex,
                        uint32_t cascade, MorphSlotFn morphSlot) {
    if (m_frames.empty() || cascade >= m_cascadeCount) return;
    Frame& f = m_frames[frameIndex % m_frames.size()];

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_renderPass;
    // framebuffer 挂的是**这一层**的 2D 视图 → CLEAR 只清这一层，
    // 各级之间互不干扰
    rp.framebuffer = f.framebuffer[cascade];
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

    const glm::mat4& clip = m_lightSpace[cascade];
    uint32_t considered = 0;
    uint32_t submitted = 0;

    // 只渲染投射阴影的物体（VisibilityComponent::castShadow）
    scene.forEachShadowCaster([&](ecs::Entity e, ecs::MeshComponent& mc) {
        if (!mc.mesh || !mc.mesh->valid()) return;
        ++considered;

        const glm::mat4 world = scene.worldMatrix(e);
        // 每一级各剔除一次：级联的正交盒只覆盖一个视锥切片，绝大多数
        // 投影体在每一级里都在盒外。不剔的话提交量就是全场景 × 级联数。
        if (mc.mesh->hasBounds() &&
            !visibleInLightClip(clip * world, mc.mesh->boundsMin(),
                                mc.mesh->boundsMax())) {
            return;
        }
        ++submitted;

        ShadowPushConstants push{};
        push.model = world;
        // which.y = 0 → shadow.vert 从 FrameUBO.lightSpace[x] 取矩阵
        push.which = glm::vec4(static_cast<float>(cascade), 0.0f, 0.0f, 0.0f);
        // 蒙皮网格：jointBase 必须和主前向路径一致，否则阴影用的是
        // 绑定姿势而不是当前姿势（角色一动影子就散架）。
        //
        // 沿父链取**本实体**的 SkinnedMeshComponent：不能按 `const Mesh*`
        // 查表 —— 同一 FBX 实例化两次时 Mesh 指针完全相同，第二个实例会
        // 覆盖第一个（阴影也跟着串姿势）。
        if (const auto* skc =
                scene.findInAncestors<ecs::SkinnedMeshComponent>(e)) {
            if (skc->jointBase >= 0) {
                push.skin = glm::vec4(static_cast<float>(skc->jointBase), 0.0f,
                                      1.0f, 0.0f);
            }
        }
        // morph（§15）：槽位由 Renderer 现场分配（同一个 MorphRig::evaluate，
        // 所以影子与实体的形变必然一致）。which.z 走 push —— 阴影的 push
        // 只有 96 B，which.z/w 一直是空闲的，不必加宽。
        std::uint32_t slot = 0;
        if (morphSlot && mc.mesh->morphTargetCount() > 0)
            slot = morphSlot(scene, e, *mc.mesh);
        push.which.z = static_cast<float>(slot);
        vkCmdPushConstants(cmd, m_pipeline->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(ShadowPushConstants), &push);
        mc.mesh->drawInstanced(cmd, 1, slot);
    });

    m_considered[cascade] = considered;
    m_submitted[cascade] = submitted;

    vkCmdEndRenderPass(cmd);
}

} // namespace render
