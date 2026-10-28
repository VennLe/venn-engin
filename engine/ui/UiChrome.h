#pragma once
// ============================================================
// engine/ui/UiChrome —— 二次元风格的"装饰件"（渐变 / 卡片 / 药丸按钮）
//
// 为什么要有这一层：
//   ImGui 的默认观感是"平涂 + 1px 直线 + 方角"，这是三次元工程软件的
//   视觉语言。用户 2026-10-27 的定位是「二次元风格的 3D 游戏引擎」，要求
//   「颜色和图标更像二次元动漫 3D 游戏引擎的风格，比如按钮颜色、按钮
//   形状、勾选框、字体」。
//
//   主题（ImGuiTheme）只能改**颜色**，改不了"形状"和"渐变"。所以把
//   "需要自己画"的那几件事集中在这里，让启动器和编辑器共用同一套：
//     · gradientRectV  —— 垂直渐变（标题带 / 卡片底）
//     · cardBackground —— 卡片：渐变底 + 软边框 + 可选外发光
//     · headerBand     —— 顶栏强调带（斜切渐变，动漫常见的"高光条"）
//     · pillButton     —— 药丸按钮（大圆角 + 渐变 + 悬停发亮）
//
// ⚠ 三条纪律：
//   1) **所有颜色从 ui::tokens() 取**，不许写死 —— 浅色主题下写死的深色
//      渐变会变成一块黑斑（这个坑在 barBg 上已经踩过一次）。
//   2) **不改布局**：pillButton 用 InvisibleButton 占位，尺寸完全由调用方
//      给，和 ImGui::Button 的语义一致（含 SameLine 行为）。这样"按钮换成
//      药丸形"不会让任何面板矩形漂移。
//   3) **不要给所有控件做自绘替代**：ImGui 的 Checkbox / Slider 内部还有
//      键盘导航、拖拽、tooltip 等行为，重画会丢。只有"按钮"这一类纯点击
//      控件值得自绘（本文件只提供它）。
// ============================================================

#include <imgui.h>

namespace ui {

// 垂直渐变矩形。`top` / `bot` 是 **ImU32**（已经过 GetColorU32），
// 因为这里要的是"能直接进 draw list 的颜色"，再转一次只是浪费。
//
// ⚠ ImGui 的 AddRectFilledMultiColor 不支持圆角（这是它的实现限制）。
//   所以 rounding > 0 时先画一个圆角矩形（用 bottom 色），再把渐变铺在
//   **上半部分**，最后用一个 1px 的圆角描边盖住接缝。视觉上等于"圆角
//   渐变"，代价是底部 r 像素内是纯色 —— 在 r<=12 时看不出来。
void gradientRectV(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot,
                   float rounding);

// 卡片底：渐变 + 软边框。hot = 悬停态（底色往强调色偏）。
// `glow` > 0 时在最外圈画一层强调色光晕（0..1，一般传 0.35）。
void cardBackground(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                    bool hot = false, float glow = 0.0f);

// 顶栏强调带：**左深右亮**的水平渐变 + 一条更亮的底边。
// 启动器 header / 编辑器状态栏用它，做出"高光条"的感觉。
void headerBand(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding);

// 药丸按钮。返回本帧是否被点击（语义同 ImGui::Button）。
//
// id      —— 唯一 ID（`"##play"` 这种也可以，会走 PushID）
// label   —— 显示文字（已经翻译好的）
// size    —— 尺寸；传 (0, h) 时宽度按文字自适应 + 两侧 padding
// kind    0 = 普通（panel 色）1 = 主按钮（强调色）2 = 危险（错误色）
//
// ⚠ 用 InvisibleButton 而不是 Button：这样 ImGui 仍然负责命中、Hover、
//   键盘导航的 **Item 注册**，我们只负责画。手搓命中测试会丢掉
//   "按住后拖出按钮再松开不算点击"这类细节。
bool pillButton(const char* id, const char* label, ImVec2 size, int kind = 0);

// 一行小的"状态药丸"（圆角标签 + 背景色）。不接收点击，纯展示。
// 用于把 "Running" / "Dirty" 这类状态词画成彩色小胶囊。
void statusPill(const char* text, const ImVec4& color);

} // namespace ui
