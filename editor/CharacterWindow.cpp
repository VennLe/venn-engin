#include "CharacterWindow.h"

#include "Localization.h"
#include "DebugRects.h"
#include "EditorDragDrop.h"
#include "EditorScene.h"
#include "PanelDock.h"
#include "PickingSystem.h"

#include "animation/AnimatorSystem.h"
#include "animation/Locomotion.h"
#include "ui/KeyCodes.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Model.h"
#include "assets/Skeleton.h"
#include "core/Logger.h"
#include "render/Renderer.h"
#include "scene/ModelInstance.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace editor {

namespace {

constexpr float kMinPreview = 64.0f;
// 预览导航手感（与 ViewportPanel 对齐）
constexpr float kLookPerPixel = 0.0025f;
constexpr float kDollyPerNotch = 0.6f;
constexpr float kPanPerPixel = 0.01f;

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool isFbxPath(const std::string& p) {
    return endsWith(p, ".fbx") || endsWith(p, ".FBX");
}

// 四段移动动画的**内建键位**显示名（只读徽标）。nullptr = 这一段没有内建控制。
//
// 为什么要有它：这四段由 `animation/Locomotion` 的状态机直接驱动 ——
//   Idle  停下就播（还带"静止 8 秒重放"）
//   Walk  水平速度 > walkSpeed
//   Run   水平速度 > runSpeed（鼠标右键长按 = 加速）
//   Jump  空格敲击后"离地"这个事实
// 面板上如果只有一个空的 [+] 键槽，用户会以为"还得先绑个键才能动"，
// 于是把 W 绑给 Walk —— 绑完反而得到"按 W 强播一段固定动画、人不动"。
// 这个徽标就是回答"那我到底要不要配"：不用。
const char* builtinKeyLabel(const std::string& clip) {
    static const animation::LocomotionConfig kDefault;
    if (clip == kDefault.walkClip) return "WASD";
    if (clip == kDefault.runClip) return "RMB";
    if (clip == kDefault.jumpClip) return "Space";
    if (clip == kDefault.idleClip) return "auto";
    return nullptr;
}

std::string pathStem(const std::string& p) {
    const std::string s = std::filesystem::path(p).stem().string();
    return s.empty() ? std::string("Model") : s;
}

// 从路径加载模型（按扩展名分派 FBX / glTF），失败返回 null。
//
// ⚠ `.vmesh` 必须走 `loadFBX`：那条路会**先探同名 .vmesh**，探不到再落
//   ufbx（见 AssetManager::loadFBX）。只按"是不是 .fbx"分流的话，
//   `alicia.vmesh` 会被送进 `loadGLTF` → tinygltf 解析二进制网格 →
//   `Fatal: Failed to load glTF` → **整个编辑器进程退出**（2026-10-24
//   实测：加个第三人称角色，编辑器直接没了）。只有 .glb/.gltf 才必须
//   走 GLTF，其它（.fbx/.FBX/.vmesh）一律 FBX 那条路。
assets::Model* loadModelByPath(assets::AssetManager& am,
                               const std::string& relPath) {
    const std::string resolved = assets::resolveAssetPath(relPath);
    const bool isGltf = endsWith(relPath, ".glb") || endsWith(relPath, ".gltf");
    if (isGltf) return am.loadGLTF(resolved, relPath);
    return am.loadFBX(resolved, relPath);
}

} // namespace

CharacterWindow::CharacterWindow(EditorContext& ctx, render::Renderer& renderer)
    : m_ctx(ctx), m_renderer(renderer) {}

void CharacterWindow::open(ecs::Entity player) {
    m_open = true;
    m_player = player;
    const std::string* nm =
        m_ctx.editorScene().world().name(player);
    m_playerName = nm ? *nm : std::string("Player");
    m_previewDirty = true;
    m_currentClip.clear();
    m_animTime = 0.0f;
    m_animPlaying = false;
    // 有骨骼就默认停在静止姿势，等用户选动画
    VK_LOG_INFO("CharacterWindow: opened for '%s'", m_playerName.c_str());

    // 自动化钩子：MYVK_CHARACTER_MODEL=<相对路径> → 打开即加载该模型
    // （免去真鼠标拖拽，让验证脚本可以直接测"预览 + 绑定 + 动画"链路）
    if (const char* env = std::getenv("MYVK_CHARACTER_MODEL")) {
        if (env[0] != '\0') {
            assets::Model* m = loadModelByPath(m_ctx.assets(), env);
            if (m && m->valid()) {
                m_model = m;
                m_modelPath = env;
                m_previewDirty = true;
                VK_LOG_INFO("CharacterWindow: auto-bound model '%s' "
                            "(submeshes=%zu skinned=%d clips=%zu)",
                            m->name.c_str(), m->subMeshes.size(),
                            m->skinned() ? 1 : 0,
                            m->skeleton ? m->skeleton->clips.size() : 0);
            } else {
                VK_LOG_WARN("CharacterWindow: auto-bind failed for '%s'", env);
            }
        }
    }

    // 没有显式指定 → 用玩家身上**已经绑好的**那个模型。
    //
    // 2026-10-22 起 `createPlayer` 默认就装配了角色（模型 + 骨骼 + 四段
    // 动画），面板打开时若不认它，用户看到的是一具空胶囊预览 + "no model"，
    // 得再拖一次同一个文件 —— 而"面板显示的角色"和"场上那个角色"本该是
    // 同一个东西。
    if (!m_model && m_player.valid()) {
        if (const auto* sk =
                m_ctx.editorScene().world().get<ecs::SkinnedMeshComponent>(
                    m_player)) {
            if (sk->model && sk->model->valid()) {
                m_model = sk->model;
                m_modelPath = sk->model->name;
                m_previewDirty = true;
                VK_LOG_INFO("CharacterWindow: adopted bound model '%s' "
                            "(joints=%d clips=%zu)",
                            m_modelPath.c_str(),
                            m_model->skeleton ? m_model->skeleton->jointCount() : 0,
                            m_model->skeleton ? m_model->skeleton->clips.size() : 0u);
            }
        }
    }

    // 还没有模型 → 预览里放**默认角色**（2026-10-24）。
    //
    // 语义变了：视口里不再摆角色（它在运行时才生成），所以"场上那个角色
    // 身上绑的模型"这条来源消失了。但面板仍然得给出一具有动画的身体 ——
    // 否则用户点开就是一片空白 + "no model"，只能自己去拖文件。
    //
    // ⚠ 只进**预览场景**（`m_previewScene`），一个实体都不写主场景。这条
    //   边界是这个面板现在唯一的安全约束：预览怎么折腾都不会让角色重新
    //   出现在视口里。
    if (!m_model) {
        assets::Model* m = loadModelByPath(m_ctx.assets(), kDefaultCharacterAsset);
        if (m && m->valid()) {
            m_model = m;
            m_modelPath = kDefaultCharacterAsset;
            m_previewDirty = true;
            VK_LOG_INFO("CharacterWindow: preview-only default character '%s' "
                        "(nothing is added to the viewport; the real character "
                        "is spawned at GameStart on Run)",
                        kDefaultCharacterAsset);
        }
    }
}

void CharacterWindow::close() {
    m_open = false;
    // 关掉预览渲染，让 Renderer 不再画预览场景
    m_renderer.setPreviewScene(nullptr);
    m_renderer.setPreviewSize(0, 0);
    m_previewScene.clear();
    m_previewRoot = ecs::Entity{};
    m_model = nullptr;
    m_modelPath.clear();
}

// ---------------------------------------------------------------- 预览场景

// 把 Model 实例化进目标场景的父节点下。
//
// 实现已搬到 `engine/scene/ModelInstance`（A0-1）：主场景的导入路径
// （Content 拖拽）以前走的是另一份实现、不判 skinned，导致带骨骼的 FBX
// 拖进来只是一堆静态网格。现在两条入口共用同一份，这里只剩一层转发。
//
// autoPlay 的语义：
//   * 预览场景（m_previewScene）→ false：姿势由底部时间轴滑条驱动，
//     抢过来推进会和滑条打架。
//   * **主场景的玩家实体**（bindModelToPlayer / generateSkeletonBinding）
//     → true：绑上去就该动。以前这里一律 false，于是"绑定了角色但站在原地
//     不动"——那不是 bug 的错觉，是真的没人在推进 time。
std::vector<ecs::Entity> CharacterWindow::instanceModelInto(scene::Scene& scene,
                                                            assets::Model* model,
                                                            ecs::Entity parent,
                                                            bool autoPlay) {
    scene::ModelInstanceOptions opt;
    opt.parent = parent;
    opt.subNameSuffix = "";        // 预览树里保持与改动前一致的名字
    opt.autoPlay = autoPlay;
    scene::ModelInstanceResult r = scene::instantiateModel(scene, model, opt);
    return std::move(r.entities);
}

void CharacterWindow::computeModelBounds() {
    m_boundsValid = false;
    m_boundsMin = glm::vec3(0.0f);
    m_boundsMax = glm::vec3(0.0f);
    if (!m_model || !m_model->valid()) return;

    bool first = true;
    for (const auto& sub : m_model->subMeshes) {
        if (!sub.mesh || !sub.mesh->hasBounds()) continue;
        // 8 个角点各乘一次 SubMesh 变换，取 min/max —— 比"变换 8 个包围盒角
        // 再求 AABB"稍多几行，但对带旋转的部件不会算出偏小的盒子。
        const glm::vec3 a = sub.mesh->boundsMin();
        const glm::vec3 b = sub.mesh->boundsMax();
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 c((i & 1) ? b.x : a.x, (i & 2) ? b.y : a.y,
                              (i & 4) ? b.z : a.z);
            const glm::vec3 w = glm::vec3(sub.transform * glm::vec4(c, 1.0f));
            if (first) {
                m_boundsMin = m_boundsMax = w;
                first = false;
            } else {
                m_boundsMin = glm::min(m_boundsMin, w);
                m_boundsMax = glm::max(m_boundsMax, w);
            }
        }
    }
    m_boundsValid = !first;
}

