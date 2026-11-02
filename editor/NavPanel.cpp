// ============================================================
// editor/NavPanel.cpp
//
// ⚠ 界面字面量**全部**包 `TR()`：这是 i18n 闸门（B / B2 组）的硬要求，
//   而且它抓的是"漏翻"，不是"崩"——漏一处只是中文界面里冒一行英文，
//   跑一遍所有用例都不会红。所以每个新面板的第一版就要接上。
// ⚠ 控件**不传 format 串**（`DragFloat(..., "%.2f")` 那种）：控件格式串是
//   i18n 的第三个盲区（E 组），而"标签可见 + 数字无单位"已经够读了 ——
//   少一个格式串就少一条要翻译、还要对占位符的东西。
// ============================================================

#include "NavPanel.h"

#include "DebugRects.h"
#include "Localization.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "scene/Scene.h"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace editor {

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// 显示名 + "##锚点"。锚点让两个同名控件（比如两处 "Speed"）不撞 ID。
std::string lbl(const char* shown, const char* tag) {
    return std::string(shown) + "##" + tag;
}

} // namespace

// ---------------------------------------------------------------- 命令入口

std::string NavPanel::bakeInto(nav::NavSystem& sys, const BakeFn& bake) {
    if (!bake) {
        m_status = "NAV-BAKE failed reason=no-bake-host";
        return m_status;
    }
    m_status = bake(m_desc, sys);
    return m_status;
}

std::string NavPanel::saveGrid(nav::NavSystem& sys, const GridIoFn& io) {
    if (!io) {
        m_status = "NAV-GRID save=0 reason=no-io-host";
        return m_status;
    }
    m_status = io(true, std::string(m_gridPath), sys);
    return m_status;
}

std::string NavPanel::loadGrid(nav::NavSystem& sys, const GridIoFn& io) {
    if (!io) {
        m_status = "NAV-GRID load=0 reason=no-io-host";
        return m_status;
    }
    m_status = io(false, std::string(m_gridPath), sys);
    return m_status;
}

std::string NavPanel::stateLine(const nav::NavSystem& sys) const {
    char buf[320];
    if (!sys.hasGrid()) {
        // i18n-keep —— 纯日志锚点（回归脚本按字段名解析，翻成中文等于让脚本失联）
        std::snprintf(buf, sizeof(buf), "nav.state grid=0 agents=%d", sys.stats().agents);
        return std::string(buf);
    }
    const nav::NavGrid& g = sys.grid();
    // i18n-keep —— 纯日志锚点（回归脚本按字段名解析，翻成中文等于让脚本失联）
    std::snprintf(buf, sizeof(buf),
                  "nav.state grid=1 cols=%d rows=%d cell=%.2f walk=%zu origin=(%.2f,%.2f) "
                  "maxStep=%.2f agents=%d",
                  g.cols, g.rows, g.cellSize, g.walkableCount(), g.origin.x, g.origin.y,
                  g.maxStep, sys.stats().agents);
    return std::string(buf);
}

std::string NavPanel::ascii(const nav::NavSystem& sys) const {
    if (!sys.hasGrid()) return std::string("nav.ascii grid=0");
    const nav::NavGrid& g = sys.grid();
    char head[160];
    // i18n-keep —— 纯日志锚点（回归脚本按字段名解析，翻成中文等于让脚本失联）
    std::snprintf(head, sizeof(head), "nav.ascii cols=%d rows=%d cell=%.2f walk=%zu", g.cols,
                  g.rows, g.cellSize, g.walkableCount());
    return std::string(head) + "\n" + nav::toAscii(g);
}

std::string NavPanel::pathTest(const nav::NavSystem& sys, const glm::vec3& from,
                               const glm::vec3& to) const {
    if (!sys.hasGrid()) return std::string("NAV-PATH grid=0 reached=0 pts=0 len=0.00 expanded=0");
    nav::NavPath p;
    const bool ok = nav::findPath(sys.grid(), from, to, p);
    if (!ok) return std::string("NAV-PATH grid=1 ok=0 reached=0 pts=0 len=0.00 expanded=0");
    // ⚠ 报的是**平滑前**的点数与长度：平滑是"少走几步"的展示，判据要看
    //   A* 自己出来的东西（否则"平滑坏了"会被当成"A* 坏了"）。
    char buf[200];
    // i18n-keep —— 纯日志锚点（回归脚本按字段名解析，翻成中文等于让脚本失联）
    std::snprintf(buf, sizeof(buf), "NAV-PATH grid=1 ok=1 reached=%d pts=%zu len=%.3f expanded=%d",
                  p.reached ? 1 : 0, p.pts.size(), static_cast<double>(p.length), p.expanded);
    return std::string(buf);
}

