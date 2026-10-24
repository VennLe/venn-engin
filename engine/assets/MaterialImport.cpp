#include "assets/MaterialImport.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace assets {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kTexturesSub = "textures/materials";
constexpr const char* kVmatSub = "materials/library";
constexpr const char* kManifestSub = "materials/library.json";
constexpr const char* kSettingsSub = "materials/library.source.json";
constexpr const char* kIdPrefix = "materials/library/";

// 贴图槽 → 输出文件名后缀（与 gen_material_library.py 完全一致：
// 两个文件的产物必须能互相覆盖，否则"脚本生成的"和"编辑器导入的"会
// 在 assets 里各存一份、互不认账）。
constexpr const char* kSlotDiff = "diff";
constexpr const char* kSlotNormal = "nor_gl";
constexpr const char* kSlotArm = "arm";

// 已知材质的中文标签与分类。与 gen 脚本的 LOCALE 表保持一致；
// 没登记的目录退化成"标签 = 目录名、分类 = 目录名"。
struct Locale {
    const char* name;
    const char* label;
    const char* labelZh;
    const char* category;
    const char* categoryZh;
};
constexpr Locale kLocale[] = {
    {"grass_ground", "Grass Ground", "草地", "ground", "地面"},
    {"concrete_moss", "Concrete Moss", "苔藓混凝土", "concrete", "混凝土"},
};

const Locale* localeFor(const std::string& name) {
    for (const auto& l : kLocale) {
        if (name == l.name) return &l;
    }
    return nullptr;
}

// 默认地面材质（用户要求：以后地形地面默认 grass_ground）
constexpr const char* kDefaultName = "grass_ground";

float round3(float v) {
    return std::round(static_cast<double>(v) * 1000.0) / 1000.0f;
}

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

