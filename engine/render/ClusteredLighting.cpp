#include "render/ClusteredLighting.h"

#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/ComputePipeline.h"
#include "rhi/Device.h"

#include "render/ShaderPath.h"

#include "core/Logger.h"

#include <cstring>

namespace render {

namespace {

// lightIndices 的显存上限。超过就说明 tileSize / maxLightsPerCluster 配得太激进
constexpr VkDeviceSize kMaxLightIndexBytes = 48ull * 1024ull * 1024ull;

} // namespace

ClusteredLighting::ClusteredLighting(rhi::Device& device,
                                     rhi::CommandPool& cmdPool,
                                     VkDescriptorSetLayout globalSetLayout,
                                     uint32_t framesInFlight)
    : m_device(device), m_cmdPool(cmdPool), m_setLayout(globalSetLayout),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    m_appliedConfig = m_config;
    m_frames.resize(m_framesInFlight);
    createPipeline();
}

ClusteredLighting::~ClusteredLighting() {
    m_pipeline.reset();  // 需要 device 仍有效
    destroyFrames();
}

void ClusteredLighting::createPipeline() {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset = 0;
    pc.size = sizeof(ClusterPushConstants);

    m_pipeline = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("cluster_build.comp.spv")),
        std::vector<VkDescriptorSetLayout>{m_setLayout}, &pc);
}

// ---------------------------------------------------------------- 缓冲

void ClusteredLighting::recreate(uint32_t width, uint32_t height) {
    destroyFrames();
    m_appliedConfig = m_config;

    m_extentW = width > 0 ? width : 1;
    m_extentH = height > 0 ? height : 1;

    m_gridX = (m_extentW + m_config.tileSize - 1) / m_config.tileSize;
    m_gridY = (m_extentH + m_config.tileSize - 1) / m_config.tileSize;
    m_clusterCount = m_gridX * m_gridY * m_config.depthSlices;

    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(m_clusterCount) *
        m_config.maxLightsPerCluster * sizeof(uint32_t);
    if (indexBytes > kMaxLightIndexBytes) {
        VK_LOG_WARN(
            "ClusteredLighting: light index buffer would be %llu MB "
            "(clusters=%u x %u). Consider a larger tileSize.",
            static_cast<unsigned long long>(indexBytes / (1024 * 1024)),
            m_clusterCount, m_config.maxLightsPerCluster);
    }

    buildBuffers();

    VK_LOG_INFO(
        "ClusteredLighting: %ux%u clusters (tile %u, %u slices) = %u clusters, "
        "light index %llu KB/frame",
        m_gridX, m_gridY, m_config.tileSize, m_config.depthSlices,
        m_clusterCount,
        static_cast<unsigned long long>(indexBytes / 1024));
}

