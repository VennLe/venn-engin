#pragma once
// ============================================================
// rhi/CommandPool —— 命令池封装
// 提供：主循环命令缓冲分配 + 一次性命令（上传/布局转换）
// ============================================================

#include "rhi/VulkanCommon.h"
#include <vector>

namespace rhi {

class Device;

class CommandPool {
public:
    explicit CommandPool(Device& device);
    ~CommandPool();

    CommandPool(const CommandPool&) = delete;
    CommandPool& operator=(const CommandPool&) = delete;

    VkCommandPool get() const { return m_pool; }
    Device& device() const { return m_device; }

    std::vector<VkCommandBuffer> allocate(uint32_t count);

    // 一次性命令：begin → 调用方录命令 → endAndSubmit
    VkCommandBuffer beginOneTime();
    void endAndSubmit(VkCommandBuffer cmd);

    void free(const std::vector<VkCommandBuffer>& cmds);

private:
    Device& m_device;
    VkCommandPool m_pool = VK_NULL_HANDLE;
};

} // namespace rhi
