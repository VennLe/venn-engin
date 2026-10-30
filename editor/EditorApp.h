#pragma once
// ============================================================
// editor/EditorApp —— 编辑器应用（继承 core::Application）
//
// 这是把整个编辑器接到引擎主循环上的地方，关键只有两个接缝：
//
//   1) activeScene() override
//      Application::run() 里"更新哪个场景、渲染哪个场景"全部改问它。
//      编辑器在这里返回 EditorContext::activeScene() —— 编辑态或运行态，
//      由 Play 状态决定。引擎其余部分一行都不用改。
//
//   2) renderer().setViewportSize(w, h)
//      由 ViewportPanel 每帧按面板大小设置 → 3D 场景不再直接写交换链，
//      而是写进一张离屏图，再由 ImGui::Image 贴进面板。
//
// 编辑器的职责边界：
//   · onUpdate 里**只有在 Play 态**才跑脚本 —— 而且跑在"运行态副本"上。
//     渲染则通过 activeScene() 直接落到同一个视口，不需要第二个窗口。
//   · 视口导航是**编辑器独有的**：编辑态按 UE 的习惯驾驶相机
//     （按住右键转头 + WASD 飞行、中键平移、滚轮推拉，见 ViewportPanel）。
//     Play 态视口不抢任何键鼠输入 —— 那时相机归游戏自己管。
//   · 全屏：Play 态按 F（或点按钮）会把**编辑器窗口本身**切成真全屏
//     （GLFW 显示器全屏），同时让视口独占整屏；退出时恢复原窗口几何。
//     另有 F11 = 只切窗口全屏（面板照旧），编辑态也能用。
//   · 窗口几何：第一次启动主显示器居中占 70%，之后按上次关闭时的存档恢复
//     （见 core/WindowGeometry）。
//   · 所有面板的绘制顺序、布局矩形、快捷键都在这里统一编排
//
// 快捷键有一条容易踩的交叉线：W / E / R 既是手柄模式切换，又是飞行键。
// 规则是"按住右键（= 正在驾驶相机）时飞行优先"，见 handleShortcuts。
//
// 编辑器是 venn 引擎唯一的应用层产物（曾经还有一个 game/SandboxGame
// 独立示例程序，2026-10-09 按用户要求删除）。engine/ 依然不认识 editor/，
// 依赖方向保持单向。
// ============================================================

#include "core/Application.h"
#include "animation/AnimatorSystem.h"
// 四态 locomotion（P4）：编辑器 Play 与运行时共用同一份实现
#include "animation/Locomotion.h"
#include "audio/AudioSystem.h"
#include "script/ScriptRuntime.h"

#include "ConsolePanel.h"    // E16：m_console 成员需要完整类型（unique_ptr 析构）
#include "NotificationCenter.h"  // E21：m_notifications（同上，unique_ptr 析构）
#include "GpuTimelinePanel.h"    // E25：m_gpuTimeline（同上）
#include "PanelRegistry.h"   // PanelDesc（面板表是 EditorApp 的静态成员）
// E29-2：面板拖拽换位。只放**纯数据**结构（DragInput / PanelDragState），
// 这个头文件仍然不 include imgui.h —— 拖拽的 ImGui 读取全在 .cpp 里。
#include "PanelDragDrop.h"
#include "Toolbar.h"         // EditorHost（菜单里那些"要动引擎"的动作走它）

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// 物理世界（R7-1）。只前向声明 —— 完整类型只在本类的 .cpp 里需要，头文件
// 不必把整个 physics 模块拖给每一个 include 它的翻译单元。
//
// ⚠ 必须声明在 `namespace editor` **外面**。写在里面的话就成了嵌套的
//   `editor::physics`，它会把名字查找里外层的 `::physics` 遮住 —— 后果是
//   .cpp 里所有 `physics::xxx` 全部找不到（编译器报的是
//   `"resolveBodyOverlaps" 不是 "editor::physics" 的成员`，看起来像少
//   include，其实是这里多了一层命名空间）。
namespace physics {
class PhysicsWorld;
}

// 脚本引擎（G02 之后住在 engine/script/，namespace 也从 editor 改成 vks）。
// ⚠ 与上面 physics 完全同一条理由：必须声明在 `namespace editor` **外面**，
//   否则会变成嵌套的 `editor::vks`，把外层的 `::vks` 遮住。
namespace vks {
class ScriptEngine;
}

namespace editor {

class EditorContext;
class Toolbar;
class ViewportPanel;
class SceneHierarchy;
class InspectorPanel;
class ContentBrowser;
class SplitLayout;
class CharacterWindow;
class PhysicsPanel;
class CinematographyPanel;
class ParticlesPanel;
class QualitySettingsPanel;   // E26
class MaterialLibraryPanel;   // 2026-10-24：材质仓库
class DialogueEditorWindow;   // 2026-10-27：剧本编辑器（#310）
class TimelineEditorWindow;   // 2026-10-27：时间轴轨道编辑器（#311）
class UiEditorWindow;         // 2026-10-27：UI 编辑器（#312）
class CurveEditorPanel;       // 2026-10-28（R6）：曲线编辑器（Graph Editor）
class DopeSheetWindow;        // 2026-10-28（R8）：动画摄影表（Dope Sheet）
class BoneCurvePanel;         // 2026-10-29（S-D）：骨骼曲线编辑器
class NlaPanel;               // 2026-10-29（R9）：NLA 时间轴
class LandscapeEditor;        // 2026-10-20：地形编辑模式
class FoliageEditor;          // 2026-10-20：植被编辑模式
// 2026-10-20 晚：下拉里其余 6 个模式（ToolkitEditors.h / .cpp）
class MeshPaintEditor;
class ModelingEditor;
class FractureEditor;
class BrushEditor;
class AnimationEditor;
class PCGEditor;
class WaterEditor;            // 2026-10-21：水体编辑模式
class ParamEditCommand;

// EditorApp 同时实现 EditorHost —— 菜单里那些"要动引擎"的动作
// （切工作区 / 换视图模式 / 读 GPU 型号）都在这里落地。Toolbar 只知道
// "我要切工作区"，不知道它是怎么实现的（见 Toolbar.h 的说明）。
class EditorApp : public core::Application, public EditorHost {
public:
    EditorApp();
    ~EditorApp() override;

