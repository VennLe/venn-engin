#pragma once
// ============================================================
// rhi/Buffer —— VkBuffer + 内存 RAII 封装
// 支持宿主可见内存映射；提供设备本地上传（staging）工厂
// ============================================================

#include "rhi/VulkanCommon.h"
#include <memory>

namespace rhi {

class Device;
class CommandPool;

class Buffer {
public:
    Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags memoryProps);
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    VkBuffer get() const { return m_buffer; }
    VkDeviceSize size() const { return m_size; }

    void* map();       // 持久映射（HOST_VISIBLE）
    void unmap();

    // 一次性写入（HOST_VISIBLE 内存）
    void writeData(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

    // 设备本地缓冲工厂：staging 上传（用于顶点/索引缓冲）
    static std::unique_ptr<Buffer> createDeviceLocal(
        Device& device, CommandPool& cmdPool, const void* data, VkDeviceSize size,
        VkBufferUsageFlags usage);

private:
    Device& m_device;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkDeviceSize m_size = 0;
    void* m_mapped = nullptr;
};

} // namespace rhi
