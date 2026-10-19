#include "assets/LodSelfTest.h"

#include "assets/Lod.h"
#include "core/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace assets {

namespace {

int g_pass = 0;
int g_fail = 0;

void check(const char* name, bool ok, const char* detail = "") {
    if (ok) {
        ++g_pass;
        VK_LOG_INFO("LOD-T pass %s %s", name, detail);
    } else {
        ++g_fail;
        VK_LOG_ERROR("LOD-T FAIL %s %s", name, detail);
    }
}

// 一张 (n x n) 的方格平面，落在 z=0。
// `splitJoint = true` 时按 x 的正负分给 joint 0 / 1（每个顶点**只受一根**
// 骨头影响，权重 1）—— 这样"跨骨合并"可以一眼看出来：合并出来的顶点位置
// 会跑到对面去，或者带上对面的 joint 下标。
// `blend` 不为 0 时，靠近 x=0 的顶点改成两根骨头混合（0.6 / 0.4）。
struct SkinnedGrid {
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
};

SkinnedGrid makeGrid(int n, bool splitJoint, float blend) {
    SkinnedGrid g;
    const int row = n + 1;
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            Vertex v;
            v.pos = glm::vec3(-1.0f + 2.0f * static_cast<float>(x) / n,
                              -1.0f + 2.0f * static_cast<float>(y) / n, 0.0f);
            v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            v.uv = glm::vec2(static_cast<float>(x) / n,
                             static_cast<float>(y) / n);
            if (splitJoint) {
                const bool left = v.pos.x < 0.0f;
                const float seam = std::fabs(v.pos.x);
                if (blend > 0.0f && seam <= (2.0f / n) * 1.5f) {
                    // 接缝附近：两根骨头混合
                    v.joints = glm::uvec4(left ? 0u : 1u, left ? 1u : 0u, 0u, 0u);
                    v.weights = glm::vec4(1.0f - blend, blend, 0.0f, 0.0f);
                } else {
                    v.joints = glm::uvec4(left ? 0u : 1u, 0u, 0u, 0u);
                    v.weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                }
            }
            g.verts.push_back(v);
        }
    }
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const uint32_t i0 = static_cast<uint32_t>(y * row + x);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + static_cast<uint32_t>(row);
            const uint32_t i3 = i2 + 1;
            g.idx.insert(g.idx.end(), {i0, i1, i3, i0, i3, i2});
        }
    }
    return g;
}

float weightSum(const Vertex& v) {
    return v.weights.x + v.weights.y + v.weights.z + v.weights.w;
}

// 主影响 joint（与 Lod.cpp 里的 primaryJoint 同一口径，独立实现一份：
// 自检如果复用被测代码，被测代码写错时它也会跟着错）
uint32_t primaryOf(const Vertex& v) {
    uint32_t best = 0;
    float bw = v.weights.x;
    if (v.weights.y > bw) { bw = v.weights.y; best = 1; }
    if (v.weights.z > bw) { bw = v.weights.z; best = 2; }
    if (v.weights.w > bw) { bw = v.weights.w; best = 3; }
    return v.joints[best];
}

} // namespace

