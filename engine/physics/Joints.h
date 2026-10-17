#pragma once
// ============================================================
// physics/Joints —— 关节（约束）
//
// ------------------------------------------------------------
// 统一抽象：把每种关节都拆成一组"标量约束行"
// ------------------------------------------------------------
// 接触约束和关节约束在数学上是**同一种东西** —— 都是一条
//
//     Cdot = J · v ≥/=/≤ 0
//
// 形式的速度约束，区别只在"限制哪几个自由度"以及"是等式还是不等式"。
// 所以这里不发明新解法：每种关节在 prestep 时把自己展开成 1~6 行
// **标量约束**（一个世界方向 n + 一对力臂 rA/rB），然后交给和接触完全
// 相同的顺序冲量迭代去解。
//
// 好处是：
//   · 求解器只有一套代码（Solver.cpp 里那 20 行），新增关节类型不用碰它；
//   · 关节和接触在**同一次迭代**里交替求解 → "被关节拉住的箱子同时压在
//     地面上"这种耦合天然收敛（分成两遍解会互相打架）。
//
// 每行可以带上下限（lower/upper），于是：
//   · 等式约束（球窝锁住三个方向）→ 不限
//   · 单边限位（铰链的 ±90°）     → 只有一侧有界
//   · 马达                        → 只有一侧有界 + bias 是目标速度
//
// ------------------------------------------------------------
// 位置误差怎么修
// ------------------------------------------------------------
// 用 Baumgarte：把"当前的位置误差 C"折算成一个偏置速度 C·β/dt 加进约束，
// 于是求解器会顺便把误差推回去。这对手工搭的链条（关节两端被拉长了几
// 毫米）足够；不追求"误差严格为零"（那需要单独的位置投影通道，代价是
// 破坏动量守恒）。
// ============================================================

#include "physics/RigidBody.h"

#include <cstdint>

namespace physics {

enum class JointType : int {
    Fixed = 0,     // 焊死：位置与朝向全锁
    Ball = 1,      // 球窝：位置锁、朝向自由（万向节）
    Hinge = 2,     // 铰链：位置锁、只留一个转轴（门 / 轮子）
    Slider = 3,    // 滑动：位置只留一个轴向、朝向锁（活塞 / 抽屉）
    Distance = 4,  // 距离：两点间距离保持（绳子 / 弹簧）
};

constexpr float kInf = 1.0e30f;
constexpr int kMaxJointRows = 8;

// 一条标量约束行（求解器的工作单元）
struct JointRow {
    glm::vec3 n{1.0f, 0.0f, 0.0f};  // 约束方向（世界空间）
    glm::vec3 rA{0.0f};             // A 侧的力臂（角行的两个力臂都是 0）
    glm::vec3 rB{0.0f};
    bool angular = false;           // true = 只约束相对角速度
    float mass = 0.0f;              // 有效质量（1/k）
    float impulse = 0.0f;           // 累积冲量（热启动的载体）
    float bias = 0.0f;              // 位置误差折算出的目标速度
    float lower = -kInf;            // 冲量下界（等式 = -inf）
    float upper = kInf;             // 冲量上界
};

struct Joint {
    JointType type = JointType::Ball;
    std::uint32_t bodyA = 0;
    std::uint32_t bodyB = 0;

    // 锚点（**各自体的局部空间**）。用局部量而不是世界量，是因为物体动了
    // 之后锚点要跟着动 —— 世界量会让关节"粘在原地"。
    glm::vec3 localAnchorA{0.0f};
    glm::vec3 localAnchorB{0.0f};
    // 铰链轴 / 滑动轴（A 的局部空间）
    glm::vec3 localAxisA{0.0f, 0.0f, 1.0f};
    // 参考朝向（用于把"相对转角"定义成一个可比较的标量）
    glm::quat localRefRotation{1.0f, 0.0f, 0.0f, 0.0f};

    // ---- 距离关节 ----
    float distance = 1.0f;
    // ---- 限位（铰链转角 / 滑动位移，单位 = 弧度或米）----
    bool useLimit = false;
    float lowerLimit = 0.0f;
    float upperLimit = 0.0f;
    // ---- 马达（铰链转速 / 滑动速度）----
    bool useMotor = false;
    float motorSpeed = 0.0f;
    float maxMotorForce = 0.0f;

    // ---- 求解器工作区（Prestep 填，迭代用）----
    int rowCount = 0;
    JointRow rows[kMaxJointRows];
    // 世界空间的锚点与轴（Prestep 算一次，供所有行共用）
    glm::vec3 worldAnchorA{0.0f};
    glm::vec3 worldAnchorB{0.0f};
    glm::vec3 worldAxis{0.0f, 0.0f, 1.0f};
    // 当前关节相对位置/角度（限位与马达要用）
    float currentPosition = 0.0f;

    // 计算世界锚点与行。必须在求解前调用（body 的变换已就位）。
    void prestep(const RigidBody& A, const RigidBody& B, float dt,
                 float baumgarte);
    // 用给定的两个体求解一次（一行一轮，被外层迭代反复调用）。
    // 非 const：它会更新 rows[].impulse（累积冲量就是热启动的载体）。
    void solveVelocity(RigidBody& A, RigidBody& B);
    // 把累积冲量清空（场景重建 / 首次求解时用，避免继承到错误的量）
    void clearImpulses();
};

} // namespace physics
