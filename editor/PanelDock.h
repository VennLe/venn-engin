#pragma once
// ============================================================
// editor/PanelDock.h —— "这个面板这一帧被当作视口标签页在画"
//
// ------------------------------------------------------------
// 为什么需要它
// ------------------------------------------------------------
// 本工程没有用 ImGui 的 docking 分支：所有面板都是**普通顶层窗口**，每帧
// 由 SplitLayout 算出矩形、面板自己 SetNextWindowPos/Size 钉上去。
// 要做到"浮动面板像浏览器标签页一样长在视口里"，缺的只有一件事 ——
// 在面板 Begin 之前把它的矩形钉到**标签页内容区**。
//
// 为什么用全局状态，而不是给每个面板塞一个 EditorContext&：
//   · 面板的构造签名各不相同（Physics/Particles 拿 ctx，Console /
//     GpuTimeline / CharacterWindow 完全不认识 ctx）；
//   · "当前哪个面板正被作为标签页派发"这件事**每帧只有一个值**，由
//     EditorApp::drawPanels 在调用某个面板的 draw 之前置位、之后清位。
//   把它做成参数要改 9 个类 + 9 个构造点；做成全局只有一处写、一处清。
//
// ------------------------------------------------------------
// 用法（每个可能被托管的浮动面板开头三行）
// ------------------------------------------------------------
//     ImGuiWindowFlags wf = 0;
//     const bool docked = editor::applyDockHint(wf);
//     if (!docked) {
//         ImGui::SetNextWindowSize(...);              // 原来的浮动默认值
//         ImGui::SetNextWindowPos(...);
//     }
//     if (!ImGui::Begin(name, docked ? nullptr : &open, wf)) { ... }
//
// ⚠ 必须写 `docked ? nullptr : &open`：传 &open 会在标题栏上画一个 ×，
//   而标签页的关闭按钮在标签条上 —— 两个 × 会让人以为"关一次不够"。
//
// ⚠ applyDockHint 命中时会把 flags 加上 NoTitleBar | NoMove | NoResize |
//   NoCollapse，并清掉尺寸约束（见函数里的注释）。NoTitleBar 是为了让
//   标签条成为**唯一**的标题 —— 否则一行标签 + 一行标题占掉 60px。
// ============================================================

#include <imgui.h>

namespace editor {

// 一个矩形（与 EditorContext::Rect 同构；这里不 include EditorContext.h，
// 免得每个面板都被拖进那 800 行）
struct DockRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// ---- 每帧唯一的"托管提示" ----
// w/h <= 1 视为没在被托管。写入方只有 EditorApp::drawPanels。
DockRect& dockHintRect();
bool& dockHintActive();

inline void pushDockHint(float x, float y, float w, float h) {
    DockRect& r = dockHintRect();
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    dockHintActive() = (w > 1.0f && h > 1.0f);
}

inline void clearDockHint() { dockHintActive() = false; }

// 面板开头调用。命中返回 true，并且**已经把窗口矩形钉好、flags 已补齐**。
inline bool applyDockHint(ImGuiWindowFlags& flags) {
    if (!dockHintActive()) return false;
    const DockRect& r = dockHintRect();
    ImGui::SetNextWindowPos(ImVec2(r.x, r.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(r.w, r.h), ImGuiCond_Always);
    // 尺寸约束每帧都生效（不看 cond），会跟上面的 Always 尺寸打架 ——
    // 面板自己设的 Min（比如 Particles 的 430×560）在窄标签页里会把窗口
    // 顶出内容区、盖住视口右边的分隔条。托管时直接清成无约束。
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    return true;
}

} // namespace editor
