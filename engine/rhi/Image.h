#pragma once
// ============================================================
// rhi/Image —— VkImage + 内存 + 视图 RAII 封装
// 提供布局转换 / buffer→image 拷贝等静态工具
// ============================================================

#include "rhi/VulkanCommon.h"

namespace rhi {

class Device;
class CommandPool;

class Image {
public:
    Image(Device& device, uint32_t width, uint32_t height, VkFormat format,
          VkImageUsageFlags usage, VkImageTiling tiling,
          VkMemoryPropertyFlags memoryProps,
          VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    VkImage get() const { return m_image; }
    VkImageView view() const { return m_view; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    VkFormat format() const { return m_format; }

    // ---- 工具函数 ----
    static void transitionLayout(Device& device, CommandPool& cmdPool,
                                 VkImage image, VkImageLayout oldLayout,
                                 VkImageLayout newLayout,
                                 uint32_t mipLevels = 1, uint32_t layerCount = 1);

    static void copyBufferToImage(Device& device, CommandPool& cmdPool,
                                  VkBuffer buffer, VkImage image, uint32_t width,
                                  uint32_t height);

private:
    Device& m_device;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
};

} // namespace rhi
