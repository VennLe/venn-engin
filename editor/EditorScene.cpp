#include "EditorScene.h"

#include "PickingSystem.h"
#include "PrimitiveBuilder.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Material.h"
#include "assets/Mesh.h"
#include "assets/Model.h"
#include "assets/ModelOrientation.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/ModelInstance.h"
#include "scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
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

bool isObjPath(const std::string& p) {
    // 判定本体已提到 `assets/AssetPath.*`（运行时的 `spawn()` 也要问同一句）。
    // 这里保留同名薄壳：本文件里几十个调用点不必改。
    return assets::isObjPath(p);
}

bool isFbxPath(const std::string& p) { return assets::isFbxPath(p); }

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
                                            200, 200, 205, 0.55f, 0.0f));
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
            // Plane 的几何面正好在 z=0，与自带 Ground 的顶面完全共面 →
            // z-fighting：两块面逐像素抢深度，看起来"灰白半透明、若影若现"
            // （透出来的就是 Ground 的灰色）。抬 1cm 视觉上仍然贴地。
            if (kind == PrimitiveKind::Plane) pos.z += 0.01f;
            break;
    }
    return createPrimitive(scene, assets, kind, pos);
}

// ---------------------------------------------------------------- 特殊灯光

// ------------------------------------------------------------
// Player（第一人称 / 第三人称）
//
// 实体 position = 玩家的"脚"（永远贴地，运行态只在水平面移动）。
// 第一人称：空节点 + 胶囊碰撞体 —— 编辑态碰撞线框就是"这里站着一个人"的
// 占位提示（用户明确说第一人称不用管模型，视角本身就是玩家）。
// 第三人称：只保留胶囊碰撞体占位（2026-10-11 起不再程序化拼小人）——
// 角色模型（静态网格 / 骨骼网格 + 动画）由"角色详情窗口"绑定。
// 胶囊：r=0.30、圆柱段半高 0.55 → 总高 1.70，中心抬 z=0.85 底部贴地。
// ------------------------------------------------------------
// ---------------------------------------------------------------- 默认角色
//
// 2026-10-23 用户要求（原话）：
//   "当前这个初始化的角色模型太粗糙了，E:\dev\UE-resources\Animate_Girl 这个
//   目录中有一个角色模型和对应的角色动画，你把它导入我的引擎中作为初始化
//   角色模型试试看，并且要绑定好动画。"
//
// 旧默认角色是 P2/P3 那套（`--strip-skin --autorig --procanim` 烤出来的
// 23 骨 + 四段程序化动画）：骨架和权重是量测法生成的，动画是正弦函数拼的，
// 所以看起来"粗糙"—— 那是**兜底资产**，不是成品角色。
//
// 新角色 = `assets/models/characters/animate_girl.vmesh`：
//   · 65 骨 Mixamo 骨架（`mixamorig:*`），网格 25626 顶点 / 7 个材质
//   · 四段**真人动捕**clip：Idle 6.00s / Walk 0.97s / Run 0.53s / Jump 1.00s
//   · 身高 1.626 m，100% 顶点有蒙皮权重
// 离线管线见 `tools/blender_import_animategirl.py`（Blender 绑骨 + 烘帧）
// 与 `docs/animate_girl_import.md`（含三个坑：坏 rest / 逐壳绑骨 / clip 名）。
//
// ⚠ **暂时没切成默认**（2026-10-23）：资产本身是干净的（GLB 里 26134 个顶点
//   权重和精确为 1.000、65 关节、四段 clip 名不带前缀），在 Blender 里回读
//   也完全正常；但引擎这边——不论走 `.vmesh`(AssetC/FBX) 还是原生 glTF——
//   都在**静止姿势**就已经塌成一片（不是动画的问题：关掉 clip 一样塌）。
//   说明 `invMeshNode · world · invBind` 这条链对"mesh 节点带非单位变换 /
//   骨架在 Armature 子节点下"的资产算错了，与 65 骨无关（AutoRig 的 23 骨
//   资产 `jointMat` 恰好恒等，所以从来没暴露）。根因没钉死之前不换默认，
//   免得把一个坏角色推给所有人。详见 docs/animate_girl_import.md §5。
//
// 旧资产 `default_character.vmesh` **保留在盘上**：它是 23 骨、1.805 m，
//   好几个物理用例（verify_player_feel / ground / jump）在自己的场景 JSON 里
//   显式指名它。删掉会让那些用例从"跑起来"变成"找不到模型"。
namespace {

constexpr const char* kDefaultCharacterPath = kDefaultCharacterAsset;

// 模型正面补偿角（度）。FBX 源（Y-up、正面朝 +Z）经 ufbx 的 (x,-z,y) 映射
// 后正面落在 **-Y**，而引擎实体的 yaw 语义是"0 = 朝 +X"，所以 +90°。
// ⚠ 必须与 `RuntimeConfig::PlayerVisual::modelYaw` 的默认值相同：这条链上
//   有两处装配（编辑器 createPlayer / 运行时 spawnPlayerFromGameStart），
//   差一个符号就会变成"编辑器里朝前、Game.exe 里横着走"。
constexpr float kDefaultModelYaw = 90.0f;

// 模型包围盒的 Z 范围（8 个角点各乘一次 SubMesh 变换再取 min/max）。
// 与 CharacterWindow::computeModelBounds / RuntimeApp::modelWorldBounds
// 同一套算法 —— 对带旋转的部件不会算出偏小的盒子。
bool modelBoundsZ(const assets::Model& m, float* lo, float* hi) {
    bool any = false;
    for (const auto& sub : m.subMeshes) {
        if (!sub.mesh || !sub.mesh->hasBounds()) continue;
        const glm::vec3 a = sub.mesh->boundsMin();
        const glm::vec3 b = sub.mesh->boundsMax();
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 c((i & 1) ? b.x : a.x, (i & 2) ? b.y : a.y,
                              (i & 4) ? b.z : a.z);
            const float z = (sub.transform * glm::vec4(c, 1.0f)).z;
            if (!any) {
                *lo = *hi = z;
                any = true;
            } else {
                *lo = std::min(*lo, z);
                *hi = std::max(*hi, z);
            }
        }
    }
    return any;
}

