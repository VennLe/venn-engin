// ============================================================
// script/LuaBackend —— Lua 5.4 后端实现
//
// 设计理由见 LuaBackend.h 的四条。这里只记**实现上会咬人的地方**：
//
// ① 绑定函数用 **upvalue 拿 backend**，不用 `lua_getextraspace`。
//    extraspace 是**每个线程独立**的，而脚本方法跑在协程（`lua_newthread`）
//    里 —— 在协程里取 extraspace 会拿到 NULL。
//
// ② 熔断的 count hook 拿不到 upvalue（hook 签名是 `(lua_State*, lua_Debug*)`），
//    所以用**文件内静态指针** `g_be`。成立的前提是"进程内只有一个 LuaBackend"
//    —— 这正是"一个 lua_State"决策的副产品（见 LuaBackend.h ①）。
//
// ③ `lua_yield` 之后栈上是 resume 传进来的参数。`engine.wait` 靠它把
//    "剩余时间"还给脚本（`local left = engine.wait(2.0)`）。
//
// ④ 模板表（脚本 `return` 的那张表）存在 registry 里，**不做 unref**：
//    它的生命周期由 `LuaBackend::m_keepAlive` 兜到 `lua_close`。
//    代价是热重载每改一次多一张表（几 KB）；换来的是"实例的 __index
//    永远指向一个活着的表"，不会出现悬空引用。
// ============================================================

#include "script/LuaBackend.h"

#include "core/Logger.h"
#include "animation/ExpressionDriver.h"
#include "ecs/Components.h"
#include "ecs/EntityHandle.h"
#include "scene/CameraMove.h"
#include "scene/Scene.h"
#include "assets/Material.h"
#include "assets/Model.h"
#include "script/ApiTable.h"
#include "script/ScriptEngine.h"

#include <lua.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vks {
namespace {

// hook 每多少条 VM 指令触发一次。1000 是"够粗不拖慢、够细不失控"的折中：
// 20000 步预算下最多多出 20 次 hook 调用。
constexpr int kHookCount = 1000;

LuaBackend* g_be = nullptr;  // 见文件头 ②
int g_steps = 0;
int g_budget = 20000;

// 熔断钩子：每 `kHookCount` 条 VM 指令进一次，超预算就抛 Lua 错误。
//
// ⚠ 它必须在**每一段"跑的是作者代码"的地方**都装上，不只是 `Tick`：
//   顶层 chunk（`compile()` 里的 `lua_pcall`）和 `waitUntil` 的条件函数
//   同样是作者写的东西 —— 一个 `while true do end` 写在顶层，曾经能把
//   游戏直接冻住（钩子只在 `doResume` 里装，那两处一次都没装过）。
void luaBudgetHook(lua_State* L, lua_Debug*) {
    if (++g_steps <= g_budget) return;
    luaL_error(L, "budget exceeded (%d steps) — check for an infinite loop", g_budget);
}

// 装 / 摘预算。
// ⚠ **不用 RAII**：`luaL_error` 是 longjmp，栈上带析构的对象不会被析构 ——
//   守卫的析构函数跑不到，钩子就永远留在 state 上，之后每一段代码都会
//   被上一轮的预算打断。所以一律"设 → pcall / resume → 立刻摘"。
void budgetAttach(lua_State* L, int budget) {
    g_steps = 0;
    g_budget = budget;
    lua_sethook(L, &luaBudgetHook, LUA_MASKCOUNT, kHookCount);
}
void budgetDetach(lua_State* L) {
    lua_sethook(L, nullptr, 0, 0);
}

// ---------------------------------------------------------------- 句柄
//
// Lua 5.4 的 integer 是 64 位 → `{id, generation}` 打包成一个数字，
// 脚本里可比较、可作表键、可存数组，不用造 userdata。
// ⚠ generation 那一半**不能丢**（丢了就是"实体被删后句柄复用，脚本静默
//   改到别人身上"，`.vks` 的 Value::K::Entity 注释里记过这个坑）。
//
// ⚠⚠ 编码（`+1`、空句柄 = 0、以及"为什么"）**已经搬到**
//   `ecs/EntityHandle.h` —— 因为 C++ 游戏模块（`engine/game/`）与游戏
//   工程本身也要用同一套编码，各写一份必然漂移。这里只做转发，
//   留住旧名字是为了不动下面 4 个调用点。
inline uint64_t packEntity(ecs::Entity e) { return ecs::packHandle(e); }
inline ecs::Entity unpackEntity(uint64_t v) { return ecs::unpackHandle(v); }

inline LuaBackend* backendOf(lua_State* L) {
    return static_cast<LuaBackend*>(lua_touserdata(L, lua_upvalueindex(1)));
}
inline ScriptHost* hostOf(lua_State* L) { return backendOf(L)->host(); }

// ---------------------------------------------------------------- vec3
//
// 表形式 `{x=, y=, z=}` 是**唯一**的 vec3 表示。理由：脚本里最常写 `p.x`，
// 三返回值形式（`local x,y,z = pos(e)`）读起来顺但写回去别扭（要再拼一个）。
// 热路径想省 table 的话走 `world.posX/Y/Z`（阶段 C 补）。
void pushVec3(lua_State* L, const glm::vec3& v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z); lua_setfield(L, -2, "z");
}

glm::vec3 checkVec3(lua_State* L, int idx) {
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    if (lua_istable(L, idx)) {
        lua_getfield(L, idx, "x"); const float x = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, idx, "y"); const float y = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_getfield(L, idx, "z"); const float z = float(lua_tonumber(L, -1)); lua_pop(L, 1);
        return glm::vec3(x, y, z);
    }
    luaL_error(L, "expected vec3 table {x=,y=,z=}");
    return glm::vec3(0.0f);  // 到不了（luaL_error 是 longjmp）
}

// ---------------------------------------------------------------- 世界写位姿
//
// 与 `ScriptEngine.cpp` 的 setWorldPos **逐条等价**：有父节点时先反解局部矩阵，
// 否则直接写 position。两处必须同步改 —— 这是"一套语义两个后端"的代价，
// 阶段 C 会把它抽成 `ScriptWorld::setWorldPos` 共用。
void setWorldPosImpl(ScriptHost* h, ecs::Entity e, const glm::vec3& p) {
    if (!h || !h->scene) return;
    // 实现在 `Scene::setWorldPos` —— C++ 游戏模块的 `entitySetPos` 也走
    // 同一条，两份实现漂移的话只在"有父节点"时才看出来。
    h->scene->setWorldPos(e, p);
}

// 拿实体并校验；失败直接 luaL_error（与 `.vks` 的 `alive()` 同一条纪律：
// 对死句柄静默返回 0 会让"东西被搬到原点"这种 bug 离现场很远）
ecs::Entity needEntity(lua_State* L, int idx, const char* what) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->scene) {
        luaL_error(L, "%s: no scene bound", what);
    }
    const ecs::Entity e = unpackEntity(uint64_t(luaL_checkinteger(L, idx)));
    if (!h->scene->world().alive(e)) {
        luaL_error(L, "%s: entity #%d is not alive", what,
                   unsigned(e.id));
    }
    return e;
}

// ---------------------------------------------------------------- 参数取样
//
// 每个绑定都从这里取参数，不再各写一遍 `luaL_checknumber(L, 1)`：
// 参数个数由 `ApiTable` 在注册时校验过，这里只管取值。
inline float aNum(lua_State* L, int i) { return float(luaL_checknumber(L, i)); }
inline int aInt(lua_State* L, int i) { return int(luaL_checkinteger(L, i)); }

// 实体句柄**必须**走这个，不能走 `aInt`。
//
// ⚠⚠ 句柄是 `(generation << 32) | id` 的 **64 位**整数，`int` 只有 32 位 ——
//   走 `aInt` 等于把 generation 整段截掉：
//     · `engine.valid(e)` 恒 false（槽位被复用过一次之后，gen 就不是 0 了）；
//     · `world.setParent(child, p)` 报 "parent handle is not alive"；
//     · `game.joint(a, b, ...)` 把 b 挂到 generation 0 的**旧**句柄上。
//   这三处**都只错在 gen≠0 时**，而 gen≠0 要"先销毁再创建"才出现 ——
//   新建场景里一次都复现不出来，是典型的"上线才炸"。
inline uint64_t aHandle(lua_State* L, int i) {
    return static_cast<uint64_t>(luaL_checkinteger(L, i));
}

inline bool aBool(lua_State* L, int i) {
    luaL_checkany(L, i);
    return lua_toboolean(L, i) != 0;
}
inline const char* aStr(lua_State* L, int i) { return luaL_checkstring(L, i); }
inline glm::vec3 aVec(lua_State* L, int i) { return checkVec3(L, i); }

scene::Scene* sceneOf(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->scene) luaL_error(L, "no scene bound (self-test / edit mode?)");
    return h->scene;
}

ecs::Entity selfOf(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) luaL_error(L, "no host");
    return h->entity;
}

// 宿主动作缺失 = 这一局没有该能力 → **报错**。
// 静默返回成功会让"我写了怎么没反应"变成玄学（与 `.vks` 的 noBody() 同一条纪律）。
template <typename F>
void needAction(lua_State* L, const F& f, const char* what) {
    if (!f) luaL_error(L, "%s: not available (host did not install this action)", what);
}

// 挂载实体身上的点光 / 聚光（两者取其一）
template <typename Comp>
Comp* compOf(scene::Scene* sc, ecs::Entity e) {
    return sc ? sc->world().get<Comp>(e) : nullptr;
}

// 递归销毁（子节点跟着走）。`World::destroy` 只删一个实体。
//
// ⚠ 必须**先把自己从父节点的 `children` 里摘掉**再 destroy：
//   `World::destroy` 不碰别人的 `HierarchyComponent`，所以父节点的
//   `children` 会留着一个已销毁实体的 id。那个 id 之后会被 `m_free` 复用给
//   新实体 —— 于是 `Scene::setParent` 摘链时按 id 比较，会把**另一个实体**
//   的登记项删掉（层级树里少一行，或者某个物体死活挂不上父节点）。
//   `children` 是序列化/编辑器层级树的输入，不是缓存，不能留悬空值。
void destroyRecursive(scene::Scene& sc, ecs::Entity e) {
    if (!sc.world().alive(e)) return;
    std::vector<ecs::Entity> kids;
    sc.world().each<ecs::HierarchyComponent>(
        [&](ecs::Entity c, ecs::HierarchyComponent& hc) {
            if (hc.parent == e.id) kids.push_back(c);
        });
    for (ecs::Entity k : kids) destroyRecursive(sc, k);
    if (sc.world().alive(e)) {
        sc.setParent(e, ecs::Entity{});  // 摘链（父节点的 children 也要干净）
        sc.world().destroy(e);
    }
}

// ---------------------------------------------------------------- engine
int l_engine_self(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "self(): no host");
    lua_pushinteger(L, lua_Integer(packEntity(h->entity)));
    return 1;
}

int l_engine_valid(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->scene) { lua_pushboolean(L, 0); return 1; }
    const ecs::Entity e = unpackEntity(aHandle(L, 1));
    lua_pushboolean(L, h->scene->world().alive(e) ? 1 : 0);
    return 1;
}

