#include "Toolbar.h"

#include "EditorScene.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace editor {

namespace {

// 播放控制按钮的配色（绿=播放、黄=暂停、红=停止）
// 播放条配色（Run / Play / Pause / Stop）现在由 ContentBrowser 底部的
// 播放行使用 —— 颜色常量跟着搬过去了，这里不再需要。

} // namespace

// ---------------------------------------------------------------- 文件操作

void Toolbar::newScene() {
    m_ctx.stop();
    resetToEmptyScene(m_ctx.editorScene(), m_ctx.assets());
    // onSceneReplaced() 里会把相机重设成编辑态模式（显式输入 + 自由飞行）
    m_ctx.onSceneReplaced();
    m_ctx.notify("New empty scene");
    VK_LOG_INFO("Editor: new empty scene");
}

void Toolbar::openScene(const std::string& relPath) {
    const scene::SceneIoResult r = scene::loadScene(
        m_ctx.editorScene(), m_ctx.assets(),
        assets::resolveAssetPath(relPath));
    if (r.ok) {
        // 相机是刚从 JSON 读回来的（轨道模式）—— onSceneReplaced() 会重设
        m_ctx.onSceneReplaced();
        m_ctx.scenePath() = relPath;
        m_ctx.notify("Opened " + relPath);
    } else {
        m_ctx.notify("Open failed: " + r.error);
    }
}

void Toolbar::saveScene(const std::string& relPath) {
    const std::string abs = assets::resolveAssetPath(relPath);
    const scene::SceneIoResult r = scene::saveScene(m_ctx.editorScene(), abs);
    if (r.ok) {
        m_ctx.scenePath() = relPath;
        m_ctx.dirty() = false;
        m_ctx.notify("Saved " + relPath + " (" + std::to_string(r.entities) +
                     " entities)");
    } else {
        m_ctx.notify("Save failed: " + r.error);
    }
}

// ---------------------------------------------------------------- 菜单

