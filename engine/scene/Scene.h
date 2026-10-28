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
#include "scene/PhysicsSettings.h"
#include "scene/PostVolume.h"

#include <string>
#include <unordered_map>
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

    // 沿父链向上找第一个带组件 T 的祖先（**含自己**）。找不到 → nullptr。
    //
    // 用来回答"这个网格属于哪具骨架"：`SkinnedMeshComponent` 挂在**模型根**
    // 上，而每个 SubMesh 是它的子级；渲染层要拿到逐实体的 jointBase 就必须
    // 从网格实体往上走一层。
    //
    // 为什么必须逐实体查、不能按 `Mesh*` 建映射：`AssetManager` 对同一个
    // 文件返回同一个 `Model*`，它下面的 `Mesh*` 也就完全相同 —— 按 mesh
    // 建映射时，同一 FBX 的第二个实例会覆盖第一个，三个敌人共享一份关节
    // 矩阵（全部显示同一个姿势）。2026-10-18 角色链 A0-4 修的就是这个。
    //
    // 深度用 guard 夹住：环形父子关系（不该出现，但序列化能造出来）
    // 不该让渲染线程死循环。
    template <typename T>
    T* findInAncestors(ecs::Entity e) {
        int guard = 0;
        while (e.valid() && guard++ < 128) {
            if (T* c = m_world.get<T>(e)) return c;
            const auto* h = m_world.get<ecs::HierarchyComponent>(e);
            if (!h || h->parent == ecs::kInvalidEntity) return nullptr;
            e = m_world.handle(h->parent);
        }
        return nullptr;
    }
    template <typename T>
    const T* findInAncestors(ecs::Entity e) const {
        int guard = 0;
        while (e.valid() && guard++ < 128) {
            if (const T* c = m_world.get<T>(e)) return c;
            const auto* h = m_world.get<ecs::HierarchyComponent>(e);
            if (!h || h->parent == ecs::kInvalidEntity) return nullptr;
            e = m_world.handle(h->parent);
        }
        return nullptr;
    }

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

    // 所有**启用**的延迟贴花（R4-4，见 ecs::DecalComponent）。
    //
    // 贴花没有网格 —— 盒体几何是渲染器自己建的单位立方体，场景侧只提供
    // 世界矩阵与参数。所以这里不像 forEachRenderable 那样要求 MeshComponent。
    template <typename Fn>
    void forEachDecal(Fn&& fn) {
        m_world.each<ecs::DecalComponent>(
            [&](ecs::Entity e, ecs::DecalComponent& dc) {
                if (!dc.enabled) return;
                fn(e, dc);
            });
    }

    // 所有**启用**的反射探针（R5-1，见 ecs::ReflectionProbeComponent）。
    template <typename Fn>
    void forEachReflectionProbe(Fn&& fn) {
        m_world.each<ecs::ReflectionProbeComponent>(
            [&](ecs::Entity e, ecs::ReflectionProbeComponent& pc) {
                if (!pc.enabled) return;
                fn(e, pc);
            });
    }

    // 所有**启用**的地形（W4，见 ecs::TerrainComponent）。
    // 与贴花/探针/粒子同一条约定：地形也没有网格组件 —— 几何由高度图
    // 在渲染层生成，场景侧只提供"哪份资产 + 什么变换"。
    template <typename Fn>
    void forEachTerrain(Fn&& fn) {
        m_world.each<ecs::TerrainComponent>([&](ecs::Entity e,
                                               ecs::TerrainComponent& tc) {
            // ⚠ 两处都要认：`tc.visible` 是地形自己的开关（Inspector 里那一行），
            //   `VisibilityComponent` 是**实体级**的（Hierarchy 的眼睛图标）。
            //   地形是整片地面 —— 用户闭眼睛的时候期待它消失，只认前者会
            //   出现"眼睛关掉了，山还在"。（贴花/探针历史上只认前者，但那两类
            //   都是小物件，看不出问题；地形不一样，所以这里多认一次。）
            const auto* vis = m_world.get<ecs::VisibilityComponent>(e);
            if (vis && !vis->visible) return;
            if (!tc.visible || tc.source.empty()) return;
            fn(e, tc);
        });
    }

    // 所有**启用**的水体（2026-10-21，见 ecs::WaterComponent）。
    // 与地形同一条约定：没有网格组件 —— 几何由渲染层按形状现生成，
    // 场景侧只提供"一块水体长什么样"。
    template <typename Fn>
    void forEachWater(Fn&& fn) {
        m_world.each<ecs::WaterComponent>([&](ecs::Entity e,
                                              ecs::WaterComponent& wc) {
            // 与地形一样两处都认：组件自己的 visible + 实体级的眼睛图标。
            const auto* vis = m_world.get<ecs::VisibilityComponent>(e);
            if (vis && !vis->visible) return;
            if (!wc.visible) return;
            fn(e, wc);
        });
    }

    // 所有**启用**的粒子发射器（R6-1，见 ecs::ParticleEmitterComponent）。
    // 与贴花 / 探针同一条约定：粒子也没有网格 —— 几何是顶点着色器现拼的
    // billboard，场景侧只提供世界矩阵（位置 + 朝向）与发射参数。
    template <typename Fn>
    void forEachParticleEmitter(Fn&& fn) {
        m_world.each<ecs::ParticleEmitterComponent>(
            [&](ecs::Entity e, ecs::ParticleEmitterComponent& pc) {
                if (!pc.enabled) return;
                fn(e, pc);
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

    // ---- 脚本世界黑板（S2）----
    //
    // `worldAttr(key)` / `setWorldAttr(key, v)` 读写这里。与
    // `ecs::ScriptVarsComponent` 的分工（刻意分开、别混）：
    //   · ScriptVarsComponent = 逐实体的**本局**状态（血量/朝向），不存盘；
    //   · 这里                = 整关的**世界**状态（门开过没有、拿到了几把钥匙），
    //                           **进存档**。
    //
    // 为什么放在 Scene 而不是某个单例：存档的粒度就是 Scene，放在别处就得
    // 单独设计一套"关卡级状态"的存盘/恢复流程。
    std::unordered_map<std::string, double>& scriptVars() { return m_scriptVars; }
    const std::unordered_map<std::string, double>& scriptVars() const {
        return m_scriptVars;
    }

    // ---- 相机（多相机）----
    // 相机是"观察者"而不是场景内容，所以不由 ECS 持有，而是 Scene 直接
    // 维护一个数组：下标 0 是**默认相机**（编辑器轨道相机 / 游戏主相机），
    // camera() 等价于 camera(0) —— 所有既有调用点语义不变。
    //
    // 其余相机供"多视口"使用：Renderer 的通用 RenderTarget 可以各自绑定
    // 一台（小地图 / 驾驶舱 / 反射探针 / 画中画），一台相机一个观察角度。
    // 相机之间没有任何隐含耦合，改一台不影响另一台。
    Camera& camera() { return m_cameras[0]; }
    const Camera& camera() const { return m_cameras[0]; }
    Camera& camera(size_t index) {
        return m_cameras[index < m_cameras.size() ? index : 0];
    }
    const Camera& camera(size_t index) const {
        return m_cameras[index < m_cameras.size() ? index : 0];
    }
    size_t cameraCount() const { return m_cameras.size(); }
    // 追加一台相机（默认从 0 号相机复制一份 —— 新视角的起点通常是
    // 当前视角，这比从单位矩阵开始好调得多）。返回新相机的下标。
    size_t addCamera();

    // ---- 后处理体积（见 scene/PostVolume.h）----
    // 体积是**场景内容**：一个 AABB 里覆盖一组相机后处理参数（景深对焦
    // 距离 / 运动模糊强度 / 色差 / 暗角）。Renderer 每帧按相机位置求值，
    // 叠加到基础设置上得到生效值。
    //
    // 相机与体积都在 Scene 上，好处是"多相机 + 体积"天然可用：换一台
    // 相机就是换一个求值位置。
    PostVolumeSet& postVolumes() { return m_postVolumes; }
    const PostVolumeSet& postVolumes() const { return m_postVolumes; }

    // 物理世界参数（重力 / 步长 / 迭代 / 休眠）。是**场景数据** ——
    // 随场景一起序列化，换场景就换物理手感。见 scene/PhysicsSettings.h。
    PhysicsSettings& physicsSettings() { return m_physics; }
    const PhysicsSettings& physicsSettings() const { return m_physics; }

    // ---- 方向光（懒创建：首次调用时自动建实体）----
    Light& light();
    const Light& light() const;
    // 太阳实体的句柄（编辑器选中它时 Inspector 就能显示太阳参数）。
    // 注意：light() 尚未被调用过时句柄可能无效 —— 先调一次 light()。
    ecs::Entity lightEntity() const { return m_lightEntity; }

    // 场景级太阳是否**可见**（Hierarchy 行尾 👁 的落点）。
    //
    // 为什么单独给一个口子：太阳不是"可渲染物"（没有 MeshComponent），
    // `forEachRenderable` 的 `visible` 闸门拦不到它 —— 它的光是从
    // `light()` 直接喂进 FrameUBO 的。所以渲染器得自己问一句。
    // **缺组件 = 可见**（老场景 / 程序化建的太阳都按可见处理）。
    bool sunVisible() const {
        const auto* vis = m_world.get<ecs::VisibilityComponent>(m_lightEntity);
        return !vis || vis->visible;
    }

    // ---- 局部光源收集（分簇前向渲染）----
    // 把所有 enabled 的点光/射灯展平成 LightInstance（位置取世界矩阵平移）。
    // 传入 out 由调用方复用，避免每帧分配。
    // mods 非空时同步填一份逐灯修饰描述（R3-3：区域光 / IES / light function），
    // **下标与 out 严格一一对应**（同一盏灯在同一位置 push）。
    void collectLights(std::vector<LightInstance>& out,
                       std::vector<LightModifierDesc>* mods = nullptr) const;

    // 统计信息（ImGui 显示用）
    struct LightStats {
        size_t pointLights = 0;
        size_t spotLights = 0;
        size_t disabled = 0;
    };
    LightStats lightStats() const;

    ecs::Entity find(const std::string& name);

    // ---- 把一个"曾经指向某实体"的引用接回当前世界（G16-4）----
    //
    // 需要它的场合：撤销 / 进 Play 再 Stop / 重新加载 —— 世界被整场重建，
    // 旧句柄全失效，而调用方还记着"我刚才选的是谁"。
    //
    // **guid 优先，名字兜底**。顺序不能反：
    //   * 名字找的是"第一个同名的"，场景里有两个 `Crate` 时会跳到**另一个** ——
    //     这是 H5 的现网 bug（撤销一次就选错），而它看起来完全正常。
    //   * guid 是随机 64 位，重建后仍然唯一（只要快照里存了 guid）。
    //   * 老场景 / 手写文件没有 guid → 退化成名字，保持旧行为（不变得更糟）。
    //
    // guid == kInvalidGuid 且 name 为空 → 返回 Entity{}（没有线索，接不回来）。
    ecs::Entity reconnectByGuidOrName(ecs::EntityGuid guid, const std::string& name);

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
    // 相机数组：0 号恒存在（默认相机），camera() = m_cameras[0]。
    // 用 vector 而不是"一个相机 + 几个额外相机"，是为了让"下标即相机 id"
    // 这件事只有一处定义（Renderer::setViewCameraIndex 直接用下标）。
    std::vector<Camera> m_cameras{1};
    PostVolumeSet m_postVolumes;
    PhysicsSettings m_physics;
    ecs::Entity m_lightEntity;
    // 脚本世界黑板（S2）：进存档
    std::unordered_map<std::string, double> m_scriptVars;

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
