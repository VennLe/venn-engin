#pragma once
// ============================================================
// scene/PostVolume —— 后处理体积（Post-Process Volume）
//
// UE 的 Post Process Volume 的 lite 版：场景里放一个 AABB，相机进入后
// 它携带的一组"相机效果覆盖值"按 **优先级 + 过渡带权重** 混进当前相机
// 的相机效果设置（见 render/CameraEffects.h）。
//
// 放在 scene/ 而不是 render/：它是**纯场景数据**（几何 + 标量），
// 不碰任何 Vulkan 对象 —— 和 Light 一样由场景持有、由序列化落盘、
// 由渲染层每帧读取求值。
//
// 覆盖值约定：
//   · 浮点项用 **kNoOverride = -1** 表示"这一项不覆盖"（用相机自己的值）。
//     所有可覆盖的浮点项天然 >= 0，所以 -1 是个安全哨兵。
//   · 三态开关用 int：-1 = 不覆盖 / 0 = 关 / 1 = 开。
//
// 求值规则（PostVolumeSet::evaluate）：
//   · 相机点落在 AABB 内 → 权重 1；在过渡带（blendDistance 米内）→ 线性
//     衰减到 0；在外 → 0。
//   · 多个体积同时生效时按 priority 升序叠加（权重大的覆盖权重小的），
//     与 UE 的"小体积 / 高优先级压大体积"直觉一致。
// ============================================================

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace scene {

// "不覆盖"哨兵。所有可覆盖浮点项都 >= 0，用 -1 表示缺省（不动 base）。
static constexpr float kNoOverride = -1.0f;

struct PostVolume {
    std::string name;

    // AABB（世界空间）
    glm::vec3 boundsMin{-1.0f};
    glm::vec3 boundsMax{1.0f};

    // 越大越"权威"。多个生效时按升序叠加。
    float priority = 1.0f;
    // 过渡带宽度（米）：从 AABB 表面往外这么多米内权重线性衰减到 0。
    // 0 = 硬边界（进出体积时参数会突跳）。
    float blendDistance = 1.0f;

    // ---- 浮点覆盖（kNoOverride = 不动）----
    float dofFocus = kNoOverride;
    float dofAperture = kNoOverride;
    float dofMaxRadius = kNoOverride;
    float motionBlurStrength = kNoOverride;
    float chromaStrength = kNoOverride;
    float vignetteStrength = kNoOverride;
    // 色彩分级 LUT 的混合权重（R1-3）。0 = 不用 LUT，1 = 完全走 LUT。
    float cgLutBlend = kNoOverride;

    // 自动曝光的 EV 偏置（R1-2）。同样用 kNoOverride 表示"不覆盖" ——
    // 于是**恰好 -1.0 的偏置无法表达**（用 -0.99 代替）。这是沿用
    // "浮点哨兵统一取 -1" 这条约定的代价，写在这里免得以后踩。
    float autoExposureBias = kNoOverride;

    // ---- 开关覆盖（-1 不动 / 0 关 / 1 开）----
    int dof = -1;
    int motionBlur = -1;
    int chromatic = -1;
    int vignette = -1;
};

class PostVolumeSet {
public:
    void add(const PostVolume& v) { m_volumes.push_back(v); }
    void clear() { m_volumes.clear(); }
    bool empty() const { return m_volumes.empty(); }
    const std::vector<PostVolume>& volumes() const { return m_volumes; }
    std::vector<PostVolume>& volumes() { return m_volumes; }

    // 求相机位置处"生效的覆盖值"。
    // 返回**每个字段独立的权重系数**没意义 —— 覆盖值是标量，直接把
    // "当前生效值"和"该值是否被覆盖"两个信息打包成下面的输出结构。
    struct Blended {
        // 权重加权后的覆盖值（未覆盖时 = 缺省常量，调用方按 mask 决定用不用）
        float dofFocus = kNoOverride;
        float dofAperture = kNoOverride;
        float dofMaxRadius = kNoOverride;
        float motionBlurStrength = kNoOverride;
        float chromaStrength = kNoOverride;
        float vignetteStrength = kNoOverride;
        float cgLutBlend = kNoOverride;
        float autoExposureBias = kNoOverride;
        int dof = -1;
        int motionBlur = -1;
        int chromatic = -1;
        int vignette = -1;
    };

    // camPos：相机世界位置。返回叠加后的覆盖值。
    Blended evaluate(const glm::vec3& camPos) const;

private:
    // 点到 AABB 的权重（1 = 内部，0 = 过渡带外）
    static float weightOf(const PostVolume& v, const glm::vec3& p);

    std::vector<PostVolume> m_volumes;
};

} // namespace scene
