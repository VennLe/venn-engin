#pragma once
// DescriptorSet.h 的内联实现部分（被 .h 尾部 include）

#include "rhi/Device.h"

namespace rhi {

inline DescriptorAllocator::DescriptorAllocator(Device& device, uint32_t maxSets,
                                                uint32_t maxUBOs,
                                                uint32_t maxSamplers,
                                                uint32_t maxStorageBuffers)
    : m_device(device) {
    VkDescriptorPoolSize sizes[3]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = maxUBOs;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = maxSamplers;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[2].descriptorCount = maxStorageBuffers;

    // descriptorCount 为 0 的 pool size 是合法的，但没必要带进去
    const uint32_t poolSizeCount = maxStorageBuffers > 0 ? 3u : 2u;

    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.poolSizeCount = poolSizeCount;
    ci.pPoolSizes = sizes;
    ci.maxSets = maxSets;

    VK_CHECK(vkCreateDescriptorPool(m_device.get(), &ci, nullptr, &m_pool));
}

inline DescriptorAllocator::~DescriptorAllocator() {
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device.get(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

inline VkDescriptorSet DescriptorAllocator::allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(m_device.get(), &ai, &set));
    return set;
}

inline void DescriptorAllocator::writeBuffer(
    VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize range,
    VkDescriptorType type) {
    VkDescriptorBufferInfo info{};
    info.buffer = buffer;
    info.offset = 0;
    info.range = range;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(m_device.get(), 1, &write, 0, nullptr);
}

inline void DescriptorAllocator::writeTexture(VkDescriptorSet set,
                                              uint32_t binding,
                                              VkSampler sampler,
                                              VkImageView view,
                                              VkImageLayout layout) {
    VkDescriptorImageInfo info{};
    info.sampler = sampler;
    info.imageView = view;
    info.imageLayout = layout;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(m_device.get(), 1, &write, 0, nullptr);
}

inline VkDescriptorSetLayout makeSetLayout(
    Device& device,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device.get(), &ci, nullptr, &layout));
    return layout;
}

} // namespace rhi
