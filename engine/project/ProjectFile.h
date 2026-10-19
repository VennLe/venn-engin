#pragma once
// ============================================================
// project/ProjectFile —— 「工程」这个概念的**唯一数据定义**
//
// 启动器路线图 D3：工程 = 一个目录 + `venn.project.json`。
// 这个文件里的东西**只做三件事**：
//
//   ① 读写 `venn.project.json`（身份与元数据）
//   ② 路径规则（assetRoot / startScene / codeRoot）
//   ③ 逐条校验（format 版本 / 必有键 / 路径逃逸）
//
// ⚠⚠ **不存第二份配置**。分辨率 / 画质 / 窗口模式 / 语言**不在**这里
//   —— 它们的权威是 `assets/game.json` 与 `config/render.json`（既有格式）。
//   这个项目里最贵的 bug 全是"两份数据漂移"（材质同名静默合并、
//   `kPrefsVersion` 升版后 fixture 还写旧值、`SceneSerializer` 的 Model
//   分支只写两个键）。启动器一旦成为第二个存储，立刻多出一整类
//   "启动器显示 1080p、游戏跑 720p" 且**两边都不报错**的 bug。
//
// ⚠ 本头文件**不 include render/scene**：工程层是纯数据层，谁都能 include
//   它（启动器 / 编辑器 / 工具），不该把 Vulkan 或场景类型拖进来。
//   `ProjectFile.cpp` 只用 nlohmann/json + <filesystem>。
//
// ------------------------------------------------------------
// 路径规则（三处都靠这三条，别各写一遍）
// ------------------------------------------------------------
//   assetRoot()   = <root>/assets                    ← MYVK_ASSET_DIR 指这里
//   startScene()  = <root>/assets/<startScene>       ← startScene 是资产相对路径
//   codeRoot()    = <root>/assets/<code.root>        （相对）
//                 | <code.root>                      （绝对 = 外置代码根）
//
// `code.root` 相对时**相对 assets/**（不是相对工程根）：因为脚本路径在引擎
// 里是资产相对路径（`ScriptComponent::path` 走 `resolveAssetPath`），
// 把两者对齐才不会出现"启动器写的路径和编辑器找的路径差一层"。
// ============================================================

#include <string>
#include <vector>

namespace project {

// `venn.project.json` 的格式版本。
//
// ⚠ **只增不改**：加字段 = 版本号不动（旧读法忽略未知键，新读法给默认）；
//   改语义 / 删键 = 版本号 +1，并且要写迁移。读不懂的高版本一律**拒绝打开
//   并给一句人话**，不许"尽力而为地猜" —— 猜错的表现是"工程打开了一半"，
//   比打不开难查十倍。
constexpr int kProjectFormat = 1;

// 文件名（相对工程根）
constexpr const char* kProjectFileName = "venn.project.json";

// ---------------------------------------------------------------- 枚举

// 游戏类型。⚠ **展示用**：权威永远是场景里的 `player.mode`
// （见 docs/launcher_roadmap.md §4）。两者不一致只打 WARN，不改场景 ——
// 用户可能在编辑器里手动把第一人称改成了第三人称，那才是他的真实意图。
enum class GameType : int {
    FirstPerson = 0,
    ThirdPerson = 1,
};
const char* gameTypeId(GameType t);          // "first_person" / "third_person"
const char* gameTypeLabel(GameType t);       // "First person" / "Third person"（过 TR）
bool parseGameTypeId(const std::string& s, GameType& out);

// 代码方案（路线图 D8 的三档里，启动器只管前两档）
enum class CodeMode : int {
    Vks = 0,   // T1：`<工程>/assets/scripts/*.vks`（默认，现在就能交付）
    Cpp = 1,   // T2：C++ 游戏模块（M5-c 才实现）
};
const char* codeModeId(CodeMode m);          // "vks" / "cpp"
const char* codeModeLabel(CodeMode m);
bool parseCodeModeId(const std::string& s, CodeMode& out);

// ---------------------------------------------------------------- 数据

struct ProjectFile {
    int format = kProjectFormat;

    std::string name;            // "MyGame"（展示名，也是目录名建议值）
    std::string templateId;      // 创建时用的模板 id："first_person" / "third_person"
    GameType gameType = GameType::ThirdPerson;

    // 相对 `assets/` 的正斜杠路径。⚠ 必须落在 assets 里（逃逸检查会拒）。
    std::string startScene = "scenes/main.json";

    CodeMode codeMode = CodeMode::Vks;
    // 相对 `assets/` 的路径，**或**绝对路径（`codeLinked == true` 时）
    std::string codeRoot = "scripts";
    // true = `<root>/assets/scripts` 是指向外面 `codeRoot` 的目录联接
    bool codeLinked = false;

    // ISO8601 UTC（"2026-10-19T14:00:00Z"）。只展示，不参与任何决策。
    std::string createdAt;

    // 引擎标识。⚠ **只记录不校验**（路线图 §9 第 1 条：不做引擎版本管理）。
    std::string engineName = "venn";
    std::string engineConfig = "Debug";

