#include "render/LightTextureLibrary.h"

#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/Device.h"

#include "core/Logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace render {

namespace {

constexpr float kPi = 3.14159265358979323846f;

constexpr VkFormat kFmt = VK_FORMAT_R8G8B8A8_UNORM;

inline uint8_t to8(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint8_t>(c * 255.0f + 0.5f);
}

// ---- 内建 cookie 图案：返回 [0,1] 的单通道值（uv ∈ [0,1]²）----
float cookieGrid(float u, float v) {
    // 8×8 的亮格 + 细暗格线（像窗框 / 铁丝网的投影）
    const float cell = 8.0f;
    const float fx = u * cell - std::floor(u * cell);
    const float fy = v * cell - std::floor(v * cell);
    const float gx = std::min(fx, 1.0f - fx);
    const float gy = std::min(fy, 1.0f - fy);
    const float line = std::min(gx, gy);
    // 线宽 0.10：线内压暗到 0.25
    return line < 0.10f ? 0.25f : 1.0f;
}

float cookieRings(float u, float v) {
    const float dx = u - 0.5f, dy = v - 0.5f;
    const float r = std::sqrt(dx * dx + dy * dy) * 2.0f;
    return 0.5f + 0.5f * std::cos(r * 22.0f);
}

float cookieBars(float u, float v) {
    (void)v;
    return 0.5f + 0.5f * std::cos(u * 44.0f);
}

// ---- 内建 IES 剖面：cd(θ)，θ = 与灯轴夹角（弧度）----
// 归一化由 stageIes 统一做，这里只管形状。
float iesNarrow(float theta) {
    const float c = std::max(std::cos(theta), 0.0f);
    return std::pow(c, 80.0f);  // 极窄的笔形光束
}
float iesWide(float theta) {
    const float c = std::max(std::cos(theta), 0.0f);
    return std::pow(c, 8.0f);
}
float iesBatwing(float theta) {
    // 峰值偏离轴线 ~45°，轴心反而暗 —— "蝙蝠翼"配光
    const float s = std::sin(2.0f * theta);
    return std::max(s, 0.0f) * std::max(s, 0.0f);
}

// 把一组 (nVert × nHoriz) 的 candela 网格重采样成 kSize×kSize 的图：
//   u（列）= 水平角 φ ∈ [0,2π)，v（行）= 竖直角 θ ∈ [0,π]
// candela 归一化到 [0,1] 后按行主序存进 rgba（r=g=b=值，a=255）。
//
// 约定：IES 的竖直角数组 vert[i] 直接当作"θ（度）"用（标准 LM-63
// 的 C 平面就是 0..180°）；水平角数组 horiz[j] 当作 φ（度）。
void bakeIes(const std::vector<float>& vert, const std::vector<float>& horiz,
             const std::vector<float>& candela, std::vector<uint8_t>& out) {
    out.assign(LightTextureLibrary::kSize * LightTextureLibrary::kSize * 4, 255);
    const uint32_t S = LightTextureLibrary::kSize;
    if (vert.empty() || horiz.empty() || candela.size() < vert.size() * horiz.size())
        return;

    const uint32_t nv = static_cast<uint32_t>(vert.size());
    const uint32_t nh = static_cast<uint32_t>(horiz.size());

    float vmin = vert.front(), vmax = vert.back();
    if (vmin > vmax) std::swap(vmin, vmax);
    float hmin = horiz.front(), hmax = horiz.back();
    if (hmin > hmax) std::swap(hmin, hmax);

    // 找最大值用于归一化
    float cmax = 1e-6f;
    for (float c : candela) cmax = std::max(cmax, c);

    auto lookup = [&](float thDeg, float phDeg) -> float {
        // 在 (vert, horiz) 网格里做双线性插值
        float t = (thDeg - vmin) / std::max(vmax - vmin, 1e-4f);
        float p = (phDeg - hmin) / std::max(hmax - hmin, 1e-4f);
        t = std::min(std::max(t, 0.0f), 1.0f);
        p = std::min(std::max(p, 0.0f), 1.0f);
        const float tv = t * static_cast<float>(nv - 1);
        const float pv = p * static_cast<float>(nh - 1);
        const uint32_t i0 = static_cast<uint32_t>(std::floor(tv));
        const uint32_t j0 = static_cast<uint32_t>(std::floor(pv));
        const uint32_t i1 = std::min(i0 + 1, nv - 1);
        const uint32_t j1 = std::min(j0 + 1, nh - 1);
        const float ft = tv - static_cast<float>(i0);
        const float fp = pv - static_cast<float>(j0);
        auto at = [&](uint32_t j, uint32_t i) {
            return candela[j * nv + i];
        };
        const float a = at(j0, i0) * (1 - ft) + at(j0, i1) * ft;
        const float b = at(j1, i0) * (1 - ft) + at(j1, i1) * ft;
        return a * (1 - fp) + b * fp;
    };

    for (uint32_t y = 0; y < S; ++y) {
        const float th = (static_cast<float>(y) + 0.5f) / static_cast<float>(S) *
                         180.0f;  // 竖直角 θ 度数（0 = 轴心）
        for (uint32_t x = 0; x < S; ++x) {
            const float ph = (static_cast<float>(x) + 0.5f) /
                             static_cast<float>(S) * 360.0f;
            const float c = std::min(lookup(th, ph) / cmax, 1.0f);
            const uint8_t b8 = to8(c);
            const size_t o = (static_cast<size_t>(y) * S + x) * 4;
            out[o + 0] = b8;
            out[o + 1] = b8;
            out[o + 2] = b8;
            out[o + 3] = 255;
        }
    }
}

} // namespace

