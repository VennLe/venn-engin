#pragma once
// ============================================================
// assetio/AnimOps —— 动画关键帧压缩（**误差有界**的抽帧）
//
// 为什么压缩一定能压：
//   FBX 导入时是按固定 30fps **逐帧重采样**的（见 FbxImport），
//   于是"一个静止的 IK 手臂"也会被存成 30 个完全相同的四元数。
//   真实资产里 60~90% 的关键帧是这种冗余。
//
// 为什么不做量化（u16）：
//   这个引擎的运行时采样器 `assets::AnimTrack` 是 float 的
//   （`Skeleton.h`），`sampleTrack` 直接线性/slerp 插值。要把关键帧
//   量化成 u16，就得让**采样器**知道 scale/bias —— 那是把运行时也
//   一起改，属于"格式变了、运行时没变"的经典事故现场。
//   抽帧是纯删冗余，运行时一行都不用改，收益却拿到了大头。
//   （量化留给"采样器换存储后端"那一轮，接口在这里已经留好位置。）
//
// ⚠ 抽帧必须**保时长**：`AnimationClip::duration` 不跟着缩。
//   循环播放按 duration 回绕，duration 一缩，动画就"变快且卡点"。
// ============================================================

#include "assetio/SourceModel.h"

#include <cstddef>

namespace assetio {

struct ReduceStats {
    std::size_t keysIn = 0;
    std::size_t keysOut = 0;
    std::size_t tracksIn = 0;
    std::size_t tracksOut = 0;
    float maxRotErrDeg = 0.0f;   // 实际产生的最大旋转误差（度）
    float maxPosErr = 0.0f;      // 实际产生的最大位移误差（米）
    float maxScaleErr = 0.0f;
};

// 单条轨道抽帧。
//   rotationToleranceDeg —— 旋转容差（度）。0.1° 在 1080p 的角色上
//                           肉眼不可见（骨骼长度 0.3m 时位移 < 0.5mm）。
//   positionTolerance    —— 位移容差（米）
//   scaleTolerance       —— 缩放容差（倍数）
// 返回这条轨道进了多少、出了多少。
std::size_t reduceTrack(SourceTrack& t, float rotationToleranceDeg = 0.1f,
                        float positionTolerance = 1.0e-4f,
                        float scaleTolerance = 1.0e-4f,
                        ReduceStats* stats = nullptr);

// 对整个 clip 的每条轨道做抽帧。duration 原样保留。
ReduceStats reduceClip(SourceClip& clip, float rotationToleranceDeg = 0.1f,
                       float positionTolerance = 1.0e-4f,
                       float scaleTolerance = 1.0e-4f);

// 两个四元数之间的夹角（度）。用于误差评估，也可被自检直接调用。
float quatAngleDeg(const glm::vec4& a, const glm::vec4& b);

} // namespace assetio
