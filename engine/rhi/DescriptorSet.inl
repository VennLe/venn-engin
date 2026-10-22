#pragma once
// DescriptorSet.h 的内联实现部分（被 .h 尾部 include）

#include "core/Logger.h"
#include "rhi/Device.h"

namespace rhi {

inline DescriptorAllocator::DescriptorAllocator(Device& device, uint32_t maxSets,
                                                uint32_t maxUBOs,
                                                uint32_t maxSamplers,
                                                uint32_t maxStorageBuffers,
                                                uint32_t maxStorageImages,
                                                VkDescriptorPoolCreateFlags flags)
    : m_device(device) {
    // 计数为 0 的类型不进池（VUID-VkDescriptorPoolSize-descriptorCount-00302：
    // descriptorCount 必须大于 0 —— 传 0 的条目会直接触发验证层报错）
    VkDescriptorPoolSize sizes[4]{};
    uint32_t poolSizeCount = 0;
    if (maxUBOs > 0) {
        sizes[poolSizeCount].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[poolSizeCount++].descriptorCount = maxUBOs;
    }
    if (maxSamplers > 0) {
        sizes[poolSizeCount].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[poolSizeCount++].descriptorCount = maxSamplers;
    }
    if (maxStorageBuffers > 0) {
        sizes[poolSizeCount].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[poolSizeCount++].descriptorCount = maxStorageBuffers;
    }
    if (maxStorageImages > 0) {
        sizes[poolSizeCount].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[poolSizeCount++].descriptorCount = maxStorageImages;
    }

    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.poolSizeCount = poolSizeCount;
    ci.pPoolSizes = sizes;
    ci.maxSets = maxSets;
    ci.flags = flags;

    // 留底：扩容时原样复用（见 addPool）
    for (uint32_t i = 0; i < poolSizeCount; ++i) m_sizes[i] = sizes[i];
    m_sizeCount = poolSizeCount;
    m_maxSets = maxSets;
    m_flags = flags;

    VK_CHECK(vkCreateDescriptorPool(m_device.get(), &ci, nullptr, &m_pool));
    m_pools.push_back(m_pool);
}

inline bool DescriptorAllocator::addPool() {
    if (m_maxSets == 0) return false;   // 构造时没留底（不该发生）
    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.poolSizeCount = m_sizeCount;
    ci.pPoolSizes = m_sizes;
    ci.maxSets = m_maxSets;
    ci.flags = m_flags;

    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(m_device.get(), &ci, nullptr, &p) != VK_SUCCESS)
        return false;
    // ⚠ 旧池**不能**销毁：里面的 set 还在被管线引用。只把它挪到"退役"位置，
    //   新池接任当前池。
    m_pool = p;
    m_pools.push_back(p);
    VK_LOG_WARN("DESCRIPTOR-POOL 扩容：原池已满（maxSets=%u），追加第 %zu 个池",
                m_maxSets, m_pools.size());
    return true;
}

inline DescriptorAllocator::~DescriptorAllocator() {
    // 销毁**所有**池（一次 vkDestroyDescriptorPool 连带释放其中的 set）。
    for (VkDescriptorPool p : m_pools)
        if (p != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device.get(), p, nullptr);
    m_pools.clear();
    m_pool = VK_NULL_HANDLE;
}

inline VkDescriptorSet DescriptorAllocator::allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult r = vkAllocateDescriptorSets(m_device.get(), &ai, &set);

    // 池满 / 池碎片 → 追加一个同规格的池再试。
    //
    // ⚠ 只对这两个结果扩容：别的失败（OOM、layout 与池不兼容）重试也没有
    //   意义，硬试 8 次只会把真正的错误信息淹掉。
    // ⚠ 上限 8 个池：真到这一步说明每个 set 都在漏（每帧新建、从不释放），
    //   那是**泄漏**，该炸出来看日志，而不是无限建池把显存吃干净。
    constexpr int kMaxGrow = 8;
    for (int i = 0;
         (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) &&
         i < kMaxGrow;
         ++i) {
        if (!addPool()) break;
        ai.descriptorPool = m_pool;
        r = vkAllocateDescriptorSets(m_device.get(), &ai, &set);
    }

    if (r != VK_SUCCESS) {
        std::fprintf(stderr,
                     "[VK_CHECK] vkAllocateDescriptorSets 失败 %d（已扩到 %zu 个池）"
                     " at %s:%d\n",
                     static_cast<int>(r), m_pools.size(), __FILE__, __LINE__);
        throw std::runtime_error("Vulkan call failed: descriptor pool exhausted");
    }
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

inline void DescriptorAllocator::writeTextureAt(VkDescriptorSet set,
                                                uint32_t binding,
                                                uint32_t arrayElement,
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
    write.dstArrayElement = arrayElement;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(m_device.get(), 1, &write, 0, nullptr);
}

inline void DescriptorAllocator::writeStorageImage(VkDescriptorSet set,
                                                   uint32_t binding,
                                                   VkImageView view,
                                                   VkImageLayout layout) {
    VkDescriptorImageInfo info{};
    info.sampler = VK_NULL_HANDLE;   // 存储图像不配采样器
    info.imageView = view;
    info.imageLayout = layout;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(m_device.get(), 1, &write, 0, nullptr);
}

inline VkDescriptorSetLayout makeSetLayout(
    Device& device,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    const std::vector<VkDescriptorBindingFlags>& bindingFlags,
    VkDescriptorSetLayoutCreateFlags flags) {
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagInfo{};
    flagInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.data();
    ci.flags = flags;
    if (bindingFlags.size() == bindings.size() && !bindingFlags.empty()) {
        flagInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
        flagInfo.pBindingFlags = bindingFlags.data();
        ci.pNext = &flagInfo;
    }

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device.get(), &ci, nullptr, &layout));
    return layout;
}

} // namespace rhi
