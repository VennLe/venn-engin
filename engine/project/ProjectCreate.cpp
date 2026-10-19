#include "project/ProjectCreate.h"

#include "project/RecentList.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace project {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// 从可执行文件所在目录（`AssetPath.cpp` 同一个取法，但不 include 它 ——
// 那是 assets 层，project 层不该依赖）。
std::string exeDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    if (n == 0 || n >= MAX_PATH * 4) return {};
    // 宽字符 → UTF-8（中文安装路径必须能走通）
    const int need = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr,
                                         nullptr);
    if (need <= 0) return {};
    std::string s(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, s.data(), need, nullptr, nullptr);
    return fs::path(s).parent_path().generic_string();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path().generic_string();
#endif
}

// 文本文件判定：不含 NUL 字节就按文本处理。
// ⚠ 为什么不按扩展名白名单：模板里将来可能加 `.md` / `.ini` / `.h`，
//   一个白名单漏了新扩展名 = 占位符静默不替换（工程名还是 `__PROJECT_NAME__`）。
//   而"不含 NUL"是个不会漏的判据 —— 二进制资产几乎必然含 NUL。
bool looksLikeText(const fs::path& p, std::string& textOut) {
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec || sz > 4u * 1024u * 1024u) return false;   // 模板不该有超大文件
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    if (s.find('\0') != std::string::npos) return false;
    textOut = std::move(s);
    return true;
}

// 宽字符命令行（中文路径 / 中文用户名必须走这条路）
bool runCommandW(const std::wstring& cmdline, std::string* err) {
#if defined(_WIN32)
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline;   // CreateProcessW 要求可写缓冲
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (err) *err = "CreateProcessW failed (" +
                        std::to_string(static_cast<long>(GetLastError())) + ")";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) {
        if (err) *err = "command exited with " + std::to_string(code);
        return false;
    }
    return true;
#else
    (void)cmdline;
    if (err) *err = "not implemented on this platform";
    return false;
#endif
}

std::wstring utf8ToWide(const std::string& s) {
#if defined(_WIN32)
    if (s.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 0) return {};
    std::wstring w(static_cast<std::size_t>(need - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), need);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

} // namespace

const char* createStepLabel(CreateStep s) {
    switch (s) {
    case CreateStep::Validate:      return "Validate";
    case CreateStep::CopyTemplate:  return "Copy template";
    case CreateStep::ReplaceAndWrite: return "Write project files";
    case CreateStep::Commit:        return "Commit";
    case CreateStep::Count:         break;
    }
    return "?";
}

// ---------------------------------------------------------------- 模板查找

std::string templatesRoot() {
    std::vector<std::string> cands;
    if (const char* env = std::getenv("MYVK_TEMPLATE_DIR"))
        if (env[0]) cands.push_back(normalizePath(env));

    const std::string ed = exeDir();
    if (!ed.empty()) cands.push_back((fs::path(ed) / "templates").generic_string());

    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (!ec) cands.push_back((cwd / "templates").generic_string());

    for (const auto& c : cands) {
        std::error_code e2;
        if (fs::is_directory(fs::path(c), e2)) return c;
    }
    return cands.empty() ? std::string("templates") : cands.front();
}

std::string templateDir(const std::string& templateId) {
    if (templateId.empty()) return {};
    const std::string root = templatesRoot();
    const std::string dir = (fs::path(root) / templateId).generic_string();
    std::error_code ec;
    if (fs::is_regular_file(fs::path(dir) / "template.json", ec)) return dir;
    return {};
}

