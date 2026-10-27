#include "PlayerController.h"

#include "scene/SpringArm.h"
#include "scene/LookLimits.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "physics/PhysicsWorld.h"
#include "scene/Scene.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace runtime {

namespace {

float wrapPi(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

float lerpAngle(float a, float b, float t) {
    return a + wrapPi(b - a) * std::clamp(t, 0.0f, 1.0f);
}

constexpr float kPi = 3.14159265f;

// 物理层的 desc.maxSpeed 是**输入速度的硬上限**（stepCharacter 会把超过它
// 的期望速度截回去）。冲刺是给期望速度乘 sprintMultiplier，所以这个上限
// 必须留到冲刺速度 —— 否则按住 Shift 也只是"想要 9 m/s、被截成 4.5 m/s"，
// 表现就是"冲刺完全没反应"（动画状态机的 run 阈值永远够不到）。
// 半径 = 15 cm 的球在高度场上的近似扫掠：**沿射线采样**，第一次穿到
// 地表以下就算命中。为什么不用解析解：heightfield 是离散网格 + 双线性
// 插值，解析解要解四次方程，不值得 —— 24 步的误差在 4 m 臂长上是 2 cm 级。
float terrainCastAlong(const physics::Heightfield& hf, const glm::vec3& o,
                       const glm::vec3& d, float maxDist) {
    constexpr int kSteps = 24;
    for (int i = 1; i <= kSteps; ++i) {
        const float t = maxDist * static_cast<float>(i) / kSteps;
        const glm::vec3 p = o + d * t;
        if (p.z < physics::heightAt(hf, p.x, p.y)) return t;
    }
    return maxDist;
}

// 把"刚体射线 + 地形"合成弹簧臂用的那一个查询。
//
// ⚠ `ignore` 只忽略角色自己的运动学代理。**不能**连整条父链一起忽略 ——
//   角色的网格是**独立实体**、不在物理世界里，所以不用管。
scene::SpringArmCast makeWorldCast(const physics::PhysicsWorld* world,
                                   const physics::Heightfield* ground,
                                   physics::BodyId ignore) {
    return [world, ground, ignore](const glm::vec3& o, const glm::vec3& d,
                                   float maxDist) -> float {
        float best = maxDist;
        if (world) {
            const physics::RayHit hit = world->raycast(o, d, maxDist, ignore);
            if (hit.hit && hit.distance < best) best = hit.distance;
        }
        if (ground && ground->valid()) {
            const float t = terrainCastAlong(*ground, o, d, best);
            if (t < best) best = t;
        }
        return best;
    };
}

float physicsMaxSpeed(const ecs::CharacterControllerComponent& cc) {
    const float sprint = cc.sprintMultiplier > 1.0f ? cc.sprintMultiplier : 1.0f;
    return cc.maxSpeed * sprint;
}

} // namespace

PlayerController::PlayerController(core::InputMap& input) : m_input(input) {}

bool PlayerController::attach(scene::Scene& scene, physics::PhysicsWorld* world) {
    detach();

    ecs::Entity found{};
    scene.world().each<ecs::CharacterControllerComponent>(
        [&](ecs::Entity e, ecs::CharacterControllerComponent& cc) {
            if (!found.valid() && cc.enabled) found = e;
        });
    if (!found.valid()) return false;

    m_entity = found;
    auto* cc = scene.world().get<ecs::CharacterControllerComponent>(m_entity);
    auto* tc = scene.world().get<ecs::TransformComponent>(m_entity);
    if (!cc || !tc) return false;

    m_desc.radius = cc->radius;
    m_desc.halfHeight = cc->halfHeight;
    m_desc.stepHeight = cc->stepHeight;
    m_desc.slopeLimitDeg = cc->slopeLimitDeg;
    m_desc.groundSnap = cc->groundSnap;
    m_desc.groundSnapDistance = cc->groundSnapDistance;
    m_desc.gravityScale = cc->gravityScale;
    m_desc.maxSpeed = physicsMaxSpeed(*cc);
    m_desc.jumpSpeed = cc->jumpSpeed;
    m_desc.acceleration = cc->acceleration;
    m_desc.airControl = cc->airControl;
    m_desc.pushStrength = cc->pushStrength;
    // 地形高度场（第十轮，见 sync 里同一行）。
    m_desc.ground = world ? world->heightfield() : nullptr;

    // 实体原点是**脚底**（与编辑器里摆人的习惯一致），物理状态要的是
    // 胶囊中心 —— 差一个 (halfHeight + radius)。
    const float halfTotal = cc->halfHeight + cc->radius;
    m_state = physics::CharacterState{};
    m_state.position = tc->position + glm::vec3(0.0f, 0.0f, halfTotal);

    // ---- 出生点不能陷在地形里（2026-10-22 第十轮）----
    //
    // 用户原话："物理特性上不允许说有任何物体时能陷入其中的，包括 GameStart。"
    //
    // 根因有两个，**都要修**：
    //   ① 模板地形曾经在原点处是 3.35 m（归一化基准取的是全局最低点），
    //      而 GameStart 写的是 z=0 → 一开局脚就在土里 3.35 m；这条已经在
    //      `tools/gen_templates.py` 里修掉（出生点 = 0 m）。
    //   ② 用户也可能把 GameStart 摆到山坡上、或把地形刷高 —— 所以这里兜一次：
    //      脚底低于地表就抬到地表。
    //
    // ⚠ 为什么放在 **attach（出生那一刻）**，而不是每一步的 `stepCharacter` 里：
    //   后者等于"每帧强行把人按在地表"——在超过 slopeLimit 的陡坡上本该滑下去
    //   （自检 D4 明确断言这一点），一按就永远滑不动了。出生时校正一次
    //   才是对的语义：**摆错位置是编辑期问题，不是物理问题**。
    if (m_desc.ground && m_desc.ground->valid()) {
        const float surf = physics::heightAt(*m_desc.ground, tc->position.x,
                                            tc->position.y);
        if (tc->position.z < surf - 1.0e-3f) {
            VK_LOG_INFO("GAME-PLAYER unburied feet=%.2f -> %.2f",
                        static_cast<double>(tc->position.z),
                        static_cast<double>(surf));
            tc->position.z = surf;
            m_state.position.z = surf + halfTotal;
        }
    }

    m_state.onGround = true;
    m_lastCenter = m_state.position;
    m_lastFeet = tc->position;
    m_travelled = 0.0f;

    m_bodyYaw = tc->rotation.z;
    m_yaw = m_bodyYaw;
    m_pitch = 0.30f;   // 仰角：相机略高于注视点（往下看 ~17°）
    cc->bodyYaw = m_bodyYaw;
    cc->started = true;

    const auto* initialCam =
        scene.world().get<ecs::CameraComponent>(m_entity);
    if (initialCam) {
        m_distance = std::max(initialCam->distance, initialCam->minDistance);
        m_minDistance = initialCam->minDistance;
        m_pose.fov = initialCam->fov;
    }

    // ⚠ 姿态要在 attach 里就成型：`CameraSystem` 在**第一帧渲染之前**就
    //   会把 m_pose 写进场景相机，而第一个固定步可能还没跑到。不初始化，
    //   屏幕上会先闪一帧默认的 fov=45 / yaw=0，然后跳到组件的值（实测：
    //   日志里第一行 CAM-POSE 是 fov=45.0，第二行才是 55.0）。
    m_pose.fov = initialCam ? initialCam->fov : m_pose.fov;
    m_pose.yaw = kPi - m_yaw;
    m_pose.pitch = m_pitch;
    m_armSmoothed = m_distance;
    m_camPitchEff = m_pitch;
    m_pose.position =
        m_state.position +
        glm::vec3(-std::cos(m_pitch) * std::cos(m_yaw),
                  -std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch)) *
            m_distance;