// 模型包围盒的 XY 跨度（含朝向补偿）。
// 用来做 facing 自检：人形的**左右跨度必须大于前后跨度**
// （肩宽 ~0.45 m vs 身体厚 ~0.25 m）。与 `RuntimeApp::modelWorldBoundsYaw`
// 同一判据 —— 两边装配必须朝同一个方向。
bool modelBoundsXY(const assets::Model& m, float yawDeg, glm::vec3* lo,
                   glm::vec3* hi) {
    const glm::mat4 rot =
        glm::rotate(glm::mat4(1.0f), glm::radians(yawDeg),
                    glm::vec3(0.0f, 0.0f, 1.0f));
    bool any = false;
    for (const auto& sub : m.subMeshes) {
        if (!sub.mesh || !sub.mesh->hasBounds()) continue;
        const glm::vec3 a = sub.mesh->boundsMin();
        const glm::vec3 b = sub.mesh->boundsMax();
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 c((i & 1) ? b.x : a.x, (i & 2) ? b.y : a.y,
                              (i & 4) ? b.z : a.z);
            const glm::vec3 p = glm::vec3(rot * sub.transform * glm::vec4(c, 1.0f));
            if (!any) {
                *lo = *hi = p;
                any = true;
            } else {
                *lo = glm::min(*lo, p);
                *hi = glm::max(*hi, p);
            }
        }
    }
    return any;
}

