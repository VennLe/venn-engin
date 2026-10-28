#include "EditorLayoutIO.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>

namespace editor {

namespace {
using json = nlohmann::json;

constexpr const char* kDefaultPath = "config/editor_layout.json";
// 2026-10-21 v1 → v2：分栏树的**节点语义**换了（左栏从 Hierarchy/Content
// 变成模式栏；右栏变成 Hierarchy 上下 Inspector）。三个节点的 ratio 数组
// 长度没变，但 v1 存档里的数字放到新树上会得到一套四不像的布局（左栏 50%
// 宽、视口挤成一条）—— 所以版本不符直接当"没有存档"，回出厂布局。
// 代价只有一次：用户自己拖过的比例丢掉一次。
constexpr int kFormatVersion = 2;

// 偏好存档**单独**的 schema 版本（布局继续用 kFormatVersion，互不牵连）。
// 为什么要它：`editor_prefs.json` 里的键会**逐键覆盖**出厂默认值，所以只要
// 出厂默认变过一次，老存档就会把新默认"顶掉" —— 用户看到的就是"你改了个
// 寂寞"（2026-10-17 实况：存档里 snap.move=0 / grid.extent=60 把"吸附默认
// 全开"和"栅格放大到 500m"两条改动全盖没了）。加一版 → 老存档不再认，
// 出厂默认生效一次；用户随后改的任何值照常落盘。
constexpr int kPrefsVersion = 2;
} // namespace

bool editorLayoutAutomationRun() {
    return std::getenv("MYVK_FRAMES") != nullptr ||
           std::getenv("MYVK_NO_WINDOW_SAVE") != nullptr;
}

// MYVK_EDITOR_LAYOUT 是否被**显式**指定（非空）。
bool editorLayoutPathExplicit() {
    const char* p = std::getenv("MYVK_EDITOR_LAYOUT");
    return p != nullptr && p[0] != '\0';
}

std::string editorLayoutPath() {
    if (const char* p = std::getenv("MYVK_EDITOR_LAYOUT")) {
        if (p[0] != '\0') return p;
    }
    return kDefaultPath;
}

bool loadEditorLayout(LayoutState& out) {
    // 规则 1：自动化运行不读**默认路径**。
    //   理由：上一次手动会话留下的布局会把自动化跑出的画面改掉，像素级
    //   对照（回归）就不再成立。
    //   但 MYVK_EDITOR_LAYOUT 是**显式**指定的路径 —— 那是自动化脚本自己
    //   要求读的（比如验"落盘往返"），不是残留状态，所以放行。没有这一条
    //   落盘功能在自动化里就没法验证，只能靠人开窗口手点。
    if (editorLayoutAutomationRun() && !editorLayoutPathExplicit()) return false;

    const std::string path = editorLayoutPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        VK_LOG_WARN("Editor layout: '%s' 解析失败（%s），改用出厂布局", path.c_str(),
                    e.what());
        return false;
    }
    if (!j.is_object()) return false;

    // 版本不符 = 老语义的存档，当没有（见 kFormatVersion 的注释）
    if (j.value("version", 0) != kFormatVersion) {
        VK_LOG_INFO("Editor layout: '%s' 版本 %d ≠ %d，改用出厂布局",
                    path.c_str(), j.value("version", 0), kFormatVersion);
        return false;
    }

    LayoutState s;

    // ---- panels ----
    if (j.contains("panels") && j["panels"].is_array()) {
        for (const auto& it : j["panels"]) {
            if (!it.is_object()) continue;
            const auto idIt = it.find("id");
            if (idIt == it.end() || !idIt->is_string()) continue;
            s.panels.emplace_back(idIt->get<std::string>(),
                                  it.value("open", true));
        }
    }

    // ---- workspace ----
    s.workspace = j.value("workspace", std::string("level"));

