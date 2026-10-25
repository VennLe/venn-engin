// ============================================================
// animation/SpringBone.cpp
//
// 数据流：pose --FK--> 世界位置/旋转 --Verlet--> 新位置 --最短弧--> 新局部旋转
// 三个"看起来能省、其实不能省"的地方在下面各段的注释里标了 ⚠。
// ============================================================

#include "animation/SpringBone.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace animation {

// ⚠ 默认前缀是**裸 `hair` / `skirt`，不带分隔符**。
//   第一版写的是 `hair_` / `hair.`（Blender 的 `hair_01` 与 Max 的
//   `hair.001` 都覆盖），结果在真实资产上**一个都没匹配到**：
//   `assets/models/characters/alicia.vmesh` 的头发骨叫
//   `hair1_L` / `hair2_L` / `hair3_L` / `hair4_L` —— 下划线在数字**后面**，
//   两种带分隔符的前缀都落空。症状是"接上功能了但头发一动不动"，
//   而且 Inspector 上显示 chains: 0，很容易被当成"资产没绑骨"。
//
// ⚠ 也**不放 `tail`**：人体骨架里 tail 常常指尾椎（tailbone），
//   默认匹配会让脊椎末端跟着重力飘（整条脊柱软掉）。兽娘的尾巴
//   请手动填前缀 —— 默认值应该是"绝不会误伤"的那一组。
const char* const kDefaultSpringPrefixes[] = {"hair", "skirt"};
const int kDefaultSpringPrefixCount = 2;

namespace {

// ⚠ `lowered` / `startsWith` / `matchAny` 三个本地函数在第八刀删掉了 ——
//   它们和 `LookAt.cpp` 里的那套是同一件事（不区分大小写的骨骼名匹配），
//   已经收敛到 `Skeleton::findJointsWithPrefix`。留着两份的话，改一处
//   （比如加"跳过末端骨"）另一处不会跟着改。

} // namespace

