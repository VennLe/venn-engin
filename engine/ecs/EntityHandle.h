#pragma once
// ============================================================
// ecs/EntityHandle —— 实体在"脚本 / DLL 边界"上的**唯一**整数表示
//
// 为什么单独一个头：这套编码现在有**三个**消费者
//   ① `engine/script/LuaBackend.cpp`（Lua 侧句柄）
//   ② `engine/game/GameModule.cpp`  （C++ 游戏模块的 ABI）
//   ③ 游戏工程自己的代码（`vennModuleEntry` 拿到的 uint64_t）
// 各写一份必然漂移，而漂移的表现是"某个消费者的 0 号实体突然变成空"。
//
// ------------------------------------------------------------
// 编码：`((generation << 32) | id) + 1`，**0 = 空句柄**
// ------------------------------------------------------------
// · `generation` 那一半**不能丢**：丢了就是"实体被删后句柄复用，脚本/
//   模块静默改到别人身上"（`.vks` 的 `Value::K::Entity` 注释里记过）。
// · `+1` 的理由：`(0<<32)|0 == 0`，不加一是"0 号实体"和"没有实体"
//   共用一个数字 —— 文档里 `world.setParent(kid, 0)  -- 拔到根` 于是会
//   静默把子节点挂到 0 号实体（常常就是玩家）。
//   加一之后 0 只表示空，`valid(0)` 为假。
//   ⚠ 判据：`--selftest lua` 的 **J7 / J8**（两半分开断言 —— `+1` 被撤
//     和"空分支被删"是两种不同的坏法，合并成一条就分不清是哪半坏了）。
// ============================================================

#include "ecs/Entity.h"

#include <cstdint>

namespace ecs {

// 空句柄的唯一取值。所有"找不到 / 没有父节点 / 实体已销毁"都返回它。
inline constexpr uint64_t kEmptyHandle = 0ull;

inline uint64_t packHandle(Entity e) {
    if (!e.valid()) return kEmptyHandle;
    return ((uint64_t(e.generation) << 32) | uint64_t(e.id)) + 1ull;
}

inline Entity unpackHandle(uint64_t v) {
    if (v == kEmptyHandle) return Entity{};
    const uint64_t raw = v - 1ull;
    Entity e;
    e.id = static_cast<EntityId>(raw & 0xFFFFFFFFull);
    e.generation = static_cast<uint32_t>(raw >> 32);
    // `0xFFFFFFFF` 那一档是 `kInvalidEntity` 的 id，还原出来也该是"空"
    if (e.id == kInvalidEntity) return Entity{};
    return e;
}

} // namespace ecs
