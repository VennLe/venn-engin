#include "render/TerrainPass.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "render/RenderTypes.h"
#include "render/ShaderPath.h"
#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"
#include "rhi/Image.h"
#include "rhi/Pipeline.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>

namespace render {

namespace {

using json = nlohmann::json;

// 地形层材质的**数值部分**直接从 `.vmat` 读，不走 AssetManager：
//   · 渲染层不持有 AssetManager（那是 editor/runtime 的所有物），
//     为它加一条依赖不划算；
//   · `.vmat` 的数值字段就是 baseColor / roughness / metallic / ao /
//     normalScale 这几个数，读 JSON 就够；
//   · 贴图走 assets::Texture（TextureContext 是 Renderer 给的）。
// ⚠ 这意味着**只有文件贴图**能进地形层；程序化纹理（Checker/Solid）暂时
//   解析不了 —— 当前 4 个地形层材质都没挂贴图，等真挂上了再补。
bool loadLayerJson(const std::string& layerId, TerrainLayerGpu& out,
                   std::string* albedoPath, std::string* normalPath) {
    albedoPath->clear();
    normalPath->clear();
    const std::string rel = layerId + ".vmat";
    const std::string abs = assets::resolveAssetPath(rel);
    std::ifstream f(abs);
    if (!f) {
        VK_LOG_WARN("TERRAIN-LAYER-MISS id=%s path=%s", layerId.c_str(),
                    abs.c_str());
        return false;
    }
    json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        VK_LOG_WARN("TERRAIN-LAYER-FAIL id=%s reason=%s", layerId.c_str(),
                    e.what());
        return false;
    }
    if (doc.contains("baseColor") && doc["baseColor"].is_array() &&
        doc["baseColor"].size() >= 4) {
        out.baseColor = glm::vec4(doc["baseColor"][0].get<float>(),
                                  doc["baseColor"][1].get<float>(),
                                  doc["baseColor"][2].get<float>(),
                                  doc["baseColor"][3].get<float>());
    }
    out.params.x = doc.value("roughness", 0.8f);
    out.params.y = doc.value("metallic", 0.0f);
    out.params.z = doc.value("ao", 1.0f);
    out.params.w = doc.value("normalScale", 1.0f);

    // 贴图：材质 JSON 里是 {"kind":"file","path":"textures/x.png"}
    //
    // ⚠ 这里必须走 `resolveAssetPath`：材质里存的是**相对资产根**的路径，
    //   而 `Texture::fromFile` 直接把它交给 stbi_load —— 那是相对**进程 cwd**
    //   打开的。之前引擎自带的两块地形材质都没有贴图，所以这条从来没被走到；
    //   一旦地形层带贴图就立刻 `can't fopen` → throw → 整个编辑器 Fatal。
    auto texPath = [&doc](const char* key) -> std::string {
        if (!doc.contains(key) || !doc[key].is_object()) return std::string();
        const auto& o = doc[key];
        if (!o.contains("path") || !o["path"].is_string()) return std::string();
        const std::string raw = o["path"].get<std::string>();
        if (raw.empty()) return raw;
        return assets::resolveAssetPath(raw);
    };
    *albedoPath = texPath("albedo");
    *normalPath = texPath("normal");
    return true;
}

VkSampler makeSampler(rhi::Device& device, VkFilter filter,
                      VkSamplerAddressMode addr) {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = filter;
    si.minFilter = filter;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = addr;
    si.addressModeV = addr;
    si.addressModeW = addr;
    VkSampler s = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(device.get(), &si, nullptr, &s));
    return s;
}