void CharacterWindow::rebuildPreviewScene() {
    m_previewScene.clear();
    m_previewRoot = ecs::Entity{};
    if (!m_model || !m_model->valid()) {
        m_previewDirty = false;
        return;
    }

    // 预览场景：一盏方向光（柔和，环绕照亮角色）+ 模型 + 独立相机
    //
    // ⚠ 参数意义（2026-10-17 实测澄清，别再照旧注释猜）：
    //   之前一直有"预览是一张白剪影"的现象，先后把光强从 1.8 调到 1.1 再调到
    //   0.65、把环境从 0.6 调到 0.25 再调到 0.12，**画面一点没变** ——
    //   因为根源不在光：FbxLoader 把 FBX 的 `EmissiveColor`（恒为 1,1,1）
    //   当成了自发光，丢掉了真正起作用的 `EmissiveFactor`，于是模型整具
    //   纯白自发光，任何光照都盖不过它。见 FbxLoader.cpp 里 fbxMaterial 的
    //   注释。修好之后这盏灯才真正生效（用红光/归零都能验证出来了）。
    //
    //   现在这组数值是对着"白 albedo 的无贴图模型"调的：受光面落在 ACES
    //   的中高段、背光面靠 ambientScale 兜住不死黑，读起来像 UE Persona
    //   里的默认 clay。模型的包围盒与取景见 computeModelBounds /
    //   previewDistanceFor。
    scene::Light& L = m_previewScene.light();
    L.direction = glm::normalize(glm::vec3(-0.4f, -0.5f, -0.8f));
    L.color = glm::vec3(1.0f, 0.97f, 0.92f);
    L.intensity = 1.2f;
    L.castsShadow = true;
    L.ambientScale = 0.35f;

    m_previewRoot = m_previewScene.createObject("PreviewRoot");
    // 根放在原点；模型自己的 submesh 变换会相对它摆
    instanceModelInto(m_previewScene, m_model, m_previewRoot,
                      /*autoPlay=*/false);

    // 诊断：模型的包围球与每个 submesh 的世界包围 —— 排查"预览视口看不到
    // 模型"这类取景/尺度问题（FBX 的单位常是 cm，转成米后模型可能极小或
    // 巨大，日志一打便知）。
    VK_LOG_INFO("CharacterWindow: model bounds center=(%.3f,%.3f,%.3f) "
                "radius=%.3f",
                m_model->center.x, m_model->center.y, m_model->center.z,
                m_model->radius);
    for (size_t i = 0; i < m_model->subMeshes.size(); ++i) {
        const auto& sub = m_model->subMeshes[i];
        const glm::vec3 p(sub.transform[3]);
        if (sub.mesh && sub.mesh->hasBounds()) {
            const glm::vec3 bmin = sub.mesh->boundsMin();
            const glm::vec3 bmax = sub.mesh->boundsMax();
            VK_LOG_INFO("CharacterWindow:   sub[%zu] '%s' "
                        "bounds=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f) "
                        "transform.t=(%.3f,%.3f,%.3f)",
                        i, sub.name.c_str(),
                        bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z,
                        p.x, p.y, p.z);
        } else {
            VK_LOG_INFO("CharacterWindow:   sub[%zu] '%s' (no bounds) "
                        "transform.t=(%.3f,%.3f,%.3f)",
                        i, sub.name.c_str(), p.x, p.y, p.z);
        }
    }

    // 预览相机：按**实测包围盒**取景（原因见 m_boundsMin 的注释）
    computeModelBounds();
    const bool hasBox = m_boundsValid;
    const glm::vec3 boxCenter =
        hasBox ? (m_boundsMin + m_boundsMax) * 0.5f : glm::vec3(0.0f, 0.0f, 0.9f);
    const glm::vec3 boxSize =
        hasBox ? (m_boundsMax - m_boundsMin) : glm::vec3(1.0f);

    // ---- 把预览根抬到"脚踩地面"（2026-10-22）----
    //
    // 模型的 pivot 在**几何中心**，脚在 m_boundsMin.z（默认角色是 −0.902）。
    // 根留在原点的话，预览那层栅格地面会从大腿上穿过去 —— 画面上就是
    // "人半身埋在地里"，和需求 2（脚不能陷到地形下面）是同一个观感 bug，
    // 只不过这里的地面是栅格。抬升之后：脚底 = z 0 = 栅格平面。
    const float footLift = hasBox ? -m_boundsMin.z : 0.0f;
    if (hasBox) {
        if (auto* rt = m_previewScene.world().get<ecs::TransformComponent>(
                m_previewRoot)) {
            rt->position.z = footLift;
        }
    }
    m_previewTarget = boxCenter + glm::vec3(0.0f, 0.0f, footLift);
    m_previewDistance = previewDistanceFor(boxSize);

    // ---- 预览胶囊的初始尺寸（2026-10-22）----
    // 按模型身高套同一组解剖比例（与 bindDefaultCharacter / RuntimeApp 的
    // attachPlayerModel 同源），于是**一打开就是"套住人"的状态** ——
    // 用户要调的是"差多少"，不是"从哪个数开始猜"。
    // 脚底对齐模型的包围盒底：模型的 pivot 在几何中心，预览根已经抬到
    // 脚底踩 z=0，所以胶囊也从 z=0 起（见上面 footLift 那段）。
    if (hasBox) {
        const float h = std::max(boxSize.z, 0.2f);
        m_capsuleRadius = glm::clamp(h * 0.16f, 0.15f, 0.60f);
        m_capsuleHalfHeight = std::max(0.10f, h * 0.5f - m_capsuleRadius);
        m_capsulePos = glm::vec3(0.0f, 0.0f, 0.0f);
    }
    VK_LOG_INFO("CharacterWindow: bounds=[%.3f,%.3f,%.3f]-[%.3f,%.3f,%.3f] "
                "center=(%.3f,%.3f,%.3f) distance=%.3f",
                m_boundsMin.x, m_boundsMin.y, m_boundsMin.z,
                m_boundsMax.x, m_boundsMax.y, m_boundsMax.z,
                boxCenter.x, boxCenter.y, boxCenter.z,
                static_cast<double>(m_previewDistance));
    scene::Camera& cam = m_previewScene.camera();
    cam.setYaw(m_previewYaw);
    cam.setPitch(m_previewPitch);
    cam.setDistance(m_previewDistance);
    cam.setTarget(m_previewTarget);
    cam.setFov(m_previewFov);

    m_previewDirty = false;
    VK_LOG_INFO("CharacterWindow: preview scene rebuilt (model=%s, %zu "
                "submeshes, skinned=%d)",
                m_model->name.c_str(), m_model->subMeshes.size(),
                m_model->skinned() ? 1 : 0);
    // 叠加层的开关状态 —— 自动化要能断言"栅格/胶囊真的开着"，而不是靠
    // 一张截图里数绿色像素（那在不同配色主题下会漂）。
    VK_LOG_INFO("CharacterWindow: preview overlay grid=%d capsule=%d gizmo=%d "
                "capsuleR=%.3f capsuleHH=%.3f",
                m_renderer.previewGridEnabled() ? 1 : 0, m_showCapsule ? 1 : 0,
                m_showGizmo ? 1 : 0,
                static_cast<double>(m_capsuleRadius),
                static_cast<double>(m_capsuleHalfHeight));
}

// ---------------------------------------------------------------- 模型加载
// （loadModelByPath 在文件顶部的匿名命名空间里，供 open() 与拖拽共用）

// ---------------------------------------------------------------- 小工具