IoResult loadTemplateManifest(const std::string& templateId,
                              TemplateManifest& out, std::string* err) {
    auto fail = [err](const std::string& why) {
        if (err) *err = why;
        return IoResult{false, why};
    };

    const std::string dir = templateDir(templateId);
    if (dir.empty())
        return fail("Template '" + templateId + "' not found under " +
                    templatesRoot());

    json j;
    try {
        std::ifstream f((fs::path(dir) / "template.json").string());
        if (!f) return fail("Cannot open " + dir + "/template.json");
        f >> j;
    } catch (const std::exception& e) {
        return fail(std::string("template.json is not valid JSON: ") + e.what());
    }
    if (!j.is_object()) return fail("template.json must be a JSON object");

    TemplateManifest m;
    m.dir = dir;
    if (j.contains("format") && j["format"].is_number_integer())
        m.format = j["format"].get<int>();
    if (j.contains("id") && j["id"].is_string()) m.id = j["id"].get<std::string>();
    if (j.contains("label") && j["label"].is_string())
        m.label = j["label"].get<std::string>();
    if (j.contains("summary") && j["summary"].is_string())
        m.summary = j["summary"].get<std::string>();
    if (j.contains("startScene") && j["startScene"].is_string())
        m.startScene = j["startScene"].get<std::string>();

    std::string gt;
    if (j.contains("gameType") && j["gameType"].is_string())
        gt = j["gameType"].get<std::string>();
    if (!gt.empty() && !parseGameTypeId(gt, m.gameType))
        VK_LOG_WARN("TEMPLATE unknown gameType '%s' (using third_person)",
                    gt.c_str());

    if (m.id.empty()) m.id = templateId;

    if (j.contains("placeholders") && j["placeholders"].is_array()) {
        for (const auto& p : j["placeholders"])
            if (p.is_string()) m.placeholders.push_back(p.get<std::string>());
    }
    if (j.contains("files") && j["files"].is_array()) {
        for (const auto& p : j["files"])
            if (p.is_string()) m.files.push_back(p.get<std::string>());
    }

    out = m;
    return IoResult{true, {}};
}

// ---------------------------------------------------------------- junction

