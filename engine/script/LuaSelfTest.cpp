// ============================================================
// script/LuaSelfTest —— Lua 后端自检（--selftest lua）
//
// 十组，覆盖"换语言后会真出事的四件事"：
//   A 白名单（os/io/debug/math.random 必须不存在）
//   B 编译（合法 / 语法错 / 没 return table）
//   C 执行（BeginPlay 一次、Tick 每帧、实例跨帧状态）
//   D 协程（wait 挂起，到点才继续）
//   E 熔断（死循环被预算打断，而不是冻住）
//   F 热重载（方法换新、字段保留）
//
// ⚠ 这里的断言全部是**可数的事实**（位置数值、n 的值、返回码），
//   不做"跑起来没崩"这种推断 —— 脚本的错误几乎全都不报错，
//   只能靠数出来（与 ScriptSelfTest 同一条纪律）。
// ============================================================

#include "script/LuaSelfTest.h"

#include "assets/Material.h"
#include "assets/Model.h"
#include "assets/MorphRig.h"
#include "ecs/Components.h"
#include "scene/CameraMove.h"
#include "scene/Scene.h"
#include "script/ApiTable.h"
#include "script/LuaBackend.h"
#include "script/ScriptEngine.h"

// 自检要直接摸 `lua_State`（验 os/io/debug 在不在），所以显式包含 Lua 头。
// 引擎其余部分**不应该**包含它 —— 那是"只有后端懂 Lua"的边界。
#include <lua.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
// MSVC 侧靠这条自动带上 dbghelp.lib；g++（MinGW）**不认识** #pragma comment，
// 会静默忽略 → 链接期报 `undefined reference to __imp_SymFromAddr` 等一串。
// MinGW 的依赖由 CMakeLists.txt 的 `target_link_libraries(... dbghelp)` 提供，
// 两边都必须留：删了这里 MSVC 会断，删了那里 MinGW 会断。
#if defined(_MSC_VER)
#pragma comment(lib, "dbghelp.lib")
#endif
namespace {
// 崩溃时把异常码和地址打出来 —— 段错误只说"崩了"，异常码才说"为什么崩"：
//   0xC0000005 访问冲突 / 0xC0000409 __fastfail（栈破坏）/ 0xC00000FD 栈溢出
//
// ⚠ 光有地址不够（ASLR + 14 MB 的 exe，肉眼无从对号）。所以这里用 dbghelp
//   把地址翻成"函数名 + 行号"，并把调用栈整条打出来 —— `Game.pdb` 就在 exe
//   旁边，SymInitialize 会自己找到，不需要额外配置。
void dumpSym(HANDLE proc, DWORD64 addr, const char* tag) {
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;

    DWORD64 disp = 0;
    const char* name = SymFromAddr(proc, addr, &disp, si) ? si->Name : "<no symbol>";

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisp = 0;
    if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
        std::fprintf(stderr, "LUA-CRASH %s %s+0x%llx  (%s:%lu)\n", tag, name,
                     static_cast<unsigned long long>(disp), line.FileName, line.LineNumber);
    } else {
        std::fprintf(stderr, "LUA-CRASH %s %s+0x%llx\n", tag, name,
                     static_cast<unsigned long long>(disp));
    }
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "LUA-CRASH code=0x%08lX addr=%p\n",
                 static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                 ep->ExceptionRecord->ExceptionAddress);
    std::fflush(stderr);

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (SymInitialize(proc, nullptr, TRUE)) {
        dumpSym(proc, reinterpret_cast<DWORD64>(ep->ExceptionRecord->ExceptionAddress), "AT ");

        CONTEXT ctx = *ep->ContextRecord;
        STACKFRAME64 sf{};
        sf.AddrPC.Offset    = ctx.Rip; sf.AddrPC.Mode    = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 20; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx,
                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (sf.AddrPC.Offset == 0) break;
            dumpSym(proc, sf.AddrPC.Offset, "BT ");
        }
        SymCleanup(proc);
    } else {
        std::fprintf(stderr, "LUA-CRASH (SymInitialize failed, err=%lu)\n", GetLastError());
    }
    std::fflush(stderr);
    std::_Exit(99);
}
} // namespace
#endif

namespace vks {
namespace {

int g_pass = 0;
int g_fail = 0;

void check(const char* name, bool ok, const std::string& detail = {}) {
    std::printf("LUA-T %s %s %s\n", ok ? "pass" : "fail", name, detail.c_str());
    if (ok) ++g_pass; else ++g_fail;
}

std::string fmt(const char* a, double v) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s=%.4f", a, v);
    return buf;
}

// 造一个带 Transform 的探针实体
ecs::Entity makeProbe(scene::Scene& sc, const char* name) {
    const ecs::Entity e = sc.createObject(name);
    if (!sc.world().get<ecs::TransformComponent>(e)) {
        ecs::TransformComponent t;
        sc.world().add<ecs::TransformComponent>(e, t);
    }
    return e;
}

float posX(scene::Scene& sc, ecs::Entity e) {
    return sc.worldMatrix(e)[3].x;
}

// 跑 n 帧，返回"最后一次 tick 的返回值"
bool runTicks(IScriptInstance& inst, ScriptHost& host, int n, float dt) {
    bool ok = true;
    for (int i = 0; i < n; ++i) ok = inst.tick(host, dt) && ok;
    return ok;
}

const char* kCounter = R"LUA(
local M = { n = 0 }
function M:BeginPlay()
    self.n = 1
end
function M:Tick(dt)
    self.n = self.n + 1
    world.setPos(self.entity, engine.vec3(self.n, 0, 0))
end
return M
)LUA";

