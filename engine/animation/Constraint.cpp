// ============================================================
// animation/Constraint.cpp
//
// 五条约束的求值 + 一条栈的顺序执行 + JSON 往返。**这里不碰渲染、不碰
// 实体**：输入一份 `Pose`，输出改过的 `Pose`。所以自检可以不开窗口、
// 不建渲染器就把"顺序有意义""关掉等于没有""限幅内逐位不变"钉住。
//
// 求值器的形状：
//
//     world      = 懒算（`poseToWorld`），每条写完就置脏
//     restWorld  = 更懒（只在 ChildOf 需要时算一次）
//
// 为什么懒算世界矩阵：`LimitRotation` 只看自己那根骨的局部旋转，为一个
// 纯局部操作跑一遍全骨架 FK 是白花钱；而 `CopyRotation` 必须看**上一条
// 改完之后**的世界矩阵（否则"先转上臂、再复制给前臂"读到的是转之前的
// 值，前臂少转一个量，且只有两条以上约束时才复现 —— 最难查的那类）。
// ============================================================

#include "animation/Constraint.h"

#include "animation/TwoBoneIK.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

namespace animation {

namespace {

using json = nlohmann::json;

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;

const char* const kTypeNames[kConTypeCount] = {
    "LimitRotation", "CopyRotation", "CopyLocation", "ChildOf", "TwoBoneIK",
};

glm::vec3 translationOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// 世界矩阵 → 旋转。与 `TwoBoneIK.cpp` 里那份**同一个式子**（按第 0 列的
// 长度归一，不是除行列式）：两条约束算同一个父骨旋转时差一个系数，症状是
// "复制旋转之后子骨跟着缩放变瘦"，而日志一行不报。
glm::quat worldRotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    const float s = glm::length(r[0]);
    if (s > 1.0e-8f) r /= s;
    return glm::normalize(glm::quat_cast(r));
}

glm::quat parentRotationOf(const assets::Skeleton& skel,
                           const std::vector<glm::mat4>& world, int joint) {
    const int p = skel.joints[static_cast<std::size_t>(joint)].parent;
    if (p >= 0 && p < static_cast<int>(world.size()))
        return worldRotationOf(world[static_cast<std::size_t>(p)]);
    return worldRotationOf(skel.joints[static_cast<std::size_t>(joint)].externalChain);
}

const glm::mat4& parentWorldOf(const assets::Skeleton& skel,
                               const std::vector<glm::mat4>& world, int joint) {
    const int p = skel.joints[static_cast<std::size_t>(joint)].parent;
    if (p >= 0 && p < static_cast<int>(world.size()))
        return world[static_cast<std::size_t>(p)];
    return skel.joints[static_cast<std::size_t>(joint)].externalChain;
}

glm::quat normalized(const glm::quat& q) {
    const float l = glm::length(q);
    return l > 1.0e-6f ? q / l : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

// 旋转混合。w >= 1 直接取目标（**不**走 slerp：`slerp(a,b,1)` 在浮点下
// 不保证逐位等于 b，而"满权重 = 精确复制"是要能断言的）。
glm::quat blendRotation(const glm::quat& from, const glm::quat& to, float w) {
    if (w >= 1.0f) return to;
    if (w <= 0.0f) return from;
    return normalized(glm::slerp(from, to, w));
}

// 一次求值的临时上下文：懒算的世界矩阵 + 脏标志。
struct EvalCtx {
    const assets::Skeleton& skel;
    Pose& pose;

    std::vector<glm::mat4> world;
    bool worldDirty = true;

    std::vector<glm::mat4> restWorld;
    bool restWorldReady = false;

    EvalCtx(const assets::Skeleton& s, Pose& p) : skel(s), pose(p) {}

    const std::vector<glm::mat4>& getWorld() {
        if (worldDirty) {
            poseToWorld(skel, pose, world);
            worldDirty = false;
        }
        return world;
    }

    const std::vector<glm::mat4>& getRestWorld() {
        if (!restWorldReady) {
            Pose rest;
            rest.setRest(skel);
            poseToWorld(skel, rest, restWorld);
            restWorldReady = true;
        }
        return restWorld;
    }

    void touched() { worldDirty = true; }
};

enum ApplyOutcome : int { kOutSkipped = 0, kOutApplied = 1, kOutFailed = 2 };

// ---- LimitRotation ----
//
// 欧拉约定 = glm 的 `eulerAngles` / `quat(vec3)`（0=X 1=Y 2=Z，YXZ 复合）。
// 换句话说：这里的"X 角度"不是万向架无关的，它就是**这个分解下**的角度。
// 对"肘只能 0..145°"这种单轴限位完全够用，而且 decompose→clamp→recompose
// 是同一个约定的往返（我们自己也用同一对函数）。
ApplyOutcome applyLimitRotation(EvalCtx& ctx, const Constraint& c, bool& hit) {
    hit = false;
    const int bone = c.bone;
    if (bone < 0 || bone >= ctx.pose.count() || bone >= ctx.skel.jointCount())
        return kOutFailed;

    const glm::quat cur = normalized(ctx.pose.rotation[static_cast<std::size_t>(bone)]);
    const glm::vec3 e = glm::degrees(glm::eulerAngles(cur));

    glm::vec3 ce = e;
    for (int a = 0; a < 3; ++a) {
        if (!(c.limitMask & (1 << a))) continue;
        const float lo = std::min(c.minDeg[a], c.maxDeg[a]);
        const float hi = std::max(c.minDeg[a], c.maxDeg[a]);
        ce[a] = std::clamp(e[a], lo, hi);
        // 逐轴比**浮点相等**：夹过的角度就是同一个数，不是"差不多的数"。
        if (ce[a] != e[a]) hit = true;
    }
    // 限幅以内：一个字节都不写（见头文件约定 ①）。
    if (!hit) return kOutSkipped;

    const glm::quat target = normalized(glm::quat(glm::radians(ce)));
    const glm::quat out = blendRotation(cur, target, c.influence);
    ctx.pose.rotation[static_cast<std::size_t>(bone)] = out;
    ctx.touched();
    return kOutApplied;
}

// ---- CopyRotation ----
ApplyOutcome applyCopyRotation(EvalCtx& ctx, const Constraint& c) {
    const int bone = c.bone;
    const int src = c.target;
    if (bone < 0 || src < 0) return kOutFailed;
    if (bone >= ctx.pose.count() || src >= ctx.skel.jointCount()) return kOutFailed;

    const std::vector<glm::mat4>& w = ctx.getWorld();
    const glm::quat wantWorld = worldRotationOf(w[static_cast<std::size_t>(src)]);
    const glm::quat parentRot = parentRotationOf(ctx.skel, w, bone);
    glm::quat wantLocal = normalized(glm::inverse(parentRot) * wantWorld);

    // 轴掩码：只抄选中的轴，其余保留本骨原来的角度。
    // ⚠ 这一步只能过欧拉（四元数没有"某根轴的角度"），所以掩码 ≠ 7 时
    //   会引入一次 decompose/recompose —— 精度损失 ~1e-7，方向正确。
    //   掩码 = 7（默认）走纯四元数，逐位精确。
    const glm::quat curLocal =
        normalized(ctx.pose.rotation[static_cast<std::size_t>(bone)]);
    if (c.axisMask != 7) {
        if (c.axisMask == 0) return kOutSkipped;
        glm::vec3 eo = glm::degrees(glm::eulerAngles(curLocal));
        glm::vec3 ew = glm::degrees(glm::eulerAngles(wantLocal));
        for (int a = 0; a < 3; ++a) {
            if (!(c.axisMask & (1 << a))) ew[a] = eo[a];
        }
        wantLocal = normalized(glm::quat(glm::radians(ew)));
    }

    const glm::quat out = blendRotation(curLocal, wantLocal, c.influence);
    ctx.pose.rotation[static_cast<std::size_t>(bone)] = out;
    ctx.touched();
    return kOutApplied;
}

// ---- CopyLocation ----
//
// 语义：让这根骨的**世界头部位置**等于目标骨的世界头部位置 + offset。
// 走"先算世界目标点，再换回父骨空间的局部平移"，而不是直接抄局部平移 ——
// 后者在父骨被别的约束转过之后会差一个父骨旋转（"跟着走却走错方向"）。
ApplyOutcome applyCopyLocation(EvalCtx& ctx, const Constraint& c) {
    const int bone = c.bone;
    const int src = c.target;
    if (bone < 0 || src < 0) return kOutFailed;
    if (bone >= ctx.pose.count() || src >= ctx.skel.jointCount()) return kOutFailed;

    const std::vector<glm::mat4>& w = ctx.getWorld();
    const glm::vec3 curWorld = translationOf(w[static_cast<std::size_t>(bone)]);
    const glm::vec3 wantWorld =
        translationOf(w[static_cast<std::size_t>(src)]) + c.offset;

    glm::vec3 delta = wantWorld - curWorld;
    for (int a = 0; a < 3; ++a) {
        if (!(c.axisMask & (1 << a))) delta[a] = 0.0f;
    }
    const float wgt = std::clamp(c.influence, 0.0f, 1.0f);
    const glm::vec3 newWorld = curWorld + delta * wgt;

    const glm::mat4& pw = parentWorldOf(ctx.skel, w, bone);
    const glm::vec3 localT =
        glm::vec3(glm::inverse(pw) * glm::vec4(newWorld, 1.0f));
    ctx.pose.translation[static_cast<std::size_t>(bone)] = localT;
    ctx.touched();
    return kOutApplied;
}

// ---- ChildOf ----
//
// "像挂在目标骨下面"：世界变换 = 目标骨世界 × **rest 时量出来的偏移**。
// 那个偏移就是 Blender 里 `Set Inverse` 的结果，只是我们从静止姿势直接
// 推导，不需要用户点一下。
ApplyOutcome applyChildOf(EvalCtx& ctx, const Constraint& c) {
    const int bone = c.bone;
    const int src = c.target;
    if (bone < 0 || src < 0) return kOutFailed;
    if (bone >= ctx.pose.count() || src >= ctx.skel.jointCount()) return kOutFailed;

    const std::vector<glm::mat4>& w = ctx.getWorld();
    const std::vector<glm::mat4>& rw = ctx.getRestWorld();

    const glm::mat4 offsetLocal =
        glm::inverse(rw[static_cast<std::size_t>(src)]) *
        rw[static_cast<std::size_t>(bone)];
    const glm::mat4 wantWorld = w[static_cast<std::size_t>(src)] * offsetLocal;

    const glm::mat4& pw = parentWorldOf(ctx.skel, w, bone);
    const glm::mat4 localM = glm::inverse(pw) * wantWorld;

    const glm::vec3 wantT = translationOf(localM);
    const glm::quat wantR = worldRotationOf(localM);

    const glm::vec3 curT = ctx.pose.translation[static_cast<std::size_t>(bone)];
    const glm::quat curR =
        normalized(ctx.pose.rotation[static_cast<std::size_t>(bone)]);

    const float wgt = std::clamp(c.influence, 0.0f, 1.0f);
    ctx.pose.translation[static_cast<std::size_t>(bone)] =
        curT + (wantT - curT) * wgt;
    ctx.pose.rotation[static_cast<std::size_t>(bone)] = blendRotation(curR, wantR, wgt);
    ctx.touched();
    return kOutApplied;
}

// ---- TwoBoneIK（薄包装）----
ApplyOutcome applyIK(EvalCtx& ctx, const Constraint& c, bool& solved) {
    solved = false;
    if (!c.ikHasTarget) return kOutFailed;

    TwoBoneIKSettings s;
    s.enabled = true;               // enabled / influence 由调用方短路
    s.hasTarget = true;
    s.target = c.ikTarget;
    s.weight = std::clamp(c.influence, 0.0f, 1.0f);
    s.poleMode = (c.ikPoleMode == 1) ? TwoBoneIKSettings::PoleFromVector
                                     : TwoBoneIKSettings::PoleFromAnimation;
    s.poleVector = c.ikPole;
    s.maxReachScale = std::clamp(c.ikMaxReach, 0.05f, 0.999f);
    // 下标 → 名字：`resolveTwoBoneChain` 的"留空自动挑最长子骨"是它自己的
    // 启发式，这里只在显式指定时覆盖。
    const auto nameOf = [&](int i) -> std::string {
        if (i < 0 || i >= ctx.skel.jointCount()) return std::string();
        return ctx.skel.joints[static_cast<std::size_t>(i)].name;
    };
    s.rootBone = nameOf(c.ikRoot);
    s.midBone = nameOf(c.ikMid);
    s.endBone = nameOf(c.ikEnd);

    // ---- 拉伸（先改骨长，再解算）----
    // 只放大 mid / end 的**局部平移长度**（glTF 里骨长就是子骨的平移量），
    // 不动旋转 —— 于是"够得更远"和"朝向对不对"互不干扰。
    if (c.stretch > 0.0f) {
        const std::vector<glm::mat4>& w = ctx.getWorld();
        const TwoBoneIKChain ch = resolveTwoBoneChain(ctx.skel, s);
        if (ch.valid()) {
            const glm::vec3 A = translationOf(w[static_cast<std::size_t>(ch.root)]);
            const glm::vec3 B = translationOf(w[static_cast<std::size_t>(ch.mid)]);
            const glm::vec3 C = translationOf(w[static_cast<std::size_t>(ch.end)]);
            const float reach = glm::length(B - A) + glm::length(C - B);
            const float want = glm::length(c.ikTarget - A);
            if (reach > 1.0e-4f && want > reach) {
                const float cap = 1.0f + std::max(0.0f, c.stretch);
                const float scale = std::min(want / reach, cap);
                const float wgt = std::clamp(c.influence, 0.0f, 1.0f);
                const float k = 1.0f + (scale - 1.0f) * wgt;
                ctx.pose.translation[static_cast<std::size_t>(ch.mid)] *= k;
                ctx.pose.translation[static_cast<std::size_t>(ch.end)] *= k;
                ctx.touched();
            }
        }
    }

    const TwoBoneIKResult r = solveTwoBoneIK(ctx.skel, s, ctx.pose);
    if (!r.applied) return kOutFailed;
    solved = true;
    ctx.touched();
    return kOutApplied;
}

} // namespace

// ============================================================
// 类型名
// ============================================================

const char* constraintTypeName(int t) {
    if (t < 0 || t >= kConTypeCount) return "?";
    return kTypeNames[t];
}

bool constraintTypeFromName(const std::string& name, int& out) {
    for (int i = 0; i < kConTypeCount; ++i) {
        const char* n = kTypeNames[i];
        if (name.size() != std::string(n).size()) continue;
        bool same = true;
        for (std::size_t k = 0; k < name.size() && same; ++k) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(name[k])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(n[k])));
            same = (a == b);
        }
        if (same) {
            out = i;
            return true;
        }
    }
    return false;
}

