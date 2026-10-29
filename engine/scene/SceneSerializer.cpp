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
#include "scene/SceneDocument.h"
#include "scene/SceneEntityIO.h"

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

// ⚠ 真正的定义在 `scene/SceneDocument.h`（`kSceneFormatVersion`）。这里只
//   起一个匿名 namespace 内的别名，让下面 4 处调用点保持原样。
//   两份字面量会漂，一个别名不会。
constexpr int kFormatVersion = kSceneFormatVersion;

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
        j["shadingModel"] = M::shadingModelName(p.shadingModel);
    if (bits & M::kOvDoubleSided) j["doubleSided"] = p.doubleSided;
    if (bits & M::kOvHeightScale) j["heightScale"] = p.heightScale;
    // 渲染顺序（P0-2/B）。只在被覆盖时写 —— 与上面同一条规则：
    // overrides 里只出现"用户真的改过、或者该跟随母材质"的字段。
    if (bits & M::kOvRenderQueue) j["renderQueue"] = p.renderQueue;
    if (bits & M::kOvSortOrder) j["sortOrder"] = p.sortOrder;
    // A 类语义槽的标量（2026-10-25）。与上面同一条规则：只写被覆盖的。
    if (bits & M::kOvRampSteps) j["rampSteps"] = p.rampSteps;
    if (bits & M::kOvMatcapStrength) j["matcapStrength"] = p.matcapStrength;
    if (bits & M::kOvMatcapRotate) j["matcapRotate"] = p.matcapRotate;
    if (bits & M::kOvSdfShadowStrength)
        j["sdfShadowStrength"] = p.sdfShadowStrength;
    if (bits & M::kOvDetailStrength) j["detailStrength"] = p.detailStrength;
    // A6（2026-10-26）：色带的交界位置/宽度
    if (bits & M::kOvRampEdge) j["rampEdge"] = p.rampEdge;
    if (bits & M::kOvRampSoftness) j["rampSoftness"] = p.rampSoftness;
    if (bits & M::kOvAnisoStrength) j["anisoStrength"] = p.anisoStrength;
    if (bits & M::kOvAnisoShift) j["anisoShift"] = p.anisoShift;
    // A3（2026-10-26）：眼睛的 unlit 权重 / 虹膜视差 / 眼神光
    if (bits & M::kOvEyeStrength) j["eyeStrength"] = p.eyeStrength;
    if (bits & M::kOvEyeIrisDepth) j["eyeIrisDepth"] = p.eyeIrisDepth;
    if (bits & M::kOvEyeCatchlight) j["eyeCatchlight"] = p.eyeCatchlight;
    if (bits & M::kOvDetailTiling) j["detailTiling"] = p.detailTiling;
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
        // 字符串 → 枚举走 Material 的同名函数：新增着色模型（如 Toon）
        // 只改那一处，序列化/Inspector/材质导入三条链自动跟上。
        p.shadingModel =
            M::shadingModelFromName(jo.value("shadingModel", std::string()));
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
    // 渲染顺序（P0-2/B）。⚠ 用 int 读而不是 value<int32_t> 的默认：
    // 老场景没有这两个键 → contains() 为 false → 位不亮 → 参数保持
    // importDefaults/母材质的值，正好是"没改过"的语义。
    if (jo.contains("renderQueue")) {
        p.renderQueue = jo.value("renderQueue", p.renderQueue);
        bits |= M::kOvRenderQueue;
    }
    if (jo.contains("sortOrder")) {
        p.sortOrder = jo.value("sortOrder", p.sortOrder);
        bits |= M::kOvSortOrder;
    }
    // A 类语义槽（2026-10-25）。同样是"键在 = 被覆盖"，缺席不亮位。
    if (jo.contains("rampSteps")) {
        p.rampSteps = jo.value("rampSteps", p.rampSteps);
        bits |= M::kOvRampSteps;
    }
    if (jo.contains("matcapStrength")) {
        p.matcapStrength = jo.value("matcapStrength", p.matcapStrength);
        bits |= M::kOvMatcapStrength;
    }
    if (jo.contains("matcapRotate")) {
        p.matcapRotate = jo.value("matcapRotate", p.matcapRotate);
        bits |= M::kOvMatcapRotate;
    }
    if (jo.contains("sdfShadowStrength")) {
        p.sdfShadowStrength = jo.value("sdfShadowStrength", p.sdfShadowStrength);
        bits |= M::kOvSdfShadowStrength;
    }
    if (jo.contains("detailStrength")) {
        p.detailStrength = jo.value("detailStrength", p.detailStrength);
        bits |= M::kOvDetailStrength;
    }
    if (jo.contains("detailTiling")) {
        p.detailTiling = jo.value("detailTiling", p.detailTiling);
        bits |= M::kOvDetailTiling;
    }
    // A6（2026-10-26）
    if (jo.contains("rampEdge")) {
        p.rampEdge = jo.value("rampEdge", p.rampEdge);
        bits |= M::kOvRampEdge;
    }
    if (jo.contains("rampSoftness")) {
        p.rampSoftness = jo.value("rampSoftness", p.rampSoftness);
        bits |= M::kOvRampSoftness;
    }
    // A4（2026-10-26）：头发各向异性强度 / 环位置
    if (jo.contains("anisoStrength")) {
        p.anisoStrength = jo.value("anisoStrength", p.anisoStrength);
        bits |= M::kOvAnisoStrength;
    }
    if (jo.contains("anisoShift")) {
        p.anisoShift = jo.value("anisoShift", p.anisoShift);
        bits |= M::kOvAnisoShift;
    }
    // A3（2026-10-26）：眼睛三字段
    if (jo.contains("eyeStrength")) {
        p.eyeStrength = jo.value("eyeStrength", p.eyeStrength);
        bits |= M::kOvEyeStrength;
    }
    if (jo.contains("eyeIrisDepth")) {
        p.eyeIrisDepth = jo.value("eyeIrisDepth", p.eyeIrisDepth);
        bits |= M::kOvEyeIrisDepth;
    }
    if (jo.contains("eyeCatchlight")) {
        p.eyeCatchlight = jo.value("eyeCatchlight", p.eyeCatchlight);
        bits |= M::kOvEyeCatchlight;
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
    // A 类语义槽（2026-10-25）。与上面同一条规则：只写带 TextureSource
    // 的（用户手工换上的），模型/资产自带的不写 —— 写出来是 null，
    // 读回来不该去覆盖它。
    put("rampMap", mat->rampMap);
    put("matcapMap", mat->matcapMap);
    put("sdfShadowMap", mat->sdfShadowMap);
    put("detailMap", mat->detailMap);
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
    // 着色模型（R4-2 / A1）：写字符串。"default" 不写也能读（见 resolveMaterial），
    // 但显式写出让人一眼看得出这个材质走哪条漫反射路径。
    // 用 Material 的枚举→名字函数，新增模型（Toon）不必再回来改这里 ——
    // 这里曾经是 `== Skin ? "skin" : "default"`，加 Toon 时会静默存成 default。
    j["shadingModel"] = assets::Material::shadingModelName(mat->shadingModel);
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
    // 渲染顺序（P0-2/B）。全量写（与 heightScale/alphaCutoff 同风格）：
    // 默认的 2000/0 也写出来，读回来还是 2000/0，所以"写得多"不影响正确性，
    // 换来的是"打开文件就能看见每个部件排在第几层"—— 二次元角色的排序
    // 是美术要反复调的东西，藏在默认值里反而更难查。
    j["renderQueue"] = mat->renderQueue;
    j["sortOrder"] = mat->sortOrder;
    // A 类语义槽（2026-10-25）。全量写（与 heightScale / renderQueue 同风格）：
    // 默认值也写出来，读回来还是默认值，所以"写得多"不影响正确性；
    // 换来的是打开文件就能看见"这个角色的明暗带是几档"。
    j["rampMap"] = textureSourceToJson(mat->rampMap);
    j["rampSteps"] = mat->rampSteps;
    j["matcapMap"] = textureSourceToJson(mat->matcapMap);
    j["matcapStrength"] = mat->matcapStrength;
    j["matcapRotate"] = mat->matcapRotate;
    j["sdfShadowMap"] = textureSourceToJson(mat->sdfShadowMap);
    j["sdfShadowStrength"] = mat->sdfShadowStrength;
    j["detailMap"] = textureSourceToJson(mat->detailMap);
    j["detailStrength"] = mat->detailStrength;
    j["detailTiling"] = mat->detailTiling;
    // A6/A1/A5（2026-10-26）。全量写（同 heightScale 的风格）。
    // ⚠ `rampPath` 是**相对 assets/ 的路径**，不是缓存键 —— 缓存键带
    //   加载时的 name 前缀，写进存档换台机器就找不到了。
    j["rampPath"] = mat->rampPath;
    j["rampEdge"] = mat->rampEdge;
    j["rampSoftness"] = mat->rampSoftness;
    j["sdfFaceShift"] = mat->sdfFaceShift;
    j["useCharacterLight"] = mat->useCharacterLight;
    // A4（2026-10-26）：头发各向异性。**只在非 0 时写**（0 是缺省 = 关），
    // 这样没开这个特性的场景 JSON 与引入之前逐字节相同 —— 老场景不会被
    // 多出两行"看起来像改过"的字段。
    if (mat->anisoStrength != 0.0f) j["anisoStrength"] = mat->anisoStrength;
    if (mat->anisoShift != 0.0f) j["anisoShift"] = mat->anisoShift;
    // A3（2026-10-26）：眼睛。同样**只在非 0 时写** —— 三个字段的缺省都是 0
    // （= 不吃眼睛通道），所以没配眼睛的场景与引入之前逐字节相同。
    if (mat->eyeStrength != 0.0f) j["eyeStrength"] = mat->eyeStrength;
    if (mat->eyeIrisDepth != 0.0f) j["eyeIrisDepth"] = mat->eyeIrisDepth;
    if (mat->eyeCatchlight != 0.0f) j["eyeCatchlight"] = mat->eyeCatchlight;
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
        if (auto* t = get("rampMap")) m->rampMap = t;
        if (auto* t = get("matcapMap")) m->matcapMap = t;
        if (auto* t = get("sdfShadowMap")) m->sdfShadowMap = t;
        if (auto* t = get("detailMap")) m->detailMap = t;
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
    // A 类语义槽（2026-10-25）。缺键 → resolveTexture 拿到 nullptr →
    // 页表里兜底成白图 → 乘法/加法恒等 → 老场景逐像素不变。
    assets::Texture* rampMap = resolveTexture(
        am, jm.contains("rampMap") ? jm["rampMap"] : json(nullptr));
    assets::Texture* matcapMap = resolveTexture(
        am, jm.contains("matcapMap") ? jm["matcapMap"] : json(nullptr));
    assets::Texture* sdfShadowMap = resolveTexture(
        am, jm.contains("sdfShadowMap") ? jm["sdfShadowMap"] : json(nullptr));
    assets::Texture* detailMap = resolveTexture(
        am, jm.contains("detailMap") ? jm["detailMap"] : json(nullptr));

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
        // A 类语义槽（2026-10-25）
        mat->rampMap = rampMap;
        mat->rampSteps = jm.value("rampSteps", 0.0f);
        mat->matcapMap = matcapMap;
        mat->matcapStrength = jm.value("matcapStrength", 0.0f);
        mat->matcapRotate = jm.value("matcapRotate", 0.0f);
        mat->sdfShadowMap = sdfShadowMap;
        mat->sdfShadowStrength = jm.value("sdfShadowStrength", 1.0f);
        mat->detailMap = detailMap;
        mat->detailStrength = jm.value("detailStrength", 1.0f);
        mat->detailTiling = jm.value("detailTiling", 1.0f);
        mat->normalScale = normalScale;
        mat->ao = ao;
        mat->alphaMode = jm.value("alphaBlend", false)
                             ? assets::Material::AlphaMode::Blend
                             : assets::Material::AlphaMode::Opaque;
        // alphaMode 字符串优先（新格式）。缺席时退回上面的 alphaBlend 布尔
        // —— 老场景文件没有这个字段，不能因此把 Mask/Blend 读丢。
        // ⚠ 变量名**不能叫 am** —— 那是本函数的参数（AssetManager&），
        //   遮蔽之后 `am.loadRampAsset(...)` 会编译成 "std::string 没有
        //   这个成员"（2026-10-26 实测踩过）。
        const std::string alphaModeStr = jm.value("alphaMode", std::string());
        if (alphaModeStr == "blend") {
            mat->alphaMode = assets::Material::AlphaMode::Blend;
        } else if (alphaModeStr == "mask") {
            mat->alphaMode = assets::Material::AlphaMode::Mask;
        } else if (alphaModeStr == "opaque") {
            mat->alphaMode = assets::Material::AlphaMode::Opaque;
        }
        mat->alphaCutoff = jm.value("alphaCutoff", 0.5f);
        mat->ditherAlpha = jm.value("ditherAlpha", false);
        // 着色模型（R4-2 / A1）。缺席 = 老场景 = Default（逐像素不变）。
        // 走 Material 的字符串→枚举函数：这里曾经是
        // `== "skin" ? Skin : Default`，新增 Toon 时会**静默读丢** ——
        // 场景里写着 "toon"、读回来变成 Default，画面看着像"Toon 没生效"。
        mat->shadingModel = assets::Material::shadingModelFromName(
            jm.value("shadingModel", std::string()));
        // 渲染顺序（P0-2/B）。缺席 = 老场景 = 2000/0 = 只按距离排，
        // 与改造前逐像素一致。
        mat->renderQueue = jm.value("renderQueue", mat->renderQueue);
        mat->sortOrder = jm.value("sortOrder", mat->sortOrder);
        // A6/A1/A5（2026-10-26）
        mat->rampEdge = jm.value("rampEdge", mat->rampEdge);
        mat->rampSoftness = jm.value("rampSoftness", mat->rampSoftness);
        mat->sdfFaceShift = jm.value("sdfFaceShift", mat->sdfFaceShift);
        mat->useCharacterLight = jm.value("useCharacterLight", false);
        // A4（2026-10-26）：头发各向异性。缺省 0 = 关，所以老场景读回来
        // 就是"关"，与引入这条通道之前逐像素相同。
        mat->anisoStrength = jm.value("anisoStrength", 0.0f);
        mat->anisoShift = jm.value("anisoShift", 0.0f);
        // A3（2026-10-26）：眼睛。缺省全是 0 = 关 → 老场景读回来与引入
        // 这条通道之前逐像素相同。
        mat->eyeStrength = jm.value("eyeStrength", 0.0f);
        mat->eyeIrisDepth = jm.value("eyeIrisDepth", 0.0f);
        mat->eyeCatchlight = jm.value("eyeCatchlight", 0.0f);
        // 色带资产：**有 rampPath 就以它为准**，把 rampMap 换成资产烘出来的
        // 纹理。理由：rampMap 存的是"纹理来源"（file/solid/checker），而色带
        // 是**程序化烘出来的**，那条路根本表达不了 bands —— 只靠 rampMap
        // 往返会把色带退化成一张静态图，编辑界面上再也改不动。
        // 顺序上必须在 `mat->rampMap = rampMap;` 之后（见上）。
        const std::string rampPath = jm.value("rampPath", std::string());
        if (!rampPath.empty()) {
            if (assets::RampAsset* ra = am.loadRampAsset(rampPath)) {
                mat->ramp = ra;
                mat->rampPath = rampPath;
                mat->rampMap = ra->texture.get();
            } else {
                // 资产丢了：保留 rampMap（可能是一张手工贴的渐变图），
                // 但清掉 ramp 指针，免得界面显示"有色带"却改不动。
                mat->ramp = nullptr;
                mat->rampPath.clear();
            }
        }
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
// 表情滑条（§15，2026-10-25）
// ------------------------------------------------------------
//
// 只存两件事：模型来源 + 每个滑条的当前值。
//
// ⚠ 权重**按名字**存，不按下标。
//   下标是 `MorphRig` 建表顺序（子网格序 × channel 序），重烤一次 FBX
//   就可能变（美术加了一个 shape、材质桶顺序变了都会重排）。按下标存
//   的后果是"表情没报错、但套到了另一根滑条上"—— 比丢掉更糟。
//   名字是唯一的语义键，也是 `setMorphWeight` 用的键。
//
// ⚠ 值为 0 的滑条**不写**：一只角色 49 根滑条、绝大多数时候全是 0，
//   全写进去会把场景 JSON 撑出 49 行噪音。读回来时缺的键就是 0。
json morphToJson(const ecs::MorphComponent& mp) {
    json src = modelSourceToJson(mp.model);
    if (src.is_null()) return nullptr;
    json j = std::move(src);
    json w = json::object();
    if (mp.model) {
        const assets::MorphRig& rig = mp.model->morphRig;
        const std::size_t n = std::min(mp.weights.size(), rig.channelCount());
        for (std::size_t c = 0; c < n; ++c) {
            if (mp.weights[c] == 0.0f) continue;
            w[rig.channels()[c].name] = mp.weights[c];
        }
    }
    if (!w.empty()) j["weights"] = std::move(w);
    return j;
}

// 返回 false = 重建失败（资产缺失 / 这个模型没有 morph）→ 调用方不建组件。
bool morphFromJson(assets::AssetManager& am, const json& j,
                   ecs::MorphComponent& out) {
    if (!j.is_object()) return false;
    assets::Model* model = modelFromSource(am, j, "morph");
    if (!model) return false;
    // 资产换了（比如美术重新导了个没有 blend shape 的版本）→ 不建组件，
    // 而不是建一个"49 根滑条一条也不驱动"的空壳。
    if (model->morphRig.empty()) return false;

    out.model = model;
    out.weights.assign(model->morphRig.channelCount(), 0.0f);
    if (j.contains("weights") && j["weights"].is_object()) {
        for (auto it = j["weights"].begin(); it != j["weights"].end(); ++it) {
            // 认不出的名字**忽略**而不是报错：重烤后滑条改名是很常见的
            // （target_0 → Blink_smile），为这个拒绝整张场景没道理。
            const int ci = model->morphRig.channelIndex(it.key());
            if (ci < 0) continue;
            if (it.value().is_number()) {
                out.weights[static_cast<std::size_t>(ci)] =
                    it.value().get<float>();
            }
        }
    }
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
    // 根运动骨名（B3）：**只在非空时写** —— 老场景没有这个键，输出逐字节
    // 不变。空 = 自动，而"自动"是读档时的同一份默认值，所以不写也是对的。
    if (!an.rootMotionBone.empty()) j["rootMotionBone"] = an.rootMotionBone;
    // 骨架来源只在**显式给过**的时候才写：空 = 用 skinned 那一份。
    // 写一个空对象会让读档的人以为"这里本来有东西被清掉了"。
    json src = modelSourceToJson(an.model);
    if (!src.is_null()) j["model"] = std::move(src);

    // ---- 附加动画层（B1，2026-10-26）----
    // ⚠ **只在非空时写这个键**：老场景没有 `layers` → 输出逐字节不变，
    //   回归快照与"存档 diff"都不受影响。这与 SkinnedMesh 的 fade 四件套
    //   同一个纪律。
    // ⚠ 运行时字段（playingClip / time / fadeT）**一律不写**：它们是"这一
    //   帧正在发生的事"，读档时恢复半个淡入比不复原更怪；而且它们每帧都在
    //   变，写进去会让 `structuralEdit` 的快照前后永不相等（每次改一个
    //   滑块都会进一条"什么都没变"的撤销记录）。
    if (!an.layers.empty()) {
        json arr = json::array();
        for (const ecs::AnimLayer& L : an.layers) {
            json o;
            o["name"] = L.name;
            o["clip"] = L.clip;
            o["weight"] = L.weight;
            o["speed"] = L.speed;
            o["loop"] = L.loop;
            o["playing"] = L.playing;
            o["fadeDur"] = L.fadeDur;
            // 遮罩：All + 空 roots 是默认值，不写（读回来也是它）
            if (!L.mask.isAll()) {
                json m;
                m["mode"] = static_cast<int>(L.mask.mode);
                m["roots"] = L.mask.roots;
                o["mask"] = std::move(m);
            }
            arr.push_back(std::move(o));
        }
        j["layers"] = std::move(arr);
    }

    // ---- 动画事件（B2，2026-10-26）----
    // 同一条纪律：**只在非空时写**（老场景逐字节不变）。每段 clip 一个对象，
    // 空事件表也跳过 —— 一个只有 clip 名的空表在语义上和"没有这段"完全一样，
    // 写出去只会在存档 diff 里多一行噪音。
    if (!an.eventTracks.empty()) {
        json arr = json::array();
        for (const animation::AnimEventTrack& tr : an.eventTracks) {
            if (tr.clip.empty() || tr.events.empty()) continue;
            json evs = json::array();
            for (const animation::AnimEvent& e : tr.events) {
                json eo;
                eo["t"] = e.time;
                eo["kind"] = e.kind;
                // payload 空 → 不写（绝大多数事件没有参数）
                if (!e.payload.empty()) eo["payload"] = e.payload;
                evs.push_back(std::move(eo));
            }
            json to;
            to["clip"] = tr.clip;
            to["events"] = std::move(evs);
            arr.push_back(std::move(to));
        }
        if (!arr.empty()) j["events"] = std::move(arr);
    }

    // ---- NLA（R9，2026-10-29）----
    // 同一条纪律：**只在非空时写**（老场景逐字节不变）。
    //
    // 与 `layers` / `events` 的唯一区别：连**播放头**一起写。层的播放头是
    // "这一帧正在发生的事"（进 Play 就重来），而 NLA 的播放头是作者摆好的
    // 位置 —— 停在 3.2 s 存盘，下次打开必须看到同一帧。
    if (!an.nlaTracks.empty()) {
        json tracks = json::array();
        for (const animation::NlaTrack& T : an.nlaTracks) {
            json to;
            to["name"] = T.name;
            if (T.muted) to["muted"] = true;
            if (T.solo) to["solo"] = true;
            json strips = json::array();
            for (const animation::NlaStrip& s : T.strips) {
                json so;
                so["name"] = s.name;
                so["clip"] = s.clip;
                so["start"] = s.start;
                so["end"] = s.end;
                // 默认值全部省掉（"最短可回读"）：一条刚建出来的 strip
                // 只写 name/clip/start/end，读回来逐字段相同。
                if (s.clipStart != 0.0f) so["clipStart"] = s.clipStart;
                if (s.clipEnd != 0.0f) so["clipEnd"] = s.clipEnd;
                if (s.speed != 1.0f) so["speed"] = s.speed;
                if (!s.loop) so["loop"] = false;
                if (s.weight != 1.0f) so["weight"] = s.weight;
                if (s.blendIn != 0.0f) so["blendIn"] = s.blendIn;
                if (s.blendOut != 0.0f) so["blendOut"] = s.blendOut;
                if (s.blend != animation::NlaBlend::Replace)
                    so["blend"] = static_cast<int>(s.blend);
                if (s.muted) so["muted"] = true;
                if (!s.mask.isAll()) {
                    json m;
                    m["mode"] = static_cast<int>(s.mask.mode);
                    m["roots"] = s.mask.roots;
                    so["mask"] = std::move(m);
                }
                strips.push_back(std::move(so));
            }
            to["strips"] = std::move(strips);
            tracks.push_back(std::move(to));
        }
        j["nlaTracks"] = std::move(tracks);
        j["nlaTime"] = an.nlaTime;
        j["nlaSpeed"] = an.nlaSpeed;
        j["nlaPlaying"] = an.nlaPlaying;
        j["nlaLoop"] = an.nlaLoop;
    }
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
    // 根运动骨名（B3）。空 = 自动（第一根 parent == -1 的骨）。
    out.rootMotionBone = j.value("rootMotionBone", std::string());

    // ---- 附加动画层（B1）----
    // 容错优先：单条坏数据（不是对象 / roots 不是数组）只丢**那一条**，
    // 不能连累整块 animator 配置。理由与文件头一致 —— 用户下次打开工程
    // 发现"动画时间被重置"比"少了一个层"严重得多。
    out.layers.clear();
    if (j.contains("layers") && j["layers"].is_array()) {
        for (const json& lo : j["layers"]) {
            if (!lo.is_object()) continue;
            ecs::AnimLayer L;
            L.name = lo.value("name", std::string("Layer"));
            L.clip = lo.value("clip", std::string());
            L.weight = lo.value("weight", 1.0f);
            L.speed = lo.value("speed", 1.0f);
            L.loop = lo.value("loop", true);
            L.playing = lo.value("playing", true);
            L.fadeDur = lo.value("fadeDur", 0.15f);
            if (lo.contains("mask") && lo["mask"].is_object()) {
                const json& m = lo["mask"];
                animation::BoneMaskMode mode = animation::BoneMaskMode::All;
                // 越界 / 非整数 → 保持 All（= 这一层不按骨过滤），
                // 而不是"随便挑一个模式"—— 后者会让角色突然少半边身子。
                animation::boneMaskModeFromInt(m.value("mode", 0), &mode);
                L.mask.mode = mode;
                if (m.contains("roots") && m["roots"].is_array()) {
                    for (const json& r : m["roots"]) {
                        if (r.is_string())
                            L.mask.roots.push_back(r.get<std::string>());
                    }
                }
            }
            // 运行时：重置（与上面 stateIndex 同一理由）
            L.playingClip.clear();
            L.time = 0.0f;
            L.fadeT = 1.0f;
            out.layers.push_back(std::move(L));
        }
    }

    // ---- 动画事件（B2）----
    // 与 layers 同一套容错：坏的一条只丢那一条。事件表本身按时间升序整理
    // 一次 —— 文件可能是手写的 / 用户拖过时间输入框（把 0.8 改成 0.2），
    // 而分发侧假设"表里顺序即时间顺序"只在同刻多事件时有意义，整理一遍
    // 让"同刻事件的先后"变成可复现的。
    out.eventTracks.clear();
    if (j.contains("events") && j["events"].is_array()) {
        for (const json& to : j["events"]) {
            if (!to.is_object()) continue;
            animation::AnimEventTrack tr;
            tr.clip = to.value("clip", std::string());
            if (tr.clip.empty()) continue;   // 没 clip 名 = 挂不上，丢掉
            if (to.contains("events") && to["events"].is_array()) {
                for (const json& eo : to["events"]) {
                    if (!eo.is_object()) continue;
                    animation::AnimEvent e;
                    e.time = eo.value("t", 0.0f);
                    e.kind = eo.value("kind", std::string());
                    e.payload = eo.value("payload", std::string());
                    // ⚠ 负时间 / 越界时间**照收**（不 clamp）：写错的事件应该
                    //   在面板上看得见并从"永远不发"里被发现，而不是读档时
                    //   被悄悄改成 0 —— 那会变成"开场每圈都响一次"。
                    tr.events.push_back(std::move(e));
                }
            }
            if (tr.events.empty()) continue;
            animation::sortAnimEventTrack(tr);
            out.eventTracks.push_back(std::move(tr));
        }
    }

    // ---- NLA（R9，2026-10-29）----
    // 容错纪律与 `layers` 一致：单条坏数据只丢**那一条**，不连累整块
    // animator 配置（"少了半条时间轴"比"动画时间被重置"轻得多）。
    out.nlaTracks.clear();
    if (j.contains("nlaTracks") && j["nlaTracks"].is_array()) {
        for (const json& to : j["nlaTracks"]) {
            if (!to.is_object()) continue;
            animation::NlaTrack T;
            T.name = to.value("name", std::string("Track"));
            T.muted = to.value("muted", false);
            T.solo = to.value("solo", false);
            if (to.contains("strips") && to["strips"].is_array()) {
                for (const json& so : to["strips"]) {
                    if (!so.is_object()) continue;
                    animation::NlaStrip s;
                    s.name = so.value("name", std::string("Strip"));
                    s.clip = so.value("clip", std::string());
                    s.start = so.value("start", 0.0f);
                    s.end = so.value("end", 1.0f);
                    s.clipStart = so.value("clipStart", 0.0f);
                    s.clipEnd = so.value("clipEnd", 0.0f);
                    s.speed = so.value("speed", 1.0f);
                    s.loop = so.value("loop", true);
                    s.weight = so.value("weight", 1.0f);
                    s.blendIn = so.value("blendIn", 0.0f);
                    s.blendOut = so.value("blendOut", 0.0f);
                    s.muted = so.value("muted", false);
                    animation::NlaBlend bl = animation::NlaBlend::Replace;
                    // 越界 / 非整数 → 保持 Replace（= "换成这一段的姿势"），
                    // 而不是"随便挑一个模式"：挑到 Add 的话角色的静止姿势
                    // 会被叠两遍，整具身体变形。
                    animation::nlaBlendFromInt(so.value("blend", 0), &bl);
                    s.blend = bl;
                    if (so.contains("mask") && so["mask"].is_object()) {
                        const json& m = so["mask"];
                        animation::BoneMaskMode mode =
                            animation::BoneMaskMode::All;
                        animation::boneMaskModeFromInt(m.value("mode", 0),
                                                       &mode);
                        s.mask.mode = mode;
                        if (m.contains("roots") && m["roots"].is_array()) {
                            for (const json& r : m["roots"]) {
                                if (r.is_string())
                                    s.mask.roots.push_back(r.get<std::string>());
                            }
                        }
                    }
                    T.strips.push_back(std::move(s));
                }
            }
            out.nlaTracks.push_back(std::move(T));
        }
    }
    // 播放头是**作者数据**（不是"这一局正在发生的事"），所以照读 ——
    // 与上面 eventPrev / rootPrevValid 那两条相反，别顺手一起重置。
    out.nlaTime = j.value("nlaTime", 0.0f);
    out.nlaSpeed = j.value("nlaSpeed", 1.0f);
    out.nlaPlaying = j.value("nlaPlaying", true);
    out.nlaLoop = j.value("nlaLoop", true);

    // 运行时：一律重置（绝不能从文件里读回来，那会让"这一局"的状态
    // 跟着存档走）
    out.stateIndex = -1;
    out.stateTime = 0.0f;
    out.appliedDefaultClip = false;
    // 事件的播放头同理：读档 = 从这一帧重新开始数
    out.eventClip.clear();
    out.eventPrev = 0.0f;
    // 根运动的"上一帧"同理，而且**更要紧**：留着上一局的基准矩阵，
    // 读档后的第一帧会算出 `cur - 上一局的 cur` —— 如果两局之间角色被
    // 搬到别处，那是一个巨大的位移，读档瞬间角色直接飞出去。
    out.rootPrevValid = false;
    out.rootPrevWorld = glm::mat4(1.0f);
    out.rootBasePos = glm::vec3(0.0f);
    out.rootDelta = animation::RootMotionDelta{};
    out.floatParams.clear();
    out.boolParams.clear();
    out.triggerParams.clear();
    return true;
}

// ---------------- 过场时间轴（C1，2026-10-26）----------------
//
// 只存**纯数据**（路径 / 播放控制 / 优先级）。资产内容不落进场景：
// `.seq.json` 是独立资产，两个场景可以引用同一段过场，把内容抄进场景
// 就会出现"改一处只更新了一个场景"的分叉。
//
// ⚠ 运行时字段（asset / playhead / started / finished / loadFailedLogged）
//   一律不写 —— 理由与 animator 那边一样：读档后应该从 `startTime` 重新
//   起播，而不是接着上次的位置（那是存档系统的活，不是场景的）。
json sequencerToJson(const ecs::SequencerComponent& sc) {
    json j;
    j["path"] = sc.sequencePath;
    j["enabled"] = sc.enabled;
    j["playing"] = sc.playing;
    j["loop"] = sc.loop;
    j["startTime"] = sc.startTime;
    j["speed"] = sc.speed;
    j["priority"] = sc.priority;
    return j;
}

bool sequencerFromJson(const json& j, ecs::SequencerComponent& out) {
    if (!j.is_object()) return false;
    out = ecs::SequencerComponent{};
    out.sequencePath = j.value("path", std::string());
    out.enabled = j.value("enabled", true);
    out.playing = j.value("playing", true);
    out.loop = j.value("loop", false);
    out.startTime = j.value("startTime", 0.0f);
    out.speed = j.value("speed", 1.0f);
    out.priority = j.value("priority", 0);
    // 路径空 → 组件等于没配，**不建**（返回 false 让调用方跳过）。
    // 否则场景里会留一堆"配了但什么也不做"的实体，Inspector 上看着像
    // 功能坏了。
    return !out.sequencePath.empty();
}

// ---------------- 对话（C2，2026-10-26）----------------
//
// 与 sequencer 同一条纪律：**只存纯数据**（路径 / 语速 / 起点 / 优先级）。
// 资产内容不落盘 —— `.dlg.json` 是独立资产，两个 NPC 可以引用同一段台词，
// 把内容抄进场景就会出现"改一处只更新了一个场景"。
//
// ⚠ 运行时字段（`player` / `started` / `loadFailedLogged`）一个都不写：
//   `player` 里有播放头、资产缓存与打字机进度，写进去等于把"演到第几句"
//   变成场景的一部分 —— 下次开门那张场景会从中途开始说话。
//   这也是 C1 那条"`playhead` 不落盘"的同一个判断。
json dialogueToJson(const ecs::DialogueComponent& dc) {
    json j;
    j["path"] = dc.dialoguePath;
    j["enabled"] = dc.enabled;
    j["autoStart"] = dc.autoStart;
    j["cps"] = dc.cps;
    // 空串也写：作者可能刻意清空过它（"从资产自己的 start 开始"是显式意图）。
    j["startNode"] = dc.startNode;
    j["priority"] = dc.priority;
    return j;
}

bool dialogueFromJson(const json& j, ecs::DialogueComponent& out) {
    if (!j.is_object()) return false;
    out = ecs::DialogueComponent{};
    out.dialoguePath = j.value("path", std::string());
    out.enabled = j.value("enabled", true);
    out.autoStart = j.value("autoStart", false);
    out.cps = j.value("cps", 30.0f);
    out.startNode = j.value("startNode", std::string());
    out.priority = j.value("priority", 0);
    // 路径空 → 组件等于没配，**不建**（同 sequencerFromJson 的理由）。
    return !out.dialoguePath.empty();
}

// ---------------- 弹簧骨 / LookAt（L2/L4 表演层，2026-10-25）----------------
//
// 两个都是"纯配置"，没有运行时字段要排除 —— 质点位置与上一帧的偏转角
// 存在 `AnimatorSystem` 里，压根不在这两个组件上（见 Components.h 的注释）。
//
// ⚠ 老场景没有这两个键 → 不建组件 → `applyPerformance` 直接 return →
//   与加功能之前逐像素相同。与 `skinned` / `animator` 同一条硬约束。
json springBoneToJson(const ecs::SpringBoneComponent& sb) {
    json j;
    const animation::SpringBoneSettings& s = sb.settings;
    j["enabled"] = s.enabled;
    j["gravity"] = {s.gravity.x, s.gravity.y, s.gravity.z};
    j["wind"] = {s.wind.x, s.wind.y, s.wind.z};
    j["damping"] = s.damping;
    j["restStiffness"] = s.restStiffness;
    j["iterations"] = s.iterations;
    j["maxStep"] = s.maxStep;
    // 空 = 用内置默认前缀。写成 [] 而不是省略：用户可能刻意清空过它。
    j["prefixes"] = sb.prefixes;
    return j;
}

void springBoneFromJson(const json& j, ecs::SpringBoneComponent& out) {
    if (!j.is_object()) return;
    animation::SpringBoneSettings& s = out.settings;
    s.enabled = j.value("enabled", true);
    s.gravity = vec3FromJson(j.contains("gravity") ? j["gravity"] : json(nullptr),
                             s.gravity);
    s.wind = vec3FromJson(j.contains("wind") ? j["wind"] : json(nullptr), s.wind);
    s.damping = j.value("damping", 0.90f);
    s.restStiffness = j.value("restStiffness", 0.15f);
    s.iterations = j.value("iterations", 3);
    s.maxStep = j.value("maxStep", 1.0f / 30.0f);
    out.prefixes.clear();
    if (j.contains("prefixes") && j["prefixes"].is_array()) {
        for (const auto& p : j["prefixes"]) {
            if (p.is_string()) out.prefixes.push_back(p.get<std::string>());
        }
    }
}

json lookAtToJson(const ecs::LookAtComponent& la) {
    json j;
    const animation::LookAtSettings& s = la.settings;
    j["enabled"] = s.enabled;
    j["hasTarget"] = s.hasTarget;
    j["boneName"] = s.boneName;
    j["target"] = {s.target.x, s.target.y, s.target.z};
    j["weight"] = s.weight;
    j["maxAngleDeg"] = s.maxAngleDeg;
    j["smoothRate"] = s.smoothRate;
    j["forwardLocal"] = {s.forwardLocal.x, s.forwardLocal.y, s.forwardLocal.z};
    return j;
}

void lookAtFromJson(const json& j, ecs::LookAtComponent& out) {
    if (!j.is_object()) return;
    animation::LookAtSettings& s = out.settings;
    s.enabled = j.value("enabled", true);
    s.hasTarget = j.value("hasTarget", false);
    s.boneName = j.value("boneName", std::string());
    s.target = vec3FromJson(j.contains("target") ? j["target"] : json(nullptr),
                            s.target);
    s.weight = j.value("weight", 1.0f);
    s.maxAngleDeg = j.value("maxAngleDeg", 55.0f);
    s.smoothRate = j.value("smoothRate", 12.0f);
    s.forwardLocal = vec3FromJson(
        j.contains("forwardLocal") ? j["forwardLocal"] : json(nullptr),
        s.forwardLocal);
}

// ---------------- 两骨 IK / 骨骼挂点（L1 修正层，2026-10-25 第八刀）----------------
//
// 两个都是纯配置：IK 求解器**无状态**（见 `animation/TwoBoneIK.h`），
// 挂点的矩阵每帧从活着的 jointMatrices 重算。所以没有"运行时字段要排除"。
//
// ⚠ `target` 是 **glTF 场景空间**（Y-up），不是引擎的 Z-up 世界空间。
//   存盘时不做任何换算 —— 与 `Pose` / `LookAt::target` 保持同一套坐标，
//   在这里转一次的话"读回来的值和存下去的值不同"，diff 场景时永远有噪音。
//
// ⚠ `poleMode` 存成**字符串**（"animation" / "vector"）而不是整数：
//   枚举加值时整数会静默错位（旧文件里的 1 变成新枚举的另一项），
//   字符串错了只会退回默认。
json ikToJson(const ecs::TwoBoneIKComponent& ik) {
    json j;
    const animation::TwoBoneIKSettings& s = ik.settings;
    j["enabled"] = s.enabled;
    j["hasTarget"] = s.hasTarget;
    j["target"] = {s.target.x, s.target.y, s.target.z};
    j["rootBone"] = s.rootBone;
    j["midBone"] = s.midBone;
    j["endBone"] = s.endBone;
    j["weight"] = s.weight;
    j["poleMode"] = (s.poleMode == animation::TwoBoneIKSettings::PoleFromVector)
                        ? "vector"
                        : "animation";
    j["poleVector"] = {s.poleVector.x, s.poleVector.y, s.poleVector.z};
    j["maxReachScale"] = s.maxReachScale;
    return j;
}

void ikFromJson(const json& j, ecs::TwoBoneIKComponent& out) {
    if (!j.is_object()) return;
    animation::TwoBoneIKSettings& s = out.settings;
    s.enabled = j.value("enabled", true);
    s.hasTarget = j.value("hasTarget", false);
    s.target = vec3FromJson(j.contains("target") ? j["target"] : json(nullptr),
                            s.target);
    s.rootBone = j.value("rootBone", std::string());
    s.midBone = j.value("midBone", std::string());
    s.endBone = j.value("endBone", std::string());
    s.weight = j.value("weight", 1.0f);
    const std::string mode = j.value("poleMode", std::string("animation"));
    s.poleMode = (mode == "vector")
                     ? animation::TwoBoneIKSettings::PoleFromVector
                     : animation::TwoBoneIKSettings::PoleFromAnimation;
    s.poleVector = vec3FromJson(
        j.contains("poleVector") ? j["poleVector"] : json(nullptr),
        s.poleVector);
    s.maxReachScale = j.value("maxReachScale", 0.999f);
}

json attachToJson(const ecs::BoneAttachmentComponent& a) {
    json j;
    j["enabled"] = a.enabled;
    // 宿主按**名字**存（实体 id 是运行时序号，进存档不稳定）
    j["host"] = a.hostName;
    j["bone"] = a.boneName;
    j["offset"] = {a.offset.x, a.offset.y, a.offset.z};
    j["eulerDeg"] = {a.eulerDeg.x, a.eulerDeg.y, a.eulerDeg.z};
    return j;
}

void attachFromJson(const json& j, ecs::BoneAttachmentComponent& out) {
    if (!j.is_object()) return;
    out.enabled = j.value("enabled", true);
    out.hostName = j.value("host", std::string());
    out.boneName = j.value("bone", std::string());
    out.offset = vec3FromJson(j.contains("offset") ? j["offset"] : json(nullptr),
                              out.offset);
    out.eulerDeg = vec3FromJson(
        j.contains("eulerDeg") ? j["eulerDeg"] : json(nullptr), out.eulerDeg);
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

json jointToJson(const ecs::JointComponent& j) {
    // ⚠ `target` 存 **16 位 hex 字符串**，不存下标、也不存 JSON number ——
    //   下标会在"销毁一个实体"之后指到别人身上；number 会被中间工具
    //   （Python json → float、JS → 精度截断）改坏。理由与 `parentGuid`
    //   完全一致，见 Entity.h 顶部那段。
    return json{
        {"kind", static_cast<int>(j.kind)},
        {"target", j.target == ecs::kInvalidGuid ? std::string()
                                                 : ecs::guidToHex(j.target)},
        {"anchorSelf", vec3ToJson(j.anchorSelf)},
        {"anchorTarget", vec3ToJson(j.anchorTarget)},
        {"axis", vec3ToJson(j.axis)},
        {"distance", j.distance},
        {"useLimit", j.useLimit},
        {"lowerLimit", j.lowerLimit},
        {"upperLimit", j.upperLimit},
        {"useMotor", j.useMotor},
        {"motorSpeed", j.motorSpeed},
        {"maxMotorForce", j.maxMotorForce},
        {"enabled", j.enabled},
    };
}

ecs::JointComponent jointFromJson(const json& j) {
    ecs::JointComponent out;   // 缺字段 = 保持默认（与 rigidBodyFromJson 同一条规矩）
    if (!j.is_object()) return out;

    out.kind = static_cast<ecs::JointKind>(
        j.value("kind", static_cast<int>(out.kind)));
    // 空串 / 解析失败都归到"未指定"，让 Play 时走那条明确的 WARN 分支，
    // 而不是拿着一个 0 去 find(0)（那会静默返回无效实体）。
    if (j.contains("target") && j["target"].is_string()) {
        const std::string hex = j["target"].get<std::string>();
        out.target = hex.empty() ? ecs::kInvalidGuid : ecs::parseGuidHex(hex);
    }
    if (j.contains("anchorSelf"))
        out.anchorSelf = vec3FromJson(j["anchorSelf"], out.anchorSelf);
    if (j.contains("anchorTarget"))
        out.anchorTarget = vec3FromJson(j["anchorTarget"], out.anchorTarget);
    if (j.contains("axis")) out.axis = vec3FromJson(j["axis"], out.axis);
    out.distance = j.value("distance", out.distance);
    out.useLimit = j.value("useLimit", out.useLimit);
    out.lowerLimit = j.value("lowerLimit", out.lowerLimit);
    out.upperLimit = j.value("upperLimit", out.upperLimit);
    out.useMotor = j.value("useMotor", out.useMotor);
    out.motorSpeed = j.value("motorSpeed", out.motorSpeed);
    out.maxMotorForce = j.value("maxMotorForce", out.maxMotorForce);
    out.enabled = j.value("enabled", out.enabled);
    return out;
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

// ============================================================
// 单实体 → JSON（场景与 Prefab 共用的**唯一一份**字段实现）
//
// 为什么抽出来：Prefab 的资产格式与场景文件**同构**（平铺 entities + parent
// 下标），如果 Prefab 自己再写一遍"哪些组件、哪些字段"，以后每加一个组件
// 都要记得改两处 —— 而漏改不会报错，只会让 Prefab 默默丢字段（存一次就
// 永久丢）。这里的 `indexOf` 由调用方给：写整场景时是"全场景 id → 下标"，
// 写子树时是"子树成员 id → 子树内下标"，根节点因此自然得到 parent=-1。
//
// 返回 false = 这个实体"没有内容"（既没网格，也没有任何非网格组件），
// 由调用方决定怎么计数（场景算 skipped，Prefab 直接拒收）。
// ============================================================
bool entityToJsonImpl(const ecs::World& w, ecs::Entity e,
                      const ecs::TransformComponent& tr,
                      const std::unordered_map<ecs::EntityId, int>& indexOf,
                      json& out, bool includeEmpty) {
    json je;
    const std::string* nm = w.name(e);
    je["name"] = nm ? *nm : std::string("Entity");

    // ---- 身份紧跟 name（G16-2）----
    // ⚠ 位置是有意的：别插在 `parent` 后面。读文件的人第一眼该看到"这是谁"，
    //   而不是先读三行变换再去找一个 16 位 hex。
    // 无身份（guid==0）就不写 —— 只可能出现在自检手工构造的 World 里。
    const ecs::EntityGuid selfGuid = w.guid(e);
    if (selfGuid != ecs::kInvalidGuid) je["guid"] = ecs::guidToHex(selfGuid);

    je["transform"] = json{
        {"position", vec3ToJson(tr.position)},
        {"rotation", vec3ToJson(tr.rotation)},
        {"scale", vec3ToJson(tr.scale)},
    };

    if (const auto* vis = w.get<ecs::VisibilityComponent>(e)) {
        je["visible"] = vis->visible;
        je["castShadow"] = vis->castShadow;
    }

    // 编辑器锁定（"看得见、动不了"；自带地面就是靠它固定的）
    if (w.has<ecs::LockedComponent>(e)) je["locked"] = true;

    int parentIndex = -1;
    ecs::EntityId parentId = ecs::kInvalidEntity;
    if (const auto* h = w.get<ecs::HierarchyComponent>(e)) {
        parentId = h->parent;
        if (parentId != ecs::kInvalidEntity) {
            auto f = indexOf.find(parentId);
            if (f != indexOf.end()) parentIndex = f->second;
        }
        // "文件夹"标记（Hierarchy 里显示成目录图标的纯分组节点）
        if (h->group) je["folder"] = true;
    }
    je["parent"] = parentIndex;

    // 父**不在本次导出集合里** → 下标表达不了，只能写 guid。
    // 出现的场合：Prefab 子树的根（它的父在场景里）、G19 分块的边界实体。
    // 父已死则 guid 取到 0，什么都不写（留一个 -1 比留一个指不到人的 guid 好）。
    if (parentIndex < 0 && parentId != ecs::kInvalidEntity) {
        const ecs::EntityGuid pg = w.guid(w.handle(parentId));
        if (pg != ecs::kInvalidGuid) je["parentGuid"] = ecs::guidToHex(pg);
    }

    const auto* mc = w.get<ecs::MeshComponent>(e);
    const auto* matc = w.get<ecs::MaterialComponent>(e);
    je["mesh"] = meshSourceToJson(mc ? mc->mesh : nullptr);
    je["material"] = materialToJson(matc ? matc->material : nullptr);
    // 内置图元的生成参数（编辑器可回改，见 ecs::MeshComponent::primitive）
    if (mc) {
        json jp = primitiveParamsToJson(*mc);
        if (!jp.is_null()) je["primitive"] = jp;
    }

    // 局部光源（位置在 transform 里，已经存过了）
    if (const auto* pl = w.get<ecs::PointLightComponent>(e))
        je["pointLight"] = pointLightToJson(*pl);
    if (const auto* sl = w.get<ecs::SpotLightComponent>(e))
        je["spotLight"] = spotLightToJson(*sl);
    // 实体级方向光（补光；场景级太阳走顶层 "light" 字段）
    if (const auto* dl = w.get<ecs::DirectionalLightComponent>(e))
        je["directionalLight"] = directionalLightToJson(*dl);

    // 碰撞体（几何是派生数据，只存来源与摆放，见 collisionToJson）
    if (const auto* cc = w.get<ecs::CollisionComponent>(e))
        je["collision"] = collisionToJson(*cc);

    // 延迟贴花（R4-4）。注意：贴花**没有网格**，所以下面那句
    // "mesh 为空 → skipped++" 对它不适用 —— 只有贴花的实体是合法的。
    if (const auto* dc = w.get<ecs::DecalComponent>(e))
        je["decal"] = decalToJson(*dc);

    // 地形（W4）。它**没有网格组件** —— 几何由高度图在渲染层生成，
    // 所以同样不适用下面那句 "mesh 为空 → skipped++"。
    if (const auto* tc = w.get<ecs::TerrainComponent>(e))
        je["terrain"] = terrainToJson(*tc);

    // 水体（2026-10-21）：全部参数都是场景数据，整份落盘
    if (const auto* wc = w.get<ecs::WaterComponent>(e))
        je["water"] = waterToJson(*wc);

    // 玩家出生点（仿 UE5 PlayerStart）。没有网格：只有一个位置 + 朝向，
    // 视口里的 Q 版小人由编辑器叠加层画，不进场景实体。
    if (const auto* gs = w.get<ecs::GameStartComponent>(e))
        je["gameStart"] = gameStartToJson(*gs);

    // 反射探针（R5-1）。与贴花同理：探针**没有网格**，
    // 下面那句 "mesh 为空 → skipped++" 对它也不适用。
    if (const auto* rp = w.get<ecs::ReflectionProbeComponent>(e))
        je["probe"] = reflectionProbeToJson(*rp);

    // 蒙皮 / 骨骼动画（角色链 A0-2）。模型根实体**没有网格**，
    // 所以它同样不适用下面那句 "mesh 为空 → skipped++"。
    if (const auto* sk = w.get<ecs::SkinnedMeshComponent>(e)) {
        json js = skinnedToJson(*sk);
        if (!js.is_null()) je["skinned"] = js;
        VK_LOG_INFO("SCENE-SKINNED entity=%u clip=%s speed=%.2f "
                    "playing=%d loop=%d stored=%d",
                    static_cast<unsigned>(e.id),
                    sk->clipName.c_str(), static_cast<double>(sk->speed),
                    sk->playing ? 1 : 0, sk->loop ? 1 : 0,
                    js.is_null() ? 0 : 1);
    }

    // 表情滑条（§15）。与 skinned 同一个实体（模型根），紧跟其后。
    if (const auto* mp = w.get<ecs::MorphComponent>(e)) {
        json jm = morphToJson(*mp);
        if (!jm.is_null()) je["morph"] = std::move(jm);
        VK_LOG_INFO("SCENE-MORPH entity=%u stored=%d channels=%d",
                    static_cast<unsigned>(e.id),
                    jm.is_null() ? 0 : 1,
                    mp->model
                        ? static_cast<int>(mp->model->morphRig.channelCount())
                        : 0);
    }

    // GPU 粒子发射器（R6-1）。与贴花 / 探针同理：发射器**没有网格**
    //（几何是顶点着色器现拼的 billboard），同样不适用 skipped++ 那句。
    if (const auto* fx = w.get<ecs::ParticleEmitterComponent>(e))
        je["particleEmitter"] = particleEmitterToJson(*fx);

    // 刚体（R7-1）。与贴花 / 探针 / 发射器同理：它也不带网格 ——
    // "隐形的墙"就是一个只有刚体的实体，所以下面那句 skipped++ 同样
    // 要为它开一个口子。位置/朝向已经在 transform 里，这里只存参数。
    if (const auto* rb = w.get<ecs::RigidBodyComponent>(e))
        je["rigidbody"] = rigidBodyToJson(*rb);

    // 关节（J5）。这条实体是关节的 **A 侧**，另一端由 `target` 的 guid 指。
    // 与刚体同款：它也不带网格（通常正好挂在"只有刚体 + 关节"的实体上，
    // 比如门轴），所以同样要为 skipped++ 开一个口子。
    if (const auto* jt = w.get<ecs::JointComponent>(e))
        je["joint"] = jointToJson(*jt);

    // 动画器（角色链 A1）。挂在同一个模型根实体上，紧跟 skinned 之后。
    // 存盘顺序 = 读档顺序，所以 animatorFromJson 里的 modelFromSource
    // 一定能命中 AssetManager 缓存（键 = modelKey）。
    if (const auto* an = w.get<ecs::AnimatorComponent>(e)) {
        je["animator"] = animatorToJson(*an);
        VK_LOG_INFO("SCENE-ANIMATOR entity=%u clip=%s time=%.3f "
                    "speed=%.2f playing=%d loop=%d ctrl=%d",
                    static_cast<unsigned>(e.id),
                    an->defaultClip.c_str(),
                    static_cast<double>(an->time),
                    static_cast<double>(an->speed), an->playing ? 1 : 0,
                    an->loop ? 1 : 0,
                    an->controllerPath.empty() ? 0 : 1);
    }

    // 表演层（L2/L4，2026-10-25）。紧跟 animator：三者是同一个角色的
    // 三层（播什么 / 头看哪 / 头发怎么飘），读档顺序上也没有依赖。
    if (const auto* sb = w.get<ecs::SpringBoneComponent>(e))
        je["springBone"] = springBoneToJson(*sb);
    if (const auto* la = w.get<ecs::LookAtComponent>(e))
        je["lookAt"] = lookAtToJson(*la);
    if (const auto* ik = w.get<ecs::TwoBoneIKComponent>(e))
        je["ik"] = ikToJson(*ik);
    if (const auto* at = w.get<ecs::BoneAttachmentComponent>(e))
        je["attach"] = attachToJson(*at);

    // 过场时间轴（C1，2026-10-26）。挂在任意实体上；同一场景可以挂多段，
    // 由 priority 决定这一帧谁在跑。空路径不写（与 animClip 同一条理由：
    // 别给每个实体都添一个空块，场景 diff 会全是噪音）。
    if (const auto* sq = w.get<ecs::SequencerComponent>(e)) {
        if (!sq->sequencePath.empty()) {
            je["sequencer"] = sequencerToJson(*sq);
            VK_LOG_INFO("SCENE-SEQUENCER entity=%u path=%s dur=%.3f "
                        "keys=%d events=%d pri=%d",
                        static_cast<unsigned>(e.id),
                        sq->sequencePath.c_str(),
                        static_cast<double>(sq->asset.duration),
                        static_cast<int>(sq->asset.cameraKeys.size()),
                        static_cast<int>(sq->asset.events.size()), sq->priority);
        }
    }

    // 对话（C2）。空路径不写（同 animClip 的理由：别给每个实体都添一个
    // 空块，场景 diff 会全是噪音）。
    if (const auto* dlg = w.get<ecs::DialogueComponent>(e)) {
        if (!dlg->dialoguePath.empty()) {
            je["dialogue"] = dialogueToJson(*dlg);
            VK_LOG_INFO("SCENE-DIALOGUE entity=%u path=%s auto=%d cps=%.1f "
                        "pri=%d",
                        static_cast<unsigned>(e.id),
                        dlg->dialoguePath.c_str(), dlg->autoStart ? 1 : 0,
                        static_cast<double>(dlg->cps), dlg->priority);
        }
    }

    // 实体 transform 关键帧轨道（编辑器 Animation 模式的产物）。
    // 空轨道**不写** —— 否则每个用过的实体都会多一个 {"keys":[]}，
    // 场景文件的 diff 全是噪音。
    if (const auto* ac = w.get<ecs::AnimationClipComponent>(e)) {
        if (!ac->keys.empty()) je["animClip"] = animClipToJson(*ac);
    }

    // 角色控制器（G07）。与刚体**互斥**：角色是运动学的，由
    // stepCharacter 驱动，不交给物理求解器（互斥关系在 Inspector 里
    // 会给出提示，见 InspectorPanel 的 "角色控制器" 段）。
    if (const auto* chc =
            w.get<ecs::CharacterControllerComponent>(e))
        je["character"] = characterToJson(*chc);

    // 触发体（G08）/ 物理材质（G08）/ 相机（G09）
    if (const auto* trg = w.get<ecs::TriggerComponent>(e))
        je["trigger"] = triggerToJson(*trg);
    if (const auto* pmat =
            w.get<ecs::PhysicsMaterialComponent>(e))
        je["physicsMaterial"] = physicsMaterialToJson(*pmat);
    if (const auto* cam = w.get<ecs::CameraComponent>(e))
        je["camera"] = cameraComponentToJson(*cam);

    // 动画键盘映射（P4）。空表不写（理由见 animKeyMapToJson）。
    if (const auto* akm = w.get<ecs::AnimKeyMapComponent>(e)) {
        if (!akm->entries.empty()) je["animKeys"] = animKeyMapToJson(*akm);
    }

    // 脚本（只有路径与开关，脚本源码本身是独立文件）
    if (const auto* sc = w.get<ecs::ScriptComponent>(e)) {
        je["script"] = json{
            {"path", sc->path},
            {"enabled", sc->enabled},
            {"timeScale", sc->timeScale},
        };
    }

    // 音频源（G14-7）。字段少 → **全写**（读端每个字段都有默认，
    // 所以旧场景不受影响；而"只写非默认值"会让"我把 volume 调回 1.0
    // 之后存档里为什么没这个键"变成一个要解释的问题）。
    if (const auto* asc = w.get<ecs::AudioSourceComponent>(e)) {
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
    if (const auto* al = w.get<ecs::AudioListenerComponent>(e))
        je["audioListener"] = al->enabled;

    // 玩家（walkPhase/bodyYaw/started 是运行态派生值，不存）
    if (const auto* pc = w.get<ecs::PlayerComponent>(e)) {
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
    if (const auto* limb = w.get<ecs::PlayerLimbComponent>(e))
        je["playerLimb"] = limb->kind;

    // 预制体成员标记（G18）。写在这一块的理由与别的组件一样：它是"内容"，
    // 不是诊断 —— 少了它，存一次盘实例就"脱钩"（改母版不再跟着变）。
    if (const auto* pl = w.get<ecs::PrefabLinkComponent>(e)) {
        json jp = json{
            {"path", pl->prefabPath},
            {"localId", pl->localId},
            {"instance", pl->instance},
        };
        if (pl->root) jp["root"] = true;
        // 覆盖表只在根上有意义，为空时**不写** —— 让文件里"没有覆盖"和
        // "有 0 条覆盖"是同一件事（少一个要解释的状态）。
        if (!pl->overrides.empty()) jp["overrides"] = pl->overrides;
        je["prefab"] = std::move(jp);
    }

    // "有没有内容"：没有网格时，贴花 / 探针 / 发射器 / 刚体 / 角色 /
    // 触发体 / 物理材质 / 相机 / **蒙皮** 任意一个存在就不算空 —— 它们
    // 都是"没有网格但有意义"的实体（隐形的墙、触发区域、相机机位、
    // 模型根节点）。
    //
    // ⚠ 这份名单必须与**读端**（entityFromJsonImpl 末尾）一字不差 ——
    //   2026-10-27 之前它是 loadSceneFromJson 的一整段，抽成单实体函数后
    //   两份仍在，改一处必须改两处。
    //   两处不一致的后果不是崩溃，而是 `skipped` 计数骗人：
    //   2026-11 实测，第三人称模板场景（2 个实体：地面 + 出生点）报
    //   `entities=2 skipped=1` —— 出生点被当成"空实体"，可它其实
    //   好好加载了。而回归脚本正是拿 `skipped=0` 断言"没丢东西"的，
    //   假红比不报更费时间。
    const bool hasNonMeshContent =
        je.contains("terrain") || je.contains("water") ||
        je.contains("decal") || je.contains("probe") ||
        je.contains("particleEmitter") || je.contains("rigidbody") ||
        je.contains("joint") ||
        je.contains("character") || je.contains("trigger") ||
        je.contains("physicsMaterial") || je.contains("camera") ||
        je.contains("skinned") || je.contains("animator") ||
        je.contains("morph") ||
        // 表演层三件套 + IK + 挂点。**必须在这里**：一个只挂 IK/挂点的
        // 实体可以没有网格（挂点是"钉在别人骨头上的空物体"就是典型），
        // 漏了它会被算进 skipped，而回归拿 skipped=0 断"没丢东西"。
        // springBone / lookAt 原来也漏在这儿（同一类 bug），一起补。
        je.contains("springBone") || je.contains("lookAt") ||
        je.contains("ik") || je.contains("attach") ||
        je.contains("animClip") || je.contains("gameStart") ||
        je.contains("animKeys") ||
        // C1 过场时间轴。挂它的实体**通常没有网格**（就是一根"导演轨"），
        // 漏在这里会被算进 skipped，而回归拿 skipped=0 断"没丢东西"。
        // ⚠ 写端与读端各有一份同样名单，改一处必须改两处。
        je.contains("sequencer") ||
        // C2 对话。同样通常挂在没有网格的"导演轨"实体上 —— 漏了会被
        // 算进 skipped，而回归拿 skipped=0 断"没丢东西"。
        je.contains("dialogue") ||
        je.contains("pointLight") || je.contains("spotLight") ||
        je.contains("directionalLight") || je.contains("player") ||
        je.contains("playerLimb") || je.contains("folder") ||
        // G18 预制体成员标记。prefab 里可以有"纯分组节点"（没有网格、
        // 只有 children），漏了它会被算进 skipped。
        je.contains("prefab") ||
        // 内置图元的生成参数（`MeshComponent::primitive`，2026-10-29）。
        // 加它是因为"只写 primitive、没有 mesh"的实体确实存在（历史手写夹具
        // 就是这种形状），而**丢弃它**等于把读进来的数据在存盘时抹掉 ——
        // 存盘应该保真，不该顺手做语义裁剪。
        // ⚠ 但别指望它能把这种实体"救活"：读端 `entityApplyComponentsImpl`
        //   只在 `resolveMesh()` 拿到网格时才建 MeshComponent，`primitive`
        //   是**挂在已有网格上的意图**。所以 primitive-only 的实体在运行时
        //   仍然是空物体（`runtime_test/mat_conflict_probe.json` 当年就是这么
        //   空了两年，`verify_material_instances` 的 B1 一直拿 A=None）。
        //   那份夹具已经补上 `"mesh": {"kind":"builtin","shape":"cube"}`。
        // ⚠ 写端与读端各有一份同样名单，改一处必须改两处。
        je.contains("primitive");
    // ⚠ `includeEmpty` 只给**存档**用（G17-1）。场景文件要的是"不写空壳"，
    //   存档要的是"全量" —— 一个只挂 Transform 的空物体（玩家、出生点、
    //   纯分组节点）在场景里确实没什么好存的，但存档漏了它 = 读档后
    //   那个物体"消失了"（或者被补发一个新 guid）。
    if (!includeEmpty && je["mesh"].is_null() && !hasNonMeshContent)
        return false;

    out = std::move(je);
    return true;
}


// ============================================================
// JSON → 单实体（与 `entityToJsonImpl` 成对，场景与 Prefab 共用）
//
// 调用前提（顺序是硬依赖，Prefab 实例化必须照做）：
//   1. 实体已经由 `Scene::createObject` 建好，并且 transform / visible /
//      locked 已经写过（那三样在"第一遍"里，父子关系要先于它们存在吗？
//      不需要 —— 但 `setParent` 必须在 `mesh` 之前：蒙皮的 model 解析会
//      沿着父链找骨架）。
//   2. `created` 是"文件下标 → 实体"的映射，父引用（`parent`）靠它解析。
//      写子树时传"子树内的下标表"，跨集合的父引用**不支持**（Roadmap D3）。
//   3. 组件块顺序不能换：mesh → material → skinned → morph → animator →
//      表演层。`modelFromSource` 命中 AssetManager 缓存靠的就是这个顺序。
//
// 返回 true = 这个实体"有内容"（有网格/材质，或挂着任一非网格组件）。
// ============================================================

// ---- G16-5 两段拆分 ----
// 上面那份注释里的"三件事"其实只有两种依赖：父子/文件夹是**纯 CPU**，
// 组件与资产要 AssetManager。劈开之后：
//   * `entityApplyHierarchyImpl` → ② 段（无设备），
//   * `entityApplyComponentsImpl` → ③ 段（要设备）。
// 公开壳在外层（`entityApplyHierarchy` / `entityApplyComponents`），
// `entityFromJsonImpl` 只是把两段串起来 —— 这样 Prefab 与场景加载
// **仍然只有一条路**，不会出现"拆完之后 Prefab 漏了父子"这种事故。
void entityApplyHierarchyImpl(Scene& scene, const json& je, ecs::Entity e,
                              const std::vector<ecs::Entity>& created) {
    ecs::World& w = scene.world();
    // ---- 父子关系：跨集合用 `parentGuid`，本文件内用 `parent` 下标 ----
    // 两条路互斥：写了 `parentGuid` 就以它为准（Prefab 子树的根、G19 分块的边界
    // 实体都只有 guid 可用，它们的父不在这份文件里，下标无从谈起）。
    if (je.contains("parentGuid") && je["parentGuid"].is_string()) {
        const ecs::EntityGuid pg = ecs::parseGuidHex(je["parentGuid"].get<std::string>());
        const ecs::Entity p = w.find(pg);
        if (p.valid()) {
            scene.setParent(e, p);
        } else {
            // 不 fail 整份加载：父可能在还没流送进来的块里（G19），先让实体独立存在。
            VK_LOG_WARN("SCENE-PARENT-GUID-MISS guid=%s entity=%u",
                        je["parentGuid"].get<std::string>().c_str(), e.id);
        }
    } else {
        // ⚠ 必须显式判 `is_number_integer()`：`je.value("parent", -1)` 遇到字符串会抛
        //   `type_error.302`，而手写 / 别的工具导出的文件里 `"parent": "-1"` 完全可能。
        const int parentIndex =
            (je.contains("parent") && je["parent"].is_number_integer()) ? je["parent"].get<int>()
                                                                        : -1;
        if (parentIndex >= 0 && parentIndex < static_cast<int>(created.size()) &&
            created[static_cast<size_t>(parentIndex)].valid()) {
            scene.setParent(e, created[static_cast<size_t>(parentIndex)]);
        }
    }

    // "文件夹"标记
    // ⚠ 它读的是 `folder` 布尔，写端在 `entityToJsonImpl` 里 —— 两处别漏一处：
    //   漏了读端，存一次盘所有文件夹就摊平成普通空物体（在选择树里完全看不出来）。
    if (je.value("folder", false)) {
        if (auto* h = w.get<ecs::HierarchyComponent>(e)) h->group = true;
    }
}

// ③ 段：组件值与资产。**这里才开始碰 AssetManager**。
bool entityApplyComponentsImpl(Scene& scene, assets::AssetManager& assets,
                               const json& je, ecs::Entity e) {
    ecs::World& w = scene.world();
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

    // ---- 表情滑条（§15）----
    // 在 skinned 之后：`modelFromSource` 命中的是同一个 AssetManager
    // 缓存项（键 = modelKey），所以 morphRig 已经建好了。
    // ⚠ 老场景没有这个键 → 不建组件 → 逐像素不变（滑条全 0 时
    //   GPU 形变分支本来就不执行，见 pbr.vert 的 `mi.z > 0`）。
    if (je.contains("morph")) {
        ecs::MorphComponent mp;
        if (morphFromJson(assets, je["morph"], mp)) {
            if (auto* p = w.get<ecs::MorphComponent>(e))
                *p = mp;
            else
                w.add<ecs::MorphComponent>(e, mp);
            VK_LOG_INFO("SCENE-MORPH entity=%u channels=%d ok=1",
                        static_cast<unsigned>(e.id),
                        static_cast<int>(mp.weights.size()));
        } else {
            VK_LOG_INFO("SCENE-MORPH entity=%u ok=0",
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

    // 过场时间轴（C1）。与 springBone 那几块无关，位置随意；
    // 资产是**懒加载**的（第一次 tick 才读盘），所以这里只存路径。
    if (je.contains("sequencer")) {
        ecs::SequencerComponent sq;
        if (sequencerFromJson(je["sequencer"], sq)) {
            if (auto* p = w.get<ecs::SequencerComponent>(e))
                *p = sq;
            else
                w.add<ecs::SequencerComponent>(e, sq);
            VK_LOG_INFO("SCENE-SEQUENCER entity=%u path=%s pri=%d ok=1",
                        static_cast<unsigned>(e.id), sq.sequencePath.c_str(),
                        sq.priority);
        } else {
            VK_LOG_INFO("SCENE-SEQUENCER entity=%u ok=0", static_cast<unsigned>(e.id));
        }
    }

    // 对话（C2）。与 sequencer 一样是懒加载（第一次 tick 才读盘），
    // 所以这里只存路径与配置。
    if (je.contains("dialogue")) {
        ecs::DialogueComponent dlg;
        if (dialogueFromJson(je["dialogue"], dlg)) {
            if (auto* p = w.get<ecs::DialogueComponent>(e))
                *p = dlg;
            else
                w.add<ecs::DialogueComponent>(e, dlg);
            VK_LOG_INFO("SCENE-DIALOGUE entity=%u path=%s auto=%d cps=%.1f "
                        "start=[%s] pri=%d ok=1",
                        static_cast<unsigned>(e.id),
                        dlg.dialoguePath.c_str(), dlg.autoStart ? 1 : 0,
                        static_cast<double>(dlg.cps), dlg.startNode.c_str(),
                        dlg.priority);
        } else {
            VK_LOG_INFO("SCENE-DIALOGUE entity=%u ok=0", static_cast<unsigned>(e.id));
        }
    }

    // 表演层（L2/L4）。必须在 skinned 之后 —— 两者的算法都要骨架，
    // 而骨架来自 SkinnedMeshComponent::model。
    if (je.contains("springBone")) {
        ecs::SpringBoneComponent sb;
        springBoneFromJson(je["springBone"], sb);
        if (auto* p = w.get<ecs::SpringBoneComponent>(e))
            *p = sb;
        else
            w.add<ecs::SpringBoneComponent>(e, sb);
        VK_LOG_INFO("SCENE-SPRINGBONE entity=%u enabled=%d prefixes=%d",
                    static_cast<unsigned>(e.id), sb.settings.enabled ? 1 : 0,
                    static_cast<int>(sb.prefixes.size()));
    }
    if (je.contains("lookAt")) {
        ecs::LookAtComponent la;
        lookAtFromJson(je["lookAt"], la);
        if (auto* p = w.get<ecs::LookAtComponent>(e))
            *p = la;
        else
            w.add<ecs::LookAtComponent>(e, la);
        VK_LOG_INFO("SCENE-LOOKAT entity=%u enabled=%d target=%d weight=%.2f",
                    static_cast<unsigned>(e.id), la.settings.enabled ? 1 : 0,
                    la.settings.hasTarget ? 1 : 0,
                    static_cast<double>(la.settings.weight));
    }
    // IK：必须在 skinned 之后（要骨架才能解析链）；
    // 挂点则只要宿主**存在**即可，但它引用的 jointMatrices 要等运行时
    // 第一次 tick 才有 —— 读档时链没解析出来是正常的，不是错误。
    if (je.contains("ik")) {
        ecs::TwoBoneIKComponent ik;
        ikFromJson(je["ik"], ik);
        if (auto* p = w.get<ecs::TwoBoneIKComponent>(e))
            *p = ik;
        else
            w.add<ecs::TwoBoneIKComponent>(e, ik);
        VK_LOG_INFO("SCENE-IK entity=%u enabled=%d target=%d weight=%.2f "
                    "root=[%s]",
                    static_cast<unsigned>(e.id), ik.settings.enabled ? 1 : 0,
                    ik.settings.hasTarget ? 1 : 0,
                    static_cast<double>(ik.settings.weight),
                    ik.settings.rootBone.c_str());
    }
    if (je.contains("attach")) {
        ecs::BoneAttachmentComponent at;
        attachFromJson(je["attach"], at);
        if (auto* p = w.get<ecs::BoneAttachmentComponent>(e))
            *p = at;
        else
            w.add<ecs::BoneAttachmentComponent>(e, at);
        VK_LOG_INFO("SCENE-ATTACH entity=%u host=[%s] bone=[%s]",
                    static_cast<unsigned>(e.id), at.hostName.c_str(),
                    at.boneName.c_str());
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

    // 关节（J5）。同样是**纯数据**：Play 建物理世界时由 ScenePhysics 翻译成
    // physics::Joint。`target` 里是 guid，不需要在这里解析成实体 ——
    // 解析放在建世界那一步，这样读盘顺序与 guid 分配顺序无关。
    if (je.contains("joint") && je["joint"].is_object()) {
        const ecs::JointComponent jc = jointFromJson(je["joint"]);
        if (auto* p = w.get<ecs::JointComponent>(e))
            *p = jc;
        else
            w.add<ecs::JointComponent>(e, jc);
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

    // 预制体成员标记（G18）。**必须读回来**：少了它，存一次盘实例就脱钩
    // （改母版不再跟着变），而画面上完全看不出来 —— 这是本功能最容易
    // "看起来做完了其实没做"的地方。
    if (je.contains("prefab") && je["prefab"].is_object()) {
        const json& jp = je["prefab"];
        ecs::PrefabLinkComponent pl;
        pl.prefabPath = jp.value("path", std::string());
        pl.localId = jp.value("localId", std::string());
        pl.instance = jp.value("instance", 0u);
        pl.root = jp.value("root", false);
        if (jp.contains("overrides") && jp["overrides"].is_array()) {
            for (const auto& o : jp["overrides"])
                if (o.is_string()) pl.overrides.push_back(o.get<std::string>());
        }
        w.add<ecs::PrefabLinkComponent>(e, pl);
    }

    // ⚠ 与写端（saveSceneFromJson 里那份）必须一字不差，理由见那里。
    const bool hasNonMeshContent =
        je.contains("terrain") || je.contains("water") ||
        je.contains("decal") || je.contains("probe") ||
        je.contains("particleEmitter") || je.contains("rigidbody") ||
        je.contains("joint") ||
        je.contains("character") || je.contains("trigger") ||
        je.contains("physicsMaterial") || je.contains("camera") ||
        je.contains("skinned") || je.contains("animator") ||
        je.contains("morph") ||
        // 表演层三件套 + IK + 挂点。**必须在这里**：一个只挂 IK/挂点的
        // 实体可以没有网格（挂点是"钉在别人骨头上的空物体"就是典型），
        // 漏了它会被算进 skipped，而回归拿 skipped=0 断"没丢东西"。
        // springBone / lookAt 原来也漏在这儿（同一类 bug），一起补。
        je.contains("springBone") || je.contains("lookAt") ||
        je.contains("ik") || je.contains("attach") ||
        je.contains("animClip") || je.contains("gameStart") ||
        je.contains("animKeys") ||
        // C1 过场时间轴。挂它的实体**通常没有网格**（就是一根"导演轨"），
        // 漏在这里会被算进 skipped，而回归拿 skipped=0 断"没丢东西"。
        // ⚠ 写端与读端各有一份同样名单，改一处必须改两处。
        je.contains("sequencer") ||
        // C2 对话。同样通常挂在没有网格的"导演轨"实体上 —— 漏了会被
        // 算进 skipped，而回归拿 skipped=0 断"没丢东西"。
        je.contains("dialogue") ||
        je.contains("pointLight") || je.contains("spotLight") ||
        je.contains("directionalLight") || je.contains("player") ||
        je.contains("playerLimb") || je.contains("folder") ||
        // G18 预制体成员标记。prefab 里可以有"纯分组节点"（没有网格、
        // 只有 children），漏了它会被算进 skipped。
        je.contains("prefab") ||
        // 内置图元（`MeshComponent::primitive`）。与写端同一份名单、同一个
        // 理由：程序化箱体没有 mesh，只有 primitive。漏了它 → 读端把这种
        // 实体判成"空壳"丢掉（或者只留 Transform），表现成"存了读不回来"。
        je.contains("primitive");
    return mesh || mat || hasNonMeshContent;
}

// 两段的**唯一**串联点。保留这个函数体只有两行，是为了让"拆完之后
// Prefab 与场景加载仍然是同一条路"这件事在代码上无法被悄悄改坏。
bool entityFromJsonImpl(Scene& scene, assets::AssetManager& assets,
                        const json& je, ecs::Entity e,
                        const std::vector<ecs::Entity>& created) {
    entityApplyHierarchy(scene, je, e, created);
    return entityApplyComponents(scene, assets, je, e);
}

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
        json jent;
        if (entityToJsonImpl(w, it.entity, *it.transform, indexOf, jent, /*includeEmpty=*/false))
            root["entities"].push_back(std::move(jent));
        else
            ++r.skipped;
    }

    r.entities = static_cast<int>(items.size());
    return root;
}

// ============================================================
// JSON → 场景（文件版与内存版共用的核心）
// ============================================================

// `path` 只为日志（`SCENE-GUID ... path=`）而传：内存版 `sceneFromJson` 给默认值，
// 读盘版给真实路径。它**不**参与解析，所以别拿它做任何路径解析。
//
// ---- G16-5：这里现在是三段的**组合**，不再是三件事的混合体 ----
// 想"后台解析 / 主线程上传"的话，把 ① ② 搬到工作线程、只把 ③ 留在这里即可。
SceneIoResult loadSceneFromJson(Scene& scene, assets::AssetManager& assets, const json& root,
                                const std::string& path = "<memory>") {
    SceneIoResult r;

    // ① 解析（版本校验在里，早于任何 `clear`）
    SceneDocument doc;
    std::string err;
    if (!parseSceneDocument(root, doc, err)) {
        r.error = err;
        return r;
    }

    // ② 建骨架（纯 CPU）
    std::vector<ecs::Entity> created;
    std::unordered_map<ecs::EntityGuid, ecs::Entity> guidMap;
    DocumentStats st;
    instantiateDocument(scene, doc, created, guidMap, st);

    // 一条汇总，不是每实体一条 —— 42 个实体刷 42 行会把真正的问题淹没。
    VK_LOG_INFO("SCENE-GUID assigned=%d dup=%d total=%d path=%s", st.guidAssigned, st.guidDup,
                st.entities, path.c_str());

    // ③ 资产（唯一要设备的一句）
    resolveDocumentAssets(scene, assets, doc, created, st);

    r.ok = true;
    r.entities = st.entities;
    r.skipped = st.skipped;
    return r;
}

} // namespace

// ============================================================
// 公开：G16-5 三段（见 scene/SceneDocument.h）
//
// ⚠ 定义必须在匿名 namespace **外面**：里面定义就是内部链接，而头文件
//   声明的是外部链接 —— 别的 TU 会拿到"未定义引用"，报错点在链接期。
//   但它们要用的 `applyCamera` / `physicsFromJson` / `entityApplyComponentsImpl`
//   全在匿名 namespace 里，所以实现只能留在本文件（这就是为什么没有
//   `SceneDocument.cpp`）。
// ============================================================

// ① 解析 + 校验。纯 CPU。
bool parseSceneDocument(const nlohmann::json& root, SceneDocument& out, std::string& err) {
    out = SceneDocument{};

    if (!root.is_object()) {
        err = "root JSON value is not an object";
        return false;
    }

    // ---- 格式版本（G16-2）----
    // v1 → v2：实体多了 `guid` / `parentGuid`。**旧文件照读**（缺 guid 就补发），
    // 所以这里只拒绝"比我们新"的版本。
    if (root.contains("version") && root["version"].is_number_integer()) {
        const int v = root["version"].get<int>();
        out.formatVersion = v;
        if (v > kSceneFormatVersion) {
            err = "scene format version " + std::to_string(v) + " is newer than supported (" +
                  std::to_string(kSceneFormatVersion) + ")";
            VK_LOG_ERROR("SCENE-VERSION too-new found=%d supported=%d", v, kSceneFormatVersion);
            return false;
        }
    }

    // ---- 全局块：原样留着，由 ② 段按 key 应用 ----
    // 白名单是刻意的（列出"认识的"，而不是"排除 entities"）：漏掉一个 key =
    // 读档后那个设置静默回到默认值，而画面上未必看得出来；反过来用排除法，
    // 将来的未知块也会被搬进来，`documentToJson` 写回时就变成垃圾。
    static const char* kGlobalKeys[] = {"camera", "cameras",     "light",
                                        "physics", "scriptVars", "postVolumes"};
    for (const char* k : kGlobalKeys) {
        if (root.contains(k)) out.globals[k] = root[k];
    }

    if (!root.contains("entities") || !root["entities"].is_array()) {
        // 只有相机 / 光照的空场景也算成功（老文件与"新建场景"都走这条）。
        out.hasEntities = false;
        return true;
    }
    out.hasEntities = true;

    const nlohmann::json& ents = root["entities"];
    out.entities.reserve(ents.size());
    for (const nlohmann::json& je : ents) {
        DocumentEntity de;
        if (!je.is_object()) {
            // ⚠ 保留占位：`parent` 下标是**文件下标**，压缩一下后面全体错位。
            //   用"raw 不是 object"当无效标记，② 段据此跳过。
            de.name.clear();
            de.raw = nlohmann::json(nullptr);
            out.entities.push_back(std::move(de));
            continue;
        }
        de.name = je.value("name", std::string("Entity"));
        // 空串 / 非法 / 全零 → `kInvalidGuid`（② 段会保留 create() 随机补发的那个）。
        if (je.contains("guid") && je["guid"].is_string())
            de.guid = ecs::parseGuidHex(je["guid"].get<std::string>());
        if (je.contains("parentGuid") && je["parentGuid"].is_string())
            de.parentGuid = ecs::parseGuidHex(je["parentGuid"].get<std::string>());
        de.parentIndex = (je.contains("parent") && je["parent"].is_number_integer())
                             ? je["parent"].get<int>()
                             : -1;
        de.raw = je;
        out.entities.push_back(std::move(de));
    }
    return true;
}

// ② 建世界骨架。纯 CPU —— 参数表里没有 AssetManager，这是判据不是巧合。
void instantiateDocument(Scene& scene, const SceneDocument& doc,
                         std::vector<ecs::Entity>& created,
                         std::unordered_map<ecs::EntityGuid, ecs::Entity>& guidMap,
                         DocumentStats& stats) {
    created.clear();
    guidMap.clear();

    // 清空旧场景（AssetManager 的网格/材质/纹理缓存保留并可复用）
    scene.clear();

    const nlohmann::json& g = doc.globals;
    auto has = [&g](const char* k) { return g.contains(k); };

    applyCamera(scene.camera(), has("camera") ? g["camera"] : nlohmann::json(nullptr));
    // 额外相机（多相机）。顶层 "camera" 已经钉好 0 号，这里从 1 号起补。
    if (has("cameras") && g["cameras"].is_array()) {
        const nlohmann::json& cams = g["cameras"];
        for (size_t i = 0; i < cams.size(); ++i) {
            Camera& cam = (scene.cameraCount() > i + 1) ? scene.camera(i + 1)
                                                        : scene.camera(scene.addCamera());
            applyCamera(cam, cams[i]);
        }
    }
    applyLight(scene.light(), has("light") ? g["light"] : nlohmann::json(nullptr));

    // 物理世界（R7-1）。scene.clear() 已经把 m_physics 复位成默认值，
    // 所以"老场景没有 physics 字段"这条路径**什么都不用做**就是对的。
    physicsFromJson(scene.physicsSettings(),
                    has("physics") ? g["physics"] : nlohmann::json(nullptr));

    // 脚本世界黑板（S2）+ 后处理体积
    scene.scriptVars().clear();
    if (has("scriptVars") && g["scriptVars"].is_object()) {
        for (auto it = g["scriptVars"].begin(); it != g["scriptVars"].end(); ++it) {
            if (it.value().is_number()) scene.scriptVars()[it.key()] = it.value().get<double>();
        }
    }
    if (has("postVolumes") && g["postVolumes"].is_array()) {
        for (const nlohmann::json& jv : g["postVolumes"])
            scene.postVolumes().add(postVolumeFromJson(jv));
    }

    created.reserve(doc.entities.size());

    // ---- 第一遍：建实体 + 名字 + guid + 变换 + 可见性 ----
    // ⚠ guid 必须在这里（早于任何其它字段）落定：后面的 `parentGuid` 解析
    //   要按 guid 反查实体，而反查表是在这一步建起来的。
    ecs::World& w = scene.world();
    for (const DocumentEntity& de : doc.entities) {
        if (!de.raw.is_object()) {
            created.push_back(ecs::Entity{});
            continue;
        }
        ecs::Entity e = scene.createObject(de.name);

        // `createObject` 已经给了随机 guid，这里用文件里的值**覆盖**。
        // 空串 / 非法 / 全零 → 都算"没有身份"，保留随机值（老场景与手写用例走这条）。
        if (de.guid == ecs::kInvalidGuid) {
            ++stats.guidAssigned;
        } else if (!w.setGuid(e, de.guid)) {
            ++stats.guidDup;
            VK_LOG_WARN("SCENE-GUID-DUP guid=%s action=reassign entity=%u",
                        ecs::guidToHex(de.guid).c_str(), e.id);
        }
        guidMap[w.guid(e)] = e;

        // 外壳（transform / visible / locked）与 Prefab 实例化共用一份实现
        entityApplyShell(scene, de.raw, e);
        created.push_back(e);
    }

    // ---- 第二遍：父子关系 + 文件夹 ----
    // ⚠ 必须等**所有**实体建完：`parentGuid` 要按 guid 反查，`parent` 下标
    //   可能指向文件里靠后的实体。
    // ⚠ 这一段是纯 CPU，所以它属于 ② 而不是 ③ —— 拆错的话"后台解析"出来的
    //   文档就带着一个没接好的层级，而且不报错。
    for (size_t i = 0; i < created.size(); ++i) {
        if (!created[i].valid()) continue;
        entityApplyHierarchy(scene, doc.entities[i].raw, created[i], created);
    }

    stats.entities = static_cast<int>(created.size());
}

// ③ 资产与其余组件。唯一要 AssetManager 的一段。
void resolveDocumentAssets(Scene& scene, assets::AssetManager& assets,
                           const SceneDocument& doc,
                           const std::vector<ecs::Entity>& created,
                           DocumentStats& stats) {
    if (!doc.hasEntities) return;
    const size_t n = std::min(created.size(), doc.entities.size());
    for (size_t i = 0; i < n; ++i) {
        if (!created[i].valid()) continue;
        if (!entityApplyComponents(scene, assets, doc.entities[i].raw, created[i]))
            ++stats.skipped;
    }
}

// ============================================================
// 公开：单实体 ↔ JSON（见 scene/SceneEntityIO.h）
//
// 这几个壳只做一件事：把匿名 namespace 里的 `entityTo*Impl` 递出去。
// **不要**在这里写第二套字段处理 —— 那正是抽它们出来要避免的。
// ============================================================

bool entityToJson(const ecs::World& w, ecs::Entity e,
                  const std::unordered_map<ecs::EntityId, int>& indexOf,
                  nlohmann::json& out, bool includeEmpty) {
    const auto* tr = w.get<ecs::TransformComponent>(e);
    if (!tr) {
        // 没有变换的实体（场景级太阳）从来没进过场景 JSON；
        // Prefab 也不该收它 —— 判"没内容"比判"没变换"更诚实的是后者，
        // 但调用方只关心"要不要这个实体"，两种都返回 false。
        out = nlohmann::json();
        return false;
    }
    return entityToJsonImpl(w, e, *tr, indexOf, out, includeEmpty);
}

bool entityFromJson(Scene& scene, assets::AssetManager& assets,
                    const nlohmann::json& je, ecs::Entity e,
                    const std::vector<ecs::Entity>& created) {
    return entityFromJsonImpl(scene, assets, je, e, created);
}

// 两段拆分（G16-5）的公开壳：与 `entityFromJson` 同样的形态 —— 只把
// 匿名 namespace 里的 impl 递出去，**不**在这里写第二套。
void entityApplyHierarchy(Scene& scene, const nlohmann::json& je, ecs::Entity e,
                          const std::vector<ecs::Entity>& created) {
    if (!e.valid()) return;
    entityApplyHierarchyImpl(scene, je, e, created);
}

bool entityApplyComponents(Scene& scene, assets::AssetManager& assets,
                           const nlohmann::json& je, ecs::Entity e) {
    if (!e.valid()) return false;
    return entityApplyComponentsImpl(scene, assets, je, e);
}

void entityApplyShell(Scene& scene, const nlohmann::json& je, ecs::Entity e) {
    ecs::World& w = scene.world();
    if (!e.valid()) return;

    if (je.contains("transform") && je["transform"].is_object()) {
        const json& jt = je["transform"];
        if (auto* t = w.get<ecs::TransformComponent>(e)) {
            if (jt.contains("position"))
                t->position = vec3FromJson(jt["position"], t->position);
            if (jt.contains("rotation"))
                t->rotation = vec3FromJson(jt["rotation"], t->rotation);
            // ⚠ scale 的默认值是 1 而不是 `t->scale`：缺 key 时若保留旧值，
            //   "把 scale 从 prefab 里删掉"就永远不生效。
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
    } else if (w.has<ecs::LockedComponent>(e)) {
        // 反向也要生效，否则 Revert 之后"锁定"会一直粘着。
        w.remove<ecs::LockedComponent>(e);
    }
}

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

    SceneIoResult r = loadSceneFromJson(scene, assets, root, path);
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
