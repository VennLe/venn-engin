#pragma once
// ============================================================
// animation/NlaTimeline —— NLA（非线性动画）：把片段排到时间轴上（R9）
//
// ------------------------------------------------------------
// 它和 `AnimLayer` 为什么**不是**同一个东西
// ------------------------------------------------------------
// 这是本文件最重要的一段。两者都长得像"多段动画叠在一起"，但问的问题
// 完全不同：
//
//   `AnimLayer`（B1）= **按骨覆盖的当前姿态**。它没有时间轴位置，播放头
//     自己跑自己的（换 clip 就从 0 开始 + 淡入）。问的是"这台角色**此刻**
//     身上盖着哪几段动作"，典型用途是"上半身挥手 + 下半身走路"。
//
//   `NlaTimeline`（R9）= **时间轴上排好的片段**。它问的是"第 3.2 秒该演
//     什么"，片段有起止时刻、可以错开、可以叠、可以缩放，整条时间轴有
//     一个播放头。典型用途是过场 / 演出 / 需要精确对时的连招。
//
// 硬合并的下场（这一条是任务书里点名的）：把 strip 塞进 `layers` 之后，
// "我把片段拖上去，角色的上半身被整段替换了" —— 因为层的语义是**覆盖**
// （遮罩内权重 1 = 与只播那一层逐位相同），而 strip 的语义是**在某个时刻
// 参与混合**。同一个 `weight` 字段在两边意思不一样，这不是命名问题，是
// 两个不同的东西被塞进了一个字段。
//
// 所以：**两者共存，各走各的**。求值顺序（见 `AnimatorSystem::tick`）：
//
//     底座（状态机 / 单 clip）
//       → NLA 轨（本文件，下标 0 在最下面）
//         → `AnimLayer[]`（按骨覆盖，语义一个字没改）
//           → 表演层（LookAt / IK / 弹簧骨）
//
// ⚠ NLA 在底座**之上**，而 Blender 恰好相反（active action 在 NLA 栈
//   之上）。理由是这里的状态机是**玩法权威**（idle/run/attack 由代码决定），
//   让 NLA 盖住它意味着"一拖上片段角色就不攻击了"。要完全接管整具身体，
//   放一条 `weight=1 + mask=All + blend=Replace` 的 strip 就等价了 —— 主动
//   选择比默认行为更安全。
//
// ------------------------------------------------------------
// 四条"省了就错"的约定
// ------------------------------------------------------------
// ⚠⚠ ① **没有 `nlaTracks` = 逐位不变**。`applyNlaTimeline` 在轨列表为空
//     （或全静音、或 solo 过滤后一条不剩）时立刻返回，`pose` 一个分量都
//     不碰。这是"加了这个功能，老场景一帧都不许变"的依据，由
//     `nla_empty_bitwise` / `nla_no_active_strip_bitwise` 钉住。
//
// ⚠⚠ ② **strip 的 `span` 决定"这一段占多久"，`speed` 决定"span 里塞几遍
//     源动作"**（Blender 的 Repeat/Scale 效果），**不是**"播放快多少"。
//     让 strip 单独跑得快，就得回答"那它提前跑完了画什么"（停住？循环？
//     拉伸到 end？），三种答案在时间轴上看起来一样，画面上完全不同。
//     所以要"更快的动作"就缩短 span。
//
// ⚠ ③ **strip 只在 `[start, end]` 内参与求值**。区间外它**不存在**，不是
//     "停在首帧"也不是"停在末帧" —— 后者会让两条首尾相接的 strip 在
//     接缝处同时贡献姿势（第一条停在末帧 + 第二条从头开始），接缝位置
//     出现一次没人要的混合。
//
// ⚠ ④ **`Add` 是"以 rest 为基准的增量"，不是"两个姿势相加"**。
//     直接相加的话，静止姿势会被数两遍（rest + rest），静态角色一开 NLA
//     就变形成两倍大小。增量 = `rest⁻¹ * strip`（旋转）、`strip - rest`
//     （平移 / 缩放），乘权重后**叠在底座上**。这也正是"呼吸叠加在走路
//     上面"需要的语义。
// ============================================================

#include "animation/BoneMask.h"
#include "animation/Pose.h"
#include "assets/Skeleton.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace animation {

// 混合模式。只有两种 —— 不是"暂时只做两种"，而是这两种的语义边界很清楚：
//   Replace：遮罩内的骨**换成** strip 的姿态（权重 1 = 逐位等于只播 strip）
//   Add    ：遮罩内的骨**加上** strip 相对 rest 的增量（权重 1 = 全幅度）
// Blender 还有 Multiply / Subtract 之类，那些是"逐通道算术"，落到四元数上
// 需要先选一条插值路径（见 CurveBake 里 slerp / nlerp 那段），在没人真的
// 需要之前做出来只会让"为什么我的旋转不能乘"变成一个反复出现的问题。
enum class NlaBlend : int {
    Replace = 0,
    Add = 1,
};

const char* nlaBlendName(NlaBlend b);
// 越界 → false（调用方保持原值）。读档容错用，与 boneMaskModeFromInt 同款。
bool nlaBlendFromInt(int v, NlaBlend* out);

// 时间轴上的一个片段。
struct NlaStrip {
    std::string name = "Strip";

    // 源 clip 名（`Skeleton::findClip` 逐字比较）。空 = 本 strip 失效。
    std::string clip;

    // 时间轴上的起止（秒）。`end <= start` = 失效。
    float start = 0.0f;
    float end = 1.0f;

