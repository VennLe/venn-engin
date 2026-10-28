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

    // ---------------- 稳定身份（G16） ----------------
    // 与 EntityId 无关的持久身份。存档 / 跨文件引用一律用它，详见 Entity.h。
    // 不变量：同一时刻 `m_byGuid[guid(e)] == e.id`，且 `guid(e) == 0` 表示"无身份"。
    EntityGuid guid(Entity e) const;             // 无效实体 / 无 guid → kInvalidGuid
    Entity     find(EntityGuid g) const;         // 找不到 → Entity{}（valid()==false）
    bool       setGuid(Entity e, EntityGuid g);  // 已被别的存活实体占用 → false
    EntityGuid allocateGuid();                   // 由 create() 内部调用，也供自检单测

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
        // ⚠ 必须在 `create()` 的 `m_records.emplace_back()` **之后**赋值 ——
        //   emplace_back 造出的是带默认 guid=0 的 Record，顺序写反会读到 0。
        EntityGuid guid = kInvalidGuid;
    };

    // 只取已存在的组件池，不存在返回 nullptr（const 版遍历用）
    template <typename T>
    const ComponentPool<T>* poolOrNull() const;

    std::vector<Record> m_records;
    std::vector<EntityId> m_free;  // 可复用的槽位
    // guid → EntityId 的反查表。**只增删，不重建**：重建会让 guid 的顺序影响
    // find() 的结果，而 guid 是随机值，顺序不稳定 = 偶发不一致。
    std::unordered_map<EntityGuid, EntityId> m_byGuid;
    size_t m_aliveCount = 0;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_pools;
};

} // namespace ecs

#include "ecs/World.inl"
