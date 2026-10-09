#pragma once
// ============================================================
// render/ClusteredLighting —— 分簇光照的 GPU 端数据与 compute Pass
//
// 三个 SSBO（每 frame-in-flight 各一份，避免跨命令缓冲竞争）：
//
//   binding 2  lights[]        所有局部光源（LightInstance，64B/盏）
//   binding 3  clusters[]      每簇 2 个 uint：灯索引槽位基址 + 灯数
//   binding 4  lightIndices[]  扁平的灯索引表，每簇固定预留
//                              maxLightsPerCluster 个槽位
//   binding 5  stats[]         遥测（最大/超限/总和/空簇）
//
// 编址：clusterIndex = (slice * gridY + tileY) * gridX + tileX
//   gridX = ceil(width / tileSize)   gridY = ceil(height / tileSize)
//   slice ∈ [0, depthSlices)
// 这套编址在 C++、cluster_build.comp、pbr.frag 三处必须完全一致。
//
// 显存：clusters 与 lightIndices 纯粹是 GPU 侧随机访问，放 DEVICE_LOCAL；
// lights 每帧要从 CPU 上传，放 HOST_VISIBLE。分帧备份让显存翻倍，
// 但换掉了"A 帧在写、B 帧在读"的竞争 —— 这笔账很划算。
// ============================================================

#include "rhi/VulkanCommon.h"
#include "render/RenderTypes.h"
#include "scene/Light.h"

#include <memory>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class ComputePipeline;
class Buffer;
}

namespace render {

// 分簇参数。改这几个值只需要 recreate()，不用重建管线。
struct ClusterConfig {
    // 屏幕瓦片边长（像素）。越小剔除越精确，但簇数与显存线性增长
    uint32_t tileSize = 32;
    // 深度方向的分层数（指数分布：近处密、远处疏）
    uint32_t depthSlices = 16;
    // 每簇固定预留的灯槽位数。这个值会通过 UBO 的 lightParams.w 传给
    // 片元着色器（不能再在 pbr.frag 里硬编码 —— 否则调大它会静默截断）。
    //
    // 为什么默认给到 128（实测平均每簇只用 8 盏）：
    //   * 这个值**只花显存，不花时间** —— compute 侧无论 cap 多大都要遍历
    //     全部灯，片元侧遍历的是 clusters[].y（真实灯数），不是 cap。
    //     所以"调大它"除了多占内存没有任何代价。
    //   * 它是唯一的正确性风险点：真需要超过 cap 盏灯的簇会被截断漏光。
    //     实测 200 盏灯 cap=128 时与暴力渲染逐像素一致；cap=32/64 则出现
    //     上百个溢出簇（遥测里 visible 为 overflowClusters > 0）。
    //   显存参考：1080p ≈ 16 MB/帧，720p ≈ 7.4 MB/帧（再乘帧数）。
    //   4K 下想省显存可以降到 64，代价是远处大切片可能截断。
    uint32_t maxLightsPerCluster = 128;
};

// 遥测结果（把上一帧 GPU 侧统计回读到 CPU）
struct ClusterStats {
    uint32_t clusterX = 0;
    uint32_t clusterY = 0;
    uint32_t clusterZ = 0;
    uint32_t clusterCount = 0;

    uint32_t maxLightsInCluster = 0;   // 单簇最大灯数（贴近上限就该调大槽位了）
    uint32_t overflowClusters = 0;     // 因超过槽位上限被截断的簇数
    uint32_t totalAssignments = 0;     // 灯-簇配对数总和（剔除后）
    uint32_t emptyClusters = 0;        // 一盏灯都没有的簇数

    // 平均每簇灯数 —— 片元着色器循环次数的期望值
    float avgLightsPerCluster() const {
        return clusterCount > 0
                   ? static_cast<float>(totalAssignments) /
                         static_cast<float>(clusterCount)
                   : 0.0f;
    }
    bool overflowed() const { return overflowClusters > 0; }
};

// 与 cluster_build.comp 的 push_constant 块严格对应（16 字节）
struct ClusterPushConstants {
    uint32_t clusterCount;
    uint32_t maxLightsPerCluster;
    uint32_t gridX;
    uint32_t gridY;
};

class ClusteredLighting {
public:
    static constexpr uint32_t kMaxLights = 512;  // lights[] 容量

