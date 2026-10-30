#pragma once
// ============================================================
// animation/StrideMatch —— 把"移动速度"和"这段 clip 该按多快播"对上
//（2026-10-30，A4 滑步）
//
// ------------------------------------------------------------
// 要解决的是哪一个错
// ------------------------------------------------------------
// 到 GaitCycle 为止，"走起来不滑步"只解决了一半：**clip 自己是原地走**
//（treadmill，髋的前向位移峰峰 = 0），而角色在世界里是靠碰撞驱动的位移。
// 两者之间没有任何约定的时候：
//
//   · 这段 walk clip 的自然步速是 0.60 m/s（`GaitReport::nominalSpeed`）；
//   · 玩家的 `moveSpeed` 是 3.0 m/s（`PlayerMoveComponent` 的出厂值）；
//   · 于是动画按 1× 播，身体却以 5 倍步速在前进 —— 脚在地上"滑冰"。
//
// 这个错**不会**在日志里出现，也不会让任何断言变红：腿在动、位置在变，
// 看上去"就是有点飘"。所以判据必须是一个数，而且这个数只能从这里出：
//
//   rate = 移动速度 / clip 的自然步速          （本文档的 strideRateFor）
//   残差 = 移动速度 / (自然步速 × rate) − 1     （夹过之后还剩多少滑步）
//
// ------------------------------------------------------------
// 自然步速为什么要**从 clip 里量**，而不是让调用方填
// ------------------------------------------------------------
// 填的话有三个逃不掉的坑：
//   ① 混用多个来源的 clip（本引擎生成 / Mixamo / Rigify）时每段都要人填，
//      而填错的症状就是上面那种"说不出哪里不对"的飘；
//   ② 改一个步幅参数（`GaitParams::stepLength`）就得同步改一个配置数字，
//      两处不同步没有任何提示；
//   ③ 换角色（腿长不同 → 步幅被夹）时，填的那个数就过期了。
//
// 而"自然步速"这个东西**本来就在 clip 里**：原地走的承重期，脚相对身体
// 以恒定速率往后走，那个速率就是地面速度。量它只要三件事 —— 骨架的角色
// 表（复用 `resolveHumanoidRig`）、骨架自己推出来的三轴（复用
// `measureHumanoidAxes`）、一条按时间采样的路径。于是 Mixamo 的
// `mixamorig:LeftFoot` 和 Rigify 的 `foot.L` 走的是**同一条代码**。
//
// ⚠ 量的和播的必须是**同一段** clip：`measureClipStride`（运行时
//   `assets::AnimationClip`）与 `measureEditClipStride`（编辑器的
//   `EditClip`）共用 `measureStride` 那一个内核 —— 两份实现在"承重期判定"
//   上差一点，就会得到"编辑器里不滑、Game.exe 里滑"。
//
// ⚠ **夹范围**（`minRate / maxRate`）不是保守，是承认做不到：只靠变速，
//   一段 0.6 m/s 的走路动画在 4.5 m/s 下要播 7.5 倍速 —— 那是发抖不是跑。
//   所以夹住之后**残差不为零是正常的**，但必须能被报出来（`residual`），
//   而不是悄悄滑。真正的解是换 clip（walk→run）或改步幅，两者都在
//   调用方手里。
// ============================================================

#include "animation/GaitCycle.h"
#include "animation/Pose.h"

#include "assets/Skeleton.h"

#include <functional>
#include <string>
#include <vector>

namespace animation {

// ------------------------------------------------------------
// 量出来的结果
// ------------------------------------------------------------
struct StrideInfo {
    bool ok = false;
    std::string error;      // ok=false 时的人话原因

    // **让这份动画不滑步的地面速度**（m/s）。承重期脚相对身体后退速率的
    // 中位数（中位数而不是均值：蹬离那几帧的脚尖回抽是离群点）。
    float nominalSpeed = 0.0f;
    float cycleSeconds = 0.0f;   // 参与测量的时长（一个循环）
    // 单脚承重占整个周期的比例（0.5 = 没有双支撑；0.62 ≈ 常态走路；
    // 0.35 左右 ≈ 慢跑）。
    //
    // ⚠ 判据是**两条一起**：高度在承重带里 **且** 脚相对身体在向后走。
    //   只看高度会得到一个自相矛盾的数 —— 真实资产 `alicia` 的 Run 曾经
    //   量出 0.61，比它的 Walk（0.52）还高（跑步抬腿高，松带宽到 15 cm，
    //   腾空期被算成承重）。见 `kContactFracOfTravel` 的注释。
    //
    // ⚠ 它仍然是**代理量**：真实走路有脚跟→脚尖的滚动，那一段脚在地上
    //   但踝已经抬起，落在承重带外 → duty 会比教科书的 stance 略小。
    //   判"这段是走还是跑"用它 + `airborneFraction` 一起看才可靠。
    float dutyFactor = 0.0f;

