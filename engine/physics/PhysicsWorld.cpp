#include "physics/PhysicsWorld.h"

#include "core/Logger.h"
#include "physics/ConvexCollision.h"
#include "physics/Narrowphase.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace physics {

namespace {

// 点 p 在线段 [a,b] 上的最近点参数 t ∈ [0,1]
// （Narrowphase.cpp 里有一份同样的；这里是文件局部的小工具，不跨 TU 共享
//   匿名命名空间的符号，所以各自留一份比抽头文件更省事。）
float closestParamOnSegment(const glm::vec3& a, const glm::vec3& b,
                            const glm::vec3& p) {
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < kEpsilon) return 0.0f;
    return glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
}

// 速度平方（省一次 sqrt）
inline float speed2(const RigidBody& b) {
    return glm::dot(b.linearVelocity, b.linearVelocity);
}
inline float angSpeed2(const RigidBody& b) {
    return glm::dot(b.angularVelocity, b.angularVelocity);
}

// ---- 射线 vs 各种形状 ----
//
// 每个都返回"进入形状的距离 t"（无交点返回 < 0）。用解析法而不是
// GJK 射线版：解析法精确、无迭代、且这些形状本身就够简单。

float raySphere(const glm::vec3& o, const glm::vec3& d, const glm::vec3& c,
                float r) {
    const glm::vec3 m = o - c;
    const float b = glm::dot(m, d);          // d 已是单位向量
    const float cc = glm::dot(m, m) - r * r;
    if (cc > 0.0f && b > 0.0f) return -1.0f; // 起点在外且背向球
    const float disc = b * b - cc;
    if (disc < 0.0f) return -1.0f;
    float t = -b - std::sqrt(disc);
    if (t < 0.0f) t = 0.0f;                  // 起点在球内
    return t;
}

// 射线 vs 轴对齐盒（**局部空间**），slab 法。需要求出进入/离开参数。
float rayAabbLocal(const glm::vec3& o, const glm::vec3& d,
                   const glm::vec3& he, glm::vec3& outNormal) {
    float tMin = -1.0e30f;
    float tMax = 1.0e30f;
    int hitAxis = 0;
    float hitSign = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1.0e-8f) {
            // 平行于该轴的 slab：起点必须在 slab 内，否则无解
            if (o[i] < -he[i] || o[i] > he[i]) return -1.0f;
            continue;
        }
        const float inv = 1.0f / d[i];
        float t1 = (-he[i] - o[i]) * inv;
        float t2 = (he[i] - o[i]) * inv;
        float sign = -1.0f;                  // 进入的是 -he 那一面
        if (t1 > t2) {
            std::swap(t1, t2);
            sign = 1.0f;
        }
        if (t1 > tMin) {
            tMin = t1;
            hitAxis = i;
            hitSign = sign;
        }
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return -1.0f;
    }
    if (tMax < 0.0f) return -1.0f;
    outNormal = glm::vec3(0.0f);
    outNormal[hitAxis] = hitSign;
    return std::max(tMin, 0.0f);
}

// 射线 vs 线段（用于胶囊）：先求射线与线段的最近点参数，再看距离。
float raySegment(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a,
                 const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const glm::vec3 ao = o - a;
    const float abab = glm::dot(ab, ab);
    const float abd = glm::dot(ab, d);
    const float abao = glm::dot(ab, ao);
    const float denom = abab - abd * abd;
    if (std::fabs(denom) < 1.0e-8f) return 0.0f;   // 平行
    float t = (abd * abao - abab * glm::dot(ao, d)) / denom;
    t = std::max(t, 0.0f);
    return t;
}

const RigidBody* findBody(const std::vector<RigidBody>& v, BodyId id) {
    if (id >= v.size()) return nullptr;
    if (v[id].id != id) return nullptr;
    return &v[id];
}

} // namespace

// ================================================================
//  刚体管理
// ================================================================

