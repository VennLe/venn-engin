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

// ---------------------------------------------------------------- 包内直读钩子
//
// `rhi` 是**最底层**，不能 include `assets/`（依赖方向反了）。但"从包里读
// 字节"这件事必须在 readFile 里做掉，否则那五十来处
// `readFile(resolveShaderPath(x))` 全都要改。所以这里放一个**函数指针**：
// `assets` 层挂载 pak 时把它填上，rhi 只管调。
//
// 为什么不用虚基类/接口：只有一个函数、没有状态、没有生命周期管理。
// 一个 `inline` 函数指针变量（C++17）就够了，也不需要新增一个 TU。
using PakReadFn = bool (*)(const std::string& entry, std::string& out);
inline PakReadFn g_pakRead = nullptr;

// 虚拟路径前缀。`resolveShaderPath` 命中包时返回 `"pak:shaders/x.spv"`，
// 于是**不落盘**（G29-d）—— 这是 pak 唯一还剩下的成本。
inline bool isPakPath(const std::string& p) {
    return p.size() > 4 && p.compare(0, 4, "pak:") == 0;
}

// 读取二进制文件（用于加载 SPIR-V 着色器）
inline std::vector<char> readFile(const std::string& path) {
    // `pak:` 虚拟路径：直接从包内存读，不碰文件系统
    if (isPakPath(path)) {
        std::string bytes;
        if (g_pakRead && g_pakRead(path.substr(4), bytes)) {
            return std::vector<char>(bytes.begin(), bytes.end());
        }
        // 走到这里说明调用方拿到了一条包路径，但包里没有 → 必须报错，
        // 不能返回空 vector（空 SPIR-V 会让管线创建失败，而报错指向
        // "创建管线"，离"包不对"很远）
        throw std::runtime_error("pak entry not readable: " + path);
    }
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