    if (world) ensureProxy(world);

    m_attached = true;
    VK_LOG_INFO("PLAYER attach entity='%s' center=(%.2f,%.2f,%.2f) r=%.2f hh=%.2f",
                scene.world().name(m_entity) ? scene.world().name(m_entity)->c_str()
                                             : "?",
                m_state.position.x, m_state.position.y, m_state.position.z,
                cc->radius, cc->halfHeight);
    return true;
}

void PlayerController::detach() {
    m_attached = false;
    m_entity = ecs::Entity{};
    m_proxy = physics::kInvalidBody;
}

void PlayerController::setOrbit(float yaw, float pitch) {
    m_yaw = yaw;
    // 与 update 里**同一口径**（scene/LookLimits.h）。这里曾经写死 ±1.2，
    // 与 update 的 [-0.96, 1.22] 差一截：存档读出来的俯仰会被这个更紧的
    // 夹子截掉，表现为"读档后视角总比存档时平一点"。
    m_pitch = std::clamp(pitch, -scene::kLookUpRad, scene::kLookDownRad);
}

void PlayerController::teleport(scene::Scene& scene, const glm::vec3& feetPos) {
    if (!m_attached) return;
    auto* tc = scene.world().get<ecs::TransformComponent>(m_entity);
    auto* cc = scene.world().get<ecs::CharacterControllerComponent>(m_entity);
    if (!tc || !cc) return;
    tc->position = feetPos;
    m_state.position = feetPos + glm::vec3(0.0f, 0.0f, cc->halfHeight + cc->radius);
    m_state.velocity = glm::vec3(0.0f);
    m_lastCenter = m_state.position;
    m_lastFeet = feetPos;
}

void PlayerController::ensureProxy(physics::PhysicsWorld* world) {
    if (!world || m_proxy != physics::kInvalidBody) return;
    physics::RigidBody b;
    b.type = physics::BodyType::Kinematic;
    b.shape = physics::Shape::capsule(m_desc.radius, m_desc.halfHeight);
    b.position = m_state.position;
    b.entity = m_entity.id;
    b.setFlag(physics::kBodyUseGravity, false);
    b.setFlag(physics::kBodyAllowSleep, false);
    b.linearDamping = 0.0f;
    b.angularDamping = 0.0f;
    m_proxy = world->addBody(std::move(b));
}

void PlayerController::syncProxy(physics::PhysicsWorld* world, float dt) {
    if (!world || m_proxy == physics::kInvalidBody) return;
    physics::RigidBody* b = world->body(m_proxy);
    if (!b) return;
    // 位移差 / dt = "这一帧它走多快" —— 求解器拿这个速度去推动态刚体。
    // pushStrength 只是手感倍率（推箱子轻一点/重一点），不影响角色自身。
    const glm::vec3 v =
        (m_state.position - m_lastCenter) / std::max(dt, 1.0e-5f);
    b->position = m_state.position;
    b->rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    b->linearVelocity = v * std::max(m_desc.pushStrength, 0.0f);
    b->awake = true;
}

