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
#include "PanelRegistry.h"  // 面板显隐的权威存储 + panelOpenRef
#include "Workspace.h"      // WorkspaceManager / ViewportLayout

#include "core/InputMap.h"  // G06：键位绑定表（按值持有，见 inputMap()）
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
//
// 2026-10-17 晚：新增 Select（Q）。用户要求视口工具条按 UE5 的样子给
// "选择 / 平移 / 旋转 / 缩放"四个工具，而"选择"不是一个手柄 —— 它
// 恰好是**没有手柄**的状态：鼠标左键只做点选，选中物体上不出现三轴。
// 把它做成枚举值而不是另一个 bool，是因为工具条是"四选一"的单选组：
// 两个平行状态（"哪个手柄" + "要不要手柄"）会立刻出现"选择模式下手柄
// 还亮着"这种自相矛盾的组合。
//
// ⚠ 所有 `switch (gizmoMode())` 都必须显式写上 Select —— 原来的
//   `default:` 分支是当 Scale 用的，放任 Select 落进去会让"选择模式"
//   画出一把缩放手柄（比没有更糟）。
enum class GizmoMode : int {
    Translate = 0,
    Rotate = 1,
    Scale = 2,
    Select = 3,
};
// 手柄坐标系
enum class GizmoSpace : int { World = 0, Local = 1 };
// 播放状态机
enum class PlayState : int { Stopped = 0, Playing = 1, Paused = 2 };

// 动画控制（编辑态脚本）的独立状态机。与 Run（PlayState）**互斥使用**：
//   Run = 把编辑场景复制成运行态副本跑游戏逻辑（EditorContext::play）；
//   Play/Pause/Stop = 直接在**编辑场景自身**上跑/冻结脚本（当动画用），
//   Stop 用 JSON 快照把场景恢复成按下动画 Play 时的样子。
// 两套状态机的按钮分开放：Run 在播放条绿色按钮，动画三键在它旁边。
enum class AnimState : int { Stopped = 0, Playing = 1, Paused = 2 };

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
    // transport = 视口正下方那一行播放条（Run / Play / Pause / Stop），
    // 高度从 viewport 里扣出来 —— 视口的 3D 画面在这行上方结束。
    //
    // ⚠ 2026-10-17 起 transport **不再绘制**（播放条的 UI 被移除，动画相关
    //   入口暂时只留快捷键 / 菜单）。字段保留，值恒为 0×0：EditorLayoutIO
    //   与几个回归脚本还在读它，删字段会让它们编译 / 解析失败。
    //
    // bottom = 状态栏正上方那条通栏（Content / 输出日志 / CMD 三个带图标的
    // 按钮 + 资源条目统计）。高度恒为 kBottomBarH（见 EditorApp）。
    Rect menu, toolbar, hierarchy, content, inspector, viewport, status,
         transport, bottom;
};

// 物理世界的**运行态只读快照**（R7-1）。
//
// 为什么不让 Inspector 直接摸 physics::PhysicsWorld：
//   · 那个对象归 EditorApp 所有，生命周期与 Play 严格绑定（Play 建 / Stop 丢）；
//   · 面板只需要"看一眼现在跑成什么样"，不需要改它 —— 拿一个指针反而
//     会诱导面板去写运行态（那会破坏"Play 期间编辑态零改动"的前提）。
// 所以 EditorApp 每帧把统计数灌进来，面板只读。world 为 false 时面板知道
// "物理没在跑"，可以显示原因而不是一片 0。
struct PhysicsRuntimeInfo {
    bool active = false;          // 物理世界当前存在（Play 中且总开关开着）
    int bodies = 0;
    int dynamic = 0;
    int sleeping = 0;
    int manifolds = 0;
    int contactPoints = 0;
    int broadphasePairs = 0;
    int joints = 0;
    float maxPenetration = 0.0f;
    float maxSpeed = 0.0f;
    unsigned long long steps = 0;
    float simTime = 0.0f;
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