bool Constraint::needsWorld() const {
    switch (type) {
    case kConLimitRotation: return false;
    case kConCopyRotation:
    case kConCopyLocation:
    case kConChildOf:
    case kConTwoBoneIK: return true;
    default: return false;
    }
}

// ============================================================
// 栈
// ============================================================

int ConstraintStack::add(const Constraint& c) {
    items.push_back(c);
    return static_cast<int>(items.size()) - 1;
}

int ConstraintStack::removeAt(int i) {
    if (i < 0 || i >= size()) return 0;
    items.erase(items.begin() + i);
    return 1;
}

int ConstraintStack::moveUp(int i) {
    if (i <= 0 || i >= size()) return 0;
    std::swap(items[static_cast<std::size_t>(i)],
              items[static_cast<std::size_t>(i - 1)]);
    return 1;
}

int ConstraintStack::moveDown(int i) {
    if (i < 0 || i + 1 >= size()) return 0;
    std::swap(items[static_cast<std::size_t>(i)],
              items[static_cast<std::size_t>(i + 1)]);
    return 1;
}

int ConstraintStack::clearBone(int bone) {
    const std::size_t before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(),
                               [bone](const Constraint& c) { return c.bone == bone; }),
                items.end());
    return static_cast<int>(before - items.size());
}

