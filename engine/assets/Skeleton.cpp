// ============================================================
// assets/Skeleton.cpp —— 轨道采样 + sample() 兼容壳
//
// ⚠ sample() 的实现已搬到 `engine/animation/Pose`（A0-5）：
//   拆成 samplePose（局部 TRS）+ poseToJoints（世界累乘 + invBind），
//   中间留出混合 / 遮罩 / IK 的插入点，并**删掉了原来的两个
//   `static thread_local` 缓冲**（它们会让嵌套采样互相覆盖）。
//   这里只保留一个转发壳，给还在用旧接口的调用方（角色预览窗口、
//   以及"重构前后逐位一致"那条自检）。
// ============================================================

#include "assets/Skeleton.h"

#include "animation/Pose.h"

#include <algorithm>
#include <cmath>

namespace assets {

glm::vec4 sampleTrack(const AnimTrack& t, float time) {
    if (t.times.empty()) return glm::vec4(0.0f);
    if (time <= t.times.front()) return t.values.front();
    if (time >= t.times.back()) return t.values.back();

    // 二分：第一个 > time 的下标
    const auto it =
        std::upper_bound(t.times.begin(), t.times.end(), time);
    const size_t i = static_cast<size_t>(it - t.times.begin());
    const float t0 = t.times[i - 1];
    const float t1 = t.times[i];
    const float span = t1 - t0;
    float u = span > 1e-9f ? (time - t0) / span : 0.0f;
    if (t.step) u = 0.0f;

    const glm::vec4& a = t.values[i - 1];
    const glm::vec4& b = t.values[i];
    if (t.path == AnimTrack::Rotation) {
        // 球面插值（端点情况 slerp 内部已处理）
        const glm::quat qa(a.w, a.x, a.y, a.z);
        const glm::quat qb(b.w, b.x, b.y, b.z);
        const glm::quat qr = glm::slerp(qa, qb, u);
        return glm::vec4(qr.x, qr.y, qr.z, qr.w);
    }
    return a + (b - a) * u;
}

void Skeleton::sample(const AnimationClip* clip, float time,
                      std::vector<glm::mat4>& out) const {
    // 兼容壳：内部 = 采样 → 局部转世界 → 乘 invBind。
    // 拆分前这里还有两个 `static thread_local` 中间缓冲，现在由 Pose
    // 显式持有 —— 于是"在采样里再采样"（将来的分层 / IK）不会互相踩。
    if (joints.empty()) {
        out.clear();
        return;
    }
    animation::Pose pose;
    animation::samplePose(*this, clip, time, pose);
    animation::poseToJoints(*this, pose, out);
}

} // namespace assets