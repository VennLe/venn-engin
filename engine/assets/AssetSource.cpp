#include "assets/AssetSource.h"

#include "assets/Pak.h"
#include "core/Logger.h"

// ⚠ 只为把 `vkutil::g_pakRead` 填上（见 rhi/VulkanCommon.h 的说明）。
//   assets 依赖 rhi 是既有方向（assets/Texture.h 就在用 rhi 的类型）。
#include "rhi/VulkanCommon.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace assets {

namespace {

namespace fs = std::filesystem;

Pak g_pak;
bool g_open = false;
bool g_probed = false;          // 是否已经探测过 <exeDir>/assets.pak
std::string g_pakPath;
std::string g_cacheRoot;
std::mutex g_mu;                // 保护 materialize（并发加载时同一条目只落一次）
// 落出来的缓存文件 → 原始相对路径。反查用（见 relativeForMaterialized）。
std::unordered_map<std::string, std::string> g_materialized;

std::string exeDir() {
    char buf[1024] = {};
#if defined(_WIN32)
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
#else
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0';
#endif
    if (!buf[0]) return {};
    return fs::path(buf).parent_path().string();
}

// 缓存根：%LOCALAPPDATA%\Venn\assetcache（Windows）/ $XDG_CACHE_HOME/venn
// 放用户目录而不是 exe 旁边：发出去的目录里多个 `.venn_cache` 会让人以为
// 是垃圾文件；而且 Program Files 下通常不可写。
std::string resolveCacheRoot() {
    std::error_code ec;
    // 环境变量优先：测试要能指定一个**自己的**缓存目录（不然跑一次用例
    // 就往用户的 %LOCALAPPDATA% 里堆一堆哈希目录，而且没法从冷路径开始）。
    // 顺带也解决了"漫游配置 / 多硬盘"这种部署偏好。
    if (const char* env = std::getenv("MYVK_ASSET_CACHE")) {
        if (env[0]) {
            fs::create_directories(fs::path(env), ec);
            if (!ec) return fs::path(env).string();
        }
    }
    fs::path base;
#if defined(_WIN32)
    if (const char* la = std::getenv("LOCALAPPDATA")) base = fs::path(la) / "Venn";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME")) base = fs::path(x) / "venn";
#endif
    if (base.empty()) {
        base = fs::temp_directory_path(ec);
        if (ec) return {};
        base /= "venn-assetcache";
    }
    base /= "assetcache";
    fs::create_directories(base, ec);
    if (ec) return {};
    return base.string();
}

std::string cacheRootLocked() {
    if (g_cacheRoot.empty()) g_cacheRoot = resolveCacheRoot();
    return g_cacheRoot;
}

} // namespace

// ============================================================
// 挂载
// ============================================================
bool mountAssetPak(const std::string& absPakPath, std::string& err) {
    std::string e;
    if (!g_pak.open(absPakPath, e)) {
        err = e;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_open = true;
        g_pakPath = absPakPath;
        g_cacheRoot.clear();   // 换包可能换缓存根（首次访问时重算）
    }
    // 让 rhi 的 readFile 能直读包内存（`pak:` 虚拟路径）。
    // 注册在**挂载成功之后**：没挂包时这个指针是 nullptr，readFile 走原路径。
    vkutil::g_pakRead = [](const std::string& entry, std::string& out) -> bool {
        std::string err;
        return readFromAssetPak(entry, out, err);
    };

    VK_LOG_INFO("ASSET-PAK mounted path=%s entries=%d raw=%llu packed=%llu",
                absPakPath.c_str(), g_pak.fileCount(),
                static_cast<unsigned long long>(g_pak.rawBytes()),
                static_cast<unsigned long long>(g_pak.packedBytes()));
    return true;
}

bool mountAssetPakFromExeDir() {
    const std::string dir = exeDir();
    if (dir.empty()) return false;
    const fs::path cand = fs::path(dir) / "game.pak";
    std::error_code ec;
    if (!fs::is_regular_file(cand, ec)) return false;
    std::string err;
    if (!mountAssetPak(cand.string(), err)) {
        // ⚠ 包坏了要**喊出来**：静默回退成"文件不存在"会让排查从
        //   "包坏了"变成"为什么贴图加载不出来"，完全两个方向。
        VK_LOG_ERROR("ASSET-PAK failed path=%s err=%s", cand.string().c_str(),
                     err.c_str());
        return false;
    }
    return true;
}

bool ensureAssetPakMounted() {
    if (g_open) return true;
    if (g_probed) return false;
    g_probed = true;
    return mountAssetPakFromExeDir();
}

void unmountAssetPak() {
    std::lock_guard<std::mutex> lk(g_mu);
    vkutil::g_pakRead = nullptr;
    g_pak.close();
    g_open = false;
    g_pakPath.clear();
    g_cacheRoot.clear();
    g_materialized.clear();
    g_probed = false;
}

