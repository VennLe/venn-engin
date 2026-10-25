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
//   1) m_depthPrePass：几何预通道（深度 + 几何缓冲）
//        - 附件 = 多采样深度图 + 几何缓冲（法线/线性深度）
//        - 不透明几何先跑一遍，把最前面的深度填好
//        - 主 Pass 靠它做 early-Z：被遮挡的像素在深度测试阶段就被丢掉，
//          片元着色器不会执行 —— 分簇光照的片元开销由此大幅下降
//        - **几何缓冲是 2026-10-25 加的**（描边用）。为什么挂在预通道而不是
//          另起一个 pass：不透明几何已经在这里完整画过一遍，多画一遍等于
//          顶点+光栅化成本翻倍，而这里只多一个 4 通道的 G16R16F 附件。
//          一次投入三件事受益：描边、线宽视距补偿、后续 SSAO/DOF。
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
//
// ---- 共享深度（2026-10-12 屏幕空间效果引入）----
// 深度图原本只有附件一种身份（预通道写、前向 LOAD、之后没人再读）。
// SSAO / SSR / SSGI / god rays 都要"读深度"才能重建视图空间坐标，
// 所以深度图多了一个身份：**可采样纹理**（sampler2DMS，MSAA 图像
// 只能用 2DMS 取样本）。为此两处改动：
//   · 深度图 usage 加 SAMPLED_BIT，每帧多一个 depthSampledView
//     （与附件视图同图不同 view —— view 只是布局无关的引用，安全）；
//   · 两个 renderpass 的深度附件进出布局改为 DEPTH_READ_ONLY：
//     预通道 finalLayout = DEPTH_READ_ONLY（SSAO 在预通道与前向之间
//     采样它）→ 前向 initialLayout = DEPTH_READ_ONLY、subpass 内自动
//     升回 DEPTH_ATTACHMENT（renderpass 的附件引用布局自带迁移）、
//     finalLayout = DEPTH_READ_ONLY（前向之后的链继续采样）。
//     storeOp 相应从 DONT_CARE 改成 STORE（不然链读到的是未定义内容）。
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

    // 几何缓冲（深度预通道的颜色附件）：
    //   rgb = 世界空间法线（-1..1，**不**编码成 0..1 —— 16F 存得下负值，
    //         省掉着色器里两次 *0.5+0.5 / *2-1，也避免编码误差）
    //   a   = 视图空间线性距离（米，= 1/gl_FragCoord.w）
    //
    // 为什么是 16F 而不是 RGBA8：
    //   · 法线用 8bit 量化在掠射角会出现 1/255 的台阶，边缘检测会把台阶
    //     当边缘 → 出现一圈虚线状的假描边；
    //   · a 通道存的是**米**（0.1 ~ 200），8bit 归一化后精度根本不够做
    //     "相对深度差"判据。
    // 为什么把线性深度塞进法线的 a 通道，而不是单独读深度图：
    //   深度图是**多采样**的（MSAA 下只能以 texture2DMS 取样），后处理的
    //   全屏 pass 用普通 sampler2D 采样会直接验证报错。要塞进后处理就得
    //   再给深度做一次 resolve；而几何缓冲本来就要 resolve —— 复用这一次
    //   硬件 resolve，省一张图、省一次 pass、省一次屏障。
    static constexpr VkFormat kGeometryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

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

    // 深度预通道（depth + geometry）
    VkRenderPass depthPrePass() const { return m_depthPrePass; }
    VkFramebuffer depthFramebuffer(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].depthFb;
    }

    // 几何缓冲的**单采样**视图（后处理描边采样它）。
    // MSAA 关时它就是颜色附件本身；开时是硬件 resolve 的结果。
    VkImageView geometryView(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].geoResolveView;
    }
    // 几何缓冲的可采样性是否成立（图没建出来时后处理要能优雅地不绑）
    bool hasGeometry() const {
        return !m_frames.empty() &&
               m_frames[0].geoResolveView != VK_NULL_HANDLE;
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

    // 深度图的**可采样**视图（屏幕空间效果用 sampler2DMS 取样本）。
    // 与附件视图同图不同 view；布局由两个 renderpass 的附件描述统一管理。
    VkImageView depthSampledView(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].depthSampledView;
    }

    // 深度图的 image 本体（Hi-Z 的 compute 屏障需要 image，不是 view）。
    // 与 depthSampledView 是同一张图。
    VkImage depthImage(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].depthImage;
    }

    // 单采样 HDR 颜色图的 **image** 本体（不只是 view）。
    // 给"把这张图拷到别处"的用途 —— 反射探针捕获（R5-1）要把每个面的
    // 解析结果 vkCmdCopyImage 进立方体贴图的一层。renderpass 的
    // finalLayout = SHADER_READ_ONLY，所以调用方从它迁到 TRANSFER_SRC。
    VkImage resolveImage(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].resolveImage;
    }

    VkFormat colorFormat() const { return kColorFormat; }
    VkFormat depthFormat() const { return m_depthFormat; }
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

        // 多采样深度（预通道写、前向通道 LOAD、屏幕空间效果采样）
        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        // 同一张深度图的"纹理身份"视图（usage 含 SAMPLED）。
        // 与 depthView 指向同一 image —— 布局由 renderpass 统一管理。
        VkImageView depthSampledView = VK_NULL_HANDLE;

        // 几何缓冲：多采样那张只在 MSAA 开时存在（它只是 resolve 的源，
        // 之后没人读 → storeOp = DONT_CARE）。MSAA 关时为空。
        VkImage geoImage = VK_NULL_HANDLE;
        VkDeviceMemory geoMemory = VK_NULL_HANDLE;
        VkImageView geoView = VK_NULL_HANDLE;
        // 单采样几何缓冲：后处理唯一能采样的那张（MSAA 关时它就是
        // 颜色附件本身，此时 geoImage/geoView 为空）。
        VkImage geoResolveImage = VK_NULL_HANDLE;
        VkDeviceMemory geoResolveMemory = VK_NULL_HANDLE;
        VkImageView geoResolveView = VK_NULL_HANDLE;

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
