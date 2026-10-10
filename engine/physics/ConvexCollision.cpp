// ============================================================
// physics/ConvexCollision.cpp —— GJK + EPA 实现
//
// 结构：文件上半是几何原语（点到线段 / 点到三角形最近点、单纯形约简），
// 下半是 GJK 主循环与 EPA。所有原语都是"求原点在某个单纯形上的最近点"，
// 因为 GJK 的世界里原点就是目标。
// ============================================================

#include "physics/ConvexCollision.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace physics {
namespace {

// Minkowski 差 A ⊖ B 的支撑函数。
// 记住：差集里的一点 = A 上一点 减 B 上一点，所以 B 的支撑方向要取反。
struct Minkowski {
    const TransformedShape* a = nullptr;
    const TransformedShape* b = nullptr;

    glm::vec3 support(const glm::vec3& d) const {
        return a->support(d) - b->support(-d);
    }
};

// 单纯形：最多 4 个点（四面体）。n 是当前实际点数。
struct Simplex {
    glm::vec3 p[4] = {};
    int n = 0;
};

// 点到线段的最近点。t 是参数（0 = a，1 = b）。
// 目标点是原点，所以 AO = -a，t = <0-a, ab> / |ab|²。
float closestParamOnSegment(const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1e-30f) return 0.0f;
    return glm::clamp(-glm::dot(a, ab) / len2, 0.0f, 1.0f);
}

