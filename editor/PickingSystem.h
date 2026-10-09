#pragma once
// ============================================================
// editor/PickingSystem —— 屏幕空间 ⇄ 世界空间，以及射线求交
//
// 视口里所有"点鼠标"的行为都从这里出发：
//   · 点选物体         → pick()
//   · gizmo 拖拽       → rayFromViewport + closestOnAxis / intersectPlane
//   · gizmo 画在屏幕上 → worldToScreen
//
// ------------------------------------------------------------
// 坐标约定（很关键，弄反了拾取会"上下颠倒"）
// ------------------------------------------------------------
// 本引擎的投影矩阵是 glm::perspective 再把 proj[1][1] 取负（Vulkan 裁剪
// 空间 Y 向下）。于是 **世界 Y 增大 → NDC y 减小 → 屏幕 y 减小（往上）**，
// 与 ImGui/Win32 的 y 向下坐标系天然一致。
//
// 所以两边都用"最朴素"的公式即可，不需要额外的 Y 翻转：
//     screen.x = pos.x + (ndc.x * 0.5 + 0.5) * size.x
//     screen.y = pos.y + (ndc.y * 0.5 + 0.5) * size.y
//
// 射线反投影时用近平面与远平面上同一 NDC 点的世界坐标求方向，
// 比"从相机位置出发"更稳（正交/透视都不用改代码）。
// ============================================================

#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <unordered_map>

namespace assets {
class Mesh;
}
namespace scene {
class Scene;
class Camera;
}

namespace editor {

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 dir{0.0f, 0.0f, -1.0f};
};

struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool valid = false;
};

struct PickResult {
    bool hit = false;
    ecs::Entity entity{};
    float distance = 0.0f;
    glm::vec3 point{0.0f};
};

class PickingSystem {
public:
    // ---- 投影 ----
    // 视口矩形（imagePos / imageSize，屏幕像素）+ 相机 → 世界空间射线
    static Ray rayFromViewport(const scene::Camera& camera,
                              const glm::vec2& viewportPos,
                              const glm::vec2& viewportSize,
                              const glm::vec2& mousePos);

    static glm::mat4 viewProj(const scene::Camera& camera,
                              const glm::vec2& viewportSize);

    static bool worldToScreen(const glm::mat4& viewProj,
                              const glm::vec2& viewportPos,
                              const glm::vec2& viewportSize,
                              const glm::vec3& world, glm::vec2& outScreen);

    // ---- 拾取 ----
    // 遍历所有"可见 + 有网格"的实体，做射线 / 世界 AABB 求交，取最近命中
    PickResult pick(scene::Scene& scene, const Ray& ray);

    // ---- 求交原语 ----
    static bool intersectPlane(const Ray& r, const glm::vec3& planePoint,
                              const glm::vec3& planeNormal, float& t);
    // 射线与"过 lineOrigin、方向 lineAxis"的直线的最近点参数（沿 lineAxis）
    static bool closestOnAxis(const Ray& r, const glm::vec3& lineOrigin,
                              const glm::vec3& lineAxis, float& param);
    // 点到线段的屏幕距离（gizmo 轴命中判定）
    static float screenDistanceToSegment(const glm::vec2& p, const glm::vec2& a,
                                         const glm::vec2& b);

    // ---- AABB ----
    // 网格局部空间的包围盒（顶点数据在 CPU 侧保留，直接扫一遍；带缓存）
    Aabb meshAabb(const assets::Mesh& mesh) const;
    Aabb worldAabb(scene::Scene& scene, ecs::Entity e) const;

    void clearCache() { m_meshCache.clear(); }

private:
    // 查找缓存：const 方法也要写，标记 mutable
    mutable std::unordered_map<const assets::Mesh*, Aabb> m_meshCache;
};

} // namespace editor
