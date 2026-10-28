#include "EditorContext.h"

#include "EditorScene.h"
#include "InputMapIO.h"
#include "Localization.h"
#include "assets/AssetManager.h"
#include "core/Logger.h"
#include "scene/Prefab.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include "assets/AssetPath.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace editor {

namespace {

// ============================================================
// 出生点（P5，2026-10-22；2026-10-24 扩成"生成 + 归位"）
// ============================================================
// 用户原话（第一轮）："角色初始化的位置为什么不是 GameStart 的位置，
// GameStart 只是视口里的标记作用，当游戏启动运行的时候，这个标记就要消失，
// 然后创建的角色放在这个位置。"
//
// 用户原话（2026-10-24）："GameStart 不仅仅是角色的一个标记，也是游戏运行
// 后角色初始化的坐标，在添加人称角色时不需要直接把角色加入到 3d 视口中，
// 只是在游戏运行后，把角色初始化到 GameStart 位置即可。"
//
// 所以 Play 要做的是**两件事之一**，按"运行态里有没有玩家实体"分：
//   · **没有**（新语义，常态）→ 按 GameStart 现造一个（`spawnPlayerAtGameStart`），
//     第三人称顺手装上默认角色。角色从这一刻才存在，编辑态场景里没有它。
//   · **有**（老场景 / 脚本摆的 / 手工加过 PlayerComponent）→ 只把它归位到
//     GameStart，行为与之前完全一致，不重复生成。
//
// `spawnOnRun = false` 的出生点表示"这个场景不要玩家"（纯观览），此时
// 明明有出生点也不生成 —— 老场景里没有这个字段，解析时默认 true。
//
// 返回 true = 运行态里有玩家了（新造的或归位过的）。
bool preparePlayerAtGameStart(scene::Scene& scene,
                              assets::AssetManager& assets) {
    ecs::World& w = scene.world();

    // ---- 出生点：第一个启用的 ----
    glm::vec3 feet(0.0f);
    float yaw = 0.0f;
    bool found = false;
    ecs::PlayerMode mode = ecs::PlayerMode::ThirdPerson;
    bool spawnOnRun = true;
    ecs::Entity startEntity{};
    w.each<ecs::GameStartComponent>(
        [&](ecs::Entity e, ecs::GameStartComponent& g) {
            if (found || !g.enabled) return;
            const auto* tf = w.get<ecs::TransformComponent>(e);
            if (!tf) return;
            feet = tf->position;
            yaw = tf->rotation.z + g.yaw;
            mode = g.mode;
            spawnOnRun = g.spawnOnRun;
            startEntity = e;
            found = true;
        });

    // ---- 运行态里已经有玩家了吗 ----
    ecs::Entity player;
    w.each<ecs::CharacterControllerComponent>(
        [&](ecs::Entity e, ecs::CharacterControllerComponent& cc) {
            if (!player.valid() && cc.enabled) player = e;
        });

    if (!player.valid()) {
        if (!found || !spawnOnRun) {
            VK_LOG_INFO("EDIT-PLAYER-SPAWN skipped found=%d spawnOnRun=%d",
                        found ? 1 : 0, spawnOnRun ? 1 : 0);
            return false;
        }
        const ecs::Entity e =
            spawnPlayerAtGameStart(scene, assets, feet, yaw, mode, startEntity);
        VK_LOG_INFO("EDIT-PLAYER-SPAWN source=spawn mode=%s at=(%.2f,%.2f,%.2f) "
                    "yaw=%.3f ok=%d",
                    mode == ecs::PlayerMode::FirstPerson ? "first" : "third",
                    static_cast<double>(feet.x), static_cast<double>(feet.y),
                    static_cast<double>(feet.z), static_cast<double>(yaw),
                    e.valid() ? 1 : 0);
        return e.valid();
    }

    // 老场景：把已有玩家归位（一个字节的行为都不改，只是少了"没出生点就
    // 留在原处"这个分支的日志噪音）。
    if (!found) return false;

    if (auto* tc = w.get<ecs::TransformComponent>(player)) {
        tc->position = feet;
        tc->rotation = glm::vec3(0.0f, 0.0f, yaw);
    }
    // 胶囊中心与 `PlayerController::attach` 同一口径：脚底 + (半高 + 半径)。
    if (auto* cc = w.get<ecs::CharacterControllerComponent>(player)) {
        cc->bodyYaw = yaw;
        cc->position =
            feet + glm::vec3(0.0f, 0.0f, cc->halfHeight + cc->radius);
    }
    // 玩家的"起步"状态要清掉：出生点变了，上一轮 Play 残留的 started
    // 会让起步动画/冲刺惯性直接跳过。
    if (auto* pc = w.get<ecs::PlayerComponent>(player)) {
        pc->started = false;
        pc->walkPhase = 0.0f;
        // 跳跃的竖直状态也要归零：上一轮 Play 若停在半空，不清的话这一轮
        // 一进门就在下落（表现是"一按 Play 人就往下掉"）。
        pc->velocityZ = 0.0f;
        pc->onGround = true;
    }
    VK_LOG_INFO("EDIT-PLAYER-SPAWN source=seat pos=(%.2f,%.2f,%.2f) yaw=%.3f",
                static_cast<double>(feet.x), static_cast<double>(feet.y),
                static_cast<double>(feet.z), static_cast<double>(yaw));
    return true;
}

} // namespace