    // ---- R7-1 物理：运行态只读快照（见 PhysicsRuntimeInfo 的说明）----
    const PhysicsRuntimeInfo& physicsRuntime() const { return m_physicsRuntime; }
    void setPhysicsRuntime(const PhysicsRuntimeInfo& info) {
        m_physicsRuntime = info;
    }

    // ---- 动画（编辑态脚本，见 AnimState 的说明）----
    AnimState animState() const { return m_anim; }
    // 动画是否正在走（只有 Playing；Paused 不走）
    bool animPlaying() const { return m_anim == AnimState::Playing; }
    // 动画是否被"占用"（Playing 或 Paused —— Stop 按钮/互斥判断用）
    bool animActive() const { return m_anim != AnimState::Stopped; }
    void animPlay();    // Stopped → Playing（先拍快照）；Paused → Playing
    void animPause();   // Playing → Paused
    void animStop();    // 任意 → Stopped（用快照恢复编辑场景）
    float animTime() const { return m_animTime; }
    void advanceAnimTime(float dt) { m_animTime += dt; }
    // 时间轴拖拽（scrub）：直接改写时钟读数。脚本读的就是这个值，
    // 所以往回拖 = 让下一帧的脚本看到更小的 time。
    void setAnimTime(float t) { m_animTime = t; }

    // 整个场景被替换（新建 / 打开文件）后调用：清选择、清历史
    void onSceneReplaced();

    // 运行态已经跑了多久（脚本的 time，也用于 HUD 显示）
    float playTime() const { return m_playTime; }
    void advancePlayTime(float dt) { m_playTime += dt; }
    void setPlayTime(float t) { m_playTime = t; }

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

    // "基于距离的相机速度"（UE 的 Camera Speed scaled by distance）。
    // 打开后 WASD 的飞行速度随"相机到注视点的距离"线性放大 —— 远景下
    // 拖同样的鼠标不该只挪一厘米。
    //
    // **默认关**：打开它会改变同样一段按键走多远，这是行为变化，必须由
    // 用户显式选。关着时算子恒为 1，飞行速度与改动前逐位一致。
    bool navSpeedScalesWithDistance() const { return m_navSpeedByDistance; }
    bool& navSpeedScaleRef() { return m_navSpeedByDistance; }
    // 当前应乘的距离系数（1 = 不缩放）。夹在 [0.25, 8]，否则贴脸时飞不动、
    // 远景时一按就冲出场景。
    float navDistanceSpeedFactor(float distance) const {
        if (!m_navSpeedByDistance) return 1.0f;
        return glm::clamp(distance / 10.0f, 0.25f, 8.0f);
    }

    // 编辑态相机 = 显式输入（视口用 ImGui 驱动）+ 自由飞行（UE 视口手感）。
    // 相机状态是从 JSON 读回来的，所以**每次场景被重建都要再设一遍**：
    // 新建 / 打开（都走 onSceneReplaced）以及 undo / redo（整场景快照恢复）
    // 之后。漏掉的话表现为"视口导航突然变回老式轨道相机"。
    void applyEditorCameraMode();

    // ---------------------------------------------------------- 选择
    ecs::Entity selection() const { return m_selection; }
    void select(ecs::Entity e);
    // "在 3D 视口里用左键点中的选中" —— 与 select() 的唯一区别是记下了出处。
    // 少数操作只认这种选中（目前是数字键 0 的"落地"，见
    // ViewportPanel::dropSelectionToGround），因为在层级树 / 内容浏览器里
    // 选中的东西并不一定在视口里看得见，"落地"对它没有意义。
    void selectFromViewport(ecs::Entity e);
    void clearSelection();
    bool hasSelection() const;
    bool selectionFromViewport() const { return m_selectionFromViewport; }

    // ---------------------------------------------------------- 碰撞体编辑
    // 当前 W/E/R 手柄作用在**哪儿**：
    //   false → 实体的 TransformComponent（常规）
    //   true  → 选中实体身上的 CollisionComponent（点视口里的碰撞框、
    //           或勾 Inspector 里的 "Edit collider" 都会切过来）
    //
    // 为什么放在这里而不是 CollisionComponent 上：它是**编辑器状态**。
    // 存进组件就会被 Play 的整场景复制带进运行态、还会写进场景文件 ——
    // 那两条路都不该关心"编辑器当前在编辑谁"。
    //
    // 选择一变就复位（见 select() / clearSelection()），语义是"这个开关
    // 永远只对当前选中项有效"。
    bool colliderEdit() const { return m_colliderEdit; }
    void setColliderEdit(bool on) { m_colliderEdit = on; }