// ============================================================
// 列主序 → 行主序（2026-10-21）
//
// ⚠ 这是"刷地形位置对不上"的根因，务必看清楚。
//
// 存储约定两边**不一样**：
//   · `height.r32` / `splat.rgba8` / `holes.r8` 是**列主序**：
//       heights[col * rows + row]
//     （physics::Heightfield 的约定，Heightfield.h 顶部写明了；
//      CPU 侧 heightAt / gridOf / 笔刷 applyStroke 全按这个顺序索引）
//   · 纹理是**行主序**：texel(x, y) = data[y * width + x]
//     而 terrain.vert 用 `uv = ((col+0.5)/cols, (row+0.5)/rows)` 采样，
//     也就是"纹素 (x=col, y=row) 应该就是 H(col,row)"。
//
// 原样把列主序数组喂给 upload2D（width=cols, height=rows）意味着
//   texel(col, row) = data[row * cols + col] = H(col=row, row=col)
// —— 整片地形绕对角线**镜像**了。物理（角色踩地、笔刷射线）用的是
// H(col,row)，屏幕上是 H(row,col)，于是"鼠标刷的地方和隆起出现的地方
// 不同"，而且离对角线越远偏得越多（257×257 的地形上，最高点会被画到
// 200 多米以外）。天空/噪声看起来还挺合理，所以肉眼不容易发现。
//
// 修法：上传前翻一次（264 KB，几十微秒），让"纹素 = H(col,row)"成立。
// 注意**只翻上传的那份拷贝**：`inst.asset.heights` 必须保持列主序，
// 因为 LOD 误差、CPU 侧查询都按列主序读它。
// ============================================================
template <typename T>
std::vector<T> toRowMajor(const std::vector<T>& src, int cols, int rows) {
    std::vector<T> out(src.size());
    for (int c = 0; c < cols; ++c) {
        const std::size_t cOff = static_cast<std::size_t>(c) *
                                 static_cast<std::size_t>(rows);
        for (int r = 0; r < rows; ++r) {
            out[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) +
                static_cast<std::size_t>(c)] = src[cOff +
                                                   static_cast<std::size_t>(r)];
        }
    }
    return out;
}

