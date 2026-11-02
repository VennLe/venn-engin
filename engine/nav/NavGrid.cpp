// ============================================================
// nav/NavGrid.cpp —— 格子网的数据、A*、视线、平滑
//
// ⚠ 本文件**不 include physics / rhi / scene**。理由见头文件顶部：
//   "能无设备自检"是选格子法的第一个理由，把射线拉进来就没了。
// ============================================================

#include "nav/NavGrid.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <utility>

namespace nav {

namespace {
constexpr float kInf = std::numeric_limits<float>::infinity();

// 8 邻域。斜向排在后面，四方向优先 —— 同代价时先走直线，
// 出来的路径更像"人走的"，而 A* 本身并不关心。
struct Dir {
    int dc;
    int dr;
    float cost;
    bool diagonal;
};
constexpr Dir kDirs[8] = {
    {1, 0, 1.0f, false},
    {-1, 0, 1.0f, false},
    {0, 1, 1.0f, false},
    {0, -1, 1.0f, false},
    {1, 1, 1.41421356f, true},
    {1, -1, 1.41421356f, true},
    {-1, 1, 1.41421356f, true},
    {-1, -1, 1.41421356f, true},
};
} // namespace

std::size_t NavGrid::walkableCount() const {
    std::size_t n = 0;
    for (std::uint8_t v : cell) {
        if (v) ++n;
    }
    return n;
}

NavGridDesc NavGrid::desc() const {
    NavGridDesc d;
    d.min = origin;
    d.max = worldMax();
    d.cellSize = cellSize;
    d.maxStep = maxStep;
    return d;
}

bool buildFromHeights(NavGrid& out, const NavGridDesc& d, const std::vector<float>& heights) {
    if (d.cellSize <= 1e-4f) return false;
    const glm::vec3 span = d.max - d.min;
    if (span.x <= 0.0f || span.y <= 0.0f) return false;
    const int cols = static_cast<int>(std::ceil(span.x / d.cellSize));
    const int rows = static_cast<int>(std::ceil(span.y / d.cellSize));
    if (cols <= 0 || rows <= 0) return false;
    const std::size_t n = static_cast<std::size_t>(cols) * rows;
    if (heights.size() != n) return false;

    out.origin = d.min;
    out.cellSize = d.cellSize;
    out.cols = cols;
    out.rows = rows;
    out.maxStep = d.maxStep;
    out.cell.assign(n, 0u);
    out.height.assign(n, d.min.z);

    // ⚠ "这格没有地面"的判定只有一个地方：非有限值，或者比采样盒底面
    //   还低 1 米。烘焙侧拿不到射线时写 NaN 进来，不写别处约定的魔数 ——
    //   两种哨兵会让"漏了一格"变成"那一格是地面在 -60 米处"。
    for (std::size_t i = 0; i < n; ++i) {
        const float h = heights[i];
        const bool hasGround = std::isfinite(h) && h > d.min.z - 1.0f;
        out.cell[i] = hasGround ? 1u : 0u;
        out.height[i] = hasGround ? h : d.min.z;
    }
    return true;
}

bool nearestWalkable(const NavGrid& g, const glm::vec3& p, float maxRadius, glm::ivec2& out) {
    if (!g.valid()) return false;
    const glm::ivec2 c0 = g.cellOf(p);
    if (g.walkable(c0.x, c0.y)) {
        out = c0;
        return true;
    }
    const float cs = g.cellSize;
    const int maxRing = std::max(1, static_cast<int>(std::ceil(std::max(maxRadius, 0.0f) / cs)));
    // 一圈圈外扩，取这一圈里**欧氏最近**的格；同距离时按 (r,c) 字典序，
    // 保证同一张图两次调用拿到同一格（用例要能逐位比）。
    for (int ring = 1; ring <= maxRing; ++ring) {
        int bestC = -1;
        int bestR = -1;
        float bestD2 = kInf;
        for (int dr = -ring; dr <= ring; ++dr) {
            for (int dc = -ring; dc <= ring; ++dc) {
                if (std::max(std::abs(dc), std::abs(dr)) != ring) continue;   // 只扫边
                const int c = c0.x + dc;
                const int r = c0.y + dr;
                if (!g.walkable(c, r)) continue;
                const glm::vec3 cc = g.centerOf(c, r);
                const float dx = cc.x - p.x;
                const float dy = cc.y - p.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < bestD2 - 1e-6f) {
                    bestD2 = d2;
                    bestC = c;
                    bestR = r;
                }
            }
        }
        if (bestC >= 0) {
            out = glm::ivec2(bestC, bestR);
            return true;
        }
    }
    return false;
}

