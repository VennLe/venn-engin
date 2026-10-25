// ============================================================
// animation/TwoBoneIK.cpp
//
// 数据流：
//   pose --FK--> 世界 A/B/C --余弦定理--> 新 B --最短弧--> root 局部旋转
//        --再FK(只需父骨)--> mid 局部旋转 --再FK--> 实测末端
//
// ⚠ 第二根骨的"父骨世界旋转"必须用**改完之后**的 root，不是改之前的。
//   用错的那一版看起来也能动，但 mid 的旋转被套在一个已经转过的坐标系里，
//   症状是"腿越弯，脚踝偏得越多"，而伸直时完全正常 —— 极难从画面反推。
// ============================================================

#include "animation/TwoBoneIK.h"

#include "animation/SpringBone.h"   // shortestArc（与弹簧骨 / LookAt 共用）

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace animation {

namespace {

glm::vec3 translationOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// 世界矩阵 → 旋转（去掉缩放）。
//
// ⚠ 不用 `quat_cast(mat3(m) / determinant)`（LookAt 里那一版）：均匀缩放 s
//   的 3x3 行列式是 s³，除完得到的是 s·R/s³ = R/s²，既不正交也不归一，
//   `quat_cast` 会在非正交矩阵上给出近似值。按**第 0 列的长度**除才对
//   （均匀缩放下三列长度相同）。非均匀缩放本身在这条链上就没有良定义
//   （DCC 导出的骨架都是均匀缩放），这里不假装支持它。
glm::quat worldRotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    const float s = glm::length(r[0]);
    if (s > 1.0e-8f) r /= s;
    return glm::normalize(glm::quat_cast(r));
}

// root 的父骨世界旋转（root 是骨架根时用 externalChain）。
glm::quat parentRotationOf(const assets::Skeleton& skel,
                           const std::vector<glm::mat4>& world, int joint) {
    const int p = skel.joints[static_cast<size_t>(joint)].parent;
    if (p >= 0 && p < static_cast<int>(world.size()))
        return worldRotationOf(world[static_cast<size_t>(p)]);
    return worldRotationOf(skel.joints[static_cast<size_t>(joint)].externalChain);
}

// 父骨是 `parent` 的所有骨里**restLocal 平移最长**的那一根。
// restLocal 是父骨相对量，所以它的平移长度就是骨长 —— 不需要 FK。
int longestChild(const assets::Skeleton& skel, int parent) {
    int best = -1;
    float bestLen = 0.0f;
    const int n = skel.jointCount();
    for (int j = 0; j < n; ++j) {
        if (skel.joints[static_cast<size_t>(j)].parent != parent) continue;
        const float len =
            glm::length(translationOf(skel.joints[static_cast<size_t>(j)].restLocal));
        if (len <= bestLen) continue;   // 平手时取下标小的（稳定）
        bestLen = len;
        best = j;
    }
    // 零长度的子骨（DCC 里常见的"同点辅助骨"）不算候选：拿它当 chain 的
    // 中间骨会让 L1 = 0，下面除以 L1 直接出 NaN。
    return bestLen > 1.0e-5f ? best : -1;
}

// 自动挑 root 时依次尝试的骨名（不区分大小写、允许包含）。
// **腿在前**：两骨 IK 的主战场是"脚贴地"，而手部抓握大多数时候用
// LookAt 式朝向 + 动画本身就够了。宁可默认选中腿，也不要默认不生效。
const char* const kAutoRootNames[] = {"thigh", "upleg", "upperleg",
                                      "upperarm", "uparm"};
constexpr int kAutoRootCount = 5;

} // namespace

TwoBoneIKChain resolveTwoBoneChain(const assets::Skeleton& skel,
                                   const TwoBoneIKSettings& s) {
    TwoBoneIKChain ch;
    const int n = skel.jointCount();
    if (n <= 0) return ch;

    const unsigned flags = assets::kJointMatchCaseInsensitive |
                           assets::kJointMatchSubstring |
                           assets::kJointMatchSkipEndTips;

    // ---- root ----
    if (!s.rootBone.empty()) {
        ch.root = skel.findJoint(s.rootBone, flags);
    } else {
        for (int i = 0; i < kAutoRootCount && ch.root < 0; ++i)
            ch.root = skel.findJoint(kAutoRootNames[i], flags);
    }
    if (ch.root < 0) return TwoBoneIKChain{};

    // ---- mid ----
    if (!s.midBone.empty()) ch.mid = skel.findJoint(s.midBone, flags);
    else ch.mid = longestChild(skel, ch.root);
    if (ch.mid < 0) return TwoBoneIKChain{};
    // 显式名字配错（找得到但那根骨不是 root 的子）→ 整条链拒绝，
    // 不"就近凑一根"。凑出来的链会在下面用错父骨坐标系。
    if (skel.joints[static_cast<size_t>(ch.mid)].parent != ch.root)
        return TwoBoneIKChain{};

    // ---- end ----
    if (!s.endBone.empty()) ch.end = skel.findJoint(s.endBone, flags);
    else ch.end = longestChild(skel, ch.mid);
    if (ch.end < 0) return TwoBoneIKChain{};
    if (skel.joints[static_cast<size_t>(ch.end)].parent != ch.mid)
        return TwoBoneIKChain{};

    return ch;
}

