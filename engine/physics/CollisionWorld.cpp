// ============================================================
// physics/CollisionWorld.cpp —— 场景级碰撞体生成 + 求解
// ============================================================

#include "physics/CollisionWorld.h"

#include "assets/Mesh.h"
#include "physics/ConvexCollision.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace physics {
namespace {

// 网格局部 AABB。优先用 Mesh 上传时算好的那份（顶点数据在 CPU 侧也留着，
// 但没必要每次重扫）。
void meshBounds(const assets::Mesh& mesh, glm::vec3& mn, glm::vec3& mx) {
    if (mesh.hasBounds()) {
        mn = mesh.boundsMin();
        mx = mesh.boundsMax();
        return;
    }
    mn = mx = glm::vec3(0.0f);
    bool first = true;
    for (const assets::Vertex& v : mesh.vertices()) {
        if (first) {
            mn = mx = v.pos;
            first = false;
        } else {
            mn = glm::min(mn, v.pos);
            mx = glm::max(mx, v.pos);
        }
    }
}

std::vector<glm::vec3> boxCorners(const glm::vec3& mn, const glm::vec3& mx) {
    std::vector<glm::vec3> c(8);
    for (int i = 0; i < 8; ++i) {
        c[static_cast<std::size_t>(i)] =
            glm::vec3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y,
                      (i & 4) ? mx.z : mn.z);
    }
    return c;
}

// 立方体 8 个角的标准三角面（线框要用）。角点编号与 boxCorners 一致：
// bit0 = x, bit1 = y, bit2 = z。
std::vector<glm::uvec3> boxFaces() {
    static const std::uint32_t tri[12][3] = {
        {0, 2, 6}, {0, 6, 4},  // -x
        {1, 5, 7}, {1, 7, 3},  // +x
        {0, 4, 5}, {0, 5, 1},  // -y
        {2, 3, 7}, {2, 7, 6},  // +y
        {0, 1, 3}, {0, 3, 2},  // -z
        {4, 6, 7}, {4, 7, 5},  // +z
    };
    std::vector<glm::uvec3> out;
    out.reserve(12);
    for (const auto& t : tri) out.emplace_back(t[0], t[1], t[2]);
    return out;
}

bool aabbOverlap(const Collider& a, const Collider& b, float margin = 0.0f) {
    for (int k = 0; k < 3; ++k) {
        if (a.aabbMin[k] > b.aabbMax[k] + margin) return false;
        if (b.aabbMin[k] > a.aabbMax[k] + margin) return false;
    }
    return true;
}

// 实体的父节点世界矩阵（没有父节点 = 单位阵）
glm::mat4 parentWorld(const scene::Scene& scene, ecs::Entity e) {
    const auto* h = scene.world().get<ecs::HierarchyComponent>(e);
    if (!h || h->parent == ecs::kInvalidEntity) return glm::mat4(1.0f);
    return scene.worldMatrix(scene.world().handle(h->parent));
}

} // namespace

// ---------------------------------------------------------------- 生成

bool buildHullCollider(const assets::Mesh& mesh, ConvexShape& shape,
                       glm::vec3& position, glm::vec3& rotation) {
    shape = ConvexShape{};
    shape.kind = ConvexShape::Kind::Hull;
    position = glm::vec3(0.0f);
    rotation = glm::vec3(0.0f);

    std::vector<glm::vec3> pts;
    pts.reserve(mesh.vertices().size());
    for (const assets::Vertex& v : mesh.vertices()) pts.push_back(v.pos);
    if (pts.empty()) return false;

    ConvexHull hull = buildConvexHull(pts, 1e-4f);

    if (hull.empty()) {
        // 退化输入（共面 / 共线 / 顶点太少）——典型例子是内置 Plane。
        // 兜底成一块薄板：AABB 在"厚度不足 2cm"的维度上撑开到 2cm。
        // 这样它仍然是一个合法的三维凸体（GJK/EPA 在零体积的集合上会
        // 退化），视觉上也就是一块很薄的碰撞板。
        glm::vec3 mn, mx;
        meshBounds(mesh, mn, mx);
        constexpr float kMinThickness = 0.02f;
        for (int a = 0; a < 3; ++a) {
            if (mx[a] - mn[a] >= kMinThickness) continue;
            const float c = 0.5f * (mn[a] + mx[a]);
            mn[a] = c - kMinThickness * 0.5f;
            mx[a] = c + kMinThickness * 0.5f;
        }
        position = (mn + mx) * 0.5f;
        shape.hull = boxCorners(mn, mx);
        shape.hullFaces = boxFaces();
        for (glm::vec3& p : shape.hull) p -= position;
        return true;
    }

    // 居中：凸包顶点整体平移 -重心（AABB 中心），平移量记进 position。
    // 于是"碰撞体局部 TRS = 单位阵"时，碰撞体位置 = 刚好包住模型。
    const glm::vec3 center = 0.5f * (hull.minBound + hull.maxBound);
    position = center;
    shape.hull.reserve(hull.points.size());
    for (const glm::vec3& p : hull.points) shape.hull.push_back(p - center);
    shape.hullFaces.reserve(hull.faces.size());
    for (const ConvexHull::Face& f : hull.faces)
        shape.hullFaces.emplace_back(f.a, f.b, f.c);
    return true;
}