// G 组：把 **12 个命名空间的 106 个函数全部调一遍**。
//
// 为什么值得单列一条：绑定是表驱动的，漏一条不会编译失败、不会报错，
// 只会变成"文档里有、脚本里调不到"。这条脚本是那张表的**运行期核对** ——
// 任何一个名字写错 / 实现缺失 / 参数个数不符，`beginPlay` 立刻红。
// 宿主桩全部填满（下面的 ScriptHost），缺一个就会报 `not available`。
const char* kAllApi = R"LUA(
local M = {}
function M:BeginPlay()
    local me = engine.self()
    local v  = engine.vec3

    -- engine
    engine.log("cover", me)
    engine.warn("cover-warn")
    engine.clamp(3, 0, 1)
    engine.lerp(0, 10, 0.5)
    engine.dist(v(0, 0, 0), v(3, 4, 0))
    engine.valid(me)
    engine.exists(me)
    engine.name(me)

    -- world
    world.setPos(me, v(1, 2, 3))
    world.pos(me); world.rot(me); world.rotY(me); world.scale(me)
    world.setRot(me, v(0, 0, 0.5)); world.setScale(me, v(1, 1, 1))
    world.moveBy(me, v(0.5, 0, 0))
    world.find("ProbeG")
    world.count()
    local near = world.near(v(0, 0, 0), 99, 4)
    local sp   = world.spawn("models/box.vmodel", v(0, 0, 5))
    game.damage(sp, 10, "fire")          -- 打别人（ui.damage 是打自己）
    world.despawn(sp)
    world.loadScene("scenes/sandbox.json")

    -- actor
    actor.setVisible(me, true); actor.visible(me)
    actor.setShadow(me, true)
    actor.setVel(me, v(0, 0, 0)); actor.vel(me)
    actor.impulse(me, v(0, 0, 1))
    actor.grounded(me)
    actor.setEmissive(me, 1, 0, 0)

    -- input
    input.pressed("Jump"); input.held("Jump")
    input.released("Jump"); input.axis("MoveX")

    -- anim / nav / fx
    anim.play(me, "Run")
    nav.moveTo(me, 0, 0, 5); nav.arrived(me); nav.failed(me); nav.stop(me)
    local fid = fx.play("fx/hit")
    fx.play("fx/hit", 1, 2, 3)
    fx.stop(fid)
    fx.shake(0.2); fx.hitstop(0.05); fx.timeScale(1.0)

    -- audio / light / ui
    audio.play("audio/click.wav"); audio.bus("music", 0.4); audio.stopAll()
    light.color(me, 1, 0.9, 0.8); light.intensity(me, 2.0)
    light.range(me, 4.0); light.direction(me, 0, 0, -1)
    ui.health(80); ui.damage(5, "fire"); ui.popup(7)
    ui.dialogue("ch01"); ui.sequence("sequences/open.seq.json"); ui.open("")

    -- game
    game.setAttr(me, "hp", 10)
    local hp  = game.attr(me, "hp")
    local hp2 = game.attr(me, "mp", 3)        -- 带默认值 → 写回
    game.stat(me, "atk"); game.hasStat(me, "atk")
    game.setWorldAttr("wave", 2); game.worldAttr("wave")
    game.round(); game.status(me, "poison")
    game.flag("met", 1); local met = game.flag("met")
    game.castCard(me, 0); game.drawCards(me, 1)
    game.tableNum("cards.strike.cost"); game.tableStr("cards.strike.name")
    game.tableHas("cards.strike.cost"); game.tableRows("cards")

    game.joint(me, me, "hinge")
    game.jointAnchors(me, v(0, 0, 0), v(0, 0, 0))
    game.jointAxis(me, v(0, 0, 1))
    game.jointLimit(me, true, -1, 1)
    game.jointMotor(me, 1, 10)
    game.jointOn(me, true)
    game.jointKind(me); game.jointTarget(me)
    game.jointRemove(me)

    -- 组件读写（C+ 新增 19 个：层级 / 材质 / 动画控制 / 形态键 / 表情 / 相机 / 灯开关）
    local kid = world.spawn("models/box.vmodel", v(0, 0, 1))
    world.setName(kid, "KidG")
    world.setParent(kid, me)
    world.parent(kid); world.children(me)
    world.setParent(kid, 0)                  -- 无效句柄 = 拔到根
    actor.color(me); actor.setColor(me, 1, 1, 1)
    anim.pause(me); anim.resume(me)
    anim.speed(me, 1.5); anim.time(me, 0.1)
    anim.clip(me); anim.stop(me)
    anim.morph(me, "smile", 0.5); anim.morphWeight(me, "smile")
    anim.expression(me, "")
    camera.fov(me, 60); camera.distance(me, 5); camera.enabled(me, true)
    light.enabled(me, true)

    self.api_cover_done = true
end
return M
)LUA";

} // namespace

