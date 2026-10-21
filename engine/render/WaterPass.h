#pragma once
// ============================================================
// render/WaterPass —— 水面渲染（2026-10-21 第五轮）
//
// ------------------------------------------------------------
// 它为什么必须是**独立 RenderPass**，而不是前向 Pass 里的一条管线
// ------------------------------------------------------------
// 地形能挂在前向 Pass 内，是因为它只需要"深度已经写好"。水面多要一样
// 东西：**采样这一帧已经画完的场景颜色**做折射。
//
// 在同一个 RenderPass 内，颜色附件正在被写，规范禁止同时把它当纹理读
//（那是 feedback loop，验证层直接报错）。所以顺序必须是：
//
//   前向 Pass 结束（resolve 出单采样 HDR 图）
//     ↓ vkCmdCopyImage：resolve → sceneCopy（本类持有，可采样）
//     ↓ 本类的 RenderPass：只有颜色附件（LOAD，单采样，无深度附件）
//   水面画进 resolve 图（水面之外逐像素不变）
//
// ------------------------------------------------------------
// 为什么没有深度附件
// ------------------------------------------------------------
// HdrTarget 的深度图是**多采样**的，本通道要采样它做遮挡/折射判断。
// 一个 RenderPass 里所有附件的采样数必须一致；若水面也开 4x MSAA，
// 就没法把同一张深度图既当附件又当纹理（那是同一个 feedback loop）。
//
// 于是水面走**单采样 + 手写深度测试**：
//   water.frag 里从深度图重建场景世界坐标，比较它与水面片元到相机的
//   距离，前景更近就 discard。等价于硬件深度测试，代价是一次额外采样，
//   换来的是"同一条通道里能采样颜色 + 深度"。
//
// ------------------------------------------------------------
// 一块水体 = 一份网格 + 每帧一个 UBO
// ------------------------------------------------------------
// 网格按形状现生成（圆形径向 / 矩形方格，约 1 米一格，上限 128²）。
// 只有形状或尺寸变了才重建 —— 拖波高/流速/颜色滑块不动任何缓冲，
// 写 UBO 就生效（这正是用户要的"参数面板实时控制"的实现依据）。
// ============================================================

#include "rhi/Buffer.h"
#include "rhi/VulkanCommon.h"
#include "water/WaterBody.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace rhi {
class CommandPool;
class DescriptorAllocator;
class Device;
class Pipeline;
} // namespace rhi

namespace render {

// 与 water.vert / water.frag 的 WaterUbo **逐字段一致**（std140，8×vec4）
struct WaterUboGpu {
    glm::vec4 shallow;  // rgb = 浅水色, w = opacity
    glm::vec4 deep;     // rgb = 深水色, w = clarity
    glm::vec4 p0;       // x=level y=waveHeight z=waveLength w=flowSpeed
    glm::vec4 p1;       // x=flowDir.x y=flowDir.y z=ripple w=foam
    glm::vec4 p2;       // x=refraction y=ior z=roughness w=time
    glm::vec4 p3;       // x=kind y=shape z=halfX w=halfZ
    // x = 池底世界高度（footprint 内最低地形）
    // y = 侧壁下沿的世界高度（池底再往下沉一点，保证被地形遮住）
    // 侧壁的"有多少米高"完全由这两个标量决定 —— 拖水位滑块不用重建网格。
    glm::vec4 p4;
    glm::vec4 screen;   // x=width y=height z=1/width w=1/height
    glm::vec4 camera;   // x=zNear y=zFar
};
static_assert(sizeof(WaterUboGpu) == 144, "WaterUboGpu 布局变了");

class WaterPass {
public:
    // 上限：一个场景里最多这么多水体同屏。超出只打一条警告并忽略 ——
    // 水体是手摆的关卡物件，几十块已经不正常了。
    static constexpr uint32_t kMaxBodies = 32;

    struct InitInfo {
        rhi::Device* device = nullptr;
        // 建/重建水体网格时上传顶点用（staging 需要一条一次性命令缓冲）
        rhi::CommandPool* cmdPool = nullptr;
        // 主前向的 set 0（**同一份**对象，不是"等价的布局"）
        VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;
        VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        uint32_t framesInFlight = 3;
    };

    WaterPass() = default;
    ~WaterPass();
    WaterPass(const WaterPass&) = delete;
    WaterPass& operator=(const WaterPass&) = delete;

    bool init(const InitInfo& info);
    void shutdown();
    bool ready() const { return m_pipeline != nullptr; }

