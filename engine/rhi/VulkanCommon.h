#pragma once
// ============================================================
// VulkanCommon.h
// 引擎内部共享的公共头：Vulkan / GLM / STL + 错误检查宏
// 所有 rhi/render 层代码都包含本文件
// ============================================================

#include <vulkan/vulkan.h>

#define GLM_FORCE_RADIANS
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace vkutil {

// 读取二进制文件（用于加载 SPIR-V 着色器）
inline std::vector<char> readFile(const std::string& path) {
    FILE* f = nullptr;
#if defined(_MSC_VER)
    fopen_s(&f, path.c_str(), "rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        throw std::runtime_error("Failed to open file: " + path);
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> data(static_cast<size_t>(size));
    if (size > 0) {
        size_t rd = std::fread(data.data(), 1, static_cast<size_t>(size), f);
        (void)rd;
    }
    std::fclose(f);
    return data;
}

} // namespace vkutil

// Vulkan 调用结果检查：失败时打日志并抛异常
#define VK_CHECK(call)                                                        \
    do {                                                                      \
        VkResult _vkres = (call);                                             \
        if (_vkres != VK_SUCCESS) {                                           \
            std::fprintf(stderr, "[VK_CHECK] Vulkan error %d at %s:%d\n",     \
                         static_cast<int>(_vkres), __FILE__, __LINE__);        \
            throw std::runtime_error("Vulkan call failed");                   \
        }                                                                     \
    } while (0)