    // ---------------------------------------------------------- EditorHost
    void applyWorkspace(int index) override;
    void saveWorkspace() override;
    void resetWorkspace() override;
    void applyViewportLayout(int layout) override;
    int currentViewportLayout() override;
    void applyViewMode(int mode) override;
    int currentViewMode() override;
    void reloadShaders() override;
    void reloadAssets() override;
    const char* gpuName() override;
    const char* engineVersion() override;

    // ---- 顶栏播放条：前进一帧 ----
    // 语义是"进入暂停（有运行态）后推进一个固定步长"。真正的一帧要走
    // 脚本 → 玩家 → 物理 → 碰撞这条链，全在本类手里，Toolbar 只发意图。
    void stepOneFrame() override;

    // ---- E18 命令面板 ----
    // Help ▸ Command Palette… 与 Ctrl+Shift+P 都落到这里（Toolbar 拿不到
    // 面板状态，所以走 EditorHost 这条窄接口，与 applyWorkspace 一个套路）。
    void showCommandPalette() override;

    // ---- E21 通知历史（Help 菜单入口）----
    // 与 showCommandPalette 同一个理由：Toolbar 拿不到窗口状态位。
    void showNotificationHistory() override;

    // ---- Edit ▸ Duplicate ----
    // 真正的复制在 `SceneHierarchy::duplicateSelection()`（它才知道
    // uniqueName / 一批一次 structuralEdit 那套约定）。Toolbar 只认
    // EditorContext，拿不到 Hierarchy 对象 —— 所以走这条窄接口。
    void duplicateSelection() override;

    // ---- File ▸ Import ▸ Import assets... ----
    // 原生文件对话框在 ContentBrowser 里（`OPENFILENAMEW` + 不覆盖同名
    // 的 uniqueChildPath）。这里只转发"请打开它"这个意图 —— 实际弹窗必须
    // 等本帧 ImGui 数据提交完（ContentBrowser 用 m_importRequest 延后一帧）。
    void requestImportDialog() override;

    // ---------------------------------------------------------- E04 面板表
    // 表本体在 EditorApp.cpp（表项里的 draw 指针要指向本类的私有绘制函数）。
    // 为什么是**静态成员**而不是"自由函数 + friend"：自由函数要拿私有成员
    // 就得 friend，而 friend 声明必须和 PanelRegistry.h 里的声明逐字一致 ——
    // 那种"两处同步"是编译期查不出来的坑。静态成员天然有访问权。
    static const std::vector<PanelDesc>& panelTableVec();
    static std::vector<PanelDesc> buildPanelTable();

protected:
    void onInit() override;
    void onUpdate(float dt) override;
    void onImGui() override;
    // 窗口还活着时保存窗口几何，供下次启动恢复
    void onShutdown() override;

    // 引擎问"现在该更新/渲染哪一份场景" —— 编辑器的核心接缝
    scene::Scene& activeScene() override;

private:
    void computeLayout();
    void handleShortcuts();

    // Play 态的鼠标捕获同步（P8，2026-10-23）。
    //
    // 三件事，按顺序：
    //   ① ViewportPanel 报告"左键点了视口" → 进入操作态（隐藏 + 锁定光标）
    //   ② 不在 Playing 了 → 强制交还（暂停 / Stop / 崩溃恢复都走这条）
    //   ③ 状态真的变了才去碰 GLFW 与 ImGui（每帧写光标模式会让 GLFW 反复
    //     warp 光标，还会把 discardNextMouseDelta 用光）
    //
    // Esc 的**按键判定不在这里**：它和"退出全屏"抢同一个键，必须和
    // `handleShortcuts` 里那段放在一起用 if/else 分流，否则一帧会既交还
    // 鼠标又退出全屏。
    void updatePlayMouse();

    // 每帧把所有"浮在分栏之上"的窗口矩形喂给分栏器（见 SplitLayout::addOccluder）。
    // 必须排在**所有窗口都画完**之后、handleSplitters 之前调用。
    void collectSplitOccluders();

    // ---------------------------------------------------------- E04 面板表
    // 表项里的 draw 入口（静态薄壳 → 转调到成员函数；表的定义见
    // EditorApp.cpp 的 buildPanelTable）
    static void panelDrawHierarchy(EditorApp& a);
    static void panelDrawContent(EditorApp& a);
    static void panelDrawInspector(EditorApp& a);
    static void panelDrawViewport(EditorApp& a);
    static void panelDrawStats(EditorApp& a);
    static void panelDrawScripts(EditorApp& a);
    static void panelDrawEngine(EditorApp& a);
    static void panelDrawConsole(EditorApp& a);
    static void panelDrawGpuTimeline(EditorApp& a);
    // 2026-10-17 晚新增的三张（底栏托盘可勾选）
    static void panelDrawCharacter(EditorApp& a);
    static void panelDrawPhysics(EditorApp& a);
    static void panelDrawParticles(EditorApp& a);
    // E26：画质档位面板
    static void panelDrawQuality(EditorApp& a);
    // 2026-10-30：运镜面板（Cinematography）
    static void panelDrawCinematography(EditorApp& a);
    // 2026-10-20：编辑模式自带的两张面板（地形 / 植被）
    static void panelDrawLandscape(EditorApp& a);
    static void panelDrawFoliage(EditorApp& a);
    static void panelDrawMeshPaint(EditorApp& a);
    static void panelDrawModeling(EditorApp& a);
    static void panelDrawFracture(EditorApp& a);
    static void panelDrawBrushEdit(EditorApp& a);
    static void panelDrawAnimation(EditorApp& a);
    static void panelDrawPCG(EditorApp& a);
    static void panelDrawWater(EditorApp& a);
    static void panelDrawMaterialLibrary(EditorApp& a);
    // 剧本编辑器：和 Character 一样，窗口对象自己管 open/close，表项只做
    // 显隐同步（真正的 draw 在 drawDialogueEditorWindow 里，每帧无条件跑）
    static void panelDrawDialogueEditor(EditorApp& a);
    // 时间轴编辑器（#311）：与 Script editor 完全同构（窗口对象自己管
    // open/close，表项只做显隐同步），真正的 draw 在 drawTimelineEditorWindow。
    static void panelDrawTimelineEditor(EditorApp& a);
    // UI 编辑器（#312）：与上面两个完全同构（窗口对象自己管 open/close，
    // 表项只做显隐同步），真正的 draw 在 drawUiEditorWindow。
    static void panelDrawUiEditor(EditorApp& a);
    // 曲线编辑器（R6）：同上（窗口对象自己管 open/close），真正的 draw 在
    // drawCurveEditorWindow。
    static void panelDrawCurveEditor(EditorApp& a);
    // 动画摄影表（R8）：与上面四格完全同构，真正的 draw 在
    // drawDopeSheetWindow。
    static void panelDrawDopeSheet(EditorApp& a);
    // 骨骼曲线（S-D）：与上面几格同构，真正的 draw 在 drawBoneCurveWindow。
    static void panelDrawBoneCurve(EditorApp& a);
    // NLA 时间轴（R9）：同样同构，真正的 draw 在 drawNlaWindow。
    static void panelDrawNla(EditorApp& a);

