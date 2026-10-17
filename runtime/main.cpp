// ============================================================
// runtime/main.cpp —— 运行时（出货程序）入口
// ============================================================
// 与 editor/main.cpp **完全平行**：同一个 core::Application 主循环，
// 换一个子类。两个可执行文件共用 engine/，互相之间没有依赖。
//
// 这里只做四件事（顺序有意义）：
//   1. 读命令行 / 环境变量 / assets/game.json  → RuntimeConfig
//   2. 无窗口自检（--selftest / MYVK_RUNTIME_SELFTEST）→ 直接 return
//      ⚠ 必须在**创建窗口之前**：自检不该占用 GPU、也不该在无显示环境失败
//   3. 打开日志（落用户目录，见 runtime/RuntimeLog.h）
//   4. 起 RuntimeApp
//
// ------------------------------------------------------------
// 环境变量（全部带 MYVK_RUNTIME_ 前缀，除 MYVK_RUNTIME 本身）
// ------------------------------------------------------------
//   MYVK_RUNTIME=1                     每 60 帧一行 GAME-LOOP（verbose）
//   MYVK_RUNTIME_SCENE=<path>          起始场景
//   MYVK_RUNTIME_WIDTH / _HEIGHT       窗口尺寸
//   MYVK_RUNTIME_FULLSCREEN=0|1
//   MYVK_RUNTIME_MAXFPS / _UNFOCUSED_FPS
//   MYVK_RUNTIME_TIMESCALE=<f>
//   MYVK_RUNTIME_START_STATE=boot|mainmenu|playing
//   MYVK_RUNTIME_TRACE_STATES=1        每次迁移打一行 GAME-STATE
//   MYVK_RUNTIME_SELFTEST=states|determinism|input|character|contact|camera|player
//   MYVK_RUNTIME_GAME_JSON=<path>      覆盖 game.json 的位置
//   MYVK_FIXED_DT=<秒>                 把每帧 dt 钉成常量（确定性对照用）
//   MYVK_PHYS_DUMP=<N>                 每 N 个仿真帧一行 PHYS-BODY
//
// ⚠ `MYVK_*` 一律要过 tools/check_feature_surface.py：`MYVK_RUNTIME_*`
//   按前缀放行，`MYVK_RUNTIME` 与 `MYVK_FIXED_DT` 在 ALLOW_EXACT 里。
// ============================================================

#include "RuntimeApp.h"
#include "RuntimeConfig.h"
#include "RuntimeLog.h"

#include "core/Logger.h"

#include "GameState.h"
#include "GameStateStack.h"
#include "GameLoop.h"

#include "physics/CharacterController.h"
#include "physics/ContactEvents.h"
#include "physics/PhysicsWorld.h"

#include "core/InputMap.h"
#include "scene/CameraSystem.h"

#include "PlayerController.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- 自检工具

int g_selftestPass = 0;
int g_selftestFail = 0;

void check(const char* name, bool ok, const char* detail) {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (ok) ++g_selftestPass;
    else ++g_selftestFail;
}

// FNV-1a over the raw bytes of a float — 用来把"一串位置"压成一个可比较的数。
// 逐位比较必须是**位**比较，不能是"差值小于 eps"。
std::uint64_t hashFloat(std::uint64_t h, float v) {
    unsigned char buf[sizeof(float)];
    std::memcpy(buf, &v, sizeof(float));
    for (unsigned char b : buf) {
        h ^= static_cast<std::uint64_t>(b);
        h *= 1099511628211ull;
    }
    return h;
}

