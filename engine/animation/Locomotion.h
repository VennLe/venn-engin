#pragma once
// ============================================================
// animation/Locomotion —— idle / walk / run / jump 四态切换 + 按键强播
//
// 为什么要有这个文件（2026-10-22）：
//   这套"按速度选 clip"的逻辑原本**只**长在 `runtime/RuntimeApp.cpp` 的
//   `tickPlayerAnimation` 里，于是同一个角色在两只程序里表现不同：
//     · Game.exe（运行时） —— 有状态机，走起来会迈步
//     · 编辑器 Play       —— 只有 `runPlayers` 挪实体，clip 永远停在
//                            实例化时挑的第一段（通常是 Idle）
//   用户原话："按 wasd 可以让角色前后左右移动，这个移动时是要结合角色的
//   行走动画的"。所以把它抽成一份**两边共用**的实现，谁改都只改一处。
//
// ------------------------------------------------------------
// 为什么用"速度"而不是"输入轴"
// ------------------------------------------------------------
// 输入只有两个**物理真值**：水平速度、是否贴地。用输入轴的话，撞墙时输入
// 满档但速度是 0 —— 腿会在原地跑，而日志里一切正常（这是最典型的"手感
// 不对"来源）。
//
// 状态判定的顺序是硬的：**空中优先**。跳跃中速度可能很小（起跳瞬间），
// 也可能很大（助跑跳），按速度判会先落进 idle/walk，于是"跳到一半突然迈步"。
//
// ------------------------------------------------------------
// 地面三态 = 混合空间（B 段，2026-11-06）
// ------------------------------------------------------------
// idle / walk / run 从"三态硬切"改成 UE `BS_Locomotion` 那一套：一张 1D
// 混合空间，横轴 = 水平速度，样本 = Idle@0 / Walk@walkAt / Run@runAt。
// 速度一变，两段动画的权重连续滑动，**不再换 clip**（只在主导段真的
// 跨过半时更新 `clipName`）。
//
// 为什么这件事值得单独做：硬切的问题是"过渡期整身平均"——A 段整体淡出、
// B 段整体淡入，混合中段两条腿各走各的相位，肉眼是腿打结。混合空间里
// 同一时刻只有相邻两段、且相位对齐（`blendSyncPhase`），混出来的是
// "3/4 步 + 1/4 步"，仍然是一个连续步态。
//
// 判据仍然是**速度**（见上一节）：撞墙时输入满档、速度为 0，混合空间
// 自然落到 Idle 端 —— 不需要额外特判。
//
// ------------------------------------------------------------
// 空中三态：Jump / Fall / Land（UE 的状态机）
// ------------------------------------------------------------
// 旧版只有一个 jump 态（`!onGround` → Jump clip，**循环**播放）。按 UE
// 的 ABP 应当分三段：
//   · Jump   —— 上升（`velocity.z > 0`），一次性，**不循环**；
//   · Fall   —— 下降（`velocity.z <= 0`），循环/定格；
//   · Land   —— 落地后的一小段缓冲（`landRecoverSec`），一次性。
//
// "不循环"这条是硬修正：从 3 m 高的地方掉下来要 0.8 s，而 Jump clip 只有
// 1.0 s，看不出问题；但从 20 m 掉下来，旧的循环逻辑会让**起跳动作重播
// 五次** —— 玩家看到的是人在空中反复蹬腿。UE 里 Jump 段的 `Loop` 是关的，
// 播到末帧定格。
//
// 缺 clip 时的退化（本工程的 alicia.vmesh 只有 Idle/Walk/Run/Jump）：
//   · 无 `fallClip` → 沿用 `jumpClip`（仍然不循环）；
//   · 无 `landClip` → 不启用 Land 段，落地直接回地面混合空间。
// 退化**不报错**，但会在 `-ANIM-AIR` 里报出 `fall=Jump`，一眼看得出是
// "资产没有这一段"还是"配置写错了"。
//
// ------------------------------------------------------------
// 静止 8 秒重触发 idle
// ------------------------------------------------------------
// 需求原文："角色停在原地如果不动长达 8 秒以上就触发 idle 动画，此后如果
// 一直没移动，则每隔 8 秒继续触发。"
//
// 语义：站着不动的那一刻起算，每满 `idleTriggerSec` 秒把 idle **从头重放**
// 一次（`time = 0`）。也就是说：
//   · 停下来的瞬间照常进入 idle 循环（否则人会僵在上一个姿势里等 8 秒）；
//   · 第 8 / 16 / 24 … 秒各重放一次，节奏由 `idleTriggerSec` 决定；
//   · 一旦移动，两个计时器都清零，下次停下来重新起算。
// 0 = 关掉这条（此时行为与之前逐位一致）。
//
// ------------------------------------------------------------
// 按键强播
// ------------------------------------------------------------
// 角色动画面板里可以给任意 clip 绑一个键（`ecs::AnimKeyMapComponent`）。
// 按下后**强播**这一段：忽略状态机，`loop = false`，播完（或到
// `holdSeconds`）自动交还给状态机。这条路上不做混合 —— 引擎还没有
// crossfade（见 docs/character_locomotion.md §8 的"还没做"清单）。
// ============================================================

