#pragma once
// ============================================================
// render/VariantPipelines —— 按关键字掩码缓存的管线集合
//
// 一个 Pass 里"同一份描述符布局、同一套顶点布局，只有关键字不同"的
// 那一族管线，全部由这一个对象管：掩码进去、管线出来，第一次用到才
// 惰性创建，之后命中缓存。
//
// 为什么惰性而不是 init 时把 2^N 份全建出来：
//   · 全建要 8 次 vkCreateGraphicsPipelines（含 SPIR-V 解析与驱动编译），
//     而一个场景通常只用得到 2~3 种组合；
//   · 更多关键字（4 位 → 16 份）时这个差距会继续放大。
//   代价只是"首次用到多一次创建"，完全可以接受。
//   注意惰性只针对**管线**：那份唯一的 VkPipelineLayout 是 init 时就建的
//   （它只有一个，且 set 0 必须在第一个 draw 之前绑好）。
//
// 为什么所有变体必须共用同一个 VkPipelineLayout：
//   set 0 是整个 Pass 一次性绑定的。中途绑一条 layout 不兼容的管线，
//   Vulkan 就认为 set 0 被"打断"，后面所有 draw 报 VUID-...-08600
//   （栅格/天空管线为这个坑专门对齐过推常量范围，见 Renderer::createPipelines）。
//   这里更进一步：init() 只建**一份** VkPipelineLayout，所有变体通过
//   PipelineDesc::sharedLayout 复用它 —— "兄弟管线 layout 不同"这件事在
//   结构上就不可能发生，不必靠"从同一份 baseDesc 派生"这种约定来保证。
// ============================================================

#include "rhi/Pipeline.h"
#include "rhi/VulkanCommon.h"
#include "render/ShaderKeywords.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace rhi {
class Device;
}

namespace render {

// 变体从哪来
enum class ShaderPermutation {
    // 每个掩码一份 .spv：fragBaseName="pbr.frag" → 读 "pbr.k<id>.frag.spv"。
    // 关键字真的改代码时用这个（NORMAL_MAP / DOUBLE_SIDED）。
    PerMask,
    // 所有掩码共用一份 .spv：fragBaseName 直接给完整文件名。
    // 关键字只改**管线固定状态**、不改代码时用这个 —— 比如深度预通道，
    // 它只有 DOUBLE_SIDED 关不关剔除的差别，为它多编 8 份是纯浪费。
    SingleSpv,
};

// ------------------------------------------------------------
// 关键字 → 管线固定状态
//
// ⚠ 与 GLSL 里的 #if 是**一体两面**，必须一起改：
//   DOUBLE_SIDED 既要在片元里按 gl_FrontFacing 翻转法线，也要在这里把
//   cullMode 关掉。只做一半就会出现"背面画出来了、但法线是反的"（或者
//   反过来：法线翻转逻辑写了，背面根本没被画出来）。
//   ALPHA_BLEND 既要不写深度，也要在片元里输出真的 alpha。
//
// stateMask 之外的位一律忽略：深度预通道只关心 DOUBLE_SIDED，
// 它不输出颜色，ALPHA_BLEND 对它是无意义的（不该影响它的缓冲对齐）。
// ------------------------------------------------------------
void applyKeywordState(KeywordMask m, KeywordMask stateMask,
                       rhi::PipelineDesc& desc);

class VariantPipelines {
public:
    VariantPipelines() = default;
    ~VariantPipelines() = default;
    VariantPipelines(const VariantPipelines&) = delete;
    VariantPipelines& operator=(const VariantPipelines&) = delete;

    // baseDesc：基线（掩码 = 0）的完整描述。顶点布局 / set 布局 / 推常量 /
    // RenderPass / 采样数都从它来，各变体只在固定状态与 .spv 上不同。
    // stateMask：哪些关键字位参与推导固定状态（见 applyKeywordState）。
    void init(rhi::Device& device, const rhi::PipelineDesc& baseDesc,
              const std::string& fragBaseName, ShaderPermutation permutation,
              KeywordMask stateMask);

    // 取（必要时创建）。返回引用在 clear() 或对象析构前一直有效。
    const rhi::Pipeline& get(KeywordMask m);

    // 变体族**共享**的管线布局 —— 绑 set 0 要用它，而 set 0 在第一个
    // draw 之前就要绑好，那时还不知道会用哪个变体。init() 时建好、
    // 全体变体复用，因此这个调用**不创建任何管线**。
    //
    // （早期版本这里返回 get(0).layout()，"取布局"顺手就把基线变体建了
    //   出来：强制掩码实验里明明只该有 1 条管线，日志却报 2 条 ——
    //   verify_shader_variants.py 第 2/3 组就是这么把它揪出来的。）
    VkPipelineLayout layout() const { return m_layout; }

    // init() 是否已经调过。管线是惰性创建的，没有"还没建"这个状态，
    // 所以调用方要判"这套东西准备好了没有"，只能看这个 ——
    // 原来的写法（判第一条管线指针是否为空）在惰性创建下永远为真。
    bool initialized() const { return m_device != nullptr; }

    // 已创建的变体数。验证脚本靠它判断"缓存真的生效了"：
    // 一个用 3 种组合的场景，跑很多帧之后这个数应当停在 3 不动。
    uint32_t created() const {
        return static_cast<uint32_t>(m_pipelines.size());
    }
    // 本帧的取用次数 / 其中真的命中缓存的次数（统计用）
    uint32_t lookups() const { return m_lookups; }
    uint32_t hits() const { return m_hits; }

    // 建了哪些变体（日志用）：返回 "k0=[none] k3=[double-sided|alpha-blend]" 之类
    std::string createdList() const;

    void clear();

private:
    rhi::Device* m_device = nullptr;
    rhi::PipelineDesc m_base;
    std::string m_fragBaseName;
    ShaderPermutation m_permutation = ShaderPermutation::PerMask;
    // 真正区分变体的位 = 选 .spv 的位 ∪ 改固定状态的位。
    // 不在这个集合里的位一律归零后再做缓存键 —— 否则深度预通道会为
    // "只有 NORMAL_MAP 不同"的掩码各建一条一模一样的管线。
    KeywordMask m_keyMask = kKeywordNone;
    KeywordMask m_spvMask = kKeywordNone;
    KeywordMask m_stateMask = kKeywordNone;

    std::unordered_map<KeywordMask, std::unique_ptr<rhi::Pipeline>> m_pipelines;
    // 全体变体共用的那份布局（本对象拥有，clear() 时销毁）
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    uint32_t m_lookups = 0;
    uint32_t m_hits = 0;
};

} // namespace render
