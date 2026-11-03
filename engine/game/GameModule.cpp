#include "game/GameModule.h"

#include "core/Logger.h"
#include "core/Utf8.h"
#include "ecs/Components.h"
#include "ecs/EntityHandle.h"
#include "scene/Scene.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace game {
namespace {

// ---------------------------------------------------------------- ABI 实现
//
// 这些函数是**裸函数指针**被 DLL 调用的，没有 `this` 可传 —— 一律回到
// `GameModule::instance()`。所以进程内只能有一个游戏模块（见头文件）。
GameModule& host() { return GameModule::instance(); }

void abiLog(int level, const char* msg) {
    const char* text = msg ? msg : "(null)";
    switch (level) {
        case VENN_LOG_ERROR: VK_LOG_ERROR("GAME %s", text); break;
        case VENN_LOG_WARN:  VK_LOG_WARN("GAME %s", text); break;
        default:             VK_LOG_INFO("GAME %s", text); break;
    }
}

uint64_t abiEntityByName(const char* name) {
    scene::Scene* sc = host().scene();
    if (!sc || !name) return ecs::kEmptyHandle;
    return ecs::packHandle(sc->find(name));
}

int abiEntityAlive(uint64_t handle) {
    scene::Scene* sc = host().scene();
    if (!sc) return 0;
    return sc->world().alive(ecs::unpackHandle(handle)) ? 1 : 0;
}

VennVec3 abiEntityPos(uint64_t handle) {
    VennVec3 out{0.0f, 0.0f, 0.0f};
    scene::Scene* sc = host().scene();
    if (!sc) return out;
    const ecs::Entity e = ecs::unpackHandle(handle);
    if (!sc->world().alive(e)) return out;   // 死句柄 = 原点，不报错（正常时序）
    const glm::vec3 p(sc->worldMatrix(e)[3]);
    out.x = p.x;
    out.y = p.y;
    out.z = p.z;
    return out;
}

int abiEntityCount() {
    scene::Scene* sc = host().scene();
    if (!sc) return 0;
    // 与 Lua 的 `world.count` 同一个口径（数**有 Transform** 的实体）
    int n = 0;
    sc->world().each<ecs::TransformComponent>(
        [&](ecs::Entity, ecs::TransformComponent&) { ++n; });
    return n;
}

void abiEntitySetPos(uint64_t handle, VennVec3 p) {
    scene::Scene* sc = host().scene();
    if (!sc) return;
    const ecs::Entity e = ecs::unpackHandle(handle);
    if (!sc->world().alive(e)) return;
    // 走 `Scene::setWorldPos` —— 与 Lua 的 `world.setPos` **同一条实现**
    sc->setWorldPos(e, glm::vec3(p.x, p.y, p.z));
}

void abiEntitySetVisible(uint64_t handle, int visible) {
    scene::Scene* sc = host().scene();
    if (!sc) return;
    const ecs::Entity e = ecs::unpackHandle(handle);
    if (!sc->world().alive(e)) return;
    ecs::World& w = sc->world();
    auto* v = w.get<ecs::VisibilityComponent>(e);
    if (!v) v = &w.add<ecs::VisibilityComponent>(e);
    v->visible = (visible != 0);
}

float abiDeltaTime() { return host().deltaTime(); }
float abiElapsedTime() { return host().elapsedTime(); }

void abiRegisterFunction(const char* ns, const char* name, VennFunc fn) {
    host().addFunction(ns, name, fn);
}

// 进程内唯一的表。**按值静态初始化**：字段顺序与 `VennAbi` 定义严格一致，
// 加字段时这里必须同步（编译期能抓 —— 少一个字段就是初始化器数量不匹配）。
const VennAbi kAbi = {
    static_cast<uint32_t>(sizeof(VennAbi)),
    VENN_ABI_VERSION,
    &abiLog,
    &abiEntityByName,
    &abiEntityAlive,
    &abiEntityPos,
    &abiEntityCount,
    &abiEntitySetPos,
    &abiEntitySetVisible,
    &abiDeltaTime,
    &abiElapsedTime,
    &abiRegisterFunction,
};

} // namespace

