#include "render/VolumetricFog.h"

#include "render/OffscreenImage.h"
#include "render/ScreenSpaceSettings.h"
#include "render/ShaderPath.h"

#include "rhi/CommandPool.h"
#include "rhi/ComputePipeline.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"

#include "core/Logger.h"

#include <cstring>

namespace render {

namespace {

// froxel 网格基尺寸（在 1920×1080 下的"参考"分辨率）。实际尺寸 =
// 屏幕尺寸 / kFroxelTile。z 层数固定（与屏幕无关）。
constexpr uint32_t kFroxelTile = 16;      // 屏幕每 16px 一个 froxel 瓦片
constexpr uint32_t kFroxelDepth = 64;     // 深度层数

constexpr VkFormat kFogFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

} // namespace

VolumetricFog::VolumetricFog(rhi::Device& device, rhi::CommandPool& cmdPool,
                             VkDescriptorSetLayout globalSetLayout,
                             uint32_t framesInFlight)
    : m_device(device), m_cmdPool(cmdPool), m_setLayout(globalSetLayout),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    m_frames.resize(m_framesInFlight);
    m_injectSets.assign(m_framesInFlight, VK_NULL_HANDLE);
    m_integrateSets.assign(m_framesInFlight, VK_NULL_HANDLE);
    m_storageViews.assign(m_framesInFlight, VK_NULL_HANDLE);

    // 采样器：3D 图给前向 pbr.frag 用线性采样（雾是低频信号，
    // 双线性插值让瓦片边界不出现方块）。
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_sampler));

    // 深度图最近邻（插值深度会造出不存在的中间深度）
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_depthSampler));

    // 缺省"透明"图采样器（最近邻即可）
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_neutralSampler));

    createNeutralImage();
    createPipelines(false);
}

VolumetricFog::~VolumetricFog() {
    m_device.waitIdle();
    m_injectPipeline.reset();
    m_integratePipeline.reset();
    destroyFrames();

    VkDevice dev = m_device.get();
    if (m_neutralView != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, m_neutralView, nullptr);
        m_neutralView = VK_NULL_HANDLE;
    }
    if (m_neutralImage != VK_NULL_HANDLE) {
        vkDestroyImage(dev, m_neutralImage, nullptr);
        m_neutralImage = VK_NULL_HANDLE;
    }
    if (m_neutralMemory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, m_neutralMemory, nullptr);
        m_neutralMemory = VK_NULL_HANDLE;
    }
    if (m_localSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_localSetLayout, nullptr);
        m_localSetLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(dev, m_sampler, nullptr);
    if (m_depthSampler != VK_NULL_HANDLE)
        vkDestroySampler(dev, m_depthSampler, nullptr);
    if (m_neutralSampler != VK_NULL_HANDLE)
        vkDestroySampler(dev, m_neutralSampler, nullptr);
}

void VolumetricFog::createNeutralImage() {
    VkDevice dev = m_device.get();
    // 1×1×1 的 rgba16f = (0,0,0,1)：T=1、inscatter=0 → 雾合成退化成
    // out = lit，与基线逐位一致。
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = kFogFormat;
    ci.extent = {1, 1, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImage(dev, &ci, nullptr, &img));
    m_neutralImage = img;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, img, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &mem));
    VK_CHECK(vkBindImageMemory(dev, img, mem, 0));
    m_neutralMemory = mem;

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = kFogFormat;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &m_neutralView));

    // 清成 (0,0,0,1)。用 clear 命令把 image 直接置位。
    VkCommandBuffer cmd = m_cmdPool.beginOneTime();
    // 先转到 TRANSFER_DST
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);

    VkClearColorValue cc{};
    cc.float32[0] = 0.0f;
    cc.float32[1] = 0.0f;
    cc.float32[2] = 0.0f;
    cc.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1,
                         &range);

    // 转到 SHADER_READ_ONLY（前向采样）
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    m_cmdPool.endAndSubmit(cmd);

    // 图与内存交给进程生命周期（极小，无需每帧管理）。
    // 不保留句柄回收 —— 引擎退出时 OS 回收，与缺省贴图同款处理。
}

void VolumetricFog::createPipelines(bool msaaDepth) {
    m_msaaDepth = msaaDepth;
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset = 0;
    pc.size = sizeof(PushConstants);

    // set 0 = 全局集；set 1 = 本模块私有（深度图 + storage image）
    VkDescriptorSetLayoutBinding b0{};
    b0.binding = 0;
    b0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b0.descriptorCount = 1;
    b0.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding b1{};
    b1.binding = 1;
    b1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b1.descriptorCount = 1;
    b1.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    if (m_localSetLayout == VK_NULL_HANDLE) {
        m_localSetLayout = rhi::makeSetLayout(m_device, {b0, b1});
    }

    // 深度图 MSAA 时选 .msaa.comp.spv（sampler2DMS），否则默认那份。
    const std::string injectSpv =
        msaaDepth ? "fog_inject.msaa.comp.spv" : "fog_inject.comp.spv";

    m_injectPipeline = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath(injectSpv)),
        std::vector<VkDescriptorSetLayout>{m_setLayout, m_localSetLayout}, &pc);
    m_integratePipeline = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("fog_integrate.comp.spv")),
        std::vector<VkDescriptorSetLayout>{m_setLayout, m_localSetLayout}, &pc);
}

void VolumetricFog::destroyFrames() {
    m_device.waitIdle();
    for (auto& f : m_frames) {
        if (f.view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device.get(), f.view, nullptr);
            f.view = VK_NULL_HANDLE;
        }
        if (f.image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device.get(), f.image, nullptr);
            f.image = VK_NULL_HANDLE;
        }
        if (f.memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device.get(), f.memory, nullptr);
            f.memory = VK_NULL_HANDLE;
        }
    }
    m_storageViews.assign(m_frames.size(), VK_NULL_HANDLE);
    m_descriptors.reset();
}

