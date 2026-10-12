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
#include "scene/Bvh.h"
#include "scene/Camera.h"
#include "scene/Light.h"

#include <string>
#include <vector>

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

    // ---- 空间加速结构（BVH）----
    //
    // 场景里所有"可见 + 有网格"实体的世界空间 AABB 索引。渲染的物体级
    // 视锥剔除与编辑器的射线拾取**共用同一棵**，省掉两套线性扫描。
    //
    // 刷新策略（每次调用都会走一遍）：
    //   * 实体集合没变 → Bvh::refit()：拓扑不动，只重算包围盒，O(N) 无分配；
    //   * 集合变了     → 整棵重建。
    //
    // ⚠ 每次调用都要把全场景的世界矩阵 + 8 个角点算一遍，同一帧里请
    //   **取一次往下传**，别在每个 pass 里各调一次。
    Bvh& spatialIndex();

    // 索引规模（ImGui 的 Stats 面板显示用）
    struct SpatialStats {
        size_t nodes = 0;     // 总节点数（含内部节点）
        size_t entities = 0;  // 被索引的实体数
        size_t depth = 0;     // 树高
    };
    SpatialStats spatialStats() const;

    // ---- 相机 ----
    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }

    // ---- 方向光（懒创建：首次调用时自动建实体）----
    Light& light();
    const Light& light() const;
    // 太阳实体的句柄（编辑器选中它时 Inspector 就能显示太阳参数）。
    // 注意：light() 尚未被调用过时句柄可能无效 —— 先调一次 light()。
    ecs::Entity lightEntity() const { return m_lightEntity; }

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
    // 重新收集"可见 + 有网格"实体并拟合 BVH（集合变了就重建拓扑）
    void refreshSpatialIndex();
    // 网格局部空间包围盒（扫一遍顶点）。返回 false = 没有顶点数据，
    // 该实体不参与索引（也就不会被剔除 / 拾取）。
    bool meshLocalBounds(const assets::Mesh& mesh, BvhAabb& out) const;

    ecs::World m_world;
    Camera m_camera;
    ecs::Entity m_lightEntity;

    // ---- 空间索引 ----
    Bvh m_bvh;
    // 复用的中间数组：每帧都要填一遍，避免反复堆分配
    std::vector<Bvh::Item> m_indexItems;
    std::vector<BvhAabb> m_indexBounds;
    // 上一轮索引到的实体序列（顺序也一起比 —— ECS 重新排列组件存储
    // 会让遍历顺序变化，这时叶子与 items 的下标对应关系失效，必须重建）
    std::vector<ecs::Entity> m_indexedEntities;
};

} // namespace scene
