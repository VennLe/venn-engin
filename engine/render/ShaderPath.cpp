#include "render/ShaderPath.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace render {

std::string resolveShaderPath(const std::string& filename) {
    namespace fs = std::filesystem;
    std::vector<std::string> candidates;

    if (const char* env = std::getenv("MYVK_SHADER_DIR")) {
        candidates.push_back(std::string(env) + "/" + filename);
    }

    // 可执行文件所在目录 /shaders
    {
        char buf[1024] = {};
#if defined(_WIN32)
        GetModuleFileNameA(nullptr, buf, sizeof(buf));
#else
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) buf[n] = '\0';
#endif
        if (buf[0]) {
            fs::path exeDir = fs::path(buf).parent_path();
            candidates.push_back((exeDir / "shaders" / filename).string());
        }
    }

    candidates.push_back("shaders/" + filename);
    candidates.push_back("assets/shaders/" + filename);

    for (auto& c : candidates) {
        if (fs::exists(c)) return c;
    }
    return candidates.front();  // 让读取处报出可读的错误
}

} // namespace render
