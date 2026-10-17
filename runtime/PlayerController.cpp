#include "PlayerController.h"

#include "SpringArm.h"

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
    m_desc.maxSpeed = cc->maxSpeed;
    m_desc.jumpSpeed = cc->jumpSpeed;
    m_desc.acceleration = cc->acceleration;
    m_desc.airControl = cc->airControl;
    m_desc.pushStrength = cc->pushStrength;

    // 实体原点是**脚底**（与编辑器里摆人的习惯一致），物理状态要的是
    // 胶囊中心 —— 差一个 (halfHeight + radius)。
    const float halfTotal = cc->halfHeight + cc->radius;
    m_state = physics::CharacterState{};
    m_state.position = tc->position + glm::vec3(0.0f, 0.0f, halfTotal);
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
    m_pitch = std::clamp(pitch, -1.2f, 1.2f);
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
    m_desc.maxSpeed = cc->maxSpeed;
    m_desc.jumpSpeed = cc->jumpSpeed;
    m_desc.acceleration = cc->acceleration;
    m_desc.airControl = cc->airControl;
    m_desc.pushStrength = cc->pushStrength;

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
    m_pitch = std::clamp(m_pitch, -0.55f, 1.15f);
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
    const float speed = m_desc.maxSpeed * (sprint ? cc->sprintMultiplier : 1.0f);
    const glm::vec3 desired = dir * speed;

    const bool jump = m_input.wasPressed("Jump");

    // 没有物理世界时也要能跑（纯镜头调试）：给一个空世界，角色只会自由落体。
    static physics::PhysicsWorld kEmptyWorld;
    const physics::PhysicsWorld& wref = world ? *world : kEmptyWorld;

    const physics::CharacterStepResult r = physics::stepCharacter(
        wref, m_desc, m_state, desired, dt, jump, m_proxy);

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
    SpringArmResult arm;
    if (!cam || cam->collideWithWorld) {
        arm = resolveSpringArm(world, pivot, back, m_distance, m_minDistance,
                               m_cameraRadius, m_proxy);
    } else {
        arm.position = pivot + back * m_distance;
        arm.distance = m_distance;
        arm.blocked = false;
    }
    m_armDistance = arm.distance;
    m_armBlocked = arm.blocked;

    m_pose.position = arm.position;
    // 由 back 反解 Camera 的 (yaw, pitch)：见 Camera.h 里 angleDir 的公式
    m_pose.yaw = kPi - m_yaw;
    m_pose.pitch = m_pitch;
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

    std::printf("PLAYER-TOTAL pass=%d fail=%d\n", g_plPass, g_plFail);
    return g_plFail;
}

} // namespace runtime