    // ---- 视口矩形（ViewportPanel 每帧写）----
    //
    // 运行态 Player 的鼠标视角就用"鼠标是否落在视口矩形内"门控。
    //
    // ⚠ 为什么不用 ImGui 的 hover（IsItemHovered / IsWindowHovered）：
    //   本编辑器开了 ImGuiConfigFlags_NavEnableKeyboard。用户一旦用过
    //   键盘，ImGui 会进入 nav 高亮状态，而 ItemHoverable() 里有这么一句
    //       if (g.NavHighlightItemUnderNav && ...) return false;
    //   —— 于是 IsItemHovered() 会**一直**返回 false，直到鼠标重新动过
    //   （NavHighlightItemUnderNav 只在 io.MouseDelta != 0 时才清除）。
    //   结果就是：在运行态按过 Space / WASD 之后，鼠标视角直接失效。
    //   游戏输入不该被 UI 的 nav 状态左右，所以这里改成自己判矩形。
    void setViewportRect(const glm::vec2& pos, const glm::vec2& size) {
        m_viewportPos = pos;
        m_viewportSize = size;
        m_viewportValid = true;
    }
    bool mouseInViewport(const glm::vec2& mouse) const {
        return m_viewportValid && mouse.x >= m_viewportPos.x &&
               mouse.y >= m_viewportPos.y &&
               mouse.x < m_viewportPos.x + m_viewportSize.x &&
               mouse.y < m_viewportPos.y + m_viewportSize.y;
    }

    // 帧校验：句柄失效时按名字找回，找不回就清空。
    // （撤销删除、Play/Stop 之后都会依赖它把选择"接上"）
    void validateSelection();

    const std::string& selectionName() const { return m_selectionName; }

    // ---------------------------------------------------------- 键位绑定（G06）
    // 编辑器**按值持有**一份 `core::InputMap`。
    //
    // 为什么不是"引擎全局单例"：绑定表是**这份工程的数据**（存成
    // `assets/input_map.json`，Game.exe 读同一份）。放进 EditorContext 让
    // 面板只依赖一个入口，也让"改 → 撤销 → 再改"有地方挂。
    //
    // ⚠ 编辑器**从不 poll() 它**。编辑器的键鼠归 core::Input 与各面板自己
    //   （视口导航、快捷键）；这里的实例只是"可编辑的表"，一旦让它开始
    //   采集，两套输入会在同一个窗口上互相抢。
    core::InputMap& inputMap() { return m_inputMap; }
    const core::InputMap& inputMap() const { return m_inputMap; }
    // 改过但还没落盘（面板顶部的 * 号）
    bool inputMapDirty() const { return m_inputMapDirty; }
    void setInputMapDirty(bool v) { m_inputMapDirty = v; }

    // 启动时读一次。**读不到 = 保持出厂绑定**（文件不存在是正常状态，
    // 第一次启动/从没改过键位就是这样），不是错误。
    void loadInputMap();
    bool saveInputMap();   // 落盘 + 清 dirty；失败只打 WARN

    // 改动的**唯一**入口：应用 + 入撤销栈（整表 JSON 文本的前后快照）。
    // 面板不许直接调 inputMap().setActionBinding(...) —— 那样改了撤不回。
    //
    // ⚠ 为什么整表快照而不是"精确改一条"：绑定表只有几十条、几百字节，
    //   操作频率是"人按一下键"。精确回滚要处理"这条是新加的/被替换的/
    //   多来源里哪一条"，而快照一行搞定且绝不出错 —— 与 SceneSnapshotCommand
    //   同一个取舍理由。
    bool rebindAction(const std::string& context, const std::string& action,
                      const core::InputBinding& binding);
    bool resetActionBinding(const std::string& context,
                            const std::string& action);
    bool clearActionBinding(const std::string& context,
                            const std::string& action);
    bool resetAllBindings();

