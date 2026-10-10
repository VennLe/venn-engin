#pragma once
// ============================================================
// physics/ConvexShape —— 碰撞体形状（凸体）与它的世界变换
//
// 三种形状共用同一个接缝：**支撑函数** support(dir) = 沿 dir 最远的点。
// 这是凸体碰撞算法（GJK / EPA）唯一需要的东西 —— 有了它，凸包、胶囊、
// 球体在碰撞代码里走的是同一条路，不需要为每种组合写一遍。
//
// ------------------------------------------------------------
// 方向为什么要用 transpose(mat3) 变换
// ------------------------------------------------------------
// 直觉上"把形状变换到世界空间"是把顶点乘上去。但支撑函数要的是**方向**
// 的变换，而方向是按 M⁻ᵀ 变的（法线变换的同一套道理）：
//
//     support_{M·S}(d) = M · support_S(Mᵀ · d)
//
// 好处是**非均匀缩放也精确**。物体会被 TransformComponent 拉扁拉长，
// 碰撞体跟着一起扁；如果用"单位矩阵近似"或者"只取旋转"，形状就对不上
// 模型了。所以这里老老实实做 Mᵀ。
// （推导：x ∈ S，最大化 <Mx, d>；令 y = Mx，则 <y, d> = <x, Mᵀd>，
//   所以先在 S 上沿 Mᵀd 取支撑，再乘 M 回去。）
// ============================================================

#include "physics/ConvexHull.h"

#include <glm/glm.hpp>

#include <vector>

namespace physics {

struct ConvexShape {
    enum class Kind : int {
        Hull = 0,     // 由网格顶点求出的凸包
        Capsule = 1,  // 胶囊体（轴 = 局部 +Z，本引擎世界是 Z-up）
        Sphere = 2,   // 球（相机碰撞体用）
    };

    Kind kind = Kind::Capsule;

    // ---- Hull ----
    // 凸包顶点，**已按碰撞体自身原点居中**：居中之后"缩放碰撞体"才是绕它
    // 自己放大，而不是从实体原点往外长。
    std::vector<glm::vec3> hull;

    // 凸包的面（索引进 hull）。只有线框绘制需要它 —— 要画的是"边"，
    // 顶点两两相连会画成一团糊（球体的凸包有上千个顶点）。
    std::vector<glm::uvec3> hullFaces;

    // ---- Capsule / Sphere ----
    float radius = 0.5f;
    float halfHeight = 0.5f;  // Capsule 圆柱段的半高（不含两端半球）

    // 沿 dir 最远的点（局部空间）。dir 不必归一化，允许零向量。
    glm::vec3 supportLocal(const glm::vec3& dir) const;

    // 局部空间 AABB（粗筛用；也是"默认碰撞体刚好包裹物体"的依据）
    void localAabb(glm::vec3& mn, glm::vec3& mx) const;
};

// 形状 + 世界变换。GJK / EPA 只认这个。
struct TransformedShape {
    const ConvexShape* shape = nullptr;
    glm::mat4 xform{1.0f};

    glm::vec3 support(const glm::vec3& worldDir) const;

    // 世界空间 AABB（保守：把局部 AABB 的 8 个角都变换过去取包围盒）
    void worldAabb(glm::vec3& mn, glm::vec3& mx) const;

    // 局部 AABB 的角点变换到世界后的中心
    glm::vec3 center() const;
};

} // namespace physics
