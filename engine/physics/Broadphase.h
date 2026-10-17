#pragma once
// ============================================================
// physics/Broadphase —— 宽相：把 N² 的候选对压到 O(N log N + K)
//
// 用的是 **Sweep and Prune（SAP，扫描-剪枝）**：
//
//   1. 把所有体按 AABB 的 min.x 排序；
//   2. 线性扫过去。扫到 i 时，只需要往后看那些 min.x <= max.x[i] 的体
//      （一旦 min.x 超过 max.x[i] 就可以停 —— 后面的更不可能重叠）；
//   3. 对每个"x 轴上重叠"的对，再查 y / z 两轴。
//
// 为什么不用均匀网格 / 动态 AABB 树：
//   · 网格要选格子大小，而本引擎的场景尺度跨度极大（1 cm 螺丝到 200 m 地形），
//     一个格子大小不可能都对；
//   · 动态树（Box2D 那套）增量插入/旋转更新写起来是 SAP 的三倍代码量，
//     收益只在大规模动态场景里才明显。
//   本引擎的目标是"学习向、可验证、几百个刚体"，SAP 的性价比最高。
//
// ------------------------------------------------------------
// 关于"每帧重排序"这个取舍
// ------------------------------------------------------------
// 教科书 SAP 维护一个**增量**的端点数组（插入排序，近似 O(N)）。这里
// 每帧直接 std::sort —— 因为：
//   · 增量版的正确性依赖"帧间 AABB 不会跳变"这个前提，而刚体会被
//     外力瞬移（传送 / 出界重置），一破前提就会漏对；
//   · std::sort 在几百个元素上是微秒级，不是瓶颈。
// 代价是丢掉了 SAP 最漂亮的那个性质（时间相关性），写在注释里备查。
//
// ------------------------------------------------------------
// 平面（Shape::Kind::Plane）走另一条路
// ------------------------------------------------------------
// 平面是无限半空间，没有有限的 AABB，塞不进 SAP 的排序（min.x = -inf）。
// 它走**一对多**：每个平面 × 每个可动的体。平面通常只有 1~3 个（地面 +
// 两面墙），这个 N×3 的量级可以忽略。
// ============================================================

#include "physics/RigidBody.h"

#include <cstdint>
#include <vector>

namespace physics {

struct BroadphasePair {
    std::uint32_t a = 0;   // 体索引（< b，保证不重复计数）
    std::uint32_t b = 0;
};

class Broadphase {
public:
    // 重建候选对。bodies 的 aabbMin/aabbMax 必须已经填好。
    // margin 会把 AABB 向外扩一点（"预测性接触"）：让**即将**接触的
    // 一对提前进入窄相，于是接触约束在真正穿透之前就已经存在 —— 这是
    // "高速物体落地不抖"的关键之一。
    void rebuild(const std::vector<RigidBody>& bodies, float margin);

    const std::vector<BroadphasePair>& pairs() const { return m_pairs; }

    // 本帧统计（验证脚本 + 面板显示用）
    struct Stats {
        std::uint32_t bodyCount = 0;
        std::uint32_t planeCount = 0;
        std::uint32_t pairs = 0;
        std::uint32_t axisTests = 0;   // 实际做过的 y/z 重叠测试次数
    };
    const Stats& stats() const { return m_stats; }

private:
    std::vector<BroadphasePair> m_pairs;
    std::vector<std::uint32_t> m_order;   // 按 min.x 排序后的体索引
    std::vector<std::uint32_t> m_planes;  // 平面体的索引
    Stats m_stats;
};

} // namespace physics
