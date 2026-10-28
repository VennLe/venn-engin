#pragma once
// ============================================================
// editor/Toolbar —— 主菜单栏 + 工具条（E08 重构）
//
// 历史上这条工具栏塞过 Save/New/手柄模式/播放按钮；现在全部移走：
//   · Save / New       → File 菜单（Ctrl+S / Ctrl+N 一直都在）
//   · Move/Rotate/Scale → 视口工具条（W / E / R），工具栏里是重复入口
//   · Fullscreen       → F / F11 快捷键 + View 菜单
//   · Run / Play/Pause/Stop → 视口正下方那一行播放条
//     （Run=运行游戏；Play/Pause/Stop=编辑态脚本动画，见 EditorContext）
//
// ------------------------------------------------------------
// E08：菜单栏补全 + 工具栏分组成"可配置的条"
// ------------------------------------------------------------
// 菜单结构（括号里是数据来源）：
//   File  New / Open / Open Recent ▸ / Save / Save As / Import ▸ / Export ▸ / Quit
//   Edit  Undo / Redo / History ▸ / Duplicate / Delete / Select None /
//         Project Settings ▸ / Preferences ▸
//   View  View Mode ▸ / Panel ▸（**由 PanelRegistry 生成**）/ Workspace ▸ /
//         Fullscreen / Reset Layout
//   Build Reload Shaders / Reload Assets / Package ▸（E28，暂占位）
//   Window 面板显隐 + 工作区（同 View 的两个子菜单）
//   Help  Shortcut Reference / API Docs / About（版本 + GPU + 后端）
//
// 工具栏从"一排按钮"变成**可折叠的工具组**（Transform / Snapping / Play /
// View / Build），每组的显隐右键工具栏切换并落盘。
//
// ------------------------------------------------------------
// 为什么要一个 EditorHost 接口
// ------------------------------------------------------------
// 菜单里有几项要动**引擎**（切工作区要动 SplitLayout、切视图模式要动
// Renderer、About 要读 GPU 型号）—— 这些 Toolbar 都拿不到。而 Toolbar
// 只依赖 EditorContext 是本项目的一条硬约束（面板之间不互相引用）。
// 所以用一个**窄接口**把它们挡在外面：Toolbar 只知道"我要切工作区"，
// 不知道工作区是怎么落地的。实现由 EditorApp 提供（它两者都持有）。
// ============================================================

#include "EditorContext.h"

#include <glm/glm.hpp>

#include <string>

namespace editor {

// 菜单里那些"必须动引擎"的动作的窄接口。EditorApp 实现它。
class EditorHost {
public:
    virtual ~EditorHost() = default;

    // ---- E05 工作区 ----
    virtual void applyWorkspace(int index) = 0;
    virtual void saveWorkspace() = 0;
    virtual void resetWorkspace() = 0;

    // ---- E07 视口布局（ViewportLayout 的整数值）----
    virtual void applyViewportLayout(int layout) = 0;
    virtual int currentViewportLayout() = 0;

    // ---- 渲染视图模式（Renderer::ViewMode 的整数值）----
    virtual void applyViewMode(int mode) = 0;
    virtual int currentViewMode() = 0;

    // ---- Build 菜单 ----
    virtual void reloadShaders() = 0;
    virtual void reloadAssets() = 0;

    // ---- Help ▸ About ----
    virtual const char* gpuName() = 0;
    virtual const char* engineVersion() = 0;

    // ---- 顶栏播放条：前进一帧 ----
    // "先进入暂停（有运行态）再推进一个固定步长"这件事要动物理世界和
    // 脚本引擎，Toolbar 两样都拿不到 —— 所以只发一个意图出去。
    virtual void stepOneFrame() = 0;

    // ---- E18 命令面板（Help 菜单的入口；Ctrl+Shift+P 由 EditorApp 直接处理）----
    virtual void showCommandPalette() = 0;
    // ---- E21 通知历史（Help 菜单的入口）----
    virtual void showNotificationHistory() = 0;

    // ---- Edit ▸ Duplicate ----
    // 实现是 `SceneHierarchy::duplicateSelection()`；Toolbar 不持有 Hierarchy
    // （面板之间只准依赖 EditorContext），所以只发意图。
    virtual void duplicateSelection() = 0;

    // ---- File ▸ Import ▸ Import assets... ----
    // 原生文件对话框属于 ContentBrowser；这里只请求"打开它"。
    virtual void requestImportDialog() = 0;
};

class Toolbar {
public:
    Toolbar(EditorContext& ctx, EditorHost& host) : m_ctx(ctx), m_host(host) {}

    void draw();

    // 文件操作（Ctrl+N / Ctrl+S 由 EditorApp 转发到这里）
    void newScene();
    void saveSceneToCurrentPath();
    // 打开某个相对路径的场景（File ▸ Open Recent / 启动钩子用）
    void openScene(const std::string& relPath);
    // File ▸ Save As / Ctrl+Shift+S：先弹原生"另存为"对话框，再走 saveSceneAs。
    // 单独一个函数是因为它要碰 Win32（Toolbar.cpp 里那段 #ifdef）。
    void saveSceneAsDialog();

    // File ▸ Package project…（E28-1）：把当前工程打成 `dist/<Name>/`。
    // 同步执行（模板工程几 MB，一秒内完事）+ 结果走通知条；
    // 不做成"后台任务 + 进度条"是因为那需要一条任务队列，
    // 而这条命令现在的实际耗时支撑不起那套设施。
    // `asPak=true` = `assets/` + `shaders/` 收成单个 `game.pak`（G29-b/c）。
    // 默认 false（散文件）：出包后能直接翻目录核对，调试包的时候有用。
    void packageProject(bool asPak = false);

private:
    void drawMenuBar();
    void drawBar();
    void drawAboutWindow();
    void drawShortcutReference();

    // 菜单里的两个"由注册表生成"的子菜单
    void drawPanelMenu();
    void drawWorkspaceMenu();
    void drawViewModeMenu();

    // 2026-10-21："+ Add" 菜单搬到视口工具条了（见 editor/AddMenu.h）——
    // 顶栏不再有这颗按钮，也就不再需要这两个成员。

    void saveSceneAs(const std::string& relPath);

    // 工具栏分组之间的竖线（比 TextUnformatted("|") 更像 UE5 的分组线）
    static void toolbarSeparator();

    EditorContext& m_ctx;
    EditorHost& m_host;

    // ---- 工具组显隐（右键工具栏切换；落盘见 EditorLayoutIO 的 TODO）----
    struct BarGroups {
        bool transform = true;
        bool snapping = true;
        bool play = true;
        bool view = true;
        bool build = true;
    };
    BarGroups m_groups;

    // 顶栏添加菜单的摆位计数（见 nextAddSpot）

    // 两个帮助窗口
    bool m_showAbout = false;
    bool m_showShortcuts = false;
};

} // namespace editor