#include "animation/BlendSpace.h"
#include "ecs/Components.h"

#include <string>

namespace assets {
struct Skeleton;
}

namespace animation {

// 与 RuntimeConfig::PlayerVisual 里的四段 clip 名同源；编辑器用的是
// 同一套默认名（默认角色资产就是按这四个名字烤出来的）。
struct LocomotionConfig {
    std::string idleClip = "Idle";
    std::string walkClip = "Walk";
    std::string runClip = "Run";
    std::string jumpClip = "Jump";

    // 走 / 跑分界（m/s）：低于 walkSpeed 算 idle，高于 runSpeed 算 run。
    // runSpeed <= 0 = **自动**（控制器最大速度 × 1.15）。
    //
    // ⚠ 不能写死一个数：默认 maxSpeed 是 4.5 m/s、冲刺 ×2，写死 3.2 的话
    //   普通走路就已经超过阈值 —— 于是永远在播跑步，而"走路在跑"这种错最
    //   难被当成 bug 看出来（腿在动、位置在变，只是不像走路）。
    float walkSpeed = 0.15f;
    float runSpeed = 0.0f;

    // ---- Ground Locomotion 混合空间（2026-11-06）----
    //
    // 这三段不再是"选一段播"而是"混两段"。样本位置（横轴 = m/s）：
    //   Idle @ 0         Walk @ walkAt            Run @ runAt
    //
    // `walkAt` / `runAt` 为 0 = 自动：
    //   · walkAt = 控制器最大速度（**走路满速**时完全用 Walk 段）；
    //   · runAt  = `runSpeed`（自动时 = 最大速度 × 1.15）。
    // 为什么按"移动速度"而不是"这段动画的自然步速"放样本：UE 的
    // `BS_Locomotion` 里 Walk@200 / Run@500 是**动画自身的速度**，因为
    // 模板的 `MaxWalkSpeed` 就是 500。本工程走路满速 4.5、冲刺 9，而
    // Walk clip 的自然步速只有 1.4 —— 按自然步速放的话，4.5 m/s 的走路
    // 会整段落到 Run 上（"一按 W 就在跑"）。步幅不匹配那部分交给
    // `strideMatch`（每段各自调播放速率），两件事分工不同。
    bool blendSpace = true;
    float walkAt = 0.0f;
    float runAt = 0.0f;
    // 混合时两段**共享相位**（UE 的 Sync Group）。
    //
    // 只在 walk ↔ run 之间生效 —— idle 不在同步组里（待机没有步态相位，
    // 把它拉进组只会让起步时从"待机循环的中段"开始迈腿）。这一条就是
    // "跑步动作接在走路动作上不垫步"的全部秘密。
    bool blendSyncPhase = true;

    // ---- 空中三态（UE Jump / Fall / Land）----
    std::string fallClip;      // 下降段；空 = 沿用 jumpClip
    std::string landClip;      // 落地缓冲段；空 = 不启用
    // 落地后多少秒内保持在 Land 段（UE 的 `LandRecovery` 概念的最小版）。
    // 0 = 落地立刻回地面混合空间。0.10~0.20 s 是"能看出蹲了一下"、
    // 又不会让玩家觉得"落地有硬直"的区间。
    float landRecoverSec = 0.15f;

    // 切 clip 时是否把播放头归零。true = 每步都从头迈（步伐清晰），
    // false = 相位连续（不会每步"抖一下"，但停下再走会接在中间）。
    bool restartOnSwitch = true;

    // 静止多久重放一次 idle（秒）。0 = 关。
    float idleTriggerSec = 8.0f;

    // **状态切换的交叉淡入时长（秒）**。0 = 硬切（P4 的行为）。
    //
    // 这是"机械僵硬"的解药：硬切时腿的相位、手臂摆幅在同一帧内突变，
    // 眼睛一眼就能看出"动画在切"。0.18~0.25 s 是人能感觉到"顺"、
    // 又不会觉得"反应变慢"的区间。
    //
    // ⚠ 只对**换 clip** 生效，不作用于 idle 重放（同一段动画归零重播，
    //   淡入淡出只会让它看起来"卡一下"）。
    float blendSeconds = 0.20f;

