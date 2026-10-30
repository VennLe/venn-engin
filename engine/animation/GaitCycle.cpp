// ============================================================
// animation/GaitCycle.cpp —— 参数化行走循环（实现）
//
// 头文件里有设计说明与"表 → 相位"的换算约定，这里只写实现要点：
//
//   ① 所有旋转都用**世界轴 + 角度**表达，再换算回关节局部旋转：
//        local = inv(parentWorldRot) · R(worldAxis, θ) · restWorldRot
//      这样"绕侧向轴摆 22°"在任何骨架上都是同一件事，不需要知道
//      这些骨的局部轴被导出器掰成什么样（Mixamo 的局部轴是歪的）。
//   ② 腿不走 FK：先算**踝的世界目标点**，再 `solveTwoBoneIK` 回填。
//      "脚不滑步"这条规则因此变成几何约束，而不是"K 得准不准"。
//   ③ 逐帧烘焙成 `EditClip` 的键，求值仍走 `overlayEditPose`（唯一入口）。
// ============================================================

#include "animation/GaitCycle.h"

#include "animation/Pose.h"
#include "animation/TwoBoneIK.h"

#include "core/Logger.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace animation {

namespace {

// ============================================================
// 小工具
// ============================================================

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg = kPi / 180.0f;

float frac(float x) { return x - std::floor(x); }

glm::vec3 col3(const glm::mat4& m, int i) {
    return glm::vec3(m[static_cast<glm::length_t>(i)]);
}
glm::vec3 originOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// 世界矩阵 → 旋转。剥掉缩放（骨架带缩放时 quat_cast 会给一段乱转）。
glm::quat rotOf(const glm::mat4& m) {
    const glm::vec3 c0 = glm::normalize(col3(m, 0));
    const glm::vec3 c1 = glm::normalize(col3(m, 1));
    const glm::vec3 c2 = glm::normalize(col3(m, 2));
    return glm::normalize(glm::quat_cast(glm::mat3(c0, c1, c2)));
}

// v 去掉沿 axis 的分量
glm::vec3 reject(const glm::vec3& v, const glm::vec3& axis) {
    return v - axis * glm::dot(v, axis);
}

bool finite3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// 骨名规范化：取最后一个 ':' / '|' 之后 → 小写 → 去掉分隔符。
//    `mixamorig:LeftUpLeg` → leftupleg
//    `thigh.L`             → thighl
//    `Bip01_L_Thigh`       → bip01lthigh
std::string normalizeBone(const std::string& n) {
    std::size_t cut = 0;
    for (std::size_t i = 0; i < n.size(); ++i)
        if (n[i] == ':' || n[i] == '|') cut = i + 1;
    std::string out;
    out.reserve(n.size() - cut);
    for (std::size_t i = cut; i < n.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(n[i]);
        if (c == ' ' || c == '_' || c == '.' || c == '-') continue;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

// 角色 → 候选名（规范形）。顺序 = 优先级。
// ⚠ 这张表是"换角色要改的唯一一处"。找不到就走结构回退（见 resolve…）。
const char* const* roleCandidates(GaitRole r, int& count) {
    static const char* kHips[] = {"hips", "pelvis", "hip", "bip01pelvis"};
    static const char* kSpine[] = {"spine", "spine1", "abdomen", "bip01spine"};
    static const char* kChest[] = {"spine2", "chest", "upperchest", "spine3",
                                   "bip01spine3", "spine04"};
    static const char* kNeck[] = {"neck", "bip01neck"};
    static const char* kHead[] = {"head", "bip01head"};
    static const char* kLUpperLeg[] = {"leftupleg", "leftupperleg", "leftthigh",
                                       "thighl", "upperlegl", "bip01lthigh"};
    static const char* kLLowerLeg[] = {"leftleg", "leftlowerleg", "leftshin",
                                       "leftcalf", "shinl", "lowerlegl",
                                       "calfl", "bip01lcalf"};
    static const char* kLFoot[] = {"leftfoot", "footl", "bip01lfoot"};
    static const char* kLToe[] = {"lefttoebase", "lefttoe", "toel",
                                  "toebasel", "ltoe"};
    static const char* kRUpperLeg[] = {"rightupleg", "rightupperleg",
                                       "rightthigh", "thighr", "upperlegr",
                                       "bip01rthigh"};
    static const char* kRLowerLeg[] = {"rightleg", "rightlowerleg", "rightshin",
                                       "rightcalf", "shinr", "lowerlegr",
                                       "calfr", "bip01rcalf"};
    static const char* kRFoot[] = {"rightfoot", "footr", "bip01rfoot"};
    static const char* kRToe[] = {"righttoebase", "righttoe", "toer",
                                  "toebaser", "rtoe"};
    static const char* kLUpperArm[] = {"leftarm", "leftupperarm", "upperarml",
                                       "bip01lupperarm"};
    static const char* kLForeArm[] = {"leftforearm", "leftlowerarm",
                                      "forearml", "lowerarml",
                                      "bip01lforearm"};
    static const char* kRUpperArm[] = {"rightarm", "rightupperarm", "upperarmr",
                                       "bip01rupperarm"};
    static const char* kRForeArm[] = {"rightforearm", "rightlowerarm",
                                      "forearmr", "lowerarmr",
                                      "bip01rforearm"};
    static const char* kLHand[] = {"lefthand", "handl", "bip01lhand"};
    static const char* kRHand[] = {"righthand", "handr", "bip01rhand"};

#define VENN_ROLE_TABLE(arr)                                                   \
    do {                                                                       \
        count = static_cast<int>(sizeof(arr) / sizeof(arr[0]));                 \
        return arr;                                                            \
    } while (0)

    switch (r) {
    case GaitRole::Hips: VENN_ROLE_TABLE(kHips);
    case GaitRole::Spine: VENN_ROLE_TABLE(kSpine);
    case GaitRole::Chest: VENN_ROLE_TABLE(kChest);
    case GaitRole::Neck: VENN_ROLE_TABLE(kNeck);
    case GaitRole::Head: VENN_ROLE_TABLE(kHead);
    case GaitRole::LeftUpperLeg: VENN_ROLE_TABLE(kLUpperLeg);
    case GaitRole::LeftLowerLeg: VENN_ROLE_TABLE(kLLowerLeg);
    case GaitRole::LeftFoot: VENN_ROLE_TABLE(kLFoot);
    case GaitRole::LeftToe: VENN_ROLE_TABLE(kLToe);
    case GaitRole::RightUpperLeg: VENN_ROLE_TABLE(kRUpperLeg);
    case GaitRole::RightLowerLeg: VENN_ROLE_TABLE(kRLowerLeg);
    case GaitRole::RightFoot: VENN_ROLE_TABLE(kRFoot);
    case GaitRole::RightToe: VENN_ROLE_TABLE(kRToe);
    case GaitRole::LeftUpperArm: VENN_ROLE_TABLE(kLUpperArm);
    case GaitRole::LeftForeArm: VENN_ROLE_TABLE(kLForeArm);
    case GaitRole::RightUpperArm: VENN_ROLE_TABLE(kRUpperArm);
    case GaitRole::RightForeArm: VENN_ROLE_TABLE(kRForeArm);
    case GaitRole::LeftHand: VENN_ROLE_TABLE(kLHand);
    case GaitRole::RightHand: VENN_ROLE_TABLE(kRHand);
    default: break;
    }
#undef VENN_ROLE_TABLE
    count = 0;
    return nullptr;
}

// ============================================================
// 世界矩阵的"读 / 写"（写=把想要的世界姿势反解回局部）
// ============================================================

glm::mat4 parentWorldOf(const assets::Skeleton& skel, int j,
                        const std::vector<glm::mat4>& world) {
    const int p = skel.joints[static_cast<std::size_t>(j)].parent;
    if (p >= 0 && p < static_cast<int>(world.size()))
        return world[static_cast<std::size_t>(p)];
    return skel.joints[static_cast<std::size_t>(j)].externalChain;
}

// 让关节 j 的世界旋转变成 desired（局部旋转反解出来写回 pose）。
void setWorldRot(const assets::Skeleton& skel, int j,
                 const std::vector<glm::mat4>& world, const glm::quat& desired,
                 Pose& pose) {
    if (j < 0) return;
    const glm::quat parentRot = rotOf(parentWorldOf(skel, j, world));
    pose.rotation[static_cast<std::size_t>(j)] =
        glm::normalize(glm::inverse(parentRot) * desired);
}

// 让关节 j 的世界原点变成 desired。
// ⚠ 只对**没有缩放**的骨架成立（本仓库所有资产都是 1），带缩放时
//   要把位移除 S —— 那份代码现在没人需要，不写。
void setWorldPos(const assets::Skeleton& skel, int j,
                 const std::vector<glm::mat4>& world, const glm::vec3& desired,
                 Pose& pose) {
    if (j < 0) return;
    const glm::mat4 base = parentWorldOf(skel, j, world);
    const glm::vec3 local(glm::inverse(base) * glm::vec4(desired, 1.0f));
    pose.translation[static_cast<std::size_t>(j)] = local;
}

// 绕**世界轴**转 θ（θ 用弧度）。与 setWorldRot 的区别：这个是"在静止
// 姿势上叠加一个世界轴旋转"，写起来短、也不依赖 restWorldRot。
void rotAboutWorldAxis(const assets::Skeleton& skel, int j,
                       const std::vector<glm::mat4>& world,
                       const glm::vec3& axis, float angle, Pose& pose) {
    if (j < 0 || std::fabs(angle) < 1.0e-9f) return;
    const glm::quat parentRot = rotOf(parentWorldOf(skel, j, world));
    const glm::vec3 axisLocal = glm::inverse(parentRot) * axis;
    const float len = glm::length(axisLocal);
    if (len < 1.0e-6f) return;
    const glm::quat q =
        glm::angleAxis(angle, axisLocal / len);
    pose.rotation[static_cast<std::size_t>(j)] =
        glm::normalize(q * pose.rotation[static_cast<std::size_t>(j)]);
}

// 把 j → child 这根骨的**世界指向**转到 desired（最小旋转）。
//
// 为什么需要它：`rotAboutWorldAxis` 只能绕一个给定轴转，"把折起来的肘
// 伸直"这件事没有固定轴 —— 轴取决于当前指向。导入姿势里手臂常常是折的
// （`anime_girl.fbx` 上肩到腕只有 24 cm，正常是 50 cm），不先伸直的话
// "绕侧向轴摆臂"只是让一只折着的手臂微微晃一下（实测前后差 4.5 cm），
// 而正确的结果是 38 cm。⚠ 调用方负责跟一次 poseToWorld。
void alignBoneDir(const assets::Skeleton& skel, int j, int child,
                  std::vector<glm::mat4>& world, const glm::vec3& desired,
                  Pose& pose) {
    if (j < 0 || child < 0 || desired == glm::vec3(0.0f)) return;
    const glm::vec3 cur = originOf(world[static_cast<std::size_t>(child)]) -
                          originOf(world[static_cast<std::size_t>(j)]);
    if (glm::length(cur) < 1.0e-6f) return;
    const glm::vec3 a = glm::normalize(cur);
    const glm::vec3 b = glm::normalize(desired);
    const float c = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
    if (c > 0.99999f) return;             // 已经对齐（夹具就是这条路）
    const glm::vec3 axis = glm::cross(a, b);
    if (glm::length(axis) < 1.0e-6f) return;   // 恰好反向：没有唯一轴
    rotAboutWorldAxis(skel, j, world, glm::normalize(axis), std::acos(c), pose);
}

// ============================================================
// 骨架的静止参考（只算一次）
// ============================================================

struct RestRef {
    Pose pose;
    std::vector<glm::mat4> world;

    bool build(const assets::Skeleton& skel) {
        if (skel.jointCount() <= 0) return false;
        pose.setRest(skel);
        poseToWorld(skel, pose, world);
        return true;
    }
    glm::vec3 pos(int j) const {
        if (j < 0 || j >= static_cast<int>(world.size())) return glm::vec3(0.0f);
        return originOf(world[static_cast<std::size_t>(j)]);
    }
};

// 最长子骨（结构回退用）。长度用静止姿势下的世界距离量，"最长"在腿上是
// 小腿、在臂上是前臂 —— 与 TwoBoneIK 的默认解析同一个判据。
int longestChild(const assets::Skeleton& skel, int parent,
                 const RestRef& rest) {
    if (parent < 0) return -1;
    int best = -1;
    float bestLen = 0.0f;
    for (int j = 0; j < skel.jointCount(); ++j) {
        if (skel.joints[static_cast<std::size_t>(j)].parent != parent) continue;
        const float len = glm::length(rest.pos(j) - rest.pos(parent));
        if (len > bestLen) {
            bestLen = len;
            best = j;
        }
    }
    return best;
}

// ============================================================
// 步态几何（一次算完，逐帧复用）
// ============================================================

struct GaitGeom {
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    glm::vec3 left{1.0f, 0.0f, 0.0f};
    glm::vec3 fwd{0.0f, 1.0f, 0.0f};

    glm::vec3 hipsPos{0.0f};         // 静止时骨盆原点
    glm::vec3 ankle[2];              // 静止时踝（0=L 1=R）
    float ankleLat[2] = {0.0f, 0.0f};  // 踝相对髋的侧向偏移
    float ankleUp[2] = {0.0f, 0.0f};   // 踝相对髋的竖直偏移（负）

    float legLen = 0.8f;   // 大腿 + 小腿
    float hipHeight = 0.8f; // 髋关节到踝平面的高度（静止）
    float A = 0.3f;        // 承重期前后各走多远（"半步幅"）
    float hBase = 0.0f;    // 生成时用的骨盆高度（相对踝平面）
    float drop = 0.0f;     // 相对静止姿势下蹲了多少
    float bobEff = 0.0f;   // 夹过的上下振幅
    float T = 2.0f;        // 周期（秒）
    // 把手臂从**导入姿势**放到自然下垂所需的、绕 fwd 的带符号角度
    // （0 = 本来就垂着）。见 buildGeom 里的"从静止姿势量"。
    float armDown[2] = {0.0f, 0.0f};
};

// ---- 三轴推导 ----
//
// 抽成独立函数（2026-10-30，A4 步幅匹配）：`measureHumanoidAxes` 要把
// 同一套三轴交给"从 clip 里量自然步速"那一条路。两处各写一份的下场是
// "生成时的 fwd"与"量步速时的 fwd"不是同一个方向 —— 实测步速会变成负数，
// 然后被当成"这不是步态 clip"，而在这两个函数各自看都对。
bool deriveAxes(const RestRef& rest, const HumanoidRig& rig,
                const glm::vec3& hipsPos, const glm::vec3& ankleL,
                const glm::vec3& ankleR, glm::vec3& upOut, glm::vec3& leftOut,
                glm::vec3& fwdOut, std::string& err) {
    const int ch = rig.joint[static_cast<int>(GaitRole::Chest)];
    const int hd = rig.joint[static_cast<int>(GaitRole::Head)];
    const int toL = rig.joint[static_cast<int>(GaitRole::LeftToe)];
    const int toR = rig.joint[static_cast<int>(GaitRole::RightToe)];

    // ---- up：髋 → 胸（没有胸就用头），再退化到"两踝中点 → 髋" ----
    {
        const glm::vec3 hipToChest =
            (ch >= 0) ? (rest.pos(ch) - hipsPos) : glm::vec3(0.0f);
        const glm::vec3 hipToHead =
            (hd >= 0) ? (rest.pos(hd) - hipsPos) : glm::vec3(0.0f);
        glm::vec3 up = hipToChest;
        if (glm::length(up) < 1.0e-4f) up = hipToHead;
        if (glm::length(up) < 1.0e-4f)
            up = hipsPos - (ankleL + ankleR) * 0.5f;
        if (glm::length(up) < 1.0e-4f) {
            err = "量不出 up 轴（髋 / 胸 / 踝的世界位置重合？）";
            return false;
        }
        upOut = glm::normalize(up);
    }

    // ---- left：两个踝的连线（用**真实的左右脚**定侧向）----
    {
        glm::vec3 left = reject(ankleL - ankleR, upOut);
        if (glm::length(left) < 1.0e-4f) {
            err = "量不出 left 轴（左右踝的侧向距离为 0）";
            return false;
        }
        leftOut = glm::normalize(left);
        fwdOut = glm::normalize(glm::cross(leftOut, upOut));
    }

    // ---- 用"脚尖朝前"校验 fwd 的符号（翻就两个一起翻）----
    {
        const glm::vec3 toeDir =
            (toL >= 0 || toR >= 0)
                ? reject((toL >= 0 ? rest.pos(toL) : rest.pos(toR)) -
                             (toL >= 0 ? ankleL : ankleR),
                         upOut)
                : glm::vec3(0.0f);
        if (glm::length(toeDir) > 1.0e-4f &&
            glm::dot(glm::normalize(toeDir), fwdOut) < 0.0f) {
            fwdOut = -fwdOut;
            leftOut = -leftOut;
        }
    }
    return true;
}

bool buildGeom(const assets::Skeleton& skel, const HumanoidRig& rig,
               const GaitParams& p, const RestRef& rest, GaitGeom& g,
               std::string& err) {
    const int hips = rig.joint[static_cast<int>(GaitRole::Hips)];
    const int ulL = rig.joint[static_cast<int>(GaitRole::LeftUpperLeg)];
    const int llL = rig.joint[static_cast<int>(GaitRole::LeftLowerLeg)];
    const int ftL = rig.joint[static_cast<int>(GaitRole::LeftFoot)];
    const int ulR = rig.joint[static_cast<int>(GaitRole::RightUpperLeg)];
    const int llR = rig.joint[static_cast<int>(GaitRole::RightLowerLeg)];
    const int ftR = rig.joint[static_cast<int>(GaitRole::RightFoot)];

    if (hips < 0 || ulL < 0 || llL < 0 || ftL < 0 || ulR < 0 || llR < 0 ||
        ftR < 0) {
        err = "骨架缺少髋 / 大腿 / 小腿 / 踝，无法解两骨 IK";
        return false;
    }

    g.hipsPos = rest.pos(hips);
    g.ankle[0] = rest.pos(ftL);
    g.ankle[1] = rest.pos(ftR);

    // 三轴（up / left / fwd）走共享实现 —— 见 deriveAxes 的注释。
    // ⚠ 胸 / 头 / 脚尖只在这一个函数里用到，所以它们的下标也搬到那边取，
    //   这里不再声明（留着就是四个"声明了没人用"的变量，而它们的存在
    //   会让人以为本函数还自己算轴）。
    if (!deriveAxes(rest, rig, g.hipsPos, g.ankle[0], g.ankle[1], g.up, g.left,
                    g.fwd, err))
        return false;

    for (int s = 0; s < 2; ++s) {
        const glm::vec3 d = g.ankle[s] - g.hipsPos;
        g.ankleLat[s] = glm::dot(d, g.left);
        g.ankleUp[s] = glm::dot(d, g.up);
    }

    // ---- 腿长 / 髋高 ----
    const float tc = glm::length(rest.pos(llL) - rest.pos(ulL));
    const float sc = glm::length(rest.pos(ftL) - rest.pos(llL));
    if (tc < 1.0e-4f || sc < 1.0e-4f) {
        err = "大腿或小腿长度为 0";
        return false;
    }
    g.legLen = tc + sc;
    g.hipHeight = glm::dot(rest.pos(ulL) - g.ankle[0], g.up);
    if (g.hipHeight <= 1.0e-3f) {
        err = "髋关节不在踝上方（腿的反向？）";
        return false;
    }

    g.T = static_cast<float>(std::max(4, p.cycleFrames)) / std::max(1.0f, p.fps);

    // ---- 前伸量：先按参数要，再按腿长夹 ----
    // 腿几乎伸直的静止姿势够不到大步幅：髋-踝距离 = sqrt(A² + h²) 不能超过
    // 腿长。够了就会"腿拉直、脚离地"（看起来就是滑步）。所以这里把 A 夹到
    // 可达范围内，并把"夹过"这个事实报出去 —— 静默夹才是坑。
    const float Lmax = 0.995f * g.legLen;
    const float stance = glm::clamp(p.stanceFrac, 0.35f, 0.85f);
    const float hFloor =
        std::max(0.55f * g.legLen, g.hipHeight - std::max(0.0f, p.maxCrouch));
    const float aCap =
        std::sqrt(std::max(0.0f, Lmax * Lmax - hFloor * hFloor));
    g.A = std::min(p.stepLength * stance, aCap);
    // 保证在 contact 处够得着（hBase ≤ sqrt(Lmax² − A²)）
    const float hNeed = std::sqrt(std::max(0.0f, Lmax * Lmax - g.A * g.A));
    g.hBase = glm::clamp(std::min(g.hipHeight, hNeed), hFloor, g.hipHeight);
    g.drop = std::max(0.0f, g.hipHeight - g.hBase);
    g.bobEff = std::max(0.0f, std::min(p.bob, Lmax - g.hBase));

    // ---- 手臂垂下量：**从静止姿势量**，不写死 ----
    //
    // T-pose 的资产两只手臂是横的。这时候"绕侧向轴摆臂"等于绕手臂**自己
    // 的轴**转 —— 手几乎原地打转（实测 `anime_girl.fbx` 上前后摆幅只有
    // 5.8 cm），看起来就是僵尸走。A-pose / 自然站姿的手臂本来就垂着，
    // 这个量接近 0，加上去也没副作用。
    //
    // 判据：上臂指向投影到**矢状面内**（去掉 fwd 分量）后，与"下方"的
    // 带符号夹角（绕 fwd）。符号天然左右相反，正好满足镜像要求。
    for (int s = 0; s < 2; ++s) {
        g.armDown[s] = 0.0f;
        const GaitRole ua =
            s == 0 ? GaitRole::LeftUpperArm : GaitRole::RightUpperArm;
        const GaitRole fa =
            s == 0 ? GaitRole::LeftForeArm : GaitRole::RightForeArm;
        if (!rig.has(ua) || !rig.has(fa)) continue;
        const glm::vec3 dir =
            reject(rest.pos(rig.joint[static_cast<int>(fa)]) -
                       rest.pos(rig.joint[static_cast<int>(ua)]),
                   g.fwd);
        // 手臂正好指着正前方时投影退化 —— 那是"量不出来"，不是 0。
        if (glm::length(dir) < 1.0e-4f) continue;
        const glm::vec3 d = glm::normalize(dir);
        const glm::vec3 down = -g.up;
        g.armDown[s] = std::atan2(glm::dot(glm::cross(d, down), g.fwd),
                                  glm::dot(d, down));
    }
    return true;
}

// 一帧的中间量（生成与测距都用同一套公式，避免"量的和生成的不是一回事"）
struct PhaseState {
    float phase = 0.0f;
    float pelvisUp = 0.0f;   // 相对 hBase
    float pelvisLeft = 0.0f;
    float hipYaw = 0.0f;     // 弧度
    float hipRoll = 0.0f;
    float chestYaw = 0.0f;
    float lean = 0.0f;
    float armSwing[2] = {0.0f, 0.0f};   // 0=L 1=R（弧度，正=后摆）
    float along[2] = {0.0f, 0.0f};      // 踝的前向偏移
    float lift[2] = {0.0f, 0.0f};       // 踝的竖直抬升
    float footPitch[2] = {0.0f, 0.0f};  // 正 = 脚尖向下
    float toeBend[2] = {0.0f, 0.0f};    // 正 = 相对脚再往下
    bool stance[2] = {true, true};
};

// 相位 → 所有标量。**这是整个步态的唯一"动作定义"**：
// 生成和测距都从这里取，所以"量的东西"与"生成的东西"不可能漂。
void evalPhase(const GaitParams& p, const GaitGeom& g, float phase,
               PhaseState& s) {
    s.phase = frac(phase);
    const float stance = glm::clamp(p.stanceFrac, 0.35f, 0.85f);

    // ---- 重心：两个波峰两个波谷（表 §五）----
    // 波谷在 1/8（Down）、波峰在 3/8（Up），周期 = 半个周期 → cos(4π φ)
    s.pelvisUp = -g.bobEff * std::cos(2.0f * kPi * 2.0f * (s.phase - 0.125f));
    s.pelvisLeft = p.sway * std::sin(2.0f * kPi * s.phase);

    // ---- 骨盆 / 胸 ----
    // 符号：绕 up 正向 = 左侧往后；contact（φ=0）左侧在前 → 取负。
    s.hipYaw = -p.hipYawDeg * kDeg * std::cos(2.0f * kPi * s.phase);
    s.chestYaw = p.chestYawDeg * kDeg * std::cos(2.0f * kPi * s.phase);
    // 承重侧髋略高；侧倾跟着重心走，稍微超前一点（先沉下去再抬起来）。
    s.hipRoll = p.hipRollDeg * kDeg * std::sin(2.0f * kPi * (s.phase - 0.03f));
    s.lean = p.torsoLeanDeg * kDeg;

    // ---- 手臂：与**同侧腿**反相，晚 armLagFrames 帧 ----
    const float lag = p.armLagFrames / std::max(1, p.cycleFrames);
    for (int side = 0; side < 2; ++side) {
        const float ph = frac(s.phase + (side == 1 ? 0.5f : 0.0f));
        // 正 = 后摆（绕 left 轴正向转，见头文件"坐标系"）
        s.armSwing[side] =
            p.armSwingDeg * kDeg * std::cos(2.0f * kPi * (ph - lag));
    }

    // ---- 腿：承重期"匀速后退"，摆动相"平滑前移 + 抛物线抬脚" ----
    const float A = g.A;
    for (int side = 0; side < 2; ++side) {
        const float ph = frac(s.phase + (side == 1 ? 0.5f : 0.0f));
        const bool inStance = (ph < stance);
        s.stance[side] = inStance;
        float u = 0.0f;
        if (inStance) {
            u = (stance > 1.0e-4f) ? (ph / stance) : 0.0f;
            // 线性：这是"脚掌贴地不动"（表 §6）在**身体坐标系**里的样子
            s.along[side] = A * (1.0f - 2.0f * u);
            // 后 25% 脚跟起来（表：帧19 Up L 的"脚跟离地"）
            const float heel = glm::clamp((u - 0.75f) / 0.25f, 0.0f, 1.0f);
            const float hs = heel * heel * (3.0f - 2.0f * heel);
            s.lift[side] = std::max(0.0f, p.toeRise) * hs;
            // 触地瞬间脚尖上抬（脚跟着地）→ 迅速踩平 → 蹬离时脚尖下压
            const float roll = glm::clamp(u / 0.15f, 0.0f, 1.0f);
            s.footPitch[side] = -p.heelStrikeDeg * kDeg * (1.0f - roll) +
                                p.toeOffDeg * kDeg * hs;
            s.toeBend[side] = -0.6f * p.toeOffDeg * kDeg * hs;
        } else {
            const float sw = std::max(1.0e-4f, 1.0f - stance);
            u = (ph - stance) / sw;
            const float us = u * u * (3.0f - 2.0f * u);
            s.along[side] = -A + 2.0f * A * us;
            s.lift[side] = p.footLift * std::sin(kPi * glm::clamp(u, 0.0f, 1.0f));
            s.footPitch[side] =
                glm::mix(p.toeOffDeg * kDeg, -p.heelStrikeDeg * kDeg, us);
            s.toeBend[side] = 0.0f;
        }
    }
}

// ============================================================
// 把一帧姿态摆出来
// ============================================================

struct PoseScratch {
    Pose pose;
    std::vector<glm::mat4> world;
};

void poseAtPhase(const assets::Skeleton& skel, const HumanoidRig& rig,
                 const GaitParams& p, const GaitGeom& g, const RestRef& rest,
                 const PhaseState& s, PoseScratch& sc) {
    Pose& pose = sc.pose;
    pose.setRest(skel);
    poseToWorld(skel, pose, sc.world);

    const int hips = rig.joint[static_cast<int>(GaitRole::Hips)];
    const int spine = rig.joint[static_cast<int>(GaitRole::Spine)];
    const int chest = rig.joint[static_cast<int>(GaitRole::Chest)];
    const int neck = rig.joint[static_cast<int>(GaitRole::Neck)];
    const int head = rig.joint[static_cast<int>(GaitRole::Head)];

    // ---- 骨盆：位移（上下 / 左右）+ 三个旋转 ----
    if (hips >= 0) {
        const glm::vec3 target =
            g.hipsPos + g.up * (s.pelvisUp - g.drop) + g.left * s.pelvisLeft;
        setWorldPos(skel, hips, sc.world, target, pose);
        const glm::quat rest = rotOf(sc.world[static_cast<std::size_t>(hips)]);
        const glm::quat q = glm::angleAxis(s.hipYaw, g.up) *
                            glm::angleAxis(s.hipRoll, g.fwd) * rest;
        setWorldRot(skel, hips, sc.world, q, pose);
        poseToWorld(skel, pose, sc.world);
    }

    // ---- 脊柱：前倾 + 反拧（腰椎 40% + 胸 60%，两段相加才是"胸的转度"）----
    if (spine >= 0) {
        rotAboutWorldAxis(skel, spine, sc.world, g.up, s.chestYaw * 0.4f, pose);
        rotAboutWorldAxis(skel, spine, sc.world, g.left, s.lean * 0.4f, pose);
        poseToWorld(skel, pose, sc.world);
    }
    if (chest >= 0) {
        rotAboutWorldAxis(skel, chest, sc.world, g.up, s.chestYaw * 0.6f, pose);
        rotAboutWorldAxis(skel, chest, sc.world, g.left, s.lean * 0.6f, pose);
        poseToWorld(skel, pose, sc.world);
    }
    // ---- 颈 / 头：抵掉躯干的前倾与拧转，保持"头基本水平" ----
    if (neck >= 0) {
        rotAboutWorldAxis(skel, neck, sc.world, g.up, -s.chestYaw * 0.5f, pose);
        rotAboutWorldAxis(skel, neck, sc.world, g.left, -s.lean * 0.5f, pose);
        poseToWorld(skel, pose, sc.world);
    }
    if (head >= 0) {
        rotAboutWorldAxis(skel, head, sc.world, g.up, -s.chestYaw * 0.5f, pose);
        rotAboutWorldAxis(skel, head, sc.world, g.left, -s.lean * 0.5f, pose);
        poseToWorld(skel, pose, sc.world);
    }

    // ---- 手臂：摆 + 肘 ----
    //
    // 三步，顺序不能换：
    //   ① **垂下**（T-pose 的资产手臂是横的，不先放下来就等于绕手臂
    //      自己的轴转）；
    //   ② **伸直**（导入姿势常把肘折起来 —— 折着摆臂的摆幅只有 4.5 cm，
    //      伸直之后才有 38 cm）；
    //   ③ 然后是摆臂 + 那一点点常驻弯曲。
    for (int side = 0; side < 2; ++side) {
        const int ua = rig.joint[static_cast<int>(
            side == 0 ? GaitRole::LeftUpperArm : GaitRole::RightUpperArm)];
        const int fa = rig.joint[static_cast<int>(
            side == 0 ? GaitRole::LeftForeArm : GaitRole::RightForeArm)];
        const int hn = rig.joint[static_cast<int>(
            side == 0 ? GaitRole::LeftHand : GaitRole::RightHand)];
        if (ua >= 0) {
            // 先把手臂放到自然下垂（T-pose 的资产必须走这一步，否则下面
            // 那个"摆臂"就变成绕手臂自己的轴转了）。
            if (std::fabs(g.armDown[side]) > 1.0e-4f) {
                rotAboutWorldAxis(skel, ua, sc.world, g.fwd, g.armDown[side],
                                  pose);
                poseToWorld(skel, pose, sc.world);
            }
            rotAboutWorldAxis(skel, ua, sc.world, g.left, s.armSwing[side],
                              pose);
            poseToWorld(skel, pose, sc.world);
        }
        // 前臂指向对齐到上臂指向 = 肘伸直（没有 hand 骨时跳过：拿不到
        // "前臂指向"，硬来会把前臂转到不知道哪里去）。
        if (fa >= 0 && ua >= 0 && hn >= 0) {
            const glm::vec3 upDir =
                originOf(sc.world[static_cast<std::size_t>(fa)]) -
                originOf(sc.world[static_cast<std::size_t>(ua)]);
            alignBoneDir(skel, fa, hn, sc.world, upDir, pose);
            poseToWorld(skel, pose, sc.world);
        }
        if (fa >= 0) {
            // 肘往里收（正 = 前臂往前）—— 与"手臂后摆为正"同一套符号
            rotAboutWorldAxis(skel, fa, sc.world, g.left, -p.elbowDeg * kDeg,
                              pose);
            poseToWorld(skel, pose, sc.world);
        }
    }

    // ---- 腿：两骨 IK ----
    for (int side = 0; side < 2; ++side) {
        const GaitRole ur = side == 0 ? GaitRole::LeftUpperLeg
                                      : GaitRole::RightUpperLeg;
        const GaitRole lr = side == 0 ? GaitRole::LeftLowerLeg
                                      : GaitRole::RightLowerLeg;
        const GaitRole fr = side == 0 ? GaitRole::LeftFoot : GaitRole::RightFoot;
        const int ru = rig.joint[static_cast<int>(ur)];
        const int rm = rig.joint[static_cast<int>(lr)];
        const int re = rig.joint[static_cast<int>(fr)];
        if (ru < 0 || rm < 0 || re < 0) continue;

        const glm::vec3 target = g.hipsPos + g.left * g.ankleLat[side] +
                                 g.fwd * s.along[side] +
                                 g.up * (g.ankleUp[side] + s.lift[side]);

        TwoBoneIKSettings ik;
        ik.enabled = true;
        ik.hasTarget = true;
        ik.target = target;
        ik.rootBone = skel.joints[static_cast<std::size_t>(ru)].name;
        ik.midBone = skel.joints[static_cast<std::size_t>(rm)].name;
        ik.endBone = skel.joints[static_cast<std::size_t>(re)].name;
        ik.weight = 1.0f;
        // 膝盖朝前：极向量给 fwd（它会先投影到垂直 root→target 的平面上）
        ik.poleMode = TwoBoneIKSettings::PoleFromVector;
        ik.poleVector = g.fwd;
        solveTwoBoneIK(skel, ik, pose);
    }
    poseToWorld(skel, pose, sc.world);

    // ---- 脚 + 脚尖：世界姿势直接给（脚掌要"贴地"，不是"跟着小腿转"）----
    for (int side = 0; side < 2; ++side) {
        const int ft = rig.joint[static_cast<int>(
            side == 0 ? GaitRole::LeftFoot : GaitRole::RightFoot)];
        const int toe = rig.joint[static_cast<int>(
            side == 0 ? GaitRole::LeftToe : GaitRole::RightToe)];
        if (ft >= 0) {
            // ⚠ 基准必须是**静止的世界旋转**（rest.world），不能用
            //   sc.world[ft]：IK 已经把大腿/小腿转过了，此刻 `world[ft]`
            //   里带着小腿的动画旋转，拿它当"脚掌平放"的基准 → 脚会跟着
            //   小腿一起转，触地那一帧脚尖扎进地里（而且很难看出是这原因）。
            const glm::quat restFoot =
                rotOf(rest.world[static_cast<std::size_t>(ft)]);
            const glm::quat desired =
                glm::angleAxis(s.footPitch[side], g.left) * restFoot;
            setWorldRot(skel, ft, sc.world, desired, pose);
            poseToWorld(skel, pose, sc.world);
        }
        if (toe >= 0) {
            rotAboutWorldAxis(skel, toe, sc.world, g.left, s.toeBend[side],
                              pose);
            poseToWorld(skel, pose, sc.world);
        }
    }
}

// 哪些关节要写轨道（有就写，没有就跳过）
void animatedRoles(const HumanoidRig& rig, std::vector<GaitRole>& out) {
    static const GaitRole kList[] = {
        GaitRole::Hips,        GaitRole::Spine,       GaitRole::Chest,
        GaitRole::Neck,        GaitRole::Head,
        GaitRole::LeftUpperArm, GaitRole::LeftForeArm,
        GaitRole::RightUpperArm, GaitRole::RightForeArm,
        GaitRole::LeftUpperLeg, GaitRole::LeftLowerLeg, GaitRole::LeftFoot,
        GaitRole::LeftToe,
        GaitRole::RightUpperLeg, GaitRole::RightLowerLeg, GaitRole::RightFoot,
        GaitRole::RightToe,
    };
    out.clear();
    for (GaitRole r : kList)
        if (rig.has(r)) out.push_back(r);
}

} // namespace

// ============================================================
// 角色名
// ============================================================

const char* gaitRoleName(GaitRole r) {
    switch (r) {
    case GaitRole::Hips: return "hips";
    case GaitRole::Spine: return "spine";
    case GaitRole::Chest: return "chest";
    case GaitRole::Neck: return "neck";
    case GaitRole::Head: return "head";
    case GaitRole::LeftUpperLeg: return "leftUpperLeg";
    case GaitRole::LeftLowerLeg: return "leftLowerLeg";
    case GaitRole::LeftFoot: return "leftFoot";
    case GaitRole::LeftToe: return "leftToe";
    case GaitRole::RightUpperLeg: return "rightUpperLeg";
    case GaitRole::RightLowerLeg: return "rightLowerLeg";
    case GaitRole::RightFoot: return "rightFoot";
    case GaitRole::RightToe: return "rightToe";
    case GaitRole::LeftUpperArm: return "leftUpperArm";
    case GaitRole::LeftForeArm: return "leftForeArm";
    case GaitRole::RightUpperArm: return "rightUpperArm";
    case GaitRole::RightForeArm: return "rightForeArm";
    case GaitRole::LeftHand: return "leftHand";
    case GaitRole::RightHand: return "rightHand";
    default: return "?";
    }
}

HumanoidRig::HumanoidRig() {
    for (int i = 0; i < kGaitRoleCount; ++i) joint[i] = -1;
}
bool HumanoidRig::has(GaitRole r) const {
    const int i = static_cast<int>(r);
    return i >= 0 && i < kGaitRoleCount && joint[i] >= 0;
}
bool HumanoidRig::walkable() const {
    return has(GaitRole::Hips) && has(GaitRole::LeftUpperLeg) &&
           has(GaitRole::LeftLowerLeg) && has(GaitRole::LeftFoot) &&
           has(GaitRole::RightUpperLeg) && has(GaitRole::RightLowerLeg) &&
           has(GaitRole::RightFoot);
}
std::string HumanoidRig::summary() const {
    std::string s = "HUMANOID";
    for (int i = 0; i < kGaitRoleCount; ++i) {
        if (joint[i] < 0) continue;
        s += ' ';
        s += gaitRoleName(static_cast<GaitRole>(i));
        s += '=';
        s += std::to_string(joint[i]);
    }
    if (!missing.empty()) {
        s += " missing=";
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i) s += ',';
            s += missing[i];
        }
    }
    return s;
}

