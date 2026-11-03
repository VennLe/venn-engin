#pragma once
// ============================================================
// game/GameModule —— 加载并驱动**游戏自己的 C++ 模块**（路线图 M5-c）
//
// 一个游戏工程可以有一个 `<Name>Game.dll`（由 `<工程>/code/` 那个独立
// CMake 工程编出来）。这个类负责：
//
//   LoadLibrary → vennModuleEntry() → 校验 ABI 版本 → onRegister(表)
//   →（进播放）onBeginPlay → 每帧 onTick(dt) →（退播放）onEndPlay
//   →（换工程 / 退出）FreeLibrary
//
// ------------------------------------------------------------
// 四条不让步的纪律
// ------------------------------------------------------------
// ① **DLL 不链接引擎**（`VennAbi.h` 里写了理由）。所有能力从函数表拿。
//
// ② **版本不匹配就拒载**，并打一行带两个版本号的 ERROR。绝不"尽力而为
//    地调" —— 那种崩在 `onTick` 里、栈不完整，比拒载难查十倍。
//
// ③ **注册函数只在 `onRegister` 窗口内有效**。窗口关掉之后再调
//    `registerFunction` → 打 WARN 并忽略（不是静默丢：静默丢的表现是
//    "我明明注册了，Lua 里就是找不到"，这是本项目最忌讳的那类 bug）。
//
// ④ **卸载前一定 `FreeLibrary`**，且**不缓存 DLL 里的任何裸指针**。
//    换工程时旧模块必须先走完 `onEndPlay` 再卸 —— 否则它的 `onTick`
//    下一帧就会踩到已经释放的代码段（访问冲突，栈在系统 DLL 里）。
//
// ------------------------------------------------------------
// 一个工程一个模块
// ------------------------------------------------------------
// 不做"多模块依赖图"：这个引擎的玩法编排在 Lua，C++ 模块是**补充**，
// 一个工程一个 DLL 足够。多模块要处理加载序、符号冲突、跨模块调用，
// 收益是零、复杂度是 L。真需要时再单开一档。
// ============================================================

#include "game/VennAbi.h"

#include <string>
#include <vector>

namespace scene {
class Scene;
}

namespace game {

// 一条"游戏模块注册给 Lua 的函数"。
struct ExternalFn {
    std::string ns;
    std::string name;
    VennFunc fn = nullptr;
};

// 加载器的状态（也是 ABI 函数的宿主）。
//
// ⚠ 单例：ABI 表里的函数是**裸函数指针**，没有 `this` 可传，只能指回
//   一个进程内唯一的宿主。这一条决定了"一个进程只能有一个游戏模块"。
class GameModule {
public:
    static GameModule& instance();

    // 加载 `<dllPath>`（UTF-8 路径）。成功时 `err` 不变；失败时 `err` 是人话，
    // 且**保证不会留下半加载状态**（HMODULE 已释放、函数表已清空）。
    bool load(const std::string& dllPath, scene::Scene* sc, std::string* err);

    // 卸载。没加载时是空操作。**先 endPlay 再 FreeLibrary**。
    void unload();

    bool loaded() const { return m_desc != nullptr; }
    // 已经在"播放中"（`beginPlay` 之后、`endPlay` 之前）。
    // 宿主用它做"懒开始"：初始化早期还没有场景，等第一次 tick 再 begin。
    bool playing() const { return m_playing; }
    const std::string& name() const { return m_name; }
    const std::string& dllPath() const { return m_path; }

    // 模块注册的函数（已按 `ns.name` 去重，重名保留**第一条**并打过 WARN）。
    const std::vector<ExternalFn>& functions() const { return m_funcs; }

    // 取走函数表并清空 —— 应用层在把函数挂到 Lua 之后调用。
    // 为什么要清：Lua 后端可能被重建（热重载 / 换工程），重建后要重新挂；
    // 留着旧的一直累积会让"同一个名字注册到两张表"变成不确定行为。
    std::vector<ExternalFn> takeFunctions();

    void beginPlay(scene::Scene* sc);
    void tick(float dt, scene::Scene* sc);
    void endPlay();

    // ABI 函数内部用：当前场景（可能为空 —— 编辑器没打开场景时）。
    scene::Scene* scene() const { return m_scene; }

    // 注册窗口是否开着（只有 `onRegister` 期间为 true）。
    bool registering() const { return m_registering; }

    // `onRegister` 期间收集一条注册。返回 false = 被拒（重名 / 不在窗口内）。
    bool addFunction(const char* ns, const char* name, VennFunc fn);

    // 时间（ABI 的 `deltaTime` / `elapsedTime` 从这里读）。
    float deltaTime() const { return m_dt; }
    float elapsedTime() const { return m_elapsed; }

private:
    GameModule() = default;
    ~GameModule() = default;
    GameModule(const GameModule&) = delete;
    GameModule& operator=(const GameModule&) = delete;

    void reset();

#if defined(_WIN32)
    void* m_osHandle = nullptr;   // HMODULE（用 void* 免得头文件拖 windows.h）
#endif
    const VennModuleDesc* m_desc = nullptr;
    std::string m_name;
    std::string m_path;
    std::vector<ExternalFn> m_funcs;

    scene::Scene* m_scene = nullptr;
    bool m_registering = false;
    bool m_playing = false;
    float m_dt = 0.0f;
    float m_elapsed = 0.0f;
};

// 宿主侧的 ABI 函数表（进程内唯一实例）。
const VennAbi* abiTable();

// ---------------------------------------------------------------- 自检
//
// 无 DLL 也能跑的部分：ABI 表完整性（没有空函数指针）/ 版本号 /
// 注册窗口纪律（窗口外注册必须被拒）。真加载 DLL 的用例在
// `--selftest game` 里按"工程里有没有 code/bin/*.dll"决定 SKIP。
//
// 返回失败条数，打 `GAME-T ...`。
int runGameModuleSelfTest();

} // namespace game
