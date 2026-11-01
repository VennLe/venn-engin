// ============================================================
// editor/SaveSlotsPanel.cpp —— 存档槽位面板（见 .h 的三条纪律）
// ============================================================

#include "SaveSlotsPanel.h"

#include "DebugRects.h"
#include "Localization.h"
#include "PanelDock.h"
#include "PanelRegistry.h"

#include "core/Logger.h"

#include "scene/Scene.h"
#include "scene/SaveSlot.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace editor {

namespace {

// 槽位摘要的一行文字。**"坏档"必须能显示出来**（见 .h 的纪律 ①）。
std::string slotSummary(const scene::SaveSlotInfo& i) {
    if (!i.exists) return tr("empty");
    if (i.timestamp.empty()) return tr("unreadable (bad file)");
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s  |  %s  |  %d %s  |  %.1f s",
                  i.timestamp.c_str(), i.scenePath.c_str(), i.entities,
                  tr("entities"), i.playTime);
    return std::string(buf);
}

} // namespace

void SaveSlotsPanel::doSave(const std::string& slot) {
    // ⚠⚠ 基线**必须保持为空**（= `captureDiff` 走全量分支）。
    //
    //   引擎里"基线"的语义是"**关卡加载完成**那一刻的状态"，增量是相对
    //   关卡算的。编辑器这个面板不是运行时会话，它在作者说"存"的时候
    //   该做的是**把整个关卡快照进槽位** —— 所以每次都传一份空基线。
    //
    //   第一版在这里写了 `if (!m_baselineReady) m_state.captureBaseline(...)`
    //   （"先用当前场景当基线，再算增量"）—— 结果 `captureDiff` 的结论
    //   必然是"什么都没变"，于是存出来的档 `entities: []`：文件合法、
    //   `ok=1`、磁盘上真有、读回来什么都恢复不了。2026-10-31 实测踩到。
    //   `SaveGameState` 的自检（`slot_save_repeat_keeps_entry`）钉的是
    //   "基线是关卡不是上一份档"，这里踩的是它的另一半。
    m_state.clear();
    const scene::SaveToSlotResult r = scene::saveToSlot(
        m_ctx.editorScene(), m_state, m_root, slot, m_ctx.scenePath(), 0.0,
        ecs::kInvalidGuid);
    m_statusOk = r.ok;
    m_status = r.ok ? (std::string(tr("saved: ")) + r.path)
                    : (std::string(tr("save failed: ")) + r.error);
    // 锚点：验收脚本靠 `ok=` / `entities=` 判"真的写了"，靠 `path=` 判"写对了地方"。
    VK_LOG_INFO("SAVEPANEL op=save slot=%s ok=%d entities=%d path=%s",
                scene::normalizeSaveSlot(slot).c_str(), r.ok ? 1 : 0,
                r.entities, r.path.c_str());
}

void SaveSlotsPanel::doLoad(const std::string& slot) {
    const scene::LoadFromSlotResult r = scene::loadFromSlot(
        m_ctx.editorScene(), m_state, m_ctx.assets(), m_root, slot);
    m_statusOk = r.ok;
    if (r.ok) {
        // 读档改了场景 → 必须置脏，否则关窗时不会提示保存，一关就丢。
        m_ctx.dirty() = true;
        m_status = std::string(tr("loaded: ")) + slot;
    } else {
        m_status = std::string(tr("load failed: ")) + r.error;
    }
    VK_LOG_INFO("SAVEPANEL op=load slot=%s ok=%d applied=%d destroyed=%d "
                "missing=%d drift=%d",
                scene::normalizeSaveSlot(slot).c_str(), r.ok ? 1 : 0,
                r.stats.applied, r.stats.destroyed, r.stats.missing,
                r.stats.driftRejected ? 1 : 0);
}

void SaveSlotsPanel::doDelete(const std::string& slot) {
    const std::string path = scene::saveSlotPath(m_root, slot);
    std::error_code ec;
    const bool existed =
        !path.empty() && std::filesystem::exists(std::filesystem::u8path(path), ec);
    bool removed = false;
    if (existed) {
        removed = std::filesystem::remove(std::filesystem::u8path(path), ec);
    }
    m_statusOk = removed || !existed;
    if (removed) m_status = std::string(tr("deleted: ")) + slot;
    else if (!existed) m_status = std::string(tr("nothing to delete: ")) + slot;
    else m_status = std::string(tr("delete failed: ")) + ec.message();
    // 删档之后把基线也清掉（现在 `doSave` 每轮都清，这里是幂等的保险）：
    // 万一将来有人把"别处抓的基线"塞进 `m_state`，删完不重置就会存出空档。
    m_state.clear();
    VK_LOG_INFO("SAVEPANEL op=delete slot=%s ok=%d existed=%d path=%s",
                scene::normalizeSaveSlot(slot).c_str(), m_statusOk ? 1 : 0,
                existed ? 1 : 0, path.c_str());
}

