#include "TablePanels.h"

#include "Localization.h"
#include "PanelDock.h"
#include "PanelRegistry.h"
#include "DebugRects.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace editor {

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// 显示名 + "##锚点"。⚠ `tag` 必须自带 "##"（与 ProjectSettingsPanel 同）。
std::string idLabel(const char* shown, const char* tag) {
    return std::string(shown) + tag;
}

bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    const size_t n = needle.size();
    if (hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < n; ++k) {
            const unsigned char a = static_cast<unsigned char>(hay[i + k]);
            const unsigned char b = static_cast<unsigned char>(needle[k]);
            const char la = static_cast<char>(std::tolower(a));
            const char lb = static_cast<char>(std::tolower(b));
            if (la != lb) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

} // namespace

// ================================================================ 配表

void DataTablePanel::ensureLoaded() {
    if (m_loaded) return;
    m_loaded = true;
    m_dir = assets::resolveAssetPath("data");
    std::string err;
    project::TableStore::get().loadAll(m_dir, &err);
    if (!err.empty()) m_status = err;
}

const std::vector<project::DataTable>& DataTablePanel::tables() const {
    return project::TableStore::get().all();
}

bool DataTablePanel::selectTable(const std::string& name) {
    ensureLoaded();
    const auto& all = project::TableStore::get().all();
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].name == name) {
            m_sel = static_cast<int>(i);
            VK_LOG_INFO("TABLE-UI select name=%s idx=%d", name.c_str(), m_sel);
            return true;
        }
    }
    return false;
}

bool DataTablePanel::saveCurrent(std::string* msg) {
    ensureLoaded();
    auto& all = project::TableStore::get().allMutable();
    if (m_sel < 0 || m_sel >= static_cast<int>(all.size())) {
        if (msg) *msg = TR("No table selected.");
        return false;
    }
    std::string err;
    if (!project::TableStore::get().save(all[m_sel], &err)) {
        m_status = err;
        if (msg) *msg = err;
        VK_LOG_WARN("TABLE-UI save-failed %s", err.c_str());
        return false;
    }
    m_status = TR("Saved.");
    VK_LOG_INFO("TABLE-UI save name=%s rows=%zu cols=%zu",
                all[m_sel].name.c_str(), all[m_sel].rows(),
                all[m_sel].cols());
    if (msg) *msg = m_status;
    return true;
}

bool DataTablePanel::reloadAll(std::string* msg) {
    m_loaded = false;
    ensureLoaded();
    m_status = TR("Reloaded.");
    if (msg) *msg = m_status;
    return true;
}

bool DataTablePanel::setCell(int row, int col, const std::string& value,
                             std::string* msg) {
    ensureLoaded();
    auto& all = project::TableStore::get().allMutable();
    if (m_sel < 0 || m_sel >= static_cast<int>(all.size())) return false;
    auto& t = all[m_sel];
    if (row < 0 || static_cast<size_t>(row) >= t.data.rows.size()) return false;
    if (col < 0 || static_cast<size_t>(col) >= t.data.header.size()) return false;
    auto& cells = t.data.rows[static_cast<size_t>(row)];
    if (cells.size() < t.data.header.size()) cells.resize(t.data.header.size());
    cells[static_cast<size_t>(col)] = value;
    VK_LOG_INFO("TABLE-UI set table=%s row=%d col=%d value=%s",
                t.name.c_str(), row, col, value.c_str());
    if (msg) *msg = TR("Changed (not saved yet).");
    return true;
}

std::string DataTablePanel::cellText(int row, int col) const {
    const auto& all = project::TableStore::get().all();
    if (m_sel < 0 || m_sel >= static_cast<int>(all.size())) return {};
    return all[m_sel].raw(static_cast<size_t>(row), col);
}

