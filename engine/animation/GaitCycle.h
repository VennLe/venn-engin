#pragma once
// ============================================================
// animation/GaitCycle —— 参数化行走循环生成器（2026-10-30）
//
// ------------------------------------------------------------
// 需求怎么来的
// ------------------------------------------------------------
// 用户给了一份 Rigify 人形「24fps · 24 帧行走循环」的逐帧 K 帧表：
//
//   帧 1  Contact L（步幅最大，左脚跟触地）
//   帧 7  Down L   （重心沉底，左膝吸收冲击）
//   帧 13 Passing  （左腿单撑，右腿从体侧掠过）
//   帧 19 Up L     （重心顶到最高，左脚跟离地）
//   帧 25 Contact R（= 帧 1 的左右镜像，闭合）
//
// 那是一份**手工 K 帧**的工作流。搬到引擎里，"落地"要解决的是手工流程
// 天然做不到的三件事：
//
//   ① **换角色就作废**。表里写的是 `thigh.L` / `foot_ik.L`（Rigify 命名），
//      而本仓库的角色是 `mixamorig:LeftUpLeg` 这一套。所以先把骨架解析成
//      人形角色表（`HumanoidRig`），生成器只认**角色**，不认名字。
//   ② **"脚不滑步"靠眼睛做不到**。表 §6 那条硬规则（承重期脚掌 X/Z 完全
//      不动）在手工 K 帧里必然被破坏 —— 关键帧之间的插值不认这条约束。
//      这里反过来做：**先定脚的世界轨迹，再用两骨 IK 回填大腿/小腿**，
//      然后逐帧烘焙。于是"滑没滑"变成可以量的毫米数（`GaitReport`）。
//   ③ **表是死的**。真实项目天天要调步幅、步频、摆幅。全部参数进
//      `GaitParams`；产物是普通的 `animation::EditClip`，落地之后照样能
//      在 Dope Sheet / 曲线编辑器里继续手改 —— 生成器不制造第二套数据。
//
// ------------------------------------------------------------
// 表 → 相位的换算（这是唯一一处"表"与"引擎"的对齐约定）
// ------------------------------------------------------------
// 一个**完整周期** = 两步（左脚一步 + 右脚一步）。`cycleFrames = 48`
// （默认）时，表里那五帧正好落在：
//
//   相位 φ = 0      → 帧 1   Contact L（Extreme）
//   相位 φ = 1/8    → 帧 7   Down L   （Extreme）
//   相位 φ = 1/4    → 帧 13  Passing  （Extreme）
//   相位 φ = 3/8    → 帧 19  Up L     （Extreme）
//   相位 φ = 1/2    → 帧 25  Contact R（Extreme）＝ 帧 1 的镜像
//   …… 后半个周期是前半个的镜像
//   相位 φ = 1      → 帧 49  ＝ 帧 1（闭合帧，烘焙时与 φ=0 逐位相同）
//
// 表里"中间帧补充"那四帧（4 / 10 / 16 / 22）是 1/16 的奇数倍，烘焙模式下
// 会被标成 `KeyType::Breakdown` —— 摄影表上第一眼就能看出"这一下是转折点
// 还是补间"（R8 的类型表本来就是干这个的，它不参与求值）。
//
// ⚠ 表里"单步 24 帧"= 60 步/分（散步）。要日常步速就把 `cycleFrames`
//   调到 24~28（它自己的 §9 写的是"单步 12 帧"）。两个都要能出，所以
//   周期长度是参数，不是常量。
//
// ------------------------------------------------------------
// 坐标系：全部从骨架自己推
// ------------------------------------------------------------
// `Pose` 在 glTF 场景空间里（Y-up 或 Z-up 取决于加载路径），所以这里**不**
// 写死 ±X/±Y/±Z，而是从静止姿势量出三个轴：
//
//   up   = normalize(chest − hips)
//   left = 水平化(footL − footR)          ← 用**真实左右脚**定侧向，不猜
//   fwd  = normalize(cross(left, up))     ← 右手系下与 (left,up) 正交
//   然后用"脚尖朝前"当 oracle 校验 fwd 的符号（反了就两个一起翻）
//
// 于是同一份参数在 Mixamo（Y-up、朝 −Y）、Rigify（Z-up、朝 +Y）、以及
// 任意带 yaw 的摆位上给出**同一个动作**。自检里 `gait_frame_invariance`
// 就是拿"整具骨架先绕 up 转 37°"来钉这条的 —— 这种错误在截图上看不出来
// （就是一个朝别的方向的角色在走），只能靠数值。
// ============================================================