namespace {

// 悬浮说明。**面板上不写说明文字**（2026-10-17 用户要求：面板上只留功能，
// 解释一律放进 tooltip）。这个 helper 统一了"什么时候弹"：
// 延迟一点点，鼠标匆匆划过时不弹 —— 否则整屏都是浮窗。
void tip(const char* text) {
    if (text && text[0] && ImGui::IsItemHovered(
            ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

// 键码换算统一在 `engine/ui/KeyCodes`（ImGui 捕获 / GLFW 码 / 显示名
// 三张面孔只有那一张表）。这里不再自带一份 —— 自带一份的后果是"面板上
// 显示 F、实际绑的是别的码"这类只在真机上手才发现的问题。

} // namespace

// ---------------------------------------------------------------- 骨骼查询

bool CharacterWindow::jointWorldPosition(int joint, glm::vec3& out) const {
    if (!m_model || !m_model->skeleton) return false;
    const assets::Skeleton& skel = *m_model->skeleton;
    if (joint < 0 || joint >= skel.jointCount()) return false;
    if (joint >= static_cast<int>(m_jointMats.size())) return false;

    // jointMat = invMeshNode * world * invBind  →  world = invMeshNode⁻¹ *
    //            jointMat * invBind⁻¹
    // 位置取 world 的平移列。这是**模型空间**（不含实体的世界变换），
    // 正好是预览视口里看到的位置，拿来做"骨头在哪"的读数最直观。
    const glm::mat4 w = glm::inverse(skel.invMeshNode) * m_jointMats[
                            static_cast<size_t>(joint)] *
                        glm::inverse(skel.joints[static_cast<size_t>(joint)]
                                         .invBind);
    out = glm::vec3(w[3]);
    return true;
}

// ---------------------------------------------------------------- 预览导航

void CharacterWindow::handlePreviewNavigation(const glm::vec2& vpPos,
                                              const glm::vec2& vpSize,
                                              bool hovered) {
    (void)vpPos;
    (void)vpSize;
    if (!hovered) return;
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 d(io.MouseDelta.x, io.MouseDelta.y);
    const bool lookDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    const bool panDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    // 右键旋转（轨道式：绕 m_previewTarget）
    if (lookDown) {
        m_previewYaw -= d.x * kLookPerPixel;
        m_previewPitch += d.y * kLookPerPixel;
        constexpr float kPitchLimit = 1.5533f;  // 89°
        m_previewPitch = glm::clamp(m_previewPitch, -kPitchLimit, kPitchLimit);
    }
    // 中键平移注视点（沿相机右/上方向）
    if (panDown && (d.x != 0.0f || d.y != 0.0f)) {
        scene::Camera& cam = m_previewScene.camera();
        const glm::vec3 r = cam.rightAxis();
        const glm::vec3 u = glm::cross(r, cam.forwardAxis());
        m_previewTarget += (r * -d.x + u * d.y) * kPanPerPixel;
    }
    // 滚轮推拉
    if (io.MouseWheel != 0.0f && !lookDown) {
        m_previewDistance =
            std::max(0.2f, m_previewDistance - io.MouseWheel * kDollyPerNotch);
    }

    // 把导航状态同步给预览相机
    scene::Camera& cam = m_previewScene.camera();
    cam.setYaw(m_previewYaw);
    cam.setPitch(m_previewPitch);
    cam.setDistance(m_previewDistance);
    cam.setTarget(m_previewTarget);
}

void CharacterWindow::framePreviewCamera() {
    if (!m_model) return;
    if (!m_boundsValid) computeModelBounds();
    glm::vec3 center = m_boundsValid ? (m_boundsMin + m_boundsMax) * 0.5f
                                     : glm::vec3(0.0f, 0.0f, 0.9f);
    const glm::vec3 size =
        m_boundsValid ? (m_boundsMax - m_boundsMin) : glm::vec3(1.0f);
    // 预览根被抬到了"脚底踩 z=0"（见 rebuildPreviewScene），取景也要跟着抬
    center.z += m_boundsValid ? -m_boundsMin.z : 0.0f;
    m_previewTarget = center;
    m_previewDistance = previewDistanceFor(size);
    scene::Camera& cam = m_previewScene.camera();
    cam.setTarget(m_previewTarget);
    cam.setDistance(m_previewDistance);
}

// 让"整个包围盒"刚好装进视锥所需的距离（再留 25% 边距）。
//
// ⚠ 不能用包围球直径 × 常数：那样**竖长的人形**会被裁头（实测 1.56 m 高的
//   角色在 1.8 m 处，45° 垂直 FOV 只装得下 1.51 m）。这里按"垂直 FOV 装下
//   高度、水平 FOV 装下最宽的水平尺寸"各算一次，取大的那个；横向按 4:3 的
//   保守宽高比估（预览视口通常比这更宽，估窄一点只会多留白，不会裁切）。
float CharacterWindow::previewDistanceFor(const glm::vec3& size) const {
    const float halfZ = std::max(size.z * 0.5f, 0.05f);
    const float halfX = std::max(std::max(size.x, size.y) * 0.5f, 0.05f);
    const float tanV = std::tan(glm::radians(m_previewFov) * 0.5f);
    if (tanV < 1.0e-4f) return 2.0f;
    const float tanH = tanV * (4.0f / 3.0f);
    const float dV = halfZ / tanV;
    const float dH = halfX / tanH;
    return std::max(std::max(dV, dH) * 1.25f, 0.6f);
}

// ---------------------------------------------------------------- 预览视口

void CharacterWindow::drawPreviewViewport() {
    // ---- 底部控件条先占位 ----
    // 图片高度决定了**离屏目标的尺寸**，所以必须"先减掉控件条、再请求
    // 尺寸"：反过来（画完再减）会让目标每帧重建一次，HDR 链重建是
    // waitIdle 级别的操作，窗口会明显卡。
    const float rowH = ImGui::GetFrameHeightWithSpacing();
    const float stripH = rowH * 4.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const uint32_t wantW = static_cast<uint32_t>(std::max(avail.x, kMinPreview));
    const uint32_t wantH = static_cast<uint32_t>(
        std::max(avail.y - stripH, kMinPreview));

    // 只在**真的变了**的时候请求重建。setPreviewSize 内部会去抖，但这里
    // 还多一层"非退化"保护：窗口被拖到极窄时 ImGui 会给 0 甚至负数，
    // 直接丢给渲染器会得到 1×1 的目标（重建风暴）。
    if (wantW != m_reqW || wantH != m_reqH) {
        m_reqW = wantW;
        m_reqH = wantH;
        m_renderer.setPreviewSize(wantW, wantH);
    }

    // 把预览场景交给 Renderer 渲染
    m_renderer.setPreviewScene(&m_previewScene);

    const uint64_t texId = m_renderer.previewTextureId();
    const ImVec2 imageSize(static_cast<float>(m_reqW),
                           static_cast<float>(m_reqH));

    if (texId != 0) {
        ImGui::Image(static_cast<ImTextureID>(texId), imageSize, ImVec2(0, 0),
                     ImVec2(1, 1));
    } else {
        ImGui::Dummy(imageSize);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                          IM_COL32(16, 18, 22, 255));
        dl->AddText(ImVec2(ImGui::GetItemRectMin().x + 10.0f,
                           ImGui::GetItemRectMin().y + 10.0f),
                    IM_COL32(140, 148, 158, 220), TR("building preview..."));
    }

    const ImVec2 imgMin = ImGui::GetItemRectMin();
    const ImVec2 imgSz = ImGui::GetItemRectSize();
    m_vpMin = imgMin;
    m_vpSize = imgSz;
    const bool hovered = ImGui::IsItemHovered();

    // ---- 胶囊投影（点选与日志共用一份几何）----
    //
    // m_capsulePos 是**脚底**，胶囊中轴的两端是两个球心：底 (r)、顶 (2hh+r)。
    // 半径的像素值取"底球心 → 底球心 + X*r"的屏幕距离 —— 透视下它在远处会
    // 变小，所以不能写死一个像素常数。
    const scene::Camera& pcam = m_previewScene.camera();
    const glm::mat4 pvp =
        PickingSystem::viewProj(pcam, glm::vec2(imgSz.x, imgSz.y));
    m_capScrValid = false;
    if (m_showCapsule && m_capsuleRadius > 1.0e-4f && imgSz.x > 4.0f &&
        imgSz.y > 4.0f) {
        const glm::vec3 botC = m_capsulePos + glm::vec3(0.0f, 0.0f,
                                                        m_capsuleRadius);
        const glm::vec3 topC = m_capsulePos +
                               glm::vec3(0.0f, 0.0f,
                                         2.0f * m_capsuleHalfHeight +
                                             m_capsuleRadius);
        glm::vec2 sa, sb, se;
        if (PickingSystem::worldToScreen(pvp, glm::vec2(imgMin.x, imgMin.y),
                                         glm::vec2(imgSz.x, imgSz.y), botC, sa) &&
            PickingSystem::worldToScreen(pvp, glm::vec2(imgMin.x, imgMin.y),
                                         glm::vec2(imgSz.x, imgSz.y), topC, sb) &&
            PickingSystem::worldToScreen(pvp, glm::vec2(imgMin.x, imgMin.y),
                                         glm::vec2(imgSz.x, imgSz.y),
                                         botC + glm::vec3(m_capsuleRadius, 0, 0),
                                         se)) {
            const float rx = se.x - sa.x;
            const float ry = se.y - sa.y;
            m_capScrR = std::sqrt(rx * rx + ry * ry);
            m_capScrA = ImVec2(sa.x, sa.y);
            m_capScrB = ImVec2(sb.x, sb.y);
            m_capScrOff = ImVec2(rx, ry);
            m_capScrValid = m_capScrR > 0.5f;
        }
    }
    // 只在真的变了的时候打一行：相机一推拉这组数就变，逐帧打会刷爆日志
    if (m_capScrValid) {
        static ImVec2 la(-1.0e9f, -1.0e9f), lb(-1.0e9f, -1.0e9f);
        static float lr = -1.0f;
        constexpr float kEps = 1.5f;
        if (std::fabs(la.x - m_capScrA.x) > kEps ||
            std::fabs(la.y - m_capScrA.y) > kEps ||
            std::fabs(lb.x - m_capScrB.x) > kEps ||
            std::fabs(lb.y - m_capScrB.y) > kEps ||
            std::fabs(lr - m_capScrR) > kEps) {
            la = m_capScrA;
            lb = m_capScrB;
            lr = m_capScrR;
            VK_LOG_INFO("CHAR-PREVIEW-CAPSULE-SCREEN a=(%.0f,%.0f) "
                        "b=(%.0f,%.0f) r=%.1f off=(%.0f,%.0f)",
                        m_capScrA.x, m_capScrA.y, m_capScrB.x, m_capScrB.y,
                        static_cast<double>(m_capScrR), m_capScrOff.x,
                        m_capScrOff.y);
        }
    }

    // 叠加层：栅格以外的 3D 参照（胶囊线框 / 坐标轴 / 选中框）
    //
    // **必须排在点选之前**：胶囊线框的屏幕线段是这里画的时候记下来的
    // （m_capScrSegs），点选量的是"眼睛里看到的那几根线"。反过来的话
    // 量的是上一帧的几何，绕着模型转视角时点选会偏一帧。
    drawPreviewOverlay(imgMin, imgSz);

    // ---- 点选（选中模型 / 胶囊）----
    // 只在**左键单击**（按下那一帧）时改选中：拖拽是导航（右键/中键），
    // 两者不冲突。判定分三层，顺序就是"心里想点的那个东西"的优先级：
    //
    //   ① 鼠标离胶囊线框 ≤ 9 px → 胶囊。线框看得见，点它选中它是最不需要
    //      解释的动作。
    //   ② 否则射线命中模型包围盒 → 模型。
    //   ③ 否则射线命中胶囊包围盒 → 胶囊（兜底：线框细，点到里面也算）。
    //
    // 不用"射线-AABB 取更近"：胶囊比人粗，正面看永远先撞胶囊，模型就
    // 点不中了（实测就是：点身体选到胶囊、点脚底选到模型，正好反了）。
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        m_previewRoot.valid()) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const Ray ray = PickingSystem::rayFromViewport(
            m_previewScene.camera(), glm::vec2(imgMin.x, imgMin.y),
            glm::vec2(imgSz.x, imgSz.y), glm::vec2(mp.x, mp.y));

        auto hitAabb = [](const Ray& r, const glm::vec3& lo,
                          const glm::vec3& hi, float* tOut) {
            // 标准 slab 法（射线方向分量为 0 的轴单独处理，否则 0/0 = NaN）
            float t0 = -1.0e30f, t1 = 1.0e30f;
            for (int i = 0; i < 3; ++i) {
                if (std::fabs(r.dir[i]) < 1.0e-8f) {
                    if (r.origin[i] < lo[i] || r.origin[i] > hi[i]) return false;
                    continue;
                }
                float a = (lo[i] - r.origin[i]) / r.dir[i];
                float b = (hi[i] - r.origin[i]) / r.dir[i];
                if (a > b) std::swap(a, b);
                t0 = std::max(t0, a);
                t1 = std::min(t1, b);
                if (t0 > t1) return false;
            }
            if (t1 < 0.0f) return false;
            *tOut = t0 > 0.0f ? t0 : t1;
            return true;
        };

        // 点到线段的距离（屏幕空间）
        auto segDist = [](const glm::vec2& p, const glm::vec2& a,
                          const glm::vec2& b) {
            const glm::vec2 ab = b - a;
            const float len2 = glm::dot(ab, ab);
            float u = len2 > 1.0e-6f ? glm::dot(p - a, ab) / len2 : 0.0f;
            u = glm::clamp(u, 0.0f, 1.0f);
            return glm::length(p - (a + ab * u));
        };

        PreviewPick best = PreviewPick::None;

        // ① 线框（量 overlay 记下来的真实线段，不是"中轴 ± r"）
        if (!m_capScrSegs.empty()) {
            const glm::vec2 p(mp.x, mp.y);
            float bestD = 1.0e30f;
            for (std::size_t i = 0; i + 1 < m_capScrSegs.size(); i += 2) {
                const glm::vec2 a(m_capScrSegs[i].x, m_capScrSegs[i].y);
                const glm::vec2 b(m_capScrSegs[i + 1].x, m_capScrSegs[i + 1].y);
                bestD = std::min(bestD, segDist(p, a, b));
            }
            if (bestD <= 9.0f) best = PreviewPick::Capsule;
        }

        // ② 模型包围盒（computeModelBounds 已经把各 SubMesh 的变换算进去了）
        if (best == PreviewPick::None && m_boundsValid) {
            float t = 0.0f;
            const auto* rt = m_previewScene.world().get<ecs::TransformComponent>(
                m_previewRoot);
            const glm::vec3 off = rt ? rt->position : glm::vec3(0.0f);
            if (hitAabb(ray, m_boundsMin + off, m_boundsMax + off, &t))
                best = PreviewPick::Model;
        }

        // ③ 胶囊包围盒（**带 m_capsulePos**：拿它当原点算过一版，胶囊整体
        //    高了 0.9 m，点脚底能中、点胸口点不中，查了很久）
        if (best == PreviewPick::None && m_showCapsule) {
            const glm::vec3 c = m_capsulePos +
                                glm::vec3(0.0f, 0.0f, m_capsuleHalfHeight +
                                                        m_capsuleRadius);
            const glm::vec3 ext(m_capsuleRadius, m_capsuleRadius,
                                m_capsuleHalfHeight + m_capsuleRadius);
            float t = 0.0f;
            if (hitAabb(ray, c - ext, c + ext, &t)) best = PreviewPick::Capsule;
        }
        m_pick = best;
        VK_LOG_INFO("CHAR-PREVIEW-PICK target=%s",
                    best == PreviewPick::Model     ? "model"
                    : best == PreviewPick::Capsule ? "capsule"
                                                   : "none");
    }


