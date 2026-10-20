#include "render/DecalBuffer.h"

#include "render/OffscreenImage.h"
#include "render/RenderTypes.h"
#include "render/ShaderPath.h"

#include "assets/Mesh.h"

#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"
#include "rhi/Pipeline.h"

#include <algorithm>
#include <cstring>

namespace render {

DecalBuffer::DecalBuffer(rhi::Device& device, rhi::CommandPool& commandPool,
                         uint32_t framesInFlight)
    : m_device(device),
      m_commandPool(commandPool),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    // ---- set 0：FrameUBO + 场景深度 + 贴花记录 SSBO ----
    // 与主链的 set 0 分开一份：这里要的深度图是**逐帧**的 HdrTarget 视图，
    // 而主链 set 0 里没有"场景深度"这个 binding（SSAO 等也各持一份）。
    // 自己持有布局的另一个好处：push 范围可以只有 4 字节（主链是 128），
    // 不需要为了"set 0 兼容"去凑那一整块。
    {
        VkDescriptorSetLayoutBinding b[3]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                          VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[2].binding = 2;
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[2].descriptorCount = 1;
        // 顶点阶段要 rec.world（盒体的世界矩阵），片元阶段要全部字段
        b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                          VK_SHADER_STAGE_FRAGMENT_BIT;
        m_frameSetLayout = rhi::makeSetLayout(
            m_device, std::vector<VkDescriptorSetLayoutBinding>(b, b + 3));
    }

    // ---- set 1：逐贴花的两张贴图 ----
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        m_texSetLayout = rhi::makeSetLayout(
            m_device, std::vector<VkDescriptorSetLayoutBinding>(b, b + 2));
    }

    // DBuffer 给前向 Pass 采样：线性 + CLAMP_TO_EDGE。CLAMP 是必需的 ——
    // 贴花边缘的像素做双线性时会采到 [0,1] 之外，REPEAT 会把对边的贴花
    // 拖过来（屏幕右侧出现左侧贴花的鬼影）。
    {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_sampler));

        // 深度图：NEAREST。插值两个不同表面的深度会造出**不存在**的
        // 中间深度，重建出来的世界坐标就落在空气里，盒内测试随之乱掉。
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_depthSampler));

        // 贴花贴图：线性 + REPEAT（贴花可以平铺；uv 在同一个盒体内不越界，
        // 但用户可能配一张小图当花纹）。
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_texSampler));
    }

    // 盒体几何：单位立方体（[-0.5, 0.5]³，法线朝外，UV 用不上）。
    // 顶点/索引布局与主链完全一致（assets::Vertex），所以管线能直接复用
    // Vertex::bindingDescription()。
    m_cubeMesh = std::make_unique<assets::Mesh>();
    m_cubeMesh->upload(m_device, m_commandPool, assets::makeCubeVertices(1.0f),
                       assets::makeCubeIndices());

    createRenderPass();

    // 贴花记录缓冲（每帧一块，常驻映射）
    m_records.reserve(m_framesInFlight);
    m_recordsMapped.resize(m_framesInFlight, nullptr);
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        m_records.push_back(std::make_unique<rhi::Buffer>(
            m_device, sizeof(DecalRecord) * kMaxDecals,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        m_recordsMapped[i] = m_records.back()->map();
    }

    // 描述符池：每帧 1 个 frame set + kMaxDecals 个贴图 set
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        m_device, m_framesInFlight * (1 + kMaxDecals),
        m_framesInFlight,                              // UBO
        m_framesInFlight * (1 + kMaxDecals * 2),       // 采样器
        m_framesInFlight,                              // SSBO
        0);
}