int ConstraintStack::clear() {
    const int n = size();
    items.clear();
    return n;
}

int ConstraintStack::indexOf(int bone, int type) const {
    for (int i = 0; i < size(); ++i) {
        if (items[static_cast<std::size_t>(i)].bone == bone &&
            items[static_cast<std::size_t>(i)].type == type)
            return i;
    }
    return -1;
}

Constraint* ConstraintStack::at(int i) {
    if (i < 0 || i >= size()) return nullptr;
    return &items[static_cast<std::size_t>(i)];
}

const Constraint* ConstraintStack::at(int i) const {
    if (i < 0 || i >= size()) return nullptr;
    return &items[static_cast<std::size_t>(i)];
}

ConstraintEvalResult ConstraintStack::evaluate(const assets::Skeleton& skel,
                                              Pose& pose) const {
    ConstraintEvalResult res;
    res.total = size();
    if (items.empty()) return res;
    if (skel.jointCount() <= 0 || pose.count() < skel.jointCount()) {
        res.failed = size();
        return res;
    }

    EvalCtx ctx(skel, pose);
    for (const Constraint& c : items) {
        // 约定 ①：关掉 / influence 0 → 一个字节都不写。
        if (!c.enabled || c.influence <= 0.0f) {
            ++res.skipped;
            continue;
        }
        if (c.bone < 0 && c.type != kConTwoBoneIK) {
            ++res.failed;
            continue;
        }
        bool hit = false;
        bool solved = false;
        ApplyOutcome o = kOutSkipped;
        switch (c.type) {
        case kConLimitRotation: o = applyLimitRotation(ctx, c, hit); break;
        case kConCopyRotation: o = applyCopyRotation(ctx, c); break;
        case kConCopyLocation: o = applyCopyLocation(ctx, c); break;
        case kConChildOf: o = applyChildOf(ctx, c); break;
        case kConTwoBoneIK: o = applyIK(ctx, c, solved); break;
        default: o = kOutFailed; break;
        }
        if (o == kOutApplied) ++res.applied;
        else if (o == kOutSkipped) ++res.skipped;
        else ++res.failed;
        if (hit) ++res.clamped;
        if (solved) ++res.ikSolved;
    }
    return res;
}

