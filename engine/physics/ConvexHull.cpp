// ============================================================
// physics/ConvexHull.cpp —— QuickHull 三维凸包实现
//
// 算法骨架（每一步在代码里都有对应注释）：
//
//   1. 去重：丢掉重合点（重合点会让初始四面体退化）
//   2. 初始四面体：先找一对最远的点，再找离这条直线最远的点，最后找离
//      这个三角形最远的点。三步都取极值，必然落在凸包上，且能保证四面
//      体不退化成平面
//   3. 建冲突表：每个剩余点挂到它能"看见"的第一个面上
//   4. 迭代：
//        a. 取一个还挂着外部点的面，挑出离它最远的那个点 p
//        b. 从该面出发泛洪，收集"能看见 p 的所有面"（可见集合）
//        c. 可见集合与其余面之间的边 = 地平线（horizon）
//        d. 删掉可见面，用 p + 每条地平线边重建一圈新面
//        e. 把可见面上挂着的点重新分配给新面
//   5. 直到没有面还挂着外部点
//
// 「面 f 能看见点 p」的判据是 p 在 f 平面的**外侧**：
//     dot(f.normal, p) - f.d > eps
// 泛洪只检查相邻面，所以每次迭代的代价与"新增面的数量"成正比，与总面数
// 无关 —— 这正是 QuickHull 比"每轮全量扫点×面"快的地方。
// ============================================================

#include "physics/ConvexHull.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace physics {
namespace {

// 一个"活着的"面 + 邻接信息 + 挂在它上面的外部点。
struct QhFace {
    int v[3] = {0, 0, 0};       // 顶点索引，绕序：从外面看 CCW
    glm::vec3 normal{0.0f};     // 单位外法线
    float d = 0.0f;             // 平面偏移：面内点满足 dot(normal, x) == d
    int adj[3] = {-1, -1, -1};  // adj[i] = 与有向边 (v[i] -> v[i+1]) 相邻的面
    bool dead = false;          // 已被后续迭代删掉（不再是凸包的面）
    int stamp = -1;             // 泛洪用的访问标记（= 当前迭代轮次）
    std::vector<int> outside;   // 冲突表：还在这张面外面的点（去重后的索引）
};

inline bool sees(const QhFace& f, const glm::vec3& p, float eps) {
    return glm::dot(f.normal, p) - f.d > eps;
}

// 设定平面。退化三角形（三点共线 / 重合）给一个"看不见任何点"的平面：
// d = +inf 让 sees() 恒假，这样它不会吸住任何冲突点。
void makePlane(QhFace& f, const std::vector<glm::vec3>& pts) {
    const glm::vec3& a = pts[static_cast<std::size_t>(f.v[0])];
    const glm::vec3& b = pts[static_cast<std::size_t>(f.v[1])];
    const glm::vec3& c = pts[static_cast<std::size_t>(f.v[2])];
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float len = glm::length(n);
    if (len < 1e-20f) {
        f.normal = glm::vec3(0.0f, 0.0f, 1.0f);
        f.d = std::numeric_limits<float>::infinity();
        f.dead = true;  // 退化面直接判死，免得它进结果集
        return;
    }
    f.normal = n / len;
    f.d = glm::dot(f.normal, a);
}

// 去重：按 eps 网格量化后查表，O(N) 期望。
// 查 27 个邻格而不是 1 个 —— 否则"两点只差半个格子"会被判成不同点。
std::vector<glm::vec3> dedupe(const std::vector<glm::vec3>& in, float eps) {
    if (in.empty()) return {};
    const float inv = 1.0f / std::max(eps, 1e-6f);

    struct Key {
        long long x, y, z;
        bool operator==(const Key& o) const {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return static_cast<std::size_t>(k.x * 73856093LL) ^
                   static_cast<std::size_t>(k.y * 19349663LL) ^
                   static_cast<std::size_t>(k.z * 83492791LL);
        }
    };

    std::unordered_map<Key, int, KeyHash> grid;
    grid.reserve(in.size() * 2);
    std::vector<glm::vec3> out;
    out.reserve(in.size());
    const float e2 = eps * eps;

    for (const glm::vec3& p : in) {
        const Key k{static_cast<long long>(std::floor(p.x * inv)),
                    static_cast<long long>(std::floor(p.y * inv)),
                    static_cast<long long>(std::floor(p.z * inv))};
        bool dup = false;
        for (long long dx = -1; dx <= 1 && !dup; ++dx)
            for (long long dy = -1; dy <= 1 && !dup; ++dy)
                for (long long dz = -1; dz <= 1 && !dup; ++dz) {
                    auto it = grid.find(Key{k.x + dx, k.y + dy, k.z + dz});
                    if (it == grid.end()) continue;
                    const glm::vec3& q = out[static_cast<std::size_t>(it->second)];
                    if (glm::dot(q - p, q - p) <= e2) dup = true;
                }
        if (dup) continue;
        grid.emplace(k, static_cast<int>(out.size()));
        out.push_back(p);
    }
    return out;
}

void rebuildBounds(ConvexHull& h) {
    if (h.points.empty()) return;
    h.minBound = h.maxBound = h.points[0];
    glm::dvec3 sum(0.0);
    for (const glm::vec3& p : h.points) {
        h.minBound = glm::min(h.minBound, p);
        h.maxBound = glm::max(h.maxBound, p);
        sum += glm::dvec3(p);
    }
    h.centroid = glm::vec3(sum / static_cast<double>(h.points.size()));
}

} // namespace

