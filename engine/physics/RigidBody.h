#pragma once
// ============================================================
// physics/RigidBody —— 一个刚体的全部状态
//
// ------------------------------------------------------------
// 坐标系与"原点"的约定（读求解器前必须先记住这条）
// ------------------------------------------------------------
//   · position 是**质心**在世界空间的位置，rotation 是姿态四元数。
//   · 所有内置形状都**以质心为原点**（球心 / 盒心 / 胶囊中点 / 凸包已居中），
//     所以 position 同时就是形状原点 —— 不需要"shape offset"这一层。
//     这一条换来巨大的简化：不需要局部质心偏移的平移-旋转耦合项。
//     （要支持偏心刚体，得在冲量公式里加 r_com 修正；本引擎不做。）
//   · 世界是 Z-up（见 PhysicsMath.h）。
//
// ------------------------------------------------------------
// 三种刚体（BodyType）
// ------------------------------------------------------------
//   Static    —— 质量无穷，永远不动。地面 / 墙 / 场景装饰。只参与碰撞。
//   Kinematic —— 质量无穷，**由外部直接设位置与速度**，求解器不推它，
//                但它的速度参与接触约束（所以能把动态物体推开、传送带能
//                带着箱子走）。动画驱动的平台 / 玩家胶囊用这个。
//   Dynamic   —— 正常受力积分。
//
// 这三类的区别全部落在**逆质量（invMass）与逆惯量**上：静态/运动学是 0，
// 于是冲量公式里所有"除以质量"的项自然变成 0，求解器不需要任何 if。
// 唯一的例外是运动学体要参与"相对速度"，所以积分时按速度积分但不感受力。
// ============================================================

#include "physics/PhysicsMath.h"
#include "physics/PhysicsShape.h"

#include <cstdint>

namespace physics {

using BodyId = std::uint32_t;
constexpr BodyId kInvalidBody = 0xFFFFFFFFu;

enum class BodyType : int {
    Static = 0,
    Kinematic = 1,
    Dynamic = 2,
};

// 逐位开关。做成位域而不是一堆 bool：这些标志经常要"整组"传递
// （序列化、复制、休眠时判断"能不能睡"），一个 uint32 拷起来最省事。
enum BodyFlags : std::uint32_t {
    kBodyUseGravity = 1u << 0,   // 受重力（默认开）
    kBodyCcd = 1u << 1,          // 连续碰撞检测（高速物体防穿透）
    kBodyAllowSleep = 1u << 2,   // 允许休眠（默认开）
    kBodyLockLinX = 1u << 3,     // 锁平移（常用于 2.5D / 摆锤）
    kBodyLockLinY = 1u << 4,
    kBodyLockLinZ = 1u << 5,
    kBodyLockRotX = 1u << 6,     // 锁转动（常用于"角色不倒"）
    kBodyLockRotY = 1u << 7,
    kBodyLockRotZ = 1u << 8,
    kBodyIsTrigger = 1u << 9,    // 只产生接触事件、不产生冲量
};

constexpr std::uint32_t kBodyDefaultFlags =
    kBodyUseGravity | kBodyAllowSleep;

struct RigidBody {
    BodyId id = kInvalidBody;
    // 关联的 ECS 实体（0 = 纯物理体，比如"隐形的墙"）。
    // 由 PhysicsWorld 填，求解完把变换写回实体的 TransformComponent。
    std::uint32_t entity = 0xFFFFFFFFu;

    BodyType type = BodyType::Dynamic;
    Shape shape;
    PhysicsMaterial material;

    // ---- 变换（世界空间，position = 质心）----
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};

    // ---- 速度 ----
    glm::vec3 linearVelocity{0.0f};
    glm::vec3 angularVelocity{0.0f};   // 世界空间角速度（rad/s）

    // ---- 质量属性（含 invMass / invInertia，由 shape 算出）----
    MassProperties massProps;

    // ---- 阻尼 ----
    // 每步按 v *= 1/(1 + damping*dt) 施加。默认很小但**不为 0**：
    // 完全无阻尼时，求解器的微小误差会慢慢累积成"物体自己漂移"。
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    float gravityScale = 1.0f;

