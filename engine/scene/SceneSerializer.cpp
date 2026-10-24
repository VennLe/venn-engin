#include "scene/SceneSerializer.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Material.h"
#include "assets/Mesh.h"
#include "assets/Model.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "physics/CollisionWorld.h"
#include "scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace scene {

namespace {

using json = nlohmann::json;

constexpr int kFormatVersion = 1;

// ============================================================
// 基础类型 <-> JSON
// ============================================================

json vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
json vec4ToJson(const glm::vec4& v) {
    return json::array({v.x, v.y, v.z, v.w});
}
json colorToJson(const uint8_t c[4]) {
    return json::array({c[0], c[1], c[2], c[3]});
}

glm::vec3 vec3FromJson(const json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() < 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
glm::vec4 vec4FromJson(const json& j, const glm::vec4& def) {
    if (!j.is_array() || j.size() < 4) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(),
            j[3].get<float>()};
}
void colorFromJson(const json& j, uint8_t out[4], const uint8_t def[4]) {
    if (!j.is_array() || j.size() < 4) {
        for (int i = 0; i < 4; ++i) out[i] = def[i];
        return;
    }
    for (int i = 0; i < 4; ++i) {
        int v = 0;
        try {
            v = j[i].get<int>();
        } catch (...) {
            v = def[i];
        }
        out[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

// ============================================================
// 纹理来源
// ============================================================

json textureSourceToJson(const assets::Texture* tex) {
    if (!tex) return nullptr;
    const assets::TextureSource& s = tex->source();

    json j;
    switch (s.kind) {
        case assets::TextureSource::Kind::Solid:
            j["kind"] = "solid";
            j["name"] = s.name;
            j["color"] = colorToJson(s.color);
            j["size"] = s.size;
            return j;
        case assets::TextureSource::Kind::Checker:
            j["kind"] = "checker";
            j["name"] = s.name;
            j["colorA"] = colorToJson(s.colorA);
            j["colorB"] = colorToJson(s.colorB);
            j["cell"] = s.cell;
            j["size"] = s.size;
            // 色彩空间（数据贴图 = false）。历史文件没有这个字段，
            // 读的时候默认 true（= 旧行为）。
            j["srgb"] = s.srgb;
            return j;
        case assets::TextureSource::Kind::File:
            j["kind"] = "file";
            j["name"] = s.name;
            j["path"] = s.path;
            j["srgb"] = s.srgb;
            return j;
        default:
            // Unknown：glTF 内嵌像素贴图等，随模型重新加载得到，不单独落盘
            return nullptr;
    }
}

assets::Texture* resolveTexture(assets::AssetManager& am, const json& jt) {
    if (!jt.is_object()) return nullptr;

    const std::string kind = jt.value("kind", std::string());
    const std::string name = jt.value("name", std::string());

    if (kind == "solid") {
        uint8_t def[4] = {255, 255, 255, 255};
        uint8_t c[4];
        colorFromJson(jt.contains("color") ? jt["color"] : json(nullptr), c, def);
        return am.solid(name.empty() ? "solid" : name, c);
    }
    if (kind == "checker") {
        uint8_t defA[4] = {0, 0, 0, 255};
        uint8_t defB[4] = {255, 255, 255, 255};
        uint8_t a[4], b[4];
        colorFromJson(jt.contains("colorA") ? jt["colorA"] : json(nullptr), a,
                      defA);
        colorFromJson(jt.contains("colorB") ? jt["colorB"] : json(nullptr), b,
                      defB);
        const uint32_t cell =
            static_cast<uint32_t>(jt.value("cell", 32));
        const uint32_t size =
            static_cast<uint32_t>(jt.value("size", 512));
        const bool srgb = jt.value("srgb", true);
        return am.checker(name.empty() ? "checker" : name, a, b, cell, size,
                          srgb);
    }
    if (kind == "file") {
        const std::string path = jt.value("path", std::string());
        if (path.empty()) return nullptr;
        const bool srgb = jt.value("srgb", true);
        const std::string key = name.empty() ? path : name;
        try {
            return am.loadTexture(assets::resolveAssetPath(path), key, srgb);
        } catch (const std::exception& ex) {
            VK_LOG_WARN("SceneSerializer: texture load failed (%s): %s",
                        path.c_str(), ex.what());
            return nullptr;
        }
    }
    return nullptr;
}

// ============================================================
// 材质来源
// ============================================================

// ---- 覆盖项（W0-3 / W1-4）----
//
// 只写"与基准不同的字段"。基准有两种：
//   · Model 材质  → 导入时的原始参数（`Material::importDefaults`）
//   · Instance 材质 → 母材质（W1）
// 存全量会有一个隐蔽后果：母材质/模型改了以后，所有实例的对应字段都
// "值 ≠ 基准"，于是**全部被当成覆盖** → 改母材质不生效。这就是
// `world_scale_roadmap` D2 与 `persistence_roadmap` D3 的同一条道理。
const char* alphaModeToStr(assets::Material::AlphaMode m) {
    switch (m) {
        case assets::Material::AlphaMode::Blend: return "blend";
        case assets::Material::AlphaMode::Mask: return "mask";
        case assets::Material::AlphaMode::Opaque: break;
    }
    return "opaque";
}

json overridesToJson(const assets::Material::Params& p, std::uint32_t bits) {
    json j = json::object();
    using M = assets::Material;
    if (bits & M::kOvBaseColor) j["baseColor"] = vec4ToJson(p.baseColorFactor);
    if (bits & M::kOvMetallic) j["metallic"] = p.metallic;
    if (bits & M::kOvRoughness) j["roughness"] = p.roughness;
    if (bits & M::kOvNormalScale) j["normalScale"] = p.normalScale;
    if (bits & M::kOvAo) j["ao"] = p.ao;
    if (bits & M::kOvEmissive) j["emissive"] = vec3ToJson(p.emissive);
    if (bits & M::kOvAlphaMode) j["alphaMode"] = alphaModeToStr(p.alphaMode);
    if (bits & M::kOvAlphaCutoff) j["alphaCutoff"] = p.alphaCutoff;
    if (bits & M::kOvDitherAlpha) j["ditherAlpha"] = p.ditherAlpha;
    if (bits & M::kOvShadingModel)
        j["shadingModel"] =
            (p.shadingModel == M::ShadingModel::Skin) ? "skin" : "default";
    if (bits & M::kOvDoubleSided) j["doubleSided"] = p.doubleSided;
    if (bits & M::kOvHeightScale) j["heightScale"] = p.heightScale;
    return j;
}

std::uint32_t overridesFromJson(const json& jo, assets::Material::Params& p) {
    std::uint32_t bits = 0;
    if (!jo.is_object()) return bits;
    using M = assets::Material;
    if (jo.contains("baseColor")) {
        p.baseColorFactor = vec4FromJson(jo["baseColor"], p.baseColorFactor);
        bits |= M::kOvBaseColor;
    }
    if (jo.contains("metallic")) {
        p.metallic = jo.value("metallic", p.metallic);
        bits |= M::kOvMetallic;
    }
    if (jo.contains("roughness")) {
        p.roughness = jo.value("roughness", p.roughness);
        bits |= M::kOvRoughness;
    }
    if (jo.contains("normalScale")) {
        p.normalScale = jo.value("normalScale", p.normalScale);
        bits |= M::kOvNormalScale;
    }
    if (jo.contains("ao")) {
        p.ao = jo.value("ao", p.ao);
        bits |= M::kOvAo;
    }
    if (jo.contains("emissive")) {
        p.emissive = vec3FromJson(jo["emissive"], p.emissive);
        bits |= M::kOvEmissive;
    }
    if (jo.contains("alphaMode")) {
        const std::string s = jo.value("alphaMode", std::string("opaque"));
        p.alphaMode = (s == "blend")   ? M::AlphaMode::Blend
                      : (s == "mask")  ? M::AlphaMode::Mask
                                       : M::AlphaMode::Opaque;
        bits |= M::kOvAlphaMode;
    }
    if (jo.contains("alphaCutoff")) {
        p.alphaCutoff = jo.value("alphaCutoff", p.alphaCutoff);
        bits |= M::kOvAlphaCutoff;
    }
    if (jo.contains("ditherAlpha")) {
        p.ditherAlpha = jo.value("ditherAlpha", p.ditherAlpha);
        bits |= M::kOvDitherAlpha;
    }
    if (jo.contains("shadingModel")) {
        p.shadingModel = (jo.value("shadingModel", std::string()) == "skin")
                             ? M::ShadingModel::Skin
                             : M::ShadingModel::Default;
        bits |= M::kOvShadingModel;
    }
    if (jo.contains("doubleSided")) {
        p.doubleSided = jo.value("doubleSided", p.doubleSided);
        bits |= M::kOvDoubleSided;
    }
    if (jo.contains("heightScale")) {
        p.heightScale = jo.value("heightScale", p.heightScale);
        bits |= M::kOvHeightScale;
    }
    return bits;
}

// 只写"用户手工换上"的纹理槽（带 TextureSource 的那些）。
// 模型/贴图资产自带的那几张没有 source，写出来是 null，读回来不该覆盖它们。
json materialTextureOverrides(const assets::Material* mat) {
    if (!mat) return json::object();
    json tex = json::object();
    auto put = [&tex](const char* key, assets::Texture* t) {
        json s = textureSourceToJson(t);
        if (!s.is_null()) tex[key] = s;
    };
    put("albedo", mat->albedoMap);
    put("normal", mat->normalMap);
    put("orm", mat->ormMap);
    put("roughnessMap", mat->roughnessMap);
    put("metallicMap", mat->metallicMap);
    put("emissiveMap", mat->emissiveMap);
    put("heightMap", mat->heightMap);
    return tex;
}

json materialToJson(const assets::Material* mat) {
    if (!mat) return nullptr;

    json j;
    j["name"] = mat->name;
    // W0-2：稳定身份。**所有 kind 都写** —— 读端先按 id 找，
    // 找不到再按 name 回落，这样"模型重导出改了材质顺序"不会静默错位。
    if (!mat->id.empty()) j["id"] = mat->id;

    // 实例（W1-4）：只写母材质 id + 覆盖项，不写全量参数
    if (mat->source.kind == assets::MaterialSource::Kind::Instance) {
        j["kind"] = "instance";
        j["parent"] = mat->parent ? (mat->parent->id.empty() ? mat->parent->name
                                                             : mat->parent->id)
                                  : std::string();
        j["overrides"] =
            overridesToJson(mat->captureParams(), mat->overridden);
        j["alphaBlend"] = (mat->alphaMode == assets::Material::AlphaMode::Blend);
        return j;
    }

    // 材质资产（W2-1）：只写"引用哪个 .vmat" + 用户改过的字段。
    // 参数本身住在 .vmat 里 —— 这就是"改一次 .vmat，两个关卡都变"的机制。
    if (mat->source.kind == assets::MaterialSource::Kind::VMAT) {
        j["kind"] = "vmat";
        j["path"] = mat->source.vmatPath;
        assets::Material::Params cur = mat->captureParams();
        const std::uint32_t bits =
            mat->hasImportDefaults
                ? assets::Material::diffParams(mat->importDefaults, cur)
                : 0u;
        if (bits) j["overrides"] = overridesToJson(cur, bits);
        json tex = materialTextureOverrides(mat);
        if (!tex.empty()) j["overridesTex"] = tex;
        return j;
    }

    // glTF/FBX 派生的材质：记"来自哪个模型" + 用户改过的那几个字段
    if (mat->source.kind == assets::MaterialSource::Kind::Model) {
        j["kind"] = "model";
        j["modelKey"] = mat->source.modelKey;
        // ⚠ 以前这里直接 `return j`，于是编辑器里对导入模型材质做的任何
        //   修改**存盘即丢**，重开还会因为找不到而变成 null（物体变无材质）。
        std::uint32_t bits = 0;
        assets::Material::Params cur = mat->captureParams();
        if (mat->hasImportDefaults)
            bits = assets::Material::diffParams(mat->importDefaults, cur);
        if (bits) j["overrides"] = overridesToJson(cur, bits);
        // 纹理槽：只有"用户手工换上的"（带 TextureSource 的）才写 ——
        // 模型自带的那几张没有 source，写出来是 null，读回来不该覆盖。
        json tex = materialTextureOverrides(mat);
        if (!tex.empty()) j["overridesTex"] = tex;
        return j;
    }

    j["kind"] = "procedural";
    j["baseColor"] = vec4ToJson(mat->baseColorFactor);
    j["metallic"] = mat->metallic;
    j["roughness"] = mat->roughness;
    j["normalScale"] = mat->normalScale;
    j["ao"] = mat->ao;
    j["emissive"] = vec3ToJson(mat->emissive);
    j["doubleSided"] = mat->doubleSided;
    // 透明模式：写字符串（Opaque / Blend / Mask）。**同时保留旧的 alphaBlend
    // 布尔**——老场景文件只有它，读的时候要能兼容（见 resolveMaterial）。
    j["alphaMode"] = (mat->alphaMode == assets::Material::AlphaMode::Blend)
                         ? "blend"
                         : (mat->alphaMode == assets::Material::AlphaMode::Mask)
                               ? "mask"
                               : "opaque";
    j["alphaBlend"] = (mat->alphaMode == assets::Material::AlphaMode::Blend);
    j["alphaCutoff"] = mat->alphaCutoff;
    j["ditherAlpha"] = mat->ditherAlpha;
    // 着色模型（R4-2）：写字符串。"default" 不写也能读（见 resolveMaterial），
    // 但显式写出让人一眼看得出这个材质走哪条漫反射路径。
    j["shadingModel"] =
        (mat->shadingModel == assets::Material::ShadingModel::Skin) ? "skin"
                                                                   : "default";
    j["albedo"] = textureSourceToJson(mat->albedoMap);
    j["normal"] = textureSourceToJson(mat->normalMap);
    j["orm"] = textureSourceToJson(mat->ormMap);
    // 单因子贴图槽（白/白/黑 是"未绑定"的中性值，各自与因子相乘或相加）
    j["roughnessMap"] = textureSourceToJson(mat->roughnessMap);
    j["metallicMap"] = textureSourceToJson(mat->metallicMap);
    j["emissiveMap"] = textureSourceToJson(mat->emissiveMap);
    // 视差遮挡贴图（R4-3）：高度图 + 深度尺度。高度图缺席 = 这个材质
    // 走非 PARALLAX 变体（逐像素与引入前相同），heightScale 也就不参与。
    j["heightMap"] = textureSourceToJson(mat->heightMap);
    j["heightScale"] = mat->heightScale;
    return j;
}

assets::Material* resolveMaterial(assets::AssetManager& am, const json& jm) {
    if (!jm.is_object()) return nullptr;

    const std::string kind = jm.value("kind", std::string("procedural"));
    const std::string name = jm.value("name", std::string("material"));
    const std::string id = jm.value("id", std::string());

    // ---- 手工换上的纹理（W0-3 / W1-4）----
    auto applyTexOverrides = [&am](assets::Material* m, const json& jo) {
        if (!m || !jo.is_object()) return;
        auto get = [&](const char* k) -> assets::Texture* {
            return jo.contains(k) ? resolveTexture(am, jo[k]) : nullptr;
        };
        if (auto* t = get("albedo")) m->albedoMap = t;
        if (auto* t = get("normal")) m->normalMap = t;
        if (auto* t = get("orm")) m->ormMap = t;
        if (auto* t = get("roughnessMap")) m->roughnessMap = t;
        if (auto* t = get("metallicMap")) m->metallicMap = t;
        if (auto* t = get("emissiveMap")) m->emissiveMap = t;
        if (auto* t = get("heightMap")) m->heightMap = t;
    };

    // ---- 实例（W1-4）----
    if (kind == "instance") {
        const std::string parentId = jm.value("parent", std::string());
        assets::Material* p = am.findMaterialById(parentId);
        if (!p && !parentId.empty()) p = am.findMaterial(parentId);
        assets::Material* m = nullptr;
        if (p) {
            m = am.makeInstanceOf(p, name);
        } else {
            // ⚠ 母材质找不到（模型文件删了 / .vmat 改名了）**不能变 null**：
            //   退化成独立材质，overrides 照样应用 —— 关卡里的东西还在，
            //   只是没了共享关系。
            VK_LOG_WARN("ASSET-MAT-MISS parent=%s name=%s reason=parent-not-found",
                        assets::assetLogSafe(parentId).c_str(),
                        assets::assetLogSafe(name).c_str());
            m = am.makeMaterialPBR(name, nullptr, nullptr, nullptr,
                                   glm::vec4(1.0f), 0.5f, 0.0f, glm::vec3(0.0f),
                                   false);
        }
        if (!m) return nullptr;
        assets::Material::Params pp = m->captureParams();
        const std::uint32_t bits = overridesFromJson(
            jm.contains("overrides") ? jm["overrides"] : json::object(), pp);
        m->applyParams(pp);
        m->overridden = bits;
        applyTexOverrides(m, jm.contains("overridesTex") ? jm["overridesTex"]
                                                         : json::object());
        // 老字段兼容：alphaBlend 布尔
        if (jm.value("alphaBlend", false))
            m->alphaMode = assets::Material::AlphaMode::Blend;
        return m;
    }

    // ---- 材质资产文件（W2-1）----
    // `assets/materials/*.vmat` 的字段与场景里的 `procedural` 块**完全相同**，
    // 所以这里直接递归回自己：读文件 → 把 doc 当成 procedural 块解析。
    // 一套解析器两处用，是这条最省的地方（多一套就会有两套 bug）。
    if (kind == "vmat") {
        const std::string relPath = jm.value("path", std::string());
        // 先按 id 找已解析过的（300 个物体引用同一个 .vmat 不该解析 300 次）
        if (!id.empty()) {
            if (assets::Material* cached = am.findMaterialById(id)) {
                if (jm.contains("overrides")) {
                    assets::Material::Params pp = cached->captureParams();
                    const std::uint32_t bits =
                        overridesFromJson(jm["overrides"], pp);
                    cached->applyParams(pp);
                    cached->overridden |= bits;
                }
                if (jm.contains("overridesTex"))
                    applyTexOverrides(cached, jm["overridesTex"]);
                return cached;
            }
        }
        json doc;
        bool loaded = false;
        if (!relPath.empty()) {
            std::ifstream in(assets::resolveAssetPath(relPath),
                             std::ios::binary);
            if (in) {
                try {
                    in >> doc;
                    loaded = doc.is_object();
                } catch (const std::exception& ex) {
                    VK_LOG_WARN("SceneSerializer: .vmat parse failed (%s): %s",
                                relPath.c_str(), ex.what());
                }
            }
        }
        if (!loaded) {
            VK_LOG_WARN("ASSET-MAT-MISS id=%s path=%s reason=vmat-not-found",
                        assets::assetLogSafe(id).c_str(),
                        assets::assetLogSafe(relPath).c_str());
            const std::string missName =
                (relPath.empty() ? std::string("vmat") : relPath) + "#missing";
            assets::Material* m = am.findMaterial(missName);
            if (!m)
                m = am.makeMaterialPBR(missName, nullptr, nullptr, nullptr,
                                       glm::vec4(1.0f), 0.5f, 0.0f,
                                       glm::vec3(0.0f), false);
            if (m) am.assignMaterialId(m, missName);
            return m;
        }

        // .vmat 自己的 id 优先（这就是"材质资产可以被多个关卡共享"的锚点）
        const std::string fileId = doc.value("id", std::string());
        const std::string wantId =
            !fileId.empty() ? fileId : (!id.empty() ? id : relPath);
        if (!doc.contains("name")) doc["name"] = wantId;
        doc["kind"] = "procedural";

        assets::Material* m = resolveMaterial(am, doc);
        if (m) {
            m->source.kind = assets::MaterialSource::Kind::VMAT;
            m->source.vmatPath = relPath;
            am.assignMaterialId(m, wantId);
            m->captureImportDefaults();
            VK_LOG_INFO("ASSET-VMAT path=%s id=%s name=%s",
                        assets::assetLogSafe(relPath).c_str(),
                        assets::assetLogSafe(wantId).c_str(),
                        assets::assetLogSafe(m->name).c_str());
            if (jm.contains("overrides")) {
                assets::Material::Params pp = m->captureParams();
                const std::uint32_t bits =
                    overridesFromJson(jm["overrides"], pp);
                m->applyParams(pp);
                m->overridden |= bits;
            }
            if (jm.contains("overridesTex"))
                applyTexOverrides(m, jm["overridesTex"]);
        }
        return m;
    }

    if (kind == "model") {
        assets::Material* m = nullptr;
        // 1) 先按**稳定 id** 找（W0-2：模型重导出改了材质顺序也能对上）
        if (!id.empty()) m = am.findMaterialById(id);
        // 2) 再按缓存键 name 回落（兼容没有 id 的老场景文件）
        if (!m) m = am.findMaterial(name);

        if (!m) {
            // 3) 兜底：实体只有材质没有网格时，这里补一次模型加载
            const std::string modelKey = jm.value("modelKey", std::string());
            if (!modelKey.empty()) {
                try {
                    const std::string abs = assets::resolveAssetPath(modelKey);
                    // 按扩展名分派 —— FBX 走 loadGLTF 会抛异常，
                    // 然后被静默当成"没有材质"（这正是 F3 的第二层）
                    const bool fbx =
                        abs.size() > 4 &&
                        (abs.compare(abs.size() - 4, 4, ".fbx") == 0 ||
                         abs.compare(abs.size() - 4, 4, ".FBX") == 0);
                    if (fbx)
                        am.loadFBX(abs, modelKey);
                    else
                        am.loadGLTF(abs, modelKey);
                } catch (const std::exception& ex) {
                    VK_LOG_WARN(
                        "SceneSerializer: model reload for material failed (%s): %s",
                        modelKey.c_str(), ex.what());
                }
                if (!id.empty()) m = am.findMaterialById(id);
                if (!m) m = am.findMaterial(name);
            }
        }

        if (!m) {
            // ⚠ 以前这里 `return m`（= nullptr），后果是**物体直接消失**：
            //   下方 skip 判定 `!mesh && !mat && !hasNonMeshContent` 会把它
            //   排除出渲染列表，一个日志都没有。现在退化成占位材质（默认白），
            //   并且**打日志**。
            VK_LOG_WARN("ASSET-MAT-MISS id=%s name=%s reason=not-found",
                        assets::assetLogSafe(id).c_str(),
                        assets::assetLogSafe(name).c_str());
            // 占位用 `#missing` 后缀：既不污染模型真正的缓存键
            // （否则以后模型加载成功会被冲突改名），又保证同一份文件读两次
            // 落到同一个占位材质上。
            const std::string missName = name + "#missing";
            m = am.findMaterial(missName);
            if (!m) {
                m = am.makeMaterialPBR(missName, nullptr, nullptr, nullptr,
                                       glm::vec4(1.0f), 0.5f, 0.0f,
                                       glm::vec3(0.0f), false);
                if (m) {
                    const std::string modelKey =
                        jm.value("modelKey", std::string());
                    if (!modelKey.empty()) m->source.setModel(modelKey);
                    am.assignMaterialId(m, missName);
                    // 基准 = 占位材质的默认参数。有了它，overrides 才能
                    // 被算成"差值"再写回去（否则本次读进来的覆盖会在
                    // 下一次存盘时丢掉）。
                    m->captureImportDefaults();
                }
            }
        }

        // 4) 应用用户改过的字段（W0-3 的核心）
        if (m && jm.contains("overrides")) {
            assets::Material::Params pp = m->captureParams();
            const std::uint32_t bits =
                overridesFromJson(jm["overrides"], pp);
            m->applyParams(pp);
            m->overridden |= bits;
        }
        if (m && jm.contains("overridesTex"))
            applyTexOverrides(m, jm["overridesTex"]);
        return m;
    }

    glm::vec4 baseColor(1.0f);
    glm::vec3 emissive(0.0f);
    if (jm.contains("baseColor")) baseColor = vec4FromJson(jm["baseColor"], baseColor);
    if (jm.contains("emissive")) emissive = vec3FromJson(jm["emissive"], emissive);

    const float metallic = jm.value("metallic", 0.0f);
    const float roughness = jm.value("roughness", 0.5f);
    const float normalScale = jm.value("normalScale", 1.0f);
    const float ao = jm.value("ao", 1.0f);
    const bool doubleSided = jm.value("doubleSided", false);

    assets::Texture* albedo = resolveTexture(
        am, jm.contains("albedo") ? jm["albedo"] : json(nullptr));
    assets::Texture* normal = resolveTexture(
        am, jm.contains("normal") ? jm["normal"] : json(nullptr));
    assets::Texture* orm =
        resolveTexture(am, jm.contains("orm") ? jm["orm"] : json(nullptr));
    assets::Texture* roughnessMap = resolveTexture(
        am, jm.contains("roughnessMap") ? jm["roughnessMap"] : json(nullptr));
    assets::Texture* metallicMap = resolveTexture(
        am, jm.contains("metallicMap") ? jm["metallicMap"] : json(nullptr));
    assets::Texture* emissiveMap = resolveTexture(
        am, jm.contains("emissiveMap") ? jm["emissiveMap"] : json(nullptr));
    // 高度图（R4-3 POM）。readTexture 用同一个 resolveTexture，
    // 所以 checker/solid/file 三种来源都能往返。
    assets::Texture* heightMap = resolveTexture(
        am, jm.contains("heightMap") ? jm["heightMap"] : json(nullptr));

    assets::Material* mat = am.makeMaterialPBR(
        name, albedo, normal, orm, baseColor, roughness, metallic, emissive,
        doubleSided);
    if (mat) {
        // makeMaterialPBR 未覆盖这几项，补齐以保证往返一致
        mat->roughnessMap = roughnessMap;
        mat->metallicMap = metallicMap;
        mat->emissiveMap = emissiveMap;
        mat->heightMap = heightMap;
        mat->heightScale = jm.value("heightScale", 0.05f);
        mat->normalScale = normalScale;
        mat->ao = ao;
        mat->alphaMode = jm.value("alphaBlend", false)
                             ? assets::Material::AlphaMode::Blend
                             : assets::Material::AlphaMode::Opaque;
        // alphaMode 字符串优先（新格式）。缺席时退回上面的 alphaBlend 布尔
        // —— 老场景文件没有这个字段，不能因此把 Mask/Blend 读丢。
        const std::string am = jm.value("alphaMode", std::string());
        if (am == "blend") {
            mat->alphaMode = assets::Material::AlphaMode::Blend;
        } else if (am == "mask") {
            mat->alphaMode = assets::Material::AlphaMode::Mask;
        } else if (am == "opaque") {
            mat->alphaMode = assets::Material::AlphaMode::Opaque;
        }
        mat->alphaCutoff = jm.value("alphaCutoff", 0.5f);
        mat->ditherAlpha = jm.value("ditherAlpha", false);
        // 着色模型（R4-2）。缺席 = 老场景 = Default（逐像素不变）。
        mat->shadingModel =
            (jm.value("shadingModel", std::string()) == "skin")
                ? assets::Material::ShadingModel::Skin
                : assets::Material::ShadingModel::Default;
    }
    return mat;
}

// ============================================================
// 网格来源
// ============================================================

// ---- 内置图元的生成参数（编辑器可回改的那一份，见 ecs::MeshComponent）----
//
// 注意这里和 MeshSource 是**两回事**：
//   * MeshSource 描述"这颗 mesh 是哪来的"，序列化后靠它重建几何；
//   * MeshComponent::primitive 是编辑器在几何之上记的一份意图，
//     Inspector 拿它决定"这个物体该显示哪些参数栏"。
// 两者在正常流程里是同步的（改参数时两个一起改），但分开存 ——
// 万一 mesh 被换成别的东西（比如外部模型），参数栏就跟着消失了。

const char* primitiveKindToStr(ecs::MeshPrimitive k) {
    switch (k) {
        case ecs::MeshPrimitive::Cube: return "cube";
        case ecs::MeshPrimitive::Plane: return "plane";
        case ecs::MeshPrimitive::Sphere: return "sphere";
        case ecs::MeshPrimitive::Cylinder: return "cylinder";
        case ecs::MeshPrimitive::None: break;
    }
    return "none";
}

ecs::MeshPrimitive primitiveKindFromStr(const std::string& s) {
    if (s == "cube") return ecs::MeshPrimitive::Cube;
    if (s == "plane") return ecs::MeshPrimitive::Plane;
    if (s == "sphere") return ecs::MeshPrimitive::Sphere;
    if (s == "cylinder") return ecs::MeshPrimitive::Cylinder;
    return ecs::MeshPrimitive::None;
}

json primitiveParamsToJson(const ecs::MeshComponent& mc) {
    const ecs::MeshPrimitiveParams& p = mc.params;
    if (!p.isBuiltin()) return nullptr;
    json j;
    j["kind"] = primitiveKindToStr(p.primitive);
    j["size"] = p.size;
    j["radius"] = p.radius;
    j["segments"] = p.segments;
    j["rings"] = p.rings;
    return j;
}

void primitiveParamsFromJson(ecs::MeshComponent& mc, const json& jp) {
    if (!jp.is_object()) return;
    ecs::MeshPrimitiveParams& p = mc.params;
    p.primitive = primitiveKindFromStr(jp.value("kind", std::string("none")));
    p.size = jp.value("size", p.size);
    p.radius = jp.value("radius", p.radius);
    p.segments = jp.value("segments", p.segments);
    p.rings = jp.value("rings", p.rings);
}

json meshSourceToJson(const assets::Mesh* mesh) {
    if (!mesh) return nullptr;
    const assets::MeshSource& s = mesh->source();

    json j;
    switch (s.kind) {
        case assets::MeshSource::Kind::Builtin:
            j["kind"] = "builtin";
            j["shape"] = s.shape;
            j["size"] = s.size;
            // 细分参数：改了图元生成参数的网格，缓存键里带着这组值，
            // 重建时必须原样喂回去（0 = 用生成器默认值）
            if (s.segments > 0) j["segments"] = s.segments;
            if (s.rings > 0) j["rings"] = s.rings;
            j["name"] = s.name;
            return j;
        case assets::MeshSource::Kind::OBJ:
            j["kind"] = "obj";
            j["path"] = s.path;
            j["name"] = s.name;
            return j;
        case assets::MeshSource::Kind::GLTF:
            j["kind"] = "gltf";
            j["path"] = s.path;
            j["modelKey"] = s.name;  // GLTF 下 MeshSource.name 存模型键
            j["subMeshIndex"] = s.subMeshIndex;
            return j;
        case assets::MeshSource::Kind::FBX:
            j["kind"] = "fbx";
            j["path"] = s.path;
            j["modelKey"] = s.name;  // FBX 下 MeshSource.name 存模型键
            j["subMeshIndex"] = s.subMeshIndex;
            return j;
        default:
            // Unknown：没有来源信息就无法重建，调用方会计入 skipped
            return nullptr;
    }
}

assets::Mesh* resolveMesh(assets::AssetManager& am, const json& jm) {
    if (!jm.is_object()) return nullptr;

    const std::string kind = jm.value("kind", std::string());

    if (kind == "builtin") {
        const std::string shape = jm.value("shape", std::string("cube"));
        const float size = jm.value("size", 1.0f);
        const std::string name =
            jm.value("name", shape.empty() ? std::string("mesh") : shape);
        // 细分参数：省略时用生成器的默认档（老场景文件里没有这两个字段）
        const int segments = jm.value("segments", 48);
        const int rings = jm.value("rings", 24);
        if (shape == "cube") return am.cube(size, name);
        if (shape == "plane") return am.plane(size, name);
        if (shape == "sphere") return am.sphere(size, name, segments, rings);
        if (shape == "cylinder") return am.cylinder(size, name, segments);
        // 植被（2026-10-20）：撒点生成的实例是普通实体，所以它们必须
        // 能被重建 —— 否则"存盘再打开，植被全没了"，而日志一条错都没有。
        if (shape == "grass") return am.grass(size, name);
        if (shape == "tree") return am.tree(size, name);
        if (shape == "flower") return am.flower(size, name);
        if (shape == "rock") return am.rock(size, name);
        VK_LOG_WARN("SceneSerializer: unknown builtin shape '%s'", shape.c_str());
        return nullptr;
    }

    if (kind == "obj") {
        const std::string path = jm.value("path", std::string());
        if (path.empty()) return nullptr;
        const std::string name = jm.value("name", path);
        try {
            return am.loadModel(assets::resolveAssetPath(path), name);
        } catch (const std::exception& ex) {
            VK_LOG_WARN("SceneSerializer: OBJ load failed (%s): %s",
                        path.c_str(), ex.what());
            return nullptr;
        }
    }

    if (kind == "gltf") {
        const std::string path = jm.value("path", std::string());
        if (path.empty()) return nullptr;
        const std::string modelKey = jm.value("modelKey", path);
        const int sub = jm.value("subMeshIndex", -1);
        try {
            assets::Model* model =
                am.loadGLTF(assets::resolveAssetPath(path), modelKey);
            if (model && sub >= 0 &&
                sub < static_cast<int>(model->subMeshes.size())) {
                return model->subMeshes[static_cast<size_t>(sub)].mesh;
            }
            VK_LOG_WARN("SceneSerializer: glTF submesh %d out of range (%s)", sub,
                        path.c_str());
        } catch (const std::exception& ex) {
            VK_LOG_WARN("SceneSerializer: glTF load failed (%s): %s",
                        path.c_str(), ex.what());
        }
        return nullptr;
    }

    if (kind == "fbx") {
        const std::string path = jm.value("path", std::string());
        if (path.empty()) return nullptr;
        const std::string modelKey = jm.value("modelKey", path);
        const int sub = jm.value("subMeshIndex", -1);
        try {
            assets::Model* model =
                am.loadFBX(assets::resolveAssetPath(path), modelKey);
            if (model && sub >= 0 &&
                sub < static_cast<int>(model->subMeshes.size())) {
                return model->subMeshes[static_cast<size_t>(sub)].mesh;
            }
            VK_LOG_WARN("SceneSerializer: FBX submesh %d out of range (%s)", sub,
                        path.c_str());
        } catch (const std::exception& ex) {
            VK_LOG_WARN("SceneSerializer: FBX load failed (%s): %s",
                        path.c_str(), ex.what());
        }
        return nullptr;
    }

    return nullptr;
}

// ============================================================
// 相机 / 光照
// ============================================================

json cameraToJson(const Camera& cam) {
    return json{
        {"target", vec3ToJson(cam.target())},
        {"yaw", cam.yaw()},
        {"pitch", cam.pitch()},
        {"distance", cam.distance()},
        {"fov", cam.fov()},
    };
}

void applyCamera(Camera& cam, const json& jc) {
    if (!jc.is_object()) return;
    // ⚠ 顺序有讲究：编辑器相机是**飞行模式**（applyEditorCameraMode），
    //   yaw/pitch/distance 的 setter 都会从"当前 target"反推相机位置；
    //   如果先设 target，后面三个 setter 一同步 target 就被旧位置盖掉 ——
    //   表现为"场景 JSON 里的相机没生效，画面看向莫名其妙的地方"。
    //   target 必须**最后**设（把相机精确钉到 target + dir*distance）。

    float yaw = jc.value("yaw", cam.yaw());
    float pitch = jc.value("pitch", cam.pitch());

    // ------------------------------------------------------------
    // 单位嗅探：pitch 写成"度"的历史存档（2026-10-21 修）
    // ------------------------------------------------------------
    // 本文件的写入方 cameraToJson 存的是**弧度**，Camera::pitch 的合法区间
    // 也只有 ±89°（kPitchLimit ≈ 1.5533 rad）—— 于是"|pitch| 超出这个区间"
    // 就只可能有一个原因：它是按度写的。
    //
    // 真实事故（用户原话："选择创建第三人称视角的游戏创建后，进去的初始化
    // 游戏画面视口为什么是一个视角朝上的（朝向 Z 正方向的），并且是处于地面
    // 栅格平面之下的一个位置"）：模板写的是 `"pitch": -18.0`（度），这里
    // 当成 -18 **弧度** 读，setPitch 一夹就成了 -89° —— 相机沉到地面以下、
    // 几乎垂直向上看。yaw 一起换算，否则画面对不上。
    //
    // 也接受显式的 `"angleUnit": "deg"`：新场景用不着（gen_templates 已经
    // 改成写弧度），但手写 JSON 的人可以不用赌这条启发式。
    const bool explicitDeg =
        jc.contains("angleUnit") && jc["angleUnit"].is_string() &&
        jc["angleUnit"].get<std::string>() == "deg";
    const bool outOfRange = std::fabs(pitch) > Camera::kPitchLimit * 1.001f;
    if (explicitDeg || outOfRange) {
        const float yawDeg = yaw, pitchDeg = pitch;
        yaw = glm::radians(yaw);
        pitch = glm::radians(pitch);
        VK_LOG_WARN("SCENE-CAMERA-UNIT 角度按度读入（yaw=%.2f pitch=%.2f deg）"
                    " → rad (%.3f, %.3f)",
                    static_cast<double>(yawDeg), static_cast<double>(pitchDeg),
                    static_cast<double>(yaw), static_cast<double>(pitch));
    }

    cam.setYaw(yaw);
    cam.setPitch(pitch);
    cam.setDistance(jc.value("distance", cam.distance()));
    cam.setTarget(vec3FromJson(
        jc.contains("target") ? jc["target"] : json(nullptr), cam.target()));
    cam.setFov(jc.value("fov", cam.fov()));

    // 一行可断言的落位（脚本用它验证"相机在地面之上、俯视而不是仰视"）
    const glm::vec3 eye = cam.position();
    VK_LOG_INFO("SCENE-CAMERA eye=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f) "
                "yaw=%.2f pitch=%.2f dist=%.2f",
                static_cast<double>(eye.x), static_cast<double>(eye.y),
                static_cast<double>(eye.z),
                static_cast<double>(cam.target().x),
                static_cast<double>(cam.target().y),
                static_cast<double>(cam.target().z),
                static_cast<double>(cam.yaw()),
                static_cast<double>(cam.pitch()),
                static_cast<double>(cam.distance()));

    // ------------------------------------------------------------
    // 自证：相机不该在栅格面之下
    // ------------------------------------------------------------
    // 2026-10-21（用户原话："为什么你创建的 3d 视角总是在栅格面底下朝上仰视
    // 看的，就很怪"）：`pitch` 的符号是**正 = 相机在目标上方**，存档里写成
    // 负数就等于把相机摆到地面以下往上看。这类错误渲染完全正常（没有任何
    // VUID），只能靠"看着怪"发现 —— 所以在这里留一条可抓的告警：
    //   · pitch < 0                → 相机在目标下方（仰视）
    //   · eye.z <= 0               → 相机真的沉到栅格面以下了
    // 注意判据是 z<=0 而不是 z<0：贴地（z 恰好 0）时画面同样是从地面往里看，
    // 一样是坏的初始视角。
    if (cam.pitch() < 0.0f || eye.z <= 0.0f) {
        VK_LOG_WARN("SCENE-CAMERA-LOW eye.z=%.2f pitch=%.2f (%s) —— 相机在"
                    "栅格面之下/仰视，检查场景 camera.pitch 的符号与单位"
                    "（正 = 在上方俯视；用弧度写，度会触发单位嗅探）",
                    static_cast<double>(eye.z),
                    static_cast<double>(cam.pitch()),
                    cam.pitch() < 0.0f ? "仰视" : "贴地");
    }
}

// ============================================================
// 后处理体积（scene/PostVolume.h）
// ============================================================

json postVolumeToJson(const PostVolume& v) {
    return json{
        {"name", v.name},
        {"boundsMin", vec3ToJson(v.boundsMin)},
        {"boundsMax", vec3ToJson(v.boundsMax)},
        {"priority", v.priority},
        {"blendDistance", v.blendDistance},
        {"dofFocus", v.dofFocus},
        {"dofAperture", v.dofAperture},
        {"dofMaxRadius", v.dofMaxRadius},
        {"motionBlurStrength", v.motionBlurStrength},
        {"chromaStrength", v.chromaStrength},
        {"vignetteStrength", v.vignetteStrength},
        {"dof", v.dof},
        {"motionBlur", v.motionBlur},
        {"chromatic", v.chromatic},
        {"vignette", v.vignette},
    };
}

PostVolume postVolumeFromJson(const json& j) {
    PostVolume v;
    if (!j.is_object()) return v;
    v.name = j.value("name", std::string());
    v.boundsMin = vec3FromJson(j.contains("boundsMin") ? j["boundsMin"]
                                                      : json(nullptr),
                               v.boundsMin);
    v.boundsMax = vec3FromJson(j.contains("boundsMax") ? j["boundsMax"]
                                                      : json(nullptr),
                               v.boundsMax);
    v.priority = j.value("priority", v.priority);
    v.blendDistance = j.value("blendDistance", v.blendDistance);
    // ⚠ 缺省值必须是 kNoOverride 而不是结构体默认 —— 用 j.value("x", v.x)
    //   写法时，JSON 里没写的项本来就是 v.x（= -1 = 不覆盖），语义正合；
    //   这里显式写出来是为了让"没写 = 不覆盖"这条约定在源码里看得见。
    v.dofFocus = j.value("dofFocus", v.dofFocus);
    v.dofAperture = j.value("dofAperture", v.dofAperture);
    v.dofMaxRadius = j.value("dofMaxRadius", v.dofMaxRadius);
    v.motionBlurStrength = j.value("motionBlurStrength", v.motionBlurStrength);
    v.chromaStrength = j.value("chromaStrength", v.chromaStrength);
    v.vignetteStrength = j.value("vignetteStrength", v.vignetteStrength);
    v.dof = j.value("dof", v.dof);
    v.motionBlur = j.value("motionBlur", v.motionBlur);
    v.chromatic = j.value("chromatic", v.chromatic);
    v.vignette = j.value("vignette", v.vignette);
    return v;
}

json lightToJson(const Light& L) {
    return json{
        {"direction", vec3ToJson(L.direction)},
        {"color", vec3ToJson(L.color)},
        {"intensity", L.intensity},
        {"castsShadow", L.castsShadow},
        {"ambientScale", L.ambientScale},
    };
}

void applyLight(Light& L, const json& jl) {
    if (!jl.is_object()) return;
    L.direction = vec3FromJson(
        jl.contains("direction") ? jl["direction"] : json(nullptr), L.direction);
    L.color = vec3FromJson(jl.contains("color") ? jl["color"] : json(nullptr),
                           L.color);
    L.intensity = jl.value("intensity", L.intensity);
    L.castsShadow = jl.value("castsShadow", L.castsShadow);
    L.ambientScale = jl.value("ambientScale", L.ambientScale);
}

// ---- 局部光源（点光 / 射灯）----
// 位置不落盘：它在 TransformComponent 里，已经随实体一起存了。

json pointLightToJson(const ecs::PointLightComponent& L) {
    return json{
        {"color", vec3ToJson(L.color)},
        {"intensity", L.intensity},
        {"range", L.range},
        {"enabled", L.enabled},
        {"castsShadow", L.castsShadow},
    };
}

json spotLightToJson(const ecs::SpotLightComponent& L) {
    return json{
        {"color", vec3ToJson(L.color)},   {"intensity", L.intensity},
        {"range", L.range},               {"direction", vec3ToJson(L.direction)},
        {"innerAngle", L.innerAngle},     {"outerAngle", L.outerAngle},
        {"enabled", L.enabled},
        {"castsShadow", L.castsShadow},
        // R3-3 光源修饰（空串 / 0 = 不用，与旧场景二进制兼容）
        {"areaWidth", L.areaWidth},
        {"areaHeight", L.areaHeight},
        {"iesProfile", L.iesProfile},
        {"lightFunction", L.lightFunction},
    };
}

ecs::PointLightComponent pointLightFromJson(const json& j) {
    ecs::PointLightComponent L;
    if (!j.is_object()) return L;
    L.color = vec3FromJson(j.contains("color") ? j["color"] : json(nullptr),
                           L.color);
    L.intensity = j.value("intensity", L.intensity);
    L.range = j.value("range", L.range);
    L.enabled = j.value("enabled", L.enabled);
    L.castsShadow = j.value("castsShadow", L.castsShadow);
    return L;
}

ecs::SpotLightComponent spotLightFromJson(const json& j) {
    ecs::SpotLightComponent L;
    if (!j.is_object()) return L;
    L.color = vec3FromJson(j.contains("color") ? j["color"] : json(nullptr),
                           L.color);
    L.intensity = j.value("intensity", L.intensity);
    L.range = j.value("range", L.range);
    L.direction = vec3FromJson(
        j.contains("direction") ? j["direction"] : json(nullptr), L.direction);
    L.innerAngle = j.value("innerAngle", L.innerAngle);
    L.outerAngle = j.value("outerAngle", L.outerAngle);
    L.enabled = j.value("enabled", L.enabled);
    L.castsShadow = j.value("castsShadow", L.castsShadow);
    // R3-3 光源修饰
    L.areaWidth = j.value("areaWidth", L.areaWidth);
    L.areaHeight = j.value("areaHeight", L.areaHeight);
    L.iesProfile = j.value("iesProfile", L.iesProfile);
    L.lightFunction = j.value("lightFunction", L.lightFunction);
    return L;
}

// ---- 实体级方向光 ----
// 场景级太阳不经过这条路（它是 Scene 顶层的 "light" 字段）；这里只
// 序列化用户手动摆进场景的方向光实体。太阳实体没有 TransformComponent，
// 不会出现在实体列表里，因此不存在"太阳被存两遍"的问题。
json directionalLightToJson(const ecs::DirectionalLightComponent& L) {
    return json{
        {"direction", vec3ToJson(L.direction)},
        {"color", vec3ToJson(L.color)},
        {"intensity", L.intensity},
    };
}

ecs::DirectionalLightComponent directionalLightFromJson(const json& j) {
    ecs::DirectionalLightComponent L;
    if (!j.is_object()) return L;
    L.direction = vec3FromJson(
        j.contains("direction") ? j["direction"] : json(nullptr), L.direction);
    L.color = vec3FromJson(j.contains("color") ? j["color"] : json(nullptr),
                           L.color);
    L.intensity = j.value("intensity", L.intensity);
    // 实体级方向光固定不投影（阴影只有场景级太阳有）、不贡献环境光
    L.castsShadow = false;
    L.ambientScale = 0.0f;
    return L;
}

// ---- 碰撞体 ----
// 只存"来源 + 摆放"：形状、相对实体的 TRS、胶囊参数、开关。
// **凸包顶点不存** —— 那是从网格顶点算出来的派生数据，读盘时按 mesh 重建
// （和网格来源 / 材质来源是同一条思路）。存下来的话，几百个顶点会让场景
// 文件里每个物体多出几 KB 纯冗余。
json collisionToJson(const ecs::CollisionComponent& c) {
    json j;
    j["shape"] = (c.shape == ecs::ColliderShape::ConvexHull) ? "convex"
                                                            : "capsule";
    j["position"] = vec3ToJson(c.position);
    j["rotation"] = vec3ToJson(c.rotation);
    j["scale"] = vec3ToJson(c.scale);
    j["capsuleRadius"] = c.capsuleRadius;
    j["capsuleHalfHeight"] = c.capsuleHalfHeight;
    j["solid"] = c.solid;
    j["showWireframe"] = c.showWireframe;
    return j;
}

ecs::CollisionComponent collisionFromJson(const json& j) {
    ecs::CollisionComponent c;
    if (!j.is_object()) return c;
    const std::string shape = j.value("shape", std::string("capsule"));
    c.shape = (shape == "convex") ? ecs::ColliderShape::ConvexHull
                                  : ecs::ColliderShape::Capsule;
    c.position = vec3FromJson(j.contains("position") ? j["position"]
                                                     : json(nullptr),
                              c.position);
    c.rotation = vec3FromJson(j.contains("rotation") ? j["rotation"]
                                                     : json(nullptr),
                              c.rotation);
    c.scale = vec3FromJson(j.contains("scale") ? j["scale"] : json(nullptr),
                           c.scale);
    c.capsuleRadius = j.value("capsuleRadius", c.capsuleRadius);
    c.capsuleHalfHeight = j.value("capsuleHalfHeight", c.capsuleHalfHeight);
    c.solid = j.value("solid", true);
    c.showWireframe = j.value("showWireframe", true);
    return c;
}

// ---- 延迟贴花（DBuffer，R4-4）----
// 存"盒体尺寸 + 参数 + 两张贴图的来源"。位置 / 朝向走实体的 transform，
// 所以这里不重复存 position/rotation —— 与"移动灯"是同一条约定。
//
// 贴图走 textureSourceToJson：与材质槽完全同一套（file / solid / checker），
// 于是**程序化来源也能往返**（验证脚本就是这么构造贴花的：不依赖磁盘上
// 有没有 PNG）。
json decalToJson(const ecs::DecalComponent& d) {
    json j;
    j["size"] = vec3ToJson(d.size);
    j["opacity"] = d.opacity;
    j["tint"] = vec3ToJson(d.tint);
    j["roughness"] = d.roughness;
    j["metallic"] = d.metallic;
    j["normalScale"] = d.normalScale;
    j["fadeStart"] = d.fadeStart;
    j["fadeEnd"] = d.fadeEnd;
    j["edgeFade"] = d.edgeFade;
    j["enabled"] = d.enabled;
    j["albedo"] = textureSourceToJson(d.albedo);
    j["normal"] = textureSourceToJson(d.normal);
    return j;
}

ecs::DecalComponent decalFromJson(assets::AssetManager& am, const json& j) {
    ecs::DecalComponent d;
    if (!j.is_object()) return d;
    d.size = vec3FromJson(j.contains("size") ? j["size"] : json(nullptr), d.size);
    d.opacity = j.value("opacity", d.opacity);
    d.tint = vec3FromJson(j.contains("tint") ? j["tint"] : json(nullptr), d.tint);
    d.roughness = j.value("roughness", d.roughness);
    d.metallic = j.value("metallic", d.metallic);
    d.normalScale = j.value("normalScale", d.normalScale);
    d.fadeStart = j.value("fadeStart", d.fadeStart);
    d.fadeEnd = j.value("fadeEnd", d.fadeEnd);
    d.edgeFade = j.value("edgeFade", d.edgeFade);
    d.enabled = j.value("enabled", true);
    d.albedo = resolveTexture(
        am, j.contains("albedo") ? j["albedo"] : json(nullptr));
    d.normal = resolveTexture(
        am, j.contains("normal") ? j["normal"] : json(nullptr));
    return d;
}

// ---- 反射探针（R5-1）----
// 存"盒体半轴 + 强度 + 盒投影开关 + 边界混合"。位置 / 朝向走实体的
// transform（与贴花 / 灯同一条约定），所以这里不重复存 position/rotation。
// 捕获本身**不进文件**：它是烘焙产物，下次打开时由渲染器重新烘
// （探针数据一变就自动重捕，见 Renderer::collectProbes）。
// ------------------------------------------------------------
// 地形（W4）
//
// 只存"哪份资产 + 变换 + 倍率 + 4 层覆盖"。高度图 / 权重图 / 分块网格
// 全部由 `.vterrain`（及其兄弟文件）决定，不落进场景 —— 否则一个场景里
// 就有 66049 个浮点数，diff 与合并都会变成灾难。
// ------------------------------------------------------------
json terrainToJson(const ecs::TerrainComponent& t) {
    json j;
    j["source"] = t.source;
    j["visible"] = t.visible;
    j["heightScale"] = t.heightScale;
    json layers = json::array();
    for (int i = 0; i < 4; ++i) layers.push_back(t.layerOverrides[i]);
    j["layers"] = layers;
    return j;
}

ecs::TerrainComponent terrainFromJson(const json& j) {
    ecs::TerrainComponent t;
    t.source = j.value("source", std::string());
    t.visible = j.value("visible", true);
    t.heightScale = j.value("heightScale", 1.0f);
    if (j.contains("layers") && j["layers"].is_array()) {
        const auto& arr = j["layers"];
        for (int i = 0; i < 4 && static_cast<std::size_t>(i) < arr.size(); ++i) {
            if (arr[static_cast<std::size_t>(i)].is_string()) {
                t.layerOverrides[i] = arr[static_cast<std::size_t>(i)].get<std::string>();
            }
        }
    }
    return t;
}

// ------------------------------------------------------------
// 水体（2026-10-21）
//
// 与地形相反：水体**没有资产**，全部参数都是场景数据，所以整份落盘。
// 只有 20 来个标量 + 两个颜色，比"资产路径 + 反序列化"还短。
// ------------------------------------------------------------
json waterToJson(const ecs::WaterComponent& w) {
    const water::WaterBody& b = w.body;
    json j;
    j["visible"] = w.visible;
    j["shape"] = static_cast<int>(b.shape);
    j["center"] = json::array({b.center.x, b.center.y});
    j["halfExtent"] = json::array({b.halfExtent.x, b.halfExtent.y});
    j["rotation"] = b.rotation;
    j["level"] = b.level;
    // 与地面的关系（第六轮）：池底高度 + 水位是否自动贴合。
    // 不存这两个字段的话，重新载入后侧壁会退化成"以水面为基准的薄片"，
    // 水体立刻又开始悬空。
    j["bedLevel"] = b.bedLevel;
    j["autoFit"] = b.autoFit;
    j["kind"] = static_cast<int>(b.kind);
    j["shallowColor"] = vec3ToJson(b.shallowColor);
    j["deepColor"] = vec3ToJson(b.deepColor);
    j["opacity"] = b.opacity;
    j["refraction"] = b.refraction;
    j["ior"] = b.ior;
    j["clarity"] = b.clarity;
    j["flowSpeed"] = b.flowSpeed;
    j["flowDir"] = json::array({b.flowDir.x, b.flowDir.y});
    // 浪况是**唯一**输入；下面四个是派生量，写出去只为让旧版本引擎还能
    // 读出"看起来差不多"的水面（见 WaterBody.h 的 legacy 注释）。
    const water::WaveDerived wd = water::deriveWaves(b.waves);
    j["waves"] = b.waves;
    j["waveHeight"] = wd.amplitude;
    j["waveLength"] = wd.wavelength;
    j["ripple"] = wd.ripple;
    j["foam"] = wd.foam;
    j["roughness"] = b.roughness;
    return j;
}

ecs::WaterComponent waterFromJson(const json& j) {
    ecs::WaterComponent w;
    water::WaterBody& b = w.body;
    // 先按 kind 铺一遍预设，再逐字段覆盖 —— 老场景缺字段时拿到的是
    // 该水质的合理默认值，而不是"零值水体"（透明、无波、无色）。
    b.kind = water::kindFromIndex(j.value("kind", 1));
    water::applyPreset(b, b.kind);

    w.visible = j.value("visible", true);
    b.shape = static_cast<water::Shape>(
        std::clamp(j.value("shape", 0), 0, 1));
    if (j.contains("center") && j["center"].is_array() &&
        j["center"].size() >= 2) {
        b.center = glm::vec2(j["center"][0].get<float>(),
                             j["center"][1].get<float>());
    }
    if (j.contains("halfExtent") && j["halfExtent"].is_array() &&
        j["halfExtent"].size() >= 2) {
        b.halfExtent = glm::vec2(j["halfExtent"][0].get<float>(),
                                 j["halfExtent"][1].get<float>());
    }
    if (j.contains("flowDir") && j["flowDir"].is_array() &&
        j["flowDir"].size() >= 2) {
        b.flowDir = glm::vec2(j["flowDir"][0].get<float>(),
                              j["flowDir"][1].get<float>());
    }
    b.rotation = j.value("rotation", b.rotation);
    b.level = j.value("level", b.level);
    // 老场景没有这两个字段：bedLevel 退化成"水面下方 0.2 m"（侧壁很薄，
    // 但仍然是贴地的），autoFit 默认 true，下次改尺寸就会重新对齐地形。
    b.bedLevel = j.value("bedLevel", b.level - 0.2f);
    b.autoFit = j.value("autoFit", true);
    if (j.contains("shallowColor"))
        b.shallowColor = vec3FromJson(j["shallowColor"], b.shallowColor);
    if (j.contains("deepColor"))
        b.deepColor = vec3FromJson(j["deepColor"], b.deepColor);
    b.opacity = j.value("opacity", b.opacity);
    b.refraction = j.value("refraction", b.refraction);
    b.ior = j.value("ior", b.ior);
    b.clarity = j.value("clarity", b.clarity);
    b.flowSpeed = j.value("flowSpeed", b.flowSpeed);
    b.waveHeight = j.value("waveHeight", b.waveHeight);
    b.waveLength = j.value("waveLength", b.waveLength);
    b.ripple = j.value("ripple", b.ripple);
    b.foam = j.value("foam", b.foam);
    b.roughness = j.value("roughness", b.roughness);

    // ---- 浪况（2026-10-21 第七轮）----
    // 新场景直接读 waves；**老场景没有这个字段** → 由 waveHeight 反推出
    // 一个等效浪况，保证"打开旧场景，水面看起来还是原来的样子"。
    // 映射见 water/WaterBody.h::deriveWaves（amp = 0.60·t^1.35）。
    if (j.contains("waves")) {
        b.waves = std::clamp(j.value("waves", b.waves), 0.0f, 10.0f);
    } else {
        const float amp = std::max(b.waveHeight, 0.0f);
        const float t = (amp > 0.0f)
                            ? std::pow(amp / 0.6f, 1.0f / 1.35f)
                            : 0.0f;
        b.waves = std::clamp(t * 10.0f, 0.0f, 10.0f);
    }
    return w;
}

// ---- 玩家出生点（GameStart，2026-10-21 第八轮）----
// 位置就是实体的 transform（与贴花/探针同一约定）；这里只存"朝向 + 开关
// + 玩家生成配置"。
//
// 2026-10-24：`mode` 用**字符串**而不是枚举整数 —— 场景文件是给人看的，
// `"third"` 比 `1` 好读，也不怕以后枚举顺序变。读的时候两种都认（老文件
// 里没有这个键 → 默认第三人称）。
json gameStartToJson(const ecs::GameStartComponent& g) {
    json j;
    j["yaw"] = g.yaw;
    j["enabled"] = g.enabled;
    j["mode"] = g.mode == ecs::PlayerMode::FirstPerson ? "first" : "third";
    j["spawnOnRun"] = g.spawnOnRun;
    return j;
}

ecs::GameStartComponent gameStartFromJson(const json& j) {
    ecs::GameStartComponent g;
    if (!j.is_object()) return g;
    g.yaw = j.value("yaw", g.yaw);
    g.enabled = j.value("enabled", true);
    g.spawnOnRun = j.value("spawnOnRun", true);
    if (j.contains("mode")) {
        const json& m = j["mode"];
        if (m.is_string()) {
            const std::string s = m.get<std::string>();
            g.mode = (s == "first" || s == "firstPerson" ||
                      s == "FirstPerson")
                         ? ecs::PlayerMode::FirstPerson
                         : ecs::PlayerMode::ThirdPerson;
        } else if (m.is_number_integer()) {
            g.mode = m.get<int>() == 0 ? ecs::PlayerMode::FirstPerson
                                       : ecs::PlayerMode::ThirdPerson;
        }
    }
    return g;
}

json reflectionProbeToJson(const ecs::ReflectionProbeComponent& p) {
    json j;
    j["extents"] = vec3ToJson(p.extents);
    j["intensity"] = p.intensity;
    j["boxProjection"] = p.boxProjection;
    j["blendDistance"] = p.blendDistance;
    j["enabled"] = p.enabled;
    return j;
}

ecs::ReflectionProbeComponent reflectionProbeFromJson(const json& j) {
    ecs::ReflectionProbeComponent p;
    if (!j.is_object()) return p;
    const json null;
    p.extents = vec3FromJson(j.contains("extents") ? j["extents"] : null,
                             p.extents);
    p.intensity = j.value("intensity", p.intensity);
    p.boxProjection = j.value("boxProjection", p.boxProjection);
    p.blendDistance = j.value("blendDistance", p.blendDistance);
    p.enabled = j.value("enabled", true);
    return p;
}

// ---- GPU 粒子发射器（R6-1）----
// 位置 / 朝向 / 缩放走实体的 transform（与贴花 / 探针同一条约定），
// 所以这里不存 position/rotation —— 发射器的局部 +Z 就是锥体轴、
// 就是形状采样的坐标系。
//
// **粒子本身不进文件**：它们是 GPU 池里的一堆瞬时状态，打开场景时从
// 零开始重新发射（帧号从 1 起，见 Renderer::m_particleSimFrame）。
// 存"某一刻有哪些粒子"既不必要（下一帧就不一样了）也无法复现。
// ------------------------------------------------------------
// 蒙皮 / 骨骼动画（角色链 A0-2）
// ------------------------------------------------------------
//
// 骨架来源从 `model->subMeshes[0].mesh->source()` 反推：组件里存的是
// 加载后的 `Model*`，而 `MeshSource` 才是"怎么重建"的唯一权威 ——
// `path` 是资产相对路径、`name` 是 AssetManager 的缓存键，与 `mesh`
// 块里的 `modelKey` 是同一套约定，所以读档时能命中同一份缓存。
//
// ⚠ **老场景没有 `skinned` 键 → 不建组件** → 渲染结果逐像素等于今天。
//   这是硬约束（"特性默认关 = 逐像素一致"），别改成默认建。
// `Model*` → {"kind","path","modelKey"}。拿不到可靠的来源就返回 null
// （程序化 / 未知来源的骨架没有重建信息 —— 只丢动画字段，网格本身走它
// 自己的 `mesh` 块）。
//
// A1 起被 skinned 与 animator 两个块共用：两处各写一份 kind 判断，
// 早晚会有一处漏掉 glTF（那是"存下来读不回来"的经典症状）。
json modelSourceToJson(const assets::Model* model) {
    if (!model || model->subMeshes.empty()) return nullptr;
    const assets::Mesh* m0 = model->subMeshes.front().mesh;
    if (!m0) return nullptr;
    const assets::MeshSource& s = m0->source();
    const char* kind = nullptr;
    if (s.kind == assets::MeshSource::Kind::FBX) {
        kind = "fbx";
    } else if (s.kind == assets::MeshSource::Kind::GLTF) {
        kind = "gltf";
    }
    if (!kind) return nullptr;
    json j;
    j["kind"] = kind;
    j["path"] = s.path;
    j["modelKey"] = s.name;
    return j;
}

json skinnedToJson(const ecs::SkinnedMeshComponent& sk) {
    json src = modelSourceToJson(sk.model);
    if (src.is_null()) return nullptr;
    json j = std::move(src);
    j["clipName"] = sk.clipName;
    j["time"] = sk.time;
    j["speed"] = sk.speed;
    j["playing"] = sk.playing;
    j["loop"] = sk.loop;
    return j;
}

// 按 {"kind","path","modelKey"} 重建模型。返回 nullptr = 失败（已打日志）。
// `what` 只用于日志前缀（"skinned" / "animator"），方便定位是哪一块坏了。
assets::Model* modelFromSource(assets::AssetManager& am, const json& j,
                               const char* what) {
    if (!j.is_object()) return nullptr;
    const std::string kind = j.value("kind", std::string());
    const std::string path = j.value("path", std::string());
    if (path.empty()) return nullptr;
    const std::string modelKey = j.value("modelKey", path);

    assets::Model* model = nullptr;
    try {
        if (kind == "fbx") {
            model = am.loadFBX(assets::resolveAssetPath(path), modelKey);
        } else if (kind == "gltf") {
            model = am.loadGLTF(assets::resolveAssetPath(path), modelKey);
        } else {
            VK_LOG_WARN("SceneSerializer: unknown %s kind '%s' (%s)", what,
                        kind.c_str(), path.c_str());
            return nullptr;
        }
    } catch (const std::exception& ex) {
        VK_LOG_WARN("SceneSerializer: %s model load failed (%s): %s", what,
                    path.c_str(), ex.what());
        return nullptr;
    }
    if (!model || !model->skinned()) {
        VK_LOG_WARN("SceneSerializer: '%s' block but model has no "
                    "skeleton (%s)", what, path.c_str());
        return nullptr;
    }
    return model;
}

// 返回 false = 重建失败（资产缺失 / 不是骨架模型）→ 调用方不建组件，
// 并保留 `SCENE-SKINNED ... ok=0` 一行日志（断言靠它区分"没存"与"存了但读失败"）。
bool skinnedFromJson(assets::AssetManager& am, const json& j,
                     ecs::SkinnedMeshComponent& out) {
    if (!j.is_object()) return false;
    assets::Model* model = modelFromSource(am, j, "skinned");
    if (!model) return false;

    out.model = model;
    out.clipName = j.value("clipName", std::string());
    out.time = j.value("time", 0.0f);
    out.speed = j.value("speed", 1.0f);
    out.playing = j.value("playing", true);
    out.loop = j.value("loop", true);
    // 派生数据：由 AnimatorSystem / Renderer 每帧重算，绝不落盘
    out.jointBase = -1;
    out.jointMatrices.clear();
    return true;
}

// ------------------------------------------------------------
// 动画器（角色链 A1，2026-10-19）
// ------------------------------------------------------------
//
// 只存"配置"：可选骨架来源 + controller/defaultClip + 四个播放字段 +
// rootMotion。**运行时字段一律不落盘**（stateIndex / stateTime /
// appliedDefaultClip / 三张参数黑板）—— 它们要么每帧重算，要么由控制器
// 资产的 default 决定（A2）。
//
// ⚠ 老场景没有 `animator` 键 → 不建组件 → 走 A0 老路径，逐像素不变。
//   这与 `skinned` 同一条硬约束，别改成默认建。
json animatorToJson(const ecs::AnimatorComponent& an) {
    json j;
    j["defaultClip"] = an.defaultClip;
    j["controllerPath"] = an.controllerPath;
    j["time"] = an.time;
    j["speed"] = an.speed;
    j["playing"] = an.playing;
    j["loop"] = an.loop;
    j["rootMotion"] = static_cast<int>(an.rootMotion);
    // 骨架来源只在**显式给过**的时候才写：空 = 用 skinned 那一份。
    // 写一个空对象会让读档的人以为"这里本来有东西被清掉了"。
    json src = modelSourceToJson(an.model);
    if (!src.is_null()) j["model"] = std::move(src);
    return j;
}

// 返回 false = 读不回来。**与 skinnedFromJson 的关键区别**：骨架来源读
// 失败 / 没写都**不算失败** —— 组件仍然建起来，只是把 model 留空，让
// AnimatorSystem 回退到 skinned 那一份。理由是"动画器的配置（clip /
// time / speed）比骨架来源重要"，为了一个可回退的字段丢掉整块配置，
// 用户下次打开会看到时间被重置。
bool animatorFromJson(assets::AssetManager& am, const json& j,
                      ecs::AnimatorComponent& out) {
    if (!j.is_object()) return false;
    if (j.contains("model")) {
        out.model = modelFromSource(am, j["model"], "animator");
    }
    out.defaultClip = j.value("defaultClip", std::string());
    out.controllerPath = j.value("controllerPath", std::string());
    out.time = j.value("time", 0.0f);
    out.speed = j.value("speed", 1.0f);
    out.playing = j.value("playing", true);
    out.loop = j.value("loop", true);
    const int rm = j.value("rootMotion", 0);
    out.rootMotion = (rm >= 0 && rm <= 2)
                         ? static_cast<ecs::AnimatorComponent::RootMotionMode>(rm)
                         : ecs::AnimatorComponent::RootMotionMode::Off;
    // 运行时：一律重置（绝不能从文件里读回来，那会让"这一局"的状态
    // 跟着存档走）
    out.stateIndex = -1;
    out.stateTime = 0.0f;
    out.appliedDefaultClip = false;
    out.floatParams.clear();
    out.boolParams.clear();
    out.triggerParams.clear();
    return true;
}

// ---------------- 实体 transform 关键帧轨道 ----------------
//
// 与上面的 animator（骨骼片段）刻意分开，理由见 Components.h。这里只是
// 一段"时刻 → TRS"的数组，读写都直白。
//
// 两个刻意的取舍：
//   · `playing` 不写盘 —— 重开场景时是否在播由 autoplay 决定，而不是由
//     "上次退出那一刻刚好在播"决定（否则每次开场景都在动）。
//   · `time` 写盘 —— 播放头位置是用户的观察点，重开接着看是符合预期的。
json animClipToJson(const ecs::AnimationClipComponent& c) {
    json j;
    j["duration"] = c.duration;
    j["loop"] = c.loop;
    j["autoplay"] = c.autoplay;
    j["time"] = c.time;
    json arr = json::array();
    for (const ecs::TransformKey& k : c.keys) {
        arr.push_back(json{
            {"t", k.t},
            {"pos", vec3ToJson(k.position)},
            {"rot", vec3ToJson(k.rotation)},
            {"scale", vec3ToJson(k.scale)},
        });
    }
    j["keys"] = std::move(arr);
    return j;
}

ecs::AnimationClipComponent animClipFromJson(const json& j) {
    ecs::AnimationClipComponent c;
    if (!j.is_object()) return c;
    c.duration = j.value("duration", 4.0f);
    c.loop = j.value("loop", true);
    c.autoplay = j.value("autoplay", false);
    c.time = j.value("time", 0.0f);
    if (j.contains("keys") && j["keys"].is_array()) {
        for (const json& jk : j["keys"]) {
            if (!jk.is_object()) continue;
            ecs::TransformKey k;
            k.t = jk.value("t", 0.0f);
            k.position =
                vec3FromJson(jk.contains("pos") ? jk["pos"] : json(), k.position);
            k.rotation =
                vec3FromJson(jk.contains("rot") ? jk["rot"] : json(), k.rotation);
            k.scale =
                vec3FromJson(jk.contains("scale") ? jk["scale"] : json(), k.scale);
            c.keys.push_back(k);
        }
        // 手改过 JSON 的话顺序不可信 —— 插值是按相邻两帧做的，乱序会插出
        // 完全错误的中间态（看起来像"动画在抽搐"）。
        std::sort(c.keys.begin(), c.keys.end(),
                  [](const ecs::TransformKey& a, const ecs::TransformKey& b) {
                      return a.t < b.t;
                  });
    }
    if (c.duration < 1e-3f) c.duration = 4.0f;
    // 时长必须能罩住最后一帧，否则最后一帧永远播不到
    if (!c.keys.empty() && c.duration < c.keys.back().t)
        c.duration = c.keys.back().t;
    c.time = std::clamp(c.time, 0.0f, c.duration);
    c.playing = c.autoplay;
    return c;
}

json particleEmitterToJson(const ecs::ParticleEmitterComponent& p) {
    json j;
    // ---- Main / Playback ----
    j["rate"] = p.rate;
    j["rateOverDistance"] = p.rateOverDistance;
    j["duration"] = p.duration;
    j["looping"] = p.looping;
    j["prewarm"] = p.prewarm;
    j["startDelay"] = p.startDelay;
    j["lifetime"] = p.lifetime;
    j["lifetimeVariance"] = p.lifetimeVariance;
    j["startSpeed"] = p.startSpeed;
    j["speedVariance"] = p.speedVariance;
    j["spread"] = p.spread;
    // ---- Emission（bursts 是数组；burns[0] 同时也写成 "burst" 作为
    //      老读法 / 老写法的兼容入口）----
    {
        json arr = json::array();
        for (int b = 0; b < ecs::kMaxBursts; ++b) {
            const ecs::ParticleBurst& s = p.bursts[b];
            json e;
            e["time"] = s.time;
            e["count"] = s.count;
            e["cycles"] = s.cycles;
            e["interval"] = s.interval;
            arr.push_back(e);
        }
        j["bursts"] = arr;
        j["burst"] = p.bursts[0].count;
    }
    // ---- Shape ----
    j["shape"] = static_cast<int>(p.shape);
    j["shapeExtents"] = vec3ToJson(p.shapeExtents);
    j["shapeRadius"] = p.shapeRadius;
    j["radiusThickness"] = p.radiusThickness;
    j["arc"] = p.arc;
    j["shapeRotation"] = p.shapeRotation;
    j["emitFrom"] = static_cast<int>(p.emitFrom);
    // ---- Velocity over Lifetime ----
    j["velOverLifeEnabled"] = p.velOverLifeEnabled;
    j["velOverLifeX"] = vec4ToJson(p.velOverLifeX);
    j["velOverLifeY"] = vec4ToJson(p.velOverLifeY);
    j["velOverLifeZ"] = vec4ToJson(p.velOverLifeZ);
    j["velOverLifeWorldSpace"] = p.velOverLifeWorldSpace;
    j["orbital"] = vec3ToJson(p.orbital);
    j["orbitalOffset"] = p.orbitalOffset;
    j["radial"] = p.radial;
    j["speedModifier"] = vec4ToJson(p.speedModifier);
    // ---- Limit Velocity ----
    j["limitVelEnabled"] = p.limitVelEnabled;
    j["limitSpeed"] = p.limitSpeed;
    j["limitDampen"] = p.limitDampen;
    // ---- Force ----
    j["forceEnabled"] = p.forceEnabled;
    j["force"] = vec3ToJson(p.force);
    j["forceRandomize"] = p.forceRandomize;
    // ---- Rotation over Lifetime ----
    j["startRotation"] = p.startRotation;
    j["startRotationVariance"] = p.startRotationVariance;
    j["angularVelocity"] = p.angularVelocity;
    j["angularVelocityVariance"] = p.angularVelocityVariance;
    // ---- Noise ----
    j["noiseEnabled"] = p.noiseEnabled;
    j["noiseStrength"] = p.noiseStrength;
    j["noiseFrequency"] = p.noiseFrequency;
    j["noiseScrollSpeed"] = p.noiseScrollSpeed;
    j["noiseDamping"] = p.noiseDamping;
    j["noiseOctaves"] = p.noiseOctaves;
    // ---- Physics ----
    j["gravity"] = vec3ToJson(p.gravity);
    j["drag"] = p.drag;
    j["simulationSpace"] = static_cast<int>(p.simulationSpace);
    // ---- Size / Color over Lifetime ----
    j["sizeOverLife"] = vec4ToJson(p.sizeOverLife);
    j["alphaOverLife"] = vec4ToJson(p.alphaOverLife);
    {
        json ramp = json::array();
        for (int s = 0; s < p.colorRamp.count && s < ecs::kMaxColorStops; ++s) {
            json e;
            e["t"] = p.colorRamp.stops[s].w;
            e["color"] = vec3ToJson(glm::vec3(p.colorRamp.stops[s]));
            ramp.push_back(e);
        }
        j["colorRamp"] = ramp;
        j["colorRampCount"] = p.colorRamp.count;
        // 老的三个字段继续写（外部工具 / 老版本的读取路径），
        // 取渐变带上 t = 0 / 0.5 / 1 的采样值。
        j["colorStart"] = vec3ToJson(p.colorRamp.eval(0.0f));
        j["colorMid"] = vec3ToJson(p.colorRamp.eval(0.5f));
        j["colorEnd"] = vec3ToJson(p.colorRamp.eval(1.0f));
    }
    // ---- Renderer ----
    j["startSize"] = p.startSize;
    j["sprite"] = static_cast<int>(p.sprite);
    j["blend"] = static_cast<int>(p.blend);
    j["renderMode"] = static_cast<int>(p.renderMode);
    j["sortMode"] = static_cast<int>(p.sortMode);
    j["stretchScale"] = p.stretchScale;
    j["emissive"] = p.emissive;
    j["softParticles"] = p.softParticles;
    j["softDistance"] = p.softDistance;
    j["enabled"] = p.enabled;
    return j;
}

ecs::ParticleEmitterComponent particleEmitterFromJson(const json& j) {
    ecs::ParticleEmitterComponent p;
    if (!j.is_object()) return p;
    const json null;
    // ---- Main / Playback ----
    p.rate = j.value("rate", p.rate);
    p.rateOverDistance = j.value("rateOverDistance", p.rateOverDistance);
    p.duration = j.value("duration", p.duration);
    p.looping = j.value("looping", p.looping);
    p.prewarm = j.value("prewarm", p.prewarm);
    p.startDelay = j.value("startDelay", p.startDelay);
    p.lifetime = j.value("lifetime", p.lifetime);
    p.lifetimeVariance = j.value("lifetimeVariance", p.lifetimeVariance);
    p.startSpeed = j.value("startSpeed", p.startSpeed);
    p.speedVariance = j.value("speedVariance", p.speedVariance);
    p.spread = j.value("spread", p.spread);
    // ---- Emission ----
    // 新格式：bursts 数组。老格式：单个 "burst"（= 时刻 0 的一次爆发）。
    // 两条都认，这样老场景能原样打开。
    if (j.contains("bursts") && j["bursts"].is_array()) {
        const json& arr = j["bursts"];
        for (int b = 0; b < ecs::kMaxBursts && b < static_cast<int>(arr.size());
             ++b) {
            const json& e = arr[static_cast<size_t>(b)];
            if (!e.is_object()) continue;
            p.bursts[b].time = e.value("time", p.bursts[b].time);
            p.bursts[b].count = e.value("count", p.bursts[b].count);
            p.bursts[b].cycles = e.value("cycles", p.bursts[b].cycles);
            p.bursts[b].interval = e.value("interval", p.bursts[b].interval);
        }
    } else if (j.contains("burst")) {
        p.bursts[0].time = 0.0f;
        p.bursts[0].count = j.value("burst", 0);
        p.bursts[0].cycles = 1;
        p.bursts[0].interval = 0.0f;
    }
    // ---- Shape ----
    p.shape = static_cast<ecs::ParticleShape>(
        j.value("shape", static_cast<int>(p.shape)));
    p.shapeExtents = vec3FromJson(j.contains("shapeExtents")
                                      ? j["shapeExtents"] : null,
                                  p.shapeExtents);
    p.shapeRadius = j.value("shapeRadius", p.shapeRadius);
    p.radiusThickness = j.value("radiusThickness", p.radiusThickness);
    p.arc = j.value("arc", p.arc);
    p.shapeRotation = j.value("shapeRotation", p.shapeRotation);
    p.emitFrom = static_cast<ecs::ParticleEmitFrom>(
        j.value("emitFrom", static_cast<int>(p.emitFrom)));
    // ---- Velocity over Lifetime ----
    p.velOverLifeEnabled = j.value("velOverLifeEnabled", p.velOverLifeEnabled);
    p.velOverLifeX = vec4FromJson(
        j.contains("velOverLifeX") ? j["velOverLifeX"] : null, p.velOverLifeX);
    p.velOverLifeY = vec4FromJson(
        j.contains("velOverLifeY") ? j["velOverLifeY"] : null, p.velOverLifeY);
    p.velOverLifeZ = vec4FromJson(
        j.contains("velOverLifeZ") ? j["velOverLifeZ"] : null, p.velOverLifeZ);
    p.velOverLifeWorldSpace =
        j.value("velOverLifeWorldSpace", p.velOverLifeWorldSpace);
    p.orbital = vec3FromJson(j.contains("orbital") ? j["orbital"] : null,
                             p.orbital);
    p.orbitalOffset = j.value("orbitalOffset", p.orbitalOffset);
    p.radial = j.value("radial", p.radial);
    p.speedModifier = vec4FromJson(
        j.contains("speedModifier") ? j["speedModifier"] : null,
        p.speedModifier);
    // ---- Limit Velocity ----
    p.limitVelEnabled = j.value("limitVelEnabled", p.limitVelEnabled);
    p.limitSpeed = j.value("limitSpeed", p.limitSpeed);
    p.limitDampen = j.value("limitDampen", p.limitDampen);
    // ---- Force ----
    p.forceEnabled = j.value("forceEnabled", p.forceEnabled);
    p.force = vec3FromJson(j.contains("force") ? j["force"] : null, p.force);
    p.forceRandomize = j.value("forceRandomize", p.forceRandomize);
    // ---- Rotation over Lifetime ----
    p.startRotation = j.value("startRotation", p.startRotation);
    p.startRotationVariance =
        j.value("startRotationVariance", p.startRotationVariance);
    p.angularVelocity = j.value("angularVelocity", p.angularVelocity);
    p.angularVelocityVariance =
        j.value("angularVelocityVariance", p.angularVelocityVariance);
    // ---- Noise ----
    p.noiseEnabled = j.value("noiseEnabled", p.noiseEnabled);
    p.noiseStrength = j.value("noiseStrength", p.noiseStrength);
    p.noiseFrequency = j.value("noiseFrequency", p.noiseFrequency);
    p.noiseScrollSpeed = j.value("noiseScrollSpeed", p.noiseScrollSpeed);
    p.noiseDamping = j.value("noiseDamping", p.noiseDamping);
    p.noiseOctaves = j.value("noiseOctaves", p.noiseOctaves);
    // ---- Physics ----
    p.gravity = vec3FromJson(j.contains("gravity") ? j["gravity"] : null,
                             p.gravity);
    p.drag = j.value("drag", p.drag);
    p.simulationSpace = static_cast<ecs::ParticleSimulationSpace>(
        j.value("simulationSpace", static_cast<int>(p.simulationSpace)));
    // ---- Size / Color over Lifetime ----
    p.sizeOverLife = vec4FromJson(
        j.contains("sizeOverLife") ? j["sizeOverLife"] : null, p.sizeOverLife);
    p.alphaOverLife = vec4FromJson(
        j.contains("alphaOverLife") ? j["alphaOverLife"] : null,
        p.alphaOverLife);
    if (j.contains("colorRamp") && j["colorRamp"].is_array() &&
        !j["colorRamp"].empty()) {
        const json& ramp = j["colorRamp"];
        const int n = glm::clamp(
            j.value("colorRampCount", static_cast<int>(ramp.size())), 1,
            ecs::kMaxColorStops);
        for (int s = 0; s < n; ++s) {
            const size_t idx = static_cast<size_t>(
                s < static_cast<int>(ramp.size()) ? s : ramp.size() - 1);
            const json& e = ramp[idx];
            glm::vec3 c(1.0f);
            if (e.is_object()) {
                c = vec3FromJson(e.contains("color") ? e["color"] : null, c);
                p.colorRamp.stops[s].w =
                    e.value("t", p.colorRamp.stops[s].w);
            }
            p.colorRamp.stops[s] = glm::vec4(c, p.colorRamp.stops[s].w);
        }
        p.colorRamp.count = n;
    } else {
        // 老格式：三段固定颜色 → 塞进渐变带（t = 0 / 0.5 / 1）
        const glm::vec3 c0 = vec3FromJson(
            j.contains("colorStart") ? j["colorStart"] : null,
            glm::vec3(1.0f, 0.75f, 0.25f));
        const glm::vec3 c1 = vec3FromJson(
            j.contains("colorMid") ? j["colorMid"] : null,
            glm::vec3(1.0f, 0.35f, 0.05f));
        const glm::vec3 c2 = vec3FromJson(
            j.contains("colorEnd") ? j["colorEnd"] : null,
            glm::vec3(0.25f, 0.04f, 0.02f));
        p.colorRamp.count = 3;
        p.colorRamp.stops[0] = glm::vec4(c0, 0.0f);
        p.colorRamp.stops[1] = glm::vec4(c1, 0.5f);
        p.colorRamp.stops[2] = glm::vec4(c2, 1.0f);
    }
    p.colorRamp.sort();
    // ---- Renderer ----
    p.startSize = j.value("startSize", p.startSize);
    // 贴图形状（ParticleSprite）。老场景没有这个键 → 保持默认 Disc，
    // 也就是**逐像素不变**。
    p.sprite = static_cast<ecs::ParticleSprite>(
        glm::clamp(j.value("sprite", 0), 0,
                   static_cast<int>(ecs::ParticleSprite::Count) - 1));
    p.blend = static_cast<ecs::ParticleBlend>(
        j.value("blend", static_cast<int>(p.blend)));
    p.renderMode = static_cast<ecs::ParticleRenderMode>(
        j.value("renderMode", static_cast<int>(p.renderMode)));
    p.sortMode = static_cast<ecs::ParticleSortMode>(
        j.value("sortMode", static_cast<int>(p.sortMode)));
    p.stretchScale = j.value("stretchScale", p.stretchScale);
    p.emissive = j.value("emissive", p.emissive);
    p.softParticles = j.value("softParticles", p.softParticles);
    p.softDistance = j.value("softDistance", p.softDistance);
    p.enabled = j.value("enabled", true);
    return p;
}

// ---------------------------------------------------------------- 物理（R7-1）
//
// 两层数据，各自挂在它该在的地方：
//   · **场景级**（重力 / 步长 / 求解器 / 休眠）→ 顶层 "physics"
//     （跟相机、太阳一样是"一份场景一套参数"）
//   · **实体级**（刚体：类型 / 形状 / 质量 / 材质 / 初速度）→ 每实体的
//     "rigidbody"
//
// ⚠ 这里**不存**任何解算结果（速度、位置、休眠标记）。位置本来就在
//   transform 里，而速度/休眠是运行态派生量 —— Play 每次都从编辑态摆好的
//   位置重新开始，这正是"Stop 即回滚"能成立的前提。存了反而会让同一份
//   场景文件读两次得到不同的初始条件。

json physicsToJson(const PhysicsSettings& p) {
    return json{
        {"enabled", p.enabled},
        {"gravity", vec3ToJson(p.gravity)},
        {"groundPlane", p.groundPlane},
        {"fixedTimeStep", p.fixedTimeStep},
        {"maxSubSteps", p.maxSubSteps},
        {"velocityIterations", p.velocityIterations},
        {"positionIterations", p.positionIterations},
        {"baumgarte", p.baumgarte},
        {"slop", p.slop},
        {"allowSleep", p.allowSleep},
        {"sleepLinearThreshold", p.sleepLinearThreshold},
        {"sleepAngularThreshold", p.sleepAngularThreshold},
        {"sleepTime", p.sleepTime},
    };
}

void physicsFromJson(PhysicsSettings& p, const json& j) {
    if (!j.is_object()) return;   // 老场景 / 空 → 保留 clear() 后的默认值
    p.enabled = j.value("enabled", p.enabled);
    if (j.contains("gravity")) p.gravity = vec3FromJson(j["gravity"], p.gravity);
    p.groundPlane = j.value("groundPlane", p.groundPlane);
    p.fixedTimeStep = j.value("fixedTimeStep", p.fixedTimeStep);
    p.maxSubSteps = j.value("maxSubSteps", p.maxSubSteps);
    p.velocityIterations = j.value("velocityIterations", p.velocityIterations);
    p.positionIterations = j.value("positionIterations", p.positionIterations);
    p.baumgarte = j.value("baumgarte", p.baumgarte);
    p.slop = j.value("slop", p.slop);
    p.allowSleep = j.value("allowSleep", p.allowSleep);
    p.sleepLinearThreshold =
        j.value("sleepLinearThreshold", p.sleepLinearThreshold);
    p.sleepAngularThreshold =
        j.value("sleepAngularThreshold", p.sleepAngularThreshold);
    p.sleepTime = j.value("sleepTime", p.sleepTime);
}

json rigidBodyToJson(const ecs::RigidBodyComponent& b) {
    return json{
        {"bodyType", static_cast<int>(b.bodyType)},
        {"shape", static_cast<int>(b.shape)},
        {"radius", b.radius},
        {"halfHeight", b.halfHeight},
        {"halfExtents", vec3ToJson(b.halfExtents)},
        {"mass", b.mass},
        {"density", b.density},
        {"staticFriction", b.staticFriction},
        {"dynamicFriction", b.dynamicFriction},
        {"restitution", b.restitution},
        {"linearDamping", b.linearDamping},
        {"angularDamping", b.angularDamping},
        {"useGravity", b.useGravity},
        {"allowSleep", b.allowSleep},
        {"initialVelocity", vec3ToJson(b.initialVelocity)},
        {"initialAngularVelocity", vec3ToJson(b.initialAngularVelocity)},
    };
}

ecs::RigidBodyComponent rigidBodyFromJson(const json& j) {
    ecs::RigidBodyComponent b;   // 从默认值出发：缺字段 = 该字段保持默认
    if (!j.is_object()) return b;

    b.bodyType = static_cast<ecs::PhysicsBodyType>(
        j.value("bodyType", static_cast<int>(b.bodyType)));
    b.shape = static_cast<ecs::PhysicsShapeType>(
        j.value("shape", static_cast<int>(b.shape)));
    b.radius = j.value("radius", b.radius);
    b.halfHeight = j.value("halfHeight", b.halfHeight);
    if (j.contains("halfExtents"))
        b.halfExtents = vec3FromJson(j["halfExtents"], b.halfExtents);
    b.mass = j.value("mass", b.mass);
    b.density = j.value("density", b.density);
    b.staticFriction = j.value("staticFriction", b.staticFriction);
    b.dynamicFriction = j.value("dynamicFriction", b.dynamicFriction);
    b.restitution = j.value("restitution", b.restitution);
    b.linearDamping = j.value("linearDamping", b.linearDamping);
    b.angularDamping = j.value("angularDamping", b.angularDamping);
    b.useGravity = j.value("useGravity", b.useGravity);
    b.allowSleep = j.value("allowSleep", b.allowSleep);
    if (j.contains("initialVelocity"))
        b.initialVelocity = vec3FromJson(j["initialVelocity"], b.initialVelocity);
    if (j.contains("initialAngularVelocity"))
        b.initialAngularVelocity =
            vec3FromJson(j["initialAngularVelocity"], b.initialAngularVelocity);
    return b;
}

// ---------------- 角色控制器（G07）----------------
//
// ⚠ `velocity / position / bodyYaw / onGround / groundNormalZ / started`
//   是**运行态**：每帧由 stepCharacter 写回，存下来既没意义（下一帧就被
//   覆盖）也会把"上一轮的悬崖边状态"带进新会话。只存决定手感的那一组。
json characterToJson(const ecs::CharacterControllerComponent& c) {
    return json{
        {"radius", c.radius},
        {"halfHeight", c.halfHeight},
        {"stepHeight", c.stepHeight},
        {"slopeLimitDeg", c.slopeLimitDeg},
        {"groundSnap", c.groundSnap},
        {"groundSnapDistance", c.groundSnapDistance},
        {"gravityScale", c.gravityScale},
        {"maxSpeed", c.maxSpeed},
        {"sprintMultiplier", c.sprintMultiplier},
        {"jumpSpeed", c.jumpSpeed},
        {"acceleration", c.acceleration},
        {"airControl", c.airControl},
        {"turnSpeed", c.turnSpeed},
        {"pushStrength", c.pushStrength},
        {"enabled", c.enabled},
    };
}

ecs::CharacterControllerComponent characterFromJson(const json& j) {
    ecs::CharacterControllerComponent c;
    if (!j.is_object()) return c;
    c.radius = j.value("radius", c.radius);
    c.halfHeight = j.value("halfHeight", c.halfHeight);
    c.stepHeight = j.value("stepHeight", c.stepHeight);
    c.slopeLimitDeg = j.value("slopeLimitDeg", c.slopeLimitDeg);
    c.groundSnap = j.value("groundSnap", c.groundSnap);
    c.groundSnapDistance =
        j.value("groundSnapDistance", c.groundSnapDistance);
    c.gravityScale = j.value("gravityScale", c.gravityScale);
    c.maxSpeed = j.value("maxSpeed", c.maxSpeed);
    c.sprintMultiplier = j.value("sprintMultiplier", c.sprintMultiplier);
    c.jumpSpeed = j.value("jumpSpeed", c.jumpSpeed);
    c.acceleration = j.value("acceleration", c.acceleration);
    c.airControl = j.value("airControl", c.airControl);
    c.turnSpeed = j.value("turnSpeed", c.turnSpeed);
    c.pushStrength = j.value("pushStrength", c.pushStrength);
    c.enabled = j.value("enabled", c.enabled);
    return c;
}

// ---------------- 动画键盘映射（P4）----------------
//
// 空表**不写**整块（同 animClip 的道理）：没绑过键的角色不该在场景文件里
// 多出一段 `{"entries":[]}` 的噪音。
//
// `key` 存 GLFW 键码（整数）。为什么不存 "F"：字符串要解析、要处理大小写与
// 别名，而这个值唯一的用途是喂 `core::Input::keyPressed(int)` 与 ImGui 的
// 捕获结果 —— 两者都是 GLFW 码，存整数是零转换。
json animKeyMapToJson(const ecs::AnimKeyMapComponent& m) {
    json entries = json::array();
    for (const auto& e : m.entries) {
        if (e.clip.empty() || e.key == 0) continue;   // 半截记录不落盘
        entries.push_back(json{{"clip", e.clip}, {"key", e.key}});
    }
    return json{
        {"entries", std::move(entries)},
        {"holdSeconds", m.holdSeconds},
        {"enabled", m.enabled},
    };
}

ecs::AnimKeyMapComponent animKeyMapFromJson(const json& j) {
    ecs::AnimKeyMapComponent m;
    if (!j.is_object()) return m;
    m.holdSeconds = j.value("holdSeconds", m.holdSeconds);
    m.enabled = j.value("enabled", m.enabled);
    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& e : j["entries"]) {
            if (!e.is_object()) continue;
            ecs::AnimKeyMapEntry one;
            one.clip = e.value("clip", std::string());
            one.key = e.value("key", 0);
            if (one.clip.empty() || one.key == 0) continue;
            m.entries.push_back(std::move(one));
        }
    }
    return m;
}

// ---------------- 触发体（G08）----------------
json triggerToJson(const ecs::TriggerComponent& t) {
    return json{
        {"enabled", t.enabled},
        {"mask", t.mask},
        {"reportStay", t.reportStay},
    };
}

ecs::TriggerComponent triggerFromJson(const json& j) {
    ecs::TriggerComponent t;
    if (!j.is_object()) return t;
    t.enabled = j.value("enabled", t.enabled);
    t.mask = j.value("mask", t.mask);
    t.reportStay = j.value("reportStay", t.reportStay);
    return t;
}

// ---------------- 物理材质（G08）----------------
// 覆盖层：没有本组件时物理行为与加它之前**逐字段相同**，所以"缺文件"和
// "有文件但缺字段"都必须落回 RigidBody 的默认值，不能凭空造一个覆盖层。
json physicsMaterialToJson(const ecs::PhysicsMaterialComponent& m) {
    return json{
        {"staticFriction", m.staticFriction},
        {"dynamicFriction", m.dynamicFriction},
        {"restitution", m.restitution},
        {"density", m.density},
    };
}

ecs::PhysicsMaterialComponent physicsMaterialFromJson(const json& j) {
    ecs::PhysicsMaterialComponent m;
    if (!j.is_object()) return m;
    m.staticFriction = j.value("staticFriction", m.staticFriction);
    m.dynamicFriction = j.value("dynamicFriction", m.dynamicFriction);
    m.restitution = j.value("restitution", m.restitution);
    m.density = j.value("density", m.density);
    return m;
}

// ---------------- 相机组件（G09）----------------
// `trauma` 是运行态（外部 addShake 累加、随时间衰减），不落盘。
json cameraComponentToJson(const ecs::CameraComponent& c) {
    return json{
        {"fov", c.fov},
        {"priority", c.priority},
        {"enabled", c.enabled},
        {"targetOffset", vec3ToJson(c.targetOffset)},
        {"distance", c.distance},
        {"minDistance", c.minDistance},
        {"collideWithWorld", c.collideWithWorld},
        {"shakeAmplitude", c.shakeAmplitude},
        {"shakeFrequency", c.shakeFrequency},
    };
}

ecs::CameraComponent cameraComponentFromJson(const json& j) {
    ecs::CameraComponent c;
    if (!j.is_object()) return c;
    c.fov = j.value("fov", c.fov);
    c.priority = j.value("priority", c.priority);
    c.enabled = j.value("enabled", c.enabled);
    if (j.contains("targetOffset"))
        c.targetOffset = vec3FromJson(j["targetOffset"], c.targetOffset);
    c.distance = j.value("distance", c.distance);
    c.minDistance = j.value("minDistance", c.minDistance);
    c.collideWithWorld = j.value("collideWithWorld", c.collideWithWorld);
    c.shakeAmplitude = j.value("shakeAmplitude", c.shakeAmplitude);
    c.shakeFrequency = j.value("shakeFrequency", c.shakeFrequency);
    return c;
}

// ============================================================
// 场景 → JSON（文件版与内存版共用的核心）
// ============================================================

json buildSceneJson(const Scene& scene, SceneIoResult& r) {
    const ecs::World& w = scene.world();

    // ---- 第一遍：收集所有"带变换"的实体，并建立 id → 下标 映射 ----
    // 父节点用 **数组下标** 表达，比用名字可靠（名字可能重复或为空）
    struct Item {
        ecs::Entity entity;
        const ecs::TransformComponent* transform;
    };
    std::vector<Item> items;
    std::unordered_map<ecs::EntityId, int> indexOf;

    w.each<ecs::TransformComponent>(
        [&](ecs::Entity e, const ecs::TransformComponent& t) {
            indexOf[e.id] = static_cast<int>(items.size());
            items.push_back(Item{e, &t});
        });

    json root;
    root["version"] = kFormatVersion;
    root["generator"] = "Venn/SceneSerializer";
    root["camera"] = cameraToJson(scene.camera());
    // 多相机：0 号相机同时也写在顶层 "camera"（旧场景 / 旧读取器仍认它），
    // 额外相机列在这里。读的时候以 "camera" 为准覆盖 0 号，再从 "cameras"
    // 补齐 1..n —— 两个字段都能单独改而不会互相打架。
    if (scene.cameraCount() > 1) {
        json cams = json::array();
        for (size_t i = 1; i < scene.cameraCount(); ++i)
            cams.push_back(cameraToJson(scene.camera(i)));
        root["cameras"] = std::move(cams);
    }
    root["light"] = lightToJson(scene.light());
    // 物理世界（R7-1）。只有**非默认**才写 —— 老场景文件里没有这段，
    // 读的时候也就自然落到默认值上，两份文件的 diff 不会被"一整套默认
    // 物理参数"撑爆。判据用一个显式函数而不是逐字段比较，是为了让"新增
    // 一个字段忘了同步"变成编译期 / 一看就懂的错误。
    {
        const PhysicsSettings& ps = scene.physicsSettings();
        const PhysicsSettings def;
        if (ps.enabled != def.enabled || ps.gravity != def.gravity ||
            ps.groundPlane != def.groundPlane ||
            ps.fixedTimeStep != def.fixedTimeStep ||
            ps.maxSubSteps != def.maxSubSteps ||
            ps.velocityIterations != def.velocityIterations ||
            ps.positionIterations != def.positionIterations ||
            ps.baumgarte != def.baumgarte || ps.slop != def.slop ||
            ps.allowSleep != def.allowSleep ||
            ps.sleepLinearThreshold != def.sleepLinearThreshold ||
            ps.sleepAngularThreshold != def.sleepAngularThreshold ||
            ps.sleepTime != def.sleepTime)
            root["physics"] = physicsToJson(ps);
    }
    // 脚本世界黑板（S2）。只有存在时才写 —— 与 physics 同款：老场景文件
    // 里没有这段，读的时候自然落到空表。
    // ⚠ 与 `ecs::ScriptVarsComponent`（逐实体、不存盘）**刻意分开**：
    //   这里存的是"这关的进度"，组件存的是"这一局的血量"。
    if (!scene.scriptVars().empty()) {
        json sv = json::object();
        for (const auto& kv : scene.scriptVars()) sv[kv.first] = kv.second;
        root["scriptVars"] = std::move(sv);
    }
    // 后处理体积（相机级效果的场景化覆盖）：为空时不写，旧场景文件不含它。
    if (!scene.postVolumes().empty()) {
        json vols = json::array();
        for (const PostVolume& v : scene.postVolumes().volumes())
            vols.push_back(postVolumeToJson(v));
        root["postVolumes"] = std::move(vols);
    }

    root["entities"] = json::array();
    for (const Item& it : items) {
        json je;
        const std::string* nm = w.name(it.entity);
        je["name"] = nm ? *nm : std::string("Entity");

        je["transform"] = json{
            {"position", vec3ToJson(it.transform->position)},
            {"rotation", vec3ToJson(it.transform->rotation)},
            {"scale", vec3ToJson(it.transform->scale)},
        };

        if (const auto* vis = w.get<ecs::VisibilityComponent>(it.entity)) {
            je["visible"] = vis->visible;
            je["castShadow"] = vis->castShadow;
        }

        // 编辑器锁定（"看得见、动不了"；自带地面就是靠它固定的）
        if (w.has<ecs::LockedComponent>(it.entity)) je["locked"] = true;

        int parentIndex = -1;
        if (const auto* h = w.get<ecs::HierarchyComponent>(it.entity)) {
            if (h->parent != ecs::kInvalidEntity) {
                auto f = indexOf.find(h->parent);
                if (f != indexOf.end()) parentIndex = f->second;
            }
            // "文件夹"标记（Hierarchy 里显示成目录图标的纯分组节点）
            if (h->group) je["folder"] = true;
        }
        je["parent"] = parentIndex;

        const auto* mc = w.get<ecs::MeshComponent>(it.entity);
        const auto* matc = w.get<ecs::MaterialComponent>(it.entity);
        je["mesh"] = meshSourceToJson(mc ? mc->mesh : nullptr);
        je["material"] = materialToJson(matc ? matc->material : nullptr);
        // 内置图元的生成参数（编辑器可回改，见 ecs::MeshComponent::primitive）
        if (mc) {
            json jp = primitiveParamsToJson(*mc);
            if (!jp.is_null()) je["primitive"] = jp;
        }

        // 局部光源（位置在 transform 里，已经存过了）
        if (const auto* pl = w.get<ecs::PointLightComponent>(it.entity))
            je["pointLight"] = pointLightToJson(*pl);
        if (const auto* sl = w.get<ecs::SpotLightComponent>(it.entity))
            je["spotLight"] = spotLightToJson(*sl);
        // 实体级方向光（补光；场景级太阳走顶层 "light" 字段）
        if (const auto* dl = w.get<ecs::DirectionalLightComponent>(it.entity))
            je["directionalLight"] = directionalLightToJson(*dl);

        // 碰撞体（几何是派生数据，只存来源与摆放，见 collisionToJson）
        if (const auto* cc = w.get<ecs::CollisionComponent>(it.entity))
            je["collision"] = collisionToJson(*cc);

        // 延迟贴花（R4-4）。注意：贴花**没有网格**，所以下面那句
        // "mesh 为空 → skipped++" 对它不适用 —— 只有贴花的实体是合法的。
        if (const auto* dc = w.get<ecs::DecalComponent>(it.entity))
            je["decal"] = decalToJson(*dc);

        // 地形（W4）。它**没有网格组件** —— 几何由高度图在渲染层生成，
        // 所以同样不适用下面那句 "mesh 为空 → skipped++"。
        if (const auto* tc = w.get<ecs::TerrainComponent>(it.entity))
            je["terrain"] = terrainToJson(*tc);

        // 水体（2026-10-21）：全部参数都是场景数据，整份落盘
        if (const auto* wc = w.get<ecs::WaterComponent>(it.entity))
            je["water"] = waterToJson(*wc);

        // 玩家出生点（仿 UE5 PlayerStart）。没有网格：只有一个位置 + 朝向，
        // 视口里的 Q 版小人由编辑器叠加层画，不进场景实体。
        if (const auto* gs = w.get<ecs::GameStartComponent>(it.entity))
            je["gameStart"] = gameStartToJson(*gs);

        // 反射探针（R5-1）。与贴花同理：探针**没有网格**，
        // 下面那句 "mesh 为空 → skipped++" 对它也不适用。
        if (const auto* rp = w.get<ecs::ReflectionProbeComponent>(it.entity))
            je["probe"] = reflectionProbeToJson(*rp);

        // 蒙皮 / 骨骼动画（角色链 A0-2）。模型根实体**没有网格**，
        // 所以它同样不适用下面那句 "mesh 为空 → skipped++"。
        if (const auto* sk = w.get<ecs::SkinnedMeshComponent>(it.entity)) {
            json js = skinnedToJson(*sk);
            if (!js.is_null()) je["skinned"] = js;
            VK_LOG_INFO("SCENE-SKINNED entity=%u clip=%s speed=%.2f "
                        "playing=%d loop=%d stored=%d",
                        static_cast<unsigned>(it.entity.id),
                        sk->clipName.c_str(), static_cast<double>(sk->speed),
                        sk->playing ? 1 : 0, sk->loop ? 1 : 0,
                        js.is_null() ? 0 : 1);
        }

        // GPU 粒子发射器（R6-1）。与贴花 / 探针同理：发射器**没有网格**
        //（几何是顶点着色器现拼的 billboard），同样不适用 skipped++ 那句。
        if (const auto* fx = w.get<ecs::ParticleEmitterComponent>(it.entity))
            je["particleEmitter"] = particleEmitterToJson(*fx);

        // 刚体（R7-1）。与贴花 / 探针 / 发射器同理：它也不带网格 ——
        // "隐形的墙"就是一个只有刚体的实体，所以下面那句 skipped++ 同样
        // 要为它开一个口子。位置/朝向已经在 transform 里，这里只存参数。
        if (const auto* rb = w.get<ecs::RigidBodyComponent>(it.entity))
            je["rigidbody"] = rigidBodyToJson(*rb);

        // 动画器（角色链 A1）。挂在同一个模型根实体上，紧跟 skinned 之后。
        // 存盘顺序 = 读档顺序，所以 animatorFromJson 里的 modelFromSource
        // 一定能命中 AssetManager 缓存（键 = modelKey）。
        if (const auto* an = w.get<ecs::AnimatorComponent>(it.entity)) {
            je["animator"] = animatorToJson(*an);
            VK_LOG_INFO("SCENE-ANIMATOR entity=%u clip=%s time=%.3f "
                        "speed=%.2f playing=%d loop=%d ctrl=%d",
                        static_cast<unsigned>(it.entity.id),
                        an->defaultClip.c_str(),
                        static_cast<double>(an->time),
                        static_cast<double>(an->speed), an->playing ? 1 : 0,
                        an->loop ? 1 : 0,
                        an->controllerPath.empty() ? 0 : 1);
        }

        // 实体 transform 关键帧轨道（编辑器 Animation 模式的产物）。
        // 空轨道**不写** —— 否则每个用过的实体都会多一个 {"keys":[]}，
        // 场景文件的 diff 全是噪音。
        if (const auto* ac = w.get<ecs::AnimationClipComponent>(it.entity)) {
            if (!ac->keys.empty()) je["animClip"] = animClipToJson(*ac);
        }

        // 角色控制器（G07）。与刚体**互斥**：角色是运动学的，由
        // stepCharacter 驱动，不交给物理求解器（互斥关系在 Inspector 里
        // 会给出提示，见 InspectorPanel 的 "角色控制器" 段）。
        if (const auto* chc =
                w.get<ecs::CharacterControllerComponent>(it.entity))
            je["character"] = characterToJson(*chc);

        // 触发体（G08）/ 物理材质（G08）/ 相机（G09）
        if (const auto* trg = w.get<ecs::TriggerComponent>(it.entity))
            je["trigger"] = triggerToJson(*trg);
        if (const auto* pmat =
                w.get<ecs::PhysicsMaterialComponent>(it.entity))
            je["physicsMaterial"] = physicsMaterialToJson(*pmat);
        if (const auto* cam = w.get<ecs::CameraComponent>(it.entity))
            je["camera"] = cameraComponentToJson(*cam);

        // 动画键盘映射（P4）。空表不写（理由见 animKeyMapToJson）。
        if (const auto* akm = w.get<ecs::AnimKeyMapComponent>(it.entity)) {
            if (!akm->entries.empty()) je["animKeys"] = animKeyMapToJson(*akm);
        }

        // 脚本（只有路径与开关，脚本源码本身是独立文件）
        if (const auto* sc = w.get<ecs::ScriptComponent>(it.entity)) {
            je["script"] = json{
                {"path", sc->path},
                {"enabled", sc->enabled},
                {"timeScale", sc->timeScale},
            };
        }

        // 音频源（G14-7）。字段少 → **全写**（读端每个字段都有默认，
        // 所以旧场景不受影响；而"只写非默认值"会让"我把 volume 调回 1.0
        // 之后存档里为什么没这个键"变成一个要解释的问题）。
        if (const auto* asc = w.get<ecs::AudioSourceComponent>(it.entity)) {
            je["audio"] = json{
                {"clip", asc->clip},
                {"playOnAwake", asc->playOnAwake},
                {"loop", asc->loop},
                {"spatial", asc->spatial},
                {"volume", asc->volume},
                {"pitch", asc->pitch},
                {"minDist", asc->minDist},
                {"maxDist", asc->maxDist},
                {"rolloff", asc->rolloff},
                {"bus", asc->bus},
                {"streaming", asc->streaming},
            };
        }
        // 听者标记：只有一个 bool，直接用裸 bool（对象形式没有第二个字段可放）
        if (const auto* al = w.get<ecs::AudioListenerComponent>(it.entity))
            je["audioListener"] = al->enabled;

        // 玩家（walkPhase/bodyYaw/started 是运行态派生值，不存）
        if (const auto* pc = w.get<ecs::PlayerComponent>(it.entity)) {
            je["player"] = json{
                {"mode", pc->mode == ecs::PlayerMode::ThirdPerson
                             ? "third"
                             : "first"},
                {"eyeHeight", pc->eyeHeight},
                {"moveSpeed", pc->moveSpeed},
                {"lookSpeed", pc->lookSpeed},
                {"yaw", pc->yaw},
                {"pitch", pc->pitch},
                {"camDistance", pc->camDistance},
                {"turnSpeed", pc->turnSpeed},
            };
        }
        if (const auto* limb = w.get<ecs::PlayerLimbComponent>(it.entity))
            je["playerLimb"] = limb->kind;

        // "有没有内容"：没有网格时，贴花 / 探针 / 发射器 / 刚体 / 角色 /
        // 触发体 / 物理材质 / 相机 / **蒙皮** 任意一个存在就不算空 —— 它们
        // 都是"没有网格但有意义"的实体（隐形的墙、触发区域、相机机位、
        // 模型根节点）。
        //
        // ⚠ 这份名单必须与**读端**（loadSceneFromJson 末尾）一字不差。
        //   两处不一致的后果不是崩溃，而是 `skipped` 计数骗人：
        //   2026-11 实测，第三人称模板场景（2 个实体：地面 + 出生点）报
        //   `entities=2 skipped=1` —— 出生点被当成"空实体"，可它其实
        //   好好加载了。而回归脚本正是拿 `skipped=0` 断言"没丢东西"的，
        //   假红比不报更费时间。
        const bool hasNonMeshContent =
            je.contains("terrain") || je.contains("water") ||
            je.contains("decal") || je.contains("probe") ||
            je.contains("particleEmitter") || je.contains("rigidbody") ||
            je.contains("character") || je.contains("trigger") ||
            je.contains("physicsMaterial") || je.contains("camera") ||
            je.contains("skinned") || je.contains("animator") ||
            je.contains("animClip") || je.contains("gameStart") ||
            je.contains("animKeys") ||
            je.contains("pointLight") || je.contains("spotLight") ||
            je.contains("directionalLight") || je.contains("player") ||
            je.contains("playerLimb") || je.contains("folder");
        if (je["mesh"].is_null() && !hasNonMeshContent)
            ++r.skipped;

        root["entities"].push_back(std::move(je));
    }

    r.entities = static_cast<int>(items.size());
    return root;
}

// ============================================================
// JSON → 场景（文件版与内存版共用的核心）
// ============================================================

SceneIoResult loadSceneFromJson(Scene& scene, assets::AssetManager& assets,
                                const json& root) {
    SceneIoResult r;

    if (!root.is_object()) {
        r.error = "root JSON value is not an object";
        return r;
    }

    // 清空旧场景（AssetManager 的网格/材质/纹理缓存保留并可复用）
    scene.clear();

    applyCamera(scene.camera(), root.contains("camera") ? root["camera"]
                                                        : json(nullptr));
    // 额外相机（多相机）。顶层 "camera" 已经钉好 0 号，这里从 1 号起补。
    if (root.contains("cameras") && root["cameras"].is_array()) {
        const json& cams = root["cameras"];
        for (size_t i = 0; i < cams.size(); ++i) {
            Camera& cam = (scene.cameraCount() > i + 1)
                              ? scene.camera(i + 1)
                              : scene.camera(scene.addCamera());
            applyCamera(cam, cams[i]);
        }
    }
    applyLight(scene.light(), root.contains("light") ? root["light"]
                                                     : json(nullptr));

    // 物理世界（R7-1）。scene.clear() 已经把 m_physics 复位成默认值，
    // 所以"老场景没有 physics 字段"这条路径**什么都不用做**就是对的。
    physicsFromJson(scene.physicsSettings(),
                    root.contains("physics") ? root["physics"] : json(nullptr));

    // 后处理体积
    // 脚本世界黑板（S2）
    scene.scriptVars().clear();
    if (root.contains("scriptVars") && root["scriptVars"].is_object()) {
        for (auto it = root["scriptVars"].begin();
             it != root["scriptVars"].end(); ++it) {
            if (it.value().is_number())
                scene.scriptVars()[it.key()] = it.value().get<double>();
        }
    }
    if (root.contains("postVolumes") && root["postVolumes"].is_array()) {
        for (const json& jv : root["postVolumes"])
            scene.postVolumes().add(postVolumeFromJson(jv));
    }

    if (!root.contains("entities") || !root["entities"].is_array()) {
        r.ok = true;  // 只有相机/光照的空场景也算成功
        return r;
    }

    const json& ents = root["entities"];
    ecs::World& w = scene.world();

    // ---- 第一遍：建实体 + 名字 + 变换 + 可见性 ----
    std::vector<ecs::Entity> created;
    created.reserve(ents.size());

    for (const json& je : ents) {
        if (!je.is_object()) {
            created.push_back(ecs::Entity{});
            continue;
        }
        const std::string name = je.value("name", std::string("Entity"));
        ecs::Entity e = scene.createObject(name);

        if (je.contains("transform") && je["transform"].is_object()) {
            const json& jt = je["transform"];
            if (auto* t = w.get<ecs::TransformComponent>(e)) {
                if (jt.contains("position"))
                    t->position = vec3FromJson(jt["position"], t->position);
                if (jt.contains("rotation"))
                    t->rotation = vec3FromJson(jt["rotation"], t->rotation);
                if (jt.contains("scale"))
                    t->scale = vec3FromJson(jt["scale"], glm::vec3(1.0f));
            }
        }

        if (auto* vis = w.get<ecs::VisibilityComponent>(e)) {
            vis->visible = je.value("visible", true);
            vis->castShadow = je.value("castShadow", true);
        }

        if (je.value("locked", false)) {
            if (!w.has<ecs::LockedComponent>(e)) w.add<ecs::LockedComponent>(e);
        }

        created.push_back(e);
    }

    // ---- 第二遍：父子关系 + 网格 + 材质 ----
    // 网格放在材质之前解析：glTF 网格一加载，配套材质也就登记好了
    for (size_t i = 0; i < ents.size(); ++i) {
        const json& je = ents[i];
        if (!je.is_object()) continue;
        ecs::Entity e = created[i];
        if (!e.valid()) continue;

        const int parentIndex = je.value("parent", -1);
        if (parentIndex >= 0 &&
            parentIndex < static_cast<int>(created.size()) &&
            created[static_cast<size_t>(parentIndex)].valid()) {
            scene.setParent(e, created[static_cast<size_t>(parentIndex)]);
        }

        // "文件夹"标记
        if (je.value("folder", false)) {
            if (auto* h = w.get<ecs::HierarchyComponent>(e)) h->group = true;
        }

        assets::Mesh* mesh = resolveMesh(
            assets, je.contains("mesh") ? je["mesh"] : json(nullptr));
        assets::Material* mat = resolveMaterial(
            assets, je.contains("material") ? je["material"] : json(nullptr));

        if (mesh) {
            if (!w.has<ecs::MeshComponent>(e)) {
                ecs::MeshComponent fresh;
                fresh.mesh = mesh;
                w.add<ecs::MeshComponent>(e, fresh);
            }
            if (auto* mc = w.get<ecs::MeshComponent>(e)) {
                mc->mesh = mesh;
                // 图元生成参数（没有就是默认值 —— 老场景文件也走这条）
                if (je.contains("primitive"))
                    primitiveParamsFromJson(*mc, je["primitive"]);
            }
        }
        if (mat) {
            if (auto* matc = w.get<ecs::MaterialComponent>(e))
                matc->material = mat;
            else
                w.add<ecs::MaterialComponent>(e, ecs::MaterialComponent{mat});
        }

        // ---- 蒙皮 / 骨骼动画（A0-2）----
        // ⚠ 放在网格重建**之后**：那时 AssetManager 里模型已经热了，
        //   `loadFBX/loadGLTF` 直接命中缓存（键 = `modelKey`）。
        // ⚠ 老场景没有这个键 → 不建组件 → 逐像素等于今天。
        if (je.contains("skinned")) {
            ecs::SkinnedMeshComponent sk;
            if (skinnedFromJson(assets, je["skinned"], sk)) {
                if (auto* p = w.get<ecs::SkinnedMeshComponent>(e))
                    *p = sk;
                else
                    w.add<ecs::SkinnedMeshComponent>(e, sk);
                VK_LOG_INFO("SCENE-SKINNED entity=%u clip=%s speed=%.2f "
                            "playing=%d loop=%d time=%.3f joints=%d ok=1",
                            static_cast<unsigned>(e.id), sk.clipName.c_str(),
                            static_cast<double>(sk.speed), sk.playing ? 1 : 0,
                            sk.loop ? 1 : 0, static_cast<double>(sk.time),
                            sk.model ? sk.model->skeleton->jointCount() : 0);
            } else {
                VK_LOG_INFO("SCENE-SKINNED entity=%u ok=0",
                            static_cast<unsigned>(e.id));
            }
        }

        // ---- 动画器（角色链 A1）----
        // 必须在 skinned **之后**：它的 model 可以为空，靠 AnimatorSystem
        // 回退到刚建好的 SkinnedMeshComponent::model。
        // ⚠ 老场景没有这个键 → 不建组件 → 走 A0 老路径，逐像素不变。
        if (je.contains("animator")) {
            ecs::AnimatorComponent an;
            if (animatorFromJson(assets, je["animator"], an)) {
                if (auto* p = w.get<ecs::AnimatorComponent>(e))
                    *p = an;
                else
                    w.add<ecs::AnimatorComponent>(e, an);
                VK_LOG_INFO("SCENE-ANIMATOR entity=%u clip=%s time=%.3f "
                            "speed=%.2f playing=%d loop=%d model=%d ok=1",
                            static_cast<unsigned>(e.id),
                            an.defaultClip.c_str(),
                            static_cast<double>(an.time),
                            static_cast<double>(an.speed),
                            an.playing ? 1 : 0, an.loop ? 1 : 0,
                            an.model ? 1 : 0);
            } else {
                VK_LOG_INFO("SCENE-ANIMATOR entity=%u ok=0",
                            static_cast<unsigned>(e.id));
            }
        }

        // 实体 transform 关键帧轨道
        if (je.contains("animClip")) {
            const ecs::AnimationClipComponent ac =
                animClipFromJson(je["animClip"]);
            if (auto* p = w.get<ecs::AnimationClipComponent>(e))
                *p = ac;
            else
                w.add<ecs::AnimationClipComponent>(e, ac);
            // 读回来必须说清楚"读到几条、多长"：用户存完再开发现关键帧
            // 没了，这条日志是唯一能分辨"没写进去"和"写了但被覆盖"的地方。
            VK_LOG_INFO("SCENE-ANIMCLIP entity=%u keys=%d dur=%.2f loop=%d "
                        "autoplay=%d time=%.2f",
                        static_cast<unsigned>(e.id),
                        static_cast<int>(ac.keys.size()),
                        static_cast<double>(ac.duration), ac.loop ? 1 : 0,
                        ac.autoplay ? 1 : 0, static_cast<double>(ac.time));
        }

        // 局部光源组件
        if (je.contains("pointLight")) {
            const ecs::PointLightComponent pl =
                pointLightFromJson(je["pointLight"]);
            if (auto* p = w.get<ecs::PointLightComponent>(e))
                *p = pl;
            else
                w.add<ecs::PointLightComponent>(e, pl);
        }
        if (je.contains("spotLight")) {
            const ecs::SpotLightComponent sl =
                spotLightFromJson(je["spotLight"]);
            if (auto* p = w.get<ecs::SpotLightComponent>(e))
                *p = sl;
            else
                w.add<ecs::SpotLightComponent>(e, sl);
        }
        if (je.contains("directionalLight") &&
            je["directionalLight"].is_object()) {
            const ecs::DirectionalLightComponent dl =
                directionalLightFromJson(je["directionalLight"]);
            if (auto* p = w.get<ecs::DirectionalLightComponent>(e))
                *p = dl;
            else
                w.add<ecs::DirectionalLightComponent>(e, dl);
        }

        // 脚本
        if (je.contains("script") && je["script"].is_object()) {
            const json& js = je["script"];
            ecs::ScriptComponent sc;
            sc.path = js.value("path", std::string());
            sc.enabled = js.value("enabled", true);
            sc.timeScale = js.value("timeScale", 1.0f);
            if (!sc.path.empty()) {
                if (auto* p = w.get<ecs::ScriptComponent>(e))
                    *p = sc;
                else
                    w.add<ecs::ScriptComponent>(e, sc);
            }
        }

        // 音频源（G14-7）
        //
        // ⚠ `clip` 为空也照样把组件加上：那是编辑器里"刚点击 Add Audio
        //   Source、还没选素材"的正常中间态。跳过它会让"加完组件存盘再
        //   打开就没了"，而用户完全看不出原因。
        if (je.contains("audio") && je["audio"].is_object()) {
            const json& ja = je["audio"];
            ecs::AudioSourceComponent as;
            as.clip = ja.value("clip", std::string());
            as.playOnAwake = ja.value("playOnAwake", as.playOnAwake);
            as.loop = ja.value("loop", as.loop);
            as.spatial = ja.value("spatial", as.spatial);
            as.volume = ja.value("volume", as.volume);
            as.pitch = ja.value("pitch", as.pitch);
            as.minDist = ja.value("minDist", as.minDist);
            as.maxDist = ja.value("maxDist", as.maxDist);
            as.rolloff = ja.value("rolloff", as.rolloff);
            as.bus = ja.value("bus", as.bus);
            as.streaming = ja.value("streaming", as.streaming);
            if (auto* p = w.get<ecs::AudioSourceComponent>(e))
                *p = as;
            else
                w.add<ecs::AudioSourceComponent>(e, as);
        }
        if (je.contains("audioListener") && je["audioListener"].is_boolean()) {
            ecs::AudioListenerComponent al;
            al.enabled = je["audioListener"].get<bool>();
            if (auto* p = w.get<ecs::AudioListenerComponent>(e))
                *p = al;
            else
                w.add<ecs::AudioListenerComponent>(e, al);
        }

        // 玩家 / 肢体标记
        if (je.contains("player") && je["player"].is_object()) {
            const json& jp = je["player"];
            ecs::PlayerComponent pc;
            pc.mode = (jp.value("mode", std::string("first")) == "third")
                          ? ecs::PlayerMode::ThirdPerson
                          : ecs::PlayerMode::FirstPerson;
            pc.eyeHeight = jp.value("eyeHeight", pc.eyeHeight);
            pc.moveSpeed = jp.value("moveSpeed", pc.moveSpeed);
            pc.lookSpeed = jp.value("lookSpeed", pc.lookSpeed);
            pc.yaw = jp.value("yaw", pc.yaw);
            pc.pitch = jp.value("pitch", pc.pitch);
            pc.camDistance = jp.value("camDistance", pc.camDistance);
            pc.turnSpeed = jp.value("turnSpeed", pc.turnSpeed);
            if (auto* p = w.get<ecs::PlayerComponent>(e))
                *p = pc;
            else
                w.add<ecs::PlayerComponent>(e, pc);
        }
        if (je.contains("playerLimb") && je["playerLimb"].is_number_integer()) {
            ecs::PlayerLimbComponent limb;
            limb.kind = je["playerLimb"].get<int>();
            if (auto* p = w.get<ecs::PlayerLimbComponent>(e))
                p->kind = limb.kind;
            else
                w.add<ecs::PlayerLimbComponent>(e, limb);
        }

        // 碰撞体。position/rotation/scale 从文件读；**凸包顶点按网格重建**
        // —— 它没进 JSON（见 collisionToJson 的说明）。
        if (je.contains("collision") && je["collision"].is_object()) {
            ecs::CollisionComponent cc = collisionFromJson(je["collision"]);
            if (cc.shape == ecs::ColliderShape::ConvexHull) {
                const auto* mc = w.get<ecs::MeshComponent>(e);
                if (mc && mc->mesh) physics::refreshHullGeometry(*mc->mesh, cc);
            }
            if (auto* p = w.get<ecs::CollisionComponent>(e))
                *p = cc;
            else
                w.add<ecs::CollisionComponent>(e, cc);
        }

        // 延迟贴花（R4-4）。组件只描述"盒体 + 参数 + 贴图"—— 盒体几何
        // 由渲染器自建，所以这里不碰 MeshComponent（贴花实体通常没有网格）。
        if (je.contains("decal") && je["decal"].is_object()) {
            const ecs::DecalComponent dc = decalFromJson(assets, je["decal"]);
            if (auto* p = w.get<ecs::DecalComponent>(e))
                *p = dc;
            else
                w.add<ecs::DecalComponent>(e, dc);
        }

        // 地形（W4）。读盘只恢复组件；GPU 资源由渲染器在
        // syncTerrains 里按需建（见 render/Renderer.cpp）。
        if (je.contains("terrain") && je["terrain"].is_object()) {
            const ecs::TerrainComponent tc = terrainFromJson(je["terrain"]);
            if (auto* p = w.get<ecs::TerrainComponent>(e))
                *p = tc;
            else
                w.add<ecs::TerrainComponent>(e, tc);
        }

        // 水体（2026-10-21）。没有资产：读盘就把整块参数塞回组件，
        // GPU 资源由渲染器在 syncWaters 里按形状现建。
        if (je.contains("water") && je["water"].is_object()) {
            const ecs::WaterComponent wc = waterFromJson(je["water"]);
            if (auto* p = w.get<ecs::WaterComponent>(e))
                *p = wc;
            else
                w.add<ecs::WaterComponent>(e, wc);
        }

        // 玩家出生点（GameStart）
        if (je.contains("gameStart") && je["gameStart"].is_object()) {
            const ecs::GameStartComponent gs =
                gameStartFromJson(je["gameStart"]);
            if (auto* p = w.get<ecs::GameStartComponent>(e))
                *p = gs;
            else
                w.add<ecs::GameStartComponent>(e, gs);
        }

        // 反射探针（R5-1）。与贴花同一形态：实体只提供变换 + 参数，
        // 立方体贴图由渲染器烘（不落盘）。
        if (je.contains("probe") && je["probe"].is_object()) {
            const ecs::ReflectionProbeComponent rp =
                reflectionProbeFromJson(je["probe"]);
            if (auto* p = w.get<ecs::ReflectionProbeComponent>(e))
                *p = rp;
            else
                w.add<ecs::ReflectionProbeComponent>(e, rp);
        }

        // GPU 粒子发射器（R6-1）。粒子本身**不落盘**（瞬时状态），
        // 存的是发射器的配置；打开场景时从帧号 1 重新发射。
        if (je.contains("particleEmitter") &&
            je["particleEmitter"].is_object()) {
            const ecs::ParticleEmitterComponent fx =
                particleEmitterFromJson(je["particleEmitter"]);
            if (auto* p = w.get<ecs::ParticleEmitterComponent>(e))
                *p = fx;
            else
                w.add<ecs::ParticleEmitterComponent>(e, fx);
        }

        // 刚体（R7-1）。只是**数据**：真正的 physics::RigidBody 由编辑器在
        // 进入 Play 时按它建（见 EditorApp::buildPhysicsWorld）。编辑态读盘
        // 之后场景里就多了一个"待仿真的物体"，仅此而已。
        if (je.contains("rigidbody") && je["rigidbody"].is_object()) {
            const ecs::RigidBodyComponent rb = rigidBodyFromJson(je["rigidbody"]);
            if (auto* p = w.get<ecs::RigidBodyComponent>(e))
                *p = rb;
            else
                w.add<ecs::RigidBodyComponent>(e, rb);
        }

        // 角色控制器（G07）/ 触发体 / 物理材质（G08）/ 相机（G09）。
        // 全是纯数据：真正的行为由运行时或编辑器在 Play 时消费。
        if (je.contains("character") && je["character"].is_object()) {
            const ecs::CharacterControllerComponent ch =
                characterFromJson(je["character"]);
            if (auto* p = w.get<ecs::CharacterControllerComponent>(e))
                *p = ch;
            else
                w.add<ecs::CharacterControllerComponent>(e, ch);
        }
        if (je.contains("trigger") && je["trigger"].is_object()) {
            const ecs::TriggerComponent tr = triggerFromJson(je["trigger"]);
            if (auto* p = w.get<ecs::TriggerComponent>(e))
                *p = tr;
            else
                w.add<ecs::TriggerComponent>(e, tr);
        }
        if (je.contains("physicsMaterial") && je["physicsMaterial"].is_object()) {
            const ecs::PhysicsMaterialComponent pm =
                physicsMaterialFromJson(je["physicsMaterial"]);
            if (auto* p = w.get<ecs::PhysicsMaterialComponent>(e))
                *p = pm;
            else
                w.add<ecs::PhysicsMaterialComponent>(e, pm);
        }
        if (je.contains("camera") && je["camera"].is_object()) {
            const ecs::CameraComponent cm =
                cameraComponentFromJson(je["camera"]);
            if (auto* p = w.get<ecs::CameraComponent>(e))
                *p = cm;
            else
                w.add<ecs::CameraComponent>(e, cm);
        }

        // 动画键盘映射（P4）。与 skinned 同一实体；缺块 = 没绑过键（正常）。
        if (je.contains("animKeys") && je["animKeys"].is_object()) {
            const ecs::AnimKeyMapComponent akm =
                animKeyMapFromJson(je["animKeys"]);
            if (auto* p = w.get<ecs::AnimKeyMapComponent>(e))
                *p = akm;
            else
                w.add<ecs::AnimKeyMapComponent>(e, akm);
        }

        // ⚠ 与写端（saveSceneFromJson 里那份）必须一字不差，理由见那里。
        const bool hasNonMeshContent =
            je.contains("terrain") || je.contains("water") ||
            je.contains("decal") || je.contains("probe") ||
            je.contains("particleEmitter") || je.contains("rigidbody") ||
            je.contains("character") || je.contains("trigger") ||
            je.contains("physicsMaterial") || je.contains("camera") ||
            je.contains("skinned") || je.contains("animator") ||
            je.contains("animClip") || je.contains("gameStart") ||
            je.contains("animKeys") ||
            je.contains("pointLight") || je.contains("spotLight") ||
            je.contains("directionalLight") || je.contains("player") ||
            je.contains("playerLimb") || je.contains("folder");
        if (!mesh && !mat && !hasNonMeshContent) ++r.skipped;
    }

    r.ok = true;
    r.entities = static_cast<int>(created.size());
    return r;
}

} // namespace

// ============================================================
// 公开接口：单独加载一份 .vmat
// ============================================================
// 只是把"场景里写成一个 vmat 块"这件事从 JSON 里搬到函数参数上 ——
// 真正的活儿全在 resolveMaterial 的 vmat 分支里（id 去重 / 贴图解析 /
// 导入默认值）。这样"手动赋材质"与"场景引用材质"走的**一定是同一条路**，
// 不会出现两套 bug。
assets::Material* loadMaterialAsset(assets::AssetManager& am,
                                    const std::string& relPath) {
    if (relPath.empty()) return nullptr;
    json jm;
    jm["kind"] = "vmat";
    jm["path"] = relPath;
    return resolveMaterial(am, jm);
}

// ============================================================
// 公开接口：文件版
// ============================================================

SceneIoResult saveScene(const Scene& scene, const std::string& path) {
    SceneIoResult r;
    const json root = buildSceneJson(scene, r);

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        r.error = "cannot open for write: " + path;
        return r;
    }
    out << root.dump(2) << '\n';
    if (!out) {
        r.error = "write failed: " + path;
        return r;
    }

