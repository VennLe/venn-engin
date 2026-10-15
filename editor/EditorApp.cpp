#include "EditorApp.h"

#include "CharacterWindow.h"
#include "CommandRegistry.h"
#include "ContentBrowser.h"
#include "DebugRects.h"
#include "EditorContext.h"
#include "EditorLayoutIO.h"
#include "ConsolePanel.h"
#include "FeatureUIGen.h"
// E21：通知 / 进度（构造即接管 Logger 的 hook）。
#include "NotificationCenter.h"
#include "GpuTimelinePanel.h"
// E09：生成区直接遍历 kFeatures / paramPtr（FeatureUIGen.h 只前向声明）
#include "render/FeatureRegistry.h"
#include "EditorScene.h"
#include "InspectorPanel.h"
#include "PanelRegistry.h"
#include "SceneHierarchy.h"
#include "SplitLayout.h"
#include "Toolbar.h"
#include "ViewportPanel.h"
#include "Workspace.h"
#include "script/ScriptEngine.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "core/WindowGeometry.h"
#include "ecs/Components.h"
#include "physics/CollisionWorld.h"
#include "render/Renderer.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "ui/ImGuiManager.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace editor {

namespace {

// 布局常量（像素）。主体四个面板（Hierarchy/Content/Viewport/Inspector）
// 已交给 SplitLayout 分栏树按比例自适应；这里只保留横贯全宽的固定条
// 与浮动调试面板的尺寸常量。
constexpr float kStatusH = 26.0f;
// 视口正下方播放条（Run / Play / Pause / Stop）的高度。
// "刚刚好一行"：一行按钮（28px）+ 上下各 3px 内边距。
constexpr float kTransportH = 34.0f;
// 视口最小渲染尺寸（与 ViewportPanel 的同名常量保持一致）
constexpr float kMinViewport = 64.0f;

// 运行态相机的碰撞半径（米）。
//
// 相机碰撞是"球 vs 凸体"：这个球必须比 0 大，否则相机是个点 —— 点在
// 凸体内部才有穿透可言，贴着墙面走时就会一半身体插进墙里。取 0.25 米
// 大约是"人眼到墙的舒适距离"，也和近平面（0.1）一个量级。
constexpr float kPlayCameraRadius = 0.25f;

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

// 引擎版本号（Help ▸ About 显示）。与 CMakeLists 的
// `project(Venn VERSION x.y.z)` 对齐 —— 那里没有把版本编进代码，
// 所以这里是唯一的事实源；改版本时两处一起改。
constexpr const char* kVennVersion = "0.2.0";

// 自动化跑（冒烟测试 / 截图工具）不该覆写用户存下来的窗口几何：
//   MYVK_FRAMES      —— 跑固定帧数自动退出
//   MYVK_NO_WINDOW_SAVE —— 截图工具会故意缩放窗口做压测
bool automationRun() {
    return std::getenv("MYVK_FRAMES") != nullptr ||
           std::getenv("MYVK_NO_WINDOW_SAVE") != nullptr;
}

// 数字键 1..9 的 ImGuiKey —— 面板显隐（Alt+N）与工作区切换（Ctrl+Alt+N）
// 两处共用，别再各写一份。
const ImGuiKey kDigitKeys[9] = {
    ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5,
    ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9};

} // namespace

// ============================================================
// E04 · 面板表（"加一个面板只加一行"的那一行）
// ============================================================
// 一条表项派生四路：
//   · View / Window 菜单（Toolbar::drawMenuBar 遍历表）
//   · 快捷键（handleShortcuts 遍历表匹配）
//   · 绘制派发（drawPanels 遍历表）
//   · 持久化（EditorLayoutIO 按 id 存取）
//
// ⚠ 快捷键一律 **Alt+数字**，不用 Ctrl+数字：Ctrl+1/3/7 与 Ctrl+2/4/6/8
//   已经被视口的 Blender 那套视图方向键占了（ViewportPanel::
//   handleViewShortcuts），再叠一层就变成"一按下去两个都响应"。
std::vector<PanelDesc> EditorApp::buildPanelTable() {
    return {
        // id          title             group                 shortcut  icon      defaultOpen floating layoutSlot     draw
        {"hierarchy", "Hierarchy", PanelGroup::General, "Alt+1", "tree",
         true, false, kSlotHierarchy, &EditorApp::panelDrawHierarchy},
        {"inspector", "Inspector", PanelGroup::General, "Alt+2", "sliders",
         true, false, kSlotInspector, &EditorApp::panelDrawInspector},
        {"content", "Content Browser", PanelGroup::General, "Alt+3", "folder",
         true, false, kSlotContent, &EditorApp::panelDrawContent},
        {"viewport", "Viewport", PanelGroup::General, "Alt+4", "camera",
         true, false, kSlotViewport, &EditorApp::panelDrawViewport},
        {"stats", "Stats", PanelGroup::Debug, "Alt+5", "chart",
         false, true, kSlotNone, &EditorApp::panelDrawStats},
        {"scripts", "Scripts", PanelGroup::Debug, "Alt+6", "code",
         false, true, kSlotNone, &EditorApp::panelDrawScripts},
        {"engine", "Engine", PanelGroup::Authoring, "Alt+7", "gear",
         false, true, kSlotNone, &EditorApp::panelDrawEngine},
        // ↓ 加面板 = 加这样一行。E16（Console）已把这一行填满 ——
        //   菜单 / 快捷键 / 持久化 / 绘制派发四路全部自动跟上。
        {"console", "Console", PanelGroup::Debug, "Alt+8", "terminal",
         false, true, kSlotNone, &EditorApp::panelDrawConsole},
        // E25：GPU 时戳时间轴。归 Authoring 组（"调参类"），并且由
        // Rendering 工作区默认打开 —— 见 Workspace.cpp 的 kRenderOpen。
        {"gputimeline", "GPU Timeline", PanelGroup::Authoring, "Alt+9",
         "timer", false, true, kSlotNone, &EditorApp::panelDrawGpuTimeline},
    };
}

const std::vector<PanelDesc>& EditorApp::panelTableVec() {
    static const std::vector<PanelDesc> kPanels = buildPanelTable();
    return kPanels;
}

// PanelRegistry 只认这两个自由函数（它不 include EditorApp.h）
const PanelDesc* panelTable() { return EditorApp::panelTableVec().data(); }
int panelCount() { return static_cast<int>(EditorApp::panelTableVec().size()); }

// ---- 表项的 draw 入口（静态薄壳：把 EditorApp& 转成成员调用）----
void EditorApp::panelDrawHierarchy(EditorApp& a) { a.m_hierarchy->draw(); }
void EditorApp::panelDrawContent(EditorApp& a) { a.m_content->draw(); }
void EditorApp::panelDrawInspector(EditorApp& a) { a.m_inspector->draw(); }
void EditorApp::panelDrawViewport(EditorApp& a) { a.m_viewport->draw(); }
void EditorApp::panelDrawStats(EditorApp& a) { a.drawStatsPanel(); }
void EditorApp::panelDrawScripts(EditorApp& a) { a.drawScriptPanel(); }
void EditorApp::panelDrawEngine(EditorApp& a) { a.drawEnginePanel(); }
void EditorApp::panelDrawConsole(EditorApp& a) { a.drawConsolePanel(); }
void EditorApp::panelDrawGpuTimeline(EditorApp& a) {
    a.drawGpuTimelinePanel();
}