HumanoidRig resolveHumanoidRig(const assets::Skeleton& skel) {
    HumanoidRig rig;
    if (skel.jointCount() <= 0) return rig;

    RestRef rest;
    if (!rest.build(skel)) return rig;

    // 规范名 → 关节（同名取第一个：`anime_girl` 那种重名的骨架是
    // 归一化后 [Idle,Walk,Idle,Walk] 的重名 clip，骨名也可能重）
    std::map<std::string, int> byName;
    for (int j = 0; j < skel.jointCount(); ++j) {
        const std::string n =
            normalizeBone(skel.joints[static_cast<std::size_t>(j)].name);
        if (n.empty()) continue;
        byName.emplace(n, j);   // emplace 不覆盖 → 第一个赢
    }

    auto byRole = [&](GaitRole r) -> int {
        int n = 0;
        const char* const* cand = roleCandidates(r, n);
        for (int i = 0; i < n; ++i) {
            const auto it = byName.find(cand[i]);
            if (it != byName.end()) return it->second;
        }
        return -1;
    };

    // ---- 腿：名字优先，**结构回退**（最长子骨）----
    // 结构回退是"任意双足骨架都能用"的关键：名字表再全也总有没见过的
    // 命名（`J_Bip_C_...`、`Bip01_...`、各种 DCC 的中文骨名）。
    struct LegSide {
        GaitRole upper, lower, foot, toe;
    };
    const LegSide legs[2] = {
        {GaitRole::LeftUpperLeg, GaitRole::LeftLowerLeg, GaitRole::LeftFoot,
         GaitRole::LeftToe},
        {GaitRole::RightUpperLeg, GaitRole::RightLowerLeg, GaitRole::RightFoot,
         GaitRole::RightToe},
    };
    for (int s = 0; s < 2; ++s) {
        int up = byRole(legs[s].upper);
        if (up < 0) continue;
        rig.joint[static_cast<int>(legs[s].upper)] = up;
        int lo = byRole(legs[s].lower);
        if (lo < 0) lo = longestChild(skel, up, rest);
        if (lo >= 0) {
            rig.joint[static_cast<int>(legs[s].lower)] = lo;
            int ft = byRole(legs[s].foot);
            if (ft < 0) ft = longestChild(skel, lo, rest);
            if (ft >= 0) {
                rig.joint[static_cast<int>(legs[s].foot)] = ft;
                int to = byRole(legs[s].toe);
                if (to < 0) to = longestChild(skel, ft, rest);
                if (to >= 0) rig.joint[static_cast<int>(legs[s].toe)] = to;
            }
        }
    }

    // ---- 髋：名字优先，否则是大腿的父 ----
    int hips = byRole(GaitRole::Hips);
    if (hips < 0) {
        const int ul = rig.joint[static_cast<int>(GaitRole::LeftUpperLeg)];
        if (ul >= 0) hips = skel.joints[static_cast<std::size_t>(ul)].parent;
    }
    rig.joint[static_cast<int>(GaitRole::Hips)] = hips;

    // ---- 上肢：只按名字（缺了就不摆臂，步态照样成立）----
    const GaitRole upper[] = {GaitRole::Spine, GaitRole::Chest, GaitRole::Neck,
                              GaitRole::Head, GaitRole::LeftUpperArm,
                              GaitRole::LeftForeArm, GaitRole::RightUpperArm,
                              GaitRole::RightForeArm, GaitRole::LeftHand,
                              GaitRole::RightHand};
    for (GaitRole r : upper) rig.joint[static_cast<int>(r)] = byRole(r);

    // 名字表里没有 neck/head 的骨架（不少游戏骨架把它们合并了）：
    // 颈 = 头的父，头 = 颈的最长子。两头都缺就都不动。
    if (!rig.has(GaitRole::Head) && rig.has(GaitRole::Neck))
        rig.joint[static_cast<int>(GaitRole::Head)] =
            longestChild(skel, rig.joint[static_cast<int>(GaitRole::Neck)],
                         rest);
    if (!rig.has(GaitRole::Neck) && rig.has(GaitRole::Head))
        rig.joint[static_cast<int>(GaitRole::Neck)] =
            skel.joints[static_cast<std::size_t>(
                rig.joint[static_cast<int>(GaitRole::Head)])]
                .parent;

    for (int i = 0; i < kGaitRoleCount; ++i) {
        if (rig.joint[i] >= 0) continue;
        // 腿和髋缺了要说，其余只是"少一段动作"
        const GaitRole r = static_cast<GaitRole>(i);
        rig.missing.emplace_back(gaitRoleName(r));
    }
    return rig;
}

