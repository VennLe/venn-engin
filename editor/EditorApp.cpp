#include "EditorApp.h"

#include "ContentBrowser.h"
#include "EditorContext.h"
#include "EditorScene.h"
#include "InspectorPanel.h"
#include "SceneHierarchy.h"
#include "SplitLayout.h"
#include "Toolbar.h"
#include "ViewportPanel.h"
#include "script/ScriptEngine.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "core/WindowGeometry.h"
#include "ecs/Components.h"
#include "render/Renderer.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "ui/ImGuiManager.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace editor {

namespace {

// 布局常量（像素）。主体四个面板（Hierarchy/Content/Viewport/Inspector）
// 已交给 SplitLayout 分栏树按比例自适应；这里只保留横贯全宽的固定条
// 与浮动调试面板的尺寸常量。
constexpr float kStatusH = 26.0f;

// 浮动调试面板（Stats / Scripts）的排布：右下角**竖向码放**，互不重叠。
// 它们不走布局矩形，所以位置只能自己算；用一个 FirstUseEver 落位，
// 之后用户拖动就记住了。
constexpr float kFloatW = 360.0f;
constexpr float kFloatMargin = 12.0f;
constexpr float kScriptsH = 300.0f;
constexpr float kStatsH = 230.0f;
constexpr float kFloatGap = 8.0f;

// ---------------------------------------------------------------- 窗口几何
// 存档与 imgui.ini 一样落在当前工作目录（`make run` 会把 cwd 设到
// build/bin，双击 exe 时就是 exe 目录）—— 两者保持一致，别分两处。
constexpr const char* kWindowStateFile = "editor_window.ini";
// 第一次启动：主显示器工作区里居中、占 70%。之后按存档恢复。
constexpr float kStartupScreenFraction = 0.70f;
// 兜底尺寸（理论上只在拿不到显示器信息时用到）
constexpr int kFallbackWindowW = 1600;
constexpr int kFallbackWindowH = 900;

// 自动化跑（冒烟测试 / 截图工具）不该覆写用户存下来的窗口几何：
//   MYVK_FRAMES      —— 跑固定帧数自动退出
//   MYVK_NO_WINDOW_SAVE —— 截图工具会故意缩放窗口做压测
bool automationRun() {
    return std::getenv("MYVK_FRAMES") != nullptr ||
           std::getenv("MYVK_NO_WINDOW_SAVE") != nullptr;
}

} // namespace

// ---------------------------------------------------------------- 生命周期

EditorApp::EditorApp()
    : core::Application("Venn Editor", 1600, 900) {}

EditorApp::~EditorApp() = default;

scene::Scene& EditorApp::activeScene() {
    if (m_ctx) return m_ctx->activeScene();
    // onInit 之前的兜底（理论上不会走到）
    return core::Application::activeScene();
}

