// ============================================================
// animation/Locomotion.cpp —— 四态切换实现（编辑器与运行时共用）
//
// 判据、日志措辞、状态编号全部沿用 P3 那一版（`GAME-ANIM state=...`），
// 因为 `tools/verify_procanim.py` 就是按那些锚点断言的。抽成共享模块时
// **一个字符都没改** —— 改措辞会让一条已经绿了的用例变红，而后人只会看到
// "P3 挂了"，不会想到是重构动的。
// ============================================================

#include "animation/Locomotion.h"

#include "animation/ClipSwitch.h"
#include "animation/StrideMatch.h"
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

// 换 clip 时"量自然步速"的入口。
//
// 三级优先：配置覆盖（明确知道这段多少）→ 从 clip 里量 → 0（放弃匹配）。
// 缓存键是**clip 名**：换回同一段就不用再量（一次量要 60~120 帧全身 FK）。
float nominalSpeedFor(LocomotionRuntime& rt, const assets::Skeleton& skel,
                      const std::string& clip, int state,
                      const LocomotionConfig& cfg, const char* tag) {
    const float override =
        (state == 2) ? cfg.runStrideSpeed : cfg.walkStrideSpeed;
    if (override > 0.0f) {
        // 覆盖值也要进缓存，否则每帧都会走一遍上面的分支（无副作用，
        // 但会让 `strideMeasured` 这个计数失去意义）。
        if (rt.strideClip != clip) {
            rt.strideClip = clip;
            rt.strideSpeed = override;
            rt.strideWarned = false;
            ++rt.strideMeasured;
        }
        return rt.strideSpeed;
    }
    if (rt.strideClip == clip) return rt.strideSpeed;

    rt.strideClip = clip;
    rt.strideSpeed = 0.0f;
    rt.strideWarned = false;
    const assets::AnimationClip* c = skel.findClip(clip);
    if (!c) return 0.0f;
    const StrideInfo si = measureClipStride(skel, *c);
    if (!si.ok) {
        // 量不出来**不是错误**：这不是步态 clip（挥手 / 坐下 / idle），
        // 或者骨架不是人形。退回 1× 播放，与引入本功能之前逐位一致。
        VK_LOG_INFO("%s-STRIDE-SKIP clip=%s reason='%s'", tag, clip.c_str(),
                    si.error.c_str());
        return 0.0f;
    }
    rt.strideSpeed = si.nominalSpeed;
    ++rt.strideMeasured;
    // `air` = 两只脚同时离地的帧占比。0 = 走路（总有脚在地上），
    // 非 0 = 跑步。它与 `duty` 一起才是"这段是走还是跑"的可靠判据
    // （duty 单独看得不到结论 —— 见 StrideMatch.h 里记的那次实测）。
    VK_LOG_INFO("%s-STRIDE-MEASURE clip=%s nominal=%.3f cycle=%.2fs "
                "duty=%.2f air=%.2f samples=%d contact=%d span=%.3f/%.3f",
                tag, clip.c_str(), static_cast<double>(si.nominalSpeed),
                static_cast<double>(si.cycleSeconds),
                static_cast<double>(si.dutyFactor),
                static_cast<double>(si.airborneFraction), si.samples,
                si.contactSamples, static_cast<double>(si.strideSpan),
                static_cast<double>(si.spanSpeed));
    return rt.strideSpeed;
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
        setClipLoop(sk, an, rt.restoreLoop);
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
        switchClip(sk, an, want, cfg.restartOnSwitch, cfg.blendSeconds);
        setClipPlaying(sk, an, true);
        setClipLoop(sk, an, true);   // 状态机的四段永远是循环的
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
        switchClip(sk, an, want, true, 0.0f);
        setClipPlaying(sk, an, true);
        sk.time = 0.0f;
        if (an) an->time = 0.0f;
        VK_LOG_INFO("%s-ANIM-IDLE retrigger=%d idle=%.2fs clip=%s",
                    t, rt.idleRetriggers,
                    static_cast<double>(rt.idleHold), want.c_str());
        f.retriggered = true;
    }

    // ---- 6) 步幅匹配（A4）：把移动速度翻译成播放速率 ----
    //
    // 到这一步为止，状态机只解决了"该播哪一段"；播多快从来没人管过，
    // 于是 0.6 m/s 的走路动画配上 3 m/s 的移动速度 —— 脚在地上滑。
    // 这里把 `速度 ÷ 这段 clip 的自然步速` 写进 `speed`，位移与脚步对上。
    //
    // 只有 walk / run 参与：
    //   · idle 的"速度"是噪声（撞墙时可能是 0.01），不该被当成步频；
    //   · jump 的位移是抛物线，与脚步无关。
    // 这两态**显式写回 1×** —— 否则从 walk 切到 idle 之后会留着上一步的
    // 1.6 倍速把待机动作播快（"待机在快进"这种错没人会往步幅匹配上想）。
    const bool strideState = (state == 1 || state == 2);
    if (!strideState || !cfg.strideMatch) {
        setClipSpeed(sk, an, 1.0f);
        rt.strideWarned = false;
        return f;
    }

    const float nominal = nominalSpeedFor(rt, skel, want, state, cfg, t);
    if (!(nominal > 0.0f)) {
        // 量不出来（不是步态 clip / 非人形骨架）→ 1×，与引入本功能之前
        // 逐位一致。`nominalSpeedFor` 已经打过 STRIDE-SKIP，不重复喊。
        setClipSpeed(sk, an, 1.0f);
        rt.strideWarned = false;
        return f;
    }

    const StrideRate sr = strideRateFor(horizSpeed, nominal, cfg.strideMinRate,
                                       cfg.strideMaxRate, cfg.slopeDeg);
    setClipSpeed(sk, an, sr.rate);
    f.playbackRate = sr.rate;
    f.strideSpeed = nominal;
    f.strideResidual = sr.residual;
    f.strideVertical = sr.verticalResidual;
    f.strideMatched = true;
    f.strideClamped = sr.clamped;

    // 只在"换了 clip"或"夹持状态翻转"时打一行：逐帧打会把日志冲掉
    // （速度每帧都在变，一行一个数等于没有数）。
    const bool wasClamped = rt.strideWarned;
    if (f.switched || sr.clamped != wasClamped) {
        VK_LOG_INFO("%s-STRIDE clip=%s nominal=%.3f speed=%.3f rate=%.3f "
                    "residual=%+.1f%% vert=%.2f slope=%.1fdeg clamped=%d",
                    t, want.c_str(), static_cast<double>(nominal),
                    static_cast<double>(horizSpeed),
                    static_cast<double>(sr.rate),
                    static_cast<double>(sr.residual * 100.0f),
                    static_cast<double>(sr.verticalResidual),
                    static_cast<double>(cfg.slopeDeg), sr.clamped ? 1 : 0);
        // 斜坡上 rate 只能对齐水平分量。竖直那一份是**平地 clip 表达不了**
        // 的，报出来免得被当成"rate 算错了"（见 LocomotionConfig::slopeDeg）。
        if (sr.verticalResidual > 0.10f && f.switched) {
            VK_LOG_WARN("%s-STRIDE-SLOPE clip=%s slope=%.1fdeg 竖直方向落后"
                        "身体 %.2f×名义步速 → 平地 clip 没有爬升，只能靠"
                        "脚 IK（TwoBoneIKComponent）或换带抬脚的 clip",
                        t, want.c_str(), static_cast<double>(cfg.slopeDeg),
                        static_cast<double>(sr.verticalResidual));
        }
    }
    if (sr.clamped && !wasClamped) {
        rt.strideWarned = true;
        // 夹住 = 变速救不回来。两条出路都写进日志，别让人去猜：
        //   · 降移动速度（把 maxSpeed / moveSpeed 贴近自然步速）；
        //   · 换一段更快的 clip（walk → run），或调大生成器的步幅 / 步频。
        VK_LOG_WARN("%s-STRIDE-CLAMPED clip=%s 速度 %.2f m/s 超出这段动画"
                    "能表达的范围（自然步速 %.2f × [%.2f, %.2f]），"
                    "还剩 %.0f%% 滑步",
                    t, want.c_str(), static_cast<double>(horizSpeed),
                    static_cast<double>(nominal),
                    static_cast<double>(cfg.strideMinRate),
                    static_cast<double>(cfg.strideMaxRate),
                    static_cast<double>(sr.residual * 100.0f));
    } else if (!sr.clamped && wasClamped) {
        rt.strideWarned = false;
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
    switchClip(sk, an, clip, true, kTriggerBlendSec);
    setClipPlaying(sk, an, true);
    setClipLoop(sk, an, false);   // 一次性动作：播到最后一帧就停（不循环）
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
