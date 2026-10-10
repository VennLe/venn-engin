#include "InspectorPanel.h"

#include "DebugRects.h"
#include "EditorDragDrop.h"
#include "PrimitiveBuilder.h"

#include "assets/AssetManager.h"
#include "assets/AssetPath.h"
#include "assets/Material.h"
#include "assets/Mesh.h"
#include "assets/Texture.h"
#include "core/Logger.h"
#include "ecs/Components.h"
#include "physics/CollisionWorld.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <unordered_map>

namespace editor {

namespace {

// 小工具：一个带 tooltip 的灰色说明行
void hint(const char* text) {
    // 换行显示：侧栏不宽，长提示不换行就会被右边缘裁掉（原来是 TextDisabled
    // 单行，实测 "Click an object in the viewport (or in the Hierarchy)…"
    // 正好被切掉半句）。
    const ImVec4 col = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// 组件的"移除"按钮（跟在同行控件后面）
bool removeButton() {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
    const bool clicked = ImGui::SmallButton("Remove");
    ImGui::PopStyleColor();
    return clicked;
}

} // namespace

// ---------------------------------------------------------------- 主流程

void InspectorPanel::draw() {
    EditorContext::PanelVisibility& panels = m_ctx.panels();
    if (!panels.inspector) return;

    const LayoutRects& L = m_ctx.layout();
    // 分栏布局：每帧跟随分栏树，不允许手动拖动窗口
    ImGui::SetNextWindowPos(ImVec2(L.inspector.x, L.inspector.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L.inspector.w, L.inspector.h),
                             ImGuiCond_Always);

    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("Inspector", &panels.inspector, wf)) {
        ImGui::End();
        return;
    }

    scene::Scene& sc = m_ctx.activeScene();
    const ecs::Entity e = m_ctx.selection();

    if (!m_ctx.hasSelection()) {
        drawSceneryFallback();
        ImGui::End();
        return;
    }

    const std::string* np = sc.world().name(e);
    ImGui::Text("Entity: %s", np ? np->c_str() : "(unnamed)");
    ImGui::SameLine();
    ImGui::TextDisabled("#%u.%u", e.id, e.generation);

    if (!m_ctx.isEditing()) {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(1.0f, 0.72f, 0.3f, 1.0f),
            "  (playing - showing the runtime copy, not the editable scene)");
    }

    ImGui::Separator();

    drawTransform(e);
    drawVisibility(e);
    drawMeshAndMaterial(e);
    drawLights(e);
    drawScript(e);
    drawAddRemove(e);

    ImGui::End();
}

// ---------------------------------------------------------------- 兜底视图

void InspectorPanel::drawSceneryFallback() {
    ImGui::TextDisabled("Nothing selected.");
    hint("Click an object in the viewport (or in the Hierarchy) to edit it.");
    ImGui::Separator();

    scene::Scene& sc = m_ctx.editorScene();

    // 相机与主光不进撤销栈：视口导航本来就是"看一眼就过去了"的操作，
    // 把它们塞进历史只会把有用的历史挤掉。
    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        scene::Camera& cam = sc.camera();
        glm::vec3 t = cam.target();
        if (ImGui::DragFloat3("Target", glm::value_ptr(t), 0.05f)) {
            cam.setTarget(t);
        }
        float dist = cam.distance();
        if (ImGui::DragFloat("Distance", &dist, 0.1f, 0.5f, 200.0f)) {
            cam.setDistance(dist);
        }
        float fov = cam.fov();
        if (ImGui::SliderFloat("FOV", &fov, 15.0f, 110.0f)) cam.setFov(fov);

        // yaw / pitch 是角度，给个直观的滑块
        float yawDeg = glm::degrees(cam.yaw());
        if (ImGui::SliderFloat("Yaw", &yawDeg, -180.0f, 180.0f, "%.1f deg")) {
            cam.setYaw(glm::radians(yawDeg));
        }
        float pitchDeg = glm::degrees(cam.pitch());
        if (ImGui::SliderFloat("Pitch", &pitchDeg, -85.0f, 85.0f, "%.1f deg")) {
            cam.setPitch(glm::radians(pitchDeg));
        }
    }

    if (ImGui::CollapsingHeader("Directional Light",
                               ImGuiTreeNodeFlags_DefaultOpen)) {
        scene::Light& L = sc.light();
        const glm::vec3 db = L.direction;
        ImGui::DragFloat3("Direction", glm::value_ptr(L.direction), 0.01f, -1.0f,
                          1.0f);
        discreteEdit(m_ctx, "Sun Direction", &L.direction, db);

        const glm::vec3 cb = L.color;
        ImGui::ColorEdit3("Color", glm::value_ptr(L.color));
        discreteEdit(m_ctx, "Sun Color", &L.color, cb);

        const float ib = L.intensity;
        ImGui::SliderFloat("Intensity", &L.intensity, 0.0f, 4.0f);
        discreteEdit(m_ctx, "Sun Intensity", &L.intensity, ib);

        const float ab = L.ambientScale;
        ImGui::SliderFloat("Ambient", &L.ambientScale, 0.0f, 1.0f, "%.2f");
        discreteEdit(m_ctx, "Ambient Scale", &L.ambientScale, ab);
        hint("Low ambient = indoor mood; 0.55 is a good outdoor default");
    }

    if (ImGui::CollapsingHeader("Scene")) {
        ImGui::Text("Entities: %zu", sc.objectCount());
        const scene::Scene::LightStats ls = sc.lightStats();
        ImGui::Text("Local lights: %zu (%zu point / %zu spot)",
                    ls.pointLights + ls.spotLights, ls.pointLights,
                    ls.spotLights);
        ImGui::Text("Editor commands: %zu (cursor %zu)",
                    m_ctx.commands().depth(), m_ctx.commands().cursor());
    }
}

// ---------------------------------------------------------------- 变换

