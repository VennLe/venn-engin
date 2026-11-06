// ============================================================
// character/MannequinSelfTest.cpp
//
// 这套断言要防的是**"看起来对、其实错"**的三类事：
//
//   ① 骨表抄错（少一根 / 父子挂错 / ik_* 当成形变骨）。
//      症状不是崩，是"动画能播但手的位置差一截"。
//   ② 重定向把姿势挪了。父子关系一变（Neck 的父从 Spine2 变成 spine_05），
//      如果先算 restLocal 再求世界，整个人会平移到别处 —— 而不是报错。
//      所以这一条必须用**世界矩阵逐元素比对**钉住，不能靠看截图。
//   ③ 动画轨道没重映射。`AnimTrack::joint` 是下标，重定向后下标全变了；
//      漏掉这一步的表现是"动画在播，但扭的是别的骨"。
//
// 夹具是**手搭的 23 根 Mixamo 风格骨架**（不加载资产）：期望值可以手算，
// 而且刻意带旋转（纯平移的夹具测不出上面第 ② 类错误）。
// ============================================================

#include "character/MannequinSelfTest.h"

#include "character/MannequinSkeleton.h"

#include "assets/Skeleton.h"
#include "core/Logger.h"

#include "assetio/SkelFormat.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace character {

namespace {

int g_pass = 0;
int g_fail = 0;

// ⚠ detail 里**不能有空格**：工具用 `detail=(\S*)` 解析。
void report(const char* name, double actual, double expected, double tol,
            const std::string& detail = {}) {
    const bool ok = std::fabs(actual - expected) <= tol;
    VK_LOG_INFO("MQ-TEST %s actual=%.6f expected=%.6f tol=%.6f detail=%s "
                "result=%s",
                name, actual, expected, tol, detail.c_str(),
                ok ? "PASS" : "FAIL");
    if (ok) ++g_pass; else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("MQ-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s",
                name, ok ? 1 : 0, detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass; else ++g_fail;
}

// ------------------------------------------------------------ 夹具
// 一具 Mixamo 命名的 23 根骨骨架。刻意带旋转：只平移的链上，
// "先 restLocal 后世界"和"先世界后 restLocal"两种写法结果一样，
// 测不出第 ② 类错误。
struct Fixture {
    assets::Skeleton skel;
};

void addJoint(assets::Skeleton& s, const char* name, int parent,
              const glm::vec3& t, const glm::vec3& euler) {
    assets::Skeleton::Joint jt;
    jt.name = name;
    jt.parent = parent;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), t);
    m = glm::rotate(m, euler.z, glm::vec3(0, 0, 1));
    m = glm::rotate(m, euler.y, glm::vec3(0, 1, 0));
    m = glm::rotate(m, euler.x, glm::vec3(1, 0, 0));
    jt.restLocal = m;
    s.joints.push_back(jt);
}

