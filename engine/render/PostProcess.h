#pragma once
// ============================================================
// render/PostProcess —— Forward 之后的全部 Pass
//
// 编排（全部在同一个命令缓冲内，靠 render pass 的依赖自动同步）：
//   1. Bloom 亮部提取   HDR 全分辨率   → bloomA（半分辨率）
//   2. 高斯模糊 H       bloomA         → bloomB
//   3. 高斯模糊 V       bloomB         → bloomA
//   4. 合成 + Tonemap   HDR + bloomA   → 输出
//
// ---- 两种输出模式（编辑器视口的关键）----
//
//   A) 场景直写交换链（游戏模式，viewportExtent = {0,0}）
//      · 合成 Pass 的 framebuffer 就是交换链图像
//      · ImGui 通过 overlay 回调画在**同一个** Pass 内
//      · 这就是 PostProcess 过去唯一的行为，游戏路径零变化
//
//   B) 场景写离屏图（编辑器模式，viewportExtent != {0,0}）
//      · 合成 Pass 写入一张 viewport 大小的离屏图（format 与交换链一致）
//      · ImGui 拿这张图当纹理，用 ImGui::Image() 画在 Viewport 面板里
//      · 再单独跑一个 Pass：清交换链 + 画 ImGui
//
//   为什么离屏图格式取交换链格式（B8G8R8A8_SRGB）而不是 HDR 格式：
//     ImGui 采样 SRGB 视图时硬件自动做 sRGB→线性，写回 SRGB 交换链时
//     硬件再做线性→sRGB —— 编码两次、解码一次，净结果恰好等价。
//     若换成线性格式，中间那次 sRGB 编码就会丢高光/出现色偏。
//
//   为什么离屏要独立一条合成管线（m_postPipelineOffscreen）：
//     Vulkan 的图形管线绑定在具体的 VkRenderPass 上。离屏 Pass 的
//     finalLayout 是 SHADER_READ_ONLY_OPTIMAL（要被 ImGui 采样），
//     与交换链 Pass 的 PRESENT_SRC_KHR 不同 → 属于两个 RenderPass，
//     必须各建一条管线。多一条全屏三角形管线，代价可以忽略。
//
//   为什么不复用交换链 RenderPass 给离屏用：
//     finalLayout 会被强制成 PRESENT_SRC_KHR，那张图就没法被采样了。
//
// 为什么 Bloom 图像每个 frame-in-flight 各一份：
//   同 ShadowPass/HdrTarget —— 两条命令缓冲可能同时在 GPU 上跑。
//
// 为什么本类自带 DescriptorAllocator：
//   交换链 resize 后 HDR/bloom 的 VkImageView 会变，必须重建描述符集。
//   若复用 Renderer 的池，重置池会连同**纹理**的描述符集一起失效。
//   独立一个池 → resize 时整池丢弃重建，互不影响。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <functional>
#include <memory>
#include <vector>

namespace rhi {
class Device;
class Pipeline;
class DescriptorAllocator;
}

namespace render {

// 后处理参数（ImGui 实时可调）
struct PostSettings {
    float exposure = 1.0f;        // 曝光（乘在 tonemap 之前）
    float bloomThreshold = 1.2f;  // 亮部阈值（>1 → 只让真正的高光发光）
    float bloomKnee = 0.4f;       // 软过渡带宽
    float bloomStrength = 0.30f;  // bloom 叠加强度（0 = 关闭）
    int   tonemapMode = 0;        // 0=ACES 1=Reinhard 2=Clamp
    float vignette = 0.25f;       // 暗角强度
};

class PostProcess {
public:
    // Bloom 链跑在 1/2 分辨率上：开销约 1/4，光晕更柔
    static constexpr uint32_t kBloomDownscale = 2;

    PostProcess(rhi::Device& device, VkFormat swapchainFormat,
                VkFormat hdrFormat, uint32_t framesInFlight);
    ~PostProcess();

    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // 创建/重建。
    //   swapchainViews / swapchainExtent：交换链（UI 与模式 A 的场景输出）
    //   hdrColorViews                   ：HdrTarget 的 resolve 视图（每帧一张）
    //   viewportExtent                  ：{0,0} = 模式 A；否则模式 B 的离屏尺寸
    void recreate(const std::vector<VkImageView>& swapchainViews,
                  VkExtent2D swapchainExtent,
                  const std::vector<VkImageView>& hdrColorViews,
                  VkExtent2D viewportExtent);

    // 是否处于"场景 → 离屏图"模式（编辑器视口）
    bool offscreen() const { return m_splitOutput; }
    // 场景渲染分辨率（模式 A = 交换链尺寸；模式 B = 视口尺寸）
    VkExtent2D sceneExtent() const { return m_sceneExtent; }
    VkExtent2D swapchainExtent() const { return m_swapExtent; }
    // 离屏输出视图（供 ImGui_ImplVulkan_AddTexture 注册成纹理）
    VkImageView sceneOutputView(uint32_t frameIndex) const;
    VkFormat sceneOutputFormat() const { return m_swapFormat; }

