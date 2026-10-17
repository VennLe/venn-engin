#include "Toolbar.h"

#include "Localization.h"
#include "DebugRects.h"    // logRect：MYVK_LOG_RECTS 下把顶栏矩形打给脚本
#include "EditorScene.h"
#include "PanelRegistry.h"
#include "Workspace.h"
#include "script/ScriptEngine.h"

#include "ui/ImGuiTheme.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace editor {

namespace {

// 播放条配色（Run / Play / Pause / Stop）现在由 ContentBrowser 底部的
// 播放行使用 —— 颜色常量跟着搬过去了，这里不再需要。

// 视图模式枚举的合法范围（菜单生成用）
constexpr int kViewModeCount = 8;   // Renderer::ViewMode 的条目数

const char* groupName(editor::PanelGroup g) {
    switch (g) {
        case PanelGroup::General: return "General";
        case PanelGroup::Debug: return "Debug";
        case PanelGroup::Authoring: return "Authoring";
    }
    return "General";
}

constexpr PanelGroup kGroups[3] = {PanelGroup::General, PanelGroup::Debug,
                                   PanelGroup::Authoring};

} // namespace

// ---------------------------------------------------------------- 文件操作

void Toolbar::newScene() {
    m_ctx.stop();
    resetToEmptyScene(m_ctx.editorScene(), m_ctx.assets());
    // onSceneReplaced() 里会把相机重设成编辑态模式（显式输入 + 自由飞行）
    m_ctx.onSceneReplaced();
    m_ctx.notify(TR("New empty scene"));
    VK_LOG_INFO("Editor: new empty scene");
}

void Toolbar::openScene(const std::string& relPath) {
    const scene::SceneIoResult r = scene::loadScene(
        m_ctx.editorScene(), m_ctx.assets(), assets::resolveAssetPath(relPath));
    if (r.ok) {
        // 相机是刚从 JSON 读回来的（轨道模式）—— onSceneReplaced() 会重设
        m_ctx.onSceneReplaced();
        m_ctx.scenePath() = relPath;
        m_ctx.pushRecentScene(relPath);
        m_ctx.notify(TR("Opened ") + relPath);
        VK_LOG_INFO("Scene opened: '%s' (%zu entities)", relPath.c_str(),
                    m_ctx.editorScene().objectCount());
    } else {
        m_ctx.notify(TR("Open failed: ") + r.error);
        VK_LOG_WARN("Scene open failed for '%s': %s", relPath.c_str(),
                    r.error.c_str());
    }
}

void Toolbar::saveSceneAs(const std::string& relPath) {
    const std::string abs = assets::resolveAssetPath(relPath);
    const scene::SceneIoResult r = scene::saveScene(m_ctx.editorScene(), abs);
    if (r.ok) {
        m_ctx.scenePath() = relPath;
        m_ctx.dirty() = false;
        m_ctx.pushRecentScene(relPath);
        m_ctx.notify(std::string(TR("Saved ")) + relPath + " (" +
                     std::to_string(r.entities) + TR(" entities)"));
        VK_LOG_INFO("Scene saved: '%s'", abs.c_str());
    } else {
        m_ctx.notify(TR("Save failed: ") + r.error);
        VK_LOG_WARN("Scene save failed for '%s': %s", abs.c_str(),
                    r.error.c_str());
    }
}

void Toolbar::saveSceneToCurrentPath() { saveSceneAs(m_ctx.scenePath()); }

// ---------------------------------------------------------------- 菜单子项

void Toolbar::drawPanelMenu() {
    const PanelDesc* t = panelTable();
    const int n = panelCount();

    for (PanelGroup g : kGroups) {
        bool any = false;
        for (int i = 0; i < n; ++i) {
            if (t[i].group == g) {
                any = true;
                break;
            }
        }
        if (!any) continue;

        if (!ImGui::BeginMenu(groupName(g))) continue;
        for (int i = 0; i < n; ++i) {
            if (t[i].group != g) continue;
            bool open = panelOpen(i);
            const char* sc = (t[i].shortcut && t[i].shortcut[0]) ? t[i].shortcut
                                                                 : nullptr;
            if (ImGui::MenuItem(TR(t[i].title), sc, &open)) {
                setPanelOpen(i, open);
                m_ctx.notify(std::string(t[i].title) +
                             (open ? " shown" : " hidden"));
            }
        }
        ImGui::EndMenu();
    }
}

