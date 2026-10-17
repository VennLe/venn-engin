#pragma once
// ============================================================
// scene/PhysicsSettings —— 物理世界的**场景级**参数
//
// ------------------------------------------------------------
// 为什么要单独放一层，而不是直接用 physics::PhysicsWorldSettings
// ------------------------------------------------------------
// 依赖方向是 `physics → scene`（CollisionWorld 的操作数就是 Scene），
// 所以 scene 层**不能**回头 include physics 的头文件 —— 那会成环。
// 这个结构体因此是"纯数据"，字段名与物理引擎的那一份一一对应，
// 由**编辑器**在 Play 时翻译成 `physics::PhysicsWorldSettings`。
//
// 这份翻译只有一处（EditorApp::buildPhysicsWorld），所以两边的字段
// 即使将来不同步，也只是"这个旋钮暂时不生效"，不会出现两套语义。
//
// 它是场景数据（跟相机、太阳一样），所以随场景一起序列化 —— 一份
// 场景应该有自己的一套重力/步长，换场景就换物理手感。
// ============================================================

#include <glm/vec3.hpp>

namespace scene {

struct PhysicsSettings {
    // 总开关。关掉时 Play 不建物理世界，所有刚体就只是普通静态物体。
    bool enabled = true;

    // ---- 世界 ----
    glm::vec3 gravity{0.0f, 0.0f, -9.81f};
    // Play 时是否自动加一张**无限地面**（z = 0 平面）。
    //
    // 为什么默认开：编辑器场景里通常没有"地板物体"（栅格只是屏幕空间的
    // 参考线，不是几何）。不开的话第一次 Play 的体验就是"所有东西直接
    // 掉出世界"，而那是**看起来像 bug 的正确行为**。要真实下落就在
    // Physics 面板里关掉它，或者自己摆一块地板并给它加刚体。
    bool groundPlane = true;

    // ---- 步长（固定步长 + 累加器，见 physics/PhysicsWorld.h 的说明）----
    float fixedTimeStep = 1.0f / 60.0f;
    int maxSubSteps = 4;

    // ---- 求解器 ----
    int velocityIterations = 8;
    int positionIterations = 3;
    float baumgarte = 0.2f;
    float slop = 0.005f;

    // ---- 休眠 ----
    bool allowSleep = true;
    float sleepLinearThreshold = 0.05f;   // m/s
    float sleepAngularThreshold = 0.15f;  // rad/s
    float sleepTime = 0.5f;               // 秒
};

} // namespace scene
