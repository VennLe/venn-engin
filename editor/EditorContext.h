#pragma once
// ============================================================
// editor/EditorContext —— 编辑器的全局状态与服务
//
// 所有面板（Viewport / Hierarchy / Inspector / ContentBrowser /
// Toolbar）只依赖这一个类，彼此之间不互相引用 —— 面板是可以随便
// 增删的，状态只有一份。
//
// ============================================================
// 一、编辑态 / 运行态分离（本类最重要的职责）
// ============================================================
//
//   编辑器持有**两套** scene::Scene：
//
//     m_editorScene  —— 编辑态。你唯一真正在改的那一份。
//                       它上面**不跑任何游戏逻辑**（没有脚本、没有动画），
//                       所见即所得。
//
//     m_runtimeScene —— 运行态。按下 Play 的那一刻，把编辑态整份
//                       **复制**过来（走内存态 JSON 序列化，见
//                       scene::sceneFromJson）。脚本、动画全部只在这份
//                       副本上跑，并且**渲染在同一个视口里**。
//
//   于是：
//     · Play  → 复制 + 开跑（视口切到运行态，可全屏）
//     · Pause → 冻结运行态的每帧更新，但保留它的全部状态
//     · Stop  → 直接丢弃运行态。编辑态从头到尾**一个字节都没被改过**，
//               所以"停止后回到播放前"是天然成立的，不需要额外
//               做任何回滚 —— 这正是分离机制的价值。
//
//   为什么用"复制"而不是"编辑器里直接跑、停止时用快照还原"：
//     后者要求还原必须完美（组件、层级、句柄、资产指针全都要一致），
//     任何一处遗漏都会污染工程。复制法把风险挡在编辑态之外。
//
//   activeScene() 是唯一的分发点：渲染、拾取、gizmo、面板全都问它要
//   "现在该操作哪一份"，其余代码不需要知道 Play 状态。运行时切到
//   m_runtimeScene —— 于是视口里看到的直接就是游戏画面（同一个离屏
//   渲染链、同一套 ImGui 贴图，不需要第二个窗口）。
//
//   全屏（gameFullscreen）：Play 期间可以让视口**独占整个窗口**
//   （工具栏 / 层级 / 检查器全部不画），按 F 或点右上角按钮切换。
//   从 2026-10-09 起它还多一层含义：**编辑器窗口本身也切成真全屏**
//   （GLFW 显示器全屏，铺满主显示器），所以 70% 的窗口一按按钮就真的
//   铺满整屏。窗口那一侧由 EditorApp::syncWindowFullscreen 负责 ——
//   本类不认识 Window，只保存这个逻辑状态。
//
// ============================================================
// 二、其余职责
// ============================================================
//   · 选择（选中实体 + 选中名字；名字是跨"场景重建"的唯一稳定标识）
//   · 命令栈（撤销 / 重做）
//   · gizmo 的模式 / 坐标系；平移 / 旋转 / 缩放各自的**吸附开关与步长**
//   · 地平面栅格的显示开关（真正的绘制在 render/Renderer 里）
//   · 状态栏文字与瞬时提示
//   · 面板可见性
//   · **编辑态视口的导航灵敏度**（视口右上角 HUD 可编辑，右键 + 滚轮可调）
//
// ============================================================
// 三、视口相机：谁是"游戏视角"，谁是"编辑器视角"
// ============================================================
//   两者共用 scene::Camera 的自由飞行模式，但归属完全不同：
//
//     · 编辑态 → 编辑器视口的导航相机。**完全由编辑器驱动**：视口悬停时
//       按 UE 的习惯操作（按住右键转头 + WASD 飞行、中键平移、单独滚轮
//       推 / 拉，右键 + 滚轮调灵敏度）。它受上面的 navSensitivity 控制。
//
//     · Play 态 → 游戏自己的相机。编辑器**不碰它**：视口在 Play 期间
//       不响应任何键鼠导航，相机怎么动由游戏逻辑决定（脚本 / 播放器）。
//       编辑器只负责把它渲染到视口里，以及全屏切换。
//
//   分清这条界线很重要：上一版把导航做进了 Play 态，但用户的意图是
//   "运行前那个视口要好用，运行后归游戏"。
// ============================================================

#include "Command.h"

