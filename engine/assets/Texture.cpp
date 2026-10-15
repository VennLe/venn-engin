#include "assets/Texture.h"
#include "rhi/Device.h"
#include "rhi/CommandPool.h"
#include "rhi/Buffer.h"
#include "rhi/Image.h"
#include "rhi/DescriptorSet.h"

#include "core/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace assets {

namespace {

// ============================================================
// DDS 头（微软老式头，128 字节）——只解析 BC 块压缩格式
// ============================================================
#pragma pack(push, 1)
struct DdsPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rMask, gMask, bMask, aMask;
};
struct DdsHeader {
    uint32_t size;
    uint32_t flags;
    uint32_t height, width, pitch, depth, mipMapCount;
    uint32_t reserved[11];
    DdsPixelFormat ddspf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DdsHeaderDx10 {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
#pragma pack(pop)

constexpr uint32_t kDdsMagic = 0x20534444;      // 'DDS '
constexpr uint32_t kFourCcDx10 = 0x30315844;    // 'DX10'

// FourCC → (Vulkan format, 每块字节数)；srgb 由调用方决定 UNORM/SRGB 变体
struct BcFormatInfo {
    VkFormat unorm;
    VkFormat srgb;
    uint32_t bytesPerBlock;
    const char* name;
};

// 老式 FourCC
BcFormatInfo bcFromFourCC(uint32_t cc) {
    switch (cc) {
        case 0x31545844: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                                 VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, "BC1"};
        case 0x33545844: return {VK_FORMAT_BC2_UNORM_BLOCK,
                                 VK_FORMAT_BC2_SRGB_BLOCK, 16, "BC2"};
        case 0x35545844: return {VK_FORMAT_BC3_UNORM_BLOCK,
                                 VK_FORMAT_BC3_SRGB_BLOCK, 16, "BC3"};
        case 0x31495441:  // 'ATI1'
        case 0x55344342:  // 'BC4U'
            return {VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, 8,
                    "BC4"};
        case 0x53344342:  // 'BC4S'
            return {VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK, 8,
                    "BC4S"};
        case 0x32495441:  // 'ATI2'
        case 0x55354342:  // 'BC5U'
            return {VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, 16,
                    "BC5"};
        case 0x53354342:  // 'BC5S'
            return {VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK, 16,
                    "BC5S"};
        default: return {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 0, ""};
    }
}

// DX10 扩展的 DXGI_FORMAT（只收 UNORM / SRGB / 无符号浮点）
BcFormatInfo bcFromDxgi(uint32_t dxgi) {
    switch (dxgi) {
        case 71: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                         VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, "BC1"};
        case 74: return {VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK,
                         16, "BC2"};
        case 77: return {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
                         16, "BC3"};
        case 80: return {VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK,
                         8, "BC4"};
        case 83: return {VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
                         16, "BC5"};
        case 95: return {VK_FORMAT_BC6H_UFLOAT_BLOCK,
                         VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, "BC6H"};
        case 98: return {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
                         16, "BC7"};
        default: return {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 0, ""};
    }
}

// mip 链的逐层尺寸（width/height 各 max(1, /2)）
VkExtent2D mipExtent(uint32_t w, uint32_t h, uint32_t level) {
    return {std::max<uint32_t>(1, w >> level),
            std::max<uint32_t>(1, h >> level)};
}

uint32_t mipCountFor(uint32_t w, uint32_t h) {
    return static_cast<uint32_t>(
               std::floor(std::log2(static_cast<double>(
                   std::max(std::max(w, h), 1u))))) +
           1;
}

bool endsWithCI(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const char a = s[s.size() - n + i];
        const char b = suffix[i];
        if (std::tolower(static_cast<unsigned char>(a)) !=
            std::tolower(static_cast<unsigned char>(b)))
            return false;
    }
    return true;
}

} // namespace

Texture::~Texture() { destroy(); }

void Texture::destroy() {
    if (m_device == nullptr) return;
    VkDevice dev = m_device->get();
    // 描述符集随池销毁释放，无需手动 free
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, m_view, nullptr);
        m_view = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE) {
        vkDestroyImage(dev, m_image, nullptr);
        m_image = VK_NULL_HANDLE;
    }
    if (m_memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, m_memory, nullptr);
        m_memory = VK_NULL_HANDLE;
    }
    m_streamStaging.reset();
    m_device = nullptr;
}

