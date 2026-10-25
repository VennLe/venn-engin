#pragma once
// ============================================================
// render/Bindless —— 无绑定资源（R2-3，docs/render_roadmap.md）
//
// 解决的问题：过去"每个材质占 6 个 set"（set 1..6 = albedo/normal/orm/
// roughness/metallic/emissive 各一个 sampler2D）。一个 draw 要绑 6 个
// 描述符集，而 set 1..6 又恰好把"Vulkan 只保证至少 4 个 set 可同时绑定"
// 这条余量吃光 —— 想再加任何资源都只能往 set 0 里挤（7..15 就是这么来的）。
//
// 现在：set 1 只有两个 binding
//   binding 0 = `sampler2D textures[]`   运行时大小数组（PARTIALLY_BOUND
//                                          + UPDATE_AFTER_BIND）
//   binding 1 = `GpuMaterialPage pages[]` 材质"页表"（SSBO）
// per-draw 只 push 一个**页号**（挤在 push 常量的空闲位 emissive.w 里），
// 片元着色器据此查出 6 个纹理下标再索引数组：
//
//     vec4 c = texture(textures[pages[page].tex0.x], uv);   // albedo
//
// 于是材质数量不再影响"绑几次 set"，纹理数量也不再受 set 拆分限制 ——
// 这就是 R7-3 ray tracing 要的那种资源绑定模型。
//
// ------------------------------------------------------------
// 两个刻意的设计决定
//
// ① **纹理下标是稳定的、惰性分配的**：`textureIndex(tex)` 第一次见到某张
//    纹理时分配一个槽并写描述符，之后同指针永远返回同一个下标（重复注册
//    是 O(1) 的哈希查找）。index 一旦分配永不回收 —— 场景里纹理数量本来
//    就远小于上限，回收带来的"重用槽位被 GPU 读着"问题不值得。
//    纹理流的收尾会**重建 sampler**（放开 maxLod），所以每次查表都比一下
//    记录里的 sampler/view 句柄，变了就重写该槽 —— 这是"运行时改纹理"
//    唯一需要走的路。池是 UPDATE_AFTER_BIND 的，在途帧不受影响。
//
// ② **材质页是每帧惰性分配的**：页表缓冲按帧槽分离（MAX_FRAMES_IN_FLIGHT
//    份），beginFrame() 把该槽的页计数清零。用同一 slot 的上一帧早已被
//    栅栏保证跑完，所以"重写整个页表"不会撕裂任何在途读取。
//    页号与材质的关系是**帧内**确定、跨帧不保证相同 —— 因此页号必须
//    per-draw push，不能缓存在材质对象上。
// ============================================================

#include "rhi/VulkanCommon.h"

#include <glm/glm.hpp>

#include <cstddef>   // offsetof（材质页的布局断言）
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rhi {
class Device;
class Buffer;
class DescriptorAllocator;
} // namespace rhi

namespace assets {
class Texture;
struct Material;
} // namespace assets

namespace render {

// ============================================================
// GPU 材质页 —— 与 assets/shaders/pbr.frag 的 GpuMaterialPage 逐字段一致
//
// 只放"纹理槽 → bindless 下标"，**不放材质参数**：baseColorFactor /
// metallic / roughness / normalScale / ao / emissive 仍在 push 常量里
// （见 render/RenderTypes.h 的 PushConstants）。这样从旧路径切过来时，
// 所有参与 PBR 计算的浮点数还是那同一份值 —— "开 = 关"的逐像素对照
// 才可能是严格相等的。
//
// ------------------------------------------------------------
// 32 B → 96 B（A 类槽位，2026-10-25）
//
// **为什么必须扩容、而不是再加一张贴图到 set 里**：三条 GPU 通道在
// 32B 时代就已经全部占满了 ——
//   · PushConstants 恰好 128 B（Vulkan 保证的下限），唯一空位
//     `emissive.w` 已被页号占用；
//   · 纹理 set 用满 7 个，加上 set0 正好等于 maxBoundDescriptorSets
//     的规范下限 8；
//   · 非 bindless 路径在结构上已经封死。
// 所以二次元的四个语义槽（ramp / matcap / SDF 阴影 / 贴花）只能进页表。
//
// **为什么一次到 96 B 而不是先加 16 B**：页表格式一变，`pbr.frag` 的
// std430 结构、C++ 结构、以及任何未来的读端都要同步；分批加就是分批
// 改同一处。审计文档已经算过账：96 B × 4096 页 × 2 槽 = 768 KB，
// 对显存不是问题，对"改一次改对"是。
//
// **语义槽而不是通用层栈**：这里刻意不做"任意层数的材质层栈"。
// 层栈要每层一个变换/混合模式/UV 集，页表会变成不定长结构 + 二级
// 索引，而二次元真正需要的只有**固定的那几件**。固定语义槽的另一个
// 好处是着色器里可以直接写 `#define matcapMap textures[...]`。
// ============================================================
struct GpuMaterialPage {
    // ---- 前 32 B：与 32 B 时代**逐字节相同** ----
    // 语义不变是刻意的：非 bindless 路径、老资产、以及"关 = 基线"的
    // 逐像素对照都依赖这段布局没有动过。
    glm::uvec4 tex0{0};  // 0  x=albedo y=normal z=ORM w=roughness
    glm::uvec4 tex1{0};  // 16 x=metallic y=emissive z=height(R4-3 POM)
                         //    w=ramp（原来是空位，A 类第一个落点）

    // ---- 32 B ~ 48 B：A 类新增的三个语义槽 ----
    glm::uvec4 tex2{0};  // 32 x=matcap y=sdfShadow z=detail w=(预留)

