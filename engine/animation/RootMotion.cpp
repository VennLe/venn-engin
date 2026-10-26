#include "animation/RootMotion.h"

#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>

namespace animation {

namespace {

// 沿父链把一个 joint 的世界矩阵算出来。与 `Pose::poseToWorld` 的区别：
// 那个是 O(n) 全树、这个只走一条链（典型深度 3~8）。
glm::mat4 worldAlongChain(const assets::Skeleton& skel, const Pose& pose,
                          int joint) {
    if (joint < 0 || joint >= skel.jointCount()) return glm::mat4(1.0f);
    // 最多 64 层：正常骨架个位数，加保护是防脏数据成环（`parent` 互相指）。
    int guard = 0;
    glm::mat4 acc = glm::mat4(1.0f);
    // 从目标骨一路向上收集，再自顶向下累乘 —— 用一个小数组（64 深足够）。
    int chain[64];
    int depth = 0;
    int j = joint;
    while (j >= 0 && j < skel.jointCount() && depth < 64) {
        chain[depth++] = j;
        j = skel.joints[static_cast<size_t>(j)].parent;
        if (++guard > 64) break;
    }
    // 链顶那根骨的"父"是 externalChain（骨架挂在 armature 下的那几层）
    const int topIdx = chain[depth - 1];
    acc = skel.joints[static_cast<size_t>(topIdx)].externalChain;
    for (int k = depth - 1; k >= 0; --k) {
        const int cur = chain[k];
        const size_t c = static_cast<size_t>(cur);
        const assets::Skeleton::Joint& jt = skel.joints[c];
        const bool inPose = cur < pose.count();
        const glm::vec3 t = inPose ? pose.translation[c]
                                   : glm::vec3(jt.restLocal[3]);
        const glm::quat r = inPose ? pose.rotation[c]
                                   : glm::quat_cast(glm::mat3(jt.restLocal));
        const glm::vec3 s = inPose
                                ? pose.scale[c]
                                : glm::vec3(glm::length(jt.restLocal[0]),
                                            glm::length(jt.restLocal[1]),
                                            glm::length(jt.restLocal[2]));
        acc = acc * composeLocalTRS(t, r, s);
    }
    return acc;
}

// 引擎世界 up（Z-up）。写死而不是从 camera 取：根运动是**世界**语义，
// 跟摄像机朝哪无关（第一版要是跟着相机 up，角色走路方向会随视角转 ——
// 而且只在特定机位下看得出来）。
constexpr glm::vec3 kEngineUp{0.0f, 0.0f, 1.0f};

} // namespace

int resolveRootBone(const assets::Skeleton& skel, const std::string& name) {
    const int n = skel.jointCount();
    if (n <= 0) return -1;
    if (!name.empty()) {
        // 走全仓库那一套匹配（不分大小写 / 包含 / 跳过末端辅助骨）
        return skel.findJoint(name);
    }
    // 自动：拓扑序里第一根 parent == -1 的骨。
    // ⚠ 用 topoOrder 而不是 0 号下标：`joints` 的顺序是 glTF skin 里的
    //   声明顺序（可以任意），而 topoOrder 保证"根先被访问"。
    for (int j : skel.topoOrder) {
        if (j >= 0 && j < n && skel.joints[static_cast<size_t>(j)].parent < 0)
            return j;
    }
    return 0;   // 全是子骨（脏数据）：退化成 0 号，别整条链关掉
}

glm::mat4 boneWorldInSkeleton(const assets::Skeleton& skel, const Pose& pose,
                              int joint) {
    return worldAlongChain(skel, pose, joint);
}

RootMotionDelta extractRootMotion(const assets::Skeleton& skel,
                                  const glm::mat4& prevWorld,
                                  const glm::mat4& curWorld,
                                  bool horizontalOnly) {
    RootMotionDelta out;

    // 骨架空间 → 引擎空间。只取 3x3：位移是方向量，不该吃平移；
    // 但**要吃缩放**（导入缩放在 `skeletonToEngine` 里，见 MeshLoader）。
    const glm::mat3 toEngine = glm::mat3(skel.skeletonToEngine);

    const glm::vec3 p0 = toEngine * glm::vec3(prevWorld[3]);
    const glm::vec3 p1 = toEngine * glm::vec3(curWorld[3]);
    glm::vec3 d = p1 - p0;
    if (horizontalOnly) d.z = 0.0f;   // 引擎 up = +Z
    out.position = d;

    // 旋转：转到引擎空间后再做 swing-twist。
    // ⚠ `glm::mat3` 里如果有非均匀缩放，`quat_cast` 的结果不是纯旋转 ——
    //   先 normalize 掉（列归一化）。人形骨架的根基本是单位缩放，这一步
    //   是给"导入缩放 ≠ 1"的情况兜底。
    glm::mat3 r0 = toEngine * glm::mat3(prevWorld);
    glm::mat3 r1 = toEngine * glm::mat3(curWorld);
    for (int c = 0; c < 3; ++c) {
        const float len0 = glm::length(r0[c]);
        const float len1 = glm::length(r1[c]);
        if (len0 > 1.0e-8f) r0[c] /= len0;
        if (len1 > 1.0e-8f) r1[c] /= len1;
    }
    const glm::quat q0 = glm::quat_cast(r0);
    const glm::quat q1 = glm::quat_cast(r1);
    out.yaw = twistAngleAround(q1 * glm::conjugate(q0), kEngineUp);

    out.valid = true;
    return out;
}

glm::vec3 boneWorldPosition(const assets::Skeleton& skel, const Pose& pose,
                            int joint) {
    const glm::mat4 w = worldAlongChain(skel, pose, joint);
    return glm::mat3(skel.skeletonToEngine) * glm::vec3(w[3]);
}

glm::vec3 stripRootMotion(const assets::Skeleton& skel, Pose& pose, int joint,
                          const glm::vec3& curWorldPos,
                          const glm::vec3& baseWorldPos, bool horizontalOnly) {
    glm::vec3 off = curWorldPos - baseWorldPos;
    if (horizontalOnly) off.z = 0.0f;   // 引擎 up = +Z
    if (off.x == 0.0f && off.y == 0.0f && off.z == 0.0f) return off;
    if (joint < 0 || joint >= pose.count()) return glm::vec3(0.0f);
    const size_t j = static_cast<size_t>(joint);

    // 引擎空间 → 骨架空间（extract 的逆）
    const glm::mat3 toSkeleton = glm::inverse(glm::mat3(skel.skeletonToEngine));

    // 骨架空间 → **根骨的父空间**（`pose.translation` 是相对父的量）
    glm::mat4 parentW(1.0f);
    const int p = skel.joints[j].parent;
    if (p >= 0) {
        parentW = worldAlongChain(skel, pose, p);
    } else {
        parentW = skel.joints[j].externalChain;
    }
    const glm::mat3 parentInv = glm::inverse(glm::mat3(parentW));
    if (!std::isfinite(parentInv[0][0])) return glm::vec3(0.0f);  // 退化父矩阵

    pose.translation[j] -= parentInv * (toSkeleton * off);
    return off;
}

float twistAngleAround(const glm::quat& qIn, const glm::vec3& axis) {
    // swing-twist 分解：把 q 拆成"绕 axis 的扭转"×"剩下的摆动"。
    // 设 v = (q.x,q.y,q.z)（即 sin(θ/2)·axis），投影到 axis 上：
    //   扭转分量 = (proj, q.w)，归一化后求角 = 2·atan2(proj, w)。
    glm::vec3 a = axis;
    const float alen = glm::length(a);
    if (alen < 1.0e-8f) return 0.0f;
    a /= alen;

    glm::quat q = qIn;
    // 四元数双覆盖：q 与 -q 表示同一个旋转，取 w >= 0 的那一支才能让
    // 角度连续（不取的话 180° 附近会突然翻符号，表现为"转身时抖一下"）。
    if (q.w < 0.0f) q = glm::quat(-q.w, -q.x, -q.y, -q.z);

    const glm::vec3 v(q.x, q.y, q.z);
    const float proj = glm::dot(v, a);
    // 扭转四元数 = 归一化(proj·a + q.w)，角度 = 2·atan2(|proj|·sign, q.w)
    const float twistW = q.w;
    const float len = std::sqrt(proj * proj + twistW * twistW);
    if (len < 1.0e-8f) return 0.0f;
    float angle = 2.0f * std::atan2(proj, twistW);
    // 归到 [-π, π]
    const float twoPi = 6.28318530718f;
    while (angle > 3.14159265359f) angle -= twoPi;
    while (angle < -3.14159265359f) angle += twoPi;
    return angle;
}

} // namespace animation
