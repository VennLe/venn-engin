#pragma once
// ============================================================
// engine/i18n/I18n —— 引擎级界面语言（中 / 英）
//
// 为什么住在 engine/ 而不是 editor/（U7 搬家，2026-10-18）：
//   运行时（Game.exe）也要显示中文。i18n 只要还留在 editor/ 里，
//   `runtime → engine ← editor` 这条单向依赖就断了 —— 运行时要么够不着
//   词典，要么被迫依赖 editor/。搬到 engine/ 之后两侧共用**同一份**词典。
//
// ------------------------------------------------------------
// 单一权威：`engine/i18n/ZhTable.cpp`
// ------------------------------------------------------------
// 词典只许有一份。两份一定漂移，而且漂移的方式很难发现（"改了词条界面
// 没变"就是其中一种，本地化这边还叠着 `emplace` 先到先得的坑）。
// `editor::tr` 现在是**一行转发**，调用点那 700 多处 `TR()` 一个字没动。
//
// ------------------------------------------------------------
// 三条纪律（沿用 editor/Localization.h 的原文，仍然全部有效）
// ------------------------------------------------------------
//   1) `tr()` 的入参必须是**字符串字面量**：英文原文得永远可查，
//      拼接出来的串查不到只会静默回退英文。
//   2) **`%` 占位符在译文里必须一模一样**（数量、顺序、类型）。
//      翻错会在运行时把格式化输出搞乱 —— 崩溃级（`%s` 换成 `%d` 会让
//      `snprintf` 把整数当指针解引用）。`verify_i18n_coverage.py` 的 F 组
//      专治这一条。
//   3) **标识符不翻译**：枚举值、命令 id、落盘 key、日志 tag、着色器
//      关键字一律英文；翻译只发生在"要画给人看的那一层"。
//
// ------------------------------------------------------------
// 英文模式 = 零开销
// ------------------------------------------------------------
// 词典表是**首次用到时才建**的函数内静态。英文模式下 `tr()` 在查表之前
// 就返回了，表根本不会被构造 —— 这就是启动日志里 `I18N-KEY lang=en count=0`
// 的来源（`translationCount()` 返回 0 而不是 1558）。
// ============================================================

#include <cstddef>

namespace i18n {

// 一条译文。`en` 是**英文原文**（调用点写的那个字面量），`zh` 是中文。
struct Entry {
    const char* en;
    const char* zh;
};

// 词典本体（定义在 ZhTable.cpp）。数组长度由 `kZhCount` 单独给出 ——
// 跨 TU 拿不到 `sizeof(kZh)`（不完整类型）。
extern const Entry kZh[];
extern const std::size_t kZhCount;

enum class Language : int {
    English = 0,
    Chinese = 1,
    Count
};

int languageCount();
// 语言**自名**（"English" / "中文"）。语言选择器里就该用它自己的文字 ——
// 这是通行做法，也让"我看不懂当前语言"这件事不可能发生。
const char* languageName(int index);

// 语言**码**（"en" / "zh"）：给 game.json / 命令行 / 日志用。
// ⚠ 与 languageName 分开：配置值一旦本地化，换个界面语言旧存档就读不懂了。
const char* languageCode(int index);
// "zh" / "zh-CN" / "chinese" → 1；"en" / "en-US" / "english" → 0。
// 认不出返回 **-1**（调用方保留原值 —— 别静默改成英文，那会让"配错了"
// 看起来像"配对了但没生效"）。
int languageFromCode(const char* code);

void setLanguage(int index);
int language();
bool isChinese();

// 英文原文 → 当前语言的显示文本。没有译文时**原样返回英文**。
const char* tr(const char* english);

// ---- 运行时补词条（模组 / 脚本 / 将来的设置界面）----
// 静态表在只读段、只许有一份；运行时新增的放堆上。两者走同一条查表路径，
// 但**新增的优先**（后写的覆盖，符合"运行时覆盖"的直觉）。
void addTranslation(const char* english, const char* localized);
void clearAddedTranslations();
// 期望出现在启动日志里：`I18N-KEY lang=zh count=1558`。
// 英文模式返回 0 —— 表还没建，这就是"英文 = 零开销"的观测量。
std::size_t translationCount();

} // namespace i18n