    // 表驱动派发：遍历表，画所有"开着 + 该画"的面板
    void drawPanels();
    // 画一块"待建设"的占位（表里 draw == nullptr 的非视口面板）
    void drawPlaceholderPanel(int panelIndex);

    // ---------------------------------------------------------- E05 工作区
    // 施加某套工作区：面板显隐 + 分栏比例 + 视口布局 + 视图模式。
    // 只有本类同时拿得到 SplitLayout / Renderer / 面板状态，所以放在这儿。
    // （applyWorkspace / saveWorkspace / resetWorkspace 是 EditorHost 的
    //   实现，声明在上面。）
    // 当前实际布局是否偏离了该工作区的出厂预设（标题栏的 * 号）
    bool workspaceModified();
    // 视图模式名（菜单用）
    static const char* viewModeLabel(int mode);

    // ---------------------------------------------------------- 布局持久化
    void loadEditorLayoutFromDisk();   // onInit
    void saveEditorLayoutToDisk();     // onShutdown
    // 把"视口布局"同步给渲染器（子视口数量）+ 记录日志
    void syncViewportLayout();
    // 让 GLFW 顶层窗口的全屏状态跟上"游戏全屏"逻辑状态。
    // **只在状态发生变化的那一帧动窗口** —— 每帧无条件同步会把 F11
    // （纯窗口全屏）立刻覆盖掉。
    void syncWindowFullscreen();
    // G14-7/8：每帧喂音频 —— 取走试听请求、同步 Play 态声源、推进设备侧。
    void syncEditorAudio(float dt);
    // F11：只切窗口全屏（面板照旧），编辑态也能用
    void toggleWindowFullscreen();
    // 把编辑场景立刻落盘到当前场景路径（Ctrl+S 与 Content 工具栏的
    // "Save All" 共用这一条路径 —— 两处各写一遍必然漂移）
    void saveSceneNow();
    // 把编辑态场景引用到的脚本全部加载 + 编译一遍（只为在面板上暴露错误；
    // 真正执行它们的地方在下面的 runScriptsOn）
    void preloadScripts();
    // 脚本 tick 内核：Run（运行态副本）与动画（编辑场景）共用。
    // `simTime < 0` → 实例自己累加本地时间（仿真态）；否则跟随外部时间轴
    // （动画预览要能拖、能回放）。语义见 `ScriptRuntime.h` 的文件头。
    void tickScripts(scene::Scene& sc, double simTime, float dt, const char* tag,
                     uint64_t& summaryFrame);
    // 每帧在**运行态副本**上执行一遍脚本（只在 Run 态调用）
    void runRuntimeScripts(float dt);
    // 每帧驱动**运行态副本**上的玩家（只在 Run 态、脚本之后 / 碰撞之前调用）：
    // WASD 移动 + 鼠标转视角（第一人称）或环绕轨道相机（第三人称），
    // 并把运行态相机摆到玩家视野 / 轨道位上。第三人称顺带驱动 walk 摆腿。
    void runPlayers(float dt);
    // 编辑器 Play 态的"脚下地面高度"（米）：
    //   · 运行态副本里有可见地形 → 地形高度场（与出包后是同一份算法）；
    //   · 没有地形 → 0（编辑器里那块 0 高度的参考地面）。
    // 只用**竖直方向**（抬脚 / 落地），水平方向仍由碰撞求解负责。
    // ⚠ `radius`：角色足迹半径（默认 0.35 = 胶囊出厂半径）。贴地高度取
    //   **足迹内最高点**（中心 + 前后左右 4 点取 max）—— 只取中心一点时，
    //   斜坡上"前脚"那半边比中心高，模型的脚会插进坡面里（2026-10-23）。
    float editorGroundHeight(float x, float y, float radius = 0.35f) const;
    // 玩家状态探针的分帧计数（PLAYER-STATE 每 30 帧一条，防刷屏）
    int m_playerLogTick = 0;
    // Play 态第三人称的平滑臂长（缩短立刻、伸长限速）。跨帧状态，
    // 所以要挂成员而不是局部变量 —— 见 runPlayers 的相机段。
    float m_playArmLen = 0.0f;
    // 每帧在**运行态副本**上做碰撞求解（只在 Run 态、脚本之后调用）：
    //   1) 带碰撞体的实体两两分离 —— "不能互相穿过"
    //   2) 相机当成一个球推出碰撞体 —— "相机视角也不允许穿过"
    void runCollision(float dt);
    // 每帧在**编辑场景**上执行脚本（动画 Play 态调用；Pause 不调）
    void runAnimScripts(float dt);
    // 游戏全屏：视口独占整窗，其余面板一律不画。返回是否处于该状态。
    bool drawFullscreenGame();

