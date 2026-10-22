// ============================================================
// assetio/ProcAnim.cpp —— 四段基础动画的程序化生成
//
// 所有角度都是"在关节**静止局部坐标系**里绕某根世界轴转"，因为 AutoRig
// 生成的骨架 restLocal 旋转恒为单位阵。旋转轴与符号的推导写在下面每个
// 函数旁边 —— 这一段的符号错一次就是"倒着走"，而倒着走在静帧截图里
// 完全看不出来，所以 `ProcAnimReport` 里那几个诊断量（脚的**前向**位移
// 范围、抬升高度）才是真正的验收点。
// ============================================================

#include "assetio/ProcAnim.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace assetio {

namespace {

constexpr float kPi = 3.14159265358979f;

glm::quat axisDeg(const glm::vec3& axis, float deg) {
    const float len = glm::length(axis);
    if (len < 1.0e-8f || std::fabs(deg) < 1.0e-6f)
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::angleAxis(deg * kPi / 180.0f, axis / len);
}

// 世界轴 / 朝向（全部从骨架自己推，见头文件）
struct Frame {
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    glm::vec3 down{0.0f, 0.0f, -1.0f};
    glm::vec3 fwd{0.0f, -1.0f, 0.0f};
    glm::vec3 left{1.0f, 0.0f, 0.0f};
    glm::vec3 swing{1.0f, 0.0f, 0.0f};   // +angle = 下垂肢体向前摆

    glm::vec3 armDownAxis(int side) const {
        return glm::normalize(glm::cross(left * static_cast<float>(side), down));
    }
};

// 关节名 → 下标
struct Joints {
    std::unordered_map<std::string, int> byName;
    int count = 0;
    int operator[](const char* n) const {
        const auto it = byName.find(n);
        return it == byName.end() ? -1 : it->second;
    }
    bool has(const char* n) const { return byName.find(n) != byName.end(); }
};

// 采样出一帧的局部旋转 / 平移，然后做一次 FK 求关节世界位置。
// 只用于自检 —— 与引擎的 `animation::poseToJoints` 同一套公式，但本模块
// 不能依赖 engine（assetio 是纯 CPU 的离线层，方向是 assetc → assetio）。
struct Skel {
    const SourceModel* m = nullptr;
    std::vector<glm::mat4> world;

