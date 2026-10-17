#include "physics/Solver.h"

#include <algorithm>
#include <cmath>

namespace physics {

namespace {

// 从法线构造一组正交切向基。
//
// 分支判据用 |n.x| > 0.577（= 1/√3）：三个分量里最大的那个当"被替换轴"，
// 这样叉积的结果不会退化成零向量。如果无条件用 cross(n, (0,0,1))，那么
// **法线正好是 ±Z**（地面上所有物体都是！）时叉积恒为 0 → 切向基塌掉 →
// 摩擦完全失效。这是最容易踩的一个坑。
void tangentBasis(const glm::vec3& n, glm::vec3& t1, glm::vec3& t2) {
    if (std::fabs(n.x) >= 0.57735027f) {
        const float len = std::sqrt(n.y * n.y + n.z * n.z);
        t1 = glm::vec3(0.0f, -n.z, n.y) / std::max(len, kEpsilon);
    } else {
        const float len = std::sqrt(n.x * n.x + n.z * n.z);
        t1 = glm::vec3(-n.z, 0.0f, n.x) / std::max(len, kEpsilon);
    }
    // 再正交化一次：上面的构造在数值上已经有 1e-7 量级的偏差，
    // 而"切向必须严格垂直于法线"是摩擦不发散的前提。
    t1 = glm::normalize(t1 - n * glm::dot(n, t1));
    t2 = glm::cross(n, t1);
}

// 约束方向 dir 上的有效质量倒数：k = invMassA + invMassB
//                                     + (rA×d)ᵀ·IA⁻¹·(rA×d) + (rB×d)ᵀ·IB⁻¹·(rB×d)
//
// 后两项是**转动惯量对约束的贡献**：碰到离质心远的点，物体更"容易"绕开，
// 所以有效质量更小。漏掉它的话，长条物体被压住一端时会像"钉子钉住"一样
// 不动（因为没有角响应）。
float effectiveMassInv(const glm::vec3& dir, const glm::vec3& rA,
                       const glm::vec3& rB, float invMassA, float invMassB,
                       const glm::mat3& iA, const glm::mat3& iB) {
    const glm::vec3 cA = glm::cross(rA, dir);
    const glm::vec3 cB = glm::cross(rB, dir);
    return invMassA + invMassB + glm::dot(cA, iA * cA) + glm::dot(cB, iB * cB);
}

glm::vec3 velocityAt(const RigidBody& b, const glm::vec3& r) {
    return b.linearVelocity + glm::cross(b.angularVelocity, r);
}

// 施加冲量（沿 dir 的 λ）：B 加 +，A 加 −。写成一处，避免 6 个调用点
// 各自抄一遍符号（抄错一次就是"物体被吸向地面"这种诡异 bug）。
inline void applyImpulse(RigidBody& A, RigidBody& B, const glm::vec3& rA,
                         const glm::vec3& rB, const glm::vec3& dir, float lambda,
                         const glm::mat3& iA, const glm::mat3& iB) {
    const glm::vec3 imp = dir * lambda;
    A.linearVelocity -= imp * A.invMassEff();
    A.angularVelocity -= iA * glm::cross(rA, imp);
    B.linearVelocity += imp * B.invMassEff();
    B.angularVelocity += iB * glm::cross(rB, imp);
}

constexpr float kMaxBiasVelocity = 4.0f;

} // namespace

void Solver::solve(std::vector<RigidBody>& bodies, const ContactCache& cache,
                   std::vector<ContactManifold>& manifolds,
                   std::vector<Joint>* joints, const SolverSettings& settings,
                   float dt) {
    m_stats = SolverStats{};
    if (dt <= 0.0f) return;
    const float invDt = 1.0f / dt;

    // ================================================================
    //  1) 预计算 + 热启动
    // ================================================================
    for (ContactManifold& m : manifolds) {
        if (m.count == 0) continue;
        RigidBody& A = bodies[m.bodyA];
        RigidBody& B = bodies[m.bodyB];

        const float invMassA = A.invMassEff();
        const float invMassB = B.invMassEff();
        const glm::mat3 iA = A.invInertiaWorld();
        const glm::mat3 iB = B.invInertiaWorld();

        // 上一帧同一对体的流形（热启动的来源）
        const ContactManifold* prev =
            cache.find(std::min(m.bodyA, m.bodyB), std::max(m.bodyA, m.bodyB));

        for (int i = 0; i < m.count; ++i) {
            ContactPoint& p = m.points[i];

            // ---- 热启动：按 featureId 从上一帧继承冲量 ----
            p.normalImpulse = 0.0f;
            p.tangentImpulse[0] = 0.0f;
            p.tangentImpulse[1] = 0.0f;
            p.positionImpulse = 0.0f;
            if (prev && p.featureId != 0) {
                for (int k = 0; k < prev->count; ++k) {
                    if (prev->points[k].featureId != p.featureId) continue;
                    p.normalImpulse = prev->points[k].normalImpulse;
                    p.tangentImpulse[0] = prev->points[k].tangentImpulse[0];
                    p.tangentImpulse[1] = prev->points[k].tangentImpulse[1];
                    break;
                }
            }

            // ---- 接触臂与有效质量 ----
            p.rA = p.position - A.position;
            p.rB = p.position - B.position;

            const float kn = effectiveMassInv(m.normal, p.rA, p.rB, invMassA,
                                              invMassB, iA, iB);
            p.normalMass = (kn > kEpsilon) ? 1.0f / kn : 0.0f;

            tangentBasis(m.normal, p.tangent[0], p.tangent[1]);
            for (int t = 0; t < 2; ++t) {
                const float kt = effectiveMassInv(p.tangent[t], p.rA, p.rB,
                                                  invMassA, invMassB, iA, iB);
                p.tangentMass[t] = (kt > kEpsilon) ? 1.0f / kt : 0.0f;
            }

            // ---- 弹性：只用**接触刚开始时**的相对速度决定反弹量 ----
            //
            // 这里读的是"还没施加任何冲量"的速度。如果放在迭代里读，
            // 每轮的速度都不一样 → 反弹量随迭代次数变，物理就不确定了。
            // 阈值是为了让慢速接触不弹（否则静置的球会永远微跳）。
            p.velocityBias = 0.0f;
            if (m.restitution > 0.0f) {
                const glm::vec3 vRel =
                    velocityAt(B, p.rB) - velocityAt(A, p.rA);
                const float vn = glm::dot(vRel, m.normal);
                if (vn < -settings.restitutionThreshold) {
                    p.velocityBias = -m.restitution * vn;
                }
            }

            // ---- 施加继承来的冲量 ----
            // ⚠ warmStartScale 只乘**冲量**一次，不要再乘到 invMass 上
            //   （那样就平方了，等于把热启动效果变成 0.01）。
            if (p.normalImpulse != 0.0f || p.tangentImpulse[0] != 0.0f ||
                p.tangentImpulse[1] != 0.0f) {
                const glm::vec3 imp =
                    m.normal * (p.normalImpulse * settings.warmStartScale) +
                    p.tangent[0] * (p.tangentImpulse[0] * settings.warmStartScale) +
                    p.tangent[1] * (p.tangentImpulse[1] * settings.warmStartScale);
                A.linearVelocity -= imp * invMassA;
                A.angularVelocity -= iA * glm::cross(p.rA, imp);
                B.linearVelocity += imp * invMassB;
                B.angularVelocity += iB * glm::cross(p.rB, imp);
            }

            m_stats.maxPenetration = std::max(m_stats.maxPenetration, p.depth);
        }
    }

    // ================================================================
    //  2) 速度迭代
    // ================================================================
    //
    // 顺序：**先摩擦后法向**。反过来的话，法向冲量会在这一轮里先把两物体
    // 推开，摩擦用的"正压力"（normalImpulse）就是上一轮的值 —— 摩擦会
    // 系统性偏小。先摩擦能用到**本轮**刚更新的正压力，止滑更干净。
    //
    // 每次迭代都重新取 invInertiaWorld：因为**同一轮里前面的接触已经改过
    // 角速度**，而惯量随姿态变 —— 一帧之内姿态不变，所以每帧取一次就够。
    // 这里为了简单直接放在循环外（见上面的 iA / iB）。
    for (int iter = 0; iter < settings.velocityIterations; ++iter) {
        for (ContactManifold& m : manifolds) {
            if (m.count == 0) continue;
            RigidBody& A = bodies[m.bodyA];
            RigidBody& B = bodies[m.bodyB];
            const float invMassA = A.invMassEff();
            const float invMassB = B.invMassEff();
            const glm::mat3 iA = A.invInertiaWorld();
            const glm::mat3 iB = B.invInertiaWorld();

            for (int i = 0; i < m.count; ++i) {
                ContactPoint& p = m.points[i];

                // ---- 摩擦 ----
                if (settings.enableFriction && m.friction > 0.0f) {
                    for (int t = 0; t < 2; ++t) {
                        const glm::vec3 vRel =
                            velocityAt(B, p.rB) - velocityAt(A, p.rA);
                        const float vt = glm::dot(vRel, p.tangent[t]);
                        float lambda = -p.tangentMass[t] * vt;

                        // 库仑锥：|Ft| ≤ μ·Fn。用**当前累积的**法向冲量当
                        // 正压力 —— 所以摩擦天然是"压得越紧越难滑"。
                        const float maxFriction = m.friction * p.normalImpulse;
                        const float old = p.tangentImpulse[t];
                        const float nw =
                            glm::clamp(old + lambda, -maxFriction, maxFriction);
                        lambda = nw - old;
                        p.tangentImpulse[t] = nw;

                        if (lambda != 0.0f) {
                            const glm::vec3 imp = p.tangent[t] * lambda;
                            A.linearVelocity -= imp * invMassA;
                            A.angularVelocity -= iA * glm::cross(p.rA, imp);
                            B.linearVelocity += imp * invMassB;
                            B.angularVelocity += iB * glm::cross(p.rB, imp);
                        }
                    }
                }

                // ---- 法向 ----
                {
                    const glm::vec3 vRel =
                        velocityAt(B, p.rB) - velocityAt(A, p.rA);
                    const float vn = glm::dot(vRel, m.normal);

                    // 目标：vn 至少达到 velocityBias（弹性）。
                    // splitImpulse 打开时位置修正不在这里做（见第 4 步），
                    // 关闭时退回经典 Baumgarte：把偏置加进真实速度。
                    float bias = p.velocityBias;
                    if (!settings.splitImpulse) {
                        const float pen =
                            std::max(0.0f, p.depth - settings.slop);
                        bias += std::min(settings.baumgarte * pen * invDt,
                                         kMaxBiasVelocity);
                    }

                    float lambda = p.normalMass * (bias - vn);
                    // 累积冲量必须 ≥ 0：接触只能"推"不能"拉"。
                    // 少了这个 clamp，物体会被**吸**在一起（经典 bug）。
                    const float old = p.normalImpulse;
                    const float nw = std::max(old + lambda, 0.0f);
                    lambda = nw - old;
                    p.normalImpulse = nw;

                    if (lambda != 0.0f) {
                        applyImpulse(A, B, p.rA, p.rB, m.normal, lambda, iA, iB);
                        m_stats.totalNormalImpulse += lambda;
                    }
                }
            }
        }

        // ---- 关节：与接触**交替**求解 ----
        // 放在接触之后：接触是"硬约束"（不能穿透），关节是"软约束"（拉回来
        // 就行）。先满足硬的，再由关节在自己的式子里把误差收掉，整叠结构
        // 的收敛顺序更接近直觉（地面先顶住，链条再被拉直）。
        if (joints) {
            for (Joint& j : *joints) {
                j.solveVelocity(bodies[j.bodyA], bodies[j.bodyB]);
            }
        }
    }

    // ================================================================
    //  3) 速度积分（半隐式欧拉：先速度后位置）
    // ================================================================
    for (RigidBody& b : bodies) {
        if (b.isStatic()) continue;
        if (b.isKinematic()) {
            // 运动学体：速度由外部给定（平台 / 动画驱动），这里只推进位置。
            // 它**不参与休眠判定** —— 一个正在移动的平台必须每帧都动。
            b.integratePosition(dt);
            continue;
        }
        // 休眠体：完全跳过（速度已被清零，位置不该动）。
        if (!b.awake) continue;
        // 速度上限（防爆）。放在**位置积分之前**，否则这一步已经用
        // 巨大的速度把物体送到天外了。
        const float lin2 = glm::dot(b.linearVelocity, b.linearVelocity);
        const float maxLin2 = settings.maxLinearVelocity *
                              settings.maxLinearVelocity;
        if (lin2 > maxLin2) {
            b.linearVelocity *= settings.maxLinearVelocity / std::sqrt(lin2);
        }
        const float ang2 = glm::dot(b.angularVelocity, b.angularVelocity);
        const float maxAng2 = settings.maxAngularVelocity *
                              settings.maxAngularVelocity;
        if (ang2 > maxAng2) {
            b.angularVelocity *= settings.maxAngularVelocity / std::sqrt(ang2);
        }
        b.integratePosition(dt);
        m_stats.maxSpeed = std::max(m_stats.maxSpeed,
                                    glm::length(b.linearVelocity));
    }

    // ================================================================
    //  4) 位置修正（split impulse）
    // ================================================================
    //
    // 用一个**独立的伪速度场**去消除穿透。伪速度只用来改位置，算完就丢 ——
    // 所以"把物体推出来"不会给真实速度注入能量（这就是它比 Baumgarte 好的
    // 地方：一叠箱子静置时不会因为一直互相推而轻微抖动）。
    // 不关闭 splitImpulse 时这一步不跑。
    if (settings.splitImpulse && settings.positionIterations > 0) {
        const std::size_t n = bodies.size();
        m_pseudoLin.assign(n, glm::vec3(0.0f));
        m_pseudoAng.assign(n, glm::vec3(0.0f));

        for (int iter = 0; iter < settings.positionIterations; ++iter) {
            for (ContactManifold& m : manifolds) {
                if (m.count == 0) continue;
                RigidBody& A = bodies[m.bodyA];
                RigidBody& B = bodies[m.bodyB];
                const float invMassA = A.invMassEff();
                const float invMassB = B.invMassEff();
                if (invMassA <= 0.0f && invMassB <= 0.0f) continue;
                const glm::mat3 iA = A.invInertiaWorld();
                const glm::mat3 iB = B.invInertiaWorld();

                for (int i = 0; i < m.count; ++i) {
                    ContactPoint& p = m.points[i];
                    // 目标：这一步把穿透消到只剩 slop。
                    const float pen = std::max(0.0f, p.depth - settings.slop);
                    const float target =
                        std::min(settings.baumgarte * pen, settings.maxCorrection);
                    if (target <= 0.0f) continue;

                    const glm::vec3 vRel =
                        (m_pseudoLin[m.bodyB] +
                         glm::cross(m_pseudoAng[m.bodyB], p.rB)) -
                        (m_pseudoLin[m.bodyA] +
                         glm::cross(m_pseudoAng[m.bodyA], p.rA));
                    const float vn = glm::dot(vRel, m.normal);

                    // 希望伪速度沿法线达到 target/dt（= 这一步走完 target 米）
                    float lambda = p.normalMass * (target * invDt - vn);
                    const float old = p.positionImpulse;
                    const float nw = std::max(old + lambda, 0.0f);
                    lambda = nw - old;
                    p.positionImpulse = nw;
                    if (lambda == 0.0f) continue;

                    const glm::vec3 imp = m.normal * lambda;
                    m_pseudoLin[m.bodyA] -= imp * invMassA;
                    m_pseudoAng[m.bodyA] -= iA * glm::cross(p.rA, imp);
                    m_pseudoLin[m.bodyB] += imp * invMassB;
                    m_pseudoAng[m.bodyB] += iB * glm::cross(p.rB, imp);
                }
            }
        }

        // 把伪速度积分成位置修正
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            RigidBody& b = bodies[i];
            if (!b.movable() || !b.awake) continue;
            b.position += m_pseudoLin[i] * dt;
            b.rotation = integrateRotation(b.rotation, m_pseudoAng[i], dt);
        }
    }

    // ================================================================
    //  5) 统计
    // ================================================================
    m_stats.manifolds = 0;
    m_stats.points = 0;
    for (const ContactManifold& m : manifolds) {
        if (m.count == 0) continue;
        ++m_stats.manifolds;
        m_stats.points += m.count;
    }
    m_stats.velocityIterations = settings.velocityIterations;
}

} // namespace physics
