#include "CharacterWindow.h"

#include "DebugRects.h"
#include "EditorDragDrop.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Model.h"
#include "assets/Skeleton.h"
#include "core/Logger.h"
#include "render/Renderer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
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

std::string pathStem(const std::string& p) {
    const std::string s = std::filesystem::path(p).stem().string();
    return s.empty() ? std::string("Model") : s;
}

// 从路径加载模型（按扩展名分派 FBX / glTF），失败返回 null。
assets::Model* loadModelByPath(assets::AssetManager& am,
                               const std::string& relPath) {
    const std::string resolved = assets::resolveAssetPath(relPath);
    if (isFbxPath(relPath)) {
        return am.loadFBX(resolved, relPath);
    }
    return am.loadGLTF(resolved, relPath);
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

// 把 Model 实例化进目标场景的父节点下。蒙皮模型（skeleton 非空）会在父节点
// 挂 SkinnedMeshComponent；静态模型只建普通 MeshComponent 实体。
std::vector<ecs::Entity> CharacterWindow::instanceModelInto(scene::Scene& scene,
                                                            assets::Model* model,
                                                            ecs::Entity parent) {
    std::vector<ecs::Entity> out;
    if (!model || !model->valid()) return out;

    ecs::World& w = scene.world();

    for (const auto& sub : model->subMeshes) {
        if (!sub.mesh) continue;
        ecs::Entity e = scene.createObject(sub.name.empty() ? "mesh" : sub.name);

        ecs::MeshComponent mc;
        mc.mesh = sub.mesh;
        w.add<ecs::MeshComponent>(e, mc);
        w.add<ecs::MaterialComponent>(e, ecs::MaterialComponent{sub.material});

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
        if (parent.valid()) scene.setParent(e, parent);
        out.push_back(e);
    }

    // 蒙皮：在父节点（模型根）挂 SkinnedMeshComponent
    if (model->skinned() && parent.valid()) {
        ecs::SkinnedMeshComponent sk;
        sk.model = model;
        sk.clipName.clear();
        sk.time = 0.0f;
        sk.speed = 1.0f;
        sk.playing = false;
        if (auto* p = w.get<ecs::SkinnedMeshComponent>(parent))
            *p = sk;
        else
            w.add<ecs::SkinnedMeshComponent>(parent, sk);
    }
    return out;
}

void CharacterWindow::rebuildPreviewScene() {
    m_previewScene.clear();
    m_previewRoot = ecs::Entity{};
    if (!m_model || !m_model->valid()) {
        m_previewDirty = false;
        return;
    }

    // 预览场景：一盏方向光（柔和，环绕照亮角色）+ 模型 + 独立相机
    scene::Light& L = m_previewScene.light();
    L.direction = glm::normalize(glm::vec3(-0.4f, -0.5f, -0.8f));
    L.color = glm::vec3(1.0f, 0.97f, 0.92f);
    L.intensity = 1.8f;
    L.castsShadow = true;
    L.ambientScale = 0.6f;

    m_previewRoot = m_previewScene.createObject("PreviewRoot");
    // 根放在原点；模型自己的 submesh 变换会相对它摆
    instanceModelInto(m_previewScene, m_model, m_previewRoot);

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

    // 预览相机：绕模型包围球摆到合适距离
    const float radius = m_model->radius > 0.0f ? m_model->radius : 1.0f;
    m_previewDistance = radius * 3.0f;
    m_previewTarget = m_model->center;
    scene::Camera& cam = m_previewScene.camera();
    cam.setYaw(m_previewYaw);
    cam.setPitch(m_previewPitch);
    cam.setDistance(m_previewDistance);
    cam.setTarget(m_previewTarget);

    m_previewDirty = false;
    VK_LOG_INFO("CharacterWindow: preview scene rebuilt (model=%s, %zu "
                "submeshes, skinned=%d)",
                m_model->name.c_str(), m_model->subMeshes.size(),
                m_model->skinned() ? 1 : 0);
}

// ---------------------------------------------------------------- 模型加载
// （loadModelByPath 在文件顶部的匿名命名空间里，供 open() 与拖拽共用）

// ---------------------------------------------------------------- UI

void CharacterWindow::handlePreviewNavigation(const glm::vec2& vpPos,
                                              const glm::vec2& vpSize,
                                              bool hovered) {
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

void CharacterWindow::drawPreviewViewport() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const uint32_t wantW = static_cast<uint32_t>(std::max(avail.x, kMinPreview));
    const uint32_t wantH = static_cast<uint32_t>(std::max(avail.y, kMinPreview));

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
                    IM_COL32(140, 148, 158, 220), "building preview...");
    }

    const ImVec2 imgMin = ImGui::GetItemRectMin();
    const ImVec2 imgSz = ImGui::GetItemRectSize();
    const bool hovered = ImGui::IsItemHovered();
    handlePreviewNavigation(glm::vec2(imgMin.x, imgMin.y),
                            glm::vec2(imgSz.x, imgSz.y), hovered);

    // 底部操作提示
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(imgMin.x + 8.0f, imgMin.y + imgSz.y - 18.0f),
                IM_COL32(150, 158, 168, 220),
                "RMB: orbit   MMB: pan   wheel: dolly");
}

