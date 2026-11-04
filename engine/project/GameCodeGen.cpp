#include "project/GameCodeGen.h"

#include "core/Logger.h"
#include "core/Utf8.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cwctype>   // towlower（环境变量名去重）
#include <iterator>  // std::size（MAX_PATH 缓冲）
#include <map>
#include <vector>

#if defined(_WIN32)
// ⚠ `ShellExecuteW` **不在** windows.h 里，必须单独 include shellapi.h
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace project {
namespace {

// ---------------------------------------------------------------- 小工具
std::string gsl(const fs::path& p) { return p.generic_string(); }

bool writeText(const fs::path& p, const std::string& text, std::string* err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) {
        if (err) *err = "cannot create " + gsl(p.parent_path()) + ": " + ec.message();
        return false;
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (err) *err = "cannot write " + gsl(p);
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        if (err) *err = "write failed (disk full?) " + gsl(p);
        return false;
    }
    return true;
}

// 引擎自带的 ABI 头在哪。
//
// 两条候选，**第一个存在的赢**：
//   ① 编译期源码路径（`__FILE__` 推导）—— 开发机上永远是对的那份；
//   ② `<exe>/include/venn/VennAbi.h` —— CMake 在构建时拷过去的一份，
//      发布版没有源码树时走这条。
//
// ⚠ 不走"内嵌一份文本"：内嵌 = 引擎里有两份 ABI 定义，改了头忘了改文本，
//   生成出来的游戏工程会拿旧结构体去对新的函数表 —— 这是最坏的一种漂移
//   （编译通过、运行到 `onTick` 才炸）。
std::string abiHeaderSource() {
    std::vector<fs::path> cands;

    const fs::path self(__FILE__);
    // <repo>/engine/project/GameCodeGen.cpp → <repo>/engine/game/VennAbi.h
    cands.push_back(self.parent_path().parent_path() / "game" / "VennAbi.h");

#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 2] = {0};
    if (::GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf))) > 0) {
        fs::path exeDir = fs::path(buf).parent_path();
        cands.push_back(exeDir / "include" / "venn" / "VennAbi.h");
        // 开发机：exe 在 <repo>/build/bin/ → 往上三层还有个 engine/
        cands.push_back(exeDir / ".." / ".." / ".." / "engine" / "game" / "VennAbi.h");
    }
#endif

    for (const auto& c : cands) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) return gsl(fs::weakly_canonical(c, ec));
    }
    return std::string();
}

std::string readAll(const fs::path& p, bool* ok) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        if (ok) *ok = false;
        return std::string();
    }
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (ok) *ok = static_cast<bool>(f) || f.eof();
    return s;
}

// 生成文本里的占位符替换。**只替换我们自己的那四个**（与 ProjectCreate
// 的白名单纪律同源：全量替换会改坏用户内容）。
std::string fill(const std::string& tpl, const std::string& moduleId,
                 const std::string& projectName, const std::string& typeKey) {
    // Lua 命名空间：模块标识全小写（"MyGameGame" → "mygamegame"）。
    // 与引擎自带的 12 个命名空间（world / actor / game / …）撞名时，
    // 加载器会打 WARN 并以游戏模块为准 —— 刻意保留，不静默。
    std::string ns;
    ns.reserve(moduleId.size());
    for (char c : moduleId)
        ns.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);

    std::string s = tpl;
    auto replaceAll = [](std::string& s2, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s2.find(from, pos)) != std::string::npos) {
            s2.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(s, "@MODULE@", moduleId);
    replaceAll(s, "@PROJECT@", projectName);
    replaceAll(s, "@TYPE@", typeKey);
    replaceAll(s, "@NS@", ns);
    return s;
}

// ---------------------------------------------------------------- 模板
const char* kCMakeTpl = R"GEN(# ============================================================
# @PROJECT@ —— 游戏 C++ 模块（由 venn 引擎生成）
#
# 这是一个**独立**的 CMake 工程：不 find_package 引擎、不链接引擎的
# 任何 .lib、不需要 Vulkan / glm / 第三方目录。它只 include 一份
# `include/venn/VennAbi.h`（引擎 ABI 的快照）。
#
# 为什么能独立：模块与引擎之间只有一张 C 函数表（见 VennAbi.h 头注释）。
# 一旦链接引擎的 STATIC 库，链接器会把 World / Logger 等全局对象**复制
# 一份**进 DLL —— 进程里就有两份，DLL 那份是空的，症状是"日志打到别处、
# 找实体永远找不到"，而且不报错。所以这条红线不能碰。
# ============================================================
cmake_minimum_required(VERSION 3.20)

project(@MODULE@ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(@MODULE@ SHARED
    src/GameModule.cpp
    src/Gameplay.cpp
)

target_include_directories(@MODULE@ PRIVATE include)

if (MSVC)
    # /utf-8：源文件里有中文注释时不加会按 GBK 读，报 C4819 并可能吃掉字符
    target_compile_options(@MODULE@ PRIVATE /utf-8 /W4)
endif()

# ------------------------------------------------------------
# ⚠⚠ 产物必须落在 `code/bin/`，而且**五个配置全设**。
#
# Visual Studio 是多配置生成器：只设 `RUNTIME_OUTPUT_DIRECTORY` 时，
# Debug 配置仍然会落到 `bin/Debug/`。引擎按 `code/bin/@MODULE@.dll`
# 找模块 —— 对不上就是"编译成功了、游戏里一点反应都没有"，
# 而且两边都不报错。这是这个功能最容易踩的一脚。
# ------------------------------------------------------------
set(_out "${CMAKE_CURRENT_SOURCE_DIR}/bin")
set_target_properties(@MODULE@ PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_out}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${_out}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${_out}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${_out}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${_out}"
)
)GEN";

const char* kGameModuleTpl = R"GEN(// ============================================================
// @PROJECT@ —— 游戏 C++ 模块入口
//
// 这个文件是**你的**，随便改。它做三件事：
//   ① 保存引擎给的函数表（`onRegister` 里那一份）
//   ② 把本模块的函数注册成 Lua 能调的名字
//   ③ 生命周期钩子：BeginPlay / Tick / EndPlay
//
// ------------------------------------------------------------
// 什么时候该写在这里，什么时候该写在 Lua
// ------------------------------------------------------------
// 这个引擎的判据是 **Lua 做决策、C++ 做执行**：
//   · 玩法编排、数值公式、状态机      → Lua（可以热重载，改完不用重启）
//   · 每帧几百次以上的数学、确定性算法、第三方 C++ 库 → 这里
// 写在这里的代价是"改一行要重编译 + 重启"，所以默认别往这儿放。
//
// ------------------------------------------------------------
// 三条容易踩的
// ------------------------------------------------------------
// ① `registerFunction` **只能在 onRegister 里调**。窗口关掉之后再调会被
//    拒绝并打 WARN（不会静默丢，但也不会生效）。
// ② 实体句柄是 `uint64_t`，**0 = 空**。别把 0 当实体用。
// ③ 参数与返回值都走 `double`，返回值只能是数值 —— 这是刻意的边界，
//    见 VennAbi.h 里 `VennFunc` 的注释。
// ============================================================
#include "venn/VennAbi.h"

#include <cmath>

namespace {

// 引擎给的函数表。进程内唯一、地址不变，存下来一直用。
const VennAbi* g_abi = nullptr;

// 一个最小的示例函数：`@NS@.ping()` 返回 1。
// 它的用途是"证明这条链路是通的" —— 在 Lua 里调一次就知道模块加载成没成。
int LuaPing(const double* args, int argc, double* ret, int retCap) {
    (void)args;
    (void)argc;
    if (retCap < 1) return 0;
    ret[0] = 1.0;
    return 1;
}

} // namespace

// 由 src/Gameplay.cpp 提供：注册本类型的示例函数。
void GameplayBind(const VennAbi* abi);

// ---------------------------------------------------------------- 生命周期

