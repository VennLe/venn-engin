#pragma once
// ============================================================
// ConsolePanel —— E16：日志 + 命令二合一面板（3A 引擎必备）。
//
// 解决的问题：日志只进 stdout/文件，用户**看不到**自己的 WARN/ERROR
// （无控制台的窗口进程尤其如此）。
//
// ---- 三个设计点 ----
//
// ① **日志汇入走 Logger sink**：core::Logger 的全局 sink 在本面板
//   构造时装上、析构时卸下。环形缓冲保留最近 kMaxLines 条，UI 只读
//   它 —— 不碰 stdout（自动化回归照常解析 stdout，互不干扰）。
//
// ② **命令表驱动**：Command = {name, help, handler}。具体命令由宿主
//   （EditorApp::registerConsoleCommands）注册 —— 面板不认识注册表
//   /渲染器。将来 E18 命令面板共用同一张表（那时再把表挪到公共处）。
//
// ③ **自动补全**：输入非空时列出名字前缀命中的命令（≤6 条），Tab
//   补全第一条，点击即填。↑/↓ 翻输入历史。
// ============================================================

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "core/Logger.h"

namespace editor {

class ConsolePanel {
public:
    struct Line {
        core::LogLevel level;
        std::string text;
    };
    // handler 收到拆好的参数（args[0] = 命令名）；返回值会以 Info 级
    // 打进缓冲（多行用 \n）。返回空串 = 无输出。
    using Handler =
        std::function<std::string(const std::vector<std::string>& args)>;
    struct Command {
        std::string name;
        std::string help;
        Handler run;
    };

    ConsolePanel();
    ~ConsolePanel();

    // 宿主注册命令（重复名会覆盖前者 —— 允许宿主热替换实现）。
    void addCommand(const char* name, const char* help, Handler run);
    // 面板自己的输出（不走全局 sink，避免回环）。
    void print(core::LogLevel level, const std::string& text);
    // 清空缓冲（clear 命令 / Clear 按钮共用）。
    void clear() { m_lines.clear(); }
    // 执行一条命令行（也供 env 钩子 MYVK_EDITOR_CONSOLE_CMD 注入用）。
    void execute(const std::string& cmdline);

    // E21：请求把日志区滚到**最后一条包含 text 的行**（点 Error 的 toast
    // 打开 Console 定位时用）。只是记一个待处理请求，真正滚动发生在下一次
    // draw() —— 那时行已经全部在缓冲里，帧内不做任何跨面板访问。
    void requestScrollToText(const std::string& text) { m_scrollToText = text; }

    // E25：由外部（GPU 时间轴点击某段）设置日志过滤文本。
    // 只填那个已有的搜索框 —— 不新增过滤机制，用户看到的就是他手打
    // 一样的效果（也能马上接着编辑）。超长自动截断到框宽上限。
    void setSearchFilter(const std::string& text);

    // E18：命令表只读访问 —— 宿主把控制台命令**登记进命令注册表**，
    // 于是"控制台能敲的"和"命令面板能搜的"是同一批（消除第二处硬编码）。
    const std::vector<Command>& commands() const { return m_commands; }

    // 缓冲统计（自动化断言用：CONSOLE-DUMP）。
    size_t lineCount() const { return m_lines.size(); }
    size_t countByLevel(core::LogLevel lv) const;

    void draw(bool* open);

    static constexpr size_t kMaxLines = 2048;   // 环形缓冲上限

private:
    std::deque<Line> m_lines;
    std::vector<Command> m_commands;
    std::vector<std::string> m_history;   // ↑/↓ 翻的输入历史
    int m_historyIdx = -1;                // -1 = 不在历史里
    bool m_showInfo = true;
    bool m_showWarn = true;
    bool m_showError = true;
    bool m_autoscroll = true;
    char m_input[256] = {};
    char m_search[64] = {};
    // E21：待处理的"滚到这条日志"请求（空 = 没有）
    std::string m_scrollToText;

    void clearLocked();   // 清缓冲
};

}  // namespace editor
