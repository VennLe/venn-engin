// ============================================================
// animation/AnimatorSystem.cpp
//
// A1（2026-10-19）：多了一条"AnimatorComponent 接管"的路径。两条路径
// 共用同一段推进 / 采样代码，只在**取字段**与**镜像回写**两处分叉 ——
// 复制一份循环出来的话，A2 加 crossfade 时必然只改到其中一份。
// ============================================================

#include "animation/AnimatorSystem.h"

#include "animation/NlaTimeline.h"   // R9 NLA 求值器
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

// 交叉淡入淡出的权重曲线：smoothstep。
//
// 为什么不用线性：线性权重下**角速度在两端是突变的**，过渡的两头各有一个
// 肉眼可见的"顿挫"（也就是做了过渡仍然显得僵硬）。smoothstep 的导数在
// 0 与 1 处为 0，与两侧的稳态播放衔接得上 —— 这才是"看着顺"的关键。
//
// 2026-10-29（R9）：实现搬到 `animation::smoothFade`（Pose.h）—— NLA 的
// strip 交叉淡入是同一件事，两处各写一个三行式子，迟早有人只改一边，
// 然后就变成"层的淡入和片段的淡入手感不一样"这种没人会报的偏差。
// 表达式一字未改，所以行为逐位不变。
float fadeWeight(float t) { return smoothFade(t); }

} // namespace

const AnimController* AnimatorSystem::controllerFor(const std::string& path) {
    if (path.empty()) return nullptr;
    auto it = m_ctlCache.find(path);
    if (it != m_ctlCache.end()) return it->second.get();

    std::shared_ptr<AnimController> ctl = loadAnimController(path);
    // 失败也缓存（见头文件里的说明）：配错路径的实体不该每帧 fopen 一次。
    // 日志只在这里打一次，因为这里是一次性的加载点。
    if (!ctl->valid()) {
        VK_LOG_ERROR("ANIM-CTL-BAD path=%s err=[%s] → 状态机不接管，"
                     "回落到单 clip 直放",
                     path.c_str(), ctl->error.c_str());
    } else {
        VK_LOG_INFO("ANIM-CTL-LOAD path=%s states=%d params=%d transitions=%d "
                    "default=%s",
                    path.c_str(), static_cast<int>(ctl->states.size()),
                    static_cast<int>(ctl->params.size()),
                    static_cast<int>(ctl->transitions.size()),
                    ctl->stateName(ctl->defaultState));
    }
    it = m_ctlCache.emplace(path, ctl).first;
    return it->second.get();
}

const StridePhaseCurve* AnimatorSystem::strideCurveFor(
    const assets::Skeleton& skel, const assets::AnimationClip& clip) {
    // key = 骨架地址 + clip 名 + 时长（毫秒）。见头文件里的说明：
    // 不带时长的话，"同一个 clip 名被重新烤过"会留着旧相位的曲线。
    std::string key = std::to_string(static_cast<unsigned long long>(
        reinterpret_cast<uintptr_t>(&skel)));
    key += '\x1f';
    key += clip.name;
    key += '\x1f';
    key += std::to_string(static_cast<int>(std::lround(clip.duration * 1000.0f)));

    const auto it = m_strideCurves.find(key);
    if (it != m_strideCurves.end())
        return it->second.valid() ? &it->second : nullptr;

    StridePhaseCurve cv;
    const StrideInfo si = measureClipStride(skel, clip, &cv);
    float gMin = 1.0f, gMax = 1.0f, gMean = 1.0f;
    cv.stats(gMin, gMax, gMean);
    if (si.ok && cv.valid()) {
        VK_LOG_INFO("ANIM-STRIDE-CURVE clip=%s nominal=%.3f bins=%d "
                    "gain=[%.3f, %.3f] mean=%.3f",
                    clip.name.c_str(), static_cast<double>(si.nominalSpeed),
                    cv.bins, static_cast<double>(gMin),
                    static_cast<double>(gMax), static_cast<double>(gMean));
    } else {
        // 量不出来不是错误（挥手 / idle / 非人形骨架）→ 存一条 invalid，
        // 这条路径上 `time += dt * speed` 逐位不变。
        VK_LOG_INFO("ANIM-STRIDE-CURVE-SKIP clip=%s reason='%s'",
                    clip.name.c_str(),
                    si.ok ? "曲线为空（承重 bin 太少）" : si.error.c_str());
        cv = StridePhaseCurve{};
    }
    // 失败也缓存（见头文件的说明）：不缓存的话这个实体每帧重量一次。
    const auto ins = m_strideCurves.emplace(std::move(key), std::move(cv));
    return ins.first->second.valid() ? &ins.first->second : nullptr;
}

const SpringRig& AnimatorSystem::rigFor(const assets::Skeleton& skel,
                                        const std::vector<std::string>& prefixes) {
    RigKey key;
    key.skel = &skel;
    // 前缀拼起来当 key 的一部分。分隔符用 \x1f（不会出现在骨骼名里），
    // 否则 {"hair_"} 与 {"hair"} + {"_"} 这类组合会撞同一个 key。
    for (const std::string& p : prefixes) {
        key.prefixes += p;
        key.prefixes += '\x1f';
    }

    const auto it = m_rigCache.find(key);
    if (it != m_rigCache.end()) return it->second;

    // ⚠ 用 std::map 而不是 unordered_map：这里返回的是**引用**，而
    //   unordered_map 在 rehash 时会让所有元素引用失效（拿回一份悬垂的
    //   SpringRig，症状是头发位置读成随机数）。std::map 的节点地址稳定。
    //
    // 另外不能塞进 `ecs::SpringBoneComponent` 里缓存：那是场景数据，
    // 会跟着撤销快照与序列化走，而 SpringRig 是派生数据。
    auto ins = m_rigCache.emplace(key, buildSpringRig(skel, prefixes));

    // 只在**新建**时打一条。这是"接上了但头发一动不动"这一类问题的
    // 唯一直接证据：识别到 0 根 = 前缀没命中（真实资产 alicia 的骨叫
    // `hair1_L`，第一版默认前缀 `hair_` 就在这里栽过）。
    // 一次构建一条，角色数量是个位数，不会刷屏。
    {
        std::string pfx;
        for (const std::string& p : prefixes) {
            if (!pfx.empty()) pfx += ",";
            pfx += p;
        }
        if (pfx.empty()) pfx = "<default>";
        VK_LOG_INFO("ANIM-SPRING-RIG bones=%d joints=%d prefixes=[%s]",
                    ins.first->second.count(), skel.jointCount(), pfx.c_str());
    }
    return ins.first->second;
}