glm::quat shortestArc(const glm::vec3& from, const glm::vec3& to) {
    const float d = glm::dot(from, to);
    // 平行：恒等（阈值取 1-1e-6，太紧会在数值上翻来覆去）
    if (d > 0.999999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.999999f) {
        // 反向 180°：绕任一垂直轴。挑一个与 from 最不平行的基轴，
        // 否则 cross 出零向量 → normalize 出 NaN → 整条头发消失
        // （在画面上表现为"头发不见了"，日志里一行错都没有）。
        glm::vec3 axis = glm::cross(from, glm::vec3(0.0f, 0.0f, 1.0f));
        if (glm::length(axis) < 1.0e-4f)
            axis = glm::cross(from, glm::vec3(1.0f, 0.0f, 0.0f));
        if (glm::length(axis) < 1.0e-4f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 axis = glm::normalize(glm::cross(from, to));
    const float ang = std::acos(std::max(-1.0f, std::min(1.0f, d)));
    return glm::angleAxis(ang, axis);
}

SpringRig buildSpringRig(const assets::Skeleton& skel,
                         const std::vector<std::string>& prefixes) {
    SpringRig rig;
    const int n = skel.jointCount();
    if (n <= 0) return rig;

    std::vector<std::string> pfx;
    if (prefixes.empty()) {
        for (int i = 0; i < kDefaultSpringPrefixCount; ++i)
            pfx.emplace_back(kDefaultSpringPrefixes[i]);
    } else {
        for (const std::string& p : prefixes) {
            if (!p.empty()) pfx.push_back(p);
        }
    }
    if (pfx.empty()) return rig;

    // ---- 1) 哪些骨匹配前缀 ----
    // 走 `Skeleton::findJointsWithPrefix`：不区分大小写的"以…开头"，
    // 与 LookAt 的按名查找是同一份实现（第八刀收敛）。
    std::vector<char> match(static_cast<size_t>(n), 0);
    {
        std::vector<int> hits;
        for (const std::string& p : pfx) {
            hits.clear();
            skel.findJointsWithPrefix(p, hits);
            for (int j : hits)
                if (j >= 0 && j < n) match[static_cast<size_t>(j)] = 1;
        }
    }

    // ---- 2) 静止姿势的一次 FK ----
    // restLen / restDir 都必须在**静止姿势**下取：拿运行时姿势算的话，
    // "角色弯着腰的时候存了链长"，站起来头发就被拉长。
    //
    // ⚠ 走 setRest + poseToWorld，而不是直接 `base * jt.restLocal` ——
    //   后者与运行时那条路径（restLocal 先 decompose 再 compose）在浮点上
    //   不完全等价，restLen 会差 1e-7 量级。差这么点本身无所谓，但它会让
    //   "静止时质点应该恰好落在骨头上"这类断言永远差一点点，白花排查时间。
    std::vector<glm::mat4> restWorld;
    {
        Pose restPose;
        restPose.setRest(skel);
        poseToWorld(skel, restPose, restWorld);
    }

    // ---- 3) 收集"链内"骨 ----
    // 判据：自己匹配 **且** 父骨也匹配。父骨不匹配的那个（通常是 head）
    // 是 attach —— 它受动画驱动，物理不碰（见头文件 ②）。
    for (int j : skel.topoOrder) {
        if (j < 0 || j >= n || !match[static_cast<size_t>(j)]) continue;
        const int p = skel.joints[static_cast<size_t>(j)].parent;
        if (p < 0 || p >= n || !match[static_cast<size_t>(p)]) continue;

        const glm::vec3 d = glm::vec3(restWorld[static_cast<size_t>(j)][3]) -
                            glm::vec3(restWorld[static_cast<size_t>(p)][3]);
        const float len = glm::length(d);
        // 父子重合（DCC 里常见于"零长度辅助骨"）：跳过，否则约束里除以 0
        if (len < 1.0e-5f) continue;

        rig.simulated.push_back(j);
        rig.restLen.push_back(len);
        // 静止方向存**父骨空间**：世界方向每帧都被动画改，父骨空间里才是常量
        const glm::quat parentRot =
            glm::quat_cast(glm::mat3(restWorld[static_cast<size_t>(p)]));
        rig.restDir.push_back(glm::inverse(parentRot) * (d / len));
    }
    return rig;
}

int SpringBoneState::update(const assets::Skeleton& skel, const SpringRig& rig,
                            float dt, const SpringBoneSettings& s, Pose& pose) {
    const int n = skel.jointCount();
    if (!s.enabled || n <= 0 || rig.empty()) {
        m_init = false;
        return 0;
    }
    if (pose.count() < n) {
        m_init = false;
        return 0;
    }
    const int nc = rig.count();

    // ---- 1) 当前 pose 的世界矩阵 ----
    // poseToJoints 紧接着会再算一遍。这一次重复是为了拿到"物理前"的世界
    // 位置与旋转；整具骨架 O(n) 次矩阵乘（200 骨约几微秒）。
    // 换方案的话要改 poseToJoints 的签名 —— 那函数有一条"与
    // Skeleton::sample 输出 memcmp 级相同"的契约，不值得为省这点开销去动。
    std::vector<glm::mat4> world;
    poseToWorld(skel, pose, world);

    // ---- 2) 起步 / 重置 ----
    if (!m_init || static_cast<int>(m_pos.size()) != nc) {
        m_pos.assign(static_cast<size_t>(nc), glm::vec3(0.0f));
        m_prev.assign(static_cast<size_t>(nc), glm::vec3(0.0f));
        for (int c = 0; c < nc; ++c) {
            const glm::vec3 p =
                glm::vec3(world[static_cast<size_t>(rig.simulated[c])][3]);
            m_pos[c] = p;
            m_prev[c] = p;
        }
        m_init = true;
        // ⚠ 第一帧不改姿态。这样"挂上组件的瞬间"与"没挂"逐像素相同，
        //   回归基线不会因为多跑一帧物理而飘。
        return 0;
    }

    if (!(dt > 0.0f)) return 0;  // 暂停（编辑器停表）：姿态原样，物理不动
    dt = std::min(dt, std::max(s.maxStep, 1.0e-4f));

    std::vector<int> slot(static_cast<size_t>(n), -1);
    for (int c = 0; c < nc; ++c) slot[static_cast<size_t>(rig.simulated[c])] = c;

    // ---- 3) 积分 + 距离约束 ----
    const glm::vec3 accel = s.gravity + s.wind;
    const float dt2 = dt * dt;
    const int iters = std::max(1, s.iterations);
    // 回弹用**指数插值**而不是"加速度"：加速度形式在 restStiffness 偏大时
    // 会自激震荡（头发抽筋），指数插值天然有界、且与帧率无关。
    const float pull = 1.0f - std::exp(-s.restStiffness * 20.0f * dt);

    for (int it = 0; it < iters; ++it) {
        for (int c = 0; c < nc; ++c) {
            const int j = rig.simulated[static_cast<size_t>(c)];
            const int p = skel.joints[static_cast<size_t>(j)].parent;
            const size_t pc = static_cast<size_t>(p);
            // 父骨位置：链内的读它的质点（本轮已更新），attach 读 FK 结果
            const int ci = slot[pc];
            const glm::vec3 parentPos =
                ci >= 0 ? m_pos[static_cast<size_t>(ci)] : glm::vec3(world[pc][3]);

            if (it == 0) {
                const glm::vec3 vel =
                    (m_pos[static_cast<size_t>(c)] - m_prev[static_cast<size_t>(c)]) *
                    s.damping;
                m_prev[static_cast<size_t>(c)] = m_pos[static_cast<size_t>(c)];
                m_pos[static_cast<size_t>(c)] += vel + accel * dt2;

                // 回弹到"造型"：目标点 = 父骨位置 + 静止方向 × 链长。
                // ⚠ 静止方向用**物理前**的父骨旋转转回世界 —— 差别只有
                //   一帧的转动量，肉眼不可见；换来的是不必在积分阶段解一遍
                //   父骨的旋转（那会变成迭代里套迭代）。
                if (pull > 0.0f) {
                    const glm::quat parentRot = glm::quat_cast(glm::mat3(world[pc]));
                    const glm::vec3 want =
                        parentPos + (parentRot * rig.restDir[static_cast<size_t>(c)]) *
                                        rig.restLen[static_cast<size_t>(c)];
                    m_pos[static_cast<size_t>(c)] +=
                        (want - m_pos[static_cast<size_t>(c)]) * pull;
                }
            }

            // 距离约束（完全刚性：头发被拉长很怪，不需要可拉伸）
            glm::vec3 d = m_pos[static_cast<size_t>(c)] - parentPos;
            const float len = glm::length(d);
            if (len > 1.0e-6f) {
                m_pos[static_cast<size_t>(c)] =
                    parentPos + d * (rig.restLen[static_cast<size_t>(c)] / len);
            }
        }
    }

    // ---- 4) 从世界位置反解局部旋转 ----
    // 按 topoOrder（rig.simulated 保证）走：父骨的世界旋转先定，
    // 子骨才能用 `inverse(父) * 世界` 反解出自己的局部旋转。
    std::vector<glm::quat> newRot(static_cast<size_t>(n));
    for (int j = 0; j < n; ++j)
        newRot[static_cast<size_t>(j)] =
            glm::quat_cast(glm::mat3(world[static_cast<size_t>(j)]));

    for (int c = 0; c < nc; ++c) {
        const int j = rig.simulated[static_cast<size_t>(c)];
        const int p = skel.joints[static_cast<size_t>(j)].parent;
        if (p < 0) continue;
        const size_t pj = static_cast<size_t>(j);
        const size_t pp = static_cast<size_t>(p);
        const int ci = slot[pp];

        const glm::vec3 oldPosJ = glm::vec3(world[pj][3]);
        const glm::vec3 oldPosP = glm::vec3(world[pp][3]);
        const glm::vec3 newPosP = ci >= 0 ? m_pos[static_cast<size_t>(ci)] : oldPosP;

        const glm::vec3 oldDir = oldPosJ - oldPosP;
        const glm::vec3 newDir = m_pos[static_cast<size_t>(c)] - newPosP;
        const float lo = glm::length(oldDir);
        const float ln = glm::length(newDir);
        if (lo < 1.0e-6f || ln < 1.0e-6f) continue;

        // 只纠正"指向"：shortestArc 不含绕骨轴的自转，所以骨骼原本的
        // roll（沿轴扭转）被完整保留 —— 换用 lookAt 式的完整旋转会把它抹掉。
        const glm::quat delta =
            shortestArc(oldDir / lo, newDir / ln);
        const glm::quat newWorld = delta * newRot[pj];
        newRot[pj] = newWorld;
        pose.rotation[pj] = glm::normalize(glm::inverse(newRot[pp]) * newWorld);
    }

    return nc;
}

} // namespace animation