    // 两只脚**同时**离地的帧占比（0 = 任何一刻都至少有一只脚在地上）。
    //
    // 这是"有没有腾空"最直接的判据：走路恒为 0（有双支撑期），跑步
    // 必须 > 0。用"0 与非 0"而不是一个阈值 —— 那是两种步态，不是程度
    // 差别。（阈值型判据在 duty 上已经骗过我们一次。）
    // ⚠ 真实资产的 Walk 也可能量出非 0（动画本身带轻微腾空，比如"轻快
    //   的走法"）—— 那时判据要用**相对关系** `air(Run) > air(Walk)`，
    //   而不是"Walk 必须等于 0"。见 `tools/verify_gait_cycle.py` G 组。
    float airborneFraction = 0.0f;
    int airborneSamples = 0;

    // 脚相对身体的**前向摆幅**（峰峰，米，取两脚中的大者）。
    //
    // 它是"步幅"的几何代理，用来**独立交叉校验** `nominalSpeed`：
    //
    //     spanSpeed = 2 × strideSpan / cycleSeconds
    //
    // 两者应当同量级（±40%）。差得多说明承重期样本被污染了 —— 症状是
    // rate 用了错的 nominal，而 `duty` / `air` 看上去都正常。
    // ⚠ 它对跑步会**偏小**：跑步的脚在空中划圈，摆幅里有相当一部分不是
    //   步幅；所以只当旁证，不当主算法。
    float strideSpan = 0.0f;
    float spanSpeed = 0.0f;

    int samples = 0;         // 总采样帧数（两只脚合计算 2×）
    int contactSamples = 0;  // 落在承重期的样本数
    // 角色表里缺了几根（0 = 完整）。缺 Neck / Toe 不影响量步速。
    int missingRoles = 0;

    // ---- 诊断（"为什么 duty 是这个数"只能靠这两组）----
    // 每只脚的**脚底**上下行程（米）。0 = 这只脚整段没离开过低点，
    // 不参与承重判定（双撑站立 / 只有一只脚在动）。
    float footTravel[2] = {0.0f, 0.0f};
    // 脚底高度的最低点（米）。含义取决于 `soleAbsolute`：
    // true = 世界绝对高度（离地基准），false = **相对髋**的高度（负值正常）。
    // ⚠ 带根运动的 clip 里绝对值会在一段里漂 —— 它与 `hipRise` 一起
    //   才是"脚在抬"与"整个人在升"的分辨依据。
    float footLow[2] = {0.0f, 0.0f};
    // 髋沿 up 轴的上下行程（米）。远大于 0.1 说明"整具身体在竖直移动"
    // （跳跃 / 带根运动的 clip）。
    float hipRise = 0.0f;
    // 承重判定用的是哪套参考系：true = 世界绝对高度，false = 相对髋。
    // **由"两脚行程更小的那套"自动选出**（见 StrideMatch.cpp 里的长注释）：
    // 身体竖直漂移大的 clip 会选相对，骨盆 bob 大的会选绝对。
    // ⚠ 这个位是"duty 像是错的"时第二个要看的东西（第一个是 `soleFromToe`）。
    bool soleAbsolute = true;
    // 脚底高度是否用上了脚尖骨（`min(踝, 脚尖)`）。false = 骨架没有脚尖
    // 骨 → 走路"脚跟→脚尖滚动"那一段会被漏掉，duty 偏小。
    // ⚠ 这个位是"duty 看起来不对"时第一个要看的东西：它不是实现细节，
    //   而是"这具骨架能不能量出正确的 duty"的前提。
    bool soleFromToe = false;
};

// 采样函数：把 `time` 处的姿态写进 `out`（内部会自己算世界矩阵）。
// 用 std::function 是为了让"运行时 clip / 编辑器 EditClip / 自检夹具"
// 三条路共用同一个内核 —— 每次换 clip 才调用一次，开销无关。
using PoseSampler = std::function<void(float time, Pose& out)>;

// ------------------------------------------------------------
// 相位速率曲线（2026-10-30，接触锁定 / 时间重映射）
// ------------------------------------------------------------
// 常量倍率收掉的是**平均**滑步。承重期的脚速在 clip 内并不是恒定的：
// 骨盆起伏、脚踝滚动、关键帧量化都会让它在 nominal 上下浮动 5~15%。
// 乘上常量倍率之后，这段起伏原样留在世界里 —— 脚在地上"微微抖着滑"，
// 单帧看不出来，连续看就是"脚底在地面上蹭"。
//
// 要真的钉住，播放速率必须**按相位**变：
//
//   gain(φ) = nominal / v(φ)      承重期（v = 该相位的脚速）
//   gain(φ) = 1                   摆动期（没有接触约束，不用管）
//
// 不需要额外归一化：承重期的 v(φ) 围绕 nominal 波动，所以 gain 围绕 1
// 波动，一圈的平均步频几乎不变（`stats()` 报出来的 `gMean` 就是这件事
// 的判据）。摆动期恒为 1，所以周期的改变只来自承重期那点起伏。
//
// ⚠ gain 必须**夹范围**：v(φ) 在承重期的两个边界（刚触地 / 刚蹬离）会
//   趋向 0，取倒数会爆掉。夹在 [0.5, 2.0] 是"还能看出是同一段动作"的
//   边界 —— 再大就是"一步之内快慢两截"，比滑步更难看。
// ⚠ 推进必须**积分**（`advance`），不能"每帧查一次 gain 再乘 dt"：
//   一帧 dt 内 gain 在变，逐帧查表等于把曲线量化成帧率 —— 帧率一变
//   实际播放速度就变（这正是"60fps 不滑、30fps 滑"的来源）。
struct StridePhaseCurve {
    // 每个 bin 的承重期脚速（m/s，脚相对身体后退的速率）。
    // `<= 0` = 该 bin 不承重（摆动期）。
    std::vector<float> speed;
    float nominalSpeed = 0.0f;
    int bins = 0;

