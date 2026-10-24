#include "assets/MaterialLibrary.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace assets {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kManifestRel = "materials/library.json";
constexpr const char* kLibraryDirRel = "materials/library";
constexpr const char* kIdPrefix = "materials/library/";

// 贴图槽：`{"kind":"file","path":"textures/..."}`
std::string slotPath(const json& jm, const char* key) {
    if (!jm.contains(key) || !jm[key].is_object()) return std::string();
    const auto& o = jm[key];
    if (!o.contains("path") || !o["path"].is_string()) return std::string();
    return o["path"].get<std::string>();
}

std::string jstr(const json& j, const char* key, const std::string& fallback = std::string()) {
    if (!j.contains(key) || !j[key].is_string()) return fallback;
    return j[key].get<std::string>();
}

float jnum(const json& j, const char* key, float fallback) {
    if (!j.contains(key) || !j[key].is_number()) return fallback;
    return j[key].get<float>();
}

bool jbool(const json& j, const char* key, bool fallback) {
    if (!j.contains(key) || !j[key].is_boolean()) return fallback;
    return j[key].get<bool>();
}

glm::vec4 jvec4(const json& j, const char* key, glm::vec4 fallback) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 4) return fallback;
    return glm::vec4(j[key][0].get<float>(), j[key][1].get<float>(),
                     j[key][2].get<float>(), j[key][3].get<float>());
}

// "materials/library/grass_ground" → "grass_ground"
std::string stripIdPrefix(std::string id) {
    const std::string prefix = kIdPrefix;
    if (id.rfind(prefix, 0) == 0) id = id.substr(prefix.size());
    const std::string ext = ".vmat";
    if (id.size() > ext.size() &&
        id.compare(id.size() - ext.size(), ext.size(), ext) == 0) {
        id.resize(id.size() - ext.size());
    }
    return id;
}