#include "animation/ClipEdit.h"

#include "assets/Skeleton.h"

#include <string>
#include <vector>

namespace animation {

// ------------------------------------------------------------
// 人形角色表
// ------------------------------------------------------------
// 只收**驱动步态必须用到**的那些骨。髋/脊/胸/颈/头 + 两条腿 + 两条手臂，
// 一共 17 根。手指、脚趾末端、肩膀辅助骨都不进来 —— 多一根就多一个
// "这个模型没有它怎么办"的分支，而步态对它们没有需求。
enum class GaitRole : int {
    Hips = 0,
    Spine,        // 腰椎（可以是 Hips 的下一根，也可以就是胸）
    Chest,        // 胸腔（脊柱最上面那节）
    Neck,
    Head,
    LeftUpperLeg, // 大腿根（髋关节）
    LeftLowerLeg, // 小腿（膝关节）
    LeftFoot,     // 踝
    LeftToe,
    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToe,
    LeftUpperArm,
    LeftForeArm,
    RightUpperArm,
    RightForeArm,
    // 手只参与**测距**（"触地帧左臂在后"这条判据要量到手），不写轨道。
    LeftHand,
    RightHand,
    Count,
};

constexpr int kGaitRoleCount = static_cast<int>(GaitRole::Count);

// 角色名（日志 / 报告用）："hips" / "leftUpperLeg" / ...
const char* gaitRoleName(GaitRole r);

// 解析结果。`joint[i] < 0` = 这个角色在这具骨架上没找到。
struct HumanoidRig {
    int joint[kGaitRoleCount];
    // 没解析到的角色名（去重、稳定顺序）。**不是错误** —— 缺 Neck/Toe 的
    // 骨架照样能生成（只是少了对应那一段动作），缺腿才不能。
    std::vector<std::string> missing;

    HumanoidRig();
    bool has(GaitRole r) const;
    // 步态必需：髋 + 两条腿（大腿/小腿/踝）。缺一根就没法解 IK。
    bool walkable() const;
    // 日志一行：`HUMANOID hips=.. legs=.. arms=.. missing=..`
    std::string summary() const;
};

// 按名字解析。匹配规则（**不看导入顺序，不看下标**）：
//   名字规范化 = 取最后一个 ':' 或 '|' 之后那段 → 小写 → 去掉空格/下划线/点。
//   于是 `mixamorig:LeftUpLeg` / `LeftUpLeg` / `thigh.L` / `Bip01_L_Thigh`
//   都能落到同一张候选表里（见 GaitCycle.cpp 的 kRoleNames）。
//
// ⚠ 不用 `Skeleton::findJoint` 的"包含匹配"：`findJoint("Leg")` 会先撞上
//   `LeftUpLeg`（包含且更短），于是"小腿"解析成"大腿"—— 症状是膝盖不弯、
//   整条腿变成一根直棍，而 IK 会"成功"地解出一个伸直的解。
HumanoidRig resolveHumanoidRig(const assets::Skeleton& skel);

// ------------------------------------------------------------
// 骨架三轴（up / left / fwd）
// ------------------------------------------------------------
// 纯几何，与参数无关：up = 髋→胸、left = 两踝连线的水平分量、
// fwd = cross(left, up)，再用"脚尖朝前"当 oracle 校验 fwd 的符号。
//
// 导出它是为了**只有一份实现**：生成器（buildGeom）与"从 clip 里量步速"
// （`StrideMatch.h`）必须拿到同一组轴，否则量出来的步速符号可能反。
struct HumanoidAxes {
    bool ok = false;
    std::string error;
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    glm::vec3 left{1.0f, 0.0f, 0.0f};
    glm::vec3 fwd{0.0f, 1.0f, 0.0f};
};
HumanoidAxes measureHumanoidAxes(const assets::Skeleton& skel,
                                 const HumanoidRig& rig);

// ------------------------------------------------------------
// 参数
// ------------------------------------------------------------
struct GaitParams {
    std::string clipName = "WalkCycle";

