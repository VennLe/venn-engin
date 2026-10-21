// ============================================================
// editor/ModeRail.cpp —— 左栏模式列表
//
// 行的画法与 Toolbar.cpp 的 `editModeRow` 逐像素对齐（圆点 + 图标 + 名字 +
// 右侧 TODO）。两处都从 EditModes 那套接口取名字/图标，唯一会漂的是排版，
// 所以常量也照抄：行高 28、圆点 x+11、图标 x+26、文字 x+50。
//
// 为什么不把 Toolbar 里那个函数抽出来共用：它写死在 Toolbar.cpp 的匿名
// 命名空间里、宽度是下拉菜单的 226px，而这里要跟着左栏宽度自适应。复制
// 18 行排版代码，比给一个"既当菜单项又当栏内行"的函数加一堆参数清楚。
// ============================================================

#include "ModeRail.h"

#include "DebugRects.h"
#include "EditModes.h"
#include "Localization.h"

#include "ui/ImGuiTheme.h"
#include "core/Logger.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace editor {

namespace {

constexpr float kRowH = 28.0f;
// 行间距。写成常量而不是读 `ImGui::GetStyle().ItemSpacing.y`：List 的高度
// 必须和这里**逐像素**对上（差 1px 就有一条缝或最后一行被切），读主题的话
// 字号一变两边就对不上。
constexpr float kRowGap = 3.0f;

} // namespace

float modeRailListHeight() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float rows = static_cast<float>(kEditModeCount) * (kRowH + kRowGap);
    // 表头：一行文字 + 一点间距（+ 下面那条线画在光标处，不额外占高度）
    const float header = ImGui::GetTextLineHeight() + st.ItemSpacing.y + 4.0f;
    return header + rows + st.WindowPadding.y * 2.0f;
}

Rect modeRailListRect(const LayoutRects& L) {
    const float h = std::min(modeRailListHeight(), L.modes.h);
    return {L.modes.x, L.modes.y, L.modes.w, h};
}

Rect modeRailBodyRect(const LayoutRects& L) {
    const Rect top = modeRailListRect(L);
    const float y = top.y + top.h;
    return {L.modes.x, y, L.modes.w, std::max(L.modes.h - top.h, 0.0f)};
}

