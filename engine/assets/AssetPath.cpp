#include "assets/AssetPath.h"

#include "assets/AssetSource.h"

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

    // ① 磁盘优先。散文件目录永远赢 —— 编辑器改完立刻生效、热重载也靠它。
    for (const auto& c : candidates) {
        if (fs::exists(c)) return c;
    }

    // ② 磁盘上没有 → 看看是不是打成了包（G29-b）。
    //    这里调用的是**幂等**探测：第一次走到这条分支才去找
    //    `<exeDir>/assets.pak`，之后不再碰文件系统。
    //    包内路径 = "assets/" + 相对路径（包是**相对 dist 根**布局的，
    //    见 AssetSource.h 的说明）。
    const std::string entry = "assets/" + relative;
    if (ensureAssetPakMounted() && assetPakHas(entry)) {
        const std::string mat = materializePakEntry(entry);
        if (!mat.empty()) return mat;
    }

    return candidates.front();  // 交给读取处报出可读的错误
}

std::string makeAssetRelative(const std::string& path) {
    std::error_code ec;
    const fs::path abs = fs::absolute(fs::path(path), ec);
    if (ec) return path;

    // ⓪ 从包里落出来的文件：绝对路径在用户的缓存目录里，**不能**写进场景。
    //    先问一遍反查表，命中就还原成原始相对路径。
    {
        std::string rel;
        if (relativeForMaterialized(abs.generic_string(), rel)) return rel;
    }

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

// ---------------------------------------------------------------- 路径类型
//
// ⚠ 大小写不敏感（`std::tolower` 逐字符比）：`.FBX` / `.Fbx` / `.fbx` 都是
//   FBX。资源商店的包里三种写法都出现过，按字节比会漏掉一半。
// ⚠ 用后缀比而不是 `fs::path::extension()`：后者对 `a.b/c` 这类没有扩展名
//   的路径返回空串是**对的**，但它不处理大小写，还要多一次构造。
namespace {

bool hasSuffixCI(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

} // namespace

bool isObjPath(const std::string& p) { return hasSuffixCI(p, ".obj"); }
bool isFbxPath(const std::string& p) { return hasSuffixCI(p, ".fbx"); }

} // namespace assets

