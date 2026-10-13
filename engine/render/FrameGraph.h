#pragma once
// ============================================================
// render/FrameGraph —— 前向之后的屏幕空间 / 镜头效果编排（Frame Graph）
//
// 之前的 pass 编排硬编码在 Renderer::recordFrame 里：每加一个
// 效果都要动 Renderer.cpp 的录制代码、接一次中间图、插一次屏障。
// 这个类把"前向之后"那一段变成**声明式**的：
//
//   · 资源：HDR 场景色（导入）+ MSAA 深度（导入）+ 2 张 ping-pong
//     工作图（transient，尺寸 = 场景渲染分辨率，格式 = HDR）。
//   · pass：按注册表声明"读什么、写什么、跑哪个着色器"，execute
//     时按注册顺序挑出启用的 pass 依次执行，屏障按统一规则自动插。
//   · 输出：链上最后一个 pass 写的工作图（或全关时的 HDR 本尊）。
//     PostProcess 采样哪张图由 Renderer 在"链激活状态变化"时重接。
//
//   前向(EndRP) ─→ [SSGI]─→[SSR]─→[GODRAYS]─→[DOF]─→[MOTIONBLUR]
//                  ↓每个 pass 读上一张图 + 深度，写下一张工作图
//                                                        └→ PostProcess
//
// 排序理由：SSGI/SSR/godrays 都是"改颜色"的光照类效果，先跑；
// DoF 与运动模糊是**镜头**效果，作用于"最终的画面"（含反射/光柱），
// 所以排在最后 —— 运动模糊是曝光期的时间积分，最末一个跑。
//
// 新增一个效果的完整动作（**不需要碰 Renderer.cpp**）：
//   1. 写 assets/shaders/fg_<效果>.frag（需要深度就按 space.glsl 的
//      双形态约定写，并加进 CMake 的 SHADER_MSAA_DEPTH_SHADERS）；
//   2. kPasses 注册表加一行（名字 / .spv / 推常量填充）；
//   3. 设置结构加开关与参数（ScreenSpaceSettings 或 CameraEffectSettings）；
//   4. EditorApp 的面板加 checkbox。
//
// 同步设计（为什么屏障这么少）：
//   · 每个 pass 写的是**独占的工作图**（每帧每图只写一次），renderpass
//     的 finalLayout=SHADER_READ_ONLY 完成布局迁移与"写对后续采样
//     可见"的内存依赖（PostProcess 的 bloom 链同款）；
//   · ping-pong 复用意味着存在 WAW/WAR（pass 2 重写 pass 0 写过的
//     图）—— execute 在每个 pass 前对两张工作图插一条保守屏障
//     （oldLayout=UNDEFINED 匹配任意实际布局 + 读写两 stage/access）；
//   · 深度输入的可见性由前向 renderpass 的外部依赖保证
//     （HdrTarget.cpp 的 deps[1]：LATE_TESTS|DEPTH_WRITE →
//     FRAGMENT|SHADER_READ）。
//
// 全部效果关闭时 execute 一个 pass 都不录，PostProcess 直接采样
// HDR —— 与引入本类之前**逐像素一致**（回归基线的硬要求）。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <array>
#include <memory>
#include <vector>

namespace rhi {
class Device;
class Pipeline;
class DescriptorAllocator;
}

namespace render {

struct ScreenSpaceSettings;
struct CameraEffectSettings;

// 每帧由 Renderer 填一次的"链公共输入"（相机 / 太阳投影 / 上一帧矩阵）。
// 各 pass 的推常量从这里拼 —— 每个都压在 Vulkan 保证的 128B 限内。
struct FrameGraphInput {
    glm::mat4 invProj{1.0f};        // 深度 → 视图空间
    glm::vec4 projCoefs{0, 0, 0, 0}; // P00 P11 P22 P23（视图点 → 屏幕）
    glm::vec4 camera{0, 0, 0, 0};   // x=zNear y=zFar（深度线性化）
    glm::vec2 sunUv{0.5f, 0.5f};    // 太阳屏幕坐标（god rays 用）
    float sunVisible = 0.0f;        // 太阳在屏内且朝向相机（0/1）
    glm::vec4 sunColor{0, 0, 0, 0}; // rgb = 光色 × 强度（HDR 值域）
    // 运动模糊用：prevViewProj × inverse(view)。把"世界位置 → 上一帧
    // 屏幕坐标"压成一个矩阵 —— 这样片元只要从深度重建视图空间点，
    // 一次矩阵乘就能算出速度，省掉整套相机矩阵（推常量放不下）。
    glm::mat4 clipFromView{1.0f};
};

class FrameGraph {
public:
    FrameGraph(rhi::Device& device, VkFormat sceneFormat,
               uint32_t framesInFlight);
    ~FrameGraph();

    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;

    // 尺寸 / MSAA 变化时重建（2 张工作图 × 每帧 + 管线 + 描述符）
    void recreate(VkExtent2D extent, VkSampleCountFlagBits samples,
                  VkFormat depthFormat);
    // 每帧的深度可采样视图（HdrTarget 重建后必须重喂）
    void setDepthViews(const std::vector<VkImageView>& views);
    // 建/重建全部链管线（shader 热重载复用）
    void createPipelines();

