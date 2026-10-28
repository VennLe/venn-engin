#include "render/ShaderPath.h"

#include "assets/AssetSource.h"

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

    // ① 磁盘优先（开发态 / 编辑器 / 热重载都靠这条）
    for (auto& c : candidates) {
        if (fs::exists(c)) return c;
    }

    // ② 磁盘上没有 → 打成了包？（G29-c）
    //    着色器是**引擎侧**资产，出包后躺在 `game.pak` 的 `shaders/` 下。
    //    和 `resolveAssetPath` 同一个套路：落到内容寻址缓存再返回路径，
    //    所以 `vkutil::readFile(path)` 那六十来个调用点一行都不用改。
    //
    //    ⚠ 会有**大量未命中**：`watchShaderFiles()` 故意把全部关键字变体的
    //      .spv 都注册进热重载（"多听几个空路径比漏听安全"），这些文件本来
    //      就不存在。所以这条分支必须便宜 —— `Pak::has` 只做一次哈希查表。
    {
        const std::string entry = "shaders/" + filename;
        if (assets::ensureAssetPakMounted() && assets::assetPakHas(entry)) {
            // **返回虚拟路径**（`pak:shaders/x.spv`），不落盘：
            // `vkutil::readFile` 认这个前缀，直接从包内存读（G29-d）。
            //
            // 为什么不落到缓存文件（第一版就是这么做的）：启动时几乎要建
            // 全部管线变体，实测会写 210 个 .spv / 13 MB 到用户缓存目录 ——
            // "按需解包"对 shader 来说等于"全解包"。改成直读之后缓存里
            // 一个 .spv 都没有，而调用点仍然一行不用改。
            return std::string("pak:") + entry;
        }
    }
    return candidates.front();  // 让读取处报出可读的错误
}

} // namespace render