// ---- 三轴：给"从 clip 里量步速"那一条路用（A4 步幅匹配）----
//
// 为什么必须导出而不是让 `StrideMatch.cpp` 自己算一遍：那三行
// （up = 髋→胸、left = 两踝连线水平化、fwd = cross + 脚尖 oracle）里
// 有两个"符号可以被翻"的地方。翻错的后果不是崩溃，而是**量出来的步速
// 变负数**，然后被当成"这段不是步态 clip"—— 两个函数各自看都不像有病。
HumanoidAxes measureHumanoidAxes(const assets::Skeleton& skel,
                                 const HumanoidRig& rig) {
    HumanoidAxes ax;
    if (skel.jointCount() <= 0) {
        ax.error = "空骨架";
        return ax;
    }
    const int hips = rig.joint[static_cast<int>(GaitRole::Hips)];
    const int ftL = rig.joint[static_cast<int>(GaitRole::LeftFoot)];
    const int ftR = rig.joint[static_cast<int>(GaitRole::RightFoot)];
    if (hips < 0 || ftL < 0 || ftR < 0) {
        ax.error = "骨架缺少髋 / 踝，量不出三轴";
        return ax;
    }
    RestRef rest;
    if (!rest.build(skel)) {
        ax.error = "静止姿势算不出世界矩阵";
        return ax;
    }
    ax.ok = deriveAxes(rest, rig, rest.pos(hips), rest.pos(ftL), rest.pos(ftR),
                       ax.up, ax.left, ax.fwd, ax.error);
    return ax;
}

