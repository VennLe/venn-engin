#pragma once
// ============================================================
// project/ProjectCreate —— 从模板创建工程（路线图 M2 的核心）
//
// 一次创建 = 复制一个模板目录 + 替换白名单占位符 + 写身份文件。
//
// ------------------------------------------------------------
// 五个必须做对的地方（每一条都对应一个会静默出错的失效模式）
// ------------------------------------------------------------
// ① **先写到 `<root>.tmp` 再 rename**（P8）。写一半断电/崩溃不会留下
//    半成品工程；下次创建不会被"目录非空"拒掉。
//
// ② **失败删掉本次建的目录**。只删 `.tmp`（我们自己的），**绝不**碰
//    用户原有的 `root`。这条要专门测（M2 验收："失败无残留"）。
//
// ③ **占位符只按白名单替换**（P9）。`template.json.placeholders[]` 里的
//    那几个串才替换。全目录字符串替换会改坏材质名 / 资产 id —— 而那种
//    损坏在打开工程之前完全看不出来。
//
// ④ **复制的是"模板目录"本身，不是"模板目录里选出来的文件"**。
//    `files[]` 只用于**事后对账**（自检逐个断言存在），不参与复制逻辑 ——
//    否则模板里新增一个文件而忘了登记，它会被静默漏掉。
//
// ⑤ **recents 失败不回滚工程**（路线图 §7-M2 第 5 条）。工程已经建好了，
//    列表没记上只是"下次要手动找一下"，删掉整个工程是更糟的结果。
// ============================================================

#include "project/ProjectDisplay.h"
#include "project/ProjectFile.h"

#include <functional>
#include <string>
#include <vector>

namespace project {

// 创建的四个阶段（UI 的进度条 + 失败时"停在哪一步"都用它）。
enum class CreateStep : int {
    Validate = 0,      // 校验名字 / 目标目录 / 模板完整性
    CopyTemplate,      // 复制到 <root>.tmp
    ReplaceAndWrite,   // 占位符替换 + venn.project.json + 画面设置
    Commit,            // rename .tmp → root + 记入 recents
    Count
};
const char* createStepLabel(CreateStep s);   // "Validate" / "Copy template" / …

struct CreateSpec {
    GameType gameType = GameType::ThirdPerson;
    // 空 = 用 `gameTypeId(gameType)`。非空时必须是存在的模板 id。
    std::string templateId;
    std::string name;        // 展示名（会经过 sanitizeName）
    std::string root;        // **绝对**工程根（UI 已算好；必须不存在或为空）
    // 外置代码根（M5-b）。true 时 `externalCodeRoot` 必须绝对。
    bool codeExternal = false;
    std::string externalCodeRoot;
    // 画面（M4）：写 assets/game.json + config/render.json
    DisplaySpec display;
    bool addToRecent = true;
};

struct CreateResult {
    bool ok = false;
    std::string error;        // 人话（UI 直接显示）
    std::string root;         // 成功时的工程根（正斜杠）
    int filesCopied = 0;
    CreateStep failedAt = CreateStep::Validate;
};

// `progress` 可为空。每进入一个阶段调一次（UI 画进度条用）。
using ProgressFn = std::function<void(CreateStep, const char*)>;

CreateResult createFromTemplate(const CreateSpec& spec, const ProgressFn& progress);

// ---------------------------------------------------------------- 模板查找
//
// 候选链（第一个含 `<id>/template.json` 的赢）：
//   1. `MYVK_TEMPLATE_DIR`
//   2. `<exe>/templates`
//   3. `<cwd>/templates`
//
// 与 `assets::resolveAssetPath` 同一条"第一个存在的赢"思路 —— 工程里
// 不需要自带模板（它们随引擎发），但 CI 可以指到别处。
std::string templatesRoot();
std::string templateDir(const std::string& templateId);

// 模板清单（`template.json`）。`files[]` 供事后对账。
struct TemplateManifest {
    int format = 1;
    std::string id;
    std::string label;
    std::string summary;
    GameType gameType = GameType::ThirdPerson;
    std::string startScene = "scenes/main.json";
    std::vector<std::string> placeholders;
    std::vector<std::string> files;
    std::string dir;   // 模板目录（绝对）
};

IoResult loadTemplateManifest(const std::string& templateId,
                              TemplateManifest& out, std::string* err);

// ---------------------------------------------------------------- junction
//
// 目录联接（`mklink /J`）。**不需要管理员权限**（这是选它而不是
// `mklink /D` 的唯一理由 —— 后者要开发者模式或管理员）。
//
// 为什么用 junction 而不是"改引擎读 code.root"：脚本路径在引擎里是
// **资产相对路径**（`ScriptComponent::path` 走 `resolveAssetPath`）。
// junction 让引擎零改动，也让用户在外部目录里用任何编辑器打开同一个
// 代码根。删工程时 `rmdir` 不跟随目标，外部代码根天然保留。
bool createDirectoryJunction(const std::string& linkPath,
                             const std::string& targetPath, std::string* err);

// 这个路径是不是一个目录联接 / 符号链接。
bool isDirectoryLink(const std::string& path);

// 读回联接指向的目标（不是联接返回空串）。
std::string directoryLinkTarget(const std::string& path);

// ---------------------------------------------------------------- 自检
//
// `--selftest project` 的 K 组。**会真的建目录**（在 temp 下），
// 覆盖：模板查找 / 清单解析 / 创建成功 / 目录结构对账 /
//       重复创建被拒 / 非空目录被拒 / 占位符替换 / 失败无残留。
int runCreateSelfTest();

} // namespace project
