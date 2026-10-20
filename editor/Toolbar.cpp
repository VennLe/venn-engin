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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

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

// ---- 播放控制的四个矢量字形 ----
// 为什么手画而不用字符 "▶ ■ ⏸"：ImGui 自带字体里这几个是**全角/方块**
// 或者根本没有，换字体就会变成豆腐块。矢量在任何 DPI 下都清楚。
enum TransportGlyph : int {
    kTrPlay = 0,   // 实心三角
    kTrStep,       // 三角 + 右侧竖条（前进一帧）
    kTrPause,      // 两根竖条
    kTrStop,       // 实心方块
    kTrMore,       // 三个点
};

void paintTransportIcon(ImDrawList* dl, ImVec2 p, float s, int glyph,
                        ImU32 col) {
    switch (glyph) {
        case kTrPlay:
            dl->AddTriangleFilled(ImVec2(p.x + s * 0.12f, p.y),
                                  ImVec2(p.x + s * 0.12f, p.y + s),
                                  ImVec2(p.x + s * 0.92f, p.y + s * 0.5f), col);
            break;
        case kTrStep:
            dl->AddTriangleFilled(ImVec2(p.x, p.y),
                                  ImVec2(p.x, p.y + s),
                                  ImVec2(p.x + s * 0.68f, p.y + s * 0.5f), col);
            dl->AddRectFilled(ImVec2(p.x + s * 0.78f, p.y),
                              ImVec2(p.x + s * 0.92f, p.y + s), col);
            break;
        case kTrPause:
            dl->AddRectFilled(ImVec2(p.x + s * 0.18f, p.y),
                              ImVec2(p.x + s * 0.40f, p.y + s), col);
            dl->AddRectFilled(ImVec2(p.x + s * 0.60f, p.y),
                              ImVec2(p.x + s * 0.82f, p.y + s), col);
            break;
        case kTrStop:
            dl->AddRectFilled(ImVec2(p.x + s * 0.12f, p.y + s * 0.12f),
                              ImVec2(p.x + s * 0.88f, p.y + s * 0.88f), col,
                              1.5f);
            break;
        case kTrMore:
            for (int i = 0; i < 3; ++i) {
                const float cx = p.x + s * (0.16f + 0.34f * static_cast<float>(i));
                dl->AddCircleFilled(ImVec2(cx, p.y + s * 0.5f), s * 0.09f, col);
            }
            break;
        default:
            break;
    }
}

