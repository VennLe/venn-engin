#include "render/Environment.h"

#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/ComputePipeline.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"

#include "render/ShaderPath.h"

#include "core/Logger.h"

#include <algorithm>
#include <cstring>

namespace render {

namespace {

// 立方体面数（Vulkan 的 +X,-X,+Y,-Y,+Z,-Z 六面）
constexpr uint32_t kCubeFaces = 6;
constexpr uint32_t kGroupSize = 8;

uint32_t groupCount(uint32_t n) { return (n + kGroupSize - 1) / kGroupSize; }

// 统一的一次性布局转换。这个类只在 build() 里跑一次，所以不用精算
// stage/access 的覆盖范围 —— 写清除、读清除，宁保守不宁激进。
void transitionImage(VkCommandBuffer cmd, VkImage image,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags srcStage,
                     VkAccessFlags srcAccess,
                     VkPipelineStageFlags dstStage,
                     VkAccessFlags dstAccess) {
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

void writeStorageImage(VkDevice device, VkDescriptorSet set, uint32_t binding,
                       VkImageView view) {
    VkDescriptorImageInfo info{};
    info.sampler = VK_NULL_HANDLE;
    info.imageView = view;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

void writeSampledCube(VkDevice device, VkDescriptorSet set, uint32_t binding,
                      VkSampler sampler, VkImageView view) {
    VkDescriptorImageInfo info{};
    info.sampler = sampler;
    info.imageView = view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}

} // namespace

// ============================================================
// 构造 / 析构
// ============================================================

Environment::Environment(rhi::Device& device, rhi::CommandPool& cmdPool,
                         const EnvSettings& settings)
    : m_device(device), m_cmdPool(cmdPool), m_settings(settings) {
    // 尺寸下限：太小的话卷积会把天空糊成一块常数
    m_settings.skySize = std::max<uint32_t>(m_settings.skySize, 16u);
    m_settings.irradianceSize = std::max<uint32_t>(m_settings.irradianceSize, 8u);
    m_settings.prefilterSize = std::max<uint32_t>(m_settings.prefilterSize, 16u);
    m_settings.lutSize = std::max<uint32_t>(m_settings.lutSize, 16u);
    m_settings.prefilterMips = std::max<uint32_t>(m_settings.prefilterMips, 1u);
}

Environment::~Environment() {
    VkDevice dev = m_device.get();
    m_device.waitIdle();

    // 管线要在设备还有效时销毁
    m_lutPipe.reset();
    m_prefPipe.reset();
    m_irrPipe.reset();
    m_skyPipe.reset();

    destroyImage(m_lut);
    destroyImage(m_pref);
    destroyImage(m_irr);
    destroyImage(m_sky);

    if (m_cubeSampler != VK_NULL_HANDLE)
        vkDestroySampler(dev, m_cubeSampler, nullptr);
    if (m_lutSampler != VK_NULL_HANDLE)
        vkDestroySampler(dev, m_lutSampler, nullptr);
    if (m_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_pool, nullptr);
    if (m_sourceLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, m_sourceLayout, nullptr);
    if (m_sinkLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, m_sinkLayout, nullptr);
}

// ============================================================
// 图像创建
// ============================================================

void Environment::createCube(Image& img, VkFormat format, uint32_t size,
                             uint32_t mips, bool readback) {
    VkDevice dev = m_device.get();
    img.format = format;
    img.size = size;
    img.mips = mips;
    img.cube = true;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    // 没有这个 flag 就不能建 CUBE 视图，采样时验证层会直接报
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {size, size, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = kCubeFaces;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (readback) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // CPU 回读自检
    ci.usage = usage;
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

    // 采样视图：CUBE（覆盖全部 mip）
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = kCubeFaces;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &img.sampleView));

    // 写入视图：每一级 mip 一个。imageStore 不能指定 mip 层级，
    // 所以只能靠"视图只暴露那一级"来定位。
    img.storageViews.resize(mips, VK_NULL_HANDLE);
    for (uint32_t m = 0; m < mips; ++m) {
        VkImageViewCreateInfo si = vi;
        si.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        si.subresourceRange.baseMipLevel = m;
        si.subresourceRange.levelCount = 1;
        VK_CHECK(vkCreateImageView(dev, &si, nullptr, &img.storageViews[m]));
    }
}

void Environment::create2D(Image& img, VkFormat format, uint32_t size,
                           bool readback) {
    VkDevice dev = m_device.get();
    img.format = format;
    img.size = size;
    img.mips = 1;
    img.cube = false;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {size, size, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (readback) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.usage = usage;
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

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &img.sampleView));

    img.storageViews.resize(1, VK_NULL_HANDLE);
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &img.storageViews[0]));
}