bool readJson(const fs::path& p, json& out) {
    std::ifstream f(p);
    if (!f) return false;
    try {
        f >> out;
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

void writeJson(const fs::path& p, const json& doc) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return;
    // 与 Python 脚本的输出逐字节对齐：2 空格缩进、不转义中文、末尾换行。
    // 这样"编辑器导入的"和"脚本生成的"可以是同一个文件，不会因为格式
    // 差异在 git 里反复抖动。
    f << doc.dump(2) << "\n";
}

// 运行时资产根（`<exe>/assets`）：resolveAssetPath 的候选链已经算好了，
// 借它拿一下 —— 不论文件是否存在，形态都是 <root>/materials/library.json。
std::string runtimeAssetRoot() {
    const fs::path probe(resolveAssetPath(kManifestSub));
    return probe.parent_path().parent_path().string();
}

std::string executableDir() {
    char buf[1024] = {};
#if defined(_WIN32)
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
#else
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0';
#endif
    if (!buf[0]) return std::string();
    return fs::path(buf).parent_path().string();
}

// 仓库自己的 **源** 资产目录（`<工程根>/assets`）。
//
// 为什么要写这里：`build/bin/assets` 每次构建都会被 CMake 覆盖，
// 用户导入的材质只有进了源目录才算"留下来"。做法是从 exe 目录向上找
// 工程根（同时有 CMakeLists.txt 与 assets/materials）—— `build/bin/…`
// 往上两三层就到了。找不到就返回空（只写运行时根，功能照常）。
std::string sourceAssetRoot() {
    fs::path dir(executableDir());
    std::error_code ec;
    for (int i = 0; i < 6 && !dir.empty(); ++i) {
        if (fs::exists(dir / "CMakeLists.txt", ec) &&
            fs::is_directory(dir / "assets" / "materials", ec)) {
            return (dir / "assets").string();
        }
        const fs::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    return std::string();
}

// ---------------------------------------------------------------- glTF 解析

struct Package {
    std::string name;
    fs::path pkgDir;      // 材质包目录（贴图 uri 的基准）
    fs::path gltfFile;
    json doc;
};

// glTF 的贴图引用 → 真实文件路径。三级索引，任一级缺失就返回空。
fs::path texUri(const json& doc, const json& texRef, const fs::path& base,
                const char* what) {
    if (!texRef.is_object()) return {};
    const json textures =
        doc.contains("textures") ? doc["textures"] : json::array();
    const json images = doc.contains("images") ? doc["images"] : json::array();
    if (!textures.is_array() || !images.is_array()) return {};

    const int ti = texRef.contains("index") && texRef["index"].is_number_integer()
                       ? texRef["index"].get<int>()
                       : -1;
    if (ti < 0 || ti >= static_cast<int>(textures.size())) return {};
    if (!textures[ti].contains("source")) return {};
    const int ii = textures[ti]["source"].is_number_integer()
                       ? textures[ti]["source"].get<int>()
                       : -1;
    if (ii < 0 || ii >= static_cast<int>(images.size())) return {};
    if (!images[ii].contains("uri") || !images[ii]["uri"].is_string()) return {};

    const std::string uri = images[ii]["uri"].get<std::string>();
    if (uri.empty() || uri.rfind("data:", 0) == 0) return {};

    // glTF 的 uri 用 `/` 分隔；Windows 的 filesystem 同样认它的前半段，
    // 不需要手工换成反斜杠。
    const fs::path p = base / fs::path(uri);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        VK_LOG_WARN("MATLIB-IMPORT-TEX-MISS slot=%s uri=%s base=%s", what,
                    uri.c_str(), base.string().c_str());
        return {};
    }
    return p;
}

// 在 `<matDir>/` 下挑出这个材质的包目录。
fs::path findPackageDir(const fs::path& matDir, const std::string& name,
                        const std::string& tier) {
    std::error_code ec;
    const fs::path exact = matDir / (name + "_" + tier + ".gltf");
    if (fs::is_directory(exact, ec)) return exact;
    // 目标档位没有：退回 1k / 任意一档（宁可导进来，也别让用户对着
    // "0 个材质"发呆）
    for (const char* t : {"1k", "2k", "4k"}) {
        const fs::path alt = matDir / (name + "_" + t + ".gltf");
        if (fs::is_directory(alt, ec)) return alt;
    }
    const fs::path plain = matDir / (name + ".gltf");
    if (fs::is_directory(plain, ec)) return plain;
    for (const auto& de : fs::directory_iterator(matDir, ec)) {
        if (ec) break;
        if (de.is_directory() && de.path().extension() == ".gltf") {
            return de.path();
        }
    }
    return {};
}

fs::path findGltfFile(const fs::path& pkgDir, const std::string& name,
                      const std::string& tier) {
    std::error_code ec;
    const fs::path exact = pkgDir / (name + "_" + tier + ".gltf");
    if (fs::is_regular_file(exact, ec)) return exact;
    for (const auto& de : fs::directory_iterator(pkgDir, ec)) {
        if (ec) break;
        if (de.is_regular_file() && de.path().extension() == ".gltf") {
            return de.path();
        }
    }
    return {};
}

// ---------------------------------------------------------------- 贴图统计

// arm 贴图（R=AO / G=Rough / B=Metal）的通道均值 → 三个标量。
//
// 为什么非算不可：地形层着色器（`terrain.frag`）**只吃标量**、不采样 orm
// 贴图。不给一组合理初值的话，地形地面会变成"粗糙度 1.0 的全白高光"。
// 采样步长按到 64×64 缩略的比例取 —— 和 Python 里 resize(64,64) 等效。
bool channelMeans(const fs::path& p, float* ao, float* rough, float* metal) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(p.string().c_str(), &w, &h, &n, 3);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    const int step = std::max(1, std::min(w, h) / 64);
    double sr = 0.0, sg = 0.0, sb = 0.0;
    long long cnt = 0;
    for (int y = 0; y < h; y += step) {
        const unsigned char* row = px + static_cast<std::size_t>(y) * w * 3;
        for (int x = 0; x < w; x += step) {
            sr += row[x * 3 + 0];
            sg += row[x * 3 + 1];
            sb += row[x * 3 + 2];
            ++cnt;
        }
    }
    stbi_image_free(px);
    if (cnt == 0) return false;
    const double k = 1.0 / (255.0 * static_cast<double>(cnt));
    *ao = static_cast<float>(sr * k);
    *rough = static_cast<float>(sg * k);
    *metal = static_cast<float>(sb * k);
    return true;
}