LightTextureLibrary::LightTextureLibrary(rhi::Device& device,
                                         rhi::CommandPool& cmdPool)
    : m_device(device), m_cmdPool(cmdPool) {
    m_pixels.assign(static_cast<size_t>(kLayers) * kSize * kSize * 4, 255);
}

LightTextureLibrary::~LightTextureLibrary() {
    m_device.waitIdle();
    destroyImage();
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device.get(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
}

void LightTextureLibrary::stageLayer(uint32_t layer,
                                     const std::vector<uint8_t>& rgba) {
    if (layer >= kLayers) return;
    const size_t layerBytes = static_cast<size_t>(kSize) * kSize * 4;
    if (rgba.size() != layerBytes) return;
    std::memcpy(m_pixels.data() + layer * layerBytes, rgba.data(), layerBytes);
    if (layer + 1 > m_usedLayers) m_usedLayers = layer + 1;
    if (m_names.size() < m_usedLayers) m_names.resize(m_usedLayers);
}

void LightTextureLibrary::create() {
    if (m_view != VK_NULL_HANDLE) return;  // 幂等

    // ---- 内建图层 ----
    const size_t layerBytes = static_cast<size_t>(kSize) * kSize * 4;
    std::vector<uint8_t> buf(layerBytes, 255);

    // 0: 全白（恒等）
    for (size_t i = 0; i < buf.size(); i += 4) {
        buf[i + 0] = 255; buf[i + 1] = 255; buf[i + 2] = 255; buf[i + 3] = 255;
    }
    stageLayer(0, buf);
    m_names[0] = "white";

    // 1..3: cookie
    auto bakeCookie = [&](uint32_t layer, float (*fn)(float, float),
                          const char* name) {
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                const float u = (x + 0.5f) / static_cast<float>(kSize);
                const float v = (y + 0.5f) / static_cast<float>(kSize);
                const uint8_t c = to8(fn(u, v));
                const size_t o = (static_cast<size_t>(y) * kSize + x) * 4;
                buf[o + 0] = c; buf[o + 1] = c; buf[o + 2] = c; buf[o + 3] = 255;
            }
        }
        stageLayer(layer, buf);
        m_names[layer] = name;
    };
    bakeCookie(1, cookieGrid, "cookie:grid");
    bakeCookie(2, cookieRings, "cookie:rings");
    bakeCookie(3, cookieBars, "cookie:bars");

    // 4..6: 内建 IES（轴对齐：单列竖直角样本，水平角复制）
    auto bakeBuiltinIes = [&](uint32_t layer, float (*fn)(float),
                              const char* name) {
        std::vector<float> vert, cand;
        for (int i = 0; i <= 180; ++i) {
            const float th = static_cast<float>(i) * kPi / 180.0f;
            vert.push_back(static_cast<float>(i));
            cand.push_back(fn(th));
        }
        std::vector<float> horiz{0.0f};
        // bakeIes 需要 (nVert × nHoriz) 行主序；nh=1 → 直接就是 vert 顺序
        bakeIes(vert, horiz, cand, buf);
        stageLayer(layer, buf);
        m_names[layer] = name;
    };
    bakeBuiltinIes(4, iesNarrow, "ies:narrow");
    bakeBuiltinIes(5, iesWide, "ies:wide");
    bakeBuiltinIes(6, iesBatwing, "ies:batwing");

    m_builtinLayers = m_usedLayers;

    // 采样器：线性 + clamp（cookie 锥外/IES 边缘都靠 clamp 兜底）
    if (m_sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_sampler));
    }

    upload();

    VK_LOG_INFO("LightTextureLibrary: %u layers (%ux%u), %u builtin",
                m_usedLayers, kSize, kSize, m_builtinLayers);
}

