#include "RuntimeApp.h"

#include "assets/AssetPath.h"
#include "core/Input.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "i18n/I18n.h"
#include "physics/PhysicsWorld.h"
#include "scene/CameraSystem.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "script/ScriptEngine.h"
#include "script/ScriptRuntime.h"
#include "ui2/UiCanvas.h"

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace runtime {

RuntimeApp::RuntimeApp(const RuntimeConfig& cfg)
    : core::Application(cfg.title, cfg.width, cfg.height), m_cfg(cfg) {
    m_loop.config().maxFps = cfg.maxFps;
    m_loop.config().unfocusedFps = cfg.unfocusedFps;
    m_loop.config().timeScale = cfg.timeScale;
    // GAME-LOOP 每 60 帧一行：只在 verbose 下打（正常玩不需要刷屏）。
    m_loop.setLogInterval(cfg.verbose ? 60 : 0);

    if (const char* dump = std::getenv("MYVK_PHYS_DUMP")) {
        const int n = std::atoi(dump);
        if (n > 0) m_physicsDumpEvery = n;
    }
    // M2 的两个调试口子。刻意都用 `MYVK_RUNTIME_` 前缀：它与其他运行时
    // 开关同一条规矩（前缀放行），不需要再往白名单里加裸名字。
    if (const char* d = std::getenv("MYVK_RUNTIME_PLAYER_DUMP")) {
        const int n = std::atoi(d);
        if (n > 0) m_playerDumpEvery = n;
    }
    if (const char* c = std::getenv("MYVK_RUNTIME_CONTACT_TRACE")) {
        m_contactTrace = (c[0] != '0');
        m_contacts.setTrace(m_contactTrace);
    }
}

RuntimeApp::~RuntimeApp() = default;

// ---------------------------------------------------------------- 启动

void RuntimeApp::onInit() {
    VK_LOG_INFO("GAME-INIT runtime booting (scene='%s')", m_cfg.scenePath.c_str());

    // ---- 界面语言（U7）----
    // 词典与查表在 `engine/i18n/`（与编辑器**同一份**），这里只决定"用哪种"。
    //
    // 优先级：`MYVK_LANG` > `--language` / `MYVK_RUNTIME_LANGUAGE` / game.json。
    // ⚠ `MYVK_LANG` 不合 `MYVK_RUNTIME_*` 前缀纪律，是**唯一**的例外：
    //   路线图 U7 的验收条款就是按这个名字写的（`MYVK_LANG=zh`），
    //   它已在 `check_feature_surface.py` 的 ALLOW_EXACT 里显式登记。
    //   正规入口仍是 `--language` / game.json 的 `language`。
    //
    // ⚠ 认不出的值**不改语言**并打 WARN：静默回退英文会让"game.json 拼错"
    //   看起来像"配了但不生效"，那是最难查的一类。
    {
        const char* envLang = std::getenv("MYVK_LANG");
        const char* effective =
            (envLang && *envLang) ? envLang : m_cfg.language.c_str();
        const int idx = i18n::languageFromCode(effective);
        if (idx < 0) {
            VK_LOG_WARN("I18N-BADLANG value='%s' — 保留 %s（可用：en | zh）",
                        effective, i18n::languageCode(i18n::language()));
        } else {
            i18n::setLanguage(idx);
        }
        // 锚点（U7 验收）：中文模式是真实条数，英文模式**必须是 0**
        // —— 表根本没构造，这就是"英文 = 零开销"的观测量。
        VK_LOG_INFO("I18N-KEY lang=%s count=%zu",
                    i18n::languageCode(i18n::language()),
                    i18n::translationCount());
    }

    // 全屏在这里切（窗口已经存在；setFullscreen 会记住窗口模式下的几何）
    if (m_cfg.fullscreen) window().setFullscreen(true);

    // ---- 脚本运行时（S0）----
    m_scriptRuntime = std::make_unique<vks::ScriptRuntime>();
    m_scripts = &m_scriptRuntime->engine();
    installScriptServices();
    m_menuScene = std::make_unique<scene::Scene>();

    // 输入映射：出厂绑定 + 压一层 Gameplay（菜单进栈时会再压 Menu）
    //
    // G06：再叠一层**玩家改过的绑定表**（编辑器 Preferences ▸ Bindings 写的
    // 同一份文件）。文件不存在是正常状态 —— 从来没改过键位就一直是出厂绑定，
    // 所以这条路径**不报错**，只把"用的是哪一份"打出来。
    m_input = core::InputMap::makeDefault();
    {
        const std::string rel =
            m_cfg.inputMapPath.empty() ? "input_map.json" : m_cfg.inputMapPath;
        const std::string path = assets::resolveAssetPath(rel);
        const bool fromFile = m_input.loadFromFile(path);
        VK_LOG_INFO("GAME-INPUT source=%s path=%s",
                    fromFile ? "file" : "factory", path.c_str());
        // 逐条把**关键动作**的实际绑定点名打出来：这是"读进来了"与"真的
        // 生效了"之间唯一能被脚本断言的桥梁（改绑后这里不再是 Space）。
        VK_LOG_INFO("GAME-INPUT Jump=%s Sprint=%s Interact=%s",
                    m_input.describeBinding("Gameplay", "Jump").c_str(),
                    m_input.describeBinding("Gameplay", "Sprint").c_str(),
                    m_input.describeBinding("Gameplay", "Interact").c_str());
    }
    VK_LOG_INFO("GAME-INPUT %s", m_input.describe().c_str());

    // ---- 状态栈先立起来，再加载内容 ----
    // 顺序是有意为之：Loading 是"内容还没进来"的那一帧的真实状态；
    // 日志里先看到 Boot>Loading 再看到 GAME-SCENE，读起来是对的。
    m_stateStack.setTrace(m_cfg.traceStates);
    m_stateStack.push(makeBuiltinState(StateId::Boot), m_svc);
    m_stateStack.replace(makeBuiltinState(StateId::Loading), m_svc);

    loadStartScene();
    buildPhysics();
    preloadScripts();

    // ---- 玩家（G10）：场景里有角色控制器才接管 ----
    m_player = std::make_unique<PlayerController>(m_input);
    if (m_player->attach(activeScene(), m_binding.world.get())) {
        // 相机交给 CameraSystem（PlayerController 给意图），别再让
        // scene::Camera 自己读 WASD —— 否则两边会互相打架。
        activeScene().camera().setUseGlobalInput(false);
        VK_LOG_INFO("GAME-PLAYER attached '%s'",
                    activeScene().world().name(m_player->entity())
                        ? activeScene().world().name(m_player->entity())->c_str()
                        : "?");
    } else {
        m_player.reset();
        VK_LOG_INFO("GAME-PLAYER none (no CharacterControllerComponent)");
    }
    scene::activeCameraSystem().setTrace(m_cfg.traceStates);

    // ---- 音频（G14-5）----
    //
    // ⚠ 位置有讲究：必须在 `loadStartScene()` 之后（要读场景里的
    //   AudioSourceComponent），但可以在 `syncAudio` 第一次跑之前——
    //   设备打开是同步的，所以第一帧就有正确的格式供 Mixer init。
    //   设备打不开**不是错误**：`init` 内部进 Failed，所有 API 变空操作。
    m_audio.init(m_cfg.audio);

    // ---- 服务面接线 ----
    m_svc.simulate = [this](float dt) { simulate(dt); };
    m_svc.updateVariable = [this](float vdt) {
        // M2 的相机系统接在这里：PlayerController 已经算好"意图姿态"，
        // CameraSystem 负责优先级 / 混合 / 震动，最后写进 scene::Camera。
        // 用**真实帧时长**（不是固定步）：镜头不该被 timeScale 冻结。
        if (m_player)
            scene::activeCameraSystem().setDesiredPose(m_player->cameraPose());
        scene::activeCameraSystem().update(activeScene(), vdt);
    };
    m_svc.renderWorld = [this] {
        m_renderedWorldThisFrame = true;
        renderer().drawFrame(activeScene());
    };
    m_svc.timeScale = m_cfg.timeScale;

    // ---- Loading → MainMenu（→ Playing）----
    //
    // 默认流程**一定经过 MainMenu**（这样"清空 env 也能进主菜单"这件事
    // 是字面成立的），只是 M1 还没有菜单 UI（G15 才有）——没有可点的
    // "开始游戏"，所以 `startState != mainmenu` 时立刻接着进 Playing，
    // 等价于"菜单上自动按了继续"。
    //
    // `--start-state mainmenu` 停下来，用来验证 MainMenu 作为**不透明**
    // 状态的行为（此时世界不渲染，画面是空场景）。
    m_stateStack.replace(makeBuiltinState(StateId::MainMenu), m_svc);
    if (m_cfg.startState != "mainmenu") {
        m_stateStack.push(makeBuiltinState(StateId::Playing), m_svc);
    }

    // ---- HUD 外观（`config/ui.json`，U8）----
    // 文件不存在 = 出厂默认，这是**正常状态**，不打 WARN（同 E03 的纪律）。
    {
        UiConfig uc;
        const std::string p = defaultUiConfigPath();
        if (loadUiConfig(p, &uc)) m_hud.setConfig(uc);
    }

    // ---- 游戏 UI 的绘制挂点（U6/U8）----
    // `engine` 不认识 `runtime`（分层是 runtime → engine），所以由这里把
    // "画什么"注进去。回调在 Post 输出 pass 内、ImGui 之前被调用。
    ui2::setGameUiBuilder([this](ui2::UiCanvas& c, uint32_t w, uint32_t h) {
        if (w == 0 || h == 0) return;

        // ① HUD（血条 / 准星 / 伤害数字）
        //
        // 投影矩阵在这里算而不是缓存：相机每帧都可能被 CameraSystem 改，
        // 伤害数字要**贴着世界里的位置**——晚一帧就会"数字追着目标跑"。
        const auto& cam = activeScene().camera();
        const glm::mat4 vp =
            cam.projMatrix(static_cast<float>(w) / static_cast<float>(h)) *
            cam.viewMatrix();
        m_hud.build(c, m_hudModel, vp, w, h, ui2::frameFont());

        // ② 暂停菜单盖在最上面
        if (m_lastUiState > 0)
            m_pause.build(c, w, h, ui2::frameFont(), m_focus.focusId());
    });

    VK_LOG_INFO("GAME-INIT ready — state=%s entities=%zu bodies=%d scripts=%zu",
                m_stateStack.topName(), activeScene().world().entityCount(),
                m_binding.bodies, m_scripts->scriptCount());
}

void RuntimeApp::loadStartScene() {
    if (m_cfg.scenePath.empty()) {
        VK_LOG_INFO("GAME-SCENE path='' entities=%zu (empty world)",
                    activeScene().world().entityCount());
        return;
    }
    const std::string resolved = assets::resolveAssetPath(m_cfg.scenePath);
    const scene::SceneIoResult r =
        scene::loadScene(activeScene(), assets(), resolved);
    if (!r.ok) {
        // 场景加载失败**不该**让运行时起不来：窗口照开，世界是空的，
        // 日志里说清楚原因（出货版给玩家看"一片空白"至少要有据可查）。
        VK_LOG_ERROR("GAME-SCENE load failed path='%s' error=%s",
                     resolved.c_str(), r.error.c_str());
        return;
    }
    VK_LOG_INFO("GAME-SCENE path=%s entities=%d skipped=%d",
                resolved.c_str(), r.entities, r.skipped);

    // 读→写往返自检（`--dump-scene`）：把**刚读进来**的场景再写一份。
    // 单独测读路径是不够的 —— `saveScene` 是另一套代码，少写一个字段
    // 在"只读不写"的验证里永远暴露不出来。
    if (!m_cfg.dumpScenePath.empty()) {
        const scene::SceneIoResult w =
            scene::saveScene(activeScene(), m_cfg.dumpScenePath);
        if (w.ok)
            VK_LOG_INFO("GAME-SCENE dumped path=%s entities=%d skipped=%d",
                        m_cfg.dumpScenePath.c_str(), w.entities, w.skipped);
        else
            VK_LOG_ERROR("GAME-SCENE dump failed path=%s error=%s",
                         m_cfg.dumpScenePath.c_str(), w.error.c_str());
    }
}

void RuntimeApp::buildPhysics() {
    // 与编辑器**同一份**绑定逻辑（engine/physics/ScenePhysics.cpp）——
    // 这是"编辑器里调好的手感，出包一样"的结构保证。
    m_binding = physics::buildSceneWorld(activeScene());
    if (!m_binding.enabled) {
        VK_LOG_INFO("GAME-PHYS disabled by scene settings");
        return;
    }
    VK_LOG_INFO("GAME-PHYS world built — %d body(ies) (ground=%d triggers=%d "
                "materials=%d) from %d rigid entities",
                m_binding.bodies, m_binding.ground ? 1 : 0, m_binding.triggers,
                m_binding.materials, m_binding.rigidEntities);
}

void RuntimeApp::preloadScripts() {
    if (!m_scripts) return;
    m_scripts->clear();
    int failed = 0;
    std::size_t count = 0;
    activeScene().world().each<ecs::ScriptComponent>(
        [&](ecs::Entity, ecs::ScriptComponent& s) {
            if (!s.enabled || s.path.empty()) return;
            ++count;
            vks::ScriptAsset* a = m_scripts->get(s.path);
            if (a && !a->lastError().ok()) ++failed;
        });
    if (count)
        VK_LOG_INFO("GAME-SCRIPT precompiled %zu reference(s), %d with errors",
                    count, failed);
}

// ---------------------------------------------------------------- 每帧

void RuntimeApp::onUpdate(float dt) {
    // 时间缩放的权威在服务面（状态可以改它，M3 的子弹时间就靠这个）
    m_loop.config().timeScale = m_svc.timeScale;

    // 失焦降频：GLFW 没有"焦点变化"以外更便宜的做法，每帧读一次窗口属性
    // 就是最省的（`glfwGetWindowAttrib` 是纯状态查询，不触发任何系统调用）。
    // ⚠ GLFW 里**没有** glfwGetWindowFocus —— 焦点就是窗口属性 GLFW_FOCUSED。
    const bool focused =
        glfwGetWindowAttrib(window().handle(), GLFW_FOCUSED) != 0;

    m_renderedWorldThisFrame = false;

    // 输入采样每帧一次（在固定步之前）。鼠标位移在这里被消费 ——
    // 之后任何地方再读 Input::mouseDelta 都会拿到 0。
    m_input.poll();

    LoopCallbacks cb;
    // ① 事件泵：Application::run 里已经 pollEvents 过，这里不重复。
    //
    // ⚠ **UI tick 放在这里**（不是 fixedUpdate 里）：这是"在状态栈之外"
    //   最直接的位置 —— pollInput 每帧无条件调一次，不受 blocksUpdate
    //   影响，也不受 timeScale 影响（将来做子弹时间 UI 不该冻住）。
    //   同时它在 fixedUpdate **之前**，所以 `route()` 天然早于
    //   PlayerController::update() —— 顺序是路线图 U6 的硬要求。
    cb.pollInput = [this] { tickUi(1.0f / 60.0f); };
    // ② 固定步：状态栈按 blocksUpdate 决定谁收到
    cb.fixedUpdate = [this](float step) {
        m_svc.frame = time().frameCount();
        m_stateStack.update(m_svc, step);
    };
    // ③ 变量更新
    cb.variableUpdate = [this](float vdt) {
        if (m_svc.updateVariable) m_svc.updateVariable(vdt);
    };
    // ④ 渲染
    cb.render = [this] {
        m_stateStack.render(m_svc);
        // 本帧没有任何状态画世界（例如不透明的 MainMenu 在栈顶）→ 仍然
        // 提交一帧空场景：ImGui 的帧必须闭环，交换链也要推进，
        // 否则窗口会一直停在上一帧（甚至第一帧都没呈现过）。
        if (!m_renderedWorldThisFrame && m_menuScene)
            renderer().drawFrame(*m_menuScene);
    };

    m_loop.tick(dt, focused, cb);

    // ---- 音频（G14-5）----
    //
    // ⚠ 放在 `tick` **之后**：本帧的相机姿态 / 角色位置 / 暂停状态都已经
    //   定下来了，listener 晚一帧就会"声音追着镜头跑"。
    // ⚠ 用 `dt` 而不是固定步长：`dt` 只参与**包络插值**（ducking / 音量平滑），
    //   播放位置由声卡 block 推进（§3.6）。所以 `timeScale=0` 时音乐照常。
    syncAudio(dt, focused);

    // ---- UI 动作 → 状态迁移（U6/U8）----
    //
    // ⚠ 迁移**只在这里**做：本帧的 m_stateStack.update 已经跑完，此刻改栈
    //   不会让遍历中的 vector 失效（GameStateStack.h 的第 2 条约定）。
    //   tickUi / Paused::onUpdate 都只置意图位。
    if (m_svc.popRequested) {
        m_svc.popRequested = false;
        m_stateStack.pop(m_svc);
    } else if (m_pauseRequested) {
        m_pauseRequested = false;
        // 只有 Playing 能暂停：Boot / Loading / MainMenu / GameOver 压一个
        // 暂停菜单没有意义（而且 MainMenu 本来就不透明，菜单会画在黑底上）。
        if (std::strcmp(m_stateStack.topName(), "Playing") == 0)
            m_stateStack.push(makeBuiltinState(StateId::Paused), m_svc);
    }

    // 栈空 = 没有任何阶段了 → 正常退出（不黑窗卡死）
    if (m_stateStack.empty() || m_svc.quitRequested) {
        VK_LOG_INFO("GAME-STATE stack empty / quit requested → shutting down");
        window().invalidate();
    }
}

// ---------------------------------------------------------------- 音频

void RuntimeApp::syncAudio(float dt, bool focused) {
    // 设备不可用 / 关掉了：仍然要 `update`（失败重试计时、AUDIO-STATS 计数），
    // 但不去枚举声源 —— 那是白跑一遍 ECS。
    if (!m_audio.running()) {
        m_audio.update(dt);
        return;
    }

    // ---- 失焦静音（只有用户显式打开才生效，§3.6 表第三行）----
    if (m_cfg.audio.muteOnFocusLost)
        m_audio.setMuted(m_cfg.audio.mute || !focused);

    // ---- listener = 相机 ----
    //
    // ⚠ 引擎是 **Z-up**，所以世界 up 轴就是 +Z（不是 +Y）。
    //   写错了 pan 会整体歪 90°，而且"左/右"听起来只是"薄一点" ——
    //   这种错只能靠断言（spatial_left_source_left_louder）抓。
    const scene::Camera& cam = activeScene().camera();
    glm::vec3 lp = cam.position();
    // 允许场景里挂一个 AudioListenerComponent 覆盖**听者位置**
    // （例如做耳朵在角色头上、而相机在身后 3 m 的第三人称混音）。
    // ⚠ 只取位置，不取朝向：朝向的唯一权威是 CameraSystem（它混合了
    //   多个来源），实体旋转不一定等于相机朝向，取错了是静默的整体歪。
    bool listenerFromEntity = false;
    activeScene().world().each<ecs::AudioListenerComponent>(
        [&](ecs::Entity e, const ecs::AudioListenerComponent& c) {
            if (listenerFromEntity || !c.enabled) return;
            lp = glm::vec3(activeScene().worldMatrix(e)[3]);
            listenerFromEntity = true;
        });
    {
        const glm::vec3 f = cam.forwardAxis();
        const glm::vec3 up(0.0f, 0.0f, 1.0f);
        const float a[3] = {lp.x, lp.y, lp.z};
        const float b[3] = {f.x, f.y, f.z};
        const float u[3] = {up.x, up.y, up.z};
        m_audio.setListener(a, b, u);
    }
    if (listenerFromEntity && !m_audioListenerLogged) {
        // 只打一次就够：每帧打会把日志淹掉（R15）
        m_audioListenerLogged = true;
        VK_LOG_INFO("AUDIO-LISTENER source=entity pos=%.2f,%.2f,%.2f",
                    static_cast<double>(lp.x), static_cast<double>(lp.y),
                    static_cast<double>(lp.z));
    }

    // ---- 声源 → SourceUpdate ----
    //
    // ⚠ 暂停语义（§3.6）：**BGM 继续，SFX/Voice 淡出**。
    //   做法不是"停掉整个音频系统"，而是把 SFX/Voice 报成 `playing=false`
    //   → `applySources` 走它本来就会走的 20 ms 淡出（不新增第三条停止路径）。
    const bool paused = pausedNow();

    m_audioSrcs.clear();
    activeScene().world().each<ecs::AudioSourceComponent>(
        [&](ecs::Entity e, const ecs::AudioSourceComponent& a) {
            if (a.clip.empty()) return;

            audio::SourceUpdate u;
            // ⚠ id 必须带代际：实体槽位会复用，只用 `id` 的话"上一具尸体
            //   的音效"会被认成"新实体的音效"，一直播下去（ABA）。
            u.id = (static_cast<std::uint64_t>(e.generation) << 32) | e.id;
            u.clip = a.clip;
            u.bus = (a.bus >= 1 && a.bus <= 3) ? a.bus
                                               : static_cast<int>(audio::BusId::SFX);
            u.loop = a.loop;
            u.spatial = a.spatial;
            u.playing = a.playOnAwake;
            if (paused && u.bus != static_cast<int>(audio::BusId::Music))
                u.playing = false;
            u.volume = a.volume;
            u.pitch = a.pitch;
            u.minDist = a.minDist;
            u.maxDist = a.maxDist;
            u.rolloff = a.rolloff;
            if (a.spatial) {
                const glm::vec3 wp = glm::vec3(activeScene().worldMatrix(e)[3]);
                u.pos[0] = wp.x;
                u.pos[1] = wp.y;
                u.pos[2] = wp.z;
            }
            m_audioSrcs.push_back(u);
        });

    m_audio.applySources(m_audioSrcs.data(), m_audioSrcs.size());
    m_audio.update(dt);
}

// ---------------------------------------------------------------- UI tick

bool RuntimeApp::pausedNow() const {
    // ⚠ 暂停的权威是**状态栈的栈顶名**，不是某个 bool 成员。
    //   多一个 bool 就多一份可能与栈不一致的状态（最典型的症状是
    //   "菜单关掉了但角色还是不能动"）。
    return std::strcmp(m_stateStack.topName(), "Paused") == 0;
}

void RuntimeApp::tickUi(float dt) {
    (void)dt;

    // 输出尺寸与 PostProcess 画 UI 时**同源**（见 Renderer::uiOutputExtent）：
    // 拿窗口尺寸或渲染分辨率算命中框都会整体偏一小段比例 —— 屏幕上看着
    // 光标就在按钮上，却点不中。
    const VkExtent2D out = renderer().uiOutputExtent();
    const uint32_t outW = out.width;
    const uint32_t outH = out.height;

    const bool paused = pausedNow();

    // ---- ① 光标 / 输入上下文：**只在状态变化时**动 ----
    //
    // 每帧都 pushContext 会让上下文栈无限增长（然后 Gameplay 永远回不来）。
    const int state = paused ? 1 : 0;
    if (state != m_lastUiState) {
        m_lastUiState = state;
        if (paused) {
            // 压 Menu：Gameplay 整组**结构性地**失效 —— 于是"暂停时按 W
            // 角色不动"不是靠某个 if，而是靠只有栈顶上下文被求值。
            m_input.pushContext("Menu");
            // 菜单要用鼠标点，光标得看得见，并且锁在窗口内（GLFW 的
            // DISABLED 之外没有单独的 clip 模式；可见是菜单的硬需求）。
            window().setCursorVisible(true);
            m_focus.clearFocus();
        } else {
            if (m_input.depth() > 1) m_input.popContext();
            window().setCursorVisible(false);
            m_focus.endFrame();
        }
        VK_LOG_INFO("UI2-PAUSE open=%d context=%s cursor=%d", state,
                    m_input.topContext().c_str(),
                    window().cursorVisible() ? 1 : 0);
    }

    // ---- ② 暂停开关（状态迁移的**意图**，真正改栈在 onUpdate 末尾）----
    if (paused) {
        // 暂停时 Gameplay 整组失效，所以 ESC 走 Menu 的 MenuBack。
        // ⚠ 用 wasPressed（边沿）：按住 ESC 会每帧开关一次。
        if (m_input.wasPressed("MenuBack")) m_svc.popRequested = true;
    } else if (m_input.wasPressed("Pause")) {
        m_pauseRequested = true;
    }

    // ---- ③ 没有菜单 → UI 不吃任何输入 ----
    if (!paused) {
        m_svc.uiAction.clear();
        // HUD 一直有内容（血条 + 准星），所以这条恒为 true。
        // `ui.enabled=0` 的守卫不在这里 —— 在 Renderer：通道没建就根本
        // 不会走到 buildOverlay（逐位不变的保证在那儿）。
        ui2::setGameUiActive(true);
        return;
    }

    m_focus.beginFrame(m_pause.focusItems(outW, outH));

    // 打开菜单就**立起第一项**（"继续"）。不这么做的话，菜单刚出现时
    // 没有任何高亮，玩家必须按一下方向键才看得到选中的是哪个 —— 而且
    // `UI2-FOCUS` 要等到那一下才打，自动化根本分不清"菜单还没开"和
    // "开了但没焦点"。（`focusFirst` 内部只在**变化时**打锚点。）
    if (!m_focus.hasFocus()) m_focus.focusFirst();

    // 鼠标：窗口坐标 → 输出像素（高 DPI 下 framebuffer != window）
    double cx = 0.0, cy = 0.0;
    core::Input::cursorPos(&cx, &cy);
    int winW = 1, winH = 1;
    window().windowSize(&winW, &winH);
    const float px = static_cast<float>(
        cx * (winW > 0 ? static_cast<double>(outW) / winW : 1.0));
    const float py = static_cast<float>(
        cy * (winH > 0 ? static_cast<double>(outH) / winH : 1.0));

    // 悬停跟手：鼠标停在某个按钮上就聚焦它。不这么做的话，玩家"看着
    // 光标在按钮上"却要点两次（第一次只是把焦点拉过来）。
    if (m_focus.hitIndex(px, py) >= 0) m_focus.hoverAt(px, py);

    ui2::UiNav nav;
    nav.moveY = (m_input.wasPressed("MenuDown") ? 1 : 0) -
                (m_input.wasPressed("MenuUp") ? 1 : 0);
    nav.moveX = (m_input.wasPressed("MenuRight") ? 1 : 0) -
                (m_input.wasPressed("MenuLeft") ? 1 : 0);
    nav.tab = m_input.wasPressed("MenuTab");
    nav.activate = m_input.wasPressed("MenuConfirm");

    // 左键点中按钮 = 激活。走**同一条 route**，不另开一条路径 ——
    // 两条路径迟早会在"点中和键盘选中不一样"这种地方分叉。
    if (!nav.activate && core::Input::mousePressed(GLFW_MOUSE_BUTTON_LEFT)) {
        const int hit = m_focus.hitIndex(px, py);
        if (hit >= 0) {
            m_focus.setFocus(m_focus.items()[static_cast<std::size_t>(hit)].id);
            nav.activate = true;
        }
    }

    std::string act;
    const bool consumed = m_focus.route(nav, &act);
    (void)consumed;
    if (!act.empty()) {
        m_svc.uiAction = act;
        VK_LOG_INFO("UI2-ACTION id=%s consumed=1", act.c_str());
    } else {
        m_svc.uiAction.clear();
    }

    // 暂停菜单这一帧确实有东西要画
    ui2::setGameUiActive(true);
}

// ---------------------------------------------------------------- 仿真

void RuntimeApp::simulate(float dt) {
    ++m_simFrames;

    // HUD 数据跟着**游戏时间**走（不是真实帧时长）：暂停时 Paused 截断
    // 下层 update → simulate 不跑 → 伤害数字冻在屏幕上，那是对的
    // （画面定住了，数字还在往上飘会很怪）。
    m_hudModel.tick(dt);

    runScripts(dt);

    // 顺序（与编辑器 Play 一致，见 ship_roadmap §4 G10）：
    //   脚本 → 玩家 → 物理 → 接触事件
    // 玩家必须先于物理：它写入的变换/代理速度是这一步接触求解的输入。
    if (m_player) m_player->update(activeScene(), m_binding.world.get(), dt);

    // ---- 骨骼动画（A0-3）----
    // 排在物理之前，与编辑器 Play 同位置：将来 A4 的根运动会把位移写进
    // TransformComponent，物理必须读到它这一帧的最终值。
    m_animator.tick(activeScene(), dt);

    if (m_binding.world) {
        // 顺序与编辑器 Play 一致：脚本 → 物理（M2 会在中间插入"玩家"，
        // 在物理之后插入"碰撞"）。
        physics::syncKinematicFromScene(activeScene(), *m_binding.world,
                                        m_binding.entityToBody, dt);
        m_binding.world->step(dt);
        physics::writeBackToScene(activeScene(), *m_binding.world,
                                  m_binding.entityToBody);
        pumpContactEvents();
    }

    if (m_playerDumpEvery > 0 &&
        (m_simFrames % static_cast<std::uint64_t>(m_playerDumpEvery)) == 0u &&
        m_player) {
        const glm::vec3 p = m_player->characterCenter();
        VK_LOG_INFO("GAME-PLAYER-POS f=%llu pos=(%.5f,%.5f,%.5f) ground=%d "
                    "arm=%.4f",
                    static_cast<unsigned long long>(m_simFrames), p.x, p.y, p.z,
                    m_player->onGround() ? 1 : 0, m_player->springArmDistance());
    }

    if (m_physicsDumpEvery > 0 &&
        (m_simFrames % static_cast<std::uint64_t>(m_physicsDumpEvery)) == 0u) {
        dumpPhysicsBodies("PHYS-BODY");
    }
}

int RuntimeApp::runScripts(float dt) {
    if (!m_scriptRuntime) return 0;

    // ⚠ `kSelfTimed`：仿真态由**实例自己**累加本地时间（修 P3）。
    //   以前是 `m_simFrames * dt` 再乘 timeScale —— 倍速从 1.0 改到 0.5 的
    //   瞬间，脚本看到的 `time` 会倒退一半。
    m_scriptRuntime->tick(activeScene(), vks::ScriptRuntime::kSelfTimed, dt,
                          time().frameCount());
    return static_cast<int>(m_scriptRuntime->lastStats().executed);
}

// 宿主能力只填**一次**（S0 之前是每帧每个实体重建六个 std::function ——
// 那是纯浪费，而且把"结构"藏进了循环里）。
void RuntimeApp::installScriptServices() {
    vks::ScriptServices svc;

    // ---- HUD（U8）----
    // 三个窄动作接到 m_hudModel 上（engine 不认识 runtime，所以走
    // std::function 而不是指针）。`damage` 顺带在实体头顶弹数字。
    svc.setHealth = [this](float v) { m_hudModel.setHealth(v); };
    svc.damage = [this](float v) {
        m_hudModel.setHealth(m_hudModel.health - v);
    };
    svc.popup = [this](float x, float y, float z, float amount) {
        m_hudModel.addPopup(glm::vec3(x, y, z), amount);
    };

    // ---- 音频（G14-8）----
    // `playSound` 是一次性 2D 音效（不跟实体位置走）：脚本里不知道该实体
    // 此刻在哪，跟过去反而要每次查 Transform。要 3D 就用组件。
    svc.playSound = [this](const std::string& clip) {
        m_audio.playOneShot(clip, audio::BusId::SFX, nullptr, 1.0f);
    };
    svc.stopAllSounds = [this]() { m_audio.stopAll(); };
    svc.setBusVolume = [this](const std::string& bus, float v) {
        const int idx = audio::busIndexFromName(bus.c_str());
        if (idx < 0) {
            // ⚠ 认不出就留痕：静默忽略会让"我把音量调到 0.3 了怎么没变"
            //   变成一次排查（与 busIndexFromName 返回 -1 的设计同源）。
            VK_LOG_WARN("GAME-SCRIPT setBusVolume: 未知总线 '%s'"
                        "（可用：master|music|sfx|voice）",
                        bus.c_str());
            return;
        }
        m_audio.setBusGain(static_cast<audio::BusId>(idx), v);
    };

    // ---- 玩家 / 输入（S3）----
    // 玩家是"被 PlayerController 接管的那个实体" —— 只有 runtime 知道，
    // engine 不知道（这正是把它做成注入的理由）。
    svc.act.player = [this]() {
        return m_player ? m_player->entity() : ecs::Entity{};
    };
    // ⚠ 传的是**动作名**（InputMap 的 "Jump"），不是键码。
    svc.act.pressed = [this](const std::string& a) { return m_input.wasPressed(a); };
    svc.act.held = [this](const std::string& a) { return m_input.isHeld(a); };
    svc.act.released = [this](const std::string& a) { return m_input.wasReleased(a); };
    svc.act.axis = [this](const std::string& a) { return m_input.axis(a); };

    m_scriptRuntime->setServices(std::move(svc));

    m_scriptRuntime->setPrintSink(
        [](const vks::ScriptInstance& inst, const std::string& line) {
            VK_LOG_INFO("GAME-SCRIPT print %s: %s", inst.path.c_str(),
                        line.c_str());
        });
    // ⚠ 措辞与 S0 之前逐字相同（`GAME-SCRIPT not runnable` /
    //   `GAME-SCRIPT runtime error`）：verify 脚本 grep 的就是这两句。
    m_scriptRuntime->setErrorSink(
        [](const vks::ScriptInstance& inst, const std::string& msg) {
            const bool compileErr =
                inst.errorKind == vks::ScriptInstance::ErrorKind::Compile;
            VK_LOG_WARN("GAME-SCRIPT %s (%s): %s",
                        compileErr ? "not runnable" : "runtime error",
                        inst.path.c_str(), msg.c_str());
        });
}

// ---------------------------------------------------------------- 接触事件

void RuntimeApp::pumpContactEvents() {
    if (!m_binding.world) return;
    m_events.clear();
    m_contacts.collect(*m_binding.world, m_events);
    if (m_events.empty()) return;

    // ★ S3：翻译成 `vks::ScriptContact` 投递给脚本事例。
    //   `ContactEvent` 用的是 EntityId（没有 generation），这里必须用
    //   `World::handle()` 补成完整句柄 —— 否则"旧 id 复用"的碰撞会派发到
    //   刚被删掉的那个实体的继任者身上。
    {
        ecs::World& w = activeScene().world();
        for (const physics::ContactEvent& e : m_events) {
            vks::ScriptContact sc;
            switch (e.phase) {
                case physics::ContactPhase::Enter:
                    sc.kind = vks::ScriptContact::Kind::Enter;
                    break;
                case physics::ContactPhase::Stay:
                    sc.kind = vks::ScriptContact::Kind::Stay;
                    break;
                case physics::ContactPhase::Exit:
                    sc.kind = vks::ScriptContact::Kind::Exit;
                    break;
            }
            sc.a = w.handle(e.a);
            sc.b = w.handle(e.b);
            sc.trigger = e.trigger;
            m_scriptRuntime->postContact(sc);
        }
    }

    // 事件本身在 G08 里已经派发到 ContactTracker；这里只负责"让脚本 /
    // 日志看得见"（GAME-CONTACT 行默认只在 trace 下打，正常玩不刷屏）。
    if (m_contactTrace) {
        for (const physics::ContactEvent& e : m_events) {
            VK_LOG_INFO("GAME-CONTACT %s a=%u b=%u trigger=%d stay=%d",
                        physics::contactPhaseName(e.phase), e.a, e.b,
                        e.trigger ? 1 : 0, e.stay);
        }
    }
}

// ---------------------------------------------------------------- 转储

// 格式与编辑器的 `MYVK_PHYS_DUMP` **逐字一致** ——
// tools/verify_runtime_determinism.py 就是拿两边的同一帧逐行比数字的。
void RuntimeApp::dumpPhysicsBodies(const char* tag) {
    if (!m_binding.world) return;
    ecs::World& w = activeScene().world();
    for (const auto& kv : m_binding.entityToBody) {
        const physics::RigidBody* b = m_binding.world->body(kv.second);
        if (!b || !b->isDynamic()) continue;
        const ecs::Entity e = w.handle(kv.first);
        const std::string* nm = e.valid() ? w.name(e) : nullptr;
        const auto* tc = e.valid() ? w.get<ecs::TransformComponent>(e) : nullptr;
        const glm::vec3 p = tc ? tc->position : b->position;
        VK_LOG_INFO("%s f=%llu name='%s' pos=(%.5f,%.5f,%.5f) "
                    "vel=(%.5f,%.5f,%.5f) awake=%d",
                    tag, static_cast<unsigned long long>(m_simFrames),
                    nm ? nm->c_str() : "?", static_cast<double>(p.x),
                    static_cast<double>(p.y), static_cast<double>(p.z),
                    static_cast<double>(b->linearVelocity.x),
                    static_cast<double>(b->linearVelocity.y),
                    static_cast<double>(b->linearVelocity.z), b->awake ? 1 : 0);
    }
}

// ---------------------------------------------------------------- 收尾

void RuntimeApp::onShutdown() {
    // ---- S6 / G5：退出时把**运行态**场景写一份 ----
    //
    // `--dump-scene` 是"读进来立刻再写一份"，它证明的是**读盘的规范化形状**；
    // 而"世界黑板进不进存档"必须**跑完之后**再写 —— 脚本只有在 Play 里
    // 才写 `setWorldAttr`，所以 dump 出来的永远是空的。
    //
    // 与 `--dump-scene` 同一个写端（`scene::saveScene`），所以往返形状一致：
    // 拿它当输入再起一次 Game.exe 就是一次真正的 roundtrip。
    if (const char* out = std::getenv("MYVK_RUNTIME_SAVE_SCENE")) {
        if (out[0] != '\0') {
            const scene::SceneIoResult w =
                scene::saveScene(activeScene(), std::string(out));
            if (w.ok) {
                VK_LOG_INFO("GAME-SCENE-SAVED path=%s entities=%d vars=%zu",
                            out, w.entities, activeScene().scriptVars().size());
            } else {
                VK_LOG_WARN("GAME-SCENE-SAVE-FAIL path=%s %s", out,
                            w.error.c_str());
            }
        }
    }

    // ⚠ 音频要**先**关：它有一条自己的线程，必须在窗口 / 日志之前停掉，
    //   否则设备回调可能在我们销毁 Mixer 之后还被调一次。
    {
        const audio::MixStats as = m_audio.stats();
        VK_LOG_INFO("AUDIO-SUMMARY backend=%s device=%s running=%d voices=%d "
                    "sr=%u ch=%u mem=%.0f",
                    m_audio.backendName(), m_audio.deviceName().c_str(),
                    m_audio.running() ? 1 : 0, as.voices,
                    m_audio.deviceFormat().sampleRate,
                    m_audio.deviceFormat().channels,
                    static_cast<double>(m_audio.cacheResidentBytes() +
                                        m_audio.streamResidentBytes()));
    }
    m_audio.shutdown();
    m_audioSrcs.clear();

    m_loop.logNow();
    VK_LOG_INFO("GAME-LOOP-SUMMARY ticks=%llu fixed=%llu var=%llu render=%llu "
                "sim=%.3fs dropped=%llu",
                static_cast<unsigned long long>(m_loop.stats().ticks),
                static_cast<unsigned long long>(m_loop.stats().fixedSteps),
                static_cast<unsigned long long>(m_loop.stats().variableSteps),
                static_cast<unsigned long long>(m_loop.stats().renders),
                static_cast<double>(m_loop.stats().simTime),
                static_cast<unsigned long long>(m_loop.stats().droppedSteps));
    VK_LOG_INFO("GAME-STATE-SEQ %s", m_stateStack.topSequenceString().c_str());
    if (m_player) {
        VK_LOG_INFO("GAME-PLAYER-SUMMARY travelled=%.3fm arm=%.3fm contacts=%llu",
                    static_cast<double>(m_player->travelledDistance()),
                    static_cast<double>(m_player->springArmDistance()),
                    static_cast<unsigned long long>(m_contacts.totalEvents()));
    } else {
        VK_LOG_INFO("GAME-PLAYER-SUMMARY none");
    }
    if (m_binding.world) {
        const physics::PhysicsStats& st = m_binding.world->stats();
        VK_LOG_INFO("GAME-PHYS-SUMMARY steps=%llu sim=%.3fs bodies=%d dyn=%d "
                    "manifolds=%d",
                    static_cast<unsigned long long>(st.steps),
                    static_cast<double>(st.simTime), st.bodies, st.dynamic,
                    st.manifolds);
    }
}

} // namespace runtime
