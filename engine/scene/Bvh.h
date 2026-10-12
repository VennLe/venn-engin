#pragma once
// ============================================================
// scene/Bvh —— 场景空间加速结构（AABB 层次包围盒 / 二叉 BVH）
//
// 为什么需要它
// ------------------------------------------------------------
// 在这之前，剔除与拾取都是"每个实体做一次 AABB 测试"的**线性扫描**：
//   * 视锥剔除：N 个实体 × 6 个平面，逐个测；
//   * 编辑器射线拾取：N 个实体 × 先算世界 AABB（8 个角点过世界矩阵）
//     × 再测射线 —— 场景一上量，"点一下选中物体"就明显发涩。
//
// BVH 把这些世界 AABB 组织成一棵树：视锥查询能**整棵子树剪掉**
// （节点包围盒在视锥外 → 里面所有东西都不用看），射线查询变成
// O(log N)。这是"场景上量"之后最先该补的那块基础设施。
//
// 结构
// ------------------------------------------------------------
//   * 叶子：若干个 ecs::Entity + 它们的世界空间 AABB（kLeafSize 个一组）；
//   * 内部节点：两个子节点的并集包围盒。
//   * 节点存在一个**扁平数组**里（0 号是根），遍历用显式栈 ——
//     不递归，避免深树爆栈，也让遍历对 CPU 缓存更友好。
//
// 与渲染 / 编辑器解耦
// ------------------------------------------------------------
// 引擎层不该反向依赖 render::Frustum 或 editor::Ray，所以查询接口做成
// "泛型剪枝谓词"：
//   * 视锥查询只要求传入一个提供
//       bool intersects(const glm::vec3& lo, const glm::vec3& hi) const
//     的对象（render::Frustum 恰好就是这个签名，直接传即可）；
//   * 射线用本文件自己的 BvhRay，拒绝沿用 editor::Ray。
// 于是 Bvh 只依赖 glm + ecs::Entity，可以放在最底层的 scene 里。
//
// 生命周期
// ------------------------------------------------------------
//   * 实体**集合**变了（增删物体 / 换网格）→ build()：重建拓扑 + 包围盒；
//   * 只是**变换**变了 → refit()：拓扑不动，自底向上重算包围盒（O(N)，
//     无排序 / 无分配）。
// 每帧"变换在动"是常态，所以 refit 是热路径，必须便宜。
//
// 已知取舍
// ------------------------------------------------------------
//   * 切分用"最长轴 + 中心中位数"（nth_element），不是完整 SAH。
//     完整 SAH 建树更慢，而本引擎的场景规模（几百~几万）下差别不大，
//     交互编辑时"改一下物体就重建"的响应速度更重要。
//   * 不做增量插入 / 删除：拓扑变化整棵重建。简单且没有隐藏状态，
//     对轻量引擎来说这笔账划算。
// ============================================================

#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace scene {

// 世界空间轴对齐包围盒。
// （编辑器里那份 editor::Aabb 是历史遗留的同义类型，语义一致。）
struct BvhAabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extent() const { return max - min; }

    void expand(const glm::vec3& p) {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void expand(const BvhAabb& b) {
        min = glm::min(min, b.min);
        max = glm::max(max, b.max);
    }
};

// 射线（世界空间）。dir 不要求归一化，但**要求归一化**才能让 distance
// 直接当"米"用 —— raycast 的返回值按此约定。
struct BvhRay {
    glm::vec3 origin{0.0f};
    glm::vec3 dir{0.0f, 0.0f, -1.0f};
};

struct BvhHit {
    bool hit = false;
    ecs::Entity entity{};
    float distance = 0.0f;  // origin 到命中点的距离（dir 为单位长度时即米）
};

// 把局部 AABB 用变换矩阵变成世界 AABB：8 个角点各过一遍矩阵，再取包围。
//
// 注意这是**保守**做法：AABB 对旋转不封闭，转过的盒子包围盒只会变大。
// 对本用途（剔除 / 拾取）正好 —— 宁可多画一个、多测一次，也不能误剔。
BvhAabb transformAabb(const glm::mat4& m, const BvhAabb& local);

class Bvh {
public:
    struct Item {
        ecs::Entity entity{};
        BvhAabb bounds;
    };

    // 重建整棵树（拓扑 + 包围盒）。items 只读，不会被改动。
    // 内部只重排一张**下标表**，并记下"叶子槽位 ↔ 原始下标"的对应，
    // 所以 refit() 要求的"同序"就是这里传入 items 的顺序。
    void build(const std::vector<Item>& items);