bool isDirectoryLink(const std::string& path) {
    std::error_code ec;
    if (fs::is_symlink(fs::path(path), ec)) return true;
#if defined(_WIN32)
    const DWORD attr =
        GetFileAttributesW(utf8ToWide(normalizePath(path)).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    return (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

std::string directoryLinkTarget(const std::string& path) {
    std::error_code ec;
    if (fs::is_symlink(fs::path(path), ec)) {
        const fs::path t = fs::read_symlink(fs::path(path), ec);
        if (!ec) return t.generic_string();
    }
#if defined(_WIN32)
    // junction：走 Win32 的 GetFinalPathNameByHandle —— 比对 reparse 标签
    // 再解析 mount point 要几百行，而"打开目录拿最终路径"一行就够。
    const std::wstring wp = utf8ToWide(normalizePath(path));
    HANDLE h = CreateFileW(wp.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = GetFinalPathNameByHandleW(h, buf, MAX_PATH * 4,
                                              FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= MAX_PATH * 4) return {};
    // 去掉 "\\?\" 前缀
    std::wstring s(buf);
    if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    const int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0,
                                         nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), need, nullptr,
                        nullptr);
    return out;
#else
    return {};
#endif
}

bool createDirectoryJunction(const std::string& linkPath,
                             const std::string& targetPath, std::string* err) {
    std::error_code ec;
    const fs::path link = fs::path(linkPath);
    const fs::path target = fs::path(targetPath);

    if (!isAbsolutePath(linkPath) || !isAbsolutePath(targetPath)) {
        if (err) *err = "junction: both paths must be absolute";
        return false;
    }
    if (fs::exists(link, ec)) {
        if (err) *err = "junction target path already exists: " + linkPath;
        return false;
    }
    fs::create_directories(target, ec);
    if (ec) {
        if (err) *err = "Cannot create code root " + targetPath + ": " +
                        ec.message();
        return false;
    }
    fs::create_directories(link.parent_path(), ec);

#if defined(_WIN32)
    // ⚠ 必须走 wide（中文路径）。`mklink` 是 cmd 内建命令，所以要
    //   `cmd /c mklink /J "<link>" "<target>"`。
    std::wstring cmd = L"cmd.exe /c mklink /J \"" +
                       utf8ToWide(link.generic_string()) + L"\" \"" +
                       utf8ToWide(target.generic_string()) + L"\"";
    if (!runCommandW(cmd, err)) return false;
#else
    fs::create_directory_symlink(target, link, ec);
    if (ec) {
        if (err) *err = "symlink failed: " + ec.message();
        return false;
    }
#endif

    if (!fs::exists(link, ec)) {
        if (err) *err = "junction created but link is not visible: " + linkPath;
        return false;
    }
    VK_LOG_INFO("JUNCTION-CREATE link=%s target=%s", linkPath.c_str(),
                targetPath.c_str());
    return true;
}

// ---------------------------------------------------------------- 创建

namespace {

// 递归复制一个目录（fs::copy 的 recursive 在某些 MSVC 版本上对
// "目标已存在" 的行为不一致，这里自己走一遍，行为可预期）。
int copyTree(const fs::path& from, const fs::path& to, std::string* err) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        if (err) *err = "Cannot create " + to.string() + ": " + ec.message();
        return -1;
    }
    int n = 0;
    for (const auto& entry : fs::recursive_directory_iterator(from, ec)) {
        if (ec) {
            if (err) *err = "Iterate failed: " + ec.message();
            return -1;
        }
        const fs::path rel = fs::relative(entry.path(), from, ec);
        if (ec) continue;
        const fs::path dst = to / rel;
        if (entry.is_directory(ec)) {
            fs::create_directories(dst, ec);
        } else if (entry.is_regular_file(ec)) {
            fs::copy_file(entry.path(), dst,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                if (err) *err = "Copy failed for " + rel.generic_string() + ": " +
                                ec.message();
                return -1;
            }
            ++n;
        }
    }
    return n;
}

// 遍历目录里所有文本文件，按白名单替换占位符。返回替换过的文件数。
int replacePlaceholders(const fs::path& root,
                        const std::vector<std::string>& placeholders,
                        const std::vector<std::pair<std::string, std::string>>& subs,
                        std::string* err) {
    std::error_code ec;
    int changed = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        std::string text;
        if (!looksLikeText(entry.path(), text)) continue;

        std::string out = text;
        for (const auto& ph : placeholders) {
            // 只在**白名单里**的占位符上找替换值
            for (const auto& kv : subs) {
                if (kv.first != ph) continue;
                std::size_t pos = 0;
                while ((pos = out.find(ph, pos)) != std::string::npos) {
                    out.replace(pos, ph.size(), kv.second);
                    pos += kv.second.size();
                }
            }
        }
        if (out == text) continue;

        std::ofstream f(entry.path(), std::ios::binary | std::ios::trunc);
        if (!f) {
            if (err) *err = "Cannot rewrite " + entry.path().string();
            return -1;
        }
        f << out;
        if (!f.good()) {
            if (err) *err = "Write failed: " + entry.path().string();
            return -1;
        }
        ++changed;
    }
    return changed;
}

} // namespace