static void OnRegister(const VennAbi* abi) {
    g_abi = abi;

    abi->log(VENN_LOG_INFO, "@PROJECT@: game module registering");

    // `ns.name` —— Lua 里就是 `@NS@.ping()`
    abi->registerFunction("@NS@", "ping", &LuaPing);

    // 类型专属的示例（冲刺曲线 / 头部摇晃 / 格子对齐 / 洗牌键），
    // 实现在 src/Gameplay.cpp。不需要就整段删掉。
    GameplayBind(abi);
}

static void OnBeginPlay() {
    // 每次"开始播放"都会进来（编辑器里反复 Play/Stop 也会）。
    // 这里的计数**必须**能重入：别在这里分配不释放的资源。
    if (g_abi) {
        g_abi->log(VENN_LOG_INFO, "@PROJECT@: begin play");
    }
}

static void OnTick(float dt) {
    (void)dt;
    // 每帧调用。没有每帧活儿就留空 —— 空函数是零成本的。
}

static void OnEndPlay() {
    if (g_abi) {
        g_abi->log(VENN_LOG_INFO, "@PROJECT@: end play");
    }
}

// ---------------------------------------------------------------- 导出
//
// 加载器按这个符号找模块（`VENN_MODULE_ENTRY_SYMBOL`）。
// `abiVersion` 与引擎不一致时会被**拒载**并打 ERROR，这是好事：
// 结构体布局变了还硬调，崩在 onTick 里根本没法查。
VENN_MODULE_EXPORT const VennModuleDesc* vennModuleEntry(void) {
    static const VennModuleDesc desc = {
        VENN_ABI_VERSION,
        "@PROJECT@",
        "1.0.0",
        &OnRegister,
        &OnBeginPlay,
        &OnTick,
        &OnEndPlay,
    };
    return &desc;
}
)GEN";

// Gameplay.cpp：按游戏类型给一段**真的有用**的 C++ 示例。
// 选的都是"Lua 写起来别扭或需确定性"的东西，而不是为了填满文件。
const char* kGameplayThirdPerson = R"GEN(// 第三人称：冲刺位移曲线
//
// 为什么要用 C++：这是逐帧积分的位移曲线，Lua 里也能写，但每次 Play
// 都要在 VM 里跑一遍浮点循环；而且它要"给定时间点算出精确位移"，
// 用 C++ 更容易保证与帧率无关。
#include "venn/VennAbi.h"

#include <cmath>

void GameplayBind(const VennAbi* abi);

// Lua: `@NS@.dashDistance(speed, t, duration)` → 距离（米）
//
//   曲线：前 30% 线性加速到 speed，之后按 cos 衰减到 0。
//   用途：脚本只做"按没按冲刺键"，位移交给这里。
static int LuaDashDistance(const double* args, int argc, double* ret, int retCap) {
    if (argc < 3 || retCap < 1) return 0;
    const double speed = args[0];
    const double t = args[1];
    const double duration = args[2];
    if (duration <= 0.0) {
        ret[0] = 0.0;
        return 1;
    }
    const double k = t / duration;
    if (k <= 0.0) {
        ret[0] = 0.0;
        return 1;
    }
    if (k >= 1.0) {
        // 整段积分闭式解（避免"跑完还继续加"）
        ret[0] = speed * duration * 0.5;
        return 1;
    }
    const double ramp = 0.30;
    if (k < ramp) {
        ret[0] = speed * duration * (k * k) / (2.0 * ramp);
    } else {
        const double a = speed * duration * ramp * 0.5;
        const double phase = (k - ramp) / (1.0 - ramp);
        ret[0] = a + speed * duration * (std::sin(phase * 3.14159265358979) *
                                         (1.0 - ramp) / 3.14159265358979);
    }
    return 1;
}

void GameplayBind(const VennAbi* abi) {
    abi->registerFunction("@NS@", "dashDistance", &LuaDashDistance);
}
)GEN";

const char* kGameplayFirstPerson = R"GEN(// 第一人称：头部呼吸 / 走动摇晃
//
// 为什么要用 C++：这是每帧一次的三角函数叠加，值要**确定**（同参数同结果），
// 而且它在相机热路径上 —— 放 Lua 里每帧一次 VM 调用不划算。
#include "venn/VennAbi.h"

#include <cmath>

void GameplayBind(const VennAbi* abi);

// Lua: `@NS@.headBob(t, amp, speed)` → 竖直偏移（米）
//
// 两段不同频率的正弦相加 = 走路的"上下 + 轻微左右"手感，
// 单一正弦看起来像在电梯里。
static int LuaHeadBob(const double* args, int argc, double* ret, int retCap) {
    if (argc < 3 || retCap < 1) return 0;
    const double t = args[0];
    const double amp = args[1];
    const double speed = args[2];
    const double a = std::sin(t * speed);
    const double b = std::sin(t * speed * 2.0) * 0.35;
    ret[0] = (a + b) * amp;
    return 1;
}

void GameplayBind(const VennAbi* abi) {
    abi->registerFunction("@NS@", "headBob", &LuaHeadBob);
}
)GEN";

const char* kGameplayIsometric = R"GEN(// 俯视等距 / 战棋：格子对齐
//
// 为什么要用 C++：战棋的"格子"必须是**确定性**的，四舍五入的边界行为
// 要在一个地方定死；Lua 的浮点行为虽然一样，但每帧每个单位都跑一遍 VM
// 调用没必要。
#include "venn/VennAbi.h"

#include <cmath>

void GameplayBind(const VennAbi* abi);

// Lua: `@NS@.snapToCell(v, cell)` → 最近的格子中心
//
// ⚠ 用 `floor(v / cell + 0.5)` 而不是 `round()`：`round()` 在
//   -0.5 / +0.5 上的舍入方向不统一，会让左右两边的格子宽度差一像素，
//   战棋里表现为"往左走比往右走多一格"。
static int LuaSnapToCell(const double* args, int argc, double* ret, int retCap) {
    if (argc < 2 || retCap < 1) return 0;
    const double v = args[0];
    const double cell = args[1];
    if (cell <= 0.0) {
        ret[0] = v;
        return 1;
    }
    ret[0] = (std::floor(v / cell + 0.5)) * cell;
    return 1;
}

void GameplayBind(const VennAbi* abi) {
    abi->registerFunction("@NS@", "snapToCell", &LuaSnapToCell);
}
)GEN";

const char* kGameplayCardBattler = R"GEN(// 卡牌对战：确定性洗牌
//
// 为什么要用 C++：洗牌必须是**可复现**的 —— 同一个种子在任何机器、
// 任何帧率下都得出同一个牌序（回放、录像、联机同步都靠这条）。
// 自己实现一个明确的整数 PRNG，比依赖 Lua 的 math.random 版本差异稳。
#include "venn/VennAbi.h"

#include <cstdint>

namespace {

// SplitMix64 —— 小、快、跨平台结果一致（不依赖 `rand()` 的实现差异）。
uint64_t Mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

} // namespace

void GameplayBind(const VennAbi* abi);

// Lua: `@NS@.shuffleKey(seed, cardIndex)` → 这张牌的**排序键**（0..1）
//
// 洗牌本身不在这里做 —— 这里只给每张牌一个确定的键，Lua 侧按键排序就是
// 牌序。这样：
//   · 同一个 seed 在任何机器、任何帧率下得到同一个牌序（回放 / 联机同步）；
//   · 存档里只要存 seed，不用存整副牌序；
//   · C++ 侧只出一个数 —— 正好卡在"AIB 返回值只能是数值"的边界内。
//
// ⚠ 别在这里返回"第 i 张是原始第几张"：无状态的 Fisher-Yates 需要知道
//   之前换过谁，单点求值算不出来（第一版就是这么写错的 —— 循环里
//   `break` 掉了，看起来像洗牌，其实每次返回同一个下标）。
static int LuaShuffleKey(const double* args, int argc, double* ret, int retCap) {
    if (argc < 2 || retCap < 1) return 0;
    const uint64_t seed = static_cast<uint64_t>(args[0]);
    const uint64_t idx = static_cast<uint64_t>(args[1]);
    const uint64_t h = Mix(seed ^ (idx * 0x9E3779B97F4A7C15ull));
    // 取高 53 位折成 [0,1)：Lua 的 double 在这个区间能精确表示整数
    ret[0] = static_cast<double>(h >> 11) / 9007199254740992.0;
    return 1;
}

