#pragma once
// ============================================================
// assets/Material —— PBR 材质
//
// 参数集对齐 glTF 2.0 的 metallic-roughness 工作流：
//   baseColorFactor / metallicFactor / roughnessFactor / emissive
//   + albedo / normal / ORM 三张纹理
//
// ORM 贴图 = Occlusion-Roughness-Metallic 打包（glTF 标准做法）：
//   R = AO, G = Roughness, B = Metallic
// 三个通道塞进一张图，省采样器和显存。
// ============================================================

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace assets {

class Texture;

// ============================================================
// MaterialSource —— 材质的"来源描述"（供场景序列化重建）
//
//   Procedural —— 由因子 + 纹理槽直接构成，可以完整序列化
//                 （纹理槽自身也有 TextureSource，见 Texture.h）
//   Model      —— 由 glTF 解析而来，重建方式是重新加载该模型，
//                 所以这里只记模型缓存键即可（贴图一并回来）
//   Instance   —— 母材质的实例（W1）。参数值语义拷贝自母材质，
//                 但只有 `overridden` 位标出的字段才随本地修改。
//   VMAT       —— 来自一个独立的材质资产文件（`assets/materials/*.vmat`，W2-1）。
//                 与 Model 的区别：Model 的身份由模型文件里的材质名决定，
//                 VMAT 的身份由文件自己写（`"id"` 字段），所以**跨关卡可共享**。
// ============================================================
struct MaterialSource {
    enum class Kind { Procedural, Model, Instance, VMAT };

    Kind kind = Kind::Procedural;
    std::string modelKey;  // Model：AssetManager 里的模型缓存键
    std::string vmatPath;  // VMAT：相对 assets/ 的 .vmat 路径

    void setModel(const std::string& key) {
        kind = Kind::Model;
        modelKey = key;
    }
};

struct Material {
    std::string name;

    // ---- 透明模式 ----
    // Opaque：参与深度预通道，不透明管线绘制
    // Blend ：不参与深度预通道，混合管线绘制（不写深度、按到相机距离降序排序）
    // Mask  ：alpha 裁切（alpha test）。参与深度预通道、走不透明管线（写深度），
    //         但片元在 alpha < alphaCutoff 时直接 discard —— 于是"镂空"的
    //         部分既不写颜色也不写深度，从背面能直接看到透光。植被叶子、
    //         栅栏、铁丝网靠它，代价只有一次比较。**筛孔边缘是硬的**
    //         （没有半透明过渡），要高频道效果再叠抖动模式（见 ditherEnabled）。
    // 最终 alpha = baseColorFactor.a × albedo 贴图的 alpha 通道
    enum class AlphaMode { Opaque, Blend, Mask };
    AlphaMode alphaMode = AlphaMode::Opaque;

    // Mask 模式的裁切阈值：alpha < cutoff 的片元被丢掉。
    // 默认 0.5 是业界惯例（也正好是"sRGB 贴图里 0.5 线性亮度"这种直觉值）。
    // 只在 alphaMode == Mask 时参与着色器编译（其它模式这段代码整段编掉）。
    float alphaCutoff = 0.5f;

    // 抖动裁切（stochastic alpha test）：Mask 模式下不 discard，而是把
    // alpha 与一个屏幕空间蓝噪声阈值比较后写成 0/1 —— 得到"筛网边缘有
    // 概率穿透"的效果。配合 TAA（R1-1）多帧累积，边缘会被时间滤波成
    // 柔和的半透明观感，同时**保留写深度**（不像 Blend 那样排序崩坏）。
    // 关闭时（默认）Mask 走原样的硬 discard 路径 → 逐像素不变。
    bool ditherAlpha = false;

    // ---- 着色模型（R4-2）----
    // Default：标准金属-粗糙度 BRDF（漫反射 = max(N·L,0)）。
    // Skin   ：在此之上叠一层**预积分次表面散射** —— 用 (N·L, 曲率) 查一张
    //          预烘焙的散射 LUT 代替朴素的 max(N·L,0)，光按扩散核"绕过"
    //          明暗交界线，且红通道散射最远 → 鼻梁/耳缘/指尖泛红透光。
    //
    // "曲率"由片元处**几何法线**的屏幕空间导数（fwidth）近似，所以这个
    // 效果**不需要任何额外顶点数据或贴图**：同一张网格换成 Skin 就有反应，
    // 平坦的地方（脸颊正面）几乎与 Default 一致，只有法线变化快的地方
    // （轮廓边缘、鼻梁、耳缘）才明显。
    //
    // 语义上等价于"给这个材质换一个漫反射响应函数"，所以走**关键字变体**
    // （bit4 VENN_SHADING_SKIN）而不是运行时 uniform：Default 材质那段代码
    // 整段编掉 → 逐像素不变。
    enum class ShadingModel { Default, Skin };
    ShadingModel shadingModel = ShadingModel::Default;

    // ---- 因子（线性空间）----
    glm::vec4 baseColorFactor{1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float normalScale = 1.0f;
    float ao = 1.0f;
    glm::vec3 emissive{0.0f};  // 自发光，HDR 下可 > 1 触发 bloom

    // ---- 纹理槽 ----
    // nullptr 表示使用内置默认纹理；此时值等价于"中性"，只剩因子生效
    Texture* albedoMap = nullptr;  // 默认白 → albedo = baseColorFactor
    Texture* normalMap = nullptr;  // 默认 (0.5,0.5,1) → 平坦法线
    Texture* ormMap = nullptr;     // 默认白 → ao/rough/metallic 均为 1

    // ---- 单通道因子贴图（UE5 风格：每个因子旁边可以再挂一张图）----
    //
    // 与上面的 ormMap 是**相乘**关系，不是二选一：
    //     rough = roughnessFactor × ormMap.g × roughnessMap.r
    //     metal = metallicFactor  × ormMap.b × metallicMap.r
    // 未绑定时绑定内置的纯白（1,1,1）→ 乘法退化成恒等，行为与加这两个槽
    // 之前逐像素一致。glTF 导进来的材质走打包的 ORM，手工搭的材质则可以
    // 一张一张单独指定（只有粗糙度图、没有金属度图是很常见的）。
    //
    // 只读 R 通道（灰度图），所以按 **线性** 采样（srgb=false）：粗糙度是
    // 一个物理量，不是颜色，做 sRGB 解码会把数值整体抬亮。
    Texture* roughnessMap = nullptr;   // 默认白 → 只吃 roughnessFactor
    Texture* metallicMap = nullptr;    // 默认白 → 只吃 metallicFactor

    // 自发光贴图（RGB 颜色贴图，srgb=true）。
    // 这里是**相加**而不是相乘：emissive 因子默认是 0（不发光），若做乘法
    // 那么"拖一张自发光图进来却什么都不亮"——得先去把因子调上去才知道生效。
    //     emissive = emissiveFactor + emissiveMap.rgb
    // 未绑定时绑定内置纯黑（0,0,0）→ 加法退化成恒等。
    Texture* emissiveMap = nullptr;    // 默认黑

    // ---- 视差遮挡贴图（POM，R4-3）----
    //
    // heightMap：单通道高度场（只读 R，**线性**采样 srgb=false —— 高度是几何量
    //   不是颜色）。挂了它 → 关键字 bit5 VENN_PARALLAX 置位，片元在采样
    //   albedo / normal / orm / roughness / metallic / emissive **之前**先沿
    //   切线空间视向把 UV 推开一段 —— 于是平面上的砖缝/凹槽会随视角"错位"，
    //   看起来真的有凹凸。没挂它 → 那段代码整段编掉，逐像素不变。
    //
    // heightScale：视差深度，单位是 **UV**（不是米）—— 一条射线在 UV 空间里
    //   最多能推出多远。0.02~0.1 是常见区间（砖墙取小的、石板取大的）。
    //   **= 0 时 offset 恒为 0 → 与没挂高度图逐像素相同**，这正是
    //   "关闭 = 基线"那条约定的可断言形式。
    //
    // ⚠ 高度图**不能**当普通贴图理解：它描述的是"表面起伏"，所以：
    //   · 采样必须是线性的（sRGB 解码会把 0.5 抬成 0.21，高度全塌）；
    //   · 地址模式最好是 REPEAT（与 albedo 一致，否则 UV 推出边界会出现
    //     一条硬缝 —— 视差一旦生效，UV 必然会被推出 [0,1]）；
    //   · 没有高度数据时应当绑**纯黑**（0）而不是白：白 = 处处等高 = 射线
    //     会一路走到层数上限，退化成一个大偏移。黑 = 处处最低 = 立即停下
    //     → offset 0。这就是缺省槽位绑 m_defaultBlack 的原因。
    Texture* heightMap = nullptr;
    float heightScale = 0.05f;

    // glTF 双面标记（当前管线仍按背面剔除处理，留作扩展）
    bool doubleSided = false;

    // ---- 来源溯源（序列化用）----
    MaterialSource source;

    // ============================================================
    // 稳定身份（W0-2）
    //
    // 以前材质的"身份"是 `m_materials` 的缓存键，而模型材质那个键是
    // `models/xxx.glb/mat0` —— **数组下标**。模型重导出时材质顺序一变，
    // 所有引用静默错位（mat0 从 Skin 变成 Hair，没有任何报错）。
    //
    // `id` 是给人看、给存档用的语义身份：`models/xxx.glb#Skin`。
    // ⚠ 缓存键（`name`）**保持不变** —— `resolveMaterial` / `findMaterial` /
    //   `Model::subMeshes[i].material` 多处依赖它，改名会动 5 处以上。
    //   加一个并列字段、只让序列化用它，是成本最低且不破坏任何现有路径的做法。
    // ============================================================
    std::string id;

    // ============================================================
    // 可覆盖字段的参数集（W1 的位掩码就是这个字段序）
    //
    // 把「12 个可被实例覆盖 / 被存档记录的标量字段」收成一个可拷贝的
    // 结构，是为了三件事共用一份定义：
    //   · W0-3：导入模型的材质，靠它算出"用户到底改过哪几个字段"
    //   · W1-1：实例的 `overridden` 位掩码（位序写死在下面）
    //   · W1-2：母材质变更时按位刷新未覆盖字段
    //
    // ⚠ 纹理槽（7 个）**不进参数集**：换纹理是"结构变化"，
    //   一律视为覆盖（见 kOvAllTextures），不进位掩码。
    // ============================================================
    struct Params {
        glm::vec4 baseColorFactor{1.0f};
        float metallic = 0.0f;
        float roughness = 0.5f;
        float normalScale = 1.0f;
        float ao = 1.0f;
        glm::vec3 emissive{0.0f};
        AlphaMode alphaMode = AlphaMode::Opaque;
        float alphaCutoff = 0.5f;
        bool ditherAlpha = false;
        ShadingModel shadingModel = ShadingModel::Default;
        bool doubleSided = false;
        float heightScale = 0.05f;
    };

    // 位序（W1-1）—— ⚠ 一旦发布就不能改：存档里是裸数字。
    // 升位序必须同时升 `kMaterialVersion` 并写迁移。
    enum OverrideBits : std::uint32_t {
        kOvBaseColor   = 1u << 0,
        kOvMetallic    = 1u << 1,
        kOvRoughness   = 1u << 2,
        kOvNormalScale = 1u << 3,
        kOvAo          = 1u << 4,
        kOvEmissive    = 1u << 5,
        kOvAlphaMode   = 1u << 6,
        kOvAlphaCutoff = 1u << 7,
        kOvDitherAlpha = 1u << 8,
        kOvShadingModel = 1u << 9,
        kOvDoubleSided = 1u << 10,
        kOvHeightScale = 1u << 11,
        kOvScalarMask  = (1u << 12) - 1u,
    };

    // 取当前参数 / 写回参数
    Params captureParams() const {
        Params p;
        p.baseColorFactor = baseColorFactor;
        p.metallic = metallic;
        p.roughness = roughness;
        p.normalScale = normalScale;
        p.ao = ao;
        p.emissive = emissive;
        p.alphaMode = alphaMode;
        p.alphaCutoff = alphaCutoff;
        p.ditherAlpha = ditherAlpha;
        p.shadingModel = shadingModel;
        p.doubleSided = doubleSided;
        p.heightScale = heightScale;
        return p;
    }
    void applyParams(const Params& p) {
        baseColorFactor = p.baseColorFactor;
        metallic = p.metallic;
        roughness = p.roughness;
        normalScale = p.normalScale;
        ao = p.ao;
        emissive = p.emissive;
        alphaMode = p.alphaMode;
        alphaCutoff = p.alphaCutoff;
        ditherAlpha = p.ditherAlpha;
        shadingModel = p.shadingModel;
        doubleSided = p.doubleSided;
        heightScale = p.heightScale;
    }

    // 逐字段比较，返回"不同的那几位"（0 = 完全一致）。
    // ⚠ 浮点用精确相等而不是 eps：来源同为 `json.value(...)`，
    //   同一份文件读两次逐位相同，不需要容差（见 W0-1 的判据说明）。
    static bool sameVec4(const glm::vec4& a, const glm::vec4& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }
    static bool sameVec3(const glm::vec3& a, const glm::vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    static std::uint32_t diffParams(const Params& a, const Params& b) {
        std::uint32_t d = 0;
        if (!sameVec4(a.baseColorFactor, b.baseColorFactor)) d |= kOvBaseColor;
        if (a.metallic != b.metallic) d |= kOvMetallic;
        if (a.roughness != b.roughness) d |= kOvRoughness;
        if (a.normalScale != b.normalScale) d |= kOvNormalScale;
        if (a.ao != b.ao) d |= kOvAo;
        if (!sameVec3(a.emissive, b.emissive)) d |= kOvEmissive;
        if (a.alphaMode != b.alphaMode) d |= kOvAlphaMode;
        if (a.alphaCutoff != b.alphaCutoff) d |= kOvAlphaCutoff;
        if (a.ditherAlpha != b.ditherAlpha) d |= kOvDitherAlpha;
        if (a.shadingModel != b.shadingModel) d |= kOvShadingModel;
        if (a.doubleSided != b.doubleSided) d |= kOvDoubleSided;
        if (a.heightScale != b.heightScale) d |= kOvHeightScale;
        return d;
    }
    // 按位把 b 的字段刷进 a（用于母材质变更传播 / 读档应用 overrides）
    static void mergeParams(Params& a, const Params& b, std::uint32_t bits) {
        if (bits & kOvBaseColor) a.baseColorFactor = b.baseColorFactor;
        if (bits & kOvMetallic) a.metallic = b.metallic;
        if (bits & kOvRoughness) a.roughness = b.roughness;
        if (bits & kOvNormalScale) a.normalScale = b.normalScale;
        if (bits & kOvAo) a.ao = b.ao;
        if (bits & kOvEmissive) a.emissive = b.emissive;
        if (bits & kOvAlphaMode) a.alphaMode = b.alphaMode;
        if (bits & kOvAlphaCutoff) a.alphaCutoff = b.alphaCutoff;
        if (bits & kOvDitherAlpha) a.ditherAlpha = b.ditherAlpha;
        if (bits & kOvShadingModel) a.shadingModel = b.shadingModel;
        if (bits & kOvDoubleSided) a.doubleSided = b.doubleSided;
        if (bits & kOvHeightScale) a.heightScale = b.heightScale;
    }

    // ---- 导入快照（W0-3）----
    // 只有 `Kind::Model` 的材质会填：记录"模型刚加载出来时"的参数。
    // 有了它才能回答"用户到底改过哪几个字段"，进而只把差的那些写进存档
    // （只写覆盖项，见 world_scale_roadmap D2）。
    Params importDefaults;
    bool hasImportDefaults = false;

    void captureImportDefaults() {
        importDefaults = captureParams();
        hasImportDefaults = true;
    }

    // ---- 实例语义（W1-1）----
    // 母材质（nullptr = 自己是母材质）。值语义：实例的参数字段就是真值，
    // `overridden` 只决定"母材质变了要不要跟进"。
    // ⚠ 永远指向 AssetManager 里由 unique_ptr 持有的对象 —— 材质在本进程内
    //   不销毁，所以裸指针安全（`m_materials` 只增不删）。
    const Material* parent = nullptr;
    std::uint32_t overridden = 0;

    bool isInstance() const { return parent != nullptr; }

    // 两级限制：实例不能再当母材质（见 AssetManager::makeInstanceOf）
    bool canBeParent() const { return parent == nullptr; }

    // 把 child 里**未被覆盖**的字段刷成 parent 的值（W1-2 的核心算法）。
    // 返回 1 = 真的刷了，0 = 全被覆盖（跳过）。
    //
    // 抽成静态自由函数而不是留在 AssetManager 里，是为了让自检能在
    // **没有 AssetManager、没有 GPU、没有窗口** 的机器上直接测这段逻辑
    // （AssetManager 的构造需要一个 TextureContext = 真设备）。
    static int syncInstanceParams(Material& child, const Material& parent) {
        const std::uint32_t follow = kOvScalarMask & ~child.overridden;
        if (!follow) return 0;
        Params p = child.captureParams();
        mergeParams(p, parent.captureParams(), follow);
        child.applyParams(p);
        return 1;
    }
};

} // namespace assets