CreateResult createFromTemplate(const CreateSpec& spec, const ProgressFn& progress) {
    CreateResult r;
    auto report = [&](CreateStep s, const char* detail) {
        if (progress) progress(s, detail);
        VK_LOG_INFO("PROJ-CREATE step=%s %s", createStepLabel(s),
                    detail ? detail : "");
    };
    auto fail = [&](CreateStep where, const std::string& why) {
        r.ok = false;
        r.failedAt = where;
        r.error = why;
        VK_LOG_WARN("PROJ-CREATE FAILED at %s: %s", createStepLabel(where),
                    why.c_str());
        return r;
    };

    // ================= ① 校验 =================
    report(CreateStep::Validate, "checking target folder");

    const std::string templateId =
        spec.templateId.empty() ? gameTypeId(spec.gameType) : spec.templateId;

    const std::string name = sanitizeName(spec.name);
    if (name.empty()) return fail(CreateStep::Validate, "Project name is empty");
    if (spec.root.empty())
        return fail(CreateStep::Validate, "Project folder is empty");
    if (!isAbsolutePath(spec.root))
        return fail(CreateStep::Validate,
                    "Project folder must be an absolute path: " + spec.root);

    const std::string root = normalizePath(spec.root);
    {
        const IoResult t = checkTargetDirUsable(root);
        if (!t.ok) return fail(CreateStep::Validate, t.error);
    }

    TemplateManifest tpl;
    {
        std::string err;
        const IoResult m = loadTemplateManifest(templateId, tpl, &err);
        if (!m.ok) return fail(CreateStep::Validate, err);
    }

    if (spec.codeExternal) {
        if (!isAbsolutePath(spec.externalCodeRoot))
            return fail(CreateStep::Validate,
                        "External code folder must be an absolute path");
        std::error_code ec;
        const fs::path ext = fs::path(spec.externalCodeRoot);
        if (fs::exists(ext, ec)) {
            fs::directory_iterator it(ext, ec);
            if (!ec && it != fs::directory_iterator())
                return fail(CreateStep::Validate,
                            "External code folder is not empty: " +
                                ext.generic_string());
        }
    }

    // ================= ② 复制到 <root>.tmp =================
    const std::string tmp = root + ".tmp";
    {
        std::error_code ec;
        // 上次崩溃留下的残留：**它是我们的**（名字是我们定的），可以删。
        // ⚠ 绝不对 `root` 做这件事 —— 那可能是用户自己的目录。
        if (fs::exists(tmp, ec)) {
            fs::remove_all(tmp, ec);
            VK_LOG_INFO("PROJ-CREATE removed stale tmp=%s", tmp.c_str());
        }
    }
    report(CreateStep::CopyTemplate, "copying template");

    int copied = 0;
    {
        std::string err;
        copied = copyTree(fs::path(tpl.dir), fs::path(tmp), &err);
        if (copied < 0) {
            std::error_code ec;
            fs::remove_all(tmp, ec);
            return fail(CreateStep::CopyTemplate, err);
        }
    }

    // ================= ③ 占位符 + 身份文件 + 画面 =================
    report(CreateStep::ReplaceAndWrite, "replacing placeholders");

    {
        std::string err;
        // 白名单替换：只有 `template.json.placeholders[]` 列出的串会被替换。
        const std::vector<std::pair<std::string, std::string>> subs = {
            {"__PROJECT_NAME__", name},
            {"__TEMPLATE_ID__", tpl.id},
        };
        const int changed = replacePlaceholders(fs::path(tmp), tpl.placeholders,
                                                subs, &err);
        if (changed < 0) {
            std::error_code ec;
            fs::remove_all(tmp, ec);
            return fail(CreateStep::ReplaceAndWrite, err);
        }
        if (changed == 0) {
            // 模板声明了占位符却一处都没替换 = 模板坏了（少了占位符）。
            // 不报错的话工程名永远是 `__PROJECT_NAME__`，且没有任何提示。
            VK_LOG_WARN("PROJ-CREATE no placeholder replaced (template declares "
                        "%zu) — template may be missing them",
                        tpl.placeholders.size());
        }
    }

    // 3a) venn.project.json
    ProjectFile pf;
    pf.format = kProjectFormat;
    pf.name = name;
    pf.templateId = tpl.id;
    pf.gameType = spec.gameType == tpl.gameType ? spec.gameType : tpl.gameType;
    pf.startScene = tpl.startScene;
    pf.codeMode = CodeMode::Vks;
    pf.codeRoot = "scripts";
    pf.codeLinked = spec.codeExternal;
    pf.createdAt = nowIso8601();
    pf.engineName = "venn";
#ifdef NDEBUG
    pf.engineConfig = "Release";
#else
    pf.engineConfig = "Debug";
#endif
    pf.root = tmp;

    if (spec.codeExternal) {
        // codeRoot 存**绝对路径**（外置）；scripts 变成指向它的 junction。
        pf.codeRoot = normalizePath(spec.externalCodeRoot);
    }

    {
        std::string err;
        const IoResult w = saveProject(pf, &err);
        if (!w.ok) {
            std::error_code ec;
            fs::remove_all(tmp, ec);
            return fail(CreateStep::ReplaceAndWrite, err);
        }
    }

    // 3b) 外置代码根：把模板自带的 scripts 内容搬过去，再建 junction。
    if (spec.codeExternal) {
        const fs::path ext = fs::path(pf.codeRoot);
        const fs::path tmpScripts = fs::path(tmp) / "assets" / "scripts";
        std::error_code ec;
        std::string err;

        // 外部目录在 ① 里已确认"不存在或为空" → 可以安全搬内容过去。
        if (fs::exists(tmpScripts, ec)) {
            const int n = copyTree(tmpScripts, ext, &err);
            if (n < 0) {
                fs::remove_all(tmp, ec);
                return fail(CreateStep::ReplaceAndWrite,
                            "Cannot seed external code folder: " + err);
            }
            fs::remove_all(tmpScripts, ec);
        }
        if (!createDirectoryJunction(tmpScripts.generic_string(),
                                     ext.generic_string(), &err)) {
            fs::remove_all(tmp, ec);
            return fail(CreateStep::ReplaceAndWrite,
                        "Cannot link code folder: " + err);
        }
    }

    // 3c) 画面设置（M4）
    {
        DisplaySpec d = spec.display;
        if (d.title.empty()) d.title = name;
        std::string err;
        const IoResult w = writeDisplay(tmp, d, &err);
        if (!w.ok) {
            std::error_code ec;
            fs::remove_all(tmp, ec);
            return fail(CreateStep::ReplaceAndWrite, err);
        }
        if (d.qualityPreset >= 0) {
            const IoResult q = writeQuality(tmp, d.qualityPreset, &err);
            if (!q.ok) {
                std::error_code ec;
                fs::remove_all(tmp, ec);
                return fail(CreateStep::ReplaceAndWrite, err);
            }
        }
    }

    // ================= ④ commit =================
    report(CreateStep::Commit, "finalizing");

    {
        std::error_code ec;
        // root 在 ① 里确认过"不存在或为空"。为空时要先摘掉，否则
        // Windows 上 rename 到已存在的目录会失败。
        if (fs::exists(root, ec)) {
            // 再确认一次（TOCTOU 防护）：这次非空就**放弃**，不删。
            const IoResult t = checkTargetDirUsable(root);
            if (!t.ok) {
                fs::remove_all(tmp, ec);
                return fail(CreateStep::Commit,
                            "Target folder became non-empty: " + t.error);
            }
            fs::remove(root, ec);
            if (ec) {
                fs::remove_all(tmp, ec);
                return fail(CreateStep::Commit,
                            "Cannot clear empty target folder: " + ec.message());
            }
        }
        fs::rename(fs::path(tmp), fs::path(root), ec);
        if (ec) {
            fs::remove_all(tmp, ec);
            return fail(CreateStep::Commit,
                        "Cannot move project into place: " + ec.message());
        }
    }

    // 4b) recents（**失败不回滚工程** —— 见头文件 ⑤）
    if (spec.addToRecent) {
        RecentList recents;
        recents.load();
        ProjectFile committed = pf;
        committed.root = root;
        recents.touch(committed);
        std::string err;
        if (!recents.save(&err))
            VK_LOG_WARN("PROJ-CREATE recents save failed (project kept): %s",
                        err.c_str());
    }

    r.ok = true;
    r.root = root;
    r.filesCopied = copied;
    r.failedAt = CreateStep::Count;
    VK_LOG_INFO("PROJ-CREATE ok root=%s template=%s files=%d linked=%d", root.c_str(),
                tpl.id.c_str(), copied, spec.codeExternal ? 1 : 0);
    return r;
}

