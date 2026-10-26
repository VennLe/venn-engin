#pragma once
// ============================================================
// animation/AnimController —— 数据驱动的动画状态机（A2b）
//
// 为什么要有这个文件：
//   在它之前，"idle / walk / run / jump"这套切换逻辑是 `Locomotion.cpp` 里
//   一段**硬编码的四态 if-else**，而 `AnimatorComponent::controllerPath`
//   字段从 A1 起就在序列化里、Inspector 上有控件，**全仓无读取者**。
//   界面上能填路径、能存盘、跑起来什么都不发生 —— 比"没有这个功能"更伤。
//
//   本文件把那七个占位字段里的五个接成真的：
//     controllerPath → 加载控制器资产
//     stateIndex     → 当前状态下标（真正被读写）
//     stateTime      → 在当前状态里待了多久（hasExitTime 用）
//     floatParams    → 转移条件的数值输入
//     boolParams     → 转移条件的布尔输入
//     triggerParams  → 一次性事件（被采纳后立即清零）
//   `rootMotion` 当时仍是占位 —— **2026-10-26 的 B3 已经接上了**
//   （`animation/RootMotion.*`，状态机切 clip 时它照常工作：位移按
//   增量提，切 clip 那一帧因为 `wrapTime` 不跨圈所以照提，不需要特判）。
//
// ------------------------------------------------------------
// 资产 schema（`assets/animators/*.anim.json`，schema 定稿见
// `docs/character_pipeline_roadmap.md` §A2）
// ------------------------------------------------------------
//   {
//     "version": 1,
//     "defaultState": "Idle",
//     "parameters": [
//       { "name": "Speed",    "type": "float",   "default": 0.0 },
//       { "name": "Grounded", "type": "bool",    "default": true },
//       { "name": "Jump",     "type": "trigger" }
//     ],
//     "states": [
//       { "name": "Idle", "clip": "Idle", "speed": 1.0, "loop": true  },
//       { "name": "Walk", "clip": "Walk", "speed": 1.0, "loop": true  }
//     ],
//     "transitions": [
//       { "from": "Idle", "to": "Walk", "duration": 0.20,
//         "conditions": [ { "param": "Speed", "op": ">", "value": 0.1 } ] },
//       { "from": "Any",  "to": "Jump", "duration": 0.10,
//         "conditions": [ { "param": "Jump", "op": "trigger" } ] }
//     ]
//   }
//
// ------------------------------------------------------------
// 四条容易写错的语义（都踩过或差一点踩）
// ------------------------------------------------------------
// 1) **trigger 被采纳后必须立即清零**。不清的话，从 Jump 回到 Idle 的下一帧
//    那条 `Any → Jump` 又成立 —— 角色会无限起跳。这是本类最典型的 bug。
//
// 2) **每帧最多采纳一条转移**。允许一帧连转（Idle→Walk→Run）时，条件恰好
//    同时成立会穿过中间的过渡时长，肉眼是"没过渡、直接弹到 Run"。
//
// 3) **`from: "Any"` 的优先级 = 数组顺序**，不是 map 遍历顺序、更不是随机。
//    实现上就是一个 `for` 从前往后扫、命中即 break —— 别改写成先按 to 分组
//    再用 unordered_map 查，那样同一份 JSON 在两台机器上可能走不同的边。
//
// 4) **`dt == 0` 不推进 stateTime，但仍要做初始化**。编辑器非 Play 态传的
//    就是 dt=0（"只按当前 time 重算姿势"）。此时状态机必须**已经**把
//    clip 切到 defaultState，否则用户拖时间轴看到的是一动不动的第 0 帧，
//    而日志里一切正常。
//
// ------------------------------------------------------------
// 加载失败怎么办：**不崩、不静默**
// ------------------------------------------------------------
// 校验不过（状态名重复 / 转移引用未知状态 / from==to / 条件引用未知参数 /
// JSON 语法错）→ 返回一个 `error` 非空的控制器对象，状态机**整个不接管**，
// 实体回落到"单 clip 直放"（defaultClip）。同时打一条 `ANIM-CTL-BAD`。
//
// 为什么不静默降级：静默降级后用户看到的是"我配的状态机没生效"，而唯一
// 线索是"角色还在播 defaultClip" —— 和"状态机生效了但只配了一个状态"
// 长得一模一样。所有加载期问题都必须在日志里指名道姓。
// ============================================================