    // ---- 键位捕获态（面板写、全局快捷键读）----
    // 捕获期间 `handleShortcuts` 必须整体闭嘴：玩家想把 Jump 绑到 Space，
    // 而 Space 是"运行/暂停" —— 不收手的话点一下 Rebind 就把场景跑起来了。
    bool inputCaptureActive() const { return !m_captureAction.empty(); }
    void beginInputCapture(std::string context, std::string action);
    void endInputCapture();
    const std::string& captureContext() const { return m_captureContext; }
    const std::string& captureAction() const { return m_captureAction; }

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
    // 三个开关**默认全开**（2026-10-17 按用户要求改；UE5 原本是默认关）。
    // 步长仍是 UE5 那套：0.1 m / 10 度 / 0.1 倍 —— 所以默认状态下拖动
    // 手柄会稳定地落在 10 cm / 10 度 / 10% 的整数倍上。
    // 临时关掉：按 G（只影响当前手柄模式），或 Preferences ▸ Snapping。
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

    // ---------------------------------------------------------- 视口浮层（E14）
    // 视口里两块浮层，**各自独立显隐**（右键视口空白处的 "Viewport
    // Display" 菜单切换），状态经 Settings 落盘（config/editor_prefs.json）。
    //
    // 这样分的理由：两块信息的使用频率完全不同 —— 统计 HUD 只在调性能时看、
    // 导航球有个性偏好。绑成一个 "HUD" 总开关时，用户被迫"要么全看要么全不看"。
    //
    // ⚠ **左上角的"视图模式条"已删（2026-10-17 用户要求）**：Lit/Unl/Nrm/...
    //   八个按钮和右上角 Lighting 下拉里的视图模式是同一组东西，摆两遍纯属
    //   重复。视图模式现在只有两个入口：Lighting 下拉 + Shift+1..8。
    struct ViewportOverlay {
        // 右上：统计 HUD。**默认关** —— 它每帧都在变（FPS / 帧时间），
        // 是视口里最"吵"的一块；要调性能时从右键 Viewport Display 打开即可，
        // 打开后还能用标题行折叠成一颗小胶囊（见 ViewportPanel::m_statsCollapsed）。
        bool stats = false;
        bool navGizmo = true;   // 右下：导航 gizmo
    };
    // ⚠ **左下角的相机浮层已经删掉了**（原来叫 ov.camera）。
    //   原因：它和 Inspector → World → Camera 是**同一个相机对象的两个编辑
    //   入口**（FOV / 距离 / 朝向两边都能改），既重复又容易让人以为"两处
    //   数值不一致"。现在相机只有一个面板：Inspector 的 World 页。
    //   浮层里那些**只在那里才有**的功能（9 个相机书签、导航灵敏度、网格
    //   开关）已经搬进 Inspector，键鼠快捷键（Ctrl+Shift+N / Alt+Shift+N）
    //   完全没动。
    // 统计 HUD 的**逐项**开关。默认全开；关掉自己不看的那几行就能让
    // 一块 6 行的浮层缩成 2 行，盯画面时干扰小很多。
    struct StatsHudItems {
        bool fps = true;
        bool frameMs = true;
        bool drawCalls = true;
        bool triangles = true;
        bool vram = true;
        bool resolution = true;
        bool viewMode = true;
        bool entities = true;
        bool selection = true;
        // 手柄状态行（"EDIT gizmo: move / world"）与拖拽实时读数。
        // 它原来画在左上角的 drawOverlay 里，E14 左上角让给了视图模式条，
        // 于是并进统计 HUD —— 拖拽时"沿哪个轴走了多少"是最需要盯的一行。
        bool gizmo = true;
    };
    // 相机书签：Ctrl+Shift+数字 存当前视角，Alt+Shift+数字 跳回去。
    // （数字键本身被 Blender 那套视图方向键与 Alt 面板显隐占满了，见
    //   EditorApp::handleShortcuts 的说明，所以这里只能挂到 Shift 层。）
    struct CameraBookmark {
        bool set = false;
        glm::vec3 pos{0.0f};
        float yaw = 0.0f;
        float pitch = 0.0f;
    };
    static constexpr int kBookmarkCount = 9;

