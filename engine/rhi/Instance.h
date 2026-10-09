#pragma once
// ============================================================
// rhi/Instance —— VkInstance 封装
// 负责：实例创建、扩展/层协商、Debug Messenger
// 验证层默认开启（可用环境变量 MYVK_VALIDATION=0 关闭）
// ============================================================

#include "rhi/VulkanCommon.h"
#include <vector>

namespace rhi {

class Instance {
public:
    explicit Instance(bool requestValidation = true);
    ~Instance();

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    VkInstance get() const { return m_instance; }
    bool validationEnabled() const { return m_validation; }

private:
    bool create();
    void setupDebugMessenger();
    bool hasLayer(const char* name) const;
    bool hasExtension(const char* name) const;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    bool m_validation = false;
};

} // namespace rhi