void PlayerController::update(scene::Scene& scene, physics::PhysicsWorld* world,
                             float dt) {
    if (!m_attached || dt <= 0.0f) return;

    ecs::World& w = scene.world();
    auto* tc = w.get<ecs::TransformComponent>(m_entity);
    auto* cc = w.get<ecs::CharacterControllerComponent>(m_entity);
    if (!tc || !cc || !cc->enabled) {
        detach();
        return;
    }

    // Inspector 里改了尺寸/速度立刻生效（不用重进 Play）
    m_desc.radius = cc->radius;
    m_desc.halfHeight = cc->halfHeight;
    m_desc.stepHeight = cc->stepHeight;
    m_desc.slopeLimitDeg = cc->slopeLimitDeg;
    m_desc.groundSnap = cc->groundSnap;
    m_desc.groundSnapDistance = cc->groundSnapDistance;
    m_desc.gravityScale = cc->gravityScale;
    m_desc.maxSpeed = physicsMaxSpeed(*cc);
    m_desc.jumpSpeed = cc->jumpSpeed;
    m_desc.acceleration = cc->acceleration;
    m_desc.airControl = cc->airControl;
    m_desc.pushStrength = cc->pushStrength;
    // 地形高度场（第十轮）：物理世界已经把它当"真地面"（刚体走地形接触），
    // 角色这边必须接上同一个指针 —— 否则会出现"木箱停在山坡上、人却穿进
    // 山里走过去"这种两边不一致。
    m_desc.ground = world ? world->heightfield() : nullptr;

    // ---------------- 视角 ----------------
    // ⚠ `m_pitch` 是**仰角**（正 = 相机在注视点上方、往下看），与
    //   scene::Camera 的 pitch 是同一个约定（见 Camera::angleDir：
    //   angleDir.z = sin(p)；CameraSystem::poseFromEntity 也按 asin(-fwd.z) 反解）。
    //   历史 bug：默认值写成 -0.30 → 相机落在注视点**下方**、镜头朝上看，
    //   第三人称变成"从脚底板往上拍"。
    //
    // 鼠标：右移 → 镜头向右转（yaw 减小）；下移 → 往下看（仰角变大）。
    // GLFW 的光标 dy 向下为正，所以是 +=。
    const glm::vec2 md = m_input.lookDelta();
    m_yaw -= md.x * m_sensitivity;
    m_pitch += md.y * m_sensitivity;
    // 右摇杆：持续输入，速度乘 dt。GLFW 手柄的 Y 轴向上为 -1 →
    // 推上去要"抬头"（仰角变小），所以这里也是 +=。
    m_yaw -= m_input.axis("LookYaw") * 2.6f * dt;
    m_pitch += m_input.axis("LookPitch") * 1.8f * dt;
    // ---- 俯仰范围：按"人真的能怎么看"来定（2026-10-24 第二轮修正）----
    //
    // m_pitch **正 = 相机在注视点上方、往下看**，负 = 相机下沉、往上仰。
    //
    // 旧值 [-0.55, 1.15]（-31.5° … +66°）两头都不像人：
    //   · 仰视只给 31°，抬头看树梢/天空几乎不可能（玩家最常见的一个动作）；
    //   · 而真要给到 -60°，4 m 的臂会把相机送到地面以下 2 m —— 那时不是
    //     "角度不对"，是眼睛贴在土里。
    // 所以答案是**分开管**：范围按人的颈椎给（仰 55°、俯 70°，与 UE 第三人称
    // 模板的 ±70 同量级），"会不会钻地"交给下面的地面钳制 —— 它会把相机沿
    // 竖直方向顶到地表之上，再按新高度反解俯仰，镜头依旧看着角色。
    //
    // ⚠ 两个角度取自 `scene/LookLimits.h` 的单一来源：编辑器 Play 那条链
    //   （EditorApp::runPlayers）用的是**反号**的约定，同一份数值抄两遍必然
    //   会漂。这里只做符号适配。
    m_pitch = std::clamp(m_pitch, -scene::kLookUpRad, scene::kLookDownRad);
    m_yaw = wrapPi(m_yaw);

    // ---------------- 移动（相机相对）----------------
    const float mf = m_input.axis("MoveForward");
    const float mr = m_input.axis("MoveRight");
    const glm::vec3 fwd(std::cos(m_yaw), std::sin(m_yaw), 0.0f);
    const glm::vec3 right(std::sin(m_yaw), -std::cos(m_yaw), 0.0f);
    glm::vec3 dir = fwd * mf + right * mr;
    const float dl = glm::length(dir);
    if (dl > 1.0f) dir /= dl;

    const bool sprint = m_input.isHeld("Sprint");
    // 走路速度永远取 cc->maxSpeed；m_desc.maxSpeed 是物理层的**截断上限**
    // （已含冲刺余量），不能拿来当走路速度，否则不按 Shift 也会飞奔。
    const float speed = cc->maxSpeed * (sprint ? cc->sprintMultiplier : 1.0f);
    const glm::vec3 desired = dir * speed;

    const bool jump = !m_jumpBlocked && m_input.wasPressed("Jump");

    // 没有物理世界时也要能跑（纯镜头调试）：给一个空世界，角色只会自由落体。
    static physics::PhysicsWorld kEmptyWorld;
    const physics::PhysicsWorld& wref = world ? *world : kEmptyWorld;

    const physics::CharacterStepResult r = physics::stepCharacter(
        wref, m_desc, m_state, desired, dt, jump, m_proxy);
    m_justJumped = r.jumped;

    // ---------------- 足迹取最高点：脚不许插进坡面（2026-10-23）----------------
    //
    // stepCharacter 的贴地射线只看胶囊**中心**正下方一点。斜坡上行走时，
    // 足迹前缘那半边地表比中心高 —— 物理胶囊有水平扫掠兜着，但**模型**的
    // 脚是刚体，前脚正好插在高于中心的坡面里（用户报的"脚陷进地形"）。
    // 把脚抬到足迹（中心 + ±半径的前后左右）内最高点 = "胶囊搁在地形棱
    // 上"的同一语义。只对 onGround 生效：跳跃/下落的抛物线不能被改。
    if (m_desc.ground && m_desc.ground->valid() && m_state.onGround) {
        const float fr = m_desc.radius;
        const float px = m_state.position.x;
        const float py = m_state.position.y;
        float surf = physics::heightAt(*m_desc.ground, px, py);
        surf = std::max(surf, physics::heightAt(*m_desc.ground, px + fr, py));
        surf = std::max(surf, physics::heightAt(*m_desc.ground, px - fr, py));
        surf = std::max(surf, physics::heightAt(*m_desc.ground, px, py + fr));
        surf = std::max(surf, physics::heightAt(*m_desc.ground, px, py - fr));
        const float halfTotalR = cc->halfHeight + cc->radius;
        if (m_state.position.z - halfTotalR < surf - 1.0e-4f)
            m_state.position.z = surf + halfTotalR;
    }

    // ---------------- 兜底：脚底绝不允许留在地形下面 ----------------
    //
    // 用户原话："角色在踩在地形的地面时，应该是脚部踩在上面的，不允许脚底
    // 踩到地形地面的下方。"
    //
    // `stepCharacter` 里的扫掠窗口（见 CharacterController.cpp 里的 sweep）
    // 已经从根上避免"一帧跨过地面"，这里再放一道**保险**：万一还有没覆盖到
    // 的情形（陡坎、地形刚加载完、脚本瞬移），也绝不允许角色留在土里。
    // 因为"在地形内部"是**不可恢复**状态 —— 竖直射线从胶囊中心往下打不到
    // 任何东西（地面在头顶），控制器再也不可能自己贴回去。
    //
    // 阈值 0.25 m：正常贴地误差是毫米级；超过 25 cm 只可能是真的陷进去了。
    // 触发时打 WARN，日志里有据可查，不做静默修补。
    if (m_desc.ground && m_desc.ground->valid() && !m_state.onGround) {
        const float halfTotalR = cc->halfHeight + cc->radius;
        const float surf = physics::heightAt(*m_desc.ground, m_state.position.x,
                                            m_state.position.y);
        const float feetZ = m_state.position.z - halfTotalR;
        if (feetZ < surf - 0.25f) {
            VK_LOG_WARN("GAME-PLAYER-RESCUE feet=%.3f surf=%.3f depth=%.3f",
                        static_cast<double>(feetZ),
                        static_cast<double>(surf),
                        static_cast<double>(surf - feetZ));
            m_state.position.z = surf + halfTotalR;
            m_state.velocity.z = 0.0f;
            m_state.onGround = true;
        }
    }

    // ---------------- 写回实体 ----------------
    const float halfTotal = cc->halfHeight + cc->radius;
    const glm::vec3 feet = m_state.position - glm::vec3(0.0f, 0.0f, halfTotal);
    const glm::vec3 delta = feet - m_lastFeet;
    // 相对位移写回局部位移：有父节点时也能对上（父节点只平移的常见情况）。
    tc->position += delta;
    m_lastFeet = feet;

    m_travelled += glm::length(glm::vec2(r.position.x, r.position.y) -
                               glm::vec2(m_lastCenter.x, m_lastCenter.y));
    m_lastCenter = m_state.position;

    // 平滑转身：直接瞬转看着像机器人
    if (dl > 0.05f) {
        const float targetYaw = std::atan2(dir.y, dir.x);
        const float t = 1.0f - std::exp(-std::max(cc->turnSpeed, 0.0f) * dt);
        m_bodyYaw = lerpAngle(m_bodyYaw, targetYaw, t);
        tc->rotation.z = m_bodyYaw;
    }

    cc->position = m_state.position;
    cc->velocity = m_state.velocity;
    cc->onGround = m_state.onGround;
    cc->groundNormalZ = m_state.groundNormalZ;
    cc->bodyYaw = m_bodyYaw;

    syncProxy(world, dt);

    // ---------------- 相机 ----------------
    // 枢轴 = 胶囊中心 + CameraComponent::targetOffset（默认落在头肩之间）。
    // 没有相机组件时退回老常数（与 G09 之前逐位一致）。
    const auto* cam = w.get<ecs::CameraComponent>(m_entity);
    const glm::vec3 pivot =
        cam ? m_state.position + cam->targetOffset
            : m_state.position +
                  glm::vec3(0.0f, 0.0f, cc->halfHeight + cc->radius * 0.6f);

    const glm::vec3 back(-std::cos(m_pitch) * std::cos(m_yaw),
                         -std::cos(m_pitch) * std::sin(m_yaw),
                         std::sin(m_pitch));

    // collideWithWorld = false → 不做弹簧臂检测（过场镜头贴墙用）。
    scene::SpringArmResult arm;
    const bool collides = (!cam || cam->collideWithWorld);
    const scene::SpringArmCast cast =
        collides ? makeWorldCast(world, m_desc.ground, m_proxy)
                 : scene::SpringArmCast();
    if (collides) {
        arm = scene::resolveSpringArm(cast, pivot, back, m_distance,
                                      m_minDistance, m_cameraRadius);
    } else {
        arm.position = pivot + back * m_distance;
        arm.distance = m_distance;
        arm.blocked = false;
    }

    // ---- 臂长平滑（UE SpringArm 的"缩得快、伸得慢"）----
    //
    // 缩短必须**立刻**生效（慢一点就意味着这一帧镜头在墙里）；伸长限速，
    // 否则一拐出墙角镜头会"啪"地弹回全长。限速取 6 m/s：4 m 的臂从
    // 贴脸恢复到全长约 0.7 s，够柔和又不显得黏。
    if (arm.distance < m_armSmoothed) {
        m_armSmoothed = arm.distance;
    } else {
        constexpr float kExtendSpeed = 6.0f;
        m_armSmoothed = std::min(arm.distance,
                                 m_armSmoothed + kExtendSpeed * dt);
        arm.position = pivot + back * m_armSmoothed;
        arm.distance = m_armSmoothed;
    }

    // ---- 相机不许钻进地面（第二轮修正，见 update 里 pitch 的注释）----
    //
    // 仰视到极限时臂会伸到枢轴下方，长臂 + 平地 = 相机整个沉到地表以下，
    // 视野里全是地形背面（"往下俯视/往上仰视角度不正常"的主因之一）。
    // 这里把相机沿**竖直方向**抬到 clearance 之上，再按新的高度反解
    // 俯仰角 —— 这样相机的**朝向**仍然指着枢轴，不会突然看天。
    //
    // 反解逻辑放在 `scene::clampCameraAboveGround`（与编辑器 Play 共用一份）：
    // 两边各写一遍就等于"符号抄反"等着发生 —— 第一版这里写成
    // `pivot.z - cam.z`，钳制时会把相机镜像到枢轴下面（正好是地底）。
    m_camPitchEff = m_pitch;
    m_posePivot = pivot;
    if (collides && arm.distance > 1.0e-3f) {
        constexpr float kClearance = 0.25f;
        // ① 地形高度场（有 Terrain 实体时最准，双线性插值）
        float minZ = -1.0e30f;
        bool haveFloor = false;
        if (m_desc.ground && m_desc.ground->valid()) {
            const float surf = physics::heightAt(*m_desc.ground,
                                                 arm.position.x, arm.position.y);
            minZ = surf + kClearance;
            haveFloor = true;
        }
        // ② **向下射线**兜住"没有地形"的场地：物理平面 / 静态网格地面。
        //    旧代码只看 heightfield，而多数场景（sandbox / 关卡模板）的地面
        //    是 physics.groundPlane 或一块静态网格 —— heightfield 是空的，
        //    于是完全不钳制，仰视到极限相机照样沉到 z<0（用户看到的"视野
        //    在地里"）。射线从相机上方 1 m 往下打，取较**高**的那个面。
        if (cast) {
            constexpr float kProbeUp = 1.0f;
            constexpr float kProbeDown = 6.0f;
            const glm::vec3 o = arm.position + glm::vec3(0.0f, 0.0f, kProbeUp);
            const float t = cast(o, glm::vec3(0.0f, 0.0f, -1.0f),
                                 kProbeUp + kProbeDown);
            if (t < kProbeUp + kProbeDown) {
                minZ = std::max(minZ, o.z - t + kClearance);
                haveFloor = true;
            }
        }
        if (haveFloor && scene::clampCameraAboveGround(pivot, minZ,
                                                       &arm.position,
                                                       &arm.distance,
                                                       &m_camPitchEff)) {
            arm.blocked = true;
        }
    }

    m_armDistance = arm.distance;
    m_armBlocked = arm.blocked;
    m_pose.position = arm.position;
    // 由 back 反解 Camera 的 (yaw, pitch)：见 Camera.h 里 angleDir 的公式
    m_pose.yaw = kPi - m_yaw;
    m_pose.pitch = m_camPitchEff;
    if (cam) m_pose.fov = cam->fov;

    if (m_trace) {
        VK_LOG_INFO("PLAYER pos=(%.3f,%.3f,%.3f) vel=(%.2f,%.2f,%.2f) ground=%d "
                    "arm=%.2f blocked=%d yaw=%.2f",
                    m_state.position.x, m_state.position.y, m_state.position.z,
                    m_state.velocity.x, m_state.velocity.y, m_state.velocity.z,
                    m_state.onGround ? 1 : 0, m_armDistance, m_armBlocked ? 1 : 0,
                    m_bodyYaw);
    }
}

