#pragma once
// ============================================================
// ecs/Entity —— 实体句柄
//
// EntityId 是"槽位索引"，generation 是"代际号"。
// 实体销毁后槽位被回收复用，同时 generation 自增 —— 于是旧的
// 句柄再过 alive() 检查时 generation 不匹配，会被判为失效。
// 这是 ECS 中防止悬空句柄（use-after-free）的标准手法。
// ============================================================

#include <cstdint>

namespace ecs {

using EntityId = uint32_t;
constexpr EntityId kInvalidEntity = 0xFFFFFFFFu;

struct Entity {
    EntityId id = kInvalidEntity;
    uint32_t generation = 0;

    bool valid() const { return id != kInvalidEntity; }

    bool operator==(const Entity& o) const {
        return id == o.id && generation == o.generation;
    }
    bool operator!=(const Entity& o) const { return !(*this == o); }
};

} // namespace ecs
