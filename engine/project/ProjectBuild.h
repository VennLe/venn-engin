#pragma once
// ============================================================
// project/ProjectBuild —— 出包（Package）：把工程变成一个能直接发出去的目录
//
// ------------------------------------------------------------
// 它与「新建工程」是两件事，别混
// ------------------------------------------------------------
//   · `ProjectCreate`（M2，已完成）= 从模板复制出一个**可编辑**的工程
//     （`venn.project.json` + `assets/` + `config/`）。
//   · 本文件（E28-1）= 把一个做完的工程压成**可分发的运行目录**：
//         dist/<Name>/
//           <Name>.exe        ← 引擎运行时（改名）
//           assets/           ← 工程资产（闭包后的那些，G29-a）
//           shaders/          ← 引擎侧 .spv（263 个）
//           config/           ← 画面/输入等配置
//           build.json        ← 清单：每个文件的 size + 哈希 + 打包信息
//           README.txt
//     加 `--pak`（G29-b/c）之后 `assets/` 与 `shaders/` 会被收进一个
//     `game.pak`（条目路径相对 dist 根），distribution 只剩 5 个文件。
//     `config/` **永远**是散文件：它要能被玩家/美术手改。
//
// 与 UE 的对应关系：`ProjectCreate` ≈ New Project Wizard，本文件 ≈
// `Project → Package Project`。UE 出包后会删掉 `.uproject`、只留 cooked
// 内容；我们这一版**只留运行时需要的东西**，也不带 `venn.project.json`
// （运行时根本不读它，见 `runtime/RuntimeConfig.cpp`：入口是
// `assets/game.json`）。
//
// ------------------------------------------------------------
// 为什么第一版是"整棵 assets 拷过去"，而不是"引用闭包"
// ------------------------------------------------------------
// 闭包式的 cooked 内容（只打被引用的资产、打成 .pak）是 G29 的目标，
// 也是正确的终点。但它有一个**必须先解决**的前提：引用扫描器必须能覆盖
// 场景 / 材质 / 预制体 / 时间轴 / 对话 / 脚本里的每一种引用，并且算出
// "未解析清单"——`ship_roadmap` G29 的验收第 2 条就写着"故意漏一个材质
// 引用 → 打包报错"。
//
// 在扫描器还不存在的时候先写"猜引用"，结果一定是**静默漏文件**（打包成功、
// 玩家那里黑屏）。所以第一版反过来选：**宁可多带，不可少带**，同时用
// 三条硬校验守住"少带"这件事会暴露的地方 ——
//   ① 工程声明的起始场景必须存在（`venn.project.json` 的 `startScene`）；
//   ② `assets/game.json` 必须存在，且它里面写的 `scene` 必须存在；
//   ③ 目标目录已存在且非空 → **拒绝**（不覆盖别人的 dist）。
// 三条任何一条不过 = 打包失败并列出缺哪个文件，绝不产出"看起来成功"的包。
//
// ------------------------------------------------------------
// 三条纪律（都是从 ProjectCreate 那边继承的，理由一模一样）
// ------------------------------------------------------------
// ① **先写 `<out>.tmp` 再 rename**：半成品目录比没有更难查（用户会双击它）。
// ② **失败删掉自己建的 `.tmp`**，绝不碰用户原有的目录。
// ③ **热路径不猜**：所有失败都返回人话 `error`，不抛异常（UI 要显示它）。
// ============================================================

#include <functional>
#include <string>
#include <vector>