void InspectorPanel::drawTransform(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    auto* t = sc.world().get<ecs::TransformComponent>(e);
    if (!t) return;

    if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const std::string* np = sc.world().name(e);
    const std::string who = np ? *np : std::string("Object");

    // ---- 锁定开关：上锁 = 视口点不到 + 不给变换手柄 ----
    // 引擎自带的地面默认就是锁着的（它是参照系，不该被平移/缩放/旋转）。
    // 想调整地面，先在这里把勾去掉 —— 所以这个开关是"出口"，不能省。
    const bool locked = sc.world().has<ecs::LockedComponent>(e);
    ImGui::BeginDisabled(!m_ctx.isEditing());
    bool lockFlag = locked;
    if (ImGui::Checkbox("Locked", &lockFlag) && m_ctx.isEditing()) {
        const char* label = lockFlag ? "Lock " : "Unlock ";
        m_ctx.structuralEdit(std::string(label) + who, [&] {
            if (lockFlag) {
                sc.world().add<ecs::LockedComponent>(e);
            } else {
                sc.world().remove<ecs::LockedComponent>(e);
            }
        });
        m_ctx.setStatus(std::string(lockFlag ? "Locked " : "Unlocked ") + who);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("上锁 = 视口点不到 + 不给变换手柄（自带地面默认锁着）。\n"
                          "要调整地面就先取消这个勾。");
    }
    ImGui::SameLine();
    ImGui::TextDisabled(locked ? "(fixed, cannot be picked or transformed)"
                               : "(unlocked)");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(locked);

    // 位置
    const glm::vec3 pb = t->position;
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::DragFloat3("Position", glm::value_ptr(t->position), 0.02f);
    fieldEdit(m_ctx, ("Move " + who).c_str(), &t->position, pb);
    ImGui::SameLine();
    if (ImGui::SmallButton("R##pos") && m_ctx.isEditing()) {
        const glm::vec3 zero(0.0f);
        m_ctx.commands().pushAlreadyApplied(
            std::make_unique<RawBytesEditCommand>("Reset Position",
                                                  &t->position, &t->position,
                                                  &zero, sizeof(glm::vec3)));
        m_ctx.dirty() = true;
    }

    // 旋转（内部是弧度，界面上用角度）
    const glm::vec3 rb = t->rotation;
    glm::vec3 rotDeg = glm::degrees(rb);
    ImGui::SetNextItemWidth(-90.0f);
    if (ImGui::DragFloat3("Rotation", glm::value_ptr(rotDeg), 0.5f)) {
        t->rotation = glm::radians(rotDeg);
    }
    fieldEdit(m_ctx, ("Rotate " + who).c_str(), &t->rotation, rb);
    ImGui::SameLine();
    if (ImGui::SmallButton("R##rot") && m_ctx.isEditing()) {
        const glm::vec3 zero(0.0f);
        m_ctx.commands().pushAlreadyApplied(
            std::make_unique<RawBytesEditCommand>("Reset Rotation", &t->rotation,
                                                  &t->rotation, &zero,
                                                  sizeof(glm::vec3)));
        m_ctx.dirty() = true;
    }

    // 缩放
    const glm::vec3 sb = t->scale;
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::DragFloat3("Scale", glm::value_ptr(t->scale), 0.01f, 0.001f, 100.0f);
    fieldEdit(m_ctx, ("Scale " + who).c_str(), &t->scale, sb);
    ImGui::SameLine();
    if (ImGui::SmallButton("R##scl") && m_ctx.isEditing()) {
        const glm::vec3 one(1.0f);
        m_ctx.commands().pushAlreadyApplied(
            std::make_unique<RawBytesEditCommand>("Reset Scale", &t->scale,
                                                  &t->scale, &one,
                                                  sizeof(glm::vec3)));
        m_ctx.dirty() = true;
    }

    // 世界坐标（只读）：有父级时最容易看清"局部 vs 世界"
    const glm::mat4 wm = sc.worldMatrix(e);
    const glm::vec3 wp = glm::vec3(wm[3]);
    ImGui::TextDisabled("world position: (%.3f, %.3f, %.3f)", wp.x, wp.y, wp.z);

    ImGui::EndDisabled();
    if (locked) {
        ImGui::SameLine();
        ImGui::TextDisabled("(locked)");
    }
}

void InspectorPanel::drawVisibility(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    auto* v = sc.world().get<ecs::VisibilityComponent>(e);
    if (!v) return;

    if (!ImGui::CollapsingHeader("Visibility", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    bool visible = v->visible;
    if (ImGui::Checkbox("Visible", &visible)) {
        v->visible = visible;
        discreteEdit(m_ctx, "Toggle Visible", &v->visible, !visible);
    }
    ImGui::SameLine();
    bool shadow = v->castShadow;
    if (ImGui::Checkbox("Cast Shadow", &shadow)) {
        v->castShadow = shadow;
        discreteEdit(m_ctx, "Toggle Cast Shadow", &v->castShadow, !shadow);
    }
}

// ---------------------------------------------------------------- 网格
//
// 这一段抄的是 Blender 的 Object Data Properties 的组织方式：
//
//   · 上半 = **只读的资产信息**（它是什么、从哪来、多少几何）。
//     这些值由文件或工厂决定，改不了，所以画成普通文本而不是控件 ——
//     做成灰掉的输入框只会让人以为"是不是我没权限"。
//   · 下半 = **能改的参数**，而且按资产类型分化：
//       内置图元 → 生成参数（边长 / 半径 / 分段数），松开鼠标就重建几何
//       OBJ/glTF → 一个可改项都没有，明确说一句"几何由文件决定"
//     这就是需求里"不同的 mesh 其属性参数不同"的落点。

void InspectorPanel::drawMeshAndMaterial(ecs::Entity e) {
    drawMeshSection(e);
    drawMaterialSection(e);
    drawCollisionSection(e);
}

// ---------------------------------------------------------------- 碰撞体
//
// 从 2026-10-10 起碰撞体是可以加到任意物体上的（视口右键菜单：
// Add Collision Body → Convex Hull / Capsule）。这一段负责"加完之后还能改"：
// 换形状、单独摆位、调胶囊尺寸、关掉 solid、移除。
//
// 为什么把"手柄编辑碰撞体"的开关也放这儿：视口里点碰撞框也能切过去，但
// 那要求碰撞框在屏幕上看得见、点得准。面板上的勾选框是那条路走不通时的
// 兜底（也是"怎么看都回不到编辑物体"的唯一出口）。
void InspectorPanel::drawCollisionSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    auto* cc = w.get<ecs::CollisionComponent>(e);
    if (!cc) return;

    const bool open = ImGui::CollapsingHeader("Collision");
    logRect("INS-COLLISION-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    if (!m_ctx.isEditing()) {
        ImGui::TextDisabled("run mode - colliders are read-only here");
        return;
    }

    // ---- 形状切换：换完按网格重建几何（位置/旋转/缩放保留）----
    int shape = static_cast<int>(cc->shape);
    const char* kShapes[] = {"Convex Hull", "Capsule"};
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Shape", &shape, kShapes, 2)) {
        const auto next = static_cast<ecs::ColliderShape>(shape);
        const auto prev = cc->shape;
        if (next != prev) {
            m_ctx.structuralEdit(
                next == ecs::ColliderShape::ConvexHull
                    ? "Collider -> Convex Hull"
                    : "Collider -> Capsule",
                [&]() {
                    auto* c2 = w.get<ecs::CollisionComponent>(e);
                    const auto* mc = w.get<ecs::MeshComponent>(e);
                    if (!c2 || !mc || !mc->mesh) return;
                    if (next == ecs::ColliderShape::ConvexHull) {
                        physics::setupHullCollider(*mc->mesh, *c2);
                    } else {
                        physics::setupCapsuleCollider(*mc->mesh, *c2);
                    }
                });
        }
    }

    // ---- 手柄目标开关 ----
    bool editColl = m_ctx.colliderEdit();
    if (ImGui::Checkbox("Edit with W/E/R", &editColl))
        m_ctx.setColliderEdit(editColl);
    ImGui::SameLine();
    bool solid = cc->solid;
    if (ImGui::Checkbox("Solid", &solid))
        discreteEdit(m_ctx, "Collider Solid", &cc->solid, !solid);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Solid = blocks other colliders and the camera.\n"
                          "Off = a pure trigger (still visible, still blocks "
                          "the camera while flying through it).");

    // ---- 摆放（相对实体）----
    const glm::vec3 p0 = cc->position;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##collider-pos", glm::value_ptr(cc->position), 0.01f);
    fieldEdit(m_ctx, "Collider Position", &cc->position, p0);
    ImGui::SetItemTooltip("Position (relative to the object)");

    const glm::vec3 r0 = cc->rotation;
    ImGui::SetNextItemWidth(-1.0f);
    glm::vec3 rotDeg = glm::degrees(cc->rotation);
    if (ImGui::DragFloat3("##collider-rot", glm::value_ptr(rotDeg), 0.5f)) {
        cc->rotation = glm::radians(rotDeg);
    }
    fieldEdit(m_ctx, "Collider Rotation", &cc->rotation, r0);
    ImGui::SetItemTooltip("Rotation (degrees)");

    const glm::vec3 s0 = cc->scale;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::DragFloat3("##collider-scale", glm::value_ptr(cc->scale), 0.01f,
                      0.01f, 100.0f);
    fieldEdit(m_ctx, "Collider Scale", &cc->scale, s0);
    ImGui::SetItemTooltip("Scale (uniform is not required)");

    // ---- 胶囊尺寸 ----
    if (cc->shape == ecs::ColliderShape::Capsule) {
        const float rad0 = cc->capsuleRadius;
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Radius", &cc->capsuleRadius, 0.01f, 10.0f, "%.3f");
        fieldEdit(m_ctx, "Collider Radius", &cc->capsuleRadius, rad0);

        const float hh0 = cc->capsuleHalfHeight;
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat("Half Height", &cc->capsuleHalfHeight, 0.0f, 10.0f,
                           "%.3f");
        fieldEdit(m_ctx, "Collider Half Height", &cc->capsuleHalfHeight, hh0);
    } else {
        ImGui::TextDisabled("convex hull: %zu points / %zu edges",
                            cc->hullPoints.size(), cc->hullEdges.size());
    }

    if (ImGui::Button("Remove Collision Body")) {
        m_ctx.structuralEdit("Remove Collision Body", [&]() {
            w.remove<ecs::CollisionComponent>(e);
        });
        m_ctx.setColliderEdit(false);
    }
}

