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
    if (mat.shadingModel == assets::Material::ShadingModel::Skin) {
        m |= kKeywordShadingSkin;
    }
    return m;
}

std::string variantTable(const uint32_t* counts, uint32_t count) {
    // 格式 "k0=2,k3=1"：逗号分隔、不带空格，便于验证脚本直接正则截取。
    // 掩码到名字的映射单独由 keywordNames() 给出（见 SHADER-VARIANTS-BUILT
    // 那条日志），这里不复述名字，免得同一行太长看不清。
    std::string out;
    char buf[32];
    for (uint32_t i = 0; i < count && i <= kKeywordAll; ++i) {
        if (counts[i] == 0) continue;  // 只列真的用到的，日志才读得动
        std::snprintf(buf, sizeof(buf), "k%u=%u", i, counts[i]);
        if (!out.empty()) out += ",";
        out += buf;
    }
    return out.empty() ? std::string("none") : out;
}

} // namespace render
