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
    // （分簇光照的 lights/clusters/lightIndex 三个 SSBO 需要它）；
    // maxStorageImages > 0 时预留 STORAGE_IMAGE（Hi-Z 金字塔逐 mip 写入需要它）
    //
    // flags 只在 bindless 池用得上：`VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT`
    // 允许集合在**队列里还在用**的时候更新描述符（运行时注册新纹理必需）。
    // 该池里分配的 set 必须来自带 UPDATE_AFTER_BIND_POOL 位的 layout。
    DescriptorAllocator(Device& device, uint32_t maxSets, uint32_t maxUBOs,
                        uint32_t maxSamplers, uint32_t maxStorageBuffers = 0,
                        uint32_t maxStorageImages = 0,
                        VkDescriptorPoolCreateFlags flags = 0);
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

    // 写**数组型**绑定的第 arrayElement 个元素（bindless 的 textures[]）。
    // 与 writeTexture 的唯一区别是 dstArrayElement —— 用 uint32_t 下标
    // 定位槽位，而不是整个绑定。池/布局必须带 UPDATE_AFTER_BIND 位。
    void writeTextureAt(VkDescriptorSet set, uint32_t binding,
                        uint32_t arrayElement, VkSampler sampler,
                        VkImageView view,
                        VkImageLayout layout =
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 存储图像（compute 里用 imageStore/imageLoad 读写）。storage image
    // 只能处于 GENERAL 布局（其它布局下读写未定义），所以 layout 默认 GENERAL。
    void writeStorageImage(VkDescriptorSet set, uint32_t binding,
                           VkImageView view,
                           VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL);

private:
    Device& m_device;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
};

// 便捷：创建单个 set layout
//
// bindingFlags 非空时（长度必须 == bindings.size()）挂
// VkDescriptorSetLayoutBindingFlagsCreateInfo —— bindless 的
// PARTIALLY_BOUND / UPDATE_AFTER_BIND 位只能从这里给，不能写在
// VkDescriptorSetLayoutBinding 里。flags 同理（UPDATE_AFTER_BIND_POOL）。
inline VkDescriptorSetLayout makeSetLayout(
    Device& device,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    const std::vector<VkDescriptorBindingFlags>& bindingFlags = {},
    VkDescriptorSetLayoutCreateFlags flags = 0);

} // namespace rhi

// 实现放头文件内联，避免额外 .cpp
#include "rhi/DescriptorSet.inl"
