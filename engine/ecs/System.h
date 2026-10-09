#pragma once
// ============================================================
// ecs/System —— 系统基类与调度器
//
// 系统只写"逻辑"，数据都在 World 的组件里。同一个 World 可以由
// 不同系统按固定顺序处理（例如 AnimationSystem → HierarchySystem），
// 取代原先 GameObject::update 的虚函数调用。
// ============================================================

#include "ecs/World.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace ecs {

class System {
public:
    virtual ~System() = default;

    virtual const char* name() const { return "System"; }

    // 每帧调用；dt 为秒
    virtual void update(World& world, float dt) = 0;
};

class SystemManager {
public:
    SystemManager() = default;

    template <typename T, typename... Args>
    T& add(Args&&... args) {
        auto owned = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *owned;
        m_systems.push_back(std::move(owned));
        return ref;
    }

    template <typename T>
    T* find() {
        for (auto& s : m_systems) {
            if (auto* p = dynamic_cast<T*>(s.get())) return p;
        }
        return nullptr;
    }

    // 按注册顺序依次执行
    void update(World& world, float dt) {
        for (auto& s : m_systems) {
            s->update(world, dt);
        }
    }

    void clear() { m_systems.clear(); }

    size_t count() const { return m_systems.size(); }

    const std::vector<std::unique_ptr<System>>& systems() const {
        return m_systems;
    }

private:
    std::vector<std::unique_ptr<System>> m_systems;
};

} // namespace ecs
