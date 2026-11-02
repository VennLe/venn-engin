#include "LevelPanel.h"

#include "scene/LevelStreamer.h"

#include "DebugRects.h"
#include "Localization.h"

#include <imgui.h>

#include <cstdio>

namespace editor {

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// 显示名 + "##锚点"。⚠ `tag` 必须自带 "##"（与 TablePanels / ScriptDebugPanel 同）。
std::string idLabel(const char* shown, const char* tag) {
    return std::string(shown) + tag;
}

} // namespace

// ---------------------------------------------------------------- 查询

std::size_t LevelPanel::count() const {
    return m_streamer ? m_streamer->slots().size() : 0;
}

std::size_t LevelPanel::loadedCount() const {
    if (!m_streamer) return 0;
    std::size_t n = 0;
    for (const auto& s : m_streamer->slots())
        if (s.loaded) ++n;
    return n;
}

std::vector<std::string> LevelPanel::rowsText() const {
    std::vector<std::string> out;
    if (!m_streamer) return out;
    // i18n-keep：给脚本读的转储行（`level.list`），不上屏。
    char buf[640];
    for (const auto& s : m_streamer->slots()) {
        // i18n-keep：给脚本读的转储行（`level.list`），不上屏。
        std::snprintf(buf, sizeof(buf),
                      "name=%s loaded=%d always=%d entities=%zu path=%s",
                      s.entry.name.c_str(), s.loaded ? 1 : 0,
                      s.entry.alwaysLoaded ? 1 : 0, s.entities.size(),
                      s.entry.path.c_str());
        out.emplace_back(buf);
    }
    return out;
}

std::string LevelPanel::stateLine() const {
    if (!m_streamer) return "levels=0 loaded=0 wanted=0";
    const auto& st = m_streamer->stats();
    const glm::vec3& v = m_streamer->lastViewer();
    char buf[512];
    // i18n-keep：`LVL-STATE` 的解析用字符串。
    std::snprintf(buf, sizeof(buf),
                  "levels=%d loaded=%d wanted=%d entities=%d failed=%d "
                  "viewer=%.1f,%.1f,%.1f",
                  st.levels, st.loaded, st.wanted, st.entities, st.failed,
                  v.x, v.y, v.z);
    return std::string(buf);
}

// ---------------------------------------------------------------- 绘制

void LevelPanel::draw() {
    std::string title = TR("Levels");

    // 固定位置 + 不用 p_open：与其它 Debug 面板同一条规矩（关闭走托盘）。
    ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin((title + "###panel_levels").c_str(), nullptr,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    tagItem("LVL-RECT");

    if (!m_streamer) {
        ImGui::TextDisabled("%s", TR("No level manifest loaded."));
        ImGui::End();
        return;
    }

    const auto& st = m_streamer->stats();

    // ---- ① 概览 ----
    ImGui::TextDisabled(TR("%d level(s), %d loaded, %d always-loaded"),
                        st.levels, st.loaded, st.alwaysLoaded);
    tagItem("LVL-COUNT");

    const glm::vec3& v = m_streamer->lastViewer();
    ImGui::SameLine();
    ImGui::TextDisabled(TR("   viewer (%.1f, %.1f, %.1f)"), v.x, v.y, v.z);
    tagItem("LVL-VIEWER");

    // ---- ③ 动词（放上面：表格很长时不用滚到底才能点）----
    ImGui::BeginDisabled(!m_reload);
    if (ImGui::Button(TR("Reload manifest")))
        if (m_reload) m_reload();
    ImGui::EndDisabled();
    tagItem("LVL-RELOAD");

    ImGui::SameLine();
    // 选中某一行的 Load / Unload：没有选中就禁用（而不是"点了没反应"）
    const bool hasSel = !m_selected.empty();
    ImGui::BeginDisabled(!hasSel || !m_load);
    if (ImGui::Button(idLabel(TR("Load"), "##LVL-LOAD").c_str()))
        if (m_load) m_load(m_selected);
    ImGui::EndDisabled();
    tagItem("LVL-LOAD");

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSel || !m_unload);
    if (ImGui::Button(idLabel(TR("Unload"), "##LVL-UNLOAD").c_str()))
        if (m_unload) m_unload(m_selected);
    ImGui::EndDisabled();
    tagItem("LVL-UNLOAD");

    ImGui::SameLine();
    ImGui::BeginDisabled(st.loaded == 0 || !m_unloadAll);
    if (ImGui::Button(idLabel(TR("Unload all"), "##LVL-UNLOADALL").c_str()))
        if (m_unloadAll) m_unloadAll();
    ImGui::EndDisabled();
    tagItem("LVL-UNLOADALL");

    ImGui::Separator();

    // ---- ② 表格 ----
    if (m_streamer->slots().empty()) {
        ImGui::TextDisabled("%s", TR("This manifest lists no level."));
    } else if (ImGui::BeginTable("##lvlgrid", 5,
                                 ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_ScrollY,
                                 ImVec2(0.0f, 260.0f))) {
        ImGui::TableSetupColumn(TR("Name"));
        ImGui::TableSetupColumn(TR("State"));
        ImGui::TableSetupColumn(TR("Always"));
        ImGui::TableSetupColumn(TR("Entities"));
        ImGui::TableSetupColumn(TR("Path"));
        ImGui::TableHeadersRow();

        for (const auto& s : m_streamer->slots()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(s.entry.name.c_str());
            // 选中 = 后面 Load/Unload 动词的目标。做成 Selectable 而不是
            // 单选按钮：一行占满，点哪儿都算选中。
            if (ImGui::Selectable(s.entry.name.c_str(),
                                  s.entry.name == m_selected,
                                  ImGuiSelectableFlags_SpanAllColumns))
                m_selected = s.entry.name;
            // 锚点打两套，与 `SCRDBG-LINE` / `SCRDBG-LINE-<行号>` 同一理由：
            //   `LVL-LIST`         —— `logRect` 按 tag 去重，只留**一条**，
            //                        而且留哪一条取决于**首帧布局是否已稳定**
            //                        （实测打出来的是最后一行）→ 自动化
            //                        点它等于随机选一张关卡。
            //   `LVL-ROW-<name>`   —— 每行一条，想点哪张就点哪张。
            tagItem("LVL-LIST");
            {
                const std::string rowTag = "LVL-ROW-" + s.entry.name;
                tagItem(rowTag.c_str());
            }

            ImGui::TableNextColumn();
            if (!s.entry.enabled)
                ImGui::TextDisabled("%s", TR("disabled"));
            else if (s.loaded)
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "%s",
                                   TR("loaded"));
            else
                ImGui::TextDisabled("%s", TR("unloaded"));

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", s.entry.alwaysLoaded ? TR("yes") : "-");

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%zu", s.entities.size());

            ImGui::TableNextColumn();
            if (m_showPaths) ImGui::TextDisabled("%s", s.entry.path.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    tagItem("LVL-SRC");

    if (!m_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_status.c_str());
        tagItem("LVL-STATUS");
    }

    ImGui::End();
}

} // namespace editor
