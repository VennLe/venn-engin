// ============================================================
// render/Bindless —— 实现
// 设计说明见 Bindless.h
// ============================================================

#include "render/Bindless.h"

#include "assets/Material.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "rhi/Buffer.h"
#include "rhi/DescriptorSet.h"
#include "rhi/Device.h"

#include <algorithm>
#include <cstring>

namespace render {

namespace {

// 页表用的是 std430 的 struct 数组（stride = 32，两个 uvec4 都是
// align-16），与 GpuMaterialPage 的 C++ 布局逐字节一致 —— 直接 memcpy。
constexpr VkDeviceSize kPageTableBytes =
    static_cast<VkDeviceSize>(BindlessTextures::kMaxMaterialPages) *
    sizeof(GpuMaterialPage);

} // namespace

BindlessTextures::~BindlessTextures() {
    // 页表缓冲先走（描述符集引用它），再拆池，最后拆 layout。
    m_pageMapped.clear();
    m_pageTables.clear();
    m_pool.reset();
    if (m_setLayout != VK_NULL_HANDLE && m_device) {
        vkDestroyDescriptorSetLayout(m_device->get(), m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
}

bool BindlessTextures::init(rhi::Device& device, uint32_t framesInFlight) {
    m_device = &device;
    if (framesInFlight == 0) framesInFlight = 1;

    // 设备上限是**每阶段**的采样器数量。开了 descriptor indexing 的现代
    // 驱动上这个值很大（NVIDIA 1048576），集显上可能只有 128 —— 取值必须
    // 按设备收窄，否则 vkCreateDescriptorSetLayout 直接失败。
    // （update-after-bind 那套上限在 VkPhysicalDeviceVulkan12Properties 里，
    //   要再挂一条 pNext 才查得到；这里刻意只查核心 limits：它是更保守的
    //   那个，而容量本来就不是瓶颈。PARTIALLY_BOUND 之后上限严格说不被强制，
    //   收窄只是省内存 + 避开驱动差异。）
    const VkPhysicalDeviceLimits& lim = device.properties().limits;
    const uint32_t cap = lim.maxPerStageDescriptorSamplers;
    m_capacity = (cap == 0) ? 0 : (cap < kMaxTextures ? cap : kMaxTextures);
    if (m_capacity < 64) {
        VK_LOG_ERROR("Bindless: device sampler-array limit too small (%u)",
                     m_capacity);
        return false;
    }

    // ---- set 1 layout ----
    // binding 0：运行时大小数组（着色器写 `sampler2D textures[]`）。
    //   · PARTIALLY_BOUND       —— 未注册到的槽可以留着不写（不是"必须
    //                              全填"，否则每帧要往几千个槽灌缺省图）
    //   · UPDATE_AFTER_BIND     —— 允许在队列还在用这个 set 的时候补写
    //                              （运行时加载模型/流式纹理必需）
    // binding 1：材质页表（每帧全量重写内容，但**缓冲对象不变**，
    //   所以这个 binding 不需要 UPDATE_AFTER_BIND）。
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = m_capacity;
    // FRAGMENT only：顶点阶段从来不采样材质贴图（连 pbr.vert 都不声明 set 1）。
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    const VkDescriptorBindingFlags flags[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        0u,
    };
    m_setLayout = rhi::makeSetLayout(
        device,
        std::vector<VkDescriptorSetLayoutBinding>(bindings, bindings + 2),
        std::vector<VkDescriptorBindingFlags>(flags, flags + 2),
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);

    // ---- 池 ----
    // 数组太大，不可能和主池（256 个 sampler）共用：这里按
    // 容量 × set 数预留。一个 set 就是 4096 个 COMBINED_IMAGE_SAMPLER
    // 描述符，两个在途帧槽 → 8192 个，驱动侧只是描述符表，几十 KB 而已。
    m_pool = std::make_unique<rhi::DescriptorAllocator>(
        device, framesInFlight, 0,
        m_capacity * framesInFlight, framesInFlight, 0,
        VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT);

    // ---- 每帧槽：一个 set + 一份页表 ----
    m_sets.resize(framesInFlight);
    m_pageTables.resize(framesInFlight);
    m_pageMapped.resize(framesInFlight);
    m_pageCount.assign(framesInFlight, 0);
    for (uint32_t i = 0; i < framesInFlight; ++i) {
        m_sets[i] = m_pool->allocate(m_setLayout);

        m_pageTables[i] = std::make_unique<rhi::Buffer>(
            device, kPageTableBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        auto* mapped = static_cast<GpuMaterialPage*>(m_pageTables[i]->map());
        // 用赋值而不是 memset：GpuMaterialPage 有非平凡的默认构造
        // （glm::uvec4），memset 会被 -Wclass-memaccess 挡下来。
        // 页表是"按需写 + 惰性读"，清零只是让"读到未分配的页"不至于
        // 索引到随机的纹理下标（真发生的话会采样到错误贴图而不报错）。
        std::fill(mapped, mapped + kMaxMaterialPages, GpuMaterialPage{});
        m_pageMapped[i] = mapped;

        m_pool->writeBuffer(m_sets[i], 1, m_pageTables[i]->get(),
                            kPageTableBytes, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }

    m_stats.maxTextures = m_capacity;
    m_stats.pageCapacity = kMaxMaterialPages;
    m_ready = true;
    VK_LOG_INFO("Bindless: ready (textures<=%u, pages/frame=%u, slots=%u)",
                m_capacity, kMaxMaterialPages, framesInFlight);
    return true;
}

void BindlessTextures::beginFrame(uint32_t frame) {
    if (!m_ready) return;
    m_frame = (frame < m_pageCount.size()) ? frame : 0;
    m_pageCount[m_frame] = 0;
    m_pageOf.clear();
    m_stats.framePages = 0;
}

uint32_t BindlessTextures::textureIndex(const assets::Texture& tex) {
    if (!m_ready) return 0;

    const VkImageView view = tex.view();
    const VkSampler sampler = tex.sampler();

    const auto it = m_indexOf.find(&tex);
    if (it != m_indexOf.end()) {
        const uint32_t idx = it->second;
        Slot& slot = m_slots[idx];
        // 纹理流的收尾会重建 sampler（放开 maxLod），重载也可能换掉 image。
        // 句柄一变就重写该槽 —— 池是 UPDATE_AFTER_BIND 的，在途帧安全。
        if (slot.sampler != sampler || slot.view != view) {
            slot.sampler = sampler;
            slot.view = view;
            writeSlot(idx, view, sampler);
        }
        return idx;
    }

    if (m_slots.size() >= m_capacity) {
        // 溢出：退回缺省白图。绝不返回越界下标 —— 那会在着色器里
        // 索引到未写入的槽（PARTIALLY_BOUND 的"合法"只是不报验证错，
        // 采样结果是未定义的）。
        static bool warned = false;
        if (!warned) {
            warned = true;
            VK_LOG_WARN("Bindless: texture array full (%u), extra textures "
                        "fall back to slot 0",
                        m_capacity);
        }
        return m_defaultWhite;
    }

    const uint32_t idx = static_cast<uint32_t>(m_slots.size());
    m_slots.push_back({&tex, view, sampler});
    m_indexOf.emplace(&tex, idx);
    writeSlot(idx, view, sampler);
    m_stats.textures = static_cast<uint32_t>(m_slots.size());
    return idx;
}

void BindlessTextures::writeSlot(uint32_t index, VkImageView view,
                                 VkSampler sampler) {
    for (VkDescriptorSet set : m_sets) {
        m_pool->writeTextureAt(set, 0, index, sampler, view);
    }
}

uint32_t BindlessTextures::materialPage(const assets::Material& mat) {
    if (!m_ready) return 0;

    const auto it = m_pageOf.find(&mat);
    if (it != m_pageOf.end()) return it->second;

    if (m_pageCount[m_frame] >= kMaxMaterialPages) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            VK_LOG_WARN("Bindless: material page table full (%u), extra "
                        "materials share page 0",
                        kMaxMaterialPages);
        }
        m_pageOf.emplace(&mat, 0u);
        return 0;
    }

    const uint32_t page = m_pageCount[m_frame]++;
    const auto tex = [&](const assets::Texture* t, uint32_t fallback) {
        return t ? textureIndex(*t) : fallback;
    };

    GpuMaterialPage rec;
    rec.tex0 = glm::uvec4(tex(mat.albedoMap, m_defaultWhite),
                          tex(mat.normalMap, m_defaultFlatNormal),
                          tex(mat.ormMap, m_defaultWhite),
                          tex(mat.roughnessMap, m_defaultWhite));
    rec.tex1 = glm::uvec4(tex(mat.metallicMap, m_defaultWhite),
                          tex(mat.emissiveMap, m_defaultBlack), 0u, 0u);
    m_pageMapped[m_frame][page] = rec;

    m_pageOf.emplace(&mat, page);
    m_stats.framePages = m_pageCount[m_frame];
    if (m_stats.framePages > m_stats.maxFramePages) {
        m_stats.maxFramePages = m_stats.framePages;
    }
    return page;
}

void BindlessTextures::bind(VkCommandBuffer cmd, VkPipelineLayout layout,
                            uint32_t frame) {
    if (!m_ready) return;
    const uint32_t f = (frame < m_sets.size()) ? frame : 0;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                            &m_sets[f], 0, nullptr);
    ++m_stats.boundSets;
}

} // namespace render