unsigned long long ConstraintStack::digest() const {
    unsigned long long h = 1469598103934665603ULL;
    const auto feed = [&h](const void* p, std::size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= static_cast<unsigned long long>(b[i]);
            h *= 1099511628211ULL;
        }
    };
    for (const Constraint& c : items) {
        feed(&c.type, sizeof(c.type));
        feed(&c.bone, sizeof(c.bone));
        feed(&c.target, sizeof(c.target));
        feed(&c.influence, sizeof(c.influence));
        feed(&c.axisMask, sizeof(c.axisMask));
        feed(&c.limitMask, sizeof(c.limitMask));
        feed(&c.offset, sizeof(c.offset));
        feed(&c.minDeg, sizeof(c.minDeg));
        feed(&c.maxDeg, sizeof(c.maxDeg));
        feed(&c.ikRoot, sizeof(c.ikRoot));
        feed(&c.ikMid, sizeof(c.ikMid));
        feed(&c.ikEnd, sizeof(c.ikEnd));
        feed(&c.ikTarget, sizeof(c.ikTarget));
        feed(&c.ikPole, sizeof(c.ikPole));
        feed(&c.ikMaxReach, sizeof(c.ikMaxReach));
        feed(&c.stretch, sizeof(c.stretch));
        // enabled / ikHasTarget / ikPoleMode 是 bool / 小整数，用 1 字节喂，
        // 免得 padding 进摘要（同一份栈在不同编译器下摘要不同 = 假红）。
        const unsigned char flags =
            static_cast<unsigned char>((c.enabled ? 1 : 0) |
                                       ((c.ikHasTarget ? 1 : 0) << 1) |
                                       ((c.ikPoleMode & 0x3) << 2));
        feed(&flags, 1);
    }
    return h;
}