    // 拓扑不变、只有包围盒变了：按 build() 时**同样的顺序**传入新包围盒，
    // 自底向上重算整棵树。比 build() 便宜得多（无排序、无新分配）。
    // 传入的 bounds.size() 必须等于 build() 时的 items.size()。
    void refit(const std::vector<BvhAabb>& bounds);

    void clear();

    bool empty() const { return m_nodes.empty(); }
    size_t nodeCount() const { return m_nodes.size(); }
    size_t leafCount() const { return m_leafEntities.size(); }
    // 树高（根 = 1）。用于验证"中位数切分确实把树压到 O(log N)"。
    size_t depth() const;

    const BvhAabb& rootBounds() const { return m_nodes[0].bounds; }

    // ------------------------------------------------------------
    // 遥测：上一次查询实际访过多少节点 / 做过多少次叶子级 AABB 测试。
    //
    // 这些计数是**剔除"有没有真的剪枝"的唯一硬证据** —— 视锥查询如果
    // 退化成"把整棵树走完"，节点访问数会逼近 nodeCount()，一眼就能看出
    // 树建歪了。渲染器的 Stats 面板与验证脚本都读它。
    // ------------------------------------------------------------
    uint32_t lastQueryNodeVisits() const { return m_nodeVisits; }
    uint32_t lastQueryLeafTests() const { return m_queryLeafTests; }
    uint32_t lastRayNodeVisits() const { return m_rayNodeVisits; }
    uint32_t lastRayLeafTests() const { return m_rayLeafTests; }

    // ------------------------------------------------------------
    // 视锥 / 任意保守体积查询
    //
    // Volume 需提供 bool intersects(const glm::vec3& lo,
    //                               const glm::vec3& hi) const，
    // **且必须是保守的**（不能把可见的判成不可见），否则会漏画。
    // 命中结果**追加**到 out（不清空 out，方便一次遍历多个体积）。
    // ------------------------------------------------------------
    template <typename Volume>
    void query(const Volume& vol, std::vector<ecs::Entity>& out) const {
        m_nodeVisits = 0;
        m_queryLeafTests = 0;
        if (m_nodes.empty()) return;

        int32_t stack[kMaxStack];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const Node& nd = m_nodes[stack[--sp]];
            ++m_nodeVisits;
            if (!vol.intersects(nd.bounds.min, nd.bounds.max)) continue;  // 剪枝

            if (nd.isLeaf()) {
                // 叶子里的实体**逐个再测一次**。叶子的包围盒是成员的并集，
                // 并集相交不代表每个成员都相交 —— 不再测一遍的话会把同叶
                // 的邻居一起放进结果（保守，不漏画，但白画）。
                // 这一步让 query 的结果与"逐个实体直接测"**完全一致**，
                // 于是"BVH 结果 == 暴力结果"可以当成硬断言来验证。
                for (uint32_t i = 0; i < nd.count; ++i) {
                    ++m_queryLeafTests;
                    const BvhAabb& b = m_leafBounds[nd.first + i];
                    if (!vol.intersects(b.min, b.max)) continue;
                    out.push_back(m_leafEntities[nd.first + i]);
                }
                continue;
            }
            // DFS 栈的峰值深度就是树高（每层最多多压 1 个），正常不会溢出；
            // 这里仍然守一手，宁可有极端输入时少画一个节点，也不要越界写。
            if (sp + 2 > kMaxStack) continue;
            stack[sp++] = nd.left;
            stack[sp++] = nd.right;
        }
    }

    // 射线查询：返回**最近**命中的叶子（AABB 级，不做三角形求交）。
    BvhHit raycast(const BvhRay& r, float tMax = 3.4e38f) const {
        return raycastFiltered(r, [](ecs::Entity) { return true; }, tMax);
    }

    // 带过滤的射线查询。accept(entity) 返回 false 的叶子既不参与竞争，
    // 也不会提前终止遍历 —— 编辑器用它跳过"上锁"的实体（自带地面是参照物，
    // 不该被点选），语义与老版 pick() 一致。
    template <typename LeafFilter>
    BvhHit raycastFiltered(const BvhRay& r, const LeafFilter& accept,
                           float tMax = 3.4e38f) const {
        BvhHit best;
        m_rayNodeVisits = 0;
        m_rayLeafTests = 0;
        if (m_nodes.empty()) return best;

        // 分量恰好为 0 会让 1/dir 变成 inf，而 (min-o)*inf 在 o==min 时
        // 是 NaN，NaN 会污染 slab 测试的比较（glm::min/max 遇到 NaN 的
        // 行为不可靠）。把 0 顶开一个极小量即可，对结果没有实际影响。
        glm::vec3 d = r.dir;
        for (int i = 0; i < 3; ++i)
            if (d[i] == 0.0f) d[i] = 1e-20f;
        const glm::vec3 inv = 1.0f / d;

        float bestT = tMax;
        int32_t stack[kMaxStack];
        float stackT[kMaxStack];
        int sp = 0;
        stack[sp] = 0;
        stackT[sp] = 0.0f;
        ++sp;

        while (sp > 0) {
            --sp;
            const int32_t ni = stack[sp];
            // 节点整体比已知最近命中还远 → 整棵子树跳过
            if (stackT[sp] > bestT) continue;
            const Node& nd = m_nodes[ni];
            ++m_rayNodeVisits;

            float tNode = 0.0f;
            if (!intersectAabb(r.origin, inv, nd.bounds, tNode)) continue;
            if (tNode > bestT) continue;

            if (nd.isLeaf()) {
                for (uint32_t i = 0; i < nd.count; ++i) {
                    ++m_rayLeafTests;
                    const ecs::Entity e = m_leafEntities[nd.first + i];
                    if (!accept(e)) continue;
                    float t = 0.0f;
                    if (!intersectAabb(r.origin, inv, m_leafBounds[nd.first + i],
                                       t))
                        continue;
                    if (t < bestT) {
                        bestT = t;
                        best.hit = true;
                        best.entity = e;
                        best.distance = t;
                    }
                }
                continue;
            }
            if (sp + 2 > kMaxStack) continue;
            stack[sp] = nd.left;
            stackT[sp] = tNode;
            ++sp;
            stack[sp] = nd.right;
            stackT[sp] = tNode;
            ++sp;
        }
        return best;
    }

    // 便利：把整棵树里的实体全倒出来（"关闭剔除"的全量路径用）
    void collectAll(std::vector<ecs::Entity>& out) const {
        out.insert(out.end(), m_leafEntities.begin(), m_leafEntities.end());
    }

    // slab 法射线 / AABB 求交（文件名放在类里，方便连同 Bvh 一起复用）。
    // inv 是 1/dir。命中时 tHit 是进入盒子的参数（起点在盒内则为负）。
    static bool intersectAabb(const glm::vec3& o, const glm::vec3& inv,
                              const BvhAabb& b, float& tHit) {
        const glm::vec3 t0 = (b.min - o) * inv;
        const glm::vec3 t1 = (b.max - o) * inv;
        const glm::vec3 tsmall = glm::min(t0, t1);
        const glm::vec3 tbig = glm::max(t0, t1);
        const float tmin = glm::max(glm::max(tsmall.x, tsmall.y), tsmall.z);
        const float tmax = glm::min(glm::min(tbig.x, tbig.y), tbig.z);
        if (tmax < glm::max(tmin, 0.0f)) return false;
        tHit = tmin;
        return true;
    }

