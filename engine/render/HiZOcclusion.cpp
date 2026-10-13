#include "render/HiZOcclusion.h"

#include "render/ShaderPath.h"

#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/ComputePipeline.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"

#include "core/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace render {

namespace {

// 与 hiz_build.comp 的 push_constant 一致（16B）
struct HiZBuildPush {
    int32_t srcW = 0;
    int32_t srcH = 0;
    int32_t samples = 1;
    int32_t pad = 0;
};

// 与 hiz_downsample.comp 的 push_constant 一致（16B）
struct HiZDownPush {
    int32_t srcW = 0;
    int32_t srcH = 0;
    int32_t pad0 = 0;
    int32_t pad1 = 0;
};

// 与 occlusion_test.comp 的 push_constant 一致（96B）
struct HiZTestPush {
    glm::mat4 viewProj{1.0f};
    glm::vec4 params{0.0f};   // x=物体数 y=深度偏置 z=最大 mip w=保留
    glm::vec4 screen{0.0f};   // x=宽 y=高 z=1/宽 w=1/高
};
static_assert(sizeof(HiZTestPush) == 96, "occlusion_test.comp 的 push 常量布局变了");

constexpr VkFormat kHizFormat = VK_FORMAT_R32_SFLOAT;
// 金字塔最大级数：2^15 的边长已经远超任何现实分辨率
constexpr uint32_t kMaxMipLevels = 16;

} // namespace

HiZOcclusion::HiZOcclusion(rhi::Device& device, rhi::CommandPool& cmdPool,
                           uint32_t framesInFlight)
    : m_device(device), m_cmdPool(cmdPool),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    createPipelines();
}

HiZOcclusion::~HiZOcclusion() {
    m_buildPipeline.reset();
    m_buildPipelineMsaa.reset();
    m_downPipeline.reset();
    m_testPipeline.reset();
    destroyDescriptorSets();
    destroyFrames();

    VkDevice dev = m_device.get();
    if (m_pointSampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_pointSampler, nullptr);
        m_pointSampler = VK_NULL_HANDLE;
    }
    if (m_downSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_downSetLayout, nullptr);
        m_downSetLayout = VK_NULL_HANDLE;
    }
    if (m_testSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_testSetLayout, nullptr);
        m_testSetLayout = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------- 管线

void HiZOcclusion::createPipelines() {
    // ---- 采样器：Hi-Z 用 NEAREST（textureLod 显式指定层，不需要过滤；
    //      开了 LINEAR 反而会跨 mip 混，破坏保守性）----
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f;
    si.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_pointSampler));

    // ---- set 布局（降采样 / 建 mip0 共用）：binding0 采样图、binding1 存储图 ----
    {
        std::vector<VkDescriptorSetLayoutBinding> b(2);
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        m_downSetLayout = rhi::makeSetLayout(m_device, b);
    }
    // ---- 测试：binding0 Hi-Z、binding1 AABB SSBO、binding2 vis SSBO ----
    {
        std::vector<VkDescriptorSetLayoutBinding> b(3);
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
        m_testSetLayout = rhi::makeSetLayout(m_device, b);
    }

    VkPushConstantRange pcBuild{};
    pcBuild.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcBuild.offset = 0;
    pcBuild.size = sizeof(HiZBuildPush);

    VkPushConstantRange pcTest{};
    pcTest.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcTest.offset = 0;
    pcTest.size = sizeof(HiZTestPush);

    const std::vector<VkDescriptorSetLayout> downLayouts{m_downSetLayout};
    const std::vector<VkDescriptorSetLayout> testLayouts{m_testSetLayout};

    m_buildPipeline = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("hiz_build.comp.spv")),
        downLayouts, &pcBuild);
    m_buildPipelineMsaa = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("hiz_build.msaa.comp.spv")),
        downLayouts, &pcBuild);
    m_downPipeline = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("hiz_downsample.comp.spv")),
        downLayouts, &pcBuild);
    m_testPipeline = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("occlusion_test.comp.spv")),
        testLayouts, &pcTest);
}

// ---------------------------------------------------------------- 资源