// 点到三角形的最近点（Ericson, Real-Time Collision Detection §5.1.5）。
// 重心坐标写回 u/v/w（对应 a/b/c），调用方据此把单纯形约简到真正用到的
// 那几个顶点上。
glm::vec3 closestOnTriangle(const glm::vec3& a, const glm::vec3& b,
                            const glm::vec3& c, float& u, float& v, float& w) {
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = -a;  // 目标是原点

    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {  // 顶点区域 a
        u = 1.0f; v = 0.0f; w = 0.0f;
        return a;
    }

    const glm::vec3 bp = -b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {  // 顶点区域 b
        u = 0.0f; v = 1.0f; w = 0.0f;
        return b;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {  // 边 ab
        const float t = d1 / (d1 - d3);
        u = 1.0f - t; v = t; w = 0.0f;
        return a + ab * t;
    }

    const glm::vec3 cp = -c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {  // 顶点区域 c
        u = 0.0f; v = 0.0f; w = 1.0f;
        return c;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {  // 边 ac
        const float t = d2 / (d2 - d6);
        u = 1.0f - t; v = 0.0f; w = t;
        return a + ac * t;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {  // 边 bc
        const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        u = 0.0f; v = 1.0f - t; w = t;
        return b + (c - b) * t;
    }

    const float denom = 1.0f / (va + vb + vc);  // 内部
    v = vb * denom;
    w = vc * denom;
    u = 1.0f - v - w;
    return a + ab * v + ac * w;
}

// 原点相对四面体某个面 (x,y,z) 是否在**外侧**。
// opp 是四面体剩下的那个顶点 —— 用它把面法线定向到"背离四面体内部"，
// 所以完全不需要关心绕序（这是这套写法最省心的地方）。
bool originOutsideFace(const glm::vec3& x, const glm::vec3& y,
                       const glm::vec3& z, const glm::vec3& opp) {
    glm::vec3 n = glm::cross(y - x, z - x);
    if (glm::dot(n, opp - x) > 0.0f) n = -n;
    return glm::dot(n, -x) > 0.0f;
}

// 单纯形约简：把单纯形缩到"含离原点最近点的最小特征"，并把 dir 指向
// 原点（dir = -最近点）。返回 true 表示原点已经被单纯形包住（相交）。
bool reduceSimplex(Simplex& s, glm::vec3& dir) {
    if (s.n == 1) {
        dir = -s.p[0];
        return false;
    }

    if (s.n == 2) {
        const float t = closestParamOnSegment(s.p[0], s.p[1]);
        if (t <= 0.0f) {
            s.p[0] = s.p[0];
            s.n = 1;
            dir = -s.p[0];
        } else if (t >= 1.0f) {
            s.p[0] = s.p[1];
            s.n = 1;
            dir = -s.p[0];
        } else {
            const glm::vec3 q = s.p[0] + (s.p[1] - s.p[0]) * t;
            s.n = 2;
            dir = -q;
            // 线段上的最近点不可能就是原点（除非退化），再走一轮
            if (glm::dot(q, q) < 1e-16f) return true;
        }
        return false;
    }

    if (s.n == 3) {
        float u, v, w;
        const glm::vec3 q = closestOnTriangle(s.p[0], s.p[1], s.p[2], u, v, w);
        const glm::vec3 keep[3] = {s.p[0], s.p[1], s.p[2]};
        const float bary[3] = {u, v, w};
        Simplex ns;
        for (int i = 0; i < 3; ++i) {
            if (bary[i] > 1e-7f) ns.p[ns.n++] = keep[i];
        }
        if (ns.n == 0) ns.p[ns.n++] = keep[0];
        s = ns;
        dir = -q;
        if (glm::dot(q, q) < 1e-16f) return true;
        return false;
    }

    // n == 4：四面体。四个面逐个问"原点在我外面吗"，都在内部 → 相交。
    const glm::vec3 a = s.p[0], b = s.p[1], c = s.p[2], d = s.p[3];
    if (originOutsideFace(a, b, c, d)) {
        s.p[0] = a; s.p[1] = b; s.p[2] = c; s.n = 3;
        return reduceSimplex(s, dir);
    }
    if (originOutsideFace(a, c, d, b)) {
        s.p[0] = a; s.p[1] = c; s.p[2] = d; s.n = 3;
        return reduceSimplex(s, dir);
    }
    if (originOutsideFace(a, d, b, c)) {
        s.p[0] = a; s.p[1] = d; s.p[2] = b; s.n = 3;
        return reduceSimplex(s, dir);
    }
    if (originOutsideFace(b, d, c, a)) {
        s.p[0] = b; s.p[1] = d; s.p[2] = c; s.n = 3;
        return reduceSimplex(s, dir);
    }
    return true;  // 原点在四面体内部
}

// GJK 主循环。hit = true 时 simplex 里装着含原点的单纯形（喂给 EPA）。
bool gjk(const Minkowski& m, Simplex& out) {
    Simplex s;
    // 第一个方向随便取轴向，避免零向量
    glm::vec3 dir(1.0f, 0.0f, 0.0f);
    s.p[0] = m.support(dir);
    s.n = 1;
    dir = -s.p[0];
    if (glm::dot(dir, dir) < 1e-18f) {
        dir = glm::vec3(0.0f, 1.0f, 0.0f);
        s.p[0] = m.support(dir);
        s.n = 1;
        dir = -s.p[0];
    }

    for (int iter = 0; iter < 64; ++iter) {
        if (glm::dot(dir, dir) < 1e-18f) {
            out = s;
            return true;  // 原点上就有单纯形顶点
        }
        const glm::vec3 w = m.support(dir);
        // 支撑点在该方向上还没能越过原点 → 原点不在差集里
        if (glm::dot(w, dir) <= 0.0f) return false;
        if (s.n >= 4) return false;  // 理论上不会走到（约简后最多 3）
        s.p[s.n++] = w;
        if (reduceSimplex(s, dir)) {
            out = s;
            return true;
        }
    }
    return false;
}

// ---------------- EPA ----------------

struct EpaFace {
    int v[3] = {0, 0, 0};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float d = 0.0f;  // 平面偏移：n·x = d；原点在内侧，所以 d > 0 表示"到原点距离"
    bool dead = false;
};

// 顶点不足 4 个时的补救：沿几个轴向补采样点，凑出一个包住原点的四面体。
// 触发条件很少（GJK 通常直接给出满单纯形），但漏掉的话 EPA 会直接崩。
bool expandToTetrahedron(const Minkowski& m, Simplex& s) {
    static const glm::vec3 dirs[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& d : dirs) {
        if (s.n >= 4) break;
        const glm::vec3 w = m.support(d);
        bool dup = false;
        for (int i = 0; i < s.n; ++i)
            if (glm::dot(s.p[i] - w, s.p[i] - w) < 1e-14f) dup = true;
        if (!dup) s.p[s.n++] = w;
    }
    if (s.n < 4) return false;
    // 找一个原点在内侧的子集（顺序不同结果不同，逐个试）
    const int combos[4][4] = {{0, 1, 2, 3}, {0, 1, 3, 2},
                              {0, 2, 3, 1}, {1, 2, 3, 0}};
    for (const auto& c : combos) {
        Simplex t;
        for (int i = 0; i < 4; ++i) t.p[i] = s.p[c[i]];
        t.n = 4;
        glm::vec3 dir;
        if (reduceSimplex(t, dir) && t.n == 4) {
            s = t;
            return true;
        }
    }
    return false;
}

// 返回最近且没被删掉的面
int closestFace(const std::vector<EpaFace>& faces) {
    int best = -1;
    float bestD = std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (faces[i].dead) continue;
        if (faces[i].d < bestD) {
            bestD = faces[i].d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

} // namespace

bool gjkIntersect(const TransformedShape& a, const TransformedShape& b) {
    if (!a.shape || !b.shape) return false;
    Minkowski m{&a, &b};
    Simplex s;
    return gjk(m, s);
}

Contact collide(const TransformedShape& a, const TransformedShape& b) {
    Contact c;
    if (!a.shape || !b.shape) return c;

    Minkowski m{&a, &b};
    Simplex s;
    if (!gjk(m, s)) return c;

    if (s.n < 4 && !expandToTetrahedron(m, s)) {
        // 数值退化：给一个"退回上一帧"用的最小分离量，方向取 GJK 的最后
        // 方向。比"什么都不做、眼睁睁看着穿过去"强。
        glm::vec3 d = s.n > 0 ? -s.p[0] : glm::vec3(0.0f, 1.0f, 0.0f);
        if (glm::dot(d, d) < 1e-18f) d = glm::vec3(0.0f, 1.0f, 0.0f);
        c.hit = true;
        c.normal = glm::normalize(d);
        c.depth = 1e-3f;
        c.point = glm::vec3(a.xform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        return c;
    }

    std::vector<glm::vec3> verts(s.p, s.p + 4);
    std::vector<EpaFace> faces;
    faces.reserve(64);

    // 初始 4 个面。定向准则：原点在内部，所以"外法线"必然满足 n·v > 0
    // （v 是面上任意一点）。d < 0 就翻面 —— 不依赖任何绕序假设。
    auto addFace = [&](int ia, int ib, int ic) {
        EpaFace f;
        f.v[0] = ia;
        f.v[1] = ib;
        f.v[2] = ic;
        glm::vec3 n = glm::cross(verts[static_cast<std::size_t>(ib)] -
                                     verts[static_cast<std::size_t>(ia)],
                                 verts[static_cast<std::size_t>(ic)] -
                                     verts[static_cast<std::size_t>(ia)]);
        const float len = glm::length(n);
        if (len < 1e-18f) {
            f.dead = true;
            f.d = 0.0f;
            faces.push_back(f);
            return;
        }
        n /= len;
        float d = glm::dot(n, verts[static_cast<std::size_t>(ia)]);
        if (d < 0.0f) {
            n = -n;
            d = -d;
            std::swap(f.v[1], f.v[2]);
        }
        f.normal = n;
        f.d = d;
        faces.push_back(f);
    };
    addFace(0, 1, 2);
    addFace(0, 2, 3);
    addFace(0, 3, 1);
    addFace(1, 3, 2);

    for (int iter = 0; iter < 96; ++iter) {
        const int fi = closestFace(faces);
        if (fi < 0) break;
        const glm::vec3 n = faces[static_cast<std::size_t>(fi)].normal;
        const float minD = faces[static_cast<std::size_t>(fi)].d;

        const glm::vec3 p = m.support(n);
        const float dNew = glm::dot(n, p);
        // 撑不动了 → 收敛，n 就是最小平移方向
        if (dNew - minD < 1e-5f) {
            c.hit = true;
            c.normal = n;
            c.depth = dNew;
            c.point = 0.5f * (a.support(n) + b.support(-n));
            return c;
        }

        // 可见面：p 在这些面的外侧
        std::vector<int> visible;
        for (std::size_t i = 0; i < faces.size(); ++i) {
            if (faces[i].dead) continue;
            if (glm::dot(faces[i].normal, p) > faces[i].d + 1e-9f)
                visible.push_back(static_cast<int>(i));
        }
        if (visible.empty()) {  // 数值上已经收敛
            c.hit = true;
            c.normal = n;
            c.depth = dNew;
            c.point = 0.5f * (a.support(n) + b.support(-n));
            return c;
        }

        // 边界边 = 可见集合里"反向边不在集合内"的有向边。
        // 用有向边的集合判反向，天然处理绕序不一致。
        std::vector<std::pair<int, int>> edges;
        edges.reserve(visible.size() * 3);
        for (int vf : visible) {
            for (int e = 0; e < 3; ++e) {
                edges.emplace_back(faces[static_cast<std::size_t>(vf)].v[e],
                                   faces[static_cast<std::size_t>(vf)]
                                       .v[(e + 1) % 3]);
            }
        }
        std::vector<std::pair<int, int>> horizon;
        for (const auto& e : edges) {
            bool reversedHere = false;
            for (const auto& o : edges) {
                if (o.first == e.second && o.second == e.first) {
                    reversedHere = true;
                    break;
                }
            }
            if (!reversedHere) horizon.push_back(e);
        }
        if (horizon.empty()) {  // 同上：当作收敛
            c.hit = true;
            c.normal = n;
            c.depth = dNew;
            c.point = 0.5f * (a.support(n) + b.support(-n));
            return c;
        }

        for (int vf : visible) faces[static_cast<std::size_t>(vf)].dead = true;

        const int pi = static_cast<int>(verts.size());
        verts.push_back(p);
        for (const auto& e : horizon) addFace(e.first, e.second, pi);
    }

    // 迭代打满：用最后那个最近面兜底，别把相交判成不相交
    const int fi = closestFace(faces);
    if (fi >= 0) {
        c.hit = true;
        c.normal = faces[static_cast<std::size_t>(fi)].normal;
        c.depth = faces[static_cast<std::size_t>(fi)].d;
        c.point = 0.5f * (a.support(c.normal) + b.support(-c.normal));
    }
    return c;
}

} // namespace physics