void GameplayBind(const VennAbi* abi) {
    abi->registerFunction("@NS@", "shuffleKey", &LuaShuffleKey);
}
)GEN";

const char* kReadmeTpl = R"GEN(# @PROJECT@ —— 游戏代码工程

由 venn 引擎在创建工程时生成。这是一个**独立**的 C++ 工程：
双击 `@MODULE@.sln`（或者用 VS 打开它），按 F7 编译。

## 它和引擎的关系

只有一层：`include/venn/VennAbi.h`（引擎 ABI 的快照）。
编译产物 `bin/@MODULE@.dll` 由引擎加载，引擎把一张 C 函数表传进来，
你的代码通过这张表读场景、写位置、往 Lua 暴露函数。

**不要**链接引擎的 `.lib` —— 理由见 `VennAbi.h` 开头（会有两份全局对象）。

## 目录

```
@MODULE@.sln              ← 用 VS 打开这个
@MODULE@.vcxproj          ← x64 Debug / Release
CMakeLists.txt            ← 等价的 cmake 描述（CI 或不装 VS 时用）
include/venn/VennAbi.h    ← 引擎 ABI 快照，别改
src/GameModule.cpp        ← 入口 + 生命周期 + 注册
src/Gameplay.cpp          ← 按游戏类型给的示例函数
bin/@MODULE@.dll          ← 产物（引擎从这里加载）
```

## 怎么编

**路线一（推荐）：** 打开 `@MODULE@.sln`，选 `Debug|x64` 或 `Release|x64`，
生成解决方案。产物固定落在 `bin/@MODULE@.dll`。

**路线二：** 命令行
```
msbuild @MODULE@.sln /p:Configuration=Debug /p:Platform=x64
```

**路线三：** cmake（产物位置由 `CMakeLists.txt` 保证一致）
```
cmake -S . -B build && cmake --build build --config Release
```

编完引擎下次启动（或重新 Play）就会加载它。

⚠ 引擎本身是 x64 的，所以这里也只有 x64 —— 换成 Win32 会在加载时报
"不是有效的 Win32 应用程序"。

## 怎么被 Lua 调用

`src/GameModule.cpp` 的 `OnRegister` 里：

```cpp
abi->registerFunction("@NS@", "sample", &LuaSample);
```

Lua 侧：

```lua
local v = @NS@.sample(1, 2)
```

## 什么时候该写这里

| 放 Lua | 放这里 |
|---|---|
| 玩法编排、状态机、数值公式 | 每帧几百次以上的数学 |
| 需要热重载反复调的东西 | 确定性算法（洗牌、寻路） |
| 事件响应、UI 流程 | 第三方 C++ 库的封装 |

代价：改一行要重编译 + 重启。所以默认别往这儿放。

## 三个坑

1. `registerFunction` 只在 `onRegister` 里有效，之后调用会被拒并打 WARN。
2. 实体句柄 `uint64_t`，**0 = 空**，不是"0 号实体"。
3. ABI 版本不一致 → 引擎**拒载**并打一行 `GAME-MODULE abi mismatch`。
   升级引擎后如果加载失败，先看这行；重新生成工程或对齐 `include/venn/VennAbi.h` 即可。
)GEN";

const char* kGitignore = "build/\nbin/\n.vs/\n";

// ---------------------------------------------------------------- VS 工程文本
//
// 直接写 `.sln` / `.vcxproj`，**不调 cmake**。三个理由：
//   ① 用户机器上可能只装了 VS Build Tools（或只有 MSBuild）—— cmake 不过是
//      "生成器"，真正编译的仍是 MSBuild。少一层外部依赖就少一类失败。
//   ② cmake configure 要跑 1~3 秒，而且在**代理环境变量大小写重复**的机器上
//      会莫名报 `MSB6001: CL.exe 命令行开关无效`（见 buildDedupEnvBlock）。
//      这三个文件是纯文本，写它们不需要任何外部程序、不需要联网、瞬间完成。
//   ③ 产物**不含绝对路径**（cmake 会把 `<dir>/CMakeCache.txt` 里的绝对路径
//      写进 .vcxproj）—— 所以整个 `code/` 目录可以直接拷给别人、可以进
//      版本库、可以在 rename 之前的临时目录里就写好。
//
// `CMakeLists.txt` 仍然保留：同一份源文件的等价构建描述，给 CI / 非 VS 用。
// 两边源文件列表由自检 `_vs_sources_match` 盯着，改一处忘改另一处会红。

// 工程 GUID 必须**确定性**：同一个工程每次重新生成时 GUID 不变，否则 VS 会
// 把它当两个工程，`git diff` 也每次全红。
// 做法：从 moduleId 算两个 FNV-1a 64 位散列拼成 128 位，再按 UUID v4 的位布局
// 把版本位/变体位改成固定值（VS 不看这两位，但格式要合法）。
std::string projectGuid(const std::string& seed) {
    auto fnv = [](const std::string& s, uint64_t h) {
        for (unsigned char c : s) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;   // FNV-1a 64 位质数
        }
        return h;
    };
    const uint64_t a = fnv(seed, 14695981039346656037ull);
    const uint64_t b = fnv(seed + "#2", 14695981039346656037ull);

    unsigned char g[16];
    for (int i = 0; i < 8; ++i)
        g[i] = static_cast<unsigned char>((a >> (56 - i * 8)) & 0xFFull);
    for (int i = 0; i < 8; ++i)
        g[8 + i] = static_cast<unsigned char>((b >> (56 - i * 8)) & 0xFFull);
    g[6] = static_cast<unsigned char>((g[6] & 0x0F) | 0x40);   // version = 4
    g[8] = static_cast<unsigned char>((g[8] & 0x3F) | 0x80);   // variant = RFC 4122

    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
                  "%02X%02X%02X%02X%02X%02X}",
                  g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9],
                  g[10], g[11], g[12], g[13], g[14], g[15]);
    return std::string(buf);
}

// `.sln` / `.vcxproj` 用 CRLF。MSBuild 两种都吃，但 VS 自己存盘写 CRLF，
// 用 LF 会让"第一次打开还没改任何东西，文件就已显示被修改"。
std::string toCrlf(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 16);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) out += "\r\n";
        else out.push_back(s[i]);
    }
    return out;
}

// C++ 工程的 project type GUID —— VS 里所有 .vcxproj 都是这个固定值，
// 不是我们编的（`{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}` = C++ 项目类型）。
const char* kVsCppProjectTypeGuid = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";

std::string buildSlnText(const std::string& moduleId, const std::string& guid) {
    std::string s;
    s += "Microsoft Visual Studio Solution File, Format Version 12.00\n";
    s += "# Visual Studio Version 17\n";
    s += "VisualStudioVersion = 17.0.31903.59\n";
    s += "MinimumVisualStudioVersion = 10.0.40219.1\n";
    s += std::string("Project(\"") + kVsCppProjectTypeGuid + "\") = \"" + moduleId +
         "\", \"" + moduleId + ".vcxproj\", \"" + guid + "\"\n";
    s += "EndProject\n";
    s += "Global\n";
    s += "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n";
    s += "\t\tDebug|x64 = Debug|x64\n";
    s += "\t\tRelease|x64 = Release|x64\n";
    s += "\tEndGlobalSection\n";
    s += "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n";
    for (const char* cfg : {"Debug", "Release"}) {
        const std::string c(cfg);
        s += "\t\t" + guid + "." + c + "|x64.ActiveCfg = " + c + "|x64\n";
        s += "\t\t" + guid + "." + c + "|x64.Build.0 = " + c + "|x64\n";
    }
    s += "\tEndGlobalSection\n";
    s += "\tGlobalSection(SolutionProperties) = preSolution\n";
    s += "\t\tHideSolutionNode = FALSE\n";
    s += "\tEndGlobalSection\n";
    s += "EndGlobal\n";
    return toCrlf(s);
}

