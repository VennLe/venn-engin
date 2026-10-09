#pragma once
// ============================================================
// rhi/DescriptorSet —— 描述符池 + 布局创建 + 写入工具
// v1 场景：全局 UBO（set 0）与纹理采样器（set 1）
// ============================================================

#include "rhi/VulkanCommon.h"
#include <vector>

namespace rhi {

class Device;
class Buffer;

class DescriptorAllocator {
public:
    // maxStorageBuffers > 0 时才会在池里预留 STORAGE_BUFFER 描述符
    // （分簇光照的 lights/clusters/lightIndex 三个 SSBO 需要它）
    DescriptorAllocator(Device& device, uint32_t maxSets, uint32_t maxUBOs,
                        uint32_t maxSamplers, uint32_t maxStorageBuffers = 0);
    ~DescriptorAllocator();

    DescriptorAllocator(const DescriptorAllocator&) = delete;
    DescriptorAllocator& operator=(const DescriptorAllocator&) = delete;

    VkDescriptorPool pool() const { return m_pool; }

    VkDescriptorSet allocate(VkDescriptorSetLayout layout);

    void writeBuffer(VkDescriptorSet set, uint32_t binding, VkBuffer buffer,
                     VkDeviceSize range, VkDescriptorType type =
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    // layout 默认为 SHADER_READ_ONLY_OPTIMAL；深度贴图（阴影）需传
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL，否则与 render pass 的实际布局不符。
    void writeTexture(VkDescriptorSet set, uint32_t binding, VkSampler sampler,
                      VkImageView view,
                      VkImageLayout layout =
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

private:
    Device& m_device;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
};

// 便捷：创建单个 set layout
inline VkDescriptorSetLayout makeSetLayout(
    Device& device,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings);

} // namespace rhi

// 实现放头文件内联，避免额外 .cpp
#include "rhi/DescriptorSet.inl"