void Toolbar::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Scene", "Ctrl+N")) newScene();

        ImGui::Separator();
        if (ImGui::MenuItem("Open (current path)")) {
            openScene(m_ctx.scenePath());
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            saveScene(m_ctx.scenePath());
        }
        if (ImGui::MenuItem("Save As (assets/scenes/scene.json)")) {
            saveScene("assets/scenes/scene.json");
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Reload Scripts")) {
            m_ctx.notify("(scripts hot-reload automatically)");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) m_ctx.quitRequested() = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        ImGui::BeginDisabled(!m_ctx.commands().canUndo());
        std::string undoLabel = "Undo";
        if (m_ctx.commands().canUndo())
            undoLabel += std::string("  ") + m_ctx.commands().undoName();
        if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z")) m_ctx.undo();
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!m_ctx.commands().canRedo());
        std::string redoLabel = "Redo";
        if (m_ctx.commands().canRedo())
            redoLabel += std::string("  ") + m_ctx.commands().redoName();
        if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y")) m_ctx.redo();
        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::MenuItem("Clear History")) {
            m_ctx.commands().clear();
            m_ctx.notify("History cleared");
        }

        ImGui::Separator();
        ImGui::BeginDisabled(!m_ctx.hasSelection());
        if (ImGui::MenuItem("Deselect")) m_ctx.clearSelection();
        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::MenuItem("Preferences...", nullptr, false, true))
            m_ctx.showSettings() = true;

        // 历史列表（最近 12 条，倒序）
        const auto names = m_ctx.commands().historyNames();
        if (!names.empty()) {
            ImGui::SeparatorText("History");
            const size_t cursor = m_ctx.commands().cursor();
            const size_t begin = names.size() > 12 ? names.size() - 12 : 0;
            for (size_t i = names.size(); i-- > begin;) {
                const bool applied = i < cursor;
                ImGui::TextDisabled("%s %s", applied ? "[x]" : "[ ]",
                                    names[i].c_str());
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("GameObject")) {
        static const PrimitiveKind kinds[] = {
            PrimitiveKind::Cube,      PrimitiveKind::Sphere,
            PrimitiveKind::Cylinder,  PrimitiveKind::Plane,
            PrimitiveKind::PointLight, PrimitiveKind::SpotLight,
            PrimitiveKind::Empty};
        for (PrimitiveKind k : kinds) {
            if (ImGui::MenuItem(primitiveKindName(k))) {
                ecs::Entity created{};
                m_ctx.structuralEdit(
                    std::string("Create ") + primitiveKindName(k), [&]() {
                        created = createPrimitive(
                            m_ctx.editorScene(), m_ctx.assets(), k,
                            glm::vec3(0.0f));
                    });
                if (created.valid()) m_ctx.select(created);
            }
        }
        // 特殊灯光条目（语义不在 PrimitiveKind 里，见 EditorScene.h）
        ImGui::Separator();
        if (ImGui::MenuItem("Sunlight")) {
            ecs::Entity sun{};
            m_ctx.structuralEdit("Add Sunlight", [&]() {
                sun = createSunlight(m_ctx.editorScene());
            });
            if (sun.valid()) m_ctx.select(sun);
        }
        if (ImGui::MenuItem("Directional Light")) {
            ecs::Entity created{};
            m_ctx.structuralEdit("Add Directional Light", [&]() {
                created = createDirectionalLight(m_ctx.editorScene());
            });
            if (created.valid()) m_ctx.select(created);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        EditorContext::PanelVisibility& p = m_ctx.panels();
        ImGui::MenuItem("Hierarchy", nullptr, &p.hierarchy);
        ImGui::MenuItem("Inspector", nullptr, &p.inspector);
        ImGui::MenuItem("Content", nullptr, &p.content);
        ImGui::MenuItem("Scripts", nullptr, &p.scripts);
        ImGui::MenuItem("Stats", nullptr, &p.stats);
        ImGui::MenuItem("Engine (render settings)", nullptr, &p.engine);
        ImGui::Separator();
        ImGui::MenuItem("Viewport", nullptr, &p.viewport);
        // 地平面栅格（和视口右上角浮层里那个勾选框是同一份状态）
        ImGui::MenuItem("Ground grid (1 m)", nullptr, &m_ctx.showGridRef());
        if (ImGui::MenuItem("Reset Layout")) {
            m_ctx.relayout() = true;
            m_ctx.notify("Layout reset");
        }
        ImGui::Separator();
        // 窗口真全屏（面板照旧显示）。Play 态的 F 是"全屏 + 只看游戏"。
        if (ImGui::MenuItem("Toggle Fullscreen", "F11")) {
            m_ctx.windowFullscreenRequest() = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        ImGui::SeparatorText("Viewport navigation (edit mode)");
        ImGui::MenuItem("Hold RMB + drag: look around", nullptr, false, false);
        ImGui::MenuItem("Hold RMB + WASD: fly   Q / E: down / up", nullptr,
                        false, false);
        ImGui::MenuItem("Hold MMB + drag: pan   Shift: fly faster", nullptr,
                        false, false);
        ImGui::MenuItem("Wheel: dolly in / out", nullptr, false, false);
        ImGui::MenuItem("Hold RMB + wheel: navigation sensitivity (1.00)",
                        nullptr, false, false);
        ImGui::SeparatorText("Editing");
        ImGui::MenuItem("LMB: select object / drag gizmo handle", nullptr, false,
                        false);
        ImGui::MenuItem(
            "W / E / R: gizmo mode   X: world / local   (not while RMB)",
            nullptr, false, false);
        ImGui::MenuItem("LMB drag handle: move / rotate / scale", nullptr, false,
                        false);
        ImGui::MenuItem("G: snapping on/off for the current tool", nullptr,
                        false, false);
        ImGui::MenuItem(
            "Snap steps: move 0.1 m / rotate 10 deg / scale 0.1 (= UE5)",
            nullptr, false, false);
        ImGui::MenuItem("F: focus selection   Del: delete", nullptr, false,
                        false);
        ImGui::MenuItem("Shift+A (in viewport): add menu (Mesh / Light)",
                        nullptr, false, false);
        ImGui::MenuItem("F2: rename (Hierarchy)   Ctrl+D: duplicate", nullptr,
                        false, false);
        ImGui::MenuItem("Ctrl+Z / Ctrl+Y: undo / redo", nullptr, false, false);
        ImGui::MenuItem("Ctrl+S: save scene   Ctrl+N: new scene", nullptr,
                        false, false);
        ImGui::SeparatorText("Panels");
        ImGui::MenuItem("Right-click a Hierarchy node: rename / duplicate /\n"
                        "delete / create child / new folder",
                        nullptr, false, false);
        ImGui::MenuItem("Right-click a Content item: open / rename /\n"
                        "duplicate / delete / reveal in Explorer",
                        nullptr, false, false);
        ImGui::MenuItem("Double-click folders to enter, '..' or the arrow\n"
                        "button to go back up",
                        nullptr, false, false);
        ImGui::SeparatorText("Playing (game runs in the viewport)");
        ImGui::MenuItem("Space: play / pause", nullptr, false, false);
        ImGui::MenuItem("F: fullscreen, game only (Esc to leave)", nullptr,
                        false, false);
        ImGui::MenuItem("F11: window fullscreen (works in any mode)", nullptr,
                        false, false);
        ImGui::MenuItem("The game owns the camera - viewport takes no input",
                        nullptr, false, false);
        ImGui::EndMenu();
    }

    // 右侧显示当前场景路径与"未保存"标记
    const float rightWidth = 320.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - rightWidth);
    ImGui::TextColored(m_ctx.dirty() ? ImVec4(1.0f, 0.82f, 0.4f, 1.0f)
                                     : ImVec4(0.6f, 0.65f, 0.7f, 1.0f),
                       "%s%s", m_ctx.scenePath().c_str(),
                       m_ctx.dirty() ? " *" : "");

    ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------- 工具条
//
// 只剩右对齐的 Undo / Redo（Save/New/手柄模式/全屏/播放控制都已移走，
// 见 Toolbar.h 顶部的去向说明）。

void Toolbar::drawBar() {
    const LayoutRects& L = m_ctx.layout();
    // 全宽固定条：窗口缩放时跟随宽度
    ImGui::SetNextWindowPos(ImVec2(L.toolbar.x, L.toolbar.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.toolbar.w, L.toolbar.h),
                             ImGuiCond_Always);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus;

    // 与菜单栏同色 → 视觉上和菜单栏连成一条"顶栏"（UE5 就是这么做的）
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.113f, 0.117f, 0.125f, 1.0f));
    const ImVec2 pad(12.0f, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(9.0f, 7.0f));

    if (!ImGui::Begin("##toolbar", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    // 按钮统一高度（比文字框略高，整条看起来是"一整块"而不是一行小控件）
    const float btnH = 28.0f;
    // 按文字长度算按钮宽度（留一点内边距，标签不贴边）
    auto btnW = [&](const char* label) {
        return ImGui::CalcTextSize(label).x + st.FramePadding.x * 2.0f + 12.0f;
    };

    // 把窗口内容宽度扣掉左右内边距，下面按它做右对齐
    const float barW = ImGui::GetWindowWidth() - pad.x * 2.0f;
    const float rowY = ImGui::GetCursorPosY();

    // 左侧：当前手柄模式提示（轻量文字，不占按钮 —— 模式切换在视口工具条）
    const char* modeName = "Move";
    switch (m_ctx.gizmoMode()) {
        case GizmoMode::Translate: modeName = "Move"; break;
        case GizmoMode::Rotate:    modeName = "Rotate"; break;
        case GizmoMode::Scale:     modeName = "Scale"; break;
    }
    ImGui::SetCursorPos(ImVec2(pad.x, rowY + (btnH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextDisabled("%s   (W / E / R)", modeName);

    // ================================================================
    // 右组：撤销 / 重做（右对齐）
    // ================================================================
    const float rightW = btnW("Undo") + btnW("Redo") + st.ItemSpacing.x;
    ImGui::SetCursorPos(ImVec2(pad.x + barW - rightW, rowY));

    ImGui::BeginDisabled(!m_ctx.commands().canUndo());
    if (ImGui::Button("Undo", ImVec2(btnW("Undo"), btnH))) m_ctx.undo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && m_ctx.commands().canUndo())
        ImGui::SetTooltip("Undo: %s  (Ctrl+Z)", m_ctx.commands().undoName());

    ImGui::SameLine();
    ImGui::BeginDisabled(!m_ctx.commands().canRedo());
    if (ImGui::Button("Redo", ImVec2(btnW("Redo"), btnH))) m_ctx.redo();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && m_ctx.commands().canRedo())
        ImGui::SetTooltip("Redo: %s  (Ctrl+Y)", m_ctx.commands().redoName());

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// 工具栏里的一条竖分隔（比 TextUnformatted("|") 更像 UE5 的分组线）
void Toolbar::toolbarSeparator() {
    const float h = 22.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y0 = p.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 4.0f, y0),
                                        ImVec2(p.x + 4.0f, y0 + h),
                                        IM_COL32(84, 88, 96, 200), 1.0f);
    ImGui::Dummy(ImVec2(9.0f, 0.0f));
}

void Toolbar::draw() {
    drawMenuBar();
    drawBar();
}

} // namespace editor