void ConstraintStack::logSummary(const char* tag) const {
    int perType[kConTypeCount] = {0, 0, 0, 0, 0};
    int disabled = 0;
    for (const Constraint& c : items) {
        if (c.type >= 0 && c.type < kConTypeCount) ++perType[c.type];
        if (!c.enabled) ++disabled;
    }
    VK_LOG_INFO("CONSTRAINT-STACK tag=%s n=%d limit=%d copyRot=%d copyLoc=%d "
                "childOf=%d ik=%d disabled=%d digest=%016llx",
                tag ? tag : "-", size(), perType[kConLimitRotation],
                perType[kConCopyRotation], perType[kConCopyLocation],
                perType[kConChildOf], perType[kConTwoBoneIK], disabled,
                digest());
}

// ============================================================
// JSON 往返
// ============================================================

bool saveConstraints(const std::string& path, const ConstraintStack& stack,
                     const assets::Skeleton& skel, std::string& err) {
    err.clear();
    if (path.empty()) {
        err = "路径为空";
        return false;
    }
    const auto nameOf = [&skel](int i) -> std::string {
        if (i < 0 || i >= skel.jointCount()) return std::string();
        return skel.joints[static_cast<std::size_t>(i)].name;
    };
    json doc;
    doc["format"] = "venn.constraints";
    doc["version"] = 1;
    doc["joints"] = skel.jointCount();

    json arr = json::array();
    for (const Constraint& c : stack.items) {
        json o;
        o["type"] = constraintTypeName(c.type);
        o["enabled"] = c.enabled;
        o["influence"] = c.influence;
        // 名字 + 下标都写：名字给人看，下标给读盘用（并且让"重导入之后
        // 下标变了"在 diff 里一眼可见）。
        o["bone"] = nameOf(c.bone);
        o["boneIndex"] = c.bone;
        o["target"] = nameOf(c.target);
        o["targetIndex"] = c.target;
        o["axisMask"] = c.axisMask;
        o["offset"] = json::array({c.offset.x, c.offset.y, c.offset.z});
        o["limitMask"] = c.limitMask;
        o["minDeg"] = json::array({c.minDeg[0], c.minDeg[1], c.minDeg[2]});
        o["maxDeg"] = json::array({c.maxDeg[0], c.maxDeg[1], c.maxDeg[2]});
        json ik;
        ik["root"] = nameOf(c.ikRoot);
        ik["mid"] = nameOf(c.ikMid);
        ik["end"] = nameOf(c.ikEnd);
        ik["rootIndex"] = c.ikRoot;
        ik["midIndex"] = c.ikMid;
        ik["endIndex"] = c.ikEnd;
        ik["hasTarget"] = c.ikHasTarget;
        ik["target"] = json::array({c.ikTarget.x, c.ikTarget.y, c.ikTarget.z});
        ik["poleMode"] = c.ikPoleMode;
        ik["pole"] = json::array({c.ikPole.x, c.ikPole.y, c.ikPole.z});
        ik["maxReach"] = c.ikMaxReach;
        ik["stretch"] = c.stretch;
        o["ik"] = std::move(ik);
        arr.push_back(std::move(o));
    }
    doc["items"] = std::move(arr);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "写不了文件";
        return false;
    }
    f << doc.dump(1);
    if (!f) {
        err = "写入失败";
        return false;
    }
    VK_LOG_INFO("CONSTRAINT-SAVE path='%s' items=%d", path.c_str(), stack.size());
    return true;
}