void SaveSlotsPanel::draw() {
    bool& open = panelOpenRef("save");
    if (!open) return;

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowSize(ImVec2(520.0f, 380.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.55f,
                                       io.DisplaySize.y * 0.30f),
                                ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin(trId("Save slots", "panel_save"),
                      docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    logRect("SAVEPANEL-WIN", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    const std::vector<scene::SaveSlotInfo> slots =
        scene::listSaveSlots(m_root);

    // 下拉：五个白名单槽位全列（空槽也列 —— "哪些还能用"本身是信息）。
    if (m_selected < 0 || m_selected >= static_cast<int>(slots.size()))
        m_selected = 0;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("###slot",
                          slots.empty() ? "" : slots[m_selected].slot.c_str())) {
        for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
            const bool sel = (i == m_selected);
            if (ImGui::Selectable(slots[i].slot.c_str(), sel)) m_selected = i;
        }
        ImGui::EndCombo();
    }
    logRect("SAVEPANEL-COMBO", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    ImGui::SameLine();
    ImGui::TextDisabled("%s %s", tr("dir:"),
                        m_root.empty() ? scene::kSaveDirDefault : m_root.c_str());

    // ---- 槽位清单 ----
    if (ImGui::BeginTable("###slots", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("info", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // 选中那一行高亮：下拉与清单是同一个下标，两处必须同步 ——
            // 只高亮清单不联动下拉的话，"我点了 star1 但存到 quick 了"。
            if (ImGui::Selectable(slots[i].slot.c_str(), i == m_selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                m_selected = i;
            }
            ImGui::TableNextColumn();
            const bool bad = slots[i].exists && slots[i].timestamp.empty();
            if (bad) ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.95f, 0.55f, 0.5f, 1.0f));
            ImGui::TextUnformatted(slotSummary(slots[i]).c_str());
            if (bad) ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    const std::string cur =
        slots.empty() ? std::string() : slots[static_cast<size_t>(m_selected)].slot;

    if (ImGui::Button(tr(TRL("Save")))) doSave(cur);
    logRect("SAVEPANEL-SAVE", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    // ⚠ "读"在空槽上是可点的：点了会给出明确的"没有这个档"，这比灰掉更好
    //   —— 灰掉之后用户不知道是"不能读"还是"没这个档"。
    if (ImGui::Button(tr(TRL("Load")))) doLoad(cur);
    logRect("SAVEPANEL-LOAD", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::SameLine();
    if (ImGui::Button(tr(TRL("Delete")))) {
        m_confirmDelete = m_selected;
        // ⚠ `OpenPopup` **只能在这里**（点下的那一帧）调。
        //   写成"每帧 `if (m_confirmDelete >= 0) OpenPopup(...)`"的话，
        //   弹窗被 Cancel 关掉的**下一帧**又会重开 —— 症状是"删不掉、也
        //   取消不掉，那个框永远赖在屏幕上"。
        // ⚠⚠ 而且这个字符串**必须**与下面的 `BeginPopupModal` 逐字一致：
        //   原来是 `"###confirm_delete"`，而下面传的是
        //   `trId("Delete this save?", "dlg_delete")`（ID = `dlg_delete`）——
        //   两个不同的 ID，于是 `BeginPopupModal` 永远返回 false：点了
        //   Delete、画面什么都不变、日志也没有一条错。2026-10-31 实测
        //   （Hierarchy 的 `Select by GUID…` 是同一个坑，同一轮逮到两次）。
        ImGui::OpenPopup(trId("Delete this save?", "dlg_delete"));
    }
    logRect("SAVEPANEL-DELETE", ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax());

    // ---- 删除二次确认（本面板唯一不可撤销的动作）----
    if (ImGui::BeginPopupModal(trId("Delete this save?", "dlg_delete"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_confirmDelete >= 0 &&
            m_confirmDelete < static_cast<int>(slots.size())) {
            ImGui::TextUnformatted(slots[static_cast<size_t>(m_confirmDelete)]
                                       .slot.c_str());
        }
        ImGui::TextDisabled("%s", tr("This cannot be undone."));
        if (ImGui::Button(tr(TRL("Delete")), ImVec2(120, 0))) {
            if (m_confirmDelete >= 0 &&
                m_confirmDelete < static_cast<int>(slots.size())) {
                doDelete(slots[static_cast<size_t>(m_confirmDelete)].slot);
            }
            m_confirmDelete = -1;
            ImGui::CloseCurrentPopup();
        }
        logRect("SAVEPANEL-CONFIRM-DEL", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::SameLine();
        if (ImGui::Button(tr(TRL("Cancel")), ImVec2(120, 0))) {
            m_confirmDelete = -1;
            ImGui::CloseCurrentPopup();
        }
        logRect("SAVEPANEL-CONFIRM-CANCEL", ImGui::GetItemRectMin(),
                ImGui::GetItemRectMax());
        ImGui::EndPopup();
    }

    if (!m_status.empty()) {
        ImGui::Separator();
        if (m_statusOk) {
            ImGui::TextColored(ImVec4(0.52f, 0.82f, 0.55f, 1.0f), "%s",
                               m_status.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.5f, 1.0f), "%s",
                               m_status.c_str());
        }
    }

    ImGui::End();
}

} // namespace editor