bool readJsonFile(const std::string& abs, json& out) {
    std::ifstream f(abs);
    if (!f) return false;
    try {
        f >> out;
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

} // namespace

// ============================================================
// load —— 先读清单，再补扫目录
//
// 两条来源合并的用意：清单给出**顺序与默认项**，目录扫描兜住"用户手丢进来
// 的 .vmat"。同一 id 只留一份（清单优先，因为它带分类与中文标签）。
// ============================================================
bool MaterialLibrary::load() {
    m_entries.clear();
    m_loaded = false;
    m_usedManifest = false;

    bool any = false;

    const std::string manifestAbs = resolveAssetPath(kManifestRel);
    if (fs::is_regular_file(manifestAbs)) {
        any = loadManifest(manifestAbs) || any;
    } else {
        VK_LOG_WARN("MATLIB-MANIFEST-MISS path=%s（退化为扫目录）", manifestAbs.c_str());
    }

    const std::string dirAbs = resolveAssetPath(kLibraryDirRel);
    if (fs::is_directory(dirAbs)) {
        scanDirectory(dirAbs);
        any = any || !m_entries.empty();
    } else {
        VK_LOG_WARN("MATLIB-DIR-MISS path=%s", dirAbs.c_str());
    }

    m_source = m_usedManifest ? (m_entries.empty() ? "manifest" : "manifest+scan")
                              : "scan";
    sortEntries();
    m_loaded = any;
    report();
    return any;
}

bool MaterialLibrary::loadManifest(const std::string& absPath) {
    json doc;
    if (!readJsonFile(absPath, doc)) {
        VK_LOG_WARN("MATLIB-MANIFEST-FAIL path=%s（JSON 解析失败）", absPath.c_str());
        return false;
    }
    if (!doc.contains("entries") || !doc["entries"].is_array()) {
        VK_LOG_WARN("MATLIB-MANIFEST-FAIL path=%s（缺 entries）", absPath.c_str());
        return false;
    }

    size_t n = 0;
    for (const auto& je : doc["entries"]) {
        if (!je.is_object()) continue;
        MaterialLibraryEntry e;
        e.name = jstr(je, "name");
        e.id = jstr(je, "id", kIdPrefix + e.name);
        if (e.name.empty()) e.name = stripIdPrefix(e.id);
        if (e.id.empty()) e.id = kIdPrefix + e.name;

        e.label = jstr(je, "label", e.name);
        e.labelZh = jstr(je, "labelZh", e.label);
        e.category = jstr(je, "category", "misc");
        e.categoryZh = jstr(je, "categoryZh", e.category);
        e.tier = jstr(je, "tier", "1k");
        e.vmatPath = jstr(je, "vmat", e.id + ".vmat");
        e.isDefault = jbool(je, "default", false);

        if (je.contains("textures") && je["textures"].is_object()) {
            const auto& jt = je["textures"];
            e.albedoPath = jstr(jt, "albedo");
            e.normalPath = jstr(jt, "normal");
            e.ormPath = jstr(jt, "orm");
        }
        e.roughness = jnum(je, "roughness", 0.9f);
        e.metallic = jnum(je, "metallic", 0.0f);
        e.ao = jnum(je, "ao", 1.0f);

        m_entries.push_back(std::move(e));
        ++n;
    }

    m_usedManifest = true;
    VK_LOG_INFO("MATLIB-MANIFEST path=%s tier=%s count=%zu default=%s",
                absPath.c_str(), jstr(doc, "tier", "?").c_str(), n,
                jstr(doc, "default", "-").c_str());
    return n > 0;
}

// 扫目录：每个 .vmat 的 `library` 块补齐元数据；清单里已有的 id 跳过。
void MaterialLibrary::scanDirectory(const std::string& absDir) {
    std::error_code ec;
    size_t added = 0;
    for (const auto& de : fs::directory_iterator(absDir, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        if (de.path().extension() != ".vmat") continue;
        const std::string rel =
            std::string(kLibraryDirRel) + "/" + de.path().filename().string();
        const std::string id = stripIdPrefix(rel);
        bool dup = false;
        for (const auto& e : m_entries) {
            if (e.name == id) { dup = true; break; }
        }
        if (dup) continue;
        parseVmat(de.path().string(), rel, false);
        ++added;
    }
    if (added > 0) {
        VK_LOG_INFO("MATLIB-SCAN dir=%s added=%zu", absDir.c_str(), added);
    }
}

void MaterialLibrary::parseVmat(const std::string& absPath,
                               const std::string& relPath, bool fromManifest) {
    (void)fromManifest;
    json doc;
    if (!readJsonFile(absPath, doc)) {
        VK_LOG_WARN("MATLIB-VMAT-FAIL path=%s", absPath.c_str());
        return;
    }

    MaterialLibraryEntry e;
    e.name = stripIdPrefix(relPath);
    e.id = jstr(doc, "id", kIdPrefix + e.name);
    e.vmatPath = relPath;
    e.baseColor = jvec4(doc, "baseColor", glm::vec4(1.0f));
    e.roughness = jnum(doc, "roughness", 0.9f);
    e.metallic = jnum(doc, "metallic", 0.0f);
    e.ao = jnum(doc, "ao", 1.0f);
    e.albedoPath = slotPath(doc, "albedo");
    e.normalPath = slotPath(doc, "normal");
    e.ormPath = slotPath(doc, "orm");

    // .vmat 里的 library 块（清单缺失时的元数据来源）
    if (doc.contains("library") && doc["library"].is_object()) {
        const auto& jl = doc["library"];
        e.label = jstr(jl, "label", e.name);
        e.labelZh = jstr(jl, "labelZh", e.label);
        e.category = jstr(jl, "category", "misc");
        e.categoryZh = jstr(jl, "categoryZh", e.category);
        e.tier = jstr(jl, "tier", "1k");
    } else {
        e.label = e.name;
        e.labelZh = e.name;
        e.category = "misc";
        e.categoryZh = "其它";
        e.tier = "1k";
    }

    m_entries.push_back(std::move(e));
}

// 分类顺序：ground 永远在最前（地形默认地面），其余按出现顺序。
// 同分类内 default 排第一，然后按 name 字典序 —— 保证面板顺序稳定。
void MaterialLibrary::sortEntries() {
    std::vector<std::string> order;
    for (const auto& e : m_entries) {
        if (std::find(order.begin(), order.end(), e.category) == order.end()) {
            order.push_back(e.category);
        }
    }
    auto catRank = [&order](const std::string& c) {
        if (c == "ground") return size_t(0);
        auto it = std::find(order.begin(), order.end(), c);
        return it == order.end() ? size_t(99) : size_t(1) + size_t(it - order.begin());
    };
    std::stable_sort(m_entries.begin(), m_entries.end(),
                     [&](const MaterialLibraryEntry& a, const MaterialLibraryEntry& b) {
                         const size_t ra = catRank(a.category);
                         const size_t rb = catRank(b.category);
                         if (ra != rb) return ra < rb;
                         if (a.isDefault != b.isDefault) return a.isDefault;
                         return a.name < b.name;
                     });
}

const MaterialLibraryEntry* MaterialLibrary::find(const std::string& idOrName) const {
    if (idOrName.empty()) return nullptr;
    // 三种写法都认：全 id / 裸名 / .vmat 相对路径
    const std::string bare = stripIdPrefix(idOrName);
    for (const auto& e : m_entries) {
        if (e.id == idOrName) return &e;
    }
    for (const auto& e : m_entries) {
        if (e.name == idOrName || e.name == bare) return &e;
    }
    for (const auto& e : m_entries) {
        if (e.vmatPath == idOrName) return &e;
    }
    return nullptr;
}

const MaterialLibraryEntry* MaterialLibrary::defaultEntry() const {
    for (const auto& e : m_entries) {
        if (e.isDefault) return &e;
    }
    return m_entries.empty() ? nullptr : &m_entries.front();
}

std::vector<std::string> MaterialLibrary::categories() const {
    std::vector<std::string> out;
    for (const auto& e : m_entries) {
        if (std::find(out.begin(), out.end(), e.category) == out.end()) {
            out.push_back(e.category);
        }
    }
    return out;
}

void MaterialLibrary::report() const {
    if (m_entries.empty()) {
        VK_LOG_WARN("MATLIB-EMPTY source=%s（材质仓库为空，地形层会退回纯色）",
                    m_source.c_str());
        return;
    }
    const auto* d = defaultEntry();
    VK_LOG_INFO("MATLIB-LOAD source=%s count=%zu default=%s",
                m_source.c_str(), m_entries.size(), d ? d->id.c_str() : "-");
    for (const auto& e : m_entries) {
        VK_LOG_INFO("MATLIB-ENTRY id=%s cat=%s tier=%s albedo=%s normal=%s orm=%s",
                    e.id.c_str(), e.category.c_str(), e.tier.c_str(),
                    e.albedoPath.empty() ? "-" : e.albedoPath.c_str(),
                    e.normalPath.empty() ? "-" : e.normalPath.c_str(),
                    e.ormPath.empty() ? "-" : e.ormPath.c_str());
    }
}

} // namespace assets
