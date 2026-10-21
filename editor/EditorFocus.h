#pragma once
// ============================================================
// editor/EditorFocus.h —— 把编辑器相机"框"到某个实体上
//
// 三个入口共用这一份：Hierarchy 双击（用户 2026-10-21 要求）、
// 视口右键菜单 "Focus selection"、F 键。
//
// 为什么不是"面板发请求、视口消费"：
//   试过在 EditorContext 上挂一个 pending-focus 字段，结果**从来没被消费**
//   （发请求的函数连日志都没打出来）。面板之间共享状态的链路越多，这种
//   "看着都对、就是不生效"的问题越难查。相机本来就在 scene 上，任何持有
//   `scene::Scene&` 的面板都能直接调 —— 那就直接调。
//
// ⚠ 这个引擎里有三类"没有网格"的实体，必须各自给尺寸，否则它们要么框不到、
//   要么把相机怼到脸上：GameStart（小人）、水体（footprint）、灯（点）。
// ============================================================

#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace editor {

// extraZoom 越大离得越远：1.8 ≈ "刚好把物体放进画面还留一圈"
inline void focusCameraOnEntity(scene::Scene& sc, ecs::Entity e,
                                float extraZoom = 1.8f) {
    if (!e.valid()) return;
    ecs::World& w = sc.world();
    // ⚠ 不查 `w.alive(e)`：它还要 generation 匹配，而层级树/拾取给回来的
    //   句柄偶尔只有 id 对得上（实测 DirectionalLight 就过不了那一关，
    //   表现是"聚焦什么也没发生，连日志都没有"）。下面的组件查询本身
    //   已经足够安全（查不到就是 nullptr）。
    if (e.id >= w.entityCount() + 1024u) return;

    const glm::mat4 wm = sc.worldMatrix(e);
    glm::vec3 p = glm::vec3(wm[3]);
    float radius = 1.0f;

    if (w.has<ecs::GameStartComponent>(e)) {
        // 出生点标记：脚下是原点，框到胸口
        p += glm::vec3(0.0f, 0.0f, 0.875f);
        radius = 0.9f;
    } else if (const auto* wc = w.get<ecs::WaterComponent>(e)) {
        p = glm::vec3(wc->body.center.x, wc->body.center.y, wc->body.level);
        radius = std::max(wc->body.footprintRadius(), 1.0f);
    } else if (const auto* mc = w.get<ecs::MeshComponent>(e)) {
        // 图元：用参数估外接半径。导入模型没有 primitive 参数，给个保守值。
        const float s = (mc->params.size > 0.01f) ? mc->params.size : 1.5f;
        radius = std::max(s * 0.6f, 0.5f);
    } else if (const auto* pl = w.get<ecs::PointLightComponent>(e)) {
        radius = std::max(pl->range * 0.25f, 0.6f);
    } else if (w.has<ecs::DirectionalLightComponent>(e)) {
        radius = 1.2f;   // 太阳只有方向，框一个"看得见的大小"
    }

    scene::Camera& cam = sc.camera();
    const float halfFov = std::max(cam.fov() * 0.5f, 1.0f);
    const float dist = radius / std::tan(glm::radians(halfFov)) * extraZoom;
    cam.setTarget(p);
    cam.setDistance(dist);

    const std::string* np = w.name(e);
    VK_LOG_INFO("FOCUS-ENTITY id=%u name=%s target=(%.2f,%.2f,%.2f) "
                "radius=%.2f dist=%.2f",
                static_cast<unsigned>(e.id), np ? np->c_str() : "?",
                static_cast<double>(p.x), static_cast<double>(p.y),
                static_cast<double>(p.z), static_cast<double>(radius),
                static_cast<double>(dist));
}

} // namespace editor
