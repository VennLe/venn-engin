#include "CharacterWindow.h"

#include "Localization.h"
#include "DebugRects.h"
#include "DopeSheetWindow.h"
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
#include <glm/gtx/quaternion.hpp>   // eulerAngles（Pose Mode 的欧拉角编辑）

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
    // 判定本体在 `assets/AssetPath.*`（运行时的 `spawn()` 也走这一份）。
    return assets::isFbxPath(p);
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

bool CharacterWindow::jointWorldMatrix(int joint, glm::mat4& out) const {
    if (!m_model || !m_model->skeleton) return false;
    const assets::Skeleton& skel = *m_model->skeleton;
    if (joint < 0 || joint >= skel.jointCount()) return false;
    if (joint >= static_cast<int>(m_jointMats.size())) return false;
    out = glm::inverse(skel.invMeshNode) *
          m_jointMats[static_cast<size_t>(joint)] *
          glm::inverse(skel.joints[static_cast<size_t>(joint)].invBind);
    return true;
}

// ---------------------------------------------------------------- 采样链

void CharacterWindow::revalidateEdit() {
    m_editDirty = false;
    m_editResolved.clear();
    m_editStats = animation::ResolveStats{};
    if (!m_model || !m_model->skeleton) return;
    m_editStats =
        animation::resolveEditTracks(*m_model->skeleton, m_edit, m_editResolved);
}