// ---------------------------------------------------------------- 自检：状态栈
//
// 断言两件事，都是 GameState.h 里那两条规矩的直接推论：
//   A 迁移序列与预期字符串逐字一致
//   B Paused 压住 Playing 时：Playing **不 update** 但**仍在 render**
int runStateSelftest() {
    std::printf("== selftest: states ==\n");

    // ---- A：迁移序列 ----
    {
        runtime::GameServices svc;
        runtime::GameStateStack stack;
        stack.setTrace(true);
        stack.push(runtime::makeBuiltinState(runtime::StateId::Boot), svc);
        stack.replace(runtime::makeBuiltinState(runtime::StateId::Loading), svc);
        stack.replace(runtime::makeBuiltinState(runtime::StateId::MainMenu), svc);
        stack.push(runtime::makeBuiltinState(runtime::StateId::Playing), svc);
        stack.push(runtime::makeBuiltinState(runtime::StateId::Paused), svc);
        stack.pop(svc);   // 暂停 → 恢复
        stack.pop(svc);   // 退出游戏 → 回主菜单

        const std::string seq = stack.topSequenceString();
        const std::string want =
            "Boot>Loading>MainMenu>Playing>Paused>Playing>MainMenu";
        char detail[512];
        std::snprintf(detail, sizeof(detail), "seq=%s", seq.c_str());
        check("A 迁移序列 Boot>Loading>MainMenu>Playing>Paused>Playing>"
              "MainMenu",
              seq == want, seq == want ? "" : detail);

        char out[512];
        std::snprintf(out, sizeof(out), "seq=%s expect=%s result=%s",
                      seq.c_str(), want.c_str(),
                      seq == want ? "PASS" : "FAIL");
        std::printf("GAME-STATE-TEST %s\n", out);
    }

    // ---- B：Paused 的阻断语义 ----
    {
        unsigned simulateCalls = 0;
        unsigned worldRenders = 0;
        runtime::GameServices svc;
        svc.simulate = [&](float) { ++simulateCalls; };
        svc.renderWorld = [&] { ++worldRenders; };

        runtime::GameStateStack stack;
        auto play = std::make_unique<runtime::BuiltinState>(
            runtime::StateId::Playing);
        auto* playRaw = play.get();
        stack.push(std::move(play), svc);

        auto paused = std::make_unique<runtime::BuiltinState>(
            runtime::StateId::Paused);
        auto* pausedRaw = paused.get();
        stack.push(std::move(paused), svc);

        for (int i = 0; i < 30; ++i) {
            stack.update(svc, 1.0f / 60.0f);
            stack.render(svc);
        }

        const bool okUpdates = (playRaw->updates == 0) && (pausedRaw->updates == 30);
        const bool okRenders = (playRaw->renders == 30) && (pausedRaw->renders == 30);
        const bool okSide = (simulateCalls == 0) && (worldRenders == 30);

        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "playingUpdates=%u playingRenders=%u pausedUpdates=%u "
                      "pausedRenders=%u worldRenders=%u simulate=%u",
                      playRaw->updates, playRaw->renders, pausedRaw->updates,
                      pausedRaw->renders, worldRenders, simulateCalls);
        check("B1 Paused 压住 Playing → Playing 的 update 被截断", okUpdates,
              detail);
        check("B2 Paused 不截断 render → 画面仍在出帧", okRenders, detail);
        check("B3 玩法仿真停住、世界仍被画 30 次", okSide, detail);

        std::printf("GAME-STATE-TEST paused %s result=%s\n", detail,
                    (okUpdates && okRenders && okSide) ? "PASS" : "FAIL");
    }

    // ---- C：栈空 → 宿主该退出 ----
    {
        runtime::GameServices svc;
        runtime::GameStateStack stack;
        stack.push(runtime::makeBuiltinState(runtime::StateId::MainMenu), svc);
        stack.pop(svc);
        check("C 栈空时 empty() 为真（宿主据此退出，不黑窗卡死）",
              stack.empty(), "");
    }

    std::printf("GAME-STATE-TOTAL pass=%d fail=%d\n", g_selftestPass,
                g_selftestFail);
    return g_selftestFail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------- 自检：定步长
