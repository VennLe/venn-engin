#include "scene/Bvh.h"

#include <algorithm>

namespace scene {

namespace {

// [begin, end) 区间（按 order 间接寻址）的并集包围盒。
// 空区间不会被调用 —— 上层保证叶子至少有 1 个元素。
BvhAabb boundsOf(const std::vector<Bvh::Item>& items,
                 const std::vector<uint32_t>& order, uint32_t begin,
                 uint32_t end) {
    BvhAabb b = items[order[begin]].bounds;
    for (uint32_t i = begin + 1; i < end; ++i) b.expand(items[order[i]].bounds);
    return b;
}

} // namespace

BvhAabb transformAabb(const glm::mat4& m, const BvhAabb& local) {
    BvhAabb out;
    // 8 个角点
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 c((i & 1) ? local.max.x : local.min.x,
                          (i & 2) ? local.max.y : local.min.y,
                          (i & 4) ? local.max.z : local.min.z);
        const glm::vec3 p = glm::vec3(m * glm::vec4(c, 1.0f));
        if (i == 0)
            out.min = out.max = p;
        else
            out.expand(p);
    }
    return out;
}

void Bvh::clear() {
    m_nodes.clear();
    m_leafEntities.clear();
    m_leafBounds.clear();
    m_leafItemIndex.clear();
}

int32_t Bvh::buildRange(const std::vector<Item>& items,
                        std::vector<uint32_t>& order, uint32_t begin,
                        uint32_t end) {
    // 先占位：父节点下标一定小于两个子节点（先 push_back 再递归），
    // refit 的"倒序自底向上"依赖这个不变量。
    const int32_t self = static_cast<int32_t>(m_nodes.size());
    m_nodes.push_back(Node{});

    const uint32_t n = end - begin;

    // ---- 叶子 ----
    if (n <= kLeafSize) {
        Node& nd = m_nodes[self];
        nd.bounds = boundsOf(items, order, begin, end);
        nd.first = static_cast<uint32_t>(m_leafEntities.size());
        nd.count = n;
        for (uint32_t i = begin; i < end; ++i) {
            // ⚠ 存的是 order[i]（**原始**下标），不是 i。
            //   order 会被 nth_element 重排，i 只是重排后的位置；而
            //   refit() 拿到的包围盒数组是按**原始顺序**给的。存错了就会
            //   把别的实体的包围盒拟合到叶子上 —— 表现为"可见物体被判到
            //   视锥外"，直接漏画（这个 bug 真实发生过，见 Bvh.h 顶部说明）。
            const uint32_t src = order[i];
            m_leafEntities.push_back(items[src].entity);
            m_leafBounds.push_back(items[src].bounds);
            m_leafItemIndex.push_back(src);
        }
        return self;  // left 保持 -1 → 是叶子
    }

    // ---- 内部节点：沿最长轴按包围盒中心做中位数切分 ----
    // 必须在 nth_element **之前**取整段的并集（切分后顺序会变）。
    const glm::vec3 ext = boundsOf(items, order, begin, end).extent();
    const int axis = (ext.x > ext.y) ? (ext.x > ext.z ? 0 : 2)
                                     : (ext.y > ext.z ? 1 : 2);

    const uint32_t mid = begin + n / 2;
    // 只重排**下标**，不动 items —— 这样 items 始终保持调用方给的顺序，
    // order[i] 天然就是"原始下标"。
    std::nth_element(order.begin() + begin, order.begin() + mid,
                     order.begin() + end,
                     [&items, axis](uint32_t a, uint32_t c) {
                         return items[a].bounds.center()[axis] <
                                items[c].bounds.center()[axis];
                     });

    const int32_t l = buildRange(items, order, begin, mid);
    const int32_t r = buildRange(items, order, mid, end);

    Node& nd = m_nodes[self];  // 递归里可能 reallocate，必须重新取引用
    nd.bounds = m_nodes[l].bounds;
    nd.bounds.expand(m_nodes[r].bounds);
    nd.left = l;
    nd.right = r;
    return self;
}

void Bvh::build(const std::vector<Item>& items) {
    clear();
    if (items.empty()) return;

    // 只建一份下标表并就地重排它；items 只读。
    std::vector<uint32_t> order(items.size());
    for (uint32_t i = 0; i < items.size(); ++i) order[i] = i;

    m_nodes.reserve(items.size() * 2);
    m_leafEntities.reserve(items.size());
    m_leafBounds.reserve(items.size());
    m_leafItemIndex.reserve(items.size());

    buildRange(items, order, 0, static_cast<uint32_t>(items.size()));
}

void Bvh::refit(const std::vector<BvhAabb>& bounds) {
    if (m_nodes.empty()) return;
    if (bounds.size() != m_leafItemIndex.size()) return;  // 集合变了，调用方该 build

    // 1) 叶子槽位的新包围盒：按 build 时记下的 item 下标回填
    for (size_t k = 0; k < m_leafItemIndex.size(); ++k)
        m_leafBounds[k] = bounds[m_leafItemIndex[k]];

    // 2) 自底向上重算。
    //    建树时"父节点先于子节点 push_back" → 父下标恒小于子下标，
    //    所以**倒序遍历**就等价于后序遍历：轮到某个节点时，它的子节点
    //    一定已经算完了。
    for (size_t i = m_nodes.size(); i-- > 0;) {
        Node& nd = m_nodes[i];
        if (nd.isLeaf()) {
            BvhAabb b = m_leafBounds[nd.first];
            for (uint32_t k = 1; k < nd.count; ++k)
                b.expand(m_leafBounds[nd.first + k]);
            nd.bounds = b;
        } else {
            BvhAabb b = m_nodes[nd.left].bounds;
            b.expand(m_nodes[nd.right].bounds);
            nd.bounds = b;
        }
    }
}

size_t Bvh::depth() const {
    if (m_nodes.empty()) return 0;
    // 迭代 DFS：(节点下标, 深度)
    int32_t stack[kMaxStack];
    uint8_t stackD[kMaxStack];
    size_t maxD = 0;
    int sp = 0;
    stack[sp] = 0;
    stackD[sp] = 1;
    ++sp;
    while (sp > 0) {
        --sp;
        const int32_t ni = stack[sp];
        const size_t d = stackD[sp];
        maxD = std::max(maxD, d);
        const Node& nd = m_nodes[ni];
        if (nd.isLeaf()) continue;
        if (sp + 2 > kMaxStack) continue;
        stack[sp] = nd.left;
        stackD[sp] = static_cast<uint8_t>(d + 1);
        ++sp;
        stack[sp] = nd.right;
        stackD[sp] = static_cast<uint8_t>(d + 1);
        ++sp;
    }
    return maxD;
}

} // namespace scene