    handlePreviewNavigation(glm::vec2(imgMin.x, imgMin.y),
                            glm::vec2(imgSz.x, imgSz.y), hovered);

    logRect("CHAR-PREVIEW", imgMin,
            ImVec2(imgMin.x + imgSz.x, imgMin.y + imgSz.y));

    // 导航提示改成**悬浮才出现**：视口里常驻一行小字会挡模型，也不符合
    // "面板上不写说明"的约定。
    tip(TR("LMB: body = model, outline = capsule.   RMB: orbit   wheel: zoom"));

    // 底部：选中对象的位置 / 旋转 / 大小
    drawPreviewTransformStrip();
}

// ---------------------------------------------------------------- 预览叠加层
//
// 三样东西画在**离屏图之上**的 ImGui draw list 里：
//   ① 胶囊线框（绿）—— 隔着模型也看得见，这是"胶囊套没套住人"的唯一判据；
//   ② 坐标轴 gizmo（红X 绿Y 蓝Z）—— 原点 + 选中对象的局部原点；
//   ③ 选中框（黄）—— 点选后在包围盒上描一圈，明确"我现在在动谁"。
//
// 为什么全部走投影而不是做成场景实体：见头文件里的注释。这里只补一句
// 实现层面的理由 —— **3D 线段投影之后仍然是直线**，所以两个端点各算一次
// 屏幕坐标再连线就是精确的，不需要逐像素光栅化，也不会和深度缓冲打架。
namespace {

// 画一个水平圆（圆心 + 半径，Z-up 平面）。透视下投影成椭圆，靠分段逼近。
void drawWorldCircle(ImDrawList* dl, const glm::vec3& center, float radius,
                     const std::function<bool(const glm::vec3&, ImVec2&)>& proj,
                     ImU32 col, float thickness, int seg = 28) {
    const float step = 6.28318530718f / static_cast<float>(seg);
    for (int i = 0; i < seg; ++i) {
        const float a0 = step * static_cast<float>(i);
        const float a1 = step * static_cast<float>(i + 1);
        const glm::vec3 p0(center.x + std::cos(a0) * radius,
                           center.y + std::sin(a0) * radius, center.z);
        const glm::vec3 p1(center.x + std::cos(a1) * radius,
                           center.y + std::sin(a1) * radius, center.z);
        ImVec2 s0, s1;
        if (proj(p0, s0) && proj(p1, s1)) dl->AddLine(s0, s1, col, thickness);
    }
}

// 画一段半圆弧：从 center 朝 `dir` 方向"拱起"，用于胶囊两端的球冠。
void drawWorldArc(ImDrawList* dl, const glm::vec3& center, const glm::vec3& dir,
                  const glm::vec3& bend, float radius,
                  const std::function<bool(const glm::vec3&, ImVec2&)>& proj,
                  ImU32 col, float thickness, int seg = 14) {
    const float step = 3.14159265f / static_cast<float>(seg);
    for (int i = 0; i < seg; ++i) {
        const float a0 = step * static_cast<float>(i);
        const float a1 = step * static_cast<float>(i + 1);
        const glm::vec3 p0 = center + dir * (radius * std::cos(a0)) +
                             bend * (radius * std::sin(a0));
        const glm::vec3 p1 = center + dir * (radius * std::cos(a1)) +
                             bend * (radius * std::sin(a1));
        ImVec2 s0, s1;
        if (proj(p0, s0) && proj(p1, s1)) dl->AddLine(s0, s1, col, thickness);
    }
}

} // namespace

void CharacterWindow::drawPreviewOverlay(const ImVec2& imgMin,
                                         const ImVec2& imgSz) {
    // 先清：早退的分支（没模型 / 视口太小）不该留下上一帧的线段给点选用
    m_capScrSegs.clear();
    if (!m_previewRoot.valid() || imgSz.x < 4.0f || imgSz.y < 4.0f) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // 裁到视口内：窗口被拖窄 / 模型出画时，线不能画到右栏的 Inspector 上
    dl->PushClipRect(imgMin, ImVec2(imgMin.x + imgSz.x, imgMin.y + imgSz.y),
                     true);

    const scene::Camera& cam = m_previewScene.camera();
    const glm::mat4 vp =
        PickingSystem::viewProj(cam, glm::vec2(imgSz.x, imgSz.y));
    const auto proj = [&](const glm::vec3& p, ImVec2& out) {
        glm::vec2 s;
        if (!PickingSystem::worldToScreen(vp, glm::vec2(imgMin.x, imgMin.y),
                                          glm::vec2(imgSz.x, imgSz.y), p, s))
            return false;
        out = ImVec2(s.x, s.y);
        return true;
    };

    // ---- ① 胶囊线框 ----
    if (m_showCapsule && m_capsuleRadius > 1.0e-4f) {
        const float r = m_capsuleRadius;
        const float hh = std::max(m_capsuleHalfHeight, 0.0f);
        const glm::vec3 base = m_capsulePos;
        const glm::vec3 botC = base + glm::vec3(0.0f, 0.0f, r);
        const glm::vec3 topC = base + glm::vec3(0.0f, 0.0f, 2.0f * hh + r);

        // 记录版投影：画线的每个端点都进 m_capScrSegs，点选直接量这些线段。
        // 顺序即成对顺序（每两个点之间就是一段线），见头文件那段注释。
        const auto recProj = [&](const glm::vec3& p, ImVec2& out) {
            if (!proj(p, out)) return false;
            m_capScrSegs.push_back(out);
            return true;
        };

        const bool sel = (m_pick == PreviewPick::Capsule);
        const ImU32 col = sel ? IM_COL32(255, 226, 96, 235)
                              : IM_COL32(120, 235, 150, 175);
        const float th = sel ? 2.2f : 1.4f;

        drawWorldCircle(dl, botC, r, recProj, col, th);
        drawWorldCircle(dl, topC, r, recProj, col, th);
        // 两条母线（X 方向的两个极端）—— 透视下正好是胶囊的左右轮廓
        ImVec2 a0, a1, b0, b1;
        if (recProj(botC + glm::vec3(r, 0, 0), a0) &&
            recProj(topC + glm::vec3(r, 0, 0), a1))
            dl->AddLine(a0, a1, col, th);
        if (recProj(botC - glm::vec3(r, 0, 0), b0) &&
            recProj(topC - glm::vec3(r, 0, 0), b1))
            dl->AddLine(b0, b1, col, th);

        // 两端球冠：XZ / YZ 各一条半圆（只画 2 条就足够读出"这是个胶囊"）
        const glm::vec3 up(0.0f, 0.0f, 1.0f);
        drawWorldArc(dl, topC, glm::vec3(1, 0, 0), up, r, recProj, col, th);
        drawWorldArc(dl, topC, glm::vec3(0, 1, 0), up, r, recProj, col, th);
        drawWorldArc(dl, botC, glm::vec3(1, 0, 0), -up, r, recProj, col, th);
        drawWorldArc(dl, botC, glm::vec3(0, 1, 0), -up, r, recProj, col, th);

        // 脚底中线：一眼看出"胶囊底 = 地面"（贴地问题的直接读数）
        ImVec2 f0, f1;
        if (proj(base, f0) && proj(base + glm::vec3(0.35f, 0, 0), f1))
            dl->AddLine(f0, f1, IM_COL32(120, 235, 150, 120), 1.0f);
        // 奇数个点说明中途有一段投影失败（点在相机后面），丢掉尾巴保配对
        if (m_capScrSegs.size() & 1u) m_capScrSegs.pop_back();
    }

    // ---- ② 坐标轴 gizmo ----
    if (m_showGizmo) {
        // 选中谁就画在谁的原点上：模型 → 模型根的世界位置 + 包围盒底；
        // 胶囊 → 胶囊脚底。都没选 → 世界原点。
        glm::vec3 origin(0.0f);
        const auto* rt =
            m_previewScene.world().get<ecs::TransformComponent>(m_previewRoot);
        if (m_pick == PreviewPick::Capsule) {
            origin = m_capsulePos;
        } else if (rt) {
            // 模型：画在包围盒底（= 脚底），不是模型根（pivot 在几何中心）
            origin = rt->position +
                     glm::vec3(0.0f, 0.0f, m_boundsValid ? m_boundsMin.z : 0.0f);
        }
        const float L = 0.35f;
        struct Ax {
            glm::vec3 d;
            ImU32 c;
            const char* name;
        };
        const Ax axes[3] = {{glm::vec3(1, 0, 0), IM_COL32(236, 96, 96, 255), "X"},
                            {glm::vec3(0, 1, 0), IM_COL32(110, 220, 110, 255), "Y"},
                            {glm::vec3(0, 0, 1), IM_COL32(110, 160, 255, 255), "Z"}};
        ImVec2 o;
        if (proj(origin, o)) {
            for (const Ax& ax : axes) {
                ImVec2 t;
                if (!proj(origin + ax.d * L, t)) continue;
                dl->AddLine(o, t, ax.c, 2.0f);
                // 末端方块：屏幕空间 5 px —— 世界大小会随距离缩，太小的
                // 轴头在缩远时等于没有
                dl->AddRectFilled(ImVec2(t.x - 3.0f, t.y - 3.0f),
                                  ImVec2(t.x + 3.0f, t.y + 3.0f), ax.c);
                dl->AddText(ImVec2(t.x + 5.0f, t.y - 7.0f), ax.c, ax.name);
            }
        }
    }

    // ---- ③ 选中框（模型）----
    if (m_pick == PreviewPick::Model && m_boundsValid) {
        const auto* rt =
            m_previewScene.world().get<ecs::TransformComponent>(m_previewRoot);
        const glm::vec3 off = rt ? rt->position : glm::vec3(0.0f);
        glm::vec3 lo = m_boundsMin + off;
        glm::vec3 hi = m_boundsMax + off;
        // 8 个角 + 12 条棱
        glm::vec3 c[8];
        for (int i = 0; i < 8; ++i)
            c[i] = glm::vec3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
                             (i & 4) ? hi.z : lo.z);
        static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},
                                          {0, 2}, {1, 3}, {4, 6}, {5, 7},
                                          {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        const ImU32 col = IM_COL32(255, 226, 96, 200);
        for (const auto& ed : kEdges) {
            ImVec2 s0, s1;
            if (proj(c[ed[0]], s0) && proj(c[ed[1]], s1))
                dl->AddLine(s0, s1, col, 1.6f);
        }
    }

    dl->PopClipRect();
}

// ---------------------------------------------------------------- 变换控件条

