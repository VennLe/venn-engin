// ============================================================
// animation/Pose.cpp —— 采样 / 关节矩阵 / 混合
//
// ⚠ 这里的 decompose / compose 表达式与 `assets/Skeleton.cpp` 拆分前
//   **逐字符一致**（同样的 glm::decompose、同样的
//   `translate * mat4_cast * scale` 顺序）。A0-5 的验收是"重构前后
//   关节矩阵 memcmp 级相同"，任何"顺手优化"都会让那条断言变红。
// ============================================================

#include "animation/Pose.h"

#include <algorithm>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace animation {

namespace {

struct TRS {
    glm::vec3 scale{1.0f};
    glm::quat orient{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 translation{0.0f};
};

TRS decomposeTRS(const glm::mat4& m) {
    TRS r;
    glm::vec3 skew(0.0f);
    glm::vec4 persp(0.0f);
    glm::decompose(m, r.scale, r.orient, r.translation, skew, persp);
    return r;
}

glm::mat4 composeTRS(const TRS& t) {
    return glm::translate(glm::mat4(1.0f), t.translation) *
           glm::mat4_cast(t.orient) * glm::scale(glm::mat4(1.0f), t.scale);
}

} // namespace

void Pose::resize(int n) {
    if (n < 0) n = 0;
    translation.resize(static_cast<size_t>(n), glm::vec3(0.0f));
    rotation.resize(static_cast<size_t>(n), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    scale.resize(static_cast<size_t>(n), glm::vec3(1.0f));
}

void Pose::setRest(const assets::Skeleton& s) {
    const int n = s.jointCount();
    resize(n);
    for (int j = 0; j < n; ++j) {
        const TRS t = decomposeTRS(s.joints[static_cast<size_t>(j)].restLocal);
        const size_t k = static_cast<size_t>(j);
        scale[k] = t.scale;
        rotation[k] = t.orient;
        translation[k] = t.translation;
    }
}

void samplePose(const assets::Skeleton& skel, const assets::AnimationClip* clip,
                float time, Pose& out) {
    out.setRest(skel);
    if (!clip) return;

    const int n = out.count();
    for (const auto& tr : clip->tracks) {
        if (tr.joint < 0 || tr.joint >= n) continue;
        const glm::vec4 v = assets::sampleTrack(tr, time);
        const size_t k = static_cast<size_t>(tr.joint);
        switch (tr.path) {
            case assets::AnimTrack::Translation:
                out.translation[k] = glm::vec3(v.x, v.y, v.z);
                break;
            case assets::AnimTrack::Rotation:
                // 轨道值存 (x,y,z,w)，glm::quat 构造是 (w,x,y,z)
                out.rotation[k] = glm::quat(v.w, v.x, v.y, v.z);
                break;
            case assets::AnimTrack::Scale:
                out.scale[k] = glm::vec3(v.x, v.y, v.z);
                break;
        }
    }
}

void poseToWorld(const assets::Skeleton& skel, const Pose& pose,
                 std::vector<glm::mat4>& out) {
    const int n = skel.jointCount();
    const size_t un = static_cast<size_t>(n);
    if (out.size() != un) out.assign(un, glm::mat4(1.0f));
    if (n == 0) return;

    const int pn = pose.count();
    for (int j : skel.topoOrder) {
        if (j < 0 || j >= n) continue;
        const size_t k = static_cast<size_t>(j);
        const assets::Skeleton::Joint& jt = skel.joints[k];

        TRS local;
        if (j < pn) {
            local.translation = pose.translation[k];
            local.orient = pose.rotation[k];
            local.scale = pose.scale[k];
        } else {
            // 姿态比骨架短（不该发生，防御性取值）
            local = decomposeTRS(jt.restLocal);
        }

        const glm::mat4 base = jt.parent >= 0
                                   ? out[static_cast<size_t>(jt.parent)]
                                   : jt.externalChain;
        out[k] = base * composeTRS(local);
    }
}

void poseToJoints(const assets::Skeleton& skel, const Pose& pose,
                  std::vector<glm::mat4>& out) {
    const int n = skel.jointCount();
    const size_t un = static_cast<size_t>(n);
    if (out.size() != un) out.assign(un, glm::mat4(1.0f));
    if (n == 0) return;

    // 世界矩阵。放在**局部**缓冲里，不再用 static thread_local ——
    // 嵌套调用（分层 / IK 里回采）不会再互相踩。
    // （每个角色每帧一次 4KB 级的分配；A8 若量出这是热点，再往 Pose 里
    //   挂一个复用缓冲，不要退回 thread_local。）
    std::vector<glm::mat4> world;
    poseToWorld(skel, pose, world);

    for (size_t j = 0; j < un; ++j)
        out[j] = skel.invMeshNode * world[j] * skel.joints[j].invBind;
}

void blendPose(const Pose& a, const Pose& b, float w, Pose& out) {
    // 端点短路：见头文件说明（slerp 在浮点下不是恒等）。
    if (w <= 0.0f) {
        out = a;
        return;
    }
    if (w >= 1.0f) {
        out = b;
        return;
    }

    const int na = a.count();
    const int nb = b.count();
    const int n = std::min(na, nb);
    out.resize(std::max(na, nb));
    const size_t un = static_cast<size_t>(n);

    for (size_t j = 0; j < un; ++j) {
        out.translation[j] = glm::mix(a.translation[j], b.translation[j], w);
        // nlerp 还是 slerp？这里用 slerp —— 与"按角度插值"的直觉一致，
        // 且 A2 的断言 3 就是拿 slerp(qa,qb,0.5) 对数的。
        out.rotation[j] = glm::slerp(a.rotation[j], b.rotation[j], w);
        out.scale[j] = glm::mix(a.scale[j], b.scale[j], w);
    }
    // 长度不等时，多出来的骨骼原样保留 a 的值（b 没有这些骨骼的信息）
    for (int j = n; j < na; ++j) {
        const size_t k = static_cast<size_t>(j);
        out.translation[k] = a.translation[k];
        out.rotation[k] = a.rotation[k];
        out.scale[k] = a.scale[k];
    }
}

} // namespace animation