std::string NavPanel::attachAgent(scene::Scene& scene, ecs::Entity e, const glm::vec3& goal) {
    if (!e.valid()) return std::string("NAV-AGENT ok=0 reason=no-selection");
    if (auto* a = scene.world().get<ecs::NavAgentComponent>(e)) {
        a->goal = goal;
        a->hasGoal = true;
        a->path.clear();
        a->next = 0;
        a->repathTimer = 0.0f;
        a->reached = false;
        a->failed = false;
    } else {
        ecs::NavAgentComponent fresh;
        fresh.goal = goal;
        fresh.hasGoal = true;
        scene.world().add<ecs::NavAgentComponent>(e, fresh);
    }
    char buf[200];
    // i18n-keep —— 纯日志锚点（回归脚本按字段名解析，翻成中文等于让脚本失联）
    std::snprintf(buf, sizeof(buf), "NAV-AGENT ok=1 goal=(%.2f,%.2f,%.2f)", goal.x, goal.y, goal.z);
    return std::string(buf);
}

std::string NavPanel::stopAgent(scene::Scene& scene, nav::NavSystem& sys, ecs::Entity e) {
    if (!e.valid()) return std::string("NAV-STOP ok=0 reason=no-selection");
    if (!scene.world().get<ecs::NavAgentComponent>(e))
        return std::string("NAV-STOP ok=0 reason=not-an-agent");
    sys.stop(scene, e);
    return std::string("NAV-STOP ok=1");
}

std::string NavPanel::agentLine(scene::Scene& scene, ecs::Entity e) const {
    if (!e.valid()) return std::string("agent=none");
    auto* a = scene.world().get<ecs::NavAgentComponent>(e);
    if (!a) return std::string("agent=none");
    char buf[320];
    // i18n-keep —— 纯日志锚点（回归脚本按字段名解析，翻成中文等于让脚本失联）
    std::snprintf(buf, sizeof(buf),
                  "agent=1 speed=%.2f arrive=%.2f hasGoal=%d goal=(%.2f,%.2f,%.2f) pts=%zu next=%zu "
                  "reached=%d failed=%d expanded=%d",
                  a->speed, a->arriveDist, a->hasGoal ? 1 : 0, a->goal.x, a->goal.y, a->goal.z,
                  a->path.size(), a->next, a->reached ? 1 : 0, a->failed ? 1 : 0, a->lastExpanded);
    return std::string(buf);
}

// ---------------------------------------------------------------- 绘制