void buildCapsuleCollider(const assets::Mesh& mesh, ConvexShape& shape,
                          glm::vec3& position, glm::vec3& rotation) {
    shape = ConvexShape{};
    shape.kind = ConvexShape::Kind::Capsule;
    rotation = glm::vec3(0.0f);

    glm::vec3 mn, mx;
    meshBounds(mesh, mn, mx);
    const glm::vec3 half = 0.5f * (mx - mn);
    position = 0.5f * (mn + mx);

    // 轴 = AABB 最长的那一维
    int axis = 2;
    if (half.x > half[axis]) axis = 0;
    if (half.y > half[axis]) axis = 1;

    // 半径 = 另外两维半长的外接圆半径。外接（而不是内切）是为了保证胶囊
    // **整个盖住** AABB —— 内切的话立方体的角会露在胶囊外面。
    float r2 = 0.0f;
    for (int k = 0; k < 3; ++k)
        if (k != axis) r2 += half[k] * half[k];
    const float r = std::max(std::sqrt(r2), 1e-3f);

    shape.radius = r;
    shape.halfHeight = std::max(half[axis] - r, 0.0f);

    // 胶囊的轴固定在局部 +Z，用旋转把它转到最长的那一维
    if (axis == 0) {
        rotation = glm::vec3(0.0f, glm::radians(90.0f), 0.0f);  // +Z → +X
    } else if (axis == 1) {
        rotation = glm::vec3(glm::radians(-90.0f), 0.0f, 0.0f);  // +Z → +Y
    }
}

// ---------------------------------------------------------------- 装配