// ============================================================
// 采样器（maxLod 由调用点决定：无 mip=1，全链=mipLevels-1）
// ============================================================
void Texture::createSampler(const TextureContext& ctx, float maxLod) {
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device->get(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    // MYVK_ANISO 覆盖设备上限（夹到 [0, max]）；<0 = 维持现状（设备上限）
    float aniso = ctx.maxAnisotropy;
    if (ctx.anisotropyOverride >= 0.0f)
        aniso = std::min(ctx.anisotropyOverride, ctx.maxAnisotropy);

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable = aniso > 1.0f ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = aniso;
    si.minLod = 0.0f;
    si.maxLod = maxLod;
    VK_CHECK(vkCreateSampler(m_device->get(), &si, nullptr, &m_sampler));
}

// ============================================================
// mip 层布局屏障（TRANSFER/SHADER 之间的常用组合）
// ============================================================
namespace {

void barrierMip(VkCommandBuffer cmd, VkImage image, uint32_t level,
                VkImageLayout oldL, VkImageLayout newL, VkAccessFlags srcAcc,
                VkAccessFlags dstAcc, VkPipelineStageFlags srcStage,
                VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    b.srcAccessMask = srcAcc;
    b.dstAccessMask = dstAcc;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr,
                         1, &b);
}

} // namespace