// 源文件列表只在这里写一次 —— `.vcxproj` / `.filters` / `CMakeLists.txt`
// 三处的列表必须一致，改一处忘改另一处，自检 `_vs_sources_match` 会红。
const char* kSources[] = {"src/GameModule.cpp", "src/Gameplay.cpp"};

std::string vcxPath(const char* rel) {
    std::string p(rel);
    for (char& c : p)
        if (c == '/') c = '\\';   // MSBuild 只吃反斜杠
    return p;
}

std::string buildVcxprojText(const std::string& moduleId, const std::string& guid,
                             const std::string& toolset) {
    std::string s;
    s += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    s += "<Project DefaultTargets=\"Build\" "
         "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n";

    s += "  <ItemGroup Label=\"ProjectConfigurations\">\n";
    for (const char* cfg : {"Debug", "Release"}) {
        s += std::string("    <ProjectConfiguration Include=\"") + cfg + "|x64\">\n";
        s += std::string("      <Configuration>") + cfg + "</Configuration>\n";
        s += "      <Platform>x64</Platform>\n";
        s += "    </ProjectConfiguration>\n";
    }
    s += "  </ItemGroup>\n";

    s += "  <PropertyGroup Label=\"Globals\">\n";
    s += "    <VCProjectVersion>17.0</VCProjectVersion>\n";
    s += "    <ProjectGuid>" + guid + "</ProjectGuid>\n";
    s += "    <RootNamespace>" + moduleId + "</RootNamespace>\n";
    s += "    <Keyword>Win32Proj</Keyword>\n";
    // ⚠ 刻意**不写** `<WindowsTargetPlatformVersion>`：写死 `10.0.19041.0`
    //   在没装那个版本 SDK 的机器上直接报"找不到 SDK"。留空 = 用最新已装的。
    s += "  </PropertyGroup>\n";

    s += "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n";

    s += "  <PropertyGroup Label=\"Configuration\">\n";
    s += "    <ConfigurationType>DynamicLibrary</ConfigurationType>\n";
    // ⚠ Toolset 由生成时探测（v143 = VS2022 / v142 = VS2019 / v141 = VS2017）。
    //   写死 v143 的话，只装了 VS2019 的机器 MSBuild 直接报
    //   "MSB8020: 找不到 v143 的生成工具"。探测不到时退回 v143（最新）。
    s += "    <PlatformToolset>" + toolset + "</PlatformToolset>\n";
    s += "    <CharacterSet>Unicode</CharacterSet>\n";
    s += "  </PropertyGroup>\n";

    s += "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n";

    // ⚠⚠ 产物目录。引擎按 `code/bin/<Module>.dll` 加载 —— 这里**必须**
    //    让所有配置都落到 `bin\`。VS 的默认是 `$(SolutionDir)$(Configuration)\`
    //    （Debug 落 `Debug\`），对不上就是"编译成功了、游戏里一点反应都没有"，
    //    而且两边都不报错。这是这个功能最容易踩的一脚。
    //    `$(ProjectDir)` 自带结尾反斜杠，所以是 `$(ProjectDir)bin\`。
    s += "  <PropertyGroup>\n";
    s += "    <OutDir>$(ProjectDir)bin\\</OutDir>\n";
    s += "    <IntDir>$(ProjectDir)build\\$(Configuration)\\</IntDir>\n";
    s += "    <TargetName>" + moduleId + "</TargetName>\n";
    s += "  </PropertyGroup>\n";

    s += "  <ItemDefinitionGroup>\n";
    s += "    <ClCompile>\n";
    s += "      <LanguageStandard>stdcpp17</LanguageStandard>\n";
    s += "      <AdditionalIncludeDirectories>$(ProjectDir)include;"
         "%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>\n";
    // /utf-8：源文件里有中文注释，不加会按 GBK 读 → C4819 并可能吃掉字符
    s += "      <AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>\n";
    s += "      <WarningLevel>Level4</WarningLevel>\n";
    // 不关 `/sdl`（VS 默认的"安全开发生命周期检查"）：我们的代码是干净的，
    // 留着还能在用户往里写代码时挡掉一批 `strcpy` 类问题。
    s += "    </ClCompile>\n";
    s += "    <Link>\n";
    s += "      <SubSystem>Windows</SubSystem>\n";
    s += "    </Link>\n";
    s += "  </ItemDefinitionGroup>\n";

    s += "  <ItemGroup>\n";
    for (const char* src : kSources)
        s += std::string("    <ClCompile Include=\"") + vcxPath(src) + "\" />\n";
    s += "  </ItemGroup>\n";

    s += "  <ItemGroup>\n";
    s += "    <ClInclude Include=\"include\\venn\\VennAbi.h\" />\n";
    s += "  </ItemGroup>\n";

    s += "  <Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n";
    s += "</Project>\n";
    return toCrlf(s);
}

// VS 的"解决方案资源管理器"里的文件夹树。可有可无，但没有的话所有文件
// 平铺在工程根下 —— 对"只有 3 个文件"的小工程无所谓，但它是 VS 新建 C++
// 项目时的默认产物，缺了会让用户觉得这工程是野路子生成的。
std::string buildFiltersText() {
    std::string s;
    s += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    s += "<Project ToolsVersion=\"4.0\" "
         "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n";
    s += "  <ItemGroup>\n";
    s += "    <Filter Include=\"src\">\n";
    s += "      <UniqueIdentifier>" + projectGuid("filter:src") +
         "</UniqueIdentifier>\n";
    s += "    </Filter>\n";
    s += "    <Filter Include=\"include\">\n";
    s += "      <UniqueIdentifier>" + projectGuid("filter:include") +
         "</UniqueIdentifier>\n";
    s += "    </Filter>\n";
    s += "  </ItemGroup>\n";
    s += "  <ItemGroup>\n";
    for (const char* src : kSources) {
        s += std::string("    <ClCompile Include=\"") + vcxPath(src) +
             "\">\n      <Filter>src</Filter>\n    </ClCompile>\n";
    }
    s += "  </ItemGroup>\n";
    s += "  <ItemGroup>\n";
    s += "    <ClInclude Include=\"include\\venn\\VennAbi.h\">\n";
    s += "      <Filter>include</Filter>\n";
    s += "    </ClInclude>\n";
    s += "  </ItemGroup>\n";
    s += "</Project>\n";
    return toCrlf(s);
}

// ----------------------------------------------------- Visual Studio 探测

// 本机 VS 的几个关键路径。**全部靠探测**，不写死盘符：
// 实测这台机器装在 `D:\Microsoft Visual Studio\2022\Professional` ——
// 按 `%ProgramFiles%\Microsoft Visual Studio\<年>\<版>` 去猜是猜不到的。
// 唯一可靠的入口是 VS 安装器自带的 `vswhere.exe`（VS2017 起固定在这个位置）。
struct VsInfo {
    std::string installDir;   // D:\...\2022\Professional
    std::string devenv;       // ...\Common7\IDE\devenv.exe
    std::string msbuild;      // ...\MSBuild\Current\Bin\MSBuild.exe
    std::string toolset;      // v143 / v142 / v141
};