const BoneMask& AnimatorSystem::maskFor(const assets::Skeleton& skel,
                                       const BoneMaskSpec& spec) {
    // key = 骨架地址 + spec 摘要。地址进 key 是必须的：同一份 spec
    // （"spine"）落到不同骨架上，解出来的 weights 长度都不一样。
    std::string key = std::to_string(
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(&skel)));
    key += '\x1f';
    key += boneMaskKey(spec);

    const auto it = m_maskCache.find(key);
    if (it != m_maskCache.end()) return it->second;

    BoneMask m = buildBoneMask(skel, spec);
    // 只在**新建**时打一条（缓存命中不打了）。这是"配了层却一动不动"的
    // 唯一直接证据：`included=0` 一定是 root 名字没命中（真实资产的骨叫
    // `mixamorig:Spine1`，第一版默认 root 写 `spine_01` 就在这里栽过）。
    if (m.unresolved > 0 || m.empty()) {
        VK_LOG_WARN("ANIM-MASK-UNRESOLVED mode=%s roots=%d first=[%s] "
                    "joints=%d included=%d → 该 root 一根骨都没命中，"
                    "这一层等于不生效",
                    boneMaskModeName(spec.mode),
                    static_cast<int>(spec.roots.size()),
                    m.firstProblem.c_str(), skel.jointCount(), m.included);
    } else {
        VK_LOG_INFO("ANIM-MASK mode=%s roots=%d joints=%d included=%d",
                    boneMaskModeName(spec.mode),
                    static_cast<int>(spec.roots.size()), skel.jointCount(),
                    m.included);
    }
    return m_maskCache.emplace(std::move(key), std::move(m)).first->second;
}

int AnimatorSystem::applyLayers(const assets::Skeleton& skel, Pose& pose,
                                ecs::AnimatorComponent& an, float dt,
                                bool advance, uint32_t entityId) {
    int applied = 0;
    for (size_t li = 0; li < an.layers.size(); ++li) {
        ecs::AnimLayer& L = an.layers[li];

        // ---- 层关闭的三种形态，一律"什么都不做" ----
        // ① clip 空：配了一半的中间状态（见 Components.h 的约束 ③）
        // ② weight 0：用户把滑块拉到底 = 逐位回到没有这一层
        // ③ 遮罩一根骨都没命中：buildBoneMask 会给全 0，blend 直接短路
        if (L.clip.empty() || L.weight <= 0.0f) {
            // ⚠ 这里**不清** playingClip：清掉的话，用户把 weight 拉回 0.8
            //   会重新触发一次淡入（"拉滑块 → 动作从第一帧重来"）。
            continue;
        }

        const assets::AnimationClip* c = skel.findClip(L.clip);
        if (!c) {
            // 逐层只喊一次（用 playingClip 当"喊过了"的标记有点脏，所以
            // 这里改用 time < 0 的哨兵：不值得为此加一个字段，而每帧
            // fopen/刷日志的代价是整段日志没人看）。
            if (L.playingClip != L.clip) {
                VK_LOG_WARN("ANIM-LAYER-CLIP-MISSING layer=[%s] clip=[%s] "
                            "→ 该层跳过（findClip 是逐字比较）",
                            L.name.c_str(), L.clip.c_str());
                L.playingClip = L.clip;
            }
            continue;
        }

        // ---- 换 clip：重置播放头 + 重新开始淡入 ----
        if (L.playingClip != L.clip) {
            L.playingClip = L.clip;
            L.time = 0.0f;
            L.fadeT = L.fadeDur > 0.0f ? 0.0f : 1.0f;
            // ⚠ 事件的上一帧播放头也要归零（B2）。不归零的话，新 clip 的
            //   事件表会被"上一段停在哪"当起点 —— 第一个事件随机漏发。
            L.eventPrev = 0.0f;
        }

        // ---- 播放头 ----
        // 与底座同一个 wrapTime 语义（负 speed 也兜住、恰好整圈也归零）。
        const float dur = c->duration;
        const float evPrev = L.eventPrev;
        if (advance && L.playing) {
            L.time += dt * L.speed;
            if (dur > 0.0f) {
                if (L.loop) {
                    if (L.time >= dur || L.time < 0.0f)
                        L.time = wrapTime(L.time, dur);
                } else {
                    L.time = std::clamp(L.time, 0.0f, dur);
                }
            } else if (L.time < 0.0f) {
                L.time = 0.0f;
            }
        }
        // ⚠ 无论这一层最终有没有混进去，`eventPrev` 都要推进：权重被拉到 0
        //   的那段时间播放头仍在走（只是不参与混合），不推进的话下次把权重
        //   拉回来会一次性把中间整段的事件全吐出来（"凭空多出 40 次脚步声"）。
        L.eventPrev = L.time;

        // ---- 淡入 ----
        // ⚠ `!advance`（编辑器非 Play，dt=0）时**视为已经淡完**：
        //   否则用户刚配好一层、还没进 Play，在视口里看到的是 weight=0
        //   的姿态 —— 与"层没生效"完全一样，是这条功能最容易挨骂的地方。
        float fadeW = 1.0f;
        if (advance && L.fadeDur > 0.0f && L.fadeT < 1.0f) {
            L.fadeT = std::min(1.0f,
                               L.fadeT + dt / std::max(L.fadeDur, 1.0e-4f));
            fadeW = fadeWeight(L.fadeT);
        } else if (!advance) {
            L.fadeT = 1.0f;
        }

        const float w = L.weight * fadeW;
        if (w <= 0.0f) continue;

        // ---- 事件（B2）----
        // 只有"真的混进去了"的层才发事件：权重 0 的层在画面上不存在，
        // 它的脚步声也不该响（否则"关掉上层"变成"声音还在"）。
        if (!an.eventTracks.empty()) {
            const AnimEventTrack* tr = findAnimEventTrack(an.eventTracks, L.clip);
            if (tr) dispatchEvents(*tr, evPrev, L.time, dur, L.loop, entityId, L.name);
        }

        // ---- 采样 + 按遮罩混合 ----
        // ⚠ 采样进 `m_poseLayer` 而不是局部 Pose：见头文件的说明（分配）。
        samplePose(skel, c, L.time, m_poseLayer);
        const BoneMask& mask = maskFor(skel, L.mask);
        if (mask.empty()) continue;   // 一根骨都没命中，等价于没这一层
        blendPoseMasked(pose, m_poseLayer, mask, w, m_poseMix);
        // 就地接管：`m_poseMix` 与 `pose` 同址时这一步是自赋值（安全，
        // 见 blendPoseMasked 的约束 ③），不同址时是一次拷贝。
        pose = m_poseMix;
        ++applied;
    }
    return applied;
}

