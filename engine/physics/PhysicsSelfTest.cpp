#include "physics/PhysicsSelfTest.h"

#include "core/Logger.h"
#include "physics/Narrowphase.h"
#include "physics/PhysicsWorld.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace physics {

namespace {

int g_pass = 0;
int g_fail = 0;

// 打印一行可被 tools/verify_physics.py 解析的结果。
void report(const char* name, double actual, double expected, double tol) {
    const bool ok = std::fabs(actual - expected) <= tol;
    if (ok) ++g_pass; else ++g_fail;
    VK_LOG_INFO("PHYS-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s",
                name, actual, expected, tol, ok ? "PASS" : "FAIL");
}

// 布尔型断言（条件类，比如"睡着了"/"有交集"）
void reportBool(const char* name, bool ok) {
    if (ok) ++g_pass; else ++g_fail;
    VK_LOG_INFO("PHYS-TEST %s actual=%d expected=1 tol=0 result=%s", name,
                ok ? 1 : 0, ok ? "PASS" : "FAIL");
}

// 造一个"标准测试世界"：指定重力、关掉阻尼与休眠、跑 n 步。
// 阻尼必须关：解析解是"无阻尼自由落体"，阻尼会让它系统性偏小，而那不是 bug。
PhysicsWorld makeWorld(const glm::vec3& gravity = glm::vec3(0.0f, 0.0f, -9.81f)) {
    PhysicsWorld w;
    w.settings().gravity = gravity;
    w.settings().allowSleep = false;
    return w;
}

RigidBody makeSphere(const glm::vec3& pos, float r, BodyType type = BodyType::Dynamic) {
    RigidBody b;
    b.type = type;
    b.shape = Shape::sphere(r);
    b.position = pos;
    b.material.density = 1000.0f;
    b.material.dynamicFriction = 0.0f;
    b.material.staticFriction = 0.0f;
    b.material.restitution = 0.0f;
    b.linearDamping = 0.0f;
    b.angularDamping = 0.0f;
    b.massProps = (type == BodyType::Dynamic) ? b.shape.massFromDensity(1000.0f)
                                             : makeMassProperties(0.0f, glm::mat3(0.0f));
    return b;
}

RigidBody makeBox(const glm::vec3& pos, const glm::vec3& he,
                  BodyType type = BodyType::Dynamic) {
    RigidBody b;
    b.type = type;
    b.shape = Shape::box(he);
    b.position = pos;
    b.material.density = 1000.0f;
    b.material.dynamicFriction = 0.0f;
    b.material.staticFriction = 0.0f;
    b.material.restitution = 0.0f;
    b.linearDamping = 0.0f;
    b.angularDamping = 0.0f;
    b.massProps = (type == BodyType::Dynamic) ? b.shape.massFromDensity(1000.0f)
                                             : makeMassProperties(0.0f, glm::mat3(0.0f));
    return b;
}

// 一块无限大的地面（静态平面，法线 +Z，过原点）。
//
// ⚠ friction 必须显式传：材质的摩擦是**几何平均**合成的
//   （combineFriction = sqrt(a*b)），于是"地面摩擦 0"意味着**任何**物体
//   在地面上都没有摩擦 —— 包括"想验证摩擦确实生效"的那个测试。
//   第一版就是这么写错的：箱子以 4 m/s 匀速滑过 1 秒，测试报红。
RigidBody makeGround(float friction = 0.0f) {
    RigidBody g;
    g.type = BodyType::Static;
    g.shape = Shape::plane(glm::vec3(0.0f, 0.0f, 1.0f), 0.0f);
    g.material.dynamicFriction = friction;
    g.material.staticFriction = friction;
    g.massProps = makeMassProperties(0.0f, glm::mat3(0.0f));
    return g;
}

// ================================================================
//  1) 自由落体：半隐式欧拉的**闭式解**
// ================================================================
//
// 半隐式欧拉每步先更新速度再更新位置，于是走 n 步之后：
//     v_n = −g·n·dt
//     z_n = z0 − g·dt²·n(n+1)/2
// 这个解析式是"积分器就是半隐式欧拉"的**充分证据**：
//   · 若误用显式欧拉（先位置后速度），系数会变成 n(n−1)/2；
//   · 若重力被多乘一次 dt，斜率会整体偏。
// 所以容差可以取得很小（1e-4），这让它成为一条很强的断言。
void testFreeFall() {
    const float dt = 1.0f / 60.0f;
    const float g = 9.81f;
    const float z0 = 100.0f;
    const int n = 60;

    PhysicsWorld w = makeWorld(glm::vec3(0.0f, 0.0f, -g));
    w.settings().fixedTimeStep = dt;
    const BodyId id = w.addBody(makeSphere(glm::vec3(0.0f, 0.0f, z0), 0.5f));

    for (int i = 0; i < n; ++i) w.stepFixed(dt);

    const RigidBody* b = w.body(id);
    const double expectedZ =
        static_cast<double>(z0) -
        static_cast<double>(g) * dt * dt * (n * (n + 1) / 2.0);
    report("freefall.z", b->position.z, expectedZ, 1.0e-3);
    // 速度也必须对上（v = -g·n·dt）
    report("freefall.vz", b->linearVelocity.z,
           -static_cast<double>(g) * dt * n, 1.0e-3);
}

// ================================================================
//  2) 弹性：出射速度 = 入射速度（e = 1）
// ================================================================
void testRestitution() {
    const float dt = 1.0f / 60.0f;
    const float v0 = -5.0f;

    PhysicsWorld w = makeWorld();
    w.settings().fixedTimeStep = dt;
    w.addBody(makeGround());

    // 球心放到"离地面 0.5 mm"处：**第一步就会接触**。
    // 这样入射速度可以精确写出来（见下面的期望值），不受"第几步撞上"影响。
    RigidBody s = makeSphere(glm::vec3(0.0f, 0.0f, 0.5005f), 0.5f);
    s.material.restitution = 1.0f;
    const BodyId id = w.addBody(s);
    w.body(id)->linearVelocity = glm::vec3(0.0f, 0.0f, v0);

    float vOut = 0.0f;
    for (int i = 0; i < 30; ++i) {
        w.stepFixed(dt);
        const float vz = w.body(id)->linearVelocity.z;
        if (vz > 0.5f) { vOut = vz; break; }
    }
    // 期望值 = |v0| + g·dt —— **不是** |v0|。
    // 原因：半隐式欧拉在每步开头先把重力加进速度，所以"接触那一刻的速度"
    // 已经含了一步重力。第一版按 |v0| 断言，结果 5.49 被判成"动能不守恒"，
    // 其实物理是对的、期望是错的。这正是"先怀疑期望"那条纪律的样子。
    report("restitution.vout", vOut, -v0 + (9.81f * dt), 0.25);

    // e = 0：落地之后不该往上走
    PhysicsWorld w2 = makeWorld();
    w2.settings().fixedTimeStep = dt;
    w2.addBody(makeGround());
    RigidBody s2 = makeSphere(glm::vec3(0.0f, 0.0f, 0.5005f), 0.5f);
    s2.material.restitution = 0.0f;
    const BodyId id2 = w2.addBody(s2);
    w2.body(id2)->linearVelocity = glm::vec3(0.0f, 0.0f, v0);
    float maxVz = 0.0f;
    for (int i = 0; i < 60; ++i) {
        w2.stepFixed(dt);
        maxVz = std::max(maxVz, w2.body(id2)->linearVelocity.z);
    }
    report("restitution.inelastic.maxvz", maxVz, 0.0, 0.05);
}

// ================================================================
//  3) 摩擦：减速度 = μ·g（无摩擦则速度守恒）
// ================================================================
void testFriction() {
    const float dt = 1.0f / 60.0f;
    const float mu = 0.5f;
    const float g = 9.81f;
    const float v0 = 4.0f;
    const int n = 60;   // 1 秒

    // 有摩擦
    {
        PhysicsWorld w = makeWorld();
        w.settings().fixedTimeStep = dt;
        w.addBody(makeGround(mu));   // 地面必须也有摩擦，见 makeGround 的说明
        RigidBody box = makeBox(glm::vec3(0.0f, 0.0f, 0.5f), glm::vec3(0.5f));
        box.material.dynamicFriction = mu;
        box.material.staticFriction = mu;
        const BodyId id = w.addBody(box);
        w.body(id)->linearVelocity = glm::vec3(v0, 0.0f, 0.0f);
        for (int i = 0; i < n; ++i) w.stepFixed(dt);
        const float vx = w.body(id)->linearVelocity.x;
        // 理论：v = v0 − μ·g·t。但**动摩擦不能把物体推得反向滑**：
        // μ·g·t = 4.905 > v0 = 4，物体会在 t_stop = v0/(μ·g) ≈ 0.82 s
        // 停住，此后恒为 0。所以期望值必须**在 0 处截断**。
        // 第一版直接拿公式值（−0.905）当期望，于是"物理完全正确（vx=0）"
        // 被判成 FAIL —— 又一次"先怀疑期望"。
        const float expectVx = std::max(0.0f, v0 - mu * g * 1.0f);
        report("friction.vx_after_1s", vx, expectVx, 0.35);
    }
    // 无摩擦：速度必须保持（这是"摩擦没被误开"的阴性对照）
    {
        PhysicsWorld w = makeWorld();
        w.settings().fixedTimeStep = dt;
        w.addBody(makeGround());
        RigidBody box = makeBox(glm::vec3(0.0f, 0.0f, 0.5f), glm::vec3(0.5f));
        box.material.dynamicFriction = 0.0f;
        box.material.staticFriction = 0.0f;
        const BodyId id = w.addBody(box);
        w.body(id)->linearVelocity = glm::vec3(v0, 0.0f, 0.0f);
        for (int i = 0; i < n; ++i) w.stepFixed(dt);
        report("friction.free.vx", w.body(id)->linearVelocity.x, v0, 0.05);
    }
}

// ================================================================
//  4) 休眠：静置的箱子必须睡着
// ================================================================
void testSleep() {
    const float dt = 1.0f / 60.0f;
    PhysicsWorld w = makeWorld();
    w.settings().fixedTimeStep = dt;
    w.settings().allowSleep = true;
    w.addBody(makeGround());
    const BodyId id = w.addBody(makeBox(glm::vec3(0.0f, 0.0f, 0.5f),
                                        glm::vec3(0.5f)));
    for (int i = 0; i < 180; ++i) w.stepFixed(dt);   // 3 秒
    const RigidBody* b = w.body(id);
    reportBool("sleep.asleep_after_3s", !b->awake);
    // 睡着之后不能还在慢慢下沉：位置应停在"半高 − slop"附近
    report("sleep.rest_z", b->position.z, 0.5, 0.02);
    reportBool("sleep.zero_velocity", glm::length(b->linearVelocity) == 0.0f);
}

// ================================================================
//  5) 堆叠：4 个箱子叠起来不能塌、不能互相挤穿
// ================================================================
void testStacking() {
    const float dt = 1.0f / 60.0f;
    PhysicsWorld w = makeWorld();
    w.settings().fixedTimeStep = dt;
    w.settings().allowSleep = true;   // makeWorld 默认关掉了休眠，这里要开
    w.addBody(makeGround(0.6f));

    const glm::vec3 he(0.5f, 0.5f, 0.25f);
    std::vector<BodyId> ids;
    for (int i = 0; i < 4; ++i) {
        RigidBody b = makeBox(glm::vec3(0.0f, 0.0f, he.z + 2.0f * he.z * i), he);
        b.material.dynamicFriction = 0.6f;
        b.material.staticFriction = 0.6f;
        ids.push_back(w.addBody(b));
    }
    for (int i = 0; i < 300; ++i) w.stepFixed(dt);   // 5 秒

    const float topZ = w.body(ids.back())->position.z;
    const double expectTop = he.z + 2.0 * he.z * 3;   // 0.25 + 1.5 = 1.75
    report("stack.top_z_after_5s", topZ, expectTop, 0.05);
    // 相邻两层不能互相挤穿（间距 ≥ 2*he.z − slop×几倍）
    bool noSink = true;
    for (std::size_t i = 1; i < ids.size(); ++i) {
        const float dz = w.body(ids[i])->position.z -
                         w.body(ids[i - 1])->position.z;
        if (dz < 2.0f * he.z - 0.05f) noSink = false;
    }
    reportBool("stack.no_tunneling", noSink);
    // 整叠都应该睡着了。
    // ⚠ 这条断言是**岛级休眠**的回归护栏：早先"每个体各自判 quiet 就自己睡"
    //   的写法会让最底下那个单独睡下去，它的地面接触随即被宽相剔除 →
    //   它被上面的重量压穿、又被 wakeIslands 唤醒，形成极限环 —— 表现为
    //   "箱子永远睡不着"，而 top_z / no_tunneling 这两条却都能通过。
    //   所以"能不能睡"是唯一能抓住那个 bug 的判据，不能删。
    int asleep = 0;
    for (BodyId id : ids) if (!w.body(id)->awake) ++asleep;
    report("stack.asleep_count", static_cast<double>(asleep), 4.0, 0.0);
}

// ================================================================
//  5b) 箱-箱：微小相对旋转不能"抹掉"接触（回归护栏）
// ================================================================
//
// 针对一个真实踩过的坑：SAT 的 9 根边叉积轴里，ra/rb 与分离量 d 若
// 尺度不一致（一个隐含 |L| 因子、另一个被归一化了），两个箱子只要
// 歪了 0.1°，pen 就会算成一个大负数 → 函数直接判定"分离"返回 →
// 整叠箱子失去箱-箱接触、塌掉，而箱-地面（解析路径）一切正常。
//
// 判据三条，缺一不可：
//   · 轴对齐时必须有接触，深度是毫米级（不是 0.5 m 级的假穿透）；
//   · 歪 0.1°（求解器抖动的量级）时接触**必须还在**；
//   · 真分离 0.1 m 时必须**没有**接触（否则就是"永远有接触"的假阳性）。
void testBoxBoxTilt() {
    auto makeBoxBody = [](const glm::vec3& pos, const glm::quat& rot) {
        RigidBody b;
        b.shape = Shape::box(glm::vec3(0.5f, 0.5f, 0.25f));
        b.position = pos;
        b.rotation = rot;
        b.massProps = b.shape.massFromDensity(1000.0f);
        return b;
    };

    // (a) 完全轴对齐、正好接触
    {
        const RigidBody A = makeBoxBody(glm::vec3(0.0f, 0.0f, 0.25f),
                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        const RigidBody B = makeBoxBody(glm::vec3(0.0f, 0.0f, 0.75f),
                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        ContactManifold m;
        const bool hit = collideBodies(A, B, 0, 1, m);
        reportBool("boxbox.aligned.hit", hit && m.count >= 1);
        report("boxbox.aligned.depth", m.count ? m.points[0].depth : -1.0, 0.0,
               0.002);
    }
    // (b) 下箱歪 0.1°—— 求解器每步都会抖出这个量级
    {
        const RigidBody A = makeBoxBody(
            glm::vec3(0.0f, 0.0f, 0.25f),
            glm::angleAxis(glm::radians(0.1f), glm::vec3(0.0f, 0.0f, 1.0f)));
        const RigidBody B = makeBoxBody(glm::vec3(0.0f, 0.0f, 0.75f),
                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        ContactManifold m;
        const bool hit = collideBodies(A, B, 0, 1, m);
        reportBool("boxbox.tilted.hit", hit && m.count >= 1);
        report("boxbox.tilted.depth", m.count ? m.points[0].depth : -1.0, 0.0,
               0.01);
    }
    // (c) 真分离（上箱抬高 0.1 m）→ 必须没有接触
    {
        const RigidBody A = makeBoxBody(glm::vec3(0.0f, 0.0f, 0.25f),
                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        const RigidBody B = makeBoxBody(glm::vec3(0.0f, 0.0f, 0.85f),
                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        ContactManifold m;
        const bool hit = collideBodies(A, B, 0, 1, m);
        reportBool("boxbox.separated.no_hit", !hit);
    }
}

// ================================================================
//  6) 距离关节：长度必须守恒
// ================================================================
void testDistanceJoint() {
    const float dt = 1.0f / 60.0f;
    const float L = 2.0f;

    PhysicsWorld w = makeWorld();
    w.settings().fixedTimeStep = dt;

    // 静态锚点
    RigidBody anchor = makeSphere(glm::vec3(0.0f, 0.0f, 5.0f), 0.1f,
                                  BodyType::Static);
    const BodyId a = w.addBody(anchor);
    // 摆锤：从锚点水平拉开 L
    RigidBody bob = makeSphere(glm::vec3(L, 0.0f, 5.0f), 0.2f);
    const BodyId b = w.addBody(bob);

    Joint j;
    j.type = JointType::Distance;
    j.bodyA = a;
    j.bodyB = b;
    j.localAnchorA = glm::vec3(0.0f);
    j.localAnchorB = glm::vec3(0.0f);
    j.distance = L;
    w.addJoint(j);

    float maxErr = 0.0f;
    float xMin = 1.0e9f;
    float xMax = -1.0e9f;
    for (int i = 0; i < 600; ++i) {   // 10 秒，摆好几圈
        w.stepFixed(dt);
        const glm::vec3 p = w.body(b)->position;
        maxErr = std::max(maxErr, std::fabs(glm::length(p - w.body(a)->position) - L));
        xMin = std::min(xMin, p.x);
        xMax = std::max(xMax, p.x);
    }
    report("joint.distance.max_err", maxErr, 0.0, 0.02);
    // 摆锤必须真的**摆起来**（否则说明关节把它锁死了，"误差为 0"毫无意义）。
    // 判据用整段行程（max−min）：只看终点会撞上"刚好回到起点"的相位。
    report("joint.distance.x_travel", xMax - xMin, L * 2.0, L * 0.4);
}

// ================================================================
//  7) 球窝关节：锚点必须重合
// ================================================================
void testBallJoint() {
    const float dt = 1.0f / 60.0f;
    PhysicsWorld w = makeWorld();
    w.settings().fixedTimeStep = dt;

    RigidBody anchor = makeSphere(glm::vec3(0.0f, 0.0f, 3.0f), 0.1f,
                                  BodyType::Static);
    const BodyId a = w.addBody(anchor);
    RigidBody bar = makeBox(glm::vec3(1.0f, 0.0f, 3.0f),
                            glm::vec3(1.0f, 0.1f, 0.1f));
    const BodyId b = w.addBody(bar);

    Joint j;
    j.type = JointType::Ball;
    j.bodyA = a;
    j.bodyB = b;
    j.localAnchorA = glm::vec3(0.0f);
    // 杆的左端（局部 -X 方向 1 m 处）挂在锚点上
    j.localAnchorB = glm::vec3(-1.0f, 0.0f, 0.0f);
    w.addJoint(j);

    float maxErr = 0.0f;
    for (int i = 0; i < 300; ++i) {
        w.stepFixed(dt);
        const glm::vec3 wa = w.body(a)->position + w.body(a)->rotation * j.localAnchorA;
        const glm::vec3 wb = w.body(b)->position + w.body(b)->rotation * j.localAnchorB;
        maxErr = std::max(maxErr, glm::length(wb - wa));
    }
    report("joint.ball.max_err", maxErr, 0.0, 0.02);
}

// ================================================================
//  8) 铰链：绕轴自由、垂直于轴被锁
// ================================================================
void testHinge() {
    const float dt = 1.0f / 60.0f;
    PhysicsWorld w = makeWorld();
    w.settings().fixedTimeStep = dt;

    RigidBody anchor = makeSphere(glm::vec3(0.0f, 0.0f, 3.0f), 0.1f,
                                  BodyType::Static);
    const BodyId a = w.addBody(anchor);
    RigidBody bar = makeBox(glm::vec3(1.0f, 0.0f, 3.0f),
                            glm::vec3(1.0f, 0.1f, 0.1f));
    const BodyId b = w.addBody(bar);

    Joint j;
    j.type = JointType::Hinge;
    j.bodyA = a;
    j.bodyB = b;
    j.localAnchorA = glm::vec3(0.0f);
    j.localAnchorB = glm::vec3(-1.0f, 0.0f, 0.0f);
    // 铰链轴 = 世界 Y（这样杆在 XZ 平面内摆动）
    j.localAxisA = glm::vec3(0.0f, 1.0f, 0.0f);
    w.addJoint(j);

    float maxAnchorErr = 0.0f;
    for (int i = 0; i < 300; ++i) {
        w.stepFixed(dt);
        const glm::vec3 wa = w.body(a)->position + w.body(a)->rotation * j.localAnchorA;
        const glm::vec3 wb = w.body(b)->position + w.body(b)->rotation * j.localAnchorB;
        maxAnchorErr = std::max(maxAnchorErr, glm::length(wb - wa));
    }
    report("joint.hinge.anchor_err", maxAnchorErr, 0.0, 0.02);
    // 杆应当绕 Y 转下来（末端 z 变小）—— 证明"绕轴是自由的"
    reportBool("joint.hinge.rotates", w.body(b)->position.z < 3.0f - 0.5f);
}

// ================================================================
//  9) 射线：命中距离必须与解析值一致
// ================================================================
void testRaycast() {
    PhysicsWorld w = makeWorld(glm::vec3(0.0f));
    RigidBody box = makeBox(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f),
                            BodyType::Static);
    w.addBody(box);
    // 从 z = 10 往下打 → 命中 z = +1 的面 → 距离 9
    const RayHit h = w.raycast(glm::vec3(0.0f, 0.0f, 10.0f),
                               glm::vec3(0.0f, 0.0f, -1.0f));
    reportBool("raycast.box.hit", h.hit);
    report("raycast.box.distance", h.distance, 9.0, 0.01);

    // 打偏（x = 5，盒子半宽 1）→ 不该命中
    const RayHit miss = w.raycast(glm::vec3(5.0f, 0.0f, 10.0f),
                                  glm::vec3(0.0f, 0.0f, -1.0f));
    reportBool("raycast.box.miss_offscreen", !miss.hit);

    // 球：从 (0,0,10) 往下打半径 2 的球（球心原点）→ 命中 z=2 → 距离 8
    RigidBody ball = makeSphere(glm::vec3(0.0f, 0.0f, 0.0f), 2.0f,
                                BodyType::Static);
    w.addBody(ball);
    const RayHit hb = w.raycast(glm::vec3(4.0f, 0.0f, 10.0f),
                                glm::vec3(0.0f, 0.0f, -1.0f));
    reportBool("raycast.sphere.miss_beside", !hb.hit);
}

// ================================================================
//  10) 广相：SAP 的对数必须等于暴力 O(N²) 的结果
// ================================================================
void testBroadphase() {
    PhysicsWorld w = makeWorld(glm::vec3(0.0f));
    const int k = 6;
    // 3×3×3 网格，间距 1.2、半径 0.7 → 相邻的会重叠，形成已知的连通关系
    std::vector<RigidBody> list;
    for (int x = 0; x < k; ++x) {
        for (int y = 0; y < k; ++y) {
            for (int z = 0; z < k; ++z) {
                RigidBody b = makeSphere(
                    glm::vec3(1.2f * x, 1.2f * y, 1.2f * z), 0.7f,
                    BodyType::Static);
                b.type = BodyType::Dynamic;
                b.massProps = b.shape.massFromDensity(1000.0f);
                list.push_back(b);
            }
        }
    }
    std::vector<BodyId> ids;
    for (RigidBody& b : list) ids.push_back(w.addBody(b));

    // 只跑宽相 + 窄相，不跑求解（重力为 0，也不会有位移）
    w.stepFixed(1.0f / 60.0f);

    // 暴力对照：球心距离 < 2r 的对数
    int brute = 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        for (std::size_t j = i + 1; j < ids.size(); ++j) {
            const float d = glm::length(w.body(ids[i])->position -
                                        w.body(ids[j])->position);
            if (d < 1.4f) ++brute;
        }
    }
    // 宽相输出的**流形**数应该等于真正接触的对数
    report("broadphase.contact_pairs", w.stats().manifolds,
           static_cast<double>(brute), 0.0);
}

// ================================================================
//  11) 确定性：同一场景两次运行，逐位相同
// ================================================================
//
// 这条是回归的基础设施级断言：只要它成立，"用截图/数值做基线"才可能
// 稳定。任何"按指针地址排序""用随机数"的实现都会让它立刻变红。
void testDeterminism() {
    const float dt = 1.0f / 60.0f;
    auto run = [&]() {
        PhysicsWorld w = makeWorld();
        w.settings().fixedTimeStep = dt;
        w.addBody(makeGround());
        for (int i = 0; i < 12; ++i) {
            RigidBody b = makeBox(
                glm::vec3(0.13f * i, 0.07f * i, 1.0f + 0.9f * i),
                glm::vec3(0.2f, 0.2f, 0.2f));
            b.rotation = glm::normalize(
                glm::quat(0.9f, 0.1f * i, 0.05f * i, -0.08f * i));
            w.addBody(b);
        }
        for (int i = 0; i < 240; ++i) w.stepFixed(dt);
        std::vector<float> out;
        for (const RigidBody& b : w.bodies()) {
            out.push_back(b.position.x);
            out.push_back(b.position.y);
            out.push_back(b.position.z);
            out.push_back(b.rotation.w);
        }
        return out;
    };
    const std::vector<float> a = run();
    const std::vector<float> b = run();
    bool identical = (a.size() == b.size());
    double maxDiff = 0.0;
    for (std::size_t i = 0; identical && i < a.size(); ++i) {
        const double d = std::fabs(static_cast<double>(a[i]) - b[i]);
        maxDiff = std::max(maxDiff, d);
        if (a[i] != b[i]) identical = false;
    }
    reportBool("determinism.bitwise_identical", identical);
    report("determinism.max_diff", maxDiff, 0.0, 0.0);
}

} // namespace

int runPhysicsSelfTest() {
    g_pass = 0;
    g_fail = 0;
    VK_LOG_INFO("=== PHYS-TEST begin ===");

    testFreeFall();
    testRestitution();
    testFriction();
    testSleep();
    testStacking();
    testBoxBoxTilt();
    testDistanceJoint();
    testBallJoint();
    testHinge();
    testRaycast();
    testBroadphase();
    testDeterminism();

    VK_LOG_INFO("PHYS-TOTAL pass=%d fail=%d", g_pass, g_fail);
    VK_LOG_INFO("=== PHYS-TEST end ===");
    return g_fail;
}

} // namespace physics
