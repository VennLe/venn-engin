#pragma once
// ============================================================
// ecs/Component —— 组件池（稀疏集 sparse set）
//
// 为什么不用 unordered_map<EntityId, T>：
//   哈希表节点在内存里是散开的，遍历时 cache miss 严重。
//   稀疏集把组件值密集存在 m_data 里，遍历只顺序读 m_data，
//   对 CPU 缓存极其友好 —— 这正是 ECS 的性能收益来源。
//
// 结构：
//   m_sparse[entityId] -> 该实体在 dense 数组中的下标（kNone = 无）
//   m_dense[i]         -> 下标 i 对应的 entityId
//   m_data[i]          -> 下标 i 的组件值
// 删除用 swap-and-pop：末尾元素填补空洞，保证 m_data 始终连续。
// ============================================================

#include "ecs/Entity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace ecs {

// 类型擦除接口：让 World 能用统一容器存放各种组件池
class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual void remove(EntityId id) = 0;
    virtual bool contains(EntityId id) const = 0;
    virtual size_t size() const = 0;
};

template <typename T>
class ComponentPool final : public IComponentPool {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    // 不存在则创建，已存在则覆盖
    T& add(EntityId id, T value = T{}) {
        ensure(id);
        uint32_t idx = m_sparse[id];
        if (idx != kNone) {
            m_data[idx] = std::move(value);
            return m_data[idx];
        }
        m_sparse[id] = static_cast<uint32_t>(m_data.size());
        m_dense.push_back(id);
        m_data.push_back(std::move(value));
        return m_data.back();
    }

    T* get(EntityId id) {
        if (id >= m_sparse.size()) return nullptr;
        uint32_t idx = m_sparse[id];
        return idx == kNone ? nullptr : &m_data[idx];
    }

    const T* get(EntityId id) const {
        if (id >= m_sparse.size()) return nullptr;
        uint32_t idx = m_sparse[id];
        return idx == kNone ? nullptr : &m_data[idx];
    }

    bool contains(EntityId id) const override {
        return id < m_sparse.size() && m_sparse[id] != kNone;
    }

    void remove(EntityId id) override {
        if (id >= m_sparse.size()) return;
        uint32_t idx = m_sparse[id];
        if (idx == kNone) return;

        const uint32_t last = static_cast<uint32_t>(m_data.size() - 1);
        if (idx != last) {
            // 末尾元素填补空洞，并修正它在 sparse 中的下标
            m_data[idx] = std::move(m_data[last]);
            const EntityId moved = m_dense[last];
            m_dense[idx] = moved;
            m_sparse[moved] = idx;
        }
        m_data.pop_back();
        m_dense.pop_back();
        m_sparse[id] = kNone;
    }

    size_t size() const override { return m_data.size(); }

    void clear() {
        m_sparse.clear();
        m_dense.clear();
        m_data.clear();
    }

    // ---- 供 World 遍历使用 ----
    std::vector<T>& data() { return m_data; }
    const std::vector<T>& data() const { return m_data; }
    const std::vector<EntityId>& entities() const { return m_dense; }

private:
    void ensure(EntityId id) {
        if (id >= m_sparse.size()) m_sparse.resize(id + 1, kNone);
    }

    std::vector<uint32_t> m_sparse;  // entityId -> dense 下标
    std::vector<EntityId> m_dense;   // dense 下标 -> entityId
    std::vector<T> m_data;           // 组件值（连续存储）
};

} // namespace ecs