// ============================================================ 自检

namespace {

int g_plFail = 0;
int g_plPass = 0;

void pcheck(const char* what, bool ok, const std::string& detail = "") {
    if (ok) {
        ++g_plPass;
        std::printf("PLAYER-TEST pass %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    } else {
        ++g_plFail;
        std::printf("PLAYER-TEST FAIL %s%s%s\n", what,
                    detail.empty() ? "" : " — ", detail.c_str());
    }
}

physics::BodyId addStaticBox(physics::PhysicsWorld& w, const glm::vec3& half,
                             const glm::vec3& pos) {
    physics::RigidBody b;
    b.type = physics::BodyType::Static;
    b.shape = physics::Shape::box(half);
    b.position = pos;
    b.setFlag(physics::kBodyUseGravity, false);
    b.setFlag(physics::kBodyAllowSleep, false);
    return w.addBody(std::move(b));
}

} // namespace

int runPlayerSelfTest() {
    g_plPass = 0;
    g_plFail = 0;
    std::printf("PLAYER-TEST begin (no window)\n");

    // ---- A. 走 10 m 的用时与 maxSpeed 相符（±5%）----
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Player");
        ecs::CharacterControllerComponent cc;
        s.world().add<ecs::CharacterControllerComponent>(e, cc);

        physics::PhysicsWorld w;
        w.settings().allowSleep = false;
        physics::RigidBody ground;
        ground.type = physics::BodyType::Static;
        ground.shape = physics::Shape::plane(glm::vec3(0, 0, 1), 0.0f);
        ground.flags = 0;
        w.addBody(std::move(ground));

        core::InputMap in = core::InputMap::makeDefault();
        PlayerController pc(in);
        pcheck("A1 找到并接管角色实体", pc.attach(s, &w));

        core::RawInput raw;
        raw.keys[GLFW_KEY_W] = true;
        const float dt = 1.0f / 60.0f;

        // 先跑 30 帧加速到稳态
        for (int i = 0; i < 30; ++i) {
            in.pollRaw(raw);
            pc.update(s, &w, dt);
        }
        const float d0 = pc.travelledDistance();
        const glm::vec3 p0 = pc.characterCenter();
        const int kFrames = 180;   // 3 秒
        for (int i = 0; i < kFrames; ++i) {
            in.pollRaw(raw);
            pc.update(s, &w, dt);
        }
        const float travelled = pc.travelledDistance() - d0;
        const float secs = kFrames * dt;
        const float measured = travelled / secs;
        const float expect = cc.maxSpeed;
        const float err = std::fabs(measured - expect) / expect;
        pcheck("A2 稳态速度与 maxSpeed 相符（±5%）", err <= 0.05f,
               "measured=" + std::to_string(measured) + " expect=" +
                   std::to_string(expect) + " err=" + std::to_string(err * 100.0f) +
                   "%");
        pcheck("A3 走的是 +X（相机 yaw=0 时 W = +X）",
               pc.characterCenter().x > p0.x + 1.0f,
               "dx=" + std::to_string(pc.characterCenter().x - p0.x));

        // ---- A2b 冲刺：按住 Sprint 必须真的更快 ----
        // 这一条是补回来的：物理层的 desc.maxSpeed 会截断期望速度，若它按走路
        // 速度填，Sprint 就完全没反应（前文 physicsMaxSpeed 的注释）。
        // 2026-10-22：出厂绑定从 LShift 换成**鼠标右键**，这里跟着换 —— 否则
        // 自检会替一条已经不存在的手感背书（测的是新表里没有的键 = 测了个空）。
        core::RawInput rawSprint;
        rawSprint.keys[GLFW_KEY_W] = true;
        rawSprint.mouse[GLFW_MOUSE_BUTTON_RIGHT] = true;
        for (int i = 0; i < 30; ++i) {
            in.pollRaw(rawSprint);
            pc.update(s, &w, dt);
        }
        const float ds0 = pc.travelledDistance();
        for (int i = 0; i < kFrames; ++i) {
            in.pollRaw(rawSprint);
            pc.update(s, &w, dt);
        }
        const float sprintMeasured = (pc.travelledDistance() - ds0) / secs;
        const float sprintExpect = cc.maxSpeed * cc.sprintMultiplier;
        const float sprintErr =
            std::fabs(sprintMeasured - sprintExpect) / sprintExpect;
        pcheck("A2b 冲刺速度 = maxSpeed × sprintMultiplier（±5%）",
               sprintErr <= 0.05f,
               "measured=" + std::to_string(sprintMeasured) + " expect=" +
                   std::to_string(sprintExpect) +
                   " err=" + std::to_string(sprintErr * 100.0f) + "%");
    }

