#include "render/MdiDraw.h"

#include "render/HiZOcclusion.h"   // HiZAabb（AABB 描述符的字节数）
#include "render/ShaderPath.h"

#include "rhi/Buffer.h"
#include "rhi/CommandPool.h"
#include "rhi/ComputePipeline.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"

#include "core/Logger.h"

#include <algorithm>
#include <cstring>

namespace render {

namespace {

// 与 mdi_compact.comp 的 push_constant 一致：mat4 + vec4×3 + uvec4 = 112 字节。
struct MdiPush {
    glm::mat4 viewProj{1.0f};
    glm::vec4 params{0.0f};  // x=物体数 y=深度偏置 z=最大 mip w=保留
    glm::vec4 screen{0.0f};  // x=宽 y=高 z=1/宽 w=1/高
    glm::uvec4 meta{0u};     // x=每批命令占的 uint 数(=5)
};
static_assert(sizeof(MdiPush) == 112, "mdi_compact.comp 的 push 常量布局变了");

constexpr uint32_t kUintsPerCommand = 5;  // VkDrawIndexedIndirectCommand 是 5 个 uint32

} // namespace

MdiDraw::MdiDraw(rhi::Device& device, rhi::CommandPool& cmdPool,
                 uint32_t framesInFlight)
    : m_device(device), m_cmdPool(cmdPool),
      m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1) {
    createPipeline();

    // 采样器：Hi-Z 是逐 mip 显式 textureLod 采样（NEAREST），与
    // HiZOcclusion 内部那一个参数完全一致 —— 过滤/C 跨 mip 混合都会
    // 破坏"取区域最远深度"的保守性。
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f;
    si.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(m_device.get(), &si, nullptr, &m_sampler));

    m_frames.resize(m_framesInFlight);
    m_hizViews.assign(m_framesInFlight, VK_NULL_HANDLE);
    m_aabbBuffers.assign(m_framesInFlight, VK_NULL_HANDLE);

    const uint32_t n = m_framesInFlight;
    m_descriptors = std::make_unique<rhi::DescriptorAllocator>(
        m_device, n + 4, 0, n + 4, n * 5 + 8, 0);
    for (uint32_t i = 0; i < n; ++i)
        m_frames[i].set = m_descriptors->allocate(m_setLayout);

    // 缓冲一次分到底（见 instancesBuffer 的注释：永不换对象 → binding 15
    // 只需在 init / recreate 时写一次）。
    createBuffers();
    writeDescriptors();
}