private:
    struct Node {
        BvhAabb bounds;
        int32_t left = -1;    // 内部节点：左子下标（叶子恒为 -1）
        int32_t right = -1;   // 内部节点：右子下标
        uint32_t first = 0;   // 叶子：在 m_leafEntities 里的起始下标
        uint32_t count = 0;   // 叶子：实体个数
        bool isLeaf() const { return left < 0; }
    };

    // 深度优先建树。order 是"原始下标表"，会被 nth_element 重排；
    // items 只读（调用方给什么顺序就是什么顺序）。
    // **父节点下标一定小于子节点下标**（父先 push_back），refit 依赖
    // 这个不变量做"倒序自底向上累加"。
    int32_t buildRange(const std::vector<Item>& items,
                       std::vector<uint32_t>& order, uint32_t begin,
                       uint32_t end);

    // 栈深上限。中位数切分保证树高 ≤ log2(N)+1，64 层足够 2^63 个物体，
    // 留足余量且不至于让每一帧的栈帧太大。
    static constexpr int kMaxStack = 64;
    // 一个叶子最多装几个实体。取 4：再小则节点数暴涨（遍历开销占上风），
    // 再大则叶子内部的线性测试变多。
    static constexpr uint32_t kLeafSize = 4;

    std::vector<Node> m_nodes;
    std::vector<ecs::Entity> m_leafEntities;  // 与 m_leafBounds 平行
    std::vector<BvhAabb> m_leafBounds;
    // 叶子槽位 → build() 时 items 的**原始**下标（refit 用它回填新包围盒）。
    // 记错了就会把别的实体的包围盒拟合到叶子上，直接导致漏画。
    std::vector<uint32_t> m_leafItemIndex;

    // 遥测计数器（const 查询方法里也要写 → mutable）
    mutable uint32_t m_nodeVisits = 0;
    mutable uint32_t m_queryLeafTests = 0;
    mutable uint32_t m_rayNodeVisits = 0;
    mutable uint32_t m_rayLeafTests = 0;
};

} // namespace scene
