#pragma once
// ============================================================
// render/HiZOcclusion —— Hi-Z 遮挡剔除（R2-1，两帧延迟）
//
// 解决 BVH 视锥剔除管不到的那一类浪费：**被建筑物挡住的物体照样提交**。
// 视锥剔除只问"在不在画面里"，Hi-Z 补上"在画面里但被更近的东西盖住了"。
//
// ---- 一帧的流程 ----
//   depth prepass / 前向 已把深度图准备好（HdrTarget 的深度附件）
//        │
//        ├─ hiz_build.comp       深度（可 MSAA）→ 金字塔 mip0（取样本 max）
//        ├─ hiz_downsample.comp  mip k-1 → mip k（2×2 取 max），逐级到底
//        └─ occlusion_test.comp  每个候选 AABB 投影 → 采样 Hi-Z → 写 vis 标志
//                                       │
//                              vis SSBO（HOST_VISIBLE）
//                                       │
//                     两帧后：GPU 完成 → CPU 回读（beginFrame）
//                                       │
//                       被剔的实体不进下一帧的绘制列表
//
// ---- 为什么是"两帧延迟" ----
//   本帧写进 vis SSBO 的结果，要等这一帧的命令缓冲真正跑完（下一次轮到
//   同一个 frame-in-flight 槽、in-flight 栅栏过后）才能安全读回 ——
//   MAX_FRAMES_IN_FLIGHT = 2，所以恰好延迟两帧。这也顺带保证"读回的
//   永远是已完成的结果"，不需要额外的阻塞式等待。
//
// ---- 保守性 ----
//   Hi-Z 存的是"区域内最远的遮挡体深度"，判定条件是"物体最近点 > 区域
//   最远深度 + 偏置"。任何投影异常（相机后方 / 被裁剪 / 出屏 / 天空）
//   一律判可见。详见 occlusion_test.comp 顶部。
//
// ---- 默认关 ----
//   关闭时一个 compute 都不录、CPU 不剔任何物体 → 与引入前逐像素相同。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class ComputePipeline;
class Buffer;
class DescriptorAllocator;
}

namespace render {

// 一个候选物体的世界空间 AABB（std430，两个 vec4 = 32 字节）
struct HiZAabb {
    glm::vec4 lo{0.0f};   // xyz = min
    glm::vec4 hi{0.0f};   // xyz = max
};
static_assert(sizeof(HiZAabb) == 32, "HiZAabb 必须与着色器里的 vec4 aabbs[] 对齐");

struct HiZStats {
    uint32_t objects = 0;      // 最近一次回读对应的物体数
    uint32_t occluded = 0;     // 其中判定被遮挡的数量
    uint32_t mipCount = 0;     // 金字塔级数
    uint32_t extentW = 0;
    uint32_t extentH = 0;
};

class HiZOcclusion {
public:
    // 同时能测的 AABB 上限。超出的部分不测（保持可见）。
    static constexpr uint32_t kMaxObjects = 8192;

    HiZOcclusion(rhi::Device& device, rhi::CommandPool& cmdPool,
                 uint32_t framesInFlight);
    ~HiZOcclusion();

    HiZOcclusion(const HiZOcclusion&) = delete;
    HiZOcclusion& operator=(const HiZOcclusion&) = delete;

    // 分辨率 / 采样数变化时重建金字塔与缓冲。会 waitIdle。
    void recreate(VkExtent2D extent, VkSampleCountFlagBits samples);

    // HdrTarget 重建后深度视图会换 —— 重新喂给 build 描述符集。
    void setDepthViews(const std::vector<VkImageView>& views);

    // 每帧上传候选 AABB（本帧视锥可见列表）。frameIndex 用 Renderer 的
    // 当前 frame-in-flight 槽。
    void uploadAabbs(uint32_t frameIndex, const std::vector<HiZAabb>& boxes);

    // 本槽这一帧上传了多少个（写进 push 常量用）
    uint32_t uploadedCount(uint32_t frameIndex) const;

    // in-flight 栅栏之后调用：把两帧前那份 GPU 结果读回。
    void beginFrame(uint32_t frameIndex);