// `exists` 是 `valid` 的语义化别名：文档里两个都写，实现只有一份。
int l_engine_exists(lua_State* L) { return l_engine_valid(L); }

int l_engine_name(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "engine.name");
    const std::string* n = sc->world().name(e);
    lua_pushstring(L, n ? n->c_str() : "");
    return 1;
}

int l_engine_vec3(lua_State* L) {
    pushVec3(L, glm::vec3(aNum(L, 1), aNum(L, 2), aNum(L, 3)));
    return 1;
}

int l_engine_dist(lua_State* L) {
    const glm::vec3 a = aVec(L, 1), b = aVec(L, 2);
    lua_pushnumber(L, double(glm::length(a - b)));
    return 1;
}

int l_engine_clamp(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    const double lo = luaL_checknumber(L, 2);
    const double hi = luaL_checknumber(L, 3);
    lua_pushnumber(L, v < lo ? lo : (v > hi ? hi : v));
    return 1;
}

int l_engine_lerp(lua_State* L) {
    const double a = luaL_checknumber(L, 1);
    const double b = luaL_checknumber(L, 2);
    const double t = luaL_checknumber(L, 3);
    lua_pushnumber(L, a + (b - a) * t);
    return 1;
}

int l_engine_log(lua_State* L) {
    std::string s;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* p = luaL_tolstring(L, i, &len);
        if (i > 1) s += ' ';
        if (p) s.append(p, len);
        lua_pop(L, 1);  // luaL_tolstring 压的那一份
    }
    VK_LOG_INFO("LUA-PRINT %s", s.c_str());
    return 0;
}

int l_engine_warn(lua_State* L) {
    VK_LOG_WARN("LUA-WARN %s", aStr(L, 1));
    return 0;
}

// `engine.wait` / `waitFrames` / `waitUntil` 都是"把意图寄存到 backend，
// 然后 yield"。实例 resume 时按 `takePending*` 决定还等不等。
int l_engine_wait(lua_State* L) {
    LuaBackend* be = backendOf(L);
    const double sec = luaL_checknumber(L, 1);
    if (sec < 0.0) return luaL_error(L, "wait(seconds): must be >= 0");
    be->setPendingWait(sec);
    return lua_yield(L, 0);
}

int l_engine_waitFrames(lua_State* L) {
    LuaBackend* be = backendOf(L);
    const int n = aInt(L, 1);
    if (n < 0) return luaL_error(L, "waitFrames(n): must be >= 0");
    be->setPendingWait(-double(n));  // 负数 = 按帧计（见 LuaBackend.h 的语义）
    return lua_yield(L, 0);
}

int l_engine_waitUntil(lua_State* L) {
    LuaBackend* be = backendOf(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    be->setPendingUntil(luaL_ref(L, LUA_REGISTRYINDEX));
    return lua_yield(L, 0);
}

// ---------------------------------------------------------------- world
int l_world_pos(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.pos");
    pushVec3(L, glm::vec3(sc->worldMatrix(e)[3]));
    return 1;
}

int l_world_setPos(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.setPos");
    const glm::vec3 p = aVec(L, 2);
    if (glm::any(glm::isnan(p))) return luaL_error(L, "setPos: NaN position");
    setWorldPosImpl(hostOf(L), e, p);
    (void)sc;
    return 0;
}

int l_world_rot(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.rot");
    const auto* t = compOf<ecs::TransformComponent>(sc, e);
    pushVec3(L, t ? t->rotation : glm::vec3(0.0f));
    return 1;
}

int l_world_setRot(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.setRot");
    auto* t = sc->world().get<ecs::TransformComponent>(e);
    if (!t) return luaL_error(L, "world.setRot: entity #%d has no transform", unsigned(e.id));
    t->rotation = aVec(L, 2);  // **弧度**
    return 0;
}

int l_world_rotY(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.rotY");
    const auto* t = compOf<ecs::TransformComponent>(sc, e);
    lua_pushnumber(L, t ? double(t->rotation.z) : 0.0);
    return 1;
}

int l_world_scale(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.scale");
    const auto* t = compOf<ecs::TransformComponent>(sc, e);
    pushVec3(L, t ? t->scale : glm::vec3(1.0f));
    return 1;
}

int l_world_setScale(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.setScale");
    auto* t = sc->world().get<ecs::TransformComponent>(e);
    if (!t) return luaL_error(L, "world.setScale: entity #%d has no transform", unsigned(e.id));
    t->scale = aVec(L, 2);
    return 0;
}

int l_world_moveBy(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.moveBy");
    auto* t = sc->world().get<ecs::TransformComponent>(e);
    if (!t) return luaL_error(L, "world.moveBy: entity #%d has no transform", unsigned(e.id));
    t->position += aVec(L, 2);
    return 0;
}

int l_world_near(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const glm::vec3 c = aVec(L, 1);
    const float r = aNum(L, 2);
    const int limit = (lua_gettop(L) >= 3) ? aInt(L, 3) : 0;

    struct Hit { float d; ecs::Entity e; };
    std::vector<Hit> hits;
    sc->world().each<ecs::TransformComponent>(
        [&](ecs::Entity e, ecs::TransformComponent&) {
            const glm::vec3 p(sc->worldMatrix(e)[3]);
            const glm::vec3 diff = p - c;
            const float d2 = glm::dot(diff, diff);
            if (d2 <= r * r) hits.push_back(Hit{std::sqrt(d2), e});
        });
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.d != b.d) return a.d < b.d;
        return a.e.id < b.e.id;  // 同距离按 id 定序 → 确定性
    });
    if (limit > 0 && int(hits.size()) > limit) hits.resize(size_t(limit));

    lua_createtable(L, int(hits.size()), 0);
    for (size_t i = 0; i < hits.size(); ++i) {
        lua_pushinteger(L, lua_Integer(packEntity(hits[i].e)));
        lua_rawseti(L, -2, int(i) + 1);  // 1 基下标
    }
    return 1;
}

int l_world_find(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = sc->find(aStr(L, 1));
    lua_pushinteger(L, lua_Integer(packEntity(e)));
    return 1;
}

int l_world_count(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    int n = 0;
    sc->world().each<ecs::TransformComponent>([&](ecs::Entity, ecs::TransformComponent&) { ++n; });
    lua_pushinteger(L, n);
    return 1;
}

int l_world_spawn(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "world.spawn: no host");
    needAction(L, h->act.spawn, "world.spawn");
    const char* path = aStr(L, 1);
    glm::vec3 p(0.0f);
    if (lua_gettop(L) >= 2) p = aVec(L, 2);
    const ecs::Entity e = h->act.spawn(path, p);
    lua_pushinteger(L, lua_Integer(packEntity(e)));
    return 1;
}

int l_world_despawn(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.despawn");
    const ecs::Entity self = selfOf(L);
    if (e == self && !sc->world().alive(e)) return 0;
    destroyRecursive(*sc, e);
    return 0;
}

int l_world_loadScene(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "world.loadScene: no host");
    needAction(L, h->act.loadScene, "world.loadScene");
    lua_pushboolean(L, h->act.loadScene(aStr(L, 1)) ? 1 : 0);
    return 1;
}

// ---- 命名 / 层级（Name / Hierarchy）----
//
// `HierarchyComponent` 之前没有一条 API：脚本能 `spawn` 能 `despawn`，
// 但**挂不上**。下面这四个把"父子"补齐。
int l_world_setName(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.setName");
    const char* name = aStr(L, 2);
    // ⚠⚠ 名字有**两处存储**：`World::Record::name`（`World::name()` 读它，
    //   `world.find` / `engine.name` 都走这条）和 `NameComponent::name`
    //   （序列化与 Inspector 读它）。只写组件的话，脚本改完名字，
    //   `engine.name(e)` 与 `world.find` 还是旧值 —— 而且**不报错**。
    //   `World::setName` 是唯一同时写两处的入口，必须走它。
    sc->world().setName(e, name);
    return 0;
}

int l_world_parent(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.parent");
    const auto* hc = sc->world().get<ecs::HierarchyComponent>(e);
    ecs::Entity p{};  // 默认无效句柄 = "我是根"，这是查询的正常结果，不报错
    if (hc && hc->parent != ecs::kInvalidEntity) p = sc->world().handle(hc->parent);
    lua_pushinteger(L, lua_Integer(packEntity(p)));
    return 1;
}

int l_world_children(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.children");
    std::vector<ecs::Entity> kids;
    sc->world().each<ecs::HierarchyComponent>(
        [&](ecs::Entity c, ecs::HierarchyComponent& hc) {
            if (hc.parent == e.id) kids.push_back(c);
        });
    lua_createtable(L, int(kids.size()), 0);
    for (size_t i = 0; i < kids.size(); ++i) {
        lua_pushinteger(L, lua_Integer(packEntity(kids[i])));
        lua_rawseti(L, -2, int(i) + 1);  // 1 基下标，与 world.near 一致
    }
    return 1;
}

// `p` 是不是 `e` 自己或 `e` 的后代 —— 用来挡成环。
//
// ⚠ 必须挡：环形父子关系让 `Scene::worldMatrix` 沿父链无限乘下去
//   （那里有 128 层 guard，但结果是位置变成一个无意义的值），
//   而序列化**能**把环写进文件，脚本同样能造出来。
static bool isSelfOrDescendant(scene::Scene& sc, ecs::Entity p, ecs::Entity e) {
    ecs::Entity cur = p;
    int guard = 0;
    while (cur.valid() && guard++ < 256) {
        if (cur.id == e.id) return true;
        const auto* hc = sc.world().get<ecs::HierarchyComponent>(cur);
        if (!hc || hc->parent == ecs::kInvalidEntity) break;
        cur = sc.world().handle(hc->parent);
    }
    return false;
}

int l_world_setParent(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "world.setParent");
    ecs::Entity p{};  // 空句柄 = 拔到根
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        const uint64_t raw = aHandle(L, 2);
        // `nil` 和 `0` 都是"没有父节点"（见 `packEntity` 的 +1 编码：
        // 0 是**唯一**的空句柄，不再是"0 号实体"的别名）。
        if (raw != 0) {
            p = unpackEntity(raw);
            if (p.valid() && !sc->world().alive(p))
                return luaL_error(L, "world.setParent: parent handle is not alive");
            if (p.valid() && isSelfOrDescendant(*sc, p, e))
                return luaL_error(L,
                    "world.setParent: entity #%d cannot be parented under its own descendant",
                    unsigned(e.id));
        }
    }
    // `Scene::setParent` 只改局部变换（世界位置会跟着父节点跳）。
    // 不在这里补"保持世界位置"：那需要先算世界矩阵再反解，
    // 而脚本那边一句 `setPos(e, 记住的位置)` 更直白、也更省得猜。
    sc->setParent(e, p);
    return 0;
}

// ---------------------------------------------------------------- actor
int l_actor_visible(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "actor.visible");
    const auto* v = compOf<ecs::VisibilityComponent>(sc, e);
    lua_pushboolean(L, (v ? v->visible : true) ? 1 : 0);
    return 1;
}

