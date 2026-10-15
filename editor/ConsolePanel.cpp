#include "ConsolePanel.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace editor {

namespace {
// sink 是无上下文的函数指针 → 用一个全局"当前面板"指针桥接。
// 编辑器只有一个 ConsolePanel 实例；构造时装上、析构时卸下。
ConsolePanel* g_active = nullptr;

void sinkBridge(core::LogLevel level, const char* msg) {
    if (g_active) g_active->print(level, msg);
}

bool startsWithCI(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}
}  // namespace

ConsolePanel::ConsolePanel() { g_active = this; core::setLogSink(&sinkBridge); }

ConsolePanel::~ConsolePanel() {
    if (g_active == this) {
        core::setLogSink(nullptr);
        g_active = nullptr;
    }
}

void ConsolePanel::addCommand(const char* name, const char* help,
                              Handler run) {
    for (Command& c : m_commands) {
        if (c.name == name) {
            c.help = help;
            c.run = std::move(run);
            return;
        }
    }
    m_commands.push_back(Command{name, help, std::move(run)});
    std::sort(m_commands.begin(), m_commands.end(),
              [](const Command& a, const Command& b) {
                  return a.name < b.name;
              });
}

void ConsolePanel::print(core::LogLevel level, const std::string& text) {
    // 单行一条；带换行的日志拆开（有些 VK_LOG 用 \n 排版）。
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        if (nl > start || start == 0)
            m_lines.push_back(Line{level, text.substr(start, nl - start)});
        if (m_lines.size() > kMaxLines) m_lines.pop_front();
        if (nl == text.size()) break;
        start = nl + 1;
    }
}

size_t ConsolePanel::countByLevel(core::LogLevel lv) const {
    size_t n = 0;
    for (const Line& l : m_lines)
        if (l.level == lv) ++n;
    return n;
}

void ConsolePanel::execute(const std::string& cmdline) {
    // 拆 token（空格 / tab）
    std::vector<std::string> args;
    size_t i = 0;
    while (i < cmdline.size()) {
        while (i < cmdline.size() &&
               std::isspace(static_cast<unsigned char>(cmdline[i])))
            ++i;
        size_t j = i;
        while (j < cmdline.size() &&
               !std::isspace(static_cast<unsigned char>(cmdline[j])))
            ++j;
        if (j > i) args.push_back(cmdline.substr(i, j - i));
        i = j;
    }
    if (args.empty()) return;

    print(core::LogLevel::Info, "> " + cmdline);
    m_history.push_back(cmdline);
    m_historyIdx = -1;

    // 大小写不敏感找命令
    Command* cmd = nullptr;
    for (Command& c : m_commands)
        if (startsWithCI(c.name, args[0]) && c.name.size() == args[0].size()) {
            cmd = &c;
            break;
        }
    if (!cmd) {
        // 前缀唯一的命令也算命中（敲 "cle" 就能 clear）
        Command* partial = nullptr;
        for (Command& c : m_commands)
            if (startsWithCI(c.name, args[0])) {
                if (partial) {
                    print(core::LogLevel::Warn,
                          "ambiguous command '" + args[0] + "'");
                    return;
                }
                partial = &c;
            }
        cmd = partial;
    }
    if (!cmd) {
        print(core::LogLevel::Warn,
              "unknown command '" + args[0] + "' (try 'help')");
        return;
    }
    const std::string out = cmd->run(args);
    if (!out.empty()) print(core::LogLevel::Info, out);
}

void ConsolePanel::clearLocked() {
    m_lines.clear();
}

// E25：外部设置过滤（GPU 时间轴点段 → 看该 pass 的日志）。
// 打一条 CONSOLE-FILTER 锚点，让自动化能断言"联动真的发生了"——否则
// 脚本只能靠"日志条数变少"这种间接证据，容易误判。
void ConsolePanel::setSearchFilter(const std::string& text) {
    std::snprintf(m_search, sizeof(m_search), "%s", text.c_str());
    VK_LOG_INFO("CONSOLE-FILTER text=%s", m_search);
}