void EditorApp::drawPlaceholderPanel(int panelIndex) {
    const char* title = panelTitle(panelIndex);
    const char* id = panelId(panelIndex);

    char win[160];
    std::snprintf(win, sizeof(win), "%s###panel_%s", title, id);

    ImGui::SetNextWindowSize(ImVec2(kFloatW, 150.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(kFloatMargin, kFloatMargin + kStatsH + kScriptsH +
                                                    kFloatGap * 2.0f),
                            ImGuiCond_FirstUseEver);

    bool open = panelOpen(panelIndex);
    if (!ImGui::Begin(win, &open)) {
        ImGui::End();
        // 关掉要把状态写回去（否则菜单里的勾还在）
        if (!open) setPanelOpen(panelIndex, false);
        return;
    }
    ImGui::TextDisabled("%s", title);
    ImGui::Separator();
    ImGui::TextWrapped(
        "这块面板已经在注册表里登记了（id = \"%s\"）：\n"
        "View/Window 菜单、快捷键、显隐持久化三路都已生效。\n"
        "内容由后续任务填充（见 docs/editor_roadmap.md）。",
        id);
    ImGui::End();
    if (!open) setPanelOpen(panelIndex, false);
}

void EditorApp::drawPanels() {
    const std::vector<PanelDesc>& t = panelTableVec();
    for (int i = 0; i < static_cast<int>(t.size()); ++i) {
        if (!panelOpen(i)) continue;
        const PanelDesc& p = t[i];
        // 分栏树里的面板：被"最大化"顶掉的那三个不画
        if (p.layoutSlot != kSlotNone && !m_split->showsInLayout(p.layoutSlot)) {
            continue;
        }
        if (p.draw) {
            p.draw(*this);
        } else if (p.layoutSlot == kSlotNone) {
            drawPlaceholderPanel(i);
        }
    }
}

// ---------------------------------------------------------------- 生命周期

EditorApp::EditorApp()
    : core::Application("Venn Editor", 1600, 900) {}

EditorApp::~EditorApp() {
    // E21：卸掉 Logger 的 hook。通知中心先于引擎销毁，留着会变成悬垂指针
    // （下一次 VK_LOG_* 就会写进已释放的对象）。
    if (activeNotificationCenter() == m_notifications.get()) {
        core::setLogHook(nullptr);
        attachNotificationCenter(nullptr);
    }
}

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
    //
    // ⚠ 自动化跑（MYVK_FRAMES / MYVK_NO_WINDOW_SAVE）**读也别读**：存档
    //   可能是上次手动会话留下的全屏几何（3840x2054），而脚本启动后马上
    //   会把窗口缩到自己的尺寸 —— 首帧按全屏居中的 Appearing 窗口
    //   （Preferences 等）就会停在旧坐标上，缩窗后"飞到"客户区角落
    //   （实测踩过）。脚本随后自己 SetWindowPos，用默认 1600x900 即可。
    if (!automationRun()) {
        core::applyStartupGeometry(window(), kStartupScreenFraction,
                                   kWindowStateFile, kFallbackWindowW,
                                   kFallbackWindowH);
    }

    // 注意顺序：AssetManager 里存的是 renderer().textureContext() 的**拷贝**，
    // 而 TextureContext 全是裸指针 —— 必须在 Renderer::init() 之后才有效。
    // Application::run() 保证 onInit 在 init() 之后调用，所以这里安全。
    assets::AssetManager& am = assets();

    m_ctx = std::make_unique<EditorContext>(am);
    // Engine 面板默认收起；自动化按需打开（MYVK_ENGINE_PANEL=1）：
    // 面板里的 View mode（R0-2）/ MSAA 下拉是"控件存在性"验证的对象
    // （VM-COMBO 标签）。不能用 automationRun() 一刀切 —— 面板里的
    // "Lights/cluster: avg %.2f" 每帧微变，会毁掉逐像素对照轮。
    if (std::getenv("MYVK_ENGINE_PANEL")) {
        m_ctx->panels().engine = true;
    }
    m_scripts = std::make_unique<ScriptEngine>();
    m_split = std::make_unique<SplitLayout>();
    m_toolbar = std::make_unique<Toolbar>(*m_ctx, *this);
    // E02：生成器写完参数值后回调这里（FTR-UI 日志 + 副作用）。
    // EditorApp 与应用同生命周期，捕获 this 是安全的。
    editor::setParamChangeHook(
        [this](const char* id) { onFeatureParamChanged(id); });
    // E19：参数编辑**结束**时记一条可撤销的设置改动（拖动中是每帧改值，
    // 只有"失活"才是用户心里的一次操作）。
    editor::setParamUndoHook(
        [this](const char* id, const void* before, const void* after,
               std::size_t size) {
            onFeatureParamUndo(id, before, after, size);
        });
    // E16：Console 面板（构造即挂 Logger sink，日志开始汇入环形缓冲）。
    m_console = std::make_unique<editor::ConsolePanel>();
    // E25：GPU 时戳时间轴。纯读 GpuTimer（R0-1），构造/析构无副作用 ——
    // 与 Console 的顺序无关，唯一的前提是"点段联动 Console"要有 Console
    // 可写（由 draw 时的入参保证，不在构造期绑定）。
    m_gpuTimeline = std::make_unique<editor::GpuTimelinePanel>();
    // E21：通知中心 —— 接管 Logger 的**第二条**通道（hook）。
    // ⚠ 顺序：必须先有 Console 再建通知中心。两条通道本身互不依赖，但
    //   "点 Error 的 toast → 打开 Console 滚到那条"这个动作要有 Console 可开。
    m_notifications = std::make_unique<editor::NotificationCenter>();
    attachNotificationCenter(m_notifications.get());
    core::setLogHook(&notificationLogHook);
    m_notifications->loadFromDisk();
    registerConsoleCommands();
    // E17：偏好读盘（无存档 / 自动化 → 出厂值；见 EditorLayoutIO 的守卫）
    applyPrefsFromDisk();
    m_viewport = std::make_unique<ViewportPanel>(*m_ctx, renderer());
    m_hierarchy = std::make_unique<SceneHierarchy>(*m_ctx);
    m_inspector = std::make_unique<InspectorPanel>(*m_ctx);
    m_content = std::make_unique<ContentBrowser>(*m_ctx);
    m_characterWindow = std::make_unique<CharacterWindow>(*m_ctx, renderer());

    // E18：命令注册表（命令面板 / 快捷键表 / 总览共用）。必须在所有面板
    // 与 Renderer 都就绪之后 —— 命令的 handler 会摸它们。
    registerEditorCommands();

    // ---- E04 / E05：读回上次的布局、工作区、视口布局 ----
    // ⚠ 必须排在下面所有 MYVK_EDITOR_* 钩子**之前**：那些钩子是用户显式
    //   指定的，优先级高于存档（否则自动化会被上一次手动会话的布局干扰）。
    //   文件不存在 / 自动化运行时本函数**什么都不改**（保持出厂）。
    loadEditorLayoutFromDisk();
    // 读盘会把面板显隐整份覆盖掉 → 显式 env 钩子要在它之后重新落一次
    if (std::getenv("MYVK_ENGINE_PANEL")) m_ctx->panels().engine = true;

    // MYVK_EDITOR_WORKSPACE=<level|rendering|character|animation|cinematic|debug>
    if (const char* ws = std::getenv("MYVK_EDITOR_WORKSPACE")) {
        const int wi = workspaceIndexOf(ws);
        if (wi >= 0) applyWorkspace(wi);
        else VK_LOG_WARN("MYVK_EDITOR_WORKSPACE: 未知工作区 '%s'", ws);
    }
    // MYVK_EDITOR_WORKSPACE_SEQ="30:debug 60:level" —— 运行中按帧切工作区。
    // E05 的验收是"切过去 → 断言 → 切回来 → 复原"，一次会话里必须切两次，
    // 所以启动钩子不够用（见 EditorApp.h 的 WsSwitch 说明）。
    if (const char* seq = std::getenv("MYVK_EDITOR_WORKSPACE_SEQ")) {
        parseWorkspaceSequence(seq);
    }
    // MYVK_EDITOR_VP_LAYOUT=<single|two_vertical|two_horizontal|quad>
    // （2v / 2h / quad 是简写）—— 多视口（E07）的自动化入口。
    if (const char* vl = std::getenv("MYVK_EDITOR_VP_LAYOUT")) {
        const std::string s = vl;
        ViewportLayout want = ViewportLayout::Single;
        bool ok = true;
        if (s == "single") want = ViewportLayout::Single;
        else if (s == "two_vertical" || s == "2v") want = ViewportLayout::TwoVertical;
        else if (s == "two_horizontal" || s == "2h") want = ViewportLayout::TwoHorizontal;
        else if (s == "quad") want = ViewportLayout::Quad;
        else ok = false;
        if (ok) {
            m_ctx->setViewportLayout(want);
            syncViewportLayout();
        } else {
            VK_LOG_WARN("MYVK_EDITOR_VP_LAYOUT: 未知布局 '%s'", vl);
        }
    }

    // MYVK_EDITOR_GPUTIMELINE=1 —— 打开 GPU 时间轴面板（E25 的自动化入口）。
    // ⚠ 排在 MYVK_EDITOR_WORKSPACE 之后：工作区切换会把面板显隐整份重设，
    //   而 Rendering 工作区本来就会打开这个面板（kRenderOpen）。
    if (std::getenv("MYVK_EDITOR_GPUTIMELINE")) {
        if (bool* p = panelOpenSlotById("gputimeline")) *p = true;
        m_gpuTimelineOpenDone = true;
        // 这一行是给自动化的证据：launch 之后面板状态必须**可见**地变了。
        // （onInit 里没有其它 logPanelStates 调用点，不打就只剩"closed"。）
        logPanelStates();
    }

    // 启动场景 = **一张白纸**：一块不透明地面 + 一盏方向光 + 一个默认机位。
    // 这里以前会搭出一整套内置演示内容，2026-10-09 按用户要求连同独立的
    // Sandbox 示例程序一起删掉了 —— venn 启动不再加载任何样例。
    resetToEmptyScene(m_ctx->editorScene(), am);
    // 视口导航相机：显式输入 + 自由飞行（UE 视口手感，见 ViewportPanel）
    m_ctx->applyEditorCameraMode();

    // ---- 自动化钩子（截图 / 回归测试；正常使用不设这些变量就没有任何影响）----
    //   MYVK_EDITOR_SCENE=路径   启动即打开指定场景，而不是空场景
    //   MYVK_EDITOR_SELECT=名字  启动后按名字选中一个实体
    //   MYVK_EDITOR_CAM="x,y,z,yawDeg,pitchDeg"   把视口相机摆到指定位姿
    //   MYVK_EDITOR_GIZMO=move|rotate|scale   启动即切手柄模式
    //   MYVK_EDITOR_COLLIDER=hull|capsule     给选中项加碰撞体并进入碰撞体编辑
    //   MYVK_EDITOR_PREFS=1      启动即打开 Preferences 窗口（截图验证用）
    //   MYVK_EDITOR_NO_HUD=1     关掉视口里的统计 / 导航叠加层，纯 3D 出图。
    //       自动化截图的像素断言需要"同一配置两次运行背景逐像素相同"，而
    //       叠加层里的 FPS / 帧时间每次都不同，会在背景上留下几阶噪声。
    if (std::getenv("MYVK_EDITOR_NO_HUD")) m_viewport->setHudVisible(false);
    if (const char* envScene = std::getenv("MYVK_EDITOR_SCENE")) {
        if (envScene[0] != '\0') {
            const std::string abs = assets::resolveAssetPath(envScene);
            const scene::SceneIoResult r =
                scene::loadScene(m_ctx->editorScene(), am, abs);
            // scenePath 无论加载成败都要设：它表示"这个编辑器当前开着哪个
            // 文档"，而不是"哪个文件读过"。路径不存在时就是一份新文档，
            // Ctrl+S 应该存到**这个**路径，而不是悄悄落到默认的
            // assets/scenes/scene.json 上去。（自动化测试也依赖这一点：
            // 它用一个临时路径起步，跑完直接读那个临时文件做断言。）
            m_ctx->scenePath() = envScene;
            if (r.ok) {
                m_ctx->onSceneReplaced();
                m_ctx->pushRecentScene(envScene);
                VK_LOG_INFO("Editor: auto-opened scene '%s' (%zu entities)",
                            envScene, m_ctx->editorScene().objectCount());
            } else {
                VK_LOG_WARN("Editor: auto-open failed for '%s': %s (treated as "
                            "a new document at that path)",
                            envScene, r.error.c_str());
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

    // ---- MYVK_EDITOR_CAM="x,y,z,yawDeg,pitchDeg"：把视口相机摆到指定位姿 ----
    // 自动化截图 / 交互测试专用：有些机位人手摆不准（"贴到离地 40 cm 平视"
    // 复现栅格问题、或"让某个物体正好落在视口中心好让脚本点得到"）。
    //
    // ⚠ 必须放在 MYVK_EDITOR_SCENE **之后** —— 加载场景会连相机一起覆盖，
    //   顺序反了这个钩子就白设了。
    if (const char* envCam = std::getenv("MYVK_EDITOR_CAM")) {
        float x = 0.0f, y = 0.0f, z = 0.0f, yawDeg = 0.0f, pitchDeg = 0.0f;
        if (std::sscanf(envCam, "%f,%f,%f,%f,%f", &x, &y, &z, &yawDeg,
                        &pitchDeg) == 5) {
            scene::Camera& cam = m_ctx->editorScene().camera();
            cam.setYaw(glm::radians(yawDeg));
            cam.setPitch(glm::radians(pitchDeg));
            // 飞行模式下 setTarget 会把相机重摆到 target + angleDir*distance，
            // 所以传 "eye + forward*distance" 正好让 position 落在 eye 上。
            cam.setTarget(glm::vec3(x, y, z) + cam.forwardAxis() * cam.distance());
            VK_LOG_INFO("Editor: camera eye=(%.2f,%.2f,%.2f) yaw=%.1f pitch=%.1f",
                        x, y, z, yawDeg, pitchDeg);
        } else {
            VK_LOG_WARN("MYVK_EDITOR_CAM 解析失败: '%s'（期望 x,y,z,yawDeg,pitchDeg）",
                        envCam);
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

    // MYVK_EDITOR_SPACE=world|local：启动就把手柄坐标系切过去。
    // 世界轴和局部轴对**旋转过的物体**是两套不同的方向，缩放的行为
    // 也随之外观不同 —— 自动化要能分别覆盖，不能只测默认那一套。
    if (const char* envSpace = std::getenv("MYVK_EDITOR_SPACE")) {
        const std::string s = envSpace;
        if (s == "local")
            m_ctx->setGizmoSpace(GizmoSpace::Local);
        else if (s == "world")
            m_ctx->setGizmoSpace(GizmoSpace::World);
    }
    if (std::getenv("MYVK_EDITOR_PREFS")) m_ctx->showSettings() = true;

    // MYVK_EDITOR_COLLIDER=hull|capsule：给**当前选中项**加一个碰撞体，并
    // 直接进入"编辑碰撞体"状态（手柄立刻作用在碰撞框上）。
    //
    // 为什么需要它：验证"碰撞框能用 W/E/R 调整"需要一个已经存在的碰撞体，
    // 而"加碰撞体"那条路（视口右键菜单）另有脚本专门覆盖。这里只负责把
    // 前置状态摆好，让手柄用例专注于手柄本身 —— 免得每个用例都要先演一遍
    // 右键→悬停子菜单→点菜单项。
    if (const char* envColl = std::getenv("MYVK_EDITOR_COLLIDER")) {
        const std::string c = envColl;
        if (c == "hull" || c == "capsule") {
            const ecs::Entity e = m_ctx->selection();
            ecs::World& w = m_ctx->editorScene().world();
            auto* mc = e.valid() ? w.get<ecs::MeshComponent>(e) : nullptr;
            if (mc && mc->mesh) {
                ecs::CollisionComponent cc;
                const bool hull = (c == "hull");
                const bool ok = hull
                                    ? physics::setupHullCollider(*mc->mesh, cc)
                                    : physics::setupCapsuleCollider(*mc->mesh,
                                                                    cc);
                if (ok) {
                    if (auto* p = w.get<ecs::CollisionComponent>(e)) *p = cc;
                    else w.add<ecs::CollisionComponent>(e, cc);
                    m_ctx->setColliderEdit(true);
                    const std::string* nm = w.name(e);
                    VK_LOG_INFO("Editor auto-collider (%s): '%s' points=%zu "
                                "edges=%zu radius=%.3f halfHeight=%.3f",
                                hull ? "hull" : "capsule",
                                nm ? nm->c_str() : "?", cc.hullPoints.size(),
                                cc.hullEdges.size(),
                                static_cast<double>(cc.capsuleRadius),
                                static_cast<double>(cc.capsuleHalfHeight));
                    // "创建时默认刚好包裹住物体"的可断言形式 —— 与视口右键
                    // 菜单那条路打同一格式的 `collision: fit`，于是验证脚本
                    // 不管碰撞体是点菜单建的还是这个钩子建的，都读同一行。
                    glm::vec3 mmn, mmx, cmn, cmx;
                    if (physics::meshAabb(*mc->mesh, mmn, mmx) &&
                        physics::colliderLocalAabb(cc, cmn, cmx)) {
                        const glm::vec3 ms = mmx - mmn;
                        const glm::vec3 cs = cmx - cmn;
                        VK_LOG_INFO(
                            "collision: fit '%s' shape=%s "
                            "mesh=(%.4f,%.4f,%.4f) collider=(%.4f,%.4f,%.4f) "
                            "ratio=(%.4f,%.4f,%.4f)",
                            nm ? nm->c_str() : "?",
                            hull ? "convex" : "capsule",
                            static_cast<double>(ms.x),
                            static_cast<double>(ms.y),
                            static_cast<double>(ms.z),
                            static_cast<double>(cs.x),
                            static_cast<double>(cs.y),
                            static_cast<double>(cs.z),
                            ms.x > 0.0f ? static_cast<double>(cs.x / ms.x) : 0.0,
                            ms.y > 0.0f ? static_cast<double>(cs.y / ms.y) : 0.0,
                            ms.z > 0.0f ? static_cast<double>(cs.z / ms.z)
                                        : 0.0);
                    }
                } else {
                    VK_LOG_WARN("Editor auto-collider (%s): build failed "
                                "(empty mesh?)", c.c_str());
                }
            } else {
                VK_LOG_WARN("Editor auto-collider (%s): selection has no mesh",
                            c.c_str());
            }
        } else {
            VK_LOG_WARN("MYVK_EDITOR_COLLIDER 只认 hull|capsule，收到 '%s'",
                        envColl);
        }
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

    // ---- E05：按帧序列切工作区（自动化用，见 MYVK_EDITOR_WORKSPACE_SEQ）----
    // 放在 onUpdate 的最前面：切完当帧生效，剩下的帧都处于稳定状态 ——
    // 截图脚本不需要猜"等几帧才稳定"。
    ++m_editorFrames;
    if (!m_wsSwitch.empty() && m_editorFrames >= m_wsSwitch.front().frame) {
        const int idx = m_wsSwitch.front().index;
        m_wsSwitch.erase(m_wsSwitch.begin());
        applyWorkspace(idx);
        m_ctx->notify(std::string("Workspace: ") + kWorkspaces[idx].title);
    }

    // ---- E16：Console 命令注入（自动化验收用）----
    // MYVK_EDITOR_CONSOLE_CMD="set ssr.enabled 1"：第 40 帧执行一次。
    // 走的是与 UI/命令面板完全同一条 execute 路径（含 emitParamChange
    // 的副作用），所以日志里能看到 FTR-UI / PRESET-APPLY 等锚点。
    if (!m_consoleCmdDone && m_editorFrames >= 40) {
        m_consoleCmdDone = true;
        if (const char* cmd = std::getenv("MYVK_EDITOR_CONSOLE_CMD")) {
            if (cmd[0]) m_console->execute(cmd);
        }
    }

    // 视口接管了相机输入 —— Camera::update 里会直接 return
    m_ctx->activeScene().camera().setUseGlobalInput(false);

    // 地平面栅格的**最终开关**：编辑态 + 用户勾了才画。
    // Play 期间视口里是游戏画面，参考网格不该混进去，所以这里直接压掉 ——
    // 用户回到编辑态时（勾选框还是打勾的）它自己就回来了。
    // 真正的绘制在 Renderer 的 grid pass（最小格 1 m / 主格 10 m）。
    renderer().gridEnabled() = m_ctx->isEditing() && m_ctx->showGrid();

    m_ctx->tickNotification(dt);
    // E21：通知 / 进度 —— 推进 toast 寿命、淘汰过期项，再跑自动化钩子。
    // 放在 onUpdate 里（不是 onImGui）：自动化的"第 40 帧"计数在 onUpdate
    // 头部自增，两个钩子用同一个帧号才不会互相错帧。
    m_notifications->update(dt);
    driveNotificationHooks();
    driveToastClickHook();
    // E25：GPU 时戳时间轴 —— **每帧**采样一次（面板关着也在积累历史，
    // 打开即见完整的 60 帧）。时戳的 readFrame 发生在 Renderer 的帧栅栏
    // 之后，所以这里读到的永远是"最近一次成功读回"的那一帧；首帧无数据
    // 与异常读数由面板自己挡掉（不会插假帧）。
    if (m_gpuTimeline) {
        m_gpuTimeline->sample(renderer().gpuTimer());
        m_gpuTimeline->driveHooks(static_cast<int>(m_editorFrames));
        // MYVK_EDITOR_GPU_TIMELINE=1：每 30 帧一行快照（与 GPU-TIMING 同频但错开
        // 相位，避免两行日志叠在一起看不清）。
        if (std::getenv("MYVK_EDITOR_GPU_TIMELINE") &&
            (m_editorFrames % 30u) == 10u) {
            m_gpuTimeline->logSnapshot();
        }
    }
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

    // ---- E08：播放倍速 ----
    // 夹到 [0.05, 4]：0 会让脚本时间冻住（看起来像卡死），负数会让物理
    // 反向积分。倍速只乘在**仿真** dt 上，渲染帧率不受影响。
    m_timeScale = std::clamp(m_timeScale, 0.05f, 4.0f);
    const float simDt = dt * m_timeScale;

    // ---- Run：游戏逻辑跑在**运行态副本**上；动画跑在编辑场景上 ----
    // 渲染那一边不用额外做什么：activeScene() 在 Run 期间已经返回运行态，
    // 视口自然会显示这份世界。
    if (m_ctx->isPlaying()) {
        // 一次新的 Play 开始（上一段 Play 的帧计数已被下面的 else 归零，
        // 或者这是启动即 Play、计数器还是初值）→ 把详细日志预算灌满。
        if (m_collisionFrames == 0) m_collisionLogBudget = 240;
        m_ctx->advancePlayTime(simDt);
        runRuntimeScripts(simDt);
        // 玩家在脚本之后 / 碰撞之前：脚本可能挪物体，玩家再动自己，
        // 碰撞最后保证谁都不穿谁（玩家也被墙挡）。
        runPlayers(simDt);
        // 碰撞必须在脚本**之后**：脚本负责让物体动，碰撞负责不让它们穿过去。
        // 反过来（先碰撞后脚本）等于每一帧结束时都留一次穿透。
        runCollision(simDt);
    } else {
        // 回到编辑态/暂停态就把帧计数归零，这样每次重新 Run
        // 都会重新打一条脚本执行汇总
        m_runtimeFrames = 0;
        m_scriptErrors.clear();
        m_collisionFrames = 0;
        m_collisionLogBudget = 240;
    }

    // ---- 动画（编辑态脚本）：Play 走、Pause 冻结、Stop 恢复快照 ----
    if (m_ctx->isEditing() && m_ctx->animPlaying()) {
        m_ctx->advanceAnimTime(simDt);
        runAnimScripts(simDt);
    } else {
        m_animFrames = 0;
        m_animErrors.clear();
    }
}

// ---------------------------------------------------------------- 窗口收尾

// 关窗口 / 菜单 Quit / 跑满帧数都会走到这里（窗口此时还活着）。
// 只做一件事：把窗口几何存下来，下次打开接着用。
void EditorApp::onShutdown() {
    // E04 / E05 / E17：布局 + 工作区 + 偏好落盘（自动化时各自跳过）
    saveEditorLayoutToDisk();
    if (m_console) savePrefsToDisk();
    // E21：通知历史落盘（自动化跳过 —— 与上面同一条规则）
    if (m_notifications) m_notifications->saveToDisk();

    // E16：控制台缓冲统计（自动化断言锚点：注入 WARN / 过滤后的级别
    // 计数在这里一次给全）。只挂在 LOG_RECTS 下 —— 平时关闭是安静路径。
    if (m_console && std::getenv("MYVK_LOG_RECTS")) {
        VK_LOG_INFO("CONSOLE-DUMP lines=%zu info=%zu warn=%zu err=%zu",
                    m_console->lineCount(),
                    m_console->countByLevel(core::LogLevel::Info),
                    m_console->countByLevel(core::LogLevel::Warn),
                    m_console->countByLevel(core::LogLevel::Error));
    }
    // E21：通知 / 进度计数快照（同一条纪律：只在 LOG_RECTS 下打 —— 它是
    // "一次跑完全部可断言的量"，而不是运行时的常规日志）。
    if (m_notifications && std::getenv("MYVK_LOG_RECTS")) {
        VK_LOG_INFO(
            "NOTIF-DUMP toasts=%d history=%d warn=%d error=%d unread=%d "
            "tasks=%d minimized=%d",
            m_notifications->toastCount(), m_notifications->historyCount(),
            m_notifications->warnCount(), m_notifications->errorCount(),
            m_notifications->unread(), m_notifications->taskCount(),
            m_notifications->tasksMinimized() ? 1 : 0);
    }

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

// 在指定场景上执行所有启用了 ScriptComponent 的实体。
// 运行态（Run）与动画（编辑场景）共用这一个内核，只有目标场景/时间源/
// 错误表不同 —— 复制两份循环迟早会改出分叉。
void EditorApp::runScriptsOn(scene::Scene& sc, double t, float dt,
                             std::unordered_map<unsigned, std::string>& errors,
                             const char* tag, uint64_t& summaryFrame) {
    ecs::World& w = sc.world();
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
                auto it = errors.find(e.id);
                if (it == errors.end() || it->second != msg) {
                    errors[e.id] = msg;
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
                auto it = errors.find(e.id);
                if (it == errors.end() || it->second != msg) {
                    errors[e.id] = msg;
                    m_scripts->logLine("[runtime] " + scomp.path + " > " + msg);
                    VK_LOG_WARN("Script runtime error (%s): %s",
                                scomp.path.c_str(), msg.c_str());
                }
                ++failed;
                return;
            }
            errors.erase(e.id);
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

    // 第一帧打一条汇总：冒烟测试据此确认"场景 + 脚本执行"链路真的通了
    // （只看"没有 WARN"证明力不够）
    if (++summaryFrame == 1) {
        VK_LOG_INFO("%s scripts: %d executed, %d failed (t=%.3fs)", tag,
                    executed, failed, t);
    }
}

void EditorApp::runRuntimeScripts(float dt) {
    runScriptsOn(m_ctx->runtimeScene(), static_cast<double>(m_ctx->playTime()),
                 dt, m_scriptErrors, "Runtime", m_runtimeFrames);
}

// ---------------------------------------------------------------- 运行态碰撞
//
// 两件事，都在**运行态副本**上做（编辑态一个字节都不动，所以 Stop 之后
// 场景永远回到播放前的样子）：
//
//   1. resolveBodyOverlaps —— 所有带碰撞体的实体两两分离。
//      GJK 判"有没有插进去"，EPA 给出"插了多深、往哪个方向退"，
//      然后把两个实体各推一半。跑 4 轮，墙角那种被夹住的情况也能推开。
//
//   2. resolveCamera —— 相机当成一个半径 kPlayCameraRadius 的球，
//      用同一套 GJK/EPA 推出所有碰撞体。于是"镜头不会飞进墙里"。
//
// 顺序很重要：脚本先跑（它负责让物体动），碰撞后跑（它负责不让物体重合）。
// ---------------------------------------------------------------- 玩家
//
// 运行态驱动 Shift+A 加进来的 Player（第一人称 / 第三人称）。手感对齐
// 市面游戏，两种视角的鼠标语义刻意不同：
//   第一人称 —— 鼠标 = 转动**视野**；WASD 沿视线（水平分量）走。
//   第三人称 —— 鼠标 = **环绕角色**的轨道相机；WASD 沿相机朝向走，
//               角色平滑转身朝移动方向、边走边摆腿（walk 动画）。
// 输入来源就是 ImGui IO：鼠标要在视口里才转视角；键盘全窗口生效
// （游戏习惯）。玩家带胶囊碰撞体 → 移动后由 runCollision 保证不穿墙。
void EditorApp::runPlayers(float dt) {
    scene::Scene& sc = m_ctx->runtimeScene();
    ecs::World& w = sc.world();

    std::vector<ecs::Entity> players;
    w.each<ecs::PlayerComponent>(
        [&](ecs::Entity e, ecs::PlayerComponent&) { players.push_back(e); });
    if (players.empty()) return;

    const ImGuiIO& io = ImGui::GetIO();
    const bool mouseInViewport =
        m_ctx->mouseInViewport(glm::vec2(io.MousePos.x, io.MousePos.y));
    const bool keysActive = !io.WantTextInput;

    for (ecs::Entity e : players) {
        auto* pc = w.get<ecs::PlayerComponent>(e);
        auto* tc = w.get<ecs::TransformComponent>(e);
        if (!pc || !tc) continue;

        const bool firstPerson =
            pc->mode == ecs::PlayerMode::FirstPerson;

        // ---- 鼠标 ----
        // pitch 是**视线仰角**（正 = 抬头）：io.MouseDelta.y 向下为正，
        // 所以"鼠标往上"= 负值 = pitch 增大 = 抬头。市面游戏就是这样，
        // 第一/第三人称同一套语义。
        if (mouseInViewport) {
            pc->yaw += io.MouseDelta.x * pc->lookSpeed;
            pc->pitch -= io.MouseDelta.y * pc->lookSpeed;
        }
        pc->pitch = glm::clamp(pc->pitch, -1.45f, 1.45f);

        // ---- WASD（(forward, right) 键位组合）----
        glm::vec2 wish(0.0f);
        if (keysActive) {
            if (ImGui::IsKeyDown(ImGuiKey_W)) wish.x += 1.0f;
            if (ImGui::IsKeyDown(ImGuiKey_S)) wish.x -= 1.0f;
            if (ImGui::IsKeyDown(ImGuiKey_D)) wish.y += 1.0f;
            if (ImGui::IsKeyDown(ImGuiKey_A)) wish.y -= 1.0f;
        }
        const bool moving = (wish.x != 0.0f || wish.y != 0.0f);
        const float speed = pc->moveSpeed * (io.KeyShift ? 2.0f : 1.0f);

        // 相机系水平基（与 Camera::angleDir 同约定：yaw=0 → +X，
        // yaw 增大向右转）。第一人称：forward = 视线水平分量；
        // 第三人称：相机在角色 +dir 方位上、看向角色 →
        // W（远离相机）= -方位向量的水平分量。
        const float cy = std::cos(pc->yaw), sy = std::sin(pc->yaw);
        const glm::vec3 fwdH =
            firstPerson ? glm::vec3(cy, -sy, 0.0f)
                        : glm::vec3(-cy, sy, 0.0f);
        const glm::vec3 rightH = firstPerson
                                     ? glm::vec3(-sy, -cy, 0.0f)
                                     : glm::vec3(sy, cy, 0.0f);

        if (moving) {
            const glm::vec2 dir2 = glm::normalize(wish);
            tc->position +=
                (fwdH * dir2.x + rightH * dir2.y) * (speed * dt);
        }

        // ---- 第三人称：转身 + walk 摆腿 ----
        if (!firstPerson) {
            if (!pc->started) pc->bodyYaw = pc->yaw;
            if (moving) {
                const glm::vec3 dir =
                    glm::normalize(fwdH * wish.x + rightH * wish.y);
                // 面朝方向的 yaw：dir=(cos b, -sin b) → b = atan2(-y, x)
                const float targetYaw = std::atan2(-dir.y, dir.x);
                float diff = targetYaw - pc->bodyYaw;
                // 最短角差（bodyYaw 会一直增大，先 wrap 到 [-π,π]）
                constexpr float kTwoPi = 6.28318530718f;
                diff = std::fmod(diff, kTwoPi);
                if (diff > 3.14159265f) diff -= kTwoPi;
                if (diff < -3.14159265f) diff += kTwoPi;
                pc->bodyYaw +=
                    diff * std::min(1.0f, pc->turnSpeed * dt);
                // 角色建模面朝 +X：绕 Z 转 -bodyYaw 把 +X 转到面朝方向
                tc->rotation.z = -pc->bodyYaw;
                pc->walkPhase += speed * 3.2f * dt;
            } else {
                // 停下：摆动相位衰减回 0（四肢滑回直立）
                pc->walkPhase *= 1.0f - std::min(1.0f, 10.0f * dt);
            }

            // 摆四肢：关节是 root 的**直接子节点**（Hierarchy.parent 判属主），
            // 转关节的局部 rotation.y = 肢体绕肩/髋前后摆。
            // 2026-10-11：程序化小人已删除，这里只在有 PlayerLimbComponent
            // 时才摆动（骨骼网格角色走 Skeleton 采样路径，不经过这里）。
            const float swing = std::sin(pc->walkPhase) * 0.65f;
            w.each<ecs::PlayerLimbComponent>(
                [&](ecs::Entity le, ecs::PlayerLimbComponent& limb) {
                    const auto* h = w.get<ecs::HierarchyComponent>(le);
                    if (!h || h->parent != e.id) return;  // 别的玩家的
                    auto* lt = w.get<ecs::TransformComponent>(le);
                    if (!lt) return;
                    switch (limb.kind) {
                        case ecs::PlayerLimbComponent::ArmL:
                            lt->rotation.y = swing;
                            break;
                        case ecs::PlayerLimbComponent::ArmR:
                            lt->rotation.y = -swing;
                            break;
                        case ecs::PlayerLimbComponent::LegL:
                            lt->rotation.y = -swing;
                            break;
                        case ecs::PlayerLimbComponent::LegR:
                            lt->rotation.y = swing;
                            break;
                        default:  // 躯干 / 头不摆
                            break;
                    }
                });
        }
        pc->started = true;

        // ---- 相机 ----
        // Camera 不变式：position = target + angleDir * distance。
        // 把 target / distance / yaw / pitch 全部显式设置，
        // 相机就精确落在我们要的位置（不依赖上一帧状态）。
        scene::Camera& cam = sc.camera();
        if (firstPerson) {
            const float cp = std::cos(pc->pitch);
            const glm::vec3 dir(std::cos(pc->yaw) * cp,
                                -std::sin(pc->yaw) * cp,
                                std::sin(pc->pitch));
            const glm::vec3 eye =
                tc->position + glm::vec3(0.0f, 0.0f, pc->eyeHeight);
            cam.setTarget(eye + dir);   // 看向眼前 1m → 相机正好在眼睛
            cam.setDistance(1.0f);
            cam.setYaw(pc->yaw);
            cam.setPitch(pc->pitch);
        } else {
            const glm::vec3 focus =
                tc->position + glm::vec3(0.0f, 0.0f, 1.2f);
            cam.setTarget(focus);
            cam.setDistance(pc->camDistance);
            cam.setYaw(pc->yaw);
            // Camera 的 pitch 是"相机在目标上方"的俯视角，与我们的视线
            // 仰角反号 —— 不取负的话"鼠标往上"会变成"相机升得更高、看得
            // 更低"（这正是 2026-10-11 修掉的那个上下颠倒）。
            cam.setPitch(-pc->pitch);
        }
    }

    // 自动化探针：每 30 帧报一次玩家状态（pos / yaw / pitch），
    // 验证脚本据此断言"按 D 玩家往右挪了 / 动鼠标视野转了"。
    // 末尾带 miv = 鼠标是否落在视口矩形内：鼠标视角是它门控的，注入不生效
    // 时第一眼就该看出是"鼠标不在视口里"还是"位移没收到"。
    ++m_playerLogTick;
    if (m_playerLogTick >= 30) {
        m_playerLogTick = 0;
        for (ecs::Entity e : players) {
            const auto* pc = w.get<ecs::PlayerComponent>(e);
            const auto* tc = w.get<ecs::TransformComponent>(e);
            if (!pc || !tc) continue;
            const std::string* nm = w.name(e);
            VK_LOG_INFO("PLAYER-STATE '%s' mode=%s pos=(%.3f,%.3f,%.3f) "
                        "yaw=%.3f pitch=%.3f miv=%d",
                        nm ? nm->c_str() : "?",
                        pc->mode == ecs::PlayerMode::ThirdPerson ? "third"
                                                                 : "first",
                        static_cast<double>(tc->position.x),
                        static_cast<double>(tc->position.y),
                        static_cast<double>(tc->position.z),
                        static_cast<double>(pc->yaw),
                        static_cast<double>(pc->pitch),
                        mouseInViewport ? 1 : 0);
        }
    }
}

void EditorApp::runCollision(float dt) {
    (void)dt;
    scene::Scene& sc = m_ctx->runtimeScene();

    std::vector<physics::BodyContact> contacts;
    const int pairs = physics::resolveBodyOverlaps(sc, 4, &contacts);

    // 相机求解要跳过玩家自己的胶囊（第一人称的"眼睛"就在里面）
    std::vector<ecs::Entity> players;
    const ecs::World& w = sc.world();
    w.each<ecs::PlayerComponent>(
        [&](ecs::Entity e, const ecs::PlayerComponent&) {
            players.push_back(e);
        });

    const glm::vec3 camBefore = sc.camera().position();
    const bool camPushed = physics::resolveCamera(sc, sc.camera(),
                                                  kPlayCameraRadius, players);
    const glm::vec3 camAfter = sc.camera().position();

    ++m_collisionFrames;
    if (pairs == 0 && !camPushed) return;

    const bool verbose = m_collisionLogBudget > 0;
    if (!verbose && (m_collisionFrames % 60) != 0) return;

    if (pairs > 0) {
        VK_LOG_INFO("collision: %d overlapping pair(s) resolved (frame %llu)",
                    pairs, static_cast<unsigned long long>(m_collisionFrames));
        if (verbose) {
            for (const physics::BodyContact& c : contacts) {
                const std::string* na = sc.world().name(c.a);
                const std::string* nb = sc.world().name(c.b);
                VK_LOG_INFO("collision: '%s' <-> '%s' depth=%.4f "
                            "normal=(%.3f,%.3f,%.3f)",
                            na ? na->c_str() : "?",
                            nb ? nb->c_str() : "?", 
                            static_cast<double>(c.depth),
                            static_cast<double>(c.normal.x),
                            static_cast<double>(c.normal.y),
                            static_cast<double>(c.normal.z));
            }
        }
    }
    if (camPushed) {
        VK_LOG_INFO("collision: camera blocked, pushed (%.3f,%.3f,%.3f) -> "
                    "(%.3f,%.3f,%.3f)",
                    static_cast<double>(camBefore.x),
                    static_cast<double>(camBefore.y),
                    static_cast<double>(camBefore.z),
                    static_cast<double>(camAfter.x),
                    static_cast<double>(camAfter.y),
                    static_cast<double>(camAfter.z));
    }
    if (verbose) --m_collisionLogBudget;
}

// 动画：直接在**编辑场景**上跑脚本（Play 走 / Pause 冻结 / Stop 恢复快照）
void EditorApp::runAnimScripts(float dt) {
    runScriptsOn(m_ctx->editorScene(), static_cast<double>(m_ctx->animTime()),
                 dt, m_animErrors, "Anim", m_animFrames);
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

    // E06-5：面板被关掉时那一侧收成 0（邻居吃掉空间，而不是留一块空白）。
    // 必须在 apply() 之前 —— resolve 读的就是这里算出来的 liveA/liveB。
    m_split->syncPanelVisibility();

    m_split->apply(m_ctx->layout());

    // 视口正下方腾出"刚刚好一行"播放条：从视口矩形底部扣掉一行，
    // 扣出来的就是 transport 条。3D 画面的渲染高度随之自然缩小。
    //
    // ⚠ 只有视口真的占着地方时才扣：面板被关掉 / 被别的面板最大化顶掉时
    //   它的矩形是 0×0，硬扣会把 0 抬到 kMinViewport，于是"最大化 Hierarchy"
    //   之后屏幕底部会冒出一条属于视口的播放条。
    LayoutRects& L = m_ctx->layout();
    if (L.viewport.w > 1.0f && L.viewport.h > 1.0f) {
        L.transport = {L.viewport.x, L.viewport.y + L.viewport.h - kTransportH,
                       L.viewport.w, kTransportH};
        L.viewport.h = std::max(L.viewport.h - kTransportH, kMinViewport);
    } else {
        L.transport = {0.0f, 0.0f, 0.0f, 0.0f};
    }
}

// ============================================================
// E05 · 工作区 + 布局持久化
// ============================================================
namespace {

// 布局槽位 → 面板 id（落盘用）
const char* slotPanelId(int slot) {
    if (slot == kSlotNone) return "";
    const std::vector<PanelDesc>& t = EditorApp::panelTableVec();
    for (const PanelDesc& p : t) {
        if (p.layoutSlot == slot) return p.id ? p.id : "";
    }
    return "";
}

// 面板 id → 布局槽位（读盘用）。找不到 / 不在分栏树里 → kSlotNone。
int slotForPanelId(const char* id) {
    const int i = panelIndexOf(id);
    if (i < 0) return kSlotNone;
    const std::vector<PanelDesc>& t = EditorApp::panelTableVec();
    if (i >= static_cast<int>(t.size())) return kSlotNone;
    return t[static_cast<size_t>(i)].layoutSlot;
}

} // namespace

const char* EditorApp::viewModeLabel(int mode) {
    using VM = render::Renderer::ViewMode;
    if (mode < 0 || mode > static_cast<int>(VM::LightComplexity)) return "lit";
    return render::Renderer::viewModeName(static_cast<VM>(mode));
}

void EditorApp::syncViewportLayout() {
    const ViewportLayout l = m_ctx->viewportLayout();
    // Single = 只有主视口；其余布局每个格子各一个离屏目标，
    // 主视口用 0 号，剩下的都是"子视口"。
    size_t sub = 0;
    switch (l) {
        case ViewportLayout::Single: sub = 0; break;
        case ViewportLayout::TwoVertical:
        case ViewportLayout::TwoHorizontal: sub = 1; break;
        case ViewportLayout::Quad: sub = 3; break;
    }
    renderer().setSubviewCount(sub);
    VK_LOG_INFO("VIEWPORT-LAYOUT %s (subviews=%zu)", viewportLayoutName(l), sub);
}

void EditorApp::applyWorkspace(int index) {
    if (index < 0 || index >= kWorkspaceCount) return;
    const WorkspacePreset& w = kWorkspaces[index];

    // 1) 面板显隐
    // ⚠ **必须先复位到出厂默认**，再套用本工作区的开/关清单。
    //   不加这一步就是"增量式"的：分栏里没点名的面板会**保持当前状态**，
    //   于是同一套工作区从不同地方切进来长得不一样 —— 直接违反 Workspace.h
    //   的两条承诺（"切回来 = 回到出厂预设"、"没点名 = 保持 defaultOpen"）。
    //   这里不存在"丢掉用户手动改动"的问题：用户改动要么存在工作区里
    //   （Save Workspace → editor_layout.json，走 loadEditorLayoutFromDisk
    //   那条路恢复，不经过本函数），要么本来就是被放弃的临时状态。
    resetPanelStateToDefaults();
    auto setById = [](const char* id, bool on) {
        const int i = panelIndexOf(id);
        if (i >= 0) setPanelOpen(i, on);
    };
    if (w.openPanels) {
        for (const char* const* p = w.openPanels; *p; ++p) setById(*p, true);
    }
    if (w.closePanels) {
        for (const char* const* p = w.closePanels; *p; ++p) setById(*p, false);
    }

    // 2) 分栏比例。预设里写 0 的节点交回"按窗口自适应"。
    for (int n = 0; n < SplitLayout::kNodeCount; ++n) {
        if (w.ratio[n] > 0.0f) m_split->setRatio(n, w.ratio[n]);
        else m_split->setAutoRatio(n, true);
        m_split->setCollapsed(n, false);
    }
    m_split->markNeedsAutoRecalc();
    m_split->setMaximized(kSlotNone);   // 切工作区先退出最大化

    // 3) 视口布局（E07）—— 通知渲染器准备 / 释放子视口目标
    m_ctx->setViewportLayout(w.viewportLayout);
    m_ctx->setActiveSubview(0);
    syncViewportLayout();

    // 4) 视图模式
    using VM = render::Renderer::ViewMode;
    renderer().setViewMode(static_cast<VM>(w.viewMode));

    m_ctx->workspace().setCurrent(index);
    VK_LOG_INFO("WORKSPACE %s viewportLayout=%s viewMode=%s", w.id,
                viewportLayoutName(w.viewportLayout), viewModeLabel(w.viewMode));
    logPanelStates();
}

bool EditorApp::workspaceModified() {
    const int idx = m_ctx->workspace().current();
    if (idx < 0 || idx >= kWorkspaceCount) return false;
    const WorkspacePreset& w = kWorkspaces[idx];

    // 面板显隐：预设里明确点名过的那些，逐个对
    if (w.openPanels) {
        for (const char* const* p = w.openPanels; *p; ++p) {
            const int i = panelIndexOf(*p);
            if (i >= 0 && !panelOpen(i)) return true;
        }
    }
    if (w.closePanels) {
        for (const char* const* p = w.closePanels; *p; ++p) {
            const int i = panelIndexOf(*p);
            if (i >= 0 && panelOpen(i)) return true;
        }
    }
    // 视口布局 / 视图模式
    if (static_cast<int>(m_ctx->viewportLayout()) !=
        static_cast<int>(w.viewportLayout)) {
        return true;
    }
    if (static_cast<int>(renderer().viewMode()) != w.viewMode) return true;
    // 比例：只比对预设里给了明确数字的节点（0 = 自适应，不算偏离）
    for (int n = 0; n < SplitLayout::kNodeCount; ++n) {
        if (w.ratio[n] > 0.0f &&
            std::fabs(m_split->ratio(n) - w.ratio[n]) > 0.02f) {
            return true;
        }
    }
    return false;
}

void EditorApp::saveWorkspace() {
    saveEditorLayoutToDisk();
    m_ctx->notify(std::string("Workspace saved: ") +
                  m_ctx->workspace().currentTitle());
}

void EditorApp::resetWorkspace() {
    applyWorkspace(m_ctx->workspace().current());
    m_ctx->notify("Workspace reset to factory preset");
}

// ---------------------------------------------------------------- EditorHost

void EditorApp::applyViewportLayout(int layout) {
    if (layout < 0 || layout >= viewportLayoutCount()) return;
    m_ctx->setViewportLayout(static_cast<ViewportLayout>(layout));
    m_ctx->setActiveSubview(0);
    syncViewportLayout();
}

int EditorApp::currentViewportLayout() {
    return static_cast<int>(m_ctx->viewportLayout());
}

void EditorApp::applyViewMode(int mode) {
    using VM = render::Renderer::ViewMode;
    if (mode < 0 || mode > static_cast<int>(VM::LightComplexity)) return;
    renderer().setViewMode(static_cast<VM>(mode));
}

int EditorApp::currentViewMode() {
    return static_cast<int>(renderer().viewMode());
}

void EditorApp::reloadShaders() {
    // 热重载平时是"每 15 帧扫一轮 .spv 的 mtime"；这里把节流推到阈值以上
    // → 下一帧立刻扫一遍（等价于手动催一次）。
    renderer().forceShaderReloadScan();
    m_ctx->notify("Shader reload requested (see the log)");
}

void EditorApp::reloadAssets() {
    // AssetManager 是"按路径惰性加载 + 永不淘汰"的模型（见 assets/
    // AssetManager.h）：没有"整表重扫"这件事可做。真正需要的是**脚本**
    // 重编译，那由 EditorApp::preloadScripts 每帧节流做。
    preloadScripts();
    m_ctx->notify("Assets reloaded (scripts recompiled)");
    VK_LOG_INFO("ASSETS-RELOAD requested (scripts recompiled)");
}

const char* EditorApp::gpuName() {
    // renderer().device() 返回 rhi::Device&（非 const），所以本方法也
    // 是非 const（EditorHost 那边同步去掉了 const）。
    if (!renderer().device().properties().deviceName[0]) return "(unknown)";
    return renderer().device().properties().deviceName;
}

const char* EditorApp::engineVersion() { return kVennVersion; }

// ---------------------------------------------------------------- 布局落盘

// "30:debug 60:level" → {{30, debugIdx}, {60, levelIdx}}
// 分隔符认空格 / 逗号 / 分号 / 冒号以外的任意非字母数字字符。
// 坏格式或未知工作区名只跳过那一条并 WARN —— 一个笔误不该让整轮自动化
// 跑不起来（也就看不到别的断言了）。
void EditorApp::parseWorkspaceSequence(const char* spec) {
    std::string s(spec ? spec : "");
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() &&
               !std::isalnum(static_cast<unsigned char>(s[i])) &&
               s[i] != ':') {
            ++i;
        }
        if (i >= s.size()) break;
        // 帧号
        size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j == i || j >= s.size() || s[j] != ':') {
            VK_LOG_WARN("MYVK_EDITOR_WORKSPACE_SEQ: 跳过坏条目 '%s'",
                        s.substr(i, 12).c_str());
            while (i < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[i])) ||
                    s[i] == ':')) {
                ++i;
            }
            continue;
        }
        const uint64_t frame = std::strtoull(s.substr(i, j - i).c_str(), nullptr, 10);
        ++j;   // 跳过 ':'
        size_t k = j;
        while (k < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[k])) ||
                s[k] == '_')) {
            ++k;
        }
        const std::string name = s.substr(j, k - j);
        const int wi = workspaceIndexOf(name.c_str());
        if (wi < 0) {
            VK_LOG_WARN("MYVK_EDITOR_WORKSPACE_SEQ: 未知工作区 '%s'（第 %llu 帧）",
                        name.c_str(), static_cast<unsigned long long>(frame));
        } else {
            m_wsSwitch.push_back(WsSwitch{frame, wi});
        }
        i = k;
    }
    // 按帧号升序（写反了也能按预期顺序执行）
    std::sort(m_wsSwitch.begin(), m_wsSwitch.end(),
              [](const WsSwitch& a, const WsSwitch& b) {
                  return a.frame < b.frame;
              });
    VK_LOG_INFO("WORKSPACE-SEQ parsed %zu switch(es)", m_wsSwitch.size());
}

