// ============================================================
// assets/Skeleton.cpp —— 采样实现
// ============================================================

#include "assets/Skeleton.h"

#include <algorithm>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace assets {

namespace {

// 把 restLocal 分解成 TRS（glm 的 gtx decompose）。
// 采样时对每个 joint 先分解一次 rest，再用动画轨道**覆盖**对应分量，
// 最后重拼 —— 这样同一 joint 的 T/R/S 三条轨道能正确叠加。
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
    const size_t n = joints.size();
    if (out.size() != n) out.assign(n, glm::mat4(1.0f));
    if (n == 0) return;

    // 1) 局部姿态 = rest 分解成 TRS，再按轨道覆盖分量。
    //    同一 joint 的多条轨道（T/R/S）会正确叠加，不会互相覆盖。
    static thread_local std::vector<TRS> local;
    local.resize(n);
    for (size_t j = 0; j < n; ++j) local[j] = decomposeTRS(joints[j].restLocal);

    if (clip) {
        for (const auto& tr : clip->tracks) {
            if (tr.joint < 0 || tr.joint >= static_cast<int>(n)) continue;
            const glm::vec4 v = sampleTrack(tr, time);
            TRS& l = local[tr.joint];
            switch (tr.path) {
                case AnimTrack::Translation:
                    l.translation = glm::vec3(v.x, v.y, v.z);
                    break;
                case AnimTrack::Rotation:
                    l.orient = glm::quat(v.w, v.x, v.y, v.z);  // 存 (x,y,z,w)
                    break;
                case AnimTrack::Scale:
                    l.scale = glm::vec3(v.x, v.y, v.z);
                    break;
            }
        }
    }

    // 2) 沿 topoOrder 拼世界矩阵（parent 保证先算）
    static thread_local std::vector<glm::mat4> world;
    world.resize(n);
    for (int j : topoOrder) {
        if (j < 0 || j >= static_cast<int>(n)) continue;
        const glm::mat4 base =
            joints[j].parent >= 0 ? world[joints[j].parent]
                                  : joints[j].externalChain;
        world[j] = base * composeTRS(local[j]);
    }

    // 3) joint 矩阵（可直接乘顶点，见头文件注释里的空间推导）
    for (size_t j = 0; j < n; ++j)
        out[j] = invMeshNode * world[j] * joints[j].invBind;
}

} // namespace assets