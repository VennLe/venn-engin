#include "EditorScene.h"

#include "PickingSystem.h"
#include "PrimitiveBuilder.h"

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
    ecs::MeshComponent mc;
    mc.mesh = mesh;   // params 保持 None：几何由文件决定
    w.add<ecs::MeshComponent>(e, mc);
    w.add<ecs::MaterialComponent>(e, ecs::MaterialComponent{mat});
}

// 内置图元：几何 + 材质 + **生成参数**。
// 参数那一份是给 Inspector 用的 —— 加了之后那颗 Cube 的边长、球的
// 半径/分段数都能在右侧面板里回头改（见 editor/PrimitiveBuilder.h）。
// OBJ / glTF 走 attachRenderable，params 保持 None（几何由文件决定）。
void attachPrimitive(ecs::World& w, ecs::Entity e, assets::AssetManager& am,
                     ecs::MeshPrimitive kind, assets::Material* mat) {
    ecs::MeshComponent mc;
    mc.params = defaultPrimitiveParams(kind);
    rebuildPrimitive(am, mc);
    w.add<ecs::MeshComponent>(e, mc);
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
        case PrimitiveKind::Cylinder: return "Cylinder";
        case PrimitiveKind::PointLight: return "PointLight";
        case PrimitiveKind::SpotLight: return "SpotLight";
        case PrimitiveKind::Empty: return "Empty";
    }
    return "Object";
}

// 半高：网格类把中心抬这么高，底部就正好贴在 groundSpot.z 上
static float primitiveHalfHeight(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Cube: return 0.5f;      // cube(1.0)
        case PrimitiveKind::Sphere: return 0.5f;    // sphere(0.5)
        case PrimitiveKind::Cylinder: return 0.5f;  // cylinder(0.5) → 高 1
        case PrimitiveKind::Plane: return 0.0f;     // 平面就贴在面上
        default: return 0.0f;
    }
}

ecs::Entity createPrimitive(scene::Scene& scene, assets::AssetManager& assets,
                            PrimitiveKind kind, const glm::vec3& position) {
    ecs::World& w = scene.world();
    ecs::Entity e = scene.createObject(primitiveKindName(kind));
    if (auto* t = w.get<ecs::TransformComponent>(e)) t->position = position;

    switch (kind) {
        case PrimitiveKind::Cube:
            attachPrimitive(w, e, assets, ecs::MeshPrimitive::Cube,
                            paletteMaterial(assets, "editor_cube_default",
                                            200, 200, 205, 0.55f, 0.0f));
            break;
        case PrimitiveKind::Sphere:
            attachPrimitive(w, e, assets, ecs::MeshPrimitive::Sphere,
                            paletteMaterial(assets, "editor_sphere_default",
                                            210, 180, 120, 0.25f, 1.0f));
            break;
        case PrimitiveKind::Cylinder:
            attachPrimitive(w, e, assets, ecs::MeshPrimitive::Cylinder,
                            paletteMaterial(assets, "editor_cylinder_default",
                                            185, 200, 170, 0.45f, 0.0f));
            break;
        case PrimitiveKind::Plane:
            attachPrimitive(w, e, assets, ecs::MeshPrimitive::Plane,
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
            sl.direction = glm::vec3(0.0f, 0.0f, -1.0f);  // Z-up：朝下照
            w.add<ecs::SpotLightComponent>(e, sl);
            break;
        }
        case PrimitiveKind::Empty:
            break;
    }
    return e;
}

ecs::Entity createPrimitiveOnGround(scene::Scene& scene,
                                    assets::AssetManager& assets,
                                    PrimitiveKind kind,
                                    const glm::vec3& groundSpot) {
    glm::vec3 pos = groundSpot;
    switch (kind) {
        case PrimitiveKind::PointLight:
            pos.z += 1.6f;   // 挂在半空，别埋进地板
            break;
        case PrimitiveKind::SpotLight:
            pos.z += 2.6f;   // 射灯默认朝下照，放高一点才有光斑
            break;
        default:
            pos.z += primitiveHalfHeight(kind);
            break;
    }
    return createPrimitive(scene, assets, kind, pos);
}

// ---------------------------------------------------------------- 特殊灯光

ecs::Entity createSunlight(scene::Scene& scene) {
    // 日照预设（与 resetToEmptyScene 的默认太阳一致：
    // 暖白光、从上前方打下、开阴影、开阔场景的环境光强度）
    scene::Light& sun = scene.light();   // 没有就懒创建
    sun.direction = glm::normalize(glm::vec3(-0.38f, -0.42f, -1.0f));
    sun.color = glm::vec3(1.0f, 0.97f, 0.92f);
    sun.intensity = 1.6f;
    sun.castsShadow = true;
    sun.ambientScale = 0.55f;
    return scene.lightEntity();
}

