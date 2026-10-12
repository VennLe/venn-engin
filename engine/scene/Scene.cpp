#include "scene/Scene.h"

#include "assets/Mesh.h"

#include <algorithm>

namespace scene {

ecs::Entity Scene::createObject(const std::string& name) {
    ecs::Entity e = m_world.create(name);
    m_world.add<ecs::NameComponent>(e, ecs::NameComponent{name});
    m_world.add<ecs::TransformComponent>(e);
    m_world.add<ecs::HierarchyComponent>(e);
    m_world.add<ecs::VisibilityComponent>(e);
    return e;
}

void Scene::setParent(ecs::Entity child, ecs::Entity parent) {
    if (!m_world.alive(child)) return;

    auto* ch = m_world.get<ecs::HierarchyComponent>(child);
    if (!ch) ch = &m_world.add<ecs::HierarchyComponent>(child);

    // 先从原父节点的 children 列表里摘除
    if (ch->parent != ecs::kInvalidEntity) {
        ecs::Entity oldParent = m_world.handle(ch->parent);
        if (auto* op = m_world.get<ecs::HierarchyComponent>(oldParent)) {
            auto& kids = op->children;
            kids.erase(std::remove(kids.begin(), kids.end(), child.id),
                       kids.end());
        }
    }

    ch->parent = parent.id;

    if (m_world.alive(parent)) {
        auto* ph = m_world.get<ecs::HierarchyComponent>(parent);
        if (!ph) ph = &m_world.add<ecs::HierarchyComponent>(parent);
        ph->children.push_back(child.id);
    }
}

glm::mat4 Scene::worldMatrix(ecs::Entity e) const {
    const auto* t = m_world.get<ecs::TransformComponent>(e);
    if (!t) return glm::mat4(1.0f);

    glm::mat4 m = t->localMatrix();
    const auto* h = m_world.get<ecs::HierarchyComponent>(e);
    if (!h || h->parent == ecs::kInvalidEntity) return m;

    // 沿父链向上累乘；限制深度 64 层防止层级数据成环时死循环
    ecs::EntityId p = h->parent;
    for (int depth = 0; depth < 64 && p != ecs::kInvalidEntity; ++depth) {
        ecs::Entity pe = m_world.handle(p);
        const auto* pt = m_world.get<ecs::TransformComponent>(pe);
        if (!pt) break;
        m = pt->localMatrix() * m;
        const auto* ph = m_world.get<ecs::HierarchyComponent>(pe);
        p = ph ? ph->parent : ecs::kInvalidEntity;
    }
    return m;
}

void Scene::update(float dt) {
    m_camera.update(dt);
}

Light& Scene::light() {
    auto* p = m_world.get<ecs::DirectionalLightComponent>(m_lightEntity);
    if (!p) {
        if (!m_world.alive(m_lightEntity)) {
            m_lightEntity = m_world.create("DirectionalLight");
            m_world.add<ecs::NameComponent>(
                m_lightEntity, ecs::NameComponent{"DirectionalLight"});
        }
        p = &m_world.add<ecs::DirectionalLightComponent>(m_lightEntity);
    }
    return *p;
}

const Light& Scene::light() const {
    const auto* p = m_world.get<ecs::DirectionalLightComponent>(m_lightEntity);
    if (!p) {
        // const 路径下不应创建，返回一个静态默认光照
        static const Light kDefault{};
        return kDefault;
    }
    return *p;
}

// ---------------------------------------------------------------- 局部光源

void Scene::collectLights(std::vector<LightInstance>& out) const {
    out.clear();

    // 点光
    m_world.each<ecs::PointLightComponent>(
        [&](ecs::Entity e, const ecs::PointLightComponent& pl) {
            if (!pl.enabled) return;
            const glm::vec3 pos = glm::vec3(worldMatrix(e)[3]);
            out.push_back(makePointLight(pos, pl));
        });

    // 射灯（放在后面：着色器按顺序遍历，点光通常更多、
    // 分支更简单，早期退出的机会更大）
    m_world.each<ecs::SpotLightComponent>(
        [&](ecs::Entity e, const ecs::SpotLightComponent& sl) {
            if (!sl.enabled) return;
            const glm::vec3 pos = glm::vec3(worldMatrix(e)[3]);
            out.push_back(makeSpotLight(pos, sl));
        });

    // 实体级方向光（编辑器可以摆多盏，作为"补光"用 —— 无阴影、无衰减）。
    // 场景级太阳（m_lightEntity）不在这里：它走 ShadowPass + sun UBO
    // 的独立通道，混进局部光列表会照两遍。
    m_world.each<ecs::DirectionalLightComponent>(
        [&](ecs::Entity e, const ecs::DirectionalLightComponent& dl) {
            if (e == m_lightEntity) return;
            if (dl.intensity <= 0.0f) return;
            const glm::vec3 pos = glm::vec3(worldMatrix(e)[3]);
            out.push_back(makeDirectionalLight(pos, dl));
        });
}

Scene::LightStats Scene::lightStats() const {
    LightStats st;
    m_world.each<ecs::PointLightComponent>(
        [&](ecs::Entity, const ecs::PointLightComponent& pl) {
            if (pl.enabled)
                ++st.pointLights;
            else
                ++st.disabled;
        });
    m_world.each<ecs::SpotLightComponent>(
        [&](ecs::Entity, const ecs::SpotLightComponent& sl) {
            if (sl.enabled)
                ++st.spotLights;
            else
                ++st.disabled;
        });
    return st;
}

ecs::Entity Scene::find(const std::string& name) {
    ecs::Entity found{};
    m_world.each<ecs::NameComponent>(
        [&](ecs::Entity e, ecs::NameComponent& nc) {
            if (!found.valid() && nc.name == name) found = e;
        });
    return found;
}

void Scene::clear() {
    m_world.clear();
    // 世界清空后旧句柄可能"碰巧"仍通过 alive 检查（generation 归零），
    // 必须显式重置，避免 light() 误把方向光挂到别的实体上
    m_lightEntity = ecs::Entity{};

    // 空间索引一并作废
    m_bvh.clear();
    m_indexedEntities.clear();
}

// ---------------------------------------------------------------- 空间索引

bool Scene::meshLocalBounds(const assets::Mesh& mesh, BvhAabb& out) const {
    if (mesh.hasBounds()) {
        // 快路径：Mesh 在上传顶点时就把局部 AABB 算好了
        out.min = mesh.boundsMin();
        out.max = mesh.boundsMax();
        return true;
    }
    // 慢路径：还没上传 / 上传时没有顶点的网格，退回扫一遍 CPU 顶点。
    // 拾取必须能选中"刚导入、还没上传"的物体，所以不能只认 hasBounds。
    const auto& verts = mesh.vertices();
    if (verts.empty()) return false;
    out.min = out.max = verts[0].pos;
    for (const auto& v : verts) out.expand(v.pos);
    return true;
}

Bvh& Scene::spatialIndex() {
    m_indexItems.clear();

    forEachRenderable([&](ecs::Entity e, ecs::MeshComponent& mc,
                          ecs::MaterialComponent&) {
        if (!mc.mesh) return;
        BvhAabb local;
        if (!meshLocalBounds(*mc.mesh, local)) return;

        Bvh::Item item;
        item.entity = e;
        item.bounds = transformAabb(worldMatrix(e), local);
        m_indexItems.push_back(item);
    });

    // 实体集合（含顺序）没变 → 只重新拟合包围盒；变了 → 整棵重建。
    // 顺序也要比：ECS 的存储重排会让"叶子 → items 下标"的对应关系失效，
    // 这时 refit 会把包围盒填到错的实体上，必须重建。
    bool sameSet = m_indexItems.size() == m_indexedEntities.size();
    if (sameSet) {
        for (size_t i = 0; i < m_indexItems.size(); ++i) {
            if (m_indexItems[i].entity != m_indexedEntities[i]) {
                sameSet = false;
                break;
            }
        }
    }

    if (!sameSet) {
        m_bvh.build(m_indexItems);
        m_indexedEntities.clear();
        m_indexedEntities.reserve(m_indexItems.size());
        for (const Bvh::Item& it : m_indexItems)
            m_indexedEntities.push_back(it.entity);
    } else {
        m_indexBounds.clear();
        m_indexBounds.reserve(m_indexItems.size());
        for (const Bvh::Item& it : m_indexItems)
            m_indexBounds.push_back(it.bounds);
        m_bvh.refit(m_indexBounds);
    }
    return m_bvh;
}

Scene::SpatialStats Scene::spatialStats() const {
    SpatialStats st;
    st.nodes = m_bvh.nodeCount();
    st.entities = m_bvh.leafCount();
    st.depth = m_bvh.depth();
    return st;
}

} // namespace scene
