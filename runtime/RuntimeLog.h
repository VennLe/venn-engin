#pragma once
// ============================================================
// runtime/RuntimeLog —— 日志落盘（**用户目录**，不是安装目录）
//
// ------------------------------------------------------------
// 为什么必须落用户目录
// ------------------------------------------------------------
// 出货版可能装在 `C:\Program Files\...`（普通用户**没有写权限**），
// 而"日志写在 exe 旁边"还有第二个问题：发布包里会越跑越脏 ——
// 玩家看到的目录里零件文件一堆。
//
// 位置：`%LOCALAPPDATA%/Venn/<game>/logs/runtime-YYYYMMDD-HHMMSS.log`
//   · LOCALAPPDATA 拿不到（非 Windows / 精简环境）→ 退 %TEMP%
//   · 任何一步失败都**不致命**：日志写不下去，游戏照跑
//
// ------------------------------------------------------------
// 实现方式
// ------------------------------------------------------------
// `core::Logger` 有一个 sink 钩子（编辑器用它把日志喂进 Console 面板）。
// 运行时没有 Console，所以这里挂一个"写文件"的 sink —— 同一个钩子，
// 换一个消费者。**单消费者**是它的设计前提（见 Logger.h），
// 所以运行时不要再挂第二个。
// ============================================================

#include <string>

namespace runtime {

class RuntimeLog {
public:
    RuntimeLog() = default;
    ~RuntimeLog();

    RuntimeLog(const RuntimeLog&) = delete;
    RuntimeLog& operator=(const RuntimeLog&) = delete;

    // 打开日志文件并挂上 sink。返回日志文件路径；失败返回空串。
    // `gameName` 决定子目录（例："venn" → .../Venn/venn/logs/）。
    std::string open(const std::string& gameName);
    void close();

    const std::string& path() const { return m_path; }
    bool isOpen() const { return m_file != nullptr; }

private:
    void* m_file = nullptr;   // std::FILE*（头文件里不拖 <cstdio>）
    std::string m_path;
};

// 日志目录（不创建）。`gameName` 为空时用 "venn"。
std::string runtimeLogDir(const std::string& gameName);

} // namespace runtime
