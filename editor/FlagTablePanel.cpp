#include "FlagTablePanel.h"

#include "DebugRects.h"
#include "Localization.h"
#include "PanelDock.h"
#include "PanelRegistry.h"

#include "core/Logger.h"

#include <imgui.h>

#include <cctype>
#include <cstdio>
#include <cstring>

namespace editor {

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// 显示名 + "##锚点"。⚠ `tag` 必须自带 "##" —— 否则 ImGui 会把锚点名
// 当成可见标题的一部分显示出来（与 ProjectSettingsPanel / TablePanels 同）。
std::string idLabel(const char* shown, const char* tag) {
    return std::string(shown) + tag;
}

bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    const std::size_t n = needle.size();
    for (std::size_t i = 0; i + n <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < n; ++k) {
            const auto a = static_cast<unsigned char>(hay[i + k]);
            const auto b = static_cast<unsigned char>(needle[k]);
            if (std::tolower(a) != std::tolower(b)) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------- 查询

bool FlagTablePanel::visible(const std::string& name) const {
    if (!icontains(name, m_filter)) return false;
    if (m_onlyNonZero && m_store && m_store->get(name) == 0.0) return false;
    return true;
}

std::size_t FlagTablePanel::count() const {
    return m_store ? m_store->size() : 0;
}

double FlagTablePanel::get(const std::string& name) const {
    return m_store ? m_store->get(name) : 0.0;
}

std::vector<scene::FlagRow> FlagTablePanel::rows(
    const std::string& filter) const {
    std::vector<scene::FlagRow> out;
    if (!m_store) return out;
    // `GameFlags::rows()` 已按名字排序；这里只做过滤，**不重排** ——
    // 控制台输出的顺序因此与面板表格完全一致。
    for (const scene::FlagRow& r : m_store->rows()) {
        if (!icontains(r.name, filter)) continue;
        if (m_onlyNonZero && r.value == 0.0) continue;
        out.push_back(r);
    }
    return out;
}

std::string FlagTablePanel::rowText(const scene::FlagRow& r) const {
    // 一行一条、字段固定顺序、浮点定宽 —— 自动化按 `name=... value=...`
    // 解析时不会因为"某个值是整数就少一位小数"而对不上。
    char buf[512];
    // i18n-keep：给脚本读的转储行（`flags.list`），不上屏。
    std::snprintf(buf, sizeof(buf), "name=%s value=%.4f kind=%s writer=%s",
                  r.name.c_str(), r.value, scene::flagKindName(r.value),
                  r.writer.empty() ? "-" : r.writer.c_str());
    return std::string(buf);
}

// ---------------------------------------------------------------- 写

bool FlagTablePanel::set(const std::string& name, double v,
                         const std::string& writer) {
    if (!m_store) return false;
    if (name.empty()) return false;
    m_store->set(name, v, writer);
    VK_LOG_INFO("EDITOR-FLAG set name=%s value=%.4f writer=%s total=%zu",
                name.c_str(), v, writer.c_str(), m_store->size());
    return true;
}

bool FlagTablePanel::erase(const std::string& name) {
    if (!m_store || !m_store->has(name)) return false;
    m_store->erase(name);
    VK_LOG_INFO("EDITOR-FLAG erase name=%s total=%zu", name.c_str(),
                m_store->size());
    return true;
}

std::size_t FlagTablePanel::wipe() {
    if (!m_store) return 0;
    const std::size_t n = m_store->size();
    m_store->clear();
    m_status = TR("Cleared all flags.");
    VK_LOG_INFO("EDITOR-FLAG clear removed=%zu", n);
    return n;
}

bool FlagTablePanel::snapshot() {
    if (!m_store) return false;
    m_snap = m_store->snap();
    m_status = TR("Snapshot taken.");
    VK_LOG_INFO("EDITOR-FLAG snap count=%zu", m_store->size());
    return true;
}

std::size_t FlagTablePanel::restore() {
    if (!m_store || !m_snap.valid) return 0;
    const std::size_t n = m_store->restore(m_snap);
    m_status = TR("Restored from snapshot.");
    VK_LOG_INFO("EDITOR-FLAG restore count=%zu", n);
    return n;
}

// ---------------------------------------------------------------- 绘制

void FlagTablePanel::draw() {
    bool& open = panelOpenRef("flags");
    if (!open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 240.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(660.0f, 460.0f), ImGuiCond_FirstUseEver);
    }
    std::string title = TR("Flags");
    title += "###panel_flags";
    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    ui::logRect("FLAG-RECT", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    if (!m_store) {
        ImGui::TextDisabled("%s", TR("No flag store attached."));
        ImGui::End();
        return;
    }

    // ---- 顶栏：过滤 / 计数 / 三个动作 ----
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint(idLabel("##flt", "##FLAG-FILTER").c_str(),
                             TR("filter by name"), m_filter,
                             sizeof(m_filter));
    tagItem("FLAG-FILTER");
    ImGui::SameLine();
    ImGui::Checkbox(idLabel(TR("Non-zero only"), "##FLAG-NONZERO").c_str(),
                    &m_onlyNonZero);
    tagItem("FLAG-NONZERO");

    std::size_t shown = 0;
    for (const scene::FlagRow& r : m_store->rows())
        if (visible(r.name)) ++shown;
    ImGui::SameLine();
    // ⚠ 计数要同时给"全部"与"过滤后"：只给一个数时，"过滤框里打了字、
    //   表变空了"会被当成"旗标全没了"。
    ImGui::TextDisabled(TR("%zu shown / %zu total"), shown, m_store->size());
    tagItem("FLAG-COUNT");

    if (ImGui::Button(idLabel(TR("Snapshot"), "##FLAG-SNAP").c_str()))
        snapshot();
    tagItem("FLAG-SNAP");
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_snap.valid);
    if (ImGui::Button(idLabel(TR("Restore"), "##FLAG-RESTORE").c_str()))
        restore();
    ImGui::EndDisabled();
    tagItem("FLAG-RESTORE");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_store->empty());
    if (ImGui::Button(idLabel(TR("Clear all"), "##FLAG-CLEAR").c_str()))
        wipe();
    ImGui::EndDisabled();
    tagItem("FLAG-CLEAR");