void InspectorPanel::drawMeshSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    auto* mc = w.get<ecs::MeshComponent>(e);
    if (!mc) return;

    // 标题栏的矩形也要报：Mesh 段默认是折叠的（ini 被 MYVK_NO_WINDOW_SAVE
    // 关掉了，状态不持久，所以恒定折叠），验收脚本得先点开它才能读到参数。
    const bool meshOpen = ImGui::CollapsingHeader("Mesh");
    logRect("INS-MESH-HEADER", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!meshOpen) return;

    if (!mc->mesh) {
        ImGui::TextDisabled("(null mesh)");
        return;
    }

    const assets::Mesh& m = *mc->mesh;
    const assets::MeshSource& s = m.source();

    // ---- 身份（只读）----
    static const char* kKinds[] = {"Unknown", "Builtin", "OBJ", "glTF"};
    const int ki = static_cast<int>(s.kind);
    ImGui::Text("asset: %s", kKinds[(ki >= 0 && ki < 4) ? ki : 0]);
    if (!s.shape.empty()) ImGui::Text("shape: %s", s.shape.c_str());
    if (!s.path.empty()) ImGui::Text("file : %s", s.path.c_str());
    if (s.subMeshIndex >= 0)
        ImGui::Text("sub  : primitive #%d", s.subMeshIndex);
    if (!s.name.empty()) {
        ImGui::TextDisabled("key  : %s", s.name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "AssetManager cache key.\n"
                "Two meshes share GPU buffers exactly when their keys match.");
        }
    }

    // ---- 几何统计（只读）----
    ImGui::SeparatorText("Geometry");
    ImGui::Text("vertices : %zu", m.vertices().size());
    ImGui::Text("triangles: %zu", m.indices().size() / 3);
    if (m.hasBounds()) {
        const glm::vec3 mn = m.boundsMin();
        const glm::vec3 mx = m.boundsMax();
        const glm::vec3 ext = mx - mn;
        ImGui::Text("size     : %.3f x %.3f x %.3f",
                    static_cast<double>(ext.x), static_cast<double>(ext.y),
                    static_cast<double>(ext.z));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("local AABB\nmin (%.3f, %.3f, %.3f)\nmax (%.3f, %.3f, %.3f)",
                              static_cast<double>(mn.x), static_cast<double>(mn.y),
                              static_cast<double>(mn.z), static_cast<double>(mx.x),
                              static_cast<double>(mx.y), static_cast<double>(mx.z));
        }
    }
    hint("read-only - these come from the file or from the built-in generator");

    // ---- 生成参数（可拖拽；只有内置图元才有）----
    ImGui::SeparatorText("Parameters");

    if (!mc->params.isBuiltin()) {
        hint("OBJ / glTF geometry is defined by the file - nothing to tune here");
        return;
    }
    if (w.has<ecs::LockedComponent>(e)) {
        hint("locked - primitive parameters are disabled for this entity");
        return;
    }

    // 撤销要用"改之前"的整份组件（参数 + 当时那颗 mesh）
    const ecs::MeshComponent before = *mc;
    ecs::MeshPrimitiveParams& p = mc->params;
    bool edited = false;

    // 控件矩形探针：tools/verify_mesh_params.py 靠这些日志定位滑块，然后用
    // 真实鼠标去拖 —— 和验证材质拖拽是同一套路子。
    const auto probe = [](const char* label) {
        const std::string tag = std::string("MESH-PARAM ") + label;
        logRect(tag.c_str(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    };

    std::string labels;   // 本帧画出的参数名，喂给下面的签名日志

    switch (p.primitive) {
        case ecs::MeshPrimitive::Cube:
        case ecs::MeshPrimitive::Plane: {
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragFloat("Size", &p.size, 0.01f, 0.01f, 500.0f, "%.3f");
            probe("Size");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            labels = "Size";
            break;
        }
        case ecs::MeshPrimitive::Sphere: {
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragFloat("Radius", &p.radius, 0.005f, 0.001f, 200.0f, "%.3f");
            probe("Radius");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragInt("Segments", &p.segments, 0.5f, 3, 256);
            probe("Segments");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("longitude divisions - drives the vertex count");
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragInt("Rings", &p.rings, 0.5f, 2, 256);
            probe("Rings");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("latitude divisions - drives the vertex count");
            labels = "Radius,Segments,Rings";
            break;
        }
        case ecs::MeshPrimitive::Cylinder: {
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragFloat("Radius", &p.radius, 0.005f, 0.001f, 200.0f, "%.3f");
            probe("Radius");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::DragInt("Segments", &p.segments, 0.5f, 3, 256);
            probe("Segments");
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            labels = "Radius,Segments";
            hint("height is fixed at 2 x radius (Blender's default cylinder)");
            break;
        }
        case ecs::MeshPrimitive::None:
            break;
    }

    // "这一帧到底画了哪些参数" —— 只在签名变化时打一行，验收脚本据此断言
    // 不同图元的**结构性差异**（Cube 只有 Size；Sphere 多出 Segments/Rings）。
    {
        const char* kindName = "?";
        switch (p.primitive) {
            case ecs::MeshPrimitive::Cube: kindName = "Cube"; break;
            case ecs::MeshPrimitive::Plane: kindName = "Plane"; break;
            case ecs::MeshPrimitive::Sphere: kindName = "Sphere"; break;
            case ecs::MeshPrimitive::Cylinder: kindName = "Cylinder"; break;
            case ecs::MeshPrimitive::None: kindName = "None"; break;
        }
        // 顺带把资产缓存键也带上：键里编着生成参数（builtin/cube/s1.9000），
        // 所以这一行同时能证明"参数有没有活着走过存盘/读盘"。
        const std::string key = mc->mesh ? mc->mesh->source().name : "";
        const std::string sig =
            std::string(kindName) + "|" + labels + "|" + key;
        if (sig != m_lastMeshParamSig) {
            m_lastMeshParamSig = sig;
            VK_LOG_INFO("MESH-PARAMS kind=%s labels=%s key=%s", kindName,
                        labels.empty() ? "(none)" : labels.c_str(),
                        key.empty() ? "(none)" : key.c_str());
        }
    }

    hint("the geometry is regenerated when you release the mouse - undoable");

    if (edited) commitPrimitiveEdit(e, before);
}

void InspectorPanel::commitPrimitiveEdit(ecs::Entity e,
                                         const ecs::MeshComponent& before) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();
    auto* mc = w.get<ecs::MeshComponent>(e);
    if (!mc) return;

    // 参数值已经由 DragFloat 写进组件了，这里负责把它变成几何。
    // 内部只走 AssetManager 的缓存键查表 —— 参数没变就是纯查表，零开销。
    rebuildPrimitive(m_ctx.assets(), *mc);

    const std::string who = [&] {
        const auto* nm = w.get<ecs::NameComponent>(e);
        return nm && !nm->name.empty() ? nm->name : std::string("mesh");
    }();
    const assets::MeshSource& s =
        mc->mesh ? mc->mesh->source() : assets::MeshSource{};
    VK_LOG_INFO("primitive rebuild: '%s' %s size=%.4f radius=%.4f seg=%d rings=%d "
                "-> %zu verts (key %s)",
                who.c_str(), s.shape.c_str(), static_cast<double>(mc->params.size),
                static_cast<double>(mc->params.radius), mc->params.segments,
                mc->params.rings, mc->mesh ? mc->mesh->vertices().size() : 0,
                s.name.c_str());

    if (!m_ctx.isEditing()) return;

    m_ctx.commands().pushAlreadyApplied(std::make_unique<PrimitiveEditCommand>(
        "Rebuild " + who, mc, &m_ctx.assets(), before, *mc));
    m_ctx.setStatus("Rebuild " + who);
    m_ctx.dirty() = true;
}