void EditorApp::onInit() {
    // 窗口几何**第一件事**就摆好：第一次跑 = 主显示器居中占 70%，
    // 之后 = 上次关闭时的位置 / 大小（见 core/WindowGeometry）。
    // 尺寸变化会置 resize 标志，Renderer 在下一帧重建交换链 —— 和用户
    // 手动拖窗口走的是同一条路，所以不用在别处再做特殊处理。
    core::applyStartupGeometry(window(), kStartupScreenFraction,
                               kWindowStateFile, kFallbackWindowW,
                               kFallbackWindowH);

    // 注意顺序：AssetManager 里存的是 renderer().textureContext() 的**拷贝**，
    // 而 TextureContext 全是裸指针 —— 必须在 Renderer::init() 之后才有效。
    // Application::run() 保证 onInit 在 init() 之后调用，所以这里安全。
    assets::AssetManager& am = assets();

    m_ctx = std::make_unique<EditorContext>(am);
    m_scripts = std::make_unique<ScriptEngine>();
    m_split = std::make_unique<SplitLayout>();
    m_toolbar = std::make_unique<Toolbar>(*m_ctx);
    m_viewport = std::make_unique<ViewportPanel>(*m_ctx, renderer());
    m_hierarchy = std::make_unique<SceneHierarchy>(*m_ctx);
    m_inspector = std::make_unique<InspectorPanel>(*m_ctx);
    m_content = std::make_unique<ContentBrowser>(*m_ctx);

    // 启动场景 = **一张白纸**：没有实体，只有一盏方向光和一个默认机位。
    // 这里以前会搭出一整套内置演示内容，2026-10-09 按用户要求连同独立的
    // Sandbox 示例程序一起删掉了 —— venn 启动不再加载任何样例。
    resetToEmptyScene(m_ctx->editorScene());
    // 视口导航相机：显式输入 + 自由飞行（UE 视口手感，见 ViewportPanel）
    m_ctx->applyEditorCameraMode();

    // ---- 自动化钩子（截图 / 回归测试；正常使用不设这两个变量就没有任何影响）----
    //   MYVK_EDITOR_SCENE=路径   启动即打开指定场景，而不是空场景
    //   MYVK_EDITOR_SELECT=名字  启动后按名字选中一个实体（让手柄/检查器有内容）
    // 有它们才能把"层级图标 / 手柄三种粒度"这类界面做成可脚本化的截图验证，
    // 否则只能靠人手点。改成别的场景在 File 菜单里一样能做，这只是个快捷入口。
    if (const char* envScene = std::getenv("MYVK_EDITOR_SCENE")) {
        if (envScene[0] != '\0') {
            const std::string abs = assets::resolveAssetPath(envScene);
            const scene::SceneIoResult r =
                scene::loadScene(m_ctx->editorScene(), am, abs);
            if (r.ok) {
                m_ctx->onSceneReplaced();
                m_ctx->scenePath() = envScene;
                VK_LOG_INFO("Editor: auto-opened scene '%s' (%zu entities)",
                            envScene, m_ctx->editorScene().objectCount());
            } else {
                VK_LOG_WARN("Editor: auto-open failed for '%s': %s", envScene,
                            r.error.c_str());
            }
        }
    }
    if (const char* envSel = std::getenv("MYVK_EDITOR_SELECT")) {
        if (envSel[0] != '\0') {
            const ecs::Entity found = m_ctx->editorScene().find(envSel);
            if (found.valid()) {
                m_ctx->select(found);
                VK_LOG_INFO("Editor: auto-selected '%s'", envSel);
            } else {
                VK_LOG_WARN("Editor: auto-select '%s' not found", envSel);
            }
        }
    }
    // MYVK_EDITOR_GIZMO=move|rotate|scale：启动就把手柄切到某个模式，
    // 这样三种粒度的界面（轴向 / 平面 / 整体）都能被截图覆盖到。
    if (const char* envGizmo = std::getenv("MYVK_EDITOR_GIZMO")) {
        const std::string g = envGizmo;
        if (g == "move")
            m_ctx->setGizmoMode(GizmoMode::Translate);
        else if (g == "rotate")
            m_ctx->setGizmoMode(GizmoMode::Rotate);
        else if (g == "scale")
            m_ctx->setGizmoMode(GizmoMode::Scale);
    }


    // 只编译、不执行：让 Scripts 面板能立刻报出脚本的语法/类型错误
    preloadScripts();

    VK_LOG_INFO("Venn editor ready: %zu entities (empty scene, edit mode)",
                m_ctx->editorScene().objectCount());

    // MYVK_EDITOR_PLAY=1：启动即进入 Play。
    // 自动化冒烟测试用 —— 配合 MYVK_FRAMES 可以在无人值守下把
    // "复制运行态 → 编译脚本 → 每帧在运行态上执行脚本 → 渲染到视口"
    // 这条链路也跑到，否则这一段只能靠人手点按钮才能验证。
    if (const char* env = std::getenv("MYVK_EDITOR_PLAY")) {
        if (env[0] == '1') {
            m_ctx->play();
            VK_LOG_INFO("Editor auto-Play (MYVK_EDITOR_PLAY=1): runtime scene "
                        "= %zu entities",
                        m_ctx->runtimeScene().objectCount());
        }
    }
}

void EditorApp::onUpdate(float dt) {
    if (m_ctx->quitRequested()) {
        window().invalidate();
        return;
    }

    // 视口接管了相机输入 —— Camera::update 里会直接 return
    m_ctx->activeScene().camera().setUseGlobalInput(false);

    // 地平面栅格的**最终开关**：编辑态 + 用户勾了才画。
    // Play 期间视口里是游戏画面，参考网格不该混进去，所以这里直接压掉 ——
    // 用户回到编辑态时（勾选框还是打勾的）它自己就回来了。
    // 真正的绘制在 Renderer 的 grid pass（最小格 1 m / 主格 10 m）。
    renderer().gridEnabled() = m_ctx->isEditing() && m_ctx->showGrid();

    m_ctx->tickNotification(dt);
    m_ctx->validateSelection();

    // ---- 脚本热重载：定时扫文件时间戳 ----
    m_hotReloadTimer += dt;
    if (m_hotReloadTimer >= 0.5f) {
        m_hotReloadTimer = 0.0f;
        const int reloaded = m_scripts->pollHotReload();
        preloadScripts();  // 场景里新增/删除了脚本也要跟上
        if (reloaded > 0) {
            m_ctx->notify("Hot-reloaded " + std::to_string(reloaded) +
                          " script file(s)");
        }
    }

    // ---- 游戏逻辑只在 Play 态跑，而且跑在**运行态副本**上 ----
    // 渲染那一边不用额外做什么：activeScene() 在 Play 期间已经返回运行态，
    // 视口自然会显示这份世界。
    if (m_ctx->isPlaying()) {
        m_ctx->advancePlayTime(dt);
        runRuntimeScripts(dt);
    } else {
        // 回到编辑态/暂停态就把帧计数归零，这样每次重新 Play
        // 都会重新打一条脚本执行汇总
        m_runtimeFrames = 0;
        m_scriptErrors.clear();
    }
}