const char* gaitKeyPoseName(float phase) {
    const float ph = frac(phase);
    const float eighth = ph * 8.0f;
    if (std::fabs(eighth - std::round(eighth)) > 1.0e-3f) return "";
    const int k = static_cast<int>(std::lround(eighth)) % 8;
    switch (k) {
    case 0: return "contact";
    case 2: return "down";
    case 4: return "passing";
    case 6: return "up";
    default: return "breakdown";
    }
}

// ============================================================
// 生成
// ============================================================

namespace {

// 把一帧写进 clip（时间单位秒）。四元数按"与上一个键同半球"存 ——
// 否则 slerp 会走远路（一段 20° 的摆动被插成 340° 的翻转）。
void writePoseKeys(EditClip& clip, const assets::Skeleton& skel,
                   const HumanoidRig& rig, const std::vector<GaitRole>& roles,
                   float time, const Pose& pose, KeyType type,
                   std::map<std::string, glm::quat>& lastRot) {
    for (GaitRole r : roles) {
        const int j = rig.joint[static_cast<int>(r)];
        if (j < 0) continue;
        const std::string& name = skel.joints[static_cast<std::size_t>(j)].name;
        glm::quat q = pose.rotation[static_cast<std::size_t>(j)];
        auto it = lastRot.find(name);
        if (it != lastRot.end() && glm::dot(it->second, q) < 0.0f) q = -q;
        lastRot[name] = q;
        clip.setKey(name, assets::AnimTrack::Rotation, time,
                    glm::vec4(q.x, q.y, q.z, q.w), type);
        if (r == GaitRole::Hips) {
            const glm::vec3 t = pose.translation[static_cast<std::size_t>(j)];
            clip.setKey(name, assets::AnimTrack::Translation, time,
                        glm::vec4(t, 0.0f), type);
        }
    }
}

} // namespace

