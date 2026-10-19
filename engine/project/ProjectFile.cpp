#include "project/ProjectFile.h"

// `runAllProjectSelfTests` 要把另外三组的自检函数调起来 —— 只有 .cpp 需要
// 这三个头（头文件仍然互不依赖，`ProjectFile.h` 是它们共同的地基）。
#include "project/ProjectCreate.h"
#include "project/ProjectDisplay.h"
#include "project/RecentList.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace project {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------- 枚举映射表
//
// ⚠ 落盘写的是**字符串 id**，不是下标。下标会随着"往中间加一个类型"整体
//   漂移，老工程读回来会静默变成另一种游戏类型 —— 这与 Inspector 页签
//   "落盘写 id 不写下标"是同一条纪律。

struct GameTypeEntry {
    GameType type;
    const char* id;
    const char* label;
};
const GameTypeEntry kGameTypes[] = {
    {GameType::FirstPerson, "first_person", "First person"},
    {GameType::ThirdPerson, "third_person", "Third person"},
};

struct CodeModeEntry {
    CodeMode mode;
    const char* id;
    const char* label;
};
const CodeModeEntry kCodeModes[] = {
    {CodeMode::Vks, "vks", "VKS scripts"},
    {CodeMode::Cpp, "cpp", "C++ module"},
};

// ---------------------------------------------------------------- JSON 小工具
//
// 读 json 一律"类型不符就保持默认 + 记一条问题"，不抛。理由与
// RuntimeConfig::readConfig 同一条：工程文件是用户能手改的，改错一个字
// 不该让启动器直接崩掉。
void readString(const json& j, const char* key, std::string& dst) {
    if (j.contains(key) && j[key].is_string()) dst = j[key].get<std::string>();
}

// 从 json 还原成 ProjectFile。`issues` 收集"键存在但类型不符"这类可降级问题。
void fromJson(const json& j, ProjectFile& p, std::vector<std::string>& issues) {
    if (j.contains("format") && j["format"].is_number_integer())
        p.format = j["format"].get<int>();
    else
        issues.push_back("missing or non-integer key: format");

    readString(j, "name", p.name);
    readString(j, "template", p.templateId);
    readString(j, "createdAt", p.createdAt);

    std::string gt;
    readString(j, "gameType", gt);
    if (!gt.empty() && !parseGameTypeId(gt, p.gameType))
        issues.push_back("unknown gameType: " + gt);

    readString(j, "startScene", p.startScene);

    if (j.contains("code") && j["code"].is_object()) {
        const json& c = j["code"];
        std::string cm;
        readString(c, "mode", cm);
        if (!cm.empty() && !parseCodeModeId(cm, p.codeMode))
            issues.push_back("unknown code.mode: " + cm);
        readString(c, "root", p.codeRoot);
        if (c.contains("linked") && c["linked"].is_boolean())
            p.codeLinked = c["linked"].get<bool>();
    } else {
        issues.push_back("missing key: code");
    }

    if (j.contains("engine") && j["engine"].is_object()) {
        const json& e = j["engine"];
        readString(e, "name", p.engineName);
        readString(e, "config", p.engineConfig);
    }
}

json toJson(const ProjectFile& p) {
    json j;
    j["format"] = p.format;
    j["name"] = p.name;
    j["template"] = p.templateId;
    j["gameType"] = gameTypeId(p.gameType);
    j["startScene"] = p.startScene;
    j["code"] = json{{"mode", codeModeId(p.codeMode)},
                     {"root", p.codeRoot},
                     {"linked", p.codeLinked}};
    j["createdAt"] = p.createdAt;
    j["engine"] = json{{"name", p.engineName}, {"config", p.engineConfig}};
    return j;
}

