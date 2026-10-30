#pragma once
// ============================================================
// scene/CameraMoveKind —— 运镜"动词"的名字表（零依赖）
//
// 这个头**只放一个枚举和三个小函数**，不 include glm、不 include 任何
// 引擎头。原因和 `sequence/SequenceAsset.h` 自我约束的理由完全一样：
// `ecs/Components.h` 需要把"这台相机默认用什么动词 / 手柄绑了哪个"存进
// 组件里，而 `ecs/Components.h` 被几乎所有 TU 包含。一旦这里牵进 glm 或
// `CameraSystem.h`，`CameraSystem.h → ecs/Entity.h → ecs/Components.h`
// 就成环 —— 那正是 MEMORY 里 C4099 / LNK2019 那组坑的来源。
//
// ------------------------------------------------------------
// 为什么"动词"要单独成一层，而不是给相机加九个布尔开关
// ------------------------------------------------------------
// 运镜动作是**相对量**：`Pan(+15°)` 的意思是"在当前朝向上再向右摇 15°"，
// 它必须作用在**当前姿态**上，而不是"把 yaw 设成 15°"。两者的差别在
// 连续两次 Pan 时立刻暴露：设值型会停在 15°，增量型会到 30°。
// 所以它们是**算子**（`applyCameraMove(pose, spec)`），不是状态。
//
// 状态只有两种：
//   · 时间轴上的关键帧（`SeqCameraKey`，绝对姿态）—— "拍多久、走到哪"
//   · 相机的实时 rig（`CameraComponent` 上的注视 / 手持）—— "一直怎么看着"
// 动词是**作者在面板上推一下**、**脚本调一次**那个动作本身。
//
// ------------------------------------------------------------
// 九个动词怎么来的
// ------------------------------------------------------------
// 直接对齐摄影机术语（Blender 的 Numpad / G+轴 那套手感只是它的 UI）：
//
//   Pan       摇      绕竖直轴转（左右看）   → yaw 增量
//   Tilt      俯仰    绕横向轴转（抬头低头） → pitch 增量
//   Roll      翻滚    绕视线转（画面歪）     → roll 增量
//   Dolly     推拉    沿视线前后             → position += forward * m
//   Truck     横移    沿相机右方向           → position += right * m
//   Pedestal  升降    沿世界竖直             → position.z += m
//   Orbit     环绕    绕支点水平公转         → 位置绕 pivot 转 + 始终看向它
//   Crane     摇臂    绕支点在竖直平面内转   → 位置绕 pivot 转（高度跟着变）
//   Handheld  手持    持续微抖               → **不是一次性算子**，见下
//
// ⚠⚠ `Handheld` 与其余八个**不同构**：前八个是"施加一次就结束"的增量，
//    手持是"只要开着就每帧都在动"。它靠时间驱动的确定性噪声实现
//    （`applyHandheld`），不经过 `applyCameraMove` 的增量路径。
//    把它塞进同一个 switch 会得到一个"推一下抖一下就停"的假手持 ——
//    那跟 `CameraSystem` 的震动（受击反馈）是两回事：
//      震动 = 事件驱动的、会衰减的（trauma 包络）
//      手持 = 常驻的、不衰减的、低频的（呼吸感）
// ============================================================

#include <cstdint>
#include <string>

namespace scene {

enum class CameraMoveKind : std::uint8_t {
    Pan = 0,
    Tilt,
    Roll,
    Dolly,
    Truck,
    Pedestal,
    Orbit,
    Crane,
    Handheld,
    Count
};

// 稳定名字（写进 JSON / 日志 / 脚本）。**只追加不改名** —— 已有资产里
// 存的是这些字符串，改名等于让老工程里的动作静默失效。
const char* cameraMoveName(CameraMoveKind k);
bool parseCameraMove(const std::string& s, CameraMoveKind& out);

// 旋转类（Pan/Tilt/Roll/Orbit/Crane）的 amount 单位是**度**；
// 平移类（Dolly/Truck/Pedestal）的单位是**米**。
bool cameraMoveIsAngular(CameraMoveKind k);

// 是否需要支点（Orbit / Crane）。这两者没有支点就只能凭空兜底，
// UI 上必须逼作者给一个（"镜头绕到了莫名其妙的地方"十次有九次是支点错）。
bool cameraMoveNeedsPivot(CameraMoveKind k);

} // namespace scene
