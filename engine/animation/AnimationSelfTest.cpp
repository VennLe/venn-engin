// ============================================================
// animation/AnimationSelfTest.cpp
// ============================================================

#include "animation/AnimationSelfTest.h"

#include "animation/AnimatorSystem.h"
#include "animation/AnimEvent.h"
#include "animation/ClipEdit.h"
#include "animation/Constraint.h"
#include "animation/Curve.h"
#include "animation/CurveBake.h"
#include "animation/NlaTimeline.h"
#include "animation/RigEdit.h"
#include "animation/WeightSet.h"
#include "animation/RootMotion.h"
#include "animation/BoneAttachment.h"
#include "animation/Pose.h"
#include "animation/GaitCycle.h"
#include "animation/Locomotion.h"
#include "animation/StrideMatch.h"
#include "animation/TwoBoneIK.h"
#include "assets/Mesh.h"
#include "assets/Model.h"
#include "assets/Skeleton.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

// ============================================================
// B1（2026-10-26）：分层夹具 + 可手算的判别式
//
// 层的语义要能**手算**，所以两段 clip 各自只动一根骨：
//   base  → joint0(root) 绕 Z 0→90°，hand 缩放 1→2（1.0 秒）
//   upper → joint1(arm)  绕 X 0→90°，hand 缩放 1→3（1.0 秒）
// 于是"遮罩落在哪一根上，差异就只出现在那一根上"，断言不用写整条算式。
//
// ⚠ hand 的**缩放**是刻意挑的判别式：世界矩阵第 0 列的长度等于该骨的
//   scale.x，且与父骨的旋转无关 —— 所以"这一层有没有覆盖到 hand"可以直接
//   读一个数，不必再比一遍旋转（比旋转要先算夹角，写错了还看不出）。
// ============================================================

assets::AnimationClip makeUpperClip(float dur, const std::string& name) {
    assets::AnimationClip c;
    c.name = name;
    c.duration = dur;

    const glm::quat q0(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat q1 =
        glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    assets::AnimTrack rot;
    rot.joint = 1;
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
                 glm::vec4(3.0f, 3.0f, 3.0f, 0.0f)};
    c.tracks.push_back(sc);
    return c;
}

// 带 AnimatorComponent 的夹具（层挂在它上面）。
void buildLayerFixture(Fixture& f) {
    auto skel = std::make_shared<assets::Skeleton>(makeChainSkeleton());
    skel->clips.push_back(makeClip(1.0f, "base"));
    skel->clips.push_back(makeUpperClip(1.0f, "upper"));
    f.model.skeleton = skel;

    f.root = f.scene.createObject("layered");
    ecs::SkinnedMeshComponent sk;
    sk.model = &f.model;
    sk.clipName = "base";
    sk.playing = true;
    sk.loop = true;
    f.scene.world().add<ecs::SkinnedMeshComponent>(f.root, sk);

    ecs::AnimatorComponent a;
    a.playing = true;
    a.loop = true;
    a.defaultClip = "base";
    f.scene.world().add<ecs::AnimatorComponent>(f.root, a);
}

// 逐位比较 [from, to) 区间内的关节矩阵，返回不同的个数。
// ⚠ 逐位（`memcmp`）而不是容差：这组的核心判据就是"没被遮罩碰过的骨
//   **一个 bit 都不许变**"，容差会让"多混了 1e-7"这种真 bug 混过去。
int countDiffMats(const std::vector<glm::mat4>& a,
                  const std::vector<glm::mat4>& b, size_t from, size_t to) {
    int bad = 0;
    const size_t m = std::min(a.size(), b.size());
    for (size_t i = from; i < std::min(to, m); ++i) {
        if (std::memcmp(&a[i], &b[i], sizeof(glm::mat4)) != 0) ++bad;
    }
    return bad;
}

// 世界矩阵第 0 列的长度 = 该骨的 scale.x（与父骨旋转无关）。
float matScaleX(const glm::mat4& m) {
    return glm::length(glm::vec3(m[0]));
}

// ============================================================
// B2（2026-10-26）：动画事件
// ============================================================
//
// 事件表的判据几乎全是"**发了没有 / 发了几次**"，所以夹具刻意让每件事
// 都是可数的整数：三个事件落在 0.0 / 0.3 / 0.6，clip 时长 1.0。
// 一圈正好 3 次、两圈 6 次 —— 任何"每帧都发"（60 次）、"边界重复发"
// （4 次）、"时间不推进"（0 次）都一眼能看出来。
// ⚠ 不用"落在区间内就发"来写判据：那种实现一圈会发 60 次，而 60 这个数
//   在不同 dt 下还会变 —— 一旦把它当成期望值，用例就跟着实现走了。

// 造一张固定的事件表（0.0 / 0.3 / 0.6）。
animation::AnimEventTrack makeEventTrack(const std::string& clip) {
    animation::AnimEventTrack tr;
    tr.clip = clip;
    tr.events = {
        {0.0f, "Open", ""},
        {0.3f, "Footstep", "walk1"},
        {0.6f, "Footstep", "walk2"},
    };
    return tr;
}

// 在 buildLayerFixture（base + upper，各 1.0 秒）上挂事件表。
void buildEventFixture(Fixture& f, bool loop) {
    buildLayerFixture(f);
    ecs::AnimatorComponent& a = anim(f);
    a.loop = loop;
    a.eventTracks.push_back(makeEventTrack("base"));
}

// 把 FiredAnimEvent 压成一个可比对的短串。`t` 用的是**事件自身的时间**，
// 这正是"序列不随帧率变"能被逐条比对的原因（播放头位置会变，事件时间不会）。
std::string evKey(const FiredAnimEvent& e) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s@%.4f#%s", e.kind.c_str(),
                  static_cast<double>(e.time), e.payload.c_str());
    return std::string(buf);
}

// 跑一段时间，把发出的事件按"圈"分组。
// 判圈靠**播放头回绕**（`time` 比上一帧小）：事件自己不会告诉我们那是
// 第几圈，而 runFrames 的帧数在两圈之间又对不齐（1/60 × 60 并不精确等于
// 1.0），所以只能看播放头。
std::vector<std::vector<std::string>> runLaps(AnimatorSystem& sys, Fixture& f,
                                             float dt, int frames) {
    std::vector<std::vector<std::string>> laps(1);
    float last = -1.0f;
    for (int i = 0; i < frames; ++i) {
        sys.tick(f.scene, dt);
        const float t = anim(f).time;
        if (t < last) laps.emplace_back();   // 回绕 = 新的一圈
        last = t;
        for (const FiredAnimEvent& e : sys.eventsThisFrame())
            laps.back().push_back(evKey(e));
    }
    return laps;
}

// ============================================================
// B3（2026-10-26）：根运动
// ============================================================
//
// 夹具要能让"走了多远"**手算**出来：根骨沿 +X 从 0 匀速走到 1.0 m，
// clip 时长 1.0 s → 每帧位移恒为 `dt × 1.0` 米。同时绕 +Z 转 30°，
// 于是"位移"和"朝向"两条链各有一个能被反推的数。
// ⚠ 用**匀速直线**而不是缓动曲线：缓动的期望值得积一次分，而积分写错
//   之后"看起来差不多"（差个系数），断言反而失去判别力。
assets::AnimationClip makeRootMotionClip(float dur, const std::string& name) {
    assets::AnimationClip c;
    c.name = name;
    c.duration = dur;

    assets::AnimTrack tr;
    tr.joint = 0;
    tr.path = assets::AnimTrack::Translation;
    tr.times = {0.0f, dur};
    tr.values = {glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
                 glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)};
    c.tracks.push_back(tr);

    const glm::quat q0(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::quat q1 =
        glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    assets::AnimTrack rot;
    rot.joint = 0;
    rot.path = assets::AnimTrack::Rotation;
    rot.times = {0.0f, dur};
    rot.values = {glm::vec4(q0.x, q0.y, q0.z, q0.w),
                  glm::vec4(q1.x, q1.y, q1.z, q1.w)};
    c.tracks.push_back(rot);
    return c;
}

void buildRootFixture(Fixture& f,
                      ecs::AnimatorComponent::RootMotionMode mode, bool loop) {
    auto skel = std::make_shared<assets::Skeleton>(makeChainSkeleton());
    skel->clips.push_back(makeRootMotionClip(1.0f, "walk"));
    f.model.skeleton = skel;

    f.root = f.scene.createObject("walker");
    ecs::SkinnedMeshComponent sk;
    sk.model = &f.model;
    sk.clipName = "walk";
    sk.playing = true;
    sk.loop = loop;
    f.scene.world().add<ecs::SkinnedMeshComponent>(f.root, sk);

    ecs::AnimatorComponent a;
    a.playing = true;
    a.loop = loop;
    a.defaultClip = "walk";
    a.rootMotion = mode;
    f.scene.world().add<ecs::AnimatorComponent>(f.root, a);
}

ecs::TransformComponent& xf(Fixture& f) {
    return *f.scene.world().get<ecs::TransformComponent>(f.root);
}

// ============================================================
// 步态夹具：手工搭一具 1.7 m 的人形骨架
// ============================================================
//
// 为什么手搭而不是加载 fbx：这条链要断的是"K 帧表 → 数值"的换算，不是
// 导入器。手搭骨架每个关节的**世界位置都是写死的常量**，于是
//   · 腿长 / 髋高 / 踝距 全部能手算（断言的期望值才有出处）；
//   · "整具骨架转 37° 后读数不变"这条不变性才有意义 —— 真实资产带 yaw，
//     但那是导入器给的，改了不好对照。
//
// 姿态：Z-up、朝 −Y、左侧 +X（右手系里的一个人：fwd×up=left）。
// ⚠ 命名**刻意分成两套**（Mixamo / Rigify）：角色表的价值全在"换一套命名
//   还能认出腿"，只测一套等于把那张候选名表当装饰。
struct HumanoidBoneDef {
    const char* mixamo;
    const char* rigify;
    int parent;      // 本表里的下标（-1 = 根）
    glm::vec3 pos;   // 静止世界位置（米）
};

const HumanoidBoneDef kHumanoidBones[] = {
    //                        Mixamo 名               Rigify 名      父   世界位置
    {"mixamorig:Hips",        "hips",                 -1, {0.00f,  0.00f, 0.95f}},
    {"mixamorig:Spine",       "spine",                 0, {0.00f,  0.00f, 1.05f}},
    {"mixamorig:Spine2",      "chest",                 1, {0.00f,  0.00f, 1.25f}},
    {"mixamorig:Neck",        "neck",                  2, {0.00f,  0.00f, 1.48f}},
    {"mixamorig:Head",        "head",                  3, {0.00f,  0.00f, 1.58f}},
    {"mixamorig:LeftArm",     "upper_arm.L",           2, {0.20f,  0.00f, 1.42f}},
    {"mixamorig:LeftForeArm", "forearm.L",             5, {0.20f,  0.00f, 1.15f}},
    {"mixamorig:LeftHand",    "hand.L",                6, {0.20f,  0.00f, 0.90f}},
    {"mixamorig:RightArm",    "upper_arm.R",           2, {-0.20f, 0.00f, 1.42f}},
    {"mixamorig:RightForeArm","forearm.R",             8, {-0.20f, 0.00f, 1.15f}},
    {"mixamorig:RightHand",   "hand.R",                9, {-0.20f, 0.00f, 0.90f}},
    // 腿：大腿 0.4205 + 小腿 0.4205 = 0.8409，髋高 0.84 → 静止时**几乎
    // 伸直**（真人站着就是这样）。膝往前偏 2 cm 只是为了让极向量有个符号，
    // "要不要下蹲"由 GaitParams::maxCrouch 决定，不靠夹具预留。
    {"mixamorig:LeftUpLeg",   "thigh.L",               0, {0.10f,  0.00f, 0.92f}},
    {"mixamorig:LeftLeg",     "shin.L",               11, {0.10f, -0.02f, 0.50f}},
    {"mixamorig:LeftFoot",    "foot.L",               12, {0.10f,  0.00f, 0.08f}},
    {"mixamorig:LeftToeBase", "toe.L",                13, {0.10f, -0.12f, 0.03f}},
    {"mixamorig:RightUpLeg",  "thigh.R",               0, {-0.10f, 0.00f, 0.92f}},
    {"mixamorig:RightLeg",    "shin.R",               15, {-0.10f,-0.02f, 0.50f}},
    {"mixamorig:RightFoot",   "foot.R",               16, {-0.10f, 0.00f, 0.08f}},
    {"mixamorig:RightToeBase","toe.R",                17, {-0.10f,-0.12f, 0.03f}},
};

constexpr int kHumanoidBoneCount =
    static_cast<int>(sizeof(kHumanoidBones) / sizeof(kHumanoidBones[0]));