    // ---- 链激活状态（Renderer 据此决定 PostProcess 采样哪张图）----
    static bool chainActive(const ScreenSpaceSettings& s,
                            const CameraEffectSettings& fx);
    // 当前设置下会执行几个 pass（决定输出是哪张工作图）
    // ⚠ 只数**写工作图**的 pass（SSGI..MOTIONBLUR）——TAA 是预 pass，
    //   它写自己的 m_taaOut，输出图的归属由 outputView 单独判定。
    static int passCount(const ScreenSpaceSettings& s,
                         const CameraEffectSettings& fx);

    // 丢弃 TAA 历史（下一帧 historyValid=0，等价于"重新开始累积"）。
    // 调用时机：相机跳变 / 场景切换 / 用户要求重置。尺寸变化与 MSAA
    // 变化走 recreate（图都换了，历史自然失效）——不必手动调。
    void invalidateTaaHistory();

    // 录制整条链。hdrColorView = 前向 resolve 的输出（链的源头）。
    // 全关时什么都不录 —— 零开销、零行为差异。
    void execute(VkCommandBuffer cmd, uint32_t frameIndex,
                 VkImageView hdrColorView, const ScreenSpaceSettings& s,
                 const CameraEffectSettings& fx, const FrameGraphInput& in);

    // 链输出视图（passCount==0 返回 null，调用方回退 HDR 视图）。
    // 与 execute 用同一份设置调用 —— 输出图是确定性的。
    VkImageView outputView(uint32_t frameIndex, const ScreenSpaceSettings& s,
                           const CameraEffectSettings& fx) const;

    // 链上最多几个 pass（注册表长度，Renderer 的接线缓存用）
    static constexpr int kMaxPasses = 5;

private:
    // 每帧两张工作图（ping-pong）
    struct WorkImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;  // 挂在共享链 renderpass 上
    };

    void destroyWorkImages();
    void createWorkImages();
    void buildDescriptorSets();

    // ---- TAA 的持久图（每帧两张）----
    // TAA 与其它链 pass 的关键差别是**跨帧**：它要读上一轮的结果。
    //   m_taaOut[fi]  —— 本帧解析结果。视图固定不变（PostProcess 的
    //                    输入描述符只在 recreate 时写一次，视图一变就
    //                    指向已销毁对象 —— 见 PostProcess::recreate）。
    //   m_taaHist[fi] —— 上一轮的解析结果，被 TAA 采样。
    // 顺序：copy(out→hist) → TAA(hist, HDR, 深度 → out) → 链剩余 pass。
    // 那次 copy 每帧约一张全分辨率 RGBA16F 的带宽（1600×900 ≈ 11MB），
    // 换的是"输出视图恒定"这条硬约束，值得。
    struct TaaImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;  // 挂在共享链 renderpass 上
    };

    void createTaaImages();
    void destroyTaaImages();
    // 录制 TAA，返回本帧的解析结果视图（链的新源头）。
    VkImageView recordTaaPass(VkCommandBuffer cmd, uint32_t fi,
                              VkImageView hdrColorView,
                              const ScreenSpaceSettings& s,
                              const FrameGraphInput& in);

    rhi::Device& m_device;
    VkFormat m_sceneFormat = VK_FORMAT_UNDEFINED;
    uint32_t m_framesInFlight = 2;

    VkExtent2D m_extent{0, 0};
    bool m_msaa = true;
    bool m_ready = false;

    // 共享链 renderpass：所有 pass 的输出图同格式（HDR RGBA16F）、
    // 同步语义一致（UNDEFINED 进 / SHADER_READ_ONLY 出）—— 一条 RP
    // 全链共用，pass 之间只差管线与输入描述符。
    VkRenderPass m_chainRenderPass = VK_NULL_HANDLE;
    // 每个 pass × MSAA 双形态
    std::unique_ptr<rhi::Pipeline> m_pipelines[kMaxPasses][2];

    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;      // 场景色（线性）
    VkSampler m_depthSampler = VK_NULL_HANDLE; // 深度（最近邻）
    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;
    // 输入描述符：HDR 源 / 工作图 0 / 工作图 1 / 深度（每帧各一）
    std::vector<VkDescriptorSet> m_hdrSets;
    std::vector<VkDescriptorSet> m_workSets[2];
    std::vector<VkDescriptorSet> m_depthSets;

    std::vector<std::array<WorkImage, 2>> m_work;

    // TAA（预 pass）：每帧一张输出图（视图固定）+ 一张历史图 + 有效位。
    std::vector<TaaImage> m_taaOut;
    std::vector<TaaImage> m_taaHist;
    std::vector<bool> m_taaValid;
    bool m_taaWasOn = false;  // 上升沿检测：从关到开 → 历史不可信
    std::unique_ptr<rhi::Pipeline> m_taaPipelines[2];  // [非 MSAA / MSAA 深度]
    VkDescriptorSetLayout m_taaSetLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_taaSets;
};

} // namespace render
