#include "render/ReflectionProbe.h"

#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/ComputePipeline.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"

#include "render/HdrTarget.h"
#include "render/ShaderPath.h"

#include "core/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace render {

namespace {

constexpr uint32_t kCubeFaces = 6;
constexpr uint32_t kGroupSize = 8;
uint32_t groupCount(uint32_t n) { return (n + kGroupSize - 1) / kGroupSize; }

// 回读自检用的半精度 → float。选 16F 是因为它必须与捕获目标同格式
// （vkCmdCopyImage 要求源/目标格式一致），所以回读出来就是 half。
float halfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;  // ±0
        } else {
            // 次正规数：规格化
            uint32_t e = 127u - 15u + 1u;
            uint32_t m = man;
            while ((m & 0x400u) == 0) {
                m <<= 1u;
                --e;
            }
            bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (man << 13);  // Inf / NaN
    } else {
        bits = sign | ((exp - 15u + 127u) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void transitionImage(VkCommandBuffer cmd, VkImage image,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                     VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                         &b);
}

} // namespace

// ============================================================
// 构造 / 析构
// ============================================================

ReflectionProbeSystem::ReflectionProbeSystem(rhi::Device& device,
                                             rhi::CommandPool& cmdPool,
                                             const ProbeSettings& settings,
                                             uint32_t framesInFlight)
    : m_device(device), m_cmdPool(cmdPool), m_settings(settings),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    m_settings.maxProbes = std::max<uint32_t>(m_settings.maxProbes, 1u);
    m_settings.captureSize =
        std::max<uint32_t>(static_cast<uint32_t>(m_settings.captureSize), 16u);
    m_settings.prefilterSize =
        std::max<uint32_t>(m_settings.prefilterSize, 8u);
    m_settings.prefilterMips =
        std::max<uint32_t>(m_settings.prefilterMips, 1u);
    m_settings.prefilterSamples =
        std::max<uint32_t>(m_settings.prefilterSamples, 16u);

    m_dataByteSize = static_cast<uint32_t>(sizeof(ProbeGpu)) *
                     m_settings.maxProbes;
    m_stats.probeCount = 0;
    m_stats.captureSize = m_settings.captureSize;
    m_stats.prefilterSize = m_settings.prefilterSize;
    m_stats.mips = m_settings.prefilterMips;

    m_readbackEnabled = std::getenv("MYVK_PROBE_DBG") != nullptr;

    createFallback();
}

ReflectionProbeSystem::~ReflectionProbeSystem() {
    VkDevice dev = m_device.get();
    m_device.waitIdle();

    m_prefPipe.reset();
    destroyResources();

    if (m_fallback.view != VK_NULL_HANDLE)
        vkDestroyImageView(dev, m_fallback.view, nullptr);
    if (m_fallback.image != VK_NULL_HANDLE)
        vkDestroyImage(dev, m_fallback.image, nullptr);
    if (m_fallback.memory != VK_NULL_HANDLE)
        vkFreeMemory(dev, m_fallback.memory, nullptr);
    if (m_fallback.sampler != VK_NULL_HANDLE)
        vkDestroySampler(dev, m_fallback.sampler, nullptr);

    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(dev, m_sampler, nullptr);
    if (m_prefSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, m_prefSetLayout, nullptr);
    if (m_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_pool, nullptr);
}

// ---------------------------------------------------------------- 缺省资源

// 未启用时绑在 binding 23 的那张 1×1 cube array（6 层，全黑）。
//
// 为什么不能"不绑"：pbr.frag 里声明了 binding 23，只要 layout 里有这个
// 资源，创建**任何**用 pbr.frag 的管线时都必须有一个有效的描述符 ——
// 否则验证层报 VUID-VkGraphicsPipelineCreateInfo-layout-07988。
// 与 SSAO 的缺省白图 / 贴花的全透明图是同一条既有约定。
void ReflectionProbeSystem::createFallback() {
    VkDevice dev = m_device.get();

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = HdrTarget::kColorFormat;
    ci.extent = {1, 1, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = kCubeFaces;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, nullptr, &m_fallback.image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, m_fallback.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &m_fallback.memory));
    VK_CHECK(vkBindImageMemory(dev, m_fallback.image, m_fallback.memory, 0));

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_fallback.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    vi.format = HdrTarget::kColorFormat;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = kCubeFaces;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &m_fallback.view));

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &m_fallback.sampler));

    // 转到只读：这张图永远不会被写，但描述符声明的是 SHADER_READ_ONLY，
    // 让它一开始就处于该布局，验证层在任何情况下都不会抱怨"布局不符"。
    {
        VkCommandBuffer cmd = m_cmdPool.beginOneTime();
        transitionImage(cmd, m_fallback.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT);
        m_cmdPool.endAndSubmit(cmd);
        m_device.waitIdle();
    }
}