ecs::Entity createDirectionalLight(scene::Scene& scene,
                                   const glm::vec3& position) {
    ecs::World& w = scene.world();
    ecs::Entity e = scene.createObject("Directional Light");
    if (auto* t = w.get<ecs::TransformComponent>(e)) t->position = position;

    ecs::DirectionalLightComponent dl;
    dl.direction = glm::vec3(0.0f, 0.0f, -1.0f);  // 默认竖直向下
    dl.color = glm::vec3(1.0f);
    dl.intensity = 1.0f;
    dl.castsShadow = false;   // 阴影只有场景级太阳有
    dl.ambientScale = 0.0f;
    w.add<ecs::DirectionalLightComponent>(e, dl);
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
        // Z-up：把包围盒的 **min.z**（底部）贴到 target.z（栅格面 z=0）
        const glm::vec3 c = (box.min + box.max) * 0.5f;
        shift = glm::vec3(target.x - c.x, target.y - c.y, target.z - box.min.z);
    } else {
        VK_LOG_WARN("alignImportToGround: no CPU-side mesh bounds, "
                    "placing root at the ray hit without ground snapping");
    }

    if (auto* t = scene.world().get<ecs::TransformComponent>(imported.root)) {
        t->position += shift;
        // 自动化断言用：算完之后"底部"应当**正好**等于 target.z。
        // （tools/verify_asset_drag.py 解析这一行来验证贴地是否生效。）
        VK_LOG_INFO(
            "alignImportToGround: box.min.z=%.4f -> bottom=%.4f "
            "(target.z=%.4f) root=(%.3f, %.3f, %.3f)",
            box.valid ? box.min.z : 0.0f,
            box.valid ? box.min.z + shift.z : 0.0f, target.z, t->position.x,
            t->position.y, t->position.z);
    }
}

// ---------------------------------------------------------------- 空场景

void resetToEmptyScene(scene::Scene& scene, assets::AssetManager& assets) {
    scene.clear();

    // 空场景也得"看得见东西"：一盏方向光（否则一片死黑，用户会以为坏了）
    // + 一个能一眼看到原点的相机机位。Z-up：光从上前方打下来。
    scene.light().direction = glm::normalize(glm::vec3(-0.38f, -0.42f, -1.0f));
    scene.light().color = glm::vec3(1.0f, 0.97f, 0.92f);
    scene.light().intensity = 1.6f;
    scene.light().castsShadow = true;
    scene.light().ambientScale = 0.55f;   // 开阔场景，环境光给足

    scene.camera().setTarget(glm::vec3(0.0f, 0.0f, 0.5f));
    scene.camera().setDistance(9.0f);
    scene.camera().setYaw(0.62f);
    scene.camera().setPitch(0.30f);

    // ---- 自带地面（UE5 手感的不透明平面）----
    // 尺寸与栅格的 extent（半边长 60 → 120×120）对齐：栅格正好铺满整块地面，
    // 边缘不会出现"线飘在没有地板的地方"。
    {
        constexpr float kGroundSize = 120.0f;   // 边长（米）
        ecs::World& w = scene.world();
        ecs::Entity ground = scene.createObject("Ground");
        if (auto* t = w.get<ecs::TransformComponent>(ground)) {
            t->position = glm::vec3(0.0f);   // 平面单面、居中于 z = 0 → 顶面 = 栅格面
            t->scale = glm::vec3(kGroundSize, kGroundSize, 1.0f);
        }
        attachRenderable(w, ground, assets.plane(1.0f, "editor_ground"),
                         paletteMaterial(assets, "editor_ground_default",
                                         105, 108, 114, 0.9f, 0.0f));
        // 地面只**接收**阴影、不投影：地板自投影只会引入 acne 条纹，
        // 而它本来也挡不住什么光（这也是 UE5 里 Floor 的默认设置）。
        if (auto* v = w.get<ecs::VisibilityComponent>(ground)) {
            v->castShadow = false;
        }
        // **地面固定不动**：它是编辑器的参照系，不该被平移 / 缩放 / 旋转。
        // LockedComponent 让视口点选跳过它、选中也不给变换手柄（工程里
        // 所有实体都支持这个标记，地面只是默认带上的那一个）。
        w.add<ecs::LockedComponent>(ground);
    }

    VK_LOG_INFO("Venn editor: empty scene (%zu entities)", scene.objectCount());
}

} // namespace editor