    ViewportOverlay& vpOverlayRef() { return m_vpOverlay; }
    const ViewportOverlay& vpOverlay() const { return m_vpOverlay; }
    StatsHudItems& statsHudRef() { return m_statsHud; }
    const StatsHudItems& statsHud() const { return m_statsHud; }
    CameraBookmark* bookmarksRef() { return m_bookmarks; }
    const CameraBookmark* bookmarks() const { return m_bookmarks; }

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
    //
    // ⚠ E04 起这里**不再是存储**。权威存储是 PanelRegistry 里那份按表下标
    //   索引的持久 bool 数组（加一个面板只加一行表项）。本结构体只是一层
    //   **命名引用视图** —— 几个引用绑到那份数组上，于是所有既有调用点
    //   （`panels().viewport = true`、`&p.hierarchy`、`if (panels.stats)`）
    //   一行都不用改。默认值（defaultOpen）也搬到了表里。
    //
    //   为什么不做成"每帧从表拷一份 bool 回来"：`MenuItem(..., &p.stats)`
    //   是**直接写指针**的，拷贝出去的副本改了不会回写。引用才是对的。
    struct PanelVisibility {
        bool& hierarchy;
        bool& inspector;
        bool& content;
        bool& viewport;
        bool& stats;
        bool& scripts;
        bool& engine;
        bool& console;

        PanelVisibility()
            : hierarchy(panelOpenRef("hierarchy")),
              inspector(panelOpenRef("inspector")),
              content(panelOpenRef("content")),
              viewport(panelOpenRef("viewport")),
              stats(panelOpenRef("stats")),
              scripts(panelOpenRef("scripts")),
              engine(panelOpenRef("engine")),
              console(panelOpenRef("console")) {}
    };
    // 引用成员 → 本对象不可赋值，但可以整体拷贝构造（调用点只读它）
    PanelVisibility& panels() { return m_panels; }

    // ---------------------------------------------------------- 工作区（E05）
    // "当前是哪一套工作区"。真正的施加在 EditorApp::applyWorkspace ——
    // 它才拿得到 SplitLayout / Renderer。
    WorkspaceManager& workspace() { return m_workspace; }
    const WorkspaceManager& workspace() const { return m_workspace; }

    // 当前布局是否偏离了该工作区的出厂预设（标题栏 / 菜单上的 * 号）。
    // 由 EditorApp 逐帧比对后写进来 —— 比对要同时看 SplitLayout 与
    // Renderer，那两个本类都拿不到，所以只能"外面算、这里存"。
    bool workspaceModified() const { return m_wsModified; }
    void setWorkspaceModified(bool v) { m_wsModified = v; }

    // ---------------------------------------------------------- 多视口（E07）
    // 视口布局 + 哪个子视口是"活动的"（唯一接受键鼠输入的那个）。
    ViewportLayout viewportLayout() const { return m_viewportLayout; }
    void setViewportLayout(ViewportLayout l) { m_viewportLayout = l; }
    int activeSubview() const { return m_activeSubview; }
    void setActiveSubview(int i) {
        if (i >= 0 && i < 4) m_activeSubview = i;
    }

    // ---------------------------------------------------------- 最大化（E06）
    // Shift+Space 让鼠标所在的分栏面板临时占满工作区。
    // ⚠ 权威状态**只有一份**，在 SplitLayout（m_maximized）—— EditorContext
    //   不存第二份，因为两份状态必然会漂。Shift+Space 直接在
    //   EditorApp::handleShortcuts 里落到 SplitLayout 上（那里拿得到鼠标
    //   位置和各个面板矩形）。落盘时再由 EditorApp 把槽位翻译成面板 id。

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

