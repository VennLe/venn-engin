# 游戏 C++ 代码工程（M5-c）

> 状态（2026-11-03）：**创建 → 生成 VS 工程 → 编译 → 加载 → 生命周期** 全链路
> 实测闭环。唯一未闭环的是"注册的函数在 Lua 里可见" —— 它等 **阶段 D**
> （把 Lua 后端接进 `ScriptRuntime`）。见 §7 已知边界。

## 1 · 它是什么

在启动器里新建工程时勾一项，`<工程>/code/` 会多出一个**独立的 Visual Studio
工程**，创建完成后自动用本机 VS 打开：

```
<工程>/
  venn.project.json          ← 多一个 "code.module": "code/bin/MyGameGame.dll"
  assets/                    ← 场景 / 资产 / Lua（照旧）
  code/                      ← ★ 这个（就是一个 VS 工程）
    MyGameGame.sln           ← 双击这个
    MyGameGame.vcxproj       ← x64 Debug / Release
    MyGameGame.vcxproj.filters
    CMakeLists.txt           ← 等价的 cmake 描述（CI / 非 VS 用）
    README.md
    .gitignore
    include/venn/VennAbi.h   ← 引擎 ABI 的**快照**（只读）
    src/GameModule.cpp       ← 入口 + 生命周期 + 注册
    src/Gameplay.cpp         ← 按游戏类型给的示例
    bin/MyGameGame.dll       ← 编译产物（引擎从这里加载）
    build/                   ← MSBuild 中间文件（.gitignore 已排除）
```

"独立"是字面意思：**不 find_package 引擎、不链接引擎的 .lib、不需要
Vulkan / glm / 第三方目录、也不需要装 cmake**。它只 include 那份 ABI 快照。

## 2 · 为什么 DLL 不能链接引擎

`VennEngine` 是 STATIC 库。DLL 一旦链接它，链接器会把用到的目标文件
**复制一份**进 DLL —— 于是 `World` / `Logger` / 各种全局单例在进程里有
**两份**：DLL 里那份是空的，引擎里那份是真的。

症状是"日志打到另一个地方、找实体永远找不到"，而且**不报错**。

所以规则只有一条：**DLL 不链接任何引擎库**，它需要的每一样东西都从宿主
传进来的函数表里拿（`VennAbi.h`）。

## 3 · ABI v1 能做什么

| 类别 | 函数 |
|---|---|
| 日志 | `log(level, msg)` |
| 场景读 | `entityByName` / `entityAlive` / `entityPos` / `entityCount` |
| 场景写 | `entitySetPos` / `entitySetVisible` |
| 时间 | `deltaTime` / `elapsedTime` |
| 注册 | `registerFunction(ns, name, fn)` |

生命周期：`onRegister(&abi)` → `onBeginPlay()` → `onTick(dt)`… →
`onEndPlay()`。

约定：

- 实体句柄是 `uint64_t`，**`0` 是唯一空句柄**（与 Lua 侧同一编码，
  实现在 `engine/ecs/EntityHandle.h`）。
- 函数参数与返回值**只有 `double`**。返回值只能是数值 —— 返回值一旦能是
  结构或字符串，Lua 侧就得懂 C++ 类型，这套东西会立刻长成一个反射系统，
  而"不做反射"是本引擎写死的取舍。
- `registerFunction` **只在 `onRegister` 里有效**。窗口外调用 → 打 WARN
  并拒绝（不静默丢，但也不生效）。
- ABI 版本不一致 → **拒载**并打 `GAME-MODULE abi mismatch`。绝不"尽力而为
  地调"：结构体布局变了还硬调，崩在 `onTick` 里根本没法查。

## 4 · 四种类型生成什么

底座一样（入口 + 注册 + 生命周期），`Gameplay.cpp` 按类型给一个**真的
需要在 C++ 做**的示例：

| 游戏类型 | 函数 | 为什么放 C++ |
|---|---|---|
| 第三人称 | `dashDistance(speed, t, duration)` | 逐帧积分的冲刺位移曲线 |
| 第一人称 | `headBob(t, amp, speed)` | 相机热路径上的三角函数叠加 |
| 俯视等距 | `snapToCell(v, cell)` | 战棋格子必须确定性对齐 |
| 卡牌对战 | `shuffleKey(seed, index)` | 牌序必须可复现（回放 / 联机） |

