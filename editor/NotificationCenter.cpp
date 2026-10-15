#include "NotificationCenter.h"

#include "EditorLayoutIO.h"   // editorLayoutAutomationRun()（落盘守卫，与其他持久化同一条规则）

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace editor {

namespace {
using json = nlohmann::json;

constexpr const char* kDefaultPath = "config/editor_notifications.json";
constexpr int kFormatVersion = 1;
constexpr size_t kMaxPersist = 128;   // 落盘只存最近这么多条（文件别无限长）

// hook 是无上下文的函数指针 → 用一个全局"当前通知中心"指针桥接
// （与 ConsolePanel 的 sinkBridge 同一套路；编辑器只有一个实例）。
NotificationCenter* g_active = nullptr;

std::string notifPath() {
    if (const char* p = std::getenv("MYVK_EDITOR_NOTIFICATIONS_PATH")) {
        if (p[0] != '\0') return p;
    }
    return kDefaultPath;
}
}  // namespace

// ============================================================
// Logger 接线
// ============================================================

void notificationLogHook(core::LogLevel level, const char* msg) {
    if (g_active) g_active->onLog(level, msg);
}

void attachNotificationCenter(NotificationCenter* c) { g_active = c; }

NotificationCenter* activeNotificationCenter() { return g_active; }

// ============================================================
// 配色 / 名字
// ============================================================

const char* NotificationCenter::levelName(core::LogLevel lv) {
    switch (lv) {
        case core::LogLevel::Debug: return "Debug";
        case core::LogLevel::Info:  return "Info";
        case core::LogLevel::Warn:  return "Warn";
        case core::LogLevel::Error: return "Error";
    }
    return "?";
}

NotificationCenter::Color NotificationCenter::levelColor(core::LogLevel lv) {
    switch (lv) {
        case core::LogLevel::Debug: return {0.55f, 0.57f, 0.62f};   // 暗灰
        case core::LogLevel::Warn:  return {0.95f, 0.80f, 0.40f};   // 黄
        case core::LogLevel::Error: return {1.00f, 0.45f, 0.40f};   // 红
        default:                    return {0.72f, 0.74f, 0.78f};   // 灰
    }
}

// ============================================================
// 产生通知
// ============================================================

void NotificationCenter::add(const Notification& n, bool toast) {
    if (toast) {
        m_toasts.push_back(n);
        while (m_toasts.size() > kMaxToasts) m_toasts.pop_front();
    }
    m_history.push_back(n);
    while (m_history.size() > kMaxHistory) m_history.pop_front();
    core::logMessage(core::LogLevel::Info,
                     "NOTIF-PUSH level=%s source=%s text=\"%s\" toasts=%zu "
                     "history=%zu",
                     levelName(n.level), n.fromLog ? "log" : "api",
                     n.text.c_str(), m_toasts.size(), m_history.size());
}

void NotificationCenter::onLog(core::LogLevel level, const char* msg) {
    // 只有 Warn / Error 自动升级 —— Info/Debug 启动一次就是几百条，
    // 全弹出来会把屏幕糊满（Info 通知只能显式 push()）。
    if (level != core::LogLevel::Warn && level != core::LogLevel::Error) return;
    if (!msg || !msg[0]) return;
    const std::string text = msg;

    // 去重：同文本的 toast 还活着 → 只刷新它的寿命并 +1，不再叠第二条。
    // 引擎里"每帧一条同样的警告"很常见，不去重的话 4 秒能堆出几百条。
    for (Notification& t : m_toasts) {
        if (t.level == level && t.text == text) {
            t.age = 0.0;
            t.repeat += 1;
            if (t.repeat <= 3 || t.repeat % 100 == 0) {
                core::logMessage(core::LogLevel::Info,
                                 "NOTIF-DEDUP level=%s repeat=%d text=\"%s\"",
                                 levelName(level), t.repeat, text.c_str());
            }
            return;
        }
    }

    Notification n;
    n.level = level;
    n.text = text;
    n.fromLog = true;
    add(n, true);
}

void NotificationCenter::push(core::LogLevel level, const std::string& text) {
    if (text.empty()) return;
    Notification n;
    n.level = level;
    n.text = text;
    n.fromLog = false;
    add(n, true);
}

// ============================================================
// 每帧
// ============================================================

void NotificationCenter::update(float dt) {
    for (Notification& t : m_toasts) t.age += static_cast<double>(dt);
    for (Notification& t : m_history) t.age += static_cast<double>(dt);
    // 过期淘汰：m_toasts 是按时间 push_back 的，所以队头一定最老。
    while (!m_toasts.empty() && m_toasts.front().age >= kToastLife)
        m_toasts.pop_front();
}

void NotificationCenter::drawToasts() {
    if (m_toasts.empty()) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 1.0f || io.DisplaySize.y <= 1.0f) return;

    const float width = 360.0f;
    const float margin = 12.0f;
    const float right = io.DisplaySize.x - width - margin;
    float y = margin;

    int clicked = -1;   // 记下来，循环外再处理（避免在遍历 m_toasts 时改它）
    for (size_t i = 0; i < m_toasts.size(); ++i) {
        const Notification& t = m_toasts[i];
        // 淡入 0.25s / 淡出 0.8s
        float a = 1.0f;
        if (t.age < 0.25) a = static_cast<float>(t.age / 0.25);
        const double left = kToastLife - t.age;
        if (left < 0.8) a = std::min(a, static_cast<float>(left / 0.8));
        a = std::clamp(a, 0.0f, 1.0f);
        const Color c = levelColor(t.level);

        ImGui::SetNextWindowPos(ImVec2(right, y), ImGuiCond_Always);
        // 宽固定 / 高自适应（0 = 该轴自动）
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.94f * a);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 7.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(c.r, c.g, c.b, 0.85f * a));

        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoBringToFrontOnFocus;

        char id[32];
        std::snprintf(id, sizeof(id), "##toast%zu", i);
        bool hovered = false;
        if (ImGui::Begin(id, nullptr, flags)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, a));
            ImGui::TextUnformatted(levelName(t.level));
            ImGui::PopStyleColor();
            if (t.repeat > 1) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.62f, 0.64f, 0.68f, a), "x%d",
                                   t.repeat);
            }
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.88f, 0.90f, 0.93f, a));
            ImGui::TextWrapped("%s", t.text.c_str());
            ImGui::PopStyleColor();
            if (t.fromLog) {
                ImGui::TextColored(ImVec4(0.58f, 0.62f, 0.70f, 0.9f * a),
                                   t.level == core::LogLevel::Error
                                       ? "click: open in Console"
                                       : "click: dismiss");
            }
            hovered = ImGui::IsWindowHovered();
            y += ImGui::GetWindowHeight() + 6.0f;
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            clicked = static_cast<int>(i);
    }

    if (clicked >= 0) clickToast(clicked);
}