// ---------------------------------------------------------------- 材质

namespace {

// 材质纹理的缓存键。
//
// ⚠ 必须把 sRGB 编进键里。AssetManager 只按名字缓存，而同一张图片当
// albedo（要 sRGB 解码）和当 normal（必须保持线性）需要的是**两种不同的
// GPU 纹理**（VK_FORMAT_R8G8B8A8_SRGB vs _UNORM，见 Texture.h）。
// 共用一个键的话第二次会拿回第一次那张 —— 法线贴图被 gamma 过一次，
// 光照方向就全歪了，而且这种错很难查。
std::string slotCacheKey(const std::string& rel, bool srgb) {
    return srgb ? rel : rel + "#linear";
}

std::string lowerExtOf(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return e;
}

bool isImagePath(const std::string& path) {
    const std::string e = lowerExtOf(path);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" ||
           e == ".tga";
}

// 按名字取缩略图缓存键。纹理是共享资产、指针稳定，所以"有名字用名字、
// 没名字（程序化 solid/checker）退回指针"就够了。
std::string thumbKeyFor(const assets::Texture* t) {
    if (!t) return {};
    const assets::TextureSource& s = t->source();
    if (!s.name.empty()) return "slot:" + s.name;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "slot:@%p", static_cast<const void*>(t));
    return buf;
}

// "#linear" 只是 slotCacheKey 用来区分色彩空间分支的内部标记，纯粹给缓存
// 看的。它必须留在键里，但绝不能显示到界面上（"xxx.png#linear" 看着像文
// 件名的一部分，很容易让人以为磁盘上真有这么个文件）。
std::string stripVariantSuffix(std::string s) {
    static const std::string kMarker = "#linear";
    if (s.size() > kMarker.size() &&
        s.compare(s.size() - kMarker.size(), kMarker.size(), kMarker) == 0) {
        s.erase(s.size() - kMarker.size());
    }
    return s;
}

std::string displayNameOf(const assets::Texture* t) {
    if (!t) return "(default)";
    const assets::TextureSource& s = t->source();
    if (!s.name.empty()) return stripVariantSuffix(s.name);
    if (!s.path.empty()) return stripVariantSuffix(s.path);
    switch (s.kind) {
        case assets::TextureSource::Kind::Solid: return "(solid colour)";
        case assets::TextureSource::Kind::Checker: return "(checkerboard)";
        default: break;
    }
    return "(procedural)";
}

void truncateLeft(std::string& s, size_t maxChars) {
    if (s.size() <= maxChars) return;
    s = "..." + s.substr(s.size() - (maxChars - 3));
}

// 槽位方块的尺寸。空着的时候就是一个"带加号的小方块"，绑定之后向右
// 长成一条胶囊（缩略图 + 文件名）。
//
// 28 而不是更小：它是**唯一**的拖放落点，大小直接决定好不好点中；行高
// 也随之变成 28，六行排下来属性之间自然拉开，不再挤成一片。
constexpr float kChipH = 28.0f;
constexpr float kChipBox = 28.0f;

// ---- 三列布局：属性名 | 值 | 槽位方块 ----
//
// 两列都定宽，于是"标签起点"和"方块左边缘"在六行之间天然对齐 —— 这是
// "不拥挤"的关键。原来靠 SetNextItemWidth 把标签甩在控件后面，每行的
// 起点取决于标签宽度，六行看下来参差不齐。
//
// 88 是量出来的：最长的属性名 "Normal Scale" 在默认字体下约 84px，留
// 4px 余量。面板默认只有 330px 宽（SplitLayout::kRightMin），两列
// 88+88 加上两次间距之后就只剩 ~116px 给槽位 —— kChipMaxW 也不需要
// 原来那个 190 了，反正吃不满。
constexpr float kLabelWidth = 88.0f;
constexpr float kValueWidth = 88.0f;
// 数值控件与标签之间、方块与值之间的呼吸空间（比默认 ItemSpacing 略大）
constexpr float kColGap = 10.0f;

constexpr float kChipMinW = kChipBox;  // 空槽就是那个小方块
constexpr float kChipMaxW = 160.0f;    // 上限（面板更宽时不至于拉成一长条）

// 绑定后显示的是**文件名**，不是整条资产路径：这么小的方块放不下
// "models/Cube/Cube_BaseColor.png"，而路径的前缀在同一个材质里几乎总是
// 重复的，真正有信息量的是最后那一段。
std::string fileNameOf(const std::string& pathOrName) {
    const size_t slash = pathOrName.find_last_of("/\\");
    return slash == std::string::npos ? pathOrName
                                      : pathOrName.substr(slash + 1);
}