void MdiDraw::createBuffers() {
    // kMaxObjects 个实例记录 + kMaxBatches 条命令：合计约 1.5 MB/槽，
    // 完全不值得为它做动态扩容那一套（换 buffer → 重写描述符 → 还得
    // 保证 set 0 那边同步，是这条路径最容易出错的地方）。
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (auto& f : m_frames) {
        f.catalog = std::make_unique<rhi::Buffer>(
            m_device, static_cast<VkDeviceSize>(kMaxObjects) *
                          sizeof(MdiInstance),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
        f.catalogMapped = f.catalog->map();

        f.objBatch = std::make_unique<rhi::Buffer>(
            m_device, static_cast<VkDeviceSize>(kMaxObjects) * sizeof(uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
        f.objBatchMapped = f.objBatch->map();

        // 紧凑化后的实例表：GPU 写；顶点着色器读它做实例取址（binding 15）。
        f.instances = std::make_unique<rhi::Buffer>(
            m_device, static_cast<VkDeviceSize>(kMaxObjects) *
                          sizeof(MdiInstance),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);

        // 间接命令缓冲：既当 SSBO（compute 里 atomicAdd）又当 indirect。
        f.indirect = std::make_unique<rhi::Buffer>(
            m_device, static_cast<VkDeviceSize>(kMaxBatches) *
                          sizeof(MdiDrawCommand),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            host);
        f.indirectMapped = f.indirect->map();
    }
}

MdiDraw::~MdiDraw() {
    m_device.waitIdle();
    m_pipeline.reset();
    destroyDescriptors();
    destroyFrames();

    VkDevice dev = m_device.get();
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
}

void MdiDraw::destroyDescriptors() { m_descriptors.reset(); }

void MdiDraw::createPipeline() {
    // 6 个 binding：Hi-Z 采样图 + AABB / catalog / objBatch / instances /
    // indirect 五个 SSBO。
    std::vector<VkDescriptorSetLayoutBinding> b(6);
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    for (uint32_t i = 1; i <= 5; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    m_setLayout = rhi::makeSetLayout(m_device, b);

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset = 0;
    pc.size = sizeof(MdiPush);

    m_pipeline = std::make_unique<rhi::ComputePipeline>(
        m_device, vkutil::readFile(resolveShaderPath("mdi_compact.comp.spv")),
        std::vector<VkDescriptorSetLayout>{m_setLayout}, &pc);

    VK_LOG_INFO("MdiDraw: mdi_compact.comp.spv 已就绪（storage image 0 / SSBO 5）");
}

void MdiDraw::setInputs(const std::vector<VkImageView>& hizViews,
                        const std::vector<VkBuffer>& aabbBuffers) {
    const size_t n = m_frames.size();
    m_hizViews.assign(n, VK_NULL_HANDLE);
    m_aabbBuffers.assign(n, VK_NULL_HANDLE);
    for (size_t i = 0; i < n && i < hizViews.size(); ++i)
        m_hizViews[i] = hizViews[i];
    for (size_t i = 0; i < n && i < aabbBuffers.size(); ++i)
        m_aabbBuffers[i] = aabbBuffers[i];

    bool all = n > 0;
    for (size_t i = 0; i < n; ++i) {
        if (m_hizViews[i] == VK_NULL_HANDLE ||
            m_aabbBuffers[i] == VK_NULL_HANDLE) {
            all = false;
        }
    }
    m_ready = all;
    if (m_descriptors) writeDescriptors();
    VK_LOG_INFO("MdiDraw: inputs %s (%u slots)",
                all ? "ready" : "incomplete", static_cast<unsigned>(n));
}

void MdiDraw::writeDescriptors() {
    if (!m_descriptors) return;
    for (size_t i = 0; i < m_frames.size(); ++i) {
        auto& f = m_frames[i];
        if (f.set == VK_NULL_HANDLE) continue;

        // binding 0/1：Hi-Z 视图与候选 AABB 缓冲（两者属于 HiZOcclusion，
        // 由 setInputs 在重建后喂进来；还没喂就先跳过）。
        if (i < m_hizViews.size() && m_hizViews[i] != VK_NULL_HANDLE) {
            m_descriptors->writeTexture(f.set, 0, m_sampler, m_hizViews[i],
                                        VK_IMAGE_LAYOUT_GENERAL);
        }
        if (i < m_aabbBuffers.size() && m_aabbBuffers[i] != VK_NULL_HANDLE) {
            m_descriptors->writeBuffer(
                f.set, 1, m_aabbBuffers[i],
                static_cast<VkDeviceSize>(kMaxObjects) * sizeof(HiZAabb),
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        }

        // binding 2..5：本模块自己的缓冲
        if (!f.catalog) continue;
        m_descriptors->writeBuffer(
            f.set, 2, f.catalog->get(),
            static_cast<VkDeviceSize>(kMaxObjects) * sizeof(MdiInstance),
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        m_descriptors->writeBuffer(
            f.set, 3, f.objBatch->get(),
            static_cast<VkDeviceSize>(kMaxObjects) * sizeof(uint32_t),
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        m_descriptors->writeBuffer(
            f.set, 4, f.instances->get(),
            static_cast<VkDeviceSize>(kMaxObjects) * sizeof(MdiInstance),
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        m_descriptors->writeBuffer(
            f.set, 5, f.indirect->get(),
            static_cast<VkDeviceSize>(kMaxBatches) * sizeof(MdiDrawCommand),
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
}

void MdiDraw::destroyFrames() {
    for (auto& f : m_frames) {
        f.catalogMapped = nullptr;
        f.objBatchMapped = nullptr;
        f.indirectMapped = nullptr;
        f.catalog.reset();
        f.objBatch.reset();
        f.instances.reset();
        f.indirect.reset();
        f.set = VK_NULL_HANDLE;
        f.commandCount = 0;
        f.lastObjectCount = 0;
    }
    m_frames.clear();
}

// ---------------------------------------------------------------- 每帧

bool MdiDraw::beginFrame(uint32_t frameIndex,
                         const std::vector<MdiInstance>& catalog,
                         const std::vector<uint32_t>& objBatch,
                         const std::vector<MdiDrawCommand>& commands,
                         const std::vector<MdiSubmitGroup>& groups) {
    m_groups.clear();
    m_objectCount = 0;
    if (m_frames.empty()) return false;

    // ⚠ catalog / objBatch 是**每物体**一条（长度 = 候选数 n），
    //   而 commands 是**每批**一条（长度 = 批数）—— 两者长度不同！
    const uint32_t n = static_cast<uint32_t>(catalog.size());
    if (n == 0 || n != objBatch.size() || commands.empty()) return false;
    // 容量是构造时一次分到底的固定上限，超了只能退回旧路径（不是扩容，
    // 而是本帧不启用 MDI —— 换 buffer 会牵动 set 0 的 binding 15）。
    if (n > kMaxObjects || commands.size() > kMaxBatches) return false;
    if (!m_ready) return false;

    auto& f = m_frames[frameIndex % m_frames.size()];
    if (!f.catalog || !f.indirect) return false;

    std::memcpy(f.catalogMapped, catalog.data(),
                static_cast<size_t>(n) * sizeof(MdiInstance));
    std::memcpy(f.objBatchMapped, objBatch.data(),
                static_cast<size_t>(n) * sizeof(uint32_t));

    // 命令：静态字段整条拷进去，但 instanceCount 必须清 0 —— 它是
    // mdi_compact.comp 里 atomicAdd 的累加起点，也是"本帧要画几个"的答案。
    // ⚠ 必须留 0：上次的值（本槽两帧前写的）不清掉的话，计数会一路涨。
    auto* dst = static_cast<MdiDrawCommand*>(f.indirectMapped);
    const size_t cmdCount = commands.size();
    for (size_t i = 0; i < cmdCount; ++i) {
        dst[i] = commands[i];
        dst[i].instanceCount = 0;
    }
    f.commandCount = static_cast<uint32_t>(cmdCount);
    f.lastObjectCount = n;

    m_groups = groups;
    m_objectCount = n;
    m_stats.candidates = n;
    m_stats.batches = static_cast<uint32_t>(cmdCount);
    m_stats.submissions = static_cast<uint32_t>(groups.size());
    return true;
}

void MdiDraw::readBack(uint32_t frameIndex) {
    if (m_frames.empty()) return;
    auto& f = m_frames[frameIndex % m_frames.size()];
    if (!f.indirectMapped || f.commandCount == 0) {
        m_stats.instancesDrawn = 0;
        m_stats.culled = 0;
        return;
    }

    // 调用点在等待该槽 in-flight 栅栏之后 → GPU 已不再访问这个缓冲。
    // 读到的计数是**两帧前**那次 compact 留下的（本槽每帧只被写一次）。
    const auto* cmds = static_cast<const uint32_t*>(f.indirectMapped);
    uint32_t drawn = 0;
    for (uint32_t i = 0; i < f.commandCount; ++i)
        drawn += cmds[i * kUintsPerCommand + 1u];  // instanceCount

    m_stats.instancesDrawn = drawn;
    m_stats.culled = f.lastObjectCount > drawn ? f.lastObjectCount - drawn : 0;
}

void MdiDraw::compact(VkCommandBuffer cmd, uint32_t frameIndex,
                      const glm::mat4& viewProj, float depthBias, float maxLod,
                      VkExtent2D extent, uint32_t objectCount) {
    if (!m_ready || !m_pipeline || m_frames.empty() || objectCount == 0)
        return;
    auto& f = m_frames[frameIndex % m_frames.size()];
    if (f.set == VK_NULL_HANDLE || !f.indirect) return;
    if (objectCount > kMaxObjects) objectCount = kMaxObjects;
    if (objectCount == 0) return;

    MdiPush pc{};
    pc.viewProj = viewProj;
    pc.params = glm::vec4(static_cast<float>(objectCount), depthBias, maxLod,
                          0.0f);
    pc.screen = glm::vec4(static_cast<float>(extent.width),
                          static_cast<float>(extent.height),
                          1.0f / static_cast<float>(extent.width),
                          1.0f / static_cast<float>(extent.height));
    pc.meta = glm::uvec4(kUintsPerCommand, 0u, 0u, 0u);

    const uint32_t groups = (objectCount + 63u) / 64u;
    m_pipeline->dispatch(cmd, groups, 1, 1, &f.set, 1, &pc, sizeof(pc));

    // 紧凑化的产物要被三处读：
    //   · instances 被顶点着色器取址读（VERTEX_SHADER）
    //   · indirect 被 vkCmdDrawIndexedIndirect 读（DRAW_INDIRECT）
    //   · 计数被下一轮的主机回读读（HOST）
    // 一条屏障同时覆盖两个 buffer 的三个用途。
    VkBufferMemoryBarrier bb[2]{};
    const VkBuffer bufs[2] = {f.instances->get(), f.indirect->get()};
    for (int i = 0; i < 2; ++i) {
        bb[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bb[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                              VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                              VK_ACCESS_HOST_READ_BIT;
        bb[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb[i].buffer = bufs[i];
        bb[i].offset = 0;
        bb[i].size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                             VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 2, bb, 0, nullptr);
}

VkBuffer MdiDraw::indirectBuffer(uint32_t frameIndex) const {
    if (m_frames.empty()) return VK_NULL_HANDLE;
    const auto& f = m_frames[frameIndex % m_frames.size()];
    return f.indirect ? f.indirect->get() : VK_NULL_HANDLE;
}

VkBuffer MdiDraw::instancesBuffer(uint32_t frameIndex) const {
    if (m_frames.empty()) return VK_NULL_HANDLE;
    const auto& f = m_frames[frameIndex % m_frames.size()];
    return f.instances ? f.instances->get() : VK_NULL_HANDLE;
}

} // namespace render
