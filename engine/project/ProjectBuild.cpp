#include "project/ProjectBuild.h"

#include "assets/Pak.h"
#include "project/AssetScan.h"
#include "project/ProjectFile.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>   // popen / _popen（G30 的风味探测）
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
// ⚠ 只为了 `GetModuleFileNameA`。**不放进头文件**：那会让整个引擎的
//   TU 都吃一遍 windows.h（宏污染 / NOMINMAX 打架，这个仓里踩过）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace project {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::string slash(std::string p) {
    for (char& c : p) {
        if (c == '\\') c = '/';
    }
    return p;
}

// 要跳过的东西。**不做通用通配**（`*~` 之类），只跳这几类明确的：
//   · 点开头 → `.gitkeep` / `.DS_Store` / 隐藏目录
//   · `.tmp` / `.bak` → 原子写的残留
//   · `~$` 开头 → Office/WPS 的锁文件
// ⚠ 不要跳 `.vmeta`：它是网格导入的缓存**元数据**（和 `.vmesh` 配对），
//   少了它下次加载会走回退解析（慢一个量级），甚至因为"判过期"而重烤。
bool skipName(const std::string& name) {
    if (name.empty()) return true;
    if (name == "." || name == "..") return true;
    if (name[0] == '.') return true;
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) return true;
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".bak") == 0) return true;
    if (name.size() >= 2 && name.compare(0, 2, "~$") == 0) return true;
    // 回收站残片：`x.vmat.trash-200246` / `pbr.k0.frag.spv.hotreload-bak.trash-192907`。
    // 后缀是 `.trash-<数字>`，既不是 `.tmp` 也不是 `.bak` —— 第一版漏了它，
    // 包里混进了一个 shader 的垃圾副本。
    {
        std::string ln = name;
        for (char& c : ln) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (ln.find(".trash") != std::string::npos) return true;
    }
    return false;
}

// ⚠ 不要再写一份 `nowIso8601()`：`ProjectFile.h` 里**已经有**一个
//   （`sanitizeName` 隔壁），同名同签名会让编译器在两个重载之间二义
//   （C2668：`project::nowIso8601` 与匿名命名空间里那个）。第一版就是
//   这么撞上的 —— 时间戳这种东西尤其容易被复制成第二份。

// FNV-1a 64。逐块读，不把整个文件塞进内存（资产里有几十 MB 的贴图）。
bool fnvFile(const fs::path& p, std::uint64_t& h, long long& size) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    h = kFnvOffset;
    size = 0;
    std::vector<char> buf(1u << 16);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n <= 0) break;
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= static_cast<std::uint64_t>(
                static_cast<unsigned char>(buf[static_cast<size_t>(i)]));
            h *= kFnvPrime;
        }
        size += static_cast<long long>(n);
    }
    return true;
}

std::string hex64(std::uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(v));
    return std::string(buf);
}

// 引擎 exe 是不是 Debug 构建。
//
// 判据是"可执行文件里有没有调试版 CRT 的导入名"（`VCRUNTIME140D.dll`）。
// 为什么不传一个 CMake 宏进来：宏只能描述**构建时**的意图，而这里要判断
// 的是"手上这个字节流能不能发给玩家"。用字符串判据的唯一缺点是它对
// "有人手工改过 import 表"不成立 —— 那种情况本来就不在我们关心的范围里。
//
// ⚠ 现状（2026-10-28）：`make.cmd build` 默认 `Debug`，所以默认产物**一定是**
//   Debug，依赖 `MSVCP140D/MSVCP140D/VCRUNTIME140_1D`——这三个 DLL **不允许
//   再分发**。出包时必须让用户看见这句话，否则"拷到同事机器上双击没反应"。
bool exeLooksDebug(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    const std::string needle = "VCRUNTIME140D.dll";
    std::vector<char> buf(1u << 16);
    std::string carry;
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n <= 0) break;
        std::string chunk = carry;
        chunk.append(buf.data(), static_cast<size_t>(n));
        if (chunk.find(needle) != std::string::npos) return true;
        // 保留一小段尾巴：needle 可能被切在两次读之间
        carry = chunk.size() > needle.size()
                    ? chunk.substr(chunk.size() - needle.size())
                    : chunk;
    }
    return false;
}

// ------------------------------------------------------------
// G30：探测 exe 的构建风味 —— **跑一次** `<exe> --version`
// ------------------------------------------------------------
// 为什么不能靠"文件里有没有某个字符串"或"路径里有没有 Debug"：
//   · 风味是 `core::Build.h` 的编译期常量，最终只反映在行为上；
//   · 文件名可以说谎（拷一份改个名就绕过），release 里也可能有调试字符串。
// 而 `--version` 本来就为 G30 存在（玩家报问题时第一句话就是"哪个版本"），
// 一份输出两处用。它**在打开日志 / 建窗口之前就返回**，所以这个探测
// 不会产生任何副作用（不留日志、不占 GPU）。
//
// 失败（exe 起不来 / 输出格式不认识）→ `"unknown"`，**不阻断打包**：
// 用户完全可能指着一个别的 exe 来打包（老版本、Debug 版）。
std::string probeExeFlavorImpl(const std::string& exePath, std::string& gitOut) {
    gitOut.clear();
    if (exePath.empty()) return "unknown";
#if defined(_WIN32)
    const std::string cmd = "\"" + exePath + "\" --version 2>nul";
    std::FILE* p = _popen(cmd.c_str(), "r");
#else
    const std::string cmd = "\"" + exePath + "\" --version 2>/dev/null";
    std::FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return "unknown";
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) {
        out += buf;
        if (out.size() > 4096) break;   // 不是我们的 exe 时别无限读
    }
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    // 认的是 token，不是整行格式：
    //   `venn 0.2.0 shipping git=v0.1-3-gabc-dirty`
    // 将来戳里再加字段（比如 buildType）也不会让这里失效。
    std::string flavor = "unknown";
    std::size_t i = 0;
    while (i < out.size()) {
        while (i < out.size() && (out[i] == ' ' || out[i] == '\t' ||
                                  out[i] == '\r' || out[i] == '\n')) {
            ++i;
        }
        const std::size_t b = i;
        while (i < out.size() && out[i] != ' ' && out[i] != '\t' &&
               out[i] != '\r' && out[i] != '\n') {
            ++i;
        }
        const std::string t = out.substr(b, i - b);
        if (t == "shipping" || t == "dev") flavor = t;
        else if (t.rfind("git=", 0) == 0) gitOut = t.substr(4);
    }
    return flavor;
}

// 只删**我们自己建的** `.tmp`。名字不对就拒绝 —— 这个函数唯一的合法目标
// 是 `<out>.tmp`，写成通用的 remove_all 就是给自己留一把能删用户目录的刀。
bool safeRemoveTmp(const fs::path& p) {
    const std::string s = slash(p.string());
    if (s.size() < 4 || s.compare(s.size() - 4, 4, ".tmp") != 0) return false;
    std::error_code ec;
    fs::remove_all(p, ec);
    return !ec;
}