#ifdef _WIN32
// 跑一个进程并把它的 stdout 收回来（UTF-8 字节）。
//
// ⚠ `vswhere` 的输出按系统 ANSI 代码页出来，路径里若有中文会被当成
//   GBK —— 这时 `vsInfo()` 的路径就找不到文件，后果是**退回** ShellExecute
//   打开 .sln / SKIP 掉真编译，不会静默用错路径。所以这里不做编码转换，
//   宁可"探测失败走兜底"也不猜。
bool runCapture(const std::wstring& cmdLine, std::string& out, int timeoutSeconds) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;   // 管道句柄必须可继承

    HANDLE rd = INVALID_HANDLE_VALUE, wr = INVALID_HANDLE_VALUE;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return false;
    // 读端**不**给子进程继承，否则父进程读完也等不到 EOF
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    // GUI 进程（Launcher / 编辑器）没有控制台，`GetStdHandle(STD_INPUT)`
    // 返回 nullptr —— 那种句柄传给 CreateProcessW 会直接 87。
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                               OPEN_EXISTING, 0, nullptr);
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : ::GetStdHandle(STD_INPUT_HANDLE);

    std::wstring mutableCmd = cmdLine;
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(wr);   // 父进程必须放掉写端，否则 ReadFile 永远等不到 EOF
    if (!ok) {
        ::CloseHandle(rd);
        if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
        return false;
    }

    char buf[4096];
    DWORD got = 0;
    while (::ReadFile(rd, buf, DWORD(sizeof(buf)), &got, nullptr) && got > 0)
        out.append(buf, got);

    ::WaitForSingleObject(pi.hProcess, DWORD(timeoutSeconds) * 1000u);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(rd);
    if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);

    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return !out.empty();
}

// `VC\Tools\MSVC\14.44.35207` 这样的目录名 → `v143`。
// 只认前三段：14.4x / 14.3x 都是 v143（VS2022），14.2x = v142，14.1x = v141。
std::string toolsetFromMsvcDirs(const std::string& vcToolsDir) {
    std::error_code ec;
    fs::path best;
    for (const auto& e : fs::directory_iterator(fs::path(vcToolsDir), ec)) {
        if (!e.is_directory()) continue;
        if (best.empty() || e.path().filename().string() > best.filename().string())
            best = e.path();
    }
    if (best.empty()) return "v143";   // 探测不到就按最新的猜
    const std::string v = best.filename().string();   // 14.44.35207
    if (v.rfind("14.4", 0) == 0 || v.rfind("14.3", 0) == 0) return "v143";
    if (v.rfind("14.2", 0) == 0) return "v142";
    if (v.rfind("14.1", 0) == 0) return "v141";
    return "v143";
}

VsInfo detectVs() {
    VsInfo info;
    info.toolset = "v143";

    const char* pf86 = std::getenv("ProgramFiles(x86)");
    if (!pf86) pf86 = "C:\\Program Files (x86)";
    const fs::path vswhere = fs::path(pf86) / "Microsoft Visual Studio" / "Installer" /
                             "vswhere.exe";
    std::error_code ec;
    if (!fs::is_regular_file(vswhere, ec)) {
        VK_LOG_INFO("GAMECODE vswhere not found, Visual Studio assumed absent");
        return info;
    }

    // `-latest -products *`：`*` 是为了把 Build Tools 也算进来 —— 只装 Build
    // Tools 的机器没有 devenv，但有 MSBuild，工程照样能编。
    std::string out;
    if (!runCapture(L"\"" + vswhere.wstring() +
                        L"\" -latest -products * -property installationPath",
                    out, 20)) {
        VK_LOG_WARN("GAMECODE vswhere produced nothing");
        return info;
    }
    // 多行时取第一行（`-latest` 正常只有一行，防御一下）
    const size_t nl = out.find('\n');
    if (nl != std::string::npos) out.resize(nl);

    info.installDir = out;
    info.devenv = (fs::path(out) / "Common7" / "IDE" / "devenv.exe").string();
    info.msbuild = (fs::path(out) / "MSBuild" / "Current" / "Bin" / "MSBuild.exe").string();
    info.toolset = toolsetFromMsvcDirs((fs::path(out) / "VC" / "Tools" / "MSVC").string());
    VK_LOG_INFO("GAMECODE vs=%s toolset=%s", info.installDir.c_str(), info.toolset.c_str());
    return info;
}
#else
VsInfo detectVs() {
    VsInfo info;
    info.toolset = "v143";
    return info;
}
#endif

// 探测一次，全进程复用（`vswhere` 要起一个进程，别每次建工程都跑）。
const VsInfo& vsInfo() {
    static const VsInfo info = detectVs();
    return info;
}

} // namespace

// ================================================================
// 公共接口
// ================================================================

