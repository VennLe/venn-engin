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
//      （见 CMakeLists.txt 的"维度声明 → 白名单"一节）；
//   3. 运行时把关键字组合压成一个位掩码（KeywordMask），按掩码去缓存里
//      取管线；没有就惰性建一条（见 VariantPipelines）。
//
// 于是"加一个特性"从"再加一条管线 + 再补三处 if"变成：
// 在下面的表里加一行、在着色器里加一段 #if。其余全自动。
//
// ------------------------------------------------------------
// ⚠ 位宽 ≠ 组合数（2026-10-25 改造的核心）
// ------------------------------------------------------------
// 早期版本用 `foreach(MASK RANGE 0..1<<N)` 把 2^N 种组合**无条件全展开**，
// 于是"加一个关键字就翻倍"。这在纯 PBR 时代没问题（6 位 = 64 份），
// 但二次元渲染要加 Toon / ToonHair / ToonEye / FaceSDF 四个着色模型：
//
//   · 旧机制：9 位 = 512 组合 × 2 个 .spv 族 = 1024 份，约 80 MB
//   · 新机制：Σ_模型(alpha 取值数 × 2^该模型特性数) ≈ 84 份 × 2 = 168 份
//
// 差别来自两个观察：
//   ① 有些关键字**语义互斥** —— alphaMode 只能是 Opaque / Blend / Mask
//      之一，"既 Blend 又 Mask"这个材质根本不存在，编它纯属浪费；
//      同理 shadingModel 也只能五选一。
//   ② 有些关键字**只对特定模型有意义** —— Parallax 视差贴图在 Toon 上
//      没人用，Ramp 渐变贴图在 PBR 上也没人用。按模型裁剪特性集，组合数
//      按"该模型允许的特性数"算，而不是按全局位数算。
//
// 所以：**位宽（下表的 kKeywordCount）与编译出的组合数是解耦的。**
// 位宽只决定 KeywordMask 的位空间与 .spv 文件名里的数字；编哪几种组合
// 由 CMakeLists.txt 的 GROUP_*/FEATURES_*/MODELS_* 声明决定。加模型时
// 位宽 +1、组合数 +Σ(该模型)，线性增长而不是翻倍。
//
// 两处的**位序**仍然必须逐位一致（掩码值就是文件名里的数字，错一位就
// 读不到文件）。这条一致性由下面的 static_assert 钉死：CMake 会把
// `-DVENN_KEYWORD_COUNT=<它数的位数>` 传进来，两张表长度不同直接编译
// 失败 —— 比等到运行时报"找不到变体着色器"早得多。
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
//   bit 6  SHADING_TOON  二次元赛璐璐：明暗交界二值化 + 冷色调阴影（A1）
//   bit 7  OVERLAY       覆盖带：不写深度 + 深度测试恒通过（P0-2/B）
//
// ⚠ OVERLAY 是一个**纯状态位**：它不改变任何一行片元代码，只改管线固定
//   状态（depthTest/depthWrite）。这类位由 kKeywordStateOnly 标出来，
//   VariantPipelines 拼 .spv 文件名时会把它剔掉 —— 于是 60 份变体
//   不用因为"想多一个深度状态"翻倍到 120 份。这是本文件头那段
//   "位宽 ≠ 组合数"的第二个应用场景（第一个是白名单）。
//
// ⚠ 位序就是"规范化顺序"：变体 ID、.spv 文件名、GLSL 宏名全部按它排。
//   调换位序等于换掉所有变体的身份，改之前先搜一遍 CMakeLists.txt。
//
// ⚠ SHADING_SKIN 与 SHADING_TOON 在**语义上互斥**（一个材质只能是其中
//   一种着色模型），但既然两者各占一位，掩码空间里就存在"两位都置"的
//   非法值。CMake 的白名单不会编它，运行时 keywordsOf() 也产不出它 ——
//   这里保留两立的表示只是为了让 ShadingModel 枚举能一对一映射到位，
//   不要在别处手工拼这种掩码。
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
    // shadingModel == Toon：漫反射响应二值化（赛璐璐），阴影用冷色调（A1）
    kKeywordShadingToon = 1u << 6,
    // 覆盖带（P0-2/B）：renderQueue >= Material::kOverlayQueueStart。
    // 只改管线固定状态（depthTest=ALWAYS + depthWrite=false），
    // **不改代码** —— 因此不参与 .spv 选择，见 kKeywordStateOnly。
    kKeywordOverlay = 1u << 7,

    // ⚠ 这里的数字只决定**位空间**，不决定编译出多少组合。
    //   组合数由 CMakeLists.txt 的白名单（GROUP_*/FEATURES_*/MODELS_*）决定，
    //   详见文件头"位宽 ≠ 组合数"。加模型时这里加一位即可，不会翻倍。
    kKeywordCount = 8,
    kKeywordAll = (1u << kKeywordCount) - 1u,

    // 只改管线固定状态、不改着色器代码的位。
    //
    // 拼 .spv 文件名时必须把它们从掩码里剔掉，否则会去找
    // "pbr.k128.frag.spv" —— 而白名单根本不会编这个组合（它和 k0 的
    // 字节码**逐字节相同**）。剔掉之后 60 份变体覆盖了 120 种管线状态。
    kKeywordStateOnly = kKeywordOverlay,
};

// 与 CMakeLists.txt 的 SHADER_KEYWORDS 长度对齐。
// CMake 传 -DVENN_KEYWORD_COUNT=<列表长度>；两边不等时的症状如果不在这里
// 拦住，就是运行期"找不到变体着色器 pbr.k<N>.frag.spv"——而那时已经
// 编译、链接、启动、加载场景全跑完了，排查成本高得多。
#ifndef VENN_KEYWORD_COUNT
// 非 CMake 构建（手工 g++ / IDE 直接打开）时的兜底。值与上表同步。
#define VENN_KEYWORD_COUNT 8
#endif
static_assert(kKeywordCount == VENN_KEYWORD_COUNT,
              "ShaderKeywords.h 的 kKeywordCount 与 CMakeLists.txt 的 "
              "SHADER_KEYWORDS 长度不一致：位序表与 -D 表已经错位了，"
              "编译出来的变体文件名会与运行时拼出来的对不上。");

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
// 帧末自检：本帧每个变体各画了多少个
//
// ⚠ 稀疏表（2026-10-25）：原来传的是 `uint32_t counts[1<<kKeywordCount]`
//   定长数组 —— 位宽一涨就是平方级浪费（7 位 512 B 尚可，15 位 128 KB，
//   20 位 4 MB，而 Renderer 是值语义存的）。实测一帧里同时出现的组合
//   通常不超过 10 种，所以改成"掩码 + 计数"的稀疏槽。
//   数组由调用方提供（Renderer 里是帧统计的一部分），容量用
//   kMaxVariantSlots，超出的项丢弃（只影响调试日志，不影响渲染）。
// ------------------------------------------------------------
struct VariantSlot {
    uint32_t mask = 0;
    uint32_t count = 0;
};
inline constexpr uint32_t kMaxVariantSlots = 64;

// 往稀疏表里累加一次。返回 false = 表满（调用方可以忽略）。
bool addVariantCount(VariantSlot* slots, uint32_t& used, KeywordMask m);

// 稀疏表 → 日志串 "k0=2,k3=1"；只列计数非 0 的项
std::string variantTable(const VariantSlot* slots, uint32_t used);

} // namespace render
