#include "ScriptDebugPanel.h"

#include "script/ScriptEngine.h"

#include "DebugRects.h"
#include "Localization.h"
#include "PanelDock.h"
#include "PanelRegistry.h"

#include "core/Logger.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace editor {

namespace {

void tagItem(const char* tag) {
    ui::logRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// 显示名 + "##锚点"。⚠ `tag` 必须自带 "##"（与 TablePanels 同）。
std::string idLabel(const char* shown, const char* tag) {
    return std::string(shown) + tag;
}

bool hasLine(const std::vector<int>& lines, int want) {
    for (int l : lines)
        if (l == want) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------- 列表

void ScriptDebugPanel::refreshPaths() {
    if (!m_rt) {
        m_paths.clear();
        return;
    }
    m_paths = m_rt->engine().loadedPaths();
    // 选择按路径记 → 列表变了不会"选到别人身上"。没选过就取第一个。
    if (m_selPath.empty() && !m_paths.empty()) m_selPath = m_paths.front();
    // 选中的那个被 clear() 掉了 → 回落到第一个
    bool found = false;
    for (const std::string& p : m_paths)
        if (p == m_selPath) found = true;
    if (!found) m_selPath = m_paths.empty() ? std::string() : m_paths.front();
}

bool ScriptDebugPanel::selectPath(const std::string& path) {
    if (!m_rt) return false;
    for (const std::string& p : m_rt->engine().loadedPaths())
        if (p == path) {
            m_selPath = path;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------- 命令

bool ScriptDebugPanel::addBreakpoint(const std::string& path, int line,
                                     std::string* msg) {
    if (!m_rt) {
        if (msg) *msg = "no script runtime";
        return false;
    }
    std::string err;
    if (!m_rt->addBreakpoint(path, line, &err)) {
        m_status = err;
        if (msg) *msg = err;
        VK_LOG_WARN("SCRDBG-BP rejected path=%s line=%d err=%s", path.c_str(),
                    line, err.c_str());
        return false;
    }
    m_status = TR("Breakpoint added.");
    if (msg) *msg = m_status;
    return true;
}

int ScriptDebugPanel::removeBreakpoint(const std::string& path, int line) {
    return m_rt ? m_rt->removeBreakpoint(path, line) : 0;
}

int ScriptDebugPanel::clearBreakpoints() {
    const int n = m_rt ? m_rt->clearBreakpoints() : 0;
    if (n > 0) m_status = TR("Breakpoints cleared.");
    return n;
}

int ScriptDebugPanel::resumeAll() {
    const int n = m_rt ? m_rt->resumeAll() : 0;
    if (n > 0) m_status = TR("Resumed.");
    return n;
}

int ScriptDebugPanel::stepAll() {
    const int n = m_rt ? m_rt->stepAll() : 0;
    if (n > 0) m_status = TR("Stepped.");
    return n;
}

std::string ScriptDebugPanel::stateLine() const {
    if (!m_rt) return "paused=0 runtime=0";
    const auto list = m_rt->pausedInstances();
    char buf[512];
    if (list.empty()) {
        // i18n-keep：`SCRDBG-STATE` 的解析用字符串（脚本按 paused= 取值）。
        std::snprintf(buf, sizeof(buf), "paused=0 breakpoints=%zu",
                      m_rt->breakpoints().size());
        return std::string(buf);
    }
    const vks::ScriptInstance* p = list.front();
    // i18n-keep（D 组）：同上。
    std::snprintf(buf, sizeof(buf),
                  "paused=%zu breakpoints=%zu entity=%u path=%s line=%d",
                  list.size(), m_rt->breakpoints().size(), p->entity.id,
                  p->path.c_str(), p->pauseLine);
    return std::string(buf);
}

std::vector<std::string> ScriptDebugPanel::pausedLines() const {
    std::vector<std::string> out;
    if (!m_rt) return out;
    for (const vks::ScriptInstance* p : m_rt->pausedInstances()) {
        char buf[384];
        // i18n-keep（D 组）：同上，这是 `SCR-AT-ROW` 的转储。
        std::snprintf(buf, sizeof(buf), "entity=%u path=%s line=%d",
                      p->entity.id, p->path.c_str(), p->pauseLine);
        out.emplace_back(buf);
    }
    return out;
}

std::vector<std::string> ScriptDebugPanel::pausedVarLines() const {
    std::vector<std::string> out;
    if (!m_rt) return out;
    const auto list = m_rt->pausedInstances();
    if (list.empty()) return out;
    // 只报**第一个**暂停实例的变量：面板同一时刻也只显示一个，
    // 而控制台里"三个实例的 vars 混在一起"读不出东西。
    for (const auto& kv : m_rt->sortedVars(*list.front())) {
        out.push_back(kv.first + "=" + kv.second);
    }
    return out;
}

// ---------------------------------------------------------------- 绘制

void ScriptDebugPanel::draw() {
    bool& open = panelOpenRef("scriptdbg");
    if (!open) return;
    refreshPaths();

    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 260.0f),
                                            ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowSize(ImVec2(760.0f, 540.0f), ImGuiCond_FirstUseEver);
    }
    std::string title = TR("Script debug");
    title += "###panel_scriptdbg";
    if (!ImGui::Begin(title.c_str(), docked ? nullptr : &open, wf)) {
        ImGui::End();
        return;
    }
    ui::logRect("SCRDBG-RECT", ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    if (!m_rt) {
        ImGui::TextDisabled("%s", TR("No script runtime."));
        ImGui::End();
        return;
    }

    const auto paused = m_rt->pausedInstances();
    const std::size_t bpCount = m_rt->breakpoints().size();

    // ---- 顶栏：状态 + 三个动词 ----
    if (paused.empty()) {
        ImGui::TextDisabled("%s", TR("Running (no breakpoint hit)."));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "%s",
                           TR("Paused at line %d of %s"),
                           paused.front()->pauseLine,
                           paused.front()->path.c_str());
    }
    tagItem("SCRDBG-STATE");

    ImGui::BeginDisabled(paused.empty());
    if (ImGui::Button(idLabel(TR("Continue"), "##SCRDBG-CONT").c_str()))
        resumeAll();
    tagItem("SCRDBG-CONT");
    ImGui::SameLine();
    if (ImGui::Button(idLabel(TR("Step"), "##SCRDBG-STEP").c_str()))
        stepAll();
    tagItem("SCRDBG-STEP");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(bpCount == 0);
    if (ImGui::Button(idLabel(TR("Clear breakpoints"), "##SCRDBG-CLEARBP")
                          .c_str()))
        clearBreakpoints();
    ImGui::EndDisabled();
    tagItem("SCRDBG-CLEARBP");
    ImGui::SameLine();
    ImGui::TextDisabled(TR("%zu breakpoint(s)"), bpCount);
    tagItem("SCRDBG-BPCOUNT");

    // ---- 选脚本 ----
    if (m_paths.empty()) {
        ImGui::TextDisabled("%s",
                            TR("No script loaded yet (enter Play first)."));
    } else {
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::BeginCombo(idLabel("##src", "##SCRDBG-PATH").c_str(),
                              m_selPath.c_str())) {
            for (const std::string& p : m_paths) {
                if (ImGui::Selectable(p.c_str(), p == m_selPath)) m_selPath = p;
            }
            ImGui::EndCombo();
        }
        tagItem("SCRDBG-PATH");
        ImGui::SameLine();
        ImGui::Checkbox(idLabel(TR("Variables"), "##SCRDBG-VARCHK").c_str(),
                        &m_showVars);
    }

    ImGui::Separator();

    // ---- 源码（点行 = 加/去断点）----
    vks::ScriptAsset* asset =
        m_selPath.empty() ? nullptr : m_rt->engine().get(m_selPath);
    vks::ScriptProgram* prog = asset ? asset->program() : nullptr;
    if (!prog || !prog->valid()) {
        ImGui::TextDisabled("%s", TR("This script did not compile."));
        if (asset && !asset->lastError().toString().empty())
            ImGui::TextWrapped("%s", asset->lastError().toString().c_str());
    } else {
        const std::vector<int>& exec = prog->executableLines();
        const std::set<int>* bps = m_rt->breakpoints().linesOf(m_selPath);
        // 暂停行：只标"当前脚本"的那一个（别的脚本的暂停行在别的文件里）
        int pauseLine = 0;
        for (const vks::ScriptInstance* p : paused)
            if (p->path == m_selPath) pauseLine = p->pauseLine;

        // ⚠ 源码按行切：`\r\n` 与 `\n` 都要吃掉。少吃掉 `\r` 的话
        //   每行末尾多一个不可见字符，而**行号仍然对** —— 于是这个 bug
        //   只会表现成"某行看起来有怪字符"，查起来极其费时。
        std::vector<std::string> lines;
        {
            const std::string& src = prog->source();
            std::string cur;
            for (char c : src) {
                if (c == '\n') {
                    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                    lines.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) lines.push_back(cur);
        }

        if (ImGui::BeginChild("##srcview", ImVec2(0.0f, 260.0f), true)) {
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const int ln = static_cast<int>(i) + 1;
                const bool canBreak = hasLine(exec, ln);
                const bool isBp = bps && bps->find(ln) != bps->end();
                const bool isPause = (ln == pauseLine);

                char label[512];
                std::snprintf(label, sizeof(label), "%s %4d  %s",
                              isPause ? ">" : (isBp ? "*" : " "), ln,
                              lines[i].c_str());

                if (!canBreak) {
                    // 空行 / 注释行 / 纯括号行：画出来但不给点 ——
                    // 在它们上面点断点**永远不会命中**，比不给点更糟。
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(0.55f, 0.55f, 0.6f, 1.0f));
                    ImGui::TextUnformatted(label);
                    ImGui::PopStyleColor();
                    continue;
                }
                if (isPause)
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(1.0f, 0.78f, 0.32f, 1.0f));
                else if (isBp)
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                ImGui::PushID(ln);
                if (ImGui::Selectable(label)) {
                    if (isBp)
                        m_rt->removeBreakpoint(m_selPath, ln);
                    else
                        addBreakpoint(m_selPath, ln, nullptr);
                }
                ImGui::PopID();
                if (isPause || isBp) ImGui::PopStyleColor();
                // 锚点打两套：
                //   `SCRDBG-LINE`      —— 只留**第一条**可打断行（`logRect`
                //                        按 tag 去重）。给"随便点一行看看"
                //                        的老用例用，别删。
                //   `SCRDBG-LINE-<行号>` —— 每一行一条。
                // ⚠ 为什么必须有第二套：`logRect` 的去重让自动化只能点到
                //   **第一**可打断行，而第一行往往是 `let x = 0`（顶层相位，
                //   只跑一次）—— 想在 Play 里验证"这个断点真的会停"就必须
                //   点得到 `on update` 里的那一行。只有一套 tag 时，验收只能
                //   验到"加了一条断点"，验不到"那条断点有用"。
                tagItem("SCRDBG-LINE");
                {
                    char lineTag[40];
                    // i18n-keep：`logRect` 锚点名（SCRDBG-LINE-<行号>）。
                    std::snprintf(lineTag, sizeof(lineTag), "SCRDBG-LINE-%d", ln);
                    tagItem(lineTag);
                }
            }
        }
        ImGui::EndChild();
        tagItem("SCRDBG-SRC");
        ImGui::TextDisabled("%s",
                            TR("Click a line to toggle a breakpoint."));
    }

    // ---- 暂停时的 vars ----
    if (!paused.empty() && m_showVars) {
        ImGui::Separator();
        ImGui::TextDisabled(TR("Variables of entity %u"), paused.front()->entity.id);
        if (ImGui::BeginChild("##vars", ImVec2(0.0f, 140.0f), true)) {
            for (const auto& kv : m_rt->sortedVars(*paused.front())) {
                // `print` 出来的临时量也在这张表里 —— 与 Scripts 面板同源，
                // 用同一个 `sortedVars()` 才不会出现"两边数不一样"。
                ImGui::Text("%s = %s", kv.first.c_str(), kv.second.c_str());
            }
        }
        ImGui::EndChild();
        tagItem("SCRDBG-VARS");
    }

    if (!m_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_status.c_str());
        tagItem("SCRDBG-STATUS");
    }

    if (paused.size() > 1) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", TR("Other paused instances:"));
        for (std::size_t i = 1; i < paused.size(); ++i) {
            ImGui::BulletText(TR("entity %u  %s:%d"), paused[i]->entity.id,
                              paused[i]->path.c_str(), paused[i]->pauseLine);
        }
        tagItem("SCRDBG-PAUSED");
    }

    ImGui::End();
}

} // namespace editor