std::string moduleIdFromName(const std::string& projectName) {    // ⚠ 按 **UTF-8 码位** 折叠，不是按字节：一个汉字占 3 个字节，按字节
    //   替换会得到 3 个下划线 —— "我的游戏" 变成 12 个 `_`，名字完全看不出
    //   原样。按码位折叠才是 4 个。
    std::string base;
    base.reserve(projectName.size());
    for (size_t i = 0; i < projectName.size();) {
        const unsigned char c = static_cast<unsigned char>(projectName[i]);
        if (c < 0x80) {
            const bool ascii =
                (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '_';
            base.push_back(ascii ? static_cast<char>(c) : '_');
            ++i;
            continue;
        }
        // 非 ASCII：吃掉整个码位的后续字节，输出一个 '_'
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        i += len;
        base.push_back('_');
    }
    // 首字符不能是数字（C++ 标识符 + 某些生成器的限制）
    if (!base.empty() && base[0] >= '0' && base[0] <= '9') base.insert(base.begin(), 'G');
    if (base.empty()) base = "Game";
    if (base.size() > 48) base.resize(48);
    return base + "Game";
}

GameCodeResult writeGameCode(const GameCodeSpec& spec) {
    GameCodeResult r;
    r.codeDir = normalizePath(spec.codeDir);
    if (r.codeDir.empty()) {
        r.error = "game code directory is empty";
        return r;
    }

    const fs::path dir(r.codeDir);
    r.moduleId = spec.moduleId.empty() ? moduleIdFromName(spec.projectName) : spec.moduleId;
    const std::string title = spec.projectName.empty() ? std::string("Game") : spec.projectName;
    const std::string typeKey = gameTypeId(spec.gameType);

    // ---- ABI 快照。找不到就是硬错误：没有它的工程一定编不过，
    //      与其让用户对着 20 行 include 错误发愣，不如在这里说清楚。
    const std::string abiSrc = abiHeaderSource();
    if (abiSrc.empty()) {
        r.error = "cannot locate engine's VennAbi.h (looked next to the source tree "
                  "and under <exe>/include/venn/)";
        return r;
    }
    bool readOk = false;
    const std::string abiText = readAll(abiSrc, &readOk);
    if (!readOk || abiText.empty()) {
        r.error = "cannot read " + abiSrc;
        return r;
    }

    struct Item {
        // ⚠ 这里**必须是 std::string**，不能是 `const char*`：文件名带
        //   moduleId 时要现拼（`(r.moduleId + ".sln")`），拼出来的临时
        //   std::string 在语句结束就析构 —— 存 `const char*` 会留下悬垂指针，
        //   后面 `r.files.push_back(it.rel)` 读到的是已经被释放的内存。
        std::string rel;
        std::string text;
    };
    std::vector<Item> items;

    items.push_back({"CMakeLists.txt", fill(kCMakeTpl, r.moduleId, title, typeKey)});
    items.push_back({"README.md", fill(kReadmeTpl, r.moduleId, title, typeKey)});
    items.push_back({".gitignore", std::string(kGitignore)});
    items.push_back({"include/venn/VennAbi.h", abiText});

    // ---- Visual Studio 工程（.sln 在 `code/` 根，与 CMakeLists.txt 同级）
    const std::string guid = projectGuid(r.moduleId);
    items.push_back({(r.moduleId + ".sln").c_str(),
                     buildSlnText(r.moduleId, guid)});
    items.push_back({(r.moduleId + ".vcxproj").c_str(),
                     buildVcxprojText(r.moduleId, guid, vsInfo().toolset)});
    items.push_back({(r.moduleId + ".vcxproj.filters").c_str(), buildFiltersText()});

    // `src/GameModule.cpp` 需要引用 GameplayBind（由 Gameplay.cpp 提供）
    // —— 见下面 kGameModuleTpl 里那段 extern 声明。
    std::string moduleSrc = fill(kGameModuleTpl, r.moduleId, title, typeKey);

    const char* gameplay = kGameplayThirdPerson;
    switch (spec.gameType) {
        case GameType::FirstPerson: gameplay = kGameplayFirstPerson; break;
        case GameType::Isometric:   gameplay = kGameplayIsometric; break;
        case GameType::CardBattler: gameplay = kGameplayCardBattler; break;
        default:                    gameplay = kGameplayThirdPerson; break;
    }
    items.push_back({"src/GameModule.cpp", moduleSrc});
    items.push_back({"src/Gameplay.cpp", fill(gameplay, r.moduleId, title, typeKey)});

    for (const auto& it : items) {
        std::string err;
        const fs::path p = dir / it.rel;
        if (!writeText(p, it.text, &err)) {
            r.error = err;
            return r;
        }
        r.files.push_back(it.rel);
    }

    r.dllName = r.moduleId + ".dll";
    r.dllRelPath = "code/bin/" + r.dllName;
    r.slnPath = gsl(dir / (r.moduleId + ".sln"));
    r.ok = true;
    VK_LOG_INFO("GAMECODE wrote %d files module=%s type=%s dir=%s",
                int(r.files.size()), r.moduleId.c_str(), typeKey.c_str(),
                r.codeDir.c_str());
    return r;
}

// ================================================================
// 子进程（只在自检"真编一次"和"打开 VS"上用）
// ================================================================

#if defined(_WIN32)

namespace {

// 构造一份**大小写去重**的环境块。
//
// ⚠⚠ 为什么必须自己构造（这是本功能踩到的最深一个坑，两个 bug 叠在一起）：
//
//   [1] 为什么需要去重：
//   MSBuild 用**大小写敏感的字典**装环境变量。Windows 的环境变量名本身
//   大小写不敏感，但同一个进程里可以同时存在 `HTTPS_PROXY` 和
//   `https_proxy` 两条（装了代理工具 + 手写过一条，就会这样 —— 本机实测
//   正好有 HTTP_PROXY/HTTPS_PROXY 两组）。
//   于是 MSBuild 在 `ToolTask.GetProcessStartInfo` 里
//   `Hashtable.Insert` 抛 `ArgumentException: 已添加项`，
//   最终报成 **`MSB6001: "CL.exe" 的命令行开关无效`** ——
//   报错文本跟环境变量毫无关系，照字面查一辈子也查不到。
//
//   [2] 为什么"传了环境块就失败"（`error=87`）：
//   传 Unicode 环境块时 `dwCreationFlags` **必须**带
//   `CREATE_UNICODE_ENVIRONMENT`。不带的话系统按 ANSI 块去解析那串 UTF-16
//   字节，直接返回 `ERROR_INVALID_PARAMETER (87)` —— 看起来像命令行写错。
//   （2026-11-03 用 ctypes 直接调 CreateProcessW 对比实测：48 字符的小块
//     不带 flag 也是 87，带上 flag 连 27370 字符的真实环境也一次成功。
//     **和块大小完全无关**，之前怀疑的"超 32K 上限"是错的。）
//
//   Windows 语义上变量名大小写不敏感，所以去重不改变任何行为。
std::wstring buildDedupEnvBlock() {
#if defined(_WIN32)
    LPWCH raw = ::GetEnvironmentStringsW();
    if (!raw) return std::wstring();

    std::vector<std::wstring> keep;
    std::map<std::wstring, bool> seen;
    for (LPWCH p = raw; *p;) {
        std::wstring line(p);
        p += line.size() + 1;

        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0) {
            // `=C:=D:\path` 这种伪变量（当前目录）没有名字，原样保留
            keep.push_back(line);
            continue;
        }
        std::wstring lower = line.substr(0, eq);
        for (wchar_t& c : lower)
            c = static_cast<wchar_t>(::towlower(static_cast<wint_t>(c)));
        if (seen.find(lower) != seen.end()) continue;   // 重复（忽略大小写）
        seen[lower] = true;
        keep.push_back(line);
    }
    ::FreeEnvironmentStringsW(raw);

    std::wstring block;
    for (const auto& l : keep) {
        block += l;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');   // 双 NUL 结尾

    // 诊断：条目数 / 字节数 / 第一条。`CreateProcessW` 返回 87
    // (ERROR_INVALID_PARAMETER) 时，唯一能判断"是不是环境块格式错"的信息。
    VK_LOG_INFO("GAMECODE env block entries=%zu chars=%zu first='%s'",
                keep.size(), block.size(),
                core::wideToUtf8(keep.empty() ? std::wstring() : keep[0]).c_str());

    return block;
#else
    return std::wstring();
#endif
}

// 跑一个进程并等它结束。返回退出码；-1 = 起不来，-2 = 超时。
//
// `logPath` 非空时把子进程的 stdout/stderr **重定向到那个文件**。
// ⚠ 必须重定向：用 `CREATE_NO_WINDOW` 时输出直接丢进虚空，MSBuild 编译失败
//   时用户（和排查的人）只能看到一句 "exit 1"，连哪一行错都看不到。
//   这是本项目反复吃亏的一类："失败了，但没有任何线索"。
int runAndWait(const std::wstring& cmdLine, int timeoutSeconds,
               const std::string& logPath) {
    std::wstring mutableCmd = cmdLine;   // CreateProcessW 可能要写这块内存
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;            // 不要弹一个黑窗口出来
    PROCESS_INFORMATION pi{};

    HANDLE logFile = INVALID_HANDLE_VALUE;
    HANDLE nulHandle = INVALID_HANDLE_VALUE;
    if (!logPath.empty()) {
        // ⚠ 句柄必须**可继承**（`bInheritHandles = TRUE` + `STARTF_USESTDHANDLES`
        //   要求）。`CreateFileW` 的 `lpSecurityAttributes = nullptr` 建出来的
        //   是**不可继承**句柄 —— 那种情况下 `CreateProcessW` 直接返回
        //   `ERROR_INVALID_PARAMETER (87)`，看起来像"命令行写错了"。
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = nullptr;
        sa.bInheritHandle = TRUE;

        const std::wstring wlog = core::utf8ToWide(logPath);
        logFile = ::CreateFileW(wlog.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile != INVALID_HANDLE_VALUE) {
            // GUI 进程（Launcher / 编辑器）没有控制台，`GetStdHandle(STD_INPUT)`
            // 返回 nullptr —— stdin 一律用 NUL（cmake 不读 stdin）。
            nulHandle = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                                      OPEN_EXISTING, 0, nullptr);
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = logFile;
            si.hStdError = logFile;
            si.hStdInput = (nulHandle != INVALID_HANDLE_VALUE)
                               ? nulHandle
                               : ::GetStdHandle(STD_INPUT_HANDLE);
        }
    }

    // 重定向句柄必须可继承 → `bInheritHandles = TRUE`
    // 环境块：去重后再传（见 `buildDedupEnvBlock` 的长注释）。
    // ⚠ `MYVK_GAMECODE_RAWENV=1` 时不传环境块（继承父进程）—— 排查用：
    //   "传环境块就 87、不传就正常"能一步定位是环境块的问题。
    std::wstring envBlock;
    if (!std::getenv("MYVK_GAMECODE_RAWENV")) envBlock = buildDedupEnvBlock();
    LPVOID envPtr = envBlock.empty() ? nullptr : &envBlock[0];

    // ⚠⚠ `CREATE_UNICODE_ENVIRONMENT` **不是可选项**：`envPtr` 指向 UTF-16
    //    块，不带这个 flag 系统会按 ANSI 解析 → `ERROR_INVALID_PARAMETER (87)`。
    //    （就是这一行让整条链路卡了很久，见 buildDedupEnvBlock 的注释 [2]。）
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    const BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr,
                                     logFile != INVALID_HANDLE_VALUE ? TRUE : FALSE,
                                     flags, envPtr, nullptr, &si, &pi);
    if (!ok) {
        const DWORD err = ::GetLastError();
        VK_LOG_ERROR("GAMECODE CreateProcess failed error=%lu cmd=%s",
                     static_cast<unsigned long>(err),
                     core::wideToUtf8(cmdLine).c_str());
        if (logFile != INVALID_HANDLE_VALUE) ::CloseHandle(logFile);
        if (nulHandle != INVALID_HANDLE_VALUE) ::CloseHandle(nulHandle);
        return -1;
    }
    const DWORD waitMs = DWORD(timeoutSeconds) * 1000u;
    const DWORD wr = ::WaitForSingleObject(pi.hProcess, waitMs);
    int code = -1;
    if (wr == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1);
        code = -2;
    } else {
        DWORD exitCode = 1;
        if (::GetExitCodeProcess(pi.hProcess, &exitCode)) code = int(exitCode);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (logFile != INVALID_HANDLE_VALUE) ::CloseHandle(logFile);
    if (nulHandle != INVALID_HANDLE_VALUE) ::CloseHandle(nulHandle);
    return code;
}

} // namespace

