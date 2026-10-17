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

json materialToJson(const assets::Material* mat) {
    if (!mat) return nullptr;

    json j;
    j["name"] = mat->name;

    // glTF 派生的材质：只记"来自哪个模型"，重建 = 重新加载该模型
    if (mat->source.kind == assets::MaterialSource::Kind::Model) {
        j["kind"] = "model";
        j["modelKey"] = mat->source.modelKey;
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

    if (kind == "model") {
        // 网格解析时通常已经重新加载过模型，材质随之登记完毕
        assets::Material* m = am.findMaterial(name);
        if (m) return m;

        // 兜底：万一实体只有材质没有网格，这里补一次模型加载
        const std::string modelKey = jm.value("modelKey", std::string());
        if (!modelKey.empty()) {
            try {
                am.loadGLTF(assets::resolveAssetPath(modelKey), modelKey);
            } catch (const std::exception& ex) {
                VK_LOG_WARN(
                    "SceneSerializer: model reload for material failed (%s): %s",
                    modelKey.c_str(), ex.what());
            }
            m = am.findMaterial(name);
        }
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
    cam.setYaw(jc.value("yaw", cam.yaw()));
    cam.setPitch(jc.value("pitch", cam.pitch()));
    cam.setDistance(jc.value("distance", cam.distance()));
    cam.setTarget(vec3FromJson(
        jc.contains("target") ? jc["target"] : json(nullptr), cam.target()));
    cam.setFov(jc.value("fov", cam.fov()));
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

        // 反射探针（R5-1）。与贴花同理：探针**没有网格**，
        // 下面那句 "mesh 为空 → skipped++" 对它也不适用。
        if (const auto* rp = w.get<ecs::ReflectionProbeComponent>(it.entity))
            je["probe"] = reflectionProbeToJson(*rp);

        // GPU 粒子发射器（R6-1）。与贴花 / 探针同理：发射器**没有网格**
        //（几何是顶点着色器现拼的 billboard），同样不适用 skipped++ 那句。
        if (const auto* fx = w.get<ecs::ParticleEmitterComponent>(it.entity))
            je["particleEmitter"] = particleEmitterToJson(*fx);

        // 刚体（R7-1）。与贴花 / 探针 / 发射器同理：它也不带网格 ——
        // "隐形的墙"就是一个只有刚体的实体，所以下面那句 skipped++ 同样
        // 要为它开一个口子。位置/朝向已经在 transform 里，这里只存参数。
        if (const auto* rb = w.get<ecs::RigidBodyComponent>(it.entity))
            je["rigidbody"] = rigidBodyToJson(*rb);

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

        // 脚本（只有路径与开关，脚本源码本身是独立文件）
        if (const auto* sc = w.get<ecs::ScriptComponent>(it.entity)) {
            je["script"] = json{
                {"path", sc->path},
                {"enabled", sc->enabled},
                {"timeScale", sc->timeScale},
            };
        }

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
        // 触发体 / 物理材质 / 相机 任意一个存在就不算空 —— 它们都是
        // "没有网格但有意义"的实体（隐形的墙、触发区域、相机机位）。
        const bool hasNonMeshContent =
            je.contains("decal") || je.contains("probe") ||
            je.contains("particleEmitter") || je.contains("rigidbody") ||
            je.contains("character") || je.contains("trigger") ||
            je.contains("physicsMaterial") || je.contains("camera");
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

        const bool hasNonMeshContent =
            je.contains("decal") || je.contains("probe") ||
            je.contains("particleEmitter") || je.contains("rigidbody") ||
            je.contains("character") || je.contains("trigger") ||
            je.contains("physicsMaterial") || je.contains("camera");
        if (!mesh && !mat && !hasNonMeshContent) ++r.skipped;
    }

    r.ok = true;
    r.entities = static_cast<int>(created.size());
    return r;
}

} // namespace

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
