#pragma once
// ============================================================
// animation/AnimatorSystem —— 推进动画时间并刷新关节矩阵
//
// 为什么要有它（2026-10-18，角色链 A0-3）：
//   在这次改动之前，`SkinnedMeshComponent::time` 在**全仓库只有一个写入点**
//   —— `editor/CharacterWindow.cpp` 的时间轴滑条。运行时没有任何系统去加
//   这个时间，于是 Game.exe 里的角色永远定格在第 0 帧。
//   这也解释了为什么"动画在动"这件事过去无法自动化：没有时间轴就没有
//   可断言的量。
//
// 职责（与 `Renderer::updateSkinning` 的分工，见
// `docs/character_pipeline_roadmap.md` §3.3）：
//   * 本类：推进 time、采样、产出 `sk.jointMatrices`
//   * Renderer：分配 jointBase、把 jointMatrices 拷进 SSBO、绑描述符
// 这样拆开之后，动画逻辑可以**不开窗口**跑（`MYVK_ANIM_TEST=1`），
// 数值断言才成立。
//
// ⚠ 每帧都要调（不只是 Play 时）：编辑器在非 Play 态也要显示"当前 time
//   处"的姿势（用户拖时间轴 / 改 clip 要立刻看到）。dt 传 0 → 不推进时间，
//   只按当前 time 重算姿势 —— 语义与拆分前 `updateSkinning` 每帧采样一致。
//
// ============================================================
// A1（2026-10-19）：`AnimatorComponent` 上线 —— 谁是 time 的权威
// ============================================================
// 两条路径，按"实体上有没有 AnimatorComponent"分：
//
//   · 没有（老场景 / 角色预览窗口的私有预览场景）
//       → 读 `SkinnedMeshComponent` 的 time/speed/playing/loop。
//         行为与 A0 **逐位相同**（这条是硬约束：老场景渲染基线不许飘）。
//   · 有
//       → 读 `AnimatorComponent` 的那四项（**权威**），然后把它们
//         **镜像**回 `SkinnedMeshComponent` 的同名字段。镜像不是可有可无的
//         同步：`Renderer::updateSkinning` 的兜底采样、`ANIM-SKIN` 日志、
//         控制台 `anim.time`、以及所有读 `sk.time` 的截图断言都吃那一份。
//         镜像的写入点**只有本文件一处** —— 多一个写入点，症状就是
//         "拖了时间轴又弹回去"。
//
// 为什么"多份 Pose"这一条 A1 不落成代码：A2 的 crossfade 需要 `m_poseA` /
//   `m_poseB` 两份采样缓冲，但 A1 只用得到一份。**刻意不加一个没人用的
//   成员** —— 空字段会被后来的人读成"混合已经支持了"（项目里"界面上能调、
//   实际没接线"比没有更糟的同一条判断）。A1 准备好的是**结构**：采样、
//   时间推进、关节矩阵输出全部收在这一个系统里，A2 只需要在这里加第二次
//   `samplePose` 和一条 `blendPose`。
// ============================================================

#include "animation/AnimController.h"
#include "animation/AnimEvent.h"
#include "animation/BoneAttachment.h"
#include "animation/BoneMask.h"
#include "animation/LookAt.h"
#include "animation/Pose.h"
#include "animation/SpringBone.h"
#include "animation/TwoBoneIK.h"
#include "scene/Scene.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace animation {

class AnimatorSystem {
public:
    // 推进 + 采样。dt 的单位是秒；dt <= 0 → 只刷新姿势，不推进时间。
    // 返回本次处理的蒙皮实体数。
    int tick(scene::Scene& scene, float dt);