void EditorApp::loadEditorLayoutFromDisk() {
    LayoutState s;
    if (!loadEditorLayout(s)) {
        // 没有存档（或自动化运行）：一切保持出厂值。
        // ⚠ 这里**什么都不改**是对的 —— "配置文件缺失时行为与今天一致"
        //   是回归测试的硬前提。
        m_ctx->workspace().setCurrent(0);
        // 但仍然把出厂状态打一遍：验收脚本要在"没有任何存档"的裸跑里就能
        // 读到 PANEL-STATE（否则它只能测"有存档"那条分支，覆盖不到默认值）。
        logPanelStates();
        return;
    }

    applyPanelState(s.panels);
    for (int n = 0; n < SplitLayout::kNodeCount; ++n) {
        // ⚠ 折叠中的节点必须走**自己的分支**：它的比例（0 或 1）是"往哪边
        //   坍缩"这个事实，不是"自适应"哨兵。若按普通路径把 0 当成哨兵、
        //   只置 autoRatio=true，beginFrame 下一帧就会把比例重算回默认值 ——
        //   于是折叠状态读回来就没了（而 collapsed 标志还是 true，自相矛盾）。
        //   setRatio 顺带把 autoRatio 置 false，正是这里要的语义。
        if (s.splitCollapsed[n]) {
            m_split->setRatio(n, s.splitRatio[n] >= 0.5f ? 1.0f : 0.0f);
            m_split->setCollapsed(n, true);
        } else if (s.splitRatio[n] > 0.0f) {
            m_split->setRatio(n, s.splitRatio[n]);
        } else {
            m_split->setAutoRatio(n, true);
        }
        m_split->setSavedRatio(n, s.splitSaved[n]);
    }
    m_split->markNeedsAutoRecalc();
    m_split->setMaximized(slotForPanelId(s.maximized.c_str()));

    // 视口布局夹到合法区间（老文件 / 手改文件都可能越界）
    int vl = s.viewportLayout;
    if (vl < 0 || vl >= viewportLayoutCount()) vl = 0;
    m_ctx->setViewportLayout(static_cast<ViewportLayout>(vl));

    using VM = render::Renderer::ViewMode;
    const int vmLimit = static_cast<int>(VM::LightComplexity);
    const int vm = (s.viewMode >= 0 && s.viewMode <= vmLimit) ? s.viewMode : 0;
    renderer().setViewMode(static_cast<VM>(vm));

    const int ws = workspaceIndexOf(s.workspace.c_str());
    m_ctx->workspace().setCurrent(ws >= 0 ? ws : 0);
    m_ctx->recentScenesRef() = s.recent;

    // 子视口目标按新布局建起来（applyViewportSize 那套由渲染器自己管）
    syncViewportLayout();
    // 读盘摘要打一行：验收脚本靠它区分"读到了存档"和"退回了出厂值"。
    VK_LOG_INFO("LAYOUT-LOAD workspace=%s viewportLayout=%s viewMode=%s "
                "maximized=%s recent=%zu panels=%zu",
                m_ctx->workspace().currentId(),
                viewportLayoutName(m_ctx->viewportLayout()),
                viewModeLabel(static_cast<int>(renderer().viewMode())),
                s.maximized.empty() ? "(none)" : s.maximized.c_str(),
                s.recent.size(), s.panels.size());
    logPanelStates();
}

