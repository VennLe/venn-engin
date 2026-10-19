// ============================================================
// animation/AnimatorSystem.cpp
//
// A1（2026-10-19）：多了一条"AnimatorComponent 接管"的路径。两条路径
// 共用同一段推进 / 采样代码，只在**取字段**与**镜像回写**两处分叉 ——
// 复制一份循环出来的话，A2 加 crossfade 时必然只改到其中一份。
// ============================================================

#include "animation/AnimatorSystem.h"

#include "assets/Model.h"
#include "core/Logger.h"

#include <algorithm>
#include <cmath>

namespace animation {

namespace {

// 把 time 归一到 [0, dur)。负值也兜住（speed 可以被 Inspector 设成负数，
// 虽然滑块下限是 0，但组件是公开字段，别假定）。
float wrapTime(float t, float dur) {
    if (dur <= 0.0f) return 0.0f;
    float r = std::fmod(t, dur);
    if (r < 0.0f) r += dur;
    // fmod 在 t 恰好等于 dur 的整数倍时会给出 0，但浮点累积可能给出
    // dur - ε 或 dur 本身（后者会溢出一帧）。夹一下更稳。
    if (r >= dur) r = 0.0f;
    return r;
}

} // namespace

int AnimatorSystem::tick(scene::Scene& scene, float dt) {
    ++m_frames;

    ecs::World& w = scene.world();
    int count = 0;
    int animators = 0;
    int mismatched = 0;
    int wrapped = 0;
    // 日志用副本而不是 c_str()：组件里的 clipName 随时可能被 Inspector
    // 改掉，存指针有悬垂风险。（只报最后一个跨圈 / 最后一个 animator，
    // 够断言用。）
    std::string wrapClip;
    float wrapT = 0.0f;
    float wrapDur = 0.0f;
    std::string lastAnimClip;
    float lastAnimTime = 0.0f;
    float lastAnimSpeed = 1.0f;
    bool lastAnimPlaying = true;
    bool lastAnimLoop = true;

    const bool advance = dt > 0.0f;

    w.each<ecs::SkinnedMeshComponent>(
        [&](ecs::Entity ent, ecs::SkinnedMeshComponent& sk) {
            ecs::AnimatorComponent* an = w.get<ecs::AnimatorComponent>(ent);

            // ---- 0) 骨架来源 ----
            // 契约（`AnimatorComponent::model` 的注释）：给了就用它，空则回退
            // 用蒙皮组件那一份。
            // ⚠ 两者都给**且不是同一份** = 采样用的骨架与网格绑的骨架不同 →
            //   姿势会"对不上但不报错"（关节数一样时连越界都没有）。自动改不
            //   了，只能数出来喊一声。
            assets::Model* model = (an && an->model) ? an->model : sk.model;
            if (an && an->model && sk.model && an->model != sk.model) ++mismatched;
            if (!sk.model && model) sk.model = model;  // 让渲染侧也拿得到
            if (!model || !model->skeleton) return;
            const assets::Skeleton& skel = *model->skeleton;
            const int n = skel.jointCount();
            if (n <= 0) return;
            ++count;

            // ---- 1) Animator：defaultClip 补一次 ----
            // 只在**第一次见到这个实体**时补，且只在 clipName 空着时补。
            // 补过之后用户再把 clipName 清空 = 真的要看静止姿势，系统不许
            // 每帧把它填回去（否则 Inspector 上 "empty = rest pose" 是谎话）。
            if (an) {
                ++animators;
                if (!an->appliedDefaultClip) {
                    an->appliedDefaultClip = true;
                    if (sk.clipName.empty() && !an->defaultClip.empty()) {
                        sk.clipName = an->defaultClip;
                        VK_LOG_INFO("ANIM-CLIP-FROM-DEFAULT entity=%u clip=%s",
                                    static_cast<unsigned>(ent.id),
                                    sk.clipName.c_str());
                    }
                }
            }

            // ---- 2) 选权威字段 ----
            // 有 animator → 它的四项；没有 → 蒙皮组件那四项（A0 老路径，
            // 逐位不变）。三元表达式在这里是 **lvalue**，所以 `time` 是引用，
            // 推进直接落在权威字段上。
            float& time = an ? an->time : sk.time;
            const float speed = an ? an->speed : sk.speed;
            const bool playing = an ? an->playing : sk.playing;
            const bool loop = an ? an->loop : sk.loop;

            // clipName 空 → 静止姿势（skel.findClip 会返回 nullptr）
            const assets::AnimationClip* clip =
                sk.clipName.empty() ? nullptr : skel.findClip(sk.clipName);
            const float dur = clip ? clip->duration : 0.0f;

            // ---- 3) 推进时间 ----
            if (advance && playing) {
                time += dt * speed;

                if (dur > 0.0f) {
                    if (loop) {
                        // 只在真的跨圈时打日志（每帧刷会淹掉别的锚点）
                        if (time >= dur || time < 0.0f) {
                            time = wrapTime(time, dur);
                            ++wrapped;
                            wrapClip = sk.clipName;
                            wrapT = time;
                            wrapDur = dur;
                        }
                    } else {
                        // 不循环：停在末帧（拆前 sampleTrack 也是钳到端点）
                        time = std::clamp(time, 0.0f, dur);
                    }
                } else if (time < 0.0f) {
                    time = 0.0f;
                }
            }

            // ---- 4) 镜像回 SkinnedMeshComponent（animator 路径）----
            // 四项一起镜像，不只 time：任何还读 sk.* 的地方（Renderer 兜底、
            // 控制台、日志、断言）看到的必须是同一份真相。写入点只有这一处。
            if (an) {
                sk.time = an->time;
                sk.speed = an->speed;
                sk.playing = an->playing;
                sk.loop = an->loop;
                lastAnimClip = sk.clipName;
                lastAnimTime = an->time;
                lastAnimSpeed = an->speed;
                lastAnimPlaying = an->playing;
                lastAnimLoop = an->loop;
            }

            // ---- 5) 采样 → 关节矩阵（渲染层只负责搬运）----
            samplePose(skel, clip, time, m_pose);
            poseToJoints(skel, m_pose, sk.jointMatrices);
        });

    m_lastEntities = count;
    m_lastAnimators = animators;

    // ---- 自检锚点 ----
    // 每 60 帧一条：证明"系统真的在跑"（而不是每帧都要看到它）。
    if ((m_frames % 60) == 1) {
        VK_LOG_INFO("ANIM-TICK f=%llu entities=%d animators=%d dt=%.5f",
                    static_cast<unsigned long long>(m_frames), count, animators,
                    static_cast<double>(dt));
        // 走 Animator 路径的那一份（time 每帧都在变，只按 60 帧的节奏报）。
        if (animators > 0) {
            VK_LOG_INFO("ANIM-ANIMATOR clip=%s time=%.3f speed=%.2f "
                        "playing=%d loop=%d n=%d",
                        lastAnimClip.c_str(),
                        static_cast<double>(lastAnimTime),
                        static_cast<double>(lastAnimSpeed),
                        lastAnimPlaying ? 1 : 0, lastAnimLoop ? 1 : 0,
                        animators);
        }
        if (mismatched > 0) {
            VK_LOG_WARN("ANIM-MODEL-MISMATCH n=%d (animator->model 与 "
                        "skinned.model 不是同一份 → 姿势会对不上但不报错)",
                        mismatched);
        }
    }
    // 跨圈单独一条：fmod 的正确性靠它断（只报最后一次，避免高频刷屏）
    if (wrapped > 0) {
        VK_LOG_INFO("ANIM-WRAP clip=%s t=%.4f dur=%.4f n=%d", wrapClip.c_str(),
                    static_cast<double>(wrapT), static_cast<double>(wrapDur),
                    wrapped);
    }
    return count;
}

} // namespace animation
