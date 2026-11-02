#include "ui2/UiAction.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ui2 {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// key / clip 的合法性：非空、无空格。
// ⚠ 为什么禁空格：日志锚点（`UIDOC-ACTION kind=flag key=hp value=10`）
//   按 `\S*` 解析，key 里一个空格就把后面所有字段挤位。
bool validKey(const std::string& k) {
    if (k.empty()) return false;
    for (char c : k) {
        if (std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

}  // namespace

const char* uiActionKindName(UiActionKind k) {
    switch (k) {
        case UiActionKind::None:   return "none";
        case UiActionKind::Flag:   return "flag";
        case UiActionKind::Toggle: return "toggle";
        case UiActionKind::Sound:  return "sound";
        case UiActionKind::Quit:   return "quit";
        default:                   return "?";
    }
}

bool uiActionKindFromName(const std::string& s, UiActionKind& out) {
    for (int i = 0; i < static_cast<int>(UiActionKind::Count); ++i) {
        const auto k = static_cast<UiActionKind>(i);
        if (s == uiActionKindName(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

bool parseUiAction(const std::string& text, UiAction& out, std::string* err) {
    out = UiAction{};
    if (err) err->clear();

    const std::string s = trim(text);
    if (s.empty()) return true;         // 没配动作 = 合法

    // `quit` 是唯一不带冒号的动词。
    if (s == "quit") {
        out.kind = UiActionKind::Quit;
        return true;
    }

    const std::size_t colon = s.find(':');
    if (colon == std::string::npos) {
        if (err) {
            *err = "action must be '<verb>:<arg>' or 'quit' (got '" + s + "')";
        }
        return false;
    }
    const std::string verb = trim(s.substr(0, colon));
    const std::string arg = trim(s.substr(colon + 1));

    UiActionKind kind = UiActionKind::None;
    if (!uiActionKindFromName(verb, kind) || kind == UiActionKind::None) {
        if (err) {
            // ⚠ 提示里的动词表必须与 `UiActionKind` 逐字一致。原来这里
            //   还写着 `emit`（评估过但砍掉的动词）—— 作者照着提示去写
            //   `emit:xxx` 会再吃一次同样的错，等于错误提示本身在骗人。
            *err = "unknown action verb '" + verb +
                   "' (flag / toggle / sound / quit)";
        }
        return false;
    }

    switch (kind) {
        case UiActionKind::Flag: {
            // `flag:hp=10` / `flag:hp`（= 1）
            const std::size_t eq = arg.find('=');
            const std::string key = trim(eq == std::string::npos
                                             ? arg
                                             : arg.substr(0, eq));
            if (!validKey(key)) {
                if (err) *err = "flag:<name> needs a name without spaces";
                return false;
            }
            out.kind = kind;
            out.key = key;
            if (eq != std::string::npos) {
                const std::string num = trim(arg.substr(eq + 1));
                if (num.empty()) {
                    if (err) *err = "flag:<name>=<number> is missing the number";
                    return false;
                }
                char* end = nullptr;
                const double v = std::strtod(num.c_str(), &end);
                // ⚠ 必须整串都吃掉：`flag:hp=10abc` 用 atof 会静默得到 10，
                //   于是"写错了"表现成"值对了但语义不对"，最难查。
                if (!end || *end != '\0') {
                    if (err) *err = "flag:<name>=<number> got '" + num +
                                    "' which is not a number";
                    return false;
                }
                out.value = v;
            }
            return true;
        }
        case UiActionKind::Toggle:
        case UiActionKind::Sound: {
            if (!validKey(arg)) {
                if (err) {
                    *err = std::string(uiActionKindName(kind)) +
                           ":<arg> needs a non-empty argument without spaces";
                }
                return false;
            }
            out.kind = kind;
            out.key = arg;
            return true;
        }
        default:
            break;
    }
    if (err) *err = "unsupported action";
    return false;
}

std::string uiActionDescribe(const UiAction& a) {
    switch (a.kind) {
        case UiActionKind::None:
            return "(none)";
        case UiActionKind::Flag: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "flag:%s=%.4g", a.key.c_str(),
                          a.value);
            return buf;
        }
        case UiActionKind::Toggle:
            return "toggle:" + a.key;
        case UiActionKind::Sound:
            return "sound:" + a.key;
        case UiActionKind::Quit:
            return "quit";
        default:
            return "?";
    }
}

}  // namespace ui2
