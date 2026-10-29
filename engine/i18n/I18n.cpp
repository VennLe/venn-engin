// ============================================================
// engine/i18n/I18n.cpp —— 查表 / 语言切换 / 运行时补词条
//
// 词典本体在 ZhTable.cpp（单一权威）。这个文件只负责"怎么查"。
// ============================================================

#include "i18n/I18n.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
// 只要 `GetUserDefaultUILanguage` / `LANGID` / `PRIMARYLANGID` / `LANG_CHINESE`。
// 一个 .cpp 里独占 include 就不会污染模板重的那些 TU（min/max 宏是经典雷）。
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace i18n {
namespace {

using Map = std::unordered_map<std::string_view, const char*>;

// 静态词典表：**首次用到时才建**（函数内静态）。
//
// 为什么这件事重要：英文模式下 `tr()` 在查表之前就返回，这个函数永远不会
// 被调用 → 表不构造 → `translationCount()` 是 0。启动日志里的
// `I18N-KEY lang=en count=0` 就是它的观测量，也是"英文 = 零开销"的证据。
const Map& zhMap() {
    static const Map m = [] {
        Map out;
        out.reserve(kZhCount * 2);
        // ⚠ emplace = **先到先得**。同一个 en 出现两次时后写的永远读不到。
        //   重复键由 verify_i18n_coverage.py 的 C 组报，别在这里改成
        //   insert_or_assign —— 那会把"源码里写重了"这个错误藏起来。
        for (std::size_t i = 0; i < kZhCount; ++i)
            out.emplace(kZh[i].en, kZh[i].zh);
        return out;
    }();
    return m;
}

// 运行时新增/覆盖的词条。键是 std::string（调用方给的是动态串），
// 与静态表的 string_view 键分开存：静态表在只读数据段，谁也不该动它。
std::unordered_map<std::string, std::string>& addedMap() {
    static std::unordered_map<std::string, std::string> m;
    return m;
}

// 当前语言。**-1 = 还没决定过** → 首次使用时取系统语言。
//
// 为什么用哨兵而不是"构造期就算一次"：`systemLanguage()` 在 Windows 上要
// 问 `GetUserDefaultUILanguage()`，那是进程级常量、随时可问；而静态初始化
// 阶段（DLL 依赖还没走完）调 Win32 是真出过事的。惰性求值两头都安全，
// 也顺手保住"英文模式不建表"那条（没人查过语言 = 一条都不碰）。
int g_lang = -1;

// 唯一读 `g_lang` 的地方：把"未决定"钉成系统语言。
int resolvedLang() {
    if (g_lang < 0) g_lang = systemLanguage();
    return g_lang;
}

// 大小写不敏感的前缀比较（"ZH-CN" / "zh_CN" 都要认）。
bool startsWithCI(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    for (; *prefix; ++s, ++prefix) {
        if (!*s) return false;
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

} // namespace

int languageCount() { return static_cast<int>(Language::Count); }

const char* languageName(int index) {
    switch (static_cast<Language>(index)) {
    case Language::Chinese: return "中文";
    case Language::English:
    default: return "English";
    }
}

const char* languageCode(int index) {
    return static_cast<Language>(index) == Language::Chinese ? "zh" : "en";
}

int languageFromCode(const char* code) {
    if (!code || !*code) return -1;
    if (startsWithCI(code, "zh") || startsWithCI(code, "chinese") ||
        startsWithCI(code, "cn"))
        return static_cast<int>(Language::Chinese);
    if (startsWithCI(code, "en") || startsWithCI(code, "english"))
        return static_cast<int>(Language::English);
    // 认不出就**明确失败**，让调用方保留原值：
    // "配错了"（拼错 game.json）与"配对了没生效"必须是两种可区分的现象。
    return -1;
}

int systemLanguage() {
#if defined(_WIN32)
    // `GetUserDefaultUILanguage`：**用户界面语言**（不是区域格式、不是
    // 系统安装语言）。设成"格式：中国 / 界面：English"的机器答 1033，
    // 那就该给英文 —— 界面语言是"这个人看得懂哪种字"的唯一权威答案。
    const LANGID ui = ::GetUserDefaultUILanguage();
    if (ui != 0 && PRIMARYLANGID(ui) == LANG_CHINESE)
        return static_cast<int>(Language::Chinese);
    return static_cast<int>(Language::English);
#else
    // POSIX：没有"界面语言"这个概念，惯例就是看这几个环境变量
    // （"zh_CN.UTF-8" / "zh_TW" / "en_US.UTF-8"，`languageFromCode` 认前缀）。
    static const char* const kEnv[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    for (const char* k : kEnv) {
        const char* v = std::getenv(k);
        if (!v || !*v) continue;
        const int idx = languageFromCode(v);
        if (idx >= 0) return idx;
    }
    return static_cast<int>(Language::English);
#endif
}

const char* systemLanguageCode() { return languageCode(systemLanguage()); }

void setLanguage(int index) {
    if (index < 0) index = 0;
    if (index >= languageCount()) index = languageCount() - 1;
    if (index == resolvedLang()) return;
    g_lang = index;
    // ⚠ 这里**不**清 addedMap()：运行时补的词条是"这个游戏的内容"，
    //   不是"当前语言的缓存"（缓存是 editor 侧 trId/trLabel 各自的 key，
    //   它们把语言下标编进了 key，换语言自然拿新值）。
}

int language() { return resolvedLang(); }

bool isChinese() { return resolvedLang() == static_cast<int>(Language::Chinese); }

const char* tr(const char* english) {
    if (!english) return "";
    // 英文模式直接原样返回：**零风险路径**，也是回归的基准路径。
    // 在查表之前就返回 = 表不会构造 = 零开销。
    if (resolvedLang() != static_cast<int>(Language::Chinese)) return english;

    // 运行时新增的优先（显式覆盖；模组改一条词条不需要改源码）。
    const auto& extra = addedMap();
    if (!extra.empty()) {
        const auto it = extra.find(english);
        if (it != extra.end()) return it->second.c_str();
    }

    const Map& m = zhMap();
    const auto it = m.find(std::string_view(english));
    return it == m.end() ? english : it->second;
}

void addTranslation(const char* english, const char* localized) {
    if (!english || !*english) return;
    if (!localized) localized = "";
    // insert_or_assign：与静态表的 emplace 不同，这里的"后写覆盖"是
    // **显式行为**（调用方明确要给这条换个译文），不是"源码写重了"。
    addedMap().insert_or_assign(std::string(english), std::string(localized));
}

void clearAddedTranslations() { addedMap().clear(); }

std::size_t translationCount() {
    // 英文模式：表还没建，如实报 0（"不建表 = 零开销"的观测量）。
    if (resolvedLang() != static_cast<int>(Language::Chinese)) return 0;
    return zhMap().size() + addedMap().size();
}

} // namespace i18n