// ---------------------------------------------------------------- 清单合并

bool mergeEntry(json& manifest, const json& entry) {
    const std::string name = entry.value("name", std::string());
    for (auto& e : manifest["entries"]) {
        if (e.value("name", std::string()) == name) {
            e = entry;
            return false;          // 更新既有条目
        }
    }
    manifest["entries"].push_back(entry);
    return true;                   // 新条目
}

void sortEntries(json& manifest) {
    // 分类顺序：ground 永远最前（地形默认地面），其余按名字。
    std::stable_sort(manifest["entries"].begin(), manifest["entries"].end(),
                     [](const json& a, const json& b) {
                         const std::string ca = a.value("category", "misc");
                         const std::string cb = b.value("category", "misc");
                         const bool ga = (ca == "ground");
                         const bool gb = (cb == "ground");
                         if (ga != gb) return ga;
                         return a.value("name", std::string()) <
                                b.value("name", std::string());
                     });
}

} // namespace

// ============================================================
// summary —— UI 直接显示的短句
// ============================================================
std::string MaterialImportResult::summary() const {
    char buf[256];
    if (!ok) {
        std::snprintf(buf, sizeof(buf), "import failed: %s", error.c_str());
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%d material(s) from %d package(s), %d new",
                  imported, packages, added);
    return buf;
}

