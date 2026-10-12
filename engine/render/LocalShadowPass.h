#pragma once
// ============================================================
// render/LocalShadowPass —— 局部光源阴影（点光 / 聚光灯）
//
// 与级联方向光（ShadowPass）并列的第二套阴影通道。两者**不共用深度图**：
//   · 级联：2048² × 4 层，正交投影，覆盖相机视锥切片；
//   · 局部：1024² × 12 层，透视投影，覆盖每盏灯的锥体 / 立方体。
// 分辨率与用途都不同，硬塞进同一张 array 只会让两边互相将就。
//
// ---- 层分配 ----
// 所有需要投影的局部光源共用一张 2D array 深度图：
//   · 聚光灯占 **1 层**（沿锥轴一次透视投影）；
//   · 点光占 **6 层**（立方体 6 个面各一次 90° 透视投影）。
// 层号由 Renderer 每帧分配（见 Renderer::prepareLocalShadows），分配结果
// 通过两处告知采样侧，二者同源：
//   1. LightInstance::cosOuterType.z = 基准层号（写进 set0/binding2 的灯表）
//   2. set0/binding11 的 LightShadowRecord 数组（逐层一个矩阵）
//
// ---- 矩阵只有一份副本 ----
// 世界 → 光源裁剪矩阵**只存在** binding11 的 SSBO 里：本 Pass 渲染时从它
// 取（shadow.vert 的 push.which.y == 1），pbr.frag 采样时也读它。所以不可能
// 出现"渲染与采样用了不同矩阵"——与 CSM 的 FrameUBO.lightSpace[] 同一条约定。
//
// ---- 为什么必须有"空层也要清" ----
// 未使用的层不渲染几何，但**每帧仍要走一次空的 render pass**（只清深度）。
// 否则那张层会停在 UNDEFINED / 上一帧的布局，而它挂在同一个 array 视图上
// 被采样 —— 验证层立刻报 IMAGE_LAYOUT 错。清一遍是"快清"，
// 代价远小于"按需重建 image / 维护 per-frame 布局状态机"。
//
// 本 Pass 不持有 set 0：贴图由 Renderer 绑到 set 0 / binding 12。
// ============================================================

#include "render/RenderTypes.h"
#include "rhi/VulkanCommon.h"

#include <glm/glm.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace assets {
class Mesh;
}

namespace rhi {
class Device;
class Pipeline;
}

namespace scene {
class Scene;
struct LightInstance;
} // namespace scene

namespace render {

// 立方体 6 个面的视图方向与 up（标准 GL / Vulkan 立方体面约定）。
// **必须与 assets/shaders/common.glsl 的 vennCubeDir 表逐条对应** ——
// 那张表是这套视图矩阵的反解。对不上时的症状是"某个面的阴影跑到相邻
// 面上"，在画面上表现为一片错位的暗区，极难定位。
//
// 注意 up 用的是立方体自身的轴（不是世界 up）：Z-up 世界照样适用，
// 因为面序是立方体的内禀性质，与世界朝向无关。
extern const glm::vec3 kCubeFaceDir[6];
extern const glm::vec3 kCubeFaceUp[6];

// 计算单盏局部光源投射阴影所需的矩阵，返回条数：
//   点光 6 条（立方体 6 面，顺序 = kCubeFaceDir），聚光灯 1 条，
//   其它（含 Directional）0 条。
// 全部使用 *_RH_ZO 投影 —— Vulkan 的 clip z 是 [0,1]。
// out 容量不足时截断并返回实际写入条数（调用方按 kPointLightFaces 备足）。
//
// outNear / outFar（可传 nullptr）：本次投影的近 / 远平面（米）。
// 采样侧要靠它们把"世界单位的深度偏移"换算成 NDC（见
// pbr.frag::ndcBiasFromWorld）—— 透视投影的 NDC 深度是双曲的，
// 没有这一对数值就没法给出正确的偏移。
uint32_t buildLocalLightMatrices(const scene::LightInstance& li, glm::mat4* out,
                                 uint32_t maxOut, float* outNear = nullptr,
                                 float* outFar = nullptr);

class LocalShadowPass {
public:
    static constexpr uint32_t kDefaultResolution = 1024;
    static constexpr uint32_t kMaxLayers = kMaxLocalShadowLayers;

    LocalShadowPass(rhi::Device& device, VkDescriptorSetLayout frameSetLayout,
                    uint32_t framesInFlight = MAX_FRAMES_IN_FLIGHT,
                    uint32_t resolution = kDefaultResolution);
    ~LocalShadowPass();

    LocalShadowPass(const LocalShadowPass&) = delete;
    LocalShadowPass& operator=(const LocalShadowPass&) = delete;

    uint32_t resolution() const { return m_resolution; }
    uint32_t layerCount() const { return kMaxLayers; }

    // 供 Renderer 写入 set 0 / binding 12（2D array 视图，按帧取）
    VkImageView view(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].arrayView;
    }
    VkSampler sampler() const { return m_sampler; }

    // 在独立的 depth-only render pass 内渲染**第 layer 层**。
    //   clip == nullptr → 本层未使用：只清深度，不提交几何。
    // jointBases：蒙皮网格的关节矩阵起始下标（与主前向路径同源）。
    void render(VkCommandBuffer cmd, scene::Scene& scene,
                VkDescriptorSet frameSet, uint32_t frameIndex, uint32_t layer,
                const glm::mat4* clip,
                const std::unordered_map<const assets::Mesh*, int>* jointBases);

    // 本帧第 layer 层的提交统计（验证脚本用：证明剔除真的在省）
    uint32_t submitted(uint32_t layer) const;
    uint32_t considered(uint32_t layer) const;
    void resetStats();

private:
    struct Frame {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        // 采样用：全部层一起
        VkImageView arrayView = VK_NULL_HANDLE;
        // 渲染用：每层一个 2D 视图（framebuffer 不能挂 array 视图，
        // 那会变成 layered rendering，需要 multiview / gl_Layer）
        VkImageView layerView[kMaxLayers] = {};
        VkFramebuffer framebuffer[kMaxLayers] = {};
    };

    void createResource();
    void createFrame(Frame& f);
    void destroyFrame(Frame& f);
    void createPipeline();

    rhi::Device& m_device;
    VkDescriptorSetLayout m_frameSetLayout = VK_NULL_HANDLE;

    uint32_t m_resolution = kDefaultResolution;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

    VkSampler m_sampler = VK_NULL_HANDLE;  // compare sampler（各帧共用）
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::unique_ptr<rhi::Pipeline> m_pipeline;

    std::vector<Frame> m_frames;

    // 本帧各层的剔除统计（render 时填）
    uint32_t m_submitted[kMaxLayers] = {};
    uint32_t m_considered[kMaxLayers] = {};
};

} // namespace render
