#pragma once
// ============================================================
// rhi/Device —— 物理设备选择 + 逻辑设备 + 队列
// 选择策略：优先独显 > 核显；必须支持图形/呈现队列与交换链扩展
// ============================================================

#include "rhi/VulkanCommon.h"
#include <vector>

namespace rhi {

class Device {
public:
    Device(VkInstance instance, VkSurfaceKHR surface);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkPhysicalDevice physical() const { return m_physical; }
    VkDevice get() const { return m_device; }
    uint32_t graphicsFamily() const { return m_graphicsFamily; }
    VkQueue graphicsQueue() const { return m_queue; }

    void waitIdle() const { vkDeviceWaitIdle(m_device); }

    uint32_t findMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags props) const;

    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates,
                                 VkImageTiling tiling,
                                 VkFormatFeatureFlags features) const;
    VkFormat findDepthFormat() const;

    const VkPhysicalDeviceProperties& properties() const { return m_props; }
    float maxSamplerAnisotropy() const { return m_maxAnisotropy; }

private:
    bool pickPhysicalDevice();
    void createLogicalDevice();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_graphicsFamily = UINT32_MAX;
    VkQueue m_queue = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_props{};
    float m_maxAnisotropy = 1.0f;
};

} // namespace rhi
