#pragma once
// ============================================================
// animation/Pose —— 把采样拆成"姿态"和"关节矩阵"两步
//
// 为什么要有这一层（2026-10-18，角色链 A0-5）：
//   `assets::Skeleton::sample` 一次做完三件事 ——
//     ① 采轨道得到每根骨骼的局部 TRS
//     ② 沿 topoOrder 累乘出世界矩阵
//     ③ 乘 invMeshNode / invBind 得到 joint 矩阵
//   中间**没有任何可插入点**，而且它用两个 `static thread_local` 缓冲
//   存中间结果（`Skeleton.cpp:77/101`）。后果：
//     * 无法 crossfade（需要两份 local 姿态再混合）
//     * 无法分层（每层一份姿态）
//     * 无法 IK（要在"局部旋转"或"世界矩阵已算、invBind 未乘"上改）
//     * 嵌套调用会互相踩缓冲（A 层采样里调 B 层 → 缓冲被覆盖）
//
//   拆开之后：`samplePose` 只产出局部 TRS，`poseToJoints` 只做世界累乘 +
//   invBind。中间那一步（blend / mask / IK）就有了插入位置，而且全部
//   缓冲由调用方显式拥有 —— 嵌套多少层都不会互相污染。
//
// ⚠ 逐位一致是硬要求：`Skeleton::sample` 保留为兼容壳（= samplePose +
//   poseToJoints），`MYVK_ANIM_TEST=1` 会对比两者输出的关节矩阵
//   **memcmp 级相同**。所以这里的 decompose / compose 顺序与表达式必须
//   与拆分前的实现完全一致，别"顺手优化"。
//
// ⚠ 混合必须在 **local TRS 空间**：直接对 `mat4` 做 lerp 会插出缩放与
//   剪切，肉眼是"角色混合时变瘦"。见 blendPose。
// ============================================================

#include "assets/Skeleton.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace animation {

// 一具骨架的局部姿态，长度恒等于 skeleton.jointCount()。
// 三个数组而不是"struct{vec3,quat,vec3} 的数组"：blend / mask / IK 都是
// 逐分量的操作，分开存能让循环更短、也方便将来换成 SIMD 友好布局。
struct Pose {
    std::vector<glm::vec3> translation;
    std::vector<glm::quat> rotation;
    std::vector<glm::vec3> scale;

    int count() const { return static_cast<int>(rotation.size()); }
    void resize(int n);
    // 全部置为静止姿势（restLocal 分解出的 TRS）。
    void setRest(const assets::Skeleton& s);
};

// 采样：把 clip 在 time 处的轨道值覆盖到 out 上。
//   * out 会先被 setRest（轨道没覆盖的分量用静止姿势）
//   * clip == nullptr → 纯静止姿势
//   * time 超出轨道范围 → 取端点（与 sampleTrack 的语义一致）
void samplePose(const assets::Skeleton& skel, const assets::AnimationClip* clip,
                float time, Pose& out);

// 局部姿态 → joint 矩阵（invMeshNode * world * invBind）。
// 世界矩阵沿 skel.topoOrder 累乘（保证 parent 先于 child）。
// out 会被 resize 到 jointCount()。
void poseToJoints(const assets::Skeleton& skel, const Pose& pose,
                  std::vector<glm::mat4>& out);

// 两个姿态按 w ∈ [0,1] 混合：w=0 → a，w=1 → b。
// ⚠ **端点必须短路**：`slerp(q,q,w)` 在浮点下不是恒等，
//   w=0 走一次 slerp 会引入 1e-7 的漂移，长跑后角色会"慢慢转歪"。
//   短路之后 w=0 的结果与纯 a **逐位相同**（A2 的断言 4 靠这个）。
// 长度不等时遍历较短的那一侧，多出来的骨骼从 a 原样拷贝。
void blendPose(const Pose& a, const Pose& b, float w, Pose& out);

} // namespace animation
