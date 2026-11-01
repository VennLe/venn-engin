// ============================================================
// animation/ExpressionPreset.cpp
// ============================================================

#include "animation/ExpressionPreset.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace anim {

namespace {

using ojson = nlohmann::ordered_json;

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string s = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t expr = s.rfind(".expr.json");
    if (expr != std::string::npos) return s.substr(0, expr);
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

// ---------------------------------------------------------------- 查询

int ExpressionSet::find(const std::string& presetName) const {
    if (presetName.empty()) return -1;
    for (std::size_t i = 0; i < presets.size(); ++i)
        if (presets[i].name == presetName) return static_cast<int>(i);
    return -1;
}

const ExpressionPreset* ExpressionSet::preset(const std::string& n) const {
    const int i = find(n);
    return (i < 0) ? nullptr : &presets[static_cast<std::size_t>(i)];
}

// ---------------------------------------------------------------- 解析

bool parseExpressionSet(const std::string& text, const std::string& tag,
                        ExpressionSet& out) {
    out = ExpressionSet{};
    out.name = baseName(tag);

    ojson doc;
    try {
        doc = ojson::parse(text);
    } catch (const std::exception& e) {
        out.error = std::string("json: ") + e.what();
        return false;
    }
    if (!doc.is_object()) {
        out.error = "root is not an object";
        return false;
    }

    // version：老资产没有这个键时**不报错**（本功能之前不存在 .expr.json，
    // 所以"缺 version"只可能来自手写文件），但要报一条警告，否则以后想
    // 做 v2 迁移时没法区分"作者写的是 v1"和"作者没写"。
    const auto vit = doc.find("version");
    if (vit == doc.end()) {
        out.warnings.push_back("no 'version' key — assuming 1");
    } else {
        const int v = vit->is_number_integer() ? vit->get<int>() : -1;
        if (v != 1) {
            out.error = "unsupported version " +
                        (vit->is_number_integer() ? std::to_string(v)
                                                  : std::string("(not int)"));
            return false;
        }
    }

    if (doc.contains("name") && doc["name"].is_string())
        out.name = doc["name"].get<std::string>();

    const auto pit = doc.find("presets");
    if (pit == doc.end() || !pit->is_array()) {
        out.error = "'presets' missing or not an array";
        return false;
    }

    for (std::size_t i = 0; i < pit->size(); ++i) {
        const ojson& jp = (*pit)[i];
        const std::string where = "preset #" + std::to_string(i);
        if (!jp.is_object()) {
            out.warnings.push_back(where + " is not an object — skipped");
            continue;
        }
        ExpressionPreset p;
        if (jp.contains("name") && jp["name"].is_string())
            p.name = jp["name"].get<std::string>();
        if (p.name.empty()) {
            out.warnings.push_back(where + " has no name — skipped");
            continue;
        }
        if (jp.contains("label") && jp["label"].is_string())
            p.label = jp["label"].get<std::string>();

        // 重复名：**先到的赢**，后来的整条跳过。
        // 不能"后来的覆盖前面的" —— 那会让文件里靠后的一条悄悄生效，
        // 而作者改的是靠前那条（两份都看得见，改错了不报错）。
        if (out.find(p.name) >= 0) {
            out.warnings.push_back("duplicate preset name '" + p.name +
                                   "' (preset #" + std::to_string(i) +
                                   ") — ignored");
            continue;
        }

        const auto wit = jp.find("weights");
        if (wit == jp.end() || !wit->is_object()) {
            out.warnings.push_back("preset '" + p.name +
                                   "' has no 'weights' object — skipped");
            continue;
        }
        for (auto it = wit->begin(); it != wit->end(); ++it) {
            ExprWeight w;
            w.channel = it.key();
            if (!it.value().is_number()) {
                out.warnings.push_back("preset '" + p.name + "' channel '" +
                                       w.channel + "' is not a number — skipped");
                continue;
            }
            float v = it.value().get<float>();
            if (!std::isfinite(v)) {
                out.warnings.push_back("preset '" + p.name + "' channel '" +
                                       w.channel + "' is not finite — skipped");
                continue;
            }
            // 见文件头第 ③ 条：钳位但**不静默**。
            if (v < 0.0f) {
                out.warnings.push_back("preset '" + p.name + "' channel '" +
                                       w.channel + "' = " +
                                       std::to_string(v) + " < 0 — clamped");
                v = 0.0f;
            } else if (v > 1.0f) {
                out.warnings.push_back("preset '" + p.name + "' channel '" +
                                       w.channel + "' = " +
                                       std::to_string(v) + " > 1 — clamped");
                v = 1.0f;
            }
            if (w.channel.empty()) continue;
            w.value = v;
            p.weights.push_back(std::move(w));
        }
        if (p.weights.empty()) {
            out.warnings.push_back("preset '" + p.name +
                                   "' has no usable weight — skipped");
            continue;
        }
        out.presets.push_back(std::move(p));
    }

    if (out.presets.empty()) {
        out.error = "no usable presets";
        return false;
    }

    out.valid = true;
    // 一条 INFO 交代"读到什么"，避免只有警告时看不出总数。
    VK_LOG_INFO("EXPR-ASSET name=%s presets=%d warns=%d",
                out.name.c_str(), static_cast<int>(out.presets.size()),
                static_cast<int>(out.warnings.size()));
    for (const std::string& w : out.warnings) VK_LOG_WARN("EXPR-WARN %s", w.c_str());
    return true;
}

bool loadExpressionSet(const std::string& path, ExpressionSet& out) {
    const std::string full = assets::resolveAssetPath(path);
    std::string text;
    if (!readFile(full, text)) {
        out = ExpressionSet{};
        out.name = baseName(path);
        // 绝对路径进 error：只写相对路径的话"文件到底去哪找"永远是个谜。
        out.error = "cannot read " + full;
        return false;
    }
    return parseExpressionSet(text, path, out);
}

// ---------------------------------------------------------------- 写回

std::string toJson(const ExpressionSet& s) {
    ojson doc = ojson::object();
    doc["version"] = 1;
    doc["name"] = s.name;
    ojson arr = ojson::array();
    for (const ExpressionPreset& p : s.presets) {
        ojson jp = ojson::object();
        jp["name"] = p.name;
        // label 为空时**不写**：老文件存回去不许长出 `"label": ""`。
        if (!p.label.empty()) jp["label"] = p.label;
        ojson jw = ojson::object();
        for (const ExprWeight& w : p.weights) jw[w.channel] = w.value;
        jp["weights"] = std::move(jw);
        arr.push_back(std::move(jp));
    }
    doc["presets"] = std::move(arr);
    return doc.dump(2, ' ', false) + "\n";
}

bool saveExpressionSet(const std::string& path, const ExpressionSet& s,
                       std::string& err) {
    err.clear();
    if (path.empty()) {
        err = "empty path";
        return false;
    }
    const std::string text = toJson(s);
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
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- rig 适配度

void checkAgainstRig(const ExpressionSet& s,
                     const std::vector<std::string>& rigChannels,
                     std::vector<std::string>& missing) {
    missing.clear();
    for (const ExpressionPreset& p : s.presets) {
        for (const ExprWeight& w : p.weights) {
            if (std::find(rigChannels.begin(), rigChannels.end(), w.channel) !=
                rigChannels.end())
                continue;
            if (std::find(missing.begin(), missing.end(), w.channel) ==
                missing.end())
                missing.push_back(w.channel);
        }
    }
}

} // namespace anim