    float fps = 24.0f;
    // 一个**完整两步周期**的帧数。48 = 表里 1→25 那半步的两倍（帧号与表
    // 完全对齐）；24 = 一步 12 帧的日常步速（表 §9）。
    int cycleFrames = 48;

    // 一步走过的距离（米）。真正用到的前伸量还会被腿长夹一次
    // （见 GaitReport::strideLimited），夹住了就有报告，不会静默滑步。
    float stepLength = 0.60f;
    // 单腿承重占整个周期的比例。0.5 = 无腾空也无双支撑；0.62 ≈ 常态走路；
    // 0.8 = 慢走；0.42 = 跑（会有腾空）。
    float stanceFrac = 0.62f;

    float bob = 0.030f;   // 重心上下半振幅（米）——表 §9 的 ±0.02~0.04
    float sway = 0.030f;  // 重心左右半振幅（米）——表 §9 的 ±0.03
    float footLift = 0.100f;  // 摆动相脚离地峰值（米）

    float hipYawDeg = 4.0f;    // 骨盆随步幅左右拧（表 §5：3~5°）
    float chestYawDeg = 5.0f;  // 胸与骨盆反拧
    float torsoLeanDeg = 3.0f; // 躯干前倾
    float hipRollDeg = 3.0f;   // 骨盆侧倾（承重侧髋高）

    float armSwingDeg = 22.0f; // 摆臂幅值（相对静止姿势，绕侧向轴）
    // 手臂比腿晚几帧（overlap）。表 §7："手臂曲线比肩膀晚 2~3 帧"。
    float armLagFrames = 2.0f;
    float elbowDeg = 12.0f;    // 肘永远微曲

    float heelStrikeDeg = 10.0f; // 触地时脚尖上抬
    float toeOffDeg = 22.0f;     // 蹬离时脚跟起来
    // 蹬离时踝**抬高**多少（脚跟离地、绕脚尖转，踝自然升高）。
    // 不能给 0：脚掌绕脚尖转 22° 时踝会升 3~5 cm，踝不跟着升就是"脚插进
    // 地里"；给太多则是"踮着脚走"。
    float toeRise = 0.050f;

    // 允许为了"够得着"而下蹲多少（米）。腿几乎伸直的静止姿势如果直接
    // 迈大步，脚会落在接触范围之外 —— IK 只能把腿拉直、脚离地。
    // 这里给一个上限，超了就改夹前伸量（并报告 strideLimited）。
    float maxCrouch = 0.09f;

    // true = 只写关键姿态（表 §8 的 "Blocking 只用 1/7/13/19/25"），
    // 每个周期 9 个键；false = 逐帧烘焙（每帧一个键，脚不滑步靠它）。
    bool blocking = false;
};

// ------------------------------------------------------------
// 报告（判据全在这里 —— "看起来像在走" 不是判据）
// ------------------------------------------------------------
struct GaitReport {
    bool ok = false;
    std::string error;        // ok=false 时的人话原因
    HumanoidRig rig;

    float cycleSeconds = 0.0f;
    // **让这份动画不滑步的地面速度**（m/s）：把角色的移动速度设成它，
    // 动画里"承重脚相对身体后退"的速率就正好等于身体前进的速率。
    float nominalSpeed = 0.0f;
    float effectiveStep = 0.0f;   // 实际用上的一步长度（被夹之后）
    bool strideLimited = false;   // 被腿长夹过（想要 > 实际）
    float pelvisDrop = 0.0f;      // 为了够得着而下蹲了多少
    float bobEffective = 0.0f;    // 夹过之后的上下振幅
    // 把手臂从**导入姿势**放到自然下垂，转了多少度。T-pose 的资产会是
    // 70~90°，本来就垂着的是 0。不报这个数的话，"摆臂怎么没动静"
    // （= 绕手臂自身的轴转）查不出来。
    float armDownDeg = 0.0f;

