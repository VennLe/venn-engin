#pragma once
// ============================================================
// editor/CinematographyPanel —— 运镜面板（Cinematography）
//
// 一块面板把"摄影机"这件事的四层参数放在一张纸上，因为它们在别处**根本
// 凑不齐**：Inspector 一次只看一个组件、时间轴只认关键帧、而"这颗镜头多广
// + 走哪条轨迹 + 盯着谁 + 抖不抖"是一起调的。
//
//   ① 镜头（这台相机是什么）  —— 焦距 / 画幅 / 离轴 / 滚转 / 裁剪 / 景深 / 快门
//   ② rig（怎么被开着）        —— 注视目标 + 阻尼 / 手持 / 路径
//   ③ 动词（手动推一下）      —— Pan/Tilt/Roll/Dolly/Truck/Pedestal/Orbit/Crane
//   ④ 定稿（写回实体）        —— 把视口里的姿态烘成实体的 Transform
//
// 为什么 ③ 作用在**视口相机**而不是直接改组件：运镜是"看着画面调"的，
// 先在手边推到位、画面确认了再写回（Blender 的 nudge-then-key 同一套路）。
// 直接改组件的话每次微调都要先选中实体、改数字、再回去看，反馈环断了。
//
// ⚠ 面板**不持有**引擎对象：只拿 `EditorContext`，动引擎走它的接口
//   （与 PhysicsPanel / QualityPanel 同一条纪律）。
// ⚠ 面板改的是**编辑场景**的组件；Play 期间编辑场景仍在，但画面来自运行态，
//   所以 Play 时这些改动要等停下才看得到 —— 面板顶部有一条状态提示。
// ============================================================

#include "EditorContext.h"

#include "scene/CameraMoveKind.h"

#include <string>

namespace editor {

class CinematographyPanel {
public:
    explicit CinematographyPanel(EditorContext& ctx) : m_ctx(ctx) {}

    void draw();

private:
    // 选中实体上的相机组件（没有就 nullptr，面板会显示"先选一台相机"）。
    ecs::CameraComponent* cur();

    void drawCameraList();
    void drawLens(ecs::CameraComponent& cc);
    void drawRig(ecs::CameraComponent& cc);
    void drawPath(ecs::CameraComponent& cc);
    void drawVerbs();
    void drawBake();

    // 一行动作按钮：点一下就把 `kind` 施加到视口相机上。
    // `sign` 给 −1 时反向（"Pan −" 与 "Pan +" 共用同一段代码，面板上分两格）。
    void verbButton(scene::CameraMoveKind kind, const char* label,
                    const char* anchor, float sign = 1.0f);

    EditorContext& m_ctx;

    // 动词的幅度（角度类=度，距离类=米）。**分成两格**：把 15（度）当米用
    // 会一推把镜头甩出场景，而两个动词共用一个输入框这种事每天都在发生。
    float m_amountDeg = 15.0f;
    float m_amountM = 0.5f;

    // 新建路径时的默认速度（每秒钟走过的弧长比例）
    float m_newPathSpeed = 0.0f;

    // 状态行的签名：只有变化时才打日志（每帧打会淹掉真正有用的行）
    std::string m_lastSig;
};

} // namespace editor