void NavPanel::draw(nav::NavSystem& sys, scene::Scene& scene, ecs::Entity selected,
                    const BakeFn& bake, const GridIoFn& io) {
    ImGui::TextUnformatted(TR("Navigation grid"));
    ImGui::Separator();

    ImGui::PushItemWidth(230.0f);
    ImGui::DragFloat3(lbl(TR("Bounds min"), "navboundsmin").c_str(), &m_desc.min.x, 0.25f, -4096.0f,
                      4096.0f);
    tagItem("NAV-BOUNDS-MIN");
    ImGui::DragFloat3(lbl(TR("Bounds max"), "navboundsmax").c_str(), &m_desc.max.x, 0.25f, -4096.0f,
                      4096.0f);
    tagItem("NAV-BOUNDS-MAX");
    ImGui::DragFloat(lbl(TR("Cell size"), "navcell").c_str(), &m_desc.cellSize, 0.05f, 0.1f, 4.0f);
    tagItem("NAV-CELL");
    ImGui::DragFloat(lbl(TR("Agent radius"), "navradius").c_str(), &m_desc.agentRadius, 0.02f, 0.0f,
                     3.0f);
    tagItem("NAV-RADIUS");
    ImGui::DragFloat(lbl(TR("Agent height"), "navheight").c_str(), &m_desc.agentHeight, 0.05f, 0.1f,
                     4.0f);
    tagItem("NAV-HEIGHT");
    ImGui::DragFloat(lbl(TR("Max slope"), "navslope").c_str(), &m_desc.maxSlopeDeg, 0.5f, 5.0f,
                     89.0f);
    tagItem("NAV-SLOPE");
    ImGui::DragFloat(lbl(TR("Max step"), "navstep").c_str(), &m_desc.maxStep, 0.02f, 0.0f, 2.0f);
    tagItem("NAV-STEP");
    ImGui::PopItemWidth();

    if (ImGui::Button(TR("Bake"))) {
        VK_LOG_INFO("[nav] %s", bakeInto(sys, bake).c_str());
    }
    tagItem("NAV-BAKE");
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear"))) {
        sys.clearGrid();
        m_status = "NAV-BAKE cleared";
        VK_LOG_INFO("[nav] grid cleared");
    }
    tagItem("NAV-CLEAR");
    ImGui::SameLine();
    if (ImGui::Button(TR("Print grid"))) {
        // 多行一次打完：逐行 print 会把 Console 的环形缓冲冲掉。
        VK_LOG_INFO("%s", ascii(sys).c_str());
        m_status = sys.hasGrid() ? "NAV-ASCII printed" : "NAV-ASCII grid=0";
    }
    tagItem("NAV-ASCII");

    ImGui::PushItemWidth(230.0f);
    ImGui::InputText(lbl(TR("Grid asset"), "navgridpath").c_str(), m_gridPath, sizeof(m_gridPath));
    tagItem("NAV-GRID-PATH");
    ImGui::PopItemWidth();
    if (ImGui::Button(TR("Save grid"))) {
        VK_LOG_INFO("[nav] %s", saveGrid(sys, io).c_str());
    }
    tagItem("NAV-GRID-SAVE");
    ImGui::SameLine();
    if (ImGui::Button(TR("Load grid"))) {
        VK_LOG_INFO("[nav] %s", loadGrid(sys, io).c_str());
    }
    tagItem("NAV-GRID-LOAD");

    ImGui::TextWrapped("%s", m_status.empty() ? "(no grid baked yet)" : m_status.c_str());
    if (sys.hasGrid()) {
        const nav::NavGrid& g = sys.grid();
        ImGui::Text(TR("cols=%d rows=%d cell=%.2f walk=%zu"), g.cols, g.rows, g.cellSize,
                    g.walkableCount());
    }

    ImGui::Separator();
    ImGui::TextUnformatted(TR("Path test"));
    ImGui::PushItemWidth(230.0f);
    ImGui::DragFloat3(lbl(TR("From"), "navfrom").c_str(), &m_from.x, 0.25f, -4096.0f, 4096.0f);
    tagItem("NAV-FROM");
    ImGui::DragFloat3(lbl(TR("To"), "navto").c_str(), &m_to.x, 0.25f, -4096.0f, 4096.0f);
    tagItem("NAV-TO");
    ImGui::PopItemWidth();
    if (ImGui::Button(TR("Find path"))) {
        const std::string line = pathTest(sys, m_from, m_to);
        VK_LOG_INFO("%s", line.c_str());
        m_status = line;
    }
    tagItem("NAV-PATH-TEST");
    ImGui::SameLine();
    if (ImGui::Button(TR("From selection"))) {
        if (selected.valid()) {
            if (auto* tr = scene.world().get<ecs::TransformComponent>(selected)) {
                m_from = tr->position;
                m_to = tr->position;
            }
        }
    }
    tagItem("NAV-USE-SELECTION");

    ImGui::Separator();
    ImGui::TextUnformatted(TR("Agent (selection)"));
    ImGui::TextWrapped("%s", agentLine(scene, selected).c_str());
    if (ImGui::Button(TR("Attach agent"))) {
        const std::string line = attachAgent(scene, selected, m_to);
        VK_LOG_INFO("%s", line.c_str());
        m_status = line;
    }
    tagItem("NAV-AGENT-ATTACH");
    ImGui::SameLine();
    if (ImGui::Button(TR("Stop"))) {
        const std::string line = stopAgent(scene, sys, selected);
        VK_LOG_INFO("%s", line.c_str());
        m_status = line;
    }
    tagItem("NAV-AGENT-STOP");

    if (auto* a = selected.valid() ? scene.world().get<ecs::NavAgentComponent>(selected) : nullptr) {
        ImGui::PushItemWidth(230.0f);
        ImGui::DragFloat(lbl(TR("Speed"), "navagentspeed").c_str(), &a->speed, 0.05f, 0.0f, 40.0f);
        tagItem("NAV-AGENT-SPEED");
        ImGui::DragFloat(lbl(TR("Arrive dist"), "navagentarrive").c_str(), &a->arriveDist, 0.01f,
                         0.01f, 2.0f);
        tagItem("NAV-AGENT-ARRIVE");
        ImGui::DragFloat(lbl(TR("Repath interval"), "navagentrepath").c_str(), &a->repathInterval,
                         0.02f, 0.0f, 5.0f);
        tagItem("NAV-AGENT-REPATH");
        ImGui::PopItemWidth();
        ImGui::Checkbox(TR("Face move direction"), &a->faceMoveDir);
        tagItem("NAV-AGENT-FACE");
        ImGui::Checkbox(TR("Smooth path"), &a->smooth);
        tagItem("NAV-AGENT-SMOOTH");
        ImGui::Checkbox(TR("Auto start"), &a->autoStart);
        tagItem("NAV-AGENT-AUTOSTART");
    }
}

} // namespace editor
