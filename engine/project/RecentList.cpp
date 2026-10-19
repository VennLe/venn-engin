#include "project/RecentList.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace project {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// 与 runtime/RuntimeLog.cpp 的 baseDir 同一套回退链。刻意不 include
// runtime/（那会破坏 `runtime → engine ← editor` 的方向），所以这里
// 独立写一遍 —— 三条 getenv 的重复远小于"为了复用把依赖反过来"。
std::string userBaseDir() {
    if (const char* p = std::getenv("LOCALAPPDATA"))
        if (p[0]) return std::string(p);
    if (const char* p = std::getenv("APPDATA"))
        if (p[0]) return std::string(p);
    if (const char* p = std::getenv("TEMP"))
        if (p[0]) return std::string(p);
    return ".";
}

// Windows 路径比较：一律小写 + 正斜杠。不做这个的话
// `D:/Games/MyGame` 与 `d:/games/mygame` 会被当成两个工程，列表里出现
// 两条指向同一个目录的条目 —— 用户点哪条都一样，但看起来像两个工程。
std::string pathKey(const std::string& p) {
    std::string s = normalizePath(p);
    for (char& c : s)
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::string recentListPath() {
    if (const char* p = std::getenv("MYVK_LAUNCHER_RECENT"))
        if (p[0]) return normalizePath(p);
    return (fs::path(userBaseDir()) / "Venn" / "launcher" / "recent.json")
        .generic_string();
}

bool RecentList::load(std::string* err) {
    m_items.clear();
    const std::string path = recentListPath();

    std::error_code ec;
    if (!fs::is_regular_file(fs::path(path), ec)) {
        VK_LOG_INFO("RECENT-LOAD empty (no file at %s)", path.c_str());
        return true;   // 没有文件 = 第一次跑，正常
    }

    json j;
    try {
        std::ifstream f(path);
        if (!f) {
            if (err) *err = "Cannot open " + path;
            VK_LOG_WARN("RECENT-LOAD %s", (err ? *err : "cannot open").c_str());
            return false;
        }
        f >> j;
    } catch (const std::exception& e) {
        if (err) *err = std::string("recent.json is not valid JSON: ") + e.what();
        VK_LOG_WARN("RECENT-LOAD bad json (%s)", e.what());
        return false;
    }

    if (!j.is_object() || !j.contains("items") || !j["items"].is_array()) {
        if (err) *err = "recent.json has no 'items' array";
        VK_LOG_WARN("RECENT-LOAD bad shape (%s)", path.c_str());
        return false;
    }

    for (const auto& it : j["items"]) {
        if (!it.is_object()) continue;
        RecentItem r;
        if (it.contains("name") && it["name"].is_string())
            r.name = it["name"].get<std::string>();
        if (it.contains("path") && it["path"].is_string())
            r.path = normalizePath(it["path"].get<std::string>());
        if (it.contains("lastOpenedAt") && it["lastOpenedAt"].is_string())
            r.lastOpenedAt = it["lastOpenedAt"].get<std::string>();
        std::string gt;
        if (it.contains("gameType") && it["gameType"].is_string())
            gt = it["gameType"].get<std::string>();
        parseGameTypeId(gt, r.gameType);   // 认不出就用默认（不算错误）
        if (r.path.empty()) continue;      // 没有路径的条目毫无用处
        if (r.name.empty()) r.name = fs::path(r.path).filename().string();
        m_items.push_back(std::move(r));
        if (static_cast<int>(m_items.size()) >= kRecentMaxItems * 2) break;
    }

    VK_LOG_INFO("RECENT-LOAD count=%zu path=%s", m_items.size(), path.c_str());
    return true;
}

bool RecentList::save(std::string* err) const {
    const std::string path = recentListPath();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        if (err) *err = "Cannot create " + fs::path(path).parent_path().string() +
                        ": " + ec.message();
        VK_LOG_WARN("RECENT-SAVE %s", err ? err->c_str() : "mkdir failed");
        return false;
    }

    json j;
    j["format"] = 1;
    json arr = json::array();
    for (const auto& r : m_items) {
        arr.push_back(json{{"name", r.name},
                           {"path", r.path},
                           {"gameType", gameTypeId(r.gameType)},
                           {"lastOpenedAt", r.lastOpenedAt}});
    }
    j["items"] = std::move(arr);

    // 先写临时文件再 rename（与 ProjectFile::saveProject 同一条）。
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) {
            if (err) *err = "Cannot write " + tmp;
            VK_LOG_WARN("RECENT-SAVE %s", err ? err->c_str() : "open failed");
            return false;
        }
        f << j.dump(2) << "\n";
        if (!f.good()) {
            if (err) *err = "Write failed: " + tmp;
            VK_LOG_WARN("RECENT-SAVE write failed");
            return false;
        }
    }
    fs::rename(fs::path(tmp), fs::path(path), ec);
    if (ec) {
        fs::remove(fs::path(path), ec);
        ec.clear();
        fs::rename(fs::path(tmp), fs::path(path), ec);
    }
    if (ec) {
        fs::remove(fs::path(tmp), ec);
        if (err) *err = "Cannot commit recent.json: " + ec.message();
        VK_LOG_WARN("RECENT-SAVE commit failed: %s", ec.message().c_str());
        return false;
    }

    VK_LOG_INFO("RECENT-SAVE count=%zu path=%s", m_items.size(), path.c_str());
    return true;
}

