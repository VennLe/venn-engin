#pragma once
// ============================================================
// scene/ParticleJson —— 粒子发射器组件的 JSON 读写（**唯一一份**）
//
// 2026-11-02：从 `SceneSerializer.cpp` 提出来单独一个 TU，原因只有一个：
// `.veffect`（特效资产）要复用**同一份**粒子参数清单。
//
// 为什么必须复用而不是各写一套：
//   粒子组件有 50+ 个字段。特效资产若自己再写一份解析，两份清单必然
//   漂移 —— 加一个字段只在场景里生效、特效里静默按默认值走。
//   这种漂移**不报错**，表现成"同一个发射器摆进场景里是对的，做成特效
//   就少了一半参数"，而且只有逐字段对比才能发现。
//   所以：作者可以把场景里调好的发射器**整个拷进** `.veffect`，
//   字段名、默认值、兼容读法（`burst` ↔ `bursts`、`colorStart` 等）
//   全部一致。
//
// 分层：本文件只 include `ecs/Components.h` 与 json，**不**碰 Vulkan、
// 不碰 AssetManager —— 于是自检可以脱离设备跑（与 GameFlags 同一条理由）。
// ============================================================

#include "ecs/Components.h"

#include <nlohmann/json.hpp>

namespace scene {

// 组件 → JSON。写全字段（含 `burst` / `colorStart|Mid|End` 这些老读法），
// 所以产物既能被新版本读、也能被外部工具读。
nlohmann::json particleEmitterToJson(const ecs::ParticleEmitterComponent& p);

// JSON → 组件。**缺字段保留组件默认值**（不是清零）——
// 于是手写一个只有三行的发射器也能加到默认参数上。
ecs::ParticleEmitterComponent particleEmitterFromJson(const nlohmann::json& j);

} // namespace scene
