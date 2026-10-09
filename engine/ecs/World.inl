#pragma once
// ============================================================
// ecs/World.inl —— World 的模板实现（由 World.h 末尾包含）
// ============================================================

namespace ecs {

template <typename T>
ComponentPool<T>& World::pool() {
    const std::type_index key(typeid(T));
    auto it = m_pools.find(key);
    if (it == m_pools.end()) {
        auto owned = std::make_unique<ComponentPool<T>>();
        ComponentPool<T>* raw = owned.get();
        m_pools.emplace(key, std::move(owned));
        return *raw;
    }
    return static_cast<ComponentPool<T>&>(*it->second);
}

template <typename T>
T& World::add(Entity e, T value) {
    return pool<T>().add(e.id, std::move(value));
}

template <typename T>
T* World::get(Entity e) {
    if (!alive(e)) return nullptr;
    return pool<T>().get(e.id);
}

template <typename T>
const T* World::get(Entity e) const {
    if (!alive(e)) return nullptr;
    auto it = m_pools.find(std::type_index(typeid(T)));
    if (it == m_pools.end()) return nullptr;
    const auto& p = static_cast<const ComponentPool<T>&>(*it->second);
    return p.get(e.id);
}

template <typename T>
void World::remove(Entity e) {
    if (e.id >= m_records.size()) return;
    auto it = m_pools.find(std::type_index(typeid(T)));
    if (it != m_pools.end()) it->second->remove(e.id);
}

template <typename T, typename Fn>
void World::each(Fn&& fn) {
    auto& p = pool<T>();
    auto& data = p.data();
    const auto& ents = p.entities();
    const size_t n = data.size();
    for (size_t i = 0; i < n; ++i) {
        fn(handle(ents[i]), data[i]);
    }
}

template <typename T, typename U, typename Fn>
void World::each(Fn&& fn) {
    auto& p = pool<T>();
    auto& data = p.data();
    const auto& ents = p.entities();
    const size_t n = data.size();
    for (size_t i = 0; i < n; ++i) {
        Entity e = handle(ents[i]);
        if (U* u = get<U>(e)) {
            fn(e, data[i], *u);
        }
    }
}

// ---------------- const 遍历（只读，不创建池）----------------

template <typename T>
const ComponentPool<T>* World::poolOrNull() const {
    auto it = m_pools.find(std::type_index(typeid(T)));
    if (it == m_pools.end()) return nullptr;
    return &static_cast<const ComponentPool<T>&>(*it->second);
}

template <typename T, typename Fn>
void World::each(Fn&& fn) const {
    const ComponentPool<T>* p = poolOrNull<T>();
    if (!p) return;
    const auto& data = p->data();
    const auto& ents = p->entities();
    const size_t n = data.size();
    for (size_t i = 0; i < n; ++i) {
        fn(handle(ents[i]), static_cast<const T&>(data[i]));
    }
}

template <typename T, typename U, typename Fn>
void World::each(Fn&& fn) const {
    const ComponentPool<T>* p = poolOrNull<T>();
    if (!p) return;
    const auto& data = p->data();
    const auto& ents = p->entities();
    const size_t n = data.size();
    for (size_t i = 0; i < n; ++i) {
        Entity e = handle(ents[i]);
        if (const U* u = get<U>(e)) {
            fn(e, static_cast<const T&>(data[i]), *u);
        }
    }
}

} // namespace ecs