int l_actor_setVisible(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    // 两种签名：`setVisible(e, false)` / `setVisible(false)`（对 self）
    if (lua_gettop(L) == 1 && lua_type(L, 1) == LUA_TBOOLEAN) {
        ecs::World& w = sc->world();
        const ecs::Entity self = selfOf(L);
        auto* v = w.get<ecs::VisibilityComponent>(self);
        if (!v) v = &w.add<ecs::VisibilityComponent>(self);
        v->visible = aBool(L, 1);
        return 0;
    }
    const ecs::Entity e = needEntity(L, 1, "actor.setVisible");
    ecs::World& w = sc->world();
    auto* v = w.get<ecs::VisibilityComponent>(e);
    if (!v) v = &w.add<ecs::VisibilityComponent>(e);
    v->visible = (lua_gettop(L) >= 2) ? aBool(L, 2) : true;
    return 0;
}

int l_actor_setShadow(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "actor.setShadow");
    ecs::World& w = sc->world();
    auto* v = w.get<ecs::VisibilityComponent>(e);
    if (!v) v = &w.add<ecs::VisibilityComponent>(e);
    v->castShadow = aBool(L, 2);
    return 0;
}

int l_actor_vel(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "actor.vel: no host");
    const ecs::Entity e = needEntity(L, 1, "actor.vel");
    needAction(L, h->act.bodyVel, "actor.vel");
    glm::vec3 v(0.0f);
    if (!h->act.bodyVel(e, &v))
        return luaL_error(L, "actor.vel: entity #%d has no rigid body", unsigned(e.id));
    pushVec3(L, v);
    return 1;
}

int l_actor_setVel(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "actor.setVel: no host");
    const ecs::Entity e = needEntity(L, 1, "actor.setVel");
    needAction(L, h->act.setBodyVel, "actor.setVel");
    h->act.setBodyVel(e, aVec(L, 2));
    return 0;
}

int l_actor_impulse(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "actor.impulse: no host");
    const ecs::Entity e = needEntity(L, 1, "actor.impulse");
    needAction(L, h->act.impulse, "actor.impulse");
    h->act.impulse(e, aVec(L, 2));
    return 0;
}

int l_actor_grounded(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "actor.grounded: no host");
    const ecs::Entity e = needEntity(L, 1, "actor.grounded");
    lua_pushboolean(L, (h->act.grounded && h->act.grounded(e)) ? 1 : 0);
    return 1;
}

int l_actor_setEmissive(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "actor.setEmissive");
    auto* mc = sc->world().get<ecs::MaterialComponent>(e);
    if (!mc || !mc->material)
        return luaL_error(L, "actor.setEmissive: entity #%d has no material", unsigned(e.id));
    mc->material->emissive = glm::vec3(aNum(L, 2), aNum(L, 3), aNum(L, 4));
    return 0;
}

// 材质底色。`setEmissive` 只做"发光"，受击闪白 / 中毒变绿 / 无敌闪烁都要改底色。
//
// ⚠ `MaterialComponent::material` 是**按名共享的资产指针**（见 MEMORY 里
//   "材质按名共享缓存"那条）—— 改它等于改所有用同一材质的实体。
//   这是引擎的既有语义，不是这里引入的；文档与 ApiTable 的 note 都写明了。
ecs::MaterialComponent* materialOf(lua_State* L, ecs::Entity e, const char* what) {
    scene::Scene* sc = sceneOf(L);
    auto* mc = sc->world().get<ecs::MaterialComponent>(e);
    if (!mc || !mc->material)
        luaL_error(L, "%s: entity #%d has no material", what, unsigned(e.id));
    return (mc && mc->material) ? mc : nullptr;
}

int l_actor_color(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "actor.color");
    auto* mc = materialOf(L, e, "actor.color");
    if (!mc) { lua_pushnil(L); return 1; }
    pushVec3(L, glm::vec3(mc->material->baseColorFactor));
    return 1;
}

int l_actor_setColor(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "actor.setColor");
    auto* mc = materialOf(L, e, "actor.setColor");
    if (!mc) return 0;
    // alpha 保持原值：这个 API 只承诺 RGB。要改透明度得连 alphaMode
    // 一起改（`Opaque` 下 alpha 写多少都看不见），那是材质面板的事。
    mc->material->baseColorFactor =
        glm::vec4(aNum(L, 2), aNum(L, 3), aNum(L, 4),
                  mc->material->baseColorFactor.a);
    return 0;
}

// ---------------------------------------------------------------- input
int l_input_pressed(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* a = aStr(L, 1);
    lua_pushboolean(L, (h && h->act.pressed && h->act.pressed(a)) ? 1 : 0);
    return 1;
}
int l_input_held(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* a = aStr(L, 1);
    lua_pushboolean(L, (h && h->act.held && h->act.held(a)) ? 1 : 0);
    return 1;
}
int l_input_released(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* a = aStr(L, 1);
    lua_pushboolean(L, (h && h->act.released && h->act.released(a)) ? 1 : 0);
    return 1;
}
int l_input_axis(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* a = aStr(L, 1);
    lua_pushnumber(L, (h && h->act.axis) ? double(h->act.axis(a)) : 0.0);
    return 1;
}

// ---------------------------------------------------------------- anim
int l_anim_play(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "anim.play: no host");
    const ecs::Entity e = needEntity(L, 1, "anim.play");
    needAction(L, h->act.playAnim, "anim.play");
    lua_pushboolean(L, h->act.playAnim(e, aStr(L, 2)) ? 1 : 0);
    return 1;
}

// ---- 播放控制 ----
//
// `anim.play` 只能"换一个片段从头放"。战斗要的是停在某一帧（受击定住）、
// 慢放（子弹时间）、跳到某秒（对齐演出）—— 下面这些补的就是这三类。
//
// ⚠⚠ **权威字段不是"有 AnimatorComponent 才有"**：`AnimatorSystem` 自己的
//   规则是 `an ? an->time : sk.time`（见它 tick 里那几个 `float&`）——
//   简单角色只挂 `SkinnedMeshComponent` 时，`time/speed/playing` 住在蒙皮
//   组件上。只认 Animator 的话，"能 `anim.play` 却 `anim.pause` 报错"，
//   而这两句在作者眼里是同一件事。
//   所以这里和引擎走同一条规则，两种组件都认。
struct AnimFields {
    ecs::AnimatorComponent* an = nullptr;
    ecs::SkinnedMeshComponent* sk = nullptr;
    bool ok() const { return an || sk; }
    bool playing() const { return an ? an->playing : sk->playing; }
    void setPlaying(bool v) { if (an) an->playing = v; else sk->playing = v; }
    float speed() const { return an ? an->speed : sk->speed; }
    void setSpeed(float v) { if (an) an->speed = v; else sk->speed = v; }
    float time() const { return an ? an->time : sk->time; }
    void setTime(float v) {
        if (an) {
            an->time = v;
            // ⚠ `eventPrev` 必须跟着跳：事件区间是**半开** `(eventPrev, time]`，
            //   只挪 time 不挪它的话，中间那些事件会在下一帧一次性全炸
            //   （"跳帧跳过一段剧情，结果所有音效同时响"）。
            an->eventPrev = v;
        } else {
            sk->time = v;  // 蒙皮组件没有事件轨，不用同步
        }
    }
};

AnimFields animFieldsOf(lua_State* L, ecs::Entity e, const char* what) {
    scene::Scene* sc = sceneOf(L);
    AnimFields f;
    f.an = sc->world().get<ecs::AnimatorComponent>(e);
    f.sk = sc->world().get<ecs::SkinnedMeshComponent>(e);
    if (!f.ok())
        luaL_error(L, "%s: entity #%d has neither AnimatorComponent nor SkinnedMeshComponent",
                   what, int(e.id));
    return f;
}

int l_anim_pause(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "anim.pause");
    AnimFields f = animFieldsOf(L, e, "anim.pause");
    if (f.ok()) f.setPlaying(false);
    return 0;
}

int l_anim_resume(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "anim.resume");
    AnimFields f = animFieldsOf(L, e, "anim.resume");
    if (f.ok()) f.setPlaying(true);
    return 0;
}

int l_anim_stop(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "anim.stop");
    AnimFields f = animFieldsOf(L, e, "anim.stop");
    if (!f.ok()) return 0;
    f.setPlaying(false);
    f.setTime(0.0f);
    if (f.an)
        for (auto& layer : f.an->layers) layer.time = 0.0f;
    return 0;
}

int l_anim_speed(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "anim.speed");
    AnimFields f = animFieldsOf(L, e, "anim.speed");
    if (!f.ok()) { lua_pushnumber(L, 1.0); return 1; }
    if (lua_gettop(L) >= 2) f.setSpeed(aNum(L, 2));
    lua_pushnumber(L, double(f.speed()));
    return 1;
}

int l_anim_time(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "anim.time");
    AnimFields f = animFieldsOf(L, e, "anim.time");
    if (!f.ok()) { lua_pushnumber(L, 0.0); return 1; }
    if (lua_gettop(L) >= 2) {
        const float t = aNum(L, 2);
        if (!(t >= 0.0f)) return luaL_error(L, "anim.time: must be >= 0");
        f.setTime(t);
    }
    lua_pushnumber(L, double(f.time()));
    return 1;
}

int l_anim_clip(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "anim.clip");
    // 单 clip 直放模式：真正在放的名字住在蒙皮组件上（AnimatorSystem 用
    // `sk.clipName` 找片段）。分层模式下才看 layer 0。
    if (const auto* sk = sc->world().get<ecs::SkinnedMeshComponent>(e)) {
        if (!sk->clipName.empty()) {
            lua_pushstring(L, sk->clipName.c_str());
            return 1;
        }
    }
    if (const auto* an = sc->world().get<ecs::AnimatorComponent>(e)) {
        if (!an->layers.empty() && !an->layers[0].playingClip.empty()) {
            lua_pushstring(L, an->layers[0].playingClip.c_str());
            return 1;
        }
        lua_pushstring(L, an->defaultClip.c_str());
        return 1;
    }
    lua_pushstring(L, "");
    return 1;
}

// 形态键（BlendShape）。通道名来自模型资产的 `MorphRig`，拼错**报错** ——
// 静默的话症状是"表情没变"，而日志一行都没有。
int l_anim_morph(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "anim.morph");
    const char* ch = aStr(L, 2);
    const float v = aNum(L, 3);
    auto* mp = sc->world().get<ecs::MorphComponent>(e);
    if (!mp || !mp->model)
        return luaL_error(L, "anim.morph: entity #%d has no morph rig", unsigned(e.id));
    const assets::MorphRig& rig = mp->model->morphRig;
    if (rig.empty())
        return luaL_error(L, "anim.morph: entity #%d model has no morph channels", unsigned(e.id));
    const int idx = rig.channelIndex(ch);
    if (idx < 0)
        return luaL_error(L, "anim.morph: unknown channel '%s'", ch);
    // 权重表可能比通道数短（组件默认是空表）→ 先补齐再写，
    // 否则 `weights[idx]` 是越界写（堆破坏，症状随机）。
    if (mp->weights.size() < rig.channelCount())
        mp->weights.resize(rig.channelCount(), 0.0f);
    mp->weights[size_t(idx)] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return 0;
}

