#include "ConsolePanel.h"

#include "DebugRects.h"
#include "Localization.h"
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include "PanelDock.h"

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
        evictOverflow();
        if (nl == text.size()) break;
        start = nl + 1;
    }
}

void ConsolePanel::evictOverflow() {
    // 满了以后**先丢最老的 INFO**，WARN/ERROR 留到最后才丢。
    //
    // 为什么不是简单 pop_front（2026-10-19 实测）：一次普通启动会打
    // 2000+ 条 INFO（着色器变体 / 资产加载），启动期的一条 ERROR 会在
    // 用户打开 Console 之前就被顶掉 —— `verify_notifications` 抓到的是
    // `CONSOLE-DUMP lines=2048 info=2048 warn=0 err=0`，而通知历史里
    // 明明有 error=1。也就是说：**用户永远看不到那条错误**，而这正是
    // Console 面板存在的理由（头注释 ①）。
    //
    // 丢的时候仍然是 FIFO 语义（丢最老的那条 Info），所以"最近发生了什么"
    // 的观感不变；只有当整个缓冲里一条 Info 都没有（极端：全是 ERROR）时
    // 才真正从头部丢。
    while (m_lines.size() > kMaxLines) {
        auto it = std::find_if(m_lines.begin(), m_lines.end(),
                               [](const Line& l) {
                                   return l.level == core::LogLevel::Info;
                               });
        if (it != m_lines.end())
            m_lines.erase(it);
        else
            m_lines.pop_front();
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
    // 首次出现的几何：**贴着底栏上面、横向居中偏左**的一块宽面板。
    //
    // 原来这里什么都没有 → ImGui 用默认的 (60,60) + 自动尺寸，于是一个
    // 440×169 的小窗凭空出现在左上角、正好压在 Hierarchy 标题栏上。用户
    // 报的"点 Output Log 没打开面板"就是这个：窗其实开了，只是那副样子
    // 怎么看都不像"输出日志开着"。给一个像样的默认位置和尺寸。
    // 2026-10-21：视口标签页的一页。托管时矩形由 EditorApp 给，浮动默认
    // 位置/尺寸一律跳过（FirstUseEver 会被 Always 顶掉，没必要设）。
    ImGuiWindowFlags wf = 0;
    const bool docked = applyDockHint(wf);
    if (!docked) {
        const ImGuiIO& io = ImGui::GetIO();
        const float w = std::min(860.0f, io.DisplaySize.x * 0.62f);
        const float h = std::min(330.0f, io.DisplaySize.y * 0.40f);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
        // 底边离窗口底 48px（= 状态栏 + 底栏那一条），左边对齐视口左缘一帶
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f,
                                       io.DisplaySize.y - 48.0f),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 1.0f));
    }
    if (!ImGui::Begin(trId("Console", "panel_console"),
                      docked ? nullptr : open, wf)) {
        ImGui::End();
        return;
    }

    // 自动化锚点：底栏 "输出日志" 按钮的端到端断言要能证明"面板真的开出来了"
    // （只查 OUTPUT-LOG open=1 那条日志，证明不了绘制）。
    logRect("CONSOLE-PANEL", ImGui::GetWindowPos(),
            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                   ImGui::GetWindowPos().y + ImGui::GetWindowHeight()));

    // ---- 工具行：级别过滤 + 搜索 + 操作 ----
    ImGui::Checkbox(TR("Info"), &m_showInfo);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Warn"), &m_showWarn);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Error"), &m_showError);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-190.0f);
    ImGui::InputTextWithHint("##consearch", "filter...", m_search,
                             sizeof(m_search));
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear"))) clearLocked();
    ImGui::SameLine();
    if (ImGui::Button(TR("Copy"))) {
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
    if (ImGui::Button(TR("Save"))) {
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
                ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f), TR("%s"),
                                   l.text.c_str());
                break;
            case core::LogLevel::Error:
                ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.40f, 1.0f), TR("%s"),
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
    ImGui::Checkbox(TR("autoscroll"), &m_autoscroll);

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
                ImGui::SetTooltip(TR("%s"), c->help.c_str());
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
    if (ImGui::Button(TR("Run")) || submit) {
        const std::string cmd(m_input);
        m_input[0] = '\0';
        execute(cmd);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("Tab to complete - Up/Down history - 'help' lists all commands"));
    }

    ImGui::End();
}

}  // namespace editor