namespace project {

// pak 文件名（`--pak` 时的产物）。放在头文件里是因为**三处都要用同一份**：
// 打包器写它、测试断言它、README 描述它。写在三个地方就一定会有一处漂。
constexpr const char* kPakFileName = "game.pak";

// 出包的阶段（进度显示 + "失败停在哪一步"）。顺序即执行顺序。
enum class BuildStep : int {
    Validate = 0,   // 校验入口文件 / 引擎 exe / 目标目录
    Collect,        // 枚举要带的文件（assets/ + config/）
    Copy,           // 复制到 <out>.tmp
    Manifest,       // 写 build.json（size + 哈希）
    Commit,         // rename .tmp → out
    Count
};
const char* buildStepLabel(BuildStep s);

// 一个被打包的文件在清单里的样子。
struct BuildFile {
    std::string path;    // 相对 dist 根，正斜杠
    long long size = 0;
    // FNV-1a **64** 的十六进制。⚠ 不是 SHA-256：仓库里没有加密库，
    // 而这里要回答的是"两个包是不是同一份内容 / 拷坏了没有"，
    // FNV-1a 足够，且实现只有 8 行（不会引入一个会写错的依赖）。
    // 清单里 `hashKind` 明写着算法名，将来换 SHA-256 时能一眼分辨旧包。
    std::string hash;
};

struct BuildSpec {
    // 工程根（绝对路径、正斜杠）。必须有 `venn.project.json` + `assets/`。
    std::string projectRoot;
    // 输出目录（绝对）。**必须不存在或是空目录** —— 覆盖别人的 dist 是
    // 最不该发生的事（里面可能有上一次的存档 / 玩家日志）。
    std::string outDir;
    // 引擎运行时 exe（绝对）。通常就是"正在运行的自己"
    // （`GetModuleFileNameA`），这样"编辑器出包"和"命令行出包"用的是
    // 同一个二进制，不会出现"编辑器打出来的包跑的是另一个版本"。
    std::string engineExe;
    // true（默认）= **引用闭包**：从 `game.json` + 起始场景出发，只打被引用到的
    // 资产（G29-a）。false = 整棵 `assets/` 拷（E28-1 的老行为，`--no-closure`）。
    //
    // 为什么要留这个开关：闭包是"算出来的"，算错就是**少文件**；整棵拷是
    // "不管对错都全带"。出包失败时用 `--no-closure` 一秒钟能判定"是不是闭包
    // 漏了"—— 这条排查路径比读日志快得多。
    bool useClosure = true;
    // true = **把 `assets/` 打成单个 `assets.pak`**（G29-b）；false（默认）= 散文件目录。
    //
    // 为什么默认关：pak 要运行时能读（`assets/AssetSource`），而那套在
    // "资产目录还在原地"时**根本不会被走到**。所以 pak 必须单独验一遍
    // "拷到别处、删掉工程、双击能跑"，验过了才允许它成为默认。
    // 命令行 `--pak` 开它；编辑器菜单暂时还是散文件（用户能一眼看到内容）。
    bool usePak = false;
    // true = 允许把 Debug 构建的 exe 打进包（默认允许，但会写 WARNING）。
    // 之所以不直接拒绝：Debug 包在本机自测时是常用的（能看到断言），
    // 拒绝会逼人去做无意义的 Release 构建。
    bool allowDebugEngine = true;
    // G30：把 exe 旁边的 `<exe 同名>.pdb` 归档到 `dist/<Name>/debug/`。
    //
    // 为什么默认**不是**恒开：Debug 版的 pdb 有几十上百 MB，而且对拿到包
    // 的人毫无用处（他们看的是"包跑不跑得起来"）。所以命令行默认按
    // **探测到的风味**决定：shipping → 归档（崩溃时要靠它还原栈），
    // dev → 不归档。`--pdb` / `--no-pdb` 可以显式覆盖。
    bool copyPdb = false;
};

struct BuildResult {
    bool ok = false;
    std::string error;      // 人话；ok==true 时为空
    std::string outDir;     // 成功时的最终目录
    int files = 0;
    long long bytes = 0;
    // 不致命但必须让人看见的问题（Debug 运行时、缺 config/render.json…）。
    // ⚠ 单独一个列表而不是拼进 error：`error` 只表示"没打成"。
    std::vector<std::string> warnings;
    // 缺的入口文件（Validate 阶段发现）。失败时至少有一条。
    std::vector<std::string> missing;
    // 闭包阶段发现的**未解析引用**（"谁引用的 → 引用了什么"）。非空 = 打包失败。
    // ⚠ 与 `missing` 分开：`missing` 是"工程声明了但文件不在"（入口级），
    //   `unresolved` 是"某个文件里写了一个指向不存在资产的引用"（内容级）。
    //   两者的修法完全不同（一个是补入口、一个是改引用或补文件）。
    std::vector<std::string> unresolved;
    // 闭包剔掉了多少个文件（整棵 vs 闭包的差）。0 = 闭包没省东西（或用了 --no-closure）。
    int pruned = 0;
    int scanned = 0;
    // pak 模式下的统计（未用 pak 时全 0）
    bool pak = false;
    int pakEntries = 0;
    unsigned long long pakRaw = 0;
    unsigned long long pakPacked = 0;
    // G30：打进去的那个 exe 是什么味道（`dev` / `shipping` / `unknown`）。
    //
    // 怎么来的：**跑一次** `<exe> --version` 读它的构建戳。不是"看文件名"、
    // 也不是"猜是不是 Release"—— 风味是编译进二进制的，只有问它自己
    // 才准。`unknown` = 探测失败（exe 起不来 / 不是我们的 exe），
    // 此时不阻断打包（用户可能就是打了个奇怪的 exe 进来），但会写进清单。
    std::string engineFlavor = "unknown";
    std::string engineGit;
    // 归档的 pdb 在包里的相对路径（空 = 没归档）。
    std::string pdbPath;
    BuildStep failedAt = BuildStep::Validate;
};

// 当前进程的 exe 绝对路径（正斜杠）。取不到返回空串。
// 放在这里而不是各家长一份：出包要的"游戏 exe"就是**正在执行的自己**，
// 运行时（`--package`）与编辑器（`File → Package`）必须拿到同一个答案。
std::string engineExePath();

// G30：探测任意 exe 的构建风味 —— 跑一次 `<exe> --version`，解析它的构建戳。
// 返回 `"dev"` / `"shipping"` / `"unknown"`（起不来或格式不认识）。
// `gitOut` 收到 `git=<...>` 那一段（没有则清空）。
//
// ⚠ 会**启动一个子进程**（毫秒级，`--version` 在开日志/建窗口之前返回）。
//   不要放进逐帧路径。
std::string probeExeFlavor(const std::string& exePath, std::string& gitOut);

// 每进入一个阶段调一次（UI 画进度用）。可为空。
using BuildProgressFn = std::function<void(BuildStep, const char*)>;

BuildResult buildProject(const BuildSpec& spec, const BuildProgressFn& progress);

// ---------------------------------------------------------------- 自检
// `--selftest package`（`Game.exe`）与 `VennEditor --selftest package` 都跑它。
// 纯文件 IO + 不建窗口、不碰 GPU —— 与 project 其它自检同一条路。
// 返回失败条数（0 = 全绿）。
int runBuildSelfTest();

} // namespace project