    void drawStatsPanel();
    void drawScriptPanel();
    void drawEnginePanel();
    // E09 特殊块：生成器表达不了的四小块（动作型控件 / 只读信息），
    // 各自独立成函数，保持 drawEnginePanel 本体是个薄驱动。
    void drawEngineCameras();
    void drawEngineViewMode();
    void drawEngineMsaa();
    void drawEngineClusterStats();
    // E03：设置层级 —— 覆盖链概览 + Export / Reload。
    // 这里只画"层"这一级；每个参数自己的控件仍在各特性块里。
    void drawEngineSettingsIO();
    // E03：把"当前非默认值"写成 config/render.json。
    // `explicitExport=false` = 用户改过参数后的自动保存；true = 面板上的
    // Export 按钮（全默认时也照写，因为那是用户明确要求的）。
    void saveRenderSettings(bool explicitExport);
    // E16：Console 面板（日志汇入 + 表驱动命令）。
    void drawConsolePanel();
    void registerConsoleCommands();
    // E25：GPU 时戳时间轴（甘特图 + 60 帧历史 + 点段联动 Console）。
    void drawGpuTimelinePanel();
    // E21：通知 / 进度 —— 状态栏计数与 toast 的接线（见 NotificationCenter.h）。
    //   ① 打开 Console 并滚到某条日志（点 toast / 历史项的落地）
    void serviceNotificationFocus();
    //   ② 自动化钩子：MYVK_EDITOR_TOAST / MYVK_EDITOR_ASYNC_TASK（第 40 帧起）
    void driveNotificationHooks();
    //   ②b MYVK_EDITOR_TOAST_CLICK=<idx>：第 60 帧"点"那条 toast
    void driveToastClickHook();
    //   ③ 进度条打开时把任务数喂给状态栏锚点
    void logNotificationStatus();
    // E18：把"动作"登记进命令注册表（命令面板 / 快捷键表 / 总览共用一张表）。
    void registerEditorCommands();
    bool& commandPaletteRef() { return m_showCommandPalette; }
    // E17：偏好表驱动（单项重置 / 搜索 / Reset All / 落盘）。
    // ⚠ PrefItem::ptr 返回 void*（指向 float 或 bool，看 isBool）——
    //   设置项的存储分散在 EditorContext / Renderer，地址走访问器拿。
    //
    // 另有一条"选项型"支路（Theme / Language 这类枚举）：`isChoice` 为真时
    // ptr 不再使用，改走 getChoice / setChoice，落盘存的是**下标**。
    struct PrefItem {
        const char* id;        // 稳定键（落盘 + ImGui ID + 搜索文本）
        const char* label;
        const char* category;  // 页名 / 搜索结果分组
        bool isBool;
        float def;             // 出厂默认（重置用；选项型是默认下标）
        float lo, hi;          // Float 档的 UI 范围
        std::function<void*(EditorApp&)> ptr;

        // ---- 选项型（下拉框）----
        // optionCount > 0 时这一项画成下拉：显示 options[i]（过 TR），
        // 取值是下标。两个 lambda 直接操作真正的存储。
        std::function<int()> getChoice;
        std::function<void(int)> setChoice;
        // 改完之后的副作用（换主题要立刻 applyTheme）。可空。
        std::function<void(int)> onChoiceChanged;
        std::vector<const char*> options;   // 英文选项名（语言层负责翻译）
    };
    static const std::vector<PrefItem>& prefsTable();
    void drawPrefRow(const PrefItem& it);
    void applyPrefsFromDisk();
    void savePrefsToDisk();
    // 把当前主题 / 语言真正应用下去（启动读盘后、以及下拉框改动后都要调）
    void applyAppearance();
    bool m_confirmResetAll = false;   // Reset All 的二次确认状态
    // 界面语言是**谁定的**（"system" / "prefs" / "env" / "automation"）。
    // 只给 `EDITOR-LANG lang=… src=…` 那行日志用：没有它，"为什么是中文"
    // 与"为什么是英文"在日志里长得一模一样，而这两个结论的排查方向相反。
    const char* m_langSrc = "system";
    // FeatureUIGen 的参数变更钩子（FTR-UI 日志 + 个别参数的副作用）。
    void onFeatureParamChanged(const char* paramId);
    // E03：用户改过渲染参数 → 需要落盘（"首次改参数时才写文件"）。
    // 用**计数**而不是 bool：拖动滑条时每帧都会回调，用计数才能在
    // 落盘后清干净（bool 会被"落盘那一刻又来一次改动"吃掉）。
    int m_renderSettingsDirty = 0;
    // E19：参数编辑结束 → 推一条可撤销的 ParamEditCommand（500ms 内同一
    // 参数连续编辑合并成一条）。是 FeatureUIGen 的 undo 钩子的落地实现。
    void onFeatureParamUndo(const char* paramId, const void* before,
                            const void* after, std::size_t size);
    void drawStatusBar();
    void drawSettingsPanel();
    // 角色详情窗口（创建第一/第三人称角色时弹出）
    void drawCharacterWindow();
    // 剧本编辑器（#310）：接管 Inspector dialogue 页签的 "Edit script…"
    // 与 View 菜单里的入口，并负责面板勾选 ↔ 窗口 × 的双向同步。
    void drawDialogueEditorWindow();
    // 时间轴编辑器（#311）：同上，入口是 sequence 页签的 "Edit timeline…"。
    void drawTimelineEditorWindow();
    // UI 编辑器（#312）：入口是 View 菜单 / 托盘 / `MYVK_EDITOR_UIED`。
    // 与另两个资产编辑器不同，它**没有** Inspector 入口 —— UI 文档不是
    // 场景里的组件，没有哪一行能挂一个 "Edit…" 按钮。
    void drawUiEditorWindow();
    // 曲线编辑器（R6）：入口是 View 菜单 / 底栏托盘 / `MYVK_EDITOR_CURVED`。
    // 它**没有** Inspector 入口 —— 曲线是"段的属性"，而 Inspector 的
    // sequence 页签是按组件给的，挂不上"编辑第 2 段的手柄"这种粒度。
    void drawCurveEditorWindow();
    // 动画摄影表（R8）：入口是 View 菜单 / 底栏托盘 / `MYVK_EDITOR_DOPE`，
    // 以及 Character 窗口 Pose 页的 "Dope sheet" 按钮。
    //
    // ⚠ 它比前几个资产编辑器多一个"绑定"形态：Pose Mode 打开时编辑的是
    //   CharacterWindow 内存里那份 `EditClip`（见 DopeSheetWindow.h ⚠⑤），
    //   这时面板表只是显隐开关，数据源由 CharacterWindow 每帧推过来。
    void drawDopeSheetWindow();
    // 骨骼曲线（S-D）：入口是 View 菜单 / 底栏托盘 / `MYVK_EDITOR_BONEC`。
    // 与相机曲线编辑器**不是同一个窗口**（见 BoneCurvePanel.h 开头那段：
    // 手柄归属的根本差别）。
    void drawBoneCurveWindow();
    // NLA 时间轴（R9）：与上面同构。
    void drawNlaWindow();
    // 视口正下方那一行播放条（Run = 运行游戏；Play/Pause/Stop = 动画）。
    //
    // ⚠ 2026-10-17：UI 被用户要求移除，**这个函数不再被调用**（保留实现
    //   是为了将来恢复时不用从 git 里翻）。computeLayout 里 transport 矩形
    //   恒为 0×0，即使误调也画不出来。
    void drawTransportBar();