// ---------------------------------------------------------------- 图像

void ReflectionProbeSystem::createCube(Image& img, uint32_t size, uint32_t mips,
                                       uint32_t layers,
                                       VkImageUsageFlags extraUsage) {
    VkDevice dev = m_device.get();
    img.format = HdrTarget::kColorFormat;
    img.size = size;
    img.mips = mips;
    img.layers = layers;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = img.format;
    ci.extent = {size, size, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, nullptr, &img.image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, img.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &img.memory));
    VK_CHECK(vkBindImageMemory(dev, img.image, img.memory, 0));

    // 采样视图：6 层 → CUBE；多于 6 层 → CUBE_ARRAY
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = (layers == kCubeFaces) ? VK_IMAGE_VIEW_TYPE_CUBE
                                         : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    vi.format = img.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = layers;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &img.sampleView));

    // 写入视图：每 mip 一个 2D_ARRAY（imageStore 定位不了 mip）。
    // 每个 mip 再为**每个 6 层切片**建一个窄视图 —— 派发一次只写一个探针，
    // 让 shader 的 z = 面号（0..5）保持简单，不必知道自己在第几个探针。
    img.storageViews.resize(mips, VK_NULL_HANDLE);
    img.probeStorageViews.resize(static_cast<size_t>(mips) * (layers / kCubeFaces),
                                VK_NULL_HANDLE);
    for (uint32_t m = 0; m < mips; ++m) {
        VkImageViewCreateInfo si = vi;
        si.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        si.subresourceRange.baseMipLevel = m;
        si.subresourceRange.levelCount = 1;
        si.subresourceRange.baseArrayLayer = 0;
        si.subresourceRange.layerCount = layers;
        VK_CHECK(vkCreateImageView(dev, &si, nullptr, &img.storageViews[m]));

        for (uint32_t p = 0; p < layers / kCubeFaces; ++p) {
            VkImageViewCreateInfo pi = si;
            pi.subresourceRange.baseArrayLayer = p * kCubeFaces;
            pi.subresourceRange.layerCount = kCubeFaces;
            VK_CHECK(vkCreateImageView(
                dev, &pi, nullptr,
                &img.probeStorageViews[static_cast<size_t>(p) * mips + m]));
        }
    }
}

void ReflectionProbeSystem::destroyImage(Image& img) {
    VkDevice dev = m_device.get();
    for (VkImageView v : img.storageViews) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
    }
    img.storageViews.clear();
    for (VkImageView v : img.probeStorageViews) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
    }
    img.probeStorageViews.clear();
    if (img.sampleView != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, img.sampleView, nullptr);
        img.sampleView = VK_NULL_HANDLE;
    }
    if (img.image != VK_NULL_HANDLE) {
        vkDestroyImage(dev, img.image, nullptr);
        img.image = VK_NULL_HANDLE;
    }
    if (img.memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, img.memory, nullptr);
        img.memory = VK_NULL_HANDLE;
    }
    img.storageViews.clear();
}

// ---------------------------------------------------------------- 资源