    // ---- A4 步幅匹配（2026-10-30）----
    //
    // 开着的时候，walk / run 两段按"移动速度 ÷ 这段 clip 的自然步速"
    // 缩放播放速率，让脚步与位移对上（不滑步）。自然步速默认**从 clip 里
    // 量**（`animation/measureClipStride`），所以换 Mixamo 的 Walk 也自动
    // 生效；下面两个 `*StrideSpeed` 是"我知道这段是多少，别量"的覆盖。
    //
    // ⚠ 关掉它 = 退回 P4 的行为（1× 播放）。留着这个开关是因为它正是
    //   "滑步 vs 不滑步"的**对照**：同一段动画、同一个速度，只改这一位。
    bool strideMatch = true;
    // 速率夹范围。上面写了"夹住 = 承认做不到"，而 0.6~1.7 大致是
    // "还看得出是走路"的区间：0.6 以下像慢动作，1.7 以上像快进。
    float strideMinRate = 0.6f;
    float strideMaxRate = 1.7f;
    // 覆盖量出来的自然步速（m/s）。0 = 自动量。
    float walkStrideSpeed = 0.0f;
    float runStrideSpeed = 0.0f;

    // **脚下的坡度（度）**：0 = 平地，正 = 上坡，负 = 下坡。
    //
    // ⚠ 它**不参与** rate 的计算。rate 的分子永远是**水平速度**（调用方
    //   给的就是水平投影，见 `StrideMatch.h`），再乘一次 cos 就是二次投影，
    //   20° 坡上会少播 6%。
    // 它只用来把一个数报出来：平地 clip 没有竖直分量，于是脚在竖直方向
    // 落后身体 `v·tanθ` —— 那部分是 rate 修不了的（只能靠脚 IK 或换带
    // 抬脚的 clip）。不报的话，"上坡看起来比平地滑"会被当成 rate 算错。
    float slopeDeg = 0.0f;

    // ---- 相位速率曲线（A4b，2026-10-30）----
    //
    // 上面那些决定**平均**播放速率（常量倍率）。这一位决定一圈之内怎么
    // 分配它：开着时 `AnimatorSystem` 按相位查 `gain(φ)` 推进播放头，
    // 把承重期的残余起伏也收掉（见 `StridePhaseCurve`）。
    //
    // ⚠ 默认**关**，两个理由：
    //   ① 它改变所有循环 clip 的播放头轨迹 → 逐位基线会飘
    //      （`verify_procanim` / `verify_anim_runtime` 的 time 断言吃这个）；
    //   ② 它需要 `Locomotion` 已经把 `speed` 设成对的基准值才有效 ——
    //      单独打开只是在 1× 上做微调，看不出效果。
    // 所以它是"`strideMatch` 之上的第二层"，由应用层显式打开。
    bool strideCurve = false;
};

// 每个角色一份的运行时游标（不序列化：场景一打开就该从 idle 起算）。
struct LocomotionRuntime {
    std::string clip;          // 当前挂着的 clip（空 = 还没定过）
    int lastState = -1;        // 0 idle / 1 walk / 2 run / 3 jump
    unsigned missingMask = 0;  // bit i = 第 i 段的"不存在"已经喊过

    float idleHold = 0.0f;          // 连续静止时长（统计用）
    float sinceIdleRetrigger = 0.0f; // 距上次 idle 重触发的秒数

    // ---- 空中相位（2026-11-06）----
    // 0 ground / 1 rising / 2 falling / 3 landing（值是 `AirPhase`）。
    int airPhase = 0;
    float landRemain = 0.0f;   // Land 段剩余秒数
    int airSwitches = 0;       // 进/出空中态的次数（日志与自检用）
    int blendSamples = 0;      // 处于"两段混合"的帧数（证明混合真的在跑）

    // ---- 地面混合表的缓存（2026-11-06）----
    //
    // 键 = 三段 clip 名 + 两个样本位置。任何一个变了就重建（见 .cpp 的
    // `groundSpaceFor`）。不缓存的话每帧每角色一次 vector 分配 —— 在
    // 40 个 NPC 的场景里就是每帧 40 次。
    BlendSpace1D ground;
    float groundWalkAt = -1.0f;
    float groundRunAt = -1.0f;
    std::string groundIdle;
    std::string groundWalk;
    std::string groundRun;

    // 按键强播
    std::string triggerClip;
    float triggerRemain = 0.0f;
    bool restoreLoop = true;   // 强播前 sk.loop 的值，结束时还原
    int triggerCount = 0;
    int idleRetriggers = 0;

