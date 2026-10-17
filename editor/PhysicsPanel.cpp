#include "PhysicsPanel.h"

#include "DebugRects.h"
#include "Localization.h"
#include "PanelRegistry.h"

#include "core/Logger.h"

#include "ecs/Components.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cstdio>

namespace editor {

namespace {

// 一行设置的排版：左标签、右控件。-1 的宽度让控件吃到右边缘。
constexpr float kLabelW = 132.0f;

// 自动化（MYVK_LOG_RECTS=1）下把这个控件的矩形打出来，脚本按矩形点。
void tagItem(const char* tag) {
    logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

} // namespace

void PhysicsPanel::labelValue(const char* label, const char* tooltip) {
    ImGui::TextUnformatted(TR(label));
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR(tooltip));
    ImGui::SameLine(kLabelW);
    ImGui::SetNextItemWidth(-1.0f);
}

void PhysicsPanel::draw() {
    bool& open = panelOpenRef("physics");
    if (!open) return;

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(380.0f, 540.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.60f,
                                   io.DisplaySize.y * 0.14f),
                            ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(trId("Physics", "panel_physics"), &open)) {
        ImGui::End();
        return;
    }
    logRect("PHYS-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    scene::PhysicsSettings& ps = m_ctx.editorScene().physicsSettings();
    const PhysicsRuntimeInfo& rt = m_ctx.physicsRuntime();

    // ---------------------------------------------------------- 运行状态
    if (rt.active) {
        ImGui::TextColored(ImVec4(0.52f, 0.82f, 0.55f, 1.0f), "%s",
                           TR("SIMULATING"));
    } else {
        ImGui::TextDisabled("%s", TR("Idle (physics builds on the first Play frame)"));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_ctx.isEditing() ? TR("edit mode") : TR("runtime"));

    if (rt.active) {
        if (ImGui::BeginTable("##phys_stats", 2,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_RowBg)) {
            char buf[8][48];
            std::snprintf(buf[0], sizeof(buf[0]), "%d (%d dyn)", rt.bodies, rt.dynamic);
            std::snprintf(buf[1], sizeof(buf[1]), "%d", rt.sleeping);
            std::snprintf(buf[2], sizeof(buf[2]), "%d", rt.manifolds);
            std::snprintf(buf[3], sizeof(buf[3]), "%d", rt.contactPoints);
            std::snprintf(buf[4], sizeof(buf[4]), "%d", rt.broadphasePairs);
            std::snprintf(buf[5], sizeof(buf[5]), "%d", rt.joints);
            std::snprintf(buf[6], sizeof(buf[6]), "%.4f m", rt.maxPenetration);
            std::snprintf(buf[7], sizeof(buf[7]), "%.2f m/s", rt.maxSpeed);
            const char* keys[8] = {"Bodies", "Sleeping", "Manifolds", "Contacts",
                                   "Broadphase pairs", "Joints",
                                   "Max penetration", "Max speed"};
            for (int i = 0; i < 8; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(TR(keys[i]));
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", buf[i]);
            }
            ImGui::EndTable();
        }
        ImGui::Text(TR("steps %llu   sim time %.2f s"),
                    static_cast<unsigned long long>(rt.steps),
                    static_cast<double>(rt.simTime));
    } else {
        ImGui::TextDisabled(TR("Run the scene (green Play) to see live numbers."));
    }

    ImGui::Separator();

    // ---------------------------------------------------------- 场景级设置
    //
    // ⚠ 所有改动都打在**编辑场景**的设置上：Play 期间 activeScene() 是运行态
    //   副本，那儿改完 Stop 就丢，等于"调了没留下"。而运行态要读到新值，
    //   必须重新 Run —— 面板顶部那句提示说的就是这件事。
    bool dirty = false;

    if (ImGui::CollapsingHeader(TR("World"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(TR("Physics enabled"), &ps.enabled)) dirty = true;
        tagItem("PHYS-SET enabled");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("Off = no physics world is built at all; rigid\n"
                              "bodies just sit where they are."));
        }

        if (ImGui::Checkbox(TR("Ground plane"), &ps.groundPlane)) dirty = true;
        tagItem("PHYS-SET ground");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("An infinite z = 0 plane is added when Play starts.\n"
                              "Turn it off if you placed your own floor - otherwise\n"
                              "everything falls out of the world."));
        }

        labelValue("Gravity", "m/s^2, applied to every dynamic body");
        if (ImGui::DragFloat3("##phys_gravity", &ps.gravity.x, 0.05f, -100.0f,
                              100.0f, "%.2f")) {
            dirty = true;
            VK_LOG_INFO("PHYS-SET gravity=(%.2f,%.2f,%.2f)",
                        static_cast<double>(ps.gravity.x),
                        static_cast<double>(ps.gravity.y),
                        static_cast<double>(ps.gravity.z));
        }
        tagItem("PHYS-SET gravity");
    }

    if (ImGui::CollapsingHeader(TR("Timestep"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        labelValue("Fixed step", "Simulated seconds per physics step (1/60 = 60 Hz)");
        float hz = ps.fixedTimeStep > 1e-6f ? 1.0f / ps.fixedTimeStep : 60.0f;
        if (ImGui::DragFloat("##phys_hz", &hz, 1.0f, 20.0f, 240.0f, "%.0f Hz")) {
            ps.fixedTimeStep = 1.0f / hz;
            dirty = true;
            VK_LOG_INFO("PHYS-SET hz=%.0f", static_cast<double>(hz));
        }
        tagItem("PHYS-SET hz");

        labelValue("Max sub-steps", "Per frame cap - stops the death spiral");
        if (ImGui::SliderInt("##phys_sub", &ps.maxSubSteps, 1, 16)) {
            dirty = true;
            VK_LOG_INFO("PHYS-SET substeps=%d", ps.maxSubSteps);
        }
        tagItem("PHYS-SET substeps");
    }

    if (ImGui::CollapsingHeader(TR("Solver"), ImGuiTreeNodeFlags_DefaultOpen)) {
        labelValue("Velocity iters", "Sequential impulse iterations");
        if (ImGui::SliderInt("##phys_vi", &ps.velocityIterations, 1, 32)) {
            dirty = true;
            VK_LOG_INFO("PHYS-SET velocityIters=%d", ps.velocityIterations);
        }
        tagItem("PHYS-SET velit");

        labelValue("Position iters", "Position-correction iterations");
        if (ImGui::SliderInt("##phys_pi", &ps.positionIterations, 0, 16)) {
            dirty = true;
            VK_LOG_INFO("PHYS-SET positionIters=%d", ps.positionIterations);
        }
        tagItem("PHYS-SET posit");

        labelValue("Baumgarte", "Position error fed back into velocity (0.1-0.3)");
        if (ImGui::DragFloat("##phys_baum", &ps.baumgarte, 0.005f, 0.0f, 1.0f,
                             "%.3f")) {
            dirty = true;
            VK_LOG_INFO("PHYS-SET baumgarte=%.3f",
                        static_cast<double>(ps.baumgarte));
        }
        tagItem("PHYS-SET baumgarte");

        labelValue("Slop", "Allowed penetration (m) - jitter dampener");
        if (ImGui::DragFloat("##phys_slop", &ps.slop, 0.0005f, 0.0f, 0.1f,
                             "%.4f")) {
            dirty = true;
            VK_LOG_INFO("PHYS-SET slop=%.4f", static_cast<double>(ps.slop));
        }
        tagItem("PHYS-SET slop");
    }

    if (ImGui::CollapsingHeader(TR("Sleeping"))) {
        if (ImGui::Checkbox(TR("Allow sleeping"), &ps.allowSleep)) dirty = true;
        tagItem("PHYS-SET sleep");
        ImGui::BeginDisabled(!ps.allowSleep);
        labelValue("Linear threshold", "m/s below which a body may sleep");
        if (ImGui::DragFloat("##phys_slt", &ps.sleepLinearThreshold, 0.001f,
                             0.0f, 1.0f, "%.3f")) {
            dirty = true;
        }
        labelValue("Angular threshold", "rad/s below which a body may sleep");
        if (ImGui::DragFloat("##phys_sat", &ps.sleepAngularThreshold, 0.001f,
                             0.0f, 2.0f, "%.3f")) {
            dirty = true;
        }
        labelValue("Sleep time", "Seconds of stillness before sleeping");
        if (ImGui::DragFloat("##phys_st", &ps.sleepTime, 0.01f, 0.0f, 5.0f,
                             "%.2f s")) {
            dirty = true;
        }
        ImGui::EndDisabled();
    }

    if (dirty) {
        m_ctx.dirty() = true;
        if (!m_ctx.isEditing()) {
            // 运行态改设置 → 明确告诉用户"下一次 Run 才生效"
            m_ctx.setStatus(TR("Physics settings saved (applies on the next Run)"));
        }
        VK_LOG_INFO("PHYS-SET dirty=1 enabled=%d ground=%d",
                    ps.enabled ? 1 : 0, ps.groundPlane ? 1 : 0);
    }

    ImGui::Separator();
    if (ImGui::Button(TR("Reset to defaults"), ImVec2(-1.0f, 0.0f))) {
        m_ctx.structuralEdit("Reset Physics Settings", [&]() {
            ps = scene::PhysicsSettings{};
        });
        m_ctx.dirty() = true;
        VK_LOG_INFO("PHYS-SET reset=1");
    }
    tagItem("PHYS-SET reset");
    ImGui::TextDisabled(TR("Changes apply the next time you press Run."));

    ImGui::End();
}

} // namespace editor