    // 已跑的帧数 / 上一帧的实体数 / 其中走 Animator 路径的数量
    // （调试与自检用；后两个是 A1 新加的）
    uint64_t frames() const { return m_frames; }
    int lastEntityCount() const { return m_lastEntities; }
    int lastAnimatorCount() const { return m_lastAnimators; }
    // 本帧真的跑了弹簧物理 / 真的转了头的实体数（自检与 ANIM-SPRING 锚点用）
    int lastSpringCount() const { return m_lastSpring; }
    int lastLookAtCount() const { return m_lastLookAt; }
    // 本帧真的解算了 IK 的实体数 / 其中"没落在目标上"的（够不着）
    int lastIKCount() const { return m_lastIK; }
    int lastIKMissCount() const { return m_lastIKMiss; }
    // 本帧摆好的骨骼挂点数 / 解析失败的（宿主或骨名对不上）
    int lastAttachCount() const { return m_lastAttach.placed; }
    int lastAttachUnresolved() const { return m_lastAttach.unresolved; }
    // 本帧真的由状态机驱动的实体数（A2b）
    int lastControllerCount() const { return m_lastControllers; }
    // 本帧状态机采纳的转移总数
    int lastTransitionCount() const { return m_lastTransitions; }
    // 本帧配了附加层的实体数 / 其中真的混合了至少一层的（B1）
    // 两者之差 = "配了层但一段都没生效"（clip 空 / weight 0 / 遮罩一根都
    // 没命中），这是配错时最常见的形态，必须能从日志一眼看出来。
    int lastLayerCount() const { return m_lastLayers; }
    int lastLayerApplied() const { return m_lastLayersApplied; }
    // 本帧发出的事件数（B2）。与 `eventsThisFrame()` 直接相关：
    // 前者给日志/断言，后者给消费方（脚本 / 音频 / 特效）。
    int lastEventCount() const { return m_lastEvents; }
    // 本帧真的提取了根运动位移的实体数（B3）。注意它统计的是
    // **"提到了非零增量"**，不是"配了 rootMotion ≠ Off" —— 两者之差 =
    // 配了但一步没走（骨选错 / 动画是原地做的 / 第一帧）。
    int lastRootMotionCount() const { return m_lastRootMotion; }
    // ---- NLA（R9，2026-10-29）----
    // 本帧配了 NLA 轨的实体数 / 时间轴上**摆了几条** strip / 其中真的
    // 混进姿势的 / 想混但混不进去的。
    // `strips` 与 `applied` 之差**不是**错误：区间外的 strip 天然不生效。
    // 真正要看的是 `skipped` —— 它只统计"落在区间内、却因为配置问题没生效"
    // 的那些（clip 名找不到 / 遮罩一根骨没命中 / span 为 0）。
    int lastNlaCount() const { return m_lastNla; }
    int lastNlaStripCount() const { return m_lastNlaStrips; }
    int lastNlaApplied() const { return m_lastNlaApplied; }
    int lastNlaSkipped() const { return m_lastNlaSkipped; }
    const std::string& lastNlaProblem() const { return m_lastNlaProblem; }
    // 本帧最近一次提取用的骨名 + 位移量（ANIM-ROOTMOTION 锚点用）。
    const std::string& lastRootBone() const { return m_lastRootBone; }

    // ---- 本帧发出的事件（B2，2026-10-26）----
    //
    // 每次 `tick` 开头清空，逐条 push。**消费方必须在同一帧里读完**
    // （下一帧就被清掉了）—— 这是刻意的：事件是"这一帧发生的事"，
    // 攒起来让下游"稍后处理"会让音效晚一帧、伤害判定在死亡之后才结算。
    //
    // ⚠ 引擎内部**不消费**这个数组，只打 `ANIM-EVENT` 锚点。谁是消费方
    //   由应用层决定（`runtime/RuntimeApp` 或脚本）—— 引擎里塞一个默认
    //   实现（比如"自动播音效"）会让"没接线的系统"看起来像接好了。
    const std::vector<FiredAnimEvent>& eventsThisFrame() const {
        return m_frameEvents;
    }

private:
    // 采样缓冲（复用，避免每帧分配）。
    //   m_pose      = 目标 clip 的姿势（每帧都采）
    //   m_poseFrom  = 过渡时旧 clip 的姿势（只在 fading() 时采）
    //   m_poseMix   = blendPose 的输出（只在 fading() 时用）
    // 三份都要**成员**而不是局部变量：blendPose 的输出不能与输入同址，
    // 而每帧构造 Pose 会反复分配 3 × jointCount 个向量。
    Pose m_pose;
    Pose m_poseFrom;
    Pose m_poseMix;
    // 附加层采样缓冲（B1）。与上面三份同理：**成员**而不是局部变量，
    // 否则每帧每层都要分配 3 × jointCount 个向量（4 层 × 63 骨 × 60 fps
    // = 每秒 4.5 万次分配）。
    Pose m_poseLayer;

