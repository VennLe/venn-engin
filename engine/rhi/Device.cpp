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

    // 各向异性过滤（可选特性：有就用）
    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m_physical, &supported);
    if (supported.samplerAnisotropy) {
        features.samplerAnisotropy = VK_TRUE;
        m_maxAnisotropy = m_props.limits.maxSamplerAnisotropy;
        if (m_maxAnisotropy > 8.0f) m_maxAnisotropy = 8.0f;
    } else {
        m_maxAnisotropy = 1.0f;
    }

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &queueCI;
    ci.pEnabledFeatures = &features;
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