void EditorApp::saveEditorLayoutToDisk() {
    LayoutState s;
    snapshotPanelState(s.panels);
    s.workspace = m_ctx->workspace().currentId();
    for (int n = 0; n < SplitLayout::kNodeCount; ++n) {
        // 自适应节点写 0 —— 0 就是"下次按窗口尺寸重算"的语义，
        // 写死当前像素比例会把"先小窗后最大化"的体验搞坏。
        // ⚠ 折叠中的节点相反：**必须写真实比例**（0 或 1）。那是人定的
        //   坍缩方向，不是自适应哨兵 —— 写成 0 的话读回来会被当成"自适应"，
        //   于是折叠丢失（见 loadEditorLayoutFromDisk 的对应分支）。
        const bool collapsed = m_split->collapsed(n);
        s.splitRatio[n] = (m_split->autoRatio(n) && !collapsed)
                              ? 0.0f
                              : m_split->ratio(n);
        s.splitCollapsed[n] = collapsed;
        s.splitSaved[n] = m_split->savedRatio(n);
    }
    s.viewportLayout = static_cast<int>(m_ctx->viewportLayout());
    s.viewMode = static_cast<int>(renderer().viewMode());
    s.maximized = slotPanelId(m_split->maximizedSlot());
    s.recent = m_ctx->recentScenes();
    const bool ok = saveEditorLayout(s);
    VK_LOG_INFO("LAYOUT-SAVE %s path=%s workspace=%s maximized=%s recent=%zu",
                ok ? "ok" : "SKIPPED", editorLayoutPath().c_str(), s.workspace.c_str(),
                s.maximized.empty() ? "(none)" : s.maximized.c_str(),
                s.recent.size());
}

// ---------------------------------------------------------------- 快捷键

void EditorApp::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    // 正在文本输入（重命名 / 脚本路径）时不要抢键
    if (io.WantTextInput) return;

    // ---- F11：窗口全屏（任何模式都可用，面板照旧显示）----
    // 与 Play 态的 F 分工：F = "全屏 + 只看游戏"，F11 = 只是把窗口铺满显示器。
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) toggleWindowFullscreen();

    // ---- E18：Ctrl+Shift+P = 命令面板 ----
    // 必须排在下面 `if (io.KeyCtrl)` 那一支**之前** —— 那支末尾直接 return，
    // 而且 Ctrl+P 本来没被占用（Ctrl+R 归"重载着色器"，Ctrl+N/S/D/Z/Y 各有其主）。
    if (io.KeyCtrl && io.KeyShift &&
        ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        m_showCommandPalette = true;
        return;
    }

    // ---- E05：工作区快捷键 Ctrl+Alt+1..6 ----
    // **必须排在视口的视图方向键之前**：handleViewShortcuts 认 Ctrl+1/3/7，
    // 不先拦下来就会"切了工作区的同时顺手把视角转正"，两件事一起发生。
    if (io.KeyCtrl && io.KeyAlt) {
        for (int i = 0; i < kWorkspaceCount && i < 9; ++i) {
            if (ImGui::IsKeyPressed(kDigitKeys[i], false)) {
                applyWorkspace(i);
                m_ctx->notify(std::string("Workspace: ") +
                              kWorkspaces[i].title);
                return;
            }
        }
        return;  // Ctrl+Alt 组合到此为止，不再往下传
    }

    // ---- E04：面板显隐快捷键 Alt+1..8（从注册表**派生**，不是硬编码）----
    // 为什么不是 Ctrl+数字：Ctrl+1/3/7 与 Ctrl+2/4/6/8 已经被下面视口那套
    // Blender 视图方向键占了。Alt+数字没人用，且"切换可见性"配上 Alt 挺好记。
    //
    // `!io.KeyShift` 是 E14 加的：Alt+Shift+数字 现在是**相机书签跳转**
    // （见 ViewportPanel::handleOverlayShortcuts），不能让这一支顺手把面板
    // 也切了。
    if (io.KeyAlt && !io.KeyCtrl && !io.KeyShift) {
        const std::vector<PanelDesc>& t = panelTableVec();
        for (int i = 0; i < static_cast<int>(t.size()); ++i) {
            const char* sc = t[i].shortcut;
            if (!sc || std::strncmp(sc, "Alt+", 4) != 0) continue;
            const int d = sc[4] - '1';
            if (d < 0 || d > 8) continue;
            if (ImGui::IsKeyPressed(kDigitKeys[d], false)) {
                togglePanel(i);
                m_ctx->notify(std::string(panelTitle(i)) +
                              (panelOpen(i) ? " shown" : " hidden"));
            }
        }
    }

    // ---- 视图方向快捷键（Blender 小键盘那一套：1/3/7 + Ctrl 反向，2/4/6/8 步进）----
    // 必须放在下面 Ctrl 分支**之前** —— 那个分支末尾会直接 return，
    // 会把 Ctrl+1/3/7（Back / Left / Bottom）一起吞掉。
    // 按住 Alt 时不认：Alt+数字已被面板显隐占用。
    //
    // E14 的浮层快捷键（Shift+数字 = 视图模式 / Ctrl+Alt+Shift+数字 = 书签）
    // 先跑：它们内部自带"不是 Shift 层就直接返回"的判断，放在这里是为了
    // 排在视口那套之前，免得被它的 return 吞掉。
    m_viewport->handleOverlayShortcuts();
    if (!io.KeyAlt) m_viewport->handleViewShortcuts();

    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) m_ctx->undo();
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) m_ctx->redo();
        // Ctrl+D = 复制选中项（UE5 也是这个键）
        if (ImGui::IsKeyPressed(ImGuiKey_D, false)) m_hierarchy->duplicateSelection();
        // Ctrl+N = 新建空场景
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) m_toolbar->newScene();
        // Ctrl+Shift+R = 立刻扫一遍 .spv（等价于 Build ▸ Reload Shaders）
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            reloadShaders();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            const std::string abs =
                assets::resolveAssetPath(m_ctx->scenePath());
            const scene::SceneIoResult r =
                scene::saveScene(m_ctx->editorScene(), abs);
            if (r.ok) {
                m_ctx->dirty() = false;
                m_ctx->notify("Saved " + m_ctx->scenePath());
                // 落到日志里：自动化脚本靠它确认"真的存了、存到哪了"
                VK_LOG_INFO("Scene saved: '%s'", abs.c_str());
            } else {
                m_ctx->notify("Save failed: " + r.error);
                VK_LOG_WARN("Scene save failed for '%s': %s", abs.c_str(),
                            r.error.c_str());
            }
        }
        return;  // 带 Ctrl 的组合键到此为止
    }

    // 手柄模式（和主流编辑器一致）。
    // **只在编辑态**：Play 态 W/E/R 是玩家的移动键（runPlayers 里读），
    // 不能顺手把手柄模式切了。**按住右键飞行时同样让位**：这时 W / E 是
    // "前进 / 上升"（见 ViewportPanel::handleNavigation），不该顺手把
    // 手柄切成平移 / 旋转模式。UE 也是这么分的 —— 字母键只在没在"驾驶"
    // 相机时才是工具快捷键。
    const bool flying = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (m_ctx->isEditing() && !flying) {
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

    // ---- E06-3：Shift+Space = 最大化鼠标所在的分栏面板（再按还原）----
    // 调参时极常用：Engine 面板挤在右下角，想全屏看几眼。
    // ⚠ 必须排在下面 Space（播放 / 暂停）**之前**，否则一次 Shift+Space
    //   会同时触发"最大化"和"播放"两件事。
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        const ImVec2 mp = io.MousePos;
        const LayoutRects& L = m_ctx->layout();
        auto inside = [&](const Rect& r) {
            return r.w > 1.0f && r.h > 1.0f && mp.x >= r.x && mp.x < r.x + r.w &&
                   mp.y >= r.y && mp.y < r.y + r.h;
        };
        int slot = kSlotNone;
        if (inside(L.hierarchy)) slot = kSlotHierarchy;
        else if (inside(L.content)) slot = kSlotContent;
        else if (inside(L.viewport)) slot = kSlotViewport;
        else if (inside(L.inspector)) slot = kSlotInspector;

        if (slot != kSlotNone) {
            const bool wasMax = (m_split->maximizedSlot() == slot);
            m_split->toggleMaximize(slot);
            m_ctx->notify(wasMax ? "Layout restored"
                                 : "Panel maximized  (Shift+Space to restore)");
        } else if (m_split->maximizedSlot() != kSlotNone) {
            m_split->setMaximized(kSlotNone);
            m_ctx->notify("Layout restored");
        }
    }

    // 播放 / 暂停（Shift+Space 已归"最大化"，所以这里要把 Shift 排除掉）
    if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Space, false))
        m_ctx->togglePlayPause();

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

    // ---- 数字键 0：把选中的物体"落地" ----
    // x / y 一个像素都不动，只把世界包围盒的底部贴到 z = 0 的地面上。
    // 只认"在 3D 视口里左键点中"的选中（ViewportPanel::dropSelectionToGround
    // 内部会判断，非视口选中时只在状态栏提示一句）。
    // 上排数字 0 与小键盘 0 都收 —— 和 1/3/7 那套视图快捷键的处理一致。
    if (ImGui::IsKeyPressed(ImGuiKey_0, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Keypad0, false)) {
        m_viewport->dropSelectionToGround();
    }

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
    // E18：命令面板开着时**不让快捷键穿透** —— 那时面板的输入框在收字，
    // 若照跑 handleShortcuts，敲 "s" 会顺手触发吸附、敲数字会切视角。
    if (!m_showCommandPalette) handleShortcuts();

    // View 菜单里的 "Toggle Fullscreen (F11)"：菜单在面板之后就画了，
    // 置位当帧处理掉即可（下一帧才生效，肉眼无感）
    if (m_ctx->windowFullscreenRequest()) {
        m_ctx->windowFullscreenRequest() = false;
        toggleWindowFullscreen();
    }

    // R5-1：Inspector 的 Capture 按钮 → 立刻重捕反射探针。
    // 与上面 Fullscreen 同一套路：面板只置位，拿得到 Renderer 的这里取走
    // 并转交。渲染器在**下一帧开头**执行捕获（记录在独立命令缓冲里，
    // 见 Renderer::recordProbeCapture），所以这里不必管时序。
    if (m_ctx->takeProbeCaptureRequest()) {
        renderer().requestProbeCapture();
    }

    // 快捷键/按钮刚可能改过"游戏全屏" → 这一帧就把窗口切过去
    syncWindowFullscreen();

    // 字体缩放：设置面板里拖的字号在这里落到 ImGui（运行时即时生效）。
    // 放最前面，本帧所有 UI（含布局计算）都用新字号。
    renderer().ui().setFontScale(m_ctx->fontScale());

    // 游戏全屏优先：这一帧只画视口，其余面板一个都不创建。
    // E21：但**通知照画** —— 全屏跑游戏时视口里一条 UI 都没有，恰恰是最
    //   需要"刚才出错了"提示的时候（状态栏不画，计数看不到）。
    if (drawFullscreenGame()) {
        m_notifications->drawTaskBar();
        m_notifications->drawToasts();
        serviceNotificationFocus();
        return;
    }

    computeLayout();

    // E05：当前布局是否偏离了该工作区的出厂预设（标题栏 / 菜单里的 * 号）。
    // 逐帧比对 —— 比"手工 markModified 上报"自洽：加新维度时不用回来补上报点。
    m_ctx->setWorkspaceModified(workspaceModified());

    // 菜单栏 + 工具栏（菜单里的 Panel ▸ / Workspace ▸ 都由注册表生成）
    m_toolbar->draw();

    // E04：所有面板**表驱动**绘制。顺序 = 表里的顺序（主面板在前、
    // 浮动调试面板在后），与改造前逐条一致。
    drawPanels();

    // 播放条只在视口真的占着地方时画（视口被关掉、或被别的面板最大化
    // 顶掉时，它的矩形是 0×0，再画就会浮在别的面板上）
    if (m_ctx->panels().viewport && m_split->showsInLayout(kSlotViewport)) {
        drawTransportBar();
    }

    if (m_ctx->showSettings()) drawSettingsPanel();
    drawCharacterWindow();

    drawStatusBar();

    // 分栏分隔条：在**所有面板之后**处理 —— 分隔线画在前景层（盖在面板上），
    // 拖拽用鼠标位置手动判断（不依赖面板窗口层级）
    m_split->handleSplitters();

    // "Reset Layout"：重置分栏比例
    if (m_ctx->relayout()) {
        m_split->reset();
        m_ctx->relayout() = false;
    }

    // E18：命令面板。放在**最后**画 —— ImGui 里后画的窗口在上层，
    // 面板要盖住所有面板。关着时本函数只做一次"env 钩子要不要开"的检查。
    // ⚠ 不能放进 `if (m_showCommandPalette)`：MYVK_EDITOR_CMDPAL 钩子
    //   需要"面板还关着"的那一帧来把它打开。
    drawCommandPalette(CommandRegistry::get(), &m_showCommandPalette);

    // E21：通知层永远最上层（在命令面板之后）—— 它是"出事了"的最后一道
    // 可见防线，被任何面板盖住都失去意义。顺序：任务条 → 历史窗口 → toast。
    m_notifications->drawTaskBar();
    m_notifications->drawHistoryWindow(&m_showNotificationHistory);
    m_notifications->drawToasts();
    // 点 toast 的落地：打开 Console 并滚到那一条（UI 与自动化同一条路）
    serviceNotificationFocus();
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

// ---------------------------------------------------------------- E21 通知 / 进度

// 点 toast / 点历史项 → "打开 Console 并滚到那一条"。UI 与自动化走**同一条
// 路**：两边都只是把请求塞进 NotificationCenter，这里负责落地。
// （ConsolePanel 只记一个待处理请求，真正的滚动发生在它下一次 draw —— 那时
//   那条日志必定已经在缓冲里，帧内也不做跨面板的迭代。）
void EditorApp::serviceNotificationFocus() {
    std::string text;
    if (!m_notifications->takeConsoleFocusRequest(&text)) return;
    m_ctx->panels().console = true;
    m_console->requestScrollToText(text);
    VK_LOG_INFO("NOTIF-FOCUS tab=console ok=1 text=\"%s\"", text.c_str());
}

// 状态栏的四个数值（errors / warns / tasks / dirty）喂给通知中心的边沿检测
// —— 变化时才留一条 NOTIF-STATUSBAR，于是"注入一条 ERROR 后计数 0→1"
// 必然有锚点，平稳状态又不会刷屏。
void EditorApp::logNotificationStatus() {
    m_notifications->logStatus(m_notifications->errorCount(),
                               m_notifications->warnCount(),
                               m_notifications->taskCount(), m_ctx->dirty(),
                               m_ctx->status().c_str());
}

