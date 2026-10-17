#include "EditorContext.h"

#include "InputMapIO.h"
#include "Localization.h"
#include "assets/AssetManager.h"
#include "core/Logger.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace editor {

EditorContext::EditorContext(assets::AssetManager& assets) : m_assets(assets) {
    // MYVK_GRID=0：连默认值一起关掉地平面栅格。
    // 用途是做像素级回归对比 —— 栅格是有抗锯齿的细线，任何一度视角变化
    // 都会牵动大片像素，把它排除掉才能让对比图有意义（和 MYVK_HIDE_UI
    // 是同一类开关）。
    if (const char* s = std::getenv("MYVK_GRID")) {
        if (s[0] == '0' || s[0] == 'f' || s[0] == 'F') m_showGrid = false;
    }
}

// ---------------------------------------------------------------- 最近打开

void EditorContext::pushRecentScene(const std::string& relPath) {
    if (relPath.empty()) return;
    // 去重（同一条挪到最前），再截到上限
    for (size_t i = 0; i < m_recent.size(); ++i) {
        if (m_recent[i] == relPath) {
            m_recent.erase(m_recent.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    m_recent.insert(m_recent.begin(), relPath);
    if (m_recent.size() > kMaxRecent) m_recent.resize(kMaxRecent);
}

// ---------------------------------------------------------------- 吸附

bool EditorContext::snapForCurrentMode() const {
    switch (m_gizmoMode) {
        case GizmoMode::Translate: return m_snapMove;
        case GizmoMode::Rotate: return m_snapRotate;
        case GizmoMode::Scale: return m_snapScale;
        // 选择模式没有变换，吸附自然谈不上。返回 false 而不是"随便挑一套"：
        // 工具条上那颗吸附指示器读的就是这里。
        case GizmoMode::Select: return false;
    }
    return false;
}

float EditorContext::snapStepForCurrentMode() const {
    switch (m_gizmoMode) {
        case GizmoMode::Translate: return m_snapMoveStep;
        case GizmoMode::Rotate: return m_snapRotateStep;
        case GizmoMode::Scale: return m_snapScaleStep;
        case GizmoMode::Select: return 0.0f;
    }
    return 0.0f;
}

// 只翻转"当前模式"那一套 —— 工具栏上的快捷开关按 X 用的就是它
// （UE5 里 X 是"切换当前工具的网格吸附"，行为一致）。
void EditorContext::toggleSnapForCurrentMode() {
    switch (m_gizmoMode) {
        case GizmoMode::Translate: m_snapMove = !m_snapMove; break;
        case GizmoMode::Rotate: m_snapRotate = !m_snapRotate; break;
        case GizmoMode::Scale: m_snapScale = !m_snapScale; break;
        // 选择模式下 X 什么也不做（没有可吸附的变换）
        case GizmoMode::Select: break;
    }
}

// ---------------------------------------------------------------- 场景分离

// activeScene() 是唯一的分发点：Play 期间返回运行态副本（于是视口里
// 直接就是游戏画面），其余时候返回编辑态。所有面板都问这里。
scene::Scene& EditorContext::activeScene() {
    return usingRuntime() ? m_runtimeScene : m_editorScene;
}

const scene::Scene& EditorContext::activeScene() const {
    return usingRuntime() ? m_runtimeScene : m_editorScene;
}

// 编辑态相机 = 显式输入 + 自由飞行。相机参数（target/yaw/pitch/distance）
// 来自 JSON，**飞行模式不在序列化范围内**，所以每次场景重建都要重设。
// setFlyMode(true) 内部把 position 固化成 m_flyPos 并同步 target ——
// 幂等且不跳变，多调几次没有副作用。
void EditorContext::applyEditorCameraMode() {
    scene::Camera& cam = m_editorScene.camera();
    cam.setUseGlobalInput(false);  // 输入由 ViewportPanel 显式喂（ImGui 捕获了鼠标）
    cam.setFlyMode(true);          // UE 视口手感：转自己 + WASD 飞行

    // ---- 远平面：编辑器视口用 5 km，而不是游戏的 200 m ----
    // 这是"飞到高处俯视地面时视野越来越窄"的根因：远平面 200 m 会把
    // 远侧的地面整片裁掉。相机在 150 m 高、俯角 60° 时，画面里最远的地面
    // 已经在 ~250 m 外 —— 于是可见地面被切成画面下方的一条窄带，升得越高
    // 带子越窄，到 300 m 时整屏空白。
    //
    // 近平面保持 0.1（贴地看细节要用），far/near = 5e4 对 24 位深度来说
    // 在 100 m 处仍有 ~6 mm 精度，够用；1 km 处 ~0.6 m，远小于一像素。
    cam.setClipPlanes(0.1f, 5000.0f);
}

void EditorContext::play() {
    if (m_play == PlayState::Playing) return;

    if (m_play == PlayState::Paused) {
        // 从暂停恢复：运行态的实体与状态原封不动，直接继续
        m_play = PlayState::Playing;
        setStatus(TR("Resumed"));
        return;
    }

    // ---- Stopped → Playing：把编辑态整份复制成运行态 ----
    std::string snap;
    const scene::SceneIoResult saved = scene::sceneToJson(m_editorScene, snap);
    if (!saved.ok) {
        VK_LOG_ERROR("Play failed: cannot snapshot scene: %s",
                     saved.error.c_str());
        setStatus(TR("Play failed: ") + saved.error);
        return;
    }

    const scene::SceneIoResult loaded =
        scene::sceneFromJson(m_runtimeScene, m_assets, snap);
    if (!loaded.ok) {
        VK_LOG_ERROR("Play failed: cannot restore snapshot: %s",
                     loaded.error.c_str());
        setStatus(TR("Play failed: ") + loaded.error);
        return;
    }

    // 相机状态随快照一起复制过来，所以按下 Play 的瞬间画面与编辑画面连续。
    // 切成自由飞行：游戏视角是第一人称式的（转自己），不是绕着目标点打转。
    //
    // 关键：**从这一刻起编辑器不再驱动这台相机**。视口在 Play 期间不处理
    // 任何键鼠导航（见 ViewportPanel::draw），相机怎么动完全由游戏逻辑决定。
    m_runtimeScene.camera().setUseGlobalInput(false);
    m_runtimeScene.camera().setFlyMode(true);

    m_playTime = 0.0f;
    m_play = PlayState::Playing;

    // 动画与 Run 互斥：进 Run 时丢掉动画快照（当前（可能已被动画改过）的
    // 编辑场景就是要被复制的那一份，无需恢复）
    m_anim = AnimState::Stopped;
    m_animTime = 0.0f;
    m_animSnapshot.clear();

    // 运行态的实体句柄与编辑态不同，选择先放下（名字留着，Stop 时接回来）
    m_selection = ecs::Entity{};
    m_colliderEdit = false;  // 运行态不该带着"在编辑碰撞体"的状态回来

    VK_LOG_INFO("Editor: PLAY in viewport (runtime scene = %zu entities, "
                "editor untouched)",
                m_runtimeScene.objectCount());
    setStatus(TR("Playing in viewport (F = fullscreen)"));
    notify(TR("Playing in viewport (F = fullscreen)"));
}

void EditorContext::pause() {
    if (m_play != PlayState::Playing) return;
    m_play = PlayState::Paused;
    setStatus(TR("Paused"));
}

void EditorContext::stop() {
    if (m_play == PlayState::Stopped) return;

    // 直接丢弃运行态，并退出全屏。编辑态从 Play 到现在一个字节都没动过 ——
    // "停止后回到播放前"是结构上保证的，不需要任何回滚逻辑。
    m_runtimeScene.clear();
    m_play = PlayState::Stopped;
    m_playTime = 0.0f;
    m_gameFullscreen = false;

    validateSelection();  // 把选择按名字接回编辑态
    VK_LOG_INFO("Editor: STOP (runtime discarded, editor scene restored)");
    setStatus(TR("Stopped"));
}

void EditorContext::togglePlayPause() {
    if (m_play == PlayState::Playing) {
        pause();
    } else {
        play();
    }
}

// ---------------------------------------------------------------- 动画控制
//
// 编辑态脚本当成动画用：Play 直接在编辑场景上跑脚本，Stop 用 JSON 快照
// 把场景恢复成按下 Play 时的样子。与 Run（运行态副本）互斥 —— 游戏跑着
// 的时候动画按钮是禁用的（播放条上做了 BeginDisabled，这里再挡一层）。

void EditorContext::animPlay() {
    if (!isEditing()) {
        setStatus(TR("Animation unavailable while Run is active"));
        return;
    }
    if (m_anim == AnimState::Playing) return;

    if (m_anim == AnimState::Paused) {
        m_anim = AnimState::Playing;
        setStatus(TR("Animation resumed"));
        return;
    }

    // Stopped → Playing：先拍快照（Stop 时恢复）
    const scene::SceneIoResult saved = scene::sceneToJson(m_editorScene, m_animSnapshot);
    if (!saved.ok) {
        VK_LOG_ERROR("Anim play failed: cannot snapshot scene: %s",
                     saved.error.c_str());
        setStatus(TR("Anim failed: ") + saved.error);
        return;
    }

    m_animTime = 0.0f;
    m_anim = AnimState::Playing;
    VK_LOG_INFO("Editor: ANIM play (editing the live scene; Stop restores "
                "the snapshot)");
    setStatus(TR("Animation playing"));
}

void EditorContext::animPause() {
    if (m_anim != AnimState::Playing) return;
    m_anim = AnimState::Paused;
    setStatus(TR("Animation paused"));
}

void EditorContext::animStop() {
    if (m_anim == AnimState::Stopped) return;

    m_anim = AnimState::Stopped;
    m_animTime = 0.0f;

    // 脚本可能改过任何东西（实体/组件/变换），用快照整体恢复编辑场景。
    if (!m_animSnapshot.empty()) {
        const scene::SceneIoResult loaded =
            scene::sceneFromJson(m_editorScene, m_assets, m_animSnapshot);
        if (!loaded.ok) {
            VK_LOG_ERROR("Anim stop: snapshot restore FAILED: %s",
                         loaded.error.c_str());
        }
        m_animSnapshot.clear();
    }

    // 快照里带着相机 → 恢复后重设回编辑态相机模式；undo 历史里的实体
    // 句柄已经不可信（场景被整体重建），一并清掉。
    applyEditorCameraMode();
    validateSelection();
    m_commands.clear();
    m_dirty = true;

    VK_LOG_INFO("Editor: ANIM stop (scene restored from snapshot)");
    setStatus(TR("Animation stopped"));
}

void EditorContext::onSceneReplaced() {
    // 换场景必然要先丢掉正在跑的运行态副本 —— 否则视口里还是**旧**场景的
    // 游戏画面，而编辑态已经是新的了，两边对不上。
    stop();
    m_playTime = 0.0f;
    // 动画快照属于旧场景，一并作废
    m_anim = AnimState::Stopped;
    m_animTime = 0.0f;
    m_animSnapshot.clear();
    clearSelection();
    m_commands.clear();
    m_dirty = false;
    // 新场景的相机是刚从一个 JSON 读出来的（轨道模式）—— 重新设回编辑态相机
    applyEditorCameraMode();
}

// ---------------------------------------------------------------- 选择

void EditorContext::select(ecs::Entity e) {
    select(e, /*fromViewport=*/false);
}

void EditorContext::selectFromViewport(ecs::Entity e) {
    select(e, /*fromViewport=*/true);
}

void EditorContext::select(ecs::Entity e, bool fromViewport) {
    if (e.valid() && !activeScene().world().alive(e)) e = ecs::Entity{};
    m_selection = e;
    m_selectionFromViewport = fromViewport && e.valid();
    // 换了选中项，"手柄是否作用在碰撞体上"必须复位 —— 否则新选中的物体
    // 会莫名其妙地拖到旧物体的碰撞体上（或者干脆拖不动）。
    m_colliderEdit = false;
    if (e.valid()) {
        if (const std::string* n = activeScene().world().name(e))
            m_selectionName = *n;
        else
            m_selectionName.clear();
    }
}

void EditorContext::clearSelection() {
    m_selection = ecs::Entity{};
    m_selectionName.clear();
    m_selectionFromViewport = false;
    m_colliderEdit = false;
}

bool EditorContext::hasSelection() const {
    return m_selection.valid() && activeScene().world().alive(m_selection);
}

void EditorContext::validateSelection() {
    scene::Scene& sc = activeScene();

    if (m_selection.valid() && sc.world().alive(m_selection)) {
        if (const std::string* n = sc.world().name(m_selection))
            m_selectionName = *n;
        return;
    }

    // 句柄失效 —— 撤销删除 / Play / Stop 之后都会走到这里。
    // 名字是跨"场景重建"唯一稳定的标识，按它找回来。
    m_selection = ecs::Entity{};
    // 新建出来的组件是全新的，手柄不能再指着一个"已经不存在的碰撞体"
    m_colliderEdit = false;
    if (!m_selectionName.empty()) {
        const ecs::Entity found = sc.find(m_selectionName);
        if (found.valid()) m_selection = found;
    }
}

// ---------------------------------------------------------------- 撤销

void EditorContext::undo() {
    if (!m_commands.undo()) {
        setStatus(TR("Nothing to undo"));
        return;
    }
    setStatus(std::string("Undo: ") + m_commands.redoName());
    validateSelection();
    // 整场景快照恢复会把相机一起重建（轨道模式）—— 设回编辑态相机。
    // 快照里的 target/distance 是当时的值，所以画面不会跳。
    applyEditorCameraMode();
}

void EditorContext::redo() {
    if (!m_commands.redo()) {
        setStatus(TR("Nothing to redo"));
        return;
    }
    setStatus(std::string("Redo: ") + m_commands.undoName());
    validateSelection();
    applyEditorCameraMode();
}

std::string EditorContext::snapshot() const {
    std::string out;
    // 结构编辑永远只发生在编辑态，快照也只抓编辑态
    scene::sceneToJson(m_editorScene, out);
    return out;
}

// ============================================================
// G06 · 键位绑定
// ============================================================

void EditorContext::loadInputMap() {
    // 出厂绑定是**唯一事实源**（core::InputMap::makeDefault）。文件只是
    // 覆盖 —— 读不到就保持出厂，这条路径不该有任何错误处理分支。
    m_inputMap = core::InputMap::makeDefault();
    const bool fromFile = loadEditorInputMap(m_inputMap);
    // 这一行是回归脚本的锚点：`source=factory` / `source=file` 一眼可判，
    // 且**两条分支都会打**（"以为读了文件其实没读"是最难查的一类）。
    VK_LOG_INFO("INS-BIND source=%s path=%s actions=%s",
                fromFile ? "file" : "factory", editorInputMapPath().c_str(),
                m_inputMap.describe().c_str());
    m_inputMapDirty = false;
}

bool EditorContext::saveInputMap() {
    if (!saveEditorInputMap(m_inputMap)) {
        setStatus(TR("Key bindings NOT saved (see log)"));
        return false;
    }
    m_inputMapDirty = false;
    setStatus(TR("Key bindings saved"));
    return true;
}

bool EditorContext::rebindAction(const std::string& context,
                                 const std::string& action,
                                 const core::InputBinding& binding) {
    // ⚠ 两条日志打在**这里**而不是面板里：面板的"捕获"和自动化的
    //   `MYVK_EDITOR_REBIND` 注入走的是同一条写路径，日志跟着路径走才能
    //   保证"界面能做的 = 脚本能测的"。放在面板里的话，脚本改绑不会打
    //   CLASH，冲突检测就少了一半覆盖。
    const std::string key = m_inputMap.describeBindingOne(binding);
    const std::vector<std::string> clash =
        m_inputMap.conflictingActions(context, binding, action);

    const std::string label = "Rebind " + action + " -> " + key;
    const bool changed = inputMapEdit(label, [&]() {
        return m_inputMap.setActionBinding(context, action, binding);
    });
    VK_LOG_INFO("INS-BIND-SET context=%s action=%s key=%s changed=%d "
                "conflicts=%zu",
                context.c_str(), action.c_str(), key.c_str(),
                changed ? 1 : 0, clash.size());
    if (!clash.empty()) {
        std::string names;
        for (std::size_t i = 0; i < clash.size(); ++i) {
            if (i) names += ", ";
            names += clash[i];
        }
        // 冲突**不阻止**绑定：同键多动作在游戏里是合法配置（玩家自己的
        // 选择），面板只负责让他知道。
        VK_LOG_WARN("INS-BIND-CLASH action=%s key=%s also=%s", action.c_str(),
                    key.c_str(), names.c_str());
    }
    return changed;
}

bool EditorContext::resetActionBinding(const std::string& context,
                                      const std::string& action) {
    return inputMapEdit("Reset binding: " + action, [&]() {
        return m_inputMap.resetActionBinding(context, action);
    });
}

bool EditorContext::clearActionBinding(const std::string& context,
                                      const std::string& action) {
    // 只解键盘：手柄绑定留着（见 core::InputMap::setActionBinding 的说明，
    // 面板在 v1 里只捕获键盘）。
    return inputMapEdit("Clear binding: " + action, [&]() {
        m_inputMap.clearActionBinding(context, action, core::InputSource::Key);
        return true;
    });
}

bool EditorContext::resetAllBindings() {
    return inputMapEdit("Reset all key bindings", [&]() {
        m_inputMap.resetToDefault();
        return true;
    });
}

void EditorContext::beginInputCapture(std::string context, std::string action) {
    m_captureContext = std::move(context);
    m_captureAction = std::move(action);
    setStatus("Press a key for " + m_captureAction + "  (Esc = cancel)");
}

void EditorContext::endInputCapture() {
    m_captureContext.clear();
    m_captureAction.clear();
}

// ---------------------------------------------------------------- 提示

void EditorContext::notify(std::string msg) {
    m_notification = std::move(msg);
    m_notificationTimer = 3.0f;
}

void EditorContext::tickNotification(float dt) {
    if (m_notificationTimer > 0.0f) {
        m_notificationTimer -= dt;
        if (m_notificationTimer <= 0.0f) {
            m_notificationTimer = 0.0f;
            m_notification.clear();
        }
    }
}

} // namespace editor