// 自检里用的"序列化 → 反序列化"往返。生产路径上 load/save 走文件和这个
// 同一对函数，所以往返测试覆盖的是**真实代码**，不是复制品。
bool roundTripEqual(const ProjectFile& a, const ProjectFile& b,
                    std::string* why) {
    auto eq = [&](const char* field, const std::string& x, const std::string& y) {
        if (x != y) {
            if (why) *why = std::string(field) + ": '" + x + "' != '" + y + "'";
            return false;
        }
        return true;
    };
    if (!eq("name", a.name, b.name)) return false;
    if (!eq("template", a.templateId, b.templateId)) return false;
    if (!eq("startScene", a.startScene, b.startScene)) return false;
    if (!eq("codeRoot", a.codeRoot, b.codeRoot)) return false;
    if (!eq("createdAt", a.createdAt, b.createdAt)) return false;
    if (!eq("engineName", a.engineName, b.engineName)) return false;
    if (!eq("engineConfig", a.engineConfig, b.engineConfig)) return false;
    if (!eq("gameType", gameTypeId(a.gameType), gameTypeId(b.gameType)))
        return false;
    if (!eq("codeMode", codeModeId(a.codeMode), codeModeId(b.codeMode)))
        return false;
    if (a.codeLinked != b.codeLinked) {
        if (why) *why = "codeLinked differs";
        return false;
    }
    if (a.format != b.format) {
        if (why) *why = "format differs";
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------- 枚举

const char* gameTypeId(GameType t) {
    for (const auto& e : kGameTypes)
        if (e.type == t) return e.id;
    return "third_person";
}

const char* gameTypeLabel(GameType t) {
    for (const auto& e : kGameTypes)
        if (e.type == t) return e.label;
    return "Third person";
}

bool parseGameTypeId(const std::string& s, GameType& out) {
    for (const auto& e : kGameTypes) {
        if (s == e.id) {
            out = e.type;
            return true;
        }
    }
    return false;
}

const char* codeModeId(CodeMode m) {
    for (const auto& e : kCodeModes)
        if (e.mode == m) return e.id;
    return "vks";
}

const char* codeModeLabel(CodeMode m) {
    for (const auto& e : kCodeModes)
        if (e.mode == m) return e.label;
    return "VKS scripts";
}

bool parseCodeModeId(const std::string& s, CodeMode& out) {
    for (const auto& e : kCodeModes) {
        if (s == e.id) {
            out = e.mode;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 路径工具

std::string normalizePath(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    fs::path p(path);
    // 只在出错时回退成"纯字符串替换反斜杠" —— 绝不因为"路径不存在"
    // 就返回空串：写 `venn.project.json` 时目标目录可能刚建出来，
    // 但 `normalizePath` 也会用在"还没建"的路径上。
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    std::string s = abs.lexically_normal().generic_string();
    // 去掉尾斜杠（根目录 "/" 除外）
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

bool isAbsolutePath(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
    // Windows 盘符："C:" / "c:/"
    if (path.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
        return true;
    return false;
}

bool isSafeRelative(const std::string& rel) {
    if (rel.empty()) return false;
    if (isAbsolutePath(rel)) return false;
    // 反斜杠一律拒：json 里的路径统一正斜杠，混进反斜杠说明有地方在拼
    // 平台相关的路径，那种串跨机器就会变味。
    if (rel.find('\\') != std::string::npos) return false;
    if (rel.find('\0') != std::string::npos) return false;

    // 逐段检查：不允许 ".."（逃逸）、不允许空段（"a//b" 这种手误）
    std::size_t pos = 0;
    while (pos <= rel.size()) {
        const std::size_t slash = rel.find('/', pos);
        const std::size_t end = (slash == std::string::npos) ? rel.size() : slash;
        const std::string seg = rel.substr(pos, end - pos);
        if (seg.empty()) return false;   // 开头/结尾/连续斜杠
        if (seg == "." || seg == "..") return false;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

std::string sanitizeName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        // Windows 文件名非法字符 + 控制字符一并换成 '_'
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*' || u < 0x20)
            out.push_back('_');
        else
            out.push_back(c);
    }
    // 去首尾空白与点（Windows 上 "abc." 实际落盘会变成 "abc"，而
    // `venn.project.json` 里的 name 还是 "abc." → 显示与目录不一致）
    auto notJunk = [](char c) {
        return !std::isspace(static_cast<unsigned char>(c)) && c != '.';
    };
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), notJunk));
    out.erase(std::find_if(out.rbegin(), out.rend(), notJunk).base(), out.end());

    if (out.size() > 64) {
        // 截断要在 UTF-8 边界上（否则半个汉字 → 目录名在资源管理器里变方块）
        std::size_t cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
            // 截断后再去一次尾部空白
        out.erase(std::find_if(out.rbegin(), out.rend(), notJunk).base(),
                  out.end());
    }
    if (out.empty()) return "Untitled";
    return out;
}

std::string nowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// ---------------------------------------------------------------- 读写

std::string projectFilePath(const std::string& root) {
    if (root.empty()) return kProjectFileName;
    return (fs::path(root) / kProjectFileName).generic_string();
}

IoResult loadProject(const std::string& root, ProjectFile& out, std::string* err) {
    auto fail = [err](const std::string& why) {
        if (err) *err = why;
        return IoResult{false, why};
    };

    if (root.empty()) return fail("Project path is empty");

    std::error_code ec;
    const fs::path dir(root);
    if (!fs::is_directory(dir, ec))
        return fail("Folder does not exist: " + root);

    const std::string path = projectFilePath(root);
    if (!fs::is_regular_file(fs::path(path), ec))
        return fail("Not a Venn project (missing venn.project.json): " + root);

    json j;
    try {
        std::ifstream f(path);
        if (!f) return fail("Cannot open " + path);
        f >> j;
    } catch (const std::exception& e) {
        return fail(std::string("venn.project.json is not valid JSON: ") + e.what());
    }
    if (!j.is_object())
        return fail("venn.project.json must contain a JSON object");

    // ---- 版本检查**放在最前**：高版本文件的键语义可能是新的，
    //      先解析再报版本会给出误导性的"字段缺失"错误。
    int fmt = 0;
    if (j.contains("format") && j["format"].is_number_integer())
        fmt = j["format"].get<int>();
    if (fmt <= 0)
        return fail("venn.project.json has no valid 'format' field");
    if (fmt > kProjectFormat) {
        return fail("This project needs a newer Venn (format " +
                    std::to_string(fmt) + ", this build reads up to " +
                    std::to_string(kProjectFormat) + ")");
    }

    ProjectFile p;
    std::vector<std::string> issues;
    fromJson(j, p, issues);
    for (const auto& s : issues) VK_LOG_WARN("PROJ-WARN %s", s.c_str());

    if (p.name.empty()) return fail("venn.project.json has no 'name'");
    if (!validate(p, err)) return IoResult{false, err ? *err : "invalid project"};

    p.root = normalizePath(root);
    out = p;
    VK_LOG_INFO("PROJ-LOAD name=%s type=%s scene=%s root=%s", p.name.c_str(),
                gameTypeId(p.gameType), p.startScene.c_str(), p.root.c_str());
    return IoResult{true, {}};
}

IoResult saveProject(const ProjectFile& p, std::string* err) {
    if (!validate(p, err)) return IoResult{false, err ? *err : "invalid project"};
    if (p.root.empty()) {
        const std::string why = "ProjectFile::root is empty (caller must set it)";
        if (err) *err = why;
        return IoResult{false, why};
    }

    const std::string path = projectFilePath(p.root);
    std::error_code ec;
    fs::create_directories(fs::path(p.root), ec);
    if (ec) {
        const std::string why = "Cannot create project folder: " + ec.message();
        if (err) *err = why;
        return IoResult{false, why};
    }

    // 先写临时文件再 rename：写一半断电不留半成品（P8 同一条思路）。
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) {
            const std::string why = "Cannot write " + tmp;
            if (err) *err = why;
            return IoResult{false, why};
        }
        f << toJson(p).dump(2) << "\n";
        if (!f.good()) {
            const std::string why = "Write failed: " + tmp;
            if (err) *err = why;
            return IoResult{false, why};
        }
    }
    fs::rename(fs::path(tmp), fs::path(path), ec);
    if (ec) {
        // Windows 上 rename 到已存在的文件会失败 —— 先删再改名。
        fs::remove(fs::path(path), ec);
        ec.clear();
        fs::rename(fs::path(tmp), fs::path(path), ec);
    }
    if (ec) {
        fs::remove(fs::path(tmp), ec);
        const std::string why = "Cannot commit venn.project.json: " + ec.message();
        if (err) *err = why;
        return IoResult{false, why};
    }

    VK_LOG_INFO("PROJ-SAVE name=%s path=%s", p.name.c_str(), path.c_str());
    return IoResult{true, {}};
}

std::string toJsonText(const ProjectFile& p) { return toJson(p).dump(2); }

// ---------------------------------------------------------------- 校验

bool validate(const ProjectFile& p, std::string* err) {
    auto fail = [err](const std::string& why) {
        if (err) *err = why;
        return false;
    };

    if (p.format <= 0 || p.format > kProjectFormat)
        return fail("Unsupported format: " + std::to_string(p.format));
    if (p.name.empty()) return fail("Project name is empty");
    if (!isSafeRelative(p.startScene))
        return fail("startScene must be a safe path inside assets/: '" +
                    p.startScene + "'");

    if (p.codeLinked) {
        // 外置代码根必须绝对：相对路径 + 联接 = "相对谁"说不清，
        // 而 junction 只能指向绝对路径。
        if (!isAbsolutePath(p.codeRoot))
            return fail("code.root must be an absolute path when code.linked "
                        "is true: '" + p.codeRoot + "'");
    } else {
        if (!isSafeRelative(p.codeRoot))
            return fail("code.root must be a safe path inside assets/: '" +
                        p.codeRoot + "'");
    }

    // 只 WARN 不算失败：用户完全可能在编辑器里把类型改掉了。
    if (!p.templateId.empty() && p.templateId != gameTypeId(p.gameType)) {
        VK_LOG_WARN("PROJ-WARN template='%s' but gameType='%s' (=展示用, "
                    "权威是场景里的 player.mode)",
                    p.templateId.c_str(), gameTypeId(p.gameType));
    }
    return true;
}

IoResult checkTargetDirUsable(const std::string& absPath) {
    auto fail = [](const std::string& why) {
        return IoResult{false, why};
    };
    if (absPath.empty()) return fail("Target folder is empty");

    std::error_code ec;
    const fs::path dir(absPath);
    if (!fs::exists(dir, ec)) {
        // 不存在 = 可用（父目录能不能建出来由创建流程负责）
        return IoResult{true, {}};
    }
    if (!fs::is_directory(dir, ec))
        return fail("A file with that name already exists: " + absPath);

    fs::directory_iterator it(dir, ec);
    if (ec) return fail("Cannot read folder: " + absPath);
    if (it != fs::directory_iterator()) {
        // 带出第一个挡路的条目名 —— "非空"这句话用户没法据此动手，
        // 知道是谁在里面才知道该删什么。
        std::string first = it->path().filename().string();
        return fail("Folder is not empty: " + absPath + " (contains '" + first +
                    "'...)");
    }
    return IoResult{true, {}};
}

// ---------------------------------------------------------------- 路径规则

std::string assetRoot(const ProjectFile& p) {
    return (fs::path(p.root) / "assets").generic_string();
}

std::string startSceneAbs(const ProjectFile& p) {
    return (fs::path(assetRoot(p)) / p.startScene).lexically_normal().generic_string();
}

std::string codeRootAbs(const ProjectFile& p) {
    if (isAbsolutePath(p.codeRoot)) return normalizePath(p.codeRoot);
    return (fs::path(assetRoot(p)) / p.codeRoot).lexically_normal().generic_string();
}

std::string scriptsDirAbs(const ProjectFile& p) {
    return (fs::path(assetRoot(p)) / "scripts").lexically_normal().generic_string();
}

// ---------------------------------------------------------------- 自检
//
// 铁律（记忆里的断言纪律）：**每条断言都要能真的变红**。
// 所以这里的"期望失败"用例都成对写 —— 一个合法样本 + 一个只差一点的
// 非法样本，两者必须给出相反的结果。只测"合法通过"等于什么都没测。

int runProjectSelfTest() {
    int pass = 0;
    int fail = 0;

    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            std::printf("PROJ-TEST PASS %s\n", name);
        } else {
            ++fail;
            std::printf("PROJ-TEST FAIL %s (%s)\n", name, detail.c_str());
        }
    };

    // ---- A 组：isSafeRelative 的成对用例 ----
    check(isSafeRelative("scenes/main.json"), "A1 safe-rel-ok", "");
    check(!isSafeRelative("../escape.json"), "A2 safe-rel-dotdot", ".. accepted");
    check(!isSafeRelative("a/../../b.json"), "A3 safe-rel-embedded-dotdot",
          "embedded .. accepted");
    check(!isSafeRelative("/abs.json"), "A4 safe-rel-absolute", "absolute accepted");
    check(!isSafeRelative("C:/x.json"), "A5 safe-rel-drive",
          "drive letter accepted");
    check(!isSafeRelative("a\\b.json"), "A6 safe-rel-backslash",
          "backslash accepted");
    check(!isSafeRelative(""), "A7 safe-rel-empty", "empty accepted");
    check(!isSafeRelative("a//b.json"), "A8 safe-rel-empty-segment",
          "empty segment accepted");
    check(!isSafeRelative("./a.json"), "A9 safe-rel-dot-segment",
          "'.' segment accepted");
    check(isSafeRelative("scripts/ai/patrol.vks"), "A10 safe-rel-nested-ok", "");

    // ---- B 组：isAbsolutePath ----
    check(isAbsolutePath("D:/games/x"), "B1 abs-drive", "");
    check(isAbsolutePath("/home/x"), "B2 abs-posix", "");
    check(!isAbsolutePath("games/x"), "B3 abs-relativ-not", "relative seen as abs");
    check(!isAbsolutePath(""), "B4 abs-empty", "");

    // ---- C 组：sanitizeName ----
    check(sanitizeName("My Game") == "My Game", "C1 name-plain",
          sanitizeName("My Game"));
    check(sanitizeName("a/b:c*d") == "a_b_c_d", "C2 name-illegal",
          sanitizeName("a/b:c*d"));
    check(sanitizeName("  ..trim..  ") == "trim", "C3 name-trim",
          sanitizeName("  ..trim..  "));
    check(sanitizeName("") == "Untitled", "C4 name-empty-fallback",
          sanitizeName(""));
    check(sanitizeName("...").empty() == false &&
              sanitizeName("...") == "Untitled",
          "C5 name-only-dots", sanitizeName("..."));
    {
        // UTF-8 截断必须落在字符边界上：64 个汉字之后再加，结果仍能原样打印。
        std::string many;
        for (int i = 0; i < 40; ++i) many += "汉";
        const std::string s = sanitizeName(many);
        bool boundaryOk = true;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if ((c & 0xC0) != 0x80) continue;          // 非续字节，跳过
            // 续字节前面必须有一个多字节首字节
            if (i == 0) { boundaryOk = false; break; }
        }
        // 更硬的判据：字节数必须是 3 的倍数（全汉字），且小于原长
        boundaryOk = boundaryOk && (s.size() % 3 == 0) && s.size() < many.size() + 1;
        check(boundaryOk, "C6 name-utf8-boundary",
              "size=" + std::to_string(s.size()));
    }

    // ---- D 组：路径规则 ----
    {
        ProjectFile p;
        p.root = "D:/Games/MyGame";
        p.startScene = "scenes/main.json";
        p.codeRoot = "scripts";
        p.codeLinked = false;
        check(assetRoot(p) == "D:/Games/MyGame/assets", "D1 asset-root",
              assetRoot(p));
        check(startSceneAbs(p) == "D:/Games/MyGame/assets/scenes/main.json",
              "D2 start-scene", startSceneAbs(p));
        check(codeRootAbs(p) == "D:/Games/MyGame/assets/scripts", "D3 code-root-rel",
              codeRootAbs(p));

        p.codeLinked = true;
        p.codeRoot = "E:/SharedCode/MyGame";
        check(codeRootAbs(p) == "E:/SharedCode/MyGame", "D4 code-root-abs",
              codeRootAbs(p));
        check(scriptsDirAbs(p) == "D:/Games/MyGame/assets/scripts",
              "D5 scripts-dir", scriptsDirAbs(p));
    }

    // ---- E 组：validate 的成对用例 ----
    {
        ProjectFile good;
        good.name = "MyGame";
        good.startScene = "scenes/main.json";
        good.codeRoot = "scripts";
        good.templateId = "third_person";
        good.gameType = GameType::ThirdPerson;
        std::string err;
        check(validate(good, &err), "E1 validate-good", err);

        ProjectFile bad = good;
        bad.name.clear();
        check(!validate(bad, &err), "E2 validate-empty-name", "empty name passed");

        bad = good;
        bad.startScene = "../../etc/passwd";
        check(!validate(bad, &err), "E3 validate-escape", "escape passed");

        bad = good;
        bad.format = kProjectFormat + 1;
        check(!validate(bad, &err), "E4 validate-future-format",
              "future format passed");

        bad = good;
        bad.codeLinked = true;
        bad.codeRoot = "scripts";   // 相对 + linked = 矛盾
        check(!validate(bad, &err), "E5 validate-linked-relative",
              "relative linked passed");
    }

    // ---- F 组：序列化往返（走的是 load/save 同一对函数）----
    {
        ProjectFile a;
        a.name = "往返工程";
        a.templateId = "first_person";
        a.gameType = GameType::FirstPerson;
        a.startScene = "scenes/main.json";
        a.codeMode = CodeMode::Vks;
        a.codeRoot = "scripts";
        a.codeLinked = false;
        a.createdAt = "2026-10-19T14:00:00Z";
        a.engineName = "venn";
        a.engineConfig = "Debug";

        const std::string text = toJsonText(a);
        ProjectFile b;
        std::vector<std::string> issues;
        fromJson(json::parse(text), b, issues);
        std::string why;
        check(roundTripEqual(a, b, &why), "F1 roundtrip", why);
        check(issues.empty(), "F2 roundtrip-no-issues",
              issues.empty() ? "" : issues.front());

        // 否定对照：只改一个字段，往返必须**不等**
        b.name = "other";
        check(!roundTripEqual(a, b, &why), "F3 roundtrip-negative",
              "difference not detected");

        // 未知键必须被忽略而不是报错（前向兼容：老启动器读新工程）
        json withExtra = json::parse(text);
        withExtra["futureKey"] = 42;
        ProjectFile c;
        std::vector<std::string> issues2;
        fromJson(withExtra, c, issues2);
        check(roundTripEqual(a, c, &why), "F4 roundtrip-unknown-key", why);
    }

    // ---- G 组：冲突检测（checkTargetDirUsable）----
    {
        std::error_code ec;
        const fs::path base =
            fs::temp_directory_path(ec) / "venn_projtest_checkdir";
        fs::remove_all(base, ec);
        fs::create_directories(base, ec);

        // 不存在 → 可用
        check(checkTargetDirUsable((base / "notyet").generic_string()).ok,
              "G1 dir-missing-usable", "");

        // 存在且空 → 可用
        fs::create_directories(base / "empty", ec);
        check(checkTargetDirUsable((base / "empty").generic_string()).ok,
              "G2 dir-empty-usable", "");

        // 存在且非空 → 拒绝，且错误串里带挡路的条目名
        fs::create_directories(base / "full", ec);
        { std::ofstream f((base / "full" / "blocker.txt").string()); f << "x"; }
        const IoResult r =
            checkTargetDirUsable((base / "full").generic_string());
        check(!r.ok && r.error.find("blocker.txt") != std::string::npos,
              "G3 dir-nonempty-rejected", r.ok ? "accepted" : r.error);

        // 同名文件 → 拒绝
        { std::ofstream f((base / "afile").string()); f << "x"; }
        check(!checkTargetDirUsable((base / "afile").generic_string()).ok,
              "G4 path-is-file-rejected", "accepted");

        fs::remove_all(base, ec);
    }

    // ---- H 组：枚举 id 往返 ----
    {
        GameType g = GameType::ThirdPerson;
        check(parseGameTypeId("first_person", g) && g == GameType::FirstPerson,
              "H1 gametype-parse-first", "");
        check(parseGameTypeId("third_person", g) && g == GameType::ThirdPerson,
              "H2 gametype-parse-third", "");
        check(!parseGameTypeId("FourthPerson", g), "H3 gametype-parse-bad",
              "unknown id accepted");
        CodeMode m = CodeMode::Vks;
        check(parseCodeModeId("cpp", m) && m == CodeMode::Cpp,
              "H4 codemode-parse-cpp", "");
        check(!parseCodeModeId("rust", m), "H5 codemode-parse-bad",
              "unknown id accepted");
    }

    std::printf("PROJ-TEST SUMMARY pass=%d fail=%d\n", pass, fail);
    return fail;
}

int runAllProjectSelfTests() {
    // 四组的实现在各自的 TU 里（RecentList / ProjectDisplay / ProjectCreate），
    // 这里只负责**相加**。顺序固定：数据模型 → 最近列表 → 画面设置 → 创建，
    // 从"最底层的数据规则"到"最上层的目录操作"，红在哪一组一眼能看出来。
    int failures = 0;
    failures += runProjectSelfTest();
    failures += runRecentListSelfTest();
    failures += runDisplaySelfTest();
    failures += runCreateSelfTest();
    std::printf("PROJECT-ALL-TOTAL failures=%d\n", failures);
    return failures;
}

} // namespace project