int AnimatorSystem::applyNla(const assets::Skeleton& skel, Pose& pose,
                             ecs::AnimatorComponent& an, float dt,
                             bool advance) {
    // 约定 ①：没有轨 = 什么都不做（`pose` 一个分量都不碰）
    if (an.nlaTracks.empty()) return 0;

    // ---- 播放头 ----
    //
    // 与底座同一个 `wrapTime` 语义（负 speed 也兜住、恰好整圈也归零）。
    // ⚠ `!advance`（编辑器非 Play）时**不推进**：那时候时间轴由面板的
    //   滑条 / 注入脚本控制，跟着 dt 走的话"拖到 3.2 s 松手就跳回去"。
    if (advance && an.nlaPlaying) {
        an.nlaTime += dt * an.nlaSpeed;
        const float dur = nlaTimelineDuration(an.nlaTracks);
        if (dur > 0.0f) {
            if (an.nlaLoop) {
                if (an.nlaTime >= dur || an.nlaTime < 0.0f)
                    an.nlaTime = wrapTime(an.nlaTime, dur);
            } else {
                an.nlaTime = std::clamp(an.nlaTime, 0.0f, dur);
            }
        } else if (an.nlaTime < 0.0f) {
            an.nlaTime = 0.0f;
        }
    }

    const NlaEvalStats st = applyNlaTimeline(skel, an.nlaTracks, an.nlaTime,
                                             pose, m_nlaScratch);
    ++m_lastNla;
    m_lastNlaStrips += st.strips;
    m_lastNlaApplied += st.applied;
    m_lastNlaSkipped += st.skipped;
    if (!st.firstProblem.empty() && m_lastNlaProblem.empty())
        m_lastNlaProblem = st.firstProblem;
    return st.applied;
}

void AnimatorSystem::dispatchEvents(const AnimEventTrack& track, float prevTime,
                                    float newTime, float duration, bool looped,
                                    uint32_t entityId, const std::string& layer) {
    collectAnimEvents(track, prevTime, newTime, duration, looped, m_eventScratch);
    if (m_eventScratch.empty()) return;

    const int total = static_cast<int>(m_eventScratch.size());
    const int n = std::min(total, kMaxEventsPerDispatch);
    for (int i = 0; i < n; ++i) {
        const AnimEvent& e =
            track.events[static_cast<size_t>(m_eventScratch[static_cast<size_t>(i)])];
        FiredAnimEvent f;
        f.entity = entityId;
        f.clip = track.clip;
        f.layer = layer;
        f.time = e.time;
        f.kind = e.kind;
        f.payload = e.payload;
        m_frameEvents.push_back(f);
        ++m_lastEvents;
        // 一帧一条、带**事件自身的时间**（不是播放头）。
        // ⚠ 这条是"序列不随帧率变"能被验证的原因：播放头在不同帧率下落在
        //   不同位置，而 `t=` 永远是作者填的那个数 → 两份日志可以逐条比对。
        VK_LOG_INFO("ANIM-EVENT entity=%u clip=%s layer=[%s] kind=%s t=%.4f "
                    "payload=[%s]",
                    static_cast<unsigned>(entityId), track.clip.c_str(),
                    layer.c_str(), e.kind.c_str(),
                    static_cast<double>(e.time), e.payload.c_str());
    }
    if (total > n) {
        VK_LOG_WARN("ANIM-EVENT-DROPPED n=%d cap=%d (一个 dt 跨过了太多事件："
                    "检查事件表时间是否挤在一起，或 dt 是否异常)",
                    total - n, kMaxEventsPerDispatch);
    }
}

