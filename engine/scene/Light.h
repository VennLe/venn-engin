#pragma once
// ============================================================
// scene/Light —— 光照数据结构
//
// 光属于"场景内容"，所以数据体都在 ecs::Components.h 里，这里只做：
//   1. 方向光的旧别名（scene::Light），保证既有代码零改动
//   2. LightInstance —— CPU → GPU 展平后的**逐灯**记录
//
// LightInstance 会被 Renderer 直接 memcpy 进 SSBO，所以它必须与
// 着色器里的 std430 布局逐字段对齐：4 个 vec4 紧凑排列、无填充。
// 着色器声明见 assets/shaders/pbr.frag 的 struct Light。
// ============================================================

#include "ecs/Components.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace scene {

// 方向光（既有代码沿用的名字）
using Light = ecs::DirectionalLightComponent;

// ============================================================
// LightInstance —— 展平后的单盏局部光源
//
// 用 vec4 而不是 vec3+float 分开写，是为了让 4 个 vec4 在 std430 下
// 恰好连续排列（vec3 成员会按 16 字节对齐并引入隐式填充，
// 手写布局非常容易算错）。
// ============================================================
struct LightInstance {
    glm::vec4 positionRange{0.0f};       // xyz = 世界坐标, w = 影响半径
    glm::vec4 colorIntensity{1.0f};      // rgb = 颜色, a = 强度
    glm::vec4 directionCosInner{0, -1, 0, 1.0f};  // xyz = 传播方向, w = cos(内锥)
    // x = cos(外锥)
    // y = 类型(0=点 1=射灯 2=方向光)
    // z = **阴影层号**：本灯在这张局部阴影图（2D array）里的基准层。
    //     -1 = 不投影。点灯要 6 层（立方体 6 个面），所以基准层 + 面号 = 实际层；
    //     射灯 1 层，基准层就是它自己。由 Renderer 每帧分配（见 prepareLocalShadows）。
    //     这个字段是"渲染侧算好、采样侧直接读"的 —— 着色器不需要知道分配策略。
    // w = 是否投射阴影（0/1）。**必须由组件带过来**：Renderer 只看得见
    //     LightInstance，看不到 ECS 组件。
    // ⚠ 用 z/w 这两个原本空着的分量承载阴影信息，是为了让 LightInstance
    //   **保持 64 字节**、与着色器 struct Light 的布局一字不改。
    glm::vec4 cosOuterType{0.0f};

    // 类型编码，写进 cosOuterType.y 供着色器分支。
    // 注意：C++ 的 enum 底层类型必须是整型（不能是 float），
    // 转成浮点是在 makePointLight/makeSpotLight/makeDirectionalLight 里做的。
    //
    // Directional（=2）是**实体级方向光**（不加阴影、无衰减，只参与
    // 分簇前向的局部光通道）。场景级太阳走的是另一条通道
    // （ShadowPass + 着色器里的 sun UBO），不在这里。
    enum class Type : int { Point = 0, Spot = 1, Directional = 2 };

    // 阴影层号（-1 = 不投影）
    int shadowLayer() const {
        return static_cast<int>(cosOuterType.z + 0.5f);
    }
    void setShadowLayer(int layer) { cosOuterType.z = static_cast<float>(layer); }
    bool castsShadow() const { return cosOuterType.w > 0.5f; }
};

// 与着色器 Light 结构体（4×vec4 = 64 字节）必须严格一致
static_assert(sizeof(LightInstance) == 64,
              "LightInstance 必须与着色器 struct Light 的 std430 布局一致");

