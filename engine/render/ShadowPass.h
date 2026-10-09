#pragma once
// ============================================================
// render/ShadowPass —— 方向光阴影贴图
//
// 实现路线（单张 shadow map + PCF）：
//   1. 光源位于场景中心沿光照反方向偏移处，用**正交投影**覆盖场景包围球
//   2. depth-only RenderPass 把场景深度渲染进 2048² 的 D32 贴图
//   3. 主 Pass 用 sampler2DShadow 采样，硬件完成深度比较，再做 3×3 PCF
//
// 为什么用正交投影：方向光的光线彼此平行，若用透视投影会出现
// 近大远小的深度偏差，无法正确表达平行光。
//
// 为什么每个 frame-in-flight 各持有一张深度图：
//   两张帧的 command buffer 可以同时在 GPU 上执行。若共用一张深度图，
//   就会出现"A 帧正在写、B 帧正在采样"的跨 command buffer 数据竞争
//   （render pass 的 subpass dependency 只在单个 command buffer 内生效）。
//
// 本 Pass 不持有描述符集：贴图由 Renderer 绑到 set 0 / binding 1
// （Vulkan 只保证至少 4 个 set，塞进 set 0 可避免超出上限）。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <memory>
#include <vector>

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

    ShadowPass(rhi::Device& device, VkDescriptorSetLayout frameSetLayout,
               uint32_t framesInFlight = 2,
               uint32_t resolution = kDefaultResolution);
    ~ShadowPass();

    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

    // 由光方向 + 场景包围球计算光源正交矩阵
    void updateLightMatrix(const glm::vec3& direction, const glm::vec3& center,
                           float radius);

    const glm::mat4& lightSpaceMatrix() const { return m_lightSpace; }
    uint32_t resolution() const { return m_resolution; }

    // 供 Renderer 写入 set 0 / binding 1（按帧取对应那张）
    VkImageView view(uint32_t frameIndex) const {
        return m_frames[frameIndex % m_frames.size()].view;
    }
    VkSampler sampler() const { return m_sampler; }

    // 在独立的 depth-only render pass 内把场景深度渲染到本帧的阴影贴图
    void render(VkCommandBuffer cmd, scene::Scene& scene,
                VkDescriptorSet frameSet, uint32_t frameIndex);

private:
    struct Frame {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
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

    glm::mat4 m_lightSpace{1.0f};
};

} // namespace render
