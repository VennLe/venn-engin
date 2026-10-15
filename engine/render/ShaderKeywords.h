#pragma once
// ============================================================
// render/ShaderKeywords —— Shader 变体 / 关键字系统
//
// 问题：着色器是**离线 glslc 预编译**的固定管线。一个 .spv 编出来就把
// 所有分支固化死了，没法"这个材质是双面的 / 这张材质没挂法线图"地开关。
// 于是过去只能走两条路，两条都不好：
//   · 把分支写成运行时 uniform → 每个像素都要判断，还被驱动当成真分支；
//   · 手写 N 条管线（本引擎原来就是：不透明/双面/透明/透明双面四条，
//     外加三处 if 阶梯）。再加一个特性就是 8 条，再加一个就是 16 条。
//
// 方案：**关键字（keyword）**。一个关键字 = 一个编译期开关：
//   1. 着色器里用 `#if VENN_XXX` 包住那段代码；
//   2. 构建期对每一种关键字组合，用 `glslc -DVENN_XXX=1` 各编一份 .spv
//      （见 CMakeLists.txt 的 SHADER_KEYWORDS / SHADER_VARIANT_SHADERS）；
//   3. 运行时把关键字组合压成一个位掩码（KeywordMask），按掩码去缓存里
//      取管线；没有就惰性建一条（见 VariantPipelines）。
//
// 于是"加一个特性"从"再加一条管线 + 再补三处 if"变成：
// 在下面的表里加一行、在着色器里加一段 #if。其余全自动。
//
// ⚠ 关键字掩码同时决定**两组东西**，这也是它必须由掩码统一推导的原因：
//   · 编进 .spv 的代码路径（NORMAL_MAP 少一次纹理采样 + 一次 TBN，
//     DOUBLE_SIDED 多一次 gl_FrontFacing 法线翻转）；
//   · 管线**固定状态**（DOUBLE_SIDED → cullMode=NONE，
//     ALPHA_BLEND → blendEnable=true + depthWrite=false）。
//   两者分开推导迟早会错位（比如掩码说双面、状态却还在背面剔除）。
//
// ---- 关键字位序（改动要同步 CMakeLists.txt 与 assets/shaders/）----
//   bit 0  DOUBLE_SIDED  双面：关背面剔除 + 背面翻转法线
//   bit 1  ALPHA_BLEND   透明混合：走混合管线、不写深度、alpha 参与输出
//   bit 2  NORMAL_MAP    材质挂了法线贴图：采样 + TBN 变换
//   bit 3  ALPHA_MASK    alpha 裁切：按 cutoff discard（或不 discard 时抖动）
//   bit 4  SHADING_SKIN  预积分皮肤散射：漫反射项改查 (N·L, 曲率) LUT
//   bit 5  PARALLAX      POM 视差遮挡贴图：UV 沿切线空间视向推开（R4-3）
//
// ⚠ 位序就是"规范化顺序"：变体 ID、.spv 文件名、GLSL 宏名全部按它排。
//   调换位序等于换掉所有变体的身份，改之前先搜一遍 CMakeLists.txt。
// ============================================================

#include <cstdint>
#include <string>

namespace assets {
struct Material;
}

namespace render {

// ------------------------------------------------------------
// 关键字位
// ------------------------------------------------------------
enum ShaderKeyword : uint32_t {
    kKeywordNone = 0u,
    // doubleSided：两面都画，且背面用翻转后的法线着色
    kKeywordDoubleSided = 1u << 0,
    // alphaMode == Blend：走半透明管线
    kKeywordAlphaBlend = 1u << 1,
    // 材质挂了法线贴图：采样 + cotangent frame
    kKeywordNormalMap = 1u << 2,
    // alphaMode == Mask：片元按 alphaCutoff 裁切（可选抖动）
    kKeywordAlphaMask = 1u << 3,
    // shadingModel == Skin：漫反射按预积分 SSS LUT 采样（R4-2）
    kKeywordShadingSkin = 1u << 4,
    // 材质挂了高度图：UV 先做视差遮挡贴图（POM）再采样其余贴图（R4-3）
    kKeywordParallax = 1u << 5,

    kKeywordCount = 6,
    kKeywordAll = (1u << kKeywordCount) - 1u,
};

using KeywordMask = uint32_t;

// 一个关键字的三份名字。三份都必须有，缺一样就会出现
// "CMake 编了、运行时找不到"或"日志看不懂是哪个变体"这类问题。
struct KeywordInfo {
    ShaderKeyword bit;
    const char* macro;  // GLSL 宏名（glslc -D<macro>=1，着色器里 #if <macro>）
    const char* name;   // 日志 / UI 用的短名
};

// 长度 = kKeywordCount，按位序排列
const KeywordInfo* keywordTable();
const KeywordInfo& keywordInfo(ShaderKeyword k);

inline bool hasKeyword(KeywordMask m, ShaderKeyword k) {
    return (m & static_cast<uint32_t>(k)) != 0u;
}

inline KeywordMask withKeyword(KeywordMask m, ShaderKeyword k, bool on) {
    const uint32_t bit = static_cast<uint32_t>(k);
    return on ? (m | bit) : (m & ~bit);
}

// 掩码 → 变体 ID。约定 **ID 就是掩码本身**（0..7）：
// 不做任何重映射，看一眼日志里的数字就知道是哪几个关键字。
inline uint32_t variantId(KeywordMask m) { return m & kKeywordAll; }

// ------------------------------------------------------------
// 变体的 .spv 文件名
//
//   variantShaderFile("pbr.frag", 0)  ->  "pbr.k0.frag.spv"
//   variantShaderFile("pbr.frag", 5)  ->  "pbr.k5.frag.spv"
//
// 用十进制 ID 而不是 "ds1_ab0_nm1" 这种可读后缀：文件名要跟 C++ 里的
// variantId 逐字一致，可读性交给 logKeywordMask()（它会把 ID 展开成
// "double-sided|normal-map"）。
// ------------------------------------------------------------
std::string variantShaderFile(const std::string& baseName, KeywordMask m);

// 人类可读的关键字列表：kKeywordNone -> "none"，5 -> "double-sided|normal-map"
std::string keywordNames(KeywordMask m);

// ------------------------------------------------------------
// 材质 → 关键字。**这是"材质特性开关变体"的唯一一条规则**，
// 绘制路径与验证脚本都必须走它，不要各自再写一遍 if。
//
//   doubleSided         -> kKeywordDoubleSided
//   alphaMode == Blend  -> kKeywordAlphaBlend
//   normalMap != null   -> kKeywordNormalMap
//   alphaMode == Mask   -> kKeywordAlphaMask
//   shadingModel == Skin -> kKeywordShadingSkin
//
// ⚠ Blend 与 Mask 互斥（一次只能一个），枚举本身就保证了这点。
//
// ⚠ normalMap 为 null 时**不是**"采样一张白图"，而是把整段采样与 TBN
//   变换编译掉。两者的画面必须逐像素相同（默认平坦法线贴图算出来的
//   法线就是顶点法线本身）—— 验证脚本把这条当核心断言。
// ------------------------------------------------------------
KeywordMask keywordsOf(const assets::Material& mat);

// ------------------------------------------------------------
// 帧末自检：本帧每个变体各画了多少个（counts 长度 = 1<<kKeywordCount）
// ------------------------------------------------------------
std::string variantTable(const uint32_t* counts, uint32_t count);

} // namespace render