// 自动化钩子。与 MYVK_EDITOR_CONSOLE_CMD 一样在**第 40 帧**动手（那时布局、
// 面板、命令注册表都已就绪，日志前后文也齐全）。
//
//   MYVK_EDITOR_TOAST=error|warn|info
//       error/warn 走**真实日志路径**（VK_LOG_ERROR / VK_LOG_WARN）——
//       于是 Console 里也有那一条、状态栏计数也跟着动，验的是真链路而不是
//       另写一套；info 只能显式 push（Info 不自动升级成 toast，否则启动那
//       几百条 Info 会把屏幕糊满）。
//   MYVK_EDITOR_ASYNC_TASK=1        起一个 30 帧走完的假任务（验证进度条）
//   MYVK_EDITOR_ASYNC_TASK_CANCEL=1 在第 52 帧请求取消（验证取消契约）
//   MYVK_EDITOR_NOTIF_HISTORY=1     直接打开通知历史窗口（截图用）
void EditorApp::driveNotificationHooks() {
    if (m_editorFrames < 40) return;
    if (std::getenv("MYVK_EDITOR_NOTIF_HISTORY"))
        m_showNotificationHistory = true;

    if (!m_notifHookDone) {
        m_notifHookDone = true;
        if (const char* t = std::getenv("MYVK_EDITOR_TOAST")) {
            const std::string lv = t;
            if (lv == "error")
                VK_LOG_ERROR("Editor: injected test error (MYVK_EDITOR_TOAST)");
            else if (lv == "warn")
                VK_LOG_WARN("Editor: injected test warning (MYVK_EDITOR_TOAST)");
            else if (lv == "info")
                m_notifications->push(
                    core::LogLevel::Info,
                    "Injected info notification (MYVK_EDITOR_TOAST)");
            else
                VK_LOG_WARN("MYVK_EDITOR_TOAST 只认 error|warn|info，收到 '%s'",
                            t);
        }
        if (std::getenv("MYVK_EDITOR_ASYNC_TASK")) {
            m_notifications->beginTask("demo", "Compiling shaders (demo)");
            m_taskDemoActive = true;
        }
    }

    // 假任务：30 帧走完 0→100%。**按帧**推进而不是按 dt —— 自动化下同一份
    // 二进制每次跑出的进度轨迹必须逐帧一致。
    if (!m_taskDemoActive) return;
    const float p = static_cast<float>(m_editorFrames - 40) / 30.0f;
    if (!m_taskDemoCancelled && std::getenv("MYVK_EDITOR_ASYNC_TASK_CANCEL") &&
        m_editorFrames >= 52) {
        m_taskDemoCancelled = true;
        m_notifications->requestCancel("demo");
    }
    // setTaskProgress 返回 false = 取消请求被消费 / 任务已不在 → 宿主收尾。
    // 这正是"取消按钮"的契约：**取消由宿主执行**，通知中心只传话。
    if (!m_notifications->setTaskProgress("demo", p)) {
        m_notifications->endTask("demo");
        m_taskDemoActive = false;
    } else if (p >= 1.0f) {
        m_notifications->endTask("demo");
        m_taskDemoActive = false;
    }
}

// 点 toast 的自动化入口：**第 50 帧**点第 idx 条（默认 0）。走的是与鼠标
// 点击完全同一条 clickToast 路径，于是"点 Error → 打开 Console 定位到那条"
// 可以在无人值守下断言（NOTIF-CLICK + NOTIF-FOCUS）。
void EditorApp::driveToastClickHook() {
    if (m_toastClickDone || m_editorFrames < 50) return;
    const char* c = std::getenv("MYVK_EDITOR_TOAST_CLICK");
    if (!c) return;
    m_toastClickDone = true;
    if (m_notifications->toastCount() <= 0) {
        VK_LOG_WARN("MYVK_EDITOR_TOAST_CLICK: 没有 toast 可点（注入是否没生效？）");
        return;
    }
    m_notifications->clickToast(std::atoi(c));
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

    // 选中数（E21 要的是"一眼看全局"）：单选时顺带把名字带上 —— 名字比
    // 数字有用，但"到底选了没有"这件事得有个数字化的答案。
    const int selCount = m_ctx->hasSelection() ? 1 : 0;
    ImGui::SameLine();
    if (selCount == 1) {
        const std::string* np = m_ctx->activeScene().world().name(
            m_ctx->selection());
        ImGui::TextDisabled("| selected: %d (%s)", selCount,
                            np ? np->c_str() : "unnamed");
    } else {
        ImGui::TextDisabled("| selected: %d", selCount);
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

    // ---- E21：保存状态 / 后台任务 / WARN-ERROR 计数（状态栏常驻四项）----
    // 保存状态：`*` 未保存（`dirty` 开关由 EditorContext 维护，保存即清）。
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (m_ctx->dirty())
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f), "* unsaved");
    else
        ImGui::TextDisabled("saved");

    // 后台任务数：>0 时点一下把进度条恢复出来（最小化后的归处）。
    const int nTasks = m_notifications->taskCount();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (nTasks > 0) {
        char label[48];
        std::snprintf(label, sizeof(label), "tasks %d", nTasks);
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), "%s", label);
        if (m_notifications->tasksMinimized() && ImGui::IsItemClicked())
            m_notifications->setTasksMinimized(false);
    } else {
        ImGui::TextDisabled("tasks 0");
    }

    // WARN / ERROR 计数：点一下打开 Console（"我刚错过了什么"的最短路径）。
    const int nErr = m_notifications->errorCount();
    const int nWarn = m_notifications->warnCount();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (nErr > 0) {
        const auto c = NotificationCenter::levelColor(core::LogLevel::Error);
        ImGui::TextColored(ImVec4(c.r, c.g, c.b, 1.0f), "errors %d", nErr);
    } else {
        ImGui::TextDisabled("errors 0");
    }
    const bool errClicked = ImGui::IsItemClicked();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (nWarn > 0) {
        const auto c = NotificationCenter::levelColor(core::LogLevel::Warn);
        ImGui::TextColored(ImVec4(c.r, c.g, c.b, 1.0f), "warns %d", nWarn);
    } else {
        ImGui::TextDisabled("warns 0");
    }
    const bool warnClicked = ImGui::IsItemClicked();
    if ((errClicked || warnClicked) && m_notifications->historyCount() > 0) {
        m_ctx->panels().console = true;
        m_showNotificationHistory = true;
    }

    const int nUnread = m_notifications->unread();
    if (nUnread > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::SmallButton("notifications")) m_showNotificationHistory = true;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), "%d", nUnread);
    }

    // 自动化锚点：上面四个数值任一变化就打一条 NOTIF-STATUSBAR（边沿触发）。
    logNotificationStatus();

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

// ---------------------------------------------------------------- 播放条
//
// 视口正下方那一行（LayoutRects::transport，高度由 computeLayout 从视口
// 底部扣出来）：Run = 运行游戏（编辑场景复制成运行态副本）；Play / Pause /
// Stop = **动画**：直接在编辑场景上跑/冻结脚本，Stop 用 JSON 快照恢复
// （见 EditorContext::anim*）。四个按钮在这一行的水平正中。
void EditorApp::drawTransportBar() {
    const LayoutRects& L = m_ctx->layout();
    if (L.transport.w < 120.0f) return;   // 面板被折叠到极窄就不画了

    // 自动化用（MYVK_LOG_RECTS=1）：播放条矩形 + 各按钮矩形，
    // 脚本据此断言"这一行在视口正下方、按钮居中"。
    logRect("TR-BAR", ImVec2(L.transport.x, L.transport.y),
            ImVec2(L.transport.x + L.transport.w,
                   L.transport.y + L.transport.h));

    ImGui::SetNextWindowPos(ImVec2(L.transport.x, L.transport.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.transport.w, L.transport.h),
                             ImGuiCond_Always);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    // 与工具栏同色，看起来是视口下方一体的"控制条"
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(0.113f, 0.117f, 0.125f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 3.0f));
    if (!ImGui::Begin("##transport", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    constexpr ImVec4 kRunCol(0.20f, 0.52f, 0.26f, 1.0f);   // Run 绿
    constexpr ImVec4 kExitCol(0.48f, 0.22f, 0.22f, 1.0f);  // Exit Run 红
    constexpr ImVec4 kPlayCol(0.16f, 0.42f, 0.52f, 1.0f);  // 动画 Play 青蓝
    constexpr ImVec4 kPauseCol(0.62f, 0.50f, 0.16f, 1.0f); // Pause 琥珀
    constexpr ImVec4 kStopCol(0.48f, 0.22f, 0.22f, 1.0f);  // Stop 红

    const bool runActive = m_ctx->isPlaying();
    const char* runLabel = runActive ? "Exit Run" : "Run";
    const char* labels[4] = {runLabel, "Play", "Pause", "Stop"};

    const ImGuiStyle& st = ImGui::GetStyle();
    auto btnW = [&](const char* s) {
        return ImGui::CalcTextSize(s).x + st.FramePadding.x * 2.0f + 12.0f;
    };

    // 居中：总宽 = 4 个按钮 + 3 个间隔（其中一个是分隔线）
    const float sepW = ImGui::CalcTextSize("|").x;
    const float totalW = btnW(labels[0]) + btnW(labels[1]) + btnW(labels[2]) +
                         btnW(labels[3]) + sepW + st.ItemSpacing.x * 3.0f;
    const float centerX =
        (ImGui::GetWindowWidth() - totalW) * 0.5f;
    // 这一整条控件共用的行 Y。E08 的时间轴区块要用绝对定位（SetCursorPos）
    // 从左侧往右摆，而这些控件必须先知道"这一行从哪儿开始"。
    // Begin() 之后、任何控件之前的 CursorPosY 就是内容区首行的顶部。
    const float rowY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosX(std::max(centerX, 4.0f));

    // ---- Run ----
    ImGui::PushStyleColor(ImGuiCol_Button, runActive ? kExitCol : kRunCol);
    if (ImGui::Button(runLabel, ImVec2(btnW(runLabel), 28.0f))) {
        runActive ? m_ctx->stop() : m_ctx->play();
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(runActive
            ? "Stop the running game (editor scene was never touched)."
            : "Run the game in the viewport (logic runs on a copy;\n"
              "the editor scene is left untouched).  Space");
    }

    // ---- 动画 Play / Pause / Stop ----
    ImGui::SameLine();
    ImGui::TextDisabled("|");

    ImGui::SameLine();
    ImGui::BeginDisabled(runActive || m_ctx->animPlaying());
    ImGui::PushStyleColor(ImGuiCol_Button, kPlayCol);
    if (ImGui::Button("Play", ImVec2(btnW("Play"), 28.0f))) m_ctx->animPlay();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !runActive) {
        ImGui::SetTooltip("Run scripts as an animation **on the editing "
                          "scene**.\nStop restores the scene from a snapshot.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!m_ctx->animPlaying());
    ImGui::PushStyleColor(ImGuiCol_Button, kPauseCol);
    if (ImGui::Button("Pause", ImVec2(btnW("Pause"), 28.0f)))
        m_ctx->animPause();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Freeze the animation (state is kept).");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!m_ctx->animActive());
    ImGui::PushStyleColor(ImGuiCol_Button, kStopCol);
    if (ImGui::Button("Stop", ImVec2(btnW("Stop"), 28.0f))) m_ctx->animStop();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Stop the animation and restore the scene to\n"
                          "how it looked when Play was pressed.");
    }

    // ================================================================
    // E08：时间轴 / 帧号 / 倍速 / 单帧步进（左侧区域）
    // ================================================================
    //
    // ⚠⚠ 这一整块**必须是灰阶配色**。
    //   tools/verify_add_menu.py 的断言 2 是这样做的：扫这一行的所有像素，
    //   取"高饱和"（max-min > 45）像素的质心，断言它落在 TR-BAR 水平正中
    //   —— 也就是那四个彩色按钮组的中心。任何**彩色**控件出现在这一行都会
    //   把质心拽偏，于是那条断言误报 FAIL（而按钮其实一动没动）。
    //   所以下面所有 PushStyleColor 都刻意只给灰阶（max-min ≈ 6）。
    //
    // 布局：这一块只吃**左侧**剩余空间（到彩色按钮组左边缘为止），
    // 右侧什么都不放 —— 对称留白，按钮的居中位置与改造前逐像素一致。
    const float leftBudget = std::max(centerX - 24.0f, 0.0f);
    const float clockT = m_ctx->isPlaying() ? m_ctx->playTime()
                                            : m_ctx->animTime();
    // 阈值 300：帧号(~116) + 倍速(~82) + Step(~66) ≈ 264，再给时间轴留
    // 至少 24px。窗口更窄时整块**不画** —— 宁可少一排读数，也不让控件
    // 压到中间那四个彩色按钮上（那会同时破坏观感和像素断言）。
    if (leftBudget > 300.0f) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.21f, 0.23f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.26f, 0.27f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.30f, 0.31f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,
                              ImVec4(0.42f, 0.43f, 0.46f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,
                              ImVec4(0.52f, 0.53f, 0.56f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.17f, 0.18f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                              ImVec4(0.23f, 0.24f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                              ImVec4(0.27f, 0.28f, 0.31f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(st.FramePadding.x, 7.0f));

        float x = st.WindowPadding.x;

        // ---- 帧号 + 时钟读数 ----
        {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "f%-6d %.2fs",
                          static_cast<int>(clockT * 60.0f + 0.5f), clockT);
            ImGui::SetCursorPos(ImVec2(x, rowY + 4.0f));
            ImGui::TextDisabled("%s", buf);
            x += ImGui::CalcTextSize("f000000 00.00s").x + 10.0f;
        }

        // ---- 倍速 ----
        {
            static const float kSpeeds[3] = {0.25f, 1.0f, 2.0f};
            static const char* kSpeedNames[3] = {"0.25x", "1x", "2x"};
            int cur = 1;
            for (int i = 0; i < 3; ++i) {
                if (std::fabs(m_timeScale - kSpeeds[i]) < 0.01f) cur = i;
            }
            ImGui::SetCursorPos(ImVec2(x, rowY));
            ImGui::SetNextItemWidth(74.0f);
            char item[32];
            std::snprintf(item, sizeof(item), "%s##speed", kSpeedNames[cur]);
            if (ImGui::BeginCombo(item, kSpeedNames[cur])) {
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::Selectable(kSpeedNames[i], i == cur)) {
                        m_timeScale = kSpeeds[i];
                        m_ctx->notify(std::string("Playback speed: ") +
                                      kSpeedNames[i]);
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Playback speed (applies to scripts, "
                                  "players and collision)");
            }
            x += 74.0f + 8.0f;
        }

        // ---- 单帧步进 ----
        {
            // 只在动画**暂停**时可用：那时前进一帧是安全的（Stop 的快照
            // 还在，能整体回滚）。编辑态随便步进会在快照之外改场景 ——
            // 那种"改了但撤销不了"的操作不该由一个按钮提供。
            const bool canStep = m_ctx->animState() == AnimState::Paused;
            ImGui::SetCursorPos(ImVec2(x, rowY));
            ImGui::BeginDisabled(!canStep);
            if (ImGui::Button("Step", ImVec2(56.0f, 28.0f))) {
                const float oneFrame = 1.0f / 60.0f;
                m_ctx->advanceAnimTime(oneFrame);
                runAnimScripts(oneFrame);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(canStep
                    ? "Advance the animation by exactly one frame (1/60 s)"
                    : "Pause the animation first (Play/Pause/Stop row)");
            }
            x += 56.0f + 10.0f;
        }

        // ---- 时间轴 ----
        {
            const float sliderW = std::max(leftBudget - x - 8.0f, 24.0f);
            float scrub = clockT;
            const float tmax = std::max(clockT + 0.5f, 1.0f);
            ImGui::SetCursorPos(ImVec2(x, rowY));
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::SliderFloat("##timeline", &scrub, 0.0f, tmax, "%.2f s")) {
                if (m_ctx->isPlaying()) m_ctx->setPlayTime(scrub);
                else m_ctx->setAnimTime(scrub);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Timeline: drag to scrub the script clock.\n"
                                  "A real sequencer (keyframes / tracks) "
                                  "lands with E24.");
            }
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(8);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- 统计面板

void EditorApp::drawStatsPanel() {
    render::Renderer& r = renderer();
    const ImGuiIO& io = ImGui::GetIO();

    // 浮在 Scripts 面板上方（两者都在右下角），同时打开也不互相压住。
    // E15 起默认加宽到 660：内容分 CPU/GPU 双栏，360 的旧宽度塞不下。
    const float w660 = 660.0f;
    const float x = io.DisplaySize.x - w660 - kFloatMargin;
    const float y = io.DisplaySize.y - kStatusH - kScriptsH - kFloatGap - kStatsH;

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w660, 0.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Stats", &m_ctx->panels().stats)) {
        ImGui::End();
        return;
    }

    // ---- E15 历史曲线：帧时间 / FPS 各 120 帧滚动（原来只有瞬时值）----
    static float ftHist[120] = {};
    static float fpsHist[120] = {};
    const float ftMs =
        io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f;
    std::memmove(ftHist, ftHist + 1, sizeof(float) * 119);
    ftHist[119] = ftMs;
    std::memmove(fpsHist, fpsHist + 1, sizeof(float) * 119);
    fpsHist[119] = io.Framerate;
    ImGui::Text("FPS: %.1f  (%.2f ms)", io.Framerate, ftMs);
    ImGui::PlotLines("##ft-hist", ftHist, 120, 0, "frame time (ms) / 120f",
                     0.0f, 50.0f, ImVec2(-1.0f, 40.0f));
    ImGui::PlotLines("##fps-hist", fpsHist, 120, 0, "fps / 120f", 0.0f, 240.0f,
                     ImVec2(-1.0f, 40.0f));

    // ---- E15 CPU / GPU 双栏 ----
    ImGui::Columns(2, "statscols", false);
    ImGui::SetColumnWidth(0, w660 * 0.56f);

    // ================= 左栏：CPU 侧 =================
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

    // ---- 物体级视锥剔除的实际效果 ----
    // "drawn / total"：实际提交的物体数 / 场景里可见的网格实体总数。
    // culled 为两者之差；draw calls 与 triangles 是提交量（不是着色量），
    // 正是视锥剔除要压下去的那部分 CPU 提交开销。
    {
        const render::Renderer::RenderStats& rs = r.renderStats();
        const uint32_t drawn = rs.drawnOpaque + rs.drawnTransparent;
        ImGui::Text("Objects: %u drawn / %u", drawn, rs.renderables);
        if (!r.frustumCullingEnabled()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                               "[frustum culling off]");
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled("(culled %u, lights %u)", rs.culledFrustum,
                                rs.gizmosCulled);
        }
        // Hi-Z 遮挡剔除：occl = 本帧因"被挡住"没进绘制列表的物体数；
        // tested = 两帧前那次 GPU 实际测过的候选数（上限 kMaxObjects）。
        if (r.occlusionCullingEnabled()) {
            const render::HiZStats& hs = r.hizStats();
            ImGui::SameLine();
            ImGui::TextDisabled("| Hi-Z tested %u, occluded %u (%u mips)",
                                hs.objects, rs.culledOcclusion, hs.mipCount);
        }
        ImGui::Text("Draw calls: %u   Triangles: %u", rs.drawCalls,
                    rs.triangles);

        // ---- 空间索引（BVH）----
        // visits = 本帧视锥查询**实际访过**的节点数。它远小于 nodes 才说明
        // 子树真的被剪掉了；如果逼近 nodes，说明树建歪了（退化成了全遍历）。
        const scene::Scene::SpatialStats bs =
            m_ctx->activeScene().spatialStats();
        ImGui::Text("BVH: %llu nodes / %llu objects / depth %llu, visits %u",
                    static_cast<unsigned long long>(bs.nodes),
                    static_cast<unsigned long long>(bs.entities),
                    static_cast<unsigned long long>(bs.depth),
                    rs.bvhNodeVisits);

        // ---- LOD ----
        // 直接给出"用 LOD / 不用 LOD"的三角形数对照 —— 这是这套系统唯一
        // 值得看的数字。noLod 是假设所有物体都用 0 级时的提交量。
        if (rs.lodEntities > 0) {
            const float saved = rs.trianglesNoLod > 0
                ? 100.0f * (1.0f - static_cast<float>(rs.triangles) /
                                     static_cast<float>(rs.trianglesNoLod))
                : 0.0f;
            ImGui::Text("LOD: %u ents  L0 %u / L1 %u / L2 %u / L3 %u",
                        rs.lodEntities, rs.lodLevelCounts[0],
                        rs.lodLevelCounts[1], rs.lodLevelCounts[2],
                        rs.lodLevelCounts[3]);
            ImGui::SameLine();
            ImGui::TextDisabled("(tris %u vs %u, -%.0f%%)", rs.triangles,
                                rs.trianglesNoLod,
                                static_cast<double>(saved));
        } else {
            ImGui::TextDisabled("LOD: 没有带 LOD 链的可见物体");
        }

        // ---- 环境（天空盒 + IBL）----
        if (const render::Environment* env = r.environment()) {
            const render::Environment::Stats& es = env->stats();
            ImGui::Text("Env: sky %u / irr %u / pref %u x%u mips / lut %u",
                        es.skySize, es.irradianceSize, es.prefilterSize,
                        es.prefilterMips, es.lutSize);
            ImGui::SameLine();
            ImGui::TextDisabled("(sky %s, IBL %s)",
                                r.skyEnabled() ? "on" : "off",
                                r.iblEnabled() ? "split-sum" : "approx");
        } else {
            ImGui::TextDisabled("Env: 环境贴图未就绪");
        }

        // ---- 级联阴影（CSM）----
        if (const render::ShadowPass* sp = r.shadowPass()) {
            ImGui::Text("Shadow: %u casc %ux%u  dist %.0fm  %s",
                        sp->cascadeCount(), sp->resolution(), sp->resolution(),
                        static_cast<double>(sp->maxDistance()),
                        r.shadowsEnabled() ? "on" : "off");
            uint32_t sub = 0, con = 0;
            for (uint32_t c = 0; c < sp->cascadeCount(); ++c) {
                sub += sp->submitted(c);
                con += sp->considered(c);
            }
            // "提交/考虑"就是每级裁剪空间剔除的效果：没有剔除时
            // 提交量会正好等于 考虑数 × 级联数。
            ImGui::TextDisabled(
                "splits %.1f / %.1f / %.1f / %.1f | submitted %u of %u "
                "(per-cascade cull)",
                static_cast<double>(sp->split(0)),
                static_cast<double>(sp->split(1)),
                static_cast<double>(sp->split(2)),
                static_cast<double>(sp->split(3)), sub, con);
        }

        // ---- 局部光源阴影（点光 / 聚光灯）----
        if (const render::LocalShadowPass* lsp = r.localShadowPass()) {
            ImGui::Text("Local shadow: %ux%u x %u layers, used %u  %s",
                        lsp->resolution(), lsp->resolution(),
                        lsp->layerCount(), r.localShadowLayersUsed(),
                        r.localShadowsEnabled() ? "on" : "off");
            uint32_t sub = 0, con = 0;
            for (uint32_t l = 0; l < lsp->layerCount(); ++l) {
                sub += lsp->submitted(l);
                con += lsp->considered(l);
            }
            // 与级联同理：没有逐层剔除时，提交量会等于 考虑数 × 用掉的层数。
            ImGui::TextDisabled(
                "submitted %u of %u (per-layer cull) | max %u lamps",
                sub, con, r.localShadowMaxLights());
        }

        // ---- Shader 变体 / 关键字（见 render/ShaderKeywords.h）----
        // 第一行是"惰性创建有没有失控"的体温计：一个只用得到 3 种材质组合
        // 的场景应当稳定停在 3；数字随帧数一直涨就说明缓存根本没命中。
        // switches 是每帧真的发生了几次 vkCmdBindPipeline —— 同材质的相邻
        // 物体只付一次，所以它通常远小于绘制数。
        if (r.forwardPipelines().initialized()) {
            ImGui::Text("Shader vars: %u fwd + %u depth, %u switches",
                        r.forwardPipelines().created(),
                        r.depthPrePipelines().created(), rs.variantSwitches);

            std::string vars, tip;
            for (uint32_t m = 0; m <= render::kKeywordAll; ++m) {
                if (rs.variantCounts[m] == 0) continue;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%sk%u:%u",
                              vars.empty() ? "" : " ", m, rs.variantCounts[m]);
                vars += buf;

                char tbuf[96];
                std::snprintf(tbuf, sizeof(tbuf), "k%u  %s  x%u\n", m,
                              render::keywordNames(m).c_str(),
                              rs.variantCounts[m]);
                tip += tbuf;
            }
            ImGui::TextDisabled("%s", vars.empty() ? "(本帧无前向变体绘制)"
                                                   : vars.c_str());
            // 掩码是位集合，"k5" 这种编号不看名字是读不出来的 → 悬停给出映射。
            if (!tip.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("本帧各变体画了几个物体：\n%s"
                                  "\nbit0 double-sided / bit1 alpha-blend / "
                                  "bit2 normal-map", tip.c_str());
            }

            // 强制掩码：对照实验留下的印记。非 -1 时画面是被"篡改"过的，
            // 必须显眼 —— 否则看到的人会以为材质开关失效了。
            if (r.forcedKeywordMask() >= 0) {
                const auto fm = static_cast<render::KeywordMask>(
                    r.forcedKeywordMask());
                ImGui::TextColored(
                    ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                    "FORCED mask %d: %s", r.forcedKeywordMask(),
                    render::keywordNames(fm).c_str());
            }
        }
    }
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

    // ================= 右栏：GPU 侧（时戳剖析）=================
    ImGui::NextColumn();
    if (const render::GpuTimer* gt = r.gpuTimer()) {
        if (gt->valid() && gt->lastTotalMs() >= 0.0f) {
            const float total = gt->lastTotalMs();
            ImGui::Text("GPU: %.2f ms/frame", total);

            // ---- E15 时戳堆叠条形图：10 段按占比横向铺满一条 ----
            // 一眼看出哪个 pass 最贵；悬停读精确毫秒（R0-1 的数据早就
            // 就绪，缺的只是这可视化）。≈0 的段不画。
            static const ImVec4 kSegCol[render::GpuTimer::kSectionCount] = {
                {0.90f, 0.47f, 0.27f, 1.0f},   // shadows
                {0.86f, 0.31f, 0.63f, 1.0f},   // local_shadows
                {0.47f, 0.71f, 1.00f, 1.0f},   // depth_pre
                {0.35f, 0.86f, 0.67f, 1.0f},   // decals
                {0.94f, 0.78f, 0.31f, 1.0f},   // ssao
                {0.63f, 0.47f, 1.00f, 1.0f},   // cluster
                {0.31f, 0.78f, 0.47f, 1.0f},   // forward
                {1.00f, 0.55f, 0.15f, 1.0f},   // particles (R6-1)
                {0.27f, 0.59f, 0.90f, 1.0f},   // framegraph
                {0.78f, 0.78f, 0.35f, 1.0f},   // offscreen
                {0.59f, 0.59f, 0.63f, 1.0f},   // post
            };
            const float barW = ImGui::GetContentRegionAvail().x;
            const float barH = 16.0f;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float acc = 0.0f;
            for (uint32_t s = 0; s < render::GpuTimer::kSectionCount; ++s) {
                const float ms = gt->lastMs(s);
                const float frac = (total > 0.0f) ? ms / total : 0.0f;
                if (barW * frac >= 0.75f) {
                    dl->AddRectFilled(
                        ImVec2(p0.x + barW * acc, p0.y),
                        ImVec2(p0.x + barW * (acc + frac), p0.y + barH),
                        ImGui::ColorConvertFloat4ToU32(kSegCol[s]));
                }
                acc += frac;
            }
            dl->AddRect(p0, ImVec2(p0.x + barW, p0.y + barH),
                        IM_COL32(0, 0, 0, 120));
            ImGui::Dummy(ImVec2(barW, barH));
            if (ImGui::IsItemHovered()) {
                // 鼠标落在哪一段：按累计占比反查
                const float rel =
                    (io.MousePos.x - p0.x) / std::max(barW, 1.0f);
                float a = 0.0f;
                for (uint32_t s = 0; s < render::GpuTimer::kSectionCount; ++s) {
                    a += (total > 0.0f) ? gt->lastMs(s) / total : 0.0f;
                    if (rel <= a) {
                        ImGui::SetTooltip("%s  %.3f ms  (%.1f%%)",
                                          gt->sectionName(s),
                                          static_cast<double>(gt->lastMs(s)),
                                          static_cast<double>(
                                              100.0f * gt->lastMs(s) /
                                              std::max(total, 1e-6f)));
                        break;
                    }
                }
            }

            // ---- 段明细（= 图例 + 数字）：贵段亮色，≈0 灰显 ----
            float maxSeg = 0.0f;
            for (uint32_t s = 0; s < render::GpuTimer::kSectionCount; ++s)
                maxSeg = std::max(maxSeg, gt->lastMs(s));
            for (uint32_t s = 0; s < render::GpuTimer::kSectionCount; ++s) {
                const float ms = gt->lastMs(s);
                if (ms > maxSeg * 0.05f && ms > 0.005f) {
                    ImGui::ColorButton("##chip", kSegCol[s],
                                       ImGuiColorEditFlags_NoTooltip,
                                       ImVec2(10.0f, 10.0f));
                    ImGui::SameLine();
                    ImGui::Text("%s %.2f", gt->sectionName(s),
                                static_cast<double>(ms));
                } else {
                    ImGui::ColorButton("##chip", ImVec4(0.3f, 0.3f, 0.33f, 1.0f),
                                       ImGuiColorEditFlags_NoTooltip,
                                       ImVec2(10.0f, 10.0f));
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s -", gt->sectionName(s));
                }
            }
        } else {
            ImGui::TextDisabled("GPU: 时戳数据未就绪");
        }
    }

    ImGui::Columns(1);
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
    // E09：面板主体从 FeatureRegistry 生成（editor/FeatureUIGen）。写值
    // 走注册表绑定的地址 —— 与 env 路径**同一份存储**。这里只剩
    // ① 顶栏（档位 / 搜索 / 画质预设）② 生成器表达不了的四小块
    // （见下面四个 drawEngineXxx：动作型控件与只读信息）。
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

    // ---- 顶栏：档位（100+ 旋钮平铺没人能用，默认只给 Basic）+ 搜索 ----
    static int tier = 0;               // 0=Basic 1=Advanced 2=Debug
    static char search[64] = "";
    ImGui::TextUnformatted("Show");
    ImGui::SameLine();
    ImGui::RadioButton("Basic", &tier, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Adv", &tier, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Debug", &tier, 2);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ftrsearch", "Search features / params...",
                             search, sizeof(search));

    // ---- 画质预设（E09；质量参数的档位化是 E26 的数据活，这里只拨开关）----
    static int preset = -1;            // -1 = Custom
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo(
            "Preset", preset < 0 ? "Custom" : editor::qualityPresetName(preset))) {
        for (int i = 0; i < editor::qualityPresetCount(); ++i) {
            if (ImGui::Selectable(editor::qualityPresetName(i), preset == i)) {
                preset = i;
                editor::applyQualityPreset(i);
            }
        }
        ImGui::EndCombo();
    }

    // ---- 生成区：全部特性按 registry 的 group 分节，逐块生成 ----
    const char* lastGroup = nullptr;
    for (int i = 0; i < render::kFeatureCount; ++i) {
        const render::FeatureDesc& f = render::kFeatures[i];
        if (!f.group) continue;
        if (!lastGroup || std::strcmp(lastGroup, f.group) != 0) {
            ImGui::SeparatorText(f.group);
            lastGroup = f.group;
        }
        editor::drawFeatureBlock(f, tier, search, r.gpuTimer());
    }

    drawEngineCameras();
    drawEngineViewMode();
    drawEngineMsaa();
    drawEngineClusterStats();

    ImGui::End();
}