void NotificationCenter::drawTaskBar() {
    if (m_tasks.empty() || m_tasksMinimized) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 1.0f) return;

    const float width = 340.0f;
    const float margin = 12.0f;
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x - width - margin, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 7.0f));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoFocusOnAppearing;

    if (ImGui::Begin("##taskbar", nullptr, flags)) {
        ImGui::TextUnformatted("Tasks");
        ImGui::SameLine(ImGui::GetWindowWidth() - 28.0f);
        if (ImGui::SmallButton("v")) m_tasksMinimized = true;   // 最小化到状态栏
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Minimize (the count stays in the status bar)");
        ImGui::Separator();

        for (size_t i = 0; i < m_tasks.size(); ++i) {
            const NotifTask& t = m_tasks[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(t.title.c_str());
            if (t.progress >= 0.0f) {
                char pct[16];
                std::snprintf(pct, sizeof(pct), "%d%%",
                              static_cast<int>(t.progress * 100.0f + 0.5f));
                const float w = ImGui::CalcTextSize(pct).x;
                ImGui::SameLine(ImGui::GetWindowWidth() - w - 12.0f);
                ImGui::TextDisabled("%s", pct);
                ImGui::ProgressBar(std::clamp(t.progress, 0.0f, 1.0f),
                                   ImVec2(-1.0f, 6.0f), "");
            } else {
                // 不确定进度：用负值 + 时间驱动的滚动条（ImGui 的惯用法）
                ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()),
                                   ImVec2(-1.0f, 6.0f), "working...");
            }
            ImGui::SameLine(ImGui::GetWindowWidth() - 62.0f);
            if (ImGui::SmallButton("Cancel")) requestCancel(t.id);
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void NotificationCenter::drawHistoryWindow(bool* open) {
    if (!open || !*open) return;
    ImGui::SetNextWindowSize(ImVec2(620.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Notification History###notifhist", open)) {
        ImGui::End();
        return;
    }

    static char search[64] = {};
    ImGui::Text("total %d", historyCount());
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f), "| warns %d",
                       warnCount());
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "| errors %d",
                       errorCount());
    ImGui::SameLine();
    ImGui::TextDisabled("| unread %d", unread());
    ImGui::SameLine(ImGui::GetWindowWidth() - 210.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##notifsearch", "filter...", search,
                             sizeof(search));

    if (ImGui::Button("Mark all read")) markAllRead();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) clear();

    // 最小化的任务条要能从历史窗口恢复（否则用户点了 v 就再也找不回来）
    if (!m_tasks.empty()) {
        ImGui::SameLine();
        bool minimized = m_tasksMinimized;
        if (ImGui::Checkbox("minimize task bar", &minimized))
            m_tasksMinimized = minimized;
    }

    ImGui::Separator();
    // 最新在上：错过的通知要一眼看到，不用往回滚。
    if (ImGui::BeginChild("##notiflist", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders)) {
        for (int i = 0; i < historyCount(); ++i) {
            Notification& n = m_history[m_history.size() - 1 -
                                        static_cast<size_t>(i)];
            if (search[0] && n.text.find(search) == std::string::npos) continue;
            const Color c = levelColor(n.level);
            ImGui::PushID(i);
            ImGui::TextColored(ImVec4(c.r, c.g, c.b, 1.0f), "%-5s",
                               levelName(n.level));
            ImGui::SameLine();
            if (!n.read)
                ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), "*");
            else
                ImGui::TextDisabled(" ");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", n.text.c_str());
            if (n.repeat > 1) {
                ImGui::SameLine();
                ImGui::TextDisabled("x%d", n.repeat);
            }
            if (n.fromLog && ImGui::IsItemHovered())
                ImGui::SetTooltip("Open in Console");
            if (n.fromLog && ImGui::IsItemClicked()) {
                // 与点 toast 完全同一条路：立刻打开 Console 并滚到那一行
                m_focusText = n.text;
                m_focusPending = true;
                n.read = true;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ============================================================
// 查询
// ============================================================

int NotificationCenter::warnCount() const {
    int n = 0;
    for (const Notification& t : m_history)
        if (t.level == core::LogLevel::Warn) ++n;
    return n;
}

int NotificationCenter::errorCount() const {
    int n = 0;
    for (const Notification& t : m_history)
        if (t.level == core::LogLevel::Error) ++n;
    return n;
}

int NotificationCenter::unread() const {
    int n = 0;
    for (const Notification& t : m_history)
        if (!t.read) ++n;
    return n;
}

const std::string& NotificationCenter::historyText(int i) const {
    static const std::string empty;
    if (i < 0 || i >= historyCount()) return empty;
    return m_history[m_history.size() - 1 - static_cast<size_t>(i)].text;
}

void NotificationCenter::markAllRead() {
    for (Notification& t : m_history) t.read = true;
}

void NotificationCenter::clear() {
    m_toasts.clear();
    m_history.clear();
}

// ============================================================
// 状态栏锚点
// ============================================================

void NotificationCenter::logStatus(int errors, int warns, int tasks,
                                   bool dirty, const char* tool) {
    const int d = dirty ? 1 : 0;
    // 边沿触发：四个数值都没变就不打（tool 提示每帧都在变，不进判等，
    // 否则这条锚点会退化成每帧一条的噪声）。
    if (errors == m_lastErrors && warns == m_lastWarns &&
        tasks == m_lastTasks && d == m_lastDirty)
        return;
    m_lastErrors = errors;
    m_lastWarns = warns;
    m_lastTasks = tasks;
    m_lastDirty = d;
    core::logMessage(
        core::LogLevel::Info,
        "NOTIF-STATUSBAR errors=%d warns=%d tasks=%d dirty=%d unread=%d "
        "tool=\"%s\"",
        errors, warns, tasks, d, unread(), tool ? tool : "");
}

// ============================================================
// 点击 toast
// ============================================================

bool NotificationCenter::takeConsoleFocusRequest(std::string* text) {
    if (!m_focusPending) return false;
    m_focusPending = false;
    if (text) *text = m_focusText;
    m_focusText.clear();
    return true;
}

void NotificationCenter::clickToast(int idx) {
    if (idx < 0 || idx >= toastCount()) return;
    const Notification n = m_toasts[static_cast<size_t>(idx)];   // 拷贝
    m_toasts.erase(m_toasts.begin() + idx);

    if (n.fromLog) {
        m_focusText = n.text;
        m_focusPending = true;
        core::logMessage(core::LogLevel::Info,
                         "NOTIF-CLICK idx=%d level=%s text=\"%s\" "
                         "focus=console",
                         idx, levelName(n.level), n.text.c_str());
    } else {
        core::logMessage(core::LogLevel::Info, "NOTIF-DISMISS idx=%d level=%s",
                         idx, levelName(n.level));
    }
    // 被点过 = 已读（历史里最近的一条同文本同级别）
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (it->level == n.level && it->text == n.text) {
            it->read = true;
            break;
        }
    }
}

