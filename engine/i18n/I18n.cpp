// ============================================================
// engine/i18n/I18n.cpp —— 查表 / 语言切换 / 运行时补词条
//
// 词典本体在 ZhTable.cpp（单一权威）。这个文件只负责"怎么查"。
// ============================================================

#include "i18n/I18n.h"

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

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

int g_lang = static_cast<int>(Language::English);

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

void setLanguage(int index) {
    if (index < 0) index = 0;
    if (index >= languageCount()) index = languageCount() - 1;
    if (index == g_lang) return;
    g_lang = index;
    // ⚠ 这里**不**清 addedMap()：运行时补的词条是"这个游戏的内容"，
    //   不是"当前语言的缓存"（缓存是 editor 侧 trId/trLabel 各自的 key，
    //   它们把语言下标编进了 key，换语言自然拿新值）。
}

int language() { return g_lang; }

bool isChinese() { return g_lang == static_cast<int>(Language::Chinese); }

const char* tr(const char* english) {
    if (!english) return "";
    // 英文模式直接原样返回：**零风险路径**，也是回归的基准路径。
    // 在查表之前就返回 = 表不会构造 = 零开销。
    if (g_lang != static_cast<int>(Language::Chinese)) return english;

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
    if (g_lang != static_cast<int>(Language::Chinese)) return 0;
    return zhMap().size() + addedMap().size();
}

} // namespace i18n