bool ReflectionProbeSystem::ensureResources() {
    if (m_ready && !m_needRebuild) return true;
    destroyResources();
    m_needRebuild = false;

    createPrefilterPipeline();

    // ① 捕获立方体：6 层，单 mip。TRANSFER_DST 供逐面拷贝，
    //    TRANSFER_SRC 供（可选的）回读自检。
    createCube(m_capture, m_settings.captureSize, 1, kCubeFaces,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    // ② 预过滤结果：cube array，层 = 6 × maxProbes，含完整 mip 链。
    createCube(m_prefilter, m_settings.prefilterSize, m_settings.prefilterMips,
               kCubeFaces * m_settings.maxProbes, VK_IMAGE_USAGE_STORAGE_BIT);

    // 采样器：三线性 + 边缘钳制（探针盒体边界处的方向可能略微超出面范围）
    if (m_sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = static_cast<float>(m_settings.prefilterMips);
        VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_sampler));
    }

    // ③ 捕获用的正方形离屏目标。**采样数必须与主视口一致** ——
    //    前向管线是按主 HdrTarget 的 renderpass 建的，两边的附件格式 /
    //    采样数 / load-store 全一样时 render pass 才算兼容（可复用管线）。
    m_captureTarget = std::make_unique<HdrTarget>(
        m_device, 1, m_captureSamples);
    m_captureTarget->recreate(m_settings.captureSize, m_settings.captureSize);

    createDescriptors();

    // 预过滤结果图**永久停在 GENERAL**：它既作 storage image（compute 写）
    // 又作 sampled cube array（片元读），GENERAL 是唯一对两种用途都合法的
    // 布局。换成 SHADER_READ_ONLY 会逼出"每次捕获都要转两次布局"，而
    // 且 UNDEFINED 起始的迁移会丢掉其它探针已经烘好的层。
    {
        VkCommandBuffer cmd = m_cmdPool.beginOneTime();
        transitionImage(cmd, m_prefilter.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT);
        m_cmdPool.endAndSubmit(cmd);
        m_device.waitIdle();
    }

    m_ready = true;
    VK_LOG_INFO("PROBE-RES capture=%ux%u prefilter=%u mips=%u maxProbes=%u",
                m_settings.captureSize, m_settings.captureSize,
                m_settings.prefilterSize, m_settings.prefilterMips,
                m_settings.maxProbes);
    return true;
}

void ReflectionProbeSystem::destroyResources() {
    if (!m_ready) {
        // 即使没 ready 也要保证管线/布局没漏（重建路径会走到这里）
    }
    VkDevice dev = m_device.get();
    m_device.waitIdle();

    for (auto& d : m_data) {
        if (d.buffer) {
            d.buffer->unmap();
            d.buffer.reset();
        }
        d.mapped = nullptr;
    }
    m_data.clear();

    if (m_readback) {
        m_readback->unmap();
        m_readback.reset();
    }
    m_readbackMapped = nullptr;
    m_readbackPending = false;

    m_prefSets.clear();
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }

    m_captureTarget.reset();
    destroyImage(m_prefilter);
    destroyImage(m_capture);

    m_probeCount = 0;
    m_ready = false;
}

// ---------------------------------------------------------------- 管线

void ReflectionProbeSystem::createPrefilterPipeline() {
    if (m_prefPipe) return;

    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    m_prefSetLayout =
        rhi::makeSetLayout(m_device, {binds[0], binds[1]});

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset = 0;
    pc.size = sizeof(glm::vec4);

    // 与 Environment 用的是**同一个** ibl_prefilter.comp —— GGX 预过滤
    // 的算法只允许有一份实现，否则"I B L 的粗糙反射"与"探针的粗糙反射"
    // 会悄悄分叉。
    m_prefPipe = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("ibl_prefilter.comp.spv")),
        std::vector<VkDescriptorSetLayout>{m_prefSetLayout}, &pc);
}