BodyId PhysicsWorld::addBody(RigidBody b) {
    b.id = static_cast<BodyId>(m_bodies.size());
    b.sleepTimer = 0.0f;
    b.awake = true;
    b.prevPosition = b.position;
    b.prevRotation = b.rotation;
    b.contactStart = -1;
    b.contactCount = 0;

    // 质量：调用方可能只填了形状。这里兜底算一次，避免"忘了设质量 →
    // 逆质量 1（= 1 千克）"这种静默错误。静态/运动学体强制 0。
    if (b.type != BodyType::Dynamic) {
        b.massProps = makeMassProperties(0.0f, glm::mat3(0.0f));
    } else if (b.massProps.mass <= 0.0f) {
        b.massProps = b.shape.massFromDensity(b.material.density);
        // 密度也算出非正质量（形状是平面等）→ 给一个 1 kg 兜底，
        // 否则 invMass = 0 会让"动态体"完全不动（很难排查）。
        if (b.massProps.mass <= 0.0f) {
            b.massProps = b.shape.massFromMass(1.0f);
        }
    }

    m_bodies.push_back(b);
    return b.id;
}

void PhysicsWorld::removeBody(BodyId id) {
    if (id >= m_bodies.size() || m_bodies[id].id != id) return;
    m_bodies[id].id = kInvalidBody;
    m_bodies[id].type = BodyType::Static;
    m_bodies[id].massProps = makeMassProperties(0.0f, glm::mat3(0.0f));
    // 引用它的关节必须一并删掉 —— 留着会指向一个已经"不是那个体"的槽位。
    m_joints.erase(std::remove_if(m_joints.begin(), m_joints.end(),
                                  [&](const Joint& j) {
                                      return j.bodyA == id || j.bodyB == id;
                                  }),
                   m_joints.end());
}

void PhysicsWorld::clear() {
    m_bodies.clear();
    m_joints.clear();
    m_manifolds.clear();
    m_cache.clear();
    m_accumulator = 0.0f;
    m_stats = PhysicsStats{};
    // 地形本身不该被 clear 掉（它是世界里的一件"设施"），但那个伪刚体
    // 已经随 m_bodies 一起没了 → 下次 setHeightfield / 下一步会重建。
    m_terrainBody = kInvalidBody;
}

// ================================================================
//  地形（高度场）
// ================================================================

void PhysicsWorld::setHeightfield(const Heightfield* hf) {
    // ---- 拷贝一份高度数据（2026-10-23）----
    //
    // 不再借用调用方的指针：`heights` 实际指向调用方（通常是
    // `SceneBinding::terrainHeights`）的 vector 内存，那个 vector 一析构，
    // 这里就成了悬垂。编辑器正是 `std::move(binding.world)` 之后让
    // binding 立刻销毁的 —— 于是每次进 Play 读地形都踩释放过的内存。
    // 详见 PhysicsWorld.h 里 `m_terrainOwned` 的注释。
    if (hf && hf->valid()) {
        const std::size_t n = static_cast<std::size_t>(hf->sampleCount());
        m_terrainHeights.assign(hf->heights, hf->heights + n);
        m_terrainOwned = std::make_unique<Heightfield>(*hf);
        m_terrainOwned->heights = m_terrainHeights.data();
        m_terrain = m_terrainOwned.get();
    } else {
        m_terrain = nullptr;
        m_terrainOwned.reset();
        m_terrainHeights.clear();
        m_terrainHeights.shrink_to_fit();
    }

    if (!m_terrain) {
        // 关掉：伪刚体留着也没坏处（它配不到任何体），但为了让
        // "关掉地形"之后的世界和改动前**完全一致**，直接摘掉。
        if (m_terrainBody != kInvalidBody) {
            removeBody(m_terrainBody);
            m_terrainBody = kInvalidBody;
        }
        return;
    }
    if (m_terrainBody == kInvalidBody) {
        // 伪静态刚体：求解器要求 `bodyA` 是 `bodies` 里的合法下标。
        //
        // 形状给一个**远在 -1e6 米、半径 1 mm** 的球 —— 宽相的 AABB 与
        // 任何真实物体都不相交，于是它永远不会产出"球-盒"之类的接触，
        // 只作为地形流形的 A 侧存在。（用 Plane 不行：平面 AABB 是无限大，
        // 会和每个体配对，白白多跑 N 次窄相。）
        RigidBody t;
        t.type = BodyType::Static;
        t.entity = 0xFFFFFFFFu;      // 没有实体 → 写回变换时自然跳过
        t.shape = Shape::sphere(1.0e-3f);
        t.position = glm::vec3(0.0f, 0.0f, -1.0e6f);
        t.flags = 0;                 // 不受重力、不休眠
        t.material.dynamicFriction = 0.85f;
        t.material.staticFriction = 0.9f;
        t.material.restitution = 0.0f;
        m_terrainBody = addBody(std::move(t));
        VK_LOG_INFO("PHYS-TERRAIN attach cols=%d rows=%d mpp=%.2f maxh=%.2f "
                    "proxyBody=%u",
                    m_terrain->cols, m_terrain->rows,
                    static_cast<double>(m_terrain->metersPerPixel),
                    static_cast<double>(m_terrain->heightScale), m_terrainBody);
    }
}

