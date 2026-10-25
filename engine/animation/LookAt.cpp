// ============================================================
// animation/LookAt.cpp
// ============================================================

#include "animation/LookAt.h"

#include "animation/SpringBone.h"   // shortestArc（与弹簧骨共用一份实现）

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace animation {

namespace {

// 四元数 → 旋转角（度）。用 |w| 而不是 w：q 与 -q 表示同一旋转，
// 直接 acos(w) 在 w<0 时会给出 >180° 的"角"，限幅判断会误判成需要限。
float angleDeg(const glm::quat& q) {
    const float w = std::min(1.0f, std::fabs(q.w));
    return glm::degrees(2.0f * std::acos(w));
}

// 自动选择时的默认骨名。写成常量而不是把字面量散在调用点：
// "默认挑头"这个语义只有一处，改的人一眼能看到。
constexpr const char* kDefaultLookAtBone = "head";

} // namespace

int resolveLookAtBone(const assets::Skeleton& skel, const std::string& name) {
    // 实现已收敛到 `Skeleton::findJoint`（第八刀）。这里只剩"空名字选什么"。
    //
    // ⚠ 为什么空名字**不能**直接扔给 findJoint("")：它对空串返回 -1
    //   （"没指定"），而 LookAt 的语义是"自动挑头"。两者必须分开 ——
    //   混在一起的症状是"Inspector 上骨名留空就再也不转头了"。
    const unsigned flags = assets::kJointMatchCaseInsensitive |
                           assets::kJointMatchSubstring |
                           assets::kJointMatchSkipEndTips;
    if (name.empty()) return skel.findJoint(kDefaultLookAtBone, flags);
    return skel.findJoint(name, flags);
}

float LookAtState::appliedAngleDeg() const { return angleDeg(m_delta); }

int LookAtState::apply(const assets::Skeleton& skel, int bone, float dt,
                       const LookAtSettings& s, Pose& pose) {
    if (!s.enabled || !s.hasTarget || s.weight <= 0.0f) {
        m_init = false;
        return 0;
    }
    const int n = skel.jointCount();
    if (bone < 0 || bone >= n || pose.count() < n) {
        m_init = false;
        return 0;
    }
    if (!(dt > 0.0f)) return 0;   // 暂停：姿态原样

    // 世界矩阵（父骨旋转与骨头位置都从这里取）
    std::vector<glm::mat4> world;
    poseToWorld(skel, pose, world);

    const assets::Skeleton::Joint& jt = skel.joints[static_cast<size_t>(bone)];
    const glm::vec3 headPos = glm::vec3(world[static_cast<size_t>(bone)][3]);
    const glm::vec3 toTarget = s.target - headPos;
    const float dist = glm::length(toTarget);
    // 目标点与骨头重合：方向未定义。此时**保持上一帧的 delta**（而不是
    // 归零），否则目标点从骨头里穿过去的那几帧，头会抽一下。
    if (dist < 1.0e-5f) return m_init ? 1 : 0;

    // ---- 父骨空间 ----
    // 父骨不存在（bone 是根）时用 externalChain 的旋转。
    const glm::mat4 parentWorld =
        jt.parent >= 0 ? world[static_cast<size_t>(jt.parent)] : jt.externalChain;
    const float det = glm::determinant(glm::mat3(parentWorld));
    const glm::quat parentRot =
        glm::quat_cast(glm::mat3(parentWorld) * (det != 0.0f ? 1.0f / det : 1.0f));

    // 目标方向转到父骨空间（见头文件 ①）
    const glm::vec3 dirParent = glm::inverse(parentRot) * (toTarget / dist);

    glm::quat curLocal = pose.rotation[static_cast<size_t>(bone)];
    const float qlen = glm::length(curLocal);
    if (qlen > 1.0e-6f) curLocal /= qlen;
    else curLocal = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

    // 当前"眼睛朝向"（父骨空间）= 局部旋转 × 骨骼空间里的前方。
    // 这样即使 forwardLocal 猜错符号，也只是方向反了 —— 一眼能看出来。
    glm::vec3 curDir = curLocal * s.forwardLocal;
    const float cl = glm::length(curDir);
    if (cl < 1.0e-6f) return 0;
    curDir /= cl;

    glm::quat align = shortestArc(curDir, dirParent);

    // ---- 限幅（见头文件 ③）----
    const float maxAng = std::max(0.0f, s.maxAngleDeg);
    const float ang = angleDeg(align);
    if (ang > maxAng) {
        glm::vec3 axis(align.x, align.y, align.z);
        const float al = glm::length(axis);
        // ang 略大于 0 但 axis 数值上可能极小；≥ 1e-6 才算有意义的方向
        if (al > 1.0e-6f)
            align = glm::angleAxis(glm::radians(maxAng), axis / al);
        else
            align = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    // ---- 平滑 ----
    if (!m_init) {
        // 第一帧：**直接取目标**，不从上一次的 delta 插值。
        // 用恒等起步的话，刚加组件时会看到头"从正面缓慢转过去"的开场动画。
        m_delta = align;
        m_init = true;
    } else {
        const float k = 1.0f - std::exp(-std::max(0.0f, s.smoothRate) * dt);
        m_delta = glm::slerp(m_delta, align, k);
        // slerp 在两端不保证归一化（浮点），显式归一化避免逐帧累积漂移
        const float dl = glm::length(m_delta);
        if (dl > 1.0e-6f) m_delta /= dl;
    }

    // ---- 写回 ----
    // weight=0 已在函数开头短路，所以这里 weight 恒 > 0；但 0.5 这类
    // 中间值要"只转一半"，用 slerp 从恒等插值（直接乘 0.5 是错的：
    // 四元数的线性缩放不等于"转一半角度"）。
    const float w = std::min(1.0f, s.weight);
    const glm::quat applied =
        (w >= 1.0f) ? m_delta
                    : glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), m_delta, w);

    glm::quat out = applied * curLocal;
    const float ol = glm::length(out);
    if (ol < 1.0e-6f) return 0;
    pose.rotation[static_cast<size_t>(bone)] = out / ol;
    return 1;
}

} // namespace animation