    // ---- 底部通栏 + Content 抽屉（2026-10-17 布局改造）----
    // 底栏：左侧三个带图标的按钮（内容 / 输出日志 / CMD），右侧资源统计。
    void drawBottomBar();
    // 一个"Logo + 文字"的底栏按钮。active = 对应面板当前是否打开（高亮）。
    bool barButton(const char* id, const char* label, int icon,
                   bool active, const char* tip);

    // ---- 编辑模式（2026-10-20）----
    // 顶栏那颗下拉改的、视口笔刷用的都是它。这里每帧做两件事：
    //   ① 模式变了 → 开关对应的编辑器面板 + 把上一个模式的未落盘改动写完；
    //   ② 未实现的模式（网格体绘制 / 建模 / ...）保持"不假装切过去"。
    void serviceEditMode();
    // 上一帧的编辑模式（差量用）。初值与 EditorContext 一致。
    EditMode m_lastEditMode = EditMode::Select;
    // 自动化：MYVK_EDITMODE=landscape / foliage 启动即进对应模式；
    // MYVK_LANDSCAPE_AUTOCREATE=1 在新场景里自动建一片地形（脚本不必
    // 依赖"点面板按钮"这条含 UI 坐标的路径）。
    bool m_autoCreateTerrain = false;
    bool m_autoCreateDone = false;

    // ---- 底栏面板托盘（2026-10-17 晚）----
    // "CMD 右边一个 +，点开往上弹一列勾选；勾上的面板在 + 右侧出现一个
    //  带 Logo 的按钮，点按开关，按钮右上角的 × 把它移掉"。
    //
    // 托盘按钮的三种结果。用一个枚举而不是三个 bool：调用点的 switch
    // 逼着把"点了按钮"和"点了叉号"分开处理，漏一个编译器就报。
    enum class TrayHit { None = 0, Toggle, Remove };
    TrayHit trayButton(const char* id, const char* label, int icon, bool active,
                       const char* tip);
    // "+" 弹出的勾选列表（向上弹）。每帧无条件提交。
    void drawPanelPicker();
    // 托盘面板的"最小化"状态上报：ImGui 的折叠箭头 / 双击标题栏都会改这个
    // 状态，而它**不产生任何 ImGui 调用**可以挂钩。所以每帧读一次窗口对象，
    // 变了就打一行 PANEL-STATE（自动化据此断言"最小化真的生效了"）。
    void servicePanelTrayState();

    // 浮动面板标题栏右上角的"最小化"按钮（2026-10-17 第四轮）。
    //
    // 用户原话："在 Content 面板这里通过点击 + 打开的所有面板，右上角都要有
    // 可以最小化的按钮，点击最小化即把面板窗口最小化放到 + 按钮右侧原来的
    // 位置上。" —— 也就是收回成底栏那个托盘按钮。
    //
    // 为什么是手工命中 + 手工绘制，而不是塞一个 ImGui 控件：标题栏是 ImGui
    // 在 `Begin()` 内部画完的，面板各自的 `Begin/End` 之外**没有**官方入口
    // 往标题栏里加控件（想加只能整窗换成 NoTitleBar + 自绘标题栏，那会连带
    // 丢掉拖拽 / 折叠 / 双击最大化）。所以这里在**所有面板画完之后**，按窗口
    // 对象算出"关闭 X 左边那一格"，用该窗口自己的 DrawList 画、再手动判命中。
    // 命中前要求 `HoveredWindow == 该窗口`，所以被别的窗口盖住时点不到。
    void drawPanelMinimizeButtons();
    // E29-2：面板标题栏拖拽换位。**必须排在最后**（所有窗口画完之后）——
    // 它要读这一帧真实的 LayoutRects 与鼠标位置，早一帧拿到的是旧矩形，
    // 表现是"拖到一半落点闪一下旧位置"。
    void drawPanelDragLayer();

    // 把某个面板弄到"真的看得见"：清掉托盘的临时收起 + 打开 + 请求焦点。
    // 底栏的 Output Log / CMD 按钮走这条路 —— 上一版直接 `open = !open`，
    // 一旦面板处于"勾着但被收起"的状态，点一下只是把勾取了，屏幕上什么都
    // 不会变，用户看到的就是"点了没反应"。
    void focusPanel(const char* id);
    // 下一帧要 `SetWindowFocus` 的面板 id（空 = 不请求）。窗口还没建起来的
    // 那一帧调 SetWindowFocus 是无效的，所以延后一帧。
    const char* m_focusPanelWant = nullptr;
    // E29-2：拖拽换位的跨帧状态（armed → active → 落下）。
    PanelDragState m_panelDrag;
    // E29-5：角落抓手（两条分隔条的交点）的拖拽态。与 m_panelDrag 并列而
    // 不合并 —— 两者抓的东西不一样（一个抓标题栏、一个抓交点），同一帧
    // 只可能有一个 active，但状态各自独立更不容易互相污染。
    CornerDragState m_cornerDrag;
    // Character 窗口"上一帧是不是开着"。它自己管 `m_open`，点 × 时只改自己
    // 的成员，面板注册表那边还是勾着的 —— 于是下一帧同步逻辑又把它打开，
    // 表现就是"叉号关不掉"。靠这个字段把"用户关的"和"刚被勾上"区分开。
    bool m_characterWasOpen = false;
    // "+" 按钮的屏幕位置（列表要开在它正上方 —— 底栏贴着屏幕底边，
    // 往下弹会直接出屏）。每次画 + 时更新。
    // ⚠ 用 4 个 float 而不是 ImVec2：这个头文件不 include imgui.h（和
    //   SplitLayout::setOccluder 同一条理由）。
    float m_trayPlusX = 0.0f;
    float m_trayPlusY = 0.0f;
    float m_trayBtnH = 24.0f;
    // 上一次上报的（开 / 折叠）状态，按面板表下标。只用来"变化时才打日志"。
    bool m_trayStateOpen[kMaxPanels] = {};
    int m_trayStateMin[kMaxPanels] = {};
    // 抽屉（浮层或停靠，取决于 m_contentDock）。
    void drawContentDrawer();
    // 未停靠时点外部关闭（在所有窗口画完之后调用）。
    void serviceContentDrawerDismiss();