void RecentList::touch(const ProjectFile& p) {
    const std::string key = pathKey(p.root);
    RecentItem item;
    item.name = p.name.empty() ? fs::path(p.root).filename().string() : p.name;
    item.path = normalizePath(p.root);
    item.gameType = p.gameType;
    item.lastOpenedAt = nowIso8601();

    // 同路径的旧条目删掉（"移到最前"而不是"加一条"）
    m_items.erase(
        std::remove_if(m_items.begin(), m_items.end(),
                       [&](const RecentItem& r) { return pathKey(r.path) == key; }),
        m_items.end());
    m_items.insert(m_items.begin(), std::move(item));
    if (static_cast<int>(m_items.size()) > kRecentMaxItems)
        m_items.resize(static_cast<std::size_t>(kRecentMaxItems));
}

bool RecentList::remove(const std::string& path) {
    const std::string key = pathKey(path);
    const std::size_t before = m_items.size();
    m_items.erase(
        std::remove_if(m_items.begin(), m_items.end(),
                       [&](const RecentItem& r) { return pathKey(r.path) == key; }),
        m_items.end());
    const bool removed = m_items.size() != before;
    if (removed) VK_LOG_INFO("RECENT-REMOVE path=%s", path.c_str());
    return removed;
}

int RecentList::pruneMissing() {
    const std::size_t before = m_items.size();
    m_items.erase(
        std::remove_if(m_items.begin(), m_items.end(),
                       [](const RecentItem& r) {
                           std::error_code ec;
                           if (!fs::is_regular_file(
                                   fs::path(projectFilePath(r.path)), ec))
                               return true;
                           return false;
                       }),
        m_items.end());
    const int n = static_cast<int>(before - m_items.size());
    if (n > 0) VK_LOG_INFO("RECENT-PRUNE removed=%d left=%zu", n, m_items.size());
    return n;
}

// ---------------------------------------------------------------- 自检

int runRecentListSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            std::printf("RECENT-TEST PASS %s\n", name);
        } else {
            ++fail;
            std::printf("RECENT-TEST FAIL %s (%s)\n", name, detail.c_str());
        }
    };

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "venn_recent_test";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    // ⚠ 用 env 把落盘重定向到临时目录 —— 自检绝不能碰用户真实的最近列表。
    const std::string savedPath = recentListPath();
#if defined(_WIN32)
    _putenv_s("MYVK_LAUNCHER_RECENT",
              (base / "recent.json").string().c_str());
#else
    setenv("MYVK_LAUNCHER_RECENT", (base / "recent.json").c_str(), 1);