    // ---- 表演层后处理（L2 LookAt / L1 IK / L4 弹簧骨）----
    //
    // 三者最终都落在 `Pose::rotation` 上（**局部**旋转），所以必须在
    // `poseToJoints` 之前跑 —— 写进世界矩阵就没有"再 FK 一次"的机会了。
    // 区别只在**改几根骨**：
    //   · LookAt   一根（头 / 颈）
    //   · IK       两根（root + mid；末端那根只被读，不被写）
    //   · 弹簧骨   一条前缀链上的 N 根（每根一个四元数）
    // 三者**都不写平移**：骨长不变是 FK 的前提，也是"脚落在目标上"能用
    // 余弦定理解的前提。
    //
    // ⚠ 顺序是 **LookAt → IK → 弹簧骨**，三条理由：
    //   ① LookAt 与 IK 都是"目标驱动"的旋转修正，弹簧骨是**动力学**；
    //      动力学必须读"最终姿势"，否则头发会慢一帧（反向时快速转头能
    //      看到头发慢半拍）。
    //   ② IK 排在 LookAt 之后：接触约束（脚落地、手握物）应该是最接近
    //      最终姿势的那一层修正 —— 它要吃掉前面所有层留下的误差。
    //   ③ 前两层默认不重叠（腿/臂 vs 头），但**不假设**它们不重叠：
    //      `rootBone` 完全可以配成 `neck` —— 那时顺序就有语义（LookAt 先
    //      摆头，IK 再按目标校正）。固定顺序的价值不是"反正互不影响"，
    //      而是"同一帧两次求值逐位相同"这条可断言的性质（
    //      `ik_deterministic` 断的就是它）。
    void applyPerformance(const assets::Skeleton& skel, Pose& pose,
                          uint32_t entityId, float dt,
                          const ecs::LookAtComponent* la,
                          const ecs::TwoBoneIKComponent* ik,
                          const ecs::SpringBoneComponent* sb);

    // ---- 附加动画层（B1，2026-10-26）----
    //
    // 就地改 `pose`：逐层采样 → 按骨骼遮罩混合进 pose。
    //
    // ⚠ 必须在 `applyPerformance` **之前**、`poseToJoints` 之前：
    //   弹簧骨/LookAt/IK 都要读"最终姿势"，否则头发会跟着底座（下半身）
    //   的头走、脚部 IK 会把上层动画的腿又拉回去。
    // ⚠ 这里**只写局部 TRS**，不碰平移以外的世界量 —— 与表演层同一个理由。
    //
    // 返回本帧真的混合进去的层数（0 = 一层都没生效）。
    int applyLayers(const assets::Skeleton& skel, Pose& pose,
                    ecs::AnimatorComponent& an, float dt, bool advance,
                    uint32_t entityId);

    // ---- NLA（R9，2026-10-29）----
    //
    // 就地改 `pose`：推进 `an.nlaTime`，然后把时间轴上此刻有效的 strip
    // 逐条盖上去。返回本帧真的混进去的 strip 数。
    //
    // ⚠ 位置是硬约束：**在底座之后、`applyLayers` 之前**。
    //   理由是"谁盖住谁"：底座是玩法权威（idle/run/attack 由代码决定），
    //   NLA 是作者排好的演出叠加，层（B1）是表演性质的覆盖（挥手/瞄准）。
    //   三者从下往上的优先级正好是这个顺序。反过来的话，一条 weight=1 的
    //   strip 会让角色**再也攻击不了**（状态机切了 clip，姿势被 NLA 盖回）。
    // ⚠ 空 `nlaTracks` 时本函数立刻返回（**逐位不变**，老场景一帧不变）。
    int applyNla(const assets::Skeleton& skel, Pose& pose,
                 ecs::AnimatorComponent& an, float dt, bool advance);