struct Entry {
    std::string rel;   // 相对 dist 根（正斜杠）
    fs::path abs;
};

// 递归收集。目录进不去就**当失败**（不能静默少一整个目录）。
bool collectDir(const fs::path& dir, const std::string& relPrefix,
                std::vector<Entry>& out, std::string& err) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return true;  // 可选目录
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            err = "cannot read directory: " + slash(it->path().string());
            return false;
        }
        const std::string name = it->path().filename().string();
        if (skipName(name)) continue;
        const std::string rel = relPrefix.empty() ? name
                                                 : (relPrefix + "/" + name);
        std::error_code sec;
        if (it->is_directory(sec)) {
            if (!collectDir(it->path(), rel, out, err)) return false;
        } else if (it->is_regular_file(sec)) {
            out.push_back({rel, it->path()});
        }
        // 其它类型（符号链接 / 设备）静默跳过：目录联接在 Windows 上是
        // 目录，会走上面那条分支；真正的符号链接指到工程外，打包时不该跟。
    }
    return true;
}

// ------------------------------------------------------------
// 引擎侧运行时资产（E28-1 第一版踩到的坑，见本文件的文档注释 §"引擎资产"）
// ------------------------------------------------------------
// 引擎的**着色器与内置资产不在工程里**：它们躺在 exe 旁边
// （`<exeDir>/shaders`、`<exeDir>/assets`），靠 `resolveShaderPath` /
// `resolveAssetPath` 的"逐候选根回退"才找得到。开发时 `build/bin/assets`
// 里啥都有，所以这个依赖完全隐形 —— 一旦把 exe 拷到一个只有项目资产的
// 目录里，就变成：
//   · 缺 shaders → 管线建不出来 → **退出时 24 条 VUID 校验错误**（rc=1）
//   · 缺 fonts / ui/fonts → ImGui 断言框 → **进程卡死不退**
// 这两个症状都不指向"资产管理"，所以第一版是这么红的（实测记录在
// docs/package_roadmap.md §3）。
//
// 判据：**白名单目录**。理由与"整棵拷项目资产"相反 —— 引擎侧目录里混着
// 大量开发内容（`assets/models` 165 MB、一堆 `_fixture.json`），整棵拷会让
// 包体积失控。所以这里点名列"代码会按固定名字打开的东西"，并打一条
// 审计日志（`PACKAGE-ENGINE`）让"带了哪些引擎资产"可见。
//
// ⚠⚠ 新增"引擎内置资产"（新的 LUT / 字体 / 默认材质）时必须往这张表里
//   加一行，否则**只有打包版**会缺它 —— 开发机永远复现不了。
const char* kEngineAssetDirs[] = {
    // shaders 在 assets **之外**（`<exeDir>/shaders`），所以路径前缀不同
    "shaders",
    "assets/fonts",      // ImGui 字体（DroidSans）+ SDF 图集
    "assets/ui",         // UI2 的 SDF 字体与默认布局/主题
    "assets/ramps",      // Toon 色带
    "assets/icons",      // 运行时 HUD / 启动器图标
    "assets/materials",  // 默认母材质（很小，项目有则项目优先）
    "assets/textures",   // SSS / BRDF 之类查找表（5 MB 量级）
};

// 把 `<engineDir>/<rel>` 下的文件补进 dist，**项目已有的不覆盖**。
// 目标路径 = `<tmpDir>/<rel>/...` —— rel 自带 `assets/` 或 `shaders/` 前缀，
// 所以"问到哪去"由白名单项自己决定，不需要第二个前缀参数
// （两个前缀参数必然有一处写错，而写错的表现是"文件在，但代码找不到"）。
int copyEngineAssetDir(const fs::path& engineDir, const char* rel,
                       const fs::path& tmpDir, std::vector<BuildFile>& manifest) {
    std::error_code ec;
    const fs::path src = engineDir / fs::path(rel);
    if (!fs::is_directory(src, ec)) return -1;   // -1 = 这一项不存在（调用方报警告）
    int copied = 0;
    for (auto it = fs::recursive_directory_iterator(src, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (skipName(it->path().filename().string())) {
            // ⚠ 跳名字**不等于**剪枝：`.trash/` 这类目录用 `continue` 跳过之后，
            //   递归还会进去（实测把 `ramps/.trash/diag_rb.vramp.105210`
            //   带进了包）。这里显式剪枝。
            std::error_code dec;
                if (it->is_directory(dec)) it.disable_recursion_pending();
            continue;
        }
        std::error_code sec;
        if (!it->is_regular_file(sec)) continue;
        // ⚠ 相对 **engineDir** 算，不是相对 src：要的是 `assets/fonts/x.ttf`
        //   而不是 `fonts/x.ttf`。
        const fs::path r = fs::relative(it->path(), engineDir, sec);
        if (sec) continue;
        const fs::path dst = tmpDir / r;
        std::error_code e2;
        if (fs::exists(dst, e2)) continue;   // 项目优先：不覆盖
        fs::create_directories(dst.parent_path(), e2);
        std::error_code e3;
        fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, e3);
        if (e3) continue;
        std::uint64_t h = 0;
        long long sz = 0;
        if (fnvFile(dst, h, sz)) {
            manifest.push_back({slash(r.string()), sz, hex64(h)});
        }
        ++copied;
    }
    return copied;
}

// game.json 里 `scene` 字段（相对 assets/）。缺失 / 解析失败都返回空。
std::string gameJsonScene(const fs::path& gameJson, std::string& err) {
    std::ifstream f(gameJson);
    if (!f) {
        err = "assets/game.json cannot be opened";
        return {};
    }
    try {
        json j;
        f >> j;
        if (j.contains("scene") && j["scene"].is_string()) {
            return slash(j["scene"].get<std::string>());
        }
        return {};
    } catch (const std::exception& e) {
        err = std::string("assets/game.json is not valid json: ") + e.what();
        return {};
    }
}

bool existsFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool dirNonEmpty(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) return false;
    return fs::directory_iterator(p, ec) != fs::directory_iterator();
}