// 把默认角色装到玩家实体上：模型 + 蒙皮 + 胶囊/相机（按模型身高）+ 起始 clip。
//
// 失败时**什么也不做**（玩家退回胶囊占位体）并打一行 WARN：资产缺失不该让
// "创建角色"这个动作整套失败。
void bindDefaultCharacter(scene::Scene& scene, assets::AssetManager& am,
                          ecs::Entity player) {
    ecs::World& w = scene.world();

    assets::Model* model = nullptr;
    try {
        // 按扩展名分派：`loadFBX` 会先探同名 .vmesh，再落到 ufbx；
        // `.glb/.gltf` 必须走 `loadGLTF`，否则会被当成 FBX 解析而失败。
        const std::string relPath = kDefaultCharacterPath;
        const bool isGltf = relPath.size() > 4 &&
            (relPath.compare(relPath.size() - 4, 4, ".glb") == 0 ||
             relPath.compare(relPath.size() - 5, 5, ".gltf") == 0);
        const std::string absPath = assets::resolveAssetPath(relPath);
        model = isGltf ? am.loadGLTF(absPath, relPath)
                       : am.loadFBX(absPath, relPath);
    } catch (const std::exception& ex) {
        VK_LOG_WARN("PLAYER-DEFAULT load failed path=%s error=%s",
                    kDefaultCharacterPath, ex.what());
        return;
    }
    if (!model || !model->valid()) {
        VK_LOG_WARN("PLAYER-DEFAULT model invalid path=%s（玩家退回胶囊占位体）",
                    kDefaultCharacterPath);
        return;
    }

    scene::ModelInstanceOptions opt;
    opt.parent = player;
    opt.autoPlay = true;              // 挂上去就播（起始 = Idle）
    opt.sourcePath = kDefaultCharacterPath;
    // ---- 朝向补偿（P5 / 2026-10-24 重做）----
    //
    // 以前这里写死 `kDefaultModelYaw = 90`（"FBX 正面落在 -Y"）。默认角色
    // 换成 VRM 来源的 alicia 之后那条假设失效了：它的正面在 **+Y**，+90°
    // 会把人转成 -X —— 用户看到的就是"往前走、脸朝后"。
    //
    // 现在**问骨架**：由"脚踝 → 脚趾"的方向反解补偿角（见
    // assets/ModelOrientation.h 的长注释）。骨架里找不到 toe 骨才退回声明的值。
    float derivedYaw = kDefaultModelYaw;
    glm::vec2 localFwd(0.0f);
    float rawYaw = derivedYaw;
    const bool derivedYawOk =
        assets::modelForwardYawDeg(*model, &derivedYaw, &localFwd, &rawYaw);
    if (!derivedYawOk) derivedYaw = kDefaultModelYaw;
    opt.yawOffsetDeg = derivedYaw;
    const scene::ModelInstanceResult r = scene::instantiateModel(scene, model, opt);
    // 验收锚点（别删）：朝向是"看起来对不对"里最难用像素断言的一项，
    // 把判定依据整条打出来 —— 退回到声明值时也能从日志看出来。
    VK_LOG_INFO("PLAYER-DEFAULT forward local=(%.3f,%.3f) rawYaw=%.1f "
                "yaw=%.1f derived=%d (declared=%.1f)",
                static_cast<double>(localFwd.x),
                static_cast<double>(localFwd.y),
                static_cast<double>(rawYaw), static_cast<double>(derivedYaw),
                derivedYawOk ? 1 : 0, static_cast<double>(kDefaultModelYaw));

    // ---- 抬到脚底：模型的 pivot 在几何中心，玩家实体的原点是**脚** ----
    //
    // ⚠ 不能用 `dropToGround`：那条只在"自建根节点"时生效（见
    //   ModelInstanceOptions 的注释），这里的父节点就是玩家。
    float lo = 0.0f, hi = 0.0f;
    const bool hasBounds = modelBoundsZ(*model, &lo, &hi);
    const float height = hasBounds ? std::max(hi - lo, 0.2f) : 1.8f;
    const float lift = hasBounds ? -lo : 0.0f;
    if (hasBounds) {
        for (ecs::Entity ce : r.entities) {
            if (auto* t = w.get<ecs::TransformComponent>(ce))
                t->position.z += lift;
        }
    }

    // ---- 解剖比例（与 CharacterWindow::generateSkeletonBinding、
    //      RuntimeApp::attachPlayerModel 同一套）：人形高 h →
    //      半径 ≈ 0.16h，胶囊总高 = 2 × (halfHeight + radius) = h ----
    const float radius = glm::clamp(height * 0.16f, 0.15f, 0.60f);
    const float halfHeight = std::max(0.10f, height * 0.5f - radius);

    ecs::CharacterControllerComponent ctrl;
    ctrl.radius = radius;
    ctrl.halfHeight = halfHeight;
    ctrl.stepHeight = glm::clamp(height * 0.20f, 0.20f, 0.50f);
    // ---- 跳跃：按身高缩放（2026-10-22）----
    // 目标是"跳起 0.39 × 身高"：1.8 m 的人 → 0.70 m，这是普通人立定跳的
    // 竖直高度（不是"游戏里好看"的 1.2 m）。v₀ = √(2gh) 反算初速，于是
    // 跳得高的角色滞空也更长 —— 与 Jump clip 的 0.75 s 对齐。
    ctrl.jumpSpeed = glm::clamp(std::sqrt(2.0f * 9.81f * 0.39f * height),
                                3.0f, 5.0f);
    ctrl.airControl = 0.45f;
    w.add<ecs::CharacterControllerComponent>(player, ctrl);

    // 编辑态的胶囊线框跟着模型走：以前是写死的 0.30/0.55（1.7 m），
    // 换一个 2 m 的角色就"框比人小一圈"，撞墙判定会提前。
    if (auto* col = w.get<ecs::CollisionComponent>(player)) {
        col->capsuleRadius = radius;
        col->capsuleHalfHeight = halfHeight;
        col->position = glm::vec3(0.0f, 0.0f, halfHeight + radius);
    }

    // 编辑器 Play 的"便携移动"（runPlayers）读这三个数
    if (auto* pc = w.get<ecs::PlayerComponent>(player)) {
        pc->eyeHeight = height * 0.92f;
        pc->camDistance = glm::clamp(height * 2.0f, 2.5f, 6.5f);
    }

    // 运行时 PlayerController 的弹簧臂参数（没有它镜头不跟随）
    ecs::CameraComponent camC;
    camC.targetOffset = glm::vec3(0.0f, 0.0f, height * 0.42f);
    camC.distance = glm::clamp(height * 2.0f, 2.5f, 6.5f);
    camC.minDistance = 0.6f;
    w.add<ecs::CameraComponent>(player, camC);

    // 起始 clip：Idle（状态机第一次跑之前也得有个合理姿势）
    if (auto* sk = w.get<ecs::SkinnedMeshComponent>(player)) {
        if (model->skeleton && model->skeleton->findClip("Idle"))
            sk->clipName = "Idle";
        sk->playing = true;
        sk->loop = true;
    }

    // 诊断：clip 名逐字打印。`findClip` 是 `c.name == name` 的逐字比较，
    // 名字里多一个前缀/空格就静默退回 rest —— 画面表现为"角色站成 T-pose
    // 但日志全绿"，正是之前踩过的形态。
    std::string clipNames;
    if (model->skeleton) {
        for (const auto& c : model->skeleton->clips) {
            if (!clipNames.empty()) clipNames += ",";
            clipNames += c.name;
        }
    }
    VK_LOG_INFO("PLAYER-DEFAULT clipNames=[%s] clipName=[%s]",
                clipNames.c_str(),
                w.get<ecs::SkinnedMeshComponent>(player)
                    ? w.get<ecs::SkinnedMeshComponent>(player)->clipName.c_str()
                    : "?");

    VK_LOG_INFO("PLAYER-DEFAULT bound path=%s meshes=%d skinned=%d joints=%d "
                "clips=%d height=%.3f capsule=%.3f/%.3f yaw=%.1f "
                "lift=%.3f boundsZ=%.3f..%.3f -> bottom=%.4f",
                kDefaultCharacterPath, static_cast<int>(r.entities.size()),
                r.skinned ? 1 : 0, r.joints,
                model->skeleton ? static_cast<int>(model->skeleton->clips.size()) : 0,
                static_cast<double>(height), static_cast<double>(radius),
                static_cast<double>(halfHeight),
                static_cast<double>(derivedYaw),
                static_cast<double>(lift), static_cast<double>(lo),
                static_cast<double>(hi),
                static_cast<double>(lo + lift));

    // 朝向自检（与 RuntimeApp::attachPlayerModel 同判据，见那边的注释）
    //
    // ⚠ 2026-10-24 追加了**带符号**的那一条。老判据只比"臂展轴 vs 体厚轴"，
    //   而这恰好对 180° 相反**不敏感** —— alicia 戴反了 180° 它照样绿。
    //   现在再加一条：补偿后的脚尖方向必须落在 +X（实体的"前"）。
    glm::vec3 fLo(0.0f), fHi(0.0f);
    if (modelBoundsXY(*model, derivedYaw, &fLo, &fHi)) {
        const float spanX = fHi.x - fLo.x;
        const float spanY = fHi.y - fLo.y;
        // 局部正面 → 补偿后的世界正面：R(yaw)·f
        const float cy = std::cos(glm::radians(derivedYaw));
        const float sy = std::sin(glm::radians(derivedYaw));
        const glm::vec2 face(cy * localFwd.x - sy * localFwd.y,
                             sy * localFwd.x + cy * localFwd.y);
        const bool forwardOk = derivedYawOk ? (face.x > 0.9f) : true;
        VK_LOG_INFO("PLAYER-DEFAULT facing yaw=%.1f spanX=%.3f spanY=%.3f "
                    "shouldersAlong=%s ok=%d face=(%.3f,%.3f) forwardOk=%d",
                    static_cast<double>(derivedYaw),
                    static_cast<double>(spanX), static_cast<double>(spanY),
                    spanX < spanY ? "Y" : "X", spanX < spanY ? 1 : 0,
                    static_cast<double>(face.x), static_cast<double>(face.y),
                    forwardOk ? 1 : 0);
        if (!forwardOk)
            VK_LOG_WARN("PLAYER-DEFAULT 朝向补偿疑似差 180°（脸落到 -X，"
                        "人会在倒退着走）——见 assets/ModelOrientation.h");
    }
}

} // namespace

