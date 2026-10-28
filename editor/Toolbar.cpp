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

#include "project/ProjectBuild.h"
#include "project/ProjectFile.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

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

// ---- "另存为"的原生对话框 ----
//
// 为什么必须换成对话框：原来这一项写死 `assets/scenes/scene.json`，
// 于是"打开 level2.json → Save As"会**覆盖 scene.json**，而用户以为
// 自己另存了一份。这不是难看，是丢数据。
//
// ⚠ `OFN_NOCHANGEDIR` 不能省：不带它，对话框会把**进程当前目录**改成用户
//   最后浏览的那个文件夹 —— 这套引擎的资源查找 / m_root 全建立在
//   "工作目录 = 项目根"上，一改就全盘错位，且下次启动还开在那儿。
//   （ContentBrowser 的导入对话框踩过同一个坑。）
std::string askSaveSceneAbsPath(const std::string& currentRel) {
#ifdef _WIN32
    // 自动化：脚本点不了系统对话框，而"Save As 到底写到哪个文件"恰恰是
    // 要断言的语义 —— 所以给一个直接给目标路径的开关。
    if (const char* env = std::getenv("MYVK_EDITOR_SAVEAS_PATH")) {
        if (env[0] != '\0') {
            VK_LOG_INFO("SAVEAS path_from_env=%s", env);
            return std::string(env);
        }
    }
    // 连"只走流程"也不要（与导入那边的开关同名）
    if (std::getenv("MYVK_EDITOR_CB_NO_DIALOG")) {
        VK_LOG_INFO("SAVEAS dialog=suppressed");
        return std::string();
    }

    // 默认落点 = 当前场景所在目录 + 同名文件。
    std::filesystem::path guess(currentRel);
    if (guess.is_relative()) guess = std::filesystem::absolute(guess);
    std::string defaultName = guess.filename().string();
    if (defaultName.empty()) defaultName = "scene.json";
    const std::wstring seed =
        (guess.parent_path() / std::filesystem::path(defaultName)).wstring();

    std::vector<wchar_t> buf(4096, L'\0');
    {
        const std::size_t n = seed.copy(buf.data(), buf.size() - 1);
        buf[n] = L'\0';
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    if (!ofn.hwndOwner) ofn.hwndOwner = GetForegroundWindow();
    ofn.lpstrFilter = L"Scene (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Save scene as";
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_EXPLORER |
                OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) {
        VK_LOG_INFO("SAVEAS cancelled");
        return std::string();
    }
    return std::filesystem::path(std::wstring(buf.data())).u8string();
#else
    (void)currentRel;
    return std::string();
#endif
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

void Toolbar::saveSceneAsDialog() {
    const std::string abs = askSaveSceneAbsPath(m_ctx.scenePath());
    if (abs.empty()) {
        // 取消 / 非 Windows / 自动化抑制。**绝不能**退回到"写 scene.json"
        // —— 那正是这个入口原来的 bug（把另一个场景覆盖掉）。
        VK_LOG_INFO("SAVEAS abort (no path)");
        return;
    }
    // 对话框给的是绝对路径；存盘与 scenePath 都用项目相对路径
    // （`makeAssetRelative` 在"逃出资产根"时原样返回绝对路径，仍然可用）。
    saveSceneAs(assets::makeAssetRelative(abs));
    VK_LOG_INFO("SAVEAS abs=%s", abs.c_str());
}

// ---------------------------------------------------------------- 出包（E28-1）
void Toolbar::packageProject(bool asPak) {
    namespace fs = std::filesystem;

    // 工程根 = 当前工作目录。编辑器在认到 `MYVK_EDITOR_PROJECT` 时会
    // `chdir` 到工程根（EditorApp 里那段 "D7"），所以这里不需要再问环境变量；
    // 反过来说，**没打开工程**时 CWD 就是仓库根 —— 那里没有
    // `venn.project.json`，下面会明确拒绝，而不是"打包出一个空壳"。
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (ec) {
        m_ctx.notify(TR("Package failed: ") + ec.message());
        return;
    }

    project::ProjectFile pf;
    std::string perr;
    if (!project::loadProject(cwd.generic_string(), pf, &perr)) {
        m_ctx.notify(TR("Package needs an open project: ") + perr);
        VK_LOG_WARN("PACKAGE-FAIL no project at %s (%s)",
                    cwd.generic_string().c_str(), perr.c_str());
        return;
    }

    // 打包用的是 **Game.exe**，不是 Editor.exe —— 出包出去的东西里不能有
    // 编辑器。两者是同一个构建目录里的兄弟文件（`build/bin/`）。
    const std::string self = project::engineExePath();
    const size_t slash = self.find_last_of('/');
    const std::string exeDir = (slash == std::string::npos) ? std::string(".")
                                                            : self.substr(0, slash);
    const std::string gameExe = exeDir + "/Game.exe";
    if (!fs::is_regular_file(fs::path(gameExe), ec)) {
        m_ctx.notify(TR("Package failed: Game.exe not found next to the editor"));
        VK_LOG_WARN("PACKAGE-FAIL game exe missing: %s", gameExe.c_str());
        return;
    }

    project::BuildSpec spec;
    spec.projectRoot = cwd.generic_string();
    spec.engineExe = gameExe;
    spec.usePak = asPak;
    // 输出到 `<工程根>/dist/<Name>`。**不自动覆盖**：上一次的包还在的话
    // 会明确失败并告诉你删哪个目录 —— 出包覆盖是最容易"以为发了新版、
    // 其实发的还是旧包"的地方。
    std::string name = pf.name.empty() ? std::string("Game") : pf.name;
    for (char& c : name) {
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    spec.outDir = (fs::path(spec.projectRoot) / "dist" / name).generic_string();

    VK_LOG_INFO("PACKAGE-START from=%s out=%s pak=%d", spec.projectRoot.c_str(),
                spec.outDir.c_str(), asPak ? 1 : 0);
    const project::BuildResult r = project::buildProject(spec, nullptr);
    for (const auto& w : r.warnings) {
        VK_LOG_WARN("PACKAGE-WARN %s", w.c_str());
    }
    if (!r.ok) {
        // 通知条只放第一行（多行会把 toast 撑爆），完整原因进日志。
        std::string first = r.error.substr(0, r.error.find('\n'));
        m_ctx.notify(TR("Package failed: ") + first);
        VK_LOG_ERROR("PACKAGE-FAIL %s", r.error.c_str());
        return;
    }
    // ⚠ 成功提示用**一条**完整格式串，不要把"已打包"/数字/单位拼起来：
    //   语序不同的语言里拼出来是断的（本项目 i18n 检查也只看整条带 TR 的
    //   格式串，拼接出来的碎片它数不到，于是"缺译文"会一直挂着）。
    char msg[320];
    std::snprintf(msg, sizeof(msg), TR("Packaged %d files · %lld KB  →  %s"),
                  r.files, static_cast<long long>(r.bytes / 1024),
                  r.outDir.c_str());
    m_ctx.notify(msg);
    VK_LOG_INFO("PACKAGE-OK out=%s files=%d bytes=%lld warnings=%zu",
                r.outDir.c_str(), r.files, r.bytes, r.warnings.size());
}

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

// ---------------------------------------------------------------- 菜单栏

void Toolbar::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    // ================================================================
    // File
    // ================================================================
    // 一级菜单的锚点（E08 之后第一次被自动化覆盖）：
    // 菜单项**没有** ImGui ID 可查，脚本只能按矩形点 —— 所以每个要点进去
    // 的项都留一个 logRect。标签一律 ASCII（脚本按它取坐标）。
    auto tagMenu = [](const char* tag) {
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    };

    const bool fileMenuOpen = ImGui::BeginMenu(TR("File"));
    tagMenu("MENU-FILE");   // ⚠ 锚点必须在 if 之前：菜单没展开时也要能点到它
    if (fileMenuOpen) {
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
        tagMenu("MENU-SAVE");
        // 走原生"另存为"对话框（默认落在当前场景目录、预填同名文件）。
        // 原来这里写死 `assets/scenes/scene.json` —— 打开别的场景后点它
        // 会把那个场景覆盖掉，而用户以为自己另存了一份。
        if (ImGui::MenuItem(TR("Save As"))) saveSceneAsDialog();
        tagMenu("MENU-SAVEAS");

        ImGui::Separator();
        // ---- E28-1 出包 ----
        // 放在 File 里、不单开一个 Build 菜单：现在只有一条命令，为它开一个
        // 顶级菜单只会让人以为下面还有东西（UE 那个 Platforms 菜单是几十年
        // 攒出来的，我们还没到那一步）。等有了"Cook 内容 / 平台 / 签名"
        // 才值得开。
        if (ImGui::MenuItem(TR("Package project..."))) packageProject(false);
        tagMenu("MENU-PACKAGE");
        // 第一版 pak 单列一条，不做成"出包对话框里的一个勾选框"：
        // 现在没有出包对话框（就一条同步命令），为一个布尔值先造一套对话框
        // 不值。而且两条菜单项**各自有名字**，用户能直接看出区别；
        // 一个勾选框藏在哪、默认是什么，反而要试。
        if (ImGui::MenuItem(TR("Package project as .pak..."))) {
            packageProject(true);
        }
        tagMenu("MENU-PACKAGE-PAK");

        ImGui::Separator();
        // ---- Import / Export ----
        // Import 现在是**真入口**：转到 ContentBrowser 那条既有的原生
        // 文件对话框（`OPENFILENAMEW` + 同名不覆盖），不再只是"去看一眼"。
        // 原来这里有一条灰着的 `Native file dialog (E28)`，注释写着"等平台
        // 层有原生对话框"—— 可 Content 里早就在跑了。同一个能力两个入口，
        // 一个能用、一个灰着，只会让人以为引擎没有它。
        const bool importMenuOpen = ImGui::BeginMenu(TR("Import"));
        tagMenu("MENU-FILE-IMPORT");
        if (importMenuOpen) {
            if (ImGui::MenuItem(TR("Import assets..."))) {
                m_host.requestImportDialog();
            }
            tagMenu("MENU-IMPORT-ASSETS");
            if (ImGui::MenuItem(TR("Reveal assets/ in Content Browser"))) {
                panelOpenRef("content") = true;
                m_ctx.notify(TR("Drag a .gltf/.obj/.fbx from the Content "
                             "browser into the viewport to place it"));
            }
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
    const bool editMenuOpen = ImGui::BeginMenu(TR("Edit"));
    tagMenu("MENU-EDIT");
    if (editMenuOpen) {
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
        // 真的复制（`SceneHierarchy::duplicateSelection()`：uniqueName +
        // 一批一次 structuralEdit，撤销一次全回滚）。原来这里只弹一句
        // "用 Ctrl+D"——菜单项自己不做事的唯一结果是让人以为功能没做。
        if (ImGui::MenuItem(TR("Duplicate"), "Ctrl+D")) {
            m_host.duplicateSelection();
        }
        tagMenu("MENU-EDIT-DUP");
        ImGui::EndDisabled();

        ImGui::Separator();
        // ---- 选择 ----
        // 多选（selectionSet / selectMany / selectAll）2026-10-27 就落地了，
        // 这三项跟着接线。原来三项全灰着、tooltip 写"multi-selection lands
        // with E11 / E22"—— 那两件早就做完了，菜单还停在当时的注上。
        if (ImGui::MenuItem(TR("Select None"), "Esc")) m_ctx.clearSelection();
        tagMenu("MENU-EDIT-SELECTNONE");
        if (ImGui::MenuItem(TR("Select All"), "Ctrl+A")) m_ctx.selectAll();
        tagMenu("MENU-EDIT-SELECTALL");
        if (ImGui::MenuItem(TR("Invert Selection"))) m_ctx.invertSelection();
        tagMenu("MENU-EDIT-INVERT");

        ImGui::Separator();
        // E26 落地的是**画质档位**面板（`quality`）。这一项当时是占位提示
        // （"lands with E26"），现在直接接到那张面板上 —— 编辑器里现存的
        // 项目级设置就是画质档位；输入映射在 Preferences ▸ Keymap 页。
        if (ImGui::MenuItem(TR("Project Settings..."))) {
            panelOpenRef("quality") = true;
            m_ctx.notify(TR("Project settings: quality tiers"));
        }
        tagMenu("MENU-EDIT-PROJSET");
        if (ImGui::MenuItem(TR("Editor Preferences..."))) m_ctx.showSettings() = true;
        ImGui::EndMenu();
    }

    // ================================================================
    // GameObject（保持既有内容不动）
    // ================================================================
    const bool gameObjectMenuOpen = ImGui::BeginMenu(TR("GameObject"));
    tagMenu("MENU-GAMEOBJECT");
    if (gameObjectMenuOpen) {
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
            // 按**枚举值**打锚点（Cube=0 / Sphere=1 / … / Empty=6），
            // 不用可翻译的名字：锚点是脚本的契约，不能随界面语言变。
            char tag[32];
            std::snprintf(tag, sizeof(tag), "MENU-GO-%d", static_cast<int>(k));
            tagMenu(tag);
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
    // 2026-10-21：顶栏**只剩播放控制**（右对齐那一组）。
    //
    // 用户原话：
    //   「编辑器上的原本的模式类型的那个下拉框可以去掉了，因为是冗余的，
    //     还有旁边的那个 Add 的列表可以把它的位置调整到视口上侧工具条
    //     左侧的末尾。」
    //
    // 两样东西的去向：
    //   · 编辑模式下拉 → **删掉**。左栏 ModeRail 那 10 行单选是同一个状态的
    //     另一个入口，而且更常驻（顶栏这颗要先把下拉点开才看得到十项）。
    //     删掉后顶栏这条横条空出来，不再和"当前模式"抢注意力。
    //   · "+ Add" 按钮 → 搬到**视口工具条左组末尾**（ViewportPanel::
    //     drawToolbar）。它加的物体落在视口里，放在视口上沿比顶栏更近；
    //     菜单内容见 editor/AddMenu.cpp。
    //
    // 手柄模式（Q/W/E/R）的入口一直是视口工具条那四个图标，这两块删掉
    // 不影响它。
    // ============================================================

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
