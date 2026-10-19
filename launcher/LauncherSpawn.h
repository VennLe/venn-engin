#pragma once
// ============================================================
// launcher/LauncherSpawn —— 从启动器拉起 Editor.exe / Game.exe
//
// ------------------------------------------------------------
// 为什么必须自己建环境变量块（而不是 `_wputenv` + `CreateProcessW(NULL)`)
// ------------------------------------------------------------
// `CreateProcessW` 传 `lpEnvironment = NULL` 时，子进程继承的是**调用时刻**
// 的进程环境。而启动器要传的 `MYVK_ASSET_DIR` / `MYVK_EDITOR_SCENE` /
// `MYVK_EDITOR_PROJECT` 是**这一台机器上同时可能有两个工程**的区分依据。
//
// 用 `_wputenv` 改自己的环境再拉起，等于把启动器进程本身污染成"某个工程
// 的启动器"：先开 A 再开 B，两个子进程都继承到最后一次写入的值 —— 表现为
// "第二个工程的编辑器打开的是第一个工程的场景"，而且**没有任何报错**。
// （这正是 MATERIAL 同名静默合并那一类 bug 的同构版本。）
//
// 所以：读一份自己的环境 → 只在这份**拷贝**上覆盖那几个键 →
// `CREATE_UNICODE_ENVIRONMENT` 把整块传下去。父进程环境一字不动。
//
// ------------------------------------------------------------
// 为什么 `lpCurrentDirectory` 必须是工程根（D5）
// ------------------------------------------------------------
// 引擎里所有"用户状态"都是**相对 CWD** 的：`config/editor_layout.json`、
// `config/editor_prefs.json`、`imgui.ini`、`editor_window.ini`。
// 仓库根现在就躺着 `imgui.ini` 和 `editor_window.ini` —— 那是"一直在
// 仓库根启动"的化石证据。不换 CWD 的话，"每个工程一份布局"永远不成立。
//
// ⚠ 用 `CreateProcessW` 的 `lpCurrentDirectory`，**不用**
//   `SetCurrentDirectory`：后者改的是启动器自己的工作目录，回收/异常路径上
//   很容易忘了改回来，而且它会影响启动器后续所有相对路径 IO（`recent.json`
//   的兜底路径就有一条是 `"."`）。
// ============================================================

#include <string>
#include <vector>

namespace launcher {

// 一次拉起的全部输入。字段全部由调用方算好（绝对路径、正斜杠）。
struct SpawnTarget {
    std::string exe;          // 要拉起的 exe（绝对路径）
    std::string projectRoot;  // 子进程的 CWD（= 工程根）
    std::string assetDir;     // MYVK_ASSET_DIR
    std::string scene;        // MYVK_EDITOR_SCENE（资产相对路径，可为空）
    std::string projectFile;  // MYVK_EDITOR_PROJECT（`venn.project.json` 绝对路径）
};

struct SpawnResult {
    bool ok = false;
    std::string error;        // 人话，UI 直接显示
    unsigned long pid = 0;
};

// 拉起。**不等待**子进程退出（启动器随后自己就退了）。
// 失败原因已"可读化"：exe 不存在 / 权限不足 / 路径被占用 / 参数非法。
SpawnResult spawn(const SpawnTarget& t);

// 当前 exe 所在目录（正斜杠、无尾斜杠）。拿不到返回空串。
std::string exeDir();

// `<exeDir()>/<baseName>`。文件不存在时**照样返回路径** ——
// 让调用方用 `fileExists` 判断并给出"哪个文件找不到"的完整路径。
std::string siblingExe(const char* baseName);

bool fileExists(const std::string& path);
bool dirExists(const std::string& path);

// 只算"要不要拉起"和"传什么环境"，**不真的拉起** —— 给
// `--selftest project` 的 L 组做纯数值校验（环境块拼装是最容易错、
// 又最难在 GUI 里看出来的一环）。
struct SpawnPlan {
    std::string commandLine;
    std::string workingDir;
    // 展开成人话，逐行给自检断言 / UI 的"将执行"清单用。
    std::vector<std::string> envOverrides;
};
SpawnPlan plan(const SpawnTarget& t);

} // namespace launcher
