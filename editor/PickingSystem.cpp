#include "PickingSystem.h"

#include "assets/Mesh.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "water/WaterBody.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace editor {

namespace {

constexpr float kEps = 1e-6f;

} // namespace

// ---------------------------------------------------------------- 投影

glm::mat4 PickingSystem::viewProj(const scene::Camera& camera,
                                 const glm::vec2& viewportSize) {
    const float w = std::max(viewportSize.x, 1.0f);
    const float h = std::max(viewportSize.y, 1.0f);
    return camera.projMatrix(w / h) * camera.viewMatrix();
}

Ray PickingSystem::rayFromViewport(const scene::Camera& camera,
                                   const glm::vec2& viewportPos,
                                   const glm::vec2& viewportSize,
                                   const glm::vec2& mousePos) {
    Ray r;

    const float w = std::max(viewportSize.x, 1.0f);
    const float h = std::max(viewportSize.y, 1.0f);

    const float ndcX = (mousePos.x - viewportPos.x) / w * 2.0f - 1.0f;
    const float ndcY = (mousePos.y - viewportPos.y) / h * 2.0f - 1.0f;

    const glm::mat4 inv = glm::inverse(viewProj(camera, viewportSize));

    glm::vec4 nearP = inv * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farP = inv * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (std::fabs(nearP.w) < kEps || std::fabs(farP.w) < kEps) {
        r.origin = camera.position();
        r.dir = glm::normalize(camera.target() - camera.position());
        return r;
    }
    nearP /= nearP.w;
    farP /= farP.w;

    r.origin = glm::vec3(nearP);
    const glm::vec3 d = glm::vec3(farP) - glm::vec3(nearP);
    r.dir = glm::length(d) > kEps ? glm::normalize(d) : glm::vec3(0, 0, -1);
    return r;
}

bool PickingSystem::worldToScreen(const glm::mat4& vp,
                                  const glm::vec2& viewportPos,
                                  const glm::vec2& viewportSize,
                                  const glm::vec3& world, glm::vec2& out) {
    const glm::vec4 clip = vp * glm::vec4(world, 1.0f);
    if (clip.w <= kEps) return false;  // 在相机背后

    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    out.x = viewportPos.x + (ndc.x * 0.5f + 0.5f) * viewportSize.x;
    out.y = viewportPos.y + (ndc.y * 0.5f + 0.5f) * viewportSize.y;
    return true;
}

// ---------------------------------------------------------------- 求交原语

bool PickingSystem::intersectPlane(const Ray& r, const glm::vec3& planePoint,
                                   const glm::vec3& planeNormal, float& t) {
    const float denom = glm::dot(planeNormal, r.dir);
    if (std::fabs(denom) < kEps) return false;  // 射线与平面平行
    t = glm::dot(planePoint - r.origin, planeNormal) / denom;
    return true;
}

// 射线与"过 lineOrigin、方向 lineAxis"的**直线**的最近点，返回它在直线上的参数
// （param = 0 就是 lineOrigin，单位是米）。
//
// 推导（教科书 closest-points-between-two-lines，Ericson 5.1.9）：
//   直线1  P(s) = lineOrigin + s*d1      （d1 = lineAxis）
//   直线2  Q(t) = r.origin  + t*d2       （d2 = r.dir）
//   令 r0 = lineOrigin - r.origin，则
//     a = d1·d1   b = d1·d2   c = d2·d2   d = d1·r0   e = d2·r0
//     s = (b*e - c*d) / (a*c - b*b)
//
// ⚠ 这里曾经写成 (b*e - c*d) / (b*b - a*c) —— 分母取了相反数，等于整个
// 参数被取反，表现为**拖拽方向是反的**（往右拖物体往左跑）。分母保留
// 原式的 a*c - b*b，判平行用它的相反数即可，不必再自己翻符号。
bool PickingSystem::closestOnAxis(const Ray& r, const glm::vec3& lineOrigin,
                                  const glm::vec3& lineAxis, float& param) {
    const glm::vec3 d1 = lineAxis;   // 轴
    const glm::vec3 d2 = r.dir;      // 射线
    const glm::vec3 w0 = lineOrigin - r.origin;

    const float a = glm::dot(d1, d1);
    const float b = glm::dot(d1, d2);
    const float c = glm::dot(d2, d2);
    const float d = glm::dot(w0, d1);
    const float e = glm::dot(w0, d2);

    // a*c - b*b = |d1|²|d2|²(1 - cos²θ) ≤ 0，在两条线平行时退化
    const float denom = b * b - a * c;
    if (std::fabs(denom) < 1e-8f) return false;  // 平行（几乎正对轴看过去）

    param = (c * d - b * e) / denom;             // = (b*e - c*d) / (a*c - b*b)
    return true;
}