GaitReport buildWalkCycle(const assets::Skeleton& skel, const GaitParams& p,
                          EditClip& out) {
    GaitReport rep;
    rep.rig = resolveHumanoidRig(skel);
    if (!rep.rig.walkable()) {
        rep.error = "骨架不是双足（缺髋或某条腿），无法生成步态";
        out = EditClip{};
        return rep;
    }

    RestRef rest;
    if (!rest.build(skel)) {
        rep.error = "骨架为空";
        out = EditClip{};
        return rep;
    }

    GaitGeom g;
    std::string err;
    if (!buildGeom(skel, rep.rig, p, rest, g, err)) {
        rep.error = err;
        out = EditClip{};
        return rep;
    }

    const int frames = std::max(4, p.cycleFrames);
    const float fps = std::max(1.0f, p.fps);
    const float stance = glm::clamp(p.stanceFrac, 0.35f, 0.85f);

    out = EditClip{};
    out.name = p.clipName;
    out.baseClip.clear();       // 纯自定义：未打帧的骨保持静止姿势
    out.loop = true;
    out.duration = static_cast<float>(frames) / fps;

    std::vector<GaitRole> roles;
    animatedRoles(rep.rig, roles);

    PoseScratch sc;
    sc.pose.resize(skel.jointCount());
    std::map<std::string, glm::quat> lastRot;

    // ---- 时间表：烘焙 = 每帧一个键；blocking = 表里的关键姿态 ----
    std::vector<int> frameList;
    if (p.blocking) {
        // 1/8 的整数倍（表：1 / 7 / 13 / 19 / 25，后半个周期镜像）
        const int step = std::max(1, frames / 8);
        for (int f = 0; f < frames; f += step) frameList.push_back(f);
        frameList.push_back(frames);
    } else {
        for (int f = 0; f <= frames; ++f) frameList.push_back(f);
    }

    PhaseState s;
    for (int f : frameList) {
        // ⚠ 末帧用 **相位 0** 而不是 1：frac(1)==0，走同一条公式，
        //   于是"闭合帧与首帧逐位相同"是构造保证，不靠浮点巧合。
        const float phase = (f >= frames) ? 0.0f
                                          : static_cast<float>(f) /
                                                static_cast<float>(frames);
        evalPhase(p, g, phase, s);
        poseAtPhase(skel, rep.rig, p, g, rest, s, sc);

        // 关键姿态标 Extreme，1/16 的奇数倍标 Breakdown（表 §4 的
        // "中间帧补充"），其余 Inbetween。**类型不参与求值**（R8 的约定）。
        KeyType type = KeyType::Inbetween;
        const int eighth = std::max(1, frames / 8);
        const int sixteenth = std::max(1, frames / 16);
        if (f >= frames || f % eighth == 0) type = KeyType::Extreme;
        else if (f % sixteenth == 0) type = KeyType::Breakdown;

        writePoseKeys(out, skel, rep.rig, roles,
                      static_cast<float>(f) / fps, sc.pose, type, lastRot);
    }

    // ---- 生成侧的几何读数 ----
    rep.ok = true;
    rep.cycleSeconds = g.T;
    rep.effectiveStep = g.A / stance;
    rep.nominalSpeed = 2.0f * g.A / (stance * g.T);
    rep.strideLimited = (g.A + 1.0e-5f < p.stepLength * stance);
    rep.pelvisDrop = g.drop;
    rep.bobEffective = g.bobEff;
    rep.armDownDeg = std::max(std::fabs(g.armDown[0]), std::fabs(g.armDown[1])) /
                     kDeg;

    // ---- 实测：把**烘焙结果**重新采一遍（判据必须落在成品上）----
    GaitReport m = measureGaitClip(skel, out, p);
    rep.bobRange = m.bobRange;
    rep.swayRange = m.swayRange;
    rep.fwdDriftMm = m.fwdDriftMm;
    rep.footLiftPeak = m.footLiftPeak;
    rep.stanceSlideMm = m.stanceSlideMm;
    rep.stanceVerticalMm = m.stanceVerticalMm;
    rep.stanceLineErrMm = m.stanceLineErrMm;
    rep.stanceSpeed = m.stanceSpeed;
    rep.mirrorErrMm = m.mirrorErrMm;
    rep.restMirrorMm = m.restMirrorMm;
    rep.loopErrMm = m.loopErrMm;
    rep.legSwingM = m.legSwingM;
    rep.armCounterM = m.armCounterM;
    rep.reachMargin = m.reachMargin;
    rep.tracks = m.tracks;
    rep.keys = m.keys;
    rep.extremeKeys = m.extremeKeys;

    VK_LOG_INFO("GAIT-BUILD clip=%s frames=%d fps=%.0f sec=%.3f "
                "step=%.3f speed=%.3f limited=%d drop=%.3f bob=%.3f "
                "tracks=%d keys=%d extreme=%d",
                out.name.c_str(), frames, static_cast<double>(fps),
                static_cast<double>(rep.cycleSeconds),
                static_cast<double>(rep.effectiveStep),
                static_cast<double>(rep.nominalSpeed),
                rep.strideLimited ? 1 : 0,
                static_cast<double>(rep.pelvisDrop),
                static_cast<double>(rep.bobEffective), rep.tracks, rep.keys,
                rep.extremeKeys);
    VK_LOG_INFO("GAIT-CHECK slide=%.2fmm vert=%.2fmm line=%.2fmm "
                "mirror=%.2fmm rest=%.2fmm loop=%.2fmm bob=%.3f sway=%.3f "
                "fwd=%.3fmm lift=%.3f leg=%.3f arm=%.3f armdown=%.1fdeg "
                "reach=%.3f speed=%.3f",
                static_cast<double>(rep.stanceSlideMm),
                static_cast<double>(rep.stanceVerticalMm),
                static_cast<double>(rep.stanceLineErrMm),
                static_cast<double>(rep.mirrorErrMm),
                static_cast<double>(rep.restMirrorMm),
                static_cast<double>(rep.loopErrMm),
                static_cast<double>(rep.bobRange),
                static_cast<double>(rep.swayRange),
                static_cast<double>(rep.fwdDriftMm),
                static_cast<double>(rep.footLiftPeak),
                static_cast<double>(rep.legSwingM),
                static_cast<double>(rep.armCounterM),
                static_cast<double>(rep.armDownDeg),
                static_cast<double>(rep.reachMargin),
                static_cast<double>(rep.stanceSpeed));
    return rep;
}