void ReflectionProbeSystem::createDescriptors() {
    VkDevice dev = m_device.get();

    const uint32_t probes = m_settings.maxProbes;
    const uint32_t mips = m_settings.prefilterMips;
    const uint32_t setCount = probes * mips;

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = setCount;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[1].descriptorCount = setCount;

    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = setCount;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &pci, nullptr, &m_pool));

    m_prefSets.resize(setCount, VK_NULL_HANDLE);
    for (uint32_t p = 0; p < probes; ++p) {
        for (uint32_t m = 0; m < mips; ++m) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = m_pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &m_prefSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &set));
            m_prefSets[static_cast<size_t>(p) * mips + m] = set;

            // binding 0：捕获立方体（所有探针共用同一张，捕获是串行的）
            VkDescriptorImageInfo src{};
            src.sampler = m_sampler;
            src.imageView = m_capture.sampleView;
            src.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // binding 1：本探针本 mip 的 6 层写入视图
            VkDescriptorImageInfo dst{};
            dst.sampler = VK_NULL_HANDLE;
            dst.imageView =
                m_prefilter.probeStorageViews[static_cast<size_t>(p) * mips + m];
            dst.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet w[2]{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = set;
            w[0].dstBinding = 0;
            w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[0].pImageInfo = &src;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = set;
            w[1].dstBinding = 1;
            w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[1].pImageInfo = &dst;
            vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
        }
    }

    // 探针数据 SSBO（每帧在途一块）
    m_data.resize(m_framesInFlight);
    for (auto& d : m_data) {
        d.buffer = std::make_unique<rhi::Buffer>(
            m_device, m_dataByteSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        d.mapped = d.buffer->map();
        std::memset(d.mapped, 0, m_dataByteSize);
    }

    // 回读缓冲（只在 MYVK_PROBE_DBG 下建）
    if (m_readbackEnabled) {
        const size_t bytes = static_cast<size_t>(kCubeFaces) *
                             m_settings.captureSize * m_settings.captureSize * 8;
        m_readback = std::make_unique<rhi::Buffer>(
            m_device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        m_readbackMapped = m_readback->map();
    }
}

// ---------------------------------------------------------------- 每帧上传

void ReflectionProbeSystem::upload(const std::vector<ProbeGpu>& probes,
                                   uint32_t frameIndex) {
    m_probeCount = std::min<uint32_t>(static_cast<uint32_t>(probes.size()),
                                      m_settings.maxProbes);
    m_stats.probeCount = m_probeCount;
    if (!m_ready) return;

    DataSlot& slot = m_data[frameIndex % m_data.size()];
    std::memset(slot.mapped, 0, m_dataByteSize);
    if (m_probeCount > 0)
        std::memcpy(slot.mapped, probes.data(),
                    sizeof(ProbeGpu) * m_probeCount);
}

VkBuffer ReflectionProbeSystem::dataBuffer(uint32_t frameIndex) const {
    return m_data[frameIndex % m_data.size()].buffer->get();
}

VkExtent2D ReflectionProbeSystem::captureExtent() const {
    const uint32_t s = static_cast<uint32_t>(m_settings.captureSize);
    return {s, s};
}

// ---------------------------------------------------------------- 捕获

void ReflectionProbeSystem::beginCapture(VkCommandBuffer cmd, uint32_t probe) {
    (void)probe;  // 捕获立方体是所有探针共享的，这里不需要区分
    if (!m_ready) return;
    // oldLayout = UNDEFINED：6 个面马上都会被覆盖，旧内容不重要。
    transitionImage(cmd, m_capture.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
}

void ReflectionProbeSystem::copyFace(VkCommandBuffer cmd, uint32_t face) {
    if (!m_ready || !m_captureTarget) return;
    const uint32_t cs = m_settings.captureSize;

    // 源：本帧刚渲染完的 resolve 图（前向 renderpass 的 finalLayout 就是
    // SHADER_READ_ONLY，所以这里从它迁到 TRANSFER_SRC）。
    transitionImage(cmd, m_captureTarget->resolveImage(0),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.mipLevel = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource = region.srcSubresource;
    region.dstSubresource.baseArrayLayer = face;
    region.extent = {cs, cs, 1};
    vkCmdCopyImage(cmd, m_captureTarget->resolveImage(0),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_capture.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void ReflectionProbeSystem::finishCapture(VkCommandBuffer cmd, uint32_t probe) {
    if (!m_ready) return;
    constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    // 拷贝写完 → 预过滤要采样它
    transitionImage(cmd, m_capture.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, kCompute,
                    VK_ACCESS_SHADER_READ_BIT);

    const uint32_t mips = m_settings.prefilterMips;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mipSize =
            std::max<uint32_t>(m_settings.prefilterSize >> m, 1u);
        const float roughness =
            mips > 1 ? static_cast<float>(m) / static_cast<float>(mips - 1)
                     : 0.0f;
        // 与 Environment 的预过滤**逐字同一组推常量**（源边长 = 捕获边长，
        // 不是 prefilterSize —— mip 选择依赖源图的立体角）。
        const glm::vec4 push(roughness,
                             static_cast<float>(m_settings.captureSize),
                             static_cast<float>(m_settings.prefilterSamples),
                             static_cast<float>(m));
        const uint32_t g = groupCount(mipSize);
        VkDescriptorSet set =
            m_prefSets[static_cast<size_t>(probe) * mips + m];
        m_prefPipe->dispatch(cmd, g, g, kCubeFaces, &set, 1, &push,
                             sizeof(push));

        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, kCompute, kCompute, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
    }

    // 回读自检：把捕获立方体拷到主机可见缓冲。结果在**下一帧**解析
    // （fence 之后），与 Hi-Z / GPU 时戳同一条"延迟回读"约定。
    if (m_readbackEnabled && m_readback) {
        transitionImage(cmd, m_capture.image,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kCompute,
                        VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
        // 逐面拷贝，**不复用**一个 layerCount=6 的 region。
        //
        // ⚠ 曾以为理由在"cube-compatible 图像的拷贝范围必须 6 层一组" ——
        //   这个说法**不成立**（标准立方体贴图加载器普遍逐层建 region，
        //   见 SaschaWillems 的 loader；R5-1 排查中已实测证伪）。真正的
        //   理由是显式给 bufferOffset / bufferRowLength / bufferImageHeight
        //   后，回读布局**不再依赖"紧打包"这个隐式约定**，一个 region 一面、
        //   偏移自己算 —— 读回是自检用的，宁可啰嗦也别让布局约定成为又一个
        //   "偶尔对偶尔错"的来源。
        //
        //   （顺带记录：R5-1 里"只有最后一个面有内容"的真因不在拷贝，
        //   而在渲染阶段的相机 UBO 复用，见 Renderer::kProbeFaceUboSlotBase。）
        const uint32_t cs = static_cast<uint32_t>(m_settings.captureSize);
        for (uint32_t f = 0; f < kCubeFaces; ++f) {
            VkBufferImageCopy region{};
            region.bufferOffset = static_cast<VkDeviceSize>(f) * cs * cs * 8ull;
            region.bufferRowLength = cs;
            region.bufferImageHeight = cs;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = f;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {cs, cs, 1};
            vkCmdCopyImageToBuffer(cmd, m_capture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   m_readback->get(), 1, &region);
        }
        transitionImage(cmd, m_capture.image,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, kCompute,
                        VK_ACCESS_SHADER_READ_BIT);
        m_readbackPending = true;
    }
}

void ReflectionProbeSystem::pollReadback() {
    if (!m_readbackPending || !m_readbackMapped) return;
    m_readbackPending = false;

    const uint32_t cs = m_settings.captureSize;
    const auto* data = static_cast<const uint16_t*>(m_readbackMapped);
    for (uint32_t f = 0; f < kCubeFaces; ++f) {
        const size_t base =
            static_cast<size_t>(f) * cs * cs * 4u;  // RGBA16F = 4 个 half
        double sum = 0.0;
        uint32_t nz = 0;
        for (uint32_t i = 0; i < cs * cs; ++i) {
            const uint16_t* texel = data + base + static_cast<size_t>(i) * 4u;
            const float r = halfToFloat(texel[0]);
            const float g = halfToFloat(texel[1]);
            const float b = halfToFloat(texel[2]);
            sum += 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) +
                   0.0722 * static_cast<double>(b);
            // 1e-3 而不是 == 0：半精度下 0.0001 级别的噪声不该算"有内容"。
            if (r > 1e-3f || g > 1e-3f || b > 1e-3f) ++nz;
        }
        m_stats.faceLum[f] =
            static_cast<float>(sum / static_cast<double>(cs * cs));
        m_stats.faceNonZero[f] = nz;

        // 中心纹素（uv = 0.5）。cube 面在 Vulkan 里的 2D 布局与本引擎的
        // 面序无关，但"中心 = 视轴方向"这一点与布局无关 —— 所以它是
        // 判断"第 f 个面到底朝哪儿"的最短路径。
        const size_t mid =
            base + static_cast<size_t>((cs / 2u) * cs + (cs / 2u)) * 4u;
        m_stats.faceMid[f] = glm::vec3(halfToFloat(data[mid + 0]),
                                       halfToFloat(data[mid + 1]),
                                       halfToFloat(data[mid + 2]));
    }
    m_stats.readbackValid = true;
}

} // namespace render