glm::vec3 ConvexHull::support(const glm::vec3& dir) const {
    if (points.empty()) return glm::vec3(0.0f);
    std::size_t best = 0;
    float bestDot = glm::dot(points[0], dir);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const float dt = glm::dot(points[i], dir);
        if (dt > bestDot) {
            bestDot = dt;
            best = i;
        }
    }
    return points[best];
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> ConvexHull::edges() const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    out.reserve(faces.size() * 3 / 2);
    std::unordered_map<std::uint64_t, bool> seen;  // 无向边去重
    seen.reserve(faces.size() * 2);
    for (const Face& f : faces) {
        const std::uint32_t tri[3] = {f.a, f.b, f.c};
        for (int e = 0; e < 3; ++e) {
            std::uint32_t u = tri[e];
            std::uint32_t v = tri[(e + 1) % 3];
            if (u > v) std::swap(u, v);
            const std::uint64_t key = (static_cast<std::uint64_t>(u) << 32) | v;
            if (seen.emplace(key, true).second) out.emplace_back(u, v);
        }
    }
    return out;
}

ConvexHull buildConvexHull(const std::vector<glm::vec3>& input, float eps) {
    ConvexHull out;

    std::vector<glm::vec3> pts = dedupe(input, std::max(eps, 1e-6f) * 4.0f);
    if (pts.size() < 4) return out;

    // ---------------- 1) 初始四面体 ----------------
    int i0 = 0, i1 = 0;
    for (int axis = 0; axis < 3; ++axis) {
        int lo = 0, hi = 0;
        for (std::size_t i = 1; i < pts.size(); ++i) {
            if (pts[i][axis] < pts[static_cast<std::size_t>(lo)][axis])
                lo = static_cast<int>(i);
            if (pts[i][axis] > pts[static_cast<std::size_t>(hi)][axis])
                hi = static_cast<int>(i);
        }
        if (glm::dot(pts[static_cast<std::size_t>(hi)] -
                         pts[static_cast<std::size_t>(lo)], pts[static_cast<std::size_t>(hi)] -
                         pts[static_cast<std::size_t>(lo)]) >
            glm::dot(pts[static_cast<std::size_t>(i1)] -
                         pts[static_cast<std::size_t>(i0)], pts[static_cast<std::size_t>(i1)] -
                         pts[static_cast<std::size_t>(i0)])) {
            i0 = lo;
            i1 = hi;
        }
    }
    if (glm::dot(pts[static_cast<std::size_t>(i1)] -
                     pts[static_cast<std::size_t>(i0)], pts[static_cast<std::size_t>(i1)] -
                     pts[static_cast<std::size_t>(i0)]) < eps * eps)
        return out;

    const glm::vec3 lineDir = glm::normalize(
        pts[static_cast<std::size_t>(i1)] - pts[static_cast<std::size_t>(i0)]);
    int i2 = -1;
    float bestArea = eps;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const int ii = static_cast<int>(i);
        if (ii == i0 || ii == i1) continue;
        const float area = glm::length(glm::cross(
            pts[i] - pts[static_cast<std::size_t>(i0)], lineDir));
        if (area > bestArea) {
            bestArea = area;
            i2 = ii;
        }
    }
    if (i2 < 0) return out;  // 所有点共线

    glm::vec3 triN = glm::cross(pts[static_cast<std::size_t>(i1)] -
                                    pts[static_cast<std::size_t>(i0)],
                                pts[static_cast<std::size_t>(i2)] -
                                    pts[static_cast<std::size_t>(i0)]);
    const float triLen = glm::length(triN);
    if (triLen < eps) return out;
    triN /= triLen;

    int i3 = -1;
    float bestVol = eps;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const int ii = static_cast<int>(i);
        if (ii == i0 || ii == i1 || ii == i2) continue;
        const float vol = std::fabs(glm::dot(
            pts[i] - pts[static_cast<std::size_t>(i0)], triN));
        if (vol > bestVol) {
            bestVol = vol;
            i3 = ii;
        }
    }
    if (i3 < 0) return out;  // 所有点共面（薄片输入）

    // ---------------- 2) 四面体的四个面 + 邻接 ----------------
    std::vector<QhFace> faces;
    faces.reserve(pts.size() * 2 + 8);
    {
        const int idx[4] = {i0, i1, i2, i3};
        for (int f = 0; f < 4; ++f) {
            QhFace face;
            int a = idx[f];
            int b = idx[(f + 1) % 4];
            int c = idx[(f + 2) % 4];
            // 法线朝外 ⟺ 与剩下那个顶点异侧
            const glm::vec3 n = glm::cross(pts[static_cast<std::size_t>(b)] -
                                               pts[static_cast<std::size_t>(a)],
                                           pts[static_cast<std::size_t>(c)] -
                                               pts[static_cast<std::size_t>(a)]);
            if (glm::dot(n, pts[static_cast<std::size_t>(idx[(f + 3) % 4])] -
                                pts[static_cast<std::size_t>(a)]) > 0.0f)
                std::swap(b, c);
            face.v[0] = a;
            face.v[1] = b;
            face.v[2] = c;
            makePlane(face, pts);
            faces.push_back(face);
        }
        // 四面体只有 4 个面，两两配一次就够
        for (std::size_t i = 0; i < faces.size(); ++i) {
            for (int e = 0; e < 3; ++e) {
                const int a = faces[i].v[e];
                const int b = faces[i].v[(e + 1) % 3];
                for (std::size_t j = 0; j < faces.size(); ++j) {
                    if (i == j) continue;
                    for (int f = 0; f < 3; ++f) {
                        if (faces[j].v[f] == b &&
                            faces[j].v[(f + 1) % 3] == a)
                            faces[i].adj[e] = static_cast<int>(j);
                    }
                }
            }
        }
    }

    // ---------------- 3) 冲突表 ----------------
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const int ii = static_cast<int>(i);
        if (ii == i0 || ii == i1 || ii == i2 || ii == i3) continue;
        for (std::size_t f = 0; f < faces.size(); ++f) {
            if (!faces[f].dead && sees(faces[f], pts[i], eps)) {
                faces[f].outside.push_back(ii);
                break;
            }
        }
    }

    // ---------------- 4) 迭代 ----------------
    // 上限保护：正常每轮至少吃掉一个外部点。真触顶说明数值上打转，直接
    // 收下当前结果（比死循环强）。
    const std::size_t maxIterations = pts.size() * 4 + 64;
    int stamp = 0;

    for (std::size_t iter = 0; iter < maxIterations; ++iter) {
        // a) 找一个还挂着外部点的活面
        int seed = -1;
        for (std::size_t f = 0; f < faces.size(); ++f) {
            if (!faces[f].dead && !faces[f].outside.empty()) {
                seed = static_cast<int>(f);
                break;
            }
        }
        if (seed < 0) break;  // 收敛

        // 挑离这个面最远的点
        int farIdx = faces[static_cast<std::size_t>(seed)].outside[0];
        float farDist = glm::dot(faces[static_cast<std::size_t>(seed)].normal,
                                 pts[static_cast<std::size_t>(farIdx)]) -
                        faces[static_cast<std::size_t>(seed)].d;
        for (int pi : faces[static_cast<std::size_t>(seed)].outside) {
            const float dd =
                glm::dot(faces[static_cast<std::size_t>(seed)].normal,
                         pts[static_cast<std::size_t>(pi)]) -
                faces[static_cast<std::size_t>(seed)].d;
            if (dd > farDist) {
                farDist = dd;
                farIdx = pi;
            }
        }
        const glm::vec3 p = pts[static_cast<std::size_t>(farIdx)];

        // b) 泛洪：所有"能看见 p"的面
        ++stamp;
        std::vector<int> visible;
        std::vector<int> stack{seed};
        faces[static_cast<std::size_t>(seed)].stamp = stamp;
        while (!stack.empty()) {
            const int fi = stack.back();
            stack.pop_back();
            visible.push_back(fi);
            for (int e = 0; e < 3; ++e) {
                const int nb = faces[static_cast<std::size_t>(fi)].adj[e];
                if (nb < 0) continue;
                QhFace& nbf = faces[static_cast<std::size_t>(nb)];
                if (nbf.dead || nbf.stamp == stamp) continue;
                if (sees(nbf, p, eps)) {
                    nbf.stamp = stamp;
                    stack.push_back(nb);
                }
            }
        }

        // c) 地平线 = 可见集合的边界边。
        //    可见面里的有向边 (a -> b)，若反向边 (b -> a) 也在可见集合里，
        //    就是内部边；否则是边界边，另一侧是个**没被删掉**的面。
        struct HorizonEdge {
            int a, b;
            int outsideNeighbor;  // 边另一侧那个没被删的面（-1 = 没有）
            int neighborEdge;     // 该面里这条边的下标
        };
        std::vector<HorizonEdge> horizon;
        horizon.reserve(16);
        for (int fi : visible) {
            for (int e = 0; e < 3; ++e) {
                const int a = faces[static_cast<std::size_t>(fi)].v[e];
                const int b = faces[static_cast<std::size_t>(fi)].v[(e + 1) % 3];
                const int nb = faces[static_cast<std::size_t>(fi)].adj[e];
                int nEdge = -1;
                if (nb >= 0) {
                    for (int nf = 0; nf < 3; ++nf) {
                        if (faces[static_cast<std::size_t>(nb)].v[nf] == b &&
                            faces[static_cast<std::size_t>(nb)]
                                    .v[(nf + 1) % 3] == a)
                            nEdge = nf;
                    }
                }
                if (nEdge >= 0 &&
                    faces[static_cast<std::size_t>(nb)].stamp == stamp)
                    continue;  // 内部边
                horizon.push_back(HorizonEdge{a, b, nb, nEdge});
            }
        }
        if (horizon.empty()) {
            // 退化：这轮的泛洪没形成闭合环。把这个面的冲突表清掉，
            // 免得下一轮又选中同一个面而原地打转。
            faces[static_cast<std::size_t>(seed)].outside.clear();
            continue;
        }

        // 待重分配的点（farIdx 自己会被新面吃掉）
        std::vector<int> pending;
        for (int fi : visible) {
            for (int pi : faces[static_cast<std::size_t>(fi)].outside)
                if (pi != farIdx) pending.push_back(pi);
            faces[static_cast<std::size_t>(fi)].outside.clear();
        }

        // d) 用 p + 每条地平线边建新面。
        //    绕序由地平线有向边直接决定：可见面里是 (a -> b)，新面写成
        //    (b -> a -> p) 才能继续保持"从外面看 CCW"。
        std::vector<int> newFaces;
        newFaces.reserve(horizon.size());
        for (const HorizonEdge& he : horizon) {
            QhFace nf;
            nf.v[0] = he.b;
            nf.v[1] = he.a;
            nf.v[2] = farIdx;
            makePlane(nf, pts);
            faces.push_back(nf);
            newFaces.push_back(static_cast<int>(faces.size()) - 1);
        }

        // 新面之间两两配对（地平线环一般十几条边，平方完全够用）
        for (std::size_t i = 0; i < newFaces.size(); ++i) {
            for (int e = 1; e < 3; ++e) {  // 边 0 留给地平线外侧那个面
                const int a = faces[static_cast<std::size_t>(newFaces[i])].v[e];
                const int b =
                    faces[static_cast<std::size_t>(newFaces[i])].v[(e + 1) % 3];
                for (std::size_t j = 0; j < newFaces.size(); ++j) {
                    if (i == j) continue;
                    for (int f = 0; f < 3; ++f) {
                        if (faces[static_cast<std::size_t>(newFaces[j])].v[f] ==
                                b &&
                            faces[static_cast<std::size_t>(newFaces[j])]
                                    .v[(f + 1) % 3] == a) {
                            faces[static_cast<std::size_t>(newFaces[i])].adj[e] =
                                newFaces[j];
                        }
                    }
                }
            }
        }
        for (std::size_t i = 0; i < horizon.size(); ++i) {
            const HorizonEdge& he = horizon[i];
            faces[static_cast<std::size_t>(newFaces[i])].adj[0] =
                he.outsideNeighbor;
            if (he.outsideNeighbor >= 0 && he.neighborEdge >= 0)
                faces[static_cast<std::size_t>(he.outsideNeighbor)]
                    .adj[he.neighborEdge] = newFaces[i];
        }

        // e) 重分配：只测新面（旧面要么被删了，要么已经看不见这些点）
        for (int pi : pending) {
            for (int nf : newFaces) {
                if (sees(faces[static_cast<std::size_t>(nf)],
                         pts[static_cast<std::size_t>(pi)], eps)) {
                    faces[static_cast<std::size_t>(nf)].outside.push_back(pi);
                    break;
                }
            }
        }

        // 可见面此刻正式作废（stamp 只用于泛洪，dead 才是真相）
        for (int fi : visible) faces[static_cast<std::size_t>(fi)].dead = true;
    }

    // ---------------- 5) 收集结果 ----------------
    std::vector<int> remap(pts.size(), -1);
    for (const QhFace& f : faces) {
        if (f.dead) continue;
        for (int k = 0; k < 3; ++k) {
            const int vi = f.v[k];
            if (remap[static_cast<std::size_t>(vi)] < 0) {
                remap[static_cast<std::size_t>(vi)] =
                    static_cast<int>(out.points.size());
                out.points.push_back(pts[static_cast<std::size_t>(vi)]);
            }
        }
    }
    if (out.points.size() < 4) return ConvexHull{};

    for (const QhFace& f : faces) {
        if (f.dead) continue;
        ConvexHull::Face face;
        face.a = static_cast<std::uint32_t>(remap[static_cast<std::size_t>(f.v[0])]);
        face.b = static_cast<std::uint32_t>(remap[static_cast<std::size_t>(f.v[1])]);
        face.c = static_cast<std::uint32_t>(remap[static_cast<std::size_t>(f.v[2])]);
        if (face.a == face.b || face.b == face.c || face.a == face.c) continue;
        out.faces.push_back(face);
    }

    rebuildBounds(out);
    if (out.faces.empty()) return ConvexHull{};
    return out;
}

} // namespace physics