float PickingSystem::screenDistanceToSegment(const glm::vec2& p,
                                             const glm::vec2& a,
                                             const glm::vec2& b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1e-6f) return glm::length(p - a);
    float t = glm::dot(p - a, ab) / len2;
    t = glm::clamp(t, 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

// ---------------------------------------------------------------- AABB

Aabb PickingSystem::meshAabb(const assets::Mesh& mesh) const {
    auto it = m_meshCache.find(&mesh);
    if (it != m_meshCache.end()) return it->second;

    Aabb b;
    const auto& verts = mesh.vertices();
    if (!verts.empty()) {
        b.min = verts[0].pos;
        b.max = verts[0].pos;
        for (const auto& v : verts) {
            b.min = glm::min(b.min, v.pos);
            b.max = glm::max(b.max, v.pos);
        }
        b.valid = true;
    }
    m_meshCache.emplace(&mesh, b);
    return b;
}

Aabb PickingSystem::worldAabb(scene::Scene& scene, ecs::Entity e) const {
    Aabb out;
    const auto* mc = scene.world().get<ecs::MeshComponent>(e);
    if (!mc || !mc->mesh) return out;

    const Aabb local = meshAabb(*mc->mesh);
    if (!local.valid) return out;

    // 局部 AABB 的 8 个角点过一遍世界矩阵，再取包围
    const glm::mat4 m = scene.worldMatrix(e);
    const glm::vec3 c[8] = {
        {local.min.x, local.min.y, local.min.z},
        {local.max.x, local.min.y, local.min.z},
        {local.min.x, local.max.y, local.min.z},
        {local.max.x, local.max.y, local.min.z},
        {local.min.x, local.min.y, local.max.z},
        {local.max.x, local.min.y, local.max.z},
        {local.min.x, local.max.y, local.max.z},
        {local.max.x, local.max.y, local.max.z},
    };

    out.min = glm::vec3(m * glm::vec4(c[0], 1.0f));
    out.max = out.min;
    for (int i = 1; i < 8; ++i) {
        const glm::vec3 p = glm::vec3(m * glm::vec4(c[i], 1.0f));
        out.min = glm::min(out.min, p);
        out.max = glm::max(out.max, p);
    }
    out.valid = true;
    return out;
}

// ---------------------------------------------------------------- 拾取

PickResult PickingSystem::pick(scene::Scene& scene, const Ray& ray) {
    PickResult best;

    // 走场景的**空间索引（BVH）**做射线遍历。
    //
    // 老实现是线性扫描：每个实体先算世界 AABB（8 个角点过世界矩阵）再
    // 做射线测试 —— 场景一上量，"点一下选中物体"就明显发涩。BVH 把它
    // 变成 O(log N)：节点包围盒没被射线碰到，整棵子树直接跳过。
    //
    // 上锁的实体（自带地面）不参与点选：它是参照物，不该被选中 / 变换。
    // 交给叶子过滤器跳过 —— 语义与老版"跳过而不是排在最后"完全一致。
    const scene::Bvh& bvh = scene.spatialIndex();
    const scene::BvhHit hit = bvh.raycastFiltered(
        scene::BvhRay{ray.origin, ray.dir}, [&scene](ecs::Entity e) {
            return !scene.world().has<ecs::LockedComponent>(e);
        });

    // 先落结果，再自检：自检要比较的就是 best 与线性扫描 —— 顺序反了
    // same 会恒为 0（best 还是空的）。
    if (hit.hit) {
        best.hit = true;
        best.entity = hit.entity;
        best.distance = hit.distance;
        best.point = ray.origin + ray.dir * hit.distance;
    }

    // ---- 自检（MYVK_BVH_DBG=1）：BVH 的最近命中必须与"逐个实体线性扫描"
    //      完全一致。拾取是"点一下选错人"类的 bug 最容易藏身的地方，
    //      而它只在点击时触发、平时看不出来，所以留一条可验证的对照。
    //
    // ⚠ 位置必须在"没打中就直接 return"**之前**。放在后面的话，"点空地"
    //   这一路完全不出日志，验证脚本就分不清两种情况：射线本来就没打中
    //   （场景/机位问题），还是自检压根没跑到（回归失败最难查的一类）。
    //   实测就是被这条坑过：verify_bvh 报 `PICK-DBG : None`，无从判断。
    if (std::getenv("MYVK_BVH_DBG")) {
        glm::vec3 d = ray.dir;
        for (int i = 0; i < 3; ++i)
            if (d[i] == 0.0f) d[i] = 1e-20f;
        const glm::vec3 inv = 1.0f / d;
        float linBest = std::numeric_limits<float>::max();
        ecs::Entity linEntity{};
        bool linHit = false;
        scene.forEachRenderable([&](ecs::Entity e, ecs::MeshComponent&,
                                    ecs::MaterialComponent&) {
            if (scene.world().has<ecs::LockedComponent>(e)) return;
            const Aabb b = worldAabb(scene, e);
            if (!b.valid) return;
            float t = 0.0f;
            if (!scene::Bvh::intersectAabb(ray.origin, inv,
                                           scene::BvhAabb{b.min, b.max}, t))
                return;
            if (t < linBest) {
                linBest = t;
                linEntity = e;
                linHit = true;
            }
        });
        const bool same = (linHit == best.hit) &&
                          (!best.hit || linEntity.id == best.entity.id);
        VK_LOG_INFO("PICK-DBG bvh=%u(d=%.3f) linear=%u(d=%.3f) same=%d "
                    "rayNodes=%u rayLeafTests=%u hit=%d",
                    best.entity.id, static_cast<double>(best.distance),
                    linEntity.id, static_cast<double>(linBest),
                    same ? 1 : 0, bvh.lastRayNodeVisits(),
                    bvh.lastRayLeafTests(), best.hit ? 1 : 0);
    }

    // ---- 水体：没有网格 → 不在 BVH 里，单独按"水面圆/矩形"求交 ----
    // 不补这一步，创建之后只能在层级里选中它、视口里点不到 —— 用户要的
    // "创建后直接在水上拖"就缺了半截。
    //
    // 放在自检**之后**：PICK-DBG 对照的是"BVH vs 线性扫描（只有带网格的
    // 实体）"，水体本来就不参与那一对照，混进来会让 same 恒为 0。
    {
        float bestWaterT = best.hit ? best.distance
                                    : std::numeric_limits<float>::max();
        ecs::Entity bestWater{};
        scene.forEachWater([&](ecs::Entity e, ecs::WaterComponent& wc) {
            if (scene.world().has<ecs::LockedComponent>(e)) return;
            const water::WaterBody& b = wc.body;

            float t = 0.0f;
            if (!intersectPlane(ray,
                                glm::vec3(b.center.x, b.center.y, b.level),
                                glm::vec3(0.0f, 0.0f, 1.0f), t))
                return;
            if (t <= 0.0f || t >= bestWaterT) return;

            const glm::vec3 P = ray.origin + ray.dir * t;
            // 落到局部坐标（逆旋转）再比 footprint：圆看半径，矩形看半长
            const glm::vec2 d(P.x - b.center.x, P.y - b.center.y);
            const float rad = glm::radians(b.rotation);
            const float c = std::cos(rad);
            const float s = std::sin(rad);
            const glm::vec2 l(d.x * c + d.y * s, -d.x * s + d.y * c);
            const bool inside =
                (b.shape == water::Shape::Circle)
                    ? glm::length(l) <= b.halfExtent.x
                    : (std::fabs(l.x) <= b.halfExtent.x &&
                       std::fabs(l.y) <= b.halfExtent.y);
            if (!inside) return;

            bestWaterT = t;
            bestWater = e;
        });
        const float limit =
            best.hit ? best.distance : std::numeric_limits<float>::max();
        if (bestWater.valid() && bestWaterT < limit) {
            best.hit = true;
            best.entity = bestWater;
            best.distance = bestWaterT;
            best.point = ray.origin + ray.dir * bestWaterT;
        }
    }

    // ---- GameStart 出生点：视口里只有一个**叠加层小人**（没有网格），
    //      所以也必须在 BVH 之外单独求交，否则鼠标点不中它 ——
    //      点不中就没有手柄、数字键 0 的"落地"也会被
    //      `selectionFromViewport()` 挡掉（用户报的正是这两件事）。
    //
    // 形状用"竖直胶囊"近似那个小人：半径 0.45 m、从脚下到头顶 1.75 m。
    // 比真实像素轮廓宽容一点是对的 —— 这是个标记，不是可碰撞的物体。
    {
        float bestGsT = best.hit ? best.distance
                                 : std::numeric_limits<float>::max();
        ecs::Entity bestGs{};
        scene.world().each<ecs::GameStartComponent>(
            [&](ecs::Entity e, ecs::GameStartComponent& g) {
                if (!g.enabled) return;
                if (scene.world().has<ecs::LockedComponent>(e)) return;
                const auto* tf = scene.world().get<ecs::TransformComponent>(e);
                if (!tf) return;

                const glm::vec3 c = tf->position + glm::vec3(0.0f, 0.0f, 0.875f);
                const glm::vec3 base = tf->position;
                const glm::vec3 axis(0.0f, 0.0f, 1.0f);
                // 无限圆柱求交 → 再夹到 [0, 1.75] 的高度段
                const glm::vec3 oc = ray.origin - base;
                const glm::vec3 dPerp = ray.dir - axis * glm::dot(ray.dir, axis);
                const glm::vec3 ocPerp = oc - axis * glm::dot(oc, axis);
                const float a = glm::dot(dPerp, dPerp);
                const float b = 2.0f * glm::dot(dPerp, ocPerp);
                const float cc = glm::dot(ocPerp, ocPerp) - 0.45f * 0.45f;
                if (a < 1e-8f) return;            // 射线与轴平行
                const float disc = b * b - 4.0f * a * cc;
                if (disc < 0.0f) return;
                const float sq = std::sqrt(disc);
                float t = (-b - sq) / (2.0f * a);
                if (t <= 0.0f) t = (-b + sq) / (2.0f * a);
                if (t <= 0.0f || t >= bestGsT) return;
                const float h = glm::dot(ray.origin + ray.dir * t - base, axis);
                if (h < 0.0f || h > 1.75f) return;
                bestGsT = t;
                bestGs = e;
            });
        const float gsLimit =
            best.hit ? best.distance : std::numeric_limits<float>::max();
        if (bestGs.valid() && bestGsT < gsLimit) {
            best.hit = true;
            best.entity = bestGs;
            best.distance = bestGsT;
            best.point = ray.origin + ray.dir * bestGsT;
        }
    }

    return best;
}

} // namespace editor