// ============================================================
// 运行时装配：按 GameStart 生成玩家（2026-10-24）
// ============================================================
// 用户原话："GameStart 不仅仅是角色的一个标记，也是游戏运行后角色初始化的
// 坐标，在添加人称角色时不需要直接把角色加入到 3d 视口中，只是在游戏运行
// 后，把角色初始化到 GameStart 位置即可。"
//
// 于是场景文件里**不再有摆着的角色**：Player 实体是按下 Play 那一刻才生成
// 的（Game.exe 里是 `RuntimeApp::spawnPlayerFromGameStart`，同一套口径）。
//
// 两份实现不能合并（editor/ 不能依赖 runtime/），所以约束写在这里：
//   · 脚底 = GameStart 的 transform，朝向 = transform.rotation.z + yaw；
//   · 第三人称装配默认角色（模型 + 骨骼 + 四段动画），第一人称**不装**
//     （相机就在脑袋里，装了只会看到头颅内侧）；
//   · 胶囊尺寸按角色真实身高折算（`bindDefaultCharacter` 内部做的）。
ecs::Entity spawnPlayerAtGameStart(scene::Scene& scene,
                                   assets::AssetManager& assets,
                                   const glm::vec3& feet, float yaw,
                                   ecs::PlayerMode mode,
                                   ecs::Entity configFrom) {
    ecs::World& w = scene.world();
    ecs::Entity e = scene.createObject("Player");
    if (!e.valid()) return {};
    if (auto* t = w.get<ecs::TransformComponent>(e)) {
        t->position = feet;
        t->rotation = glm::vec3(0.0f, 0.0f, yaw);
        t->scale = glm::vec3(1.0f);
    }

    // ---- 角色配置跟着出生点走 ----
    //
    // 角色面板（CharacterWindow）在编辑态绑的按键映射挂在**出生点**实体上
    // （视口里没有 Player 可挂），而 `EditorApp::runPlayers` 是从**玩家实体**
    // 上读 `AnimKeyMapComponent` 的。不搬这一手，用户绑的 K→Jump 在 Play 里
    // 一声不响（2026-10-24 实测：C9 挂在"没有 EDIT-ANIM-KEY-TRIGGER"）。
    //
    // 只搬"配置型"组件，不搬网格/骨骼 —— 那些是运行时装配出来的。
    if (configFrom.valid()) {
        if (const auto* km = w.get<ecs::AnimKeyMapComponent>(configFrom))
            w.add<ecs::AnimKeyMapComponent>(e, *km);
    }

    // ⚠ 顺序死板：PlayerComponent 必须先挂，`bindDefaultCharacter` 要按它写
    //   eyeHeight / camDistance；CC / Camera 反而是它自己的产物（`add` 是
    //   "有就覆盖"，先放一份兜底值不冲突，但第一人称没有那一步，得自己来）。
    ecs::PlayerComponent pc;
    pc.mode = mode;
    pc.yaw = yaw;
    pc.bodyYaw = yaw;
    // 第三人称初始略微低头 → 相机落在角色斜上方俯视（与老 createPlayer 同）
    if (mode == ecs::PlayerMode::ThirdPerson) pc.pitch = -0.35f;
    w.add<ecs::PlayerComponent>(e, pc);

    constexpr float kFallbackHeight = 1.8f;
    {
        const float radius = kFallbackHeight * 0.16f;
        ecs::CharacterControllerComponent cc;
        cc.radius = radius;
        cc.halfHeight = kFallbackHeight * 0.5f - radius;
        cc.bodyYaw = yaw;
        // 实体原点是脚底，物理状态要胶囊中心 —— 差 (halfHeight + radius)。
        cc.position = feet + glm::vec3(0.0f, 0.0f, cc.halfHeight + cc.radius);
        w.add<ecs::CharacterControllerComponent>(e, cc);

        ecs::CameraComponent camC;
        camC.targetOffset = glm::vec3(0.0f, 0.0f, kFallbackHeight * 0.42f);
        camC.distance = 3.6f;
        camC.minDistance = 0.6f;
        w.add<ecs::CameraComponent>(e, camC);
    }

    if (mode == ecs::PlayerMode::ThirdPerson)
        bindDefaultCharacter(scene, assets, e);

    VK_LOG_INFO("PLAYER-SPAWN at GameStart feet=(%.2f,%.2f,%.2f) yaw=%.3f "
                "mode=%s visual=%s",
                static_cast<double>(feet.x), static_cast<double>(feet.y),
                static_cast<double>(feet.z), static_cast<double>(yaw),
                mode == ecs::PlayerMode::FirstPerson ? "first" : "third",
                mode == ecs::PlayerMode::FirstPerson ? "none(first-person)"
                                                    : "default");
    return e;
}