bool lineWalkable(const NavGrid& g, const glm::vec3& a, const glm::vec3& b) {
    if (!g.valid()) return false;
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    // 采样步长 = 半格：再粗就可能整格跳过去（一条 45° 斜线穿过一格墙角
    // 时，采样点全落在两个相邻格里）。
    const int steps = std::max(1, static_cast<int>(std::ceil(dist / (g.cellSize * 0.5f))));
    float prevH = 0.0f;
    bool hasPrev = false;
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const glm::vec3 p(a.x + dx * t, a.y + dy * t, 0.0f);
        const glm::ivec2 c = g.cellOf(p);
        if (!g.walkable(c.x, c.y)) return false;
        const float h = g.heightAt(c.x, c.y);
        if (hasPrev && std::fabs(h - prevH) > g.maxStep + 1e-4f) return false;
        prevH = h;
        hasPrev = true;
    }
    return true;
}

bool findPath(const NavGrid& g, const glm::vec3& from, const glm::vec3& to, NavPath& out) {
    out.clear();
    if (!g.valid()) return false;

    glm::ivec2 startCell{0, 0};
    glm::ivec2 goalCell{0, 0};
    const float snapRadius = g.cellSize * 2.0f;
    if (!nearestWalkable(g, from, snapRadius, startCell)) return false;
    if (!nearestWalkable(g, to, snapRadius, goalCell)) return false;

    const int startIdx = g.index(startCell.x, startCell.y);
    const int goalIdx = g.index(goalCell.x, goalCell.y);

    // 终点是否"原样落在可行走格上"决定了路径最后一点用不用精确坐标。
    const glm::ivec2 rawGoal = g.cellOf(to);
    const bool goalExact = (rawGoal == goalCell);
    const glm::ivec2 rawStart = g.cellOf(from);
    const bool startExact = (rawStart == startCell);

    const std::size_t n = g.cell.size();
    std::vector<float> gScore(n, kInf);
    std::vector<int> cameFrom(n, -1);
    std::vector<std::uint8_t> closed(n, 0u);

    const auto heuristic = [&](int c, int r) {
        const int dc = std::abs(c - goalCell.x);
        const int dr = std::abs(r - goalCell.y);
        const int mn = std::min(dc, dr);
        const int mx = std::max(dc, dr);
        return (static_cast<float>(mx - mn) + 1.41421356f * static_cast<float>(mn));
    };

    // (f, index) 全序。⚠ 只比 f 的话，同 f 节点的展开顺序取决于堆的内部
    // 结构 —— 同一张图两次跑会给出两条不同形状的路径（用例偶发红）。
    using Node = std::pair<float, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    gScore[static_cast<std::size_t>(startIdx)] = 0.0f;
    open.push(Node(heuristic(startCell.x, startCell.y), startIdx));
    int expanded = 0;
    // 终点不可达时的退路：记住"离终点最近的那个已到达节点"。
    int bestIdx = startIdx;
    float bestH = heuristic(startCell.x, startCell.y);

    bool found = false;
    while (!open.empty()) {
        const Node cur = open.top();
        open.pop();
        const int idx = cur.second;
        if (closed[static_cast<std::size_t>(idx)]) continue;
        closed[static_cast<std::size_t>(idx)] = 1u;
        ++expanded;
        if (idx == goalIdx) {
            found = true;
            break;
        }
        const int c = idx % g.cols;
        const int r = idx / g.cols;
        const float h = heuristic(c, r);
        if (h < bestH - 1e-4f) {
            bestH = h;
            bestIdx = idx;
        }
        const float hCur = g.heightAt(c, r);
        for (const Dir& d : kDirs) {
            const int nc = c + d.dc;
            const int nr = r + d.dr;
            if (!g.walkable(nc, nr)) continue;
            // ① 不许斜向切角：斜迈一步时两个正交邻格都得能走。
            if (d.diagonal) {
                if (!g.walkable(c + d.dc, r) || !g.walkable(c, r + d.dr)) continue;
            }
            // ② 高差超过 maxStep 的边不通（格本身仍算可走 —— 它只是
            //    "从这边上不去"，不代表这一格不能用）。
            if (std::fabs(g.heightAt(nc, nr) - hCur) > g.maxStep + 1e-4f) continue;
            const int nIdx = g.index(nc, nr);
            if (closed[static_cast<std::size_t>(nIdx)]) continue;
            const float tentative = gScore[static_cast<std::size_t>(idx)] + d.cost;
            if (tentative < gScore[static_cast<std::size_t>(nIdx)] - 1e-6f) {
                gScore[static_cast<std::size_t>(nIdx)] = tentative;
                cameFrom[static_cast<std::size_t>(nIdx)] = idx;
                open.push(Node(tentative + heuristic(nc, nr), nIdx));
            }
        }
    }

    const int endIdx = found ? goalIdx : bestIdx;
    out.expanded = expanded;
    out.reached = found;

    if (!found && endIdx == startIdx) {
        // 原位不动：仍然给一条单点路径，调用方不必特判空。
        out.pts.push_back(from);
        return true;
    }

    std::vector<int> chain;
    for (int i = endIdx; i >= 0; i = cameFrom[static_cast<std::size_t>(i)]) {
        chain.push_back(i);
        if (i == startIdx) break;
    }
    if (chain.empty() || chain.back() != startIdx) {
        out.pts.push_back(from);
        return true;
    }
    std::reverse(chain.begin(), chain.end());

    out.pts.reserve(chain.size());
    for (int i : chain) {
        const int c = i % g.cols;
        const int r = i / g.cols;
        out.pts.push_back(g.centerOf(c, r));
    }
    // 首尾两点尽量用**调用方给的精确坐标**（否则"点到哪儿就走到哪儿"
    // 会差半格，玩家看到的是角色停在离目标 25 cm 的地方）。
    if (startExact) out.pts.front() = from;
    if (goalExact && found) out.pts.back() = to;

    out.length = 0.0f;
    for (std::size_t i = 1; i < out.pts.size(); ++i) {
        out.length += glm::length(out.pts[i] - out.pts[i - 1]);
    }
    return true;
}