// ================================================================
//  关节
// ================================================================

std::uint32_t PhysicsWorld::addJoint(const Joint& j) {
    m_joints.push_back(j);
    m_joints.back().clearImpulses();
    return static_cast<std::uint32_t>(m_joints.size() - 1);
}

// ================================================================
//  步进
// ================================================================

int PhysicsWorld::step(float realDt) {
    if (realDt <= 0.0f) return 0;
    const float dt = m_settings.fixedTimeStep;
    if (dt <= 0.0f) return 0;

    // 长卡顿之后不要让物理"追时间"（见文件头的死亡螺旋）
    m_accumulator += std::min(realDt, m_settings.maxFrameTime);

    int steps = 0;
    while (m_accumulator >= dt && steps < m_settings.maxSubSteps) {
        stepFixed(dt);
        m_accumulator -= dt;
        ++steps;
    }
    // 跑满子步上限还有剩余 → 直接丢掉（宁可物理变慢，也不要越积越多）
    if (steps >= m_settings.maxSubSteps && m_accumulator > dt) {
        m_accumulator = 0.0f;
    }
    m_stats.simTime = static_cast<float>(m_stats.steps) * dt;
    return steps;
}

void PhysicsWorld::stepFixed(float dt) {
    // ---- 0) 记下上一步变换（插值 / 静止判定用）----
    for (RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody) continue;
        b.savePrevTransform();
    }

    // ---- 1) 速度积分（重力 + 外力 + 阻尼 + 锁轴）----
    for (RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody || !b.movable() || !b.awake) continue;
        b.integrateVelocity(m_settings.gravity, dt);
        b.force = glm::vec3(0.0f);
        b.torque = glm::vec3(0.0f);
    }

    // ---- 2) 宽相 ----
    updateAabbs();
    m_broadphase.rebuild(m_bodies, m_settings.broadphaseMargin);

    // ---- 3) 窄相 ----
    collectContacts();

    // ---- 4) 唤醒传播（必须**在求解前**：新唤醒的体要参与本步求解）----
    wakeIslands();

    // ---- 5) 关节 prestep ----
    for (Joint& j : m_joints) {
        const RigidBody* A = findBody(m_bodies, j.bodyA);
        const RigidBody* B = findBody(m_bodies, j.bodyB);
        if (!A || !B) {
            j.rowCount = 0;
            continue;
        }
        j.prestep(*A, *B, dt, m_settings.baumgarte);
    }

    // ---- 6) 求解（速度 + 位置积分 + 位置修正）----
    SolverSettings ss;
    ss.velocityIterations = m_settings.velocityIterations;
    ss.positionIterations = m_settings.positionIterations;
    ss.baumgarte = m_settings.baumgarte;
    ss.slop = m_settings.slop;
    ss.restitutionThreshold = m_settings.restitutionThreshold;
    ss.enableFriction = m_settings.enableFriction;
    ss.splitImpulse = m_settings.splitImpulse;
    ss.maxLinearVelocity = m_settings.maxLinearVelocity;
    ss.maxAngularVelocity = m_settings.maxAngularVelocity;
    ss.gravity = m_settings.gravity;
    m_solver.solve(m_bodies, m_cache, m_manifolds, &m_joints, ss, dt);

    // ---- 7) 休眠判定 ----
    updateSleep(dt);

    // ---- 8) 把本帧接触存成"上一帧"（下一帧热启动用）----
    m_cache.commit(m_manifolds);

    ++m_stats.steps;
    // ---- 9) 刷新统计 ----
    // ⚠ 必须在这里刷：`stats()` 是**只读快照**，它不会自己更新。早先只在
    //   外部显式调用 refreshStats() 才更新，于是"跑完一步读 manifolds"
    //   永远读到 0 —— 自检的广相对照用例就是这么被坑的（宽相明明配对了
    //   540 对，报告却是 0）。统计本身是 O(N)，每步刷一次的开销可忽略，
    //   而"读到过期数字"是排查成本极高的一类 bug。
    refreshStats();
}