void HiZOcclusion::recreate(VkExtent2D extent, VkSampleCountFlagBits samples) {
    m_device.waitIdle();
    destroyDescriptorSets();
    destroyFrames();

    m_extent.width = extent.width > 0 ? extent.width : 1;
    m_extent.height = extent.height > 0 ? extent.height : 1;
    m_msaa = samples != VK_SAMPLE_COUNT_1_BIT;
    m_sampleCount = static_cast<uint32_t>(samples);

    const uint32_t maxDim = std::max(m_extent.width, m_extent.height);
    m_mipCount = 1;
    while ((1u << m_mipCount) <= maxDim && m_mipCount < kMaxMipLevels) ++m_mipCount;

    createFrames();
    buildDescriptorSets();
    m_ready = true;

    VK_LOG_INFO("HiZOcclusion: %ux%u, %u mips, depth format %s (%u samples)",
                m_extent.width, m_extent.height, m_mipCount,
                m_msaa ? "MSAA" : "single", m_sampleCount);
}

void HiZOcclusion::createFrames() {
    m_frames.resize(m_framesInFlight);
    VkDevice dev = m_device.get();

    const VkDeviceSize aabbBytes =
        static_cast<VkDeviceSize>(kMaxObjects) * sizeof(HiZAabb);
    const VkDeviceSize visBytes =
        static_cast<VkDeviceSize>(kMaxObjects) * sizeof(uint32_t);

    for (auto& f : m_frames) {
        // ---- Hi-Z 金字塔：R32_SFLOAT，全 mip 链，GENERAL（存储图像的唯一可用布局）----
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kHizFormat;
        ci.extent = {m_extent.width, m_extent.height, 1};
        ci.mipLevels = m_mipCount;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
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

        // 全 mip 的采样视图（test 用）
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = f.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kHizFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_mipCount, 0, 1};
        VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &f.sampleView));

        // 每 mip 一个单级视图（存储图像写用）
        f.mipViews.resize(m_mipCount, VK_NULL_HANDLE);
        for (uint32_t m = 0; m < m_mipCount; ++m) {
            VkImageViewCreateInfo mvi = vi;
            mvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
            VK_CHECK(vkCreateImageView(dev, &mvi, nullptr, &f.mipViews[m]));
        }

        // ---- 一次布局迁移 UNDEFINED → GENERAL（之后永久停在这）----
        {
            VkCommandBuffer cmd = m_cmdPool.beginOneTime();
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = f.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_mipCount, 0, 1};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                              VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
            m_cmdPool.endAndSubmit(cmd);
        }

        // ---- AABB：CPU 每帧写 → HOST_VISIBLE ----
        f.aabb = std::make_unique<rhi::Buffer>(
            m_device, aabbBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        // ---- vis：GPU 写 / CPU 读 → HOST_VISIBLE ----
        f.vis = std::make_unique<rhi::Buffer>(
            m_device, visBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        f.visMapped = f.vis->map();
        // 首帧还没被测过 —— 全部标"可见"，绝不能默认判成遮挡
        std::memset(f.visMapped, 1, static_cast<size_t>(visBytes));
        f.count = 0;
    }
}

void HiZOcclusion::destroyFrames() {
    VkDevice dev = m_device.get();
    for (auto& f : m_frames) {
        for (VkImageView v : f.mipViews) {
            if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
        }
        f.mipViews.clear();
        if (f.sampleView != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, f.sampleView, nullptr);
            f.sampleView = VK_NULL_HANDLE;
        }
        if (f.image != VK_NULL_HANDLE) {
            vkDestroyImage(dev, f.image, nullptr);
            f.image = VK_NULL_HANDLE;
        }
        if (f.memory != VK_NULL_HANDLE) {
            vkFreeMemory(dev, f.memory, nullptr);
            f.memory = VK_NULL_HANDLE;
        }
        f.visMapped = nullptr;
        f.vis.reset();
        f.aabb.reset();
        f.buildSet = VK_NULL_HANDLE;
        f.testSet = VK_NULL_HANDLE;
        f.downSets.clear();
        f.count = 0;
    }
    m_frames.clear();
}

void HiZOcclusion::destroyDescriptorSets() {
    m_descriptors.reset();
}

