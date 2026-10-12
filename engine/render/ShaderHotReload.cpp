#include "render/ShaderHotReload.h"

#include "render/ShaderPath.h"

#include <filesystem>

namespace render {

namespace {

// 与 editor/script/ScriptEngine.cpp 的 fileWriteTime 同款实现：
// last_write_time 失败（文件不存在/被锁）一律返回 0，绝不上抛 ——
// 外部构建器写文件的瞬间可能读不到，下一轮 poll 自然追上。
int64_t fileWriteTime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<int64_t>(t.time_since_epoch().count());
}

} // namespace

void ShaderHotReload::watch(const std::string& spvFile) {
    Watched w;
    w.name = spvFile;
    w.resolved = resolveShaderPath(spvFile);
    w.lastWrite = fileWriteTime(w.resolved);
    m_watched.push_back(std::move(w));
}

uint32_t ShaderHotReload::poll() {
    m_dirty.clear();
    for (auto& w : m_watched) {
        const int64_t t = fileWriteTime(w.resolved);
        // t == 0：文件此刻读不到（构建器写一半），跳过等下一轮；
        // t == lastWrite：没变。命中变化后**先记下再上报**，否则同一
        // 次改动会每轮 poll 重复触发重建。
        if (t == 0 || t == w.lastWrite) continue;
        w.lastWrite = t;
        m_dirty.push_back(w.name);
    }
    return static_cast<uint32_t>(m_dirty.size());
}

} // namespace render
