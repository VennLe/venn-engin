// ============================================================
// render/VariantPipelines —— 实现
// 设计说明见 VariantPipelines.h
// ============================================================

#include "render/VariantPipelines.h"

#include "rhi/Device.h"
#include "render/ShaderPath.h"

#include <cstdio>

namespace render {

namespace {

// 读一份 .spv。读失败时返回空（Pipeline 构造会因空字节码报错并给出
// 足够明确的提示）—— 真正要防止的是"静默用错变体"，所以这里不兜底。
std::vector<char> loadSpv(const std::string& file) {
    return vkutil::readFile(resolveShaderPath(file));
}

} // namespace

void applyKeywordState(KeywordMask m, KeywordMask stateMask,
                       rhi::PipelineDesc& desc) {
    // 不在 stateMask 里的位先清掉，免得深度预通道被 ALPHA_BLEND 带偏。
    const KeywordMask bits = m & stateMask;

    // ---- 双面 ----
    // 关背面剔除是"看得见背面"的前提；片元侧还要把背面法线翻过来
    // （pbr.frag 的 VENN_DOUBLE_SIDED 分支）。两件事缺一不可。
    if (hasKeyword(bits, kKeywordDoubleSided)) {
        desc.cullMode = VK_CULL_MODE_NONE;
    } else {
        desc.cullMode = VK_CULL_MODE_BACK_BIT;
    }

    // ---- 透明混合 ----
    // 不写深度是透明物体的铁律：写了就会把后面的透明物体挡掉。
    // 深度测试保留（半透明物体之间仍要按深度正确遮挡不透明的）。
    if (hasKeyword(bits, kKeywordAlphaBlend)) {
        desc.blendEnable = true;
        desc.depthWrite = false;
    } else {
        desc.blendEnable = false;
        desc.depthWrite = true;
    }

    // ---- 覆盖带（P0-2/B，2026-10-25）----
    // 放在 ALPHA_BLEND **之后**：overlay 可能是不透明的（眼白），也可能是
    // 混合的（睫毛），blendEnable 由上一段决定，这里只覆盖深度两项。
    //
    // depthTest = false → VK_FALSE，等价于"比较函数恒为 ALWAYS"：所有片元
    // 都通过，且**完全不读深度缓冲**。这是"眉毛穿透刘海"唯一能做对的写法 ——
    // 保留深度测试就一定会得出"刘海在前，眉毛被挡"的物理正确答案，
    // 而二次元要的是语义答案。
    //
    // 代价（已知，先接受）：overlay 项会无条件画在已画内容之上，包括
    // 它"物理上真该被挡住"的地方（侧脸时脸颊凸起挡住眼球边缘）。
    // 更精细的做法是保留 LEQUAL + 给这些顶点一个深度偏置（depthBias），
    // 那需要 PipelineDesc 再加一个字段；等真在某个资产上看到问题再做。
    if (hasKeyword(bits, kKeywordOverlay)) {
        desc.depthTest = false;
        desc.depthWrite = false;
    }
    // ALPHA_MASK 不参与固定状态：它**不改变**管线状态（写深度、不混合，
    // 与 Opaque 一模一样），只在片元里按 cutoff 丢弃片元。
    // 这正是它相对 Blend 的核心优势 —— 镂空物体照样写深度、照样能被
    // 不透明物体正确遮挡，且不需要排序。
    //
    // alphaToCoverage：只有当采样数 > 1 时才启用。它把片元的 alpha 转成
    // 逐采样覆盖掩码，于是 MSAA 下 Mask 的筛孔边缘会得到**硬件级的抗锯齿**
    // （比 discard 的硬边好看，且不需要 TAA）。采样数为 1 时 MUST 关 ——
    // Vulkan 规定 alphaToCoverageEnable 只在 rasterizationSamples != 1 时可用，
    // 否则 vkCreateGraphicsPipelines 直接报错。
    desc.alphaToCoverage = hasKeyword(bits, kKeywordAlphaMask) &&
                           desc.samples != VK_SAMPLE_COUNT_1_BIT;
    // NORMAL_MAP 不参与固定状态：它只改代码（少一次采样与 TBN）。
}

void VariantPipelines::init(rhi::Device& device,
                            const rhi::PipelineDesc& baseDesc,
                            const std::string& fragBaseName,
                            ShaderPermutation permutation,
                            KeywordMask stateMask) {
    // 热重载会对同一对象**二次 init**（Renderer::createPipelines 重跑）：
    // 先把上一代资源整个放掉。尤其是 m_layout —— 若不在这里销毁，
    // 下面新建的布局会把旧句柄覆盖掉，泄漏一份 VkPipelineLayout。
    // waitIdle 由调用方（pollShaderReload / setSampleCount）保证。
    clear();

    m_device = &device;
    m_base = baseDesc;
    m_fragBaseName = fragBaseName;
    m_permutation = permutation;
    m_stateMask = stateMask;
    // ⚠ 剔掉"只改状态、不改代码"的位（kKeywordStateOnly，目前是 OVERLAY）：
    //   它们参与缓存键（不同深度状态必须是不同管线），但不参与 .spv 选择
    //   —— 编出来的字节码会和掩码 0 完全相同，白编一倍纯属浪费。
    //   60 份变体 × 2 种深度状态 = 120 条管线，靠的就这一行。
    m_spvMask = (permutation == ShaderPermutation::PerMask)
                    ? (kKeywordAll & ~static_cast<KeywordMask>(kKeywordStateOnly))
                    : kKeywordNone;
    m_keyMask = m_spvMask | m_stateMask;

    // 布局在这里就建好，让所有变体共用 —— 这样 layout() 可以在**一条管线
    // 都还没建**的时候就被调用（recordFrame 要用它绑 set 0，那发生在第一
    // 个 draw 之前）。各变体透传 sharedLayout，谁都不再自己建布局。
    m_base.sharedLayout = rhi::createPipelineLayout(device, baseDesc);
    m_layout = m_base.sharedLayout;
}

const rhi::Pipeline& VariantPipelines::get(KeywordMask m) {
    // 缓存键只保留"真的会改变结果"的位。见 m_keyMask 的注释。
    const KeywordMask key = m & m_keyMask;
    ++m_lookups;

    const auto it = m_pipelines.find(key);
    if (it != m_pipelines.end()) {
        ++m_hits;
        return *it->second;
    }

    rhi::PipelineDesc desc = m_base;
    // 先按掩码改固定状态，再换 .spv —— 顺序无所谓，但没有这一步
    // 就会出现"编译期是双面、栅格化还在剔背面"的半成品变体。
    applyKeywordState(key, m_stateMask, desc);

    if (m_permutation == ShaderPermutation::PerMask) {
        // key & m_spvMask：缓存键里有 OVERLAY（要区分管线），文件名里没有
        // （字节码相同）。两件事用两个掩码分别表达，见 init 的注释。
        const std::string file =
            variantShaderFile(m_fragBaseName, key & m_spvMask);
        desc.fragSpv = loadSpv(file);
        if (desc.fragSpv.empty()) {
            std::fprintf(stderr,
                         "[ShaderVariants] 找不到变体着色器 %s（掩码 %u = %s）。"
                         "检查 CMakeLists.txt 的 SHADER_VARIANT_SHADERS 是否"
                         "覆盖了它。\n",
                         file.c_str(), key, keywordNames(key).c_str());
        }
    } else {
        desc.fragSpv = loadSpv(m_fragBaseName);
    }

    auto pipe = std::make_unique<rhi::Pipeline>(*m_device, desc);
    const rhi::Pipeline& ref = *pipe;
    m_pipelines.emplace(key, std::move(pipe));
    return ref;
}

std::string VariantPipelines::createdList() const {
    std::string out;
    char buf[128];
    for (uint32_t k = 0; k <= kKeywordAll; ++k) {
        if (m_pipelines.find(k) == m_pipelines.end()) continue;
        std::snprintf(buf, sizeof(buf), "k%u[%s]", k,
                      keywordNames(k).c_str());
        if (!out.empty()) out += " ";
        out += buf;
    }
    return out.empty() ? "(none)" : out;
}

void VariantPipelines::clear() {
    // 管线先走：它们借用了 m_layout，不能比它活得久。
    m_pipelines.clear();
    if (m_layout != VK_NULL_HANDLE && m_device) {
        vkDestroyPipelineLayout(m_device->get(), m_layout, nullptr);
    }
    m_layout = VK_NULL_HANDLE;
    m_base.sharedLayout = VK_NULL_HANDLE;
    m_device = nullptr;
    m_lookups = 0;
    m_hits = 0;
}

} // namespace render
