// ============================================================
// nav/NavSystem.cpp —— 代理推进
// ============================================================

#include "nav/NavSystem.h"

#include "ecs/Components.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace nav {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;
} // namespace

float approachAngle(float cur, float target, float t) {
    float d = target - cur;
    while (d > kPi) d -= kTwoPi;
    while (d < -kPi) d += kTwoPi;
    if (t >= 1.0f) return target;
    if (t <= 0.0f) return cur;
    return cur + d * t;
}

void NavSystem::setGrid(NavGrid g) {
    m_has = g.valid();
    m_grid = m_has ? std::move(g) : NavGrid{};
}

void NavSystem::clearGrid() {
    m_grid = NavGrid{};
    m_has = false;
}

bool NavSystem::gotoPoint(scene::Scene& s, ecs::Entity e, const glm::vec3& goal) {
    auto* agent = s.world().get<ecs::NavAgentComponent>(e);
    if (!agent) return false;
    agent->goal = goal;
    agent->hasGoal = true;
    agent->reached = false;
    agent->failed = false;
    agent->path.clear();
    agent->next = 0;
    agent->repathTimer = 0.0f;   // 下一帧强制求路
    return m_has;
}

void NavSystem::stop(scene::Scene& s, ecs::Entity e) {
    auto* agent = s.world().get<ecs::NavAgentComponent>(e);
    if (!agent) return;
    agent->hasGoal = false;
    agent->reached = false;
    agent->failed = false;
    agent->path.clear();
    agent->next = 0;
    agent->repathTimer = 0.0f;
}

void NavSystem::resetAgents(scene::Scene& s) {
    s.world().each<ecs::NavAgentComponent>([&](ecs::Entity, ecs::NavAgentComponent& a) {
        a.path.clear();
        a.next = 0;
        a.repathTimer = 0.0f;
        a.reached = false;
        a.failed = false;
        a.lastExpanded = 0;
        // ⚠ 刻意**不清 `goal` / `hasGoal`**：那是关卡作者摆的数据。
        //   清掉会让"重开一局"之后巡逻兵不再巡逻，而场景没变。
    });
    m_stats = Stats{};
}

int NavSystem::tick(scene::Scene& s, float dt) {
    m_stats = Stats{};
    if (!(dt > 0.0f)) return 0;

    ecs::World& w = s.world();
    w.each<ecs::TransformComponent, ecs::NavAgentComponent>(
        [&](ecs::Entity e, ecs::TransformComponent& tr, ecs::NavAgentComponent& a) {
            ++m_stats.agents;
            if (!a.enabled || !a.hasGoal) return;

            a.repathTimer -= dt;
            const bool needPlan = a.path.empty() || a.next >= a.path.size();
            if (needPlan) {
                if (!m_has) {
                    // 没有网格 ≠ 求路失败一次就算了：每帧都重试会让日志
                    // 刷屏，所以还是走 repathInterval 节流。
                    a.failed = true;
                    ++m_stats.failed;
                    if (a.repathTimer <= 0.0f) a.repathTimer = a.repathInterval;
                    return;
                }
                if (a.repathTimer <= 0.0f) {
                    NavPath p;
                    if (findPath(m_grid, tr.position, a.goal, p)) {
                        if (a.smooth) smoothPath(m_grid, p);
                        a.path = p.pts;
                        // 路径第 0 点就是"我现在站的地方"，从第 1 点开始走。
                        a.next = (a.path.size() > 1) ? 1 : a.path.size();
                        a.lastExpanded = p.expanded;
                        a.failed = false;
                        a.reached = false;
                        m_stats.expanded += p.expanded;
                    } else {
                        a.failed = true;
                        a.path.clear();
                        a.next = 0;
                        ++m_stats.failed;
                    }
                    a.repathTimer = a.repathInterval;
                }
                if (a.path.empty()) return;
            }

            if (a.next >= a.path.size()) {
                if (!a.reached) {
                    a.reached = true;
                    ++m_stats.arrived;
                }
                return;
            }

            glm::vec3 target = a.path[a.next];
            glm::vec3 delta(target.x - tr.position.x, target.y - tr.position.y, 0.0f);
            float dist = glm::length(delta);
            // 到点判定可能会**连跨好几个路点**（dt 大或路点密），用循环吃掉。
            while (dist <= a.arriveDist && a.next < a.path.size()) {
                ++a.next;
                if (a.next >= a.path.size()) {
                    a.reached = true;
                    ++m_stats.arrived;
                    return;
                }
                target = a.path[a.next];
                delta = glm::vec3(target.x - tr.position.x, target.y - tr.position.y, 0.0f);
                dist = glm::length(delta);
            }
            if (dist <= 1e-5f) return;

            const glm::vec3 dir = delta / dist;
            const float step = std::min(a.speed * dt, dist);

            if (auto* cc = w.get<ecs::CharacterControllerComponent>(e)) {
                // 有胶囊 → 只写"期望速度"，位移交给物理那套（台阶/斜坡/贴地）。
                cc->velocity.x = dir.x * a.speed;
                cc->velocity.y = dir.y * a.speed;
            } else {
                tr.position.x += dir.x * step;
                tr.position.y += dir.y * step;
                // 2.5D：高度贴网格。⚠ 只在目标格可行走时贴 —— 越界的中间
                //   状态（被外力推出网格）不该把 z 拉回网格底面的哨兵值。
                const glm::ivec2 c = m_grid.cellOf(tr.position);
                if (m_grid.walkable(c.x, c.y)) {
                    tr.position.z = m_grid.heightAt(c.x, c.y);
                }
            }

            if (a.faceMoveDir) {
                const float want = std::atan2(dir.y, dir.x);
                const float t = (a.turnSpeed <= 0.0f) ? 1.0f : std::min(1.0f, dt * a.turnSpeed);
                tr.rotation.z = approachAngle(tr.rotation.z, want, t);
            }
            ++m_stats.moving;
        });

    return m_stats.moving;
}

} // namespace nav
