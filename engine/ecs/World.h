#pragma once
// ============================================================
// ecs/World —— 实体与组件的容器
//
// * 实体：id + generation 的句柄；销毁后槽位进入空闲表复用
// * 组件：每种类型一个稀疏集池（见 Component.h）
// * 查询：each<T>() / each<T,U>() 批量遍历，组件连续存储
//
// 注意：在 each 回调里增删组件会打乱正在遍历的 dense 数组。
//       需要增删时请先收集到临时 vector，遍历结束后再统一处理。
// ============================================================

#include "ecs/Component.h"
#include "ecs/Components.h"
#include "ecs/Entity.h"

#include <cstddef>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecs {

class World {
public:
    World() = default;
    ~World() = default;

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ---------------- 实体 ----------------
    Entity create(const std::string& name = "Entity");
    void destroy(Entity e);
    bool alive(Entity e) const;

    // 由 id 还原完整句柄（补上当前 generation）
    Entity handle(EntityId id) const;

    const std::string* name(Entity e) const;
    void setName(Entity e, const std::string& n);

    size_t entityCount() const { return m_aliveCount; }

    // ---------------- 组件 ----------------
    template <typename T>
    T& add(Entity e, T value = T{});

    template <typename T>
    T* get(Entity e);

    template <typename T>
    const T* get(Entity e) const;

    template <typename T>
    bool has(Entity e) const {
        return get<T>(e) != nullptr;
    }

    template <typename T>
    void remove(Entity e);

    // 取某种组件的池（不存在则创建）
    template <typename T>
    ComponentPool<T>& pool();

    // ---------------- 查询 ----------------
    // 遍历所有拥有 T 的实体
    template <typename T, typename Fn>
    void each(Fn&& fn);

    // 遍历同时拥有 T 与 U 的实体（以 T 的池为驱动）
    template <typename T, typename U, typename Fn>
    void each(Fn&& fn);

    // const 版本：只读遍历（不会创建空池），回调收到 const T&
    // 供序列化等只读场景使用
    template <typename T, typename Fn>
    void each(Fn&& fn) const;

    template <typename T, typename U, typename Fn>
    void each(Fn&& fn) const;

    void clear();

private:
    struct Record {
        uint32_t generation = 0;
        bool alive = false;
        std::string name;
    };

    // 只取已存在的组件池，不存在返回 nullptr（const 版遍历用）
    template <typename T>
    const ComponentPool<T>* poolOrNull() const;

    std::vector<Record> m_records;
    std::vector<EntityId> m_free;  // 可复用的槽位
    size_t m_aliveCount = 0;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_pools;
};

} // namespace ecs

#include "ecs/World.inl"