    // ---------------------------------------------------------- 最近打开（E08）
    // File ▸ Open Recent。**最新在前**，去重，最多 kMaxRecent 条。
    // 落盘进 config/editor_layout.json（见 EditorLayoutIO）——
    // 不然每次重启都清空，这个菜单就永远只有一行 "no recent scenes"。
    static constexpr size_t kMaxRecent = 10;
    const std::vector<std::string>& recentScenes() const { return m_recent; }
    std::vector<std::string>& recentScenesRef() { return m_recent; }
    void pushRecentScene(const std::string& relPath);

    // 菜单里的 "Quit"：置位后由 EditorApp 在下一帧关闭窗口
    bool& quitRequested() { return m_quit; }

    // View 菜单里的 "Toggle Fullscreen (F11)"：置位后由 EditorApp 处理。
    // 这里只放一个请求标志 —— 本类不认识 core::Window，真正的窗口切换
    // 属于 EditorApp 的职责（和 quitRequested / relayout 一个套路）。
    bool& windowFullscreenRequest() { return m_windowFsRequest; }

    // Content 工具栏里的 "Save All"：置位后由 EditorApp 处理。
    // 为什么不让 ContentBrowser 自己存：场景路径、InputMap、以及"存完要清
    // dirty"这三件事都在 EditorApp 手里；面板只认识文件名，复制一份保存
    // 逻辑过去必然两边漂移。
    bool& saveAllRequest() { return m_saveAllRequest; }

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

    // ---------------------------------------------------------- 角色详情窗口
    // 创建第一/第三人称角色后，请求打开角色详情窗口（Persona 风格）。
    // 由 ViewportPanel 在创建玩家后置位，EditorApp 每帧检查并打开窗口，
    // 打开后即清空请求。实体句柄可能因撤销/重建失效，窗口打开时校验。
    void requestCharacterWindow(ecs::Entity player) {
        m_pendingCharacter = player;
    }
    ecs::Entity takeCharacterWindowRequest() {
        ecs::Entity e = m_pendingCharacter;
        m_pendingCharacter = ecs::Entity{};
        return e;
    }

    // ---------------------------------------------------------- 反射探针（R5-1）
    // Inspector 的 "Capture" 按钮请求**立刻重捕一次**全部探针。
    // 与角色窗口同一个套路：本类不认识 Renderer（那要拖进整条渲染头
    // 依赖），所以只放一个请求标志，EditorApp 每帧取走并转给渲染器。
    void requestProbeCapture() { m_probeCaptureRequest = true; }
    bool takeProbeCaptureRequest() {
        const bool v = m_probeCaptureRequest;
        m_probeCaptureRequest = false;
        return v;
    }

private:
    assets::AssetManager& m_assets;

    scene::Scene m_editorScene;
    scene::Scene m_runtimeScene;
    PlayState m_play = PlayState::Stopped;
    float m_playTime = 0.0f;

    // R7-1：物理世界的运行态统计（由 EditorApp 每帧灌入，面板只读）
    PhysicsRuntimeInfo m_physicsRuntime;

    // ---- 动画状态 ----
    AnimState m_anim = AnimState::Stopped;
    float m_animTime = 0.0f;
    std::string m_animSnapshot;  // 按下动画 Play 时的场景 JSON（Stop 恢复用）

    bool m_gameFullscreen = false;
    float m_navSensitivity = 1.0f;
    // 见 navSpeedScalesWithDistance()。默认关 = 行为与改动前完全一致。
    bool m_navSpeedByDistance = false;

    // 选中项的两个参数版本（select / selectFromViewport 都转发到它）——
    // 只有它能改 m_selectionFromViewport，所以放在私有区。
    void select(ecs::Entity e, bool fromViewport);

    ecs::Entity m_selection{};
    std::string m_selectionName;
    // 当前选中项是不是"在 3D 视口里用左键点中的"（见 selectionFromViewport）
    bool m_selectionFromViewport = false;
    // W/E/R 手柄作用在碰撞体上？（见 colliderEdit()）
    bool m_colliderEdit = false;

    // 视口矩形（ImGui 屏幕坐标；Player 运行态鼠标门控，见 setViewportRect）
    glm::vec2 m_viewportPos{0.0f};
    glm::vec2 m_viewportSize{0.0f};
    bool m_viewportValid = false;

    CommandStack m_commands;