#include "ecs/Components.h"

#include <memory>
#include <string>
#include <vector>

namespace assets {
struct Skeleton;
}

namespace animation {

enum class ParamType {
    Float = 0,
    Bool = 1,
    Trigger = 2,
};

enum class CondOp {
    Greater = 0,       // >
    GreaterEqual = 1,  // >=
    Less = 2,          // <
    LessEqual = 3,     // <=
    Equal = 4,         // ==
    NotEqual = 5,      // !=
    Trigger = 6,       // 该 trigger 参数为 true
};

const char* condOpName(CondOp op);
const char* paramTypeName(ParamType t);

struct ControllerParam {
    std::string name;
    ParamType type = ParamType::Float;
    float defaultValue = 0.0f;   // Float 用；Bool 用 !=0 判；Trigger 恒 false
};

struct TransitionCondition {
    std::string param;
    CondOp op = CondOp::Greater;
    float value = 0.0f;
};

struct ControllerState {
    std::string name;
    std::string clip;
    float speed = 1.0f;
    bool loop = true;
};

struct ControllerTransition {
    int from = -1;            // -1 = Any（任意状态）
    int to = -1;
    // 过渡时长（秒）。0 = 硬切。**不是**"延迟多少秒后才开始转" ——
    // 那是 `hasExitTime` + `exitTime` 的事，两件事别混。
    float duration = 0.15f;
    // 归一化退出时刻 [0,1]：只有 `stateTime >= exitTime * clipDur` 才允许转。
    // 典型用途：攻击动作播到 0.8 才能接下一段（防止连打穿模）。
    bool hasExitTime = false;
    float exitTime = 0.0f;
    // 切进新状态时是否把播放头归零。false = 相位连续（walk↔run 常用）。
    bool restart = true;
    std::vector<TransitionCondition> conditions;
};

struct AnimController {
    std::vector<ControllerParam> params;
    std::vector<ControllerState> states;
    std::vector<ControllerTransition> transitions;
    int defaultState = -1;

    // 非空 = 加载 / 校验失败。**此时整个控制器不接管**（见文件头）。
    std::string error;

    bool valid() const { return error.empty() && defaultState >= 0; }

    int findState(const std::string& name) const;
    int findParam(const std::string& name) const;
    const char* stateName(int idx) const;
};

// 从内存里的 JSON 文本解析（自检用 —— 不依赖磁盘）。
// 失败时返回一个 `error` 非空的对象，永远不返回 nullptr。
std::shared_ptr<AnimController> parseAnimController(const std::string& json);

// 从磁盘加载。内部用 `assets::resolveAssetPath`，所以传相对路径
// （`animators/humanoid.anim.json`）或绝对路径都行。
std::shared_ptr<AnimController> loadAnimController(const std::string& path);

// 按控制器声明初始化参数黑板。**只在第一次**（由 `an.paramsInitialized`
// 守）—— 每帧重置会把用户/脚本写进去的值抹掉。
void initControllerParams(const AnimController& ctl,
                          ecs::AnimatorComponent& an);

// 推进状态机一帧。
//
// 返回本帧**采纳的转移数**（0 或 1，见文件头第 2 条）。
// 写入：`an.stateIndex` / `an.stateTime` / `sk.clipName` / `an.time` /
//       `an.loop` / `an.speed`，以及被消费的 trigger 参数。
//
// `dt <= 0` → 只保证"已经初始化"（切到 defaultState），不推进 stateTime、
// 不求值转移 —— 编辑器的语义是"停在当前这一刻"。
int stepAnimController(const AnimController& ctl, ecs::AnimatorComponent& an,
                       ecs::SkinnedMeshComponent& sk,
                       const assets::Skeleton& skel, float dt, const char* tag);

} // namespace animation
