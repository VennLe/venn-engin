#pragma once
// ============================================================
// editor/ModeRail —— 窗口左栏：编辑模式列表 + 当前模式的参数区
//
// 2026-10-21 用户要求：
//   「将模式类型选择列表……这些功能的的相关配置和参数信息放在编辑器窗口的
//     左侧，当选择不同的模式类型的时候就对应的切换就好。」
//
// ------------------------------------------------------------
// 这一栏里有两块东西，分别由两个模块画
// ------------------------------------------------------------
//   ┌─────────────┐  ← modeRailListRect()：本文件画（9 行单选，样式照
//   │ ○ Select    │     顶栏那颗下拉里的行：圆点 + 图标 + 名字）
//   │ ● Landscape │
//   │ …           │
//   ├─────────────┤  ← modeRailBodyRect()：**各模式自己的 drawPanel()** 画
//   │ 地形参数…   │     （ToolkitEditors.cpp 的 beginToolkitWindow 把窗口
//   └─────────────┘      钉进这块矩形，所以 8 个模式一行都不用改）
//
// 为什么参数区不在这里统一绘制：8 个模式面板各有几百行 UI，而且已经能
// 独立工作（原来是浮动窗口）。这里只负责"给它一块矩形"，是改动最小的接法。
//
// ⚠ 列表高度是**常量**（kRow * 9 + 表头），不随窗口高度走：模式就 9 个，
//   参数区该吃掉剩下的全部高度。做成比例的话窗口一矮，参数区会先被压没。
// ============================================================

#include "EditorContext.h"

namespace editor {

class EditorContext;

// 9 行模式列表的总高度（含表头）。
//
// ⚠ 必须是**函数**而不是常量：表头高度含 `ImGui::GetTextLineHeight()`，
//   随主题/字号变（大字号主题能到 8）。写死常量在字号放大后会让最后两行
//   掉出窗口外面（列表窗口是 NoScrollbar 的，直接看不见 —— 2026-10-21
//   第一版就是这样，9 行里只显示到第 5 行）。
//   行间距用 ModeRail.cpp 里的 kRowGap 常量，与绘制侧**同一份**，
//   不比读主题的 ItemSpacing（那边被 PushStyleVar 改过，读到的不是实际值）。
float modeRailListHeight();

// 列表窗口矩形（左栏上半）
Rect modeRailListRect(const LayoutRects& L);
// 当前模式的参数区（左栏下半）。ToolkitEditors 的 beginToolkitWindow 用它。
Rect modeRailBodyRect(const LayoutRects& L);

// 把"当前模式的参数面板"钉进左栏下半。
//
// 返回 true = 已经设好 SetNextWindowPos/Size（Always）、清掉尺寸约束，并把
// NoTitleBar|NoResize|NoMove|NoCollapse|NoSavedSettings 或进 flags —— 此时
// 调用方要写 `ImGui::Begin(win, nullptr, flags)`（不传 p_open：标题栏没了，
// × 也就没地方画；关面板由左栏那 9 行单选反向驱动）。
//
// 返回 false = 左栏被拖到折叠（宽度归零）。调用方应退回原来的浮动窗口，
// 否则模式面板会变成一块 0 宽的东西 —— 点也点不到、键盘也聚焦不上。
bool beginModeRailPanel(EditorContext& ctx, ImGuiWindowFlags& flags);

// 画列表。点某一行 = ctx.setEditMode(那一行)。只画列表，不画参数。
void drawModeRail(EditorContext& ctx);

} // namespace editor
