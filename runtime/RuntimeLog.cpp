#include "RuntimeLog.h"

#include "core/Logger.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace runtime {

namespace {

// 当前挂着的文件（sink 是裸函数指针，没有 userdata，只能走文件级静态）。
// 单实例、主线程 —— 与 Logger 的 sink 约定一致。
std::FILE* g_logFile = nullptr;

void fileSink(core::LogLevel level, const char* msg) {
    if (!g_logFile || !msg) return;
    std::fprintf(g_logFile, "[%s] %s\n", core::logLevelTag(level), msg);
    std::fflush(g_logFile);
}

std::string baseDir() {
    if (const char* p = std::getenv("LOCALAPPDATA")) {
        if (p[0]) return std::string(p);
    }
    if (const char* p = std::getenv("APPDATA")) {
        if (p[0]) return std::string(p);
    }
    if (const char* p = std::getenv("TEMP")) {
        if (p[0]) return std::string(p);
    }
    return ".";
}

std::string stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

} // namespace

std::string runtimeLogDir(const std::string& gameName) {
    namespace fs = std::filesystem;
    const std::string name = gameName.empty() ? "venn" : gameName;
    fs::path p = fs::path(baseDir()) / "Venn" / name / "logs";
    return p.string();
}

std::string RuntimeLog::open(const std::string& gameName) {
    close();

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = runtimeLogDir(gameName);
    fs::create_directories(dir, ec);   // ec 版本：目录已存在也不算错
    if (ec) {
        VK_LOG_WARN("GAME-LOG 无法创建日志目录 %s（%s）", dir.string().c_str(),
                    ec.message().c_str());
        return {};
    }

    const fs::path file = dir / ("runtime-" + stamp() + ".log");
    std::FILE* f = std::fopen(file.string().c_str(), "w");
    if (!f) {
        VK_LOG_WARN("GAME-LOG 无法打开日志文件 %s", file.string().c_str());
        return {};
    }
    m_file = f;
    g_logFile = f;
    m_path = file.string();
    core::setLogSink(&fileSink);
    return m_path;
}

void RuntimeLog::close() {
    if (m_file || g_logFile) {
        core::setLogSink(nullptr);
        g_logFile = nullptr;
    }
    if (m_file) {
        std::fclose(static_cast<std::FILE*>(m_file));
        m_file = nullptr;
    }
}

RuntimeLog::~RuntimeLog() { close(); }

} // namespace runtime
