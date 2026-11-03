#pragma once
// ============================================================
// project/GameCodeGen —— 建工程时生成 `<工程>/code/`（路线图 M5-c）
//
// 产物是一个**独立的 C++ 工程**，Visual Studio 双击就能打开：
//
//     <工程>/code/
//       <Name>Game.sln            ← ★ VS 解决方案（直接打开这个）
//       <Name>Game.vcxproj        ← ★ VS 工程（x64 Debug/Release）
//       <Name>Game.vcxproj.filters
//       CMakeLists.txt            ← 等价构建描述（CI / 非 VS 用）
//       README.md
//       .gitignore
//       include/venn/VennAbi.h    ← 引擎 ABI 的**快照**（只读，别改）
//       src/GameModule.cpp        ← 入口 + 生命周期 + 注册
//       src/Gameplay.cpp          ← 按游戏类型给的玩法示例
//       bin/<Name>Game.dll        ← 编译产物（引擎从这里加载）
//
// ------------------------------------------------------------
// 六条不这么写就会静默失败的
// ------------------------------------------------------------
// ① **DLL 输出必须落在 `code/bin/`**。VS 的默认是 `$(Configuration)\`
//    （Debug 落 `Debug\`）；引擎按 `code/bin/<Name>Game.dll` 找 ——
//    对不上就是"编译成功了、游戏里没反应"，两边都不报错。
//    所以 `.vcxproj` 里显式写 `<OutDir>$(ProjectDir)bin\</OutDir>`。
//
// ② **target 名必须是 ASCII**。工程名可以叫"我的游戏"，但 VS 的工程名 /
//    CMake target 不能带中文或空格。所以模块名由 `moduleIdFromName()`
//    生成（非 ASCII 一律折成 `_`）。
//
// ③ **不调 cmake，直接写 `.sln` / `.vcxproj`**。cmake 只是"生成器"，
//    编译终究靠 MSBuild；少一层外部依赖就少一类失败（而且 cmake 在
//    代理环境变量大小写重复的机器上会莫名报 `MSB6001`）。
//    顺带的好处：产物**不含绝对路径**，整个 `code/` 目录可以拷走。
//
// ④ **`PlatformToolset` / `WindowsTargetPlatformVersion` 都靠探测，
//    不写死**。写死 v143 在只装了 VS2019 的机器上报 MSB8020；写死 SDK
//    版本在没装那个 SDK 的机器上报"找不到 SDK"。
//
// ⑤ **不复制引擎源码目录，只复制 `VennAbi.h` 一份**。游戏工程要能脱离
//    引擎目录单独打开/编译/进版本库。ABI 版本对不上时加载器会拒载并
//    说清楚 —— 那是**明确失败**，比"include 到引擎某个版本的头"好。
//
// ⑥ **生成的是"能编译通过"的骨架**，不是伪代码。判据在自检 E1：
//    真跑一次 MSBuild，DLL 出来才算过。
// ============================================================

#include "project/ProjectFile.h"

#include <string>
#include <vector>

namespace project {

struct GameCodeSpec {
    GameType gameType = GameType::ThirdPerson;
    std::string projectName;   // 展示名（可以是中文）
    std::string codeDir;       // `<工程根>/code`，**绝对**路径（正斜杠）
    // 空 = 由 `moduleIdFromName(projectName) + "Game"` 算
    std::string moduleId;
};

struct GameCodeResult {
    bool ok = false;
    std::string error;         // 人话
    std::string codeDir;
    std::string moduleId;      // "MyGameGame" 之类（VS 工程名 / CMake target 名）
    std::string dllName;       // "MyGameGame.dll"
    std::string dllRelPath;    // 相对工程根："code/bin/MyGameGame.dll"
    // ⚠ 这里填的是"按 `codeDir` 算出来"的路径。创建流程中途会把整个目录
    //   rename 一次，调用方（ProjectCreate）必须用最终路径覆盖它。
    std::string slnPath;       // `<codeDir>/<moduleId>.sln`
    std::vector<std::string> files;   // 已写出的文件（相对 code/）
};

// 工程名 → VS 工程名 / CMake target 用的 ASCII 标识（不保证唯一，
// 调用方要的只是"能编"）。
std::string moduleIdFromName(const std::string& projectName);

// 写 `code/` 的全部文件（含 `.sln` / `.vcxproj`）。不启动任何外部进程
// （只有探测 VS 工具集时会起一次 `vswhere`，全进程只跑一次）。
// 目录已存在时：只覆盖本函数负责的那几个文件（幂等，方便"补生成"）。
GameCodeResult writeGameCode(const GameCodeSpec& spec);

// 用本机 Visual Studio 打开 `.sln`。
// 优先直接起 `devenv.exe`（不依赖文件关联），退路是 ShellExecute。
// 失败时 `err` 是人话（没装 VS / 路径不存在 / 关联被抢）。
bool openInVisualStudio(const std::string& slnPath, std::string* err);

// ---------------------------------------------------------------- 自检
//
// `--selftest game`。会真的在临时目录生成一套并**跑一次 MSBuild**：
//   没装 MSBuild 时那条按 SKIP 计（不算失败），
//   但"文件写出来了 / 内容里该有的都在 / 不带绝对路径"必须过。
int runGameCodeSelfTest();

} // namespace project
