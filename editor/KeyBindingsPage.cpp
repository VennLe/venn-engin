#include "KeyBindingsPage.h"

#include "DebugRects.h"
#include "EditorContext.h"
#include "InputMapIO.h"
#include "Localization.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace editor {
namespace {

// 捕获用的边沿状态。**文件级 static**（不是 EditorContext 的成员）：
// "上一帧这个键按着没有"是纯 UI 细节，跟工程数据无关；放进去只会让
// 场景序列化那边多一个永远不该落盘的字段。
std::array<bool, GLFW_KEY_LAST + 1> g_prevKeys{};

constexpr ImVec4 kWarnColor{1.00f, 0.72f, 0.30f, 1.0f};
constexpr ImVec4 kBadColor{0.98f, 0.42f, 0.36f, 1.0f};

bool modifierKey(int k) {
    return k == GLFW_KEY_LEFT_SHIFT || k == GLFW_KEY_RIGHT_SHIFT ||
           k == GLFW_KEY_LEFT_CONTROL || k == GLFW_KEY_RIGHT_CONTROL ||
           k == GLFW_KEY_LEFT_ALT || k == GLFW_KEY_RIGHT_ALT ||
           k == GLFW_KEY_LEFT_SUPER || k == GLFW_KEY_RIGHT_SUPER;
}

bool anyDown(GLFWwindow* w, int a, int b) {
    return glfwGetKey(w, a) == GLFW_PRESS || glfwGetKey(w, b) == GLFW_PRESS;
}

// 某个动作当前**跟谁撞键**（同一上下文、同一 source+code）。
//
// ⚠ 只在同一个上下文里比：Gameplay 的 Space（跳）与 Menu 的 Space（确认）
//   是刻意共存的 —— 压栈后只有栈顶生效。跨上下文报冲突等于永远报冲突。
std::vector<std::string> conflictsOf(const core::InputMap& map,
                                     const std::string& ctxName,
                                     const std::string& action) {
    std::vector<std::string> out;
    const core::InputContext* c = map.context(ctxName);
    if (!c) return out;
    const auto it = c->actions.find(action);
    if (it == c->actions.end()) return out;
    for (const core::InputBinding& b : it->second) {
        for (const std::string& other :
             map.conflictingActions(ctxName, b, action)) {
            if (std::find(out.begin(), out.end(), other) == out.end())
                out.push_back(other);
        }
    }
    return out;
}

// 把捕获到的键真正绑上去。
//
// ⚠ 日志（INS-BIND-SET / INS-BIND-CLASH）**不在**这里 —— 它们在
//   EditorContext::rebindAction 里，因为自动化的 `MYVK_EDITOR_REBIND`
//   注入走的是同一个函数。日志跟着**写路径**走，界面能做的就等于脚本能测的。
void applyCapture(EditorContext& ctx, const core::InputBinding& b) {
    const std::string action = ctx.captureAction();
    const std::string key = ctx.inputMap().describeBindingOne(b);
    const std::vector<std::string> clash =
        ctx.inputMap().conflictingActions(ctx.captureContext(), b, action);

    const bool changed = ctx.rebindAction(ctx.captureContext(), action, b);

    if (!clash.empty()) {
        std::string names;
        for (std::size_t i = 0; i < clash.size(); ++i) {
            if (i) names += ", ";
            names += clash[i];
        }
        ctx.notify("Bound " + action + " to " + key + "  (also " + names + ")");
    } else if (changed) {
        ctx.notify("Bound " + action + " to " + key);
    }
    ctx.endInputCapture();
}

} // namespace