void CharacterWindow::drawSkeletonPanel() {
    if (!m_model || !m_model->skeleton) {
        ImGui::TextDisabled("(no skeleton - static mesh)");
        return;
    }
    const assets::Skeleton& skel = *m_model->skeleton;
    const int n = skel.jointCount();
    if (n <= 0) {
        ImGui::TextDisabled("(empty skeleton)");
        return;
    }

    if (m_jointExpanded.size() != static_cast<size_t>(n))
        m_jointExpanded.assign(static_cast<size_t>(n), 1);  // 默认展开

    // 递归绘制骨骼树。用简单的前向遍历：先收集每个 joint 的子节点。
    std::vector<std::vector<int>> children(static_cast<size_t>(n));
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        const int p = skel.joints[static_cast<size_t>(i)].parent;
        if (p < 0) roots.push_back(i);
        else if (p < n)
            children[static_cast<size_t>(p)].push_back(i);
    }

    std::function<void(int, int)> drawJoint = [&](int idx, int depth) {
        const auto& j = skel.joints[static_cast<size_t>(idx)];
        const bool hasKids = !children[static_cast<size_t>(idx)].empty();
        ImGui::PushID(idx);
        ImGui::Indent(static_cast<float>(depth) * 14.0f);
        const char* label = j.name.empty() ? "(unnamed)" : j.name.c_str();
        if (hasKids) {
            if (ImGui::SmallButton(m_jointExpanded[static_cast<size_t>(idx)]
                                       ? "-"
                                       : "+")) {
                m_jointExpanded[static_cast<size_t>(idx)] =
                    !m_jointExpanded[static_cast<size_t>(idx)];
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
        } else {
            ImGui::Text("  %s", label);
        }
        ImGui::Unindent(static_cast<float>(depth) * 14.0f);
        ImGui::PopID();

        if (m_jointExpanded[static_cast<size_t>(idx)]) {
            for (int c : children[static_cast<size_t>(idx)])
                drawJoint(c, depth + 1);
        }
    };

    ImGui::TextDisabled("Joints: %d", n);
    for (int r : roots) drawJoint(r, 0);
}

