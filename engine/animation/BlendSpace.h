#pragma once
// ============================================================
// animation/BlendSpace —— 1D 混合空间（UE `UBlendSpace1D` 的等价物）
//
// 为什么要有这个文件（2026-11-06）：
//   在这之前，idle / walk / run 是**三态硬切**（换 clip + 0.2 s 交叉淡入）。
//   它能跑，但它不是 UE 的做法 —— UE 的 ThirdPerson 模板里，
//   `BS_Locomotion` 是一张**混合空间**：横轴 = Speed，样本 = Idle@0 /
//   Walk@200 / Run@500，速度一变，两段动画的权重就连续滑动。
//
//   两者的差别不在"看起来有没有过渡"，而在**过渡期间腿在干什么**：
//     · 硬切：A 段整体淡出、B 段整体淡入。混合期两个完整姿势在
//       "整身平均"，腿会短暂地处在"半步 + 半步"的位置 —— 也就是
//       两步交叠，肉眼像腿打结；
//     · 混合空间：同一时刻只有**相邻两段**、且相位对齐（Sync Group），
//       混出来的是"3/4 步 + 1/4 步"，仍然是一个连续的步态。
//   更关键的是：速度在 walk 与 run 之间来回小幅波动时，硬切会反复
//   换 clip（每换一次就是一次 0.2 s 淡入），而混合空间只是权重晃一下，
//   所以**看不出在切**。
//
// ------------------------------------------------------------
// 为什么是"线性 + 钳制"，不是曲线插值
// ------------------------------------------------------------
// UE 的 `FBlendSpace1D` 默认就是相邻两点线性插值（`GridBlendSample` 里
// 对 1D 退化成 lerp）；曲线的自由度来自作者拖样本点，而不是插值函数。
// 这里同样：想改"在哪个速度上完全走路"，改样本点的位置，不要改插值。
//
// 超出两端 → 钳到最近的样本（UE 的 `bClamp`/`bMirror` 默认钳）。**不能**
// 外插：Run 样本在 5.2 m/s，冲刺到 9 m/s 时外插会把 Walk 的权重算成负数，
// 混合结果是一个"反相"的姿势 —— 比不混合更糟。
//
// ------------------------------------------------------------
// 为什么样本带 `slot`
// ------------------------------------------------------------
// 上层（`animation/Locomotion`）要往日志里报 `state=idle|walk|run`，
// 而这份日志是**已有验证脚本的锚点**（`EDIT-ANIM state=walk clip=Walk`）。
// 让样本自己带语义槽（0 idle / 1 walk / 2 run），混合后取"权重大的那一
// 段"的槽名，日志措辞就与硬切时代逐字一致 —— 用例不用改，人也不用重新
// 学日志。槽只用于**命名**，不参与插值。
// ============================================================

#include <string>
#include <vector>

namespace animation {

// 语义槽：只用于给"当前主段"命名（写进日志），不参与插值。
enum BlendSlot {
    kSlotIdle = 0,
    kSlotWalk = 1,
    kSlotRun = 2,
    kSlotCount = 3,
};

struct BlendSample {
    std::string clip;   // 空 = 这个槽没有 clip，建表时就跳过
    float at = 0.0f;    // 横轴位置（本工程 = 移动速度 m/s）
    int slot = kSlotIdle;
};

// 求值结果。`lo == hi` 且 `wHi == 0` 表示"落在端点之外"（钳住了）。
struct BlendResult {
    int lo = 0;
    int hi = 1;
    float wLo = 1.0f;   // lo 段的权重（hi 段 = 1 - wLo）
    float x = 0.0f;     // 钳制之后的输入（报日志用）
    bool clamped = false;
};

class BlendSpace1D {
public:
    // 传进来的顺序无所谓（内部按 `at` 排一次）。`at` 相同的会保留先来的。
    void setSamples(std::vector<BlendSample> samples);

    bool empty() const { return m_samples.empty(); }
    int count() const { return static_cast<int>(m_samples.size()); }

    // x 落在 [s[i].at, s[i+1].at] → lo=i、hi=i+1，wLo 线性插值。
    // 小于第一点 / 大于最后一点 → 钳到端点（lo==hi，wLo=1）。
    BlendResult evaluate(float x) const;

    const BlendSample& sample(int i) const { return m_samples[static_cast<size_t>(i)]; }

    float minAt() const { return m_samples.empty() ? 0.0f : m_samples.front().at; }
    float maxAt() const { return m_samples.empty() ? 0.0f : m_samples.back().at; }

    // 主导段的槽（权重 >= 0.5 的那一段）。
    int dominantSlot(const BlendResult& r) const;

private:
    std::vector<BlendSample> m_samples;   // 按 at 升序
};

// 按 UE `BS_Locomotion` 的形状建一张地面混合表：
//   Idle @ 0、Walk @ walkAt、Run @ runAt
//
// 三个位置由调用方给（Locomotion 那边从配置 / 控制器最大速度算出来），
// 这里只管"单调、去重、跳过空 clip"。
BlendSpace1D makeGroundBlendSpace(const std::string& idleClip,
                                  const std::string& walkClip,
                                  const std::string& runClip, float walkAt,
                                  float runAt);

} // namespace animation