#include "ecs/Entity.h"
// Scene 是本类的**按值成员**，前向声明不够，必须拿到完整定义
#include "scene/Scene.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace assets {
class AssetManager;
}
namespace editor {

// 手柄模式（W / E / R 切换，与主流编辑器一致）
enum class GizmoMode : int { Translate = 0, Rotate = 1, Scale = 2 };
// 手柄坐标系
enum class GizmoSpace : int { World = 0, Local = 1 };
// 播放状态机
enum class PlayState : int { Stopped = 0, Playing = 1, Paused = 2 };

// 面板矩形（像素，屏幕空间）。编辑器没有用 ImGui 的 docking 分支，
// 改成"每帧算一遍矩形、面板各自 FirstUseEver 落位"—— 效果接近，
// 且不依赖 imgui 的具体分支版本。
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct LayoutRects {
    Rect menu, toolbar, hierarchy, content, inspector, viewport, status;
};

class EditorContext {
public:
    // assets 由 Application 持有，编辑器只借用（与 Scene 共享同一份资源缓存）
    explicit EditorContext(assets::AssetManager& assets);

    assets::AssetManager& assets() { return m_assets; }

    // ---------------------------------------------------------- 编辑态场景
    scene::Scene& editorScene() { return m_editorScene; }
    const scene::Scene& editorScene() const { return m_editorScene; }
    scene::Scene& runtimeScene() { return m_runtimeScene; }
    const scene::Scene& runtimeScene() const { return m_runtimeScene; }

    // 当前该渲染 / 该操作的那一份：Play 期间是运行态副本，否则是编辑态。
    // 渲染、拾取、gizmo、面板全都只依赖它 —— 视口因此能在 Play 时直接
    // 显示游戏画面，不需要第二个窗口。
    scene::Scene& activeScene();
    const scene::Scene& activeScene() const;
    bool usingRuntime() const { return m_play != PlayState::Stopped; }

    PlayState playState() const { return m_play; }
    bool isPlaying() const { return m_play == PlayState::Playing; }
    bool isPaused() const { return m_play == PlayState::Paused; }
    bool isEditing() const { return m_play == PlayState::Stopped; }

    void play();    // Stopped → Playing（复制场景）；Paused → Playing
    void pause();   // Playing → Paused
    void stop();    // 任意 → Stopped（丢弃运行态）
    void togglePlayPause();

    // 整个场景被替换（新建 / 打开文件）后调用：清选择、清历史
    void onSceneReplaced();

    // 运行态已经跑了多久（脚本的 time，也用于 HUD 显示）
    float playTime() const { return m_playTime; }
    void advancePlayTime(float dt) { m_playTime += dt; }

    // ---------------------------------------------------------- 游戏全屏
    // 只在 Play 期间有效：让视口独占整个编辑器窗口（F 键 / 右上角按钮切换），
    // 并且 EditorApp 会把 GLFW 顶层窗口一起切成真全屏。
    bool gameFullscreen() const { return m_gameFullscreen; }
    bool& gameFullscreenRef() { return m_gameFullscreen; }
    void toggleGameFullscreen() { m_gameFullscreen = !m_gameFullscreen; }

    // ---------------------------------------------------------- 视口导航灵敏度
    // **编辑态视口**的导航灵敏度：移动与旋转共用一个系数（UE 视口那套）。
    // 默认 1、必须 > 0；视口右上角 HUD 可直接填数字，**按住右键 + 滚轮**
    // 上滑变大 / 下滑变小（单独滚轮是推拉相机，不碰这个值）。
    //
    // 它**不作用于 Play 态** —— 游戏画面里的相机归游戏自己管，编辑器视口
    // 在 Play 期间不抢任何键鼠输入（见 ViewportPanel 的导航分支）。
    static constexpr float kMinSensitivity = 0.05f;
    static constexpr float kMaxSensitivity = 20.0f;
    float navSensitivity() const { return m_navSensitivity; }
    float& navSensitivityRef() { return m_navSensitivity; }
    void addNavSensitivity(float delta) {
        m_navSensitivity = glm::clamp(m_navSensitivity + delta,
                                      kMinSensitivity, kMaxSensitivity);
    }

    // 编辑态相机 = 显式输入（视口用 ImGui 驱动）+ 自由飞行（UE 视口手感）。
    // 相机状态是从 JSON 读回来的，所以**每次场景被重建都要再设一遍**：
    // 新建 / 打开（都走 onSceneReplaced）以及 undo / redo（整场景快照恢复）
    // 之后。漏掉的话表现为"视口导航突然变回老式轨道相机"。
    void applyEditorCameraMode();