void drawKeyBindingsPage(EditorContext& ctx, GLFWwindow* window) {
    core::InputMap& map = ctx.inputMap();

    // 先把"上一帧的按键状态"整体刷新一遍。
    //
    // ⚠ 为什么不是"捕获开始时刷新"：这一页没画的时候 `g_prevKeys` 是停住的，
    //   隔十几秒再回到这页，表里的值已经陈旧 —— 期间一直按着的键会被当成
    //   "刚按下"，悄悄改掉绑定。刷新成本是 300 来次 glfwGetKey，一帧一次
    //   完全无所谓。
    if (window) {
        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k)
            g_prevKeys[static_cast<std::size_t>(k)] =
                glfwGetKey(window, k) == GLFW_PRESS;
    }

    // ---------------------------------------------------------------- 来源
    // 顶部把"这张表从哪来"说清楚：出厂 / 文件 / 哪条文件。改了没存要显眼。
    //
    // ⚠ 路径**必须 `TextWrapped` 单独一行**：资产根的绝对路径动辄六七十字符，
    //   跟标签挤一行会从右边缘裁掉（实测只显示到 `...\build\input_ma`）——
    //   "文件到底写到哪"是这一页最该说清的一件事，裁掉就等于没说。
    {
        const std::string path = editorInputMapPath();
        const bool hasFile = editorInputMapFileExists();
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::TextDisabled(TR("Bindings file"));
        if (ctx.inputMapDirty()) {
            ImGui::SameLine();
            ImGui::TextColored(kBadColor, TR("* unsaved"));
        }
        ImGui::TextWrapped("%s", path.c_str());
        if (!hasFile)
            ImGui::TextColored(kWarnColor, TR("(none yet - factory bindings)"));
        const ImVec2 max = ImGui::GetCursorScreenPos();
        logRect("INS-BIND-SOURCE", min,
                ImVec2(std::max(max.x, min.x + 200.0f), max.y));
    }

    if (ImGui::Button(TR("Save to disk"))) ctx.saveInputMap();
    ImGui::SameLine();
    if (ImGui::Button(TR("Reload from disk"))) {
        ctx.loadInputMap();
        ctx.notify(TR("Key bindings reloaded (unsaved edits dropped)"));
    }
    ImGui::SameLine();
    {
        static bool confirm = false;
        if (!confirm) {
            if (ImGui::Button(TR("Reset all to factory"))) confirm = true;
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.65f, 0.22f, 0.18f, 1.0f));
            if (ImGui::Button(TR("Confirm reset all?"))) {
                ctx.resetAllBindings();
                confirm = false;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton(TR("cancel"))) confirm = false;
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.66f, 0.72f, 1.0f));
    ImGui::TextUnformatted(TR("Game bindings"));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s",
                          TR("These bindings belong to the game (Game.exe). "
                             "Editor shortcuts are not rebindable here."));
    }

    // ---------------------------------------------------------------- 捕获
    // 捕获是**模态**的：这一段画出来之后到下一次按键之间，表不画、全局
    // 快捷键闭嘴（见 EditorApp::handleShortcuts 的第一行）。
    if (ctx.inputCaptureActive()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kWarnColor);
        ImGui::Text(TR("Press a key for '%s'   (Esc cancels)"),
                    ctx.captureAction().c_str());
        ImGui::PopStyleColor();
        logRect("INS-BIND-CAPTURE", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::TextDisabled(TR("Keys currently held become modifiers "
                               "(Shift / Ctrl / Alt)."));

        if (window) {
            for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k) {
                const bool down = glfwGetKey(window, k) == GLFW_PRESS;
                const bool edge = down && !g_prevKeys[static_cast<std::size_t>(k)];
                g_prevKeys[static_cast<std::size_t>(k)] = down;
                if (!edge) continue;

                if (k == GLFW_KEY_ESCAPE) {
                    VK_LOG_INFO("INS-BIND-CANCEL action=%s",
                                ctx.captureAction().c_str());
                    ctx.endInputCapture();
                    return;
                }
                // 修饰键**单独**按下不作为绑定目标（它只用来配组合键）。
                if (modifierKey(k)) continue;

                core::InputBinding b;
                b.source = core::InputSource::Key;
                b.code = k;
                b.shift = anyDown(window, GLFW_KEY_LEFT_SHIFT, GLFW_KEY_RIGHT_SHIFT);
                b.ctrl = anyDown(window, GLFW_KEY_LEFT_CONTROL,
                                 GLFW_KEY_RIGHT_CONTROL);
                b.alt = anyDown(window, GLFW_KEY_LEFT_ALT, GLFW_KEY_RIGHT_ALT);
                applyCapture(ctx, b);
                return;
            }
        }
        return;
    }

    // ---------------------------------------------------------------- 表
    const std::vector<std::string> ctxNames = map.contextNames();
    if (ctxNames.empty()) {
        ImGui::TextDisabled(TR("(no input context in the map)"));
        return;
    }

    // ⚠ 用**名字**记住当前上下文，不存下标：map 的上下文集合可能变
    //   （装载了别的绑定表 / 以后加了新上下文），下标会指到别人身上。
    static std::string currentCtx = "Gameplay";
    int ctxPick = 0;
    for (std::size_t i = 0; i < ctxNames.size(); ++i)
        if (ctxNames[i] == currentCtx) ctxPick = static_cast<int>(i);

    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("##bindctx", currentCtx.c_str())) {
        for (std::size_t i = 0; i < ctxNames.size(); ++i) {
            const bool sel = static_cast<int>(i) == ctxPick;
            if (ImGui::Selectable(ctxNames[i].c_str(), sel)) {
                currentCtx = ctxNames[i];
                VK_LOG_INFO("INS-BIND-CTX context=%s", currentCtx.c_str());
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (map.context(currentCtx) == nullptr) currentCtx = ctxNames.front();
    ImGui::SameLine();
    ImGui::TextDisabled(TR("%zu actions"),
                        map.actionNames(currentCtx).size());

    const std::vector<std::string> actions = map.actionNames(currentCtx);
    std::string conflictSummary;   // 逐帧汇总，变化时才打日志

    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerH |
                               ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##bindtable", 3, tf)) {
        ImGui::TableSetupColumn(TR("Action"), ImGuiTableColumnFlags_WidthFixed,
                                150.0f);
        ImGui::TableSetupColumn(TR("Binding"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(TR(""), ImGuiTableColumnFlags_WidthFixed, 210.0f);
        ImGui::TableHeadersRow();

        for (const std::string& action : actions) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(action.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            const std::string desc = map.describeBinding(currentCtx, action);
            const std::vector<std::string> clash =
                conflictsOf(map, currentCtx, action);
            if (clash.empty()) {
                if (desc == "(unbound)")
                    ImGui::TextColored(kWarnColor, "%s", desc.c_str());
                else
                    ImGui::TextUnformatted(desc.c_str());
            } else {
                std::string names;
                for (std::size_t i = 0; i < clash.size(); ++i) {
                    if (i) names += ", ";
                    names += clash[i];
                }
                ImGui::TextColored(kBadColor, "%s", desc.c_str());
                ImGui::SameLine();
                ImGui::TextColored(kBadColor, TR("(! also %s)"), names.c_str());
                if (!conflictSummary.empty()) conflictSummary += "; ";
                conflictSummary += currentCtx + "/" + action + "=" + names;
            }
            logRect(("INS-BIND-ROW-" + action).c_str(),
                    ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            ImGui::TableSetColumnIndex(2);
            const std::string id = "##" + action;
            if (ImGui::SmallButton((TR("Rebind") + id).c_str()))
                ctx.beginInputCapture(currentCtx, action);
            ImGui::SameLine();
            if (ImGui::SmallButton((TR("Reset") + id).c_str()))
                ctx.resetActionBinding(currentCtx, action);
            ImGui::SameLine();
            if (ImGui::SmallButton((TR("Clear") + id).c_str()))
                ctx.clearActionBinding(currentCtx, action);
        }
        ImGui::EndTable();
        logRect("INS-BIND-BODY", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
    }

    // 冲突汇总只在**变化时**打一行。测试据此断言"出厂表零冲突 / 改完有冲突"，
    // 逐帧刷屏的日志没法这么断言。
    {
        static std::string last;
        const std::string now = conflictSummary.empty()
                                    ? std::string("none")
                                    : conflictSummary;
        if (now != last) {
            last = now;
            VK_LOG_INFO("INS-BIND-CONFLICT %s", now.c_str());
        }
    }
}

} // namespace editor
