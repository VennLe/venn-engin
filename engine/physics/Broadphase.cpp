#include "physics/Broadphase.h"

#include <algorithm>

namespace physics {

void Broadphase::rebuild(const std::vector<RigidBody>& bodies, float margin) {
    m_pairs.clear();
    m_order.clear();
    m_planes.clear();
    m_stats = Stats{};

    const float m = std::max(margin, 0.0f);

    // ---- 1) 分拣：平面走一对多，其余进 SAP ----
    for (std::uint32_t i = 0; i < bodies.size(); ++i) {
        const RigidBody& b = bodies[i];
        if (!b.shape.valid()) continue;
        if (b.shape.isPlane()) {
            m_planes.push_back(i);
            continue;
        }
        m_order.push_back(i);
    }
    m_stats.bodyCount = static_cast<std::uint32_t>(m_order.size());
    m_stats.planeCount = static_cast<std::uint32_t>(m_planes.size());

    // 平面 × 可动体。静态体不进（静态碰静态永远无解、也无意义）。
    for (std::uint32_t pi : m_planes) {
        for (std::uint32_t bi : m_order) {
            if (!bodies[bi].movable()) continue;
            m_pairs.push_back(BroadphasePair{std::min(pi, bi),
                                             std::max(pi, bi)});
        }
    }

    // ---- 2) 按 AABB 的 min 排序（x 轴为主轴）----
    //
    // 主轴选 x 而不是 z（虽然世界是 Z-up）：SAP 的效率取决于**端点沿主轴
    // 的分散程度**。重力让所有物体的 z 都挤在同一条带上（地面附近），
    // 而 x/y 是自由散布的 —— 所以主轴不该选重力轴。
    std::sort(m_order.begin(), m_order.end(),
              [&](std::uint32_t a, std::uint32_t b) {
                  return bodies[a].aabbMin.x < bodies[b].aabbMin.x;
              });

    // ---- 3) 扫描 ----
    for (std::size_t i = 0; i < m_order.size(); ++i) {
        const RigidBody& A = bodies[m_order[i]];
        const glm::vec3 aMin = A.aabbMin - m;
        const glm::vec3 aMax = A.aabbMax + m;

        for (std::size_t j = i + 1; j < m_order.size(); ++j) {
            const RigidBody& B = bodies[m_order[j]];
            const glm::vec3 bMin = B.aabbMin - m;

            // 主轴分离 → 后面只会更远，整段剪掉
            if (bMin.x > aMax.x) break;

            const glm::vec3 bMax = B.aabbMax + m;
            ++m_stats.axisTests;
            if (aMin.y > bMax.y || bMin.y > aMax.y) continue;
            if (aMin.z > bMax.z || bMin.z > aMax.z) continue;

            // 两个都不会动的体没有任何求解意义 → 直接丢
            // （静态-静态在场景里非常常见：地面 + 墙 + 装饰，全部两两配对
            //   会让宽相输出里塞满无用对）。
            const bool aMoves = A.movable();
            const bool bMoves = B.movable();
            if (!aMoves && !bMoves) continue;

            const std::uint32_t ia = m_order[i];
            const std::uint32_t ib = m_order[j];
            m_pairs.push_back(BroadphasePair{std::min(ia, ib),
                                             std::max(ia, ib)});
        }
    }

    // ---- 4) 归一化输出顺序 ----
    //
    // 排序不是为了让算法更快，而是为了**确定性**：同一场景每次运行必须
    // 产生完全相同的对序列，否则求解顺序变化会让接触冲量分配合计值漂移，
    // 验证脚本里"同一场景两次运行逐位相同"就会失败。
    std::sort(m_pairs.begin(), m_pairs.end(),
              [](const BroadphasePair& x, const BroadphasePair& y) {
                  return x.a != y.a ? x.a < y.a : x.b < y.b;
              });
    m_pairs.erase(std::unique(m_pairs.begin(), m_pairs.end(),
                              [](const BroadphasePair& x,
                                 const BroadphasePair& y) {
                                  return x.a == y.a && x.b == y.b;
                              }),
                  m_pairs.end());

    m_stats.pairs = static_cast<std::uint32_t>(m_pairs.size());
}

} // namespace physics