namespace {

// ConvexShape 的面 → 组件用的去重无向边
std::vector<glm::uvec2> edgesOf(const ConvexShape& shape) {
    std::vector<glm::uvec2> out;
    out.reserve(shape.hullFaces.size() * 3 / 2);
    std::unordered_map<std::uint64_t, bool> seen;
    seen.reserve(shape.hullFaces.size() * 2);
    for (const glm::uvec3& f : shape.hullFaces) {
        const std::uint32_t tri[3] = {f.x, f.y, f.z};
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

} // namespace

bool meshAabb(const assets::Mesh& mesh, glm::vec3& mn, glm::vec3& mx) {
    if (mesh.vertices().empty()) return false;
    meshBounds(mesh, mn, mx);
    return true;
}

bool colliderLocalAabb(const ecs::CollisionComponent& cc, glm::vec3& mn,
                       glm::vec3& mx) {
    ConvexShape shape;
    shape.kind = (cc.shape == ecs::ColliderShape::ConvexHull)
                     ? ConvexShape::Kind::Hull
                     : ConvexShape::Kind::Capsule;
    shape.radius = cc.capsuleRadius;
    shape.halfHeight = cc.capsuleHalfHeight;
    shape.hull = cc.hullPoints;
    if (shape.kind == ConvexShape::Kind::Hull && shape.hull.empty())
        return false;
    if (shape.kind == ConvexShape::Kind::Capsule && shape.radius <= 0.0f)
        return false;

    glm::vec3 lmn, lmx;
    shape.localAabb(lmn, lmx);
    const glm::mat4 m = cc.localMatrix();

    mn = mx = glm::vec3(m * glm::vec4(lmn, 1.0f));
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 c((i & 1) ? lmx.x : lmn.x, (i & 2) ? lmx.y : lmn.y,
                          (i & 4) ? lmx.z : lmn.z);
        const glm::vec3 w(m * glm::vec4(c, 1.0f));
        mn = glm::min(mn, w);
        mx = glm::max(mx, w);
    }
    return true;
}

bool setupHullCollider(const assets::Mesh& mesh, ecs::CollisionComponent& cc) {
    ConvexShape shape;
    glm::vec3 pos(0.0f), rot(0.0f);
    if (!buildHullCollider(mesh, shape, pos, rot)) return false;
    cc.shape = ecs::ColliderShape::ConvexHull;
    cc.position = pos;
    cc.rotation = glm::vec3(0.0f);
    cc.scale = glm::vec3(1.0f);
    cc.hullPoints = std::move(shape.hull);
    cc.hullEdges = edgesOf(shape);
    return true;
}

bool refreshHullGeometry(const assets::Mesh& mesh, ecs::CollisionComponent& cc) {
    ConvexShape shape;
    glm::vec3 pos(0.0f), rot(0.0f);
    if (!buildHullCollider(mesh, shape, pos, rot)) return false;
    cc.hullPoints = std::move(shape.hull);
    cc.hullEdges = edgesOf(shape);
    return true;
}

bool setupCapsuleCollider(const assets::Mesh& mesh,
                          ecs::CollisionComponent& cc) {
    if (mesh.vertices().empty()) return false;
    ConvexShape shape;
    glm::vec3 pos(0.0f), rot(0.0f);
    buildCapsuleCollider(mesh, shape, pos, rot);
    cc.shape = ecs::ColliderShape::Capsule;
    cc.position = pos;
    cc.rotation = rot;
    cc.scale = glm::vec3(1.0f);
    cc.capsuleRadius = shape.radius;
    cc.capsuleHalfHeight = shape.halfHeight;
    cc.hullPoints.clear();
    cc.hullEdges.clear();
    return true;
}


bool buildCollider(scene::Scene& scene, ecs::Entity e, Collider& out) {
    ecs::World& w = scene.world();
    const auto* cc = w.get<ecs::CollisionComponent>(e);
    if (!cc) return false;

    out = Collider{};
    out.entity = e;
    out.solid = cc->solid;
    out.shape.kind = (cc->shape == ecs::ColliderShape::ConvexHull)
                         ? ConvexShape::Kind::Hull
                         : ConvexShape::Kind::Capsule;
    out.shape.radius = cc->capsuleRadius;
    out.shape.halfHeight = cc->capsuleHalfHeight;
    out.shape.hull = cc->hullPoints;

    if (out.shape.kind == ConvexShape::Kind::Hull) {
        if (out.shape.hull.empty()) {
            refreshColliderGeometry(scene, e);
            out.shape.hull = cc->hullPoints;
        }
        if (out.shape.hull.empty()) return false;
        // 线的面只在可视化时用；这里不复刻一份，绘制侧从 CollisionComponent
        // 直接取（见 ColliderWireframe）
    } else if (out.shape.radius <= 0.0f) {
        return false;
    }

    out.world = scene.worldMatrix(e) * cc->localMatrix();
    glm::vec3 mn, mx;
    TransformedShape ts{&out.shape, out.world};
    ts.worldAabb(mn, mx);
    out.aabbMin = mn;
    out.aabbMax = mx;
    return true;
}

void refreshColliderGeometry(scene::Scene& scene, ecs::Entity e) {
    ecs::World& w = scene.world();
    auto* cc = w.get<ecs::CollisionComponent>(e);
    if (!cc) return;
    if (cc->shape != ecs::ColliderShape::ConvexHull) return;
    const auto* mc = w.get<ecs::MeshComponent>(e);
    if (!mc || !mc->mesh) return;

    // 只在"还没生成过"时套用默认摆放 —— 否则读盘会把用户调过的
    // position/rotation 覆盖掉（refreshHullGeometry 只动几何）。
    const bool firstTime = cc->hullPoints.empty();
    ConvexShape shape;
    glm::vec3 pos(0.0f), rot(0.0f);
    if (!buildHullCollider(*mc->mesh, shape, pos, rot)) return;
    if (firstTime) cc->position = pos;
    cc->hullPoints = std::move(shape.hull);

    std::vector<glm::uvec2> edges;
    edges.reserve(shape.hullFaces.size() * 3 / 2);
    {
        std::unordered_map<std::uint64_t, bool> seen;
        for (const glm::uvec3& f : shape.hullFaces) {
            const std::uint32_t tri[3] = {f.x, f.y, f.z};
            for (int k = 0; k < 3; ++k) {
                std::uint32_t u = tri[k];
                std::uint32_t v = tri[(k + 1) % 3];
                if (u > v) std::swap(u, v);
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(u) << 32) | v;
                if (seen.emplace(key, true).second) edges.emplace_back(u, v);
            }
        }
    }
    cc->hullEdges = std::move(edges);
}

