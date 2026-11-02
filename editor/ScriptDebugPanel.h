#pragma once
// ============================================================
// editor/ScriptDebugPanel —— 脚本断点调试（C8）
//
// 补的洞：`.vks` 从 S0 起就有实例表（`Scripts` 面板能看到实例与 vars），
// 但**没有断点** —— `grep -rn breakpoint` 全仓 0 命中。于是"这个脚本
// 停在哪、那一刻 hp 是多少"只能靠 `print` 往 Console 里刷，而 `print`
// 恰恰看不见"没走到的那一行"。
//
// 这一张面板给的是三件东西：
//   ① 断点：点源码某一行 → 加上/去掉；命中的那一行标 `▶`；
//   ② 继续 / 单步：放行一行之后停在**下一条**语句之前；
//   ③ 停住那一刻的 vars 快照（"协程里的状态看不见"就是这一格）。
//
// ⚠ 断点表住在 `ScriptRuntime`（跨实例共享、活过热重载）；**暂停态住在
//   实例上**（每个实体停在自己的行）。面板只管显示与转发，不持状态。
// ============================================================

#include "script/ScriptRuntime.h"

#include <string>
#include <vector>

namespace editor {

class ScriptDebugPanel {
public:
    void attach(vks::ScriptRuntime* rt) { m_rt = rt; }

    void draw();

    // ---- 命令入口（与按钮走同一条路）----
    bool addBreakpoint(const std::string& path, int line, std::string* msg);
    int removeBreakpoint(const std::string& path, int line);
    int clearBreakpoints();
    int resumeAll();
    int stepAll();

    // "paused=<n> ..." 一行（控制台 `script.at` 与自动化读它）
    std::string stateLine() const;
    // 暂停实例的逐条明细（entity / path / line），已按实体 id 排序
    std::vector<std::string> pausedLines() const;
    // 暂停实例的变量快照（按名字排序；没暂停 → 空）
    std::vector<std::string> pausedVarLines() const;

    const std::string& selectedPath() const { return m_selPath; }
    bool selectPath(const std::string& path);

private:
    // 每帧刷新"已加载脚本"列表；选择按**路径**记（下标会随列表变化漂移）
    void refreshPaths();

    vks::ScriptRuntime* m_rt = nullptr;
    std::vector<std::string> m_paths;
    std::string m_selPath;
    std::string m_status;
    bool m_showVars = true;
};

} // namespace editor
