#pragma once
// ============================================================
// character/ViewRig —— 第一／第三人称的**同一套**装配与相机口径
//
// 这个文件存在的理由，是"同一个人在两台程序里长得不一样"：
//   · 编辑器 Play 的 `EditorApp::runPlayers` 与
//     运行时的 `runtime::PlayerController` 各写了一套"眼睛在哪、往哪走、
//     镜头怎么摆"；
//   · `EditorScene::bindDefaultCharacter` 与
//     `RuntimeApp::attachPlayerModel` 各写了一套"胶囊多大、镜头多远"。
//
// 于是历史上出现了两种**只有一边错**的症状（都能在截图里看出来，日志里
// 一条错都没有）：
//   ① 第一人称相机不在眼睛里。编辑器那条路写成
//      `setTarget(eye + dir) + setDistance(1)`，而 `Camera::position()`
//      是 `target + angleDir*distance`、且 `angleDir == dir` ——
//      相机实际落在**眼前 2 m**；运行时那条路把眼高写成
//      `胶囊中心 + 1.464`，1.8 m 的人算出来是 **2.36 m**（头顶上方半米）。
//   ② 第一人称按 W 的方向与视线相反（运行时的 `fwd` 用的是第三人称的
//      `(cos yaw, +sin yaw)`，而第一人称的视线是 `(cos yaw, -sin yaw)`）。
//
// 参照的对象是 UE5 的 FirstPerson / ThirdPerson 模板（`ACharacter` +
// `USpringArmComponent` + `UCameraComponent`）：
//   · 眼位 = 骨架的 **head 骨**（UE 把 FirstPersonCamera 挂在 Mesh1P 的
//     头部 socket 上），不是"脚 + 一个常数"；
//   · 第一人称**看不见自己的身体**（`bOwnerNoSee`），但**影子还在**
//     （`bCastHiddenShadow`）—— 低头能看见自己的影子，这是"我在这个世界里"
//     最便宜的一条反馈；
//   · 相机朝向**永远来自输入**（`bUsePawnControlRotation`），只借骨骼的
//     **位置**。把朝向也交给头骨，LookAt / 弹簧骨一转，镜头就跟着甩 ——
//     那不是第一人称，那是"头上绑了个 GoPro"；
//   · 换视角是**一段过渡**（`SetViewTargetWithBlend`），不是一帧硬切。
//
// 与 UE 的差异只有一处，是刻意的：这里的"平滑"全部走
// `1 - exp(-dt/tau)`（帧率无关），而不用 UE 的 `FInterpTo`
// （后者是逐帧 lerp，60 fps 与 144 fps 的手感不同）。
// ============================================================

#include "ecs/Components.h"

#include <glm/glm.hpp>

#include <string>

namespace assets {
struct Skeleton;
}
namespace scene {
class Scene;
struct CameraPose;
} // namespace scene