void collectColliders(scene::Scene& scene, std::vector<Collider>& out) {
    out.clear();
    std::vector<ecs::Entity> ents;
    scene.world().each<ecs::CollisionComponent>(
        [&](ecs::Entity e, ecs::CollisionComponent& cc) {
            // 读盘后 hullPoints 还是空的 → 现场补一次
            if (cc.shape == ecs::ColliderShape::ConvexHull && cc.hullPoints.empty())
                refreshColliderGeometry(scene, e);
            ents.push_back(e);
        });

    out.reserve(ents.size());
    for (ecs::Entity e : ents) {
        Collider c;
        if (buildCollider(scene, e, c)) out.push_back(std::move(c));
    }
}

// ---------------------------------------------------------------- 求解

void translateEntityWorld(scene::Scene& scene, ecs::Entity e,
                          const glm::vec3& worldDelta) {
    auto* t = scene.world().get<ecs::TransformComponent>(e);
    if (!t) return;
    const glm::mat4 pw = parentWorld(scene, e);
    const glm::mat3 basis(pw);
    // 父节点没有缩放/旋转时 basis 是单位阵，逆就是它自己 —— 绝大多数
    // 场景（顶层物体）走的是这条最快路径。
    const glm::mat3 inv = glm::inverse(basis);
    t->position += inv * worldDelta;
}

int resolveBodyOverlaps(scene::Scene& scene, int iterations,
                        std::vector<BodyContact>* contacts) {
    std::vector<Collider> cols;
    collectColliders(scene, cols);

    std::size_t solidCount = 0;
    for (const Collider& c : cols)
        if (c.solid) ++solidCount;
    if (solidCount < 2) return 0;

    int touched = 0;
    for (int it = 0; it < iterations; ++it) {
        bool any = false;
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (!cols[i].solid) continue;
            for (std::size_t j = i + 1; j < cols.size(); ++j) {
                if (!cols[j].solid) continue;
                if (!aabbOverlap(cols[i], cols[j])) continue;

                TransformedShape sa{&cols[i].shape, cols[i].world};
                TransformedShape sb{&cols[j].shape, cols[j].world};
                const Contact c = collide(sa, sb);
                if (!c.hit || c.depth <= 1e-5f) continue;

                // 各推一半：不区分动静（这个引擎还没有刚体 / 质量的概念，
                // "两个都能动"是最不容易看出破绽的默认行为）
                const glm::vec3 half = c.normal * (c.depth * 0.5f);
                translateEntityWorld(scene, cols[i].entity, -half);
                translateEntityWorld(scene, cols[j].entity, +half);

                // 位移之后世界矩阵变了，立刻刷新这两个碰撞体，避免同一轮
                // 里用旧矩阵重复判同一对
                Collider ti, tj;
                if (buildCollider(scene, cols[i].entity, ti)) cols[i] = ti;
                if (buildCollider(scene, cols[j].entity, tj)) cols[j] = tj;

                if (contacts) {
                    contacts->push_back(BodyContact{cols[i].entity,
                                                    cols[j].entity, c.depth,
                                                    c.normal});
                }
                any = true;
                if (it == 0) ++touched;
            }
        }
        if (!any) break;
    }
    return touched;
}

bool resolveCamera(scene::Scene& scene, scene::Camera& cam, float radius) {
    std::vector<Collider> cols;
    collectColliders(scene, cols);
    if (cols.empty()) return false;

    ConvexShape ball;
    ball.kind = ConvexShape::Kind::Sphere;
    ball.radius = std::max(radius, 1e-3f);

    bool moved = false;
    glm::vec3 pos = cam.position();
    // 几轮迭代：被多个碰撞体包夹时（墙角）一轮推不干净
    for (int it = 0; it < 4; ++it) {
        bool any = false;
        for (const Collider& c : cols) {
            if (!c.solid) continue;
            // 粗筛：相机球 AABB 与碰撞体 AABB
            bool skip = false;
            for (int k = 0; k < 3; ++k) {
                if (pos[k] - ball.radius > c.aabbMax[k] ||
                    pos[k] + ball.radius < c.aabbMin[k]) {
                    skip = true;
                    break;
                }
            }
            if (skip) continue;

            TransformedShape ss{
                &ball, glm::translate(glm::mat4(1.0f), pos)};
            TransformedShape sc{&c.shape, c.world};
            const Contact ct = collide(ss, sc);
            if (!ct.hit || ct.depth <= 0.0f) continue;

            // normal 是"球 → 碰撞体"，所以球要往 -normal 挪
            pos -= ct.normal * (ct.depth + 1e-4f);
            any = true;
            moved = true;
        }
        if (!any) break;
    }
    if (moved) cam.setPosition(pos);
    return moved;
}

} // namespace physics