    // ---- split ----
    if (j.contains("split") && j["split"].is_array()) {
        const auto& arr = j["split"];
        const int n = static_cast<int>(arr.size());
        for (int i = 0; i < kLayoutSplitNodes && i < n; ++i) {
            const json& n0 = arr[static_cast<size_t>(i)];
            if (!n0.is_object()) continue;
            s.splitRatio[i] = n0.value("ratio", 0.0f);
            s.splitCollapsed[i] = n0.value("collapsed", false);
            s.splitSaved[i] = n0.value("saved", 0.30f);
        }
    }

    // ---- slots（E29-1：叶子 → 面板）----
    // 形状：{"slots": [{"a": ["modes"], "b": []}, ...]}，数组长度 = 节点数。
    // 缺这一段 = 老文件 = 保持默认树（**不覆盖**，所以读回来与改造前一致）。
    if (j.contains("slots") && j["slots"].is_array()) {
        const auto& arr = j["slots"];
        const int n = static_cast<int>(arr.size());
        for (int i = 0; i < kLayoutSplitNodes && i < n; ++i) {
            const json& n0 = arr[static_cast<size_t>(i)];
            if (!n0.is_object()) continue;
            for (int side = 0; side < 2; ++side) {
                const char* key = (side == 0) ? "a" : "b";
                if (!n0.contains(key) || !n0[key].is_array()) continue;
                for (const auto& it : n0[key]) {
                    if (it.is_string()) {
                        s.slotGroups[i][side].push_back(it.get<std::string>());
                    }
                }
            }
        }
    }
    s.viewportLayout = j.value("viewportLayout", 0);
    s.viewMode = j.value("viewMode", 0);
    s.maximized = j.value("maximized", std::string(""));
    s.inspTab = j.value("inspTab", std::string("object"));

    if (j.contains("recent") && j["recent"].is_array()) {
        for (const auto& it : j["recent"]) {
            if (it.is_string()) s.recent.push_back(it.get<std::string>());
        }
    }

    // 防御：比例非法（NaN / 负 / >1）时退回默认，不让坏文件把布局搞成 0 宽
    for (int i = 0; i < kLayoutSplitNodes; ++i) {
        const float r = s.splitRatio[i];
        if (!(r >= 0.0f && r <= 1.0f)) s.splitRatio[i] = 0.0f;
        const float v = s.splitSaved[i];
        if (!(v > 0.0f && v <= 1.0f)) s.splitSaved[i] = 0.30f;
    }

    s.valid = true;
    out = std::move(s);
    VK_LOG_INFO("Editor layout: loaded '%s' (workspace=%s, panels=%zu)",
                path.c_str(), out.workspace.c_str(), out.panels.size());
    return true;
}

bool saveEditorLayout(const LayoutState& s) {
    // 规则 1：自动化运行不写（免得把测试跑出来的布局存成"用户的"）。
    // 与读不同，这里**显式路径也不放行** —— 脚本没有任何正当理由借自动化
    // 会话去覆写一个布局文件；要生成存档就用正常窗口跑一次。
    if (editorLayoutAutomationRun()) return false;

    json j;
    j["version"] = kFormatVersion;

    json panels = json::array();
    for (const auto& kv : s.panels) {
        panels.push_back(json{{"id", kv.first}, {"open", kv.second}});
    }
    j["panels"] = std::move(panels);

    j["workspace"] = s.workspace;

    json split = json::array();
    for (int i = 0; i < kLayoutSplitNodes; ++i) {
        split.push_back(json{{"ratio", s.splitRatio[i]},
                             {"collapsed", s.splitCollapsed[i]},
                             {"saved", s.splitSaved[i]}});
    }
    j["split"] = std::move(split);

    // 叶子映射（E29-1）。**只在非空时写** —— 默认树不落盘，这样"用户没动过
    // 布局"的文件与改造前逐字节相同，老读者的 diff 是零。
    {
        bool any = false;
        for (int i = 0; i < kLayoutSplitNodes && !any; ++i)
            any = !s.slotGroups[i][0].empty() || !s.slotGroups[i][1].empty();
        if (any) {
            json slots = json::array();
            for (int i = 0; i < kLayoutSplitNodes; ++i) {
                slots.push_back(json{{"a", s.slotGroups[i][0]},
                                     {"b", s.slotGroups[i][1]}});
            }
            j["slots"] = std::move(slots);
        }
    }
    j["viewportLayout"] = s.viewportLayout;
    j["viewMode"] = s.viewMode;
    j["maximized"] = s.maximized;
    j["inspTab"] = s.inspTab;
    j["recent"] = s.recent;

    const std::string path = editorLayoutPath();
    std::error_code ec;

    const std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            VK_LOG_WARN("Editor layout: 建目录失败 '%s': %s",
                        p.parent_path().string().c_str(), ec.message().c_str());
            return false;
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        VK_LOG_WARN("Editor layout: 打不开写入目标 '%s'", path.c_str());
        return false;
    }
    out << j.dump(2) << "\n";
    if (!out) {
        VK_LOG_WARN("Editor layout: 写入失败 '%s'", path.c_str());
        return false;
    }
    VK_LOG_INFO("Editor layout: saved '%s' (workspace=%s)", path.c_str(),
                s.workspace.c_str());
    return true;
}

