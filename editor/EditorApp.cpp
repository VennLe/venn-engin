#include "EditorApp.h"

#include "Localization.h"
#include "CharacterWindow.h"
#include "DialogueEditorWindow.h"
#include "TimelineEditorWindow.h"
#include "UiEditorWindow.h"
#include "CommandRegistry.h"
#include "ContentBrowser.h"
#include "DebugRects.h"
#include "EditorContext.h"
#include "EditorIcons.h"
#include "ui/ImGuiTheme.h"
#include "EditorLayoutIO.h"
#include "ConsolePanel.h"
#include "FeatureUIGen.h"
// E21：通知 / 进度（构造即接管 Logger 的 hook）。
#include "NotificationCenter.h"
#include "GpuTimelinePanel.h"
// E09：生成区直接遍历 kFeatures / paramPtr（FeatureUIGen.h 只前向声明）
#include "render/FeatureRegistry.h"
#include "render/RenderSettingsIO.h"
#include "EditorScene.h"
#include "InputMapIO.h"
#include "InspectorPanel.h"
#include "LandscapeEditor.h"
#include "FoliageEditor.h"
#include "ToolkitEditors.h"
#include "WaterEditor.h"
#include "KeyBindingsPage.h"
#include "PanelRegistry.h"
#include "PanelDock.h"
#include "ModeRail.h"
#include "ViewportTabs.h"
#include "ParticlesPanel.h"
#include "PhysicsPanel.h"
#include "QualitySettingsPanel.h"
#include "MaterialLibraryPanel.h"
#include "SceneHierarchy.h"
#include "SplitLayout.h"
#include "Toolbar.h"
#include "ViewportPanel.h"
#include "Workspace.h"
#include "script/ScriptEngine.h"
// 四态 locomotion：与运行时共用同一份实现（P4）
#include "animation/Locomotion.h"
// 过场时间轴（C1）：编辑器侧走"直接写 scene::Camera"这条路
#include "sequence/SequencerSystem.h"
// 键码换算（编辑器侧判定走 ImGui 原始键状态，见 ui/KeyCodes.h）
#include "ui/KeyCodes.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "core/Input.h"       // Play 态的原始鼠标位移 / 右键（P8）
#include "core/Version.h"
#include "core/WindowGeometry.h"
#include "ecs/Components.h"
#include "physics/CollisionWorld.h"
#include "physics/CharacterController.h"   // physics::kGravity（编辑器跳跃同源）
#include "physics/PhysicsWorld.h"
// 场景 ⇄ 物理的绑定层（G01 从本文件提到引擎层，与 Game.exe 共用）
#include "physics/ScenePhysics.h"
#include "render/Renderer.h"
#include "scene/Camera.h"
#include "scene/LookLimits.h"
#include "scene/Prefab.h"
#include "scene/SpringArm.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "project/ProjectCreate.h"
#include "project/ProjectFile.h"
#include "ui/ImGuiManager.h"
#include "ui/ImGuiTheme.h"
// U2：控制台 `ui.rect` 写的就是 ui2::debugRect()（Game UI 通道的调试图元）
// U5：`ui.button` / `ui.click` 同上（按钮与命中测试的调试对象）
#include "ui2/UiCanvas.h"
#include "ui2/UiWidget.h"

#include <imgui.h>
// ⚠ 只有一处用到内部 API：抽屉每帧 BringWindowToDisplayFront（见
//   drawContentDrawer 末尾）。公开 API 没有"只改绘制顺序、不动焦点"
//   的入口，而 SetNextWindowFocus 会抢键盘焦点。
#include <imgui_internal.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace editor {

namespace {

// 早期声明：drawPanels（本文件前部）要给浮动面板抢层级，需要按 id 拿到
// 它的 ImGui 窗口名；真正的定义在文件后半段（托盘那一节）。
std::string trayWindowName(const char* id);

// 布局常量（像素）。主体四个面板（Hierarchy/Content/Viewport/Inspector）
// 已交给 SplitLayout 分栏树按比例自适应；这里只保留横贯全宽的固定条
// 与浮动调试面板的尺寸常量。
constexpr float kStatusH = 26.0f;
// 视口正下方播放条（Run / Play / Pause / Stop）的高度。
// "刚刚好一行"：一行按钮（28px）+ 上下各 3px 内边距。
//
// ⚠ 2026-10-17：这一条**不再绘制**（用户要求"动画部分的操作 UI 先全部
//   移除"）。常量保留给 LayoutRects::transport 的注释与历史脚本对照，
//   但 computeLayout 不再从视口里扣它。
constexpr float kTransportH = 34.0f;
// 底部通栏（Content / 输出日志 / CMD 三个带图标的按钮）的高度。
// 比状态栏高一点：里面有 22px 的图标，上下各留 4px 呼吸。
constexpr float kBottomBarH = 32.0f;
// Content 抽屉的高度范围与默认值（未停靠时是浮层，停靠时占布局）。
constexpr float kContentDrawerMin = 120.0f;
constexpr float kContentDrawerDefault = 320.0f;
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

// 引擎版本号（Help ▸ About 显示）。⚠ 实体在 `engine/core/Version.h` ——
// 启动器也要显示版本，两个应用层不许各写一份（改版本必漏一处）。
constexpr const char* kVennVersion = ::core::kVennVersion;

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

// ---------------------------------------------------------------- 偏好页
// **顺序即下标**（`page == N` 的分支都写数字，改顺序等于改语义）。
// 名字是逻辑名：显示时过 TR，落盘/自动化按名字查 —— 换语言不会选中别的页。
constexpr const char* kPrefsPages[] = {"Interface", "Appearance", "Viewport",
                                       "Editing",   "Keymap",     "Scene",
                                       "Bindings"};
constexpr int kPrefsPageCount =
    static_cast<int>(sizeof(kPrefsPages) / sizeof(kPrefsPages[0]));
// G06：游戏动作键位重绑定（见 KeyBindingsPage.h 的说明）
constexpr int kPrefsPageBindings = 6;

// MYVK_EDITOR_PREFS_PAGE=<页名|下标>：打开 Preferences 时直接落到这一页。
// 没有它，"改键位"这条链只能靠人手点两下才能进入被测状态。
int prefsPageFromEnv() {
    const char* p = std::getenv("MYVK_EDITOR_PREFS_PAGE");
    if (!p || p[0] == '\0') return 0;
    for (int i = 0; i < kPrefsPageCount; ++i)
        if (std::strcmp(p, kPrefsPages[i]) == 0) return i;
    char* end = nullptr;
    const long v = std::strtol(p, &end, 10);
    if (end && *end == '\0' && v >= 0 && v < kPrefsPageCount)
        return static_cast<int>(v);
    VK_LOG_WARN("PREFS unknown MYVK_EDITOR_PREFS_PAGE='%s' (valid: %s..%s or "
                "an index) — falling back to Interface",
                p, kPrefsPages[0], kPrefsPages[kPrefsPageCount - 1]);
    return 0;
}

// ============================================================
// G06 · 键位注入脚本（MYVK_EDITOR_REBIND）
// ============================================================
// 语法：分号分隔的一串命令，按顺序执行 ——
//
//   <context>/<action>=<glfw键码>   绑一条键盘绑定（走 UI 的同一条路径）
//   <context>/<action>=clear        只解掉键盘绑定（手柄保留）
//   <context>/<action>=reset        这个动作恢复出厂（键盘 + 手柄）
//   resetall                        整表恢复出厂
//   undo                            走**同一条**撤销栈（验证可撤销性）
//   save                            写盘到 editorInputMapPath()
//
// 例：`Gameplay/Jump=70;save`（70 = GLFW_KEY_F）
//
// ⚠ 为什么不直接点 UI：捕获必须"按下真实按键"，自动化里没有可靠的合成
//   按键（PostMessage 到 GLFW 那条路只在窗口有焦点时成立）。所以给
//   **唯一的写路径**（EditorContext::rebindAction / saveInputMap）开一个
//   脚本口子 —— UI 与脚本走同一段代码，脚本能测的就等于 UI 能做的。
//
// ⚠ 键码用数字而不是 "F"：GLFW 的键码本来就是数字，再造一套名字表等于
//   多一个会漂移的映射（"F" 到底是不是 GLFW_KEY_F）。
void applyRebindScript(EditorContext& ctx, const char* script) {
    std::string s(script);
    int applied = 0;
    int failed = 0;

    std::size_t begin = 0;
    while (begin <= s.size()) {
        const std::size_t semi = s.find(';', begin);
        std::string tok = (semi == std::string::npos)
                              ? s.substr(begin)
                              : s.substr(begin, semi - begin);
        begin = (semi == std::string::npos) ? s.size() + 1 : semi + 1;

        // 去掉两端空白（允许写成 "a; b" 这种好读的形式）
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.pop_back();
        if (tok.empty()) continue;

        if (tok == "save") {
            ++applied;
            if (!ctx.saveInputMap()) ++failed;
            continue;
        }
        if (tok == "undo") {
            ++applied;
            ctx.undo();
            continue;
        }
        if (tok == "resetall") {
            ++applied;
            if (!ctx.resetAllBindings()) ++failed;
            continue;
        }
        if (tok == "dump") {
            // 什么都不改，只为了让末尾那次**全表输出**跑起来（只读断言用）。
            ++applied;
            continue;
        }

        const std::size_t eq = tok.find('=');
        const std::size_t slash = tok.find('/');
        if (eq == std::string::npos || slash == std::string::npos ||
            slash > eq) {
            VK_LOG_WARN("INS-BIND-SCRIPT bad command '%s'", tok.c_str());
            ++failed;
            continue;
        }
        const std::string c = tok.substr(0, slash);
        const std::string a = tok.substr(slash + 1, eq - slash - 1);
        const std::string v = tok.substr(eq + 1);

        bool ok = false;
        if (v == "clear")
            ok = ctx.clearActionBinding(c, a);
        else if (v == "reset")
            ok = ctx.resetActionBinding(c, a);
        else {
            const int code = std::atoi(v.c_str());
            if (code <= 0) {
                VK_LOG_WARN("INS-BIND-SCRIPT bad key code '%s'", v.c_str());
                ++failed;
                continue;
            }
            core::InputBinding b;
            b.source = core::InputSource::Key;
            b.code = code;
            ok = ctx.rebindAction(c, a, b);
        }
        VK_LOG_INFO("INS-BIND-INJECT %s ok=%d", tok.c_str(), ok ? 1 : 0);
        if (ok) ++applied;
        else ++failed;
    }

    // 脚本跑完把**生效后的**整张表逐条打出来。
    //
    // ⚠ 这是自动化唯一稳定的"改完到底长什么样"口径：手写 JSON 断言只能看到
    //   落盘格式，而这里看到的是**内存里真正生效的**绑定（`PadBtn0 / F` 这种
    //   多来源描述也在）。只在注入时打，不污染正常运行的日志。
    for (const std::string& cn : ctx.inputMap().contextNames())
        for (const std::string& an : ctx.inputMap().actionNames(cn))
            VK_LOG_INFO("INS-BIND-AFTER %s/%s=%s", cn.c_str(), an.c_str(),
                        ctx.inputMap().describeBinding(cn, an).c_str());

    VK_LOG_INFO("INS-BIND-SCRIPT applied=%d failed=%d", applied, failed);
}

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
        // ⚠ content 的 defaultOpen = false：2026-10-17 起它是"底栏抽屉"，
        //   默认关着（用户要求）。仍然留在表里 → View/Window 菜单、Alt+3、
        //   布局持久化照旧可用；只是它不再占分栏（见 SplitLayout 的注释）。
        {"content", "Content Browser", PanelGroup::General, "Alt+3", "folder",
         false, false, kSlotContent, &EditorApp::panelDrawContent},
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
        // ---- 2026-10-17 晚：底栏"+"托盘里勾出来的功能面板 ----
        // 这三张都画在自己的文件里（CharacterWindow / PhysicsPanel /
        // ParticlesPanel），表项只负责"显隐 + 快捷键 + 落盘"，与其余浮动
        // 面板完全一致 —— 于是托盘、View 菜单、快捷键、布局持久化四路
        // 自动跟上，不需要在托盘里另开一套注册表。
        {"character", "Character", PanelGroup::Authoring, "Alt+0", "user",
         false, true, kSlotNone, &EditorApp::panelDrawCharacter},
        {"physics", "Physics", PanelGroup::Authoring, "", "atom",
         false, true, kSlotNone, &EditorApp::panelDrawPhysics},
        {"particles", "Particles", PanelGroup::Authoring, "", "spark",
         false, true, kSlotNone, &EditorApp::panelDrawParticles},
        // E26：画质档位。归 Authoring（"调参类"），无快捷键 —— 它是一张
        // 偶尔打开的面板（跟 Physics / Particles 同款），不该占 Alt+N。
        {"quality", "Quality Settings", PanelGroup::Authoring, "", "sliders",
         false, true, kSlotNone, &EditorApp::panelDrawQuality},
        // ---- 2026-10-20：编辑模式自带的两个编辑器 ----
        // 它们的显隐**跟顶栏的编辑模式走**（见 serviceEditMode），但仍然是
        // 普通面板表项 —— View 菜单、快捷键、布局持久化、底栏托盘自动跟上。
        {"landscape", "Landscape", PanelGroup::Authoring, "", "mountain",
         false, true, kSlotModes, &EditorApp::panelDrawLandscape},
        {"foliage", "Foliage", PanelGroup::Authoring, "", "leaf",
         false, true, kSlotModes, &EditorApp::panelDrawFoliage},
        // 2026-10-20 晚：下拉里其余 6 个模式的面板。同一套约定 ——
        // 显隐跟 EditMode 走，但仍是普通表项（View 菜单 / 托盘 / 落盘都跟得上）。
        {"meshpaint", "Mesh Paint", PanelGroup::Authoring, "", "palette",
         false, true, kSlotModes, &EditorApp::panelDrawMeshPaint},
        {"modeling", "Modeling", PanelGroup::Authoring, "", "cube",
         false, true, kSlotModes, &EditorApp::panelDrawModeling},
        {"fracture", "Fracture", PanelGroup::Authoring, "", "bolt",
         false, true, kSlotModes, &EditorApp::panelDrawFracture},
        {"brushedit", "Brush Editing", PanelGroup::Authoring, "", "brush",
         false, true, kSlotModes, &EditorApp::panelDrawBrushEdit},
        {"animation", "Animation", PanelGroup::Authoring, "", "play",
         false, true, kSlotModes, &EditorApp::panelDrawAnimation},
        {"pcg", "PCG", PanelGroup::Authoring, "", "nodes",
         false, true, kSlotModes, &EditorApp::panelDrawPCG},
        // 2026-10-21：水体编辑模式。显隐跟 EditMode 走，与上面八个同构。
        {"water", "Water", PanelGroup::Authoring, "", "water",
         false, true, kSlotModes, &EditorApp::panelDrawWater},
        // 2026-10-24：材质仓库（贴图版材质货架）。
        // 归 Authoring 组、浮动窗口、无快捷键 —— 和 Physics / Particles /
        // Quality 同款（"偶尔打开的一张面板"，不该占 Alt+N）。
        // 它的两个动作分别服务地形笔刷（Landscape 模式）与属性栏换材质。
        {"matlib", "Material library", PanelGroup::Authoring, "", "palette",
         false, true, kSlotNone, &EditorApp::panelDrawMaterialLibrary},
        // 2026-10-27（#310）：剧本编辑器。与 Character 同款 —— 窗口对象自己
        // 管 open/close（它还要带一个资产路径），表项只做"显隐 + 快捷键 +
        // 落盘"三路，真正的 draw 在 drawDialogueEditorWindow 里每帧无条件跑。
        {"dialogueed", "Script editor", PanelGroup::Authoring, "", "code",
         false, true, kSlotNone, &EditorApp::panelDrawDialogueEditor},
        // 2026-10-27（#311）：时间轴轨道编辑器。与上面那一格**完全同构** ——
        // 同样是"窗口对象自己管 open/close + 带一个资产路径"，表项只做
        // 显隐 / 快捷键 / 落盘三路，真正的 draw 在 drawTimelineEditorWindow。
        {"timelineed", "Timeline editor", PanelGroup::Authoring, "", "timeline",
         false, true, kSlotNone, &EditorApp::panelDrawTimelineEditor},
        // 2026-10-27（#312）：UI 编辑器（`.ui.json` 控件树 + 锚点 + 绑定）。
        // 与上两格**完全同构**：窗口对象自己管 open/close，表项只做
        // 显隐 / 落盘，真正的 draw 在 drawUiEditorWindow。
        {"uied", "UI editor", PanelGroup::Authoring, "", "layout",
         false, true, kSlotNone, &EditorApp::panelDrawUiEditor},
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
// Character 是唯一"窗口对象活得比面板状态久"的一张：CharacterWindow 自己
// 有 open/close，还要绑定一个玩家实体。所以这里只做**双向同步** ——
//   · 面板勾上（open=true）而窗口没开 → 用当前选中项（没有就空实体）打开
//   · 关窗口的 × 被点 → 把面板勾选也去掉
//
// ⚠ 同步**不能**放在这个 draw 钩子里：`drawPanels` 只在 `panelOpen()`
//   为真时才派发，于是"从菜单/命令面板把勾去掉"这条路走不到这里，窗口
//   会留在屏幕上（表里以为关了、屏幕上还开着）。真正的同步在
//   `drawCharacterWindow()` 里 —— 它每帧无条件被调用。这里保持空壳只是
//   为了让面板表有一项 draw 入口（表项没有 draw 且 slot==kSlotNone 时会
//   画一个占位窗口）。
//
// ⚠ 2026-10-17 第四轮修的 bug（用户原话："点击 character 面板右上角的叉号
//   关闭按钮无法关闭 character 面板"）：上一版写的是 `panelOpen && !isOpen
//   → open()`。点 × 时 CharacterWindow 把自己的 `m_open` 关掉，但注册表那
//   一格**还是勾着的** → 下一个同步周期看到"勾着却没开"，于是又给它打开，
//   表现就是叉号按了没反应。现在用 m_characterWasOpen 记住"上一帧它开过"，
//   由此区分两种"勾着但没开"：**用户刚关的**（上帧开过）与**刚被勾上的**
//   （上帧没开）。前者把注册表也关掉，后者才打开。
void EditorApp::panelDrawCharacter(EditorApp& a) { (void)a; }
// 同款空壳：drawDialogueEditorWindow 每帧无条件被调用（见 onImGui），
// 这里留一个入口只是为了让面板表有一项 draw（否则表项没有 draw 且
// slot == kSlotNone 时 drawPanels 会画一个"待建设"占位窗口）。
void EditorApp::panelDrawDialogueEditor(EditorApp& a) { (void)a; }
// 同上：drawTimelineEditorWindow 每帧无条件被调用，这里只给面板表一个入口。
void EditorApp::panelDrawTimelineEditor(EditorApp& a) { (void)a; }
// 同上（#312）：drawUiEditorWindow 每帧无条件被调用。
void EditorApp::panelDrawUiEditor(EditorApp& a) { (void)a; }
void EditorApp::panelDrawPhysics(EditorApp& a) {
    if (a.m_physicsPanel) a.m_physicsPanel->draw();
}
void EditorApp::panelDrawMaterialLibrary(EditorApp& a) {
    if (a.m_materialLib) a.m_materialLib->draw();
}
void EditorApp::panelDrawParticles(EditorApp& a) {
    if (a.m_particlesPanel) a.m_particlesPanel->draw();
}
// E26：画质档位。面板自己不持任何引擎引用（参数在 FeatureRegistry 里），
// 于是它可以在没有设备的情况下被构造 —— 与 Physics/Particles 的差别仅此。
void EditorApp::panelDrawQuality(EditorApp& a) {
    if (a.m_qualityPanel) a.m_qualityPanel->draw();
}

// 2026-10-20：两个编辑模式各自的面板。它们自己判 panelOpenRef，所以这里
// 只要指针非空就转发 —— 与 Physics / Particles 同一条写法。
void EditorApp::panelDrawLandscape(EditorApp& a) {
    if (a.m_landscape) a.m_landscape->drawPanel();
}
void EditorApp::panelDrawFoliage(EditorApp& a) {
    if (a.m_foliage) a.m_foliage->drawPanel();
}
void EditorApp::panelDrawMeshPaint(EditorApp& a) {
    if (a.m_meshPaint) a.m_meshPaint->drawPanel();
}
void EditorApp::panelDrawModeling(EditorApp& a) {
    if (a.m_modeling) a.m_modeling->drawPanel();
}
void EditorApp::panelDrawFracture(EditorApp& a) {
    if (a.m_fracture) a.m_fracture->drawPanel();
}
void EditorApp::panelDrawBrushEdit(EditorApp& a) {
    if (a.m_brushEdit) a.m_brushEdit->drawPanel();
}
void EditorApp::panelDrawAnimation(EditorApp& a) {
    if (a.m_animation) a.m_animation->drawPanel();
}
void EditorApp::panelDrawWater(EditorApp& a) {
    if (a.m_water) a.m_water->drawPanel();
}
void EditorApp::panelDrawPCG(EditorApp& a) {
    if (a.m_pcg) a.m_pcg->drawPanel();
}

void EditorApp::drawPlaceholderPanel(int panelIndex) {
    const char* title = panelTitle(panelIndex);
    const char* id = panelId(panelIndex);

    char win[160];
    std::snprintf(win, sizeof(win), "%s###panel_%s", TR(title), id);

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
    ImGui::TextDisabled(TR("%s"), TR(title));
    ImGui::Separator();
    ImGui::TextWrapped(
        TR("This panel is registered in the panel table (id = \"%s\"):\n"
           "the View/Window menus, its shortcut and its visibility "
           "persistence are all live.\n"
           "Its content is filled in by later tasks "
           "(see docs/editor_roadmap.md)."),
        id);
    ImGui::End();
    if (!open) setPanelOpen(panelIndex, false);
}

// ============================================================
// 分隔条遮挡：把"浮在分栏之上的一切"交给分栏器
// ============================================================
//
// 为什么需要这个函数（2026-10-18 用户报的 bug）：
//   分隔条画在 `GetForegroundDrawList()` 上 —— 那是**所有窗口之上**的最后
//   一层，目的是"不被面板盖住"。代价是它同样会**穿过弹层**：点 `+` 打开的
//   勾选菜单中间横着一条分栏线、右键菜单里也是、角色窗口里也是。用户的原话
//   是"也包括其他所有的面板窗口或子菜单都有这个问题"。
//
// 做法：每帧在**所有窗口都画完**之后，把这一帧真正活着的顶层窗口矩形收集
// 起来，分栏器逐段裁掉落在它们里面的部分（画和命中都用裁后的可见段）。
//
// 为什么要**排除布局面板**：分隔线正好压在两个面板的公共边界上
// （A 的 x1 == B 的 x0），拿面板自己的矩形去裁，整条线会被裁成 0 长 ——
// 那就等于把分隔条全弄没了。布局面板的位置就是 `LayoutRects` 里那些矩形，
// 所以按"窗口矩形 == 某个布局矩形"来排除，几何自洽、也不用改各面板的
// 窗口命名（改名 / 翻译都不会影响这里）。
void EditorApp::collectSplitOccluders() {
    m_split->clearOccluders();

    const LayoutRects& L = m_ctx->layout();
    // m_tabBarRect 必须在这里：标签条是**独立窗口**，不在 LayoutRects 里，
    // 不列进"布局面板"名单就会被当成浮层 → 左边的竖直分隔条在标签条高度
    // 上被裁掉一截（7px 的断口，肉眼很明显）。
    const Rect layoutRects[] = {L.hierarchy, L.content, L.viewport, L.inspector,
                                L.modes,     m_tabBarRect, L.bottom,
                                L.status,    L.toolbar,  L.transport};

    auto isLayoutPanel = [&layoutRects](const ImGuiWindow* w) {
        constexpr float kEps = 2.0f;   // 面板位置有取整，留 2px 余量
        for (const Rect& r : layoutRects) {
            if (r.w <= 0.0f || r.h <= 0.0f) continue;
            if (std::abs(w->Pos.x - r.x) <= kEps &&
                std::abs(w->Pos.y - r.y) <= kEps &&
                std::abs(w->Size.x - r.w) <= kEps &&
                std::abs(w->Size.y - r.h) <= kEps) {
                return true;
            }
        }
        return false;
    };

    // ---- 1) 浮层形态的 Content 抽屉（不在分栏树里）----
    if (L.content.w > 0.0f && !m_contentDock) {
        m_split->addOccluder(L.content.x, L.content.y,
                             L.content.x + L.content.w,
                             L.content.y + L.content.h);
    }

    // ---- 2) 其余所有活着的顶层窗口：弹层 / 菜单 / 浮动面板 / 角色窗口 /
    //         tooltip / 通知 ----
    // ⚠ 用 `WasActive`（本帧或上一帧用过）而不是 `Active`：菜单弹出那一帧
    //   窗口刚被创建，用它自己上报的 Active 会漏掉一帧 —— 一帧的线穿过菜单
    //   顶部，肉眼就是闪一下。
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiWindow* w : g.Windows) {
        if (!w) continue;
        if (!w->WasActive || w->Hidden) continue;
        if (w->Flags & ImGuiWindowFlags_ChildWindow) continue;   // 面板内部子窗口
        if (w->Size.x < 2.0f || w->Size.y < 2.0f) continue;      // 折叠/占位
        if (isLayoutPanel(w)) continue;                          // 见函数头注释
        m_split->addOccluder(w->Pos.x, w->Pos.y, w->Pos.x + w->Size.x,
                             w->Pos.y + w->Size.y);
    }
}

// ============================================================
// 编辑模式（2026-10-20）
// ============================================================
//
// 顶栏那颗下拉只负责改 EditorContext 里的状态；"开哪个面板 / 关哪个面板 /
// 写未落盘的改动"全在这里做 —— 因为只有 EditorApp 同时拿得到面板表、
// 焦点请求和两个编辑器的裸指针。
//
// 为什么用**差量**而不是在 Toolbar 的点击回调里直接调：
//   · 模式还可能从别处改（环境变量钩子、将来 Foliage 面板里的
//     "去建地形"按钮、脚本）；
//   · 回调里做副作用会让"切模式"变成一次打包动作，很难测。
// 编辑模式 → 它自己的面板 id（`PanelDesc::id`）。Select 没有面板：它就是
// "点一下选中、W/E/R 变换"，面板是 Inspector。所以这里返回 nullptr。
//
// 抽成表而不是在 serviceEditMode 里写九个 if：九个模式里只有 Select 特例，
// 剩下八个完全同构（关旧面板 / 开新面板 / sync / focus），一份表就够。
// Toolbar 里那颗下拉也是按 EditMode 顺序枚举的 —— 加模式时这两处必须一起改。
static const char* editModePanelId(EditMode m) {
    switch (m) {
        case EditMode::Landscape: return "landscape";
        case EditMode::Foliage:   return "foliage";
        case EditMode::MeshPaint: return "meshpaint";
        case EditMode::Modeling:  return "modeling";
        case EditMode::Fracture:  return "fracture";
        case EditMode::BrushEdit: return "brushedit";
        case EditMode::Animation: return "animation";
        case EditMode::PCG:       return "pcg";
        case EditMode::Water:     return "water";
        case EditMode::Select:    return nullptr;
    }
    return nullptr;
}

void EditorApp::serviceEditMode() {
    // ---- 自动化：MYVK_LANDSCAPE_AUTOCREATE=1 ----
    // 前 30 帧什么都别做：场景还要加载、面板要第一次落位。等到场景稳定
    // 再建地形，否则 createTerrain 会挂在"场景已被替换"的旧句柄上。
    if (m_autoCreateTerrain && !m_autoCreateDone && m_editorFrames >= 30) {
        m_autoCreateDone = true;
        if (m_landscape && !m_landscape->hasTerrain()) {
            m_ctx->setEditMode(EditMode::Landscape);
            m_landscape->createWithCurrentSettings();
            VK_LOG_INFO("LS-AUTOCREATE done");
        }
    }

    const EditMode cur = m_ctx->editMode();
    const EditMode prev = m_lastEditMode;

    // ---- 每帧都要跑的：动画时间轴 ----
    // 必须在下面的 `cur == prev` 早退**之前**：切模式只发生一次，而播放要
    // 逐帧推进。`AnimationEditor::sync` 自己会在没播放时立刻返回。
    //
    // 只在 Animation 模式下推进（而不是无条件）：用户切走之后时间轴就该停，
    // 否则"在建模面板里改着网格，选中物体还在偷偷移动"。
    if (cur == EditMode::Animation && m_animation) m_animation->sync();

    if (cur == prev) return;
    m_lastEditMode = cur;

    // 离开地形模式：把最后一笔没赶上的落盘写完
    if (prev == EditMode::Landscape && m_landscape) m_landscape->flush();

    // 面板跟模式一一对应：先关掉上一个模式的编辑器（不叠加）
    const char* prevId = editModePanelId(prev);
    if (prevId) {
        const int prevIdx = panelIndexOf(prevId);
        if (prevIdx >= 0) setPanelOpen(prevIdx, false);
    }

    // ---- 打开新模式的编辑器 + 把相机对到能看见东西的地方 ----
    const char* curId = editModePanelId(cur);
    const int curIdx = curId ? panelIndexOf(curId) : -1;
    if (curIdx >= 0) setPanelOpen(curIdx, true);

    switch (cur) {
        case EditMode::Landscape:
            if (m_landscape) {
                m_landscape->sync();
                // 正常高度对着角色（2026-10-24）—— 以前是 focusCamera()，
                // 那会把镜头甩到 500 m 外看地形全貌，切一次模式就被"弹飞"
                // 一次。全貌仍然留在面板的 Focus 按钮上。
                if (m_landscape->hasTerrain()) m_landscape->focusCameraAtPlayer();
            }
            break;
        case EditMode::Foliage:
            if (m_foliage) {
                m_foliage->sync();
                if (m_landscape && m_landscape->hasTerrain()) {
                    m_landscape->focusCameraClose();
                }
            }
            break;
        // 下面六个的 sync 都是"把选中项重新绑一遍"：切过来的时候选中项可能
        // 已经换了（在 Select 模式下点的另一个物体）。
        case EditMode::MeshPaint:
            if (m_meshPaint) m_meshPaint->sync();
            break;
        case EditMode::Modeling:
            if (m_modeling) m_modeling->sync();
            break;
        case EditMode::Fracture:
            if (m_fracture) m_fracture->sync();
            break;
        case EditMode::BrushEdit:
            if (m_brushEdit) m_brushEdit->sync();
            break;
        case EditMode::Animation:
            if (m_animation) m_animation->sync();
            break;
        case EditMode::PCG:
            if (m_pcg) m_pcg->sync();
            break;
        case EditMode::Water:
            if (m_water) {
                m_water->sync();
                m_ctx->setStatus(TR("Water mode: drag on the ground to create "
                                    "a water body"));
            }
            break;
        case EditMode::Select:
            m_ctx->setStatus(TR("Select mode: click to pick, W/E/R to transform"));
            break;
    }

    if (curId) focusPanel(curId);

    // 日志：老字段（landscape/foliage）保留 —— 已有脚本按它断言；新字段
    // `panel`/`open` 覆盖全部八个有面板的模式。
    const int lsIdx = panelIndexOf("landscape");
    const int foIdx = panelIndexOf("foliage");
    VK_LOG_INFO("EDITMODE-PANELS mode=%s landscape=%d foliage=%d panel=%s open=%d",
                editModeId(cur),
                (lsIdx >= 0 && panelOpen(lsIdx)) ? 1 : 0,
                (foIdx >= 0 && panelOpen(foIdx)) ? 1 : 0,
                curId ? curId : "-",
                (curIdx >= 0 && panelOpen(curIdx)) ? 1 : 0);
}