// README 分两种：散文件版 / 包版。**必须分**——包版的文件夹里根本没有
// `assets/`，照抄散文件版的说明会让玩家以为"少拷了资产"。
// 纯 ASCII：记事本按 UTF-8 打开中文会乱码，而这是玩家会双击的文件。
std::string readmeBody(bool pak, bool hasPdb) {
    std::string s;
    s += "Venn Engine - packaged game\r\n";
    s += "===========================\r\n\r\n";
    s += "How to run\r\n";
    s += "  1. Double-click the .exe in this folder.\r\n";
    if (pak) {
        s += "  2. Everything it needs is in this folder (.pak + config/).\r\n";
        s += "     Do not move the .exe away from the .pak - it looks for\r\n";
        s += "     <exe folder>/game.pak when a file is not on disk.\r\n";
    } else {
        s += "  2. Everything it needs is in this folder (assets/ + config/).\r\n";
        s += "     Do not move the .exe away from assets/ - it looks for\r\n";
        s += "     <exe folder>/assets first.\r\n";
    }
    s += "\r\n";
    s += "Files\r\n";
    if (pak) {
        s += "  game.pak    game content + shaders (scenes, materials, scripts,\r\n";
        s += "              terrain, .spv ...). One file, compressed.\r\n";
    } else {
        s += "  assets/     game content (scenes, materials, scripts, terrain...)\r\n";
        s += "  shaders/    compiled shaders (.spv)\r\n";
    }
    s += "  config/     render / input settings (plain files - safe to edit)\r\n";
    if (hasPdb) {
        s += "  debug/      symbols (.pdb) for this .exe. Not used at runtime;\r\n";
        s += "              keep it so a crash report can be symbolized.\r\n";
    }
    s += "  build.json  manifest: every file with its size and FNV-1a 64 hash.\r\n";
    s += "              Use it to verify the package or to diff two builds.\r\n";
    s += "\r\n";
    s += "Note\r\n";
    s += "  This package was produced by `Venn` (see docs/ship_roadmap.md, E28-1).\r\n";
    s += "  If build.json lists a warning about a Debug runtime, the exe needs the\r\n";
    s += "  Visual C++ *debug* runtime, which is NOT redistributable - build with\r\n";
    s += "  config=Release before shipping.\r\n";
    return s;
}

} // namespace

// ⚠ 这个 `} // namespace` 必须在 `engineExePath` **之前**：它下面三个函数
//   （engineExePath / buildStepLabel / buildProject）都是头文件里声明的公开
//   API，留在匿名命名空间里会变成 `project::(anon)::buildProject` 与
//   `project::buildProject` 两个候选 → 每个调用点 C2668 二义。
// G30：公开版的探测（外部要"打包前先知道这个 exe 是什么味道"时用）。
std::string probeExeFlavor(const std::string& exePath, std::string& gitOut) {
    return probeExeFlavorImpl(exePath, gitOut);
}

std::string engineExePath() {
    char buf[1024] = {};
#if defined(_WIN32)
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0';
#endif
    if (buf[0] == '\0') return {};
    return slash(std::string(buf));
}

const char* buildStepLabel(BuildStep s) {
    switch (s) {
        case BuildStep::Validate: return "Validate";
        case BuildStep::Collect:  return "Collect files";
        case BuildStep::Copy:     return "Copy files";
        case BuildStep::Manifest: return "Write manifest";
        case BuildStep::Commit:   return "Commit";
        default:                  return "?";
    }
}