int l_anim_morphWeight(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "anim.morphWeight");
    const char* ch = aStr(L, 2);
    const auto* mp = sc->world().get<ecs::MorphComponent>(e);
    if (!mp || !mp->model)
        return luaL_error(L, "anim.morphWeight: entity #%d has no morph rig", unsigned(e.id));
    const int idx = mp->model->morphRig.channelIndex(ch);
    if (idx < 0)
        return luaL_error(L, "anim.morphWeight: unknown channel '%s'", ch);
    const size_t i = size_t(idx);
    lua_pushnumber(L, i < mp->weights.size() ? double(mp->weights[i]) : 0.0);
    return 1;
}

// 表情预设。直接复用 `animation::setExpressionPreset`（编辑器面板走的是
// 同一个函数）—— 语义只有一份，不会"面板上能用、脚本里行为不一样"。
int l_anim_expression(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "anim.expression");
    const char* name = aStr(L, 2);
    std::string err;
    if (!anim::setExpressionPreset(sc->world(), e, name, &err)) {
        // 不静默：预设表没加载 / 名字不存在都只表现成"表情没变"，
        // 不打日志的话完全是玄学。
        VK_LOG_WARN("LUA-EXPR entity=%u preset='%s' failed: %s",
                    unsigned(e.id), name, err.c_str());
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ---------------------------------------------------------------- nav
int l_nav_goto(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "nav.goto: no host");
    const ecs::Entity e = needEntity(L, 1, "nav.goto");
    needAction(L, h->act.navGoto, "nav.goto");
    lua_pushboolean(L, h->act.navGoto(e, glm::vec3(aNum(L, 2), aNum(L, 3), aNum(L, 4))) ? 1 : 0);
    return 1;
}
int l_nav_arrived(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const ecs::Entity e = needEntity(L, 1, "nav.arrived");
    lua_pushboolean(L, (h && h->act.navArrived && h->act.navArrived(e)) ? 1 : 0);
    return 1;
}
int l_nav_failed(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const ecs::Entity e = needEntity(L, 1, "nav.failed");
    lua_pushboolean(L, (h && h->act.navFailed && h->act.navFailed(e)) ? 1 : 0);
    return 1;
}
int l_nav_stop(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const ecs::Entity e = needEntity(L, 1, "nav.stop");
    lua_pushboolean(L, (h && h->act.navStop && h->act.navStop(e)) ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------- fx
int l_fx_play(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.play: no host");
    const char* path = aStr(L, 1);
    uint32_t id = 0;
    if (lua_gettop(L) >= 4) {
        needAction(L, h->act.playEffectAt, "fx.play(path, x, y, z)");
        id = h->act.playEffectAt(path, glm::vec3(aNum(L, 2), aNum(L, 3), aNum(L, 4)));
    } else {
        needAction(L, h->act.playEffect, "fx.play(path)");
        id = h->act.playEffect(h->entity, path);
    }
    lua_pushinteger(L, lua_Integer(id));
    return 1;
}
int l_fx_stop(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.stop: no host");
    needAction(L, h->act.stopEffect, "fx.stop");
    lua_pushboolean(L, h->act.stopEffect(uint32_t(aInt(L, 1))) ? 1 : 0);
    return 1;
}
int l_fx_shake(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.shake: no host");
    needAction(L, h->act.shake, "fx.shake");
    h->act.shake(aNum(L, 1), h->entity);
    return 0;
}
int l_fx_hitstop(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.hitstop: no host");
    needAction(L, h->act.hitstop, "fx.hitstop");
    h->act.hitstop(aNum(L, 1));
    return 0;
}
int l_fx_timeScale(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "fx.timeScale: no host");
    needAction(L, h->act.setTimeScale, "fx.timeScale");
    const float v = aNum(L, 1);
    if (!(v >= 0.0f)) return luaL_error(L, "fx.timeScale: must be >= 0");
    h->act.setTimeScale(v);
    return 0;
}

// ---------------------------------------------------------------- audio
int l_audio_play(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* p = aStr(L, 1);
    if (h && h->playSound) h->playSound(p);
    return 0;
}
int l_audio_stopAll(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (h && h->stopAllSounds) h->stopAllSounds();
    return 0;
}
int l_audio_bus(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* bus = aStr(L, 1);
    const float v = aNum(L, 2);
    if (h && h->setBusVolume) h->setBusVolume(bus, v);
    return 0;
}

// ---------------------------------------------------------------- light
//
// 三个灯组件都要覆盖：只认点光/聚光的话，`light.color(sun, ...)` 会报
// "has no light" —— 而太阳恰是最常被剧本改的那一盏（入夜、血月）。
int l_light_color(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "light.color");
    const glm::vec3 c(aNum(L, 2), aNum(L, 3), aNum(L, 4));
    bool any = false;
    if (auto* pl = sc->world().get<ecs::PointLightComponent>(e)) { pl->color = c; any = true; }
    if (auto* sl = sc->world().get<ecs::SpotLightComponent>(e)) { sl->color = c; any = true; }
    if (auto* dl = sc->world().get<ecs::DirectionalLightComponent>(e)) { dl->color = c; any = true; }
    if (!any) return luaL_error(L, "light.color: entity #%d has no light", unsigned(e.id));
    return 0;
}
int l_light_intensity(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "light.intensity");
    const float v = aNum(L, 2);
    bool any = false;
    if (auto* pl = sc->world().get<ecs::PointLightComponent>(e)) { pl->intensity = v; any = true; }
    if (auto* sl = sc->world().get<ecs::SpotLightComponent>(e)) { sl->intensity = v; any = true; }
    if (auto* dl = sc->world().get<ecs::DirectionalLightComponent>(e)) { dl->intensity = v; any = true; }
    if (!any) return luaL_error(L, "light.intensity: entity #%d has no light", unsigned(e.id));
    return 0;
}
int l_light_range(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "light.range");
    const float v = aNum(L, 2);
    bool any = false;
    if (auto* pl = sc->world().get<ecs::PointLightComponent>(e)) { pl->range = v; any = true; }
    if (auto* sl = sc->world().get<ecs::SpotLightComponent>(e)) { sl->range = v; any = true; }
    // 平行光没有"半径"（光照无衰减）→ 只有它时按"没有这个属性"报错
    if (!any) return luaL_error(L, "light.range: entity #%d has no point/spot light", unsigned(e.id));
    return 0;
}
int l_light_direction(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "light.direction");
    const glm::vec3 d(aNum(L, 2), aNum(L, 3), aNum(L, 4));
    const glm::vec3 n = glm::length(d) > 1e-6f ? glm::normalize(d) : glm::vec3(0, -1, 0);
    if (auto* sl = sc->world().get<ecs::SpotLightComponent>(e)) { sl->direction = n; return 0; }
    if (auto* dl = sc->world().get<ecs::DirectionalLightComponent>(e)) { dl->direction = n; return 0; }
    return 0;  // 点光没有方向：空操作（见 ApiTable 的 note）
}
// 开关。点光/聚光有 `enabled`，平行光**没有**这个字段 —— 所以它只能靠
// `intensity = 0` 关掉。这里不替它"假装有开关"：写一个不存在的字段
// 等于让脚本以为关掉了，而画面还亮着。
int l_light_enabled(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "light.enabled");
    const bool hasArg = (lua_gettop(L) >= 2);
    if (auto* pl = sc->world().get<ecs::PointLightComponent>(e)) {
        if (hasArg) pl->enabled = aBool(L, 2);
        lua_pushboolean(L, pl->enabled ? 1 : 0);
        return 1;
    }
    if (auto* sl = sc->world().get<ecs::SpotLightComponent>(e)) {
        if (hasArg) sl->enabled = aBool(L, 2);
        lua_pushboolean(L, sl->enabled ? 1 : 0);
        return 1;
    }
    if (sc->world().get<ecs::DirectionalLightComponent>(e))
        return luaL_error(L,
            "light.enabled: directional light has no switch; use light.intensity(e, 0)");
    return luaL_error(L, "light.enabled: entity #%d has no point/spot light", unsigned(e.id));
}

// ---------------------------------------------------------------- camera
//
// 相机也是组件（`CameraComponent`）。脚本以前完全碰不到它：`fx.shake`
// 只借了相机的 trauma，不碰机位本身。于是"冲刺拉 FOV""剧情推近景"
// "切到二号机位"都写不出来。
ecs::CameraComponent* cameraOf(lua_State* L, ecs::Entity e, const char* what) {
    scene::Scene* sc = sceneOf(L);
    auto* cam = sc->world().get<ecs::CameraComponent>(e);
    if (!cam) luaL_error(L, "%s: entity #%d has no CameraComponent", what, unsigned(e.id));
    return cam;
}

int l_camera_fov(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "camera.fov");
    auto* cam = cameraOf(L, e, "camera.fov");
    if (!cam) { lua_pushnumber(L, 45.0); return 1; }
    if (lua_gettop(L) >= 2) {
        const float v = aNum(L, 2);
        // 1/179 是"能算"的边界（0 或 180 会让投影矩阵退化）
        if (!(v > 1.0f && v < 179.0f))
            return luaL_error(L, "camera.fov: must be in (1, 179) degrees, got %f", double(v));
        cam->fov = v;
        // ⚠⚠ **焦距优先于 fov**（`CameraComponent::focal`）：装了定焦镜头
        //   （`focal > 0`）时 `CameraSystem::poseFromEntity` / `apply` /
        //   `applyCameraEntityTo` 三处都用 `focalToFovY()` **覆盖** fov。
        //   只写 `cam->fov` 的画面**一动不动**，而且一句日志都没有 ——
        //   正是本项目明令禁止的"写了没反应"。
        //   脚本显式给 fov = 明确要这个视角，所以把焦距清掉：两者是同一个量
        //   的两种表示，清 focal 就是"改用 fov 说话"。
        if (cam->focal > 0.0f) {
            VK_LOG_INFO("LUA-CAMERA entity=%u focal=%.1fmm cleared -> fov=%.1f deg",
                        unsigned(e.id), double(cam->focal), double(v));
            cam->focal = 0.0f;
        }
    }
    // 读回来的必须是**实际生效**的视场角（与 `poseFromEntity` 同一条换算），
    // 否则"设了 70、读回 45"又是一个不报错的谎言。
    lua_pushnumber(L, double(cam->focal > 0.0f
                                 ? scene::focalToFovY(cam->focal, cam->sensorHeight)
                                 : cam->fov));
    return 1;
}

int l_camera_distance(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "camera.distance");
    auto* cam = cameraOf(L, e, "camera.distance");
    if (!cam) { lua_pushnumber(L, 0.0); return 1; }
    if (lua_gettop(L) >= 2) {
        const float v = aNum(L, 2);
        if (!(v >= 0.0f)) return luaL_error(L, "camera.distance: must be >= 0");
        cam->distance = v;
    }
    lua_pushnumber(L, double(cam->distance));
    return 1;
}

