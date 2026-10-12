#pragma once
// ============================================================
// render/ShadowPass —— 级联方向光阴影贴图（CSM）
//
// 从"单张 2048² 正交阴影图"演进而来，现在是 **4 级级联 2D array + PCF**：
//   1. 每级一个正交矩阵，由**相机视锥切片的包围球**求出（updateCascades）
//   2. 深度图是 VK_IMAGE_VIEW_TYPE_2D_ARRAY，第 i 层 = 第 i 级
//      → 采样侧用 sampler2DArrayShadow，一次采样顺便选级
//   3. 每级都在**裁剪空间**做一次 AABB 剔除（visibleInCascade），否则
//      距离一拉开，投影体的提交量就变成全场景的 4 倍
//
// 两个"稳定化"措施，缺一个相机一动就会看到阴影边缘闪：
//   · **每级用包围球而不是包围盒**：球的半径与光方向无关，光一转半径不变；
//     盒会跟着方向变，必须抖。
//   · **球心在光空间里量化到纹素网格**：相机连续平移时球心连续移动，
//     采样点相对纹素滑动 → 整片边缘闪烁；量化后只在纹素边界跳变。
//
// 矩阵只有一个副本：4 个矩阵**只存在 FrameUBO.lightSpace[4] 里**。本 Pass
// 算完写进去，渲染时从同一个数组取（靠推常量里的级联索引选），采样侧
// pbr.frag 读的也是它 —— 所以不可能出现"渲染与采样用了不同矩阵"。
//
// 本 Pass 不持有 set 0：贴图由 Renderer 绑到 set 0 / binding 1。
// ============================================================

#include "render/RenderTypes.h"
#include "rhi/VulkanCommon.h"

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
}

namespace render {

class ShadowPass {
public:
    static constexpr uint32_t kDefaultResolution = 2048;
    static constexpr uint32_t kMaxCascades = kShadowCascadeMax;  // = 4

    // 级联矩阵算出来后，给切片包围球额外加的"沿光轴的外扩距离"。
    // 没有它，切片外（更靠近光源一侧）的遮挡体不会进深度图，
    // 于是高墙投到近处的影子会被截断。
    static constexpr float kDepthPadding = 25.0f;

    ShadowPass(rhi::Device& device, VkDescriptorSetLayout frameSetLayout,
               uint32_t framesInFlight = 2,
               uint32_t resolution = kDefaultResolution);
    ~ShadowPass();

    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

    // 由相机视锥 + 光方向算出 N 级级联的光源空间矩阵。
    //   camView / camProj : 相机的视图 / 投影矩阵（proj 已含 Vulkan 的 Y 翻转，
    //                       本函数用它的逆来反投影视锥角点，翻不翻都自洽）
    //   camNear / camFar  : **必须与 camProj 用的是同一组值**，分割点按线性
    //                       视深度计算，拿别的值算出来的切片会对不上
    //   lightDir          : 光的**传播**方向（光 → 场景），与 FrameUBO 一致
    //   maxDistance       : 最远阴影距离（世界单位）
    //   lambda            : log / uniform 混合系数（0 = 全均匀，1 = 全对数）
    void updateCascades(const glm::mat4& camView, const glm::mat4& camProj,
                        float camNear, float camFar,
                        const glm::vec3& lightDir, float maxDistance,
                        float lambda);

    uint32_t cascadeCount() const { return m_cascadeCount; }
    void setCascadeCount(uint32_t n);
    float maxDistance() const { return m_maxDistance; }
    const glm::mat4& lightSpace(uint32_t i) const {
        return m_lightSpace[i < kMaxCascades ? i : kMaxCascades - 1];
    }
    // 第 i 级覆盖到的远端世界距离（分割点）
    float split(uint32_t i) const {
        return m_splits[i < kMaxCascades ? i : kMaxCascades - 1];
    }

    uint32_t resolution() const { return m_resolution; }
    // 供 Renderer 写入 set 0 / binding 1（2D array 视图，按帧取）
    VkImageView view(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].arrayView;
    }
    VkSampler sampler() const { return m_sampler; }

    // 在独立的 depth-only render pass 内渲染**第 cascade 级**。
    // jointBases：蒙皮网格的关节矩阵起始下标（与主前向路径同源）。
    // 传 nullptr 表示场景里没有蒙皮网格。
    void render(VkCommandBuffer cmd, scene::Scene& scene,
                VkDescriptorSet frameSet, uint32_t frameIndex,
                uint32_t cascade,
                const std::unordered_map<const assets::Mesh*, int>* jointBases);

    // 本帧第 cascade 级的提交统计（验证脚本用：证明剔除真的在省）
    uint32_t submitted(uint32_t cascade) const;
    uint32_t considered(uint32_t cascade) const;

private:
    struct Frame {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        // 采样用：2D array 视图（4 层一起）
        VkImageView arrayView = VK_NULL_HANDLE;
        // 渲染用：每一层一个 2D 视图（framebuffer 不能直接挂 array 视图，
        // 那会变成 layered rendering，需要 multiview / gl_Layer）
        VkImageView layerView[kMaxCascades] = {};
        VkFramebuffer framebuffer[kMaxCascades] = {};
    };

    void createResource();
    void createFrame(Frame& f);
    void destroyFrame(Frame& f);
    void createPipeline();

    rhi::Device& m_device;
    VkDescriptorSetLayout m_frameSetLayout = VK_NULL_HANDLE;

    uint32_t m_resolution = kDefaultResolution;
    uint32_t m_cascadeCount = kMaxCascades;
    float m_maxDistance = 0.0f;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

    VkSampler m_sampler = VK_NULL_HANDLE;  // compare sampler（各帧共用）
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::unique_ptr<rhi::Pipeline> m_pipeline;

    std::vector<Frame> m_frames;

    glm::mat4 m_lightSpace[kMaxCascades];
    float m_splits[kMaxCascades] = {0.0f, 0.0f, 0.0f, 0.0f};

    // 本帧各级的剔除统计（render 时填）
    uint32_t m_submitted[kMaxCascades] = {};
    uint32_t m_considered[kMaxCascades] = {};
};

} // namespace render
