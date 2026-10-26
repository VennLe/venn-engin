// ============================================================
// dialogue/DialogueAsset.cpp
// ============================================================

#include "dialogue/DialogueAsset.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <sstream>

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

int revealedChars(float elapsed, float cps, int total) {
    if (total <= 0) return 0;
    // ⚠ `cps <= 0` = **关掉打字机**（立刻全出），不是"永远显示 0 个字"。
    if (!(cps > 0.0f)) return total;
    if (!(elapsed > 0.0f)) return 0;
    const float n = std::floor(elapsed * cps);
    if (n >= static_cast<float>(total)) return total;
    return static_cast<int>(n);
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
        for (const auto& c : n.choices) {
            if (findNode(out, c.gotoId) < 0) {
                out.error = "line \"" + n.id + "\": choice \"" + c.text +
                            "\" goto \"" + c.gotoId + "\" is not a line id";
                return false;
            }
        }
    }

    out.valid = true;
    VK_LOG_INFO("DLG-ASSET name=%s lines=%d start=%s warns=%d",
                out.name.c_str(), static_cast<int>(out.nodes.size()),
                out.startId.c_str(), static_cast<int>(out.warnings.size()));
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

} // namespace dialogue