    // ---------------------------------------------------------- 选择
    ecs::Entity selection() const { return m_selection; }
    void select(ecs::Entity e);
    void clearSelection();
    bool hasSelection() const;

    // 帧校验：句柄失效时按名字找回，找不回就清空。
    // （撤销删除、Play/Stop 之后都会依赖它把选择"接上"）
    void validateSelection();

    const std::string& selectionName() const { return m_selectionName; }

    // ---------------------------------------------------------- 撤销栈
    CommandStack& commands() { return m_commands; }
    void undo();
    void redo();

    // 抓一份当前编辑态场景的内存快照（JSON 文本）
    std::string snapshot() const;

    // 结构性编辑：抓 before → 执行 fn → 抓 after → 入栈（不重复执行）。
    // fn 里可以随便增删实体、改父子关系，回滚由整场景快照负责。
    //
    // 时序要点：fn() 当场就把改动生效了（不重建世界，句柄此时仍然有效），
    // 真正的"整场景重建"只发生在之后 undo/redo 触发快照恢复时 —— 那时
    // 旧句柄才会失效，由 undo()/redo() 里的 validateSelection 按名字接回。
    template <typename Fn>
    void structuralEdit(const std::string& name, Fn&& fn) {
        const std::string before = snapshot();
        fn();
        const std::string after = snapshot();
        if (before == after) return;  // 什么都没变，不进历史
        m_commands.pushAlreadyApplied(std::make_unique<SceneSnapshotCommand>(
            name, &m_editorScene, &m_assets, before, after));
        setStatus(name);
        m_dirty = true;
    }

    // ---------------------------------------------------------- Gizmo
    GizmoMode gizmoMode() const { return m_gizmoMode; }
    void setGizmoMode(GizmoMode m) { m_gizmoMode = m; }
    GizmoSpace gizmoSpace() const { return m_gizmoSpace; }
    void setGizmoSpace(GizmoSpace s) { m_gizmoSpace = s; }

    // ---------------------------------------------------------- 吸附
    // 平移 / 旋转 / 缩放**各有一套独立的开关 + 步长** —— 这就是 UE5
    // 视口工具条上的模型（勾选框 + 数值 + 下拉预设，三种变换互不影响）。
    // 默认值也照 UE5 取，只是把单位换算到了本引擎（1 单位 = 1 米）：
    //     平移 10 uu = 10 cm → **0.1 m**
    //     旋转 → **10 度**
    //     缩放 → **0.1**（倍率）
    // 三个开关默认都是**关**：吸附是"想对齐时按一下"的手段，默认开着会让
    // 自由摆放变得一格一格的，反而别扭。
    bool snapMove() const { return m_snapMove; }
    bool& snapMoveRef() { return m_snapMove; }
    float snapMoveStep() const { return m_snapMoveStep; }
    float& snapMoveStepRef() { return m_snapMoveStep; }

    bool snapRotate() const { return m_snapRotate; }
    bool& snapRotateRef() { return m_snapRotate; }
    float snapRotateStep() const { return m_snapRotateStep; }
    float& snapRotateStepRef() { return m_snapRotateStep; }

    bool snapScale() const { return m_snapScale; }
    bool& snapScaleRef() { return m_snapScale; }
    float snapScaleStep() const { return m_snapScaleStep; }
    float& snapScaleStepRef() { return m_snapScaleStep; }

    // 当前手柄模式对应的那一套（UI 与 gizmo 都只关心"当前模式"）
    bool snapForCurrentMode() const;
    float snapStepForCurrentMode() const;
    void toggleSnapForCurrentMode();

    // ---------------------------------------------------------- 地平面栅格
    // 视口里画在 y = 0 平面上的参考栅格（最小格 1 m、主格 10 m）。
    // 默认**开**；视口右上角浮层里的勾选框可以关掉（View 菜单里也有）。
    // 只在**编辑态**渲染 —— Play 期间视口里是游戏画面，参考网格不该混进去。
    // 想彻底禁掉（比如做像素回归对比）可以设 MYVK_GRID=0 再启动。
    bool showGrid() const { return m_showGrid; }
    bool& showGridRef() { return m_showGrid; }

    // ---------------------------------------------------------- 状态 / 提示
    void setStatus(std::string s) { m_status = std::move(s); }
    const std::string& status() const { return m_status; }

    void notify(std::string msg);          // 显示 3 秒的瞬时提示
    const std::string& notification() const { return m_notification; }
    float notificationTimer() const { return m_notificationTimer; }
    void tickNotification(float dt);