void PhysicsWorld::updateAabbs() {
    for (RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody) continue;
        glm::vec3 mn, mx;
        if (b.shape.worldAabb(b.worldMatrix(), mn, mx)) {
            b.aabbMin = mn;
            b.aabbMax = mx;
        } else {
            // 平面（无限半空间）：SAP 排不进去，但**必须**给一个有限的
            // 占位值 —— 否则 min.x = -inf 会让排序与"bMin.x > aMax.x"
            // 的剪枝判据全部失效。平面在 Broadphase 里走一对多路径，
            // 这里的 AABB 只用于统计，不参与配对。
            b.aabbMin = b.position - glm::vec3(1.0f);
            b.aabbMax = b.position + glm::vec3(1.0f);
        }
    }
}

void PhysicsWorld::collectContacts() {
    m_manifolds.clear();
    const std::vector<BroadphasePair>& pairs = m_broadphase.pairs();

    for (const BroadphasePair& pr : pairs) {
        RigidBody& A = m_bodies[pr.a];
        RigidBody& B = m_bodies[pr.b];
        if (A.id == kInvalidBody || B.id == kInvalidBody) continue;
        // 两边都睡了 / 都不会动 → 跳过（本步解它没有任何意义）
        const bool aActive = A.movable() && A.awake;
        const bool bActive = B.movable() && B.awake;
        if (!aActive && !bActive) continue;
        // 被关节连起来的两个体默认不互碰（否则"铰链"会把自己锁死）
        bool connected = false;
        for (const Joint& j : m_joints) {
            if ((j.bodyA == pr.a && j.bodyB == pr.b) ||
                (j.bodyA == pr.b && j.bodyB == pr.a)) {
                connected = true;
                break;
            }
        }
        if (connected) continue;

        ContactManifold m;
        if (collideBodies(A, B, pr.a, pr.b, m)) {
            m_manifolds.push_back(m);
        }
    }

    collectTerrainContacts();
}

// 地形接触：不走宽相（高度场没有 AABB），每步 O(N) 扫一遍。
//
// 为什么放在 collectContacts **之后**：热启动缓存按 (bodyA, bodyB) 存，
// 顺序无关；放在同一个函数里保证"一帧的接触集合"是一个整体，
// `wakeIslands` / `updateSleep` 看到的是完整的那一份。
void PhysicsWorld::collectTerrainContacts() {
    if (!hasHeightfield() || m_terrainBody == kInvalidBody) return;

    for (RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody || b.id == m_terrainBody) continue;
        // 与"体对"同一条规则：不会动或睡着的体不必解；触发器不产生冲量。
        if (!b.movable() || !b.awake) continue;
        if (b.hasFlag(kBodyIsTrigger)) continue;

        ContactManifold m;
        if (terrainVsBody(*m_terrain, b, m_terrainBody, b.id, m)) {
            m_manifolds.push_back(m);
        }
    }
}

void PhysicsWorld::wakeIslands() {
    // 只从"确实在动"的体向外传播。用"在动"而不是"醒着"当判据是关键：
    // 一叠已经静下来的箱子，每个都还醒着（计时器还没到）但速度已经接近 0，
    // 用"醒着"传播会让它们永远互相唤醒 → 永远睡不着。
    const float linTh = m_settings.sleepLinearThreshold;
    const float angTh = m_settings.sleepAngularThreshold;
    const float linTh2 = linTh * linTh;
    const float angTh2 = angTh * angTh;

    for (int pass = 0; pass < 2; ++pass) {
        bool changed = false;
        for (const ContactManifold& m : m_manifolds) {
            RigidBody& A = m_bodies[m.bodyA];
            RigidBody& B = m_bodies[m.bodyB];

            auto active = [&](const RigidBody& b) {
                if (!b.awake) return false;
                if (b.isKinematic()) {
                    return speed2(b) > linTh2;   // 动着的平台会带动物体
                }
                if (!b.movable()) return false;
                return speed2(b) > linTh2 || angSpeed2(b) > angTh2;
            };

            if (active(A) && B.movable() && !B.awake) {
                B.awake = true;
                B.sleepTimer = 0.0f;
                changed = true;
            }
            if (active(B) && A.movable() && !A.awake) {
                A.awake = true;
                A.sleepTimer = 0.0f;
                changed = true;
            }
        }
        if (!changed) break;
    }
}