    // ---- 48 B ~ 96 B：逐材质标量 ----
    // push 常量已经满了，所以**这是全引擎唯一能放逐材质浮点的地方**。
    // 它们不能走关键字变体：ramp 档数/强度是美术每帧都要调的连续量。
    glm::vec4 params{0};   // 48 x=rampSteps y=matcapStrength
                           //    z=sdfShadowStrength w=detailStrength
    glm::vec4 params2{0};  // 64 x=detailTiling y=matcapRotate z/w=(预留)
    // 80 B ~ 96 B：整块预留，给下一刀（形态键权重基址 / 分层遮罩）。
    // 留白是**在页表格式上**留的：真的到了那天只要往里填，不必再动
    // std430 结构、不必再改一次 pbr.frag 的声明。
    glm::uvec4 spare{0};  // 80
};
static_assert(sizeof(GpuMaterialPage) == 96,
              "GpuMaterialPage 必须与 pbr.frag 的 std430 布局一致（6 × 16B）");
static_assert(offsetof(GpuMaterialPage, tex1) == 16 &&
                  offsetof(GpuMaterialPage, tex2) == 32 &&
                  offsetof(GpuMaterialPage, params) == 48 &&
                  offsetof(GpuMaterialPage, params2) == 64 &&
                  offsetof(GpuMaterialPage, spare) == 80,
              "偏移必须逐 16B 对齐 —— std430 的 vec4 对齐规则，"
              "差 4 字节就是采样到别的贴图（不报错的那种错）");

struct BindlessStats {
    uint32_t textures = 0;        // 已注册纹理数（本帧累计，只增不减）
    uint32_t maxTextures = 0;     // 数组容量（设备相关裁剪后）
    uint32_t framePages = 0;      // 本帧分配出去的材质页数
    uint32_t maxFramePages = 0;   // 本帧历史峰值（观察是否贴近上限）
    uint32_t pageCapacity = 0;    // 每帧槽的页表容量
    uint32_t boundSets = 0;       // 本帧录制的 set 1 绑定次数
};

class BindlessTextures {
public:
    // 数组容量上限。真正生效的值取 min(这个, 设备 maxPerStageDescriptor*Samplers)
    // —— 少数驱动（尤其集显）这个上限只有 128/256，硬写 4096 会让
    // vkCreateDescriptorSetLayout 直接崩。查到的容量写进 stats().maxTextures。
    static constexpr uint32_t kMaxTextures = 4096;
    // 每帧槽的材质页容量。4096 页 × 96 B × 2 槽 = 768 KB，一次分到底。
    // 页表是 HOST_VISIBLE + 常驻映射的，扩容只影响这块缓冲的大小，
    // 不影响分配/上传路径（见 Bindless.cpp 的 materialPage）。
    static constexpr uint32_t kMaxMaterialPages = 4096;

    BindlessTextures() = default;
    ~BindlessTextures();

    BindlessTextures(const BindlessTextures&) = delete;
    BindlessTextures& operator=(const BindlessTextures&) = delete;

    // framesInFlight = MAX_FRAMES_IN_FLIGHT（页表缓冲与描述符集各一份）
    bool init(rhi::Device& device, uint32_t framesInFlight);

    // 缺省贴图的下标（白 / 平坦法线 / 黑）。必须在第一次 materialPage()
    // 之前设好 —— 材质槽为空时要拿它们兜底，否则会索引到未写入的槽
    // （PARTIALLY_BOUND 只是"合法"，不是"有内容"）。
    void setDefaults(uint32_t white, uint32_t flatNormal, uint32_t black) {
        m_defaultWhite = white;
        m_defaultFlatNormal = flatNormal;
        m_defaultBlack = black;
    }

    bool ready() const { return m_ready; }
    VkDescriptorSetLayout setLayout() const { return m_setLayout; }
    VkDescriptorSet set(uint32_t frame) const { return m_sets[frame]; }

    // 帧首：清本槽的材质页表计数（页表内容会被就地重写）。
    void beginFrame(uint32_t frame);

    // 惰性注册纹理，返回稳定的数组下标。同指针重复调用 = O(1) 命中。
    uint32_t textureIndex(const assets::Texture& tex);

    // 惰性分配本帧的材质页，返回页号（写进 push 常量）。
    uint32_t materialPage(const assets::Material& mat);

    // 把 set 1 绑上（每个 render pass 一次即可 —— 页表内容随 push 变化，
    // 不需要逐 draw 重绑）。
    void bind(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frame);

    const BindlessStats& stats() const { return m_stats; }

private:
    struct Slot {
        const assets::Texture* tex = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };

    void writeSlot(uint32_t index, VkImageView view, VkSampler sampler);

    rhi::Device* m_device = nullptr;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    std::unique_ptr<rhi::DescriptorAllocator> m_pool;
    std::vector<VkDescriptorSet> m_sets;
    // 每帧槽一份页表（HOST_VISIBLE + 常驻映射）
    std::vector<std::unique_ptr<rhi::Buffer>> m_pageTables;
    std::vector<GpuMaterialPage*> m_pageMapped;
    std::vector<uint32_t> m_pageCount;
    // 页号缓存：材质指针 → 本帧页号（beginFrame 清空）
    std::unordered_map<const assets::Material*, uint32_t> m_pageOf;
    // 纹理指针 → 数组下标（跨帧稳定）
    std::unordered_map<const assets::Texture*, uint32_t> m_indexOf;
    std::vector<Slot> m_slots;

    uint32_t m_capacity = 0;   // 实际数组容量（≤ kMaxTextures）
    uint32_t m_frame = 0;
    bool m_ready = false;

    uint32_t m_defaultWhite = 0;
    uint32_t m_defaultFlatNormal = 0;
    uint32_t m_defaultBlack = 0;

    BindlessStats m_stats;
};

} // namespace render
