#pragma once
// ============================================================
// physics/Contact —— 接触点 / 接触流形 / 跨帧接触缓存
//
// ------------------------------------------------------------
// 为什么需要"流形"而不是"一个接触点"
// ------------------------------------------------------------
// 一个箱子平放在地面上，如果只生成**一个**接触点，求解器只能在那一点上
// 加冲量 —— 箱子的另外三个角会陷下去，表现为"箱子慢慢翻倒 / 抖动"。
// 四个角各有一个接触点，法向冲量才能组成一个**支撑多边形**，箱子才躺得稳。
//
// 所以窄相的输出形态是 ContactManifold（最多 4 个点 + 一个公共法线），
// 这也是业界（Bullet 的 btPersistentManifold / PhysX 的 ContactBuffer）
// 的共同设计。
//
// ------------------------------------------------------------
// 热启动（warm starting）与 featureId
// ------------------------------------------------------------
// 顺序冲量求解器每帧从 0 开始迭代。如果每帧都重新"从零找冲量"，那么：
//   · 需要更多迭代次数才能收敛（接触约束之间互相干扰）；
//   · 迭代次数不够时，重物会缓慢下沉（每个子步只补回一部分穿透）。
//
// 热启动 = **把上一帧的冲量当作本帧的初值**。关键在于"上一帧的哪个
// 点对应这一帧的哪个点"：靠 featureId 匹配。
//
// featureId 是"这个接触点来自两个形状的哪一对特征"（盒盒就是参考面 ×
// 入射面的顶点配对）。同一对盒子持续接触时，featureId 稳定 → 冲量能继承；
// 一旦换了接触特征（比如箱子翻转了），featureId 变了 → 冲量归零重来，
// 不会把错误方向的冲量带过去。
//
// featureId == 0 表示"无法稳定标识"（球、GJK/EPA 回退路径）→ 不做热启动。
// 这是**安全的退化**：结果仍然正确，只是收敛稍慢。
// ============================================================

#include "physics/PhysicsMath.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace physics {

// 求解器实际使用的上限。稳定的堆叠只需要 4 个支撑点，多于 4 个对
// 收敛没有帮助（只是让每帧多迭代几遍），所以热启动/求解阶段一律按 4 个走。
constexpr int kMaxManifoldPoints = 4;
// 窄相的**工作容量**：盒-盒裁剪中途可能产出 6~8 个点，最后再"取最深的 4 个"。
// 如果这里也开 4，addPoint 就会静默丢点（丢掉的可能正好是最深的那个角）。
constexpr int kMaxManifoldCapacity = 8;

struct ContactPoint {
    glm::vec3 position{0.0f};   // 世界空间接触点（A、B 表面点的中点）
    float depth = 0.0f;         // 穿透深度（> 0 = 已经嵌进去）

    // 跨帧匹配用。0 = 不稳定（不参与热启动）。
    std::uint64_t featureId = 0;

    // ---- 求解器的工作区（每步清零再由迭代填）----
    glm::vec3 rA{0.0f};         // 接触点相对 A 质心的臂
    glm::vec3 rB{0.0f};
    float normalMass = 0.0f;    // 法向有效质量（1/(J M⁻¹ Jᵀ)）
    float tangentMass[2] = {0.0f, 0.0f};
    float normalImpulse = 0.0f;          // 累积法向冲量（热启动的载体）
    float tangentImpulse[2] = {0.0f, 0.0f};
    glm::vec3 tangent[2]{glm::vec3(1.0f, 0.0f, 0.0f),
                         glm::vec3(0.0f, 1.0f, 0.0f)};

    // ---- 求解器预计算出来的两个"目标速度" ----
    // velocityBias：本步要额外达到的分离速度（弹性反弹量），单位 m/s。
    float velocityBias = 0.0f;
    // positionImpulse：**位置修正通道**累积的冲量（split impulse 用）。
    // 它与 normalImpulse 分开记：位置修正只改位置、不改真实速度，所以两套
    // 冲量必须各攒各的，否则"把物体推出来"的那部分能量会留在速度里。
    float positionImpulse = 0.0f;
};

struct ContactManifold {
    std::uint32_t bodyA = 0;
    std::uint32_t bodyB = 0;
    // 法线方向：**从 A 指向 B**（和 ConvexCollision::Contact 的约定一致）。
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    int count = 0;
    ContactPoint points[kMaxManifoldCapacity];

    // 组合后的材质系数（窄相算一次，求解器直接用）
    float friction = 0.4f;
    float restitution = 0.0f;
};

// ---------------------------------------------------------------- 缓存

// 跨帧接触缓存：pair key → 上一帧的流形。
//
// 用 unordered_map 而不是"按 body 下标开二维数组"：场景里绝大多数体对
// 从不接触，二维数组在几百个刚体时是几万个空槽，而实际接触对只有几十个。
class ContactCache {
public:
    void beginFrame() { m_newFrame.clear(); }

    // 取上一帧同一对体的流形（没有则 nullptr）
    const ContactManifold* find(std::uint32_t a, std::uint32_t b) const {
        const auto it = m_prev.find(key(a, b));
        return (it == m_prev.end()) ? nullptr : &it->second;
    }

    // 本帧结束前把本帧的流形存成"上一帧"
    void commit(std::vector<ContactManifold>& manifolds) {
        m_newFrame.clear();
        for (ContactManifold& m : manifolds) {
            // 没有接触点的流形不必留——留着只会让 map 无限膨胀
            if (m.count == 0) continue;
            m_newFrame[key(m.bodyA, m.bodyB)] = m;
        }
        m_prev.swap(m_newFrame);
    }

    void clear() {
        m_prev.clear();
        m_newFrame.clear();
    }

    std::size_t size() const { return m_prev.size(); }

private:
    // 打包成 64 位键。两个 32 位下标拼起来，避免用 pair 当 key
    //（那需要自定义 hash，而这里 a<b 已经归一化，直接拼就唯一）。
    static std::uint64_t key(std::uint32_t a, std::uint32_t b) {
        return (static_cast<std::uint64_t>(a) << 32) |
               static_cast<std::uint64_t>(b);
    }

    std::unordered_map<std::uint64_t, ContactManifold> m_prev;
    std::unordered_map<std::uint64_t, ContactManifold> m_newFrame;
};

} // namespace physics
