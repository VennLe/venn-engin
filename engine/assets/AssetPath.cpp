#include "assets/AssetPath.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace assets {

namespace {

namespace fs = std::filesystem;

// 资产根目录候选（顺序即优先级）：
//   1. 环境变量 MYVK_ASSET_DIR
//   2. 可执行文件所在目录 /assets     （CMake 会把 assets 拷过去）
//   3. 当前工作目录 /assets
//   4. 当前工作目录本身              （从项目根目录直接运行）
std::vector<std::string> assetRoots() {
    std::vector<std::string> roots;

    if (const char* env = std::getenv("MYVK_ASSET_DIR")) {
        roots.push_back(fs::path(env).string());
    }

    char buf[1024] = {};
#if defined(_WIN32)
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0';
#endif
    if (buf[0]) {
        const fs::path exeDir = fs::path(buf).parent_path();
        roots.push_back((exeDir / "assets").string());
    }

    roots.push_back("assets");
    roots.push_back("");  // 当前工作目录自身
    return roots;
}

std::string joinRoot(const std::string& root, const std::string& relative) {
    if (root.empty()) return relative;
    return (fs::path(root) / relative).string();
}

} // namespace

std::string resolveAssetPath(const std::string& relative) {
    std::vector<std::string> candidates;
    for (const auto& root : assetRoots()) {
        candidates.push_back(joinRoot(root, relative));
    }

    for (const auto& c : candidates) {
        if (fs::exists(c)) return c;
    }
    return candidates.front();  // 交给读取处报出可读的错误
}

std::string makeAssetRelative(const std::string& path) {
    std::error_code ec;
    const fs::path abs = fs::absolute(fs::path(path), ec);
    if (ec) return path;

    // 依次尝试各资产根：只要能算出"不逃出该根"的相对路径就采用
    // （用 generic_string() 统一成正斜杠，跨平台 JSON 才不会变味）
    for (const auto& root : assetRoots()) {
        if (root.empty()) continue;  // cwd 兜底留到后面单独处理
        const fs::path rp = fs::absolute(fs::path(root), ec);
        if (ec) continue;
        const fs::path rel = fs::relative(abs, rp, ec);
        if (ec || rel.empty()) continue;
        const std::string s = rel.generic_string();
        if (s.rfind("..", 0) != 0) return s;
    }

    // 退而求其次：相对当前工作目录
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        const fs::path rel = fs::relative(abs, cwd, ec);
        if (!ec && !rel.empty()) {
            const std::string s = rel.generic_string();
            if (s.rfind("..", 0) != 0) return s;
        }
    }

    return fs::path(path).generic_string();
}

} // namespace assets