// ============================================================
// E17：偏好落盘
// ============================================================

std::string editorPrefsPath() {
    if (const char* p = std::getenv("MYVK_EDITOR_PREFS_PATH")) {
        if (p[0] != '\0') return p;
    }
    return "config/editor_prefs.json";
}

bool saveEditorPrefs(
    const std::vector<std::pair<std::string, double>>& kv) {
    if (editorLayoutAutomationRun()) return false;   // 写全程禁止
    json j;
    j["version"] = kPrefsVersion;
    json prefs = json::object();
    for (const auto& it : kv) prefs[it.first] = it.second;
    j["prefs"] = prefs;

    const std::string path = editorPrefsPath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        VK_LOG_WARN("Editor prefs: 打不开写入目标 '%s'", path.c_str());
        return false;
    }
    out << j.dump(2) << "\n";
    if (!out) {
        VK_LOG_WARN("Editor prefs: 写入失败 '%s'", path.c_str());
        return false;
    }
    VK_LOG_INFO("Editor prefs: saved '%s' (%zu keys)", path.c_str(),
                kv.size());
    return true;
}

bool loadEditorPrefs(std::vector<std::pair<std::string, double>>& out) {
    // 显式路径 → 读放行（与布局同一条规则）
    const bool explicitPath = std::getenv("MYVK_EDITOR_PREFS_PATH") != nullptr;
    if (editorLayoutAutomationRun() && !explicitPath) return false;

    const std::string path = editorPrefsPath();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;   // 没有存档 = 第一次启动，保持出厂值

    try {
        json j = json::parse(in);
        if (!j.is_object() || !j.contains("prefs") ||
            !j["prefs"].is_object())
            return false;
        // 老 schema（或无版本号）→ 整份作废，回到出厂默认（见 kPrefsVersion
        // 的注释）。**不能只删几个键** —— 出厂默认一变，旧值的语义就不可信了。
        if (!j.contains("version") || !j["version"].is_number() ||
            j["version"].get<int>() < kPrefsVersion) {
            const std::string seen =
                j.contains("version") ? j["version"].dump() : std::string("none");
            VK_LOG_INFO("Editor prefs: '%s' 是旧 schema（version=%s，需要 %d）"
                        "→ 作废，改用出厂偏好",
                        path.c_str(), seen.c_str(), kPrefsVersion);
            return false;
        }
        out.clear();
        for (auto it = j["prefs"].begin(); it != j["prefs"].end(); ++it) {
            if (it.value().is_number())
                out.emplace_back(it.key(), it.value().get<double>());
        }
        VK_LOG_INFO("Editor prefs: loaded '%s' (%zu keys)", path.c_str(),
                    out.size());
        return true;
    } catch (const std::exception& e) {
        VK_LOG_WARN("Editor prefs: '%s' 解析失败（%s），改用出厂偏好",
                    path.c_str(), e.what());
        return false;
    }
}

} // namespace editor