int runLuaSelfTest() {
    g_pass = 0;
    g_fail = 0;
#ifdef _WIN32
    SetUnhandledExceptionFilter(&crashFilter);
#endif

    std::printf("== selftest: lua ==\n");

    // ---- A5 最小协程：绕开本引擎所有封装，直接验 Lua 库本身 ----
    //
    // 这一条的价值是"把责任范围一刀切开"：它只用 Lua 公开 API，不碰
    // LuaBackend 任何一行。它红了 → 问题在库/链接；它绿而别的红 → 问题在
    // 后端。2026-11-03 那次 `lua_resume` 段错误就是靠它把范围从"全是嫌疑"
    // 缩到"一行代码" —— 最终根因是这里第 4 个参数传了 nullptr。
    {
        lua_State* L = luaL_newstate();
        check("A5a_new_state", L != nullptr);
        if (L) {
            luaL_openlibs(L);
            lua_State* co = lua_newthread(L);
            const int loadRc = luaL_loadstring(co, "local a = ...; return a + 1");
            check("A5b_load_string", loadRc == LUA_OK, std::to_string(loadRc));
            if (loadRc == LUA_OK) {
                lua_pushinteger(co, 41);
                // ⚠ 第 4 个参数必须指向有效内存：Lua 5.4.7 的 ldo.c:863
                //   无条件 `*nresults = ...`，传 nullptr 就是往地址 0 写。
                int a5nres = 0;
                const int rc = lua_resume(co, L, 1, &a5nres);
                const int got = int(lua_tointeger(co, -1));
                check("A5c_coroutine_ran", rc == LUA_OK && got == 42,
                      "rc=" + std::to_string(rc) + " got=" + std::to_string(got));
            }
            lua_close(L);
        }
    }

    // ---- A 白名单 ----
    {
        LuaBackend be;
        lua_State* L = be.state();
        check("A1_vm_alive", L != nullptr);

        if (L) {
            lua_getglobal(L, "os");
            const bool noOs = lua_isnil(L, -1);
            lua_pop(L, 1);
            lua_getglobal(L, "io");
            const bool noIo = lua_isnil(L, -1);
            lua_pop(L, 1);
            lua_getglobal(L, "debug");
            const bool noDebug = lua_isnil(L, -1);
            lua_pop(L, 1);
            check("A2_no_os_io_debug", noOs && noIo && noDebug);

            lua_getglobal(L, "math");
            lua_getfield(L, -1, "random");
            const bool noRandom = lua_isnil(L, -1);
            lua_pop(L, 2);
            check("A3_no_math_random", noRandom);

            lua_getglobal(L, "coroutine");
            const bool hasCo = lua_istable(L, -1);
            lua_pop(L, 1);
            check("A4_has_coroutine", hasCo);
        }
    }

    // ---- B 编译 ----
    {
        LuaBackend be;
        auto ok1 = be.compile("t_ok.lua", kCounter);
        check("B1_compile_ok", ok1 && ok1->ok(), ok1 ? ok1->error() : "null");
        check("B2_hook_detected", ok1 && ok1->hasHook(HookKind::Update) &&
                                     ok1->hasHook(HookKind::Start));

        auto bad = be.compile("t_bad.lua", "function oops( end");
        check("B3_syntax_error", bad && !bad->ok() && !bad->error().empty(),
              bad ? bad->error() : "null");

        auto noRet = be.compile("t_noret.lua", "local x = 1");
        check("B4_requires_table", noRet && !noRet->ok() &&
                                       noRet->error().find("table") != std::string::npos,
              noRet ? noRet->error() : "null");
    }

    // ---- C 执行 ----
    {
        scene::Scene sc;
        const ecs::Entity e = makeProbe(sc, "ProbeC");
        LuaBackend be;
        auto prog = be.compile("t_counter.lua", kCounter);
        auto inst = be.createInstance(prog, e);
        check("C0_instance_created", inst != nullptr);
        if (inst) {
            ScriptHost host;
            host.scene = &sc;
            host.entity = e;
            inst->beginPlay(host);
            runTicks(*inst, host, 3, 1.0f / 60.0f);
            const float x = posX(sc, e);
            check("C1_position_moved", x > 3.5f && x < 4.5f, fmt("x", x));

            auto vars = inst->snapshotVars();
            std::string dump;
            for (auto& kv : vars) dump += kv.first + "=" + kv.second + " ";
            check("C2_vars_visible", !vars.empty(), dump);
        }
    }

    // ---- D 协程 ----
    {
        scene::Scene sc;
        const ecs::Entity e = makeProbe(sc, "ProbeD");
        LuaBackend be;
        auto prog = be.compile("t_wait.lua", R"LUA(
local M = { phase = 0 }
function M:Tick(dt)
    self.phase = 1
    engine.wait(0.5)
    self.phase = 2
end
return M
)LUA");
        auto inst = be.createInstance(prog, e);
        ScriptHost host;
        host.scene = &sc;
        host.entity = e;
        if (inst) {
            inst->tick(host, 0.1f);
            check("D1_yielded", inst->waiting());
            inst->tick(host, 0.1f);
            inst->tick(host, 0.1f);
            check("D2_still_waiting", inst->waiting());
            inst->tick(host, 0.4f);  // 累计 0.5 → 到点
            check("D3_resumed", !inst->waiting());
            auto vars = inst->snapshotVars();
            std::string phase = "?";
            for (auto& kv : vars)
                if (kv.first == "phase") phase = kv.second;
            check("D4_second_half_ran", phase == "2", "phase=" + phase);
        } else {
            check("D1_yielded", false, "no instance");
        }
    }

    // ---- E 熔断 ----
    {
        scene::Scene sc;
        const ecs::Entity e = makeProbe(sc, "ProbeE");
        LuaBackend be(20000);
        auto prog = be.compile("t_loop.lua", R"LUA(
local M = {}
function M:Tick(dt)
    while true do end
end
return M
)LUA");
        auto inst = be.createInstance(prog, e);
        ScriptHost host;
        host.scene = &sc;
        host.entity = e;
        if (inst) {
            const bool ok = inst->tick(host, 0.016f);
            check("E1_budget_stopped", !ok && inst->failed(), inst->lastError());
            const bool again = inst->tick(host, 0.016f);
            check("E2_stays_failed", !again);
        } else {
            check("E1_budget_stopped", false, "no instance");
        }
    }

    // ---- F 热重载 ----
    {
        scene::Scene sc;
        const ecs::Entity e = makeProbe(sc, "ProbeF");
        LuaBackend be;
        auto v1 = be.compile("t_reload.lua", R"LUA(
local M = { hp = 100 }
function M:Tick(dt)
    self.hp = self.hp - 1
end
return M
)LUA");
        auto inst = be.createInstance(v1, e);
        ScriptHost host;
        host.scene = &sc;
        host.entity = e;
        if (inst) {
            inst->tick(host, 0.016f);  // hp: 100 → 99

            auto v2 = be.compile("t_reload.lua", R"LUA(
local M = { hp = 999 }
function M:Tick(dt)
    self.hp = self.hp + 10
end
return M
)LUA");
            const bool reloaded = inst->reload(v2);
            check("F1_reload_ok", reloaded);
            inst->tick(host, 0.016f);  // 方法换新(+10)，字段保留(99) → 109

            std::string hp = "?";
            for (auto& kv : inst->snapshotVars())
                if (kv.first == "hp") hp = kv.second;
            check("F2_field_preserved", hp == "109", "hp=" + hp);
        } else {
            check("F1_reload_ok", false, "no instance");
        }
    }

    // ---- G 覆盖：12 个命名空间 106 个函数全调一遍 ----
    {
        scene::Scene sc;
        const ecs::Entity e = makeProbe(sc, "ProbeG");
        // 材质与灯光：`actor.setEmissive` / `light.*` 要有组件才成立
        assets::Material mat;
        auto& mc = sc.world().add<ecs::MaterialComponent>(e);
        mc.material = &mat;
        sc.world().add<ecs::PointLightComponent>(e);
        // C+ 新增的那批也要有组件才成立（camera / anim / morph / expression）
        sc.world().add<ecs::CameraComponent>(e);
        sc.world().add<ecs::AnimatorComponent>(e);
        sc.world().add<ecs::ExpressionComponent>(e);
        // ⚠ `gm` 必须与实例同寿命（脚本里会读 `morphRig`）—— 放块作用域，
        //   别塞进内层大括号：那会变成悬垂指针，读到的是别人的栈。
        assets::Model gm;
        {
            std::vector<std::vector<assets::MorphChannel>> perSub(1);
            assets::MorphChannel ch;
            ch.name = "smile";
            assets::MorphChannelKey k;
            k.targetWeight = 1.0f;
            k.target = 0u;
            ch.keys.push_back(k);
            perSub[0].push_back(ch);
            std::vector<std::uint32_t> counts{1u};
            gm.morphRig.build(perSub, counts);
        }
        auto& gmp = sc.world().add<ecs::MorphComponent>(e);
        gmp.model = &gm;
        gmp.weights.assign(gm.morphRig.channelCount(), 0.0f);

        LuaBackend be;
        auto prog = be.compile("t_api.lua", kAllApi);
        auto inst = be.createInstance(prog, e);
        check("G0_all_api_compiled", inst != nullptr, prog ? prog->error() : "null");
        if (inst) {
            ScriptHost host;
            host.scene = &sc;
            host.entity = e;
            // 宿主桩**全部填满**：缺哪个，脚本调它就会报 `not available`，
            // 正好当"这个域有没有接上"的探针。
            host.setHealth = [](float) {};
            host.damage = [](float) {};
            host.popup = [](float, float, float, float) {};
            host.playSound = [](const std::string&) {};
            host.stopAllSounds = []() {};
            host.setBusVolume = [](const std::string&, float) {};

            host.act.player = [] { return ecs::Entity{}; };
            host.act.bodyVel = [](ecs::Entity, glm::vec3* v) {
                if (v) *v = glm::vec3(0.0f);
                return true;
            };
            host.act.setBodyVel = [](ecs::Entity, glm::vec3) {};
            host.act.impulse = [](ecs::Entity, glm::vec3) {};
            host.act.grounded = [](ecs::Entity) { return true; };
            host.act.pressed = [](const std::string&) { return false; };
            host.act.held = [](const std::string&) { return false; };
            host.act.released = [](const std::string&) { return false; };
            host.act.axis = [](const std::string&) { return 0.0f; };
            host.act.playSequence = [](const std::string&) { return true; };
            host.act.showDialogue = [](const std::string&) { return true; };
            host.act.playAnim = [](ecs::Entity, const std::string&) { return true; };
            host.act.loadScene = [](const std::string&) { return true; };
            host.act.spawn = [&sc](const std::string&, glm::vec3) {
                return sc.createObject("SpawnedG");
            };
            host.act.playEffect = [](ecs::Entity, const std::string&) {
                return uint32_t(1);
            };
            host.act.playEffectAt = [](const std::string&, glm::vec3) {
                return uint32_t(2);
            };
            host.act.stopEffect = [](uint32_t) { return true; };
            host.act.shake = [](float, ecs::Entity) {};
            host.act.hitstop = [](float) {};
            host.act.setTimeScale = [](float) {};
            host.act.getAttr = [](ecs::Entity, const std::string&) { return 1.0f; };
            host.act.gameplayHasAttr = [](ecs::Entity, const std::string&) { return true; };
            host.act.addStatus = [](ecs::Entity, const std::string&) { return 1; };
            host.act.damage = [](ecs::Entity, float, const std::string&) { return 5.0f; };
            host.act.tickRound = [] { return 0; };
            host.act.castCard = [](ecs::Entity, int) { return 3.0f; };
            host.act.drawCards = [](ecs::Entity, int n) { return n; };
            host.act.navGoto = [](ecs::Entity, glm::vec3) { return true; };
            host.act.navArrived = [](ecs::Entity) { return false; };
            host.act.navFailed = [](ecs::Entity) { return false; };
            host.act.navStop = [](ecs::Entity) { return true; };
            host.act.tableNum = [](const std::string&) { return 1.0; };
            host.act.tableStr = [](const std::string&) { return std::string("x"); };
            host.act.tableHas = [](const std::string&) { return true; };
            host.act.tableRows = [](const std::string&) { return 3; };
            host.act.openUI = [](const std::string&) { return true; };
            host.act.setFlag = [](const std::string&, double) { return true; };
            host.act.getFlag = [](const std::string&) { return 0.0; };
            host.act.joints.set = [](ecs::Entity, ecs::Entity, const std::string&) {
                return true;
            };
            host.act.joints.anchors = [](ecs::Entity, glm::vec3, glm::vec3) {
                return true;
            };
            host.act.joints.axis = [](ecs::Entity, glm::vec3) { return true; };
            host.act.joints.limit = [](ecs::Entity, bool, float, float) { return true; };
            host.act.joints.motor = [](ecs::Entity, float, float) { return true; };
            host.act.joints.enable = [](ecs::Entity, bool) { return true; };
            host.act.joints.remove = [](ecs::Entity) { return true; };
            host.act.joints.kindOf = [](ecs::Entity) -> const char* { return "hinge"; };
            host.act.joints.target = [](ecs::Entity) { return ecs::Entity{}; };

            const bool ok = inst->beginPlay(host);
            std::string done = "false";
            for (auto& kv : inst->snapshotVars())
                if (kv.first == "api_cover_done") done = kv.second;
            check("G1_all_namespaces_called", ok && !inst->failed() && done == "true",
                  std::string("beginPlay=") + (ok ? "ok" : "fail") +
                      " done=" + done + " err=" + inst->lastError());
        }
    }

    // ---- H `engine.dt/time/frame` 真的在动 ----
    //
    // 这三个曾经是**死字段**：注册时写成 0，之后没人刷新 —— 文档写了却
    // 永远返回 0，是最难被发现的那类"API 存在但没用"。所以留一条断言。
    {
        scene::Scene sc;
        const ecs::Entity e = makeProbe(sc, "ProbeH");
        LuaBackend be;
        auto prog = be.compile("t_clock.lua", R"LUA(
local M = {}
function M:BeginPlay()
    self.t0 = engine.time
    self.f0 = engine.frame
end
function M:Tick(dt)
    self.dt = engine.dt
    self.t  = engine.time
    self.f  = engine.frame
end
return M
)LUA");
        auto inst = be.createInstance(prog, e);
        if (inst) {
            ScriptHost host;
            host.scene = &sc;
            host.entity = e;
            inst->beginPlay(host);
            inst->tick(host, 0.5f);

            std::string dt, t, f;
            for (auto& kv : inst->snapshotVars()) {
                if (kv.first == "dt") dt = kv.second;
                if (kv.first == "t") t = kv.second;
                if (kv.first == "f") f = kv.second;
            }
            check("H1_clock_fields_live", dt == "0.5" && t == "0.5" && f == "1",
                  "dt=" + dt + " time=" + t + " frame=" + f);
        } else {
            check("H1_clock_fields_live", false, "no instance");
        }
    }

    // ---- I 组件写入真的落到组件上了 ----
    //
    // 为什么不能只看"没报错"：`actor.setColor` 写错字段（改成 `albedoMap`）、
    // `camera.fov` 单位写成弧度、`anim.time` 忘了同步 `eventPrev` ——
    // 这三个都**不报错、不崩溃**，只是数值不对，而"数值不对"正是脚本作者
    // 最先撞上的那类 bug。所以这里**从 C++ 侧读组件**比对，不让脚本自证。
    {
        scene::Scene sc;
        const ecs::Entity e = makeProbe(sc, "ProbeI");

        // 探针补齐这轮 API 需要的组件（材质是共享资产，用块内的静态
        // 对象保证生命周期盖住整段 —— 用局部对象会在实例还活着时失效）。
        static assets::Material matI;
        matI.baseColorFactor = glm::vec4(1.0f);
        matI.emissive = glm::vec3(0.0f);
        ecs::MaterialComponent mc;
        mc.material = &matI;
        sc.world().add<ecs::MaterialComponent>(e, mc);

        ecs::CameraComponent cam;
        sc.world().add<ecs::CameraComponent>(e, cam);
        ecs::PointLightComponent pl;
        sc.world().add<ecs::PointLightComponent>(e, pl);

        ecs::AnimatorComponent an;
        an.defaultClip = "Idle";
        sc.world().add<ecs::AnimatorComponent>(e, an);

        // 形态键：造一个只有一条滑条 "smile" 的合成 rig
        assets::Model modelI;
        {
            std::vector<std::vector<assets::MorphChannel>> perSub(1);
            assets::MorphChannel ch;
            ch.name = "smile";
            assets::MorphChannelKey k;
            k.targetWeight = 1.0f;
            k.target = 0u;
            ch.keys.push_back(k);
            perSub[0].push_back(ch);
            std::vector<std::uint32_t> counts{1u};
            modelI.morphRig.build(perSub, counts);
        }
        ecs::MorphComponent mp;
        mp.model = &modelI;
        mp.weights.assign(modelI.morphRig.channelCount(), 0.0f);
        sc.world().add<ecs::MorphComponent>(e, mp);

        LuaBackend be;
        auto prog = be.compile("t_comp.lua", R"LUA(
local M = {}
function M:BeginPlay()
    local me  = engine.self()
    local kid = world.spawn("models/box.vmodel", engine.vec3(0, 0, 1))
    world.setName(kid, "kidI")
    world.setParent(kid, me)
    self.kids    = #world.children(me)
    -- 否定测试：把自己的父设成自己的子 → 必须被拒（否则父链成环）
    self.cycleOk = pcall(function() world.setParent(me, kid) end)

    actor.setColor(me, 0.25, 0.5, 0.75)  -- 只有 me 挂了材质

    camera.fov(me, 70)
    camera.distance(me, 9)
    camera.enabled(me, false)

    light.enabled(me, false)

    anim.speed(me, 2.5)
    anim.time(me, 0.5)
    anim.pause(me)
    self.clip = anim.clip(me)

    anim.morph(me, "smile", 0.75)
    self.morphW = anim.morphWeight(me, "smile")
    -- 否定测试：通道名拼错必须报错（静默的话症状只是"表情没变"）
    self.badChannelOk = pcall(function() anim.morph(me, "nope", 1) end)
end
return M
)LUA");
        auto inst = be.createInstance(prog, e);
        check("I0_comp_script_compiled", inst != nullptr,
              prog ? prog->error() : "null");
        if (inst) {
            ScriptHost host;
            host.scene = &sc;
            host.entity = e;
            host.act.spawn = [&sc](const std::string&, glm::vec3) {
                return sc.createObject("SpawnedI");
            };
            const bool ok = inst->beginPlay(host);

            std::string kids, clip, cycleOk, badCh, morphW;
            for (auto& kv : inst->snapshotVars()) {
                if (kv.first == "kids") kids = kv.second;
                if (kv.first == "clip") clip = kv.second;
                if (kv.first == "cycleOk") cycleOk = kv.second;
                if (kv.first == "badChannelOk") badCh = kv.second;
                if (kv.first == "morphW") morphW = kv.second;
            }

            // ① 层级：子表里真有一个、名字改了、父指回来
            const auto* hc = sc.world().get<ecs::HierarchyComponent>(e);
            bool kidOk = false;
            if (hc && hc->children.size() == 1) {
                const ecs::Entity kid = sc.world().handle(hc->children[0]);
                const std::string* kn = sc.world().name(kid);
                const auto* kh = sc.world().get<ecs::HierarchyComponent>(kid);
                kidOk = kn && *kn == "kidI" && kh && kh->parent == e.id;
            }
            std::string diag = "kids=" + kids;
            if (hc) {
                diag += " n=" + std::to_string(hc->children.size());
                if (!hc->children.empty()) {
                    const ecs::Entity kid = sc.world().handle(hc->children[0]);
                    const std::string* kn = sc.world().name(kid);
                    const auto* kh = sc.world().get<ecs::HierarchyComponent>(kid);
                    diag += " name=" + (kn ? *kn : std::string("<none>"));
                    diag += " parent=" + std::to_string(kh ? kh->parent : 0u);
                    diag += " me=" + std::to_string(e.id);
                }
            } else {
                diag += " no-hierarchy";
            }
            check("I1_hierarchy_written", ok && !inst->failed() && kids == "1" && kidOk,
                  "beginPlay=" + std::string(ok ? "ok" : "fail") + " " + diag +
                      " err=" + inst->lastError());
            check("I2_cycle_rejected", cycleOk == "false",
                  "pcall 应该失败（成环被拒），实际 cycleOk=" + cycleOk);

            // ② 材质底色：rgb 三个都要对，alpha 保持原值
            check("I3_material_color",
                  std::fabs(matI.baseColorFactor.r - 0.25f) < 1e-5f &&
                      std::fabs(matI.baseColorFactor.g - 0.50f) < 1e-5f &&
                      std::fabs(matI.baseColorFactor.b - 0.75f) < 1e-5f &&
                      std::fabs(matI.baseColorFactor.a - 1.00f) < 1e-5f,
                  "baseColorFactor=(" + std::to_string(matI.baseColorFactor.r) + "," +
                      std::to_string(matI.baseColorFactor.g) + "," +
                      std::to_string(matI.baseColorFactor.b) + "," +
                      std::to_string(matI.baseColorFactor.a) + ")");

            // ③ 相机：度 vs 弧度写错会变成 70 rad（≈4010°），这里能立刻看出
            const auto* camI = sc.world().get<ecs::CameraComponent>(e);
            check("I4_camera_fields",
                  camI && std::fabs(camI->fov - 70.0f) < 1e-4f &&
                      std::fabs(camI->distance - 9.0f) < 1e-4f && !camI->enabled,
                  camI ? ("fov=" + std::to_string(camI->fov) +
                          " dist=" + std::to_string(camI->distance) +
                          " enabled=" + (camI->enabled ? "1" : "0"))
                       : "no camera");

            // ④ 灯：点光的 enabled
            const auto* plI = sc.world().get<ecs::PointLightComponent>(e);
            check("I5_light_enabled", plI && !plI->enabled,
                  plI ? (plI->enabled ? "still on" : "off") : "no light");

            // ⑤ 动画：速度 / 播放头 / 暂停 / eventPrev 跟着跳
            const auto* anI = sc.world().get<ecs::AnimatorComponent>(e);
            check("I6_anim_fields",
                  anI && std::fabs(anI->speed - 2.5f) < 1e-5f &&
                      std::fabs(anI->time - 0.5f) < 1e-5f &&
                      std::fabs(anI->eventPrev - 0.5f) < 1e-5f && !anI->playing,
                  anI ? ("speed=" + std::to_string(anI->speed) +
                          " time=" + std::to_string(anI->time) +
                          " eventPrev=" + std::to_string(anI->eventPrev) +
                          " playing=" + (anI->playing ? "1" : "0"))
                       : "no animator");
            check("I7_anim_clip_name", clip == "Idle", "clip=" + clip);

            // ⑥ 形态键：权重落到组件上，坏名字被拒
            const auto* mpI = sc.world().get<ecs::MorphComponent>(e);
            check("I8_morph_weight", mpI && mpI->weights.size() == 1 &&
                  std::fabs(mpI->weights[0] - 0.75f) < 1e-5f && morphW == "0.75",
                  "w=" + (mpI && !mpI->weights.empty()
                              ? std::to_string(mpI->weights[0])
                              : std::string("?")) + " ret=" + morphW);
            check("I9_bad_channel_rejected", badCh == "false",
                  "pcall 应该失败（通道名拼错），实际 badChannelOk=" + badCh);
        }
    }

    // ---- J 句柄 / 参数个数 / 空句柄 ----
    //
    // 这三类都是"新建场景里一次都复现不出来"的静默失效：
    //   ① 句柄是 `(generation<<32)|id` 的 64 位整数，而取参数时曾经走
    //      `aInt`（32 位）→ generation 被整段截掉。只有"销毁过一个实体、
    //      槽位被复用"之后才错：`engine.valid()` 恒假、`setParent` 报
    //      "not alive"、`game.joint` 挂到旧句柄上。
    //   ② `ApiTable` 的 `minArgs/maxArgs` 曾经只写在表里、**注册时没人查**。
    //   ③ 空句柄曾经与"0 号实体"共用数字 0 → `setParent(kid, 0)`
    //      静默把子节点挂到 0 号实体（常常就是玩家自己）身上。
    {
        scene::Scene sc;
        const ecs::Entity me = makeProbe(sc, "ProbeJ");

        // ① 造"槽位复用"现场：销毁后立刻再建，id 从空闲表里拿回来、gen+1。
        const ecs::Entity victim = makeProbe(sc, "VictimJ");
        sc.world().destroy(victim);
        const ecs::Entity reused = makeProbe(sc, "ReusedJ");
        const bool slotReused = (reused.id == victim.id) &&
                                (reused.generation == victim.generation + 1);

        // 与 `LuaBackend.cpp` 的 `packEntity` 同一编码（含 +1）
        auto packOf = [](ecs::Entity x) {
            return ((uint64_t(x.generation) << 32) | uint64_t(x.id)) + 1ull;
        };
        auto unpackOf = [](uint64_t v) {
            ecs::Entity x;
            const uint64_t raw = v - 1ull;
            x.id = ecs::EntityId(raw & 0xFFFFFFFFull);
            x.generation = uint32_t(raw >> 32);
            return x;
        };
        const unsigned long long oldH = packOf(victim);   // 销毁前的句柄
        const unsigned long long newH = packOf(reused);   // 复用后的句柄

        // ② 定焦镜头：`camera.fov` 读回实际生效值、写 fov 必须清 focal
        ecs::CameraComponent cam;
        cam.focal = 35.0f;
        cam.sensorHeight = 24.0f;
        cam.fov = 45.0f;
        sc.world().add<ecs::CameraComponent>(me, cam);

        // ③ 一个**没有** NameComponent 的实体（改名后 `world.find` 必须能找到）
        const ecs::Entity nameless = sc.world().create("NamelessJ");
        sc.world().add<ecs::TransformComponent>(nameless);
        const bool hadNoName = !sc.world().has<ecs::NameComponent>(nameless);
        const unsigned long long namelessH = packOf(nameless);

        char src[6144];
        std::snprintf(src, sizeof(src), R"LUA(
local M = {}
function M:BeginPlay()
    local me = engine.self()
    -- ⚠ 这条放在**最前面**：本夹具里 `me` 就是 0 号实体（`id=0, gen=0`），
    --   没有 `+1` 时它的句柄正好是 0，会被当成空句柄。放在后面的话，
    --   脚本会在中途 `world.children` 那行先崩，这条只能读到空值 ——
    --   看上去红了，却不是它自己抓到的。
    self.zeroSelf  = (me ~= 0)
    self.oldValid  = engine.valid(%llu)   -- 已销毁的旧句柄 → false
    self.newValid  = engine.valid(%llu)   -- 复用槽位的新句柄 → true
    self.selfValid = engine.valid(me)     -- 自己 → true

    local kid = world.spawn("models/box.vmodel", engine.vec3(0, 0, 1))
    self.kidHandle = kid
    world.setParent(kid, me)
    world.setParent(kid, 0)               -- 0 = 空句柄 → 拔到根
    self.stillParented = #world.children(me)
    self.parentHandle  = world.parent(kid)

    -- 参数个数：少给 / 多给都必须当场报错，而不是静默忽略
    self.arityFew  = pcall(function() world.setPos(me) end)
    self.arityMany = pcall(function() world.pos(me, 1, 2) end)
    self.arityOk   = pcall(function() world.pos(me) end)

    self.fovFocal = camera.fov(me)        -- 定焦 35 mm → 由焦距算出的视场角
    self.fovMatch = math.abs(self.fovFocal - %f) < 1e-3
    camera.fov(me, 70)                    -- 写 fov → 必须清掉 focal
    self.fovAfter = camera.fov(me)

    world.setName(%llu, "RenamedJ")       -- 原本没有 NameComponent
    self.foundRenamed = engine.valid(world.find("RenamedJ"))

    -- `0` 是**唯一**空句柄：它必须被判空，`find` 找不到也必须返回它。
    -- J3 只证明"传 0 能拔根"，那是承诺的一半；另一半是"0 不被当成实体"。
    self.zeroValid  = engine.valid(0)
    self.missHandle = world.find("no_such_entity_J")
    self.missValid  = engine.valid(self.missHandle)
end
return M
)LUA", oldH, newH,
            double(scene::focalToFovY(35.0f, 24.0f)), namelessH);

        LuaBackend be;
        auto prog = be.compile("t_handle.lua", src);
        auto inst = be.createInstance(prog, me);
        check("J0_handle_script_compiled", inst != nullptr, prog ? prog->error() : "null");
        if (inst) {
            ScriptHost host;
            host.scene = &sc;
            host.entity = me;
            host.act.spawn = [&sc](const std::string&, glm::vec3) {
                return sc.createObject("SpawnedJ");
            };
            const bool ok = inst->beginPlay(host);

            std::string oldV, newV, selfV, stillP, parentH, few, many, arityOk,
                        fovMatch, fovAfter, foundRenamed, kidH;
            std::string zeroV, missH, missV, zeroSelf;
            for (auto& kv : inst->snapshotVars()) {
                if (kv.first == "oldValid") oldV = kv.second;
                if (kv.first == "newValid") newV = kv.second;
                if (kv.first == "selfValid") selfV = kv.second;
                if (kv.first == "stillParented") stillP = kv.second;
                if (kv.first == "parentHandle") parentH = kv.second;
                if (kv.first == "arityFew") few = kv.second;
                if (kv.first == "arityMany") many = kv.second;
                if (kv.first == "arityOk") arityOk = kv.second;
                if (kv.first == "fovMatch") fovMatch = kv.second;
                if (kv.first == "fovAfter") fovAfter = kv.second;
                if (kv.first == "foundRenamed") foundRenamed = kv.second;
                if (kv.first == "kidHandle") kidH = kv.second;
                if (kv.first == "zeroValid") zeroV = kv.second;
                if (kv.first == "missHandle") missH = kv.second;
                if (kv.first == "missValid") missV = kv.second;
                if (kv.first == "zeroSelf") zeroSelf = kv.second;
            }

            check("J1_slot_reused", slotReused,
                  "夹具前提：销毁后重建必须复用同一 id（否则这条测不到 gen）");
            check("J2_handle_generation",
                  ok && oldV == "false" && newV == "true" && selfV == "true",
                  "old=" + oldV + " new=" + newV + " self=" + selfV +
                      " err=" + inst->lastError());

            // ③ `setParent(kid, 0)` 之后：children 空、父 = 无效、且 kid 还活着
            const ecs::Entity kid = unpackOf(std::strtoull(kidH.c_str(), nullptr, 10));
            const auto* kh = sc.world().get<ecs::HierarchyComponent>(kid);
            const bool detached = sc.world().alive(kid) && kh &&
                                  kh->parent == ecs::kInvalidEntity;
            check("J3_zero_handle_detaches",
                  stillP == "0" && parentH == "0" && detached,
                  "children=" + stillP + " parent=" + parentH +
                      " hc=" + std::to_string(kh ? kh->parent : 0u));

            check("J4_arity_enforced",
                  few == "false" && many == "false" && arityOk == "true",
                  "few=" + few + " many=" + many + " ok=" + arityOk);

            const auto* camJ = sc.world().get<ecs::CameraComponent>(me);
            // 快照里的数字是 `luaL_tolstring` 的文本（"70.0"），按数值比更稳
            const double fovAfterNum = std::strtod(fovAfter.c_str(), nullptr);
            check("J5_camera_fov_effective",
                  fovMatch == "true" && camJ && std::fabs(camJ->fov - 70.0f) < 1e-4f &&
                      camJ->focal == 0.0f && std::fabs(fovAfterNum - 70.0) < 1e-4,
                  "readMatch=" + fovMatch + " after=" + fovAfter +
                      " compFov=" + (camJ ? std::to_string(camJ->fov) : std::string("?")) +
                      " focal=" + (camJ ? std::to_string(camJ->focal) : std::string("?")));

            check("J6_rename_adds_name_component",
                  hadNoName && foundRenamed == "true" &&
                      sc.find("RenamedJ").valid(),
                  "hadNoName=" + std::string(hadNoName ? "1" : "0") +
                      " found=" + foundRenamed);

            // ⑥ 空句柄的**另一半**承诺：`0` 被判空、`find` 找不到也返回 0。
            //    这条守的是 `unpackEntity` 里的显式空分支（`if (v == 0)`）：
            //    删掉它，`0` 会被解成"0 号实体"，`valid(0)` 立刻变真。
            //    ⚠ 它在"有 +1 / 没 +1"两种实现下**都成立**，抓不住"撤掉 +1"
            //      （那条由 J8 管），落点只对"空分支被删"这一种改动敏感。
            check("J7_zero_is_null",
                  zeroV == "false" && missH == "0" && missV == "false",
                  "valid(0)=" + zeroV + " find(missing)=" + missH +
                      " valid(missing)=" + missV);

            // ⑦ 上一条在"有 +1 / 没 +1"两种实现下**都成立**（`unpack(0)`
            //    里有显式的空分支兜着），所以它抓不住"`+1` 被撤掉"。
            //    真正区分两种实现的是这一条：**0 号实体自己的句柄不许是 0**。
            //    实测（2026-11-03 否定测试）：撤掉 `+1` 后本组 17 条红，
            //    第一条红是 J2（`world.children` 报 `entity #-1 is not alive`
            //    —— 0 号实体的句柄 0 被当成空，解出来 id=0xFFFFFFFF）。
            check("J8_zero_entity_handle_nonzero",
                  me.id == 0 && zeroSelf == "true",
                  "夹具前提 me.id=" + std::to_string(me.id) +
                      "（必须是 0 号实体，否则这条测不到）· self()~=0 -> " +
                      zeroSelf);
        }
    }

    std::printf("LUA-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}

// ============================================================
// 文档生成：`Game.exe --selftest api`
//
// 为什么由**代码**生成而不是手写：86 条 API 手写一份 markdown，改三条就
// 开始漂移，而"文档说 `world.pos(e)` 返回 vec3、代码里返回表"这种错
// 只有使用者会踩到。表在 `ApiTable.cpp`，这里只负责排版。
// ============================================================
namespace {

struct NsInfo {
    const char* ns;
    const char* title;
};

const NsInfo kNsInfo[] = {
    {"engine", "自身 / 值 / 时间 / 日志"},
    {"world", "场景查询与变换"},
    {"actor", "实体行为（可见 / 刚体 / 材质）"},
    {"input", "动作输入"},
    {"anim", "动画"},
    {"nav", "导航"},
    {"fx", "特效与镜头"},
    {"audio", "音频"},
    {"light", "灯光"},
    {"camera", "相机（FOV / 臂长 / 机位开关）"},
    {"ui", "HUD / 对话 / 过场 / 界面"},
    {"game", "玩法数据（属性 / 状态 / 配表 / 卡牌 / 旗标 / 关节）"},
};

const char* nsTitle(const char* ns) {
    for (const NsInfo& i : kNsInfo)
        if (std::strcmp(i.ns, ns) == 0) return i.title;
    return "";
}

// "entity, vec3 -> void" → args = "entity, vec3", ret = "void"
void splitSig(const char* sig, std::string* args, std::string* ret) {
    const std::string s = sig ? sig : "";
    const std::size_t p = s.find(" -> ");
    if (p == std::string::npos) {
        *args = s;
        *ret = "";
        return;
    }
    *args = s.substr(0, p);
    *ret = s.substr(p + 4);
}

} // namespace

void dumpApiMarkdown() {
    const ApiEntry* es = apiEntries();
    const int n = apiEntryCount();

    // 命名空间顺序 = 表里第一次出现的顺序（改表即改文档顺序）
    std::vector<std::string> order;
    for (int i = 0; i < n; ++i) {
        const std::string ns = es[i].ns;
        bool seen = false;
        for (const std::string& s : order)
            if (s == ns) { seen = true; break; }
        if (!seen) order.push_back(ns);
    }

    std::printf("# Lua 游戏逻辑 API · 完整参考\n\n");
    std::printf("> 本文件由 `Game.exe --selftest api` **自动生成**，"
                "源数据是 `engine/script/ApiTable.cpp`。\n");
    std::printf("> **不要手改这一份** —— 改表后重跑，否则下次生成会被覆盖。\n\n");
    std::printf("共 **%d** 个函数，分布在 **%d** 个命名空间。"
                "概念与用法见 `docs/script_api.md`。\n\n", n, int(order.size()));

    std::printf("| 命名空间 | 函数数 | 作用 |\n|---|---|---|\n");
    for (const std::string& ns : order) {
        int c = 0;
        for (int i = 0; i < n; ++i)
            if (ns == es[i].ns) ++c;
        std::printf("| `%s` | %d | %s |\n", ns.c_str(), c, nsTitle(ns.c_str()));
    }
    std::printf("\n");

    for (const std::string& ns : order) {
        int c = 0;
        for (int i = 0; i < n; ++i)
            if (ns == es[i].ns) ++c;
        std::printf("## `%s` · %s（%d）\n\n", ns.c_str(), nsTitle(ns.c_str()), c);
        std::printf("| 函数 | 返回 | 说明 | 注意 |\n|---|---|---|---|\n");
        for (int i = 0; i < n; ++i) {
            if (ns != es[i].ns) continue;
            std::string args, ret;
            splitSig(es[i].sig, &args, &ret);
            std::printf("| `%s.%s(%s)` | `%s` | %s | %s |\n",
                        es[i].ns, es[i].name, args.c_str(), ret.c_str(),
                        es[i].doc, (es[i].note && es[i].note[0]) ? es[i].note : "");
        }
        std::printf("\n");
    }
}

} // namespace vks