void LightTextureLibrary::upload() {
    VkDevice dev = m_device.get();

    // ---- 建 array 图 ----
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kFmt;
    ci.extent = {kSize, kSize, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = kLayers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, nullptr, &m_image));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, m_image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = m_device.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, nullptr, &m_memory));
    VK_CHECK(vkBindImageMemory(dev, m_image, m_memory, 0));

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = m_image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = kFmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &m_view));

    // ---- staging：一次把所有层拷进去 ----
    const VkDeviceSize total = static_cast<VkDeviceSize>(m_pixels.size());
    rhi::Buffer staging(m_device, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.writeData(m_pixels.data(), total);

    VkCommandBuffer cmd = m_cmdPool.beginOneTime();
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m_image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kLayers};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kSize, kSize, 1};
    vkCmdCopyBufferToImage(cmd, staging.get(), m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    m_cmdPool.endAndSubmit(cmd);
}

void LightTextureLibrary::destroyImage() {
    VkDevice dev = m_device.get();
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
}

int LightTextureLibrary::layerOf(const std::string& name) const {
    if (name.empty()) return -1;
    for (size_t i = 0; i < m_names.size(); ++i) {
        if (m_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

int LightTextureLibrary::loadIesFile(const std::string& path,
                                     const std::string& name) {
    if (m_usedLayers >= kLayers) {
        VK_LOG_WARN("LightTextureLibrary: no free layer for IES '%s'", name.c_str());
        return -1;
    }
    std::ifstream f(path);
    if (!f) {
        VK_LOG_WARN("LightTextureLibrary: cannot open IES '%s'", path.c_str());
        return -1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    // 逐行找"数据头行"：第一行首字符是数字 / 正负号 / 小数点。
    // 关键字行（[TEST] 等）与 TILT 行都被跳过。
    std::istringstream lines(text);
    std::string line;
    size_t headerPos = std::string::npos;
    bool found = false;
    std::streamoff headerStart = 0;
    while (std::getline(lines, line)) {
        size_t p = line.find_first_not_of(" \t\r\n");
        if (p == std::string::npos) continue;
        const char ch = line[p];
        if (ch == '[' || ch == 'T' || ch == 't') continue;  // 关键字 / TILT
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' ||
            ch == '+' || ch == '.') {
            headerPos = p;
            headerStart = static_cast<std::streamoff>(lines.tellg());
            found = true;
            break;
        }
    }
    if (!found) {
        VK_LOG_WARN("LightTextureLibrary: IES '%s' has no numeric header",
                    path.c_str());
        return -1;
    }

    // 头行 10 个数字
    std::istringstream hs(line.substr(headerPos));
    double numLamps = 1, lumens = 0, mult = 1, nv = 0, nh = 0, ptype = 1;
    hs >> numLamps >> lumens >> mult >> nv >> nh >> ptype;
    const int nVert = static_cast<int>(nv + 0.5);
    const int nHoriz = static_cast<int>(nh + 0.5);
    if (nVert <= 0 || nHoriz <= 0 || nVert > 4096 || nHoriz > 4096) {
        VK_LOG_WARN("LightTextureLibrary: IES '%s' bad dims %dx%d", path.c_str(),
                    nVert, nHoriz);
        return -1;
    }

    // 头行之后的所有数字 token
    std::string rest = text.substr(static_cast<size_t>(headerStart));
    std::istringstream ts(rest);
    std::vector<float> nums;
    double val = 0;
    while (ts >> val) nums.push_back(static_cast<float>(val));

    const size_t need = static_cast<size_t>(nVert) + nHoriz +
                        static_cast<size_t>(nVert) * nHoriz;
    if (nums.size() < need) {
        VK_LOG_WARN("LightTextureLibrary: IES '%s' truncated (%zu < %zu)",
                    path.c_str(), nums.size(), need);
        return -1;
    }

    std::vector<float> vert(nums.begin(), nums.begin() + nVert);
    std::vector<float> horiz(nums.begin() + nVert, nums.begin() + nVert + nHoriz);
    std::vector<float> cand(nums.begin() + nVert + nHoriz,
                            nums.begin() + nVert + nHoriz + nVert * nHoriz);
    for (float& c : cand) c *= static_cast<float>(mult);

    std::vector<uint8_t> buf;
    bakeIes(vert, horiz, cand, buf);

    const uint32_t layer = m_usedLayers;
    stageLayer(layer, buf);
    m_names[layer] = name;

    // 全量重建（层内容变了）。只读使用，重建代价可忽略。
    m_device.waitIdle();
    destroyImage();
    upload();

    VK_LOG_INFO("LightTextureLibrary: loaded IES '%s' -> layer %u (%dx%d)",
                name.c_str(), layer, nVert, nHoriz);
    return static_cast<int>(layer);
}

} // namespace render