//
// 三条，全部是"确定性能不能成立"的直接前提：
//   A 同一个 dt 序列喂给 GameLoop 的累加器 → 固定步数**恰好**等于预期
//   B 走累加器的结果与直接 stepFixed 的结果**逐位相同**
//   C timeScale = 0 → 一步都不推进、位置零漂移
int runDeterminismSelftest() {
    std::printf("== selftest: determinism ==\n");

    const float kStep = 1.0f / 60.0f;
    const int kSteps = 300;

    // ---- 用一个自由下落的盒子当被测物（没有地面 → 没有接触 → 纯积分，
    //      这样两条驱动路径的差异只可能来自"步进调度"，不可能来自求解器）----
    auto makeWorld = [&]() {
        auto w = std::make_unique<physics::PhysicsWorld>();
        w->settings().fixedTimeStep = kStep;
        w->settings().maxSubSteps = 4;
        physics::RigidBody b;
        b.type = physics::BodyType::Dynamic;
        b.entity = 1;
        b.shape = physics::Shape::box(glm::vec3(0.5f));
        b.position = glm::vec3(0.0f, 0.0f, 1000.0f);
        b.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        b.material.density = 500.0f;
        b.massProps = b.shape.massFromDensity(b.material.density);
        b.setFlag(physics::kBodyUseGravity, true);
        b.setFlag(physics::kBodyAllowSleep, true);
        w->addBody(std::move(b));
        return w;
    };

    auto hashPositions = [](const physics::PhysicsWorld& w) {
        std::uint64_t h = 1469598103934665603ull;
        for (const physics::RigidBody& b : w.bodies()) {
            h = hashFloat(h, b.position.x);
            h = hashFloat(h, b.position.y);
            h = hashFloat(h, b.position.z);
            h = hashFloat(h, b.linearVelocity.x);
            h = hashFloat(h, b.linearVelocity.y);
            h = hashFloat(h, b.linearVelocity.z);
        }
        return h;
    };

    // ---- A：GameLoop 的累加器恰好跑 kSteps 步 ----
    std::uint64_t accelSteps = 0;
    {
        runtime::GameLoop loop;
        loop.config().fixedTimeStep = kStep;
        loop.config().maxSubSteps = 4;
        loop.config().timeScale = 1.0f;
        loop.config().maxFps = 0;
        loop.config().unfocusedFps = 0;
        loop.setLogInterval(0);

        runtime::LoopCallbacks cb;
        cb.fixedUpdate = [&](float) { ++accelSteps; };
        for (int i = 0; i < kSteps; ++i) loop.tick(kStep, true, cb);
    }
    {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "accel=%llu expect=%d",
                      static_cast<unsigned long long>(accelSteps), kSteps);
        check("A 累加器跑 300 帧 dt=1/60 → 恰好 300 步",
              accelSteps == static_cast<std::uint64_t>(kSteps), detail);
        std::printf("GAME-DET steps accel=%llu expect=%d result=%s\n",
                    static_cast<unsigned long long>(accelSteps), kSteps,
                    accelSteps == static_cast<std::uint64_t>(kSteps) ? "PASS"
                                                                   : "FAIL");
    }

    // ---- B：累加器路径 vs stepFixed 路径，逐位相同 ----
    std::uint64_t hAccel = 0;
    {
        auto w = makeWorld();
        runtime::GameLoop loop;
        loop.config().fixedTimeStep = kStep;
        loop.config().maxSubSteps = 4;
        loop.config().timeScale = 1.0f;
        loop.config().maxFps = 0;
        loop.config().unfocusedFps = 0;
        loop.setLogInterval(0);
        runtime::LoopCallbacks cb;
        cb.fixedUpdate = [&](float step) { w->step(step); };
        for (int i = 0; i < kSteps; ++i) loop.tick(kStep, true, cb);
        hAccel = hashPositions(*w);
    }
    std::uint64_t hRef = 0;
    {
        auto w = makeWorld();
        for (int i = 0; i < kSteps; ++i) w->stepFixed(kStep);
        hRef = hashPositions(*w);
    }
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail), "accel=%016llx ref=%016llx",
                      static_cast<unsigned long long>(hAccel),
                      static_cast<unsigned long long>(hRef));
        check("B 累加器路径与 stepFixed 路径逐位相同", hAccel == hRef, detail);
        std::printf("GAME-DET accel_hash=%016llx ref_hash=%016llx result=%s\n",
                    static_cast<unsigned long long>(hAccel),
                    static_cast<unsigned long long>(hRef),
                    hAccel == hRef ? "PASS" : "FAIL");
    }

    // ---- C：timeScale = 0 → 完全停住 ----
    {
        auto w = makeWorld();
        for (int i = 0; i < 10; ++i) w->stepFixed(kStep);   // 先动 10 步
        const std::uint64_t before = hashPositions(*w);

        runtime::GameLoop loop;
        loop.config().fixedTimeStep = kStep;
        loop.config().timeScale = 0.0f;
        loop.config().maxFps = 0;
        loop.config().unfocusedFps = 0;
        loop.setLogInterval(0);
        std::uint64_t steps = 0;
        runtime::LoopCallbacks cb;
        cb.fixedUpdate = [&](float step) {
            ++steps;
            w->step(step);
        };
        for (int i = 0; i < 120; ++i) loop.tick(kStep, true, cb);

        const std::uint64_t after = hashPositions(*w);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "steps=%llu moved=%s",
                      static_cast<unsigned long long>(steps),
                      before == after ? "0" : "non-zero");
        check("C timeScale=0 时物理完全停住（位置零漂移）",
              steps == 0 && before == after, detail);
        std::printf("GAME-DET frozen steps=%llu moved=%s result=%s\n",
                    static_cast<unsigned long long>(steps),
                    before == after ? "0" : "non-zero",
                    (steps == 0 && before == after) ? "PASS" : "FAIL");
    }

    std::printf("GAME-DET-TOTAL pass=%d fail=%d\n", g_selftestPass,
                g_selftestFail);
    return g_selftestFail == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        // --help 先拦掉：它不该创建日志文件、也不该起窗口
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            std::printf("%s", runtime::runtimeHelpText());
            return 0;
        }
        args.emplace_back(argv[i]);
    }

    try {
        VK_LOG_INFO("=== Venn Engine - Runtime (Game.exe) ===");

        // 日志先开：这样下面 parseRuntimeConfig 打出来的 GAME-CONFIG 也会
        // 落进文件里（排查"配错参数"时那几行最关键）。
        runtime::RuntimeLog log;
        const std::string logPath = log.open("venn");
        if (!logPath.empty()) VK_LOG_INFO("GAME-LOG path=%s", logPath.c_str());

        const runtime::RuntimeConfig cfg = runtime::parseRuntimeConfig(args);

        // ---- 无窗口自检：在 Vulkan / 窗口之前返回 ----
        // （与 editor/main.cpp 的 MYVK_PHYS_TEST 同一个位置、同一条理由：
        //   自检不依赖 GPU，就该能在任何机器上跑，且跑得飞快。）
        if (cfg.selftest == "states") return runStateSelftest();
        if (cfg.selftest == "determinism") return runDeterminismSelftest();

        // ---- M2 的五个无窗口自检 ----
        // 每一个都是"跑完打印 TOTAL，失败数当退出码"，与上面两个同一条规矩：
        // 自动化脚本只看最后一行 GAME-*-TOTAL 与进程返回码。
        if (cfg.selftest == "input")
            return core::runInputSelfTest() == 0 ? 0 : 1;
        if (cfg.selftest == "character")
            return physics::runCharacterSelfTest() == 0 ? 0 : 1;
        if (cfg.selftest == "contact")
            return physics::runContactSelfTest() == 0 ? 0 : 1;
        if (cfg.selftest == "camera")
            return scene::runCameraSelfTest() == 0 ? 0 : 1;
        if (cfg.selftest == "player")
            return runtime::runPlayerSelfTest() == 0 ? 0 : 1;

        if (!cfg.selftest.empty()) {
            VK_LOG_WARN("未知自检项 '%s'（可用：states | determinism | input | "
                        "character | contact | camera | player）",
                        cfg.selftest.c_str());
        }

        runtime::RuntimeApp app(cfg);
        const int rc = app.run(cfg.maxFrames);
        VK_LOG_INFO("GAME-EXIT code=%d log=%s", rc, logPath.c_str());
        return rc;
    } catch (const std::exception& e) {
        VK_LOG_ERROR("GAME-EXIT fatal: %s", e.what());
        return 1;
    }
}
