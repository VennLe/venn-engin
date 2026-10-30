#pragma once
// ============================================================
// scene/CameraMove —— 运镜算子 + 镜头光学（纯函数，无场景、无渲染）
//
// 这一层只回答三个问题，**不持有任何状态**：
//   ① 给我一个姿态和一个动词，推一下之后姿态变成什么？
//   ② 给我一个注视点，姿态的 yaw/pitch 该是多少（Track To）？
//   ③ 给我焦距和画幅，视场角是多少（以及反过来）？
//
// 之所以全是纯函数：运镜是"同一份输入必须永远得到同一份输出"的东西。
// 一旦让它持有状态（"上次推到哪了"），
//   · 拖时间轴回退时姿态就回不去了（成了积分器而不是求值器）；
//   · 自检只能跑一遍（第二遍从上次的终点开始）；
//   · 编辑器里预览与出包后运行会慢慢分叉。
// 状态属于**调用方**：时间轴持有播放头，`CameraComponent` 持有 rig 参数。
//
// ------------------------------------------------------------
// ⚠⚠ 姿态约定（与 `scene::Camera` 逐条一致，写错一处镜头就朝反方向）
// ------------------------------------------------------------
//   · 右手系、**Z 轴朝上**。
//   · `yaw` 弧度，绕世界 +Z。`pitch` 弧度，正 = **相机在上方俯视**。
//   · `forward = (−cosP·cosY, +cosP·sinY, −sinP)`
//         yaw=0 时看向 −X；yaw 增大 = 视线向右转。
//     这条式子与 `Camera::angleDir()` 是**互为反号**的同一个约定
//     （`angleDir` 指向相机背后），改动必须两边一起改。
//   · `right = normalize(cross(forward, worldUp))`，yaw=0 时 = +Y。
//   · `roll` 弧度，绕**视线**（forward）转，正 = 相机自身**顺时针**（画面
//     里的地平线右端下沉）。0 时与没有 roll 的支持前**逐位相同**。
//   · `shiftX / shiftY` 是**传感器比例的离轴偏移**（Blender 同一口径）：
//     0.1 = 成像中心挪 10% 传感器高度/宽度。用于建筑摄影防垂线、以及
//     "不转相机只挪构图"。0 时投影矩阵逐位不变。
// ============================================================

#include "scene/CameraMoveKind.h"
#include "scene/CameraSystem.h"   // CameraPose（本头不反向被它包含，无环）

#include <glm/glm.hpp>

namespace scene {

// 一次运镜动作的描述。`amount` 的单位由动词决定（见 cameraMoveIsAngular）：
//   · 角度类（Pan / Tilt / Roll / Orbit / Crane）→ **度**
//   · 距离类（Dolly / Truck / Pedestal）        → **米**
// ⚠ 角度用度、内部姿态用弧度 —— 这条不对称是故意的：面板/脚本/JSON 里
//   作者写的是"摇 15 度"，而 `CameraPose` 与 `SeqCameraKey` 存的是弧度
//   （与 `yaw` 同一口径）。转换只在 `applyCameraMove` 内部发生一次。
struct CameraMoveSpec {
    CameraMoveKind kind = CameraMoveKind::Pan;
    float amount = 0.0f;

