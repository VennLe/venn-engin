#include "PhysicsPanel.h"

#include "DebugRects.h"
#include "PanelDock.h"
#include "Localization.h"
#include "PanelRegistry.h"

#include "core/Logger.h"

#include "ecs/Components.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

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

// ============================================================
// 关节总览
//
// 为什么要单独一块：`JointComponent` 的 `target` 是一个 guid，指向"另一端"。
// 从任何**单端**视角（Inspector）都看不出下面这三种坏法：
//   · 另一端被删了        → guid 悬空，Play 时 WARN 跳过，屏幕上看不出原因
//   · 另一端没有刚体      → 同样建不出来
//   · 另一端悄悄改了名    → 你以为连着的是 A，其实早换成 B 了
// 而这三类恰恰是搭链条时最常见的。所以这里按场景全局列一遍，坏的标红，
// 点一行就选中 —— 选不中/选错了才能立刻发现。
//
// 判定顺序是有讲究的：先判**自己**再判对面。自己没刚体时，无论对面是什么
// 都建不成，此时报"对面缺刚体"是误导（用户会去对面乱加东西）。
// ============================================================
void PhysicsPanel::drawJointList() {
    ecs::World& w = m_ctx.editorScene().world();
    const PhysicsRuntimeInfo& rt = m_ctx.physicsRuntime();

    struct Row {
        ecs::Entity self{};
        const ecs::JointComponent* jc = nullptr;
        ecs::Entity other{};
        const char* state = "ok";   // ok / no-self-body / no-target / missing /
                                    // no-other-body
    };
    std::vector<Row> rows;
    w.each<ecs::JointComponent>([&](ecs::Entity e, ecs::JointComponent& jc) {
        Row r;
        r.self = e;
        r.jc = &jc;
        if (!w.has<ecs::RigidBodyComponent>(e)) {
            r.state = "no-self-body";
        } else if (jc.target == ecs::kInvalidGuid) {
            r.state = "no-target";
        } else {
            const ecs::Entity o = w.find(jc.target);
            if (!o.valid()) {
                r.state = "missing";
            } else if (!w.has<ecs::RigidBodyComponent>(o)) {
                r.state = "no-other-body";
            } else {
                r.other = o;
            }
        }
        rows.push_back(r);
    });

    // 按名字排（同名的用 guid 定序）—— 和 Inspector 里那个下拉用同一条规则，
    // 免得两边顺序不一致让人以为选错了。
    auto nameOf = [&](ecs::Entity e) -> std::string {
        const std::string* n = w.name(e);
        return n ? *n : std::string();
    };
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        const std::string sa = nameOf(a.self), sb = nameOf(b.self);
        if (sa != sb) return sa < sb;
        return w.guid(a.self) < w.guid(b.self);
    });

    const bool open = ImGui::CollapsingHeader(TR("Joints"),
                                              ImGuiTreeNodeFlags_DefaultOpen);
    logRect("PHYS-JOINTS-HEADER", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (!open) return;

    int bad = 0, off = 0;
    for (const Row& r : rows) {
        const bool broken = std::strcmp(r.state, "ok") != 0;
        if (broken) ++bad;
        if (!r.jc->enabled) ++off;
    }

    // 建成的条数只有在跑起来之后才有意义 —— 编辑态显示 authored 数会被
    // 误读成"已经建好了"。
    if (rt.active) {
        ImGui::Text(TR("%d joint(s), %d built"), static_cast<int>(rows.size()),
                    rt.joints);
    } else {
        ImGui::Text(TR("%d joint(s)"), static_cast<int>(rows.size()));
    }
    logRect("PHYS-JOINTS-COUNT", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    if (bad > 0) {
        ImGui::TextColored(ImVec4(0.93f, 0.55f, 0.42f, 1.0f), "%s",
                           TR("some joints cannot be built - see the red rows"));
    }
    if (rt.active && bad == 0 && rt.joints < static_cast<int>(rows.size()) - off) {
        // authored 的都能建，但实际建成的比"启用且有效"的少：说明有别的原因
        // （世界没建起来 / 目标在另一个场景……）。不猜，直接指日志。
        ImGui::TextColored(ImVec4(0.92f, 0.72f, 0.35f, 1.0f), "%s",
                           TR("fewer joints were built than authored - check "
                              "the PHYS-JOINT lines in the log"));
    }

    if (rows.empty()) {
        ImGui::TextDisabled(TR("No joints in this scene."));
    }

    if (ImGui::BeginTable("##phys_joints", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV)) {
        const ecs::Entity sel = m_ctx.selection();
        int i = 0;
        for (const Row& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);

            const bool broken = std::strcmp(r.state, "ok") != 0;
            if (broken) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.95f, 0.48f, 0.38f, 1.0f));
            } else if (!r.jc->enabled) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.55f, 0.55f, 0.58f, 1.0f));
            }

            // 行标签 = 名字 → 另一端名字。名字为空时给个稳定占位，否则
            // 整行只剩箭头（ImGui 也会因为没有唯一 label 而抢 ID）。
            std::string label = nameOf(r.self);
            if (label.empty()) label = "(unnamed)";
            label += "  ->  ";
            if (r.other.valid()) {
                const std::string on = nameOf(r.other);
                label += on.empty() ? "(unnamed)" : on;
            } else if (std::strcmp(r.state, "no-target") == 0) {
                label += TR("(no target)");
            } else if (std::strcmp(r.state, "missing") == 0) {
                label += TR("(deleted)");
            } else {
                label += TR("(no rigid body)");
            }

            if (ImGui::Selectable(label.c_str(), sel.valid() &&
                                                       sel.id == r.self.id)) {
                m_ctx.select(r.self);
                VK_LOG_INFO("PHYS-JOINTS pick entity=%u state=%s", r.self.id,
                            r.state);
            }
            if (broken || !r.jc->enabled) ImGui::PopStyleColor();

            char tag[48];
            std::snprintf(tag, sizeof(tag), "PHYS-JOINT-ROW-%d", i);
            logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            // 右键：跳到这一端 / 跳到另一端 / 删掉。
            // "Select other end" 是这块面板最实用的一条 —— 顺着链条往下走，
            // 不用在 Hierarchy 里翻。
            if (ImGui::BeginPopupContextItem("##joint_ctx")) {
                if (ImGui::MenuItem(TR("Select this object"))) {
                    m_ctx.select(r.self);
                }
                if (ImGui::MenuItem(TR("Select the other end"),
                                    nullptr, false, r.other.valid())) {
                    m_ctx.select(r.other);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(TR("Remove joint"))) {
                    const ecs::Entity self = r.self;
                    m_ctx.structuralEdit("Remove Joint", [&] {
                        w.remove<ecs::JointComponent>(self);
                    });
                    m_ctx.dirty() = true;
                    VK_LOG_INFO("PHYS-JOINTS remove entity=%u", self.id);
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;   // w 改过了，rows 里的指针立刻失效
                }
                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", TR(ecs::jointKindName(r.jc->kind)));
            ImGui::PopID();
            ++i;
        }
        ImGui::EndTable();
    }

    // ---- 新加一条：省得为了"加关节"跑一趟 Inspector ----
    // 只在**恰好选中一个、带刚体、还没有关节**的实体上可用。三个条件缺一个
    // 都说明用户还没准备好，给灰按钮比给一个点了没反应的按钮好。
    const ecs::Entity one = m_ctx.selection();
    const bool canAdd = one.valid() && w.has<ecs::RigidBodyComponent>(one) &&
                        !w.has<ecs::JointComponent>(one) && m_ctx.isEditing();
    ImGui::BeginDisabled(!canAdd);
    if (ImGui::Button(TR("Add joint to selection"))) {
        m_ctx.structuralEdit("Add Joint", [&] {
            w.add<ecs::JointComponent>(one, ecs::JointComponent{});
        });
        m_ctx.dirty() = true;
        VK_LOG_INFO("PHYS-JOINTS add entity=%u", one.id);
    }
    ImGui::EndDisabled();
    logRect("PHYS-JOINTS-ADD", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            TR("Select one object that has a rigid body (and no joint yet).\n"
               "The other end is picked inside the Inspector."));
    }

    // ---- 状态日志（签名变了才打）----
    std::string sig;
    for (const Row& r : rows) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%u:%d:%d:%llx:%s|", r.self.id,
                      static_cast<int>(r.jc->kind), r.jc->enabled ? 1 : 0,
                      static_cast<unsigned long long>(r.jc->target), r.state);
        sig += buf;
    }
    if (sig != m_jointSig) {
        m_jointSig = sig;
        VK_LOG_INFO("PHYS-JOINTS authored=%d bad=%d off=%d built=%d",
                    static_cast<int>(rows.size()), bad, off,
                    rt.active ? rt.joints : -1);
        for (std::size_t k = 0; k < rows.size(); ++k) {
            const Row& r = rows[k];
            VK_LOG_INFO("PHYS-JOINT-LIST i=%d entity=%u kind=%d en=%d state=%s",
                        static_cast<int>(k), r.self.id,
                        static_cast<int>(r.jc->kind), r.jc->enabled ? 1 : 0,
                        r.state);
        }
    }
}

