#pragma once
// ============================================================
// Logger.h —— 极简日志（header-only）
// 用法：VK_LOG_INFO("x = %d", x);
// 输出到 stdout，带级别前缀；另有两个**互相独立**的观察者：
//   · sink（E16）—— Console 面板用它把日志汇进内存环形缓冲，
//     让用户在编辑器里看见自己的 WARN。
//   · hook（E21）—— 通知中心用它把 Warn/Error 升级成可见的 toast。
// 两者都是单消费者、都在主线程。
// ============================================================

#include <cstdarg>
#include <cstdio>

namespace core {

enum class LogLevel : int { Debug = 0, Info, Warn, Error };

inline const char* logLevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

// ---- E16：日志 sink（单消费者；编辑器 Console 面板是唯一挂载者）----
// msg 是格式化后的**整行**（不含级别前缀），级别单独给 —— 显示端
// 自己决定配色。只在主线程打日志（本项目所有 VK_LOG_* 都在主线程），
// 所以不做加锁。
using LogSinkFn = void (*)(LogLevel level, const char* msg);
inline LogSinkFn g_logSink = nullptr;
inline void setLogSink(LogSinkFn s) { g_logSink = s; }

// ---- E21：日志 hook（第二个、**正交**的观察者；通知中心挂它）----
// 为什么不是"再挂一个 sink"：sink 的语义是"日志的**显示端**"（Console
// 面板的环形缓冲），而通知中心关心的是完全另一件事 —— "这条该不该弹给
// 用户看"。两件事各走一条通道，谁挂了谁都没关系：
//     sink：文本留痕（Console 里能翻到）
//     hook：要不要弹 toast（只有 Warn/Error）
// 同样是单消费者、同样只在主线程。
using LogHookFn = void (*)(LogLevel level, const char* msg);
inline LogHookFn g_logHook = nullptr;
inline void setLogHook(LogHookFn h) { g_logHook = h; }

// 变参函数（由宏转发，保证 va_start 合法）
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
inline void logMessage(LogLevel level, const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::printf("[%s] %s\n", logLevelTag(level), buf);
    std::fflush(stdout);
    if (g_logSink) g_logSink(level, buf);
    if (g_logHook) g_logHook(level, buf);
}

} // namespace core

#define VK_LOG_DEBUG(...) core::logMessage(core::LogLevel::Debug, __VA_ARGS__)
#define VK_LOG_INFO(...)  core::logMessage(core::LogLevel::Info, __VA_ARGS__)
#define VK_LOG_WARN(...)  core::logMessage(core::LogLevel::Warn, __VA_ARGS__)
#define VK_LOG_ERROR(...) core::logMessage(core::LogLevel::Error, __VA_ARGS__)