// ============================================================
// 添加人称角色 = 配置出生点（不再往视口里塞角色）
// ============================================================
// 以前这里会建一个带网格的 "Player (Third-Person)" 实体并立刻装配角色：
// 用户按 Shift+A 之后，一个角色出现在鼠标落点，同时场景里还有另一个叫
// GameStart 的标记 —— 两个东西各指一处，Play 时到底以谁为准没人说得清。
//
// 现在只做三件事：
//   ① 找（或新建）GameStart；
//   ② 把它摆到鼠标落点、按地形高度贴地；
//   ③ 写入视角模式。
// 返回 GameStart 实体本身（选中它 → Inspector 里能看到并改这些参数）。
ecs::Entity createPlayer(scene::Scene& scene, assets::AssetManager& assets,
                         ecs::PlayerMode mode,
                         const glm::vec3& groundSpot) {
    (void)assets;   // 角色在运行时才装配，这里不需要任何资产
    ecs::World& w = scene.world();

    ecs::Entity gs{};
    w.each<ecs::GameStartComponent>(
        [&](ecs::Entity e, ecs::GameStartComponent&) {
            if (!gs.valid()) gs = e;   // 一个场景只认一个出生点
        });

    const bool created = !gs.valid();
    if (created) {
        gs = scene.createObject("GameStart");
        if (!gs.valid()) return {};
        if (auto* t = w.get<ecs::TransformComponent>(gs))
            t->position = groundSpot;
    }

    // 已有出生点就**只改模式、不搬家**：绝大多数工程里那个 GameStart 是
    // 建地形时摆好的（已经贴着地表），为了"加个第三人称"把它拽到当前鼠标
    // 位置，等于把作者摆好的出生点悄悄挪走。要挪的话它就在选中状态，
    // W/E/R 手柄随手就能挪。
    ecs::GameStartComponent g =
        w.get<ecs::GameStartComponent>(gs) ? *w.get<ecs::GameStartComponent>(gs)
                                           : ecs::GameStartComponent{};
    g.mode = mode;
    g.enabled = true;
    g.spawnOnRun = true;
    w.add<ecs::GameStartComponent>(gs, g);

    glm::vec3 at = groundSpot;
    if (const auto* t = w.get<ecs::TransformComponent>(gs)) at = t->position;
    VK_LOG_INFO("PLAYER-START %s mode=%s pos=(%.2f,%.2f,%.2f) "
                "(角色在运行时生成，编辑态视口里不出现)",
                created ? "created" : "reconfigured",
                mode == ecs::PlayerMode::FirstPerson ? "first" : "third",
                static_cast<double>(at.x), static_cast<double>(at.y),
                static_cast<double>(at.z));
    return gs;
}

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