int runLodSelfTest() {
    g_pass = 0;
    g_fail = 0;

    // ---- A 组：非蒙皮网格行为不变 ----
    {
        SkinnedGrid g = makeGrid(12, false, 0.0f);
        LodMeshData d = decimateClustered(g.verts, g.idx, 4);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "in=%u out=%u",
                      static_cast<unsigned>(g.idx.size() / 3),
                      static_cast<unsigned>(d.indices.size() / 3));
        check("A1 非蒙皮网格仍然能减面", d.ok && d.indices.size() < g.idx.size(),
              buf);
        bool allZero = true;
        for (const Vertex& v : d.vertices) {
            if (weightSum(v) > 0.0f) { allZero = false; break; }
        }
        check("A2 非蒙皮网格减面后 joints/weights 仍是全 0", allZero);
    }

    // ---- B 组：单骨顶点 → 合并后仍只受一根骨头影响 ----
    {
        SkinnedGrid g = makeGrid(12, true, 0.0f);
        // ⚠⚠ gridRes 必须是**奇数**：格边长 = 2/gridRes，偶数时格线正好落在
        //   x=0（两根骨头的分界）上，于是**没有任何格子跨过接缝** —— 就算
        //   分组键里完全不带 joint，"跨骨合并"也一次都不会发生，断言全绿。
        //   这不是假设：本自检第一版用 gridRes=4，把 joint 分组整个去掉后
        //   仍然 pass=9 fail=0（假绿）。5 的格线在 -1,-0.6,-0.2,0.2,...，
        //   中间那格 [-0.2,0.2) 真的横跨两根骨头。
        LodMeshData d = decimateClustered(g.verts, g.idx, 5);
        check("B1 蒙皮网格也能减面了（W2-3）",
              d.ok && d.indices.size() < g.idx.size());

        int mixed = 0, notOne = 0, wrongJoint = 0;
        for (const Vertex& v : d.vertices) {
            int nz = 0;
            for (int k = 0; k < 4; ++k)
                if (v.weights[k] > 0.0f) ++nz;
            if (nz != 1) ++notOne;
            if (std::fabs(weightSum(v) - 1.0f) > 0.01f) ++mixed;
            // 跨骨污染：joint 0 只该出现在 x<0 那半边
            const uint32_t pj = primaryOf(v);
            if (pj == 0 && v.pos.x > 0.0f) ++wrongJoint;
            if (pj == 1 && v.pos.x < 0.0f) ++wrongJoint;
            // 更直接：别的 joint 不该出现在同一个顶点上
            for (int k = 0; k < 4; ++k) {
                if (v.weights[k] <= 0.0f) continue;
                if (v.joints[k] != pj) ++wrongJoint;
            }
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "verts=%u notOne=%d sumBad=%d cross=%d",
                      static_cast<unsigned>(d.vertices.size()), notOne, mixed,
                      wrongJoint);
        check("B2 每个顶点恰好受一根骨头影响", notOne == 0, buf);
        check("B3 权重和 = 1（不缩水）", mixed == 0, buf);
        check("B4 没有跨骨合并（另一根骨头的顶点没被吸过来）",
              wrongJoint == 0, buf);
    }

    // ---- C 组：混合权重 → 归一化后仍然和 = 1 ----
    {
        SkinnedGrid g = makeGrid(12, true, 0.4f);
        LodMeshData d = decimateClustered(g.verts, g.idx, 5);   // 奇数，见 B 组
        float worst = 0.0f;
        int bad = 0;
        for (const Vertex& v : d.vertices) {
            const float s = weightSum(v);
            if (s <= 0.0f) continue;   // 网格里没有权重的顶点（不存在于本例）
            if (std::fabs(s - 1.0f) > 0.01f) ++bad;
            worst = std::max(worst, std::fabs(s - 1.0f));
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "bad=%d worst=%.4f", bad, worst);
        check("C1 混合权重的顶点归一化到 1", bad == 0, buf);

        // joint 下标必须是整数原值，不能出现"平均出来的中间骨头"
        int bogus = 0;
        for (const Vertex& v : d.vertices) {
            for (int k = 0; k < 4; ++k) {
                if (v.weights[k] <= 0.0f) continue;
                if (v.joints[k] > 1) ++bogus;   // 本例只有 0 / 1 两根骨头
            }
        }
        check("C2 joint 下标没被平均（只可能是 0 或 1）", bogus == 0, buf);
    }

    // ---- D 组：退化输入不崩 ----
    {
        std::vector<Vertex> one{Vertex{}};
        std::vector<uint32_t> none;
        LodMeshData d = decimateClustered(one, none, 4);
        check("D1 空索引安全返回（不崩）", !d.ok);
    }

    char summary[96];
    std::snprintf(summary, sizeof(summary), "pass=%d fail=%d", g_pass, g_fail);
    VK_LOG_INFO("LOD-TOTAL %s", summary);
    return g_fail == 0 ? 0 : 1;
}

} // namespace assets
