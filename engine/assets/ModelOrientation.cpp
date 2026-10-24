#include "ModelOrientation.h"

#include "Model.h"
#include "Skeleton.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace assets {

namespace {

constexpr float kPi = 3.14159265f;

bool containsCI(const std::string& s, const char* needle) {
    const std::size_t n = std::strlen(needle);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i + n <= s.size(); ++i) {
        bool same = true;
        for (std::size_t k = 0; k < n; ++k) {
            if (std::tolower(static_cast<unsigned char>(s[i + k])) !=
                std::tolower(static_cast<unsigned char>(needle[k]))) {
                same = false;
                break;
            }
        }
        if (same) return true;
    }
    return false;
}

// 关节的**静止世界位置**。骨架里只存 restLocal，父链要自己乘。
std::vector<glm::vec3> restWorldHeads(const Skeleton& sk) {
    std::vector<glm::vec3> heads(sk.joints.size(), glm::vec3(0.0f));
    std::vector<glm::mat4> world(sk.joints.size(), glm::mat4(1.0f));
    // topoOrder 保证父先于子；万一它不完整（老资产），按 joints 顺序兜一遍
    // —— 那种情况父链可能算错，但比"整段返回 false"更有机会救回来。
    std::vector<int> order = sk.topoOrder;
    if (order.size() != sk.joints.size()) {
        order.clear();
        for (int i = 0; i < static_cast<int>(sk.joints.size()); ++i)
            order.push_back(i);
    }
    for (int idx : order) {
        if (idx < 0 || idx >= static_cast<int>(sk.joints.size())) continue;
        const auto& j = sk.joints[static_cast<std::size_t>(idx)];
        const glm::mat4 parentW =
            (j.parent >= 0 && j.parent < static_cast<int>(world.size()))
                ? world[static_cast<std::size_t>(j.parent)]
                : glm::mat4(1.0f);
        world[static_cast<std::size_t>(idx)] = parentW * j.restLocal;
        heads[static_cast<std::size_t>(idx)] =
            glm::vec3(world[static_cast<std::size_t>(idx)][3]);
    }
    return heads;
}

float snapToQuadrant(float deg) {
    return std::round(deg / 90.0f) * 90.0f;
}

} // namespace

bool modelForwardYawDeg(const Model& m, float* outDeg,
                        glm::vec2* outLocalForward, float* outRawDeg) {
    if (!m.skeleton || m.skeleton->joints.empty()) return false;
    const Skeleton& sk = *m.skeleton;
    const std::vector<glm::vec3> head = restWorldHeads(sk);

    // 逐根"脚趾"骨找它的父（= 脚踝），累加脚尖方向。
    //
    // ⚠ 不要只看"名字里有 foot 的骨的朝向"：脚踝骨的 +Z/+Y 轴朝向在
    //   各家 rig 里不一致（VRM 的 rest 轴甚至是歪的），而"脚踝 → 脚趾"
    //   这条**位移**是纯几何量，与骨轴无关。
    glm::vec2 sum(0.0f);
    int pairs = 0;
    glm::vec2 firstDir(0.0f);
    for (int i = 0; i < static_cast<int>(sk.joints.size()); ++i) {
        const auto& j = sk.joints[static_cast<std::size_t>(i)];
        if (!containsCI(j.name, "toe")) continue;
        if (j.parent < 0) continue;
        const glm::vec3 d = head[static_cast<std::size_t>(i)] -
                            head[static_cast<std::size_t>(j.parent)];
        const glm::vec2 d2(d.x, d.y);
        const float len = glm::length(d2);
        if (len < 1.0e-4f) continue;
        const glm::vec2 n = d2 / len;
        if (pairs == 0) firstDir = n;
        // 两脚的脚尖方向必须大致一致（人不会一只脚朝前一只朝后）。
        if (glm::dot(n, firstDir) < 0.7071f) continue;   // > 45° 视为异常
        sum += n;
        ++pairs;
    }
    if (pairs == 0) return false;
    const float len = glm::length(sum);
    if (len < 1.0e-3f) return false;      // 对称抵消（朝向不定）
    const glm::vec2 fwd = sum / len;

    // 想把 fwd 转到 +X：R(θ)·fwd = (|fwd|, 0)  →  θ = -atan2(fy, fx)
    const float rawDeg = -std::atan2(fwd.y, fwd.x) * (180.0f / kPi);
    const float snapped = snapToQuadrant(rawDeg);

    if (outDeg) *outDeg = snapped;
    if (outLocalForward) *outLocalForward = fwd;
    if (outRawDeg) *outRawDeg = rawDeg;
    return true;
}

} // namespace assets