    std::unique_ptr<EditorContext> m_ctx;
    std::unique_ptr<Toolbar> m_toolbar;
    std::unique_ptr<ViewportPanel> m_viewport;
    std::unique_ptr<SceneHierarchy> m_hierarchy;
    std::unique_ptr<InspectorPanel> m_inspector;
    std::unique_ptr<ContentBrowser> m_content;
    // ---- 脚本（S0）----
    // ⚠ **只留一个 tick 内核**：Play（运行态副本）与动画预览（编辑场景）走
    //   同一条 `ScriptRuntime::tick`，区别只在场景与时间源。S0 之前这里是
    //   两份复制粘贴后各自演化的循环 —— 编辑器那份漏接了 HUD/音频，
    //   于是"同一个脚本在编辑器 Play 和 Game.exe 里能力不一样"（P2）。
    std::unique_ptr<vks::ScriptRuntime> m_scriptRuntime;
    // 面板与 preloadScripts 要的资产列表/热重载（按路径共享的那一半）。
    // 裸指针指向 m_scriptRuntime 内部的引擎：生命周期由 runtime 保证。
    vks::ScriptEngine* m_scripts = nullptr;

    // 编辑器里没有 runtime 的 HudModel。但 S0 的"能力一致"要求脚本调
    // setHealth 时**不能静默不响** —— 记一份数值镜像，Scripts 面板显示它。
    // ⚠ 这不是"编辑器也要画血条"：血条是 runtime 的 UI，编辑器只需要
    //   能看见"脚本确实调到了 HUD"。
    struct HudMirror {
        float health = 100.0f;
        int popups = 0;
        float lastPopup = 0.0f;
    };
    HudMirror m_hudMirror;
    std::unique_ptr<SplitLayout> m_split;
    // 2026-10-21：视口顶部那条标签条（浏览器式标签页，见 ViewportTabs.h）。
    // 矩形由 computeLayout 在最后一步从 L.viewport 顶部切出来 —— 于是
    // L.viewport 本身就是"标签条以下的 3D 画面区"，ViewportPanel 一行都不用改。
    // 单独存一份是给 collectSplitOccluders 用的：标签条是独立窗口，
    // 不记进"布局面板"名单就会被当成浮层，把左边的分隔条裁掉一截。
    Rect m_tabBarRect;
    std::unique_ptr<CharacterWindow> m_characterWindow;
    // 2026-10-27：剧本编辑器。它的位置在 CharacterWindow 之后 —— 生命周期
    // 与它同构（构造在 init 里、draw 在 onImGui 末尾）。
    std::unique_ptr<DialogueEditorWindow> m_dlgEditor;
    // 2026-10-27（#311）：时间轴编辑器。生命周期与上面那个同构。
    std::unique_ptr<TimelineEditorWindow> m_tlEditor;
    // 2026-10-27（#312）：UI 编辑器。同样与上面两个同构。
    std::unique_ptr<UiEditorWindow> m_uiEditor;
    // 2026-10-28（R6）：曲线编辑器。同样与上面三个同构。
    std::unique_ptr<CurveEditorPanel> m_curveEditor;
    // 2026-10-28（R8）：动画摄影表。生命周期与上面四个同构 —— 但**销毁次序
    // 有要求**：绑定形态下它持有 CharacterWindow 的 `EditClip*`，
    // 而 unique_ptr 成员按"声明的逆序"析构。所以它必须在 m_characterWindow
    // **之后**声明（= 先析构），否则关窗口时 dope sheet 会拿着悬空指针。
    std::unique_ptr<DopeSheetWindow> m_dopeSheet;
    // 2026-10-29（S-D）：骨骼曲线编辑器。与上面几格同构，但它**不**持有
    // CharacterWindow 的指针 —— 它自己从 `.anim.json` 读一份 `EditClip`
    // （"一个文件一个写者"：Pose Mode 打开时两个窗口各自持副本，靠
    //  存盘前的磁盘时间戳比对来避免静默互相覆盖）。
    std::unique_ptr<BoneCurvePanel> m_boneCurve;
    // 2026-10-29（R9）：NLA 时间轴面板。与上面几格最大的不同：它**不持有
    // 数据** —— strip 住在选中实体的 `AnimatorComponent::nlaTracks` 上
    // （跟场景一起落盘），面板每次 draw 都从场景里取，直接改字段。
    std::unique_ptr<NlaPanel> m_nla;
    // 2026-10-17 晚：底栏"+"托盘里的两张功能面板（Physics / Particles）
    std::unique_ptr<PhysicsPanel> m_physicsPanel;
    std::unique_ptr<ParticlesPanel> m_particlesPanel;
    std::unique_ptr<QualitySettingsPanel> m_qualityPanel;   // E26
    // 2026-10-30：运镜面板。只拿 EditorContext（与 PhysicsPanel 同一条纪律）。
    std::unique_ptr<CinematographyPanel> m_cinePanel;
    // 2026-10-24：材质仓库面板。它要能"把材质设成地形笔刷"，
    // 所以持有一个 LandscapeEditor 裸指针（在构造之后 attach）。
    std::unique_ptr<MaterialLibraryPanel> m_materialLib;
    // 2026-10-20：地形 / 植被编辑模式。它们**不是**普通面板：
    //   · 面板只是它们的 UI，真正干活的是视口里的笔刷；
    //   · 显隐跟顶栏的 EditMode 走（见 serviceEditMode）。
    // 所以由 EditorApp 持有，ViewportPanel 只拿裸指针转发输入。
    std::unique_ptr<LandscapeEditor> m_landscape;
    std::unique_ptr<FoliageEditor> m_foliage;
    // 2026-10-20 晚：下拉里其余 6 个模式的编辑器（见 ToolkitEditors.h）。
    // 与上面两个同一套管理：显隐跟 EditMode 走，视口输入按模式转发。
    std::unique_ptr<MeshPaintEditor> m_meshPaint;
    std::unique_ptr<ModelingEditor> m_modeling;
    std::unique_ptr<FractureEditor> m_fracture;
    std::unique_ptr<BrushEditor> m_brushEdit;
    std::unique_ptr<AnimationEditor> m_animation;
    std::unique_ptr<PCGEditor> m_pcg;
    // 2026-10-21：水体编辑模式（拖出水体 + 水质参数面板）。
    // 与地形/植被同一套管理：显隐跟 EditMode 走，视口输入按模式转发。
    std::unique_ptr<WaterEditor> m_water;
    std::unique_ptr<ConsolePanel> m_console;
    // E16：MYVK_EDITOR_CONSOLE_CMD 注入命令只执行一次的哨兵。
    bool m_consoleCmdDone = false;
    bool m_panelsOpenedOnce = false;
    bool m_scriptTabForced = false;
    // E18：命令面板开合位（Ctrl+Shift+P / Help 菜单）
    bool m_showCommandPalette = false;