void CharacterWindow::drawPreviewTransformStrip() {
    if (!m_previewRoot.valid()) return;
    ecs::World& w = m_previewScene.world();
    auto* rt = w.get<ecs::TransformComponent>(m_previewRoot);
    if (!rt) return;

    // ---- 选中切换 ----
    // PushID：面板里别处也可能有名为 "Model" 的控件，同帧同窗口重名会让
    // ImGui 的 ID 撞车（表现是点一个另一个也变）。
    ImGui::PushID("##preview_pick");
    if (ImGui::RadioButton(TR("Model"), m_pick == PreviewPick::Model))
        m_pick = PreviewPick::Model;
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::RadioButton(TR("Capsule"), m_pick == PreviewPick::Capsule))
        m_pick = PreviewPick::Capsule;
    ImGui::PopID();
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::Checkbox(TR("Show capsule"), &m_showCapsule);
    tip(TR("The capsule is the collision proxy. If it does not wrap the "
           "character, the player is fatter / thinner than the model."));
    ImGui::SameLine();
    ImGui::Checkbox(TR("Gizmo"), &m_showGizmo);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", m_pick == PreviewPick::Capsule ? "capsule"
                            : m_pick == PreviewPick::Model ? "model"
                                                           : "nothing");

    const bool capsule = (m_pick == PreviewPick::Capsule);

    // ---- 位置 ----
    // 两个 DragFloat3 都作用在**各自的对象**上：模型改预览根实体的
    // Transform，胶囊改它自己的状态（它没有实体）。这样"人站在哪、胶囊
    // 套在哪"可以分别调 —— 正是"胶囊没包住人"时需要的那个自由度。
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Pos"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (capsule) {
        float p[3] = {m_capsulePos.x, m_capsulePos.y, m_capsulePos.z};
        if (ImGui::DragFloat3("##cap_pos", p, 0.01f, -10.0f, 10.0f, "%.3f"))
            m_capsulePos = glm::vec3(p[0], p[1], p[2]);
    } else {
        float p[3] = {rt->position.x, rt->position.y, rt->position.z};
        if (ImGui::DragFloat3("##mdl_pos", p, 0.01f, -10.0f, 10.0f, "%.3f"))
            rt->position = glm::vec3(p[0], p[1], p[2]);
    }
    tip(TR("Preview-only offset (does not touch the player entity)"));

    // ---- 旋转（绕 Z：人形的朝向只有这一个自由度）----
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Yaw"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (capsule) {
        // 胶囊绕自身轴旋转不可见，但仍然给一个数（对齐口径、以后加盒子用）
        ImGui::DragFloat("##cap_yaw", &m_capsuleYawDeg, 0.5f, -180.0f, 180.0f,
                         TR("%.1f deg"));
    } else {
        float deg = glm::degrees(rt->rotation.z);
        if (ImGui::DragFloat("##mdl_yaw", &deg, 0.5f, -180.0f, 180.0f,
                             TR("%.1f deg")))
            rt->rotation.z = glm::radians(deg);
    }

    // ---- 大小 ----
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Size"));
    ImGui::SameLine();
    if (capsule) {
        // 用 "半径 / 半高" 两个数（就是 CharacterControllerComponent 的字面字段），
        // 不用"总高"—— 因为写回玩家时也是这两个字段，绕一层反而容易对不上。
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f - 4.0f);
        ImGui::DragFloat(TR("R"), &m_capsuleRadius, 0.005f, 0.05f, 1.0f, "%.3f");
        tip(TR("Capsule radius (metres)"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat(TR("HH"), &m_capsuleHalfHeight, 0.005f, 0.05f, 2.0f,
                         "%.3f");
        tip(TR("Half height of the cylindrical part; total height = "
               "2 × (half height + radius)"));
    } else {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("##mdl_scale", &rt->scale.x, 0.005f, 0.05f, 5.0f,
                             "%.3f"))
            rt->scale = glm::vec3(rt->scale.x);
        tip(TR("Uniform scale of the preview model"));
        // 模型根的 scale 只影响预览里的观感，不写回玩家（绑定时按包围盒重算）
    }
}

void CharacterWindow::drawToolbar() {
    // Viewport 工具条（UE 的视口左上角那排）。只放**真的接线了**的开关：
    // 着色模式 / 骨骼叠加之类没实现的，宁可没有 —— "点了没反应"比"没有"
    // 更糟（这是本项目的老纪律）。
    if (ImGui::Button(TR("Fit View"))) framePreviewCamera();
    tip(TR("Frame the whole model (same as F in UE's viewport)"));
    logRect("CHAR-FIT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::SliderFloat("##preview_fov", &m_previewFov, 20.0f, 100.0f,
                           TR("FOV %.0f"))) {
        m_previewScene.camera().setFov(m_previewFov);
    }
    tip(TR("Preview camera field of view"));

    // 预览栅格：走渲染器自己的"预览栅格"参数（与主视口那份互不影响，
    // 见 Renderer::previewGridSettings 的注释）。
    ImGui::SameLine();
    ImGui::Checkbox(TR("Grid"), &m_renderer.previewGridEnabled());
    tip(TR("Ground grid in the preview viewport (independent of the "
           "main viewport's grid)"));

    // 模型状态徽标（右端）
    ImGui::SameLine();
    const char* badge = !m_model ? TR("no model")
                        : m_model->skinned() ? TR("SKINNED")
                                             : TR("STATIC");
    const ImVec4 col = m_model && m_model->skinned()
                           ? ImVec4(0.55f, 0.82f, 0.55f, 1.0f)
                           : ImVec4(0.72f, 0.74f, 0.78f, 1.0f);
    ImGui::TextColored(col, "%s", badge);
    tip(TR("SKINNED = the model carries a skeleton, so it can play clips"));
}

// ---------------------------------------------------------------- 左栏

void CharacterWindow::drawSkeletonPanel() {
    if (!m_model || !m_model->skeleton) {
        ImGui::TextDisabled(TR("(no skeleton)"));
        tip(TR("This model has no skeleton. Drop a rigged .fbx / .glb in the "
               "Mesh tab to load bones."));
        return;
    }
    const assets::Skeleton& skel = *m_model->skeleton;
    const int n = skel.jointCount();
    if (n <= 0) {
        ImGui::TextDisabled(TR("(empty skeleton)"));
        return;
    }

    if (m_jointExpanded.size() != static_cast<size_t>(n))
        m_jointExpanded.assign(static_cast<size_t>(n), 0);  // 默认折叠

    // 骨骼搜索：大量骨架上逐个找某根骨头非常痛苦（UE 的 Skeleton Tree 也有）
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##bonesearch", TR("search bones..."),
                             m_boneSearch, sizeof(m_boneSearch));
    const std::string filter(m_boneSearch);
    const bool filtering = !filter.empty();

    // 递归绘制骨骼树。用简单的前向遍历：先收集每个 joint 的子节点。
    std::vector<std::vector<int>> children(static_cast<size_t>(n));
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        const int p = skel.joints[static_cast<size_t>(i)].parent;
        if (p < 0) roots.push_back(i);
        else if (p < n)
            children[static_cast<size_t>(p)].push_back(i);
    }

    // 过滤时：命中的关节以 0 缩进平铺（不画树），一眼看到全部匹配项。
    if (filtering) {
        std::string low;
        low.reserve(filter.size());
        for (char c : filter) low.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
        int hits = 0;
        for (int i = 0; i < n; ++i) {
            std::string nm = skel.joints[static_cast<size_t>(i)].name;
            for (char& c : nm)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (nm.find(low) == std::string::npos) continue;
            ++hits;
            ImGui::PushID(i);
            const bool sel = (m_selectedJoint == i);
            if (ImGui::Selectable(skel.joints[static_cast<size_t>(i)].name.c_str(),
                                  sel))
                m_selectedJoint = i;
            ImGui::PopID();
        }
        if (hits == 0) ImGui::TextDisabled(TR("(no match)"));
        return;
    }

    std::function<void(int, int)> drawJoint = [&](int idx, int depth) {
        const auto& j = skel.joints[static_cast<size_t>(idx)];
        const bool hasKids = !children[static_cast<size_t>(idx)].empty();
        ImGui::PushID(idx);
        ImGui::Indent(static_cast<float>(depth) * 12.0f);
        const char* label = j.name.empty() ? "(unnamed)" : j.name.c_str();
        if (hasKids) {
            if (ImGui::ArrowButton("##exp",
                                   m_jointExpanded[static_cast<size_t>(idx)]
                                       ? ImGuiDir_Down
                                       : ImGuiDir_Right)) {
                m_jointExpanded[static_cast<size_t>(idx)] =
                    !m_jointExpanded[static_cast<size_t>(idx)];
            }
            ImGui::SameLine();
        }
        // 骨骼本身可选：选中之后右栏 Details 会显示它的静止/动画世界位置
        if (ImGui::Selectable(label, m_selectedJoint == idx)) m_selectedJoint = idx;
        tip(TR("Select to inspect this bone in the Details panel"));
        ImGui::Unindent(static_cast<float>(depth) * 12.0f);
        ImGui::PopID();

        // 折叠状态下也允许展开（默认全折叠，大骨架一屏塞不下）
        if (m_jointExpanded[static_cast<size_t>(idx)]) {
            for (int c : children[static_cast<size_t>(idx)])
                drawJoint(c, depth + 1);
        }
    };

    ImGui::TextDisabled(TR("%d joints"), n);
    tip(TR("Hierarchy of the skeleton bound to this model"));
    for (int r : roots) drawJoint(r, 0);
}

