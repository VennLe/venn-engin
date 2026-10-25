// ============================================================
// render/ShaderKeywords —— 关键字表与掩码推导的实现
// 设计说明见 ShaderKeywords.h
// ============================================================

#include "render/ShaderKeywords.h"

#include "assets/Material.h"

#include <cstdio>
#include <cstring>

namespace render {

namespace {

// 表顺序 = 位序（见 ShaderKeywords.h 末尾的"关键字位序"）。
// ⚠ 与之对应的两份定义：
//     · CMakeLists.txt 的 SHADER_KEYWORDS（决定编出哪些 -D）
//     · assets/shaders/pbr.frag 里的 #if <宏名>
//   三处任何一处改了，另外两处必须跟着改。
const KeywordInfo kKeywords[kKeywordCount] = {
    {kKeywordDoubleSided, "VENN_DOUBLE_SIDED", "double-sided"},
    {kKeywordAlphaBlend, "VENN_ALPHA_BLEND", "alpha-blend"},
    {kKeywordNormalMap, "VENN_NORMAL_MAP", "normal-map"},
    {kKeywordAlphaMask, "VENN_ALPHA_MASK", "alpha-mask"},
    {kKeywordShadingSkin, "VENN_SHADING_SKIN", "shading-skin"},
    {kKeywordParallax, "VENN_PARALLAX", "parallax"},
    {kKeywordShadingToon, "VENN_SHADING_TOON", "shading-toon"},
    {kKeywordOverlay, "VENN_OVERLAY", "overlay"},
};

// 位序 → 表下标的换算。表按位序排，所以下标就是"这是第几个被置的位"。
inline int bitIndex(ShaderKeyword k) {
    const uint32_t bit = static_cast<uint32_t>(k);
    for (int i = 0; i < static_cast<int>(kKeywordCount); ++i) {
        if ((1u << i) == bit) return i;
    }
    return -1;
}

} // namespace

const KeywordInfo* keywordTable() { return kKeywords; }

const KeywordInfo& keywordInfo(ShaderKeyword k) {
    const int i = bitIndex(k);
    static const KeywordInfo kUnknown{kKeywordNone, "?", "?"};
    return (i >= 0) ? kKeywords[i] : kUnknown;
}

std::string variantShaderFile(const std::string& baseName, KeywordMask m) {
    // "pbr.frag" -> "pbr" + ".k3." + "frag" + ".spv"
    // 没有扩展名时退化成 "name.k3.spv"（防呆，正常不会走到）。
    const std::size_t dot = baseName.rfind('.');
    const std::string stem =
        (dot == std::string::npos) ? baseName : baseName.substr(0, dot);
    const std::string ext =
        (dot == std::string::npos) ? std::string() : baseName.substr(dot + 1);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s.k%u", stem.c_str(), variantId(m));
    std::string out(buf);
    if (!ext.empty()) out += "." + ext;
    out += ".spv";
    return out;
}

std::string keywordNames(KeywordMask m) {
    m &= kKeywordAll;
    if (m == kKeywordNone) return "none";
    std::string out;
    for (int i = 0; i < static_cast<int>(kKeywordCount); ++i) {
        if ((m & (1u << i)) == 0u) continue;
        if (!out.empty()) out += "|";
        out += kKeywords[i].name;
    }
    return out;
}

KeywordMask keywordsOf(const assets::Material& mat) {
    KeywordMask m = kKeywordNone;
    if (mat.doubleSided) m |= kKeywordDoubleSided;
    if (mat.alphaMode == assets::Material::AlphaMode::Blend) {
        m |= kKeywordAlphaBlend;
    }
    if (mat.alphaMode == assets::Material::AlphaMode::Mask) {
        m |= kKeywordAlphaMask;
    }
    // 只看"有没有挂图"。normalScale == 0 时不置位也说得过去（缩放成 0
    // 等于不起作用），但那样同一个材质在拖动 normalScale 经过 0 时会
    // 反复切换变体、还要多编一份。少一份变体、多一次无害采样更划算。
    if (mat.normalMap) m |= kKeywordNormalMap;
    // 着色模型：**一对一映射**，不是"叠加开关"。
    // 枚举上 Toon 与 Skin 互斥，所以这里只会置其中一位 —— 但两者各占一个
    // bit，掩码空间里"两位都置"的值仍然存在。CMake 的白名单不编它，运行时
    // 也产不出它（只有这一个入口推导材质掩码）。见 ShaderKeywords.h 的说明。
    switch (mat.shadingModel) {
        case assets::Material::ShadingModel::Skin:
            m |= kKeywordShadingSkin;
            break;
        case assets::Material::ShadingModel::Toon:
            m |= kKeywordShadingToon;
            break;
        case assets::Material::ShadingModel::Default:
        default:
            break;
    }
    // POM：只看"有没有挂高度图"。heightScale == 0 时不置位也说得过去
    // （offset 恒 0 = 不起作用），但那样在编辑器里把滑条从 0 往上推的
    // 一瞬间才切变体、而且要多编一份"挂了图但不生效"的变体 ——
    // 与 normalScale 同一条取舍：少一份变体、多一次无害的采样。
    if (mat.heightMap) m |= kKeywordParallax;
    // 覆盖带（P0-2/B）：由 renderQueue 推出，**没有独立的布尔开关**。
    // 两个字段描述同一件事，迟早会出现"queue 设成 3100、开关忘了勾"
    // 的不一致，那时症状是"眼睛又被脸皮挡回去"—— 和改造前一模一样，
    // 却极难看出是配置问题而不是代码问题。
    if (mat.isOverlay()) m |= kKeywordOverlay;
    return m;
}

bool addVariantCount(VariantSlot* slots, uint32_t& used, KeywordMask m) {
    const KeywordMask key = m & kKeywordAll;
    // 线性扫 —— 表通常只有个位数项，比 hash 快且没有分配。
    for (uint32_t i = 0; i < used; ++i) {
        if (slots[i].mask == key) {
            ++slots[i].count;
            return true;
        }
    }
    if (used >= kMaxVariantSlots) return false;
    slots[used].mask = key;
    slots[used].count = 1;
    ++used;
    return true;
}

std::string variantTable(const VariantSlot* slots, uint32_t used) {
    // 格式 "k0=2,k3=1"：逗号分隔、不带空格，便于验证脚本直接正则截取。
    // 掩码到名字的映射单独由 keywordNames() 给出（见 SHADER-VARIANTS-BUILT
    // 那条日志），这里不复述名字，免得同一行太长看不清。
    std::string out;
    char buf[48];
    for (uint32_t i = 0; i < used && i < kMaxVariantSlots; ++i) {
        if (slots[i].count == 0) continue;  // 只列真的用到的，日志才读得动
        std::snprintf(buf, sizeof(buf), "k%u=%u", slots[i].mask, slots[i].count);
        if (!out.empty()) out += ",";
        out += buf;
    }
    return out.empty() ? std::string("none") : out;
}

} // namespace render