void HiZOcclusion::buildDescriptorSets() {
    const uint32_t n = static_cast<uint32_t>(m_frames.size());
    if (n == 0) return;

    const uint32_t samplersPerFrame = m_mipCount + 1;  // 1 build + (mips-1) down + 1 test
    const uint32_t setsPerFrame = m_mipCount + 1;

    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        m_device, n * setsPerFrame + 8, 0, n * samplersPerFrame + 8,
        n * 2 + 8,        // storage buffers：aabb + vis
        n * m_mipCount + 8);  // storage images：每 mip 一个（写）

    for (uint32_t i = 0; i < n; ++i) {
        auto& f = m_frames[i];

        // ---- build：binding0 = 深度（由 setDepthViews 填）、binding1 = mip0 ----
        f.buildSet = m_descriptors->allocate(m_downSetLayout);
        m_descriptors->writeStorageImage(f.buildSet, 1, f.mipViews[0]);

        // ---- 逐级降采样：src = mip(k-1)，dst = mip(k) ----
        f.downSets.resize(m_mipCount > 1 ? m_mipCount - 1 : 0, VK_NULL_HANDLE);
        for (uint32_t m = 1; m < m_mipCount; ++m) {
            VkDescriptorSet s = m_descriptors->allocate(m_downSetLayout);
            m_descriptors->writeTexture(s, 0, m_pointSampler, f.mipViews[m - 1],
                                        VK_IMAGE_LAYOUT_GENERAL);
            m_descriptors->writeStorageImage(s, 1, f.mipViews[m]);
            f.downSets[m - 1] = s;
        }

        // ---- test：Hi-Z + AABB + vis ----
        f.testSet = m_descriptors->allocate(m_testSetLayout);
        m_descriptors->writeTexture(f.testSet, 0, m_pointSampler, f.sampleView,
                                    VK_IMAGE_LAYOUT_GENERAL);
        m_descriptors->writeBuffer(f.testSet, 1, f.aabb->get(),
                                   static_cast<VkDeviceSize>(kMaxObjects) *
                                       sizeof(HiZAabb),
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        m_descriptors->writeBuffer(f.testSet, 2, f.vis->get(),
                                   static_cast<VkDeviceSize>(kMaxObjects) *
                                       sizeof(uint32_t),
                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
}

void HiZOcclusion::setDepthViews(const std::vector<VkImageView>& views) {
    if (!m_descriptors) return;
    const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(views.size()),
                                          static_cast<uint32_t>(m_frames.size()));
    for (uint32_t i = 0; i < n; ++i) {
        m_descriptors->writeTexture(m_frames[i].buildSet, 0, m_pointSampler,
                                    views[i],
                                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
}

// ---------------------------------------------------------------- 每帧

void HiZOcclusion::uploadAabbs(uint32_t frameIndex,
                               const std::vector<HiZAabb>& boxes) {
    if (m_frames.empty()) return;
    auto& f = m_frames[frameIndex % m_frames.size()];
    if (!f.aabb) return;

    uint32_t n = static_cast<uint32_t>(boxes.size());
    if (n > kMaxObjects) n = kMaxObjects;
    if (n > 0) {
        std::memcpy(f.aabb->map(), boxes.data(),
                    static_cast<size_t>(n) * sizeof(HiZAabb));
    }
    f.count = n;
}

uint32_t HiZOcclusion::uploadedCount(uint32_t frameIndex) const {
    if (m_frames.empty()) return 0;
    return m_frames[frameIndex % m_frames.size()].count;
}

VkBuffer HiZOcclusion::aabbBuffer(uint32_t frameIndex) const {
    if (m_frames.empty()) return VK_NULL_HANDLE;
    const auto& f = m_frames[frameIndex % m_frames.size()];
    return f.aabb ? f.aabb->get() : VK_NULL_HANDLE;
}

void HiZOcclusion::beginFrame(uint32_t frameIndex) {
    m_visFlags.clear();
    if (m_frames.empty()) return;
    auto& f = m_frames[frameIndex % m_frames.size()];
    if (!f.visMapped) return;

    // 调用点在等待该槽 in-flight 栅栏之后 → GPU 已不再访问这个缓冲。
    const uint32_t n = f.count;
    const auto* flags = static_cast<const uint32_t*>(f.visMapped);
    m_visFlags.resize(n);
    uint32_t occluded = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const bool visible = flags[i] != 0u;
        m_visFlags[i] = visible ? 1u : 0u;
        if (!visible) ++occluded;
    }
    m_stats.objects = n;
    m_stats.occluded = occluded;
    m_stats.mipCount = m_mipCount;
    m_stats.extentW = m_extent.width;
    m_stats.extentH = m_extent.height;
}

void HiZOcclusion::record(VkCommandBuffer cmd, uint32_t frameIndex,
                          VkImage depthImage, const glm::mat4& viewProj,
                          float depthBias) {
    recordPyramid(cmd, frameIndex, depthImage);
    recordTest(cmd, frameIndex, viewProj, depthBias);
}

void HiZOcclusion::recordPyramid(VkCommandBuffer cmd, uint32_t frameIndex,
                                 VkImage depthImage) {
    if (!m_ready || !m_buildPipeline || m_frames.empty() || m_mipCount == 0) {
        return;
    }
    auto& f = m_frames[frameIndex % m_frames.size()];

    // ---- 深度图：前向 pass 的附件写 → compute 采样的内存依赖 ----
    // HdrTarget 的 subpass 依赖只把深度写同步到 **片元** 阶段（屏幕空间
    // 效果都是片元 pass），compute 阶段要自己补一条。布局不变（一直在
    // DEPTH_READ_ONLY），所以 old/new 都是同一个布局。
    if (depthImage != VK_NULL_HANDLE) {
        VkImageMemoryBarrier ib{};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = depthImage;
        ib.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        ib.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &ib);
    }

    // compute 之间需要一个写→读的内存屏障（Hi-Z 逐级依赖）。
    // 用粗粒度的 VkMemoryBarrier 就够：每个 mip 的写量很小，多等一点不值一提。
    auto computeBarrier = [&]() {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    };

    // ---- 1) 深度 → mip0 ----
    {
        HiZBuildPush pc{};
        pc.srcW = static_cast<int32_t>(m_extent.width);
        pc.srcH = static_cast<int32_t>(m_extent.height);
        pc.samples = static_cast<int32_t>(m_sampleCount);
        const uint32_t gx = (m_extent.width + 7u) / 8u;
        const uint32_t gy = (m_extent.height + 7u) / 8u;
        rhi::ComputePipeline& p =
            m_msaa ? *m_buildPipelineMsaa : *m_buildPipeline;
        p.dispatch(cmd, gx, gy, 1, &f.buildSet, 1, &pc, sizeof(pc));
    }
    computeBarrier();

    // ---- 2) 逐级降采样 ----
    uint32_t w = m_extent.width;
    uint32_t h = m_extent.height;
    for (uint32_t m = 1; m < m_mipCount; ++m) {
        const uint32_t sw = w, sh = h;
        w = std::max(1u, w / 2u);
        h = std::max(1u, h / 2u);

        HiZDownPush pc{};
        pc.srcW = static_cast<int32_t>(sw);
        pc.srcH = static_cast<int32_t>(sh);
        const uint32_t gx = (w + 7u) / 8u;
        const uint32_t gy = (h + 7u) / 8u;
        m_downPipeline->dispatch(cmd, gx, gy, 1, &f.downSets[m - 1], 1, &pc,
                                 sizeof(pc));
        computeBarrier();
    }
}

void HiZOcclusion::recordTest(VkCommandBuffer cmd, uint32_t frameIndex,
                              const glm::mat4& viewProj, float depthBias) {
    if (!m_ready || !m_testPipeline || m_frames.empty() || m_mipCount == 0) {
        return;
    }
    auto& f = m_frames[frameIndex % m_frames.size()];

    // ---- 3) 遮挡测试 ----
    if (f.count > 0) {
        HiZTestPush pc{};
        pc.viewProj = viewProj;
        pc.params = glm::vec4(static_cast<float>(f.count), depthBias,
                              static_cast<float>(m_mipCount - 1), 0.0f);
        pc.screen = glm::vec4(static_cast<float>(m_extent.width),
                              static_cast<float>(m_extent.height),
                              1.0f / static_cast<float>(m_extent.width),
                              1.0f / static_cast<float>(m_extent.height));
        const uint32_t groups = (f.count + 63u) / 64u;
        m_testPipeline->dispatch(cmd, groups, 1, 1, &f.testSet, 1, &pc,
                                 sizeof(pc));

        // 让 GPU 对 vis 的写对主机可见（回读发生在两帧后的栅栏之后，
        // 但这条屏障才是规范要求的"writes available to host"）
        VkBufferMemoryBarrier bb{};
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = f.vis->get();
        bb.offset = 0;
        bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb,
                             0, nullptr);
    }
}

} // namespace render
