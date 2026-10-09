#include "ecs/World.h"

#include <stdexcept>

namespace ecs {

Entity World::create(const std::string& name) {
    EntityId id;
    if (!m_free.empty()) {
        id = m_free.back();
        m_free.pop_back();
    } else {
        id = static_cast<EntityId>(m_records.size());
        if (id == kInvalidEntity) {
            throw std::runtime_error("ecs::World entity id overflow");
        }
        m_records.emplace_back();
    }

    Record& r = m_records[id];
    r.alive = true;
    r.name = name;
    ++m_aliveCount;

    return Entity{id, r.generation};
}

void World::destroy(Entity e) {
    if (!alive(e)) return;

    // 先摘掉该实体的全部组件（池会做 swap-and-pop 保持连续）
    for (auto& kv : m_pools) {
        kv.second->remove(e.id);
    }

    Record& r = m_records[e.id];
    r.alive = false;
    r.name.clear();
    ++r.generation;  // 代际自增 → 旧句柄全部失效
    m_free.push_back(e.id);
    --m_aliveCount;
}

bool World::alive(Entity e) const {
    return e.id < m_records.size() && m_records[e.id].alive &&
           m_records[e.id].generation == e.generation;
}

Entity World::handle(EntityId id) const {
    if (id >= m_records.size()) return Entity{};
    return Entity{id, m_records[id].generation};
}

const std::string* World::name(Entity e) const {
    if (!alive(e)) return nullptr;
    return &m_records[e.id].name;
}

void World::setName(Entity e, const std::string& n) {
    if (!alive(e)) return;
    m_records[e.id].name = n;
    if (auto* nc = get<NameComponent>(e)) {
        nc->name = n;
    }
}

void World::clear() {
    m_pools.clear();
    m_records.clear();
    m_free.clear();
    m_aliveCount = 0;
}

} // namespace ecs
