#pragma once
// ============================================================
// render/OffscreenImage —— 离屏 VkImage 创建/销毁小工具
//
// HdrTarget 与 PostProcess 都要创建"颜色附件 + 可采样"的离屏图像，
// 逻辑完全一样，抽出来避免两份拷贝。header-only inline，
// 不引入额外编译单元。
// ============================================================

#include "rhi/VulkanCommon.h"
#include "rhi/Device.h"

namespace render {

// samples 默认单采样；MSAA 附件传 4/8 等。
// 注意：多采样图像**不能**带 VK_IMAGE_USAGE_SAMPLED_BIT 却当普通 sampler2D
// 采样 —— 需要采样时得先 resolve 到一张单采样图（见 HdrTarget）。
inline void createImage2D(rhi::Device& device, uint32_t w, uint32_t h,
                          VkFormat format, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect, VkImage& outImage,
                          VkDeviceMemory& outMemory, VkImageView& outView,
                          VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
    VkDevice dev = device.get();

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, nullptr, &outImage));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, outImage, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &outMemory));
    VK_CHECK(vkBindImageMemory(dev, outImage, outMemory, 0));

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = outImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &outView));
}

inline void destroyImage2D(rhi::Device& device, VkImage& image,
                           VkDeviceMemory& memory, VkImageView& view) {
    VkDevice dev = device.get();
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(dev, image, nullptr);
        image = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
}

} // namespace render
