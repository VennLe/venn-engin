#pragma once
// ============================================================
// ui2/UiRenderer —— 游戏 UI 的正交批渲染通道（U2）
//
// 这是整条 ui2 链条里唯一碰 GPU 的地方。它做四件事：
//   1. 一个**独立的** descriptor set layout（1 个 set / 1 个 binding：图集）；
//   2. 每帧一份环形顶点缓冲（HOST_COHERENT、常驻 map、**不跨帧复用**）；
//   3. 一条 pipeline（动态 viewport/scissor、alpha 混合、无深度）；
//   4. record()：上传顶点 → 绑管线/缓冲/描述符 → 一次 vkCmdDraw。
//
// ---- 为什么全局那 8 个 set 一个都不动 ----
// `maxBoundDescriptorSets` 是**单次绘制时同时绑定**的 set 数上限，不是
// 全局总数。3D 主通道恰好用满 8 个，但 UI 是一条独立管线、一次只绑 1 个
// —— 1 ≤ 8，完全合法（ImGui 自己就是这么干的）。反过来"复用 set0 的空余
// binding"才是灾难：set0 的 25 个 binding 已全满，加一个要改 25 处布局
// 加所有管线的 setLayouts。
//
// ---- 为什么**按 render pass 缓存管线**（而不是一份管线两模式通用）----
// 路线图里原本的判断是"两个输出 pass 只差 finalLayout，而兼容性判定不看
// initial/finalLayout，所以一份管线够用"。**实测是错的**：
//   · 两个 pass 还差一条 subpass dependency（离屏那条的 dstStageMask 是
//     FRAGMENT_SHADER —— 写出的图马上要被 ImGui 采样；交换链那条是
//     BOTTOM_OF_PIPE）。而 **dependencies 参与兼容性判定**。
//   · 在编辑器视口（模式 B）里画 UI，验证层立刻报
//     `VUID-vkCmdDraw-renderPass-02684`（2028 条/秒，一次没漏）。
// 所以改成"按 VkRenderPass 懒建 + 缓存"：第一次在某个 pass 里画时建一条，
// 之后复用。代价是两条全屏 UI 管线（可忽略），换来的是**这条契约不再脆**：
// 将来谁给输出 pass 加附件/改依赖，都不会让 UI 静默失效。
//
// ---- 为什么顶点缓冲是环形 + 每帧独立 ----
// 3 帧 in-flight 意味着 3 条命令缓冲可能同时在 GPU 上跑。共用一块内存会
// 让"第 N 帧的顶点"被第 N+1 帧覆写 → 花屏/闪烁。每帧固定一段偏移，
// 只在**该帧的栅栏已过**之后复用（Renderer 本来就是这么调度 frameIndex 的）。
// ============================================================

#include "rhi/VulkanCommon.h"
#include "ui2/SdfFont.h"
#include "ui2/UiCanvas.h"

#include <cstdint>
#include <memory>

namespace rhi {
class Device;
class Pipeline;
class Buffer;
class Image;
class CommandPool;
class DescriptorAllocator;
}  // namespace rhi

namespace ui2 {

class UiRenderer {
public:
    UiRenderer();
    ~UiRenderer();

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    // renderPass：Post 输出 pass（模式 A 那份；模式 B 靠兼容性复用）。
    // 必须在 PostProcess 建好 pass 之后调用。
    bool init(rhi::Device& device, rhi::CommandPool& cmdPool,
              VkRenderPass renderPass, uint32_t framesInFlight);
    void destroy();

    bool ready() const { return !m_pipelines.empty(); }

    // 换图集（U1 的 SDF 字体就绪后调用）。view 必须处于
    // SHADER_READ_ONLY_OPTIMAL 且是 **UNORM** 格式（SDF 是数据不是颜色）。
    void setAtlas(VkImageView view);

    // 已加载的字体度量（没加载成 = nullptr，此时文本什么都不画）。
    const SdfFont* font() const { return m_font.get(); }

    // 在 Post 输出 pass 内调用（必须在 vkCmdEndRenderPass 之前）。
    // 空画布 = 什么都不做（不绑管线、不设状态）。
    //
    // renderPass **必须**是当前正在进行的那个 pass：管线按它懒建/缓存
    // （两个输出 pass 的 subpass dependency 不同 → 不兼容，见文件头）。
    void record(VkCommandBuffer cmd, uint32_t frameIndex,
                const UiCanvas& canvas, VkExtent2D outExtent,
                VkRenderPass renderPass);

    // "这一帧的界面"的入口：内部灌 buildOverlay() 再 record。
    // Renderer 只调这一个函数 —— UI 画什么完全不归它管（分层）。
    void recordFrameUi(VkCommandBuffer cmd, uint32_t frameIndex,
                       VkExtent2D outExtent, VkRenderPass renderPass);

    struct Stats {
        uint32_t draws = 0;
        uint32_t verts = 0;
    };
    Stats lastStats() const { return m_stats; }

    // 单帧顶点容量（顶点数）。超出部分被丢弃并打一次 WARN ——
    // 静默截断会让"UI 少了一半"变成一个查不出原因的怪现象。
    static constexpr uint32_t kMaxVertsPerFrame = 32768;

private:
    bool createAtlasFallback(rhi::CommandPool& cmdPool);
    // 加载 SDF 字体（JSON 度量 + R8 图集 PNG → GPU 图）。
    // 失败**不是**错误：只是没有字形，通道照常工作（矩形/圆角矩形仍可用）。
    bool loadFont(rhi::CommandPool& cmdPool);
    bool createSetLayoutAndPool();
    bool createVertexBuffers();
    // 取（必要时建）绑在 renderPass 上的那条 UI 管线。
    rhi::Pipeline* pipelineFor(VkRenderPass renderPass);

    rhi::Device* m_device = nullptr;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    std::unique_ptr<rhi::DescriptorAllocator> m_pool;
    std::vector<VkDescriptorSet> m_sets;

    // 每个 render pass 一条管线（键 = VkRenderPass，句柄稳定不回收 ——
    // 输出 pass 与 Renderer 同生命周期，不会重建）。
    struct PassPipeline {
        VkRenderPass pass = VK_NULL_HANDLE;
        std::unique_ptr<rhi::Pipeline> pipeline;
    };
    std::vector<PassPipeline> m_pipelines;

    std::unique_ptr<rhi::Buffer> m_vbo;
    void* m_mapped = nullptr;

    std::unique_ptr<rhi::Image> m_white;   // 缺省 1×1 白（图集未就绪时兜底）
    VkImageView m_atlasView = VK_NULL_HANDLE;

    // SDF 字体：CPU 度量（排版用）+ GPU 图集（采样用）。
    std::unique_ptr<SdfFont> m_font;
    std::unique_ptr<rhi::Image> m_atlasImage;

    uint32_t m_framesInFlight = 2;
    VkDeviceSize m_frameCapacity = 0;      // 字节/帧
    Stats m_stats;
    bool m_overflowWarned = false;
    // 复用同一个画布：每帧 new 一个 vector 会在 60fps 下变成明显的分配抖动
    UiCanvas m_canvas;
    uint32_t m_uiFrames = 0;               // UI2-STATS 的节流计数
};

}  // namespace ui2