    // ---- B. 相机遇墙缩臂（不穿墙）----
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Player");
        s.world().add<ecs::CharacterControllerComponent>(e);

        physics::PhysicsWorld w;
        w.settings().allowSleep = false;
        physics::RigidBody ground;
        ground.type = physics::BodyType::Static;
        ground.shape = physics::Shape::plane(glm::vec3(0, 0, 1), 0.0f);
        ground.flags = 0;
        w.addBody(std::move(ground));
        // 玩家在 (0,0,0)，镜头在背后 = -X；墙放在 x = -1.5
        addStaticBox(w, glm::vec3(0.1f, 5.0f, 3.0f), glm::vec3(-1.5f, 0.0f, 1.5f));

        core::InputMap in = core::InputMap::makeDefault();
        PlayerController pc(in);
        pc.setOrbit(0.0f, -0.05f);   // 视线水平，臂正对墙
        pc.attach(s, &w);

        core::RawInput idle;
        for (int i = 0; i < 10; ++i) {
            in.pollRaw(idle);
            pc.update(s, &w, 1.0f / 60.0f);
        }
        pcheck("B1 背后有墙时臂长被缩短", pc.springArmBlocked(),
               "arm=" + std::to_string(pc.springArmDistance()));
        pcheck("B2 相机停在墙前（臂长 < 1.5 m）", pc.springArmDistance() < 1.5f,
               "arm=" + std::to_string(pc.springArmDistance()));
        const glm::vec3 cam = pc.cameraPose().position;
        pcheck("B3 相机没有穿到墙后面", cam.x > -1.5f,
               "cam.x=" + std::to_string(cam.x));
    }

    // ---- C. 静止 300 帧相机不抖（极差 < 0.5 mm）----
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Player");
        s.world().add<ecs::CharacterControllerComponent>(e);

        physics::PhysicsWorld w;
        w.settings().allowSleep = false;
        physics::RigidBody ground;
        ground.type = physics::BodyType::Static;
        ground.shape = physics::Shape::plane(glm::vec3(0, 0, 1), 0.0f);
        ground.flags = 0;
        w.addBody(std::move(ground));

        core::InputMap in = core::InputMap::makeDefault();
        PlayerController pc(in);
        pc.attach(s, &w);

        core::RawInput idle;
        // 先落稳
        for (int i = 0; i < 60; ++i) {
            in.pollRaw(idle);
            pc.update(s, &w, 1.0f / 60.0f);
        }
        glm::vec3 mn = pc.cameraPose().position;
        glm::vec3 mx = mn;
        for (int i = 0; i < 300; ++i) {
            in.pollRaw(idle);
            pc.update(s, &w, 1.0f / 60.0f);
            const glm::vec3 p = pc.cameraPose().position;
            mn = glm::min(mn, p);
            mx = glm::max(mx, p);
        }
        const float range = glm::length(mx - mn);
        pcheck("C1 静止 300 帧相机位置极差 < 0.5 mm", range < 5.0e-4f,
               "range=" + std::to_string(range * 1000.0f) + " mm");
    }

    // ---- D. 跳跃：贴地才起跳 ----
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Player");
        s.world().add<ecs::CharacterControllerComponent>(e);

        physics::PhysicsWorld w;
        w.settings().allowSleep = false;
        physics::RigidBody ground;
        ground.type = physics::BodyType::Static;
        ground.shape = physics::Shape::plane(glm::vec3(0, 0, 1), 0.0f);
        ground.flags = 0;
        w.addBody(std::move(ground));

        core::InputMap in = core::InputMap::makeDefault();
        PlayerController pc(in);
        pc.attach(s, &w);

        core::RawInput idle;
        for (int i = 0; i < 10; ++i) {
            in.pollRaw(idle);
            pc.update(s, &w, 1.0f / 60.0f);
        }
        const float z0 = pc.characterCenter().z;

        core::RawInput jump;
        jump.keys[GLFW_KEY_SPACE] = true;
        in.pollRaw(jump);
        pc.update(s, &w, 1.0f / 60.0f);
        float peak = pc.characterCenter().z;
        // 5 m/s 起跳的滞空约 1 秒（60 帧），给 100 帧足够落回来
        for (int i = 0; i < 100; ++i) {
            in.pollRaw(idle);
            pc.update(s, &w, 1.0f / 60.0f);
            peak = std::max(peak, pc.characterCenter().z);
        }
        pcheck("D1 按跳之后角色升空（峰值 > 起点 + 0.5 m）", peak > z0 + 0.5f,
               "peak=" + std::to_string(peak - z0));
        pcheck("D2 落地后回到贴地", pc.onGround(),
               "z=" + std::to_string(pc.characterCenter().z));
    }

    // ---- E. 俯仰范围：按"人怎么看"钳，而不是按"能转多少" ----
    //
    // 用户原话："角色相机视野在往上仰视和往下俯视的角度有些不正常，要符合
    // 人类生活中的现实场景。"
    //
    // 这里直接喂一个**远超极限**的轨道角再 update 一帧 —— 钳制发生在 update
    // 里（鼠标输入之后），所以只要最终 pitch 落在 kLookUpRad / kLookDownRad
    // 上，就说明"无论鼠标推多狠都翻不过去"。老值 [-0.55, 1.15] 会在这里挂。
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Player");
        s.world().add<ecs::CharacterControllerComponent>(e);

        core::InputMap in = core::InputMap::makeDefault();
        PlayerController pc(in);
        pc.attach(s, nullptr);
        core::RawInput idle;

        // 抬头方向：m_pitch 负 = 相机下沉、往上看
        pc.setOrbit(0.0f, -10.0f);
        in.pollRaw(idle);
        pc.update(s, nullptr, 1.0f / 60.0f);
        const float up = pc.orbitPitch();
        pcheck("E1 仰视上限 = scene::kLookUpRad（55°）",
               std::fabs(up + scene::kLookUpRad) < 1.0e-4f,
               "pitch=" + std::to_string(up) + " expect=" +
                   std::to_string(-scene::kLookUpRad));

        // 低头方向：m_pitch 正 = 相机在上方、往下看
        pc.setOrbit(0.0f, 10.0f);
        in.pollRaw(idle);
        pc.update(s, nullptr, 1.0f / 60.0f);
        const float down = pc.orbitPitch();
        pcheck("E2 俯视上限 = scene::kLookDownRad（70°）",
               std::fabs(down - scene::kLookDownRad) < 1.0e-4f,
               "pitch=" + std::to_string(down) + " expect=" +
                   std::to_string(scene::kLookDownRad));

        // 上下**不对称**是刻意的（仰 55 < 俯 70）。写成对称的话，用户说的
        // "低头看不到脚下"会回来。
        pcheck("E3 仰视比俯视窄（55° < 70°）", scene::kLookUpRad < scene::kLookDownRad,
               "up=" + std::to_string(scene::kLookUpRad) +
                   " down=" + std::to_string(scene::kLookDownRad));
    }

    // ---- F. 地面钳制：仰视到极限也不许把相机沉到地表以下 ----
    //
    // 只靠 E 组的范围不够：4 m 的臂在 55° 仰角下会把相机送到枢轴下方
    // 4·sin(55°) = 3.3 m —— 站在平地上就是地底 2 m。所以钳制必须真的发生，
    // 而且**反解出的俯仰得让镜头继续看着角色**（不能变成看天）。
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Player");
        s.world().add<ecs::CharacterControllerComponent>(e);

        physics::PhysicsWorld w;
        w.settings().allowSleep = false;
        physics::RigidBody ground;
        ground.type = physics::BodyType::Static;
        ground.shape = physics::Shape::plane(glm::vec3(0, 0, 1), 0.0f);
        ground.flags = 0;
        w.addBody(std::move(ground));

        core::InputMap in = core::InputMap::makeDefault();
        PlayerController pc(in);
        pc.attach(s, &w);
        pc.setOrbit(0.0f, -scene::kLookUpRad);   // 仰到极限

        core::RawInput idle;
        for (int i = 0; i < 30; ++i) {
            in.pollRaw(idle);
            pc.update(s, &w, 1.0f / 60.0f);
        }
        const glm::vec3 cam = pc.cameraPose().position;
        pcheck("F1 仰视到极限时相机仍在地表之上", cam.z > 0.20f,
               "cam.z=" + std::to_string(cam.z));
        // 钳制之后相机被压到枢轴**下方**（仰视时它本来就该在下面），
        // 所以反解出的 pose.pitch 必须是**负**的 —— 正了就说明符号抄反，
        // 相机会被镜像到枢轴另一侧（也就是它刚逃出来的地底），下一帧再
        // 钳制、再镜像 → 抖动。这里直接和"从几何量出来的角度"比。
        const glm::vec3 pv = pc.cameraPivot();
        const float dist = glm::length(cam - pv);
        const float expectPitch =
            std::asin(std::clamp((cam.z - pv.z) / std::max(dist, 1.0e-5f),
                                 -1.0f, 1.0f));
        pcheck("F2 反解的 pitch 与 |相机-枢轴| 方向一致（符号没抄反）",
               std::fabs(pc.cameraPose().pitch - expectPitch) < 1.0e-3f &&
                   (expectPitch < 0.0f),
               "pose.pitch=" + std::to_string(pc.cameraPose().pitch) +
                   " expect=" + std::to_string(expectPitch));
        // 相机到枢轴的距离必须和位置一致 —— 这就是"反解没算错"的判据。
        const float expectDist =
            glm::length(cam - pc.cameraPivot());
        const float dErr = std::fabs(expectDist - pc.springArmDistance());
        pcheck("F3 臂长 == |相机 - 枢轴|（地面钳制反解自洽）",
               dErr < 1.0e-3f,
               "arm=" + std::to_string(pc.springArmDistance()) + " actual=" +
                   std::to_string(expectDist));
    }

    // ---- G. 墙角：中心射线漏掉、环射线发现（单射线就穿过去了）----
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Player");
        s.world().add<ecs::CharacterControllerComponent>(e);

        physics::PhysicsWorld w;
        w.settings().allowSleep = false;
        physics::RigidBody ground;
        ground.type = physics::BodyType::Static;
        ground.shape = physics::Shape::plane(glm::vec3(0, 0, 1), 0.0f);
        ground.flags = 0;
        w.addBody(std::move(ground));
        // 一根柱子：细长的静态盒，刻意**不让它挡在相机正后方**，
        // 而是偏到 y = 0.30 的侧后方（相机半径 0.25 + 一点余量）。
        addStaticBox(w, glm::vec3(0.12f, 0.12f, 3.0f), glm::vec3(-1.6f, 0.30f, 1.5f));

        core::InputMap in = core::InputMap::makeDefault();
        PlayerController pc(in);
        pc.setOrbit(0.0f, 0.0f);
        pc.attach(s, &w);

        core::RawInput idle;
        for (int i = 0; i < 10; ++i) {
            in.pollRaw(idle);
            pc.update(s, &w, 1.0f / 60.0f);
        }
        // 注：这一条测的是"侧后方有细柱时，相机不该被它扎穿"。
        // 柱子中心 y = 0.30、半宽 0.12 → 近侧 y = 0.18；相机球半径 0.25
        // 从 y = 0 出发会擦到它，所以臂长应当被压到 1.6 以内。
        pcheck("G1 侧后方细柱也会缩短臂长（球扫掠，不是单射线）",
               pc.springArmDistance() < 1.9f,
               "arm=" + std::to_string(pc.springArmDistance()));
    }

    // ---- H. 臂长平滑：缩短立刻、伸长限速 ----
    {
        scene::Scene s;
        ecs::Entity e = s.createObject("Player");
        s.world().add<ecs::CharacterControllerComponent>(e);

        physics::PhysicsWorld w;
        w.settings().allowSleep = false;
        physics::RigidBody ground;
        ground.type = physics::BodyType::Static;
        ground.shape = physics::Shape::plane(glm::vec3(0, 0, 1), 0.0f);
        ground.flags = 0;
        w.addBody(std::move(ground));

        core::InputMap in = core::InputMap::makeDefault();
        PlayerController pc(in);
        pc.attach(s, &w);
        core::RawInput idle;
        for (int i = 0; i < 120; ++i) {   // 先伸到全长
            in.pollRaw(idle);
            pc.update(s, &w, 1.0f / 60.0f);
        }
        const float full = pc.springArmDistance();

        // 突然在背后放一堵近墙
        addStaticBox(w, glm::vec3(0.1f, 5.0f, 3.0f), glm::vec3(-1.0f, 0.0f, 1.5f));
        in.pollRaw(idle);
        pc.update(s, &w, 1.0f / 60.0f);
        const float afterOne = pc.springArmDistance();
        pcheck("H1 撞墙那一帧臂长立刻缩短（不隔帧）",
               afterOne < full * 0.5f,
               "full=" + std::to_string(full) + " after1=" +
                   std::to_string(afterOne));
    }

    std::printf("PLAYER-TOTAL pass=%d fail=%d\n", g_plPass, g_plFail);
    return g_plFail;
}
} // namespace runtime
