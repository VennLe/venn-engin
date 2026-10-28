#include "project/AssetScan.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace project {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string slash(std::string p) {
    for (char& c : p) {
        if (c == '\\') c = '/';
    }
    return p;
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 与出包那边同一套跳过规则（原子写残留 / 隐藏文件 / Office 锁）
bool skipName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return true;
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

// 已知资产扩展名。**只用来判两件事**：这串是不是引用、要不要当 JSON 解析。
// ⚠ 加新资产类型时必须加进来：少了它 → 那种引用扫不到（静默漏文件），
//   但也不会误报（"未解析"只认这张表里的后缀）。
const char* kExts[] = {
    ".json",   ".vmat", ".vterrain", ".r32",   ".rgba8",   ".png",
    ".jpg",    ".jpeg", ".dds",      ".ktx",   ".ktx2",    ".hdr",
    ".exr",    ".gltf", ".glb",      ".fbx",   ".obj",     ".vmesh",
    ".vmeta",  ".vks",  ".wav",      ".ogg",   ".mp3",     ".ttf",
    ".otf",    ".bin",  ".anim",     ".seq",   ".dlg",     ".prefab",
};

// "id 形态"的目录：这些目录下的引用可以不带扩展名
// （`materials/template_ground` 就是地形分层图的写法）。
const char* kIdDirs[] = {
    "materials/", "terrain/", "textures/", "models/",    "meshes/",
    "scenes/",    "scripts/", "sequences/", "dialogues/", "animators/",
    "ui/",        "fonts/",   "ramps/",    "audio/",     "icons/",
    "prefabs/",
};

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    return lower(s.substr(s.size() - n)) == suffix;
}

bool hasKnownExt(const std::string& s) {
    for (const char* e : kExts) {
        if (endsWith(s, e)) return true;
    }
    return false;
}

bool startsWithIdDir(const std::string& s) {
    const std::string l = lower(s);
    for (const char* d : kIdDirs) {
        if (l.rfind(d, 0) == 0) return true;
    }
    return false;
}

bool isJsonish(const std::string& rel) {
    return endsWith(rel, ".json") || endsWith(rel, ".vmat") ||
           endsWith(rel, ".vterrain");
}

bool isGltf(const std::string& rel) { return endsWith(rel, ".gltf"); }
bool isScript(const std::string& rel) { return endsWith(rel, ".vks"); }

std::string dirOf(const std::string& rel) {
    const size_t p = rel.find_last_of('/');
    return (p == std::string::npos) ? std::string() : rel.substr(0, p);
}

std::string stemOf(const std::string& rel) {
    const size_t dot = rel.find_last_of('.');
    return (dot == std::string::npos) ? rel : rel.substr(0, dot);
}

// ---------------------------------------------------------------- 索引
// 一次遍历 assets/，建三张表。之后所有"这个字符串是不是文件"都是**哈希查表**，
// 不碰磁盘 —— 场景里有成千上万个字符串，逐个 exists() 会慢到不可接受。
struct Index {
    std::string root;
    std::unordered_map<std::string, std::string> exact;               // lower(rel) -> rel
    std::unordered_map<std::string, std::vector<std::string>> noExt;  // lower(去后缀) -> rels
    // 只有 basename 的 id（`playSequence("intro")` / `showDialogue("ch01")`）。
    // 单独一张表而不是塞进 noExt：塞进去的话"`main`"会同时命中
    // `scenes/main.json` 与 `materials/main.vmat`，读起来分不清是哪个规则命中。
    std::unordered_map<std::string, std::vector<std::string>> byBase;
    std::vector<std::string> all;                                     // rel 列表

    void indexOne(const std::string& rel) {
        all.push_back(rel);
        exact[lower(rel)] = rel;
        // 把"每去掉一截后缀"的形态都登记一份：
        //   sequences/intro.seq.json → sequences/intro.seq、sequences/intro
        // 于是 playSequence("intro") 这种只有 basename 的 id 也能命中。
        std::string cur = rel;
        while (true) {
            const size_t dot = cur.find_last_of('.');
            if (dot == std::string::npos) break;
            const size_t slashPos = cur.find_last_of('/');
            if (slashPos != std::string::npos && dot < slashPos) break;
            cur = cur.substr(0, dot);
            noExt[lower(cur)].push_back(rel);
        }
        // basename 链：`a/b/intro.seq.json` → intro.seq / intro
        std::string base = rel;
        const size_t sl = base.find_last_of('/');
        if (sl != std::string::npos) base = base.substr(sl + 1);
        while (true) {
            const size_t dot = base.find_last_of('.');
            if (dot == std::string::npos) break;
            base = base.substr(0, dot);
            byBase[lower(base)].push_back(rel);
        }
    }

