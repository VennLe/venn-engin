#include "physics/Joints.h"

#include <algorithm>
#include <cmath>

namespace physics {

namespace {

// 四元数 → 旋转向量（axis × angle）。小角时用 2·vec 的线性近似。
//
// 为什么不用"转成欧拉角再比"：欧拉角有万向锁，绕 Y 转 90° 之后绕 X 与
// 绕 Z 会变成同一个自由度，两行约束就会去解同一个方向 —— 表现为关节
// 在特定朝向下"锁不住"。旋转向量没有这个问题。
glm::vec3 quatToRotationVector(const glm::quat& qIn) {
    glm::quat q = qIn;
    // w < 0 表示"绕远路"的那一半 → 取反得到等价的短路径旋转，
    // 否则误差向量会突然翻向，求解器每帧都在推反方向（抖动）。
    if (q.w < 0.0f) q = glm::quat(-q.w, -q.x, -q.y, -q.z);
    const glm::vec3 v(q.x, q.y, q.z);
    const float s = glm::length(v);
    if (s < 1.0e-6f) return 2.0f * v;          // 小角近似
    const float angle = 2.0f * std::atan2(s, q.w);
    return v * (angle / s);
}

// 约束方向上一条标量行的**有效质量倒数**。
// 与 Solver.cpp 里接触用的是同一个公式；角行为纯角约束（力臂为 0），
// 公式自然退化成 nᵀ(IA⁻¹+IB⁻¹)n。
float rowMassInv(const glm::vec3& n, bool angular, const glm::vec3& rA,
                 const glm::vec3& rB, const RigidBody& A, const RigidBody& B,
                 const glm::mat3& iA, const glm::mat3& iB) {
    if (angular) return glm::dot(n, iA * n + iB * n);
    const glm::vec3 cA = glm::cross(rA, n);
    const glm::vec3 cB = glm::cross(rB, n);
    return A.massProps.invMass + B.massProps.invMass + glm::dot(cA, iA * cA) +
           glm::dot(cB, iB * cB);
}

// 从一根轴构造一组**正交基**（铰链 / 滑动的两个被锁方向）。
// 分支判据同 Solver.cpp 的 tangentBasis —— 无条件用 cross(axis, (0,0,1))
// 会在轴正好是 ±Z 时退化，而 Z-up 世界里"竖直的铰链"恰好最常见。
void perpendicularBasis(const glm::vec3& axis, glm::vec3& u, glm::vec3& v) {
    const glm::vec3 a = glm::normalize(axis);
    const glm::vec3 ref = (std::fabs(a.x) > 0.9f) ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                  : glm::vec3(1.0f, 0.0f, 0.0f);
    u = glm::normalize(glm::cross(a, ref));
    v = glm::cross(a, u);
}

} // namespace

void Joint::clearImpulses() {
    for (int i = 0; i < rowCount; ++i) rows[i].impulse = 0.0f;
}

void Joint::prestep(const RigidBody& A, const RigidBody& B, float dt,
                    float baumgarte) {
    rowCount = 0;
    const float beta = baumgarte / std::max(dt, 1.0e-6f);

    worldAnchorA = A.position + A.rotation * localAnchorA;
    worldAnchorB = B.position + B.rotation * localAnchorB;
    worldAxis = glm::normalize(A.rotation * localAxisA);

    const glm::vec3 rA = worldAnchorA - A.position;
    const glm::vec3 rB = worldAnchorB - B.position;
    const glm::mat3 iA = A.invInertiaWorld();
    const glm::mat3 iB = B.invInertiaWorld();
    // 线性位置误差：B 侧锚点相对 A 侧锚点的偏移
    const glm::vec3 error = worldAnchorB - worldAnchorA;

    // 线性等式行：把锚点在 n 方向上的偏移拉回 0
    auto pushLinear = [&](const glm::vec3& n, float biasOverride,
                          bool hasOverride) {
        JointRow& row = rows[rowCount++];
        row.n = n;
        row.rA = rA;
        row.rB = rB;
        row.angular = false;
        row.lower = -kInf;
        row.upper = kInf;
        const float k = rowMassInv(n, false, rA, rB, A, B, iA, iB);
        row.mass = (k > kEpsilon) ? 1.0f / k : 0.0f;
        // 目标：把误差消掉。error 在 +n 上偏了，就要让相对速度沿 -n。
        row.bias = hasOverride ? biasOverride : -beta * glm::dot(error, n);
    };
    // 角等式行：angularError 已经投影到这个方向上
    auto pushAngular = [&](const glm::vec3& n, float angularError) {
        JointRow& row = rows[rowCount++];
        row.n = n;
        row.rA = glm::vec3(0.0f);
        row.rB = glm::vec3(0.0f);
        row.angular = true;
        row.lower = -kInf;
        row.upper = kInf;
        const float k = rowMassInv(n, true, rA, rB, A, B, iA, iB);
        row.mass = (k > kEpsilon) ? 1.0f / k : 0.0f;
        row.bias = -beta * angularError;
    };

    switch (type) {
        case JointType::Ball: {
            pushLinear(glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, false);
            pushLinear(glm::vec3(0.0f, 1.0f, 0.0f), 0.0f, false);
            pushLinear(glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, false);
            currentPosition = 0.0f;
            break;
        }
        case JointType::Fixed: {
            pushLinear(glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, false);
            pushLinear(glm::vec3(0.0f, 1.0f, 0.0f), 0.0f, false);
            pushLinear(glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, false);
            // 角误差在 A 的局部帧里算，再转到世界 —— 三根世界轴就能覆盖
            // 全部三个转动自由度，且没有万向锁。
            const glm::quat qRel = glm::conjugate(A.rotation) * B.rotation;
            const glm::quat qErr = glm::conjugate(localRefRotation) * qRel;
            const glm::vec3 errWorld = A.rotation * quatToRotationVector(qErr);
            pushAngular(glm::vec3(1.0f, 0.0f, 0.0f), errWorld.x);
            pushAngular(glm::vec3(0.0f, 1.0f, 0.0f), errWorld.y);
            pushAngular(glm::vec3(0.0f, 0.0f, 1.0f), errWorld.z);
            currentPosition = 0.0f;
            break;
        }
        case JointType::Hinge: {
            pushLinear(glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, false);
            pushLinear(glm::vec3(0.0f, 1.0f, 0.0f), 0.0f, false);
            pushLinear(glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, false);

            // 两个被锁的转动方向：垂直于铰链轴的一对正交方向。
            // ⚠ 这里必须区分**局部**与**世界**两套表示：
            //   · 行方向要用世界向量（求解器在世界空间工作）
            //   · 误差投影要用局部向量（qRel 的向量部分在 A 的局部帧里）
            //   混用会让约束在物体一转起来时就失效（看起来"铰链会松"）。
            glm::vec3 uL, vL;
            perpendicularBasis(localAxisA, uL, vL);
            const glm::vec3 uW = A.rotation * uL;
            const glm::vec3 vW = A.rotation * vL;

            const glm::quat qRel = glm::conjugate(A.rotation) * B.rotation;
            const glm::vec3 errLocal = quatToRotationVector(qRel);
            pushAngular(uW, glm::dot(errLocal, uL));
            pushAngular(vW, glm::dot(errLocal, vL));

            // 绕铰链轴的转角（qRel 的向量部分在 A 局部帧 → 用局部轴）
            const glm::vec3 axisL = glm::normalize(localAxisA);
            const glm::vec3 vq(qRel.x, qRel.y, qRel.z);
            currentPosition = 2.0f * std::atan2(glm::dot(vq, axisL), qRel.w);

            // ---- 限位：单边不等式行 ----
            //
            // 约束的是"绕轴相对角速度"：在**下**限位时只允许正向转速
            // （累积冲量 ≥ 0），在**上**限位时只允许负向（≤ 0）。
            // bias 用一个公式覆盖两种情况：-β·(当前 − 目标)。
            if (useLimit) {
                const bool atLower = currentPosition <= lowerLimit;
                const bool atUpper = currentPosition >= upperLimit;
                if (atLower || atUpper) {
                    JointRow& row = rows[rowCount++];
                    row.n = worldAxis;
                    row.angular = true;
                    row.lower = atLower ? 0.0f : -kInf;
                    row.upper = atLower ? kInf : 0.0f;
                    const float k =
                        rowMassInv(worldAxis, true, rA, rB, A, B, iA, iB);
                    row.mass = (k > kEpsilon) ? 1.0f / k : 0.0f;
                    row.bias = -beta * (currentPosition -
                                        (atLower ? lowerLimit : upperLimit));
                    row.impulse = 0.0f;
                }
            }
            // ---- 马达：等式行 + 冲量有界（= 最大力矩）----
            if (useMotor) {
                JointRow& row = rows[rowCount++];
                row.n = worldAxis;
                row.angular = true;
                const float maxImp = maxMotorForce * dt;
                row.lower = -maxImp;
                row.upper = maxImp;
                const float k =
                    rowMassInv(worldAxis, true, rA, rB, A, B, iA, iB);
                row.mass = (k > kEpsilon) ? 1.0f / k : 0.0f;
                row.bias = motorSpeed;   // 目标相对角速度
                row.impulse = 0.0f;
            }
            break;
        }
        case JointType::Slider: {
            glm::vec3 uL, vL;
            perpendicularBasis(localAxisA, uL, vL);
            pushLinear(A.rotation * uL, 0.0f, false);
            pushLinear(A.rotation * vL, 0.0f, false);

            const glm::quat qRel = glm::conjugate(A.rotation) * B.rotation;
            const glm::quat qErr = glm::conjugate(localRefRotation) * qRel;
            const glm::vec3 errWorld = A.rotation * quatToRotationVector(qErr);
            pushAngular(glm::vec3(1.0f, 0.0f, 0.0f), errWorld.x);
            pushAngular(glm::vec3(0.0f, 1.0f, 0.0f), errWorld.y);
            pushAngular(glm::vec3(0.0f, 0.0f, 1.0f), errWorld.z);

            currentPosition = glm::dot(worldAnchorB - worldAnchorA, worldAxis);

            if (useLimit) {
                const bool atLower = currentPosition <= lowerLimit;
                const bool atUpper = currentPosition >= upperLimit;
                if (atLower || atUpper) {
                    JointRow& row = rows[rowCount++];
                    row.n = worldAxis;
                    row.rA = rA;
                    row.rB = rB;
                    row.angular = false;
                    row.lower = atLower ? 0.0f : -kInf;
                    row.upper = atLower ? kInf : 0.0f;
                    const float k =
                        rowMassInv(worldAxis, false, rA, rB, A, B, iA, iB);
                    row.mass = (k > kEpsilon) ? 1.0f / k : 0.0f;
                    row.bias = -beta * (currentPosition -
                                        (atLower ? lowerLimit : upperLimit));
                    row.impulse = 0.0f;
                }
            }
            if (useMotor) {
                JointRow& row = rows[rowCount++];
                row.n = worldAxis;
                row.rA = rA;
                row.rB = rB;
                row.angular = false;
                const float maxImp = maxMotorForce * dt;
                row.lower = -maxImp;
                row.upper = maxImp;
                const float k =
                    rowMassInv(worldAxis, false, rA, rB, A, B, iA, iB);
                row.mass = (k > kEpsilon) ? 1.0f / k : 0.0f;
                row.bias = motorSpeed;
                row.impulse = 0.0f;
            }
            break;
        }
        case JointType::Distance: {
            glm::vec3 d = worldAnchorB - worldAnchorA;
            const float len = glm::length(d);
            currentPosition = len;
            // 两点重合：方向无定义 → 用 +Z（确定性，不取随机方向）
            d = (len < kEpsilon) ? glm::vec3(0.0f, 0.0f, 1.0f) : d / len;
            // ⚠ bias 用"当前长度 − 目标长度"这个**标量**误差，而不是向量差：
            //   后者会把垂直于连线方向的偏移也算成伸缩，于是关节会试图
            //   把两个锚点按误差方向平移（表现为"绳子自己往侧面拽"）。
            pushLinear(d, -beta * (len - distance), true);
            break;
        }
    }
}

void Joint::solveVelocity(RigidBody& A, RigidBody& B) {
    for (int i = 0; i < rowCount; ++i) {
        JointRow& row = rows[i];
        if (row.mass <= 0.0f) continue;

        // 约束方向的相对速度
        float cdot;
        if (row.angular) {
            cdot = glm::dot(B.angularVelocity - A.angularVelocity, row.n);
        } else {
            const glm::vec3 vA =
                A.linearVelocity + glm::cross(A.angularVelocity, row.rA);
            const glm::vec3 vB =
                B.linearVelocity + glm::cross(B.angularVelocity, row.rB);
            cdot = glm::dot(vB - vA, row.n);
        }

        float lambda = row.mass * (row.bias - cdot);
        const float old = row.impulse;
        // 等式行（lower/upper = ±inf）时 clamp 是空操作，累积冲量可以自由
        // 取负 —— 那正是"拉"的能力。限位行则被 clamp 成单边。
        const float nw = glm::clamp(old + lambda, row.lower, row.upper);
        lambda = nw - old;
        row.impulse = nw;
        if (lambda == 0.0f) continue;

        if (row.angular) {
            // 角行：力臂为 0，所以**没有**线性分量，冲量全部落在角速度上。
            const glm::vec3 angImp = row.n * lambda;
            A.angularVelocity -= A.invInertiaWorld() * angImp;
            B.angularVelocity += B.invInertiaWorld() * angImp;
        } else {
            const glm::vec3 imp = row.n * lambda;
            A.linearVelocity -= imp * A.massProps.invMass;
            A.angularVelocity -= A.invInertiaWorld() * glm::cross(row.rA, imp);
            B.linearVelocity += imp * B.massProps.invMass;
            B.angularVelocity += B.invInertiaWorld() * glm::cross(row.rB, imp);
        }
    }
}

} // namespace physics