    // 录制场景链（bloom + 合成）。overlay 只在模式 A 下被调用。
    void renderScene(VkCommandBuffer cmd, uint32_t imageIndex,
                     uint32_t frameIndex,
                     const std::function<void(VkCommandBuffer)>& overlay);

    // 模式 B 的第二步：清交换链 + 画 ImGui。
    void renderUiToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex,
                             const std::function<void(VkCommandBuffer)>& overlay);

    // ImGui 后端的管线绑定在这个 RenderPass 上（模式 A 的合成 Pass 与
    // 模式 B 的 UI Pass 共用同一个 RenderPass 对象 —— 两者附件描述完全
    // 一致：交换链格式、单采样、CLEAR、PRESENT_SRC）。
    VkRenderPass renderPass() const { return m_postRenderPass; }
    VkExtent2D bloomExtent() const { return m_bloomExtent; }

    PostSettings& settings() { return m_settings; }
    const PostSettings& settings() const { return m_settings; }

private:
    struct BloomFrame {
        VkImage aImage = VK_NULL_HANDLE;
        VkDeviceMemory aMemory = VK_NULL_HANDLE;
        VkImageView aView = VK_NULL_HANDLE;
        VkFramebuffer aFb = VK_NULL_HANDLE;

        VkImage bImage = VK_NULL_HANDLE;
        VkDeviceMemory bMemory = VK_NULL_HANDLE;
        VkImageView bView = VK_NULL_HANDLE;
        VkFramebuffer bFb = VK_NULL_HANDLE;
    };

    // 模式 B：每帧一张 viewport 大小的场景输出图
    struct SceneOutputFrame {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
    };

    void createSetLayoutAndSampler();
    void createRenderPasses();
    void createPipelines();
    void destroyBloom();
    void createBloomFrames();
    void createSwapFramebuffers(const std::vector<VkImageView>& views);
    void destroySwapFramebuffers();
    void createSceneOutputFrames();
    void destroySceneOutputFrames();
    void buildDescriptorSets(const std::vector<VkImageView>& hdrColorViews);
    void destroyDescriptors();

    // 全屏三角形 Pass 的通用管线构造（无顶点缓冲 / 无深度）
    std::unique_ptr<rhi::Pipeline> makeFullscreenPipeline(
        VkRenderPass renderPass, const char* fragSpv, uint32_t setCount);

    // 在 bloom render pass 内画一个全屏三角形到指定 framebuffer
    void drawFullscreen(VkCommandBuffer cmd, rhi::Pipeline& pipeline,
                        VkFramebuffer fb, VkExtent2D extent,
                        const VkDescriptorSet* sets, uint32_t setCount,
                        const float push[4]);

    // 合成 + Tonemap。targetExtent/RP/pipeline 由调用方按模式选择。
    // overlay 非空时在其中绘制（必须落在同一个 RenderPass 实例内）。
    void recordComposite(VkCommandBuffer cmd, VkFramebuffer fb,
                         VkRenderPass rp, rhi::Pipeline& pipeline,
                         VkExtent2D targetExtent, uint32_t frameIndex,
                         const std::function<void(VkCommandBuffer)>* overlay);

    rhi::Device& m_device;
    VkFormat m_swapFormat = VK_FORMAT_UNDEFINED;
    VkFormat m_hdrFormat = VK_FORMAT_UNDEFINED;
    uint32_t m_framesInFlight = 2;

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkRenderPass m_bloomRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_postRenderPass = VK_NULL_HANDLE;       // → 交换链（PRESENT_SRC）
    VkRenderPass m_offscreenRenderPass = VK_NULL_HANDLE;  // → 离屏（SHADER_READ）

    std::unique_ptr<rhi::Pipeline> m_brightPipeline;
    std::unique_ptr<rhi::Pipeline> m_blurPipeline;
    std::unique_ptr<rhi::Pipeline> m_postPipeline;           // 绑 m_postRenderPass
    std::unique_ptr<rhi::Pipeline> m_postOffscreenPipeline;  // 绑离屏 RenderPass

    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;

    bool m_splitOutput = false;         // 模式 B
    VkExtent2D m_swapExtent{0, 0};      // 交换链尺寸
    VkExtent2D m_sceneExtent{0, 0};     // 场景渲染尺寸
    VkExtent2D m_bloomExtent{0, 0};     // = m_sceneExtent / kBloomDownscale

    std::vector<BloomFrame> m_bloom;
    std::vector<VkFramebuffer> m_swapFramebuffers;
    std::vector<SceneOutputFrame> m_sceneOutput;

    // 每帧三个输入集，分别指向 HDR / bloomA / bloomB
    std::vector<VkDescriptorSet> m_hdrSets;
    std::vector<VkDescriptorSet> m_bloomASets;
    std::vector<VkDescriptorSet> m_bloomBSets;

    PostSettings m_settings;
};

} // namespace render