    bool build(const fs::path& base, std::string& err) {
        root = slash(base.string());
        std::error_code ec;
        if (!fs::is_directory(base, ec)) {
            err = "assets root is not a directory: " + root;
            return false;
        }
        for (auto it = fs::recursive_directory_iterator(base, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (skipName(it->path().filename().string())) {
                // ⚠⚠ 光 `continue` 只是"不处理这一项"，**递归仍会进去**：
                //   `assets/ramps/.trash/` 里的东西会照样被索引到（实测出包里
                //   混进了 `diag_rb.vramp.105210`）。必须显式剪枝。
                std::error_code dec;
                if (it->is_directory(dec)) it.disable_recursion_pending();
                continue;
            }
            std::error_code sec;
            if (!it->is_regular_file(sec)) continue;
            const fs::path rel = fs::relative(it->path(), base, sec);
            if (sec) continue;
            indexOne(slash(rel.string()));
        }
        return true;
    }
};

// ---------------------------------------------------------------- 扫描器
struct Scanner {
    const Index& idx;
    const ScanOptions& opt;
    ScanResult r;
    std::unordered_set<std::string> inClosure;
    std::unordered_set<std::string> unresolvedSet;
    std::vector<std::string> queue;   // 待解析的 rel（已进闭包）

    Scanner(const Index& i, const ScanOptions& o) : idx(i), opt(o) {}

    void noteUnresolved(const std::string& from, const std::string& what) {
        const std::string s = from + " -> " + what;
        if (unresolvedSet.insert(s).second) r.unresolved.push_back(s);
    }

    void addFile(const std::string& rel) {
        if (!inClosure.insert(rel).second) return;
        r.files.push_back(rel);
        if (isJsonish(rel) || isGltf(rel) || isScript(rel)) queue.push_back(rel);
        // 模型：顺带带上同 stem 的导入缓存。它们不会被任何 JSON 引用，
        // 少了就要在玩家机器上重烤（慢一个量级）。
        if (endsWith(rel, ".fbx") || endsWith(rel, ".glb") ||
            endsWith(rel, ".gltf") || endsWith(rel, ".obj")) {
            const std::string stem = stemOf(rel);
            for (const char* ext : {".vmesh", ".vmeta"}) {
                auto it = idx.exact.find(lower(stem + ext));
                if (it != idx.exact.end()) addFile(it->second);
            }
        }
    }

    // 把一个字符串解析成资产文件（0~N 个）。
    std::vector<std::string> resolve(const std::string& raw,
                                     const std::string& baseRel,
                                     const std::string& fromRel) {
        std::vector<std::string> out;
        std::string s = slash(raw);
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
            s.pop_back();
        if (s.empty()) return out;
        while (s.rfind("./", 0) == 0) s = s.substr(2);
        // ⚠ 判"绝对路径"只认**盘符**（`C:/`）与 **UNC**（`//server`）：
        //   见冒号就报的话，中文散文里的 `优先级: 命令行` 会被当成绝对路径
        //   （模板 game.json 的 `_comment` 里就有这么一句）。URL 直接忽略。
        {
            const bool unc = s.rfind("//", 0) == 0;
            const bool drive = s.size() >= 2 &&
                               ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) &&
                               s[1] == ':' &&
                               (s.size() > 2 && (s[2] == '/' || s[2] == '\\'));
            if (unc || drive) {
                r.warnings.push_back(baseRel + " references an absolute path: " + s);
                return out;
            }
            if (s.find("://") != std::string::npos) return out;
        }
        while (!s.empty() && s.front() == '/') s.erase(s.begin());

        auto probe = [&](const std::string& key) {
            auto e = idx.exact.find(lower(key));
            if (e != idx.exact.end()) out.push_back(e->second);
            auto n = idx.noExt.find(lower(key));
            if (n != idx.noExt.end()) {
                for (const std::string& v : n->second) out.push_back(v);
            }
        };