// ============================================================
// importMaterialPackages
// ============================================================
MaterialImportResult importMaterialPackages(const MaterialImportOptions& opt) {
    MaterialImportResult r;

    if (opt.sourceDir.empty()) {
        r.error = "no source directory given";
        VK_LOG_WARN("MATLIB-IMPORT-SKIP reason=no-dir");
        return r;
    }

    std::error_code ec;
    const fs::path srcDir(opt.sourceDir);
    if (!fs::is_directory(srcDir, ec)) {
        r.error = "directory not found: " + opt.sourceDir;
        VK_LOG_WARN("MATLIB-IMPORT-FAIL dir=%s reason=not-a-directory",
                    opt.sourceDir.c_str());
        return r;
    }

    // ---- 1. 扫材质包 ----
    std::vector<Package> pkgs;
    for (const auto& de : fs::directory_iterator(srcDir, ec)) {
        if (ec) break;
        if (!de.is_directory()) continue;
        const std::string name = de.path().filename().string();
        const fs::path pkgDir = findPackageDir(de.path(), name, opt.tier);
        if (pkgDir.empty()) continue;
        const fs::path gltf = findGltfFile(pkgDir, name, opt.tier);
        if (gltf.empty()) {
            r.warnings.push_back(name + ": no .gltf inside " + pkgDir.string());
            continue;
        }
        json doc;
        if (!readJson(gltf, doc)) {
            r.warnings.push_back(name + ": .gltf is not valid JSON");
            continue;
        }
        if (!doc.contains("materials") || !doc["materials"].is_array() ||
            doc["materials"].empty()) {
            r.warnings.push_back(name + ": glTF has no materials");
            continue;
        }
        pkgs.push_back(Package{name, pkgDir, gltf, std::move(doc)});
    }
    r.packages = static_cast<int>(pkgs.size());
    VK_LOG_INFO("MATLIB-IMPORT-SCAN dir=%s tier=%s packages=%d",
                opt.sourceDir.c_str(), opt.tier.c_str(), r.packages);

    if (pkgs.empty()) {
        r.error = "no material packages under " + opt.sourceDir +
                  " (expected <name>/<name>_" + opt.tier + ".gltf/)";
        VK_LOG_WARN("MATLIB-IMPORT-EMPTY dir=%s", opt.sourceDir.c_str());
        return r;
    }

    // ---- 2. 目标资产根 ----
    const fs::path rtRoot(runtimeAssetRoot());
    // `MYVK_MATERIAL_NO_MIRROR=1`：自动化用临时工程跑时关掉"回写源目录"，
    // 否则用例会往仓库的 assets/ 里塞测试材质。
    const bool mirror = opt.mirrorToSourceTree &&
                        std::getenv("MYVK_MATERIAL_NO_MIRROR") == nullptr;
    const std::string srcRootStr = mirror ? sourceAssetRoot() : std::string();
    const fs::path srcRoot(srcRootStr);
    // 有些部署里两者相同（直接从工程根跑），这时别写两遍。
    const bool alsoSource = !srcRootStr.empty() && srcRootStr != rtRoot.string();

    // ---- 3. 清单（只更新运行时根；理由见文件头）----
    json manifest;
    const fs::path manifestPath = rtRoot / kManifestSub;
    const bool hadManifest =
        readJson(manifestPath, manifest) && manifest.contains("entries") &&
        manifest["entries"].is_array();
    if (!hadManifest) {
        manifest = json::object();
        manifest["type"] = "material_library";
        manifest["version"] = 1;
        manifest["generatedBy"] = "editor: assets::importMaterialPackages";
        manifest["tier"] = opt.tier;
        manifest["default"] = "";
        manifest["count"] = 0;
        manifest["entries"] = json::array();
    }
    manifest["tier"] = opt.tier;

    // ---- 4. 逐包落盘 ----
    for (const Package& p : pkgs) {
        const json& mat = p.doc["materials"][0];
        const json& pbr =
            mat.contains("pbrMetallicRoughness") && mat["pbrMetallicRoughness"].is_object()
                ? mat["pbrMetallicRoughness"]
                : json::object();

        // 三个槽（缺 slot 就是没有那张贴图）
        struct SlotSrc {
            const char* slot;
            fs::path file;
        };
        std::vector<SlotSrc> slots;
        if (const fs::path f = texUri(p.doc, pbr.contains("baseColorTexture")
                                                  ? pbr["baseColorTexture"]
                                                  : json::object(),
                                      p.pkgDir, kSlotDiff);
            !f.empty()) {
            slots.push_back({kSlotDiff, f});
        }
        if (const fs::path f = texUri(p.doc, mat.contains("normalTexture")
                                                  ? mat["normalTexture"]
                                                  : json::object(),
                                      p.pkgDir, kSlotNormal);
            !f.empty()) {
            slots.push_back({kSlotNormal, f});
        }
        {
            fs::path arm;
            if (pbr.contains("metallicRoughnessTexture")) {
                arm = texUri(p.doc, pbr["metallicRoughnessTexture"], p.pkgDir,
                             kSlotArm);
            }
            if (arm.empty() && mat.contains("occlusionTexture")) {
                arm = texUri(p.doc, mat["occlusionTexture"], p.pkgDir, kSlotArm);
            }
            if (!arm.empty()) slots.push_back({kSlotArm, arm});
        }

        // 复制贴图到每个资产根，同时记下相对路径
        json textures = json::object();
        for (const SlotSrc& s : slots) {
            const std::string ext = s.file.extension().string();
            const std::string fileName = p.name + "_" + s.slot + ext;
            const std::string rel =
                std::string(kTexturesSub) + "/" + p.name + "/" + fileName;

            auto copyTo = [&](const fs::path& root) {
                if (root.empty()) return;
                const fs::path dst = root / rel;
                std::error_code c2;
                fs::create_directories(dst.parent_path(), c2);
                if (fs::equivalent(s.file, dst, c2)) return;   // 同文件别自拷
                fs::copy_file(s.file, dst, fs::copy_options::overwrite_existing,
                              c2);
                if (c2) {
                    VK_LOG_WARN("MATLIB-IMPORT-COPY-FAIL src=%s dst=%s err=%s",
                                s.file.string().c_str(), dst.string().c_str(),
                                c2.message().c_str());
                }
            };
            copyTo(rtRoot);
            if (alsoSource) copyTo(srcRoot);
            textures[s.slot] = rel;
        }

        // 标量：从 arm 贴图统计（拿不到就按"粗糙不反光"的地面初值）
        float ao = 1.0f, rough = 0.9f, metal = 0.0f;
        const auto armIt = std::find_if(
            slots.begin(), slots.end(),
            [](const SlotSrc& s) { return std::string(s.slot) == kSlotArm; });
        if (armIt != slots.end()) {
            if (!channelMeans(armIt->file, &ao, &rough, &metal)) {
                r.warnings.push_back(p.name + ": cannot decode arm texture");
            }
        }
        // glTF 明确写 metallicFactor = 0 就尊重它（这批资源都是 0）
        if (pbr.contains("metallicFactor") && pbr["metallicFactor"].is_number() &&
            pbr["metallicFactor"].get<float>() == 0.0f) {
            metal = 0.0f;
        }
        ao = clamp01(ao);
        rough = clamp01(rough);
        metal = clamp01(metal);

        const Locale* loc = localeFor(p.name);
        const std::string label = loc ? loc->label : p.name;
        const std::string labelZh = loc ? loc->labelZh : p.name;
        const std::string category = loc ? loc->category : p.name;
        const std::string categoryZh = loc ? loc->categoryZh : p.name;

        // .vmat（顺序与 gen 脚本一致，便于人工 diff）
        json vmat = json::object();
        vmat["type"] = "material";
        vmat["version"] = 1;
        vmat["id"] = std::string(kIdPrefix) + p.name;
        vmat["emissive"] = json::array({0.0, 0.0, 0.0});
        vmat["doubleSided"] = false;
        vmat["normalScale"] = 1.0;
        vmat["ao"] = round3(ao);
        vmat["alphaCutoff"] = 0.5;
        vmat["alphaMode"] = "opaque";
        vmat["shadingModel"] = "default";
        vmat["baseColor"] = json::array({1.0, 1.0, 1.0, 1.0});
        vmat["metallic"] = round3(metal);
        vmat["roughness"] = round3(rough);
        vmat["library"] = json::object();
        vmat["library"]["name"] = p.name;
        vmat["library"]["label"] = label;
        vmat["library"]["labelZh"] = labelZh;
        vmat["library"]["category"] = category;
        vmat["library"]["categoryZh"] = categoryZh;
        vmat["library"]["tier"] = opt.tier;
        vmat["library"]["tags"] =
            (category == "ground")
                ? json::array({"terrain", "ground"})
                : json::array({"terrain"});

        auto addSlot = [&](const char* slot, const char* key, bool srgb) {
            if (!textures.contains(slot)) return;
            json s = json::object();
            s["kind"] = "file";
            s["name"] = p.name + "_" + slot;
            s["path"] = textures[slot];
            s["srgb"] = srgb;
            vmat[key] = s;
        };
        addSlot(kSlotDiff, "albedo", true);
        addSlot(kSlotNormal, "normal", false);
        addSlot(kSlotArm, "orm", false);

        const std::string vmatRel =
            std::string(kVmatSub) + "/" + p.name + ".vmat";
        writeJson(rtRoot / vmatRel, vmat);
        if (alsoSource) writeJson(fs::path(srcRoot) / vmatRel, vmat);

        // 清单条目
        json entry = json::object();
        entry["id"] = std::string(kIdPrefix) + p.name;
        entry["name"] = p.name;
        entry["label"] = label;
        entry["labelZh"] = labelZh;
        entry["category"] = category;
        entry["categoryZh"] = categoryZh;
        entry["tier"] = opt.tier;
        entry["vmat"] = vmatRel;
        entry["textures"] = json::object();
        entry["textures"]["albedo"] = textures.contains(kSlotDiff)
                                          ? textures[kSlotDiff].get<std::string>()
                                          : std::string();
        entry["textures"]["normal"] = textures.contains(kSlotNormal)
                                          ? textures[kSlotNormal].get<std::string>()
                                          : std::string();
        entry["textures"]["orm"] = textures.contains(kSlotArm)
                                       ? textures[kSlotArm].get<std::string>()
                                       : std::string();
        entry["roughness"] = round3(rough);
        entry["metallic"] = round3(metal);
        entry["ao"] = round3(ao);

        if (mergeEntry(manifest, entry)) ++r.added;
        ++r.imported;
        r.names.push_back(p.name);

        VK_LOG_INFO("MATLIB-IMPORT-ONE name=%s tier=%s albedo=%s normal=%s "
                    "orm=%s rough=%.3f ao=%.3f metal=%.3f",
                    p.name.c_str(), opt.tier.c_str(),
                    textures.contains(kSlotDiff) ? textures[kSlotDiff].get<std::string>().c_str() : "-",
                    textures.contains(kSlotNormal) ? textures[kSlotNormal].get<std::string>().c_str() : "-",
                    textures.contains(kSlotArm) ? textures[kSlotArm].get<std::string>().c_str() : "-",
                    static_cast<double>(rough), static_cast<double>(ao),
                    static_cast<double>(metal));
    }

    // ---- 5. 排序 + 默认项 + 写回 ----
    sortEntries(manifest);
    manifest["count"] = static_cast<int>(manifest["entries"].size());

    // 默认项：保留原值（指向已存在的条目），否则取 grass_ground / 第一条
    std::string def = manifest.value("default", std::string());
    bool defOk = false;
    for (const auto& e : manifest["entries"]) {
        if (e.value("id", std::string()) == def) { defOk = true; break; }
    }
    if (!defOk) {
        for (const auto& e : manifest["entries"]) {
            if (e.value("name", std::string()) == kDefaultName) {
                def = e.value("id", std::string());
                break;
            }
        }
        if (def.empty() && !manifest["entries"].empty()) {
            def = manifest["entries"][0].value("id", std::string());
        }
    }
    manifest["default"] = def;
    for (auto& e : manifest["entries"]) {
        e["default"] = (e.value("id", std::string()) == def);
    }

    writeJson(manifestPath, manifest);
    r.manifestPath = manifestPath.string();
    r.ok = true;

    VK_LOG_INFO("MATLIB-IMPORT-DONE dir=%s tier=%s imported=%d added=%d "
                "manifest=%s sourceRoot=%s",
                opt.sourceDir.c_str(), opt.tier.c_str(), r.imported, r.added,
                manifestPath.string().c_str(),
                alsoSource ? srcRoot.string().c_str() : "(same as runtime)");
    return r;
}