void CharacterWindow::samplePoseInto() {
    if (!m_model || !m_model->skeleton) {
        m_jointMats.clear();
        m_worldMats.clear();
        m_pose.resize(0);
        return;
    }
    const assets::Skeleton& skel = *m_model->skeleton;

    if (!m_poseMode) {
        // ⚠ 这一支必须与改动前**逐位相同**：条件、顺序、参数一个都不能动。
        //   老路径是 `sample(cond ? curClip : nullptr, m_animTime, out)`，
        //   而 sample 就是 samplePose + poseToJoints —— 于是拆开写等价。
        const assets::AnimationClip* clip =
            (m_animPlaying || m_animTime > 0.0f) ? skel.findClip(m_currentClip)
                                                 : nullptr;
        animation::samplePose(skel, clip, m_animTime, m_pose);
        // S-C：约束栈在最上层。**空栈时 `evaluate` 立刻返回、一个字节都不写**，
        // 所以"没有约束 = 逐位等于旧路径"这条老断言不受影响（自检里
        // `constraint_*_bitwise` 那几条钉的就是短路本身）。
        m_conStats = m_constraints.evaluate(skel, m_pose);
        animation::poseToJoints(skel, m_pose, m_jointMats);
        m_worldMats.clear();   // 非 Pose Mode 不算第二遍 FK
        return;
    }

    // ---- Pose Mode ----
    // 底子：base clip（没设就纯静止姿势），然后编辑轨道只覆盖它自己那几个
    // (joint, path)。所以"只动了右臂"的资产不会把其他 60 根骨锁死。
    const assets::AnimationClip* base =
        m_edit.baseClip.empty() ? nullptr : skel.findClip(m_edit.baseClip);
    animation::samplePose(skel, base, m_animTime, m_pose);
    if (m_editDirty) revalidateEdit();
    animation::overlayEditPose(m_editResolved, m_animTime, m_pose);

    // 瞬态层盖在最上面：用户手摆出来、还没打帧的那几根骨。
    {
        const size_t n = static_cast<size_t>(m_pose.count());
        ensureTransient(m_pose.count());
        for (size_t k = 0; k < n; ++k) {
            if (!m_touched[k]) continue;
            m_pose.translation[k] = m_touchedT[k];
            m_pose.rotation[k] = m_touchedR[k];
            m_pose.scale[k] = m_touchedS[k];
        }
    }

    // 约束栈（S-C）必须盖在**所有**姿态源之上：base clip → 编辑轨道 →
    // 手摆的瞬态层 → 约束。顺序反过来就会"约束被下一帧的编辑轨道冲掉"。
    m_conStats = m_constraints.evaluate(skel, m_pose);

    animation::poseToJoints(skel, m_pose, m_jointMats);
    // gizmo 要"骨在哪 + 朝向哪"，世界矩阵单独一遍（63 根骨，可忽略）。
    animation::poseToWorld(skel, m_pose, m_worldMats);
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

    // 权重绘制（S-B）：只在 Weights 页被选中时生效。
    // 顺序是"先刷、后画" —— 同一帧就能看到刷的结果，否则作者的观感是
    // "画笔跟不上手"（权重带是上一帧的）。
    if (m_weightsTabOpen) {
        handleWeightsPaint(imgMin, imgSz, pvp, hovered);
        drawWeightsOverlay(imgMin, imgSz, pvp, hovered);
    }

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
    // ⚠ `!m_gizmoConsumed`：Pose Mode 下按在旋转环上的那一下是"开始转骨"，
    //   不是"改选中对象"。不挡的话，按住环拖的同时还会把 m_pick 换掉
    //   （拖右臂结果选中了胶囊，脚下那条变换条跟着变，非常迷惑）。
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !m_gizmoConsumed && m_previewRoot.valid()) {
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

    // ---- ①b 骨架（S-A）：rest 骨线 + 选中骨的三轴 ----
    //
    // 为什么必须画三轴：`roll` 是"绕骨轴扭"，光看一根线**看不出任何变化**
    // （线段本身旋转对称）。三轴一起画，roll 才有视觉反馈 —— 否则作者只能
    // 靠"拧了之后手脚翻不翻"来猜，而这个猜法一次要摆十分钟 pose。
    if (m_rigActive && m_showRigBones &&
        m_rigWorld.size() == m_rig.bones.size()) {
        for (std::size_t i = 0; i < m_rig.bones.size(); ++i) {
            const animation::EditBone& b = m_rig.bones[i];
            const glm::mat4& W = m_rigWorld[i];
            const glm::vec3 h = glm::vec3(W[3]);
            const glm::vec3 t = h + b.dir * b.length;
            ImVec2 pa, pb;
            if (!proj(h, pa) || !proj(t, pb)) continue;   // 有一端在相机后面
            const bool sel = (static_cast<int>(i) == m_rigSel);
            const ImU32 col = sel ? IM_COL32(255, 226, 96, 255)
                                  : (b.deform ? IM_COL32(120, 200, 235, 200)
                                              : IM_COL32(150, 150, 150, 130));
            dl->AddLine(pa, pb, col, sel ? 2.6f : 1.6f);
            dl->AddCircleFilled(pa, sel ? 3.4f : 2.2f, col);
            if (!sel) continue;
            const float al = glm::clamp(b.length * 0.5f, 0.02f, 0.10f);
            const glm::vec3 ax[3] = {glm::normalize(glm::vec3(W[0])),
                                     glm::normalize(glm::vec3(W[1])),
                                     glm::normalize(glm::vec3(W[2]))};
            const ImU32 ac[3] = {IM_COL32(235, 96, 96, 230),
                                 IM_COL32(96, 208, 116, 230),
                                 IM_COL32(96, 148, 235, 230)};
            for (int k = 0; k < 3; ++k) {
                ImVec2 pe;
                if (proj(h + ax[k] * al, pe)) dl->AddLine(pa, pe, ac[k], 1.4f);
            }
        }
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

    // ---- ④ 骨骼旋转 gizmo（Pose Mode）----
    //
    // ⚠ 放在 overlay 里而不是"点选之后"，是因为这个函数在
    //   drawPreviewViewport 里**先于**点选跑（overlay 是第 654 行、点选在
    //   其后）。命中判定与绘制放在同一个函数里，就不会出现"画出来的环
    //   和能点中的环不是同一圈"——胶囊线框当初踩过这个坑（见 m_capScrSegs
    //   那段注释），这次直接按"画什么就量什么"来做。
    if (m_poseMode) drawBoneGizmo(imgMin, imgSz, vp);

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

    // ---- Pose Mode（R7）----
    //
    // 和 Animation 页是两件事：那边是"播导入的片段"，这边是"自己摆姿势并
    // 打帧"。开的时候顺手把 baseClip 设成当前正在预览的片段 —— 你看到什么
    // 就该编辑什么，否则一开 Pose Mode 姿势会跳回静止，用户第一反应是
    // "刚才那段动画丢了"。
    ImGui::SameLine();
    {
        const bool was = m_poseMode;
        const ImVec4 onCol(0.35f, 0.62f, 0.95f, 1.0f);
        if (was) ImGui::PushStyleColor(ImGuiCol_Button, onCol);
        if (ImGui::Button(TR("Pose Mode"))) {
            m_poseMode = !m_poseMode;
            if (m_poseMode) {
                if (m_edit.baseClip.empty()) m_edit.baseClip = m_currentClip;
                if (m_edit.name.empty())
                    m_edit.name = m_currentClip.empty() ? std::string("custom")
                                                        : m_currentClip;
                if (m_editPath.empty()) {
                    const std::string nm =
                        m_edit.name.empty() ? std::string("clip") : m_edit.name;
                    m_editPath = "assets/anims/" + nm + ".anim.json";
                    std::snprintf(m_editPathBuf, sizeof(m_editPathBuf), "%s",
                                  m_editPath.c_str());
                }
                m_editDirty = true;
                m_animPlaying = false;   // 摆姿势要一个静止的画面
                m_focusPoseTab = true;   // 左栏自动切到 Pose 页
            }
        }
        if (was) ImGui::PopStyleColor();
        logRect("CHAR-POSE-MODE", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        tip(TR("Pose Mode: pick a bone, rotate it with the gizmo, press I to "
               "key. Keys are stored in a .anim.json asset (the imported clip "
               "stays read-only)."));
        if (was != m_poseMode) {
            VK_LOG_INFO("CHAR-POSE-MODE on=%d base='%s' tracks=%d keys=%d "
                        "path=%s",
                        m_poseMode ? 1 : 0, m_edit.baseClip.c_str(),
                        static_cast<int>(m_edit.tracks.size()),
                        m_edit.totalKeys(), m_editPath.c_str());
        }
    }

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
    //
    // ⚠ PushID 用**下标**，不用 clip.name（2026-10-28 实测踩到）。名字看起来
    //   天然唯一，实际不是：`anime_girl.fbx` 里就有 4 个动画栈，归一化掉
    //   `<armature>|` 前缀之后变成 [Idle, Walk, Idle, Walk] —— 于是两行 "Walk"
    //   同帧同 ID。ImGui 的 ID 冲突检测会**弹一个模态告警窗**（"Programmer
    //   error: 2 visible items with conflicting ID!"），模态窗一出现，后面
    //   所有鼠标点击全被它吃掉：`verify_character_window` 的"点动画条目触发
    //   播放"就是这么红的 —— 界面看起来完全正常，只是点哪都没反应。
    //   下标在同一个 skel 里一定唯一，`logRect` 的锚点仍按名字打（脚本要看
    //   的是"哪一段"，不是"第几行"）。
    for (std::size_t ci = 0; ci < skel.clips.size(); ++ci) {
        const assets::AnimationClip& clip = skel.clips[ci];
        ImGui::PushID(static_cast<int>(ci));
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
    // Pose Mode 下时间轴归编辑资产管：长度是 m_edit.duration，而且**没有
    // 导入片段也要能拖**（从零手 K 一段动画是完全正常的用法）。
    const float dur = m_poseMode ? std::max(m_edit.duration, 1.0e-3f)
                                 : (clip ? clip->duration : 0.0f);
    const bool disabled = !hasClip && !m_poseMode;

    if (disabled) ImGui::BeginDisabled();
    if (ImGui::Button(m_animPlaying ? TR("Pause") : TR("Play"))) {
        m_animPlaying = !m_animPlaying;
        // 播放 = 时间要动 → 未打帧的姿态该丢掉（Blender 同语义）。
        // 不丢的话播放时那几根骨会卡着不动，看起来像"播放坏了"。
        if (m_animPlaying) clearTransient();
    }
    tip(TR("Play / pause the preview (space in UE's Persona)"));
    ImGui::SameLine();
    if (ImGui::Button(TR("Stop"))) {
        m_animPlaying = false;
        m_animTime = 0.0f;
        clearTransient();
    }
    tip(TR("Stop and rewind to 0"));
    ImGui::SameLine();
    if (ImGui::Button(TR("|<<"))) {
        m_animTime = 0.0f;
        m_animPlaying = false;
        clearTransient();
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
    if (ImGui::SliderFloat("##timeline", &m_animTime, 0.0f,
                           dur > 0.0f ? dur : 1.0f, TR("%.2fs"))) {
        m_animPlaying = false;
        clearTransient();
    }
    tip(TR("Scrub the clip. Dragging pauses playback so you can inspect a pose."));
    logRect("CHAR-TRANSPORT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f / %.2fs", static_cast<double>(m_animTime),
                        static_cast<double>(dur));
    if (disabled) ImGui::EndDisabled();
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
    //   （实测 "assets/models/characters/animate_girl.fbx" 直接顶出面板）。全路径走 tooltip。
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

    // ---- Pose Mode：选中骨的局部 TRS + 打帧 ----
    drawPoseDetails();

    // ---- 选中的动画片段 ----
    //
    // UE 的 Persona 右栏在选中动画时显示 Clip 详情 + 一条带关键帧刻度的
    // 时间条。这里做同一件事，但**只读**。
    //
    // ⚠ 这里原来写着"引擎没有把关键帧写回资产的通路"。2026-10-28（R7）
    //   之后这句话只对了一半：写回通路有了，但它写的是**自己的**
    //   `.anim.json`（见 animation/ClipEdit），导入片段依然是只读的 ——
    //   重新导入一次就把用户数据冲掉的设计不能要。所以这一栏仍然只用来
    //   "看这条片段长什么样"，改动作请切 Pose Mode。
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

// ================================================================
// Pose Mode（R7）
// ================================================================
//
// 这一段有三层，从上到下：
//   ① 瞬态姿态（m_touched*）—— 用户摆出来但还没打帧的东西
//   ② 关键帧轨道（m_edit.tracks）—— 打了帧的
//   ③ base clip（导入的）—— 剩下没被覆盖的骨
//
// 为什么要 ①：摆姿势的时候不能每动一下就写一个关键帧（Blender / UE 也都
// 不是）。但 `samplePoseInto()` 每帧都从头重算，所以"摆出来的角度"必须
// 存在某个地方，否则鼠标一松就弹回去。所以有一条独立的瞬态层：
//   · 拖动 gizmo / 拨 TRS 控件 → 写瞬态
//   · 按 I → 把瞬态写进轨道，并**清掉该骨的瞬态**（之后由轨道驱动）
//   · 拖时间轴 / 播放 → 清掉全部瞬态（Blender 里"未打帧的姿态在跳帧时
//     丢弃"就是这个语义）

namespace {

// 屏幕空间点到线段距离（与点选那块用的是同一套算法，见 drawPreviewViewport）
float screenSegDist(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const glm::vec2 ab(b.x - a.x, b.y - a.y);
    const glm::vec2 ap(p.x - a.x, p.y - a.y);
    const float len2 = glm::dot(ab, ab);
    float u = len2 > 1.0e-6f ? glm::dot(ap, ab) / len2 : 0.0f;
    u = glm::clamp(u, 0.0f, 1.0f);
    const glm::vec2 d = ap - ab * u;
    return glm::length(d);
}

constexpr float kGizmoHitPx = 8.0f;   // 环的命中半径（像素）
constexpr int   kGizmoSeg = 40;       // 每个环分多少段

const char* axisName(int a) {
    return a == 0 ? "x" : a == 1 ? "y" : a == 2 ? "z" : "-";
}

} // namespace

void CharacterWindow::clearTransient() {
    int n = 0;
    for (char c : m_touched) {
        if (c) ++n;
    }
    std::fill(m_touched.begin(), m_touched.end(), 0);
    if (n > 0) VK_LOG_INFO("CHAR-POSE-TRANSIENT-CLEAR n=%d", n);
}

void CharacterWindow::ensureTransient(int n) {
    if (n < 0) n = 0;
    if (static_cast<int>(m_touched.size()) == n) return;
    m_touched.assign(static_cast<size_t>(n), 0);
    m_touchedT.assign(static_cast<size_t>(n), glm::vec3(0.0f));
    m_touchedR.assign(static_cast<size_t>(n), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    m_touchedS.assign(static_cast<size_t>(n), glm::vec3(1.0f));
}

void CharacterWindow::touchBone(int j) {
    if (j < 0 || j >= m_pose.count()) return;
    ensureTransient(m_pose.count());
    const size_t k = static_cast<size_t>(j);
    if (m_touched[k]) return;        // 已经碰过了，别把改动抄回去
    // 三个通道各抄一份当前值（见头文件里的 ⚠）
    m_touchedT[k] = m_pose.translation[k];
    m_touchedR[k] = m_pose.rotation[k];
    m_touchedS[k] = m_pose.scale[k];
    m_touched[k] = 1;
}

void CharacterWindow::drawBoneGizmo(const ImVec2& imgMin, const ImVec2& imgSz,
                                   const glm::mat4& viewProj) {
    m_gizmoHoverAxis = -1;
    m_gizmoConsumed = false;
    if (!m_poseMode || !m_showBoneGizmo) return;
    if (!m_model || !m_model->skeleton) return;
    const assets::Skeleton& skel = *m_model->skeleton;
    const int j = m_selectedJoint;
    if (j < 0 || j >= skel.jointCount()) return;
    if (j >= static_cast<int>(m_worldMats.size())) return;
    if (imgSz.x < 4.0f || imgSz.y < 4.0f) return;

    const glm::mat4& W = m_worldMats[static_cast<size_t>(j)];
    const glm::vec3 center(W[3]);
    const glm::vec3 ax = glm::normalize(glm::vec3(W[0]));
    const glm::vec3 ay = glm::normalize(glm::vec3(W[1]));
    const glm::vec3 az = glm::normalize(glm::vec3(W[2]));

    // 环的半径按模型高度取：写死米数的话，给一只 3 cm 的小虫子和 1.8 m 的
    // 角色调同一份代码，一边是麻花一边是看不见。
    float h = 1.8f;
    if (m_boundsValid) h = std::max(m_boundsMax.z - m_boundsMin.z, 0.05f);
    const float r = glm::clamp(h * 0.11f, 0.02f, 2.0f);

    const auto proj = [&](const glm::vec3& p, ImVec2& out) {
        glm::vec2 s;
        if (!PickingSystem::worldToScreen(viewProj,
                                          glm::vec2(imgMin.x, imgMin.y),
                                          glm::vec2(imgSz.x, imgSz.y), p, s))
            return false;
        out = ImVec2(s.x, s.y);
        return true;
    };

    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool hovered = mp.x >= imgMin.x && mp.x <= imgMin.x + imgSz.x &&
                         mp.y >= imgMin.y && mp.y <= imgMin.y + imgSz.y;

    // 三个环：绕 X（红，在 YZ 平面）/ 绕 Y（绿，XZ）/ 绕 Z（蓝，XY）
    const glm::vec3 u[3] = {ay, ax, ax};
    const glm::vec3 v[3] = {az, az, ay};
    const ImU32 base[3] = {IM_COL32(226, 86, 86, 210), IM_COL32(96, 208, 116, 210),
                           IM_COL32(96, 148, 235, 210)};
    const ImU32 hot[3] = {IM_COL32(255, 150, 150, 255), IM_COL32(160, 255, 180, 255),
                          IM_COL32(170, 205, 255, 255)};

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- 命中：先算三个环的屏幕折线，再量鼠标到折线的最近距离 ----
    float bestD[3] = {1.0e30f, 1.0e30f, 1.0e30f};
    for (int a = 0; a < 3; ++a) {
        ImVec2 prev;
        bool havePrev = false;
        for (int i = 0; i <= kGizmoSeg; ++i) {
            // 360° 的环但绕轴旋转时是 360°；这里刻意只画 360°，因为
            // 半环会让"往哪个方向转"变得含糊（Blender 的旋转环是整圈）。
            const float t = 6.28318530718f * static_cast<float>(i) /
                            static_cast<float>(kGizmoSeg);
            const glm::vec3 p =
                center + u[a] * (std::cos(t) * r) + v[a] * (std::sin(t) * r);
            ImVec2 s;
            if (!proj(p, s)) { havePrev = false; continue; }
            if (havePrev && hovered)
                bestD[a] = std::min(bestD[a], screenSegDist(mp, prev, s));
            prev = s;
            havePrev = true;
        }
    }
    if (hovered) {
        int best = -1;
        float bd = kGizmoHitPx;
        for (int a = 0; a < 3; ++a) {
            if (bestD[a] <= bd) { bd = bestD[a]; best = a; }
        }
        m_gizmoHoverAxis = best;
    }

    // ---- 拖拽状态机 ----
    if (m_boneDragAxis < 0) {
        if (m_gizmoHoverAxis >= 0 &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const int a = m_gizmoHoverAxis;
            m_boneDragAxis = a;
            m_boneDragStart = glm::vec2(mp.x, mp.y);
            m_boneDragAccum = 0.0f;
            m_boneDragApplied = 0.0f;
            m_boneDragOrig = m_pose.rotation[static_cast<size_t>(j)];
            // 父骨世界旋转：把"绕世界轴转多少"换算成"本骨局部转多少"要用它。
            // 根骨（parent < 0）的父空间就是模型空间 → 单位四元数。
            const int pj = skel.joints[static_cast<size_t>(j)].parent;
            if (pj >= 0 && pj < static_cast<int>(m_worldMats.size())) {
                m_boneDragParentWorld = glm::quat_cast(glm::mat3(
                    glm::mat4(glm::mat3(m_worldMats[static_cast<size_t>(pj)]))));
            } else {
                m_boneDragParentWorld = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            // 切向：屏幕空间里"往上/往右划过环"对应哪个旋转方向。
            // 用环在抓点处的切向而不是"鼠标 dx"—— 后者在正对相机看环时
            // 完全没反应（环投影成一条线）。
            ImVec2 sc;
            glm::vec2 tang(1.0f, 0.0f);
            if (proj(center, sc)) {
                const glm::vec2 rad(mp.x - sc.x, mp.y - sc.y);
                const float l = glm::length(rad);
                if (l > 4.0f) tang = glm::vec2(-rad.y / l, rad.x / l);
            }
            m_boneDragTangent = tang;
            m_gizmoConsumed = true;
            VK_LOG_INFO("CHAR-POSE-GIZMO-BEGIN joint='%s' axis=%s",
                        skel.joints[static_cast<size_t>(j)].name.c_str(),
                        axisName(a));
        }
    } else {
        const int a = m_boneDragAxis;
        // 关掉动画播放，否则下一帧 samplePoseInto 会把旋转算回去
        m_animPlaying = false;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_gizmoConsumed = true;
            const glm::vec2 d(mp.x - m_boneDragStart.x, mp.y - m_boneDragStart.y);
            const float deg = glm::dot(d, m_boneDragTangent) * 0.45f;
            m_boneDragAccum = glm::radians(deg);

            // 绕**世界轴**转 Q，换算到局部：R' = P⁻¹·Q·P·R（P = 父骨世界旋转）。
            // ⚠ 直接 `q * orig` 就是"绕本骨自己的轴转"，感觉上像在拧麻花 ——
            //   这也是大多数手写 gizmo 看起来"方向不对"的原因。
            const glm::vec3 axis = a == 0 ? ax : a == 1 ? ay : az;
            const glm::quat q = glm::angleAxis(m_boneDragAccum, axis);
            const glm::quat delta = glm::conjugate(m_boneDragParentWorld) * q *
                                    m_boneDragParentWorld;
            const size_t k = static_cast<size_t>(j);
            touchBone(j);            // 先把三个通道抄一遍，再只改旋转
            if (k < m_touchedR.size()) m_touchedR[k] = delta * m_boneDragOrig;
        } else {
            VK_LOG_INFO("CHAR-POSE-GIZMO-END joint='%s' axis=%s deg=%.2f",
                        skel.joints[static_cast<size_t>(j)].name.c_str(),
                        axisName(a),
                        static_cast<double>(glm::degrees(m_boneDragAccum)));
            m_boneDragAxis = -1;
        }
    }

    // ---- 画 ----
    for (int a = 0; a < 3; ++a) {
        // ⚠ 这个局部标志叫 `hi` 不叫 `hot`：`hot` 是上面的颜色数组，
        //   同名会把数组遮住，编译报 "invalid types 'const bool[int]'"。
        const bool hi = (a == m_boneDragAxis) ||
                        (m_boneDragAxis < 0 && a == m_gizmoHoverAxis);
        ImVec2 prev;
        bool havePrev = false;
        for (int i = 0; i <= kGizmoSeg; ++i) {
            const float t = 6.28318530718f * static_cast<float>(i) /
                            static_cast<float>(kGizmoSeg);
            const glm::vec3 p =
                center + u[a] * (std::cos(t) * r) + v[a] * (std::sin(t) * r);
            ImVec2 s;
            if (!proj(p, s)) { havePrev = false; continue; }
            if (havePrev) dl->AddLine(prev, s, hi ? hot[a] : base[a],
                                      hi ? 2.6f : 1.6f);
            prev = s;
            havePrev = true;
        }
    }
}

void CharacterWindow::handlePoseKeys() {
    if (!m_poseMode) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;                         // 正在输入资产名
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_I, false)) keySelectedBone(true, true, true);
    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) clearTransient();
}

int CharacterWindow::keyJoint(const std::string& jointName,
                              const animation::Pose& pose, int joint,
                              float time, bool rot, bool loc, bool scale) {
    if (jointName.empty() || joint < 0 || joint >= pose.count()) return 0;
    const size_t k = static_cast<size_t>(joint);
    int n = 0;
    if (rot) {
        const bool ins = m_edit.setKey(
            jointName, assets::AnimTrack::Rotation, time,
            glm::vec4(pose.rotation[k].x, pose.rotation[k].y, pose.rotation[k].z,
                      pose.rotation[k].w));
        VK_LOG_INFO("CHAR-POSE-KEY joint='%s' path=rot t=%.6f new=%d",
                    jointName.c_str(), static_cast<double>(time), ins ? 1 : 0);
        ++n;
    }
    if (loc) {
        const bool ins = m_edit.setKey(
            jointName, assets::AnimTrack::Translation, time,
            glm::vec4(pose.translation[k], 0.0f));
        VK_LOG_INFO("CHAR-POSE-KEY joint='%s' path=loc t=%.6f new=%d",
                    jointName.c_str(), static_cast<double>(time), ins ? 1 : 0);
        ++n;
    }
    if (scale) {
        const bool ins = m_edit.setKey(
            jointName, assets::AnimTrack::Scale, time,
            glm::vec4(pose.scale[k], 0.0f));
        VK_LOG_INFO("CHAR-POSE-KEY joint='%s' path=scale t=%.6f new=%d",
                    jointName.c_str(), static_cast<double>(time), ins ? 1 : 0);
        ++n;
    }
    // 打完帧之后由轨道驱动，瞬态要让位 —— 不然拖时间轴时这根骨会卡在
    // 刚才那个角度上，看起来像"打帧没生效"。
    if (k < m_touched.size()) m_touched[k] = 0;
    m_editDirty = true;
    return n;
}

void CharacterWindow::keySelectedBone(bool rot, bool loc, bool scale) {
    if (!m_model || !m_model->skeleton) return;
    const assets::Skeleton& skel = *m_model->skeleton;
    if (m_selectedJoint < 0 || m_selectedJoint >= skel.jointCount()) return;
    if (m_selectedJoint >= m_pose.count()) return;
    const std::string& nm =
        skel.joints[static_cast<size_t>(m_selectedJoint)].name;
    const int n = keyJoint(nm, m_pose, m_selectedJoint, m_animTime, rot, loc,
                           scale);
    VK_LOG_INFO("CHAR-POSE-KEY-DONE joint='%s' channels=%d keys=%d",
                nm.c_str(), n, m_edit.totalKeys());
}


// ---------------------------------------------------------------- Pose 页

// 摄影表绑定（R8）。每帧调一次：Pose Mode 开着就推数据源，关了就解绑。
// `bind()` 自己只在数据源变了的时候打日志，所以这里不做去重。
void CharacterWindow::syncDopeSheet() {
    if (!m_dope) return;
    if (!m_poseMode) {
        m_dope->unbind();
        return;
    }
    DopeSheetWindow::Host h;
    h.clip = &m_edit;
    h.skel = (m_model && m_model->skeleton) ? m_model->skeleton.get() : nullptr;
    h.head = &m_animTime;
    h.dirty = &m_editDirty;
    // 标题里显示"哪份片段"：优先编辑资产自己的名字，其次文件名，最后模型名。
    // 空串会让标题变成 "Dope sheet —  (live)"，看着像坏掉。
    if (!m_edit.name.empty()) m_dopeLabel = m_edit.name;
    else if (!m_editPath.empty())
        m_dopeLabel = animation::clipNameFromPath(m_editPath);
    else
        m_dopeLabel = m_modelPath.empty() ? std::string("Pose Mode") : m_modelPath;
    h.label = m_dopeLabel.c_str();
    m_dope->bind(h);
}

// ---------------------------------------------------------------- 行走循环生成
//
// 面板按钮与脚本命令 `gait:` **共用这一个入口**。分两份实现的话，
// "脚本能过"就不代表"按钮接了线" —— 而验收要看的是后者。
//
// 它只写 `m_edit`（内存里那份），落盘仍然由「保存」决定。
bool CharacterWindow::generateWalk() {
    const assets::Skeleton* skel =
        (m_model && m_model->skeleton) ? m_model->skeleton.get() : nullptr;
    if (!skel) {
        m_gaitRan = false;
        m_gaitReport = animation::GaitReport{};
        m_gaitReport.error = "no skeleton";
        VK_LOG_WARN("CHAR-GAIT op=generate ok=0 reason=noskeleton");
        return false;
    }

    const animation::GaitReport rep =
        animation::buildWalkCycle(*skel, m_gait, m_edit);
    m_gaitReport = rep;
    m_gaitRan = rep.ok;
    if (rep.ok) {
        m_editDirty = true;
        m_animTime = 0.0f;
        clearTransient();
    } else {
        VK_LOG_WARN("CHAR-GAIT-FAIL why='%s'", rep.error.c_str());
    }
    VK_LOG_INFO("CHAR-GAIT op=generate ok=%d tracks=%d keys=%d "
                "step=%.3f speed=%.3f slide=%.2fmm vert=%.2fmm "
                "mirror=%.2fmm loop=%.2fmm reach=%.3f arm=%.3f",
                rep.ok ? 1 : 0, rep.tracks, rep.keys,
                static_cast<double>(rep.effectiveStep),
                static_cast<double>(rep.nominalSpeed),
                static_cast<double>(rep.stanceSlideMm),
                static_cast<double>(rep.stanceVerticalMm),
                static_cast<double>(rep.mirrorErrMm),
                static_cast<double>(rep.loopErrMm),
                static_cast<double>(rep.reachMargin),
                static_cast<double>(rep.armCounterM));
    return rep.ok;
}

// ============================================================
// 步幅匹配（A4）：量模型里某一段的自然步速
// ============================================================
// 面板读数与脚本 `stride:` 共用 —— 这一点很重要：`Locomotion` 每帧算播放
// 速率用的就是「模型里这段 clip + 同一个 measureClipStride」，所以面板上
// 显示的 0.598 和 Play 里真正用到的必然是同一个数。两处各写一份度量的话，
// "面板说不滑、Play 里却滑"这种偏差根本无从查起。
//
// ⚠ 量的是**模型里的 clip**（骨骼上那段导入/保存好的动画），不是 `m_edit`
//   里还没落盘的编辑片段。要看编辑片段自己的步速，走 `measureEditClipStride`
//   （生成报告里的 `nominalSpeed` 就是它的结果）。
bool CharacterWindow::measureStride(const std::string& clipName) {
    m_strideInfo = animation::StrideInfo{};
    m_strideClip = clipName;

    // 每一条出口都打**同一行**结构化日志（含失败）。
    // ⚠ 失败时只打一行 WARN 是不够的：验收脚本靠 `CHAR-STRIDE clip=...
    //   ok=0` 来判"不存在的 clip 被拒绝了"，而没有这行时"命令没跑"与
    //   "跑了但拒绝"在日志里长得一样。
    auto emit = [&]() {
        VK_LOG_INFO("CHAR-STRIDE clip=%s ok=%d nominal=%.3f cycle=%.2fs "
                    "duty=%.2f air=%.2f samples=%d contact=%d missing=%d "
                    "travel=%.3f/%.3f low=%.3f/%.3f rise=%.3f toe=%d abs=%d "
                    "span=%.3f/%.3f err='%s'",
                    m_strideClip.c_str(), m_strideInfo.ok ? 1 : 0,
                    static_cast<double>(m_strideInfo.nominalSpeed),
                    static_cast<double>(m_strideInfo.cycleSeconds),
                    static_cast<double>(m_strideInfo.dutyFactor),
                    static_cast<double>(m_strideInfo.airborneFraction),
                    m_strideInfo.samples, m_strideInfo.contactSamples,
                    m_strideInfo.missingRoles,
                    static_cast<double>(m_strideInfo.footTravel[0]),
                    static_cast<double>(m_strideInfo.footTravel[1]),
                    static_cast<double>(m_strideInfo.footLow[0]),
                    static_cast<double>(m_strideInfo.footLow[1]),
                    static_cast<double>(m_strideInfo.hipRise),
                    m_strideInfo.soleFromToe ? 1 : 0,
                    m_strideInfo.soleAbsolute ? 1 : 0,
                    static_cast<double>(m_strideInfo.strideSpan),
                    static_cast<double>(m_strideInfo.spanSpeed),
                    m_strideInfo.error.c_str());
    };

    const assets::Skeleton* skel =
        (m_model && m_model->skeleton) ? m_model->skeleton.get() : nullptr;
    if (!skel) {
        m_strideInfo.error = "no skeleton";
        emit();
        VK_LOG_WARN("CHAR-STRIDE-MISS clip='%s' reason=noskeleton",
                    clipName.c_str());
        return false;
    }
    const assets::AnimationClip* clip = skel->findClip(clipName);
    if (!clip) {
        m_strideInfo.error = "clip not found";
        emit();
        VK_LOG_WARN("CHAR-STRIDE-MISS clip='%s' reason=noclip (共 %d 段)",
                    clipName.c_str(), static_cast<int>(skel->clips.size()));
        return false;
    }
    m_strideInfo = animation::measureClipStride(*skel, *clip);
    emit();
    if (!m_strideInfo.ok)
        VK_LOG_WARN("CHAR-STRIDE-MISS clip='%s' why='%s'", clipName.c_str(),
                    m_strideInfo.error.c_str());
    return m_strideInfo.ok;
}

void CharacterWindow::drawPosePanel() {
    const assets::Skeleton* skel =
        (m_model && m_model->skeleton) ? m_model->skeleton.get() : nullptr;
    if (!skel) {
        ImGui::TextDisabled(TR("(no skeleton)"));
        return;
    }

    // ---- 编辑资产（路径 / 存 / 读）----
    ImGui::TextDisabled(TR("EDIT CLIP"));
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("File"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##pose_path", m_editPathBuf, sizeof(m_editPathBuf));
    tip(TR("Where the .anim.json lives. The imported clip is never modified — "
           "this asset only stores the keys you author here."));

    if (ImGui::Button(TR("Save"))) saveEdit(m_editPathBuf);
    logRect("CHAR-POSE-SAVE-BTN", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Load"))) loadEdit(m_editPathBuf);
    logRect("CHAR-POSE-LOAD-BTN", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear all"))) {
        m_edit.tracks.clear();
        m_editDirty = true;
        clearTransient();
        VK_LOG_INFO("CHAR-POSE-CLEAR keys=0");
    }
    tip(TR("Throw away every key in this asset (the file on disk is untouched "
           "until you press Save)"));

    // ---- 摄影表（R8）----
    // 只负责"把那张表调出来"：数据源已经由 syncDopeSheet() 每帧推过去了。
    // 面板显隐的唯一真源是面板表，所以这里直接写表里那一格 —— 不另立开关
    // （两个真相源会让"点了按钮窗口没出来"变成可能）。
    ImGui::SameLine();
    if (ImGui::Button(TR("Dope sheet"))) {
        panelOpenRef("dope") = true;
        VK_LOG_INFO("CHAR-POSE-DOPE open=1");
    }
    logRect("CHAR-POSE-DOPE-BTN", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("Open the dope sheet on this clip — select, drag, mirror and align "
           "the keys you just made. It edits this same in-memory clip."));

    // ---- base clip / 时长 ----
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Base clip"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##pose_base", m_edit.baseClip.empty()
                                            ? TR("(rest pose)")
                                            : m_edit.baseClip.c_str())) {
        if (ImGui::Selectable(TR("(rest pose)"), m_edit.baseClip.empty())) {
            m_edit.baseClip.clear();
            m_editDirty = true;
        }
        // 同样用下标当 ID：这个模型的 clip 名字**会重**（见上方片段列表那段
        // 注释）。名字重复时下拉里会有两条同 ID 的 Selectable，点哪条都跳
        // 到后一条 —— 而且 ImGui 会为它弹冲突告警窗。
        for (std::size_t ci = 0; ci < skel->clips.size(); ++ci) {
            const auto& c = skel->clips[ci];
            ImGui::PushID(static_cast<int>(ci));
            if (ImGui::Selectable(c.name.c_str(), m_edit.baseClip == c.name)) {
                m_edit.baseClip = c.name;
                m_editDirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    tip(TR("Keys are layered on top of this clip. Bones you never key keep "
           "playing it; '(rest pose)' means the unkeyed bones stay still."));

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Duration"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("##pose_dur", &m_edit.duration, 0.01f, 0.01f, 600.0f,
                     TR("%.2fs"));
    ImGui::SameLine();
    ImGui::Checkbox(TR("Loop"), &m_edit.loop);

    // ---- 统计 ----
    ImGui::Spacing();
    ImGui::TextDisabled(TR("STATS"));
    ImGui::Separator();
    ImGui::Text(TR("Tracks"));
    ImGui::SameLine(90.0f);
    ImGui::TextDisabled("%d", static_cast<int>(m_edit.tracks.size()));
    ImGui::Text(TR("Keys"));
    ImGui::SameLine(90.0f);
    ImGui::TextDisabled("%d", m_edit.totalKeys());
    ImGui::Text(TR("Resolved"));
    ImGui::SameLine(90.0f);
    ImGui::TextDisabled("%d / %d", m_editStats.resolvedChannels,
                        static_cast<int>(m_edit.tracks.size()));
    tip(TR("How many of the stored tracks found their bone in this skeleton. "
           "Keys are stored by bone NAME, so a re-import with shifted joint "
           "indices still works."));
    if (!m_editStats.missing.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), "%s",
                           TR("Missing bones"));
        for (const auto& m : m_editStats.missing)
            ImGui::BulletText("%s", m.c_str());
    }

    // ---- 行走循环生成器（2026-10-30）----
    //
    // 为什么生成器要做在引擎里，而不是让人照着 K 帧表手动摆：
    //   表 §6 那条"承重期脚掌 X/Z 完全不动"是**约束**，而关键帧之间的
    //   插值不认约束 —— 手工摆出来的每一帧都对，中间的帧一定滑。这里的
    //   做法反过来：先定脚的世界轨迹（承重期是一条直线、摆动期是抛物线），
    //   再用两骨 IK 回填大腿 / 小腿，最后**逐帧烘焙**。于是"滑没滑"
    //   变成下面那几个毫米数，而不是"看着还行"。
    //
    // ⚠ 它只写 `m_edit`（内存里的那份），落盘仍然走上面的「保存」——
    //   "生成了但没存盘"和别的手工编辑一样能反悔。
    ImGui::Spacing();
    ImGui::TextDisabled(TR("WALK CYCLE"));
    ImGui::Separator();

    // 一行一个参数：左列固定 112px 让输入框对齐（和上面 STATS 的 90px
    // 不同，这里的中文标签更长）。标签走 TextUnformatted(TR(...)) 而不是
    // 当 ImGui 的 ID —— ID 跟着语言变会让"切语言后控件状态丢了"。
    const float kWalkLabelW = 112.0f;
    auto walkRow = [&](const char* id, const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(kWalkLabelW);
        ImGui::SetNextItemWidth(-1.0f);
    };

    walkRow("##gait_step", TR("Step length"));
    ImGui::DragFloat("##gait_step", &m_gait.stepLength, 0.005f, 0.10f, 2.00f,
                     TR("%.3f m"));
    logRect("CHAR-GAIT-STEP", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    tip(TR("Distance one step covers. It gets clamped to what this leg can "
           "reach — the report says so instead of silently sliding."));

    walkRow("##gait_frames", TR("Cycle frames"));
    // 不给格式串：DragInt 的默认就是 "%d"。写一个只为好看的字面量还要
    // 进词典（E 组要求 DragFloat/DragInt 的显示格式串包 TR），不值得。
    ImGui::DragInt("##gait_frames", &m_gait.cycleFrames, 1, 8, 240);
    logRect("CHAR-GAIT-FRAMES", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("A full cycle is TWO steps. The table's 1/7/13/19/25 maps to the "
           "first half, so 48 keeps the frame numbers identical."));

    walkRow("##gait_stance", TR("Stance"));
    ImGui::DragFloat("##gait_stance", &m_gait.stanceFrac, 0.005f, 0.35f, 0.85f);
    logRect("CHAR-GAIT-STANCE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("How much of the cycle one leg carries the weight. 0.62 walks, "
           "0.8 is slow, 0.42 runs."));

    walkRow("##gait_bob", TR("Bob / sway"));
    // 两个数并排：先按剩余宽度算一半，第二个再吃剩下的（`-1`）。
    const float halfW = (ImGui::GetContentRegionAvail().x -
                         ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::SetNextItemWidth(halfW);
    ImGui::DragFloat("##gait_bob", &m_gait.bob, 0.001f, 0.0f, 0.12f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##gait_sway", &m_gait.sway, 0.001f, 0.0f, 0.12f);
    logRect("CHAR-GAIT-BOB", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    tip(TR("Half amplitudes of the body's up/down and left/right travel "
           "(metres). The peak-to-peak is twice these."));

    walkRow("##gait_lift", TR("Foot lift"));
    ImGui::DragFloat("##gait_lift", &m_gait.footLift, 0.002f, 0.0f, 0.40f,
                     TR("%.3f m"));
    logRect("CHAR-GAIT-LIFT", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("Peak toe clearance during the swing phase. Below 2 cm the foot "
           "scrapes the floor."));

    walkRow("##gait_arm", TR("Arm swing"));
    ImGui::DragFloat("##gait_arm", &m_gait.armSwingDeg, 0.5f, 0.0f, 60.0f,
                     TR("%.1f deg"));
    logRect("CHAR-GAIT-ARM", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    tip(TR("Arms swing opposite to the same-side leg and lag 2 frames behind "
           "the shoulders — that lag is what stops it looking robotic."));

    ImGui::Checkbox(TR("Blocking (key poses only)"), &m_gait.blocking);
    logRect("CHAR-GAIT-BLOCKING", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("Only keys the 1/7/13/19/25 poses. Fast to tweak, but the "
           "in-betweens slide — the foot-drift numbers below go up 30x."));

    if (ImGui::Button(TR("Generate walk cycle"),
                      ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
        generateWalk();
    }
    logRect("CHAR-GAIT-BTN", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("Replaces the keys of this edit clip with a baked walk cycle. "
           "Press Save to write it to the .anim.json."));

    if (m_gaitRan) {
        // 读数一律带单位。同一个 0.03 是「半振幅」还是「峰峰」差一倍 ——
        // 面板上不写清楚，读者就会按错的那一半去调参数。
        ImGui::TextDisabled(TR("Speed"));
        ImGui::SameLine(kWalkLabelW);
        ImGui::Text("%.3f m/s", static_cast<double>(m_gaitReport.nominalSpeed));
        // 这三个是这一整块存在的理由：表 §6 的硬规则在数值上就是"约等于 0"。
        ImGui::TextDisabled(TR("Foot drift (stance)"));
        ImGui::TextWrapped(TR("slide %.2f / vertical %.2f / line %.2f mm"),
                           static_cast<double>(m_gaitReport.stanceSlideMm),
                           static_cast<double>(m_gaitReport.stanceVerticalMm),
                           static_cast<double>(m_gaitReport.stanceLineErrMm));
        ImGui::TextWrapped(TR("loop %.2f / mirror %.2f mm"),
                           static_cast<double>(m_gaitReport.loopErrMm),
                           static_cast<double>(m_gaitReport.mirrorErrMm));
        ImGui::TextWrapped(TR("reach %.3f / arm %.3f m"),
                           static_cast<double>(m_gaitReport.reachMargin),
                           static_cast<double>(m_gaitReport.armCounterM));
        if (m_gaitReport.strideLimited) {
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1.0f),
                               TR("Step clamped to %.3f m by leg length"),
                               static_cast<double>(m_gaitReport.effectiveStep));
        }
        if (!m_gaitReport.rig.missing.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), "%s",
                               TR("Bones not found (those parts stay still)"));
            for (const auto& m : m_gaitReport.rig.missing)
                ImGui::BulletText("%s", m.c_str());
        }
    }
    if (!m_gaitRan && !m_gaitReport.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s",
                           m_gaitReport.error.c_str());
    }

    // ---- 步幅匹配（A4）----
    //
    // 这一段的三个数回答同一个问题："这段动画能表达多快的移动速度而不滑步"。
    // 生成器给出 `nominalSpeed` 之后，剩下的接线在这里：开关（Play 里用不用）、
    // 夹范围（变速最多到几倍）、以及**模型里那段 clip 的实测值**（不是
    // 刚生成的编辑片段 —— Play 播的是模型里那段）。
    ImGui::Spacing();
    ImGui::TextDisabled(TR("STRIDE MATCH"));
    ImGui::Separator();

    bool strideOn = m_ctx.strideMatch();
    if (ImGui::Checkbox(TR("Match stride in Play"), &strideOn)) {
        m_ctx.setStrideMatch(strideOn);
        VK_LOG_INFO("CHAR-STRIDE-SET match=%d min=%.2f max=%.2f",
                    strideOn ? 1 : 0, static_cast<double>(m_ctx.strideMinRate()),
                    static_cast<double>(m_ctx.strideMaxRate()));
    }
    logRect("CHAR-STRIDE-MATCH", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("Playback rate = move speed ÷ the clip's own ground speed, so the "
           "feet stay planted. Off = 1x playback, which is the sliding "
           "version — that is what makes it a control group."));

    walkRow("##stride_range", TR("Rate range"));
    float rateLo = m_ctx.strideMinRate();
    float rateHi = m_ctx.strideMaxRate();
    const float halfRateW =
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
        0.5f;
    ImGui::SetNextItemWidth(halfRateW);
    ImGui::DragFloat("##stride_lo", &rateLo, 0.01f, 0.10f, 3.00f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat("##stride_hi", &rateHi, 0.01f, 0.10f, 5.00f);
    if (rateLo > rateHi) std::swap(rateLo, rateHi);   // 拖反了也认（别让速率倒着播）
    if (rateLo != m_ctx.strideMinRate() || rateHi != m_ctx.strideMaxRate())
        m_ctx.setStrideRateRange(rateLo, rateHi);
    logRect("CHAR-STRIDE-RANGE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("Clamping means the clip cannot express that speed: 0.6 is slow "
           "motion, 1.7 is fast forward. Outside it you get residual sliding, "
           "which Play reports as STRIDE-CLAMPED."));

    // A4b 相位速率曲线：常量倍率收掉的是**平均**滑步；承重期脚速在 clip
    // 内本身有起伏，这一层按相位把它压平（接触锁定）。
    // ⚠ 它改的是**播放头轨迹**，不是平均速率 —— 所以面板上单独一行，
    //   不该和上面那个开关合并成一个（合并之后就没法 A/B "是平均没对上
    //   还是分配没对上"）。
    bool curveOn = m_ctx.strideCurve();
    if (ImGui::Checkbox(TR("Phase rate curve (foot lock)"), &curveOn)) {
        m_ctx.setStrideCurve(curveOn);
        VK_LOG_INFO("CHAR-STRIDE-CURVE-SET curve=%d", curveOn ? 1 : 0);
    }
    logRect("CHAR-STRIDE-CURVE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("Keeps the rate constant where the foot is planted: the rest of the "
           "cycle runs at 1x. Off = one constant rate for the whole cycle."));

    if (ImGui::Button(TR("Measure clip stride"),
                      ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
        measureStride("Walk");
    }
    logRect("CHAR-STRIDE-BTN", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("Measures the 'Walk' clip of the model (the one Play uses), not "
           "the edit clip above — those are different objects until you save."));

    if (!m_strideClip.empty()) {
        ImGui::TextDisabled(TR("Clip"));
        ImGui::SameLine(kWalkLabelW);
        if (m_strideInfo.ok) {
            ImGui::Text(TR("%s · %.3f m/s"), m_strideClip.c_str(),
                        static_cast<double>(m_strideInfo.nominalSpeed));
            // 这段动画**能覆盖的移动速度区间** —— 夹范围乘出来的。
            // 玩家的速度落在外面，剩下的就是滑步（数值上是多少，日志里喊）。
            ImGui::TextWrapped(TR("expressible %.2f - %.2f m/s"),
                               static_cast<double>(m_strideInfo.nominalSpeed *
                                                   m_ctx.strideMinRate()),
                               static_cast<double>(m_strideInfo.nominalSpeed *
                                                   m_ctx.strideMaxRate()));
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s",
                               m_strideInfo.error.c_str());
        }
    }

    // ---- 关键帧列表 ----
    ImGui::Spacing();
    ImGui::TextDisabled(TR("KEYS"));
    ImGui::Separator();
    const std::vector<std::string> joints = m_edit.keyedJoints();
    if (joints.empty()) {
        ImGui::TextDisabled(TR("(no keys yet — pick a bone and press I)"));
    }
    for (const auto& jn : joints) {
        ImGui::PushID(jn.c_str());
        const bool isSel =
            (m_selectedJoint >= 0 && m_selectedJoint < skel->jointCount() &&
             skel->joints[static_cast<size_t>(m_selectedJoint)].name == jn);
        if (ImGui::TreeNodeEx("##jn",
                              ImGuiTreeNodeFlags_SpanAvailWidth |
                                  (isSel ? ImGuiTreeNodeFlags_Selected : 0),
                              "%s", jn.c_str())) {
            static const assets::AnimTrack::Path kPaths[3] = {
                assets::AnimTrack::Rotation, assets::AnimTrack::Translation,
                assets::AnimTrack::Scale};
            static const char* kPathNames[3] = {"rot", "loc", "scale"};
            for (int pi = 0; pi < 3; ++pi) {
                const animation::EditTrack* tr = m_edit.track(jn, kPaths[pi]);
                if (!tr) continue;
                ImGui::TextDisabled("%s (%d)", kPathNames[pi], tr->keyCount());
                // 帧表：时间 + 删。用 Selectable 让"点一下跳过去"成立。
                std::vector<int> drop;
                for (int ki = 0; ki < tr->keyCount(); ++ki) {
                    ImGui::PushID(ki);
                    char lbl[64];
                    // ⚠ 格式串要包 TR()（i18n 的 D 组会红），而且**不要**再拼
                    //   `##k` 做 ID：唯一性已经由外面的 PushID(ki) 给了，
                    //   拼进去反而让"这一行显示的是不是时间"变得含糊。
                    std::snprintf(lbl, sizeof(lbl), TR("%.3fs"),
                                  static_cast<double>(tr->times[static_cast<size_t>(ki)]));
                    if (ImGui::Selectable(lbl, false,
                                          ImGuiSelectableFlags_AllowOverlap,
                                          ImVec2(ImGui::GetContentRegionAvail().x -
                                                     24.0f, 0.0f))) {
                        m_animTime = tr->times[static_cast<size_t>(ki)];
                        clearTransient();
                        VK_LOG_INFO("CHAR-POSE-GOTO joint='%s' t=%.6f",
                                    jn.c_str(),
                                    static_cast<double>(m_animTime));
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) drop.push_back(ki);
                    ImGui::PopID();
                }
                // 倒序删：正序删会让后面的下标整体前移，删一个漏一个。
                for (int i = static_cast<int>(drop.size()) - 1; i >= 0; --i) {
                    m_edit.removeKey(jn, kPaths[pi], drop[static_cast<size_t>(i)]);
                    m_editDirty = true;
                    VK_LOG_INFO("CHAR-POSE-KEY-DEL joint='%s' path=%s idx=%d",
                                jn.c_str(), kPathNames[pi],
                                drop[static_cast<size_t>(i)]);
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // ---- 姿态库 ----
    ImGui::Spacing();
    ImGui::TextDisabled(TR("POSE LIBRARY"));
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Name"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##pose_name", m_poseNameBuf, sizeof(m_poseNameBuf));
    if (ImGui::Button(TR("Store"))) {
        const std::string nm = m_poseNameBuf;
        if (!nm.empty() && m_pose.count() > 0) {
            animation::EditPose p = animation::capturePose(m_pose, *skel, nm);
            const bool added = m_edit.storePose(p);
            VK_LOG_INFO("CHAR-POSELIB op=store name='%s' bones=%d new=%d "
                        "total=%d",
                        nm.c_str(), static_cast<int>(p.bones.size()),
                        added ? 1 : 0,
                        static_cast<int>(m_edit.poses.size()));
        }
    }
    logRect("CHAR-POSE-STORE-BTN", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    tip(TR("Save the pose currently on screen under this name (whole body)"));
    ImGui::SameLine();
    if (ImGui::Button(TR("Apply"))) {
        const animation::EditPose* p = m_edit.findPose(m_poseNameBuf);
        if (p) {
            const int n = animation::applyEditPose(*p, *skel, m_pose);
            // 应用到**瞬态层**而不是直接写轨道：姿态库是"摆姿势的起点"，
            // 还要接着转、接着打帧，直接落轨道等于偷偷帮你打了一帧。
            for (const auto& b : p->bones) {
                const int j = skel->findJoint(b.joint,
                                              assets::kJointMatchCaseInsensitive);
                if (j < 0 || j >= m_pose.count()) continue;
                ensureTransient(m_pose.count());
                if (static_cast<size_t>(j) >= m_touched.size()) continue;
                m_touched[static_cast<size_t>(j)] = 1;
                m_touchedT[static_cast<size_t>(j)] = b.translation;
                m_touchedR[static_cast<size_t>(j)] = b.rotation;
                m_touchedS[static_cast<size_t>(j)] = b.scale;
            }
            VK_LOG_INFO("CHAR-POSELIB-APPLIED name='%s' bones=%d", p->name.c_str(),
                        n);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Delete"))) {
        const std::string nm = m_poseNameBuf;
        if (m_edit.removePose(nm))
            VK_LOG_INFO("CHAR-POSELIB op=delete name='%s'", nm.c_str());
    }
    for (const auto& p : m_edit.poses) {
        ImGui::Bullet();
        ImGui::SameLine();
        if (ImGui::SmallButton(p.name.c_str())) {
            std::snprintf(m_poseNameBuf, sizeof(m_poseNameBuf), "%s",
                          p.name.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d", static_cast<int>(p.bones.size()));
    }

}

void CharacterWindow::drawPoseDetails() {
    if (!m_poseMode) return;
    handlePoseKeys();

    const assets::Skeleton* skel =
        (m_model && m_model->skeleton) ? m_model->skeleton.get() : nullptr;
    ImGui::Spacing();
    ImGui::TextDisabled(TR("POSE"));
    ImGui::Separator();
    if (!skel || m_selectedJoint < 0 || m_selectedJoint >= skel->jointCount() ||
        m_selectedJoint >= m_pose.count()) {
        ImGui::TextDisabled(TR("(pick a bone in the Skeleton tab)"));
        return;
    }

    const size_t k = static_cast<size_t>(m_selectedJoint);
    const std::string& nm = skel->joints[k].name;

    // 局部 TRS。改这里 = 写瞬态层（和拖 gizmo 等价），不会自动打帧。
    // ⚠ 每次都是"先 touchBone（把三个通道抄一遍），再改其中一个" ——
    //   顺序反过来就把另外两个通道清成默认值了（见 touchBone 的注释）。

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Pos"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    {
        float p[3] = {m_pose.translation[k].x, m_pose.translation[k].y,
                      m_pose.translation[k].z};
        if (ImGui::DragFloat3("##pose_t", p, 0.001f, -100.0f, 100.0f, "%.4f")) {
            touchBone(m_selectedJoint);
            if (k < m_touchedT.size())
                m_touchedT[k] = glm::vec3(p[0], p[1], p[2]);
        }
    }
    tip(TR("Bone translation relative to its parent (rest offset included)"));

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Rot"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    {
        const glm::vec3 e = glm::degrees(glm::eulerAngles(m_pose.rotation[k]));
        float r[3] = {e.x, e.y, e.z};
        if (ImGui::DragFloat3("##pose_r", r, 0.25f, -360.0f, 360.0f, "%.2f")) {
            touchBone(m_selectedJoint);
            if (k < m_touchedR.size())
                m_touchedR[k] = glm::quat(glm::radians(glm::vec3(r[0], r[1], r[2])));
        }
    }
    tip(TR("Bone rotation in degrees. Same convention as glm::eulerAngles, so "
           "the numbers here and the script API agree."));

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Scale"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    {
        float s[3] = {m_pose.scale[k].x, m_pose.scale[k].y, m_pose.scale[k].z};
        if (ImGui::DragFloat3("##pose_s", s, 0.002f, 0.001f, 10.0f, "%.4f")) {
            touchBone(m_selectedJoint);
            if (k < m_touchedS.size())
                m_touchedS[k] = glm::vec3(s[0], s[1], s[2]);
        }
    }

    ImGui::Spacing();
    if (ImGui::Button(TR("Key (I)"))) keySelectedBone(true, true, true);
    logRect("CHAR-POSE-KEY-BTN", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Rot only"))) keySelectedBone(true, false, false);
    ImGui::SameLine();
    if (ImGui::Button(TR("Loc only"))) keySelectedBone(false, true, false);
    ImGui::SameLine();
    if (ImGui::Button(TR("Scl only"))) keySelectedBone(false, false, true);
    tip(TR("Write the current values as keys at the playhead. Only the bones "
           "you key are stored — everything else keeps following the base "
           "clip."));

    if (ImGui::Button(TR("Revert bone"))) {
        if (k < m_touched.size()) m_touched[k] = 0;
        VK_LOG_INFO("CHAR-POSE-REVERT joint='%s'", nm.c_str());
    }
    tip(TR("Drop the un-keyed changes on this bone and fall back to the keys / "
           "base clip"));

    ImGui::Spacing();
    ImGui::Text(TR("Keys on this bone"));
    ImGui::SameLine();
    {
        int n = 0;
        for (int pi = 0; pi < 3; ++pi) {
            const auto path = pi == 0   ? assets::AnimTrack::Rotation
                              : pi == 1 ? assets::AnimTrack::Translation
                                        : assets::AnimTrack::Scale;
            if (const animation::EditTrack* tr = m_edit.track(nm, path))
                n += tr->keyCount();
        }
        ImGui::TextDisabled("%d", n);
    }
}

// ---------------------------------------------------------------- 存读

bool CharacterWindow::saveEdit(const std::string& path) {
    if (path.empty()) {
        VK_LOG_WARN("CHAR-POSE-SAVE-BLOCKED reason=no-path");
        return false;
    }
    // 资产名跟文件名走：UI 上显示的名字和磁盘上的文件名不一致，
    // 下一次"打开哪个文件"就要靠记忆了。
    const std::string nm = animation::clipNameFromPath(path);
    if (!nm.empty()) m_edit.name = nm;
    std::string err;
    if (!animation::saveEditClip(path, m_edit, err)) {
        VK_LOG_ERROR("CHAR-POSE-SAVE-FAIL path=%s err=%s", path.c_str(),
                     err.c_str());
        return false;
    }
    m_editPath = path;
    std::snprintf(m_editPathBuf, sizeof(m_editPathBuf), "%s", path.c_str());
    return true;
}

bool CharacterWindow::loadEdit(const std::string& path) {
    animation::EditClip c;
    std::string err;
    if (!animation::loadEditClip(path, c, err)) {
        VK_LOG_WARN("CHAR-POSE-LOAD-FAIL path=%s err=%s", path.c_str(),
                    err.c_str());
        return false;
    }
    m_edit = std::move(c);
    m_editPath = path;
    std::snprintf(m_editPathBuf, sizeof(m_editPathBuf), "%s", path.c_str());
    m_editDirty = true;
    // base clip 存在就切过去：读进来的资产应该"一眼看到它自己的样子"，
    // 而不是因为时间轴还停在上一段片段上而显示成别的东西。
    if (!m_edit.baseClip.empty() && m_model && m_model->skeleton &&
        m_model->skeleton->findClip(m_edit.baseClip)) {
        m_currentClip = m_edit.baseClip;
    }
    clearTransient();
    return true;
}

// ---------------------------------------------------------------- 脚本注入

void CharacterWindow::runPoseScript() {
    if (m_scriptRan) return;
    const char* raw = std::getenv("MYVK_EDITOR_POSE_SCRIPT");
    if (!raw || !raw[0]) {
        m_scriptRan = true;
        return;
    }
    // 等骨架：模型是异步加载的，脚本不能跑在"还没有 skeleton"的那一帧。
    if (!m_model || !m_model->skeleton) return;
    m_scriptRan = true;
    const assets::Skeleton& skel = *m_model->skeleton;

    int applied = 0;
    int failed = 0;
    auto split = [](const std::string& s, char sep) {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos <= s.size()) {
            const std::size_t p = s.find(sep, pos);
            out.push_back(s.substr(pos, p == std::string::npos ? std::string::npos
                                                               : p - pos));
            if (p == std::string::npos) break;
            pos = p + 1;
        }
        return out;
    };

    const std::string all(raw);
    std::size_t pos = 0;
    // 骨名里**可能带冒号**（mixamo 全族：`mixamorig:RightArm`），而脚本的
    // 分隔符也是冒号。"名字"这一段不能直接取 `a[1]` —— 必须按**尾部还剩
    // 几段**拼：`setrot:<name>:x:y:z` 从尾部数 3 段是数字，前面全是名字。
    // 2026-10-29 换资产时踩到：`a[1]` 拿到的是 `mixamorig`，9 条命令全部
    // `CHAR-POSE-SCRIPT-BAD`，而报告只说"脚本没跑完"，根因藏在解析里。
    auto jn = [](const std::vector<std::string>& v, std::size_t tail) {
        std::string s = v[1];
        for (std::size_t i = 2; i + tail < v.size(); ++i) s += ":" + v[i];
        return s;
    };
    while (pos <= all.size()) {
        std::size_t semi = all.find(';', pos);
        std::string tok = all.substr(pos, semi == std::string::npos
                                              ? std::string::npos
                                              : semi - pos);
        pos = (semi == std::string::npos) ? all.size() + 1 : semi + 1;
        // 去空白
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.pop_back();
        if (tok.empty()) continue;

        const std::vector<std::string> a = split(tok, ':');
        const std::string& cmd = a[0];
        bool ok = false;

        if (cmd == "list") {
            // 把骨架的骨名全表打出来。验收脚本靠它**发现**这根模型上到底
            // 有哪些骨，而不是把 `mixamorig:LeftArm` 写死在测试里 ——
            // 换个模型就得改测试的用例不是验收，是负债。
            VK_LOG_INFO("CHAR-POSE-JOINTS n=%d", skel.jointCount());
            for (int i = 0; i < skel.jointCount(); ++i) {
                VK_LOG_INFO("CHAR-POSE-JOINT i=%d name=%s parent=%d", i,
                            skel.joints[static_cast<size_t>(i)].name.c_str(),
                            skel.joints[static_cast<size_t>(i)].parent);
            }
            ok = true;
        } else if (cmd == "mode" && a.size() >= 2) {
            const bool on = (a[1] == "on" || a[1] == "1");
            if (on && !m_poseMode) {
                m_poseMode = true;
                if (m_edit.baseClip.empty()) m_edit.baseClip = m_currentClip;
                if (m_edit.name.empty())
                    m_edit.name = m_currentClip.empty() ? std::string("custom")
                                                        : m_currentClip;
                m_editDirty = true;
                m_focusPoseTab = true;
            } else if (!on) {
                m_poseMode = false;
            }
            VK_LOG_INFO("CHAR-POSE-MODE on=%d base='%s' tracks=%d keys=%d "
                        "path=%s",
                        m_poseMode ? 1 : 0, m_edit.baseClip.c_str(),
                        static_cast<int>(m_edit.tracks.size()), m_edit.totalKeys(),
                        m_editPath.c_str());
            ok = true;
        } else if (cmd == "select" && a.size() >= 2) {
            const std::string nm = jn(a, 0);
            const int j = skel.findJoint(nm, assets::kJointMatchCaseInsensitive);
            if (j >= 0) {
                m_selectedJoint = j;
                VK_LOG_INFO("CHAR-POSE-SELECT joint='%s' index=%d",
                            skel.joints[static_cast<size_t>(j)].name.c_str(), j);
                ok = true;
            } else {
                VK_LOG_WARN("CHAR-POSE-SELECT-MISS name='%s'", nm.c_str());
            }
        } else if ((cmd == "setrot" || cmd == "setloc" || cmd == "setscale") &&
                   a.size() >= 5) {
            const std::string nm = jn(a, 3);
            const int j = skel.findJoint(nm, assets::kJointMatchCaseInsensitive);
            if (j >= 0 && j < m_pose.count()) {
                touchBone(j);   // 先把三个通道抄一遍，再只改其中一个
                const size_t k = static_cast<size_t>(j);
                // 三个数字取**尾部**三段（原因见上面的 `jn`）。
                const float x = std::strtof(a[a.size() - 3].c_str(), nullptr);
                const float y = std::strtof(a[a.size() - 2].c_str(), nullptr);
                const float z = std::strtof(a.back().c_str(), nullptr);
                if (cmd == "setrot") {
                    m_touchedR[k] = glm::quat(glm::radians(glm::vec3(x, y, z)));
                } else if (cmd == "setloc") {
                    m_touchedT[k] = glm::vec3(x, y, z);
                } else {
                    m_touchedS[k] = glm::vec3(x, y, z);
                }
                ok = true;
            }
        } else if (cmd == "key" && a.size() >= 4) {
            const std::string nm = jn(a, 2);
            const int j = skel.findJoint(nm, assets::kJointMatchCaseInsensitive);
            const std::string what = a[a.size() - 2];
            const float t = std::strtof(a.back().c_str(), nullptr);
            if (j >= 0) {
                m_animTime = t;
                samplePoseInto();
                const bool rot = (what == "rot" || what == "all");
                const bool loc = (what == "loc" || what == "all");
                const bool scl = (what == "scale" || what == "all");
                keyJoint(skel.joints[static_cast<size_t>(j)].name, m_pose, j, t,
                         rot, loc, scl);
                ok = true;
            }
        } else if (cmd == "sample" && a.size() >= 2) {
            m_animTime = std::strtof(a[1].c_str(), nullptr);
            samplePoseInto();
            logPoseDigest("script");
            ok = true;
        } else if (cmd == "dump" && a.size() >= 2) {
            const std::string nm = jn(a, 0);
            const int j = skel.findJoint(nm, assets::kJointMatchCaseInsensitive);
            if (j >= 0 && j < m_pose.count()) {
                const size_t k = static_cast<size_t>(j);
                // 世界位置也打出来：局部 TRS 不变**不**代表外观没变 ——
                // 父骨被改过的话子骨的局部不变、世界会跟着动。验收里那条
                // "打帧不波及另一根骨"要同时钉住两样才成立。
                glm::vec3 w(0.0f);
                const bool haveW = (j < static_cast<int>(m_worldMats.size()));
                if (haveW) w = glm::vec3(m_worldMats[k][3]);
                // %a = 精确十六进制浮点：跨进程比对时逐位可比，不受
                // printf 的十进制舍入影响。这是 A 组"存盘→重开→逐位一致"
                // 能做成断言的前提。
                VK_LOG_INFO("CHAR-POSE-DUMP joint='%s' t=%a,%a,%a "
                            "r=%a,%a,%a,%a s=%a,%a,%a w=%a,%a,%a",
                            skel.joints[k].name.c_str(),
                            static_cast<double>(m_pose.translation[k].x),
                            static_cast<double>(m_pose.translation[k].y),
                            static_cast<double>(m_pose.translation[k].z),
                            static_cast<double>(m_pose.rotation[k].x),
                            static_cast<double>(m_pose.rotation[k].y),
                            static_cast<double>(m_pose.rotation[k].z),
                            static_cast<double>(m_pose.rotation[k].w),
                            static_cast<double>(m_pose.scale[k].x),
                            static_cast<double>(m_pose.scale[k].y),
                            static_cast<double>(m_pose.scale[k].z),
                            static_cast<double>(w.x), static_cast<double>(w.y),
                            static_cast<double>(w.z));
                ok = true;
            }
        } else if (cmd == "posestore" && a.size() >= 2) {
            const std::string pn = jn(a, 0);
            if (m_pose.count() > 0) {
                animation::EditPose p = animation::capturePose(m_pose, skel, pn);
                const bool added = m_edit.storePose(p);
                VK_LOG_INFO("CHAR-POSELIB op=store name='%s' bones=%d new=%d "
                            "total=%d", pn.c_str(),
                            static_cast<int>(p.bones.size()), added ? 1 : 0,
                            static_cast<int>(m_edit.poses.size()));
                ok = true;
            }
        } else if (cmd == "poseapply" && a.size() >= 2) {
            const std::string pn = jn(a, 0);
            const animation::EditPose* p = m_edit.findPose(pn);
            if (p) {
                const int n = animation::applyEditPose(*p, skel, m_pose);
                for (const auto& b : p->bones) {
                    const int j = skel.findJoint(
                        b.joint, assets::kJointMatchCaseInsensitive);
                    if (j < 0 || j >= m_pose.count()) continue;
                    ensureTransient(m_pose.count());
                    if (static_cast<size_t>(j) >= m_touched.size()) continue;
                    m_touched[static_cast<size_t>(j)] = 1;
                    m_touchedT[static_cast<size_t>(j)] = b.translation;
                    m_touchedR[static_cast<size_t>(j)] = b.rotation;
                    m_touchedS[static_cast<size_t>(j)] = b.scale;
                }
                VK_LOG_INFO("CHAR-POSELIB-APPLIED name='%s' bones=%d",
                            p->name.c_str(), n);
                ok = true;
            }
        } else if (cmd == "gait") {
            // `gait[:step[:frames[:blocking]]]` —— 生成器的**无头入口**。
            // 走的是和面板上那个按钮同一个函数，所以"UI 真的接上了线"这件事
            // 可以被机器验证（否则脚本能过、按钮却没接线，是两回事）。
            if (a.size() >= 2)
                m_gait.stepLength = std::strtof(a[1].c_str(), nullptr);
            if (a.size() >= 3)
                m_gait.cycleFrames =
                    static_cast<int>(std::strtof(a[2].c_str(), nullptr));
            if (a.size() >= 4) m_gait.blocking = (a[3] != "0");
            ok = generateWalk();
        } else if (cmd == "stride") {
            // `stride[:clipName]` —— 步幅匹配的**无头入口**（A4）。
            // 量的是**模型里**那段 clip（Play 会播的就是它），所以脚本能验
            // "生成器说的步速"和"运行时量到的步速"是同一条路径上的数。
            // 不给名字时量 "Walk"（LocomotionConfig 的默认走路段名）。
            const std::string clipName = (a.size() >= 2) ? a[1] : "Walk";
            ok = measureStride(clipName);
        } else if (cmd == "stridecurve") {
            // `stridecurve[:0|1]` —— A4b 相位速率曲线的开关（缺省 = 打开）。
            // 与面板上那个复选框是**同一个** setter：脚本能开，界面也能开，
            // 两条路各写一次状态的话，"脚本开了但面板显示关"会变成
            // 一次盲查（而它只影响播放头轨迹，肉眼完全看不出来）。
            const bool on = (a.size() < 2) ? true : (a[1] != "0");
            m_ctx.setStrideCurve(on);
            VK_LOG_INFO("CHAR-STRIDE-CURVE-SET curve=%d", on ? 1 : 0);
            ok = true;
        } else if (cmd == "save" && a.size() >= 2) {
            // 路径里可能带盘符冒号（C:\...），所以从第 2 段起全部拼回去。
            std::string p = a[1];
            for (std::size_t i = 2; i < a.size(); ++i) p += ":" + a[i];
            ok = saveEdit(p);
        } else if (cmd == "load" && a.size() >= 2) {
            std::string p = a[1];
            for (std::size_t i = 2; i < a.size(); ++i) p += ":" + a[i];
            ok = loadEdit(p);
        } else if (cmd == "clear") {
            m_edit.tracks.clear();
            m_editDirty = true;
            clearTransient();
            VK_LOG_INFO("CHAR-POSE-CLEAR keys=0");
            ok = true;
        }

        if (ok) ++applied;
        else {
            ++failed;
            VK_LOG_WARN("CHAR-POSE-SCRIPT-BAD tok='%s'", tok.c_str());
        }
    }
    VK_LOG_INFO("CHAR-POSE-SCRIPT applied=%d failed=%d tracks=%d keys=%d "
                "poses=%d", applied, failed,
                static_cast<int>(m_edit.tracks.size()), m_edit.totalKeys(),
                static_cast<int>(m_edit.poses.size()));
}

void CharacterWindow::logPoseDigest(const char* tag) {
    // FNV-1a over 关节矩阵的**原始字节**。
    //
    // 为什么用字节摘要而不是"把位置加起来比"：求和会让两个不同的姿态
    // 撞出同一个值（对称的左右臂互换正好抵消）。验收要的是"逐位一致"，
    // 那就只能对着字节算。碰撞概率对一次断言来说可以忽略。
    unsigned long long h = 1469598103934665603ull;
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(m_jointMats.data());
    const std::size_t n = m_jointMats.size() * sizeof(glm::mat4);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned long long>(p[i]);
        h *= 1099511628211ull;
    }
    VK_LOG_INFO("CHAR-POSE-DIGEST tag=%s t=%.6f joints=%d bytes=%d fnv=%016llx",
                tag ? tag : "-", static_cast<double>(m_animTime),
                static_cast<int>(m_jointMats.size()), static_cast<int>(n), h);
}

// ============================================================
// Rig 页（S-A，2026-10-29）—— 骨架编辑
//
// 这一页只做一件事：把 `animation::RigEdit` 摆出来，让作者能改
// head / length / roll / deform / connected / inherit。
//
// 三条纪律（和 Dope Sheet 那边同源）：
//   ① 改数据只走 `RigEdit::setXxx` 一族 —— 注入脚本与 UI 共用同一份实现，
//      验收因此不依赖"把某个 DragFloat 拖到正确位置"；
//   ② 烘焙**只**在 `m_rigDirty` 时做（拖滑条一帧一次就够，不要每个控件各烤一遍）；
//   ③ `assets::Skeleton` 一个字节不改：`m_rigBase` 握着导入骨架，
//      `applyRig` 生成**新的** Skeleton 换上 —— 改坏了 `Revert` 就回去。
// ============================================================

namespace {
constexpr float kRigRad2Deg = 57.29577951308232f;
constexpr float kRigDeg2Rad = 0.017453292519943295f;
} // namespace

void CharacterWindow::resetRigToCurrent() {
    m_rigActive = false;
    m_rigSel = -1;
    m_rigWorld.clear();
    m_rigSkel = assets::Skeleton{};
    m_rig.bones.clear();
    m_rigDirty = false;
}

void CharacterWindow::ensureRig() {
    if (!m_model || !m_model->skeleton) return;
    // 换了模型（或从没导入过）就以**当前**骨架为基准重新导入。
    // 用 `m_rig.source` 而不是另立一个"上次的模型路径"：它本来就是干这个的。
    if (m_rigActive && m_rig.source == m_modelPath) return;
    resetRigToCurrent();

    m_rigBase = m_model->skeleton;
    m_rig = animation::RigEdit::fromSkeleton(*m_rigBase, m_modelPath);
    m_rigActive = true;
    m_rigSel = m_rig.boneCount() > 0 ? 0 : -1;
    m_rigDirty = true;

    if (m_rigPathBuf[0] == 0) {
        std::string p = m_modelPath.empty() ? std::string("character") : m_modelPath;
        const std::size_t dot = p.find_last_of('.');
        const std::size_t slash = p.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            p = p.substr(0, dot);
        p += ".rig.json";
        std::snprintf(m_rigPathBuf, sizeof(m_rigPathBuf), "%s", p.c_str());
    }
    m_rig.logSummary("import");
    VK_LOG_INFO("CHAR-RIG-IMPORT bones=%d model='%s'", m_rig.boneCount(),
                m_modelPath.c_str());
}

void CharacterWindow::applyRig() {
    if (!m_rigActive || !m_rigBase || !m_model) return;
    assets::Skeleton baked;
    // 以**导入骨架**为基准烘焙：没动过的骨从它逐位照抄（约定 ①）。
    // 拿上一次烘焙结果当基准会一代代漂 —— 每烤一次多一次舍入。
    m_rig.bake(*m_rigBase, baked);
    m_rigSkel = std::move(baked);
    m_rig.restWorld(m_rigWorld);
    m_model->skeleton = std::make_shared<assets::Skeleton>(m_rigSkel);
    m_rigDirty = false;
    // 加/删骨之后按名字解析的下标全变了，必须重解（不然帧会搬到别的骨上）
    m_editDirty = true;
    revalidateEdit();
    VK_LOG_INFO("CHAR-RIG-APPLY bones=%d touched=%d", m_rigSkel.jointCount(),
                m_rig.info().touched);
}

void CharacterWindow::revertRig() {
    if (!m_rigBase) return;
    VK_LOG_INFO("CHAR-RIG-REVERT bones=%d", m_rigBase->jointCount());
    m_model->skeleton = m_rigBase;
    m_rigActive = false;
    ensureRig();   // 重新从导入骨架导入（等价于"丢掉所有编辑"）
}

void CharacterWindow::drawRigPanel() {
    if (!m_model || !m_model->skeleton) {
        ImGui::TextDisabled(TR("(no skeleton)"));
        return;
    }
    ensureRig();

    const animation::RigInfo st = m_rig.info();
    ImGui::TextDisabled(TR("EDIT RIG"));
    ImGui::Separator();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("File"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##rig_path", m_rigPathBuf, sizeof(m_rigPathBuf));
    tip(TR("Where the .rig.json lives. The imported skeleton is never modified."));

    if (ImGui::Button(TR("Save"))) {
        std::string err;
        const bool ok = animation::saveRig(m_rigPathBuf, m_rig, err);
        if (!ok) VK_LOG_WARN("CHAR-RIG-SAVE-FAIL err='%s'", err.c_str());
    }
    logRect("CHAR-RIG-SAVE-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Load"))) {
        animation::RigEdit tmp;
        std::string err;
        if (animation::loadRig(m_rigPathBuf, tmp, err)) {
            // 只接受**同一副骨架**：骨名对不上的 rig 换上去，等于把角色换成
            // 别人的骨架，模型会散成碎片而日志只有一行 SAVE-OK。
            const bool sameRig = tmp.boneCount() == m_rig.boneCount();
            if (sameRig) {
                m_rig = std::move(tmp);
                m_rig.source = m_modelPath;
                m_rigSel = m_rig.boneCount() > 0 ? 0 : -1;
                m_rigDirty = true;
            } else {
                VK_LOG_WARN("CHAR-RIG-LOAD-vs bones=%d vs %d（骨数不同，拒绝）",
                            tmp.boneCount(), m_rig.boneCount());
            }
        } else {
            VK_LOG_WARN("CHAR-RIG-LOAD-FAIL err='%s'", err.c_str());
        }
    }
    logRect("CHAR-RIG-LOAD-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Revert"))) revertRig();
    logRect("CHAR-RIG-REVERT-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::Checkbox(TR("Show bones"), &m_showRigBones);
    // 镜像轴：默认从骨名+位置认出来，允许覆盖（X/Y/Z 是轴名，不是人话，不翻）
    const char* kAxes[] = {"X", "Y", "Z"};
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::Combo("##rig_axis", &m_rig.mirrorAxis, kAxes, 3)) {
        VK_LOG_INFO("CHAR-RIG-AXIS axis=%s",
                    animation::RigEdit::axisName(m_rig.mirrorAxis));
    }
    tip(TR("Mirror plane. Detected from the left/right bone pairs — override it if "
           "the mirrored side comes out twisted."));
    ImGui::Text("%s %d  %s %d", TR("bones"), st.bones, TR("touched"), st.touched);
    logRect("CHAR-RIG-STATS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 骨树 ----
    ImGui::Separator();
    const float treeH = std::max(90.0f, ImGui::GetContentRegionAvail().y * 0.42f);
    ImGui::BeginChild("##rig_tree", ImVec2(0.0f, treeH), ImGuiChildFlags_Borders);
    std::function<void(int, int)> drawNode;
    drawNode = [&](int i, int depth) {
        if (i < 0 || i >= m_rig.boneCount()) return;
        const animation::EditBone& b = m_rig.bones[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        if (depth > 0) ImGui::Indent(10.0f);
        // `*` = 动过（与 Details 的"已改"角标同一口径）
        char label[192];
        std::snprintf(label, sizeof(label), "%s%s", b.touched() ? "* " : "",
                      b.name.c_str());
        if (ImGui::Selectable(label, m_rigSel == i)) m_rigSel = i;
        if (!b.deform) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", TR("no deform"));
        }
        std::vector<int> kids;
        m_rig.childrenOf(i, kids);
        for (int c : kids) drawNode(c, depth + 1);
        if (depth > 0) ImGui::Unindent(10.0f);
        ImGui::PopID();
    };
    for (int i = 0; i < m_rig.boneCount(); ++i)
        if (m_rig.bones[static_cast<std::size_t>(i)].parent < 0) drawNode(i, 0);
    ImGui::EndChild();

    // ---- 选中骨 ----
    if (m_rigSel < 0 || m_rigSel >= m_rig.boneCount()) return;
    // ⚠ 这里**不能**缓存引用：下面的 Add / Delete 会让 bones 重新分配，
    //   之后再读 `b` 就是悬空引用（症状是"改完加骨，界面上一堆随机名字"）。
    {
        const animation::EditBone& cb = m_rig.bones[static_cast<std::size_t>(m_rigSel)];
        ImGui::Text("%s: %s", TR("Selected bone"), cb.name.c_str());
    }

    float head[3] = {m_rig.bones[static_cast<std::size_t>(m_rigSel)].head.x,
                     m_rig.bones[static_cast<std::size_t>(m_rigSel)].head.y,
                     m_rig.bones[static_cast<std::size_t>(m_rigSel)].head.z};
    if (ImGui::DragFloat3(TR("Head"), head, 0.005f)) {
        m_rig.setHead(m_rigSel, glm::vec3(head[0], head[1], head[2]));
        m_rigDirty = true;
    }
    logRect("CHAR-RIG-HEAD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    tip(TR("Where this bone's head sits in skeleton space. Connected children "
           "follow it."));

    float len = m_rig.bones[static_cast<std::size_t>(m_rigSel)].length;
    if (ImGui::DragFloat(TR("Length"), &len, 0.002f, 0.001f, 20.0f, "%.4f")) {
        m_rig.setLength(m_rigSel, len);
        m_rigDirty = true;
    }
    logRect("CHAR-RIG-LEN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    float rollDeg = m_rig.bones[static_cast<std::size_t>(m_rigSel)].roll * kRigRad2Deg;
    if (ImGui::DragFloat(TR("Roll (deg)"), &rollDeg, 0.5f, -360.0f, 360.0f, "%.2f")) {
        m_rig.setRoll(m_rigSel, rollDeg * kRigDeg2Rad);
        m_rigDirty = true;
    }
    logRect("CHAR-RIG-ROLL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Reset"))) {
        m_rig.setRoll(m_rigSel, 0.0f);
        m_rigDirty = true;
    }
    tip(TR("Twist about the bone's own axis. 0 = keep the imported orientation — "
           "so 'only fix the hand' never re-orients the whole skeleton."));

    bool df = m_rig.bones[static_cast<std::size_t>(m_rigSel)].deform;
    if (ImGui::Checkbox(TR("Deform"), &df)) {
        m_rig.setDeform(m_rigSel, df);
        m_rigDirty = true;
    }
    ImGui::SameLine();
    bool cn = m_rig.bones[static_cast<std::size_t>(m_rigSel)].connected;
    if (ImGui::Checkbox(TR("Connected"), &cn)) {
        m_rig.setConnected(m_rigSel, cn);
        m_rigDirty = true;
    }
    ImGui::SameLine();
    bool ih = m_rig.bones[static_cast<std::size_t>(m_rigSel)].inheritRot;
    if (ImGui::Checkbox(TR("Inherit rotation"), &ih)) {
        m_rig.setInheritRot(m_rigSel, ih);
        m_rigDirty = true;
    }
    logRect("CHAR-RIG-FLAGS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    if (ImGui::Button(TR("Align to child"))) {
        std::vector<int> kids;
        m_rig.childrenOf(m_rigSel, kids);
        if (!kids.empty()) {
            const glm::vec3 t =
                m_rig.bones[static_cast<std::size_t>(kids.front())].head;
            const glm::vec3 h = m_rig.bones[static_cast<std::size_t>(m_rigSel)].head;
            const float d = glm::length(t - h);
            if (m_rig.setDir(m_rigSel, t - h) && d > 1.0e-4f)
                m_rig.setLength(m_rigSel, d);
            m_rigDirty = true;
        } else {
            VK_LOG_WARN("CHAR-RIG-ALIGN 选中骨没有子骨（'%s'）",
                        m_rig.bones[static_cast<std::size_t>(m_rigSel)].name.c_str());
        }
    }
    logRect("CHAR-RIG-ALIGN-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Mirror to opposite"))) {
        m_rig.mirrorToOpposite(m_rigSel);
        m_rigDirty = true;
    }
    logRect("CHAR-RIG-MIRROR-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Add child bone"))) {
        const animation::EditBone& cb = m_rig.bones[static_cast<std::size_t>(m_rigSel)];
        const glm::vec3 at = cb.tail();
        const glm::vec3 d = cb.dir;
        const int par = m_rigSel;
        std::string nm = cb.name + "_child";
        for (int k = 2; m_rig.indexOf(nm) >= 0 && k < 100; ++k)
            nm = cb.name + "_child" + std::to_string(k);
        const int idx = m_rig.addBone(nm, par, at, d, 0.1f);
        if (idx >= 0) {
            m_rigSel = idx;
            m_rigDirty = true;
        }
    }
    logRect("CHAR-RIG-ADD-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Delete bone"))) {
        const int gone = m_rigSel;
        const int n = m_rig.removeBone(gone);
        if (n > 0) {
            m_rigSel = m_rig.boneCount() > 0 ? std::min(gone, m_rig.boneCount() - 1) : -1;
            m_rigDirty = true;
        }
    }
    logRect("CHAR-RIG-DEL-BTN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // 约束栈（S-C）跟着选中骨走 —— 它是"这根骨的属性"，和 Head/Roll 一样，
    // 所以放在 Rig 页里而不是再开一个页签：左栏 380px 已经装不下第 6 个页签了
    // （见 draw() 里 leftW 那段注释）。
    drawConstraintsSection();

    if (m_rigDirty) applyRig();
}

