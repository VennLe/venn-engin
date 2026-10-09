#pragma once
// ============================================================
// scene/Scene —— 游戏世界（ecs::World 的门面）
//
//   * 实体与组件全部由 ecs::World 管理
//   * 相机是"观察者"而非场景内容，由 Scene 直接持有
//   * 方向光以组件形式存在，light() 返回第一个方向光组件
//   * 父链世界矩阵在此递归累乘（HierarchyComponent）
// ============================================================

#include "ecs/Components.h"
#include "ecs/World.h"
#include "scene/Camera.h"
#include "scene/Light.h"

#include <string>

namespace scene {

class Scene {
public:
    Scene() = default;

    ecs::World& world() { return m_world; }
    const ecs::World& world() const { return m_world; }

    // 创建实体，自带 Name / Transform / Visibility 三个组件
    ecs::Entity createObject(const std::string& name = "GameObject");

    // 挂接父子关系：child 成为 parent 的子节点
    // （直接改局部矩阵语义，不做世界变换补偿）
    void setParent(ecs::Entity child, ecs::Entity parent);

    // 世界矩阵 = 沿父链累乘各层 localMatrix
    glm::mat4 worldMatrix(ecs::Entity e) const;

    void update(float dt);

    // ---- 遍历（供渲染层使用）----
    // 所有"可见 + 有网格 + 有材质"的实体
    template <typename Fn>
    void forEachRenderable(Fn&& fn) {
        m_world.each<ecs::MeshComponent, ecs::MaterialComponent>(
            [&](ecs::Entity e, ecs::MeshComponent& mc,
                ecs::MaterialComponent& matc) {
                const auto* vis = m_world.get<ecs::VisibilityComponent>(e);
                if (vis && !vis->visible) return;
                fn(e, mc, matc);
            });
    }

    // 所有"投射阴影"的实体（可见 + castShadow + 有网格）
    template <typename Fn>
    void forEachShadowCaster(Fn&& fn) {
        m_world.each<ecs::MeshComponent>(
            [&](ecs::Entity e, ecs::MeshComponent& mc) {
                const auto* vis = m_world.get<ecs::VisibilityComponent>(e);
                if (vis && (!vis->visible || !vis->castShadow)) return;
                fn(e, mc);
            });
    }

    // ---- 相机 ----
    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }

    // ---- 方向光（懒创建：首次调用时自动建实体）----
    Light& light();
    const Light& light() const;

    // ---- 局部光源收集（分簇前向渲染）----
    // 把所有 enabled 的点光/射灯展平成 LightInstance（位置取世界矩阵平移）。
    // 传入 out 由调用方复用，避免每帧分配。
    void collectLights(std::vector<LightInstance>& out) const;

    // 统计信息（ImGui 显示用）
    struct LightStats {
        size_t pointLights = 0;
        size_t spotLights = 0;
        size_t disabled = 0;
    };
    LightStats lightStats() const;

    ecs::Entity find(const std::string& name);

    // 清空场景（含方向光实体句柄的重置）
    // 注意：只清 ECS 世界，不动 AssetManager 里的网格/材质缓存
    void clear();

    size_t objectCount() const { return m_world.entityCount(); }

private:
    ecs::World m_world;
    Camera m_camera;
    ecs::Entity m_lightEntity;
};

} // namespace scene