// ============================================================
// 进度任务
// ============================================================

void NotificationCenter::beginTask(const std::string& id,
                                   const std::string& title) {
    for (NotifTask& t : m_tasks) {
        if (t.id != id) continue;
        t.title = title;               // 同名任务重启
        t.progress = 0.0f;
        t.cancelRequested = false;
        t.cancelled = false;
        t.lastBucket = -1;
        core::logMessage(core::LogLevel::Info,
                         "NOTIF-TASK-BEGIN id=%s title=\"%s\" tasks=%zu", id.c_str(),
                         title.c_str(), m_tasks.size());
        return;
    }
    NotifTask t;
    t.id = id;
    t.title = title;
    m_tasks.push_back(t);
    core::logMessage(core::LogLevel::Info,
                     "NOTIF-TASK-BEGIN id=%s title=\"%s\" tasks=%zu", id.c_str(),
                     title.c_str(), m_tasks.size());
}

bool NotificationCenter::setTaskProgress(const std::string& id, float p) {
    for (NotifTask& t : m_tasks) {
        if (t.id != id) continue;
        // 取消请求被消费的那一刻：交给宿主（返回 false → 它自己 endTask）
        if (t.cancelRequested && !t.cancelled) {
            t.cancelled = true;
            return false;
        }
        if (t.cancelled) return false;
        t.progress = p;
        // 逐格进度不刷屏：只在跨过 25% 的整数桶时打一条
        const int bucket = (p < 0.0f)
                               ? -1
                               : static_cast<int>(
                                     std::clamp(p, 0.0f, 1.0f) * 4.0f);
        if (bucket != t.lastBucket) {
            t.lastBucket = bucket;
            core::logMessage(core::LogLevel::Info,
                             "NOTIF-TASK-STEP id=%s progress=%.2f", id.c_str(),
                             static_cast<double>(p));
        }
        return true;
    }
    return false;   // 任务不存在 = 已经结束了
}

