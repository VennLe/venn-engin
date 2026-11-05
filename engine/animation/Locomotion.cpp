// ============================================================
// animation/Locomotion.cpp —— 四态切换实现（编辑器与运行时共用）
//
// 判据、日志措辞、状态编号全部沿用 P3 那一版（`GAME-ANIM state=...`），
// 因为 `tools/verify_procanim.py` 就是按那些锚点断言的。抽成共享模块时
// **一个字符都没改** —— 改措辞会让一条已经绿了的用例变红，而后人只会看到
// "P3 挂了"，不会想到是重构动的。
// ============================================================

#include "animation/Locomotion.h"

#include "animation/BlendSpace.h"
#include "animation/ClipSwitch.h"
#include "animation/StrideMatch.h"
#include "assets/Skeleton.h"
#include "core/Logger.h"

#include <algorithm>
#include <cmath>

namespace animation {

namespace {

const char* const kName[4] = {"idle", "walk", "run", "jump"};

const char* airPhaseName(int phase) {
    switch (phase) {
        case 1: return "rising";
        case 2: return "falling";
        case 3: return "land";
        default: return "ground";
    }
}

// 强播"缺失"用的位（前四位被四个 locomotion 状态占着）
constexpr unsigned kTriggerMissingBit = 1u << 4;

// 按键强播的淡入时长。与 `LocomotionConfig::blendSeconds` 的出厂值同源 ——
// 强播没有 cfg 可读（它是"手动点播"，不属于状态机），所以写死一个常量，
// 但**必须与默认值相等**，否则同一台机器上"走路切换"和"按 K 起跳"的
// 过渡手感会不一致。
constexpr float kTriggerBlendSec = 0.20f;

// 一段 clip 的时长。找不到 → 0（调用方据此放弃相位换算）。
float clipDuration(const assets::Skeleton& skel, const std::string& clip) {
    const assets::AnimationClip* c = skel.findClip(clip);
    return c ? c->duration : 0.0f;
}

// 播放头 → 归一化相位 [0,1)。时长 <= 0 → 0（没有"一圈"可言）。
float phaseOf(float time, float dur) {
    if (!(dur > 1.0e-6f)) return 0.0f;
    float p = time / dur;
    p -= std::floor(p);
    return p;
}

// 这张表只在地面态用。缓存键是"三段 clip 名 + 两个样本位置" —— 任何一个
// 变了就重建。不缓存的话每帧一次 `vector<string>` 分配，而这个函数在一个
// 40 个 NPC 的场景里每帧会被调 40 次。
const BlendSpace1D& groundSpaceFor(LocomotionRuntime& rt,
                                   const LocomotionConfig& cfg,
                                   float maxSpeedHint, float* walkAt,
                                   float* runAt) {
    float runThresh = cfg.runSpeed;
    if (runThresh <= 0.0f)
        runThresh = (maxSpeedHint > 0.0f ? maxSpeedHint : 4.5f) * 1.15f;
    // `walkAt` 自动值 = 控制器最大速度，即"走路满速时完全用 Walk 段"。
    // 为什么不是"Walk clip 的自然步速"：见 Locomotion.h 里 blendSpace 那段。
    const float w = cfg.walkAt > 0.0f
                        ? cfg.walkAt
                        : (maxSpeedHint > 0.0f ? maxSpeedHint : 4.5f);
    const float r = cfg.runAt > 0.0f ? cfg.runAt : runThresh;
    *walkAt = w;
    *runAt = r;

    if (rt.groundWalkAt != w || rt.groundRunAt != r ||
        rt.groundIdle != cfg.idleClip || rt.groundWalk != cfg.walkClip ||
        rt.groundRun != cfg.runClip) {
        rt.ground = makeGroundBlendSpace(cfg.idleClip, cfg.walkClip,
                                         cfg.runClip, w, r);
        rt.groundWalkAt = w;
        rt.groundRunAt = r;
        rt.groundIdle = cfg.idleClip;
        rt.groundWalk = cfg.walkClip;
        rt.groundRun = cfg.runClip;
    }
    return rt.ground;
}

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
                                 float verticalSpeed, float maxSpeedHint,
                                 float dt, const LocomotionConfig& cfg,
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
            f.airPhase = rt.airPhase;
            f.airName = airPhaseName(rt.airPhase);
            f.triggerActive = true;
            return f;
        }
        setClipLoop(sk, an, rt.restoreLoop);
        rt.triggerClip.clear();
    }

    // ---- 1) 空中相位：UE 的 Jump / Fall / Land ----
    //
    // 判据只有两个**物理真值**：是否贴地、竖直速度的方向。UE 的 ABP 就是
    // 这么分的（`Velocity.Z > 0` → Jump，否则 → Fall），不能用"滞空时长"
    // 或"速度大小"代替 —— 起跳瞬间竖直速度最大但正在上升，抛物线顶点
    // 速度为零却在下降。
    const int prevPhase = rt.airPhase;
    int phase = kAirGround;
    if (!onGround) {
        phase = (verticalSpeed > 0.0f) ? kAirRising : kAirFalling;
        rt.landRemain = 0.0f;   // 一离地就清掉落地的缓冲
    } else if (rt.landRemain > 0.0f) {
        rt.landRemain -= (dt > 0.0f ? dt : 0.0f);
        if (rt.landRemain > 0.0f) phase = kAirLanding;
    } else if ((prevPhase == kAirRising || prevPhase == kAirFalling) &&
               !cfg.landClip.empty() && cfg.landRecoverSec > 0.0f) {
        // 刚从空中落地 → 播一小段缓冲。没有 `landClip` 就不启用这一段
        // （退化，见 Locomotion.h 的"缺 clip 时的退化"）。
        rt.landRemain = cfg.landRecoverSec;
        phase = kAirLanding;
    }
    rt.airPhase = phase;
    f.airPhase = phase;
    f.airName = airPhaseName(phase);
    if (phase != prevPhase) ++rt.airSwitches;

    // ---- 2) 选段 ----
    int state = 0;
    std::string want;
    std::string other;      // 混合的另一段（空 = 只播一段）
    float wMain = 1.0f;     // `want` 的权重
    bool wantLoop = true;
    float walkAt = 0.0f;
    float runAt = 0.0f;

    if (phase == kAirGround) {
        if (!cfg.blendSpace) {
            // ---- A/B 对照：三态硬切（P4 的行为，一个字符都没改）----
            //
            // 留着这条路不是为了兼容，是为了**对照**：同一段动画、同一个
            // 速度，只改 `blendSpace` 这一位，"腿打结 / 不打结"就能被
            // 单独看见。删掉它以后，"混合空间到底解决了什么"就只剩传说。
            float runThresh = cfg.runSpeed;
            if (runThresh <= 0.0f)
                runThresh = (maxSpeedHint > 0.0f ? maxSpeedHint : 4.5f) * 1.15f;
            if (horizSpeed >= runThresh && !cfg.runClip.empty()) {
                state = 2;
                want = cfg.runClip;
            } else if (horizSpeed > cfg.walkSpeed && !cfg.walkClip.empty()) {
                state = 1;
                want = cfg.walkClip;
            } else {
                state = 0;
                want = cfg.idleClip;
            }
        } else {
            const BlendSpace1D& bs =
                groundSpaceFor(rt, cfg, maxSpeedHint, &walkAt, &runAt);
            if (bs.empty()) {
                // 一段地面动画都没有（空模型 / 三段名全对不上）→ 保持
                // 当前 clip，什么都不写。与引入混合空间之前的行为一致。
                f.state = 0;
                f.stateName = kName[0];
                return f;
            }
            const BlendResult br = bs.evaluate(horizSpeed);
            // 主段 = 权重 >= 0.5 的那一边。`state` 取它的**语义槽**，于是
            // 日志里的 `state=idle|walk|run` 与硬切时代逐字一致（老脚本里
            // `EDIT-ANIM state=walk clip=Walk` 这些锚点不用改）。
            const bool loIsMain = (br.wLo >= 0.5f);
            const int mainIdx = loIsMain ? br.lo : br.hi;
            const int otherIdx = loIsMain ? br.hi : br.lo;
            wMain = loIsMain ? br.wLo : (1.0f - br.wLo);
            want = bs.sample(mainIdx).clip;
            state = bs.sample(mainIdx).slot;
            // **带迟滞的混合开关**（进入 3% / 退出 0.5%）。
            //
            // 这不是性能优化，是**正确性**。速度刚从 0 起来时正落在样本点
            // 附近（本工程最常见的一种：编辑器 Play 的 `moveSpeed` 就是
            // `walkAt` 的自动值），而实测速度带抖动 —— 贴地校正、上坡、
            // 扫掠阻挡都会让 `|Δpos|/dt` 在 `moveSpeed` 上下漂 1~3%。
            // 单阈值下"另一段权重"就在 0 与 0.02 之间来回跳，于是
            // `blending()` 隔几帧翻转一次、采样路径跟着切、日志跟着打。
            // 实测代价：`verify_frame_pacing` 的尖刺从 0 帧涨到 50/129 帧
            // （中位间隔 4 帧），帧耗时双峰化成 9 ms / 27 ms。
            //
            // 迟滞用"当前有没有在混"（`sk.blendClip` 非空）当状态记忆：
            // 没在混要涨到 3% 才开始，已经在混要跌到 0.5% 以下才停。
            const float otherWeight = 1.0f - wMain;
            const bool wasBlending = !sk.blendClip.empty();
            const float enterWeight = wasBlending ? 0.005f : 0.03f;
            if (br.lo != br.hi && otherWeight >= enterWeight)
                other = bs.sample(otherIdx).clip;
        }
    } else {
        state = 3;
        wantLoop = false;   // UE：Jump / Fall / Land 都是**一次性**段
        switch (phase) {
            case kAirRising:
                want = cfg.jumpClip;
                break;
            case kAirFalling:
                // 没有 Fall 段就沿用 Jump 段。仍然**不循环** —— 从 20 m
                // 掉下来时循环起跳动作会让人在空中反复蹬腿。
                want = cfg.fallClip.empty() ? cfg.jumpClip : cfg.fallClip;
                break;
            default:
                want = cfg.landClip;
                break;
        }
    }

    f.state = state;
    f.stateName = kName[state];

    // ---- 3) 目标片段不存在 → 保持当前姿势，且**只喊一次** ----
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
        other.clear();
    }
    if (want.empty()) return f;

    // ---- 4) 计时器（只有**真的站在地上待机**才累计）----
    const bool trulyIdle = (phase == kAirGround && state == 0);
    if (dt > 0.0f) {
        if (trulyIdle) {
            rt.idleHold += dt;
            rt.sinceIdleRetrigger += dt;
        } else {
            rt.idleHold = 0.0f;
            rt.sinceIdleRetrigger = 0.0f;
        }
    }

    // ---- 5) 套用 ----
    //
    // `mainTime` 是**权威**播放头的引用（有 animator 时住它身上），下面
    // 相位换算必须写在这一个上 —— 只写 `sk.time` 的话，AnimatorSystem 下
    // 一帧会用 `an->time` 把换算结果覆盖掉，表现是"换段时相位没保住"。
    float& mainTime = an ? an->time : sk.time;
    if (want != sk.clipName) {
        const std::string from = sk.clipName;
        const float oldDur = clipDuration(skel, from);
        const float newDur = clipDuration(skel, want);
        // walk ↔ run 之间**保相位**（UE 的 Sync Group）：两段都是循环步态，
        // 相位一断就是"迈到一半突然换腿"。idle 不参与 —— 待机没有步态
        // 相位，从它身上继承会让起步从循环中段开始迈腿。
        const bool gaitPair =
            (from == cfg.walkClip || from == cfg.runClip) &&
            (want == cfg.walkClip || want == cfg.runClip);
        const bool keepPhase = gaitPair && cfg.blendSyncPhase && oldDur > 0.0f &&
                               newDur > 0.0f;
        const float ph = keepPhase ? phaseOf(mainTime, oldDur) : 0.0f;
        const bool restart = keepPhase ? false : cfg.restartOnSwitch;

        // 地面态**之间**不用 fade：过渡由混合空间承担。fade 的终点是
        // "纯新 clip"，与"两段加权"是两回事，同时做只会互相抵消。
        // 进/出空中态仍然用 fade（那是真正的"姿势跳转"，没有中间样本）。
        // 混合空间**关掉**时这一条必须失效 —— 硬切三态全靠 fade 才不平。
        const bool groundToGround =
            cfg.blendSpace && (phase == kAirGround) &&
            (from == cfg.idleClip || from == cfg.walkClip ||
             from == cfg.runClip) &&
            (want == cfg.idleClip || want == cfg.walkClip ||
             want == cfg.runClip);
        const float fadeSec = groundToGround ? 0.0f : cfg.blendSeconds;

        switchClip(sk, an, want, restart, fadeSec);
        setClipPlaying(sk, an, true);
        setClipLoop(sk, an, wantLoop);
        if (keepPhase) {
            const float nt = ph * newDur;
            sk.time = nt;
            if (an) an->time = nt;
        }
        VK_LOG_INFO("%s-ANIM state=%s clip=%s (from='%s') speed=%.2f ground=%d "
                    "blend=%.2fs",
                    t, kName[state], want.c_str(), from.c_str(),
                    static_cast<double>(horizSpeed), onGround ? 1 : 0,
                    static_cast<double>(fadeSec));
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

    // 空中相位翻转时单独留一条：它只改 `loop` 与相位，可能不换 clip
    // （Rising→Falling 共用 Jump 段时），光看 `-ANIM` 会以为什么都没发生。
    if (phase != prevPhase) {
        VK_LOG_INFO("%s-ANIM-AIR phase=%s clip=%s ground=%d vz=%+.2f "
                    "landLeft=%.2fs",
                    t, airPhaseName(phase), want.c_str(), onGround ? 1 : 0,
                    static_cast<double>(verticalSpeed),
                    static_cast<double>(rt.landRemain));
    }

    // ---- 6) 静止够久 → 重放 idle ----
    //
    // 放在套用**之后**：保证"进入 idle"与"重放 idle"不会在同一帧各写一次
    // （同帧重放会让 clip 刚从 walk 切过来就跳到 0，步伐白切）。
    if (trulyIdle && cfg.idleTriggerSec > 0.0f && dt > 0.0f &&
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

    // ---- 7) 混合段（只有地面态内部才有）----
    //
    // 写在步幅匹配**之前**：那一段有两个提前 return，放在后面会让
    // "刚好这一帧不需要步幅匹配"的实体丢掉混合。
    const std::string& mainClip = sk.clipName;
    // `other` 的取舍已经带了迟滞（见上面 `enterWeight`），这里只补两条
    // 结构性的排除：不在空中、且不是同一段。
    const bool groundBlend = (phase == kAirGround) && !other.empty() &&
                             other != mainClip;
    if (groundBlend) {
        sk.blendClip = other;
        sk.blendWeight = std::clamp(wMain, 0.0f, 1.0f);
        if (cfg.blendSyncPhase) {
            const float mainDur = clipDuration(skel, mainClip);
            const float otherDur = clipDuration(skel, other);
            if (mainDur > 0.0f && otherDur > 0.0f) {
                // 从属段的采样时刻 = 主段相位 × 从属段时长。用**归一化
                // 相位**而不是时间，是因为两段时长不同（Walk 1.03 s /
                // Run 0.63 s）—— 直接对齐秒数会让两段永远差一圈。
                sk.blendTime = phaseOf(mainTime, mainDur) * otherDur;
                sk.blendTimeLocked = true;   // 别再让采样层加一次 dt
            }
        }
        f.blended = true;
        f.blendClipName = other;
        f.blendWeight = sk.blendWeight;
        ++rt.blendSamples;
    } else {
        // 不混就**清干净**：留着旧的 `blendClip` 会让下一帧的采样把一段
        // 无关动画混进来（症状是"落地后腿上还挂着走路的抽动"）。
        sk.blendClip.clear();
        sk.blendWeight = 1.0f;
    }

    // ---- 8) 步幅匹配（A4）：把移动速度翻译成播放速率 ----
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
    //
    // ⚠ 混两段时 **两段共用一个速率**（取主段的）。UE 也是这么做的：
    //   BlendSpace 的 Rate 只有一个输入，相位对齐靠 Sync Group。各算各的
    //   会让相位立刻漂开 —— 那正是"腿打结"。
    const bool strideState = (state == 1 || state == 2);
    if (!strideState || !cfg.strideMatch) {
        setClipSpeed(sk, an, 1.0f);
        rt.strideWarned = false;
        return f;
    }

    const float nominal = nominalSpeedFor(rt, skel, mainClip, state, cfg, t);
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
                    t, mainClip.c_str(), static_cast<double>(nominal),
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
                        t, mainClip.c_str(), static_cast<double>(cfg.slopeDeg),
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
                    t, mainClip.c_str(), static_cast<double>(horizSpeed),
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
