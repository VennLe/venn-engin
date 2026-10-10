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
    // 最终 alpha = baseColorFactor.a × albedo 贴图的 alpha 通道
    enum class AlphaMode { Opaque, Blend };
    AlphaMode alphaMode = AlphaMode::Opaque;

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