        // ① 相对**引用者自己所在的目录**：地形资产的 `height`/`splat` 就是
        //    这么写的（`terrain/g/terrain.vterrain` 里写 `height.r32`），
        //    glTF 的 uri 同理。这一条不加，地形永远"少两个文件"。
        if (!baseRel.empty()) probe(baseRel + "/" + s);
        if (!fromRel.empty()) {
            const std::string d = dirOf(fromRel);
            if (!d.empty()) probe(d + "/" + s);
        }
        // ② 相对 assets 根（绝大多数场景字段）
        probe(s);
        // ③ 纯 id（没有斜杠、没有目录）→ 查 basename 表，
        //    这是 `playSequence("intro")` / `showDialogue("ch01")` 的形态。
        if (s.find('/') == std::string::npos) {
            auto b = idx.byBase.find(lower(s));
            if (b != idx.byBase.end()) {
                for (const std::string& v : b->second) out.push_back(v);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    void handleString(const std::string& from, const std::string& base,
                      const std::string& v) {
        const std::vector<std::string> hits = resolve(v, base, from);
        if (!hits.empty()) {
            for (const std::string& h : hits) addFile(h);
            return;
        }
        // 没命中：只有"看起来确实是资产引用"的才算错
        if (hasKnownExt(v) || startsWithIdDir(v)) noteUnresolved(from, v);
    }

    void walkJson(const std::string& rel, const json& j, const std::string& base) {
        if (j.is_string()) {
            handleString(rel, base, j.get<std::string>());
        } else if (j.is_array()) {
            for (const json& e : j) walkJson(rel, e, base);
        } else if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                // ⚠ 跳过 `_comment` / `_note` 这类**散文**字段。模板的
                //   `game.json` 就有一条 `_comment` 里写着 "见 config/render.json"
                //   —— 它不以 `config/` 开头（不在 id 目录表里）、但**以 .json
                //   结尾**，按规则会被当成引用，然后因为"assets 下没有它"而
                //   把打包判死。注释不是引用，这是唯一可靠的判据。
                const std::string& k = it.key();
                if (!k.empty() && k[0] == '_') continue;
                walkJson(rel, it.value(), base);
            }
        }
    }

    void scanJsonFile(const std::string& rel) {
        const fs::path p = fs::path(idx.root) / fs::path(rel);
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            r.warnings.push_back(rel + " cannot be opened");
            return;
        }
        json j;
        try {
            f >> j;
        } catch (const std::exception& e) {
            // ⚠ 坏 JSON 不当致命：一个坏文件不该让出包停下。但**必须**说出来
            //   —— 否则"扫过了"是假的，漏的文件没人知道。
            r.warnings.push_back(rel + " is not valid json: " + e.what());
            return;
        }
        ++r.scanned;
        // glTF 里的相对路径是相对文件自己（规范），其余是相对 assets 根
        walkJson(rel, j, isGltf(rel) ? dirOf(rel) : std::string());
    }

    void scanScriptFile(const std::string& rel) {
        const fs::path p = fs::path(idx.root) / fs::path(rel);
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            r.warnings.push_back(rel + " cannot be opened");
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        ++r.scanned;
        // 只抽**引号里**的字符串：注释里的路径不算引用（模板脚本的注释里就有
        // `assets/scripts/pickup.vks` 这种"说明性"路径，跟着它走会把没人用的
        // 脚本也打进去）。
        for (size_t i = 0; i < text.size(); ++i) {
            const char q = text[i];
            if (q != '"' && q != '\'') continue;
            size_t j = i + 1;
            std::string s;
            while (j < text.size() && text[j] != q) {
                if (text[j] == '\\' && j + 1 < text.size()) {
                    ++j;
                }
                s.push_back(text[j]);
                ++j;
            }
            if (j >= text.size()) break;   // 没闭合：不管
            i = j;
            if (s.empty() || s.size() > 300) continue;
            handleString(rel, std::string(), s);
        }
    }

    void run(const std::vector<std::string>& roots) {
        for (const std::string& raw : roots) {
            std::string rel = slash(raw);
            while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
            if (rel.empty()) continue;
            // ⚠ `idx` 是 const → 只能用 at/find，不能用 operator[]
            //   （用 `operator[]` 编译期就炸，报的还是"找不到接受左值的运算符"）。
            const auto hit = idx.exact.find(lower(rel));
            if (hit == idx.exact.end()) {
                noteUnresolved("(entry)", rel);
                continue;
            }
            addFile(hit->second);
        }

        // 固定带上的目录（ui/ fonts/ 这类"运行时按名字打开"的）
        for (const std::string& pfx : opt.alwaysInclude) {
            const std::string p = lower(pfx);
            for (const std::string& rel : idx.all) {
                if (lower(rel).rfind(p, 0) == 0) addFile(rel);
            }
        }

        for (size_t i = 0; i < queue.size(); ++i) {
            const std::string rel = queue[i];
            if (isJsonish(rel) || isGltf(rel)) scanJsonFile(rel);
            else if (isScript(rel)) scanScriptFile(rel);
        }

        std::sort(r.files.begin(), r.files.end());
        r.indexSize = static_cast<int>(idx.all.size());
        r.ok = r.unresolved.empty();
    }
};

} // namespace

