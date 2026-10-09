#pragma once
// ============================================================
// render/HdrTarget —— 前向渲染的离屏目标（MSAA + HDR + resolve）
//
// 为什么需要 HDR：
//   前向渲染直接写交换链（8bit SRGB）会丢高光 —— 金属/灯光高光一旦
//   超过 1.0 就被截断。改为渲染到 R16G16B16A16_SFLOAT（线性、无界），
//   再由 PostProcess 做 tonemapping 压回 [0,1]。
//
// 为什么需要 MSAA：
//   几何边缘与细小高光在单采样下是硬锯齿。分簇光照把大量小范围灯
//   (30~200 盏) 塞进画面后，锯齿会格外显眼，所以这里保留 MSAA。
//
// ---- 两个 RenderPass ----
//   1) m_depthPrePass：只写深度的预通道（深度预通道 / depth prepass）
//        - 附件只有多采样深度图
//        - 不透明几何先跑一遍，把最前面的深度填好
//        - 主 Pass 靠它做 early-Z：被遮挡的像素在深度测试阶段就被丢掉，
//          片元着色器不会执行 —— 分簇光照的片元开销由此大幅下降
//
//   2) m_renderPass：前向通道
//        - 多采样颜色（CLEAR，DONT_CARE 不导出）
//        - 深度（**LOAD** 复用预通道的结果，DONT_CARE 不导出）
//        - resolve 目标（单采样，STORE，出去就是 SHADER_READ_ONLY）
//        - 不透明 → 透明都在这一个 subpass 内顺序绘制：
//          同一个 subpass 内的图元按提交顺序光栅化，混合顺序是确定的
//
// 为什么 colorView() 返回的是 resolve 那一张：
//   PostProcess 只认"一张可以被采样的单采样图"。把 resolve 视图作为
//   对外接口，PostProcess / Bloom 链一行都不用改。
//
// 为什么每个 frame-in-flight 各持一份：
//   两条命令缓冲可能同时在 GPU 上执行，共用附件会出现
//   "A 帧在写、B 帧在采样"的跨命令缓冲竞争。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <vector>

namespace rhi {
class Device;
}

namespace render {

class HdrTarget {
public:
    // 16bit 浮点 RGBA：保留高光、无 banding，且比 32F 省一半带宽
    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    HdrTarget(rhi::Device& device, uint32_t framesInFlight,
              VkSampleCountFlagBits samples);
    ~HdrTarget();

    HdrTarget(const HdrTarget&) = delete;
    HdrTarget& operator=(const HdrTarget&) = delete;

    // 创建 / 重建（交换链尺寸变化时调用）。
    // 深度格式由本类统一挑选，保证 RenderPass 的附件格式与实际图像格式一致。
    void recreate(uint32_t width, uint32_t height);

    VkSampleCountFlagBits samples() const { return m_samples; }
    bool msaaEnabled() const { return m_samples != VK_SAMPLE_COUNT_1_BIT; }

    // 深度预通道（depth-only）
    VkRenderPass depthPrePass() const { return m_depthPrePass; }
    VkFramebuffer depthFramebuffer(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].depthFb;
    }

    // 前向通道（颜色 + 深度 LOAD + resolve）
    VkRenderPass renderPass() const { return m_renderPass; }
    VkFramebuffer framebuffer(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].forwardFb;
    }

    // resolve 后的单采样 HDR 颜色视图 —— 后处理采样它
    VkImageView colorView(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].resolveView;
    }

    VkFormat colorFormat() const { return kColorFormat; }
    VkExtent2D extent() const { return m_extent; }
    uint32_t framesInFlight() const {
        return static_cast<uint32_t>(m_frames.size());
    }

private:
    struct Frame {
        // 多采样颜色附件。MSAA 关闭时不创建（直接借用 resolve 图）
        VkImage msaaColorImage = VK_NULL_HANDLE;
        VkDeviceMemory msaaColorMemory = VK_NULL_HANDLE;
        VkImageView msaaColorView = VK_NULL_HANDLE;

        // 单采样 resolve 图（MSAA 关闭时它就是颜色附件本身）
        VkImage resolveImage = VK_NULL_HANDLE;
        VkDeviceMemory resolveMemory = VK_NULL_HANDLE;
        VkImageView resolveView = VK_NULL_HANDLE;

        // 多采样深度（预通道写、前向通道 LOAD）
        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;

        VkFramebuffer depthFb = VK_NULL_HANDLE;    // 深度预通道
        VkFramebuffer forwardFb = VK_NULL_HANDLE;  // 前向通道
    };

    void createRenderPasses();
    void createFrame(Frame& f);
    void destroyFrame(Frame& f);
    void destroyAllFrames();

    rhi::Device& m_device;
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
    VkRenderPass m_depthPrePass = VK_NULL_HANDLE;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{0, 0};
    std::vector<Frame> m_frames;
};

} // namespace render
