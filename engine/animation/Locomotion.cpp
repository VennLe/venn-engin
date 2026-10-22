// ============================================================
// animation/Locomotion.cpp —— 四态切换实现（编辑器与运行时共用）
//
// 判据、日志措辞、状态编号全部沿用 P3 那一版（`GAME-ANIM state=...`），
// 因为 `tools/verify_procanim.py` 就是按那些锚点断言的。抽成共享模块时
// **一个字符都没改** —— 改措辞会让一条已经绿了的用例变红，而后人只会看到
// "P3 挂了"，不会想到是重构动的。
// ============================================================

#include "animation/Locomotion.h"

#include "assets/Skeleton.h"
#include "core/Logger.h"

#include <algorithm>
#include <cmath>

namespace animation {

namespace {

const char* const kName[4] = {"idle", "walk", "run", "jump"};

// 强播"缺失"用的位（前四位被四个 locomotion 状态占着）
constexpr unsigned kTriggerMissingBit = 1u << 4;

// 按键强播的淡入时长。与 `LocomotionConfig::blendSeconds` 的出厂值同源 ——
// 强播没有 cfg 可读（它是"手动点播"，不属于状态机），所以写死一个常量，
// 但**必须与默认值相等**，否则同一台机器上"走路切换"和"按 K 起跳"的
// 过渡手感会不一致。
constexpr float kTriggerBlendSec = 0.20f;

// 把 clip / 播放头写到两处权威字段上。
//
// 为什么两处都写：没有 AnimatorComponent 时 `SkinnedMeshComponent` 的四项
// 是权威；有它时**它的**四项是权威（见 Components.h）。这里两边都写，
// 谁在权威位上都不会出现"改了不动"。
//
// `blendSec > 0` 且确实在换 clip 时，把旧的 clip 名与播放头记进
// `fadeFrom*`，由 AnimatorSystem 在采样阶段做双采样混合（P5）。
// ⚠ 旧播放头必须取**当时**的权威值（有 AnimatorComponent 时是 an->time），
//   取错会从旧片段的第 0 帧开始淡入 —— 腿会突然"缩回去再迈出来"。
void setClip(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
             const std::string& clip, bool restart, float blendSec) {
    if (sk.clipName != clip) {
        if (blendSec > 0.0f && !sk.clipName.empty()) {
            sk.fadeFromClip = sk.clipName;
            sk.fadeFromTime = an ? an->time : sk.time;
            sk.fadeT = 0.0f;
            sk.fadeDur = blendSec;
        } else {
            // 硬切（或第一次选 clip）：清掉残留的过渡，别让它淡进来
            sk.fadeFromClip.clear();
            sk.fadeT = 0.0f;
            sk.fadeDur = 0.0f;
        }
        sk.clipName = clip;
        if (restart) sk.time = 0.0f;
    }
    if (an) {
        if (restart) an->time = 0.0f;
        an->appliedDefaultClip = true;  // 我们亲自选了 clip，别再补 defaultClip
    }
}

void setPlaying(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
                bool playing) {
    sk.playing = playing;
    if (an) an->playing = playing;
}

void setLoop(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
             bool loop) {
    sk.loop = loop;
    if (an) an->loop = loop;
}

} // namespace

LocomotionFrame updateLocomotion(LocomotionRuntime& rt,
                                 ecs::SkinnedMeshComponent& sk,
                                 ecs::AnimatorComponent* an,
                                 const assets::Skeleton& skel,
                                 float horizSpeed, bool onGround,
                                 float maxSpeedHint, float dt,
                                 const LocomotionConfig& cfg,
                                 const char* tag) {
    LocomotionFrame f;
    const char* t = (tag && tag[0]) ? tag : "GAME";

    // ---- 0) 按键强播期间：状态机让位 ----
    //
    // 只倒计时、不选 clip。播完这一帧就把 loop 还原并**继续往下走**，
    // 让状态机在同一帧接管 —— 否则会多出一帧"卡在最后一帧"的空档。
    if (rt.triggerRemain > 0.0f) {
        rt.triggerRemain -= (dt > 0.0f ? dt : 0.0f);
        if (rt.triggerRemain > 0.0f) {
            f.state = rt.lastState < 0 ? 0 : rt.lastState;
            f.stateName = kName[f.state < 0 || f.state > 3 ? 0 : f.state];
            f.triggerActive = true;
            return f;
        }
        setLoop(sk, an, rt.restoreLoop);
        rt.triggerClip.clear();
    }

    // ---- 1) 选状态 ----
    float runThresh = cfg.runSpeed;
    if (runThresh <= 0.0f)
        runThresh = (maxSpeedHint > 0.0f ? maxSpeedHint : 4.5f) * 1.15f;

    int state = 0;
    std::string want = cfg.idleClip;
    if (!onGround && !cfg.jumpClip.empty()) {
        state = 3;
        want = cfg.jumpClip;
    } else if (horizSpeed >= runThresh && !cfg.runClip.empty()) {
        state = 2;
        want = cfg.runClip;
    } else if (horizSpeed > cfg.walkSpeed && !cfg.walkClip.empty()) {
        state = 1;
        want = cfg.walkClip;
    }
    f.state = state;
    f.stateName = kName[state];

    // ---- 2) 目标片段不存在 → 保持当前姿势，且**只喊一次** ----
    // 缺 clip 是配置问题（模型里没有这段动画），每帧喊等于没日志；
    // 而"悄悄不动"会让"角色像块石头"变成一次盲查。
    if (!want.empty() && !skel.findClip(want)) {
        const unsigned bit = 1u << static_cast<unsigned>(state);
        if ((rt.missingMask & bit) == 0u) {
            rt.missingMask |= bit;
            VK_LOG_WARN("%s-ANIM-MISSING state=%s clip='%s' 不在模型里"
                        "（共 %d 段），保持 '%s'",
                        t, kName[state], want.c_str(),
                        static_cast<int>(skel.clips.size()),
                        sk.clipName.c_str());
        }
        want.clear();
    }
    if (want.empty()) return f;

    // ---- 3) 计时器（只有 idle 才累计）----
    if (dt > 0.0f) {
        if (state == 0) {
            rt.idleHold += dt;
            rt.sinceIdleRetrigger += dt;
        } else {
            rt.idleHold = 0.0f;
            rt.sinceIdleRetrigger = 0.0f;
        }
    }

    // ---- 4) 套用 ----
    if (want != sk.clipName) {
        const std::string from = sk.clipName;
        setClip(sk, an, want, cfg.restartOnSwitch, cfg.blendSeconds);
        setPlaying(sk, an, true);
        setLoop(sk, an, true);   // 状态机的四段永远是循环的
        VK_LOG_INFO("%s-ANIM state=%s clip=%s (from='%s') speed=%.2f ground=%d "
                    "blend=%.2fs",
                    t, kName[state], want.c_str(), from.c_str(),
                    static_cast<double>(horizSpeed), onGround ? 1 : 0,
                    static_cast<double>(cfg.blendSeconds));
        f.switched = true;
        rt.clip = want;
    } else if (state != rt.lastState) {
        // clip 名没变但状态变了（比如 walk→run 都用同一段）→ 只记状态，
        // 不打"切换"（没有切换，打出来会误导）。
        VK_LOG_INFO("%s-ANIM state=%s clip=%s (same) speed=%.2f ground=%d",
                    t, kName[state], want.c_str(),
                    static_cast<double>(horizSpeed), onGround ? 1 : 0);
    }
    rt.lastState = state;

    // ---- 5) 静止够久 → 重放 idle ----
    //
    // 放在套用**之后**：保证"进入 idle"与"重放 idle"不会在同一帧各写一次
    // （同帧重放会让 clip 刚从 walk 切过来就跳到 0，步伐白切）。
    if (state == 0 && cfg.idleTriggerSec > 0.0f && dt > 0.0f &&
        rt.sinceIdleRetrigger >= cfg.idleTriggerSec) {
        rt.sinceIdleRetrigger = 0.0f;
        ++rt.idleRetriggers;
        // 重放同一段 → **硬切**（blend=0）：这里不是"换动作"，是"同一段
        // 从头再来"，淡入淡出会让它看起来卡一下。
        setClip(sk, an, want, true, 0.0f);
        setPlaying(sk, an, true);
        sk.time = 0.0f;
        if (an) an->time = 0.0f;
        VK_LOG_INFO("%s-ANIM-IDLE retrigger=%d idle=%.2fs clip=%s",
                    t, rt.idleRetriggers,
                    static_cast<double>(rt.idleHold), want.c_str());
        f.retriggered = true;
    }
    return f;
}

bool playTriggerClip(LocomotionRuntime& rt, ecs::SkinnedMeshComponent& sk,
                     ecs::AnimatorComponent* an,
                     const assets::Skeleton& skel, const std::string& clip,
                     float holdSeconds, const char* tag) {
    const char* t = (tag && tag[0]) ? tag : "GAME";
    if (clip.empty()) return false;

    const assets::AnimationClip* c = skel.findClip(clip);
    if (!c) {
        if ((rt.missingMask & kTriggerMissingBit) == 0u) {
            rt.missingMask |= kTriggerMissingBit;
            VK_LOG_WARN("%s-ANIM-KEY-MISSING clip='%s' 不在模型里（共 %d 段）",
                        t, clip.c_str(), static_cast<int>(skel.clips.size()));
        }
        return false;
    }

    float remain = holdSeconds > 0.0f ? holdSeconds : c->duration;
    if (remain <= 0.0f) remain = 0.5f;   // 零长度 clip 兜底，别一帧就弹回去

    // 已经在强播同一段 → 重新起播（"再按一次再来一遍"）
    rt.restoreLoop = sk.loop;
    rt.triggerClip = clip;
    rt.triggerRemain = remain;
    ++rt.triggerCount;

    // 强播也走过渡（从当前姿态淡入）：直接跳到 Jump 的第 0 帧看不出是
    // "起跳"还是"闪现"。
    setClip(sk, an, clip, true, kTriggerBlendSec);
    setPlaying(sk, an, true);
    setLoop(sk, an, false);   // 一次性动作：播到最后一帧就停（不循环）
    sk.time = 0.0f;
    if (an) an->time = 0.0f;

    VK_LOG_INFO("%s-ANIM-KEY clip=%s hold=%.2fs", t, clip.c_str(),
                static_cast<double>(remain));
    return true;
}

const std::string* pollKeyTrigger(const ecs::AnimKeyMapComponent& map,
                                  bool (*isKeyPressed)(int), int* keyOut) {
    if (!map.enabled || !isKeyPressed) return nullptr;
    for (const auto& e : map.entries) {
        if (e.key == 0 || e.clip.empty()) continue;
        // **边沿**判定：按住不放只触发一次（否则会每帧重置播放头，
        // 看上去像"卡在第一帧"）。
        if (isKeyPressed(e.key)) {
            if (keyOut) *keyOut = e.key;
            return &e.clip;
        }
    }
    return nullptr;
}

} // namespace animation