void ConsolePanel::draw(bool* open) {
    if (!ImGui::Begin("Console", open)) {
        ImGui::End();
        return;
    }

    // ---- 工具行：级别过滤 + 搜索 + 操作 ----
    ImGui::Checkbox("Info", &m_showInfo);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &m_showWarn);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &m_showError);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-190.0f);
    ImGui::InputTextWithHint("##consearch", "filter...", m_search,
                             sizeof(m_search));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) clearLocked();
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        // 复制**过滤后**的行 —— 用户看到什么就复制什么
        std::string out;
        for (const Line& l : m_lines) {
            const bool lvOk = (l.level == core::LogLevel::Info && m_showInfo) ||
                              (l.level == core::LogLevel::Warn && m_showWarn) ||
                              (l.level == core::LogLevel::Error && m_showError);
            if (!lvOk) continue;
            if (m_search[0] && l.text.find(m_search) == std::string::npos)
                continue;
            out += l.text;
            out += '\n';
        }
        ImGui::SetClipboardText(out.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        std::ofstream f("editor_console.log", std::ios::app);
        for (const Line& l : m_lines)
            f << "[" << core::logLevelTag(l.level) << "] " << l.text << "\n";
        print(core::LogLevel::Info, "saved to editor_console.log (cwd)");
    }

    // ---- 日志区 ----
    ImGui::Separator();
    ImGui::BeginChild("##conlog", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
                      ImGuiChildFlags_Borders);
    // E21：点通知 toast 会请求"滚到最后一条匹配行"。逐行记下命中行的 Y，
    // 循环结束时它就是**最后一条**命中（越后写越覆盖）。
    float scrollHitY = -1.0f;
    for (const Line& l : m_lines) {
        const bool lvOk = (l.level == core::LogLevel::Info && m_showInfo) ||
                          (l.level == core::LogLevel::Warn && m_showWarn) ||
                          (l.level == core::LogLevel::Error && m_showError);
        if (!lvOk) continue;
        if (m_search[0] && l.text.find(m_search) == std::string::npos) continue;
        if (!m_scrollToText.empty() &&
            (l.text.find(m_scrollToText) != std::string::npos ||
             m_scrollToText.find(l.text) != std::string::npos))
            scrollHitY = ImGui::GetCursorPosY();
        switch (l.level) {
            case core::LogLevel::Warn:
                ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f), "%s",
                                   l.text.c_str());
                break;
            case core::LogLevel::Error:
                ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.40f, 1.0f), "%s",
                                   l.text.c_str());
                break;
            default:
                ImGui::TextUnformatted(l.text.c_str());
                break;
        }
    }
    const bool explicitScroll = !m_scrollToText.empty();
    if (explicitScroll) {
        if (scrollHitY >= 0.0f) ImGui::SetScrollY(std::max(0.0f, scrollHitY - 24.0f));
        m_scrollToText.clear();
    }
    // 自动滚动：只有当用户本来就贴着底部时才跟（自己往上翻就不打扰）。
    // ⚠ 显式定位那一帧要压掉自动滚动，否则刚滚过去的行立刻被拖回底部。
    if (!explicitScroll && m_autoscroll &&
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::Checkbox("autoscroll", &m_autoscroll);

    // ---- 命令输入 + 自动补全 ----
    ImGui::Separator();
    // 补全候选：输入非空时列出名字前缀命中的命令
    std::vector<const Command*> candidates;
    if (m_input[0]) {
        for (const Command& c : m_commands)
            if (startsWithCI(c.name, m_input)) candidates.push_back(&c);
        if (candidates.size() > 6) candidates.resize(6);
    }
    if (!candidates.empty()) {
        for (const Command* c : candidates) {
            if (ImGui::Selectable(c->name.c_str())) {
                std::snprintf(m_input, sizeof(m_input), "%s", c->name.c_str());
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", c->help.c_str());
        }
    }
    const bool submit = ImGui::InputText(
        "##cmdline", m_input, sizeof(m_input),
        ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CallbackHistory,
        [](ImGuiInputTextCallbackData* d) -> int {
            // ↑/↓ 翻输入历史（self = 面板指针经 UserData 传入）
            auto* self = static_cast<ConsolePanel*>(d->UserData);
            if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
                if (self->m_history.empty()) return 0;
                int idx = self->m_historyIdx;
                if (d->EventKey == ImGuiKey_UpArrow) {
                    idx = idx < 0 ? static_cast<int>(self->m_history.size()) - 1
                                  : std::max(0, idx - 1);
                } else {
                    idx = idx < 0 ? -1 : idx + 1;
                    if (idx >= static_cast<int>(self->m_history.size())) idx = -1;
                }
                self->m_historyIdx = idx;
                const std::string& s =
                    idx >= 0 ? self->m_history[idx] : std::string();
                d->DeleteChars(0, d->BufTextLen);
                if (!s.empty()) d->InsertChars(0, s.c_str());
            }
            return 0;
        },
        this);
    if (!candidates.empty() && ImGui::IsItemFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        std::snprintf(m_input, sizeof(m_input), "%s",
                      candidates.front()->name.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Run") || submit) {
        const std::string cmd(m_input);
        m_input[0] = '\0';
        execute(cmd);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Tab 补全 · ↑/↓ 历史 · 'help' 列出全部命令");
    }

    ImGui::End();
}

}  // namespace editor