void CharacterWindow::drawMeshPanel() {
    ImGui::Text("Model: %s", m_model ? m_model->name.c_str() : "(none)");
    if (m_model) {
        ImGui::TextDisabled("submeshes: %zu  %s", m_model->subMeshes.size(),
                            m_model->skinned() ? "SKINNED" : "STATIC");
        if (m_model->skeleton) {
            ImGui::TextDisabled("clips: %zu",
                                m_model->skeleton->clips.size());
        }
    }

    ImGui::Separator();

    // 拖入模型文件来绑定
    ImGui::TextDisabled("Drop a .fbx / .glb / .gltf here to bind a model.");
    // 自动化用（MYVK_LOG_RECTS=1）：拖拽目标区矩形 + Bind 按钮矩形
    logRect("CHAR-DROP", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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
                    m_ctx.notify("Bound model: " + std::string(path));
                } else {
                    m_ctx.notify("Failed to load model: " +
                                 std::string(path));
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // 绑定到玩家（把模型实例化进主编辑场景的玩家下）
    if (m_model) {
        if (ImGui::Button("Bind to player")) {
            bindModelToPlayer();
        }
        logRect("CHAR-BIND", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
}

void CharacterWindow::drawAnimationPanel() {
    if (!m_model || !m_model->skeleton) {
        ImGui::TextDisabled("(no animations - static mesh)");
        return;
    }
    const assets::Skeleton& skel = *m_model->skeleton;
    if (skel.clips.empty()) {
        ImGui::TextDisabled("(no animation clips)");
        return;
    }

    // 动画列表：点击即播放
    for (const auto& clip : skel.clips) {
        const bool selected = (clip.name == m_currentClip);
        if (ImGui::Selectable(clip.name.c_str(), selected)) {
            playClip(clip.name);
        }
        logRect(("CHAR-CLIP-" + clip.name).c_str(), ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
    }

    ImGui::Separator();

    // 播放控制
    if (!m_currentClip.empty()) {
        ImGui::TextDisabled("Playing: %s", m_currentClip.c_str());
        const assets::AnimationClip* clip = skel.findClip(m_currentClip);
        const float dur = clip ? clip->duration : 0.0f;

        if (m_animPlaying) {
            if (ImGui::Button("Pause")) m_animPlaying = false;
        } else {
            if (ImGui::Button("Play")) m_animPlaying = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            m_animPlaying = false;
            m_animTime = 0.0f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("t=%.2f/%.2fs", static_cast<double>(m_animTime),
                            static_cast<double>(dur));

        // 时间轴拖动
        if (dur > 0.0f) {
            if (ImGui::SliderFloat("##timeline", &m_animTime, 0.0f, dur,
                                   "%.2f")) {
                // 拖动时暂停，方便定格查看姿势
                m_animPlaying = false;
            }
        }
    } else {
        ImGui::TextDisabled("Select a clip to preview.");
    }
}

void CharacterWindow::playClip(const std::string& clipName) {
    m_currentClip = clipName;
    m_animTime = 0.0f;
    m_animPlaying = true;
    VK_LOG_INFO("CharacterWindow: play clip '%s'", clipName.c_str());
}

void CharacterWindow::bindModelToPlayer() {
    if (!m_model || !m_player.valid()) return;
    scene::Scene& sc = m_ctx.editorScene();

    // 先清掉玩家下已有的模型子节点（避免重复绑定堆叠）
    // 保留 PlayerComponent 与 CollisionComponent（胶囊碰撞体）。
    m_ctx.structuralEdit("Bind character model", [&]() {
        ecs::World& w = sc.world();
        // 删除玩家名下的所有带网格的子实体（上一轮绑定的模型）
        if (auto* h = w.get<ecs::HierarchyComponent>(m_player)) {
            auto kids = h->children;
            for (auto kidId : kids) {
                ecs::Entity kid = w.handle(kidId);
                if (!kid.valid()) continue;
                if (w.has<ecs::MeshComponent>(kid)) w.destroy(kid);
            }
        }
        instanceModelInto(sc, m_model, m_player);
    });
    m_ctx.dirty() = true;
    m_ctx.notify("Bound model to player");
    VK_LOG_INFO("CharacterWindow: bound model '%s' to player '%s'",
                m_model->name.c_str(), m_playerName.c_str());
}

// ---------------------------------------------------------------- 主流程

void CharacterWindow::draw() {
    if (!m_open) return;
    if (m_previewDirty) rebuildPreviewScene();

    // 每帧推进动画时间
    if (m_animPlaying && m_model && m_model->skeleton) {
        const assets::AnimationClip* clip =
            m_model->skeleton->findClip(m_currentClip);
        if (clip) {
            m_animTime += ImGui::GetIO().DeltaTime;
            if (m_animTime > clip->duration && clip->duration > 0.0f)
                m_animTime = 0.0f;  // 循环
        }
    }

    // 把动画状态同步给预览场景里的 SkinnedMeshComponent
    if (m_previewRoot.valid() && m_model && m_model->skinned()) {
        if (auto* sk = m_previewScene.world().get<ecs::SkinnedMeshComponent>(
                m_previewRoot)) {
            sk->clipName = m_currentClip;
            sk->time = m_animTime;
            sk->playing = m_animPlaying;
        }
    }

    // ---- 窗口 ----
    ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Character###character", &m_open)) {
        ImGui::End();
        return;
    }

    // 关闭时清理
    if (!m_open) {
        close();
        ImGui::End();
        return;
    }

    ImGui::Text("Character: %s", m_playerName.c_str());
    ImGui::Separator();

    // 左侧属性栏 + 右侧预览视口（UE5 Persona 布局）。
    // 左栏要足够宽，让 Skeleton/Mesh/Animation 三个标签都放得下
    // （太窄会溢出到 ">>" 菜单里，预览视口也被挤没了）。
    ImGui::BeginChild("##char_left", ImVec2(280.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("##char_tabs")) {
        if (ImGui::BeginTabItem("Skeleton")) {
            drawSkeletonPanel();
            ImGui::EndTabItem();
        }
        logRect("CHAR-TAB-Skeleton", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::BeginTabItem("Mesh")) {
            drawMeshPanel();
            ImGui::EndTabItem();
        }
        logRect("CHAR-TAB-Mesh", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        if (ImGui::BeginTabItem("Animation")) {
            drawAnimationPanel();
            ImGui::EndTabItem();
        }
        logRect("CHAR-TAB-Animation", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##char_preview", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    drawPreviewViewport();
    ImGui::EndChild();

    ImGui::End();
}

} // namespace editor
