// ============================================================
// editor/ViewportTabs.cpp —— 视口标签条
//
// 画法：完全手绘（InvisibleButton + DrawList）。用常规 Button 画不出
// "浏览器标签"那种"上圆下平、激活页与内容区连成一片"的形状，而这几行
// 绘制代码比拼一堆 PushStyleColor 更短、更好读。
//
// 关闭按钮的位置与命中分开算：整块标签是一个 InvisibleButton，× 只是
// 其中右侧的一小块矩形。**先判 ×、再判标签**，否则点 × 会连带切换标签页
// （两个动作叠在一起，看起来就像"点 × 反而跳到了这一页"）。
// ============================================================

#include "ViewportTabs.h"

#include "DebugRects.h"
#include "Localization.h"
#include "PanelRegistry.h"

#include "core/Logger.h"
#include "ui/ImGuiTheme.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace editor {

namespace {

int& activeRef() {
    static int active = -1;   // -1 = 3D 视口
    return active;
}

// 上一帧每个面板的 open 状态（用来识别"刚被打开"）
bool* seenOpen() {
    static bool seen[kMaxPanels] = {false};
    return seen;
}

bool isTabHosted(int index) {
    const PanelDesc* t = panelTable();
    if (!t || index < 0 || index >= panelCount()) return false;
    return t[index].layoutSlot == kSlotNone;
}

const char* tabLabel(int index) {
    if (index < 0) return TR("Viewport");
    return TR(panelTitle(index));
}

// 标签条上**应当出现**的标签（viewport 恒在第一，其余按面板表顺序）
int collectTabs(int* out, int cap) {
    int n = 0;
    const int vp = panelIndexOf("viewport");
    if (vp >= 0 && panelOpen(vp) && n < cap) out[n++] = -1;   // 3D 视口恒在首位
    const PanelDesc* t = panelTable();
    const int cnt = panelCount();
    for (int i = 0; i < cnt && n < cap; ++i) {
        if (!isTabHosted(i)) continue;
        if (!panelOpen(i)) continue;
        if (t[i].layoutSlot == kSlotContent) continue;
        out[n++] = i;
    }
    return n;
}

} // namespace

int activeViewportTab() { return activeRef(); }

void setActiveViewportTab(int panelIndex) {
    if (panelIndex >= 0 && !isTabHosted(panelIndex)) return;
    if (activeRef() == panelIndex) return;
    activeRef() = panelIndex;
    VK_LOG_INFO("VP-TAB active=%s",
                panelIndex < 0 ? "viewport" : panelId(panelIndex));
}

bool viewportTabShowsViewport() { return activeRef() < 0; }

void syncViewportTabs() {
    bool* seen = seenOpen();
    const int cnt = panelCount();
    for (int i = 0; i < cnt && i < kMaxPanels; ++i) {
        const bool now = panelOpen(i);
        if (now && !seen[i] && isTabHosted(i)) {
            // 刚打开 → 直接切过去
            activeRef() = i;
            VK_LOG_INFO("VP-TAB open=%s active=1", panelId(i));
        }
        if (!now && seen[i] && activeRef() == i) {
            // 当前页被关掉 → 回视口
            activeRef() = -1;
            VK_LOG_INFO("VP-TAB close=%s active=viewport", panelId(i));
        }
        seen[i] = now;
    }

    // 当前页虽然还开着，但已经被"收进托盘"了 → 同样回视口，否则标签条上
    // 没有它、内容区却还在画它。
    const int a = activeRef();
    if (a >= 0 && (!panelOpen(a) || panelTrayHiddenRef(a))) {
        activeRef() = -1;
        VK_LOG_INFO("VP-TAB hide=%s active=viewport", panelId(a));
    }
}

int viewportTabDrawnPanel() {
    const int a = activeRef();
    if (a < 0) return -1;
    if (!isTabHosted(a) || !panelOpen(a) || panelTrayHiddenRef(a)) return -1;
    return a;
}

void drawViewportTabBar(const Rect& bar) {
    if (bar.w < 8.0f || bar.h < 8.0f) return;

    ImGui::SetNextWindowPos(ImVec2(bar.x, bar.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(bar.w, bar.h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const ImGuiWindowFlags wf =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##vp_tabs", nullptr, wf);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::ThemeTokens& tk = ui::tokens();

    // 整条底边（内容区顶边）—— 让标签条与内容区在视觉上连成一片
    dl->AddRectFilled(ImVec2(bar.x, bar.y), ImVec2(bar.x + bar.w, bar.y + bar.h),
                      ImGui::GetColorU32(ImGuiCol_WindowBg));

    int tabs[kMaxPanels + 1];
    const int n = collectTabs(tabs, kMaxPanels + 1);

    const float h = bar.h - 2.0f;
    float x = bar.x + 6.0f;
    const float y = bar.y + 2.0f;

    for (int i = 0; i < n; ++i) {
        const int idx = tabs[i];
        const char* label = tabLabel(idx);
        const bool active = (idx == activeRef());
        const bool closable = (idx >= 0);

        const ImVec2 ts = ImGui::CalcTextSize(label);
        const float w = ts.x + (closable ? 46.0f : 30.0f);
        if (x + w > bar.x + bar.w - 4.0f) break;   // 放不下就不再画（窄窗口）

        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        const bool pressed = ImGui::InvisibleButton("##tab", ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        // ---- × 的命中区（先于标签处理）----
        const float closeCx = x + w - 14.0f;
        const float closeCy = y + h * 0.5f;
        bool closed = false;
        if (closable) {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool overClose = std::fabs(mp.x - closeCx) <= 8.0f &&
                                   std::fabs(mp.y - closeCy) <= 8.0f;
            if (overClose && ImGui::IsMouseClicked(0)) {
                setPanelOpen(idx, false);
                closed = true;
                VK_LOG_INFO("VP-TAB xclose=%s", panelId(idx));
            }
        }

        // ---- 底：激活页比内容区亮一档，未激活压暗 ----
        ImU32 bg = active ? ImGui::GetColorU32(ImGuiCol_WindowBg)
                          : ImGui::GetColorU32(ImGuiCol_TitleBg, 0.92f);
        if (!active && hovered) bg = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, 4.0f,
                          ImDrawFlags_RoundCornersTop);
        if (active) {
            // 激活页顶部一条强调色 —— 标签条上看"我在哪一页"最快的信号
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 2.0f),
                              ImGui::GetColorU32(tk.accent), 2.0f);
        }

        // ---- 文字 ----
        ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (!active) tc.w *= 0.72f;
        dl->AddText(ImVec2(x + 12.0f, y + (h - ts.y) * 0.5f),
                    ImGui::GetColorU32(tc), label);

        // ---- × ----
        if (closable) {
            const ImU32 cc = ImGui::GetColorU32(
                hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            const float s = 4.0f;
            dl->AddLine(ImVec2(closeCx - s, closeCy - s),
                        ImVec2(closeCx + s, closeCy + s), cc, 1.4f);
            dl->AddLine(ImVec2(closeCx - s, closeCy + s),
                        ImVec2(closeCx + s, closeCy - s), cc, 1.4f);
        }

        if (pressed && !closed) setActiveViewportTab(idx);

        char tag[64];
        std::snprintf(tag, sizeof(tag), "VTAB-%s", idx < 0 ? "viewport"
                                                          : panelId(idx));
        logRect(tag, ImVec2(x, y), ImVec2(x + w, y + h));

        x += w + 3.0f;
    }

    ImGui::End();
}

} // namespace editor