bool openInVisualStudio(const std::string& slnPath, std::string* err) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::path(slnPath), ec)) {
        if (err) *err = "solution file does not exist: " + slnPath;
        return false;
    }
    const VsInfo& vs = vsInfo();

    // 路线一：直接起 `devenv.exe <sln>`。
    //   比 ShellExecute 可靠 —— 不依赖 `.sln` 的文件关联有没有被别的
    //   程序抢走（装过 Blend / 某些 VS 插件后关联会被改掉）。
    if (!vs.devenv.empty() && fs::is_regular_file(fs::path(vs.devenv), ec)) {
        const std::wstring cmd = L"\"" + fs::path(vs.devenv).wstring() + L"\" \"" +
                                 core::utf8ToWide(slnPath) + L"\"";
        std::wstring mutableCmd = cmd;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // ⚠⚠ 环境块**必须去重后再传**（2026-11-04 实测：传 nullptr 会让 VS
        //   起来又立刻死掉）。
        //
        //   这里原先写的是"继承父进程环境（env 传 nullptr）"，理由看着很合理：
        //   UI 进程要能看到用户完整的环境（代理、SDK 路径）。但 VS 2022 的
        //   ServiceHub 和 MSBuild 是**同一个毛病** —— 拿大小写敏感的字典装环境
        //   变量，遇到 `HTTPS_PROXY` + `https_proxy` 两条就抛
        //   `ArgumentException: 已添加项。字典中的关键字:"HTTPS_PROXY" 所添加
        //   的关键字:"https_proxy"`。
        //
        //   症状极难猜：日志里明明白白写着 `GAMECODE opened devenv`（CreateProcess
        //   确实成功了），桌面上什么都没有 —— devenv 建完主窗口就销毁自己，
        //   **退出码还是 0**。证据在
        //   `%APPDATA%\Microsoft\VisualStudio\17.0_*\ActivityLog.xml`（`/log` 启动）
        //   里 GetIdeUserContextAsync 那条 ArgumentException。
        //
        //   去重不改变语义（Windows 的环境变量名大小写不敏感，见
        //   `buildDedupEnvBlock` 上方那段注释），用户环境依然完整可见。
        //   `CREATE_UNICODE_ENVIRONMENT` 不能省：传的是 UTF-16 块，不带这个
        //   flag 系统会按 ANSI 解析 → ERROR_INVALID_PARAMETER (87)。
        std::wstring envBlock = buildDedupEnvBlock();
        LPVOID envPtr = envBlock.empty() ? nullptr : &envBlock[0];
        const DWORD flags = envBlock.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT;
        if (::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                             flags, envPtr, nullptr, &si, &pi)) {
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);   // 不等它 —— VS 是长驻 GUI，等就卡死了
            VK_LOG_INFO("GAMECODE opened devenv: %s", slnPath.c_str());
            return true;
        }
        VK_LOG_WARN("GAMECODE devenv start failed error=%lu, falling back",
                    static_cast<unsigned long>(::GetLastError()));
    }

    // 路线二：走系统文件关联（只装了 Build Tools / 探测不到 VS 时的兜底）。
    // `ShellExecuteW` 返回值 <= 32 是错误码（SE_ERR_*），这是 Win32 的老约定。
    const std::wstring wide = core::utf8ToWide(slnPath);
    const HINSTANCE h = ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr,
                                        nullptr, SW_SHOWNORMAL);
    const INT_PTR code = reinterpret_cast<INT_PTR>(h);
    if (code <= 32) {
        if (err) {
            char b[320];
            std::snprintf(b, sizeof(b),
                          "cannot open the solution (ShellExecute code %lld) — "
                          "is Visual Studio installed? You can open %s manually",
                          static_cast<long long>(code), slnPath.c_str());
            *err = b;
        }
        VK_LOG_WARN("GAMECODE open sln failed code=%lld path=%s",
                    static_cast<long long>(code), slnPath.c_str());
        return false;
    }
    VK_LOG_INFO("GAMECODE opened in IDE: %s", slnPath.c_str());
    return true;
}

#else

bool openInVisualStudio(const std::string&, std::string* err) {
    if (err) *err = "Visual Studio is only supported on Windows";
    return false;
}

#endif

// ================================================================
// 自检
// ================================================================

namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

void check(const char* name, bool ok, const std::string& detail = std::string()) {
    if (ok) {
        ++g_pass;
        std::printf("GC-T pass %s %s\n", name, detail.c_str());
    } else {
        ++g_fail;
        std::printf("GC-T fail %s %s\n", name, detail.c_str());
    }
}

void skip(const char* name, const std::string& why) {
    ++g_skip;
    std::printf("GC-T skip %s %s\n", name, why.c_str());
}

std::string slurp(const fs::path& p) {
    bool ok = false;
    const std::string s = readAll(p, &ok);
    return ok ? s : std::string();
}

} // namespace

