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
#include "animation/BoneAttachment.h"
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
};

} // namespace animation
