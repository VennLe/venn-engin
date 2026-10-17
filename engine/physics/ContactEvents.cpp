#include "physics/ContactEvents.h"

#include "core/Logger.h"
#include "physics/ConvexCollision.h"
#include "physics/PhysicsShape.h"
#include "physics/PhysicsWorld.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace physics {

const char* contactPhaseName(ContactPhase p) {
    switch (p) {
        case ContactPhase::Enter: return "enter";
        case ContactPhase::Stay: return "stay";
        case ContactPhase::Exit: return "exit";
    }
    return "?";
}

std::uint64_t contactPairKey(ecs::EntityId a, ecs::EntityId b) {
    const std::uint32_t lo = a < b ? a : b;
    const std::uint32_t hi = a < b ? b : a;
    return (static_cast<std::uint64_t>(lo) << 32) | static_cast<std::uint64_t>(hi);
}

void ContactTracker::reset() {
    m_prev.clear();
    m_cur.clear();
    m_total = 0;
    m_enters = 0;
    m_exits = 0;
}

void ContactTracker::collect(const PhysicsWorld& world,
                             std::vector<ContactEvent>& out) {
    m_cur.clear();

    auto addPair = [&](ecs::EntityId a, ecs::EntityId b, bool trigger,
                       const glm::vec3& n, float depth) {
        if (a == ecs::kInvalidEntity || b == ecs::kInvalidEntity || a == b) return;
        const std::uint64_t k = contactPairKey(a, b);
        auto it = m_cur.find(k);
        if (it == m_cur.end()) {
            PairInfo info;
            info.stay = 1;
            info.trigger = trigger;
            info.normal = n;
            info.depth = depth;
            m_cur.emplace(k, info);
        } else {
            // 同一对可能有多个接触点 / 多条路径：保留更深的那个
            if (depth > it->second.depth) {
                it->second.depth = depth;
                it->second.normal = n;
            }
            it->second.trigger = it->second.trigger || trigger;
        }
    };

    // ---- ① 实体之间的实心接触（求解器已经算好了流形）----
    for (const ContactManifold& m : world.manifolds()) {
        if (m.count <= 0) continue;
        const RigidBody* ba = world.body(m.bodyA);
        const RigidBody* bb = world.body(m.bodyB);
        if (!ba || !bb) continue;
        if (ba->hasFlag(kBodyIsTrigger) || bb->hasFlag(kBodyIsTrigger)) continue;

        float depth = 0.0f;
        for (int i = 0; i < m.count; ++i)
            depth = std::max(depth, m.points[i].depth);
        addPair(ba->entity, bb->entity, false, m.normal, depth);
    }

    // ---- ② 触发体：窄相**不会**为触发体生成流形，这里单独测一遍 ----
    for (const RigidBody& t : world.bodies()) {
        if (t.id == kInvalidBody) continue;
        if (!t.hasFlag(kBodyIsTrigger)) continue;
        if (t.entity == ecs::kInvalidEntity) continue;
        if (!t.shape.valid()) continue;

        const TransformedShape ta = t.shape.transformed(t.worldMatrix());
        for (const RigidBody& o : world.bodies()) {
            if (o.id == kInvalidBody || o.id == t.id) continue;
            if (o.entity == ecs::kInvalidEntity) continue;
            if (!o.shape.valid()) continue;
            // 触发体 × 触发体也要报（两个区域重叠是有意义的玩法状态）
            const TransformedShape ob = o.shape.transformed(o.worldMatrix());
            const Contact c = collide(ta, ob);
            // 允许 1 cm 的"刚好贴上"：浮点误差下擦边而过不该漏事件
            if (!c.hit || c.depth < -0.01f) continue;
            addPair(t.entity, o.entity, true, c.normal, std::max(c.depth, 0.0f));
        }
    }

    // ---- ③ 与上一帧做差 ----
    // 先按 key 排序，保证输出的**顺序**也确定（unordered_map 的遍历顺序
    // 在不同实现/不同容量下会变，验证脚本要逐字符比对）。
    std::vector<std::uint64_t> keys;
    keys.reserve(m_cur.size() + m_prev.size());
    for (const auto& kv : m_cur) keys.push_back(kv.first);
    for (const auto& kv : m_prev)
        if (m_cur.find(kv.first) == m_cur.end()) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    for (std::uint64_t k : keys) {
        const auto cur = m_cur.find(k);
        const auto prev = m_prev.find(k);

        if (cur != m_cur.end() && prev == m_prev.end()) {
            ContactEvent ev;
            ev.a = static_cast<ecs::EntityId>(k >> 32);
            ev.b = static_cast<ecs::EntityId>(k & 0xFFFFFFFFu);
            ev.phase = ContactPhase::Enter;
            ev.trigger = cur->second.trigger;
            ev.normal = cur->second.normal;
            ev.depth = cur->second.depth;
            ev.stay = 1;
            out.push_back(ev);
            ++m_enters;
        } else if (cur != m_cur.end() && prev != m_prev.end()) {
            cur->second.stay = prev->second.stay + 1;
            ContactEvent ev;
            ev.a = static_cast<ecs::EntityId>(k >> 32);
            ev.b = static_cast<ecs::EntityId>(k & 0xFFFFFFFFu);
            ev.phase = ContactPhase::Stay;
            ev.trigger = cur->second.trigger;
            ev.normal = cur->second.normal;
            ev.depth = cur->second.depth;
            ev.stay = cur->second.stay;
            out.push_back(ev);
        } else if (cur == m_cur.end() && prev != m_prev.end()) {
            ContactEvent ev;
            ev.a = static_cast<ecs::EntityId>(k >> 32);
            ev.b = static_cast<ecs::EntityId>(k & 0xFFFFFFFFu);
            ev.phase = ContactPhase::Exit;
            ev.trigger = prev->second.trigger;
            ev.stay = prev->second.stay;
            out.push_back(ev);
            ++m_exits;
        }
    }

    m_total += out.size();
    m_prev.swap(m_cur);
    m_cur.clear();

    if (m_trace) {
        for (const ContactEvent& e : out) {
            VK_LOG_INFO("PHYS-EVENT %s a=%u b=%u trigger=%d stay=%d depth=%.4f",
                        contactPhaseName(e.phase), e.a, e.b, e.trigger ? 1 : 0,
                        e.stay, static_cast<double>(e.depth));
        }
    }
}