    bool valid() const { return bins > 0 && nominalSpeed > 0.0f; }

    // φ 处的速率增益。非承重区返回 1；承重区返回 nominal/v(φ) 并夹范围。
    // 相邻 bin 之间用线性插值 + 一次 3-tap 平滑，否则 32 个 bin 边界上
    // 各有一个速率阶跃 = 一秒 32 次"微顿"。
    float gain(float phase) const;

    // 把相位推进 `phaseDelta`（= dt × 基准速率 / clip 时长），按 4 个子步
    // 积分 gain。返回值已回绕到 [0,1)。
    //
    // 4 个子步是精度/开销的折中：60 fps 下每个子步 4 ms，而 bin 宽约
    // 30 ms —— 曲线在子步内近似线性，误差已在 1e-3 以下（自检里
    // `stride_curve_advance` 量的是"整圈累计误差"）。
    float advance(float phase, float phaseDelta) const;

    // 诊断：整圈 gain 的最小 / 最大 / 时间平均。`gMean` 是"步频有没有被
    // 这条曲线改变"的判据（1.00 ± 0.05 之外就说明曲线把步频带偏了）。
    void stats(float& gMin, float& gMax, float& gMean) const;
};

// 内核：在 [0, duration) 上均匀采 `duration × 60` 帧，量自然步速。
// `curve` 非空时顺带填相位速率曲线（默认不填 = 行为与引入曲线之前一致）。
StrideInfo measureStride(const assets::Skeleton& skel, float duration,
                         const PoseSampler& sample,
                         StridePhaseCurve* curve = nullptr);

// 运行时 clip（模型里那一段，Game.exe / 编辑器 Play 看到的都是它）。
StrideInfo measureClipStride(const assets::Skeleton& skel,
                             const assets::AnimationClip& clip,
                             StridePhaseCurve* curve = nullptr);

// 编辑器的编辑片段（刚生成、还没落盘的那一份）。
StrideInfo measureEditClipStride(const assets::Skeleton& skel,
                                 const EditClip& clip,
                                 StridePhaseCurve* curve = nullptr);

// ------------------------------------------------------------
// 速度 → 播放速率
// ------------------------------------------------------------
struct StrideRate {
    float rate = 1.0f;      // 要写给 `SkinnedMeshComponent::speed` 的倍率
    // 夹完之后**仍然存在**的滑步比例（0 = 完全不滑，0.25 = 还有 25%）。
    // 它等于 `moveSpeed / (nominalSpeed × rate) − 1`。
    float residual = 0.0f;
    bool clamped = false;   // 被 min/max 夹住了（= 变速已经救不回来）

    // **斜坡上 rate 修不了的那一部分**：平地 clip 没有爬升，于是脚在
    // 竖直方向上落后身体 `v·tanθ`。这里报的是"相当于名义步速的几分之几"。
    // 0 = 平地 / 没填坡度。修它只能靠脚 IK 或换带坡道的 clip，不是评分歧。
    float verticalResidual = 0.0f;
};

// `nominalSpeed <= 0`（没量出来 / 不是步态）→ 返回 rate=1、residual=0，
// 一个字节都不改 —— 与引入步幅匹配之前的行为逐位一致。
//
// ⚠ `moveSpeed` 必须是**水平投影**，不是三维速度模长。这一条在斜坡上是
//   6~15% 的差别，而两个数都能"看起来合理"：
//
//     rate = v_ground · cosθ / nominal      ← 对：水平脚速 = 水平身速
//     rate = v_ground        / nominal      ← 错：脚比身体快 (1/cosθ − 1)
//
//   调用方（运行时 / 编辑器 Play）算的本来就是水平速度，`slopeDeg` 只是
//   用来把竖直残差报出来，**不参与 rate**（参与就会二次投影）。
// ⚠ `moveSpeed` 比 `nominalSpeed` 小的时候**不夹下限到 1**：慢走就该慢放
//   （步频降下来），否则"轻推一下摇杆"会变成小碎步。下限存在的意义只是
//   防止 0.02 m/s 那种"几乎没动"给出 0.03 倍速（等于定格）。
StrideRate strideRateFor(float moveSpeed, float nominalSpeed, float minRate,
                         float maxRate, float slopeDeg = 0.0f);

} // namespace animation
