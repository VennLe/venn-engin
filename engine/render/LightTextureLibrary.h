#pragma once
// ============================================================
// render/LightTextureLibrary —— 光源修饰纹理库（R3-3）
//
// 把 light function（投影贴图 / cookie）与 IES 光域网剖面烘进**同一张**
// sampler2DArray。两种用途靠"层号"区分，互不干扰：
//
//   * light function：把片元方向投影到灯锥的截面 → [0,1]² 的 uv，
//     直接 texture(lightTex, vec3(uv, layer))。
//   * IES 剖面（轴对齐柱面展开）：u = 水平角 φ ∈ [0,2π)，
//     v = 竖直角 θ ∈ [0,π]；texture(lightTex, vec3(φ/2π, θ/π, layer))。
//
// 第 0 层**恒为全白**：任何"找不到贴图"的灯都退化成乘 1（基线不变）。
//
// 内建图案是程序化生成的（不需要任何资产文件）：
//   1 white / 2 grid / 3 rings / 4 bars（cookie）
//   5 ies-narrow / 6 ies-wide / 7 ies-batwing（IES）
// 之后 8..kLayers-1 留给运行时从磁盘加载的 .ies 文件。
//
// 全部图层一次性分配、只读使用（SHADER_READ_ONLY），唯一的上传发生在
// create() / loadIesFile()。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rhi {
class Device;
class CommandPool;
}

namespace render {

class LightTextureLibrary {
public:
    // 单层边长（像素）。256 足够表达 cookie 的高频网格与 IES 的角向细节。
    static constexpr uint32_t kSize = 256;
    // 数组总层数（固定分配，不支持增长 —— 超出时 loadIesFile 返回 -1）。
    static constexpr uint32_t kLayers = 16;

    LightTextureLibrary(rhi::Device& device, rhi::CommandPool& cmdPool);
    ~LightTextureLibrary();

    LightTextureLibrary(const LightTextureLibrary&) = delete;
    LightTextureLibrary& operator=(const LightTextureLibrary&) = delete;

    // 建数组图 + 生成内建图案。可重复调用（幂等）。
    void create();

    // 从磁盘加载一个 .ies（IESNA LM-63）文件并追加为新层。
    // 成功返回层号（>= 8），失败返回 -1。name 用于 layerOf 查询。
    int loadIesFile(const std::string& path, const std::string& name);

    // 名字 → 层号；找不到返回 -1。内建名（"cookie:grid" / "ies:narrow" …）
    // 与已加载文件的名字都在这张表里。
    int layerOf(const std::string& name) const;

    // 已注册的名字（编辑器下拉用）。
    const std::vector<std::string>& names() const { return m_names; }
    // 内建层数（前 kBuiltinLayers 层）——UI 只列内建 + 已加载文件。
    uint32_t builtinLayers() const { return m_builtinLayers; }

    bool valid() const { return m_view != VK_NULL_HANDLE; }
    VkImageView view() const { return m_view; }
    VkSampler sampler() const { return m_sampler; }
    uint32_t layerCount() const { return m_usedLayers; }

private:
    // 把一张 4 通道像素图写进第 layer 层的 CPU 侧暂存区（延迟到 flush 上传）。
    void stageLayer(uint32_t layer, const std::vector<uint8_t>& rgba);
    // 建图 + 上传全部已暂存图层。
    void upload();
    void destroyImage();

    rhi::Device& m_device;
    rhi::CommandPool& m_cmdPool;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    uint32_t m_usedLayers = 0;    // 已占用的层数（= 下一个可用层号）
    uint32_t m_builtinLayers = 0; // 内建层数（= 第一个可加载层号）
    std::vector<uint8_t> m_pixels;   // kLayers * kSize * kSize * 4
    std::vector<std::string> m_names;  // 与层号一一对应（不足补空串）
};

} // namespace render
