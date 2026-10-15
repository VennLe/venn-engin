#pragma once
// ============================================================
// editor/NotificationCenter —— E21：通知 / 进度系统
//
// 解决的问题：`WARN`/`ERROR` 只写进 stdout 和 Console 的环形缓冲，用户在
// 视口里干活时**根本看不见**；长任务（编译着色器 / 烘焙光照 / 导入资产）
// 没有任何进度反馈，看起来就像卡死。
//
// ---- 四个部件 ----
//
// ① **Toast**：右上角堆叠（最多 kMaxToasts 条），kToastLife 秒后自动淡出。
//    按级别配色（Info 灰 / Warn 黄 / Error 红）。来源是 `core::Logger` 的
//    **hook**（见 engine/core/Logger.h 的 g_logHook）—— 只有 `Warn`/`Error`
//    被**自动升级**成通知。⚠ `Info`/`Debug` 不自动升级：启动一次就是几百条
//    Info，全弹出来会把屏幕糊满；Info 通知只能显式 `push()`。
//
// ② **历史**：所有通知（含已淡出的）进环形历史，`Help ▸ Show Notification
//    History` 回看。点 Error 的 toast 会打开 Console 并**滚到那条日志** ——
//    这条路径对 UI 点击与自动化命令是同一条（clickToast）。
//
// ③ **进度条**：`Task = {id, title, progress, cancelRequested}`。宿主用
//    beginTask / setTaskProgress / endTask 驱动；取消按钮 = requestCancel，
//    宿主在 setTaskProgress 收到 false 时停下来收尾。可最小化到状态栏。
//
// ④ **状态栏常驻**：工具提示 / 选中数 / 保存状态 `*` / 后台任务数 /
//    WARN-ERROR 计数 —— 见 EditorApp::drawStatusBar。
//
// ---- 为什么通知不挂在 ConsolePanel 的 sink 上 ----
// Logger 的 sink 是**单消费者**（E16 的 Console 面板占着），而且它同时会
// 收到 Console 自己打印的"回显行"（`> cmd`、命令输出）。通知中心要的是
// "引擎真正报的警告/错误"，所以走 Logger 的第二条独立通道：两条通道互不
// 干扰，Console 管文本留痕、通知管要不要弹给用户看。
//
// ---- 自动化锚点（tools/verify_notifications.py 断言这些行）----
//   NOTIF-PUSH   level=<Info|Warn|Error> source=<log|api> text="..." toasts=N history=M
//   NOTIF-DEDUP  level=... repeat=N text="..."        （同文本在存活期内重复）
//   NOTIF-CLICK  idx=N level=... text="..."           （点了某条 toast）
//   NOTIF-DISMISS idx=N                               （点非 Error 的 toast = 只关掉）
//   NOTIF-TASK-BEGIN  id=... title="..." tasks=N
//   NOTIF-TASK-END    id=... cancelled=<0|1> tasks=N
//   NOTIF-TASK-CANCEL id=...                          （取消请求被消费的那一刻）
//   NOTIF-STATUSBAR errors=N warns=N tasks=N dirty=<0|1> unread=N tool="..."
//   NOTIF-DUMP   history=N warn=N error=N unread=N tasks=N   （退出时，MYVK_LOG_RECTS 下）
//   NOTIF-SAVE / NOTIF-LOAD                          （历史落盘 / 读回）
// ============================================================