void PhysicsWorld::updateSleep(float dt) {
    if (!m_settings.allowSleep) return;
    const float linTh2 =
        m_settings.sleepLinearThreshold * m_settings.sleepLinearThreshold;
    const float angTh2 =
        m_settings.sleepAngularThreshold * m_settings.sleepAngularThreshold;

    // ---- 1) 每个体各自累计 / 清零计时器 ----
    // 已经睡着的体**不参与**：它的计时器留着不动，否则一个刚醒来的体
    // 会带着旧计时器立刻又睡回去。
    for (RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody || !b.movable()) continue;
        if (!b.hasFlag(kBodyAllowSleep)) {
            b.sleepTimer = 0.0f;
            continue;
        }
        if (!b.awake) continue;
        const bool quiet = speed2(b) < linTh2 && angSpeed2(b) < angTh2;
        b.sleepTimer = quiet ? b.sleepTimer + dt : 0.0f;
    }

    // ---- 2) 按接触图分岛（并查集）----
    //
    // ⚠⚠ **必须整岛一起睡**，这是本文件里最不直观、也最容易漏掉的一条。
    //   早先的实现是"每个体各自判 quiet 就自己睡"，后果是：一叠箱子最底下
    //   那个先安静下来、**单独睡了**，而它上面那个还醒着。下一帧
    //   `collectContacts` 的"两边都不活跃 → 跳过"优化就把它的地面接触剔掉了
    //   （地面是静态 = 不活跃，它自己也睡 = 不活跃）—— 于是它**没有支撑**，
    //   被上面箱子的重量往下压，同时 `wakeIslands` 又把它唤醒，形成
    //   "睡下去 → 掉一段 → 被吵醒 → 再睡"的极限环。
    //   宏观症状：整叠箱子每十几帧集体抖一下，休眠计时器永远攒不满 0.5 s，
    //   箱子永远睡不着 —— 而这看起来完全不像"休眠"的 bug。
    //   判据改成"整岛都 quiet 才一起睡"之后，要么全睡（接触被剔除也无所谓，
    //   因为谁都不动），要么全醒（接触齐全，谁都不会掉）。
    const std::size_t n = m_bodies.size();
    if (m_islandParent.size() != n) m_islandParent.resize(n);
    for (std::size_t i = 0; i < n; ++i) m_islandParent[i] = i;

    std::function<std::size_t(std::size_t)> findRoot =
        [&](std::size_t x) -> std::size_t {
        while (m_islandParent[x] != x) {
            m_islandParent[x] = m_islandParent[m_islandParent[x]];
            x = m_islandParent[x];
        }
        return x;
    };
    auto unite = [&](std::size_t a, std::size_t b) {
        const std::size_t ra = findRoot(a);
        const std::size_t rb = findRoot(b);
        if (ra != rb) m_islandParent[rb] = ra;
    };

    for (const ContactManifold& m : m_manifolds) {
        // 只连"可动且醒着"的两个体。静态体（地面）不参与 —— 它不该阻止
        // 箱子休眠；睡着的体也不参与（它已经是岛的"锚"）。
        const RigidBody& A = m_bodies[m.bodyA];
        const RigidBody& B = m_bodies[m.bodyB];
        if (!A.movable() || !B.movable()) continue;
        if (!A.awake && !B.awake) continue;
        unite(m.bodyA, m.bodyB);
    }

    // ---- 3) 岛级判定：岛内每个（可动的）体都 ready 才整岛入睡 ----
    // islandReady[root] 初值 true；遇到任何一个"还醒着但没 ready"的体就否掉。
    std::vector<char> awakeSeen(n, 0);
    std::vector<char> ready(n, 1);
    for (std::size_t i = 0; i < n; ++i) {
        const RigidBody& b = m_bodies[i];
        if (b.id == kInvalidBody || !b.movable()) continue;
        if (!b.awake) continue;
        const std::size_t r = findRoot(i);
        awakeSeen[r] = 1;
        const bool readyNow = b.hasFlag(kBodyAllowSleep) &&
                              b.sleepTimer >= m_settings.sleepTime;
        if (!readyNow) ready[r] = 0;
    }

    for (std::size_t i = 0; i < n; ++i) {
        RigidBody& b = m_bodies[i];
        if (b.id == kInvalidBody || !b.movable()) continue;
        if (!b.awake) continue;
        const std::size_t r = findRoot(i);
        if (!awakeSeen[r] || !ready[r]) continue;
        // 睡下去之前**把速度清零**：不清的话，醒来的一瞬间会把睡着期间
        // 攒下的那点残余速度一次性释放（表现为"睡着的箱子被碰一下会跳起来"）。
        b.awake = false;
        b.linearVelocity = glm::vec3(0.0f);
        b.angularVelocity = glm::vec3(0.0f);
    }
}