void EditorApp::drawPanels() {
    const std::vector<PanelDesc>& t = panelTableVec();
    // 2026-10-21：视口上方多了一条标签条，浮动面板全部变成它的标签页。
    // `hosted` = 本帧真正要画进内容区的那个面板（-1 = 画 3D 视口）。
    // ---- Play 期间内容区**永远**是 3D 视口（P8，2026-10-23）----
    //
    // 旧行为：`hosted` 完全由标签条决定。用户把内容区切到 Scripts / Physics
    // 某一页之后按 Run，`hosted >= 0` 会让**视口整块不画** —— 表现是
    // "游戏跑起来了，但看不见画面，也点不到视口"。视图口是 Play 的唯一
    // 出口，所以 Play 态必须把它钉回来（标签条还照常显示，切过去也会立刻
    // 被顶回视口，这正是"运行期间不该切页"的正确表现）。
    const int hosted =
        m_ctx->isEditing() ? viewportTabDrawnPanel() : -1;
    for (int i = 0; i < static_cast<int>(t.size()); ++i) {
        if (!panelOpen(i)) continue;
        // 底栏按钮把窗口收起来了：不提交 = ImGui 认为它关着。勾选状态留着，
        // 所以按钮还在，再点一下就回来。
        if (panelTrayHiddenRef(i)) continue;
        const PanelDesc& p = t[i];
        // Content 是"底栏抽屉"，不走表驱动绘制（它要盖在所有面板之上，
        // 而这里的顺序是表顺序 → 会被后画的视口盖住）。见 drawContentDrawer。
        if (p.layoutSlot == kSlotContent) continue;
        // ---- 标签页宿主（layoutSlot == kSlotNone）----
        // 只有**当前页**画进内容区，其余的根本不提交：它们仍是"开着"的，
        // 只是这一帧不占屏幕 —— 这正是浏览器标签页的语义。
        // 模式面板挂 kSlotModes，走不到这里（它们在左栏，见 ModeRail.h）。
        const bool tabHosted = (p.layoutSlot == kSlotNone);
        if (tabHosted && i != hosted) continue;
        // 3D 视口：当前不是"视口"这一页时整块不画（连相机导航都不跑）。
        // 不画而不是"画在零矩形里"：ViewportPanel 会在 draw 里读鼠标做
        // 轨道/飞行，标签页盖着它的时候那些输入全都不该生效。
        if (p.layoutSlot == kSlotViewport && hosted >= 0) continue;
        // 分栏树里的面板：被"最大化"顶掉的那几个不画
        if (p.layoutSlot != kSlotNone && !m_split->showsInLayout(p.layoutSlot)) {
            continue;
        }
        const bool wasOpen = panelOpen(i);
        if (tabHosted) {
            // 钉到内容区 → 面板自己的 FirstUseEver 落位会被 applyDockHint
            // 里的 Always 顶掉（见 PanelDock.h 的说明）
            const Rect& vp = m_ctx->layout().viewport;
            pushDockHint(vp.x, vp.y, vp.w, vp.h);
        }
        if (p.draw) {
            p.draw(*this);
        } else if (p.layoutSlot == kSlotNone) {
            drawPlaceholderPanel(i);
        }
        if (tabHosted) clearDockHint();

        // ---- 标题栏的 × = "收进底栏"，不是"取消勾选" ----
        //
        // 2026-10-18（用户原话："点击叉号按钮关闭时下方的 Content 面板那里的
        // 粒子按钮不能随之关闭"）。上一版：× 直接写回 panelOpen=false →
        // 底栏那个按钮**整条消失**（底栏的循环里 `if (!panelOpen(i)) continue;`），
        // 想再打开只能绕到底栏的 + 托盘里重新勾一次 —— 用户看到的就是"面板关
        // 了、按钮也没了，状态对不上"。
        //
        // 现在把 × 归到和"最小化"同一条路径：勾选留着、`trayHidden` 置 1 →
        // 窗口收起、底栏按钮**留在原地但变暗**，再点一下就回来。真正"移除
        // 按钮"仍然只有一条路：底栏按钮自己右上角那个 ×（TrayHit::Remove）。
        //
        // 为什么判定放在这里：`drawPanels` 只对**本帧开着的面板**派发，而
        // 派发期间唯一能把 `panelOpen(i)` 改掉的就是面板自己 Begin 里的
        // p_open（= 标题栏的 ×）。菜单/快捷键/托盘叉号的关闭都发生在派发
        // 之前或之后，不会落进这个窗口。
        if (wasOpen && !panelOpen(i) && p.layoutSlot == kSlotNone) {
            if (bool* openSlot = panelOpenSlot(i)) *openSlot = true;  // 勾选留着
            panelTrayHiddenRef(i) = true;    // 窗口收起 → 底栏按钮变暗
            VK_LOG_INFO("PANEL-XCLOSE %s hidden=1", p.id);
        }

        // 浮动面板抢层级 —— 和 Content 抽屉完全同一个理由（那边有长注释）：
        // 一旦点中视口，ImGui 就把视口挪到 g.Windows 末尾，于是"后画的在
        // 上层"只在那一帧成立；此后新建的面板会被视口**盖住**。表现是
        // "在底栏勾上 Physics，面板却看不见" —— 用户会以为按钮坏了。
        // 有弹出层时不提（右键菜单 / 下拉都在 g.Windows 里，硬提会把它们
        // 压在面板下面）；折叠的不提（它只剩一条标题栏，没必要往前挤）。
        if (p.layoutSlot == kSlotNone && p.draw) {
            ImGuiWindow* w = ImGui::FindWindowByName(p.id ? trayWindowName(p.id).c_str()
                                                         : "");
            const bool anyPopup =
                ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                ImGuiPopupFlags_AnyPopupLevel);
            if (w && !w->Collapsed && !anyPopup) {
                ImGui::BringWindowToDisplayFront(w);
            }
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
    // ============================================================
    // 当前工作目录 —— 启动器 D5 的**唯一可断言痕迹**
    // ============================================================
    // 引擎里所有"用户状态"（`config/editor_layout.json` / `config/editor_prefs.json`
    // / `imgui.ini`）都是相对 CWD 的，而启动器是用 `CreateProcessW` 的
    // `lpCurrentDirectory` 把 CWD 设成工程根的。
    //
    // ⚠ 为什么必须**打这一行**而不是靠"布局文件落在工程目录"来判断：
    //   那份布局在**自动化跑**（`MYVK_FRAMES` / `MYVK_NO_WINDOW_SAVE` /
    //   `MYVK_LOG_RECTS` / `MYVK_EDITOR_NO_HUD` 任一存在）里是被**拒写**的
    //   （见 `editorLayoutAutomationRun`）。而验收脚本为了点控件必须开
    //   `MYVK_LOG_RECTS` —— 于是"靠布局文件证明 CWD"在自动化里永远为假，
    //   长得和"CWD 契约不成立"一模一样。日志行不受这条闸门影响。
    {
        std::error_code ec;
        const auto cwd = std::filesystem::current_path(ec);
        VK_LOG_INFO("EDITOR-CWD path=%s",
                    ec ? "(unknown)" : cwd.generic_string().c_str());
    }
    // ============================================================
    // 启动器拉起本进程时的第一件事：认工程（D7）
    // ============================================================
    // `MYVK_EDITOR_PROJECT` = 工程根，或 `<根>/venn.project.json`（两种都收：
    // 前者给人手敲，后者是启动器拼好的现成路径）。
    //
    // 三件事，顺序不能换：
    //   ① `current_path(根)` —— 引擎里所有"用户状态"（`config/editor_layout.json`
    //      / `config/editor_prefs.json` / `imgui.ini`）都是**相对 CWD** 的。
    //      不换就是"两个工程的布局互相覆盖"，而且不报错。
    //   ② 读 `venn.project.json` → 标题栏挂工程名。⚠ 读失败**不改 CWD**：
    //      半打开的工程比打不开难查十倍。
    //   ③ 场景路径来自 `MYVK_EDITOR_SCENE`（下面那段既有钩子），这里不动它。
    //
    // ⚠ 启动器还会用 `CreateProcessW` 的 `lpCurrentDirectory` 直接把 CWD
    //   设成工程根 —— 那才是主路径。这里是**兜底**：用户手敲 env 或者
    //   手工双击 `Editor.exe` 时也能落到同一个状态。
    if (const char* envProj = std::getenv("MYVK_EDITOR_PROJECT")) {
        if (envProj[0] != '\0') {
            namespace fs = std::filesystem;
            std::string root(envProj);
            for (char& c : root)
                if (c == '\\') c = '/';
            // 去掉末尾斜杠，再判断是不是 json 文件路径
            while (root.size() > 1 && root.back() == '/') root.pop_back();
            const std::string kFile = "venn.project.json";
            if (root.size() > kFile.size() &&
                root.compare(root.size() - kFile.size(), kFile.size(), kFile) == 0) {
                const size_t slash = root.find_last_of('/');
                root = (slash == std::string::npos) ? std::string(".")
                                                   : root.substr(0, slash);
            }

            project::ProjectFile pf;
            std::string perr;
            if (project::loadProject(root, pf, &perr)) {
                std::error_code ec;
                fs::current_path(fs::path(root), ec);
                if (ec) {
                    VK_LOG_WARN("EDITOR-PROJECT: cannot chdir to '%s': %s",
                                root.c_str(), ec.message().c_str());
                }
                // 工程声明了起始场景 → 记下来当**默认文档**。真正的加载
                // 在下面那段 `MYVK_EDITOR_SCENE` 钩子里（显式 env 优先，
                // 见那里的一句说明）。不在这里直接 loadScene —— 那段钩子
                // 还要处理 `scenePath` / `pushRecentScene` / 失败降级，
                // 两处各写一遍必然漂移。
                m_projectScene = pf.startScene;
                // 标题栏挂工程名 —— 自动化靠它断言"编辑器打开的确实是我
                // 刚建的那个工程"（`Venn Editor - MyGame`）。
                window().setTitle("Venn Editor - " + pf.name);

                // ---- M5-b：外置代码根的一致性校验 ----
                //
                // 外置代码根靠**目录联接**（`assets/scripts -> <外部目录>`）
                // 实现，引擎完全不知道这件事（脚本路径仍是资产相对路径）。
                // 代价是：联接可以被外部悄悄改掉 / 被删掉 / 被指向别处，
                // 而引擎只会看到"scripts 目录里没有那些文件" —— 表现是
                // "脚本突然不跑了"，没有任何线索。
                //
                // 所以打开工程时对一次账：`venn.project.json` 说 linked，
                // 文件系统上就**必须**真的是个联接、且指向声明的那个目录。
                // 不一致只打 WARN 并继续（用户可以自己修），**不改任何文件**
                // —— 打开工程就悄悄改磁盘是最不该做的事。
                {
                    const std::string scriptsDir = project::scriptsDirAbs(pf);
                    const bool actualLink = project::isDirectoryLink(scriptsDir);
                    if (pf.codeLinked && !actualLink) {
                        VK_LOG_WARN("PROJ-CODE-LINK: %s should be a junction to "
                                    "'%s' but is not linked; scripts may not load",
                                    scriptsDir.c_str(), pf.codeRoot.c_str());
                    } else if (!pf.codeLinked && actualLink) {
                        VK_LOG_WARN("PROJ-CODE-LINK: %s IS linked but "
                                    "venn.project.json says code.linked=false "
                                    "(stale project file?)",
                                    scriptsDir.c_str());
                    } else if (pf.codeLinked) {
                        const std::string target =
                            project::normalizePath(project::directoryLinkTarget(
                                scriptsDir));
                        if (target != project::normalizePath(pf.codeRoot)) {
                            VK_LOG_WARN("PROJ-CODE-LINK: junction points to '%s' "
                                        "but venn.project.json says '%s'",
                                        target.c_str(), pf.codeRoot.c_str());
                        } else {
                            VK_LOG_INFO("PROJ-CODE-LINK ok '%s' -> '%s'",
                                        scriptsDir.c_str(), target.c_str());
                        }
                    }
                }
                VK_LOG_INFO("EDITOR-PROJECT name=%s type=%s scene=%s root=%s",
                            pf.name.c_str(), project::gameTypeId(pf.gameType),
                            pf.startScene.c_str(), pf.root.c_str());
            } else {
                VK_LOG_WARN("EDITOR-PROJECT: '%s' is not a project: %s",
                            root.c_str(), perr.c_str());
            }
        }
    }

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
    // Content 抽屉（2026-10-17 起默认关着）的自动化钩子：
    //   MYVK_EDITOR_CONTENT_DRAWER=1 → 启动就打开抽屉；
    //   MYVK_EDITOR_CONTENT_DOCK=1   → 顺带勾上 "Dock in layout"。
    // MYVK_CONTENT_DIR 也算"要打开" —— 那一族脚本（CB-ENTRIES / 材质拖拽）
    // 本来就假定内容面板看得见；面板不 draw 的话连 CB-ENTRIES 都不会打，
    // 它们会以"找不到日志"的形态失败，而不是报真正的原因。
    if (std::getenv("MYVK_EDITOR_CONTENT_DRAWER") ||
        std::getenv("MYVK_CONTENT_DIR")) {
        m_ctx->panels().content = true;
    }
    if (std::getenv("MYVK_EDITOR_CONTENT_DOCK")) {
        m_ctx->panels().content = true;
        m_contentDock = true;
    }
    // ---- 脚本运行时（S0）----
    m_scriptRuntime = std::make_unique<vks::ScriptRuntime>();
    m_scripts = &m_scriptRuntime->engine();
    // S5：面板 / Inspector / 控制台三处都从 Context 取它（只借不拥有）
    m_ctx->setScriptRuntime(m_scriptRuntime.get());

    // 宿主能力（ScriptServices）。**与 RuntimeApp 填的是同一批回调**，
    // 差别只在实现：编辑器没有 HudModel → 用数值镜像；音频两边都有。
    // S0 之前编辑器这份循环压根没接 HUD/音频，同一个脚本在编辑器 Play
    // 里 `playSound` 静默不响（P2）。
    {
        vks::ScriptServices svc;
        svc.setHealth = [this](float v) { m_hudMirror.health = v; };
        svc.damage = [this](float v) {
            m_hudMirror.health -= v;
            ++m_hudMirror.popups;
            m_hudMirror.lastPopup = v;
        };
        svc.popup = [this](float, float, float, float amount) {
            ++m_hudMirror.popups;
            m_hudMirror.lastPopup = amount;
        };
        svc.playSound = [this](const std::string& clip) {
            m_audio.playOneShot(clip, audio::BusId::SFX, nullptr, 1.0f);
        };
        svc.stopAllSounds = [this]() { m_audio.stopAll(); };
        svc.setBusVolume = [this](const std::string& bus, float v) {
            const int idx = audio::busIndexFromName(bus.c_str());
            if (idx < 0) {
                // 认不出就留痕（与 RuntimeApp 同一条规矩：静默忽略会让
                // "我把音量调到 0.3 了怎么没变"变成一次排查）
                VK_LOG_WARN("Editor script setBusVolume: 未知总线 '%s'"
                            "（可用：master|music|sfx|voice）",
                            bus.c_str());
                return;
            }
            m_audio.setBusGain(static_cast<audio::BusId>(idx), v);
        };
        // ---- 玩家 / 输入（S3）----
        // 编辑器没有 PlayerController（那是 runtime 的概念），于是这里用
        // **结构判据**：场景里第一个带 CharacterControllerComponent 的实体。
        // 语义与 runtime 一致（"被接管的那一个"），只是接管者不同。
        svc.act.player = [this]() -> ecs::Entity {
            ecs::Entity found{};
            m_ctx->runtimeScene().world().each<ecs::CharacterControllerComponent>(
                [&](ecs::Entity e, ecs::CharacterControllerComponent&) {
                    if (!found.valid()) found = e;
                });
            return found;
        };
        svc.act.pressed = [this](const std::string& a) {
            return m_ctx->inputMap().wasPressed(a);
        };
        svc.act.held = [this](const std::string& a) {
            return m_ctx->inputMap().isHeld(a);
        };
        svc.act.released = [this](const std::string& a) {
            return m_ctx->inputMap().wasReleased(a);
        };
        svc.act.axis = [this](const std::string& a) {
            return m_ctx->inputMap().axis(a);
        };
        m_scriptRuntime->setServices(std::move(svc));

        m_scriptRuntime->setPrintSink(
            [this](const vks::ScriptInstance& inst, const std::string& line) {
                m_printLog.push_back(inst.path + ": " + line);
                if (m_printLog.size() > 200) {
                    m_printLog.erase(
                        m_printLog.begin(),
                        m_printLog.begin() + static_cast<std::ptrdiff_t>(
                                                 m_printLog.size() - 200));
                }
                // 脚本的 print 默认只进面板（用户脚本可能每帧打印，落日志
                // 会淹掉它）。自动化需要"脚本真的跑起来了"的硬证据，所以给
                // 一个显式开关：`MYVK_LOG_SCRIPT_PRINT=1`。
                static const bool logPrints = [] {
                    const char* e = std::getenv("MYVK_LOG_SCRIPT_PRINT");
                    return e && e[0] == '1';
                }();
                if (logPrints)
                    VK_LOG_INFO("VKS-PRINT %s: %s", inst.path.c_str(),
                                line.c_str());
            });
        // ⚠ 日志措辞与 S0 之前**逐字相同**：
        //   "Script not runnable (path): msg" / "Script runtime error (path): msg"
        m_scriptRuntime->setErrorSink(
            [this](const vks::ScriptInstance& inst, const std::string& msg) {
                const bool compileErr =
                    inst.errorKind == vks::ScriptInstance::ErrorKind::Compile;
                m_scripts->logLine(
                    std::string(compileErr ? "[compile] " : "[runtime] ") +
                    inst.path + " > " + msg);
                VK_LOG_WARN("Script %s (%s): %s",
                            compileErr ? "not runnable" : "runtime error",
                            inst.path.c_str(), msg.c_str());
            });
    }
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
    // 2026-10-17 晚：底栏"+"托盘里的两张新面板。都只借用 EditorContext /
    // Renderer 的引用，构造无副作用。
    m_physicsPanel = std::make_unique<editor::PhysicsPanel>(*m_ctx);
    m_particlesPanel =
        std::make_unique<editor::ParticlesPanel>(*m_ctx, renderer());
    // E26：画质档位面板。构造无副作用（不碰设备），参数走 FeatureRegistry ——
    // 与启动器写进 `config/render.json` 的是同一份表（QualityPresets）。
    m_qualityPanel = std::make_unique<editor::QualitySettingsPanel>();
    // 2026-10-24：材质仓库面板。构造无副作用 —— 仓库本身是**惰性加载**的
    // （见 EditorContext::materialLibrary），没打开面板就一次磁盘都不读。
    // 地形编辑器的指针在它构造之后才 attach（此时 m_landscape 还没建）。
    m_materialLib = std::make_unique<editor::MaterialLibraryPanel>(*m_ctx);
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
    // 主题 / 语言读回来之后要**真应用一次** —— 窗口已经建好了，样式是
    // ImGuiManager::init 用出厂主题画的，不改这一步存档就形同虚设。
    applyAppearance();
    m_viewport = std::make_unique<ViewportPanel>(*m_ctx, renderer());
    // 2026-10-20：地形 / 植被两个编辑模式。必须先于 ViewportPanel 的
    // attach 构造 —— 视口每帧要把"模式工具"的输入转发给它们。
    m_landscape = std::make_unique<LandscapeEditor>(*m_ctx);
    // 2026-10-20 晚：编辑模式下拉里其余 6 个模式的实现（ToolkitEditors.h）。
    // PCG 要问地形高度/坡度，所以拿 landscape 的引用；其余只要 ctx。
    m_meshPaint = std::make_unique<MeshPaintEditor>(*m_ctx);
    m_modeling = std::make_unique<ModelingEditor>(*m_ctx);
    m_fracture = std::make_unique<FractureEditor>(*m_ctx);
    m_brushEdit = std::make_unique<BrushEditor>(*m_ctx);
    m_animation = std::make_unique<AnimationEditor>(*m_ctx);
    if (m_landscape) {
        m_pcg = std::make_unique<PCGEditor>(*m_ctx, *m_landscape);
    }
    m_foliage = std::make_unique<FoliageEditor>(*m_ctx, *m_landscape);
    // 2026-10-21：水体编辑（拖出水体 + 水质参数面板）
    m_water = std::make_unique<WaterEditor>(*m_ctx);
    m_viewport->attachModeTools(m_landscape.get(), m_foliage.get(),
                                m_water.get());
    // 材质仓库面板要能"把材质设成地形笔刷"（写 layerOverrides + 切工具），
    // 所以要等 m_landscape 建好之后再接线。
    if (m_materialLib) m_materialLib->attachLandscape(m_landscape.get());
    m_viewport->attachToolkitTools(m_meshPaint.get(), m_brushEdit.get(),
                                 m_modeling.get());
    m_hierarchy = std::make_unique<SceneHierarchy>(*m_ctx);
    m_inspector = std::make_unique<InspectorPanel>(*m_ctx);
    m_content = std::make_unique<ContentBrowser>(*m_ctx);
    m_characterWindow = std::make_unique<CharacterWindow>(*m_ctx, renderer());
    // #310 剧本编辑器（窗口对象，生命周期与 Character 同构）
    m_dlgEditor = std::make_unique<DialogueEditorWindow>(*m_ctx);
    // #311 时间轴编辑器（同上）
    m_tlEditor = std::make_unique<TimelineEditorWindow>(*m_ctx);
    // #312 UI 编辑器（同上）
    m_uiEditor = std::make_unique<UiEditorWindow>(*m_ctx);

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

    // ---- 编辑模式（2026-10-20）----
    // MYVK_EDITMODE=<select|landscape|foliage|...>：启动即进该模式。
    // 自动化用它避开"点顶栏下拉"这条含 UI 坐标的路径（下拉还要先点开）。
    if (const char* m = std::getenv("MYVK_EDITMODE")) {
        bool hit = false;
        for (int i = 0; i < kEditModeCount; ++i) {
            const EditMode em = static_cast<EditMode>(i);
            if (std::strcmp(editModeId(em), m) != 0) continue;
            m_ctx->setEditMode(em);
            // ⚠ 故意**不**同步 m_lastEditMode：让第一帧 serviceEditMode 看到
            //   "从 Select 切到了 X"的差量，于是面板 / 焦点 / 相机一次性到位。
            hit = true;
            break;
        }
        if (!hit) VK_LOG_WARN("MYVK_EDITMODE: 未知模式 '%s'", m);
    }
    // MYVK_LANDSCAPE_AUTOCREATE=1：第 30 帧在空场景里建一片默认地形。
    m_autoCreateTerrain = std::getenv("MYVK_LANDSCAPE_AUTOCREATE") != nullptr;

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
    //   MYVK_EDITOR_IMPORT=路径  启动即导入一个模型（等价于从 Content 拖进视口）
    //   MYVK_EDITOR_CAM="x,y,z,yawDeg,pitchDeg"   把视口相机摆到指定位姿
    //   MYVK_EDITOR_CLIP="near,far"               钉死视口相机裁剪面（默认 0.1/5000）
    //   MYVK_EDITOR_GIZMO=move|rotate|scale   启动即切手柄模式
    //   MYVK_EDITOR_COLLIDER=hull|capsule     给选中项加碰撞体并进入碰撞体编辑
    //   MYVK_EDITOR_PREFS=1      启动即打开 Preferences 窗口（截图验证用）
    //   MYVK_EDITOR_NO_HUD=1     关掉视口里的统计 / 导航叠加层，纯 3D 出图。
    //       自动化截图的像素断言需要"同一配置两次运行背景逐像素相同"，而
    //       叠加层里的 FPS / 帧时间每次都不同，会在背景上留下几阶噪声。
    if (std::getenv("MYVK_EDITOR_NO_HUD")) m_viewport->setHudVisible(false);
    // 场景来源的优先级：显式 `MYVK_EDITOR_SCENE` > 工程的 `venn.project.json`
    // 里声明的 `startScene`（启动器拉起时很常见：它只传工程根）。
    // ⚠ 别把工程的 startScene **写回环境变量** —— 那是"改自己的环境去当
    //   默认值"，同一进程里后续任何读到它的地方都会跟着变，且不可撤回。
    const char* envScene = std::getenv("MYVK_EDITOR_SCENE");
    if ((!envScene || envScene[0] == '\0') && !m_projectScene.empty())
        envScene = m_projectScene.c_str();
    if (envScene) {
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
    // ---- MYVK_EDITOR_IMPORT=相对路径：启动即导入一个模型 ----
    // 等价于"从 Content 面板把它拖进视口"。给自动化用：ImGui 的拖拽载荷
    // 没法从外部模拟，而"拖进来会建 SkinnedMeshComponent"正是角色链
    // A0-1 的核心断言（在这之前，主场景的导入路径根本不判 skinned）。
    // 必须在 MYVK_EDITOR_SCENE 之后：先开文档，再往里放东西。
    //
    // 支持 `;` 分隔多个路径（`a.fbx;b.fbx`）—— A0-4 的否定测试要"同一个
    // FBX 实例化 3 次"，靠它一次起完。第 i 个沿 +X 排开 1.5 m（Z-up 世界里
    // X 是水平轴），三个都要在视口里分得开才截得到。名字由
    // `EditorScene::instantiateModel` 的唯一化后缀区分（`anime_girl_1`…）。
    if (const char* envImport = std::getenv("MYVK_EDITOR_IMPORT")) {
        int slot = 0;
        std::string all(envImport);
        size_t pos = 0;
        while (pos <= all.size()) {
            const size_t semi = all.find(';', pos);
            std::string one = all.substr(
                pos, semi == std::string::npos ? std::string::npos : semi - pos);
            const size_t b = one.find_first_not_of(" \t");
            const size_t e2 = one.find_last_not_of(" \t");
            one = (b == std::string::npos) ? std::string()
                                           : one.substr(b, e2 - b + 1);
            if (!one.empty()) {
                const ImportResult ir = instantiateModel(
                    m_ctx->editorScene(), am, one,
                    glm::vec3(1.5f * static_cast<float>(slot), 0.0f, 0.0f));
                if (ir.ok()) {
                    m_ctx->select(ir.root);
                    const bool skinned =
                        m_ctx->editorScene()
                            .world()
                            .has<ecs::SkinnedMeshComponent>(ir.root);
                    VK_LOG_INFO("IMPORT-AUTO path=%s slot=%d root=%u meshes=%d "
                                "skinned=%d",
                                one.c_str(), slot,
                                static_cast<unsigned>(ir.root.id), ir.count(),
                                skinned ? 1 : 0);
                    ++slot;
                } else {
                    VK_LOG_WARN("IMPORT-AUTO failed: %s", one.c_str());
                }
            }
            if (semi == std::string::npos) break;
            pos = semi + 1;
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

    // ---- MYVK_EDITOR_DLGED=<路径>：启动即打开剧本编辑器（#310）----
    // 走的是和 Inspector "Edit script…" 按钮**同一条**请求通道（ctx 里那个
    // 一次性请求），所以自动化验的是真入口，不是给测试开的旁路。
    // 配套的两个钩子在 DialogueEditorWindow 自己手里：
    //   MYVK_EDITOR_DLGED_SCRIPT="sel:2;settext:…;save"   注入命令（只跑一次）
    //   MYVK_EDITOR_DLGED_DUMP=<路径>                     把当前资产落成 JSON
    if (const char* envDlg = std::getenv("MYVK_EDITOR_DLGED")) {
        m_ctx->requestDialogueEditor(envDlg);
        VK_LOG_INFO("Editor: script-editor request path='%s'", envDlg);
    }

    // ---- MYVK_EDITOR_TLED=<路径>：启动即打开时间轴编辑器（#311）----
    // 与上面 DLGED 那条**同一条**请求通道（ctx 的一次性请求），所以自动化
    // 验的是"Inspector 点 Edit timeline…"那个真入口。
    // 配套两个钩子在 TimelineEditorWindow 自己手里：
    //   MYVK_EDITOR_TLED_SCRIPT="sel:camera:1;settime:2.5;save"  注入命令
    //   MYVK_EDITOR_TLED_DUMP=<路径>                             把资产落成 JSON
    if (const char* envTl = std::getenv("MYVK_EDITOR_TLED")) {
        m_ctx->requestTimelineEditor(envTl);
        VK_LOG_INFO("Editor: timeline-editor request path='%s'", envTl);
    }

    // ---- MYVK_EDITOR_UIED=<路径>：启动即打开 UI 编辑器（#312）----
    // 第三个资产编辑器，走同一条 ctx 一次性请求通道。
    //   MYVK_EDITOR_UIED_SCRIPT="sel:hud/hpbar;setpos:40,40;save"  注入命令
    //   MYVK_EDITOR_UIED_DUMP=<路径>                               落成 JSON
    if (const char* envUi = std::getenv("MYVK_EDITOR_UIED")) {
        m_ctx->requestUiEditor(envUi);
        VK_LOG_INFO("Editor: ui-editor request path='%s'", envUi);
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

    // ---- MYVK_EDITOR_CLIP="near,far"：把视口相机的裁剪面钉死 ----
    // 自动化专用，和 MYVK_EDITOR_CAM 同一类钩子（同样必须排在
    // MYVK_EDITOR_SCENE 之后：加载场景会连相机一起覆盖）。
    //
    // 为什么需要它：编辑器视口默认 0.1 / **5000** m（见
    // applyEditorCameraMode 的注释），而视锥剔除 / BVH 那两个用例是**按
    // 200 m 远的"看不见"来设计场景**的（39 个立方体摆在 x=250..4050）。
    // 远平面一放开到 5 km，那 39 个就名正言顺地进了视锥 → culled 从 132
    // 掉到 94。它们的基线（132 / 4）本身没错，错在**依赖了编辑器的默认
    // 值**；把默认钉回它们设计时的那一档，用例就重新只测"剔除算得对不对"，
    // 而不是顺带测"编辑器的远平面是多少"。
    if (const char* envClip = std::getenv("MYVK_EDITOR_CLIP")) {
        float nz = 0.0f, fz = 0.0f;
        if (std::sscanf(envClip, "%f,%f", &nz, &fz) == 2 && fz > nz) {
            m_ctx->editorScene().camera().setClipPlanes(nz, fz);
            VK_LOG_INFO("Editor: clip planes near=%.3f far=%.1f (MYVK_EDITOR_CLIP)",
                        (double)m_ctx->editorScene().camera().nearPlane(),
                        (double)m_ctx->editorScene().camera().farPlane());
        } else {
            VK_LOG_WARN("MYVK_EDITOR_CLIP 解析失败: '%s'（期望 near,far）",
                        envClip);
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
        else if (g == "select")
            m_ctx->setGizmoMode(GizmoMode::Select);
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

    // MYVK_EDITOR_SNAP=0|1：吸附默认值自 2026-10-17 起改成**全开**了。
    // 但"拖一段像素 → 期望一个连续的数值"这类断言在吸附下是不确定的
    // （落点被量化到 0.1 m / 10 度 / 0.1 倍，拖得不够远还会直接量化成 0），
    // 所以给需要**原始拖拽**的脚本留一个关掉的口子。
    // 不设这个变量 = 用用户默认值（全开）。
    if (const char* envSnap = std::getenv("MYVK_EDITOR_SNAP")) {
        const bool on = (envSnap[0] != '0');
        m_ctx->snapMoveRef() = on;
        m_ctx->snapRotateRef() = on;
        m_ctx->snapScaleRef() = on;
    }

    // ---- G06：键位绑定表 ----
    // 读盘（自动化下默认路径不读，见 InputMapIO 的规则说明）→ 再跑注入脚本。
    // 顺序不能反：脚本是在**读出来的表**上改。
    m_ctx->loadInputMap();
    if (const char* rebind = std::getenv("MYVK_EDITOR_REBIND"))
        applyRebindScript(*m_ctx, rebind);

    // MYVK_EDITOR_BIND_CAPTURE=<context>/<action>：开机就进"等待按键"态。
    //
    // 存在的意义只有一条：**捕获态那块 UI 要能被截图和断言**。真正的捕获
    // 必须按真实按键，自动化里没有可靠手段（合成按键只在窗口有焦点时成立），
    // 所以"进入捕获态"这件事单独开个口子，剩下的交给 G/H 组去验面板。
    if (const char* cap = std::getenv("MYVK_EDITOR_BIND_CAPTURE")) {
        const std::string s = cap;
        const std::size_t slash = s.find('/');
        if (slash != std::string::npos && slash > 0 && slash + 1 < s.size()) {
            m_ctx->beginInputCapture(s.substr(0, slash), s.substr(slash + 1));
            VK_LOG_INFO("INS-BIND-CAPTURE-ARMED %s", cap);
        } else {
            VK_LOG_WARN("MYVK_EDITOR_BIND_CAPTURE='%s' 需要 <context>/<action>",
                        cap);
        }
    }

    // 自动化可观测：开机**生效后**的吸附 / 栅格 / 主题 / 语言一把打出来。
    // 为什么需要：这四项都是"用户默认值"，改完只有界面上几个勾选框能看出来，
    // 脚本没法断言（也不该去数像素里的对勾）。打一行结构化状态最省事。
    // 只在 MYVK_LOG_RECTS 下打 —— 那是本项目统一的"自动化观测开关"。
    if (std::getenv("MYVK_LOG_RECTS")) {
        const render::GridSettings& g = renderer().gridSettings();
        VK_LOG_INFO(
            "EDITOR-STATE snap.move=%d snap.rotate=%d snap.scale=%d "
            "step.move=%.3f step.rotate=%.1f step.scale=%.3f "
            "grid.extent=%.1f grid.minor=%.3f grid.major=%.3f "
            "grid.fadeStart=%.1f grid.fadeEnd=%.1f theme=%d language=%d",
            m_ctx->snapMove() ? 1 : 0, m_ctx->snapRotate() ? 1 : 0,
            m_ctx->snapScale() ? 1 : 0,
            static_cast<double>(m_ctx->snapMoveStep()),
            static_cast<double>(m_ctx->snapRotateStep()),
            static_cast<double>(m_ctx->snapScaleStep()),
            static_cast<double>(g.extent), static_cast<double>(g.minorStep),
            static_cast<double>(g.majorStep),
            static_cast<double>(g.fadeStart),
            static_cast<double>(g.fadeEnd), ui::currentThemeIndex(),
            language());
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

    // ---- 音频（G14-7/8）----
    //
    // 编辑器默认**开**（试听按钮点下去要有声）；`MYVK_EDITOR_AUDIO=0` 关掉，
    // 无声卡机器 / 想把设备让给别的进程时用。
    // ⚠ 与运行时同一条纪律：设备打不开不是错误，`init` 内部进 Failed，
    //   之后所有 API 是空操作 —— 编辑器照常能用，只是没声。
    {
        audio::AudioSettings as;
        as.device = "default";
        as.enabled = true;
        if (const char* env = std::getenv("MYVK_EDITOR_AUDIO")) {
            as.enabled = !(env[0] == '0' && env[1] == '\0');
        }
        // `MYVK_EDITOR_AUDIO_NULL=1`：只验证链路，不真的发声（静音环境）
        if (std::getenv("MYVK_EDITOR_AUDIO_NULL")) as.forceNull = true;
        // 编辑器的试听走 SFX 总线；音乐总线压低一点，将来 BGM 不至于太吵
        as.music = 0.7f;
        m_audio.init(as);
        VK_LOG_INFO("EDITOR-AUDIO enabled=%d backend=%s device=%s sr=%u ch=%u",
                    m_audio.running() ? 1 : 0, m_audio.backendName(),
                    m_audio.deviceName().c_str(),
                    m_audio.deviceFormat().sampleRate,
                    m_audio.deviceFormat().channels);
    }

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
        m_ctx->notify(std::string(TR("Workspace: ")) + TR(kWorkspaces[idx].title));
    }

    // ---- E16：Console 命令注入（自动化验收用）----
    // MYVK_EDITOR_CONSOLE_CMD="set ssr.enabled 1"：第 40 帧执行一次。
    // 走的是与 UI/命令面板完全同一条 execute 路径（含 emitParamChange
    // 的副作用），所以日志里能看到 FTR-UI / PRESET-APPLY 等锚点。
    //
    // 支持 `;` 分隔多条（`anim.clip Walk;anim.time 0.35`）：动画对照需要
    // "先切 clip 再钉时间"两刀，而一个环境变量只能传一个字符串。
    // ⚠ 现有命令的参数里没有 `;`，按它切是安全的；将来若有命令需要分号，
    //   得先把它换成别的分隔符（比如 `|`）—— 别在下游偷偷加转义。
    // MYVK_EDITOR_PANELS="scripts,console"：启动时按 id 打开面板。
    // 为什么需要它：`MYVK_EDITOR_PANELS` 之前，自动化只能靠 Alt+N 快捷键，
    // 而 ImGui 的 Alt 组合键要求宿主真的把 Alt 键状态喂进去 —— PostMessage
    // 发过去时它常常是"半按"状态，于是快捷键时灵时不灵。面板的**打开**
    // 这件事不该依赖键盘（S5 的 F1 就是被这个坑住了）。
    if (!m_panelsOpenedOnce && m_editorFrames >= 3) {
        m_panelsOpenedOnce = true;
        if (const char* ids = std::getenv("MYVK_EDITOR_PANELS")) {
            std::string all(ids);
            size_t pos = 0;
            while (pos <= all.size()) {
                const size_t comma = all.find(',', pos);
                std::string one = all.substr(
                    pos, comma == std::string::npos ? std::string::npos
                                                    : comma - pos);
                if (!one.empty()) {
                    const int idx = panelIndexOf(one.c_str());
                    if (idx >= 0) {
                        setPanelOpen(idx, true);
                        // 收进托盘的话，面板"开着"却一帧都不画 —— 锚点
                        // 全都不出现。自动化要的是"看得见"。
                        panelTrayHiddenRef(idx) = 0;
                        VK_LOG_INFO("PANEL %s open=1 reason=env", one.c_str());
                    }
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
    }

    // `MYVK_EDITOR_CONSOLE_CMD_FRAME` 可把"第 40 帧"往后推 —— S5 的 F6 要在
    // **Play 之后**读变量（编辑态脚本不跑，`script.vars` 只会是 0 条）。
    static const long kConsoleCmdFrame = [] {
        if (const char* v = std::getenv("MYVK_EDITOR_CONSOLE_CMD_FRAME")) {
            const long n = std::atol(v);
            if (n > 0) return n;
        }
        return 40L;
    }();
    if (!m_consoleCmdDone &&
        static_cast<long>(m_editorFrames) >= kConsoleCmdFrame) {
        m_consoleCmdDone = true;
        if (const char* cmd = std::getenv("MYVK_EDITOR_CONSOLE_CMD")) {
            std::string all(cmd);
            size_t pos = 0;
            while (pos <= all.size()) {
                const size_t semi = all.find(';', pos);
                std::string one = all.substr(
                    pos,
                    semi == std::string::npos ? std::string::npos : semi - pos);
                const size_t b = one.find_first_not_of(" \t");
                const size_t e2 = one.find_last_not_of(" \t");
                one = (b == std::string::npos) ? std::string()
                                               : one.substr(b, e2 - b + 1);
                if (!one.empty()) m_console->execute(one);
                if (semi == std::string::npos) break;
                pos = semi + 1;
            }
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

    // ---- 音频（G14-7/8）----
    // 放在这里而不是 `onImGui()`：只有 onUpdate 手里有 `dt`，而音频的
    // ducking 包络 / 失败重试计时都要它（播放位置不靠它 —— 那是声卡时钟）。
    syncEditorAudio(dt);

    // ---- 脚本热重载：定时扫文件时间戳 ----
    m_hotReloadTimer += dt;
    if (m_hotReloadTimer >= 0.5f) {
        m_hotReloadTimer = 0.0f;
        const int reloaded = m_scripts->pollHotReload();
        preloadScripts();  // 场景里新增/删除了脚本也要跟上
        if (reloaded > 0) {
            m_ctx->notify(std::string(TR("Hot-reloaded ")) +
                          std::to_string(reloaded) + TR(" script file(s)"));
        }
    }

    // ---- E08：播放倍速 ----
    // 夹到 [0.05, 4]：0 会让脚本时间冻住（看起来像卡死），负数会让物理
    // 反向积分。倍速只乘在**仿真** dt 上，渲染帧率不受影响。
    m_timeScale = std::clamp(m_timeScale, 0.05f, 4.0f);
    const float simDt = dt * m_timeScale;

    // ---- Play 态鼠标捕获（P8）：必须在 runPlayers 之前 ----
    // runPlayers 按 `playMouseCaptured()` 门控键鼠；晚一帧就会出现"点了
    // 视口还要再等一帧才能动"的黏滞感。
    updatePlayMouse();

    // ---- Run：游戏逻辑跑在**运行态副本**上 ----
    // 渲染那一边不用额外做什么：activeScene() 在 Run 期间已经返回运行态，
    // 视口自然会显示这份世界。
    //
    // ⚠ 这条注释以前写的是"动画跑在编辑场景上"，与 `EditorContext` 的设计
    //   （`EditorContext.h` 开头：脚本、**动画**只跑在运行态副本上）矛盾。
    //   实际是**两边都没有骨骼动画系统** —— 直到 A0-3 才接上，现在遵循
    //   设计：骨骼动画也在 activeScene() 上跑（Play 时 = 运行态副本）。
    if (m_ctx->isPlaying()) {
        // 一次新的 Play 开始（上一段 Play 的帧计数已被下面的 else 归零，
        // 或者这是启动即 Play、计数器还是初值）→ 把详细日志预算灌满。
        if (m_collisionFrames == 0) m_collisionLogBudget = 240;
        m_scriptTableDropped = false;   // 下一段非运行态要重新丢一次
        m_ctx->advancePlayTime(simDt);
        runRuntimeScripts(simDt);
        // 玩家在脚本之后 / 碰撞之前：脚本可能挪物体，玩家再动自己，
        // 碰撞最后保证谁都不穿谁（玩家也被墙挡）。
        runPlayers(simDt);
        // ---- 骨骼动画（A0-3）----
        // 排在物理**之前**：A4 的根运动会把位移写进 TransformComponent，
        // 物理要读到它这一帧的最终位置（与"运动学平台先摆好再求解"同理）。
        m_animator.tick(m_ctx->activeScene(), simDt);
        // ---- 过场时间轴（C1）----
        // 排在动画之后：运镜与姿势是两条独立的时间线，先后无所谓；放在
        // 这里是因为它必须早于本帧的渲染，且要拿到**本帧最终**的物体位置
        // 才好对焦（将来做 LookAt 轨道时这一点是硬要求）。
        //
        // ⚠ 编辑器视口是**自由相机**（不经过 `CameraSystem`），所以直接写
        //   `scene.camera()`。运行时要走 `CameraSystem`，见 RuntimeApp。
        {
            sequence::SequencerSystem& seq = sequence::activeSequencerSystem();
            seq.tick(m_ctx->activeScene(), simDt);
            if (seq.hasPose())
                sequence::applyPoseToSceneCamera(m_ctx->activeScene(), seq.pose());
        }
        // ---- R7-1 物理 ----
        // 世界在进入 Play 的第一帧建一次（`!m_physics` 就是"还没建"），
        // 之后每帧推进 + 把解算结果写回运行态 TransformComponent。
        //
        // 顺序：脚本 → 玩家 → **物理** → 碰撞。
        //   · 物理在脚本/玩家之后：脚本可能移动**运动学**物体（平台/门），
        //     物理要读到它这一帧的最终位置才能算出正确的撞击速度。
        //   · 物理在碰撞之前：碰撞里的相机推出必须基于**本帧最终**的物体
        //     位置，否则相机会卡在"已经移走了的箱子"里。
        if (!m_physics) buildPhysicsWorld();
        stepPhysics(simDt);
        // 碰撞必须在脚本**之后**：脚本负责让物体动，碰撞负责不让它们穿过去。
        // 反过来（先碰撞后脚本）等于每一帧结束时都留一次穿透。
        runCollision(simDt);
    } else {
        // 回到编辑态/暂停态就把帧计数归零，这样每次重新 Run
        // 都会重新打一条脚本执行汇总
        m_runtimeFrames = 0;
        // 实例表只在**真正 Stop**（含编辑态）时丢一次。
        // ⚠ 以前这里是每帧无条件 `reset()`，两处后果：
        //   ① Pause 也算"非 Play" → 一暂停表就没了 → 恢复时脚本从头再来，
        //      `wait` 挂在哪、变量到几，全部丢掉（暂停 = 静默重启）；
        //   ② 编辑态表永远是空的 → Instances 面板一帧都画不出行。
        //   现在按 `usingRuntime()` 判（与物理世界丢弃用同一个判据）。
        if (m_scriptRuntime && !m_ctx->usingRuntime() && !m_scriptTableDropped) {
            m_scriptRuntime->reset();
            m_scriptTableDropped = true;
        }
        // 编辑态：按**编辑场景**同步一张实例表（只列出来、一行都不执行）。
        // 这是 Instances 面板 / Inspector 的 Script 段在编辑态唯一的数据源；
        // 没有它，面板上连"这个物体挂了脚本"都看不见（S5 F1 就是栽在这）。
        if (m_scriptRuntime && !m_ctx->usingRuntime()) {
            m_scriptRuntime->sync(m_ctx->editorScene());
        }
        m_hudMirror = HudMirror{};
        m_collisionFrames = 0;
        m_collisionLogBudget = 240;
        // 物理世界随运行态一起丢弃（Stop 之后编辑场景从没被动过，所以
        // 不需要任何回滚 —— 这条是"Stop 即还原"能成立的关键一环）。
        //
        // ⚠ 只在**真正 Stop** 时丢。Pause 走的是同一个 else 分支（isPlaying()
        //   为假），但暂停后恢复必须接着跑，所以判据是 usingRuntime()：
        //   Stopped=false / Playing|Paused=true。
        if (!m_ctx->usingRuntime()) {
            destroyPhysicsWorld();
            m_physicsFrames = 0;
            // 停止后四态动画的游标一并丢掉：下一次 Play 从头起算
            // （留着的话"上次 Play 停在第 16 秒 idle"会带着走 —— 一按 Play
            //   就立刻重放一次 idle，看起来像"刚进游戏人自己抖了一下"）。
            m_playerLoco.clear();
            // 编辑态：物理没在跑（面板据此显示原因而不是一片 0）
            m_ctx->setPhysicsRuntime(PhysicsRuntimeInfo{});
        }
        // ---- 骨骼动画：非 Play 态也要刷新 ----
        // dt=0 → 不推进时间，只按**当前** time 重算关节矩阵。这是把
        // "拖 Inspector 时间轴 / 换 clip 立刻能看到姿势"这件事接上的地方；
        // 也让编辑态的画面与拆分前（updateSkinning 每帧现场采样）一致。
        m_animator.tick(m_ctx->activeScene(), 0.0f);
    }

    // ---- 动画（编辑态脚本）：Play 走、Pause 冻结、Stop 恢复快照 ----
    if (m_ctx->isEditing() && m_ctx->animPlaying()) {
        m_ctx->advanceAnimTime(simDt);
        runAnimScripts(simDt);
    } else {
        m_animFrames = 0;
    }

    // ---- 逐帧时基探针（MYVK_LOG_DT=1）----
    //
    // 用途：诊断"移动/转视角时角色动作卡顿"这一类**时基**问题。它把三件事
    // 放在同一行里，缺一不可：
    //   dt     —— 这一帧的真实步长（抖不抖、有没有尖刺）
    //   pos    —— 玩家本帧的落点（位移在空间上是否连续）
    //   animT  —— 骨骼动画的播放头（姿势相位是否按 dt 线性推进）
    // 三者放在一起才能分辨"帧率抖"、"位移抖"、"动画相位抖" —— 只看帧率
    // 数字会漏掉后两种，只看姿势又分不清前两种。
    //
    // 配套：`MYVK_VSYNC=1|0` 切呈现模式（见 core/RunMode.h）。量"锁帧之后
    // 是不是真的每帧 16.7 ms"必须显式给 `MYVK_VSYNC=1` —— 自动化脚本默认
    // 是不锁的（跑得越快越好）。
    if (std::getenv("MYVK_LOG_DT")) {
        double px = 0.0, py = 0.0, pz = 0.0, animT = -1.0;
        if (m_ctx->isPlaying()) {
            ecs::World& rw = m_ctx->runtimeScene().world();
            rw.each<ecs::PlayerComponent>(
                [&](ecs::Entity e, const ecs::PlayerComponent&) {
                    if (const auto* t = rw.get<ecs::TransformComponent>(e))
                        px = t->position.x, py = t->position.y, pz = t->position.z;
                    if (const auto* s = rw.get<ecs::SkinnedMeshComponent>(e))
                        animT = s->time;
                });
        }
        VK_LOG_INFO("FRAME-DT f=%llu dt=%.5f play=%d cap=%d "
                    "pos=(%.4f,%.4f,%.4f) animT=%.4f",
                    static_cast<unsigned long long>(m_editorFrames),
                    static_cast<double>(dt), m_ctx->isPlaying() ? 1 : 0,
                    m_ctx->playMouseCaptured() ? 1 : 0, px, py, pz, animT);
    }
}

// ---------------------------------------------------------------- 窗口收尾

// 关窗口 / 菜单 Quit / 跑满帧数都会走到这里（窗口此时还活着）。
// 只做一件事：把窗口几何存下来，下次打开接着用。
void EditorApp::onShutdown() {
    // 音频先关：它有一条线程，必须在日志 / 引擎资源之前停掉。
    // ⚠ 放在最前面而不是最后面 —— 后面的落盘代码会打日志，那没问题；
    //   但 `onShutdown` 之后对象就析构了，晚一步就可能有一次回调落空。
    m_audio.shutdown();
    m_audioSrcs.clear();

    // E04 / E05 / E17：布局 + 工作区 + 偏好落盘（自动化时各自跳过）
    saveEditorLayoutToDisk();
    if (m_console) savePrefsToDisk();
    // E03：渲染设置落盘（只在用户真的改过参数时；自动化一律跳过）
    if (m_renderSettingsDirty > 0) saveRenderSettings(false);
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
// ---------------------------------------------------------------- 音频（G14-7/8）

// 编辑器侧的音频每帧。三件事，顺序固定：
//   ① 取走试听请求（Inspector 的 ▶ / Content 双击）→ playOneShot
//   ② **只在 Play 态**同步场景里的 AudioSourceComponent（编辑态不该有声音）
//   ③ 推进 AudioSystem（预读 / ducking 事件 / AUDIO-STATS）
//
// ⚠ `m_audio` 在编辑态与 Play 态是**同一个实例**：Play/Stop 之间不需要
//   重建设备（重建会有一声爆音，而且 Stop 之后设备可能已经被别人占了）。
//   Stop 的时候用 `stopAll()` 把 Play 留下的声音淡出掉。
void EditorApp::syncEditorAudio(float dt) {
    // ⓪ 停止试听（Content 面板"再双击同一个文件"）
    if (m_ctx->takeAudioStop()) {
        m_audio.stopAll();
        VK_LOG_INFO("AUDIO-PREVIEW-STOP");
    }

    // ① 试听：`while` 而不是 `if` —— 同一个面板在同一帧里可能被点多次
    //    （自动化的注入命令会连着发好几条），漏掉一条就是"点了没反应"。
    {
        std::string clip;
        float vol = 1.0f;
        while (m_ctx->takeAudioPreview(clip, vol)) {
            if (clip.empty()) continue;
            const bool ok = m_audio.playOneShot(clip, audio::BusId::SFX,
                                                nullptr, vol);
            VK_LOG_INFO("AUDIO-PREVIEW clip=%s vol=%.2f ok=%d", clip.c_str(),
                        static_cast<double>(vol), ok ? 1 : 0);
        }
    }

    // ② Play 态的声源同步
    if (m_ctx->isPlaying()) {
        scene::Scene& sc = m_ctx->runtimeScene();
        ecs::World& w = sc.world();

        // listener = Play 用的那台相机（Play 期间编辑器不驱动它）
        const glm::vec3 lp = sc.camera().position();
        const glm::vec3 fw = sc.camera().forwardAxis();
        bool fromEntity = false;
        glm::vec3 ep = lp;
        w.each<ecs::AudioListenerComponent>(
            [&](ecs::Entity ent, const ecs::AudioListenerComponent& c) {
                if (fromEntity || !c.enabled) return;
                ep = glm::vec3(sc.worldMatrix(ent)[3]);
                fromEntity = true;
            });
        const float a[3] = {ep.x, ep.y, ep.z};
        const float b[3] = {fw.x, fw.y, fw.z};
        const float u[3] = {0.0f, 0.0f, 1.0f};   // Z-up
        m_audio.setListener(a, b, u);

        m_audioSrcs.clear();
        w.each<ecs::AudioSourceComponent>(
            [&](ecs::Entity ent, const ecs::AudioSourceComponent& as) {
                if (as.clip.empty()) return;
                audio::SourceUpdate u2;
                u2.id = (static_cast<std::uint64_t>(ent.generation) << 32) |
                        ent.id;
                u2.clip = as.clip;
                u2.bus = (as.bus >= 1 && as.bus <= 3)
                             ? as.bus
                             : static_cast<int>(audio::BusId::SFX);
                u2.loop = as.loop;
                u2.spatial = as.spatial;
                u2.playing = as.playOnAwake;
                u2.volume = as.volume;
                u2.pitch = as.pitch;
                u2.minDist = as.minDist;
                u2.maxDist = as.maxDist;
                u2.rolloff = as.rolloff;
                if (as.spatial) {
                    const glm::vec3 wp = glm::vec3(sc.worldMatrix(ent)[3]);
                    u2.pos[0] = wp.x;
                    u2.pos[1] = wp.y;
                    u2.pos[2] = wp.z;
                }
                m_audioSrcs.push_back(u2);
            });
        m_audio.applySources(m_audioSrcs.data(), m_audioSrcs.size());
    } else if (!m_audioSrcs.empty()) {
        // 从 Play 退出的**那一帧**：把 Play 留下的声音淡出，并且只做一次。
        // ⚠ 不清 `m_audioSrcs` 的话下一帧会再 stopAll 一次 —— 那会把用户
        //   正在试听的那一声也停掉。
        m_audio.stopAll();
        m_audioSrcs.clear();
    }

    m_audio.update(dt);
}

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

// 场景落盘。抽成函数是因为它现在有**两个**入口：Ctrl+S 和 Content 工具栏
// 的 "Save All"。两处各写一遍必然漂移（改了一处忘了另一处）。
void EditorApp::saveSceneNow() {
    const std::string abs = assets::resolveAssetPath(m_ctx->scenePath());
    const scene::SceneIoResult r = scene::saveScene(m_ctx->editorScene(), abs);
    if (r.ok) {
        m_ctx->dirty() = false;
        m_ctx->notify(TR("Saved ") + m_ctx->scenePath());
        // 落到日志里：自动化脚本靠它确认"真的存了、存到哪了"
        VK_LOG_INFO("Scene saved: '%s'", abs.c_str());
    } else {
        m_ctx->notify(TR("Save failed: ") + r.error);
        VK_LOG_WARN("Scene save failed for '%s': %s", abs.c_str(),
                    r.error.c_str());
    }
}

// ---------------------------------------------------------------- 脚本执行

// 唯一的脚本 tick 内核：Run（运行态副本）与动画（编辑场景）共用。
// ⚠ 这里**没有循环**了 —— 循环在 `ScriptRuntime::tick` 里（一份实现，
//   编辑器与 Game.exe 共用）。本函数只负责"挑场景 + 挑时间源 + 打汇总"。
void EditorApp::tickScripts(scene::Scene& sc, double simTime, float dt,
                            const char* tag, uint64_t& summaryFrame) {
    if (!m_scriptRuntime) return;
    m_scriptRuntime->tick(sc, simTime, dt, time().frameCount());

    const auto& st = m_scriptRuntime->lastStats();
    // 第一帧打一条汇总：冒烟测试据此确认"场景 + 脚本执行"链路真的通了
    // （只看"没有 WARN"证明力不够）
    if (++summaryFrame == 1) {
        const double t = simTime < 0.0
                             ? static_cast<double>(m_ctx->playTime())
                             : simTime;
        VK_LOG_INFO("%s scripts: %d executed, %d failed (t=%.3fs)", tag,
                    static_cast<int>(st.executed),
                    static_cast<int>(st.failed), t);
    }
}

void EditorApp::runRuntimeScripts(float dt) {
    // ⚠ `kSelfTimed`：仿真态由**实例自己**累加本地时间（修 P3）。
    //   以前传 `playTime()` 再乘 timeScale，倍速从 1.0 改到 0.5 的瞬间
    //   脚本看到的 `time` 会倒退一半 —— 子弹时间一上来就踩。
    tickScripts(m_ctx->runtimeScene(), vks::ScriptRuntime::kSelfTimed, dt,
                "Runtime", m_runtimeFrames);
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
// 编辑器 Play 态的脚下地面高度。
//
// 语义与运行时**必须是同一份**：运行时在 `PlayerController::attach` 里用
// `physics::heightAt(*world->heightfield(), x, y)` 校正出生点，然后每帧由
// `stepCharacter` 的 ⑥ 用同一条竖直射线贴地。编辑器这边没有物理世界，
// 但只要 heightfield 是同一块资产、`heightAt` 是同一个函数，"站多高"
// 就一致。
//
// ⚠ 没有地形时**返回 0 而不是"无穷低"**：编辑器视口里那块参考地面就在
//   z=0（栅格也在那），把人放在 -∞ 会让他一路掉出屏幕。
//
// ⚠ 贴地高度取**足迹内最高点**（2026-10-23）：只取中心一个点时，斜坡上
//   "前脚"所在的半边地表比中心高 → 模型的脚插进坡面（用户报的"脚陷进
//   地形"）。对足迹圆盘（中心 + ±半径的前后左右）取 max，人就站在足迹的
//   最高接触点上 —— 与"胶囊搁在地形棱上"是同一语义。
float EditorApp::editorGroundHeight(float x, float y, float radius) const {
    if (m_physics) {
        if (const physics::Heightfield* hf = m_physics->heightfield()) {
            if (hf->valid()) {
                const float center = physics::heightAt(*hf, x, y);
                const float r = glm::max(radius, 0.0f);
                if (r <= 0.0f) return center;
                return glm::max(
                    glm::max(physics::heightAt(*hf, x + r, y),
                             physics::heightAt(*hf, x - r, y)),
                    glm::max(physics::heightAt(*hf, x, y + r),
                             glm::max(physics::heightAt(*hf, x, y - r),
                                      center)));
            }
        }
    }
    return 0.0f;
}

void EditorApp::runPlayers(float dt) {
    scene::Scene& sc = m_ctx->runtimeScene();
    ecs::World& w = sc.world();

    std::vector<ecs::Entity> players;
    w.each<ecs::PlayerComponent>(
        [&](ecs::Entity e, ecs::PlayerComponent&) { players.push_back(e); });
    if (players.empty()) return;

    const ImGuiIO& io = ImGui::GetIO();
    const bool keysActive = !io.WantTextInput;
    // ---- 操作态（P8，2026-10-23）----
    //
    // 用户要求："游戏启动运行后，必须要鼠标左键在 3D 视口中点击后才能开始
    // 正常和游戏画面内的角色开始交互操作。"
    //
    // 所以键鼠**不是**"进了 Play 就生效"，而是"投了币才生效"。未投币时：
    //   · 视角不跟鼠标转（否则鼠标一划镜头就飞，玩家还以为游戏坏了）
    //   · WASD / 跳跃 / 冲刺全部不响应，角色原地站住（动画照常呼吸）
    //   · 但**重力与贴地照跑** —— 冻掉竖直积分的话，人在空中按 Esc 就会
    //     悬在天上，看起来像卡死
    const bool active = m_ctx->playMouseCaptured();

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
        //
        // ⚠ 2026-10-23（P8）：捕获态下改用 `Input::mouseDeltaRaw`，两条原因：
        //   ① 捕获态里 ImGui 的鼠标被关掉了（NoMouse），io.MouseDelta 恒 0；
        //   ② 未捕获时也不该转 —— 但那时 ImGui 的鼠标是"看得见的光标"，
        //      拿它转视角等于"鼠标划过视口镜头就跟着乱转"，正是本次要修的病。
        // 原来的 `mouseInViewport` 判定保留在**未捕获**分支里：光标在视口内
        // 时仍允许用之前的拖拽习惯微调视线… 不，这里刻意不做——两态就是两态。
        if (active) {
            double rawDx = 0.0, rawDy = 0.0;
            core::Input::mouseDeltaRaw(&rawDx, &rawDy);
            // GLFW 的虚拟光标在窗口内 wrap：跨边界那一帧会给出 ±整窗宽 的
            // 假位移。丢掉它（不丢会"轻轻一划镜头转半圈"，最难复现）。
            const float dx = std::fabs(rawDx) > 400.0 ? 0.0f
                                                      : static_cast<float>(rawDx);
            const float dy = std::fabs(rawDy) > 400.0 ? 0.0f
                                                      : static_cast<float>(rawDy);
            pc->yaw += dx * pc->lookSpeed;
            pc->pitch -= dy * pc->lookSpeed;
        }
        // 俯仰范围与运行时**同一口径**（见 scene/LookLimits.h 的单一来源）：
        // pc->pitch 是**视线仰角**（正 = 抬头），所以"仰 55° / 俯 70°"在这里是
        // [+kLookUpRad, -kLookDownRad]，与运行时那边的 m_pitch 反号。老值
        // ±1.45（±83°）能让人把镜头转到"脚底板下面往上拍"，现实里没人这么
        // 看东西 —— 而且那时相机早就沉到地面以下了。
        pc->pitch = glm::clamp(pc->pitch, -scene::kLookDownRad,
                               scene::kLookUpRad);

        // ---- WASD（(forward, right) 键位组合）----
        glm::vec2 wish(0.0f);
        if (keysActive && active) {
            if (ImGui::IsKeyDown(ImGuiKey_W)) wish.x += 1.0f;
            if (ImGui::IsKeyDown(ImGuiKey_S)) wish.x -= 1.0f;
            if (ImGui::IsKeyDown(ImGuiKey_D)) wish.y += 1.0f;
            if (ImGui::IsKeyDown(ImGuiKey_A)) wish.y -= 1.0f;
        }
        const bool moving = (wish.x != 0.0f || wish.y != 0.0f);
        // ---- 冲刺：**鼠标右键长按**（2026-10-22）----
        // 与运行时同一套默认键位（`InputMap::makeDefault` 把 Sprint 绑在
        // 右键上）。Play 态里视口导航是关的（见 ViewportPanel 的
        // `if (m_ctx.isEditing()) handleNavigation(...)`），所以右键在游戏
        // 里是空闲的，不会"一边跑一边转镜头"。
        const bool sprinting =
            keysActive && active && core::Input::mouseDown(1);   // GLFW 1 = 右键
        const float speed = pc->moveSpeed * (sprinting ? std::max(pc->sprintMultiplier, 1.0f) : 1.0f);
        // 本帧实际位移的起点。走动之后用它算**真实水平速度**喂动画状态机
        // —— 用输入轴的话，撞墙时输入满档但人没动，腿会在原地跑。
        const glm::vec3 posBefore = tc->position;

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

        // ---- 竖直：跳跃 + 重力 + 贴地（2026-10-22）----
        //
        // 运行时那条链由 `physics::stepCharacter` 积分（真实胶囊扫掠、台阶、
        // 斜坡、地形高度场）。编辑器 Play **没有** PlayerController（那是
        // runtime 的概念），所以这里手写同一条抛物线 —— 但常量必须**同源**：
        // 重力取 `physics::kGravity`，初速优先取实体上
        // `CharacterControllerComponent::jumpSpeed`，于是"编辑器里跳多高"
        // 与"出包后跳多高"不可能漂开。
        //
        // 为什么不做成"编辑器里也跑 stepCharacter"：那需要给 Play 态建一个
        // 完整的物理世界 + 运动学代理，而编辑器 Play 的移动本来就是直接改
        // 变换（不走物理）——半条链接上去只会得到"跳跃被物理推、走路不被
        // 推"的混合手感，比没有更糟。
        // 足迹半径取胶囊半径（没有碰撞体组件就用出厂值）。
        float footR = 0.35f;
        if (const auto* cc = w.get<ecs::CharacterControllerComponent>(e))
            footR = cc->radius;
        const float groundZ = editorGroundHeight(tc->position.x, tc->position.y, footR);
        if (!pc->started) {
            pc->velocityZ = 0.0f;
            pc->onGround = true;
            if (tc->position.z < groundZ) tc->position.z = groundZ;
        }
        if (keysActive && active && pc->onGround &&
            ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            float v0 = pc->jumpSpeed;
            if (const auto* cc = w.get<ecs::CharacterControllerComponent>(e)) {
                if (cc->jumpSpeed > 0.0f) v0 = cc->jumpSpeed;
            }
            pc->velocityZ = v0;
            pc->onGround = false;
            // 状态机的 `Jump` trigger（A2b）。放在这里而不是"检测 onGround
            // 变 false"：走下台阶也是 onGround 变 false，那会把踏空当起跳。
            if (auto* anJump = w.get<ecs::AnimatorComponent>(e))
                anJump->triggerParams["Jump"] = true;
            // 锚点带上理论高度：0.70 m 是人能跳的高度，1.3 m 是"游戏里好看
            // 但不真实"的高度。用户明确要求"符合实际"，所以这个数要能查。
            VK_LOG_INFO("EDIT-JUMP v0=%.2f expectH=%.2fm airtime=%.2fs",
                        static_cast<double>(v0),
                        static_cast<double>(v0 * v0 / (2.0f * 9.81f)),
                        static_cast<double>(2.0f * v0 / 9.81f));
        }
        if (!pc->onGround) {
            float gScale = 1.0f;
            if (const auto* cc = w.get<ecs::CharacterControllerComponent>(e))
                gScale = cc->gravityScale;
            pc->velocityZ += physics::kGravity * gScale * dt;
            tc->position.z += pc->velocityZ * dt;
            if (tc->position.z <= groundZ) {
                tc->position.z = groundZ;   // 落地：脚踩在地表上
                pc->velocityZ = 0.0f;
                pc->onGround = true;
            }
        } else if (tc->position.z < groundZ) {
            // 走上坡：抬起来（**不允许脚底沉到地表以下**）
            tc->position.z = groundZ;
        } else if (tc->position.z > groundZ) {
            // 走下坡：贴着地表走下去，**别跳着下去**（2026-10-23）。
            //
            // 旧实现：比新地表高 0.02 m 就直接交给重力（onGround=false）。
            // 在起伏地形上下坡时，每帧的水平步长会让脚"掉出"贴地窗口
            // 一两帧 → onGround 反复翻 false → 动画状态机 walk↔jump 横跳
            // （restartOnSwitch 每次从头迈步）→ 用户报的"走路一卡一卡"。
            //
            // 现在按"一步最多能跨下的落差"来分：落差 ≤ 本帧水平步长 ×
            // tan(slopeLimit) + 0.02 → 跟着地表走下去（保持贴地）；
            // 真正的坡沿 / 陡坎（落差远超一步）仍然交给重力掉下去。
            const float drop = tc->position.z - groundZ;
            float tanSlope = 1.0356f;   // tan(46°)，与 slopeLimitDeg 出厂值同源
            if (const auto* cc = w.get<ecs::CharacterControllerComponent>(e))
                tanSlope = std::tan(glm::radians(
                    glm::clamp(cc->slopeLimitDeg, 1.0f, 89.0f)));
            const float maxDown =
                (moving && keysActive ? speed * dt : 0.0f) * tanSlope + 0.02f;
            if (drop <= maxDown) {
                tc->position.z = groundZ;   // 跟着地形下坡（保持贴地）
            } else {
                pc->onGround = false;       // 坡沿 / 陡坎：交给重力
            }
        } else {
            tc->position.z = groundZ;      // 微差：贴住
        }

        // ---- 第三人称：转身 + walk 摆腿 ----
        if (!firstPerson) {
            if (!pc->started) {
                // 出生朝向：**背对相机**。
                //
                // 第三人称进 Play 的第一眼应该看到角色的后脑勺，而不是两个人
                // 脸对脸。本工程的约定是 facing = (cos b, -sin b)（见下面
                // 移动分支的推导），而"远离相机"= fwdH = (-cos yaw, sin yaw)
                // → b = yaw + π。
                //
                // 老代码是 `bodyYaw = yaw`：出生时角色正对着镜头，一按 W
                // 才被下面的 moving 分支掰过去。用户报的"往前走时脸朝反方向"
                // 是另一回事（模型朝向补偿差 180°，见 assets/ModelOrientation），
                // 但两者叠在一起时"哪一帧脸朝哪边"就彻底说不清了 ——
                // 所以出生姿势也必须钉死。
                pc->bodyYaw = pc->yaw + 3.14159265f;
                tc->rotation.z = -pc->bodyYaw;
            }
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

        // ---- 四态动画（P4）：移动就迈步、停下就待机 ----
        //
        // 与运行时共用 `animation/Locomotion`（见该文件头）。三条约定：
        //   · 速度 = 本帧**实际**位移 / dt（撞墙 = 0 → 播 idle，不是"原地跑"）；
        //   · 贴地 = `pc->onGround`（2026-10-22 起编辑器 Play 也有跳跃了，
        //     之前这里恒传 true，jump 段在编辑器里永远走不到）；
        //   · 锚点前缀 `EDIT-ANIM`，与运行时的 `GAME-ANIM` 分开
        //     （两条链同时在跑时日志不会互相冒充）。
        if (ecs::SkinnedMeshComponent* sk = w.get<ecs::SkinnedMeshComponent>(e)) {
            if (sk->model && sk->model->skeleton) {
                animation::LocomotionConfig cfg;
                cfg.runSpeed = 0.0f;   // 自动：moveSpeed × 1.15
                // 按键映射（角色面板里绑的键）。
                // ⚠ 判定用 `ui_keys::pressed`（ImGui 原始键状态），
                //   不能用 `core::Input` —— 后者被 `io.WantCaptureKeyboard`
                //   门控，Play 时只要有 ImGui 窗口拿着键盘焦点就恒为 false。
                if (const auto* km = w.get<ecs::AnimKeyMapComponent>(e)) {
                    int key = 0;
                    if (const std::string* clip =
                            animation::pollKeyTrigger(*km, &ui_keys::pressed,
                                                      &key)) {
                        animation::playTriggerClip(
                            m_playerLoco[e.id], *sk,
                            w.get<ecs::AnimatorComponent>(e),
                            *sk->model->skeleton, *clip, km->holdSeconds,
                            "EDIT");
                        VK_LOG_INFO("EDIT-ANIM-KEY-TRIGGER clip=%s key=%d",
                                    clip->c_str(), key);
                    }
                }
                const float hs =
                    dt > 0.0f
                        ? glm::length(glm::vec2(tc->position.x - posBefore.x,
                                                tc->position.y - posBefore.y)) /
                              dt
                        : 0.0f;
                // 状态机（A2b）：配了控制器 → 喂参数 + 让位给状态机。
                // 参数名约定与运行时**完全一致**（Speed / Grounded / Jump），
                // 否则同一份 `.anim.json` 在编辑器里和出包后表现不同 ——
                // 那是最难查的一类（"编辑器里好好的，打包就不对"）。
                //
                // ⚠ 用 if/else 而不是 `continue`：这一段后面还有相机更新，
                //   `continue` 会把相机一起跳过（画面卡住但动画正常）。
                auto* an2 = w.get<ecs::AnimatorComponent>(e);
                if (an2 && !an2->controllerPath.empty()) {
                    an2->floatParams["Speed"] = hs;
                    an2->boolParams["Grounded"] = pc->onGround;
                } else {
                    animation::updateLocomotion(
                        m_playerLoco[e.id], *sk, an2, *sk->model->skeleton, hs,
                        pc->onGround, pc->moveSpeed, dt, cfg, "EDIT");
                }
            }
        }

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
            // ---- 第三人称：弹簧臂（2026-10-24 第二轮补上）----
            //
            // 以前这里直接把相机放在 target + dir*distance 上，**一点碰撞
            // 检测都没有** —— 贴墙站就整个镜头钻进墙体内部，视野里全是
            // 墙的背面（用户第六轮第 3 条）。运行时那条链（Game.exe）一直
            // 有弹簧臂，编辑器 Play 没有，两边手感还不一致。
            //
            // 现在共用 `scene::resolveSpringArm`（UE SpringArmComponent 的
            // 球扫掠 + 穿透优先），射线来源换成**场景 BVH + 地形高度场**：
            // 编辑器 Play 没有物理世界，但有空间索引。
            const float focusZ = 1.2f * std::max(1.0f, pc->eyeHeight / 1.7f);
            const glm::vec3 focus =
                tc->position + glm::vec3(0.0f, 0.0f, focusZ);

            // 从 focus 指向相机的方向。Camera 的 angleDir 是"相机方位"，
            // 与 pc->pitch（视线仰角）反号 —— 见下面 setPitch 的注释。
            const float cpc = std::cos(pc->pitch);
            const glm::vec3 back(std::cos(pc->yaw) * cpc,
                                 -std::sin(pc->yaw) * cpc,
                                 -std::sin(pc->pitch));

            // 角色自己的网格不能挡住相机：命中实体若在**本玩家**的子树里
            // 就直接不算（沿父链往上找，深度很小）。
            const ecs::EntityId selfId = e.id;
            const auto& bvh = sc.spatialIndex();
            const scene::SpringArmCast cast =
                [&](const glm::vec3& o, const glm::vec3& d,
                    float maxDist) -> float {
                float best = maxDist;
                const scene::BvhRay ray{o, d};
                const scene::BvhHit hit = bvh.raycastFiltered(
                    ray,
                    [&](ecs::Entity cand) {
                        ecs::EntityId cur = cand.id;
                        for (int guard = 0; guard < 64 && cur != 0xFFFFFFFFu;
                             ++guard) {
                            if (cur == selfId) return false;
                            const auto* hc =
                                w.get<ecs::HierarchyComponent>(ecs::Entity{cur});
                            if (!hc) break;
                            cur = hc->parent;
                        }
                        return true;
                    },
                    maxDist);
                if (hit.hit && hit.distance < best) best = hit.distance;
                // 地形：heightfield 不在 BVH 里（它是高度场，不是网格实体）
                constexpr int kSteps = 20;
                for (int i = 1; i <= kSteps; ++i) {
                    const float t = best * static_cast<float>(i) / kSteps;
                    const glm::vec3 p = o + d * t;
                    if (p.z < editorGroundHeight(p.x, p.y, 0.0f)) {
                        best = t;
                        break;
                    }
                }
                return best;
            };

            constexpr float kCameraRadius = 0.25f;
            constexpr float kArmMin = 0.60f;
            const scene::SpringArmResult arm = scene::resolveSpringArm(
                cast, focus, back, pc->camDistance, kArmMin, kCameraRadius);

            // 臂长平滑：缩短立刻、伸长限速（与运行时同一条规则）。
            // 首帧 m_playArmLen 还是 0：直接给全长，否则镜头会从角色脸上
            // 慢慢"拉出去"，进 Play 的第一秒很难看。
            if (m_playArmLen <= 1.0e-4f) m_playArmLen = pc->camDistance;
            if (arm.distance < m_playArmLen) {
                m_playArmLen = arm.distance;
            } else {
                m_playArmLen = std::min(arm.distance, m_playArmLen + 6.0f * dt);
            }
            float effPitch = -pc->pitch;
            glm::vec3 camPos = focus + back * m_playArmLen;
            // 地面钳制会改写"相机到枢轴的真实距离"，那是**输出**，不能回写
            // 进 m_playArmLen —— 后者是臂长平滑的状态量，混用会让平滑在
            // 贴地时来回翻（相机位置本身仍是确定的，只是状态被污染了）。
            float camDistanceNow = m_playArmLen;
            // 相机不许钻进地面：顶到地表之上，再按新高度反解俯仰 ——
            // 镜头仍指着角色，不会突然看天。
            //
            // 反解走 `scene::clampCameraAboveGround`（与运行时共用一份）：
            // 第一版这里是自己写的 `asin((groundHere - focus.z)/臂长)`，
            // 分母用错了（钳制后相机到枢轴的真实距离变长了），而且符号约定
            // 与运行时那份**相反**却没人发现。共用一份之后两边不可能漂。
            const float groundHere =
                editorGroundHeight(camPos.x, camPos.y, 0.0f) + 0.25f;
            if (m_playArmLen > 1.0e-3f) {
                scene::clampCameraAboveGround(focus, groundHere, &camPos,
                                              &camDistanceNow, &effPitch);
            }

            cam.setTarget(focus);
            cam.setDistance(camDistanceNow);
            cam.setYaw(pc->yaw);
            // Camera 的 pitch 是"相机在目标上方"的俯视角，与我们的视线
            // 仰角反号 —— 不取负的话"鼠标往上"会变成"相机升得更高、看得
            // 更低"（这正是 2026-10-11 修掉的那个上下颠倒）。
            cam.setPitch(effPitch);
        }
    }

    // 自动化探针：每 30 帧报一次玩家状态（pos / yaw / pitch），
    // 验证脚本据此断言"按 D 玩家往右挪了 / 动鼠标视野转了"。
    // 末尾带 miv = 鼠标是否落在视口矩形内；cap = 是否已接管操作（P8）。
    //
    // ⚠ 2026-10-23 起**门控是 cap 不是 miv**：进了 Play 也要先用左键点一下
    //   视口才开始转视角。注入脚本时如果发现 yaw 不动，先看 cap 是几 ——
    //   cap=0 就补一次"点视口"，miv=0 才是"鼠标压根不在视口里"。
    //   （捕获态下 ImGui 的鼠标被关掉，miv 会读成 0，这属正常。）
    ++m_playerLogTick;
    if (m_playerLogTick >= 30) {
        m_playerLogTick = 0;
        for (ecs::Entity e : players) {
            const auto* pc = w.get<ecs::PlayerComponent>(e);
            const auto* tc = w.get<ecs::TransformComponent>(e);
            if (!pc || !tc) continue;
            const std::string* nm = w.name(e);
            // arm / cam = 本帧的**弹簧臂长**与**相机世界位置**。
            // 2026-10-24 加的：用户报"贴墙时镜头钻进墙体内部"，而这件的
            // 唯一证据就是"臂长有没有被压下来、相机落在墙的哪一侧" ——
            // 光看画面截图分不清"镜头贴在墙前"和"镜头在墙里往外看"。
            const glm::vec3 camPos = sc.camera().position();
            VK_LOG_INFO("PLAYER-STATE '%s' mode=%s pos=(%.3f,%.3f,%.3f) "
                        "yaw=%.3f pitch=%.3f arm=%.3f "
                        "cam=(%.3f,%.3f,%.3f) miv=%d cap=%d",
                        nm ? nm->c_str() : "?",
                        pc->mode == ecs::PlayerMode::ThirdPerson ? "third"
                                                                 : "first",
                        static_cast<double>(tc->position.x),
                        static_cast<double>(tc->position.y),
                        static_cast<double>(tc->position.z),
                        static_cast<double>(pc->yaw),
                        static_cast<double>(pc->pitch),
                        static_cast<double>(m_playArmLen),
                        static_cast<double>(camPos.x),
                        static_cast<double>(camPos.y),
                        static_cast<double>(camPos.z),
                        m_ctx->mouseInViewport(
                            glm::vec2(io.MousePos.x, io.MousePos.y)) ? 1 : 0,
                        active ? 1 : 0);
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

// ---------------------------------------------------------------- R7-1 物理
//
// 与碰撞求解（runCollision）的分工：
//   · runCollision —— **没有动力学**的位置分离。只处理"有碰撞体、但没有
//     刚体"的实体（Collider::physicsDriven 把刚体摘出去了），外加相机推出。
//   · 本节的物理世界 —— 有 RigidBodyComponent 的实体，走完整的刚体动力学。
// 两者同时开着也不会互相打架，因为管辖范围不重叠。

// 把运行态场景里的 RigidBodyComponent 变成 physics::PhysicsWorld 里的刚体。
//
// 只在进入 Play 的第一帧调用一次。之后每帧只步进 + 写回变换 —— 参数是
// "初始条件"，运行中改它不会追溯生效（要重开 Play）。这与"编辑态改不了
// 运行态"的整体设计一致：Play 中途改 Inspector 只影响下次 Play。
void EditorApp::buildPhysicsWorld() {
    destroyPhysicsWorld();

    // 翻译逻辑本身在 engine/physics/ScenePhysics.cpp —— 与 Game.exe **共用
    // 同一份**（G01/G03 把它从编辑器提出去，就是为了不让"编辑器里调好的手感
    // 出包后不一样"，见 ship_roadmap §15 风险 #10）。
    physics::SceneBuildOptions opts;
    if (const char* e = std::getenv("MYVK_PHYS")) {
        opts.enabledOverride = (std::atoi(e) != 0) ? 1 : 0;
    }

    physics::SceneBinding binding =
        physics::buildSceneWorld(m_ctx->runtimeScene(), opts);
    if (!binding.enabled) return;

    m_physics = std::move(binding.world);
    m_physicsEntityToBody = std::move(binding.entityToBody);

    m_physicsFrames = 0;
    m_physicsLogBudget = 60;
    // MYVK_PHYS_DUMP=N → 每 N 帧一行/每体（N=1 就是逐帧，配 MYVK_FRAMES
    // 截断用）；不设 / 非法值 → 关。
    m_physicsDumpEvery = 0;
    if (const char* dump = std::getenv("MYVK_PHYS_DUMP")) {
        const int n = std::atoi(dump);
        if (n > 0) m_physicsDumpEvery = n;
    }

    const scene::PhysicsSettings& ps = m_ctx->runtimeScene().physicsSettings();
    VK_LOG_INFO("physics: world built — %d body(ies) (ground=%d triggers=%d "
                "materials=%d) from %d "
                "rigid entities, gravity=(%.2f,%.2f,%.2f) dt=%.5f",
                binding.bodies, binding.ground ? 1 : 0, binding.triggers,
                binding.materials,
                binding.rigidEntities,
                static_cast<double>(ps.gravity.x),
                static_cast<double>(ps.gravity.y),
                static_cast<double>(ps.gravity.z),
                static_cast<double>(ps.fixedTimeStep));
}

void EditorApp::stepPhysics(float dt) {
    if (!m_physics) return;
    scene::Scene& sc = m_ctx->runtimeScene();
    ecs::World& w = sc.world();

    ++m_physicsFrames;

    // ---- 1) 运动学体：实体 → 刚体（步进前）----
    physics::syncKinematicFromScene(sc, *m_physics, m_physicsEntityToBody, dt);

    // ---- 2) 步进 ----
    m_physics->step(dt);

    // ---- 3) 写回实体变换（只写动态体）----
    physics::writeBackToScene(sc, *m_physics, m_physicsEntityToBody);

    // ---- 4) 统计日志（前几十帧给全，之后只报一次心跳）----
    const physics::PhysicsStats& st = m_physics->stats();
    // 面板的只读快照（Inspector 的 Physics 页读它，见 PhysicsRuntimeInfo）
    editor::PhysicsRuntimeInfo info;
    info.active = true;
    info.bodies = st.bodies;
    info.dynamic = st.dynamic;
    info.sleeping = st.asleep;
    info.manifolds = st.manifolds;
    info.contactPoints = st.contactPoints;
    info.broadphasePairs = st.broadphasePairs;
    info.joints = st.joints;
    info.maxPenetration = st.maxPenetration;
    info.maxSpeed = st.maxSpeed;
    info.steps = static_cast<unsigned long long>(st.steps);
    info.simTime = st.simTime;
    m_ctx->setPhysicsRuntime(info);

    // 前 60 帧逐帧给全（自动化断言用），之后每 120 帧一条心跳 ——
    // 长时间 Play 不该把日志刷爆，但"物理还在走"这件事要能看见。
    const bool verbose = m_physicsLogBudget > 0;
    if (verbose) --m_physicsLogBudget;
    if (verbose || (m_physicsFrames % 120u) == 0u) {
        VK_LOG_INFO("physics: step=%llu bodies=%d dyn=%d asleep=%d "
                    "manifolds=%d contacts=%d pairs=%d maxPen=%.4f",
                    static_cast<unsigned long long>(st.steps), st.bodies,
                    st.dynamic, st.asleep, st.manifolds, st.contactPoints,
                    st.broadphasePairs,
                    static_cast<double>(st.maxPenetration));
    }

    // ---- 5) 调试转储（MYVK_PHYS_DUMP=<N>）----
    // 每 N 帧一行/每体。**位置从实体读**（而不是从刚体读）——这样断言的是
    // "写回链路也对了"，而不仅是"求解器算对了"。这两件事会各自坏掉：
    // 求解器对了但写回用错坐标系，画面照样是错的。
    if (m_physicsDumpEvery > 0 &&
        (m_physicsFrames % static_cast<uint64_t>(m_physicsDumpEvery)) == 0u) {
        for (auto& kv : m_physicsEntityToBody) {
            const physics::RigidBody* b = m_physics->body(kv.second);
            if (!b || !b->isDynamic()) continue;
            const ecs::Entity e = w.handle(kv.first);
            const std::string* nm = e.valid() ? w.name(e) : nullptr;
            const auto* tc = e.valid() ? w.get<ecs::TransformComponent>(e) : nullptr;
            const glm::vec3 p = tc ? tc->position : b->position;
            VK_LOG_INFO("PHYS-BODY f=%llu name='%s' pos=(%.5f,%.5f,%.5f) "
                        "vel=(%.5f,%.5f,%.5f) awake=%d",
                        static_cast<unsigned long long>(m_physicsFrames),
                        nm ? nm->c_str() : "?", static_cast<double>(p.x),
                        static_cast<double>(p.y), static_cast<double>(p.z),
                        static_cast<double>(b->linearVelocity.x),
                        static_cast<double>(b->linearVelocity.y),
                        static_cast<double>(b->linearVelocity.z),
                        b->awake ? 1 : 0);
        }
    }
}

void EditorApp::destroyPhysicsWorld() {
    if (!m_physics) return;
    m_physics.reset();
    m_physicsEntityToBody.clear();
    // 面板不该在"世界已经没了"之后继续显示上一帧的步数
    m_ctx->setPhysicsRuntime(editor::PhysicsRuntimeInfo{});
}

// 动画：直接在**编辑场景**上跑脚本（Play 走 / Pause 冻结 / Stop 恢复快照）
void EditorApp::runAnimScripts(float dt) {
    // 动画预览是**预览态**：时间跟随 `animTime()`（能拖、能回放），
    // 所以传真实时间而不是 kSelfTimed。时间轴被拖动时脚本看到的 time
    // 跟着跳变，这是对的。
    tickScripts(m_ctx->editorScene(),
                static_cast<double>(m_ctx->animTime()), dt, "Anim",
                m_animFrames);
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
        vks::ScriptAsset* a = m_scripts->get(p);
        if (a && !a->lastError().ok()) {
            ++failed;
            m_scripts->logLine("[compile] " + p + " > " +
                               a->lastError().toString());
            // ⚠ 同时进日志：面板只在用户打开 Scripts 面板时可见，而
            //   "模板脚本编译失败"是**新建工程第一眼**就该看见的事。
            //   以前只写面板 → 日志里只有一句 `1 with errors`，查不下去。
            VK_LOG_WARN("Editor: script compile error '%s' line %d: %s",
                        p.c_str(), a->lastError().line,
                        a->lastError().message.c_str());
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

    // ============================================================
    // 底部：底栏（Content / 输出日志 / CMD）+ 可停靠的 Content 抽屉
    // ============================================================
    //
    // 2026-10-17 布局改造（用户要求）：
    //   · Content 不再是常驻分栏面板（SplitLayout 里它恒不 live）；
    //   · 底栏是常驻的一行通栏，左下三个带图标的按钮，右侧是资源统计；
    //   · 点"内容"→ Content 抽屉从底栏往上弹；
    //   · 抽屉右上角 "Dock in layout" 勾上 = 真的占布局（视口/层级随之变矮），
    //     不勾 = 浮层（盖在视口上，点面板外任意处就关）。
    //
    // 两次 apply 的原因：抽屉高度要按"扣掉底栏之后还剩多少主体"来夹
    // （不能超过 bodyH - kMinViewport，否则视口会被压没）。第一次 apply
    // 只为拿到这个 bodyH；夹完再 apply 一次把停靠高度真正扣进去。
    LayoutRects& L = m_ctx->layout();
    const bool drawerOpen = m_ctx->panels().content;

    m_split->setBottomInset(kBottomBarH);
    m_split->apply(L);
    const float bodyTop = L.toolbar.y + L.toolbar.h;
    // 主体可用的高度 = 到"底栏上沿"为止（不要再减 statusH：底栏自己就在
    // 状态栏上方，apply 已经把它让出来了）。
    const float barTop = io.DisplaySize.y - kStatusH - kBottomBarH;
    const float bodyH = std::max(barTop - bodyTop, 120.0f);
    const float maxDrawerH = std::max(bodyH - kMinViewport, kContentDrawerMin);
    m_contentDrawerH =
        std::clamp(m_contentDrawerH, kContentDrawerMin, maxDrawerH);

    const float dockH = (drawerOpen && m_contentDock) ? m_contentDrawerH : 0.0f;
    if (dockH > 0.0f) {
        m_split->setBottomInset(kBottomBarH + dockH);
        m_split->apply(L);
    }

    // 底栏矩形：**永远**贴着状态栏上方、高度恒为 kBottomBarH。
    // （停靠抽屉长在它上面，所以主体底边 = L.bottom.y - dockH，这个由
    //   SplitLayout 的 bottomInset 负责 —— 两边算的必须是同一件事。）
    L.bottom = {0.0f, io.DisplaySize.y - kStatusH - kBottomBarH,
                io.DisplaySize.x, kBottomBarH};

    // 抽屉矩形：横跨整窗、贴着底栏上沿往上长（停靠/浮层用同一个矩形，
    // 区别只在"有没有从布局里扣掉"）。
    if (drawerOpen) {
        L.content = {0.0f, L.bottom.y - m_contentDrawerH, io.DisplaySize.x,
                     m_contentDrawerH};
    } else {
        L.content = {0.0f, 0.0f, 0.0f, 0.0f};
    }

    // 浮层抽屉的形状就是下面这个矩形；至于"要不要挖掉分隔线"以及弹层 /
    // 浮动窗口的遮挡，统一在 collectSplitOccluders() 里收集（它读的正是
    // m_ctx->layout()，所以这里不需要额外上报）。
    // 停靠形态不用挖：bottomInset 已经把分隔条的下端收到抽屉上沿了。

    // transport（视口下方播放条）已停用：UI 被移除，矩形恒为 0×0。
    L.transport = {0.0f, 0.0f, 0.0f, 0.0f};

    // ---- 视口标签条（2026-10-21）----
    // 从视口矩形顶部切 kViewportTabBarH 给标签条，剩下的才是 3D 画面区。
    // **放在最后一步**：前面两次 apply() 都会重写 L.viewport，早切会被盖掉。
    // 切完 L.viewport 就等于"标签条下方的内容区"，于是：
    //   · ViewportPanel 无需改动（它本来就画在 L.viewport 里）；
    //   · 被托管的浮动面板也用这块矩形（drawPanels 里的 pushDockHint）。
    m_tabBarRect = {0.0f, 0.0f, 0.0f, 0.0f};
    if (L.viewport.w > 8.0f && L.viewport.h > kViewportTabBarH + 60.0f) {
        m_tabBarRect = {L.viewport.x, L.viewport.y, L.viewport.w,
                        kViewportTabBarH};
        L.viewport.y += kViewportTabBarH;
        L.viewport.h -= kViewportTabBarH;
    }

    // ---- 布局摘要（只在矩形变化时打一行）----
    // 自动化脚本靠它一次断言整棵分栏树：模式栏在最左、Hierarchy 在右栏
    // 上半、Inspector 在其正下方、标签条紧贴视口顶部。
    // 逐面板的锚点（HIER-* / TK-* / VIEWPORT）只能证明"某块面板在哪"，
    // 证明不了"两块的上下关系"—— 而那正是这次改动的核心。
    {
        static Rect s_last[5] = {};
        const Rect cur[5] = {L.modes, L.viewport, L.hierarchy, L.inspector,
                             m_tabBarRect};
        bool changed = false;
        for (int i = 0; i < 5; ++i) {
            if (std::fabs(cur[i].x - s_last[i].x) > 0.5f ||
                std::fabs(cur[i].y - s_last[i].y) > 0.5f ||
                std::fabs(cur[i].w - s_last[i].w) > 0.5f ||
                std::fabs(cur[i].h - s_last[i].h) > 0.5f) {
                changed = true;
                s_last[i] = cur[i];
            }
        }
        if (changed) {
            VK_LOG_INFO("LAYOUT-RECTS modes=(%.0f,%.0f,%.0f,%.0f) "
                        "hier=(%.0f,%.0f,%.0f,%.0f) "
                        "insp=(%.0f,%.0f,%.0f,%.0f) "
                        "tabs=(%.0f,%.0f,%.0f,%.0f) "
                        "vp=(%.0f,%.0f,%.0f,%.0f)",
                        cur[0].x, cur[0].y, cur[0].w, cur[0].h,
                        cur[2].x, cur[2].y, cur[2].w, cur[2].h,
                        cur[3].x, cur[3].y, cur[3].w, cur[3].h,
                        cur[4].x, cur[4].y, cur[4].w, cur[4].h,
                        cur[1].x, cur[1].y, cur[1].w, cur[1].h);
        }
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
    if (mode < 0 || mode > static_cast<int>(VM::LightComplexity)) return TR("lit");
    // ⚠ 引擎的 viewModeName() 也用于日志（VIEW-MODE init mode=unlit），所以它
    //   不能就地翻；在编辑器这一层包一道：日志仍可 grep 英文，界面走译文。
    return TR(render::Renderer::viewModeName(static_cast<VM>(mode)));
}

void EditorApp::syncViewportLayout() {
    // 写 EditorContext + 设子视口数 + 打 VIEWPORT-LAYOUT 三件事都在
    // Workspace.cpp 里（视口工具条的"1 / 2V / 2H / 4"下拉走同一个函数），
    // 这样两条入口的行为与日志不会分叉。
    //
    // ⚠ 必须写全 `editor::`：本类有一个同名的虚函数
    //   `applyViewportLayout(int)`（EditorHost 那条窄接口），不加限定会被
    //   它挡住，报的是"参数个数不对"，看着像调用写错了。
    editor::applyViewportLayout(*m_ctx, renderer(), m_ctx->viewportLayout());
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
    m_ctx->notify(TR("Workspace reset to factory preset"));
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

// 顶栏那颗"前进一帧"(▶|)。
//
// 语义与 UE 的 Step 一致：**必须有运行态**才能步进（没有运行态时先 Play 出
// 一份副本，立刻暂停），然后手动推进**一个固定步长**。为什么不复用
// onUpdate 里那条链：那边的 dt 是墙钟时间，一次点击推进多少完全看机器快慢，
// 步进就失去了"逐帧调试"的意义。这里写死 1/60，与项目的固定物理步长对齐。
//
// 顺序照抄 onUpdate：脚本 → 玩家 → 物理 → 碰撞。少一环都会让"步进出来的
// 状态"和"跑起来的状态"不一致 —— 那种不一致最难查。
void EditorApp::stepOneFrame() {
    if (!m_ctx->usingRuntime()) {
        m_ctx->play();     // 复制运行态（失败时 play() 内部已经报错并回退）
        if (!m_ctx->usingRuntime()) return;
    }
    if (m_ctx->isPlaying()) m_ctx->pause();  // 步进总是停在暂停态

    constexpr float kStepDt = 1.0f / 60.0f;
    m_ctx->advancePlayTime(kStepDt);
    runRuntimeScripts(kStepDt);
    runPlayers(kStepDt);
    if (!m_physics) buildPhysicsWorld();
    stepPhysics(kStepDt);
    runCollision(kStepDt);

    VK_LOG_INFO("Editor: STEP one frame (playTime=%.3f run=%d)",
                static_cast<double>(m_ctx->playTime()),
                static_cast<int>(m_ctx->usingRuntime()));
}

void EditorApp::reloadShaders() {
    // 热重载平时是"每 15 帧扫一轮 .spv 的 mtime"；这里把节流推到阈值以上
    // → 下一帧立刻扫一遍（等价于手动催一次）。
    renderer().forceShaderReloadScan();
    m_ctx->notify(TR("Shader reload requested (see the log)"));
}

void EditorApp::reloadAssets() {
    // AssetManager 是"按路径惰性加载 + 永不淘汰"的模型（见 assets/
    // AssetManager.h）：没有"整表重扫"这件事可做。真正需要的是**脚本**
    // 重编译，那由 EditorApp::preloadScripts 每帧节流做。
    preloadScripts();
    m_ctx->notify(TR("Assets reloaded (scripts recompiled)"));
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

    // Inspector 的当前页签。存的是 **id 字符串**，所以"往中间插一个页签"
    // 不会让老配置读回来跳到别的页上；认不出来的 id 保持默认（不报错）。
    if (m_inspector) {
        const int tab = InspectorPanel::tabIndexById(s.inspTab.c_str());
        if (tab >= 0) m_inspector->setActiveTab(tab);
    }

    const int ws = workspaceIndexOf(s.workspace.c_str());
    m_ctx->workspace().setCurrent(ws >= 0 ? ws : 0);
    m_ctx->recentScenesRef() = s.recent;

    // 子视口目标按新布局建起来（applyViewportSize 那套由渲染器自己管）
    syncViewportLayout();
    // 读盘摘要打一行：验收脚本靠它区分"读到了存档"和"退回了出厂值"。
    VK_LOG_INFO("LAYOUT-LOAD workspace=%s viewportLayout=%s viewMode=%s "
                "maximized=%s recent=%zu panels=%zu inspTab=%s",
                m_ctx->workspace().currentId(),
                viewportLayoutName(m_ctx->viewportLayout()),
                viewModeLabel(static_cast<int>(renderer().viewMode())),
                s.maximized.empty() ? "(none)" : s.maximized.c_str(),
                s.recent.size(), s.panels.size(), s.inspTab.c_str());
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
    if (m_inspector) s.inspTab = InspectorPanel::tabId(m_inspector->activeTab());
    s.recent = m_ctx->recentScenes();
    const bool ok = saveEditorLayout(s);
    VK_LOG_INFO("LAYOUT-SAVE %s path=%s workspace=%s maximized=%s recent=%zu",
                ok ? "ok" : "SKIPPED", editorLayoutPath().c_str(), s.workspace.c_str(),
                s.maximized.empty() ? "(none)" : s.maximized.c_str(),
                s.recent.size());
}

// ---------------------------------------------------------------- 快捷键

// Play 态鼠标捕获：状态机只有两格，但**谁改谁同步**必须说清楚。
//
//   未捕获 --(左键点视口)--> 捕获    ：ViewportPanel 发请求，这里执行
//   捕获   --(Esc)---------> 未捕获  ：handleShortcuts 里做（和"退出全屏"
//                                      抢同一个键，必须 if/else 分流）
//   捕获   --(暂停 / Stop)--> 未捕获  ：这里兜底（Context 里也会置 false）
//
// 为什么"隐藏鼠标"必须配 `ImGuiConfigFlags_NoMouse`：
//   GLFW_CURSOR_DISABLED 把光标锁在**窗口中心**，而编辑器窗口中心通常正
//   落在 Hierarchy / Inspector 上。ImGui 照旧认为"鼠标在这儿"，于是玩家
//   在游戏里按左键（射击 / 点地移动）会去点那些面板的按钮 —— 而且看不见
//   光标，根本不知道点了什么。关掉 ImGui 的鼠标输入才是真的"鼠标归游戏"。
void EditorApp::updatePlayMouse() {
    // ---- ① 点视口 → 接管 ----
    const bool requested = m_ctx->takePlayMouseCaptureRequest();
    if (requested && m_ctx->isPlaying() && !m_ctx->playMouseCaptured()) {
        m_ctx->setPlayMouseCaptured(true);
    }

    // ---- ② 只有 Playing 能保持捕获（Paused / Stopped 一律交还）----
    if (m_ctx->playMouseCaptured() && !m_ctx->isPlaying()) {
        m_ctx->setPlayMouseCaptured(false);
    }

    // ---- ③ 同步到 GLFW + ImGui ----
    if (!m_ctx->takePlayCursorDirty()) return;
    const bool captured = m_ctx->playMouseCaptured();
    ImGuiIO& io = ImGui::GetIO();
    if (captured) {
        window().setCursorVisible(false);   // DISABLED = 隐藏 + 锁定 + 相对位移
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    } else {
        window().setCursorVisible(true);
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }
    // 自动化锚点（tools/verify_play_mouse_capture.py 的判据就是这个）
    VK_LOG_INFO("EDIT-MOUSE captured=%d viewport=%s", captured ? 1 : 0,
                captured ? "input-live"
                         : "input-frozen (LMB on the 3D viewport to take "
                           "control, Esc releases)");
}

void EditorApp::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    // 正在文本输入（重命名 / 脚本路径）时不要抢键
    if (io.WantTextInput) return;

    // ---- G06 键位捕获：模态，收下**全部**按键 ----
    // 必须排在所有快捷键之前（包括 F11 与命令面板）。玩家按下 Rebind 之后
    // 要把 Jump 绑到 Space，而 Space 是"运行/暂停"、F 是全屏 —— 不收手的话
    // "点一下 Rebind" 会顺带把场景跑起来。捕获本身在 KeyBindingsPage 里读
    // GLFW 原始状态（那一层不受 ImGui 的键盘门控影响）。
    if (m_ctx->inputCaptureActive()) return;

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
                m_ctx->notify(std::string(TR("Workspace: ")) +
                              TR(kWorkspaces[i].title));
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

    // ---- Modeling 模式的网格编辑快捷键（Blender 键位）----
    // **必须排在 handleViewShortcuts 之前**：那个函数认主键盘 1/3/7 当
    // "视图方向"，而建模模式里 1/2/3 是"顶点/边/面"。顺序反过来就永远
    // 切不到顶点模式（1 被当成"看 -Y"）。
    // 内部自带 `editMode == Modeling` 判断与模态处理，返回 true = 已吃掉。
    if (m_modeling && m_modeling->handleKeys()) return;

    if (!io.KeyAlt) m_viewport->handleViewShortcuts();

    // ---- 地形笔刷快捷键 ----
    // 必须排在 `if (io.KeyCtrl)` 之前：那一支会把 Ctrl+Z 交给
    // EditorContext::undo()，也就是**整场景快照**回滚 —— 在刷地形时那是
    // 灾难（一笔撤不掉，反而把所有实体和相机弹回去）。地形的高度图不在
    // 场景里，它有自己的笔划撤销栈（见 LandscapeEditor::undoStroke）。
    if (m_ctx->editMode() == EditMode::Landscape && m_landscape &&
        m_landscape->handleShortcuts()) {
        return;
    }

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
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) saveSceneNow();
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
        // Q = 选择模式（2026-10-17 晚加的第四个工具，见 GizmoMode::Select）。
        // ⚠ Q 同时也是"按住右键下降"（handleNavigation 里的飞行键）—— 上面
        //   的 `!flying` 已经把它们分开了，别删这个条件。
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
            m_ctx->setGizmoMode(GizmoMode::Select);
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
            m_ctx->notify(TR("Layout restored"));
        }
    }

    // 播放 / 暂停（Shift+Space 已归"最大化"，所以这里要把 Shift 排除掉）
    //
    // ⚠ 2026-10-22：**只在编辑态**由 Space 触发。用户要求"角色跳跃是敲击
    //   空格键"，Play 态下 Space 必须归玩家 —— 否则按一下跳起来的同时游戏
    //   被暂停，看起来像"跳跃把游戏卡住了"。Play 态的暂停走工具栏按钮 /
    //   Esc（见下方全屏退出）。
    if (m_ctx->isEditing() && !io.KeyShift &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false))
        m_ctx->togglePlayPause();

    // ---- 运行态：F = 游戏全屏 / 恢复，Esc = ①交还鼠标 ②退出全屏 ----
    // 注意 F 在编辑态里是"聚焦选中项"，两种语义按 Play 状态分开，
    // 不会互相抢键。
    //
    // ⚠ 2026-10-23（P8）：Esc 在这一帧**只做一件事**，而且有优先级 ——
    //   捕获中就交还鼠标（用户原话："按下 Esc 键，此时就正常显示鼠标"），
    //   鼠标已经可见了才是"退出全屏"。写成两个独立 if 的话，全屏 + 捕获
    //   时按一下 Esc 会同时交还鼠标和退全屏，玩家以为按了两次。
    if (!m_ctx->isEditing()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) m_ctx->toggleGameFullscreen();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (m_ctx->playMouseCaptured()) {
                m_ctx->setPlayMouseCaptured(false);
            } else if (m_ctx->gameFullscreen()) {
                m_ctx->gameFullscreenRef() = false;
            }
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

    // 删除：**多选时一次全删**（一条撤销记录）。逻辑收敛在 SceneHierarchy
    // 里 —— 右键菜单的 Delete 走的是同一个函数，两处各写一遍的话迟早
    // 出现"右键删得干净、按 Del 留下孤儿"。
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_ctx->isEditing())
        m_hierarchy->deleteSelection();
}

// ---------------------------------------------------------------- UI 编排

void EditorApp::onImGui() {
    // ⚠ 第一件事：把上一帧记下的"离屏目标尺寸变了"真正落地。
    // 必须抢在**任何 UI 被构建之前** —— 重建会重新注册 ImGui 纹理，
    // 而 UI 里 ImGui::Image 抓到的必须是重建**之后**的句柄（详见
    // Renderer::applyPendingResizes 的注释：放晚一步就是拖窗口边框崩溃）。
    renderer().applyPendingResizes();

    // E18：命令面板开着时**不让快捷键穿透** —— 那时面板的输入框在收字，
    // 若照跑 handleShortcuts，敲 "s" 会顺手触发吸附、敲数字会切视角。
    if (!m_showCommandPalette) handleShortcuts();

    // View 菜单里的 "Toggle Fullscreen (F11)"：菜单在面板之后就画了，
    // 置位当帧处理掉即可（下一帧才生效，肉眼无感）
    if (m_ctx->windowFullscreenRequest()) {
        m_ctx->windowFullscreenRequest() = false;
        toggleWindowFullscreen();
    }

    // Content 工具栏的 "Save All"：场景 + 输入映射一起落盘。
    // 面板只置位（见 ContentBrowser::saveAll），存盘路径留在这边 ——
    // 它才知道 scenePath，也才拿得到 InputMap 的落盘入口。
    if (m_ctx->saveAllRequest()) {
        m_ctx->saveAllRequest() = false;
        saveSceneNow();
        m_ctx->saveInputMap();
        VK_LOG_INFO("CB-SAVEALL done scene=%s", m_ctx->scenePath().c_str());
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

    // 编辑模式（2026-10-20）：模式变了就开关对应的编辑器面板。
    // 必须排在 drawPanels **之前** —— 面板的显隐在这一帧就要定下来，
    // 晚一帧会出现"点了地形模式、面板闪一下才出来"。
    serviceEditMode();

    // 左栏（模式列表 + 当前模式参数）。列表先画：它是用户切换模式的入口，
    // 参数区（各模式自己的 drawPanel）在后面由 drawPanels 派发。
    drawModeRail(*m_ctx);

    // 视口标签条：先把"面板开/关"同步成标签页切换，再画条。
    // 顺序不能反 —— 否则这一帧点开的 Physics 要等下一帧才高亮。
    syncViewportTabs();
    drawViewportTabBar(m_tabBarRect);

    // E04：所有面板**表驱动**绘制。顺序 = 表里的顺序（主面板在前、
    // 浮动调试面板在后），与改造前逐条一致。
    // Content 已改成抽屉（见 drawContentDrawer），这里跳过它。
    drawPanels();

    // 托盘面板的最小化状态上报（每帧读窗口对象，变化时打一行日志）
    servicePanelTrayState();

    // Content 抽屉：必须排在 drawPanels 之后 —— ImGui 里后画的窗口在上层，
    // 浮层形态（未停靠）要盖住视口。
    drawContentDrawer();

    // 底栏（内容 / 输出日志 / CMD + 资源统计）。画在抽屉之后、状态栏之前：
    // 它是一条常驻通栏，Z 序上要压住任何从它上面弹出来的东西的底边。
    drawBottomBar();

    if (m_ctx->showSettings()) drawSettingsPanel();
    drawCharacterWindow();
    drawDialogueEditorWindow();
    drawTimelineEditorWindow();
    drawUiEditorWindow();

    // 浮动面板标题栏的"最小化"按钮。必须排在**所有窗口画完之后**：它要读
    // 每个窗口这一帧的 Pos/Size 才能把那一格摆到关闭 X 的左边（见函数注释）。
    drawPanelMinimizeButtons();

    // 上一帧请求的焦点（窗口那时可能还没建起来，所以延后一帧）
    if (m_focusPanelWant) {
        const std::string n = trayWindowName(m_focusPanelWant);
        if (ImGui::FindWindowByName(n.c_str())) ImGui::SetWindowFocus(n.c_str());
        m_focusPanelWant = nullptr;
    }

    drawStatusBar();

    // 未停靠的抽屉：点面板外任意位置即关。判定放在**所有窗口都画完之后**
    // —— 否则"这一帧谁被点到"还没确定（ImGui 的 hovered 是按窗口累积的）。
    serviceContentDrawerDismiss();

    // 分栏分隔条：在**所有面板之后**处理 —— 分隔线画在前景层（盖在面板上），
    // 拖拽用鼠标位置手动判断（不依赖面板窗口层级）
    // 遮挡列表必须在这里收集：此刻 g.Windows 里才有这一帧**全部**窗口的
    // 真实矩形（弹层/菜单也是这一帧才建起来的），早一帧就会漏。
    collectSplitOccluders();
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

    // ⚠ 状态串要在**显示时**过一遍 TR，而不是在 setStatus / 构造时。
    //   原因：初始值 `m_status = "Ready"` 是成员默认初始化，那一刻界面语言
    //   还没从存档 / MYVK_EDITOR_LANG 应用（applyAppearance 在后面跑），
    //   在那里翻会永久钉死在英文。显示时翻则换语言立刻跟着变。
    //   所有 setStatus 传进来的值本身已经 TR 过（中文），tr() 查不到就原样
    //   返回 —— 所以这条对"播放失败: xxx"这类动态串是无害的。
    ImGui::TextUnformatted(TR(m_ctx->status().c_str()));
    ImGui::SameLine();
    ImGui::TextDisabled(TR("| history %zu/%zu"),
                        m_ctx->commands().cursor(),
                        m_ctx->commands().depth());

    // 选中数（E21 要的是"一眼看全局"）：单选时顺带把名字带上 —— 名字比
    // 数字有用，但"到底选了没有"这件事得有个数字化的答案。
    const int selCount = m_ctx->hasSelection() ? 1 : 0;
    ImGui::SameLine();
    if (selCount == 1) {
        const std::string* np = m_ctx->activeScene().world().name(
            m_ctx->selection());
        ImGui::TextDisabled(TR("| selected: %d (%s)"), selCount,
                            np ? np->c_str() : "unnamed");
    } else {
        ImGui::TextDisabled(TR("| selected: %d"), selCount);
    }

    // 播放状态 + 实体计数：以前挤在工具条最右端（那条工具栏本来就很长，
    // 一起被挤出右边缘裁掉），挪到状态栏反而更合理 —— 这里本来就是
    // "一眼看全局状态"的地方。
    ImGui::SameLine();
    const bool playing = m_ctx->isPlaying();
    const bool paused = m_ctx->isPaused();
    const char* stateText = playing ? (m_ctx->gameFullscreen()
                                           ? TR("PLAYING (fullscreen)")
                                           : TR("PLAYING (in viewport)"))
                                    : (paused ? TR("PAUSED") : TR("EDITING"));
    const ImVec4 stateCol = playing ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
                                    : (paused ? ImVec4(0.95f, 0.8f, 0.4f, 1.0f)
                                              : ImVec4(0.62f, 0.67f, 0.74f, 1.0f));
    ImGui::TextDisabled(TR("|"));
    ImGui::SameLine();
    ImGui::TextColored(stateCol, TR("%s"), stateText);
    ImGui::SameLine();
    ImGui::TextDisabled(TR("| %zu entities (editor %zu / runtime %zu)"),
                        m_ctx->activeScene().objectCount(),
                        m_ctx->editorScene().objectCount(),
                        m_ctx->runtimeScene().objectCount());

    // ---- E21：保存状态 / 后台任务 / WARN-ERROR 计数（状态栏常驻四项）----
    // 保存状态：`*` 未保存（`dirty` 开关由 EditorContext 维护，保存即清）。
    ImGui::SameLine();
    ImGui::TextDisabled(TR("|"));
    ImGui::SameLine();
    if (m_ctx->dirty())
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f), TR("* unsaved"));
    else
        ImGui::TextDisabled(TR("saved"));

    // 后台任务数：>0 时点一下把进度条恢复出来（最小化后的归处）。
    const int nTasks = m_notifications->taskCount();
    ImGui::SameLine();
    ImGui::TextDisabled(TR("|"));
    ImGui::SameLine();
    if (nTasks > 0) {
        char label[48];
        std::snprintf(label, sizeof(label), TR("tasks %d"), nTasks);
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), TR("%s"), label);
        if (m_notifications->tasksMinimized() && ImGui::IsItemClicked())
            m_notifications->setTasksMinimized(false);
    } else {
        ImGui::TextDisabled(TR("tasks 0"));
    }

    // WARN / ERROR 计数：点一下打开 Console（"我刚错过了什么"的最短路径）。
    const int nErr = m_notifications->errorCount();
    const int nWarn = m_notifications->warnCount();
    ImGui::SameLine();
    ImGui::TextDisabled(TR("|"));
    ImGui::SameLine();
    if (nErr > 0) {
        const auto c = NotificationCenter::levelColor(core::LogLevel::Error);
        ImGui::TextColored(ImVec4(c.r, c.g, c.b, 1.0f), TR("errors %d"), nErr);
    } else {
        ImGui::TextDisabled(TR("errors 0"));
    }
    const bool errClicked = ImGui::IsItemClicked();
    ImGui::SameLine();
    ImGui::TextDisabled(TR("|"));
    ImGui::SameLine();
    if (nWarn > 0) {
        const auto c = NotificationCenter::levelColor(core::LogLevel::Warn);
        ImGui::TextColored(ImVec4(c.r, c.g, c.b, 1.0f), TR("warns %d"), nWarn);
    } else {
        ImGui::TextDisabled(TR("warns 0"));
    }
    const bool warnClicked = ImGui::IsItemClicked();
    if ((errClicked || warnClicked) && m_notifications->historyCount() > 0) {
        m_ctx->panels().console = true;
        m_showNotificationHistory = true;
    }

    const int nUnread = m_notifications->unread();
    if (nUnread > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled(TR("|"));
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("notifications"))) m_showNotificationHistory = true;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), TR("%d"), nUnread);
    }

    // 自动化锚点：上面四个数值任一变化就打一条 NOTIF-STATUSBAR（边沿触发）。
    logNotificationStatus();

    // 瞬时提示（3 秒）：靠右显示
    if (!m_ctx->notification().empty()) {
        const float w = ImGui::CalcTextSize(m_ctx->notification().c_str()).x;
        ImGui::SameLine(std::max(ImGui::GetWindowWidth() - w - 20.0f, 300.0f));
        const float a = std::min(m_ctx->notificationTimer() / 0.6f, 1.0f);
        ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.6f, a), TR("%s"),
                           m_ctx->notification().c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

// ============================================================
// 底栏 + Content 抽屉（2026-10-17）
// ============================================================
//
// 布局动机（用户原话）："原本的 Content 面板移除掉，把 '7 entries | assets'
// 那部分位置和底部栏的动画设置部分合并成一行；在这一栏的左侧放一个内容
// 侧滑菜单（前面有 Logo），紧接着是输出日志（同样带 Logo），再往右是 CMD"。
//
// 落地方式：
//   · Content 不再占分栏（SplitLayout 里它恒不 live，左栏只剩 Hierarchy）；
//   · 状态栏上方多一条通栏 `L.bottom`：左起「内容 / 输出日志 / CMD」三个
//     带矢量图标的按钮，右侧是资源统计（原来 Content 面板底行那一条）；
//   · 点「内容」→ 抽屉从底栏往上弹（浮层），可拖上边缘改高度；
//     抽屉右上角 "Dock in layout" 勾上 = 真正占布局（视口变矮、不自动关闭）。
//   · 播放条（Run/Play/Pause/Stop + 时间轴）的 UI 整条移除。
//
// 图标全部 ImDrawList 现画（与 EditorIcons.h 同一套约定：不塞图标字体，
// 任意 DPI 都清晰）。颜色写死 —— 图标是"内容"，不该跟着主题漂移。
namespace {

// 底栏图标表。2026-10-17 晚加了一整组：托盘里那些面板按钮各要一个
// Logo —— 一列长得一样的按钮没法用。
enum BarIcon {
    kBarIconContent = 0,
    kBarIconLog,
    kBarIconCmd,
    kBarIconPlus,
    kBarIconCharacter,
    kBarIconPhysics,
    kBarIconParticles,
    kBarIconStats,
    kBarIconScripts,
    kBarIconEngine,
    kBarIconConsole,
    kBarIconTimeline,
};

void drawBarIcon(ImDrawList* dl, ImVec2 p, float s, int icon) {
    if (icon == kBarIconContent) {
        // 文件夹图标直接复用资源浏览器那个大号版本（尺寸参数化，18px 也清晰）
        drawFolderIcon(dl, p, s);
        return;
    }
    if (icon == kBarIconLog) {
        // 输出日志：一页日志纸 —— 深蓝底 + 三行亮横线 + 左侧状态点。
        // 用"纸 + 行"而不是"终端 >_"，是为了和右边的 CMD 一眼区分开。
        const ImU32 bg = IM_COL32(34, 50, 70, 255);
        const ImU32 edge = IM_COL32(96, 150, 198, 255);
        const ImU32 line = IM_COL32(152, 208, 248, 255);
        dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), bg, s * 0.16f);
        dl->AddRect(p, ImVec2(p.x + s, p.y + s), edge, s * 0.16f, 0, 1.3f);
        for (int i = 0; i < 3; ++i) {
            const float y = p.y + s * (0.29f + 0.21f * static_cast<float>(i));
            dl->AddCircleFilled(ImVec2(p.x + s * 0.24f, y), s * 0.055f, line, 8);
            dl->AddRectFilled(ImVec2(p.x + s * 0.40f, y - s * 0.042f),
                              ImVec2(p.x + s * 0.82f, y + s * 0.042f), line,
                              s * 0.03f);
        }
        return;
    }
    if (icon == kBarIconPlus) {
        // 加号：两条粗线。底框用强调色的淡底，一眼看出"这是个入口"。
        const ImU32 bg = IM_COL32(38, 62, 92, 255);
        const ImU32 fg = IM_COL32(148, 206, 255, 255);
        dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), bg, s * 0.22f);
        const float th = std::max(1.6f, s * 0.11f);
        dl->AddLine(ImVec2(p.x + s * 0.26f, p.y + s * 0.5f),
                    ImVec2(p.x + s * 0.74f, p.y + s * 0.5f), fg, th);
        dl->AddLine(ImVec2(p.x + s * 0.5f, p.y + s * 0.26f),
                    ImVec2(p.x + s * 0.5f, p.y + s * 0.74f), fg, th);
        return;
    }
    const float cx = p.x + s * 0.5f;
    const float cy = p.y + s * 0.5f;
    switch (icon) {
        case kBarIconCmd: {
            // CMD：终端窗口 —— 近黑底 + 绿色 ">" + 光标方块
            const ImU32 bg = IM_COL32(16, 22, 18, 255);
            const ImU32 edge = IM_COL32(88, 204, 134, 255);
            const ImU32 fg = IM_COL32(124, 242, 162, 255);
            dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), bg, s * 0.16f);
            dl->AddRect(p, ImVec2(p.x + s, p.y + s), edge, s * 0.16f, 0, 1.3f);
            const ImVec2 a(p.x + s * 0.22f, p.y + s * 0.30f);
            const ImVec2 b(p.x + s * 0.44f, p.y + s * 0.50f);
            const ImVec2 c(p.x + s * 0.22f, p.y + s * 0.70f);
            dl->AddLine(a, b, fg, 1.7f);
            dl->AddLine(b, c, fg, 1.7f);
            dl->AddRectFilled(ImVec2(p.x + s * 0.52f, p.y + s * 0.58f),
                              ImVec2(p.x + s * 0.80f, p.y + s * 0.72f), fg,
                              s * 0.04f);
            break;
        }
        case kBarIconCharacter: {
            // 人形：头 + 肩。角色面板最没有歧义的图标。
            const ImU32 fg = IM_COL32(214, 186, 140, 255);
            dl->AddCircleFilled(ImVec2(cx, p.y + s * 0.30f), s * 0.16f, fg, 12);
            dl->PathClear();
            dl->PathArcTo(ImVec2(cx, p.y + s * 0.92f), s * 0.30f, 3.30f, 6.12f, 16);
            dl->PathStroke(fg, 0, std::max(1.6f, s * 0.13f));
            break;
        }
        case kBarIconPhysics: {
            // 原子：一个核 + 两个斜椭圆 = "物理"
            const ImU32 fg = IM_COL32(150, 208, 214, 255);
            dl->AddCircleFilled(ImVec2(cx, cy), s * 0.10f, fg, 10);
            dl->PathClear();
            dl->PathEllipticalArcTo(ImVec2(cx, cy), ImVec2(s * 0.42f, s * 0.18f),
                                    0.6f, 0.0f, 6.2832f, 20);
            dl->PathStroke(fg, 0, 1.4f);
            dl->PathClear();
            dl->PathEllipticalArcTo(ImVec2(cx, cy), ImVec2(s * 0.42f, s * 0.18f),
                                    2.5f, 0.0f, 6.2832f, 20);
            dl->PathStroke(fg, 0, 1.4f);
            break;
        }
        case kBarIconParticles: {
            // 火花团：中心一点 + 四向短线
            const ImU32 fg = IM_COL32(246, 176, 108, 255);
            dl->AddCircleFilled(ImVec2(cx, cy), s * 0.13f, fg, 10);
            const float r0 = s * 0.22f, r1 = s * 0.42f;
            for (int i = 0; i < 4; ++i) {
                const float a = 0.7854f + 1.5708f * static_cast<float>(i);
                dl->AddLine(ImVec2(cx + std::cos(a) * r0, cy + std::sin(a) * r0),
                            ImVec2(cx + std::cos(a) * r1, cy + std::sin(a) * r1),
                            fg, 1.5f);
            }
            break;
        }
        case kBarIconStats: {
            // 柱状图
            const ImU32 fg = IM_COL32(138, 196, 246, 255);
            const float bw = s * 0.16f;
            const float hs[3] = {0.34f, 0.62f, 0.46f};
            for (int i = 0; i < 3; ++i) {
                const float x = p.x + s * (0.16f + 0.28f * static_cast<float>(i));
                dl->AddRectFilled(ImVec2(x, p.y + s * (0.86f - hs[i])),
                                  ImVec2(x + bw, p.y + s * 0.86f), fg, 1.0f);
            }
            break;
        }
        case kBarIconScripts: {
            // 尖括号 </>
            const ImU32 fg = IM_COL32(196, 168, 246, 255);
            dl->AddLine(ImVec2(p.x + s * 0.40f, p.y + s * 0.24f),
                        ImVec2(p.x + s * 0.20f, cy), fg, 1.8f);
            dl->AddLine(ImVec2(p.x + s * 0.20f, cy),
                        ImVec2(p.x + s * 0.40f, p.y + s * 0.76f), fg, 1.8f);
            dl->AddLine(ImVec2(p.x + s * 0.60f, p.y + s * 0.24f),
                        ImVec2(p.x + s * 0.80f, cy), fg, 1.8f);
            dl->AddLine(ImVec2(p.x + s * 0.80f, cy),
                        ImVec2(p.x + s * 0.60f, p.y + s * 0.76f), fg, 1.8f);
            break;
        }
        case kBarIconEngine: {
            // 齿轮
            const ImU32 fg = IM_COL32(198, 202, 210, 255);
            dl->AddCircle(ImVec2(cx, cy), s * 0.18f, fg, 14, 1.6f);
            for (int i = 0; i < 8; ++i) {
                const float a = 6.2832f * static_cast<float>(i) / 8.0f;
                dl->AddLine(ImVec2(cx + std::cos(a) * s * 0.22f,
                                   cy + std::sin(a) * s * 0.22f),
                            ImVec2(cx + std::cos(a) * s * 0.40f,
                                   cy + std::sin(a) * s * 0.40f),
                            fg, 1.6f);
            }
            break;
        }
        case kBarIconConsole: {
            // 终端提示符（和 CMD 同形，但底色偏灰 —— 一个是"命令输入"，
            // 一个是"日志输出"）
            const ImU32 bg = IM_COL32(30, 32, 38, 255);
            const ImU32 fg = IM_COL32(196, 204, 216, 255);
            dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), bg, s * 0.16f);
            dl->AddLine(ImVec2(p.x + s * 0.22f, p.y + s * 0.32f),
                        ImVec2(p.x + s * 0.42f, p.y + s * 0.5f), fg, 1.5f);
            dl->AddLine(ImVec2(p.x + s * 0.42f, p.y + s * 0.5f),
                        ImVec2(p.x + s * 0.22f, p.y + s * 0.68f), fg, 1.5f);
            dl->AddRectFilled(ImVec2(p.x + s * 0.52f, p.y + s * 0.58f),
                              ImVec2(p.x + s * 0.78f, p.y + s * 0.70f), fg,
                              s * 0.04f);
            break;
        }
        case kBarIconTimeline: {
            // 时间轴：一条基线 + 几个刻度 + 一个游标
            const ImU32 fg = IM_COL32(228, 196, 120, 255);
            dl->AddLine(ImVec2(p.x + s * 0.14f, p.y + s * 0.70f),
                        ImVec2(p.x + s * 0.86f, p.y + s * 0.70f), fg, 1.5f);
            for (int i = 0; i < 4; ++i) {
                const float x = p.x + s * (0.20f + 0.20f * static_cast<float>(i));
                dl->AddLine(ImVec2(x, p.y + s * 0.70f),
                            ImVec2(x, p.y + s * 0.56f), fg, 1.2f);
            }
            dl->AddRectFilled(ImVec2(p.x + s * 0.56f, p.y + s * 0.24f),
                              ImVec2(p.x + s * 0.62f, p.y + s * 0.72f), fg, 0.5f);
            break;
        }
        default: break;
    }
}