// 编辑模式下拉里的一行：图标 + 名字 + 右侧状态。
//
// 排版与视口的"变换工具"下拉（ViewportPanel::vpToolRow）保持一致：单选
// 用**圆点**而不是对勾 —— 对勾是"开关"，圆点是"我只可能是其中一个"。
// 未实现的模式整行压暗 + 右侧标 TODO：用户点得下去（会得到一条 toast），
// 但视觉上先说明"这一项还没有东西"。
bool editModeRow(EditMode m, bool active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = 28.0f;
    const float w = 226.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::PushID(editModeId(m));
    const bool clicked = ImGui::Selectable("##row", false,
                                           ImGuiSelectableFlags_None,
                                           ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    const bool impl = editModeImplemented(m);
    const float cy = p.y + h * 0.5f;

    // 单选圆点
    if (active) {
        dl->AddCircleFilled(ImVec2(p.x + 11.0f, cy), 4.2f,
                            ImGui::GetColorU32(ui::tokens().accent), 12);
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

    // 右侧状态：未实现 → TODO；实现但不是当前项 → 留空
    if (!impl) {
        const char* tag = "TODO";
        const ImVec2 tw = ImGui::CalcTextSize(tag);
        ImVec4 dc = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        dc.w *= 0.8f;
        dl->AddText(ImVec2(p.x + w - tw.x - 10.0f, cy - tw.y * 0.5f),
                    ImGui::GetColorU32(dc), tag);
    }

    char rectTag[64];
    std::snprintf(rectTag, sizeof(rectTag), "TB-EDITMODE-ITEM %s",
                  editModeId(m));
    logRect(rectTag, p, ImVec2(p.x + w, p.y + h));

    if (hovered) ImGui::SetTooltip("%s", TR(editModeHint(m)));
    return clicked;
}

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
                m_ctx.notify(std::string(TR(t[i].title)) +
                             TR(open ? " shown" : " hidden"));
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
    std::snprintf(saveLabel, sizeof(saveLabel), TR("Save Workspace%s"),
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

// 分组之间的竖线。画在"当前行"里，所以要配合 SameLine 使用：
//   SameLine(); toolbarSeparator(); SameLine();
void Toolbar::toolbarSeparator() {
    const float h = 20.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = p.x + 3.0f;
    const float y0 = p.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(x, y0), ImVec2(x, y0 + h),
        ImGui::GetColorU32(ImGuiCol_Separator, 0.55f), 1.0f);
    // 占位：竖线本身没有"宽度"，不留空隙的话下一项会贴到线上
    ImGui::Dummy(ImVec2(7.0f, 1.0f));
}

// ---------------------------------------------------------------- 顶栏 Add 菜单
//
// 用户给的 UE5 截图里，主工具条左侧第二个按钮就是 "+ Add"。它和视口里的
// Shift+A 菜单**用的是同一批工厂函数**（createPrimitiveOnGround /
// createSunlight / createDirectionalLight / createPlayer），区别只有一个：
// 落点。视口有鼠标射线可以算交点，顶栏没有 —— 于是按环形排队摆，
// 免得连点两次加出两个完全重叠的物体（叠在一起看起来像"没加上"）。
glm::vec3 Toolbar::nextAddSpot() {
    const int n = m_addCounter++;
    constexpr int kPerRing = 8;
    const float angle = static_cast<float>(n % kPerRing) *
                        (3.14159265f / static_cast<float>(kPerRing) * 2.0f);
    const float radius = 3.0f * (1.0f + static_cast<float>(n / kPerRing));
    return glm::vec3(std::cos(angle) * radius, std::sin(angle) * radius, 0.0f);
}

void Toolbar::drawAddBarMenu() {
    // 自动化锚点：菜单窗口本身 + 每个条目（脚本按矩形中心点，不猜行高）
    const auto menuRect = [](const char* tag, const ImVec2& a, const ImVec2& b) {
        logRect(tag, a, b);
    };
    menuRect("TB-ADD-WIN", ImGui::GetWindowPos(),
             ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                    ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    const auto add = [this](const char* what,
                            const std::function<ecs::Entity()>& create) {
        ecs::Entity created{};
        m_ctx.structuralEdit(std::string("Add ") + what,
                             [&]() { created = create(); });
        if (created.valid()) {
            m_ctx.select(created);
            m_ctx.notify(std::string(TR("Added ")) + what);
            VK_LOG_INFO("TB-ADD-ENTITY what=%s", what);
        }
    };

    ImGui::SeparatorText(TR("Add"));

    // ---- Mesh ----
    {
        const bool open = ImGui::BeginMenu(TR("Mesh"));
        menuRect("TB-ADD-Mesh", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (open) {
            struct Item { const char* label; PrimitiveKind kind; };
            const Item items[] = {
                {"Cube", PrimitiveKind::Cube},
                {"Sphere", PrimitiveKind::Sphere},
                {"Cylinder", PrimitiveKind::Cylinder},
                {"Plane", PrimitiveKind::Plane},
                {"Empty (group)", PrimitiveKind::Empty},
            };
            for (const Item& it : items) {
                const bool hit = ImGui::MenuItem(TR(it.label));
                char tag[64];
                std::snprintf(tag, sizeof(tag), "TB-ADD-%s", it.label);
                menuRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (hit) {
                    const glm::vec3 spot = nextAddSpot();
                    const PrimitiveKind k = it.kind;
                    add(it.label, [this, k, spot]() {
                        return createPrimitiveOnGround(m_ctx.editorScene(),
                                                       m_ctx.assets(), k, spot);
                    });
                }
            }
            ImGui::EndMenu();
        }
    }

    // ---- Light ----
    {
        const bool open = ImGui::BeginMenu(TR("Light"));
        menuRect("TB-ADD-Light", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (open) {
            if (ImGui::MenuItem(TR("Sunlight"))) {
                add("Sunlight", [this]() {
                    return createSunlight(m_ctx.editorScene());
                });
            }
            menuRect("TB-ADD-Sunlight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (ImGui::MenuItem(TR("Directional Light"))) {
                const glm::vec3 spot = nextAddSpot();
                add("Directional Light", [this, spot]() {
                    return createDirectionalLight(m_ctx.editorScene(), spot);
                });
            }
            menuRect("TB-ADD-DirectionalLight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (ImGui::MenuItem(TR("Spot Light"))) {
                const glm::vec3 spot = nextAddSpot();
                add("Spot Light", [this, spot]() {
                    return createPrimitiveOnGround(m_ctx.editorScene(),
                                                   m_ctx.assets(),
                                                   PrimitiveKind::SpotLight,
                                                   spot);
                });
            }
            menuRect("TB-ADD-SpotLight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (ImGui::MenuItem(TR("Point Light"))) {
                const glm::vec3 spot = nextAddSpot();
                add("Point Light", [this, spot]() {
                    return createPrimitiveOnGround(m_ctx.editorScene(),
                                                   m_ctx.assets(),
                                                   PrimitiveKind::PointLight,
                                                   spot);
                });
            }
            menuRect("TB-ADD-PointLight", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            ImGui::EndMenu();
        }
    }

    // ---- Player ----
    {
        const bool open = ImGui::BeginMenu(TR("Player"));
        menuRect("TB-ADD-Player", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (open) {
            if (ImGui::MenuItem(TR("First-Person Player"))) {
                const glm::vec3 spot = nextAddSpot();
                add("First-Person Player", [this, spot]() {
                    return createPlayer(m_ctx.editorScene(), m_ctx.assets(),
                                        ecs::PlayerMode::FirstPerson, spot);
                });
            }
            menuRect("TB-ADD-FirstPerson", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            if (ImGui::MenuItem(TR("Third-Person Player"))) {
                const glm::vec3 spot = nextAddSpot();
                add("Third-Person Player", [this, spot]() {
                    return createPlayer(m_ctx.editorScene(), m_ctx.assets(),
                                        ecs::PlayerMode::ThirdPerson, spot);
                });
            }
            menuRect("TB-ADD-ThirdPerson", ImGui::GetItemRectMin(),
                     ImGui::GetItemRectMax());
            ImGui::EndMenu();
        }
    }

    ImGui::Separator();
    ImGui::MenuItem(TR("Place at cursor: Shift+A inside the viewport"), nullptr,
                    false, false);
    ImGui::MenuItem(TR("Particles: open the Particles panel"), nullptr, false,
                    false);
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
                       TR(m_ctx.workspace().currentTitle()),
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

    // ============================================================
    // 2026-10-17 第四轮：整条顶栏按用户给的 UE5 主工具条重做。
    //
    // 用户原话："至于原来的 Move（W / E / R ），View：lit，Reload Shaders，
    // Undo，Redo，这些按钮全都不需要，都移除掉。移除掉后可以重新按照
    // <UE 截图> 这个样子重新去做，这个图片中的选择模式是个下拉列表"。
    //
    // 删掉的五样东西**没有丢功能**，只是不再占这条最显眼的横条：
    //   · Move/Rotate/Scale  → 视口工具条那四个图标 + W/E/R/Q（这里只剩
    //                          "选择模式"下拉，是同一份状态的另一个入口）
    //   · View: lit          → 视口工具条的"照明"下拉（Ctrl+Shift 那套还在）
    //   · Reload Shaders     → Build 菜单 / Ctrl+Shift+R
    //   · Undo / Redo        → Edit 菜单 / Ctrl+Z / Ctrl+Y
    //   · 1 / 2V / 2H / 4    → 视口工具条，"透视"右边（用户要求并排放）
    //
    // ⚠ 用户以为 1/2V/2H/4 是"渲染分辨率"，其实是**视口分屏布局**
    //   （1 屏 / 左右 2 屏 / 上下 2 屏 / 4 屏）。挪过去时名字改成"布局"，
    //   免得继续被当成分辨率。
    //
    // 2026-10-20：最后那句"这里只剩选择模式下拉"也不成立了 —— 这颗下拉
    // 已经从"手柄模式"改成"编辑模式"（见下面 ① 的说明）。手柄模式的入口
    // 现在只剩视口工具条与 Q/W/E/R。
    // ============================================================

    // ---------------------------------------------------------- ① 编辑模式
    //
    // 2026-10-20：按用户给的 UE5 "Selection Mode" 截图重做。
    //
    // 原来这颗下拉里是 Select / Move / Rotate / Scale —— 那是**手柄模式**，
    // 与 UE 那颗同名按钮里装的东西并不是一回事：UE 列的是"整套编辑工具"
    // （选择 / 地形 / 植被 / 网格体绘制 / 建模 / 破裂 / 笔刷编辑 / 动画 /
    // PCG），选哪个就换一套编辑器。所以这里换成 EditMode。
    //
    // 手柄模式没有丢：视口工具条那四个图标 + Q/W/E/R 一个都没动，顶栏这颗
    // 只是它的第三个入口，现在让位给编辑模式。
    {
        const EditMode cur = m_ctx.editMode();
        const char* curLabel = TR(editModeName(cur));
        const float iconS = 16.0f;
        const float caretW = 13.0f;
        const float w = btnW(curLabel) + iconS + 6.0f + caretW;

        const bool hit = ImGui::Button("##editmode_btn", ImVec2(w, btnH));
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();

        // 按钮内容全部自绘：ImGui::Button 的标签是**居中**的，没法"左图标
        // 右文字"，而空标签按钮又只画框 —— 所以图标、文字、小三角都在
        // 同一个 DrawList 上补（与播放条 transportBtn 同一条经验）。
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);
        const float cy = (a.y + b.y) * 0.5f;
        const float ix = a.x + st.FramePadding.x + 2.0f;
        paintEditModeIcon(bdl, ImVec2(ix, cy - iconS * 0.5f), iconS, cur, fg);
        bdl->AddText(ImVec2(ix + iconS + 6.0f,
                            cy - ImGui::GetTextLineHeight() * 0.5f),
                     fg, curLabel);
        {
            const float cx = b.x - st.FramePadding.x - 6.0f;
            bdl->AddTriangleFilled(ImVec2(cx - 4.0f, cy - 2.0f),
                                   ImVec2(cx + 4.0f, cy - 2.0f),
                                   ImVec2(cx, cy + 3.0f),
                                   ImGui::GetColorU32(ImGuiCol_TextDisabled));
        }

        if (hit) ImGui::OpenPopup("##editmode_popup");
        logRect("TB-EDITMODE", a, b);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n%s", curLabel, TR(editModeHint(cur)));
        }
        if (ImGui::BeginPopup("##editmode_popup")) {
            logRect("TB-EDITMODE-WIN", ImGui::GetWindowPos(),
                    ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                           ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
            // 行自己管高度（每行 28 px），所以这里把 ItemSpacing 清零 ——
            // 否则行与行之间会出现一道道竖向空隙，看起来像分组。
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            for (int i = 0; i < kEditModeCount; ++i) {
                const EditMode m = static_cast<EditMode>(i);
                if (!editModeRow(m, cur == m)) continue;
                if (editModeImplemented(m)) {
                    m_ctx.setEditMode(m);
                    VK_LOG_INFO("TB-EDITMODE item=%s", editModeId(m));
                } else {
                    // 未实现的模式**不假装切过去**：顶栏显示当前模式、面板
                    // 跟着开，这是整套 UI 的契约，让一个"切过去了但什么都
                    // 没有"的模式上屏等于破坏它。给一条明确的提示。
                    m_ctx.notify(std::string(TR("Not implemented yet: ")) +
                                 TR(editModeName(m)));
                    VK_LOG_INFO("TB-EDITMODE todo=%s", editModeId(m));
                }
                ImGui::CloseCurrentPopup();
                break;
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
    }


    // ---------------------------------------------------------- ② 添加
    ImGui::SameLine();
    toolbarSeparator();
    ImGui::SameLine();
    {
        char label[96];
        std::snprintf(label, sizeof(label), "+  %s", TR("Add"));
        if (ImGui::Button(label, ImVec2(btnW(label), btnH))) {
            ImGui::OpenPopup("##tb_add_popup");
        }
        logRect("TB-ADD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Add an object to the scene (also: Shift+A in the\n"
                              "viewport, which drops it where the mouse points)."));
        }
        if (ImGui::BeginPopup("##tb_add_popup")) {
            drawAddBarMenu();
            ImGui::EndPopup();
        }
    }

    // ---------------------------------------------------------- ③ 播放控制（右对齐）
    //
    // 从右往左数：更多(⋮) | 停止 | 暂停 | 前进一帧 | 运行
    // 顺序反着摆，这样"运行"永远在最左边那颗 —— 与 UE 一致。
    {
        const float iconBtn = 30.0f;
        const float gap = 2.0f;
        const float totalW = iconBtn * 5.0f + gap * 4.0f;
        const float targetX = pad.x + barW - totalW;
        if (targetX > ImGui::GetCursorPosX() + 8.0f) {
            ImGui::SameLine(targetX);
        } else {
            ImGui::SameLine();
        }
        logRect("TB-PLAY", ImGui::GetCursorScreenPos(),
                ImVec2(ImGui::GetCursorScreenPos().x + totalW,
                       ImGui::GetCursorScreenPos().y + btnH));

        const bool playing = m_ctx.isPlaying();
        const bool paused = m_ctx.isPaused();
        const bool inRun = m_ctx.usingRuntime();

        const auto transportBtn = [&](const char* id, int glyph, const char* tip,
                                      bool enabled, bool accent) {
            if (!enabled) ImGui::BeginDisabled();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool hit = ImGui::InvisibleButton(id, ImVec2(iconBtn, btnH));
            const bool hov = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 q(p.x + iconBtn, p.y + btnH);

            // ⚠ InvisibleButton **什么都不画** —— 底色必须自己补。
            //   第一版只 PushStyleColor(ImGuiCol_Button) 就以为会有底色，
            //   结果"运行"那颗的绿色一次都没显示出来（截图里是灰的）。
            //   强调色只在"能按"的时候画：暂停态下绿色不再代表可运行。
            ImU32 bg = 0;
            if (accent && enabled) {
                bg = held ? IM_COL32(0x1B, 0x4A, 0x24, 255)
                          : IM_COL32(0x2E, 0x7A, 0x3A, 255);
            } else if (held) {
                bg = IM_COL32(255, 255, 255, 44);
            } else if (hov) {
                bg = IM_COL32(255, 255, 255, 26);
            }
            if (bg) dl->AddRectFilled(p, q, bg, 3.0f);

            // 图标颜色：禁用态自己压暗（手绘不走 ImGui 的 DisabledAlpha）
            ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            if (!enabled) {
                ImVec4 t = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                t.w *= 0.45f;
                col = ImGui::GetColorU32(t);
            }
            paintTransportIcon(dl, ImVec2(p.x + 8.0f, p.y + 7.0f), 14.0f, glyph,
                               col);
            if (!enabled) ImGui::EndDisabled();
            char tag[64];
            std::snprintf(tag, sizeof(tag), "TB-PLAY-%s", id);
            logRect(tag, p, q);
            if (hov && tip) ImGui::SetTooltip("%s", TR(tip));
            return hit;
        };

        // 运行（绿）：Play 期间这颗是"停止运行"的语义，所以底色跟着变
        if (transportBtn("run", kTrPlay,
                         playing ? "Stop running this scene  (Esc)"
                                 : "Run this scene in the viewport  (Alt+P)",
                         true, !playing)) {
            if (playing) {
                m_ctx.stop();
                VK_LOG_INFO("TB-PLAY run=0");
            } else {
                m_ctx.play();
                VK_LOG_INFO("TB-PLAY run=1");
            }
        }
        ImGui::SameLine(0.0f, gap);
        if (transportBtn("step", kTrStep,
                         "Advance exactly one frame (enters pause first)",
                         true, false)) {
            m_host.stepOneFrame();
            VK_LOG_INFO("TB-PLAY step");
        }
        ImGui::SameLine(0.0f, gap);
        if (transportBtn("pause", kTrPause,
                         paused ? "Resume" : "Pause  (P)", inRun, false)) {
            if (paused) m_ctx.play(); else m_ctx.pause();
            VK_LOG_INFO("TB-PLAY pause=%d", m_ctx.isPaused() ? 1 : 0);
        }
        ImGui::SameLine(0.0f, gap);
        if (transportBtn("stop", kTrStop, "Stop and return to the editor scene  (Esc)",
                         inRun, false)) {
            m_ctx.stop();
            VK_LOG_INFO("TB-PLAY stop");
        }
        ImGui::SameLine(0.0f, gap);
        if (transportBtn("more", kTrMore, "More run options", true, false)) {
            ImGui::OpenPopup("##tb_play_more");
        }
        if (ImGui::BeginPopup("##tb_play_more")) {
            ImGui::SeparatorText(TR("Play"));
            if (ImGui::MenuItem(TR("Run / continue"), "Alt+P", false, !playing)) {
                m_ctx.play();
                VK_LOG_INFO("TB-PLAY run=1");
            }
            if (ImGui::MenuItem(TR("Pause"), "P", false, inRun)) {
                m_ctx.pause();
                VK_LOG_INFO("TB-PLAY pause=1");
            }
            if (ImGui::MenuItem(TR("Stop"), "Esc", false, inRun)) {
                m_ctx.stop();
                VK_LOG_INFO("TB-PLAY stop");
            }
            ImGui::Separator();
            ImGui::MenuItem(TR("Simulate (no player input)"), nullptr, false, false);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(TR("Not implemented: the runtime has no way to\n"
                                  "detach player input from the simulation yet."));
            }
            ImGui::EndPopup();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- 帮助窗口

void Toolbar::drawAboutWindow() {
    if (m_showAbout) {
        ImGui::OpenPopup(TRL("About Venn"));
        m_showAbout = false;
    }
    if (ImGui::BeginPopupModal(TRL("About Venn"), nullptr,
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
