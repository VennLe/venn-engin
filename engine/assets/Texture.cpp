#include "assets/Texture.h"
#include "rhi/Device.h"
#include "rhi/CommandPool.h"
#include "rhi/Buffer.h"
#include "rhi/Image.h"
#include "rhi/DescriptorSet.h"

#include "core/Logger.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace assets {

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
    m_device = nullptr;
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

void Texture::fromFile(const TextureContext& ctx, const std::string& path,
                       bool srgb) {
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
    VK_LOG_INFO("Texture loaded: %s (%dx%d)", path.c_str(), w, h);

    // 记录来源，供场景序列化复现
    TextureSource& src = m_source;
    src = TextureSource{};
    src.kind = TextureSource::Kind::File;
    src.path = path;
    src.srgb = srgb;
}

void Texture::makeCheckerboard(const TextureContext& ctx, uint32_t width,
                               uint32_t height, uint32_t cell, uint8_t a[4],
                               uint8_t b[4]) {
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
    fromPixels(ctx, pixels.data(), width, height);

    // 记录来源（注意要在 fromPixels 之后设，它不覆盖 m_source）
    m_source = TextureSource{};
    m_source.kind = TextureSource::Kind::Checker;
    for (int i = 0; i < 4; ++i) {
        m_source.colorA[i] = a[i];
        m_source.colorB[i] = b[i];
    }
    m_source.cell = cell;
    m_source.size = width;
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

void Texture::uploadAndFinish(const TextureContext& ctx, const uint8_t* pixels,
                              bool srgb) {
    m_device = ctx.device;
    VkDevice dev = m_device->get();
    // 颜色贴图用 SRGB（硬件采样时自动线性化）；
    // 数据贴图（normal/ORM）必须是 UNORM，否则会被错误地做 gamma 解码
    m_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkDeviceSize imageSize =
        static_cast<VkDeviceSize>(m_width) * m_height * 4;

    // 1) staging 上传
    rhi::Buffer staging(*ctx.device, imageSize,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.writeData(pixels, imageSize);

    // 2) 创建图像
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = m_format;
    ci.extent = {m_width, m_height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
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

    // 3) 拷贝 + 布局转换
    rhi::Image::transitionLayout(*ctx.device, *ctx.cmdPool, m_image,
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rhi::Image::copyBufferToImage(*ctx.device, *ctx.cmdPool, staging.get(),
                                  m_image, m_width, m_height);
    rhi::Image::transitionLayout(*ctx.device, *ctx.cmdPool, m_image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 4) 视图
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = m_format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &m_view));

    // 5) 采样器
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable =
        ctx.maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = ctx.maxAnisotropy;
    si.maxLod = 1.0f;
    VK_CHECK(vkCreateSampler(dev, &si, nullptr, &m_sampler));

    // 6) 描述符集（set 1, binding 0）
    m_set = ctx.descriptors->allocate(ctx.textureSetLayout);
    ctx.descriptors->writeTexture(m_set, 0, m_sampler, m_view);
}

} // namespace assets