// ============================================================
// RGBA8 上传（mip 链 = blit 逐级半分辨率）
// ============================================================
void Texture::uploadAndFinish(const TextureContext& ctx, const uint8_t* pixels,
                              bool srgb) {
    m_device = ctx.device;
    VkDevice dev = m_device->get();
    // 颜色贴图用 SRGB（硬件采样时自动线性化）；
    // 数据贴图（normal/ORM）必须是 UNORM，否则会被错误地做 gamma 解码
    m_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    const uint32_t mips =
        ctx.generateMipmaps ? mipCountFor(m_width, m_height) : 1;
    m_mipLevels = mips;

    const VkDeviceSize imageSize =
        static_cast<VkDeviceSize>(m_width) * m_height * 4;

    // 1) staging 上传
    rhi::Buffer staging(*ctx.device, imageSize,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.writeData(pixels, imageSize);

    // 2) 创建图像（有链时加 TRANSFER_SRC —— blit 的源）
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = m_format;
    ci.extent = {m_width, m_height, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mips > 1) ci.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, nullptr, &m_image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, m_image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_device->findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &m_memory));
    VK_CHECK(vkBindImageMemory(dev, m_image, m_memory, 0));

    // 3) 单次提交：copy mip0 → blit 链 → 全部转 SHADER_READ
    VkCommandBuffer cmd = ctx.cmdPool->beginOneTime();

    // 全层 UNDEFINED → TRANSFER_DST（blit 的目的层必须已处于目标布局）
    VkImageMemoryBarrier b0{};
    b0.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b0.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b0.image = m_image;
    b0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    b0.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b0);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {m_width, m_height, 1};
    vkCmdCopyBufferToImage(cmd, staging.get(), m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (mips > 1) {
        // blit 链：每级从上一级线性降采样。源层先转 TRANSFER_SRC，
        // blit 完就地转 SHADER_READ（经典写法，每层状态明确）。
        for (uint32_t i = 1; i < mips; ++i) {
            barrierMip(cmd, m_image, i - 1,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);

            const VkExtent2D src = mipExtent(m_width, m_height, i - 1);
            const VkExtent2D dst = mipExtent(m_width, m_height, i);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(src.width),
                                  static_cast<int32_t>(src.height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            blit.dstOffsets[1] = {static_cast<int32_t>(dst.width),
                                  static_cast<int32_t>(dst.height), 1};
            vkCmdBlitImage(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, VK_FILTER_LINEAR);

            barrierMip(cmd, m_image, i - 1,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        // 最后一层（blit 的目的地）还停在 TRANSFER_DST
        barrierMip(cmd, m_image, mips - 1,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    } else {
        barrierMip(cmd, m_image, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    ctx.cmdPool->endAndSubmit(cmd);

    // 4) 视图（全链）
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = m_format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &m_view));

    // 5) 采样器：关 mip 时 maxLod=1 永远采 level0（基线不变）
    createSampler(ctx, mips > 1 ? static_cast<float>(mips) : 1.0f);

    // 6) 描述符集（set 1, binding 0）
    m_set = ctx.descriptors->allocate(ctx.textureSetLayout);
    ctx.descriptors->writeTexture(m_set, 0, m_sampler, m_view);

    // 验证脚本靠这行断言 mip 链真的生成了
    if (mips > 1)
        VK_LOG_INFO("Texture mip chain: %ux%u levels=%u", m_width, m_height,
                    mips);
}

// ============================================================
// DDS 加载入口（解析头 → uploadCompressed）
// ============================================================
void Texture::fromDDS(const TextureContext& ctx, const std::string& path,
                      bool srgb) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Failed to open DDS: " + path);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    if (bytes.size() < sizeof(DdsHeader) + 4 ||
        *reinterpret_cast<const uint32_t*>(bytes.data()) != kDdsMagic)
        throw std::runtime_error("Not a DDS file: " + path);

    const DdsHeader& h = *reinterpret_cast<const DdsHeader*>(bytes.data() + 4);
    if (h.size != 124)
        throw std::runtime_error("Bad DDS header size: " + path);
    m_width = h.width;
    m_height = h.height;

    BcFormatInfo fi{};
    uint32_t mipCount = 1;
    bool isBc6h = false;
    if (h.ddspf.fourCC == kFourCcDx10) {
        if (bytes.size() < 4 + sizeof(DdsHeader) + sizeof(DdsHeaderDx10))
            throw std::runtime_error("Truncated DDS(DX10): " + path);
        const DdsHeaderDx10& x = *reinterpret_cast<const DdsHeaderDx10*>(
            bytes.data() + 4 + sizeof(DdsHeader));
        fi = bcFromDxgi(x.dxgiFormat);
        isBc6h = (x.dxgiFormat == 95);
        if (x.arraySize > 1)
            throw std::runtime_error(
                "DDS texture arrays not supported: " + path);
        mipCount = h.mipMapCount ? h.mipMapCount : 1;
    } else {
        fi = bcFromFourCC(h.ddspf.fourCC);
        mipCount = (h.flags & 0x20000) && h.mipMapCount ? h.mipMapCount : 1;
    }
    if (fi.unorm == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("Unsupported DDS pixel format: " + path);

    // 尺寸合法性 + 设备格式支持（BC1-5/7 是桌面基本盘，仍显式查一次）
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(ctx.device->physical(), fi.unorm,
                                        &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        throw std::runtime_error(std::string("Device cannot sample ") +
                                 fi.name + ": " + path);

    // 逐层数据布局（tight packing：offset 顺次累加）
    std::vector<VkExtent2D> extents;
    VkDeviceSize total = 0;
    for (uint32_t i = 0; i < mipCount; ++i) {
        const VkExtent2D e = mipExtent(m_width, m_height, i);
        extents.push_back(e);
        total += static_cast<VkDeviceSize>(((e.width + 3) / 4) *
                                           ((e.height + 3) / 4)) *
                 fi.bytesPerBlock;
    }
    if (4 + sizeof(DdsHeader) + total > bytes.size())
        throw std::runtime_error("DDS data truncated: " + path);

    // sRGB 变体：DX10 的 SRGB 由文件头自决，老式 FourCC 由调用参数决定
    m_format = isBc6h ? fi.unorm : (srgb ? fi.srgb : fi.unorm);
    m_mipLevels = mipCount;

    uploadCompressed(ctx, reinterpret_cast<const uint8_t*>(
                              bytes.data() + 4 + sizeof(DdsHeader) +
                              (h.ddspf.fourCC == kFourCcDx10
                                   ? sizeof(DdsHeaderDx10)
                                   : 0)),
                     total, extents, srgb);

    VK_LOG_INFO("Texture(DDS) loaded: %s format=%s %ux%u levels=%u%s",
                path.c_str(), fi.name, m_width, m_height, mipCount,
                m_streaming ? " [streaming]" : "");

    TextureSource& src = m_source;
    src = TextureSource{};
    src.kind = TextureSource::Kind::File;
    src.path = path;
    src.srgb = srgb;
}

// ============================================================
// 压缩块上传（一次性全传 / 流式分帧）
// ============================================================
void Texture::uploadCompressed(const TextureContext& ctx, const uint8_t* data,
                               VkDeviceSize size,
                               const std::vector<VkExtent2D>& mipExtents,
                               bool srgb) {
    m_device = ctx.device;
    VkDevice dev = m_device->get();

    const uint32_t mips = static_cast<uint32_t>(mipExtents.size());

    rhi::Buffer staging(*ctx.device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.writeData(data, size);

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = m_format;
    ci.extent = {m_width, m_height, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, nullptr, &m_image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, m_image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_device->findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &m_memory));
    VK_CHECK(vkBindImageMemory(dev, m_image, m_memory, 0));

    // 每层拷贝区域（tight packing：bufferRowLength/ImageHeight = 0），
    // offset 顺次累加；块字节数按格式族查表
    uint32_t bpb = 16;
    switch (m_format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK: bpb = 8; break;
        default: break;
    }
    m_streamRegions.resize(mips);
    VkDeviceSize offset = 0;
    for (uint32_t i = 0; i < mips; ++i) {
        VkBufferImageCopy& r = m_streamRegions[i];
        r.bufferOffset = offset;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.mipLevel = i;
        r.imageSubresource.layerCount = 1;
        r.imageExtent = {mipExtents[i].width, mipExtents[i].height, 1};
        offset += static_cast<VkDeviceSize>(
                      ((mipExtents[i].width + 3) / 4) *
                      ((mipExtents[i].height + 3) / 4)) *
                  bpb;
    }

    const bool stream = ctx.streamMips && mips > 1;

    VkCommandBuffer cmd = ctx.cmdPool->beginOneTime();
    // 全层 UNDEFINED → TRANSFER_DST
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m_image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);

    if (stream) {
        // 流式：本帧只传 mip0（立即"低清可渲染"），其余层留给
        // streamTick 每帧补一层。sampler maxLod=1 挡住未传层。
        vkCmdCopyBufferToImage(cmd, staging.get(), m_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &m_streamRegions[0]);
        barrierMip(cmd, m_image, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        // 未传层也转 SHADER_READ：描述符视图覆盖全部层，验证层要求
        // draw 时整张图布局一致（内容未定义无妨 —— maxLod 挡住逻辑
        // 采样，硬件读不到）。补传时 streamTick 会先转回 TRANSFER_DST。
        VkImageMemoryBarrier b2 = b;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b2.subresourceRange.baseMipLevel = 1;
        b2.subresourceRange.levelCount = mips - 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b2);
    } else {
        vkCmdCopyBufferToImage(cmd, staging.get(), m_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips,
                               m_streamRegions.data());
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
    ctx.cmdPool->endAndSubmit(cmd);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = m_format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &m_view));

    createSampler(ctx, stream ? 1.0f : static_cast<float>(mips));

    m_set = ctx.descriptors->allocate(ctx.textureSetLayout);
    ctx.descriptors->writeTexture(m_set, 0, m_sampler, m_view);

    if (stream) {
        // 流式期间保留上下文：staging 转移到成员延长寿命（GPU 拷贝
        // 已由 endAndSubmit 的 queueWaitIdle 保证完成，转移纯为后续
        // 层的数据仍在），每帧从里面补传一层。
        m_streaming = true;
        m_streamNext = 1;
        m_streamTotal = mips;
        m_streamAniso = ctx.anisotropyOverride;
        m_streamAnisoMax = ctx.maxAnisotropy;   // 重建 sampler 用设备上限
        m_streamSrgb = srgb;
        m_streamCmdPool = ctx.cmdPool;
        m_streamDescriptors = ctx.descriptors;
        m_streamSetLayout = ctx.textureSetLayout;
        m_streamStaging = std::make_unique<rhi::Buffer>(
            *ctx.device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        m_streamStaging->writeData(data, size);
        VK_LOG_INFO("TEXSTREAM begin '%s' levels=%u (mip0 live, %u pending)",
                    m_source.path.c_str(), mips, mips - 1);
    }
}

// ============================================================
// 纹理流 tick：每帧补一层，全部到位后重建 sampler 放开 LOD
// ============================================================
bool Texture::streamTick() {
    if (!m_streaming) return false;

    const uint32_t level = m_streamNext;
    VkCommandBuffer cmd = m_streamCmdPool->beginOneTime();
    // 层当前在 SHADER_READ（见 uploadCompressed 流式分支）→ 先转回
    // TRANSFER_DST 供拷贝，拷完再转回 SHADER_READ
    barrierMip(cmd, m_image, level, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(cmd, m_streamStaging->get(), m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &m_streamRegions[level]);
    // 该层转 SHADER_READ（TRANSFER 写完 → 着色可读）
    barrierMip(cmd, m_image, level, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    m_streamCmdPool->endAndSubmit(cmd);

    VK_LOG_INFO("TEXSTREAM level %u/%u", level + 1, m_streamTotal);
    ++m_streamNext;
    if (m_streamNext < m_streamTotal) return true;   // 还有层没到位

    // 全部到位：重建 sampler 放开 LOD，刷新描述符，释放 staging
    const float fullLod = static_cast<float>(m_streamTotal);
    TextureContext ctx{};
    ctx.device = m_device;
    ctx.maxAnisotropy = m_streamAnisoMax;
    ctx.anisotropyOverride = m_streamAniso;
    createSampler(ctx, fullLod);
    m_streamDescriptors->writeTexture(m_set, 0, m_sampler, m_view);
    m_streamStaging.reset();
    m_streaming = false;
    VK_LOG_INFO("TEXSTREAM complete '%s' levels=%u",
                m_source.path.c_str(), m_streamTotal);
    return false;
}

// ============================================================
// 文件分派：.dds → 压缩路径，其余 → stb_image
// ============================================================
void Texture::fromFile(const TextureContext& ctx, const std::string& path,
                       bool srgb) {
    if (endsWithCI(path, ".dds")) {
        fromDDS(ctx, path, srgb);
        return;
    }
    int w = 0, h = 0, channels = 0;
    // 强制转 RGBA
    uint8_t* data = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!data) {
        VK_LOG_ERROR("Texture load failed: %s (%s)", path.c_str(),
                     stbi_failure_reason());
        throw std::runtime_error("Failed to load texture: " + path);
    }
    m_width = static_cast<uint32_t>(w);
    m_height = static_cast<uint32_t>(h);
    uploadAndFinish(ctx, data, srgb);
    stbi_image_free(data);
    VK_LOG_INFO("Texture loaded: %s (%dx%d)%s", path.c_str(), w, h,
                ctx.generateMipmaps ? " [mipmapped]" : "");

    // 记录来源，供场景序列化复现
    TextureSource& src = m_source;
    src = TextureSource{};
    src.kind = TextureSource::Kind::File;
    src.path = path;
    src.srgb = srgb;
}

void Texture::fromPixels(const TextureContext& ctx, const uint8_t* pixels,
                         uint32_t width, uint32_t height, bool srgb) {
    m_width = width;
    m_height = height;
    // 直接喂像素（glTF 内嵌贴图等）无法独立复现，来源置为 Unknown；
    // makeCheckerboard / makeSolid 会在调用后自行补上来源描述
    m_source = TextureSource{};
    uploadAndFinish(ctx, pixels, srgb);
}

void Texture::makeCheckerboard(const TextureContext& ctx, uint32_t width,
                               uint32_t height, uint32_t cell, uint8_t a[4],
                               uint8_t b[4], bool srgb) {
    std::vector<uint8_t> pixels(
        static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            bool even = ((x / cell) + (y / cell)) % 2 == 0;
            const uint8_t* c = even ? a : b;
            size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            pixels[idx + 0] = c[0];
            pixels[idx + 1] = c[1];
            pixels[idx + 2] = c[2];
            pixels[idx + 3] = c[3];
        }
    }
    fromPixels(ctx, pixels.data(), width, height, srgb);

    // 记录来源（注意要在 fromPixels 之后设，它不覆盖 m_source）
    m_source = TextureSource{};
    m_source.kind = TextureSource::Kind::Checker;
    for (int i = 0; i < 4; ++i) {
        m_source.colorA[i] = a[i];
        m_source.colorB[i] = b[i];
    }
    m_source.cell = cell;
    m_source.size = width;
    m_source.srgb = srgb;
}

void Texture::makeSolid(const TextureContext& ctx, uint8_t color[4],
                        uint32_t size, bool srgb) {
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    for (uint32_t i = 0; i < size * size; ++i) {
        pixels[i * 4 + 0] = color[0];
        pixels[i * 4 + 1] = color[1];
        pixels[i * 4 + 2] = color[2];
        pixels[i * 4 + 3] = color[3];
    }
    fromPixels(ctx, pixels.data(), size, size, srgb);

    m_source = TextureSource{};
    m_source.kind = TextureSource::Kind::Solid;
    for (int i = 0; i < 4; ++i) m_source.color[i] = color[i];
    m_source.size = size;
    m_source.srgb = srgb;
}

} // namespace assets