// 面板 id → 底栏图标。托盘按钮和勾选列表共用这一份映射。
int barIconForPanel(const char* id) {
    const std::string s = id ? id : "";
    if (s == "character") return kBarIconCharacter;
    if (s == "physics") return kBarIconPhysics;
    if (s == "particles") return kBarIconParticles;
    if (s == "stats") return kBarIconStats;
    if (s == "scripts") return kBarIconScripts;
    if (s == "engine") return kBarIconEngine;
    if (s == "console") return kBarIconConsole;
    if (s == "gputimeline") return kBarIconTimeline;
    return kBarIconEngine;   // 兜底：将来新加的面板至少有个像样的图标
}

// 哪些面板"可以变成底栏按钮"。判据只有一条：**它能不能离开布局**。
//   · 分栏里的四个面板（Hierarchy / Inspector / Content / Viewport）由
//     SplitLayout 定位，给它们底栏按钮只会和分栏打架 —— 它们仍然出现在
//     勾选列表里（属于"布局面板"那一段），只是不生成按钮。
//   · Content 与 Output Log 在底栏已经各有一个按钮（这是用户上一轮的设计），
//     再生成一个就是同一个开关两个入口，只会让人怀疑哪个才是真的。
bool panelTrayEligible(const PanelDesc& p) {
    if (p.layoutSlot != kSlotNone) return false;
    const std::string id = p.id ? p.id : "";
    if (id == "console" || id == "content") return false;
    return true;
}