这些不是占位函数——它们编得过、算得对，改改就能用。

## 5 · 为什么直接写 `.sln` 而不调 cmake

第一版是 `cmake -S code -B code/build` 生成工程。换成直接写文件，因为：

1. **用户机器上可能根本没装 cmake**。cmake 只是"生成器"，真正编译的仍是
   MSBuild —— 少一层外部依赖就少一整类失败。
2. **cmake 会把绝对路径写进 `.vcxproj`**（`<dir>/CMakeCache.txt` 那一串）。
   而创建流程中途要把整个目录 rename 一次，rename 之后打开 sln 就报
   "找不到项目文件"，工程本身却是完好的。直接写的文件里**一个绝对路径
   都没有**（`$(ProjectDir)` 是 MSBuild 编译时才展开的），整个 `code/`
   目录可以拷给别人。
3. 三个文件是纯文本，写它们不需要起进程、不需要联网、瞬间完成。

代价是 `.sln` / `.vcxproj` / `CMakeLists.txt` 三处各写一份源文件列表 ——
自检 `GC-T D6_vs_sources_match` 盯着，加文件只改一处会红。

### 探测而不是写死

| 项 | 怎么定 | 写死会怎样 |
|---|---|---|
| `PlatformToolset` | 扫 `VC\Tools\MSVC\14.xx` → `v143`/`v142`/`v141` | 只装 VS2019 的机器报 `MSB8020` |
| `WindowsTargetPlatformVersion` | **不写**，用最新已装 SDK | 没装那个 SDK 报"找不到 SDK" |
| `devenv.exe` / `MSBuild.exe` | `vswhere -latest -products *` | 装在 D 盘就找不到（本机就是 D 盘） |
| `OutDir` | **必须**显式 `$(ProjectDir)bin\` | DLL 落 `Debug\`，引擎找不到且不报错 |

`$(ProjectDir)` 自带结尾反斜杠，所以是 `$(ProjectDir)bin\` 而不是
`$(ProjectDir)\bin`。

## 6 · 怎么编、怎么被加载

```
双击 code/MyGameGame.sln        ← 或者
msbuild MyGameGame.sln /p:Configuration=Debug /p:Platform=x64
```

产物固定是 `code/bin/<Name>Game.dll`（Debug 也落这里 —— 这是 `OutDir`
那一行的全部意义）。

引擎这边：

- **编辑器**：打开工程时读 `venn.project.json` 的 `code.module` 去
  `LoadLibrary`；`Play` 时 `onBeginPlay`，每帧 `onTick`（**只喂运行态
  场景**，不是编辑场景），`Stop` 时 `onEndPlay`。
- **运行版**：启动后第一次 tick 时懒 `onBeginPlay`（那时场景才就位）。

加载失败一律只打 WARN、不拦工程：DLL 没编出来的工程照样能跑，Lua 那套
完全不受影响。

## 7 · 两个 CreateProcessW 的坑（值得单独记）

### ① 传环境块必须带 `CREATE_UNICODE_ENVIRONMENT`

**症状**：`CreateProcessW` 返回 `FALSE`，`GetLastError() = 87`
（`ERROR_INVALID_PARAMETER`），看起来像"命令行写错了"。

**根因**：`lpEnvironment` 指向 UTF-16 块，但 `dwCreationFlags` 里没带
`CREATE_UNICODE_ENVIRONMENT`，系统按 ANSI 去解析那串字节。

**实测**（用 ctypes 直接调 API 对比）：

| 场景 | 结果 |
|---|---|
| 48 字符的小块，不带 flag | **失败 87** |
| 48 字符的小块，带 flag | 成功 |
| 真实环境 253 项 / 27370 字符，不带 flag | 失败 87 |
| 真实环境，带 flag | **成功** |

**和块大小完全无关** —— 当时怀疑的"超过 32K 上限"是错的。这个误判让排查
绕了一大圈，最后靠"写个最小复现"才定住。

### ② MSBuild 用大小写敏感的字典装环境变量

**症状**：编译报 `MSB6001: "CL.exe" 的命令行开关无效`，报错文本跟环境变量
毫无关系。

**根因**：进程里同时存在 `HTTPS_PROXY` 和 `https_proxy` 两条（装了代理
工具 + 手写过一条）。MSBuild 的 `ToolTask.GetProcessStartInfo` 里
`Hashtable.Insert` 抛 `ArgumentException: 已添加项`。

**修法**：传给子进程的环境块里，同名（忽略大小写）只留第一条
（`buildDedupEnvBlock`）。Windows 语义上变量名大小写不敏感，去重不改变行为。

顺带一条纪律：**子进程的输出必须重定向到文件**。`CREATE_NO_WINDOW` 会把
stdout/stderr 直接丢进虚空，失败时只剩一句 "exit 1"，连哪一行错都看不到。

## 8 · 什么时候该写这里

| 放 Lua | 放这里 |
|---|---|
| 玩法编排、状态机、数值公式 | 每帧几百次以上的数学 |
| 需要热重载反复调的东西 | 确定性算法（洗牌 / 寻路） |
| 事件响应、UI 流程 | 第三方 C++ 库的封装 |

判据没变：**Lua 做决策，C++ 做执行**。写在这里的代价是"改一行要重编译 +
重启"，所以它**不是默认值**（创建向导里默认不勾）。

## 9 · 已知边界（诚实清单）

1. **`registerFunction` 注册的函数，目前在 Lua 里还用不了。**
   Lua 后端（`engine/script/LuaBackend`）还没接进 `ScriptRuntime`
   —— 那是阶段 D。接口已经就位（`LuaBackend::addExternalFunction`），
   D 阶段接上后自动生效。
   在那之前，`onTick` + ABI 的场景读写是**完全可用**的。
2. **一个工程一个模块**。不做多模块依赖图：多模块要处理加载序、符号冲突、
   跨模块调用，收益是零。
3. **不支持热重载**。改 C++ 必须重编译 + 重启（这是选它的代价）。
4. **出包（dist）不带 DLL**。`Game.exe --package` 目前不打包 `code/`，
   要带的话得单独加一档。
5. **只支持 Windows**。`.dll` + `LoadLibrary`；其它平台的加载器没写。
6. **只有 x64**。引擎本身是 x64，换成 Win32 会在加载时报"不是有效的
   Win32 应用程序"。

## 10 · 判据

| 命令 | 验什么 |
|---|---|
| `Game.exe --selftest game` | ABI 表完整性（**没有空函数指针**）、版本号、注册窗口外必须被拒、无场景时安全空操作 |
| `MYVK_GAME_DLL=<dll> Game.exe --selftest game` | **真加载一个 DLL**：入口符号、ABI 版本、模块名、注册函数数量、卸载干净（A9~A12；不设变量则 SKIP） |
| `Game.exe --selftest project`（GC 组 A~E） | 四种类型都生成得出来、`.sln`/`.vcxproj` 自洽（GUID 两端一致）、**不带绝对路径**、`.vcxproj` 与 `CMakeLists.txt` 源文件列表一致、中文工程名折成 ASCII 模块名、**真跑一次 MSBuild 编出 DLL**（E1，没装 MSBuild 则 SKIP） |
| `Game.exe --selftest project`（CREATE 组 K26b~K26g） | 带代码工程创建：`slnPath` / `codeDir` 必须指向 **rename 之后**的最终目录、`venn.project.json` 里登记了 DLL、不留 `.tmp` 残骸 |

`OutDir` 那条是重点：只设 `RUNTIME_OUTPUT_DIRECTORY` 时，VS 的 Debug 配置
照样会把 DLL 落到 `bin/Debug/`，而引擎按 `code/bin/<Name>Game.dll` 找 ——
对不上就是"编译成功了、游戏里没反应"，两边都不报错。
判据是 `GC-T D3_outdir_and_name`。

## 11 · 一次实测记录（2026-11-03）

```
GC-T pass E1_msbuild_build .../code/bin/Demo_third_personGame.dll (36352 bytes)
GAME-MODULE loaded name=Demo_third_person version=1.0.0 abi=1 functions=2
GAME-T pass A11_functions_registered n=2 first=demo_third_persongame.ping
GAME-MODULE unload name=Demo_third_person
```

"生成 → 编译 → 加载 → 卸载"四段都是真的跑出来的，不是推断。