// ---------------------------------------------------------------- 自检

int runCreateSelfTest() {
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name, const std::string& detail) {
        if (ok) {
            ++pass;
            std::printf("CREATE-TEST PASS %s\n", name);
        } else {
            ++fail;
            std::printf("CREATE-TEST FAIL %s (%s)\n", name, detail.c_str());
        }
    };

    namespace fs = std::filesystem;
    std::error_code ec;

    // ---- K1 模板查找 ----
    const std::string root = templatesRoot();
    check(!root.empty(), "K1 templates-root", "empty");
    check(!templateDir("first_person").empty(), "K2 template-dir-first",
          "first_person not found under " + root);
    check(!templateDir("third_person").empty(), "K3 template-dir-third",
          "third_person not found under " + root);
    check(templateDir("nope_missing").empty(), "K4 template-dir-missing",
          "found a nonexistent template");

    // ---- K5 清单解析 ----
    TemplateManifest m;
    std::string err;
    check(loadTemplateManifest("third_person", m, &err).ok, "K5 manifest-load", err);
    check(m.gameType == GameType::ThirdPerson, "K6 manifest-gametype",
          gameTypeId(m.gameType));
    check(!m.placeholders.empty(), "K7 manifest-placeholders", "none declared");
    check(!m.files.empty(), "K8 manifest-files", "none declared");

    // ---- K9 创建成功 + 结构对账 ----
    const fs::path base = fs::temp_directory_path(ec) / "venn_create_test";
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);

    // 最近列表重定向到临时目录（不碰用户真实列表）
    const fs::path recent = base / "recent.json";