bool loadConstraints(const std::string& path, const assets::Skeleton& skel,
                     ConstraintStack& out, std::string& err, int* unresolved) {
    err.clear();
    if (unresolved) *unresolved = 0;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "打不开文件";
        return false;
    }
    json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        err = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
    if (!doc.is_object() || doc.value("format", std::string()) != "venn.constraints") {
        err = "不是 venn.constraints 文件";
        return false;
    }

    const auto readVec3 = [](const json& j, glm::vec3& v, const glm::vec3& def) {
        if (!j.is_array() || j.size() != 3) {
            v = def;
            return;
        }
        v = glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    };

    ConstraintStack st;
    int bad = 0;
    // 下标优先（越界就作废），名字只作兜底**注释**用 —— 不按名字解析：
    // 同名骨在不同导入批次里可能指不同的关节（FBX 的 `mixamorig:` 前缀、
    // glTF 的重名节点），"猜一个同名骨"比"这条失效"更难查。
    const auto readIndex = [&skel, &bad](const json& o, const char* idxKey,
                                        int& dst) {
        dst = o.value(idxKey, -1);
        if (dst < -1 || dst >= skel.jointCount()) {
            dst = -1;
            ++bad;
        }
    };

    const auto it = doc.find("items");
    if (it != doc.end() && it->is_array()) {
        for (const json& o : *it) {
            Constraint c;
            if (!constraintTypeFromName(o.value("type", std::string()), c.type)) {
                ++bad;
                continue;
            }
            c.enabled = o.value("enabled", true);
            c.influence = o.value("influence", 1.0f);
            readIndex(o, "boneIndex", c.bone);
            readIndex(o, "targetIndex", c.target);
            c.axisMask = o.value("axisMask", 7);
            c.limitMask = o.value("limitMask", 7);
            const auto mo = o.find("offset");
            if (mo != o.end()) readVec3(*mo, c.offset, glm::vec3(0.0f));
            const auto mn = o.find("minDeg");
            if (mn != o.end() && mn->is_array() && mn->size() == 3) {
                for (int a = 0; a < 3; ++a) c.minDeg[a] = (*mn)[a].get<float>();
            }
            const auto mx = o.find("maxDeg");
            if (mx != o.end() && mx->is_array() && mx->size() == 3) {
                for (int a = 0; a < 3; ++a) c.maxDeg[a] = (*mx)[a].get<float>();
            }
            const auto ik = o.find("ik");
            if (ik != o.end() && ik->is_object()) {
                readIndex(*ik, "rootIndex", c.ikRoot);
                readIndex(*ik, "midIndex", c.ikMid);
                readIndex(*ik, "endIndex", c.ikEnd);
                c.ikHasTarget = ik->value("hasTarget", false);
                c.ikPoleMode = ik->value("poleMode", 0);
                c.ikMaxReach = ik->value("maxReach", 0.999f);
                c.stretch = ik->value("stretch", 0.0f);
                const auto tp = ik->find("target");
                if (tp != ik->end()) readVec3(*tp, c.ikTarget, glm::vec3(0.0f));
                const auto pl = ik->find("pole");
                if (pl != ik->end()) readVec3(*pl, c.ikPole, glm::vec3(0.0f, 0.0f, 1.0f));
            }
            if (c.bone < 0 && c.type != kConTwoBoneIK) ++bad;
            st.items.push_back(c);
        }
    }
    if (unresolved) *unresolved = bad;
    out = std::move(st);
    VK_LOG_INFO("CONSTRAINT-LOAD path='%s' items=%d bad=%d", path.c_str(),
                out.size(), bad);
    return true;
}

} // namespace animation
