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
// ============================================================
struct MaterialSource {
    enum class Kind { Procedural, Model };

    Kind kind = Kind::Procedural;
    std::string modelKey;  // Model：AssetManager 里的模型缓存键

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
    // 那么"拖一张自发光图进来却什么都不亮"，得先去把因子调上去才知道生效。
    //     emissive = emissiveFactor + emissiveMap.rgb
    // 未绑定时绑定内置纯黑（0,0,0）→ 加法退化成恒等。
    Texture* emissiveMap = nullptr;    // 默认黑

    // glTF 双面标记（当前管线仍按背面剔除处理，留作扩展）
    bool doubleSided = false;

    // ---- 来源溯源（序列化用）----
    MaterialSource source;
};

} // namespace assets
