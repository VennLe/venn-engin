#pragma once
// ============================================================
// ui2/Ui2Settings —— 游戏 UI / HUD 的全局设置（U0）
//
// 为什么单独一个模块、而不是塞进 render::PostSettings：
//   ui2 是**玩家看的 UI**，与 engine/ui/（编辑器的 ImGui）是两条互不
//   共享资源的通道（见 docs/ui_hud_roadmap.md §4 的对照表）。它的开关
//   必须能被 Game.exe 单独控制，而 PostSettings 是"后处理参数"，混进去
//   会让“关掉 HUD”变成“改后处理”。
//
// 存储仍是**普通成员**（不是影子结构体）：FeatureRegistry 的 bindParam
// 直接绑到这里，于是 env / 面板 / config 三条写入口写的是同一份内存
// （这是 E01 的硬约束，见 FeatureRegistry.h 顶部）。
// ============================================================

#include <cstdint>

namespace ui2 {

// 布局参考分辨率（16:9）。所有锚点/边距都以"这一档下的逻辑像素"描述，
// 换分辨率时按 min(W/refW, H/refH) 等比缩放 —— 这样 1280×720 与
// 3840×2160 上“血条离左下角 24px”看起来是同一个比例。
constexpr float kRefWidth  = 1920.0f;
constexpr float kRefHeight = 1080.0f;

struct Ui2Settings {
    // 总开关。默认 **true**，但"true 且没有 UI 数据" = 零绘制：
    // 不开任何 pass、不改任何像素（U0 的验收就是钉这一条）。
    bool enabled = true;

    // 编辑器视口里是否也画 HUD（预览用）。默认 **false**：
    // 打开会改变编辑器截图的像素 —— 几十条像素基线会整体漂。
    bool hudInEditor = false;

    // 逻辑像素 → 输出像素的额外倍率（用户在设置里放大 UI 用）。
    // 生效缩放 = min(W/refW, H/refH) * scale。
    float scale = 1.0f;

    // 把每个控件的最终矩形画成描边，并打 UI2-BOUNDS 锚点。
    // 自动化靠它读布局（仿 VP-RECT 的做法）。
    bool debugBounds = false;

    // 默认字号（逻辑像素）。文本控件不显式给字号时用它。
    float fontSize = 16.0f;
};

// 全局唯一实例。函数内静态：首次调用时按 NSDMI 初始化 ——
// 所以 bindFeatureParams() 里"绑定那一刻的值"就是默认值，
// resetToDefaults() 不需要把常量抄第二遍。
Ui2Settings& settings();

// 把 ui.* 五个参数绑到 settings() 上。幂等（只绑第一次）。
// 调用点：Renderer::bindFeatureParams()（成员默认初始化之后、
// loadEnvOverrides() 之前 —— 顺序错了默认值会变成 env 值）。
void bindFeatureParams();

// 由**输出**像素尺寸算 UI 缩放。
// ⚠ 除的是输出分辨率（交换链 / 视口），不是 DRS 之后的场景分辨率 ——
//   UI 不跟 DRS 缩放（否则 0.7x 渲染时血条会变小）。
float computeScale(uint32_t outW, uint32_t outH);

}  // namespace ui2