void CharacterWindow::drawMeshPanel() {
    ImGui::Text(TR("Model: %s"), m_model ? m_model->name.c_str() : "(none)");
    if (m_model) {
        ImGui::TextDisabled(TR("%zu submeshes  ·  %s"), m_model->subMeshes.size(),
                            m_model->skinned() ? "SKINNED" : "STATIC");
        tip(TR("Number of drawable parts and whether the model is skinned"));
        if (m_model->skeleton) {
            ImGui::TextDisabled(TR("%zu bones  ·  %zu clips"),
                                m_model->skeleton->joints.size(),
                                m_model->skeleton->clips.size());
            tip(TR("Skeleton size and how many animation clips the file carries"));
        }
    }

    ImGui::Separator();

    // 拖入模型文件来绑定（说明文字不常驻，只在悬浮时出现）
    const ImVec2 dropMin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 48.0f));
    const ImVec2 dropMax = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(
        dropMin, dropMax, ImGui::GetColorU32(ImGuiCol_Border, 0.9f), 3.0f);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(dropMin.x + 10.0f, dropMin.y + 16.0f),
        ImGui::GetColorU32(ImGuiCol_TextDisabled), TR("Drop .fbx / .glb here"));
    tip(TR("Drop a .fbx / .glb / .gltf model here to bind it"));
    logRect("CHAR-DROP", dropMin, dropMax);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(drag::kAsset)) {
            const char* path = static_cast<const char*>(p->Data);
            if (path) {
                assets::Model* m =
                    loadModelByPath(m_ctx.assets(), std::string(path));
                if (m && m->valid()) {
                    m_model = m;
                    m_modelPath = path;
                    m_previewDirty = true;
                    m_currentClip.clear();
                    m_animTime = 0.0f;
                    m_animPlaying = false;
                    m_selectedJoint = -1;
                    m_ctx.notify(TR("Bound model: ") + std::string(path));
                } else {
                    m_ctx.notify(TR("Failed to load model: ") +
                                 std::string(path));
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    const bool hasModel = (m_model != nullptr);
    if (!hasModel) ImGui::BeginDisabled();
    if (ImGui::Button(TR("Bind Model Only"), ImVec2(-1.0f, 0.0f)))
        bindModelToPlayer();
    tip(TR("Instantiate this model under the player and attach skinning, "
           "without touching the capsule or camera"));
    logRect("CHAR-BIND", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!hasModel) ImGui::EndDisabled();
}

void CharacterWindow::drawAnimationPanel() {
    if (!m_model || !m_model->skeleton) {
        ImGui::TextDisabled(TR("(no animation)"));
        tip(TR("This model has no skeleton, so it carries no animation clips."));
        return;
    }
    const assets::Skeleton& skel = *m_model->skeleton;
    if (skel.clips.empty()) {
        ImGui::TextDisabled(TR("(no clips)"));
        tip(TR("The file has a skeleton but no animation clips."));
        return;
    }

    // ---- 等按键状态（点击行尾的 [+] 进入）----
    //
    // 抓键走 ImGui 的键枚举，再**换算成 GLFW 码**存盘（两端同一套数，
    // 见 ui/KeyCodes.h）。Esc 取消：不这么做的话，想放弃就只剩
    // "随便按一个再把键清掉"这一条路。
    if (!m_captureClip.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1.0f),
                           TR("Press a key for '%s'  (Esc cancels)"),
                           m_captureClip.c_str());
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            m_captureClip.clear();
        } else {
            const int ik = ui_keys::captureImGuiKey();
            const int glfwCode = (ik == static_cast<int>(ImGuiKey_None))
                                     ? 0
                                     : ui_keys::glfwFromImGui(ik);
            if (glfwCode) {
                const std::string clip = m_captureClip;
                m_captureClip.clear();
                bindClipKey(clip, glfwCode);
            }
        }
    }

    // 玩家实体不存在时映射无处可写 → 整列禁用（但仍然能点播预览）
    const bool canBind = m_player.valid();
    if (!canBind) {
        ImGui::TextDisabled(TR("(no player - keys can't be saved)"));
        tip(TR("Open this window from a player entity (Shift+A ▸ Player) so "
               "key bindings have somewhere to live."));
    }

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float durW = ImGui::CalcTextSize("00.00s").x;
    const float keyW = 52.0f;
    const float builtinW = 46.0f;
    const float rowW = ImGui::GetContentRegionAvail().x;

    // 片段列表：点击名字即播放；行尾是时长、**内建键位**与按键绑定（P4）。
    for (const auto& clip : skel.clips) {
        ImGui::PushID(clip.name.c_str());
        const bool selected = (clip.name == m_currentClip);
        const float nameW = std::max(
            60.0f, rowW - durW - keyW - builtinW - spacing * 4.0f);
        if (ImGui::Selectable(clip.name.c_str(), selected, 0,
                              ImVec2(nameW, 0)))
            playClip(clip.name);
        // ⚠ 矩形必须在 Selectable **之后立刻**抓：下面还要在同一行画时长
        //   和绑定按钮，等画完再取 ImGui::GetItemRectMin 拿到的是最后一个
        //   控件那一小块 —— 自动化脚本按它去点就永远点不到条目（踩过）。
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        tip(TR("Click to preview this clip in the viewport above"));

        ImGui::SameLine();
        ImGui::TextDisabled("%.2fs", static_cast<double>(clip.duration));

        // ---- 内建键位（2026-10-22）----
        // 四段 loco clip 由状态机直接驱动，**不需要也不应该**先绑键：
        //   Idle 自动（停下就播）/ Walk & Run 由 WASD + 鼠标右键的**速度**决定 /
        //   Jump 由空格敲击后"离地"这个事实决定。
        // 这里显示的是**只读徽标**，不是按钮 —— 有按钮就会被当成"还得点一下
        // 才算配好"，而这正是用户这次要求去掉的步骤。
        ImGui::SameLine();
        if (const char* bl = builtinKeyLabel(clip.name)) {
            ImGui::TextDisabled("%s", bl);
            tip(TR("Built-in control (no key binding needed). "
                   "Walk / Run follow your actual speed; Jump follows "
                   "whether you are off the ground."));
        }

        ImGui::SameLine();
        const int boundKey = clipKey(clip.name);
        const bool capturing = (m_captureClip == clip.name);
        const char* keyLabel = capturing ? "..."
                                : boundKey ? ui_keys::label(boundKey)
                                           : "+";
        if (!canBind) ImGui::BeginDisabled();
        if (ImGui::Button(keyLabel, ImVec2(keyW, 0.0f))) {
            m_captureClip = capturing ? std::string() : clip.name;
        }
        const ImVec2 keyMin = ImGui::GetItemRectMin();
        const ImVec2 keyMax = ImGui::GetItemRectMax();
        tip(TR("Bind a keyboard key to this clip. In Play / Game.exe the key "
               "plays the clip once, then locomotion takes over again.\n"
               "Right-click to clear."));
        // 右键清掉这段的绑定（不弹菜单，一个动作一个入口）
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && boundKey)
            unbindClipKey(clip.name);
        if (!canBind) ImGui::EndDisabled();
        ImGui::PopID();

        logRect(("CHAR-CLIP-" + clip.name).c_str(), itemMin, itemMax);
        logRect(("CHAR-CLIP-KEY-" + clip.name).c_str(), keyMin, keyMax);
    }
}

// ---------------------------------------------------------------- 底部播放条

void CharacterWindow::drawTransportBar() {
    const bool hasClip = !m_currentClip.empty();
    const assets::AnimationClip* clip =
        (hasClip && m_model && m_model->skeleton)
            ? m_model->skeleton->findClip(m_currentClip)
            : nullptr;
    const float dur = clip ? clip->duration : 0.0f;

    if (!hasClip) ImGui::BeginDisabled();
    if (ImGui::Button(m_animPlaying ? TR("Pause") : TR("Play"))) {
        m_animPlaying = !m_animPlaying;
    }
    tip(TR("Play / pause the preview (space in UE's Persona)"));
    ImGui::SameLine();
    if (ImGui::Button(TR("Stop"))) {
        m_animPlaying = false;
        m_animTime = 0.0f;
    }
    tip(TR("Stop and rewind to 0"));
    ImGui::SameLine();
    if (ImGui::Button(TR("|<<"))) {
        m_animTime = 0.0f;
        m_animPlaying = false;
    }
    tip(TR("Jump to the first frame"));
    ImGui::SameLine();

    ImGui::Checkbox(TR("Loop"), &m_animLoop);
    tip(TR("Loop the clip when it reaches the end"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("##speed", &m_animSpeed, 0.1f, 3.0f, "x%.2f");
    tip(TR("Playback speed multiplier"));
    ImGui::SameLine();

    // 时间轴：占满剩余宽度（拖动即定格，方便看姿势）
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 130.0f));
    if (ImGui::SliderFloat("##timeline", &m_animTime, 0.0f, dur > 0.0f ? dur : 1.0f,
                           TR("%.2fs"))) {
        m_animPlaying = false;
    }
    tip(TR("Scrub the clip. Dragging pauses playback so you can inspect a pose."));
    logRect("CHAR-TRANSPORT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f / %.2fs", static_cast<double>(m_animTime),
                        static_cast<double>(dur));
    if (!hasClip) ImGui::EndDisabled();
}

// ---------------------------------------------------------------- 右栏 Details