    ClusteredLighting(rhi::Device& device, rhi::CommandPool& cmdPool,
                      VkDescriptorSetLayout globalSetLayout,
                      uint32_t framesInFlight);
    ~ClusteredLighting();

    ClusteredLighting(const ClusteredLighting&) = delete;
    ClusteredLighting& operator=(const ClusteredLighting&) = delete;

    // 交换链尺寸变化时重建簇缓冲，并重算网格尺寸
    void recreate(uint32_t width, uint32_t height);

    // 每帧把 CPU 侧的灯列表写进 SSBO（超出 kMaxLights 的部分截断）
    void uploadLights(uint32_t frameIndex,
                      const std::vector<scene::LightInstance>& lights);

    // 在帧开始（等待栅栏之后）调用，把上一帧的遥测读回 CPU，并清零统计缓冲
    void beginFrame(uint32_t frameIndex);

    // 录制 compute：dispatch 分簇剔除 + 让 SSBO 写对片元阶段可见的 barrier。
    // globalSet 是 Renderer 每帧写好的 set 0（FrameUBO + 灯/簇 SSBO），
    // compute 管线 layout 与它同源，必须绑上才能通过验证层。
    void record(VkCommandBuffer cmd, uint32_t frameIndex, bool cullingEnabled,
                VkDescriptorSet globalSet);

    // ---- 资源句柄（Renderer 负责写进 set 0 的 binding 2/3/4/5）----
    VkBuffer lightsBuffer(uint32_t frameIndex) const;
    VkDeviceSize lightsBufferSize() const;
    VkBuffer clustersBuffer(uint32_t frameIndex) const;
    VkDeviceSize clustersBufferSize() const;
    VkBuffer lightIndicesBuffer(uint32_t frameIndex) const;
    VkDeviceSize lightIndicesBufferSize() const;
    VkBuffer statsBuffer(uint32_t frameIndex) const;
    VkDeviceSize statsBufferSize() const;

    // ---- 查询 ----
    const ClusterStats& stats() const { return m_stats; }
    const ClusterConfig& config() const { return m_config; }
    uint32_t gridX() const { return m_gridX; }
    uint32_t gridY() const { return m_gridY; }
    uint32_t depthSlices() const { return m_config.depthSlices; }
    uint32_t clusterCount() const { return m_clusterCount; }
    uint32_t uploadedLightCount() const { return m_uploadedLights; }

    // 可写的分簇参数（ImGui 面板直接改）。改完要用 needsRebuild() 检测，
    // 由 Renderer 在下一帧调 recreate() 让新参数生效。
    ClusterConfig& editableConfig() { return m_config; }
    bool needsRebuild() const {
        return m_config.tileSize != m_appliedConfig.tileSize ||
               m_config.depthSlices != m_appliedConfig.depthSlices ||
               m_config.maxLightsPerCluster !=
                   m_appliedConfig.maxLightsPerCluster;
    }

private:
    struct FrameData {
        std::unique_ptr<rhi::Buffer> lights;
        std::unique_ptr<rhi::Buffer> clusters;
        std::unique_ptr<rhi::Buffer> lightIndices;
        std::unique_ptr<rhi::Buffer> stats;
        void* statsMapped = nullptr;  // 4 个 uint：见 cluster_build.comp
        void* lightsMapped = nullptr;
    };

    void destroyFrames();
    void buildBuffers();
    void createPipeline();

    rhi::Device& m_device;
    rhi::CommandPool& m_cmdPool;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;

    ClusterConfig m_config;
    ClusterConfig m_appliedConfig;  // 上一次 recreate() 实际生效的参数
    uint32_t m_framesInFlight = 1;
    uint32_t m_gridX = 0;
    uint32_t m_gridY = 0;
    uint32_t m_clusterCount = 0;
    uint32_t m_extentW = 0;
    uint32_t m_extentH = 0;

    std::unique_ptr<rhi::ComputePipeline> m_pipeline;
    std::vector<FrameData> m_frames;

    uint32_t m_uploadedLights = 0;
    ClusterStats m_stats;
};

} // namespace render