    // 源 clip 里取哪一段（秒）。`clipEnd <= clipStart` = 取到 clip 末尾
    // （于是"新建的 strip 两个都是 0"天然等于"整段"，不需要哨兵值）。
    float clipStart = 0.0f;
    float clipEnd = 0.0f;

    // span 里塞几遍源动作（见约定 ②）。
    float speed = 1.0f;

    // true = span 里循环；false = 只走一遍，走完停在源区间的末尾。
    bool loop = true;

    // 整体权重 [0,1]。0 = 逐位等于没有这条 strip。
    float weight = 1.0f;

    // 交叉淡入 / 淡出（秒，各自相对 start / end 量）。0 = 硬切。
    // ⚠ 两者**可以重叠**（短 strip + 长淡入）—— 那时包络取两者的乘积，
    //   形状是一个梯形被压成三角形，永远不会超过 1，也不会变成负数。
    float blendIn = 0.0f;
    float blendOut = 0.0f;

    NlaBlend blend = NlaBlend::Replace;

    // 骨骼遮罩。`All` 或 roots 空 = 全骨（老数据 / 刚加的 strip 天然落这里）。
    // 注意语义与 `AnimLayer::mask` **完全一样**（同一个 `BoneMaskSpec`、
    // 同一个解算器）—— 这一处共用是对的：遮罩问的是"哪些骨"，
    // 与"这段动画怎么参与混合"无关。
    BoneMaskSpec mask;

    bool muted = false;

    // ---- 派生量（都是纯函数，无缓存；strip 只有几十条，算一次不心疼）----

    // end - start（下限 1e-6，避免除零把 u 变成 inf）
    float span() const;
    // 本 strip 在这一时刻是否参与求值（约定 ③）
    bool activeAt(float t) const;
    // 配置是否完整（clip 非空 / span 为正 / weight 为正 / 没静音）
    bool valid() const;
    // 淡入淡出包络 [0,1]：没有淡入淡出时**恰好 1.0f**（约定 ①）
    float envelope(float t) const;
    // 时间轴时刻 → 源 clip 时刻（约定 ②）
    float sourceTime(float t, float clipDuration) const;
};

// 一条轨。轨内的 strip **按列表顺序求值**（与 `ConstraintStack::items`
// 同一个约定：顺序就是语义，不按 start 排序 —— 排序会让"我明明调过顺序"
// 变成一句空话）。显示时 UI 自己按 start 排。
struct NlaTrack {
    std::string name = "Track";
    bool muted = false;
    // 独占：只要有任意一条轨 solo，就**只有** solo 轨参与求值。
    bool solo = false;
    std::vector<NlaStrip> strips;
};

// 求值账。日志与面板统计都吃它。
struct NlaEvalStats {
    int tracks = 0;         // 总轨数
    int tracksActive = 0;   // 真的参与了求值的轨数（未静音 / 过 solo）
    int strips = 0;         // 这些轨里的 strip 总数
    int active = 0;         // 播放头落在区间内的 strip 数
    int applied = 0;        // 真的混进姿势的 strip 数
    int skipped = 0;        // 想混但混不进去的（clip 找不到 / 遮罩空 / 包络 0）
    int masksUnresolved = 0;// 遮罩一根骨都没命中的次数（配错名字的头号症状）
    std::string firstProblem;  // 第一个问题（clip 名 / 骨名片段 / "mask"）
};

// 求值用的临时缓冲。**由调用方持有**，理由与 `AnimatorSystem::m_poseLayer`
// 完全一样：`Pose` 里三个 vector，每帧每 strip 现分配会把分配器打满。
struct NlaScratch {
    Pose stripPose;                        // 单个 strip 的采样结果
    Pose restPose;                         // Add 模式要的基准（懒算一次）
    bool restValid = false;
    // 遮罩解算缓存。key = `boneMaskKey(spec)`。
    // ⚠ 用 `std::map` 而不是 `unordered_map`：`maskFor` 返回**引用**，
    //   rehash 会让引用失效（与 `AnimatorSystem::m_maskCache` 同一个坑）。
    std::map<std::string, BoneMask> masks;
    std::vector<int> order;                // 本帧参与求值的轨下标（复用）
};

// 把整条时间轴在 `time` 处的姿势叠到 `pose` 上（就地改）。
//
// 语义：从下标 0 的轨开始，逐轨、轨内逐 strip，用 `blendPoseMasked`
// （Replace）或 rest 相对增量（Add）盖上去。所以**下标大的轨胜出**，
// 与"越往上越优先"的直觉一致。
//
// 空轨列表 / 全静音 → 立刻返回且 `pose` 逐位不变（约定 ①）。
NlaEvalStats applyNlaTimeline(const assets::Skeleton& skel,
                              const std::vector<NlaTrack>& tracks, float time,
                              Pose& pose, NlaScratch& sc);

// 时间轴总长（= 所有 strip `end` 的最大值；空 = 0）。
// UI 的标尺长度与"循环播放时该在哪里回头"都用它。
float nlaTimelineDuration(const std::vector<NlaTrack>& tracks);

// 所有 strip 数（含静音轨里的）。UI 与日志用。
int nlaStripCount(const std::vector<NlaTrack>& tracks);

// FNV-1a 64：把 tick 顺序、每个可序列化字段都吃进去。
// 用途与 `clipDigest` / `constraintDigest` 一样 —— "改了一下再改回来必须
// 复原"、"跨进程存读一致"这两件事只能靠摘要断言。
uint64_t nlaDigest(const std::vector<NlaTrack>& tracks);

} // namespace animation
