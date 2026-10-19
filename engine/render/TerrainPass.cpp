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
    auto texPath = [&doc](const char* key) -> std::string {
        if (!doc.contains(key) || !doc[key].is_object()) return std::string();
        const auto& o = doc[key];
        if (!o.contains("path") || !o["path"].is_string()) return std::string();
        return o["path"].get<std::string>();
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

    // ---- set 1：地形专用（4 albedo + 4 normal + 权重 + 高度 + 层参数）----
    {
        VkDescriptorSetLayoutBinding b[5]{};
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
        m_terrainSetLayout = rhi::makeSetLayout(
            *m_device, std::vector<VkDescriptorSetLayoutBinding>(b, b + 5));
    }

    // 每实例 1 个 set，各 10 个采样器 + 1 个 UBO
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        *m_device, 4, 4, 40);

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
    for (auto& inst : m_instances) {
        if (inst->heightSampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device->get(), inst->heightSampler, nullptr);
            inst->heightSampler = VK_NULL_HANDLE;
        }
        if (inst->splatSampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device->get(), inst->splatSampler, nullptr);
            inst->splatSampler = VK_NULL_HANDLE;
        }
    }
    m_instances.clear();
    m_loggedLod = false;
    m_lastLod = LodStats{};
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
        if (ok && !albedoPath.empty()) {
            L.albedo = std::make_unique<assets::Texture>();
            L.albedo->fromFile(m_texCtx, albedoPath, true);
        }
        if (ok && !normalPath.empty()) {
            L.normal = std::make_unique<assets::Texture>();
            L.normal->fromFile(m_texCtx, normalPath, false);
            inst->normalMask |= (1u << i);
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
    inst.heightImage = upload2D(*m_device, *m_cmdPool, w, h,
                                VK_FORMAT_R32_SFLOAT, inst.asset.heights.data(),
                                inst.asset.heights.size() * sizeof(float));
    inst.splatImage = upload2D(*m_device, *m_cmdPool, w, h,
                               VK_FORMAT_R8G8B8A8_UNORM, inst.asset.splat.data(),
                               inst.asset.splat.size());
    // 高度：LINEAR + CLAMP_TO_EDGE。CLAMP 是必需的 —— uv 在边界上正好是
    // 半纹素，REPEAT 会把对边的高度拖过来（地形边缘出现一条"翻折"）。
    inst.heightSampler = makeSampler(*m_device, VK_FILTER_LINEAR,
                                     VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    // 权重：LINEAR + REPEAT（splatRepeat 允许平铺）
    inst.splatSampler = makeSampler(*m_device, VK_FILTER_LINEAR,
                                    VK_SAMPLER_ADDRESS_MODE_REPEAT);

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