EditorContext::EditorContext(assets::AssetManager& assets) : m_assets(assets) {
    // MYVK_GRID=0：连默认值一起关掉地平面栅格。
    // 用途是做像素级回归对比 —— 栅格是有抗锯齿的细线，任何一度视角变化
    // 都会牵动大片像素，把它排除掉才能让对比图有意义（和 MYVK_HIDE_UI
    // 是同一类开关）。
    if (const char* s = std::getenv("MYVK_GRID")) {
        if (s[0] == '0' || s[0] == 'f' || s[0] == 'F') m_showGrid = false;
    }
}

// ---------------------------------------------------------------- 材质仓库

assets::MaterialLibrary& EditorContext::materialLibrary() {
    // 惰性加载：第一次被问到才读盘。失败也记成"试过了"，
    // 否则每帧都会重试一次 IO —— 空仓库时那就是每帧一次失败的 fopen。
    if (!m_materialLibraryTried) {
        m_materialLibraryTried = true;
        m_materialLibrary.load();
    }
    return m_materialLibrary;
}

void EditorContext::reloadMaterialLibrary() {
    m_materialLibraryTried = true;
    m_materialLibrary.reload();
    ++m_materialLibraryRevision;     // 缩略图缓存据此失效
    // 笔刷面板上"当前层用的是哪个材质"的快照也要跟着失效 ——
    // 材质被改名/删掉时，界面上不该继续显示一个已不存在的名字。
    notify(TR("Material library reloaded"));
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

// 编辑模式切换。**只存状态 + 打日志**：
//   · 面板跟模式走由 EditorApp 每帧按差做（它才拿得到面板表 / 焦点）；
//   · 视口的笔刷输入由 ViewportPanel 读本状态；
//   · 两者都不该在这里被调用 —— 否则"切模式"就成了一次副作用打包。
//
// 打日志的条件是"真的变了"：Toolbar 的下拉、脚本钩子、将来可能的快捷键
// 都会走这里，重复设同一模式不该刷日志（自动化按它断言的）。
void EditorContext::setEditMode(EditMode m) {
    if (m == m_editMode) return;
    m_editMode = m;
    VK_LOG_INFO("EDITMODE %s implemented=%d", editModeId(m),
                editModeImplemented(m) ? 1 : 0);
}

void EditorContext::play() {
    if (m_play == PlayState::Playing) return;

    if (m_play == PlayState::Paused) {
        // 从暂停恢复：运行态的实体与状态原封不动，直接继续
        m_play = PlayState::Playing;
        // 恢复后**不自动接管鼠标**（P8）：玩家要再点一次视口。
        // 理由与运行时"从暂停菜单继续"完全一致 —— 否则一按继续鼠标就消失，
        // 玩家会以为鼠标失灵了。
        setPlayMouseCaptured(false);
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

    // ---- 出生点（P5）：玩家以 GameStart 为准，不是编辑态摆的位置 ----
    // 放在相机设置之前：玩家的位置决定相机的注视点，反过来的话第一帧会
    // 从旧位置"飞"到出生点。
    preparePlayerAtGameStart(m_runtimeScene, m_assets);

    // 相机状态随快照一起复制过来，所以按下 Play 的瞬间画面与编辑画面连续。
    // 切成自由飞行：游戏视角是第一人称式的（转自己），不是绕着目标点打转。
    //
    // 关键：**从这一刻起编辑器不再驱动这台相机**。视口在 Play 期间不处理
    // 任何键鼠导航（见 ViewportPanel::draw），相机怎么动完全由游戏逻辑决定。
    m_runtimeScene.camera().setUseGlobalInput(false);
    m_runtimeScene.camera().setFlyMode(true);

    m_playTime = 0.0f;
    m_play = PlayState::Playing;
    // 进 Play **不自动接管鼠标**（P8，2026-10-23）：用户明确要求
    // "必须先用左键在 3D 视口里点一下才开始和角色交互"。
    // 光标这时是可见的，玩家可以先去点编辑器的面板 / 改参数。
    //
    // ⚠ 这里**绕开 setPlayMouseCaptured 的"值没变就不脏"短路**，直接置脏：
    //   进 Play 时它本来就是 false，短路会让 `EDIT-MOUSE captured=0` 这条
    //   锚点根本不打出来 —— 而验收脚本要拿它证明"进 Play 时鼠标是可见的"。
    m_playMouseCaptured = false;
    m_playCursorDirty = true;

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
    // 暂停 = 退出操作态（P8）：暂停时玩家要能用鼠标去点播放条 / 面板。
    setPlayMouseCaptured(false);
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
    // Stop 之后鼠标一定要回来（P8）—— 否则编辑态里光标是隐藏的，
    // 用户会以为编辑器卡死了。
    setPlayMouseCaptured(false);

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
    // 关节锚点拾取同理，而且后果更严重：拾取模式记着"要写进哪个实体"，
    // 换了选中项之后那一条就是**过期目标**，接着点视口会把锚点写到
    // 用户已经不看的东西身上。宁可让他重点一次按钮。
    cancelAnchorPick();
    if (e.valid()) {
        if (const std::string* n = activeScene().world().name(e))
            m_selectionName = *n;
        else
            m_selectionName.clear();
        m_selectionGuid = activeScene().world().guid(e);
    } else {
        m_selectionGuid = ecs::kInvalidGuid;
    }

    // ---- 选择集：单选入口把集合**替换**成一项 ----
    // 所有既有调用点（层级点选、内容浏览器导入、撤销后的重接、prefab 实例化
    // 之后的选中……）走的都是这里，所以它们的语义与多选之前逐位一致。
    m_selectionSet.clear();
    m_selectionNames.clear();
    if (e.valid()) {
        m_selectionSet.push_back(e);
        m_selectionNames.push_back(m_selectionName);
    }
}

void EditorContext::clearSelection() {
    m_selection = ecs::Entity{};
    m_selectionName.clear();
    m_selectionGuid = ecs::kInvalidGuid;
    m_selectionFromViewport = false;
    m_colliderEdit = false;
    cancelAnchorPick();
    m_selectionSet.clear();
    m_selectionNames.clear();
}

bool EditorContext::hasSelection() const {
    return m_selection.valid() && activeScene().world().alive(m_selection);
}

// ---------------------------------------------------------------- 选择集（多选）

bool EditorContext::isSelected(ecs::Entity e) const {
    if (!e.valid()) return false;
    for (ecs::Entity s : m_selectionSet)
        if (s.id == e.id && s.generation == e.generation) return true;
    return false;
}

// 改主选中项的唯一入口（集合的增删另有自己的路径，但要在这里收口，否则
// "主选中项 ∈ 集合"这条不变式迟早被某一条支路打破）。
void EditorContext::setSelectionPrimary(ecs::Entity e) {
    if (e.valid() && !activeScene().world().alive(e)) e = ecs::Entity{};
    m_selection = e;
    m_colliderEdit = false;
    cancelAnchorPick();
    // 支路（Ctrl 点、框选、全选）里主选中项都是"刚操作的那一个"，
    // 而不是"在视口里点中的"—— 与 selectFromViewport 的出处标记无关。
    m_selectionFromViewport = false;
    m_selectionName.clear();
    m_selectionGuid = ecs::kInvalidGuid;
    if (!e.valid()) return;
    if (const std::string* n = activeScene().world().name(e)) m_selectionName = *n;
    m_selectionGuid = activeScene().world().guid(e);
    // 名字数组也跟着修一下：主选中项那一项的缓存可能已经过期（改过名）。
    for (std::size_t i = 0; i < m_selectionSet.size(); ++i)
        if (m_selectionSet[i].id == e.id) m_selectionNames[i] = m_selectionName;
}

void EditorContext::selectToggle(ecs::Entity e) {
    if (!e.valid() || !activeScene().world().alive(e)) return;

    const auto it = std::find_if(
        m_selectionSet.begin(), m_selectionSet.end(),
        [&](ecs::Entity s) { return s.id == e.id; });
    if (it != m_selectionSet.end()) {
        m_selectionNames.erase(m_selectionNames.begin() +
                               (it - m_selectionSet.begin()));
        m_selectionSet.erase(it);
        // 取消掉的是主选中项 → 退回集合里最后一项（"最后点过的"那个）。
        // 集合空了就整个清空（m_selection 必须跟着失效，见不变式）。
        setSelectionPrimary(m_selectionSet.empty() ? ecs::Entity{}
                                                   : m_selectionSet.back());
        return;
    }

    m_selectionSet.push_back(e);
    const std::string* n = activeScene().world().name(e);
    m_selectionNames.push_back(n ? *n : std::string());
    setSelectionPrimary(e);
}

void EditorContext::selectRange(ecs::Entity e,
                               const std::vector<ecs::Entity>& order) {
    if (!e.valid()) return;
    // "锚点" = 上一次点的那一项。它已经不在 order 里（被删了 / 被过滤掉）
    // 时退化成"只选这一项"，不要硬凑一个范围出来。
    int anchor = -1;
    int target = -1;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i].id == e.id) target = static_cast<int>(i);
        if (m_selection.valid() && order[i].id == m_selection.id)
            anchor = static_cast<int>(i);
    }
    if (target < 0) return;
    if (anchor < 0) {
        select(e);
        return;
    }

    const int lo = std::min(anchor, target);
    const int hi = std::max(anchor, target);
    m_selectionSet.clear();
    m_selectionNames.clear();
    for (int i = lo; i <= hi; ++i) {
        const ecs::Entity s = order[static_cast<std::size_t>(i)];
        // 上锁的实体可以进集合（层级里手选不算"点视口"），这里不过滤。
        m_selectionSet.push_back(s);
        const std::string* n = activeScene().world().name(s);
        m_selectionNames.push_back(n ? *n : std::string());
    }
    setSelectionPrimary(e);
}

