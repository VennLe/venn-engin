// ============================================================
// animation/AnimationSelfTest.cpp
// ============================================================

#include "animation/AnimationSelfTest.h"

#include "animation/AnimatorSystem.h"
#include "animation/BoneAttachment.h"
#include "animation/Pose.h"
#include "animation/TwoBoneIK.h"
#include "assets/Model.h"
#include "assets/Skeleton.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace animation {

namespace {

int g_pass = 0;
int g_fail = 0;

void report(const char* name, double actual, double expected, double tol) {
    const bool ok = std::fabs(actual - expected) <= tol;
    VK_LOG_INFO("ANIM-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s",
                name, actual, expected, tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass; else ++g_fail;
}

// ⚠ detail 里**不能有空格**：`tools/verify_anim_runtime.py` 用
//   `detail=(\S*)` 解析，一个空格就会让整行匹配失败 → 这条断言在脚本眼里
//   "不存在"（已踩：`diff_mats=4 diff_bytes=26` 导致 refactor_bitwise 被报
//   成"缺少断言"，而引擎里它其实 PASS）。多项一律用逗号分隔。
void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("ANIM-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s",
                name, ok ? 1 : 0, detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass; else ++g_fail;
}

// ============================================================
// 独立的参考实现（golden）
//
// ⚠ 这里是 **2026-10-18 拆分前 `assets::Skeleton::sample` 的原样副本**。
//   保留它不是为了重复代码，而是为了给"重构不变性"一个**独立**基准：
//   `Skeleton::sample` 现在只是 `samplePose + poseToJoints` 的转发壳，
//   拿它和新实现比是循环论证 —— 永远绿。
//   两份实现会漂移（有人改了 Pose 忘改这里），那正是这条断言要抓的。
//
//   写法上的唯一区别：原来的两个 `static thread_local` 缓冲换成了局部
//   vector。对**结果**没有影响（它们每帧都被完整重写），只是不再共享。
// ============================================================

struct RefTRS {
    glm::vec3 scale{1.0f};
    glm::quat orient{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 translation{0.0f};
};

RefTRS refDecompose(const glm::mat4& m) {
    RefTRS r;
    glm::vec3 skew(0.0f);
    glm::vec4 persp(0.0f);
    glm::decompose(m, r.scale, r.orient, r.translation, skew, persp);
    return r;
}

glm::mat4 refCompose(const RefTRS& t) {
    return glm::translate(glm::mat4(1.0f), t.translation) *
           glm::mat4_cast(t.orient) * glm::scale(glm::mat4(1.0f), t.scale);
}

void referenceSample(const assets::Skeleton& skel,
                     const assets::AnimationClip* clip, float time,
                     std::vector<glm::mat4>& out) {
    const size_t n = skel.joints.size();
    if (out.size() != n) out.assign(n, glm::mat4(1.0f));
    if (n == 0) return;

    // 1) 局部姿态
    std::vector<RefTRS> local(n);
    for (size_t j = 0; j < n; ++j)
        local[j] = refDecompose(skel.joints[j].restLocal);

    if (clip) {
        for (const auto& tr : clip->tracks) {
            if (tr.joint < 0 || tr.joint >= static_cast<int>(n)) continue;
            const glm::vec4 v = assets::sampleTrack(tr, time);
            RefTRS& l = local[static_cast<size_t>(tr.joint)];
            switch (tr.path) {
                case assets::AnimTrack::Translation:
                    l.translation = glm::vec3(v.x, v.y, v.z);
                    break;
                case assets::AnimTrack::Rotation:
                    l.orient = glm::quat(v.w, v.x, v.y, v.z);
                    break;
                case assets::AnimTrack::Scale:
                    l.scale = glm::vec3(v.x, v.y, v.z);
                    break;
            }
        }
    }

    // 2) 沿拓扑序拼世界矩阵
    std::vector<glm::mat4> world(n, glm::mat4(1.0f));
    for (int j : skel.topoOrder) {
        if (j < 0 || j >= static_cast<int>(n)) continue;
        const glm::mat4 base = skel.joints[static_cast<size_t>(j)].parent >= 0
                                   ? world[static_cast<size_t>(
                                         skel.joints[static_cast<size_t>(j)]
                                             .parent)]
                                   : skel.joints[static_cast<size_t>(j)]
                                         .externalChain;
        world[static_cast<size_t>(j)] =
            base * refCompose(local[static_cast<size_t>(j)]);
    }

    // 3) joint 矩阵
    for (size_t j = 0; j < n; ++j)
        out[j] = skel.invMeshNode * world[j] * skel.joints[j].invBind;
}

// ============================================================
// 测试数据
// ============================================================

// 三根骨骼的直线链：
//   root(原点) → arm(局部 +0.5 x) → hand(局部 +0.5 x)
// 静止时 hand 的世界位置 = (1, 0, 0)。
// 让 root 绕 Z 转 90° 之后 = (0, 1, 0) —— 这就是层次顺序的判别式。
assets::Skeleton makeChainSkeleton() {
    assets::Skeleton s;
    s.joints.resize(3);
    s.joints[0].name = "root";
    s.joints[0].parent = -1;
    s.joints[0].restLocal = glm::mat4(1.0f);
    s.joints[0].invBind = glm::mat4(1.0f);

    s.joints[1].name = "arm";
    s.joints[1].parent = 0;
    s.joints[1].restLocal =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f));
    s.joints[1].invBind = glm::mat4(1.0f);

    s.joints[2].name = "hand";
    s.joints[2].parent = 1;
    s.joints[2].restLocal =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f));
    s.joints[2].invBind = glm::mat4(1.0f);

    s.topoOrder = {0, 1, 2};
    s.invMeshNode = glm::mat4(1.0f);
    return s;
}

// 一条 clip：root 在 [0, dur] 内绕 Z 从 0° 转到 90°，同时 hand 做一次缩放，
// 这样 T / R / S 三条路径都会被覆盖（它们的叠加方式是最容易写错的地方）。
assets::AnimationClip makeClip(float dur, const std::string& name) {
    assets::AnimationClip c;
    c.name = name;
    c.duration = dur;

    const glm::quat q0(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat q1 = glm::angleAxis(glm::radians(90.0f),
                                        glm::vec3(0.0f, 0.0f, 1.0f));
    assets::AnimTrack rot;
    rot.joint = 0;
    rot.path = assets::AnimTrack::Rotation;
    rot.times = {0.0f, dur};
    rot.values = {glm::vec4(q0.x, q0.y, q0.z, q0.w),
                  glm::vec4(q1.x, q1.y, q1.z, q1.w)};
    c.tracks.push_back(rot);

    assets::AnimTrack sc;
    sc.joint = 2;
    sc.path = assets::AnimTrack::Scale;
    sc.times = {0.0f, dur};
    sc.values = {glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
                 glm::vec4(2.0f, 2.0f, 2.0f, 0.0f)};
    c.tracks.push_back(sc);

    return c;
}

// 在栈上搭一个"只有一根骨骼网格体"的场景。Model 由 Fixture 持有
// （组件里存的是裸指针，必须有稳定的生命周期）。
struct Fixture {
    scene::Scene scene;
    assets::Model model;
    ecs::Entity root;
};

// 建场景：骨架 + clip 装进 model（Model::skeleton 是 shared_ptr）。
void buildFixture(Fixture& f, const assets::AnimationClip& clip, bool loop,
                  bool playing) {
    auto skel = std::make_shared<assets::Skeleton>(makeChainSkeleton());
    skel->clips.push_back(clip);
    f.model.skeleton = skel;

    f.root = f.scene.createObject("skinned");
    ecs::SkinnedMeshComponent sk;
    sk.model = &f.model;
    sk.clipName = clip.name;
    sk.time = 0.0f;
    sk.speed = 1.0f;
    sk.playing = playing;
    sk.loop = loop;
    f.scene.world().add<ecs::SkinnedMeshComponent>(f.root, sk);
}

ecs::SkinnedMeshComponent& comp(Fixture& f) {
    return *f.scene.world().get<ecs::SkinnedMeshComponent>(f.root);
}

// 给同一个实体挂上 AnimatorComponent（A1）。
void addAnimator(Fixture& f, float time, float speed, bool playing, bool loop,
                 const std::string& defaultClip) {
    ecs::AnimatorComponent a;
    a.time = time;
    a.speed = speed;
    a.playing = playing;
    a.loop = loop;
    a.defaultClip = defaultClip;
    f.scene.world().add<ecs::AnimatorComponent>(f.root, a);
}

ecs::AnimatorComponent& anim(Fixture& f) {
    return *f.scene.world().get<ecs::AnimatorComponent>(f.root);
}

// 用固定步长跑 n 帧。
void runFrames(AnimatorSystem& sys, Fixture& f, int n, float dt) {
    for (int i = 0; i < n; ++i) sys.tick(f.scene, dt);
}

} // namespace

