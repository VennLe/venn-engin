#pragma once
// ============================================================
// physics/Solver —— 顺序冲量求解器（Sequential Impulse / PGS）
//
// ------------------------------------------------------------
// 它在解什么
// ------------------------------------------------------------
// 每个接触点给出一条**不等式约束**：
//
//     沿法线 n 的相对速度 ≥ 0        （不能继续互相靠近）
//     沿切向的相对速度要尽可能为 0    （摩擦，且 |F_t| ≤ μ|F_n|）
//
// 求解器把"冲量"当作未知量，反复迭代：每轮只看一条约束并**单独**把它解到
// 满足，然后把解出来的冲量立刻施加到两个刚体上 —— 于是下一轮看到的已经是
// 更新过的速度。这叫投影高斯-赛德尔（PGS）。迭代收敛后，所有约束同时（近似）
// 满足。
//
// 为什么不用"把约束写成矩阵一次性解线性方程组"：约束数量在几十到几百之间，
// 且每帧拓扑都变；PGS 无需求逆、无需稀疏矩阵、天然支持"每轮提前退出"，
// 是实时物理的标准选择（Bullet / PhysX / Box2D 全走这条路）。
//
// ------------------------------------------------------------
// 热启动（warm starting）—— 本文件里最重要的一件事
// ------------------------------------------------------------
// 如果每帧都从 0 开始累积冲量，那么收敛速度取决于迭代次数：迭代不够时，
// 上层箱子会把下层压穿一点、下一帧再补回来 —— 表现为**整叠箱子缓慢下沉
// 并抖动**。
//
// 热启动的做法：把上一帧解出来的冲量当作这一帧的**初值**（冲量按
// featureId 跨帧继承，见 Contact.h）。于是求解器只需要"修正"上一步的解，
// 而不是从零重建 —— 同样的迭代次数下，稳定性提高一个数量级。
// 这也是本引擎敢只用 8 次速度迭代的原因。
//
// ------------------------------------------------------------
// 位置修正：Baumgarte + slop（以及为什么这么做）
// ------------------------------------------------------------
// 速度约束只保证"不再继续穿透"，但**已经**发生的穿透需要额外的机制来消除。
// 这里用 Baumgarte 稳定化：在法向约束里加一个"偏置速度"
//
//     bias = (β / dt) · max(0, depth − slop)
//
// 它等价于"把穿透的物体轻轻推出来"。β 取 0.1~0.2：太大（> 0.5）会让物体
// 在静止接触时一弹一弹（因为每步都注入了多余能量），太小则穿透恢复很慢。
//
// slop（默认 5 mm）是**允许的穿透深度**：在 slop 以内不加偏置，于是静置
// 的箱子会"陷进地面 5 mm"然后停住 —— 这是刻意的。没有 slop 的话，浮点
// 误差会让深度在 0 附近抖动，偏置速度每帧正负翻转，那就是"静止物体自己
// 嗡嗡抖"。5 mm 的穿透在画面上完全看不出来。
// ============================================================

#include "physics/Contact.h"
#include "physics/Joints.h"
#include "physics/RigidBody.h"

#include <vector>

namespace physics {

struct SolverSettings {
    // 速度迭代轮数。8 次配合热启动足够稳；纯堆叠（10 层以上）建议 12~16。
    int velocityIterations = 8;
    // 位置修正迭代轮数（split impulse 通道）。3 次已经够把整叠箱子
    // 推回原位；再多次只是在做无用功（误差已经低于 slop）。
    int positionIterations = 3;
    // Baumgarte 系数（位置修正比例）。
    float baumgarte = 0.2f;
    // 允许的穿透（米）。见文件头。
    float slop = 0.005f;
    // 单步最多修正多少穿透（米）。防止"深穿透的一瞬间被弹飞"。
    float maxCorrection = 0.2f;
    // 相对速度低于它就不做弹性反弹（米/秒）。没有这个阈值的话，静置的
    // 弹性球会因为微小的接触速度而永远轻微弹跳（"弹性球不静止"）。
    float restitutionThreshold = 1.0f;
    // 热启动冲量的衰减倍数。1 = 完全继承；<1 让旧冲量缓慢失效（场景
    // 拓扑频繁变化时更稳）。这里默认 1（标准做法）。
    float warmStartScale = 1.0f;
    // 摩擦：约束锥用"库仑三角锥"的两个切向投影。关掉就是零摩擦（冰面）。
    bool enableFriction = true;
    // 位置修正通道（split impulse）。开 = 修正穿透但不污染速度（推荐）；
    // 关 = 退回经典 Baumgarte（把偏置速度加进真实速度，会轻微注入能量）。
    // 默认开；验证脚本用它做 A/B 对照。
    bool splitImpulse = true;

    // 运行时可由 PhysicsWorld 覆盖的全局重力（验证脚本要用它做解析对照）
    glm::vec3 gravity{0.0f, 0.0f, -9.81f};
    // 速度上限（米/秒）：防爆。超过之后整个世界的接触都会变得不可靠，
    // 不如直接截断（比物理上"穿模"可接受得多）。
    float maxLinearVelocity = 200.0f;
    float maxAngularVelocity = 100.0f;
};

struct SolverStats {
    int manifolds = 0;
    int points = 0;
    int velocityIterations = 0;
    float maxPenetration = 0.0f;      // 本步最深穿透（米）
    float totalNormalImpulse = 0.0f;  // 本步法向冲量总和（N·s）
    float maxSpeed = 0.0f;
};

class Solver {
public:
    struct Context;
    struct World;

    // 求解一个步长：更新 bodies 的速度，并把接触冲量写回 manifolds
    //（下一帧热启动要用）。
    //
    // 调用顺序要求：
    //   1. cache 里存着**上一帧**的流形（ContactCache::find 要能查到）
    //   2. manifolds 是本帧窄相的输出
    //   3. joints（可空）已经 Prestep 过（行已算好）
    //   4. bodies 的速度已经加上重力（位置**还没**积分）
    //
    // joints 与接触在**同一轮迭代里交替求解**：被关节拉住的箱子同时又压在
    // 地面上时，两族约束会互相影响 —— 分两遍独立解的话，第二遍会把
    // 第一遍的解破坏掉，表现为"链条末端的物体抖个不停"。
    void solve(std::vector<RigidBody>& bodies,
               const ContactCache& cache,
               std::vector<ContactManifold>& manifolds,
               std::vector<Joint>* joints,
               const SolverSettings& settings, float dt);

    const SolverStats& stats() const { return m_stats; }

private:
    SolverStats m_stats;

    // 位置修正通道的"伪速度"（每个体一项，按体下标索引）。
    // 作为成员保存是为了**跨帧复用同一块内存**：每步都重新 assign 一份
    // 会让求解器在大场景下每步都做一次堆分配。
    std::vector<glm::vec3> m_pseudoLin;
    std::vector<glm::vec3> m_pseudoAng;
};

} // namespace physics