// ============================================================ 自检

namespace {

int g_ctFail = 0;
int g_ctPass = 0;

void tcheck(const char* what, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_ctPass;
        std::printf("CONTACT-TEST pass %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    } else {
        ++g_ctFail;
        std::printf("CONTACT-TEST FAIL %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    }
}

BodyId addBody(PhysicsWorld& w, BodyType type, const Shape& shape,
               const glm::vec3& pos, ecs::EntityId entity,
               bool trigger = false) {
    RigidBody b;
    b.type = type;
    b.shape = shape;
    b.position = pos;
    b.entity = entity;
    // 动态体必须受重力（否则"落地"根本不会发生）；静态/运动学体不受力。
    b.setFlag(kBodyUseGravity, type == BodyType::Dynamic);
    b.setFlag(kBodyIsTrigger, trigger);
    if (type == BodyType::Dynamic)
        b.massProps = shape.massFromDensity(500.0f);
    return w.addBody(std::move(b));
}

} // namespace

int runContactSelfTest() {
    g_ctPass = 0;
    g_ctFail = 0;
    std::printf("CONTACT-TEST begin (no window)\n");

    // ---- A. 触发体：Enter 恰好 1 次，Stay 若干次，Exit 恰好 1 次 ----
    {
        PhysicsWorld w;
        w.settings().allowSleep = false;
        addBody(w, BodyType::Static, Shape::box(glm::vec3(1.0f, 1.0f, 1.0f)),
                glm::vec3(0.0f), /*entity=*/1001, /*trigger=*/true);
        const BodyId probe =
            addBody(w, BodyType::Kinematic, Shape::sphere(0.3f),
                    glm::vec3(-4.0f, 0.0f, 0.0f), /*entity=*/1002);

        ContactTracker tracker;
        std::vector<ContactEvent> all;
        std::vector<ContactEvent> step;

        // 从 -4 匀速走到 +4（8 m / 0.05 m per step = 160 步）
        for (int i = 0; i < 200; ++i) {
            RigidBody* b = w.body(probe);
            b->position.x += 0.05f;
            step.clear();
            tracker.collect(w, step);
            for (const ContactEvent& e : step) all.push_back(e);
        }

        int enters = 0, exits = 0, stays = 0;
        int firstEnter = -1, lastExit = -1;
        bool orderOk = true;
        int seenEnter = 0;
        for (std::size_t i = 0; i < all.size(); ++i) {
            const ContactEvent& e = all[i];
            if (e.phase == ContactPhase::Enter) {
                ++enters;
                ++seenEnter;
                if (firstEnter < 0) firstEnter = static_cast<int>(i);
            } else if (e.phase == ContactPhase::Exit) {
                ++exits;
                lastExit = static_cast<int>(i);
            } else {
                ++stays;
                if (seenEnter != 1) orderOk = false;   // Stay 必须出现在 Enter 之后
            }
        }

        tcheck("A1 触发事件 Enter 恰好 1 次", enters == 1,
               "enters=" + std::to_string(enters));
        tcheck("A2 触发事件 Exit 恰好 1 次", exits == 1,
               "exits=" + std::to_string(exits));
        tcheck("A3 中间有 Stay（≥ 10 步）", stays >= 10,
               "stays=" + std::to_string(stays));
        tcheck("A4 顺序是 Enter → Stay… → Exit",
               firstEnter == 0 && lastExit == static_cast<int>(all.size()) - 1 &&
                   orderOk,
               "first=" + std::to_string(firstEnter) + " last=" +
                   std::to_string(lastExit) + " total=" +
                   std::to_string(all.size()));
        tcheck("A5 所有事件都标记为 trigger", [&] {
            for (const ContactEvent& e : all)
                if (!e.trigger) return false;
            return !all.empty();
        }());
        tcheck("A6 计数器与事件序列一致",
               tracker.enters() == 1 && tracker.exits() == 1,
               "ent=" + std::to_string(tracker.enters()) + " ext=" +
                   std::to_string(tracker.exits()));
    }

    // ---- B. 实心接触：落地 → Enter，然后一直是 Stay ----
    {
        PhysicsWorld w;
        w.settings().allowSleep = false;
        addBody(w, BodyType::Static, Shape::box(glm::vec3(5.0f, 5.0f, 0.5f)),
                glm::vec3(0.0f, 0.0f, -0.5f), /*entity=*/2001);
        addBody(w, BodyType::Dynamic, Shape::sphere(0.4f),
                glm::vec3(0.0f, 0.0f, 3.0f), /*entity=*/2002);

        ContactTracker tracker;
        std::vector<ContactEvent> step;
        int enters = 0;
        // 落地后（最后 60 步）只允许 Stay —— 这一条才是"键稳定"的证据：
        // 键不稳的话，静止的小球会每帧 Enter + Exit 各来一次。
        int settledBad = 0;
        int settledStay = 0;
        std::unordered_map<std::uint64_t, int> distinctPairs;
        for (int i = 0; i < 240; ++i) {
            w.stepFixed(1.0f / 60.0f);
            step.clear();
            tracker.collect(w, step);
            for (const ContactEvent& e : step) {
                ++distinctPairs[contactPairKey(e.a, e.b)];
                if (i < 180) {
                    if (e.phase == ContactPhase::Enter) ++enters;
                } else {
                    if (e.phase == ContactPhase::Stay) ++settledStay;
                    else ++settledBad;
                }
            }
        }
        tcheck("B1 小球落地被检测到（≥ 1 次 Enter）", enters >= 1,
               "enters=" + std::to_string(enters));
        tcheck("B2 静止之后不再出现 Enter / Exit（键稳定、不抖动）",
               settledBad == 0, "异常事件=" + std::to_string(settledBad));
        tcheck("B3 静止之后持续收到 Stay", settledStay > 0,
               "stays=" + std::to_string(settledStay));
        // 整个 4 秒里只应该出现过**一个**键。键不稳（(a,b) 与 (b,a) 互换）
        // 会立刻变成两个，这条断言就是为它准备的。
        tcheck("B4 全程只出现一个接触键（一对体只有一个键）",
               distinctPairs.size() == 1,
               "distinct=" + std::to_string(distinctPairs.size()));
    }

    // ---- C. 触发体**不改变**动态体轨迹（逐位相同）----
    {
        auto run = [](bool withTrigger) {
            PhysicsWorld w;
            w.settings().allowSleep = false;
            addBody(w, BodyType::Static,
                    Shape::box(glm::vec3(5.0f, 5.0f, 0.5f)),
                    glm::vec3(0.0f, 0.0f, -0.5f), 3001);
            if (withTrigger) {
                addBody(w, BodyType::Static,
                        Shape::box(glm::vec3(1.0f, 1.0f, 1.0f)),
                        glm::vec3(0.0f, 0.0f, 1.0f), 3002, /*trigger=*/true);
            }
            addBody(w, BodyType::Dynamic, Shape::sphere(0.4f),
                    glm::vec3(0.0f, 0.0f, 4.0f), 3003);
            for (int i = 0; i < 300; ++i) w.stepFixed(1.0f / 60.0f);
            return w.bodies().back().position;
        };
        const glm::vec3 a = run(false);
        const glm::vec3 b = run(true);
        tcheck("C1 开关触发体后动态体 300 帧位置逐位相同", a == b,
               "(" + std::to_string(a.x) + "," + std::to_string(a.z) + ") vs (" +
                   std::to_string(b.x) + "," + std::to_string(b.z) + ")");
    }

    std::printf("CONTACT-TOTAL pass=%d fail=%d\n", g_ctPass, g_ctFail);
    return g_ctFail;
}

} // namespace physics
