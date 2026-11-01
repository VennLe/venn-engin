#pragma once
// ============================================================
// animation/TransformClip —— `AnimationClipComponent` 的运行时消费者
//
// 为什么要有它（2026-10-31，C5）：
//   这个组件从 2026-10-20 就存在，关键帧进场景 JSON、`autoplay` 也落盘，
//   但**运行时全仓没有一个消费者** —— 唯一推进 `time` 的地方是
//   `AnimationEditor::sync()`，而它只在"编辑器处于 Animation 模式且**选中
//   了那个实体**"时跑。
//   于是：编辑器里拖时间轴能看见门在动，一进 Game.exe / 点 Play，门/电梯/
//   机关**全部定格**，而且没有任何报错 —— 数据在、就是不读。
//   这一层把"数据在"补成"跑得动"。
//
// 与 `AnimatorSystem` 的分工（两者不是一回事，见 `ecs/Components.h` 的长注释）：
//   * `AnimatorSystem`  —— 推进 **clip 资产**的时间，产出 jointMatrices（骨骼）
//   * 本模块            —— 推进 **实体自身 TRS** 的轨道，直接写 `TransformComponent`
//   互不调用、互不覆盖：一个蒙皮网格可以同时挂两者（身体在走路 + 整个物件
//   在沿轨道升降），各管各的字段。
//
// 三条纪律：
//   ① **采样必须与编辑器同一套**。插值是 smoothstep，不是线性 —— 两边各写
//      一套的话，编辑器里调好的节奏一到运行时就会变快/变顿。
//   ② **没在播就不许写 transform**。每帧无条件采样会把用户/脚本摆好的
//      位姿按最后一帧覆盖掉 —— 症状是"脚本 setPos 没反应"，而且极难查。
//      → `tick` 只在**本帧确实推进过**时采样；"进入时的第一帧"由
//      `syncTransformClipPlayState` 负责。
//   ③ **空轨道不许动 transform**。`keys.empty()` 时直接返回；否则一个
//      刚加上组件还没 K 帧的物体会被瞬间拍回原点。

#include "ecs/Components.h"
#include "ecs/Entity.h"

namespace scene {
class Scene;
}

namespace anim {

// 把 clip 在 t 处的 TRS 写进 out。
// 只改 position / rotation / scale，**不碰** parentGuid / 其它字段。
// `keys` 为空 → 什么都不做（返回时 out 保持原样）。
void sampleTransformClip(const ecs::AnimationClipComponent& c, float t,
                         ecs::TransformComponent& out);

// 推进播放头。
//   返回 true  = 这一帧确实推进了 → 调用方应当重新采样
//   返回 false = 没在播 / dt<=0 → 调用方**不应该**动 transform
// `loop` 走 `fmod`（卡顿一帧跨过整圈也能接上）；不循环则停在 `duration`
// 并把 `playing` 置 false（下一帧开始返回 false，不再写 transform）。
bool advanceTransformClip(ecs::AnimationClipComponent& c, float dt);

struct TransformClipStats {
    int playing = 0;   // 本帧推进过的
    int ended = 0;     // 本帧刚播到头的（打一条 ANIMCLIP-END）
    int applied = 0;   // 本帧写回了 transform 的
};

// 每帧一次。`dt` 传 0 是合法的（编辑器暂停 / 首帧），此时只推进不动的那些。
TransformClipStats tickTransformClips(scene::Scene& sc, float dt);

// 进入运行时（点 Play / 开 Game.exe）或读档之后，把播放状态对齐到 `autoplay`：
//   * `playing = autoplay` —— "重开场景时是否在播由 autoplay 决定"，
//     不该由"上次退出时刚好在播"决定（与 `AnimatorComponent::time` 同一条规矩）
//   * `resetTime == true`  → `time = 0`（点 Play 是"从头试玩"）
//     `resetTime == false` → 保留 `time`（读档要接上存盘那一刻）
// 无论哪种都会**采样一次**，让画面在进入的那一帧就对（否则会先闪一帧模型
// 自带位姿，下一帧才跳到轨道第 0 帧）。
void syncTransformClipPlayState(scene::Scene& sc, bool resetTime);

// 数值自检（`Game.exe --selftest transformclip`）：无窗口、无设备。
// 返回 0 = 全绿。判据见 `TransformClipSelfTest.cpp` 的文件头。
int runTransformClipSelfTest();

} // namespace anim