// ---------------------------------------- E09 特殊块（生成器表达不了的）

void EditorApp::drawEngineCameras() {
    // 多相机：主视口用场景里的哪台相机（0 = 默认）。相机数量由场景决定，
    // 编辑器可以在这里追加（新相机从 0 号复制，改它的位置/朝向即可）。
    render::Renderer& r = renderer();
    scene::Scene& sc = activeScene();
    int camIdx = static_cast<int>(r.viewCameraIndex());
    const int camCount = std::max(1, static_cast<int>(sc.cameraCount()));
    ImGui::SeparatorText("Cameras");
    ImGui::Text("Cameras: %d", camCount);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add")) sc.addCamera();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy 1st -> current")) {
        if (sc.cameraCount() > 1) {
            sc.camera(static_cast<size_t>(camIdx)) = sc.camera(0);
        }
    }
    if (ImGui::SliderInt("View camera", &camIdx, 0, camCount - 1)) {
        r.setViewCameraIndex(static_cast<size_t>(std::max(camIdx, 0)));
    }
}

void EditorApp::drawEngineViewMode() {
    // 调试视图模式（R0-2）。VM-COMBO 是 verify_viewmodes 的断言锚点
    // （labels=8 + 只打一次），保持原样别挪。Lit = 默认渲染路径；其余
    // 是排查工具；wireframe 不支持 fillModeNonSolid 的设备会静默回退。
    render::Renderer& r = renderer();
    static const char* kViewModeLabels[] = {
        "Lit", "Unlit", "Normal", "Roughness", "Metallic",
        "Wireframe", "Overdraw", "Light Complexity"};
    int vmIdx = static_cast<int>(r.viewMode());
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("View mode", &vmIdx, kViewModeLabels, 8)) {
        r.setViewMode(static_cast<render::Renderer::ViewMode>(vmIdx));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Lit = 默认渲染（基线）\n"
            "Wireframe 需设备支持 fillModeNonSolid");
    }
    static bool vmTagged = false;
    if (!vmTagged) {
        vmTagged = true;
        if (std::getenv("MYVK_LOG_RECTS")) {
            VK_LOG_INFO("VM-COMBO labels=8 current=%s",
                        render::Renderer::viewModeName(r.viewMode()));
        }
    }
}

void EditorApp::drawEngineMsaa() {
    // MSAA：切换会重建 HdrTarget 与前向管线（视口离屏链保留，编辑器
    // 纹理 ID 不失效）。注册表只管"值"管不了"重建"这个动作，所以它是
    // 特殊块 —— 走 setSampleCount 专用路径。
    render::Renderer& r = renderer();
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
}

void EditorApp::drawEngineClusterStats() {
    render::Renderer& r = renderer();
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
}

// ---------------------------------------------------------------- E02 钩子

void EditorApp::onFeatureParamChanged(const char* paramId) {
    VK_LOG_INFO("FTR-UI %s", paramId);
    // 生成器只会写"值"；个别参数还伴随"动作"，集中在这里处理：
    if (std::strcmp(paramId, "rp.bindless") == 0) {
        // bindless 切换要重建前向管线。生成器已把成员写成新值，而
        // setBindlessEnabled 的短路判断（on == m_bindlessEnabled）会误
        // 认为"没变"——先写回反值，让它走完整重建路径；设备不支持时
        // 它内部 WARN 并保持关闭。
        if (void* p = render::paramPtr("rp.bindless")) {
            const bool want = *static_cast<bool*>(p);
            *static_cast<bool*>(p) = !want;
            renderer().setBindlessEnabled(want);
        }
    }
}

// ---------------------------------------------------------------- E19 设置可撤销
namespace {

// 把参数当前值格式化成人话（进历史的名字用）。
// Enum 取选项名，Bool 用 on/off，Vec3/Color3 用 (r,g,b)。
std::string formatParamValue(const render::ParamDesc* p, const void* data) {
    if (!p || !data) return "?";
    char buf[96];
    switch (p->kind) {
    case render::ParamKind::Bool:
        return *static_cast<const bool*>(data) ? "on" : "off";
    case render::ParamKind::Int:
        std::snprintf(buf, sizeof(buf), "%d",
                      *static_cast<const int*>(data));
        return buf;
    case render::ParamKind::Enum: {
        const int idx = *static_cast<const int*>(data);
        int n = 0;
        while (p->enumNames && p->enumNames[n]) ++n;
        if (idx >= 0 && idx < n) return p->enumNames[idx];
        std::snprintf(buf, sizeof(buf), "%d", idx);
        return buf;
    }
    case render::ParamKind::Float:
        std::snprintf(buf, sizeof(buf), "%.3f",
                      static_cast<double>(*static_cast<const float*>(data)));
        return buf;
    case render::ParamKind::Color3:
    case render::ParamKind::Vec3: {
        const float* v = static_cast<const float*>(data);
        std::snprintf(buf, sizeof(buf), "(%.2f,%.2f,%.2f)",
                      static_cast<double>(v[0]), static_cast<double>(v[1]),
                      static_cast<double>(v[2]));
        return buf;
    }
    }
    return "?";
}

// "Set SSR Strength" —— 用所属特性的**短名**（.enabled 参数的 label，
// 例如 "SSR"），比用全名 "Screen Space Reflections Strength" 好用。
// 特性开关本身（label 就是短名）不加前缀，免得写成 "Set SSR SSR"。
std::string paramHistoryLabel(const render::ParamDesc* p, const char* paramId) {
    std::string shortName;
    if (const render::FeatureDesc* f = render::ownerFeature(paramId)) {
        const std::string enId = std::string(f->id) + ".enabled";
        if (const render::ParamDesc* en = render::findParam(enId.c_str()))
            shortName = en->label ? en->label : f->name;
        else
            shortName = f->name;
    }
    const std::string pl = (p && p->label) ? p->label : paramId;
    if (shortName.empty() || pl == shortName) return "Set " + pl;
    return "Set " + shortName + " " + pl;
}

}  // namespace

void EditorApp::onFeatureParamUndo(const char* paramId, const void* before,
                                   const void* after, std::size_t size) {
    if (!paramId || !before || !after || size == 0) return;
    void* target = render::paramPtr(paramId);
    if (!target) return;  // 未绑定 → 没地方回滚，不记历史

    const render::ParamDesc* p = render::findParam(paramId);
    auto toBytes = [size](const void* src) {
        std::vector<std::uint8_t> v(size);
        std::memcpy(v.data(), src, size);
        return v;
    };
    const std::string afterStr = formatParamValue(p, after);

    // ---- 合并：同一参数、500ms 内、栈顶就是它 → 只更新 after ----
    const double now = ImGui::GetTime();
    if (m_lastParamCmd && m_lastParamId == paramId &&
        (now - m_lastParamTime) < 0.5 &&
        m_ctx->commands().topCommand() == m_lastParamCmd) {
        m_lastParamCmd->mergeAfter(toBytes(after), afterStr);
        m_lastParamTime = now;
        VK_LOG_INFO("UNDO-PARAM merge id=%s name=\"%s\"", paramId,
                    m_lastParamCmd->name());
        return;
    }

    auto cmd = std::make_unique<ParamEditCommand>(
        paramHistoryLabel(p, paramId), paramId, target, toBytes(before),
        formatParamValue(p, before), toBytes(after), afterStr);
    m_lastParamCmd = cmd.get();
    m_lastParamId = paramId;
    m_lastParamTime = now;
    const char* nm = cmd->name();
    m_ctx->commands().pushAlreadyApplied(std::move(cmd));  // 值已经改过了
    VK_LOG_INFO("UNDO-PARAM push id=%s name=\"%s\"", paramId, nm);
}

// ---------------------------------------------------------------- Console（E16）

void EditorApp::drawConsolePanel() {
    m_console->draw(&m_ctx->panels().console);
}

// E25：GPU 时戳时间轴。
// 与其它浮动面板同一条路：显隐 bool 直接取表里的那一格（点标题栏的 ×
// 写回的就是它）。Console 以入参传进去 —— 面板之间不互相引用是硬约束，
// 所以"点段 → 过滤 Console"这根线由宿主在调用点上接。
void EditorApp::drawGpuTimelinePanel() {
    if (!m_gpuTimeline) return;
    m_gpuTimeline->draw(&panelOpenRef("gputimeline"), m_console.get());
}