// ============================================================
// 目录记忆
// ============================================================
MaterialSourceSettings loadMaterialSourceSettings() {
    MaterialSourceSettings s;
    json doc;
    const fs::path p = fs::path(runtimeAssetRoot()) / kSettingsSub;
    if (!readJson(p, doc)) return s;
    if (doc.contains("dir") && doc["dir"].is_string()) {
        s.dir = doc["dir"].get<std::string>();
    }
    if (doc.contains("tier") && doc["tier"].is_string()) {
        s.tier = doc["tier"].get<std::string>();
    }
    if (s.tier.empty()) s.tier = "1k";
    return s;
}

void saveMaterialSourceSettings(const MaterialSourceSettings& s) {
    json doc = json::object();
    doc["type"] = "material_source";
    doc["version"] = 1;
    doc["dir"] = s.dir;
    doc["tier"] = s.tier.empty() ? "1k" : s.tier;
    writeJson(fs::path(runtimeAssetRoot()) / kSettingsSub, doc);
}

std::string guessMaterialSourceDir() {
    std::error_code ec;
    std::vector<fs::path> cands;
    if (const char* env = std::getenv("MYVK_MATERIAL_SOURCE")) {
        cands.emplace_back(env);          // 自动化：指定一个测试源目录
    }
    // 工程隔壁的 venn-resource（本仓库的默认布局）
    const fs::path repo(executableDir());
    if (!repo.empty()) {
        fs::path up = repo;
        for (int i = 0; i < 4; ++i) {
            up = up.parent_path();
            if (up.empty() || up == up.parent_path()) break;
            cands.push_back(up / "venn-resource");
        }
    }
    cands.emplace_back("E:/code/cpp/venn-resource");
    for (const auto& c : cands) {
        if (fs::is_directory(c, ec)) return c.generic_string();
    }
    return std::string();
}

} // namespace assets