void Environment::destroyImage(Image& img) {
    VkDevice dev = m_device.get();
    for (VkImageView v : img.storageViews) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
    }
    img.storageViews.clear();
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
}

// ============================================================
// 描述符与管线
// ============================================================

void Environment::createDescriptors() {
    VkDevice dev = m_device.get();

    // ---- 布局 ----
    // sourceLayout：读一张 samplerCube + 写一张 storage image（两条卷积用）
    VkDescriptorSetLayoutBinding srcBindings[2]{};
    srcBindings[0].binding = 0;
    srcBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    srcBindings[0].descriptorCount = 1;
    srcBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    srcBindings[1].binding = 1;
    srcBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    srcBindings[1].descriptorCount = 1;
    srcBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    m_sourceLayout = rhi::makeSetLayout(m_device, {srcBindings[0], srcBindings[1]});

    // sinkLayout：只写（天空生成 / BRDF 表都没有输入）
    VkDescriptorSetLayoutBinding sinkBinding{};
    sinkBinding.binding = 0;
    sinkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sinkBinding.descriptorCount = 1;
    sinkBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    m_sinkLayout = rhi::makeSetLayout(m_device, {sinkBinding});

    // ---- 池 ----
    // 一次 build 需要的 set 数：天空 1 + 辐照度 1 + 预过滤 mips + LUT 1
    const uint32_t setCount = 3 + m_settings.prefilterMips;
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

    // ---- 分配 ----
    auto alloc = [&](VkDescriptorSetLayout layout) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &set));
        return set;
    };
    m_skySet = alloc(m_sinkLayout);
    m_irrSet = alloc(m_sourceLayout);
    m_prefSets.resize(m_settings.prefilterMips, VK_NULL_HANDLE);
    for (auto& s : m_prefSets) s = alloc(m_sourceLayout);
    m_lutSet = alloc(m_sinkLayout);

    // ---- 写入 ----
    // 输入侧永远指向天空立方体的采样视图；输出侧指向各自的 storage 视图。
    // （imageLayout 此时与实际布局不符是正常的：真正用到时命令缓冲里
    //   已经把图转到 GENERAL 了，描述符的 layout 只是"承诺"。）
    writeStorageImage(dev, m_skySet, 0, m_sky.storageViews[0]);

    writeSampledCube(dev, m_irrSet, 0, m_cubeSampler, m_sky.sampleView);
    writeStorageImage(dev, m_irrSet, 1, m_irr.storageViews[0]);

    for (uint32_t m = 0; m < m_settings.prefilterMips; ++m) {
        writeSampledCube(dev, m_prefSets[m], 0, m_cubeSampler, m_sky.sampleView);
        writeStorageImage(dev, m_prefSets[m], 1, m_pref.storageViews[m]);
    }

    writeStorageImage(dev, m_lutSet, 0, m_lut.storageViews[0]);
}

void Environment::createPipelines() {
    VkPushConstantRange skyPc{};
    skyPc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    skyPc.offset = 0;
    skyPc.size = 2 * sizeof(glm::vec4);   // zenith + ground

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset = 0;
    pc.size = sizeof(glm::vec4);

    m_skyPipe = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("sky_cube.comp.spv")),
        std::vector<VkDescriptorSetLayout>{m_sinkLayout}, &skyPc);

    m_irrPipe = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("ibl_irradiance.comp.spv")),
        std::vector<VkDescriptorSetLayout>{m_sourceLayout}, &pc);

    m_prefPipe = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("ibl_prefilter.comp.spv")),
        std::vector<VkDescriptorSetLayout>{m_sourceLayout}, &pc);

    m_lutPipe = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("ibl_brdf.comp.spv")),
        std::vector<VkDescriptorSetLayout>{m_sinkLayout}, &pc);
}

// ============================================================
// 生成
// ============================================================