// 把名字裁到放得下为止：**从左边截**，保住扩展名 —— 一眼就能看出它是
// 颜色图还是数据图，这比保住文件名的前缀有用得多（同目录下前缀往往一样）。
std::string fitText(const std::string& full, float availPx) {
    if (full.empty()) return full;
    if (ImGui::CalcTextSize(full.c_str()).x <= availPx) return full;
    for (size_t keep = full.size(); keep > 3; --keep) {
        const std::string cand = "..." + full.substr(full.size() - keep);
        if (ImGui::CalcTextSize(cand.c_str()).x <= availPx) return cand;
    }
    return full.substr(full.size() - std::min<size_t>(full.size(), 4));
}

// 槽位当前"显示的是什么"的签名日志（只在变化时打一行）。
// 用途和 Mesh 的 MESH-PARAMS 一样：**让"方块里显示的是文件名"这件事
// 可被自动化断言**。靠截图认字既脆又慢，而这是界面文案的唯一来源 ——
// tools/verify_material_chips.py 断言的就是它。
void logChipContent(const char* slotId, const std::string& shown) {
    if (!std::getenv("MYVK_LOG_RECTS")) return;
    static std::unordered_map<std::string, std::string> last;
    auto it = last.find(slotId);
    if (it != last.end() && it->second == shown) return;
    last[slotId] = shown;
    VK_LOG_INFO("MAT-CHIP %s shows=\"%s\"", slotId,
                shown.empty() ? "(empty)" : shown.c_str());
}

} // namespace

