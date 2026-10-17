#include "RuntimeApp.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "physics/PhysicsWorld.h"
#include "scene/CameraSystem.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "script/ScriptEngine.h"

#include <GLFW/glfw3.h>

#include <cstdlib>
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

    // 全屏在这里切（窗口已经存在；setFullscreen 会记住窗口模式下的几何）
    if (m_cfg.fullscreen) window().setFullscreen(true);

    m_scripts = std::make_unique<vks::ScriptEngine>();
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
    cb.pollInput = nullptr;
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

    // 栈空 = 没有任何阶段了 → 正常退出（不黑窗卡死）
    if (m_stateStack.empty() || m_svc.quitRequested) {
        VK_LOG_INFO("GAME-STATE stack empty / quit requested → shutting down");
        window().invalidate();
    }
}

// ---------------------------------------------------------------- 仿真

void RuntimeApp::simulate(float dt) {
    ++m_simFrames;
    runScripts(dt);

    // 顺序（与编辑器 Play 一致，见 ship_roadmap §4 G10）：
    //   脚本 → 玩家 → 物理 → 接触事件
    // 玩家必须先于物理：它写入的变换/代理速度是这一步接触求解的输入。
    if (m_player) m_player->update(activeScene(), m_binding.world.get(), dt);

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
    if (!m_scripts) return 0;

    ecs::World& w = activeScene().world();
    const std::uint64_t frame = time().frameCount();
    const double t = static_cast<double>(m_simFrames) * static_cast<double>(dt);
    const double dtd = static_cast<double>(dt);

    int executed = 0;
    w.each<ecs::ScriptComponent>([&](ecs::Entity e, ecs::ScriptComponent& scomp) {
        if (!scomp.enabled || scomp.path.empty()) return;

        vks::ScriptAsset* asset = m_scripts->get(scomp.path);
        if (!asset) return;
        vks::ScriptProgram* prog = asset->program();
        if (!prog || !prog->valid()) {
            const std::string msg = asset->lastError().toString();
            auto it = m_scriptErrors.find(e.id);
            if (it == m_scriptErrors.end() || it->second != msg) {
                m_scriptErrors[e.id] = msg;
                VK_LOG_WARN("GAME-SCRIPT not runnable (%s): %s",
                            scomp.path.c_str(), msg.c_str());
            }
            return;
        }

        vks::ScriptHost host;
        host.scene = &activeScene();
        host.entity = e;

        vks::ScriptError err;
        const double ts = static_cast<double>(scomp.timeScale);
        if (!prog->run(host, t * ts, dtd * ts, frame, err)) {
            const std::string msg = err.toString();
            auto it = m_scriptErrors.find(e.id);
            if (it == m_scriptErrors.end() || it->second != msg) {
                m_scriptErrors[e.id] = msg;
                VK_LOG_WARN("GAME-SCRIPT runtime error (%s): %s",
                            scomp.path.c_str(), msg.c_str());
            }
            return;
        }
        m_scriptErrors.erase(e.id);
        ++executed;

        for (const std::string& line : prog->prints())
            VK_LOG_INFO("GAME-SCRIPT print %s: %s", scomp.path.c_str(),
                        line.c_str());
    });
    return executed;
}

// ---------------------------------------------------------------- 接触事件

void RuntimeApp::pumpContactEvents() {
    if (!m_binding.world) return;
    m_events.clear();
    m_contacts.collect(*m_binding.world, m_events);
    if (m_events.empty()) return;

    // 事件本身在 G08 里已经派发到 ContactTracker；这里只负责"让脚本 /
    // 日志看得见"。具体语言绑定在 G27（.vks 的 onCollisionEnter），
    // 现在先把它们打成 GAME-CONTACT 行（默认只在 trace 下打，正常玩不刷屏）。
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