// ============================================================
// 测距（只读 clip，走正式求值链）
// ============================================================

GaitReport measureGaitClip(const assets::Skeleton& skel, const EditClip& clip,
                           const GaitParams& p) {
    GaitReport rep;
    rep.rig = resolveHumanoidRig(skel);
    if (!rep.rig.walkable() || clip.tracks.empty()) {
        rep.error = clip.tracks.empty() ? "clip 里没有轨道" : "骨架不可走";
        return rep;
    }
    RestRef rest;
    if (!rest.build(skel)) return rep;

    GaitGeom g;
    std::string err;
    if (!buildGeom(skel, rep.rig, p, rest, g, err)) {
        rep.error = err;
        return rep;
    }

    // 求值走**唯一入口**：resolveEditTracks（骨名 → 下标 + 烘曲线）+
    // overlayEditPose（只写有轨道的通道）。自己不插值，才不会"生成时对、
    // 播出来不对"。
    std::vector<ResolvedTrack> resolved;
    resolveEditTracks(skel, clip, resolved);

    const int fps = std::max(1, static_cast<int>(std::lround(p.fps)));
    const int frames = std::max(4, static_cast<int>(std::lround(
                                       clip.duration * static_cast<float>(fps))));
    const float stance = glm::clamp(p.stanceFrac, 0.35f, 0.85f);

    const int hips = rep.rig.joint[static_cast<int>(GaitRole::Hips)];
    const int ft[2] = {
        rep.rig.joint[static_cast<int>(GaitRole::LeftFoot)],
        rep.rig.joint[static_cast<int>(GaitRole::RightFoot)]};

    Pose pose;
    std::vector<glm::mat4> world;
    pose.resize(skel.jointCount());

    auto sampleAt = [&](float t) {
        samplePose(skel, nullptr, 0.0f, pose);
        overlayEditPose(resolved, t, pose);
        poseToWorld(skel, pose, world);
    };

    // ---- 逐帧采 ----
    struct FrameData {
        float up = 0.0f, lat = 0.0f, fwd = 0.0f;
        float ankleUp[2] = {0.0f, 0.0f};
        float ankleLat[2] = {0.0f, 0.0f};
        float ankleAlong[2] = {0.0f, 0.0f};
        glm::vec3 pos[static_cast<int>(GaitRole::Count)];
        bool have[static_cast<int>(GaitRole::Count)]{};
    };
    std::vector<FrameData> fd(static_cast<std::size_t>(frames));
    bool finiteOk = true;

    for (int f = 0; f < frames; ++f) {
        sampleAt(static_cast<float>(f) / static_cast<float>(fps));
        FrameData& d = fd[static_cast<std::size_t>(f)];
        const glm::vec3 hp = originOf(world[static_cast<std::size_t>(hips)]);
        if (!finite3(hp)) finiteOk = false;
        d.up = glm::dot(hp - g.hipsPos, g.up);
        d.lat = glm::dot(hp - g.hipsPos, g.left);
        d.fwd = glm::dot(hp - g.hipsPos, g.fwd);
        for (int s = 0; s < 2; ++s) {
            if (ft[s] < 0) continue;
            const glm::vec3 a = originOf(world[static_cast<std::size_t>(ft[s])]);
            const glm::vec3 rel = a - g.hipsPos;
            d.ankleUp[s] = glm::dot(rel, g.up);
            d.ankleLat[s] = glm::dot(rel, g.left);
            d.ankleAlong[s] = glm::dot(rel, g.fwd);
            if (!finite3(a)) finiteOk = false;
        }
        for (int i = 0; i < kGaitRoleCount; ++i) {
            const int j = rep.rig.joint[i];
            if (j < 0) continue;
            d.pos[i] = originOf(world[static_cast<std::size_t>(j)]);
            d.have[i] = true;
        }
    }

    // ---- 上下 / 左右峰峰值 ----
    float upMin = 1e30f, upMax = -1e30f, latMin = 1e30f, latMax = -1e30f;
    float fwdMin = 1e30f, fwdMax = -1e30f;
    for (const FrameData& d : fd) {
        upMin = std::min(upMin, d.up);
        upMax = std::max(upMax, d.up);
        latMin = std::min(latMin, d.lat);
        latMax = std::max(latMax, d.lat);
        fwdMin = std::min(fwdMin, d.fwd);
        fwdMax = std::max(fwdMax, d.fwd);
    }
    rep.bobRange = upMax - upMin;
    rep.swayRange = latMax - latMin;
    // 原地走（treadmill）：髋在前向一动不动。这是那张 K 帧表唯一指定的
    // 根骨行为，而"每帧往前挪 2 cm"在单帧截图里完全看不出来。
    rep.fwdDriftMm = (fwdMax - fwdMin) * 1000.0f;

    // ---- 承重期的"不滑步"三个数 ----
    // 窗口取 [2%, 70%] 的承重期：开头一帧是触地瞬间（脚还在滚），
    // 末 30% 是提跟（那是**故意**抬的，不是滑）。
    float slideMm = 0.0f, vertMm = 0.0f, lineErrMm = 0.0f, liftPeak = 0.0f;
    float speedSum = 0.0f;
    int speedN = 0;
    for (int s = 0; s < 2; ++s) {
        std::vector<float> ts, alang, alat, aup;
        for (int f = 0; f < frames; ++f) {
            const float ph = frac(static_cast<float>(f) /
                                      static_cast<float>(frames) +
                                  (s == 1 ? 0.5f : 0.0f));
            if (ph >= 0.02f * stance && ph <= 0.70f * stance) {
                ts.push_back(static_cast<float>(f) / static_cast<float>(fps));
                alang.push_back(fd[static_cast<std::size_t>(f)].ankleAlong[s]);
                alat.push_back(fd[static_cast<std::size_t>(f)].ankleLat[s]);
                aup.push_back(fd[static_cast<std::size_t>(f)].ankleUp[s]);
            }
            if (ph > stance && ph < 1.0f) {
                liftPeak = std::max(
                    liftPeak, fd[static_cast<std::size_t>(f)].ankleUp[s] -
                                  g.ankleUp[s]);
            }
        }
        if (ts.size() < 3) continue;

        // 侧向 / 竖直：应当恒定 → 报"相对均值的最大偏差"
        auto spread = [](const std::vector<float>& v) {
            float mean = 0.0f;
            for (float x : v) mean += x;
            mean /= static_cast<float>(v.size());
            float m = 0.0f;
            for (float x : v) m = std::max(m, std::fabs(x - mean));
            return m;
        };
        slideMm = std::max(slideMm, spread(alat) * 1000.0f);
        vertMm = std::max(vertMm, spread(aup) * 1000.0f);

        // 前向：最小二乘拟合直线（**不用生成时那个 A**，量的是成品本身）
        const std::size_t n = ts.size();
        float st = 0.0f, sv = 0.0f, stt = 0.0f, stv = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            st += ts[i];
            sv += alang[i];
            stt += ts[i] * ts[i];
            stv += ts[i] * alang[i];
        }
        const float dn = static_cast<float>(n);
        const float denom = dn * stt - st * st;
        const float slope = (std::fabs(denom) > 1.0e-9f)
                                ? (dn * stv - st * sv) / denom
                                : 0.0f;
        const float icpt = (sv - slope * st) / dn;
        float maxErr = 0.0f;
        for (std::size_t i = 0; i < n; ++i)
            maxErr = std::max(maxErr,
                              std::fabs(alang[i] - (icpt + slope * ts[i])));
        lineErrMm = std::max(lineErrMm, maxErr * 1000.0f);
        // 斜率 = 脚相对身体后退的速率（负号只是方向）。取**绝对值平均**：
        // 两条腿各拟合一条线，正常步态下两者应当几乎相同。
        speedSum += std::fabs(slope);
        ++speedN;
    }
    rep.stanceSlideMm = slideMm;
    rep.stanceVerticalMm = vertMm;
    rep.stanceLineErrMm = lineErrMm;
    rep.stanceSpeed = (speedN > 0) ? (speedSum / static_cast<float>(speedN))
                                   : 0.0f;
    rep.footLiftPeak = liftPeak;

    // ---- 闭合：末帧与首帧 ----
    {
        FrameData a, b;
        sampleAt(0.0f);
        for (int i = 0; i < kGaitRoleCount; ++i) {
            const int j = rep.rig.joint[i];
            if (j < 0) continue;
            a.pos[i] = originOf(world[static_cast<std::size_t>(j)]);
            a.have[i] = true;
        }
        sampleAt(clip.duration);
        float m = 0.0f;
        for (int i = 0; i < kGaitRoleCount; ++i) {
            const int j = rep.rig.joint[i];
            if (j < 0) continue;
            const glm::vec3 bp = originOf(world[static_cast<std::size_t>(j)]);
            m = std::max(m, glm::length(bp - a.pos[i]));
        }
        rep.loopErrMm = m * 1000.0f;
    }

    // ---- 镜像：半周期之后的"左右互换"应当等于跨矢状面的镜像 ----
    //
    // 判据就是表里那句"帧 25 = 帧 1 的左右镜像"。做法：把 t=0 的每个
    // 关节点按矢状面反射，与 t=T/2 的**对侧**关节比距离。
    // （不折腾四元数的镜像 —— 点比点没有任何符号歧义。）
    {
        const float half = clip.duration * 0.5f;
        FrameData a, b;
        sampleAt(0.0f);
        for (int i = 0; i < kGaitRoleCount; ++i) {
            const int j = rep.rig.joint[i];
            if (j < 0) continue;
            a.pos[i] = originOf(world[static_cast<std::size_t>(j)]);
            a.have[i] = true;
        }
        sampleAt(half);
        for (int i = 0; i < kGaitRoleCount; ++i) {
            const int j = rep.rig.joint[i];
            if (j < 0) continue;
            b.pos[i] = originOf(world[static_cast<std::size_t>(j)]);
            b.have[i] = true;
        }
        auto mirror = [&](const glm::vec3& v) {
            return v - 2.0f * glm::dot(v - g.hipsPos, g.left) * g.left;
        };
        float m = 0.0f;
        float restBias = 0.0f;
        for (int i = 0; i < kGaitRoleCount; ++i) {
            const int j = rep.rig.joint[i];
            if (j < 0) continue;
            // 对侧角色
            int k = i;
            switch (static_cast<GaitRole>(i)) {
            case GaitRole::LeftUpperLeg: k = static_cast<int>(GaitRole::RightUpperLeg); break;
            case GaitRole::LeftLowerLeg: k = static_cast<int>(GaitRole::RightLowerLeg); break;
            case GaitRole::LeftFoot: k = static_cast<int>(GaitRole::RightFoot); break;
            case GaitRole::LeftToe: k = static_cast<int>(GaitRole::RightToe); break;
            case GaitRole::RightUpperLeg: k = static_cast<int>(GaitRole::LeftUpperLeg); break;
            case GaitRole::RightLowerLeg: k = static_cast<int>(GaitRole::LeftLowerLeg); break;
            case GaitRole::RightFoot: k = static_cast<int>(GaitRole::LeftFoot); break;
            case GaitRole::RightToe: k = static_cast<int>(GaitRole::LeftToe); break;
            case GaitRole::LeftUpperArm: k = static_cast<int>(GaitRole::RightUpperArm); break;
            case GaitRole::LeftForeArm: k = static_cast<int>(GaitRole::RightForeArm); break;
            case GaitRole::RightUpperArm: k = static_cast<int>(GaitRole::LeftUpperArm); break;
            case GaitRole::RightForeArm: k = static_cast<int>(GaitRole::LeftForeArm); break;
            case GaitRole::LeftHand: k = static_cast<int>(GaitRole::RightHand); break;
            case GaitRole::RightHand: k = static_cast<int>(GaitRole::LeftHand); break;
            default: break;
            }
            if (rep.rig.joint[k] < 0) continue;
            // ⚠⚠ 这里量的是**绝对**的逐点镜像误差，也就是表里那句话的
            // 字面意思。它只在"导入姿势本身左右对称"时才可能接近 0：
            // 手搭的夹具 0.000007 mm 过关，换上 `anime_girl.fbx` 立刻
            // 568 mm —— 而它的**静止姿势自带 580 mm** 的不对称（一只脚
            // 在前、两只手臂的朝向差很多）。那不是生成器的错，任何动画
            // 都不可能把它抹平。
            //
            // 所以判据是**两条**：`mirrorErrMm` 要小（对称的骨架），
            // 而 `restMirrorMm` 是它的地板（不对称的骨架）；
            // `mirrorErrMm <= restMirrorMm + 容差` 才是对**任何**骨架
            // 都成立的那条 —— 意思是"这段动作没有把不对称变得更糟"。
            const int jk = rep.rig.joint[k];
            const glm::vec3 base = rest.pos(jk) - mirror(rest.pos(j));
            const glm::vec3 anim = b.pos[k] - mirror(a.pos[i]);
            m = std::max(m, glm::length(anim));
            restBias = std::max(restBias, glm::length(base));
        }
        rep.mirrorErrMm = m * 1000.0f;
        rep.restMirrorMm = restBias * 1000.0f;
    }

    // ---- 可达性 / 同手同脚 ----
    //
    // ⚠⚠ 这两个量必须用**角色下标**去索引 `FrameData::pos`，不能用
    //   `rig.joint[角色]` 拿到的**关节下标**。两套下标在这具骨架上恰好
    //   都不越界（角色数 19、关节数 19），于是错着也能算出一个像模像样的
    //   数：`pos[13]` 是**左上臂**而不是左脚，`pos[11]` 是**右脚**而不是
    //   左大腿 —— `reachMargin` 报 1.586（>1 = "IK 够不着"）、
    //   `armCounterM` 报 -0.361（<0 = "同手同脚"），两条判据同时红，
    //   而动作本身完全正确。2026-10-30 实测踩到，记在这里。
    {
        float maxReach = 0.0f;
        const GaitRole ulRole[2] = {GaitRole::LeftUpperLeg,
                                    GaitRole::RightUpperLeg};
        const GaitRole ftRole[2] = {GaitRole::LeftFoot, GaitRole::RightFoot};
        for (const FrameData& d : fd)
            for (int s = 0; s < 2; ++s) {
                if (rep.rig.joint[static_cast<int>(ulRole[s])] < 0 ||
                    rep.rig.joint[static_cast<int>(ftRole[s])] < 0)
                    continue;
                maxReach = std::max(
                    maxReach,
                    glm::length(d.pos[static_cast<int>(ftRole[s])] -
                                d.pos[static_cast<int>(ulRole[s])]));
            }
        rep.reachMargin = (g.legLen > 1.0e-6f) ? (maxReach / g.legLen) : 0.0f;

        // 触地帧（t=0）与半周期后（t=T/2）的对比。
        //
        // ⚠ 用**差值**而不是绝对值：真人角色的静止姿势几乎都把一只手放在
        //   前面（这只资产的手就比髋靠前 23 cm），绝对值会被这个偏置吃掉
        //   ——`anime_girl` 上绝对量只有 0.142，差值量是 0.38，后者才是
        //   "反相"这件事本身的幅度。
        if (rep.rig.has(GaitRole::LeftFoot) && rep.rig.has(GaitRole::LeftHand) &&
            fd.size() >= 2) {
            const std::size_t h = fd.size() / 2;
            const FrameData& d0 = fd.front();
            const FrameData& dh = fd[h];
            auto alongOf = [&](const FrameData& d, GaitRole r) {
                return glm::dot(
                    d.pos[static_cast<int>(r)] - g.hipsPos, g.fwd);
            };
            const float footSwing =
                alongOf(d0, GaitRole::LeftFoot) - alongOf(dh, GaitRole::LeftFoot);
            const float handSwing =
                alongOf(d0, GaitRole::LeftHand) - alongOf(dh, GaitRole::LeftHand);
            // 正 = 触地帧脚在前（腿的相位对）；负 = 手在前（同手同脚）
            rep.legSwingM = footSwing;
            rep.armCounterM = -handSwing;
        }
    }

    // ---- 统计 ----
    rep.tracks = static_cast<int>(clip.tracks.size());
    rep.keys = clip.totalKeys();
    rep.extremeKeys = 0;
    for (const EditTrack& t : clip.tracks)
        for (int i = 0; i < t.keyCount(); ++i)
            if (t.typeAt(i) == KeyType::Extreme) ++rep.extremeKeys;

    rep.cycleSeconds = clip.duration;
    rep.ok = finiteOk;
    if (!finiteOk) rep.error = "采样出现了 NaN / inf";
    return rep;
}

} // namespace animation