Fixture makeMixamoLike() {
    Fixture f;
    assets::Skeleton& s = f.skel;

    // 躯干：Hips → Spine → Spine1 → Spine2 → Neck → Head → HeadTop_End
    addJoint(s, "Hips",      -1, {0.00f, 0.00f, 1.00f}, {0.00f, 0.00f, 0.00f});
    addJoint(s, "Spine",      0, {0.00f, 0.00f, 0.12f}, {0.05f, 0.00f, 0.02f});
    addJoint(s, "Spine1",     1, {0.00f, 0.00f, 0.12f}, {0.03f, 0.01f, 0.00f});
    addJoint(s, "Spine2",     2, {0.00f, 0.00f, 0.12f}, {-0.02f, 0.02f, 0.01f});
    addJoint(s, "Neck",       3, {0.00f, 0.00f, 0.12f}, {0.04f, -0.03f, 0.00f});
    addJoint(s, "Head",       4, {0.00f, 0.00f, 0.10f}, {-0.05f, 0.00f, 0.02f});
    addJoint(s, "HeadTop_End", 5, {0.00f, 0.00f, 0.10f}, {0.00f, 0.00f, 0.00f});

    // 手臂：左 7..10，右 11..14（肩挂在 Spine2 = 3）
    addJoint(s, "LeftShoulder",  3, { 0.05f, 0.00f, 0.08f}, {0.00f, 0.00f, 0.20f});
    addJoint(s, "LeftArm",       7, { 0.14f, 0.00f, 0.00f}, {0.00f, 0.00f, 0.12f});
    addJoint(s, "LeftForeArm",   8, { 0.26f, 0.00f, 0.00f}, {0.00f, 0.00f, -0.06f});
    addJoint(s, "LeftHand",      9, { 0.24f, 0.00f, 0.00f}, {0.00f, 0.00f, 0.00f});
    addJoint(s, "RightShoulder", 3, {-0.05f, 0.00f, 0.08f}, {0.00f, 0.00f, -0.20f});
    addJoint(s, "RightArm",     11, {-0.14f, 0.00f, 0.00f}, {0.00f, 0.00f, -0.12f});
    addJoint(s, "RightForeArm", 12, {-0.26f, 0.00f, 0.00f}, {0.00f, 0.00f, 0.06f});
    addJoint(s, "RightHand",    13, {-0.24f, 0.00f, 0.00f}, {0.00f, 0.00f, 0.00f});

    // 腿：左 15..18，右 19..22（挂在 Hips = 0）
    addJoint(s, "LeftUpLeg",    0, { 0.09f, 0.00f, -0.05f}, {0.02f, 0.00f, 0.00f});
    addJoint(s, "LeftLeg",     15, { 0.00f, 0.00f, -0.42f}, {-0.03f, 0.00f, 0.00f});
    addJoint(s, "LeftFoot",    16, { 0.00f, 0.00f, -0.42f}, {0.06f, 0.00f, 0.00f});
    addJoint(s, "LeftToeBase", 17, { 0.00f, 0.08f, -0.06f}, {0.00f, 0.00f, 0.00f});
    addJoint(s, "RightUpLeg",   0, {-0.09f, 0.00f, -0.05f}, {0.02f, 0.00f, 0.00f});
    addJoint(s, "RightLeg",    19, { 0.00f, 0.00f, -0.42f}, {-0.03f, 0.00f, 0.00f});
    addJoint(s, "RightFoot",   20, { 0.00f, 0.00f, -0.42f}, {0.06f, 0.00f, 0.00f});
    addJoint(s, "RightToeBase", 21, {0.00f, 0.08f, -0.06f}, {0.00f, 0.00f, 0.00f});

    const int n = s.jointCount();
    s.topoOrder.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) s.topoOrder[static_cast<size_t>(i)] = i;

    // 挂点：与独立骨架资产 `.vskel` 里带来的那三个一致。
    // 它们挂在**骨名**上，而重定向会把 `RightHand` 改名成 `hand_r` ——
    // 见 MannequinSkeleton.cpp ⑦（不改名的后果是"武器不跟手"，且无日志）。
    {
        auto addSocket = [&](const char* nm, const char* boneName) {
            assets::Skeleton::Socket sock;
            sock.name = nm;
            sock.boneName = boneName;
            sock.local = glm::mat4(1.0f);
            sock.bone = s.findJoint(boneName,
                                    assets::kJointMatchCaseInsensitive);
            s.sockets.push_back(std::move(sock));
        };
        addSocket("weapon_r", "RightHand");
        addSocket("weapon_l", "LeftHand");
        addSocket("eye", "Head");
    }

    // 静止世界 → invBind（真实加载器的做法：inverseBindMatrices 就是它的逆）
    std::vector<glm::mat4> world(static_cast<size_t>(n), glm::mat4(1.0f));
    for (int i : s.topoOrder) {
        const int p = s.joints[static_cast<size_t>(i)].parent;
        world[static_cast<size_t>(i)] =
            (p >= 0 ? world[static_cast<size_t>(p)] : glm::mat4(1.0f)) *
            s.joints[static_cast<size_t>(i)].restLocal;
        s.joints[static_cast<size_t>(i)].invBind =
            glm::inverse(world[static_cast<size_t>(i)]);
    }

    // 两条 clip：Idle 只动躯干；Walk 动腿 + 一条指向 HeadTop_End 的轨道
    //（后者在重定向后必须是**被丢弃**的那一条）。
    {
        assets::AnimationClip idle;
        idle.name = "Idle";
        idle.duration = 1.0f;
        for (int j : {1, 2, 4}) {
            assets::AnimTrack tr;
            tr.joint = j;
            tr.path = assets::AnimTrack::Rotation;
            tr.times = {0.0f, 0.5f, 1.0f};
            const glm::quat q = glm::angleAxis(0.10f * static_cast<float>(j),
                                               glm::vec3(0, 1, 0));
            for (int k = 0; k < 3; ++k)
                tr.values.push_back(glm::vec4(q.x, q.y, q.z, q.w));
            idle.tracks.push_back(std::move(tr));
        }
        s.clips.push_back(std::move(idle));
    }
    {
        assets::AnimationClip walk;
        walk.name = "Walk";
        walk.duration = 0.8f;
        for (int j : {15, 16, 19, 20}) {
            assets::AnimTrack tr;
            tr.joint = j;
            tr.path = assets::AnimTrack::Rotation;
            tr.times = {0.0f, 0.4f, 0.8f};
            const glm::quat q = glm::angleAxis(0.25f * static_cast<float>(j),
                                               glm::vec3(1, 0, 0));
            for (int k = 0; k < 3; ++k)
                tr.values.push_back(glm::vec4(q.x, q.y, q.z, q.w));
            walk.tracks.push_back(std::move(tr));
        }
        // 这条必须被丢掉：HeadTop_End 不在 UE 骨架里
        assets::AnimTrack aux;
        aux.joint = 6;
        aux.path = assets::AnimTrack::Translation;
        aux.times = {0.0f, 0.8f};
        aux.values = {glm::vec4(0, 0, 0, 0), glm::vec4(0, 0, 0.1f, 0)};
        walk.tracks.push_back(std::move(aux));
        s.clips.push_back(std::move(walk));
    }
    return f;
}

