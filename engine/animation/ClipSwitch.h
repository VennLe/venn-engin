#pragma once
// ============================================================
// animation/ClipSwitch —— "换当前 clip"的唯一写入点
//
// 为什么抽出来（2026-10-25，A2 状态机）：
//   在这之前，这段逻辑是 `Locomotion.cpp` 匿名 namespace 里的一个 static
//   函数 —— 只有一个消费者，写在谁家都行。状态机是第二个消费者，而这段
//   逻辑**错一处就表现为"过渡看起来怪"**，不会报错、不会崩、日志也正常：
//     * `fadeFromTime` 取成 `sk.time` 而不是权威的 `an->time`
//       → 从旧片段的第 0 帧开始淡入，腿会"缩回去再迈出来"
//     * 忘了同时归零 `sk.time` 与 `an->time` → 过渡结束后播放头跳一下
//     * 硬切时没清 `fadeFromClip` → 上一段残留的过渡又淡进来一次
//   两个消费者各抄一份的话，调手感时必然只改到一份，症状是"走路顺、
//   按 K 起跳怪"这种半好半坏，极难定位。
//
// 契约（三条必须同时成立）：
//   1. `sk.clipName` = 新 clip
//   2. `restart` 时 `sk.time` 与 `an->time` **一起**归零（权威字段是两份，
//      见 Components.h 的"权威关系"）
//   3. `blendSec > 0` 且确实在换 clip → 写 `fadeFrom*` 四件套；
//      否则（硬切 / 首次选 clip）**清空**四件套
//
// ⚠ 这里**不推进 fadeT** —— 过渡的推进在 `AnimatorSystem` 的采样阶段
//   （它才知道这一帧的 dt 与是否 advance）。本文件只负责"开始一次过渡"。
// ============================================================

#include "ecs/Components.h"

#include <string>

namespace animation {

// 换 clip。clip 与当前相同 → 只在 restart 时归零播放头（不触发过渡）。
//
// `blendSec > 0` 且当前已经有 clip → 记下旧 clip 与旧播放头，交给
// AnimatorSystem 做双采样混合。当前 clip 为空（第一次选）→ 硬切，
// 因为"从静止姿势淡入"没有意义（静止姿势不是一段动画）。
void switchClip(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
                const std::string& clip, bool restart, float blendSec);

// playing / loop 两个也要两边都写 —— 理由同上（各有两份权威字段）。
void setClipPlaying(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
                    bool playing);
void setClipLoop(ecs::SkinnedMeshComponent& sk, ecs::AnimatorComponent* an,
                 bool loop);

} // namespace animation
