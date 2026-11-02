// ============================================================
// dialogue/DialogueAsset.cpp
// ============================================================

#include "dialogue/DialogueAsset.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace dialogue {

namespace {

using json = nlohmann::json;

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string s = (slash == std::string::npos) ? path : path.substr(slash + 1);
    // 去掉 `.dlg.json` 的双后缀
    const size_t dlg = s.rfind(".dlg.json");
    if (dlg != std::string::npos) return s.substr(0, dlg);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) return s.substr(0, dot);
    return s;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

int findNode(const DialogueAsset& a, const std::string& id) {
    const auto it = a.index.find(id);
    return (it == a.index.end()) ? -1 : it->second;
}

// ---------------------------------------------------------------- C7 条件

const char* condOpError(const std::string& op) {
    if (op == ">=" || op == "<=" || op == ">" || op == "<" || op == "==" ||
        op == "!=" || op == "=")
        return nullptr;
    return "has an unknown op (use >= <= > < == != )";
}

namespace {
// `=` 是 `==` 的同义词（作者常写 `"op": "="`）。归一化只在这里做一次，
// 求值路径上不再判断 —— 两处判断就会有两处不一致。
bool compareFlag(double a, const std::string& op, double b) {
    if (op == ">=") return a >= b;
    if (op == "<=") return a <= b;
    if (op == ">") return a > b;
    if (op == "<") return a < b;
    if (op == "==" || op == "=") return a == b;
    if (op == "!=") return a != b;
    // 走到这里说明 op 没经过 `condOpError` 检查（作者绕过了 parse 直接拼
    // 结构体）。**返回 false**：不认识的运算符不能当"恒真"—— 那会让
    // 一整段条件被静默忽略，而剧本看着完全正常。
    return false;
}
} // namespace

bool evalConds(const std::vector<DlgCond>& conds, const DlgFlagStore* store) {
    if (conds.empty()) return true;
    // ⚠ 没有 store 时**恒假**，而且是调用方（runner）负责报错。
    //   在这里返回 false 而不是 true：条件不成立会跳到 else，走线是
    //   "最保守"的那条，而 runner 会把 `no flag store` 写进 error ——
    //   两个信号一起出现，排查时不会误判成"剧本写错了"。
    if (!store) return false;
    for (const auto& c : conds) {
        if (!compareFlag(store->get(c.flag), c.op, c.value)) return false;
    }
    return true;
}

int revealedChars(float elapsed, float cps, int total) {
    if (total <= 0) return 0;
    // ⚠ `cps <= 0` = **关掉打字机**（立刻全出），不是"永远显示 0 个字"。
    if (!(cps > 0.0f)) return total;
    if (!(elapsed > 0.0f)) return 0;
    const float n = std::floor(elapsed * cps);
    if (n >= static_cast<float>(total)) return total;
    return static_cast<int>(n);
}

// ------------------------------------------------------------ UTF-8

namespace {

// 从一个 UTF-8 头字节推出这个码点占几字节。非法头字节（0x80~0xBF 续字节、
// 或 0xF8+）返回 1 —— 与 `ui2::utf8Next` 的"非法按 1 字节前进"一致。
int utf8Len(unsigned char c) {
    if (c < 0x80u) return 1;
    if ((c & 0xE0u) == 0xC0u) return 2;
    if ((c & 0xF0u) == 0xE0u) return 3;
    if ((c & 0xF8u) == 0xF0u) return 4;
    return 1;
}

} // namespace

int utf8Count(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        const int len = utf8Len(static_cast<unsigned char>(s[i]));
        // 长度越界（截断的尾巴）也按 1 字节走，绝不能 `i += len` 冲出 end()
        i += (i + static_cast<size_t>(len) <= s.size()) ? static_cast<size_t>(len)
                                                        : 1u;
        ++n;
    }
    return n;
}

std::string utf8Prefix(const std::string& s, int chars) {
    if (chars <= 0) return {};
    int n = 0;
    size_t i = 0;
    while (i < s.size() && n < chars) {
        const int len = utf8Len(static_cast<unsigned char>(s[i]));
        i += (i + static_cast<size_t>(len) <= s.size()) ? static_cast<size_t>(len)
                                                        : 1u;
        ++n;
    }
    return s.substr(0, i);
}

