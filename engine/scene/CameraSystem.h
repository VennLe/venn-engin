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
#include "scene/CameraPath.h"   // m_path（Follow Path 的缓存，见 CameraPath.h）

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace scene {

class Camera;
class Scene;

struct CameraPose {
    glm::vec3 position{0.0f, 0.0f, 3.0f};
    float yaw = 0.0f;
    float pitch = -0.25f;   // 与 scene::Camera 同约定：正 = 相机在上方俯视
    float fov = 45.0f;

    // ---- 2026-10-30（运镜体系）新增的三个通道 ----
    //
    // ⚠⚠ 三者默认 0 = **逐位回到加它们之前的行为**。这不是"顺手加的字段"，
    //   是整套运镜的验收前提：没配过的相机算出来的 view/proj 矩阵必须
    //   与上一版完全一样（`Camera::viewMatrix` 在 roll==0 时走原样的
    //   lookAt，`projMatrix` 在 shift 全 0 时一个元素都不碰）。
    //
    // `roll` 单位是**弧度**（与 yaw/pitch 同口径）。正 = 相机自身顺时针，
    // 画面里的地平线右端下沉。⚠ 面板与 JSON 里用**度**，换算只在
    // `CameraSystem::apply` / `applyPoseToSceneCamera` 两处发生 —— 与 fov
    // 的"内部弧度、界面度"惯例不同（fov 本身就是度），别照抄错。
    //
    // `shiftX / shiftY` 是**传感器比例的离轴偏移**（Blender 同一口径），
    // 0.1 = 成像中心挪 10% 传感器尺寸。作用相当于"不转相机只挪构图"，
    // 建筑摄影靠它把垂直线拉直。
    float roll = 0.0f;
    float shiftX = 0.0f;
    float shiftY = 0.0f;

    bool operator==(const CameraPose& o) const {
        return position == o.position && yaw == o.yaw && pitch == o.pitch &&
               fov == o.fov && roll == o.roll && shiftX == o.shiftX &&
               shiftY == o.shiftY;
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

    // ============================================================
    // 多机位切镜（导演覆盖，2026-10-30；运镜体系 §七）
    // ============================================================
    //
    // 时间轴 marker 说"这一段用 3 号机"，那台机就得赢 —— 压过 `priority`。
    // 这是"谁在拍"与"这台相机多重要"两件事的分工：priority 是**作者摆场景
    // 时的静态意图**（备用机位低、主相机高），导演覆盖是**叙事进行中的
    // 决定**。用 priority 去实现切镜是错的：切完还得记账改回去，而"改回去"
    // 那一刻就是 bug 温床（忘了改 → 整局卡在过场机位）。
    //
    // ⚠ 覆盖是**每帧重设**的（宿主每帧把 marker 的结论喂进来），不是一次性
    //   开关 —— 这样"时间轴停了"就自然回落，不需要谁去清理。
    // ⚠ guid 指向的实体不存在 / 被禁用 → 打一次 `CAM-CUT-MISS` 并**自动
    //   清掉**覆盖，回落 priority。不能永久卡住：相机被删是编辑期的常态。
    void setDirectorCamera(std::uint64_t guid);
    void clearDirectorCamera();
    std::uint64_t directorCamera() const { return m_directorGuid; }

    // 从**当前实际姿态**混合到"接下来每帧算出来的姿态"，用 seconds 秒。
    //
    // 与 `blendTo(target, ...)` 的区别（这是切镜必须走这条的原因）：
    //   `blendTo` 会把这个 target 钉成 `m_desired` —— 也就是**冻结**姿态，
    //   之后再也不跟实体/路径/注视目标。切镜要的是"淡入到新机位，然后
    //   新机位继续按它自己的 rig 活着"（手持、Track To、路径都还要动）。
    //   所以这里只**捕获起点**，目标留空由 `update` 每帧现算。
    void blendCut(float seconds);

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
    void apply(Scene& scene, float dt);

    ecs::Entity m_active{};
    std::string m_activeName;
    bool m_hasActive = false;

    // 导演覆盖（多机位切镜）。0 = 没有覆盖，走 priority 扫描。
    std::uint64_t m_directorGuid = 0;
    // 「指针指向的相机没了」只喊一次 —— 每帧喊会把日志刷死（等于没有日志）。
    bool m_directorWarned = false;

    // Track To 的**平滑注视点**（2026-10-30）。
    //
    // ⚠ 为什么平滑"点"而不是平滑"朝向"：朝向上做指数逼近要处理 yaw 的
    //   绕圈（179°→−179° 会反向甩一整圈），点不用。而且点的平滑天然
    //   是"跟随速度随距离变化"—— 目标走得快，镜头追得也快，手感比
    //   固定角速度逼近更像真人跟拍。
    // ⚠ 一旦当前相机**没有**注视目标，`m_lookValid` 立刻清掉：否则换到
    //   一台没配 Track To 的相机时，镜头会先"回忆"上一台的注视点。
    glm::vec3 m_lookPoint{};
    bool m_lookValid = false;

    // Follow Path 的路径缓存（2026-10-30）。
    // ⚠ 缓存键 = guid + 点数 + 首末点 + 闭合位。点位**中间**的微调不会让
    //   它失效 —— 这是有意的取舍：面板拖中间点时每帧重建弧长表（16 步/段）
    //   在几段路径上是可接受的，但"每帧 smoothHandles + 分配 vector"会在
    //   运镜时带来可见的抖动。真正的位置每次都从 m_path 现采样，所以中间
    //   点**形状**改了是立刻生效的，只有**速度分布**（弧长表）沿用旧的。
    CameraPath m_path;
    std::uint64_t m_pathGuid = 0;
    std::size_t m_pathPts = 0;
    bool m_pathClosed = false;
    glm::vec3 m_pathA0{0.0f};
    glm::vec3 m_pathA1{0.0f};
    int m_pathSeg = -1;

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