bool beginModeRailPanel(EditorContext& ctx, ImGuiWindowFlags& flags) {
    const Rect body = modeRailBodyRect(ctx.layout());
    if (body.w < 80.0f || body.h < 80.0f) return false;
    ImGui::SetNextWindowPos(ImVec2(body.x, body.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(body.w, body.h), ImGuiCond_Always);
    // 面板自己设的 Min（很多是 400×420 起的）每帧都生效、不看 cond，会把
    // 窗口顶出左栏盖到视口上 —— 托管时必须清成无约束。
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
             ImGuiWindowFlags_NoSavedSettings;
    return true;
}

void drawModeRail(EditorContext& ctx) {
    const LayoutRects& L = ctx.layout();
    const Rect R = modeRailListRect(L);
    if (R.w < 8.0f || R.h < 8.0f) return;

    ImGui::SetNextWindowPos(ImVec2(R.x, R.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(R.w, R.h), ImGuiCond_Always);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    const bool open = ImGui::Begin("###mode_rail", nullptr, wf);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }

    logRect("MODE-RAIL-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // ---- 表头 ----
    {
        const ui::ThemeTokens& tk = ui::tokens();
        ImVec4 c = tk.textDim;
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextUnformatted(TR("Modes"));
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetWindowWidth() - 92.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, tk.textFaint);
        ImGui::TextUnformatted(TR("click to switch"));
        ImGui::PopStyleColor();
        // 表头下面一条细线：把"列表"和"参数区"划开（参数区是另一个窗口，
        // 那条线正好落在两个窗口的接缝上）
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(R.x, p0.y + 1.0f), ImVec2(R.x + R.w, p0.y + 1.0f),
                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
    }

    // ---- 9 行单选 ----
    // 行间距压到 3：默认 ItemSpacing.y（主题里是 7）会在 9 行上白吃 63px，
    // 而左栏下半的参数面板正缺这几十像素（Modeling 面板一屏放不下）。
    // 行高仍是 28，和顶栏下拉的 `editModeRow` 保持一致，只是挨得紧一点。
    const EditMode cur = ctx.editMode();
    const float rowW = std::max(R.w - 12.0f, 60.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, kRowGap));
    for (int i = 0; i < kEditModeCount; ++i) {
        const EditMode m = static_cast<EditMode>(i);
        const bool active = (m == cur);
        const bool impl = editModeImplemented(m);

        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::Selectable("##row", active,
                                               ImGuiSelectableFlags_None,
                                               ImVec2(rowW, kRowH));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cy = p.y + kRowH * 0.5f;
        const ui::ThemeTokens& tk = ui::tokens();

        // 单选圆点（与顶栏下拉同一套语义：圆点 = 单选，对勾 = 开关）
        if (active) {
            dl->AddCircleFilled(ImVec2(p.x + 11.0f, cy), 4.2f,
                                ImGui::GetColorU32(tk.accent), 12);
        } else {
            ImVec4 dc = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            if (!impl) dc.w *= 0.55f;
            dl->AddCircle(ImVec2(p.x + 11.0f, cy), 4.2f,
                          ImGui::GetColorU32(dc), 12, 1.2f);
        }

        // 图标 + 名字
        ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (!impl) tc.w *= 0.45f;
        else if (!active && !hovered) tc.w *= 0.88f;
        const ImU32 col = ImGui::GetColorU32(tc);
        paintEditModeIcon(dl, ImVec2(p.x + 26.0f, cy - 8.0f), 16.0f, m, col);
        dl->AddText(ImVec2(p.x + 50.0f, cy - ImGui::GetTextLineHeight() * 0.5f),
                    col, TR(editModeName(m)));

        if (!impl) {
            const char* tag = "TODO";
            const ImVec2 tw = ImGui::CalcTextSize(tag);
            ImVec4 dc = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            dc.w *= 0.8f;
            dl->AddText(ImVec2(p.x + rowW - tw.x - 6.0f, cy - tw.y * 0.5f),
                        ImGui::GetColorU32(dc), tag);
        }

        char rectTag[64];
        std::snprintf(rectTag, sizeof(rectTag), "MODE-RAIL-%d", i);
        logRect(rectTag, p, ImVec2(p.x + rowW, p.y + kRowH));
        // 按 id 的锚点：验收脚本"切到某个模式"现在走这里（顶栏那颗下拉
        // 2026-10-21 删掉了，之前用的是 TB-EDITMODE-ITEM <id>）。下标锚点
        // 留给"9 行是否齐全"这类布局断言，两者各管一头。
        char idTag[64];
        std::snprintf(idTag, sizeof(idTag), "MODE-RAIL-ITEM %s",
                      editModeId(m));
        logRect(idTag, p, ImVec2(p.x + rowW, p.y + kRowH));

        if (hovered) ImGui::SetTooltip("%s", TR(editModeHint(m)));
        if (clicked && !active) {
            ctx.setEditMode(m);
            VK_LOG_INFO("MODE-RAIL pick=%s", editModeId(m));
        }
    }
    ImGui::PopStyleVar();

    ImGui::End();

    // ---- 参数区：Select 模式下是空的，别留一大块纯底色 ----
    // Select 没有自己的面板（它就是"点一下选中 + W/E/R 变换"，参数都在
    // 右侧 Inspector 里）。这里给一张说明卡，把常用键写清楚 —— 否则左栏
    // 下半是一整块空灰，看起来像界面没加载完。
    if (ctx.editMode() == EditMode::Select) {
        const Rect body = modeRailBodyRect(L);
        if (body.w < 60.0f || body.h < 60.0f) return;
        ImGui::SetNextWindowPos(ImVec2(body.x, body.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(body.w, body.h), ImGuiCond_Always);
        const ImGuiWindowFlags wf2 = ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        const bool ok = ImGui::Begin("###mode_rail_hint", nullptr, wf2);
        ImGui::PopStyleVar();
        if (ok) {
            const ui::ThemeTokens& tk = ui::tokens();
            ImGui::PushStyleColor(ImGuiCol_Text, tk.textDim);
            ImGui::TextUnformatted(TR("Select"));
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, tk.textFaint);
            ImGui::TextWrapped(
                "%s",
                TR("Select mode has no parameters of its own: click to pick, "
                   "W/E/R for move/rotate/scale, Q for the selector, F to "
                   "focus the selection. Everything else lives in Inspector."));
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }
}

} // namespace editor
