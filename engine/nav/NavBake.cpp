// ============================================================
// nav/NavBake.cpp —— 射线采样 → 格子
// ============================================================

#include "nav/NavBake.h"

#include "physics/PhysicsWorld.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace nav {

namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
}

NavBakeStats bakeFromPhysics(const NavGridDesc& desc, const physics::PhysicsWorld* world,
                             NavGrid& out) {
    const auto t0 = std::chrono::steady_clock::now();
    NavBakeStats s;

    if (desc.cellSize <= 1e-4f) return s;
    const glm::vec3 span = desc.max - desc.min;
    if (span.x <= 0.0f || span.y <= 0.0f) return s;
    const int cols = static_cast<int>(std::ceil(span.x / desc.cellSize));
    const int rows = static_cast<int>(std::ceil(span.y / desc.cellSize));
    if (cols <= 0 || rows <= 0) return s;

    s.cols = cols;
    s.rows = rows;
    s.cells = cols * rows;

    const std::size_t n = static_cast<std::size_t>(s.cells);
    std::vector<float> heights(n, kNaN);

    if (world) {
        const float cosLimit = std::cos(glm::radians(desc.maxSlopeDeg));
        // 射线起点要高于整个采样盒上沿：从 max.z 往下打，
        // 保证"顶面在 max.z 之上的东西"不会漏检。
        const float rayTop = desc.max.z;
        const float rayLen = std::max(1.0f, desc.max.z - desc.min.z);
        // 代理球心离地面的高度。半径大、身高小时取半径那一侧 ——
        // 否则球会沉进地面，于是**每一格**都被地面自己挡住。
        const float clearZ = std::max(desc.agentRadius + 0.05f, desc.agentHeight * 0.5f);

        std::vector<physics::BodyId> hits;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const float x = desc.min.x + (static_cast<float>(c) + 0.5f) * desc.cellSize;
                const float y = desc.min.y + (static_cast<float>(r) + 0.5f) * desc.cellSize;
                ++s.rays;
                const physics::RayHit hit =
                    world->raycast(glm::vec3(x, y, rayTop), glm::vec3(0.0f, 0.0f, -1.0f), rayLen);
                if (!hit.hit) {
                    ++s.noGround;
                    continue;
                }
                if (hit.normal.z < cosLimit) {
                    ++s.tooSteep;
                    continue;
                }
                hits.clear();
                world->overlapSphere(hit.point + glm::vec3(0.0f, 0.0f, clearZ), desc.agentRadius,
                                     hits);
                if (!hits.empty()) {
                    ++s.tooTight;
                    continue;
                }
                heights[static_cast<std::size_t>(r * cols + c)] = hit.point.z;
                ++s.walk;
            }
        }
    }

    // ⚠ 一定要走 buildFromHeights：格子尺寸/原点的算法只有那一份。
    //   在这里另算一遍，会出现"烘焙出来的 cols 与 findPath 假设的 cols
    //   差一列"，症状是最后一列永远走不进去。
    NavGridDesc d = desc;
    // 让 buildFromHeights 的"没有地面"判定与这里的 NaN 对齐：
    // NaN 一律 Blocked（`buildFromHeights` 用 isfinite 判）。
    buildFromHeights(out, d, heights);

    const auto t1 = std::chrono::steady_clock::now();
    s.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return s;
}

std::string bakeSummary(const NavBakeStats& s) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "grid=%dx%d cells=%d walk=%d noGround=%d tooSteep=%d tooTight=%d rays=%d "
                  "ms=%.1f",
                  s.cols, s.rows, s.cells, s.walk, s.noGround, s.tooSteep, s.tooTight, s.rays,
                  s.ms);
    return std::string(buf);
}

} // namespace nav
