#pragma once
// ============================================================
// render/FrameResources —— 每帧在途资源（MAX_FRAMES_IN_FLIGHT 组）
// 每组包含：命令缓冲 / 栅栏 / 两个信号量 / 全局 UBO
// 由 Renderer 创建与销毁，这里只做聚合
// ============================================================

#include "rhi/VulkanCommon.h"
#include "rhi/Buffer.h"

#include <memory>

namespace render {

struct FrameResources {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;      // CPU-GPU 帧同步
    VkSemaphore imageAvailable = VK_NULL_HANDLE; // 获取图像完成
    // 注意：渲染完成信号量不在这里——它必须"每张交换链图像一个"，
    // 由 Renderer::m_renderFinished 管理，避免与呈现操作复用冲突

    std::unique_ptr<rhi::Buffer> globalUBO;      // HOST_VISIBLE，持久映射
    void* uboMapped = nullptr;
    VkDescriptorSet globalUBOSet = VK_NULL_HANDLE;  // set 0 绑定 UBO
};

} // namespace render
