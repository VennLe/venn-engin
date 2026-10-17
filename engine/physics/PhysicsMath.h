#pragma once
// ============================================================
// physics/PhysicsMath —— 刚体动力学的数学地基
//
// 这里只放"每个求解器都要用、且只有一种正确写法"的东西：
//
//   · 质量属性（MassProperties）—— 质量 / 逆质量 / **局部**惯量张量与
//     它的逆。局部量是关键：惯量张量随朝向变，所以世界空间的逆惯量
//     必须每次按当前旋转现算（invInertiaWorld），不能算一次缓存住。
//
//   · 四元数积分 —— 角速度 → 姿态。这里用**一阶近似 + 归一化**：
//       q' = normalize(q + 0.5 * ω_quat ⊗ q * dt)
//     而不是精确的 exp 映射。理由是求解器本身只有一阶精度（半隐式欧拉），
//     姿态用二阶公式并不会让整体更准，反而多一次 sin/cos。归一化是必须
//     的 —— 积分漂移会让四元数慢慢失去单位长度，进而让旋转矩阵带缩放。
//
//   · 各形状的**解析惯量张量**。这是"物体转起来像不像真的"的唯一来源：
//     拿球体惯量套在箱子上，箱子会像陀螺一样乱转。
//
// ------------------------------------------------------------
// ⚠ 本引擎的世界是**右手系 Z-up**（和 Blender 一致，不是 Unity 的 Y-up）
// ------------------------------------------------------------
// 所以：
//   · 胶囊 / 圆柱的**轴 = 局部 +Z**（不是 +Y）；
//   · 重力默认 (0, 0, -9.81)；
//   · 平面（Plane）的法线默认 (0, 0, 1)，平面方程 dot(n, x) = offset。
// 这一条贯穿整个物理模块，写新代码时不要再想 Y-up。
// ============================================================

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace physics {

// ---------------------------------------------------------------- 基础常量

constexpr float kPi = 3.14159265358979323846f;

// 引擎的世界是 Z-up，所以"竖直方向"是这个常量。
constexpr glm::vec3 kWorldUp{0.0f, 0.0f, 1.0f};

// 判定"数值上算不算 0"的阈值。用绝对量而不是相对量，是因为这里的量
// 大多是 1 米 / 1 千克量级的场景数据。
constexpr float kEpsilon = 1.0e-6f;

// 逆质量小于它 = 视作无限质量（静态 / 运动学刚体）。
// 用 0.1 千克的倒数（10）当分界太粗，这里用 1e-8 表示"质量 ≥ 1e8 公斤"。
constexpr float kInvMassEpsilon = 1.0e-8f;

// ---------------------------------------------------------------- 质量属性

// 刚体的惯性属性。**全部在局部空间**：惯量张量的世界版本随姿态变。
struct MassProperties {
    float mass = 1.0f;
    float invMass = 1.0f;
    // 相对**质心**的局部惯量张量及其逆
    glm::mat3 inertia{1.0f};
    glm::mat3 invInertia{1.0f};
    glm::vec3 centerOfMass{0.0f};

    // 静态 / 运动学刚体：质量无穷 → 逆质量 0。公式里"除以质量"的地方
    // 只要乘 invMass，就自动对无限质量体生效（冲量算出来是 0）。
    bool isStatic() const { return invMass < kInvMassEpsilon; }
};

// 把局部逆惯量转到世界空间：I_world⁻¹ = R · I_local⁻¹ · Rᵀ
//
// ⚠ 必须是 transpose(R) 在右边。写成 R ᵀ·I·R 会让**非均匀**惯量的
//   物体在非轴向姿态下转错方向（对称形状看不出来，长方体一眼可见）。
inline glm::mat3 invInertiaWorld(const MassProperties& mp,
                                 const glm::quat& rot) {
    if (mp.isStatic()) return glm::mat3(0.0f);
    const glm::mat3 r = glm::mat3_cast(rot);
    return r * mp.invInertia * glm::transpose(r);
}

// ---------------------------------------------------------------- 四元数

// 角速度积分一帧（见文件头的说明）。dt 较大时归一化尤其重要。
inline glm::quat integrateRotation(const glm::quat& q, const glm::vec3& omega,
                                  float dt) {
    if (dt <= 0.0f) return q;
    const glm::quat w(0.0f, omega.x * 0.5f * dt, omega.y * 0.5f * dt,
                      omega.z * 0.5f * dt);
    glm::quat out = q + w * q;
    // 归一化：不归一化的话每帧都会多出 ½ω²dt² 量级的长度误差，
    // 几万帧之后就变成"模型在悄悄变大"。
    return glm::normalize(out);
}

// 由单位旋转矩阵反解四元数（Shepperd 分支法）。
//
// 为什么不用 glm 的 quat_cast：那个实现在 trace 接近 -1（180° 旋转）时
// 会除以接近 0 的数，得到的四元数抖动很大 —— 而"翻转的箱子落回地面"
// 恰好就经过这个区间。这里按迹最大的那一项做除数，数值稳定。
inline glm::quat quatFromMat3(const glm::mat3& m) {
    const float t = m[0][0] + m[1][1] + m[2][2];
    glm::quat q;
    if (t > 0.0f) {
        float s = std::sqrt(t + 1.0f) * 2.0f;   // s = 4w
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    } else {
        float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return glm::normalize(q);
}

// 从矩阵里剥掉缩放，只取旋转（列向量归一化）。刚体不需要缩放 ——
// 形状尺寸是形状自己的参数，不该由 Transform 的 scale 二次施加。
inline glm::mat3 rotationOnly(const glm::mat3& m) {
    glm::mat3 r;
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 c = m[i];
        const float len = glm::length(c);
        r[i] = (len > kEpsilon) ? c / len : glm::vec3(i == 0 ? 1.0f : 0.0f,
                                                       i == 1 ? 1.0f : 0.0f,
                                                       i == 2 ? 1.0f : 0.0f);
    }
    return r;
}

// ---------------------------------------------------------------- 惯量张量
//
// 全部按"质量 m、绕**质心**"给。单位：kg·m²。
// 轴约定见文件头（Z-up）。

// 实心球
inline glm::mat3 inertiaSphere(float m, float r) {
    const float i = 0.4f * m * r * r;   // 2/5 m r²
    return glm::mat3(i, 0.0f, 0.0f, 0.0f, i, 0.0f, 0.0f, 0.0f, i);
}

// 长方体（halfExtents = 半边长）。对角元是 **m(b²+c²)/3**（b、c 是半边长），
// 不是 /12 —— /12 的写法针对的是"整边长"，混用会让立方体转得比实际慢 4 倍。
inline glm::mat3 inertiaBox(float m, const glm::vec3& halfExtents) {
    const float x2 = halfExtents.x * halfExtents.x;
    const float y2 = halfExtents.y * halfExtents.y;
    const float z2 = halfExtents.z * halfExtents.z;
    const float k = m / 3.0f;
    return glm::mat3(k * (y2 + z2), 0.0f, 0.0f,
                     0.0f, k * (x2 + z2), 0.0f,
                     0.0f, 0.0f, k * (x2 + y2));
}

// 胶囊（轴 = 局部 +Z）：圆柱段（半高 hh、半径 r）+ 两个半球。
//
// 用**半球质心**而不是"把两个半球当成一个整球"：
//   · 半球（质量 mh、半径 r）绕过球心的垂直轴：I = 2/5 mh r²
//   · 半球质心在离平面 3r/8 处 → 绕自身质心 I_c = 2/5 mh r² − mh(3r/8)²
//   · 再按平行轴定理把它挪到 (hh + 3r/8)
// 只用整球近似会让"长条胶囊"的转动惯量偏小 ~15%（两端的质量被当成
// 挤在中间了），滚起来明显发飘。
inline glm::mat3 inertiaCapsule(float m, float r, float hh) {
    const float vCyl = kPi * r * r * (2.0f * hh);
    const float vCap = (4.0f / 3.0f) * kPi * r * r * r;   // 两个半球 = 一个整球
    const float vTot = vCyl + vCap;
    if (vTot < kEpsilon) return glm::mat3(1.0f);
    const float mCyl = m * vCyl / vTot;
    const float mCap = m * vCap / vTot;   // 两个半球的**总**质量
    const float mHemi = mCap * 0.5f;

    // 绕 Z（对称轴）
    const float izz = 0.5f * mCyl * r * r + 0.4f * mCap * r * r;
    // 绕 X / Y
    const float izzCyl = mCyl * (r * r / 4.0f + hh * hh / 3.0f);
    const float d = hh + 0.375f * r;                       // 半球质心到中面
    const float iHemiOwn = 0.4f * mHemi * r * r -
                           mHemi * (0.375f * r) * (0.375f * r);
    const float ixx = izzCyl + 2.0f * (iHemiOwn + mHemi * d * d);
    return glm::mat3(ixx, 0.0f, 0.0f, 0.0f, ixx, 0.0f, 0.0f, 0.0f, izz);
}

// 把"质量 + 局部惯量"打包成 MassProperties（顺带求逆）。
inline MassProperties makeMassProperties(float mass, const glm::mat3& inertia,
                                         const glm::vec3& com = glm::vec3(0.0f)) {
    MassProperties mp;
    mp.centerOfMass = com;
    if (mass <= 0.0f) {
        // 非正质量 = 静态体。惯量留单位阵（反正逆质量是 0，用不到）。
        mp.mass = 0.0f;
        mp.invMass = 0.0f;
        mp.inertia = glm::mat3(0.0f);
        mp.invInertia = glm::mat3(0.0f);
        return mp;
    }
    mp.mass = mass;
    mp.invMass = 1.0f / mass;
    mp.inertia = inertia;
    // 惯量张量是实对称正定矩阵 → 可以用伴随矩阵求逆（3×3 直接算，
    // 比 glm::inverse 少一次分支）。退化时退到单位阵，避免 NaN 扩散。
    const glm::mat3 adj = glm::mat3(
        glm::cross(inertia[1], inertia[2]),
        glm::cross(inertia[2], inertia[0]),
        glm::cross(inertia[0], inertia[1]));
    const float det = glm::dot(inertia[0], glm::cross(inertia[1], inertia[2]));
    if (std::fabs(det) < 1.0e-12f) {
        mp.invInertia = glm::mat3(1.0f);
    } else {
        // glm 是列主序：adj 的三列分别是上面的 cross 结果，
        // transpose 才是数学上的伴随矩阵。
        mp.invInertia = glm::transpose(adj) / det;
    }
    return mp;
}

} // namespace physics
