#include "rhi/Instance.h"
#include "core/Build.h"
#include "core/Logger.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdlib>

namespace rhi {

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        VK_LOG_ERROR("[validation] %s", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        VK_LOG_WARN("[validation] %s", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        // info 太多，默认只在 Debug 级别打印
        VK_LOG_DEBUG("[validation] %s", data->pMessage);
    }
    return VK_FALSE;
}

Instance::Instance(bool requestValidation) {
    // G30：出货构建**不装**校验层。理由不是"省一点开销"，而是玩家机器上
    // 大概率没装 `VK_LAYER_KHRONOS_validation` —— 那时代码会照常创建实例、
    // 只是白查一次层列表；更糟的是**开发机上有层**，于是"我这里 0 报错"
    // 变成假象。所以 shipping 下这条是**强制关**，env 也不给翻盘的口子
    // （要验校验层就用 dev/QA 版，那是同一个源码的另一种构建）。
    if (core::kShipping) {
        if (requestValidation) {
            VK_LOG_INFO("INSTANCE validation forced OFF (shipping build)");
        }
        requestValidation = false;
    } else {
        // 环境变量：MYVK_VALIDATION=0 关闭 / =1 强制开启
        const char* env = std::getenv("MYVK_VALIDATION");
        if (env && std::strcmp(env, "0") == 0) requestValidation = false;
        if (env && std::strcmp(env, "1") == 0) requestValidation = true;
    }

    m_validation = requestValidation && hasLayer("VK_LAYER_KHRONOS_validation");
    create();
    if (m_validation) setupDebugMessenger();

    VK_LOG_INFO("Vulkan instance created (validation: %s)",
                m_validation ? "ON" : "OFF");
    // 单独一行打风味：**不动上面那行的原文** —— 十几条既有用例按
    // `VALIDATION|VUID| ERROR` 扫日志判红，改措辞的风险远大于收益。
    VK_LOG_INFO("INSTANCE build=%s git=%s validation=%s", core::kBuildFlavor,
                core::kGitDescribe, m_validation ? "on" : "off");
}

Instance::~Instance() {
    if (m_debugMessenger != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

bool Instance::hasLayer(const char* name) const {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    if (count > 0) vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

bool Instance::hasExtension(const char* name) const {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    if (count > 0) vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

bool Instance::create() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Venn";
    app.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
    app.pEngineName = "Venn";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    // 请求 1.2：屏幕空间效果链（HdrTarget 共享深度方案）依赖
    // separateDepthStencilLayouts —— 它的查询（vkGetPhysicalDeviceFeatures2）
    // 与启用（VkPhysicalDeviceVulkan12Features）都要求实例 ≥ 1.1。
    // 实例还停在 1.0 时 Features2 查询是无效调用（验证层报 ERROR、
    // 结果全 0），Device 侧会误判"设备不支持"。取 loader 支持版本与
    // 1.2 的较小者：老 loader 上退到它支持的版本，缺特性时 Device
    // 会明确报错而不是悄悄走错路。
    uint32_t instVer = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&instVer);  // Vulkan 1.1+ loader 提供
    const uint32_t kWantApi = VK_MAKE_VERSION(1, 2, 0);
    app.apiVersion = (instVer >= kWantApi) ? kWantApi : instVer;

    // GLFW 需要的实例扩展
    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions;
    for (uint32_t i = 0; i < glfwExtCount; ++i) extensions.push_back(glfwExts[i]);

    if (m_validation) {
        if (hasExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            VK_LOG_WARN("Debug utils extension unavailable, debug messenger disabled");
            m_validation = false;
        }
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();

    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    if (m_validation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = layers;
    }

    VK_CHECK(vkCreateInstance(&ci, nullptr, &m_instance));
    return true;
}

void Instance::setupDebugMessenger() {
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;

    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!create) {
        VK_LOG_WARN("vkCreateDebugUtilsMessengerEXT not found");
        return;
    }
    VK_CHECK(create(m_instance, &ci, nullptr, &m_debugMessenger));
}

} // namespace rhi
