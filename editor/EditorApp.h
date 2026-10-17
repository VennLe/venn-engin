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

#include "ConsolePanel.h"    // E16：m_console 成员需要完整类型（unique_ptr 析构）
#include "NotificationCenter.h"  // E21：m_notifications（同上，unique_ptr 析构）
#include "GpuTimelinePanel.h"    // E25：m_gpuTimeline（同上）
#include "PanelRegistry.h"   // PanelDesc（面板表是 EditorApp 的静态成员）
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

    // ---- E18 命令面板 ----
    // Help ▸ Command Palette… 与 Ctrl+Shift+P 都落到这里（Toolbar 拿不到
    // 面板状态，所以走 EditorHost 这条窄接口，与 applyWorkspace 一个套路）。
    void showCommandPalette() override;

    // ---- E21 通知历史（Help 菜单入口）----
    // 与 showCommandPalette 同一个理由：Toolbar 拿不到窗口状态位。
    void showNotificationHistory() override;

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
    // F11：只切窗口全屏（面板照旧），编辑态也能用
    void toggleWindowFullscreen();
    // 把编辑态场景引用到的脚本全部加载 + 编译一遍（只为在面板上暴露错误；
    // 真正执行它们的地方在下面的 runScriptsOn）
    void preloadScripts();
    // 脚本执行内核：Run（运行态副本）与动画（编辑场景）共用
    void runScriptsOn(scene::Scene& sc, double t, float dt,
                      std::unordered_map<unsigned, std::string>& errors,
                      const char* tag, uint64_t& summaryFrame);
    // 每帧在**运行态副本**上执行一遍脚本（只在 Run 态调用）
    void runRuntimeScripts(float dt);
    // 每帧驱动**运行态副本**上的玩家（只在 Run 态、脚本之后 / 碰撞之前调用）：
    // WASD 移动 + 鼠标转视角（第一人称）或环绕轨道相机（第三人称），
    // 并把运行态相机摆到玩家视野 / 轨道位上。第三人称顺带驱动 walk 摆腿。
    void runPlayers(float dt);
    // 玩家状态探针的分帧计数（PLAYER-STATE 每 30 帧一条，防刷屏）
    int m_playerLogTick = 0;
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
    // 视口正下方那一行播放条（Run = 运行游戏；Play/Pause/Stop = 动画）。
    // 按钮在这一行的水平正中。
    void drawTransportBar();

    std::unique_ptr<EditorContext> m_ctx;
    std::unique_ptr<Toolbar> m_toolbar;
    std::unique_ptr<ViewportPanel> m_viewport;
    std::unique_ptr<SceneHierarchy> m_hierarchy;
    std::unique_ptr<InspectorPanel> m_inspector;
    std::unique_ptr<ContentBrowser> m_content;
    std::unique_ptr<vks::ScriptEngine> m_scripts;
    std::unique_ptr<SplitLayout> m_split;
    std::unique_ptr<CharacterWindow> m_characterWindow;
    std::unique_ptr<ConsolePanel> m_console;
    // E16：MYVK_EDITOR_CONSOLE_CMD 注入命令只执行一次的哨兵。
    bool m_consoleCmdDone = false;
    // E18：命令面板开合位（Ctrl+Shift+P / Help 菜单）
    bool m_showCommandPalette = false;

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
    // 脚本运行期错误去重：同一个实体同一条错误只打一次日志
    std::unordered_map<unsigned, std::string> m_scriptErrors;
    // 动画（编辑场景脚本）的帧计数与错误表 —— 与运行态分开，互不干扰
    uint64_t m_animFrames = 0;
    std::unordered_map<unsigned, std::string> m_animErrors;
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
