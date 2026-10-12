#pragma once
// ============================================================
// render/SSAOPass —— 屏幕空间环境光遮蔽（前向之前的那一段）
//
// 位置：深度预通道之后、分簇 compute 与前向 Pass 之前。
// 输入：HdrTarget 的 **MSAA 深度图**（sampler2DMS 取样本重建视图
//       空间几何，见 assets/shaders/space.glsl）。
// 输出：R8 遮蔽图（每帧两张：ssao 原始噪声图 → blur 去噪图），
//       去噪图由 pbr.frag（set 0 / binding 14）采样乘进环境光。
//
//   深度预通道 ─→ [SSAO] ─→ [模糊] ─→ 分簇 compute ─→ 前向(采样)
//
// 为什么挤在预通道和前向之间而不是后处理：AO 要影响**着色**，
// 后处理乘上去会把直接光一起压暗 —— 那是"调暗"不是"遮蔽"。
// 真正的 AO 只该压环境光（直接光的遮挡由阴影图负责），
// 所以必须在 pbr.frag 里乘，所以必须在它跑之前算好。
//
// 为什么每帧两张 R8 中间图：SSAO 采样核带逐像素随机旋转
// （去 banding），原始输出是噪声图；直接乘进着色会把噪声带进
// 画面。中间插一个深度感知模糊（ssao_blur.frag）按视图深度差
// 加权 —— 跨轮廓的邻居权重趋 0，不会把 AO 糊出光晕。
//
// MSAA 双形态：深度图在 MSAA=4 时是 sampler2DMS、MSAA=1 时是
// sampler2D，两种管线的片元着色器不同（同一份源码两个宏编出），
// recreate(samples) 时按 HdrTarget 实际采样数选。
//
// 开/关语义：关闭时**不渲染**，Renderer 把 binding 14 指到缺省
// 纯白图 → pbr.frag 采样恒 1.0，环境光乘法逐位不变 —— 基线安全。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <memory>
#include <vector>

namespace rhi {
class Device;
class Pipeline;
class DescriptorAllocator;
}

namespace render {

struct ScreenSpaceSettings;

class SSAOPass {
public:
    SSAOPass(rhi::Device& device, uint32_t framesInFlight);
    ~SSAOPass();

    SSAOPass(const SSAOPass&) = delete;
    SSAOPass& operator=(const SSAOPass&) = delete;

    // 尺寸 / MSAA 采样数变化时整组重建（两张 R8 图 + 两个 renderpass
    // + 描述符池）。depthFormat 仅用于校验（R8 图与深度格式无关，
    // 深度图由 HdrTarget 持有，这里只拿它的视图）。
    void recreate(VkExtent2D extent, VkSampleCountFlagBits samples,
                  VkFormat depthFormat);

    // 每帧的深度可采样视图（HdrTarget::depthSampledView）。
    // HdrTarget 重建后视图换了，必须重新喂。
    void setDepthViews(const std::vector<VkImageView>& views);

    // 建/重建两条管线（× MSAA 双形态按 recreate 的采样数选）。
    // 独立出来给 shader 热重载复用。
    void createPipelines();

    // 录制 SSAO + 模糊两个全屏 pass。前提：深度预通道已结束。
    void render(VkCommandBuffer cmd, uint32_t frameIndex,
                const ScreenSpaceSettings& s, const glm::mat4& invProj,
                const glm::vec4& projCoefs, float zn, float zf);

    // 去噪后的遮蔽图视图（binding 14 绑它；未就绪时调用方绑白图）
    VkImageView resultView(uint32_t frameIndex) const;
    // 采样遮蔽图用的线性采样器（pbr.frag 的描述符要用）
    VkSampler sampler() const { return m_sampler; }
    bool ready() const { return m_ready; }

private:
    // R8 遮蔽图（原始 / 去噪各一张，每帧各一）
    struct AoFrame {
        VkImage rawImage = VK_NULL_HANDLE;
        VkDeviceMemory rawMemory = VK_NULL_HANDLE;
        VkImageView rawView = VK_NULL_HANDLE;
        VkFramebuffer rawFb = VK_NULL_HANDLE;

        VkImage finalImage = VK_NULL_HANDLE;
        VkDeviceMemory finalMemory = VK_NULL_HANDLE;
        VkImageView finalView = VK_NULL_HANDLE;
        VkFramebuffer finalFb = VK_NULL_HANDLE;
    };

    void createRenderPasses();
    void destroyFrames();
    void createFrames();
    void buildDescriptorSets();

    // 全屏三角形 pass 的通用录制（Begin → bind → push → draw → End）
    void recordPass(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb,
                    VkExtent2D extent, rhi::Pipeline& pipeline,
                    const VkDescriptorSet* sets, uint32_t setCount,
                    const void* push, uint32_t pushSize);

    rhi::Device& m_device;
    uint32_t m_framesInFlight = 2;

    VkExtent2D m_extent{0, 0};
    bool m_msaa = true;     // 深度图是否多采样（选哪份 .spv）
    bool m_ready = false;

    // 两个 renderpass：SSAO 写原始图、模糊写去噪图。
    // 附件描述与 PostProcess 的 bloom RP 同款（UNDEFINED 进、
    // SHADER_READ_ONLY 出、DONT_CARE 装载 —— 全屏覆盖不需要旧内容，
    // initialLayout=UNDEFINED 允许与上一帧残留的 READ_ONLY 不匹配）。
    VkRenderPass m_ssaoRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_blurRenderPass = VK_NULL_HANDLE;

    std::unique_ptr<rhi::Pipeline> m_ssaoPipeline;
    std::unique_ptr<rhi::Pipeline> m_blurPipeline;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    // 线性（采样 AO 图）与最近邻（texelFetch 深度图，禁过滤）各一
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkSampler m_depthSampler = VK_NULL_HANDLE;
    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;
    // ssao 输入（深度图）与模糊的两个输入（原始 AO / 深度图）
    std::vector<VkDescriptorSet> m_depthSets;
    std::vector<VkDescriptorSet> m_blurRawSets;
    std::vector<VkDescriptorSet> m_blurDepthSets;

    std::vector<AoFrame> m_frames;
};

} // namespace render
