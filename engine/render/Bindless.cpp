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

// 页表用的是 std430 的 struct 数组（stride = sizeof(GpuMaterialPage)，
// 目前 112 = 7 个 align-16 的块），与 GpuMaterialPage 的 C++ 布局逐字节
// 一致 —— 直接 memcpy。
// ⚠ std430 对"结构体数组"的 stride 规则是**向上取整到最大成员对齐**
//   （这里是 16），不是 C++ 的 sizeof。每块都是 16 B，所以两边一致；
//   真正的坑是混入 vec2/float 造成的尾部填充 ——
//   所以这个结构体里**只允许 vec4 / uvec4**。
//   （10-26 A3 从 6 块扩到 7 块，历史：32 → 96 → 112。）
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
    // z = 高度图（R4-3 POM）。缺省槽位用**纯黑**而不是白：白 = 处处等高
    // → 视差射线会一路走到层数上限（退化成一个大偏移）；黑 = 处处最低
    // → 循环立刻停下 → offset 0（"没挂高度图"的中性行为）。
    // w = Toon 的明暗渐变带（A 类，2026-10-25）。缺省**纯白**：着色器里
    // 是"程序化 ramp × 贴图值"，白 = ×1 = 与没有这个槽逐像素一致。
    // 这三个新槽（ramp / matcap / sdfShadow / detail）的缺省值必须都是
    // **乘法恒等**的白，这是"关 = 基线"在页表这一层的可断言形式 ——
    // 只要缺省值是白，没挂贴图就不可能改变任何像素。
    rec.tex1 = glm::uvec4(tex(mat.metallicMap, m_defaultWhite),
                          tex(mat.emissiveMap, m_defaultBlack),
                          tex(mat.heightMap, m_defaultBlack),
                          tex(mat.rampMap, m_defaultWhite));
    // A 类语义槽：同样全部兜底白。
    //   matcap  —— 球面高光贴图，强度 0 时整项不参与
    //   sdfShadow —— 面部阴影图，"全白" = 处处受光 = 没有 SDF 阴影
    //   detail  —— 细节/贴花，白 = 不调制 albedo
    rec.tex2 = glm::uvec4(tex(mat.matcapMap, m_defaultWhite),
                          tex(mat.sdfShadowMap, m_defaultWhite),
                          tex(mat.detailMap, m_defaultWhite), 0u);

    // 逐材质标量。rampMode 的三态是**着色器唯一能区分"有没有挂 ramp"的办法**
    // （页表里空槽绑的是白图，着色器没法从下标反推"这张是不是缺省白"）：
    //     -1 = 没挂 → 走程序化两档量化（**逐像素等于引入这个槽之前**）
    //      0 = 挂了，纯渐变（按 NdotL 直接采样）
    //     >0 = 挂了，量化成这么多档（采样点落在色带中心）
    // 少了 -1 这一档，没挂 ramp 的材质会拿到"全白 × band"= 处处亮面，
    // 整场景的 Toon 会一起变成一坨平光 —— 而且看起来像"打光错了"。
    const float rampMode =
        mat.rampMap ? std::max(mat.rampSteps, 0.0f) : -1.0f;
    // sdfShadowStrength 的闸门（A1，2026-10-26）：
    // **没挂 SDF 图 → 写 0**，而不是"写强度、靠白图 ×1 兜底"。
    // 理由与 rampMode 的 -1 完全相同：着色器分不清"这张是不是缺省白"，
    // 一次次采样白图 = 纯浪费带宽；而 0 让 faceSdfMask 直接 return 1.0。
    // 顺带修掉一个旧隐患：这个字段的默认值是 1.0，若哪天有人把 SDF 槽
    // 接上缺省白以外的图，旧写法会让所有没配 SDF 的材质跟着变。
    const float sdfStrength = mat.sdfShadowMap ? mat.sdfShadowStrength : 0.0f;
    rec.params = glm::vec4(rampMode, mat.matcapStrength, sdfStrength,
                           mat.detailStrength);
    // params2.z/w（A6）：色带的明暗交界位置与宽度。着色器的 toonGate 读它，
    // 与资产烘进纹理的台阶必须是同一组数 —— 这里直接取材质字段，材质字段
    // 又在 Inspector 编辑时与资产同步写，所以不存在两套真值。
    rec.params2 = glm::vec4(mat.detailTiling, mat.matcapRotate,
                            mat.rampEdge, mat.rampSoftness);
    // spare（A5/A1/A4）：x = 材质级角色光开关，y = 面部 SDF 采样偏移强度，
    // z = 头发各向异性强度，w = 各向异性环的位置偏移。
    // 用 vec4 而不是 uvec4 是为了不用把 float 按位塞进 uint —— 布局都是
    // 16 B，改类型不影响 std430 偏移（C++ 端已同步改，见 Bindless.h）。
    // ⚠ z 的 0 是**硬闸门**：pbr.frag 的 hairAnisoSpec 第一行就
    //   `if (strength <= 0) return 0`，所以没开各向异性的材质连切线都
    //   不算 —— 这是"关 = 基线"逐像素成立的落点，不要改成"算了再乘 0"。
    rec.spare = glm::vec4(mat.useCharacterLight ? 1.0f : 0.0f,
                          mat.sdfFaceShift, mat.anisoStrength,
                          mat.anisoShift);
    // spare2（A3，2026-10-26）：眼睛的三个旋钮。
    // ⚠ x（eyeStrength）的 0 同样是**硬闸门**：pbr.frag 里两条 eye 分支
    //   都以 `> 0` 开头，没配眼睛的材质不做虹膜视差、也不进 unlit 混合 ——
    //   "关 = 基线逐像素"靠的就是这个分支，不是"算出 eye 色再 mix 到 0"
    //   （后者在 float 精度边缘仍会改动最低位，且虹膜视差会白算一次
    //   cotangentFrame）。
    rec.spare2 = glm::vec4(mat.eyeStrength, mat.eyeIrisDepth,
                           mat.eyeCatchlight, 0.0f);
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
