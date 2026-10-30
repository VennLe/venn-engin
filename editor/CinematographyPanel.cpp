#include "CinematographyPanel.h"

#include "DebugRects.h"
#include "Localization.h"
#include "PanelDock.h"
#include "PanelRegistry.h"

#include "core/Logger.h"

#include "ecs/Components.h"
#include "scene/Camera.h"
#include "scene/CameraMove.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace editor {

namespace {

// 组件里的滚转角存**度**（与 fov 同一惯例），姿态里是**弧度**。
constexpr float kDegToRad = 3.14159265358979f / 180.0f;

struct CamRow {
    ecs::Entity e{};
    std::string name;
    int priority = 0;
    bool enabled = true;
};

} // namespace

ecs::CameraComponent* CinematographyPanel::cur() {
    const ecs::Entity sel = m_ctx.selection();
    if (!sel.valid()) return nullptr;
    return m_ctx.editorScene().world().get<ecs::CameraComponent>(sel);
}

// ---------------------------------------------------------------- 相机列表
//
// 为什么要有：`CameraSystem` 按 priority 选活跃相机，而"现在到底是哪台在
// 出画面"在视口里**看不出来**（画面对着的那台不一定是你以为的那台）。
// 这里按优先级排序列出全部相机，最高优先级那台标绿 —— 排查"画面为什么
// 是俯拍"时一眼就有答案。
void CinematographyPanel::drawCameraList() {
    ecs::World& w = m_ctx.editorScene().world();

    std::vector<CamRow> rows;
    w.each<ecs::CameraComponent>([&](ecs::Entity e, ecs::CameraComponent& cc) {
        CamRow r;
        r.e = e;
        const std::string* n = w.name(e);
        r.name = n ? *n : std::string("<unnamed>");
        r.priority = cc.priority;
        r.enabled = cc.enabled;
        rows.push_back(r);
    });

    // 优先级高的在前；同优先级按名字（与 Inspector 的下拉同一规则，
    // 免得两处顺序不一致让人以为选错了）。
    std::sort(rows.begin(), rows.end(), [](const CamRow& a, const CamRow& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.name < b.name;
    });

    if (rows.empty()) {
        ImGui::TextDisabled("%s", TR("No camera in scene."));
        return;
    }

    // 活跃相机 = 第一条启用且优先级最高的（与 CameraSystem::selectActive 同规则）
    int activeIdx = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (rows[static_cast<std::size_t>(i)].enabled) { activeIdx = i; break; }
    }

    ImGui::BeginChild("##cine_cams", ImVec2(0.0f, 96.0f), ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const CamRow& r = rows[static_cast<std::size_t>(i)];
        char label[192];
        std::snprintf(label, sizeof(label), "%s  (%d)%s", r.name.c_str(),
                      r.priority, r.enabled ? "" : "  [off]");
        if (i == activeIdx)
            ImGui::TextColored(ImVec4(0.52f, 0.82f, 0.55f, 1.0f), "%s", label);
        else if (ImGui::Selectable(label, m_ctx.selection() == r.e))
            m_ctx.select(r.e);
        // 锚点：自动化要能"点这一行的相机"。行标题带优先级与开关，直接拿
        // 名字当 tag 会把方括号/空格带进日志 —— 只留字母数字，其余换 `_`。
        {
            std::string tag = "CINE-CAM-";
            for (char ch : r.name) {
                const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
                tag.push_back(ok ? ch : '_');
            }
            logRect(tag.c_str(), ImGui::GetItemRectMin(),
                    ImGui::GetItemRectMax());
        }
        if (i == activeIdx) {
            // 绿字也要能点（TextColored 不是可选项）
            if (ImGui::IsItemClicked()) m_ctx.select(r.e);
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- 镜头
void CinematographyPanel::drawLens(ecs::CameraComponent& cc) {
    if (!ImGui::CollapsingHeader(TR("Lens"), ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // 焦距优先：> 0 时 fov 由它算出来（见 cameraComponentToJson 顶上的说明）。
    // ⚠ 0 必须能用起来 —— 老工程与"只给 fov 打关键帧"的镜头都靠它。
    // ⚠ 开关状态**从组件值推**，不另存一份 bool：另存一份就会出现"面板上
    //   勾着、组件里 focal=0"这种只在下次开面板才自愈的分裂。
    {
        bool focalOn = cc.focal > 0.0f;
        if (ImGui::Checkbox(TR("Use focal length"), &focalOn)) {
            if (focalOn) {
                cc.focal = 50.0f;
            } else {
                // 关掉时把 fov **定死**在当前位置：否则下一帧 fov 会跳回
                // 组件里那个很久以前的值，画面凭空变一下（"关焦距"不该
                // 改变构图）。
                cc.fov = scene::focalToFovY(cc.focal, cc.sensorHeight);
                cc.focal = 0.0f;
            }
        }
    }
    if (cc.focal > 0.0f) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderFloat("##focal", &cc.focal, 8.0f, 300.0f, TR("%.0f mm"));
        logRect("CINE-FOCAL", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat(TR("Sensor h"), &cc.sensorHeight, 0.1f, 4.0f, 70.0f,
                         TR("%.1f mm"));
        const float fov = scene::focalToFovY(cc.focal, cc.sensorHeight);
        ImGui::SameLine();
        ImGui::TextDisabled("→ %.1f°", fov);
    } else {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderFloat("##fov", &cc.fov, 5.0f, 150.0f, "%.1f°");
    }

    // 离轴（传感器比例）
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Shift X"), &cc.shiftX, 0.005f, -1.0f, 1.0f, "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Shift Y"), &cc.shiftY, 0.005f, -1.0f, 1.0f, "%.3f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Off-axis lens shift: move the framing "
                                   "without rotating the camera."));

    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Roll"), &cc.rollDeg, 0.2f, -180.0f, 180.0f, "%.1f°");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Clip near"), &cc.clipNear, 0.01f, 0.0f, 50.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Clip far"), &cc.clipFar, 10.0f, 0.0f, 20000.0f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("0 = keep the engine default clip planes."));

    ImGui::Separator();

    // 景深
    ImGui::Checkbox(TR("Depth of field"), &cc.dofEnabled);
    logRect("CINE-DOF", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (cc.dofEnabled) {
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat(TR("f-stop"), &cc.fStop, 0.05f, 0.7f, 32.0f, "f/%.1f");
        logRect("CINE-FSTOP", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat(TR("Focus dist"), &cc.focusDistance, 0.1f, 0.1f, 500.0f,
                         TR("%.1f m"));
        // 读出来给作者看"这颗镜头到底多虚" —— 面板填了不换算，等于没填。
        const float focal = cc.focal > 0.0f ? cc.focal : 50.0f;
        ImGui::TextDisabled("%s %.2f", TR("aperture"),
                            scene::fStopToDofAperture(cc.fStop, focal));
    }

    // 快门（运动模糊）
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat(TR("Shutter"), &cc.shutterAngle, 0.0f, 360.0f, "%.0f°");
    logRect("CINE-SHUTTER", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("0 = off. 180° is the film standard "
                                   "(half-frame exposure)."));
    if (cc.shutterAngle > 0.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s %.2f", TR("blur"),
                            scene::shutterToBlurStrength(cc.shutterAngle));
    }
}

// ---------------------------------------------------------------- rig
void CinematographyPanel::drawRig(ecs::CameraComponent& cc) {
    if (!ImGui::CollapsingHeader(TR("Rig"), ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // 注视目标：列出场景里所有实体（带名字），第一项是"不跟"。
    ecs::World& w = m_ctx.editorScene().world();
    std::vector<CamRow> rows;
    w.each<ecs::TransformComponent>([&](ecs::Entity e, ecs::TransformComponent&) {
        const std::string* n = w.name(e);
        if (!n) return;
        if (e == m_ctx.selection()) return;   // 别让自己瞄自己
        CamRow r;
        r.e = e;
        r.name = *n;
        rows.push_back(r);
    });
    std::sort(rows.begin(), rows.end(),
              [](const CamRow& a, const CamRow& b) { return a.name < b.name; });

    const std::string* curName =
        (cc.lookTargetGuid != 0) ? w.name(w.find(cc.lookTargetGuid)) : nullptr;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##look", curName ? curName->c_str() : TR("(none)"))) {
        if (ImGui::Selectable(TR("(none)"), cc.lookTargetGuid == 0))
            cc.lookTargetGuid = 0;
        for (const CamRow& r : rows) {
            const bool sel = w.guid(r.e) == cc.lookTargetGuid;
            if (ImGui::Selectable(r.name.c_str(), sel))
                cc.lookTargetGuid = w.guid(r.e);
        }
        ImGui::EndCombo();
    }
    logRect("CINE-LOOK", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Damping"), &cc.lookDamping, 0.005f, 0.0f, 3.0f,
                     TR("%.2f s"));
    logRect("CINE-DAMP", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("0 = snap. Uses exponential smoothing, so "
                                   "the feel does not depend on frame rate."));

    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Handheld"), &cc.handheldDeg, 0.02f, 0.0f, 8.0f,
                     "%.2f°");
    logRect("CINE-HANDHELD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::DragFloat(TR("Hz"), &cc.handheldHz, 0.02f, 0.05f, 8.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat(TR("Seed"), &cc.handheldSeed, 0.05f, 0.0f, 100.0f, "%.2f");
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(TR("Hand pos"), &cc.handheldPosM, 0.002f, 0.0f, 0.5f,
                     TR("%.3f m"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Position wobble. Keep it under ~0.02 m "
                                   "or the horizon visibly swims."));
}

// ---------------------------------------------------------------- 路径
void CinematographyPanel::drawPath(ecs::CameraComponent& cc) {
    if (!ImGui::CollapsingHeader(TR("Follow path")))
        return;

    ecs::World& w = m_ctx.editorScene().world();
    const ecs::Entity pe = (cc.pathGuid != 0) ? w.find(cc.pathGuid) : ecs::Entity{};
    ecs::CameraPathComponent* pc =
        pe.valid() ? w.get<ecs::CameraPathComponent>(pe) : nullptr;

    if (!pc) {
        ImGui::TextDisabled("%s", TR("No path bound."));
        // 用"视口相机当前位置"当起点：作者摆好构图之后按这个键，路径从
        // 当前位置开始长，不用再手填一遍坐标。
        if (ImGui::Button(TR("New path from view"))) {
            const ecs::Entity ne = m_ctx.editorScene().createObject("CameraPath");
            auto& c = w.add<ecs::CameraPathComponent>(ne);
            const glm::vec3 vp = m_ctx.activeScene().camera().position();
            c.points.push_back(vp);
            // 第二个点放在"视线前方 5 米"，否则只有一个点时路径长度是 0，
            // 相机被钉在原地 —— 和"路径没生效"长得一模一样。
            const scene::Camera& vc = m_ctx.activeScene().camera();
            scene::CameraPose cp;
            cp.yaw = vc.yaw();
            cp.pitch = vc.pitch();
            c.points.push_back(vp + scene::poseForward(cp) * 5.0f);
            c.speed = m_newPathSpeed;
            cc.pathGuid = w.guid(ne);
            m_ctx.dirty() = true;
            VK_LOG_INFO("CINE-PATH action=new guid=%llx pts=%zu",
                        static_cast<unsigned long long>(cc.pathGuid),
                        c.points.size());
        }
        logRect("CINE-PATH-NEW", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        return;
    }

    const std::string* pn = w.name(pe);
    ImGui::Text("%s %s", TR("Path"), pn ? pn->c_str() : "?");
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("unbind"))) {
        cc.pathGuid = 0;
        m_ctx.dirty() = true;
        return;
    }

    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat(TR("Progress"), &pc->progress, 0.0f, 1.0f, "%.3f");
    logRect("CINE-PATH-PROGRESS", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());
    ImGui::SetNextItemWidth(220.0f);
    ImGui::DragFloat(TR("Speed"), &pc->speed, 0.005f, 0.0f, 4.0f, "%.3f /s");
    logRect("CINE-PATH-SPEED", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", TR("Arc-length fraction per second. 0 = hold "
                                   "at Progress (scrubbing)."));

    ImGui::Checkbox(TR("Closed"), &pc->closed);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Look along"), &pc->lookAlong);
    logRect("CINE-PATH-LOOK", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (pc->lookAlong) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::DragFloat(TR("Yaw off"), &pc->yawOffsetDeg, 0.5f, -180.0f, 180.0f,
                         "%.1f°");
    }

    // 点位列表：每行 [序号] [坐标] [删除]。位置用 DragFloat3 直接改 ——
    // "加一个点再手填坐标"是路径编辑里最常做的一件事。
    int removeIdx = -1;
    for (int i = 0; i < static_cast<int>(pc->points.size()); ++i) {
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(200.0f);
        ImGui::DragFloat3("##p", &pc->points[static_cast<std::size_t>(i)].x,
                          0.05f);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeIdx = i;
        ImGui::SameLine();
        if (i == 0 && ImGui::SmallButton(TR("from view"))) {
            pc->points[static_cast<std::size_t>(i)] =
                m_ctx.activeScene().camera().position();
        }
        ImGui::PopID();
    }
    if (removeIdx >= 0 && pc->points.size() > 2) {
        pc->points.erase(pc->points.begin() + removeIdx);
        m_ctx.dirty() = true;
    }
    if (ImGui::Button(TR("Add point (view)"))) {
        pc->points.push_back(m_ctx.activeScene().camera().position());
        m_ctx.dirty() = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s %d", TR("segments"),
                        pc->closed ? static_cast<int>(pc->points.size())
                                   : static_cast<int>(pc->points.size()) - 1);

    // 只读回显：曲线长度（作者靠它估速度 —— "10 米 / 4 秒 = 2.5 m/s"）
    {
        scene::CameraPath tmp;
        for (const glm::vec3& q : pc->points) {
            scene::CameraPathPoint cp;
            cp.position = q;
            tmp.points.push_back(cp);
        }
        tmp.closed = pc->closed;
        tmp.smoothHandles();
        ImGui::TextDisabled("%s %.2f m", TR("length"), tmp.length());
    }
}

// ---------------------------------------------------------------- 动词
void CinematographyPanel::verbButton(scene::CameraMoveKind kind,
                                     const char* label, const char* anchor,
                                     float sign) {
    const bool clicked = ImGui::Button(label, ImVec2(0.0f, 0.0f));
    // ⚠ 锚点必须**无条件**打（布尔量另存）：写成 `if (!Button) return;` 再
    //   logRect，锚点就只有"点中的那一下"才出现 —— 自动化根本找不到它，
    //   而人肉点击是能用的，于是这条会一直"看起来正常"。
    logRect(anchor, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!clicked) return;

    scene::Camera& vc = m_ctx.activeScene().camera();
    scene::CameraPose pose;
    pose.position = vc.position();
    pose.yaw = vc.yaw();
    pose.pitch = vc.pitch();
    pose.fov = vc.fov();

    scene::CameraMoveSpec spec;
    spec.kind = kind;
    spec.amount =
        sign * (scene::cameraMoveIsAngular(kind) ? m_amountDeg : m_amountM);
    // Orbit / Crane 需要一个支点：取"视线前方 5 米"当支点 —— 那是"环绕
    // 面前这个东西"的默认意图，比要求作者先建一个空物体顺手得多。
    if (scene::cameraMoveNeedsPivot(kind)) {
        spec.pivot = pose.position + scene::poseForward(pose) * 5.0f;
        spec.pivotValid = true;
    }
    scene::applyCameraMove(pose, spec);

    vc.setFlyMode(true);
    vc.setUseGlobalInput(false);
    vc.setPosition(pose.position);
    vc.setYaw(pose.yaw);
    vc.setPitch(pose.pitch);
    vc.setFov(pose.fov);
    // Roll 也必须写回：`Camera::setYaw/setPitch` 只同步视线，roll 是**独立**
    // 的一个通道（见 Camera.h）。漏这一句 = "面板上点了滚转，画面纹丝不动"。
    vc.setRoll(pose.roll);

    VK_LOG_INFO("CINE-VERB action=%s amount=%.2f pos=(%.2f,%.2f,%.2f) "
                "yaw=%.1f pitch=%.1f",
                scene::cameraMoveName(kind), spec.amount, pose.position.x,
                pose.position.y, pose.position.z, pose.yaw / kDegToRad,
                pose.pitch / kDegToRad);
}

void CinematographyPanel::drawVerbs() {
    if (!ImGui::CollapsingHeader(TR("Move camera (viewport)"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat(TR("Angle"), &m_amountDeg, 0.5f, -180.0f, 180.0f, "%.1f°");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat(TR("Distance"), &m_amountM, 0.02f, -50.0f, 50.0f, TR("%.2f m"));
    logRect("CINE-AMOUNT", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    // 两列布局：奇数个按钮时最后一格空着，比让最后一个按钮撑满好看。
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) * 0.5f;
    auto pair = [&](scene::CameraMoveKind k, const char* l, const char* a) {
        ImGui::SameLine(0.0f, st.ItemSpacing.x);
        ImGui::SetNextItemWidth(w);
        verbButton(k, l, a);
    };

    using K = scene::CameraMoveKind;
    ImGui::SetNextItemWidth(w);
    verbButton(K::Pan, TR("Pan +"), "CINE-VERB-PAN", +1.0f);
    pair(K::Tilt, TR("Tilt +"), "CINE-VERB-TILT");
    ImGui::SetNextItemWidth(w);
    verbButton(K::Pan, TR("Pan −"), "CINE-VERB-PAN-NEG", -1.0f);
    pair(K::Truck, TR("Truck −"), "CINE-VERB-TRUCK-NEG");
    ImGui::SetNextItemWidth(w);
    verbButton(K::Dolly, TR("Dolly +"), "CINE-VERB-DOLLY");
    pair(K::Pedestal, TR("Pedestal +"), "CINE-VERB-PEDESTAL");
    ImGui::SetNextItemWidth(w);
    verbButton(K::Roll, TR("Roll +"), "CINE-VERB-ROLL");
    pair(K::Orbit, TR("Orbit +"), "CINE-VERB-ORBIT");
    ImGui::SetNextItemWidth(w);
    verbButton(K::Crane, TR("Crane +"), "CINE-VERB-CRANE");
}

// ---------------------------------------------------------------- 写回实体
void CinematographyPanel::drawBake() {
    ImGui::Separator();
    // ⚠ 与 verbButton 同一条：锚点无条件打，点击才是条件分支。
    const bool clicked = ImGui::Button(TR("Bake view pose to entity"));
    logRect("CINE-BAKE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (!clicked) return;

    const ecs::Entity sel = m_ctx.selection();
    if (!sel.valid()) {
        m_ctx.notify(TR("Select a camera entity first."));
        return;
    }
    auto* tr = m_ctx.editorScene().world().get<ecs::TransformComponent>(sel);
    if (!tr) {
        m_ctx.notify(TR("Selected entity has no transform."));
        return;
    }

    const scene::Camera& vc = m_ctx.activeScene().camera();
    scene::CameraPose pose;
    pose.position = vc.position();
    pose.yaw = vc.yaw();
    pose.pitch = vc.pitch();
    pose.fov = vc.fov();
    pose.roll = vc.roll();

    tr->position = pose.position;
    tr->rotation = scene::cameraEulerFor(pose);
    if (auto* cc = cur()) cc->fov = pose.fov;
    m_ctx.dirty() = true;

    VK_LOG_INFO("CINE-BAKE pos=(%.2f,%.2f,%.2f) rotDeg=(%.1f,%.1f,%.1f)",
                pose.position.x, pose.position.y, pose.position.z,
                tr->rotation.x / kDegToRad, tr->rotation.y / kDegToRad,
                tr->rotation.z / kDegToRad);
    m_ctx.notify(TR("Pose baked into entity."));
}

// ---------------------------------------------------------------- 主绘制
void CinematographyPanel::draw() {
    bool& open = panelOpenRef("cine");
    if (!open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(400.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.62f,
                                       io.DisplaySize.y * 0.12f),
                                ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(trId("Cinematography", "panel_cine"),
                      docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("CINE-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    if (!m_ctx.isEditing()) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s",
                           TR("Playing: edits apply to the editor scene, "
                              "the picture comes from the runtime copy."));
    }

    drawCameraList();
    ImGui::Separator();

    ecs::CameraComponent* cc = cur();
    if (!cc) {
        ImGui::TextDisabled("%s", TR("Select a camera entity in the list "
                                     "above (or in the scene tree)."));
        // 动词区不依赖组件 —— 仍然可用（它就是推视口相机）
        drawVerbs();
        ImGui::End();
        return;
    }

    drawLens(*cc);
    drawRig(*cc);
    drawPath(*cc);
    drawVerbs();
    drawBake();

    // ---- 状态行（每帧一行，变化时才落日志）----
    // 为什么要有：面板上十几个旋钮，出问题时要能一口气看到"现在这套参数
    // 到底是多少"。日志只在变化时打，回归脚本就能按它做断言。
    {
        const std::string* n = m_ctx.editorScene().world().name(m_ctx.selection());
        // `fovEff` 是**真正会进画面的**那个视场角：焦距 > 0 时由它算出来，
        // 否则就是 fov。分开打是因为 `fov` 这个字段在装了定焦镜头之后是**残留
        // 值**（老值），拿它当"现在多广"会得到一个永远不动的数。
        const float fovEff =
            cc->focal > 0.0f ? scene::focalToFovY(cc->focal, cc->sensorHeight)
                             : cc->fov;
        char buf[384];
        std::snprintf(buf, sizeof(buf),
                      "sel=%s focal=%.1f fov=%.1f fovEff=%.2f shift=(%.3f,%.3f) "
                      "roll=%.1f dof=%d fstop=%.1f shutter=%.0f look=%llx "
                      "damp=%.2f hand=%.2f/%.2f path=%llx",
                      n ? n->c_str() : "-", cc->focal, cc->fov, fovEff,
                      cc->shiftX, cc->shiftY, cc->rollDeg,
                      cc->dofEnabled ? 1 : 0, cc->fStop, cc->shutterAngle,
                      static_cast<unsigned long long>(cc->lookTargetGuid),
                      cc->lookDamping, cc->handheldDeg, cc->handheldHz,
                      static_cast<unsigned long long>(cc->pathGuid));
        if (m_lastSig != buf) {
            m_lastSig = buf;
            VK_LOG_INFO("CINE-STATE %s", buf);
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", buf);
    }

    ImGui::End();
}

} // namespace editor
