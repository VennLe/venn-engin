#include "rhi/Buffer.h"
#include "rhi/Device.h"
#include "rhi/CommandPool.h"

namespace rhi {

Buffer::Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags memoryProps)
    : m_device(device), m_size(size) {
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(m_device.get(), &ci, nullptr, &m_buffer));

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device.get(), m_buffer, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_device.findMemoryType(req.memoryTypeBits, memoryProps);
    VK_CHECK(vkAllocateMemory(m_device.get(), &ai, nullptr, &m_memory));
    VK_CHECK(vkBindBufferMemory(m_device.get(), m_buffer, m_memory, 0));
}

Buffer::~Buffer() {
    unmap();
    if (m_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device.get(), m_buffer, nullptr);
        m_buffer = VK_NULL_HANDLE;
    }
    if (m_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device.get(), m_memory, nullptr);
        m_memory = VK_NULL_HANDLE;
    }
}

void* Buffer::map() {
    if (m_mapped) return m_mapped;
    void* ptr = nullptr;
    VK_CHECK(vkMapMemory(m_device.get(), m_memory, 0, m_size, 0, &ptr));
    m_mapped = ptr;
    return m_mapped;
}

void Buffer::unmap() {
    if (m_mapped) {
        vkUnmapMemory(m_device.get(), m_memory);
        m_mapped = nullptr;
    }
}

void Buffer::writeData(const void* data, VkDeviceSize size, VkDeviceSize offset) {
    if (offset + size > m_size) {
        throw std::runtime_error("Buffer::writeData out of range");
    }
    void* ptr = map();
    std::memcpy(static_cast<uint8_t*>(ptr) + offset, data,
                static_cast<size_t>(size));
}

std::unique_ptr<Buffer> Buffer::createDeviceLocal(
    Device& device, CommandPool& cmdPool, const void* data, VkDeviceSize size,
    VkBufferUsageFlags usage) {
    // staging → GPU 拷贝
    Buffer staging(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.writeData(data, size);

    auto result = std::make_unique<Buffer>(
        device, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkCommandBuffer cmd = cmdPool.beginOneTime();
    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(cmd, staging.get(), result->get(), 1, &region);
    cmdPool.endAndSubmit(cmd);

    return result;
}

} // namespace rhi
