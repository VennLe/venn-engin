#pragma once
// ============================================================
// physics/ConvexCollision —— 两个凸体的相交测试与穿透求解（GJK + EPA）
//
// 这是"带碰撞检测的物体不能互相穿过"的数学核心，也是业内凸体碰撞的
// 标准组合：**GJK 判交，EPA 求穿透深度**。
//
// ------------------------------------------------------------
// 一、GJK（Gilbert–Johnson–Keerthi）
// ------------------------------------------------------------
// 原理只有一句话：两个凸体 A、B 相交 ⟺ **原点落在它们的 Minkowski 差
// A ⊖ B 里面**。而 A ⊖ B 也是凸的，所以"原点在不在里面"可以用支撑函数
// 迭代地逼近：
//
//   · 从任意方向取一点 w = support_A(d) - support_B(-d)
//   · 把它加进"单纯形"（最多 4 个点的点集）
//   · 求出单纯形上离原点最近的点，把方向 d 指向它
//   · 如果新取的支撑点在该方向上无法超过原点，说明原点不可达 → 不相交
//   · 如果单纯形把原点包住了 → 相交
//
// 这里用的是"最近点 + 单纯形约简"的写法（Ericson《Real-Time Collision
// Detection》里的点-三角形最近点 + 按重心坐标裁掉不参与的面），而不是
// 那套容易写错的 sameDirection 版：每一步都是显式的几何谓词，不会因为
// 单纯形绕序变了就得到相反的结论。
//
// ------------------------------------------------------------
// 二、EPA（Expanding Polytope Algorithm）
// ------------------------------------------------------------
// GJK 只回答"相交吗"。要**把物体推开**还需要穿透深度和方向，那是 EPA：
// 把 GJK 收敛时的单纯形当成初始多面体，反复用支撑函数把它"撑开"，直到
// 最近的那个面的距离不再增长 —— 那个面的法线与距离就是最小平移向量
// （MTV）。
//
// 有了 (normal, depth)：把 A 挪 -normal×depth、B 挪 +normal×depth，两个
// 物体就正好分开（见 CollisionWorld 里的用法）。
//
// 约定：normal 从 A 指向 B。
// ============================================================

#include "physics/ConvexShape.h"

#include <glm/glm.hpp>

namespace physics {

struct Contact {
    bool hit = false;
    // A → B 的分离方向（单位向量）。
    // 分开的办法：A 平移 -normal * depth，B 平移 +normal * depth。
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float depth = 0.0f;
    // 接触点（世界空间，取两个表面上沿法线相对的两点的中点）
    glm::vec3 point{0.0f};
};

// 只判交（比 collide 便宜：不需要 EPA）
bool gjkIntersect(const TransformedShape& a, const TransformedShape& b);

// 判交 + 穿透深度 / 方向
Contact collide(const TransformedShape& a, const TransformedShape& b);

} // namespace physics