// "浮动面板" = 有自己的 ImGui 标题栏、位置自己管的那种。
// ⚠ 和 panelTrayEligible **不是**同一个条件：Console 在底栏已经有专属按钮
//   （不该再生成一个托盘按钮），但它确实是浮动窗口，标题栏上那个最小化
//   按钮必须有。2026-10-17 第四轮拆出这一条，就是别再让"按钮资格"顺手
//   决定了"有没有最小化"。
bool panelFloating(const PanelDesc& p) {
    return p.layoutSlot == kSlotNone && p.draw != nullptr;
}

// 面板的 ImGui 窗口名（只要 "###" 之后那一段 —— ImGui 按它算窗口 ID，
// 所以带不带译文都指同一个窗口）。Character 是历史遗留的 "character_v2"。
std::string trayWindowName(const char* id) {
    const std::string s = id ? id : "";
    if (s == "character") return "###character_v2";
    return "###panel_" + s;
}

// 托盘按钮右侧"叉号带"的宽度。命中区、绘制、日志矩形三处共用这一个数 ——
// 分开写就会重演"点叉号却点到了按钮"那个 bug。
constexpr float kTrayRemoveW = 14.0f;

} // namespace

bool EditorApp::barButton(const char* id, const char* label, int icon,
                          bool active, const char* tip) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float iconS = 18.0f;
    const ImVec2 txt = ImGui::CalcTextSize(label);
    const float h = ImGui::GetTextLineHeight() + 7.0f;
    const float w = st.FramePadding.x * 2.0f + iconS + 8.0f + txt.x;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##hit", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImGui::PopID();

    char tag[64];
    std::snprintf(tag, sizeof(tag), "BAR-BTN-%s", id);
    logRect(tag, p, ImVec2(p.x + w, p.y + h));

    // 背景：选中 = 强调色淡底 + 描边；悬停 = 亮一档；按住 = 再亮一点。
    // 全部用 token，浅色主题下不会变成一块深色孤岛。
    const ImVec4 bg = held      ? ui::tokens().accentSoft
                      : hovered ? ImVec4(1.0f, 1.0f, 1.0f, 0.10f)
                      : active  ? ui::tokens().accentSoft
                                : ImVec4(1.0f, 1.0f, 1.0f, 0.045f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bg), 4.0f);
    if (active) {
        dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                    ImGui::GetColorU32(ui::tokens().accent), 4.0f, 0, 1.2f);
    }

    drawBarIcon(dl, ImVec2(p.x + st.FramePadding.x, p.y + (h - iconS) * 0.5f),
                iconS, icon);
    // 文字：选中 = 正常正文色；未选中 = 正常色降到 78% —— 不用 TextDisabled，
    // 那个太暗（0.48 灰），在底栏这种"常驻入口"上会显得像禁用项。
    ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!active) tc.w *= 0.78f;
    dl->AddText(ImVec2(p.x + st.FramePadding.x + iconS + 8.0f,
                       p.y + (h - txt.y) * 0.5f),
                ImGui::GetColorU32(tc), label);

    if (hovered && tip) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// 托盘按钮：和 barButton 同款，但右上角多一个 ×，右键还有小菜单。