BuildResult buildProject(const BuildSpec& spec, const BuildProgressFn& progress) {
    BuildResult r;
    auto step = [&](BuildStep s) {
        if (progress) progress(s, buildStepLabel(s));
    };

    // ============================================================
    // ① Validate —— 入口必须齐。缺一个就停在这里，不产出半成品。
    // ============================================================
    step(BuildStep::Validate);
    r.failedAt = BuildStep::Validate;

    std::error_code ec;
    const fs::path root(spec.projectRoot);
    if (!fs::is_directory(root, ec)) {
        r.error = "project root is not a directory: " + spec.projectRoot;
        return r;
    }
    if (spec.outDir.empty()) {
        r.error = "output directory is empty";
        return r;
    }
    if (spec.engineExe.empty() || !existsFile(fs::path(spec.engineExe))) {
        r.error = "engine runtime exe not found: " +
                  (spec.engineExe.empty() ? std::string("(empty)") : spec.engineExe);
        return r;
    }

    ProjectFile pf;
    std::string perr;
    if (!loadProject(slash(root.string()), pf, &perr)) {
        r.error = "cannot read " + std::string(kProjectFileName) + ": " + perr;
        return r;
    }

    const fs::path assetsDir = root / "assets";
    if (!fs::is_directory(assetsDir, ec)) {
        r.error = "project has no assets/ directory: " + slash(assetsDir.string());
        return r;
    }

    // 入口 ①：工程声明的起始场景
    if (!pf.startScene.empty() &&
        !existsFile(assetsDir / fs::path(pf.startScene))) {
        r.missing.push_back("assets/" + pf.startScene);
    }
    // 入口 ②：运行时的总入口
    const fs::path gameJson = assetsDir / "game.json";
    if (!existsFile(gameJson)) {
        r.missing.push_back("assets/game.json");
    } else {
        std::string gerr;
        const std::string gscene = gameJsonScene(gameJson, gerr);
        if (!gerr.empty()) {
            r.error = gerr;
            return r;
        }
        if (!gscene.empty() && !existsFile(assetsDir / fs::path(gscene))) {
            r.missing.push_back("assets/" + gscene);
        }
        // 两个入口不一致不是错误（运行时就听 game.json 的），但必须让人看见
        if (!gscene.empty() && !pf.startScene.empty() &&
            slash(pf.startScene) != gscene) {
            r.warnings.push_back(
                "game.json starts '" + gscene + "' but the project declares '" +
                slash(pf.startScene) + "'; the runtime uses game.json");
        }
    }
    if (!r.missing.empty()) {
        r.error = "missing entry file(s):";
        for (const auto& m : r.missing) r.error += "\n  " + m;
        return r;
    }

    // 出入口：已存在且**非空** → 拒绝。空目录允许（会先删掉再 rename）。
    const fs::path outDir(spec.outDir);
    if (dirNonEmpty(outDir)) {
        r.error = "output directory already exists and is not empty: " +
                  slash(outDir.string()) + "\n(delete it first, or pick another "
                  "output directory - packaging never overwrites)";
        return r;
    }

    // G30：问一下这个 exe 自己是什么味道（dev / shipping）+ git 号。
    // 放在**校验阶段**：它是清单里的一个字段，失败也想让人看见
    // （`unknown` 说明"这个 exe 不是我们的 / 起不来"，值得警惕，
    //  但打包本身仍可以继续 —— 用户可能就想打个别的东西）。
    r.engineFlavor = probeExeFlavor(spec.engineExe, r.engineGit);
    VK_LOG_INFO("PACKAGE-EXE flavor=%s git=%s", r.engineFlavor.c_str(),
                r.engineGit.empty() ? "-" : r.engineGit.c_str());
    if (r.engineFlavor == "unknown") {
        r.warnings.push_back(
            "could not read the engine build flavor (`" + spec.engineExe +
            " --version` failed or printed an unknown format): build.json "
            "records flavor=unknown");
    }

    // 引擎 exe 的构建配置：Debug 包依赖调试版 CRT（不可再分发）
    if (exeLooksDebug(fs::path(spec.engineExe))) {
        const std::string w =
            "engine exe looks like a Debug build (imports VCRUNTIME140D.dll): "
            "the package needs the Visual C++ debug runtime, which is NOT "
            "redistributable. Rebuild with config=Release before shipping.";
        if (!spec.allowDebugEngine) {
            r.error = w;
            return r;
        }
        r.warnings.push_back(w);
    }

    // ============================================================
    // ② Collect —— 枚举 assets/ + config/
    // ============================================================
    step(BuildStep::Collect);
    r.failedAt = BuildStep::Collect;
    std::vector<Entry> entries;
    std::string cerr;
    if (spec.useClosure) {
        // ---- G29-a：引用闭包 ----
        // 入口 = 运行时总入口 + 工程声明的起始场景。两个都要（前者是运行时
        // 真正读的，后者是编辑器/打包界面认的；不一致时上面已经打过 warning）。
        ScanOptions sopt;
        // `ui/` 与 `fonts/` 是"运行时按固定名字打开"的目录，没有引用者 ——
        // 靠闭包永远扫不到，而少了就是字体断言框（见 docs/package_roadmap §4）。
        sopt.alwaysInclude = {"ui/", "fonts/"};
        const ScanResult sc = scanAssetClosure(
            slash(assetsDir.string()), {"game.json", slash(pf.startScene)}, sopt);
        for (const auto& w : sc.warnings) r.warnings.push_back(w);
        r.scanned = sc.scanned;
        if (!sc.ok) {
            // ⚠⚠ 未解析 = **打包失败**，不是 warning（`ship_roadmap` G29 验收
            //   第 2 条）。静默放过它的后果是玩家那边黑屏/丢材质，而开发机上
            //   一切正常 —— 这类错误必须在打包机上报出来。
            r.error = "unresolved asset references (nothing was written):";
            for (const auto& u : sc.unresolved) {
                r.error += "\n  " + u;
                r.unresolved.push_back(u);
            }
            return r;
        }
        r.pruned = std::max(0, sc.indexSize - static_cast<int>(sc.files.size()));
        for (const auto& rel : sc.files) {
            entries.push_back({"assets/" + rel, assetsDir / fs::path(rel)});
        }
        VK_LOG_INFO("PACKAGE-CLOSURE roots=2 files=%d index=%d pruned=%d scanned=%d",
                    static_cast<int>(sc.files.size()), sc.indexSize, r.pruned,
                    sc.scanned);
    } else {
        if (!collectDir(assetsDir, "assets", entries, cerr)) {
            r.error = cerr;
            return r;
        }
        VK_LOG_INFO("PACKAGE-CLOSURE off (whole-tree copy)");
    }
    // `config/` 永远整棵带上：运行时按**固定名字**读（render.json / input.json），
    // 没有"引用"可言，走闭包反而会漏。
    if (!collectDir(root / "config", "config", entries, cerr)) {
        r.error = cerr;
        return r;
    }
    if (entries.empty()) {
        r.error = "nothing to package: assets/ is empty";
        return r;
    }
    if (!fs::exists(root / "config" / "render.json", ec)) {
        r.warnings.push_back(
            "config/render.json is missing: the game will use built-in render "
            "settings");
    }

    // ============================================================
    // ③ Copy —— 先写 <out>.tmp
    // ============================================================
    step(BuildStep::Copy);
    r.failedAt = BuildStep::Copy;
    const fs::path tmpDir = fs::path(spec.outDir + ".tmp");
    // 上一次被打断留下的 tmp：清掉（名字是我们的，见 safeRemoveTmp 的说明）
    if (fs::exists(tmpDir, ec)) safeRemoveTmp(tmpDir);

    std::string exeName = pf.name.empty() ? std::string("Game") : pf.name;
    for (char& c : exeName) {
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    if (exeName.empty()) exeName = "Game";
    exeName += ".exe";

    std::vector<BuildFile> manifest;
    manifest.reserve(entries.size() + 1);
    long long total = 0;

    auto failCopy = [&](const std::string& msg) {
        r.error = msg;
        safeRemoveTmp(tmpDir);   // 失败不留半成品（纪律 ②）
        return r;
    };

    if (!fs::create_directories(tmpDir, ec) && ec) {
        return failCopy("cannot create staging directory: " + slash(tmpDir.string()) +
                        " (" + ec.message() + ")");
    }

    // 引擎 exe：复制 + 改名为 <Name>.exe
    {
        std::error_code cec;
        fs::copy_file(fs::path(spec.engineExe), tmpDir / exeName,
                      fs::copy_options::overwrite_existing, cec);
        if (cec) {
            return failCopy("cannot copy engine exe: " + cec.message());
        }
        std::uint64_t h = 0;
        long long sz = 0;
        if (!fnvFile(tmpDir / exeName, h, sz)) {
            return failCopy("cannot hash copied exe");
        }
        manifest.push_back({exeName, sz, hex64(h)});
        total += sz;
    }

    // G30：符号归档。MSVC 默认把调试信息放在 `<exe 同名>.pdb`（exe 里只有
    // 一个指向它的 GUID），出包时把 pdb 收进 `debug/` 子目录 ——
    //   · 玩家拿到的那一层**没有** pdb（符号等于源码，不该发）；
    //   · 出问题让他们把整个包发回来，我们在 `debug/` 里就能还原栈。
    // 不放在 exe 旁边是刻意的：放旁边后 WinDbg/VS 会**自动**去加载它，
    // 于是"用户机器上有没有符号"变成随机事件。
    if (spec.copyPdb) {
        const fs::path srcPdb =
            fs::path(spec.engineExe).replace_extension(".pdb");
        std::error_code pec;
        if (!fs::is_regular_file(srcPdb, pec)) {
            r.warnings.push_back(
                "pdb not found next to the engine exe (" +
                slash(srcPdb.filename().string()) +
                "): the package ships without symbols, crash dumps will only "
                "have addresses");
        } else {
            const fs::path pdbName = srcPdb.filename();
            const std::string rel = "debug/" + slash(pdbName.string());
            const fs::path dst = tmpDir / fs::path(rel);
            std::error_code dec2;
            fs::create_directories(dst.parent_path(), dec2);
            std::error_code cec2;
            fs::copy_file(srcPdb, dst, fs::copy_options::overwrite_existing,
                          cec2);
            if (cec2) {
                return failCopy("cannot copy pdb: " + cec2.message());
            }
            std::uint64_t h = 0;
            long long sz = 0;
            if (!fnvFile(dst, h, sz)) return failCopy("cannot hash copied pdb");
            manifest.push_back({rel, sz, hex64(h)});
            total += sz;
            r.pdbPath = rel;
            VK_LOG_INFO("PACKAGE-PDB %s bytes=%lld", rel.c_str(), sz);
        }
    }

    for (const Entry& e : entries) {
        const fs::path dst = tmpDir / fs::path(e.rel);
        std::error_code dec;
        fs::create_directories(dst.parent_path(), dec);
        std::error_code cec;
        fs::copy_file(e.abs, dst, fs::copy_options::overwrite_existing, cec);
        if (cec) {
            return failCopy("cannot copy '" + e.rel + "': " + cec.message());
        }
        // 哈希算**目标文件**（不是源）：这样清单描述的就是包里那份字节，
        // 拿去校验另一个副本时不用再区分"源"和"dest"。
        std::uint64_t h = 0;
        long long sz = 0;
        if (!fnvFile(dst, h, sz)) {
            return failCopy("cannot hash '" + e.rel + "'");
        }
        manifest.push_back({e.rel, sz, hex64(h)});
        total += sz;
    }

    // ---- 引擎侧运行时资产：白名单目录补缺（项目优先）----
    int engineFiles = 0;
    {
        const fs::path engineDir = fs::path(spec.engineExe).parent_path();
        for (const char* rel : kEngineAssetDirs) {
            const int n = copyEngineAssetDir(engineDir, rel, tmpDir, manifest);
            if (n < 0) {
                // 找不到某一项 → 不致命（也许这台的构建没带它），但必须让人
                // 看见：包"看起来打成功了"而运行时缺资源，是最难查的一类。
                r.warnings.push_back(std::string(
                    "engine asset directory not found next to the exe: ") + rel);
            } else {
                engineFiles += n;
            }
        }
        VK_LOG_INFO("PACKAGE-ENGINE dirs=%d files=%d",
                    static_cast<int>(sizeof(kEngineAssetDirs) /
                                     sizeof(kEngineAssetDirs[0])),
                    engineFiles);
    }

    // README（纯 ASCII：记事本按 UTF-8 打开中文会乱码，而这是玩家会双击的文件）
    {
        const fs::path readme = tmpDir / "README.txt";
        std::ofstream f(readme, std::ios::binary);
        if (!f) return failCopy("cannot write README.txt");
        f << readmeBody(spec.usePak, !r.pdbPath.empty());
        f.close();
        std::uint64_t h = 0;
        long long sz = 0;
        fnvFile(readme, h, sz);
        manifest.push_back({"README.txt", sz, hex64(h)});
        total += sz;
    }

    // ============================================================
    // ③b 打包：把 `assets/**` 与 `shaders/**` 收进 `game.pak`，再删掉目录树
    // ============================================================
    // 为什么**先拷成目录、再收进包、再删目录**，而不是"直接边扫边写包"：
    //   · 引擎侧资产是**后补**进 `assets/` 的（白名单目录，见上）；shaders 更是
    //     整目录从引擎 exe 旁边拷过来的。边扫边写就得让两条路径都记得补 ——
    //     而"两条路径都得改一处"正是这个仓里反复出事的形态。
    //   · 先落树再收包，包里的内容**必然**等于"树模式下会带的东西"，一个
    //     字节不差；两种模式对比也只需看同一份中间产物。
    //   · 代价只是多写一遍磁盘（模板工程 ~39 MB），出包本来就不是热路径。
    //
    // 包内布局 = **相对 dist 根**：`assets/…` 与 `shaders/…`。
    // 运行时按根前缀取（`AssetSource.h`），所以一个包能装多个根目录，
    // 将来把 `config/` 也收进来只是多一个前缀。
    if (spec.usePak) {
        // 收哪些顶层目录。**只收这两棵**：`config/` 必须留散文件（玩家/美术
        // 可能手改画质），`build.json` / `README.txt` / exe 本来就在根上。
        const char* kPackedRoots[] = {"assets", "shaders"};
        std::vector<assets::PakInput> pakIn;
        std::string walkErr;
        for (const char* rootName : kPackedRoots) {
            const fs::path rootDir = tmpDir / rootName;
            std::error_code wec;
            if (!fs::is_directory(rootDir, wec)) continue;   // 没有就跳过（不是错）
            for (auto it = fs::recursive_directory_iterator(rootDir, wec);
                 it != fs::recursive_directory_iterator(); it.increment(wec)) {
                if (wec) break;
                if (skipName(it->path().filename().string())) {
                    std::error_code dec;
                    if (it->is_directory(dec)) it.disable_recursion_pending();
                    continue;
                }
                std::error_code sec;
                if (!it->is_regular_file(sec)) continue;
                std::ifstream in(it->path(), std::ios::binary);
                if (!in) {
                    walkErr = "cannot read for pak: " +
                              slash(fs::relative(it->path(), tmpDir).string());
                    break;
                }
                std::string bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
                // 相对 **tmpDir**（= 相对 dist 根），于是条目自带 `assets/`
                // 或 `shaders/` 前缀 —— 与运行时的查询 key 逐字对应。
                pakIn.push_back({slash(fs::relative(it->path(), tmpDir)
                                           .generic_string()),
                                 std::move(bytes)});
            }
            if (!walkErr.empty()) break;
        }
        if (!walkErr.empty()) return failCopy(walkErr);

        assets::PakWriteStats pst;
        std::string perr;
        const fs::path pakPath = tmpDir / kPakFileName;
        if (!assets::writePak(pakPath.string(), pakIn, pst, perr, true)) {
            return failCopy(std::string("cannot write ") + kPakFileName + ": " + perr);
        }
        // 释放：pakIn 里还压着 39 MB
        std::vector<assets::PakInput>().swap(pakIn);

        std::uint64_t ph = 0;
        long long psz = 0;
        if (!fnvFile(pakPath, ph, psz)) return failCopy("cannot hash pak");

        // 清单改成"一条 pak"，并把原来那几百条 `assets/**` `shaders/**` 删掉：
        // 清单要描述**磁盘上真实存在的东西**，否则拿着清单去对账会全部对不上。
        std::vector<BuildFile> keep;
        keep.reserve(manifest.size() + 1);
        for (const BuildFile& bf : manifest) {
            bool packed = false;
            for (const char* rootName : kPackedRoots) {
                const std::string pre = std::string(rootName) + "/";
                if (bf.path.rfind(pre, 0) == 0) packed = true;
            }
            if (packed) continue;
            keep.push_back(bf);
        }
        keep.push_back({kPakFileName, psz, hex64(ph)});
        manifest.swap(keep);

        r.pak = true;
        r.pakEntries = pst.files;
        r.pakRaw = pst.rawBytes;
        r.pakPacked = pst.packedBytes;
        VK_LOG_INFO("PACKAGE-PAK file=%s entries=%d stored=%d lz4=%d "
                    "raw=%llu packed=%llu largest=%s(%llu)",
                    kPakFileName, pst.files, pst.stored, pst.lz4,
                    static_cast<unsigned long long>(pst.rawBytes),
                    static_cast<unsigned long long>(pst.packedBytes),
                    pst.largestPath.c_str(),
                    static_cast<unsigned long long>(pst.largestRaw));

        // 删掉目录树：留着它等于"两份资产"，而运行时磁盘优先 —— 于是
        // "改了树没重新打包"这种错永远发现不了。
        for (const char* rootName : kPackedRoots) {
            std::error_code rmec;
            fs::remove_all(tmpDir / rootName, rmec);
            if (rmec) {
                return failCopy(std::string("cannot remove staging ") + rootName +
                                "/ after packing: " + rmec.message());
            }
        }
    }

    // ④ Manifest —— build.json
    // ============================================================
    step(BuildStep::Manifest);
    r.failedAt = BuildStep::Manifest;
    // 总字节从清单重算，不靠各段累加：引擎资产是**后补**进去的，
    // 累加变量散在三处，加第四段时必然漏一处（漏了就是 build.json 里
    // totalBytes 对不上，而它正是"包有没有被改过"的第一个对照量）。
    total = 0;
    for (const BuildFile& bf : manifest) total += bf.size;
    {
        json j;
        j["format"] = 1;
        j["engine"] = "venn";
        j["name"] = pf.name;
        j["exe"] = exeName;
        j["startScene"] = slash(pf.startScene);
        j["builtAt"] = nowIso8601();
        j["builtFrom"] = slash(root.string());
        // ⚠ 清单里明写算法名：将来换 SHA-256 时，读包的人不该靠猜
        //   （"这个 16 位 hex 是哪个算法的"是个真问题）。
        j["hashKind"] = "fnv1a64";
        j["totalFiles"] = static_cast<int>(manifest.size());
        // 其中有多少条是**引擎侧**资产（白名单补进来的）。它和"项目资产"
        // 是两种东西：前者随引擎版本变，后者随美术工作变；分成两个数，
        // 包体积异常时能立刻判断是哪一边涨了。
        j["engineFiles"] = engineFiles;
        // 闭包信息：让"这个包为什么比工程小/大"一眼可查（也让用例能断言
        // 闭包真的在起作用，而不是"恰好工程里没有孤儿"）。
        j["closure"] = spec.useClosure;
        j["pruned"] = r.pruned;
        j["scanned"] = r.scanned;
        // G30：这个包里的 exe 是**哪个味道**（dev / shipping / unknown）。
        // 它是"这个包能不能发给玩家"的第一个判据：dev 版的 exe 带着
        // 校验层与自检命令，发出去等于把调试设施一起发了。
        j["flavor"] = r.engineFlavor;
        if (!r.engineGit.empty()) j["git"] = r.engineGit;
        if (!r.pdbPath.empty()) j["pdb"] = r.pdbPath;
        j["totalBytes"] = total;
        // pak 段：一条 pak 顶几百个文件，所以"包里到底有几个文件、压了多少"
        // 必须单独记 —— 否则 `totalFiles` 会从三百多掉到个位数，
        // 看的人会以为资产丢了。
        if (r.pak) {
            json p;
            p["file"] = kPakFileName;
            p["entries"] = r.pakEntries;
            p["rawBytes"] = r.pakRaw;
            p["packedBytes"] = r.pakPacked;
            j["pak"] = std::move(p);
        }
        if (!r.warnings.empty()) j["warnings"] = r.warnings;
        json arr = json::array();
        for (const BuildFile& bf : manifest) {
            json e;
            e["path"] = bf.path;
            e["size"] = bf.size;
            e["hash"] = bf.hash;
            arr.push_back(std::move(e));
        }
        j["files"] = std::move(arr);

        const fs::path mf = tmpDir / "build.json";
        std::ofstream f(mf, std::ios::binary);
        if (!f) return failCopy("cannot write build.json");
        f << j.dump(2) << "\n";
        if (!f) return failCopy("cannot finish writing build.json");
    }

    // ============================================================
    // ⑤ Commit —— rename
    // ============================================================
    step(BuildStep::Commit);
    r.failedAt = BuildStep::Commit;
    // 空目录挡在 rename 前面：Windows 的 rename 不能覆盖已存在的目录
    if (fs::exists(outDir, ec) && !dirNonEmpty(outDir)) {
        std::error_code rec;
        fs::remove(outDir, rec);   // 只删**空**目录（dirNonEmpty 已确认）
        if (rec) return failCopy("cannot replace empty output directory: " +
                                 rec.message());
    }
    fs::rename(tmpDir, outDir, ec);
    if (ec) {
        return failCopy("cannot move staging directory into place: " +
                        ec.message());
    }

    r.ok = true;
    r.outDir = slash(outDir.string());
    r.files = static_cast<int>(manifest.size());
    r.bytes = total;
    return r;
}

// ============================================================
// 自检（P 组）
// ============================================================
namespace {

int g_pPass = 0;
int g_pFail = 0;

void pCheck(const char* name, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::printf("PACK-TEST %s actual=%d expected=1 detail=%s result=%s\n", name,
                ok ? 1 : 0, detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pPass;
    else ++g_pFail;
}

bool writeText(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << body;
    return static_cast<bool>(f);
}

} // namespace

int runBuildSelfTest() {
    g_pPass = 0;
    g_pFail = 0;
    std::printf("== selftest: package (E28-1 出包) ==\n");

    std::error_code ec;
    const fs::path base =
        fs::temp_directory_path(ec) / "venn_pack_selftest";
    if (ec) {
        pCheck("temp dir available", false, ec.message());
        std::printf("PACK-TOTAL pass=%d fail=%d\n", g_pPass, g_pFail);
        return g_pFail;
    }
    // 每次从干净目录开始：上一次失败留下的东西会让"目标已存在"那条误红。
    fs::remove_all(base, ec);

    const fs::path proj = base / "Proj";
    // 假引擎放在自己的目录里：出包会去 **exe 所在目录** 找 `shaders/` 与
    // `assets/fonts` 这类引擎侧资产（真实布局就是 `<bin>/Game.exe` +
    // `<bin>/shaders` + `<bin>/assets`）。放在 base 根上就没法验这条路径。
    const fs::path engDir = base / "engine";
    const fs::path engExe = engDir / "FakeGame.exe";

    // ---- 造一个最小工程（不依赖 templates/：自检要在任何目录都能跑）----
    ProjectFile pf;
    pf.name = "My Game";            // 故意带空格：exe 名要消毒成 My_Game.exe
    pf.startScene = "scenes/main.json";
    pf.root = slash(proj.string());
    if (!writeText(proj / kProjectFileName, toJsonText(pf))) {
        pCheck("write venn.project.json", false, "cannot write");
        std::printf("PACK-TOTAL pass=%d fail=%d\n", g_pPass, g_pFail);
        return g_pFail;
    }
    writeText(proj / "assets" / "game.json",
              "{ \"scene\": \"scenes/main.json\", \"title\": \"My Game\" }\n");
    writeText(proj / "assets" / "scenes" / "main.json", "{ \"entities\": [] }\n");
    writeText(proj / "assets" / "notes.txt", "payload\n");
    writeText(proj / "config" / "render.json", "{ \"quality\": 2 }\n");
    writeText(engExe, "MZ fake runtime\n");
    // 引擎侧资产：shaders/ 与 assets/fonts 各一个假文件。
    // 同时**故意**在项目里放一个同路径的 fonts 文件（内容不同）——
    // 用来验"项目优先"这条：引擎侧只在项目缺的时候补。
    writeText(engDir / "shaders" / "dummy.spv", "engine shader\n");
    writeText(engDir / "assets" / "fonts" / "dummy.ttf", "engine font\n");
    writeText(proj / "assets" / "fonts" / "dummy.ttf", "project font\n");

    const std::string outDir = slash((base / "dist").string());
    BuildSpec spec;
    spec.projectRoot = slash(proj.string());
    spec.outDir = outDir;
    spec.engineExe = slash(engExe.string());

    // ---- P1 正常打包 ----
    BuildResult r = buildProject(spec, nullptr);
    pCheck("P1 正常工程能打出包", r.ok, r.error);
    if (!r.ok) {
        std::printf("PACK-TOTAL pass=%d fail=%d\n", g_pPass, g_pFail);
        return g_pFail;
    }
    const fs::path dist(outDir);
    pCheck("P2 产物目录里有 exe / assets / build.json",
           existsFile(dist / "My_Game.exe") &&
               existsFile(dist / "assets" / "game.json") &&
               existsFile(dist / "build.json"),
           "exe=" + std::string(existsFile(dist / "My_Game.exe") ? "1" : "0"));
    pCheck("P3 exe 名按工程名消毒（空格 → 下划线）",
           existsFile(dist / "My_Game.exe"), "My_Game.exe");

    // ---- P15~P17 引擎侧资产 ----
    // 这一组守的是第一版**真红过**的那条：开发机 `build/bin/assets` 里什么
    // 都有，所以"引擎资产不在工程里"完全隐形；一拷到干净目录就 24 条 VUID
    // 校验错误 + 字体断言框（进程卡死）。
    {
        pCheck("P15 引擎 shaders 补进了包（<exeDir>/shaders）",
               existsFile(dist / "shaders" / "dummy.spv"), "shaders/dummy.spv");
        // 项目里有同名文件 → 必须用项目的（引擎侧只补缺，不覆盖）
        std::string body;
        {
            std::ifstream f(dist / "assets" / "fonts" / "dummy.ttf",
                            std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            body = ss.str();
        }
        pCheck("P16 项目优先：同路径文件用项目的，不被引擎侧覆盖",
               body == "project font\n",
               "body='" + body.substr(0, 20) + "'");
        std::ifstream mf(dist / "build.json");
        json mj;
        int engFiles = -1;
        try {
            mf >> mj;
            engFiles = mj.value("engineFiles", -1);
        } catch (...) {
        }
        // 只有一个引擎侧文件被补（shaders/dummy.spv）；fonts 那条被项目占了
        pCheck("P17 build.json 记了 engineFiles，且只算真正补进来的",
               engFiles == 1, "engineFiles=" + std::to_string(engFiles));
    }

    // ---- P4 清单与磁盘一致 ----
    {
        std::ifstream f(dist / "build.json");
        json j;
        bool parsed = false;
        try {
            f >> j;
            parsed = true;
        } catch (...) {
        }
        pCheck("P4 build.json 能解析", parsed, parsed ? "" : "parse failed");
        if (parsed) {
            int listed = j.value("totalFiles", 0);
            // 磁盘上的文件数 = 清单里的 + build.json 自己（自己不能哈希自己）
            size_t onDisk = 0;
            for (auto it = fs::recursive_directory_iterator(dist, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (it->is_regular_file(ec)) ++onDisk;
            }
            pCheck("P5 清单条数 + 1 == 磁盘文件数",
                   static_cast<size_t>(listed) + 1 == onDisk,
                   "listed=" + std::to_string(listed) +
                       " onDisk=" + std::to_string(onDisk));
            // 逐个 size 对账（只验条数会漏掉"拷了个空文件"）
            bool sizesOk = true;
            std::string bad;
            for (const auto& e : j["files"]) {
                const fs::path p = dist / fs::path(e.value("path", ""));
                const long long want = e.value("size", -1LL);
                std::error_code sec;
                const auto got = static_cast<long long>(fs::file_size(p, sec));
                if (sec || got != want) {
                    sizesOk = false;
                    bad = e.value("path", "?");
                    break;
                }
            }
            pCheck("P6 每条清单的 size 与磁盘一致", sizesOk, bad);
            pCheck("P7 清单里写了哈希算法名（不是靠猜）",
                   j.value("hashKind", std::string()) == "fnv1a64",
                   j.value("hashKind", std::string("(none)")));
        }
    }

    // ---- P19~P21 引用闭包（G29-a）----
    // 造假工程时故意留了一个孤儿 `assets/notes.txt`（谁都不引用）。
    {
        pCheck("P19 闭包默认开：孤儿文件不进包",
               !existsFile(dist / "assets" / "notes.txt"), "assets/notes.txt");
        std::ifstream mf(dist / "build.json");
        json mj;
        bool closure = false;
        int pruned = -1;
        try {
            mf >> mj;
            closure = mj.value("closure", false);
            pruned = mj.value("pruned", -1);
        } catch (...) {
        }
        pCheck("P20 build.json 记了 closure=true 与 pruned>0",
               closure && pruned > 0,
               "closure=" + std::string(closure ? "1" : "0") +
                   " pruned=" + std::to_string(pruned));

        BuildSpec s6 = spec;
        s6.outDir = slash((base / "dist6").string());
        s6.useClosure = false;   // 对照：整棵拷
        BuildResult r6 = buildProject(s6, nullptr);
        pCheck("P21 对照：--no-closure 时孤儿又被带上",
               r6.ok && existsFile(fs::path(s6.outDir) / "assets" / "notes.txt"),
               r6.ok ? "" : r6.error.substr(0, 40));
    }

    // ---- P22 悬空引用 → 必须失败并点名 ----
    {
        const fs::path p7 = base / "Dangling";
        ProjectFile d = pf;
        d.root = slash(p7.string());
        d.name = "Dangling";
        writeText(p7 / kProjectFileName, toJsonText(d));
        writeText(p7 / "assets" / "game.json", R"({"scene":"scenes/main.json"})");
        writeText(p7 / "assets" / "scenes" / "main.json",
                  R"({"material":"materials/ghost","tex":"textures/nope.png"})");
        BuildSpec s7;
        s7.projectRoot = slash(p7.string());
        s7.outDir = slash((base / "dist7").string());
        s7.engineExe = slash(engExe.string());
        BuildResult r7 = buildProject(s7, nullptr);
        bool named = false;
        for (const auto& u : r7.unresolved) {
            if (u.find("materials/ghost") != std::string::npos) named = true;
        }
        pCheck("P22 悬空引用 → 打包失败且点名（G29 验收第 2 条）",
               !r7.ok && named,
               r7.ok ? "unexpectedly ok" : r7.error.substr(0, 60));
        pCheck("P23 悬空引用失败时不产出 dist",
               !fs::exists(fs::path(s7.outDir)), "");
    }

    // ---- P8 缺入口 → 必须失败（不是"打个空包")----
    {
        const fs::path p2 = base / "Broken";
        writeText(p2 / "assets" / "game.json",
                  "{ \"scene\": \"scenes/missing.json\" }\n");
        ProjectFile bad = pf;
        bad.root = slash(p2.string());
        bad.startScene = "scenes/missing.json";
        writeText(p2 / kProjectFileName, toJsonText(bad));

        BuildSpec s2;
        s2.projectRoot = slash(p2.string());
        s2.outDir = slash((base / "dist2").string());
        s2.engineExe = slash(engExe.string());
        BuildResult rb = buildProject(s2, nullptr);
        pCheck("P8 缺起始场景 → 打包失败", !rb.ok && !rb.missing.empty(),
               rb.ok ? "unexpectedly ok" : rb.error.substr(0, 60));
        pCheck("P9 失败时不留 .tmp 残留",
               !fs::exists(fs::path(s2.outDir + ".tmp")), "");
        pCheck("P10 失败时不产出正式目录", !fs::exists(fs::path(s2.outDir)), "");
    }

    // ---- P11 目标目录非空 → 拒绝覆盖 ----
    {
        writeText(fs::path(outDir) / "keep.txt", "do not delete me\n");
        BuildResult rb = buildProject(spec, nullptr);
        pCheck("P11 目标目录非空 → 拒绝（不覆盖）", !rb.ok, rb.error.substr(0, 50));
        pCheck("P12 被拒绝时原目录内容没被动",
               existsFile(fs::path(outDir) / "keep.txt"), "keep.txt");
        std::error_code dec;
        fs::remove(fs::path(outDir) / "keep.txt", dec);
    }

    // ---- P13 Debug 运行时 → 成功但必须带 WARNING ----
    {
        const fs::path dbgExe = base / "FakeGameDebug.exe";
        writeText(dbgExe,
                  "MZ import table: VCRUNTIME140D.dll MSVCP140D.dll payload\n");
        BuildSpec s3 = spec;
        s3.outDir = slash((base / "dist3").string());
        s3.engineExe = slash(dbgExe.string());
        BuildResult rb = buildProject(s3, nullptr);
        bool hasWarn = false;
        for (const auto& w : rb.warnings) {
            if (w.find("Debug") != std::string::npos) hasWarn = true;
        }
        pCheck("P13 Debug 引擎 → 打得出来但带 WARNING", rb.ok && hasWarn,
               rb.ok ? (hasWarn ? "" : "no debug warning") : rb.error.substr(0, 50));

        // 反过来：allowDebugEngine=false 时必须拒绝
        BuildSpec s4 = s3;
        s4.outDir = slash((base / "dist4").string());
        s4.allowDebugEngine = false;
        BuildResult rc = buildProject(s4, nullptr);
        pCheck("P14 明确要求 Release → Debug 引擎被拒绝", !rc.ok,
               rc.ok ? "unexpectedly ok" : rc.error.substr(0, 40));
    }

    // ---- P18 引擎目录里什么都没有 → 必须留 WARNING（点名哪一项）----
    {
        const fs::path bareDir = base / "bare";
        writeText(bareDir / "Bare.exe", "MZ\n");
        BuildSpec s5 = spec;
        s5.outDir = slash((base / "dist5").string());
        s5.engineExe = slash((bareDir / "Bare.exe").string());
        BuildResult rb = buildProject(s5, nullptr);
        bool named = false;
        for (const auto& w : rb.warnings) {
            if (w.find("shaders") != std::string::npos) named = true;
        }
        pCheck("P18 引擎侧资产缺失 → 打得出来但 WARNING 点名目录",
               rb.ok && named,
               rb.ok ? (named ? "" : "no shaders warning") : rb.error.substr(0, 40));
    }

    // ---- P24~P31 pak 模式（G29-b）----
    // 断言的重点不是"文件存在"，而是**包里的字节能不能被读回来**：
    // 一个索引写着 52 条、内容却错位的包，从文件名上看完全正常。
    {
        BuildSpec s8 = spec;
        s8.outDir = slash((base / "dist8").string());
        s8.usePak = true;
        BuildResult r8 = buildProject(s8, nullptr);
        pCheck("P24 pak 模式能打出包", r8.ok, r8.error);
        if (r8.ok) {
            const fs::path d8(s8.outDir);
            // pak 里装两个根（assets/ + shaders/），所以两棵目录树都必须消失
            pCheck("P25 产物有 game.pak、没有 assets/ 与 shaders/ 目录",
                   existsFile(d8 / kPakFileName) && !fs::exists(d8 / "assets") &&
                       !fs::exists(d8 / "shaders"),
                   "");
            pCheck("P26 config/ 仍是散文件（配置不进包）",
                   existsFile(d8 / "config" / "render.json"), "");

            // 清单里不该再有 assets/** 条目 —— 它必须描述磁盘上真实存在的东西
            std::ifstream mf8(d8 / "build.json");
            json mj8;
            bool noAssetEntries = false;
            int pakEntries = -1;
            try {
                mf8 >> mj8;
                noAssetEntries = true;
                for (const auto& e : mj8["files"]) {
                    const std::string ep = e.value("path", std::string());
                    if (ep.rfind("assets/", 0) == 0 || ep.rfind("shaders/", 0) == 0) {
                        noAssetEntries = false;
                    }
                }
                if (mj8.contains("pak")) {
                    pakEntries = mj8["pak"].value("entries", -1);
                }
            } catch (...) {
            }
            pCheck("P27 清单里没有 assets/** shaders/** 条目（改记 game.pak）",
                   noAssetEntries, "");
            pCheck("P28 build.json 的 pak.entries 与结果一致",
                   pakEntries == r8.pakEntries && pakEntries > 0,
                   "json=" + std::to_string(pakEntries) + " result=" +
                       std::to_string(r8.pakEntries));

            // 真正读一遍包：内容必须与源文件逐字节相同
            assets::Pak pk;
            std::string pe;
            if (pk.open(slash((d8 / kPakFileName).string()), pe)) {
                std::string src;
                {
                    std::ifstream f(proj / "assets" / "scenes" / "main.json",
                                    std::ios::binary);
                    std::stringstream ss;
                    ss << f.rdbuf();
                    src = ss.str();
                }
                std::string got;
                std::string re;
                // ⚠ key 带 `assets/` 前缀：包内路径是**相对 dist 根**的
                //   （同一个包还装 `shaders/…`）。
                const bool okRead =
                    pk.read("assets/scenes/main.json", got, re) && got == src;
                pCheck("P29 从包里读回的场景与源文件逐字节相同", okRead,
                       okRead ? "" : re);
                pCheck("P30 包里的条目数 = pakEntries",
                       pk.fileCount() == r8.pakEntries,
                       std::to_string(pk.fileCount()));
                // shader 进包（G29-c）：假引擎里有一个 shaders/dummy.spv
                std::string spv;
                std::string serr;
                pCheck("P30b 包的 shaders/ 里有 .spv（shader 已进包）",
                       pk.read("shaders/dummy.spv", spv, serr) &&
                           spv == "engine shader\n",
                       serr);
            } else {
                pCheck("P29 打出来的 assets.pak 能被打开", false, pe);
            }
        }

        // 对照：默认（非 pak）模式下**必须**是目录树 —— 否则 P25 证明不了
        // "是 --pak 干的"（也可能哪天打包器把 assets/ 整体弄丢了还恰好没验）。
        pCheck("P31 对照：非 pak 模式产出 assets/ 与 shaders/ 目录树",
               existsFile(dist / "assets" / "scenes" / "main.json") &&
                   existsFile(dist / "shaders" / "dummy.spv") &&
                   !existsFile(dist / kPakFileName),
               "");
    }

    fs::remove_all(base, ec);
    std::printf("PACK-TOTAL pass=%d fail=%d\n", g_pPass, g_pFail);
    return g_pFail;
}

} // namespace project
