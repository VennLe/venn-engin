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
constexpr ImVec4 kPlayCol(0.20f, 0.52f, 0.26f, 1.0f);
constexpr ImVec4 kPauseCol(0.62f, 0.50f, 0.16f, 1.0f);
constexpr ImVec4 kStopCol(0.48f, 0.22f, 0.22f, 1.0f);

} // namespace

// ---------------------------------------------------------------- 文件操作

void Toolbar::newScene() {
    m_ctx.stop();
    resetToEmptyScene(m_ctx.editorScene());
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
        if (ImGui::MenuItem("Settings...")) m_ctx.showSettings() = true;

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
            PrimitiveKind::Plane,     PrimitiveKind::PointLight,
            PrimitiveKind::SpotLight, PrimitiveKind::Empty};
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

// ---------------------------------------------------------------- 播放条

void Toolbar::drawPlayControls() {
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

    const bool playing = m_ctx.isPlaying();
    const bool paused = m_ctx.isPaused();

    // 把窗口内容宽度扣掉左右内边距，下面按它做居中 / 右对齐
    const float barW = ImGui::GetWindowWidth() - pad.x * 2.0f;
    const float rowY = ImGui::GetCursorPosY();

    // ================================================================
    // 左组：文件操作
    // ================================================================
    if (ImGui::Button("Save", ImVec2(btnW("Save"), btnH)))
        saveScene(m_ctx.scenePath());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Save scene (Ctrl+S)\n%s%s", m_ctx.scenePath().c_str(),
                          m_ctx.dirty() ? "  *unsaved" : "");
    }
    ImGui::SameLine();
    if (ImGui::Button("New", ImVec2(btnW("New"), btnH))) newScene();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Throw the scene away and start from an\n"
                          "empty scene (Ctrl+N).");
    }
    const float leftW = ImGui::GetItemRectMax().x - pad.x;

    // ================================================================
    // 中组：手柄模式 + 播放控制（整组水平居中）
    // ================================================================
    const GizmoMode mode = m_ctx.gizmoMode();
    const char* kModes[3] = {"Move", "Rotate", "Scale"};
    const GizmoMode kModeVals[3] = {GizmoMode::Translate, GizmoMode::Rotate,
                                    GizmoMode::Scale};

    // 先量宽度，再决定从哪里开始画
    float centerW = btnW("Play") + btnW("Pause") + btnW("Stop") +
                    btnW("Fullscreen");
    for (const char* m : kModes) centerW += btnW(m);
    centerW += st.ItemSpacing.x * 7.0f + 34.0f;   // 7 个间隔 + 两条分隔符

    const float rightW = btnW("Undo") + btnW("Redo") + st.ItemSpacing.x;
    const float centerX = std::max(leftW + 24.0f,
                                   (barW - centerW) * 0.5f);

    ImGui::SetCursorPos(ImVec2(pad.x + centerX, rowY));

    // ---- 手柄模式（和视口工具栏 / W E R 是同一份状态）----
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine();
        const bool on = (mode == kModeVals[i]);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              on ? ImVec4(0.216f, 0.400f, 0.706f, 1.0f)
                                 : ImVec4(0.200f, 0.211f, 0.231f, 1.0f));
        if (ImGui::Button(kModes[i], ImVec2(btnW(kModes[i]), btnH)))
            m_ctx.setGizmoMode(kModeVals[i]);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s gizmo  (%c)", kModes[i],
                              i == 0 ? 'W' : (i == 1 ? 'E' : 'R'));
        }
    }

    ImGui::SameLine();
    toolbarSeparator();
    ImGui::SameLine();

    // ---- Play / Pause / Stop：三个独立按钮（UE5 就是分开的）----
    ImGui::BeginDisabled(playing);
    ImGui::PushStyleColor(ImGuiCol_Button, kPlayCol);
    if (ImGui::Button("Play", ImVec2(btnW("Play"), btnH))) m_ctx.play();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Play in the viewport (game logic runs on a copy;\n"
                          "the editor scene is left untouched).  Space");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    ImGui::PushStyleColor(ImGuiCol_Button, kPauseCol);
    if (ImGui::Button("Pause", ImVec2(btnW("Pause"), btnH))) m_ctx.pause();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pause the running game.  Space");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(m_ctx.isEditing());
    ImGui::PushStyleColor(ImGuiCol_Button, kStopCol);
    if (ImGui::Button("Stop", ImVec2(btnW("Stop"), btnH))) m_ctx.stop();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Stop and throw the runtime copy away.\n"
                          "The editor scene is exactly as it was before Play.");
    }

    // 全屏（只在 Play 期间可用；视口右上角还有个同款按钮）
    ImGui::SameLine();
    ImGui::BeginDisabled(m_ctx.isEditing());
    if (ImGui::Button(m_ctx.gameFullscreen() ? "Exit Full" : "Fullscreen",
                      ImVec2(btnW("Fullscreen"), btnH))) {
        m_ctx.toggleGameFullscreen();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !m_ctx.isEditing()) {
        ImGui::SetTooltip("Window goes fullscreen and only the game is\n"
                          "drawn (F). Esc also leaves fullscreen.");
    }

    // ================================================================
    // 右组：撤销 / 重做（右对齐）
    // ================================================================
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
    drawPlayControls();
}

} // namespace editor
