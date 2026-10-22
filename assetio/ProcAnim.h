#pragma once
// ============================================================
// assetio/ProcAnim —— 程序化生成四段基础动画（idle / walk / run / jump）
//
// 前提：骨架来自 `AutoRig`（或任何**纯平移层级 + 本文件认识的关节名**）。
// 因为那一档的 `restLocal` 旋转全是单位阵，所以"绕某根世界轴转多少度"
// 可以直接当成关节的局部旋转用 —— 不需要反解欧拉角，也不会踩到
// "旋转顺序和导出器不一致"那一类经典事故。
//
// ------------------------------------------------------------
// 为什么是"关键帧"而不是运行时算
// ------------------------------------------------------------
// 运行时算（每帧按速度求角度）看着更省，但有三个问题：
//   · 动画系统是**采样式**的（`Skeleton::sample` 按时间插值轨道），
//     不走轨道就等于绕过了 A0~A4 整条链（跨圈、speed、混合全失效）；
//   · 想改一段动作就得改代码 + 重编，而"调步伐"是要反复试的；
//   · 生成成关键帧后，美术可以在编辑器里直接看/改那几条曲线。
// 所以这里只在**烘焙期**算一遍，产物是普通的 `SourceClip`。
//
// ------------------------------------------------------------
// 坐标约定（全部从骨架自己推，不假设 ±X / ±Y）
// ------------------------------------------------------------
//   up   = +Z（引擎约定）
//   fwd  = 水平化的 (LeftToeBase - LeftFoot)   ← 脚趾一定朝前
//   left = normalize(cross(up, fwd))           ← 解剖学左
//   swingAxis = normalize(cross(down, fwd))    ← 绕它正转 = 肢体向前摆
// 用脚趾方向求朝向而不是写死 -Y：Mixamo 的人物朝 -Y，Blender 导出的
// 常常朝 +Y，写死必翻。
// ============================================================

#include "assetio/SourceModel.h"

#include <string>
#include <vector>

namespace assetio {

struct ProcAnimOptions {
    float fps = 30.0f;        // 关键帧采样率
    bool generateIdle = true;
    bool generateWalk = true;
    bool generateRun = true;
    bool generateJump = true;
    // 已有同名 clip 时是否覆盖
    bool overwriteExisting = true;
};

struct ProcAnimReport {
    std::vector<std::string> clipNames;
    std::vector<float> durations;
    int totalTracks = 0;
    int totalKeys = 0;

    // ---- 自检（这是本模块唯一能证明"姿势真的动了、方向没反"的东西）----
    // walk 一个周期内：左趾尖相对 Hips 的**前向**位移范围、**竖直**抬升范围
    // 两者的下限由调用方断言（前向 < 3 cm = 没在走 / 方向反了；
    // 抬升 < 2 cm = 腿在地上拖）。符号约定：前向为正 = 朝 fwd。
    float walkFootForwardRange = 0.0f;
    float walkFootLift = 0.0f;
    // 脚"最靠前"发生在周期的哪一刻（0..1）。约定 p=0 = 左脚跟着地。
    float walkFootFwdMaxPhase = 0.0f;
    // 摆动相（脚离地那段）的平均前向速度（m/s，正 = 朝 fwd）。
    //
    // ⚠ 这才是抓"倒着走"的判据。只比"前向位移范围"抓不到：那是**对称量**，
    //   把腿部摆动整号翻转后范围一模一样、最大值照样出现在 p=0。
    //   但真人的摆动相脚一定是**向前飞**的（把脚从身后甩到身前），
    //   着地时相对髋向后走。方向反了这一个数就整号翻（月球漫步）。
    float walkSwingFwdVel = 0.0f;
    float runFootForwardRange = 0.0f;
    float runFootLift = 0.0f;
    float walkHandForwardRange = 0.0f;
    // 手相对 Hips 的**横向**最远距离（m）。
    // 摆臂是绕竖直轴前后转，横向应当基本不变（就是肩宽那点距离）；一旦
    // 明显变大，说明关节被绕错了轴 —— 表现是"跑步时手臂向两边张开像滑翔"，
    // 而从斜后上方看截图，这个错误和"手臂摆到身前"几乎分不出来。
    float walkHandLateral = 0.0f;
    float runHandForwardRange = 0.0f;
    float runHandLateral = 0.0f;
    // idle：整体位移应该很小（否则"站着不动也在飘"）
    float idleRootDrift = 0.0f;

    // jump：**空中相**脚底相对静止姿势抬高了多少米（P6，2026-10-22）。
    //
    // 用户要求"跳跃动画换成双脚抬起地面的那个"。这件事只能靠这个数断言：
    // 从背面截一张静帧，"腿收了"和"腿没动"看起来差不多（膝盖前后摆是
    // 沿视线方向的），而趾尖相对 Hips 的高度是确定的。
    // 参考：静止时趾尖在 Hips 下方约 0.89 m；抬到 0.45 以内（即抬高
    // 0.44 m）就是"明显收腿"。
    float jumpFootLift = 0.0f;
    float jumpFootPeakZ = 0.0f;
};

// 生成四段基础动画，**就地**追加到 model.clips。
// 骨架为空 / 找不到必需的关节名 → 返回 false 并写 error。
bool generateLocomotionClips(SourceModel& model, const ProcAnimOptions& opt,
                             ProcAnimReport* report, std::string* error);

} // namespace assetio