void EditorApp::registerConsoleCommands() {
    using render::ParamDesc;
    using render::ParamKind;

    // "ssr" → "ssr.enabled"：允许敲特性 id 就操作它的启用开关。
    auto resolveParam = [](const std::string& raw) -> const ParamDesc* {
        if (const ParamDesc* p = render::findParam(raw.c_str())) return p;
        const size_t dot = raw.find('.');
        if (dot == std::string::npos) {
            const std::string en = raw + ".enabled";
            if (const ParamDesc* p = render::findParam(en.c_str())) return p;
        }
        return nullptr;
    };

    auto* con = m_console.get();

    con->addCommand("help", "列出全部命令", [con](const std::vector<std::string>&) {
        return std::string(
            "help / clear / set <id> <v> / get <id> / reset <id|feature> / "
            "params <feature> / preset <Low|Medium|High|Epic|Cinematic> / "
            "stat gpu / insp.selftest / undo.selftest / notif.push|dump|click|"
            "task|progress|cancel|done");
    });
    con->addCommand("clear", "清空日志缓冲",
                    [con](const std::vector<std::string>&) {
                        con->clear();
                        return std::string();
                    });
    con->addCommand("set", "set <paramId> <value>  写注册表参数（特性 id 等价于 <id>.enabled）",
        [resolveParam](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 3) return "usage: set <paramId> <value>";
            const ParamDesc* p = resolveParam(args[1]);
            if (!p) return "unknown param '" + args[1] + "'";
            void* ptr = render::paramPtr(p->id);
            if (!ptr) return "param not bound: " + std::string(p->id);
            const std::string& v = args[2];
            switch (p->kind) {
            case ParamKind::Bool: {
                bool on = !(v == "0" || v == "off" || v == "false" ||
                            v == "Off" || v == "OFF");
                *static_cast<bool*>(ptr) = on;
                break;
            }
            case ParamKind::Int:
            case ParamKind::Enum:
                *static_cast<int*>(ptr) = std::atoi(v.c_str());
                break;
            default:  // Float / Color3[0] / Vec3[0]
                *static_cast<float*>(ptr) = std::strtof(v.c_str(), nullptr);
                break;
            }
            editor::emitParamChange(p->id);   // 与 UI 路径同一套副作用
            return p->id + std::string(" = ") + v;
        });
    con->addCommand("get", "get <paramId>  读注册表参数当前值",
        [resolveParam](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: get <paramId>";
            const ParamDesc* p = resolveParam(args[1]);
            if (!p) return "unknown param '" + args[1] + "'";
            void* ptr = render::paramPtr(p->id);
            if (!ptr) return "param not bound: " + std::string(p->id);
            char buf[64];
            switch (p->kind) {
            case ParamKind::Bool:
                std::snprintf(buf, sizeof(buf), "%s = %s", p->id,
                              *static_cast<bool*>(ptr) ? "true" : "false");
                break;
            case ParamKind::Enum: {
                const int idx = *static_cast<int*>(ptr);
                const char* nm = "(?)";
                int n = 0;
                while (p->enumNames && p->enumNames[n]) ++n;
                if (idx >= 0 && idx < n) nm = p->enumNames[idx];
                std::snprintf(buf, sizeof(buf), "%s = %d (%s)", p->id, idx, nm);
                break;
            }
            case ParamKind::Int:
                std::snprintf(buf, sizeof(buf), "%s = %d", p->id,
                              *static_cast<int*>(ptr));
                break;
            default:
                std::snprintf(buf, sizeof(buf), "%s = %.4f", p->id,
                              static_cast<double>(*static_cast<float*>(ptr)));
                break;
            }
            return buf;
        });
    con->addCommand("reset", "reset <paramId|featureId>  恢复默认",
        [](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: reset <paramId|featureId>";
            if (const render::FeatureDesc* f = render::findFeature(args[1].c_str())) {
                int n = 0;
                for (int i = 0; i < f->paramCount; ++i)
                    n += render::resetParam(f->params[i].id) ? 1 : 0;
                if (f->paramCount > 0) editor::emitParamChange(f->params[0].id);
                return "reset " + std::string(f->id) + " (" +
                       std::to_string(n) + " params)";
            }
            if (render::resetParam(args[1].c_str())) {
                editor::emitParamChange(args[1].c_str());
                return "reset " + args[1];
            }
            return "unknown param or feature '" + args[1] + "'";
        });
    con->addCommand("params", "params <featureId>  列出某特性的全部参数",
        [](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: params <featureId>";
            const render::FeatureDesc* f =
                render::findFeature(args[1].c_str());
            if (!f) return "unknown feature '" + args[1] + "'";
            std::string out;
            for (int i = 0; i < f->paramCount; ++i) {
                out += "  ";
                out += f->params[i].id;
                out += "\n";
            }
            return out;
        });
    con->addCommand("preset", "preset <Low|Medium|High|Epic|Cinematic>",
        [](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: preset <name>";
            for (int i = 0; i < editor::qualityPresetCount(); ++i)
                if (args[1] == editor::qualityPresetName(i)) {
                    editor::applyQualityPreset(i);
                    return std::string("preset applied: ") +
                           editor::qualityPresetName(i);
                }
            return "unknown preset '" + args[1] + "'";
        });
    con->addCommand("stat", "stat gpu  往控制台打一份 GPU 时戳",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2 || args[1] != "gpu") return "usage: stat gpu";
            const render::GpuTimer* gt = renderer().gpuTimer();
            if (!gt || !gt->valid() || gt->lastTotalMs() < 0.0f)
                return "GPU timestamps not ready";
            std::string out = "GPU total " +
                std::to_string(gt->lastTotalMs()) + " ms\n";
            for (uint32_t s = 0; s < render::GpuTimer::kSectionCount; ++s) {
                out += "  ";
                out += gt->sectionName(s);
                out += " " + std::to_string(gt->lastMs(s)) + " ms\n";
            }
            return out;
        });
    // E11：Inspector 自检（剪贴板往返 / Reset / 折叠持久化）。
    // 日志形如 INSP-SELFTEST copy-paste=OK reset=OK collapse-persist=OK，
    // 供 tools/verify_inspector.py 断言。
    con->addCommand("insp.selftest", "insp.selftest  Inspector 剪贴板/重置自检",
        [this](const std::vector<std::string>&) -> std::string {
            std::string out;
            const bool ok = m_inspector->selfTest(out);
            core::logMessage(core::LogLevel::Info, "INSP-SELFTEST %s",
                             out.c_str());
            return std::string(ok ? "OK " : "FAIL ") + out;
        });
    // E19：设置类改动的撤销自检。走的是与 UI **完全同一条** hook
    // （onFeatureParamUndo），所以验的是真实路径，不是另写一套。
    //   1) 改一次 → 应 push 一条历史
    //   2) 500ms 内再改一次 → 应**合并**进同一条（depth 不变）
    //   3) undo → 值回到最初；redo → 值回到第二次编辑后；再 undo 复原
    // 日志形如 UNDO-SELFTEST param=ssr.strength kind=4 merged=1 undoOk=1
    // redoOk=1 restored=1 depthDelta=0，供 tools/verify_undo_settings.py 断言。
    con->addCommand("undo.selftest",
                    "undo.selftest [paramId]  设置类改动撤销自检",
        [this](const std::vector<std::string>& args) -> std::string {
            const std::string pid = args.size() > 1 ? args[1] : "ssr.strength";
            const render::ParamDesc* p = render::findParam(pid.c_str());
            if (!p) return "unknown param '" + pid + "'";
            void* ptr = render::paramPtr(pid.c_str());
            if (!ptr) return "param not bound: " + pid;

            const render::ParamKind k = p->kind;
            const std::size_t size =
                (k == render::ParamKind::Bool)
                    ? sizeof(bool)
                    : ((k == render::ParamKind::Color3 ||
                        k == render::ParamKind::Vec3)
                           ? sizeof(float) * 3
                           : ((k == render::ParamKind::Float) ? sizeof(float)
                                                              : sizeof(int)));

            std::vector<std::uint8_t> orig, edited, edited2;
            auto snap = [&](std::vector<std::uint8_t>& out) {
                out.resize(size);
                std::memcpy(out.data(), ptr, size);
            };
            auto same = [&](const std::vector<std::uint8_t>& a) {
                return std::memcmp(a.data(), ptr, size) == 0;
            };
            // 把值改成一个"肯定不同"的新值（按 kind 挑）
            auto perturb = [&]() {
                if (k == render::ParamKind::Bool) {
                    *static_cast<bool*>(ptr) = !*static_cast<bool*>(ptr);
                } else if (k == render::ParamKind::Float) {
                    float* v = static_cast<float*>(ptr);
                    const float mid = (p->lo + p->hi) * 0.5f;
                    *v = (*v > mid) ? p->lo : p->hi;
                } else {
                    int* v = static_cast<int*>(ptr);
                    *v = (*v == static_cast<int>(p->lo))
                             ? static_cast<int>(p->hi)
                             : static_cast<int>(p->lo);
                }
            };

            snap(orig);
            perturb();
            snap(edited);
            onFeatureParamUndo(pid.c_str(), orig.data(), ptr, size);
            const int d1 = static_cast<int>(m_ctx->commands().depth());

            std::vector<std::uint8_t> beforeSecond;
            snap(beforeSecond);
            perturb();
            snap(edited2);
            onFeatureParamUndo(pid.c_str(), beforeSecond.data(), ptr, size);
            const int d2 = static_cast<int>(m_ctx->commands().depth());

            m_ctx->undo();
            const bool undoOk = same(orig);
            m_ctx->redo();
            const bool redoOk = same(edited2);
            m_ctx->undo();  // 复原：值回 orig，历史游标也回编辑前
            const bool restored = same(orig);

            const int merged = (d2 == d1) ? 1 : 0;
            const bool ok = merged == 1 && undoOk && redoOk && restored;
            core::logMessage(core::LogLevel::Info,
                             "UNDO-SELFTEST param=%s kind=%d merged=%d "
                             "undoOk=%d redoOk=%d restored=%d depthDelta=%d",
                             pid.c_str(), static_cast<int>(k), merged,
                             undoOk ? 1 : 0, redoOk ? 1 : 0, restored ? 1 : 0,
                             d2 - d1);
            return std::string(ok ? "OK " : "FAIL ") + pid;
        });

    // ---- E21：通知 / 进度的自检命令 ----
    //   notif.push <info|warn|error> <text>  弹一条通知
    //       ⚠ warn/error 走**真实日志路径**（VK_LOG_WARN/ERROR），所以它同时
    //       会进 Console 缓冲 —— "点 toast 打开 Console 定位"才有东西可定位。
    //       info 走显式 push（Info 不自动升级成 toast）。
    //   notif.dump                     打一份计数快照（状态栏四项的来源）
    //   notif.click <idx>              模拟点第 idx 条 toast（UI 与脚本同一条路）
    //   notif.task / notif.cancel / notif.progress   进度任务的手动驱动
    con->addCommand("notif.push",
                    "notif.push <info|warn|error> <text>  弹一条通知",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 3)
                return "usage: notif.push <info|warn|error> <text>";
            const std::string& lv = args[1];
            std::string text;
            for (size_t i = 2; i < args.size(); ++i) {
                if (i > 2) text += ' ';
                text += args[i];
            }
            if (lv == "warn")
                VK_LOG_WARN("notif.push: %s", text.c_str());
            else if (lv == "error")
                VK_LOG_ERROR("notif.push: %s", text.c_str());
            else if (lv == "info")
                m_notifications->push(core::LogLevel::Info, text);
            else
                return "unknown level '" + lv + "' (info|warn|error)";
            return "pushed " + lv;
        });
    con->addCommand("notif.dump", "notif.dump  通知 / 进度计数快照",
        [this](const std::vector<std::string>&) -> std::string {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "NOTIF-DUMP toasts=%d history=%d warn=%d error=%d "
                          "unread=%d tasks=%d minimized=%d",
                          m_notifications->toastCount(),
                          m_notifications->historyCount(),
                          m_notifications->warnCount(),
                          m_notifications->errorCount(),
                          m_notifications->unread(),
                          m_notifications->taskCount(),
                          m_notifications->tasksMinimized() ? 1 : 0);
            core::logMessage(core::LogLevel::Info, "%s", buf);
            return buf;
        });
    con->addCommand("notif.click",
                    "notif.click <idx>  模拟点第 idx 条 toast（0 = 最上面那条）",
        [this](const std::vector<std::string>& args) -> std::string {
            const int idx = args.size() > 1 ? std::atoi(args[1].c_str()) : 0;
            if (m_notifications->toastCount() == 0) return "no toasts";
            m_notifications->clickToast(idx);
            return "clicked toast " + std::to_string(idx);
        });
    con->addCommand("notif.task", "notif.task <id> <title...>  起一个进度任务",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 3) return "usage: notif.task <id> <title...>";
            std::string title;
            for (size_t i = 2; i < args.size(); ++i) {
                if (i > 2) title += ' ';
                title += args[i];
            }
            m_notifications->beginTask(args[1], title);
            return "task started: " + args[1];
        });
    con->addCommand("notif.progress",
                    "notif.progress <id> <0..1>  推进任务进度",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 3) return "usage: notif.progress <id> <0..1>";
            const float p = std::strtof(args[2].c_str(), nullptr);
            if (!m_notifications->setTaskProgress(args[1], p))
                return "task '" + args[1] + "' cancelled or gone";
            return "progress " + args[1] + " = " + args[2];
        });
    con->addCommand("notif.cancel", "notif.cancel <id>  请求取消任务",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: notif.cancel <id>";
            m_notifications->requestCancel(args[1]);
            return "cancel requested: " + args[1];
        });
    con->addCommand("notif.done", "notif.done <id>  结束任务",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: notif.done <id>";
            m_notifications->endTask(args[1]);
            return "task ended: " + args[1];
        });
}