void NotificationCenter::requestCancel(const std::string& id) {
    for (NotifTask& t : m_tasks) {
        if (t.id != id || t.cancelRequested) continue;
        t.cancelRequested = true;
        core::logMessage(core::LogLevel::Info, "NOTIF-TASK-CANCEL id=%s",
                         id.c_str());
        return;
    }
}

void NotificationCenter::endTask(const std::string& id) {
    for (size_t i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id != id) continue;
        const bool cancelled =
            m_tasks[i].cancelled || m_tasks[i].cancelRequested;
        const std::string title = m_tasks[i].title;
        m_tasks.erase(m_tasks.begin() + static_cast<long>(i));
        core::logMessage(core::LogLevel::Info,
                         "NOTIF-TASK-END id=%s cancelled=%d tasks=%zu",
                         id.c_str(), cancelled ? 1 : 0, m_tasks.size());
        if (!cancelled) push(core::LogLevel::Info, title + " - done");
        return;
    }
}

bool NotificationCenter::taskActive(const std::string& id) const {
    for (const NotifTask& t : m_tasks)
        if (t.id == id) return true;
    return false;
}

bool NotificationCenter::taskCancelled(const std::string& id) const {
    for (const NotifTask& t : m_tasks)
        if (t.id == id) return t.cancelled || t.cancelRequested;
    return true;   // 已经不在了 = 结束了
}

