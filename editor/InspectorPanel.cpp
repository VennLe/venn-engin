#include "InspectorPanel.h"

#include "assets/Material.h"
#include "assets/Mesh.h"
#include "assets/Texture.h"
#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

const char* textureLabel(const assets::Texture* t) {
    if (!t) return "(default)";
    const assets::TextureSource& s = t->source();
    if (!s.name.empty()) return s.name.c_str();
    if (!s.path.empty()) return s.path.c_str();
    return "(procedural)";
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

// ---------------------------------------------------------------- 网格 / 材质

void InspectorPanel::drawMeshAndMaterial(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    // ---- 网格 ----
    if (const auto* mc = w.get<ecs::MeshComponent>(e)) {
        if (ImGui::CollapsingHeader("Mesh")) {
            const assets::Mesh* m = mc->mesh;
            if (!m) {
                ImGui::TextDisabled("(null mesh)");
            } else {
                static const char* kKinds[] = {"Unknown", "Builtin", "OBJ",
                                               "glTF"};
                const assets::MeshSource& s = m->source();
                const int ki = static_cast<int>(s.kind);
                ImGui::Text("source: %s",
                            kKinds[(ki >= 0 && ki < 4) ? ki : 0]);
                if (!s.shape.empty())
                    ImGui::Text("shape : %s (size %.3f)", s.shape.c_str(),
                                static_cast<double>(s.size));
                if (!s.path.empty())
                    ImGui::Text("path  : %s", s.path.c_str());
                ImGui::Text("verts : %zu   tris: %zu", m->vertices().size(),
                            m->indices().size() / 3);
            }
        }
    }

    // ---- 材质 ----
    auto* matc = w.get<ecs::MaterialComponent>(e);
    if (!matc || !matc->material) return;
    assets::Material* mat = matc->material;

    if (!ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Text("name: %s", mat->name.c_str());
    hint("Materials are shared assets - editing affects every user");

    // base color
    const glm::vec4 c0 = mat->baseColorFactor;
    ImGui::ColorEdit4("Base Color", glm::value_ptr(mat->baseColorFactor));
    fieldEdit(m_ctx, "Material Base Color", &mat->baseColorFactor, c0);

    const float m0 = mat->metallic;
    ImGui::SliderFloat("Metallic", &mat->metallic, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material Metallic", &mat->metallic, m0);

    const float r0 = mat->roughness;
    ImGui::SliderFloat("Roughness", &mat->roughness, 0.02f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material Roughness", &mat->roughness, r0);

    const float a0 = mat->ao;
    ImGui::SliderFloat("AO", &mat->ao, 0.0f, 1.0f, "%.2f");
    fieldEdit(m_ctx, "Material AO", &mat->ao, a0);

    const float n0 = mat->normalScale;
    ImGui::SliderFloat("Normal Scale", &mat->normalScale, 0.0f, 2.0f, "%.2f");
    fieldEdit(m_ctx, "Material Normal Scale", &mat->normalScale, n0);

    const glm::vec3 e0 = mat->emissive;
    ImGui::ColorEdit3("Emissive", glm::value_ptr(mat->emissive));
    fieldEdit(m_ctx, "Material Emissive", &mat->emissive, e0);
    hint("emissive > 1 feeds bloom");

    // alpha mode
    const int modeBefore = static_cast<int>(mat->alphaMode);
    int mode = modeBefore;
    const char* kAlpha[] = {"Opaque", "Blend"};
    if (ImGui::Combo("Alpha Mode", &mode, kAlpha, 2)) {
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
    if (ImGui::Checkbox("Double Sided", &ds)) {
        const bool ob = mat->doubleSided;
        mat->doubleSided = ds;
        discreteEdit(m_ctx, "Material Double Sided", &mat->doubleSided, ob);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Textures (read-only - assign via the Content Browser)");
    ImGui::TextWrapped("albedo: %s", textureLabel(mat->albedoMap));
    ImGui::TextWrapped("normal: %s", textureLabel(mat->normalMap));
    ImGui::TextWrapped("orm   : %s", textureLabel(mat->ormMap));
}

// ---------------------------------------------------------------- 光照

void InspectorPanel::drawLights(ecs::Entity e) {
    scene::Scene& sc = m_ctx.activeScene();
    ecs::World& w = sc.world();

    if (auto* pl = w.get<ecs::PointLightComponent>(e)) {
        if (ImGui::CollapsingHeader("Point Light",
                                   ImGuiTreeNodeFlags_DefaultOpen)) {
            const glm::vec3 c0 = pl->color;
            ImGui::ColorEdit3("Color##pl", glm::value_ptr(pl->color));
            fieldEdit(m_ctx, "Point Light Color", &pl->color, c0);

            const float i0 = pl->intensity;
            ImGui::DragFloat("Intensity##pl", &pl->intensity, 0.2f, 0.0f, 500.0f,
                             "%.1f");
            fieldEdit(m_ctx, "Point Light Intensity", &pl->intensity, i0);

            const float r0 = pl->range;
            ImGui::DragFloat("Range##pl", &pl->range, 0.05f, 0.1f, 60.0f, "%.2f");
            fieldEdit(m_ctx, "Point Light Range", &pl->range, r0);
            hint("range drives cluster culling - keep it tight");

            bool en = pl->enabled;
            if (ImGui::Checkbox("Enabled##pl", &en)) {
                pl->enabled = en;
                discreteEdit(m_ctx, "Toggle Point Light", &pl->enabled, !en);
            }
            if (removeButton() && m_ctx.isEditing()) {
                m_ctx.structuralEdit("Remove Point Light",
                                     [&]() { w.remove<ecs::PointLightComponent>(e); });
            }
        }
    }

    if (auto* sl = w.get<ecs::SpotLightComponent>(e)) {
        if (ImGui::CollapsingHeader("Spot Light",
                                   ImGuiTreeNodeFlags_DefaultOpen)) {
            const glm::vec3 c0 = sl->color;
            ImGui::ColorEdit3("Color##sl", glm::value_ptr(sl->color));
            fieldEdit(m_ctx, "Spot Light Color", &sl->color, c0);

            const float i0 = sl->intensity;
            ImGui::DragFloat("Intensity##sl", &sl->intensity, 0.4f, 0.0f, 800.0f,
                             "%.1f");
            fieldEdit(m_ctx, "Spot Light Intensity", &sl->intensity, i0);

            const float r0 = sl->range;
            ImGui::DragFloat("Range##sl", &sl->range, 0.05f, 0.1f, 80.0f, "%.2f");
            fieldEdit(m_ctx, "Spot Light Range", &sl->range, r0);

            const glm::vec3 d0 = sl->direction;
            ImGui::DragFloat3("Direction##sl", glm::value_ptr(sl->direction),
                              0.01f, -1.0f, 1.0f);
            fieldEdit(m_ctx, "Spot Light Direction", &sl->direction, d0);

            float innerDeg = glm::degrees(sl->innerAngle);
            float outerDeg = glm::degrees(sl->outerAngle);
            const float in0 = sl->innerAngle;
            const float out0 = sl->outerAngle;
            if (ImGui::SliderFloat("Inner Cone", &innerDeg, 1.0f, 80.0f,
                                   "%.1f deg")) {
                sl->innerAngle = glm::radians(innerDeg);
            }
            fieldEdit(m_ctx, "Spot Inner Cone", &sl->innerAngle, in0);
            if (ImGui::SliderFloat("Outer Cone", &outerDeg, 1.0f, 89.0f,
                                   "%.1f deg")) {
                sl->outerAngle = glm::radians(outerDeg);
            }
            fieldEdit(m_ctx, "Spot Outer Cone", &sl->outerAngle, out0);

            bool en = sl->enabled;
            if (ImGui::Checkbox("Enabled##sl", &en)) {
                sl->enabled = en;
                discreteEdit(m_ctx, "Toggle Spot Light", &sl->enabled, !en);
            }
            if (removeButton() && m_ctx.isEditing()) {
                m_ctx.structuralEdit("Remove Spot Light",
                                     [&]() { w.remove<ecs::SpotLightComponent>(e); });
            }
        }
    }

    if (auto* dl = w.get<ecs::DirectionalLightComponent>(e)) {
        if (ImGui::CollapsingHeader("Directional Light",
                                   ImGuiTreeNodeFlags_DefaultOpen)) {
            const glm::vec3 d0 = dl->direction;
            ImGui::DragFloat3("Direction##dl", glm::value_ptr(dl->direction),
                              0.01f, -1.0f, 1.0f);
            fieldEdit(m_ctx, "Sun Direction", &dl->direction, d0);

            const glm::vec3 c0 = dl->color;
            ImGui::ColorEdit3("Color##dl", glm::value_ptr(dl->color));
            fieldEdit(m_ctx, "Sun Color", &dl->color, c0);

            const float i0 = dl->intensity;
            ImGui::SliderFloat("Intensity##dl", &dl->intensity, 0.0f, 4.0f);
            fieldEdit(m_ctx, "Sun Intensity", &dl->intensity, i0);

            const float a0 = dl->ambientScale;
            ImGui::SliderFloat("Ambient##dl", &dl->ambientScale, 0.0f, 1.0f,
                               "%.2f");
            fieldEdit(m_ctx, "Sun Ambient Scale", &dl->ambientScale, a0);

            bool cs = dl->castsShadow;
            if (ImGui::Checkbox("Casts Shadow##dl", &cs)) {
                dl->castsShadow = cs;
                discreteEdit(m_ctx, "Sun Casts Shadow", &dl->castsShadow, !cs);
            }
            hint("this is the scene's key light (scene::Scene::light)");
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