// ============================================================
// 约束栈（S-C）
// ============================================================

int CharacterWindow::constraintBoneOfSel() const {
    if (!m_model || !m_model->skeleton) return -1;
    if (m_rigSel < 0 || m_rigSel >= m_rig.boneCount()) return -1;
    const animation::EditBone& b = m_rig.bones[static_cast<std::size_t>(m_rigSel)];
    const int n = m_model->skeleton->jointCount();
    // `srcJoint` = 导入时的 joint 下标；Rig 页新加的骨没有它（-1），但
    // `RigEdit::bake()` 之后 bones[i] → joints[i]，所以退回 bones 下标是对的。
    const int j = b.srcJoint >= 0 ? b.srcJoint : m_rigSel;
    return (j >= 0 && j < n) ? j : -1;
}

void CharacterWindow::pruneConstraints() {
    if (!m_model || !m_model->skeleton) return;
    const int n = m_model->skeleton->jointCount();
    if (n == m_conBoundJoints) return;
    m_conBoundJoints = n;
    int dropped = 0;
    for (int i = m_constraints.size() - 1; i >= 0; --i) {
        const animation::Constraint* c = m_constraints.at(i);
        if (!c) continue;
        bool bad = (c->bone < 0 || c->bone >= n) || (c->target >= n);
        if (c->type == animation::kConTwoBoneIK) {
            bad = bad || c->ikRoot >= n || c->ikMid >= n || c->ikEnd >= n;
        }
        if (!bad) continue;
        m_constraints.removeAt(i);
        if (m_conSel == i) m_conSel = -1;
        ++dropped;
    }
    if (dropped > 0) {
        VK_LOG_WARN("CONSTRAINT-PRUNE joints=%d dropped=%d left=%d", n, dropped,
                    m_constraints.size());
    }
}

