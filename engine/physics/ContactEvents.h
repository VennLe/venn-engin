#pragma once
// ============================================================
// physics/ContactEvents —— 接触 / 触发事件的跨帧差分（G08）
//
// 求解器每步都会重算"这一刻有哪些接触对"，但它**不记历史**：游戏逻辑要的
// 是"刚刚撞上"（Enter）、"还在接触"（Stay）、"离开了"（Exit）这三种
// **边沿语义**。本模块就是那层差分。
//
// ------------------------------------------------------------
// 键为什么用 (entityA, entityB) 的排序对
// ------------------------------------------------------------
// 接触对是**无序**的（A 撞 B 与 B 撞 A 是同一件事）。键取
// `(min << 32) | max` 之后，无论求解器先报告谁，键都一样 —— 否则同一对
// 物体会在"AB"和"BA"之间反复跳，每步都产出一次 Enter + Exit。
//
// ⚠ 这里用的是**实体 id**，它在一次 Play 内稳定、跨存档不稳定。G16 引入
//   稳定 GUID 之后，这把键会换成 GUID（接口不变，只换 key 的来源）。
//
// ------------------------------------------------------------
// 触发体为什么不走 manifolds
// ------------------------------------------------------------
// 窄相在遇到 `kBodyIsTrigger` 时**直接返回无接触**（见 Narrowphase.cpp：
// 触发器不该产生冲量，最省事的做法就是根本不生成流形）。所以触发体
// 的重叠必须单独测一遍 —— 用同一套 `collide()`，只是不看结果里的冲量。
// ============================================================

#include "ecs/Entity.h"
#include "physics/RigidBody.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace physics {

class PhysicsWorld;

enum class ContactPhase : int { Enter = 0, Stay = 1, Exit = 2 };

const char* contactPhaseName(ContactPhase p);

struct ContactEvent {
    ecs::EntityId a = ecs::kInvalidEntity;
    ecs::EntityId b = ecs::kInvalidEntity;
    ContactPhase phase = ContactPhase::Enter;
    bool trigger = false;      // 这一对里至少有一个是触发体
    glm::vec3 normal{0.0f, 0.0f, 1.0f};   // a → b
    float depth = 0.0f;        // 最大穿透深度（Exit 时为 0）
    int stay = 0;              // 连续接触步数（Enter = 1）
};

// 每**固定步**调用一次（和物理同频）。事件顺序按 (a,b) 排序，保证
// 同一场景跑两遍逐行相同 —— 验证脚本会逐字符比对。
class ContactTracker {
public:
    void setTrace(bool on) { m_trace = on; }
    void reset();

    // 采集本步事件（追加到 out，不清空 —— 调用方每步自己清）
    void collect(const PhysicsWorld& world, std::vector<ContactEvent>& out);

    std::size_t activePairs() const { return m_cur.size(); }
    std::uint64_t totalEvents() const { return m_total; }
    // 累计的 Enter / Exit 次数（自检与日志用）
    std::uint64_t enters() const { return m_enters; }
    std::uint64_t exits() const { return m_exits; }

private:
    struct PairInfo {
        int stay = 1;
        bool trigger = false;
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
        float depth = 0.0f;
    };

    std::unordered_map<std::uint64_t, PairInfo> m_prev;
    std::unordered_map<std::uint64_t, PairInfo> m_cur;

    std::uint64_t m_total = 0;
    std::uint64_t m_enters = 0;
    std::uint64_t m_exits = 0;
    bool m_trace = false;
};

// 把一对实体 id 归一化成稳定键
std::uint64_t contactPairKey(ecs::EntityId a, ecs::EntityId b);

// ------------------------------------------------------------ 自检
// 无窗口：穿孔触发盒，断言 Enter / Stay×N / Exit 各一次；并断言触发体
// 不改变动态体的轨迹（逐位）。
// 由 `Game.exe --selftest contact` 调用。
int runContactSelfTest();

} // namespace physics