int runGameCodeSelfTest() {
    g_pass = 0;
    g_fail = 0;
    g_skip = 0;

    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "venn_gamecode_selftest";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    // ---- ① 模块名必须 ASCII（工程名叫中文时 CMake 才不会炸）
    //   "我的游戏" = 4 个码位 → 4 个下划线（按字节折叠会变成 12 个）
    check("A1_module_ascii", moduleIdFromName("我的游戏") == "____Game",
          "got=" + moduleIdFromName("我的游戏"));
    check("A2_module_digit_prefix", moduleIdFromName("3DGame") == "G3DGameGame",
          "got=" + moduleIdFromName("3DGame"));
    check("A3_module_plain", moduleIdFromName("MyGame") == "MyGameGame",
          "got=" + moduleIdFromName("MyGame"));
    check("A4_module_empty", moduleIdFromName("") == "GameGame",
          "got=" + moduleIdFromName(""));

    // ---- ② 四种类型都要生成得出来，且各自带自己的示例函数
    const struct {
        GameType t;
        const char* key;
        const char* fn;
    } kinds[] = {
        {GameType::ThirdPerson, "third_person", "dashDistance"},
        {GameType::FirstPerson, "first_person", "headBob"},
        {GameType::Isometric, "isometric", "snapToCell"},
        {GameType::CardBattler, "card_battler", "shuffleKey"},
    };

    for (const auto& k : kinds) {
        GameCodeSpec spec;
        spec.gameType = k.t;
        spec.projectName = std::string("Demo_") + k.key;
        spec.codeDir = gsl(base / k.key / "code");
        const GameCodeResult r = writeGameCode(spec);
        const std::string tag = std::string("B_") + k.key;

        check((tag + "_ok").c_str(), r.ok, r.error);
        if (!r.ok) continue;

        check((tag + "_files").c_str(), r.files.size() == 9,
              "n=" + std::to_string(r.files.size()));
        check((tag + "_dll_path").c_str(),
              r.dllRelPath == "code/bin/" + r.moduleId + ".dll", r.dllRelPath);

        const fs::path d(r.codeDir);
        const std::string cm = slurp(d / "CMakeLists.txt");
        const std::string gp = slurp(d / "src" / "Gameplay.cpp");
        const std::string gm = slurp(d / "src" / "GameModule.cpp");
        const std::string abi = slurp(d / "include" / "venn" / "VennAbi.h");

        // ⚠ 这条是"CMake 多配置落到 bin/Debug"那脚的判据：
        //   五个 RUNTIME_OUTPUT_DIRECTORY* 少一个，Debug 就编到别处，
        //   引擎找不到 DLL 且**不报错**。
        int outDirs = 0;
        for (const char* cfg : {"RUNTIME_OUTPUT_DIRECTORY ", "RUNTIME_OUTPUT_DIRECTORY_DEBUG",
                                "RUNTIME_OUTPUT_DIRECTORY_RELEASE",
                                "RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO",
                                "RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL"}) {
            if (cm.find(cfg) != std::string::npos) ++outDirs;
        }
        check((tag + "_cmake_outdirs").c_str(), outDirs == 5,
              "found=" + std::to_string(outDirs));
        // ⚠ 查的是**命令形式**，不是关键字：模板的注释里就写着
        //   "不 find_package 引擎" —— 按关键字搜会把自己的说明文字当成
        //   违规命令，判据永远红（第一版就是这么错的）。
        const bool noEngineLink =
            cm.find("\nfind_package(") == std::string::npos &&
            cm.find("\nadd_subdirectory(") == std::string::npos &&
            cm.find("target_link_libraries") == std::string::npos;
        check((tag + "_cmake_standalone").c_str(), noEngineLink,
              "must not link the engine (no find_package/add_subdirectory/"
              "target_link_libraries)");
        check((tag + "_abi_copied").c_str(),
              abi.find("VENN_ABI_VERSION") != std::string::npos &&
                  abi.find("vennModuleEntry") != std::string::npos,
              "size=" + std::to_string(abi.size()));
        check((tag + "_entry_exported").c_str(),
              gm.find("VENN_MODULE_EXPORT") != std::string::npos &&
                  gm.find("vennModuleEntry") != std::string::npos);
        check((tag + "_own_function").c_str(),
              gp.find(k.fn) != std::string::npos,
              std::string("expects ") + k.fn);
        check((tag + "_placeholders_gone").c_str(),
              cm.find("@MODULE@") == std::string::npos &&
                  gm.find("@PROJECT@") == std::string::npos &&
                  gp.find("@NS@") == std::string::npos);
    }

    // ---- ③ 幂等：再写一次不报错（"补生成"场景）
    {
        GameCodeSpec spec;
        spec.gameType = GameType::ThirdPerson;
        spec.projectName = "Demo_third_person";
        spec.codeDir = gsl(base / "third_person" / "code");
        const GameCodeResult again = writeGameCode(spec);
        check("C1_idempotent", again.ok, again.error);
    }

    // ---- ④ VS 工程：`.sln` 与 `.vcxproj` 必须自洽、必须不带绝对路径
    const std::string codeDir = gsl(base / "third_person" / "code");
    const std::string mod = "Demo_third_personGame";
    {
        const fs::path d(codeDir);
        const std::string sln = slurp(d / (mod + ".sln"));
        const std::string vcx = slurp(d / (mod + ".vcxproj"));
        const std::string flt = slurp(d / (mod + ".vcxproj.filters"));
        const std::string cm = slurp(d / "CMakeLists.txt");

        check("D1_vs_files", !sln.empty() && !vcx.empty() && !flt.empty(),
              "sln=" + std::to_string(sln.size()) + " vcx=" +
                  std::to_string(vcx.size()));

        // GUID 两端必须**一模一样**：对不上 VS 会报"项目文件已被重命名或
        // 不在解决方案中"，然后拒绝加载工程。
        const std::string guid = projectGuid(mod);
        check("D2_guid_match",
              sln.find(guid) != std::string::npos && vcx.find(guid) != std::string::npos,
              guid);

        // 产物目录 + 目标名：引擎按 `code/bin/<Module>.dll` 找，
        // 少一个字就是"编译成功、游戏里没反应"。
        check("D3_outdir_and_name",
              vcx.find("<OutDir>$(ProjectDir)bin\\</OutDir>") != std::string::npos &&
                  vcx.find("<TargetName>" + mod + "</TargetName>") != std::string::npos,
              "OutDir=$(ProjectDir)bin\\  TargetName=" + mod);

        check("D4_vcx_sources",
              vcx.find("src\\GameModule.cpp") != std::string::npos &&
                  vcx.find("src\\Gameplay.cpp") != std::string::npos &&
                  vcx.find("$(ProjectDir)include") != std::string::npos,
              "2 sources + include dir");

        // ⚠ 绝对不能出现绝对路径：cmake 生成的 .vcxproj 里会写死
        //   `<dir>/CMakeCache.txt` 那一串，整个 code/ 目录一拷走就废。
        //   我们直接写文件正是为了避开这个。
        const std::string abs = gsl(base);
        check("D5_no_absolute_path",
              sln.find(abs) == std::string::npos && vcx.find(abs) == std::string::npos &&
                  flt.find(abs) == std::string::npos,
              "searched for " + abs);

        // ⚠ 防漂移：源码列表在 `.vcxproj` 与 `CMakeLists.txt` 里各写一份。
        //   加文件时只改一处 → 两条构建路径产出的 DLL 不一样。
        int vcxSrc = 0;
        for (size_t p = vcx.find("<ClCompile Include="); p != std::string::npos;
             p = vcx.find("<ClCompile Include=", p + 1))
            ++vcxSrc;
        int cmSrc = 0;
        for (size_t p = cm.find(".cpp"); p != std::string::npos;
             p = cm.find(".cpp", p + 1))
            ++cmSrc;
        check("D6_vs_sources_match", vcxSrc == cmSrc && vcxSrc == 2,
              "vcxproj=" + std::to_string(vcxSrc) + " cmake=" + std::to_string(cmSrc));
    }

    // ---- ⑤ 真编一次（这一步才是"能编译"的**唯一**证据）。
    //       没装 MSBuild 时 SKIP —— 不该因为"测试机没装编译器"而判失败。
#if defined(_WIN32)
    {
        const VsInfo& vs = vsInfo();
        const fs::path dll = fs::path(codeDir) / "bin" / (mod + ".dll");
        if (vs.msbuild.empty() || !fs::is_regular_file(fs::path(vs.msbuild), ec)) {
            skip("E1_msbuild_build", "MSBuild not found (no Visual Studio / Build Tools)");
        } else {
            const fs::path log = fs::path(codeDir) / "build" / "msbuild.log";
            fs::create_directories(log.parent_path(), ec);
            const std::wstring cmd =
                L"\"" + fs::path(vs.msbuild).wstring() + L"\" \"" +
                (fs::path(codeDir) / (mod + ".sln")).wstring() +
                L"\" /nologo /v:m /p:Configuration=Debug /p:Platform=x64";
            const int rc = runAndWait(cmd, 600, gsl(log));
            if (rc == 0 && fs::is_regular_file(dll, ec)) {
                check("E1_msbuild_build", true,
                      gsl(dll) + " (" + std::to_string(fs::file_size(dll, ec)) + " bytes)");
            } else {
                check("E1_msbuild_build", false,
                      "rc=" + std::to_string(rc) + " see " + gsl(log));
            }
        }
    }
#else
    skip("E1_msbuild_build", "Windows only");
#endif

    // 排查用：`MYVK_GAMECODE_KEEP=1` 时不删临时目录 —— cmake 失败时
    // 唯一能看的就是那份 `CMakeLists.txt` 与生成的 build 目录。
    if (!std::getenv("MYVK_GAMECODE_KEEP")) {
        fs::remove_all(base, ec);
    } else {
        std::printf("GC-TEMP kept at %s\n", gsl(base).c_str());
    }
    std::printf("GC-TOTAL pass=%d fail=%d skip=%d\n", g_pass, g_fail, g_skip);
    return g_fail;
}

} // namespace project