    // ---------------------------------------------------------- 面板可见性
    // Stats / Scripts 是**浮动**调试面板（不走布局矩形），默认收起：
    // 打开时它们浮在固定布局之上，若默认可见会在首次启动就盖住
    // Hierarchy / Content，看起来像坏了。需要时从 View 菜单打开。
    // 视口左上角的 HUD 叠加层已经提供了 FPS / 实体数 / 灯光数，
    // 所以默认收起 Stats 并不损失常用信息。
    struct PanelVisibility {
        bool hierarchy = true;
        bool inspector = true;
        bool content = true;
        bool viewport = true;
        bool scripts = false;  // 浮动，View 菜单打开
        bool stats = false;    // 浮动，View 菜单打开
        bool engine = false;   // 引擎/光照调参面板（默认收起）
    };
    PanelVisibility& panels() { return m_panels; }

    // ---------------------------------------------------------- 布局
    // 由 EditorApp 每帧填充；面板用 FirstUseEver 落位（用户拖动后不再覆盖）
    LayoutRects& layout() { return m_layout; }
    const LayoutRects& layout() const { return m_layout; }
    // 菜单里的"Reset Layout"：置位后各面板本帧用 Always，随后清除
    bool& relayout() { return m_relayout; }

    // ---------------------------------------------------------- 场景文件
    // 当前场景文件路径（供标题栏 / 保存使用）
    std::string& scenePath() { return m_scenePath; }
    bool& dirty() { return m_dirty; }

    // 菜单里的 "Quit"：置位后由 EditorApp 在下一帧关闭窗口
    bool& quitRequested() { return m_quit; }

    // View 菜单里的 "Toggle Fullscreen (F11)"：置位后由 EditorApp 处理。
    // 这里只放一个请求标志 —— 本类不认识 core::Window，真正的窗口切换
    // 属于 EditorApp 的职责（和 quitRequested / relayout 一个套路）。
    bool& windowFullscreenRequest() { return m_windowFsRequest; }

    // ---------------------------------------------------------- 界面设置
    // 字体缩放（全局，1.0 = 默认字号）。放在 EditorContext 里是为了让
    // Settings 弹窗、工具栏高度、分栏布局共用同一份状态。
    float& fontScale() { return m_fontScale; }
    float fontScale() const { return m_fontScale; }

    // 工具栏（播放控制条）高度（像素）。分栏布局与工具栏绘制都读它。
    float& toolbarHeight() { return m_toolbarH; }
    float toolbarHeight() const { return m_toolbarH; }

    // Settings 弹窗是否打开
    bool& showSettings() { return m_showSettings; }

private:
    assets::AssetManager& m_assets;

    scene::Scene m_editorScene;
    scene::Scene m_runtimeScene;
    PlayState m_play = PlayState::Stopped;
    float m_playTime = 0.0f;

    bool m_gameFullscreen = false;
    float m_navSensitivity = 1.0f;

    ecs::Entity m_selection{};
    std::string m_selectionName;

    CommandStack m_commands;

    GizmoMode m_gizmoMode = GizmoMode::Translate;
    GizmoSpace m_gizmoSpace = GizmoSpace::World;

    // 吸附：三套独立（默认值见上面 snapMove 一节的说明，对齐 UE5）
    bool m_snapMove = false;
    float m_snapMoveStep = 0.1f;    // 米
    bool m_snapRotate = false;
    float m_snapRotateStep = 10.0f;  // 度
    bool m_snapScale = false;
    float m_snapScaleStep = 0.1f;    // 倍率

    // 地平面栅格（编辑态视口）
    bool m_showGrid = true;

    std::string m_status = "Ready";
    std::string m_notification;
    float m_notificationTimer = 0.0f;

    PanelVisibility m_panels;
    LayoutRects m_layout;
    bool m_relayout = false;

    // 当前场景文件路径（供标题栏 / 保存使用）。
    // 启动是空场景（不加载任何样例），所以这只是"Ctrl+S 会写到哪儿"。
    std::string m_scenePath = "assets/scenes/scene.json";
    bool m_dirty = false;
    bool m_quit = false;
    bool m_windowFsRequest = false;

    // 界面设置（默认值：字体 1.0、工具栏高 48 —— 比旧的 40 更宽松）
    float m_fontScale = 1.0f;
    float m_toolbarH = 48.0f;
    bool m_showSettings = false;
};

} // namespace editor
