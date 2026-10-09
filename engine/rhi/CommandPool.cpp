#include "rhi/CommandPool.h"
#include "rhi/Device.h"

namespace rhi {

CommandPool::CommandPool(Device& device) : m_device(device) {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = m_device.graphicsFamily();
    VK_CHECK(vkCreateCommandPool(m_device.get(), &ci, nullptr, &m_pool));
}

CommandPool::~CommandPool() {
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device.get(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
    }
}

std::vector<VkCommandBuffer> CommandPool::allocate(uint32_t count) {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = count;
    std::vector<VkCommandBuffer> bufs(count);
    VK_CHECK(vkAllocateCommandBuffers(m_device.get(), &ai, bufs.data()));
    return bufs;
}

VkCommandBuffer CommandPool::beginOneTime() {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(m_device.get(), &ai, &cmd));

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

void CommandPool::endAndSubmit(VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    VK_CHECK(vkQueueSubmit(m_device.graphicsQueue(), 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(m_device.graphicsQueue()));
    vkFreeCommandBuffers(m_device.get(), m_pool, 1, &cmd);
}

void CommandPool::free(const std::vector<VkCommandBuffer>& cmds) {
    if (cmds.empty()) return;
    vkFreeCommandBuffers(m_device.get(), m_pool,
                         static_cast<uint32_t>(cmds.size()), cmds.data());
}

} // namespace rhi
