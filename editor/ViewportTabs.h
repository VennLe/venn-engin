#pragma once
// ============================================================
// editor/ViewportTabs —— 视口上方的标签条（浏览器式）
//
// 2026-10-21 用户要求：
//   「引擎编辑器中所有的功能面板……在打开显示的时候，让窗口停靠在 Viewport
//     视口窗口上可否，就是类似于浏览器窗口中可以打开多个不同网站页面窗口的
//     那种，并且可以通过点击其对应的页面标签去切换。」
//
// ------------------------------------------------------------
// 谁会被托管成标签页
// ------------------------------------------------------------
// 判据只有一条：**不在分栏树里的面板**（`PanelDesc::layoutSlot == kSlotNone`）
// 一律变成视口标签页。于是 Physics / Particles / Quality / Console / Stats /
// Scripts / Engine / GPU Timeline / Character 全部自动跟上，加新面板时
// 不用回来改这里。
//
// 例外（有意为之）：
//   · 分栏树里的四个（Hierarchy / Viewport / Inspector / Modes）不是标签页；
//   · Content 是底栏抽屉（它要横跨整窗），保持原样；
//   · 八个**编辑模式**面板挂 `kSlotModes` —— 它们长在左栏（见 ModeRail.h），
//     不进视口。同一套参数在左栏和标签页各来一份只会让人困惑。
//
// ------------------------------------------------------------
// 状态
// ------------------------------------------------------------
// `m_active` 存的是**面板表下标**，-1 表示"当前看的是 3D 视口"。
// 用下标而不是 id 字符串：表是常量数组、下标稳定，而每帧要拿它去
// drawPanels 里比一次，字符串比较没必要。
// ============================================================

#include "EditorContext.h"

namespace editor {

class EditorApp;

// 标签条高度（像素）。视口矩形会从顶部扣掉这么多（见 EditorApp::computeLayout）。
constexpr float kViewportTabBarH = 30.0f;

// 当前标签页：面板表下标；-1 = 3D 视口
int activeViewportTab();
void setActiveViewportTab(int panelIndex);

// true = 现在显示的是 3D 视口（此时 ViewportPanel 才画）
bool viewportTabShowsViewport();

// 画标签条。bar = 视口矩形顶部那一条。
void drawViewportTabBar(const Rect& bar);

// 每帧在 drawPanels 之前调：把"面板开/关"的变化同步成标签页切换。
//   · 刚被打开的面板 → 自动成为当前标签页（用户点 View ▸ Physics，期待的是
//     "看见了"，不是"多了一颗按钮"）
//   · 当前标签页被关掉 → 回 3D 视口
//   · 关掉的面板从标签条上消失
void syncViewportTabs();

// 本帧要被绘制成标签页内容区的那个面板下标（-1 = 不托管任何面板）。
// 必须在 syncViewportTabs 之后、drawPanels 之前调。
int viewportTabDrawnPanel();

} // namespace editor
