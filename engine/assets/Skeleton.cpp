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
#include <cctype>
#include <cmath>

namespace assets {

namespace {

// ASCII 小写化。**不用 locale**：骨骼名是 ASCII 标识符，而 std::tolower
// 在 C locale 之外会把某些字节映射成多字节序列，比较就永远不相等。
std::string asciiLower(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// 末端辅助骨的判据。为什么是"包含"而不是"以 _end 结尾"：
// Blender 写 `Head_end`，3ds Max 的 Biped 写 `HeadNub`，Mixamo 写
// `HeadTop_End`，UE mannequin 写 `head_end`。三个厂商三种写法，
// 只有"名字里出现 end/tip/nub"是共通的。
bool isEndTip(const std::string& lowered) {
    return lowered.find("end") != std::string::npos ||
           lowered.find("tip") != std::string::npos ||
           lowered.find("nub") != std::string::npos;
}

} // namespace

int Skeleton::findJoint(const std::string& name, unsigned flags) const {
    const int n = jointCount();
    if (n <= 0) return -1;

    // 空名字 = "没指定"。**不是**匹配第 0 根 —— 调用方（LookAt / IK）
    // 把空名字当"自动挑一根"，那个语义在上层，不在这里。
    if (name.empty()) return -1;

    const bool ci = (flags & kJointMatchCaseInsensitive) != 0u;
    const bool sub = (flags & kJointMatchSubstring) != 0u;
    const bool skipTips = (flags & kJointMatchSkipEndTips) != 0u;

    const std::string want = ci ? asciiLower(name) : name;

    // ---- 第一轮：精确 ----
    // 精确优先是硬要求：`hand_l` 用包含匹配会先撞上 `hand_l_end`（如果它在
    // 数组里更靠前），而两根骨的旋转轴完全不同。
    //
    // ⚠ 这一轮**不过滤** end/tip：显式写出了骨骼全名的人要的就是那一根。
    //   过滤掉等于"精确匹配返回 -1"，比匹配错更难查 —— 调用方会以为
    //   资产里没有这根骨。
    for (int i = 0; i < n; ++i) {
        const std::string& nm = joints[static_cast<size_t>(i)].name;
        const std::string cur = ci ? asciiLower(nm) : nm;
        if (cur == want) return i;
    }

    // ---- 第二轮：包含（这一轮才跳末端辅助骨）----
    if (!sub) return -1;
    for (int i = 0; i < n; ++i) {
        const std::string& nm = joints[static_cast<size_t>(i)].name;
        const std::string cur = ci ? asciiLower(nm) : nm;
        if (skipTips && isEndTip(cur)) continue;
        if (cur.find(want) != std::string::npos) return i;
    }
    return -1;
}

void Skeleton::findJointsWithPrefix(const std::string& prefix,
                                    std::vector<int>& out) const {
    if (prefix.empty()) return;
    const int n = jointCount();
    const std::string want = asciiLower(prefix);
    for (int i = 0; i < n; ++i)
        if (startsWith(asciiLower(joints[static_cast<size_t>(i)].name), want))
            out.push_back(i);
}

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