TwoBoneIKResult solveTwoBoneIK(const assets::Skeleton& skel,
                               const TwoBoneIKSettings& s, Pose& pose) {
    TwoBoneIKResult r;

    // weight = 0 / 关掉 / 没目标 → **完全不动 pose**。
    // 这条短路是"关掉功能逐像素回到基线"的唯一保证：晚一行就会有一次
    // slerp，而 slerp(q,q,0) 在浮点下不是恒等。
    if (!s.enabled || !s.hasTarget || s.weight <= 0.0f) return r;

    const int n = skel.jointCount();
    if (n <= 0 || pose.count() < n) return r;

    const TwoBoneIKChain ch = resolveTwoBoneChain(skel, s);
    if (!ch.valid()) return r;
    r.root = ch.root;
    r.mid = ch.mid;
    r.end = ch.end;

    std::vector<glm::mat4> world;
    poseToWorld(skel, pose, world);

    const glm::vec3 A = translationOf(world[static_cast<size_t>(ch.root)]);
    const glm::vec3 B = translationOf(world[static_cast<size_t>(ch.mid)]);
    const glm::vec3 C = translationOf(world[static_cast<size_t>(ch.end)]);

    const glm::vec3 ab = B - A;
    const glm::vec3 bc = C - B;
    const float L1 = glm::length(ab);
    const float L2 = glm::length(bc);
    if (L1 < 1.0e-5f || L2 < 1.0e-5f) return r;   // 退化骨（重合）

    const glm::vec3 toTarget = s.target - A;
    const float rawD = glm::length(toTarget);
    r.targetDist = rawD;
    r.errorBefore = glm::length(s.target - C);

    // 目标点与 root 重合：方向未定义。**不动** —— 保持动画姿势比瞎转一根
    // 骨头好（LookAt 里同一条处理）。
    if (rawD < 1.0e-6f) return r;
    const glm::vec3 dirAT = toTarget / rawD;

    // ---- 可达区间 ----
    // 下界 |L1−L2|：两骨"折叠到底"的距离。目标比它还近时链折不起来，
    // 只能折到极限（多余的距离变成"够过头"，不可避免）。
    const float dmin = std::fabs(L1 - L2) + 1.0e-4f;
    const float dmax = (L1 + L2) * std::clamp(s.maxReachScale, 0.05f, 1.0f);
    // 极端资产（L1 ≈ L2 且目标极限近）可能让 dmin > dmax，钳一下取中值
    const float lo = std::min(dmin, dmax);
    const float d = std::clamp(rawD, lo, dmax);
    r.reachDist = d;
    const glm::vec3 reachable = A + dirAT * d;

    // ---- 弯曲平面（见头文件：极向量从当帧动画反推）----
    glm::vec3 bend;
    if (s.poleMode == TwoBoneIKSettings::PoleFromVector) {
        bend = s.poleVector - glm::dot(s.poleVector, dirAT) * dirAT;
    } else {
        bend = ab - glm::dot(ab, dirAT) * dirAT;
    }
    if (glm::length(bend) < 1.0e-5f) {
        // 动画里完全伸直（或 poleVector 与目标方向平行）→ 退化。
        // 退到配置的 poleVector 的垂直分量。
        const glm::vec3 p = (glm::length(s.poleVector) > 1.0e-6f)
                                ? s.poleVector
                                : glm::vec3(0.0f, 0.0f, 1.0f);
        bend = p - glm::dot(p, dirAT) * dirAT;
    }
    if (glm::length(bend) < 1.0e-5f) {
        // 仍然平行 → 任取一个与 dirAT 不平行的轴叉乘。
        // ⚠ 必须分两次试：dirAT 恰好是 (1,0,0) 时第一次叉乘为零向量，
        //   normalize 出 NaN，整条链消失。
        bend = glm::cross(dirAT, glm::vec3(1.0f, 0.0f, 0.0f));
        if (glm::length(bend) < 1.0e-5f)
            bend = glm::cross(dirAT, glm::vec3(0.0f, 1.0f, 0.0f));
        if (glm::length(bend) < 1.0e-5f) return r;
    }
    const glm::vec3 bendDir = glm::normalize(bend);

    // ---- 余弦定理 ----
    float cosA = (L1 * L1 + d * d - L2 * L2) / (2.0f * L1 * d);
    cosA = std::clamp(cosA, -1.0f, 1.0f);
    const float angA = std::acos(cosA);

    float cosB = (L1 * L1 + L2 * L2 - d * d) / (2.0f * L1 * L2);
    cosB = std::clamp(cosB, -1.0f, 1.0f);
    // 弯曲角：180° − 内角。0 = 完全伸直，180 = 折到底。
    r.bendAngleDeg = 180.0f - glm::degrees(std::acos(cosB));

    // 新的 mid 位置。dirAT ⟂ bendDir 且两者都是单位向量 →
    // (dirAT·cos + bendDir·sin) 是单位向量，× L1 恰好落在半径 L1 的球上。
    const glm::vec3 newB =
        A + (dirAT * std::cos(angA) + bendDir * std::sin(angA)) * L1;

    // ---- ① root：把它到 mid 的方向转到新方向 ----
    const glm::quat qA = shortestArc(glm::normalize(ab), glm::normalize(newB - A));

    const glm::quat rootParentRot = parentRotationOf(skel, world, ch.root);
    const glm::quat rootLocalOld = pose.rotation[static_cast<size_t>(ch.root)];
    // 世界增量 q 转成局部增量：R'_local = P⁻¹ · q · P · R_local
    //（P = 父骨世界旋转）。等价于把 q 用 P 共轭到父骨空间。
    const glm::quat rootDeltaLocal =
        glm::inverse(rootParentRot) * qA * rootParentRot;
    const glm::quat rootLocalNew =
        glm::normalize(rootDeltaLocal * rootLocalOld);

    // ---- ② mid：父骨（root）的世界旋转**已经变了** ----
    // W'(root) = qA · W(root)。下面 mid 的局部增量必须用 W'(root) 共轭，
    // 用旧的 W(root) 就是文件头那条"腿越弯脚踝偏得越多"。
    const glm::quat rootWorldRotNew =
        glm::normalize(qA * worldRotationOf(world[static_cast<size_t>(ch.root)]));

    // root 转过之后，mid→end 的方向也整体转了 qA（刚体旋转不改变骨长）。
    const glm::vec3 bcAfter = qA * glm::normalize(bc);
    const glm::vec3 wantDir = glm::normalize(reachable - newB);
    const glm::quat qB = shortestArc(bcAfter, wantDir);

    const glm::quat midLocalOld = pose.rotation[static_cast<size_t>(ch.mid)];
    const glm::quat midDeltaLocal =
        glm::inverse(rootWorldRotNew) * qB * rootWorldRotNew;
    const glm::quat midLocalNew = glm::normalize(midDeltaLocal * midLocalOld);

    // ---- ③ 按权重写回 ----
    // 两根骨各插一半：weight < 1 时末端**不在**目标上（那是"半开 IK"的
    // 定义）。要"半开"仍然落在目标上，正确做法是把目标点插值到当前末端，
    // 而不是插值旋转 —— 那是另一种语义（"软 IK"），不进这一刀。
    const float w = std::min(1.0f, s.weight);
    if (w >= 1.0f) {
        pose.rotation[static_cast<size_t>(ch.root)] = rootLocalNew;
        pose.rotation[static_cast<size_t>(ch.mid)] = midLocalNew;
    } else {
        pose.rotation[static_cast<size_t>(ch.root)] =
            glm::normalize(glm::slerp(rootLocalOld, rootLocalNew, w));
        pose.rotation[static_cast<size_t>(ch.mid)] =
            glm::normalize(glm::slerp(midLocalOld, midLocalNew, w));
    }

    // ---- ④ 实测末端误差 ----
    // 走一次完整 FK 而不是解析推 |reachable − target|：weight < 1 时
    // 解析值恒为 0，日志会骗人（见头文件）。
    {
        std::vector<glm::mat4> after;
        poseToWorld(skel, pose, after);
        r.errorAfter = glm::length(s.target -
                                   translationOf(after[static_cast<size_t>(ch.end)]));
    }

    r.applied = true;
    return r;
}

} // namespace animation