    // ---- 动画事件分发（B2，2026-10-26）----
    //
    // 把 `track` 在 `[prevTime, newTime)` 里跨过的事件全部 push 进
    // `m_frameEvents`。纯粹的"落库"动作，判定本身在
    // `animation::collectAnimEvents`（那是能脱离场景跑用例的纯函数）。
    //
    // `layer` 非空 → 事件带上层名（附加层的事件要能和底座区分开，
    // 否则"上层挥拳的 Hit 事件"会被当成底座跑步的）。
    void dispatchEvents(const AnimEventTrack& track, float prevTime,
                        float newTime, float duration, bool looped,
                        uint32_t entityId, const std::string& layer);
    // 一次分发最多 push 多少条（防一个巨大 dt 把日志/队列打爆）。超出的
    // 只计数不发 —— 判据是"稳态下一次跨圈的事件数远小于它"。
    static constexpr int kMaxEventsPerDispatch = 32;
    std::vector<int> m_eventScratch;   // collectAnimEvents 的输出缓冲

    // ---- 根运动（B3，2026-10-26）----
    //
    // 就地改 `pose`（把增量扣掉 = in-place），并按 mode 决定要不要写
    // `TransformComponent`。返回"本帧真的提到了位移"。
    //
    // ⚠ 位置是硬约束：**在 applyPerformance 之后、poseToJoints 之前**。
    //   取的是"最终姿势"里根骨的世界矩阵 —— 否则 IK / 弹簧骨算完之后
    //   根骨的位置跟渲染出来的差一点点，位移会缓慢打滑。
    // ⚠ `wrapped` = 这一帧播放头刚回绕过 → **增量必须丢弃**（见 RootMotion.h）。
    // ⚠ `advance == false`（编辑器非 Play）时也不提取：拖时间轴 = 任意跳帧，
    //   每一跳都会被当成一次位移，角色会跟着滑条在场景里乱窜。
    bool applyRootMotion(const assets::Skeleton& skel, Pose& pose,
                         ecs::AnimatorComponent& an,
                         ecs::TransformComponent* tf, bool wrapped,
                         bool advance);

    // 遮罩解算缓存。key = 骨架地址 + spec 摘要 —— 同一具骨架被 20 个角色
    // 共用时只算一次（每次算要沿父链走 n·depth 步）。
    // ⚠ 用 `std::map` 而不是 `unordered_map`：返回的是**引用**，
    //   unordered_map rehash 会让引用失效（与 rigFor 同一个坑）。
    const BoneMask& maskFor(const assets::Skeleton& skel,
                            const BoneMaskSpec& spec);
    std::map<std::string, BoneMask> m_maskCache;

    // NLA 求值的临时缓冲（采样姿势 / rest 基准 / 遮罩缓存）。
    // 由系统持有一份就够 —— 求值是逐实体串行的，不存在重入。
    NlaScratch m_nlaScratch;

    // 弹簧拓扑缓存。key = (骨架, 前缀表) —— 同一角色实例化多次时共用一份，
    // 不然每个实例都要做一次静止姿势 FK。
    struct RigKey {
        const assets::Skeleton* skel = nullptr;
        std::string prefixes;
        bool operator<(const RigKey& o) const {
            if (skel != o.skel) return skel < o.skel;
            return prefixes < o.prefixes;
        }
    };
    const SpringRig& rigFor(const assets::Skeleton& skel,
                            const std::vector<std::string>& prefixes);
    std::map<RigKey, SpringRig> m_rigCache;

    // 每角色运行时状态。按 `Entity::id` 索引（**只在内存里**）：
    // 质点位置与"上一帧施加的偏转"都是"这一帧正在发生的事"，不进存档。
    // 实体删除后由 cleanupStates 清掉 —— 不清理的话，长时间编辑会一直涨。
    std::unordered_map<uint32_t, SpringBoneState> m_spring;
    std::unordered_map<uint32_t, LookAtState> m_lookAt;
    void cleanupStates(const std::vector<uint32_t>& seen);