// 静止世界矩阵（与 MannequinSkeleton.cpp 的 restWorlds 同一约定）
std::vector<glm::mat4> restWorlds(const assets::Skeleton& s) {
    const int n = s.jointCount();
    std::vector<glm::mat4> w(static_cast<size_t>(n), glm::mat4(1.0f));
    for (int j : s.topoOrder) {
        const assets::Skeleton::Joint& jt = s.joints[static_cast<size_t>(j)];
        w[static_cast<size_t>(j)] =
            (jt.parent >= 0 ? w[static_cast<size_t>(jt.parent)] : jt.externalChain) *
            jt.restLocal;
    }
    return w;
}

// 两个矩阵的最大元素差
float maxDiff(const glm::mat4& a, const glm::mat4& b) {
    float d = 0.0f;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            d = std::max(d, std::fabs(a[c][r] - b[c][r]));
    return d;
}

// 按"逗号分隔候选"的同一口径解析源骨（与 MannequinSkeleton.cpp 一致）
int resolveAny(const assets::Skeleton& s, const char* spec) {
    if (spec == nullptr || *spec == '\0') return -1;
    std::string cur;
    for (const char* p = spec;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!cur.empty()) {
                const int j = s.findJoint(cur, assets::kJointMatchDefault);
                if (j >= 0) return j;
            }
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
    return -1;
}

} // namespace