int l_camera_enabled(lua_State* L) {
    const ecs::Entity e = needEntity(L, 1, "camera.enabled");
    auto* cam = cameraOf(L, e, "camera.enabled");
    if (!cam) { lua_pushboolean(L, 0); return 1; }
    if (lua_gettop(L) >= 2) cam->enabled = aBool(L, 2);
    lua_pushboolean(L, cam->enabled ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------- ui
int l_ui_health(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const float v = aNum(L, 1);
    if (h && h->setHealth) h->setHealth(v);
    return 0;
}

int l_ui_damage(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return 0;
    const float v = aNum(L, 1);
    // 带伤害类型 → 先走玩法属性（护甲/易伤/护盾）；宿主没挂玩法组件时
    // `act.damage` 返回 -1，回退到 HUD 扣血。老脚本行为不变。
    if (lua_gettop(L) >= 2) {
        const char* type = aStr(L, 2);
        if (h->act.damage) {
            const float real = h->act.damage(h->entity, v, type);
            if (real >= 0.0f) { lua_pushnumber(L, double(real)); return 1; }
        }
    }
    if (h->damage) h->damage(v);
    return 0;
}

int l_ui_popup(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->popup) return 0;
    glm::vec3 p(0.0f);
    if (h->scene && h->scene->world().alive(h->entity))
        p = glm::vec3(h->scene->worldMatrix(h->entity)[3]);
    h->popup(p.x, p.y, p.z, aNum(L, 1));
    return 0;
}

int l_ui_dialogue(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "ui.dialogue: no host");
    needAction(L, h->act.showDialogue, "ui.dialogue");
    lua_pushboolean(L, h->act.showDialogue(aStr(L, 1)) ? 1 : 0);
    return 1;
}

int l_ui_sequence(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "ui.sequence: no host");
    needAction(L, h->act.playSequence, "ui.sequence");
    lua_pushboolean(L, h->act.playSequence(aStr(L, 1)) ? 1 : 0);
    return 1;
}

int l_ui_open(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "ui.open: no host");
    needAction(L, h->act.openUI, "ui.open");
    lua_pushboolean(L, h->act.openUI(aStr(L, 1)) ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------- game
int l_game_attr(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "game.attr");
    const char* key = aStr(L, 2);
    auto* sv = sc->world().get<ecs::ScriptVarsComponent>(e);
    if (sv) {
        auto it = sv->values.find(key);
        if (it != sv->values.end()) { lua_pushnumber(L, it->second); return 1; }
    }
    // 给了默认值 → **写回**。不写回的话 `if attr(e,"hp",100) < 20` 每次
    // 都重新取默认值，"血慢慢掉"永远不生效（`.vks` 的同一条修复）。
    if (lua_gettop(L) >= 3) {
        const double d = luaL_checknumber(L, 3);
        if (!sv) sv = &sc->world().add<ecs::ScriptVarsComponent>(e);
        sv->values[key] = d;
        lua_pushnumber(L, d);
        return 1;
    }
    return luaL_error(L, "game.attr '%s' not set on this entity "
                         "(pass a default: game.attr(e, \"%s\", 0))", key, key);
}

int l_game_setAttr(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const ecs::Entity e = needEntity(L, 1, "game.setAttr");
    const char* key = aStr(L, 2);
    const double v = luaL_checknumber(L, 3);
    auto& sv = sc->world().add<ecs::ScriptVarsComponent>(e);
    sv.values[key] = v;
    return 0;
}

int l_game_stat(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.stat: no host");
    const ecs::Entity e = needEntity(L, 1, "game.stat");
    if (!h->act.getAttr) return luaL_error(L, "game.stat: gameplay not installed");
    lua_pushnumber(L, double(h->act.getAttr(e, aStr(L, 2))));
    return 1;
}

int l_game_hasStat(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const ecs::Entity e = needEntity(L, 1, "game.hasStat");
    lua_pushboolean(L, (h && h->act.gameplayHasAttr &&
                        h->act.gameplayHasAttr(e, aStr(L, 2))) ? 1 : 0);
    return 1;
}

// 对**别人**造成伤害。`ui.damage` 是"打自己"那个（HUD 血条），
// 两者同名不同域：一个作用于 self，一个作用于显式实体。
// 返回**真正扣掉的血**（过量伤害不虚高）—— 判死亡要用它，别拿 amount 判。
int l_game_damage(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.damage: no host");
    const ecs::Entity e = needEntity(L, 1, "game.damage");
    const float amount = aNum(L, 2);
    const char* type = aStr(L, 3);
    if (!h->act.damage)
        return luaL_error(L, "game.damage: gameplay not installed");
    const float real = h->act.damage(e, amount, type);
    if (real < 0.0f)
        return luaL_error(L, "game.damage: entity #%d has no gameplay component",
                          unsigned(e.id));
    lua_pushnumber(L, double(real));
    return 1;
}

int l_game_worldAttr(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    const char* key = aStr(L, 1);
    auto& m = sc->scriptVars();
    auto it = m.find(key);
    if (it != m.end()) { lua_pushnumber(L, it->second); return 1; }
    if (lua_gettop(L) >= 2) {
        const double d = luaL_checknumber(L, 2);
        m[key] = d;
        lua_pushnumber(L, d);
        return 1;
    }
    return luaL_error(L, "game.worldAttr '%s' not set "
                         "(pass a default: game.worldAttr(\"%s\", 0))", key, key);
}

int l_game_setWorldAttr(lua_State* L) {
    scene::Scene* sc = sceneOf(L);
    sc->scriptVars()[aStr(L, 1)] = luaL_checknumber(L, 2);
    return 0;
}

int l_game_status(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.status: no host");
    const ecs::Entity e = needEntity(L, 1, "game.status");
    if (!h->act.addStatus) return luaL_error(L, "game.status: gameplay not installed");
    lua_pushinteger(L, h->act.addStatus(e, aStr(L, 2)));
    return 1;
}

int l_game_round(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.round: no host");
    if (!h->act.tickRound) return luaL_error(L, "game.round: gameplay not installed");
    lua_pushinteger(L, h->act.tickRound());
    return 1;
}

int l_game_flag(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.flag: no host");
    const char* name = aStr(L, 1);
    if (lua_gettop(L) >= 2) {
        needAction(L, h->act.setFlag, "game.flag(name, value)");
        h->act.setFlag(name, luaL_checknumber(L, 2));
        return 0;
    }
    // 未设过 → 0，不报错（"第 1 章没遇见的人好感度就是 0"是正确语义）
    lua_pushnumber(L, h->act.getFlag ? h->act.getFlag(name) : 0.0);
    return 1;
}

int l_game_castCard(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.castCard: no host");
    const ecs::Entity e = needEntity(L, 1, "game.castCard");
    if (!h->act.castCard) return luaL_error(L, "game.castCard: cards not installed");
    lua_pushnumber(L, double(h->act.castCard(e, aInt(L, 2))));
    return 1;
}

int l_game_drawCards(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.drawCards: no host");
    const ecs::Entity e = needEntity(L, 1, "game.drawCards");
    if (!h->act.drawCards) return luaL_error(L, "game.drawCards: cards not installed");
    lua_pushinteger(L, h->act.drawCards(e, aInt(L, 2)));
    return 1;
}

int l_game_tableNum(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->act.tableNum) { lua_pushnumber(L, 0.0); return 1; }
    lua_pushnumber(L, h->act.tableNum(aStr(L, 1)));
    return 1;
}
int l_game_tableStr(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h || !h->act.tableStr) { lua_pushstring(L, ""); return 1; }
    const std::string s = h->act.tableStr(aStr(L, 1));
    lua_pushlstring(L, s.c_str(), s.size());
    return 1;
}
int l_game_tableHas(lua_State* L) {
    ScriptHost* h = hostOf(L);
    lua_pushboolean(L, (h && h->act.tableHas && h->act.tableHas(aStr(L, 1))) ? 1 : 0);
    return 1;
}
int l_game_tableRows(lua_State* L) {
    ScriptHost* h = hostOf(L);
    lua_pushinteger(L, (h && h->act.tableRows) ? h->act.tableRows(aStr(L, 1)) : -1);
    return 1;
}

// ---- 关节九件。全部**显式**传 self（见 ScriptJointActions 的注释）----
int l_game_joint(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.joint: no host");
    const ecs::Entity e = needEntity(L, 1, "game.joint");
    const ecs::Entity other = unpackEntity(aHandle(L, 2));
    needAction(L, h->act.joints.set, "game.joint");
    lua_pushboolean(L, h->act.joints.set(e, other, aStr(L, 3)) ? 1 : 0);
    return 1;
}
int l_game_jointAnchors(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointAnchors: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointAnchors");
    needAction(L, h->act.joints.anchors, "game.jointAnchors");
    lua_pushboolean(L, h->act.joints.anchors(e, aVec(L, 2), aVec(L, 3)) ? 1 : 0);
    return 1;
}
int l_game_jointAxis(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointAxis: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointAxis");
    needAction(L, h->act.joints.axis, "game.jointAxis");
    lua_pushboolean(L, h->act.joints.axis(e, aVec(L, 2)) ? 1 : 0);
    return 1;
}
int l_game_jointLimit(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointLimit: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointLimit");
    needAction(L, h->act.joints.limit, "game.jointLimit");
    lua_pushboolean(L, h->act.joints.limit(e, aBool(L, 2), aNum(L, 3), aNum(L, 4)) ? 1 : 0);
    return 1;
}
int l_game_jointMotor(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointMotor: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointMotor");
    needAction(L, h->act.joints.motor, "game.jointMotor");
    lua_pushboolean(L, h->act.joints.motor(e, aNum(L, 2), aNum(L, 3)) ? 1 : 0);
    return 1;
}
int l_game_jointOn(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointOn: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointOn");
    needAction(L, h->act.joints.enable, "game.jointOn");
    lua_pushboolean(L, h->act.joints.enable(e, aBool(L, 2)) ? 1 : 0);
    return 1;
}
int l_game_jointRemove(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointRemove: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointRemove");
    needAction(L, h->act.joints.remove, "game.jointRemove");
    lua_pushboolean(L, h->act.joints.remove(e) ? 1 : 0);
    return 1;
}
int l_game_jointKind(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointKind: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointKind");
    const char* k = h->act.joints.kindOf ? h->act.joints.kindOf(e) : nullptr;
    if (k) lua_pushstring(L, k); else lua_pushnil(L);
    return 1;
}
int l_game_jointTarget(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return luaL_error(L, "game.jointTarget: no host");
    const ecs::Entity e = needEntity(L, 1, "game.jointTarget");
    const ecs::Entity t = h->act.joints.target ? h->act.joints.target(e) : ecs::Entity{};
    lua_pushinteger(L, lua_Integer(packEntity(t)));
    return 1;
}


// ---------------------------------------------------------------- 注册
//
// ⚠ 绑定**不再**直接挂实现函数，而是统一挂 `dispatch`（定义在文件后半，
//   因为要拿到 `kImpls[]`）。为什么要包一层：`ApiTable` 里每条都写了
//   `minArgs/maxArgs`，但注册时从来不查 —— 于是"签名说 2 个参数、实现只读
//   第 1 个"这种偏差编译期看不出来、运行期只表现为"某个参数永远是 nil"。
//   `.vks` 那边（`kBuiltins`）是查的，两个后端必须同一套语义。
//
// 建一张全局表并保持它在栈顶，返回后调用方负责 pop。
void newTable(lua_State* L, const char* name) {
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setglobal(L, name);
}

// ---------------------------------------------------------------- 模板与实例
const char* kHookLuaName[] = {"BeginPlay", "Tick", "OnCollide", "OnTrigger", "OnExit"};

int hookIndexOf(HookKind h) {
    switch (h) {
        case HookKind::Start:   return 0;
        case HookKind::Update:  return 1;
        case HookKind::Collide: return 2;
        case HookKind::Trigger: return 3;
        case HookKind::Exit:    return 4;
    }
    return 1;
}

