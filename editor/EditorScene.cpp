#include "EditorScene.h"

#include "PickingSystem.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Material.h"
#include "assets/Mesh.h"
#include "assets/Model.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/Scene.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace editor {

namespace {

void attachRenderable(ecs::World& w, ecs::Entity e, assets::Mesh* mesh,
                      assets::Material* mat) {
    w.add<ecs::MeshComponent>(e, ecs::MeshComponent{mesh});
    w.add<ecs::MaterialComponent>(e, ecs::MaterialComponent{mat});
}

// 材质用的通用调色板（够用就好，不追求美术效果）
assets::Material* paletteMaterial(assets::AssetManager& am, const char* name,
                                  uint8_t r, uint8_t g, uint8_t b,
                                  float roughness, float metallic) {
    uint8_t c[4] = {r, g, b, 255};
    return am.makeMaterial(name, am.solid(std::string(name) + "_tex", c),
                           glm::vec4(1.0f), roughness, metallic);
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool isObjPath(const std::string& p) {
    return endsWith(p, ".obj") || endsWith(p, ".OBJ");
}

// "assets/models/Cube/Cube.gltf" → "Cube"
// ⚠ std::filesystem::path 不能直接赋给 std::string，必须显式 .string()
std::string pathStem(const std::string& p) {
    const std::string s = std::filesystem::path(p).stem().string();
    return s.empty() ? std::string("Model") : s;
}

} // namespace

const char* primitiveKindName(PrimitiveKind k) {
    switch (k) {
        case PrimitiveKind::Cube: return "Cube";
        case PrimitiveKind::Sphere: return "Sphere";
        case PrimitiveKind::Plane: return "Plane";
        case PrimitiveKind::PointLight: return "PointLight";
        case PrimitiveKind::SpotLight: return "SpotLight";
        case PrimitiveKind::Empty: return "Empty";
    }
    return "Object";
}

ecs::Entity createPrimitive(scene::Scene& scene, assets::AssetManager& assets,
                            PrimitiveKind kind, const glm::vec3& position) {
    ecs::World& w = scene.world();
    ecs::Entity e = scene.createObject(primitiveKindName(kind));
    if (auto* t = w.get<ecs::TransformComponent>(e)) t->position = position;

    switch (kind) {
        case PrimitiveKind::Cube:
            attachRenderable(w, e, assets.cube(1.0f),
                             paletteMaterial(assets, "editor_cube_default",
                                             200, 200, 205, 0.55f, 0.0f));
            break;
        case PrimitiveKind::Sphere:
            attachRenderable(w, e, assets.sphere(0.5f),
                             paletteMaterial(assets, "editor_sphere_default",
                                             210, 180, 120, 0.25f, 1.0f));
            break;
        case PrimitiveKind::Plane:
            attachRenderable(w, e, assets.plane(2.0f),
                             paletteMaterial(assets, "editor_plane_default",
                                             170, 172, 178, 0.85f, 0.0f));
            break;
        case PrimitiveKind::PointLight: {
            ecs::PointLightComponent pl;
            pl.color = glm::vec3(1.0f, 0.92f, 0.80f);
            pl.intensity = 14.0f;
            pl.range = 5.0f;
            w.add<ecs::PointLightComponent>(e, pl);
            break;
        }
        case PrimitiveKind::SpotLight: {
            ecs::SpotLightComponent sl;
            sl.color = glm::vec3(0.92f, 0.96f, 1.0f);
            sl.intensity = 32.0f;
            sl.range = 9.0f;
            sl.direction = glm::vec3(0.0f, -1.0f, 0.0f);
            w.add<ecs::SpotLightComponent>(e, sl);
            break;
        }
        case PrimitiveKind::Empty:
            break;
    }
    return e;
}

// ---------------------------------------------------------------- 模型导入

ImportResult instantiateModel(scene::Scene& scene, assets::AssetManager& assets,
                              const std::string& modelRelativePath,
                              const glm::vec3& position) {
    ecs::World& w = scene.world();
    ImportResult out;

    try {
        const std::string resolved = assets::resolveAssetPath(modelRelativePath);

        // ---- .obj：走 OBJ 加载器（单网格）----
        if (isObjPath(modelRelativePath)) {
            assets::Mesh* mesh = assets.loadModel(resolved, modelRelativePath);
            if (!mesh || !mesh->valid()) return out;

            out.root = scene.createObject(pathStem(modelRelativePath));
            if (auto* t = w.get<ecs::TransformComponent>(out.root))
                t->position = position;

            ecs::Entity e = scene.createObject(pathStem(modelRelativePath) + "_mesh");
            attachRenderable(w, e, mesh,
                             paletteMaterial(assets, "editor_obj_default",
                                             190, 190, 195, 0.6f, 0.0f));
            scene.setParent(e, out.root);   // 局部变换 = 单位，挂在根下
            out.entities.push_back(e);
            return out;
        }

        // ---- 其余按 glTF 处理 ----
        assets::Model* model = assets.loadGLTF(resolved, modelRelativePath);
        if (!model || !model->valid()) return out;

        out.root = scene.createObject(pathStem(modelRelativePath));
        if (auto* t = w.get<ecs::TransformComponent>(out.root))
            t->position = position;

        for (const auto& sub : model->subMeshes) {
            ecs::Entity e = scene.createObject(sub.name + "_inst");
            attachRenderable(w, e, sub.mesh, sub.material);

            // glTF 节点的世界变换分解成 TRS。
            // 这是**相对根节点**的局部变换 —— 根节点在原点、姿态为单位阵，
            // 所以直接把 glTF 的 TRS 塞进去即可，位置部分不含 position
            // （position 由根节点负责）。
            glm::vec3 scale(1.0f), translation(0.0f), skew(0.0f);
            glm::quat orient(1.0f, 0.0f, 0.0f, 0.0f);
            glm::vec4 perspective(0.0f);
            glm::decompose(sub.transform, scale, orient, translation, skew,
                           perspective);

            if (auto* t = w.get<ecs::TransformComponent>(e)) {
                t->position = translation;
                t->rotation = glm::eulerAngles(orient);
                t->scale = scale;
            }
            scene.setParent(e, out.root);
            out.entities.push_back(e);
        }
    } catch (const std::exception& ex) {
        VK_LOG_WARN("instantiateModel failed (%s): %s", modelRelativePath.c_str(),
                    ex.what());
    }
    return out;
}

void alignImportToGround(scene::Scene& scene, PickingSystem& picking,
                         const ImportResult& imported, const glm::vec3& target) {
    if (!imported.root.valid()) return;

    // 联合包围盒（世界空间）。任何一个子网格给了有效盒就算数。
    Aabb box;
    for (ecs::Entity e : imported.entities) {
        const Aabb b = picking.worldAabb(scene, e);
        if (!b.valid) continue;
        if (!box.valid) {
            box = b;
        } else {
            box.min = glm::min(box.min, b.min);
            box.max = glm::max(box.max, b.max);
        }
    }

    glm::vec3 shift(0.0f);
    if (box.valid) {
        const glm::vec3 c = (box.min + box.max) * 0.5f;
        shift = glm::vec3(target.x - c.x, target.y - box.min.y, target.z - c.z);
    } else {
        VK_LOG_WARN("alignImportToGround: no CPU-side mesh bounds, "
                    "placing root at the ray hit without ground snapping");
    }

    if (auto* t = scene.world().get<ecs::TransformComponent>(imported.root)) {
        t->position += shift;
        // 自动化断言用：算完之后"底部"应当**正好**等于 target.y。
        // （tools/verify_asset_drag.py 解析这一行来验证贴地是否生效。）
        VK_LOG_INFO(
            "alignImportToGround: box.min.y=%.4f -> bottom=%.4f "
            "(target.y=%.4f) root=(%.3f, %.3f, %.3f)",
            box.valid ? box.min.y : 0.0f,
            box.valid ? box.min.y + shift.y : 0.0f, target.y, t->position.x,
            t->position.y, t->position.z);
    }
}

// ---------------------------------------------------------------- 空场景

void resetToEmptyScene(scene::Scene& scene) {
    scene.clear();

    // 空场景也得"看得见东西"：一盏方向光（否则一片死黑，用户会以为坏了）
    // + 一个能一眼看到原点的相机机位。
    scene.light().direction = glm::normalize(glm::vec3(-0.42f, -1.0f, -0.38f));
    scene.light().color = glm::vec3(1.0f, 0.97f, 0.92f);
    scene.light().intensity = 1.6f;
    scene.light().castsShadow = true;
    scene.light().ambientScale = 0.55f;   // 开阔场景，环境光给足

    scene.camera().setTarget(glm::vec3(0.0f, 0.5f, 0.0f));
    scene.camera().setDistance(9.0f);
    scene.camera().setYaw(0.62f);
    scene.camera().setPitch(0.30f);

    VK_LOG_INFO("Venn editor: empty scene (%zu entities)", scene.objectCount());
}

} // namespace editor