void EditorContext::selectMany(const std::vector<ecs::Entity>& list) {
    ecs::World& w = activeScene().world();
    const ecs::Entity prev = m_selection;

    m_selectionSet.clear();
    m_selectionNames.clear();
    for (ecs::Entity s : list) {
        if (!s.valid() || !w.alive(s)) continue;
        if (isSelected(s)) continue;          // 去重（框选 + Shift 叠加时会撞）
        m_selectionSet.push_back(s);
        const std::string* n = w.name(s);
        m_selectionNames.push_back(n ? *n : std::string());
    }

    if (m_selectionSet.empty()) {
        setSelectionPrimary(ecs::Entity{});
        return;
    }
    // 旧主选中项还在新集合里就留着 —— "拖一次框，手柄的坐标系不该跳"。
    const bool keep = prev.valid() && isSelected(prev);
    setSelectionPrimary(keep ? prev : m_selectionSet.front());
}

void EditorContext::selectAll() {
    ecs::World& w = activeScene().world();
    std::vector<ecs::Entity> all;
    w.each<ecs::TransformComponent>([&](ecs::Entity e, ecs::TransformComponent&) {
        // 上锁的（自带地面）不参与：它既点不到也拖不动，进集合只会在
        // 批量移动时制造一条"这个怎么没动"的困惑。
        if (w.has<ecs::LockedComponent>(e)) return;
        all.push_back(e);
    });
    selectMany(all);
    VK_LOG_INFO("SELECT all n=%zu", m_selectionSet.size());
}

