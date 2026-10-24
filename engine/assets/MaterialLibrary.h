#pragma once
// ============================================================
// assets/MaterialLibrary —— 材质仓库（只读索引，不碰 GPU）
//
// 素材来源：`tools/gen_material_library.py` 从 `venn-resource` 的 glTF
// 材质包抽出贴图，落成
//   assets/materials/library/<name>.vmat      ← 材质本体（走既有 .vmat 通路）
//   assets/materials/library.json             ← 仓库清单（本类读这个）
//
// 为什么要有清单而不是直接扫目录：
//   · 面板要按**分类**分组、要中文标签、要排「默认项」在最前，
//     这些信息只能来自声明的清单，从文件名猜不出来；
//   · 扫目录拿不到 label/category，就得在每个 .vmat 里塞一份重复元数据，
//     清单是单一事实源。
// 但清单**不是必需的**：`.vmat` 里的 `library` 块同样带全了元数据，
// 清单缺失时本类会退化成"扫目录 + 读每个 .vmat 的 library 块"。
// 这样用户手丢一个 .vmat 进来也能用，不用回去跑 Python。
//
// 本类**不加载贴图、不建描述符集**。渲染层拿到的是相对资产根的路径，
// 谁要显示就自己去 `assets::resolveAssetPath` + `Texture::fromFile`
// （`TerrainPass::loadLayerJson` 就是这么干的）。
// ============================================================

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace assets {

// 一条仓库条目。字段顺序与 `library.json` / `.vmat` 的 `library` 块对齐。
struct MaterialLibraryEntry {
    std::string id;          // "materials/library/grass_ground"（= .vmat 的 id）
    std::string name;        // "grass_ground"
    std::string label;       // "Grass Ground"
    std::string labelZh;     // "草地"
    std::string category;    // "ground"
    std::string categoryZh;  // "地面"
    std::string tier;        // "1k"
    std::string vmatPath;    // 相对资产根："materials/library/grass_ground.vmat"
    std::string albedoPath;  // 相对资产根；空 = 该槽没有
    std::string normalPath;
    std::string ormPath;

    glm::vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
    float roughness = 0.9f;
    float metallic = 0.0f;
    float ao = 1.0f;
    bool isDefault = false;

    // 有贴图的条目才能当"贴图笔刷"（`PaintLayer` 只认 albedo+normal）
    bool hasAlbedo() const { return !albedoPath.empty(); }
    bool hasNormal() const { return !normalPath.empty(); }
};

class MaterialLibrary {
public:
    // 扫清单 + 补扫目录。可重复调用（reload）。
    // 返回是否**至少**拿到一条条目；拿不到只打 WARN，不抛。
    bool load();
    void reload() { (void)load(); }

    bool loaded() const { return m_loaded; }
    size_t size() const { return m_entries.size(); }
    const std::vector<MaterialLibraryEntry>& entries() const { return m_entries; }

    // idOrName 可以是 "materials/library/grass_ground"，也可以只是
    // "grass_ground" 或存有该 id 的 ".vmat" 相对路径 —— 三种写法都能命中。
    const MaterialLibraryEntry* find(const std::string& idOrName) const;

    // 清单里标了 default 的那条；没有就用第一条；库空则 nullptr。
    const MaterialLibraryEntry* defaultEntry() const;

    // 去重后的分类列表，按清单里的出现顺序（先 ground 后 concrete）
    std::vector<std::string> categories() const;

private:
    bool loadManifest(const std::string& absPath);
    void scanDirectory(const std::string& absDir);
    void parseVmat(const std::string& absPath,
                   const std::string& relPath,
                   bool fromManifest);
    void sortEntries();
    void report() const;

    std::vector<MaterialLibraryEntry> m_entries;
    bool m_loaded = false;
    bool m_usedManifest = false;
    std::string m_source;       // "manifest" | "scan" | "manifest+scan"
};

} // namespace assets