//
// 命中分成两块：主体（宽 w-14）和叉号（右上 14×14）。**绝对不能用一个
// 大按钮把叉号区也吃掉** —— 那样"点叉号"和"点按钮"就没法区分了
// （用户明确要的是"点右上角的叉号移除"）。
EditorApp::TrayHit EditorApp::trayButton(const char* id, const char* label,
                                         int icon, bool active,
                                         const char* tip) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float iconS = 18.0f;
    const ImVec2 txt = ImGui::CalcTextSize(label);
    const float h = ImGui::GetTextLineHeight() + 7.0f;
    const float bodyW = st.FramePadding.x * 2.0f + iconS + 8.0f + txt.x;
    const float w = bodyW + kTrayRemoveW;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    m_trayBtnH = h;

    TrayHit hit = TrayHit::None;

    ImGui::PushID(id);
    // 主体：点它 = 开关面板
    if (ImGui::InvisibleButton("##body", ImVec2(bodyW, h))) hit = TrayHit::Toggle;
    const bool bodyHovered = ImGui::IsItemHovered();
    // 自动化锚点：每个托盘按钮自己的矩形（`BAR-TRAY-<面板id>`）。
    // 之前只有 `BAR-TRAY-PLUS` 和右键菜单里的两项有锚点，脚本没法"点某个
    // 面板的托盘按钮"——于是"最小化之后怎么恢复"根本没被覆盖过。
    {
        // id 传进来时带 "tray_" 前缀（见调用点），锚点里用干净的面板 id
        const char* pid = (id && std::strncmp(id, "tray_", 5) == 0) ? id + 5 : id;
        char tag[64];
        std::snprintf(tag, sizeof(tag), "BAR-TRAY-%s", pid ? pid : "");
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    // 叉号：紧贴主体右侧的**整块**命中区。
    //
    // 这里踩过一次：一开始只放了 12x12 的小方块（视觉上 × 就这么大），
    // 但它和日志里报出去的矩形（整条右侧竖带）对不上 —— 自动化按矩形
    // 中心点下去，落在小方块外面 → 点"叉号"变成点"按钮"，表现为
    // "叉号只会关面板、不会移除按钮"。现在命中区 = 日志矩形 = 绘制区域。
    ImGui::SetCursorScreenPos(ImVec2(p.x + bodyW, p.y));
    if (ImGui::InvisibleButton("##x", ImVec2(kTrayRemoveW, h))) hit = TrayHit::Remove;
    const bool xHovered = ImGui::IsItemHovered();

    // 右键菜单：最小化 / 关闭（最小化的主入口是窗口标题栏那颗折叠箭头，
    // 这里给一个"不用瞄准"的第二入口）
    if (bodyHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##tray_ctx");
    }
    if (ImGui::BeginPopup("##tray_ctx")) {
        const std::string wname = trayWindowName(id + 5);   // 去掉 "tray_" 前缀
        ImGuiWindow* win = ImGui::FindWindowByName(wname.c_str());
        const bool collapsed = win ? (win->Collapsed != 0) : false;
        if (ImGui::MenuItem(TR(collapsed ? TRL("Expand") : TRL("Minimize")))) {
            ImGui::SetWindowCollapsed(wname.c_str(), !collapsed);
            VK_LOG_INFO("PANEL-TRAY %s minimized=%d", id + 5,
                        collapsed ? 0 : 1);
        }
        logRect("BAR-TRAY-CTX-min", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::MenuItem(TR("Close panel"))) {
            hit = TrayHit::Remove;
        }
        logRect("BAR-TRAY-CTX-close", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::EndPopup();
    }
    ImGui::PopID();

    const ImVec2 q(p.x + w, p.y + h);
    const ImVec4 bg = active ? ui::tokens().accentSoft : ImVec4(1.0f, 1.0f, 1.0f, 0.045f);
    dl->AddRectFilled(p, ImVec2(p.x + bodyW, q.y), ImGui::GetColorU32(bg), 4.0f);
    if (active) {
        dl->AddRect(p, ImVec2(p.x + bodyW, q.y),
                    ImGui::GetColorU32(ui::tokens().accent), 4.0f, 0, 1.2f);
    } else if (bodyHovered) {
        dl->AddRectFilled(p, ImVec2(p.x + bodyW, q.y), IM_COL32(255, 255, 255, 26),
                          4.0f);
    }

    drawBarIcon(dl, ImVec2(p.x + st.FramePadding.x, p.y + (h - iconS) * 0.5f),
                iconS, icon);
    ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (!active) tc.w *= 0.78f;
    dl->AddText(ImVec2(p.x + st.FramePadding.x + iconS + 8.0f,
                       p.y + (h - txt.y) * 0.5f),
                ImGui::GetColorU32(tc), label);

    // 叉号：常态淡（不抢视线），悬停变红 —— 它是"删除"，该警示一下
    const ImVec2 xc(p.x + bodyW + kTrayRemoveW * 0.5f, p.y + h * 0.5f);
    const ImU32 xCol = xHovered ? IM_COL32(248, 122, 122, 255)
                                : IM_COL32(168, 176, 188, 150);
    dl->AddLine(ImVec2(xc.x - 3.0f, xc.y - 3.0f), ImVec2(xc.x + 3.0f, xc.y + 3.0f),
                xCol, 1.4f);
    dl->AddLine(ImVec2(xc.x - 3.0f, xc.y + 3.0f), ImVec2(xc.x + 3.0f, xc.y - 3.0f),
                xCol, 1.4f);

    char tag[64];
    std::snprintf(tag, sizeof(tag), "BAR-TRAY-%s", id);
    logRect(tag, p, ImVec2(p.x + bodyW, q.y));
    std::snprintf(tag, sizeof(tag), "BAR-TRAY-X-%s", id);
    logRect(tag, ImVec2(p.x + bodyW, p.y), q);

    if ((bodyHovered || xHovered) && tip) ImGui::SetTooltip("%s", tip);
    return hit;
}

// 托盘面板的"最小化"（= ImGui 的折叠）状态上报。
//
// 为什么需要它：折叠状态是 ImGui 窗口自己的，改动发生在**标题栏那颗折叠
// 箭头**上（或者双击标题栏），我们没有任何控件回调可以挂。而"面板可以
// 最小化"是用户明确要的能力 —— 没有可断言的痕迹，就没法证明它没坏。
// 每帧读一次窗口对象，只有变化才打一行，成本可以忽略。
void EditorApp::servicePanelTrayState() {
    for (int i = 0; i < panelCount(); ++i) {
        const PanelDesc& pd = panelTable()[i];
        // 用 panelFloating 而不是 panelTrayEligible：Console 没有托盘按钮，
        // 但它有最小化按钮，收起状态同样要上报（自动化断言要用）。
        if (!panelFloating(pd)) continue;
        const bool isOpen = panelOpen(i) && !panelTrayHiddenRef(i);
        const std::string wname = trayWindowName(pd.id);
        ImGuiWindow* win = isOpen ? ImGui::FindWindowByName(wname.c_str()) : nullptr;
        const int min = (win && win->Collapsed) ? 1 : 0;
        if (isOpen != m_trayStateOpen[i] || min != m_trayStateMin[i]) {
            m_trayStateOpen[i] = isOpen;
            m_trayStateMin[i] = min;
            VK_LOG_INFO("PANEL-STATE %s open=%d min=%d", pd.id,
                        isOpen ? 1 : 0, min);
        }
    }
}

// ---------------------------------------------------------------- 面板最小化
//
// 2026-10-17 第四轮（用户原话）："在 Content 面板这里通过点击 + 打开的所有
// 面板，右上角都要有可以最小化的按钮，点击最小化即把面板窗口最小化放到
// Content 面板上 + 按钮右侧原来的位置上。"
//
// 落地：托盘按钮不消失（那等于"取消勾选"），只是把窗口收起来 —— 于是
// "最小化"= `panelTrayHiddenRef(i) = true`，底栏那个按钮变暗、再点一下就回来。
// 这和右键托盘按钮里的"最小化"完全是同一条路径，两处入口、一份状态。

void EditorApp::focusPanel(const char* id) {
    const int idx = panelIndexOf(id);
    if (idx < 0) return;
    // setPanelOpen 里带了"打开就把托盘收起态作废"，所以这里不用再清一次
    setPanelOpen(idx, true);
    // 2026-10-21：视口标签页宿主里，"聚焦一张面板"= 切到它的那一页。
    // 少了这一句，底栏点 Output Log（本来就是开着的）会静默无效 ——
    // 面板确实活着，只是它那一页不是当前页。
    setActiveViewportTab(idx);
    m_focusPanelWant = panelId(idx);
    VK_LOG_INFO("PANEL-FOCUS %s", id);
}

void EditorApp::drawPanelMinimizeButtons() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float fs = g.FontSize;
    const ImVec2 mp = ImGui::GetIO().MousePos;

    // 上一帧上报过的矩形（只为"变化时才打日志"，不打的话每帧刷屏）
    static float lastX[kMaxPanels] = {};
    static float lastY[kMaxPanels] = {};
    static bool lastHov[kMaxPanels] = {};

    for (int i = 0; i < panelCount(); ++i) {
        const PanelDesc& pd = panelTable()[i];
        // 浮动面板才有标题栏；分栏面板与抽屉（Content）不在此列
        if (!panelFloating(pd)) continue;
        if (!panelOpen(i) || panelTrayHiddenRef(i)) continue;

        const std::string wname = trayWindowName(pd.id);
        ImGuiWindow* w = ImGui::FindWindowByName(wname.c_str());
        if (!w || w->Collapsed || w->Hidden || !w->Active) continue;
        // 2026-10-21：被托管成视口标签页的面板**没有标题栏**（标签条就是它的
        // 标题），那个位置现在是内容区右上角 —— 在这里画一条"最小化横线"
        // 会变成一件飘在内容上的怪东西，点一下还会把整页收走。跳过。
        if (w->Flags & ImGuiWindowFlags_NoTitleBar) continue;

        // 关闭 X 的几何来自 imgui.cpp:RenderWindowTitleBarContents ——
        //   close_pos = (titleMax.x - FramePadding.x - FontSize, titleMin.y + FramePadding.y)
        // 我们占它左边一格（再让 2px，别和 X 的命中区粘在一起）。
        const float x = w->Pos.x + w->Size.x - st.FramePadding.x - fs * 2.0f - 2.0f;
        const float y = w->Pos.y + st.FramePadding.y;
        const ImVec2 mn(x, y), mx(x + fs, y + fs);

        // 命中：鼠标在格子里 **且** 这一帧悬停的就是这个窗口（被别的窗口
        // 盖住时 g.HoveredWindow 是上面那个，于是点不到 —— 正是想要的）。
        const bool inRect = mp.x >= mn.x && mp.x < mx.x && mp.y >= mn.y && mp.y < mx.y;
        const bool hovered = inRect && g.HoveredWindow == w;

        if (hovered) {
            w->DrawList->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, 46), 3.0f);
        }
        // 一条横线 = 最小化（比画一个 "–" 字形更不依赖字体里有没有这个字符）
        const float cy = (mn.y + mx.y) * 0.5f;
        w->DrawList->AddLine(ImVec2(mn.x + fs * 0.22f, cy),
                             ImVec2(mx.x - fs * 0.22f, cy),
                             ImGui::GetColorU32(ImGuiCol_Text), 1.6f);

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            panelTrayHiddenRef(i) = true;
            VK_LOG_INFO("PANEL-MIN %s hidden=1", pd.id);
        }

        if (lastX[i] != mn.x || lastY[i] != mn.y || lastHov[i] != hovered) {
            lastX[i] = mn.x;
            lastY[i] = mn.y;
            lastHov[i] = hovered;
            // 自动化锚点：`PANEL-MIN <id>=(x0,y0)-(x1,y1)`
            // （tag 里带空格，脚本侧要用自己的正则取，别指望 last_rect）
            VK_LOG_INFO("PANEL-MIN %s=(%.0f,%.0f)-(%.0f,%.0f) hov=%d", pd.id,
                        mn.x, mn.y, mx.x, mx.y, hovered ? 1 : 0);
        }
    }
}

// "+" 的勾选列表。分三段：
//   ① 浮动功能面板（勾上 → 底栏出现按钮）
//   ② 底栏已有按钮的（Content / Output Log，只显示状态，不能重复勾）
//   ③ 布局面板（Hierarchy / Inspector / Viewport，勾选直接开关，但没有按钮）
void EditorApp::drawPanelPicker() {    // 位置：**底边**贴住 + 按钮的上沿，高度随内容往上长。
    //
    // 上一版是"估算总高，再用 顶=+y-估计高 摆"—— 估小了弹层就往下溢出，
    // 压住底栏（实测底部溢出 52px）。改成 SetNextWindowPos 的 pivot=(0,1)
    // 之后，不管里面有几行、翻译成多长，底边永远贴在那儿。
    ImGui::SetNextWindowPos(ImVec2(m_trayPlusX, m_trayPlusY - 6.0f),
                            ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));

    if (!ImGui::BeginPopup("##panel_picker")) return;

    logRect("BAR-PICKER-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // ---- ① 浮动功能面板 ----
    ImGui::SeparatorText(TR("Panels"));
    int nTray = 0;
    for (int i = 0; i < panelCount(); ++i) {
        const PanelDesc& pd = panelTable()[i];
        if (!panelTrayEligible(pd)) continue;
        ++nTray;
        bool* open = panelOpenSlot(i);
        if (!open) continue;
        ImGui::PushID(i);
        const bool was = *open;
        if (ImGui::Checkbox(TR(pd.title), open)) {
            VK_LOG_INFO("PANEL-PICK %s open=%d", pd.id, *open ? 1 : 0);
        }
        // 勾上=按钮出现且面板直接打开（"勾选显示出来"就是这个意思；
        // 只加按钮不开面板会让人以为没生效）
        if (was != *open) {
            // 勾选状态和"窗口被按钮收起"是两回事，重新勾上必须回到可见态
            panelTrayHiddenRef(i) = false;
            if (*open) {
                m_ctx->notify(std::string(TR("Added ")) + TR(pd.title) +
                              TR(" button"));
            }
        }
        char tag[64];
        std::snprintf(tag, sizeof(tag), "BAR-PICK-%s", pd.id);
        logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::PopID();
    }
    if (nTray == 0) ImGui::TextDisabled("%s", TR("(none)"));

    // ---- ② 底栏常驻的两个 ----
    ImGui::SeparatorText(TR("Always in this bar"));
    ImGui::BeginDisabled(true);
    ImGui::Checkbox(TR("Content Browser"), &m_ctx->panels().content);
    ImGui::Checkbox(TR("Console"), &m_ctx->panels().console);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Content and Output Log already have their own\n"
                          "buttons on this bar - they are not duplicated here."));
    }

    // ---- ③ 布局面板 ----
    ImGui::SeparatorText(TR("Docked in the layout"));
    const char* docked[3] = {"hierarchy", "inspector", "viewport"};
    for (const char* id : docked) {
        const int idx = panelIndexOf(id);
        if (idx < 0) continue;
        bool* open = panelOpenSlot(idx);
        if (!open) continue;
        ImGui::PushID(1000 + idx);
        if (ImGui::Checkbox(TR(panelTitle(idx)), open)) {
            VK_LOG_INFO("PANEL-PICK %s open=%d", id, *open ? 1 : 0);
        }
        ImGui::PopID();
    }

    ImGui::EndPopup();
}

