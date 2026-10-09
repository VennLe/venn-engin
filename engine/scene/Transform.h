#pragma once
// ============================================================
// scene/Transform —— 兼容转发头
//
// 变换数据已迁入 ecs::TransformComponent（组件化），
// 这里保留 scene::Transform 别名以免破坏已有调用。
// ============================================================

#include "ecs/Components.h"

namespace scene {

using Transform = ecs::TransformComponent;

} // namespace scene
