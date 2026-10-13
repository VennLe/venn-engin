#include "scene/PostVolume.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace scene {

namespace {

// 单个标量覆盖的叠加：base 为当前已累积的覆盖值（kNoOverride = 尚无），
// val 是本体积的值，w 是权重。
//   · w <= 0        → 不动
//   · base 未覆盖   → 直接取 val（按权重"部分生效"没有意义，参数上要么
//                     用这个体积的值、要么用相机自己的值；这里简化为
//                     "权重过半即采用"，避免出现半截参数）
//   · base 已覆盖   → 高优先级体积已经写了值，同优先级下按权重线性插值
inline void blendFloat(float& base, float val, float w) {
    if (w <= 0.0f || val == kNoOverride) return;
    if (w >= 0.5f) {
        base = val;
    } else if (base != kNoOverride) {
        base = base * (1.0f - w) + val * w;
    }
}

inline void blendInt(int& base, int val, float w) {
    if (w <= 0.0f || val < 0) return;
    if (w >= 0.5f) base = val;
}

} // namespace

float PostVolumeSet::weightOf(const PostVolume& v, const glm::vec3& p) {
    const glm::vec3 lo = glm::min(v.boundsMin, v.boundsMax);
    const glm::vec3 hi = glm::max(v.boundsMin, v.boundsMax);

    // AABB 外的"距离"：每个轴超出量取 max(0, ...)
    const glm::vec3 d = glm::max(glm::max(lo - p, p - hi), glm::vec3(0.0f));
    const float dist = glm::length(d);
    if (dist <= 0.0f) return 1.0f;  // 内部

    const float blend = std::max(v.blendDistance, 1e-4f);
    if (dist >= blend) return 0.0f;
    return 1.0f - dist / blend;  // 过渡带线性衰减
}

PostVolumeSet::Blended PostVolumeSet::evaluate(const glm::vec3& camPos) const {
    Blended out;

    // 按 priority 升序叠加：先低后高，高优先级最后写 → 覆盖低优先级。
    std::vector<const PostVolume*> sorted;
    sorted.reserve(m_volumes.size());
    for (const auto& v : m_volumes) {
        if (weightOf(v, camPos) > 0.0f) sorted.push_back(&v);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const PostVolume* a, const PostVolume* b) {
                         return a->priority < b->priority;
                     });

    for (const PostVolume* v : sorted) {
        const float w = weightOf(*v, camPos);
        blendFloat(out.dofFocus, v->dofFocus, w);
        blendFloat(out.dofAperture, v->dofAperture, w);
        blendFloat(out.dofMaxRadius, v->dofMaxRadius, w);
        blendFloat(out.motionBlurStrength, v->motionBlurStrength, w);
        blendFloat(out.chromaStrength, v->chromaStrength, w);
        blendFloat(out.vignetteStrength, v->vignetteStrength, w);
        blendFloat(out.cgLutBlend, v->cgLutBlend, w);
        blendFloat(out.autoExposureBias, v->autoExposureBias, w);
        blendInt(out.dof, v->dof, w);
        blendInt(out.motionBlur, v->motionBlur, w);
        blendInt(out.chromatic, v->chromatic, w);
        blendInt(out.vignette, v->vignette, w);
    }
    return out;
}

} // namespace scene