void Environment::recordBuild(VkCommandBuffer cmd, VkBuffer skyReadback,
                              VkBuffer lutReadback) {
    constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    constexpr VkPipelineStageFlags kTransfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    constexpr VkPipelineStageFlags kTop = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    // ---- 1) 天空立方体 ----
    transitionImage(cmd, m_sky.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL, kTop, 0, kCompute,
                    VK_ACCESS_SHADER_WRITE_BIT);
    {
        struct {
            glm::vec4 zenith;
            glm::vec4 ground;
        } push;
        push.zenith = glm::vec4(m_settings.zenith, 1.0f);
        push.ground = glm::vec4(m_settings.ground, 1.0f);
        const uint32_t g = groupCount(m_settings.skySize);
        m_skyPipe->dispatch(cmd, g, g, kCubeFaces, &m_skySet, 1, &push,
                            sizeof(push));
    }
    // 后面的卷积要采样它 —— 转成只读
    transitionImage(cmd, m_sky.image, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute,
                    VK_ACCESS_SHADER_WRITE_BIT, kCompute,
                    VK_ACCESS_SHADER_READ_BIT);

    // ---- 2) 辐照度立方体 ----
    transitionImage(cmd, m_irr.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL, kTop, 0, kCompute,
                    VK_ACCESS_SHADER_WRITE_BIT);
    {
        const glm::vec4 push(m_settings.irradianceDelta, 0.0f, 0.0f, 0.0f);
        const uint32_t g = groupCount(m_settings.irradianceSize);
        m_irrPipe->dispatch(cmd, g, g, kCubeFaces, &m_irrSet, 1, &push,
                            sizeof(push));
    }
    transitionImage(cmd, m_irr.image, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute,
                    VK_ACCESS_SHADER_WRITE_BIT, kCompute,
                    VK_ACCESS_SHADER_READ_BIT);

    // ---- 3) 预过滤立方体（逐 mip）----
    transitionImage(cmd, m_pref.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL, kTop, 0, kCompute,
                    VK_ACCESS_SHADER_WRITE_BIT);
    for (uint32_t m = 0; m < m_settings.prefilterMips; ++m) {
        // mip m 的边长（不小于 1）；当边长不足一个 mip 链时截断
        const uint32_t mipSize =
            std::max<uint32_t>(m_settings.prefilterSize >> m, 1u);
        const float roughness = m_settings.prefilterMips > 1
                                    ? static_cast<float>(m) /
                                          static_cast<float>(
                                              m_settings.prefilterMips - 1)
                                    : 0.0f;
        // 粗糙度大的 mip 用少量样本就够（波瓣大、方差小），
        // 但这里样本数固定 —— 生成是一次性的，省这点时间不值得引入
        // "质量随 mip 变化"这种额外变量。
        const glm::vec4 push(roughness,
                             static_cast<float>(m_settings.skySize),
                             static_cast<float>(m_settings.prefilterSamples),
                             static_cast<float>(m));
        const uint32_t g = groupCount(mipSize);
        m_prefPipe->dispatch(cmd, g, g, kCubeFaces, &m_prefSets[m], 1, &push,
                             sizeof(push));

        // 逐 mip 插一个 barrier：写的是同一张图的不同 mip，本身不冲突，
        // 但这样能保证 WSI 顺序在任何驱动上都成立（一次性开销，不心疼）
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, kCompute, kCompute, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
    }
    transitionImage(cmd, m_pref.image, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute,
                    VK_ACCESS_SHADER_WRITE_BIT, kCompute,
                    VK_ACCESS_SHADER_READ_BIT);

    // ---- 4) BRDF 查找表 ----
    transitionImage(cmd, m_lut.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL, kTop, 0, kCompute,
                    VK_ACCESS_SHADER_WRITE_BIT);
    {
        const glm::vec4 push(static_cast<float>(m_settings.brdfSamples), 0.0f,
                             0.0f, 0.0f);
        const uint32_t g = groupCount(m_settings.lutSize);
        m_lutPipe->dispatch(cmd, g, g, 1, &m_lutSet, 1, &push, sizeof(push));
    }
    transitionImage(cmd, m_lut.image, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute,
                    VK_ACCESS_SHADER_WRITE_BIT, kCompute,
                    VK_ACCESS_SHADER_READ_BIT);

    // ---- 5) 回读（自检用）----
    // 天空立方体是 RGBA32F、LUT 是 RG32F —— 都是 32 位浮点，
    // 回读后能直接当 float 数组读，不需要半精度转换（这正是选这两个
    // 格式而不是 16F 的原因）。
    transitionImage(cmd, m_sky.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kCompute,
                    VK_ACCESS_SHADER_READ_BIT, kTransfer,
                    VK_ACCESS_TRANSFER_READ_BIT);
    transitionImage(cmd, m_lut.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kCompute,
                    VK_ACCESS_SHADER_READ_BIT, kTransfer,
                    VK_ACCESS_TRANSFER_READ_BIT);
    {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = kCubeFaces;
        region.imageExtent = {m_settings.skySize, m_settings.skySize, 1};
        vkCmdCopyImageToBuffer(cmd, m_sky.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               skyReadback, 1, &region);

        VkBufferImageCopy lutRegion = region;
        lutRegion.imageSubresource.layerCount = 1;
        lutRegion.imageExtent = {m_settings.lutSize, m_settings.lutSize, 1};
        vkCmdCopyImageToBuffer(cmd, m_lut.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               lutReadback, 1, &lutRegion);
    }
    transitionImage(cmd, m_sky.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kTransfer,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
    transitionImage(cmd, m_lut.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kTransfer,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
}

void Environment::fillProbe(const void* skyData, const void* lutData) {
    // ---- 天空立方体：逐面求平均色 ----
    const auto* sky = static_cast<const float*>(skyData);
    const uint32_t s = m_settings.skySize;
    const size_t texels = static_cast<size_t>(s) * s;
    for (uint32_t f = 0; f < kCubeFaces; ++f) {
        const float* base = sky + static_cast<size_t>(f) * texels * 4;
        glm::dvec3 acc(0.0);
        for (size_t i = 0; i < texels; ++i) {
            acc += glm::dvec3(base[i * 4 + 0], base[i * 4 + 1], base[i * 4 + 2]);
        }
        acc /= static_cast<double>(texels);
        m_probe.face[f] = glm::vec3(acc);
    }

    // ---- BRDF 表 ----
    const auto* lut = static_cast<const float*>(lutData);
    const uint32_t L = m_settings.lutSize;
    auto sample = [&](uint32_t x, uint32_t y) {
        const size_t idx = (static_cast<size_t>(y) * L + x) * 2;
        return glm::vec2(lut[idx], lut[idx + 1]);
    };
    // 右下角列 (x=L-1) → N·V ≈ 1；第 0 行 → roughness ≈ 0
    const glm::vec2 corner = sample(L - 1, 0);
    m_probe.lutA_at_ndotv1_rough0 = corner.x;
    m_probe.lutB_at_ndotv1_rough0 = corner.y;
    const glm::vec2 half = sample(L / 2, L / 2);
    m_probe.lutA_at_half = half.x;
    m_probe.lutB_at_half = half.y;
}

void Environment::build() {
    if (m_valid) return;

    // ---- 图像 ----
    // 天空用 RGBA32F 而不是 16F：它要被 CPU 回读做自检，32F 能直接
    // 当 float 读。1.5 MB 的代价换掉一整套半精度转换代码，很划算。
    createCube(m_sky, VK_FORMAT_R32G32B32A32_SFLOAT, m_settings.skySize, 1,
               /*readback=*/true);
    createCube(m_irr, VK_FORMAT_R16G16B16A16_SFLOAT, m_settings.irradianceSize,
               1, /*readback=*/false);
    createCube(m_pref, VK_FORMAT_R16G16B16A16_SFLOAT, m_settings.prefilterSize,
               m_settings.prefilterMips, /*readback=*/false);
    create2D(m_lut, VK_FORMAT_R32G32_SFLOAT, m_settings.lutSize,
             /*readback=*/true);

    // ---- 采样器 ----
    // 立方体：线性过滤 + LINEAR mip（预过滤的粗糙度靠 mip 三线性插值
    // 变成连续量）+ CLAMP_TO_EDGE（方向采样不该有 wrap 语义）
    {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod = 0.0f;
        si.maxLod = static_cast<float>(m_settings.prefilterMips - 1);
        VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_cubeSampler));
    }
    // LUT：单 mip，绝不能线性插值 mip（只有一级，也不该有 mip 取样）
    {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod = 0.0f;
        si.maxLod = 0.0f;
        VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_lutSampler));
    }

    createDescriptors();
    createPipelines();

    // ---- 回读缓冲 ----
    const VkDeviceSize skyBytes = static_cast<VkDeviceSize>(m_settings.skySize) *
                                  m_settings.skySize * kCubeFaces * 4 *
                                  sizeof(float);
    const VkDeviceSize lutBytes = static_cast<VkDeviceSize>(m_settings.lutSize) *
                                  m_settings.lutSize * 2 * sizeof(float);
    rhi::Buffer skyRb(m_device, skyBytes,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    rhi::Buffer lutRb(m_device, lutBytes,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // ---- 跑一遍（endAndSubmit 会 vkQueueWaitIdle，之后回读是安全的）----
    {
        VkCommandBuffer cmd = m_cmdPool.beginOneTime();
        recordBuild(cmd, skyRb.get(), lutRb.get());
        m_cmdPool.endAndSubmit(cmd);
    }

    fillProbe(skyRb.map(), lutRb.map());
    skyRb.unmap();
    lutRb.unmap();

    m_stats.skySize = m_settings.skySize;
    m_stats.irradianceSize = m_settings.irradianceSize;
    m_stats.prefilterSize = m_settings.prefilterSize;
    m_stats.prefilterMips = m_settings.prefilterMips;
    m_stats.lutSize = m_settings.lutSize;
    m_valid = true;
}

} // namespace render