void EditorContext::invertSelection() {
    ecs::World& w = activeScene().world();
    // 判据必须与 selectAll 逐字相同（同样跳过上锁的），否则会出现
    // "全选里有 A、反选里也有 A"这种自相矛盾的集合。
    std::vector<ecs::Entity> rest;
    w.each<ecs::TransformComponent>([&](ecs::Entity e, ecs::TransformComponent&) {
        if (w.has<ecs::LockedComponent>(e)) return;
        if (isSelected(e)) return;      // ← 与 selectAll 的**唯一**差别
        rest.push_back(e);
    });
    selectMany(rest);                   // selectMany 会整份替换 → 结果就是反选
    VK_LOG_INFO("SELECT invert n=%zu", m_selectionSet.size());
}

void EditorContext::cancelAnchorPick() {
    if (m_anchorPick == AnchorPick::None) return;
    // 打出来：调用方可能是"换选中项"这种不相干的地方，用户不会想到
    // 自己配到一半的锚点拾取是被它清掉的。
    VK_LOG_INFO("JOINT-PICK cancel reason=implicit state=%d entity=%u",
                static_cast<int>(m_anchorPick), m_anchorPickEntity.id);
    m_anchorPick = AnchorPick::None;
    m_anchorPickEntity = ecs::Entity{};
}