// ================================================================
//  唤醒 / 摆放
// ================================================================

void PhysicsWorld::wakeAll() {
    for (RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody || !b.movable()) continue;
        b.awake = true;
        b.sleepTimer = 0.0f;
    }
}

void PhysicsWorld::wakeBody(BodyId id) {
    RigidBody* b = body(id);
    if (!b) return;
    b->awake = true;
    b->sleepTimer = 0.0f;
}

void PhysicsWorld::sleepBody(BodyId id) {
    RigidBody* b = body(id);
    if (!b) return;
    b->awake = false;
    b->linearVelocity = glm::vec3(0.0f);
    b->angularVelocity = glm::vec3(0.0f);
}

void PhysicsWorld::setTransform(BodyId id, const glm::vec3& pos,
                                const glm::quat& rot) {
    RigidBody* b = body(id);
    if (!b) return;
    b->position = pos;
    b->rotation = glm::normalize(rot);
    b->linearVelocity = glm::vec3(0.0f);
    b->angularVelocity = glm::vec3(0.0f);
    b->prevPosition = pos;
    b->prevRotation = b->rotation;
    b->awake = true;
    b->sleepTimer = 0.0f;
}

// ================================================================
//  查询
// ================================================================

RayHit PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& dirIn,
                             float maxDist, BodyId ignore) const {
    RayHit best;
    const float dlen = glm::length(dirIn);
    if (dlen < kEpsilon) return best;
    const glm::vec3 d = dirIn / dlen;
    const float limit = (maxDist < 0.0f) ? 1.0e30f : maxDist;

    for (const RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody || b.id == ignore) continue;
        if (!b.shape.valid()) continue;
        float t = -1.0f;
        glm::vec3 n(0.0f, 0.0f, 1.0f);

        switch (b.shape.kind) {
            case ShapeKind::Sphere:
                t = raySphere(origin, d, b.position, b.shape.convex.radius);
                if (t >= 0.0f) {
                    n = glm::normalize(origin + d * t - b.position);
                }
                break;
            case ShapeKind::Box: {
                // 把射线换到盒的局部空间
                const glm::mat3 r = glm::mat3_cast(b.rotation);
                const glm::vec3 ol = glm::transpose(r) * (origin - b.position);
                const glm::vec3 dl = glm::transpose(r) * d;
                glm::vec3 nl(0.0f);
                t = rayAabbLocal(ol, dl, b.shape.convex.halfExtents, nl);
                if (t >= 0.0f) n = r * nl;
                break;
            }
            case ShapeKind::Plane: {
                const glm::vec3 pn = b.rotation * b.shape.planeNormal;
                const float D = b.shape.planeOffset + glm::dot(pn, b.position);
                const float denom = glm::dot(pn, d);
                if (std::fabs(denom) < 1.0e-8f) break;
                const float tt = (D - glm::dot(pn, origin)) / denom;
                if (tt >= 0.0f) {
                    t = tt;
                    n = pn;
                }
                break;
            }
            case ShapeKind::Capsule: {
                const glm::vec3 axis =
                    b.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
                const float hh = b.shape.convex.halfHeight;
                const float rr = b.shape.convex.radius;
                // 先看两个端球
                const float ts = raySphere(origin, d, b.position + axis * hh, rr);
                const float tb = raySphere(origin, d, b.position - axis * hh, rr);
                float tBest = -1.0f;
                if (ts >= 0.0f) tBest = ts;
                if (tb >= 0.0f && (tBest < 0.0f || tb < tBest)) tBest = tb;
                // 再看圆柱段（近似：射线与轴线的最近距离 < r 就算命中）
                const float tc = raySegment(origin, d, b.position - axis * hh,
                                            b.position + axis * hh);
                if (tc >= 0.0f) {
                    const glm::vec3 p = origin + d * tc;
                    const float tt = closestParamOnSegment(
                        b.position - axis * hh, b.position + axis * hh, p);
                    const glm::vec3 cp = b.position - axis * hh +
                                         axis * (2.0f * hh) * tt;
                    if (glm::length(p - cp) <= rr &&
                        (tBest < 0.0f || tc < tBest)) {
                        tBest = tc;
                    }
                }
                t = tBest;
                if (t >= 0.0f) {
                    const glm::vec3 p = origin + d * t;
                    const float tt2 = closestParamOnSegment(
                        b.position - axis * hh, b.position + axis * hh, p);
                    const glm::vec3 cp = b.position - axis * hh +
                                         axis * (2.0f * hh) * tt2;
                    const glm::vec3 nn = p - cp;
                    n = (glm::length(nn) > kEpsilon)
                            ? glm::normalize(nn)
                            : glm::vec3(0.0f, 0.0f, 1.0f);
                }
                break;
            }
            case ShapeKind::ConvexHull: {
                // 凸包：用 AABB 近似（解析射线-凸体需要 GJK 射线版，
                // 而凸包在本引擎里是"从网格生成的碰撞体"，用它做精确
                // 拾取的需求远低于做碰撞）。命中后法线取 AABB 面法线。
                const glm::mat3 r = glm::mat3_cast(b.rotation);
                glm::vec3 lmn, lmx;
                b.shape.localAabb(lmn, lmx);
                const glm::vec3 ol = glm::transpose(r) * (origin - b.position);
                const glm::vec3 dl = glm::transpose(r) * d;
                glm::vec3 nl(0.0f);
                t = rayAabbLocal(ol, dl, (lmx - lmn) * 0.5f, nl);
                if (t >= 0.0f) n = r * nl;
                break;
            }
        }

        if (t < 0.0f || t > limit) continue;
        if (!best.hit || t < best.distance) {
            best.hit = true;
            best.distance = t;
            best.point = origin + d * t;
            best.normal = n;
            best.body = b.id;
            best.entity = b.entity;
        }
    }
    return best;
}