bool AnimatorSystem::applyRootMotion(const assets::Skeleton& skel, Pose& pose,
                                     ecs::AnimatorComponent& an,
                                     ecs::TransformComponent* tf, bool wrapped,
                                     bool advance) {
    // 先清空：上一帧的 delta 不能漏到这一帧（消费方按"本帧有效"读）。
    an.rootDelta = RootMotionDelta{};

    const int root = resolveRootBone(skel, an.rootMotionBone);
    if (root < 0) {
        // 骨架一根骨都没有 / 名字没命中 → 整条链关掉，不报错（用户可能是
        // 刚把 mode 打开、骨名还没填）。
        // ⚠ 这里必须**清空** bone 名：不清的话锚点会报上一个实体的骨，
        //   而"填的名字找不到"看起来就像"填对了在跑"。
        m_lastRootBone.clear();
        m_rootBoneMissing = !an.rootMotionBone.empty();
        return false;
    }
    m_lastRootBone = skel.joints[static_cast<size_t>(root)].name;

    const glm::mat4 cur = boneWorldInSkeleton(skel, pose, root);
    const glm::vec3 curPos = boneWorldPosition(skel, pose, root);

    // ---- 第一帧：只记录基准，不提取 ----
    // ⚠ 人形骨架的骨盆离地约 1 m，把第一帧当成 `cur - 单位阵` 会让角色
    //   一进 Play 就往天上窜 1 米。这是根运动最经典的第一帧坑。
    if (!an.rootPrevValid) {
        an.rootPrevValid = true;
        an.rootPrevWorld = cur;
        an.rootBasePos = curPos;
        return false;
    }

    const bool horizontalOnly =
        an.rootMotion == ecs::AnimatorComponent::RootMotionMode::DeltaOnly;

    // ⚠ 两个"不要提取"的时刻，理由不同但后果一样（角色乱窜）：
    //   · `wrapped`：循环点根骨从"前方 1.5 m"瞬回原点 → 增量 -1.5 m，
    //     不丢就是"每走一步被拉回原地"（原地抽搐）
    //   · `!advance`：编辑器非 Play 拖时间轴 = 任意跳帧，每一跳都成了位移
    RootMotionDelta d;
    if (!wrapped && advance) {
        d = extractRootMotion(skel, an.rootPrevWorld, cur, horizontalOnly);
    }
    an.rootPrevWorld = cur;
    if (!d.valid) return false;
    an.rootDelta = d;

    // ---- "真的动了"的阈值 ----
    // ⚠ `RootMotionDelta::isZero()` 是**逐位**比较（它要保证 Off/端点那条
    //   逐位纪律），而浮点下"两帧之间零位移"几乎不会精确成立：实测原地
    //   动画的 dist 在 1e-6 量级抖动，于是 `n` 计数会在 0 和 1 之间来回跳
    //   （日志里 `n=1 dist=0.0000 yaw=0.00deg` —— 自相矛盾，看着像 bug）。
    //   这里用物理量级卡一道：0.1 毫米 / 0.01 度以下不算位移。
    //   （60 fps 下 0.1 mm/帧 = 0.006 m/s，比这慢的移动本来也看不出来。）
    constexpr float kMovedEps = 1.0e-4f;          // 米
    constexpr float kYawEps = 1.7453292e-4f;      // 0.01 度（弧度）
    const bool moved = glm::length(d.position) > kMovedEps ||
                       std::abs(d.yaw) > kYawEps;

    // ---- 钳回基准（in-place）----
    // ⚠ 无论哪种 mode 都要钳，而且**不是减本帧增量**（那是双倍位移，
    //   见 RootMotion.h）。钳完之后根相对实体恒为 0，位移全在实体上。
    // ⚠ 钳位用的是"当前相对基准的总偏移"，所以它会**累积**被扣的量；
    //   这也意味着跨圈那一帧虽然丢了增量，姿势该钳还是钳（根仍在基准）。
    const glm::vec3 stripped =
        stripRootMotion(skel, pose, root, curPos, an.rootBasePos, horizontalOnly);
    (void)stripped;

    if (!moved) return false;

    // ---- FromClip：引擎直接把位移写到实体上 ----
    // `DeltaOnly` 刻意**不写**：那是给"物理/CharacterController 自己算速度"
    // 的场景准备的，引擎替它移动会与物理打架（每帧两个写入点 = 抖动）。
    if (an.rootMotion == ecs::AnimatorComponent::RootMotionMode::FromClip &&
        tf) {
        tf->position += d.position;
        // 朝向写 `rotation.z`：TransformComponent 只存欧拉角，而引擎是
        // Z-up —— "绕竖轴转"就是绕 Z。写成 rotation.y 的话角色会绕
        // 前后轴翻跟头（第一次接上时踩过）。
        tf->rotation.z += d.yaw;
    }

    m_lastRootDist = glm::length(d.position);
    m_lastRootYaw = d.yaw;
    return true;
}

void AnimatorSystem::applyPerformance(const assets::Skeleton& skel, Pose& pose,
                                      uint32_t entityId, float dt,
                                      const ecs::LookAtComponent* la,
                                      const ecs::TwoBoneIKComponent* ik,
                                      const ecs::SpringBoneComponent* sb) {
    if (!la && !ik && !sb) return;

    // ① LookAt 先：头先转
    if (la && la->settings.enabled && la->settings.hasTarget) {
        const int bone = resolveLookAtBone(skel, la->settings.boneName);
        if (bone >= 0) {
            auto& st = m_lookAt[entityId];
            if (st.apply(skel, bone, dt, la->settings, pose) > 0) ++m_lastLookAt;
        }
    }

    // ② IK 中间：接触约束（脚落地 / 手握物）吃掉前面各层留下的位置误差。
    //
    // ⚠ 求解器是**无状态**的（见 TwoBoneIK.h），所以这里没有 `auto& st =
    //   m_ik[entityId]` 这种东西 —— 实体删了也不需要清理。
    // ⚠ `weight <= 0` 在 solver 里提前 return，`pose` 逐位不变。这条是
    //   "关掉 IK 逐像素回到基线"的唯一保证，**不要**在调用点再写一遍
    //   条件判断来"优化"（两处判断迟早不一致）。
    if (ik) {
        const TwoBoneIKResult r = solveTwoBoneIK(skel, ik->settings, pose);
        if (r.applied) {
            ++m_lastIK;
            // 2cm 是"肉眼看不出来"的阈值（roadmap A6 的验收线）。
            // 超出就记一次 —— 但**只记数**，不打日志：脚够不着地是常态
            // （角色站在台阶边缘），每帧一条会把别的锚点淹掉。
            if (r.errorAfter > 0.02f) ++m_lastIKMiss;
            // 留给 ANIM-IK 锚点（只报最后一个实体的账，够断言用）
            m_lastIKTarget = r.targetDist;
            m_lastIKActual = r.errorAfter;
            m_lastIKBend = r.bendAngleDeg;
            m_lastIKEndBone = skel.joints[static_cast<size_t>(r.end)].name;
        }
    }

    // ③ 弹簧骨后：头发跟着转过的头走。
    //    ⚠ 顺序不能反 —— 反过来时头发用的是"上一帧的头朝向"，
    //      快速转头会看到头发慢半拍（不细看很难发现，但会显得"怪"）。
    if (sb && sb->settings.enabled) {
        const SpringRig& rig = rigFor(skel, sb->prefixes);
        if (!rig.empty()) {
            auto& st = m_spring[entityId];
            // 第一帧 update 只初始化、返回 0 —— 所以"挂上组件的瞬间"
            // 与"没挂"逐像素相同（回归基线不会飘）。
            if (st.update(skel, rig, dt, sb->settings, pose) > 0) ++m_lastSpring;
        }
    }
}

