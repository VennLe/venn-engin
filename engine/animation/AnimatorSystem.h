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

#include "animation/Pose.h"
#include "scene/Scene.h"

#include <cstdint>
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
    uint64_t m_frames = 0;
    int m_lastEntities = 0;
    int m_lastAnimators = 0;     // 其中挂了 AnimatorComponent 的
};

} // namespace animation