void PhysicsWorld::overlapSphere(const glm::vec3& center, float radius,
                                 std::vector<BodyId>& out) const {
    out.clear();
    // 用一个球形状对每个体做一次 collide()。用统一的碰撞路径（而不是
    // 各形状写一遍"AABB 距离"），保证"重叠查询"和"物理接触"的判据一致 ——
    // 否则会出现"查询说重叠、求解器说没接触"这种自相矛盾。
    Shape probeShape = Shape::sphere(radius);
    TransformedShape probe = probeShape.transformed(
        glm::translate(glm::mat4(1.0f), center));
    for (const RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody || !b.shape.valid()) continue;
        if (b.shape.isPlane()) {
            // 球心到平面的距离
            const glm::vec3 pn = b.rotation * b.shape.planeNormal;
            const float D = b.shape.planeOffset + glm::dot(pn, b.position);
            if (glm::dot(pn, center) - D <= radius) out.push_back(b.id);
            continue;
        }
        const Contact c = collide(probe, b.shape.transformed(b.worldMatrix()));
        if (c.hit) out.push_back(b.id);
    }
}

BodyId PhysicsWorld::bodyAtPoint(const glm::vec3& p) const {
    for (const RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody || !b.shape.valid()) continue;
        if (b.shape.isPlane()) {
            const glm::vec3 pn = b.rotation * b.shape.planeNormal;
            const float D = b.shape.planeOffset + glm::dot(pn, b.position);
            if (glm::dot(pn, p) - D <= 0.0f) return b.id;
            continue;
        }
        // 用"半径 0 的球"做点包含测试。0 半径的球在 GJK 里会退化，
        // 所以给 1e-4 的极小半径 —— 效果等价于点包含。
        Shape probeShape = Shape::sphere(1.0e-4f);
        TransformedShape probe =
            probeShape.transformed(glm::translate(glm::mat4(1.0f), p));
        const Contact c = collide(probe, b.shape.transformed(b.worldMatrix()));
        if (c.hit) return b.id;
    }
    return kInvalidBody;
}

// ================================================================
//  角色控制器
// ================================================================