void AnimatorSystem::cleanupStates(const std::vector<uint32_t>& seen) {
    if (m_spring.empty() && m_lookAt.empty()) return;
    // 角色数量是几十以内，线性查找比建 set 更快（也少一次分配）。
    auto alive = [&seen](uint32_t id) {
        return std::find(seen.begin(), seen.end(), id) != seen.end();
    };
    for (auto it = m_spring.begin(); it != m_spring.end();) {
        if (!alive(it->first)) it = m_spring.erase(it);
        else ++it;
    }
    for (auto it = m_lookAt.begin(); it != m_lookAt.end();) {
        if (!alive(it->first)) it = m_lookAt.erase(it);
        else ++it;
    }
}

int AnimatorSystem::tick(scene::Scene& scene, float dt) {
    ++m_frames;

    // ---- 物理用的时间步（2026-10-25，弹簧骨）----
    //
    // ⚠ 这里有一个**在编辑器里一定踩**的问题：
    //   编辑器非 Play 态调用 tick 时传的是 `dt = 0`（设计如此 —— "不推进
    //   动画时间，只按当前 time 重算姿势"，用户拖时间轴要立刻看到）。
    //   而弹簧骨是**时间相关**的物理：dt=0 那一帧等于时间静止，质点永远
    //   不动。症状是"加了弹簧骨组件、头发一动不动"，而日志里
    //   ANIM-SPRING-RIG bones=27 一切正常 —— 极难从日志反推。
    //
    //   所以物理**不复用**动画的 dt：
    //     dt > 0（Play / 运行时 / 自检）→ 用 dt，确定性优先
    //        （自检里 tick 是紧密循环，wall clock 会算出 ~0，必须走这条）
    //     dt == 0（编辑器非 Play）      → 用真实帧间隔，让用户看得见效果
    //
    //   这不是"给编辑器开后门"：Unity 的 DynamicBone / UE 的 AnimDynamics
    //   在编辑器里也是持续模拟的。动画时间暂停 ≠ 物理时间暂停。
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_frames == 1) m_lastWall = now;
        const float raw =
            std::chrono::duration<float>(now - m_lastWall).count();
        m_lastWall = now;
        // 钳住：窗口拖动 / 断点恢复 / 首帧 会有几百毫秒到几秒的间隔，
        // 直接喂给 Verlet 会一步炸开（位置飞出画面，且再也不会回来）。
        m_wallDt = std::clamp(raw, 0.0f, 0.05f);
    }
    const float physDt = (dt > 0.0f) ? dt : m_wallDt;

    ecs::World& w = scene.world();
    int count = 0;
    int animators = 0;
    int mismatched = 0;
    int wrapped = 0;
    int fading = 0;   // 本帧处于过渡中的实体数（ANIM-FADE 用）
    int strideCurved = 0;   // 本帧按相位曲线推进了播放头的实体数（A4b）
    int layersConfigured = 0;   // 配了附加层的实体数（B1）
    int layersApplied = 0;      // 其中真的混进去至少一层的
    // NLA（R9）同理：这里清一次，`applyNla` 里面累加。
    // ⚠ 清在这里而不是汇总区 —— 汇总区在 `applyNla` 之后，会把刚累加出来
    //   的数抹成 0（`m_rootBoneMissing` 那条注释记着同一个坑）。
    m_lastNla = 0;
    m_lastNlaStrips = 0;
    m_lastNlaApplied = 0;
    m_lastNlaSkipped = 0;
    m_lastNlaProblem.clear();
    int rootMotions = 0;        // 本帧真的提到了非零根位移的实体数（B3）
    int rootMotionCfg = 0;      // 本帧配了 rootMotion != Off 的实体数（B3）
    // 本帧的事件队列（B2）。**必须在遍历前清空**：消费方约定"同一帧读完"，
    // 攒着会让音效晚一帧、伤害判定在死亡之后才结算。
    m_frameEvents.clear();
    m_lastEvents = 0;
    m_lastRootMotion = 0;
    // ⚠ 必须在这里（tick 开头）清，不能放在遍历之后的汇总区 —— 那个位置
    //   在 `applyRootMotion` **之后**，会把这一帧刚设上的 true 立刻抹掉，
    //   于是 BONE-MISS 永远不打（第一版就是这么写的，端到端 ② 直接红）。
    m_rootBoneMissing = false;
    m_lastRootDist = 0.0f;
    m_lastRootYaw = 0.0f;
    m_lastSpring = 0;
    m_lastLookAt = 0;
    m_lastIK = 0;
    m_lastIKMiss = 0;
    m_lastIKTarget = 0.0f;
    m_lastIKActual = 0.0f;
    m_lastIKBend = 0.0f;
    m_lastIKEndBone.clear();
    m_lastControllers = 0;
    m_lastTransitions = 0;
    // 本帧见过的蒙皮实体。帧尾据此清掉"已经被删掉的角色"的物理状态，
    // 否则长时间编辑会一直涨（每个状态是几条 vec3，但会无界累积）。
    std::vector<uint32_t> seen;
    seen.reserve(16);
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
    // 本帧"名字填了但骨架里找不到"的实体数。`samplePose(nullptr)` 会**静默
    // 退回 rest 姿势**，所以这个数不为 0 时画面上看到的是 T-pose 而不是
    // 报错 —— 极难从截图反推。ANIM-CLIP-MISSING 就是为了把这件事变成日志。
    int missing = 0;
    std::string missingName;
    std::string availNames;

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

            // ---- 表演层后处理（L2 LookAt / L1 IK / L4 弹簧骨）----
            // 三个组件都是**可选**的：没挂 → applyPerformance 直接 return，
            // 与加这些功能之前逐位一致。
            seen.push_back(ent.id);
            const ecs::LookAtComponent* laComp = w.get<ecs::LookAtComponent>(ent);
            const ecs::TwoBoneIKComponent* ikComp =
                w.get<ecs::TwoBoneIKComponent>(ent);
            const ecs::SpringBoneComponent* sbComp =
                w.get<ecs::SpringBoneComponent>(ent);

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

            // ---- 1.5) 状态机（A2b）----
            //
            // ⚠ 必须在这里，**在"取 clip 指针"之前**：状态机会改
            //   `sk.clipName`，而下面第 2 步就要用 clipName 去 findClip。
            //   放到后面的话，状态机切了 clip、这一帧却还在采旧的那段，
            //   症状是"切状态慢了整整一帧" —— 而且只在 FADE 锚点上看得出来。
            //
            // 只在 `controllerPath` 非空时接管。空 = 单 clip 直放模式，
            // 与加状态机之前**逐位一致**（老场景 / 角色预览窗口都走这条）。
            if (an && !an->controllerPath.empty()) {
                const AnimController* ctl = controllerFor(an->controllerPath);
                if (ctl && ctl->valid()) {
                    ++m_lastControllers;
                    m_lastTransitions += stepAnimController(
                        *ctl, *an, sk, skel, advance ? dt : 0.0f, "ANIM");
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
            // clipName 非空但没找到 → 下面 samplePose 会静默退回 rest。
            // 数出来（带一份可用名单，省得再去翻 PLAYER-DEFAULT）。
            if (!clip && !sk.clipName.empty()) {
                if (missing == 0) {
                    missingName = sk.clipName;
                    for (const auto& c : skel.clips) {
                        if (!availNames.empty()) availNames += ",";
                        availNames += c.name;
                    }
                }
                ++missing;
            }
            // 先按"老路径"收一份，后面 156 行处如果有 animator 会用它的四项
            // 覆盖。两条路径都收**是必须的**：默认角色没有 AnimatorComponent，
            // 而 ANIM-ANIMATOR 只在 `animators > 0` 时才打 —— 只在这里收，
            // ANIM-TICK 才能报出"没有 animator 的实体到底在放哪个 clip"。
            lastAnimClip = sk.clipName;
            lastAnimTime = time;

            // ---- 3) 推进时间 ----
            //
            // 事件（B2）需要**推进前**的播放头。这里存一份，且必须处理
            // "这一帧换了 clip"：状态机改 `sk.clipName` 时通常也会把 time
            // 重置成 0，但单 clip 路径下换 clip 是用户直接改字段 —— time
            // 会留着上一段的位置。`eventClip` 就是判据（见 AnimEvent.h ③）。
            const bool clipChanged = an && an->eventClip != sk.clipName;
            const float evPrev = clipChanged ? 0.0f : (an ? an->eventPrev : time);
            // B3：本帧播放头有没有回绕过（根运动增量要据此丢弃）
            bool wrappedThisFrame = false;

            // ---- A4b 相位速率曲线（接触锁定 / 时间重映射）----
            //
            // 只对**循环** clip 生效：曲线描述的是"一圈之内怎么分配速度"，
            // 不循环的 clip 没有"下一圈"可对齐，硬套会让末帧永远到不了
            // （时间被折回 [0, dur)，is 单次动作就播不完了）。
            // 量不出曲线（不是步态 / 非人形）→ pc 为空 → 下面走原路径，
            // **逐位不变**。
            const StridePhaseCurve* pc = nullptr;
            if (advance && playing && m_strideCurveOn && clip && dur > 0.0f &&
                loop) {
                pc = strideCurveFor(skel, *clip);
                if (pc) ++strideCurved;
            }

            if (advance && playing && pc) {
                // 相位域推进：`dt × speed / dur` 是"按基准速率走了多少圈"，
                // `advance` 在内部按相位对 gain 做 4~32 个子步积分。
                // ⚠ 不能写成"每帧查一次 gain 再乘 dt"：那等于把曲线量化
                //   到帧率，帧率一变实际播放速度就变。
                const float phase = time / dur;
                const float next = pc->advance(phase, dt * speed / dur);
                time = next * dur;
                // 相位跳过端点 = 这一帧跨过了循环点（`advance` 已折回
                // [0,1)）。负 speed 时方向相反，按同一件事判。
                const bool crossed =
                    (speed >= 0.0f) ? (next < phase) : (next > phase);
                if (crossed) {
                    ++wrapped;
                    wrappedThisFrame = true;
                    wrapClip = sk.clipName;
                    wrapT = time;
                    wrapDur = dur;
                }
            } else if (advance && playing) {
                time += dt * speed;

                if (dur > 0.0f) {
                    if (loop) {
                        // 只在真的跨圈时打日志（每帧刷会淹掉别的锚点）
                        if (time >= dur || time < 0.0f) {
                            time = wrapTime(time, dur);
                            ++wrapped;
                            // B3：这一帧的根运动增量必须丢弃 —— 根骨刚从
                            // "前方 N 米"瞬回原点，那个负增量不是位移。
                            wrappedThisFrame = true;
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

            // ---- 3.5) 底座 clip 的事件（B2）----
            //
            // 位置在"推进时间之后、采样之前"。采样要在事件**之后**也无所谓
            // （事件只读时间），但必须在推进之后 —— 跨过的边界只在这一帧
            // 存在过一次。
            //
            // ⚠ 只有挂了 AnimatorComponent 的实体才有事件表（表就存在组件
            //   上）。老路径（只有 SkinnedMeshComponent）**逐位不变**。
            if (an) {
                if (!an->eventTracks.empty() && clip) {
                    const AnimEventTrack* tr =
                        findAnimEventTrack(an->eventTracks, sk.clipName);
                    if (tr) {
                        dispatchEvents(*tr, evPrev, time, dur, loop, ent.id, "");
                    }
                }
                // 无论有没有事件表都推进：否则"先播了几圈再加事件表"的第一帧
                // 会把 prev 当成 0，而 time 已经在 3.7 s —— 一次性吐 3.7 秒
                // 内所有事件。
                an->eventClip = sk.clipName;
                an->eventPrev = time;
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

            // ---- 5) 采样 → 底座姿势 ----
            //
            // P5（2026-10-22）：状态切换时走**双采样 + 混合**。见
            // `SkinnedMeshComponent::fadeFromClip` 的字段注释。
            // ⚠ 过渡推进用 `dt`（真实步长）而不是 `dt * speed`：过渡时长
            //   是**时间**语义（0.2 s），不该被角色的播放速度缩放。
            //
            // B1（2026-10-26）：这里只负责产出**底座**姿势（`base` 指向
            // `m_pose` 或过渡时的 `m_poseMix`），附加层与表演层在下面统一
            // 处理。三条分支收敛到一处输出是刻意的：以前"表演层 + 关节
            // 矩阵"在两个分支里各写一遍，加第三件事（附加层）时必然只改
            // 到其中一条 —— 症状是"只有过渡的时候层不生效"。
            Pose* base = &m_pose;
            if (sk.fading()) {
                // 过渡的**第一帧**（fadeT 还是 0）打一条锚点。
                // ⚠ 不要靠 `ANIM-TICK ... fading=` 来断言"真的在混"：
                //   ANIM-TICK 每 60 帧才打一行，而一次过渡只有 0.2 s（12 帧）
                //   —— 采样点几乎必然落在过渡之外（实测 fading_seen=0，
                //   功能是好的、判据是瞎的）。这里一帧一条，机器可判。
                if (sk.fadeT <= 0.0f) {
                    VK_LOG_INFO("ANIM-FADE begin from=%s to=%s dur=%.2fs",
                                sk.fadeFromClip.c_str(), sk.clipName.c_str(),
                                static_cast<double>(sk.fadeDur));
                }
                sk.fadeT += (advance ? dt : 0.0f) /
                            std::max(sk.fadeDur, 1.0e-4f);
                // 旧 clip 的播放头也要往前走：它仍在过自己的相位，冻住会让
                // 混合出来的姿势"腿在半空中不动"（比硬切更怪）。
                if (advance && playing) sk.fadeFromTime += dt * speed;

                const assets::AnimationClip* fromClip =
                    skel.findClip(sk.fadeFromClip);
                if (sk.fadeT >= 1.0f || !fromClip) {
                    // 过渡结束（或旧 clip 被删了）：回到单 clip 直采。
                    // 这一帧就按纯新 clip 采，不留半混合的尾巴。
                    sk.fadeT = 0.0f;
                    sk.fadeDur = 0.0f;
                    sk.fadeFromClip.clear();
                    samplePose(skel, clip, time, m_pose);
                } else {
                    const float w = fadeWeight(sk.fadeT);
                    samplePose(skel, fromClip, sk.fadeFromTime, m_poseFrom);
                    samplePose(skel, clip, time, m_pose);
                    blendPose(m_poseFrom, m_pose, w, m_poseMix);
                    base = &m_poseMix;
                    ++fading;
                }
            } else {
                samplePose(skel, clip, time, m_pose);
            }

            // ---- 5.4) NLA（R9）----
            //
            // 就地叠在底座上：推进时间轴播放头 → 逐轨逐 strip 混合。
            // ⚠ 位置是硬约束：**底座之后、`layers` 之前**（见头文件里
            //   applyNla 的说明：三层从下往上的优先级）。
            // ⚠ 空 `nlaTracks` 时 `applyNla` 立刻返回，`base` 一个分量
            //   都不碰 —— 老场景与加了 NLA 之前逐位相同。
            if (an && !an->nlaTracks.empty()) {
                applyNla(skel, *base, *an, advance ? dt : 0.0f, advance);
            }

            // ---- 5.5) 附加动画层（B1）----
            //
            // 就地叠在底座上：逐层采样 → 按骨骼遮罩混合。
            // ⚠ 位置是硬约束：**在 applyPerformance 之前**。弹簧骨/LookAt/
            //   IK 都必须吃最终姿势 —— 放在后面的话，头发会跟着底座的头部
            //   运动、脚部 IK 会把上层动画的腿拉回去，症状是"挥手时头发
            //   在飘但手不动"这种一半对一半错的样子。
            if (an && !an->layers.empty()) {
                ++layersConfigured;
                if (applyLayers(skel, *base, *an, advance ? dt : 0.0f,
                                advance, ent.id) > 0) {
                    ++layersApplied;
                }
            }

            // ---- 6) 表演层后处理 → 关节矩阵 ----
            // 表演层作用在**混合之后**：作用在混合前会被过渡抹平，
            // 表现为"切 clip 的那 0.2 秒头发停止摆动"。
            applyPerformance(skel, *base, ent.id, physDt, laComp, ikComp,
                             sbComp);

            // ---- 6.5) 根运动（B3）----
            // 位置是硬约束：**表演层之后、poseToJoints 之前**。
            // 取的是"最终姿势"里根骨的世界矩阵；挪到前面的话 IK / 弹簧骨
            // 算完之后根骨位置会跟渲染出来的差一点点，位移会缓慢打滑。
            if (an && an->rootMotion !=
                          ecs::AnimatorComponent::RootMotionMode::Off) {
                ++rootMotionCfg;
                if (applyRootMotion(skel, *base, *an,
                                    w.get<ecs::TransformComponent>(ent),
                                    wrappedThisFrame, advance)) {
                    ++rootMotions;
                }
            }

            poseToJoints(skel, *base, sk.jointMatrices);
        });

    m_lastEntities = count;
    m_lastAnimators = animators;
    m_lastLayers = layersConfigured;
    m_lastLayersApplied = layersApplied;
    m_lastRootMotion = rootMotions;
    m_lastStrideCurve = strideCurved;
    // 必须在遍历**之后**：seen 是这一帧真正处理过的实体。
    cleanupStates(seen);

    // ---- 骨骼挂点（第八刀）----
    //
    // 放在**遍历之后**：挂点要读的 `jointMatrices` 是上面刚写好的。
    // 放在遍历内部会让"挂点宿主排在挂点后面"这种顺序问题变成隐性依赖
    // （ECS 的遍历顺序不是用户能控制的）。
    m_lastAttach = updateBoneAttachments(scene);
    // 解析失败数**只在变化时**打：每帧一条会把真正的错误淹掉，而完全不报
    // 的话"挂点不动"只能靠肉眼在视口里找。
    if (m_lastAttach.unresolved != m_lastAttachUnresolvedSeen) {
        if (m_lastAttach.unresolved > 0) {
            VK_LOG_WARN("ANIM-ATTACH-UNRESOLVED n=%d first=[%s] → 该挂点保持原位",
                        m_lastAttach.unresolved,
                        m_lastAttach.firstProblem.c_str());
        } else {
            VK_LOG_INFO("ANIM-ATTACH-UNRESOLVED n=0 （全部解析成功）");
        }
        m_lastAttachUnresolvedSeen = m_lastAttach.unresolved;
    }

    // ---- 自检锚点 ----
    // 每 60 帧一条：证明"系统真的在跑"（而不是每帧都要看到它）。
    if ((m_frames % 60) == 1) {
        VK_LOG_INFO("ANIM-TICK f=%llu entities=%d animators=%d dt=%.5f "
                    "fading=%d lastclip=[%s] lasttime=%.3f",
                    static_cast<unsigned long long>(m_frames), count, animators,
                    static_cast<double>(dt), fading,
                    lastAnimClip.c_str(),
                    static_cast<double>(lastAnimTime));
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
        // 相位速率曲线（A4b）：开关开着就报，**哪怕 n=0**。
        // `on=1 n=0` 是"开了但一段都量不出曲线"（不是步态 clip / 非人形），
        // 那与"没接线"是两件事，必须能分开。
        if (m_strideCurveOn) {
            VK_LOG_INFO("ANIM-STRIDE-CURVE-TICK on=1 curved=%d curves=%d",
                        strideCurved, static_cast<int>(m_strideCurves.size()));
        }
        if (missing > 0) {
            VK_LOG_WARN("ANIM-CLIP-MISSING n=%d clipName=[%s] available=[%s] "
                        "→ 已静默退回 rest（findClip 是逐字比较）",
                        missing, missingName.c_str(), availNames.c_str());
        }
        // 表演层：只在真有人挂了组件时打，否则每 60 帧一行噪音。
        // ⚠ spring 的计数是"本帧真的跑了物理"，第一帧只初始化 → 会是 0，
        //   这是刻意的（挂上组件的瞬间姿态不变）。
        if (m_lastSpring > 0 || m_lastLookAt > 0) {
            VK_LOG_INFO("ANIM-PERF spring=%d lookat=%d", m_lastSpring,
                        m_lastLookAt);
        }
        // IK：只在真有人解算时打。`target` 是"要够多远"（root→目标），
        // `actual` 是"够完之后末端离目标还有多远"（**实测**，二次 FK），
        // `miss` 是误差 > 2cm 的实体数。判"脚有没有落地"只能看 actual。
        if (m_lastIK > 0) {
            VK_LOG_INFO("ANIM-IK n=%d miss=%d bone=%s target=%.4f "
                        "actual=%.4f bend=%.1fdeg",
                        m_lastIK, m_lastIKMiss, m_lastIKEndBone.c_str(),
                        static_cast<double>(m_lastIKTarget),
                        static_cast<double>(m_lastIKActual),
                        static_cast<double>(m_lastIKBend));
        }
        if (m_lastAttach.placed > 0) {
            VK_LOG_INFO("ANIM-ATTACH placed=%d", m_lastAttach.placed);
        }
        // 状态机：只在真有人配了控制器时打。`trans` 是**本帧**采纳的转移数，
        // 60 帧一条 → 稳态下几乎恒为 0（那是正常的，不是没生效）：
        // 判"到底在不在跑"要看 `active`，"切换有没有发生"看 ANIM-STATE。
        if (m_lastControllers > 0) {
            VK_LOG_INFO("ANIM-CTL active=%d trans=%d", m_lastControllers,
                        m_lastTransitions);
        }
        // 附加层（B1）：`configured` 与 `applied` 之差就是"配了但没生效"。
        // 稳态下几乎恒为 0/0（没人配层），所以只在真有人配时才打。
        if (m_lastLayers > 0) {
            VK_LOG_INFO("ANIM-LAYER configured=%d applied=%d", m_lastLayers,
                        m_lastLayersApplied);
        }
        // NLA（R9）：`strips` = 配了几条、`applied` = 本帧几条真的生效。
        // ⚠ 两者**同时**报。只有 `applied` 的话，"一条都没摆"和"摆了但
        //   全不生效"在日志上长得一样，而这两种情况的下一步动作完全不同
        //   （去摆 strip / 去查 clip 名与遮罩）。
        // `problem` 只报第一个：20 条 strip 全配错时，逐条打印会把日志读成
        // "20 个不同的问题"，而它们往往是同一个（clip 名拼错 / 骨名片段错）。
        if (m_lastNla > 0) {
            VK_LOG_INFO("ANIM-NLA configured=%d strips=%d applied=%d "
                        "skipped=%d problem=%s",
                        m_lastNla, m_lastNlaStrips, m_lastNlaApplied,
                        m_lastNlaSkipped,
                        m_lastNlaProblem.empty() ? "(none)"
                                                 : m_lastNlaProblem.c_str());
        }
        // 根运动（B3）：`cfg` = 配了的人，`n` = 本帧真的走动了的人。
        // ⚠ **`cfg > 0` 就打**，哪怕 `n == 0`。这是刻意的：最常见的两种
        //   失败（骨选错 / 这段动画是原地做的）都表现为"n 一直是 0"，
        //   而"没有日志"和"日志说 0"对排查是天壤之别 —— 前者让人以为
        //   功能没接线（引擎里根本没有这条路径），后者指向资产或配置。
        if (rootMotionCfg > 0) {
            // ⚠ yaw 用**角度**并且两位小数：走路时骨盆本来就左右摆
            //   （实测 0.5° 上下），用弧度三位小数它永远是 0.000 —— 于是
            //   "n=1 但 dist=0.0000 yaw=0.000" 看起来像自相矛盾。
            VK_LOG_INFO("ANIM-ROOTMOTION cfg=%d n=%d bone=%s dist=%.4f "
                        "yaw=%.2fdeg",
                        rootMotionCfg, m_lastRootMotion,
                        m_lastRootBone.empty() ? "(none)" : m_lastRootBone.c_str(),
                        static_cast<double>(m_lastRootDist),
                        static_cast<double>(glm::degrees(m_lastRootYaw)));
        }
        if (m_rootBoneMissing) {
            VK_LOG_WARN("ANIM-ROOTMOTION-BONE-MISS rootMotionBone 填了名字但"
                        "骨架里找不到 → 该实体的根运动整条关闭（保持原样，"
                        "不会退化成\"用根骨\"）");
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
