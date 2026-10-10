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
    glm::vec4 cosOuterType{0.0f};        // x = cos(外锥), y = 类型(0=点 1=射灯)

    // 类型编码，写进 cosOuterType.y 供着色器分支。
    // 注意：C++ 的 enum 底层类型必须是整型（不能是 float），
    // 转成浮点是在 makePointLight/makeSpotLight/makeDirectionalLight 里做的。
    //
    // Directional（=2）是**实体级方向光**（不加阴影、无衰减，只参与
    // 分簇前向的局部光通道）。场景级太阳走的是另一条通道
    // （ShadowPass + 着色器里的 sun UBO），不在这里。
    enum class Type : int { Point = 0, Spot = 1, Directional = 2 };
};

// 与着色器 Light 结构体（4×vec4 = 64 字节）必须严格一致
static_assert(sizeof(LightInstance) == 64,
              "LightInstance 必须与着色器 struct Light 的 std430 布局一致");

// 把一盏灯加成 LightInstance（位置由调用方给出，通常取实体世界矩阵平移）
inline LightInstance makePointLight(const glm::vec3& pos,
                                    const ecs::PointLightComponent& pl) {
    LightInstance li;
    li.positionRange = glm::vec4(pos, pl.range);
    li.colorIntensity = glm::vec4(pl.color, pl.intensity);
    li.directionCosInner = glm::vec4(0.0f);
    li.cosOuterType =
        glm::vec4(1.0f, static_cast<float>(LightInstance::Type::Point), 0.0f,
                  0.0f);
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
                                0.0f, 0.0f);
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
    li.cosOuterType = glm::vec4(
        0.0f, static_cast<float>(LightInstance::Type::Directional), 0.0f, 0.0f);
    return li;
}

} // namespace scene
