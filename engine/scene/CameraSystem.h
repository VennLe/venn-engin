#pragma once
// ============================================================
// scene/CameraSystem —— 多相机调度 + 混合栈 + 震动（G09）
//
// 在 G09 之前，"用哪台相机"是编辑器视口的一个下标（`Renderer::setViewCameraIndex`），
// 运行时的画面则由 `Scene::update` 让**每一台** `scene::Camera` 各自响应输入。
// 两件事在游戏里都不成立：
//   · 玩家相机、过场相机、载具相机要按**优先级**自动切换
//   · 视觉冲击（落地 / 爆炸 / 受击）要能**加**在相机上，而不是改相机本身
//
// ------------------------------------------------------------
// 三个名词
// ------------------------------------------------------------
//   CameraPose      相机的一组"意图值"（位置 / yaw / pitch / fov）。
//                   本模块**只**认这个，不读输入、不碰 GLFW。
//   CameraSystem    每帧决定活跃相机、推进混合、应用震动，最后写进
//                   `scene::Camera`（渲染用的那一台）。
//   CameraComponent 场景侧的"这台相机存在/多重要"（见 ecs/Components.h）。
//
// ------------------------------------------------------------
// 为什么震动是**加性**的
// ------------------------------------------------------------
// 震动如果直接改相机的位置/朝向，会污染相机自己的状态：震完之后的
// 朝向和震之前差一点点，连续震几次就会"越震越歪"。所以 `CameraPose`
// 永远保持**干净值**，震动只在最后一步叠加偏移 —— 关掉震动（默认
// amplitude = 0）时，写进相机的值与不震**逐位相同**。
// ============================================================

#include "ecs/Entity.h"

#include <glm/glm.hpp>

#include <string>

namespace scene {

class Camera;
class Scene;

struct CameraPose {
    glm::vec3 position{0.0f, 0.0f, 3.0f};
    float yaw = 0.0f;
    float pitch = -0.25f;   // 与 scene::Camera 同约定：正 = 相机在上方俯视
    float fov = 45.0f;

    bool operator==(const CameraPose& o) const {
        return position == o.position && yaw == o.yaw && pitch == o.pitch &&
               fov == o.fov;
    }
};

class CameraSystem {
public:
    // 每帧调用（**变量更新**相位，渲染之前）。dt 用真实帧时长：
    // 混合与震动是表现层的东西，不该被 timeScale 冻结（子弹时间里
    // 镜头仍然要跟得上手）。
    void update(Scene& scene, float dt);

    // ---- 意图 ----
    // 外部（PlayerController / 过场）给出想要的相机姿态。不调它也行 ——
    // 此时用活跃 CameraComponent 所属实体的世界变换推出来。
    void setDesiredPose(const CameraPose& pose) { m_desired = pose; m_hasDesired = true; }
    void clearDesiredPose() { m_hasDesired = false; }
    const CameraPose& desiredPose() const { return m_desired; }

    // ---- 混合 ----
    // 从**当前实际姿态**混到 `target`，用 seconds 秒。seconds <= 0 直接切。
    void blendTo(const CameraPose& target, float seconds, bool smoothStep = true);
    bool blending() const { return m_blendActive; }
    float blendAlpha() const;

    // ---- 震动 ----
    // amount ∈ [0,1]：0 = 无感，1 = 最大。多个来源会累加（并夹到 1）。
    void addShake(float amount);
    // 世界位置的震动：按距离衰减（爆炸 / 落地点）
    void shakeAt(const glm::vec3& worldPos, float amount, float radius);
    void clearShake();
    float trauma() const { return m_trauma; }

    // ---- 状态 ----
    ecs::Entity activeEntity() const { return m_active; }
    const std::string& activeName() const { return m_activeName; }
    bool hasActiveCamera() const { return m_hasActive; }
    // 本帧最后写进相机的姿态（**不含**震动偏移，自检与日志用）
    const CameraPose& appliedPose() const { return m_applied; }

    void setTrace(bool on) { m_trace = on; }

private:
    void selectActive(Scene& scene);
    CameraPose poseFromEntity(Scene& scene, ecs::Entity e) const;
    void apply(Scene& scene);

    ecs::Entity m_active{};
    std::string m_activeName;
    bool m_hasActive = false;

    CameraPose m_desired;
    bool m_hasDesired = false;
    CameraPose m_applied;

    // 混合
    CameraPose m_blendFrom;
    CameraPose m_blendTo;
    float m_blendDur = 0.0f;
    float m_blendT = 0.0f;
    bool m_blendActive = false;
    bool m_blendSmooth = true;

    // 震动
    float m_trauma = 0.0f;
    float m_time = 0.0f;

    bool m_trace = false;
    std::string m_lastLogged;
};

// 引擎层唯一一份 —— 编辑器与运行时共用（同一个场景只该有一个调度器）。
CameraSystem& activeCameraSystem();

// ------------------------------------------------------------ 自检
// 无窗口：造两台相机 + 混合 + 震动零值逐位比较。
// 由 `Game.exe --selftest camera` 调用。
int runCameraSelfTest();

} // namespace scene
