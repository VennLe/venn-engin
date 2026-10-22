#pragma once
// ============================================================
// physics/Narrowphase —— 窄相：把一对刚体变成接触流形
//
// ------------------------------------------------------------
// 为什么不是"一律走 GJK/EPA"
// ------------------------------------------------------------
// GJK/EPA 已经写在 ConvexCollision 里，它能处理**任意**凸体对。但它只给
// 一个接触点（一个 MTV 法线 + 一个深度），而稳定的堆叠需要 4 个点（见
// Contact.h 的说明）。从 EPA 的多面体上"反推面并裁剪"是可行的，但代码量
// 大、退化情况多。
//
// 所以这里采取**分派 + 特例**的经典结构：
//
//   球-球        → 1 点，闭式解（最准）
//   球-盒        → 1 点（盒上最近点）
//   盒-盒        → SAT 找最小分离轴 + **参考面/入射面裁剪** → 最多 4 点
//   胶囊-X       → 退化成"两端的球"逐个测，最多 2 点
//   平面-X       → 解析：把 X 的支撑特征投到平面上，最多 4 点
//   其余（凸包） → 回退到 GJK/EPA 的**单点**路径
//
// 好处是常见组合（球 / 盒 / 胶囊 / 平面，覆盖 95% 的游戏物理）都走
// 精确且多点的路径，只有"从网格生成的凸包"退回单点 —— 而凸包本来
// 就不该用来堆叠（它当静态场景几何或子弹壳更合适）。
//
// ------------------------------------------------------------
// 一个必须记住的约定
// ------------------------------------------------------------
//   **法线从 A 指向 B**（和 ConvexCollision::Contact 一致）。
//   求解器把冲量沿 +n 加在 B 上、沿 -n 加在 A 上。
//   本文件内部会把 (A,B) 按形状类型排序（平面永远当 A），算完再翻回来 ——
//   于是顺序问题只在一个地方处理，不会在 8 个特例里各处理一次。
// ============================================================

#include "physics/Contact.h"
#include "physics/Heightfield.h"
#include "physics/RigidBody.h"

#include <vector>

namespace physics {

// 生成接触流形。返回 false = 不接触（out.count == 0）。
// a / b 是 PhysicsWorld 里的**数组下标**（流形里会原样记下来）。
bool collideBodies(const RigidBody& A, const RigidBody& B, std::uint32_t indexA,
                   std::uint32_t indexB, ContactManifold& out);

// 供验证脚本直接调用：两个形状在世界变换下的最小穿透深度。
// （不生成流形，只回答"嵌进去多少"，用来做解析对照。）
struct ShapeQuery {
    const Shape* shape = nullptr;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::mat4 worldMatrix() const {
        return glm::translate(glm::mat4(1.0f), position) *
               glm::mat4_cast(rotation);
    }
};
// 返回穿透深度（>0 表示相交），并把法线（A→B）写进 outNormal。
float penetrationDepth(const ShapeQuery& a, const ShapeQuery& b,
                       glm::vec3& outNormal);

// ---------------------------------------------------------------- 地形
// 刚体 vs 高度场（2026-10-22 第十轮）。
//
// 高度场**不是**一个刚体形状（见 Heightfield.h 文件头），所以它没有
// `RigidBody` 可比。这里用"物体中心处的地表切平面"当代表面：
//
//   · 每个刚体只生成**一个**流形，法线取 `normalAt(中心 XY)`，
//     平面过点 (cx, cy, heightAt(cx, cy))；
//   · 然后把物体的支撑点（盒的 8 角 / 胶囊两端 / 凸包顶点 / 球心-半径）
//     投到这个平面上，取最深的 4 个。
//
// 为什么可以这样"一条平面打天下"：一张 2 m/采样点的高度图，在 1~2 m 的
// 物体尺度上曲率可以忽略；而"每个接触点各用各的法线"会让同一个流形里
// 出现方向不一致的约束，求解器会来回分冲量（抖动）。
//
// `terrainIndex` 是 PhysicsWorld 里那个**伪静态刚体**的下标（求解器要
// 一个合法的 A 侧，见 PhysicsWorld::setHeightfield 的说明）。
bool terrainVsBody(const Heightfield& hf, const RigidBody& B,
                   std::uint32_t terrainIndex, std::uint32_t bodyIndex,
                   ContactManifold& out);

} // namespace physics