#if defined(_WIN32)
    _putenv_s("MYVK_LAUNCHER_RECENT", recent.string().c_str());
#else
    setenv("MYVK_LAUNCHER_RECENT", recent.c_str(), 1);
#endif

    const std::string projRoot = (base / "MyGame").generic_string();
    CreateSpec spec;
    spec.gameType = GameType::ThirdPerson;
    spec.name = "MyGame";
    spec.root = projRoot;
    spec.display.width = 1920;
    spec.display.height = 1080;
    spec.display.qualityPreset = 2;   // High

    const CreateResult cr = createFromTemplate(spec, nullptr);
    check(cr.ok, "K9 create-ok", cr.error);
    check(cr.filesCopied > 0, "K10 create-files",
          std::to_string(cr.filesCopied));

    if (cr.ok) {
        // 逐个对账 template.json 的 files[]
        bool allThere = true;
        std::string missing;
        for (const auto& rel : m.files) {
            const fs::path p = fs::path(projRoot) / rel;
            if (!fs::is_regular_file(p, ec)) {
                allThere = false;
                missing = rel;
                break;
            }
        }
        check(allThere, "K11 create-files-manifest", "missing: " + missing);

        check(fs::is_regular_file(fs::path(projRoot) / kProjectFileName, ec),
              "K12 create-project-file", "no venn.project.json");

        // 读回来：name / gameType / startScene
        ProjectFile back;
        std::string err2;
        check(loadProject(projRoot, back, &err2).ok, "K13 create-reload", err2);
        check(back.name == "MyGame", "K14 create-name", back.name);
        check(back.gameType == GameType::ThirdPerson, "K15 create-gametype",
              gameTypeId(back.gameType));
        check(fs::is_regular_file(fs::path(startSceneAbs(back)), ec),
              "K16 create-start-scene-exists", startSceneAbs(back));

        // game.json 的分辨率真的写进去了
        DisplayInfo info;
        readDisplay(projRoot, info, &err2);
        check(info.width == 1920 && info.height == 1080, "K17 create-display",
              std::to_string(info.width) + "x" + std::to_string(info.height));

        // 占位符**全部**被替换（模板里再不许有 __PROJECT_NAME__）
        {
            bool leftover = false;
            std::string where;
            for (const auto& e : fs::recursive_directory_iterator(projRoot, ec)) {
                if (!e.is_regular_file(ec)) continue;
                std::string t;
                if (!looksLikeText(e.path(), t)) continue;
                if (t.find("__PROJECT_NAME__") != std::string::npos ||
                    t.find("__TEMPLATE_ID__") != std::string::npos) {
                    leftover = true;
                    where = e.path().filename().string();
                    break;
                }
            }
            check(!leftover, "K18 create-placeholders-replaced",
                  "leftover in " + where);
        }

        // recents 记上了
        {
            RecentList rl;
            rl.load();
            check(rl.size() == 1 && rl.items()[0].name == "MyGame",
                  "K19 create-recent-added", "size=" + std::to_string(rl.size()));
        }
    }

    // ---- K20 重复创建被拒（目录非空）----
    {
        const CreateResult again = createFromTemplate(spec, nullptr);
        check(!again.ok && !again.error.empty(), "K20 create-nonempty-rejected",
              again.ok ? "accepted" : "");
    }

    // ---- K21 失败无残留（模板不存在 → 不该留下 .tmp）----
    {
        const std::string badRoot = (base / "BadGame").generic_string();
        CreateSpec bad = spec;
        bad.root = badRoot;
        bad.templateId = "does_not_exist";
        const CreateResult rb = createFromTemplate(bad, nullptr);
        check(!rb.ok, "K21 create-bad-template-rejected", "accepted");
        check(!fs::exists(fs::path(badRoot), ec) &&
                  !fs::exists(fs::path(badRoot + ".tmp"), ec),
              "K22 create-no-residue", "left something behind");
    }

    // ---- K23 目标目录是文件 → 拒 ----
    {
        const fs::path f = base / "afile";
        { std::ofstream o(f.string()); o << "x"; }
        CreateSpec s2 = spec;
        s2.root = f.generic_string();
        check(!createFromTemplate(s2, nullptr).ok, "K23 create-path-is-file", "");
    }

    // ---- K24 名字带非法字符时被消毒 ----
    {
        CreateSpec s3 = spec;
        s3.name = "Bad/Name";
        s3.root = (base / "Sanitized").generic_string();
        s3.addToRecent = false;
        const CreateResult r3 = createFromTemplate(s3, nullptr);
        check(r3.ok, "K24 create-sanitized-name", r3.error);
        if (r3.ok) {
            ProjectFile back;
            std::string err3;
            loadProject(r3.root, back, &err3);
            check(back.name == "Bad_Name", "K25 create-sanitized-value", back.name);
        }
    }

    // ---- K26 junction 往返（Windows 上必须真的能用）----
    {
        const fs::path linkP = base / "jt" / "link";
        const fs::path targetP = base / "jt" / "target";
        fs::create_directories(linkP.parent_path(), ec);
        std::string jerr;
        const bool ok = createDirectoryJunction(linkP.generic_string(),
                                                targetP.generic_string(), &jerr);
#if defined(_WIN32)
        check(ok, "K26 junction-create", jerr);
        if (ok) {
            check(isDirectoryLink(linkP.generic_string()), "K27 junction-detect", "");
            const std::string t = directoryLinkTarget(linkP.generic_string());
            check(!t.empty() && normalizePath(t) == normalizePath(
                                    targetP.generic_string()),
                  "K28 junction-target", t);
        }
#else
        check(true, "K26 junction-create (skipped: non-Windows)", "");
        check(true, "K27 junction-detect (skipped)", "");
        check(true, "K28 junction-target (skipped)", "");
        (void)ok;
#endif
    }

    // ---- K29 外置代码根：scripts 变成 junction，代码内容已搬过去 ----
    {
        const std::string ext = (base / "ExternalCode").generic_string();
        CreateSpec s4 = spec;
        s4.name = "LinkedGame";
        s4.root = (base / "LinkedGame").generic_string();
        s4.codeExternal = true;
        s4.externalCodeRoot = ext;
        s4.addToRecent = false;
        const CreateResult r4 = createFromTemplate(s4, nullptr);
#if defined(_WIN32)
        check(r4.ok, "K29 create-external-code", r4.error);
        if (r4.ok) {
            const fs::path linkDir = fs::path(r4.root) / "assets" / "scripts";
            check(isDirectoryLink(linkDir.generic_string()),
                  "K30 external-code-is-link", "not a link");
            check(fs::is_regular_file(fs::path(ext) / "main.vks", ec),
                  "K31 external-code-seeded", "main.vks not copied");
            ProjectFile back;
            std::string err4;
            loadProject(r4.root, back, &err4);
            check(back.codeLinked && isAbsolutePath(back.codeRoot),
                  "K32 external-code-recorded", back.codeRoot);
        }
#else
        (void)r4;
        check(true, "K29 create-external-code (skipped)", "");
        check(true, "K30 external-code-is-link (skipped)", "");
        check(true, "K31 external-code-seeded (skipped)", "");
        check(true, "K32 external-code-recorded (skipped)", "");
#endif
    }

#if defined(_WIN32)
    _putenv_s("MYVK_LAUNCHER_RECENT", "");
#else
    unsetenv("MYVK_LAUNCHER_RECENT");
#endif
    fs::remove_all(base, ec);

    std::printf("CREATE-TEST SUMMARY pass=%d fail=%d\n", pass, fail);
    return fail;
}

} // namespace project