bool EditorContext::advanceAnchorPick() {
    if (m_anchorPick == AnchorPick::Self) {
        m_anchorPick = AnchorPick::Target;
        return true;
    }
    // Target 之后收工；None 调进来就是"本来就没在拾取"，同样返回 false。
    m_anchorPick = AnchorPick::None;
    m_anchorPickEntity = ecs::Entity{};
    return false;
}

glm::vec3 EditorContext::selectionPivot() {
    scene::Scene& sc = activeScene();
    if (m_selectionSet.empty()) {
        if (!m_selection.valid()) return glm::vec3(0.0f);
        return glm::vec3(sc.worldMatrix(m_selection)[3]);
    }
    glm::vec3 lo(0.0f);
    glm::vec3 hi(0.0f);
    bool first = true;
    for (ecs::Entity s : m_selectionSet) {
        if (!sc.world().alive(s)) continue;
        const glm::vec3 p = glm::vec3(sc.worldMatrix(s)[3]);
        if (first) {
            lo = hi = p;
            first = false;
        } else {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    if (first) return glm::vec3(0.0f);
    return (lo + hi) * 0.5f;
}

// ---------------------------------------------------------------- 预制体（G18）

namespace {

std::string readWholeFile(const std::string& full) {
    std::ifstream in(full, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// 名字 → 资产路径用的 slug（`Crate 2` → `crate_2`）
// （已搬到 `scene::prefabDefaultPath` —— 层级右键菜单也要用同一套取名口径，
//   两处各写一份必然出现 "crate.vprefab.json" 与 "crate_2.vprefab.json"）

} // namespace

bool EditorContext::prefabCreateFromSelection(const std::string& relPath) {
    if (!isEditing()) return false;
    const ecs::Entity src = m_selection;
    if (!src.valid()) {
        notify(TR("Select an object first"));
        return false;
    }

    scene::PrefabAsset asset;
    std::string err;
    if (!scene::prefabFromSubtree(m_editorScene, src, asset, &err)) {
        notify(TR("Create prefab failed: ") + err);
        return false;
    }
    asset.path = relPath;
    const std::string* nm = m_editorScene.world().name(src);
    asset.name = nm ? *nm : std::string("Prefab");
    asset.comment = "由编辑器从选中子树导出（Create Prefab from Selection）。";

    // 目录可能还不存在（第一次给这个工程做 prefab）
    const std::string full = assets::resolveAssetPath(relPath);
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(full).parent_path(), ec);

    if (!scene::savePrefab(asset, err)) {
        notify(TR("Create prefab failed: ") + err);
        return false;
    }

    // 就地绑定：这棵子树**当场**成为实例 1。否则作者要先删掉再拖一份
    // 进来，而"删了才发现没存上"是最典型的踩坑顺序。
    std::uint32_t inst = 0;
    structuralEdit(TR("Create Prefab"), [&] {
        scene::prefabBindExistingSubtree(m_editorScene, asset, src, &inst);
    });
    notify(TR("Prefab created: ") + relPath);
    setStatus(TR("Create Prefab"));
    VK_LOG_INFO("HIER-PREFAB create=1 path=%s nodes=%d instance=%u",
                relPath.c_str(), static_cast<int>(asset.nodes.size()), inst);
    return true;
}

bool EditorContext::prefabApply(ecs::Entity member) {
    if (!isEditing() || !member.valid()) return false;
    const auto* pl = m_editorScene.world().get<ecs::PrefabLinkComponent>(member);
    if (!pl) return false;
    const std::string rel  = pl->prefabPath;
    const std::uint32_t id = pl->instance;

    const std::string full   = assets::resolveAssetPath(rel);
    const std::string before = readWholeFile(full);

    scene::PrefabAsset asset;
    if (!scene::loadPrefab(rel, asset)) {
        notify(TR("Apply failed: cannot load ") + rel);
        return false;
    }

    const std::string sceneBefore = snapshot();
    int cleared = 0;
    const int wrote = scene::applyInstanceToAsset(m_editorScene, asset, id, &cleared);
    std::string err;
    if (!scene::savePrefab(asset, err)) {
        notify(TR("Apply failed: ") + err);
        return false;
    }

    // Apply 之后**必须**让该 prefab 的所有实例重新同步：覆盖已清空，但
    // 别的实例手里还是旧值 —— 不同步的话只有被 Apply 的那一份变，
    // 看起来像"Apply 只对一半生效"。
    {
        scene::PrefabAsset fresh;
        if (scene::loadPrefab(rel, fresh))
            scene::syncInstances(m_editorScene, m_assets, fresh);
    }

    const std::string sceneAfter = snapshot();
    const std::string after      = readWholeFile(full);
    if (sceneBefore != sceneAfter || before != after) {
        m_commands.pushAlreadyApplied(std::make_unique<editor::PrefabApplyCommand>(
            TR("Apply to Prefab"), &m_editorScene, &m_assets, full, before, after,
            sceneBefore, sceneAfter));
        m_dirty = true;
    }
    setStatus(TR("Apply to Prefab"));
    notify(TR("Applied to ") + rel);
    VK_LOG_INFO("HIER-PREFAB apply=1 path=%s instance=%u wrote=%d cleared=%d",
                rel.c_str(), id, wrote, cleared);
    return true;
}

bool EditorContext::prefabRevert(ecs::Entity member) {
    if (!isEditing() || !member.valid()) return false;
    const auto* pl = m_editorScene.world().get<ecs::PrefabLinkComponent>(member);
    if (!pl) return false;
    const std::string rel  = pl->prefabPath;
    const std::uint32_t id = pl->instance;

    scene::PrefabAsset asset;
    if (!scene::loadPrefab(rel, asset)) {
        notify(TR("Revert failed: cannot load ") + rel);
        return false;
    }

    int cleared = 0;
    structuralEdit(TR("Revert from Prefab"), [&] {
        cleared = scene::clearInstanceOverrides(m_editorScene, id);
        scene::syncInstances(m_editorScene, m_assets, asset);
    });
    notify(TR("Reverted to ") + rel);
    setStatus(TR("Revert from Prefab"));
    VK_LOG_INFO("HIER-PREFAB revert=1 path=%s instance=%u cleared=%d",
                rel.c_str(), id, cleared);
    return true;
}

bool EditorContext::prefabInstantiate(const std::string& relPath,
                                      const glm::vec3& at) {
    if (!isEditing()) return false;
    scene::PrefabAsset asset;
    if (!scene::loadPrefab(relPath, asset)) {
        notify(TR("Instantiate failed: cannot load ") + relPath);
        VK_LOG_ERROR("HIER-PREFAB instantiate=0 path=%s err=%s",
                     relPath.c_str(), asset.error.c_str());
        return false;
    }
    scene::PrefabInstance inst;
    std::string err;
    structuralEdit(TR("Instantiate Prefab"), [&] {
        scene::instantiatePrefab(m_editorScene, m_assets, asset, at, inst, &err);
    });
    if (!inst.root.valid()) {
        notify(TR("Instantiate failed: ") + err);
        return false;
    }
    select(inst.root);
    notify(TR("Instantiated ") + relPath);
    setStatus(TR("Instantiate Prefab"));
    VK_LOG_INFO("HIER-PREFAB instantiate=1 path=%s root=%u members=%d",
                relPath.c_str(), static_cast<unsigned>(inst.root.id),
                static_cast<int>(inst.members.size()));
    return true;
}

void EditorContext::validateSelection() {
    scene::Scene& sc = activeScene();
    ecs::World& w = sc.world();

    // ---- ① 逐个修补集合成员 ----
    // 句柄失效 —— 撤销删除 / Play / Stop 之后都会走到这里。名字是跨"场景
    // 重建"唯一稳定的标识，按它找回来；名字也找不到就**丢掉那一项**，
    // 否则下一次批量移动会拿一个死句柄去写内存。
    std::vector<ecs::Entity> set;
    std::vector<std::string> names;
    set.reserve(m_selectionSet.size());
    names.reserve(m_selectionSet.size());
    std::size_t restored = 0;
    std::size_t dropped = 0;
    for (std::size_t i = 0; i < m_selectionSet.size(); ++i) {
        ecs::Entity s = m_selectionSet[i];
        const std::string nm =
            (i < m_selectionNames.size()) ? m_selectionNames[i] : std::string();
        if (!s.valid() || !w.alive(s)) {
            s = nm.empty() ? ecs::Entity{} : sc.find(nm);
            if (!s.valid()) {
                ++dropped;
                continue;
            }
            ++restored;
        }
        bool dup = false;
        for (ecs::Entity k : set)
            if (k.id == s.id) dup = true;
        if (dup) continue;
        const std::string* np = w.name(s);
        set.push_back(s);
        names.push_back(np ? *np : nm);
    }

    // ---- ② 修主选中项 ----
    if (m_selection.valid() && w.alive(m_selection)) {
        if (const std::string* n = w.name(m_selection))
            m_selectionName = *n;
        // 活着就顺手刷新身份快照 —— 这样"改名之后撤销"也能靠 guid 接回来
        m_selectionGuid = w.guid(m_selection);
    } else {
        // 新建出来的组件是全新的，手柄不能再指着一个"已经不存在的碰撞体"
        m_colliderEdit = false;
        m_selection = ecs::Entity{};
        // guid 优先、名字兜底（见 `Scene::reconnectByGuidOrName` 的说明）。
        // ⚠ 顺序不能反：撤销会把整个世界**重建**，两个同名 `Crate` 时按名字找
        //   永远返回第一个 —— 用户选的是第二个，撤销一次就跳到第一个，
        //   而画面、层级、属性面板全都"看着正常"。这就是 H5 的现网 bug。
        m_selection = sc.reconnectByGuidOrName(m_selectionGuid, m_selectionName);
        if (m_selection.valid()) {
            // 接回来的可能是新 guid（老场景里原来没有，是加载时补发的）
            m_selectionGuid = w.guid(m_selection);
        } else if (m_selectionGuid != ecs::kInvalidGuid || !m_selectionName.empty()) {
            VK_LOG_WARN("INS-SELECT reconnect failed name='%s' guid=%016llx",
                        m_selectionName.c_str(),
                        static_cast<unsigned long long>(m_selectionGuid));
            m_selectionGuid = ecs::kInvalidGuid;
        }
    }

    // ---- ③ 收口不变式：集合为空 ⇔ 主选中项无效 ----
    if (m_selectionSet.size() != set.size() || restored || dropped) {
        VK_LOG_INFO("SELECT set repaired n=%zu restored=%zu dropped=%zu",
                    set.size(), restored, dropped);
    }
    m_selectionSet.swap(set);
    m_selectionNames.swap(names);

    if (m_selectionSet.empty()) {
        if (m_selection.valid()) {
            // 主选中项还活着但没进集合（历史上只可能来自"直接改 m_selection"
            // 的支路）—— 以它为准建一个单元素集合，别把它悄悄丢了。
            m_selectionSet.push_back(m_selection);
            m_selectionNames.push_back(m_selectionName);
        }
        return;
    }
    if (!m_selection.valid()) {
        m_selection = m_selectionSet.front();
        if (const std::string* n = w.name(m_selection)) m_selectionName = *n;
        m_selectionGuid = w.guid(m_selection);
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