ScanResult scanAssetClosure(const std::string& assetsRootAbs,
                            const std::vector<std::string>& roots,
                            const ScanOptions& opt) {
    ScanResult empty;
    Index idx;
    std::string err;
    if (!idx.build(fs::path(assetsRootAbs), err)) {
        empty.ok = false;
        empty.unresolved.push_back(err);
        return empty;
    }
    Scanner sc(idx, opt);
    sc.run(roots);
    return sc.r;
}

// ============================================================
// 自检（S 组）
// ============================================================
namespace {

int g_sPass = 0;
int g_sFail = 0;

void sCheck(const char* name, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::printf("SCAN-TEST %s actual=%d expected=1 detail=%s result=%s\n", name,
                ok ? 1 : 0, detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_sPass;
    else ++g_sFail;
}

bool writeText(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << body;
    return static_cast<bool>(f);
}

bool hasFile(const ScanResult& r, const char* rel) {
    return std::find(r.files.begin(), r.files.end(), std::string(rel)) != r.files.end();
}

} // namespace

int runScanSelfTest() {
    namespace fs = std::filesystem;
    g_sPass = 0;
    g_sFail = 0;
    std::printf("== selftest: asset closure (G29-a) ==\n");

    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "venn_scan_selftest";
    if (ec) {
        sCheck("temp dir available", false, ec.message());
        std::printf("SCAN-TOTAL pass=%d fail=%d\n", g_sPass, g_sFail);
        return g_sFail;
    }
    fs::remove_all(base, ec);
    const fs::path assets = base / "assets";

    // ---- 造一棵"典型工程"的资产树 ----
    // 引用链：game.json → 场景 → (地形 | 脚本)，地形 → vterrain → 高度/分层图 +
    // 材质 id → .vmat → 贴图；脚本按 id 引过场。
    writeText(assets / "game.json", R"({"scene":"scenes/main.json","title":"T"})");
    writeText(assets / "scenes" / "main.json", R"({
      "entities": [
        {"name":"Terrain","terrain":{"source":"terrain/g/terrain.vterrain"}},
        {"name":"Player","script":{"path":"scripts/main.vks"}}
      ]})");
    writeText(assets / "terrain" / "g" / "terrain.vterrain", R"({
      "height":"height.r32","splat":"splat.rgba8",
      "layers":["materials/ground","","",""]})");
    writeText(assets / "terrain" / "g" / "height.r32", "\x01\x02\x03\x04");
    writeText(assets / "terrain" / "g" / "splat.rgba8", "\x05\x06");
    writeText(assets / "materials" / "ground.vmat", R"({
      "id":"materials/ground",
      "albedo":{"kind":"file","path":"textures/ground_albedo.png"}})");
    writeText(assets / "textures" / "ground_albedo.png", "PNG-bytes");
    writeText(assets / "scripts" / "main.vks",
              "# 注释里的 assets/scripts/orphan.vks 不算引用（没引号）\n"
              "on start {\n  playSequence(\"intro\")\n  print(\"hello\")\n}\n");
    writeText(assets / "sequences" / "intro.seq.json", R"({"tracks":[]})");
    // 固定带上的（运行时按名字打开，没有引用者）
    writeText(assets / "ui" / "hud.json", R"({"bars":[]})");
    // 孤儿：谁都不引用
    writeText(assets / "textures" / "orphan.png", "PNG-orphan");
    writeText(assets / "scenes" / "orphan.json", R"({"entities":[]})");
    writeText(assets / "scripts" / "orphan.vks", "on start { }\n");

    ScanOptions opt;
    opt.alwaysInclude = {"ui/"};
    const ScanResult r =
        scanAssetClosure(slash(assets.string()), {"game.json"}, opt);

    sCheck("S1 扫描成功且无未解析", r.ok, r.unresolved.empty()
                                                ? ""
                                                : r.unresolved[0]);
    // ---- S2：**精确集合**。这条是整组的地基：多一个少一个都算错。 ----
    const std::vector<std::string> want = {
        "game.json",
        "materials/ground.vmat",
        "scenes/main.json",
        "scripts/main.vks",
        "sequences/intro.seq.json",
        "terrain/g/height.r32",
        "terrain/g/splat.rgba8",
        "terrain/g/terrain.vterrain",
        "textures/ground_albedo.png",
        "ui/hud.json",
    };
    sCheck("S2 闭包与预期**逐条相等**", r.files == want,
           r.files.size() == want.size()
               ? "内容不同"
               : ("count=" + std::to_string(r.files.size()) + "/" +
                  std::to_string(want.size())));
    sCheck("S3 孤儿贴图没进包", !hasFile(r, "textures/orphan.png"), "");
    sCheck("S4 孤儿场景没进包", !hasFile(r, "scenes/orphan.json"), "");
    sCheck("S5 注释里的路径不触发（没引号）", !hasFile(r, "scripts/orphan.vks"), "");
    sCheck("S6 材质 id（无扩展名）解析成 .vmat",
           hasFile(r, "materials/ground.vmat"), "");
    sCheck("S7 脚本的纯 id 解析成 .seq.json",
           hasFile(r, "sequences/intro.seq.json"), "");
    sCheck("S8 alwaysInclude 生效（ui/ 无引用者也带上）",
           hasFile(r, "ui/hud.json"), "");
    // 会被解析的 7 个：game.json / scenes/main.json / terrain.vterrain /
    // ground.vmat / sequences/intro.seq.json / scripts/main.vks / ui/hud.json。
    // png / r32 / rgba8 是叶子 —— 它们**不进队列**（scanned 不数它们）。
    sCheck("S9 叶子文件不进解析队列（scanned 只数 json/vks）",
           r.scanned == 7, "scanned=" + std::to_string(r.scanned));

    // ---- S10 未解析：缺材质 / 缺贴图 → 必须 ok=false 且点名 ----
    {
        const fs::path a2 = base / "assets2";
        writeText(a2 / "game.json", R"({"scene":"scenes/main.json"})");
        writeText(a2 / "scenes" / "main.json", R"({
          "materials":["materials/missing"],
          "tex":{"path":"textures/nope.png"},
          "fine":{"path":"textures/ok.png"}})");
        writeText(a2 / "textures" / "ok.png", "x");
        const ScanResult r2 =
            scanAssetClosure(slash(a2.string()), {"game.json"}, {});
        bool namedMat = false, namedTex = false;
        for (const auto& u : r2.unresolved) {
            if (u.find("materials/missing") != std::string::npos) namedMat = true;
            if (u.find("textures/nope.png") != std::string::npos) namedTex = true;
        }
        sCheck("S10 缺材质 → 未解析并点名", !r2.ok && namedMat,
               r2.unresolved.empty() ? "no unresolved" : r2.unresolved[0]);
        sCheck("S11 缺贴图 → 未解析并点名", namedTex, "");
        sCheck("S12 未解析时闭包仍然可用（已解析的部分照收）",
               hasFile(r2, "textures/ok.png"), "");
    }

    // ---- S13 环引用 + 坏 JSON ----
    {
        const fs::path a3 = base / "assets3";
        writeText(a3 / "a.json", R"({"next":"b.json"})");
        writeText(a3 / "b.json", R"({"next":"a.json","tex":"c.png"})");
        writeText(a3 / "c.png", "x");
        writeText(a3 / "bad.json", "{ this is not json");
        const ScanResult r3 =
            scanAssetClosure(slash(a3.string()), {"a.json", "bad.json"}, {});
        sCheck("S13 环引用不死循环（两个都恰好一次）",
               r3.ok && std::count(r3.files.begin(), r3.files.end(),
                                   std::string("a.json")) == 1 &&
                   std::count(r3.files.begin(), r3.files.end(),
                              std::string("b.json")) == 1,
               "files=" + std::to_string(r3.files.size()));
        sCheck("S14 坏 JSON 只记 warning，不判失败", r3.ok, "");
        bool warned = false;
        for (const auto& w : r3.warnings) {
            if (w.find("bad.json") != std::string::npos) warned = true;
        }
        sCheck("S15 坏 JSON 有 warning 点名", warned, "");
    }

    // ---- S16 入口文件缺失 ----
    {
        const ScanResult r4 =
            scanAssetClosure(slash((base / "assets").string()),
                             {"scenes/nothere.json"}, {});
        sCheck("S16 入口不存在 → 未解析（不让「打个空包」过关）", !r4.ok,
               r4.unresolved.empty() ? "" : r4.unresolved[0]);
    }

    fs::remove_all(base, ec);
    std::printf("SCAN-TOTAL pass=%d fail=%d\n", g_sPass, g_sFail);
    return g_sFail;
}

} // namespace project