bool parseDialogue(const std::string& text, const std::string& sourcePath,
                   DialogueAsset& out) {
    out = DialogueAsset{};
    out.sourcePath = sourcePath;
    out.name = baseName(sourcePath);

    json doc;
    try {
        doc = json::parse(text, nullptr, true, true);   // 允许 `//` 注释
    } catch (const std::exception& e) {
        out.error = std::string("JSON parse error: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        out.error = "root must be an object";
        return false;
    }

    if (doc.contains("name") && doc["name"].is_string())
        out.name = doc["name"].get<std::string>();

    const json* arr = nullptr;
    if (doc.contains("lines")) arr = &doc["lines"];
    else if (doc.contains("nodes")) arr = &doc["nodes"];
    if (!arr || !arr->is_array()) {
        out.error = "missing \"lines\" array";
        return false;
    }
    if (arr->empty()) {
        out.error = "no lines";
        return false;
    }

    // ---- 节点 ----
    int autoId = 0;
    for (const auto& jn : *arr) {
        if (!jn.is_object()) {
            out.error = "every line must be an object";
            return false;
        }
        DlgNode n;
        if (jn.contains("id") && jn["id"].is_string())
            n.id = jn["id"].get<std::string>();
        if (n.id.empty()) {
            // 省 id 是允许的（作者写线性对话时不该被逼着编号），但**必须**
            // 变成稳定且唯一的 id，否则 next 没法寻址。
            n.id = "line" + std::to_string(autoId);
            out.warnings.push_back("line " + std::to_string(autoId) +
                                   " has no id (auto: " + n.id + ")");
        }
        ++autoId;

        if (jn.contains("speaker") && jn["speaker"].is_string())
            n.speaker = jn["speaker"].get<std::string>();
        if (jn.contains("text") && jn["text"].is_string())
            n.text = jn["text"].get<std::string>();
        if (jn.contains("next") && jn["next"].is_string())
            n.nextId = jn["next"].get<std::string>();
        // D1：语音路径。缺省空 = 这句没有语音（老资产逐位不变）。
        if (jn.contains("voice") && jn["voice"].is_string())
            n.voice = jn["voice"].get<std::string>();

        // ---- C7：条件（`if`）----
        // 命名取 `if` 而不是 `when`/`cond`：剧作者读 JSON 时最认这个词。
        // ⚠ `if` 是 JSON 键，不是关键字 —— 不冲突。
        if (jn.contains("if")) {
            if (!jn["if"].is_array()) {
                out.error = "line \"" + n.id + "\": if must be an array";
                return false;
            }
            for (const auto& jc : jn["if"]) {
                if (!jc.is_object()) {
                    out.error = "line \"" + n.id +
                                "\": every if entry must be an object";
                    return false;
                }
                DlgCond c;
                if (jc.contains("flag") && jc["flag"].is_string())
                    c.flag = jc["flag"].get<std::string>();
                if (jc.contains("op") && jc["op"].is_string())
                    c.op = jc["op"].get<std::string>();
                if (jc.contains("value") && jc["value"].is_number())
                    c.value = jc["value"].get<double>();
                if (c.flag.empty()) {
                    out.error = "line \"" + n.id + "\": if entry needs flag";
                    return false;
                }
                // ⚠ 运算符拼错必须在**加载期**炸掉。留到运行期的话，
                //   一个不认识的 op 会被某个实现当"恒真"、另一个当"恒假"，
                //   而两边都"能跑"—— 剧本走线却对不上，谁也没错。
                if (const char* why = condOpError(c.op)) {
                    out.error = "line \"" + n.id + "\": if \"" + c.flag +
                                "\" " + why;
                    return false;
                }
                n.conds.push_back(std::move(c));
            }
            if (jn.contains("else") && jn["else"].is_string())
                n.elseId = jn["else"].get<std::string>();
            // `if` 为空数组等于无条件 —— 那是作者在删条件时留下的壳，
            // 不是错误，但要提醒一句（`else` 会永远走不到）。
            if (n.conds.empty())
                out.warnings.push_back("line \"" + n.id +
                                       "\" has an empty if (never skips)");
        } else if (jn.contains("else")) {
            // 没有 `if` 却写了 `else`：这条线永远走不到，几乎肯定是笔误。
            out.warnings.push_back("line \"" + n.id +
                                   "\" has else without if (ignored)");
        }

        if (jn.contains("choices")) {
            if (!jn["choices"].is_array()) {
                out.error = "line \"" + n.id + "\": choices must be an array";
                return false;
            }
            for (const auto& jc : jn["choices"]) {
                if (!jc.is_object()) {
                    out.error = "line \"" + n.id + "\": choice must be an object";
                    return false;
                }
                DlgChoice c;
                if (jc.contains("text") && jc["text"].is_string())
                    c.text = jc["text"].get<std::string>();
                if (jc.contains("goto") && jc["goto"].is_string())
                    c.gotoId = jc["goto"].get<std::string>();
                if (c.text.empty()) {
                    out.error = "line \"" + n.id + "\": choice needs text";
                    return false;
                }
                if (c.gotoId.empty()) {
                    out.error = "line \"" + n.id + "\": choice \"" + c.text +
                                "\" needs goto";
                    return false;
                }
                // C7：选项的副作用（`set`）。
                if (jc.contains("set")) {
                    if (!jc["set"].is_array()) {
                        out.error = "line \"" + n.id + "\": choice \"" + c.text +
                                    "\" set must be an array";
                        return false;
                    }
                    for (const auto& js : jc["set"]) {
                        if (!js.is_object()) {
                            out.error = "line \"" + n.id + "\": choice \"" +
                                        c.text + "\" set entries must be objects";
                            return false;
                        }
                        DlgWrite w;
                        if (js.contains("flag") && js["flag"].is_string())
                            w.flag = js["flag"].get<std::string>();
                        if (js.contains("value") && js["value"].is_number())
                            w.value = js["value"].get<double>();
                        if (w.flag.empty()) {
                            out.error = "line \"" + n.id + "\": choice \"" +
                                        c.text + "\" set entry needs flag";
                            return false;
                        }
                        c.writes.push_back(std::move(w));
                    }
                }
                n.choices.push_back(std::move(c));
            }
            if (!n.nextId.empty()) {
                // 两者并存 = 玩家选完还"顺便"往前走一句？语义冲突，
                // 而且两个实现会给出两种行为 —— 直接判错。
                out.error = "line \"" + n.id +
                            "\": has both next and choices";
                return false;
            }
        }

        if (n.text.empty() && n.choices.empty()) {
            out.warnings.push_back("line \"" + n.id +
                                   "\" has neither text nor choices");
        }
        out.nodes.push_back(std::move(n));
    }

    // ---- id 唯一 + 索引 ----
    for (int i = 0; i < static_cast<int>(out.nodes.size()); ++i) {
        const std::string& id = out.nodes[static_cast<size_t>(i)].id;
        if (out.index.count(id)) {
            // ⚠ 不许静默合并：两个节点变一个，goto 会落到前者身上 ——
            //   剧本"能跑"，只是少了一段。与材质同名合并同一条纪律。
            out.error = "duplicate line id \"" + id + "\"";
            return false;
        }
        out.index[id] = i;
    }

    // ---- 起点 ----
    if (doc.contains("start") && doc["start"].is_string())
        out.startId = doc["start"].get<std::string>();
    if (out.startId.empty()) out.startId = out.nodes.front().id;
    if (findNode(out, out.startId) < 0) {
        out.error = "start \"" + out.startId + "\" is not a line id";
        return false;
    }

    // ---- 跳转目标（加载时一次查完）----
    for (const auto& n : out.nodes) {
        if (!n.nextId.empty() && findNode(out, n.nextId) < 0) {
            out.error = "line \"" + n.id + "\": next \"" + n.nextId +
                        "\" is not a line id";
            return false;
        }
        // C7：`else` 与 next 一样，在加载期就查完。
        if (!n.elseId.empty() && findNode(out, n.elseId) < 0) {
            out.error = "line \"" + n.id + "\": else \"" + n.elseId +
                        "\" is not a line id";
            return false;
        }
        for (const auto& c : n.choices) {
            if (findNode(out, c.gotoId) < 0) {
                out.error = "line \"" + n.id + "\": choice \"" + c.text +
                            "\" goto \"" + c.gotoId + "\" is not a line id";
                return false;
            }
        }
    }

    out.valid = true;
    // C7：`conds` / `writes` 是"这份剧本用了分支"的机器可读证据。
    // 追加在**行尾**：验收脚本按 `next=` 之类前缀取值，加在末尾最安全。
    int conds = 0, writes = 0;
    for (const auto& n : out.nodes) {
        conds += static_cast<int>(n.conds.size());
        for (const auto& c : n.choices)
            writes += static_cast<int>(c.writes.size());
    }
    VK_LOG_INFO("DLG-ASSET name=%s lines=%d start=%s warns=%d conds=%d writes=%d",
                out.name.c_str(), static_cast<int>(out.nodes.size()),
                out.startId.c_str(), static_cast<int>(out.warnings.size()), conds,
                writes);
    for (const auto& w : out.warnings) VK_LOG_WARN("DLG-WARN %s", w.c_str());
    return true;
}

bool loadDialogue(const std::string& path, DialogueAsset& out) {
    const std::string full = assets::resolveAssetPath(path);
    std::string text;
    if (!readFile(full, text)) {
        out = DialogueAsset{};
        out.sourcePath = path;
        out.name = baseName(path);
        out.error = "cannot read " + full;
        return false;
    }
    return parseDialogue(text, path, out);
}

// ---------------------------------------------------------------- 写回

std::string toJson(const DialogueAsset& a) {
    // ⚠ `ordered_json` 而不是 `json`：后者的键序是**字典序**（choices /
    //   id / next / speaker / text），作者打开文件看到的第一个字段是
    //   `choices` —— 一份剧本里最不常改的东西排在最前面。
    //   `ordered_json` 保持插入顺序，于是落盘长得和 docs 里的示例一致。
    nlohmann::ordered_json lines = nlohmann::ordered_json::array();
    for (const auto& n : a.nodes) {
        nlohmann::ordered_json jn = nlohmann::ordered_json::object();
        jn["id"] = n.id;
        if (!n.speaker.empty()) jn["speaker"] = n.speaker;
        jn["text"] = n.text;
        if (!n.nextId.empty()) jn["next"] = n.nextId;
        // D1：只在有语音时写 —— 老资产存回去**不许长出** `"voice": ""`。
        if (!n.voice.empty()) jn["voice"] = n.voice;
        // C7：条件与 else。同样只在有内容时写（老资产存回去逐位不变）。
        // ⚠ `if` 在前、`else` 在后 —— 读起来就是"如果…否则…"。
        if (!n.conds.empty()) {
            nlohmann::ordered_json jf = nlohmann::ordered_json::array();
            for (const auto& c : n.conds) {
                nlohmann::ordered_json o = nlohmann::ordered_json::object();
                o["flag"] = c.flag;
                o["op"] = c.op;
                o["value"] = c.value;
                jf.push_back(std::move(o));
            }
            jn["if"] = std::move(jf);
            if (!n.elseId.empty()) jn["else"] = n.elseId;
        }
        if (!n.choices.empty()) {
            nlohmann::ordered_json jc = nlohmann::ordered_json::array();
            for (const auto& c : n.choices) {
                nlohmann::ordered_json o = nlohmann::ordered_json::object();
                o["text"] = c.text;
                o["goto"] = c.gotoId;
                if (!c.writes.empty()) {
                    nlohmann::ordered_json js = nlohmann::ordered_json::array();
                    for (const auto& w : c.writes) {
                        nlohmann::ordered_json wo = nlohmann::ordered_json::object();
                        wo["flag"] = w.flag;
                        wo["value"] = w.value;
                        js.push_back(std::move(wo));
                    }
                    o["set"] = std::move(js);
                }
                jc.push_back(std::move(o));
            }
            jn["choices"] = std::move(jc);
        }
        lines.push_back(std::move(jn));
    }

    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    doc["version"] = 1;
    doc["name"] = a.name;
    if (!a.startId.empty()) doc["start"] = a.startId;
    doc["lines"] = std::move(lines);
    // indent=2 / 空格缩进 / ensure_ascii=false（见 .h 的四条"不写"）
    return doc.dump(2, ' ', false) + "\n";
}

bool saveDialogue(const std::string& path, const DialogueAsset& a,
                  std::string& err) {
    err.clear();
    if (path.empty()) {
        err = "empty path";
        return false;
    }
    const std::string text = toJson(a);
    const std::string full = assets::resolveAssetPath(path);
    const std::string tmp = full + ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open " + tmp + " for writing";
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f.good()) {
            err = "write failed: " + tmp;
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, full, ec);
    if (ec) {
        // 改名失败（目标被占用 / 只读）时把临时文件清掉，不要留一地 .tmp
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        err = "rename to " + full + " failed: " + ec.message();
        return false;
    }

    VK_LOG_INFO("DLG-SAVE path=%s full=%s lines=%d start=%s bytes=%zu",
                path.c_str(), full.c_str(),
                static_cast<int>(a.nodes.size()), a.startId.c_str(),
                text.size());
    return true;
}

} // namespace dialogue