// `preRotateDeg` 绕 Z 转整具骨架（不变性用）。所有骨都是单位旋转，
// 所以"父的世界位置"直接用表里的常量，restLocal 就是两者的差。
std::shared_ptr<assets::Skeleton> makeHumanoidSkeleton(bool rigify,
                                                      float preRotateDeg = 0.0f) {
    auto skel = std::make_shared<assets::Skeleton>();
    skel->joints.resize(static_cast<std::size_t>(kHumanoidBoneCount));
    const glm::mat4 R =
        glm::rotate(glm::mat4(1.0f), glm::radians(preRotateDeg),
                    glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec3 shift(1.7f, -0.4f, 0.0f);   // 顺带挪开原点：平移也不该有影响
    for (int i = 0; i < kHumanoidBoneCount; ++i) {
        const HumanoidBoneDef& d = kHumanoidBones[i];
        const glm::vec3 wp = glm::vec3(R * glm::vec4(d.pos, 1.0f)) + shift;
        const glm::vec3 pp =
            (d.parent >= 0)
                ? (glm::vec3(R * glm::vec4(kHumanoidBones[d.parent].pos, 1.0f)) +
                   shift)
                : glm::vec3(0.0f);
        assets::Skeleton::Joint& j = skel->joints[static_cast<std::size_t>(i)];
        j.name = rigify ? d.rigify : d.mixamo;
        j.parent = d.parent;
        j.restLocal = glm::translate(glm::mat4(1.0f), wp - pp);
        j.invBind = glm::mat4(1.0f);
        skel->topoOrder.push_back(i);
    }
    skel->invMeshNode = glm::mat4(1.0f);
    return skel;
}

// 一条只有腿的"非人形"：三根骨、名字对不上、结构也不成双足。
std::shared_ptr<assets::Skeleton> makeArmChainSkeleton() {
    auto skel = std::make_shared<assets::Skeleton>();
    skel->joints.resize(3);
    const char* names[3] = {"shoulder", "elbow", "wrist"};
    for (int i = 0; i < 3; ++i) {
        skel->joints[static_cast<std::size_t>(i)].name = names[i];
        skel->joints[static_cast<std::size_t>(i)].parent = i - 1;
        skel->joints[static_cast<std::size_t>(i)].restLocal =
            glm::translate(glm::mat4(1.0f),
                           glm::vec3(0.0f, 0.0f, i == 0 ? 0.0f : -0.3f));
        skel->joints[static_cast<std::size_t>(i)].invBind = glm::mat4(1.0f);
        skel->topoOrder.push_back(i);
    }
    return skel;
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

    // ================================================================
    // B1（2026-10-26）：动画层 / 骨骼遮罩
    // ================================================================
    //
    // 这一组要证明的四件事，按"错了会怎样"排序：
    //   ① **没配层 / 权重 0 / 名字没命中 → 逐位回到基线**（否则老场景会飘，
    //      而且这种偏移是"整体略微不同"，肉眼根本发现不了）；
    //   ② **遮罩真的按骨起作用**（不是"整具身体都被覆盖"）；
    //   ③ **权重是连续混合**（0.5 落在中间，不是"要么 0 要么 1"）；
    //   ④ **层的播放头独立**（与底座不是同一个 time，且各自回绕）。
    {
        // 基线：完全不配层，跑 30 帧（0.5 s）。
        // 这个时刻是刻意选的：两段 clip 的时长都是 1.0 s，于是
        //   base 的 hand 缩放 = 1.5、upper 的 hand 缩放 = 2.0
        // 都是"一眼能算出来"的数。
        Fixture f0;
        buildLayerFixture(f0);
        AnimatorSystem s0;
        runFrames(s0, f0, 30, 1.0f / 60.0f);
        const std::vector<glm::mat4> base = comp(f0).jointMatrices;
        const float baseHandScale = matScaleX(base[2]);
        report("layer_base_hand_scale", static_cast<double>(baseHandScale), 1.5,
               1.0e-4);

        // ---- ① 三种"不生效"，都必须逐位等于基线 ----
        {   // clip 空 = 该层关闭（配了一半的中间状态）
            Fixture f;
            buildLayerFixture(f);
            ecs::AnimLayer L;
            L.clip.clear();
            L.fadeDur = 0.0f;
            anim(f).layers.push_back(L);
            AnimatorSystem sys;
            runFrames(sys, f, 30, 1.0f / 60.0f);
            report("layer_empty_clip_identity",
                   static_cast<double>(
                       countDiffMats(base, comp(f).jointMatrices, 0, 3)),
                   0.0, 0.0);
            // 配了但没生效 → configured=1 / applied=0（日志就靠这个差判）
            report("layer_empty_clip_configured", sys.lastLayerCount(), 1.0,
                   0.0);
            report("layer_empty_clip_applied", sys.lastLayerApplied(), 0.0, 0.0);
        }
        {   // weight 0 = 用户把滑块拉到底
            Fixture f;
            buildLayerFixture(f);
            ecs::AnimLayer L;
            L.clip = "upper";
            L.weight = 0.0f;
            L.fadeDur = 0.0f;
            anim(f).layers.push_back(L);
            AnimatorSystem sys;
            runFrames(sys, f, 30, 1.0f / 60.0f);
            report("layer_weight0_identity",
                   static_cast<double>(
                       countDiffMats(base, comp(f).jointMatrices, 0, 3)),
                   0.0, 0.0);
        }
        {   // 遮罩名字一根骨都没命中
            Fixture f;
            buildLayerFixture(f);
            ecs::AnimLayer L;
            L.clip = "upper";
            L.fadeDur = 0.0f;
            L.mask.mode = BoneMaskMode::Allow;
            L.mask.roots = {"no_such_bone"};
            anim(f).layers.push_back(L);
            AnimatorSystem sys;
            runFrames(sys, f, 30, 1.0f / 60.0f);
            report("layer_unresolved_identity",
                   static_cast<double>(
                       countDiffMats(base, comp(f).jointMatrices, 0, 3)),
                   0.0, 0.0);
            report("layer_unresolved_applied", sys.lastLayerApplied(), 0.0, 0.0);
            // 解算结果本身也要对：0 根命中 + 1 个 root 没解析成功
            const BoneMask m = buildBoneMask(*f.model.skeleton, L.mask);
            reportFlag("layer_unresolved_spec",
                       m.included == 0 && m.unresolved == 1 &&
                           m.firstProblem == "no_such_bone",
                       "inc=" + std::to_string(m.included) + ",unres=" +
                           std::to_string(m.unresolved));
        }

        // ---- ② 遮罩 Allow[hand]：只换 hand，root/arm 逐位不动 ----
        {
            Fixture f;
            buildLayerFixture(f);
            ecs::AnimLayer L;
            L.clip = "upper";
            L.weight = 1.0f;
            L.fadeDur = 0.0f;
            L.mask.mode = BoneMaskMode::Allow;
            L.mask.roots = {"hand"};
            anim(f).layers.push_back(L);
            AnimatorSystem sys;
            runFrames(sys, f, 30, 1.0f / 60.0f);
            const auto& m = comp(f).jointMatrices;
            // root / arm：一根 bit 都不许变
            report("layer_mask_keeps_root_arm",
                   static_cast<double>(countDiffMats(base, m, 0, 2)), 0.0,
                   0.0);
            // hand：必须变，且缩放 = 层的 2.0（不是基线的 1.5）
            report("layer_mask_changes_hand",
                   static_cast<double>(countDiffMats(base, m, 2, 3)), 1.0,
                   0.0);
            report("layer_mask_hand_scale", static_cast<double>(matScaleX(m[2])),
                   2.0, 1.0e-4);
            report("layer_applied_count", sys.lastLayerApplied(), 1.0, 0.0);
        }

        // ---- ②-b 遮罩 Deny[hand]：arm 吃层、hand 不吃 ----
        // 与 Allow 是镜像判据：如果两种模式被写成同一个分支，这一条会红。
        {
            Fixture f;
            buildLayerFixture(f);
            ecs::AnimLayer L;
            L.clip = "upper";
            L.weight = 1.0f;
            L.fadeDur = 0.0f;
            L.mask.mode = BoneMaskMode::Deny;
            L.mask.roots = {"hand"};
            anim(f).layers.push_back(L);
            AnimatorSystem sys;
            runFrames(sys, f, 30, 1.0f / 60.0f);
            const auto& m = comp(f).jointMatrices;
            // hand 的缩放仍是基线的 1.5（被排除在遮罩外）
            report("layer_deny_hand_scale",
                   static_cast<double>(matScaleX(m[2])), 1.5, 1.0e-4);
            // arm 的**局部旋转**被换了 → 它自己的世界矩阵必须变
            report("layer_deny_changes_arm",
                   static_cast<double>(countDiffMats(base, m, 1, 2)), 1.0,
                   0.0);
        }

        // ---- ③ 权重连续：0.5 → hand 缩放落在 1.5 与 2.0 的中间 ----
        {
            Fixture f;
            buildLayerFixture(f);
            ecs::AnimLayer L;
            L.clip = "upper";
            L.weight = 0.5f;
            L.fadeDur = 0.0f;
            L.mask.mode = BoneMaskMode::Allow;
            L.mask.roots = {"hand"};
            anim(f).layers.push_back(L);
            AnimatorSystem sys;
            runFrames(sys, f, 30, 1.0f / 60.0f);
            report("layer_half_hand_scale",
                   static_cast<double>(matScaleX(comp(f).jointMatrices[2])),
                   1.75, 1.0e-4);
        }

        // ---- ④ 层的播放头独立推进、独立回绕 ----
        {
            Fixture f;
            buildLayerFixture(f);
            ecs::AnimLayer L;
            L.clip = "upper";
            L.weight = 1.0f;
            L.fadeDur = 0.0f;
            anim(f).layers.push_back(L);
            AnimatorSystem sys;
            runFrames(sys, f, 30, 1.0f / 60.0f);   // 0.5 s
            report("layer_time_advance",
                   static_cast<double>(anim(f).layers[0].time), 0.5, 1.0e-4);
            // 再跑 40 帧（累计 1.1667 s）→ 自己的 dur 是 1.0 → 回绕到 0.1667
            runFrames(sys, f, 40, 1.0f / 60.0f);
            report("layer_time_wrap",
                   static_cast<double>(anim(f).layers[0].time), 0.1666667,
                   1.0e-3);
            // 底座的播放头同样累计到 1.1667 后回绕 —— 两者**彼此独立**
            report("layer_base_time_wrap",
                   static_cast<double>(comp(f).time), 0.1666667, 1.0e-3);
        }

        // ---- ⑤ 混合原语本身（pose 空间，手算）----
        // 上面几组走的是完整 tick；这一组单独盯 `blendPoseMasked` 的
        // 端点短路与局部 TRS 语义 —— 它们错了以后"看着只是差一点点"。
        {
            const assets::Skeleton s = makeChainSkeleton();
            Pose A, B, out;
            A.setRest(s);
            B.setRest(s);
            for (int j = 0; j < 3; ++j) {
                A.rotation[static_cast<size_t>(j)] =
                    glm::angleAxis(glm::radians(10.0f), glm::vec3(0, 0, 1));
                B.rotation[static_cast<size_t>(j)] =
                    glm::angleAxis(glm::radians(40.0f), glm::vec3(1, 0, 0));
            }
            A.scale[2] = glm::vec3(1.5f);
            B.scale[2] = glm::vec3(2.5f);
            A.translation[1] = glm::vec3(0.5f, 0.0f, 0.0f);
            B.translation[1] = glm::vec3(0.5f, 0.3f, 0.0f);

            BoneMaskSpec spec;
            spec.mode = BoneMaskMode::Allow;
            spec.roots = {"hand"};
            const BoneMask mk = buildBoneMask(s, spec);
            report("mask_included_hand", static_cast<double>(mk.included), 1.0,
                   0.0);

            // w = 1 + 遮罩 → 未遮罩的骨逐位等于 A，遮罩内的骨逐位等于 B
            blendPoseMasked(A, B, mk, 1.0f, out);
            reportFlag("mask_w1_untouched_bitwise",
                       std::memcmp(&out.rotation[0], &A.rotation[0],
                                   sizeof(glm::quat)) == 0 &&
                           std::memcmp(&out.rotation[1], &A.rotation[1],
                                       sizeof(glm::quat)) == 0 &&
                           std::memcmp(&out.translation[1], &A.translation[1],
                                       sizeof(glm::vec3)) == 0,
                       "r0,r1,t1");
            reportFlag("mask_w1_covered_bitwise",
                       std::memcmp(&out.rotation[2], &B.rotation[2],
                                   sizeof(glm::quat)) == 0 &&
                           std::memcmp(&out.scale[2], &B.scale[2],
                                       sizeof(glm::vec3)) == 0,
                       "r2,s2");

            // w = 0 → 全部逐位等于 A（回归基线的唯一保证）
            blendPoseMasked(A, B, mk, 0.0f, out);
            reportFlag("mask_w0_identity_bitwise",
                       std::memcmp(out.rotation.data(), A.rotation.data(),
                                   A.rotation.size() * sizeof(glm::quat)) ==
                               0 &&
                           std::memcmp(out.scale.data(), A.scale.data(),
                                       A.scale.size() * sizeof(glm::vec3)) ==
                               0,
                       "all");

            // w = 0.5 → slerp 中点 + 线性插值的缩放（局部 TRS 混合）
            // ⚠ 平移的判据也在这儿：遮罩外的骨**平移**同样一个 bit 都不许动
            //   （B 把 arm 的平移挪了 0.3，遮罩不含 arm → 仍然是 A 的 0）
            blendPoseMasked(A, B, mk, 0.5f, out);
            report("mask_w05_scale", static_cast<double>(out.scale[2].x), 2.0,
                   1.0e-6);
            reportFlag("mask_w05_translation_masked",
                       out.translation[1].y == 0.0f,
                       "ty=" + std::to_string(out.translation[1].y));

            // 遮罩全开时平移才是真的混：0.3 × 0.5 = 0.15
            BoneMaskSpec allSpec;   // mode = All（默认）
            const BoneMask mkAll = buildBoneMask(s, allSpec);
            reportFlag("mask_all_included", mkAll.full() && mkAll.included == 3,
                       "inc=" + std::to_string(mkAll.included));
            blendPoseMasked(A, B, mkAll, 0.5f, out);
            report("mask_w05_translation", static_cast<double>(out.translation[1].y),
                   0.15, 1.0e-6);
        }
    }

    // ================================================================
    // B2：动画事件 / Notify（2026-10-26）
    // ================================================================
    //
    // 分两半，缺一不可：
    //   ① `collectAnimEvents` 纯函数 —— 边界语义只能在这里钉死。这一半的
    //      判据是"发了没有 / 几次 / 什么顺序"，全部可手算。
    //   ② 真场景（`AnimatorSystem::tick`）—— 证明它**真的被接上了**。
    //      只测 ① 的话，函数再对、tick 里没调也是全绿。
    {
        // ---------------- ① 纯函数 ----------------
        const AnimEventTrack tr = makeEventTrack("base");
        const float dur = 1.0f;
        std::vector<int> hit;

        // 第一帧：prev=0 → [0, dt) 里落着 t=0 那条。
        // ⚠ 这条是"半开区间"的第一个受益者：改成 `(prev,new]` 之后
        //   t=0 永远不会发，而"开场音效"是最常见的需求。
        collectAnimEvents(tr, 0.0f, 1.0f / 60.0f, dur, true, hit);
        reportFlag("ev_first_frame_t0", hit.size() == 1 && hit[0] == 0,
                   "n=" + std::to_string(hit.size()));

        // dt = 0（编辑器非 Play / 暂停）：prev == new → 空区间 → 一条不发。
        // ⚠ 这是"每帧都发"最容易溜进来的地方：暂停时播放头停在 0.3 上，
        //   闭区间实现会每帧发一次。
        collectAnimEvents(tr, 0.5f, 0.5f, dur, true, hit);
        reportFlag("ev_pause_no_fire", hit.empty(),
                   "n=" + std::to_string(hit.size()));

        // 停在事件上 → 不发；从事件上往前走 → 只发一次（不是两次）
        collectAnimEvents(tr, 0.3f, 0.3f, dur, true, hit);
        reportFlag("ev_on_event_no_fire", hit.empty(),
                   "n=" + std::to_string(hit.size()));
        collectAnimEvents(tr, 0.3f, 0.35f, dur, true, hit);
        reportFlag("ev_after_event_once", hit.size() == 1 && hit[0] == 1,
                   "n=" + std::to_string(hit.size()));

        // 端点：正好落在 new 上的事件**不丢，只是推到下一帧**
        // （闭区间在这里会发两次；`(prev,new]` 会漏掉从左端进入的那一次）
        collectAnimEvents(tr, 0.29f, 0.30f, dur, true, hit);
        reportFlag("ev_half_open_right_end", hit.empty(),
                   "n=" + std::to_string(hit.size()));
        collectAnimEvents(tr, 0.30f, 0.31f, dur, true, hit);
        reportFlag("ev_half_open_next_frame", hit.size() == 1 && hit[0] == 1,
                   "n=" + std::to_string(hit.size()));

        // 时长 0（单帧 clip）：没有"跨过"这回事，一条都不发。
        {
            const AnimEventTrack z = makeEventTrack("base");
            std::vector<int> h2;
            collectAnimEvents(z, 0.0f, 0.0f, 0.0f, true, h2);
            reportFlag("ev_zero_duration", h2.empty(),
                       "n=" + std::to_string(h2.size()));
        }

        // ---- 帧率无关（纯函数层）----
        // 用 0.1 / 0.4 / 0.7（**避开 0.0 与 1.0 两个回绕端点**）：5 秒 → 15 条。
        // ⚠ 端点上的事件每次回绕都会发一次，把它算进"次数相同"里会让这条
        //   判据测的是回绕而不是帧率。
        {
            AnimEventTrack t5;
            t5.clip = "base";
            t5.events = {{0.1f, "A", ""}, {0.4f, "B", ""}, {0.7f, "C", ""}};
            auto countAt = [&](float dt) {
                const int frames = static_cast<int>(std::lround(5.0f / dt));
                float prev = 0.0f, t = 0.0f;
                int n = 0;
                std::vector<int> h;
                for (int i = 0; i < frames; ++i) {
                    t += dt;
                    if (t >= dur) t -= dur;
                    collectAnimEvents(t5, prev, t, dur, true, h);
                    n += static_cast<int>(h.size());
                    prev = t;
                }
                return n;
            };
            const int c60 = countAt(1.0f / 60.0f);
            const int c240 = countAt(1.0f / 240.0f);
            report("ev_count_60fps", static_cast<double>(c60), 15.0, 0.0);
            report("ev_count_240fps", static_cast<double>(c240), 15.0, 0.0);
            reportFlag("ev_count_fps_independent", c60 == c240,
                       "c60=" + std::to_string(c60) + ",c240=" +
                           std::to_string(c240));
        }

        // ---- 跨圈：上圈尾巴 → 新圈开头，顺序不能倒 ----
        // ⚠ 这是"合并成一个大区间再排序"会错的地方：0.95 的事件发生在
        //   0.0 的事件**之前**，而按时间排序会把它们倒过来。
        {
            AnimEventTrack tw;
            tw.clip = "base";
            tw.events = {{0.0f, "Head", ""}, {0.95f, "Tail", ""}};
            std::vector<int> h;
            collectAnimEvents(tw, 0.90f, 0.05f, dur, true, h);
            const bool order =
                h.size() == 2 &&
                tw.events[static_cast<size_t>(h[0])].kind == "Tail" &&
                tw.events[static_cast<size_t>(h[1])].kind == "Head";
            reportFlag("ev_wrap_tail_then_head", order,
                       "n=" + std::to_string(h.size()) +
                           ",first=" +
                           (h.empty() ? std::string("-")
                                      : tw.events[static_cast<size_t>(h[0])].kind));

            // 非循环却倒退（脚本把 time 改小）：只收新圈开头那一截，
            // **不许**把 [prev,dur] 也收进来 —— 那一段没有播放过。
            collectAnimEvents(tw, 0.90f, 0.05f, dur, false, h);
            reportFlag("ev_backward_nonloop_head_only",
                       h.size() == 1 &&
                           tw.events[static_cast<size_t>(h[0])].kind == "Head",
                       "n=" + std::to_string(h.size()));
        }

        // ---- 永远不会发的两种写法 ----
        // ① `t == duration`：`wrapTime` 之后播放头到不了这里，半开区间又把
        //    右端排除 → 永远不发（写在 0 上才对）。
        // ② `t < 0`：区间下界是 0，负时间永远落在外面。
        {
            AnimEventTrack tb;
            tb.clip = "base";
            tb.events = {{1.0f, "AtDur", ""}, {-0.5f, "Negative", ""}};
            float prev = 0.0f, t = 0.0f;
            int n = 0;
            std::vector<int> h;
            for (int i = 0; i < 300; ++i) {   // 5 秒 = 5 圈
                t += 1.0f / 60.0f;
                if (t >= dur) t -= dur;
                collectAnimEvents(tb, prev, t, dur, true, h);
                n += static_cast<int>(h.size());
                prev = t;
            }
            reportFlag("ev_t_at_duration_never_fires", n == 0,
                       "n=" + std::to_string(n));
        }

        // ---- 同一时刻多条：按表里的顺序发（stable）----
        {
            AnimEventTrack ts;
            ts.clip = "base";
            ts.events = {{0.5f, "X", ""}, {0.5f, "Y", ""}};
            std::vector<int> h;
            collectAnimEvents(ts, 0.45f, 0.55f, dur, true, h);
            reportFlag("ev_same_time_table_order",
                       h.size() == 2 && h[0] == 0 && h[1] == 1,
                       "n=" + std::to_string(h.size()) + ",f=" +
                           (h.empty() ? std::string("-")
                                      : std::to_string(h[0])));
        }

        // ---------------- ② 真场景 ----------------
        // 一圈 = 3 条（0.0 / 0.3 / 0.6）。第二圈与第一圈必须**逐条相同**。
        {
            Fixture f;
            buildEventFixture(f, /*loop=*/true);
            AnimatorSystem sys;
            const auto laps = runLaps(sys, f, 1.0f / 60.0f, 180);
            reportFlag("ev_scene_lap_count",
                       laps.size() >= 2 && laps[0].size() == 3,
                       "laps=" + std::to_string(laps.size()) + ",lap0=" +
                           std::to_string(laps.empty() ? 0 : laps[0].size()));
            reportFlag("ev_scene_two_laps_same_seq",
                       laps.size() >= 2 && laps[0] == laps[1],
                       "lap0=" + std::to_string(laps.empty() ? 0 : laps[0].size()) +
                           ",lap1=" +
                           std::to_string(laps.size() < 2 ? 0 : laps[1].size()));
        }

        // ---- 帧率无关（真场景）：60 fps 与 240 fps 的前三圈逐条相同 ----
        {
            Fixture f1;
            buildEventFixture(f1, true);
            AnimatorSystem s1;
            const auto l1 = runLaps(s1, f1, 1.0f / 60.0f, 300);

            Fixture f2;
            buildEventFixture(f2, true);
            AnimatorSystem s2;
            const auto l2 = runLaps(s2, f2, 1.0f / 240.0f, 1200);

            bool same = l1.size() >= 3 && l2.size() >= 3;
            if (same) {
                for (int i = 0; i < 3; ++i) same = same && (l1[i] == l2[i]);
            }
            reportFlag("ev_scene_fps_independent", same,
                       "l1=" + std::to_string(l1.size()) + ",l2=" +
                           std::to_string(l2.size()));
        }

        // ---- dt = 0（编辑器非 Play）：一条都不发 ----
        {
            Fixture f;
            buildEventFixture(f, true);
            AnimatorSystem sys;
            sys.tick(f.scene, 0.0f);
            reportFlag("ev_scene_dt0_no_fire", sys.eventsThisFrame().empty(),
                       "n=" + std::to_string(sys.eventsThisFrame().size()));
        }

        // ---- 字段完整：kind / payload / entity / clip / layer ----
        {
            Fixture f;
            buildEventFixture(f, /*loop=*/false);   // 不循环，免得回绕混进来
            AnimatorSystem sys;
            std::vector<std::string> seq;
            std::vector<FiredAnimEvent> raw;
            for (int i = 0; i < 60; ++i) {
                sys.tick(f.scene, 1.0f / 60.0f);
                for (const FiredAnimEvent& e : sys.eventsThisFrame()) {
                    seq.push_back(evKey(e));
                    raw.push_back(e);
                }
            }
            const bool ok = seq.size() == 3 && seq[0] == "Open@0.0000#" &&
                            seq[1] == "Footstep@0.3000#walk1" &&
                            seq[2] == "Footstep@0.6000#walk2";
            reportFlag("ev_scene_seq", ok,
                       "n=" + std::to_string(seq.size()));
            reportFlag("ev_scene_fields",
                       raw.size() == 3 && raw[1].entity == f.root.id &&
                           raw[1].clip == "base" && raw[1].layer.empty(),
                       "ent=" + std::to_string(raw.size() > 1 ? raw[1].entity : 0));
        }

        // ---- 换 clip：上一段的播放头不许当新段的起点 ----
        //
        // ⚠ 这是 `eventPrev` 唯一不能省的地方。真实来路是**状态机**：
        //   `AnimController::switchClip` 换片段时会把 `time` 归零（或按相位
        //   换算成另一个值），而 `eventPrev` 还停在旧片段的 0.5 s。此时
        //   `[0.5, 1.0)` 会被当成"这一帧跨过的区间" → 新片段里 0.8 的事件
        //   **立刻发一次**（它其实才刚开始）。
        //
        //   所以这里必须**同时**改 clipName 与 time（只改 clipName 的话
        //   time 还是 0.5，`[0.5, 0.5166)` 本来就不含 0.8，测不出区别 ——
        //   第一版就是这么写的，去掉修复后照样全绿）。
        {
            Fixture f;
            buildEventFixture(f, /*loop=*/true);
            AnimatorSystem sys;
            for (int i = 0; i < 30; ++i) sys.tick(f.scene, 1.0f / 60.0f);

            ecs::AnimatorComponent& a = anim(f);
            AnimEventTrack ut;
            ut.clip = "upper";
            ut.events = {{0.8f, "UpperLate", ""}};
            a.eventTracks.push_back(ut);
            comp(f).clipName = "upper";
            a.time = 0.0f;   // = AnimController::switchClip(restart)
            comp(f).time = 0.0f;

            sys.tick(f.scene, 1.0f / 60.0f);
            const int onSwitch = static_cast<int>(sys.eventsThisFrame().size());

            int later = 0;
            for (int i = 0; i < 60; ++i) {   // 再跑 1 秒，跨过 0.8
                sys.tick(f.scene, 1.0f / 60.0f);
                later += static_cast<int>(sys.eventsThisFrame().size());
            }
            reportFlag("ev_clip_switch_no_stale", onSwitch == 0,
                       "onSwitch=" + std::to_string(onSwitch));
            reportFlag("ev_clip_switch_then_fires", later == 1,
                       "later=" + std::to_string(later));
        }

        // ---- 附加层的事件带上层名，底座事件不受影响 ----
        {
            Fixture f;
            buildEventFixture(f, /*loop=*/false);
            ecs::AnimatorComponent& a = anim(f);
            ecs::AnimLayer L;
            L.name = "upper";
            L.clip = "upper";
            L.mask.mode = BoneMaskMode::All;
            a.layers.push_back(L);
            AnimEventTrack ut;
            ut.clip = "upper";
            ut.events = {{0.5f, "Cast", "fireball"}};
            a.eventTracks.push_back(ut);

            AnimatorSystem sys;
            int layerEv = 0, layerOk = 0, baseEv = 0;
            for (int i = 0; i < 60; ++i) {
                sys.tick(f.scene, 1.0f / 60.0f);
                for (const FiredAnimEvent& e : sys.eventsThisFrame()) {
                    if (e.layer.empty()) {
                        ++baseEv;
                    } else {
                        ++layerEv;
                        if (e.layer == "upper" && e.kind == "Cast" &&
                            e.payload == "fireball")
                            ++layerOk;
                    }
                }
            }
            reportFlag("ev_layer_fires_once", layerEv == 1 && layerOk == 1,
                       "n=" + std::to_string(layerEv));
            reportFlag("ev_layer_base_unaffected", baseEv == 3,
                       "base=" + std::to_string(baseEv));
        }

        // ---- 序列化：只在非空时写 / 读回来逐字段相同 ----
        {
            // 这里只验"整理后的顺序"与"空表不写"两个纯函数性质；
            // 落盘的往返在 `tools/verify_anim_events.py` 里过真 JSON。
            AnimEventTrack u;
            u.clip = "base";
            u.events = {{0.6f, "C", ""}, {0.1f, "A", ""}, {0.3f, "B", ""}};
            sortAnimEventTrack(u);
            const bool sorted = u.events[0].kind == "A" &&
                                u.events[1].kind == "B" &&
                                u.events[2].kind == "C";
            reportFlag("ev_sort_track", sorted,
                       "first=" + u.events[0].kind);

            const std::vector<AnimEventTrack> none;
            reportFlag("ev_find_empty",
                       findAnimEventTrack(none, "base") == nullptr,
                       "n=0");
            std::vector<AnimEventTrack> one{u};
            reportFlag("ev_find_hit",
                       findAnimEventTrack(one, "base") == &one[0], "n=1");
            reportFlag("ev_find_miss",
                       findAnimEventTrack(one, "nope") == nullptr, "n=1");
        }
    }

    // ============================================================
    // B3（2026-10-26）：根运动
    //
    // 分两层验：先验**纯函数**（增量 / 钳位 / 坐标空间，全部手算得出），
    // 再验**系统**（Transform 真的动了 / Off 逐位不动 / 跨圈丢弃）。
    // 分开的理由：纯函数层能精确到 1e-5，系统层只能验"动没动、朝哪动" ，
    // 混在一起写会逼着系统层用大容差，于是"偏移了 20%"也能过。
    // ============================================================
    {
        assets::Skeleton chain = makeChainSkeleton();

        // ---- 骨名解析 ----
        reportFlag("rm_resolve_auto", resolveRootBone(chain, "") == 0,
                   "joint=0 (first parent==-1)");
        // 不分大小写：美术嘴里说 "Arm"，文件里是 "arm"
        reportFlag("rm_resolve_named", resolveRootBone(chain, "ARM") == 1,
                   "case-insensitive -> 1");
        // ⚠ 找不到必须是 -1 而不是 0：静默取 0 号会让"名字写错"表现成
        //   "在根骨上提位移"（角色原地打转），几乎不可能从画面上反推。
        reportFlag("rm_resolve_miss", resolveRootBone(chain, "no_such") == -1,
                   "-1");

        // ---- 增量 ----
        const glm::mat4 m0 = glm::mat4(1.0f);
        glm::mat4 m1 =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.25f, 0.0f, 0.0f));
        m1 = m1 * glm::mat4_cast(
                      glm::angleAxis(glm::radians(15.0f), glm::vec3(0, 0, 1)));
        const RootMotionDelta d1 = extractRootMotion(chain, m0, m1, false);
        reportFlag("rm_delta_valid", d1.valid, "extract sets valid");
        reportFlag("rm_delta_x", std::abs(d1.position.x - 0.25f) < 1.0e-6f,
                   "d=" + std::to_string(d1.position.x));
        reportFlag("rm_delta_yaw",
                   std::abs(d1.yaw - glm::radians(15.0f)) < 1.0e-5f,
                   "yaw=" + std::to_string(d1.yaw));

        // ---- horizontalOnly 丢掉垂直分量（引擎 up = +Z）----
        const glm::mat4 m2 =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.25f, 0.0f, 0.5f));
        const RootMotionDelta dH = extractRootMotion(chain, m0, m2, true);
        reportFlag("rm_delta_horizontal",
                   dH.position.z == 0.0f &&
                       std::abs(dH.position.x - 0.25f) < 1.0e-6f,
                   "z=" + std::to_string(dH.position.z));
        const RootMotionDelta dV = extractRootMotion(chain, m0, m2, false);
        reportFlag("rm_delta_vertical_kept",
                   std::abs(dV.position.z - 0.5f) < 1.0e-6f,
                   "z=" + std::to_string(dV.position.z));

        // ---- 骨架空间 → 引擎空间（glTF 是 Y-up，这一条就是那个坑）----
        // 骨架空间里"往上走 1 m" = (0,1,0)。glTF 的骨架要求引擎侧看到
        // (0,0,1)。不转的话角色会**朝天上飞**，而每帧日志都对。
        assets::Skeleton yUp = makeChainSkeleton();
        yUp.skeletonToEngine = glm::rotate(
            glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0));
        const glm::mat4 y1 =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const RootMotionDelta dy = extractRootMotion(yUp, m0, y1, false);
        reportFlag("rm_yup_to_zup",
                   std::abs(dy.position.z - 1.0f) < 1.0e-6f &&
                       std::abs(dy.position.y) < 1.0e-6f,
                   "z=" + std::to_string(dy.position.z) +
                       ",y=" + std::to_string(dy.position.y));

        // ---- 钳回基准（这才是 in-place）----
        {
            Pose p;
            p.setRest(chain);
            p.translation[0] = glm::vec3(0.4f, 0.0f, 0.0f);
            const glm::vec3 cur = boneWorldPosition(chain, p, 0);
            const glm::vec3 off =
                stripRootMotion(chain, p, 0, cur, glm::vec3(0.0f), false);
            reportFlag("rm_strip_offset", std::abs(off.x - 0.4f) < 1.0e-6f,
                       "off=" + std::to_string(off.x));
            // 钳完之后根回到基准（0）—— 模型相对实体静止
            reportFlag("rm_strip_back",
                       std::abs(p.translation[0].x) < 1.0e-6f,
                       "x=" + std::to_string(p.translation[0].x));
        }
        {
            // horizontalOnly：水平被钳、垂直保留（"脚不打滑但不离地"）
            Pose p;
            p.setRest(chain);
            p.translation[0] = glm::vec3(0.4f, 0.0f, 0.7f);
            const glm::vec3 cur = boneWorldPosition(chain, p, 0);
            stripRootMotion(chain, p, 0, cur, glm::vec3(0.0f), true);
            reportFlag("rm_strip_keep_z",
                       std::abs(p.translation[0].x) < 1.0e-6f &&
                           std::abs(p.translation[0].z - 0.7f) < 1.0e-6f,
                       "x=" + std::to_string(p.translation[0].x) +
                           ",z=" + std::to_string(p.translation[0].z));
        }

        // ---- twist 分解 ----
        reportFlag("rm_twist_zero",
                   std::abs(twistAngleAround(glm::quat(1, 0, 0, 0),
                                             glm::vec3(0, 0, 1))) < 1.0e-6f,
                   "identity -> 0");
        // 绕 +Z 转 90°：扭转角就是 90°（不是 0、也不是 180°）
        reportFlag(
            "rm_twist_90",
            std::abs(twistAngleAround(
                         glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)),
                         glm::vec3(0, 0, 1)) -
                     glm::radians(90.0f)) < 1.0e-5f,
            "90deg");
        // 绕 +X 转 90°（纯 swing，没有扭转）→ 扭转角必须是 0。
        // 这一条钉住"别把 swing 当成 yaw 累加进 Transform"—— 否则角色
        // 每跳一下都会连带转一点朝向。
        reportFlag(
            "rm_twist_swing_is_zero",
            std::abs(twistAngleAround(
                         glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0)),
                         glm::vec3(0, 0, 1))) < 1.0e-5f,
            "swing -> 0");
    }

    // ---- 系统层：真跑 tick ----
    {
        Fixture f;
        buildRootFixture(f, ecs::AnimatorComponent::RootMotionMode::FromClip,
                         /*loop=*/false);
        AnimatorSystem sys;

        // 第一帧：只记基准，**一点位移都不许提**
        // ⚠ 人形骨架的骨盆离地 ~1 m，这一条挡的就是"一进 Play 就窜天"。
        sys.tick(f.scene, 1.0f / 60.0f);
        reportFlag("rm_first_frame_no_delta", !anim(f).rootDelta.valid,
                   "valid=0");
        reportFlag("rm_first_frame_no_move", xf(f).position.x == 0.0f,
                   "x=" + std::to_string(xf(f).position.x));

        // 记下第一帧的根骨世界位置（= 基准点）
        const float rootP1 = comp(f).jointMatrices[0][3][0];

        // 再跑 59 帧 → 共 60 帧，播放头到 1.0（= clip 末尾，clamp）
        runFrames(sys, f, 59, 1.0f / 60.0f);

        // 位移 = cur(1.0) - cur(1/60) = 1 - 1/60（第一帧不提，所以差一帧）
        reportFlag("rm_tick_moves",
                   std::abs(xf(f).position.x - (59.0f / 60.0f)) < 1.0e-4f,
                   "x=" + std::to_string(xf(f).position.x));

        // 根被钳在基准 → 模型相对实体**没动**（jointMatrices 是
        // invMeshNode*world*invBind，这个夹具里两者都是单位阵，所以
        // [3] 就是根骨的世界位置）
        const float rootP60 = comp(f).jointMatrices[0][3][0];
        reportFlag("rm_in_place", std::abs(rootP60 - rootP1) < 1.0e-4f,
                   "p1=" + std::to_string(rootP1) + ",p60=" +
                       std::to_string(rootP60));

        // 朝向：整段 clip 转 30°，这里差了第一帧 → 30° × 59/60
        reportFlag("rm_yaw_applied",
                   std::abs(xf(f).rotation.z - glm::radians(30.0f)) < 0.02f,
                   "rotZ=" + std::to_string(xf(f).rotation.z));
    }
    {
        // Off：Transform **逐位不动**，姿势也不被钳（与加 B3 之前一致）
        Fixture f;
        buildRootFixture(f, ecs::AnimatorComponent::RootMotionMode::Off,
                         /*loop=*/false);
        AnimatorSystem sys;
        const glm::vec3 before = xf(f).position;
        runFrames(sys, f, 60, 1.0f / 60.0f);
        reportFlag("rm_off_transform_untouched",
                   xf(f).position == before, "bitwise equal");
        reportFlag("rm_off_pose_untouched",
                   std::abs(comp(f).jointMatrices[0][3][0] - 1.0f) < 1.0e-4f,
                   "root follows clip -> x=" +
                       std::to_string(comp(f).jointMatrices[0][3][0]));
    }
    {
        // DeltaOnly：姿势钳住、delta 有效，但**引擎不替它移动**
        Fixture f;
        buildRootFixture(f, ecs::AnimatorComponent::RootMotionMode::DeltaOnly,
                         /*loop=*/false);
        AnimatorSystem sys;
        const glm::vec3 before = xf(f).position;
        runFrames(sys, f, 60, 1.0f / 60.0f);
        reportFlag("rm_deltaonly_transform_untouched",
                   xf(f).position == before, "bitwise equal");
        const RootMotionDelta& d = anim(f).rootDelta;
        reportFlag("rm_deltaonly_delta_valid", d.valid, "valid=1");
        // 最后一帧的增量 = 1/60 米（匀速 1 m/s × 1/60 s）
        reportFlag("rm_deltaonly_delta_step",
                   std::abs(d.position.x - (1.0f / 60.0f)) < 1.0e-4f,
                   "dx=" + std::to_string(d.position.x));
    }
    {
        // 跨圈：两圈之后必须**累计到 ~2 m**（而不是回到 0）。
        // 走路循环接回首帧时根骨从"前方 1 m"瞬回原点，那一帧的增量是
        // -1 m —— 丢掉它才是对的。不丢的话两圈跑完停在 -0.05 m（角色
        // 原地抽搐，而且日志里 dist 一帧一个数、看不出问题）。
        Fixture f;
        buildRootFixture(f, ecs::AnimatorComponent::RootMotionMode::FromClip,
                         /*loop=*/true);
        AnimatorSystem sys;
        runFrames(sys, f, 120, 1.0f / 60.0f);
        reportFlag("rm_wrap_discard", xf(f).position.x > 1.5f,
                   "x=" + std::to_string(xf(f).position.x) +
                       " (不丢会是 ~-0.05)");
    }
    {
        // 编辑器非 Play（dt == 0）：**不提取**。
        // 用户拖时间轴 = 任意跳帧，每一跳都会被当成一次位移 ——
        // 角色会跟着滑条在场景里乱窜，而"拖到 0.9 秒"本身不该让角色移动。
        Fixture f;
        buildRootFixture(f, ecs::AnimatorComponent::RootMotionMode::FromClip,
                         /*loop=*/true);
        AnimatorSystem sys;
        sys.tick(f.scene, 0.0f);
        anim(f).playing = false;
        anim(f).time = 0.9f;
        for (int i = 0; i < 5; ++i) sys.tick(f.scene, 0.0f);
        reportFlag("rm_editor_scrub_no_move", xf(f).position.x == 0.0f,
                   "x=" + std::to_string(xf(f).position.x));
    }

    // ============================================================
    // R5（2026-10-28）：F-Curve 核心
    //
    // 这些断言有一个共同点：**每一条换一种实现都会给出不同的数**。
    // 特别是 `curve_bezier_default_is_smoothstep` —— 默认手柄 (1/3,0)/(
    // -1/3,0) 在 x 上恰好是均匀参数化，于是 y(u) = 3u²−2u³ 就是
    // smoothstep 本尊；这条同时钉住了"贝塞尔求根没算错"和"默认手感
    // 与老 smooth 一致"两件事。
    // ============================================================
    {
        auto mk = [](float t0, float v0, float t1, float v1) {
            Curve c;
            CurveKey a;
            a.time = t0; a.value = v0; a.interp = Interp::Linear;
            CurveKey b;
            b.time = t1; b.value = v1;
            c.keys = {a, b};
            return c;
        };

        // ---- 空 / 单帧 ----
        {
            Curve c;
            report("curve_empty", evaluateCurve(c, 3.0f), 0.0, 1e-9);
            Curve one;
            one.keys.push_back(CurveKey{});
            one.keys[0].time = 2.0f;
            one.keys[0].value = 7.5f;
            report("curve_single_key", evaluateCurve(one, 99.0f), 7.5, 1e-9);
        }

        // ---- Constant / Linear ----
        {
            Curve c = mk(0.0f, 0.0f, 1.0f, 10.0f);
            report("curve_linear_mid", evaluateCurve(c, 0.5f), 5.0, 1e-6);
            c.keys[0].interp = Interp::Constant;
            // 0.99 仍在第一段里 → 必须还是 0（"保持"，不是"快到终点了"）
            report("curve_constant_hold", evaluateCurve(c, 0.99f), 0.0, 1e-9);
            report("curve_constant_at_end", evaluateCurve(c, 1.0f), 10.0, 1e-9);
        }

        // ---- 贝塞尔 ----
        {
            Curve c = mk(0.0f, 0.0f, 1.0f, 1.0f);
            c.keys[0].interp = Interp::Bezier;
            // ⚠⚠ 必须显式声明 Free：`CurveKey` 的默认手柄类型是
            //    `AutoClamped`，那种手柄**会盖掉**你写进 `outHandle` 的值
            //    （由邻居斜率现算）。2 帧 0→1 的自动斜率是 1，算出来的
            //    手柄 (1/3, 1/3) 让这段退化成直线 —— 第一版就在这里红过一次，
            //    而那恰恰是对的：Blender 里 Auto 手柄也不认你拖的值。
            c.keys[0].handle = HandleType::Free;
            c.keys[1].handle = HandleType::Free;
            c.keys[0].outHandle = glm::vec2(1.0f / 3.0f, 0.0f);
            c.keys[1].inHandle = glm::vec2(-1.0f / 3.0f, 0.0f);
            // 显式手柄 (1/3,0)/(-1/3,0) 在 x 上恰好是均匀参数化，
            // 于是 y(u) = 3u²−2u³ = smoothstep 本尊。这条同时钉住了
            // "贝塞尔求根没算错"和"默认手感与老 smooth 一致"。
            report("curve_bezier_free_is_smoothstep",
                   evaluateCurve(c, 0.25f), 0.15625, 1e-6);
            report("curve_bezier_mid", evaluateCurve(c, 0.5f), 0.5, 1e-6);

            // 对照：同一组数值换成 AutoClamped → 退化成直线（0.25）。
            // 这两条**必须分得开**，否则"手柄类型到底有没有生效"就测不出来。
            c.keys[0].handle = HandleType::AutoClamped;
            c.keys[1].handle = HandleType::AutoClamped;
            report("curve_bezier_auto_is_line", evaluateCurve(c, 0.25f), 0.25, 1e-6);

            // 手柄拉到对角 → 退化成直线
            report("curve_bezier_straight_handles",
                   bezierWeight(0.3f, glm::vec2(0.5f, 0.5f),
                                glm::vec2(-0.5f, -0.5f)),
                   0.3, 1e-6);

            // x 单调性：默认手柄下扫 64 个采样点，值不许回头
            bool monotonic = true;
            float prev = -1.0f;
            for (int i = 0; i <= 64; ++i) {
                const float v = evaluateCurve(c, static_cast<float>(i) / 64.0f);
                if (v < prev - 1e-6f) monotonic = false;
                prev = v;
            }
            reportFlag("curve_bezier_monotonic", monotonic, "no_reversal");
        }

        // ---- 外推 ----
        {
            Curve c = mk(0.0f, 0.0f, 1.0f, 1.0f);
            report("curve_extrap_hold_after", evaluateCurve(c, 5.0f), 1.0, 1e-9);
            c.after = Extrapolate::Linear;
            report("curve_extrap_linear", evaluateCurve(c, 1.5f), 1.5, 1e-6);
            c.after = Extrapolate::Cycle;
            report("curve_extrap_cycle", evaluateCurve(c, 1.25f), 0.25, 1e-6);
            c.after = Extrapolate::CycleOffset;
            report("curve_extrap_cycle_offset", evaluateCurve(c, 1.25f), 1.25, 1e-6);
            c.after = Extrapolate::CycleFlipped;
            // 奇数圈绕两端中点 (0.5) 镜像：0.5 - (0.25 - 0.5) = 0.75
            report("curve_extrap_cycle_flipped", evaluateCurve(c, 1.25f), 0.75, 1e-6);
        }

        // ---- 缓动 ----
        {
            report("ease_smooth_inout", easeFn(EaseShape::Smooth, EaseMode::InOut,
                                               0.25f),
                   0.15625, 1e-6);
            report("ease_quad_in", easeFn(EaseShape::Quad, EaseMode::In, 0.5f), 0.25,
                   1e-6);
            report("ease_quad_out", easeFn(EaseShape::Quad, EaseMode::Out, 0.5f),
                   0.75, 1e-6);
            report("ease_bounce_out_end",
                   easeFn(EaseShape::Bounce, EaseMode::Out, 1.0f), 1.0, 1e-9);
            // Back 的意义就是**过冲**：Out 在接近终点时会越过 1
            reportFlag("ease_back_overshoot",
                       easeFn(EaseShape::Back, EaseMode::Out, 0.9f) > 1.0f,
                       "v=" + std::to_string(easeFn(EaseShape::Back,
                                                    EaseMode::Out, 0.9f)));
            // None 必须是**逐位恒等**（所有"没配缓动"的段都靠它）
            reportFlag("ease_none_identity",
                       easeFn(EaseShape::Cubic, EaseMode::None, 0.37f) == 0.37f,
                       "bitwise_equal");
        }

        // ---- 自动手柄 ----
        {
            Curve c;
            CurveKey a; a.time = 0.0f; a.value = 0.0f; a.handle = HandleType::AutoClamped;
            CurveKey b; b.time = 1.0f; b.value = 10.0f; b.handle = HandleType::AutoClamped;
            // ⚠ 第三个值取 5 而不是 0：**对称的峰连"Auto"的中心差分也是 0**，
            //   那样这条断言就分不出 Auto 和 AutoClamped 了（第一版就栽在这）。
            CurveKey d; d.time = 2.0f; d.value = 5.0f; d.handle = HandleType::AutoClamped;
            c.keys = {a, b, d};
            // 中间那个是局部极值（先上后下）→ AutoClamped 必须压平
            report("handle_autoclamped_extremum_flat",
                   resolveOutHandle(c, 1).y, 0.0, 1e-9);
            c.keys[1].handle = HandleType::Auto;
            // Auto 不压平：斜率 = (10 + (−5)) / 2 = 2.5，手柄 y = 2.5/3
            report("handle_auto_extremum_not_flat",
                   resolveOutHandle(c, 1).y, 2.5f / 3.0f, 1e-6);

            // Vector 手柄指向下一帧 → 该段是直线
            Curve line = mk(0.0f, 0.0f, 1.0f, 5.0f);
            line.keys[0].handle = HandleType::Vector;
            const glm::vec2 oh = resolveOutHandle(line, 0);
            reportFlag("handle_vector_points_next",
                       std::fabs(oh.x - 1.0f) < 1e-6f && std::fabs(oh.y - 5.0f) < 1e-6f,
                       "h=" + std::to_string(oh.x) + "," + std::to_string(oh.y));
        }

        // ---- 段定位 ----
        {
            Curve c = mk(0.0f, 0.0f, 2.0f, 4.0f);
            report("segment_index_before", segmentIndexAt(c, -1.0f), -1.0, 0.0);
            report("segment_index_first", segmentIndexAt(c, 0.5f), 0.0, 0.0);
            report("segment_index_last", segmentIndexAt(c, 9.0f), 1.0, 0.0);
        }

        // ---- 两条入口共用同一份段内求值 ----
        // `sampleCamera` 走的是 `evaluateSegmentEase`（不分配容器），
        // 图编辑器/骨骼轨道走的是 `evaluateCurve`（容器）。两者必须是
        // **同一个结果**，否则同一段缓动在两个地方长得不一样。
        {
            Curve c = mk(0.0f, 0.0f, 1.0f, 1.0f);
            c.keys[0].ease = EaseMode::InOut;
            c.keys[0].shape = EaseShape::Smooth;
            const float viaCurve = evaluateCurve(c, 0.25f);
            const float viaSeg =
                evaluateSegmentEase(Interp::Linear, EaseMode::InOut,
                                    EaseShape::Smooth, glm::vec2(0.0f),
                                    glm::vec2(0.0f), 0.25f);
            reportFlag("curve_two_entries_agree", viaCurve == viaSeg,
                       "a=" + std::to_string(viaCurve) +
                           ",b=" + std::to_string(viaSeg));
        }
    }

    // ============================================================
    // Dope Sheet 批量操作（R8）
    // ============================================================
    //
    // 这一组只测**纯数据**：挪 / 镜像 / 对齐循环的几何性质，外加"类型
    // 不影响求值"。像素↔时间、命中判定、缩放锚点那些属 UI，由
    // `tools/verify_dope_sheet.py` 验。
    //
    // ⚠ 为什么有些断言用 == 而有些只能用容差：float 加一个常数**不保
    //   "间距不变"** —— `(a+d) − (b+d)` 与 `a − b` 在换指数时舍入不同。
    //   所以这里挑的是二进制精确的数（0.125 的整数倍 + 0.25 的位移），
    //   让"间距逐位不变"成为可断言的事实；真实资产的 1/30 秒时刻由
    //   Python 侧按 1e-6 容差验。
    {
        // ---- 挪：10 帧整体 +0.25s ----
        EditClip c;
        c.duration = 1.2f;
        std::vector<float> before;
        for (int i = 0; i < 10; ++i) {
            const float t = 0.125f * static_cast<float>(i);
            before.push_back(t);
            c.setKey("hips", assets::AnimTrack::Rotation, t,
                     glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        }
        reportFlag("dope_setup_keys", c.totalKeys() == 10,
                   "keys=" + std::to_string(c.totalKeys()));

        std::vector<KeyRef> sel = allKeys(c);
        const BatchStats mv = moveKeys(c, sel, 0.25f);
        reportFlag("dope_move_count",
                   mv.moved == 10 && mv.merged == 0 && mv.dropped == 0,
                   "moved=" + std::to_string(mv.moved) +
                       ",merged=" + std::to_string(mv.merged));

        int shifted = 0;
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (c.tracks[0].times[i] == before[i] + 0.25f) ++shifted;
        }
        reportFlag("dope_move_shift_bitwise", shifted == 10,
                   "exact=" + std::to_string(shifted));

        int spacing = 0;
        for (std::size_t i = 1; i < before.size(); ++i) {
            if ((c.tracks[0].times[i] - c.tracks[0].times[i - 1]) ==
                (before[i] - before[i - 1]))
                ++spacing;
        }
        reportFlag("dope_move_spacing_bitwise", spacing == 9,
                   "pairs=" + std::to_string(spacing));

        // 选中集必须被改写成**新下标** —— 否则"松手之后还选着那 10 帧"
        // 就是假的，下一次 G 会挪到别的帧上。
        const bool selOk =
            sel.size() == 10 && sel[3].key >= 0 &&
            sel[3].key < c.tracks[0].keyCount() &&
            c.tracks[0].times[static_cast<std::size_t>(sel[3].key)] ==
                before[3] + 0.25f;
        reportFlag("dope_move_sel_refresh", selOk,
                   "n=" + std::to_string(sel.size()));

        // ---- 镜像：绕 1.0s ----
        EditClip m;
        for (int i = 0; i < 5; ++i)
            m.setKey("hips", assets::AnimTrack::Rotation, 0.5f * i,
                     glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        std::vector<float> mb(m.tracks[0].times.begin(), m.tracks[0].times.end());
        std::vector<KeyRef> ms = allKeys(m);
        mirrorKeys(m, ms, 1.0f);
        int sym = 0;
        for (std::size_t i = 0; i < mb.size(); ++i) {
            const float want =
                static_cast<float>(2.0 * 1.0 - static_cast<double>(
                                                     mb[mb.size() - 1 - i]));
            if (m.tracks[0].times[i] == want) ++sym;
        }
        reportFlag("dope_mirror_symmetric", sym == 5,
                   "match=" + std::to_string(sym));

        // ---- 对齐循环：0/0.25/0.5/0.75 → 末帧 1.5 ----
        EditClip a;
        for (int i = 0; i < 4; ++i)
            a.setKey("hips", assets::AnimTrack::Rotation, 0.25f * i,
                     glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        std::vector<KeyRef> as = allKeys(a);
        alignCycle(a, as, 1.5f);
        reportFlag("dope_align_last_exact", a.tracks[0].times.back() == 1.5f,
                   "last=" + std::to_string(a.tracks[0].times.back()));
        report("dope_align_first_kept", a.tracks[0].times.front(), 0.0, 0.0);
        report("dope_align_mid", a.tracks[0].times[1], 0.5, 1e-7);

        // ---- 类型：不影响求值 + 能存能读 ----
        EditClip ty;
        ty.setKey("hips", assets::AnimTrack::Rotation, 0.0f,
                  glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        ty.setKey("hips", assets::AnimTrack::Rotation, 0.5f,
                  glm::vec4(0.0f, 0.3826834f, 0.0f, 0.9238795f));
        ty.setKey("hips", assets::AnimTrack::Rotation, 1.0f,
                  glm::vec4(0.0f, 0.7071068f, 0.0f, 0.7071068f));
        assets::AnimTrack probe;
        probe.joint = 0;
        probe.path = assets::AnimTrack::Rotation;
        probe.times = ty.tracks[0].times;
        probe.values = ty.tracks[0].values;
        const float kProbeT[4] = {0.1f, 0.3f, 0.55f, 0.9f};
        glm::vec4 snap0[4];
        for (int i = 0; i < 4; ++i) snap0[i] = assets::sampleTrack(probe, kProbeT[i]);
        std::vector<KeyRef> ts = allKeys(ty);
        setKeyTypes(ty, ts, KeyType::Extreme);
        int sameSample = 0;
        for (int i = 0; i < 4; ++i) {
            if (assets::sampleTrack(probe, kProbeT[i]) == snap0[i]) ++sameSample;
        }
        reportFlag("dope_types_do_not_change_sampling", sameSample == 4,
                   "same=" + std::to_string(sameSample));
        // 三次打帧之后再单独标一个：只有那一个变
        ty.tracks[0].setTypeAt(1, KeyType::Breakdown);
        reportFlag("dope_types_independent",
                   ty.tracks[0].typeAt(0) == KeyType::Extreme &&
                       ty.tracks[0].typeAt(1) == KeyType::Breakdown &&
                       ty.tracks[0].typeAt(2) == KeyType::Extreme,
                   "t0=" + std::to_string(static_cast<int>(ty.tracks[0].typeAt(0))) +
                       ",t1=" + std::to_string(static_cast<int>(ty.tracks[0].typeAt(1))));

        // 落盘往返（types 是**可选字段**，最容易被 save 漏掉）
        {
            std::string err;
            const std::string path =
                (std::filesystem::temp_directory_path() /
                 "venn_dope_selftest.anim.json").string();
            EditClip back;
            const bool saved = saveEditClip(path, ty, err);
            const bool loaded = saved && loadEditClip(path, back, err);
            const bool same =
                loaded && back.tracks.size() == ty.tracks.size() &&
                back.tracks[0].keyCount() == 3 &&
                back.tracks[0].typeAt(0) == KeyType::Extreme &&
                back.tracks[0].typeAt(1) == KeyType::Breakdown &&
                back.tracks[0].times == ty.tracks[0].times &&
                back.tracks[0].values == ty.tracks[0].values;
            reportFlag("dope_types_json_roundtrip", same,
                       "saved=" + std::to_string(saved ? 1 : 0) +
                           ",loaded=" + std::to_string(loaded ? 1 : 0));
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    // ============================================================
    // S-A：骨架作者（RigEdit）—— 2026-10-29
    //
    // 这几条钉的是**约定**，不是功能：
    //   · `rig_bake_identity` —— 没动过的骨必须逐位照抄（约定 ①）。
    //     这条一红，说明烘焙在"重建"矩阵，所有老资产都会以 1e-7 漂。
    //   · `rig_invbind` / `rig_rest_no_deform` —— 改 rest 必须重算 invBind
    //     （约定 ②），否则静止姿态下人就缩了，而日志一行都不报。
    //   · `rig_roll_*` —— roll 是"绕骨轴扭"，不是"改朝向"（约定 ③）。
    // ============================================================
    {
        // 夹具：5 根骨的半个上身。spine 的子骨 chest **正好在骨轴上**
        // （+Z），这样"绕轴 roll 不该挪动子骨头"才是可断言的。
        const auto makeRigSkel = []() {
            assets::Skeleton s;
            s.joints.resize(5);
            const glm::vec3 pos[5] = {
                {0.0f, 0.0f, 0.9f},    // hips（根）
                {0.0f, 0.0f, 1.1f},    // spine
                {0.0f, 0.0f, 1.3f},    // chest（在 spine 的轴上）
                {0.20f, 0.0f, 1.3f},   // upperarm.L
                {-0.20f, 0.0f, 1.3f},  // upperarm.R
            };
            const char* names[5] = {"hips", "spine", "chest", "upperarm.L",
                                    "upperarm.R"};
            const int parents[5] = {-1, 0, 1, 2, 2};
            for (int i = 0; i < 5; ++i) {
                s.joints[i].name = names[i];
                s.joints[i].parent = parents[i];
                const glm::vec3 local =
                    (i == 0) ? pos[0] : (pos[i] - pos[parents[i]]);
                s.joints[i].restLocal = glm::translate(glm::mat4(1.0f), local);
            }
            s.topoOrder = {0, 1, 2, 3, 4};
            s.invMeshNode = glm::mat4(1.0f);
            std::vector<glm::mat4> w(5, glm::mat4(1.0f));
            for (int i : s.topoOrder) {
                const glm::mat4 base = s.joints[i].parent >= 0
                                           ? w[s.joints[i].parent]
                                           : s.joints[i].externalChain;
                w[i] = base * s.joints[i].restLocal;
            }
            for (int i = 0; i < 5; ++i) s.joints[i].invBind = glm::inverse(w[i]);
            return s;
        };

        const auto nearV = [](const glm::vec3& a, const glm::vec3& b, float eps) {
            return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
                   std::fabs(a.z - b.z) <= eps;
        };
        const auto nearIdent = [](const glm::mat4& m, float eps) {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) {
                    const float want = (c == r) ? 1.0f : 0.0f;
                    if (std::fabs(m[c][r] - want) > eps) return false;
                }
            return true;
        };
        const auto sameMats = [](const assets::Skeleton& a, const assets::Skeleton& b) {
            if (a.jointCount() != b.jointCount()) return false;
            for (int i = 0; i < a.jointCount(); ++i) {
                if (a.joints[i].name != b.joints[i].name) return false;
                if (a.joints[i].parent != b.joints[i].parent) return false;
                if (std::memcmp(&a.joints[i].restLocal, &b.joints[i].restLocal,
                                sizeof(glm::mat4)) != 0) return false;
                if (std::memcmp(&a.joints[i].invBind, &b.joints[i].invBind,
                                sizeof(glm::mat4)) != 0) return false;
            }
            return true;
        };

        const assets::Skeleton skel = makeRigSkel();
        const animation::RigEdit rig0 = animation::RigEdit::fromSkeleton(skel);

        // ---- A1 导入：层级、头位置、几何推断 ----
        reportFlag("rig_import_bones",
                   rig0.boneCount() == 5 && rig0.bones[1].parent == 0 &&
                       rig0.bones[2].parent == 1 && rig0.bones[3].parent == 2 &&
                       rig0.bones[0].parent == -1,
                   "n=" + std::to_string(rig0.boneCount()));
        reportFlag("rig_import_heads",
                   nearV(rig0.bones[0].head, glm::vec3(0, 0, 0.9f), 1e-6f) &&
                       nearV(rig0.bones[2].head, glm::vec3(0, 0, 1.3f), 1e-6f) &&
                       nearV(rig0.bones[3].head, glm::vec3(0.2f, 0, 1.3f), 1e-6f),
                   "");
        // spine 的子骨 chest 在 +Z 上：骨长 = 0.2、朝向 = +Z
        reportFlag("rig_import_geometry",
                   nearV(rig0.bones[1].dir, glm::vec3(0, 0, 1), 1e-6f) &&
                       std::fabs(rig0.bones[1].length - 0.2f) < 1e-6f,
                   "len=" + std::to_string(rig0.bones[1].length));
        // 叶骨（upperarm）没有子骨：沿父→自己的方向，长度取一半
        reportFlag("rig_import_leaf",
                   nearV(rig0.bones[3].dir, glm::vec3(1, 0, 0), 1e-6f) &&
                       std::fabs(rig0.bones[3].length - 0.1f) < 1e-6f,
                   "len=" + std::to_string(rig0.bones[3].length));

        // ---- A2 零编辑烘焙 = 逐位照抄（约定 ①）----
        assets::Skeleton baked0;
        rig0.bake(skel, baked0);
        reportFlag("rig_bake_identity", sameMats(skel, baked0), "");

        // ---- A3 JSON 往返之后仍然逐位一致（矩阵不能只存 4 位小数）----
        {
            const std::string path =
                (std::filesystem::temp_directory_path() / "venn_rig_selftest.rig.json")
                    .string();
            std::string err;
            const bool saved = animation::saveRig(path, rig0, err);
            animation::RigEdit back;
            const bool loaded = animation::loadRig(path, back, err);
            assets::Skeleton bakedBack;
            if (loaded) back.bake(skel, bakedBack);
            reportFlag("rig_json_roundtrip",
                       saved && loaded && sameMats(skel, bakedBack),
                       "saved=" + std::to_string(saved ? 1 : 0) +
                           ",loaded=" + std::to_string(loaded ? 1 : 0));
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        // ---- A4 roll：绕骨轴扭，骨头不挪、子骨也不挪 ----
        {
            animation::RigEdit rig = rig0;
            rig.setRoll(1, 1.5707963267948966f);   // spine 绕 +Z 扭 90°
            std::vector<glm::mat4> w;
            rig.restWorld(w);
            const glm::vec3 ax = glm::normalize(glm::vec3(w[1][0]));   // 局部 X 轴
            const bool axisOk = nearV(ax, glm::vec3(0, 1, 0), 1e-5f);
            const bool headOk = nearV(glm::vec3(w[1][3]), glm::vec3(0, 0, 1.1f), 1e-5f);
            // chest 在轴上 → 扭它不挪位置（这正是"roll 只改坐标系"的判据）
            const bool childOk = nearV(glm::vec3(w[2][3]), glm::vec3(0, 0, 1.3f), 1e-5f);
            reportFlag("rig_roll_twists_axis", axisOk && headOk && childOk,
                       "ax=(" + std::to_string(ax.x) + "," + std::to_string(ax.y) +
                           "," + std::to_string(ax.z) + ")");

            // ---- A5 改 rest 之后 invBind 必须跟着变（约定 ②）----
            assets::Skeleton baked;
            rig.bake(skel, baked);
            reportFlag("rig_invbind_recomputed",
                       nearIdent(w[1] * baked.joints[1].invBind, 1e-5f) &&
                           nearIdent(w[2] * baked.joints[2].invBind, 1e-5f),
                       "");
            // 静止姿态采样：jointMat = invMeshNode·world·invBind ≈ I
            //（= 改了 rest 之后模型**不会**立刻缩/歪，只改变形坐标系）
            std::vector<glm::mat4> joints;
            baked.sample(nullptr, 0.0f, joints);
            reportFlag("rig_rest_no_deform",
                       joints.size() == 5 && nearIdent(joints[1], 1e-5f) &&
                           nearIdent(joints[3], 1e-5f),
                       "");
            // 只重算脏子树：
            //   · hips（roll 的**祖先**）整块逐位不动；
            //   · chest（roll 的**子孙**）restLocal 不动（父子相对关系没改），
            //     但 invBind 必须变（世界矩阵变了，约定 ②）。
            const bool rootSame =
                std::memcmp(&baked.joints[0].restLocal, &skel.joints[0].restLocal,
                            sizeof(glm::mat4)) == 0 &&
                std::memcmp(&baked.joints[0].invBind, &skel.joints[0].invBind,
                            sizeof(glm::mat4)) == 0;
            const bool childLocalSame =
                std::memcmp(&baked.joints[2].restLocal, &skel.joints[2].restLocal,
                            sizeof(glm::mat4)) == 0;
            const bool childBindDiff =
                std::memcmp(&baked.joints[2].invBind, &skel.joints[2].invBind,
                            sizeof(glm::mat4)) != 0;
            reportFlag("rig_bake_untouched_keeps",
                       rootSame && childLocalSame && childBindDiff,
                       "root=" + std::to_string(rootSame ? 1 : 0) +
                           ",local=" + std::to_string(childLocalSame ? 1 : 0) +
                           ",bind=" + std::to_string(childBindDiff ? 1 : 0));
        }

        // ---- A6 Connected：改父骨尾，子骨的头跟着走 ----
        {
            animation::RigEdit rig = rig0;
            rig.setConnected(1, true);
            rig.setTail(0, glm::vec3(0, 0, 1.15f));
            reportFlag("rig_connected_follows",
                       nearV(rig.bones[1].head, glm::vec3(0, 0, 1.15f), 1e-5f),
                       "");
            // 断开之后就不再跟着
            rig.setConnected(1, false);
            rig.setTail(0, glm::vec3(0, 0, 1.05f));
            reportFlag("rig_connected_off",
                       nearV(rig.bones[1].head, glm::vec3(0, 0, 1.15f), 1e-5f), "");
        }

        // ---- A7 加骨 / 删骨 / 重名 ----
        {
            animation::RigEdit rig = rig0;
            const int idx = rig.addBone("weapon.R", 4, glm::vec3(-0.2f, 0.2f, 1.3f),
                                        glm::vec3(0, 1, 0), 0.25f);
            std::vector<glm::mat4> w;
            rig.restWorld(w);
            const bool placed = idx == 5 && w.size() == 6 &&
                                nearV(glm::vec3(w[5][3]), glm::vec3(-0.2f, 0.2f, 1.3f), 1e-6f);
            const bool dup = rig.addBone("hips", -1, glm::vec3(0), glm::vec3(0, 0, 1),
                                         0.1f) == -1;
            const int removed = rig.removeBone(5);
            reportFlag("rig_add_remove",
                       placed && dup && removed == 1 && rig.boneCount() == 5,
                       "idx=" + std::to_string(idx) + ",rm=" + std::to_string(removed));
            // 删父骨连带子孙：删 chest 应该带走两条手臂
            animation::RigEdit rig2 = rig0;
            const int n2 = rig2.removeBone(2);
            reportFlag("rig_remove_subtree", n2 == 3 && rig2.boneCount() == 2,
                       "removed=" + std::to_string(n2));
        }

        // ---- A8 左右镜像（关于 X=0 平面）----
        {
            animation::RigEdit rig = rig0;
            rig.setRoll(3, 0.6f);
            const int k = rig.mirrorToOpposite(3);
            reportFlag("rig_mirror_symmetric",
                       k == 4 && nearV(rig.bones[4].head, glm::vec3(-0.2f, 0, 1.3f), 1e-6f) &&
                           nearV(rig.bones[4].dir, glm::vec3(-1, 0, 0), 1e-6f) &&
                           rig.bones[4].roll == -0.6f,
                       "k=" + std::to_string(k));
            // 没有左右标记的骨不猜
            reportFlag("rig_mirror_skips_unnamed",
                       rig.mirrorToOpposite(1) == -1 &&
                           animation::RigEdit::flipSide("spine").empty(),
                       "");
        }

        // ---- A9 标志位 ----
        {
            animation::RigEdit rig = rig0;
            rig.setDeform(2, false);
            rig.setInheritRot(3, false);
            const animation::RigInfo s = rig.info();
            reportFlag("rig_flags", s.deformOff == 1 && !rig.bones[3].inheritRot &&
                                        s.bones == 5 && s.roots == 1,
                       "roots=" + std::to_string(s.roots));
        }
    }

    // ============================================================
    // 十、S-B 权重：顶点组 / 自动权重 / 烘焙 / 镜像 / 平滑
    // ============================================================
    //
    // 与 A 组（骨架）的关系：这一组**只用** `deform` 标志，不碰 rest。
    // 三条被钉住的不变量（见 WeightSet.h 头注释）：
    //   ① 零编辑烘焙逐位一致  —— B2
    //   ② 每顶点权重和恒为 1  —— B6 / B11
    //   ③ 只给 deform 骨算权重 —— B7
    // 这三条只要有一条松了，画面上都只是"角色动起来有点怪"，日志干净。
    {
        VK_LOG_INFO("--- S-B 权重：顶点组 / 自动权重 / 烘焙 / 镜像 ---");

        // ---- 夹具一：4 根骨（含一对 .L/.R，用来验镜像）----
        // 全都不从导入骨架来（srcJoint = -1）→ 恒 touched → restWorld 直接
        // 由 head/dir/length 算，不依赖任何导入产物。
        const auto makeWeightRig = []() {
            animation::RigEdit rig;
            rig.bones.resize(4);
            const char* names[4] = {"spine", "chest", "arm.L", "arm.R"};
            const int parents[4] = {-1, 0, 1, 1};
            const glm::vec3 head[4] = {{0.0f, 0.0f, 0.0f},
                                       {0.0f, 0.0f, 0.5f},
                                       {0.02f, 0.0f, 1.0f},
                                       {-0.02f, 0.0f, 1.0f}};
            const float len[4] = {0.5f, 0.5f, 0.3f, 0.3f};
            for (int i = 0; i < 4; ++i) {
                animation::EditBone& b = rig.bones[static_cast<std::size_t>(i)];
                b.name = names[i];
                b.parent = parents[i];
                b.srcJoint = -1;
                b.head = head[i];
                b.dir = glm::vec3(0.0f, 0.0f, 1.0f);
                b.length = len[i];
                b.deform = true;
            }
            return rig;
        };

        // ---- 夹具二：8 个顶点的"人形柱子" ----
        //   v0..v5 沿骨轴从 0.10 排到 1.35（跨越三根骨的包络）
        //   v6/v7 是关于 X 轴镜像的一对（±0.05），专门给镜像断言用
        const auto makeWeightVerts = []() {
            const float zs[6] = {0.10f, 0.35f, 0.60f, 0.85f, 1.10f, 1.35f};
            std::vector<assets::Vertex> vs(8);
            for (int i = 0; i < 6; ++i) vs[static_cast<std::size_t>(i)].pos =
                glm::vec3(0.0f, 0.0f, zs[i]);
            vs[6].pos = glm::vec3(0.05f, 0.0f, 0.60f);
            vs[7].pos = glm::vec3(-0.05f, 0.0f, 0.60f);
            return vs;
        };
        // 三角面（给邻接/平滑用）：0-1-2 / 2-3-4 / 4-5-6 / 1-6-7
        const std::vector<uint32_t> wIdx = {0, 1, 2, 2, 3, 4, 4, 5, 6, 1, 6, 7};

        const animation::RigEdit wrig = makeWeightRig();
        std::vector<glm::mat4> wworld;
        wrig.restWorld(wworld);
        // A 组那个 nearV 在它自己的块里，这里要一份（块作用域不外泄）
        const auto nearV = [](const glm::vec3& a, const glm::vec3& b, float eps) {
            return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps &&
                   std::fabs(a.z - b.z) <= eps;
        };
        const bool wworldOk = wworld.size() == 4 &&
                              nearV(glm::vec3(wworld[2][3]), glm::vec3(0.02f, 0, 1.0f), 1e-6f);

        // ---- B1 导入：零权重尾巴不留 ----
        {
            std::vector<assets::Vertex> vs = makeWeightVerts();
            // v0：导入器常见的"带三条零尾巴"写法
            vs[0].joints = glm::uvec4(2u, 0u, 0u, 0u);
            vs[0].weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            // v1：两条真权重
            vs[1].joints = glm::uvec4(0u, 1u, 0u, 0u);
            vs[1].weights = glm::vec4(0.6f, 0.4f, 0.0f, 0.0f);
            const animation::WeightSet ws =
                animation::WeightSet::fromVertices(vs, "selftest");
            reportFlag("weights_import_slots",
                       ws.vertexCount == 8 && ws.influenceCount(0) == 1 &&
                           ws.influenceCount(1) == 2 && ws.influenceCount(2) == 0 &&
                           ws.weightOf(0, 2) == 1.0f && ws.weightOf(1, 1) == 0.4f,
                       "n0=" + std::to_string(ws.influenceCount(0)) + ",n1=" +
                           std::to_string(ws.influenceCount(1)));
        }

        // ---- B2 零编辑烘焙 ≡ 导入路径（约定 ①，逐位）----
        {
            std::vector<assets::Vertex> vs = makeWeightVerts();
            vs[0].joints = glm::uvec4(2u, 0u, 0u, 0u);
            vs[0].weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            vs[1].joints = glm::uvec4(0u, 1u, 0u, 0u);
            vs[1].weights = glm::vec4(0.6f, 0.4f, 0.0f, 0.0f);
            const std::vector<assets::Vertex> orig = vs;
            const animation::WeightSet ws =
                animation::WeightSet::fromVertices(vs, "selftest");
            // 先把槽位写成垃圾，再让 bake 恢复 —— 只断言"结果相同"会漏掉
            // "bake 根本没写"这种情况（垃圾正好等于原值是不可能的，但如果
            // bake 直接 early-out，槽位就保持垃圾，断言立刻红）。
            for (assets::Vertex& v : vs) {
                v.joints = glm::uvec4(9u, 9u, 9u, 9u);
                v.weights = glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f);
            }
            const int changed = ws.bake(vs.data(), nullptr);
            bool same = true;
            for (std::size_t i = 0; i < vs.size() && same; ++i) {
                same = std::memcmp(&vs[i].joints, &orig[i].joints,
                                   sizeof(glm::uvec4)) == 0 &&
                       std::memcmp(&vs[i].weights, &orig[i].weights,
                                   sizeof(glm::vec4)) == 0;
            }
            reportFlag("weights_bake_identity", same && changed == 8,
                       "changed=" + std::to_string(changed) + ",same=" +
                           std::to_string(same ? 1 : 0));
        }

        // ---- B3 动过的顶点：降序取前四 + 归一化 ----
        {
            animation::WeightSet ws =
                animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
            ws.setWeight(2, 1, 0.2f);
            ws.setWeight(2, 3, 0.9f);
            ws.setWeight(2, 0, 0.5f);
            std::vector<assets::Vertex> vs = makeWeightVerts();
            ws.bake(vs.data(), nullptr);
            const float sum = vs[2].weights.x + vs[2].weights.y + vs[2].weights.z +
                              vs[2].weights.w;
            reportFlag("weights_bake_sorted",
                       std::fabs(sum - 1.0f) <= 1e-6f &&
                           vs[2].joints.x == 3u && vs[2].joints.y == 0u &&
                           vs[2].joints.z == 1u && vs[2].weights.z > 0.0f,
                       "sum=" + std::to_string(sum) + ",j0=" +
                           std::to_string(vs[2].joints.x));
        }

        // ---- B4 刷子的三种模式 + 自动归一 ----
        {
            animation::WeightSet ws =
                animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
            const std::vector<int> hit = {0, 1, 2};
            const int a = ws.paint(hit, 0, 0.6f, animation::kPaintAdd);
            const float afterAdd = ws.weightOf(0, 0);
            const int s2 = ws.paint(hit, 0, 0.25f, animation::kPaintSubtract);
            const float afterSub = ws.weightOf(0, 0);
            const int s3 = ws.paint(hit, 3, 0.75f, animation::kPaintSet);
            const float sum0 = ws.weightOf(0, 0) + ws.weightOf(0, 3);
            reportFlag("weights_paint_modes",
                       a == 3 && s2 == 3 && s3 == 3 &&
                           std::fabs(afterAdd - 1.0f) <= 1e-6f &&
                           std::fabs(afterSub - 1.0f) <= 1e-6f &&
                           std::fabs(sum0 - 1.0f) <= 1e-6f,
                       "add=" + std::to_string(afterAdd) + ",sub=" +
                           std::to_string(afterSub) + ",sum=" + std::to_string(sum0));
        }

        // ---- B5 同一批操作跑两遍 == 同一个摘要（全序排序的意义）----
        {
            const auto runOnce = [&]() {
                animation::WeightSet ws =
                    animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
                ws.setWeight(3, 1, 0.5f);
                ws.setWeight(3, 2, 0.5f);   // 与上一条**权重相等**：排序必须定序
                ws.setWeight(3, 0, 0.25f);
                ws.normalizeAll();
                return ws.digest();
            };
            const unsigned long long h1 = runOnce();
            const unsigned long long h2 = runOnce();
            reportFlag("weights_digest_stable", h1 == h2,
                       "h=" + std::to_string(h1));
        }

        // ---- B6 自动权重：和恒为 1、无孤立顶点（约定 ②）----
        animation::WeightSet autoWs =
            animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
        {
            std::vector<glm::vec3> pos;
            for (const assets::Vertex& v : makeWeightVerts()) pos.push_back(v.pos);
            animation::AutoWeightParams p;
            const int assigned =
                autoWs.autoAssign(wrig, pos, glm::mat4(1.0f), p);
            const animation::WeightStats st = autoWs.stats();
            reportFlag("weights_auto_sum_one",
                       wworldOk && assigned == 8 && st.unweighted == 0 &&
                           st.minSum >= 1.0f - 1e-5f && st.maxSum <= 1.0f + 1e-5f,
                       "assigned=" + std::to_string(assigned) + ",unw=" +
                           std::to_string(st.unweighted) + ",sum=[" +
                           std::to_string(st.minSum) + "," +
                           std::to_string(st.maxSum) + "]");
        }

        // ---- B7 只给 deform 骨算权重（约定 ③）----
        {
            animation::RigEdit rig = makeWeightRig();
            rig.setDeform(1, false);   // chest 关掉
            animation::WeightSet ws =
                animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
            std::vector<glm::vec3> pos;
            for (const assets::Vertex& v : makeWeightVerts()) pos.push_back(v.pos);
            animation::AutoWeightParams p;
            ws.autoAssign(rig, pos, glm::mat4(1.0f), p);
            float chestMax = 0.0f;
            int verticesWithChest = 0;
            for (int v = 0; v < ws.vertexCount; ++v) {
                const float w = ws.weightOf(v, 1);
                if (w > 0.0f) ++verticesWithChest;
                chestMax = std::max(chestMax, w);
            }
            // 反向对照：不关的话它一定有权重（否则这条断言是空转）
            float chestMaxOn = 0.0f;
            for (int v = 0; v < autoWs.vertexCount; ++v)
                chestMaxOn = std::max(chestMaxOn, autoWs.weightOf(v, 1));
            reportFlag("weights_auto_only_deform",
                       chestMax == 0.0f && verticesWithChest == 0 &&
                           chestMaxOn > 0.1f,
                       "off=" + std::to_string(chestMax) + ",on=" +
                           std::to_string(chestMaxOn));
        }

        // ---- B8 最近的那根骨拿到的权重最大 ----
        {
            // v0 在 z=0.10（spine 的中段），v4 在 z=1.10（arm 的中段）
            const bool lowOnSpine = autoWs.weightOf(0, 0) > autoWs.weightOf(0, 1);
            const bool highOnArm = autoWs.weightOf(4, 2) + autoWs.weightOf(4, 3) > 0.5f;
            reportFlag("weights_auto_nearest",
                       lowOnSpine && highOnArm,
                       "v0.spine=" + std::to_string(autoWs.weightOf(0, 0)) +
                           ",v4.arms=" +
                           std::to_string(autoWs.weightOf(4, 2) +
                                          autoWs.weightOf(4, 3)));
        }

        // ---- B9 镜像：权重跟着骨名换边（用 S-A 认出来的镜像轴）----
        {
            animation::WeightSet ws =
                animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
            ws.setWeight(6, 2, 0.8f);   // v6(+x) → arm.L
            ws.setWeight(6, 1, 0.2f);
            std::vector<glm::vec3> pos;
            for (const assets::Vertex& v : makeWeightVerts()) pos.push_back(v.pos);
            // 镜像轴 X（这两个顶点只有 x 相反）
            const int k = ws.mirrorVertex(wrig, 0, pos, 6, 0.01f);
            const float mirrored = ws.weightOf(7, 3);
            // 骨名没有 .L/.R → 对面那半边无从谈起，必须**拒绝**而不是猜。
            // ⚠ 用 v6/v7 这对（它们是这具网格里唯一真对称的一对）：拿一个
            //   本来就没有镜像伙伴的顶点去断言，返回 -1 是"没找到伙伴"，
            //   和"骨名没标记"混在一起，断言就成了空转。
            animation::WeightSet ws2 =
                animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
            ws2.setWeight(6, 1, 1.0f);   // chest：没有左右标记
            const bool noSideNoMirror = ws2.mirrorVertex(wrig, 0, pos, 6, 0.01f) == -1;
            reportFlag("weights_mirror_vertex",
                       k == 7 && std::fabs(mirrored - ws.weightOf(6, 2)) <= 1e-6f &&
                           ws.weightOf(7, 2) == 0.0f && noSideNoMirror,
                       "k=" + std::to_string(k) + ",w=" + std::to_string(mirrored) +
                           ",noSide=" + std::to_string(noSideNoMirror ? 1 : 0));
        }

        // ---- B10 平滑：权重扩散到 1-ring 邻居，且和仍是 1 ----
        {
            animation::WeightSet ws =
                animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
            // 种**两条**骨而不是一条：单条骨归一化之后恒等于 1.0，
            // 那样"平滑有没有真的把两边的比例带过去"根本看不出来
            //（第一版就是这么写的，断言只能证明"邻居多了一条骨"）。
            ws.setWeight(2, 1, 0.6f);
            ws.setWeight(2, 0, 0.4f);
            std::vector<int> off;
            std::vector<int> ring;
            animation::buildVertexRing(wIdx, ws.vertexCount, off, ring);
            const int before = ws.influenceCount(1);
            const int n = ws.blur(off, ring, 1, 0.5f);
            const float nb = ws.weightOf(1, 1);   // v1 的邻居里有 v2
            const float ns = ws.weightOf(1, 0);
            const animation::WeightStats st = ws.stats();
            reportFlag("weights_blur_spreads",
                       before == 0 && n >= 2 && ws.influenceCount(1) == 2 &&
                           std::fabs(nb - 0.6f) <= 1e-4f &&
                           std::fabs(ns - 0.4f) <= 1e-4f,
                       "n=" + std::to_string(n) + ",nb=" + std::to_string(nb) +
                           ",ns=" + std::to_string(ns));
            reportFlag("weights_blur_keeps_sum",
                       st.unweighted <= ws.vertexCount - 2 &&
                           st.maxSum <= 1.0f + 1e-5f && st.minSum >= 1.0f - 1e-5f,
                       "sum=[" + std::to_string(st.minSum) + "," +
                           std::to_string(st.maxSum) + "]");
        }

        // ---- B11 摘骨之后剩下的重新归一化 ----
        {
            animation::WeightSet ws =
                animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
            ws.setWeight(0, 0, 0.5f);
            ws.setWeight(0, 1, 0.5f);
            const int cleared = ws.clearBone(0);
            const animation::WeightStats st = ws.stats();
            reportFlag("weights_clear_bone",
                       cleared == 1 && ws.weightOf(0, 0) == 0.0f &&
                           std::fabs(ws.weightOf(0, 1) - 1.0f) <= 1e-6f &&
                           st.unweighted == ws.vertexCount - 1,
                       "cleared=" + std::to_string(cleared) + ",sum=" +
                           std::to_string(ws.weightOf(0, 1)));
        }

        // ---- B12 1-ring 邻接：对称、不自环 ----
        {
            std::vector<int> off;
            std::vector<int> ring;
            animation::buildVertexRing(wIdx, 8, off, ring);
            const auto hasNb = [&](int a, int b) {
                for (int k = off[static_cast<std::size_t>(a)];
                     k < off[static_cast<std::size_t>(a) + 1]; ++k)
                    if (ring[static_cast<std::size_t>(k)] == b) return true;
                return false;
            };
            bool sym = true;
            bool self = false;
            for (int a = 0; a < 8; ++a)
                for (int k = off[static_cast<std::size_t>(a)];
                     k < off[static_cast<std::size_t>(a) + 1]; ++k) {
                    const int b = ring[static_cast<std::size_t>(k)];
                    if (b == a) self = true;
                    if (!hasNb(b, a)) sym = false;
                }
            reportFlag("weights_ring_sym", sym && !self &&
                                               off[8] > 8 && hasNb(0, 1) && hasNb(1, 0),
                       "n=" + std::to_string(off[8]));
        }

        // ---- B14 mask 的作用范围：只重算"真的用到被关掉那根骨"的顶点 ----
        {
            std::vector<assets::Vertex> vs(2);
            vs[0].joints = glm::uvec4(0u, 0u, 0u, 0u);
            vs[0].weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            vs[1].joints = glm::uvec4(0u, 1u, 0u, 0u);
            vs[1].weights = glm::vec4(0.5f, 0.5f, 0.0f, 0.0f);
            const animation::WeightSet ws =
                animation::WeightSet::fromVertices(vs, "selftest");
            const std::vector<char> mask = {1, 0};   // 骨 1 关掉
            // 打乱，证明 bake 真的写了（不然"没变"也可能只是没写）
            vs[0].joints = glm::uvec4(9u, 9u, 9u, 9u);
            vs[0].weights = glm::vec4(9.0f, 9.0f, 9.0f, 9.0f);
            const int c = ws.bake(vs.data(), &mask);
            // v0 只用骨 0（没被 mask）→ 逐位还原成导入原样
            const bool v0ok = vs[0].joints.x == 0u && vs[0].weights.x == 1.0f &&
                              vs[0].weights.y == 0.0f;
            // v1 用到骨 1（被 mask）→ 只剩骨 0 且归一
            const bool v1ok = vs[1].joints.x == 0u &&
                              std::fabs(vs[1].weights.x - 1.0f) <= 1e-6f &&
                              vs[1].weights.y == 0.0f;
            reportFlag("weights_bake_mask_scoped", v0ok && v1ok && c == 2,
                       "changed=" + std::to_string(c) + ",v0=" +
                           std::to_string(v0ok ? 1 : 0) + ",v1=" +
                           std::to_string(v1ok ? 1 : 0));
        }

        // ---- B13 JSON 往返 → 摘要不变 ----
        {
            animation::WeightSet ws =
                animation::WeightSet::fromVertices(makeWeightVerts(), "selftest");
            ws.setWeight(1, 0, 0.3f);
            ws.setWeight(1, 2, 0.7f);
            ws.setWeight(4, 1, 1.0f);
            const unsigned long long before = ws.digest();
            const std::string path =
                (std::filesystem::temp_directory_path() /
                 "venn_weights_selftest.weights.json")
                    .string();
            std::string err;
            const bool saved = animation::saveWeights(path, ws, err);
            animation::WeightSet back;
            const bool loaded = animation::loadWeights(path, back, err);
            const unsigned long long after = back.digest();
            reportFlag("weights_json_roundtrip",
                       saved && loaded && before == after &&
                           back.vertexCount == ws.vertexCount,
                       "saved=" + std::to_string(saved ? 1 : 0) + ",eq=" +
                           std::to_string(before == after ? 1 : 0));
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    // ================================================================
    // 十一、S-C 约束栈：顺序 / Influence / 限幅 / 复制 / ChildOf / IK 入栈
    // ================================================================
    //
    // 为什么这一组必须全是数值 + memcmp：
    //   "约束生效了"在截图上分不出"转了 20°"和"转了 35°"，而**顺序**这种
    //   语义连截图都看不出来 —— 两种顺序都"看起来像被约束了"。所以每条
    //   既要钉"结果对"，也要钉"该不动的时候一个字节都不动"。
    {
        // 夹具：root → spine → {armR, armL → weapon}
        // 两臂对称摆开，这样"复制旋转"与"镜像"能分开测（复制不该受对称性影响）。
        const auto makeConRig = []() {
            auto s = std::make_shared<assets::Skeleton>();
            s->joints.resize(5);
            const auto set = [&s](int i, const char* nm, int par, const glm::vec3& t) {
                s->joints[static_cast<size_t>(i)].name = nm;
                s->joints[static_cast<size_t>(i)].parent = par;
                s->joints[static_cast<size_t>(i)].restLocal =
                    glm::translate(glm::mat4(1.0f), t);
                s->joints[static_cast<size_t>(i)].invBind = glm::mat4(1.0f);
            };
            set(0, "root", -1, glm::vec3(0.0f));
            set(1, "spine", 0, glm::vec3(0.0f, 0.0f, 0.3f));
            set(2, "armR", 1, glm::vec3(0.2f, 0.0f, 0.2f));
            set(3, "armL", 1, glm::vec3(-0.2f, 0.0f, 0.2f));
            set(4, "weapon", 3, glm::vec3(0.0f, -0.1f, 0.0f));
            s->topoOrder = {0, 1, 2, 3, 4};
            s->invMeshNode = glm::mat4(1.0f);
            return s;
        };
        // 局部旋转 → 该骨 X 轴上的角度（度）。限幅的断言全用它。
        const auto eulerXDeg = [](const Pose& p, int i) {
            return static_cast<double>(glm::degrees(
                glm::eulerAngles(glm::normalize(p.rotation[static_cast<size_t>(i)])).x));
        };
        const auto worldRotOf = [](const std::vector<glm::mat4>& w, int i) {
            glm::mat3 r(w[static_cast<size_t>(i)]);
            const float s = glm::length(r[0]);
            if (s > 1.0e-8f) r /= s;
            return glm::normalize(glm::quat_cast(r));
        };
        const auto quatAngleDeg = [](const glm::quat& a, const glm::quat& b) {
            // glm 只有 `angle(q)`（单个四元数）→ 先取相对旋转再求角。
            // `angle()` 对 q 和 -q 会给不同的值，所以先做 hemisphere 对齐。
            glm::quat d = glm::normalize(glm::inverse(a) * b);
            if (d.w < 0.0f) d = -d;
            return static_cast<double>(glm::degrees(glm::angle(d)));
        };

        // ---- 类型名 / 摘要 ----
        {
            bool names = true;
            for (int t = 0; t < kConTypeCount; ++t) {
                int back = -1;
                names = names &&
                        constraintTypeFromName(constraintTypeName(t), back) &&
                        back == t;
            }
            int dummy = -1;
            // 打错一个字母不能"就近匹配"到某个类型上（那会静默变成另一种约束）
            names = names && !constraintTypeFromName("LimitRotate", dummy) &&
                    !constraintTypeFromName("", dummy);
            reportFlag("constraint_type_names", names);

            ConstraintStack a;
            ConstraintStack b;
            Constraint c;
            c.type = kConLimitRotation;
            c.bone = 3;
            a.add(c);
            b.add(c);
            const bool same = a.digest() == b.digest();
            b.items[0].influence = 0.5f;
            reportFlag("constraint_digest_stable",
                       same && a.digest() != b.digest(),
                       "same=" + std::to_string(same ? 1 : 0) + ",diff=" +
                           std::to_string(a.digest() != b.digest() ? 1 : 0));
        }

        // ---- JSON 往返 ----
        {
            const auto skel = makeConRig();
            ConstraintStack st;
            {
                Constraint c;
                c.type = kConLimitRotation;
                c.bone = 3;
                c.limitMask = 1;
                c.minDeg[0] = 0.0f;
                c.maxDeg[0] = 20.0f;
                st.add(c);
            }
            {
                Constraint c;
                c.type = kConCopyRotation;
                c.bone = 3;
                c.target = 2;
                c.influence = 0.75f;
                c.axisMask = 5;
                st.add(c);
            }
            {
                Constraint c;
                c.type = kConTwoBoneIK;
                c.ikRoot = 0;
                c.ikMid = 1;
                c.ikEnd = 2;
                c.ikHasTarget = true;
                c.ikTarget = glm::vec3(0.1f, -0.7f, 0.0f);
                c.stretch = 0.25f;
                st.add(c);
            }
            const unsigned long long before = st.digest();
            const std::string path =
                (std::filesystem::temp_directory_path() /
                 "venn_constraints_selftest.constraints.json")
                    .string();
            std::string err;
            const bool saved = saveConstraints(path, st, *skel, err);
            ConstraintStack back;
            int unresolved = -1;
            const bool loaded = loadConstraints(path, *skel, back, err, &unresolved);
            const unsigned long long after = back.digest();
            reportFlag("constraint_json_roundtrip",
                       saved && loaded && before == after && back.size() == st.size() &&
                           unresolved == 0,
                       "saved=" + std::to_string(saved ? 1 : 0) + ",loaded=" +
                           std::to_string(loaded ? 1 : 0) + ",eq=" +
                           std::to_string(before == after ? 1 : 0) + ",bad=" +
                           std::to_string(unresolved));
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        // ---- LimitRotation：夹到边界 / 限幅内逐位不变 / influence / 关掉 ----
        {
            const auto skel = makeConRig();
            Pose base;
            base.setRest(*skel);
            // armL 局部绕 X 转 60°，限位 0..20（肘不能反折的典型形状）
            base.rotation[3] =
                glm::normalize(glm::angleAxis(glm::radians(60.0f),
                                              glm::vec3(1.0f, 0.0f, 0.0f)));
            Constraint c;
            c.type = kConLimitRotation;
            c.bone = 3;
            c.limitMask = 1;      // 只管 X
            c.minDeg[0] = 0.0f;
            c.maxDeg[0] = 20.0f;
            ConstraintStack st;
            st.add(c);

            Pose q = base;
            const ConstraintEvalResult r = st.evaluate(*skel, q);
            report("constraint_limit_clamped_deg", eulerXDeg(q, 3), 20.0, 0.05);
            reportFlag("constraint_limit_clamped_count",
                       r.applied == 1 && r.clamped == 1 && r.failed == 0,
                       "applied=" + std::to_string(r.applied) + ",clamped=" +
                           std::to_string(r.clamped));

            // 限幅**以内**：一个字节都不许写（约定 ①）。
            // 这一条是"给没超限的骨加个约束 → 整条轨道开始漂"的唯一防线。
            Pose inside;
            inside.setRest(*skel);
            inside.rotation[3] =
                glm::normalize(glm::angleAxis(glm::radians(10.0f),
                                              glm::vec3(1.0f, 0.0f, 0.0f)));
            Pose q2 = inside;
            const ConstraintEvalResult r2 = st.evaluate(*skel, q2);
            const bool bitwise =
                std::memcmp(&q2.rotation[3], &inside.rotation[3],
                            sizeof(glm::quat)) == 0;
            reportFlag("constraint_limit_inside_bitwise",
                       bitwise && r2.applied == 0 && r2.skipped == 1,
                       "bitwise=" + std::to_string(bitwise ? 1 : 0) + ",applied=" +
                           std::to_string(r2.applied));

            // influence 0.5：从 60° 往 20° 走一半 = 40°（slerp，不是线性缩放）
            st.items[0].influence = 0.5f;
            Pose q3 = base;
            st.evaluate(*skel, q3);
            report("constraint_limit_influence_half_deg", eulerXDeg(q3, 3), 40.0, 0.2);

            // 关掉 = 没有（逐位）
            st.items[0].influence = 1.0f;
            st.items[0].enabled = false;
            Pose q4 = base;
            const ConstraintEvalResult r4 = st.evaluate(*skel, q4);
            reportFlag("constraint_limit_disabled_bitwise",
                       std::memcmp(&q4.rotation[3], &base.rotation[3],
                                   sizeof(glm::quat)) == 0 &&
                           r4.applied == 0,
                       "applied=" + std::to_string(r4.applied));
        }

        // ---- CopyRotation / CopyLocation ----
        {
            const auto skel = makeConRig();
            Pose base;
            base.setRest(*skel);
            base.rotation[2] = glm::normalize(glm::angleAxis(
                glm::radians(35.0f), glm::vec3(0.0f, 0.0f, 1.0f)));   // armR
            base.rotation[3] = glm::normalize(glm::angleAxis(
                glm::radians(-70.0f), glm::vec3(0.0f, 1.0f, 0.0f)));  // armL

            Constraint rot;
            rot.type = kConCopyRotation;
            rot.bone = 3;
            rot.target = 2;
            ConstraintStack st;
            st.add(rot);
            Pose q = base;
            const ConstraintEvalResult r = st.evaluate(*skel, q);
            std::vector<glm::mat4> w;
            poseToWorld(*skel, q, w);
            const double diff = quatAngleDeg(worldRotOf(w, 3), worldRotOf(w, 2));
            report("constraint_copy_rotation_world_diff_deg", diff, 0.0, 1.0e-3);
            reportFlag("constraint_copy_rotation_applied", r.applied == 1);

            // influence 0 → 逐位不变
            st.items[0].influence = 0.0f;
            Pose q0 = base;
            st.evaluate(*skel, q0);
            reportFlag("constraint_copy_rotation_influence0",
                       std::memcmp(&q0.rotation[3], &base.rotation[3],
                                   sizeof(glm::quat)) == 0);

            // CopyLocation：armL 的头跟到 armR 的头上（两臂对称摆开，
            // 所以 X 差 0.4 —— 全轴复制之后必须相等）
            Constraint loc;
            loc.type = kConCopyLocation;
            loc.bone = 3;
            loc.target = 2;
            ConstraintStack st2;
            st2.add(loc);
            Pose q5 = base;
            st2.evaluate(*skel, q5);
            std::vector<glm::mat4> w5;
            poseToWorld(*skel, q5, w5);
            const glm::vec3 pL = glm::vec3(w5[3][3]);
            const glm::vec3 pR = glm::vec3(w5[2][3]);
            report("constraint_copy_location_dist",
                   static_cast<double>(glm::length(pL - pR)), 0.0, 1.0e-4);

            // 轴掩码：只抄 X → Y/Z 必须还是原来的
            std::vector<glm::mat4> w0m;
            poseToWorld(*skel, base, w0m);
            st2.items[0].axisMask = 1;
            Pose q6 = base;
            st2.evaluate(*skel, q6);
            std::vector<glm::mat4> w6;
            poseToWorld(*skel, q6, w6);
            const glm::vec3 d = glm::vec3(w6[3][3]) - glm::vec3(w0m[3][3]);
            reportFlag("constraint_copy_location_axis_mask",
                       std::fabs(d.x) > 0.39f && std::fabs(d.y) < 1.0e-5f &&
                           std::fabs(d.z) < 1.0e-5f,
                       "dx=" + std::to_string(d.x) + ",dy=" + std::to_string(d.y));
        }

        // ---- ChildOf：像挂在目标骨下面一样跟着走 ----
        //
        // ⚠ 被测骨必须是**目标骨的非后代**。第一版拿 `weapon`（`armL` 的子骨）
        //   来测，`constraint_childof_control` 直接空转：父子链本来就会带着
        //   子骨转，所以"不做 ChildOf"和"做了 ChildOf"结果一样 —— 对照组
        //   证明不了任何事。ChildOf 的真实用途恰恰是**跨层级**：另一条链上的
        //   武器/道具骨要跟着这只手走（Blender 里也是这个用法）。
        //   所以这里让 `armR`（spine 的子骨，与 armL 平级）去跟 `armL`。
        {
            const auto skel = makeConRig();
            Pose base;
            base.setRest(*skel);
            std::vector<glm::mat4> restW;
            poseToWorld(*skel, base, restW);
            // rest 时 armR 相对 armL 的偏移 = (0.4, 0, 0)
            const glm::vec3 restOff =
                glm::vec3(restW[2][3]) - glm::vec3(restW[3][3]);

            // armL 绕 Z 转 40°（armR 不受影响：它不在 armL 下面）
            base.rotation[3] = glm::normalize(glm::angleAxis(
                glm::radians(40.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
            std::vector<glm::mat4> naiveW;
            poseToWorld(*skel, base, naiveW);
            const glm::vec3 naiveOff =
                glm::vec3(naiveW[2][3]) - glm::vec3(naiveW[3][3]);

            Constraint c;
            c.type = kConChildOf;
            c.bone = 2;      // armR
            c.target = 3;    // armL
            ConstraintStack st;
            st.add(c);
            Pose q = base;
            const ConstraintEvalResult r = st.evaluate(*skel, q);
            std::vector<glm::mat4> w;
            poseToWorld(*skel, q, w);
            const glm::vec3 off = glm::vec3(w[2][3]) - glm::vec3(w[3][3]);

            // 相对偏移应该整体转过 40°：Rz(40)·(0.4,0,0)
            const glm::vec3 want =
                glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(40.0f),
                                      glm::vec3(0.0f, 0.0f, 1.0f))) *
                restOff;
            report("constraint_childof_offset_err",
                   static_cast<double>(glm::length(off - want)), 0.0, 1.0e-4);
            // 对照组：不做 ChildOf 时偏移**没**跟着转（否则上面那条是空转）
            reportFlag("constraint_childof_control",
                       glm::length(naiveOff - want) > 0.05f,
                       "naive=" + std::to_string(glm::length(naiveOff - want)));
            reportFlag("constraint_childof_applied", r.applied == 1);
        }

        // ---- 顺序就是语义（"栈"与"三个特性"的根本区别）----
        {
            const auto skel = makeConRig();
            Pose base;
            base.setRest(*skel);
            // 两根臂都 60°。限位 0..20 +
            //   顺序 A【限位 → 复制】：先夹到 20，再被 armR 的 60 覆盖 → 60
            //   顺序 B【复制 → 限位】：先变 60，再夹到 20        → 20
            base.rotation[2] = glm::normalize(glm::angleAxis(
                glm::radians(60.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
            base.rotation[3] = base.rotation[2];

            Constraint lim;
            lim.type = kConLimitRotation;
            lim.bone = 3;
            lim.limitMask = 1;
            lim.minDeg[0] = 0.0f;
            lim.maxDeg[0] = 20.0f;
            Constraint cp;
            cp.type = kConCopyRotation;
            cp.bone = 3;
            cp.target = 2;

            ConstraintStack a;   // 限位 → 复制
            a.add(lim);
            a.add(cp);
            ConstraintStack b;   // 复制 → 限位
            b.add(cp);
            b.add(lim);

            Pose qa = base;
            Pose qb = base;
            a.evaluate(*skel, qa);
            b.evaluate(*skel, qb);
            const double ax = eulerXDeg(qa, 3);
            const double bx = eulerXDeg(qb, 3);
            report("constraint_order_limit_then_copy_deg", ax, 60.0, 0.05);
            report("constraint_order_copy_then_limit_deg", bx, 20.0, 0.05);
            // 两份栈内容一样、顺序不同 → 摘要必须不同（否则等于没顺序）
            reportFlag("constraint_order_digest_differs", a.digest() != b.digest());
        }

        // ---- IK 进栈：与直接求解逐位一致 / influence 0 逐位 / 拉伸 ----
        {
            // 髋 → 膝 → 踝，每节 0.4，沿 -Y 伸直
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

            const glm::vec3 target(0.2f, -0.7f, 0.0f);   // |t| = 0.728 < 0.8

            TwoBoneIKSettings cfg;
            cfg.hasTarget = true;
            cfg.target = target;
            Pose direct;
            direct.setRest(*skel);
            solveTwoBoneIK(*skel, cfg, direct);

            Constraint c;
            c.type = kConTwoBoneIK;
            c.ikRoot = 0;
            c.ikMid = 1;
            c.ikEnd = 2;
            c.ikHasTarget = true;
            c.ikTarget = target;
            ConstraintStack st;
            st.add(c);

            Pose viaStack;
            viaStack.setRest(*skel);
            const ConstraintEvalResult r = st.evaluate(*skel, viaStack);
            const bool bitwise =
                std::memcmp(viaStack.rotation.data(), direct.rotation.data(),
                            direct.rotation.size() * sizeof(glm::quat)) == 0;
            reportFlag("constraint_ik_via_stack_bitwise",
                       bitwise && r.ikSolved == 1 && r.applied == 1,
                       "bitwise=" + std::to_string(bitwise ? 1 : 0) + ",ik=" +
                           std::to_string(r.ikSolved));

            // influence 0 → 逐位不变
            st.items[0].influence = 0.0f;
            Pose zero;
            zero.setRest(*skel);
            Pose zeroRef = zero;
            st.evaluate(*skel, zero);
            reportFlag("constraint_ik_influence0_bitwise",
                       std::memcmp(zero.rotation.data(), zeroRef.rotation.data(),
                                   zeroRef.rotation.size() * sizeof(glm::quat)) == 0);

            // influence 0.5 的误差必须落在 [满权重, 不动] 之间（单调）
            const auto endErr = [&](const Pose& p) {
                std::vector<glm::mat4> w;
                poseToWorld(*skel, p, w);
                return glm::length(target - glm::vec3(w[2][3]));
            };
            st.items[0].influence = 1.0f;
            Pose full;
            full.setRest(*skel);
            st.evaluate(*skel, full);
            st.items[0].influence = 0.5f;
            Pose half;
            half.setRest(*skel);
            st.evaluate(*skel, half);
            const float eFull = endErr(full);
            const float eHalf = endErr(half);
            const float eNone = endErr(zeroRef);
            reportFlag("constraint_ik_influence_half_between",
                       eFull < 2.0e-3f && eHalf > eFull && eHalf < eNone,
                       "full=" + std::to_string(eFull) + ",half=" +
                           std::to_string(eHalf) + ",none=" + std::to_string(eNone));

            // 拉伸：目标在 0.95 处（够不到 0.8），stretch 0.5 应该够得着
            st.items[0].influence = 1.0f;
            st.items[0].ikTarget = glm::vec3(0.0f, -0.95f, 0.0f);
            Pose noStretch;
            noStretch.setRest(*skel);
            st.evaluate(*skel, noStretch);
            st.items[0].stretch = 0.5f;
            Pose stretched;
            stretched.setRest(*skel);
            st.evaluate(*skel, stretched);
            std::vector<glm::mat4> ws;
            poseToWorld(*skel, stretched, ws);
            const float errStretch =
                glm::length(st.items[0].ikTarget - glm::vec3(ws[2][3]));
            reportFlag("constraint_ik_stretch_reaches",
                       endErr(noStretch) > 0.10f && errStretch < 2.0e-2f,
                       "before=" + std::to_string(endErr(noStretch)) + ",after=" +
                           std::to_string(errStretch));
        }
    }

    // ============================================================
    // S-D：骨骼曲线（CurveBake）—— 2026-10-29
    //
    // 这一组钉的是"接上曲线之后**什么都没变**"和"改了的那一处**确实**变了"。
    // 前一半比后一半重要得多：
    //   · `curvebake_default_bitwise` —— 缓动表全默认时，采样必须与
    //     `assets::sampleTrack` **逐位**相同（T/S 靠同一个式子，
    //     R 靠"恒等缓动就转发 sampleTrack"那句短路）。这条一红，所有
    //     老资产的动画都会以 1e-7 量级漂，而画面上看不出来。
    //   · `curvebake_writeback_identical` —— 没改就写回，`changed` 必须是 0。
    //   · `curvebake_writeback_refuse` —— 关键帧数不等必须整条拒绝
    //     （约定 ④），不许"尽力而为"地写一半。
    // ============================================================
    {
        // 夹具：一条 3 帧的平移轨（x 0→1→2）+ 一条 3 帧的旋转轨。
        const auto makeTrack = [](assets::AnimTrack::Path p) {
            EditTrack tr;
            tr.joint = "hips";
            tr.path = p;
            tr.times = {0.0f, 0.5f, 1.0f};
            if (p == assets::AnimTrack::Rotation) {
                const glm::quat a(1.0f, 0.0f, 0.0f, 0.0f);
                const glm::quat b = glm::angleAxis(glm::radians(60.0f),
                                                   glm::vec3(0.0f, 0.0f, 1.0f));
                const glm::quat c = glm::angleAxis(glm::radians(120.0f),
                                                   glm::vec3(0.0f, 0.0f, 1.0f));
                tr.values = {glm::vec4(a.x, a.y, a.z, a.w),
                             glm::vec4(b.x, b.y, b.z, b.w),
                             glm::vec4(c.x, c.y, c.z, c.w)};
            } else {
                tr.values = {glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
                             glm::vec4(1.0f, 2.0f, 4.0f, 0.0f),
                             glm::vec4(2.0f, 4.0f, 8.0f, 0.0f)};
            }
            return tr;
        };

        report("curvebake_component_count_t",
               static_cast<double>(pathComponentCount(assets::AnimTrack::Translation)),
               3.0, 0.0);
        report("curvebake_component_count_r",
               static_cast<double>(pathComponentCount(assets::AnimTrack::Rotation)),
               4.0, 0.0);
        reportFlag("curvebake_component_name_w",
                   std::string(pathComponentName(assets::AnimTrack::Rotation, 3)) == "w",
                   std::string(pathComponentName(assets::AnimTrack::Rotation, 3)));

        // ---- ① 没有缓动 = 逐位不变 ----
        {
            const EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            assets::AnimTrack rt;
            rt.joint = 0;
            rt.path = tr.path;
            rt.times = tr.times;
            rt.values = tr.values;
            int sameNoTable = 0, sameDefaultTable = 0;
            // (a) 表为空
            for (int s = 0; s <= 20; ++s) {
                const float t = 1.0f * static_cast<float>(s) / 20.0f;
                if (sampleEditTrack(tr, t) == assets::sampleTrack(rt, t))
                    ++sameNoTable;
            }
            // (b) 表**显式填满默认值** —— 这是"进过曲线编辑器但没有改缓动"
            //     的状态，也是"打一圈再清回去"之后应该回到的状态。
            EditTrack td = tr;
            td.ease.resize(td.times.size(), KeyEase{});
            for (int s = 0; s <= 20; ++s) {
                const float t = 1.0f * static_cast<float>(s) / 20.0f;
                if (sampleEditTrack(td, t) == assets::sampleTrack(rt, t))
                    ++sameDefaultTable;
            }
            reportFlag("curvebake_no_ease_bitwise", sameNoTable == 21,
                       "same=" + std::to_string(sameNoTable) + "/21");
            reportFlag("curvebake_default_bitwise", sameDefaultTable == 21,
                       "same=" + std::to_string(sameDefaultTable) + "/21");
        }

        // ---- ② 旋转：Linear 段必须走 slerp（不是 nlerp）----
        {
            const EditTrack tr = makeTrack(assets::AnimTrack::Rotation);
            assets::AnimTrack rt;
            rt.joint = 0;
            rt.path = tr.path;
            rt.times = tr.times;
            rt.values = tr.values;
            EditTrack td = tr;
            td.ease.resize(td.times.size(), KeyEase{});
            int same = 0;
            for (int s = 0; s <= 20; ++s) {
                const float t = 1.0f * static_cast<float>(s) / 20.0f;
                if (sampleEditTrack(td, t) == assets::sampleTrack(rt, t)) ++same;
            }
            reportFlag("curvebake_rotation_linear_bitwise", same == 21,
                       "same=" + std::to_string(same) + "/21");

            // 反证：真的走 nlerp（分量各自线性）会与 slerp 不同 ——
            // 这条是给"谁把 Linear 分支改成逐分量插值"准备的报警器。
            //
            // ⚠ 取样点必须是 **u=0.25 而不是 0.5**：nlerp 与 slerp 在正中
            //   点**恰好相等**（两者都给出角平分线），在那里量差异只能量到
            //   浮点噪声（实测 3e-6 度）。这是数学性质，不是实现问题 ——
            //   第一版写 0.5，断言直接红了。
            const float uProbe = 0.25f;
            glm::vec4 nlerp(0.0f);
            for (int c = 0; c < 4; ++c) {
                const float a = rt.values[0][static_cast<glm::length_t>(c)];
                const float b = rt.values[1][static_cast<glm::length_t>(c)];
                nlerp[static_cast<glm::length_t>(c)] = a + (b - a) * uProbe;
            }
            const glm::quat qn = glm::normalize(
                glm::quat(nlerp.w, nlerp.x, nlerp.y, nlerp.z));
            const glm::quat qs = glm::slerp(
                glm::quat(rt.values[0].w, rt.values[0].x, rt.values[0].y, rt.values[0].z),
                glm::quat(rt.values[1].w, rt.values[1].x, rt.values[1].y, rt.values[1].z),
                uProbe);
            const float dn = glm::angle(glm::normalize(qn));
            const float ds = glm::angle(glm::normalize(qs));
            // 只报"两者不同"，不给绝对值断言（这个差随夹具角度变化，钉死
            // 一个数字等于把夹具绑死）。真正要报警的是"哪天有人把 Linear
            // 分支改成逐分量插值"—— 那时 `curvebake_rotation_linear_bitwise`
            // 先红，这条是它的注解。
            reportFlag("curvebake_slerp_nlerp_distinct",
                       std::fabs(dn - ds) > 1.0e-6f,
                       "deg=" + std::to_string(std::fabs(dn - ds) * 180.0 / 3.14159265));
        }

        // ---- ③ step 轨道 ----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            tr.step = true;
            tr.ease.resize(tr.times.size(), KeyEase{});   // 空表也会被填成 Constant
            assets::AnimTrack rt;
            rt.joint = 0;
            rt.path = tr.path;
            rt.times = tr.times;
            rt.values = tr.values;
            rt.step = true;
            int same = 0;
            for (int s = 0; s <= 20; ++s) {
                const float t = 1.0f * static_cast<float>(s) / 20.0f;
                if (sampleEditTrack(tr, t) == assets::sampleTrack(rt, t)) ++same;
            }
            reportFlag("curvebake_step_bitwise", same == 21,
                       "same=" + std::to_string(same) + "/21");
            // 中间帧必须**恰好等于左端点**（阶跃），不是插出来的值
            report("curvebake_step_mid_value",
                   static_cast<double>(sampleEditTrack(tr, 0.4f).x), 0.0, 0.0);
        }

        // ---- ④ 缓动确实改变中间帧，端点不动 ----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            tr.ease.assign(tr.times.size(), KeyEase{});
            KeyEase e;
            e.interp = Interp::Linear;
            e.ease = EaseMode::InOut;
            e.shape = EaseShape::Smooth;
            tr.ease[0] = e;                 // 只有第一段缓动
            // u = 0.5 → smoothstep(0.5) = 0.5（对称），所以取 u=0.25：
            // 0.25²(3-2·0.25) = 0.15625；线性是 0.25，值是 2.0 的 1/4 段
            // （t=0.125 时 x 的线性值是 0.25）。
            const float lin = 0.0f + (1.0f - 0.0f) * 0.25f;              // 0.25
            const float eased = 0.0f + (1.0f - 0.0f) * 0.15625f;         // 0.15625
            report("curvebake_ease_mid_value",
                   static_cast<double>(sampleEditTrack(tr, 0.125f).x),
                   static_cast<double>(eased), 1e-7);
            reportFlag("curvebake_ease_differs_from_linear",
                       std::fabs(sampleEditTrack(tr, 0.125f).x - lin) > 0.05f,
                       "eased=" + std::to_string(sampleEditTrack(tr, 0.125f).x) +
                           ",lin=" + std::to_string(lin));
            // 端点逐位不动
            reportFlag("curvebake_ease_endpoints_bitwise",
                       sampleEditTrack(tr, 0.0f).x == 0.0f &&
                           sampleEditTrack(tr, 1.0f).x == 2.0f,
                       "a=" + std::to_string(sampleEditTrack(tr, 0.0f).x) +
                           ",b=" + std::to_string(sampleEditTrack(tr, 1.0f).x));
        }

        // ---- ⑤ 贝塞尔：写回后逐位可还原 ----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            tr.ease.assign(tr.times.size(), KeyEase{});
            KeyEase e;
            e.interp = Interp::Bezier;
            e.handle = HandleType::Free;
            e.outHandle = glm::vec2(0.15f, 0.0f);     // 起手慢
            e.inHandle = glm::vec2(-0.4f, 0.0f);
            tr.ease[0] = e;

            Curve c = bakeTrackCurve(tr, 0);
            reportFlag("curvebake_bezier_baked",
                       c.keys.size() == 3 &&
                           c.keys[0].interp == Interp::Bezier &&
                           c.keys[0].handle == HandleType::Free,
                       "n=" + std::to_string(c.keys.size()));

            const unsigned long long d0 = curveDigest(c);
            EditTrack back = tr;
            back.ease.clear();                        // 故意从空表开始
            const CurveWriteStats st = applyCurveToTrack(back, 0, c);
            reportFlag("curvebake_writeback_identical",
                       st.changed == 0 && st.skipped == 0 && st.bitIdentical,
                       "changed=" + std::to_string(st.changed) +
                           ",worst=" + std::to_string(st.worstDeviation));

            const Curve c2 = bakeTrackCurve(back, 0);
            reportFlag("curvebake_roundtrip_digest", curveDigest(c2) == d0,
                       "d0=" + std::to_string(d0) + ",d1=" + std::to_string(curveDigest(c2)));

            // 中间帧确实早于线性（起手慢）
            reportFlag("curvebake_bezier_ease_in",
                       sampleEditTrack(back, 0.125f).x < 0.25f,
                       "v=" + std::to_string(sampleEditTrack(back, 0.125f).x));
        }

        // ---- ⑥ 写回：只改本分量，其它分量逐位不变 ----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            Curve c = bakeTrackCurve(tr, 0);
            c.keys[1].value = 9.5f;
            const glm::vec4 beforeY = tr.values[1];
            const CurveWriteStats st = applyCurveToTrack(tr, 0, c);
            reportFlag("curvebake_write_component_isolated",
                       st.changed == 1 && tr.values[1].x == 9.5f &&
                           tr.values[1].y == beforeY.y && tr.values[1].z == beforeY.z,
                       "changed=" + std::to_string(st.changed) + ",y=" +
                           std::to_string(tr.values[1].y));
        }

        // ---- ⑦ 写回：关键帧数不等 → 整条拒绝 ----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            const std::vector<float> t0 = tr.times;
            Curve c = bakeTrackCurve(tr, 0);
            c.keys.pop_back();
            const CurveWriteStats st = applyCurveToTrack(tr, 0, c);
            reportFlag("curvebake_writeback_refuse",
                       st.skipped == 1 && st.written == 0 && tr.times == t0,
                       "skipped=" + std::to_string(st.skipped));
        }

        // ---- ⑧ 写回：拖时间 + 同刻合并 ----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            Curve c = bakeTrackCurve(tr, 0);
            c.keys[2].time = 1.5f;
            const CurveWriteStats st = applyCurveToTrack(tr, 0, c);
            reportFlag("curvebake_write_time_moved",
                       st.changed == 1 && tr.times.back() == 1.5f &&
                           st.bitIdentical,
                       "changed=" + std::to_string(st.changed) + ",last=" +
                           std::to_string(tr.times.back()));

            EditTrack td = makeTrack(assets::AnimTrack::Translation);
            Curve cd = bakeTrackCurve(td, 0);
            cd.keys[1].time = 0.0f;                 // 撞到第 0 帧
            const CurveWriteStats sd = applyCurveToTrack(td, 0, cd);
            reportFlag("curvebake_write_merge",
                       sd.merged == 1 && td.keyCount() == 2,
                       "merged=" + std::to_string(sd.merged) + ",n=" +
                           std::to_string(td.keyCount()));
        }

        // ---- ⑨ step 与缓动互斥 ----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            tr.step = true;
            Curve c = bakeTrackCurve(tr, 0);
            c.keys[0].interp = Interp::Bezier;
            applyCurveToTrack(tr, 0, c);
            reportFlag("curvebake_step_off", tr.step == false,
                       "step=" + std::to_string(tr.step ? 1 : 0));
            reportFlag("curvebake_step_off_keeps_bezier",
                       tr.easeAt(0).interp == Interp::Bezier,
                       std::string(interpName(tr.easeAt(0).interp)));
        }

        // ---- ⑩ 全默认 → 抹平（回到"没有缓动"的路径）----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            tr.ease.assign(tr.times.size(), KeyEase{});
            reportFlag("curvebake_default_table_present", !tr.ease.empty(),
                       "n=" + std::to_string(tr.ease.size()));
            const bool pruned = tr.pruneDefaultEase();
            reportFlag("curvebake_prune", pruned && tr.ease.empty() && !tr.hasEasing(),
                       "pruned=" + std::to_string(pruned ? 1 : 0));

            // 打一帧之后表要跟着长一格，否则缓动会整体错位
            EditTrack te = makeTrack(assets::AnimTrack::Translation);
            te.ease.assign(te.times.size(), KeyEase{});
            te.ease[0].interp = Interp::Bezier;
            EditClip c;
            c.tracks.push_back(te);
            c.setKey("hips", assets::AnimTrack::Translation, 0.25f,
                     glm::vec4(0.5f, 0.0f, 0.0f, 0.0f));
            const EditTrack& grew = c.tracks[0];
            reportFlag("curvebake_insert_keeps_ease_aligned",
                       grew.keyCount() == 4 &&
                           grew.ease.size() == grew.times.size() &&
                           grew.easeAt(0).interp == Interp::Bezier,
                       "keys=" + std::to_string(grew.keyCount()) + ",ease=" +
                           std::to_string(grew.ease.size()));
        }

        // ---- ⑪ 旋转贝塞尔：归一化 ----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Rotation);
            tr.ease.assign(tr.times.size(), KeyEase{});
            KeyEase e;
            e.interp = Interp::Bezier;
            e.handle = HandleType::Free;
            e.outHandle = glm::vec2(0.4f, 0.2f);    // 故意把 x 分量拽出去
            tr.ease[0] = e;
            ResolvedTrack rt;
            rt.track.joint = 0;
            rt.track.path = tr.path;
            rt.track.times = tr.times;
            rt.track.values = tr.values;
            bakeResolvedTrack(tr, rt);
            const glm::vec4 v = sampleResolvedTrack(rt, 0.25f);
            const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
            report("curvebake_rotation_bezier_unit", static_cast<double>(len),
                   1.0, 1e-6);
        }

        // ---- ⑫ 通道表 ----
        {
            EditClip c;
            EditTrack t = makeTrack(assets::AnimTrack::Translation);
            EditTrack r = makeTrack(assets::AnimTrack::Rotation);
            c.tracks.push_back(t);
            c.tracks.push_back(r);
            const auto list = listBoneChannels(c);
            report("curvebake_channel_count", static_cast<double>(list.size()), 7.0, 0.0);
            reportFlag("curvebake_channel_id",
                       boneChannelId(c, list[0]) == "hips/translation.x" &&
                           boneChannelId(c, list[6]) == "hips/rotation.w",
                       boneChannelId(c, list[0]) + "," + boneChannelId(c, list[6]));
        }

        // ---- ⑬ 整条叠加链：有缓动才改，没缓动逐位不变 ----
        {
            const auto mkSkel = []() {
                assets::Skeleton s;
                s.joints.resize(1);
                s.joints[0].name = "hips";
                s.joints[0].parent = -1;
                s.topoOrder = {0};
                return s;
            };
            const assets::Skeleton sk = mkSkel();
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);

            EditClip c0;
            c0.tracks.push_back(tr);
            std::vector<ResolvedTrack> r0;
            resolveEditTracks(sk, c0, r0);
            Pose p0;
            p0.setRest(sk);
            overlayEditPose(r0, 0.2f, p0);
            // 没缓动 → 与 sampleTrack 逐位相同
            bool bit = true;
            for (int k = 0; k < 3; ++k) {
                if (p0.translation[0][static_cast<glm::length_t>(k)] !=
                    assets::sampleTrack(r0[0].track, 0.2f)[static_cast<glm::length_t>(k)])
                    bit = false;
            }
            reportFlag("curvebake_overlay_plain_bitwise", bit, "bit=" + std::to_string(bit ? 1 : 0));

            EditTrack te = tr;
            te.ease.assign(te.times.size(), KeyEase{});
            KeyEase e;
            e.interp = Interp::Linear;
            e.ease = EaseMode::InOut;
            e.shape = EaseShape::Smooth;
            te.ease[0] = e;
            EditClip c1;
            c1.tracks.push_back(te);
            std::vector<ResolvedTrack> r1;
            resolveEditTracks(sk, c1, r1);
            Pose p1;
            p1.setRest(sk);
            overlayEditPose(r1, 0.2f, p1);
            reportFlag("curvebake_overlay_eased_resolved", r1[0].hasEase,
                       "hasEase=" + std::to_string(r1[0].hasEase ? 1 : 0));
            // ⚠ 取 t=0.2（u=0.4 < 0.5）：缓入缓出在**前半段**落后于线性，
            //   后半段反超。第一版在 t=0.3（u=0.6）上断言"更小"，红了 ——
            //   那是 smoothstep 的正确行为，不是实现错。
            reportFlag("curvebake_overlay_eased_differs",
                       p1.translation[0].x < p0.translation[0].x,
                       "plain=" + std::to_string(p0.translation[0].x) +
                           ",eased=" + std::to_string(p1.translation[0].x));
        }

        // ---- ⑭ 存读往返：缓动表 ----
        {
            EditTrack tr = makeTrack(assets::AnimTrack::Translation);
            tr.ease.assign(tr.times.size(), KeyEase{});
            KeyEase e;
            e.interp = Interp::Bezier;
            e.handle = HandleType::Aligned;
            e.outHandle = glm::vec2(0.22f, 0.37f);
            e.inHandle = glm::vec2(-0.11f, -0.19f);
            tr.ease[0] = e;
            EditClip c;
            c.name = "curvetest";
            c.tracks.push_back(tr);

            std::string err;
            const std::string path =
                (std::filesystem::temp_directory_path() /
                 "venn_curvebake_selftest.anim.json").string();
            EditClip back;
            const bool saved = saveEditClip(path, c, err);
            const bool loaded = saved && loadEditClip(path, back, err);
            const unsigned long long d0 = curveDigest(bakeTrackCurve(c.tracks[0], 0));
            const unsigned long long d1 =
                loaded ? curveDigest(bakeTrackCurve(back.tracks[0], 0)) : 0ull;
            reportFlag("curvebake_json_roundtrip",
                       loaded && d0 == d1 && back.tracks.size() == 1,
                       "saved=" + std::to_string(saved ? 1 : 0) +
                           ",loaded=" + std::to_string(loaded ? 1 : 0) + ",d=" +
                           std::to_string(d0 == d1 ? 1 : 0));
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    // ============================================================
    // R9：NLA（时间轴片段）—— 2026-10-29
    //
    // 这一组的判据分两半，重要性差得很远：
    //
    //   **"没有 NLA 就等于没有这个功能"**（占一半以上）
    //     · `nla_empty_bitwise` / `nla_no_active_strip_bitwise` —— 空轨列表、
    //       或播放头落在所有 strip 之外时，`pose` 必须**逐位**不变。
    //     · `nla_replace_w1_bitwise` —— 遮罩 All + 权重 1 的 Replace strip，
    //       结果必须与"只播这条 strip"**逐位**相同（B1 的端点短路白送，
    //       但得有人钉住"我们真的走了那条短路"）。
    //
    //   **"时间映射是可手算的"**
    //     `sourceTime` 是这一层唯一的新数学（span / speed / loop /
    //     clipStart..clipEnd 五个变量折成一个时刻）。全部断言都挑在
    //     能口算的点上（0.25 / 0.5 / 0.75），因为"看起来在动"完全不能
    //     证明映射是对的 —— 一段匀速直线在错误的映射下照样匀速。
    // ============================================================
    {
        Fixture f;
        buildLayerFixture(f);
        const assets::Skeleton* skel = f.model.skeleton.get();
        const assets::AnimationClip* baseClip = skel->findClip("base");
        const assets::AnimationClip* upperClip = skel->findClip("upper");

        // 逐位比两份姿势（三条通道全比）。容差会让"多混了 1e-7"混过去，
        // 而这一组一半的断言就是要抓那 1e-7。
        const auto poseBitEqual = [](const Pose& a, const Pose& b) {
            if (a.count() != b.count()) return false;
            for (int i = 0; i < a.count(); ++i) {
                const size_t j = static_cast<size_t>(i);
                if (std::memcmp(&a.translation[j], &b.translation[j],
                                sizeof(glm::vec3)) != 0)
                    return false;
                if (std::memcmp(&a.rotation[j], &b.rotation[j],
                                sizeof(glm::quat)) != 0)
                    return false;
                if (std::memcmp(&a.scale[j], &b.scale[j],
                                sizeof(glm::vec3)) != 0)
                    return false;
            }
            return true;
        };
        const auto sampleInto = [&](const assets::AnimationClip* c, float t,
                                    Pose& out) {
            out.setRest(*skel);
            samplePose(*skel, c, t, out);
        };
        // hand 的世界 scale.x —— 这一组唯一的数值判别式（与 B1 同一个理由：
        // 它是标量、与父骨旋转无关，读一个数就知道"这条 strip 有没有
        // 覆盖到 hand"）。
        const auto handScale = [&](const Pose& p) {
            std::vector<glm::mat4> w;
            poseToWorld(*skel, p, w);
            return glm::length(glm::vec3(w[2][0]));
        };
        const auto makeStrip = [&](const std::string& clip, float s, float e) {
            NlaStrip st;
            st.name = "s";
            st.clip = clip;
            st.start = s;
            st.end = e;
            return st;
        };
        const auto makeTrack = [&](const std::string& name, NlaStrip st) {
            NlaTrack T;
            T.name = name;
            T.strips.push_back(std::move(st));
            return T;
        };

        // ---- ① 空轨列表 = 逐位不变（约定 ①）----
        {
            Pose p0;
            sampleInto(baseClip, 0.5f, p0);
            Pose p1 = p0;
            NlaScratch sc;
            const std::vector<NlaTrack> none;
            const NlaEvalStats st = applyNlaTimeline(*skel, none, 0.5f, p1, sc);
            reportFlag("nla_empty_bitwise",
                       poseBitEqual(p0, p1) && st.applied == 0 &&
                           st.tracks == 0,
                       "applied=" + std::to_string(st.applied));
        }

        // ---- ② 播放头落在所有 strip 之外 = 逐位不变（约定 ③）----
        {
            Pose p0;
            sampleInto(baseClip, 0.5f, p0);
            Pose p1 = p0;
            NlaScratch sc;
            const std::vector<NlaTrack> tr{makeTrack(
                "t", makeStrip("upper", 1.0f, 2.0f))};   // [1,2]，t=0.5 在区间外
            const NlaEvalStats st = applyNlaTimeline(*skel, tr, 0.5f, p1, sc);
            reportFlag("nla_no_active_strip_bitwise",
                       poseBitEqual(p0, p1) && st.active == 0 &&
                           st.applied == 0 && st.skipped == 0,
                       "active=" + std::to_string(st.active) +
                           ",skipped=" + std::to_string(st.skipped));
        }

        // ---- ③ 遮罩 All + 权重 1 的 Replace = 逐位等于只播那条 strip ----
        {
            Pose only;
            sampleInto(upperClip, 0.5f, only);
            Pose p;
            sampleInto(baseClip, 0.5f, p);
            NlaScratch sc;
            const std::vector<NlaTrack> tr{
                makeTrack("t", makeStrip("upper", 0.0f, 1.0f))};
            const NlaEvalStats st = applyNlaTimeline(*skel, tr, 0.5f, p, sc);
            reportFlag("nla_replace_w1_bitwise", poseBitEqual(only, p),
                       "applied=" + std::to_string(st.applied));
        }

        // ---- ④ 权重 0 = 逐位不变（不是"算个 0"）----
        {
            Pose p0;
            sampleInto(baseClip, 0.5f, p0);
            Pose p = p0;
            NlaStrip s = makeStrip("upper", 0.0f, 1.0f);
            s.weight = 0.0f;
            NlaScratch sc;
            const std::vector<NlaTrack> tr{makeTrack("t", s)};
            const NlaEvalStats st = applyNlaTimeline(*skel, tr, 0.5f, p, sc);
            // weight 0 → `valid()` 为 false → 算 skipped（配置层面"这条不算"）
            reportFlag("nla_w0_bitwise",
                       poseBitEqual(p0, p) && st.applied == 0,
                       "skipped=" + std::to_string(st.skipped));
        }

        // ---- ⑤ 时间映射：span / speed / loop（约定 ②）----
        {
            // strip [0,2] 映射 1.0 秒的 clip → t=1.0 是正中
            NlaStrip s = makeStrip("upper", 0.0f, 2.0f);
            report("nla_map_mid", static_cast<double>(s.sourceTime(1.0f, 1.0f)),
                   0.5, 1e-6);

            // span 里塞两遍（speed=2）用一条 **span=1** 的 strip 才手算得动：
            // t=0.25 → u=0.25*2=0.5；t=0.75 → u=1.5 → 折回 0.5。
            // ⚠ 别拿上面那条 span=2 的 `s` 来算 —— 它的 u 是一半
            //   （0.25 → 0.125，×2 之后 0.25），第一版就是这么把三条断言
            //   全算错的（实现是对的，是手算错了）。
            NlaStrip w = makeStrip("upper", 0.0f, 1.0f);
            w.speed = 2.0f;
            report("nla_map_loop_fold",
                   static_cast<double>(w.sourceTime(0.75f, 1.0f)), 0.5, 1e-6);
            report("nla_map_loop_quarter",
                   static_cast<double>(w.sourceTime(0.25f, 1.0f)), 0.5, 1e-6);

            // **端点折回**：loop=true 时 t=end 映射到源区间**开头**。
            // 这不是怪癖，是"首尾相接不留缝"的保证：接缝那一帧前后两条
            // strip 给出同一个姿势，而不是"停在末帧 + 从头开始"叠一下。
            report("nla_map_loop_end_wraps",
                   static_cast<double>(w.sourceTime(1.0f, 1.0f)), 0.0, 1e-6);

            // 不循环：u 夹到 1 → 停在源区间末尾
            w.loop = false;
            report("nla_map_once_clamp",
                   static_cast<double>(w.sourceTime(0.75f, 1.0f)), 1.0, 1e-6);

            // 截取源区间的一段：[0.25, 0.75] 铺满 strip。
            // ⚠ 这里必须 `loop = false`：循环时 t=end 会折回 u=0（见上一条），
            //   断言"末端 = 0.75"就永远不成立。第一版没设，红的正是它。
            NlaStrip r = makeStrip("upper", 0.0f, 1.0f);
            r.loop = false;
            r.clipStart = 0.25f;
            r.clipEnd = 0.75f;
            report("nla_map_clip_range_start",
                   static_cast<double>(r.sourceTime(0.0f, 1.0f)), 0.25, 1e-6);
            report("nla_map_clip_range_mid",
                   static_cast<double>(r.sourceTime(0.5f, 1.0f)), 0.5, 1e-6);
            report("nla_map_clip_range_end",
                   static_cast<double>(r.sourceTime(1.0f, 1.0f)), 0.75, 1e-6);

            // clipEnd 留空（0/0）= 整段（新建 strip 的默认形态）
            NlaStrip d = makeStrip("upper", 0.0f, 1.0f);
            report("nla_map_default_whole",
                   static_cast<double>(d.sourceTime(0.5f, 1.0f)), 0.5, 1e-6);

            // 负 speed 也要能倒放（fract 必须用 u-floor(u)，fmod 会给负数）
            NlaStrip n = makeStrip("upper", 0.0f, 1.0f);
            n.speed = -1.0f;
            const float neg = n.sourceTime(0.25f, 1.0f);
            reportFlag("nla_map_negative_speed",
                       neg >= -1e-6f && neg <= 1.0f + 1e-6f,
                       "src=" + std::to_string(neg));
        }

        // ---- ⑥ 包络（淡入淡出）----
        {
            // 没有淡入淡出 → **恰好** 1.0（约定 ① 的逐位不变靠它）
            NlaStrip s = makeStrip("upper", 0.0f, 1.0f);
            reportFlag("nla_env_none_exact",
                       s.envelope(0.0f) == 1.0f && s.envelope(0.5f) == 1.0f &&
                           s.envelope(1.0f) == 1.0f,
                       "e=" + std::to_string(s.envelope(0.5f)));

            s.blendIn = 0.4f;
            report("nla_env_in_start", static_cast<double>(s.envelope(0.0f)),
                   0.0, 1e-6);
            // smoothFade(0.5) = 0.5（手算：0.25*(3-1)）
            report("nla_env_in_half", static_cast<double>(s.envelope(0.2f)),
                   0.5, 1e-6);
            report("nla_env_in_done", static_cast<double>(s.envelope(0.4f)),
                   1.0, 1e-6);

            // 两侧都设、且在中间**重叠**：包络 = smoothFade(1)*smoothFade(1)
            NlaStrip b = makeStrip("upper", 0.0f, 1.0f);
            b.blendIn = 0.5f;
            b.blendOut = 0.5f;
            report("nla_env_overlap_edge",
                   static_cast<double>(b.envelope(0.5f)), 1.0, 1e-6);
            // t=0.25：smoothFade(0.5) * smoothFade(1) = 0.5
            report("nla_env_overlap_quarter",
                   static_cast<double>(b.envelope(0.25f)), 0.5, 1e-6);
            // t=0.125：**只有淡入那一侧在斜坡上**（淡出的斜坡从 0.5 才开始）。
            // env = smoothFade(0.25) * smoothFade(1.75→夹到 1) = 0.15625
            // ⚠ 第一版写成 0.15625² —— 那是"两侧同时斜坡"的情形，而两侧
            //   在 t<0.5 时根本不重叠。
            report("nla_env_overlap_eighth",
                   static_cast<double>(b.envelope(0.125f)), 0.15625, 1e-6);
        }

        // ---- ⑦ 遮罩：Allow=hand 只动 hand ----
        {
            NlaStrip s = makeStrip("upper", 0.0f, 1.0f);
            s.mask.mode = BoneMaskMode::Allow;
            s.mask.roots = {"hand"};
            Pose p0;
            sampleInto(baseClip, 0.5f, p0);
            Pose p = p0;
            NlaScratch sc;
            const std::vector<NlaTrack> tr{makeTrack("t", s)};
            const NlaEvalStats st = applyNlaTimeline(*skel, tr, 0.5f, p, sc);
            // base 在 0.5 s：hand scale = 1.5；upper 在 0.5 s：2.0
            report("nla_mask_allow_hand_scale",
                   static_cast<double>(handScale(p)), 2.0, 1e-5);
            reportFlag("nla_mask_allow_root_untouched",
                       std::memcmp(&p.rotation[0], &p0.rotation[0],
                                   sizeof(glm::quat)) == 0 &&
                           std::memcmp(&p.rotation[1], &p0.rotation[1],
                                       sizeof(glm::quat)) == 0,
                       "applied=" + std::to_string(st.applied));
        }

        // ---- ⑧ Add 模式：rest 相对增量（约定 ④）----
        {
            NlaStrip s = makeStrip("upper", 0.0f, 1.0f);
            s.blend = NlaBlend::Add;
            Pose p0;
            sampleInto(baseClip, 0.5f, p0);
            Pose p = p0;
            NlaScratch sc;
            const std::vector<NlaTrack> tr{makeTrack("t", s)};
            applyNlaTimeline(*skel, tr, 0.5f, p, sc);
            // hand：base 1.5 + (upper 2.0 - rest 1.0) = 2.5
            // （Replace 会是 2.0 —— 两个数差得远，一眼能看出走错了路）
            report("nla_add_hand_scale", static_cast<double>(handScale(p)),
                   2.5, 1e-5);
            // root：upper 根本没有 root 轨 → 增量 0 → 与 base 逐位相同
            reportFlag("nla_add_untouched_bone_bitwise",
                       std::memcmp(&p.rotation[0], &p0.rotation[0],
                                   sizeof(glm::quat)) == 0 &&
                           std::memcmp(&p.translation[0], &p0.translation[0],
                                       sizeof(glm::vec3)) == 0,
                       "ok");
        }

        // ---- ⑨ 权重 0.5 的 Add 落在"半幅度"上（不是"半姿态"）----
        {
            NlaStrip s = makeStrip("upper", 0.0f, 1.0f);
            s.blend = NlaBlend::Add;
            s.weight = 0.5f;
            Pose p;
            sampleInto(baseClip, 0.5f, p);
            NlaScratch sc;
            const std::vector<NlaTrack> tr{makeTrack("t", s)};
            applyNlaTimeline(*skel, tr, 0.5f, p, sc);
            // hand：1.5 + (2.0 - 1.0) * 0.5 = 2.0
            report("nla_add_half_weight", static_cast<double>(handScale(p)),
                   2.0, 1e-5);
        }

        // ---- ⑩ 轨顺序 = 求值顺序（下标大的胜出）----
        {
            const std::vector<NlaTrack> a{
                makeTrack("bottom", makeStrip("upper", 0.0f, 1.0f)),
                makeTrack("top", makeStrip("base", 0.0f, 1.0f))};
            const std::vector<NlaTrack> b{
                makeTrack("bottom", makeStrip("base", 0.0f, 1.0f)),
                makeTrack("top", makeStrip("upper", 0.0f, 1.0f))};

            Pose onlyBase, onlyUpper;
            sampleInto(baseClip, 0.5f, onlyBase);
            sampleInto(upperClip, 0.5f, onlyUpper);

            Pose pa, pb;
            sampleInto(baseClip, 0.5f, pa);
            NlaScratch s1;
            applyNlaTimeline(*skel, a, 0.5f, pa, s1);
            sampleInto(baseClip, 0.5f, pb);
            NlaScratch s2;
            applyNlaTimeline(*skel, b, 0.5f, pb, s2);

            reportFlag("nla_order_top_wins",
                       poseBitEqual(onlyBase, pa) &&
                           poseBitEqual(onlyUpper, pb),
                       "ok");
        }

        // ---- ⑪ 静音 / solo ----
        {
            NlaTrack muted = makeTrack("m", makeStrip("upper", 0.0f, 1.0f));
            muted.muted = true;
            NlaTrack live = makeTrack("l", makeStrip("upper", 0.0f, 1.0f));
            Pose p0;
            sampleInto(baseClip, 0.5f, p0);
            Pose p = p0;
            NlaScratch sc;
            const std::vector<NlaTrack> tr{muted, live};
            const NlaEvalStats st = applyNlaTimeline(*skel, tr, 0.5f, p, sc);
            // 静音轨的 strip **不进 active**（它在区间内，但整条轨被跳过）
            reportFlag("nla_muted_track_skipped",
                       st.tracksActive == 1 && st.applied == 1 &&
                           st.active == 1,
                       "activeTracks=" + std::to_string(st.tracksActive));

            // solo：只有 solo 轨参与
            NlaTrack soloTr = makeTrack("s", makeStrip("upper", 0.0f, 1.0f));
            soloTr.solo = true;
            NlaTrack other = makeTrack("o", makeStrip("base", 0.0f, 1.0f));
            Pose q0;
            sampleInto(baseClip, 0.5f, q0);
            Pose q;
            sampleInto(baseClip, 0.5f, q);
            NlaScratch sc2;
            const std::vector<NlaTrack> tr2{other, soloTr};
            const NlaEvalStats st2 = applyNlaTimeline(*skel, tr2, 0.5f, q, sc2);
            Pose onlyUpper;
            sampleInto(upperClip, 0.5f, onlyUpper);
            reportFlag("nla_solo_only",
                       poseBitEqual(onlyUpper, q) && st2.tracksActive == 1,
                       "activeTracks=" + std::to_string(st2.tracksActive));
        }

        // ---- ⑫ 配置坏了 = skipped + 报第一个问题（不是"全身覆盖"）----
        {
            NlaTrack miss = makeTrack("t", makeStrip("no_such_clip", 0.0f, 1.0f));
            Pose p0;
            sampleInto(baseClip, 0.5f, p0);
            Pose p = p0;
            NlaScratch sc;
            const std::vector<NlaTrack> tr{miss};
            const NlaEvalStats st = applyNlaTimeline(*skel, tr, 0.5f, p, sc);
            reportFlag("nla_missing_clip_skipped",
                       st.applied == 0 && st.skipped == 1 &&
                           poseBitEqual(p0, p) &&
                           st.firstProblem == "clip:no_such_clip",
                       "problem=" + st.firstProblem);

            // 遮罩一根骨都没命中 → 同样"这条不生效"，而不是全骨
            NlaStrip s = makeStrip("upper", 0.0f, 1.0f);
            s.mask.mode = BoneMaskMode::Allow;
            s.mask.roots = {"no_such_bone"};
            Pose r0;
            sampleInto(baseClip, 0.5f, r0);
            Pose r = r0;
            NlaScratch sc2;
            const std::vector<NlaTrack> tr2{makeTrack("t", s)};
            const NlaEvalStats st2 = applyNlaTimeline(*skel, tr2, 0.5f, r, sc2);
            reportFlag("nla_empty_mask_skipped",
                       st2.applied == 0 && st2.masksUnresolved == 1 &&
                           poseBitEqual(r0, r),
                       "unresolved=" + std::to_string(st2.masksUnresolved));

            // span 为 0（新建但还没拖过的 strip）→ 不算"问题"，直接跳过
            NlaStrip z = makeStrip("upper", 0.5f, 0.5f);
            Pose q0;
            sampleInto(baseClip, 0.5f, q0);
            Pose q = q0;
            NlaScratch sc3;
            const std::vector<NlaTrack> tr3{makeTrack("t", z)};
            const NlaEvalStats st3 = applyNlaTimeline(*skel, tr3, 0.5f, q, sc3);
            reportFlag("nla_zero_span_skipped",
                       st3.applied == 0 && st3.skipped == 1 &&
                           st3.firstProblem == "span:zero",
                       "problem=" + st3.firstProblem);
        }

        // ---- ⑬ 交叉淡入：两条 strip 在同一时刻各贡献一半 ----
        //
        // 三条都挑成可手算的：
        //   base 在 0.75 s 的 hand scale = 1.75（1→2 线性）
        //   strip A = upper [0,1]，在 0.75 s 采到 2.5（1→3 线性）
        //   strip B = upper [0.5,1.5]，在 0.75 s 采到 1.5（u=0.25）
        //   两条包络都是 smoothFade(0.5) = 0.5
        // 逐条盖上去：1.75 →(+0.5) 2.125 →(+0.5) 1.8125
        {
            NlaStrip a = makeStrip("upper", 0.0f, 1.0f);
            a.blendOut = 0.5f;
            NlaStrip b = makeStrip("upper", 0.5f, 1.5f);
            b.blendIn = 0.5f;
            NlaTrack T;
            T.name = "xfade";
            T.strips.push_back(a);
            T.strips.push_back(b);

            Pose p;
            sampleInto(baseClip, 0.75f, p);
            NlaScratch sc;
            const std::vector<NlaTrack> tr{T};
            const NlaEvalStats st = applyNlaTimeline(*skel, tr, 0.75f, p, sc);
            report("nla_crossfade_hand_scale",
                   static_cast<double>(handScale(p)), 1.8125, 1e-5);
            reportFlag("nla_crossfade_both_applied",
                       st.active == 2 && st.applied == 2 && st.skipped == 0,
                       "active=" + std::to_string(st.active) + ",applied=" +
                           std::to_string(st.applied));
        }

        // ---- ⑭ 摘要把顺序吃进去 ----
        {
            const std::vector<NlaTrack> a{
                makeTrack("A", makeStrip("base", 0.0f, 1.0f)),
                makeTrack("B", makeStrip("upper", 0.0f, 1.0f))};
            const std::vector<NlaTrack> b{
                makeTrack("B", makeStrip("upper", 0.0f, 1.0f)),
                makeTrack("A", makeStrip("base", 0.0f, 1.0f))};
            const unsigned long long da = nlaDigest(a);
            const unsigned long long db = nlaDigest(b);
            const unsigned long long da2 = nlaDigest(a);
            reportFlag("nla_digest_order",
                       da != db && da == da2,
                       "same=" + std::to_string(da == da2 ? 1 : 0));
            report("nla_duration", static_cast<double>(nlaTimelineDuration(a)),
                   1.0, 1e-6);
            report("nla_strip_count",
                   static_cast<double>(nlaStripCount(a)), 2.0, 0.0);
        }

        // ---- ⑮ 运行时接线：播放头推进 / 回绕 / 暂停 ----
        {
            AnimatorSystem sys;
            Fixture g;
            buildLayerFixture(g);
            ecs::AnimatorComponent& an = anim(g);
            an.nlaTracks.push_back(
                makeTrack("t", makeStrip("upper", 0.0f, 2.0f)));
            an.nlaTime = 0.0f;
            an.nlaSpeed = 1.0f;
            an.nlaPlaying = true;
            an.nlaLoop = true;

            runFrames(sys, g, 5, 0.1f);
            report("nla_runtime_advances", static_cast<double>(an.nlaTime), 0.5,
                   1e-4);
            // 真的混进去了（`ANIM-NLA` 的三个数）
            reportFlag("nla_runtime_applied",
                       sys.lastNlaCount() == 1 && sys.lastNlaApplied() == 1 &&
                           sys.lastNlaSkipped() == 0,
                       "strips=" + std::to_string(sys.lastNlaStripCount()) +
                           ",applied=" +
                           std::to_string(sys.lastNlaApplied()));

            // 暂停：时间不动
            an.nlaPlaying = false;
            const float frozen = an.nlaTime;
            runFrames(sys, g, 3, 0.1f);
            report("nla_runtime_paused", static_cast<double>(an.nlaTime),
                   static_cast<double>(frozen), 0.0);

            // 回绕：strip 只到 1.0，跑 12 帧 × 0.1 → 1.2 → 0.2
            an.nlaPlaying = true;
            an.nlaTracks.clear();
            an.nlaTracks.push_back(
                makeTrack("t", makeStrip("upper", 0.0f, 1.0f)));
            an.nlaTime = 0.0f;
            runFrames(sys, g, 12, 0.1f);
            report("nla_runtime_loop", static_cast<double>(an.nlaTime), 0.2,
                   1e-4);

            // 不循环 → 停在末端（`nlaLoop=false`）
            an.nlaLoop = false;
            an.nlaTime = 0.0f;
            runFrames(sys, g, 20, 0.1f);
            report("nla_runtime_no_loop_clamp",
                   static_cast<double>(an.nlaTime), 1.0, 1e-4);
        }
    }

    // ================================================================
    // 步态生成器（2026-10-30）：把一份手工 K 帧表变成参数化函数
    // ================================================================
    //
    // 这一组要证明的四件事，全都不看画面：
    //   ① **换命名还能认出腿** —— 同一具骨架用 Mixamo / Rigify 两套骨名，
    //      解析出的角色下标必须指到同一根骨。候选名表写错一个字，
    //      症状是"膝盖不弯"（小腿被解析成大腿），截图上看不出来。
    //   ② **脚真的不滑** —— 承重期侧向 / 竖直 / 直线偏差都是毫米数，
    //      直接断阈值。这是 K 帧表那条硬规则唯一能被机器验证的形态。
    //   ③ **半周期镜像 + 闭合** —— 帧 25 = 帧 1 的镜像；帧 49 == 帧 1。
    //   ④ **没有写死坐标轴** —— 整具骨架先绕 Z 转 37°、再挪开原点，
    //      所有读数必须一模一样。这条是这组里最有价值的一条：写死 ±Y
    //      的实现在前三条上可能全绿（夹具恰好就朝 −Y），只有它会红。
    {
        using animation::GaitRole;

        // ---- ① 角色解析 ----
        const auto skelM = makeHumanoidSkeleton(/*rigify=*/false);
        const auto skelR = makeHumanoidSkeleton(/*rigify=*/true);
        const animation::HumanoidRig rigM =
            animation::resolveHumanoidRig(*skelM);
        const animation::HumanoidRig rigR =
            animation::resolveHumanoidRig(*skelR);

        int resolvedM = 0, resolvedR = 0;
        for (int i = 0; i < animation::kGaitRoleCount; ++i) {
            if (rigM.joint[i] >= 0) ++resolvedM;
            if (rigR.joint[i] >= 0) ++resolvedR;
        }
        // 19 个角色在夹具里全都有名字对得上的骨。
        report("gait_rig_mixamo", static_cast<double>(resolvedM), 19.0, 0.0);
        report("gait_rig_rigify", static_cast<double>(resolvedR), 19.0, 0.0);
        // 小腿必须落到 `shin.L`（夹具下标 12），不是 `thigh.L`（11）——
        // "包含匹配"那一派实现会在这里返回 11，然后 IK 把整条腿拉直。
        report("gait_rig_shin_idx",
               static_cast<double>(
                   rigR.joint[static_cast<int>(GaitRole::LeftLowerLeg)]),
               12.0, 0.0);
        report("gait_rig_foot_idx",
               static_cast<double>(
                   rigR.joint[static_cast<int>(GaitRole::LeftFoot)]),
               13.0, 0.0);
        reportFlag("gait_rig_rigify_ok",
                   rigR.walkable() && rigM.walkable() && rigR.missing.empty(),
                   "missing=" + std::to_string(rigR.missing.size()));

        // 只有手臂的骨架：不是双足 → 明确拒绝，不许"生成一份站着不动的"。
        {
            const auto arm = makeArmChainSkeleton();
            const animation::HumanoidRig ra =
                animation::resolveHumanoidRig(*arm);
            reportFlag("gait_rig_nonhuman", !ra.walkable(),
                       "roles=" + std::to_string(ra.missing.size()));
        }

        // ---- ② 生成 + 不滑步 ----
        animation::GaitParams gp;   // 默认：48 帧 @24fps / 步长 0.6 / 62% 承重
        animation::EditClip clip;
        const animation::GaitReport r =
            animation::buildWalkCycle(*skelR, gp, clip);

        reportFlag("gait_build_ok", r.ok, "err=" + r.error);
        // 2 秒周期、48 帧 → 49 个键
        report("gait_duration", static_cast<double>(clip.duration), 2.0, 1e-6);
        // 17 根骨各一条旋转轨 + 髋的平移 = 18 条；18×49 = 882 个键。
        // 这两个数把"某根骨静默没写进去"直接钉住。
        report("gait_tracks", static_cast<double>(r.tracks), 18.0, 0.0);
        report("gait_keys", static_cast<double>(r.keys), 882.0, 0.0);

        // 重心：表 §9 的 ±0.03 → 峰峰 0.06。bob 是**半振幅**，写的时候
        // 最容易把峰峰当成半振幅填进去（差一倍，画面上只是"有点颠"）。
        report("gait_bob_range", static_cast<double>(r.bobRange), 0.06, 0.002);
        report("gait_sway_range", static_cast<double>(r.swayRange), 0.06, 0.002);
        // 抬脚峰值：48 帧里最接近 sin 顶点的那一帧取到 0.09998。
        report("gait_foot_lift", static_cast<double>(r.footLiftPeak), 0.10,
               0.002);

        // 承重期三个"不动"的量。表 §6 的硬规则：X/Z 完全不动，只动 toe。
        // 阈值 0.5 mm 是肉眼绝对看不出、但"插值没管约束"会立刻超标一个
        // 数量级的那个位置。
        report("gait_slide_mm", static_cast<double>(r.stanceSlideMm), 0.0, 0.5);
        report("gait_vert_mm", static_cast<double>(r.stanceVerticalMm), 0.0,
               0.5);
        report("gait_line_mm", static_cast<double>(r.stanceLineErrMm), 0.0, 0.5);

        // 闭合与镜像：构造保证（末帧直接取相位 0），所以只留浮点余量。
        report("gait_loop_mm", static_cast<double>(r.loopErrMm), 0.0, 0.05);
        report("gait_mirror_mm", static_cast<double>(r.mirrorErrMm), 0.0, 0.05);

        // 移动速度自洽：承重脚相对身体后退的速率 == nominalSpeed。
        // 差 1% 就会"每步滑 1 厘米"——这种滑在 1080p 上是看不见的。
        report("gait_speed_identity", static_cast<double>(r.stanceSpeed),
               static_cast<double>(r.nominalSpeed),
               static_cast<double>(r.nominalSpeed) * 0.01 + 1e-4);

        // 腿够不够长（>1 = IK 够不着 → 腿拉直、脚离地 → 看起来在滑）
        reportFlag("gait_reach", r.reachMargin > 0.0f && r.reachMargin < 1.0f,
                   "reach=" + std::to_string(r.reachMargin));
        // 触地帧到半周期后：左脚往前走 1.612A、左手往后走 2·臂长·sin(21°)
        // ≈ 0.38。**两条符号一起看**才是反相的判据（腿在前 + 手在后）；
        // 单看一条，"同手同脚"也能让其中一条为正。
        report("gait_leg_swing", static_cast<double>(r.legSwingM), 0.598, 0.01);
        reportFlag("gait_arm_counter", r.armCounterM > 0.3f,
                   "arm=" + std::to_string(r.armCounterM));

        // 原地走（treadmill）：髋在前向一动不动。表里唯一的根骨要求，
        // 而"每帧往前挪 2 cm"在单帧截图里完全看不出来。
        report("gait_fwd_drift_mm", static_cast<double>(r.fwdDriftMm), 0.0, 0.5);

        // 夹具的手臂本来就是垂着、伸直的 → 两个"中和导入姿势"的修正量
        // 都必须是 0。它们不是 0 就说明量错了（比如把向上当成了向下）。
        report("gait_arm_down", static_cast<double>(r.armDownDeg), 0.0, 1.0);
        // 夹具的静止姿势左右对称 → 镜像的"地板"是 0（见 GaitCycle.h）。
        report("gait_rest_mirror", static_cast<double>(r.restMirrorMm), 0.0,
               0.01);

        // ---- ③ blocking 模式：只打 1/7/13/19/25 那几个关键姿态 ----
        {
            animation::GaitParams bp = gp;
            bp.blocking = true;
            animation::EditClip bc;
            const animation::GaitReport br =
                animation::buildWalkCycle(*skelR, bp, bc);
            // 9 帧 × 18 轨 = 162 个键，且**全部**是 Extreme（表 §8 的
            // Blocking 只用关键姿态，中间的补间交给曲线）。
            report("gait_blocking_keys", static_cast<double>(br.keys), 162.0,
                   0.0);
            report("gait_blocking_extreme",
                   static_cast<double>(br.extremeKeys), 162.0, 0.0);
            // 关键姿态少了，重心曲线就变成"只有两个极值"——峰峰值掉一半。
            report("gait_blocking_bob", static_cast<double>(br.bobRange), 0.06,
                   0.002);
            // **对照组**（没有它上面那三个毫米数等于没测）：blocking 只在
            // 1/8 相位上打帧，中间靠曲线插值，承重期竖直漂移实测 8.8 mm。
            // 这一条不红，就说明"不滑步"那三个数是**盲的**——它们在任何
            // 输入下都报 0。这也是为什么最终交付走逐帧烘焙（`blocking`
            // 默认 false），而不是直接打表里那五帧。
            reportFlag("gait_blocking_slides", br.stanceVerticalMm > 2.0f,
                       "vert=" + std::to_string(br.stanceVerticalMm));
        }

        // ---- ④ 不变性：整具骨架转 37° + 平移，读数必须逐位同量级 ----
        {
            const auto rot = makeHumanoidSkeleton(/*rigify=*/true,
                                                  /*preRotateDeg=*/37.0f);
            animation::EditClip rc;
            const animation::GaitReport rr =
                animation::buildWalkCycle(*rot, gp, rc);
            float d = 0.0f;
            d = std::max(d, std::fabs(rr.bobRange - r.bobRange));
            d = std::max(d, std::fabs(rr.swayRange - r.swayRange));
            d = std::max(d, std::fabs(rr.footLiftPeak - r.footLiftPeak));
            d = std::max(d, std::fabs(rr.armCounterM - r.armCounterM));
            d = std::max(d, std::fabs(rr.reachMargin - r.reachMargin));
            report("gait_invariance", static_cast<double>(d), 0.0, 1e-4);

            // 毫米级的那三个也一并钉：写死 ±Y 的实现会在这里出几厘米。
            float dmm = 0.0f;
            dmm = std::max(dmm, std::fabs(rr.stanceSlideMm - r.stanceSlideMm));
            dmm = std::max(dmm, std::fabs(rr.stanceLineErrMm - r.stanceLineErrMm));
            dmm = std::max(dmm, std::fabs(rr.mirrorErrMm - r.mirrorErrMm));
            dmm = std::max(dmm, std::fabs(rr.loopErrMm - r.loopErrMm));
            report("gait_invariance_mm", static_cast<double>(dmm), 0.0, 0.05);
        }

        // ---- ⑤ 步幅匹配（A4）：量出来的 == 生成时说的 ----
        //
        // 这一组存在的理由：生成器报 `nominalSpeed`、运行时拿它算播放速率，
        // 中间隔着"存盘 → 读回 → 另一个结构体"。只要两边有一次实现分叉，
        // 表现就是"生成报告说不滑、Play 里在滑"，而两边各自看都对。
        {
            // ① 编辑片段（刚生成、没盘上）→ 量出来必须等于生成器的数
            const animation::StrideInfo se =
                animation::measureEditClipStride(*skelR, clip);
            reportFlag("stride_edit_ok", se.ok, "err=" + se.error);
            report("stride_edit_speed", static_cast<double>(se.nominalSpeed),
                   static_cast<double>(r.nominalSpeed),
                   static_cast<double>(r.nominalSpeed) * 0.02);
            // 贴地区间：生成参数是 stance=0.62，量出来 0.74（含脚跟/脚尖
            // 滚动那一段 —— 见 StrideMatch.h 对 dutyFactor 的说明）。
            // 判据用**区间**而不是点值：点值等于把实现里那个 50% 带宽
            // 写进期望，改带宽就要改期望，等于没测。
            // 它抓的是"参考系选错"这一类错（相对髋量 → 0.23）。
            reportFlag("stride_edit_duty",
                       se.dutyFactor > 0.55f && se.dutyFactor < 0.92f,
                       "duty=" + std::to_string(se.dutyFactor));
            // 夹具是**标准走路**（生成参数 stance=0.62、没有腾空）→
            // 两只脚同时离地的帧必须一个都没有。这条是"腾空判据"的阴性
            // 对照：跑段落上它必须变成非 0（见 verify_gait_cycle G 组）。
            report("stride_edit_air",
                   static_cast<double>(se.airborneFraction), 0.0, 0.0);
            // 交叉校验：脚相对身体的摆幅 × 2 / 周期，与"承重期脚速"应当
            // 同量级。容差给 30% 是因为**两者定义本来就不一样**：
            //   · 承重期脚速  = 脚在地上后退的速率（一步的位移 / 承重时长）
            //   · 摆幅法      = 脚前后总摆幅 × 2 / 整圈（含摆动期多走的）
            // 夹具上实测 0.742 vs 0.598（差 24%），方向对、量级对 —— 那正是
            // 这条断言要守的东西。它抓的是"中位数被离群速度带跑"这一类错
            // （偏差会过倍），而不是两个量的口径差。
            report("stride_edit_span", static_cast<double>(se.spanSpeed),
                   static_cast<double>(r.nominalSpeed),
                   static_cast<double>(r.nominalSpeed) * 0.30);

            // ② 换成"运行时那份"（assets::AnimationClip）—— 走的是另一条
            //    采样路径（samplePose），数必须一样。这是"存盘再读回"的
            //    替身：两条路上的插值实现不同，只有真的量过才知道同不同。
            assets::AnimationClip rt;
            rt.name = "Walk";
            rt.duration = clip.duration;
            for (const auto& t : clip.tracks) {
                const int j = skelR->findJoint(t.joint);
                if (j < 0) continue;
                assets::AnimTrack at;
                at.joint = j;
                at.path = t.path;
                at.times = t.times;
                at.values = t.values;
                at.step = t.step;
                rt.tracks.push_back(std::move(at));
            }
            const animation::StrideInfo sr =
                animation::measureClipStride(*skelR, rt);
            reportFlag("stride_clip_ok", sr.ok, "err=" + sr.error);
            report("stride_clip_speed", static_cast<double>(sr.nominalSpeed),
                   static_cast<double>(r.nominalSpeed),
                   static_cast<double>(r.nominalSpeed) * 0.02);

            // ③ 不是步态的东西必须**明确拒绝**（不是"量出 0.000"）。
            //    idle / 挥手 / 坐下都会被这条挡住，Locomotion 那边于是
            //    退回 1× 播放，与引入本功能之前一致。
            {
                assets::AnimationClip still;   // 空轨道 = 纯静止姿势
                still.name = "Idle";
                still.duration = 2.0f;
                const animation::StrideInfo si =
                    animation::measureClipStride(*skelR, still);
                reportFlag("stride_idle_rejected", !si.ok,
                           "speed=" + std::to_string(si.nominalSpeed));
                // 非人形骨架（只有手臂）→ 不可走 → 拒绝
                const auto arm = makeArmChainSkeleton();
                assets::AnimationClip dummy;
                dummy.duration = 1.0f;
                const animation::StrideInfo na =
                    animation::measureClipStride(*arm, dummy);
                reportFlag("stride_nonhuman_rejected", !na.ok,
                           "err=" + na.error);
            }

            // ④ 速率换算：速度 == 自然步速 → 1.0 倍、残差 0。
            //    "残差"是**夹住之后**还剩多少滑步，0 才是不滑。
            const float nom = r.nominalSpeed;
            {
                const animation::StrideRate same =
                    animation::strideRateFor(nom, nom, 0.6f, 1.7f);
                report("stride_rate_identity", static_cast<double>(same.rate),
                       1.0, 1e-4);
                report("stride_residual_identity",
                       static_cast<double>(same.residual), 0.0, 1e-4);
                reportFlag("stride_identity_unclamped", !same.clamped);

                // 1.5 倍速 → 1.5 倍播放、仍然不滑（这是"接上速度"的核心断言）。
                // ⚠ 用 1.5 不用 2：出厂上限就是 1.7，拿 2 去测会撞上限，
                //   测出来的是"夹住"而不是"跟得上"——第一次就是写 2 红的。
                const animation::StrideRate fast =
                    animation::strideRateFor(nom * 1.5f, nom, 0.6f, 1.7f);
                report("stride_rate_1p5x", static_cast<double>(fast.rate), 1.5,
                       1e-4);
                report("stride_residual_1p5x",
                       static_cast<double>(fast.residual), 0.0, 1e-4);

                // 10 倍速 → 夹到 1.7，残差 = 10/1.7 − 1 ≈ 4.88（还差 488%）
                const animation::StrideRate over =
                    animation::strideRateFor(nom * 10.0f, nom, 0.6f, 1.7f);
                report("stride_rate_clamped", static_cast<double>(over.rate),
                       1.7, 1e-4);
                report("stride_residual_clamped",
                       static_cast<double>(over.residual),
                       static_cast<double>(10.0f / 1.7f - 1.0f), 1e-3);
                reportFlag("stride_clamped_flag", over.clamped);

                // 没量出步速（0）→ 原样返回，一个字节都不改
                const animation::StrideRate none =
                    animation::strideRateFor(3.0f, 0.0f, 0.6f, 1.7f);
                report("stride_rate_nospeed", static_cast<double>(none.rate),
                       1.0, 0.0);
            }

            // ⑤ 端到端：喂进状态机，看它**真的写了** speed。
            //    夹具骨架只有这一段叫 "Walk" 的 clip → 速度 2× 自然步速时
            //    `sk.speed` 必须是 2.0。这条是"接线接上了"唯一能被机器
            //    看见的形态（前面四条都在函数层面，不碰组件）。
            {
                auto skelWalk = makeHumanoidSkeleton(/*rigify=*/true);
                skelWalk->clips.push_back(rt);
                // 还要一段 Idle：不然 idle 态会因为"clip 不存在"提前返回，
                // 而那段提前返回**在步幅匹配之前** —— 断言会看到"速度还是
                // 上一个值"，看起来像"没回落"，实际是压根没走到那一步。
                {
                    assets::AnimationClip idleRt;
                    idleRt.name = "Idle";
                    idleRt.duration = 2.0f;   // 空轨道 = 静止姿势
                    skelWalk->clips.push_back(idleRt);
                }

                ecs::SkinnedMeshComponent sk;
                sk.model = nullptr;          // 只用 speed 字段
                animation::LocomotionConfig cfg;
                animation::LocomotionRuntime lrt;
                const float target = nom * 1.5f;
                const animation::LocomotionFrame f =
                    animation::updateLocomotion(lrt, sk, nullptr, *skelWalk,
                                                target, true, 4.5f, 0.016f,
                                                cfg, "TEST");
                reportFlag("stride_loco_walk_state", f.state == 1,
                           "state=" + std::to_string(f.state));
                // 容差 6%：分母是**运行时量出来**的步速，它与生成器报的
                // 那个数允许差 2%（两条采样路径），比值再放大一次。
                report("stride_loco_rate", static_cast<double>(f.playbackRate),
                       1.5, 0.06);
                report("stride_loco_written", static_cast<double>(sk.speed), 1.5,
                       0.06);
                reportFlag("stride_loco_matched", f.strideMatched,
                           "nominal=" + std::to_string(f.strideSpeed));

                // 超出上限：3 倍自然步速而夹在 1.7 → 速率 1.7，且**残差
                // 必须大于 0**。残差为 0 就说明"夹住了却声称不滑"，
                // 那是最坏的一种绿（数字好看、脚在滑）。
                {
                    ecs::SkinnedMeshComponent skc;
                    animation::LocomotionRuntime lrtc;
                    const animation::LocomotionFrame fc =
                        animation::updateLocomotion(lrtc, skc, nullptr,
                                                    *skelWalk, nom * 3.0f,
                                                    true, 4.5f, 0.016f, cfg,
                                                    "TEST");
                    report("stride_loco_clamped_rate",
                           static_cast<double>(fc.playbackRate), 1.7, 0.02);
                    reportFlag("stride_loco_clamp_residual",
                               fc.strideClamped && fc.strideResidual > 0.3f,
                               "residual=" + std::to_string(fc.strideResidual));
                }

                // 关掉开关 → 回到 1×（对照组）。同一帧、同一速度，
                // 只改这一位：这就是"滑 / 不滑"的 A/B。
                animation::LocomotionConfig off = cfg;
                off.strideMatch = false;
                ecs::SkinnedMeshComponent sk2;
                animation::LocomotionRuntime lrt2;
                const animation::LocomotionFrame f2 =
                    animation::updateLocomotion(lrt2, sk2, nullptr, *skelWalk,
                                                target, true, 4.5f, 0.016f,
                                                off, "TEST");
                report("stride_loco_off_rate",
                       static_cast<double>(f2.playbackRate), 1.0, 0.0);
                report("stride_loco_off_written",
                       static_cast<double>(sk2.speed), 1.0, 0.0);

                // 站着不动（速度低于 walkSpeed）→ idle 态，播放速率必须
                // **回落到 1×**。不回落的症状是"停下后待机动作还在快进"。
                ecs::SkinnedMeshComponent sk3;
                sk3.speed = 1.7f;   // 模拟刚走完还留着 1.7 倍
                animation::LocomotionRuntime lrt3;
                const animation::LocomotionFrame f3 =
                    animation::updateLocomotion(lrt3, sk3, nullptr, *skelWalk,
                                                0.0f, true, 4.5f, 0.016f, cfg,
                                                "TEST");
                reportFlag("stride_loco_idle_state", f3.state == 0,
                           "state=" + std::to_string(f3.state));
                report("stride_loco_idle_rate", static_cast<double>(sk3.speed),
                       1.0, 0.0);
            }

            // ⑥ 相位速率曲线（A4b）：一圈之内怎么分配播放速度
            //
            // 这一组要证明两件不同的事，分开测：
            //   · 曲线**本身**的数学：均值 1（不改步频）、整圈闭合、摆动区
            //     恒为 1。这三条任意一条坏了，"不滑"与"步频"就会互相打架。
            //   · 曲线**真的抹平了起伏**：用一条人造的、故意不平整的曲线
            //     测 —— 夹具上的步态是构造出来的匀速步态，起伏太小，
            //     拿它当基准等于测了个寂寞（这正是"对照组"的意义）。
            {
                animation::StridePhaseCurve cv;
                const animation::StrideInfo sc =
                    animation::measureEditClipStride(*skelR, clip, &cv);
                reportFlag("stride_curve_ok", sc.ok && cv.valid(),
                           "bins=" + std::to_string(cv.bins) +
                               " err=" + sc.error);

                int stanceBins = 0;
                for (float v : cv.speed)
                    if (v > 0.0f) ++stanceBins;
                // ⚠ 夹具上实测 **32/32**（每个 bin 都有承重样本），这不是
                //   错：两只脚交替覆盖整圈，任意一个相位上总有一只脚在承重。
                //   所以这条只要求"承重 bin 足够多"（曲线才有意义），
                //   不要求"必须有摆动 bin"。
                reportFlag("stride_curve_bins", stanceBins >= 8,
                           "stance=" + std::to_string(stanceBins) + "/" +
                               std::to_string(cv.bins));

                float gMin = 0.0f, gMax = 0.0f, gMean = 0.0f;
                cv.stats(gMin, gMax, gMean);
                // 均值 = 1：这条曲线只**重新分配**一圈内的速度，不改变
                // 一圈的总时长（否则步频会随曲线一起变，"不滑"就成了
                // "用错的步频换来的不滑"）。
                report("stride_curve_mean", static_cast<double>(gMean), 1.0,
                       0.05);
                // 摆动区（speed<=0 的 bin）的 gain 必须**正好**是 1。
                float swingErr = 0.0f;
                for (int b = 0; b < cv.bins; ++b) {
                    if (cv.speed[static_cast<std::size_t>(b)] > 0.0f) continue;
                    const float ph = (static_cast<float>(b) + 0.5f) /
                                     static_cast<float>(cv.bins);
                    swingErr = std::max(swingErr, std::fabs(cv.gain(ph) - 1.0f));
                }
                report("stride_curve_swing_gain",
                       static_cast<double>(swingErr), 0.0, 0.02);

                // 整圈闭合：按 60 步把一圈推完，相位必须回到起点。
                // 不闭合的症状是"播放头每圈漂一点"，跑几分钟后脚相位与
                // 世界位移错开 —— 那种错只在长时间运行里显形。
                float p = 0.0f;
                for (int k = 0; k < 60; ++k) p = cv.advance(p, 1.0f / 60.0f);
                const float wrapErr =
                    std::min(std::fabs(p), std::fabs(p - 1.0f));
                report("stride_curve_loop", static_cast<double>(wrapErr), 0.0,
                       0.01);

                // 人造对照：承重期脚速有 ±30% 的正弦起伏。
                // 不开曲线时世界脚速的**相对起伏**就是这 30%；开了之后
                // 应当被压到 1/4 以下。
                animation::StridePhaseCurve bump;
                bump.bins = 16;
                bump.nominalSpeed = 1.0f;
                bump.speed.assign(16, 0.0f);
                for (int b = 0; b < 8; ++b) {
                    const float ph = (static_cast<float>(b) + 0.5f) / 16.0f;
                    bump.speed[static_cast<std::size_t>(b)] =
                        1.0f + 0.30f * std::sin(ph * 6.2831853f);
                }
                float loOn = 1.0e9f, hiOn = -1.0e9f;
                float loOff = 1.0e9f, hiOff = -1.0e9f;
                float sumOn = 0.0f;
                int cnt = 0;
                for (int b = 0; b < bump.bins; ++b) {
                    const float v = bump.speed[static_cast<std::size_t>(b)];
                    if (!(v > 0.0f)) continue;
                    const float ph = (static_cast<float>(b) + 0.5f) / 16.0f;
                    const float on = v * bump.gain(ph);
                    loOn = std::min(loOn, on);
                    hiOn = std::max(hiOn, on);
                    loOff = std::min(loOff, v);
                    hiOff = std::max(hiOff, v);
                    sumOn += on;
                    ++cnt;
                }
                // 相对起伏（峰峰 / 均值）—— 这样与速度单位无关。
                const float meanOn = (cnt > 0) ? sumOn / static_cast<float>(cnt) : 1.0f;
                const float spreadOn = (hiOn - loOn) / meanOn;
                const float spreadOff = (hiOff - loOff) /
                                        (0.5f * (hiOff + loOff));
                // 对照组：不开曲线时确实有明显起伏（实测 0.20 —— 因为
                // 正弦只铺在前 8 个 bin 上，峰峰 0.24 / 均值 1.15）。
                // 它不为 0 才说明下面那条不是空断言。
                reportFlag("stride_curve_spread_off", spreadOff > 0.15f,
                           "spreadOff=" + std::to_string(spreadOff));
                reportFlag("stride_curve_flattens", spreadOn < spreadOff * 0.25f,
                           "on=" + std::to_string(spreadOn) +
                               " off=" + std::to_string(spreadOff));
            }

            // ⑦ 斜坡：rate 的分子必须是**水平投影**
            //
            // 这一组不是"实现细节的测试"，而是把一条会静默出错 6~15% 的
            // 约定钉住：`moveSpeed` 传 3D 模长时，20° 坡上脚会比身体快
            // 6.4%，30° 快 15% —— 都还在"看着有点飘"的量级，不会有人报 bug。
            {
                const float nom = r.nominalSpeed;
                const float ground = nom;     // 沿坡面速度 = 自然步速
                const float deg = 20.0f;
                const float rad = deg * 3.14159265f / 180.0f;
                const float c = std::cos(rad);

                // 水平投影（对）vs 3D 模长（错）
                const animation::StrideRate right =
                    animation::strideRateFor(ground * c, nom, 0.6f, 5.0f, deg);
                const animation::StrideRate wrong =
                    animation::strideRateFor(ground, nom, 0.6f, 5.0f, deg);
                // 用错量 → 速率高 1/cos 倍；差 6.4%
                report("stride_slope_rate_ratio",
                       static_cast<double>(wrong.rate / right.rate),
                       static_cast<double>(1.0f / c), 1e-3);
                // 竖直残差 = 水平速度 × tanθ / nominal
                report("stride_slope_vert",
                       static_cast<double>(right.verticalResidual),
                       static_cast<double>((ground * c) * std::tan(rad) / nom),
                       1e-3);
                // **平地必须正好是 0**：不然每一段走路都会被报"上下打滑"，
                // 那个警告就没人看了。
                const animation::StrideRate flat =
                    animation::strideRateFor(ground, nom, 0.6f, 5.0f);
                report("stride_slope_zero",
                       static_cast<double>(flat.verticalResidual), 0.0, 0.0);
                // 坡角夹上限：80° 与 75° 必须给出**同一个**残差
                // （cos 在 80° 掉到 0.17，不夹的话残差会大得没有意义）。
                const animation::StrideRate steep =
                    animation::strideRateFor(ground, nom, 0.6f, 5.0f, 80.0f);
                const animation::StrideRate cap =
                    animation::strideRateFor(ground, nom, 0.6f, 5.0f, 75.0f);
                report("stride_slope_cap",
                       static_cast<double>(steep.verticalResidual),
                       static_cast<double>(cap.verticalResidual), 1e-4);
                // 坡度**不改变** rate（只报残差）—— 这是"不二次投影"的
                // 判据：同一份水平速度，带不带 slopeDeg 必须给出同一个
                // 速率。改了它，坡上会少播 6%。
                const animation::StrideRate noSlope = animation::strideRateFor(
                    ground * c, nom, 0.6f, 5.0f, 0.0f);
                report("stride_slope_rate_same",
                       static_cast<double>(right.rate),
                       static_cast<double>(noSlope.rate), 1e-6);
            }
        }
    }

    const int failures = g_fail;
    VK_LOG_INFO("ANIM-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return failures;
}

} // namespace animation
