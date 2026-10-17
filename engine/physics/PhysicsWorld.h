#pragma once
// ============================================================
// physics/PhysicsWorld —— 仿真世界（本模块唯一的门面）
//
// 它按**固定步长**推进一整套刚体动力学：
//
//   重力/外力 → 宽相(SAP) → 窄相(流形) → 顺序冲量(接触+关节)
//            → 位置修正 → 休眠判定 → 写回变换
//
// ------------------------------------------------------------
// 为什么必须是固定步长（而不是"每帧用真实 dt 走一步"）
// ------------------------------------------------------------
// 三个理由，任何一个都足以定案：
//
//   1. **复现性**：物理是迭代求解器，步长变了结果就变。验证脚本要能说
//      "第 120 步时的高度是 x.xxxx"，就必须让"第 120 步"有确定含义。
//   2. **稳定性**：显式积分在 dt 过大时会发散（穿透 → 更大速度 → 更深
//      穿透）。固定小步长把 dt 钉死在安全区，与帧率解耦。
//   3. **穿透量**：`穿透深度 ≈ 速度 × dt`。60 fps 的 30 m/s 子弹一步走
//      0.5 m —— 固定 1/60 步长 + 子步累加器让快速物体仍然每 16.7 ms 检查
//      一次，而不是"这一帧跳了 0.1 秒就跳过整面墙"。
//
// 累加器（accumulator）的写法：把真实经过的时间攒起来，每攒够一个
// fixedTimeStep 就跑一步，最多跑 maxSubSteps 步（**防止"死亡螺旋"**：
// 帧率掉下去 → 需要更多子步 → 更慢 → 掉得更狠。超出上限时直接丢掉
// 剩余时间，物理"变慢"但不会把整机拖死）。
//
// ------------------------------------------------------------
// 休眠（sleeping）
// ------------------------------------------------------------
// 完全静止的刚体不值得每帧求解。判定很简单：线速度与角速度都低于阈值
// 并持续 `sleepTime` 秒 → 标记 asleep（速度清零，逆质量视作 0，见
// RigidBody::invMassEff）。被碰到的瞬间由 WakeIslands 唤醒。
//
// 为什么这件事不能省：一叠静置的箱子如果永远在求解，浮点误差会让它们
// 一直有 1e-3 m/s 量级的抖动，而**每帧的抖动都会通过接触传给整叠** ——
// 最后看起来就是"整堆东西在轻轻嗡"。休眠把"已经静止"这件事冻结住。
// ============================================================

#include "physics/Broadphase.h"
#include "physics/Contact.h"
#include "physics/Joints.h"
#include "physics/RigidBody.h"
#include "physics/Solver.h"

#include <cstdint>
#include <vector>

namespace physics {

struct PhysicsWorldSettings {
    glm::vec3 gravity{0.0f, 0.0f, -9.81f};
    // 固定步长。1/60 是默认；追求精度可到 1/120（代价是两倍计算量）。
    float fixedTimeStep = 1.0f / 60.0f;
    // 单帧最多跑几个子步（防死亡螺旋，见文件头）
    int maxSubSteps = 4;
    // 单帧最多推进多少物理时间（秒）。超过就丢弃 —— 长卡顿之后不该
    // 让物理"追赶"到几秒钟之后的状态。
    float maxFrameTime = 0.25f;

    // ---- 求解器 ----
    int velocityIterations = 8;
    int positionIterations = 3;
    float baumgarte = 0.2f;
    float slop = 0.005f;
    float restitutionThreshold = 1.0f;
    bool enableFriction = true;
    bool splitImpulse = true;
    float maxLinearVelocity = 200.0f;
    float maxAngularVelocity = 100.0f;

    // ---- 休眠 ----
    bool allowSleep = true;
    float sleepLinearThreshold = 0.05f;    // m/s
    float sleepAngularThreshold = 0.15f;   // rad/s
    float sleepTime = 0.5f;                // 秒

    // ---- 宽相 ----
    float broadphaseMargin = 0.02f;        // 预测性接触的 AABB 外扩（米）
};

struct PhysicsStats {
    int bodies = 0;
    int dynamic = 0;
    int kinematic = 0;
    int statics = 0;
    int awake = 0;
    int asleep = 0;
    int joints = 0;
    int manifolds = 0;
    int contactPoints = 0;
    int broadphasePairs = 0;
    float maxPenetration = 0.0f;
    float maxSpeed = 0.0f;
    std::uint64_t steps = 0;      // 累计固定步数
    float simTime = 0.0f;         // 仿真时间（= steps × fixedTimeStep）
};

struct RayHit {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    BodyId body = kInvalidBody;
    std::uint32_t entity = 0xFFFFFFFFu;
};

class PhysicsWorld {
public:
    // ------------------------------------------------ 刚体
    BodyId addBody(RigidBody body);
    // 移除（会把下标引用它的关节一并清掉，避免悬垂索引）
    void removeBody(BodyId id);
    void clear();