void CharacterWindow::drawDetailsPanel() {
    ImGui::TextDisabled(TR("ASSET"));
    ImGui::Separator();
    ImGui::Text(TR("Name"));
    ImGui::SameLine(90.0f);
    // 模型名在 FBX 里就是文件路径 —— 窄栏里会顶破右边缘，只留文件名，
    // 全名走 tooltip（与下面 Source 行同一处理）。
    {
        const std::string nm = m_model ? m_model->name : std::string("-");
        const std::string shortName =
            nm.empty() || nm == "-"
                ? std::string("-")
                : std::filesystem::path(nm).filename().string();
        ImGui::TextDisabled("%s", shortName.c_str());
        if (nm.size() > shortName.size() &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s", nm.c_str());
    }
    ImGui::Text(TR("Source"));
    ImGui::SameLine(90.0f);
    // ⚠ 只显示**文件名**：完整路径动辄六七十字符，窄栏里会把右边缘撑破
    //   （实测 "assets/models/anime_girl.fbx" 直接顶出面板）。全路径走 tooltip。
    {
        const std::string file = m_modelPath.empty()
                                     ? std::string("-")
                                     : std::filesystem::path(m_modelPath)
                                           .filename()
                                           .string();
        ImGui::TextDisabled("%s", file.c_str());
        if (!m_modelPath.empty() &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", m_modelPath.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled(TR("SKELETON"));
    ImGui::Separator();
    const assets::Skeleton* skel =
        (m_model && m_model->skeleton) ? m_model->skeleton.get() : nullptr;
    ImGui::Text(TR("Joints"));
    ImGui::SameLine(90.0f);
    ImGui::TextDisabled("%d", skel ? skel->jointCount() : 0);
    ImGui::Text(TR("Clips"));
    ImGui::SameLine(90.0f);
    ImGui::TextDisabled("%zu", skel ? skel->clips.size() : 0u);
    ImGui::Text(TR("Bounds"));
    ImGui::SameLine(90.0f);
    if (m_boundsValid) {
        const glm::vec3 sz = m_boundsMax - m_boundsMin;
        ImGui::TextDisabled(TRL("%.2f x %.2f x %.2f"), static_cast<double>(sz.x),
                            static_cast<double>(sz.y), static_cast<double>(sz.z));
        tip(TR("Measured bounding box of the model in the preview scene "
               "(width x depth x height, metres). Capsule size and camera "
               "eye height are derived from the height."));
    } else {
        ImGui::TextDisabled("-");
    }

    // ---- 选中的骨骼 ----
    ImGui::Spacing();
    ImGui::TextDisabled(TR("BONE"));
    ImGui::Separator();
    if (skel && m_selectedJoint >= 0 &&
        m_selectedJoint < skel->jointCount()) {
        const auto& j = skel->joints[static_cast<size_t>(m_selectedJoint)];
        ImGui::Text(TR("Name"));
        ImGui::SameLine(90.0f);
        ImGui::TextDisabled("%s", j.name.empty() ? "(unnamed)" : j.name.c_str());
        ImGui::Text(TR("Parent"));
        ImGui::SameLine(90.0f);
        if (j.parent >= 0 && j.parent < skel->jointCount()) {
            const std::string& pn =
                skel->joints[static_cast<size_t>(j.parent)].name;
            ImGui::TextDisabled("%s", pn.empty() ? "(unnamed)" : pn.c_str());
        } else {
            ImGui::TextDisabled("(root)");
        }
        glm::vec3 wp(0.0f);
        if (jointWorldPosition(m_selectedJoint, wp)) {
            ImGui::Text(TR("Position"));
            ImGui::SameLine(90.0f);
            ImGui::TextDisabled("%.3f, %.3f, %.3f",
                                static_cast<double>(wp.x),
                                static_cast<double>(wp.y),
                                static_cast<double>(wp.z));
            tip(TR("Bone origin in model space, in the pose currently shown "
                   "in the viewport"));
        }
    } else {
        ImGui::TextDisabled("(none)");
        tip(TR("Pick a bone in the Skeleton tab to inspect it here"));
    }

    // ---- 选中的动画片段 ----
    //
    // UE 的 Persona 右栏在选中动画时显示 Clip 详情 + 一条带关键帧刻度的
    // 时间条。这里做同一件事，但**只读**：引擎没有"把关键帧写回资产"的
    // 通路（clip 是加载 glTF/FBX 时解析出来的，没有序列化格式），所以
    // 与其摆一排点不动的按钮，不如把"这条片段到底有多少帧、关键帧在哪"
    // 如实画出来 —— 编辑动作仍然只有播放 / 循环 / 变速（见底部播放条）。
    ImGui::Spacing();
    ImGui::TextDisabled(TR("CLIP"));
    ImGui::Separator();
    if (skel && !m_currentClip.empty()) {
        const assets::AnimationClip* clip = skel->findClip(m_currentClip);
        if (clip) {
            // 关键帧时刻：所有轨道 times 的并集（去重后按时间排）。
            // 一条 track 的 times 已经是单调递增，所以合并时只要逐个归并
            // 到 1e-4 的分辨率去重就够 —— O(tracks × keys)。
            std::vector<float> keys;
            size_t trackCount = 0;
            size_t keyCount = 0;
            for (const auto& tr : clip->tracks) {
                ++trackCount;
                keyCount += tr.times.size();
                for (float t : tr.times) {
                    bool dup = false;
                    for (float k : keys)
                        if (std::fabs(k - t) < 1.0e-4f) { dup = true; break; }
                    if (!dup) keys.push_back(t);
                }
            }
            std::sort(keys.begin(), keys.end());

            ImGui::Text(TR("Name"));
            ImGui::SameLine(90.0f);
            ImGui::TextDisabled("%s", clip->name.c_str());
            ImGui::Text(TR("Duration"));
            ImGui::SameLine(90.0f);
            ImGui::TextDisabled("%.3fs", static_cast<double>(clip->duration));
            ImGui::Text(TR("Tracks"));
            ImGui::SameLine(90.0f);
            ImGui::TextDisabled(TRL("%zu  (%zu keys)"), trackCount, keyCount);
            tip(TR("A track is one animated channel of one bone "
                   "(translation / rotation / scale)."));

            // 关键帧刻度：一条 14 px 高的窄条，时间线性映射。
            // 这只是"这条片段长什么样"的可视化，不可拖动（见上面的说明）。
            if (clip->duration > 0.0f && !keys.empty()) {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                const float hgt = 14.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + hgt),
                                  ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
                for (float t : keys) {
                    const float f = glm::clamp(t / clip->duration, 0.0f, 1.0f);
                    const float x = p0.x + f * w;
                    dl->AddLine(ImVec2(x, p0.y + 1.0f),
                                ImVec2(x, p0.y + hgt - 1.0f),
                                ImGui::GetColorU32(ImGuiCol_PlotLines), 1.0f);
                }
                // 播放头
                if (clip->duration > 0.0f) {
                    const float f =
                        glm::clamp(m_animTime / clip->duration, 0.0f, 1.0f);
                    const float x = p0.x + f * w;
                    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + hgt),
                                ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
                }
                ImGui::Dummy(ImVec2(w, hgt));
                tip(TR("Vertical ticks = keyframes of this clip, white line = "
                       "playhead. Editing keys is not supported yet: clips are "
                       "parsed from the model file at load time and have no "
                       "write-back path in this engine."));
            }
        }
    } else {
        ImGui::TextDisabled("(none)");
        tip(TR("Pick a clip in the Animation tab"));
    }

    // ---- 动作 ----
    ImGui::Spacing();
    ImGui::TextDisabled(TR("ACTIONS"));
    ImGui::Separator();

    const bool canAct = (m_model != nullptr && m_player.valid());
    if (!canAct) ImGui::BeginDisabled();

    // ★ 这个才是"创建角色"缺的那一步：光是绑一具模型，玩家还是空胶囊
    if (ImGui::Button(TR("Generate Skeleton Binding"), ImVec2(-1.0f, 0.0f)))
        generateSkeletonBinding();
    tip(TR("Bind the model to the player AND size the capsule / camera eye "
           "height from the model's bounds. This is the step that turns an "
           "empty player entity into a real character."));
    logRect("CHAR-BINDING", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    if (ImGui::Button(TR("Bind Model Only"), ImVec2(-1.0f, 0.0f)))
        bindModelToPlayer();
    tip(TR("Only instantiate the mesh + skinning. Leaves capsule / camera as-is."));

    if (!canAct) ImGui::EndDisabled();
    if (!m_player.valid()) {
        ImGui::TextDisabled(TR("player entity is gone"));
        tip(TR("The player this window was opened for no longer exists."));
    }
}

// ---------------------------------------------------------------- 键盘映射

int CharacterWindow::clipKey(const std::string& clip) const {
    if (!m_player.valid()) return 0;
    const scene::Scene& sc = m_ctx.editorScene();
    const auto* km = sc.world().get<ecs::AnimKeyMapComponent>(m_player);
    if (!km) return 0;
    for (const auto& e : km->entries)
        if (e.clip == clip) return e.key;
    return 0;
}

// 绑 / 解绑都走 `structuralEdit`：这是**场景数据**（跟着角色存盘），
// 与"拖模型 / 生成绑定"同级，用户应该能用 Ctrl+Z 撤掉。
//
// ⚠ 同一个键不能同时服务两段 clip：绑上去时把旧的清掉。留两份的话
//   `pollKeyTrigger` 只会命中表里靠前那一段，另一段永远触发不到，而面板上
//   两行都显示着同一个键 —— "明明绑了却不生效"。
void CharacterWindow::bindClipKey(const std::string& clip, int key) {
    if (!m_player.valid() || clip.empty() || key == 0) return;
    m_ctx.structuralEdit("Bind animation key", [&]() {
        ecs::World& w = m_ctx.editorScene().world();
        ecs::AnimKeyMapComponent* km =
            w.get<ecs::AnimKeyMapComponent>(m_player);
        if (!km) {
            ecs::AnimKeyMapComponent fresh;
            w.add<ecs::AnimKeyMapComponent>(m_player, fresh);
            km = w.get<ecs::AnimKeyMapComponent>(m_player);
        }
        if (!km) return;
        // 该键被别的 clip 占着 → 先回收
        for (auto& e : km->entries)
            if (e.key == key && e.clip != clip) e.key = 0;
        km->entries.erase(
            std::remove_if(km->entries.begin(), km->entries.end(),
                           [](const ecs::AnimKeyMapEntry& e) {
                               return e.clip.empty() || e.key == 0;
                           }),
            km->entries.end());
        bool found = false;
        for (auto& e : km->entries) {
            if (e.clip == clip) {
                e.key = key;
                found = true;
                break;
            }
        }
        if (!found) km->entries.push_back(ecs::AnimKeyMapEntry{clip, key});
    });
    m_ctx.dirty() = true;
    VK_LOG_INFO("CHAR-KEYBIND clip=%s key=%d label=%s", clip.c_str(), key,
                ui_keys::label(key));
    m_ctx.notify(TR("Bound ") + clip + " -> " + ui_keys::label(key));
}

void CharacterWindow::unbindClipKey(const std::string& clip) {
    if (!m_player.valid() || clip.empty()) return;
    m_ctx.structuralEdit("Unbind animation key", [&]() {
        ecs::World& w = m_ctx.editorScene().world();
        ecs::AnimKeyMapComponent* km =
            w.get<ecs::AnimKeyMapComponent>(m_player);
        if (!km) return;
        for (auto& e : km->entries)
            if (e.clip == clip) e.key = 0;
        km->entries.erase(
            std::remove_if(km->entries.begin(), km->entries.end(),
                           [](const ecs::AnimKeyMapEntry& e) {
                               return e.clip.empty() || e.key == 0;
                           }),
            km->entries.end());
    });
    m_ctx.dirty() = true;
    VK_LOG_INFO("CHAR-KEYUNBIND clip=%s", clip.c_str());
}

// ---------------------------------------------------------------- 播放控制

void CharacterWindow::playClip(const std::string& clipName) {
    m_currentClip = clipName;
    m_animTime = 0.0f;
    m_animPlaying = true;
    VK_LOG_INFO("CharacterWindow: play clip '%s'", clipName.c_str());
    logRect("CHAR-PLAY-CLIP", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void CharacterWindow::syncPreviewSkinning() {
    if (!m_previewRoot.valid() || !m_model || !m_model->skinned()) return;
    auto* sk =
        m_previewScene.world().get<ecs::SkinnedMeshComponent>(m_previewRoot);
    if (!sk) return;
    sk->clipName = m_currentClip;
    sk->time = m_animTime;
    sk->playing = m_animPlaying;
    sk->speed = m_animSpeed;

    // ⚠ 这一行就是"预览里能不能看见动画"的全部关键（2026-10-22）。
    //
    // 预览场景**没有 AnimatorSystem**（那是主场景 / 运行时的系统），于是
    // 没人推进这里的关节矩阵。渲染层确实有一条兜底（`updateSkinning` 里
    // "size != jointCount 就现场采一次"），但实例化时矩阵已被填成
    // jointCount 个（静止姿势）—— **尺寸永远是满的**，兜底一次都不会触发。
    // 结果：时间轴在走、`sk.time` 在变、日志一切正常，屏幕上那个人一动不动。
    //
    // 现在由这里每帧把 `draw()` 采样出来的矩阵推进去。尺寸对不上就跳过
    // （骨架换了 / 还没采过），让渲染层的兜底接住，不要写半截矩阵进去。
    if (m_model->skeleton && sk->model && sk->model->skeleton) {
        const std::size_t n =
            static_cast<std::size_t>(sk->model->skeleton->jointCount());
        if (m_jointMats.size() == n &&
            sk->jointMatrices.size() != n)
            sk->jointMatrices.resize(n);
        if (m_jointMats.size() == n) sk->jointMatrices = m_jointMats;
    }
}

// ---------------------------------------------------------------- 写主场景

void CharacterWindow::bindModelToPlayer() {
    if (!m_model || !m_player.valid()) return;
    scene::Scene& sc = m_ctx.editorScene();

    // 先清掉玩家下已有的模型子节点（避免重复绑定堆叠）
    // 保留 PlayerComponent 与 CollisionComponent（胶囊碰撞体）。
    m_ctx.structuralEdit("Bind character model", [&]() {
        ecs::World& w = sc.world();
        if (auto* h = w.get<ecs::HierarchyComponent>(m_player)) {
            auto kids = h->children;
            for (auto kidId : kids) {
                ecs::Entity kid = w.handle(kidId);
                if (!kid.valid()) continue;
                if (w.has<ecs::MeshComponent>(kid)) w.destroy(kid);
            }
        }
        instanceModelInto(sc, m_model, m_player, /*autoPlay=*/true);
    });
    m_ctx.dirty() = true;
    m_ctx.notify(TR("Bound model to player"));
    VK_LOG_INFO("CharacterWindow: bound model '%s' to player '%s'",
                m_model->name.c_str(), m_playerName.c_str());
}

void CharacterWindow::generateSkeletonBinding() {
    if (!m_model || !m_player.valid()) return;
    scene::Scene& sc = m_ctx.editorScene();

    // 模型尺寸 → 胶囊尺寸 / 相机眼高。
    //
    // ⚠ 这里**不能**直接用 m_model->radius（那是包围球）：一个张开手臂的
    //   角色包围球半径会远大于肩膀宽度，胶囊会变成一个桶。用模型包围盒的
    //   高度算更稳：人形角色高度 h → 半径 ≈ 0.16h（≈ 一个头的宽度），
    //   胶囊半高 = h/2 - r（胶囊总高 = 2*(halfHeight+radius) = h）。
    if (!m_boundsValid) computeModelBounds();
    float h = 1.8f;
    if (m_boundsValid) {
        h = std::max(m_boundsMax.z - m_boundsMin.z, 0.2f);
    } else if (m_model->radius > 0.0f) {
        h = m_model->radius * 2.0f;
    }
    float radius = glm::clamp(h * 0.16f, 0.15f, 0.60f);
    float halfHeight = std::max(0.10f, h * 0.5f - radius);
    // 眼高：胶囊中心到眼睛的距离。人眼在身高 ~0.92 处，等于
    // (h*0.92 - h/2) = 0.42h，相对胶囊中心就是这个数。
    const float eyeOffset = glm::clamp(h * 0.42f, 0.25f, 1.10f);

    m_ctx.structuralEdit("Generate character binding", [&]() {
        ecs::World& w = sc.world();

        // ① 清掉上一轮绑定的网格子节点
        if (auto* hc = w.get<ecs::HierarchyComponent>(m_player)) {
            auto kids = hc->children;
            for (auto kidId : kids) {
                ecs::Entity kid = w.handle(kidId);
                if (!kid.valid()) continue;
                if (w.has<ecs::MeshComponent>(kid)) w.destroy(kid);
            }
        }
        // ② 实例化模型（蒙皮模型会在玩家实体上挂 SkinnedMeshComponent）
        instanceModelInto(sc, m_model, m_player, /*autoPlay=*/true);

        // ③ 胶囊：没有就建，有就按模型尺寸改写
        ecs::CharacterControllerComponent* cc =
            w.get<ecs::CharacterControllerComponent>(m_player);
        if (!cc) {
            ecs::CharacterControllerComponent fresh;
            fresh.radius = radius;
            fresh.halfHeight = halfHeight;
            w.add<ecs::CharacterControllerComponent>(m_player, fresh);
            cc = w.get<ecs::CharacterControllerComponent>(m_player);
        } else {
            cc->radius = radius;
            cc->halfHeight = halfHeight;
        }
        // 台阶级别跟身高走：小腿大约 0.2h，再封在 [0.2, 0.5]
        if (cc) cc->stepHeight = glm::clamp(h * 0.20f, 0.20f, 0.50f);

        // ④ 相机：第三人称跟随相机的眼高 + 距离
        ecs::CameraComponent* camC = w.get<ecs::CameraComponent>(m_player);
        if (!camC) {
            ecs::CameraComponent fresh;
            fresh.targetOffset = glm::vec3(0.0f, 0.0f, eyeOffset);
            fresh.distance = glm::clamp(h * 2.2f, 2.0f, 7.0f);
            fresh.minDistance = 0.6f;
            w.add<ecs::CameraComponent>(m_player, fresh);
            camC = w.get<ecs::CameraComponent>(m_player);
        } else {
            camC->targetOffset.z = eyeOffset;
            camC->distance = glm::clamp(h * 2.2f, 2.0f, 7.0f);
        }
    });

    m_ctx.dirty() = true;
    m_ctx.notify(TR("Generated skeleton binding"));
    VK_LOG_INFO("CHAR-BINDING generated player='%s' model='%s' skinned=%d "
                "joints=%d clips=%zu height=%.3f radius=%.3f halfHeight=%.3f "
                "eye=%.3f",
                m_playerName.c_str(), m_model->name.c_str(),
                m_model->skinned() ? 1 : 0,
                m_model->skeleton ? m_model->skeleton->jointCount() : 0,
                m_model->skeleton ? m_model->skeleton->clips.size() : 0u,
                static_cast<double>(h), static_cast<double>(radius),
                static_cast<double>(halfHeight), static_cast<double>(eyeOffset));
}

// ---------------------------------------------------------------- 主流程

void CharacterWindow::draw() {
    if (!m_open) return;
    if (m_previewDirty) rebuildPreviewScene();

    // 每帧推进动画时间（速度 / 循环都在这里）
    const assets::AnimationClip* curClip =
        (m_model && m_model->skeleton)
            ? m_model->skeleton->findClip(m_currentClip)
            : nullptr;
    if (m_animPlaying && curClip) {
        m_animTime += ImGui::GetIO().DeltaTime * m_animSpeed;
        if (curClip->duration > 0.0f) {
            if (m_animTime > curClip->duration) {
                if (m_animLoop) {
                    m_animTime = std::fmod(m_animTime, curClip->duration);
                } else {
                    m_animTime = curClip->duration;
                    m_animPlaying = false;
                }
            }
        }
    }

    // 采样当前姿势 —— Details 的骨骼世界位置与"选中骨骼"高亮都靠它
    if (m_model && m_model->skeleton) {
        m_model->skeleton->sample(m_animPlaying || m_animTime > 0.0f
                                      ? curClip
                                      : nullptr,
                                  m_animTime, m_jointMats);
    } else {
        m_jointMats.clear();
    }

    // 把动画状态同步给预览场景里的 SkinnedMeshComponent
    syncPreviewSkinning();

    // ---- 窗口 ----
    // 默认尺寸按屏幕给：UE 的 Persona 面板是"越大越好"，760×560 那种尺寸
    // 连骨骼树都展不开。上限跟着显示器走，避免在 1366×768 的笔记本上出屏。
    //
    // ⚠ 为什么窗口 ID 带一个 `_v2`（2026-10-17）：
    //   ImGui 的浮动窗口把 Pos/Size 落在 cwd 的 imgui.ini 里，而
    //   `ImGuiCond_FirstUseEver` 只在**没有存档**时才用我们的默认值。
    //   老版本存的是 `[Window][character] Size=760,560` —— 于是把默认值
    //   从 760×560 改到 ~1230×690 之后，**老用户身上一点变化都没有**：
    //   打开还是那个挤扁的小窗。实测（1578×844 客户区）看到的正是
    //   760×560 那个旧框。改 ID 让旧条目彻底作废，是这里唯一可靠的做法
    //   （和偏好 `kPrefsVersion` 升版本让老档作废是同一个道理；性能上
    //   只丢一次窗口位置，可接受）。
    const ImGuiIO& io = ImGui::GetIO();
    const float defW = glm::clamp(io.DisplaySize.x * 0.78f, 980.0f, 1500.0f);
    const float defH = glm::clamp(io.DisplaySize.y * 0.82f, 620.0f, 980.0f);
    // 2026-10-21：被托管成**视口标签页**时，矩形由 EditorApp 给，浮动
    // 默认值（含那条 880×560 的下限）全部跳过 —— 标签页内容区可能比它窄，
    // 尺寸约束会把窗口顶出去、盖住右边的 Inspector。
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowSize(ImVec2(defW, defH), ImGuiCond_FirstUseEver);
        // 下限：低于这个尺寸三栏布局会互相挤扁（用户拖到很小也不会崩，但
        // 预览视口会缩到看不见东西 —— 直接禁止）
        ImGui::SetNextWindowSizeConstraints(ImVec2(880.0f, 560.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.09f,
                                       io.DisplaySize.y * 0.06f),
                                ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Character", "character_v2"),
                      docked ? nullptr : &m_open, wf)) {
        ImGui::End();
        return;
    }

    // 关闭时清理
    if (!m_open) {
        close();
        ImGui::End();
        return;
    }
    logRect("CHAR-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                   ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));

    // ---- 标题行 ----
    ImGui::Text(TR("Character: %s"), m_playerName.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_modelPath.empty() ? "" : m_modelPath.c_str());
    tip(TR("Player entity this window was opened for"));
    // 工具条自成一行、靠右摆（标题行留给"角色名 + 模型来源"，挤一行会在
    // 窄窗口里互相盖住）。窗口太窄时退化成左对齐，不硬算负坐标。
    const float rightW = 296.0f;
    const float toolbarW = 470.0f;
    const float titleRight = ImGui::GetWindowContentRegionMax().x;
    const float toolbarX = titleRight - toolbarW;
    if (toolbarX > 0.0f) ImGui::SetCursorPosX(toolbarX);
    drawToolbar();

    // ---- 播放条高度预留 ----
    const float transportH = ImGui::GetFrameHeightWithSpacing() +
                             ImGui::GetStyle().ItemSpacing.y;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float bodyH = std::max(120.0f, avail.y - transportH);
    const float leftW = 300.0f;
    const float rW = std::min(rightW, std::max(220.0f, avail.x - leftW - 320.0f));
    const float previewW = std::max(240.0f, avail.x - leftW - rW -
                                             ImGui::GetStyle().ItemSpacing.x * 2.0f);

    // ---- 左栏：Skeleton / Mesh / Animation ----
    ImGui::BeginChild("##char_left", ImVec2(leftW, bodyH),
                      ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("##char_tabs")) {
        if (ImGui::BeginTabItem(TR("Skeleton"))) {
            drawSkeletonPanel();
            ImGui::EndTabItem();
        }
        logRect("CHAR-TAB-Skeleton", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::BeginTabItem(TR("Mesh"))) {
            drawMeshPanel();
            ImGui::EndTabItem();
        }
        logRect("CHAR-TAB-Mesh", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::BeginTabItem(TR("Animation"))) {
            drawAnimationPanel();
            ImGui::EndTabItem();
        }
        logRect("CHAR-TAB-Animation", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // ---- 中栏：预览视口 ----
    ImGui::SameLine();
    ImGui::BeginChild("##char_preview", ImVec2(previewW, bodyH),
                      ImGuiChildFlags_Borders);
    drawPreviewViewport();
    ImGui::EndChild();

    // ---- 右栏：Details（UE 的 Asset Details）----
    ImGui::SameLine();
    ImGui::BeginChild("##char_details", ImVec2(0.0f, bodyH),
                      ImGuiChildFlags_Borders);
    drawDetailsPanel();
    ImGui::EndChild();

    // ---- 底部播放条 ----
    drawTransportBar();

    ImGui::End();
}

} // namespace editor

