// ============================================================
// editor/Localization.cpp —— **转发壳**（U7，2026-10-18）
//
// 词典（1559 条）与查表逻辑都搬到了 `engine/i18n/`：
// 运行时要和编辑器共用**同一份**词典，而 i18n 只要还住在 editor/ 里，
// `runtime → engine ← editor` 这条单向依赖就断了。
//
// 这个文件现在只剩两件事：
//   1) 把 editor 的符号（`editor::tr` / `setLanguage` / `language` …）
//      转给 `i18n::` —— 于是所有 `TR()` 与 `trId()` 调用点**一个字都没改**；
//   2) 两个**编辑器专属**的包装：`trId`（ImGui 窗口 ID）与 `trLabel`
//      （`label##stableId`）。它们服务于 ImGui 的 ID 语义，天然是编辑器
//      概念，不该塞进引擎的 i18n 里。
//
// ⚠ 别在这里再养一份词典。两份一定漂移，而且漂移得很隐蔽 ——
//   "改了词条界面没变"就是其中一种症状（本地化这边还叠着 `emplace`
//   先到先得的坑）。单一权威只有 `engine/i18n/ZhTable.cpp`。
// ============================================================

#include "Localization.h"

#include "i18n/I18n.h"

#include <cstring>
#include <string>
#include <unordered_map>

namespace editor {

int languageCount() { return i18n::languageCount(); }

const char* languageName(int index) { return i18n::languageName(index); }

void setLanguage(int index) { i18n::setLanguage(index); }

int language() { return i18n::language(); }

bool isChinese() { return i18n::isChinese(); }

const char* tr(const char* english) { return i18n::tr(english); }

const char* trId(const char* english, const char* stableId) {
    if (!stableId) stableId = "";
    if (!english) english = "";
    const int lang = i18n::language();
    // 缓存 key 必须带上语言：换语言后同一个 stableId 要拿到新译文。
    // 用 "语言 \x1f 英文" 当 key，避免中文模式下英文标题被当成英文模式的结果。
    static std::unordered_map<std::string, std::string> cache;
    std::string key;
    key.reserve(64);
    key += static_cast<char>('0' + (lang & 7));
    key += '\x1f';
    key += english;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.c_str();
    std::string v = i18n::tr(english);
    v += "###";
    v += stableId;
    return cache.emplace(std::move(key), std::move(v)).first->second.c_str();
}

const char* trLabel(const char* englishLabel) {
    if (!englishLabel) return "";
    // 英文模式原样返回：零风险路径，也是回归基准。**连缓存都不碰**，
    // 这样切回英文时拿到的就是字面量本身。
    const int lang = i18n::language();
    if (lang != static_cast<int>(i18n::Language::Chinese)) return englishLabel;

    // 按第一个 `##` 切开。`###` 也走这里（它含 `##`），后缀原样保留 ——
    // 两者对 ImGui 的语义不同（`##` 截断显示、`###` 强制全局唯一 ID），
    // 但"从这里往后的部分不许动"是共同的。
    const char* sep = std::strstr(englishLabel, "##");
    const std::size_t headLen =
        sep ? static_cast<std::size_t>(sep - englishLabel)
            : std::strlen(englishLabel);

    std::string key;
    key.reserve(headLen + 8);
    key += static_cast<char>('0' + (lang & 7));   // 语言进 key
    key += '\x1f';
    key.append(englishLabel, headLen);

    static std::unordered_map<std::string, std::string> cache;
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second.c_str();

    std::string v;
    v.reserve(std::strlen(englishLabel) + 16);
    if (headLen == 0 && sep) {
        v = englishLabel;              // 纯 "##id"（无可见文字）：原样
    } else {
        // 词条查的是**去掉后缀**的前半截 —— 词典里只存 "Duration"，
        // 不存 "Duration##fx"，否则同一句话会因为后缀不同重复几十条。
        const std::string head(englishLabel, headLen);
        v = i18n::tr(head.c_str());
        if (sep) v.append(sep);
    }
    return cache.emplace(std::move(key), std::move(v)).first->second.c_str();
}

} // namespace editor