void EditorApp::drawBottomBar() {
    const LayoutRects& L = m_ctx->layout();
    m_bottomBarHovered = false;
    if (L.bottom.w < 160.0f || L.bottom.h < 8.0f) return;

    logRect("BAR-BOTTOM", ImVec2(L.bottom.x, L.bottom.y),
            ImVec2(L.bottom.x + L.bottom.w, L.bottom.y + L.bottom.h));

    ImGui::SetNextWindowPos(ImVec2(L.bottom.x, L.bottom.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.bottom.w, L.bottom.h), ImGuiCond_Always);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    // 与工具栏/状态栏同色（走 token）：这条通栏和它们是一套"框"
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::tokens().barBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    if (!ImGui::Begin("##bottombar", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }
    // ⚠ 必须带 AllowWhenBlockedByActiveItem：点击底栏按钮的**松开那一帧**，
    //   InvisibleButton 还是 active item，默认标志下 IsWindowHovered 返回
    //   false → serviceContentDrawerDismiss 会把它当成"点了外部"，
    //   于是"点 Content 打开抽屉"在同帧内被立刻关掉（表现：按钮闪一下、
    //   抽屉永远出不来）。
    m_bottomBarHovered = ImGui::IsWindowHovered(
        ImGuiHoveredFlags_ChildWindows |
        ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    const bool drawerOpen = m_ctx->panels().content;

    if (barButton("content", TR("Content"), kBarIconContent, drawerOpen,
                  TR("Show / hide the Content drawer above this bar.\n"
                     "Drag its top edge to resize; tick “Dock in layout”\n"
                     "to keep it docked instead of a click-away overlay."))) {
        m_ctx->panels().content = !drawerOpen;
        VK_LOG_INFO("CONTENT-DRAWER open=%d dock=%d h=%.0f",
                    static_cast<int>(!drawerOpen),
                    static_cast<int>(m_contentDock), m_contentDrawerH);
    }

    ImGui::SameLine();
    if (barButton("log", TR("Output Log"), kBarIconLog,
                  m_ctx->panels().console,
                  TR("Show / hide the Output Log (same panel as Alt+8)."))) {
        // ⚠ 2026-10-17 第四轮：用户报"点 Output Log 没打开面板"。原代码是
        //   `console = !console` —— 它**只翻注册表那一格**，而 Console 这个
        //   窗口的开合还受"托盘临时收起"和 ImGui 自己的窗口位置影响。改成：
        //   看不见 → 弄到**真的看得见**（清收起态 + 请求焦点）；看得见 → 关。
        //   "看得见"= 勾着 && 没被托盘收起。
        const int idx = panelIndexOf("console");
        const bool visible = panelOpen(idx) && !panelTrayHiddenRef(idx);
        if (visible) {
            setPanelOpen(idx, false);
        } else {
            focusPanel("console");
        }
        VK_LOG_INFO("OUTPUT-LOG open=%d visible_before=%d",
                    static_cast<int>(m_ctx->panels().console),
                    static_cast<int>(visible));
    }

    ImGui::SameLine();
    if (barButton("cmd", TR("CMD"), kBarIconCmd, m_showCommandPalette,
                  TR("Open the command console (Ctrl+Shift+P)."))) {
        // 命令面板是个"开着就占焦"的弹层：再点一次收掉，否则永远是"再点无效"
        m_showCommandPalette = !m_showCommandPalette;
        VK_LOG_INFO("CMD-PALETTE open=%d", static_cast<int>(m_showCommandPalette));
    }

    // ---------------------------------------------------------- 面板托盘
    //
    // 2026-10-17 晚（用户原话）："CMD 按钮的右侧加一个 + 的 logo 按钮，
    // 点击后上方打开一个子菜单，内容有一列勾选列表……勾选好 Character
    // 面板后，CMD 右侧就会出现 Character 对应的按钮（都是带 logo 一起的），
    // 点击按钮就会打开对应的功能面板……按钮也是可以点击右上角的叉号给
    // 移除掉的"。
    //
    // 落地：+ 按钮 → 向上的勾选列表（drawPanelPicker）；勾上的浮动面板
    // 在 + 右边各生成一个按钮（trayButton），按钮右上角画一个小 ×。
    ImGui::SameLine();
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float sz = ImGui::GetTextLineHeight() + 7.0f;
        m_trayPlusX = p.x;
        m_trayPlusY = p.y;
        m_trayBtnH = sz;
        if (ImGui::InvisibleButton("##panel_tray_plus", ImVec2(sz + 6.0f, sz))) {
            ImGui::OpenPopup("##panel_picker");
        }
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hovered) {
            dl->AddRectFilled(p, ImVec2(p.x + sz + 6.0f, p.y + sz),
                              IM_COL32(255, 255, 255, 26), 4.0f);
        }
        drawBarIcon(dl, ImVec2(p.x + 3.0f, p.y + (sz - 18.0f) * 0.5f), 18.0f,
                    kBarIconPlus);
        logRect("BAR-TRAY-PLUS", p, ImVec2(p.x + sz + 6.0f, p.y + sz));
        if (hovered) {
            ImGui::SetTooltip(TR("Add a panel button to this bar.\n"
                              "Tick a panel, and a button with its logo appears\n"
                              "to the right of +."));
        }
    }

    // 勾上的浮动面板 → 底栏按钮（顺序 = 面板表顺序，稳定不跳）
    for (int i = 0; i < panelCount(); ++i) {
        const PanelDesc& pd = panelTable()[i];
        if (!panelTrayEligible(pd)) continue;
        if (!panelOpen(i)) continue;
        ImGui::SameLine();
        char id[48];
        std::snprintf(id, sizeof(id), "tray_%s", pd.id);
        const TrayHit hit = trayButton(id, TR(pd.title), barIconForPanel(pd.id),
                                       !panelTrayHiddenRef(i),
                                       TR("Show / hide this panel.\n"
                                          "The × in the corner removes the button.\n"
                                          "Right-click for minimize / close."));
        switch (hit) {
            case TrayHit::Toggle:
                // 只收窗口，不动勾选 —— 按钮必须留着，否则"再点一下打开"
                // 就没有入口了（用户要的是按钮可点、叉号才移除按钮）。
                panelTrayHiddenRef(i) = !panelTrayHiddenRef(i);
                // 2026-10-21：标签页宿主还有第二层状态 —— 它是"开着"的，
                // 但视口里现在显示的是别的页。光把 trayHidden 清掉，画面
                // 一点变化都没有（用户会以为按钮坏了）。所以恢复可见时
                // 顺手切到那一页。
                if (!panelTrayHiddenRef(i)) setActiveViewportTab(i);
                VK_LOG_INFO("PANEL-TRAY %s open=%d", pd.id,
                            panelTrayHiddenRef(i) ? 0 : 1);
                break;
            case TrayHit::Remove:
                // 叉号 = 不仅关掉，还把按钮一起撤掉（等价于取消勾选）
                setPanelOpen(i, false);
                panelTrayHiddenRef(i) = false;
                VK_LOG_INFO("PANEL-TRAY %s removed=1", pd.id);
                break;
            case TrayHit::None:
                break;
        }
    }

    // 勾选列表必须每帧提交（不提交的那一帧 ImGui 会把弹层关掉）
    drawPanelPicker();

    // 右侧：资源统计（原来 Content 面板底行那一句，现在常驻）
    char stat[512];
    const std::string& rel = m_content->currentRel();
    std::snprintf(stat, sizeof(stat), TR("%zu entries  |  %s"),
                  m_content->entryCount(),
                  rel.empty() ? TR("(root)") : rel.c_str());
    const float tw = ImGui::CalcTextSize(stat).x;
    const float x = std::max(ImGui::GetWindowWidth() - tw - 12.0f,
                             ImGui::GetCursorPosX());
    ImGui::SameLine(x);
    ImGui::TextDisabled("%s", stat);
    logRect("BAR-STATS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void EditorApp::drawContentDrawer() {
    m_contentDrawerHovered = false;
    if (!m_ctx->panels().content) return;

    LayoutRects& L = m_ctx->layout();
    if (L.content.w < 120.0f || L.content.h < 32.0f) return;

    logRect("CONTENT-DRAWER", ImVec2(L.content.x, L.content.y),
            ImVec2(L.content.x + L.content.w, L.content.y + L.content.h));

    ImGui::SetNextWindowPos(ImVec2(L.content.x, L.content.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.content.w, L.content.h), ImGuiCond_Always);

    // 无标题栏：标题行自己画（要能在右上角摆 "Dock in layout"）。
    // 没有 NoMove/NoResize 之外的拖拽需求 —— 位置由布局每帧钉死，
    // 大小只由"上边缘拖拽"决定（见下面的 grip）。
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
    if (!ImGui::Begin("##contentdrawer", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }
    m_contentDrawerHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // ---- 上边缘：拖拽改高度 ----
    // 用 InvisibleButton 让 ImGui 帮我们管"按下 / 拖出 / 松开"；
    // 高度按**起始高度 + 累计位移**算（而不是 += delta.y）—— 后者在
    // 触摸板那种高频小步长下会累积误差，松手后与预期差几个像素。
    const float gripH = 6.0f;
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##drawer_grip",
                           ImVec2(ImGui::GetContentRegionAvail().x, gripH));
    if (ImGui::IsItemActive()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (!m_contentDragging) {
            m_contentDragging = true;
            m_contentDragStartH = m_contentDrawerH;
            m_contentDragStartY = io.MousePos.y;
        }
        const float dy = m_contentDragStartY - io.MousePos.y;
        m_contentDrawerH = std::clamp(m_contentDragStartH + dy,
                                      kContentDrawerMin,
                                      io.DisplaySize.y * 0.72f);
    } else {
        m_contentDragging = false;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    // 把手可见化：一条贴着窗口顶边的细线，悬停/拖拽时变强调色。
    // 没有它的话"上边缘能拖改高度"是完全隐形的（用户只能靠猜）。
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 g0 = ImGui::GetItemRectMin();
        const ImVec2 g1 = ImGui::GetItemRectMax();
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        const ImU32 lineCol =
            hot ? ImGui::GetColorU32(ui::tokens().accent)
                : ImGui::GetColorU32(ImGuiCol_Separator);
        dl->AddLine(ImVec2(g0.x, g1.y - 1.0f), ImVec2(g1.x, g1.y - 1.0f),
                    lineCol, 1.0f);
    }
    logRect("CONTENT-DRAWER-GRIP", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    // ---- 头部行 ----
    ImGui::TextUnformatted(TR("Content"));

    // 右上角："Dock in layout" + 关闭。
    // 勾上 = 抽屉变成布局的一部分（视口/层级随之变矮、点外部不关）；
    // 不勾 = 浮层（默认）。用户明确要求"默认不勾选"。
    const char* dockLbl = TR("Dock in layout");
    const float cbW =
        ImGui::CalcTextSize(dockLbl).x + ImGui::GetFrameHeight() + 10.0f;
    ImGui::SameLine(std::max(ImGui::GetWindowWidth() - cbW - 34.0f, 120.0f));
    if (ImGui::Checkbox(dockLbl, &m_contentDock)) {
        VK_LOG_INFO("CONTENT-DRAWER dock=%d", static_cast<int>(m_contentDock));
        if (m_contentDock) m_contentDragging = false;
    }
    logRect("CONTENT-DRAWER-DOCK", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("x"))) {
        m_ctx->panels().content = false;
        VK_LOG_INFO("CONTENT-DRAWER open=0 (close button)");
    }

    ImGui::Separator();
    m_content->drawBody();

    // ------------------------------------------------------------
    // 层级：抽屉必须稳定压在所有布局面板之上
    // ------------------------------------------------------------
    // ImGui 的绘制顺序 = g.Windows 的顺序，而**点中某个窗口会把它挪到
    // 顺序末尾**（FocusWindow → BringWindowToDisplayFront）。所以"后画的
    // 在上层"只在同一帧内成立：点一下视口，视口就跑到抽屉后面去了 ——
    // 表现是"抽屉上半截被视口盖住"（浮层抽屉正好压在视口下半部）。
    //
    // 这里每帧把抽屉重新提到最前。两条保护：
    //   ① **有弹出层时不动**（右键菜单 / 排序菜单 / 组合框下拉都在
    //      g.Windows 里，硬提前会把它们压在抽屉下面 —— 菜单直接看不见）；
    //   ② 停靠形态不提（它已经是布局的一部分，提上去会盖住底栏）。
    //
    // ⚠ 用的是 imgui_internal 的 BringWindowToDisplayFront：ImGui 公开 API
    //   里没有"只改绘制顺序、不动焦点"的入口（SetNextWindowFocus 会抢
    //   键盘焦点，紧跟着的输入框就会失焦）。
    if (!m_contentDock) {
        ImGuiWindow* self = ImGui::GetCurrentWindow();
        const bool anyPopup =
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                            ImGuiPopupFlags_AnyPopupLevel);
        if (!anyPopup) {
            ImGui::BringWindowToDisplayFront(self);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void EditorApp::serviceContentDrawerDismiss() {
    if (!m_ctx->panels().content) return;
    // 停靠形态是布局的一部分 —— 点别处当然不关。
    if (m_contentDock) return;

    // ------------------------------------------------------------
    // 弹层（右键菜单 / 排序菜单 / 组合框下拉）开着时的点击不算"点外部"
    // ------------------------------------------------------------
    // 抽屉里弹出的菜单盖在抽屉上方，此时 g.HoveredWindow 是那个 popup、
    // 不是抽屉 —— IsWindowHovered 于是返回 false。所以"菜单开着时点抽屉
    // 里别的地方"会同时发生两件事：关菜单 + 把整个抽屉一起关掉。
    //
    // 判定用**上一帧是否还有弹层**：ImGui 在 NewFrame 里就把"点了外部"
    // 的弹层关掉了，等到这里（帧末）IsPopupOpen 已经是 false —— 只看当前
    // 帧状态抓不到。IDE 的抽屉/浮层都是"第一下只关菜单"，这里照抄。
    const bool popupNow =
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                        ImGuiPopupFlags_AnyPopupLevel);
    if (m_contentPopupWasOpen && !popupNow) {
        m_contentPopupWasOpen = false;
        return;   // 这一下点击的用途是"关菜单"
    }
    m_contentPopupWasOpen = popupNow;

    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    // 点在抽屉自己身上、或点在底栏的按钮上（那一帧正是它把抽屉开出来的）
    // 都不算"外部"。后者不加的话：点「内容」→ 同帧末尾判定为外部 → 立刻关掉。
    if (m_contentDrawerHovered || m_bottomBarHovered) return;

    m_ctx->panels().content = false;
    VK_LOG_INFO("CONTENT-DRAWER closed=outside");
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
    // ⚠ 走 token（同 Toolbar.cpp 的理由）：浅色主题下硬编码会留一条深色孤岛。
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::tokens().barBg);
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
            ? TRL("Stop the running game (editor scene was never touched).")
            : TRL("Run the game in the viewport (logic runs on a copy;\n"
              "the editor scene is left untouched).  Space"));
    }

    // ---- 动画 Play / Pause / Stop ----
    ImGui::SameLine();
    ImGui::TextDisabled(TR("|"));

    ImGui::SameLine();
    ImGui::BeginDisabled(runActive || m_ctx->animPlaying());
    ImGui::PushStyleColor(ImGuiCol_Button, kPlayCol);
    if (ImGui::Button(TR("Play"), ImVec2(btnW("Play"), 28.0f))) m_ctx->animPlay();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !runActive) {
        ImGui::SetTooltip(TR("Run scripts as an animation **on the editing "
                          "scene**.\nStop restores the scene from a snapshot."));
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!m_ctx->animPlaying());
    ImGui::PushStyleColor(ImGuiCol_Button, kPauseCol);
    if (ImGui::Button(TR("Pause"), ImVec2(btnW("Pause"), 28.0f)))
        m_ctx->animPause();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Freeze the animation (state is kept)."));
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!m_ctx->animActive());
    ImGui::PushStyleColor(ImGuiCol_Button, kStopCol);
    if (ImGui::Button(TR("Stop"), ImVec2(btnW("Stop"), 28.0f))) m_ctx->animStop();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Stop the animation and restore the scene to\n"
                          "how it looked when Play was pressed."));
    }

    // ================================================================
    // E08：时间轴 / 帧号 / 倍速 / 单帧步进（左侧区域）
    // ================================================================
    //
    // ⚠⚠ 这一整块**必须是灰阶配色**。
    //   tools/verify_add_menu.py 的断言 2 是这样做的：在 TR-BAR 的 x 区间内
    //   扫这一行的高饱和（max-min > 45）像素，取它们的 [min,max] 中点，断言
    //   它落在 TR-BAR 水平正中 —— 也就是那四个彩色按钮组的中心。任何**彩色**
    //   控件出现在这一行**且落在 TR-BAR 里**都会把中点拽偏，于是那条断言
    //   误报 FAIL（而按钮其实一动没动）。所以下面所有 PushStyleColor 都刻意
    //   只给灰阶（max-min ≈ 6）。
    //
    //   （2026-10-16 修：该断言原先扫**整窗宽度**，于是右侧 Inspector 的
    //     R/G/B 轴标签也能把它拽偏 —— 实测组中心被拽到 x=1020、条中心 774，
    //     而 TR-BAR 之内其实只有 x∈[630,853]。现在扫描区间已夹进 TR-BAR。）
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
        // ⚠ 灰阶**不变**，但明度必须跟主题走：浅色主题下那片 0.20 的深灰片
        //   配上深色正文 = 看不清。两条分支都是 max-min ≤ 6 的灰，所以上面
        //   那条"高饱和像素中点居中"的断言在两种主题下都成立。
        //   VennDark 走 else 分支 = 逐字段等于改动前的字面量。
        const bool lt = ui::tokens().isLight;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              lt ? ImVec4(0.80f, 0.81f, 0.83f, 1.0f)
                                 : ImVec4(0.20f, 0.21f, 0.23f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              lt ? ImVec4(0.74f, 0.75f, 0.78f, 1.0f)
                                 : ImVec4(0.26f, 0.27f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              lt ? ImVec4(0.70f, 0.71f, 0.74f, 1.0f)
                                 : ImVec4(0.30f, 0.31f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,
                              lt ? ImVec4(0.60f, 0.61f, 0.64f, 1.0f)
                                 : ImVec4(0.42f, 0.43f, 0.46f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,
                              lt ? ImVec4(0.50f, 0.51f, 0.54f, 1.0f)
                                 : ImVec4(0.52f, 0.53f, 0.56f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,
                              lt ? ImVec4(0.84f, 0.85f, 0.87f, 1.0f)
                                 : ImVec4(0.17f, 0.18f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                              lt ? ImVec4(0.78f, 0.79f, 0.82f, 1.0f)
                                 : ImVec4(0.23f, 0.24f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                              lt ? ImVec4(0.74f, 0.75f, 0.78f, 1.0f)
                                 : ImVec4(0.27f, 0.28f, 0.31f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(st.FramePadding.x, 7.0f));

        float x = st.WindowPadding.x;

        // ---- 帧号 + 时钟读数 ----
        {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "f%-6d %.2fs",
                          static_cast<int>(clockT * 60.0f + 0.5f), clockT);
            ImGui::SetCursorPos(ImVec2(x, rowY + 4.0f));
            ImGui::TextDisabled(TR("%s"), buf);
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
                ImGui::SetTooltip(TR("Playback speed (applies to scripts, "
                                  "players and collision)"));
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
            if (ImGui::Button(TR("Step"), ImVec2(56.0f, 28.0f))) {
                const float oneFrame = 1.0f / 60.0f;
                m_ctx->advanceAnimTime(oneFrame);
                runAnimScripts(oneFrame);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(canStep
                    ? TRL("Advance the animation by exactly one frame (1/60 s)")
                    : TRL("Pause the animation first (Play/Pause/Stop row)"));
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
            if (ImGui::SliderFloat("##timeline", &scrub, 0.0f, tmax, TR("%.2f s"))) {
                if (m_ctx->isPlaying()) m_ctx->setPlayTime(scrub);
                else m_ctx->setAnimTime(scrub);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Timeline: drag to scrub the script clock.\n"
                                  "A real sequencer (keyframes / tracks) "
                                  "lands with E24."));
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

    // 2026-10-21：Stats 是视口标签页的一页（见 PanelDock.h）
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(w660, 0.0f), ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Stats", "panel_stats"),
                      docked ? nullptr : &m_ctx->panels().stats, wf)) {
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
    ImGui::Text(TR("FPS: %.1f  (%.2f ms)"), io.Framerate, ftMs);
    ImGui::PlotLines("##ft-hist", ftHist, 120, 0, "frame time (ms) / 120f",
                     0.0f, 50.0f, ImVec2(-1.0f, 40.0f));
    ImGui::PlotLines("##fps-hist", fpsHist, 120, 0, "fps / 120f", 0.0f, 240.0f,
                     ImVec2(-1.0f, 40.0f));

    // ---- E15 CPU / GPU 双栏 ----
    ImGui::Columns(2, "statscols", false);
    ImGui::SetColumnWidth(0, w660 * 0.56f);

    // ================= 左栏：CPU 侧 =================
    const scene::Scene& sc = m_ctx->activeScene();
    ImGui::Text(TR("Viewport scene: %s"),
                m_ctx->isEditing() ? "EDITOR" : "RUNTIME (playing)");
    ImGui::Text(TR("Entities: %zu"), sc.objectCount());

    const scene::Scene::LightStats ls = sc.lightStats();
    ImGui::Text(TR("Local lights: %zu (%zu point / %zu spot)"),
                ls.pointLights + ls.spotLights, ls.pointLights, ls.spotLights);

    ImGui::Separator();
    const VkExtent2D re = r.renderExtent();
    ImGui::Text(TR("Render target: %ux%u%s"), re.width, re.height,
                r.viewportActive() ? " (offscreen -> ImGui)" : " (swapchain)");
    ImGui::Text(TR("MSAA: %ux"), static_cast<unsigned>(r.sampleCount()));
    ImGui::Text(TR("Depth prepass: %s"), r.depthPrePassEnabled() ? "on" : "off");

    // ---- 物体级视锥剔除的实际效果 ----
    // "drawn / total"：实际提交的物体数 / 场景里可见的网格实体总数。
    // culled 为两者之差；draw calls 与 triangles 是提交量（不是着色量），
    // 正是视锥剔除要压下去的那部分 CPU 提交开销。
    {
        const render::Renderer::RenderStats& rs = r.renderStats();
        const uint32_t drawn = rs.drawnOpaque + rs.drawnTransparent;
        ImGui::Text(TR("Objects: %u drawn / %u"), drawn, rs.renderables);
        if (!r.frustumCullingEnabled()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                               TR("[frustum culling off]"));
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled(TR("(culled %u, lights %u)"), rs.culledFrustum,
                                rs.gizmosCulled);
        }
        // Hi-Z 遮挡剔除：occl = 本帧因"被挡住"没进绘制列表的物体数；
        // tested = 两帧前那次 GPU 实际测过的候选数（上限 kMaxObjects）。
        if (r.occlusionCullingEnabled()) {
            const render::HiZStats& hs = r.hizStats();
            ImGui::SameLine();
            ImGui::TextDisabled(TR("| Hi-Z tested %u, occluded %u (%u mips)"),
                                hs.objects, rs.culledOcclusion, hs.mipCount);
        }
        ImGui::Text(TR("Draw calls: %u   Triangles: %u"), rs.drawCalls,
                    rs.triangles);

        // ---- 空间索引（BVH）----
        // visits = 本帧视锥查询**实际访过**的节点数。它远小于 nodes 才说明
        // 子树真的被剪掉了；如果逼近 nodes，说明树建歪了（退化成了全遍历）。
        const scene::Scene::SpatialStats bs =
            m_ctx->activeScene().spatialStats();
        ImGui::Text(TR("BVH: %llu nodes / %llu objects / depth %llu, visits %u"),
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
            ImGui::Text(TR("LOD: %u ents  L0 %u / L1 %u / L2 %u / L3 %u"),
                        rs.lodEntities, rs.lodLevelCounts[0],
                        rs.lodLevelCounts[1], rs.lodLevelCounts[2],
                        rs.lodLevelCounts[3]);
            ImGui::SameLine();
            ImGui::TextDisabled(TR("(tris %u vs %u, -%.0f%%)"), rs.triangles,
                                rs.trianglesNoLod,
                                static_cast<double>(saved));
        } else {
            ImGui::TextDisabled(TR("LOD: no visible objects with an LOD chain"));
        }

        // ---- 环境（天空盒 + IBL）----
        if (const render::Environment* env = r.environment()) {
            const render::Environment::Stats& es = env->stats();
            ImGui::Text(TR("Env: sky %u / irr %u / pref %u x%u mips / lut %u"),
                        es.skySize, es.irradianceSize, es.prefilterSize,
                        es.prefilterMips, es.lutSize);
            ImGui::SameLine();
            ImGui::TextDisabled(TR("(sky %s, IBL %s)"),
                                r.skyEnabled() ? "on" : "off",
                                r.iblEnabled() ? "split-sum" : "approx");
        } else {
            ImGui::TextDisabled(TR("Env: environment maps not ready"));
        }

        // ---- 级联阴影（CSM）----
        if (const render::ShadowPass* sp = r.shadowPass()) {
            ImGui::Text(TR("Shadow: %u casc %ux%u  dist %.0fm  %s"),
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
                TR("splits %.1f / %.1f / %.1f / %.1f | submitted %u of %u "
                "(per-cascade cull)"),
                static_cast<double>(sp->split(0)),
                static_cast<double>(sp->split(1)),
                static_cast<double>(sp->split(2)),
                static_cast<double>(sp->split(3)), sub, con);
        }

        // ---- 局部光源阴影（点光 / 聚光灯）----
        if (const render::LocalShadowPass* lsp = r.localShadowPass()) {
            ImGui::Text(TR("Local shadow: %ux%u x %u layers, used %u  %s"),
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
                TR("submitted %u of %u (per-layer cull) | max %u lamps"),
                sub, con, r.localShadowMaxLights());
        }

        // ---- Shader 变体 / 关键字（见 render/ShaderKeywords.h）----
        // 第一行是"惰性创建有没有失控"的体温计：一个只用得到 3 种材质组合
        // 的场景应当稳定停在 3；数字随帧数一直涨就说明缓存根本没命中。
        // switches 是每帧真的发生了几次 vkCmdBindPipeline —— 同材质的相邻
        // 物体只付一次，所以它通常远小于绘制数。
        if (r.forwardPipelines().initialized()) {
            ImGui::Text(TR("Shader vars: %u fwd + %u depth, %u switches"),
                        r.forwardPipelines().created(),
                        r.depthPrePipelines().created(), rs.variantSwitches);

            std::string vars, tip;
            // 稀疏表遍历（见 RenderStats::variantCounts）——
            // 原来的 `for m <= kKeywordAll` 在位宽增长后会变成扫几百个空槽，
            // 而且用 `variantCounts[m]` 当下标数组的写法本身就不再成立。
            for (uint32_t i = 0; i < rs.variantSlotCount; ++i) {
                const render::VariantSlot& slot = rs.variantCounts[i];
                if (slot.count == 0) continue;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%sk%u:%u",
                              vars.empty() ? "" : " ", slot.mask, slot.count);
                vars += buf;

                char tbuf[96];
                std::snprintf(tbuf, sizeof(tbuf), "k%u  %s  x%u\n", slot.mask,
                              render::keywordNames(slot.mask).c_str(),
                              slot.count);
                tip += tbuf;
            }
            ImGui::TextDisabled(TR("%s"), vars.empty() ? TR("(no forward variant draws this frame)")
                                                   : vars.c_str());
            // 掩码是位集合，"k5" 这种编号不看名字是读不出来的 → 悬停给出映射。
            if (!tip.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip(TR("Objects drawn per forward variant this frame:\n%s"
                                  "\nbit0 double-sided / bit1 alpha-blend / "
                                  "bit2 normal-map / bit6 toon"), tip.c_str());
            }

            // 强制掩码：对照实验留下的印记。非 -1 时画面是被"篡改"过的，
            // 必须显眼 —— 否则看到的人会以为材质开关失效了。
            if (r.forcedKeywordMask() >= 0) {
                const auto fm = static_cast<render::KeywordMask>(
                    r.forcedKeywordMask());
                ImGui::TextColored(
                    ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                    TR("FORCED mask %d: %s"), r.forcedKeywordMask(),
                    render::keywordNames(fm).c_str());
            }
        }
    }
    ImGui::TextWrapped(TR("GPU: %s"), r.device().properties().deviceName);

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
    ImGui::TextColored(pbCol, TR("Playback: %s (in viewport)%s"), pbState,
                       (m_ctx->gameFullscreen() && !m_ctx->isEditing())
                           ? "  [fullscreen]"
                           : "");
    if (!m_ctx->isEditing()) {
        ImGui::Text(TR("Game time: %.2fs"),
                    static_cast<double>(m_ctx->playTime()));
        ImGui::TextDisabled(TR("Camera owned by the game (viewport takes no input)"));
    }
    // 视口导航灵敏度：编辑态相机的移动 / 旋转系数（右上角 HUD 可改）
    ImGui::Text(TR("View sensitivity: x%.2f"),
                static_cast<double>(m_ctx->navSensitivity()));
    ImGui::Text(TR("Editor %zu / runtime %zu entities"),
                m_ctx->editorScene().objectCount(),
                m_ctx->runtimeScene().objectCount());
    ImGui::Text(TR("Scripts loaded: %zu"), m_scripts->scriptCount());

    // ================= 右栏：GPU 侧（时戳剖析）=================
    ImGui::NextColumn();
    if (const render::GpuTimer* gt = r.gpuTimer()) {
        if (gt->valid() && gt->lastTotalMs() >= 0.0f) {
            const float total = gt->lastTotalMs();
            ImGui::Text(TR("GPU: %.2f ms/frame"), total);

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
                        ImGui::SetTooltip(TR("%s  %.3f ms  (%.1f%%)"),
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
                    ImGui::Text(TR("%s %.2f"), gt->sectionName(s),
                                static_cast<double>(ms));
                } else {
                    ImGui::ColorButton("##chip", ImVec4(0.3f, 0.3f, 0.33f, 1.0f),
                                       ImGuiColorEditFlags_NoTooltip,
                                       ImVec2(10.0f, 10.0f));
                    ImGui::SameLine();
                    ImGui::TextDisabled(TR("%s -"), gt->sectionName(s));
                }
            }
        } else {
            ImGui::TextDisabled(TR("GPU: timestamp data not ready"));
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

    // 2026-10-21：视口标签页的一页（见 PanelDock.h）
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(kFloatW, kScriptsH),
                                 ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Scripts", "panel_scripts"),
                      docked ? nullptr : &m_ctx->panels().scripts, wf)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled(TR("Language: .vks (see engine/script/ScriptEngine.h)"));
    if (ImGui::Button(TR("Reload all"))) {
        const int n = m_scripts->pollHotReload();
        for (const std::string& p : m_scripts->loadedPaths()) {
            m_scripts->reloadNow(p);
        }
        m_ctx->notify(std::string(TR("Reloaded ")) + std::to_string(n) +
                      TR(" changed script(s), forced all"));
    }
    ImGui::SameLine();
    ImGui::Text(TR("hot-reload scan: 0.5s"));

    ImGui::Separator();
    if (ImGui::BeginTabBar("##scripttabs")) {
        if (ImGui::BeginTabItem(TRL("Loaded"))) {
            for (const std::string& p : m_scripts->loadedPaths()) {
                vks::ScriptAsset* a = m_scripts->get(p);
                if (!a) continue;
                const bool bad = !a->lastError().ok();
                ImGui::TextColored(bad ? ImVec4(1.0f, 0.5f, 0.45f, 1.0f)
                                       : ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                                   TR("%s  (#%llu)"), p.c_str(),
                                   static_cast<unsigned long long>(
                                       a->reloadCount()));
                if (bad) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(TR("<- %s"),
                                        a->lastError().toString().c_str());
                }
                ImGui::SameLine();
                ImGui::PushID(p.c_str());
                if (ImGui::SmallButton(TR("reload"))) m_scripts->reloadNow(p);
                ImGui::PopID();
            }
            if (m_scripts->scriptCount() == 0)
                ImGui::TextDisabled(TR("(the scene references no scripts)"));
            ImGui::EndTabItem();
        }
        // ★ S5：Instances 页签 —— 脚本从"编辑器看不见"变成"能看见、能重置"。
        //   这一页是五件套里的「UI 面」：没有它，`wait` 挂住、变量没涨、
        //   熔断不出声，全都只能靠日志。
        // `MYVK_EDITOR_SCRIPT_TAB=instances` 让这一页默认打开。
        // ⚠ 只**选中一次**：每帧都 SetSelected 的话用户切到 Log 会立刻被弹回来。
        static const bool kWantInstancesTab = [] {
            const char* v = std::getenv("MYVK_EDITOR_SCRIPT_TAB");
            return v && std::string(v) == "instances";
        }();
        const ImGuiTabItemFlags instFlags =
            (kWantInstancesTab && !m_scriptTabForced)
                ? ImGuiTabItemFlags_SetSelected
                : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(TRL("Instances"), nullptr, instFlags)) {
            m_scriptTabForced = true;
            const auto& insts = m_scriptRuntime->instances();
            int nRun = 0, nWait = 0, nFail = 0, nIdle = 0;
            for (const auto& ip : insts) {
                const char* st = m_scriptRuntime->stateOf(*ip);
                if (st[0] == 'R') ++nRun;
                else if (st[0] == 'W') ++nWait;
                else if (st[0] == 'F') ++nFail;
                else ++nIdle;
            }
            ImGui::TextDisabled(TR("%d instance(s): %d running, %d waiting, "
                                   "%d idle, %d failed"),
                                static_cast<int>(insts.size()), nRun, nWait,
                                nIdle, nFail);
            ImGui::Separator();

            if (insts.empty()) {
                ImGui::TextDisabled(
                    TR("(none - add a Script component to an object)"));
            }
            for (std::size_t i = 0; i < insts.size(); ++i) {
                vks::ScriptInstance& in = *insts[i];
                ImGui::PushID(static_cast<int>(i));

                const char* st = m_scriptRuntime->stateOf(in);
                const bool bad = in.failed;
                const bool waiting = in.waitingNow;
                // 四种状态四种颜色 —— `Idle` 与 `Running` **必须**不同色，
                // 否则 F2 的"像素双证"没法做（两个都是绿的，截图一样）。
                const ImVec4 col =
                    bad ? ImVec4(1.0f, 0.42f, 0.40f, 1.0f)
                        : waiting ? ImVec4(1.0f, 0.85f, 0.40f, 1.0f)
                        : st[0] == 'I' ? ImVec4(0.62f, 0.62f, 0.66f, 1.0f)
                                       : ImVec4(0.48f, 0.90f, 0.52f, 1.0f);

                // 实体名（优先用场景里的名字；实体没了就退回 #id）
                const std::string* nm =
                    m_ctx->runtimeScene().world().name(in.entity);
                char label[192];
                std::snprintf(label, sizeof(label), "%s",
                              nm ? nm->c_str() : "<no entity>");
                ImGui::TextColored(col, "%s", label);
                // ⚠ 锚点必须**贴着控件**取：再往后 SameLine 一改，
                //   GetItemRect* 就变成按钮的矩形了。
                logRect(("SCR-INST-" + std::to_string(i)).c_str(),
                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

                // ⚠⚠ 这一行**只有**「名字 + 状态 + Reset 按钮」，脚本路径、
                //   本地时间、变量数全部挪到第二行。
                //   它们是硬塞不完的：路径是完整绝对路径（自动化下是
                //   `...\Temp\venn_script_ed_xxxx\boss.vks`），一挤就把 Reset
                //   推出窗口右边缘 —— 控件被裁掉之后 `GetItemRect*` **照样
                //   返回真实坐标**，于是脚本拿着一个屏幕外的坐标去点，
                //   命中 0 次、日志 0 行，而面板看起来"按钮明明在"。
                //   （S5 的 F4/F5c 就是死在这里。）
                ImGui::SameLine();
                ImGui::TextColored(col, "%s", st);
                ImGui::SameLine();
                if (ImGui::SmallButton(TR("Reset"))) {
                    const std::size_t before = in.vars.size();
                    m_scriptRuntime->resetInstance(in.entity);
                    // 自动化读这一行（F4）：`vars_after` 必须是 0 ——
                    // 只断言"点了按钮"是不够的，"清了没清"才是要点。
                    VK_LOG_INFO("SCR-RESET ok=1 source=panel entity=%u "
                                "name=%s vars_before=%zu vars_after=%zu",
                                in.entity.id, label, before, in.vars.size());
                    m_ctx->notify(std::string(TR("Reset script on ")) + label);
                }
                // 路线图里写的锚点是 `SCR-INST-RESET`（无下标）。先打无下标的
                // 保证那个名字存在，再打带下标的给"点第 N 个"用。
                if (i == 0) {
                    logRect("SCR-INST-RESET", ImGui::GetItemRectMin(),
                            ImGui::GetItemRectMax());
                }
                logRect(("SCR-INST-RESET-" + std::to_string(i)).c_str(),
                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

                // 第二行：文件名（不是全路径）+ 本地时间 + 变量数。
                // 面板只有 360 px 宽，全路径一定被裁；名字够认出来就够了，
                // 要全路径看 Loaded 页签或 Inspector。
                {
                    const std::string& p = in.path;
                    const size_t slash = p.find_last_of("/\\");
                    const std::string base =
                        slash == std::string::npos ? p : p.substr(slash + 1);
                    // ⚠ 格式串也要包 TR：`vars=` / `t=` 是屏幕上实打实的
                    //   英文（i18n 体检 B 组会红）。占位符序列必须与原串
                    //   完全一致 —— 这条是 F 组（崩溃级）的判据。
                    ImGui::TextDisabled(TR("   %s  t=%.2fs  vars=%zu"),
                                        base.c_str(),
                                        static_cast<double>(in.localTime),
                                        in.vars.size());
                }

                if (bad && !in.lastError.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.42f, 1.0f), "%s",
                                       in.lastError.c_str());
                }

                // 变量查看器（只读）。默认收起 —— 一个实例可能有几十个变量。
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.72f, 0.72f, 0.75f, 1.0f));
                const bool open = ImGui::TreeNodeEx(
                    "##vars", ImGuiTreeNodeFlags_SpanAvailWidth, "vars (%zu)",
                    in.vars.size());
                ImGui::PopStyleColor();
                if (open) {
                    const auto rows = m_scriptRuntime->sortedVars(in);
                    for (const auto& kv : rows) {
                        ImGui::TextDisabled("%s = %s", kv.first.c_str(),
                                            kv.second.c_str());
                    }
                    if (rows.empty()) ImGui::TextDisabled("(empty)");
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TRL("Log"))) {
            ImGui::BeginChild("##log");
            const auto& log = m_scripts->log();
            for (const std::string& s : log) ImGui::TextWrapped(TR("%s"), s.c_str());
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TRL("How it works"))) {
            ImGui::BeginChild("##howto");
            ImGui::TextWrapped(
                TR("In edit mode scripts are only PRE-COMPILED: syntax errors "
                "show up immediately, but nothing runs and nothing animates."));
            ImGui::Spacing();
            ImGui::TextWrapped(
                TR("Press Play and they execute every frame on the runtime copy "
                "of the scene - which is exactly what the viewport renders. "
                "Stop discards that copy, so the scene you edited is never "
                "touched."));
            ImGui::Spacing();
            ImGui::TextWrapped(
                TR("Hot reload: save the .vks file and it is picked up within "
                "0.5s, even while playing."));
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
    // 2026-10-21：视口标签页的一页（见 PanelDock.h）
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x - kFloatW - kFloatMargin,
                   io.DisplaySize.y - kStatusH - kScriptsH),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(kFloatW, kScriptsH),
                                 ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Engine", "panel_engine"),
                      docked ? nullptr : &m_ctx->panels().engine, wf)) {
        ImGui::End();
        return;
    }

    // ---- 顶栏：档位（100+ 旋钮平铺没人能用，默认只给 Basic）+ 搜索 ----
    static int tier = 0;               // 0=Basic 1=Advanced 2=Debug
    static char search[64] = "";
    ImGui::TextUnformatted(TR("Show"));
    ImGui::SameLine();
    ImGui::RadioButton(TR("Basic"), &tier, 0);
    ImGui::SameLine();
    ImGui::RadioButton(TR("Adv"), &tier, 1);
    ImGui::SameLine();
    ImGui::RadioButton(TR("Debug"), &tier, 2);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ftrsearch", TRL("Search features / params..."),
                             search, sizeof(search));

    // ---- 画质预设（E09；质量参数的档位化是 E26 的数据活，这里只拨开关）----
    static int preset = -1;            // -1 = Custom
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo(
            TRL("Preset"), preset < 0 ? "Custom" : editor::qualityPresetName(preset))) {
        for (int i = 0; i < editor::qualityPresetCount(); ++i) {
            if (ImGui::Selectable(editor::qualityPresetName(i), preset == i)) {
                preset = i;
                editor::applyQualityPreset(i);
            }
        }
        ImGui::EndCombo();
    }

    // ---- E03：设置文件（覆盖链概览 + Export/Reload）----
    // 放在生成区**之前**：它管的是整份文件，不该沉到三十多个特性块后面。
    drawEngineSettingsIO();

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