// ============================================================
// 落盘 / 读回
// ============================================================

void NotificationCenter::saveToDisk() const {
    // 与布局 / 偏好同一条规则：自动化运行下一律拒写（免得把测试跑出来的
    // 通知存成"用户的"）。
    if (editorLayoutAutomationRun()) return;
    json j;
    j["version"] = kFormatVersion;
    j["tasksMinimized"] = m_tasksMinimized;
    json arr = json::array();
    const size_t begin = m_history.size() > kMaxPersist
                             ? m_history.size() - kMaxPersist
                             : 0;
    for (size_t i = begin; i < m_history.size(); ++i) {
        const Notification& n = m_history[i];
        arr.push_back(json{{"level", static_cast<int>(n.level)},
                           {"text", n.text},
                           {"read", n.read}});
    }
    j["history"] = arr;

    const std::string path = notifPath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        VK_LOG_WARN("Notification: 打不开写入目标 '%s'", path.c_str());
        return;
    }
    out << j.dump(2) << "\n";
    if (!out) {
        VK_LOG_WARN("Notification: 写入失败 '%s'", path.c_str());
        return;
    }
    VK_LOG_INFO("NOTIF-SAVE path=\"%s\" history=%zu", path.c_str(),
                m_history.size());
}

void NotificationCenter::loadFromDisk() {
    // 显式路径 → 读放行（与偏好同一条规则）
    const bool explicitPath =
        std::getenv("MYVK_EDITOR_NOTIFICATIONS_PATH") != nullptr;
    if (editorLayoutAutomationRun() && !explicitPath) return;

    const std::string path = notifPath();
    std::ifstream in(path, std::ios::binary);
    if (!in) return;   // 没有存档 = 第一次启动

    try {
        json j = json::parse(in);
        if (!j.is_object() || !j.contains("history") ||
            !j["history"].is_array())
            return;
        for (const auto& e : j["history"]) {
            if (!e.is_object() || !e.contains("text")) continue;
            Notification n;
            const int lv = e.value("level", 1);
            n.level = (lv >= 0 && lv <= 3) ? static_cast<core::LogLevel>(lv)
                                           : core::LogLevel::Info;
            n.text = e.value("text", std::string());
            n.read = e.value("read", true);   // 上次会话的历史默认已读
            n.age = 999.0;                    // 进来就是老的（历史列表用）
            if (!n.text.empty()) m_history.push_back(n);
        }
        m_tasksMinimized = j.value("tasksMinimized", false);
        VK_LOG_INFO("NOTIF-LOAD path=\"%s\" history=%zu", path.c_str(),
                    m_history.size());
    } catch (const std::exception& e) {
        VK_LOG_WARN("Notification: '%s' 解析失败（%s）", path.c_str(),
                    e.what());
    }
}

}  // namespace editor
