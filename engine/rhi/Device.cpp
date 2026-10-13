#include "rhi/Device.h"
#include "core/Logger.h"

namespace rhi {

namespace {

bool isDeviceSuitable(VkPhysicalDevice dev, VkSurfaceKHR surface,
                      uint32_t* outFamily) {
    // 1) 图形队列 & 支持呈现
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());

    int family = -1;
    for (uint32_t i = 0; i < count; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            family = static_cast<int>(i);
            break;
        }
    }
    if (family < 0) return false;

    // 2) 交换链扩展
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    if (extCount > 0) vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());
    bool hasSwapchain = false;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            hasSwapchain = true;
            break;
        }
    }
    if (!hasSwapchain) return false;

    *outFamily = static_cast<uint32_t>(family);
    return true;
}

} // namespace

Device::Device(VkInstance instance, VkSurfaceKHR surface)
    : m_instance(instance), m_surface(surface) {
    if (!pickPhysicalDevice()) {
        throw std::runtime_error("No suitable Vulkan device found");
    }
    createLogicalDevice();
    VK_LOG_INFO("Device created: %s (graphics family %u)", m_props.deviceName,
                m_graphicsFamily);
}

Device::~Device() {
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
}

bool Device::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        VK_LOG_ERROR("No Vulkan physical devices found!");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    uint32_t bestFamily = UINT32_MAX;
    int bestScore = -1;

    for (auto dev : devices) {
        uint32_t family = UINT32_MAX;
        if (!isDeviceSuitable(dev, m_surface, &family)) continue;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)  ? 1000
                  : (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 100
                  : 10;
        if (score > bestScore) {
            bestScore = score;
            best = dev;
            bestFamily = family;
        }
    }
    if (best == VK_NULL_HANDLE) return false;

    m_physical = best;
    m_graphicsFamily = bestFamily;
    vkGetPhysicalDeviceProperties(best, &m_props);
    return true;
}

void Device::createLogicalDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueCI{};
    queueCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCI.queueFamilyIndex = m_graphicsFamily;
    queueCI.queueCount = 1;
    queueCI.pQueuePriorities = &priority;

    // 独立深度/模板布局（Vulkan 1.2 核心特性）：HdrTarget 的共享深度
    // 方案把深度附件的 initial/finalLayout 设成 DEPTH_READ_ONLY_OPTIMAL
    // （深度预通道 → SSAO/FrameGraph 采样同一条深度图）—— 不开这个
    // 特性该布局直接非法（验证层 VUID 一串）。查询支持并显式启用。
    VkPhysicalDeviceVulkan12Features vk12{};
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 feats2{};
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats2.pNext = &vk12;
    vkGetPhysicalDeviceFeatures2(m_physical, &feats2);
    if (!vk12.separateDepthStencilLayouts) {
        throw std::runtime_error(
            "Device lacks separateDepthStencilLayouts (required by the "
            "shared-depth screen-space pipeline)");
    }
    vk12.separateDepthStencilLayouts = VK_TRUE;

    // 各向异性过滤（可选特性：有就用）。走 Features2 链声明 ——
    // pNext 一旦挂了 12Features，pEnabledFeatures 必须为 NULL。
    if (feats2.features.samplerAnisotropy) {
        feats2.features.samplerAnisotropy = VK_TRUE;
        m_maxAnisotropy = m_props.limits.maxSamplerAnisotropy;
        if (m_maxAnisotropy > 8.0f) m_maxAnisotropy = 8.0f;
    } else {
        m_maxAnisotropy = 1.0f;
    }

    // 采样器数组的动态索引（Vulkan 1.0 核心特性，桌面驱动基本全有）。
    // bindless 的 `textures[材质页.tex0.x]` 需要它 —— 不过请求它并不会
    // 影响任何既有路径：没有着色器索引采样器数组时它等于没开。
    if (feats2.features.shaderSampledImageArrayDynamicIndexing) {
        feats2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    }

    // ---- Bindless / descriptor indexing（R2-3）----
    // 一整组特性，**要么全开要么全不开**：只开一半会让"运行时补注册纹理"
    // 或"数组尾部留空"变成未定义行为，而且往往要等到跑到某个场景才崩。
    // 缺任何一个就整体不去 set，supportsBindless() 返回 false。
    const bool bindlessOK =
        vk12.descriptorIndexing &&
        vk12.runtimeDescriptorArray &&
        vk12.shaderSampledImageArrayNonUniformIndexing &&
        vk12.descriptorBindingPartiallyBound &&
        vk12.descriptorBindingSampledImageUpdateAfterBind;
    if (bindlessOK) {
        vk12.descriptorIndexing = VK_TRUE;
        vk12.runtimeDescriptorArray = VK_TRUE;
        // 索引来自 push 常量（逐 draw 一次 = 动态 uniform），严格说
        // nonUniform 用不上；照样开是为了让"以后把材质页塞进 SSBO 逐
        // 片元查"（R2-4 visibility buffer）不用回头改设备特性。
        vk12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        vk12.descriptorBindingPartiallyBound = VK_TRUE;
        vk12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        m_descriptorIndexing = true;
    }
    VK_LOG_INFO("Device: descriptor indexing %s",
                m_descriptorIndexing ? "available (bindless usable)"
                                     : "UNAVAILABLE (bindless disabled)");

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &feats2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &queueCI;
    ci.pEnabledFeatures = nullptr;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = extensions;

    VK_CHECK(vkCreateDevice(m_physical, &ci, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_queue);
}

uint32_t Device::findMemoryType(uint32_t typeFilter,
                                VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(m_physical, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    throw std::runtime_error("findMemoryType: no suitable memory type");
}

VkFormat Device::findSupportedFormat(const std::vector<VkFormat>& candidates,
                                     VkImageTiling tiling,
                                     VkFormatFeatureFlags features) const {
    for (VkFormat fmt : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_physical, fmt, &props);
        VkFormatFeatureFlags flags = (tiling == VK_IMAGE_TILING_LINEAR)
                                         ? props.linearTilingFeatures
                                         : props.optimalTilingFeatures;
        if ((flags & features) == features) return fmt;
    }
    throw std::runtime_error("findSupportedFormat: none of the candidates fit");
}

VkFormat Device::findDepthFormat() const {
    return findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
         VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM},
        VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

} // namespace rhi