// ---------------------------------------------------------------- 窗口收尾

// 关窗口 / 菜单 Quit / 跑满帧数都会走到这里（窗口此时还活着）。
// 只做一件事：把窗口几何存下来，下次打开接着用。
void EditorApp::onShutdown() {
    if (automationRun()) {
        VK_LOG_INFO("Editor: window geometry NOT saved (automation run)");
        return;
    }
    core::saveWindowGeometry(window(), kWindowStateFile);
}

// ---------------------------------------------------------------- 全屏同步
//
// "全屏"在编辑器里是两层含义叠在一起的：
//   ① GLFW 顶层窗口切成真全屏（铺满主显示器、无边框）
//   ② 视口独占整屏：工具栏 / 层级 / 检查器全部不画，只剩游戏画面
// 所以按下 Play 态那个 Fullscreen 按钮时，两层一起切；退出时窗口回到
// 进入全屏前的几何（70% 也好、用户拖过的大小也好）。
//
// 为什么要做边沿检测：这里每帧都会算一遍"该不该全屏"。如果无条件
// setFullscreen(want)，那么编辑态下 want=false，会把 F11 打开的纯窗口
// 全屏立刻关掉。只在值真的变化时动窗口，两个入口就不会打架。
void EditorApp::syncWindowFullscreen() {
    const bool want = !m_ctx->isEditing() && m_ctx->gameFullscreen();
    if (want == m_appliedWindowFullscreen) return;
    m_appliedWindowFullscreen = want;
    window().setFullscreen(want);
}

// F11 的落点：只切**窗口**全屏，编辑态也能用（想在大屏上看层级 / 检查器
// 又不想让游戏接管整屏时用它）。
// 规则：当前只要处于任何形式的全屏，这一下就是"全部退出"；否则进入窗口全屏。
// 不这么定的话，在"只看游戏"模式下按 F11 会被 syncWindowFullscreen 立刻
// 扳回全屏 —— 看起来像按键失灵。
void EditorApp::toggleWindowFullscreen() {
    if (window().isFullscreen()) {
        if (!m_ctx->isEditing() && m_ctx->gameFullscreen()) {
            // 走逻辑状态，让 syncWindowFullscreen 负责真正的窗口切换
            m_ctx->gameFullscreenRef() = false;
        }
        window().setFullscreen(false);
        m_appliedWindowFullscreen = false;
        return;
    }
    window().setFullscreen(true);
    m_appliedWindowFullscreen = true;
}

// ---------------------------------------------------------------- 脚本执行

// 在**运行态副本**上执行所有启用了 ScriptComponent 的实体。
// 这里与 PlayerApp 时代的那份实现完全一致，只是对象换成了内存里的
// m_runtimeScene —— 不再有跨进程的编译结果/日志分裂问题。
void EditorApp::runRuntimeScripts(float dt) {
    scene::Scene& sc = m_ctx->runtimeScene();
    ecs::World& w = sc.world();

    const double t = static_cast<double>(m_ctx->playTime());
    const uint64_t frame = time().frameCount();
    const double dtd = static_cast<double>(dt);

    int executed = 0;
    int failed = 0;

    w.each<ecs::ScriptComponent>(
        [&](ecs::Entity e, ecs::ScriptComponent& scomp) {
            if (!scomp.enabled || scomp.path.empty()) return;

            ScriptAsset* asset = m_scripts->get(scomp.path);
            if (!asset) return;

            ScriptProgram* prog = asset->program();
            if (!prog || !prog->valid()) {
                // 编译就没过：只在错误内容变化时报一次，别刷屏
                const std::string msg = asset->lastError().toString();
                auto it = m_scriptErrors.find(e.id);
                if (it == m_scriptErrors.end() || it->second != msg) {
                    m_scriptErrors[e.id] = msg;
                    m_scripts->logLine("[compile] " + scomp.path + " > " + msg);
                    VK_LOG_WARN("Script not runnable (%s): %s", scomp.path.c_str(),
                                msg.c_str());
                }
                ++failed;
                return;
            }

            ScriptHost host;
            host.scene = &sc;
            host.entity = e;

            ScriptError err;
            const double ts = static_cast<double>(scomp.timeScale);
            const bool ok = prog->run(host, t * ts, dtd * ts, frame, err);

            if (!ok) {
                const std::string msg = err.toString();
                auto it = m_scriptErrors.find(e.id);
                if (it == m_scriptErrors.end() || it->second != msg) {
                    m_scriptErrors[e.id] = msg;
                    m_scripts->logLine("[runtime] " + scomp.path + " > " + msg);
                    VK_LOG_WARN("Script runtime error (%s): %s",
                                scomp.path.c_str(), msg.c_str());
                }
                ++failed;
                return;
            }
            m_scriptErrors.erase(e.id);
            ++executed;

            // print() 的输出累积成滚动日志（run() 每次会清空 prints）
            for (const std::string& line : prog->prints()) {
                m_printLog.push_back(scomp.path + ": " + line);
            }
            if (m_printLog.size() > 200) {
                m_printLog.erase(m_printLog.begin(),
                                 m_printLog.begin() +
                                     static_cast<std::ptrdiff_t>(
                                         m_printLog.size() - 200));
            }
        });

    // Play 的第一帧打一条汇总：冒烟测试据此确认"运行态场景 + 脚本执行"
    // 这条链路真的通了（只看"没有 WARN"证明力不够）
    if (++m_runtimeFrames == 1) {
        VK_LOG_INFO("Runtime scripts: %d executed, %d failed (t=%.3fs)",
                    executed, failed, static_cast<double>(m_ctx->playTime()));
    }
}