PhysicsWorld::CharacterMove PhysicsWorld::moveCharacter(
    const glm::vec3& from, const glm::vec3& delta, float radius,
    float halfHeight, BodyId ignore) const {
    CharacterMove out;
    out.position = from;

    const float len = glm::length(delta);
    if (len < 1.0e-6f) return out;
    // 每子步位移不超过半个半径：保证胶囊不会"一步跨过"薄墙。
    // 这也是"不需要真 sweep 算法"的原因 —— 用足够小的步子把穿透限制
    // 在可被一次 MTV 推出的范围内。
    const float stepLen = std::max(radius * 0.5f, 0.02f);
    const int steps = std::min(64, std::max(1, static_cast<int>(len / stepLen) + 1));
    const glm::vec3 stepDelta = delta / static_cast<float>(steps);

    Shape probeShape = Shape::capsule(radius, halfHeight);

    for (int s = 0; s < steps; ++s) {
        out.position += stepDelta;

        // 反复推出穿透（3 轮足够把"墙角"这种多面同时接触的情况收敛）
        for (int iter = 0; iter < 3; ++iter) {
            bool pushed = false;
            const TransformedShape probe = probeShape.transformed(
                glm::translate(glm::mat4(1.0f), out.position) *
                glm::mat4_cast(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
            for (const RigidBody& b : m_bodies) {
                if (b.id == kInvalidBody || b.id == ignore) continue;
                if (!b.shape.valid()) continue;
                if (b.hasFlag(kBodyIsTrigger)) continue;

                glm::vec3 n(0.0f);
                float depth = 0.0f;
                if (b.shape.isPlane()) {
                    const glm::vec3 pn = b.rotation * b.shape.planeNormal;
                    const float D = b.shape.planeOffset + glm::dot(pn, b.position);
                    // 胶囊最靠近平面的点 = 中心沿 -pn 半个胶囊高
                    const float d2 = glm::dot(pn, out.position) - D -
                                     (halfHeight + radius);
                    if (d2 >= 0.0f) continue;
                    n = pn;
                    depth = -d2;
                } else {
                    const Contact c =
                        collide(probe, b.shape.transformed(b.worldMatrix()));
                    if (!c.hit || c.depth <= 0.0f) continue;
                    n = c.normal;   // 从探针指向障碍
                    depth = c.depth;
                }
                // 把胶囊沿 -n 推出去（n 指向障碍）
                out.position -= n * depth;
                if (std::fabs(n.z) < 0.7f) out.hitWall = true;
                if (n.z > 0.7f) {
                    out.onGround = true;
                    out.groundNormalZ = n.z;
                }
                pushed = true;
            }
            if (!pushed) break;
        }
    }

    // 收尾：再测一次地面（贴着地面站立时上面的推出可能没触发，因为
    // 深度已经是 0 了）
    {
        const TransformedShape probe = probeShape.transformed(
            glm::translate(glm::mat4(1.0f), out.position));
        for (const RigidBody& b : m_bodies) {
            if (b.id == kInvalidBody || b.id == ignore) continue;
            if (!b.shape.valid() || b.hasFlag(kBodyIsTrigger)) continue;
            if (b.shape.isPlane()) {
                const glm::vec3 pn = b.rotation * b.shape.planeNormal;
                const float D = b.shape.planeOffset + glm::dot(pn, b.position);
                const float gap = glm::dot(pn, out.position) - D -
                                  (halfHeight + radius);
                if (gap < 0.02f) {
                    out.onGround = true;
                    out.groundNormalZ = pn.z;
                }
                continue;
            }
            const Contact c =
                collide(probe, b.shape.transformed(b.worldMatrix()));
            // 接触但很浅（≤2 cm）也算"站着"
            if (c.hit && c.depth >= -0.02f && c.normal.z > 0.7f) {
                out.onGround = true;
                out.groundNormalZ = c.normal.z;
            }
        }
    }
    return out;
}

// ================================================================
//  统计
// ================================================================

void PhysicsWorld::refreshStats() {
    m_stats.bodies = 0;
    m_stats.dynamic = 0;
    m_stats.kinematic = 0;
    m_stats.statics = 0;
    m_stats.awake = 0;
    m_stats.asleep = 0;
    for (const RigidBody& b : m_bodies) {
        if (b.id == kInvalidBody) continue;
        ++m_stats.bodies;
        switch (b.type) {
            case BodyType::Dynamic:
                ++m_stats.dynamic;
                if (b.awake) ++m_stats.awake;
                else ++m_stats.asleep;
                break;
            case BodyType::Kinematic: ++m_stats.kinematic; break;
            case BodyType::Static: ++m_stats.statics; break;
        }
    }
    m_stats.joints = static_cast<int>(m_joints.size());
    m_stats.manifolds = 0;
    m_stats.contactPoints = 0;
    for (const ContactManifold& m : m_manifolds) {
        if (m.count == 0) continue;
        ++m_stats.manifolds;
        m_stats.contactPoints += m.count;
    }
    m_stats.broadphasePairs = static_cast<int>(m_broadphase.pairs().size());
    m_stats.maxPenetration = m_solver.stats().maxPenetration;
    m_stats.maxSpeed = m_solver.stats().maxSpeed;
}

} // namespace physics