void PhysicsPanel::draw() {
    bool& open = panelOpenRef("physics");
    if (!open) return;

    // 2026-10-21：这个面板现在是视口标签条上的一页（锚定哪一块由
    // EditorApp::drawPanels 通过 dockHint 给出）。没在被托管时才用
    // "第一次出现摆右上角"的浮动默认值。
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(380.0f, 540.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.60f,
                                       io.DisplaySize.y * 0.14f),
                                ImGuiCond_FirstUseEver);
    }

    if (!ImGui::Begin(trId("Physics", "panel_physics"),
                      docked ? nullptr : &open, wf)) {
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
            std::snprintf(buf[0], sizeof(buf[0]), TR("%d (%d dyn)"), rt.bodies, rt.dynamic);
            std::snprintf(buf[1], sizeof(buf[1]), "%d", rt.sleeping);
            std::snprintf(buf[2], sizeof(buf[2]), "%d", rt.manifolds);
            std::snprintf(buf[3], sizeof(buf[3]), "%d", rt.contactPoints);
            std::snprintf(buf[4], sizeof(buf[4]), "%d", rt.broadphasePairs);
            std::snprintf(buf[5], sizeof(buf[5]), "%d", rt.joints);
            std::snprintf(buf[6], sizeof(buf[6]), TR("%.4f m"), rt.maxPenetration);
            std::snprintf(buf[7], sizeof(buf[7]), TR("%.2f m/s"), rt.maxSpeed);
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

    // ---------------------------------------------------------- 关节总览
    drawJointList();

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
        if (ImGui::DragFloat("##phys_hz", &hz, 1.0f, 20.0f, 240.0f, TR("%.0f Hz"))) {
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
                             TR("%.2f s"))) {
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