#include "core/Logger.h"

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace editor {

// 一条通知。toast 与历史共用同一结构，只是历史活得久。
struct Notification {
    core::LogLevel level = core::LogLevel::Info;
    std::string text;
    double age = 0.0;      // 已存活秒数（toast 淡出用；历史里是"距现在多久"）
    bool read = false;     // 历史里是否已读（未读数 = 状态栏角标）
    bool fromLog = false;  // true = 来自 Logger（Console 里能定位到）
    int repeat = 1;        // 同文本被重复报了几次（去重后的计数）
};

// 一个异步进度任务。
struct NotifTask {
    std::string id;                    // 宿主的稳定键（"shader-compile" / ...）
    std::string title;                 // 显示给用户的标题
    float progress = 0.0f;             // 0..1；< 0 = 不确定进度（转一圈）
    bool cancelRequested = false;      // UI/脚本点了取消
    bool cancelled = false;            // 已经把这个取消**交给宿主**了
    int lastBucket = -1;               // 上一档 25% 进度桶（NOTIF-TASK-STEP 去重）
};

class NotificationCenter {
public:
    static constexpr double kToastLife = 4.0;   // toast 自动淡出（秒）
    static constexpr size_t kMaxToasts = 6;     // 同屏堆叠上限
    static constexpr size_t kMaxHistory = 256;  // 环形历史上限

    // ---------------------------------------------------------- 产生通知
    // 来自 core::Logger 的 hook 落地。只有 Warn / Error 会建通知；
    // 同一文本在 toast 存活期内**重复出现只留一条**（引擎常常一帧一条
    // 同样的警告 —— 去重后靠 repeat 计数体现"还在继续"）。
    void onLog(core::LogLevel level, const char* msg);
    // 显式通知（工具 / 控制台命令 / 命令面板主动弹；Info 只能走这条路）。
    void push(core::LogLevel level, const std::string& text);

    // ---------------------------------------------------------- 每帧
    void update(float dt);                          // 推进 age、淘汰过期 toast
    void drawToasts();                              // 右上角堆叠
    void drawTaskBar();                             // 右下角进度条
    void drawHistoryWindow(bool* open);             // Help ▸ Show Notification History

    // ---------------------------------------------------------- 查询
    int toastCount() const { return static_cast<int>(m_toasts.size()); }
    int historyCount() const { return static_cast<int>(m_history.size()); }
    int warnCount() const;      // 历史里的 Warn 条数
    int errorCount() const;     // 历史里的 Error 条数
    int unread() const;         // 历史里未读条数
    int taskCount() const { return static_cast<int>(m_tasks.size()); }
    // 第 i 条历史（i 从 0 起 = 最新）的文本；越界返回空串。
    const std::string& historyText(int i) const;

    void markAllRead();
    void clear();

    // ---------------------------------------------------------- 状态栏 / 自动化
    // 把状态栏的四个数值喂进来。**只在数值变化时**打一条 NOTIF-STATUSBAR
    // （边沿触发）—— 于是"注入一条 ERROR 后计数 0→1"必然留下一条锚点，
    // 而平稳状态下不刷屏。
    void logStatus(int errors, int warns, int tasks, bool dirty,
                   const char* tool);

    // ---------------------------------------------------------- 点击 toast
    // 宿主每帧问一次：有没有"需要打开 Console 并滚到某条日志"的请求。
    // 有 → 返回 true 并填 *text。
    bool takeConsoleFocusRequest(std::string* text);
    // 点第 idx 条 toast（UI 与自动化命令走**同一条路**）。
    void clickToast(int idx);

    // ---------------------------------------------------------- 进度任务
    void beginTask(const std::string& id, const std::string& title);
    // 推进进度。返回 false = 该任务已被取消（宿主必须停下并 endTask）。
    bool setTaskProgress(const std::string& id, float progress);
    void requestCancel(const std::string& id);      // 取消按钮 / notif.cancel
    void endTask(const std::string& id);            // 收尾（未取消 → 弹一条完成通知）
    bool taskActive(const std::string& id) const;
    bool taskCancelled(const std::string& id) const;
    void setTasksMinimized(bool m) { m_tasksMinimized = m; }
    bool tasksMinimized() const { return m_tasksMinimized; }

    // ---------------------------------------------------------- 落盘
    void saveToDisk() const;   // config/editor_notifications.json
    void loadFromDisk();

    // ---------------------------------------------------------- 配色 / 名字
    static const char* levelName(core::LogLevel lv);
    struct Color { float r, g, b; };
    static Color levelColor(core::LogLevel lv);

private:
    std::deque<Notification> m_toasts;    // 活的 toast（age < kToastLife）
    std::deque<Notification> m_history;   // 全部历史（最新在尾部）
    std::vector<NotifTask> m_tasks;
    bool m_tasksMinimized = false;
    bool m_historyOpenOnce = false;       // 首次打开历史时给一次滚动到底
    bool m_focusPending = false;
    std::string m_focusText;
    // 状态栏锚点的边沿检测（-1 = 还没打过第一条）
    int m_lastErrors = -1, m_lastWarns = -1, m_lastTasks = -1, m_lastDirty = -1;

    void add(const Notification& n, bool toast);
};

// ---------------------------------------------------------------- Logger 接线
// hook 是**无上下文的函数指针** → 用一个全局"当前通知中心"桥接
// （与 ConsolePanel 的 sinkBridge 同一套路；编辑器只有一个实例）。
void notificationLogHook(core::LogLevel level, const char* msg);
// 装上 / 卸下（宿主在中心构造后调用，析构前调 attachNotificationCenter(nullptr)）。
void attachNotificationCenter(NotificationCenter* c);
NotificationCenter* activeNotificationCenter();

}  // namespace editor