class LuaCompiled : public ICompiledScript {
public:
    LuaCompiled(std::string p, uint64_t ver) : m_path(std::move(p)), m_version(ver) {}
    const std::string& path() const override { return m_path; }
    bool ok() const override { return m_ok; }
    const std::string& error() const override { return m_error; }
    bool hasHook(HookKind h) const override {
        const int i = hookIndexOf(h);
        return i >= 0 && i < 5 && m_hooks[i];
    }
    uint64_t version() const override { return m_version; }

    std::string m_path;
    std::string m_error;
    bool m_ok = false;
    bool m_hooks[5] = {false, false, false, false, false};
    uint64_t m_version = 0;
    int m_templateRef = LUA_NOREF;  // registry 里的模板表（见文件头 ④）
};

class LuaInstance : public IScriptInstance {
public:
    LuaInstance(LuaBackend* be, std::shared_ptr<LuaCompiled> prog, ecs::Entity e)
        : m_be(be), m_prog(std::move(prog)), m_entity(e) {}

    ~LuaInstance() override {
        // env 表也交给 lua_close 回收（同文件头 ④），这里只把引用置空
        m_envRef = LUA_NOREF;
        m_coRef = LUA_NOREF;
    }

    bool beginPlay(ScriptHost& host) override { return callHook(host, "BeginPlay", 0.0f, false); }

    bool tick(ScriptHost& host, float dt) override {
        if (m_failed) return false;
        if (m_waiting) { resume(host, dt); return true; }
        return callHook(host, "Tick", dt, false);
    }

    bool fireHook(ScriptHost& host, HookKind h, ecs::Entity other) override {
        if (m_failed) return false;
        const int i = hookIndexOf(h);
        if (i == 0 || i == 1) return true;  // Start/Update 不走事件通道
        return callHook(host, kHookLuaName[i], 0.0f, true, other);
    }

    bool resume(ScriptHost& host, float dt) override {
        if (m_failed || !m_waiting) return true;

        // `waitUntil`：条件函数**每帧求值一次**，成立才放行。
        // ⚠ 求值不是 resume：条件不成立时协程一直在 yield 处，预算不消耗。
        if (m_untilRef >= 0) {
            lua_State* L = m_be->state();
            if (!L) return true;
            const bool ready = evalUntil(host, L);
            if (m_failed) return false;
            if (!ready) return true;
            luaL_unref(L, LUA_REGISTRYINDEX, m_untilRef);
            m_untilRef = -1;
            m_waitRemain = 0.0;
            return continueCoroutine(host);
        }

        if (m_waitByFrames) {
            m_waitRemain -= 1.0;
        } else {
            m_waitRemain -= double(dt);
        }
        if (m_waitRemain > 0.0) return true;
        return continueCoroutine(host);
    }

    bool waiting() const override { return m_waiting; }
    bool failed() const override { return m_failed; }
    const std::string& lastError() const override { return m_lastError; }

    bool reload(std::shared_ptr<ICompiledScript> next) override {
        auto* np = dynamic_cast<LuaCompiled*>(next.get());
        if (!np || !np->m_ok) return false;
        // 只换 metatable 的 __index → 方法立刻是新的，`self.hp` 这些一个不动。
        // 正在 wait 的协程仍然指向旧函数体（Lua 的 pc 无法跨 chunk 迁移），
        // 这是既定代价，见 docs/script_framework.md §5。
        rebindTemplate(np->m_templateRef);
        return true;
    }

    std::vector<std::pair<std::string, std::string>> snapshotVars() const override {
        std::vector<std::pair<std::string, std::string>> out;
        lua_State* L = m_be->state();
        if (!L || m_envRef == LUA_NOREF) return out;
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_envRef);
        const int envIdx = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, envIdx)) {
            // key 在 -2，value 在 -1
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) != LUA_TFUNCTION) {
                const char* k = lua_tostring(L, -2);
                if (k && std::strcmp(k, "entity") != 0) {
                    const char* v = luaL_tolstring(L, -1, nullptr);
                    out.emplace_back(k, v ? v : "?");
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        return out;
    }

    // 建 env 表 + metatable（由 backend 在 createInstance 里调）
    void attachEnv(lua_State* L, int templateRef) {
        lua_newtable(L);                       // [env]
        const int envIdx = lua_gettop(L);
        lua_newtable(L);                       // [env, meta]
        lua_rawgeti(L, LUA_REGISTRYINDEX, templateRef);  // [env, meta, tmpl]
        lua_setfield(L, -2, "__index");        // meta.__index = tmpl
        lua_setmetatable(L, envIdx);           // setmetatable(env, meta)

        lua_pushinteger(L, lua_Integer(packEntity(m_entity)));
        lua_setfield(L, envIdx, "entity");

        lua_pushvalue(L, envIdx);
        m_envRef = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pop(L, 1);  // 弹掉建表时留下的 env —— 栈必须回到进入时的样子
    }

private:
    void rebindTemplate(int templateRef) {
        lua_State* L = m_be->state();
        if (!L || m_envRef == LUA_NOREF || templateRef == LUA_NOREF) return;
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_envRef);      // [env]
        const int envIdx = lua_gettop(L);
        lua_getmetatable(L, envIdx);                      // [env, meta]
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, templateRef);
            lua_setfield(L, -2, "__index");
        }
        lua_pop(L, 2);
    }

    void fail(const std::string& msg) {
        m_failed = true;
        if (m_lastError != msg) {
            m_lastError = msg;
            VK_LOG_WARN("LUA-INSTANCE failed entity=%u err=%s",
                        unsigned(m_entity.id), msg.c_str());
        }
    }

    // 把这三个字段刷进**全局的 `engine` 表**。
    // ⚠ 表是全局共享的，但脚本调用是串行的（一个 VM 一个线程），所以
    //   每个实例进来前覆盖一次就是对的 —— 不需要给每个实例各建一张 engine 表。
    void setEngineFields(lua_State* L, float dt) {
        lua_getglobal(L, "engine");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
        lua_pushnumber(L, double(dt));              lua_setfield(L, -2, "dt");
        lua_pushnumber(L, m_localTime);             lua_setfield(L, -2, "time");
        lua_pushinteger(L, lua_Integer(m_frameNo)); lua_setfield(L, -2, "frame");
        lua_pop(L, 1);
    }

    bool callHook(ScriptHost& host, const char* name, float dt, bool withArg,
                  ecs::Entity other = {}) {
        lua_State* L = m_be->state();
        if (!L) return false;
        m_be->setHost(&host);
        // `engine.dt` / `time` / `frame` 是**字段不是函数**（每帧读，函数调用不值），
        // 所以每次进脚本前刷一次。`time` 由实例自己累加而不是拿全局时间乘倍速 ——
        // 改 `timeScale` 时它不会跳变，只改变前进速度（子弹时间要的正是这个）。
        if (dt > 0.0f) { m_localTime += double(dt); ++m_frameNo; }
        setEngineFields(L, dt);

        lua_rawgeti(L, LUA_REGISTRYINDEX, m_envRef);  // [env]
        const int envIdx = lua_gettop(L);
        lua_getfield(L, envIdx, name);                // [env, fn]
        if (lua_isnil(L, -1)) {                       // 没实现这个钩子
            lua_pop(L, 2);
            m_be->setHost(nullptr);
            return true;
        }

        lua_State* co = lua_newthread(L);             // [env, fn, co]

        // 把 fn 与参数搬到协程栈（`lua_xmove` 要求同一个 state）
        lua_pushvalue(L, envIdx + 1);                 // [env, fn, co, fn]
        lua_xmove(L, co, 1);                          // co: [fn]
        lua_pushvalue(L, envIdx);                     // [env, fn, co, env]
        lua_xmove(L, co, 1);                          // co: [fn, env]
        int nargs = 1;
        if (strcmp(name, "Tick") == 0) {
            lua_pushnumber(L, double(dt));
            lua_xmove(L, co, 1);
            ++nargs;
        } else if (withArg) {
            lua_pushinteger(L, lua_Integer(packEntity(other)));
            lua_xmove(L, co, 1);
            ++nargs;
        }

        m_coRef = luaL_ref(L, LUA_REGISTRYINDEX);     // 弹出 co 并保住（含 GC 根）
        lua_pop(L, 2);                                // env / fn 出栈

        m_be->setHost(&host);
        return doResume(co, nargs);
    }

    bool doResume(lua_State* co, int nargs) {
        budgetAttach(co, m_be->stepsPerFrame());

        // ⚠⚠⚠ `lua_resume` 的第 4 个参数**绝对不可以传 nullptr**。
        //
        //   Lua 5.4.7 的 `ldo.c:863` 是**无条件**解引用，没有 null 检查：
        //       *nresults = (status == LUA_YIELD) ? L->ci->u2.nyield : ...;
        //   传 nullptr 等于往地址 0 写一个 int → 必然 0xC0000005。
        //
        //   这个坑极难从现象反推：崩溃点在 `lua_resume` 内部，调用栈完整、
        //   参数栈也正常，`lua_pcall` 同样的代码毫无问题 —— 看起来像
        //   "库编坏了"。当时为此怀疑过 /RTC1、/GS、C++ 编译、Debug 优化，
        //   全部排除后才定位到这里。
        //
        //   而且它只在**协程**里触发：`lua_pcall` 没有这个参数，所以
        //   "主线程能跑、协程一启动就崩"是第一特征。
        //
        //   ⚠ 5.3 是**三个**参数；网上讲 5.4 迁移的笔记大多只说"多了一个
        //     参数"，没有一句提到"这个参数必须指向有效内存"。
        int nres = 0;
        const int rc = lua_resume(co, m_be->state(), nargs, &nres);
        budgetDetach(co);
        m_be->setHost(nullptr);

        if (rc == LUA_YIELD) {
            if (nres > 0) lua_pop(co, nres);

            // waitUntil 走独立通道：条件函数 ref 挂到实例上，由 resume() 求值。
            const int ur = m_be->takePendingUntil();
            if (ur >= 0) {
                m_untilRef = ur;
                m_waitByFrames = false;
                m_waitRemain = 0.0;
                m_waiting = true;
                return true;
            }

            const double w = m_be->takePendingWait();
            m_waiting = true;
            m_waitByFrames = (w < 0.0);
            m_waitRemain = m_waitByFrames ? -w : w;
            return true;
        }
        if (rc != LUA_OK) {
            const char* msg = lua_tostring(co, -1);
            lua_pop(co, 1);
            fail(msg ? msg : "lua error");
            m_waiting = false;
            freeCoRef();
            return false;
        }
        m_waiting = false;
        freeCoRef();  // 协程正常结束，摘掉 registry 引用让它能被 GC
        return true;
    }

    // `waitUntil(fn)` 的条件求值。失败（函数本体报错）算实例熔断 ——
    // 条件函数里出错却继续等下去，症状是"脚本卡在这一行"而日志无痕。
    bool evalUntil(ScriptHost& host, lua_State* L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_untilRef);
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); m_waiting = false; return true; }
        m_be->setHost(&host);
        // 条件函数同样是作者代码：里面写死循环也会冻住（见 `luaBudgetHook`）
        budgetAttach(L, m_be->stepsPerFrame());
        const int rc = lua_pcall(L, 0, 1, 0);
        budgetDetach(L);
        m_be->setHost(nullptr);
        if (rc != LUA_OK) {
            const char* msg = lua_tostring(L, -1);
            lua_pop(L, 1);
            fail(msg ? msg : "waitUntil: predicate raised an error");
            m_waiting = false;
            return false;
        }
        const bool ready = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return ready;
    }

    void freeCoRef() {
        lua_State* L = m_be->state();
        if (L && m_coRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, m_coRef);
        }
        if (L && m_untilRef >= 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, m_untilRef);  // 别让条件函数泄漏
        }
        m_untilRef = -1;
        m_coRef = LUA_NOREF;
    }

    bool continueCoroutine(ScriptHost& host) {
        lua_State* L = m_be->state();
        if (!L || m_coRef == LUA_NOREF) { m_waiting = false; return true; }
        lua_rawgeti(L, LUA_REGISTRYINDEX, m_coRef);   // [co]
        lua_State* co = lua_tothread(L, -1);
        if (!co) { lua_pop(L, 1); m_waiting = false; return true; }
        m_be->setHost(&host);
        // 传给协程的恢复值 = `engine.wait()` 的返回值 = **剩余等待时间**。
        // 走到这里就说明已经等完了，所以恒为 0；保留这个形参是为了
        // `while left > 0 do left = engine.wait(left) end` 这种补时写法。
        lua_pushnumber(L, 0.0);
        lua_xmove(L, co, 1);
        const bool ok = doResume(co, 1);
        lua_pop(L, 1);
        return ok;
    }

    // 熔断钩子已提到文件级（`luaBudgetHook`）—— `compile()` 与 `evalUntil()`
    // 也要用它，而静态成员函数那边取不到（private）。

    LuaBackend* m_be = nullptr;
    std::shared_ptr<LuaCompiled> m_prog;
    ecs::Entity m_entity{};
    int m_envRef = LUA_NOREF;
    int m_coRef = LUA_NOREF;
    // `waitUntil` 的条件函数（registry ref）。-1 = 没有在等条件。
    int m_untilRef = -1;
    // `engine.time` / `engine.frame` 的来源（见 setEngineFields）
    double m_localTime = 0.0;
    uint64_t m_frameNo = 0;
    bool m_waiting = false;
    bool m_waitByFrames = false;
    double m_waitRemain = 0.0;
    bool m_failed = false;
    std::string m_lastError;
};

} // namespace