    std::uint32_t flags = kBodyDefaultFlags;

    // ---- 休眠 ----
    bool awake = true;
    float sleepTimer = 0.0f;

    // ---- 每步累积的力 / 力矩（积分完清零）----
    glm::vec3 force{0.0f};
    glm::vec3 torque{0.0f};

    // ---- 上一步的变换（渲染插值用；固定步长下渲染帧未必落在步边界）----
    glm::vec3 prevPosition{0.0f};
    glm::quat prevRotation{1.0f, 0.0f, 0.0f, 0.0f};

    // ---- 上一帧的 AABB（宽相增量更新用）----
    glm::vec3 aabbMin{0.0f};
    glm::vec3 aabbMax{0.0f};

    // 接触缓存 id：求解器为每个 body 维护一段"这一帧的接触约束"，
    // 这里只记它在缓存里的下标（-1 = 没有）。
    int contactStart = -1;
    int contactCount = 0;

    // ------------------------------------------------ 便捷查询
    bool isDynamic() const { return type == BodyType::Dynamic; }
    bool isStatic() const { return type == BodyType::Static; }
    bool isKinematic() const { return type == BodyType::Kinematic; }
    // 求解器里统一用这个：只有 Dynamic 才有非零逆质量。
    bool movable() const { return type == BodyType::Dynamic; }

    void setFlag(std::uint32_t f, bool on) {
        flags = on ? (flags | f) : (flags & ~f);
    }
    bool hasFlag(std::uint32_t f) const { return (flags & f) != 0; }

    // 形状的世界矩阵（形状原点 == 质心）。
    glm::mat4 worldMatrix() const {
        return glm::translate(glm::mat4(1.0f), position) *
               glm::mat4_cast(rotation);
    }

    // 求解器**实际使用**的逆质量：休眠中的体视作无限质量。
    //
    // 为什么放在这里而不是在求解器里加 if：休眠的意义就是"别再算它了"。
    // 如果休眠体仍有逆质量，求解器会为了维持它的静止而每帧算出一堆冲量
    //（然后被丢弃），休眠就白费了。归零之后，接触对它的作用自然消失 ——
    // 而"被唤醒"这一步由 PhysicsWorld 在求解前统一传播（见 WakeIslands）。
    float invMassEff() const {
        return (movable() && awake) ? massProps.invMass : 0.0f;
    }

    // 世界空间的逆惯量（随姿态变，所以每次现算）。休眠体同样是 0。
    glm::mat3 invInertiaWorld() const {
        if (!movable() || !awake) return glm::mat3(0.0f);
        return physics::invInertiaWorld(massProps, rotation);
    }

    // 在某点施加冲量（世界空间）。冲量作用于质心之外会产生角速度 ——
    // 这就是"打到箱子一角会让它转起来"的来源。
    //   point 默认取质心（纯直线冲量）。
    void applyImpulse(const glm::vec3& impulse,
                      const glm::vec3& point = glm::vec3(NAN)) {
        if (!movable()) return;
        linearVelocity += impulse * massProps.invMass;
        const glm::vec3 r = std::isnan(point.x) ? glm::vec3(0.0f)
                                                : point - position;
        if (glm::dot(r, r) > 0.0f) {
            angularVelocity += invInertiaWorld() * glm::cross(r, impulse);
        }
    }

    void applyForce(const glm::vec3& f, const glm::vec3& point = glm::vec3(NAN)) {
        if (!movable()) return;
        force += f;
        const glm::vec3 r = std::isnan(point.x) ? glm::vec3(0.0f)
                                                : point - position;
        if (glm::dot(r, r) > 0.0f) torque += glm::cross(r, f);
    }