    void solve(const std::vector<glm::quat>& rot,
               const std::vector<glm::vec3>& trans) {
        const std::size_t n = m->joints.size();
        world.assign(n, glm::mat4(1.0f));
        for (std::size_t i = 0; i < n; ++i) {
            const SourceJoint& j = m->joints[i];
            const glm::quat q = i < rot.size() ? rot[i] : glm::quat(1, 0, 0, 0);
            const glm::vec3 t =
                i < trans.size() ? trans[i] : glm::vec3(j.restLocal[3]);
            const glm::mat4 local =
                glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(q);
            world[i] = (j.parent >= 0 && static_cast<std::size_t>(j.parent) < i)
                           ? world[static_cast<std::size_t>(j.parent)] * local
                           : local;
        }
    }
    glm::vec3 pos(int j) const {
        if (j < 0 || static_cast<std::size_t>(j) >= world.size())
            return glm::vec3(0.0f);
        return glm::vec3(world[static_cast<std::size_t>(j)][3]);
    }
};

// 一帧姿势：每个关节的局部旋转 + Hips 的额外位移
struct PoseFrame {
    std::vector<glm::quat> rot;
    glm::vec3 hipsOffset{0.0f};
};

using PoseFn = void (*)(float phase, const Frame& f, const Joints& J,
                        PoseFrame& out, void* user);

struct ClipSpec {
    const char* name;
    float duration;
    bool loop;
};

// 把 PoseFn 采样成 SourceClip。
// 循环片段会**多采一帧**（t = N/fps 处与 t=0 完全同值）：采样器在
// duration 处回绕，没有这最后一帧的话，[最后一帧, duration] 这一小段
// 会"冻住"最多 1/fps 秒 —— 走路时表现为每步末尾轻微一顿。
void buildClip(SourceModel& model, const Skel& skel, const Frame& f,
               const Joints& J, const ClipSpec& spec, const ProcAnimOptions& opt,
               PoseFn fn, void* user, ProcAnimReport* rep) {
    const int frames = std::max(2, static_cast<int>(spec.duration * opt.fps + 0.5f));
    const int samples = spec.loop ? frames + 1 : frames;

    std::vector<PoseFrame> poses(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const float phase =
            static_cast<float>(i % frames) / static_cast<float>(frames);
        PoseFrame& p = poses[static_cast<std::size_t>(i)];
        p.rot.assign(model.joints.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        p.hipsOffset = glm::vec3(0.0f);
        fn(phase, f, J, p, user);
    }

    const int hips = J["Hips"];
    const glm::vec3 hipsRest =
        hips >= 0 ? glm::vec3(model.joints[static_cast<std::size_t>(hips)]
                                  .restLocal[3])
                  : glm::vec3(0.0f);

    SourceClip clip;
    clip.name = spec.name;
    clip.duration = spec.duration;

    const std::size_t nj = model.joints.size();
    for (std::size_t j = 0; j < nj; ++j) {
        bool varies = false;
        for (int i = 1; i < samples && !varies; ++i) {
            const glm::quat a = poses[0].rot[j];
            const glm::quat b = poses[static_cast<std::size_t>(i)].rot[j];
            varies = std::fabs(glm::dot(a, b)) < 0.9999995f;
        }
        if (!varies) continue;

        SourceTrack tr;
        tr.joint = static_cast<int>(j);
        tr.path = 1;   // Rotation
        tr.step = false;
        tr.times.reserve(static_cast<std::size_t>(samples));
        tr.values.reserve(static_cast<std::size_t>(samples));
        for (int i = 0; i < samples; ++i) {
            const glm::quat q = poses[static_cast<std::size_t>(i)].rot[j];
            tr.times.push_back(static_cast<float>(i) / opt.fps);
            tr.values.push_back(glm::vec4(q.x, q.y, q.z, q.w));
        }
        ++rep->totalTracks;
        rep->totalKeys += static_cast<int>(tr.times.size());
        clip.tracks.push_back(std::move(tr));
    }

    // Hips 位移（呼吸起伏 / 走路的上下颠 / 跳跃）
    if (hips >= 0) {
        bool varies = false;
        for (int i = 1; i < samples && !varies; ++i)
            varies = glm::length(poses[0].hipsOffset -
                                 poses[static_cast<std::size_t>(i)].hipsOffset) >
                     1.0e-6f;
        if (varies) {
            SourceTrack tr;
            tr.joint = hips;
            tr.path = 0;   // Translation
            tr.step = false;
            for (int i = 0; i < samples; ++i) {
                const glm::vec3 t =
                    hipsRest + poses[static_cast<std::size_t>(i)].hipsOffset;
                tr.times.push_back(static_cast<float>(i) / opt.fps);
                tr.values.push_back(glm::vec4(t, 1.0f));
            }
            ++rep->totalTracks;
            rep->totalKeys += static_cast<int>(tr.times.size());
            clip.tracks.push_back(std::move(tr));
        }
    }

    // 覆盖同名旧片段（重复生成时不堆叠）
    const std::string nm = spec.name;
    model.clips.erase(
        std::remove_if(model.clips.begin(), model.clips.end(),
                       [&](const SourceClip& c) { return c.name == nm; }),
        model.clips.end());
    model.clips.push_back(std::move(clip));

    rep->clipNames.emplace_back(nm);
    rep->durations.push_back(spec.duration);
    (void)skel;
}

// ---------------------------------------------------------------- 诊断
// 用 FK 求一圈里某关节相对 Hips 的"前向位移范围 / 抬升范围"。
// 前向取的是 fwd 方向的投影 —— 这正是"倒着走"唯一的可观测判据：
// 反了的话前向范围仍然是正的（取的是 range），但**相位**会错；所以
// 这里同时验证"左脚的极大值出现在 p≈0.25"（脚在最前）这个相位关系。
struct SampleProbe {
    float fwdMin = 1.0e9f, fwdMax = -1.0e9f;
    float zMin = 1.0e9f, zMax = -1.0e9f;
    float phaseAtFwdMax = 0.0f;
    float swingFwdVel = 0.0f;   // 摆动相（脚离地）平均前向速度（m/s）
    // 横向最远距离（相对 Hips 沿 left 轴）。抓"手往两边撇"用：
    // 摆臂是绕**竖直轴前后**转，横向应该基本不动；横向一大就说明关节转错了轴。
    float latAbsMax = 0.0f;
    // p=0 时的相对高度 —— 也就是**静止姿势**的参考值。
    // 有了它，"抬离地面多少"才能直接算：zMax - zAtStart。
    // （直接比 zMax - zMin 只能说明"腿动了"，说明不了"脚离地了"：蹲下去
    //   也会让范围变大，但那是往下走。）
    float zAtStart = 0.0f;
};

SampleProbe probeClip(SourceModel& model, const Frame& f, const Joints& J,
                      const ClipSpec& spec, const ProcAnimOptions& opt,
                      PoseFn fn, void* user, const char* jointName) {
    SampleProbe pr;
    Skel skel;
    skel.m = &model;
    const int hips = J["Hips"];
    const int j = J[jointName];
    if (hips < 0 || j < 0) return pr;

    const int frames = std::max(2, static_cast<int>(spec.duration * opt.fps + 0.5f));
    std::vector<glm::quat> rot(model.joints.size());
    std::vector<glm::vec3> trans(model.joints.size());
    std::vector<float> fw(static_cast<std::size_t>(frames));
    std::vector<float> zz(static_cast<std::size_t>(frames));

    for (int i = 0; i < frames; ++i) {
        const float phase = static_cast<float>(i) / static_cast<float>(frames);
        PoseFrame p;
        p.rot.assign(model.joints.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        p.hipsOffset = glm::vec3(0.0f);
        fn(phase, f, J, p, user);
        for (std::size_t k = 0; k < model.joints.size(); ++k) {
            rot[k] = p.rot[k];
            trans[k] = glm::vec3(model.joints[k].restLocal[3]);
        }
        trans[static_cast<std::size_t>(hips)] += p.hipsOffset;
        skel.solve(rot, trans);

        const glm::vec3 rel = skel.pos(j) - skel.pos(hips);
        const float v = glm::dot(rel, f.fwd);
        fw[static_cast<std::size_t>(i)] = v;
        zz[static_cast<std::size_t>(i)] = rel.z;
        if (i == 0) pr.zAtStart = rel.z;
        if (v > pr.fwdMax) {
            pr.fwdMax = v;
            pr.phaseAtFwdMax = phase;
        }
        pr.fwdMin = std::min(pr.fwdMin, v);
        pr.zMin = std::min(pr.zMin, rel.z);
        pr.zMax = std::max(pr.zMax, rel.z);
        pr.latAbsMax = std::max(pr.latAbsMax, std::fabs(glm::dot(rel, f.left)));
    }

    // 摆动相 / 支撑相的平均前向速度。
    //
    // 判据（真人走路的地面视角）：脚离地时**向前飞**（把脚从身后甩到
    // 身前），着地时相对髋**向后走**（身体越过支撑脚）。方向反了这一个
    // 数就整号翻过来 —— 而"前向位移范围"是抓不到它的，那是个对称量。
    {
        const float zMid = (pr.zMin + pr.zMax) * 0.5f;
        const float dtFrame = spec.duration / static_cast<float>(frames);
        float swingSum = 0.0f;
        int swingN = 0;
        for (int i = 0; i < frames; ++i) {
            const int i1 = (i + 1) % frames;
            const float v =
                (fw[static_cast<std::size_t>(i1)] -
                 fw[static_cast<std::size_t>(i)]) / dtFrame;
            if (zz[static_cast<std::size_t>(i)] > zMid) {
                swingSum += v;
                ++swingN;
            }
        }
        pr.swingFwdVel = swingN > 0 ? swingSum / static_cast<float>(swingN)
                                    : 0.0f;
    }
    return pr;
}

// ---------------------------------------------------------------- 四段动画

// ------- idle：呼吸 + 极小的重心起伏 -------
//
// 幅度必须**小**：站着不动却明显在晃，看起来像喝醉了。呼吸取 2.4 s
// 一个来回（成人静息呼吸 12~20 次/分 → 3~5 s 一次，2.4 s 偏快一点点，
// 但游戏里"看得出在呼吸"比"生理准确"重要）。
void poseIdle(float p, const Frame& f, const Joints& J, PoseFrame& out,
              void*) {
    const float s = std::sin(p * 2.0f * kPi);
    const float s2 = std::sin(p * 2.0f * kPi + 0.9f);

    // Hips：上下 6 mm + 左右 3 mm
    // ⚠ 垂直分量同样只向上（P5）：原来 `0.006*s` 是 ±6 mm，吐气那半段会
    //   低于静止姿势 → 站着也会把脚压进地面一点点。幅度小的确不明显，但
    //   "站着不动时脚该紧贴地面"是玩家一眼能看出来的事，没必要留这 6 mm。
    out.hipsOffset = f.up * (0.006f * (s + 1.0f) * 0.5f) +
                     f.left * (0.003f * s2);

    // 手臂自然下垂（所有片段都必须先做这一步：源网格是 T-pose，
    // 不放下手臂的话每一段动画都像稻草人）
    for (int side = 1; side >= -1; side -= 2) {
        const int arm = side > 0 ? J["LeftArm"] : J["RightArm"];
        const int fore = side > 0 ? J["LeftForeArm"] : J["RightForeArm"];
        const int hand = side > 0 ? J["LeftHand"] : J["RightHand"];
        if (arm >= 0)
            out.rot[static_cast<std::size_t>(arm)] =
                axisDeg(f.armDownAxis(side), 78.0f + 1.2f * s);
        if (fore >= 0)
            out.rot[static_cast<std::size_t>(fore)] =
                axisDeg(f.up, -12.0f * static_cast<float>(side));
        if (hand >= 0)
            out.rot[static_cast<std::size_t>(hand)] =
                axisDeg(f.up, -4.0f * static_cast<float>(side));
    }

    // 脊柱：吸气挺胸（绕 swing 负角 = 向前/挺起），吐气回落
    const int sp1 = J["Spine1"];
    const int sp2 = J["Spine2"];
    const int neck = J["Neck"];
    const int head = J["Head"];
    if (sp1 >= 0)
        out.rot[static_cast<std::size_t>(sp1)] = axisDeg(f.swing, -1.4f * s);
    if (sp2 >= 0)
        out.rot[static_cast<std::size_t>(sp2)] =
            axisDeg(f.swing, -1.0f * s) * axisDeg(f.up, 0.8f * s2);
    if (neck >= 0)
        out.rot[static_cast<std::size_t>(neck)] = axisDeg(f.swing, 0.8f * s);
    if (head >= 0)
        out.rot[static_cast<std::size_t>(head)] =
            axisDeg(f.swing, 1.0f * s) * axisDeg(f.up, -1.2f * s2);

    // 腿：膝盖微屈 5°（完全绷直的人站着像假人），重心左右移
    for (int side = 1; side >= -1; side -= 2) {
        const int upLeg = side > 0 ? J["LeftUpLeg"] : J["RightUpLeg"];
        const int leg = side > 0 ? J["LeftLeg"] : J["RightLeg"];
        const int foot = side > 0 ? J["LeftFoot"] : J["RightFoot"];
        const float shift = 0.6f * s2 * static_cast<float>(side);
        if (upLeg >= 0)
            out.rot[static_cast<std::size_t>(upLeg)] = axisDeg(f.swing, shift);
        if (leg >= 0)
            out.rot[static_cast<std::size_t>(leg)] = axisDeg(f.swing, -5.0f);
        if (foot >= 0)
            out.rot[static_cast<std::size_t>(foot)] = axisDeg(f.swing, 3.0f);
    }
}

// ---------------------------------------------------------------- 步态表
//
// 为什么不用正弦叠加：试过，而且数字会立刻出卖它 ——
// `thigh = A·sin(2πp)` 与 `knee = B·max(0,-sin(2πp))` 同相，于是**膝盖最大
// 弯曲正好发生在腿摆到最后方的那一刻**（趾离地瞬间），脚被打到身后高高
// 抬起。实测"脚相对髋的前向位移 0.93 m / 抬升 0.32 m"——真人在走的时候
// 脚最多离地 5~10 cm，那个数字说明腿在身后画了半个圆（俗称驴踢）。
//
// 改成手动画步态用的那套：**四个关键姿势 + 平滑插值**。每个姿势就是
// "此刻大腿多少度、膝盖多少度"，一眼能看出对错，也不会互相拉扯。
struct GaitKey {
    float thigh = 0.0f;   // + = 向前
    float knee = 0.0f;    // 负 = 屈（小腿向后折）
    float ankle = 0.0f;   // + = 勾脚（脚尖朝上）
    float toe = 0.0f;
    float arm = 0.0f;     // + = 向前
    float elbow = 0.0f;   // 屈肘角度（正数）
};

// p 处的姿势：在四个关键帧之间做**平滑**插值（smoothstep 而不是线性 ——
// 线性会让每个关键姿势处出现一个折角，肉眼就是"卡帧"）。
GaitKey sampleGait(const GaitKey* k, int n, float p) {
    p = p - std::floor(p);
    const float x = p * static_cast<float>(n);
    const int i0 = static_cast<int>(x) % n;
    const int i1 = (i0 + 1) % n;
    float t = x - std::floor(x);
    t = t * t * (3.0f - 2.0f * t);
    const GaitKey& a = k[i0];
    const GaitKey& b = k[i1];
    GaitKey r;
    r.thigh = a.thigh + (b.thigh - a.thigh) * t;
    r.knee = a.knee + (b.knee - a.knee) * t;
    r.ankle = a.ankle + (b.ankle - a.ankle) * t;
    r.toe = a.toe + (b.toe - a.toe) * t;
    r.arm = a.arm + (b.arm - a.arm) * t;
    r.elbow = a.elbow + (b.elbow - a.elbow) * t;
    return r;
}

// 走路（1.0 s 一个周期 = 两步；p=0 是**左脚跟着地**）
//              thigh  knee  ankle  toe   arm  elbow
const GaitKey kWalk[4] = {
    {24.0f, -6.0f, 4.0f, 0.0f, -20.0f, 20.0f},   // 0.00 脚跟着地
    {2.0f, -9.0f, 0.0f, 0.0f, -4.0f, 22.0f},      // 0.25 支撑中期（腿竖直）
    {-20.0f, -34.0f, -16.0f, -14.0f, 18.0f, 26.0f},  // 0.50 趾离地
    {10.0f, -52.0f, 10.0f, 0.0f, 2.0f, 20.0f},    // 0.75 摆动中期（收腿）
};

// 跑（0.7 s 一个周期；支撑相更短、摆动相膝盖收得更多、手臂大摆 + 肘 80°）
const GaitKey kRun[4] = {
    {30.0f, -18.0f, 2.0f, 0.0f, -40.0f, 75.0f},
    {2.0f, -26.0f, -14.0f, -10.0f, -5.0f, 80.0f},
    {-26.0f, -52.0f, -20.0f, -16.0f, 34.0f, 88.0f},
    {26.0f, -84.0f, 12.0f, 0.0f, 6.0f, 78.0f},
};

// 把一条腿 + 一条手臂的姿势套到骨架上。side: +1 = 左
//
// armDown = 从 T-pose 往下压的角度，**这是最容易调错的一个数**：
// 本项目骨架的 T-pose 是手臂水平伸直（指尖 lateral ≈ 0.74 m），压 72° 之后
// 还剩 18° 外张，从斜后上方看就像"张开翅膀滑翔"——而正面/侧面截图完全看不
// 出来（手确实在身侧、确实在摆）。真人自然站立时上臂距垂直约 8~10°，即
// 80°；跑步时手臂略收但不外张，也用 80°。判据见 ProcAnimReport 的
// walkHandLateral / runHandLateral（肩宽 0.19 m + 上臂 0.55 m，压在 80° 时
// 手应在 0.28 m 以内）。
void applyGaitSide(const Frame& f, const Joints& J, PoseFrame& out, int side,
                   const GaitKey& g, float armDown) {
    const int upLeg = side > 0 ? J["LeftUpLeg"] : J["RightUpLeg"];
    const int leg = side > 0 ? J["LeftLeg"] : J["RightLeg"];
    const int foot = side > 0 ? J["LeftFoot"] : J["RightFoot"];
    const int toe = side > 0 ? J["LeftToeBase"] : J["RightToeBase"];
    const int arm = side > 0 ? J["LeftArm"] : J["RightArm"];
    const int fore = side > 0 ? J["LeftForeArm"] : J["RightForeArm"];

    if (upLeg >= 0)
        out.rot[static_cast<std::size_t>(upLeg)] = axisDeg(f.swing, g.thigh);
    if (leg >= 0)
        out.rot[static_cast<std::size_t>(leg)] = axisDeg(f.swing, g.knee);
    if (foot >= 0)
        out.rot[static_cast<std::size_t>(foot)] = axisDeg(f.swing, g.ankle);
    if (toe >= 0)
        out.rot[static_cast<std::size_t>(toe)] = axisDeg(f.swing, g.toe);

    // 手臂：先下垂，再绕 swing 前后摆（两个旋转都在**静止局部系**里给，
    // 所以 q = q_swing · q_down 的顺序是对的：先把手放下来，再前后摆）
    if (arm >= 0)
        out.rot[static_cast<std::size_t>(arm)] =
            axisDeg(f.swing, g.arm) * axisDeg(f.armDownAxis(side), armDown);
    if (fore >= 0)
        out.rot[static_cast<std::size_t>(fore)] =
            axisDeg(f.up, -g.elbow * static_cast<float>(side));
}

// ------- walk：一个周期两步 -------
void poseWalk(float p, const Frame& f, const Joints& J, PoseFrame& out, void*) {
    const float w = 2.0f * kPi * p;

    // 上下颠：每周期两次（左右脚各落地一次），最低点在脚跟着地后一点。
    //
    // ⚠ 2026-10-22（P5）：**垂直分量只许向上**。
    //   原来是 `0.016*(-cos(2w)) - 0.016` → 范围 [-0.032, 0]，即"最低点比
    //   静止姿势低 32 mm"。而最低点恰好是支撑腿接近伸直的那一帧 → 腿长不变、
    //   髋却往下走，脚只能往地里去。玩家看到的就是"走路时脚陷进地面"。
    //   改成 [0, +16 mm] 的同振幅起伏：支撑相髋不下沉，脚自然贴地。
    out.hipsOffset = f.up * (0.016f * (1.0f - std::cos(2.0f * w)) * 0.5f) +
                     f.left * (0.013f * std::sin(w));

    // 骨盆绕竖直轴转：迈左腿时左侧髋向前
    const int hips = J["Hips"];
    if (hips >= 0)
        out.rot[static_cast<std::size_t>(hips)] =
            axisDeg(f.up, 3.0f * std::sin(w));

    // 脊柱反向旋转（抵消骨盆，肩膀与腿反向摆）
    const int sp1 = J["Spine1"];
    const int sp2 = J["Spine2"];
    if (sp1 >= 0)
        out.rot[static_cast<std::size_t>(sp1)] =
            axisDeg(f.up, -2.2f * std::sin(w));
    if (sp2 >= 0)
        out.rot[static_cast<std::size_t>(sp2)] =
            axisDeg(f.up, -1.8f * std::sin(w));

    const GaitKey l = sampleGait(kWalk, 4, p);
    const GaitKey r = sampleGait(kWalk, 4, p + 0.5f);
    applyGaitSide(f, J, out, 1, l, 80.0f);
    applyGaitSide(f, J, out, -1, r, 80.0f);
}

// ------- run：更大的摆幅 + 前倾 + 手臂大摆 -------
void poseRun(float p, const Frame& f, const Joints& J, PoseFrame& out, void*) {
    const float w = 2.0f * kPi * p;

    // 跑步有"腾空相"：整体重心在双脚离地时最高。用 2w 的余弦，
    // 并且只向上偏置（跑起来人是被"弹"起来的）。
    // ⚠ 与 walk 同一条纪律（P5）：原式 `0.042*(-cos(2w)) - 0.014` 的范围是
    //   [-0.056, +0.028]，最低点比静止低 56 mm —— 跑步的支撑相比走路更接近
    //   单腿伸直，脚陷得更明显。改成 [0, +42 mm]。
    out.hipsOffset = f.up * (0.042f * (1.0f - std::cos(2.0f * w)) * 0.5f) +
                     f.left * (0.009f * std::sin(w));

    const int hips = J["Hips"];
    if (hips >= 0)
        out.rot[static_cast<std::size_t>(hips)] =
            axisDeg(f.up, 5.0f * std::sin(w)) * axisDeg(f.swing, -3.0f);

    // 上身前倾 12°（脊柱三节分摊），头抬起来看前方
    const int sp0 = J["Spine"];
    const int sp1 = J["Spine1"];
    const int sp2 = J["Spine2"];
    const int neck = J["Neck"];
    if (sp0 >= 0) out.rot[static_cast<std::size_t>(sp0)] = axisDeg(f.swing, -5.0f);
    if (sp1 >= 0)
        out.rot[static_cast<std::size_t>(sp1)] =
            axisDeg(f.swing, -4.0f) * axisDeg(f.up, -4.0f * std::sin(w));
    if (sp2 >= 0)
        out.rot[static_cast<std::size_t>(sp2)] =
            axisDeg(f.swing, -3.0f) * axisDeg(f.up, -3.0f * std::sin(w));
    if (neck >= 0)
        out.rot[static_cast<std::size_t>(neck)] = axisDeg(f.swing, 9.0f);

    const GaitKey l = sampleGait(kRun, 4, p);
    const GaitKey r = sampleGait(kRun, 4, p + 0.5f);
    applyGaitSide(f, J, out, 1, l, 80.0f);
    applyGaitSide(f, J, out, -1, r, 80.0f);
}

// ------- jump：蹬伸离地 → 空中双脚收起 → 伸腿 → 落地缓冲 -------
//
// 2026-10-22 重做（用户要求："换成双脚抬起地面的那个跳跃"）。
//
// 两条硬约束：
//
// ① **不做大幅 Hips 上移**。位移是物理的（角色控制器有重力与初速 =
//    0.70 m / 0.75 s），动画再抬一次就变成"跳两倍高"。
//
// ② **clip 时长必须等于物理滞空时间**（0.75 s）。之前是 0.9 s，扣掉开头
//    的蹲下段之后，人在下落时动画还在"上升"，落地那一刻腿是伸直的 ——
//    看起来像"踩空了"。所以这次的相位是：
//        0.00~0.12  蹬伸（腿从落地姿势蹬直，人刚离地）
//        0.22~0.48  收腿（膝盖抬到胸前，**双脚明显离地**）
//        0.48~0.62  最高点，收得最紧
//        0.62~0.80  伸腿准备落地（脚往地面伸）
//        0.84~1.00  落地缓冲（屈膝吸收）
//    开头的"蹲下蓄力"被去掉了：物理的起跳是瞬时的（按下就飞），动画再蹲
//    0.2 s 会与人已经上升的头两帧打架（表现是"人先往上窜，腿还在蹲"）。
void poseJump(float p, const Frame& f, const Joints& J, PoseFrame& out,
              void* user) {
    const float* unused = static_cast<const float*>(user);
    (void)unused;

    // 分段权重（smoothstep，边界处导数为 0 —— 线性 ramp 会让关节角在
    // 相位交界处出现折角，肉眼看得见"抽一下"）
    auto ramp = [](float x, float a, float b) {
        if (x <= a) return 0.0f;
        if (x >= b) return 1.0f;
        const float t = (x - a) / (b - a);
        return t * t * (3.0f - 2.0f * t);
    };
    const float launch = ramp(p, 0.0f, 0.12f) * (1.0f - ramp(p, 0.18f, 0.34f));
    const float tuck = ramp(p, 0.22f, 0.48f) * (1.0f - ramp(p, 0.62f, 0.80f));
    const float land = ramp(p, 0.84f, 0.94f) * (1.0f - ramp(p, 0.97f, 1.0f));

    // 重心：蹬伸时被"拉长"一点点，收腿时**不下沉**（脚已经离地，下沉只会
    // 让屁股往下坠），落地时明显压低吸收冲击。
    out.hipsOffset = f.up * (0.040f * launch + 0.010f * tuck - 0.100f * land);

    // 脊柱：蹬伸时伸展（挺胸），收腿时前倾卷腹（空中抱膝的直觉），
    // 落地时前倾缓冲。
    const int sp0 = J["Spine"];
    const int sp1 = J["Spine1"];
    if (sp0 >= 0)
        out.rot[static_cast<std::size_t>(sp0)] =
            axisDeg(f.swing, -16.0f * land + 5.0f * launch - 9.0f * tuck);
    if (sp1 >= 0)
        out.rot[static_cast<std::size_t>(sp1)] =
            axisDeg(f.swing, -9.0f * land + 4.0f * launch - 6.0f * tuck);

    for (int side = 1; side >= -1; side -= 2) {
        const int upLeg = side > 0 ? J["LeftUpLeg"] : J["RightUpLeg"];
        const int leg = side > 0 ? J["LeftLeg"] : J["RightLeg"];
        const int foot = side > 0 ? J["LeftFoot"] : J["RightFoot"];
        const int arm = side > 0 ? J["LeftArm"] : J["RightArm"];
        const int fore = side > 0 ? J["LeftForeArm"] : J["RightForeArm"];

        //  thigh：收腿时**大幅前屈**（膝盖往胸口抬），这是"双脚离地"最直观
        //  的信号；蹬伸时轻微后展（0 → 负值 = 腿伸直并略向后）。
        //  ⚠ 数值是"抬到眼睛能看见脚掌离地"的门槛：58°/82°（旧值）时脚
        //    只是稍微离地，正面看还是"站着"；75°/100° 才像真的跳起来。
        const float thigh = 78.0f * tuck + 46.0f * land - 8.0f * launch;
        const float knee = 104.0f * tuck + 66.0f * land + 3.0f * launch;
        //  踝：空中脚尖下垂（自然放松），落地时勾脚（脚跟先着地）
        const float ankle = 26.0f * tuck + 22.0f * land - 12.0f * launch;
        if (upLeg >= 0)
            out.rot[static_cast<std::size_t>(upLeg)] = axisDeg(f.swing, thigh);
        if (leg >= 0)
            out.rot[static_cast<std::size_t>(leg)] = axisDeg(f.swing, -knee);
        if (foot >= 0)
            out.rot[static_cast<std::size_t>(foot)] = axisDeg(f.swing, ankle);

        // 手臂：起跳时向前上摆（借力），空中保持上举，落地时压下来。
        const float armSwing = 100.0f * launch + 62.0f * tuck - 18.0f * land;
        if (arm >= 0)
            out.rot[static_cast<std::size_t>(arm)] =
                axisDeg(f.swing, armSwing) *
                axisDeg(f.armDownAxis(side), 78.0f);
        if (fore >= 0)
            out.rot[static_cast<std::size_t>(fore)] =
                axisDeg(f.up, -(25.0f + 26.0f * tuck) *
                                   static_cast<float>(side));
    }
}

} // namespace

bool generateLocomotionClips(SourceModel& model, const ProcAnimOptions& opt,
                            ProcAnimReport* report, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (model.joints.empty()) return fail("no_skeleton (run --autorig first)");
    if (opt.fps < 5.0f || opt.fps > 240.0f) return fail("bad_fps");

    Joints J;
    J.count = static_cast<int>(model.joints.size());
    for (std::size_t i = 0; i < model.joints.size(); ++i)
        J.byName[model.joints[i].name] = static_cast<int>(i);

    // 必需关节：没有它们就生成不出可信的四肢
    for (const char* need : {"Hips", "LeftUpLeg", "RightUpLeg", "LeftArm",
                             "RightArm", "LeftFoot", "RightFoot"}) {
        if (!J.has(need)) {
            if (error)
                *error = std::string("missing_joint '") + need +
                         "'（骨架名必须与 AutoRig 的命名一致）";
            return false;
        }
    }

    // ---- 朝向 ----
    // 世界矩阵在静止姿势下就是 restLocal 的累乘；先用一个全单位旋转的
    // 姿势求一次，从关节自己的位置推 fwd / left。
    Skel skel;
    skel.m = &model;
    {
        std::vector<glm::quat> rot(model.joints.size(),
                                   glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        std::vector<glm::vec3> trans(model.joints.size());
        for (std::size_t i = 0; i < model.joints.size(); ++i)
            trans[i] = glm::vec3(model.joints[i].restLocal[3]);
        skel.solve(rot, trans);
    }

    Frame f;
    f.up = glm::vec3(0.0f, 0.0f, 1.0f);
    f.down = -f.up;
    {
        const int foot = J["LeftFoot"];
        const int toe = J["LeftToeBase"];
        glm::vec3 d(0.0f, -1.0f, 0.0f);
        if (foot >= 0 && toe >= 0) {
            glm::vec3 v = skel.pos(toe) - skel.pos(foot);
            v.z = 0.0f;
            if (glm::length(v) > 1.0e-4f) d = glm::normalize(v);
        }
        f.fwd = d;
        f.left = glm::normalize(glm::cross(f.up, f.fwd));
        f.swing = glm::normalize(glm::cross(f.down, f.fwd));
    }

    ProcAnimReport local;
    if (!report) report = &local;
    report->totalTracks = 0;
    report->totalKeys = 0;
    report->clipNames.clear();
    report->durations.clear();

    const ClipSpec idleSpec{"Idle", 2.4f, true};
    const ClipSpec walkSpec{"Walk", 1.0f, true};
    const ClipSpec runSpec{"Run", 0.7f, true};
    // Jump 的时长 = 物理滞空时间（0.75 s，见 poseJump 的头注释）。
    // 改这里必须同时改 `CharacterControllerComponent::jumpSpeed`
    // （t = 2·v₀/g → v₀ = 3.7 m/s 时 t = 0.754 s）。
    const ClipSpec jumpSpec{"Jump", 0.75f, false};

    if (opt.generateIdle)
        buildClip(model, skel, f, J, idleSpec, opt, &poseIdle, nullptr, report);
    if (opt.generateWalk)
        buildClip(model, skel, f, J, walkSpec, opt, &poseWalk, nullptr, report);
    if (opt.generateRun)
        buildClip(model, skel, f, J, runSpec, opt, &poseRun, nullptr, report);
    if (opt.generateJump)
        buildClip(model, skel, f, J, jumpSpec, opt, &poseJump, nullptr, report);

    // ---- 自检量 ----
    if (opt.generateWalk) {
        const SampleProbe w = probeClip(model, f, J, walkSpec, opt, &poseWalk,
                                       nullptr, "LeftToeBase");
        report->walkFootForwardRange = w.fwdMax - w.fwdMin;
        report->walkFootLift = w.zMax - w.zMin;
        report->walkFootFwdMaxPhase = w.phaseAtFwdMax;
        report->walkSwingFwdVel = w.swingFwdVel;
        const SampleProbe h = probeClip(model, f, J, walkSpec, opt, &poseWalk,
                                        nullptr, "LeftHand");
        report->walkHandForwardRange = h.fwdMax - h.fwdMin;
        report->walkHandLateral = h.latAbsMax;
    }
    if (opt.generateRun) {
        const SampleProbe r = probeClip(model, f, J, runSpec, opt, &poseRun,
                                       nullptr, "LeftToeBase");
        report->runFootForwardRange = r.fwdMax - r.fwdMin;
        report->runFootLift = r.zMax - r.zMin;
        const SampleProbe rh = probeClip(model, f, J, runSpec, opt, &poseRun,
                                        nullptr, "LeftHand");
        report->runHandForwardRange = rh.fwdMax - rh.fwdMin;
        report->runHandLateral = rh.latAbsMax;
    }
    if (opt.generateIdle) {
        // idle 的"根漂移"= Hips 在整段里的位移范围（应该只有几毫米）
        Skel s2;
        s2.m = &model;
        const int hips = J["Hips"];
        std::vector<glm::quat> rot(model.joints.size());
        std::vector<glm::vec3> trans(model.joints.size());
        float lo = 1.0e9f, hi = -1.0e9f, loX = 1.0e9f, hiX = -1.0e9f;
        for (int i = 0; i < 24; ++i) {
            PoseFrame p;
            p.rot.assign(model.joints.size(),
                         glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            poseIdle(static_cast<float>(i) / 24.0f, f, J, p, nullptr);
            for (std::size_t k = 0; k < model.joints.size(); ++k) {
                rot[k] = p.rot[k];
                trans[k] = glm::vec3(model.joints[k].restLocal[3]);
            }
            trans[static_cast<std::size_t>(hips)] += p.hipsOffset;
            s2.solve(rot, trans);
            const glm::vec3 pos = s2.pos(hips);
            lo = std::min(lo, pos.z);
            hi = std::max(hi, pos.z);
            loX = std::min(loX, glm::dot(pos, f.left));
            hiX = std::max(hiX, glm::dot(pos, f.left));
        }
        report->idleRootDrift = std::max(hi - lo, hiX - loX);
    }

    // ---- Jump：双脚离地（P6，2026-10-22）----
    //
    // 用趾尖（肢体最远端）而不是脚踝：抬腿时它最先离地，量出来的数最保守
    // 也最直观。`zAtStart` 是 p=0 的姿势 —— 那里 launch/tuck/land 三个权重
    // 全是 0，也就是**完全静止**，所以 zMax - zAtStart 就是"最多抬离地面多少"。
    if (opt.generateJump) {
        const SampleProbe j = probeClip(model, f, J, jumpSpec, opt, &poseJump,
                                        nullptr, "LeftToeBase");
        report->jumpFootLift = j.zMax - j.zAtStart;
        report->jumpFootPeakZ = j.zMax;
    }
    return true;
}

} // namespace assetio
