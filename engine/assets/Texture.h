#pragma once
// ============================================================
// assets/Texture —— 纹理（图片上传 GPU + 采样器 + 描述符集）
// 支持：stb_image 文件加载 / DDS(BC1/BC2/BC3/BC4/BC5/BC7) 压缩纹理 /
//       程序化棋盘格与纯色
// 像素格式 R8G8B8A8_SRGB / UNORM；压缩路径按 DDS 头选 BC*_BLOCK
//
// mipmap：MYVK_MIPMAP=1 时 RGBA8 路径用 vkCmdBlitImage 链在加载时
//         生成完整 mip 链（DDS 自带链直接逐层上传）；默认关——
//         mipLevels=1、maxLod=1，与旧基线逐像素一致。
// 纹理流：MYVK_TEXSTREAM=1 且来源是 DDS 时，先只传 mip0 立即可渲染，
//         其余 mip 层由 AssetManager::tickTextureStreaming() 每帧补
//         一层；全部到位后重建 sampler 放开 LOD。默认关。
// ============================================================

#include "rhi/VulkanCommon.h"
#include "rhi/Buffer.h"   // unique_ptr<Buffer> 成员的完整类型
#include <memory>
#include <string>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
class DescriptorAllocator;
}

namespace assets {

// 创建 GPU 纹理所需的上下文（由 Renderer 提供，避免 assets 反向依赖 render）
struct TextureContext {
    rhi::Device* device = nullptr;
    rhi::CommandPool* cmdPool = nullptr;
    rhi::DescriptorAllocator* descriptors = nullptr;
    VkDescriptorSetLayout textureSetLayout = VK_NULL_HANDLE;
    float maxAnisotropy = 1.0f;      // 设备上限（Device 查询，≤8）
    float anisotropyOverride = -1.f; // MYVK_ANISO 覆盖；<0 = 用设备上限
    bool generateMipmaps = false;    // MYVK_MIPMAP=1：RGBA8 路径生成 mip 链
    bool streamMips = false;         // MYVK_TEXSTREAM=1：DDS 分帧补传
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
    bool srgb = true;                          // File / Solid / Checker：色彩空间
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

    // 从文件加载：.dds 走压缩路径，其余（png/jpg/tga...）走 stb_image
    void fromFile(const TextureContext& ctx, const std::string& path,
                  bool srgb = true);

    // 从 DDS 加载（BC1/BC2/BC3/BC4/BC5/BC7，可带 mip 链）。
    // 数据按块压缩，直接上传不解码 —— 解压由采样硬件免费完成。
    void fromDDS(const TextureContext& ctx, const std::string& path,
                 bool srgb = true);

    // 程序化棋盘格（两种颜色，格子大小 cell）
    // srgb 语义与 makeSolid 一致：数据贴图（法线 / 粗糙度 / **高度图**）
    // 必须传 false，否则硬件采样时会做 sRGB→线性解码，灰度被整体压暗
    // 且非线性（0.5 → 0.216）—— 对高度场来说就是"起伏形状被扭过"。
    void makeCheckerboard(const TextureContext& ctx, uint32_t width,
                          uint32_t height, uint32_t cell, uint8_t a[4],
                          uint8_t b[4], bool srgb = true);

    // 程序化纯色
    // srgb=true  → R8G8B8A8_SRGB  （albedo / emissive 等"颜色"贴图）
    // srgb=false → R8G8B8A8_UNORM （normal / ORM 等"数据"贴图，不能做 gamma）
    void makeSolid(const TextureContext& ctx, uint8_t color[4],
                   uint32_t size = 4, bool srgb = true);

    // ---- 纹理流（lite）：每帧补传一个待传 mip 层 ----
    // 返回 true = 仍在流式中（还有层没传完）。
    // 内部用一次性 command buffer 提交 transfer + per-layer barrier；
    // 同 queue 的 vkQueueSubmit 隐式保证前序提交的内存可见性，所以
    // 本帧补传的层在本帧稍后的渲染里可安全采样（sampler maxLod 挡住
    // 未完成层，全部到位后重建 sampler 放开）。
    bool streamTick();

    bool isStreaming() const { return m_streaming; }
    uint32_t mipLevels() const { return m_mipLevels; }

    VkImageView view() const { return m_view; }
    VkSampler sampler() const { return m_sampler; }
    VkDescriptorSet descriptorSet() const { return m_set; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    VkFormat format() const { return m_format; }

    // ---- 来源溯源（序列化用）----
    const TextureSource& source() const { return m_source; }
    void setSource(const TextureSource& s) { m_source = s; }

private:
    // RGBA8 通用上传（generateMipmaps 决定是否建链）
    void uploadAndFinish(const TextureContext& ctx, const uint8_t* pixels,
                         bool srgb);
    // 压缩数据上传（DDS 解出的块数据 + mip 布局）
    void uploadCompressed(const TextureContext& ctx, const uint8_t* data,
                          VkDeviceSize size,
                          const std::vector<VkExtent2D>& mipExtents,
                          bool srgb);

    void createSampler(const TextureContext& ctx, float maxLod);
    void destroy();

    rhi::Device* m_device = nullptr;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_mipLevels = 1;
    VkFormat m_format = VK_FORMAT_R8G8B8A8_SRGB;
    TextureSource m_source;

    // ---- 纹理流状态（uploadCompressed 的 streamMips 分支填充）----
    bool m_streaming = false;
    uint32_t m_streamNext = 1;            // 下一帧要补的 mip 层（0 已传）
    uint32_t m_streamTotal = 1;
    float m_streamAniso = 1.0f;           // MYVK_ANISO 覆盖值（重建用）
    float m_streamAnisoMax = 1.0f;        // 设备上限（重建用）
    bool m_streamSrgb = true;
    rhi::CommandPool* m_streamCmdPool = nullptr;    // streamTick 提交用
    rhi::DescriptorAllocator* m_streamDescriptors = nullptr;
    VkDescriptorSetLayout m_streamSetLayout = VK_NULL_HANDLE;
    std::unique_ptr<rhi::Buffer> m_streamStaging;   // 保留到流完
    std::vector<VkBufferImageCopy> m_streamRegions; // 每层的拷贝区域
};

} // namespace assets