int runMannequinSelfTest() {
    g_pass = 0;
    g_fail = 0;

    // ================= 组 A：骨表本身 =================
    //
    // 这些数字是 `strings -a SK_Mannequin.uasset` 数出来的，不是设计值。
    report("mq_count", mannequinBoneCount(), 35.0, 0.0, "SK_Mannequin");

    {
        bool topo = true;
        for (int i = 0; i < mannequinBoneCount(); ++i) {
            const int p = mannequinBones()[i].parent;
            if (p >= i) topo = false;          // 自己或子孙当爹 = 表写错了
        }
        reportFlag("mq_topology", topo, "parent<i");
    }
    {
        int dup = 0;
        for (int i = 0; i < mannequinBoneCount(); ++i)
            for (int j = i + 1; j < mannequinBoneCount(); ++j)
                if (std::string(mannequinBones()[i].name) ==
                    std::string(mannequinBones()[j].name))
                    ++dup;
        report("mq_unique_names", dup, 0.0, 0.0, "dups=0");
    }
    {
        // 抽 8 根关键骨，名字必须逐字命中（大小写敏感 —— 这些是资产里的原样）
        const char* want[] = {"root",  "pelvis",   "spine_04",    "spine_05",
                              "neck_02", "ball_l", "ik_hand_gun", "weapon_r"};
        int hit = 0;
        for (const char* w : want)
            if (mannequinBoneIndex(w) >= 0) ++hit;
        report("mq_key_names", hit, 8.0, 0.0, "8/8");
    }
    {
        // 形变骨集合必须是"22 根照抄的身体骨 + spine_04/05 + neck_02" = 25 根；
        // ik_* / weapon_* 全部非形变（它们是 IK 目标与挂点，不参与蒙皮），
        // `root` 也是非形变（UE 里它只当整具骨架的变换根）。
        //
        // ⚠ 判据**不能**写成"非 ik 的都该形变"：`root` 会立刻踩中。
        //   这个错我犯过一次，症状是自检红而代码是对的 —— 比代码错更难查。
        int deformTotal = 0;
        int ikDeforms = 0;
        for (int i = 0; i < mannequinBoneCount(); ++i) {
            const std::string nm = mannequinBones()[i].name;
            const bool ik = nm.rfind("ik_", 0) == 0 || nm.rfind("weapon_", 0) == 0;
            if (mannequinBoneDeforms(i)) ++deformTotal;
            if (ik && mannequinBoneDeforms(i)) ++ikDeforms;
        }
        report("mq_deform_total", deformTotal, 25.0, 0.0, "22body+3new");
        report("mq_ik_deforms", ikDeforms, 0.0, 0.0, "ik/weapon=nondeform");
    }
    {
        // 层级：这三处是"UE 和 Mixamo 长得不一样"最容易被挂错的地方
        const int neck = mannequinBoneIndex("neck_01");
        const int clav = mannequinBoneIndex("clavicle_l");
        const int ball = mannequinBoneIndex("ball_l");
        const int thigh = mannequinBoneIndex("thigh_l");
        const int spine5 = mannequinBoneIndex("spine_05");
        const int pelv = mannequinBoneIndex("pelvis");
        const int foot = mannequinBoneIndex("foot_l");
        int ok = 0;
        if (neck >= 0 && mannequinBones()[neck].parent == spine5) ++ok;
        if (clav >= 0 && mannequinBones()[clav].parent == spine5) ++ok;
        if (ball >= 0 && mannequinBones()[ball].parent == foot) ++ok;
        if (thigh >= 0 && mannequinBones()[thigh].parent == pelv) ++ok;
        report("mq_parents", ok, 4.0, 0.0, "neck/clav/ball/thigh");
    }

    // ================= 组 B：重定向 =================
    Fixture f = makeMixamoLike();
    const assets::Skeleton& src = f.skel;
    const std::vector<glm::mat4> srcWorld = restWorlds(src);

    assets::Skeleton dst;
    MannequinRetargetReport rep;
    const bool built = retargetToMannequin(src, dst, rep);
    reportFlag("mq_retarget_runs", built, "23/20bones");
    // 总骨数 = 源骨数(23) + 追加的 UE 骨(13)。**不是 35** —— 源里 UE 没有的
    // 辅助骨（`HeadTop_End`）原样保留在原下标上，这样顶点下标一个都不用动。
    // 见 MannequinSkeleton.cpp 的"为什么下标一个都不动"。
    report("mq_retarget_bones", dst.jointCount(), 36.0, 0.0, "23+13");
    report("mq_ue_bones", rep.ueBones, 35.0, 0.0, "35/35present");
    report("mq_kept_aux", rep.keptAux, 1.0, 0.0, "HeadTop_End");
    report("mq_missing_sources", rep.missing, 0.0, 0.0,
           rep.missingSources.empty() ? "none" : "has-missing");
    report("mq_copied", rep.copied, 22.0, 0.0, "copy=22");
    report("mq_synthesized", rep.synthesized, 13.0, 0.0, "synth=13");

    // ---- 约束 ②：世界姿势逐位不变 ----
    //
    // 这是整条链**唯一真正的验收点**：骨数从 23 变 36、`neck_01` 的父从
    // `Spine2` 改成 `spine_05`，但每一个**源骨**的世界变换必须和重定向前
    // 一模一样。而且因为"下标不动"，这里可以按下标逐个比 —— 比按名字找
    // 回来更强（下标本身就是要守住的东西）。
    {
        const std::vector<glm::mat4> dstWorld = restWorlds(dst);
        float worst = 0.0f;
        int worstIdx = -1;
        const int n = src.jointCount();
        for (int s = 0; s < n; ++s) {
            const float diff = maxDiff(dstWorld[static_cast<size_t>(s)],
                                       srcWorld[static_cast<size_t>(s)]);
            if (diff > worst) { worst = diff; worstIdx = s; }
        }
        report("mq_world_preserved", worst, 0.0, 1.0e-5,
               "n=" + std::to_string(n) +
                   ",worst=" + std::to_string(worstIdx));
        report("mq_world_compared", n, 23.0, 0.0, "23slots");
    }

    // ---- 约束 ②（续）：采样出来的 joint 矩阵也要一致 ----
    //
    // 世界姿势一致只证明"静止对"；蒙皮用的是 jointMat = invMeshNode*world*invBind。
    // 如果 invBind 没照抄（而是用新算的逆），静止帧也看不出来，一动就错。
    {
        float worst = 0.0f;
        std::string worstName;
        int samples = 0;
        const int n = src.jointCount();
        for (const assets::AnimationClip& c : src.clips) {
            const assets::AnimationClip* dc = dst.findClip(c.name);
            if (dc == nullptr) continue;
            const float times[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
            for (float k : times) {
                const float t = k * c.duration;
                std::vector<glm::mat4> a, b;
                src.sample(&c, t, a);
                dst.sample(dc, t, b);
                for (int s = 0; s < n && s < static_cast<int>(a.size()) &&
                                s < static_cast<int>(b.size()); ++s) {
                    const float diff = maxDiff(b[static_cast<size_t>(s)],
                                               a[static_cast<size_t>(s)]);
                    if (diff > worst) {
                        worst = diff;
                        worstName = dst.joints[static_cast<size_t>(s)].name;
                    }
                    ++samples;
                }
            }
        }
        report("mq_jointmat_preserved", worst, 0.0, 1.0e-5,
               "n=" + std::to_string(samples) + ",worst=" + worstName);
    }

    // ---- 约束 ①：没有额外产生"有权重的骨" ----
    //
    // 重定向只改骨架，不动权重的唯一保证是"调用方没有理由去刷权重"。
    // 这里能钉住的是"新增骨都是 ik_/weapon_/过渡骨"，即它们本来就不该有
    // 顶点绑上去。数量对不上就说明表被改过。
    {
        int deformSynth = 0;
        for (int i = 0; i < mannequinBoneCount(); ++i) {
            const MannequinBoneDef& d = mannequinBones()[i];
            if (d.origin == MannequinOrigin::Copy) continue;
            if (mannequinBoneDeforms(i)) ++deformSynth;
        }
        report("mq_synth_deform", deformSynth, 3.0, 0.0, "spine04/05+neck02");
    }

    // ---- 动画：一条都不丢、下标一个都不变 ----
    //
    // 这是"下标不动"设计的直接好处：源骨都在原下标上，所以 clip 可以**整块
    // 照抄**，包括那条原本指向 `HeadTop_End` 的轨道（它保留了，因为
    // `HeadTop_End` 也还留在原下标上）。如果哪天有人把重定向改成"重排下标"，
    // 这两条会立刻红 —— 而"重排"正是会破坏顶点关节下标的那个改动。
    report("mq_clip_count", static_cast<double>(dst.clips.size()),
           static_cast<double>(src.clips.size()), 0.0,
           "n=" + std::to_string(dst.clips.size()));
    report("mq_dropped_tracks", rep.droppedTracks, 0.0, 0.0, "none");
    {
        int bad = 0;
        int tracks = 0;
        for (const assets::AnimationClip& c : src.clips) {
            const assets::AnimationClip* dc = dst.findClip(c.name);
            if (dc == nullptr) { ++bad; continue; }
            if (dc->tracks.size() != c.tracks.size()) { ++bad; continue; }
            for (std::size_t i = 0; i < c.tracks.size(); ++i) {
                if (dc->tracks[i].joint != c.tracks[i].joint) ++bad;
                ++tracks;
            }
        }
        report("mq_track_index_stable", bad, 0.0, 0.0,
               "n=" + std::to_string(tracks));
    }
    {
        // 轨道指向的骨名必须是 UE 名（腿轨道 → thigh/calf），
        // 证明"改名"确实作用到了轨道能到达的槽上。
        int ok = 0;
        const assets::AnimationClip* dwalk = dst.findClip("Walk");
        if (dwalk) {
            for (const assets::AnimTrack& tr : dwalk->tracks) {
                const std::string nm = dst.joints[static_cast<size_t>(tr.joint)].name;
                if (nm.rfind("thigh_", 0) == 0 || nm.rfind("calf_", 0) == 0)
                    ++ok;
            }
        }
        report("mq_track_names_ue", ok, 4.0, 0.0, "4legs");
    }
    {
        // 按轨道实际指向的骨名去查，必须还能查回同一个下标。
        int ok = 0;
        const assets::AnimationClip* dwalk = dst.findClip("Walk");
        if (dwalk) {
            for (const assets::AnimTrack& tr : dwalk->tracks) {
                const std::string nm = dst.joints[static_cast<size_t>(tr.joint)].name;
                if (dst.findJoint(nm) == tr.joint) ++ok;
            }
        }
        report("mq_track_name_roundtrip", ok, 5.0, 0.0, "5/5");
    }

    // ---- 合成骨的位置 ----
    {
        const std::vector<glm::mat4> w = restWorlds(dst);
        // ⚠ 合成骨**不在表下标上**（它被追加在源槽之后），所以必须用
        //   `dst.findJoint` 拿存储下标；拿 `mannequinBoneIndex` 会越界读到
        //   别的骨，而且照样出数 —— 这个错很安静。
        auto slotOf = [&](const char* nm) {
            return dst.findJoint(nm, assets::kJointMatchExact |
                                        assets::kJointMatchCaseInsensitive);
        };
        auto pos = [&](const char* nm) {
            const int i = slotOf(nm);
            return i >= 0 ? glm::vec3(w[static_cast<size_t>(i)][3])
                          : glm::vec3(1e9f);
        };
        const int ikF = slotOf("ik_foot_l");
        const int footL = slotOf("foot_l");
        const int wpnR = slotOf("weapon_r");
        const int handR = slotOf("hand_r");
        const float ikFoot = (ikF >= 0 && footL >= 0)
                                 ? maxDiff(w[static_cast<size_t>(ikF)],
                                           w[static_cast<size_t>(footL)])
                                 : 1e9f;
        const float ikHand = (wpnR >= 0 && handR >= 0)
                                 ? maxDiff(w[static_cast<size_t>(wpnR)],
                                           w[static_cast<size_t>(handR)])
                                 : 1e9f;
        report("mq_ik_follow_foot", ikFoot, 0.0, 1.0e-6, "ik_foot_l=foot_l");
        report("mq_weapon_follow_hand", ikHand, 0.0, 1.0e-6, "weapon_r=hand_r");

        // spine_04 必须落在 spine_03 与 neck_01 的中间（1/3 处）
        const glm::vec3 p3 = pos("spine_03");
        const glm::vec3 p5 = pos("neck_01");
        const glm::vec3 p4 = pos("spine_04");
        const glm::vec3 want = glm::mix(p3, p5, 1.0f / 3.0f);
        report("mq_spine04_lerp", static_cast<double>(glm::length(p4 - want)),
               0.0, 1.0e-5, "t=1/3");
        const glm::vec3 p6 = pos("spine_05");
        const glm::vec3 want6 = glm::mix(p3, p5, 2.0f / 3.0f);
        report("mq_spine05_lerp", static_cast<double>(glm::length(p6 - want6)),
               0.0, 1.0e-5, "t=2/3");
    }

    // ---- 名字口径 ----
    {
        // UE 名能查到，且**落在源骨原来的下标上**（下标不动是硬要求）
        const int sp3 = dst.findJoint("spine_03",
                                      assets::kJointMatchCaseInsensitive);
        const int head = dst.findJoint("head", assets::kJointMatchCaseInsensitive);
        const int srcSp3 = resolveAny(src, "Spine2,spine_03");
        const int srcHead = resolveAny(src, "Head,head");
        report("mq_find_ue_names",
               (sp3 == srcSp3 && head == srcHead) ? 1.0 : 0.0, 1.0, 0.0,
               "slot=srcslot");
        // 旧名（Mixamo）必须查不到 —— 否则说明改名没生效，`findJoint("head")`
        // 之流会命中旧骨，"看着能用"，换一份资产就全错。
        const int hips = dst.findJoint("Hips", assets::kJointMatchCaseInsensitive);
        report("mq_old_names_gone", hips, -1.0, 0.0, "Hips=-1");
    }

    // ================= 组 C：幂等 =================
    //
    // 对已经是 UE 结构的骨架再跑一次重定向，世界姿势必须仍不变。
    // 这条防的是"重定向按别名糊到别的骨上"——第二遍才会暴露。
    {
        assets::Skeleton dst2;
        MannequinRetargetReport rep2;
        const bool again = retargetToMannequin(dst, dst2, rep2);
        const std::vector<glm::mat4> w1 = restWorlds(dst);
        const std::vector<glm::mat4> w2 = restWorlds(dst2);
        float worst = 0.0f;
        for (int i = 0; i < dst.jointCount() && i < dst2.jointCount(); ++i)
            worst = std::max(worst, maxDiff(w1[static_cast<size_t>(i)],
                                            w2[static_cast<size_t>(i)]));
        reportFlag("mq_idempotent_runs", again, "second_pass");
        report("mq_idempotent_world", worst, 0.0, 1.0e-5, "ue->ue");
        report("mq_idempotent_missing", rep2.missing, 0.0, 0.0, "none");
    }

    // ================= 组 D：挂点 + 独立骨架资产 =================

    // ---- D1 挂点跟着骨名改名（重定向之后）----
    //
    // 挂点存在 `Skeleton::sockets` 里、按**骨名**解析。重定向把
    // `RightHand` 改名成 `hand_r` 之后，不跟着改的挂点会静默指向 -1。
    {
        const assets::Skeleton::Socket* wr = dst.socket("weapon_r");
        const assets::Skeleton::Socket* wl = dst.socket("weapon_l");
        const assets::Skeleton::Socket* ey = dst.socket("eye");
        reportFlag("mq_socket_kept", dst.sockets.size() == 3,
                   "n=" + std::to_string(dst.sockets.size()));
        reportFlag("mq_socket_renamed",
                   wr != nullptr && wl != nullptr && ey != nullptr
                       && wr->boneName == "hand_r"
                       && wl->boneName == "hand_l" && ey->boneName == "head",
                   wr ? wr->boneName : "-");
        // 解析出来的下标必须就是新骨架里的那根骨（不是"名字对、下标没用"）
        reportFlag("mq_socket_resolved",
                   wr != nullptr && ey != nullptr
                       && wr->bone == dst.findJoint("hand_r",
                                                    assets::kJointMatchCaseInsensitive)
                       && ey->bone == dst.findJoint("head",
                                                    assets::kJointMatchCaseInsensitive),
                   wr ? ("idx=" + std::to_string(wr->bone)) : "null");
    }

    // ---- D2 `.vskel` 写盘 → 读盘 往返（用重定向后的真骨架，不是空壳）----
    //
    // 为什么用 `dst` 而不是手写一个单位阵骨架：`dst` 的 `restLocal` /
    // `invBind` 全是重定向算出来的**非平凡矩阵**（含插值骨与重挂父）。
    // 单位阵的骨架往返一遍"永远正确"，测不出任何字段的读写错位。
    {
        assetio::SourceSkeletonAsset sa;
        sa.name = "SK_Mannequin";
        sa.joints.resize(dst.joints.size());
        for (std::size_t j = 0; j < dst.joints.size(); ++j) {
            const assets::Skeleton::Joint& sj = dst.joints[j];
            assetio::SourceJoint& dj = sa.joints[j];
            dj.name = sj.name;
            dj.parent = sj.parent;
            dj.restLocal = sj.restLocal;
            dj.invBind = sj.invBind;
            dj.externalChain = sj.externalChain;
        }
        sa.topoOrder = dst.topoOrder;
        sa.invMeshNode = dst.invMeshNode;
        sa.skeletonToEngine = dst.skeletonToEngine;
        for (const assets::Skeleton::Socket& ss : dst.sockets) {
            assetio::SourceSocket ds;
            ds.name = ss.name;
            ds.boneName = ss.boneName;
            ds.local = ss.local;
            sa.sockets.push_back(std::move(ds));
        }
        sa.boneDigest = assetio::skeletonBoneDigest(sa.joints);

        const std::vector<std::uint8_t> bytes = assetio::writeSkelFile(sa);
        reportFlag("mq_vskel_written", !bytes.empty(),
                   "bytes=" + std::to_string(bytes.size()));

        assetio::SourceSkeletonAsset back;
        std::string err;
        const bool okRead =
            assetio::readSkelFile(bytes.data(), bytes.size(), back, &err);
        reportFlag("mq_vskel_roundtrip", okRead,
                   okRead ? "ok" : ("err=" + err));
        if (okRead) {
            report("mq_vskel_bones", back.jointCount(), dst.jointCount(), 0.0,
                   "=" + std::to_string(dst.jointCount()));
            // 逐骨比 restLocal / invBind：**逐元素**，不是"长度对不对"。
            float worst = 0.0f;
            int nameMismatch = 0;
            int parentMismatch = 0;
            for (std::size_t j = 0; j < back.joints.size(); ++j) {
                const assetio::SourceJoint& a = back.joints[j];
                const assets::Skeleton::Joint& b = dst.joints[j];
                if (a.name != b.name) ++nameMismatch;
                if (a.parent != b.parent) ++parentMismatch;
                worst = std::max(worst, maxDiff(a.restLocal, b.restLocal));
                worst = std::max(worst, maxDiff(a.invBind, b.invBind));
            }
            report("mq_vskel_matrices", worst, 0.0, 0.0, "local+invbind");
            report("mq_vskel_names", nameMismatch, 0.0, 0.0, "bytewise");
            report("mq_vskel_parents", parentMismatch, 0.0, 0.0, "bytewise");
            report("mq_vskel_digest",
                   static_cast<double>(back.boneDigest == sa.boneDigest), 1.0,
                   0.0, "self-consistent");
            report("mq_vskel_sockets", static_cast<double>(back.sockets.size()),
                   3.0, 0.0, "weapon_r/weapon_l/eye");
            reportFlag("mq_vskel_socket_names",
                       back.sockets.size() == 3 &&
                           back.sockets[0].name == "weapon_r" &&
                           back.sockets[0].boneName == "hand_r" &&
                           back.sockets[2].boneName == "head",
                       back.sockets.empty() ? "-" : back.sockets[0].boneName);
        }

        // ---- 坏文件必须被拒（不是崩、也不是"读出一具空骨架"）----
        {
            std::vector<std::uint8_t> bad = bytes;
            bad[0] = static_cast<std::uint8_t>('X');   // 魔数坏
            assetio::SourceSkeletonAsset junk;
            std::string e2;
            reportFlag("mq_vskel_reject_magic",
                       !assetio::readSkelFile(bad.data(), bad.size(), junk, &e2),
                       "magic");
        }
        {
            // 改一个**载荷**字节：魔数与长度都对，只有 CRC 能抓到。
            std::vector<std::uint8_t> bad = bytes;
            bad[bad.size() / 2] = static_cast<std::uint8_t>(bad[bad.size() / 2] ^ 0xFF);
            assetio::SourceSkeletonAsset junk;
            std::string e2;
            reportFlag("mq_vskel_reject_crc",
                       !assetio::readSkelFile(bad.data(), bad.size(), junk, &e2),
                       "crc");
        }
    }

    // ---- D3 真正出厂的那份资产（`writeMannequinSkeletonAsset`）----
    //
    // `assets/characters/mannequin/Rigs/SK_Mannequin.vskel` 就是这个函数写出来的。
    // 写盘 → 读回 → 35 根骨 + 3 个挂点，才算"盘上那份资产是对的"。
    // ⚠ 写到临时目录：自检不该动 `assets/`（回归会把它当基线）。
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path tmp =
            fs::temp_directory_path(ec) / "venn_mq_SK_Mannequin.vskel";
        std::string werr;
        const bool wrote = writeMannequinSkeletonAsset(tmp.string(), &werr);
        reportFlag("mq_emit_written", wrote, wrote ? "tmp" : ("err=" + werr));
        if (wrote) {
            std::ifstream fin(tmp, std::ios::binary);
            std::vector<std::uint8_t> bytes(
                (std::istreambuf_iterator<char>(fin)),
                std::istreambuf_iterator<char>());
            fin.close();
            assetio::SourceSkeletonAsset back;
            std::string err;
            const bool okRead =
                assetio::readSkelFile(bytes.data(), bytes.size(), back, &err);
            reportFlag("mq_emit_roundtrip", okRead, okRead ? "ok" : err);
            if (okRead) {
                int parentOk = 0;
                for (int i = 0; i < mannequinBoneCount() &&
                                i < back.jointCount(); ++i) {
                    if (back.joints[static_cast<std::size_t>(i)].name ==
                            mannequinBones()[i].name &&
                        back.joints[static_cast<std::size_t>(i)].parent ==
                            mannequinBones()[i].parent)
                        ++parentOk;
                }
                report("mq_emit_bones", back.jointCount(),
                       static_cast<double>(mannequinBoneCount()), 0.0, "35");
                report("mq_emit_table_match", parentOk,
                       static_cast<double>(mannequinBoneCount()), 0.0,
                       "name+parent");
                report("mq_emit_sockets",
                       static_cast<double>(back.sockets.size()), 3.0, 0.0,
                       "weapon_r/weapon_l/eye");
                // 参考骨架的静止姿势是单位阵（UE 的 Skeleton 资产不含姿势，
                // 见 MannequinSkeleton.h 里 `USkeleton` 那段）。这条断言
                // 是**故意的**：如果哪天有人往里塞了姿势，这里会红，
                // 于是"资产语义变了"不会被悄悄放过。
                float offIdentity = 0.0f;
                for (const assetio::SourceJoint& j : back.joints)
                    offIdentity = std::max(offIdentity,
                                           maxDiff(j.restLocal, glm::mat4(1.0f)));
                report("mq_emit_reference_pose", offIdentity, 0.0, 0.0,
                       "identity");
            }
            fs::remove(tmp, ec);
        }
    }

    VK_LOG_INFO("MQ-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

} // namespace character