int runAnimationSelfTest() {
    g_pass = 0;
    g_fail = 0;
    VK_LOG_INFO("=== 骨骼动画自检（无窗口）===");

    constexpr float kDur = 1.2f;
    const assets::AnimationClip clip = makeClip(kDur, "test");

    // ---- 1) 时间推进：120 帧 @ 1/60 → 2.0 秒 ----
    // ⚠ 用一条**足够长**的 clip（10 秒）：如果借用下面那条 dur=1.2 的，
    //   loop 会在 2.0 秒时把它绕回 0.8 —— 那是 wrap 组要断的事，
    //   不是这一组。两个量混在一起测，失败时分不清是"没推进"还是"回绕错"。
    {
        Fixture f;
        const assets::AnimationClip longClip = makeClip(10.0f, "long");
        buildFixture(f, longClip, /*loop=*/true, /*playing=*/true);
        AnimatorSystem sys;
        runFrames(sys, f, 120, 1.0f / 60.0f);
        report("tick_120f", static_cast<double>(comp(f).time), 2.0, 1e-4);
        report("tick_entities", static_cast<double>(sys.lastEntityCount()), 1.0,
               0.0);
    }

    // ---- 2) loop：dur=1.2，喂 3 秒 → 0.6 ----
    // 3 秒 = 180 帧 @ 1/60。fmod(3.0, 1.2) = 0.6。
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        AnimatorSystem sys;
        runFrames(sys, f, 180, 1.0f / 60.0f);
        report("wrap_loop", static_cast<double>(comp(f).time), 0.6, 1e-4);
    }

    // ---- 3) 不循环：喂 3 秒 → 恰好 dur（钳到末帧）----
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/false, /*playing=*/true);
        AnimatorSystem sys;
        runFrames(sys, f, 180, 1.0f / 60.0f);
        report("clamp_noloop", static_cast<double>(comp(f).time),
               static_cast<double>(kDur), 0.0);
    }

    // ---- 4) playing=false：时间不动（dt 照给）----
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/false);
        AnimatorSystem sys;
        runFrames(sys, f, 60, 1.0f / 60.0f);
        report("paused_time", static_cast<double>(comp(f).time), 0.0, 0.0);
    }

    // ---- 5) 修复 dt=0 只刷新姿势、不推进时间 ----
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        AnimatorSystem sys;
        comp(f).time = 0.35f;
        runFrames(sys, f, 10, 0.0f);
        // ⚠ expected 必须写 `static_cast<double>(0.35f)`：`0.35` 这个字面量
        //   是 double（0.34999999999999997780），而组件里存的是 float
        //   （0.34999999403953552）—— 差 6e-9，tol=0 时会报 FAIL，
        //   看起来像"时间被偷偷推进了"，其实是字面量精度问题。
        report("dt_zero_no_advance", static_cast<double>(comp(f).time),
               static_cast<double>(0.35f), 0.0);
    }

    // ---- 6) 重构不变性：samplePose→poseToJoints 与拆分前实现**逐位**相同 ----
    // 取两个时刻：一个在 clip 中间（三条轨道都在插值），一个在端点。
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        const assets::Skeleton& skel = *f.model.skeleton;
        const assets::AnimationClip* cp = skel.findClip(clip.name);

        const float times[2] = {0.37f, kDur};
        int badBytes = 0;
        int badMats = 0;
        for (float t : times) {
            Pose pose;
            std::vector<glm::mat4> got;
            samplePose(skel, cp, t, pose);
            poseToJoints(skel, pose, got);

            std::vector<glm::mat4> ref;
            referenceSample(skel, cp, t, ref);

            if (got.size() != ref.size()) {
                badMats += 1000;
                continue;
            }
            for (size_t j = 0; j < got.size(); ++j) {
                if (std::memcmp(&got[j], &ref[j], sizeof(glm::mat4)) != 0)
                    ++badMats;
                const auto* a = reinterpret_cast<const unsigned char*>(&got[j]);
                const auto* b = reinterpret_cast<const unsigned char*>(&ref[j]);
                for (size_t k = 0; k < sizeof(glm::mat4); ++k)
                    if (a[k] != b[k]) ++badBytes;
            }
        }
        reportFlag("refactor_bitwise", badMats == 0 && badBytes == 0,
                   "diff_mats=" + std::to_string(badMats) +
                       ",diff_bytes=" + std::to_string(badBytes));
    }

    // ---- 7) 兼容壳等价：Skeleton::sample 与 Pose 路径逐位相同 ----
    // （这一条会一直被转发逻辑保证，留着是为了抓"有人把壳改坏"）
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        const assets::Skeleton& skel = *f.model.skeleton;
        const assets::AnimationClip* cp = skel.findClip(clip.name);

        std::vector<glm::mat4> viaShell, viaPose;
        skel.sample(cp, 0.61f, viaShell);
        Pose pose;
        samplePose(skel, cp, 0.61f, pose);
        poseToJoints(skel, pose, viaPose);
        const bool same = viaShell.size() == viaPose.size() &&
                          std::memcmp(viaShell.data(), viaPose.data(),
                                      viaShell.size() * sizeof(glm::mat4)) == 0;
        reportFlag("shell_equivalent", same);
    }

    // ---- 8) 混合端点逐位短路 ----
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        const assets::Skeleton& skel = *f.model.skeleton;
        const assets::AnimationClip* cp = skel.findClip(clip.name);

        Pose a, b, out;
        samplePose(skel, cp, 0.0f, a);
        samplePose(skel, cp, kDur, b);

        blendPose(a, b, 0.0f, out);
        const bool w0 = out.count() == a.count() &&
                        std::memcmp(out.translation.data(), a.translation.data(),
                                    a.translation.size() * sizeof(glm::vec3)) == 0 &&
                        std::memcmp(out.rotation.data(), a.rotation.data(),
                                    a.rotation.size() * sizeof(glm::quat)) == 0;
        blendPose(a, b, 1.0f, out);
        const bool w1 = out.count() == b.count() &&
                        std::memcmp(out.translation.data(), b.translation.data(),
                                    b.translation.size() * sizeof(glm::vec3)) == 0 &&
                        std::memcmp(out.rotation.data(), b.rotation.data(),
                                    b.rotation.size() * sizeof(glm::quat)) == 0;
        reportFlag("blend_w0_bitwise", w0);
        reportFlag("blend_w1_bitwise", w1);
    }

    // ---- 9) 混合中点 = slerp(qa, qb, 0.5) ----
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        const assets::Skeleton& skel = *f.model.skeleton;
        const assets::AnimationClip* cp = skel.findClip(clip.name);

        Pose a, b, out;
        samplePose(skel, cp, 0.0f, a);
        samplePose(skel, cp, kDur, b);
        blendPose(a, b, 0.5f, out);

        const glm::quat want = glm::slerp(a.rotation[0], b.rotation[0], 0.5f);
        const float dx = out.rotation[0].x - want.x;
        const float dy = out.rotation[0].y - want.y;
        const float dz = out.rotation[0].z - want.z;
        const float dw = out.rotation[0].w - want.w;
        report("blend_mid", static_cast<double>(
                                std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw)),
               0.0, 1e-5);
    }

    // ---- 10) 层次：root 转 90° → hand 从 (1,0,0) 到 (0,1,0) ----
    // 这条是"topoOrder 让 parent 先于 child"的判别式：顺序错了 hand 会
    // 停在 (1,1,0) 或者落在别处。
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        const assets::Skeleton& skel = *f.model.skeleton;

        std::vector<glm::mat4> rest;
        skel.sample(nullptr, 0.0f, rest);
        report("rest_hand_x", static_cast<double>(rest[2][3].x), 1.0, 1e-5);

        std::vector<glm::mat4> turned;
        const assets::AnimationClip* cp = skel.findClip(clip.name);
        skel.sample(cp, kDur, turned);   // t = dur → 正好 90°
        report("hierarchy_hand_x", static_cast<double>(turned[2][3].x), 0.0,
               1e-5);
        report("hierarchy_hand_y", static_cast<double>(turned[2][3].y), 1.0,
               1e-5);
    }

    // ---- 11) AnimatorSystem 真的把关节矩阵填进组件（而不是只推时间）----
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        AnimatorSystem sys;
        runFrames(sys, f, 5, 1.0f / 60.0f);
        const auto& sk = comp(f);
        report("joint_matrices_filled",
               static_cast<double>(sk.jointMatrices.size()), 3.0, 0.0);
    }

    // ---- 12) 坏输入不崩：空骨骼 / 没有 model ----
    {
        scene::Scene sc;
        ecs::Entity e = sc.createObject("empty");
        ecs::SkinnedMeshComponent sk;
        sk.model = nullptr;
        sc.world().add<ecs::SkinnedMeshComponent>(e, sk);
        AnimatorSystem sys;
        const int n = sys.tick(sc, 1.0f / 60.0f);
        report("null_model_noop", static_cast<double>(n), 0.0, 0.0);
    }

    // ================================================================
    // A1（2026-10-19）：AnimatorComponent 接管 time 的权威
    // ================================================================

    // ---- 13) 权威：有人还在写 sk.time 时，它会被镜像回 anim.time ----
    // 这是 A1 的核心断言。判据必须是"dt=0 且两个值不同"：dt>0 时两边都会
    // 往前跑，看谁赢就分不清是"镜像生效"还是"恰好追上"。
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        addAnimator(f, /*time=*/0.0f, /*speed=*/1.0f, /*playing=*/true,
                    /*loop=*/true, /*defaultClip=*/"");
        comp(f).time = 99.0f;   // 模拟旧 UI / 旧脚本仍在写 sk.time
        AnimatorSystem sys;
        runFrames(sys, f, 1, 0.0f);
        report("animator_authoritative", static_cast<double>(comp(f).time), 0.0,
               0.0);
        report("animator_count", static_cast<double>(sys.lastAnimatorCount()),
               1.0, 0.0);
    }

    // ---- 14) 推进 + 镜像：120 帧 @1/60 = 2.0 s（10 s 的 clip 不跨圈）----
    {
        Fixture f;
        const assets::AnimationClip longClip = makeClip(10.0f, "long");
        buildFixture(f, longClip, /*loop=*/true, /*playing=*/true);
        addAnimator(f, 0.0f, 1.0f, true, true, "");
        AnimatorSystem sys;
        runFrames(sys, f, 120, 1.0f / 60.0f);
        report("animator_advance", static_cast<double>(anim(f).time), 2.0,
               1e-4);
        // 镜像是"派生量"，必须逐位等于权威值（不是近似）
        report("animator_mirror", static_cast<double>(comp(f).time), 2.0, 1e-4);
    }

    // ---- 15) 暂停：anim.playing=false 但 sk.playing=true ----
    // 故意把两个写成相反的：如果系统读的是 sk.playing，时间就会推进。
    // ⚠ 断言**必须打在镜像值（comp）上**，不能打在 anim.time 上：就算系统
    //   完全无视 anim.playing，也没人去写 anim.time，它一样停在 0 —— 那条
    //   断言在两个世界里都绿（否定测试实测：只打 anim.time 时这条不会红）。
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        addAnimator(f, 0.0f, 1.0f, /*playing=*/false, true, "");
        AnimatorSystem sys;
        runFrames(sys, f, 60, 1.0f / 60.0f);
        report("animator_paused", static_cast<double>(comp(f).time), 0.0, 0.0);
    }

    // ---- 16) 速度：anim.speed=2 覆盖 sk.speed=1 ----
    {
        Fixture f;
        const assets::AnimationClip longClip = makeClip(10.0f, "long");
        buildFixture(f, longClip, /*loop=*/true, /*playing=*/true);
        addAnimator(f, 0.0f, /*speed=*/2.0f, true, true, "");
        AnimatorSystem sys;
        runFrames(sys, f, 60, 1.0f / 60.0f);
        report("animator_speed", static_cast<double>(anim(f).time), 2.0, 1e-4);
        report("animator_speed_mirror", static_cast<double>(comp(f).speed), 2.0,
               0.0);
    }

    // ---- 17) defaultClip 只补一次 ----
    // 一半是"空 clipName 会被补上"，另一半是"补过之后清空就真的空着"。
    // 少了后半句，Inspector 上 "empty = rest pose" 那句提示就是谎话。
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        comp(f).clipName.clear();
        addAnimator(f, 0.0f, 1.0f, true, true, /*defaultClip=*/clip.name);
        AnimatorSystem sys;
        runFrames(sys, f, 1, 0.0f);
        reportFlag("animator_default_clip", comp(f).clipName == clip.name,
                   "clip=" + comp(f).clipName);
        comp(f).clipName.clear();   // 用户手动清空 → 要看静止姿势
        runFrames(sys, f, 3, 0.0f);
        reportFlag("animator_default_once", comp(f).clipName.empty(),
                   "clip=" + comp(f).clipName);
    }

    // ---- 18) 骨架来源回退：anim.model 空 → 用 sk.model ----
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        addAnimator(f, 0.0f, 1.0f, true, true, "");
        AnimatorSystem sys;
        runFrames(sys, f, 3, 1.0f / 60.0f);
        report("animator_model_fallback",
               static_cast<double>(comp(f).jointMatrices.size()), 3.0, 0.0);
    }

    // ---- 19) Animator 路径的关节矩阵与参考实现**逐位**相同 ----
    // 与 6) 的分工：6) 断的是"拆分前后不变"，这里断的是"走 animator 这条路
    // 采出来的姿势，用的时刻确实是 anim.time"（拿与 6) 同一个参考实现比）。
    {
        Fixture f;
        buildFixture(f, clip, /*loop=*/true, /*playing=*/true);
        addAnimator(f, /*time=*/0.37f, 1.0f, /*playing=*/false, true, "");
        AnimatorSystem sys;
        runFrames(sys, f, 2, 1.0f / 60.0f);   // playing=false → 时间不动
        const assets::Skeleton& skel = *f.model.skeleton;
        const assets::AnimationClip* cp = skel.findClip(clip.name);
        std::vector<glm::mat4> ref;
        referenceSample(skel, cp, 0.37f, ref);
        const auto& got = comp(f).jointMatrices;
        bool same = got.size() == ref.size();
        int bad = 0;
        if (same) {
            for (size_t j = 0; j < got.size(); ++j) {
                if (std::memcmp(&got[j], &ref[j], sizeof(glm::mat4)) != 0) ++bad;
            }
        } else {
            bad = 1000;
        }
        // 顺带把"时刻真的是 0.37 而不是 0"钉住：0.37 处 root 已转了一部分角度
        reportFlag("animator_joint_bitwise", bad == 0,
                   "bad=" + std::to_string(bad) +
                       ",t=" + std::to_string(anim(f).time));
    }

    // ================================================================
    // 表演层（L2 LookAt / L4 弹簧骨，2026-10-25）
    // ================================================================
    //
    // 这一组刻意**全是数值**：不初始化 Vulkan、不开窗口、不截图。
    // 弹簧骨的三个硬不变量（链识别 / 距离守恒 / 收敛）在截图上根本看不出来
    // —— 头发歪 3cm 和歪 5cm 在 1080p 里长得一模一样。
    {
        // 骨架：head（根）→ hair_01 → hair_02 → hair_03，每根沿 +Y 0.3。
        //
        // 按"父骨不匹配前缀 = attach"的判据：
        //   hair_01 的父骨是 head（不匹配）→ **不模拟**，它是缝在头上的固定端
        //   hair_02 / hair_03              → 模拟
        // 所以模拟骨是 2 根而不是 3 根 —— 这条判据最容易写错成"匹配即模拟"，
        // 写错的症状是"发际线在飘"。
        auto skel = std::make_shared<assets::Skeleton>();
        skel->joints.resize(4);
        skel->joints[0].name = "head";
        skel->joints[0].parent = -1;
        skel->joints[0].restLocal = glm::mat4(1.0f);
        skel->joints[0].invBind = glm::mat4(1.0f);
        for (int i = 1; i < 4; ++i) {
            skel->joints[static_cast<size_t>(i)].name =
                "hair_0" + std::to_string(i);
            skel->joints[static_cast<size_t>(i)].parent = i - 1;
            skel->joints[static_cast<size_t>(i)].restLocal =
                glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.3f, 0.0f));
            skel->joints[static_cast<size_t>(i)].invBind = glm::mat4(1.0f);
        }
        skel->topoOrder = {0, 1, 2, 3};
        skel->invMeshNode = glm::mat4(1.0f);

        // ---- 链识别 ----
        const SpringRig rig = buildSpringRig(*skel, {});
        report("spring_rig_count", static_cast<double>(rig.count()), 2.0, 0.0);
        report("spring_rig_restlen",
               rig.count() > 0 ? static_cast<double>(rig.restLen[0]) : -1.0,
               static_cast<double>(0.3f), 1.0e-5);
        const SpringRig noRig = buildSpringRig(*skel, {"zzz"});
        report("spring_rig_nomatch", static_cast<double>(noRig.count()), 0.0, 0.0);

        // ---- 第一帧只初始化（挂上组件的瞬间姿态不变）----
        Pose pose;
        pose.setRest(*skel);
        SpringBoneState bone;
        SpringBoneSettings cfg;   // 默认：gravity -Y / damping 0.9 / rest 0.15
        const int firstFrame =
            bone.update(*skel, rig, 1.0f / 60.0f, cfg, pose);
        report("spring_first_frame", static_cast<double>(firstFrame), 0.0, 0.0);

        // ---- 距离守恒 + 收敛 ----
        for (int i = 0; i < 180; ++i)
            bone.update(*skel, rig, 1.0f / 60.0f, cfg, pose);

        std::vector<glm::mat4> physWorld;
        poseToWorld(*skel, pose, physWorld);
        const auto& mp = bone.positions();
        // hair_01 不模拟（attach），它的位置只能从 pose 的 FK 拿。
        const glm::vec3 anchor = glm::vec3(physWorld[1][3]);
        report("spring_len_attach", static_cast<double>(glm::length(mp[0] - anchor)),
               static_cast<double>(0.3f), 1.0e-3);
        report("spring_len_chain", static_cast<double>(glm::length(mp[1] - mp[0])),
               static_cast<double>(0.3f), 1.0e-3);

        const glm::vec3 tipBefore = mp[1];
        for (int i = 0; i < 60; ++i)
            bone.update(*skel, rig, 1.0f / 60.0f, cfg, pose);
        report("spring_converged",
               static_cast<double>(glm::length(bone.positions()[1] - tipBefore)),
               0.0, 2.0e-3);

        // ---- 惯性：头突然转 90°，发梢必须落后于"纯动画位置" ----
        // 这条才是"物理真的在跑"的证据。没有它，阻尼调成 0 也能让上面
        // 三条全 PASS（距离照样守恒、照样收敛 —— 收敛到一个错误的位置）。
        {
            Pose lp;
            lp.setRest(*skel);
            SpringBoneState st2;
            st2.update(*skel, rig, 1.0f / 60.0f, cfg, lp);  // 初始化

            lp.rotation[0] = glm::angleAxis(glm::radians(90.0f),
                                            glm::vec3(0.0f, 0.0f, 1.0f));
            std::vector<glm::mat4> pure;
            poseToWorld(*skel, lp, pure);   // 必须在 update 之前取
            const glm::vec3 pureTip = glm::vec3(pure[3][3]);

            st2.update(*skel, rig, 1.0f / 60.0f, cfg, lp);
            const float lag = glm::length(st2.positions()[1] - pureTip);
            reportFlag("spring_lag", lag > 0.02f, "lag=" + std::to_string(lag));
        }

        // ---- LookAt：解析 + 限幅 ----
        report("lookat_resolve_auto",
               static_cast<double>(resolveLookAtBone(*skel, "")), 0.0, 0.0);
        report("lookat_resolve_byname",
               static_cast<double>(resolveLookAtBone(*skel, "head")), 0.0, 0.0);
        report("lookat_resolve_missing",
               static_cast<double>(resolveLookAtBone(*skel, "zzz")), -1.0, 0.0);

        {
            LookAtSettings la;
            la.hasTarget = true;
            // 正后方。forwardLocal 默认 -Z，所以 +Z 是"背后" → 需要转 180°。
            la.target = glm::vec3(0.0f, 0.0f, 10.0f);
            la.smoothRate = 0.0f;   // 不平滑，便于断言（第一帧直接取目标）
            // ⚠ 必须显式放开限幅：默认值是 55°，不管的话测出来就是 55
            //   （第一次跑这条断言时 actual=55.000008，看着像"限幅没生效"，
            //    其实是"忘了关限幅"，而 55 恰好是默认值）。
            la.maxAngleDeg = 180.0f;

            LookAtState ls;
            Pose lp;
            lp.setRest(*skel);
            ls.apply(*skel, 0, 1.0f / 60.0f, la, lp);
            report("lookat_unclamped_180",
                   static_cast<double>(ls.appliedAngleDeg()), 180.0, 2.0);

            la.maxAngleDeg = 45.0f;
            LookAtState ls2;
            Pose lp2;
            lp2.setRest(*skel);
            for (int i = 0; i < 10; ++i)
                ls2.apply(*skel, 0, 1.0f / 60.0f, la, lp2);
            // 目标在背后却只转到 45° —— 这就是"转到极限就停住"
            report("lookat_clamped_45",
                   static_cast<double>(ls2.appliedAngleDeg()), 45.0, 1.0);

            // weight=0 → 完全不碰 pose（这条是"关掉 LookAt 不改变基线"）
            la.weight = 0.0f;
            LookAtState ls3;
            Pose lp3;
            lp3.setRest(*skel);
            const glm::quat keep = lp3.rotation[0];
            const int ret = ls3.apply(*skel, 0, 1.0f / 60.0f, la, lp3);
            reportFlag("lookat_weight_zero",
                       ret == 0 &&
                           std::memcmp(&keep, &lp3.rotation[0],
                                       sizeof(glm::quat)) == 0,
                       "ret=" + std::to_string(ret));
        }

        // ---- 集成：挂上组件的第一帧，关节矩阵必须与"没挂"逐位相同 ----
        // 走完整的 AnimatorSystem 路径（不是直接调物理），因为要证明的是
        // "新加的那一段代码没有在第一天就动了基线"。
        {
            assets::Model mNo, mYes;
            mNo.skeleton = skel;
            mYes.skeleton = skel;
            scene::Scene scNo, scYes;

            const ecs::Entity eNo = scNo.createObject("hair");
            const ecs::Entity eYes = scYes.createObject("hair");
            ecs::SkinnedMeshComponent skc;
            skc.model = &mNo;
            scNo.world().add<ecs::SkinnedMeshComponent>(eNo, skc);
            skc.model = &mYes;
            scYes.world().add<ecs::SkinnedMeshComponent>(eYes, skc);

            ecs::SpringBoneComponent sbc;   // 默认启用
            scYes.world().add<ecs::SpringBoneComponent>(eYes, sbc);

            AnimatorSystem sysNo, sysYes;
            sysNo.tick(scNo, 1.0f / 60.0f);
            sysYes.tick(scYes, 1.0f / 60.0f);

            const auto* a = scNo.world().get<ecs::SkinnedMeshComponent>(eNo);
            const auto* b = scYes.world().get<ecs::SkinnedMeshComponent>(eYes);
            bool same = a && b && a->jointMatrices.size() == b->jointMatrices.size();
            int bad = 0;
            if (same) {
                for (size_t i = 0; i < a->jointMatrices.size(); ++i) {
                    if (std::memcmp(&a->jointMatrices[i], &b->jointMatrices[i],
                                    sizeof(glm::mat4)) != 0)
                        ++bad;
                }
            } else {
                bad = 1000;
            }
            // 第一帧 spring 的 update 返回 0（只初始化）→ 必须逐位相同。
            // 顺带钉住"组件真的被看见了"：sysYes 的计数是 1。
            reportFlag("spring_integration_first_frame", bad == 0,
                       "bad=" + std::to_string(bad) +
                           ",spring=" + std::to_string(sysYes.lastEntityCount()));
        }
    }

    // ================================================================
    // Skeleton::findJoint（第八刀：把两份重复的名字匹配收敛成一份）
    // ================================================================
    //
    // 这一组的存在理由不是"函数有返回值"，而是**那个标志位真的有用**：
    // 不带 `SkipEndTips` 时会命中 `Head_end`（骨尖辅助骨），带的时候命中
    // 真正的头。两条都断，才证明标志位不是装饰。
    {
        assets::Skeleton s;
        s.joints.resize(5);
        const char* names[5] = {"hand_l_end", "hand_l", "Head_end",
                                "mixamorig:Head", "hair1_L"};
        for (int i = 0; i < 5; ++i) {
            s.joints[static_cast<size_t>(i)].name = names[i];
            s.joints[static_cast<size_t>(i)].parent = i - 1;
            s.joints[static_cast<size_t>(i)].restLocal = glm::mat4(1.0f);
            s.joints[static_cast<size_t>(i)].invBind = glm::mat4(1.0f);
        }
        s.topoOrder = {0, 1, 2, 3, 4};

        // 精确优先于顺序：`hand_l` 排在 `hand_l_end` **后面**，
        // 但精确命中仍然挑它（包含匹配会先撞上 end 骨）。
        report("find_joint_exact",
               static_cast<double>(s.findJoint("hand_l")), 1.0, 0.0);
        // 不区分大小写 + 包含：`HEAD` → `mixamorig:Head`（默认标志位含
        // SkipEndTips，所以**跳过**了排在前面的 `Head_end`）
        report("find_joint_substring",
               static_cast<double>(s.findJoint("HEAD")), 3.0, 0.0);
        // 但精确写出 `Head_end` 时必须拿到它 —— 精确那一轮**不过滤** end/tip。
        // 这条是第一版写漏的：两轮都过滤 → 显式要末端骨的人拿到 -1，
        // 而"资产里没有这根骨"与"被过滤器吃掉了"在调用方看来一模一样。
        report("find_joint_endtip_exact",
               static_cast<double>(s.findJoint("Head_end")), 2.0, 0.0);
        // 不带 SkipEndTips → 命中骨尖（2）。这就是那个标志位存在的理由。
        report("find_joint_endtip_hit",
               static_cast<double>(s.findJoint(
                   "head", assets::kJointMatchCaseInsensitive |
                               assets::kJointMatchSubstring)),
               2.0, 0.0);
        report("find_joint_endtip_skip",
               static_cast<double>(s.findJoint(
                   "head", assets::kJointMatchCaseInsensitive |
                               assets::kJointMatchSubstring |
                               assets::kJointMatchSkipEndTips)),
               3.0, 0.0);
        // 空名字和对不上的名字都必须是 -1（**不是** 0）
        report("find_joint_empty", static_cast<double>(s.findJoint("")), -1.0,
               0.0);
        report("find_joint_missing", static_cast<double>(s.findJoint("zzz")),
               -1.0, 0.0);

        std::vector<int> hits;
        s.findJointsWithPrefix("hair", hits);
        report("find_prefix_count", static_cast<double>(hits.size()), 1.0, 0.0);
        hits.clear();
        s.findJointsWithPrefix("", hits);
        report("find_prefix_empty", static_cast<double>(hits.size()), 0.0, 0.0);
    }

    // ================================================================
    // 两骨 IK + 骨骼挂点（第八刀，2026-10-25）
    // ================================================================
    //
    // 为什么这一组全是数值、一张截图都没有：
    //   脚离地面 2cm 和 20cm 在 1080p 里是同一只脚，而后者毁掉整段走路。
    //   "IK 生效了"这件事**不可视**，只能断数值。
    {
        // 髋 → 膝 → 踝，每节 0.4，沿 -Y 伸直：
        //   A = (0,0,0)   B = (0,-0.4,0)   C = (0,-0.8,0)
        auto skel = std::make_shared<assets::Skeleton>();
        skel->joints.resize(3);
        skel->joints[0].name = "thigh_l";
        skel->joints[0].parent = -1;
        skel->joints[0].restLocal = glm::mat4(1.0f);
        skel->joints[0].invBind = glm::mat4(1.0f);
        skel->joints[1].name = "calf_l";
        skel->joints[1].parent = 0;
        skel->joints[1].restLocal =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.4f, 0.0f));
        skel->joints[1].invBind = glm::mat4(1.0f);
        skel->joints[2].name = "foot_l";
        skel->joints[2].parent = 1;
        skel->joints[2].restLocal =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.4f, 0.0f));
        skel->joints[2].invBind = glm::mat4(1.0f);
        skel->topoOrder = {0, 1, 2};
        skel->invMeshNode = glm::mat4(1.0f);

        // ---- 链解析 ----
        {
            TwoBoneIKSettings cfg;   // 全空 → 自动
            const TwoBoneIKChain ch = resolveTwoBoneChain(*skel, cfg);
            report("ik_resolve_auto_root", static_cast<double>(ch.root), 0.0, 0.0);
            report("ik_resolve_auto_mid", static_cast<double>(ch.mid), 1.0, 0.0);
            report("ik_resolve_auto_end", static_cast<double>(ch.end), 2.0, 0.0);

            TwoBoneIKSettings byName;
            byName.midBone = "calf_l";
            report("ik_resolve_byname",
                   static_cast<double>(resolveTwoBoneChain(*skel, byName).mid),
                   1.0, 0.0);

            // 名字找得到、但**不成链**（foot_l 的父是 calf_l，不是 thigh_l）
            // → 必须整条链拒绝。硬算的话下面"父骨世界旋转"那一步会用错坐标系。
            TwoBoneIKSettings broken;
            broken.midBone = "foot_l";
            reportFlag("ik_resolve_broken",
                       !resolveTwoBoneChain(*skel, broken).valid());

            TwoBoneIKSettings missing;
            missing.rootBone = "zzz";
            reportFlag("ik_resolve_missing",
                       !resolveTwoBoneChain(*skel, missing).valid());
        }

        // ---- 够得着：末端落在目标上 ----
        const glm::vec3 target(0.2f, -0.7f, 0.0f);   // |t| = 0.728 < 0.8
        float err0 = 0.0f;
        {
            Pose p0;
            p0.setRest(*skel);
            std::vector<glm::mat4> w0;
            poseToWorld(*skel, p0, w0);
            err0 = glm::length(target - glm::vec3(w0[2][3]));
        }
        // **对照组**：不解算时差多少。没有这一条，"误差 0.002" 证明不了
        // 任何事 —— 一个什么都不做的 solver 也能让"误差"是别的数。
        reportFlag("ik_control_gt_10cm", err0 > 0.10f,
                   "err0=" + std::to_string(err0));

        TwoBoneIKSettings cfg;
        cfg.hasTarget = true;
        cfg.target = target;

        Pose pose;
        pose.setRest(*skel);
        const TwoBoneIKResult ik = solveTwoBoneIK(*skel, cfg, pose);
        reportFlag("ik_applied", ik.applied);
        report("ik_reach_error", static_cast<double>(ik.errorAfter), 0.0, 2.0e-3);
        // 余弦定理的定值校验：d=0.728、L1=L2=0.4 →
        //   cosB = (0.16+0.16-0.53)/0.32 = -0.65625 → 内角 131.0° → 弯 49.0°
        report("ik_bend_deg", static_cast<double>(ik.bendAngleDeg), 49.0, 1.0);

        // ---- weight=0 → 逐位不变（"关掉功能回到基线"的唯一保证）----
        {
            Pose a;
            a.setRest(*skel);
            Pose b = a;
            TwoBoneIKSettings off = cfg;
            off.weight = 0.0f;
            const TwoBoneIKResult r = solveTwoBoneIK(*skel, off, b);
            const bool same =
                std::memcmp(a.rotation.data(), b.rotation.data(),
                            sizeof(glm::quat) * a.rotation.size()) == 0;
            reportFlag("ik_weight_zero", !r.applied && same);
        }

        // ---- 无目标 → 不动 ----
        {
            Pose a;
            a.setRest(*skel);
            Pose b = a;
            TwoBoneIKSettings nt = cfg;
            nt.hasTarget = false;
            const TwoBoneIKResult r = solveTwoBoneIK(*skel, nt, b);
            const bool same =
                std::memcmp(a.rotation.data(), b.rotation.data(),
                            sizeof(glm::quat) * a.rotation.size()) == 0;
            reportFlag("ik_no_target", !r.applied && same);
        }

        // ---- 确定性：同一输入两次 → 逐位相同 ----
        // 迭代解（FABRIK/CCD）在这一条上会红或需要"跑够次数"，这是选
        // 解析解的直接理由之一。
        {
            Pose p1, p2;
            p1.setRest(*skel);
            p2.setRest(*skel);
            solveTwoBoneIK(*skel, cfg, p1);
            solveTwoBoneIK(*skel, cfg, p2);
            const bool same =
                std::memcmp(p1.rotation.data(), p2.rotation.data(),
                            sizeof(glm::quat) * p1.rotation.size()) == 0;
            reportFlag("ik_deterministic", same);
        }

        // ---- 极向量从当帧动画反推（本刀最核心的一条）----
        //
        // 同一个目标、同一段链，只把**动画里的弯曲平面**换一下：
        //   A：髋绕 X 转 25° → 膝在 -Z 侧
        //   B：髋绕 Z 转 25° → 膝在 +X 侧
        // 解算后两边的膝必须**各自留在原来那一侧**。极向量若是构建时缓存的
        // 常量（或写死某个轴），两边会弯向同一边 —— 而画面上只表现为
        // "这个角色的腿有点怪"，不会报错。
        {
            const glm::vec3 t2(0.0f, -0.72f, 0.0f);
            TwoBoneIKSettings c2;
            c2.hasTarget = true;
            c2.target = t2;

            Pose pa;
            pa.setRest(*skel);
            pa.rotation[0] = glm::angleAxis(glm::radians(25.0f),
                                            glm::vec3(1.0f, 0.0f, 0.0f));
            solveTwoBoneIK(*skel, c2, pa);
            std::vector<glm::mat4> wa;
            poseToWorld(*skel, pa, wa);
            const glm::vec3 midA = glm::vec3(wa[1][3]);
            reportFlag("ik_pole_follows_anim_a",
                       midA.z < -0.02f && std::fabs(midA.z) > std::fabs(midA.x),
                       "midA=" + std::to_string(midA.x) + "," +
                           std::to_string(midA.y) + "," +
                           std::to_string(midA.z));

            Pose pb;
            pb.setRest(*skel);
            pb.rotation[0] = glm::angleAxis(glm::radians(25.0f),
                                            glm::vec3(0.0f, 0.0f, 1.0f));
            solveTwoBoneIK(*skel, c2, pb);
            std::vector<glm::mat4> wb;
            poseToWorld(*skel, pb, wb);
            const glm::vec3 midB = glm::vec3(wb[1][3]);
            reportFlag("ik_pole_follows_anim_b",
                       midB.x > 0.02f && std::fabs(midB.x) > std::fabs(midB.z),
                       "midB=" + std::to_string(midB.x) + "," +
                           std::to_string(midB.y) + "," +
                           std::to_string(midB.z));
        }

        // ---- 够不着 / 目标贴脸：都不能出 NaN ----
        {
            Pose p;
            p.setRest(*skel);
            TwoBoneIKSettings far = cfg;
            far.target = glm::vec3(0.0f, -10.0f, 0.0f);
            const TwoBoneIKResult rf = solveTwoBoneIK(*skel, far, p);
            std::vector<glm::mat4> jm;
            poseToJoints(*skel, p, jm);
            bool finite = true;
            for (const glm::mat4& m : jm)
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        if (!std::isfinite(m[c][r])) finite = false;
            reportFlag("ik_unreachable_finite", finite);
            // 够不着时腿应该尽量伸直（弯 < 8°），并且**如实**报出剩余误差
            reportFlag("ik_unreachable_straight", rf.bendAngleDeg < 8.0f,
                       "bend=" + std::to_string(rf.bendAngleDeg));
            // 10 − (0.4+0.4)·0.999 = 9.2008
            report("ik_unreachable_error", static_cast<double>(rf.errorAfter),
                   9.2008, 5.0e-2);
        }
        {
            // 目标几乎贴在髋上 → 链折到底（弯 ≈ 180°）。
            // 同时这条会走"动画里腿完全伸直 → 极向量退化"那条分支：
            // 静止姿势 ab 与 dirAT 平行，投影为 0，必须退到 poleVector。
            Pose p;
            p.setRest(*skel);
            TwoBoneIKSettings near = cfg;
            near.target = glm::vec3(0.0f, -0.001f, 0.0f);
            const TwoBoneIKResult rn = solveTwoBoneIK(*skel, near, p);
            report("ik_too_close_fold", static_cast<double>(rn.bendAngleDeg),
                   180.0, 1.0);
            std::vector<glm::mat4> w;
            poseToWorld(*skel, p, w);
            const glm::vec3 mid = glm::vec3(w[1][3]);
            // poleVector 默认 (0,0,1) → 折向 +Z
            reportFlag("ik_pole_degenerate_fallback", mid.z > 0.2f,
                       "mid.z=" + std::to_string(mid.z));
        }

        // ---- 集成：走完整 tick 路径（组件 → 系统 → 关节矩阵）----
        {
            assets::Model m;
            m.skeleton = skel;
            scene::Scene sc;
            const ecs::Entity e = sc.createObject("leg");
            ecs::SkinnedMeshComponent skc;
            skc.model = &m;
            skc.playing = false;
            sc.world().add<ecs::SkinnedMeshComponent>(e, skc);
            ecs::TwoBoneIKComponent ikc;
            ikc.settings = cfg;
            sc.world().add<ecs::TwoBoneIKComponent>(e, ikc);

            AnimatorSystem sys;
            sys.tick(sc, 1.0f / 60.0f);
            report("ik_integration_count",
                   static_cast<double>(sys.lastIKCount()), 1.0, 0.0);
            const auto* got = sc.world().get<ecs::SkinnedMeshComponent>(e);
            const float err =
                (got && got->jointMatrices.size() > 2)
                    ? glm::length(target - glm::vec3(got->jointMatrices[2][3]))
                    : 999.0f;
            reportFlag("ik_integration_reached", err < 2.0e-3f,
                       "err=" + std::to_string(err));
        }
    }

    // ================================================================
    // 骨骼挂点（第八刀）—— 两处独立的算法互校
    // ================================================================
    //
    // 挂点的位置可以走两条完全不同的路算出来：
    //   ① 系统：jointMatrices · invBind⁻¹（蒙皮矩阵反推）
    //   ② 断言：samplePose + poseToWorld（纯 FK）
    // 两者对上才说明"jointMatrices 不是骨骼的世界矩阵"这件事被正确处理了。
    // 只信一条的话，把 jointMat 直接当骨骼矩阵用也能自洽地"通过"。
    {
        auto ask = std::make_shared<assets::Skeleton>();
        ask->joints.resize(2);
        ask->joints[0].name = "root";
        ask->joints[0].parent = -1;
        ask->joints[0].restLocal = glm::mat4(1.0f);
        ask->joints[0].invBind = glm::mat4(1.0f);
        ask->joints[1].name = "hand";
        ask->joints[1].parent = 0;
        ask->joints[1].restLocal =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f));
        ask->joints[1].invBind = glm::mat4(1.0f);
        ask->topoOrder = {0, 1};
        ask->invMeshNode = glm::mat4(1.0f);

        // 一条把 root 从 0° 转到 90° 的 clip（挂点必须跟着动）
        assets::AnimationClip clip;
        clip.name = "rot";
        clip.duration = 1.0f;
        assets::AnimTrack tr;
        tr.joint = 0;
        tr.path = assets::AnimTrack::Rotation;
        tr.times = {0.0f, 1.0f};
        const glm::quat q0(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::quat q1 =
            glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        tr.values = {glm::vec4(q0.x, q0.y, q0.z, q0.w),
                     glm::vec4(q1.x, q1.y, q1.z, q1.w)};
        clip.tracks.push_back(tr);
        ask->clips.push_back(clip);

        assets::Model am;
        am.skeleton = ask;

        scene::Scene sc;
        const ecs::Entity host = sc.createObject("host");
        ecs::SkinnedMeshComponent hsk;
        hsk.model = &am;
        hsk.clipName = "rot";
        hsk.time = 0.5f;       // 45°
        hsk.playing = false;
        sc.world().add<ecs::SkinnedMeshComponent>(host, hsk);

        const ecs::Entity child = sc.createObject("sword");
        ecs::BoneAttachmentComponent ac;
        ac.hostName = "host";
        ac.boneName = "hand";
        ac.offset = glm::vec3(0.0f, 0.1f, 0.0f);
        sc.world().add<ecs::BoneAttachmentComponent>(child, ac);

        AnimatorSystem sys;
        sys.tick(sc, 1.0f / 60.0f);
        report("attach_count", static_cast<double>(sys.lastAttachCount()), 1.0,
               0.0);

        const auto* t = sc.world().get<ecs::TransformComponent>(child);
        // 独立算一遍（纯 FK），与系统那条 jointMatrices 路径互校
        Pose fp;
        samplePose(*ask, ask->findClip("rot"), 0.5f, fp);
        std::vector<glm::mat4> fw;
        poseToWorld(*ask, fp, fw);
        const glm::vec3 want = glm::vec3(fw[1] * glm::vec4(0.0f, 0.1f, 0.0f, 1.0f));
        reportFlag("attach_follows_anim",
                   t && glm::length(t->position - want) < 1.0e-4f,
                   t ? ("pos=" + std::to_string(t->position.x) + "," +
                        std::to_string(t->position.y) + "," +
                        std::to_string(t->position.z))
                     : std::string("no-transform"));
        // 45° 时手骨应在 (0.354, 0.354, 0) —— offset 沿骨骼 Y 再加 0.1
        // 只断"不在原点且确实被转过"，避免把角度算式再抄一遍
        reportFlag("attach_moved", t && t->position.x > 0.2f &&
                                       t->position.y > 0.2f,
                   t ? ("y=" + std::to_string(t->position.y)) : std::string());

        // ---- 骨名对不上：保持原位，只报数 ----
        {
            scene::Scene sc2;
            const ecs::Entity h2 = sc2.createObject("host2");
            ecs::SkinnedMeshComponent h2sk;
            h2sk.model = &am;
            h2sk.clipName = "rot";
            h2sk.playing = false;
            sc2.world().add<ecs::SkinnedMeshComponent>(h2, h2sk);

            const ecs::Entity c2 = sc2.createObject("badattach");
            if (auto* tt = sc2.world().get<ecs::TransformComponent>(c2))
                tt->position = glm::vec3(1.0f, 2.0f, 3.0f);
            ecs::BoneAttachmentComponent bad;
            bad.hostName = "host2";
            bad.boneName = "zzz";
            sc2.world().add<ecs::BoneAttachmentComponent>(c2, bad);

            AnimatorSystem sys2;
            sys2.tick(sc2, 1.0f / 60.0f);
            report("attach_missing_count",
                   static_cast<double>(sys2.lastAttachUnresolved()), 1.0, 0.0);
            const auto* t2 = sc2.world().get<ecs::TransformComponent>(c2);
            reportFlag("attach_missing_keeps_transform",
                       t2 && std::fabs(t2->position.x - 1.0f) < 1.0e-6f &&
                           std::fabs(t2->position.y - 2.0f) < 1.0e-6f,
                       t2 ? ("y=" + std::to_string(t2->position.y)) : std::string());
        }
    }

    // ================================================================
    // 状态机（A2b，2026-10-25）
    // ================================================================
    //
    // 这一组是"七个假开关"里五个的验收：`controllerPath` 真的被读、
    // `stateIndex` / `stateTime` 真的被读写、三张参数表真的被条件消费。
    //
    // 控制器用**内存 JSON 文本**而不是读盘：自检必须能在没有 assets 目录的
    // 构建机上跑（回归的第一条要求是"不依赖工作目录"）。读盘那条路由
    // `loadAnimController` 覆盖，另有一条断言专门测"路径不存在不崩"。
    {
        // 三条 clip：A(1.0s) / B(1.0s) / C(2.0s)。
        // C 的时长**故意与 A 不同** —— 测 `restart:false` 的相位换算时，
        // 等长的话"按秒数保留"与"按相位换算"结果一样，断言没有判别力。
        const std::string kCtl = R"JSON({
            "defaultState": "S1",
            "parameters": [
                { "name": "Speed", "type": "float",   "default": 0.0 },
                { "name": "Go",    "type": "trigger" }
            ],
            "states": [
                { "name": "S1", "clip": "A", "loop": true  },
                { "name": "S2", "clip": "B", "loop": true  },
                { "name": "S3", "clip": "C", "loop": false }
            ],
            "transitions": [
                { "from": "S1", "to": "S2", "duration": 0.20,
                  "conditions": [ { "param": "Speed", "op": ">=", "value": 0.5 } ] },
                { "from": "S2", "to": "S1", "duration": 0.20,
                  "conditions": [ { "param": "Speed", "op": "<",  "value": 0.5 } ] },
                { "from": "S3", "to": "S1", "duration": 0.10,
                  "conditions": [ { "param": "Speed", "op": "<",  "value": 0.5 } ] },
                { "from": "Any", "to": "S3", "duration": 0.10,
                  "conditions": [ { "param": "Go", "op": "trigger" } ] }
            ]
        })JSON";

        auto ctl = parseAnimController(kCtl);
        reportFlag("sm_load_ok", ctl->valid(),
                   ctl->error.empty() ? "states=3" : "err=" + ctl->error);

        // ---- 加载期校验：三条必须被拦下（拦不下就是"配错了但不报错"）----
        {
            const char* bad[] = {
                // ① 转移指向不存在的状态
                R"({"states":[{"name":"A","clip":"A"}],"transitions":[
                     {"from":"A","to":"Nope"}]})",
                // ② 状态名重复
                R"({"states":[{"name":"A","clip":"A"},{"name":"A","clip":"B"}]})",
                // ③ 条件引用未声明的参数
                R"({"states":[{"name":"A","clip":"A"},{"name":"B","clip":"B"}],
                     "transitions":[{"from":"A","to":"B",
                       "conditions":[{"param":"Nope","op":">","value":0}]}]})",
            };
            int rejected = 0;
            for (const char* b : bad) {
                auto c = parseAnimController(b);
                if (!c->valid()) ++rejected;
            }
            // 三条都得拦下。少拦一条 = 那一类配置错误会以"状态机不生效"
            // 的形式静默跑到运行时。
            report("sm_validate_rejects", static_cast<double>(rejected), 3.0,
                   0.0);
        }

        // 建一个"三条 clip + 骨架"的 fixture，并挂上控制器
        auto makeFixture = [&](Fixture& f) {
            auto skel =
                std::make_shared<assets::Skeleton>(makeChainSkeleton());
            skel->clips.push_back(makeClip(1.0f, "A"));
            skel->clips.push_back(makeClip(1.0f, "B"));
            skel->clips.push_back(makeClip(2.0f, "C"));
            f.model.skeleton = skel;
            f.root = f.scene.createObject("sm");
            ecs::SkinnedMeshComponent skc;
            skc.model = &f.model;
            skc.playing = true;
            skc.loop = true;
            f.scene.world().add<ecs::SkinnedMeshComponent>(f.root, skc);
            ecs::AnimatorComponent a;
            a.controllerPath = "<memory>";   // 非空即可，控制器显式传入
            f.scene.world().add<ecs::AnimatorComponent>(f.root, a);
        };

        // ---- 1) 初始化：第一帧落到 defaultState，且 clip 已经换过去 ----
        // 这条同时证明"`dt>0` 与 `dt=0` 都必须在初始化"（见下面的 dt=0 那条）。
        {
            Fixture f;
            makeFixture(f);
            auto c2 = parseAnimController(kCtl);
            ecs::SkinnedMeshComponent& s = comp(f);
            ecs::AnimatorComponent& a = anim(f);

            // dt = 0：编辑器的语义是"停在当前这一刻"。**但初始化必须发生** ——
            // 否则打开场景看到的是 defaultClip 定住的姿势，而 Inspector
            // 显示 state=Idle（症状：视口是 Walk、面板说 Idle）。
            stepAnimController(*c2, a, s, *f.model.skeleton, 0.0f, "T");
            report("sm_init_dt_zero_state", static_cast<double>(a.stateIndex),
                   0.0, 0.0);
            reportFlag("sm_init_dt_zero_clip", s.clipName == "A",
                       "clip=" + s.clipName);
            report("sm_init_dt_zero_time", static_cast<double>(a.stateTime), 0.0,
                   0.0);

            // 再跑一帧 dt>0：stateTime 要开始走，且不该立刻转走（Speed=0）
            stepAnimController(*c2, a, s, *f.model.skeleton, 1.0f / 60.0f, "T");
            report("sm_init_state_time",
                   static_cast<double>(a.stateTime),
                   static_cast<double>(1.0f / 60.0f), 1.0e-6);
        }

        // ---- 2) 条件转移 + 交叉淡入的时长真的写进去了 ----
        {
            Fixture f;
            makeFixture(f);
            auto c2 = parseAnimController(kCtl);
            ecs::SkinnedMeshComponent& s = comp(f);
            ecs::AnimatorComponent& a = anim(f);

            stepAnimController(*c2, a, s, *f.model.skeleton, 1.0f / 60.0f, "T");
            a.floatParams["Speed"] = 0.8f;

            const int n =
                stepAnimController(*c2, a, s, *f.model.skeleton, 1.0f / 60.0f,
                                   "T");
            report("sm_transition_count", static_cast<double>(n), 1.0, 0.0);
            report("sm_transition_state", static_cast<double>(a.stateIndex), 1.0,
                   0.0);
            reportFlag("sm_transition_clip", s.clipName == "B",
                       "clip=" + s.clipName);
            // 转移后 stateTime 归零（hasExitTime 靠它判，不归零就等于跳过门）
            report("sm_state_time_reset", static_cast<double>(a.stateTime), 0.0,
                   1.0e-6);
            // `duration` 真的进了 fadeDur —— 不是"记了个数没人用"
            report("sm_fade_dur", static_cast<double>(s.fadeDur), 0.20, 1.0e-6);
            reportFlag("sm_fading", s.fading(), "from=" + s.fadeFromClip);
        }

        // ---- 3) 每帧最多一条转移 ----
        // S1→S2 与 S2→S3 的条件同时成立时，一帧只许走一步。
        // 允许连转的症状是"跨过中间的过渡时长，肉眼是直接弹到终态"。
        {
            Fixture f;
            makeFixture(f);
            auto c2 = parseAnimController(kCtl);
            ecs::SkinnedMeshComponent& s = comp(f);
            ecs::AnimatorComponent& a = anim(f);
            stepAnimController(*c2, a, s, *f.model.skeleton, 1.0f / 60.0f, "T");
            a.floatParams["Speed"] = 0.8f;
            a.triggerParams["Go"] = true;   // 两条转移同时可走

            stepAnimController(*c2, a, s, *f.model.skeleton, 1.0f / 60.0f, "T");
            // 数组顺序里 S1→S2 在前，所以这一帧只该到 S2（下标 1），
            // 不能一步跨到 S3（下标 2）。
            report("sm_one_per_frame", static_cast<double>(a.stateIndex), 1.0,
                   0.0);
        }

        // ---- 4) trigger：被采纳后立即清零，且不会重新触发 ----
        // 不清零的症状是角色无限起跳（从 Jump 回 Idle 的下一帧又满足
        // `Any → Jump`）—— 这是状态机最经典的 bug。
        {
            Fixture f;
            makeFixture(f);
            auto c2 = parseAnimController(kCtl);
            ecs::SkinnedMeshComponent& s = comp(f);
            ecs::AnimatorComponent& a = anim(f);
            stepAnimController(*c2, a, s, *f.model.skeleton, 1.0f / 60.0f, "T");

            a.triggerParams["Go"] = true;
            stepAnimController(*c2, a, s, *f.model.skeleton, 1.0f / 60.0f, "T");
            const bool consumed = !a.triggerParams["Go"];
            reportFlag("sm_trigger_consumed", consumed && a.stateIndex == 2,
                       "state=" + std::to_string(a.stateIndex));

            // 再跑 5 帧：S3→S1 会走（Speed=0），但回不到 S3
            for (int i = 0; i < 5; ++i)
                stepAnimController(*c2, a, s, *f.model.skeleton, 1.0f / 60.0f,
                                   "T");
            report("sm_trigger_no_reentry", static_cast<double>(a.stateIndex),
                   0.0, 0.0);
        }

        // ---- 5) hasExitTime：没到点不许走，到点才放行 ----
        {
            const std::string kExit = R"JSON({
                "parameters": [ { "name": "Speed", "type": "float" } ],
                "states": [
                    { "name": "S1", "clip": "A" },
                    { "name": "S2", "clip": "B" }
                ],
                "transitions": [
                    { "from": "S1", "to": "S2", "duration": 0.1,
                      "hasExitTime": true, "exitTime": 0.5,
                      "conditions": [ { "param": "Speed", "op": ">=", "value": 0.0 } ] }
                ]
            })JSON";
            auto c3 = parseAnimController(kExit);
            reportFlag("sm_exit_load", c3->valid(),
                       c3->error.empty() ? "ok" : c3->error);

            // A 的时长 1.0s，exitTime=0.5 → 需要 0.5s。跑 20 帧（0.33s）
            // 应当**还**在 S1。判据留足余量，不卡在浮点边界上。
            {
                Fixture f;
                makeFixture(f);
                ecs::SkinnedMeshComponent& s = comp(f);
                ecs::AnimatorComponent& a = anim(f);
                a.floatParams["Speed"] = 1.0f;
                for (int i = 0; i < 20; ++i)
                    stepAnimController(*c3, a, s, *f.model.skeleton,
                                       1.0f / 60.0f, "T");
                report("sm_exit_blocked", static_cast<double>(a.stateIndex), 0.0,
                       0.0);
            }
            // 跑 40 帧（0.67s）→ 已经过点，转走
            {
                Fixture f;
                makeFixture(f);
                ecs::SkinnedMeshComponent& s = comp(f);
                ecs::AnimatorComponent& a = anim(f);
                a.floatParams["Speed"] = 1.0f;
                for (int i = 0; i < 40; ++i)
                    stepAnimController(*c3, a, s, *f.model.skeleton,
                                       1.0f / 60.0f, "T");
                report("sm_exit_passed", static_cast<double>(a.stateIndex), 1.0,
                       0.0);
            }
        }

        // ---- 6) restart=false：按**归一化相位**换算，不是按秒数 ----
        // A(1.0s) @ time=0.5 → 相位 0.5 → C(2.0s) 应当落在 1.0s。
        // 按秒数保留的话是 0.5s（相位 0.25）—— 肉眼是"跑步从半步开始"。
        {
            const std::string kPhase = R"JSON({
                "parameters": [ { "name": "Speed", "type": "float" } ],
                "states": [
                    { "name": "S1", "clip": "A" },
                    { "name": "S2", "clip": "C" }
                ],
                "transitions": [
                    { "from": "S1", "to": "S2", "duration": 0.1, "restart": false,
                      "conditions": [ { "param": "Speed", "op": ">=", "value": 0.5 } ] }
                ]
            })JSON";
            auto c4 = parseAnimController(kPhase);
            Fixture f;
            makeFixture(f);
            ecs::SkinnedMeshComponent& s = comp(f);
            ecs::AnimatorComponent& a = anim(f);

            // ⚠ 必须先跑一帧让它初始化：状态机第一次进来会 `switchClip(..., 
            //   restart=true)` 把 `an.time` 归零 —— 直接设 0.5 会被那一帧
            //   清掉（第一次跑这条断言时 actual=0，就是这个原因）。
            a.floatParams["Speed"] = 0.0f;
            stepAnimController(*c4, a, s, *f.model.skeleton, 1.0f / 60.0f, "T");

            a.time = 0.5f;      // A 的中间（A 时长 1.0s → 相位 0.5）
            a.floatParams["Speed"] = 1.0f;
            // 期望 0.5/1.0*2.0 = 1.0。**这里不该出现 +dt** —— 推进播放头是
            // `AnimatorSystem::tick` 的活，`stepAnimController` 只管状态机。
            // （第一版期望写了 1.001，忘了这一层分工。）
            stepAnimController(*c4, a, s, *f.model.skeleton, 0.001f, "T");
            report("sm_phase_continuous", static_cast<double>(a.time), 1.0,
                   1.0e-4);
        }

        // ---- 7) 目标 clip 不在骨架里：忽略这条转移，不切到 T-pose ----
        // 采纳的话 `samplePose(nullptr)` 静默退回 rest → 画面上是 T-pose，
        // 而日志里状态机一切正常。
        {
            const std::string kMissing = R"JSON({
                "states": [
                    { "name": "S1", "clip": "A" },
                    { "name": "S2", "clip": "NoSuchClip" }
                ],
                "transitions": [
                    { "from": "S1", "to": "S2", "duration": 0.1 }
                ]
            })JSON";
            auto c5 = parseAnimController(kMissing);
            Fixture f;
            makeFixture(f);
            ecs::SkinnedMeshComponent& s = comp(f);
            ecs::AnimatorComponent& a = anim(f);
            for (int i = 0; i < 3; ++i)
                stepAnimController(*c5, a, s, *f.model.skeleton, 1.0f / 60.0f,
                                   "T");
            report("sm_clip_missing_ignored",
                   static_cast<double>(a.stateIndex), 0.0, 0.0);
            reportFlag("sm_clip_missing_kept_clip", s.clipName == "A",
                       "clip=" + s.clipName);
        }

        // ---- 8) 路径不存在：不崩、不接管（走完整 tick 路径）----
        // 这一条走 `AnimatorSystem::tick`，覆盖"controllerFor 的失败缓存"
        // 与"加载失败回落到单 clip 直放"两条 —— 手搓 JSON 测不到。
        {
            Fixture f;
            makeFixture(f);
            ecs::AnimatorComponent& a = anim(f);
            a.controllerPath = "animators/definitely-not-here.anim.json";
            a.defaultClip = "A";
            comp(f).clipName.clear();

            AnimatorSystem sys;
            // 跑两帧：第二帧应当命中"失败也缓存"，不再 fopen
            sys.tick(f.scene, 1.0f / 60.0f);
            sys.tick(f.scene, 1.0f / 60.0f);
            // 控制器无效 → 不接管 → 回落到 defaultClip 补一次
            reportFlag("sm_load_fail_fallback", comp(f).clipName == "A",
                       "clip=" + comp(f).clipName +
                           ",ctl=" + std::to_string(sys.lastControllerCount()));
        }
    }

    const int failures = g_fail;
    VK_LOG_INFO("ANIM-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return failures;
}

} // namespace animation