    RigidBody* body(BodyId id) {
        return (id < m_bodies.size() && m_bodies[id].id == id) ? &m_bodies[id]
                                                               : nullptr;
    }
    const RigidBody* body(BodyId id) const {
        return (id < m_bodies.size() && m_bodies[id].id == id) ? &m_bodies[id]
                                                               : nullptr;
    }
    std::size_t bodyCount() const { return m_bodies.size(); }
    std::vector<RigidBody>& bodies() { return m_bodies; }
    const std::vector<RigidBody>& bodies() const { return m_bodies; }
    // BodyId 就等于下标（见 addBody），所以外部可以直接按下标访问。

    // ------------------------------------------------ 关节
    std::uint32_t addJoint(const Joint& j);
    std::vector<Joint>& joints() { return m_joints; }
    const std::vector<Joint>& joints() const { return m_joints; }

    // ------------------------------------------------ 步进
    // 用**真实经过的时间**推进（累加器 + 固定步长）。返回本帧跑的步数。
    int step(float realDt);
    // 精确推进一个固定步长（验证脚本用；不做累加、不丢弃）。
    void stepFixed(float dt);

    // ------------------------------------------------ 查询
    // 射线检测。dir 会被归一化；maxDist < 0 = 无限远。
    RayHit raycast(const glm::vec3& origin, const glm::vec3& dir,
                   float maxDist = -1.0f, BodyId ignore = kInvalidBody) const;
    // 与某点重叠的所有体（球半径 r）
    void overlapSphere(const glm::vec3& center, float radius,
                       std::vector<BodyId>& out) const;
    // 该点是否落在某个**实体**内部（触发器等都可以用）
    BodyId bodyAtPoint(const glm::vec3& p) const;

    // ------------------------------------------------ 唤醒
    void wakeAll();
    void sleepBody(BodyId id);
    void wakeBody(BodyId id);
    // 手动摆放（会清速度 + 唤醒）
    void setTransform(BodyId id, const glm::vec3& pos, const glm::quat& rot);

    // ------------------------------------------------ 角色控制器
    //
    // 把一根胶囊从 from 沿 delta 推过去，遇到障碍就**贴着表面滑**
    // （而不是停住）。返回最终位置。
    //
    // 做法（不需要单独的 sweep 算法）：
    //   1. 把整段位移切成若干子步（每步不超过半个半径，防穿透）
    //   2. 每一步先移动，再用 collide() 对附近每个体做"最小平移"把胶囊
    //      推出穿透；推出的方向里**去掉与运动方向相反的分量** → 自然滑
    //   3. 竖直方向单独做一次"贴地"（把残余的向下位移消化掉，形成台阶
    //      与斜坡上的稳定站立）
    struct CharacterMove {
        glm::vec3 position{0.0f};
        bool hitWall = false;
        bool onGround = false;
        float groundNormalZ = 0.0f;
    };
    CharacterMove moveCharacter(const glm::vec3& from, const glm::vec3& delta,
                                float radius, float halfHeight,
                                BodyId ignore = kInvalidBody) const;

    // ------------------------------------------------ 设置 / 统计
    PhysicsWorldSettings& settings() { return m_settings; }
    const PhysicsWorldSettings& settings() const { return m_settings; }
    const PhysicsStats& stats() const { return m_stats; }
    const Solver& solver() const { return m_solver; }
    // 上一步的接触流形（可视化 / 调试用）
    const std::vector<ContactManifold>& manifolds() const {
        return m_manifolds;
    }
    void refreshStats();

private:
    void updateAabbs();
    void collectContacts();
    void wakeIslands();
    void updateSleep(float dt);

    PhysicsWorldSettings m_settings;

    // BodyId == 下标。不做"删除后复用槽位"：物理体是在 Play 开始时
    // 一次性建好的，运行中增删很少见；用下标当 id 让查询/调试都最简单。
    // removeBody 会把槽位标记成 dead（id 置 kInvalidBody），不压缩数组
    //（压缩会让所有关节的 bodyA/bodyB 失效）。
    std::vector<RigidBody> m_bodies;
    std::vector<Joint> m_joints;

    Broadphase m_broadphase;
    ContactCache m_cache;
    Solver m_solver;
    std::vector<ContactManifold> m_manifolds;
    // 休眠判定的并查集父数组（按接触图分岛）。作为成员是为了**跨帧复用
    // 容量**，避免每帧在 updateSleep 里重新分配 —— 它每帧都被完整重置，
    // 不携带任何状态。
    std::vector<std::size_t> m_islandParent;

    float m_accumulator = 0.0f;
    PhysicsStats m_stats;
};

} // namespace physics
