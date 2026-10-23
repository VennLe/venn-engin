#include "rhi/Swapchain.h"
#include "rhi/Device.h"
#include "core/Logger.h"
#include "core/RunMode.h"

#include <algorithm>

namespace rhi {

Swapchain::Swapchain(Device& device, VkSurfaceKHR surface, uint32_t width,
                     uint32_t height)
    : m_device(device), m_surface(surface) {
    m_depthFormat = device.findDepthFormat();
    create(width, height, VK_NULL_HANDLE);
    VK_LOG_INFO("Swapchain created: %u x %u, %u images",
                m_extent.width, m_extent.height, (uint32_t)m_images.size());
}

Swapchain::~Swapchain() { destroyResources(); }

void Swapchain::recreate(uint32_t width, uint32_t height) {
    m_device.waitIdle();
    destroyResources();
    create(width, height, VK_NULL_HANDLE);
    VK_LOG_INFO("Swapchain recreated: %u x %u, %u images",
                m_extent.width, m_extent.height, (uint32_t)m_images.size());
}

void Swapchain::create(uint32_t width, uint32_t height, VkSwapchainKHR old) {
    VkDevice device = m_device.get();

    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_device.physical(),
                                                       m_surface, &caps));

    // ---- 表面格式 ----
    uint32_t fmtCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(m_device.physical(), m_surface,
                                                  &fmtCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(m_device.physical(), m_surface,
                                                  &fmtCount, formats.data()));
    m_format = formats[0].format;
    VkColorSpaceKHR colorSpace = formats[0].colorSpace;
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB ||
             f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            m_format = f.format;
            colorSpace = f.colorSpace;
            break;
        }
    }

    // ---- 呈现模式 ----
    //
    // 2026-10-23 改：默认 **FIFO（锁垂直同步）**，不再是"有 MAILBOX 就用
    // MAILBOX"。
    //
    // 原因（用户报的"角色在移动时、尤其是鼠标转视角时动作明显卡顿"）：
    //   MAILBOX 不限制提交速率 —— 编辑器的空场景主循环实测跑到 **470 fps**
    //   左右（dt 均值 2.1 ms、标准差 0.63 ms），而这块屏是 **60 Hz**。
    //   渲染帧与显示刷新不是整数倍关系，合成器每次取的"最新一帧"距上次
    //   刷新的时间长短不一（0~2 ms 的抖动摊在 16.7 ms 上就是 ±12%），
    //   于是画面里的匀速运动（转视角时最明显：整个视野都在动）推进得不
    //   均匀 —— 肉眼就是"一顿一顿"，走路的步频也跟着忽快忽慢。
    //   锁到 FIFO 之后每帧严格对应一次刷新，步长恒定，帧率也顺带从 470
    //  降到 60（顺带省掉 87% 的 GPU 空转）。
    //
    // 谁需要不锁：自动化（跑满 N 帧 / 逐帧钩子 / 截图对比都要"越快越好"），
    //   以及量 GPU 峰值的手工测试 → `MYVK_VSYNC=0`，见 core/RunMode.h。
    uint32_t pmCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(m_device.physical(),
                                                       m_surface, &pmCount, nullptr));
    std::vector<VkPresentModeKHR> modes(pmCount);
    if (pmCount > 0) {
        VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(
            m_device.physical(), m_surface, &pmCount, modes.data()));
    }
    const bool wantVsync = core::vsyncEnabled();
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!wantVsync) {
        // 不锁的时候优先 MAILBOX（不撕裂的"尽快呈现"）；没有就退到 FIFO
        // —— FIFO 是规范保证一定存在的模式，退过去只是变回锁帧，不会失败。
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
                presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                break;
            }
        }
    }
    VK_LOG_INFO("Swapchain present mode = %s (vsync=%d)",
                presentMode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : "MAILBOX",
                wantVsync ? 1 : 0);

    // ---- 图像数量：min + 1，不超上限 ----
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    // ---- 尺寸 ----
    if (caps.currentExtent.width != UINT32_MAX) {
        m_extent = caps.currentExtent;
    } else {
        m_extent.width = std::clamp(width, caps.minImageExtent.width,
                                    caps.maxImageExtent.width);
        m_extent.height = std::clamp(height, caps.minImageExtent.height,
                                     caps.maxImageExtent.height);
    }

    // ---- 创建交换链 ----
    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = m_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = m_format;
    ci.imageColorSpace = colorSpace;
    ci.imageExtent = m_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;  // 图形/呈现同队列族
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;

    VK_CHECK(vkCreateSwapchainKHR(device, &ci, nullptr, &m_swapchain));
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, old, nullptr);
    }

    // ---- 图像 + 视图 ----
    uint32_t count = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device, m_swapchain, &count, nullptr));
    m_images.resize(count);
    VK_CHECK(vkGetSwapchainImagesKHR(device, m_swapchain, &count, m_images.data()));

    m_views.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = m_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel = 0;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &vi, nullptr, &m_views[i]));
    }

    // ---- 深度缓冲 ----
    VkImageCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    di.imageType = VK_IMAGE_TYPE_2D;
    di.format = m_depthFormat;
    di.extent = {m_extent.width, m_extent.height, 1};
    di.mipLevels = 1;
    di.arrayLayers = 1;
    di.samples = VK_SAMPLE_COUNT_1_BIT;
    di.tiling = VK_IMAGE_TILING_OPTIMAL;
    di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VK_CHECK(vkCreateImage(device, &di, nullptr, &m_depthImage));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, m_depthImage, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &m_depthMemory));
    VK_CHECK(vkBindImageMemory(device, m_depthImage, m_depthMemory, 0));

    VkImageViewCreateInfo dvi{};
    dvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dvi.image = m_depthImage;
    dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvi.format = m_depthFormat;
    dvi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    dvi.subresourceRange.levelCount = 1;
    dvi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device, &dvi, nullptr, &m_depthView));
}

void Swapchain::destroyResources() {
    VkDevice device = m_device.get();
    if (m_depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_depthView, nullptr);
        m_depthView = VK_NULL_HANDLE;
    }
    if (m_depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_depthImage, nullptr);
        m_depthImage = VK_NULL_HANDLE;
    }
    if (m_depthMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_depthMemory, nullptr);
        m_depthMemory = VK_NULL_HANDLE;
    }
    for (auto v : m_views) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(device, v, nullptr);
    }
    m_views.clear();
    m_images.clear();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

} // namespace rhi