    // ---- 步幅匹配的缓存（A4）----
    // 量一次要采样 60~120 帧 × 全身 FK，所以只在**换 clip** 时量。
    std::string strideClip;      // 上一次量的是哪一段（空 = 还没量过）
    float strideSpeed = 0.0f;    // 量出来 / 配置指定的自然步速（0 = 量失败）
    int strideMeasured = 0;      // 成功量过几次（日志 / 自检用）
    bool strideWarned = false;   // "夹住了"只喊一次（同一次夹持期内）
};

// 空中相位。`Ground` 之外的三段对应 UE ABP 的 Jump / Fall / Land 状态。
enum AirPhase {
    kAirGround = 0,
    kAirRising = 1,
    kAirFalling = 2,
    kAirLanding = 3,
};

struct LocomotionFrame {
    int state = 0;              // 本帧状态（0 idle 1 walk 2 run 3 jump）
    const char* stateName = "idle";
    bool switched = false;      // clip 真的换了
    bool retriggered = false;   // 本帧重放了 idle
    bool triggerActive = false; // 本帧处于按键强播中

    // ---- 空中相位与混合空间（2026-11-06）----
    int airPhase = kAirGround;
    const char* airName = "ground";
    // 本帧是否处于"两段加权"里，以及那两段是谁。
    bool blended = false;
    std::string blendClipName;
    float blendWeight = 1.0f;   // 主段（`stateName` 对应的那段）的权重

    // ---- 步幅匹配（A4）本帧的结果 ----
    // `playbackRate` 是**这一帧真正写进** `speed` 的值（没匹配时 = 1）。
    // 三个数一起看才有意义：`speed / (strideSpeed × rate)` 就是残差。
    float playbackRate = 1.0f;
    float strideSpeed = 0.0f;   // 用到的自然步速（0 = 没量出来 / 没开）
    float strideResidual = 0.0f; // 夹住之后还剩多少滑步（0 = 完全不滑）
    // 斜坡上 rate 修不了的**竖直**残差（`cfg.slopeDeg == 0` 时恒为 0）。
    // 单位是"名义步速的几分之几"：0.57 = 竖直方向落后身体半个多步速。
    float strideVertical = 0.0f;
    bool strideMatched = false;
    bool strideClamped = false;
};

// 每个**固定步**调一次。dt <= 0 时只按当前状态重算（不推进计时器）。
//
// `verticalSpeed` 是竖直速度（m/s，正 = 上升）。空中三态靠它的符号分开：
// UE 就是这么分的（`Velocity.Z > 0` → Jump，否则 → Fall），**不能**用
// "滞空时长"或"速度大小"代替 —— 起跳瞬间竖直速度最大但正在上升，下落
// 到最低点时速度为零却在下降。
//
// 写入：`sk.clipName / time / playing / loop / blendClip / blendWeight /
// blendTime`，以及（A4 起）walk / run 两态的 `speed`（步幅匹配）；有
// AnimatorComponent 时同步写它的五项（那是权威字段，见 Components.h 的
// "权威关系"）。
// 日志锚点：`<tag>-ANIM ...`（tag 传 "GAME" / "EDIT"），格式与 P3 逐字一致，
// 老脚本的 `GAME-ANIM state=` 正则不用改。另有：
//   · `<tag>-ANIM-AIR phase=...`（只在相位翻转时打）
//   · `<tag>-ANIM-BLENDSPACE ...`（只在主导段变化时打）
//   · `<tag>-STRIDE ...`（步幅匹配，只在换 clip / 夹持状态变化时打）
LocomotionFrame updateLocomotion(LocomotionRuntime& rt,
                                 ecs::SkinnedMeshComponent& sk,
                                 ecs::AnimatorComponent* an,
                                 const assets::Skeleton& skel,
                                 float horizSpeed, bool onGround,
                                 float verticalSpeed, float maxSpeedHint,
                                 float dt, const LocomotionConfig& cfg,
                                 const char* tag);

// 强播一段 clip（按键映射 / 脚本调用）。
//   holdSeconds <= 0 → 用 clip 自己的时长（播完回到状态机）。
// clip 不在骨架里时返回 false 并打一行 WARN（**只喊一次**，由
// `rt.missingMask` 的 bit 4 记）。
bool playTriggerClip(LocomotionRuntime& rt, ecs::SkinnedMeshComponent& sk,
                     ecs::AnimatorComponent* an,
                     const assets::Skeleton& skel, const std::string& clip,
                     float holdSeconds, const char* tag);

// 轮询 `AnimKeyMapComponent` 里的按键映射；命中返回目标 clip 名，
// 没命中返回空指针。`*keyOut` 回填命中的键码（日志用）。
//
// ⚠ 判定函数由**调用方**提供，不在这里直接问 `core::Input`：
//   · 运行时（Game.exe）用 `core::Input::keyPressed`；
//   · 编辑器用 `ui_keys::pressed`（ImGui 的原始键状态）—— `core::Input`
//     那条路被 `io.WantCaptureKeyboard` 门控，Play 时只要有 ImGui 窗口
//     拿着键盘焦点就永远返回 false（实测"按了没反应、日志一行没有"）。
const std::string* pollKeyTrigger(const ecs::AnimKeyMapComponent& map,
                                  bool (*isKeyPressed)(int), int* keyOut);

} // namespace animation