    // ------------------------------------------------ 积分
    //
    // 半隐式（symplectic）欧拉：**先更新速度，再用新速度更新位置**。
    // 显式欧拉（先位置后速度）在重力下每步都会多注入 ½g·dt² 的能量，
    // 表现为"静止的箱子缓慢下沉"——这是最简单的能量漂移验证点。
    //
    // 顺序：重力 → 外力 → 阻尼 → 锁轴 → 位置/姿态。
    void integrateVelocity(const glm::vec3& gravity, float dt) {
        if (!movable()) return;
        // ⚠ 睡着的体必须**完全冻结**，不能只是"求解时当作无限质量"。
        //   早先这里只判 movable()，于是睡着的箱子照样被重力加速；而它的
        //   接触又因为 `collectContacts` 里"两边都不活跃 → 跳过"的优化被
        //   剔除（睡着的体 awake=false → 不活跃）。两者叠加的后果是：
        //   **睡着的箱子没有接触约束、却一直受重力 → 直接穿过地面往下掉**，
        //   直到被邻居通过 wakeIslands 唤醒才"回弹"。
        //   宏观症状：一叠本该静下来的箱子每隔十几步突然集体抖一下 ——
        //   休眠计时器永远攒不满，箱子永远睡不着。
        if (!awake) return;
        if (hasFlag(kBodyUseGravity)) {
            // 重力按**质量**给（力 = m·g），这样 invMass 约掉之后得到
            // 与质量无关的 g —— 和现实一致（真空中羽毛和铁球同时落地）。
            linearVelocity += gravity * (gravityScale * dt);
        }
        linearVelocity += force * (massProps.invMass * dt);
        angularVelocity += invInertiaWorld() * (torque * dt);

        // 阻尼：用 1/(1+c·dt) 而不是 (1 - c·dt)。后者在 c·dt > 1 时
        // 会把速度**反向**（dt 一大就炸），前者对任意 dt 都单调衰减到 0。
        linearVelocity *= 1.0f / (1.0f + linearDamping * dt);
        angularVelocity *= 1.0f / (1.0f + angularDamping * dt);

        if (hasFlag(kBodyLockLinX)) linearVelocity.x = 0.0f;
        if (hasFlag(kBodyLockLinY)) linearVelocity.y = 0.0f;
        if (hasFlag(kBodyLockLinZ)) linearVelocity.z = 0.0f;
        if (hasFlag(kBodyLockRotX)) angularVelocity.x = 0.0f;
        if (hasFlag(kBodyLockRotY)) angularVelocity.y = 0.0f;
        if (hasFlag(kBodyLockRotZ)) angularVelocity.z = 0.0f;

        // 安全阀：数值爆炸时直接清零而不是让 NaN 扩散到整张接触图。
        // 阈值取得很宽松（1e5 m/s），正常游戏永远碰不到。
        if (!std::isfinite(linearVelocity.x) ||
            glm::dot(linearVelocity, linearVelocity) > 1.0e10f) {
            linearVelocity = glm::vec3(0.0f);
        }
        if (!std::isfinite(angularVelocity.x) ||
            glm::dot(angularVelocity, angularVelocity) > 1.0e10f) {
            angularVelocity = glm::vec3(0.0f);
        }
    }

    void integratePosition(float dt) {
        if (!movable()) return;
        position += linearVelocity * dt;
        rotation = integrateRotation(rotation, angularVelocity, dt);
    }

    // 积分前记下上一帧变换（插值与"这一帧动没动"的判定都靠它）
    void savePrevTransform() {
        prevPosition = position;
        prevRotation = rotation;
    }

    // 这一步的总动能（休眠判定 + 验证脚本读数）
    float kineticEnergy() const {
        const float lin =
            0.5f * massProps.mass * glm::dot(linearVelocity, linearVelocity);
        // 角动能：½ ωᵀ · I_world · ω。用逆惯量算 I_world·ω 要再求逆，
        // 所以这里用"局部角速度 × 局部惯量"等价形式：ω_local = Rᵀω。
        const glm::vec3 wLocal = glm::conjugate(rotation) * angularVelocity;
        const float ang = 0.5f * glm::dot(wLocal, massProps.inertia * wLocal);
        return lin + ang;
    }
};

} // namespace physics