namespace character {

// ============================================================
// 一、身体度量（按模型身高折算）
// ============================================================
//
// 三条装配路径（编辑器绑默认角色 / 运行时绑外部模型 / 角色面板的预览）
// 都在算同一组数：胶囊半径、胶囊半高、台阶高、跳跃初速、镜头距离、
// 注视点高度。抄三遍的下场不是"某天有人改错"，而是**它已经错了**：
// 早期版本里三处的半径系数分别是 0.16 / 0.16 / 0.18，1.8 m 的角色
// 在角色面板里比在 Play 里瘦一圈。
//
// 所有系数都对着"1.8 m 的成年人"标定，改之前先想清楚 1.8 m 时是多少。
struct BodyMetrics {
    float height = 1.8f;      // 模型几何高（脚底到头顶）
    float radius = 0.288f;    // 0.16 h
    float halfHeight = 0.612f;// h/2 - r
    float stepHeight = 0.36f; // 0.20 h，夹在 [0.20, 0.50]
    float jumpSpeed = 3.7f;   // √(2·g·0.39h)，夹在 [3.0, 5.0]
    float camDistance = 3.6f; // 2.0 h，夹在 [2.5, 6.5]  —— 角色占屏 ~1/3
    float focusHeight = 0.756f;  // 0.42 h：注视点（胸／肩之间）
    float eyeHeight = 1.656f;    // 0.92 h：**没有骨架时**的眼高兜底
};

// 按身高算出整组度量。height <= 0 时按 1.8 m 处理（资产缺 bounds 的兜底）。
BodyMetrics fitBody(float height);

// ============================================================
// 二、眼位（第一人称相机挂点）
// ============================================================

// 眼睛的世界位置。`fromBone == false` 表示骨架里没有可用的头骨、
// 或者玩家身上根本没有蒙皮 —— 此时退回 `脚 + (0,0,eyeHeight)`。
//
// ⚠ 一定要报出 `bone`：骨名匹配是"包含 + 不分大小写"的，
//   `head` 可能命中 `mixamorig:Head` 也可能命中 `Head`，还可能是
//   `Bip01 Head`。出问题时唯一能查的就是"它到底挂在哪根骨上"。
struct EyeAnchor {
    bool valid = false;
    bool fromBone = false;
    std::string bone;             // 命中的骨名（空 = 没用骨骼）
    glm::vec3 position{0.0f};     // 世界（引擎 Z-up）
    float heightAboveFeet = 0.0f; // 相对玩家原点的抬升，日志用
};

// 解算眼位。玩家身上要有 `SkinnedMeshComponent`（模型根就是玩家实体）。
// 骨骼 → 网格 → 世界的换算见 .cpp 里的推导（它必须与渲染的蒙皮公式
// **逐项一致**，否则眼睛会偏离画面里那颗头）。
//
// 对标 `ACharacter::GetActorEyesViewPoint` 的标准改法
// （`Location = GetMesh()->GetSocketLocation("head");`）：
//
//   眼位 = **头骨在世界里的变换** + `pc.eyeOffset`
//
// `eyeOffset` 就是 UE 里那个 socket 的相对变换 / 相机组件的 relative
// location（UE4 的 FPS 模板写死 `(-39.56, 1.75, 64)`，UE5 的 Mannequin
// 是在骨架里摆一个 `head` socket）。**偏移永远是作者的数据，不是引擎猜的**：
// 头骨在颅底，眼睛在它上方 4~5 cm，这 4~5 cm 该由资产或作者给。
//
// 没有骨架 / 骨架里没有头骨时才退回 `脚 + (0,0,eyeHeight)`
// （UE 的对应物是 `APawn::BaseEyeHeight`）。
EyeAnchor resolveEye(const scene::Scene& scene, ecs::Entity player,
                     const ecs::PlayerComponent& pc);

// 在骨架里找头骨。用 `kJointMatchDefault`（不分大小写 + 允许包含 +
// 跳过末端辅助骨），因为 `Head_end` / `Head_tip` 的位置在**骨尖**上，
// 拿它当眼位会让人"从额头顶端看世界"。
int findEyeBone(const assets::Skeleton& skel, const std::string& name);

// ============================================================
// 三、视线基（WASD 的方向）与 Camera 的角度换算
// ============================================================
//
// 这里刻意用**一个标准口径**，而不是"两种模式各一套"：
//
//     视线 yaw：0 = 看向 +X，增大 = 向右转（UE 的 FRotator.Yaw）
//     视线 pitch：正 = 抬头（UE 同）
//
// ⚠ 它与 `scene::Camera` 的 yaw **差 π**：`Camera::angleDir()` 给的是
//   "**相机**在目标的哪一侧"，而 Camera 的视线 = -angleDir。所以
//   "视线 yaw" → "Camera yaw" = +π（见 cameraAnglesFromLook）。
//   早期两条链各自按自己的直觉写，结果同一个 yaw 在编辑器里相机在
//   角色右前方、在 Game.exe 里在左后方 —— 同一个场景两副样子。
//
// ⚠ 顺带记一下两台宿主**内部**的 yaw 符号（它们与标准口径的关系不同，
//   调用方自己换算，别在函数里猜）：
//     · `EditorApp::runPlayers` 的 `PlayerComponent::yaw` = 标准口径
//       （`lookBasis(pc.yaw)` 就是视线）；
//     · `runtime::PlayerController::m_yaw` 是**反号**的（它的第三人称
//       一直把 `(cos m_yaw, sin m_yaw)` 当"前"），换算成标准口径要取负：
//       `lookBasis(-m_yaw)`。
//   这条差异不是我们想要的，但它已经渗进了第三人称的整套手感与若干
//   截图基线；**第一人称**从今天起只走标准口径，第三人称另案统一。
void lookBasis(float viewYaw, glm::vec3* look, glm::vec3* right);

// 视线 (yaw, pitch) → `scene::Camera` 的 (yaw, pitch)。
//
//   camYaw   = wrapPi(viewYaw + π)
//   camPitch = -lookPitch        （Camera 的 pitch 正 = 相机在上方俯视）
//
// ⚠ 必须成对使用：只改 yaw 不改 pitch，画面会上下颠倒地转到背后
//   （2026-10-11 修过一次同类 bug，见 EditorApp 里 `setPitch(effPitch)` 的注释）。
void cameraAnglesFromLook(float viewYaw, float lookPitch, float* camYaw,
                          float* camPitch);

// 把角度归一化到 (-π, π]。两条链都在用，收在这里一份 ——
// "自己写 fmod" 的版本在 -π 那一侧会差一个 2π，表现为"转到正后方时镜头猛地一甩"。
float wrapPi(float rad);

// ============================================================
// 四、帧率无关的平滑
// ============================================================

// 指数逼近系数：`1 - exp(-dt/tau)`。dt=0 或 tau<=0 时返回 1（直接到目标）。
//
// ⚠ 为什么不是 `min(1, speed*dt)`：后者在 144 fps 下每帧只走 1/2.4 的距离，
//   60 fps 下走 1 —— 同一个 `speed` 在两台机器上是两种手感。指数形式
//   里的 `dt` 是指数上的，帧率只影响采样密度，不影响轨迹。
float dampFactor(float dt, float tau);

// 眼位阻尼器。第一人称的镜头跟着头骨走，而头骨会随呼吸 / 走动 / 眨眼
// 微动 —— 直接硬跟会把动画的每一丝抖动原样放大到整个屏幕（晕）。
// UE 的对应物是 SpringArm 的 `bEnableCameraLag`（第三人称）与
// FirstPerson 里各家自己写的 eye lag。
//
// tau 的取值：0.03~0.06 s。再大就会"转头时画面慢半拍"。
struct EyeDamp {
    bool init = false;
    glm::vec3 value{0.0f};