void ClusteredLighting::buildBuffers() {
    const VkDeviceSize lightsBytes =
        static_cast<VkDeviceSize>(kMaxLights) * sizeof(scene::LightInstance);
    const VkDeviceSize clustersBytes =
        static_cast<VkDeviceSize>(m_clusterCount) * 2u * sizeof(uint32_t);
    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(m_clusterCount) *
        m_config.maxLightsPerCluster * sizeof(uint32_t);

    for (auto& f : m_frames) {
        // ---- lights：每帧从 CPU 上传 → HOST_VISIBLE ----
        f.lights = std::make_unique<rhi::Buffer>(
            m_device, lightsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        f.lightsMapped = f.lights->map();
        std::memset(f.lightsMapped, 0, static_cast<size_t>(lightsBytes));

        // ---- clusters：纯 GPU 随机访问 → DEVICE_LOCAL ----
        // 用 vkCmdFillBuffer 清零：初始内容必须是"0 盏灯"，否则首帧
        // 描述符还没被 compute 写过时会读到垃圾。
        f.clusters = std::make_unique<rhi::Buffer>(
            m_device, clustersBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        {
            VkCommandBuffer cmd = m_cmdPool.beginOneTime();
            vkCmdFillBuffer(cmd, f.clusters->get(), 0, clustersBytes, 0u);
            m_cmdPool.endAndSubmit(cmd);
        }

        // ---- lightIndices：compute 每次都会写满有效区间，无需初始化 ----
        f.lightIndices = std::make_unique<rhi::Buffer>(
            m_device, indexBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        // ---- stats：CPU 清零 / GPU 原子累加 / CPU 回读 ----
        f.stats = std::make_unique<rhi::Buffer>(
            m_device, 4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        f.statsMapped = f.stats->map();
        std::memset(f.statsMapped, 0, 4 * sizeof(uint32_t));
    }
}

void ClusteredLighting::destroyFrames() {
    m_device.waitIdle();
    for (auto& f : m_frames) {
        f.statsMapped = nullptr;
        f.lightsMapped = nullptr;
        f.stats.reset();
        f.lightIndices.reset();
        f.clusters.reset();
        f.lights.reset();
    }
    // 缓冲区由 unique_ptr 释放，但槽位保留 —— recreate() 之后立刻重新填充
}

// ---------------------------------------------------------------- 每帧数据

void ClusteredLighting::uploadLights(
    uint32_t frameIndex, const std::vector<scene::LightInstance>& lights) {
    if (m_frames.empty()) return;
    auto& f = m_frames[frameIndex % m_frames.size()];
    if (!f.lights || !f.lightsMapped) return;

    uint32_t n = static_cast<uint32_t>(lights.size());
    if (n > kMaxLights) {
        VK_LOG_WARN("ClusteredLighting: %u lights exceeds capacity %u, clamped",
                    n, kMaxLights);
        n = kMaxLights;
    }
    m_uploadedLights = n;

    if (n > 0) {
        std::memcpy(f.lightsMapped, lights.data(),
                    static_cast<size_t>(n) * sizeof(scene::LightInstance));
    }
    // 多出来的旧数据不用清 —— 着色器只看 [0, lightParams.x)

    // 清零遥测：本帧的 compute 会重新累加
    if (f.statsMapped) std::memset(f.statsMapped, 0, 4 * sizeof(uint32_t));
}

void ClusteredLighting::beginFrame(uint32_t frameIndex) {
    if (m_frames.empty()) return;
    auto& f = m_frames[frameIndex % m_frames.size()];
    if (!f.statsMapped) return;

    // 这里的读取是安全的：调用点在等待该帧的 in-flight 栅栏之后，
    // 说明 GPU 已经不再访问这个缓冲。
    const auto* s = static_cast<const uint32_t*>(f.statsMapped);
    m_stats.clusterX = m_gridX;
    m_stats.clusterY = m_gridY;
    m_stats.clusterZ = m_config.depthSlices;
    m_stats.clusterCount = m_clusterCount;
    m_stats.maxLightsInCluster = s[0];
    m_stats.overflowClusters = s[1];
    m_stats.totalAssignments = s[2];
    m_stats.emptyClusters = s[3];
}

// ---------------------------------------------------------------- 录制

void ClusteredLighting::record(VkCommandBuffer cmd, uint32_t frameIndex,
                               bool cullingEnabled, VkDescriptorSet globalSet) {
    if (!m_pipeline || m_frames.empty() || m_clusterCount == 0) return;

    const uint32_t groups = (m_clusterCount + 63u) / 64u;

    ClusterPushConstants pc{};
    pc.clusterCount = m_clusterCount;
    pc.maxLightsPerCluster = m_config.maxLightsPerCluster;
    pc.gridX = m_gridX;
    pc.gridY = m_gridY;

    // 是否启用剔除通过 UBO 的 lightParams.z 传给着色器（Renderer 填），
    // 这里只负责派发。
    (void)cullingEnabled;

    // 必须绑定 set 0：cluster_build.comp 的 FrameUBO / lights / clusters /
    // lightIndices / stats 全在这一个 set 里。
    m_pipeline->dispatch(cmd, groups, 1, 1, &globalSet, 1, &pc, sizeof(pc));

    // 让 clusters / lightIndices 的写对后面的片元阶段可见。
    // compute 与后续的图形 Pass 在同一个命令缓冲里，中间没有 render pass
    // 依赖帮忙同步，所以必须显式插一个 buffer barrier。
    VkBufferMemoryBarrier barriers[2]{};
    for (int i = 0; i < 2; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].offset = 0;
        barriers[i].size = VK_WHOLE_SIZE;
    }
    barriers[0].buffer = m_frames[frameIndex % m_frames.size()].clusters->get();
    barriers[1].buffer =
        m_frames[frameIndex % m_frames.size()].lightIndices->get();

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         2, barriers, 0, nullptr);
}

// ---------------------------------------------------------------- 句柄

VkBuffer ClusteredLighting::lightsBuffer(uint32_t frameIndex) const {
    return m_frames[frameIndex % m_frames.size()].lights->get();
}
VkDeviceSize ClusteredLighting::lightsBufferSize() const {
    return static_cast<VkDeviceSize>(kMaxLights) * sizeof(scene::LightInstance);
}
VkBuffer ClusteredLighting::clustersBuffer(uint32_t frameIndex) const {
    return m_frames[frameIndex % m_frames.size()].clusters->get();
}
VkDeviceSize ClusteredLighting::clustersBufferSize() const {
    return static_cast<VkDeviceSize>(m_clusterCount) * 2u * sizeof(uint32_t);
}
VkBuffer ClusteredLighting::lightIndicesBuffer(uint32_t frameIndex) const {
    return m_frames[frameIndex % m_frames.size()].lightIndices->get();
}
VkDeviceSize ClusteredLighting::lightIndicesBufferSize() const {
    return static_cast<VkDeviceSize>(m_clusterCount) *
           m_config.maxLightsPerCluster * sizeof(uint32_t);
}
VkBuffer ClusteredLighting::statsBuffer(uint32_t frameIndex) const {
    return m_frames[frameIndex % m_frames.size()].stats->get();
}
VkDeviceSize ClusteredLighting::statsBufferSize() const {
    return 4 * sizeof(uint32_t);
}

} // namespace render
