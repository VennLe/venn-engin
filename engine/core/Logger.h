#pragma once
// ============================================================
// Logger.h —— 极简日志（header-only）
// 用法：VK_LOG_INFO("x = %d", x);
// 输出到 stdout，带级别前缀；后续可扩展为文件 Sink / 着色输出
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

// 变参函数（由宏转发，保证 va_start 合法）
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
inline void logMessage(LogLevel level, const char* fmt, ...) {
    std::printf("[%s] ", logLevelTag(level));
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

} // namespace core

#define VK_LOG_DEBUG(...) core::logMessage(core::LogLevel::Debug, __VA_ARGS__)
#define VK_LOG_INFO(...)  core::logMessage(core::LogLevel::Info, __VA_ARGS__)
#define VK_LOG_WARN(...)  core::logMessage(core::LogLevel::Warn, __VA_ARGS__)
#define VK_LOG_ERROR(...) core::logMessage(core::LogLevel::Error, __VA_ARGS__)
