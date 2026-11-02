#pragma once
// ============================================================
// fx/AnimEventRouter —— 把动画事件翻译成"动作"
//
// ------------------------------------------------------------
// 补的是什么洞
// ------------------------------------------------------------
//   B2 已经能把"这段动画播到第 0.3 秒了"算得很准（半开区间、换 clip 归零、
//   dt==0 不发），但 `AnimatorSystem::eventsThisFrame()` 全仓**只有自检在读**。
//   于是"第 0.3 秒出刀光"只能脚本 `wait(0.3)` 掐秒表 —— dt 一抖、
//   timeScale 一改就错位，换一段动画还得重新掐。
//
//   本文件就是那个缺失的消费者：**事件 → 特效 / 音效 / 震屏 / 顿帧**。
//   作者在 Inspector 的动画事件表里填一行 `Fx|fx/slash_a.veffect`，
//   打击感就从"写代码"变成"填数据"。
//
// ------------------------------------------------------------
// 事件写法（`kind` 不区分大小写；`payload` 是参数）
// ------------------------------------------------------------
//   kind=Fx        payload=fx/slash_a.veffect     出特效（路径相对资产根）
//   kind=Sound     payload=sfx/swing.wav          播音效（3D，位置 = 实体世界坐标）
//   kind=Shake     payload=0.35                   震屏（trauma，0..1）
//   kind=Hitstop   payload=0.08                   顿帧（秒；宿主用真实时间递减）
//   其它 kind                                       忽略（`ignored` +1）
//
//   ⚠ 也支持"整条写在 payload 里"的兜底写法（`kind` 为空时）：
//       payload = "Sound|sfx/swing.wav"
//     两种都认，是因为 `AnimEvent` 的 kind/payload 是两个自由字符串，
//     作者很容易只填一个。**但校验只有一份** —— 两条路最终都进
//     `parseAnimEventAction`，不存在两套判据。
//
// ------------------------------------------------------------
// ⚠ 谁负责"只处理一次"
// ------------------------------------------------------------
//   `eventsThisFrame()` 每帧开头清空，所以**一帧调一次 dispatch 就恰好一次**。
//   但调用方若在两条路径上都调（比如 Play 分支和编辑态分支各一次），
//   同一个事件会播两遍 —— 表现是"命中特效双重曝光"，很难看出来。
//   所以调度点必须是**唯一的**（见 EditorApp 里 `tickAndDispatchAnimEvents`）。
// ============================================================

#include "animation/AnimEvent.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace scene {
class Scene;
}
namespace fx {
class EffectService;
struct EffectSinks;
}

namespace fx {

struct AnimEventAction {
    enum class Kind { None, Effect, Sound, Shake, Hitstop };

    Kind kind = Kind::None;
    std::string path;     // Effect / Sound 用
    float value = 0.0f;   // Shake / Hitstop 用
    std::string reason;   // 为什么是 None（诊断用；成功时为空）
};

// 解析一条事件。**纯函数**（不碰场景、不碰设备），所以数值用例直接喂。
AnimEventAction parseAnimEventAction(const animation::FiredAnimEvent& ev);

// 一帧的分派统计（日志 / 自动化判据）
struct AnimEventStats {
    int total = 0;
    int effects = 0;
    int sounds = 0;
    int shakes = 0;
    int hitstops = 0;
    int ignored = 0;   // kind 不认识
    int failed = 0;    // 认识但参数坏了 / 实体没了
};

// 把一帧的事件全部落地。`scale` 乘在 shake / hitstop 上（慢动作时用）。
//
// ⚠ 位置取的是**事件所属实体**的世界坐标（不是相机、不是原点）：
//   震屏靠 `shakeAt` 做距离衰减，取错位置会让"远处的爆炸"震得和近处一样。
AnimEventStats dispatchAnimEvents(const std::vector<animation::FiredAnimEvent>& events,
                                  scene::Scene& scene, EffectService& fx,
                                  const EffectSinks& sinks);

// 自检（R1~R8），纯解析，不需要场景。返回失败条数。
int runAnimEventRouterSelfTest();

} // namespace fx