bool assetPakMounted() { return g_open; }
const std::string& assetPakPath() { return g_pakPath; }
int assetPakEntryCount() { return g_open ? g_pak.fileCount() : 0; }
unsigned long long assetPakRawBytes() {
    return g_open ? g_pak.rawBytes() : 0ULL;
}
unsigned long long assetPakPackedBytes() {
    return g_open ? g_pak.packedBytes() : 0ULL;
}

bool assetPakHas(const std::string& entry) {
    return g_open && g_pak.has(entry);
}

bool readFromAssetPak(const std::string& entry, std::string& out,
                      std::string& err) {
    if (!g_open) {
        err = "game pak not mounted";
        return false;
    }
    return g_pak.read(entry, out, err);
}

// ============================================================
// 落盘（内容寻址缓存）
// ============================================================
std::string materializePakEntry(const std::string& entry) {
    if (!g_open) return {};

    std::lock_guard<std::mutex> lk(g_mu);
    // 先查索引拿哈希（也顺便确认"确实在包里"）
    const std::vector<PakEntry>& all = g_pak.entries();
    const PakEntry* hit = nullptr;
    std::string want;
    // 索引表是大小写不敏感的，这里要靠 Pak::has + 逐条比对拿条目
    if (!g_pak.has(entry)) return {};
    // 归一化（与 Pak 内部同一规则）：反斜杠 → 正斜杠、去前导 "./"
    want = entry;
    for (char& c : want) {
        if (c == '\\') c = '/';
    }
    while (want.size() >= 2 && want[0] == '.' && want[1] == '/') want.erase(0, 2);
    for (const PakEntry& pe : all) {
        std::string p = pe.path;
        if (p.size() == want.size()) {
            bool same = true;
            for (std::size_t i = 0; i < p.size(); ++i) {
                char a = p[i];
                char b = want[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                if (a != b) { same = false; break; }
            }
            if (same) { hit = &pe; break; }
        }
    }
    if (!hit) return {};

    const std::string root = cacheRootLocked();
    if (root.empty()) return {};

    // 内容寻址：目录名 = 内容的 FNV 哈希 → 换了内容自然换目录，永不读旧字节。
    // 文件名保留原 basename（扩展名）：stbi / ufbx / tinyobj 里有按扩展名
    // 分派的地方，抹掉扩展名会踩到"明明有文件却解析失败"。
    const fs::path dir = fs::path(root) / hit->hash;
    std::string base = hit->path;
    const std::size_t sl = base.find_last_of('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    const fs::path dst = dir / base;

    std::error_code ec;
    if (fs::is_regular_file(dst, ec)) {
        g_materialized[dst.generic_string()] = hit->path;
        return dst.generic_string();  // 命中
    }

    std::string bytes;
    std::string err;
    if (!g_pak.read(hit->path, bytes, err)) {
        VK_LOG_WARN("ASSET-PAK materialize failed path=%s err=%s",
                    hit->path.c_str(), err.c_str());
        return {};
    }
    fs::create_directories(dir, ec);
    if (ec) return {};
    // 先写 tmp 再改名：并行加载时另一个线程可能正好在 check 同一个文件，
    // 见到半截内容就会算出"坏模型"。
    const fs::path tmp = dst.string() + ".tmp";
    {
        std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
        if (!f) return {};
        const std::size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (n != bytes.size()) {
            std::remove(tmp.string().c_str());
            return {};
        }
    }
    std::error_code rec;
    fs::rename(tmp, dst, rec);
    if (rec) {
        std::remove(tmp.string().c_str());
        // 另一个线程刚好抢先落了同一条（同名同内容）→ 用它的即可
        if (fs::is_regular_file(dst, rec)) {
            g_materialized[dst.generic_string()] = hit->path;
            return dst.generic_string();
        }
        return {};
    }
    g_materialized[dst.generic_string()] = hit->path;
    return dst.generic_string();
}

bool relativeForMaterialized(const std::string& abs, std::string& rel) {
    if (abs.empty()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    // 缓存命中那条路径可能带反斜杠（fs::path 在 Windows 上），统一一下再查
    std::string key = abs;
    for (char& c : key) {
        if (c == '\\') c = '/';
    }
    const auto it = g_materialized.find(key);
    if (it == g_materialized.end()) return false;
    rel = it->second;
    return true;
}

const std::string& assetCacheRoot() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_cacheRoot.empty()) g_cacheRoot = resolveCacheRoot();
    return g_cacheRoot;
}

long long clearAssetCache() {
    long long n = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_cacheRoot.empty()) g_cacheRoot = resolveCacheRoot();
        if (g_cacheRoot.empty()) return 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(g_cacheRoot, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            std::error_code sec;
            if (it->is_regular_file(sec)) {
                n += static_cast<long long>(it->file_size(sec));
            }
        }
        fs::remove_all(g_cacheRoot, ec);
    }
    return n;
}

} // namespace assets