    // ---- 底栏 / Content 抽屉状态（2026-10-17）----
    // 抽屉高度（像素）。浮层与停靠共用同一个值 —— 勾上 "Dock in layout"
    // 时不该跳一下高度。
    float m_contentDrawerH = 320.0f;
    // "Dock in layout"：勾上 = 抽屉占布局（视口变矮）、点外部不关；
    // 不勾 = 浮层、点外部即关。默认不勾（用户要求）。
    bool m_contentDock = false;
    // 拖拽抽屉上边缘用的活动标志（ImGui 的 InvisibleButton 已经帮我们
    // 处理"按下/拖出/松开"，这里只记起始高度，避免累加漂移）。
    bool m_contentDragging = false;
    float m_contentDragStartH = 0.0f;
    float m_contentDragStartY = 0.0f;
    // 本帧鼠标是否落在抽屉 / 底栏上（点外部关闭要用）。
    bool m_contentDrawerHovered = false;
    bool m_bottomBarHovered = false;
    // 上一帧抽屉里有没有弹层（右键菜单 / 排序菜单）。
    // 用来把"关弹层的那一下点击"和"点面板外部"区分开 —— ImGui 在
    // NewFrame 里就把弹层关掉了，帧末只能靠这个"上帧快照"判断。
    bool m_contentPopupWasOpen = false;

    // ---- E21：通知 / 进度 ----
    std::unique_ptr<NotificationCenter> m_notifications;
    bool m_showNotificationHistory = false;
    // 自动化钩子（MYVK_EDITOR_TOAST / MYVK_EDITOR_ASYNC_TASK）的一次性哨兵
    bool m_notifHookDone = false;
    bool m_toastClickDone = false;
    bool m_taskDemoActive = false;
    bool m_taskDemoCancelled = false;

    // ---- E25：GPU 时戳时间轴 ----
    // 面板自己按帧采样 60 帧历史（GpuTimer 只存最近一帧）。
    std::unique_ptr<GpuTimelinePanel> m_gpuTimeline;
    // MYVK_EDITOR_GPUTIMELINE=1 开面板只做一次
    bool m_gpuTimelineOpenDone = false;

    // ---- E19：设置类改动的撤销（合并窗口）----
    // 同一个参数、500ms 内的连续编辑合并成一条历史。
    // ⚠ m_lastParamCmd 是指向**命令栈持有的对象**的裸指针，用来判断"栈顶
    //   是不是我刚记的那一条"。栈自己负责它的生命周期；这里只在
    //   `topCommand() == m_lastParamCmd` 时才 deref，且还要 id 与时间窗
    //   都对得上 —— 三重条件同时误判的概率可以忽略。
    ParamEditCommand* m_lastParamCmd = nullptr;
    std::string m_lastParamId;
    double m_lastParamTime = 0.0;

    // 热重载扫描节流（每 0.5s 扫一次脚本文件时间戳）
    float m_hotReloadTimer = 0.0f;
    // 上一次同步给 ScriptEngine 的脚本路径集合（变了才重建缓存）
    std::vector<std::string> m_scriptPaths;

    // 运行态已经跑了多少帧（第一帧打一条脚本执行汇总日志，供冒烟测试确认链路）
    uint64_t m_runtimeFrames = 0;
    // 脚本实例表是否已经在"非运行态"里丢过了。
    //
    // 以前这里是"只要不在 Play 就每帧 reset()"，后果有两个，都很隐蔽：
    //   ① **暂停**也会 reset → 恢复时所有脚本从头再来（`wait` 挂在哪全丢）；
    //   ② 编辑态实例表永远是空的 → Instances 面板一帧都画不出行（S5 F1 红）。
    // 现在改成"进非运行态丢一次，然后按**编辑场景**同步一张 Idle 表"：
    // 面板在编辑态就有东西可看，按下 Play 才真正执行（S-D10）。
    bool m_scriptTableDropped = false;
    // 工程的起始场景（`venn.project.json` 的 `startScene`，资产相对路径）。
    //
    // 启动器 M1/D7：`MYVK_EDITOR_PROJECT` 只告诉我们"打开哪个工程"，工程里
    // 声明了它该打开哪份文档。真正加载仍走既有的 `MYVK_EDITOR_SCENE` 钩子，
    // 这里只是给它一个**默认值**（显式 env 优先）。
    std::string m_projectScene;
    // 动画（编辑场景脚本）的帧计数 —— 与运行态分开，互不干扰。
    // ⚠ 错误去重**不在这里**了：S0 把它搬进了 `ScriptRuntime`
    //   （一份实现，编辑器与 Game.exe 共用），这里只留帧计数打汇总日志。
    uint64_t m_animFrames = 0;
    // 脚本 print() 的滚动日志
    std::vector<std::string> m_printLog;

