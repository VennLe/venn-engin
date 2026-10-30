#include "scene/CameraMove.h"

#include "scene/Camera.h"   // kPitchLimit（俯仰上限，与 Camera 同一份常量）

#include <algorithm>
#include <cmath>

namespace scene {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

const glm::vec3 kUp{0.0f, 0.0f, 1.0f};

// 水平面内的前进方向（丢掉竖直分量）。
// Orbit / Crane 的兜底支点靠它 —— 相机俯视时 forward 几乎竖直，直接拿来
// 当"前方 3 米"会把支点放到脚下，环绕就退化成原地转身。
glm::vec3 flatForward(const CameraPose& p) {
    glm::vec3 f = poseForward(p);
    f.z = 0.0f;
    const float l = glm::length(f);
    if (l < 1.0e-5f) return glm::vec3(-1.0f, 0.0f, 0.0f);  // 正俯视兜底
    return f / l;
}

// 绕任意轴转（Rodrigues）。glm 的 rotate 要矩阵，这里只要一个向量，
// 手写三行比构造 4×4 再乘更快，也避免"矩阵乘法顺序写反"这种不易看出的错。
glm::vec3 rotateAxis(const glm::vec3& v, const glm::vec3& axis, float rad) {
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    return v * c + glm::cross(axis, v) * s + axis * (glm::dot(axis, v) * (1.0f - c));
}

float normalizeAngle(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

// 确定性噪声：只用 sin，不用 random —— 同一时刻采样多少次都一样。
// 两个不同频率叠加是为了让波形**看起来不像正弦**（单一正弦的抖动是
// "匀速摇摆"，一眼假）。
float handNoise(float t, float freq, float phase) {
    return std::sin(t * freq * 2.0f * kPi + phase) * 0.6f +
           std::sin(t * freq * 2.13f * 2.0f * kPi + phase * 1.7f) * 0.28f +
           std::sin(t * freq * 0.37f * 2.0f * kPi + phase * 0.53f) * 0.12f;
}

} // namespace

// ---------------------------------------------------------------- 基向量

glm::vec3 poseForward(const CameraPose& p) {
    // 与 `Camera::angleDir()` 互为反号的同一个约定（那边指向**背后**）。
    // 改这里必须同时改那边，否则 Dolly 会变成"往后推"。
    const float cp = std::cos(p.pitch);
    return glm::vec3(-cp * std::cos(p.yaw), cp * std::sin(p.yaw),
                     -std::sin(p.pitch));
}

glm::vec3 poseRight(const CameraPose& p) {
    const glm::vec3 f = poseForward(p);
    glm::vec3 r = glm::cross(f, kUp);
    const float l = glm::length(r);
    // 正俯视 / 正仰视时叉积退化。此时"右"仍然有唯一的合理答案：
    // 沿 yaw 转 90° 的水平方向（与画面无关，但连续，不会在 89.9° 处翻面）。
    if (l < 1.0e-5f)
        return glm::vec3(std::sin(p.yaw), std::cos(p.yaw), 0.0f);
    return r / l;
}

glm::vec3 poseUp(const CameraPose& p) {
    // up 由 right × forward 得到，再绕 forward 转 roll。
    const glm::vec3 f = poseForward(p);
    const glm::vec3 r = poseRight(p);
    const glm::vec3 u = glm::cross(r, f);
    if (p.roll == 0.0f) return u;
    const float c = std::cos(p.roll);
    const float s = std::sin(p.roll);
    return u * c + r * s;
}

glm::vec3 cameraEulerFor(const CameraPose& p) {
    // 目标基：局部 +X → forward，局部 +Y → **−right**，局部 +Z → up。
    // 列主序 mat3 的构造是 mat3(col0, col1, col2)。为什么 +Y 是负的 right，
    // 见下面 `R(f, -r, u)` 上方那三条约束。
    const glm::vec3 f = poseForward(p);
    glm::vec3 r = poseRight(p);
    // 有 roll 时 right 也要跟着转（否则"实体歪 5°"在写回之后就丢了 ——
    // 这是最容易漏的一处：位置和朝向都对了，画面却突然正过来）。
    if (p.roll != 0.0f) {
        const glm::vec3 u0 = glm::cross(r, f);
        const float c = std::cos(p.roll);
        const float s = std::sin(p.roll);
        r = r * c - u0 * s;
    }
    const glm::vec3 u = glm::cross(r, f);

    // ⚠⚠ 列的顺序是 `(forward, −right, up)`。三条约束同时要满足，只有这一组：
    //   ① 局部 +X = 视线、+Z = 上 —— 本项目世界是 **Z-up**，实体的局部 +Z
    //      也必须是上，否则从相机上挂一个道具（父链继承）会横躺着。
    //   ② 必须是**右手基**（行列式 = +1），否则反解出的欧拉是"镜像旋转"，
    //      重建之后局部 +X 直接反向（第一版写成 `(f, r, u)`，摸不着头脑地
    //      在 yaw=90° 上红了一条）。而 `u = r × f` ⇒ `(f, r, u)` 的行列式
    //      恒为 −1（`r × u = −f`），所以 +Y 只能取 **−right**）。
    //   ③ 于是 +Y = 左手方向。这条在面板上会显示成"实体朝左转 90°"，
    //      是这组基唯一的代价，换来的是 +Z = up。
    const glm::mat3 R(f, -r, u);

    // 反解 `Rx(a)·Ry(b)·Rz(c)`（展开见下），glm 列主序：R[col][row]
    //   M[0][2] =  sin b            → b = asin(R[2][0])
    //   M[2][2] =  cos a · cos b    → a = atan2(−M[1][2], M[2][2])
    //   M[0][0] =  cos b · cos c    → c = atan2(−M[0][1], M[0][0])
    // 分子分母都用 atan2，避免 acos 在 ±1 附近的精度塌陷。
    const float b = std::asin(std::clamp(R[2][0], -1.0f, 1.0f));
    const float a = std::atan2(-R[2][1], R[2][2]);
    const float c = std::atan2(-R[1][0], R[0][0]);
    return glm::vec3(a, b, c);
}

// ---------------------------------------------------------------- 动词

void applyCameraMove(CameraPose& pose, const CameraMoveSpec& spec) {
    if (spec.amount == 0.0f) return;   // 逐位不变（自检 cameramove_noop）

    switch (spec.kind) {
        case CameraMoveKind::Pan:
            pose.yaw = normalizeAngle(pose.yaw + spec.amount * kDeg);
            break;

        case CameraMoveKind::Tilt:
            // 夹到与 `Camera::kPitchLimit` 同一个值：再往上看 lookAt 的基
            // 就退化了（画面翻转）。用同一份常量而不是自己写 1.5533。
            pose.pitch = std::clamp(pose.pitch + spec.amount * kDeg,
                                    -Camera::kPitchLimit, Camera::kPitchLimit);
            break;

        case CameraMoveKind::Roll:
            pose.roll = normalizeAngle(pose.roll + spec.amount * kDeg);
            break;

        case CameraMoveKind::Dolly:
            pose.position += poseForward(pose) * spec.amount;
            break;

        case CameraMoveKind::Truck:
            pose.position += poseRight(pose) * spec.amount;
            break;

        case CameraMoveKind::Pedestal:
            pose.position += kUp * spec.amount;
            break;

        case CameraMoveKind::Orbit: {
            // 支点兜底：相机水平前方 3 米。
            // ⚠ 是**兜底**不是"默认值" —— 面板上不给支点也应该看得到东西，
            //   否则作者点一下 Orbit 画面纹丝不动，会以为按钮坏了。
            const glm::vec3 pivot =
                spec.pivotValid ? spec.pivot : pose.position + flatForward(pose) * 3.0f;
            glm::vec3 v = pose.position - pivot;
            v = rotateAxis(v, kUp, spec.amount * kDeg);
            pose.position = pivot + v;
            // 环绕的定义就是"始终看着主体"，朝向必须跟着重算 ——
            // 只挪位置不重算，转半圈之后镜头会背对主体。
            aimAt(pose, pivot, pose.roll / kDeg);
            break;
        }

        case CameraMoveKind::Crane: {
            const glm::vec3 pivot =
                spec.pivotValid ? spec.pivot : pose.position + flatForward(pose) * 4.0f;
            glm::vec3 v = pose.position - pivot;
            glm::vec3 h = glm::vec3(v.x, v.y, 0.0f);
            if (glm::length(h) < 1.0e-4f) h = flatForward(pose);  // 支点正下方
            h = glm::normalize(h);
            // 轴 = h × up：让 v 在"过 pivot 与相机的竖直平面"内画弧。
            // 这个方向保证**正角度 = 相机升高**（吊臂抬起），与直觉一致。
            const glm::vec3 axis = glm::normalize(glm::cross(h, kUp));
            v = rotateAxis(v, axis, spec.amount * kDeg);
            pose.position = pivot + v;
            // ⚠ 与 Orbit 不同：**不**自动看向 pivot。摇臂升起时摄影师是
            //   单独摇镜头的，强行跟住支点等于把两个自由度焊死。
            break;
        }

        case CameraMoveKind::Handheld:
            // 见 CameraMove.h：手持是**持续**的，不是一次性增量。
            // 走到这里说明调用方把类型派错了（UI 上不该给它按钮）——
            // 什么都不做比"抖一帧就停"好，后者会被当成"手持做好了"。
            break;

        default:
            break;
    }
}

void aimAt(CameraPose& pose, const glm::vec3& target, float rollDeg) {
    const glm::vec3 d = target - pose.position;
    const float len = glm::length(d);
    if (len < 1.0e-5f) return;   // 目标就在镜头里（退化，不改朝向）
    const glm::vec3 f = d / len;
    // 由 forward 反解 yaw/pitch：与 `CameraSystem::poseFromEntity` 用
    // **同一组式子**（那边是从世界矩阵里的局部 +X 反解），两处不一致的话
    // "固定机位相机"和"Track To 相机"的同一朝向会算出不同的 yaw，
    // 混合时就会绕一下。
    pose.yaw = std::atan2(f.y, -f.x);
    pose.pitch = std::clamp(std::asin(std::clamp(-f.z, -1.0f, 1.0f)),
                            -Camera::kPitchLimit, Camera::kPitchLimit);
    if (rollDeg != 0.0f) pose.roll = normalizeAngle(rollDeg * kDeg);
}

void applyHandheld(CameraPose& pose, float time, float amplitudeDeg,
                   float frequencyHz, float phase, float posAmplitudeM) {
    // 振幅为 0 → **逐位不变**（默认关的时候相机状态与没有手持完全一致，
    // 与 CameraSystem 震动那条纪律同款）。
    if (amplitudeDeg <= 0.0f && posAmplitudeM <= 0.0f) return;

    const float f = std::max(0.05f, frequencyHz);
    // 三个通道频率不同：同步抖动看起来像整台机器在共振，不同步才像人。
    pose.yaw += amplitudeDeg * kDeg * handNoise(time, f, phase);
    pose.pitch += amplitudeDeg * 0.72f * kDeg * handNoise(time, f * 1.27f, phase + 1.9f);
    pose.roll += amplitudeDeg * 0.45f * kDeg * handNoise(time, f * 0.73f, phase + 3.3f);
    pose.yaw = normalizeAngle(pose.yaw);
    pose.pitch = std::clamp(pose.pitch, -Camera::kPitchLimit, Camera::kPitchLimit);

    if (posAmplitudeM > 0.0f) {
        pose.position += poseRight(pose) * (posAmplitudeM * handNoise(time, f * 0.61f, phase + 0.7f));
        pose.position += kUp * (posAmplitudeM * 0.8f * handNoise(time, f * 0.83f, phase + 2.4f));
    }
}

// ---------------------------------------------------------------- 光学

float focalToFovY(float focalMm, float sensorHeightMm) {
    if (focalMm <= 1.0e-3f || sensorHeightMm <= 1.0e-3f) return 45.0f;
    // fovY = 2·atan(h / 2f)
    return 2.0f * std::atan(sensorHeightMm * 0.5f / focalMm) / kDeg;
}

float fovYToFocal(float fovYDeg, float sensorHeightMm) {
    const float fov = std::clamp(fovYDeg, 0.5f, 179.0f);
    if (sensorHeightMm <= 1.0e-3f) return 50.0f;
    return (sensorHeightMm * 0.5f) / std::tan(fov * 0.5f * kDeg);
}

float fStopToDofAperture(float fStop, float focalMm, float sensorHeightMm) {
    // 参考点：50mm / f1.4 → 1.2（= `CameraEffectSettings::dofAperture` 的
    // 出厂值，这样"面板上填 50mm f/1.4"和"手动把 aperture 填 1.2"得到
    // 同一张画面，作者不会觉得两套参数在打架）。
    //
    // 两个因子都来自物理：
    //   · 光圈直径 ∝ 1/N      → f/8 的离焦只有 f/1.4 的 1/5.7
    //   · 弥散圆 ∝ 焦距        → 85mm 在同一 N 下比 50mm 虚（长焦压缩）
    const float kRefFocal = 50.0f;
    const float kRefFStop = 1.4f;
    const float kRefAperture = 1.2f;
    (void)sensorHeightMm;   // 保留参数：将来换画幅要按裁剪系数修正
    const float n = std::max(0.7f, fStop);
    const float f = std::max(5.0f, focalMm);
    return kRefAperture * (f / kRefFocal) * (kRefFStop / n);
}

float shutterToBlurStrength(float shutterDeg) {
    // 线性：180° → 1.0（中性），360° → 2.0，0 → 0（关）。
    // 上限 2 不是物理限制，是"再高就只剩糊"的实用上限。
    if (!(shutterDeg > 0.0f)) return 0.0f;   // 同时吃掉 NaN
    return std::min(shutterDeg / 180.0f, 2.0f);
}

} // namespace scene