    // Orbit / Crane 的支点（世界坐标）。
    // ⚠ 不提供时**不会**退化成"什么都不做" —— 见 applyCameraMove 的兜底
    //   说明。UI 上这两个动词必须逼作者填支点。
    glm::vec3 pivot{0.0f, 0.0f, 0.0f};
    bool pivotValid = false;
};

// 把 `spec` 施加到 `pose` 上（原地修改）。`amount == 0` 时**逐位不变**。
//
// 动词语义（都是**相对**当前姿态的增量）：
//   Pan       yaw += amount
//   Tilt      pitch += amount（夹到 ±89°：再看下去 lookAt 的基会退化）
//   Roll      roll += amount（归一化到 (−π, π]）
//   Dolly     沿 forward 平移 amount 米（正 = 往前推）
//   Truck     沿 right 平移 amount 米（正 = 向右）
//   Pedestal  沿世界 +Z 平移 amount 米（正 = 升高）
//   Orbit     位置绕 pivot 在**水平面**内转 amount 度，转完重新看向 pivot
//   Crane     位置绕 pivot 在**过相机的竖直平面**内转 amount 度，
//             朝向保持（不自动看向 pivot：摇臂的镜头是摄影师单独摇的）
//   Handheld  ⚠ 不走这里（它是持续的），见 applyHandheld
void applyCameraMove(CameraPose& pose, const CameraMoveSpec& spec);

// Track To：把姿态的朝向改成"看向 target"，位置不动。
// `rollDeg` 非 0 时同时设 roll（度）—— 摄影师要的"歪一点"通常和注视点
// 一起给，分两次调用会让中间那一帧出现"先摆正再歪"的抖动。
void aimAt(CameraPose& pose, const glm::vec3& target, float rollDeg = 0.0f);

// 手持噪声：**常驻**的、不衰减的抖动。
//
// 与 `CameraSystem::addShake` 的区别（两者都作用于相机，但完全不是一回事）：
//   · 这里的 `time` 是**连续时间**，输出是时间的确定性函数 → 同一时刻
//     采样多少次都一样，时间轴拖回去也对得上，
//   · 振幅不随时间衰减（呼吸不会停），
//   · 只动**朝向**（一点点位置），量级是"半度 + 两厘米"，
//     够让静止镜头不死板，不会让人晕。
//
// `phase` 是给每台相机一个不同的种子，否则两台手持镜头的抖动会一模一样
// （并排一放就能看出是复制粘贴的）。
void applyHandheld(CameraPose& pose, float time, float amplitudeDeg,
                   float frequencyHz, float phase, float posAmplitudeM = 0.0f);

// ---- 姿态基向量 ----
glm::vec3 poseForward(const CameraPose& p);
glm::vec3 poseRight(const CameraPose& p);
glm::vec3 poseUp(const CameraPose& p);

// 把姿态的**朝向**转成 `TransformComponent::rotation` 用的欧拉角
// （弧度，顺序 X→Y→Z，见 `TransformComponent::localMatrix`）。
//
// 为什么需要它：`CameraSystem::poseFromEntity` 的约定是"实体局部 **+X** =
// 视线、局部 **+Z** = 上"（Z-up 世界，与模型资产同一套），而 `CameraPose`
// 的朝向是 (yaw 绕世界 Z, pitch 绕水平轴) —— 这两套参数**不能**直接互换
// （把 pitch 填进 rotation.x 会得到一台绕自己横轴翻滚的相机）。所以走一次
// 基矩阵分解：先构造"局部 +X → forward、局部 +Z → up"的**右手**正交基，
// 再按 `Rx·Ry·Rz` 反解三个角。
//
// ⚠ 右手基下 +Y 只能是 **−right**（左手方向）：`(f, r, u)` 的行列式恒为 −1。
//   这不是笔误，改回 `(f, r, u)` 会得到一组镜像旋转，D1/D2 全红。
//
// ⚠ 相机在正俯视（pitch = ±89°）附近，`cross(f, up)` 会退化，这里用的是
//   `poseRight` 的兜底分支 —— 与 `Camera::angleDir` 同一套退化处理。
// 自检 `cameramove_euler_*` 用"反解出的 +X 与 poseForward 是否一致"来钉它。
glm::vec3 cameraEulerFor(const CameraPose& p);

// ---- 镜头光学 ----
// 视场角（垂直，度）⇄ 焦距（mm）。传感器高度默认 24 mm（全画幅 36×24；
// 电影 S35 是 24.89×18.66，接近到可以直接用 24 当默认）。
//
// ⚠⚠ **只用传感器高度，不用对角线。** 混用对角线会让 16:9 与 4:3 的
//    同一支 50mm 算出不同的视场角 —— 而现实里换画幅时光心确实会变，
//    但那是"裁剪系数"，不是这里要表达的。要表达的是"这支镜头多广"，
//    所以固定一个方向（垂直）换算，宽高比交给投影矩阵。
float focalToFovY(float focalMm, float sensorHeightMm = 24.0f);
float fovYToFocal(float fovYDeg, float sensorHeightMm = 24.0f);

// 由光圈值（f-stop）与焦距推"离焦收敛速度"，喂给 `CameraEffectSettings
// ::dofAperture`。物理上前景深 ∝ 1/N，这里的映射只求**单调且量级合适**：
// f/1.4 比 f/8 虚得多，且同一 f 值下长焦更虚（压缩带来的浅景深）。
//
// 为什么不是严格公式：引擎的 CoC 是屏幕像素半径，取决于渲染分辨率与
// 传感器尺寸的换算，严格公式里有一堆本引擎不存的量。与其写个"看起来
// 严谨、实际对不上"的式子，不如把**相对关系**钉死（1.4 是 8 的 5.7 倍）
// 并让作者在面板上直接看画面调。自检里钉的就是这条比例。
float fStopToDofAperture(float fStop, float focalMm, float sensorHeightMm = 24.0f);

// 快门角（度）→ `CameraEffectSettings::motionBlurStrength`。
//
// 180° 是电影标准（"180 degree rule"：曝光半个帧间隔），映射到 **1.0**
// —— 也就是着色器里的中性强度；360°（整帧曝光，最糊）→ 2.0；0 = 关。
// ⚠ 存**角度**而不是 Blender 那种"帧数"：帧数是 fps 相关的（0.5 帧在
//   24fps 是 180°，在 60fps 只有 90°），而本引擎的帧率在编辑器和玩家
//   机器上都不一样 —— 用帧数存会让同一个镜头在别人的机器上变糊/变清。
// 自检 `cameramove_shutter_*` 钉住 180°/90°/0 三个点。
float shutterToBlurStrength(float shutterDeg);

// ---- 自检 ----
// `Game.exe --selftest cameramove` / `MYVK_CAMERA_TEST=1`。
// 无窗口、无设备：只验算子与光学换算。
int runCameraMoveSelfTest();

} // namespace scene