// ---------------------------------------------------------------- 脚本预编译

void EditorApp::preloadScripts() {
    // 收集编辑态场景里引用到的脚本路径
    std::vector<std::string> paths;
    m_ctx->editorScene().world().each<ecs::ScriptComponent>(
        [&](ecs::Entity, ecs::ScriptComponent& s) {
            if (!s.enabled || s.path.empty()) return;
            if (std::find(paths.begin(), paths.end(), s.path) == paths.end())
                paths.push_back(s.path);
        });
    std::sort(paths.begin(), paths.end());

    // 集合没变就不动缓存（否则每 0.5s 把编译结果丢掉重来一遍）
    if (paths == m_scriptPaths) return;
    m_scriptPaths = paths;

    m_scripts->clear();
    int failed = 0;
    for (const std::string& p : m_scriptPaths) {
        ScriptAsset* a = m_scripts->get(p);
        if (a && !a->lastError().ok()) {
            ++failed;
            m_scripts->logLine("[compile] " + p + " > " +
                               a->lastError().toString());
        }
    }
    VK_LOG_INFO("Editor: precompiled %zu script(s), %d with errors",
                m_scriptPaths.size(), failed);
}

// ---------------------------------------------------------------- 布局

void EditorApp::computeLayout() {
    const ImGuiIO& io = ImGui::GetIO();
    // 分栏布局器按窗口尺寸自适应：主体四个面板（Hierarchy / Content /
    // Viewport / Inspector）由分栏树产出，窗口缩放时按比例伸缩；
    // 分栏交界处可拖动调整，侧栏可拖到 0 折叠（视口优先）。
    // 工具栏高度是用户可调的（设置面板），每帧同步进布局器。
    m_split->setToolbarHeight(m_ctx->toolbarHeight());
    m_split->beginFrame(io.DisplaySize.x, io.DisplaySize.y);
    m_split->apply(m_ctx->layout());
}

// ---------------------------------------------------------------- 快捷键

void EditorApp::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    // 正在文本输入（重命名 / 脚本路径）时不要抢键
    if (io.WantTextInput) return;

    // ---- F11：窗口全屏（任何模式都可用，面板照旧显示）----
    // 与 Play 态的 F 分工：F = "全屏 + 只看游戏"，F11 = 只是把窗口铺满显示器。
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) toggleWindowFullscreen();

    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) m_ctx->undo();
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) m_ctx->redo();
        // Ctrl+D = 复制选中项（UE5 也是这个键）
        if (ImGui::IsKeyPressed(ImGuiKey_D, false)) m_hierarchy->duplicateSelection();
        // Ctrl+N = 新建空场景
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) m_toolbar->newScene();
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            const std::string abs =
                assets::resolveAssetPath(m_ctx->scenePath());
            const scene::SceneIoResult r =
                scene::saveScene(m_ctx->editorScene(), abs);
            if (r.ok) {
                m_ctx->dirty() = false;
                m_ctx->notify("Saved " + m_ctx->scenePath());
            } else {
                m_ctx->notify("Save failed: " + r.error);
            }
        }
        return;  // 带 Ctrl 的组合键到此为止
    }

    // 手柄模式（和主流编辑器一致）。
    // **按住右键飞行时让位**：这时 W / E 是"前进 / 上升"（见
    // ViewportPanel::handleNavigation），不该顺手把手柄切成平移 / 旋转模式。
    // UE 也是这么分的 —— 字母键只在没在"驾驶"相机时才是工具快捷键。
    const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!flying) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            m_ctx->setGizmoMode(GizmoMode::Translate);
        if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            m_ctx->setGizmoMode(GizmoMode::Rotate);
        if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            m_ctx->setGizmoMode(GizmoMode::Scale);
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            m_ctx->setGizmoSpace(m_ctx->gizmoSpace() == GizmoSpace::World
                                     ? GizmoSpace::Local
                                     : GizmoSpace::World);
        }
        // G：切换**当前模式**的吸附（平移 / 旋转 / 缩放各自独立，
        // 和工具栏第二行那三个勾选框是同一份状态）。
        if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
            m_ctx->toggleSnapForCurrentMode();
            m_ctx->notify(m_ctx->snapForCurrentMode() ? "Snapping ON"
                                                      : "Snapping OFF");
        }
    }

    // 播放 / 暂停
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) m_ctx->togglePlayPause();

    // ---- 运行态：F = 游戏全屏 / 恢复，Esc = 退出全屏，其余编辑快捷键失效 ----
    // 注意 F 在编辑态里是"聚焦选中项"，两种语义按 Play 状态分开，
    // 不会互相抢键。
    if (!m_ctx->isEditing()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) m_ctx->toggleGameFullscreen();
        if (m_ctx->gameFullscreen() &&
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            m_ctx->gameFullscreenRef() = false;
        }
        return;
    }

    if (!m_ctx->hasSelection()) return;

    // 聚焦（Esc 顺手取消选择）
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        scene::Scene& sc = m_ctx->editorScene();
        sc.camera().setTarget(glm::vec3(sc.worldMatrix(m_ctx->selection())[3]));
    }
    // F2：就地重命名（Hierarchy 面板里会换成输入框）。只在编辑态有效。
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) m_hierarchy->beginRenameSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_ctx->clearSelection();

    // 删除
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_ctx->isEditing()) {
        const ecs::Entity victim = m_ctx->selection();
        const std::string* np = m_ctx->editorScene().world().name(victim);
        const std::string label =
            "Delete " + (np ? *np : std::string("Entity"));

        scene::Scene& sc = m_ctx->editorScene();
        m_ctx->structuralEdit(label, [&]() {
            ecs::World& w = sc.world();
            if (auto* h = w.get<ecs::HierarchyComponent>(victim)) {
                if (h->parent != ecs::kInvalidEntity) {
                    const ecs::Entity parent = w.handle(h->parent);
                    if (auto* ph = w.get<ecs::HierarchyComponent>(parent)) {
                        auto& kids = ph->children;
                        kids.erase(std::remove(kids.begin(), kids.end(),
                                               victim.id),
                                   kids.end());
                    }
                }
            }
            w.destroy(victim);
        });
        m_ctx->validateSelection();
    }
}

