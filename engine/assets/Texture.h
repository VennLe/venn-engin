#pragma once
// ============================================================
// assets/Texture —— 纹理（图片上传 GPU + 采样器 + 描述符集）
// 支持：stb_image 文件加载 / 程序化棋盘格与纯色
// 像素格式 R8G8B8A8_SRGB（采样自动线性化）
// ============================================================

#include "rhi/VulkanCommon.h"
#include <memory>
#include <string>

namespace rhi {
class Device;
class CommandPool;
class Buffer;
class DescriptorAllocator;
}

namespace assets {

// 创建 GPU 纹理所需的上下文（由 Renderer 提供，避免 assets 反向依赖 render）
struct TextureContext {
    rhi::Device* device = nullptr;
    rhi::CommandPool* cmdPool = nullptr;
    rhi::DescriptorAllocator* descriptors = nullptr;
    VkDescriptorSetLayout textureSetLayout = VK_NULL_HANDLE;
    float maxAnisotropy = 1.0f;
};

// ============================================================
// TextureSource —— 纹理的"来源描述"（供场景序列化重建）
//
// 只覆盖"可复现"的程序化 / 文件纹理：
//   Solid   —— 纯色（记 color + size）
//   Checker —— 棋盘格（记两色 + 格子边长 + 尺寸）
//   File    —— 图片文件（记路径 + srgb）
//
// glTF 内嵌贴图（fromPixels 直接喂像素）属于模型的一部分，
// 由模型重新加载得到，所以保持 Unknown，不单独序列化。
// ============================================================
struct TextureSource {
    enum class Kind { Unknown, Solid, Checker, File };

    Kind kind = Kind::Unknown;
    std::string name;                          // AssetManager 缓存键
    uint8_t color[4] = {255, 255, 255, 255};   // Solid
    uint8_t colorA[4] = {0, 0, 0, 255};        // Checker
    uint8_t colorB[4] = {255, 255, 255, 255};  // Checker
    uint32_t cell = 32;                        // Checker 格子边长（像素）
    uint32_t size = 512;                       // Checker / Solid 边长
    std::string path;                          // File
    bool srgb = true;                          // File
};

class Texture {
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // 从 RGBA8 像素数据创建（size = 4*w*h）
    // srgb=true  → R8G8B8A8_SRGB  （albedo / emissive 等"颜色"贴图）
    // srgb=false → R8G8B8A8_UNORM （normal / ORM 等"数据"贴图，不能做 gamma）
    void fromPixels(const TextureContext& ctx, const uint8_t* pixels,
                    uint32_t width, uint32_t height, bool srgb = true);

    // 从文件加载（stb_image：png/jpg/tga...）
    void fromFile(const TextureContext& ctx, const std::string& path,
                  bool srgb = true);

    // 程序化棋盘格（两种颜色，格子大小 cell）
    void makeCheckerboard(const TextureContext& ctx, uint32_t width,
                          uint32_t height, uint32_t cell, uint8_t a[4],
                          uint8_t b[4]);

    // 程序化纯色
    void makeSolid(const TextureContext& ctx, uint8_t color[4],
                   uint32_t size = 4);

    VkImageView view() const { return m_view; }
    VkSampler sampler() const { return m_sampler; }
    VkDescriptorSet descriptorSet() const { return m_set; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }

    // ---- 来源溯源（序列化用）----
    const TextureSource& source() const { return m_source; }
    void setSource(const TextureSource& s) { m_source = s; }

private:
    void uploadAndFinish(const TextureContext& ctx, const uint8_t* pixels,
                         bool srgb);
    void destroy();

    rhi::Device* m_device = nullptr;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    VkFormat m_format = VK_FORMAT_R8G8B8A8_SRGB;
    TextureSource m_source;
};

} // namespace assets