    void reset() {
        init = false;
        value = glm::vec3(0.0f);
    }
    // tau <= 0 → 硬跟（逐位等于 target）。
    const glm::vec3& step(const glm::vec3& target, float dt, float tau);
};

// ============================================================
// 五、换视角的过渡
// ============================================================
//
// UE 里是 `PlayerController::SetViewTargetWithBlend(..., BlendTime)`：
// 相机从当前姿态平滑推到新姿态，过程中玩家**还能动**（新姿态每帧在变）。
// 硬切的问题不是"不好看"，是"玩家会短暂失去空间感"——第一人称到
// 第三人称的那一帧，视野从眼睛飞到背后 3.6 m，眼睛需要 2~3 帧才能
// 重新定位画面里的自己。
//
// 语义与 `CameraSystem::blendCut` 一致：**起点快照，目标每帧现算**。
// 但那份实现绑在场景相机系统上，编辑器 Play 那条链不经过它 ——
// 所以这里有一份两边都能用的最小实现（纯数学，可脱离场景跑用例）。
struct ViewTransition {
    bool active = false;
    float t = 0.0f;      // 已过的秒数
    float duration = 0.0f;
    glm::vec3 fromPos{0.0f};
    float fromYaw = 0.0f;
    float fromPitch = 0.0f;
    float fromFov = 45.0f;

    bool running() const { return active; }
    // 进度 0..1（曲线已应用）。没在跑时返回 1。
    float alpha() const;
    void begin(const scene::CameraPose& from, float seconds);
    void cancel() { active = false; t = 0.0f; duration = 0.0f; }
    // 推进 dt，返回"这一帧该用的姿态"。没在跑时**原样返回 target**。
    //
    // ⚠ 位置线性插值、角度走**最短角差**（跨 ±π 时不绕远路）。直接对
    //   yaw 做 lerp 会在 `yaw = 3.13 → -3.13` 时让镜头原地转一整圈。
    scene::CameraPose step(const scene::CameraPose& target, float dt);
};

// ============================================================
// 六、可见性（bOwnerNoSee / bCastHiddenShadow）
// ============================================================
//
// 把玩家的**所有子网格**设成"看不见 / 看得见"。返回改了几个实体。
//
// `castShadowWhenHidden` 就是 UE 的 `bCastHiddenShadow`：第一人称下身体
// 不可见，但影子仍要画 —— 否则低头看不见自己的影子，玩家会觉得
// "我是台悬浮摄像机"。默认开（见 `PlayerComponent::bodyShadowWhenHidden`）。
//
// ⚠ 只改 `VisibilityComponent`，**不动** `MeshComponent`：几何、蒙皮、
//   动画都照常跑，所以影子里是**当前帧的姿势**，不是 T-pose。
int applyBodyVisibility(scene::Scene& scene, ecs::Entity player, bool visible,
                        bool castShadowWhenHidden);

// 切换视角：写 `pc.mode`、套可见性、返回是否真的换了。
// 相机过渡**不在这里做** —— 起点由调用方在切之前自己抓（它才知道当前
// 画面上的姿态），见两个宿主里的用法。
//
// `outMeshes`（可空）回填"这次套可见性碰了几个网格"。用它而不是让调用方
// 自己再数一遍：数一遍就要再走一遍子节点，而且**时机**必须与套用同帧
// （中间有人加了子网格就对不上）。日志里 `mesh=0` 是"身体没建出来"的
// 唯一直接证据 —— 第一人称看不见人时，先看这一位。
bool setViewMode(scene::Scene& scene, ecs::Entity player,
                 ecs::PlayerMode mode, int* outMeshes = nullptr);

// 日志用的一行摘要（`VIEW ` 前缀由调用方加）。切视角时打一条，
// 自动化断言靠它判"模式真的换了 + 身体真的藏了"。
// `meshes` < 0 表示"没数"（打印成 `?`）。
std::string describeView(const ecs::PlayerComponent& pc, const EyeAnchor& eye,
                         int meshes);

} // namespace character
