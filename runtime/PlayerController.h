#pragma once
// ============================================================
// runtime/PlayerController —— 把输入 / 角色 / 相机串成一个能操控的人（G10）
//
// 它是 M2 的"总装"：G06 给出动作轴，G07 给出位移，G09 给出镜头，
// 这里负责把它们接起来：
//
//   Move 轴 × 相机朝向 → 期望速度 → stepCharacter → 实体 Transform
//   Look 轴（鼠标 / 右摇杆） → 轨道角 → SpringArm → CameraPose
//
// ------------------------------------------------------------
// 为什么相机朝向要参与移动
// ------------------------------------------------------------
// 第三人称里"按 W 往前走"的"前"是**镜头的前**，不是角色的前。用角色
// 自身的朝向会让"转身"变成"改变行进方向"，手感立刻变成坦克式。所以每次
// 都用镜头 yaw 把输入投影到世界，再让角色**平滑转身**去追这个方向
// （直接瞬转看着像机器人，见 turnSpeed）。
//
// ------------------------------------------------------------
// 运动学代理
// ------------------------------------------------------------
// 角色在物理世界里有一个 Kinematic 胶囊代理：位置由本模块写，速度取
// （这一帧位置 - 上一帧位置）/ dt。于是它能**推动**动态刚体（推箱子），
// 但求解器推不动它（Kinematic 逆质量为 0）—— 这正是角色该有的分工。
// ============================================================

#include "core/InputMap.h"
#include "ecs/Entity.h"
#include "physics/CharacterController.h"
#include "physics/RigidBody.h"
#include "scene/CameraSystem.h"

namespace physics {
class PhysicsWorld;
}
namespace scene {
class Scene;
}

namespace runtime {

class PlayerController {
public:
    explicit PlayerController(core::InputMap& input);

    // 场景里找第一个启用的 CharacterControllerComponent 并接管它。
    // `world` 可以为空（此时不建代理 —— 用于无物理的纯镜头测试）。
    bool attach(scene::Scene& scene, physics::PhysicsWorld* world);
    void detach();
    bool attached() const { return m_attached; }
    ecs::Entity entity() const { return m_entity; }

    // 每个**固定步**调用一次（与物理同频）。
    void update(scene::Scene& scene, physics::PhysicsWorld* world, float dt);

    // 相机意图（交给 CameraSystem::setDesiredPose）
    const scene::CameraPose& cameraPose() const { return m_pose; }
    // 本帧的轨道角（过场 / 调试用）
    float orbitYaw() const { return m_yaw; }
    float orbitPitch() const { return m_pitch; }
    void setOrbit(float yaw, float pitch);
    // 直接摆放（存档读档 / 传送）。位置是**脚底**。
    void teleport(scene::Scene& scene, const glm::vec3& feetPos);

    // 统计（验证脚本读它）
    float travelledDistance() const { return m_travelled; }
    float springArmDistance() const { return m_armDistance; }
    bool springArmBlocked() const { return m_armBlocked; }
    bool onGround() const { return m_state.onGround; }
    glm::vec3 characterCenter() const { return m_state.position; }
    // 角色速度（m/s，世界空间，含竖直分量）。给动画状态机用 —— 它要的是
    // "这一帧真的在以多快移动"，而不是输入轴的大小（撞墙时输入满档但
    // 速度为 0，腿不该还在跑）。
    glm::vec3 velocity() const { return m_state.velocity; }

    void setTrace(bool on) { m_trace = on; }

private:
    void ensureProxy(physics::PhysicsWorld* world);
    void syncProxy(physics::PhysicsWorld* world, float dt);

    core::InputMap& m_input;

    bool m_attached = false;
    ecs::Entity m_entity{};
    physics::CharacterDesc m_desc;
    physics::CharacterState m_state;
    physics::BodyId m_proxy = physics::kInvalidBody;

    // 轨道相机
    float m_yaw = 0.0f;
    float m_pitch = 0.30f;   // 仰角（正 = 相机在上方，往下看）
    float m_distance = 4.0f;
    float m_minDistance = 0.6f;
    float m_cameraRadius = 0.25f;
    float m_sensitivity = 0.0025f;
    float m_bodyYaw = 0.0f;

    scene::CameraPose m_pose;
    float m_armDistance = 0.0f;
    bool m_armBlocked = false;

    float m_travelled = 0.0f;
    glm::vec3 m_lastCenter{0.0f};
    glm::vec3 m_lastFeet{0.0f};
    bool m_trace = false;
};

// ------------------------------------------------------------ 自检
// 无窗口：地面 + 墙 + 角色，注入输入，断言"走够距离 / 相机不穿墙 /
// 静止 300 帧相机零抖动"。
// 由 `Game.exe --selftest player` 调用。
int runPlayerSelfTest();

} // namespace runtime