#endif

    auto mkProject = [&](const char* name, GameType t) {
        ProjectFile p;
        p.name = name;
        p.root = (base / name).generic_string();
        p.gameType = t;
        p.startScene = "scenes/main.json";
        p.codeRoot = "scripts";
        std::error_code e2;
        fs::create_directories(p.root, e2);
        return p;
    };

    // ---- I1 空列表（没有文件）----
    {
        RecentList r;
        std::string err;
        check(r.load(&err) && r.empty(), "I1 load-missing-is-empty", err);
    }

    // ---- I2 touch + 往返 ----
    {
        RecentList r;
        r.load();
        ProjectFile a = mkProject("Alpha", GameType::FirstPerson);
        r.touch(a);
        std::string err;
        check(r.save(&err), "I2 save", err);

        RecentList r2;
        r2.load(&err);
        check(r2.size() == 1 && r2.items()[0].name == "Alpha",
              "I3 reload-count", std::to_string(r2.size()));
        check(!r2.items().empty() &&
                  r2.items()[0].gameType == GameType::FirstPerson,
              "I4 reload-gametype", "");
        check(!r2.items().empty() && !r2.items()[0].lastOpenedAt.empty(),
              "I5 reload-timestamp", "");
    }

    // ---- I6 去重 + 移到最前 ----
    {
        RecentList r;
        ProjectFile a = mkProject("Alpha", GameType::FirstPerson);
        ProjectFile b = mkProject("Beta", GameType::ThirdPerson);
        r.touch(a);
        r.touch(b);
        r.touch(a);   // 再打开一次 Alpha → 应该到最前且只有一条
        bool ok = r.size() == 2 && r.items()[0].name == "Alpha" &&
                  r.items()[1].name == "Beta";
        check(ok, "I6 touch-dedup", "size=" + std::to_string(r.size()));
    }

    // ---- I7 大小写不敏感去重 ----
    {
        RecentList r;
        ProjectFile a = mkProject("CaseTest", GameType::ThirdPerson);
        r.touch(a);
        ProjectFile a2 = a;
        std::string up = a2.root;
        for (char& c : up) c = static_cast<char>(std::toupper(
                              static_cast<unsigned char>(c)));
        a2.root = up;
        r.touch(a2);
        check(r.size() == 1, "I7 touch-case-insensitive",
              "size=" + std::to_string(r.size()));
    }

    // ---- I8 上限 ----
    {
        RecentList r;
        for (int i = 0; i < kRecentMaxItems + 5; ++i) {
            ProjectFile p = mkProject(("Cap" + std::to_string(i)).c_str(),
                                      GameType::ThirdPerson);
            r.touch(p);
        }
        check(static_cast<int>(r.size()) == kRecentMaxItems, "I8 cap",
              "size=" + std::to_string(r.size()));
        // 最新的在最前
        check(!r.items().empty() &&
                  r.items()[0].name ==
                      "Cap" + std::to_string(kRecentMaxItems + 4),
              "I9 newest-first", r.empty() ? "" : r.items()[0].name);
    }

    // ---- I10 死路径剔除 ----
    {
        RecentList r;
        ProjectFile dead = mkProject("Dead", GameType::ThirdPerson);
        ProjectFile alive = mkProject("Alive", GameType::ThirdPerson);
        // alive 需要真的有 venn.project.json 才算"活着"
        std::string err;
        ProjectFile aliveFile = alive;
        aliveFile.templateId = "third_person";
        saveProject(aliveFile, &err);
        r.touch(dead);
        r.touch(alive);
        const int n = r.pruneMissing();
        check(n == 1 && r.size() == 1 && r.items()[0].name == "Alive",
              "I10 prune-missing", "removed=" + std::to_string(n));
    }

    // ---- I11 remove ----
    {
        RecentList r;
        ProjectFile a = mkProject("ToRemove", GameType::ThirdPerson);
        r.touch(a);
        check(r.remove(a.root) && r.empty(), "I11 remove", "");
        check(!r.remove(a.root), "I12 remove-absent-is-false", "");
    }

    // ---- I13 坏文件不崩 ----
    {
        {
            std::ofstream f((base / "recent.json").string());
            f << "{ this is not json";
        }
        RecentList r;
        std::string err;
        const bool ok = r.load(&err);
        check(!ok && r.empty(), "I13 bad-json-no-crash",
              ok ? "reported ok" : err);
    }

    // ---- I14 顶层形状不对 ----
    {
        {
            std::ofstream f((base / "recent.json").string());
            f << "[1,2,3]";
        }
        RecentList r;
        std::string err;
        check(!r.load(&err) && r.empty(), "I14 bad-shape-no-crash", "");
    }

    // 恢复 env（同一进程里后续用例可能还要读默认路径）
#if defined(_WIN32)
    _putenv_s("MYVK_LAUNCHER_RECENT", "");
#else
    unsetenv("MYVK_LAUNCHER_RECENT");
#endif
    (void)savedPath;
    fs::remove_all(base, ec);

    std::printf("RECENT-TEST SUMMARY pass=%d fail=%d\n", pass, fail);
    return fail;
}

} // namespace project
