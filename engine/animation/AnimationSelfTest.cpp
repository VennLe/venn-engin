// ============================================================
// animation/AnimationSelfTest.cpp
// ============================================================

#include "animation/AnimationSelfTest.h"

#include "animation/AnimatorSystem.h"
#include "animation/Pose.h"
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

    const int failures = g_fail;
    VK_LOG_INFO("ANIM-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return failures;
}

} // namespace animation