    // 回读到的可见性标志（与"两帧前那次 uploadAabbs 的顺序"一一对应）；
    // true = 可见，false = 被遮挡。数量 = 那次上传的对象数。
    const std::vector<uint8_t>& visFlags() const { return m_visFlags; }

    const HiZStats& stats() const { return m_stats; }
    bool ready() const { return m_ready; }
    VkExtent2D extent() const { return m_extent; }

    // 录制：建金字塔 + 遮挡测试。前提：深度图已就绪（深度预通道或前向
    // pass 已结束，深度附件处于 DEPTH_READ_ONLY）。depthImage 只用于插一条
    // "深度写 → compute 读"的屏障（附件写只对片元阶段可见，见 HdrTarget
    // 的 deps[1]），取样走 buildSet 里已绑好的深度视图。
    //
    // R2-2 的 GPU-driven 路径需要**只建金字塔**、然后由别的模块做测试与
    // 紧凑化，所以拆成两步：recordPyramid + recordTest，record() 是两者的
    // 合并（= R2-1 的 CPU 回读路径）。
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkImage depthImage,
                const glm::mat4& viewProj, float depthBias);
    void recordPyramid(VkCommandBuffer cmd, uint32_t frameIndex,
                       VkImage depthImage);
    void recordTest(VkCommandBuffer cmd, uint32_t frameIndex,
                    const glm::mat4& viewProj, float depthBias);

    // 诊断日志用
    uint32_t mipCount() const { return m_mipCount; }

    // ---- 给 R2-2（MdiDraw）用的只读引用 ----
    // Hi-Z 全 mip 链的采样视图（GENERAL 布局）。重建立即失效。
    VkImageView sampleView(uint32_t frameIndex) const {
        if (m_frames.empty()) return VK_NULL_HANDLE;
        return m_frames[frameIndex % m_frames.size()].sampleView;
    }
    // 本槽的候选 AABB 缓冲（CPU 每帧写）。R2-2 的紧凑化着色器直接读它。
    // 实现在 .cpp：这里只有 rhi::Buffer 的前向声明，取不出 VkBuffer。
    VkBuffer aabbBuffer(uint32_t frameIndex) const;

private:
    struct FrameData {
        VkImage image = VK_NULL_HANDLE;        // R32_SFLOAT，全 mip 链
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView sampleView = VK_NULL_HANDLE;  // 全 mip（读）
        std::vector<VkImageView> mipViews;        // 每 mip 一个存储视图（写）

        std::unique_ptr<rhi::Buffer> aabb;     // HOST_VISIBLE，CPU 写
        std::unique_ptr<rhi::Buffer> vis;      // HOST_VISIBLE，GPU 写 / CPU 读
        void* visMapped = nullptr;
        uint32_t count = 0;

        VkDescriptorSet buildSet = VK_NULL_HANDLE;   // 深度 → mip0
        std::vector<VkDescriptorSet> downSets;       // mip k-1 → k
        VkDescriptorSet testSet = VK_NULL_HANDLE;    // AABB vs Hi-Z
    };

    void createPipelines();
    void destroyFrames();
    void createFrames();
    void buildDescriptorSets();
    void destroyDescriptorSets();

    rhi::Device& m_device;
    rhi::CommandPool& m_cmdPool;
    uint32_t m_framesInFlight = 2;
    VkExtent2D m_extent{0, 0};
    uint32_t m_mipCount = 0;
    uint32_t m_sampleCount = 1;
    bool m_msaa = true;
    bool m_ready = false;

    // 三个 compute：建 mip0 / 逐级降采样 / 遮挡测试。
    // 建 mip0 与降采样共用一套 set 布局（sampler + storage image）。
    std::unique_ptr<rhi::ComputePipeline> m_buildPipeline;
    std::unique_ptr<rhi::ComputePipeline> m_buildPipelineMsaa;
    std::unique_ptr<rhi::ComputePipeline> m_downPipeline;
    std::unique_ptr<rhi::ComputePipeline> m_testPipeline;

    VkDescriptorSetLayout m_downSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_testSetLayout = VK_NULL_HANDLE;
    VkSampler m_pointSampler = VK_NULL_HANDLE;

    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;
    std::vector<FrameData> m_frames;

    // 回读到 CPU 的可见性
    std::vector<uint8_t> m_visFlags;
    HiZStats m_stats;
};

} // namespace render