DecalBuffer::~DecalBuffer() {
    m_device.waitIdle();

    // ⚠⚠ 必须真的**销毁**三张 DBuffer，不能只 clear() vector。
    //   一个 DecalFrame 带着 image + view + memory + framebuffer 四个裸句柄，
    //   `m_frames.clear()` 只析构 std::vector 的**元素**（成员是裸 handle，
    //   析构什么都不做）→ 这些对象永远不会被 vkDestroy*。
    //   3 张图 × 2 帧 = 6 image + 6 view + 6 memory + 2 framebuffer = **20 个**。
    //   而 DecalBuffer 是**无条件构造**的（不在 MYVK_DECALS 的判断里），
    //   所以这个泄漏与"贴花开不开"无关，每次运行都会在 vkDestroyDevice()
    //   时被验证层点名。
    //   对照：VolumetricFog / SSAOPass 的析构都规规矩矩调了 destroyFrames()，
    //   这里当初漏了 —— 由 verify_collision_play（全套里**唯一**让编辑器
    //   优雅退出、因而唯一能看到 vkDestroyDevice 报告的那个用例）抓到。
    destroyFrames();
    m_texSets.clear();
    m_frameSets.clear();
    m_records.clear();
    m_descriptors.reset();
    m_cubeMesh.reset();
    m_pipeline.reset();

    VkDevice dev = m_device.get();
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(dev, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    if (m_texSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_texSetLayout, nullptr);
        m_texSetLayout = VK_NULL_HANDLE;
    }
    if (m_frameSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_frameSetLayout, nullptr);
        m_frameSetLayout = VK_NULL_HANDLE;
    }
    if (m_texSampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_texSampler, nullptr);
        m_texSampler = VK_NULL_HANDLE;
    }
    if (m_depthSampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_depthSampler, nullptr);
        m_depthSampler = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
}

