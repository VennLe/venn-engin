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

// ============================================================
// 骨名索引 —— 抄 UE `FReferenceSkeleton::BoneNameToIndexMap` 的建表思路
//
// 旧实现（2026-11-05 之前）每次 `findJoint` 都：
//     for 每根骨 { 复制一份小写串; 比较 }
// 100 根骨 × 每帧十几次调用 = 每帧上千次 std::string 构造 + 释放。
// 这不是"理论上慢"，而是把一块**常量数据**反复算成了临时对象。
//
// 现在：第一次调用建一次表，之后精确轮是 O(1) 哈希、包含轮只读现成的小写
// 数组（仍然是 O(n) 扫描 —— 那一轮本质上就是扫描，没有更快的正确做法）。
// ============================================================
void Skeleton::buildNameIndex() const {
    const int n = jointCount();
    m_lowerNames.resize(static_cast<size_t>(n));
    m_exactByName.clear();
    m_lowerByName.clear();
    // 重复名很少见，按骨数 reserve 就够（多了会 rehash，不如一次留够）。
    m_exactByName.reserve(static_cast<size_t>(n) * 2);
    m_lowerByName.reserve(static_cast<size_t>(n) * 2);

    for (int i = 0; i < n; ++i) {
        const std::string& nm = joints[static_cast<size_t>(i)].name;
        m_lowerNames[static_cast<size_t>(i)] = asciiLower(nm);
        // ⚠ `emplace` 在键已存在时**不覆盖** —— 这正是旧线性实现"重复名
        //   返回数组里第一个"的语义。改成 `insert_or_assign` 就会变成
        //   "返回最后一个"，而 `head` / `head_01` 这类重名在合并过的骨架里
        //   是真实存在的，症状是"挂点挂到了另一根骨上"。
        m_exactByName.emplace(nm, i);
        m_lowerByName.emplace(m_lowerNames[static_cast<size_t>(i)], i);
    }
    m_nameIndexSize = joints.size();
    m_nameIndexReady = true;
}

void Skeleton::ensureNameIndex() const {
    if (m_nameIndexReady && m_nameIndexSize == joints.size()) return;
    buildNameIndex();
}

int Skeleton::nameIndexEntryCount() const {
    ensureNameIndex();
    return static_cast<int>(m_lowerByName.size());
}

int Skeleton::findJoint(const std::string& name, unsigned flags) const {
    const int n = jointCount();
    if (n <= 0) return -1;

    // 空名字 = "没指定"。**不是**匹配第 0 根 —— 调用方（LookAt / IK）
    // 把空名字当"自动挑一根"，那个语义在上层，不在这里。
    if (name.empty()) return -1;

    const bool ci = (flags & kJointMatchCaseInsensitive) != 0u;
    const bool sub = (flags & kJointMatchSubstring) != 0u;
    const bool skipTips = (flags & kJointMatchSkipEndTips) != 0u;

    ensureNameIndex();

    // ---- 第一轮：精确 ----
    // 精确优先是硬要求：`hand_l` 用包含匹配会先撞上 `hand_l_end`（如果它在
    // 数组里更靠前），而两根骨的旋转轴完全不同。
    //
    // ⚠ 这一轮**不过滤** end/tip：显式写出了骨骼全名的人要的就是那一根。
    //   过滤掉等于"精确匹配返回 -1"，比匹配错更难查 —— 调用方会以为
    //   资产里没有这根骨。
    if (ci) {
        const auto it = m_lowerByName.find(asciiLower(name));
        if (it != m_lowerByName.end()) return it->second;
    } else {
        const auto it = m_exactByName.find(name);
        if (it != m_exactByName.end()) return it->second;
    }

    // ---- 第二轮：包含（这一轮才跳末端辅助骨）----
    if (!sub) return -1;
    const std::string want = ci ? asciiLower(name) : name;
    for (int i = 0; i < n; ++i) {
        const std::string& cur = ci ? m_lowerNames[static_cast<size_t>(i)]
                                    : joints[static_cast<size_t>(i)].name;
        if (skipTips && isEndTip(cur)) continue;
        if (cur.find(want) != std::string::npos) return i;
    }
    return -1;
}

void Skeleton::findJointsWithPrefix(const std::string& prefix,
                                    std::vector<int>& out) const {
    if (prefix.empty()) return;
    ensureNameIndex();
    const int n = jointCount();
    const std::string want = asciiLower(prefix);
    for (int i = 0; i < n; ++i)
        if (startsWith(m_lowerNames[static_cast<size_t>(i)], want))
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

// ============================================================
// 挂点
// ============================================================
int Skeleton::findSocket(const std::string& name) const {
    // 与 `findJoint` 同一口径：先精确、不分大小写。搬过来只为让
    // "查挂点"和"查骨"的手感一致（写 `Weapon_R` 也能找到 `weapon_r`）。
    auto lower = [](const std::string& s) {
        std::string o = s;
        for (char& c : o)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return o;
    };
    const std::string want = lower(name);
    for (std::size_t i = 0; i < sockets.size(); ++i)
        if (lower(sockets[i].name) == want) return static_cast<int>(i);
    return -1;
}

const Skeleton::Socket* Skeleton::socket(const std::string& name) const {
    const int i = findSocket(name);
    if (i < 0) return nullptr;
    const Socket& s = sockets[static_cast<std::size_t>(i)];
    // 名字在但骨没解析出来 —— 这**不是**"挂点存在"。返回 nullptr，
    // 让调用方走"没有这个挂点"的分支，而不是拿到一个指向 -1 号骨的
    // 挂点然后在别处越界。
    return s.bone >= 0 ? &s : nullptr;
}

int Skeleton::resolveSockets() {
    int bad = 0;
    for (Socket& s : sockets) {
        // 走 `findJoint` 的全套退化（不分大小写 + 允许包含 + 跳末端骨）：
        // 资产里的骨名和运行时骨名在重定向/改名之后可能只差前半段。
        s.bone = findJoint(s.boneName);
        if (s.bone < 0) ++bad;
    }
    return bad;
}

bool Skeleton::socketWorld(const std::string& name,
                           const std::vector<glm::mat4>& world,
                           glm::mat4& out) const {
    const Socket* s = socket(name);
    if (s == nullptr) return false;
    if (s->bone < 0 || static_cast<std::size_t>(s->bone) >= world.size())
        return false;
    out = world[static_cast<std::size_t>(s->bone)] * s->local;
    return true;
}

} // namespace assets
