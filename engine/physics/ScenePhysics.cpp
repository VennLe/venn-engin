// glm::decompose 被标成 "experimental"（GTX 扩展），必须先开这个宏。
// 项目里另有 rhi/VulkanCommon.h 与 assets/Skeleton.cpp 也是这么做的。
//
// ⚠ 必须写在**任何 glm 头文件之前** —— 包括本文件自己的头文件
//   （ScenePhysics.h → PhysicsWorld.h → RigidBody.h 一路会拉进 glm）。
//   顺序反了宏就不生效，报的是
//   "GLM_GTX_matrix_decompose is an experimental extension"。
#define GLM_ENABLE_EXPERIMENTAL

#include "physics/ScenePhysics.h"

#include "core/Logger.h"
#include "physics/CollisionWorld.h"   // meshAabb（网格 AABB 兜底）
#include "ecs/Components.h"
#include "scene/Scene.h"
#include "terrain/Terrain.h"          // 地形高度场（见 attachTerrain）

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_inverse.hpp>
// glm::decompose 在 gtx 里（不在 gtc）—— 少了这行会报
// "decompose 不是 glm 的成员"，而错误位置指向 matrix_inverse.inl，很难懂。
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>

namespace physics {

namespace {

// 从任意仿射矩阵里拆出 TRS（glm 的 decompose 需要这几个出参）
struct Decomposed {
    glm::vec3 scale{1.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 translation{0.0f};
};

Decomposed decomposeMatrix(const glm::mat4& m) {
    Decomposed d;
    glm::vec3 skew(0.0f);
    glm::vec4 persp(0.0f);
    glm::decompose(m, d.scale, d.rotation, d.translation, skew, persp);
    d.rotation = glm::normalize(d.rotation);
    return d;
}

// ---------------------------------------------------------------- 地形
//
// 把场景里第一块 `TerrainComponent` 读成 CPU 高度场，挂到物理世界上。
//
// 为什么必须在这里做（而不是让 editor / runtime 各接一次）：地形是
// **物理地面**这件事对"编辑器 Play"和"打包出的游戏"必须是同一套 ——
// 分两处实现就一定会漂移（本文件头那段"两份实现必然漂移"的老账）。
//
// 代价是 `physics/` 反向 include 了 `terrain/`。但方向本来就有依赖
// （`terrain/Terrain.h` include `physics/Heightfield.h`），且两者在同一个
// 目标里，所以只是头文件层面的交叉，不构成编译环：`Terrain.h` 不会
// include 本文件。
//
// ⚠ 只取第一块地形。多块地形的场景（拼接的大世界）暂时标注在日志里，
//   而不是悄悄只认一块 —— "为什么站在西边那块地上会掉下去"必须有据可查。
void attachTerrain(scene::Scene& scene, SceneBinding& out) {
    ecs::Entity first{};
    int count = 0;
    scene.forEachTerrain([&](ecs::Entity e, ecs::TerrainComponent& tc) {
        if (!tc.visible) return;
        ++count;
        if (!first.valid()) first = e;
    });
    out.terrains = count;
    if (!first.valid()) return;

    auto* tc = scene.world().get<ecs::TerrainComponent>(first);
    if (!tc) return;

    auto asset = std::make_unique<terrain::TerrainAsset>();
    if (!terrain::loadTerrain(tc->source, *asset, nullptr, /*quiet=*/true)) {
        VK_LOG_WARN("PHYS-TERRAIN-FAIL source=%s（地形不参与碰撞）",
                    tc->source.c_str());
        return;
    }

    // `heightfield()` 里已经按 maxHeight 换算过（归一化 → 米）；这里再乘
    // 组件上的 `heightScale` 与实体缩放 —— 与渲染侧 `TerrainPass` 的口径
    // 一致（那边是 `maxHeight × heightScale`，实体缩放由 model 矩阵带）。
    physics::Heightfield hf = asset->heightfield();
    // 实体自身的缩放/位移也要带上（地形被整体抬高一截的场景是合法的）。
    // 高度场只支持"XY 平移 + 等比缩放"，所以这里只取平移与平均缩放。
    const Decomposed d = decomposeMatrix(scene.worldMatrix(first));
    const float s = (d.scale.x + d.scale.y) * 0.5f;
    hf.origin = glm::vec2(d.translation) + glm::vec2(hf.origin) * s;
    hf.metersPerPixel *= s;
    hf.heightScale *= s * std::max(tc->heightScale, 1.0e-4f);

    out.terrainHeights = asset->heights;   // 拷贝：asset 在本函数末尾就没了
    out.terrainHf = std::make_unique<Heightfield>(hf);
    out.terrainHf->heights = out.terrainHeights.data();
    out.world->setHeightfield(out.terrainHf.get());

    VK_LOG_INFO("PHYS-TERRAIN source=%s terrains=%d cols=%d rows=%d "
                "mpp=%.2f maxh=%.2f origin=(%.1f,%.1f)",
                tc->source.c_str(), count, hf.cols, hf.rows,
                static_cast<double>(hf.metersPerPixel),
                static_cast<double>(hf.heightScale),
                static_cast<double>(hf.origin.x),
                static_cast<double>(hf.origin.y));
}

} // namespace

// ---------------------------------------------------------------- 形状缩放

void scaleShapeForWorld(Shape& s, const glm::vec3& sc) {
    switch (s.kind) {
        case ShapeKind::Sphere:
            s.convex.radius *= std::max({sc.x, sc.y, sc.z});
            break;
        case ShapeKind::Box:
            s.convex.halfExtents *= sc;
            break;
        case ShapeKind::Capsule:
            // 半径取横截面的较大者（胶囊会跟着被拉粗），半高只吃 Z。
            s.convex.radius *= std::max(sc.x, sc.y);
            s.convex.halfHeight *= std::fabs(sc.z);
            break;
        case ShapeKind::ConvexHull:
            for (glm::vec3& p : s.convex.hull) p *= sc;
            break;
        case ShapeKind::Plane:
            break;   // 无限半空间，缩放无意义
    }
    // 退化缩放（某个轴被压成 0）会让形状非法 → 给个下限，宁可"碰撞体
    // 比模型大一点"，也不要一个零体积的体卡进求解器。
    if (s.kind == ShapeKind::Box)
        s.convex.halfExtents = glm::max(s.convex.halfExtents, glm::vec3(1.0e-3f));
    if (s.kind == ShapeKind::Sphere || s.kind == ShapeKind::Capsule)
        s.convex.radius = std::max(s.convex.radius, 1.0e-3f);
}

// ---------------------------------------------------------------- 欧拉角

glm::vec3 eulerXYZFromMatrix(const glm::mat3& m) {
    // 展开 R = Rx·Ry·Rz：（记 sa=sin x 等）
    //     [ cb·cc          -cb·sc          sb    ]
    //     [ sa·sb·cc+ca·sc -sa·sb·sc+ca·cc -sa·cb ]
    //     [ -ca·sb·cc+sa·sc ca·sb·sc+sa·cc  ca·cb ]
    // 于是 y = asin(R02)、x = atan2(-R12, R22)、z = atan2(-R01, R00)。
    // glm 是列主序（m[col][row]），所以 R(r,c) = m[c][r]。
    const float sb = glm::clamp(m[2][0], -1.0f, 1.0f);   // R(0,2)
    const float cb = std::sqrt(std::max(0.0f, 1.0f - sb * sb));
    glm::vec3 e(0.0f);
    e.y = std::asin(sb);
    if (cb > 1.0e-5f) {
        e.x = std::atan2(-m[2][1], m[2][2]);   // -R(1,2), R(2,2)
        e.z = std::atan2(-m[1][0], m[0][0]);   // -R(0,1), R(0,0)
    } else {
        // 万向节锁（y = ±90°）：x 与 z 只剩一个自由度，把 x 钉成 0，
        // 剩下的全给 z。两个分支的差别只在 sb 的符号。
        e.x = 0.0f;
        e.z = (sb > 0.0f) ? std::atan2(m[0][1], m[1][1])
                          : std::atan2(-m[0][1], m[1][1]);
    }
    return e;
}

// ---------------------------------------------------------------- 建世界

SceneBinding buildSceneWorld(scene::Scene& scene, const SceneBuildOptions& opts) {
    SceneBinding out;

    const scene::PhysicsSettings& ps = scene.physicsSettings();
    bool enabled = ps.enabled;
    if (opts.enabledOverride >= 0) enabled = (opts.enabledOverride != 0);
    out.enabled = enabled;
    if (!enabled) return out;

    ecs::World& w = scene.world();
    std::vector<ecs::Entity> ents;
    std::vector<ecs::EntityId> seen;
    w.each<ecs::RigidBodyComponent>(
        [&](ecs::Entity e, ecs::RigidBodyComponent&) {
            ents.push_back(e);
            seen.push_back(e.id);
        });
    out.rigidEntities = static_cast<int>(ents.size());

    // G08：触发体**不一定**有刚体组件（它只要一个碰撞体积 + 一个事件源）。
    // 所以单独再收一遍，按 id 去重（同一实体两样都挂时只建一个体）。
    w.each<ecs::TriggerComponent>([&](ecs::Entity e, ecs::TriggerComponent& tc) {
        if (!tc.enabled) return;
        if (std::find(seen.begin(), seen.end(), e.id) != seen.end()) return;
        ents.push_back(e);
        seen.push_back(e.id);
    });

    auto world = std::make_unique<PhysicsWorld>();
    PhysicsWorldSettings& ws = world->settings();
    ws.gravity = ps.gravity;
    ws.fixedTimeStep = ps.fixedTimeStep;
    ws.maxSubSteps = ps.maxSubSteps;
    ws.velocityIterations = ps.velocityIterations;
    ws.positionIterations = ps.positionIterations;
    ws.baumgarte = ps.baumgarte;
    ws.slop = ps.slop;
    ws.allowSleep = ps.allowSleep;
    ws.sleepLinearThreshold = ps.sleepLinearThreshold;
    ws.sleepAngularThreshold = ps.sleepAngularThreshold;
    ws.sleepTime = ps.sleepTime;

    // 无限地面（z = 0 半空间）。entity 留 0xFFFFFFFF → 没有任何实体，
    // 写回时会被自然跳过（它本来也不动）。
    if (ps.groundPlane) {
        RigidBody g;
        g.type = BodyType::Static;
        g.entity = 0xFFFFFFFFu;
        g.shape = Shape::plane(glm::vec3(0.0f, 0.0f, 1.0f), 0.0f);
        g.flags = 0;   // 地面既不受重力、也不休眠（休眠会把它从接触图里摘掉）
        world->addBody(std::move(g));
        out.ground = true;
    }

    int built = 0;
    for (ecs::Entity e : ents) {
        const ecs::RigidBodyComponent* rb = w.get<ecs::RigidBodyComponent>(e);
        // 只有触发体组件、没有刚体组件的实体：给它一份"静态刚体"的默认值，
        // 下面那一大段形状推断逻辑就完全不用分支。
        ecs::RigidBodyComponent rbFallback;
        rbFallback.bodyType = ecs::PhysicsBodyType::Static;
        if (!rb) rb = &rbFallback;
        const auto* cc = w.get<ecs::CollisionComponent>(e);
        const auto* trig = w.get<ecs::TriggerComponent>(e);
        const auto* pmat = w.get<ecs::PhysicsMaterialComponent>(e);
        const bool isTrigger = trig && trig->enabled;

        // 碰撞体的**局部**摆放（CollisionComponent 的 TRS）。显式形状与
        // Auto 兜底路径都用单位阵 —— 那时形状本来就以实体原点为中心。
        glm::mat4 ccLocal(1.0f);
        Shape shape;
        bool haveShape = false;

        // ---- ① 显式形状（Inspector 里手填尺寸）----
        // ⚠ 尺寸是**世界空间的米**，不乘实体缩放 —— "我填的是 0.5 米半径"
        //   是最好解释的语义，也避免"改了缩放碰撞体悄悄变"这种惊喜。
        switch (rb->shape) {
            case ecs::PhysicsShapeType::Sphere:
                shape = Shape::sphere(rb->radius);
                haveShape = true;
                break;
            case ecs::PhysicsShapeType::Box:
                shape = Shape::box(rb->halfExtents);
                haveShape = true;
                break;
            case ecs::PhysicsShapeType::Capsule:
                shape = Shape::capsule(rb->radius, rb->halfHeight);
                haveShape = true;
                break;
            case ecs::PhysicsShapeType::ConvexHull:
                if (cc && !cc->hullPoints.empty()) {
                    shape = Shape::hull(cc->hullPoints);
                    ccLocal = cc->localMatrix();
                    haveShape = true;
                }
                break;
            case ecs::PhysicsShapeType::Auto:
            default:
                break;
        }

        // ---- ② Auto：碰撞组件 → 网格 AABB → 兜底球 ----
        if (!haveShape && cc) {
            if (cc->shape == ecs::ColliderShape::ConvexHull &&
                !cc->hullPoints.empty()) {
                shape = Shape::hull(cc->hullPoints);
            } else if (cc->shape == ecs::ColliderShape::Capsule) {
                shape = Shape::capsule(cc->capsuleRadius, cc->capsuleHalfHeight);
            }
            if (!cc->hullPoints.empty() ||
                cc->shape == ecs::ColliderShape::Capsule) {
                ccLocal = cc->localMatrix();
                haveShape = true;
            }
        }
        if (!haveShape) {
            const auto* mc = w.get<ecs::MeshComponent>(e);
            glm::vec3 mn(0.0f), mx(0.0f);
            if (mc && mc->mesh && meshAabb(*mc->mesh, mn, mx)) {
                // 网格包围盒 → 解析长方体。中心偏到 AABB 中心（很多模型的
                // 原点在脚底，直接用原点会让盒子悬在模型上方半截）。
                shape = Shape::box((mx - mn) * 0.5f);
                ccLocal = glm::translate(glm::mat4(1.0f), (mn + mx) * 0.5f);
                haveShape = true;
            }
        }
        if (!haveShape) {
            // 什么都没有（空物体 + 刚体）：给它一个 0.5 m 的球，而不是静默
            // 跳过 —— 用户看得见"凭空多了一个在滚的小球"才知道自己选错了
            // 形状，比"面板上有个刚体但世界里什么都没有"好排查。
            shape = Shape::sphere(0.5f);
        }

        // ---- ③ 形状原点 → 世界位姿 ----
        const glm::mat4 colliderWorld = scene.worldMatrix(e) * ccLocal;
        const Decomposed d = decomposeMatrix(colliderWorld);
        scaleShapeForWorld(shape, d.scale);

        RigidBody body;
        body.entity = e.id;
        // 触发体一律是静态的：它只报重叠，不该被重力推走 / 被撞飞。
        body.type = isTrigger ? BodyType::Static
                              : static_cast<BodyType>(static_cast<int>(rb->bodyType));
        body.shape = std::move(shape);
        body.position = d.translation;
        body.rotation = d.rotation;
        body.linearVelocity = rb->initialVelocity;
        body.angularVelocity = rb->initialAngularVelocity;
        body.linearDamping = rb->linearDamping;
        body.angularDamping = rb->angularDamping;
        body.material.staticFriction = rb->staticFriction;
        body.material.dynamicFriction = rb->dynamicFriction;
        body.material.restitution = rb->restitution;
        body.material.density = rb->density;
        // G08 的覆盖层：**没有** PhysicsMaterialComponent 时逐字段等价于旧
        // 行为（既有物理用例的轨迹一位都不许变）。
        if (pmat) {
            body.material.staticFriction = pmat->staticFriction;
            body.material.dynamicFriction = pmat->dynamicFriction;
            body.material.restitution = pmat->restitution;
            body.material.density = pmat->density;
            ++out.materials;
        }
        // mass > 0 → 用户指定总质量；否则按密度 × 体积（改尺寸时质量自动
        // 跟着变，这是"默认路径"该有的手感）。
        body.massProps = (rb->mass > 0.0f)
                             ? body.shape.massFromMass(rb->mass)
                             : body.shape.massFromDensity(rb->density);
        body.setFlag(kBodyUseGravity, rb->useGravity && !isTrigger);
        // 世界级总开关关掉时，个体开关一律失效（一个开关就够）：
        body.setFlag(kBodyAllowSleep, rb->allowSleep && ps.allowSleep);
        body.setFlag(kBodyIsTrigger, isTrigger);

        const BodyId id = world->addBody(std::move(body));
        out.entityToBody[e.id] = id;
        ++built;
        if (isTrigger) ++out.triggers;
    }

    out.bodies = built + (out.ground ? 1 : 0);
    out.world = std::move(world);
    // 地形接进物理（第十轮）：没有它，刚体和角色都只认 z=0 的无限地面，
    // 于是"新工程一进去，玩家埋在地面下 3 米"。
    attachTerrain(scene, out);
    return out;
}

// ---------------------------------------------------------------- 运动学同步

void syncKinematicFromScene(
    scene::Scene& scene, PhysicsWorld& world,
    const std::unordered_map<ecs::EntityId, BodyId>& map, float dt) {
    if (dt <= 1.0e-6f) return;
    ecs::World& w = scene.world();

    for (const auto& kv : map) {
        RigidBody* b = world.body(kv.second);
        if (!b || !b->isKinematic()) continue;
        const ecs::Entity e = w.handle(kv.first);
        if (!e.valid()) continue;

        const auto* cc = w.get<ecs::CollisionComponent>(e);
        const glm::mat4 colliderWorld =
            scene.worldMatrix(e) * (cc ? cc->localMatrix() : glm::mat4(1.0f));
        const Decomposed d = decomposeMatrix(colliderWorld);

        b->linearVelocity = (d.translation - b->position) / dt;
        // 角速度用"小角度近似"：Δq ≈ (1, ½ω·dt) → ω ≈ 2·(Δq).xyz / dt
        const glm::quat dq = d.rotation * glm::conjugate(b->rotation);
        b->angularVelocity = 2.0f * glm::vec3(dq.x, dq.y, dq.z) / dt;
        b->position = d.translation;
        b->rotation = d.rotation;
        b->awake = true;
    }
}

// ---------------------------------------------------------------- 写回

void writeBackToScene(
    scene::Scene& scene, const PhysicsWorld& world,
    const std::unordered_map<ecs::EntityId, BodyId>& map) {
    ecs::World& w = scene.world();

    for (const auto& kv : map) {
        const RigidBody* b = world.body(kv.second);
        if (!b || !b->isDynamic()) continue;
        ecs::Entity e = w.handle(kv.first);
        if (!e.valid()) continue;
        auto* tc = w.get<ecs::TransformComponent>(e);
        if (!tc) continue;

        const auto* cc = w.get<ecs::CollisionComponent>(e);
        // 反解：碰撞体世界位姿 → 实体世界矩阵。cc->localMatrix() 是碰撞体
        // 相对实体的固定偏移/缩放，乘它的逆就把自己摘出去。
        const glm::mat4 colliderWorld =
            glm::translate(glm::mat4(1.0f), b->position) *
            glm::mat4_cast(b->rotation);
        glm::mat4 entityWorld =
            colliderWorld * glm::inverse(cc ? cc->localMatrix()
                                            : glm::mat4(1.0f));

        // 有父节点：世界矩阵还要除回父节点的世界矩阵（Transform 是局部量）
        const auto* h = w.get<ecs::HierarchyComponent>(e);
        if (h && h->parent != ecs::kInvalidEntity) {
            const ecs::Entity p = w.handle(h->parent);
            if (p.valid())
                entityWorld = glm::inverse(scene.worldMatrix(p)) * entityWorld;
        }

        const Decomposed d = decomposeMatrix(entityWorld);
        tc->position = d.translation;
        tc->rotation = eulerXYZFromMatrix(glm::mat3_cast(d.rotation));
        // ⚠ 刻意**不写 scale**：上面这次 decompose 出来的缩放带着碰撞体
        //   局部矩阵的逆，是无意义的。实体自己的缩放是编辑态摆好的，保留。
    }
}

} // namespace physics