// ============================================================
// LuaBackend
// ============================================================

LuaBackend::LuaBackend(int stepsPerFrame) : m_stepsPerFrame(stepsPerFrame) {
    if (const char* env = std::getenv("MYVK_LUA_BUDGET")) {
        const int v = std::atoi(env);
        if (v > 0) m_stepsPerFrame = v;
    }

    m_L = luaL_newstate();
    if (!m_L) {
        VK_LOG_ERROR("LUA-VM create failed");
        return;
    }
    g_be = this;

    // 白名单打开：不开 io/os/debug/package（见 LuaBackend.h ④）
    luaL_requiref(m_L, "_G", luaopen_base, 1);      lua_pop(m_L, 1);
    luaL_requiref(m_L, LUA_TABLIBNAME, luaopen_table, 1);   lua_pop(m_L, 1);
    luaL_requiref(m_L, LUA_STRLIBNAME, luaopen_string, 1);  lua_pop(m_L, 1);
    luaL_requiref(m_L, LUA_MATHLIBNAME, luaopen_math, 1);   lua_pop(m_L, 1);
    luaL_requiref(m_L, LUA_UTF8LIBNAME, luaopen_utf8, 1);   lua_pop(m_L, 1);
    luaL_requiref(m_L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(m_L, 1);

    // 掐掉破坏确定性 / 安全性的入口
    lua_pushnil(m_L); lua_setglobal(m_L, "dofile");
    lua_pushnil(m_L); lua_setglobal(m_L, "loadfile");
    lua_pushnil(m_L); lua_setglobal(m_L, "load");
    lua_getglobal(m_L, "math");
    lua_pushnil(m_L); lua_setfield(m_L, -2, "random");
    lua_pushnil(m_L); lua_setfield(m_L, -2, "randomseed");
    lua_pop(m_L, 1);

    installBindings();
    VK_LOG_INFO("LUA-VM ready budget=%d", m_stepsPerFrame);
}

LuaBackend::~LuaBackend() {
    if (g_be == this) g_be = nullptr;
    if (m_L) { lua_close(m_L); m_L = nullptr; }
}

bool LuaBackend::accepts(const std::string& path) const {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".lua";
}

std::shared_ptr<ICompiledScript> LuaBackend::compile(const std::string& path,
                                                     const std::string& source) {
    auto out = std::make_shared<LuaCompiled>(path, ++m_version);
    if (!m_L) { out->m_error = "no lua state"; return out; }

    const std::string chunk = "@" + path;  // 报错里带文件名的写法
    if (luaL_loadbufferx(m_L, source.c_str(), source.size(), chunk.c_str(), "t") != LUA_OK) {
        const char* msg = lua_tostring(m_L, -1);
        out->m_error = msg ? msg : "load failed";
        lua_pop(m_L, 1);
        return out;
    }

    // ⚠⚠ chunk 的 `_ENV` 必须是**它自己的一张表**，不能直接落到 `_G` 上。
    //   用 `_G` 有两个后果，第二个是致命的：
    //     ① 脚本里所有裸赋值（`hp = 10`）都写进真正的全局 → **所有实例共享
    //        一份**，"两个敌人各掉各的血"直接失效；
    //     ② 脚本写 `world = nil` / `engine = nil` 能把 **API 表本身**抹掉 ——
    //        之后整个 VM 里所有脚本都调不到 `world.pos`。
    //   这里建成 `{__index = _G}`：读照样穿透到全局（`engine`/`world`/... 都在），
    //   写只落在这张表上（同一份脚本的多个实例共享它 —— 语义同 C# 的 static，
    //   要实例状态请写 `self.x`，见 docs/script_api.md §3）。
    lua_newtable(m_L);                       // [chunk, env]
    lua_newtable(m_L);                       // [chunk, env, meta]
    lua_pushglobaltable(m_L);
    lua_setfield(m_L, -2, "__index");        // meta.__index = _G
    lua_setmetatable(m_L, -2);               // setmetatable(env, meta)
    lua_setupvalue(m_L, -2, 1);              // 弹出 env，设成 chunk 的 _ENV

    // ⚠ 顶层代码也是**作者写的代码**：`while true do end` 写在 chunk 顶层
    //   同样会冻住整个游戏，而这里的 `lua_pcall` 以前**没有**装熔断钩子
    //   （钩子只在 `doResume()` 里装）。装上之后，顶层死循环变成一条
    //   "编译失败：budget exceeded"，而不是一个要任务管理器才能关掉的窗口。
    budgetAttach(m_L, m_stepsPerFrame);
    const int runRc = lua_pcall(m_L, 0, 1, 0);
    budgetDetach(m_L);
    if (runRc != LUA_OK) {
        const char* msg = lua_tostring(m_L, -1);
        out->m_error = msg ? msg : "run failed";
        lua_pop(m_L, 1);
        return out;
    }
    if (!lua_istable(m_L, -1)) {
        out->m_error = "script must `return` a table (see docs/script_framework.md)";
        lua_pop(m_L, 1);
        return out;
    }

    for (int i = 0; i < 5; ++i) {
        lua_getfield(m_L, -1, kHookLuaName[i]);
        out->m_hooks[i] = !lua_isnil(m_L, -1);
        lua_pop(m_L, 1);
    }
    out->m_templateRef = luaL_ref(m_L, LUA_REGISTRYINDEX);
    out->m_ok = true;
    return out;
}

std::unique_ptr<IScriptInstance> LuaBackend::createInstance(
    std::shared_ptr<ICompiledScript> prog, ecs::Entity e) {
    auto* lp = dynamic_cast<LuaCompiled*>(prog.get());
    if (!lp || !lp->m_ok || !m_L) return nullptr;

    // 实例必须持有一份 shared_ptr<LuaCompiled>，否则 prog 被调用方释放后
    // 模板表的 ref 就没人管（表还在 registry 里，但我们不再知道它属于谁）。
    std::shared_ptr<LuaCompiled> keep = std::dynamic_pointer_cast<LuaCompiled>(prog);
    auto inst = std::make_unique<LuaInstance>(this, keep, e);
    inst->attachEnv(m_L, lp->m_templateRef);

    // 一个钩子都没写的脚本**不是错误**（纯函数库是合法用法），但十有八九
    // 是大小写写错了 —— `beginPlay` / `tick` 在 Lua 里完全合法，只是引擎
    // 永远不会调用它，表现是"脚本一行动静都没有"，日志也一行没有。
    // 所以这里给一行 WARN，把"什么都没发生"变成"日志告诉你为什么"。
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, lp->m_templateRef);
    if (lua_istable(m_L, -1)) {
        static const char* kHooks[] = {"BeginPlay", "Tick", "EndPlay",
                                       "OnCollide", "OnTrigger", "OnExit"};
        bool any = false;
        for (const char* h : kHooks) {
            lua_getfield(m_L, -1, h);
            if (lua_isfunction(m_L, -1)) any = true;
            lua_pop(m_L, 1);
        }
        lua_pop(m_L, 1);
        if (!any)
            VK_LOG_WARN("LUA-NOHOOK %s: 表里没有任何钩子（BeginPlay/Tick/...），"
                        "这份脚本永远不会被调用 —— 检查大小写",
                        lp->path().c_str());
    } else {
        lua_pop(m_L, 1);
    }
    return inst;
}