const VennAbi* abiTable() { return &kAbi; }

GameModule& GameModule::instance() {
    static GameModule inst;
    return inst;
}

void GameModule::reset() {
    m_desc = nullptr;
    m_name.clear();
    m_path.clear();
    m_funcs.clear();
    m_scene = nullptr;
    m_registering = false;
    m_playing = false;
    m_dt = 0.0f;
    m_elapsed = 0.0f;
}

bool GameModule::load(const std::string& dllPath, scene::Scene* sc, std::string* err) {
    if (m_desc) unload();   // 换工程：旧模块先走完 endPlay 再卸

#if !defined(_WIN32)
    if (err) *err = "game modules are only supported on Windows";
    return false;
#else
    const std::wstring wide = core::utf8ToWide(dllPath);
    if (wide.empty()) {
        if (err) *err = "game module path is empty or not valid UTF-8";
        return false;
    }

    // ⚠ 先查文件在不在：`LoadLibraryW` 对"路径不存在"只给
    //   `error 126 (ERROR_MOD_NOT_FOUND)`，那句话说人话是"找不到模块"，
    //   而真实原因九成是 **`code/` 建了但从来没编译过**。这个提示值一句
    //   reload —— 用户看到 "DLL not built yet" 就知道该去点 F7 了。
    {
        std::error_code fec;
        if (!std::filesystem::is_regular_file(std::filesystem::path(wide), fec)) {
            if (err) *err = "game module DLL not found (build the code project first): " + dllPath;
            VK_LOG_WARN("GAME-MODULE dll not found path=%s", dllPath.c_str());
            return false;
        }
    }

    HMODULE h = ::LoadLibraryW(wide.c_str());
    if (!h) {
        const DWORD code = ::GetLastError();
        if (err) {
            char b[512];
            std::snprintf(b, sizeof(b), "LoadLibrary failed (error %lu) for %s",
                          static_cast<unsigned long>(code), dllPath.c_str());
            *err = b;
        }
        VK_LOG_ERROR("GAME-MODULE load failed error=%lu path=%s",
                     static_cast<unsigned long>(code), dllPath.c_str());
        return false;
    }

    auto entry = reinterpret_cast<VennModuleEntryFn>(
        reinterpret_cast<void*>(::GetProcAddress(h, VENN_MODULE_ENTRY_SYMBOL)));
    if (!entry) {
        if (err) {
            char b[512];
            std::snprintf(b, sizeof(b),
                          "module does not export %s (is it a Venn game module?)",
                          VENN_MODULE_ENTRY_SYMBOL);
            *err = b;
        }
        VK_LOG_ERROR("GAME-MODULE missing entry symbol=%s path=%s",
                     VENN_MODULE_ENTRY_SYMBOL, dllPath.c_str());
        ::FreeLibrary(h);
        return false;
    }

    const VennModuleDesc* desc = entry();
    if (!desc) {
        if (err) *err = "vennModuleEntry returned null";
        VK_LOG_ERROR("GAME-MODULE entry returned null path=%s", dllPath.c_str());
        ::FreeLibrary(h);
        return false;
    }

    // ② 版本不匹配就拒载。这里**不能**宽容：DLL 用的是它编译时的结构体
    //    布局，多一个 / 少一个字段都会让函数指针错位 —— 调起来就是野指针。
    if (desc->abiVersion != VENN_ABI_VERSION) {
        if (err) {
            char b[256];
            std::snprintf(b, sizeof(b),
                          "ABI mismatch: module=%u engine=%u (rebuild the game project)",
                          unsigned(desc->abiVersion), unsigned(VENN_ABI_VERSION));
            *err = b;
        }
        VK_LOG_ERROR("GAME-MODULE abi mismatch module=%u engine=%u path=%s",
                     unsigned(desc->abiVersion), unsigned(VENN_ABI_VERSION),
                     dllPath.c_str());
        ::FreeLibrary(h);
        return false;
    }

    m_osHandle = reinterpret_cast<void*>(h);
    m_desc = desc;
    m_name = desc->name ? desc->name : "(unnamed)";
    m_path = dllPath;
    m_scene = sc;
    m_elapsed = 0.0f;
    m_dt = 0.0f;

    // 打开注册窗口。**只有这一段**允许 registerFunction。
    if (desc->onRegister) {
        m_registering = true;
        desc->onRegister(&kAbi);
        m_registering = false;
    } else {
        VK_LOG_WARN("GAME-MODULE no onRegister: module cannot expose functions");
    }

    VK_LOG_INFO("GAME-MODULE loaded name=%s version=%s abi=%u functions=%d path=%s",
                m_name.c_str(), desc->version ? desc->version : "?",
                unsigned(VENN_ABI_VERSION), int(m_funcs.size()), dllPath.c_str());
    return true;
#endif
}

