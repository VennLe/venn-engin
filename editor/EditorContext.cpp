#include "EditorContext.h"

#include "assets/AssetManager.h"
#include "core/Logger.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <cstdlib>
#include <utility>

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

// ---------------------------------------------------------------- 吸附

bool EditorContext::snapForCurrentMode() const {
    switch (m_gizmoMode) {
        case GizmoMode::Translate: return m_snapMove;
        case GizmoMode::Rotate: return m_snapRotate;
        default: return m_snapScale;
    }
}

float EditorContext::snapStepForCurrentMode() const {
    switch (m_gizmoMode) {
        case GizmoMode::Translate: return m_snapMoveStep;
        case GizmoMode::Rotate: return m_snapRotateStep;
        default: return m_snapScaleStep;
    }
}

// 只翻转"当前模式"那一套 —— 工具栏上的快捷开关按 X 用的就是它
// （UE5 里 X 是"切换当前工具的网格吸附"，行为一致）。
void EditorContext::toggleSnapForCurrentMode() {
    switch (m_gizmoMode) {
        case GizmoMode::Translate: m_snapMove = !m_snapMove; break;
        case GizmoMode::Rotate: m_snapRotate = !m_snapRotate; break;
        default: m_snapScale = !m_snapScale; break;
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
}

void EditorContext::play() {
    if (m_play == PlayState::Playing) return;

    if (m_play == PlayState::Paused) {
        // 从暂停恢复：运行态的实体与状态原封不动，直接继续
        m_play = PlayState::Playing;
        setStatus("Resumed");
        return;
    }

    // ---- Stopped → Playing：把编辑态整份复制成运行态 ----
    std::string snap;
    const scene::SceneIoResult saved = scene::sceneToJson(m_editorScene, snap);
    if (!saved.ok) {
        VK_LOG_ERROR("Play failed: cannot snapshot scene: %s",
                     saved.error.c_str());
        setStatus("Play failed: " + saved.error);
        return;
    }

    const scene::SceneIoResult loaded =
        scene::sceneFromJson(m_runtimeScene, m_assets, snap);
    if (!loaded.ok) {
        VK_LOG_ERROR("Play failed: cannot restore snapshot: %s",
                     loaded.error.c_str());
        setStatus("Play failed: " + loaded.error);
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

    // 运行态的实体句柄与编辑态不同，选择先放下（名字留着，Stop 时接回来）
    m_selection = ecs::Entity{};

    VK_LOG_INFO("Editor: PLAY in viewport (runtime scene = %zu entities, "
                "editor untouched)",
                m_runtimeScene.objectCount());
    setStatus("Playing in viewport (F = fullscreen)");
    notify("Playing in viewport (F = fullscreen)");
}

void EditorContext::pause() {
    if (m_play != PlayState::Playing) return;
    m_play = PlayState::Paused;
    setStatus("Paused");
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
    setStatus("Stopped");
}

void EditorContext::togglePlayPause() {
    if (m_play == PlayState::Playing) {
        pause();
    } else {
        play();
    }
}

void EditorContext::onSceneReplaced() {
    // 换场景必然要先丢掉正在跑的运行态副本 —— 否则视口里还是**旧**场景的
    // 游戏画面，而编辑态已经是新的了，两边对不上。
    stop();
    m_playTime = 0.0f;
    clearSelection();
    m_commands.clear();
    m_dirty = false;
    // 新场景的相机是刚从一个 JSON 读出来的（轨道模式）—— 重新设回编辑态相机
    applyEditorCameraMode();
}

// ---------------------------------------------------------------- 选择

void EditorContext::select(ecs::Entity e) {
    if (e.valid() && !activeScene().world().alive(e)) e = ecs::Entity{};
    m_selection = e;
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
    if (!m_selectionName.empty()) {
        const ecs::Entity found = sc.find(m_selectionName);
        if (found.valid()) m_selection = found;
    }
}

// ---------------------------------------------------------------- 撤销

void EditorContext::undo() {
    if (!m_commands.undo()) {
        setStatus("Nothing to undo");
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
        setStatus("Nothing to redo");
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