void smoothPath(const NavGrid& g, NavPath& path) {
    if (path.pts.size() < 3 || !g.valid()) return;
    std::vector<glm::vec3> out;
    out.reserve(path.pts.size());
    out.push_back(path.pts.front());
    std::size_t cur = 0;
    while (cur + 1 < path.pts.size()) {
        std::size_t best = cur + 1;
        // 从最远往回找第一根"能直连"的点。找不到就退一格（保证前进）。
        for (std::size_t j = path.pts.size() - 1; j > cur + 1; --j) {
            if (lineWalkable(g, path.pts[cur], path.pts[j])) {
                best = j;
                break;
            }
        }
        out.push_back(path.pts[best]);
        cur = best;
    }
    path.pts.swap(out);
    path.length = 0.0f;
    for (std::size_t i = 1; i < path.pts.size(); ++i) {
        path.length += glm::length(path.pts[i] - path.pts[i - 1]);
    }
}

std::string toAscii(const NavGrid& g, int maxCols, int maxRows) {
    if (!g.valid()) return std::string("<empty>\n");
    const int stepC = std::max(1, (g.cols + std::max(1, maxCols) - 1) / std::max(1, maxCols));
    const int stepR = std::max(1, (g.rows + std::max(1, maxRows) - 1) / std::max(1, maxRows));
    std::string s;
    s.reserve(static_cast<std::size_t>((g.rows / stepR + 2) * (g.cols / stepC + 2)));
    // r 从大到小：打印出来的上下方向与俯视图一致（+Y 在上）。
    for (int r = g.rows - 1; r >= 0; r -= stepR) {
        for (int c = 0; c < g.cols; c += stepC) {
            s.push_back(g.walkable(c, r) ? '.' : '#');
        }
        s.push_back('\n');
    }
    return s;
}

} // namespace nav
