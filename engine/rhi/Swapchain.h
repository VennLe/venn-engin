#pragma once
// ============================================================
// rhi/Swapchain —— 交换链封装
// 负责：交换链创建/重建、图像视图、深度缓冲
// Surface 格式固定偏好 B8G8R8A8_SRGB，呈现模式 FIFO
// ============================================================

#include "rhi/VulkanCommon.h"
#include <vector>

namespace rhi {

class Device;

class Swapchain {
public:
    Swapchain(Device& device, VkSurfaceKHR surface, uint32_t width, uint32_t height);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void recreate(uint32_t width, uint32_t height);

    VkSwapchainKHR get() const { return m_swapchain; }
    VkFormat format() const { return m_format; }
    VkExtent2D extent() const { return m_extent; }
    uint32_t imageCount() const { return m_images.size(); }
    const std::vector<VkImage>& images() const { return m_images; }
    const std::vector<VkImageView>& views() const { return m_views; }
    VkImageView depthView() const { return m_depthView; }
    VkFormat depthFormat() const { return m_depthFormat; }

private:
    void create(uint32_t width, uint32_t height, VkSwapchainKHR old);
    void destroyResources();

    Device& m_device;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};

    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;

    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
    VkImageView m_depthView = VK_NULL_HANDLE;
};

} // namespace rhi