void VolumetricFog::recreate(VkExtent2D screenExtent,
                             VkSampleCountFlagBits samples) {
    const bool msaa = (samples != VK_SAMPLE_COUNT_1_BIT);

    // 采样数变化 → 深度图类型变了 → 必须换 .spv 重建管线。
    // 3D 图与描述符集也要重建（深度视图会换）。
    if (msaa != m_msaaDepth) {
        createPipelines(msaa);
    }

    destroyFrames();

    m_screenExtent = screenExtent;
    const uint32_t fx = (screenExtent.width + kFroxelTile - 1) / kFroxelTile;
    const uint32_t fy = (screenExtent.height + kFroxelTile - 1) / kFroxelTile;
    m_grid = glm::uvec3(fx > 0 ? fx : 1, fy > 0 ? fy : 1, kFroxelDepth);

    createFrames();

    VK_LOG_INFO("VolumetricFog: froxel grid %ux%ux%u (screen %ux%u, msaa=%d)",
                m_grid.x, m_grid.y, m_grid.z, screenExtent.width,
                screenExtent.height, msaa ? 1 : 0);
}

void VolumetricFog::createFrames() {
    const uint32_t fx = m_grid.x, fy = m_grid.y, fz = m_grid.z;

    // 描述符池：每帧 2 个 set。第一个 set 需要 1 采样图 + 1 storage image。
    const uint32_t maxSets = m_framesInFlight * 2u;
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        m_device, maxSets, 0, m_framesInFlight + 2u, 0,
        m_framesInFlight * 2u);

    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        auto& f = m_frames[i];

        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_3D;
        ci.format = kFogFormat;
        ci.extent = {fx, fy, fz};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(m_device.get(), &ci, nullptr, &f.image));

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_device.get(), f.image, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = m_device.findMemoryType(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(m_device.get(), &ai, nullptr, &f.memory));
        VK_CHECK(vkBindImageMemory(m_device.get(), f.image, f.memory, 0));

        // 采样视图（前向用）：image3D
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = f.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vi.format = kFogFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(m_device.get(), &vi, nullptr, &f.view));
        m_storageViews[i] = f.view;  // storage 与 sampled 共用同一 image3D 视图

        // 转到 GENERAL（compute 里 imageStore/imageLoad 要求）
        VkCommandBuffer cmd = m_cmdPool.beginOneTime();
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = f.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        m_cmdPool.endAndSubmit(cmd);
    }

    // 每帧的两套 set：initialize 时都绑同一张图（注入趟写、积分趟
    // 读+写同一张 —— 靠 render() 里的 image barrier 分隔）。
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        m_injectSets[i] = m_descriptors->allocate(m_localSetLayout);
        m_integrateSets[i] = m_descriptors->allocate(m_localSetLayout);
        m_descriptors->writeStorageImage(m_injectSets[i], 1, m_storageViews[i]);
        m_descriptors->writeStorageImage(m_integrateSets[i], 1,
                                         m_storageViews[i]);
    }
}

void VolumetricFog::setDepthViews(const std::vector<VkImageView>& views) {
    if (!m_descriptors) return;
    for (uint32_t i = 0; i < m_framesInFlight && i < views.size(); ++i) {
        if (m_injectSets[i] == VK_NULL_HANDLE) continue;
        m_descriptors->writeTexture(m_injectSets[i], 0, m_depthSampler,
                                    views[i],
                                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    // 就绪判据：有图 + 有管线
    m_ready = (m_grid.x > 0) && (m_injectPipeline != nullptr) &&
              !views.empty() && (views[0] != VK_NULL_HANDLE);
}

void VolumetricFog::render(VkCommandBuffer cmd, uint32_t frameIndex,
                           const ScreenSpaceSettings& s,
                           VkDescriptorSet globalSet) {
    if (!m_ready || !m_frames.size()) return;

    const uint32_t fi = frameIndex % m_framesInFlight;
    auto& f = m_frames[fi];

    PushConstants pc{};
    pc.grid = glm::uvec4(m_grid, 0u);
    pc.params = glm::vec4(s.vfogDensity, s.vfogAlbedo, s.vfogHeightFalloff,
                          s.vfogBlobStrength);
    pc.params2 = glm::vec4(m_sliceNear, m_zNear, m_zFar, s.vfogPhaseG);

    // ---- 趟 1：注入 ----
    {
        const uint32_t gx = (m_grid.x + 3u) / 4u;
        const uint32_t gy = (m_grid.y + 3u) / 4u;
        const uint32_t gz = (m_grid.z + 3u) / 4u;
        VkDescriptorSet sets[2] = {globalSet, m_injectSets[fi]};
        m_injectPipeline->dispatch(cmd, gx, gy, gz, sets, 2, &pc, sizeof(pc));
    }

    // 注入 → 积分：image barrier（同一 image，写后读）
    {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = f.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    }

    // ---- 趟 2：积分 ----
    {
        const uint32_t gx = (m_grid.x + 7u) / 8u;
        const uint32_t gy = (m_grid.y + 7u) / 8u;
        VkDescriptorSet sets[2] = {globalSet, m_integrateSets[fi]};
        m_integratePipeline->dispatch(cmd, gx, gy, 1, sets, 2, &pc, sizeof(pc));
    }

    // 积分 → 前向：让 3D 图的写对片元采样可见
    {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = f.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
}

VkImageView VolumetricFog::view(uint32_t frameIndex) const {
    return m_frames[frameIndex % m_framesInFlight].view;
}

} // namespace render