    // 快照提示：没有快照时"恢复"是灰的，但**为什么灰**必须写出来 ——
    // 否则第一次用的人会以为按钮坏了。
    if (m_snap.valid)
        ImGui::TextDisabled(TR("Snapshot holds %zu flag(s)."),
                            m_snap.v.size());

    ImGui::Separator();

    // ---- 表格 ----
    if (m_store->empty()) {
        ImGui::TextDisabled("%s", TR("No flags yet."));
        tagItem("FLAG-EMPTY");
        ImGui::TextWrapped(
            "%s", TR("Flags appear here as soon as a script calls "
                     "setFlag(name, value) during Play."));
    } else if (ImGui::BeginTable("##flaggrid", 4,
                                 ImGuiTableFlags_Borders |
                                     ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_ScrollY,
                                 ImVec2(0.0f, 260.0f))) {
        ImGui::TableSetupColumn(TR("Name"));
        ImGui::TableSetupColumn(TR("Value"));
        ImGui::TableSetupColumn(TR("Kind"));
        ImGui::TableSetupColumn(TR("Written by"));
        ImGui::TableHeadersRow();

        // ⚠ 遍历**快照副本**而不是 `m_store->rows()` 的实时容器：
        //   下面的 `InputDouble` 一旦改值就会 `set()`，边遍历边改容器
        //   （std::map 插入新键不失效迭代器，但 `erase` 会）在这里恰好
        //   是安全的 —— 不过面板里"按下这一行就删这一行"的分支迟早会加，
        //   提前按值拷贝一份，省掉将来那个"删完就崩"。
        const std::vector<scene::FlagRow> view = rows(std::string(m_filter));
        for (std::size_t i = 0; i < view.size(); ++i) {
            const scene::FlagRow& r = view[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(r.name.c_str());

            ImGui::TableNextColumn();
            double v = r.value;
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::InputDouble("##v", &v, 1.0, 10.0, "%.4f",
                                   ImGuiInputTextFlags_EnterReturnsTrue)) {
                // ⚠ writer 写 `editor`：面板改的值不能被误认为"某个脚本
                //   写的"，否则"这个旗标哪来的"这个问题的答案会被污染。
                set(r.name, v, "editor");
                m_status = TR("Edited.");
            }
            if (i == 0) tagItem("FLAG-VALUE");

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", scene::flagKindName(r.value));

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s",
                                r.writer.empty() ? TR("(unknown)")
                                                 : r.writer.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
        tagItem("FLAG-LIST");
    }

    if (!m_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_status.c_str());
        tagItem("FLAG-STATUS");
    }

    ImGui::End();
}

} // namespace editor