    // ---- 实测（都从**烘焙好的 clip** 反采回来量，不是生成时的中间量）----
    float bobRange = 0.0f;         // 重心上下峰峰值（米）
    float swayRange = 0.0f;        // 重心左右峰峰值（米）
    // 髋在**前向**的位移峰峰（毫米）。原地走必须 ≈ 0 —— 这是 treadmill
    // 唯一的机器判据。
    float fwdDriftMm = 0.0f;
    float footLiftPeak = 0.0f;     // 摆动相脚离地峰值（米）
    float stanceSlideMm = 0.0f;    // 承重期脚掌**侧向**漂移（毫米，越小越好）
    float stanceVerticalMm = 0.0f; // 承重期脚掌**竖直**漂移（毫米）
    float stanceLineErrMm = 0.0f;  // 承重期脚掌前向"匀速直线"偏差（毫米）
    // 承重期脚掌相对身体后退的速率（m/s）——**最小二乘拟合斜率**，不是
    // 生成时那个 A。把这个数设成角色的移动速度，脚在**世界空间**里就是
    // 钉住的；因此它必须等于 `nominalSpeed`（差一个百分号就是"慢慢滑"）。
    float stanceSpeed = 0.0f;
    float mirrorErrMm = 0.0f;      // 半周期镜像误差（帧25 = 帧1 镜像）
    // 静止姿势**自带的**左右不对称（毫米）。绝大多数真人角色都有（一只脚
    // 在前、两只肘弯得不一样），而它会让"逐点镜像"这个判据在字面上永远
    // 不成立 —— 所以 `mirrorErrMm` 量的是**扣掉这个基线之后**的残差，
    // 这一项只是把基线报出来，免得有人以为镜子坏了。
    float restMirrorMm = 0.0f;
    float loopErrMm = 0.0f;        // 闭合帧与首帧的偏差（无缝循环）
    // 触地帧（φ=0）到半周期后，左脚往前走的距离（米）。它必须**为正**：
    // 负 = 触地那一帧脚在后面（腿的相位反了）。
    float legSwingM = 0.0f;
    // 同一段时间里左**手**往后走的距离（米）。它也必须为正 —— 即触地帧
    // 腿在前、同侧手在后。两条符号一起看才是判据：`legSwingM > 0` 而
    // `armCounterM < 0` 就是同手同脚（走路最刺眼的错误，而它在**单帧
    // 截图**上几乎看不出来，只能靠这一对符号）。
    float armCounterM = 0.0f;
    // 全程 max|踝−髋关节| / 腿长。> 1 = IK 够不着（腿会拉直、脚离地），
    // 也就是"看起来在滑步"的几何根因。< 1 才安全。
    float reachMargin = 0.0f;

    int tracks = 0;
    int keys = 0;
    int extremeKeys = 0;   // 被标成 Extreme 的键数（= 关键姿态数）
};

// 生成。`out` 会被整体覆盖（名字 / 时长 / loop / 轨道）。
// 失败（骨架不可走）时返回 ok=false 并填 error，`out` 保持为空。
GaitReport buildWalkCycle(const assets::Skeleton& skel, const GaitParams& p,
                          EditClip& out);

// 只量，不生成：把一段**已经烘焙好的** clip（可以是刚从盘上读回来的）
// 采样一遍，量上面那几个"滑步 / 闭合 / 镜像"的数。
//
// 为什么单独导出：判据必须落在**盘上那一份**，而不是生成函数里那个
// 还没落盘的中间对象。存盘 → 读回 → 再量，是唯一能证明"烘焙出来的
// 关键帧本身满足约束"的顺序（求值走的是另一条路：resolveEditTracks）。
GaitReport measureGaitClip(const assets::Skeleton& skel, const EditClip& clip,
                           const GaitParams& p);

// 相位 → 关键姿态名（"contact" / "down" / "passing" / "up" / ""）。
// 只用于日志与面板读数；相位不是 1/8 的整数倍时返回 ""。
const char* gaitKeyPoseName(float phase);

} // namespace animation
