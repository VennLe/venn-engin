#pragma once
// ============================================================
// sequence/SequencerSystem —— 过场时间轴的播放器
//
// 职责只有三件：**推进播放头**、**接管相机**、**按区间派发事件**。
// 采样与解析都在 `SequenceAsset` 里（那边纯函数，好测）。
//
// ------------------------------------------------------------
// 为什么相机走 `setDesiredPose` 而不是直接写 `scene::Camera`
// ------------------------------------------------------------
// `CameraSystem` 的纪律是"`CameraPose` 永远保持干净值，震动只在最后一步
// 叠加"。如果时间轴自己去改相机的位置，过场里的爆炸震动就会污染轨迹，
// 而且下一次采样又会把它盖回去 —— 表现是"震动时有时无"。
// 走 desired 就是把时间轴当成**又一个意图来源**，与玩家相机同一条路。
//
// ⚠⚠ 因此调用顺序是硬约束：
//     玩家/角色相机 setDesiredPose  →  **SequencerSystem::tick**  →  CameraSystem::update
//   排在 update 之后这一帧的运镜就丢了（画面退化成玩家视角）；
//   排在玩家之前则每帧被玩家覆盖（过场根本不动）。
//
// ------------------------------------------------------------
// ⚠⚠ 场景里**必须有 `CameraComponent` 实体**
// ------------------------------------------------------------
// `CameraSystem::update` 在"没有活跃相机"时直接 return，desired 被静默忽略。
// 症状：日志里 SEQ-TICK 一切正常、事件照发，可画面一动不动。这是排查
// 时间轴时第一个要排掉的可能。
//
// ------------------------------------------------------------
// 编辑态（`dt == 0`）
// ------------------------------------------------------------
// **不推进、不发事件，只把当前播放头的姿态应用到相机。** 这样拖 Inspector
// 里的时间滑条就能预览运镜，而不必进 Play。
// ⚠ 这与 B3「`dt == 0` 不提取根运动」正好相反 —— 那里的跳帧是噪声，
//   这里的跳帧**就是用户想看的东西**。同一个 `dt == 0` 两种含义，
//   不要因为"别处这么写"就抄过去。
// ============================================================

#include "ecs/Entity.h"
#include "sequence/SequenceAsset.h"

#include <memory>
#include <string>
#include <vector>

namespace scene {
class Scene;
struct CameraPose;
}

namespace sequence {

// 时间比较阈值，与 `SequenceAsset.cpp` 里的那份同源（0.1 ms）。
constexpr float kSeqTimeEps = 1.0e-4f;

class SequencerSystem {
public:
    // 每帧调用。`dt` 用**游戏时间**（含 timeScale）：过场在暂停时该停。
    //
    // ⚠⚠ **本函数不碰任何相机系统。** 它只把采样结果放进 `pose()`，由
    //   调用方决定怎么应用：
    //     · 运行时 → `activeCameraSystem().setDesiredPose(pose())`
    //       （走这条才能让爆炸震动叠在运镜上，且不污染轨迹）
    //     · 编辑器 → 直接写 `scene.camera()`
    //       （编辑器视口是自由相机，压根不经过 `CameraSystem`）
    //   第一版把 `CameraSystem*` 当参数传进来，结果编辑器根本没法用 ——
    //   于是只能有"Game.exe 能看过场、编辑器不能"这种半残状态。
    void tick(scene::Scene& scene, float dt);

    // ---- 本帧的相机意图 ----
    // `hasPose() == false` → 这一帧没有要接管相机（没在播 / 无关键帧），
    // 调用方**必须**保持原样，不能"没值就当原点"。
    bool hasPose() const { return m_hasPose; }
    const scene::CameraPose& pose() const { return *m_pose; }

    // ---- 状态（日志 / 自检 / UI 回显）----
    int lastRunning() const { return m_lastRunning; }   // 本帧有几段时间轴"活着"
    float lastPlayhead() const { return m_lastPlayhead; }
    const std::string& lastSequenceName() const { return m_lastName; }
    ecs::Entity activeEntity() const { return m_active; }

    // 本帧派发出去的事件（`kind` / `payload` 的拷贝，供 UI 与将来的
    // C6 脚本导演 API 消费）。每帧开头清空。
    struct FiredEvent {
        std::string kind;
        std::string payload;
        float t = 0.0f;
    };
    const std::vector<FiredEvent>& firedEvents() const { return m_fired; }

    // ---- 控制 ----
    // 清运行时状态（切场景 / 退出 Play）。**不清组件的纯数据**
    // （路径、speed 那些是作者写的，不该被抹掉）。
    void resetRuntime(scene::Scene& scene);

    void setTrace(bool on) { m_trace = on; }

private:
    // 选"哪一段过场在跑"：enabled && playing && 资产有效 && 未结束 里
    // priority 最大者。⚠ 这个 priority 与 `CameraComponent::priority`
    // **不是一回事** —— 那个选"用哪台相机"，这个选"用哪段过场"。
    ecs::Entity selectActive(scene::Scene& scene, int* runningCount);

    void fire(scene::Scene& scene, ecs::Entity e, const SeqEvent& ev);

    ecs::Entity m_active{};
    int m_lastRunning = 0;
    float m_lastPlayhead = 0.0f;
    std::string m_lastName;
    std::vector<FiredEvent> m_fired;

    // 用 unique_ptr 存：`CameraPose` 需要完整定义，而头里只前向声明了它
    // （include `scene/CameraSystem.h` 会把 scene 头拖进每个用组件的地方）。
    std::unique_ptr<scene::CameraPose> m_pose;
    bool m_hasPose = false;

    bool m_trace = false;
    bool m_loggedNoCamera = false;
};

// 把意图写进 `scene::Camera` —— **编辑器路径专用**。
// 运行时不要用它：那条路必须走 `CameraSystem`，否则震动与混合都被绕过。
// 编辑器视口是自由相机（不经过 `CameraSystem`），所以只能直接写。
void applyPoseToSceneCamera(scene::Scene& scene, const scene::CameraPose& p);

// 引擎层唯一一份（与 `activeCameraSystem()` 同一理由：一个场景只该有一个
// 播放头，否则编辑器与运行时会各推一份、互相打架）。
SequencerSystem& activeSequencerSystem();

// ------------------------------------------------------------ 自检
// 无窗口：解析 / 采样 / yaw 短路 / step / 半开区间 / 跨圈 / 首帧。
// 由 `Game.exe --selftest sequence` 调用。
int runSequencerSelfTest();

} // namespace sequence
