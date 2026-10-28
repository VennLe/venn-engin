#include "ecs/World.h"

#include <chrono>
#include <random>
#include <stdexcept>

namespace ecs {

namespace {

// 进程级 RNG —— **不能放进 World**。
// World 各持一份的话，"清空再加载"会让同一进程里两批实体从同一状态重放，
// guid 交集不为空 → 场景合并时静默串号（G16 验收 3 要抓的正是这个）。
std::mt19937_64& guidRng() {
    static std::mt19937_64 rng = [] {
        std::random_device rd;
        // `random_device` 在部分实现上只给 32 位且可能恒定（MinGW 老版本尤其），
        // 所以再掺一次时间；seed_seq 会把整个区间打散。
        const auto ticks = static_cast<unsigned>(
            std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFFll);
        std::seed_seq seq{rd(), rd(), rd(), rd(), ticks};
        return std::mt19937_64(seq);
    }();
    return rng;
}

} // namespace

EntityGuid World::allocateGuid() {
    // 64 位均匀随机：100 万实体下碰撞概率 2.7e-8，且没有隐藏假设
    // （"时间戳 + pid + 计数"会撞：时钟回拨、pid 只 15 位、计数重启归零）。
    for (;;) {
        EntityGuid g = guidRng()();
        if (g != kInvalidGuid) return g;  // 0 保留给"无身份"
    }
}

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
    // ⚠ 必须在 emplace_back 之后（见 World.h 的注释）。
    // 复用槽位时 r.guid 已在 destroy 里清成 0；这里仍显式防御一次 ——
    // 万一将来有人在 destroy 之外清了 alive，残留的旧键会变成幽灵。
    if (r.guid != kInvalidGuid) m_byGuid.erase(r.guid);
    r.guid = allocateGuid();
    m_byGuid[r.guid] = id;
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
    // ⚠ 顺序死板：**先用旧 guid erase，再清零**。写反等于"拿 0 去 erase"，
    //   旧键永远留在表里 → find(旧guid) 命中一条 alive=false 的记录，
    //   而 guid 又已经发给了新实体 → 两个实体抢同一个 guid。
    if (r.guid != kInvalidGuid) {
        m_byGuid.erase(r.guid);
        r.guid = kInvalidGuid;
    }
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

EntityGuid World::guid(Entity e) const {
    if (!alive(e)) return kInvalidGuid;
    return m_records[e.id].guid;
}

Entity World::find(EntityGuid g) const {
    if (g == kInvalidGuid) return Entity{};
    const auto it = m_byGuid.find(g);
    if (it == m_byGuid.end()) return Entity{};
    const EntityId id = it->second;
    // 防御：表里有键、记录已死 → 断言不变量被破坏，宁可返回空也不返回半死句柄。
    // ⚠ 这里**不能**直接 `return handle(id)` —— handle 会给已死实体返回
    //   `valid()==true` 的句柄（id 合法、只是 alive() 为假），而调用方普遍写
    //   `if (p.valid()) { ... }`，那样就会往下用一个死实体。
    if (id >= m_records.size() || !m_records[id].alive) return Entity{};
    return Entity{id, m_records[id].generation};
}

bool World::setGuid(Entity e, EntityGuid g) {
    if (!alive(e) || g == kInvalidGuid) return false;
    const auto it = m_byGuid.find(g);
    if (it != m_byGuid.end() && it->second != e.id) return false;  // 他人占用

    Record& r = m_records[e.id];
    // ⚠ 先摘旧键再插新键。忘了这行的后果：create() 先给的临时 guid 永远留在表里
    //   → 一个"幽灵实体"能被 find() 命中，而且要到 clear() 才消失。
    if (r.guid != kInvalidGuid) m_byGuid.erase(r.guid);
    r.guid = g;
    m_byGuid[g] = e.id;
    return true;
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
    m_byGuid.clear();
    m_aliveCount = 0;
}

} // namespace ecs
