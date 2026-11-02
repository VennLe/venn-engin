#pragma once
// ============================================================
// fx/EffectAsset —— 特效资产（`.veffect`）
//
// 一个 `.veffect` = **一次"命中"该发生的全部表现**：
//     N 层粒子 + 一个音效 + 震屏 + 闪白 + 顿帧 + 总时长
//
// ------------------------------------------------------------
// 为什么要有这一层（而不是让脚本一项项调）
// ------------------------------------------------------------
//   没有它，"打一下"要在脚本里写四五行（出粒子、播音效、抖屏、闪白），
//   而且每个角色、每种武器都要重写一遍。有了它，作者在动画事件表里填
//   `Hit|slash_a` 就够了 —— 打击感从"写代码"变成"填数据"。
//
// ------------------------------------------------------------
// 三条设计决定
// ------------------------------------------------------------
// 1) **粒子层直接复用 `ecs::ParticleEmitterComponent`**，不另立一套字段。
//    粒子组件有 50+ 个字段，自己再写一份解析必然漂移，而这种漂移
//    **不报错**（缺的字段按默认值走），表现成"场景里调好的发射器做成
//    特效就少了一半参数"。所以作者可以把 Inspector 里调好的发射器
//    整个拷进 `.veffect` 的 `particles` 数组，字段名与默认值完全一致。
//
// 2) **`duration` 不是"回收时刻"**。粒子寿命常常比特效本体长（火花
//    0.5 秒喷完、飞 1.8 秒），按 `duration` 回收会把还在飞的粒子截断。
//    真正的占用时长由 `effectiveDuration()` 算 —— 它是"最后一颗粒子
//    死掉的时刻"。这一条是 `UPDATE` 里唯一能防住"火花定格在空中"的
//    判据（共享池按遍历顺序分配槽位，提前回收 = 那批粒子不再被模拟）。
//
// 3) **层数上限 3**。不是随意取的：粒子池的发射器上限是 8（`MYVK_FX_EMITTERS`），
//    一次命中 3 层（火花 / 烟 / 光晕）已经够用，留下余量给同时发生的
//    多次命中。超出的层被**截断并记进 `warnings`**（不静默丢弃 ——
//    "我加了第 4 层怎么没效果"是最难自证的那类问题）。
//
// 分层：只依赖 `ecs/Components.h` 与 json，不碰 Vulkan / AssetManager，
// 所以自检能脱离设备跑。
// ============================================================

#include "ecs/Components.h"

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace fx {

// 一个特效最多几层粒子（见头注释第 3 条）
static constexpr int kMaxEffectLayers = 3;

// 时长的硬上限（秒）。防的是"作者填了 0 或负数"导致宿主永不回收 ——
// 表现是池被一个看不见的特效永久占满，之后所有命中都没火花。
static constexpr float kMaxEffectDuration = 10.0f;

// 顿帧期间的**仿真倍率**（不是 0）。
//
// ⚠ 为什么放 engine 侧而不是各自的 App：编辑器 Play 与出包运行时**必须
//   用同一个数** —— 否则"编辑器里手感对、出包变样"是最难查的一类问题
//   （两边都觉得自己对，而作者只能靠感觉说"好像不太一样"）。
//
// ⚠ 不是 0 的两个理由：
//   ① 计时器必须靠**真实时间**推进（我们确实是这么实现的），一旦哪天有人
//      改成用仿真 dt 递减，0 就是死锁；
//   ② 完全不推进会让"停顿中触发的下一次命中"永远排不上，连击节奏会断。
// 0.06 的含义：一帧（16.7ms）的仿真只前进 1ms —— 肉眼是"定住"，但仍在动。
static constexpr float kHitstopScale = 0.06f;

struct EffectAsset {
    std::string name;

    // ---- 时序 ----
    float duration = 0.6f;    // 本体时长（秒）。<= 0 会被夹成默认值

    // ---- 打击感 ----
    // 顿帧秒数。0 = 不顿。⚠ 由宿主用**真实时间**递减，不能用仿真 dt
    //（`timeScale` 被自己压低后会形成正反馈，永远走不完）。
    float hitstop = 0.0f;
    // 震屏量（trauma，0..1）+ 作用半径（米）。走 CameraSystem::shakeAt，
    // 所以远处的命中自动更轻。
    float shake = 0.0f;
    float shakeRadius = 12.0f;
    // 闪白（0..1 强度，秒）。0 = 不闪。
    float flash = 0.0f;
    float flashDuration = 0.0f;
    glm::vec3 flashColor{1.0f, 1.0f, 1.0f};

    // ---- 声音 ----
    std::string sound;          // 空 = 不播
    float volume = 1.0f;
    // 音高随机幅度（比例，0.08 = ±8%）。连击时同一个音反复播会"打碟"，
    // 一点点随机是廉价且有效的解药。
    float pitchVariance = 0.08f;

    // ---- 粒子层 ----
    std::vector<ecs::ParticleEmitterComponent> particles;

    // ---- 解析产物 ----
    std::vector<std::string> warnings;  // 非致命问题（层数截断 / 未知字段等）
    bool valid = false;

    // 真正占用宿主的时间：本体时长与"最后一颗粒子死掉"取大。
    // ⚠ 这是回收判据，不是 `duration`。见头注释第 2 条。
    float effectiveDuration() const;
};

// 解析一份 `.veffect` 的 JSON。`err` 非空时 `valid == false`。
//
// 非致命问题（层数超限、duration 非法被夹）写进 `warnings` 并**仍然 valid** ——
// 一份能用的资产不应该因为一处笔误整份报废，但作者必须能看到。
EffectAsset parseEffectAsset(const nlohmann::json& j, std::string* err);

// 从文件加载。路径是**绝对路径**（调用方负责用 resolveAssetPath 解析）。
EffectAsset loadEffectAssetFile(const std::string& absPath, std::string* err);

// 自检（E1~E9）。返回失败条数，输出 `FX-ASSET` / `FX-ASSET-TOTAL` 行。
int runEffectAssetSelfTest();

} // namespace fx