// ---------------------------------------------------------------- UI 编排

void EditorApp::onImGui() {
    handleShortcuts();

    // View 菜单里的 "Toggle Fullscreen (F11)"：菜单在面板之后就画了，
    // 置位当帧处理掉即可（下一帧才生效，肉眼无感）
    if (m_ctx->windowFullscreenRequest()) {
        m_ctx->windowFullscreenRequest() = false;
        toggleWindowFullscreen();
    }

    // 快捷键/按钮刚可能改过"游戏全屏" → 这一帧就把窗口切过去
    syncWindowFullscreen();

    // 字体缩放：设置面板里拖的字号在这里落到 ImGui（运行时即时生效）。
    // 放最前面，本帧所有 UI（含布局计算）都用新字号。
    renderer().ui().setFontScale(m_ctx->fontScale());

    // 游戏全屏优先：这一帧只画视口，其余面板一个都不创建。
    if (drawFullscreenGame()) return;

    computeLayout();

    m_toolbar->draw();
    m_hierarchy->draw();
    m_inspector->draw();
    m_content->draw();
    m_viewport->draw();

    if (m_ctx->panels().stats) drawStatsPanel();
    if (m_ctx->panels().scripts) drawScriptPanel();
    if (m_ctx->panels().engine) drawEnginePanel();
    if (m_ctx->showSettings()) drawSettingsPanel();

    drawStatusBar();

    // 分栏分隔条：在**所有面板之后**处理 —— 分隔线画在前景层（盖在面板上），
    // 拖拽用鼠标位置手动判断（不依赖面板窗口层级）
    m_split->handleSplitters();

    // "Reset Layout"：重置分栏比例
    if (m_ctx->relayout()) {
        m_split->reset();
        m_ctx->relayout() = false;
    }
}

// ---------------------------------------------------------------- 游戏全屏

// Play（含暂停）期间让视口独占整个窗口 —— 这就是"独立游戏窗口"想要的效果，
// 只是没有任何跨窗口/跨进程代价：同一张交换链、同一条离屏渲染链，只是
// 布局矩形变成了整屏，别的面板不画而已。
//
// 菜单栏/工具栏/状态栏这一帧都不创建，所以 L 的其它字段用不上；退出全屏后
// computeLayout() 会立刻重算，不需要在这里恢复任何东西。
bool EditorApp::drawFullscreenGame() {
    if (m_ctx->isEditing() || !m_ctx->gameFullscreen()) return false;

    const ImGuiIO& io = ImGui::GetIO();
    LayoutRects& L = m_ctx->layout();
    L.viewport = {0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};

    // 视口面板可能被用户在 View 菜单里关掉过 —— 全屏时强制打开，否则整屏
    // 空白、用户会以为崩了。
    m_ctx->panels().viewport = true;

    m_viewport->draw();
    return true;
}

// ---------------------------------------------------------------- 状态栏