bool InspectorPanel::drawTextureChip(const char* slotId, const char* hintText,
                                     assets::Texture** slot, bool srgb,
                                     float maxWidth) {
    bool changed = false;

    ImGui::PushID(slotId);

    assets::Texture* tex = *slot;
    const ThumbnailCache::Entry* th =
        tex ? m_thumbs.getBound(thumbKeyFor(tex), tex) : nullptr;
    const bool hasPixels = th && th->ds != VK_NULL_HANDLE;

    std::string name = tex ? fileNameOf(displayNameOf(tex)) : std::string();

    // 空槽恰好是那个小方块；有内容才向右长（宽度受面板剩余空间限制）
    const float boxW = std::max(std::min(maxWidth, kChipMaxW), kChipMinW);
    if (!name.empty()) name = fitText(name, boxW - kChipBox - 12.0f);
    const ImVec2 textSz =
        name.empty() ? ImVec2(0.0f, 0.0f) : ImGui::CalcTextSize(name.c_str());
    const float w = name.empty()
                        ? kChipBox
                        : std::min(kChipBox + 6.0f + textSz.x + 6.0f, boxW);

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + kChipH);
    // Dummy：占位 + 成为一个合法的拖放落点。
    // ⚠ 它给 ItemAdd 传的 ID 是 0，而 BeginDragDropTarget 在 ID 为 0 时会
    //   退回 GetIDFromRectangle()，所以"整个方块"天然就是热区，不需要
    //   额外的 InvisibleButton（tools/verify_material_drag.py 依赖这个行为）。
    ImGui::Dummy(ImVec2(w, kChipH));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    const bool dropReady =
        payload != nullptr && payload->IsDataType(drag::kAsset);

    // ---- 底与边 ----
    const ImU32 bg = hasPixels ? IM_COL32(18, 19, 23, 255)
                               : (hovered ? IM_COL32(62, 65, 73, 255)
                                          : IM_COL32(46, 48, 54, 255));
    dl->AddRectFilled(p0, p1, bg, 3.0f);
    dl->AddRect(p0, p1,
                dropReady ? IM_COL32(252, 218, 62, 230)
                          : (hovered ? IM_COL32(150, 156, 166, 230)
                                     : IM_COL32(96, 100, 110, 210)),
                3.0f, 0, dropReady ? 2.0f : 1.0f);

    const ImVec2 box0 = p0;
    const ImVec2 box1(p0.x + kChipBox, p0.y + kChipH);

    if (hasPixels) {
        // 缩略图铺满方框，再补一圈描边把它和胶囊的底色分开
        dl->AddImage(static_cast<ImTextureID>(
                         reinterpret_cast<std::uintptr_t>(th->ds)),
                     box0, box1, ImVec2(0, 0), ImVec2(1, 1));
        dl->AddRect(box0, box1, IM_COL32(96, 100, 110, 200), 3.0f, 0, 1.0f);
        if (!name.empty()) {
            dl->AddText(ImVec2(box1.x + 6.0f, p0.y + (kChipH - textSz.y) * 0.5f),
                        IM_COL32(206, 210, 218, 255), name.c_str());
        }
    } else if (tex) {
        // 有纹理对象但缩略图还没就绪（或加载失败）：画个问号
        const ImVec2 ts = ImGui::CalcTextSize("?");
        dl->AddText(ImVec2(box0.x + (kChipBox - ts.x) * 0.5f,
                           box0.y + (kChipH - ts.y) * 0.5f),
                    IM_COL32(146, 150, 158, 255), "?");
    } else {
        // 空槽：正中一个加号。两笔就够，不必引图标字体。
        const ImVec2 c((box0.x + box1.x) * 0.5f, (box0.y + box1.y) * 0.5f);
        const ImU32 pc = dropReady ? IM_COL32(252, 218, 62, 255)
                                   : IM_COL32(150, 156, 166, 255);
        const float a = 5.0f;
        dl->AddLine(ImVec2(c.x - a, c.y), ImVec2(c.x + a, c.y), pc, 1.6f);
        dl->AddLine(ImVec2(c.x, c.y - a), ImVec2(c.x, c.y + a), pc, 1.6f);
    }

    if (hovered) {
        ImGui::SetTooltip("%s\n\n%s", hintText ? hintText : slotId,
                          tex ? "drag another image here to replace\n"
                                "right-click to clear"
                              : "drag an image from the Content Browser here");
    }

    // ---- 右键清空（方块太小，放不下 Clear 按钮）----
    if (tex && ImGui::BeginPopupContextItem("##clear")) {
        if (ImGui::MenuItem("Clear texture")) {
            *slot = nullptr;
            changed = true;
            VK_LOG_INFO("material slot '%s' <- (cleared)", slotId);
        }
        ImGui::EndPopup();
    }

    // 自动化用（MYVK_LOG_RECTS=1）：槽位的屏幕矩形。脚本据此把 Content
    // 面板里的图片拖过来 —— 见 tools/verify_material_drag.py。
    {
        const std::string tag = std::string("MAT-SLOT ") + slotId;
        logRect(tag.c_str(), p0, p1);
    }
    logChipContent(slotId, name);

    // ---- 整个方块都是落点 ----
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(drag::kAsset)) {
            const char* rel = static_cast<const char*>(pl->Data);
            if (rel && *rel) {
                if (!isImagePath(rel)) {
                    m_ctx.setStatus(std::string("not an image: ") + rel);
                    VK_LOG_WARN("material slot '%s': '%s' is not an image",
                                slotId, rel);
                } else {
                    try {
                        // slotCacheKey：同样的图当颜色贴图和数据贴图用时是
                        // 两份不同的 GPU 纹理（sRGB 解码与否），必须分开缓存
                        *slot = m_ctx.assets().loadTexture(
                            assets::resolveAssetPath(rel),
                            slotCacheKey(rel, srgb), srgb);
                        changed = true;
                        VK_LOG_INFO("material slot '%s' <- %s (srgb=%d)",
                                    slotId, rel, srgb ? 1 : 0);
                    } catch (const std::exception& ex) {
                        VK_LOG_ERROR("material slot '%s': load '%s' failed: %s",
                                     slotId, rel, ex.what());
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::PopID();
    return changed;
}

void InspectorPanel::drawMaterialSection(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    auto* matc = w.get<ecs::MaterialComponent>(e);
    if (!matc || !matc->material) return;
    assets::Material* mat = matc->material;

    if (!ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Text("name: %s", mat->name.c_str());

    // ---- 三列布局：属性名 | 值 | 纹理槽方块 ----
    //
    // 每帧在**第一行的行首**取一次 rowX0，它就是"第 1 列的左缘"。
    // 之后所有列都用绝对偏移：SameLine(offset) 的 offset 是**窗口坐标系**
    // 的横坐标（imgui.cpp:11708），所以必须基于 rowX0 加，直接写 88 会被
    // 窗口 padding 顶掉一截，而且行数一多还会各偏一点。
    //
    // 两列都定宽 = 六行的"值"和"方块"各自落在同一条竖直线上。之前每行
    // 的方块都跟在标签屁股后面（标签长短不一），六行看下来参差不齐 ——
    // 那正是"拥挤"的来源。
    const float rowX0 = ImGui::GetCursorPosX();
    const float rowRight = ImGui::GetWindowContentRegionMax().x;
    const float colValue = rowX0 + kLabelWidth + kColGap;
    const float colChip = colValue + kValueWidth + kColGap;

    auto rowBegin = [&](const char* label) {
        // AlignTextToFramePadding：文字要跟右边的输入框在**同一条基线上**，
        // 否则标签会贴着行顶，看起来像没对齐。
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(colValue);
    };

    // 六个槽各自的旧值 —— 一次拖放 = 一条撤销记录。
    // 材质是**共享资产**（AssetManager 按名字缓存，可能被多个实体引用），
    // 所以只能用按字节回滚的 RawBytesEditCommand，不能用整场景快照。
    assets::Texture* const a1 = mat->albedoMap;
    assets::Texture* const mt1 = mat->metallicMap;
    assets::Texture* const rg1 = mat->roughnessMap;
    assets::Texture* const em1 = mat->emissiveMap;
    assets::Texture* const nm1 = mat->normalMap;
    assets::Texture* const or1 = mat->ormMap;

    // 跳到第 3 列画槽位方块。avail 用"面板右缘 - 方块左缘"算 —— 这样
    // 面板被拖窄时方块不会越界（tools/verify_material_chips.py 断言这条）。
    auto chipAt = [&](const char* slotId, const char* undoName, const char* tip,
                      assets::Texture** slot, assets::Texture* before,
                      bool srgb) {
        ImGui::SameLine(colChip);
        const float avail = rowRight - ImGui::GetCursorPosX() - 4.0f;
        if (drawTextureChip(slotId, tip, slot, srgb, avail))
            discreteEdit(m_ctx, undoName, slot, before);
    };

    // ---- Base Color ----
    // 颜色用 NoInputs：第 2 列只有 88px，塞 4 个数值框进去必然挤成一团。
    // 换成一个色块按钮，点开才是完整取色器 —— 既省地方又看得清当前颜色。
    const glm::vec4 c0 = mat->baseColorFactor;
    rowBegin("Base Color");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::ColorEdit4("##base-color", glm::value_ptr(mat->baseColorFactor),
                      ImGuiColorEditFlags_NoInputs);
    fieldEdit(m_ctx, "Material Base Color", &mat->baseColorFactor, c0);
    chipAt("albedo", "Material Albedo", "Base colour map (sRGB)",
           &mat->albedoMap, a1, /*srgb=*/true);

    // ---- Metallic ----
    const float m0 = mat->metallic;
    rowBegin("Metallic");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::SliderFloat("##metallic", &mat->metallic, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material Metallic", &mat->metallic, m0);
    chipAt("metallic", "Material Metallic Map",
           "Metallic map (grayscale, linear). Multiplied by the slider on the left.",
           &mat->metallicMap, mt1, /*srgb=*/false);

    // ---- Roughness ----
    const float r0 = mat->roughness;
    rowBegin("Roughness");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::SliderFloat("##roughness", &mat->roughness, 0.02f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material Roughness", &mat->roughness, r0);
    chipAt("roughness", "Material Roughness Map",
           "Roughness map (grayscale, linear). Multiplied by the slider on the left.",
           &mat->roughnessMap, rg1, /*srgb=*/false);

    // ---- Emissive ----
    const glm::vec3 e0 = mat->emissive;
    rowBegin("Emissive");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::ColorEdit3("##emissive", glm::value_ptr(mat->emissive),
                      ImGuiColorEditFlags_NoInputs);
    fieldEdit(m_ctx, "Material Emissive", &mat->emissive, e0);
    chipAt("emissive", "Material Emissive Map",
           "Emissive map (sRGB). ADDED to the colour on the left - dropping "
           "one lights the surface up even with the colour left at black.",
           &mat->emissiveMap, em1, /*srgb=*/true);

    // ---- AO / Normal Scale ----
    // 这两项没有"自己的"贴图（AO 来自 ORM 的 R，法线是整张图），槽位挂的
    // 是它们真正消费的那张纹理。
    const float a0 = mat->ao;
    rowBegin("AO");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::SliderFloat("##ao", &mat->ao, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material AO", &mat->ao, a0);
    chipAt("orm", "Material ORM",
           "ORM packed map: Occlusion(R) / Roughness(G) / Metallic(B), linear",
           &mat->ormMap, or1, /*srgb=*/false);

    const float n0 = mat->normalScale;
    rowBegin("Normal Scale");
    ImGui::SetNextItemWidth(kValueWidth);
    ImGui::SliderFloat("##normal-scale", &mat->normalScale, 0.0f, 2.0f, "%.2f");
    fieldEdit(m_ctx, "Material Normal Scale", &mat->normalScale, n0);
    chipAt("normal", "Material Normal",
           "Tangent-space normal map, kept linear",
           &mat->normalMap, nm1, /*srgb=*/false);

    // ---- 杂项（同样两列：名字 + 值，没有槽位）----
    const int modeBefore = static_cast<int>(mat->alphaMode);
    int mode = modeBefore;
    const char* kAlpha[] = {"Opaque", "Blend"};
    rowBegin("Alpha Mode");
    ImGui::SetNextItemWidth(kValueWidth);
    if (ImGui::Combo("##alpha-mode", &mode, kAlpha, 2)) {
        const auto newMode = static_cast<assets::Material::AlphaMode>(mode);
        const auto oldMode = mat->alphaMode;
        if (newMode != oldMode) {
            mat->alphaMode = newMode;
            m_ctx.commands().pushAlreadyApplied(
                std::make_unique<LambdaCommand>(
                    "Material Alpha Mode",
                    [mat, oldMode]() { mat->alphaMode = oldMode; },
                    [mat, newMode]() { mat->alphaMode = newMode; }));
            m_ctx.dirty() = true;
        }
    }

    bool ds = mat->doubleSided;
    rowBegin("Double Sided");
    if (ImGui::Checkbox("##double-sided", &ds)) {
        const bool ob = mat->doubleSided;
        mat->doubleSided = ds;
        discreteEdit(m_ctx, "Material Double Sided", &mat->doubleSided, ob);
    }
}

// ---------------------------------------------------------------- 光照
//
// 三种灯在引擎里是**三个独立的组件类型**（ecs/Components.h 刻意没有搞一个
// LightType 枚举 —— "挂了哪个组件"就是类型本身），所以"不同灯显示不同参数"
// 是天然成立的：下面每一支只画自己组件里真实存在的字段。
//
// 顶部那个 Type 下拉是额外的便利：切换 = 换组件。能带过去的参数（颜色 /
// 强度 / 方向）会带过去，其余用该类型的默认值。
//
// 唯一的例外是**场景主光**（scene::Scene::light() 返回的那盏方向光）：
// 它的类型不允许改，否则 Scene::light() 会当场给你再补一盏方向光出来，
// 实体上就同时挂着两盏灯了。

namespace {

void convertLightType(ecs::World& w, ecs::Entity e, int to) {
    // 先取值再删组件 —— 删完指针就悬垂了
    glm::vec3 color(1.0f, 0.98f, 0.95f);
    glm::vec3 dir(0.0f, 0.0f, -1.0f);
    float intensity = 1.0f;

    if (const auto* d = w.get<ecs::DirectionalLightComponent>(e)) {
        color = d->color;
        dir = d->direction;
        intensity = d->intensity;
    } else if (const auto* p = w.get<ecs::PointLightComponent>(e)) {
        color = p->color;
        intensity = p->intensity;
    } else if (const auto* s = w.get<ecs::SpotLightComponent>(e)) {
        color = s->color;
        dir = s->direction;
        intensity = s->intensity;
    }

    if (w.has<ecs::DirectionalLightComponent>(e))
        w.remove<ecs::DirectionalLightComponent>(e);
    if (w.has<ecs::PointLightComponent>(e)) w.remove<ecs::PointLightComponent>(e);
    if (w.has<ecs::SpotLightComponent>(e)) w.remove<ecs::SpotLightComponent>(e);

    switch (to) {
        case 0: {
            ecs::DirectionalLightComponent c;
            c.color = color;
            c.direction = dir;
            c.intensity = intensity;
            w.add<ecs::DirectionalLightComponent>(e, c);
            break;
        }
        case 1: {
            ecs::PointLightComponent c;
            c.color = color;
            c.intensity = intensity;
            w.add<ecs::PointLightComponent>(e, c);
            break;
        }
        case 2: {
            ecs::SpotLightComponent c;
            c.color = color;
            c.direction = dir;
            c.intensity = intensity;
            w.add<ecs::SpotLightComponent>(e, c);
            break;
        }
        default:
            break;
    }
}

} // namespace

void InspectorPanel::drawLights(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    auto* pl = w.get<ecs::PointLightComponent>(e);
    auto* sl = w.get<ecs::SpotLightComponent>(e);
    auto* dl = w.get<ecs::DirectionalLightComponent>(e);
    if (!pl && !sl && !dl) return;

    if (!ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // 控件矩形探针 + "这一帧画了哪些字段"的签名（同 Mesh 段，供
    // tools/verify_light_params.py 断言三种灯的参数集合各不相同）
    const auto probe = [](const char* label) {
        const std::string tag = std::string("LIGHT-PARAM ") + label;
        logRect(tag.c_str(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    };
    std::string labels;

    // ---- 类型 ----
    const int typeNow = dl ? 0 : (pl ? 1 : 2);
    const char* kTypes[] = {"Directional (sun)", "Point", "Spot"};

    // 场景主光的类型锁死：改掉它会让 Scene::light() 自动补一盏新的方向光
    const bool isKeyLight = (sc.lightEntity() == e);

    int type = typeNow;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::BeginDisabled(isKeyLight);
    const bool typeChanged = ImGui::Combo("Type", &type, kTypes, 3);
    probe("Type");
    ImGui::EndDisabled();
    if (isKeyLight) {
        hint("scene key light - its type is fixed (see scene::Scene::light)");
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("switching the type keeps colour / intensity / direction");
    }

    if (typeChanged && type != typeNow && m_ctx.isEditing()) {
        m_ctx.structuralEdit("Change Light Type", [&] { convertLightType(w, e, type); });
    }

    // ---- Directional ----
    if (dl) {
        ImGui::SeparatorText("Directional");

        const glm::vec3 d0 = dl->direction;
        ImGui::DragFloat3("Direction", glm::value_ptr(dl->direction), 0.01f, -1.0f, 1.0f);
        probe("Direction");
        fieldEdit(m_ctx, "Sun Direction", &dl->direction, d0);

        const glm::vec3 c0 = dl->color;
        ImGui::ColorEdit3("Color", glm::value_ptr(dl->color));
        probe("Color");
        fieldEdit(m_ctx, "Sun Color", &dl->color, c0);

        const float i0 = dl->intensity;
        ImGui::SliderFloat("Intensity", &dl->intensity, 0.0f, 4.0f);
        probe("Intensity");
        fieldEdit(m_ctx, "Sun Intensity", &dl->intensity, i0);

        const float a0 = dl->ambientScale;
        ImGui::SliderFloat("Ambient", &dl->ambientScale, 0.0f, 1.0f, "%.2f");
        probe("Ambient");
        fieldEdit(m_ctx, "Sun Ambient Scale", &dl->ambientScale, a0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("procedural IBL strength\nindoor scenes: 0.1 - 0.3");

        bool cs = dl->castsShadow;
        if (ImGui::Checkbox("Casts Shadow", &cs)) {
            dl->castsShadow = cs;
            discreteEdit(m_ctx, "Sun Casts Shadow", &dl->castsShadow, !cs);
        }
        labels = "Direction,Color,Intensity,Ambient,CastsShadow";
        hint("no range / cone: a sun is parallel light from infinitely far away");
    }

    // ---- Point ----
    if (pl) {
        ImGui::SeparatorText("Point");

        const glm::vec3 c0 = pl->color;
        ImGui::ColorEdit3("Color", glm::value_ptr(pl->color));
        probe("Color");
        fieldEdit(m_ctx, "Point Light Color", &pl->color, c0);

        const float i0 = pl->intensity;
        ImGui::DragFloat("Intensity", &pl->intensity, 0.2f, 0.0f, 500.0f, "%.1f");
        probe("Intensity");
        fieldEdit(m_ctx, "Point Light Intensity", &pl->intensity, i0);

        const float r0 = pl->range;
        ImGui::DragFloat("Range", &pl->range, 0.05f, 0.1f, 60.0f, "%.2f");
        probe("Range");
        fieldEdit(m_ctx, "Point Light Range", &pl->range, r0);
        labels = "Color,Intensity,Range,Enabled";
        hint("range drives cluster culling - keep it tight");

        bool en = pl->enabled;
        if (ImGui::Checkbox("Enabled", &en)) {
            pl->enabled = en;
            discreteEdit(m_ctx, "Toggle Point Light", &pl->enabled, !en);
        }
        if (removeButton() && m_ctx.isEditing()) {
            m_ctx.structuralEdit("Remove Point Light",
                                 [&]() { w.remove<ecs::PointLightComponent>(e); });
        }
    }

    // ---- Spot ----
    if (sl) {
        ImGui::SeparatorText("Spot");

        const glm::vec3 c0 = sl->color;
        ImGui::ColorEdit3("Color", glm::value_ptr(sl->color));
        probe("Color");
        fieldEdit(m_ctx, "Spot Light Color", &sl->color, c0);

        const float i0 = sl->intensity;
        ImGui::DragFloat("Intensity", &sl->intensity, 0.4f, 0.0f, 800.0f, "%.1f");
        probe("Intensity");
        fieldEdit(m_ctx, "Spot Light Intensity", &sl->intensity, i0);

        const float r0 = sl->range;
        ImGui::DragFloat("Range", &sl->range, 0.05f, 0.1f, 80.0f, "%.2f");
        probe("Range");
        fieldEdit(m_ctx, "Spot Light Range", &sl->range, r0);

        const glm::vec3 d0 = sl->direction;
        ImGui::DragFloat3("Direction", glm::value_ptr(sl->direction), 0.01f, -1.0f, 1.0f);
        probe("Direction");
        fieldEdit(m_ctx, "Spot Light Direction", &sl->direction, d0);

        float innerDeg = glm::degrees(sl->innerAngle);
        float outerDeg = glm::degrees(sl->outerAngle);
        const float in0 = sl->innerAngle;
        const float out0 = sl->outerAngle;
        if (ImGui::SliderFloat("Inner Cone", &innerDeg, 1.0f, 80.0f, "%.1f deg")) {
            sl->innerAngle = glm::radians(innerDeg);
        }
        fieldEdit(m_ctx, "Spot Inner Cone", &sl->innerAngle, in0);
        if (ImGui::SliderFloat("Outer Cone", &outerDeg, 1.0f, 89.0f, "%.1f deg")) {
            sl->outerAngle = glm::radians(outerDeg);
        }
        fieldEdit(m_ctx, "Spot Outer Cone", &sl->outerAngle, out0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("penumbra = outer - inner\nnarrow the gap for a harder edge");

        labels = "Color,Intensity,Range,Direction,InnerCone,OuterCone,Enabled";

        bool en = sl->enabled;
        if (ImGui::Checkbox("Enabled", &en)) {
            sl->enabled = en;
            discreteEdit(m_ctx, "Toggle Spot Light", &sl->enabled, !en);
        }
        if (removeButton() && m_ctx.isEditing()) {
            m_ctx.structuralEdit("Remove Spot Light",
                                 [&]() { w.remove<ecs::SpotLightComponent>(e); });
        }
    }

    // "这一帧画了哪些字段" —— 只在签名变化时打一行。三种灯的集合本来就该
    // 互不相同，验收脚本直接断言这个差异，不用去截图里认控件。
    {
        const char* typeName = dl ? "Directional" : (pl ? "Point" : "Spot");
        const std::string sig = std::string(typeName) + "|" + labels;
        if (sig != m_lastLightParamSig) {
            m_lastLightParamSig = sig;
            VK_LOG_INFO("LIGHT-PARAMS type=%s labels=%s", typeName,
                        labels.empty() ? "(none)" : labels.c_str());
        }
    }
}

// ---------------------------------------------------------------- 脚本

void InspectorPanel::drawScript(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();
    auto* scomp = w.get<ecs::ScriptComponent>(e);
    if (!scomp) return;

    if (!ImGui::CollapsingHeader("Script (.vks)",
                                ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (m_scriptBufFor != e) {
        std::snprintf(m_scriptBuf, sizeof(m_scriptBuf), "%s",
                      scomp->path.c_str());
        m_scriptBufFor = e;
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##scriptpath", "scripts/foo.vks", m_scriptBuf,
                             sizeof(m_scriptBuf));
    if (ImGui::IsItemDeactivatedAfterEdit() && m_ctx.isEditing()) {
        const std::string newPath = m_scriptBuf;
        if (newPath != scomp->path) {
            m_ctx.structuralEdit("Edit Script Path",
                                 [&]() { scomp->path = newPath; });
        }
    }
    hint("relative to the asset root, or an absolute path");

    bool en = scomp->enabled;
    if (ImGui::Checkbox("Enabled##script", &en)) {
        scomp->enabled = en;
        discreteEdit(m_ctx, "Toggle Script", &scomp->enabled, !en);
    }
    ImGui::SameLine();
    const float ts0 = scomp->timeScale;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Time Scale", &scomp->timeScale, 0.0f, 4.0f, "%.2f");
    fieldEdit(m_ctx, "Script Time Scale", &scomp->timeScale, ts0);

    // 脚本只在运行态求值 —— 这条提示很重要，否则用户会以为坏了
    if (m_ctx.isEditing()) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f),
                           "Scripts only run in Play mode.");
    }

    if (removeButton() && m_ctx.isEditing()) {
        m_ctx.structuralEdit("Remove Script",
                             [&]() { w.remove<ecs::ScriptComponent>(e); });
        m_scriptBufFor = ecs::Entity{};
    }
}

// ---------------------------------------------------------------- 增删组件

void InspectorPanel::drawAddRemove(ecs::Entity e) {
    // 结构性改动（加/删组件）只在编辑态可用：运行态改的是副本，
    // 而且撤销栈里的整场景快照只认编辑态那份场景。
    if (!m_ctx.isEditing()) return;

    scene::Scene& sc = m_ctx.editorScene();
    ecs::World& w = sc.world();

    ImGui::Separator();
    if (ImGui::Button("Add Component")) ImGui::OpenPopup("add_component");
    if (ImGui::BeginPopup("add_component")) {
        const bool hasPoint = w.has<ecs::PointLightComponent>(e);
        const bool hasSpot = w.has<ecs::SpotLightComponent>(e);
        const bool hasScript = w.has<ecs::ScriptComponent>(e);

        if (ImGui::MenuItem("Point Light", nullptr, false, !hasPoint)) {
            m_ctx.structuralEdit("Add Point Light", [&]() {
                ecs::PointLightComponent pl;
                w.add<ecs::PointLightComponent>(e, pl);
            });
        }
        if (ImGui::MenuItem("Spot Light", nullptr, false, !hasSpot)) {
            m_ctx.structuralEdit("Add Spot Light", [&]() {
                ecs::SpotLightComponent sl;
                w.add<ecs::SpotLightComponent>(e, sl);
            });
        }
        if (ImGui::MenuItem("Script", nullptr, false, !hasScript)) {
            m_ctx.structuralEdit("Add Script", [&]() {
                ecs::ScriptComponent scp;
                // 路径留空，让用户自己填（不再指向任何示例脚本 —— 示例内容
                // 已经按用户要求从项目里删掉了，硬写一个不存在的路径只会
                // 让 Scripts 面板一上来就报编译错误）
                scp.path.clear();
                w.add<ecs::ScriptComponent>(e, scp);
            });
            m_scriptBufFor = ecs::Entity{};
        }
        ImGui::EndPopup();
    }
}

} // namespace editor