// ------------------------------------------------ E03 设置层级（覆盖链）

void EditorApp::drawEngineSettingsIO() {
    render::SettingsChain& chain = render::activeChain();
    const std::string path = render::projectSettingsPath();
    const render::SettingsLayer* proj =
        chain.layer(render::SettingScope::Project);

    // 刻意做得**很矮**（一行按钮 + 一行状态）：它放在面板**顶部**是有理由的
    // —— 放到三十多个特性块后面就得靠滚动条才找得到，而它管的是"整份文件"。
    ImGui::SeparatorText(TR("Settings file"));
    if (ImGui::Button(TR("Export current"))) saveRenderSettings(true);
    logRect("SET-EXPORT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Write every non-default parameter to:\n%s"),
                          path.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Reload from disk"))) render::reloadFromDisk();
    logRect("SET-RELOAD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Re-read the file and apply it now (unsaved "
                             "edits are lost)."));
    }
    ImGui::SameLine();
    if (proj && proj->present) {
        ImGui::Text(TR("%zu override(s)"), proj->values.size());
        if (proj->rejected > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f),
                               TR("(%d rejected — see Console)"),
                               proj->rejected);
        }
    } else {
        ImGui::TextDisabled(TR("no file — all defaults"));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Chain: %s"), chain.describe().c_str());
    }
}

// E03：落盘。
// `explicitExport == false` 是"用户改过参数"的自动保存（关闭时一次），
// true 是面板上的 Export 按钮。两者都受同一条守卫约束：
// **自动化运行一律不写** —— 否则一次测试跑就会在仓库里留下一个 json，
// 把所有像素基线悄悄换掉。
void EditorApp::saveRenderSettings(bool explicitExport) {
    if (render::renderSettingsAutomationRun()) {
        VK_LOG_INFO("RENDER-SET not saved (automation run)");
        return;
    }
    render::SettingsLayer layer =
        render::captureFromRegistry(render::SettingScope::Project);
    if (layer.empty() && !explicitExport) {
        m_renderSettingsDirty = 0;
        return;   // 全默认 → 不生成文件（"无文件 = 无覆盖"的语义）
    }
    layer.source = render::projectSettingsPath();
    if (render::saveLayerFile(layer, layer.source)) {
        m_renderSettingsDirty = 0;
        // 让面板上的"覆盖数"立刻反映刚写下去的内容
        render::activeChain().setLayer(layer);
    }
}

// ---------------------------------------- E09 特殊块（生成器表达不了的）

void EditorApp::drawEngineCameras() {
    // 多相机：主视口用场景里的哪台相机（0 = 默认）。相机数量由场景决定，
    // 编辑器可以在这里追加（新相机从 0 号复制，改它的位置/朝向即可）。
    render::Renderer& r = renderer();
    scene::Scene& sc = activeScene();
    int camIdx = static_cast<int>(r.viewCameraIndex());
    const int camCount = std::max(1, static_cast<int>(sc.cameraCount()));
    ImGui::SeparatorText(TR("Cameras"));
    ImGui::Text(TR("Cameras: %d"), camCount);
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Add"))) sc.addCamera();
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Copy 1st -> current"))) {
        if (sc.cameraCount() > 1) {
            sc.camera(static_cast<size_t>(camIdx)) = sc.camera(0);
        }
    }
    if (ImGui::SliderInt(TRL("View camera"), &camIdx, 0, camCount - 1)) {
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
    if (ImGui::Combo(TR("View mode"), &vmIdx, TrItems<8>(kViewModeLabels), 8)) {
        r.setViewMode(static_cast<render::Renderer::ViewMode>(vmIdx));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            TR("Lit = default rendering (baseline)\n"
            "Wireframe requires device support for fillModeNonSolid"));
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
    if (ImGui::Combo(TRL("MSAA##samples"), &msaaIdx, TrItems<4>(kMsaaLabels), 4)) {
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
    ImGui::SeparatorText(TR("Cluster stats"));
    const render::ClusterStats& cs = r.clusteredLighting().stats();
    const render::ClusterConfig& cfg = r.clusteredLighting().config();
    ImGui::Text(TR("Grid: %u x %u x %u (tile %u px)"), cs.clusterX, cs.clusterY,
                cs.clusterZ, cfg.tileSize);
    ImGui::Text(TR("Lights/cluster: avg %.2f  max %u / %u"),
                cs.avgLightsPerCluster(), cs.maxLightsInCluster,
                cfg.maxLightsPerCluster);
    ImGui::Text(TR("Empty clusters: %u"), cs.emptyClusters);
    if (cs.overflowed())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                           TR("Overflow: %u clusters truncated!"),
                           cs.overflowClusters);
}

// ---------------------------------------------------------------- E02 钩子

void EditorApp::onFeatureParamChanged(const char* paramId) {
    VK_LOG_INFO("FTR-UI %s", paramId);
    // E03：用户真的动过参数 → 关闭时要落盘（"首次改参数时才写文件"）。
    ++m_renderSettingsDirty;
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

    con->addCommand("help", "List all commands", [con](const std::vector<std::string>&) {
        return std::string(
            "help / clear / set <id> <v> / get <id> / reset <id|feature> / "
            "params <feature> / preset <Low|Medium|High|Epic|Cinematic> / "
            "stat gpu / insp.selftest / undo.selftest / notif.push|dump|click|"
            "task|progress|cancel|done");
    });
    con->addCommand("clear", "Clear the log buffer",
                    [con](const std::vector<std::string>&) {
                        con->clear();
                        return std::string();
                    });
    con->addCommand("set", "set <paramId> <value>  write a registry param (a feature id means <id>.enabled)",
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
    con->addCommand("get", "get <paramId>  read the current value of a registry param",
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
    con->addCommand("reset", "reset <paramId|featureId>  restore defaults",
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
    con->addCommand("params", "params <featureId>  list every param of a feature",
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
    // 定格动画时间 / 切换 clip（角色链 A0 的像素断言入口）。
    //
    // 为什么需要：`MYVK_EDITOR_IMPORT` 能把模型导进来，但"存盘 → 重开 →
    // 蒙皮字段还在"这个往返必须能无人值守地跑；同理"时间真的驱动了姿势"
    // 也要能无人值守地钉住。快捷键 Ctrl+S 走的是同一条 saveSceneNow()，
    // 所以这里不是"测试专用路径"，只是给脚本一个入口。
    //
    // 为什么要专门钉时间：Idle 这类待机动画幅度极小（实测 0.4 秒内两帧只差
    // 0.02/255），拿"自然播放的两帧"做像素断言没有判别力。而"同进程同相机
    // 同 HUD，只改 clip/time 再各截一张"的差异只可能来自姿势 —— 这才是
    // "时间 → 关节矩阵 → 屏幕"的干净因果。anime_girl 带 4 个 clip
    // （Idle / Walk / Rig|Idle / Rig|Walk），所以还可以用 Walk 比对 Idle。
    //
    // 参数末尾可选 `<name>`：不给就作用于当前选中项，给了就按实体名找 ——
    // A0-4 的否定测试要"同一个 FBX 的三个实例各钉不同时间"，选中集只有一个，
    // 没有这个参数就改不到第二、三个。
    auto resolveSkinned = [this](const std::vector<std::string>& args, size_t at,
                                 ecs::Entity& outE,
                                 ecs::SkinnedMeshComponent*& outSk,
                                 std::string& err) -> bool {
        ecs::Entity e;
        if (args.size() > at) {
            e = m_ctx->activeScene().find(args[at]);
            if (!e.valid()) {
                err = "entity not found: " + args[at];
                return false;
            }
        } else {
            e = m_ctx->selection();
        }
        auto* sk =
            m_ctx->activeScene().world().get<ecs::SkinnedMeshComponent>(e);
        if (!sk) {
            err = "no SkinnedMeshComponent on entity " +
                  std::to_string(static_cast<unsigned>(e.id));
            return false;
        }
        outE = e;
        outSk = sk;
        return true;
    };
    // ---- 脚本（S5）----
    // 这三条是"五件套里 UI 面"的控制台一半：面板能看，但自动化只能靠命令。
    // ⚠ 日志前缀统一 `SCR-`，且**成功/失败都打**（失败不打 = 自动化以为命令
    //   没执行，会去怀疑注册而不是选择集）。
    con->addCommand("script.reset",
        "script.reset <entityName>  reset one script instance (vars + coroutine)",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: script.reset <entityName>";
            ecs::World& w = m_ctx->activeScene().world();
            ecs::Entity found{};
            w.each<ecs::ScriptComponent>([&](ecs::Entity e,
                                             ecs::ScriptComponent&) {
                const std::string* n = w.name(e);
                if (!found.valid() && n && *n == args[1]) found = e;
            });
            if (!found.valid()) {
                VK_LOG_WARN("SCR-RESET ok=0 reason=not-found name=%s",
                            args[1].c_str());
                return "no script instance on '" + args[1] + "'";
            }
            m_scriptRuntime->resetInstance(found);
            VK_LOG_INFO("SCR-RESET ok=1 entity=%u name=%s",
                        static_cast<unsigned>(found.id), args[1].c_str());
            return "reset " + args[1];
        });
    con->addCommand("script.vars",
        "script.vars <entityName>  dump one instance's variables (sorted)",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: script.vars <entityName>";
            ecs::World& w = m_ctx->activeScene().world();
            ecs::Entity found{};
            w.each<ecs::ScriptComponent>([&](ecs::Entity e,
                                             ecs::ScriptComponent&) {
                const std::string* n = w.name(e);
                if (!found.valid() && n && *n == args[1]) found = e;
            });
            const vks::ScriptInstance* inst =
                found.valid() ? m_scriptRuntime->find(found) : nullptr;
            if (!inst) {
                VK_LOG_WARN("SCR-VARS ok=0 reason=no-instance name=%s",
                            args[1].c_str());
                return "no script instance on '" + args[1] + "'";
            }
            const auto rows = m_scriptRuntime->sortedVars(*inst);
            VK_LOG_INFO("SCR-VARS ok=1 name=%s state=%s count=%zu",
                        args[1].c_str(), m_scriptRuntime->stateOf(*inst),
                        rows.size());
            // 每个变量一行 —— 自动化按行数断言（F6 要 3 行以上）
            for (const auto& kv : rows) {
                VK_LOG_INFO("SCR-VARS %s=%s", kv.first.c_str(),
                            kv.second.c_str());
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), TR("%zu variable(s)"),
                          rows.size());
            return buf;
        });
    con->addCommand("script.trace",
        "script.trace on|off  per-frame script stats line",
        [this](const std::vector<std::string>& args) -> std::string {
            const bool on = args.size() >= 2 && args[1] == "on";
            if (args.size() >= 2 && args[1] != "on" && args[1] != "off")
                return "usage: script.trace on|off";
            m_scriptRuntime->setTrace(on);
            VK_LOG_INFO("SCR-TRACE on=%d", on ? 1 : 0);
            return on ? "trace on" : "trace off";
        });
    con->addCommand("anim.time",
        "anim.time <seconds> [name]  pin a skinned mesh to a time",
        [this, resolveSkinned](const std::vector<std::string>& args)
            -> std::string {
            if (args.size() < 2) return "usage: anim.time <seconds> [name]";
            ecs::Entity e;
            ecs::SkinnedMeshComponent* sk = nullptr;
            std::string err;
            if (!resolveSkinned(args, 2, e, sk, err)) {
                // 失败也要留一行：自动化那边等的是 ANIM-SETTIME，
                // 只在成功路径打日志的话，"没选中"会表现成"命令没执行"，
                // 排查时会去怀疑命令注册而不是选择集。
                VK_LOG_WARN("ANIM-SETTIME ok=0 reason=unresolved detail=%s",
                            err.c_str());
                return err;
            }
            float t = 0.0f;
            try {
                t = std::stof(args[1]);
            } catch (...) {
                VK_LOG_WARN("ANIM-SETTIME ok=0 reason=bad-number '%s'",
                            args[1].c_str());
                return "bad number: " + args[1];
            }
            // 钉住时间：编辑态本来就不会推进（dt=0），顺手把 playing 关掉
            // 能保证切到 Play 时也不被 AnimatorSystem 抢走。
            // ⚠ A1：有 AnimatorComponent 的实体，**写它的那一份**。
            //   写 sk->time 会在下一帧被 AnimatorSystem 的镜像覆盖回来
            //   （日志里 ANIM-SETTIME ok=1 打出来了，画面却回到原处）。
            auto* an =
                m_ctx->activeScene().world().get<ecs::AnimatorComponent>(e);
            if (an) {
                an->time = t;
                an->playing = false;
            } else {
                sk->time = t;
                sk->playing = false;
            }
            VK_LOG_INFO("ANIM-SETTIME ok=1 entity=%u t=%.3f via=%s",
                        static_cast<unsigned>(e.id), static_cast<double>(t),
                        an ? "anim" : "sk");
            return "time=" + args[1];
        });
    con->addCommand("anim.clip",
        "anim.clip <clipName> [name]  switch the clip of a skinned mesh",
        [this, resolveSkinned](const std::vector<std::string>& args)
            -> std::string {
            if (args.size() < 2) return "usage: anim.clip <clipName> [name]";
            ecs::Entity e;
            ecs::SkinnedMeshComponent* sk = nullptr;
            std::string err;
            if (!resolveSkinned(args, 2, e, sk, err)) {
                VK_LOG_WARN("ANIM-SETCLIP ok=0 reason=unresolved detail=%s",
                            err.c_str());
                return err;
            }
            if (!sk->model || !sk->model->skeleton) {
                VK_LOG_WARN("ANIM-SETCLIP ok=0 reason=no-skeleton entity=%u",
                            static_cast<unsigned>(e.id));
                return "the mesh has no skeleton";
            }
            if (!sk->model->skeleton->findClip(args[1])) {
                VK_LOG_WARN("ANIM-SETCLIP ok=0 reason=unknown-clip '%s' "
                            "entity=%u",
                            args[1].c_str(), static_cast<unsigned>(e.id));
                return "unknown clip: " + args[1];
            }
            sk->clipName = args[1];
            // A1：换 clip 等于"用户自己指定了"，把 defaultClip 的一次性
            // 补位闸门关上 —— 否则之后清空 clipName 会被系统填回去。
            if (auto* an =
                    m_ctx->activeScene().world().get<ecs::AnimatorComponent>(e))
                an->appliedDefaultClip = true;
            VK_LOG_INFO("ANIM-SETCLIP ok=1 entity=%u clip=%s t=%.3f",
                        static_cast<unsigned>(e.id), sk->clipName.c_str(),
                        static_cast<double>(sk->time));
            return "clip=" + sk->clipName;
        });
    // ---- 表情滑条（§15）----
    //
    // 与 Inspector 里的滑条**同一条路**：都调 `assets::setMorphWeight`
    // → 写 `MorphComponent::weights` → Renderer 每帧求值 → GPU 形变。
    // 所以这条命令能跑通，等于证明面板上的滑条也能跑通。
    //
    // 为什么必须按**名字**：`MorphRig` 的通道表是"跨材质桶合并后的唯一
    // 名字"，下标是建表顺序（重烤就会变）。按下标驱动等于把脚本绑死在
    // 某一次烤制的内部排序上。
    //
    // 用法：morph.set <channel> <weight> [entityName]
    //       morph.list [entityName]
    con->addCommand("morph.set",
        "morph.set <channel> <weight> [name]  drive one blend-shape slider",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 3) return "usage: morph.set <channel> <weight> [name]";
            ecs::Entity e = (args.size() > 3)
                                ? m_ctx->activeScene().find(args[3])
                                : m_ctx->selection();
            if (!e.valid()) {
                VK_LOG_WARN("MORPH-SET ok=0 reason=no-entity");
                return "no entity";
            }
            auto* mp = m_ctx->activeScene().world().get<ecs::MorphComponent>(e);
            if (!mp || !mp->model) {
                VK_LOG_WARN("MORPH-SET ok=0 reason=no-component entity=%u",
                            static_cast<unsigned>(e.id));
                return "no MorphComponent";
            }
            const std::size_t n = mp->model->morphRig.channelCount();
            float w = 0.0f;
            try {
                w = std::stof(args[2]);
            } catch (...) {
                VK_LOG_WARN("MORPH-SET ok=0 reason=bad-weight '%s'",
                            args[2].c_str());
                return "bad weight: " + args[2];
            }
            // 通道名不存在时**不要**静默当成功：`setMorphWeight` 返回
            // false，这里把它变成一条 ok=0 的日志 + 一条 fmt，自动化才能
            // 区分"名字写错了"和"形变没生效"。
            if (!assets::setMorphWeight(mp->model->morphRig, mp->weights,
                                        args[1], w)) {
                VK_LOG_WARN("MORPH-SET ok=0 reason=unknown-channel '%s' "
                            "entity=%u channels=%zu",
                            args[1].c_str(), static_cast<unsigned>(e.id), n);
                return "unknown channel: " + args[1];
            }
            VK_LOG_INFO("MORPH-SET ok=1 entity=%u channel=%s w=%.3f channels=%zu",
                        static_cast<unsigned>(e.id), args[1].c_str(),
                        static_cast<double>(w), n);
            return args[1] + "=" + args[2];
        });
    con->addCommand("morph.list",
        "morph.list [name]  print the blend-shape channels of a model",
        [this](const std::vector<std::string>& args) -> std::string {
            ecs::Entity e = (args.size() > 1)
                                ? m_ctx->activeScene().find(args[1])
                                : m_ctx->selection();
            if (!e.valid()) return "no entity";
            auto* mp = m_ctx->activeScene().world().get<ecs::MorphComponent>(e);
            if (!mp || !mp->model) return "no MorphComponent";
            std::string out;
            for (std::size_t c = 0; c < mp->model->morphRig.channelCount(); ++c) {
                if (c) out += " ";
                out += mp->model->morphRig.channels()[c].name;
            }
            VK_LOG_INFO("MORPH-LIST entity=%u n=%zu %s",
                        static_cast<unsigned>(e.id),
                        mp->model->morphRig.channelCount(), out.c_str());
            return out;
        });

    // ---- 两骨 IK / 骨骼挂点（第八刀，2026-10-25）----
    //
    // 为什么这两个功能**必须**有控制台入口："IK 生效了"在截图上不可见
    // —— 脚踝离地面 2cm 和 20cm 在 1080p 里是同一只脚，而后者毁掉整段
    // 走路。所以自动化只能靠 `IK-SET` / `ANIM-IK` 这两条数值锚点。
    //
    // ⚠ target 是 **glTF 场景空间**（Y-up），不是引擎的 Z-up 世界空间。
    //   与 `Pose` / `LookAt::target` 同一套坐标。
    con->addCommand("ik.set",
        "ik.set <x> <y> <z> [name]  aim the two-bone IK target (glTF space)",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 4) return "usage: ik.set <x> <y> <z> [name]";
            ecs::Entity e = (args.size() > 4)
                                ? m_ctx->activeScene().find(args[4])
                                : m_ctx->selection();
            if (!e.valid()) {
                VK_LOG_WARN("IK-SET ok=0 reason=no-entity");
                return "no entity";
            }
            auto* ik =
                m_ctx->activeScene().world().get<ecs::TwoBoneIKComponent>(e);
            // 没有组件就**建一个**，而不是报错。理由：`ik.set` 是自动化与
            // 手敲命令的入口，"先加组件再设目标"两步会让脚本里多一条没信息量
            // 的命令，而手工用户点 Inspector 那条路本来就有按钮。
            // 走的仍是 structuralEdit —— 一次拖拽/一条命令 = 一步撤销。
            int created = 0;
            if (!ik) {
                m_ctx->structuralEdit("Add Two Bone IK", [this, e]() {
                    ecs::TwoBoneIKComponent c;
                    m_ctx->activeScene().world().add<ecs::TwoBoneIKComponent>(e, c);
                });
                ik = m_ctx->activeScene().world().get<ecs::TwoBoneIKComponent>(e);
                created = 1;
            }
            if (!ik) {
                VK_LOG_WARN("IK-SET ok=0 reason=cannot-create entity=%u",
                            static_cast<unsigned>(e.id));
                return "cannot create TwoBoneIKComponent";
            }
            glm::vec3 t = ik->settings.target;
            try {
                t.x = std::stof(args[1]);
                t.y = std::stof(args[2]);
                t.z = std::stof(args[3]);
            } catch (...) {
                VK_LOG_WARN("IK-SET ok=0 reason=bad-target '%s,%s,%s'",
                            args[1].c_str(), args[2].c_str(), args[3].c_str());
                return "bad target";
            }
            ik->settings.target = t;
            ik->settings.hasTarget = true;

            // 顺手把**链解析**的结果打进日志：组件在、但链不成立时症状是
            // "腿一动不动"，没有这条就只能靠猜是名字错了还是拓扑错了。
            int resolved = 0;
            std::string chain;
            const auto* sk = m_ctx->activeScene()
                                 .world()
                                 .get<ecs::SkinnedMeshComponent>(e);
            if (sk && sk->model && sk->model->skeleton) {
                const assets::Skeleton& skel = *sk->model->skeleton;
                const animation::TwoBoneIKChain ch =
                    animation::resolveTwoBoneChain(skel, ik->settings);
                if (ch.valid()) {
                    resolved = 1;
                    chain = skel.joints[static_cast<size_t>(ch.root)].name + ">" +
                            skel.joints[static_cast<size_t>(ch.mid)].name + ">" +
                            skel.joints[static_cast<size_t>(ch.end)].name;
                }
            }
            VK_LOG_INFO("IK-SET ok=1 entity=%u target=%.3f,%.3f,%.3f "
                        "resolved=%d chain=[%s] weight=%.2f created=%d",
                        static_cast<unsigned>(e.id),
                        static_cast<double>(t.x), static_cast<double>(t.y),
                        static_cast<double>(t.z), resolved, chain.c_str(),
                        static_cast<double>(ik->settings.weight), created);
            m_ctx->dirty() = true;
            return resolved ? ("chain=" + chain) : "target set (chain unresolved)";
        });
    con->addCommand("ik.weight",
        "ik.weight <0..1> [name]  set the IK blend weight",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: ik.weight <0..1> [name]";
            ecs::Entity e = (args.size() > 2)
                                ? m_ctx->activeScene().find(args[2])
                                : m_ctx->selection();
            if (!e.valid()) return "no entity";
            auto* ik =
                m_ctx->activeScene().world().get<ecs::TwoBoneIKComponent>(e);
            if (!ik) return "no TwoBoneIKComponent";
            float w = 1.0f;
            try {
                w = std::stof(args[1]);
            } catch (...) {
                return "bad weight: " + args[1];
            }
            ik->settings.weight = w;
            VK_LOG_INFO("IK-WEIGHT entity=%u weight=%.3f",
                        static_cast<unsigned>(e.id), static_cast<double>(w));
            m_ctx->dirty() = true;
            return args[1];
        });
    con->addCommand("ik.list",
        "ik.list [name]  print the resolved IK chain and its settings",
        [this](const std::vector<std::string>& args) -> std::string {
            ecs::Entity e = (args.size() > 1)
                                ? m_ctx->activeScene().find(args[1])
                                : m_ctx->selection();
            if (!e.valid()) return "no entity";
            const auto* sk = m_ctx->activeScene()
                                 .world()
                                 .get<ecs::SkinnedMeshComponent>(e);
            auto* ik =
                m_ctx->activeScene().world().get<ecs::TwoBoneIKComponent>(e);
            // 没组件也照样解析一遍（用默认 settings）—— 这一条是"探路"用的：
            // 拿到一个不认识的资产时，先 `ik.list` 看自动挑中的是哪三根骨，
            // 再决定要不要手填名字。组件缺失时不能只回一句 no-component。
            const animation::TwoBoneIKSettings def;
            const animation::TwoBoneIKSettings& s = ik ? ik->settings : def;
            int root = -1, mid = -1, end = -1;
            std::string names;
            if (sk && sk->model && sk->model->skeleton) {
                const assets::Skeleton& skel = *sk->model->skeleton;
                const auto ch = animation::resolveTwoBoneChain(skel, s);
                root = ch.root;
                mid = ch.mid;
                end = ch.end;
                if (ch.valid()) {
                    names = skel.joints[static_cast<size_t>(ch.root)].name +
                            ">" + skel.joints[static_cast<size_t>(ch.mid)].name +
                            ">" + skel.joints[static_cast<size_t>(ch.end)].name;
                }
            }
            VK_LOG_INFO("IK-LIST entity=%u hasComponent=%d root=%d mid=%d end=%d "
                        "chain=[%s] hasTarget=%d weight=%.2f enabled=%d",
                        static_cast<unsigned>(e.id), ik ? 1 : 0, root, mid, end,
                        names.c_str(), s.hasTarget ? 1 : 0,
                        static_cast<double>(s.weight), s.enabled ? 1 : 0);
            return names.empty() ? std::string("chain unresolved")
                                 : ("chain=" + names);
        });
    // 挂点。宿主按**名字**引用（实体 id 是运行时序号，进存档不稳定）。
    con->addCommand("attach.bind",
        "attach.bind <host> <bone> [child]  pin an object to a bone of <host>",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 3)
                return "usage: attach.bind <host> <bone> [child]";
            ecs::Entity host = m_ctx->activeScene().find(args[1]);
            if (!host.valid()) {
                VK_LOG_WARN("ATTACH-BIND ok=0 reason=no-host '%s'",
                            args[1].c_str());
                return "no host: " + args[1];
            }
            ecs::Entity child = (args.size() > 3)
                                    ? m_ctx->activeScene().find(args[3])
                                    : m_ctx->selection();
            if (!child.valid()) {
                VK_LOG_WARN("ATTACH-BIND ok=0 reason=no-child");
                return "no child entity";
            }
            // 先解析一遍再决定日志里的 resolved —— 绑上去之后才发现骨名
            // 打错的话，画面上的表现是"这个物体一动不动"，与"没绑定"一样。
            int resolved = 0;
            int joint = -1;
            const auto* sk =
                m_ctx->activeScene().world().get<ecs::SkinnedMeshComponent>(host);
            if (sk && sk->model && sk->model->skeleton) {
                joint = sk->model->skeleton->findJoint(args[2],
                                                       assets::kJointMatchDefault);
                resolved = (joint >= 0) ? 1 : 0;
            }
            const std::string hostName = args[1];
            const std::string boneName = args[2];
            m_ctx->structuralEdit("Bind Bone Attachment",
                                  [this, child, hostName, boneName]() {
                                      auto& w = m_ctx->activeScene().world();
                                      if (auto* p =
                                              w.get<ecs::BoneAttachmentComponent>(
                                                  child)) {
                                          p->hostName = hostName;
                                          p->boneName = boneName;
                                          p->enabled = true;
                                      } else {
                                          ecs::BoneAttachmentComponent c;
                                          c.hostName = hostName;
                                          c.boneName = boneName;
                                          w.add<ecs::BoneAttachmentComponent>(
                                              child, c);
                                      }
                                  });
            VK_LOG_INFO("ATTACH-BIND ok=1 child=%u host=[%s] bone=[%s] "
                        "joint=%d resolved=%d",
                        static_cast<unsigned>(child.id), hostName.c_str(),
                        boneName.c_str(), joint, resolved);
            return resolved ? ("bound to " + boneName)
                            : ("bound but bone NOT found: " + boneName);
        });

    // 存盘（角色链 A0-2 的自动化入口）。快捷键 Ctrl+S 走同一条
    // saveSceneNow()，所以这不算"测试专用路径"，只是给脚本一个入口。
    con->addCommand("scene.save", "scene.save  save the current scene to its path",
        [this](const std::vector<std::string>&) -> std::string {
            saveSceneNow();
            return "saved: " + m_ctx->scenePath();
        });

    // ---- 预制体（G18）----
    //
    // 六条命令走的是与层级右键菜单**完全相同**的 EditorContext 方法，不是
    // 测试专用捷径。因此它们同时干三件事：
    //   ① 自动化（`MYVK_EDITOR_CONSOLE_CMD`）能把"建 → 摆 → 改母版 → Apply /
    //      Revert"整条链跑一遍，且断言落在 `HIER-PREFAB` / `PREFAB-*` 上；
    //   ② 命令面板自动多出六项（E18 的命令表就是从这张控制台表派生的）；
    //   ③ 用户可以在控制台里先手动做一次，再复制成脚本。
    //
    // `prefab.reload` 单独说一句：母版是**磁盘上的文件**，用户可能在外部
    // 编辑器里改了它。没有这条命令，"改母版 → 所有实例跟着变"在编辑器里
    // 就没有触发点（文件监听是下一步的事）。
    con->addCommand("prefab.create",
        "prefab.create [name]  export the selected subtree as a prefab asset",
        [this](const std::vector<std::string>& args) -> std::string {
            const ecs::Entity sel = m_ctx->selection();
            const std::string* np = m_ctx->activeScene().world().name(sel);
            const std::string nm = args.size() >= 2
                                       ? args[1]
                                       : (np ? *np : std::string("prefab"));
            const std::string rel = scene::prefabDefaultPath(nm);
            if (!m_ctx->prefabCreateFromSelection(rel)) return "create failed";
            return "created: " + rel;
        });
    con->addCommand("prefab.instantiate",
        "prefab.instantiate <path> [x y z]  place a prefab into the scene",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: prefab.instantiate <path> [x y z]";
            glm::vec3 at(0.0f);
            if (args.size() >= 5) {
                at = glm::vec3(static_cast<float>(std::atof(args[2].c_str())),
                               static_cast<float>(std::atof(args[3].c_str())),
                               static_cast<float>(std::atof(args[4].c_str())));
            }
            if (!m_ctx->prefabInstantiate(args[1], at)) return "instantiate failed";
            return "instantiated: " + args[1];
        });
    con->addCommand("prefab.apply",
        "prefab.apply  write the selected instance's overrides back to the asset",
        [this](const std::vector<std::string>&) -> std::string {
            const ecs::Entity sel = m_ctx->selection();
            if (!m_ctx->prefabApply(sel)) return "apply failed";
            return "applied";
        });
    con->addCommand("prefab.revert",
        "prefab.revert  drop the selected instance's overrides (back to the asset)",
        [this](const std::vector<std::string>&) -> std::string {
            const ecs::Entity sel = m_ctx->selection();
            if (!m_ctx->prefabRevert(sel)) return "revert failed";
            return "reverted";
        });
    con->addCommand("prefab.reload",
        "prefab.reload <path>  re-read the asset from disk and re-sync every instance",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: prefab.reload <path>";
            scene::PrefabAsset a;
            if (!scene::loadPrefab(args[1], a))
                return "load failed: " + a.error;
            const scene::PrefabSyncResult r =
                scene::syncInstances(m_ctx->activeScene(), m_ctx->assets(), a);
            m_ctx->dirty() = true;
            return "synced: instances=" + std::to_string(r.instances) +
                   " applied=" + std::to_string(r.applied) +
                   " failed=" + std::to_string(r.failed);
        });
    con->addCommand("prefab.info",
        "prefab.info  print the selected entity's prefab link (PREFAB-INFO)",
        [this](const std::vector<std::string>&) -> std::string {
            scene::Scene& sc = m_ctx->activeScene();
            const ecs::Entity sel = m_ctx->selection();
            const auto* pl = sc.world().get<ecs::PrefabLinkComponent>(sel);
            if (!pl) {
                // 也打一条锚点：脚本要能区分"不是实例"与"命令没跑"
                VK_LOG_INFO("PREFAB-INFO ok=0 reason=not-a-member");
                return "not a prefab member";
            }
            const int ov = scene::countOverrides(sc, pl->instance);
            VK_LOG_INFO("PREFAB-INFO ok=1 path=%s instance=%u root=%d "
                        "localId=%s overrides=%d",
                        pl->prefabPath.c_str(), pl->instance,
                        pl->root ? 1 : 0, pl->localId.c_str(), ov);
            return pl->prefabPath + "  instance " + std::to_string(pl->instance) +
                   "  overrides " + std::to_string(ov);
        });

    // ---- 音频（G14-8）----
    //
    // 三个命令走的是**与 UI 完全相同**的那条路（`EditorContext` 请求 →
    // `syncEditorAudio` → AudioSystem），不是"测试专用捷径"：所以
    // `AUDIO-PREVIEW clip=... ok=1` 出现在日志里，等于证明了
    // Inspector 的 ▶ 和 Content 的双击也会走通。
    con->addCommand("audio.preview",
        "audio.preview <clip>  play a wav/ogg once (same path as double-click)",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: audio.preview <clip>";
            m_ctx->requestAudioPreview(args[1], 1.0f);
            VK_LOG_INFO("AUDIO-CMD preview clip=%s", args[1].c_str());
            return "preview: " + args[1];
        });
    con->addCommand("audio.stop", "audio.stop  stop every one-shot / preview sound",
        [this](const std::vector<std::string>&) -> std::string {
            m_ctx->requestAudioStop();
            return "stopped";
        });
    con->addCommand("audio.bus",
        "audio.bus <master|music|sfx|voice> <0..1>  set a bus volume",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 3) return "usage: audio.bus <name> <volume>";
            const int idx = audio::busIndexFromName(args[1].c_str());
            if (idx < 0) {
                // ⚠ 与脚本侧 `setBusVolume` 同一条纪律：认不出必须留痕
                VK_LOG_WARN("AUDIO-CMD bus=unknown name=%s", args[1].c_str());
                return "unknown bus: " + args[1];
            }
            const float v = static_cast<float>(std::atof(args[2].c_str()));
            m_audio.setBusGain(static_cast<audio::BusId>(idx), v);
            VK_LOG_INFO("AUDIO-CMD bus=%s vol=%.2f", args[1].c_str(),
                        static_cast<double>(v));
            return "bus " + args[1];
        });
    con->addCommand("audio.stats", "audio.stats  print one AUDIO-STATS line",
        [this](const std::vector<std::string>&) -> std::string {
            const audio::MixStats st = m_audio.stats();
            VK_LOG_INFO("AUDIO-STATS voices=%d rms=%.4f peak=%.4f cpu=0.00 "
                        "fill=0 sr=%u ch=%u muted=%d mem=%.0f msgs=0",
                        st.voices, static_cast<double>(st.rms),
                        static_cast<double>(st.peak),
                        m_audio.deviceFormat().sampleRate,
                        m_audio.deviceFormat().channels, m_audio.muted() ? 1 : 0,
                        static_cast<double>(m_audio.cacheResidentBytes() +
                                            m_audio.streamResidentBytes()));
            return "voices=" + std::to_string(st.voices);
        });

    // ---- U2：游戏 UI 的"Hello World" ----
    // ui.rect <x> <y> <w> <h> <#rrggbb[aa]|off>
    //
    // 为什么先做一条控制台命令、而不是先做控件：整条通道（挂点 / sRGB /
    // 合批 / 顶点缓冲）都可能出错，而**一块纯色矩形**能把它们一次全部暴露
    // —— 颜色不对就是 sRGB 问题、位置不对就是挂点或 NDC 问题、压根没有
    // 就是回调没接上。用控件来验证会把这三类混在一起。
    //
    // 坐标是**输出像素**（不是逻辑像素）：这一步还没有布局系统，直接照
    // 屏幕像素给，验收脚本就能用 "0 0 4096 4096" 一把铺满。
    con->addCommand("ui.rect",
        "ui.rect <x> <y> <w> <h> <#rrggbb|off>  draw a debug UI rect (U2)",
        [](const std::vector<std::string>& args) -> std::string {
            ui2::DebugRect& r = ui2::debugRect();
            if (args.size() >= 2 && (args[1] == "off" || args[1] == "clear")) {
                r.enabled = false;
                VK_LOG_INFO("UI2-DEBUG-RECT on=0");
                return "ui rect off";
            }
            if (args.size() < 5) {
                return "usage: ui.rect <x> <y> <w> <h> <#rrggbb|off>";
            }
            float v[4] = {0, 0, 0, 0};
            try {
                for (int i = 0; i < 4; ++i) v[i] = std::stof(args[i + 1]);
            } catch (...) {
                return "bad number in ui.rect";
            }
            uint32_t color = r.color;   // 省略颜色 = 沿用上一次（默认中灰）
            if (args.size() >= 6) {
                std::string s = args[5];
                if (!s.empty() && s[0] == '#') s.erase(0, 1);
                if (s.size() != 6 && s.size() != 8) {
                    return "color must be #rrggbb or #rrggbbaa";
                }
                unsigned long packed = 0;
                try {
                    packed = std::stoul(s, nullptr, 16);
                } catch (...) {
                    return "bad color: " + args[5];
                }
                color = (s.size() == 6) ? ui2::rgbA(packed, 255)
                                        : static_cast<uint32_t>(packed);
            }
            r.enabled = true;
            r.x = v[0];
            r.y = v[1];
            r.w = v[2];
            r.h = v[3];
            r.color = color;
            VK_LOG_INFO("UI2-DEBUG-RECT on=1 x=%.1f y=%.1f w=%.1f h=%.1f "
                        "color=%08X",
                        static_cast<double>(r.x), static_cast<double>(r.y),
                        static_cast<double>(r.w), static_cast<double>(r.h),
                        color);
            return "ui rect on";
        });

    // ---- U1：SDF 字体上屏 ----
    // ui.text <x> <baselineY> <sizePx> <#rrggbb|-> <text...>
    //
    // 与 ui.rect 同一思路：先把"字体链路"单独钉住（图集读到了吗、uv 对吗、
    // SDF 的平滑宽度对吗、汉字有没有缺字），再谈布局与控件。`-` 表示沿用
    // 上次颜色。文本允许含空格（后面的参数会被拼回来）。
    con->addCommand("ui.text",
        "ui.text <x> <baselineY> <sizePx> <#rrggbb|-> <text>  draw SDF text (U1)",
        [](const std::vector<std::string>& args) -> std::string {
            ui2::DebugText& t = ui2::debugText();
            if (args.size() >= 2 && (args[1] == "off" || args[1] == "clear")) {
                t.enabled = false;
                VK_LOG_INFO("UI2-DEBUG-TEXT on=0");
                return "ui text off";
            }
            if (args.size() < 6) {
                return "usage: ui.text <x> <baselineY> <sizePx> <#rrggbb|-> <text>";
            }
            float v[3] = {0, 0, 0};
            try {
                for (int i = 0; i < 3; ++i) v[i] = std::stof(args[i + 1]);
            } catch (...) {
                return "bad number in ui.text";
            }
            uint32_t color = t.color;
            const std::string& cs = args[4];
            if (cs != "-") {
                std::string s = cs;
                if (!s.empty() && s[0] == '#') s.erase(0, 1);
                if (s.size() != 6 && s.size() != 8) {
                    return "color must be #rrggbb, #rrggbbaa or -";
                }
                unsigned long packed = 0;
                try {
                    packed = std::stoul(s, nullptr, 16);
                } catch (...) {
                    return "bad color: " + cs;
                }
                color = (s.size() == 6) ? ui2::rgbA(packed, 255)
                                        : static_cast<uint32_t>(packed);
            }
            std::string text = args[5];
            for (size_t i = 6; i < args.size(); ++i) text += " " + args[i];
            t.enabled = true;
            t.x = v[0];
            t.baselineY = v[1];
            t.sizePx = v[2];
            t.color = color;
            t.utf8 = text;
            VK_LOG_INFO("UI2-DEBUG-TEXT on=1 x=%.1f base=%.1f size=%.1f "
                        "bytes=%u",
                        static_cast<double>(t.x),
                        static_cast<double>(t.baselineY),
                        static_cast<double>(t.sizePx),
                        static_cast<unsigned>(t.utf8.size()));
            return "ui text on";
        });

    // ---- U5：一个真能被点中的按钮 ----
    // ui.button <x> <y> <w> <h> <label...>   （**参考像素**，左上锚点）
    // ui.button off
    con->addCommand("ui.button",
        "ui.button <x> <y> <w> <h> <label|off>  debug button (U5)",
        [](const std::vector<std::string>& args) -> std::string {
            ui2::DebugButton& d = ui2::debugButton();
            if (args.size() >= 2 && (args[1] == "off" || args[1] == "clear")) {
                d.enabled = false;
                VK_LOG_INFO("UI2-DEBUG-BUTTON on=0");
                return "ui button off";
            }
            if (args.size() < 6) {
                return "usage: ui.button <x> <y> <w> <h> <label>";
            }
            float v[4] = {0, 0, 0, 0};
            try {
                for (int i = 0; i < 4; ++i) v[i] = std::stof(args[i + 1]);
            } catch (...) {
                return "bad number in ui.button";
            }
            std::string label = args[5];
            for (size_t i = 6; i < args.size(); ++i) label += " " + args[i];

            d.button.id = "debug";
            d.button.label = label;
            d.button.frame.anchor = ui2::UiAnchor::TopLeft;
            d.button.frame.x = v[0];
            d.button.frame.y = v[1];
            d.button.frame.w = v[2];
            d.button.frame.h = v[3];
            d.button.enabled = true;
            d.enabled = true;
            VK_LOG_INFO("UI2-DEBUG-BUTTON on=1 x=%.1f y=%.1f w=%.1f h=%.1f "
                        "label_bytes=%u",
                        static_cast<double>(v[0]), static_cast<double>(v[1]),
                        static_cast<double>(v[2]), static_cast<double>(v[3]),
                        static_cast<unsigned>(label.size()));
            return "ui button on";
        });

    // ---- U5：模拟一次点击 ----
    // ui.click <x> <y> [ref|px]   默认 **ref**（参考像素，与 ui.button 同坐标系）
    //
    // 默认用参考像素是刻意的：验收脚本不知道视口多大，只知道"按钮设计在
    // (100,100) 300×80"，于是点 (250,140) 就是点它的中心。换成输出像素的话，
    // 脚本得先算 scale —— 那个 0.4948 之类的数字正是假红的高发区。
    con->addCommand("ui.click",
        "ui.click <x> <y> [ref|px]  simulate a click (U5)",
        [](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 3) return "usage: ui.click <x> <y> [ref|px]";
            float x = 0.0f, y = 0.0f;
            try {
                x = std::stof(args[1]);
                y = std::stof(args[2]);
            } catch (...) {
                return "bad number in ui.click";
            }
            const bool px = (args.size() >= 4 && args[3] == "px");
            if (px) ui2::requestClick(x, y);
            else    ui2::requestClickRef(x, y);
            // 真正的命中判定发生在下一帧"布局算完"之后（见 ui2/UiWidget.h）
            return "click queued";
        });
    con->addCommand("stat", "stat gpu  print a GPU timestamp report to the console",
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
    con->addCommand("insp.selftest", "insp.selftest  Inspector clipboard/reset self-test",
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
                    "undo.selftest [paramId]  self-test for undoing setting changes",
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
                    "notif.push <info|warn|error> <text>  raise a notification",
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
    con->addCommand("notif.dump", "notif.dump  snapshot of notification / progress counts",
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
                    "notif.click <idx>  simulate clicking toast #idx (0 = topmost)",
        [this](const std::vector<std::string>& args) -> std::string {
            const int idx = args.size() > 1 ? std::atoi(args[1].c_str()) : 0;
            if (m_notifications->toastCount() == 0) return "no toasts";
            m_notifications->clickToast(idx);
            return "clicked toast " + std::to_string(idx);
        });
    con->addCommand("notif.task", "notif.task <id> <title...>  start a progress task",
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
                    "notif.progress <id> <0..1>  advance task progress",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 3) return "usage: notif.progress <id> <0..1>";
            const float p = std::strtof(args[2].c_str(), nullptr);
            if (!m_notifications->setTaskProgress(args[1], p))
                return "task '" + args[1] + "' cancelled or gone";
            return "progress " + args[1] + " = " + args[2];
        });
    con->addCommand("notif.cancel", "notif.cancel <id>  request task cancellation",
        [this](const std::vector<std::string>& args) -> std::string {
            if (args.size() < 2) return "usage: notif.cancel <id>";
            m_notifications->requestCancel(args[1]);
            return "cancel requested: " + args[1];
        });
    con->addCommand("notif.done", "notif.done <id>  finish a task",
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
        c.label = std::string(TR("Toggle Panel: ")) + TR(pt[i].title);
        c.category = TR("View");
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
        c.label = std::string(TR("Workspace: ")) + TR(workspaceTitle(i));
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
            m_ctx->notify(TR("Layout reset"));
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
    if (!ImGui::Begin(trId("Preferences", "prefs"), &m_ctx->showSettings(),
                      ImGuiWindowFlags_NoCollapse)) {
        // 窗口被关掉（或折叠）时这一帧谁也画不到 —— 捕获必须收掉，
        // 否则全局快捷键会一直被挡着。
        m_ctx->endInputCapture();
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
    ImGui::InputTextWithHint("##prefs_search", TRL("Search settings..."), search,
                             sizeof(search));
    ImGui::SameLine();
    if (ImGui::Button(TR("Save"))) savePrefsToDisk();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(TR("Write current preferences to %s (also saved automatically on exit)"),
                          editorPrefsPath().c_str());
    ImGui::SameLine();
    if (ImGui::Button(TR("Reload"))) {
        applyPrefsFromDisk();
        applyAppearance();
        m_ctx->notify(TR("Preferences reloaded from disk"));
    }
    ImGui::SameLine();
    if (!m_confirmResetAll) {
        if (ImGui::Button(TR("Reset All"))) m_confirmResetAll = true;
    } else {
        // 二次确认：第一次点击只进入"确认态"（按钮变红），再点才真清
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.65f, 0.22f, 0.18f, 1.0f));
        if (ImGui::Button(TR("Confirm reset all?"))) {
            for (const PrefItem& it : prefsTable()) {
                if (!it.options.empty()) {
                    const int n = static_cast<int>(it.options.size());
                    const int d = std::clamp(static_cast<int>(it.def), 0, n - 1);
                    if (it.setChoice) it.setChoice(d);
                    continue;
                }
                void* p = it.ptr(*this);
                if (it.isBool)
                    *static_cast<bool*>(p) = (it.def > 0.5f);
                else
                    *static_cast<float*>(p) = it.def;
            }
            applyAppearance();
            m_confirmResetAll = false;
            savePrefsToDisk();
            m_ctx->notify(TR("All preferences reset to defaults"));
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("cancel"))) m_confirmResetAll = false;
    }

    // ---- 左侧分类标签 ----
    //
    // 页表提到了**文件作用域**（kPrefsPages）：页签要能被自动化按名字选中
    // （MYVK_EDITOR_PREFS_PAGE=Bindings），而那个初值在函数一开始就要用，
    // 局部 static 数组在这一行还没声明。
    static int page = prefsPageFromEnv();
    constexpr int kPageCount = kPrefsPageCount;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::tokens().panelBg);
    ImGui::BeginChild("##prefs_nav", ImVec2(168.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    for (int i = 0; i < kPageCount; ++i) {
        if (ImGui::Selectable(TR(kPrefsPages[i]), page == i)) {
            page = i;
        }
        // 自动化锚点：分类页签矩形（tag 只能含字母/连字符 → 用英文页名，
        // 不能用下标数字，见 tools/verify_add_menu.py 的 RECT_RE）。
        // 语言切换不影响它（页名是**逻辑**名，不随界面语言变）。
        logRect((std::string("PREFS-TAB-") + kPrefsPages[i]).c_str(),
                ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##prefs_content", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const std::vector<PrefItem>& table = prefsTable();
    auto drawCategory = [this, &table](const char* cat, const char* note = nullptr) {
        // ⚠ SeparatorText 只吃显示文本；分类**键**保持英文（表里的
        //   category 字段就是键，翻译只发生在画出来的这一层）。
        ImGui::SeparatorText(TR(cat));
        // 分类级的说明**不上屏**：标题右边挂一个 "(?)"，悬浮才展开
        //（2026-10-17 用户要求：面板上只留控件，解释走 tooltip）。
        if (note && note[0]) {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s", note);
        }
        for (const PrefItem& it : table)
            if (std::strcmp(it.category, cat) == 0) drawPrefRow(it);
    };

    if (search[0]) {
        // ---- E17 搜索模式：跨分类平铺命中项（label / id / 分类名 / 选项名）----
        const char* lastCat = nullptr;
        int hits = 0;
        for (const PrefItem& it : table) {
            bool hit = prefMatch(it.label, search) ||
                       prefMatch(it.id, search) ||
                       prefMatch(it.category, search);
            for (const char* o : it.options) {
                if (hit) break;
                hit = prefMatch(o, search) || prefMatch(TR(o), search);
            }
            if (!hit) continue;
            if (!lastCat || std::strcmp(lastCat, it.category) != 0) {
                ImGui::SeparatorText(TR(it.category));
                lastCat = it.category;
            }
            drawPrefRow(it);
            ++hits;
        }
        if (hits == 0)
            ImGui::TextDisabled(TR("(no settings match '%s')"), search);
    } else if (page == 0) {
        drawCategory("Interface");
    } else if (page == 1) {
        drawCategory("Appearance",
                     "A theme only swaps colours and rounding - the panel "
                     "layout stays identical.\n\nSwitching the language "
                     "replaces display text only; every id, key and command "
                     "stays English.");
    } else if (page == 2) {
        drawCategory("Navigation",
                     "Also editable in the viewport HUD: hold RMB + wheel "
                     "to adjust the sensitivity.");
        drawCategory("Ground grid");
        drawCategory("Light gizmos");
    } else if (page == 3) {
        drawCategory("Snapping");
    } else if (page == 4) {
        // ---- Keymap（只读速查）----
        ImGui::SeparatorText(TR("Viewport navigation (edit mode)"));
        ImGui::BulletText(TR("Hold RMB + drag: look around"));
        ImGui::BulletText(TR("Hold RMB + WASD: fly   Q / E: down / up"));
        ImGui::BulletText(TR("Hold MMB + drag: pan   Shift: fly faster"));
        ImGui::BulletText(TR("Wheel: dolly in / out   RMB + wheel: sensitivity"));
        ImGui::SeparatorText(TR("Editing"));
        ImGui::BulletText(TR("LMB: select / drag gizmo handle"));
        ImGui::BulletText(TR("W / E / R: gizmo mode   X: world / local"));
        ImGui::BulletText(TR("G: snapping on/off for the current tool"));
        ImGui::BulletText(TR("Shift+A (in viewport): add menu (Mesh / Light)"));
        ImGui::BulletText(TR("1 / 3 / 7: Front / Right / Top (+Ctrl = opposite)"));
        ImGui::BulletText(TR("2 / 4 / 6 / 8: orbit step (Blender numpad)"));
        ImGui::BulletText(TR("F: focus selection   Del: delete   F2: rename"));
        ImGui::BulletText(TR("Ctrl+Z / Ctrl+Y: undo / redo"));
        ImGui::SeparatorText(TR("Scene & playback"));
        ImGui::BulletText(TR("Ctrl+S: save   Ctrl+N: new scene"));
        ImGui::BulletText(TR("Space: run / pause   F: game fullscreen (in run)"));
        ImGui::BulletText(TR("F11: window fullscreen (any mode)"));
    } else if (page == kPrefsPageBindings) {
        // ---- G06：游戏动作键位重绑定 ----
        drawKeyBindingsPage(*m_ctx, window().handle());
    } else if (page == 5) {
        // ---- Scene（信息页）----
        ImGui::SeparatorText(TR("Scene"));
        ImGui::Text(TR("Path: %s%s"), m_ctx->scenePath().c_str(),
                    m_ctx->dirty() ? TR("  (unsaved changes)") : "");
        if (m_ctx->dirty())
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.4f, 1.0f),
                               TR("There are unsaved changes - Ctrl+S to save."));
        ImGui::Text(TR("Entities: %zu (editor %zu / runtime %zu)"),
                    m_ctx->activeScene().objectCount(),
                    m_ctx->editorScene().objectCount(),
                    m_ctx->runtimeScene().objectCount());
        const scene::Scene::LightStats ls = m_ctx->editorScene().lightStats();
        ImGui::Text(TR("Local lights: %zu (%zu point / %zu spot)"),
                    ls.pointLights + ls.spotLights, ls.pointLights,
                    ls.spotLights);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled(TR("Preferences apply immediately. Saved to %s."),
                        editorPrefsPath().c_str());

    // G06：捕获是模态的 —— 离开这一页 / 关掉 Preferences 必须收掉，否则
    // 全局快捷键会一直被挡着（症状是"整个编辑器按什么都没反应"）。
    if (page != kPrefsPageBindings && m_ctx->inputCaptureActive())
        m_ctx->endInputCapture();

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
        // ---- Appearance（主题 / 语言）----
        // 这两项是"选项型"：落盘存下标，取值走 getChoice/setChoice。
        // options 从各自的模块派生，不写死 —— 以后往 ui::ThemeId 里加一套
        // 主题，这里自动多一个下拉项，不需要同步维护两份名单。
        {"theme", "Theme", "Appearance", false, 0.0f, 0.0f, 0.0f, nullptr,
         []() -> int { return ui::currentThemeIndex(); },
         [](int v) { ui::applyTheme(v); }, nullptr,
         [] {
             std::vector<const char*> o;
             for (int i = 0; i < ui::themeCount(); ++i) o.push_back(ui::themeName(i));
             return o;
         }()},
        {"language", "Language", "Appearance", false, 0.0f, 0.0f, 0.0f, nullptr,
         []() -> int { return language(); },
         [](int v) { setLanguage(v); }, nullptr,
         [] {
             std::vector<const char*> o;
             for (int i = 0; i < languageCount(); ++i) o.push_back(languageName(i));
             return o;
         }()},
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
        {"grid.extent", "Extent (m)", "Ground grid", false, 250.0f, 5.0f,
         1000.0f,
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
        {"grid.fadeStart", "Edge fade start (m)", "Ground grid", false, 200.0f,
         0.0f, 600.0f,
         [](EditorApp& a) -> void* {
             return &a.renderer().gridSettings().fadeStart;
         }},
        {"grid.fadeEnd", "Edge fade end (m)", "Ground grid", false, 250.0f,
         5.0f, 600.0f,
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
        // ---- Snapping（用户要求：默认全开；步长仍是 UE5 那套 0.1 / 10 / 0.1）----
        {"snap.move", "Move snapping", "Snapping", true, 1.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapMoveRef(); }},
        {"snap.moveStep", "Move step (m)", "Snapping", false, 0.1f, 0.01f,
         5.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapMoveStepRef(); }},
        {"snap.rotate", "Rotate snapping", "Snapping", true, 1.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapRotateRef(); }},
        {"snap.rotateStep", "Rotate step (deg)", "Snapping", false, 10.0f,
         1.0f, 90.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapRotateStepRef(); }},
        {"snap.scale", "Scale snapping", "Snapping", true, 1.0f, 0.0f, 0.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapScaleRef(); }},
        {"snap.scaleStep", "Scale step", "Snapping", false, 0.1f, 0.01f, 1.0f,
         [](EditorApp& a) -> void* { return &a.m_ctx->snapScaleStepRef(); }},
        // ---- Viewport overlays（E14）----
        // 浮层各自的显隐。右键视口空白处的 "Viewport Display" 菜单是主入口，
        // 这里是同一份存储的第二个视图（顺带解决了落盘 —— E17 的 prefs 表
        // 就是持久化的唯一来源）。
        // ⚠ `vp.overlay.modeBar` 已随左上角模式条一起删掉（2026-10-17）。
        //   旧存档里残留这个键是无害的：装载时按**本表**逐项查 JSON，表里
        //   没有的键根本不会被读（见 EditorLayoutIO::applyPrefs）。
        {"vp.overlay.stats", "Stats HUD", "Viewport overlays", true, 0.0f, 0.0f,
         0.0f,
         [](EditorApp& a) -> void* {
             return &a.m_ctx->vpOverlayRef().stats;
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

    // ---- 选项型（下拉）：改完立刻生效，没有"确定"按钮 ----
    if (!it.options.empty()) {
        const int n = static_cast<int>(it.options.size());
        int cur = it.getChoice ? it.getChoice() : 0;
        if (cur < 0 || cur >= n) cur = 0;
        // ImGui 的下拉要 `const char* const*`；顺便在这里过一遍 TR ——
        // 选项名只在**显示**时翻译，落盘的永远是下标。
        std::vector<const char*> disp(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) disp[i] = TR(it.options[i]);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo(it.label, &cur, disp.data(), n)) {
            if (it.setChoice) it.setChoice(cur);
            if (it.onChoiceChanged) it.onChoiceChanged(cur);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("R"))) {
            const int d = std::clamp(static_cast<int>(it.def), 0, n - 1);
            if (it.setChoice) it.setChoice(d);
            if (it.onChoiceChanged) it.onChoiceChanged(d);
        }
        if (ImGui::IsItemHovered()) {
            const int d = std::clamp(static_cast<int>(it.def), 0, n - 1);
            ImGui::SetTooltip(TR("Reset '%s' to default (%s)"), TR(it.label),
                              TR(it.options[d]));
        }
        ImGui::PopID();
        return;
    }

    void* p = it.ptr(*this);
    if (it.isBool) {
        ImGui::Checkbox(TR(it.label), static_cast<bool*>(p));
    } else {
        float* f = static_cast<float*>(p);
        ImGui::SetNextItemWidth(220.0f);
        // 用 DragFloat 而不是 SliderFloat：像 extent=500 这种"超出常用
        // 区间"的值还能往回拖，不会被滑条夹死。
        ImGui::DragFloat(TR(it.label), f, (it.hi - it.lo) * 0.005f, it.lo,
                         it.hi, "%.2f");
    }
    // 单项重置：悬停显示默认值是什么
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("R"))) {
        if (it.isBool)
            *static_cast<bool*>(p) = (it.def > 0.5f);
        else
            *static_cast<float*>(p) = it.def;
    }
    if (ImGui::IsItemHovered()) {
        if (it.isBool)
            ImGui::SetTooltip(TR("Reset '%s' to default (%s)"), TR(it.label),
                              it.def > 0.5f ? TR("on") : TR("off"));
        else
            ImGui::SetTooltip(TR("Reset '%s' to default (%.2f)"), TR(it.label),
                              static_cast<double>(it.def));
    }
    ImGui::PopID();
}