    // ---- 控制器资产的加载缓存（A2b）----
    // key = `AnimatorComponent::controllerPath` 原样字符串。
    // **按路径缓存而不是按实体**：同一份 humanoid.anim.json 被 20 个角色
    // 引用时只该解析一次 JSON。用 `shared_ptr` 是因为失败也要缓存
    // （`error` 非空的对象）—— 不缓存失败的话，配错路径的实体每帧都会
    // 重新 fopen 一次，20 个角色 = 每秒 1200 次失败的磁盘访问。
    const AnimController* controllerFor(const std::string& path);
    std::map<std::string, std::shared_ptr<AnimController>> m_ctlCache;

    uint64_t m_frames = 0;
    // 真实帧间隔（秒）。**只给物理用**，见 tick 里的说明。
    std::chrono::steady_clock::time_point m_lastWall{};
    float m_wallDt = 0.0f;
    int m_lastEntities = 0;
    int m_lastAnimators = 0;     // 其中挂了 AnimatorComponent 的
    int m_lastSpring = 0;        // 本帧真的跑了物理的实体数（ANIM-SPRING 用）
    int m_lastLookAt = 0;        // 本帧真的转了头的实体数
    int m_lastIK = 0;            // 本帧真的解算了 IK 的实体数
    int m_lastIKMiss = 0;        // 其中够不着的（目标超出可达范围）
    // ANIM-IK 锚点用的"最后一个实体的账"。只报最后一个是刻意的：
    // 同时有 20 个角色在跑时，逐条打印会把日志读成"20 个不同的问题"，
    // 而实际只关心"有没有生效 + 差多少"（自检走数值断言，不看日志）。
    float m_lastIKTarget = 0.0f;
    float m_lastIKActual = 0.0f;
    float m_lastIKBend = 0.0f;
    std::string m_lastIKEndBone;
    AttachmentStats m_lastAttach;
    // 上一帧的挂点解析失败数 —— 只在**变化**时打日志。
    // 放成员而不是函数内 static：两个场景会共用一个计数，自检里立刻串味。
    int m_lastAttachUnresolvedSeen = 0;
    int m_lastControllers = 0;   // 本帧真的被状态机驱动的实体数
    int m_lastTransitions = 0;   // 本帧采纳的转移总数
    int m_lastLayers = 0;        // 本帧配了附加层的实体数
    int m_lastLayersApplied = 0; // 其中真的混进去至少一层的
    // NLA（R9）：配了轨的实体数 / 本帧真的混进去的 strip 总数 /
    // 想混但混不进去的（clip 名错、遮罩空、包络端点）。
    // `strips` 与 `applied` **同时报**是刻意的：它们是"配了几条"和
    // "几条真的生效"，两者相等才是正常的；`skipped` 单独一列而不并进
    // `applied` 的反面 —— "区间外"的 strip 也不生效，但那是正常的。
    int m_lastNla = 0;
    int m_lastNlaStrips = 0;
    int m_lastNlaApplied = 0;
    int m_lastNlaSkipped = 0;
    std::string m_lastNlaProblem;   // 本帧第一个问题（clip:xxx / mask:empty）
    int m_lastEvents = 0;        // 本帧发出的事件数（B2）
    std::vector<FiredAnimEvent> m_frameEvents;   // 本帧的事件（tick 开头清空）
    int m_lastRootMotion = 0;    // 本帧真的提到了非零根位移的实体数（B3）
    std::string m_lastRootBone;  // 最近一次用到的根骨名（"配错了没"只能靠它）
    float m_lastRootDist = 0.0f; // 最近一次的位移模长（米）
    float m_lastRootYaw = 0.0f;  // 最近一次的朝向增量（弧度）
    bool m_rootBoneMissing = false;  // 本帧有实体填了骨名但找不到（B3）
};

} // namespace animation