    // 窗口/分辨率/MSAA 变化时重建：RenderPass 不重建（格式不变），
    // 但帧缓冲、颜色拷贝图、描述符集要跟着视图走。
    //   colorViews ：HdrTarget 的 resolve 图（水面**写**进去的那张）
    //   depthViews ：HdrTarget 的可采样深度图（水面**读**它做遮挡/折射）
    void setTargets(VkExtent2D extent, VkSampleCountFlagBits samples,
                    const std::vector<VkImageView>& colorViews,
                    const std::vector<VkImageView>& depthViews);

    // 场景侧每帧喂进来的水体。数量变了增删；形状/尺寸变了重建网格；
    // 只是参数变了就只写 UBO。
    void sync(const std::vector<water::WaterBody>& bodies);

    uint32_t bodyCount() const {
        return static_cast<uint32_t>(m_bodies.size());
    }

    VkRenderPass renderPass() const { return m_renderPass; }
    VkFramebuffer framebuffer(uint32_t frameIndex) const;
    VkExtent2D extent() const { return m_extent; }

    // 把前向 Pass 的 resolve 结果拷进本类的可采样图（Renderer 在
    // vkCmdEndRenderPass 之后、beginWaterPass 之前调用）。
    // 内部连布局迁移一起做完，返回时 resolve 图已回到 SHADER_READ_ONLY。
    void copySceneColor(VkCommandBuffer cmd, uint32_t frameIndex,
                        VkImage resolveImage);

    // 在**本类的 RenderPass 内**录制（调用方负责 RenderPass 的 begin/end）。
    // globalSet = 本帧的 set 0（FrameUBO），与水面的 set 1 一起在这里绑。
    // 返回本帧提交的三角形数（统计用）。
    uint64_t record(VkCommandBuffer cmd, uint32_t frameIndex, float time,
                    float zNear, float zFar, VkDescriptorSet globalSet);

private:
    struct Body;   // 完整定义在文件底部（先声明，供下面的方法签名用）

    void createRenderPass();
    void createPipeline();
    void destroyFrameTargets();
    void createBodyResources();
    bool ensureGeometry(Body& b, const water::WaterBody& src);

    rhi::Device* m_device = nullptr;
    rhi::CommandPool* m_cmdPool = nullptr;
    VkDescriptorSetLayout m_globalSetLayout = VK_NULL_HANDLE;
    VkFormat m_colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    uint32_t m_framesInFlight = 1;
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkSampler m_colorSampler = VK_NULL_HANDLE;
    VkSampler m_depthSampler = VK_NULL_HANDLE;
    std::unique_ptr<rhi::DescriptorAllocator> m_descriptors;
    std::unique_ptr<rhi::Pipeline> m_pipeline;

    struct FrameTarget {
        VkImage colorCopyImage = VK_NULL_HANDLE;
        VkDeviceMemory colorCopyMemory = VK_NULL_HANDLE;
        VkImageView colorCopyView = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
        VkImageView resolveView = VK_NULL_HANDLE;  // 借来的，不销毁
        VkImageView depthView = VK_NULL_HANDLE;    // 借来的，不销毁
        // 这张拷贝图是否已经被写过一次（决定它的"旧布局"是 UNDEFINED
        // 还是 SHADER_READ_ONLY —— 写错会撞上验证层的 layout 断言）
        bool colorCopyValid = false;
    };
    std::vector<FrameTarget> m_frames;

    // 一块水体在 GPU 上的资源
    struct Body {
        std::unique_ptr<rhi::Buffer> vbo;
        std::unique_ptr<rhi::Buffer> ibo;
        uint32_t indexCount = 0;

        // 网格是按这些值生成的；变了才重建（见 ensureGeometry）
        water::Shape shape = water::Shape::Circle;
        glm::vec2 halfExtent{0.0f};
        bool hasGeometry = false;

        // 每帧一份 UBO（不能共用：A 帧在 GPU 上读、B 帧在 CPU 上写 = 撕裂）
        std::vector<std::unique_ptr<rhi::Buffer>> ubo;
        std::vector<VkDescriptorSet> set;
        // 最近一次 sync 进来的那份数据（record 时写 UBO 用）
        water::WaterBody src;
    };
    std::vector<std::unique_ptr<Body>> m_bodies;

    VkExtent2D m_extent{0, 0};
    uint64_t m_triangles = 0;
    bool m_loggedLimit = false;
};

} // namespace render