void DecalBuffer::createRenderPass() {
    // 三个颜色附件，一律：
    //   UNDEFINED 进 + LOAD_OP_CLEAR 清成 (0,0,0,0) → "这里没有贴花"
    //   （initialLayout=UNDEFINED 让上一帧残留的 SHADER_READ_ONLY 状态
    //     不构成约束 —— 内容本来就要被清掉）
    //   SHADER_READ_ONLY 出 → 前向 Pass 直接采样
    const VkFormat formats[3] = {kAlbedoFormat, kNormalFormat,
                                 kRoughMetalFormat};
    VkAttachmentDescription attaches[3]{};
    for (int i = 0; i < 3; ++i) {
        attaches[i].format = formats[i];
        attaches[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attaches[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attaches[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attaches[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attaches[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attaches[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attaches[i].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkAttachmentReference refs[3]{};
    for (int i = 0; i < 3; ++i) {
        refs[i].attachment = static_cast<uint32_t>(i);
        refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 3;
    subpass.pColorAttachments = refs;
    // 没有深度附件：遮挡关系由 decal.frag 的"重建点是否在盒内"决定
    // （理由见头文件）。
    subpass.pDepthStencilAttachment = nullptr;

    VkSubpassDependency deps[2]{};
    // 外部 → 子过程（WAR）：等上一帧对这三张图的采样读完
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // 子过程 → 外部：写完转 SHADER_READ_ONLY 且对片元可见（前向 Pass 要采）
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    // 顶点阶段也要读深度？不 —— 只有片元重建世界坐标时用（binding 1 的
    // stageFlags 就是 FRAGMENT）。

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 3;
    ci.pAttachments = attaches;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(m_device.get(), &ci, nullptr, &m_renderPass));
}

void DecalBuffer::recreate(VkExtent2D extent, VkSampleCountFlagBits samples,
                           VkFormat depthFormat) {
    (void)depthFormat;  // 深度图由 HdrTarget 持有，这里只记采样数（选 .spv）
    m_device.waitIdle();

    destroyFrames();
    m_descriptors.reset();

    m_extent = extent;
    m_msaa = samples != VK_SAMPLE_COUNT_1_BIT;
    if (m_extent.width == 0) m_extent.width = 1;
    if (m_extent.height == 0) m_extent.height = 1;

    createFrames();
    createPipelines();

    // 描述符池随帧数固定，重建后重新分配
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        m_device, m_framesInFlight * (1 + kMaxDecals), m_framesInFlight,
        m_framesInFlight * (1 + kMaxDecals * 2), m_framesInFlight, 0);

    m_frameSets.resize(m_framesInFlight, VK_NULL_HANDLE);
    m_texSets.assign(m_framesInFlight, {});
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        m_frameSets[i] = m_descriptors->allocate(m_frameSetLayout);
        m_descriptors->writeBuffer(m_frameSets[i], 2, m_records[i]->get(),
                                   m_records[i]->size(),
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        m_texSets[i].resize(kMaxDecals, VK_NULL_HANDLE);
        for (uint32_t d = 0; d < kMaxDecals; ++d)
            m_texSets[i][d] = m_descriptors->allocate(m_texSetLayout);
    }

    m_ready = true;
}

void DecalBuffer::createFrames() {
    m_frames.resize(m_framesInFlight);
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    for (auto& f : m_frames) {
        createImage2D(m_device, m_extent.width, m_extent.height, kAlbedoFormat,
                      usage, VK_IMAGE_ASPECT_COLOR_BIT, f.albedoImage,
                      f.albedoMemory, f.albedoView);
        createImage2D(m_device, m_extent.width, m_extent.height, kNormalFormat,
                      usage, VK_IMAGE_ASPECT_COLOR_BIT, f.normalImage,
                      f.normalMemory, f.normalView);
        createImage2D(m_device, m_extent.width, m_extent.height,
                      kRoughMetalFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT,
                      f.rmImage, f.rmMemory, f.rmView);

        VkImageView views[3] = {f.albedoView, f.normalView, f.rmView};
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = m_renderPass;
        fb.attachmentCount = 3;
        fb.pAttachments = views;
        fb.width = m_extent.width;
        fb.height = m_extent.height;
        fb.layers = 1;
        VK_CHECK(vkCreateFramebuffer(m_device.get(), &fb, nullptr, &f.fb));
    }
}

void DecalBuffer::destroyFrames() {
    for (auto& f : m_frames) {
        if (f.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device.get(), f.fb, nullptr);
            f.fb = VK_NULL_HANDLE;
        }
        destroyImage2D(m_device, f.albedoImage, f.albedoMemory, f.albedoView);
        destroyImage2D(m_device, f.normalImage, f.normalMemory, f.normalView);
        destroyImage2D(m_device, f.rmImage, f.rmMemory, f.rmView);
    }
    m_frames.clear();
}

void DecalBuffer::createPipelines() {
    rhi::PipelineDesc desc;
    desc.vertSpv = vkutil::readFile(resolveShaderPath("decal.vert.spv"));
    desc.fragSpv = vkutil::readFile(
        resolveShaderPath(m_msaa ? "decal.msaa.frag.spv" : "decal.frag.spv"));
    // 顶点布局与主链一致（同一个 assets::Vertex），盒体网格直接绑定。
    desc.vertexBindings.push_back(assets::Vertex::bindingDescription());
    // 贴花盒用的是 decal.vert，同样不读顶点色（location 5）。
    desc.vertexAttributes = assets::Vertex::attributeDescriptions(5);
    desc.setLayouts = {m_frameSetLayout, m_texSetLayout};
    desc.pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT;
    desc.pushConstant.offset = 0;
    desc.pushConstant.size = sizeof(uint32_t);  // 贴花记录下标
    desc.renderPass = m_renderPass;
    desc.samples = VK_SAMPLE_COUNT_1_BIT;  // DBuffer 是单采样
    desc.colorAttachmentCount = 3;
    // 没有深度附件 → 深度测试/写入必须都关（否则验证层报"管线要深度
    // 附件而 RenderPass 没有"）。
    desc.depthTest = false;
    desc.depthWrite = false;
    // 画**正面**（cull back）：凸体的正面投影 == 整个盒体的投影，
    // 所以"表面点在盒内"的像素一定被覆盖到。
    desc.cullMode = VK_CULL_MODE_BACK_BIT;
    desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // 混合：颜色按 alpha 混、覆盖度按 over 累加（三张图用同一套规则 ——
    // PipelineDesc 只支持一套，而这三张恰好要的也是同一套）。
    desc.blendEnable = true;
    desc.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    desc.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    desc.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    desc.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    m_pipeline = std::make_unique<rhi::Pipeline>(m_device, desc);
}

void DecalBuffer::setDepthViews(const std::vector<VkImageView>& views) {
    if (!m_descriptors) return;
    for (size_t i = 0; i < m_frameSets.size() && i < views.size(); ++i) {
        m_descriptors->writeTexture(m_frameSets[i], 1, m_depthSampler, views[i],
                                    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    }
}

void DecalBuffer::setDefaultTextures(VkImageView albedoView,
                                     VkSampler albedoSampler,
                                     VkImageView normalView,
                                     VkSampler normalSampler) {
    m_defaultAlbedoView = albedoView;
    m_defaultAlbedoSampler = albedoSampler;
    m_defaultNormalView = normalView;
    m_defaultNormalSampler = normalSampler;
}

void DecalBuffer::upload(VkBuffer frameUbo, uint32_t frameIndex,
                         const std::vector<DecalDrawItem>& items) {
    if (!m_ready || frameIndex >= m_records.size()) return;

    m_uploadedTotal = static_cast<uint32_t>(items.size());
    uint32_t n = static_cast<uint32_t>(items.size());
    if (n > kMaxDecals) n = kMaxDecals;
    m_count = n;

    // ---- 记录 SSBO ----
    auto* out = static_cast<DecalRecord*>(m_recordsMapped[frameIndex]);
    for (uint32_t i = 0; i < n; ++i) {
        const DecalDrawItem& it = items[i];
        DecalRecord& r = out[i];
        r.world = it.world;
        // ⚠ 用 DecalDrawItem 里算好的那份（= inverse(不含 size 缩放的 TRS)），
        //   **不是** glm::inverse(it.world)。后者会把 size 缩放一起反掉，
        //   于是片元里的 local 变成单位立方体坐标，和按米写的盒内测试 /
        //   uv 投影对不上（见 DecalBuffer.h 的字段注释）。
        r.worldToLocal = it.worldToLocal;
        r.tintOpacity = glm::vec4(it.tint, it.opacity);
        r.sizeEdge = glm::vec4(it.size, it.edgeFade);
        r.params = glm::vec4(it.roughness, it.metallic, it.normalScale, 0.0f);
        r.fade = glm::vec4(it.fadeStart, it.fadeEnd, 0.0f, 0.0f);
    }

    // ---- set 0 ----
    // binding 0 = 该帧的全局 UBO（和主链共用同一块 buffer，只是换了个
    // 描述符集对象）；范围写 FrameUBO 的实际大小。
    m_descriptors->writeBuffer(m_frameSets[frameIndex], 0, frameUbo,
                               sizeof(FrameUBO),
                               VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    // （binding 2 的记录缓冲在 recreate 里写死，内容每帧原地覆盖，不必重写）

    // ---- set 1：逐贴花的 albedo / normal ----
    for (uint32_t i = 0; i < n; ++i) {
        const DecalDrawItem& it = items[i];
        VkDescriptorSet set = m_texSets[frameIndex][i];
        const VkImageView av =
            it.albedoView ? it.albedoView : m_defaultAlbedoView;
        const VkSampler as =
            it.albedoSampler ? it.albedoSampler : m_defaultAlbedoSampler;
        const VkImageView nv = it.normalView ? it.normalView : m_defaultNormalView;
        const VkSampler ns =
            it.normalSampler ? it.normalSampler : m_defaultNormalSampler;
        m_descriptors->writeTexture(set, 0, as, av);
        m_descriptors->writeTexture(set, 1, ns, nv);
    }
}

void DecalBuffer::render(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!m_ready || frameIndex >= m_frames.size()) return;
    const DecalFrame& f = m_frames[frameIndex];

    // 清成 "coverage = 0" → 前向 Pass 的贴花分支整段跳过。
    // 没有贴花时**照样要清**：不清的话上一帧的内容会留在里面
    // （initialLayout=UNDEFINED 只是允许丢弃，实际内容仍在内存里）。
    VkClearValue clears[3]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clears[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clears[2].color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_renderPass;
    rp.framebuffer = f.fb;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = m_extent;
    rp.clearValueCount = 3;
    rp.pClearValues = clears;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.width = static_cast<float>(m_extent.width);
    vp.height = static_cast<float>(m_extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = m_extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    if (m_count > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipeline->get());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeline->layout(), 0, 1,
                                &m_frameSets[frameIndex], 0, nullptr);
        m_cubeMesh->bindGeometry(cmd);
        for (uint32_t i = 0; i < m_count; ++i) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipeline->layout(), 1, 1,
                                    &m_texSets[frameIndex][i], 0, nullptr);
            vkCmdPushConstants(cmd, m_pipeline->layout(),
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(uint32_t), &i);
            m_cubeMesh->draw(cmd);
        }
    }

    vkCmdEndRenderPass(cmd);
}

VkImageView DecalBuffer::view(uint32_t frameIndex, int which) const {
    if (frameIndex >= m_frames.size()) return VK_NULL_HANDLE;
    const DecalFrame& f = m_frames[frameIndex];
    switch (which) {
        case 0: return f.albedoView;
        case 1: return f.normalView;
        default: return f.rmView;
    }
}

} // namespace render