void CharacterWindow::drawConstraintTargetCombo(const char* id, int& joint,
                                               float width) {
    if (!m_model || !m_model->skeleton) return;
    const assets::Skeleton& skel = *m_model->skeleton;
    const int n = skel.jointCount();
    const char* cur = (joint >= 0 && joint < n)
                          ? skel.joints[static_cast<std::size_t>(joint)].name.c_str()
                          : TR("(none)");
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(id, cur)) {
        if (ImGui::Selectable(TR("(none)"), joint < 0)) {
            joint = -1;
            m_conDirty = true;
        }
        for (int j = 0; j < n; ++j) {
            ImGui::PushID(j);
            const bool sel = (j == joint);
            if (ImGui::Selectable(
                    skel.joints[static_cast<std::size_t>(j)].name.c_str(), sel)) {
                joint = j;
                m_conDirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void CharacterWindow::drawConstraintsSection() {
    if (!m_model || !m_model->skeleton) return;
    const assets::Skeleton& skel = *m_model->skeleton;
    const int joints = skel.jointCount();
    pruneConstraints();

    ImGui::Separator();
    // 折叠段：Rig 页本来就长（骨树 + 十来个控件），约束再铺开就顶到可见区
    // 外面去了（实测默认窗口下锚点高度是 0）。有约束时默认展开，"有没有东西"
    // 这件事一眼可见；空栈时收成一行，不占位置。
    if (!m_constraints.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    const bool conOpen = ImGui::CollapsingHeader(TR("CONSTRAINTS"));
    logRect("CHAR-CON-TITLE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!conOpen) return;

    // 整个栈的账（不是只这一根骨的）：作者要知道"有没有条目在悄悄失败"。
    ImGui::Text(TR("stack %d: applied %d / skipped %d / failed %d"),
                m_constraints.size(), m_conStats.applied, m_conStats.skipped,
                m_conStats.failed);
    logRect("CHAR-CON-STATS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    const int bone = constraintBoneOfSel();
    if (bone < 0) {
        ImGui::TextDisabled(TR("(this bone has no joint yet — save the rig first)"));
        logRect("CHAR-CON-NOBONE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        return;
    }

    // ---- 这一根骨的条目（**保持全局顺序**：跨骨顺序也参与求值）----
    if (ImGui::BeginChild("##con_list", ImVec2(0.0f, 96.0f), ImGuiChildFlags_Borders)) {
        bool any = false;
        for (int i = 0; i < m_constraints.size(); ++i) {
            const animation::Constraint* c = m_constraints.at(i);
            if (!c || c->bone != bone) continue;
            any = true;
            char label[160];
            std::snprintf(label, sizeof(label), "%d. %s  (%.2f)", i,
                          animation::constraintTypeName(c->type),
                          static_cast<double>(c->influence));
            ImGui::PushID(i);
            bool en = c->enabled;
            if (ImGui::Checkbox("##en", &en)) {
                m_constraints.items[static_cast<std::size_t>(i)].enabled = en;
                m_conDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::Selectable(label, m_conSel == i)) m_conSel = i;
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled(TR("(no constraint on this bone)"));
        ImGui::EndChild();
    }
    logRect("CHAR-CON-LIST", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 增删移 ----
    static int sNewType = animation::kConLimitRotation;
    const char* kTypes[animation::kConTypeCount] = {
        animation::constraintTypeName(animation::kConLimitRotation),
        animation::constraintTypeName(animation::kConCopyRotation),
        animation::constraintTypeName(animation::kConCopyLocation),
        animation::constraintTypeName(animation::kConChildOf),
        animation::constraintTypeName(animation::kConTwoBoneIK),
    };
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("##con_newtype", &sNewType, kTypes, animation::kConTypeCount);
    logRect("CHAR-CON-TYPE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Add"))) {
        animation::Constraint c;
        c.type = sNewType;
        c.bone = bone;
        if (sNewType == animation::kConTwoBoneIK) {
            // 链从选中骨开始：root = 选中骨，mid/end 先留空（让 solver 自动挑
            // 最长子骨），这样"加一条 IK 就能动"是默认体验。
            c.ikRoot = bone;
            c.ikMid = -1;
            c.ikEnd = -1;
            c.ikHasTarget = true;
            c.ikTarget = glm::vec3(0.0f);
        }
        m_conSel = m_constraints.add(c);
        m_conDirty = true;
    }
    logRect("CHAR-CON-ADD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Delete"))) {
        if (m_constraints.removeAt(m_conSel) > 0) {
            m_conSel = -1;
            m_conDirty = true;
        }
    }
    logRect("CHAR-CON-DEL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Move up"))) {
        if (m_constraints.moveUp(m_conSel) > 0) {
            --m_conSel;
            m_conDirty = true;
        }
    }
    logRect("CHAR-CON-UP", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Move down"))) {
        if (m_constraints.moveDown(m_conSel) > 0) {
            ++m_conSel;
            m_conDirty = true;
        }
    }
    logRect("CHAR-CON-DOWN", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 选中条目的参数 ----
    animation::Constraint* c = m_constraints.at(m_conSel);
    if (!c || c->bone != bone) return;

    if (ImGui::SliderFloat(TR("Influence"), &c->influence, 0.0f, 1.0f)) m_conDirty = true;
    logRect("CHAR-CON-INFL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    tip(TR("Influence can be keyed: 0 → 1 over two frames is how FK switches to IK."));

    switch (c->type) {
    case animation::kConLimitRotation: {
        for (int a = 0; a < 3; ++a) {
            bool use = (c->limitMask & (1 << a)) != 0;
            ImGui::PushID(a);
            if (ImGui::Checkbox("##use", &use)) {
                c->limitMask ^= (1 << a);
                m_conDirty = true;
            }
            ImGui::SameLine();
            float mm[2] = {c->minDeg[a], c->maxDeg[a]};
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::DragFloat2(TR("Min / Max"), mm, 0.5f, -360.0f, 360.0f,
                                  "%.1f")) {
                c->minDeg[a] = mm[0];
                c->maxDeg[a] = mm[1];
                m_conDirty = true;
            }
            ImGui::PopID();
        }
        logRect("CHAR-CON-LIMIT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        break;
    }
    case animation::kConCopyRotation:
    case animation::kConChildOf: {
        ImGui::TextUnformatted(TR("Target bone"));
        drawConstraintTargetCombo("##con_tgt", c->target, 180.0f);
        logRect("CHAR-CON-TARGET", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (c->type == animation::kConCopyRotation) {
            int mask = c->axisMask;
            const char* kAxes[3] = {"X", "Y", "Z"};
            for (int a = 0; a < 3; ++a) {
                bool on = (mask & (1 << a)) != 0;
                ImGui::SameLine();
                ImGui::PushID(100 + a);
                if (ImGui::Checkbox(kAxes[a], &on)) {
                    c->axisMask ^= (1 << a);
                    m_conDirty = true;
                }
                ImGui::PopID();
            }
        }
        break;
    }
    case animation::kConCopyLocation: {
        ImGui::TextUnformatted(TR("Target bone"));
        drawConstraintTargetCombo("##con_tgt", c->target, 180.0f);
        logRect("CHAR-CON-TARGET", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        float off[3] = {c->offset.x, c->offset.y, c->offset.z};
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::DragFloat3(TR("Offset"), off, 0.01f, -10.0f, 10.0f, "%.3f")) {
            c->offset = glm::vec3(off[0], off[1], off[2]);
            m_conDirty = true;
        }
        logRect("CHAR-CON-OFFSET", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        break;
    }
    case animation::kConTwoBoneIK: {
        ImGui::TextUnformatted(TR("Chain root"));
        drawConstraintTargetCombo("##con_root", c->ikRoot, 180.0f);
        logRect("CHAR-CON-IKROOT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::TextUnformatted(TR("Chain mid"));
        drawConstraintTargetCombo("##con_mid", c->ikMid, 180.0f);
        logRect("CHAR-CON-IKMID", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::TextUnformatted(TR("Chain end"));
        drawConstraintTargetCombo("##con_end", c->ikEnd, 180.0f);
        logRect("CHAR-CON-IKEND", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        bool has = c->ikHasTarget;
        if (ImGui::Checkbox(TR("Has target"), &has)) {
            c->ikHasTarget = has;
            m_conDirty = true;
        }
        logRect("CHAR-CON-IKHAS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        float tp[3] = {c->ikTarget.x, c->ikTarget.y, c->ikTarget.z};
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::DragFloat3(TR("Target"), tp, 0.01f, -20.0f, 20.0f, "%.3f")) {
            c->ikTarget = glm::vec3(tp[0], tp[1], tp[2]);
            m_conDirty = true;
        }
        logRect("CHAR-CON-IKTGT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::SliderFloat(TR("Stretch"), &c->stretch, 0.0f, 1.0f)) m_conDirty = true;
        logRect("CHAR-CON-IKSTRETCH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        break;
    }
    default:
        break;
    }
}

void CharacterWindow::runConstraintScript() {
    if (m_conScriptRan) return;
    const char* raw = std::getenv("MYVK_EDITOR_CONSTRAINT_SCRIPT");
    if (!raw || !raw[0]) {
        m_conScriptRan = true;
        return;
    }
    if (!m_model || !m_model->skeleton) return;   // 模型异步加载：等
    m_conScriptRan = true;
    ensureRig();
    m_focusRigTab = true;   // 页签不被选中就没有锚点

    // 约束挂在**骨架 joint 下标**上，而 Rig 页选中的是 `EditBone`。脚本 `add`
    // / `sel` 之后必须把 Rig 页的选择同步过去 —— 否则 `drawConstraintsSection`
    // 里那句 `c->bone != bone` 会直接 return，类型参数一块锚点都不出现
    // （第一版就是这样：TITLE/STATS 有、INFL 之后全无）。
    const auto syncRigSel = [this](int joint) {
        if (joint < 0) return;
        for (int i = 0; i < m_rig.boneCount(); ++i) {
            const animation::EditBone& b = m_rig.bones[static_cast<std::size_t>(i)];
            const int j = b.srcJoint >= 0 ? b.srcJoint : i;
            if (j == joint) {
                m_rigSel = i;
                return;
            }
        }
    };

    int applied = 0;
    int failed = 0;
    const auto split = [](const std::string& s) {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos <= s.size()) {
            const std::size_t p = s.find(':', pos);
            out.push_back(s.substr(pos, p == std::string::npos ? std::string::npos
                                                              : p - pos));
            if (p == std::string::npos) break;
            pos = p + 1;
        }
        return out;
    };
    const auto toF = [](const std::string& s, float def) {
        try { return std::stof(s); } catch (...) { return def; }
    };
    const auto toI = [](const std::string& s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    };

    const std::string all(raw);
    std::size_t pos = 0;
    while (pos <= all.size()) {
        const std::size_t semi = all.find(';', pos);
        std::string tok = all.substr(pos, semi == std::string::npos
                                              ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? all.size() + 1 : semi + 1;
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.pop_back();
        if (tok.empty()) continue;

        const std::vector<std::string> a = split(tok);
        const std::string& cmd = a[0];
        bool ok = false;

        if (cmd == "list") {
            VK_LOG_INFO("CONSTRAINT-LIST n=%d joints=%d digest=%016llx",
                        m_constraints.size(), m_model->skeleton->jointCount(),
                        m_constraints.digest());
            for (int i = 0; i < m_constraints.size(); ++i) {
                const animation::Constraint& c =
                    m_constraints.items[static_cast<std::size_t>(i)];
                VK_LOG_INFO("CONSTRAINT-LIST-ITEM i=%d type=%s bone=%d target=%d "
                            "inf=%.4f enabled=%d",
                            i, animation::constraintTypeName(c.type), c.bone,
                            c.target, static_cast<double>(c.influence),
                            c.enabled ? 1 : 0);
            }
            ok = true;
        } else if (cmd == "add" && a.size() >= 3) {
            animation::Constraint c;
            if (!animation::constraintTypeFromName(a[1], c.type)) {
                // 允许直接写数字（脚本里更省事）
                const int t = toI(a[1], -1);
                if (t < 0 || t >= animation::kConTypeCount) { ++failed; continue; }
                c.type = t;
            }
            c.bone = toI(a[2], -1);
            if (c.type == animation::kConTwoBoneIK) {
                c.ikRoot = c.bone;
                c.ikHasTarget = true;
            }
            m_conSel = m_constraints.add(c);
            syncRigSel(c.bone);
            ok = true;
        } else if (cmd == "sel" && a.size() >= 2) {
            m_conSel = toI(a[1], -1);
            ok = m_conSel >= -1 && m_conSel < m_constraints.size();
            const animation::Constraint* c = m_constraints.at(m_conSel);
            if (c) syncRigSel(c->bone);
        } else if (cmd == "del" && a.size() >= 2) {
            ok = m_constraints.removeAt(toI(a[1], -1)) > 0;
        } else if (cmd == "move" && a.size() >= 3) {
            const int i = toI(a[1], -1);
            const int d = toI(a[2], 0);
            ok = (d < 0) ? m_constraints.moveUp(i) > 0
                         : m_constraints.moveDown(i) > 0;
        } else if (cmd == "influence" && a.size() >= 3) {
            animation::Constraint* c = m_constraints.at(toI(a[1], -1));
            if (c) {
                c->influence = toF(a[2], c->influence);
                ok = true;
            }
        } else if (cmd == "enable" && a.size() >= 3) {
            animation::Constraint* c = m_constraints.at(toI(a[1], -1));
            if (c) {
                c->enabled = toI(a[2], 1) != 0;
                ok = true;
            }
        } else if (cmd == "target" && a.size() >= 3) {
            animation::Constraint* c = m_constraints.at(toI(a[1], -1));
            if (c) {
                c->target = toI(a[2], -1);
                ok = true;
            }
        } else if (cmd == "limit" && a.size() >= 5) {
            animation::Constraint* c = m_constraints.at(toI(a[1], -1));
            const int axis = toI(a[2], -1);
            if (c && axis >= 0 && axis < 3) {
                c->minDeg[axis] = toF(a[3], c->minDeg[axis]);
                c->maxDeg[axis] = toF(a[4], c->maxDeg[axis]);
                c->limitMask |= (1 << axis);
                ok = true;
            }
        } else if (cmd == "mask" && a.size() >= 3) {
            animation::Constraint* c = m_constraints.at(toI(a[1], -1));
            if (c) {
                c->limitMask = toI(a[2], 7);
                c->axisMask = toI(a[2], 7);
                ok = true;
            }
        } else if (cmd == "ik" && a.size() >= 5) {
            animation::Constraint* c = m_constraints.at(toI(a[1], -1));
            if (c) {
                c->ikRoot = toI(a[2], c->ikRoot);
                c->ikMid = toI(a[3], c->ikMid);
                c->ikEnd = toI(a[4], c->ikEnd);
                ok = true;
            }
        } else if (cmd == "iktarget" && a.size() >= 5) {
            animation::Constraint* c = m_constraints.at(toI(a[1], -1));
            if (c) {
                c->ikTarget = glm::vec3(toF(a[2], 0.0f), toF(a[3], 0.0f),
                                        toF(a[4], 0.0f));
                c->ikHasTarget = true;
                ok = true;
            }
        } else if (cmd == "stretch" && a.size() >= 3) {
            animation::Constraint* c = m_constraints.at(toI(a[1], -1));
            if (c) {
                c->stretch = toF(a[2], 0.0f);
                ok = true;
            }
        } else if (cmd == "clear") {
            ok = m_constraints.clear() >= 0;
            m_conSel = -1;
        } else if (cmd == "digest") {
            VK_LOG_INFO("CONSTRAINT-DIGEST n=%d h=%016llx", m_constraints.size(),
                        m_constraints.digest());
            ok = true;
        } else if (cmd == "eval") {
            // 手动求一次当前姿态（预览每帧也会求，这条是给"不看画面"的断言用）
            if (m_pose.count() > 0) {
                m_conStats = m_constraints.evaluate(*m_model->skeleton, m_pose);
                animation::poseToJoints(*m_model->skeleton, m_pose, m_jointMats);
            }
            VK_LOG_INFO("CONSTRAINT-EVAL total=%d applied=%d skipped=%d failed=%d "
                        "clamped=%d ik=%d",
                        m_conStats.total, m_conStats.applied, m_conStats.skipped,
                        m_conStats.failed, m_conStats.clamped, m_conStats.ikSolved);
            ok = true;
        } else if (cmd == "save" && a.size() >= 2) {
            // 路径里可能有 `:`（`E:\...`）→ 把第 1 段之后的全拼回去
            std::string path = a[1];
            for (std::size_t k = 2; k < a.size(); ++k) path += ":" + a[k];
            ok = saveConstraintsTo(path);
        } else if (cmd == "load" && a.size() >= 2) {
            std::string path = a[1];
            for (std::size_t k = 2; k < a.size(); ++k) path += ":" + a[k];
            ok = loadConstraintsFrom(path);
        }

        if (ok) ++applied;
        else {
            ++failed;
            VK_LOG_WARN("CONSTRAINT-SCRIPT-FAIL cmd='%s'", tok.c_str());
        }
    }

    m_constraints.logSummary("script");
    VK_LOG_INFO("CONSTRAINT-SCRIPT applied=%d failed=%d n=%d sel=%d dirty=%d "
                "joints=%d",
                applied, failed, m_constraints.size(), m_conSel,
                m_conDirty ? 1 : 0, m_model->skeleton->jointCount());
}

bool CharacterWindow::saveConstraintsTo(const std::string& path) {
    if (!m_model || !m_model->skeleton) return false;
    std::string err;
    const bool ok =
        animation::saveConstraints(path, m_constraints, *m_model->skeleton, err);
    if (!ok) VK_LOG_WARN("CONSTRAINT-SAVE-FAIL path='%s' err='%s'", path.c_str(),
                         err.c_str());
    else m_conDirty = false;
    return ok;
}

bool CharacterWindow::loadConstraintsFrom(const std::string& path) {
    if (!m_model || !m_model->skeleton) return false;
    std::string err;
    int unresolved = 0;
    animation::ConstraintStack st;
    if (!animation::loadConstraints(path, *m_model->skeleton, st, err, &unresolved)) {
        VK_LOG_WARN("CONSTRAINT-LOAD-FAIL path='%s' err='%s'", path.c_str(),
                    err.c_str());
        return false;
    }
    m_constraints = std::move(st);
    m_conSel = m_constraints.size() > 0 ? 0 : -1;
    m_conBoundJoints = m_model->skeleton->jointCount();
    m_conDirty = false;
    if (unresolved > 0) {
        VK_LOG_WARN("CONSTRAINT-LOAD-UNRESOLVED n=%d", unresolved);
    }
    return true;
}

void CharacterWindow::runRigScript() {
    if (m_rigScriptRan) return;
    const char* raw = std::getenv("MYVK_EDITOR_RIG_SCRIPT");
    if (!raw || !raw[0]) {
        m_rigScriptRan = true;
        return;
    }
    // 模型是异步加载的：没有骨架的那几帧要等，不能把脚本标记成"跑过了"。
    if (!m_model || !m_model->skeleton) return;
    m_rigScriptRan = true;
    ensureRig();
    m_focusRigTab = true;   // 让这一帧真的把 Rig 页画出来（锚点靠它）

    int applied = 0;
    int failed = 0;
    const auto split = [](const std::string& s) {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos <= s.size()) {
            const std::size_t p = s.find(':', pos);
            out.push_back(s.substr(pos, p == std::string::npos ? std::string::npos
                                                              : p - pos));
            if (p == std::string::npos) break;
            pos = p + 1;
        }
        return out;
    };
    const auto toF = [](const std::string& s, float def) {
        try { return std::stof(s); } catch (...) { return def; }
    };

    const std::string all(raw);
    std::size_t pos = 0;
    while (pos <= all.size()) {
        const std::size_t semi = all.find(';', pos);
        std::string tok = all.substr(pos, semi == std::string::npos
                                              ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? all.size() + 1 : semi + 1;
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.pop_back();
        if (tok.empty()) continue;

        const std::vector<std::string> a = split(tok);
        const std::string& cmd = a[0];
        bool ok = false;

        // 骨名可能带 `.` `:`（`mixamorig:LeftArm`），所以命令里骨名放在**最后**
        // 或**第一个参数之外**都不安全 —— 约定：`<cmd>:<bone>:<args…>`，
        // 而 `mixamorig:LeftArm` 里的冒号会把 split 打碎。
        // 因此骨名一律用**下标**（`list` 会打出来），这也让断言不依赖命名。
        const auto boneArg = [&](std::size_t k, int& out) {
            if (k >= a.size()) return false;
            try {
                out = std::stoi(a[k]);
            } catch (...) {
                return false;
            }
            return out >= 0 && out < m_rig.boneCount();
        };

        int bi = -1;
        if (cmd == "list") {
            VK_LOG_INFO("RIG-LIST n=%d axis=%s", m_rig.boneCount(),
                        animation::RigEdit::axisName(m_rig.mirrorAxis));
            for (int i = 0; i < m_rig.boneCount(); ++i) {
                const animation::EditBone& b = m_rig.bones[static_cast<std::size_t>(i)];
                VK_LOG_INFO("RIG-LIST-BONE i=%d name='%s' parent=%d src=%d len=%.6f",
                            i, b.name.c_str(), b.parent, b.srcJoint,
                            static_cast<double>(b.length));
            }
            ok = true;
        } else if (cmd == "sel" && boneArg(1, bi)) {
            m_rigSel = bi;
            ok = true;
        } else if (cmd == "roll" && boneArg(1, bi) && a.size() >= 3) {
            ok = m_rig.setRoll(bi, toF(a[2], 0.0f) * kRigDeg2Rad);
            if (ok) m_rigDirty = true;
        } else if (cmd == "len" && boneArg(1, bi) && a.size() >= 3) {
            ok = m_rig.setLength(bi, toF(a[2], 0.0f));
            if (ok) m_rigDirty = true;
        } else if (cmd == "head" && boneArg(1, bi) && a.size() >= 5) {
            ok = m_rig.setHead(bi, glm::vec3(toF(a[2], 0.0f), toF(a[3], 0.0f),
                                             toF(a[4], 0.0f)));
            if (ok) m_rigDirty = true;
        } else if (cmd == "dir" && boneArg(1, bi) && a.size() >= 5) {
            ok = m_rig.setDir(bi, glm::vec3(toF(a[2], 0.0f), toF(a[3], 0.0f),
                                            toF(a[4], 0.0f)));
            if (ok) m_rigDirty = true;
        } else if (cmd == "deform" && boneArg(1, bi) && a.size() >= 3) {
            m_rig.setDeform(bi, a[2] != "0");
            ok = true;
        } else if (cmd == "conn" && boneArg(1, bi) && a.size() >= 3) {
            m_rig.setConnected(bi, a[2] != "0");
            ok = true;
            m_rigDirty = true;
        } else if (cmd == "inherit" && boneArg(1, bi) && a.size() >= 3) {
            m_rig.setInheritRot(bi, a[2] != "0");
            ok = true;
        } else if (cmd == "mirror" && boneArg(1, bi)) {
            ok = m_rig.mirrorToOpposite(bi) >= 0;
            if (ok) m_rigDirty = true;
        } else if (cmd == "add" && a.size() >= 5) {
            // add:<name>:<parent>:<len>
            const int par = std::stoi(a[2]);
            if (par >= 0 && par < m_rig.boneCount()) {
                const animation::EditBone pb = m_rig.bones[static_cast<std::size_t>(par)];
                const int idx = m_rig.addBone(a[1], par, pb.tail(), pb.dir,
                                              toF(a[3], 0.1f));
                ok = idx >= 0;
                if (ok) {
                    m_rigSel = idx;
                    m_rigDirty = true;
                }
            }
        } else if (cmd == "del" && boneArg(1, bi)) {
            ok = m_rig.removeBone(bi) > 0;
            if (ok) {
                m_rigSel = m_rig.boneCount() > 0 ? 0 : -1;
                m_rigDirty = true;
            }
        } else if (cmd == "save" && a.size() >= 2) {
            std::string err;
            std::string path = tok.substr(5);   // "save:" 之后整段（路径里有冒号）
            ok = animation::saveRig(path, m_rig, err);
            if (!ok) VK_LOG_WARN("RIG-SCRIPT-SAVE-FAIL err='%s'", err.c_str());
        } else if (cmd == "dump") {
            m_rig.restWorld(m_rigWorld);
            for (int i = 0; i < m_rig.boneCount(); ++i) {
                const animation::EditBone& b = m_rig.bones[static_cast<std::size_t>(i)];
                const glm::mat4& W = m_rigWorld[static_cast<std::size_t>(i)];
                VK_LOG_INFO("RIG-DUMP i=%d name='%s' parent=%d t=(%a,%a,%a) len=%a "
                            "roll=%a deform=%d conn=%d inh=%d d=(%a,%a,%a) ax=(%a,%a,%a)",
                            i, b.name.c_str(), b.parent,
                            static_cast<double>(W[3][0]), static_cast<double>(W[3][1]),
                            static_cast<double>(W[3][2]),
                            static_cast<double>(b.length),
                            static_cast<double>(b.roll), b.deform ? 1 : 0,
                            b.connected ? 1 : 0, b.inheritRot ? 1 : 0,
                            static_cast<double>(b.dir.x), static_cast<double>(b.dir.y),
                            static_cast<double>(b.dir.z),
                            static_cast<double>(W[0][0]), static_cast<double>(W[0][1]),
                            static_cast<double>(W[0][2]));
            }
            ok = true;
        } else if (cmd == "apply") {
            applyRig();
            ok = true;
        } else if (cmd == "revert") {
            revertRig();
            ok = true;
        }

        if (ok) ++applied; else ++failed;
        if (!ok) VK_LOG_WARN("RIG-SCRIPT-FAIL cmd='%s'", tok.c_str());
    }

    if (m_rigDirty) applyRig();
    const animation::RigInfo st = m_rig.info();
    VK_LOG_INFO("RIG-SCRIPT applied=%d failed=%d bones=%d touched=%d sel=%d",
                applied, failed, st.bones, st.touched, m_rigSel);
}

// ============================================================
// Weights 页（S-B，2026-10-29）：顶点组 / 自动权重 / 权重绘制
// ============================================================
//
// 和 Rig 页的三条纪律同源，但多一条**只有这一页才有的**：
//
//   ① 改数据只走 `animation::WeightSet` 那一族 —— UI、注入脚本、自检共用
//      同一份实现，验收因此不依赖"把鼠标拖到正确的像素上"；
//   ② 烘焙只在"脏了"的时候做（一笔刷完烤一次，不是每个顶点烤一次）；
//   ③ `assets::Mesh` 的顶点数组仍然是**导入产物**：`WeightSet` 一直是权威，
//      刷完才 `bake()` 写成四槽再重传；
//   ④ **重传 = `waitIdle()` + `Mesh::upload()`**。`upload` 内部会
//      `delete m_vertexBuffer`，而在飞的命令缓冲还引用着它 —— 不先等
//      设备空闲就是"删掉一帧之前还在读的缓冲"，验证层报
//      `VUID-vkDestroyBuffer-00867`，不报错时就是花屏。代价是一次全同步，
//      但只在"作者松手/刷了一笔"时发生，不是每帧。
namespace {
constexpr float kWeightMirrorEps = 0.01f;   // 镜像配对的容差（米）

// 权重色带：0 = 蓝、0.5 = 绿、1 = 红（Blender 的口径）。
// 为什么不直接 HSV 扫一遍：HSV 从蓝到红要绕 240°，中间会经过紫 ——
// 那一段和"零权重"的深色在灰背景上分不开。
ImU32 weightColor(float w) {
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    const float r = w < 0.5f ? 0.0f : (w - 0.5f) * 2.0f;
    const float g = (w < 0.5f ? w : (1.0f - w)) * 2.0f;
    const float b = w < 0.5f ? (1.0f - w * 2.0f) : 0.0f;
    return IM_COL32(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f),
                    static_cast<int>(b * 255.0f), 210);
}
} // namespace

int CharacterWindow::weightSetCount() const {
    return static_cast<int>(m_weights.size());
}

int CharacterWindow::activeWeightSet() const {
    const int n = weightSetCount();
    if (n <= 0) return -1;
    if (m_weightSel >= 0 && m_weightSel < n && m_weightsActive[static_cast<std::size_t>(m_weightSel)])
        return m_weightSel;
    for (int i = 0; i < n; ++i)
        if (m_weightsActive[static_cast<std::size_t>(i)]) return i;
    return -1;
}

void CharacterWindow::ensureWeights() {
    if (!m_model || !m_model->skeleton) return;
    ensureRig();   // 权重里的"骨"编号以 RigEdit 为准（见头文件那段 ⚠）

    const int subs = static_cast<int>(m_model->subMeshes.size());
    const bool sameModel =
        m_weightsInited && static_cast<int>(m_weights.size()) == subs &&
        (m_weights.empty() || m_weights.front().source == m_modelPath);
    if (sameModel) {
        if (m_weightsRigBones == m_rig.boneCount()) return;
        // 增/删骨之后 `bones[i]` 与 `joints[i]` 的对应关系整体挪位，权重里
        // 存的骨下标会指到别的骨上 —— 没有崩溃、没有断言，只有"刷好的权重
        // 跑到别处"。这里只能退回网格里的四槽（GPU 正在用的那份）。
        VK_LOG_WARN("WEIGHTS-STALE bones %d -> %d（增删骨会让权重下标错位，"
                    "重新从网格导入）", m_weightsRigBones, m_rig.boneCount());
    }

    m_weights.assign(static_cast<std::size_t>(subs), animation::WeightSet{});
    m_weightsActive.assign(static_cast<std::size_t>(subs), 0);
    m_weightsDirty.assign(static_cast<std::size_t>(subs), 0);
    int skinned = 0;
    int totalVerts = 0;
    for (int i = 0; i < subs; ++i) {
        assets::Mesh* mesh = m_model->subMeshes[static_cast<std::size_t>(i)].mesh;
        if (mesh == nullptr || mesh->vertices().empty()) continue;
        animation::WeightSet& ws = m_weights[static_cast<std::size_t>(i)];
        ws = animation::WeightSet::fromMesh(*mesh, m_modelPath);
        // 子网格没名字时给一个稳定的代名（日志/存盘都用它对齐）
        if (m_model->subMeshes[static_cast<std::size_t>(i)].name.empty())
            ws.mesh = "sub" + std::to_string(i);
        bool any = false;
        for (int v = 0; v < ws.vertexCount && !any; ++v)
            any = ws.influenceCount(v) > 0;
        m_weightsActive[static_cast<std::size_t>(i)] = any ? 1 : 0;
        totalVerts += ws.vertexCount;
        if (any) ++skinned;
    }
    m_weightsInited = true;
    m_weightsRigBones = m_rig.boneCount();
    m_weightCacheSet = -1;
    m_weightSel = 0;
    const int first = activeWeightSet();
    if (first >= 0) m_weightSel = first;

    m_deformMask.clear();
    for (const animation::EditBone& b : m_rig.bones)
        m_deformMask.push_back(b.deform ? 1 : 0);

    if (m_weightsPathBuf[0] == 0) {
        std::string p = m_modelPath.empty() ? std::string("character") : m_modelPath;
        const std::size_t dot = p.find_last_of('.');
        const std::size_t slash = p.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            p = p.substr(0, dot);
        p += ".weights.json";
        std::snprintf(m_weightsPathBuf, sizeof(m_weightsPathBuf), "%s", p.c_str());
    }
    VK_LOG_INFO("WEIGHTS-IMPORT meshes=%d skinned=%d verts=%d bones=%d model='%s'",
                subs, skinned, totalVerts, m_rig.boneCount(), m_modelPath.c_str());
}

void CharacterWindow::ensureWeightCaches() {
    const int s = activeWeightSet();
    if (s < 0 || m_weightCacheSet == s) return;
    assets::Mesh* mesh = m_model->subMeshes[static_cast<std::size_t>(s)].mesh;
    if (mesh == nullptr) return;
    animation::meshVertexPositions(*mesh, m_weightPos);
    const std::vector<assets::Vertex>& vs = mesh->vertices();
    m_weightNrm.resize(vs.size());
    for (std::size_t i = 0; i < vs.size(); ++i) m_weightNrm[i] = vs[i].normal;
    animation::buildVertexRing(*mesh, m_weightRingOff, m_weightRingIdx);
    m_weightCacheSet = s;
    VK_LOG_INFO("WEIGHTS-CACHE mesh=%d vtx=%d ring=%d", s,
                static_cast<int>(m_weightPos.size()),
                static_cast<int>(m_weightRingIdx.size()));
}

void CharacterWindow::applyWeights() {
    if (!m_model) return;
    const assets::TextureContext tc = m_renderer.textureContext();
    if (tc.device == nullptr || tc.cmdPool == nullptr) return;

    // deform mask 与骨数**必须**对齐：短一截会让 mask 把后半段骨的权重
    // 全砍掉（`bake` 里越界按"非 deform"处理），画面是"角色后半身不动"。
    std::vector<char> mask;
    mask.reserve(m_rig.bones.size());
    for (const animation::EditBone& b : m_rig.bones)
        mask.push_back(b.deform ? 1 : 0);

    int baked = 0;
    int reuploaded = 0;
    int changedVerts = 0;
    for (std::size_t i = 0; i < m_weights.size(); ++i) {
        if (!m_weightsDirty[i]) continue;
        m_weightsDirty[i] = 0;
        assets::Mesh* mesh = m_model->subMeshes[i].mesh;
        if (mesh == nullptr) continue;
        std::vector<assets::Vertex> vs = mesh->vertices();
        if (vs.size() != static_cast<std::size_t>(m_weights[i].vertexCount)) {
            VK_LOG_WARN("WEIGHTS-BAKE-SKIP mesh=%d vtx %d != %d", static_cast<int>(i),
                        m_weights[i].vertexCount, static_cast<int>(vs.size()));
            continue;
        }
        const int c = m_weights[i].bake(vs.data(), mask.empty() ? nullptr : &mask);
        ++baked;
        changedVerts += c;
        if (c > 0) {
            // 约定 ④：先等设备空闲再让 `upload` 删旧缓冲
            m_renderer.device().waitIdle();
            mesh->upload(*tc.device, *tc.cmdPool, std::move(vs), mesh->indices());
            ++reuploaded;
        }
        VK_LOG_INFO("WEIGHTS-BAKE mesh=%d vtx=%d changed=%d", static_cast<int>(i),
                    m_weights[i].vertexCount, c);
    }
    if (baked > 0)
        VK_LOG_INFO("WEIGHTS-BAKE-ALL meshes=%d reuploaded=%d changedVerts=%d",
                    baked, reuploaded, changedVerts);
}

void CharacterWindow::revertWeights() {
    VK_LOG_INFO("WEIGHTS-REVERT meshes=%d", weightSetCount());
    m_weightsInited = false;
    m_weightCacheSet = -1;
    ensureWeights();
    // 还原 = 把网格里的四槽重新写回去（`applyWeights` 走的是"脏了才烤"，
    // 这里把所有都标脏，让每个网格都重传一次）
    for (std::size_t i = 0; i < m_weightsDirty.size(); ++i)
        m_weightsDirty[i] = m_weightsActive[i];
    applyWeights();
}

void CharacterWindow::drawWeightsPanel() {
    m_weightsTabOpen = true;
    if (!m_model || !m_model->skeleton) {
        ImGui::TextDisabled(TR("(no skeleton)"));
        return;
    }
    ensureWeights();

    const int s = activeWeightSet();
    const int n = weightSetCount();
    ImGui::TextDisabled(TR("PAINT WEIGHTS"));
    ImGui::Separator();

    // ---- 网格选择 ----
    if (n > 0) {
        if (m_weightSel < 0 || m_weightSel >= n) m_weightSel = 0;
        const std::string& nm = m_weights[static_cast<std::size_t>(m_weightSel)].mesh;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##wt_mesh", nm.c_str())) {
            for (int i = 0; i < n; ++i) {
                ImGui::PushID(i);
                char label[192];
                std::snprintf(label, sizeof(label), "%s%s",
                              m_weightsActive[static_cast<std::size_t>(i)] ? "" : TR("(static) "),
                              m_weights[static_cast<std::size_t>(i)].mesh.c_str());
                if (ImGui::Selectable(label, m_weightSel == i)) {
                    m_weightSel = i;
                    m_weightCacheSet = -1;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        logRect("CHAR-WT-MESH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        tip(TR("Which sub-mesh you are painting. One vertex group set per sub-mesh."));
    }
    if (s < 0) {
        ImGui::TextDisabled(TR("(this mesh has no skin weights)"));
        return;
    }

    animation::WeightSet& ws = m_weights[static_cast<std::size_t>(s)];
    const animation::WeightStats st = ws.stats();
    ImGui::Text("%s %d  %s %d  %s %d", TR("vertices"), st.vertices, TR("touched"),
                st.touched, TR("no weights"), st.unweighted);
    logRect("CHAR-WT-STATS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 自动权重 ----
    ImGui::Separator();
    ImGui::TextDisabled(TR("AUTO WEIGHTS"));
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Radius"), &m_autoParams.radiusScale, 0.02f, 0.1f, 8.0f,
                     "x%.2f");
    logRect("CHAR-WT-RADIUS", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    tip(TR("Envelope radius = bone length x this. Too small leaves the mesh "
           "unskinned, too large smears one bone over the whole body."));
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Falloff"), &m_autoParams.falloff, 0.05f, 0.1f, 8.0f);
    logRect("CHAR-WT-FALLOFF", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragInt(TR("Max infl"), &m_autoParams.maxInfluence, 0.1f, 1,
                   animation::kMaxInfluence);
    logRect("CHAR-WT-MAXINF", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::Checkbox(TR("Deform bones only"), &m_autoParams.deformOnly);
    logRect("CHAR-WT-DEFORMONLY", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::Button(TR("Auto Weights (this mesh)"))) {
        ensureWeightCaches();
        const int got = ws.autoAssign(m_rig, m_weightPos,
                                      glm::inverse(m_model->skeleton->invMeshNode),
                                      m_autoParams);
        m_weightsDirty[static_cast<std::size_t>(s)] = got > 0;
        ws.logSummary("auto");
        VK_LOG_INFO("WEIGHTS-AUTO mesh=%d assigned=%d", s, got);
    }
    logRect("CHAR-WT-AUTO", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Auto Weights (all)"))) {
        int done = 0;
        for (int i = 0; i < n; ++i) {
            if (!m_weightsActive[static_cast<std::size_t>(i)]) continue;
            assets::Mesh* mesh = m_model->subMeshes[static_cast<std::size_t>(i)].mesh;
            if (mesh == nullptr) continue;
            std::vector<glm::vec3> pos;
            animation::meshVertexPositions(*mesh, pos);
            const int got = m_weights[static_cast<std::size_t>(i)].autoAssign(
                m_rig, pos, glm::inverse(m_model->skeleton->invMeshNode), m_autoParams);
            m_weightsDirty[static_cast<std::size_t>(i)] = got > 0;
            if (got > 0) ++done;
        }
        m_weightCacheSet = -1;
        VK_LOG_INFO("WEIGHTS-AUTO-ALL meshes=%d", done);
    }
    logRect("CHAR-WT-AUTOALL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 刷子 ----
    ImGui::Separator();
    ImGui::TextDisabled(TR("BRUSH"));
    const char* kModes[] = {TR("Add"), TR("Subtract"), TR("Set")};
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##wt_mode", &m_brushMode, kModes, 3);
    logRect("CHAR-WT-MODE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Weight"), &m_brushWeight, 0.005f, 0.0f, 1.0f, "%.3f");
    logRect("CHAR-WT-WEIGHT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SetNextItemWidth(120.0f);
    // ⚠ 格式串本身也要 TR()：i18n 覆盖率 E 组扫的就是这种"字面量不在 ImGui 调用
    //   里、但屏幕上显示英文"的漏法（`DragFloat` 的 fmt 参数正是典型）。
    ImGui::DragFloat(TR("Brush size"), &m_brushRadiusPx, 0.5f, 4.0f, 400.0f,
                     TR("%.0f px"));
    logRect("CHAR-WT-BRUSH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat(TR("Softness"), &m_strengthScale, 0.02f, 0.1f, 6.0f);
    logRect("CHAR-WT-SOFT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    tip(TR("Softness: how fast the stroke fades from the brush centre to its edge."));
    ImGui::Checkbox(TR("Auto normalize"), &m_brushAutoNormalize);
    logRect("CHAR-WT-AUTONORM", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::Checkbox(TR("X mirror"), &m_brushXMirror);
    logRect("CHAR-WT-MIRROR", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::Checkbox(TR("Front faces"), &m_brushFrontFaces);
    logRect("CHAR-WT-FRONTFACE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    tip(TR("Paint on the vertex group of the bone selected in the Rig tab."));
    ImGui::Text("%s: %s", TR("Target bone"),
                (m_rigSel >= 0 && m_rigSel < m_rig.boneCount())
                    ? m_rig.bones[static_cast<std::size_t>(m_rigSel)].name.c_str()
                    : TR("(none)"));
    logRect("CHAR-WT-BONE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 工具 ----
    ImGui::Separator();
    if (ImGui::Button(TR("Normalize All"))) {
        const int c = ws.normalizeAll();
        if (c > 0) m_weightsDirty[static_cast<std::size_t>(s)] = 1;
        VK_LOG_INFO("WEIGHTS-NORMALIZE mesh=%d fixed=%d", s, c);
    }
    logRect("CHAR-WT-NORMALIZE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Smooth"))) {
        ensureWeightCaches();
        const int c = ws.blur(m_weightRingOff, m_weightRingIdx, 1, 0.5f);
        if (c > 0) m_weightsDirty[static_cast<std::size_t>(s)] = 1;
        VK_LOG_INFO("WEIGHTS-BLUR mesh=%d affected=%d", s, c);
    }
    logRect("CHAR-WT-SMOOTH", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Bake to mesh"))) {
        m_weightsDirty[static_cast<std::size_t>(s)] = 1;
        applyWeights();
    }
    logRect("CHAR-WT-BAKE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // ---- 文件 ----
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##wt_path", m_weightsPathBuf, sizeof(m_weightsPathBuf));
    if (ImGui::Button(TR("Save"))) {
        std::string err;
        if (!animation::saveWeights(m_weightsPathBuf, ws, err))
            VK_LOG_WARN("CHAR-WT-SAVE-FAIL err='%s'", err.c_str());
    }
    logRect("CHAR-WT-SAVE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Load"))) {
        animation::WeightSet tmp;
        std::string err;
        if (animation::loadWeights(m_weightsPathBuf, tmp, err)) {
            // 只接受同一张网格（顶点数一致）：顶点数对不上的表换上去，
            // 权重会整体错位，而画面上只是"角色有点怪"。
            if (tmp.vertexCount == ws.vertexCount) {
                const std::string keepMesh = ws.mesh;
                ws = std::move(tmp);
                ws.mesh = keepMesh;
                m_weightsDirty[static_cast<std::size_t>(s)] = 1;
                m_weightCacheSet = -1;
            } else {
                VK_LOG_WARN("CHAR-WT-LOAD-vs vtx=%d vs %d（顶点数不同，拒绝）",
                            tmp.vertexCount, ws.vertexCount);
            }
        } else {
            VK_LOG_WARN("CHAR-WT-LOAD-FAIL err='%s'", err.c_str());
        }
    }
    logRect("CHAR-WT-LOAD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(TR("Revert"))) revertWeights();
    logRect("CHAR-WT-REVERT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    ImGui::Checkbox(TR("Show weights"), &m_showWeights);
    logRect("CHAR-WT-SHOW", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// ---------------------------------------------------------------- 视口绘制

bool CharacterWindow::projectWeightVertex(int v, const glm::mat4& viewProj,
                                          const ImVec2& imgMin, const ImVec2& imgSz,
                                          glm::vec2& out) const {
    const int s = activeWeightSet();
    if (s < 0 || v < 0 || v >= static_cast<int>(m_weightPos.size())) return false;
    if (!m_model || s >= static_cast<int>(m_model->subMeshes.size())) return false;
    // 顶点是**网格空间**的，子网格自带节点变换（见 assets/Model.h）
    const glm::vec3 wp =
        glm::vec3(m_model->subMeshes[static_cast<std::size_t>(s)].transform *
                  glm::vec4(m_weightPos[static_cast<std::size_t>(v)], 1.0f));
    return PickingSystem::worldToScreen(viewProj, glm::vec2(imgMin.x, imgMin.y),
                                       glm::vec2(imgSz.x, imgSz.y), wp, out);
}

void CharacterWindow::drawWeightsOverlay(const ImVec2& imgMin, const ImVec2& imgSz,
                                         const glm::mat4& viewProj, bool hovered) {
    const int s = activeWeightSet();
    if (s < 0 || !m_showWeights || m_model == nullptr) return;
    if (m_rigSel < 0 || m_rigSel >= m_rig.boneCount()) return;
    const animation::WeightSet& ws = m_weights[static_cast<std::size_t>(s)];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(imgMin, ImVec2(imgMin.x + imgSz.x, imgMin.y + imgSz.y), true);

    // 只画"当前选中骨"的权重：全画一遍在多层衣服的网格上是一团糊。
    // 上限 6000 个点 —— 再多的网格画点会把这帧的 draw call 拖到几十毫秒，
    // 而权重绘制的用处是"看这块区域分给了谁"，不是"看每个顶点"。
    constexpr int kMaxDots = 6000;
    int drawn = 0;
    int skipped = 0;
    for (int v = 0; v < ws.vertexCount && drawn < kMaxDots; ++v) {
        const float w = ws.weightOf(v, m_rigSel);
        if (w <= 0.0f) continue;
        glm::vec2 sp;
        if (!projectWeightVertex(v, viewProj, imgMin, imgSz, sp)) continue;
        dl->AddCircleFilled(ImVec2(sp.x, sp.y), 1.6f + 1.4f * w, weightColor(w), 8);
        ++drawn;
    }
    for (int v = 0; v < ws.vertexCount; ++v)
        if (ws.weightOf(v, m_rigSel) > 0.0f) ++skipped;
    m_weightHudVerts = skipped;

    if (hovered) {
        const ImVec2 mp = ImGui::GetMousePos();
        dl->AddCircle(mp, m_brushRadiusPx, IM_COL32(255, 236, 128, 210), 48, 1.6f);
        dl->AddCircleFilled(mp, 2.2f, IM_COL32(255, 236, 128, 220));
    }
    dl->PopClipRect();
}

void CharacterWindow::handleWeightsPaint(const ImVec2& imgMin, const ImVec2& imgSz,
                                         const glm::mat4& viewProj, bool hovered) {
    if (!m_weightsTabOpen || !hovered) return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (m_gizmoConsumed) return;             // 按在骨骼旋转环上时不算刷子
    const int s = activeWeightSet();
    if (s < 0) return;
    if (m_rigSel < 0 || m_rigSel >= m_rig.boneCount()) return;
    ensureWeightCaches();
    animation::WeightSet& ws = m_weights[static_cast<std::size_t>(s)];
    if (ws.vertexCount <= 0) return;

    const ImVec2 mp = ImGui::GetMousePos();
    const float r = std::max(m_brushRadiusPx, 1.0f);
    const float r2 = r * r;
    const float falloffPow = std::max(m_strengthScale, 0.05f);
    const glm::vec3 eye = m_previewScene.camera().position();
    const glm::mat4 xform = m_model->subMeshes[static_cast<std::size_t>(s)].transform;
    const glm::mat3 nrmMat = glm::mat3(xform);
    const animation::PaintMode mode = static_cast<animation::PaintMode>(m_brushMode);

    int painted = 0;
    int mirrored = 0;
    for (int v = 0; v < ws.vertexCount; ++v) {
        glm::vec2 sp;
        if (!projectWeightVertex(v, viewProj, imgMin, imgSz, sp)) continue;
        const float dx = sp.x - mp.x;
        const float dy = sp.y - mp.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 > r2) continue;
        if (m_brushFrontFaces) {
            const glm::vec3 wn = nrmMat * m_weightNrm[static_cast<std::size_t>(v)];
            const glm::vec3 wp = glm::vec3(xform *
                                           glm::vec4(m_weightPos[static_cast<std::size_t>(v)], 1.0f));
            if (glm::dot(wn, eye - wp) <= 0.0f) continue;
        }
        const float fall = std::pow(std::max(0.0f, 1.0f - std::sqrt(d2) / r), falloffPow);
        const float amt = m_brushWeight * fall;
        if (amt <= 0.0f) continue;
        painted += ws.paintVertex(v, m_rigSel, amt, mode, m_brushAutoNormalize);
        if (m_brushXMirror) {
            if (ws.mirrorVertex(m_rig, m_rig.mirrorAxis, m_weightPos, v,
                                kWeightMirrorEps) >= 0)
                ++mirrored;
        }
    }
    m_brushHits = painted;
    if (painted > 0) {
        m_weightsDirty[static_cast<std::size_t>(s)] = 1;
        applyWeights();   // 立即上屏：刷子看不到反馈等于没有刷子
        if (mirrored > 0)
            VK_LOG_INFO("WEIGHTS-PAINT-MIRROR verts=%d", mirrored);
    }
}

void CharacterWindow::runWeightsScript() {
    if (m_weightsScriptRan) return;
    const char* raw = std::getenv("MYVK_EDITOR_WEIGHTS_SCRIPT");
    if (!raw || !raw[0]) {
        m_weightsScriptRan = true;
        return;
    }
    // 模型异步加载：没有骨架的帧要等，不能把脚本标成"跑过了"
    if (!m_model || !m_model->skeleton) return;
    m_weightsScriptRan = true;
    ensureWeights();
    m_focusWeightsTab = true;   // 页签不被选中就没有锚点（F 组会整片红）

    int applied = 0;
    int failed = 0;
    const auto split = [](const std::string& s) {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos <= s.size()) {
            const std::size_t p = s.find(':', pos);
            out.push_back(s.substr(pos, p == std::string::npos ? std::string::npos
                                                              : p - pos));
            if (p == std::string::npos) break;
            pos = p + 1;
        }
        return out;
    };
    const auto toF = [](const std::string& s, float def) {
        try { return std::stof(s); } catch (...) { return def; }
    };
    const auto toI = [](const std::string& s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    };

    const std::string all(raw);
    std::size_t pos = 0;
    while (pos <= all.size()) {
        const std::size_t semi = all.find(';', pos);
        std::string tok = all.substr(pos, semi == std::string::npos
                                              ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? all.size() + 1 : semi + 1;
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
            tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
            tok.pop_back();
        if (tok.empty()) continue;

        const std::vector<std::string> a = split(tok);
        const std::string& cmd = a[0];
        bool ok = false;
        const int s = activeWeightSet();

        if (cmd == "list") {
            VK_LOG_INFO("WEIGHTS-LIST meshes=%d active=%d bones=%d deformOn=%d",
                        weightSetCount(), s, m_rig.boneCount(),
                        static_cast<int>(m_rig.info().bones - m_rig.info().deformOff));
            for (int i = 0; i < weightSetCount(); ++i) {
                const animation::WeightSet& w = m_weights[static_cast<std::size_t>(i)];
                const animation::WeightStats st2 = w.stats();
                VK_LOG_INFO("WEIGHTS-LIST-MESH i=%d name='%s' active=%d vtx=%d "
                            "touched=%d unweighted=%d",
                            i, w.mesh.c_str(),
                            static_cast<int>(m_weightsActive[static_cast<std::size_t>(i)]),
                            st2.vertices, st2.touched, st2.unweighted);
            }
            ok = true;
        } else if (cmd == "mesh" && a.size() >= 2) {
            const int want = toI(a[1], -1);
            if (want >= 0 && want < weightSetCount()) {
                m_weightSel = want;
                m_weightCacheSet = -1;
                ok = true;
            }
        } else if (cmd == "sel" && a.size() >= 2) {
            // 目标骨（刷子/色带都看它）
            const int bi = toI(a[1], -1);
            if (bi >= 0 && bi < m_rig.boneCount()) {
                m_rigSel = bi;
                ok = true;
            }
        } else if ((cmd == "auto" || cmd == "autoall")) {
            if (a.size() >= 2) m_autoParams.radiusScale = toF(a[1], m_autoParams.radiusScale);
            if (a.size() >= 3) m_autoParams.falloff = toF(a[2], m_autoParams.falloff);
            if (a.size() >= 4) m_autoParams.maxInfluence = toI(a[3], m_autoParams.maxInfluence);
            const glm::mat4 m2r = glm::inverse(m_model->skeleton->invMeshNode);
            int done = 0;
            for (int i = 0; i < weightSetCount(); ++i) {
                if (cmd == "auto" && i != s) continue;
                if (!m_weightsActive[static_cast<std::size_t>(i)]) continue;
                assets::Mesh* mesh = m_model->subMeshes[static_cast<std::size_t>(i)].mesh;
                if (mesh == nullptr) continue;
                std::vector<glm::vec3> vp;
                animation::meshVertexPositions(*mesh, vp);
                const int got = m_weights[static_cast<std::size_t>(i)].autoAssign(
                    m_rig, vp, m2r, m_autoParams);
                if (got > 0) {
                    m_weightsDirty[static_cast<std::size_t>(i)] = 1;
                    ++done;
                }
                VK_LOG_INFO("WEIGHTS-AUTO mesh=%d assigned=%d", i, got);
            }
            m_weightCacheSet = -1;
            ok = done > 0;
        } else if (cmd == "paint" && a.size() >= 4 && s >= 0) {
            // paint:<vertex>:<bone>:<weight>
            const int v = toI(a[1], -1);
            const int b = toI(a[2], -1);
            const float w = toF(a[3], 0.0f);
            if (v >= 0 && v < m_weights[static_cast<std::size_t>(s)].vertexCount &&
                b >= 0 && b < m_rig.boneCount()) {
                const int n = m_weights[static_cast<std::size_t>(s)].paintVertex(
                    v, b, w, static_cast<animation::PaintMode>(m_brushMode),
                    m_brushAutoNormalize);
                if (n > 0) m_weightsDirty[static_cast<std::size_t>(s)] = 1;
                ok = n > 0;
            }
        } else if (cmd == "mode" && a.size() >= 2) {
            m_brushMode = std::max(0, std::min(2, toI(a[1], 0)));
            ok = true;
        } else if (cmd == "autonorm" && a.size() >= 2) {
            // 对应 UI 的 Auto normalize 勾选框。关掉之后一次刷子只有
            // "加/减/设"本身，权重和可以暂时不等于 1 —— 验收靠它把
            // 三种模式的效果和"归一化"这件事分开测。
            m_brushAutoNormalize = a[1] != "0";
            ok = true;
        } else if (cmd == "norm" && s >= 0) {
            const int c = m_weights[static_cast<std::size_t>(s)].normalizeAll();
            if (c > 0) m_weightsDirty[static_cast<std::size_t>(s)] = 1;
            VK_LOG_INFO("WEIGHTS-NORMALIZE mesh=%d fixed=%d", s, c);
            ok = true;
        } else if (cmd == "blur" && s >= 0) {
            ensureWeightCaches();
            const int rounds = a.size() >= 2 ? toI(a[1], 1) : 1;
            const float stg = a.size() >= 3 ? toF(a[2], 0.5f) : 0.5f;
            const int c = m_weights[static_cast<std::size_t>(s)].blur(
                m_weightRingOff, m_weightRingIdx, rounds, stg);
            if (c > 0) m_weightsDirty[static_cast<std::size_t>(s)] = 1;
            VK_LOG_INFO("WEIGHTS-BLUR mesh=%d affected=%d", s, c);
            ok = c > 0;
        } else if (cmd == "clear" && a.size() >= 2 && s >= 0) {
            const int b = toI(a[1], -1);
            const int c = m_weights[static_cast<std::size_t>(s)].clearBone(b);
            if (c > 0) m_weightsDirty[static_cast<std::size_t>(s)] = 1;
            VK_LOG_INFO("WEIGHTS-CLEARBONE mesh=%d bone=%d verts=%d", s, b, c);
            ok = c > 0;
        } else if (cmd == "axis" && a.size() >= 2) {
            // 镜像轴的**正平面法线**（要取反的那个分量）。默认 1(Y) 是按
            // 老资产定的；换成 FAB 角色（左/右 = X）之后 0 才对，而脚本
            // 之前没有命令能改它 —— 于是 `mirror` 一路 `to=-1`，看起来
            // 像"镜像功能坏了"。给脚本一个开关，别让轴只能靠 UI 拖。
            m_rig.mirrorAxis = std::max(0, std::min(2, toI(a[1], 1)));
            VK_LOG_INFO("WEIGHTS-MIRROR-AXIS axis=%s",
                        animation::RigEdit::axisName(m_rig.mirrorAxis));
            ok = true;
        } else if (cmd == "mirror" && a.size() >= 2 && s >= 0) {
            ensureWeightCaches();
            const int v = toI(a[1], -1);
            const int k = m_weights[static_cast<std::size_t>(s)].mirrorVertex(
                m_rig, m_rig.mirrorAxis, m_weightPos, v, kWeightMirrorEps);
            if (k >= 0) m_weightsDirty[static_cast<std::size_t>(s)] = 1;
            VK_LOG_INFO("WEIGHTS-MIRROR from=%d to=%d axis=%s", v, k,
                        animation::RigEdit::axisName(m_rig.mirrorAxis));
            ok = k >= 0;
        } else if (cmd == "bake") {
            for (std::size_t i = 0; i < m_weightsDirty.size(); ++i)
                if (m_weightsActive[i]) m_weightsDirty[i] = 1;
            applyWeights();
            ok = true;
        } else if (cmd == "digest" && s >= 0) {
            const animation::WeightSet& w = m_weights[static_cast<std::size_t>(s)];
            VK_LOG_INFO("WEIGHTS-DIGEST mesh=%d name='%s' h=%llu vtx=%d", s,
                        w.mesh.c_str(), w.digest(), w.vertexCount);
            ok = true;
        } else if (cmd == "dump" && a.size() >= 2 && s >= 0) {
            m_weights[static_cast<std::size_t>(s)].dumpVertex(toI(a[1], -1), "script");
            ok = true;
        } else if (cmd == "stats" && s >= 0) {
            m_weights[static_cast<std::size_t>(s)].logSummary("script");
            ok = true;
        } else if (cmd == "save" && a.size() >= 2) {
            std::string err;
            const std::string path = tok.substr(5);
            ok = animation::saveWeights(path, m_weights[static_cast<std::size_t>(s)], err);
            if (!ok) VK_LOG_WARN("WEIGHTS-SCRIPT-SAVE-FAIL err='%s'", err.c_str());
        } else if (cmd == "load" && a.size() >= 2) {
            std::string err;
            const std::string path = tok.substr(5);
            animation::WeightSet tmp;
            ok = animation::loadWeights(path, tmp, err) && s >= 0 &&
                 tmp.vertexCount == m_weights[static_cast<std::size_t>(s)].vertexCount;
            if (ok) {
                const std::string keep = m_weights[static_cast<std::size_t>(s)].mesh;
                m_weights[static_cast<std::size_t>(s)] = std::move(tmp);
                m_weights[static_cast<std::size_t>(s)].mesh = keep;
                m_weightsDirty[static_cast<std::size_t>(s)] = 1;
                m_weightCacheSet = -1;
            } else {
                VK_LOG_WARN("WEIGHTS-SCRIPT-LOAD-FAIL err='%s'", err.c_str());
            }
        } else if (cmd == "revert") {
            revertWeights();
            ok = true;
        }

        if (ok) ++applied; else ++failed;
        if (!ok) VK_LOG_WARN("WEIGHTS-SCRIPT-FAIL cmd='%s'", tok.c_str());
    }

    // 收尾：把还有脏的一次烤完（脚本里没显式写 bake 也能上屏）
    bool anyDirty = false;
    for (char d : m_weightsDirty) if (d) anyDirty = true;
    if (anyDirty) applyWeights();

    int dirty = 0;
    for (char d : m_weightsDirty) if (d) ++dirty;
    VK_LOG_INFO("WEIGHTS-SCRIPT applied=%d failed=%d meshes=%d active=%d sel=%d "
                "dirty=%d",
                applied, failed, weightSetCount(), activeWeightSet(), m_rigSel, dirty);
}

void CharacterWindow::draw() {
    if (!m_open) return;
    // Weights 页的"这一帧画过吗"：页签内容只在被选中时绘制，于是它天然
    // 就是"当前页是不是 Weights"的信号 —— 不用再另存一个当前页下标，
    // 那样会有两个真相源（关掉再打开时对不上）。
    m_weightsTabOpen = false;
    if (m_previewDirty) rebuildPreviewScene();

    // 每帧推进动画时间（速度 / 循环都在这里）
    const assets::AnimationClip* curClip =
        (m_model && m_model->skeleton)
            ? m_model->skeleton->findClip(m_currentClip)
            : nullptr;
    // Pose Mode 下时间轴的"长度"是**编辑资产自己的 duration**，不是当前
    // 预览片段的：作者在这段时间里打帧，播放头就该在这段时间里循环。
    // 拿 base clip 的长度会让"我明明在 3.2s 打了帧，播放头到 1.0s 就绕回 0"。
    const float timelineLen =
        m_poseMode ? std::max(m_edit.duration, 1.0e-3f)
                   : (curClip ? curClip->duration : 0.0f);
    if (m_animPlaying && timelineLen > 0.0f) {
        m_animTime += ImGui::GetIO().DeltaTime * m_animSpeed;
        if (m_animTime > timelineLen) {
            if (m_animLoop) {
                m_animTime = std::fmod(m_animTime, timelineLen);
            } else {
                m_animTime = timelineLen;
                m_animPlaying = false;
            }
        }
    }

    // 采样当前姿势 —— Details 的骨骼世界位置、gizmo、预览蒙皮都靠它。
    //
    // 2026-10-28（R7）：这里原来直接调 `Skeleton::sample(clip, t, out)`。
    // 那是"采样 + FK + 乘 invBind"一步到底，**中间没有插入点**，所以没法
    // 把打好的帧叠上去。改成三段式（samplePose → overlayEditPose →
    // poseToJoints）之后，`sample()` 在数学上就是它的兼容壳，没编辑数据时
    // 输出逐位相同（见 animation/Pose.h 的头注释与 MYVK_ANIM_TEST）。
    samplePoseInto();
    // 验收脚本（MYVK_EDITOR_POSE_SCRIPT）在采样之后跑：它要读 m_pose，
    // 也会自己重新采样（`sample:` / `key:` 会改 m_animTime）。
    runPoseScript();

    // 把动画状态同步给预览场景里的 SkinnedMeshComponent
    syncPreviewSkinning();

    // 摄影表（R8）：Pose Mode 下把这份 m_edit 推过去，退出时解绑。
    // 排在采样之后 —— 摄影表要在同一帧里读到刚打的帧。
    syncDopeSheet();

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

    // 骨架编辑的验收注入（`MYVK_EDITOR_RIG_SCRIPT`）。放在**窗口最前**跑，
    // 这样同一帧的采样/绘制就已经是新骨架 —— 否则脚本改了 rest，画面要等
    // 下一帧才跟上，截图断言会拍到一个中间态。
    runRigScript();
    // 权重的验收注入（`MYVK_EDITOR_WEIGHTS_SCRIPT`）。同样放在最前 ——
    // 脚本刷完权重要在同一帧上传，截图断言才拍得到。
    runWeightsScript();
    // 约束栈的验收注入（`MYVK_EDITOR_CONSTRAINT_SCRIPT`）。同样在最前 ——
    // 脚本加的约束要在同一帧的采样里生效，否则断言拿到的是上一帧的姿态。
    runConstraintScript();

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
    // ---- 左栏宽度 ----
    // 300 是"刚好装下 Skeleton|Mesh|Rig|Animation"的数（自然宽 63+48+41+80 加
    // 间距 = 253，看着还有余量，其实余量全在 ImGui 的 TabMinWidthShrink=80 里）。
    // S-B 加进 Weights 之后总宽 316 > 300，第 5 个页签直接溢出到页签栏外面 ——
    // 表现是"'Animation' 点了没反应"（矩形还在，但在栏外，鼠标点的是空白），
    // 2026-10-29 被 verify_character_window 的 CHAR-CLIP 抓到。
    // 340 让 5 个页签全部按自然宽显示，不靠截断"…"硬挤：截断的 "Skele…"
    // 比多占 40px 难看得多。带骨架的模型才需要这几十 px（Weights 页只对有蒙皮
    // 的模型出现），静态网格还是 300。
    // ⚠ 380 而不是 340：实测 5 个页签的自然宽合计 365（Skeleton 79 / Mesh 57 /
    //   Rig 41 / Weights 80 / Animation 80 加四个间距），340 的左栏只有 349，
    //   还差 16px —— ImGui 的 `TabMinWidthShrink=80` 是**下限**，压不到 80 以下
    //   就只能溢出，于是又变成"最后一个页签点不到"。多出来的 40px 从右栏（
    //   rightW 296 → 由 avail 算出来 254）拿，预览宽度一点没变。
    const float leftW = (m_model && m_model->skeleton) ? 380.0f : 300.0f;
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
        // Rig 页（S-A）：只要模型带骨架就常驻 —— 它不是"某种模式"，而是
        // "改这具骨架"的那一页，和 Skeleton 页（只读查看）是两件事。
        if (m_model && m_model->skeleton) {
            // 脚本注入时切过来：页签内容只在被选中时绘制，不切就没有锚点，
            // 验收 F 组会整片红（而那是"测试看不见"，不是"功能没有"）。
            const ImGuiTabItemFlags rf =
                m_focusRigTab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(TR("Rig"), nullptr, rf)) {
                drawRigPanel();
                ImGui::EndTabItem();
            }
            logRect("CHAR-TAB-Rig", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
            m_focusRigTab = false;
        }
        // Weights 页（S-B）：只对**有蒙皮**的模型出现。权重绘制对静态网格
        // 没有意义（没有骨可绑），留一个空页只会让人以为坏了。
        if (m_model && m_model->skeleton) {
            const ImGuiTabItemFlags wf =
                m_focusWeightsTab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(TR("Weights"), nullptr, wf)) {
                drawWeightsPanel();
                ImGui::EndTabItem();
            }
            logRect("CHAR-TAB-Weights", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
            m_focusWeightsTab = false;
        }
        if (ImGui::BeginTabItem(TR("Animation"))) {
            drawAnimationPanel();
            ImGui::EndTabItem();
        }
        logRect("CHAR-TAB-Animation", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        // Pose 页只在 Pose Mode 下出现：它不是"看资产"的一页，是"摆姿势"
        // 的一页，关掉 Pose Mode 还留着一个空页会让左栏多一个点开没内容的坑。
        if (m_poseMode) {
            // 开 Pose Mode 的那一帧把页签切过来。用 SetSelected 而不是
            // "记一个当前页索引"：页签的选中态归 ImGui 管，自己再存一份
            // 就有了两个真相源（关掉再打开时对不上）。
            const ImGuiTabItemFlags pf =
                m_focusPoseTab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(TR("Pose"), nullptr, pf)) {
                drawPosePanel();
                ImGui::EndTabItem();
            }
            logRect("CHAR-TAB-Pose", ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
            m_focusPoseTab = false;
        }
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