void EditorApp::drawStatusBar() {
    const LayoutRects& L = m_ctx->layout();
    ImGui::SetNextWindowPos(ImVec2(L.status.x, L.status.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.status.w, L.status.h),
                             ImGuiCond_Always);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    if (!ImGui::Begin("##statusbar", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    ImGui::TextUnformatted(m_ctx->status().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("| history %zu/%zu",
                        m_ctx->commands().cursor(),
                        m_ctx->commands().depth());

    if (m_ctx->hasSelection()) {
        ImGui::SameLine();
        const std::string* np = m_ctx->activeScene().world().name(
            m_ctx->selection());
        ImGui::TextDisabled("| selected: %s", np ? np->c_str() : "(unnamed)");
    }

    // 播放状态 + 实体计数：以前挤在工具条最右端（那条工具栏本来就很长，
    // 一起被挤出右边缘裁掉），挪到状态栏反而更合理 —— 这里本来就是
    // "一眼看全局状态"的地方。
    ImGui::SameLine();
    const bool playing = m_ctx->isPlaying();
    const bool paused = m_ctx->isPaused();
    const char* stateText = playing ? (m_ctx->gameFullscreen()
                                           ? "PLAYING (fullscreen)"
                                           : "PLAYING (in viewport)")
                                    : (paused ? "PAUSED" : "EDITING");
    const ImVec4 stateCol = playing ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
                                    : (paused ? ImVec4(0.95f, 0.8f, 0.4f, 1.0f)
                                              : ImVec4(0.62f, 0.67f, 0.74f, 1.0f));
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextColored(stateCol, "%s", stateText);
    ImGui::SameLine();
    ImGui::TextDisabled("| %zu entities (editor %zu / runtime %zu)",
                        m_ctx->activeScene().objectCount(),
                        m_ctx->editorScene().objectCount(),
                        m_ctx->runtimeScene().objectCount());

    // 瞬时提示（3 秒）：靠右显示
    if (!m_ctx->notification().empty()) {
        const float w = ImGui::CalcTextSize(m_ctx->notification().c_str()).x;
        ImGui::SameLine(std::max(ImGui::GetWindowWidth() - w - 20.0f, 300.0f));
        const float a = std::min(m_ctx->notificationTimer() / 0.6f, 1.0f);
        ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.6f, a), "%s",
                           m_ctx->notification().c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- 统计面板

void EditorApp::drawStatsPanel() {
    render::Renderer& r = renderer();
    const ImGuiIO& io = ImGui::GetIO();

    // 浮在 Scripts 面板上方（两者都在右下角），同时打开也不互相压住
    const float x = io.DisplaySize.x - kFloatW - kFloatMargin;
    const float y = io.DisplaySize.y - kStatusH - kScriptsH - kFloatGap - kStatsH;

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kFloatW, 0.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Stats", &m_ctx->panels().stats)) {
        ImGui::End();
        return;
    }

    ImGui::Text("FPS: %.1f  (%.2f ms)", io.Framerate,
                io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);

    const scene::Scene& sc = m_ctx->activeScene();
    ImGui::Text("Viewport scene: %s",
                m_ctx->isEditing() ? "EDITOR" : "RUNTIME (playing)");
    ImGui::Text("Entities: %zu", sc.objectCount());

    const scene::Scene::LightStats ls = sc.lightStats();
    ImGui::Text("Local lights: %zu (%zu point / %zu spot)",
                ls.pointLights + ls.spotLights, ls.pointLights, ls.spotLights);

    ImGui::Separator();
    const VkExtent2D re = r.renderExtent();
    ImGui::Text("Render target: %ux%u%s", re.width, re.height,
                r.viewportActive() ? " (offscreen -> ImGui)" : " (swapchain)");
    ImGui::Text("MSAA: %ux", static_cast<unsigned>(r.sampleCount()));
    ImGui::Text("Depth prepass: %s", r.depthPrePassEnabled() ? "on" : "off");
    ImGui::TextWrapped("GPU: %s", r.device().properties().deviceName);

    ImGui::Separator();
    // 播放状态（现在游戏就跑在视口里，没有第二个进程/窗口）
    const char* pbState = m_ctx->isEditing() ? "stopped"
                           : m_ctx->isPaused() ? "PAUSED"
                                               : "PLAYING";
    const ImVec4 pbCol = m_ctx->isEditing()
                             ? ImVec4(0.6f, 0.65f, 0.7f, 1.0f)
                         : m_ctx->isPaused()
                             ? ImVec4(0.95f, 0.8f, 0.4f, 1.0f)
                             : ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
    ImGui::TextColored(pbCol, "Playback: %s (in viewport)%s", pbState,
                       (m_ctx->gameFullscreen() && !m_ctx->isEditing())
                           ? "  [fullscreen]"
                           : "");
    if (!m_ctx->isEditing()) {
        ImGui::Text("Game time: %.2fs",
                    static_cast<double>(m_ctx->playTime()));
        ImGui::TextDisabled("Camera owned by the game (viewport takes no input)");
    }
    // 视口导航灵敏度：编辑态相机的移动 / 旋转系数（右上角 HUD 可改）
    ImGui::Text("View sensitivity: x%.2f",
                static_cast<double>(m_ctx->navSensitivity()));
    ImGui::Text("Editor %zu / runtime %zu entities",
                m_ctx->editorScene().objectCount(),
                m_ctx->runtimeScene().objectCount());
    ImGui::Text("Scripts loaded: %zu", m_scripts->scriptCount());

    ImGui::End();
}

// ---------------------------------------------------------------- 脚本面板

void EditorApp::drawScriptPanel() {
    const ImGuiIO& io = ImGui::GetIO();
    const float x = io.DisplaySize.x - kFloatW - kFloatMargin;
    const float y = io.DisplaySize.y - kStatusH - kScriptsH;

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kFloatW, kScriptsH), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Scripts", &m_ctx->panels().scripts)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Language: .vks (see editor/script/ScriptEngine.h)");
    if (ImGui::Button("Reload all")) {
        const int n = m_scripts->pollHotReload();
        for (const std::string& p : m_scripts->loadedPaths()) {
            m_scripts->reloadNow(p);
        }
        m_ctx->notify("Reloaded " + std::to_string(n) +
                      " changed script(s) + forced all");
    }
    ImGui::SameLine();
    ImGui::Text("hot-reload scan: 0.5s");

    ImGui::Separator();
    if (ImGui::BeginTabBar("##scripttabs")) {
        if (ImGui::BeginTabItem("Loaded")) {
            for (const std::string& p : m_scripts->loadedPaths()) {
                ScriptAsset* a = m_scripts->get(p);
                if (!a) continue;
                const bool bad = !a->lastError().ok();
                ImGui::TextColored(bad ? ImVec4(1.0f, 0.5f, 0.45f, 1.0f)
                                       : ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                                   "%s  (#%llu)", p.c_str(),
                                   static_cast<unsigned long long>(
                                       a->reloadCount()));
                if (bad) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("<- %s",
                                        a->lastError().toString().c_str());
                }
                ImGui::SameLine();
                ImGui::PushID(p.c_str());
                if (ImGui::SmallButton("reload")) m_scripts->reloadNow(p);
                ImGui::PopID();
            }
            if (m_scripts->scriptCount() == 0)
                ImGui::TextDisabled("(the scene references no scripts)");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) {
            ImGui::BeginChild("##log");
            const auto& log = m_scripts->log();
            for (const std::string& s : log) ImGui::TextWrapped("%s", s.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("How it works")) {
            ImGui::BeginChild("##howto");
            ImGui::TextWrapped(
                "In edit mode scripts are only PRE-COMPILED: syntax errors "
                "show up immediately, but nothing runs and nothing animates.");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Press Play and they execute every frame on the runtime copy "
                "of the scene - which is exactly what the viewport renders. "
                "Stop discards that copy, so the scene you edited is never "
                "touched.");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Hot reload: save the .vks file and it is picked up within "
                "0.5s, even while playing.");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// ---------------------------------------------------------------- 渲染设置

void EditorApp::drawEnginePanel() {
    render::Renderer& r = renderer();
    // Engine 面板内容很长，和 Scripts 共用右下角那一格（二者都是按需打开的
    // 调试面板，实际很少同时开）。打开顺序上 Engine 最后绘制，所以它会
    // 盖在 Scripts 之上 —— 想要并排看，拖动一下即可。
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x - kFloatW - kFloatMargin,
               io.DisplaySize.y - kStatusH - kScriptsH),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kFloatW, kScriptsH), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Engine", &m_ctx->panels().engine)) {
        ImGui::End();
        return;
    }

    auto& post = r.postSettings();
    ImGui::SeparatorText("Post Processing");
    ImGui::SliderFloat("Exposure", &post.exposure, 0.05f, 4.0f, "%.2f");
    const char* tonemaps[] = {"ACES", "Reinhard", "Clamp (raw HDR)"};
    ImGui::Combo("Tonemap", &post.tonemapMode, tonemaps, 3);
    ImGui::SliderFloat("Vignette", &post.vignette, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Bloom threshold", &post.bloomThreshold, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SliderFloat("Bloom knee", &post.bloomKnee, 0.01f, 2.0f, "%.2f");
    ImGui::SliderFloat("Bloom strength", &post.bloomStrength, 0.0f, 2.0f,
                       "%.2f");

    ImGui::SeparatorText("Clustered Forward");
    ImGui::Checkbox("Cluster culling", &r.clusterCullingEnabled());
    ImGui::SameLine();
    ImGui::Checkbox("Depth prepass", &r.depthPrePassEnabled());
    ImGui::Checkbox("Light gizmos", &r.lightGizmosEnabled());
    if (r.lightGizmosEnabled()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        ImGui::DragFloat("##gizmo", &r.lightGizmoScale(), 0.002f, 0.01f, 0.25f,
                         "%.3f");
    }

    // MSAA：切换会重建 HdrTarget 与前向管线（视口离屏链会被保留下来，
    // 所以编辑器纹理 ID 不会失效 —— 见 PostProcess::recreate 的说明）
    static const int kMsaaItems[4] = {1, 2, 4, 8};
    int msaaIdx = 2;
    for (int i = 0; i < 4; ++i) {
        if (static_cast<unsigned>(kMsaaItems[i]) ==
            static_cast<unsigned>(r.sampleCount())) {
            msaaIdx = i;
        }
    }
    const char* kMsaaLabels[4] = {"1x (off)", "2x", "4x", "8x"};
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::Combo("MSAA##samples", &msaaIdx, kMsaaLabels, 4)) {
        VkSampleCountFlagBits bits = VK_SAMPLE_COUNT_4_BIT;
        switch (kMsaaItems[msaaIdx]) {
            case 1: bits = VK_SAMPLE_COUNT_1_BIT; break;
            case 2: bits = VK_SAMPLE_COUNT_2_BIT; break;
            case 8: bits = VK_SAMPLE_COUNT_8_BIT; break;
            default: bits = VK_SAMPLE_COUNT_4_BIT; break;
        }
        r.setSampleCount(bits);
    }

    // ---- 地平面栅格（参数）----
    // 开关**不在这里**：它由视口右上角的浮层 / View 菜单控制，并且每帧
    // 由 onUpdate 按"编辑态"同步给渲染器（Play 期间强制关）。这个面板
    // 只放那些"调起来才知道合不合适"的细节参数。
    ImGui::SeparatorText("Viewport grid (y = 0)");
    {
        render::GridSettings& g = r.gridSettings();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Minor step (m)", &g.minorStep, 0.05f, 0.1f, 10.0f,
                         "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Major (m)", &g.majorStep, 0.5f, 1.0f, 100.0f, "%.1f");
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Extent (m)", &g.extent, 1.0f, 5.0f, 500.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Alpha", &g.alpha, 0.01f, 0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Line width (px)", &g.lineWidthPx, 0.05f, 0.5f, 5.0f,
                         "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Fade end (m)", &g.fadeEnd, 1.0f, 5.0f, 300.0f, "%.0f");
        ImGui::TextDisabled("Toggle: viewport top-right HUD / View menu");
    }

    ImGui::SeparatorText("Cluster stats");
    const render::ClusterStats& cs = r.clusteredLighting().stats();
    const render::ClusterConfig& cfg = r.clusteredLighting().config();
    ImGui::Text("Grid: %u x %u x %u (tile %u px)", cs.clusterX, cs.clusterY,
                cs.clusterZ, cfg.tileSize);
    ImGui::Text("Lights/cluster: avg %.2f  max %u / %u",
                cs.avgLightsPerCluster(), cs.maxLightsInCluster,
                cfg.maxLightsPerCluster);
    ImGui::Text("Empty clusters: %u", cs.emptyClusters);
    if (cs.overflowed())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                           "Overflow: %u clusters truncated!",
                           cs.overflowClusters);

    ImGui::End();
}

