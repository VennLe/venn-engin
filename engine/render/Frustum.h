#pragma once
// ============================================================
// render/Frustum —— 相机视锥体（6 个平面）与可见性测试
//
// 用途：**物体级视锥剔除**。每帧从 viewProjection 矩阵里抽出六个平面，
// 把场景里每个网格实体的世界空间 AABB 拿来做"六平面外包测试"，
// 视锥外的物体直接不进入提交路径（深度预通道 / 不透明 / 透明）。
//
// 为什么用"平面 + AABB"而不是"八个角点逐个判"：
//   · 平面式测试是 O(6)，且每个平面只需一次"正顶点"比较，没有除法；
//   · 结果偏保守 —— 盒子的正顶点在平面内侧就判为可见，宁多画不漏画。
//     剔除算法漏画是肉眼可见的 bug（物体在屏幕边缘突然消失），
//     多画只是浪费一点提交，所以必须选保守的一侧。
//
// ------------------------------------------------------------
// 平面法线的方向约定
// ------------------------------------------------------------
// 这里每个平面都存成  n·x + d = 0  ，且 n **指向视锥内部**。
// 于是：
//   distance(p) > 0   点在视锥内侧
//   distance(p) < 0   点在外侧
//   整个 AABB 在外侧      ⟺ 它的"正顶点" distance < 0
// ============================================================

#include <glm/glm.hpp>

namespace render {

struct Plane {
    glm::vec3 n{0.0f, 0.0f, 1.0f};  // 单位法线，指向视锥内部
    float d = 0.0f;                 // 平面方程 n·x + d = 0

    float distance(const glm::vec3& p) const { return glm::dot(n, p) + d; }
};

struct Frustum {
    // 顺序：left / right / bottom / top / near / far
    Plane planes[6];

    // AABB 与视锥是否相交（保守：可能"相交但实际全在外"时返回 true）。
    // 这是剔除的核心：返回 false 才允许把物体丢掉。
    bool intersects(const glm::vec3& lo, const glm::vec3& hi) const;

    // 球体与视锥是否相交（灯小球剔除用；比 AABB 便宜且天然面向中心）。
    bool intersectsSphere(const glm::vec3& center, float radius) const;
};

// 从 viewProjection 矩阵抽出视锥（Gribb–Hartmann 方法）。
//
// 注意本引擎的两个约定：
//   · proj 由 glm::perspective 生成且**没有**开 GLM_FORCE_DEPTH_ZERO_TO_ONE
//     → NDC 的 z 范围是 [-1, 1]（近平面用 r3 + r2，不是 r2）。
//   · Camera::projMatrix 把 proj[1][1] 取负（Vulkan 裁剪空间 Y 翻转）。
//     那只是 NDC 的 Y 镜像，世界空间里"六个平面"这个集合本身不变，
//     所以 bottom/top 两条谁对上谁不影响剔除结果。
Frustum extractFrustum(const glm::mat4& viewProj);

} // namespace render
