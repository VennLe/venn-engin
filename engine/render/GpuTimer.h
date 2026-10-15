#pragma once
// ============================================================
// render/GpuTimer —— GPU 时戳剖析（R0-1）
//
// VK_QUERY_TYPE_TIMESTAMP 查询池：每帧在命令缓冲里对各 pass 打
// begin/end 时戳，CPU 在该帧 fence 等待之后读回并换算成毫秒。
//
// 段（Section）与 Renderer::recordFrame 的 pass 一一对应：
//
//   shadows      级联阴影（CSM）
//   local_sh     点/聚光局部阴影
//   depth        深度预通道
//   ssao         SSAO（关闭时 begin/end 紧挨 → ≈0，正好标记"未激活"）
//   cluster      分簇剔除 compute
//   forward      前向（天空/不透明/灯球/栅格/透明）
//   particles    GPU 粒子（R6-1：模拟 compute ×2 + 间接绘制）
//   framegraph   屏幕空间链（全关时 ≈0）
//   offscreen    预览 + 额外视角的离屏目标（无窗口时 ≈0）
//   post         Bloom / 合成 / FXAA + UI
//
// 时戳只写 query pool，不触碰任何渲染资源 —— 打点与否不影响画面
// 逐像素结果（verify_gpu_timing.py 的对照断言就是证明这一点）。
//
// 同步约定（安全性的关键）：
//   · resetFrame / begin / end 都录在命令缓冲里，GPU 上按序执行；
//   · readFrame 只允许在该帧 index 的 inFlightFence 等待之后调用
//     —— 那时同 slot 的上一轮命令已执行完，query 一定可用；
//   · query 在录制期由 vkCmdResetQueryPool 复位（Vulkan 1.0 core，
//     不需要 host query reset 扩展）。
// ============================================================

#include "rhi/VulkanCommon.h"
#include "rhi/Device.h"

#include <cstdint>

namespace render {

class GpuTimer {
public:
    static constexpr uint32_t kSectionCount = 11;

    enum Section : uint32_t {
        kShadows = 0,
        kLocalShadows,
        kDepthPre,
        // 延迟贴花（R4-4）：深度预通道之后、前向之前的那一段。
        kDecals,
        kSsao,
        kCluster,
        kForward,
        // GPU 粒子（R6-1）：前向之后、FrameGraph 之前的那一段
        //（模拟 compute ×2 + 间接绘制）。
        //
        // ⚠ 段在**枚举里的顺序必须与录制的先后顺序一致**：
        //   readFrame 用「第 0 段的 begin」到「最后一段的 end」算整帧
        //   耗时（stamps[kSectionCount*2-1]）。把这一段放到末尾会让
        //   "整帧"缩到粒子 pass 结束 —— 后面还有 framegraph/offscreen/
        //   post，读数就少算了。
        kParticles,
        kFrameGraph,
        kOffscreen,
        kPost,
    };

    static const char* sectionName(uint32_t section);

    // framesInFlight = MAX_FRAMES_IN_FLIGHT；每个 slot 占
    // 2 * kSectionCount 个 query。图形队列不支持时戳（timestampValidBits
    // == 0，见于个别虚拟显卡）则 valid() 为 false，所有打点静默跳过。
    GpuTimer(rhi::Device& device, uint32_t framesInFlight);
    ~GpuTimer();
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    bool valid() const { return m_pool != VK_NULL_HANDLE; }

    // ---------------- 录制期（命令缓冲内）----------------
    void resetFrame(VkCommandBuffer cmd, uint32_t frameIndex) const;
    void begin(VkCommandBuffer cmd, uint32_t frameIndex,
               uint32_t section) const;
    void end(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t section) const;

    // 每帧 vkQueueSubmit 之后调用一次：readFrame 只读提交过的 slot
    // （fence 初始为 signaled，未提交的 slot 上 query 还是
    // uninitialized，读了违反 VUID-vkGetQueryPoolResults-None-09401）。
    void markSubmitted(uint32_t frameIndex) {
        if (frameIndex < m_frames) m_submitted[frameIndex] = true;
    }

    // ---------------- CPU 读回 ----------------
    // fence 等待之后调用；query 还没写好（首帧）返回 false。
    bool readFrame(uint32_t frameIndex);
    // 最近一次 readFrame 成功的各段毫秒；<0 = 尚无数据
    float lastMs(uint32_t section) const;
    float lastTotalMs() const;  // 整条 GPU 帧（首 begin → 末 end）

private:
    uint32_t queryBase(uint32_t frameIndex, uint32_t section) const {
        return (frameIndex * kSectionCount + section) * 2;
    }

    VkDevice m_device = VK_NULL_HANDLE;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    float m_periodNs = 0.0f;   // limits.timestampPeriod（纳秒/刻度）
    uint64_t m_validMask = ~0ull;  // timestampValidBits 截断用的掩码
    uint32_t m_frames = 0;

    bool m_hasLast = false;
    float m_last[kSectionCount] = {};
    float m_lastTotal = -1.0f;
    bool m_submitted[8] = {};  // 下标 = frameIndex（MAX_FRAMES_IN_FLIGHT ≤ 8）
};

}  // namespace render