namespace {

// (ns, name) → 实现。与 `ApiTable` 的条目一一对应。
// ⚠ 这里**不是**事实源：名字和参数个数以 ApiTable 为准，这里只是把名字
//   接到函数上。漏一条 → `installBindings()` 打 `LUA-API missing ...`，
//   不会变成"文档里有、脚本里调不到"。
struct Impl {
    const char* ns;
    const char* name;
    lua_CFunction fn;
};

const Impl kImpls[] = {
    {"actor", "color", l_actor_color},
    {"actor", "grounded", l_actor_grounded},
    {"actor", "impulse", l_actor_impulse},
    {"actor", "setColor", l_actor_setColor},
    {"actor", "setEmissive", l_actor_setEmissive},
    {"actor", "setShadow", l_actor_setShadow},
    {"actor", "setVel", l_actor_setVel},
    {"actor", "setVisible", l_actor_setVisible},
    {"actor", "vel", l_actor_vel},
    {"actor", "visible", l_actor_visible},
    {"anim", "clip", l_anim_clip},
    {"anim", "expression", l_anim_expression},
    {"anim", "morph", l_anim_morph},
    {"anim", "morphWeight", l_anim_morphWeight},
    {"anim", "pause", l_anim_pause},
    {"anim", "play", l_anim_play},
    {"anim", "resume", l_anim_resume},
    {"anim", "speed", l_anim_speed},
    {"anim", "stop", l_anim_stop},
    {"anim", "time", l_anim_time},
    {"audio", "bus", l_audio_bus},
    {"audio", "play", l_audio_play},
    {"audio", "stopAll", l_audio_stopAll},
    {"camera", "distance", l_camera_distance},
    {"camera", "enabled", l_camera_enabled},
    {"camera", "fov", l_camera_fov},
    {"engine", "clamp", l_engine_clamp},
    {"engine", "dist", l_engine_dist},
    {"engine", "exists", l_engine_exists},
    {"engine", "lerp", l_engine_lerp},
    {"engine", "log", l_engine_log},
    {"engine", "name", l_engine_name},
    {"engine", "self", l_engine_self},
    {"engine", "valid", l_engine_valid},
    {"engine", "vec3", l_engine_vec3},
    {"engine", "wait", l_engine_wait},
    {"engine", "waitFrames", l_engine_waitFrames},
    {"engine", "waitUntil", l_engine_waitUntil},
    {"engine", "warn", l_engine_warn},
    {"fx", "hitstop", l_fx_hitstop},
    {"fx", "play", l_fx_play},
    {"fx", "shake", l_fx_shake},
    {"fx", "stop", l_fx_stop},
    {"fx", "timeScale", l_fx_timeScale},
    {"game", "attr", l_game_attr},
    {"game", "castCard", l_game_castCard},
    {"game", "damage", l_game_damage},
    {"game", "drawCards", l_game_drawCards},
    {"game", "flag", l_game_flag},
    {"game", "hasStat", l_game_hasStat},
    {"game", "joint", l_game_joint},
    {"game", "jointAnchors", l_game_jointAnchors},
    {"game", "jointAxis", l_game_jointAxis},
    {"game", "jointKind", l_game_jointKind},
    {"game", "jointLimit", l_game_jointLimit},
    {"game", "jointMotor", l_game_jointMotor},
    {"game", "jointOn", l_game_jointOn},
    {"game", "jointRemove", l_game_jointRemove},
    {"game", "jointTarget", l_game_jointTarget},
    {"game", "round", l_game_round},
    {"game", "setAttr", l_game_setAttr},
    {"game", "setWorldAttr", l_game_setWorldAttr},
    {"game", "stat", l_game_stat},
    {"game", "status", l_game_status},
    {"game", "tableHas", l_game_tableHas},
    {"game", "tableNum", l_game_tableNum},
    {"game", "tableRows", l_game_tableRows},
    {"game", "tableStr", l_game_tableStr},
    {"game", "worldAttr", l_game_worldAttr},
    {"input", "axis", l_input_axis},
    {"input", "held", l_input_held},
    {"input", "pressed", l_input_pressed},
    {"input", "released", l_input_released},
    {"light", "color", l_light_color},
    {"light", "direction", l_light_direction},
    {"light", "enabled", l_light_enabled},
    {"light", "intensity", l_light_intensity},
    {"light", "range", l_light_range},
    {"nav", "arrived", l_nav_arrived},
    {"nav", "failed", l_nav_failed},
    {"nav", "moveTo", l_nav_goto},
    {"nav", "stop", l_nav_stop},
    {"ui", "damage", l_ui_damage},
    {"ui", "dialogue", l_ui_dialogue},
    {"ui", "health", l_ui_health},
    {"ui", "open", l_ui_open},
    {"ui", "popup", l_ui_popup},
    {"ui", "sequence", l_ui_sequence},
    {"world", "children", l_world_children},
    {"world", "count", l_world_count},
    {"world", "despawn", l_world_despawn},
    {"world", "find", l_world_find},
    {"world", "loadScene", l_world_loadScene},
    {"world", "moveBy", l_world_moveBy},
    {"world", "near", l_world_near},
    {"world", "parent", l_world_parent},
    {"world", "pos", l_world_pos},
    {"world", "rot", l_world_rot},
    {"world", "rotY", l_world_rotY},
    {"world", "scale", l_world_scale},
    {"world", "setName", l_world_setName},
    {"world", "setParent", l_world_setParent},
    {"world", "setPos", l_world_setPos},
    {"world", "setRot", l_world_setRot},
    {"world", "setScale", l_world_setScale},
    {"world", "spawn", l_world_spawn},
};

const Impl* findImpl(const char* ns, const char* name) {
    for (const Impl& i : kImpls)
        if (std::strcmp(i.ns, ns) == 0 && std::strcmp(i.name, name) == 0)
            return &i;
    return nullptr;
}

// 所有绑定共用的入口：**先按 ApiTable 校验实参个数**，再进实现。
//
// upvalue 布局（① 必须是 backend —— `backendOf()` 认死这一位）：
//   1 = `LuaBackend*`   2 = `const Impl*`   3 = `const ApiEntry*`
//
// 报错走 `luaL_error`（带 `pcall` 能接住），措辞与 `.vks` 的
// "xxx expects N args" 保持同一风格：**调用方一眼能看出差在哪**。
int dispatch(lua_State* L) {
    auto* impl = static_cast<const Impl*>(lua_touserdata(L, lua_upvalueindex(2)));
    auto* ent = static_cast<const ApiEntry*>(lua_touserdata(L, lua_upvalueindex(3)));
    if (!impl || !ent) return luaL_error(L, "corrupt binding (missing upvalue)");

    const int n = lua_gettop(L);
    const int lo = int(ent->minArgs);
    const int hi = (ent->maxArgs == kVarArgs) ? -1 : int(ent->maxArgs);
    const bool bad = (n < lo) || (hi >= 0 && n > hi);
    if (bad) {
        if (hi < 0)
            return luaL_error(L, "%s.%s: expects at least %d argument(s), got %d",
                              ent->ns, ent->name, lo, n);
        if (lo == hi)
            return luaL_error(L, "%s.%s: expects %d argument(s), got %d",
                              ent->ns, ent->name, lo, n);
        return luaL_error(L, "%s.%s: expects %d..%d argument(s), got %d",
                          ent->ns, ent->name, lo, hi, n);
    }
    return impl->fn(L);
}

void regFn(lua_State* L, LuaBackend* be, const Impl* impl, const ApiEntry* ent) {
    lua_pushlightuserdata(L, be);
    lua_pushlightuserdata(L, const_cast<Impl*>(impl));
    lua_pushlightuserdata(L, const_cast<ApiEntry*>(ent));
    lua_pushcclosure(L, dispatch, 3);
    lua_setfield(L, -2, ent->name);  // 栈顶是目标表
}

} // namespace

void LuaBackend::installBindings() {
    lua_State* L = m_L;
    LuaBackend* be = this;

    // ① 命名空间表：**从 ApiTable 里出现过的 ns 生成**，不再手写 `newTable`。
    std::vector<std::string> namespaces;
    for (int i = 0; i < apiEntryCount(); ++i) {
        const char* ns = apiEntries()[i].ns;
        bool seen = false;
        for (const auto& s : namespaces)
            if (s == ns) { seen = true; break; }
        if (!seen) namespaces.emplace_back(ns);
    }
    for (const auto& ns : namespaces) newTable(L, ns.c_str());
    // newTable 会把表留在栈上并设了全局 —— 逐个弹掉，后面按名取。
    lua_pop(L, int(namespaces.size()));

    // ② 函数：逐条从表注册。**缺实现就报错**，不静默跳过。
    int missing = 0;
    for (int i = 0; i < apiEntryCount(); ++i) {
        const ApiEntry& e = apiEntries()[i];
        const Impl* impl = findImpl(e.ns, e.name);
        if (!impl || !impl->fn) {
            ++missing;
            VK_LOG_ERROR("LUA-API missing implementation: %s.%s", e.ns, e.name);
            continue;
        }
        lua_getglobal(L, e.ns);
        regFn(L, be, impl, &e);
        lua_pop(L, 1);
    }

    // ③ 字段（不是函数）：每帧由实例刷新，函数调用不值。
    lua_getglobal(L, "engine");
    lua_pushnumber(L, 0.0); lua_setfield(L, -2, "dt");
    lua_pushnumber(L, 0.0); lua_setfield(L, -2, "time");
    lua_pushinteger(L, 0);  lua_setfield(L, -2, "frame");
    lua_pop(L, 1);

    if (missing > 0)
        VK_LOG_ERROR("LUA-API %d of %d entries have no implementation",
                     missing, apiEntryCount());
    VK_LOG_INFO("LUA-API registered %d functions in %d namespaces",
                apiEntryCount() - missing, int(namespaces.size()));
}

// ---------------------------------------------------------------- 外部函数
//
// 游戏 C++ 模块注册进来的函数（M5-c）。签名与 `game/VennAbi.h` 的
// `VennFunc` **逐字相同**，但这里**不 include** 那个头：script 层不该认识
// game 层（`engine/game` 在 `engine/script` 之上）。两边靠"参数与返回值
// 都是 double"这条口头契约连着 —— 改一边必须同时改另一边，而
// `GameModule` 那边是 `reinterpret_cast` 过去的，编译器**不会**替你发现。
// 所以真正兜底的是 `--selftest game` 里那条"AIB 表没有空函数指针"。
static int externalDispatch(lua_State* L) {
    using ExtFn = int (*)(const double*, int, double*, int);
    auto fn = reinterpret_cast<ExtFn>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!fn) return 0;

    // 上限 8 个参数：超过这个数的接口说明设计错了（要么该传表，要么
    // 该拆成两个函数）。**截断而不是报错**是刻意的 —— 报错会让一份
    // 已经跑起来的脚本在某次小改动后整段停摆。
    const int n = lua_gettop(L);
    const int argc = (n > 8) ? 8 : n;
    double args[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < argc; ++i) args[i] = luaL_checknumber(L, i + 1);

    double ret = 0.0;
    const int nr = fn(args, argc, &ret, 1);
    if (nr > 0) {
        lua_pushnumber(L, ret);
        return 1;
    }
    return 0;
}

void LuaBackend::addExternalFunction(const std::string& ns, const std::string& name,
                                     int (*fn)(const double*, int, double*, int)) {
    if (!m_L || ns.empty() || name.empty() || !fn) {
        VK_LOG_WARN("LUA-EXT ignored bad registration '%s.%s'", ns.c_str(),
                    name.c_str());
        return;
    }

    lua_getglobal(m_L, ns.c_str());
    if (!lua_istable(m_L, -1)) {
        lua_pop(m_L, 1);
        lua_newtable(m_L);
        lua_pushvalue(m_L, -1);
        lua_setglobal(m_L, ns.c_str());   // 栈顶仍是那张表
    }

    lua_getfield(m_L, -1, name.c_str());
    const bool shadowed = lua_isfunction(m_L, -1);
    lua_pop(m_L, 1);
    if (shadowed) {
        // 覆盖引擎 API 是危险动作（`world` / `actor` 那 12 个命名空间），
        // 但不该静默失败：打一行 WARN，然后**以游戏模块为准**。
        VK_LOG_WARN("LUA-EXT %s.%s overrides an engine function (game module wins)",
                    ns.c_str(), name.c_str());
    }

    lua_pushlightuserdata(m_L, reinterpret_cast<void*>(fn));
    lua_pushcclosure(m_L, &externalDispatch, 1);
    lua_setfield(m_L, -2, name.c_str());
    lua_pop(m_L, 1);

    VK_LOG_INFO("LUA-EXT registered %s.%s", ns.c_str(), name.c_str());
}

} // namespace vks
