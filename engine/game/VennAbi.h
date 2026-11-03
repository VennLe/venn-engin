#pragma once
// ============================================================
// game/VennAbi —— 游戏 C++ 模块与引擎之间**唯一的**二进制契约
//
// 这个头文件是**纯 C**、零依赖、只含 POD 和函数指针。它会被**原样复制**
// 到每个游戏工程的 `code/include/venn/VennAbi.h`：
//
//     <工程>/code/           ← 独立 CMake 工程，只 include 这一份快照
//     <工程>/scripts/...     ← Lua（决策层）
//
// 于是游戏工程**不链接引擎的任何一个 .lib**，编译只要 MSVC，不用
// Vulkan / glm / 第三方目录。游戏侧产出 `<Name>Game.dll`，引擎
// `LoadLibrary` + `GetProcAddress` 把它挂上来。
//
// ------------------------------------------------------------
// 为什么是"宿主把函数表传进去"，而不是"游戏 DLL 链接引擎库"
// ------------------------------------------------------------
// `VennEngine` 是 STATIC 库。DLL 一旦链接它，链接器会把用到的目标文件
// **复制一份**进 DLL —— 于是 `World` / `Logger` / 各种全局单例在进程里
// 有**两份**：DLL 里那份是空的，引擎里那份是真的。症状是"游戏模块
// 里 log 打到另一个日志、找实体永远找不到"，而且**不报错**。
//
// 所以规则只有一条：**DLL 不链接任何引擎库**。它需要的每一样东西都从
// 这张表里拿。表由宿主在 `onRegister` 时传进来，DLL 自己保存指针。
//
// ------------------------------------------------------------
// 诚实说明它**不是**什么
// ------------------------------------------------------------
// · 不是"用 C++ 重写玩法"的入口 —— 这个引擎的判据仍然是
//   **Lua 做决策、C++ 做执行**。C++ 模块的定位是"Lua 做不了/做不快的事"：
//   第三方库（物理/寻路/序列化）、大数组计算、需要原生线程的活儿。
//   编排和数值公式留在 Lua，否则热重载没了、迭代速度掉一个数量级。
// · 不是热重载友好的：改了 C++ 要重编译 + 重启。**这是刻意的取舍**，
//   所以它不设为默认，创建工程时是个可选项。
//
// ------------------------------------------------------------
// 版本纪律
// ------------------------------------------------------------
// `VENN_ABI_VERSION` 只在**删字段 / 改语义**时 +1；**加字段不升版本**
// （新字段追加在末尾、旧 DLL 靠 `structSize` 判断能不能读）。
// 版本不匹配时加载器**拒载并打一行 ERROR**，绝不"尽力而为地调"——
// 那种崩在 `onTick` 里、栈不完整，比拒载难查十倍。
// ============================================================

#include <stdint.h>

// 当前 ABI 版本。改语义才 +1，加字段不动。
#define VENN_ABI_VERSION 1u

// 导出宏。游戏工程只需要在 `vennModuleEntry` 上用一次。
#if defined(_MSC_VER)
#  define VENN_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#  define VENN_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// 加载器按这个名字找入口，别改。
#define VENN_MODULE_ENTRY_SYMBOL "vennModuleEntry"

// ---------------------------------------------------------------- 基础类型
//
// ⚠ 全用 POD，**不许**出现 `glm::vec3` / `std::string` / `ecs::Entity`：
//   跨 DLL 传 STL 类型要求两边 CRT、编译器版本、迭代器调试级别完全一致，
//   一旦不一致就是"Debug 下能跑、Release 下静默乱码"。
//   字符串一律 `const char*`（UTF-8），引擎侧负责转换与生命周期。
struct VennVec3 {
    float x;
    float y;
    float z;
};

enum VennLogLevel {
    VENN_LOG_INFO = 0,
    VENN_LOG_WARN = 1,
    VENN_LOG_ERROR = 2
};