void DataTablePanel::draw() {
    bool& open = panelOpenRef("datatable");
    if (!open) return;
    ensureLoaded();

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 300.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(820.0f, 520.0f), ImGuiCond_FirstUseEver);
    }
    std::string title = TR("Data tables");
    title += "###panel_datatable";
    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    ui::logRect("DT-RECT", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    ImGui::TextWrapped("%s", m_dir.c_str());
    tagItem("DT-PATH");

    auto& all = project::TableStore::get().allMutable();
    if (all.empty()) {
        // ⚠ 空表集合**不是错误**：新工程还没配表。文案要说清"放哪"
        //   而不是"没有表"。
        ImGui::TextDisabled("%s", TR("No .csv in assets/data yet."));
        tagItem("DT-EMPTY");
        ImGui::TextWrapped("%s",
                           TR("Put a .csv there (first row = column names). "
                              "No type row: types come from the accessor."));
    } else {
        if (m_sel >= static_cast<int>(all.size())) m_sel = 0;
        ImGui::SetNextItemWidth(240.0f);
        if (ImGui::BeginCombo(idLabel("##tbl", "##DT-SELECT").c_str(),
                              all[static_cast<size_t>(m_sel)].name.c_str())) {
            for (int i = 0; i < static_cast<int>(all.size()); ++i) {
                if (ImGui::Selectable(all[static_cast<size_t>(i)].name.c_str(),
                                      i == m_sel)) {
                    m_sel = i;
                }
            }
            ImGui::EndCombo();
        }
        tagItem("DT-SELECT");
        ImGui::SameLine();
        if (ImGui::Button(idLabel(TR("Reload"), "##DT-RELOAD").c_str()))
            reloadAll(nullptr);
        tagItem("DT-RELOAD");
        ImGui::SameLine();
        if (ImGui::Button(idLabel(TR("Save"), "##DT-SAVE").c_str()))
            saveCurrent(nullptr);
        tagItem("DT-SAVE");

        auto& t = all[static_cast<size_t>(m_sel)];
        ImGui::SameLine();
        ImGui::TextDisabled(TR("%zu rows x %zu cols"), t.rows(), t.cols());

        ImGui::Separator();
        if (ImGui::BeginTable("##dtgrid", static_cast<int>(t.cols()),
                              ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 260.0f))) {
            for (size_t c = 0; c < t.data.header.size(); ++c) {
                // 注释列（`#` 前缀）加暗 —— 它们不参与取值，视觉上要区分开
                const bool note = !t.data.header[c].empty() &&
                                  t.data.header[c][0] == '#';
                if (note) ImGui::PushStyleColor(ImGuiCol_Text,
                                                ImVec4(0.6f, 0.6f, 0.65f, 1.0f));
                ImGui::TableSetupColumn(t.data.header[c].c_str());
                if (note) ImGui::PopStyleColor();
            }
            ImGui::TableHeadersRow();

            for (size_t r = 0; r < t.data.rows.size(); ++r) {
                ImGui::TableNextRow();
                for (size_t c = 0; c < t.data.header.size(); ++c) {
                    ImGui::TableNextColumn();
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s",
                                  t.data.rows[r][c].c_str());
                    ImGui::PushID(static_cast<int>(r * 1000 + c));
                    ImGui::SetNextItemWidth(160.0f);
                    // ⚠ 每帧从模型拷一份到临时 buffer 是**对的**：ImGui 在
                    //   编辑期间自己维护状态，返回 true 时才回写。
                    if (ImGui::InputText("##v", buf, sizeof(buf))) {
                        t.data.rows[r][c] = buf;
                        m_status = TR("Changed (not saved yet).");
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        tagItem("DT-GRID");
    }

    if (!m_status.empty()) ImGui::TextWrapped("%s", m_status.c_str());
    tagItem("DT-STATUS");
    ImGui::End();
}

// ================================================================ 本地化

void LocalizationPanel::ensureLoaded() {
    if (m_loaded) return;
    m_loaded = true;
    std::string err;
    // `i18n/zh.csv` 而不是 `i18n/zh-CN.csv`：文件名与 `languageCode()` 对齐
    // 会让"加一种语言"变成"改一处枚举 + 加一个文件"，不再需要分支。
    if (!m_ed.load(assets::resolveAssetPath("i18n/zh.csv"), &err)) {
        m_status = err;
        VK_LOG_WARN("I18N-UI load-failed %s", err.c_str());
    }
}

bool LocalizationPanel::save(std::string* msg) {
    ensureLoaded();
    std::string err;
    if (!m_ed.save(&err)) {
        m_status = err;
        if (msg) *msg = err;
        return false;
    }
    m_status = TR("Saved.");
    if (msg) *msg = m_status;
    return true;
}

bool LocalizationPanel::reload(std::string* msg) {
    m_loaded = false;
    ensureLoaded();
    m_status = TR("Reloaded.");
    if (msg) *msg = m_status;
    return true;
}

bool LocalizationPanel::setRow(int index, const std::string& zh,
                               std::string* msg) {
    ensureLoaded();
    if (index < 0 || static_cast<size_t>(index) >= m_ed.count()) return false;
    m_ed.set(m_ed.row(static_cast<size_t>(index)).en, zh);
    VK_LOG_INFO("I18N-UI set idx=%d zh=%s", index, zh.c_str());
    if (msg) *msg = TR("Changed (not saved yet).");
    return true;
}

std::string LocalizationPanel::valueOf(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= m_ed.count()) return {};
    return m_ed.row(static_cast<size_t>(index)).zh;
}

void LocalizationPanel::draw() {
    bool& open = panelOpenRef("localization");
    if (!open) return;
    ensureLoaded();

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 300.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f), ImGuiCond_FirstUseEver);
    }
    std::string title = TR("Localization");
    title += "###panel_localization";
    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    ui::logRect("LOC-RECT", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    ImGui::TextWrapped("%s", m_ed.path().c_str());
    tagItem("LOC-PATH");

    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint(idLabel("##f", "##LOC-FILTER").c_str(),
                             TR("filter (英文原文或译文)"), m_filter,
                             sizeof(m_filter));
    tagItem("LOC-FILTER");
    ImGui::SameLine();
    ImGui::Checkbox(idLabel(TR("Only missing"), "##LOC-ONLYMISS").c_str(),
                    &m_onlyMissing);
    tagItem("LOC-ONLYMISS");
    ImGui::SameLine();
    ImGui::Checkbox(idLabel(TR("Only changed"), "##LOC-ONLYDIRTY").c_str(),
                    &m_onlyDirty);
    tagItem("LOC-ONLYDIRTY");

    if (ImGui::Button(idLabel(TR("Save"), "##LOC-SAVE").c_str())) save(nullptr);
    tagItem("LOC-SAVE");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Reload"), "##LOC-RELOAD").c_str()))
        reload(nullptr);
    tagItem("LOC-RELOAD");

    // 三个计数必须**一直**显示：它们是"还剩多少活"的唯一读数。
    ImGui::SameLine();
    ImGui::TextDisabled(TR("%zu keys, %zu missing, %zu changed"),
                        m_ed.count(), m_ed.missingCount(),
                        m_ed.dirtyCount());
    tagItem("LOC-STATUS");

    ImGui::Separator();

    const std::string f = m_filter;
    m_view.clear();
    m_view.reserve(m_ed.count());
    for (size_t i = 0; i < m_ed.count(); ++i) {
        const auto& r = m_ed.row(i);
        if (m_onlyMissing && !r.zh.empty()) continue;
        if (m_onlyDirty && !r.dirty) continue;
        if (!f.empty() && !icontains(r.en, f) && !icontains(r.zh, f)) continue;
        m_view.push_back(static_cast<int>(i));
    }

    if (ImGui::BeginTable("##loclist", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0.0f, 300.0f))) {
        ImGui::TableSetupColumn(TR("English (key)"));
        ImGui::TableSetupColumn(TR("Translation"));
        ImGui::TableHeadersRow();
        for (int idx : m_view) {
            const auto& r = m_ed.row(static_cast<size_t>(idx));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // 缺译文的行标黄 —— 一屏几十行里"哪几行还是空的"要能扫出来
            if (r.zh.empty())
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), "%s",
                                   r.en.c_str());
            else
                ImGui::TextUnformatted(r.en.c_str());
            ImGui::TableNextColumn();
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", r.zh.c_str());
            ImGui::PushID(idx);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##zh", buf, sizeof(buf))) {
                m_ed.set(r.en, buf);
                m_status = TR("Changed (not saved yet).");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    tagItem("LOC-LIST");
    ImGui::TextDisabled(TR("%zu shown / %zu total"),
                        m_view.size(), m_ed.count());
    if (!m_status.empty()) ImGui::TextWrapped("%s", m_status.c_str());
    tagItem("LOC-NOTE");
    ImGui::End();
}

} // namespace editor
