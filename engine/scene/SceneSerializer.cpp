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
        return am.checker(name.empty() ? "checker" : name, a, b, cell, size);
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
    j["alphaBlend"] = (mat->alphaMode == assets::Material::AlphaMode::Blend);
    j["albedo"] = textureSourceToJson(mat->albedoMap);
    j["normal"] = textureSourceToJson(mat->normalMap);
    j["orm"] = textureSourceToJson(mat->ormMap);
    // 单因子贴图槽（白/白/黑 是"未绑定"的中性值，各自与因子相乘或相加）
    j["roughnessMap"] = textureSourceToJson(mat->roughnessMap);
    j["metallicMap"] = textureSourceToJson(mat->metallicMap);
    j["emissiveMap"] = textureSourceToJson(mat->emissiveMap);
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

    assets::Material* mat = am.makeMaterialPBR(
        name, albedo, normal, orm, baseColor, roughness, metallic, emissive,
        doubleSided);
    if (mat) {
        // makeMaterialPBR 未覆盖这几项，补齐以保证往返一致
        mat->roughnessMap = roughnessMap;
        mat->metallicMap = metallicMap;
        mat->emissiveMap = emissiveMap;
        mat->normalScale = normalScale;
        mat->ao = ao;
        mat->alphaMode = jm.value("alphaBlend", false)
                             ? assets::Material::AlphaMode::Blend
                             : assets::Material::AlphaMode::Opaque;
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
    cam.setTarget(vec3FromJson(
        jc.contains("target") ? jc["target"] : json(nullptr), cam.target()));
    cam.setYaw(jc.value("yaw", cam.yaw()));
    cam.setPitch(jc.value("pitch", cam.pitch()));
    cam.setDistance(jc.value("distance", cam.distance()));
    cam.setFov(jc.value("fov", cam.fov()));
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
    };
}

json spotLightToJson(const ecs::SpotLightComponent& L) {
    return json{
        {"color", vec3ToJson(L.color)},   {"intensity", L.intensity},
        {"range", L.range},               {"direction", vec3ToJson(L.direction)},
        {"innerAngle", L.innerAngle},     {"outerAngle", L.outerAngle},
        {"enabled", L.enabled},
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
    root["light"] = lightToJson(scene.light());

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

        // 脚本（只有路径与开关，脚本源码本身是独立文件）
        if (const auto* sc = w.get<ecs::ScriptComponent>(it.entity)) {
            je["script"] = json{
                {"path", sc->path},
                {"enabled", sc->enabled},
                {"timeScale", sc->timeScale},
            };
        }

        if (je["mesh"].is_null()) ++r.skipped;  // 无网格不算失败，只是没内容

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
    applyLight(scene.light(), root.contains("light") ? root["light"]
                                                     : json(nullptr));

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

        if (!mesh && !mat) ++r.skipped;
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
