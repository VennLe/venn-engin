#pragma once
// ============================================================
// animation/LeaderPose —— 模块化角色的「主姿势」（2026-11-05）
//
// 抄的是 UE 的 `USkinnedMeshComponent::LeaderPoseComponent`
// （`Engine/Source/Runtime/Engine/Classes/Components/SkinnedMeshComponent.h`）。
//
// ------------------------------------------------------------
// 它解决的是什么问题
// ------------------------------------------------------------
// 一个角色很少是**一个**网格。真实资产（FAB 上的角色尤其如此）通常是：
//
//     Body.fbx     身体 + 全套骨骼 + 动画
//     Hair.fbx     头发（同一套骨，多几根发骨）
//     Outfit.fbx   衣服（同一套骨，多几根裙摆骨）
//     Shoes.fbx    鞋
//
// 每份都是独立的 `assets::Model`、独立的 `assets::Skeleton`、独立的
// `SkinnedMeshComponent`。**如果每个都自己采样动画**，问题不是"慢"，
// 而是"不一定一样"：
//   · 每份资产的 clip 可能同名但时长/起始帧不同（导出批次不同）；
//   · 每份的 `time` 各自推进，一次暂停/跳帧就让头发和身体差半帧；
//   · 状态机切 clip 时只改得到身体那一份 —— 头发原地不动。
// 症状是"跑起来之后头发比身体慢半拍"，而**每份单独看都是对的**。
//
// UE 的做法：只有一个组件真的跑动画（leader），其余的（follower）每帧
// 去 leader 那里**取姿势**，自己不做动画。UE 取的是 component-space
// 变换（`FSkelMeshComponentLODInfo` + `LeaderBoneMap` 逐骨映射）。
//
// ------------------------------------------------------------
// 我们为什么搬的是「局部姿态」而不是 component-space
// ------------------------------------------------------------
// 两者在**同名同层级**的骨架上代数等价：
//
//     world(j) = world(parent(j)) · restLocal(j)        ← FK 的定义
//
// 逐骨把 leader 的 local 抄过来，累乘出来的 world 自然与 leader 逐位相同
// （浮点上：同样的乘法顺序，同样的输入 → 同样的结果）。
//
// 而"follower 有、leader 没有"的那些骨（裙摆 / 发梢），因为我们照抄的是
// **局部**，它自然就是 `world(祖先) · 自己的 restLocal` —— 这正是 UE 在
// `GetMissingLeaderBoneRelativeTransform()` 里绕一圈求出来的那个
// `CommonAncestorBoneIndex + RelativeTransform`（那份代码沿父链上溯，找到
// 第一个在 leader 里存在的祖先，再把中间几层的 rest 乘起来）。同一个式子，
// 一个是"由 FK 顺带算出来"，一个是"专门拼出来"。
//
// 代价（必须写清楚）：这条等价只在**两侧是同一套 rig**（同名、同父、
//   同静止姿势）时成立。不同 rig 之间共享动画是**重定向（retargeting）**，
//   另一件事 —— UE 也是分开的（IK Rig / IK Retargeter）。
//
// ------------------------------------------------------------
// ⚠ 三条约定
// ------------------------------------------------------------
// ① **按骨名映射，不按下标。** 两份资产各自由自己的导出器排序，下标顺序
//    没有任何保证；哪怕同一次导出，只要有一份多了一根骨，后面的全部错位。
//    名字是唯一的稳定键。
//
// ② **只做精确匹配**（不分大小写），**不做包含匹配**。包含匹配会把
//    `hand_l` 匹到 `hand_l_end` —— 主姿势一旦错一根骨，画面上是"整只手
//    反着长"，而不是"某个挂点偏了"。宁可留 -1（那根骨退回静止姿势）。
//
// ③ follower **不跑自己的 IK / 弹簧骨**（v1）。它的姿势源不是自己，
//    "先按 leader 摆好、再按自己的弹簧骨抖一下"这件事要先有"谁是权威"
//    的答案才谈得上。要头发动，就在 leader 上挂 `SpringBoneComponent`
//    （那是同一具骨架，覆盖得到）。
// ============================================================

#include "animation/Pose.h"
#include "assets/Skeleton.h"

#include <string>
#include <vector>

namespace animation {

struct LeaderPoseStats {
    int followers = 0;        // 配了 leaderName 的实体数
    int applied = 0;          // 真的按 leader 摆好了的
    int missingLeader = 0;    // 找不到 leader / leader 没有骨架 → 自采样回退
    int missingBones = 0;     // 这些 follower 里 leader 对不上的骨总数
    std::string firstProblem; // 第一条问题的原因（`no-leader:xxx` / `no-bones:x`）
};

// follower 的 joint → leader 的 joint（-1 = leader 里没有同名骨）。
// `out` 会被 resize 到 follower 的骨数。
// `unmatched`（可空）回填对不上的骨数。
//
// ⚠ 映射与"骨的名字"绑定，与骨架的**生命周期**绑定 —— 换资产 / 重烤
//   骨架之后必须重建。调用方（AnimatorSystem）用骨架指针做缓存键。
void buildLeaderBoneMap(const assets::Skeleton& follower,
                        const assets::Skeleton& leader,
                        std::vector<int>& out, int* unmatched = nullptr);

// 把 leader 的局部姿态按映射搬到 follower 上。`out` 先被 setRest，
// 所以 leader 里没有的骨保持 follower 自己的静止姿势（见文件头）。
void remapPose(const assets::Skeleton& follower, const Pose& leaderPose,
               const std::vector<int>& map, Pose& out);

} // namespace animation