    r.ok = true;
    VK_LOG_INFO("Scene saved: %s (%d entities, %d skipped)", path.c_str(),
                r.entities, r.skipped);
    return r;
}

SceneIoResult loadScene(Scene& scene, assets::AssetManager& assets,
                        const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        SceneIoResult r;
        r.error = "cannot open: " + path;
        return r;
    }

    json root;
    try {
        in >> root;
    } catch (const std::exception& ex) {
        SceneIoResult r;
        r.error = std::string("JSON parse error: ") + ex.what();
        return r;
    }

    SceneIoResult r = loadSceneFromJson(scene, assets, root);
    if (r.ok) {
        VK_LOG_INFO("Scene loaded: %s (%d entities, %d skipped)", path.c_str(),
                    r.entities, r.skipped);
    }
    return r;
}

// ============================================================
// 公开接口：内存版（编辑器）
// ============================================================

SceneIoResult sceneToJson(const Scene& scene, std::string& outJson) {
    SceneIoResult r;
    const json root = buildSceneJson(scene, r);
    try {
        outJson = root.dump(2);
    } catch (const std::exception& ex) {
        r.error = std::string("json dump failed: ") + ex.what();
        return r;
    }
    r.ok = true;
    return r;
}

SceneIoResult sceneFromJson(Scene& scene, assets::AssetManager& assets,
                            const std::string& jsonText) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const std::exception& ex) {
        SceneIoResult r;
        r.error = std::string("JSON parse error: ") + ex.what();
        return r;
    }
    SceneIoResult r = loadSceneFromJson(scene, assets, root);
    if (r.ok) {
        VK_LOG_INFO("Scene restored from memory (%d entities, %d skipped)",
                    r.entities, r.skipped);
    }
    return r;
}

} // namespace scene