    // 碰撞求解的详细日志预算。每帧都打的话长时间 Play 会把日志刷爆 ——
    // 但自动化测试又需要看到"到底推开了谁"。折中：前 N 帧逐条打，
    // 之后只每 60 帧打一条汇总。
    //
    // 初值必须是 240 而不是 0：`MYVK_EDITOR_PLAY=1` 是**在启动时**就进
    // Play 的，走不到"回到编辑态"那条复位分支（复位写在那儿）。预算若从
    // 0 起步，第一次 Play 的碰撞会一条日志都不留 —— 求解明明做对了，却
    // 因为"没到每 60 帧的汇总点"而被 `pairs==0` 提前 return 掉。
    // 每次新 Play 开始也会重新灌满（见 EditorApp::onUpdate）。
    int m_collisionLogBudget = 240;
    uint64_t m_collisionFrames = 0;

    // ---- R7-1 物理世界 ----
    // Play 开始时按运行态场景里的 RigidBodyComponent 建一次，之后每帧步进；
    // Stop 时直接丢掉（运行态本来就会被 clear，物理世界跟它同生共死）。
    //
    // 为什么是"Play 才建"而不是常驻：物理世界持有的是**运行态**实体的
    // entity id，编辑态的 id 在每次 Play 时会全部换掉（运行态是重新读一遍
    // 内存 JSON 建出来的）。常驻就得每帧对 id，反而更容易出错。
    std::unique_ptr<physics::PhysicsWorld> m_physics;
    // entity.id → BodyId。写回变换时按它找体；同时保证同帧内不会重复建。
    std::unordered_map<unsigned, unsigned> m_physicsEntityToBody;
    // 一帧最多打几条物理日志（与碰撞日志同一套节流思路）
    int m_physicsLogBudget = 60;
    uint64_t m_physicsFrames = 0;
    // MYVK_PHYS_DUMP=<N>：每 N 帧把每个刚体的名字/位置/速度打一行
    // （`PHYS-BODY ...`）。0 = 关。物理是"结果不对但看不出哪不对"的典型，
    // 有这行才能把数值期望写成断言（见 tools/verify_physics_editor.py）。
    int m_physicsDumpEvery = 0;
    // 按 PhysicsSettings 建世界并把场景里的刚体塞进去（只在 Play 首帧调用）。
    void buildPhysicsWorld();
    // 推进物理并把解算结果写回运行态 TransformComponent。
    void stepPhysics(float dt);
    // 丢掉物理世界（Stop / 退出 Play 时）。
    void destroyPhysicsWorld();

    // ---- 骨骼动画（角色链 A0-3）----
    // 推进 `SkinnedMeshComponent::time` 并重算关节矩阵。
    // 在 Play 分支里排在 physics **之前**（A4 的根运动要成为物理的输入）；
    // 非 Play 态传 dt=0 调一次 —— 只刷新姿势、不推进时间，这样拖时间轴
    // 或改 clip 时视口立刻跟着变。
    animation::AnimatorSystem m_animator;

    // ---- 玩家四态动画（P4：编辑器 Play 也要有走路动画）----
    // 每条 PlayerComponent 实体一份游标（entity.id → 状态机）。
    // 放在编辑器而不是组件里：它是**运行态**（不进场景 JSON，Stop 就丢），
    // 塞进 PlayerComponent 会让存档多出一堆"这次 Play 走到第几步"的垃圾。
    //
    // 判据仍然是"真的在动"：速度取本帧位移 / dt，撞墙时位移为 0 → 播 idle。
    std::unordered_map<unsigned, animation::LocomotionRuntime> m_playerLoco;

    // ---- 音频（G14-7/8）----
    // 编辑器自己也要一份：Inspector 的 ▶ 试听、Content 面板双击试听、
    // Play 态的场景音源 —— 三者都走这里。
    //
    // ⚠ 与运行时**不是同一个实例**（没做成单例，见 audio_roadmap §4.2）：
    //   编辑器里"Play 的那份音频"和"编辑器自己的试听"本来就该互不影响
    //   （试听不该被暂停菜单 duck 掉）。
    // ⚠ 设备打不开是正常工况：进 Failed，所有 API 变空操作，按钮点了没声
    //   不是崩溃。
    audio::AudioSystem m_audio;
    std::vector<audio::SourceUpdate> m_audioSrcs;

    // ---- E08：播放倍速（Transport 的 0.25x / 1x / 2x）----
    // 乘在 dt 上（等价于 Unity 的 Time.timeScale / UE 的 Global Time
    // Dilation）。默认 1.0 —— 不碰它时逐帧行为与本功能引入前完全一致。
    float m_timeScale = 1.0f;

    // ---- E05：自动化用"按帧切工作区"序列 ----
    // MYVK_EDITOR_WORKSPACE 只能定"启动时是哪一套"；而 E05 的验收要求
    // "切过去 → 断言 → 切回来 → 复原"，那就得在**同一个会话内**切两次。
    // 所以另有一个序列钩子：
    //     MYVK_EDITOR_WORKSPACE_SEQ="30:debug 60:level"
    // = 第 30 帧切到 debug、第 60 帧切回 level（帧号从 1 起，空格/逗号都能分隔）。
    // 这条钩子只在自动化里用；界面上切工作区走菜单 / Ctrl+Alt+数字。
    struct WsSwitch {
        uint64_t frame = 0;
        int index = 0;
    };
    std::vector<WsSwitch> m_wsSwitch;
    uint64_t m_editorFrames = 0;
    // 解析上面那个序列（坏格式 / 未知工作区 → WARN 后跳过该条，不整体失败：
    // 一个笔误不该让整轮自动化跑不起来）。
    void parseWorkspaceSequence(const char* spec);

    // 已经同步给 GLFW 窗口的全屏状态（边沿检测用，见 syncWindowFullscreen）
    bool m_appliedWindowFullscreen = false;
};

} // namespace editor