void Toolbar::drawWorkspaceMenu() {
    const int cur = m_ctx.workspace().current();
    for (int i = 0; i < kWorkspaceCount; ++i) {
        char label[192];
        std::snprintf(label, sizeof(label), "%s##ws_%s", TR(kWorkspaces[i].title),
                      kWorkspaces[i].id);
        const bool selected = (i == cur);
        if (ImGui::MenuItem(label, nullptr, selected)) {
            m_host.applyWorkspace(i);
            m_ctx.notify(std::string(TR("Workspace: ")) + TR(kWorkspaces[i].title));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(TR(kWorkspaces[i].hint));
    }
    ImGui::Separator();
    char saveLabel[96];
    std::snprintf(saveLabel, sizeof(saveLabel), "Save Workspace%s",
                  m_ctx.workspaceModified() ? "  *" : "");
    if (ImGui::MenuItem(saveLabel)) m_host.saveWorkspace();
    if (ImGui::MenuItem(TR("Reset Workspace"))) m_host.resetWorkspace();
    ImGui::Separator();
    ImGui::MenuItem(TR("Ctrl+Alt+1..6 switches workspaces"), nullptr, false, false);
    if (m_ctx.workspaceModified()) {
        ImGui::TextDisabled(TR("current layout differs from the preset (*)"));
    }
}

void Toolbar::drawViewModeMenu() {
    using VM = render::Renderer::ViewMode;
    const int cur = m_host.currentViewMode();
    for (int m = 0; m < kViewModeCount; ++m) {
        const char* name = render::Renderer::viewModeName(static_cast<VM>(m));
        char label[64];
        std::snprintf(label, sizeof(label), "%s", name);
        if (ImGui::MenuItem(label, nullptr, m == cur)) m_host.applyViewMode(m);
    }
}

// ---------------------------------------------------------------- 菜单栏

void Toolbar::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    // ================================================================
    // File
    // ================================================================
    if (ImGui::BeginMenu(TR("File"))) {
        if (ImGui::MenuItem(TR("New Scene"), "Ctrl+N")) newScene();

        ImGui::Separator();
        if (ImGui::MenuItem(TR("Open (current path)"), "Ctrl+O")) {
            openScene(m_ctx.scenePath());
        }

        // ---- Open Recent ▸（最近 10 个，最新在前）----
        if (ImGui::BeginMenu(TR("Open Recent"), !m_ctx.recentScenes().empty())) {
            // 拷贝一份再遍历：菜单项点下去会改 recent（pushRecentScene），
            // 直接在原容器上迭代就是"边遍历边改"。
            const std::vector<std::string> items = m_ctx.recentScenes();
            for (size_t i = 0; i < items.size(); ++i) {
                char label[320];
                std::snprintf(label, sizeof(label), "%s##recent%zu",
                              items[i].c_str(), i);
                if (ImGui::MenuItem(label)) openScene(items[i]);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(TR("Clear Recent"))) {
                m_ctx.recentScenesRef().clear();
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem(TR("Save"), "Ctrl+S")) saveSceneToCurrentPath();
        if (ImGui::MenuItem(TR("Save As (assets/scenes/scene.json)"))) {
            saveSceneAs("assets/scenes/scene.json");
        }

        ImGui::Separator();
        // ---- Import / Export ----
        // Import 走的是**拖拽进视口**那条既有路径（见 ViewportPanel 的
        // AcceptDragDropPayload）—— 这里给的是同一个动作的菜单入口：
        // 打开内容浏览器看一眼能拖什么。真正的"文件选择对话框"要等
        // 平台层有原生对话框（E28 打包 UI 会同批做）。
        if (ImGui::BeginMenu(TR("Import"))) {
            if (ImGui::MenuItem(TR("Reveal assets/ in Content Browser"))) {
                m_ctx.notify(TR("Drag a .gltf/.obj/.fbx from the Content "
                             "browser into the viewport to place it"));
            }
            ImGui::MenuItem(TR("Native file dialog (E28)"), nullptr, false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TR("Export"))) {
            ImGui::MenuItem(TR("Export scene as glTF (E28)"), nullptr, false,
                            false);
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem(TR("Reload Scripts"))) {
            m_ctx.notify(TR("(scripts hot-reload automatically)"));
        }
        ImGui::Separator();
        if (ImGui::MenuItem(TR("Quit"), "Alt+F4")) m_ctx.quitRequested() = true;
        ImGui::EndMenu();
    }

    // ================================================================
    // Edit
    // ================================================================
    if (ImGui::BeginMenu(TR("Edit"))) {
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

        // ---- History ▸：撤销栈可视化（原来是一段平铺的列表，改成子菜单）----
        const auto names = m_ctx.commands().historyNames();
        if (ImGui::BeginMenu(TR("History"), !names.empty())) {
            const size_t cursor = m_ctx.commands().cursor();
            for (size_t i = names.size(); i-- > 0;) {
                const bool applied = i < cursor;
                char label[256];
                std::snprintf(label, sizeof(label), "%s %s##hist%zu",
                              applied ? "[x]" : "[ ]", names[i].c_str(), i);
                ImGui::MenuItem(label, nullptr, false, false);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(TR("Clear History"))) {
                m_ctx.commands().clear();
                m_ctx.notify(TR("History cleared"));
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::BeginDisabled(!m_ctx.hasSelection());
        if (ImGui::MenuItem(TR("Duplicate"), "Ctrl+D")) {
            m_ctx.notify(TR("Use Ctrl+D / the Hierarchy context menu"));
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        // ---- 选择 ----
        // ⚠ Select All / Invert 需要**多选**，而当前的选择模型是单个
        //   Entity（EditorContext::m_selection）。这两项故意禁用而不是
        //   假装能用 —— 多选随 E11（Inspector 升级）/ E22（选择增强）落地。
        ImGui::MenuItem(TR("Select None"), "Esc", false, false);
        if (ImGui::MenuItem(TR("Deselect"))) m_ctx.clearSelection();
        ImGui::BeginDisabled(true);
        ImGui::MenuItem(TR("Select All"), "Ctrl+A", false, false);
        ImGui::MenuItem(TR("Invert Selection"), nullptr, false, false);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("multi-selection lands with E11 / E22"));
        }

        ImGui::Separator();
        if (ImGui::MenuItem(TR("Project Settings..."))) {
            m_ctx.notify(TR("Project settings panel lands with E26 "
                         "(quality tiers)"));
        }
        if (ImGui::MenuItem(TR("Editor Preferences..."))) m_ctx.showSettings() = true;
        ImGui::EndMenu();
    }

    // ================================================================
    // GameObject（保持既有内容不动）
    // ================================================================
    if (ImGui::BeginMenu(TR("GameObject"))) {
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
        if (ImGui::MenuItem(TR("Sunlight"))) {
            ecs::Entity sun{};
            m_ctx.structuralEdit("Add Sunlight", [&]() {
                sun = createSunlight(m_ctx.editorScene());
            });
            if (sun.valid()) m_ctx.select(sun);
        }
        if (ImGui::MenuItem(TR("Directional Light"))) {
            ecs::Entity created{};
            m_ctx.structuralEdit("Add Directional Light", [&]() {
                created = createDirectionalLight(m_ctx.editorScene());
            });
            if (created.valid()) m_ctx.select(created);
        }
        ImGui::EndMenu();
    }

    // ================================================================
    // View
    // ================================================================
    if (ImGui::BeginMenu(TR("View"))) {
        if (ImGui::BeginMenu(TR("View Mode"))) {
            drawViewModeMenu();
            ImGui::EndMenu();
        }

        // 视口布局（E07）
        if (ImGui::BeginMenu(TR("Viewport Layout"))) {
            const int cur = m_host.currentViewportLayout();
            for (int i = 0; i < viewportLayoutCount(); ++i) {
                const auto l = static_cast<ViewportLayout>(i);
                char label[96];
                std::snprintf(label, sizeof(label), "%s##vpl%d",
                              viewportLayoutName(l), i);
                if (ImGui::MenuItem(label, nullptr, i == cur)) {
                    m_host.applyViewportLayout(i);
                    m_ctx.notify(std::string("Viewport layout: ") +
                                 viewportLayoutName(l));
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(TR("Panel"))) {
            drawPanelMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TR("Workspace"))) {
            drawWorkspaceMenu();
            ImGui::EndMenu();
        }

        ImGui::Separator();
        // 地平面栅格（和视口右上角浮层里那个勾选框是同一份状态）
        ImGui::MenuItem(TR("Ground grid (1 m)"), nullptr, &m_ctx.showGridRef());
        if (ImGui::MenuItem(TR("Maximize focused panel"), "Shift+Space")) {
            m_ctx.notify(TR("Shift+Space over a panel toggles maximize"));
        }
        if (ImGui::MenuItem(TR("Reset Layout"))) {
            m_ctx.relayout() = true;
            m_ctx.notify(TR("Layout reset"));
        }
        ImGui::Separator();
        // 窗口真全屏（面板照旧显示）。Play 态的 F 是"全屏 + 只看游戏"。
        if (ImGui::MenuItem(TR("Toggle Fullscreen"), "F11")) {
            m_ctx.windowFullscreenRequest() = true;
        }
        ImGui::EndMenu();
    }

    // ================================================================
    // Build（新）
    // ================================================================
    if (ImGui::BeginMenu(TR("Build"))) {
        if (ImGui::MenuItem(TR("Reload Shaders"), "Ctrl+Shift+R")) {
            m_host.reloadShaders();
        }
        if (ImGui::MenuItem(TR("Reload Assets"))) m_host.reloadAssets();
        ImGui::Separator();
        ImGui::BeginDisabled(true);
        ImGui::MenuItem(TR("Package... (E28)"), nullptr, false, false);
        ImGui::MenuItem(TR("Bake Lighting... (E29)"), nullptr, false, false);
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    // ================================================================
    // Window（面板显隐 + 工作区，和 View 里那两个子菜单同源）
    // ================================================================
    if (ImGui::BeginMenu(TR("Window"))) {
        if (ImGui::BeginMenu(TR("Panels"))) {
            drawPanelMenu();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TR("Workspaces"))) {
            drawWorkspaceMenu();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(TR("Reset Layout"))) {
            m_ctx.relayout() = true;
            m_ctx.notify(TR("Layout reset"));
        }
        ImGui::EndMenu();
    }

    // ================================================================
    // Help
    // ================================================================
    if (ImGui::BeginMenu(TR("Help"))) {
        // E18：命令面板（可发现性的总入口）
        if (ImGui::MenuItem(TR("Command Palette..."), "Ctrl+Shift+P"))
            m_host.showCommandPalette();
        // E21：错过的 WARN/ERROR 都在这里（Toast 只活 4 秒）
        if (ImGui::MenuItem(TR("Show Notification History")))
            m_host.showNotificationHistory();
        if (ImGui::MenuItem(TR("Shortcut Reference"))) m_showShortcuts = true;
        ImGui::SeparatorText(TR("Viewport navigation (edit mode)"));
        ImGui::MenuItem(TR("Hold RMB + drag: look around"), nullptr, false, false);
        ImGui::MenuItem(TR("Hold RMB + WASD: fly   Q / E: down / up"), nullptr,
                        false, false);
        ImGui::MenuItem(TR("Hold MMB + drag: pan   Shift: fly faster"), nullptr,
                        false, false);
        ImGui::MenuItem(TR("Wheel: dolly in / out"), nullptr, false, false);
        ImGui::MenuItem(TR("Hold RMB + wheel: navigation sensitivity"), nullptr,
                        false, false);
        ImGui::SeparatorText(TR("Editing"));
        ImGui::MenuItem(
            TR("W / E / R: gizmo mode   X: world / local   (not while RMB)"),
            nullptr, false, false);
        ImGui::MenuItem(TR("G: snapping on/off for the current tool"), nullptr,
                        false, false);
        ImGui::MenuItem(TR("F: focus selection   Del: delete"), nullptr, false,
                        false);
        ImGui::MenuItem(TR("Shift+A (in viewport): add menu (Mesh / Light)"),
                        nullptr, false, false);
        ImGui::MenuItem(TR("F2: rename (Hierarchy)   Ctrl+D: duplicate"), nullptr,
                        false, false);
        ImGui::SeparatorText(TR("Panels & workspaces"));
        ImGui::MenuItem(TR("Alt+1..8: toggle a panel"), nullptr, false, false);
        ImGui::MenuItem(TR("Ctrl+Alt+1..6: switch workspace"), nullptr, false,
                        false);
        ImGui::MenuItem(TR("Shift+Space: maximize the panel under the mouse"),
                        nullptr, false, false);
        ImGui::MenuItem(TR("Double-click a splitter: collapse / expand"), nullptr,
                        false, false);
        ImGui::MenuItem(TR("Right-click a splitter: ratio presets"), nullptr, false,
                        false);
        ImGui::SeparatorText(TR("Playing (game runs in the viewport)"));
        ImGui::MenuItem(TR("Space: play / pause"), nullptr, false, false);
        ImGui::MenuItem(TR("F: fullscreen, game only (Esc to leave)"), nullptr,
                        false, false);
        ImGui::MenuItem(TR("F11: window fullscreen (works in any mode)"), nullptr,
                        false, false);
        ImGui::Separator();
        ImGui::MenuItem(TR("API docs: docs/editor_architecture.md"), nullptr, false,
                        false);
        if (ImGui::MenuItem(TR("About Venn..."))) m_showAbout = true;
        ImGui::EndMenu();
    }

    // 右侧显示当前场景路径 + 工作区 + "未保存"标记
    const float rightWidth = 460.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - rightWidth);
    ImGui::TextColored(m_ctx.dirty() ? ImVec4(1.0f, 0.82f, 0.4f, 1.0f)
                                     : ImVec4(0.6f, 0.65f, 0.7f, 1.0f),
                       TR("%s%s"), m_ctx.scenePath().c_str(),
                       m_ctx.dirty() ? " *" : "");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.45f, 0.68f, 0.92f, 1.0f), TR("|  %s%s"),
                       m_ctx.workspace().currentTitle(),
                       m_ctx.workspaceModified() ? " *" : "");

    ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------- 工具条
//
// 从"一排按钮"变成**可折叠的工具组**。组的划分按"这个动作属于哪件事"，
// 而不是"看起来排一排行不行"：
//   View  —— 视口布局 + 视图模式（E07 的布局切换按钮就在这里）
//   Build —— 重载着色器（编译期动作）
//   Edit  —— 撤销 / 重做
// 右键工具条切换组的显隐。
//
// ⚠ 刻意**不**把 Move/Rotate/Scale 和 Play 按钮搬回来：它们已经在视口
//   工具条与视口下方的播放条上，工具栏再放一遍就是两个入口写同一份状态
//   （本项目历史上正是因为"重复入口"把它们移走的，见 Toolbar.h 顶部）。

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
    // ⚠ 走 token（不是硬编码）：浅色主题下硬编码会留成一条深色孤岛。
    //   VennDark 的 tok.barBg 就等于改动前那个字面量 → 默认主题逐像素不变。
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::tokens().barBg);
    const ImVec2 pad(12.0f, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(9.0f, 7.0f));

    if (!ImGui::Begin("##toolbar", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }
    // 自动化锚点：顶栏矩形（脚本据此断言"顶栏底色跟主题走"—— 这一处原来
    // 是硬编码深灰，浅色主题下会留成一条深色孤岛）。
    logRect("TB-BAR", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    const ImGuiStyle& st = ImGui::GetStyle();
    // 按钮统一高度（比文字框略高，整条看起来是"一整块"而不是一行小控件）
    const float btnH = 28.0f;
    // 按文字长度算按钮宽度（留一点内边距，标签不贴边）
    auto btnW = [&](const char* label) {
        return ImGui::CalcTextSize(label).x + st.FramePadding.x * 2.0f + 12.0f;
    };

    const float barW = ImGui::GetWindowWidth() - pad.x * 2.0f;
    const float rowY = ImGui::GetCursorPosY();
    const float textY = rowY + (btnH - ImGui::GetTextLineHeight()) * 0.5f;

    // ---- 左侧：当前手柄模式提示（轻量文字，不占按钮）----
    const char* modeName = "Move";
    switch (m_ctx.gizmoMode()) {
        case GizmoMode::Translate: modeName = "Move"; break;
        case GizmoMode::Rotate:    modeName = "Rotate"; break;
        case GizmoMode::Scale:     modeName = "Scale"; break;
    }
    ImGui::SetCursorPos(ImVec2(pad.x, textY));
    ImGui::TextDisabled(TR("%s   (W / E / R)"), modeName);

    // ---- 右侧：工具组（从右往左摆：Edit | Build | View）----
    float cursorRight = pad.x + barW;

    // 单个工具组的绘制辅助：从 cursorRight 往左排 n 个按钮
    auto groupStart = [&](float width) {
        cursorRight -= width;
        ImGui::SetCursorPos(ImVec2(cursorRight, rowY));
    };

    // ---------- Edit 组 ----------
    if (m_groups.transform) {
        const float w = btnW("Undo") + btnW("Redo") + st.ItemSpacing.x;
        groupStart(w);
        ImGui::BeginDisabled(!m_ctx.commands().canUndo());
        if (ImGui::Button(TR("Undo"), ImVec2(btnW("Undo"), btnH))) m_ctx.undo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(m_ctx.commands().canUndo()
                                  ? "Undo: %s  (Ctrl+Z)"
                                  : "Nothing to undo  (Ctrl+Z)",
                              m_ctx.commands().undoName());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_ctx.commands().canRedo());
        if (ImGui::Button(TR("Redo"), ImVec2(btnW("Redo"), btnH))) m_ctx.redo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(m_ctx.commands().canRedo()
                                  ? "Redo: %s  (Ctrl+Y)"
                                  : "Nothing to redo  (Ctrl+Y)",
                              m_ctx.commands().redoName());
        }
        cursorRight -= st.ItemSpacing.x;   // 组间留白
    }

    // ---------- Build 组 ----------
    if (m_groups.build) {
        const float w = btnW("Reload Shaders");
        groupStart(w);
        if (ImGui::Button(TR("Reload Shaders"), ImVec2(w, btnH))) {
            m_host.reloadShaders();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Re-scan .spv mtime now (Ctrl+Shift+R)"));
        }
        cursorRight -= st.ItemSpacing.x;
    }

    // ---------- View 组：视口布局 + 视图模式 ----------
    if (m_groups.view) {
        const char* layoutNames[4] = {"1", "2V", "2H", "4"};
        const int curLayout = m_host.currentViewportLayout();
        float w = 0.0f;
        for (int i = 0; i < viewportLayoutCount() && i < 4; ++i) {
            w += btnW(layoutNames[i]) + st.ItemSpacing.x;
        }
        w += btnW("View: Lit") + st.ItemSpacing.x;
        groupStart(w);

        // 视图模式下拉
        {
            char label[64];
            std::snprintf(label, sizeof(label), "View: %s",
                          render::Renderer::viewModeName(
                              static_cast<render::Renderer::ViewMode>(
                                  m_host.currentViewMode())));
            if (ImGui::Button(label, ImVec2(btnW(label), btnH))) {
                ImGui::OpenPopup("##viewmode_popup");
            }
            if (ImGui::BeginPopup("##viewmode_popup")) {
                drawViewModeMenu();
                ImGui::EndPopup();
            }
        }

        ImGui::SameLine();
        for (int i = 0; i < viewportLayoutCount() && i < 4; ++i) {
            const auto l = static_cast<ViewportLayout>(i);
            const bool on = (i == curLayout);
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.24f, 0.42f, 0.62f, 1.0f));
            }
            if (ImGui::Button(layoutNames[i], ImVec2(btnW(layoutNames[i]), btnH))) {
                m_host.applyViewportLayout(i);
            }
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Viewport layout: %s"), viewportLayoutName(l));
            }
            ImGui::SameLine();
        }
        cursorRight -= st.ItemSpacing.x;
    }

    // ---- 右键：切换工具组显隐 ----
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##toolbar_groups");
    }
    if (ImGui::BeginPopup("##toolbar_groups")) {
        ImGui::SeparatorText(TR("Toolbar groups"));
        ImGui::MenuItem(TR("Edit (Undo/Redo)"), nullptr, &m_groups.transform);
        ImGui::MenuItem(TR("Build"), nullptr, &m_groups.build);
        ImGui::MenuItem(TR("View (layout / view mode)"), nullptr, &m_groups.view);
        ImGui::EndPopup();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- 帮助窗口

void Toolbar::drawAboutWindow() {
    if (m_showAbout) {
        ImGui::OpenPopup("About Venn");
        m_showAbout = false;
    }
    if (ImGui::BeginPopupModal("About Venn", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(TR("Venn Engine  %s"), m_host.engineVersion());
        ImGui::Separator();
        ImGui::Text(TR("GPU: %s"), m_host.gpuName());
        ImGui::Text(TR("Backend: Vulkan 1.2"));
        ImGui::Text(TR("Renderer: clustered forward+ / MSAA / Hi-Z / cascaded "
                    "shadows"));
        ImGui::Text(TR("Editor build: %s %s"), __DATE__, __TIME__);
        ImGui::Separator();
        ImGui::TextDisabled(TR("docs/editor_roadmap.md is the live task list."));
        if (ImGui::Button(TR("Close"), ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Toolbar::drawShortcutReference() {
    if (!m_showShortcuts) return;

    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(220.0f, 120.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trId("Shortcut Reference", "shortcutref"), &m_showShortcuts)) {
        ImGui::End();
        return;
    }

    // 说明不上屏：挂在标题行右边的小 "(?)" 上，悬浮才展开。
    ImGui::SeparatorText(TR("Panels (from the registry)"));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s",
                          TR("Roughly what a 3A editor needs; the panel list "
                             "below is generated from the panel registry, so "
                             "it can never go stale."));
    }
    {
        const PanelDesc* t = panelTable();
        const int n = panelCount();
        for (int i = 0; i < n; ++i) {
            ImGui::Text(TR("%-10s %s"),
                        (t[i].shortcut && t[i].shortcut[0]) ? t[i].shortcut : "-",
                        TR(t[i].title));
        }
    }

    ImGui::SeparatorText(TR("Workspaces"));
    for (int i = 0; i < kWorkspaceCount; ++i) {
        char chord[32];
        std::snprintf(chord, sizeof(chord), "Ctrl+Alt+%d", i + 1);
        ImGui::Text(TR("%-12s %-10s %s"), chord, TR(kWorkspaces[i].title),
                    TR(kWorkspaces[i].hint));
    }

    ImGui::SeparatorText(TR("Layout"));
    ImGui::Text(TR("%-12s %s"), "Shift+Space", "maximize the panel under the mouse");
    ImGui::Text(TR("%-12s %s"), "dbl-click", "collapse / expand a splitter");
    ImGui::Text(TR("%-12s %s"), "right-click", "splitter ratio presets");

    ImGui::SeparatorText(TR("Edit / File"));
    ImGui::Text(TR("%-12s %s"), "Ctrl+Z/Y", "undo / redo");
    ImGui::Text(TR("%-12s %s"), "Ctrl+D", "duplicate selection");
    ImGui::Text(TR("%-12s %s"), "Ctrl+S", "save scene");
    ImGui::Text(TR("%-12s %s"), "Ctrl+N", "new scene");
    ImGui::Text(TR("%-12s %s"), "Del", "delete selection");
    ImGui::Text(TR("%-12s %s"), "F2", "rename (Hierarchy)");
    ImGui::Text(TR("%-12s %s"), "F11", "window fullscreen");

    ImGui::End();
}

// ---------------------------------------------------------------- 入口

void Toolbar::draw() {
    drawMenuBar();
    drawBar();
    drawAboutWindow();
    drawShortcutReference();
}

} // namespace editor