// 把一段像素（或浮点）数据上传成 2D 纹理。
// ⚠ height 用 R32_SFLOAT：**没有 sRGB 变体**，所以"线性采样"是格式本身
//   保证的，不依赖任何标志位（这正是 W4 选裸浮点的第二个理由）。
template <typename T>
std::unique_ptr<rhi::Image> upload2D(rhi::Device& device,
                                     rhi::CommandPool& cmdPool, uint32_t w,
                                     uint32_t h, VkFormat format,
                                     const T* data, std::size_t bytes) {
    auto img = std::make_unique<rhi::Image>(
        device, w, h, format,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    rhi::Image::transitionLayout(device, cmdPool, img->get(),
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    auto staging = rhi::Buffer::createDeviceLocal(
        device, cmdPool, data, static_cast<VkDeviceSize>(bytes),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    // createDeviceLocal 建的是 DEVICE_LOCAL，这里只是借它做 staging：
    // 直接把 CPU 数据传进一个 transfer-src 缓冲，语义上等价。
    rhi::Image::copyBufferToImage(device, cmdPool, staging->get(), img->get(), w,
                                  h);
    rhi::Image::transitionLayout(device, cmdPool, img->get(),
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return img;
}

} // namespace

// ============================================================
// 原地重传一张 2D 图（地形笔刷用）
//
// 三段，但只开**一个**一次性 command buffer：
//   SHADER_READ_ONLY → TRANSFER_DST  (barrier)
//   vkCmdCopyBufferToImage
//   TRANSFER_DST → SHADER_READ_ONLY  (barrier)
// endAndSubmit 内部有 vkQueueWaitIdle，所以 staging 可以直接析构。
//
// ⚠ 阶段掩码必须带上 VERTEX_SHADER：高度图是在 terrain.vert 里采样的。
//   只写 FRAGMENT_SHADER 的话，"上一帧顶点读取"与"这一帧传输写"之间没有
//   同步关系 —— 表现是刷完当场看不见变化，过一会儿才跳一下。
//
// ⚠ 不复用 rhi::Image::transitionLayout：它的阶段表只覆盖
//   UNDEFINED↔TRANSFER_DST↔SHADER_READ_ONLY 三态，且 dstStage 只到
//   片元；而且那是"一次提交一次等队列"，两次调用就是两次
//   vkQueueWaitIdle。
// ============================================================
namespace {

// reuse = 常驻 staging（笔刷路径传实例里的那块）。给 nullptr 就临时建一个。
//
// ⚠ 每帧新建/销毁 VkBuffer 是这个函数原先最大的开销（一次 refresh 一次
//   分配 + 一次 vkQueueWaitIdle）。2026-10-21 加了这个复用参数：刷地形时
//   只写数据、只提交拷贝，分配次数降到 0。
bool refillImage2D(rhi::Device& device, rhi::CommandPool& cmdPool,
                   rhi::Image& img, const void* data, std::size_t bytes,
                   rhi::Buffer* reuse = nullptr) {
    if (!data || bytes == 0) return false;

    // staging：HOST_VISIBLE + TRANSFER_SRC。**不是** createDeviceLocal ——
    // 那条路径会再建一个中间缓冲并等一次队列，对 264 KB 的图不划算。
    std::unique_ptr<rhi::Buffer> owned;
    rhi::Buffer* staging = reuse;
    if (!staging || staging->size() < static_cast<VkDeviceSize>(bytes)) {
        owned = std::make_unique<rhi::Buffer>(
            device, static_cast<VkDeviceSize>(bytes),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging = owned.get();
    }
    staging->writeData(data, static_cast<VkDeviceSize>(bytes));

    VkCommandBuffer cmd = cmdPool.beginOneTime();

    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.get();
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;

    const VkPipelineStageFlags kSampleStages =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, kSampleStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {img.width(), img.height(), 1};
    vkCmdCopyBufferToImage(cmd, staging->get(), img.get(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, kSampleStages, 0,
                         0, nullptr, 0, nullptr, 1, &b);

    cmdPool.endAndSubmit(cmd);
    return true;
}

} // namespace

TerrainPass::~TerrainPass() { shutdown(); }

bool TerrainPass::init(const InitInfo& info) {
    if (!info.device || !info.cmdPool || info.globalSetLayout == VK_NULL_HANDLE ||
        info.renderPass == VK_NULL_HANDLE) {
        VK_LOG_ERROR("TERRAIN-INIT-FAIL missing_init_info");
        return false;
    }
    m_device = info.device;
    m_cmdPool = info.cmdPool;
    m_texCtx = info.texCtx;
    m_globalSetLayout = info.globalSetLayout;
    m_renderPass = info.renderPass;
    m_samples = info.samples;

    // ---- set 1：地形专用（4 albedo + 4 normal + 权重 + 高度 + 层参数 + 挖洞遮罩）----
    {
        VkDescriptorSetLayoutBinding b[6]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 4;
        b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1] = b[0];
        b[1].binding = 1;  // normal[4]
        b[2].binding = 2;
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2].descriptorCount = 1;
        b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;  // splat
        b[3].binding = 3;
        b[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[3].descriptorCount = 1;
        // 高度图**顶点阶段也要采**（位移在 VS 里做）
        b[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                          VK_SHADER_STAGE_FRAGMENT_BIT;
        b[4].binding = 4;
        b[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[4].descriptorCount = 1;
        b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        // binding 5：挖洞遮罩（R8_UNORM）。只在片元里 discard 用 ——
        // 顶点阶段不采，否则"挖穿"会让顶点位置也抖（洞的边缘会出现
        // 一圈被拉向地心的三角形）。
        b[5].binding = 5;
        b[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[5].descriptorCount = 1;
        b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        m_terrainSetLayout = rhi::makeSetLayout(
            *m_device, std::vector<VkDescriptorSetLayoutBinding>(b, b + 6));
    }

    // 每实例 1 个 set，各 11 个采样器 + 1 个 UBO
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        *m_device, 4, 4, 44);

    createPipeline();
    return m_pipeline != nullptr;
}

void TerrainPass::createPipeline() {
    // 顶点布局：pos(vec3) + uv(vec2) + skirt(float) = 24 字节
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(terrain::TerrainVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;  // pos
    attrs[0].offset = offsetof(terrain::TerrainVertex, pos);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;  // uv
    attrs[1].offset = offsetof(terrain::TerrainVertex, uv);
    attrs[2].location = 2;
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32_SFLOAT;  // skirt
    attrs[2].offset = offsetof(terrain::TerrainVertex, skirt);

    rhi::PipelineDesc desc;
    desc.vertSpv = vkutil::readFile(resolveShaderPath("terrain.vert.spv"));
    desc.fragSpv = vkutil::readFile(resolveShaderPath("terrain.frag.spv"));
    desc.vertexBindings.push_back(binding);
    desc.vertexAttributes.assign(attrs, attrs + 3);
    // ⚠ set 0 必须是**同一份** layout 对象；push range 必须是 128 字节
    //   （与主前向逐字段相同），否则 set 0 的绑定会被判为"被打断"
    //   → 后面所有 draw 报 VUID-...-08600。地形自己只推 96 字节，
    //   但 range 写小了同样不兼容。
    desc.setLayouts = {m_globalSetLayout, m_terrainSetLayout};
    desc.pushConstant.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    desc.pushConstant.offset = 0;
    desc.pushConstant.size = sizeof(PushConstants);
    desc.renderPass = m_renderPass;
    desc.samples = m_samples;
    desc.depthTest = true;
    desc.depthWrite = true;
    // cullMode = NONE：裙边的三角形绕序取决于"从哪条边看"，逐面绕序对齐
    // 只是白费力气；顺带让"相机跑到地面以下"时不会看到一片黑。
    desc.cullMode = VK_CULL_MODE_NONE;
    desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    m_pipeline = std::make_unique<rhi::Pipeline>(*m_device, desc);
    VK_LOG_INFO("TERRAIN-PIPELINE vert=terrain.vert.spv frag=terrain.frag.spv "
                "sets=2 cull=none");
}

void TerrainPass::recreatePipeline(VkRenderPass renderPass,
                                   VkSampleCountFlagBits samples) {
    if (!m_device) return;
    if (m_renderPass == renderPass && m_samples == samples && m_pipeline) return;
    m_renderPass = renderPass;
    m_samples = samples;
    m_pipeline.reset();
    createPipeline();
}

void TerrainPass::clearInstances() {
    if (!m_device) {
        m_instances.clear();
        return;
    }
    // ⚠ 必须等设备空闲再销毁。
    //
    //   这条函数在 CPU 阶段被调用，而此时**前几帧的命令缓冲可能还在 GPU 上
    //   跑**（帧在飞行中），里面正引用着下面这些 sampler / image / buffer。
    //   直接 vkDestroy* 会触发验证层错误：
    //     vkDestroySampler(): sampler can't be called on VkSampler 0x... that
    //     is currently in use by VkCommandBuffer 0x...
    //   在真机上这类错误不会崩，只是"随机花屏"；但有些驱动会直接返回
    //   VK_ERROR_DEVICE_LOST（实测 RTX 上刷地形几秒钟就死，见
    //   LandscapeEditor 的笔刷路径 —— 它以前每 80 ms 走一次重建）。
    //
    //   代价：一次全设备同步。只在"集合变了 / 层材质改了"时才会走到这里，
    //   笔刷的常规刷新走 refreshSurface，不碰这条路径。
    m_device->waitIdle();
    for (auto& inst : m_instances) {
        if (inst->heightSampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device->get(), inst->heightSampler, nullptr);
            inst->heightSampler = VK_NULL_HANDLE;
        }
        if (inst->splatSampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device->get(), inst->splatSampler, nullptr);
            inst->splatSampler = VK_NULL_HANDLE;
        }
        if (inst->holeSampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device->get(), inst->holeSampler, nullptr);
            inst->holeSampler = VK_NULL_HANDLE;
        }
    }
    m_instances.clear();
    m_loggedLod = false;
    m_lastLod = LodStats{};
}

// ============================================================
// 笔刷的原地刷新（地形雕刻链路的关键一环）
//
// 与"重建"的区别就一句话：**不销毁、不新建任何 VkObject**。
//   · 顶点缓冲不动（terrain.vert 里 pos.z 是采样高度图加出来的）
//   · sampler / 描述符集不动（图还是那张图，只是内容换了）
//   · 分块网格与 LOD 不动（块的 XY 中心不依赖高度）
// 只做三件事：取到新数据 → 翻成行主序 → 把 R32/RGBA8/R8 原地重传。
//
// ------------------------------------------------------------
// 数据从哪来（2026-10-21 改）
// ------------------------------------------------------------
// 原来**只**从磁盘读（loadTerrain）：编辑器先写盘、这里再读盘，一次
// 刷新光 I/O 就是 2 MB，而且编辑器那边每 80 ms 就写一次 —— 合起来就是
// "刷地形延迟很大"的主因。
//
// 现在优先用 `live`（编辑器内存里的那份资产，经 TerrainComponent 的
// liveHeights/liveSplat/liveHoles 传下来）。live == nullptr 时才退回
// 读盘（比如别的工具改了资产文件，或者编辑器没在刷）。
//
// 上传前必须 toRowMajor：见上面那个函数的注释（不做的话整片地形绕
// 对角线镜像，"鼠标刷的位置"和"隆起出现的位置"对不上）。
// ============================================================
bool TerrainPass::refreshSurface(uint32_t index, bool splatToo, bool holesToo,
                                 const terrain::TerrainAsset* live) {
    if (!m_device || !m_cmdPool || index >= m_instances.size()) return false;
    Instance& inst = *m_instances[index];
    if (!inst.heightImage || !inst.splatImage) return false;

    const auto t0 = std::chrono::steady_clock::now();

    const int cols = inst.asset.desc.cols;
    const int rows = inst.asset.desc.rows;

    // ---- 取数据 ----
    if (live) {
        if (live->heights.size() != inst.asset.heights.size()) {
            VK_LOG_ERROR("TERRAIN-REFRESH-SIZE path=%s live=%zu want=%zu",
                         inst.asset.path.c_str(), live->heights.size(),
                         inst.asset.heights.size());
            return false;
        }
        inst.asset.heights = live->heights;
        if (splatToo) inst.asset.splat = live->splat;
        if (holesToo) inst.asset.holes = live->holes;
    } else {
        terrain::TerrainAsset fresh;
        std::string err;
        if (!terrain::loadTerrain(inst.asset.path, fresh, &err, /*quiet=*/true)) {
            VK_LOG_ERROR("TERRAIN-REFRESH-FAIL path=%s reason=%s",
                         inst.asset.path.c_str(), err.c_str());
            return false;
        }
        // 尺寸变了说明资产被换掉了，图片尺寸对不上 → 只能用重建路径
        if (fresh.heights.size() != inst.asset.heights.size() ||
            fresh.splat.size() != inst.asset.splat.size()) {
            VK_LOG_ERROR("TERRAIN-REFRESH-SIZE path=%s got=%zu/%zu want=%zu/%zu",
                         inst.asset.path.c_str(), fresh.heights.size(),
                         fresh.splat.size(), inst.asset.heights.size(),
                         inst.asset.splat.size());
            return false;
        }
        inst.asset.heights = std::move(fresh.heights);
        if (splatToo) inst.asset.splat = std::move(fresh.splat);
        if (holesToo) inst.asset.holes = std::move(fresh.holes);
    }

    // ---- 上传（列主序 → 行主序）----
    if (!inst.uploadBuf ||
        inst.uploadBuf->size() <
            static_cast<VkDeviceSize>(inst.asset.heights.size() * sizeof(float))) {
        inst.uploadBuf = std::make_unique<rhi::Buffer>(
            *m_device,
            static_cast<VkDeviceSize>(inst.asset.heights.size() * sizeof(float)),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    const std::vector<float> hRow = toRowMajor(inst.asset.heights, cols, rows);
    refillImage2D(*m_device, *m_cmdPool, *inst.heightImage, hRow.data(),
                  hRow.size() * sizeof(float), inst.uploadBuf.get());
    if (splatToo) {
        const std::vector<std::uint8_t> sRow =
            toRowMajor(inst.asset.splat, cols, rows);
        refillImage2D(*m_device, *m_cmdPool, *inst.splatImage, sRow.data(),
                      sRow.size(), inst.uploadBuf.get());
    }
    if (holesToo && inst.holeImage &&
        inst.asset.holes.size() ==
            static_cast<std::size_t>(inst.holeImage->width()) *
                static_cast<std::size_t>(inst.holeImage->height())) {
        const std::vector<std::uint8_t> oRow =
            toRowMajor(inst.asset.holes, cols, rows);
        refillImage2D(*m_device, *m_cmdPool, *inst.holeImage, oRow.data(),
                      oRow.size(), inst.uploadBuf.get());
    }

    // ---- 计时日志（前几次每次都打，之后每 8 次一条）----
    // 前几次不能省：验收脚本要读 maxMs 判断"笔刷是不是实时的"，而一笔短
    // 拖拽只产生 1~3 次刷新，按 8 取模就一条都打不出来。
    const float ms = std::chrono::duration<float, std::milli>(
                         std::chrono::steady_clock::now() - t0).count();
    ++inst.refreshCount;
    inst.refreshMaxMs = std::max(inst.refreshMaxMs, ms);
    if (inst.refreshCount <= 6u || (inst.refreshCount & 7u) == 0u) {
        VK_LOG_INFO("TERRAIN-REFRESH n=%u lastMs=%.2f maxMs=%.2f "
                    "(row-major, src=%s)",
                    inst.refreshCount, static_cast<double>(ms),
                    static_cast<double>(inst.refreshMaxMs),
                    live ? "memory" : "disk");
    }
    return true;
}

void TerrainPass::shutdown() {
    clearInstances();
    m_pipeline.reset();
    m_defaultAlbedo.reset();
    m_defaultNormal.reset();
    m_descriptors.reset();
    if (m_terrainSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device->get(), m_terrainSetLayout, nullptr);
        m_terrainSetLayout = VK_NULL_HANDLE;
    }
    m_device = nullptr;
}

TerrainPass::Instance* TerrainPass::createTerrain(
    const std::string& relPath, const glm::mat4& model, float heightScale,
    const std::array<std::string, 4>* overrides) {
    if (!m_pipeline) return nullptr;
    // 缺省贴图（白 / 平法线）**惰性创建**：它们要 textureSetLayout，
    // 而那个布局在 Renderer::init 里是建完管线之后才有的。
    if (!m_defaultAlbedo) {
        uint8_t white[4] = {255, 255, 255, 255};
        m_defaultAlbedo = std::make_unique<assets::Texture>();
        m_defaultAlbedo->makeSolid(m_texCtx, white, 4, true);
        uint8_t flat[4] = {128, 128, 255, 255};
        m_defaultNormal = std::make_unique<assets::Texture>();
        m_defaultNormal->makeSolid(m_texCtx, flat, 4, false);
    }

    auto inst = std::make_unique<Instance>();
    inst->model = model;
    inst->heightScale = (heightScale > 1.0e-4f) ? heightScale : 1.0f;

    std::string err;
    if (!terrain::loadTerrain(relPath, inst->asset, &err)) {
        VK_LOG_ERROR("TERRAIN-INSTANCE-FAIL path=%s reason=%s", relPath.c_str(),
                     err.c_str());
        return nullptr;
    }
    inst->chunks = terrain::buildChunks(inst->asset);
    if (inst->chunks.empty()) return nullptr;
    inst->chunkSizeMeters = static_cast<float>(terrain::kChunkCells) *
                            inst->asset.desc.metersPerPixel;

    // ---- 层材质 ----
    for (int i = 0; i < 4; ++i) {
        Instance::Layer& L = inst->layers[static_cast<std::size_t>(i)];
        L.id = inst->asset.desc.layerIds[static_cast<std::size_t>(i)];
        // 组件覆盖优先（编辑器 Inspector 的 4 个槽位）
        if (overrides && !(*overrides)[static_cast<std::size_t>(i)].empty()) {
            L.id = (*overrides)[static_cast<std::size_t>(i)];
        }
        if (L.id.empty()) continue;
        TerrainLayerGpu gpu;
        std::string albedoPath, normalPath;
        const bool ok = loadLayerJson(L.id, gpu, &albedoPath, &normalPath);
        L.baseColor = gpu.baseColor;
        L.params = gpu.params;
        // ⚠ fromFile 失败会 throw —— 一块地形的贴图丢了不该让整个编辑器挂掉，
        //   降级成"默认贴图"（纯色）继续渲染，并留一条可搜索的日志。
        if (ok && !albedoPath.empty()) {
            try {
                L.albedo = std::make_unique<assets::Texture>();
                L.albedo->fromFile(m_texCtx, albedoPath, true);
            } catch (const std::exception& ex) {
                L.albedo.reset();
                VK_LOG_WARN("TERRAIN-LAYER-TEX-FAIL idx=%d albedo=%s (%s)", i,
                            albedoPath.c_str(), ex.what());
            }
        }
        if (ok && !normalPath.empty()) {
            try {
                L.normal = std::make_unique<assets::Texture>();
                L.normal->fromFile(m_texCtx, normalPath, false);
                inst->normalMask |= (1u << i);
            } catch (const std::exception& ex) {
                L.normal.reset();
                VK_LOG_WARN("TERRAIN-LAYER-TEX-FAIL idx=%d normal=%s (%s)", i,
                            normalPath.c_str(), ex.what());
            }
        }
        VK_LOG_INFO("TERRAIN-LAYER idx=%d id=%s albedo=%s normal=%s",
                    i, L.id.c_str(), L.albedo ? "file" : "default",
                    L.normal ? "file" : "default");
    }

    if (!createInstanceResources(*inst)) return nullptr;

    VK_LOG_INFO("TERRAIN-INSTANCE path=%s chunks=%d verts=%zu idx=%zu",
                relPath.c_str(), static_cast<int>(inst->chunks.size()),
                inst->ranges.empty() ? 0u : inst->ranges.size(),
                static_cast<std::size_t>(0));
    m_instances.push_back(std::move(inst));
    m_loggedLod = false;
    return m_instances.back().get();
}

bool TerrainPass::createInstanceResources(Instance& inst) {
    const terrain::TerrainDesc& d = inst.asset.desc;

    // ---- 顶点/索引合并成一对大缓冲 ----
    std::vector<terrain::TerrainVertex> allVerts;
    std::vector<std::uint32_t> allIdx;
    std::size_t vtxTotal = 0, idxTotal = 0;
    for (const auto& ch : inst.chunks) {
        for (const auto& m : ch.lods) {
            vtxTotal += m.verts.size();
            idxTotal += m.idx.size();
        }
    }
    allVerts.reserve(vtxTotal);
    allIdx.reserve(idxTotal);

    inst.ranges.resize(inst.chunks.size() * terrain::kLodCount);
    for (std::size_t c = 0; c < inst.chunks.size(); ++c) {
        for (int lod = 0; lod < terrain::kLodCount; ++lod) {
            const terrain::TerrainMeshData& m =
                inst.chunks[c].lods[static_cast<std::size_t>(lod)];
            // 索引是**块内**的局部下标，合并后要整体平移
            const std::uint32_t base = static_cast<std::uint32_t>(allVerts.size());
            inst.ranges[c * terrain::kLodCount + static_cast<std::size_t>(lod)] = {
                static_cast<std::uint32_t>(allIdx.size()),
                static_cast<std::uint32_t>(m.idx.size())};
            allVerts.insert(allVerts.end(), m.verts.begin(), m.verts.end());
            for (std::uint32_t i : m.idx) allIdx.push_back(i + base);
        }
    }

    inst.vbo = rhi::Buffer::createDeviceLocal(
        *m_device, *m_cmdPool, allVerts.data(),
        static_cast<VkDeviceSize>(allVerts.size() * sizeof(terrain::TerrainVertex)),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    inst.ibo = rhi::Buffer::createDeviceLocal(
        *m_device, *m_cmdPool, allIdx.data(),
        static_cast<VkDeviceSize>(allIdx.size() * sizeof(std::uint32_t)),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    // ---- 高度图 / 权重图 ----
    const uint32_t w = static_cast<uint32_t>(d.cols);
    const uint32_t h = static_cast<uint32_t>(d.rows);
    // ⚠ 三张图都要先翻成**行主序**再上传（见 toRowMajor 的说明）：
    //   资产是列主序，"原样上传"会让整片地形绕对角线镜像 —— 物理与屏幕
    //   各说各话，笔刷自然"刷这里、那里鼓起"。
    const std::vector<float> heightsRow =
        toRowMajor(inst.asset.heights, d.cols, d.rows);
    const std::vector<std::uint8_t> splatRow =
        toRowMajor(inst.asset.splat, d.cols, d.rows);
    inst.heightImage = upload2D(*m_device, *m_cmdPool, w, h,
                                VK_FORMAT_R32_SFLOAT, heightsRow.data(),
                                heightsRow.size() * sizeof(float));
    inst.splatImage = upload2D(*m_device, *m_cmdPool, w, h,
                               VK_FORMAT_R8G8B8A8_UNORM, splatRow.data(),
                               splatRow.size());
    // 挖洞遮罩：R8。**必须 CLAMP_TO_EDGE + 线性**，与高度图同一套采样规则 ——
    // 用 REPEAT 的话地形边界外半纹素会采到对边的遮罩，边缘出现"补回来的一圈地"。
    if (inst.asset.holes.size() ==
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
        const std::vector<std::uint8_t> holesRow =
            toRowMajor(inst.asset.holes, d.cols, d.rows);
        inst.holeImage = upload2D(*m_device, *m_cmdPool, w, h,
                                  VK_FORMAT_R8_UNORM, holesRow.data(),
                                  holesRow.size());
    } else {
        // 兜底：全实心（1×1 的 255）。理论上走不到 —— loadTerrain 一定会
        // 填满这个长度；但少一张图会让描述符写入读到空句柄。
        const uint8_t solid = 255u;
        inst.holeImage = upload2D(*m_device, *m_cmdPool, 1, 1,
                                  VK_FORMAT_R8_UNORM, &solid, 1);
    }
    // 高度：LINEAR + CLAMP_TO_EDGE。CLAMP 是必需的 —— uv 在边界上正好是
    // 半纹素，REPEAT 会把对边的高度拖过来（地形边缘出现一条"翻折"）。
    inst.heightSampler = makeSampler(*m_device, VK_FILTER_LINEAR,
                                     VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    // 权重：LINEAR + REPEAT（splatRepeat 允许平铺）
    inst.splatSampler = makeSampler(*m_device, VK_FILTER_LINEAR,
                                    VK_SAMPLER_ADDRESS_MODE_REPEAT);
    inst.holeSampler = makeSampler(*m_device, VK_FILTER_LINEAR,
                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // ---- 层参数 UBO ----
    inst.layerUbo = std::make_unique<rhi::Buffer>(
        *m_device, sizeof(TerrainLayerGpu) * 4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    {
        TerrainLayerGpu gpu[4];
        for (int i = 0; i < 4; ++i) {
            gpu[i].baseColor = inst.layers[static_cast<std::size_t>(i)].baseColor;
            gpu[i].params = inst.layers[static_cast<std::size_t>(i)].params;
        }
        inst.layerUbo->writeData(gpu, sizeof(gpu));
    }

    // ---- 描述符集 ----
    inst.texSet = m_descriptors->allocate(m_terrainSetLayout);
    for (uint32_t i = 0; i < 4; ++i) {
        const auto& L = inst.layers[i];
        const assets::Texture* a = L.albedo ? L.albedo.get() : m_defaultAlbedo.get();
        m_descriptors->writeTextureAt(inst.texSet, 0, i, a->sampler(), a->view());
        const assets::Texture* n = L.normal ? L.normal.get() : m_defaultNormal.get();
        m_descriptors->writeTextureAt(inst.texSet, 1, i, n->sampler(), n->view());
    }
    m_descriptors->writeTexture(inst.texSet, 2, inst.splatSampler,
                                inst.splatImage->view());
    m_descriptors->writeTexture(inst.texSet, 3, inst.heightSampler,
                                inst.heightImage->view());
    m_descriptors->writeBuffer(inst.texSet, 4, inst.layerUbo->get(),
                               sizeof(TerrainLayerGpu) * 4,
                               VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    m_descriptors->writeTexture(inst.texSet, 5, inst.holeSampler,
                                inst.holeImage->view());
    return true;
}

uint64_t TerrainPass::record(VkCommandBuffer cmd, const glm::vec3& camPos) {
    if (!m_pipeline || m_instances.empty()) return 0;

    uint64_t triangles = 0;
    LodStats stats;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_pipeline->get());

    for (auto& up : m_instances) {
        Instance& inst = *up;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeline->layout(), 1, 1, &inst.texSet, 0,
                                nullptr);
        VkBuffer vb = inst.vbo->get();
        const VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
        vkCmdBindIndexBuffer(cmd, inst.ibo->get(), 0, VK_INDEX_TYPE_UINT32);

        const terrain::TerrainDesc& d = inst.asset.desc;
        TerrainPush push{};
        push.model = inst.model;
        // ⚠ params.x = maxHeight × heightScale。物理侧的 Heightfield.heightScale
        //   必须取同一个乘积，否则角色会悬空/陷进地里 —— 而画面看起来完全正常。
        push.params = glm::vec4(d.maxHeight * inst.heightScale, d.splatRepeat,
                                d.layerTiling, d.metersPerPixel);
        push.params2 = glm::vec4(static_cast<float>(d.cols),
                                 static_cast<float>(d.rows), terrain::kSkirtDepth,
                                 static_cast<float>(inst.normalMask));
        vkCmdPushConstants(cmd, m_pipeline->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);

        // 相机位置变到地形的**局部空间**再算距离：地形可以平移/绕 Z 转，
        // 用世界坐标算会让 "同一块地形移动之后 LOD 分布变了"。
        const glm::mat4 inv = glm::inverse(inst.model);
        const glm::vec3 localCam = glm::vec3(inv * glm::vec4(camPos, 1.0f));

        for (std::size_t c = 0; c < inst.chunks.size(); ++c) {
            const terrain::TerrainChunk& ch = inst.chunks[c];
            const float dist = glm::length(glm::vec2(localCam.x, localCam.y) -
                                           ch.center);
            const int lod = terrain::lodForDistance(dist, inst.chunkSizeMeters);
            const Instance::Range& r =
                inst.ranges[c * terrain::kLodCount + static_cast<std::size_t>(lod)];
            if (r.indexCount == 0) continue;
            vkCmdDrawIndexed(cmd, r.indexCount, 1, r.firstIndex, 0, 0);
            stats.chunks[lod] += 1;
            stats.total += 1;
            triangles += r.indexCount / 3u;
        }
    }
    stats.triangles = triangles;

    // LOD 分布变化时才打日志（相机静止时只打一次）。
    // verify_terrain 靠"移动相机 → 分布变了"来证明 LOD 真的在按距离选。
    if (!m_loggedLod || stats.chunks[0] != m_lastLod.chunks[0] ||
        stats.chunks[1] != m_lastLod.chunks[1] ||
        stats.chunks[2] != m_lastLod.chunks[2] ||
        stats.chunks[3] != m_lastLod.chunks[3]) {
        VK_LOG_INFO("TERRAIN-LOD chunks=%u lod0=%u lod1=%u lod2=%u lod3=%u tris=%llu",
                    stats.total, stats.chunks[0], stats.chunks[1],
                    stats.chunks[2], stats.chunks[3],
                    static_cast<unsigned long long>(triangles));
        m_lastLod = stats;
        m_loggedLod = true;
    }
    return triangles;
}

} // namespace render