namespace {

// 同名对象已存在时加 `_1` / `_2` … 后缀（UE 里重复拖同一个资产的 `anime_girl1`）。
// ⚠ 不是为了好看：多实例场景（A0-4 的否定测试）要求三个同名 FBX 能被**分别**
//   指名，否则 `Scene::find` 只命中第一个，`anim.time <t> <name>` 永远改不到
//   第二、第三个 —— 于是"三个 jointBase 两两不等"就退化成一个实例的假绿。
std::string uniqueRootName(scene::Scene& scene, const std::string& base) {
    if (!scene.find(base).valid()) return base;
    for (int i = 1; i < 1000; ++i) {
        const std::string cand = base + "_" + std::to_string(i);
        if (!scene.find(cand).valid()) return cand;
    }
    return base;
}

} // namespace

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

        // ---- 其余按 FBX / glTF 分派（两者都产出 Model + SubMesh 列表）----
        assets::Model* model = isFbxPath(modelRelativePath)
                                   ? assets.loadFBX(resolved, modelRelativePath)
                                   : assets.loadGLTF(resolved, modelRelativePath);
        if (!model || !model->valid()) return out;

        // 装配交给 engine/scene/ModelInstance —— 它同时也是**唯一**
        // 会建 SkinnedMeshComponent 的地方。以前这段代码在这里手写，
        // 从不判 model->skinned()，于是带骨骼的 FBX 拖进来只是一堆
        // 静态网格（T-pose），而 Game.exe 走同一条路径 → 运行时永远
        // 看不到蒙皮角色。现在两条入口共用一份实现。
        scene::ModelInstanceOptions opt;
        opt.subNameSuffix = "_inst";
        opt.position = position;
        opt.rootName = uniqueRootName(scene, pathStem(modelRelativePath));
        opt.autoPlay = true;    // 拖进主场景就该动起来
        opt.sourcePath = modelRelativePath;
        // 拖进来的东西应该**站在地上**，而不是"几何中心在原点"。
        // 模型 pivot 在几何中心，不抬的话 1.8 m 的人有一半埋在地里 ——
        // 拖放路径后面还有 `alignImportToGround` 兜底，自动化导入
        // （`MYVK_EDITOR_IMPORT`）没有，两条路就从这里分开。
        opt.dropToGround = true;

        scene::ModelInstanceResult inst =
            scene::instantiateModel(scene, model, opt);
        out.root = inst.root;
        out.entities = std::move(inst.entities);
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