// ---------------------------------------------------------------- 设置

void EditorApp::drawSettingsPanel() {
    // 设置弹窗：字体缩放 + 工具栏高度。居中显示，带确定/关闭按钮。
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f,
                                   io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Always);

    if (!ImGui::Begin("Settings###settings", &m_ctx->showSettings(),
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Interface");

    // ---- 字体缩放 ----
    float fontScale = m_ctx->fontScale();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    if (ImGui::SliderFloat("Font scale", &fontScale, 0.75f, 2.0f, "%.2f")) {
        m_ctx->fontScale() = fontScale;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%.0f%%)", fontScale * 100.0f);

    // 快速恢复按钮
    if (ImGui::SmallButton("Reset to 1.00")) m_ctx->fontScale() = 1.0f;
    ImGui::SameLine();
    if (ImGui::SmallButton("+0.10")) {
        m_ctx->fontScale() = std::min(2.0f, m_ctx->fontScale() + 0.10f);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("-0.10")) {
        m_ctx->fontScale() = std::max(0.75f, m_ctx->fontScale() - 0.10f);
    }

    ImGui::Separator();

    // ---- 工具栏高度 ----
    float toolbarH = m_ctx->toolbarHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    if (ImGui::SliderFloat("Toolbar height", &toolbarH, 42.0f, 84.0f, "%.0f px")) {
        m_ctx->toolbarHeight() = toolbarH;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("px");

    if (ImGui::SmallButton("Reset to 46")) m_ctx->toolbarHeight() = 46.0f;

    ImGui::Spacing();
    ImGui::TextDisabled("Font scale applies immediately (global).");
    ImGui::TextDisabled("Toolbar height adjusts the play-control bar\n"
                        "(44 px is the tightest that still fits the buttons).");

    ImGui::Separator();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 90.0f);
    if (ImGui::Button("Close", ImVec2(80.0f, 0.0f)))
        m_ctx->showSettings() = false;

    ImGui::End();
}

} // namespace editor
