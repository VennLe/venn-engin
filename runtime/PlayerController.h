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
    // 相机枢轴（= 胶囊中心 + CameraComponent::targetOffset）本帧的值。
    // 自检用它断言"臂长 == |相机 - 枢轴|"（地面钳制反解自洽），
    // 游戏逻辑本身不需要这个值。
    glm::vec3 cameraPivot() const { return m_posePivot; }
    bool springArmBlocked() const { return m_armBlocked; }
    bool onGround() const { return m_state.onGround; }
    glm::vec3 characterCenter() const { return m_state.position; }
    // 角色速度（m/s，世界空间，含竖直分量）。给动画状态机用 —— 它要的是
    // "这一帧真的在以多快移动"，而不是输入轴的大小（撞墙时输入满档但
    // 速度为 0，腿不该还在跑）。
    glm::vec3 velocity() const { return m_state.velocity; }

    // 本帧是否真的起跳了（贴地 + 按下跳跃 + 物理接受）。
    //
    // 为什么要有它（2026-10-25，状态机接线）：
    //   `physics::stepCharacter` 的返回值里一直有 `jumped`，但**只有自检在读**
    //   —— 运行时没有任何消费者。而动画状态机的 `Jump` trigger 需要的正是
    //   "起跳这一刻"，用 `!onGround` 代替是不行的：走下台阶也是 onGround=false,
    //   那会把"落地/踏空"也当成起跳。
    bool justJumped() const { return m_justJumped; }

    void setTrace(bool on) { m_trace = on; }

    // 对话/过场期间把**跳跃**挡掉。
    //
    // 为什么需要它（C2，2026-10-26）：字幕的"推进"用 Space（= `Jump`），
    // 而 `InputMap` 的 `wasPressed` 是**边沿查询、不是消费** —— 同一帧
    // 对话推进完，`PlayerController::update` 照样能读到那一次按下，
    // 于是"按空格翻页"顺带让角色跳了一下。玩家不会报这个 bug，只会觉得
    // 手感怪。
    //
    // ⚠ 只挡跳跃，不挡移动：`MoveForward` 的轴值是连续的，按着 W 说话
    //   仍然应该能走（对话里走动是常见需求）；而跳跃是一次性事件，
    //   挡掉它不会有"按着不放却不动"的怪异感。
    void setJumpBlocked(bool on) { m_jumpBlocked = on; }

private:
    void ensureProxy(physics::PhysicsWorld* world);
    void syncProxy(physics::PhysicsWorld* world, float dt);

    core::InputMap& m_input;

    bool m_attached = false;
    ecs::Entity m_entity{};
    physics::CharacterDesc m_desc;
    physics::CharacterState m_state;
    physics::BodyId m_proxy = physics::kInvalidBody;
    bool m_justJumped = false;   // 见 justJumped()
    bool m_jumpBlocked = false;  // 见 setJumpBlocked()

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
    // 平滑后的臂长：缩短**立刻**生效（慢一帧就是镜头在墙里），伸长限速
    // （拐出墙角不会"啪"地弹回全长）。见 update 里的相机段。
    float m_armSmoothed = 0.0f;
    // 实际交给 Camera 的俯仰角。平时与 m_pitch 逐位相同；只有"相机被地面
    // 顶高"时才会不同 —— 那时仍然让镜头**看着枢轴**，而不是突然抬头看天。
    // （反解逻辑在 scene::clampCameraAboveGround，与编辑器 Play 共用一份。）
    float m_camPitchEff = 0.0f;
    // 本帧的枢轴（见 cameraPivot()）
    glm::vec3 m_posePivot{0.0f};

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