// ============================================================
// LightModifierDesc —— CPU 侧的光源修饰描述（R3-3）
//
// LightInstance 刻意保持 64 字节、一字不改（阴影/分簇都按它算偏移），
// 所以"区域光 / IES / light function"这三样修饰单独走一条并行数组。
// 名字（不是层号）放在这里 —— 层号要由 Renderer 用 LightTextureLibrary
// 解析（scene 层不认识 GPU 纹理）。
// ============================================================
struct LightModifierDesc {
    float areaHalfWidth = 0.0f;    // 矩形面光源半宽（米）；0 = 不是面光源
    float areaHalfHeight = 0.0f;   // 半高
    std::string iesProfile;        // IES 名字（空 = 不用）
    std::string lightFunction;     // 投影贴图名字（空 = 不用）
};

// ============================================================
// LightExtInstance —— 展平后送 GPU 的逐灯修饰记录（32 字节）
//
// 与着色器 struct LightExt 的 std430 布局严格对应（2×vec4）。
// ============================================================
struct LightExtInstance {
    // x = 半宽(米) y = 半高(米) z = 是否面光源(0/1) w = 保留
    glm::vec4 area{0.0f};
    // x = IES 层号(-1=无) y = IES 开关(0/1)
    // z = light function 层号(-1=无) w = light function 开关(0/1)
    glm::vec4 tex{-1.0f, 0.0f, -1.0f, 0.0f};
};
static_assert(sizeof(LightExtInstance) == 32,
              "LightExtInstance 必须与着色器 struct LightExt 的 std430 布局一致");

// 把一盏灯加成 LightInstance（位置由调用方给出，通常取实体世界矩阵平移）
inline LightInstance makePointLight(const glm::vec3& pos,
                                    const ecs::PointLightComponent& pl) {
    LightInstance li;
    li.positionRange = glm::vec4(pos, pl.range);
    li.colorIntensity = glm::vec4(pl.color, pl.intensity);
    li.directionCosInner = glm::vec4(0.0f);
    // z = -1：层号由 Renderer 每帧分配，这里只声明"要不要影子"
    li.cosOuterType =
        glm::vec4(1.0f, static_cast<float>(LightInstance::Type::Point), -1.0f,
                  pl.castsShadow ? 1.0f : 0.0f);
    return li;
}

inline LightInstance makeSpotLight(const glm::vec3& pos,
                                   const ecs::SpotLightComponent& sl) {
    LightInstance li;
    li.positionRange = glm::vec4(pos, sl.range);
    li.colorIntensity = glm::vec4(sl.color, sl.intensity);
    const glm::vec3 d = glm::length(sl.direction) > 1e-6f
                            ? glm::normalize(sl.direction)
                            : glm::vec3(0.0f, -1.0f, 0.0f);
    li.directionCosInner = glm::vec4(d, std::cos(sl.innerAngle));
    li.cosOuterType = glm::vec4(std::cos(sl.outerAngle),
                                static_cast<float>(LightInstance::Type::Spot),
                                -1.0f, sl.castsShadow ? 1.0f : 0.0f);
    return li;
}

// 实体级方向光：无衰减、不投射阴影，照亮整个场景。
// 分簇剔除按"影响球"做 —— 给一个足够大的 range（10⁵ 米），方向光就会
// 落进每一个簇里；着色器看到 type=2 时直接用方向求值，不再做距离衰减。
inline LightInstance makeDirectionalLight(
    const glm::vec3& pos, const ecs::DirectionalLightComponent& dl) {
    LightInstance li;
    li.positionRange = glm::vec4(pos, 1e5f);
    li.colorIntensity = glm::vec4(dl.color, dl.intensity);
    const glm::vec3 d = glm::length(dl.direction) > 1e-6f
                            ? glm::normalize(dl.direction)
                            : glm::vec3(0.0f, 0.0f, -1.0f);
    li.directionCosInner = glm::vec4(d, 0.0f);
    // 实体级方向光不投影（场景级太阳才投，走 ShadowPass 那条路）
    li.cosOuterType = glm::vec4(
        0.0f, static_cast<float>(LightInstance::Type::Directional), -1.0f,
        0.0f);
    return li;
}

} // namespace scene