// 暴露给 Lua 的函数的统一签名。
//
// 参数与返回值全走 `double`，因为 Lua 5.4 的数值就一种（64 位浮点/
// 整数统一）。
//
// ⚠ **返回值只能是数值**（含 0/1 当布尔用）。这是刻意的边界：
//   返回值一旦能是任意结构或字符串，Lua 侧就得懂 C++ 的类型，
//   这套东西会立刻长成一个反射系统 —— 而"不做反射"是本引擎写死的取舍。
//   要传字符串就用 `world.setName` 那类既有 API，别在这里开口子。
//
// 返回值：0 = 没有返回值（Lua 里得到 nil），1 = 一个数（在 ret[0]）。
typedef int (*VennFunc)(const double* args, int argc, double* ret, int retCap);

// 函数表。**由宿主填满、按值传指针**；DLL 只读。
struct VennAbi {
    // 本结构体的字节数。旧 DLL 用它判断"宿主给的表到哪为止"。
    uint32_t structSize;
    uint32_t abiVersion;

    // ---- 日志（**不要**用 printf 往 stdout 打：引擎有统一日志与等级）----
    void (*log)(int level, const char* msg);

    // ---- 场景只读查询 ----
    // 名字找不到返回 **0**（0 是唯一"空句柄"，与 Lua 侧同一编码）
    uint64_t (*entityByName)(const char* name);
    int (*entityAlive)(uint64_t handle);
    // 无效句柄返回 (0,0,0) —— **不报错**，因为"实体刚被删"是正常时序
    VennVec3 (*entityPos)(uint64_t handle);
    int (*entityCount)(void);

    // ---- 场景写（窄面：只给这两个，够表达绝大多数玩法）----
    void (*entitySetPos)(uint64_t handle, VennVec3 p);
    void (*entitySetVisible)(uint64_t handle, int visible);

    // ---- 时间 ----
    float (*deltaTime)(void);
    float (*elapsedTime)(void);

    // ---- 把一个 C 函数挂成 Lua 的 `ns.name` ----
    //
    // ⚠ **只在 `onRegister` 里调**。注册表在 BeginPlay 之前一次性建好，
    //   之后再注册**不会生效**（加载器会打 WARN）。
    //   `ns` 与引擎自带的 12 个命名空间重名时，**游戏模块优先**并打一行
    //   WARN —— 覆盖引擎 API 是危险动作，但不该静默失败。
    void (*registerFunction)(const char* ns, const char* name, VennFunc fn);
};

// 模块描述。DLL 导出一个 `vennModuleEntry()` 返回它的指针。
//
// 生命周期（宿主保证的调用序）：
//   LoadLibrary → vennModuleEntry() → 版本校验 → onRegister(&abi)
//   → （每次进入播放）onBeginPlay → 每帧 onTick(dt) ... → onEndPlay
//   → （退出时）FreeLibrary
//
// ⚠ `onRegister` 之外的回调里**不要**保存 `abi` 的副本指针 —— 直接存
//   `onRegister` 收到的那个指针即可，它指向宿主里的静态表，进程内不变。
struct VennModuleDesc {
    uint32_t abiVersion;
    const char* name;      // "MyGame"（日志前缀，也是 UI 显示名）
    const char* version;   // 游戏自己的版本串，随便写

    void (*onRegister)(const VennAbi* abi);
    void (*onBeginPlay)(void);
    void (*onTick)(float dt);
    void (*onEndPlay)(void);
};

// 入口签名。
typedef const VennModuleDesc* (*VennModuleEntryFn)(void);

// ---------------------------------------------------------------- 写模块的套路
//
// 下面这段是每个游戏工程 `src/GameModule.cpp` 的骨架（生成器会写出来，
// 这里留着是为了让读 ABI 的人一眼看懂它怎么用）：
//
//     static const VennAbi* g_abi = nullptr;
//     static int MyFunc(const double* a, int n, double* r, int cap) {
//         if (n < 1) { g_abi->log(VENN_LOG_WARN, "MyFunc needs 1 arg"); return 0; }
//         r[0] = a[0] * 2.0;   // 用之前先判 cap
//         return 1;
//     }
//     static void OnRegister(const VennAbi* abi) {
//         g_abi = abi;
//         abi->registerFunction("mygame", "doubleIt", &MyFunc);
//     }
//     VENN_MODULE_EXPORT const VennModuleDesc* vennModuleEntry(void) {
//         static const VennModuleDesc d = {
//             VENN_ABI_VERSION, "MyGame", "1.0.0",
//             &OnRegister, nullptr, nullptr, nullptr
//         };
//         return &d;
//     }
// ============================================================