void GameModule::unload() {
    if (!m_desc) return;
    if (m_playing) endPlay();
#if defined(_WIN32)
    if (m_osHandle) {
        VK_LOG_INFO("GAME-MODULE unload name=%s", m_name.c_str());
        ::FreeLibrary(reinterpret_cast<HMODULE>(m_osHandle));
    }
#endif
    m_osHandle = nullptr;
    reset();
}

std::vector<ExternalFn> GameModule::takeFunctions() {
    std::vector<ExternalFn> out;
    out.swap(m_funcs);
    return out;
}

bool GameModule::addFunction(const char* ns, const char* name, VennFunc fn) {
    if (!ns || !name || !*ns || !*name) {
        VK_LOG_WARN("GAME-MODULE registerFunction: empty namespace or name");
        return false;
    }
    if (!m_registering) {
        // ③ 窗口外注册：**打 WARN 并拒绝**。静默丢的话，用户看到的是
        //    "我明明注册了、Lua 里就是找不到"，而且找不到任何线索。
        VK_LOG_WARN("GAME-MODULE registerFunction(%s.%s) ignored: only allowed "
                    "inside onRegister", ns, name);
        return false;
    }
    if (!fn) {
        VK_LOG_WARN("GAME-MODULE registerFunction(%s.%s): null function", ns, name);
        return false;
    }
    for (const auto& f : m_funcs) {
        if (f.ns == ns && f.name == name) {
            VK_LOG_WARN("GAME-MODULE duplicate function %s.%s (first wins)", ns, name);
            return false;
        }
    }
    m_funcs.push_back(ExternalFn{ns, name, fn});
    return true;
}

void GameModule::beginPlay(scene::Scene* sc) {
    m_scene = sc;
    m_elapsed = 0.0f;
    m_dt = 0.0f;
    if (!m_desc) return;
    m_playing = true;
    if (m_desc->onBeginPlay) m_desc->onBeginPlay();
}

void GameModule::tick(float dt, scene::Scene* sc) {
    m_scene = sc;
    m_dt = dt;
    m_elapsed += dt;
    if (!m_desc || !m_playing) return;
    if (m_desc->onTick) m_desc->onTick(dt);
}

void GameModule::endPlay() {
    if (!m_desc) return;
    if (!m_playing) return;
    m_playing = false;
    if (m_desc->onEndPlay) m_desc->onEndPlay();
}

// ---------------------------------------------------------------- 自检
namespace {

int g_pass = 0;
int g_fail = 0;

void check(const char* name, bool ok, const std::string& detail = std::string()) {
    if (ok) {
        ++g_pass;
        std::printf("GAME-T pass %s %s\n", name, detail.c_str());
    } else {
        ++g_fail;
        std::printf("GAME-T fail %s %s\n", name, detail.c_str());
    }
}

} // namespace

