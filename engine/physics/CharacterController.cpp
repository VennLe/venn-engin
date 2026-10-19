#include "physics/CharacterController.h"

#include "physics/ConvexCollision.h"
#include "physics/PhysicsShape.h"
#include "physics/PhysicsWorld.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace physics {

namespace {

constexpr float kGravity = -9.81f;
// 分步长度上限。见头文件"分步长度为什么是 0.05 m"。
constexpr float kMaxSubStep = 0.05f;

float length2D(const glm::vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// 把 a 以最大步长 maxDelta 拉向 b（逐分量，不越过）
glm::vec3 approach(const glm::vec3& a, const glm::vec3& b, float maxDelta) {
    const glm::vec3 d = b - a;
    const float len = glm::length(d);
    if (len <= maxDelta || len < 1.0e-8f) return b;
    return a + d * (maxDelta / len);
}

// 与单个体做一次"最小平移"，成功则把 probe 推出去。
// 返回法线（从探针指向障碍）；depth 为穿透深度。
bool pushOutOfBody(const RigidBody& b, const Shape& probeShape,
                   const glm::vec3& probePos, float halfHeight, float radius,
                   glm::vec3& normal, float& depth) {
    if (b.id == kInvalidBody) return false;
    if (!b.shape.valid()) return false;
    if (b.hasFlag(kBodyIsTrigger)) return false;

    if (b.shape.isPlane()) {
        const glm::vec3 pn = b.rotation * b.shape.planeNormal;
        const float D = b.shape.planeOffset + glm::dot(pn, b.position);
        const float d2 = glm::dot(pn, probePos) - D - (halfHeight + radius);
        if (d2 >= 0.0f) return false;
        // ⚠ 本函数的 `normal` 统一是**推出方向**（从障碍指向探针）。
        //   平面的 pn 是"半空间外法线"，正好就是推出方向 —— 所以这里是 +pn，
        //   而凸体路径要取反（Contact::normal 是 A→B，即探针→障碍）。
        //   历史上 moveCharacter 在平面分支用了 `pos -= pn*depth`，语义是
        //   "探针→障碍"，结果每迭代一次就往地里多推一个 depth（指数发散）。
        normal = pn;
        depth = -d2;
        if (!std::isfinite(depth) || !std::isfinite(normal.x) ||
            !std::isfinite(normal.y) || !std::isfinite(normal.z))
            return false;
        return true;
    }

    const TransformedShape probe = probeShape.transformed(
        glm::translate(glm::mat4(1.0f), probePos));
    const Contact c = collide(probe, b.shape.transformed(b.worldMatrix()));
    if (!c.hit || c.depth <= 0.0f) return false;

    // ⚠ GJK/EPA 在"刚刚接触 / 退化单纯形"时会给出 NaN 法线或深度。
    //   一个 NaN 就能把角色位置永久污染成 NaN（之后所有比较都是 false，
    //   表现为"角色消失、镜头飞到无穷远"）。宁可选这一帧不推，也不能收。
    if (!std::isfinite(c.depth) || !std::isfinite(c.normal.x) ||
        !std::isfinite(c.normal.y) || !std::isfinite(c.normal.z))
        return false;
    if (glm::length(c.normal) < 0.5f) return false;

    // Contact::normal 是"探针 → 障碍"，取反才是推出方向
    normal = -c.normal;
    // 深度上限：一次推出超过一个胶囊直径一定是数值异常（不是真穿透）
    depth = std::min(c.depth, (radius + halfHeight) * 2.0f);
    return true;
}

// 胶囊放在 pos 上时，是否与任何**实心**体发生超过 tolerance 的穿透。
bool capsuleFree(const PhysicsWorld& world, float radius, float halfHeight,
                 const glm::vec3& pos, BodyId ignore, float tolerance) {
    const Shape probeShape = Shape::capsule(radius, halfHeight);
    for (const RigidBody& b : world.bodies()) {
        if (b.id == ignore) continue;
        glm::vec3 n(0.0f);
        float depth = 0.0f;
        if (!pushOutOfBody(b, probeShape, pos, halfHeight, radius, n, depth))
            continue;
        if (depth > tolerance) return false;
    }
    return true;
}

// 从 from 沿 -z 最多降 maxDrop，回答"**停在**哪个高度"。
//
// ⚠ 为什么不能直接用 sweepCapsule 往下扫：那个函数是"推出式"的 —— 撞上
//   台阶的上棱之后，推出方向带水平分量（棱的法线是斜的），胶囊会沿着棱
//   **一边下滑一边往后退**，最后退回到台阶正面的墙根（实测落点 z 从本该
//   的 1.074 一路退到 0.915，即"又站回地面上了"）。于是上台阶分支每帧都
//   判"成功"、位置却一点没往前 —— 角色永远卡在台阶前 0.006 m 处。
//   向下找**落点**要的是"撞到就停"，不是"沿着棱滑"。
//   做法：先试最底，没挡就到底；有挡就二分（10 次 → 精度 < 0.4 mm）。
float dropUntilContact(const PhysicsWorld& world, float radius, float halfHeight,
                       const glm::vec3& from, float maxDrop, BodyId ignore,
                       bool& contacted) {
    constexpr float kTol = 1.0e-3f;
    const glm::vec3 bottom = from - glm::vec3(0.0f, 0.0f, maxDrop);
    if (capsuleFree(world, radius, halfHeight, bottom, ignore, kTol)) {
        contacted = false;
        return bottom.z;
    }
    contacted = true;
    float lo = 0.0f;          // 这一段是空的（含 from 本身）
    float hi = maxDrop;       // 这一段被挡住
    for (int i = 0; i < 10; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (capsuleFree(world, radius, halfHeight,
                        from - glm::vec3(0.0f, 0.0f, mid), ignore, kTol))
            lo = mid;
        else
            hi = mid;
    }
    return from.z - lo;
}

} // namespace

CapsuleSweep sweepCapsule(const PhysicsWorld& world, float radius,
                          float halfHeight, const glm::vec3& from,
                          const glm::vec3& delta, BodyId ignore) {
    CapsuleSweep out;
    out.position = from;

    const float len = glm::length(delta);
    if (len < 1.0e-6f) return out;

    const float stepLen = std::max(std::min(radius * 0.5f, kMaxSubStep), 0.01f);
    const int steps =
        std::min(64, std::max(1, static_cast<int>(std::ceil(len / stepLen))));
    const glm::vec3 stepDelta = delta / static_cast<float>(steps);

    const Shape probeShape = Shape::capsule(radius, halfHeight);

    for (int s = 0; s < steps; ++s) {
        out.position += stepDelta;

        // 一个子步内反复推出（3 轮足够把"墙角"这种多面接触收敛）
        for (int iter = 0; iter < 3; ++iter) {
            bool pushed = false;
            for (const RigidBody& b : world.bodies()) {
                if (b.id == ignore) continue;
                glm::vec3 n(0.0f);
                float depth = 0.0f;
                if (!pushOutOfBody(b, probeShape, out.position, halfHeight,
                                   radius, n, depth))
                    continue;
                out.position += n * depth;
                if (!std::isfinite(out.position.x) || !std::isfinite(out.position.y) ||
                    !std::isfinite(out.position.z)) {
                    // 兜底：退回这一步之前的位置（宁可卡一下，也不能变 NaN）
                    out.position -= n * depth;
                    break;
                }
                out.hit = true;
                if (n.z < out.minNormalZ) {
                    out.minNormalZ = n.z;
                    out.normal = n;
                }
                pushed = true;
            }
            if (!pushed) break;
        }
    }

    out.moved = glm::length(out.position - from);
    return out;
}

CharacterStepResult stepCharacter(const PhysicsWorld& world,
                                  const CharacterDesc& desc,
                                  CharacterState& state,
                                  const glm::vec3& desiredVelocity, float dt,
                                  bool jumpRequested, BodyId ignore) {
    CharacterStepResult out;
    if (dt <= 0.0f) {
        out.position = state.position;
        out.velocity = state.velocity;
        out.onGround = state.onGround;
        return out;
    }

    const float cosSlope =
        std::cos(glm::radians(glm::clamp(desc.slopeLimitDeg, 1.0f, 89.0f)));

    // ---------------- ① 水平速度 ----------------
    glm::vec3 hv(state.velocity.x, state.velocity.y, 0.0f);
    glm::vec3 target(desiredVelocity.x, desiredVelocity.y, 0.0f);
    if (length2D(target) > desc.maxSpeed && length2D(target) > 1.0e-6f) {
        target = glm::normalize(target) * desc.maxSpeed;
    }
    const float ctl = state.onGround ? 1.0f : desc.airControl;
    hv = approach(hv, target, desc.acceleration * ctl * dt);

    // ---------------- ② 竖直速度 ----------------
    float vz = state.velocity.z;
    if (state.onGround && vz <= 0.0f) {
        // 站在地上：竖直速度归零。**不要**给"向下粘力"—— 在斜坡上求解器
        // 会沿法线把胶囊推开，那个推力的水平分量会让角色自己往下坡飘。
        // 贴地由 ⑥ 的竖直射线负责（它只改 z）。
        vz = 0.0f;
    }
    if (jumpRequested && state.onGround) {
        vz = desc.jumpSpeed;
        out.jumped = true;
        state.onGround = false;
    }
    // 贴地时**不加重力**：加了就必然穿进地面一点点，然后被沿法线推出 ——
    // 在斜坡上那个推力的水平分量就是"站着也慢慢往下坡飘"的来源（实测
    // 40° 坡 1 秒飘 0.1 m）。贴地/下坡由 ⑥ 的竖直射线负责。
    if (!state.onGround) vz += kGravity * desc.gravityScale * dt;

    // ---------------- ③ 水平扫掠 ----------------
    const glm::vec3 from = state.position;
    const glm::vec3 hDelta(hv.x * dt, hv.y * dt, 0.0f);
    CapsuleSweep hs = sweepCapsule(world, desc.radius, desc.halfHeight, from,
                                   hDelta, ignore);
    glm::vec3 pos = hs.position;

    const bool wallContact = hs.hit && hs.minNormalZ < cosSlope;
    // ⚠ "站不住"和"是墙"是**两个**判断，别用同一个阈值：
    //   · 站不住（nz < cosSlope）→ 不算地面，重力继续作用（斜坡滑落）
    //   · 是墙（nz < kWallNormalZ，即基本竖直）→ 抹掉撞进墙的那一分量
    //   台阶的接触是**上棱**，法线是斜的（实测 nz ≈ 0.64）—— 落在两者
    //   之间：它不是地面，但也绝不是墙，它是"正在往上爬"。早期两者共用
    //   `cosSlope`，于是爬上台阶的每一帧水平速度都被清零一次，0.3 m 的
    //   台阶要蹭五十多帧才过得去（手感上就是"撞了一下再慢慢挤上去"）。
    constexpr float kWallNormalZ = 0.35f;
    if (hs.hit && hs.minNormalZ < kWallNormalZ) {
        // 消掉"撞进墙"的那一分量 → 自然沿墙滑。**不反弹**：法线方向只
        // 归零、不取反。
        glm::vec3 n = hs.normal;
        n.z = 0.0f;
        if (glm::length(n) > 1.0e-6f) {
            n = glm::normalize(n);
            // n 是推出方向（朝自由空间）：点乘为负 = 正在往墙里走
            const float into = glm::dot(hv, n);
            if (into < 0.0f) hv -= n * into;
        }
    }

    // ---------------- ④ 台阶 ----------------
    const float want = length2D(hDelta);
    const float got = length2D(pos - from);
    bool stepped = false;
    // ⚠ 台阶探测用的是**期望**位移（target × dt），不是"实际走出来的位移"。
    //   撞墙时 `hv` 已经被归一化到沿墙（正面撞墙 = 0），拿它去试台阶永远
    //   只试 0.01 m —— 角色会永远卡在台阶前。这是"想走多远"与"走了多远"
    //   必须分开记的典型例子。
    const glm::vec3 stepDelta(target.x * dt, target.y * dt, 0.0f);
    const float intended = length2D(stepDelta);
    if (desc.stepHeight > 1.0e-4f && intended > 1.0e-5f && state.onGround &&
        wallContact && got < intended * 0.5f) {
        const glm::vec3 raised = from + glm::vec3(0.0f, 0.0f, desc.stepHeight);
        const CapsuleSweep hs2 = sweepCapsule(world, desc.radius,
                                              desc.halfHeight, raised, stepDelta,
                                              ignore);
        const float got2 = length2D(hs2.position - raised);
        if (got2 > got + 0.01f) {
            // 抬起来能过去 → 再落回。落点用"撞到就停"的扫掠求，
            // 水平位置**保持 hs2 的结果**（落回只该改高度）。
            bool landedOnSomething = false;
            const float downZ =
                dropUntilContact(world, desc.radius, desc.halfHeight, hs2.position,
                                 desc.stepHeight + 0.02f, ignore, landedOnSomething);
            const glm::vec3 landed(hs2.position.x, hs2.position.y, downZ);
            // 判据是"**沿运动方向**比原来推进了多少"，不是"落回来了没有"：
            // 台阶边缘的接触法线是斜的（顶面 + 立面），按"可站法线"过滤会把
            // 这一步判成失败，角色会永远停在台阶前（踩过）。
            const glm::vec3 dirH = stepDelta / std::max(intended, 1.0e-6f);
            const float ahead = glm::dot(landed - from, dirH);
            // 判据是"**沿运动方向**比原来推进了多少"，不是"落回来了没有"：
            // 台阶边缘的接触法线是斜的（顶面 + 立面），按"可站法线"过滤会把
            // 这一步判成失败，角色会永远停在台阶前（踩过）。
            //
            // ⚠ 但阈值得是**意图的一半**，不能是固定 5 mm：太松的话"贴着
            //   0.6 m 台阶往上挪了 2 cm、落在了台阶的顶棱上"也算成功 ——
            //   角色会被一步一步抬上它根本跨不上去的高度（踩过：B3 期望
            //   停在 0.9，实测被抬到 1.218）。
            if (landedOnSomething && ahead > got + intended * 0.5f) {
                pos = landed;
                stepped = true;
            }
        }
    }

    // ---------------- ⑤ 竖直扫掠 ----------------
    if (std::fabs(vz) > 1.0e-6f) {
        const CapsuleSweep vs = sweepCapsule(
            world, desc.radius, desc.halfHeight, pos,
            glm::vec3(0.0f, 0.0f, vz * dt), ignore);
        pos = vs.position;
        // 顶到天花板同样归零（否则会一直贴着天花板"顶"）
        if (vs.hit && (vz < 0.0f ? vs.minNormalZ >= cosSlope : true)) vz = 0.0f;
    }

    // ---------------- ⑥ 贴地：**竖直射线**，只改 z ----------------
    //
    // ⚠ 为什么不用"向下扫掠一段再取位置"：在斜坡上，扫掠的推出方向是法线，
    //   带**水平分量** —— 每帧被推 sideways 一点点，站着不动也会自己往下坡
    //   飘（实测 40° 坡 1 秒飘了 12.9 m）。射线只回答"正下方多远有地面"，
    //   补偿也只沿 z 做，于是站着就是真的站着。
    bool onGround = false;
    float groundNz = 1.0f;
    const float snap = desc.groundSnap
                           ? std::max(desc.groundSnapDistance, 0.02f)
                           : 0.02f;
    if (vz <= 1.0e-4f) {
        const float halfTotal = desc.halfHeight + desc.radius;
        const float maxD = halfTotal / std::max(cosSlope, 0.2f) + snap;
        glm::vec3 origin = pos;
        float travelled = 0.0f;

        // ---- 一个地面候选能不能站、要不要吸附 ----
        // 抽成 lambda 是因为现在有**两个**来源（高度场 / 物理体射线），
        // 判定必须逐字相同 —— 两份拷贝一旦漂移，就会出现"地形的坡站得住、
        // 木箱的坡站不住"这种没法解释的差异。
        auto tryGround = [&](float dist, float nz) -> bool {
            // 斜面上下垂距离天然比平面长 (hh+r)/nz；用这个基准算"多余量"
            const float need = halfTotal / std::max(nz, 0.2f);
            const float excess = dist - need;
            if (excess < -0.02f || excess > snap) return false;
            onGround = true;
            groundNz = nz;
            if (excess > 0.0f && desc.groundSnap) {
                // ⚠ 吸附是"就近贴地"的启发式，**不能凌驾于"不许穿模"之上**。
                //   胶囊搭在台阶棱上时，中心**正下方**是下面那层地面，一按
                //   就按进台阶的立面里 —— 于是"刚踩上台阶"被下一帧的吸附
                //   抵消，角色永远卡在台阶前 0.006 m 处（踩过两次）。
                //   中心射线本来就看不见"支撑在胶囊侧面（棱）"这种情形，
                //   这里用"按下去之后真的不穿"来兜住它。
                //   小量吸附直接放行：普通下坡每帧只差 0.01~0.06 m，为它
                //   多跑一遍全场景 collide 不值当。
                if (excess <= 0.02f ||
                    capsuleFree(world, desc.radius, desc.halfHeight,
                                pos - glm::vec3(0.0f, 0.0f, excess),
                                ignore, 5.0e-3f)) {
                    pos.z -= excess;
                }
            }
            return true;
        };

        // ---- ① 先问高度场（W4-3）----
        // 只取"可站"的交点（n.z ≥ cosSlope）：陡坡不算地面，于是重力继续
        // 作用、沿坡滑下去 —— 与物理体的坡度判定是同一个阈值。
        float hfDist = -1.0f;
        float hfNz = 1.0f;
        if (desc.ground && desc.ground->valid()) {
            HeightHit hh;
            if (raycast(*desc.ground, origin, glm::vec3(0.0f, 0.0f, -1.0f),
                        maxD, hh) &&
                hh.normal.z >= cosSlope) {
                hfDist = hh.t;
                hfNz = hh.normal.z;
            }
        }

        // ---- ② 再问物理世界 ----
        for (int i = 0; i < 4; ++i) {
            const RayHit h = world.raycast(origin, glm::vec3(0.0f, 0.0f, -1.0f),
                                           maxD - travelled, ignore);
            if (!h.hit) break;
            const RigidBody* rb = world.body(h.body);
            if (rb && rb->hasFlag(kBodyIsTrigger)) {
                // 触发体不是地面：从它下面一点点继续往下打
                origin = h.point - glm::vec3(0.0f, 0.0f, 1.0e-3f);
                travelled += h.distance + 1.0e-3f;
                continue;
            }
            const float nz = h.normal.z;
            // 只有"可站"的接触才算地面：55° 的坡法线 n.z≈0.57 < cos46°≈0.69，
            // 于是它不算地面 → 重力继续作用 → 沿坡滑下去。
            if (nz >= cosSlope) {
                const float dist = travelled + h.distance;
                // 地形比这个体更近 → 真正支撑角色的是地形（站在山脚下的
                // 木箱上时，箱面才是支撑面；反过来站在山坡上、脚下 10 m 处
                // 有个箱子，箱子不该把角色拽下去）。
                if (hfDist >= 0.0f && hfDist < dist) break;
                tryGround(dist, nz);
            }
            break;
        }
        // ---- ③ 物理侧没有可站的面（或地形更近）→ 用高度场 ----
        if (!onGround && hfDist >= 0.0f) tryGround(hfDist, hfNz);
    }

    state.position = pos;
    state.velocity = glm::vec3(hv.x, hv.y, vz);
    state.onGround = onGround;
    state.groundNormalZ = groundNz;
    state.timeSinceGrounded = onGround ? 0.0f : state.timeSinceGrounded + dt;

    out.position = pos;
    out.velocity = state.velocity;
    out.onGround = onGround;
    out.hitWall = wallContact;
    out.steppedUp = stepped;
    out.groundNormalZ = groundNz;
    out.movedDistance = length2D(pos - from);
    return out;
}

// ============================================================ 自检

namespace {

int g_charFail = 0;
int g_charPass = 0;

void ccheck(const char* what, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_charPass;
        std::printf("CHAR-TEST pass %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    } else {
        ++g_charFail;
        std::printf("CHAR-TEST FAIL %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    }
}

BodyId addStatic(PhysicsWorld& w, const Shape& shape, const glm::vec3& pos,
                 const glm::quat& rot = glm::quat(1, 0, 0, 0)) {
    RigidBody b;
    b.type = BodyType::Static;
    b.shape = shape;
    b.position = pos;
    b.rotation = rot;
    b.setFlag(kBodyUseGravity, false);
    return w.addBody(std::move(b));
}

// 一条标准测试世界：地面 + 两个台阶 + 薄墙
struct TestArena {
    PhysicsWorld world;
    BodyId ground = kInvalidBody;
    TestArena() {
        world.settings().allowSleep = false;
        ground = addStatic(world, Shape::plane(glm::vec3(0, 0, 1), 0.0f),
                           glm::vec3(0.0f));
        // 0.3 m 台阶（中心 z = 0.15，xz 各 1 m 半长）
        addStatic(world, Shape::box(glm::vec3(1.0f, 1.0f, 0.15f)),
                  glm::vec3(3.0f, 0.0f, 0.15f));
        // 0.6 m 台阶
        addStatic(world, Shape::box(glm::vec3(1.0f, 1.0f, 0.30f)),
                  glm::vec3(6.0f, 0.0f, 0.30f));
        // 0.1 m 薄墙（用于 50 m/s 穿透测试）
        addStatic(world, Shape::box(glm::vec3(0.05f, 2.0f, 1.0f)),
                  glm::vec3(12.0f, 0.0f, 1.0f));
    }
};

// 单级台阶的测试世界：地面 + 一级台阶 + **末端一道矮墙**。
//
// ⚠ 为什么末端要砌墙、顶面要加宽：断言"能上台阶"必须让角色**停在台阶顶上**。
//   台阶只有 2 m 长时，4.5 m/s 走 150 帧（11 m）早就跨过去、从另一侧掉下来，
//   最后测到的是"掉下去之后站在地上"，与台阶能力无关（历史 B1/B2 就是这么
//   错的：期望 z≈1.2，实测永远是 0.9）。
struct StepArena {
    PhysicsWorld world;
    static constexpr float kTopEnd = 5.7f;   // 台阶顶面一直铺到墙根
    static constexpr float kWallX = 5.5f;    // 矮墙正面
    explicit StepArena(float h) {
        world.settings().allowSleep = false;
        addStatic(world, Shape::plane(glm::vec3(0, 0, 1), 0.0f), glm::vec3(0.0f));
        // 台阶：x ∈ [2.0, 5.7]，顶面 z = h
        addStatic(world, Shape::box(glm::vec3(1.85f, 1.0f, h * 0.5f)),
                  glm::vec3(3.85f, 0.0f, h * 0.5f));
        // 矮墙：x ∈ [5.5, 5.7]，z ∈ [0, 2] —— 挡住角色，让它停在台阶顶上
        addStatic(world, Shape::box(glm::vec3(0.1f, 2.0f, 1.0f)),
                  glm::vec3(5.6f, 0.0f, 1.0f));
    }
};

CharacterState makeState(const glm::vec3& center) {
    CharacterState s;
    s.position = center;
    s.onGround = true;
    return s;
}

// 跑 N 步，返回最后一步
CharacterStepResult walk(const PhysicsWorld& world, const CharacterDesc& d,
                         CharacterState& s, const glm::vec3& vel, int steps,
                         float dt = 1.0f / 60.0f) {
    CharacterStepResult r;
    for (int i = 0; i < steps; ++i) r = stepCharacter(world, d, s, vel, dt);
    return r;
}

} // namespace

int runCharacterSelfTest() {
    g_charPass = 0;
    g_charFail = 0;
    std::printf("CHAR-TEST begin (no window)\n");

    const CharacterDesc desc;   // radius .35 / halfHeight .55 → 总高 1.8

    // ---- A. 站在地面上：不进地面、被判定为贴地 ----
    {
        TestArena a;
        CharacterState s = makeState(glm::vec3(0.0f, 0.0f, 0.9f));
        const CharacterStepResult r = walk(a.world, desc, s, glm::vec3(0.0f), 30);
        ccheck("A1 静止 30 步仍然贴地", r.onGround, "z=" + std::to_string(r.position.z));
        ccheck("A2 不会陷进地面（z ≈ 0.9）", std::fabs(r.position.z - 0.9f) < 0.01f,
               "z=" + std::to_string(r.position.z));
        ccheck("A3 水平零漂移", std::fabs(r.position.x) < 1.0e-4f &&
                                     std::fabs(r.position.y) < 1.0e-4f);
    }

    // ---- B. 台阶：0.3 m 能上，0.6 m 上不去 ----
    // 两个用例都走到"被末端矮墙顶住"为止 —— 那时角色的高度就是结论：
    // 站台阶顶 = 0.9 + h，站地面 = 0.9。
    {
        StepArena a(0.3f);
        CharacterState s = makeState(glm::vec3(1.6f, 0.0f, 0.9f));
        const CharacterStepResult r =
            walk(a.world, desc, s, glm::vec3(4.5f, 0.0f, 0.0f), 150);
        ccheck("B1 0.3 m 台阶能上去（中心 z ≈ 1.2）",
               std::fabs(r.position.z - 1.2f) < 0.06f,
               "z=" + std::to_string(r.position.z));
        ccheck("B2 上台阶之后站在台阶顶面上（x > 2.2）", r.position.x > 2.2f,
               "x=" + std::to_string(r.position.x));
        ccheck("B2b 上台阶之后仍然贴着台阶顶面", r.onGround,
               "z=" + std::to_string(r.position.z));
    }
    {
        StepArena a(0.6f);
        CharacterState s = makeState(glm::vec3(1.6f, 0.0f, 0.9f));
        const CharacterStepResult r =
            walk(a.world, desc, s, glm::vec3(4.5f, 0.0f, 0.0f), 150);
        ccheck("B3 0.6 m 台阶上不去（z 仍 ≈ 0.9）",
               std::fabs(r.position.z - 0.9f) < 0.06f,
               "z=" + std::to_string(r.position.z));
        ccheck("B4 不穿模（停在台阶面之前）",
               r.position.x <= 2.0f - desc.radius + 0.02f,
               "x=" + std::to_string(r.position.x));
    }

    // ---- C. 斜坡：40° 站得住，55° 滑下去 ----
    auto slopeWorld = [](float deg) {
        const float a = glm::radians(deg);
        // 法线朝上倾斜：n = (sin a, 0, cos a) → 沿 -X 方向升高
        return Shape::plane(glm::vec3(std::sin(a), 0.0f, std::cos(a)), 0.0f);
    };
    {
        PhysicsWorld w;
        w.settings().allowSleep = false;
        addStatic(w, slopeWorld(40.0f), glm::vec3(0.0f));
        CharacterState s = makeState(glm::vec3(0.0f));
        // 把脚放到坡面上：中心沿法线偏移 (hh + r)
        const float a = glm::radians(40.0f);
        s.position = glm::vec3(0.0f, 0.0f, 0.0f) +
                     glm::vec3(std::sin(a), 0.0f, std::cos(a)) *
                         (desc.halfHeight + desc.radius);
        walk(w, desc, s, glm::vec3(0.0f), 10);
        const glm::vec3 before = s.position;
        const CharacterStepResult r = walk(w, desc, s, glm::vec3(0.0f), 60);
        ccheck("C1 40° 坡上仍然贴地", r.onGround,
               "z=" + std::to_string(r.position.z));
        ccheck("C2 40° 坡上不滑（60 步位移 < 0.05 m）",
               glm::length(r.position - before) < 0.05f,
               "d=" + std::to_string(glm::length(r.position - before)));
    }
    {
        PhysicsWorld w;
        w.settings().allowSleep = false;
        addStatic(w, slopeWorld(55.0f), glm::vec3(0.0f));
        const float a = glm::radians(55.0f);
        CharacterState s = makeState(glm::vec3(0.0f));
        s.position = glm::vec3(std::sin(a), 0.0f, std::cos(a)) *
                     (desc.halfHeight + desc.radius);
        const glm::vec3 before = s.position;
        const CharacterStepResult r = walk(w, desc, s, glm::vec3(0.0f), 90);
        ccheck("C3 55° 坡上贴不住（onGround = false）", !r.onGround);
        ccheck("C4 55° 坡上会滑下去（高度下降 > 0.2 m）",
               before.z - r.position.z > 0.2f,
               "dz=" + std::to_string(before.z - r.position.z));
    }

    // ---- D. 下坡贴地：Y 坐标不出现"离地再落回"的振荡 ----
    {
        PhysicsWorld w;
        w.settings().allowSleep = false;
        addStatic(w, slopeWorld(10.0f), glm::vec3(0.0f));
        const float a = glm::radians(10.0f);
        CharacterState s = makeState(glm::vec3(0.0f));
        s.position = glm::vec3(0.0f) + glm::vec3(std::sin(a), 0.0f, std::cos(a)) *
                                          (desc.halfHeight + desc.radius);
        walk(w, desc, s, glm::vec3(0.0f), 5);
        // 平面 {x | dot(n,x)=0} 即 z = -tan(a)·x → +x 方向是**下坡**
        const glm::vec3 downhill(std::cos(a), 0.0f, 0.0f);
        float prevZ = s.position.z;
        int bumps = 0;
        for (int i = 0; i < 120; ++i) {
            stepCharacter(w, desc, s, downhill * 3.0f, 1.0f / 60.0f);
            if (s.position.z > prevZ + 1.0e-4f) ++bumps;   // 上升 = 抖动
            prevZ = s.position.z;
        }
        ccheck("D1 走下 10° 坡时高度单调下降（无弹跳）", bumps == 0,
               "上升帧数=" + std::to_string(bumps));
        ccheck("D2 下坡全程贴地", s.onGround);
    }

    // ---- E. 高速不穿墙：50 m/s 撞 0.1 m 薄墙 ----
    {
        TestArena a;
        CharacterState s = makeState(glm::vec3(9.0f, 0.0f, 0.9f));
        walk(a.world, desc, s, glm::vec3(50.0f, 0.0f, 0.0f), 120);
        ccheck("E1 50 m/s 撞薄墙不穿过去（x < 墙前）",
               s.position.x < 12.0f - 0.05f,
               "x=" + std::to_string(s.position.x));
        ccheck("E2 撞墙后仍站在地面上", s.onGround,
               "z=" + std::to_string(s.position.z));
    }

    // ---- F. 起跳：贴地才跳得起来，空中不重复跳 ----
    {
        TestArena a;
        CharacterState s = makeState(glm::vec3(0.0f, 0.0f, 0.9f));
        stepCharacter(a.world, desc, s, glm::vec3(0.0f), 1.0f / 60.0f);
        const CharacterStepResult j = stepCharacter(
            a.world, desc, s, glm::vec3(0.0f), 1.0f / 60.0f, /*jump=*/true);
        ccheck("F1 贴地起跳成功（vz > 0）", j.jumped && j.velocity.z > 0.0f,
               "vz=" + std::to_string(j.velocity.z));

        CharacterState air = s;
        air.onGround = false;
        const CharacterStepResult j2 = stepCharacter(
            a.world, desc, air, glm::vec3(0.0f), 1.0f / 60.0f, /*jump=*/true);
        ccheck("F2 空中按跳不起作用（二段跳由脚本决定）", !j2.jumped);
    }

    std::printf("CHAR-TOTAL pass=%d fail=%d\n", g_charPass, g_charFail);
    return g_charFail;
}

} // namespace physics