    // ---- 键位绑定（G06，见上方的说明）----
    core::InputMap m_inputMap;
    bool m_inputMapDirty = false;
    std::string m_captureContext;
    std::string m_captureAction;

    // "改一处绑定"的公共外壳：抓 before → 执行 → 抓 after → 入栈。
    // 返回 false = 操作本身失败，或结果与改之前逐字节相同（绑了同一个键）。
    template <typename Fn>
    bool inputMapEdit(const std::string& name, Fn&& fn) {
        const std::string before = m_inputMap.toText();
        if (!fn()) return false;
        const std::string after = m_inputMap.toText();
        if (before == after) return false;
        // ⚠ 捕获 this 而不是 &m_inputMap：撤销之后**也要**把 dirty 立起来
        //   （文件此刻与内存不一致了）。只捕获指针就只能改数据、改不了脏标记。
        m_commands.pushAlreadyApplied(std::make_unique<LambdaCommand>(
            name,
            [this, before]() {
                m_inputMap.loadFromText(before);
                m_inputMapDirty = true;
            },
            [this, after]() {
                m_inputMap.loadFromText(after);
                m_inputMapDirty = true;
            }));
        m_inputMapDirty = true;
        setStatus(name);
        return true;
    }

    GizmoMode m_gizmoMode = GizmoMode::Translate;
    GizmoSpace m_gizmoSpace = GizmoSpace::World;

    // 吸附：三套独立（平移 / 旋转 / 缩放）。
    //
    // ⚠ 2026-10-17 按用户要求改成**默认全开**（原来是跟 UE5 一样默认关）。
    //   步长仍是 UE5 那套：0.1 m / 10 度 / 0.1 倍。想临时关掉按 G，
    //   或者去 Preferences ▸ Snapping 取消勾选。
    //   ⚠ 若要再改这里，**必须同步** prefsTable() 里 snap.* 的 `def`——
    //     那个 def 是"Reset All / 单项 R"的落点，两边不一致会让重置把
    //     默认值改回去（已踩过一次）。
    bool m_snapMove = true;
    float m_snapMoveStep = 0.1f;    // 米
    bool m_snapRotate = true;
    float m_snapRotateStep = 10.0f;  // 度
    bool m_snapScale = true;
    float m_snapScaleStep = 0.1f;    // 倍率

    // 地平面栅格（编辑态视口）
    bool m_showGrid = true;

    // 视口浮层 / 统计 HUD 逐项 / 相机书签（E14）
    ViewportOverlay m_vpOverlay;
    StatsHudItems m_statsHud;
    CameraBookmark m_bookmarks[kBookmarkCount];

    std::string m_status = "Ready";
    std::string m_notification;
    float m_notificationTimer = 0.0f;

    PanelVisibility m_panels;   // 命名引用视图（不持有存储）
    LayoutRects m_layout;
    bool m_relayout = false;

    // ---- 工作区 / 多视口 / 最大化（E04~E07）----
    WorkspaceManager m_workspace;
    ViewportLayout m_viewportLayout = ViewportLayout::Single;
    int m_activeSubview = 0;
    bool m_wsModified = false;

    // 当前场景文件路径（供标题栏 / 保存使用）。
    // 启动是空场景（不加载任何样例），所以这只是"Ctrl+S 会写到哪儿"。
    std::string m_scenePath = "assets/scenes/scene.json";
    bool m_dirty = false;
    bool m_quit = false;
    bool m_windowFsRequest = false;
    bool m_saveAllRequest = false;

    // 界面设置（默认值：字体 1.0、工具栏高 48 —— 比旧的 40 更宽松）
    float m_fontScale = 1.0f;
    float m_toolbarH = 48.0f;
    bool m_showSettings = false;

    // 待打开角色详情窗口的玩家实体（无效 = 无请求）
    ecs::Entity m_pendingCharacter{};

    // R5-1：待处理的"立刻重捕反射探针"请求（Inspector 的 Capture 按钮）
    bool m_probeCaptureRequest = false;

    // 最近打开的场景（最新在前，见 pushRecentScene）
    std::vector<std::string> m_recent;
};

} // namespace editor