// 主题 / 语言是**立即生效的全局状态**：读盘之后必须真应用一次，否则只是
// 改了"存储"没改"表现"（窗口已经建好，样式是启动时那一套）。
// 放在这里而不是读盘循环里，是因为两个设置项都要经过同一条路。
void EditorApp::applyAppearance() {
    for (const PrefItem& it : prefsTable()) {
        if (it.options.empty() || !it.getChoice) continue;
        const int v = it.getChoice();
        if (it.setChoice) it.setChoice(v);          // 夹范围 + 应用
        if (it.onChoiceChanged) it.onChoiceChanged(v);
    }

    // ---- 验收钩子：`MYVK_EDITOR_LANG=zh` 强制中文 ----
    // ⚠ 必须放在上面那条循环**之后**：它会把落盘的语言下标重新应用一遍，
    //   存档里写着"英文"时会把外面设的值盖掉（这个坑和布局落盘那条同源）。
    // ⚠ 只影响本次进程，不落盘 —— 交互式切换仍走 偏好设置 ▸ 外观 ▸ 语言。
    if (const char* lang = std::getenv("MYVK_EDITOR_LANG")) {
        const char c = lang[0];
        const bool zh = (c == 'z' || c == 'Z' || c == 'c' || c == 'C' ||
                         c == '1');
        setLanguage(zh ? 1 : 0);
        VK_LOG_INFO("EDITOR-LANG %s", zh ? "zh" : "en");
    }
}

void EditorApp::applyPrefsFromDisk() {
    std::vector<std::pair<std::string, double>> kv;
    if (!loadEditorPrefs(kv)) return;   // 无存档 / 自动化 → 出厂值
    for (const auto& [key, val] : kv) {
        for (const PrefItem& it : prefsTable()) {
            if (key != it.id) continue;
            if (!it.options.empty()) {
                // 选项型：存的是下标，夹进合法范围再写回
                const int n = static_cast<int>(it.options.size());
                const int idx = std::clamp(
                    static_cast<int>(std::lround(val)), 0, n - 1);
                if (it.setChoice) it.setChoice(idx);
                break;
            }
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
        if (!it.options.empty()) {
            kv.emplace_back(it.id,
                            it.getChoice ? static_cast<double>(it.getChoice())
                                         : 0.0);
            continue;
        }
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
    if (!m_characterWindow) return;
    // 处理"创建角色后打开详情窗口"的请求
    const ecs::Entity req = m_ctx->takeCharacterWindowRequest();
    if (req.valid()) {
        m_characterWindow->open(req);
        // 注册表也要跟上 —— 否则托盘/View 菜单里那一格是空的，而窗口已经开着
        setPanelOpen(panelIndexOf("character"), true);
        VK_LOG_INFO("PANEL character open=1 reason=create-request");
    }

    // ---- 注册表 ⇄ 窗口 双向同步（每帧无条件跑，理由见 panelDrawCharacter）----
    const int idx = panelIndexOf("character");
    if (idx >= 0) {
        // ⚠ 2026-10-18 用户报的 bug（原话："最小化按钮点击会消失且无法正常
        //   最小化窗口，此后叉号也退不出去"）：
        //
        //   `drawPanelMinimizeButtons` 的"最小化"= 把 `panelTrayHiddenRef`
        //   置 1（收进底栏托盘）。其它浮动面板靠 `drawPanels` 里那句
        //   `if (panelTrayHiddenRef(i)) continue;` 生效 —— 但 Character 的
        //   窗口是**由本函数无条件绘制的**（它有独立的 m_open 生命周期），
        //   于是：位被置了、按钮因为 hidden 被跳过而消失、窗口却照画。
        //   表现正是"点一下按钮就没了，窗口一点没缩小"。
        //
        //   修法：这里也认这个位。窗口对象**保持打开**（不动 m_open），
        //   只是这一帧不提交 —— 从底栏托盘按钮恢复时它原样回来。
        if (panelTrayHiddenRef(idx) && panelOpen(idx)) {
            m_characterWasOpen = m_characterWindow->isOpen();
            return;
        }

        const bool wantOpen = panelOpen(idx);
        const bool isOpen = m_characterWindow->isOpen();
        if (wantOpen && !isOpen && m_characterWasOpen) {
            // 用户点的标题栏 ×：注册表跟着关，否则下一帧又会把它打开
            setPanelOpen(idx, false);
            // 顺手把"托盘收起"位清掉：关掉的窗口不该还挂着"最小化"状态，
            // 否则下次从菜单重新打开时那颗最小化按钮会继续缺席。
            panelTrayHiddenRef(idx) = false;
            VK_LOG_INFO("PANEL character open=0 reason=user-close");
        } else if (wantOpen && !isOpen) {
            m_characterWindow->open(m_ctx->hasSelection() ? m_ctx->selection()
                                                          : ecs::Entity{});
            VK_LOG_INFO("PANEL character open=1");
        } else if (!wantOpen && isOpen) {
            m_characterWindow->close();
            VK_LOG_INFO("PANEL character open=0");
        }
        m_characterWasOpen = m_characterWindow->isOpen();
    }

    // 每帧绘制（窗口内部自己判断是否打开）
    //
    // 2026-10-21：角色详情也是**视口标签页的一页**（它是表里唯一 "draw 是
    // 空壳、窗口由这里无条件画" 的面板，所以托管只能在这一层做）。
    //   · 不是当前标签页 → 整块不画（窗口对象活着，切回来原样恢复）
    //   · 是当前标签页  → 推 dockHint，CharacterWindow::draw 里 applyDockHint
    //                     会把矩形钉到内容区、并去掉标题栏
    const int activeTab = activeViewportTab();
    if (idx >= 0 && panelOpen(idx) && activeTab == idx &&
        !panelTrayHiddenRef(idx)) {
        const Rect& vp = m_ctx->layout().viewport;
        pushDockHint(vp.x, vp.y, vp.w, vp.h);
        m_characterWindow->draw();
        clearDockHint();
    }
    // 不是当前页就**不提交** —— 这正是标签页的语义。窗口对象活着
    // （m_open 不变），下次切回来时从自己的状态原样恢复。
}

// ---------------------------------------------------------------- 剧本编辑器
//
// 三个入口，全部汇到 `DialogueEditorWindow::open(path)`：
//   ① Inspector 的 dialogue 页签 → "Edit script…"（走 ctx 请求）
//   ② View / Window 菜单或底栏托盘（走面板表，空手开窗）
//   ③ `MYVK_EDITOR_DLGED=<path>`（自动化）
//
// ⚠ 显隐的**唯一真源是面板表那一格**（panelOpen("dialogueed")），窗口对象
//   只是跟着它走。反过来的写法（窗口自己说了算，面板表去追）会在"点 × 关
//   窗口"时打架：窗口关了、表里还勾着 → 下一帧同步又给它打开（Character
//   在 2026-10-17 踩过这个坑，见 panelDrawCharacter 的长注释）。
void EditorApp::drawDialogueEditorWindow() {
    if (!m_dlgEditor) return;
    const int idx = panelIndexOf("dialogueed");
    if (idx < 0) return;

    // ---- ① 请求：Inspector 点了 "Edit script…" ----
    std::string reqPath;
    if (m_ctx->takeDialogueEditorRequest(reqPath)) {
        m_dlgEditor->open(reqPath);
        setPanelOpen(idx, true);
        panelTrayHiddenRef(idx) = false;
        VK_LOG_INFO("PANEL dialogueed open=1 reason=edit-request path=%s",
                    reqPath.c_str());
    }

    // ---- ② 托盘收起：窗口对象保持打开，这一帧不提交 ----
    // 与 Character 同款：否则"最小化"会把窗口收走、按钮也跟着消失。
    if (panelTrayHiddenRef(idx) && panelOpen(idx)) return;

    // ---- ③ 表 → 窗口 ----
    bool& open = panelOpenRef("dialogueed");
    if (!open) {
        if (m_dlgEditor->isOpen()) {
            m_dlgEditor->close();
            VK_LOG_INFO("PANEL dialogueed open=0");
        }
        return;
    }
    if (!m_dlgEditor->isOpen()) {
        // 从菜单/托盘打开且手上没有资产：空手开窗（只剩 "New script" 与
        // 路径输入框）。不自动猜"当前选中的 NPC 挂的那份" —— 选中项可能
        // 是个灯，猜错等于把一份无关剧本怼到作者脸上。
        m_dlgEditor->open(std::string());
        VK_LOG_INFO("PANEL dialogueed open=1 reason=menu");
    }

    // ---- ④ 画 ----
    // `keep` 是这一帧的"还想开着吗"：窗口的 × 会把它置 false，我们再把
    // 面板表那一格跟着关掉 —— 双源冲突就此消失。
    bool keep = true;
    m_dlgEditor->draw(keep);
    if (!keep) setPanelOpen(idx, false);
}

// ---------------------------------------------------------------- 时间轴编辑器
//
// 与上面那个**逐行同构**（#311）。三个入口：
//   ① Inspector 的 sequence 页签 → "Edit timeline…"（走 ctx 请求）
//   ② View / Window 菜单或底栏托盘（走面板表，空手开窗）
//   ③ `MYVK_EDITOR_TLED=<path>`（自动化）
//
// ⚠ 两个资产编辑器刻意写成同一个形状：差别只在"打开时带不带资产路径"。
//   如果哪天要给它们加"关闭前确认"，应该改一处、两处都受益 —— 而不是
//   因为抄得走样，变成两种行为。
void EditorApp::drawTimelineEditorWindow() {
    if (!m_tlEditor) return;
    const int idx = panelIndexOf("timelineed");
    if (idx < 0) return;

    // ---- ① 请求：Inspector 点了 "Edit timeline…" ----
    std::string reqPath;
    if (m_ctx->takeTimelineEditorRequest(reqPath)) {
        m_tlEditor->open(reqPath);
        setPanelOpen(idx, true);
        panelTrayHiddenRef(idx) = false;
        VK_LOG_INFO("PANEL timelineed open=1 reason=edit-request path=%s",
                    reqPath.c_str());
    }

    // ---- ② 托盘收起：窗口对象保持打开，这一帧不提交 ----
    if (panelTrayHiddenRef(idx) && panelOpen(idx)) return;

    // ---- ③ 表 → 窗口 ----
    bool& open = panelOpenRef("timelineed");
    if (!open) {
        if (m_tlEditor->isOpen()) {
            m_tlEditor->close();
            VK_LOG_INFO("PANEL timelineed open=0");
        }
        return;
    }
    if (!m_tlEditor->isOpen()) {
        // 空手开窗：只剩 "New sequence" 与路径输入框。
        m_tlEditor->open(std::string());
        VK_LOG_INFO("PANEL timelineed open=1 reason=menu");
    }

    // ---- ④ 画 ----
    bool keep = true;
    m_tlEditor->draw(keep);
    if (!keep) setPanelOpen(idx, false);
}

// ---------------------------------------------------------------- UI 编辑器
//
// 与上面两个**逐行同构**（#312）。差别只有一处：它没有 Inspector 入口
// （UI 文档不是场景里的组件），所以三个入口是
//   ① View 菜单 / 底栏托盘（走面板表，空手开窗）
//   ② `MYVK_EDITOR_UIED=<path>`（自动化）
//   ③ 将来：Content 里双击 `.ui.json`（等 ContentBrowser 认识这个扩展名）
void EditorApp::drawUiEditorWindow() {
    if (!m_uiEditor) return;
    const int idx = panelIndexOf("uied");
    if (idx < 0) return;

    // ---- ① 请求：`MYVK_EDITOR_UIED` ----
    std::string reqPath;
    if (m_ctx->takeUiEditorRequest(reqPath)) {
        m_uiEditor->open(reqPath);
        setPanelOpen(idx, true);
        panelTrayHiddenRef(idx) = false;
        VK_LOG_INFO("PANEL uied open=1 reason=edit-request path=%s",
                    reqPath.c_str());
    }

    // ---- ② 托盘收起：窗口对象保持打开，这一帧不提交 ----
    if (panelTrayHiddenRef(idx) && panelOpen(idx)) return;

    // ---- ③ 表 → 窗口 ----
    bool& open = panelOpenRef("uied");
    if (!open) {
        if (m_uiEditor->isOpen()) {
            m_uiEditor->close();
            VK_LOG_INFO("PANEL uied open=0");
        }
        return;
    }
    if (!m_uiEditor->isOpen()) {
        // 空手开窗：只剩 "New"（与另两个资产编辑器同一条路）。
        m_uiEditor->open(std::string());
        VK_LOG_INFO("PANEL uied open=1 reason=menu");
    }

    // ---- ④ 画 ----
    bool keep = true;
    m_uiEditor->draw(keep);
    if (!keep) setPanelOpen(idx, false);
}

} // namespace editor