// ---------------------------------------------------------------- E18 命令注册表
//
// 把"动作"登记**一次**，命令面板（Ctrl+Shift+P）/ 快捷键表 / 总览窗口共用。
// 下面按来源分五组，全部是**从既有注册表派生**、不是手抄清单：
//   · 渲染特性   ← render::kFeatures[]（含 .enabled 的短名，如 "SSR"）
//   · 面板显隐   ← PanelRegistry 表（id / title / shortcut）
//   · 视图模式   ← render::kViewModeCount
//   · 工作区     ← kWorkspaceCount / workspaceTitle
//   · 控制台命令 ← ConsolePanel::commands()（与控制台同一批）
// 再加一小撮"编辑器固有动作"（保存 / 撤销 / 播放 / 全屏 / 重载着色器）。
void EditorApp::registerEditorCommands() {
    using render::FeatureDesc;
    using render::ParamDesc;

    CommandRegistry& reg = CommandRegistry::get();

    // ---- 1) 渲染特性开关（每个特性一条）----
    for (int i = 0; i < render::kFeatureCount; ++i) {
        const FeatureDesc& f = render::kFeatures[i];
        const std::string enId = std::string(f.id) + ".enabled";
        const ParamDesc* en = render::findParam(enId.c_str());
        if (!en) continue;  // 没登记 .enabled 的特性（如纯参数组）不生成开关

        CommandDesc c;
        c.id = std::string("feature.toggle.") + f.id;
        // 短名优先用 .enabled 参数的 label —— SSR 那条的 label 就是 "SSR"，
        // 于是命令叫 "Toggle SSR"（比 "Toggle Screen Space Reflections" 好用）
        c.label = std::string("Toggle ") + (en->label ? en->label : f.name);
        c.category = "Render";
        c.keywords = std::string(f.id) + " " + f.name + " " +
                     (f.group ? f.group : "") + " " +
                     (f.roadmap ? f.roadmap : "");
        const std::string featureName = f.name;
        c.run = [this, enId, featureName]() {
            const ParamDesc* p = render::findParam(enId.c_str());
            if (!p) return;
            void* ptr = render::paramPtr(p->id);
            if (!ptr) return;
            bool& v = *static_cast<bool*>(ptr);
            v = !v;
            editor::emitParamChange(p->id);  // 与 UI 路径同一套副作用
            m_ctx->notify(featureName + (v ? " ON" : " OFF"));
        };
        reg.add(std::move(c));
    }

    // ---- 2) 面板显隐（从 PanelRegistry 表派生）----
    const std::vector<PanelDesc>& pt = panelTableVec();
    for (int i = 0; i < static_cast<int>(pt.size()); ++i) {
        CommandDesc c;
        c.id = std::string("panel.toggle.") + pt[i].id;
        c.label = std::string("Toggle Panel: ") + pt[i].title;
        c.category = "View";
        c.shortcut = pt[i].shortcut ? pt[i].shortcut : "";
        c.keywords = std::string("panel window ") + pt[i].id;
        c.run = [this, i]() {
            togglePanel(i);
            m_ctx->notify(std::string(panelTitle(i)) +
                          (panelOpen(i) ? " shown" : " hidden"));
        };
        reg.add(std::move(c));
    }

    // ---- 3) 视图模式（8 种，E14 的 kViewModeCount 哨兵）----
    for (int m = 0; m < render::Renderer::kViewModeCount; ++m) {
        CommandDesc c;
        c.id = "viewmode." + std::to_string(m);
        c.label = std::string("View Mode: ") + viewModeLabel(m);
        c.category = "View";
        c.keywords = "view mode display shader debug";
        c.run = [this, m]() { applyViewMode(m); };
        reg.add(std::move(c));
    }

    // ---- 4) 工作区（Ctrl+Alt+数字）----
    for (int i = 0; i < kWorkspaceCount; ++i) {
        CommandDesc c;
        c.id = "workspace." + std::to_string(i);
        c.label = std::string("Workspace: ") + workspaceTitle(i);
        c.category = "View";
        if (i < 9) c.shortcut = "Ctrl+Alt+" + std::to_string(i + 1);
        c.keywords = std::string("workspace layout ") + workspaceId(i);
        c.run = [this, i]() { applyWorkspace(i); };
        reg.add(std::move(c));
    }

    // ---- 5) 控制台命令（与控制台**同一批**）----
    if (m_console) {
        for (const ConsolePanel::Command& cc : m_console->commands()) {
            CommandDesc c;
            c.id = "console." + cc.name;
            c.label = "Console: " + cc.name;
            c.category = "Console";
            c.keywords = cc.help;
            const std::string nm = cc.name;
            c.run = [this, nm]() {
                m_console->execute(nm);
                m_ctx->panels().console = true;  // 输出要看得见
            };
            reg.add(std::move(c));
        }
    }

    // ---- 6) 编辑器固有动作 ----
    {
        CommandDesc c;
        c.id = "help.commandPalette";
        c.label = "Show Command Palette";
        c.category = "Help";
        c.shortcut = "Ctrl+Shift+P";
        c.keywords = "command palette search action";
        c.run = [this]() { m_showCommandPalette = true; };
        reg.add(std::move(c));
    }
    {
        // E21：错过的 WARN/ERROR 都在这里（toast 只活 4 秒）
        CommandDesc c;
        c.id = "help.notificationHistory";
        c.label = "Show Notification History";
        c.category = "Help";
        c.keywords = "notification toast warn error log alert";
        c.run = [this]() { m_showNotificationHistory = true; };
        reg.add(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "edit.undo";
        c.label = "Undo";
        c.category = "Edit";
        c.shortcut = "Ctrl+Z";
        c.when = [this]() { return m_ctx->commands().canUndo(); };
        c.run = [this]() { m_ctx->undo(); };
        reg.add(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "edit.redo";
        c.label = "Redo";
        c.category = "Edit";
        c.shortcut = "Ctrl+Y";
        c.when = [this]() { return m_ctx->commands().canRedo(); };
        c.run = [this]() { m_ctx->redo(); };
        reg.add(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "file.save";
        c.label = "Save Scene";
        c.category = "File";
        c.shortcut = "Ctrl+S";
        c.keywords = "save write disk";
        c.run = [this]() { m_toolbar->saveSceneToCurrentPath(); };
        reg.add(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "file.new";
        c.label = "New Scene";
        c.category = "File";
        c.shortcut = "Ctrl+N";
        c.run = [this]() { m_toolbar->newScene(); };
        reg.add(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "edit.duplicate";
        c.label = "Duplicate Selection";
        c.category = "Edit";
        c.shortcut = "Ctrl+D";
        c.when = [this]() { return m_ctx->hasSelection(); };
        c.run = [this]() { m_hierarchy->duplicateSelection(); };
        reg.add(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "play.toggle";
        c.label = "Play / Pause";
        c.category = "Playback";
        c.shortcut = "Space";
        c.run = [this]() { m_ctx->togglePlayPause(); };
        reg.add(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "view.fullscreen";
        c.label = "Toggle Fullscreen";
        c.category = "View";
        c.shortcut = "F11";
        c.run = [this]() { toggleWindowFullscreen(); };
        reg.add(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "view.resetLayout";
        c.label = "Reset Layout";
        c.category = "View";
        c.run = [this]() {
            m_ctx->relayout() = true;
            resetPanelStateToDefaults();
            m_ctx->notify("Layout reset");
        };
        reg.add(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "build.reloadShaders";
        c.label = "Reload Shaders";
        c.category = "Build";
        c.shortcut = "Ctrl+Shift+R";
        c.run = [this]() { reloadShaders(); };
        reg.add(std::move(c));
    }

    core::logMessage(core::LogLevel::Info, "CMD-REGISTRY count=%d",
                     static_cast<int>(reg.size()));
}

void EditorApp::showCommandPalette() { m_showCommandPalette = true; }

void EditorApp::showNotificationHistory() { m_showNotificationHistory = true; }

// ---------------------------------------------------------------- 设置
//
// Preferences：对标 Blender 首选项的"大窗口"布局 ——
//   左侧一列分类标签（Interface / Viewport / Editing / Keymap / Scene），
//   右侧是该分类的内容页。窗口可缩放、内容区跟随滚动。
// 现有设置全部立即生效（没有"保存偏好"落盘，窗口底部有说明）。
namespace {
// E17：偏好的大小写不敏感子串匹配（搜索框用；表内全是英文文本）
bool prefMatch(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    auto lower = [](const char* src) {
        std::string out(src);
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}
}  // namespace

void EditorApp::drawSettingsPanel() {
    const ImGuiIO& io = ImGui::GetIO();

    // 居中弹出，尺寸接近 Blender 首选项（可拖动缩放）
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f,
                                   io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(920.0f, 620.0f), ImGuiCond_Appearing);

    // ⚠ 窗口 ID 用 ###prefs（ImGui 取 ### 之后做稳定 ID）：以前的小设置
    //   面板也叫 "settings"，imgui.ini 里残留它的 Pos/Size —— 沿用旧 ID
    //   会让新大窗口继承一个陈旧位置（甚至被 clamp 到屏幕角落，实测踩过）。
    //   换新 ID 与旧记录切割，首帧 Appearing 居中才生效。
    if (!ImGui::Begin("Preferences###prefs", &m_ctx->showSettings(),
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    // 自动化用（MYVK_LOG_RECTS=1）：窗口实际矩形 + DisplaySize，
    // 截图脚本核对"居中弹出"是否成立。
    logRect("PREFS-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));
    VK_LOG_INFO("PREFS-DBG DisplaySize=(%.0f,%.0f) ViewportPos=(%.0f,%.0f) "
                "ViewportSize=(%.0f,%.0f)",
                io.DisplaySize.x, io.DisplaySize.y,
                ImGui::GetMainViewport()->Pos.x,
                ImGui::GetMainViewport()->Pos.y,
                ImGui::GetMainViewport()->Size.x,
                ImGui::GetMainViewport()->Size.y);

    // ---- E17 顶栏：跨分类搜索 + 落盘 + Reset All（二次确认）----
    static char search[64] = "";
    ImGui::SetNextItemWidth(-330.0f);
    ImGui::InputTextWithHint("##prefs_search", "Search settings...", search,
                             sizeof(search));
    ImGui::SameLine();
    if (ImGui::Button("Save")) savePrefsToDisk();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("把当前偏好写进 %s（退出时也会自动保存）",
                          editorPrefsPath().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        applyPrefsFromDisk();
        m_ctx->notify("Preferences reloaded from disk");
    }
    ImGui::SameLine();
    if (!m_confirmResetAll) {
        if (ImGui::Button("Reset All")) m_confirmResetAll = true;
    } else {
        // 二次确认：第一次点击只进入"确认态"（按钮变红），再点才真清
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.65f, 0.22f, 0.18f, 1.0f));
        if (ImGui::Button("Confirm reset all?")) {
            for (const PrefItem& it : prefsTable()) {
                void* p = it.ptr(*this);
                if (it.isBool)
                    *static_cast<bool*>(p) = (it.def > 0.5f);
                else
                    *static_cast<float*>(p) = it.def;
            }
            m_confirmResetAll = false;
            savePrefsToDisk();
            m_ctx->notify("All preferences reset to defaults");
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("cancel")) m_confirmResetAll = false;
    }

    // ---- 左侧分类标签 ----
    static int page = 0;
    static const char* kPages[] = {"Interface", "Viewport", "Editing",
                                   "Keymap",    "Scene"};

    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(0.09f, 0.095f, 0.105f, 1.0f));
    ImGui::BeginChild("##prefs_nav", ImVec2(168.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    for (int i = 0; i < 5; ++i) {
        if (ImGui::Selectable(kPages[i], page == i)) {
            page = i;
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##prefs_content", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const std::vector<PrefItem>& table = prefsTable();
    auto drawCategory = [this, &table](const char* cat) {
        ImGui::SeparatorText(cat);
        for (const PrefItem& it : table)
            if (std::strcmp(it.category, cat) == 0) drawPrefRow(it);
    };

    if (search[0]) {
        // ---- E17 搜索模式：跨分类平铺命中项（label / id / 分类名）----
        const char* lastCat = nullptr;
        int hits = 0;
        for (const PrefItem& it : table) {
            const bool hit = prefMatch(it.label, search) ||
                             prefMatch(it.id, search) ||
                             prefMatch(it.category, search);
            if (!hit) continue;
            if (!lastCat || std::strcmp(lastCat, it.category) != 0) {
                ImGui::SeparatorText(it.category);
                lastCat = it.category;
            }
            drawPrefRow(it);
            ++hits;
        }
        if (hits == 0)
            ImGui::TextDisabled("(no settings match '%s')", search);
    } else if (page == 0) {
        drawCategory("Interface");
    } else if (page == 1) {
        drawCategory("Navigation");
        ImGui::TextDisabled("(also editable in the viewport HUD; "
                            "hold RMB + wheel to adjust)");
        drawCategory("Ground grid");
        drawCategory("Light gizmos");
    } else if (page == 2) {
        drawCategory("Snapping");
    } else if (page == 3) {
        // ---- Keymap（只读速查）----
        ImGui::SeparatorText("Viewport navigation (edit mode)");
        ImGui::BulletText("Hold RMB + drag: look around");
        ImGui::BulletText("Hold RMB + WASD: fly   Q / E: down / up");
        ImGui::BulletText("Hold MMB + drag: pan   Shift: fly faster");
        ImGui::BulletText("Wheel: dolly in / out   RMB + wheel: sensitivity");
        ImGui::SeparatorText("Editing");
        ImGui::BulletText("LMB: select / drag gizmo handle");
        ImGui::BulletText("W / E / R: gizmo mode   X: world / local");
        ImGui::BulletText("G: snapping on/off for the current tool");
        ImGui::BulletText("Shift+A (in viewport): add menu (Mesh / Light)");
        ImGui::BulletText("1 / 3 / 7: Front / Right / Top (+Ctrl = opposite)");
        ImGui::BulletText("2 / 4 / 6 / 8: orbit step (Blender numpad)");
        ImGui::BulletText("F: focus selection   Del: delete   F2: rename");
        ImGui::BulletText("Ctrl+Z / Ctrl+Y: undo / redo");
        ImGui::SeparatorText("Scene & playback");
        ImGui::BulletText("Ctrl+S: save   Ctrl+N: new scene");
        ImGui::BulletText("Space: run / pause   F: game fullscreen (in run)");
        ImGui::BulletText("F11: window fullscreen (any mode)");
    } else if (page == 4) {
        // ---- Scene（信息页）----
        ImGui::SeparatorText("Scene");
        ImGui::Text("Path: %s%s", m_ctx->scenePath().c_str(),
                    m_ctx->dirty() ? "  (unsaved changes)" : "");
        if (m_ctx->dirty())
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.4f, 1.0f),
                               "There are unsaved changes - Ctrl+S to save.");
        ImGui::Text("Entities: %zu (editor %zu / runtime %zu)",
                    m_ctx->activeScene().objectCount(),
                    m_ctx->editorScene().objectCount(),
                    m_ctx->runtimeScene().objectCount());
        const scene::Scene::LightStats ls = m_ctx->editorScene().lightStats();
        ImGui::Text("Local lights: %zu (%zu point / %zu spot)",
                    ls.pointLights + ls.spotLights, ls.pointLights,
                    ls.spotLights);
        ImGui::Spacing();
        ImGui::TextWrapped("Save / open with the File menu (Ctrl+S / Ctrl+N). "
                           "The transport row below the viewport holds Run "
                           "(game) and Play / Pause / Stop (animation).");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Preferences apply immediately. Saved to %s.",
                        editorPrefsPath().c_str());

    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------- E17 偏好表（单一事实源）

const std::vector<EditorApp::PrefItem>& EditorApp::prefsTable() {
    // def 值与各存储的**构造默认**一致：
    //   grid.*    → Renderer.h GridSettings 的成员默认值
    //   gizmos.*  → Renderer::m_lightGizmoScale / lightGizmosEnabled 初始值
    //   snap.*    → EditorContext 的构造默认（= 老 "Restore UE5 defaults"）
    //   其余      → EditorContext 的构造默认
    // （E03 落地后默认值应从设置层级拿，届时这里只是一份投影。）
    static const std::vector<PrefItem> kTable = {
        // ---- Interface ----
        {"fontScale", "Font scale", "Interface", false, 1.0f, 0.75f, 2.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->fontScale(); }},
        {"toolbarHeight", "Toolbar height", "Interface", false, 46.0f, 42.0f,
         84.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->toolbarHeight(); }},
        // ---- Navigation ----
        {"navSensitivity", "Sensitivity", "Navigation", false, 1.0f,
         EditorContext::kMinSensitivity, EditorContext::kMaxSensitivity,
         [](EditorApp& a) -> void* { return &a.m_ctx->navSensitivityRef(); }},
        // ---- Ground grid（与 Engine 面板的 grid.* 注册表参数同一存储）----
        {"grid.show", "Show grid", "Ground grid", true, 1.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->showGridRef(); }},
        {"grid.minorStep", "Minor step (m)", "Ground grid", false, 1.0f, 0.1f,
         10.0f,
         [](EditorApp& a) -> void* {
             return &a.renderer().gridSettings().minorStep;
         }},
        {"grid.majorStep", "Major (m)", "Ground grid", false, 10.0f, 1.0f,
         100.0f,
         [](EditorApp& a) -> void* {
             return &a.renderer().gridSettings().majorStep;
         }},
        {"grid.extent", "Extent (m)", "Ground grid", false, 60.0f, 5.0f,
         500.0f,
         [](EditorApp& a) -> void* {
             return &a.renderer().gridSettings().extent;
         }},
        {"grid.alpha", "Alpha", "Ground grid", false, 0.55f, 0.0f, 1.0f,
         [](EditorApp& a) -> void* {
             return &a.renderer().gridSettings().alpha;
         }},
        {"grid.lineWidthPx", "Line width (px)", "Ground grid", false, 1.4f,
         0.5f, 5.0f,
         [](EditorApp& a) -> void* {
             return &a.renderer().gridSettings().lineWidthPx;
         }},
        {"grid.fadeStart", "Edge fade start (m)", "Ground grid", false, 45.0f,
         0.0f, 300.0f,
         [](EditorApp& a) -> void* {
             return &a.renderer().gridSettings().fadeStart;
         }},
        {"grid.fadeEnd", "Edge fade end (m)", "Ground grid", false, 60.0f,
         5.0f, 300.0f,
         [](EditorApp& a) -> void* {
             return &a.renderer().gridSettings().fadeEnd;
         }},
        // ---- Light gizmos ----
        {"gizmos.show", "Draw light gizmos", "Light gizmos", true, 0.0f, 0.0f,
         0.0f,
         [](EditorApp& a) -> void* {
             return &a.renderer().lightGizmosEnabled();
         }},
        {"gizmos.scale", "Gizmo scale", "Light gizmos", false, 0.05f, 0.01f,
         0.25f,
         [](EditorApp& a) -> void* {
             return &a.renderer().lightGizmoScale();
         }},
        // ---- Snapping（UE5 默认：关 + 0.1m / 10deg / 0.1）----
        {"snap.move", "Move snapping", "Snapping", true, 0.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapMoveRef(); }},
        {"snap.moveStep", "Move step (m)", "Snapping", false, 0.1f, 0.01f,
         5.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapMoveStepRef(); }},
        {"snap.rotate", "Rotate snapping", "Snapping", true, 0.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapRotateRef(); }},
        {"snap.rotateStep", "Rotate step (deg)", "Snapping", false, 10.0f,
         1.0f, 90.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapRotateStepRef(); }},
        {"snap.scale", "Scale snapping", "Snapping", true, 0.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapScaleRef(); }},
        {"snap.scaleStep", "Scale step", "Snapping", false, 0.1f, 0.01f, 1.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapScaleStepRef(); }},
        // ---- Viewport overlays（E14）----
        // 四角浮层各自的显隐。右键视口空白处的 "Viewport Display" 菜单是
        // 主入口，这里是同一份存储的第二个视图（顺带解决了落盘 —— E17 的
        // prefs 表就是持久化的唯一来源）。
        {"vp.overlay.modeBar", "View mode bar", "Viewport overlays", true, 1.0f,
         0.0f, 0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->vpOverlayRef().modeBar;
         }},
        {"vp.overlay.stats", "Stats HUD", "Viewport overlays", true, 1.0f, 0.0f,
         0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->vpOverlayRef().stats;
         }},
        {"vp.overlay.camera", "Camera info", "Viewport overlays", true, 1.0f,
         0.0f, 0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->vpOverlayRef().camera;
         }},
        {"vp.overlay.navGizmo", "Navigation gizmo", "Viewport overlays", true,
         1.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->vpOverlayRef().navGizmo;
         }},
        // ---- Stats HUD lines（E14）----
        // 逐行开关：调性能时全开，盯画面时只留 FPS。
        {"vp.stat.fps", "FPS", "Stats HUD lines", true, 1.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().fps;
         }},
        {"vp.stat.frameMs", "Frame time", "Stats HUD lines", true, 1.0f, 0.0f,
         0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().frameMs;
         }},
        {"vp.stat.drawCalls", "Draw calls", "Stats HUD lines", true, 1.0f, 0.0f,
         0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().drawCalls;
         }},
        {"vp.stat.triangles", "Triangles", "Stats HUD lines", true, 1.0f, 0.0f,
         0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().triangles;
         }},
        {"vp.stat.vram", "VRAM", "Stats HUD lines", true, 1.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().vram;
         }},
        {"vp.stat.resolution", "Viewport size", "Stats HUD lines", true, 1.0f,
         0.0f, 0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().resolution;
         }},
        {"vp.stat.viewMode", "View mode", "Stats HUD lines", true, 1.0f, 0.0f,
         0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().viewMode;
         }},
        {"vp.stat.entities", "Entities / lights", "Stats HUD lines", true,
         1.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().entities;
         }},
        {"vp.stat.selection", "Selection", "Stats HUD lines", true, 1.0f, 0.0f,
         0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().selection;
         }},
        {"vp.stat.gizmo", "Gizmo state", "Stats HUD lines", true, 1.0f, 0.0f,
         0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->statsHudRef().gizmo;
         }},
    };
    return kTable;
}

void EditorApp::drawPrefRow(const PrefItem& it) {
    ImGui::PushID(it.id);
    void* p = it.ptr(*this);
    if (it.isBool) {
        ImGui::Checkbox(it.label, static_cast<bool*>(p));
    } else {
        float* f = static_cast<float*>(p);
        ImGui::SetNextItemWidth(220.0f);
        // 用 DragFloat 而不是 SliderFloat：像 extent=500 这种"超出常用
        // 区间"的值还能往回拖，不会被滑条夹死。
        ImGui::DragFloat(it.label, f, (it.hi - it.lo) * 0.005f, it.lo, it.hi,
                         "%.2f");
    }
    // 单项重置：悬停显示默认值是什么
    ImGui::SameLine();
    if (ImGui::SmallButton("R")) {
        if (it.isBool)
            *static_cast<bool*>(p) = (it.def > 0.5f);
        else
            *static_cast<float*>(p) = it.def;
    }
    if (ImGui::IsItemHovered()) {
        if (it.isBool)
            ImGui::SetTooltip("Reset '%s' to default (%s)", it.label,
                              it.def > 0.5f ? "on" : "off");
        else
            ImGui::SetTooltip("Reset '%s' to default (%.2f)", it.label,
                              static_cast<double>(it.def));
    }
    ImGui::PopID();
}

void EditorApp::applyPrefsFromDisk() {
    std::vector<std::pair<std::string, double>> kv;
    if (!loadEditorPrefs(kv)) return;   // 无存档 / 自动化 → 出厂值
    for (const auto& [key, val] : kv) {
        for (const PrefItem& it : prefsTable()) {
            if (key != it.id) continue;
            void* p = it.ptr(*this);
            if (it.isBool) {
                *static_cast<bool*>(p) = (val > 0.5);
            } else {
                float v = static_cast<float>(val);
                // 读回时夹进 UI 范围 —— 手改 JSON 越界不该炸出怪行为
                v = std::clamp(v, it.lo, it.hi);
                *static_cast<float*>(p) = v;
            }
            break;
        }
    }
}

void EditorApp::savePrefsToDisk() {
    std::vector<std::pair<std::string, double>> kv;
    for (const PrefItem& it : prefsTable()) {
        void* p = it.ptr(*this);
        const double v = it.isBool
                             ? (*static_cast<bool*>(p) ? 1.0 : 0.0)
                             : static_cast<double>(*static_cast<float*>(p));
        kv.emplace_back(it.id, v);
    }
    saveEditorPrefs(kv);
}

// ---------------------------------------------------------------- 角色详情窗口

void EditorApp::drawCharacterWindow() {
    // 处理"创建角色后打开详情窗口"的请求
    const ecs::Entity req = m_ctx->takeCharacterWindowRequest();
    if (req.valid()) {
        m_characterWindow->open(req);
    }
    // 每帧绘制（窗口内部自己判断是否打开）
    m_characterWindow->draw();
}

} // namespace editor