int runGameModuleSelfTest() {
    g_pass = 0;
    g_fail = 0;

    const VennAbi* t = abiTable();
    check("A1_table_present", t != nullptr);
    if (!t) {
        std::printf("GAME-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
        return g_fail;
    }

    check("A2_struct_size", t->structSize == sizeof(VennAbi),
          "size=" + std::to_string(t->structSize) + " expected=" +
              std::to_string(sizeof(VennAbi)));
    check("A3_abi_version", t->abiVersion == VENN_ABI_VERSION,
          "table=" + std::to_string(t->abiVersion) +
              " header=" + std::to_string(VENN_ABI_VERSION));

    // ⚠ 这一条是"加字段忘了填"的唯一判据：结构体加了字段、初始化器没跟，
    //   编译器只会给个警告或者补 0 —— 运行时就是调空指针。
    const bool allFilled = t->log && t->entityByName && t->entityAlive &&
                           t->entityPos && t->entityCount && t->entitySetPos &&
                           t->entitySetVisible && t->deltaTime && t->elapsedTime &&
                           t->registerFunction;
    check("A4_no_null_entries", allFilled);

    // 没加载模块时：函数表不能崩、注册必须被拒。
    check("A5_not_loaded", !GameModule::instance().loaded(),
          "loaded=" + std::string(GameModule::instance().loaded() ? "1" : "0"));

    const size_t before = GameModule::instance().functions().size();
    const bool accepted = GameModule::instance().addFunction("t", "f", nullptr);
    check("A6_register_outside_window_rejected", !accepted,
          "accepted=" + std::string(accepted ? "1" : "0") +
              " funcs=" + std::to_string(GameModule::instance().functions().size()) +
              " before=" + std::to_string(before));

    // ABI 函数在"没有场景"时必须是安全空操作（编辑器的启动早期、
    // 或者工程还没打开就 tick 一次的情形）。
    check("A7_no_scene_is_safe",
          t->entityByName("nope") == 0 && t->entityAlive(0) == 0 &&
              t->entityCount() == 0,
          "name=" + std::to_string(t->entityByName("nope")) +
              " alive=" + std::to_string(t->entityAlive(0)) +
              " count=" + std::to_string(t->entityCount()));

    const VennVec3 p = t->entityPos(0);
    check("A8_dead_handle_pos_is_zero",
          p.x == 0.0f && p.y == 0.0f && p.z == 0.0f);

    // ---- A9~A11：真加载一个 DLL。`MYVK_GAME_DLL=<路径>` 才跑。
    //
    // 这条是"创建工程"和"运行游戏"之间的接缝：`--selftest project` 那边
    // 真编出一个 DLL，把它喂进来，才能证明**引擎认得自己生成的模块**
    // （ABI 版本、入口符号名、导出宏、注册窗口全对得上）。
    // 平时不设这个变量 → 三条 SKIP，不干扰日常回归。
    //
    // ⚠ 为什么用环境变量而不是"自动去扫 code/bin/"：自检不应该有隐藏的
    //   外部依赖 —— 哪天某个工程的 DLL 编坏了，会让**无关的**自检变红。
    //   显式传路径 = 谁跑谁知道自己在测什么。
    if (const char* dllPath = std::getenv("MYVK_GAME_DLL")) {
        std::string err;
        const bool ok = GameModule::instance().load(dllPath, nullptr, &err);
        check("A9_load_generated_dll", ok, ok ? std::string(dllPath) : err);
        if (ok) {
            const std::string n = GameModule::instance().name();
            std::vector<ExternalFn> fns = GameModule::instance().takeFunctions();
            check("A10_module_identity", !n.empty(), "name='" + n + "'");
            // 生成的模板至少注册 2 个：入口里的 ping + 按类型的示例函数
            check("A11_functions_registered", fns.size() >= 2,
                  "n=" + std::to_string(fns.size()) +
                      (fns.empty() ? "" : " first=" + fns[0].ns + "." + fns[0].name));
            GameModule::instance().unload();
            check("A12_unload_clean", !GameModule::instance().loaded());
        }
    } else {
        std::printf("GAME-T skip A9_load_generated_dll "
                    "(set MYVK_GAME_DLL=<path> to test a real module)\n");
    }

    std::printf("GAME-TOTAL pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}

} // namespace game