    // ---- 运行期（不序列化）----
    // 读出来的工程根（绝对路径、正斜杠）。`load` 时填。
    // 空 = 这个对象不是从磁盘读来的（新建时由 create 填）。
    std::string root;
};

// ---------------------------------------------------------------- 结果类型
//
// 用"返回结构体 + error 字符串"而不是异常：启动器是 GUI，失败要**显示在
// 界面上**；抛异常穿出 UI 代码会变成"窗口直接没了"。
struct IoResult {
    bool ok = false;
    std::string error;      // 人话，可直接显示；ok==true 时为空
    explicit operator bool() const { return ok; }
};

// ---------------------------------------------------------------- 读写

// 工程根下的 `venn.project.json` 绝对路径。
std::string projectFilePath(const std::string& root);

// 从工程根读。失败原因写进 `err`（目录不存在 / 文件缺失 / json 语法错 /
// format 太新 / 必有键缺失 / 路径逃逸）。
// 成功时 `out.root` 被填成**规范化的绝对路径**（正斜杠、无尾斜杠）。
IoResult loadProject(const std::string& root, ProjectFile& out, std::string* err);

// 写。会创建父目录。`p.root` 必须已填。
IoResult saveProject(const ProjectFile& p, std::string* err);

// 只序列化成文本（自检 / 日志 / 对比用，不落盘）。
std::string toJsonText(const ProjectFile& p);

// ---------------------------------------------------------------- 校验

// 逐条校验（不碰磁盘）。失败返回 false + 人话。
// 覆盖：format 范围 / name 非空 / startScene 是安全的资产相对路径 /
//       codeRoot 在 linked 时必须绝对、否则必须是安全的资产相对路径 /
//       gameType 与 templateId 是否自相矛盾（只 WARN，不算失败）。
bool validate(const ProjectFile& p, std::string* err);

// 一个相对路径是否"安全"：非空、正斜杠、不含 ".." 段、不是绝对路径、
// 不带盘符、不带反斜杠、不带 NUL。
//
// ⚠ 这不是洁癖：`startScene` / `codeRoot` 会被拼成 `<root>/assets/<rel>`
// 然后**拿去打开 / 写文件**。`../../` 能让它写到工程外面去。
bool isSafeRelative(const std::string& rel);

// 一个路径是否绝对（含 Windows 盘符 / UNC / POSIX 根）。
bool isAbsolutePath(const std::string& path);

// 目标目录能不能用来建工程：**不存在**，或**存在且为空**。
//
// ⚠ 这一条是 P8 的正面判据。允许"存在但空"是因为文件夹选择器天然会返回
//   一个已经存在的目录（用户先建好再选）；不允许"非空"是因为创建失败不
//   清理会在里面留半成品，下次创建被"目录非空"拒，用户以为坏了。
//   错误串里带**第一个**挡路的条目名，让人一眼知道挡在哪。
IoResult checkTargetDirUsable(const std::string& absPath);

// ---------------------------------------------------------------- 路径规则

std::string assetRoot(const ProjectFile& p);      // <root>/assets
std::string startSceneAbs(const ProjectFile& p);  // <root>/assets/<startScene>
std::string codeRootAbs(const ProjectFile& p);    // 见文件头

// `<root>/assets/scripts`（T1 的默认落点；外置时它是指向 codeRoot 的联接）
std::string scriptsDirAbs(const ProjectFile& p);

// 把绝对路径规范成"正斜杠 + 无尾斜杠"的形式。失败返回原串。
std::string normalizePath(const std::string& path);

// 工程名 → 合法目录名（去掉 Windows 非法字符、去首尾空白/点、截断 64）。
// 空结果时返回 "Untitled"。
std::string sanitizeName(const std::string& name);

// 当前时间戳，ISO8601 UTC（"2026-10-19T14:00:00Z"）。
std::string nowIso8601();

// ---------------------------------------------------------------- 自检

// 无窗口、无 Vulkan、无 GPU（`--selftest project` / `MYVK_PROJECT_TEST=1`）。
// 覆盖：路径规则 / 逃逸拒绝 / 序列化往返 / 版本拒绝 / 名称消毒 / 冲突检测。
// 返回失败条数（0 = 全过），并在 stdout 打 `PROJ-TEST ...` 逐条结果。
int runProjectSelfTest();

// 四组一起跑：工程数据 / 最近列表 / 画面设置 / 模板创建。
//
// ⚠ **聚合只在这里写一遍**。`Game.exe` 与 `Launcher.exe` 都要跑全套，
//   各自写一份"四项相加"的话，将来加第五组必然漏掉一边 —— 而漏掉的表现
//   是"某个 exe 的自检全绿，实际那一组从没跑过"（正是本项目反复踩的
//   "断言没挂上去"那一类）。
// 返回**失败总数**（0 = 全过），并打 `PROJECT-ALL-TOTAL failures=N`。
int runAllProjectSelfTests();

} // namespace project
