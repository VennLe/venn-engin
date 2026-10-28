#include "assets/Pak.h"

#include "core/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace assets {

const char* pakCodecName(PakCodec c) {
    switch (c) {
        case PakCodec::Lz4: return "lz4";
        default: return "store";
    }
}

// ============================================================
// 编解码
// ============================================================
std::uint64_t fnv1a64(const void* data, std::size_t size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = 1469598103934665603ULL;   // offset basis
    for (std::size_t i = 0; i < size; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 1099511628211ULL;                   // prime
    }
    return h;
}

std::string hex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(v));
    return std::string(buf);
}

// ------------------------------------------------------------ LZ4 block 压缩
//
// 这是**格式兼容**的 LZ4 block（不是 LZ4 frame）：
//   序列 = [token][可选扩展字面量长度][字面量][u16 offset][可选扩展匹配长度]
//   token 高 4 位 = 字面量长度（15 = 需要扩展），低 4 位 = 匹配长度-4（15 = 扩展）
//
// 三条格式约束（写错任何一条，标准 LZ4 解压器会拒绝）：
//   ① offset ∈ [1, 65535]（窗口只有 64 KB）；
//   ② 最后一段**只有字面量**，结尾没有 offset；
//   ③ 最后 5 个字节必须是字面量、最后 12 个字节里不能再起新匹配。
//
// 压缩器是"贪心 + 单探测"（每个位置只查一次哈希表），压缩率不如 zstd，
// 但胜在**四十行就能写对**，而对我们的资产（JSON / vmat / vmesh / r32）
// 已经能拿到 2~3 倍；PNG/JPG 这类压不动的，写完发现更大就改存原文。
bool lz4CompressBlock(const std::string& src, std::string& dst) {
    dst.clear();
    const std::size_t n = src.size();
    if (n == 0) return false;

    constexpr int kHashBits = 15;
    constexpr std::size_t kHashSize = static_cast<std::size_t>(1) << kHashBits;
    std::vector<std::int32_t> table(kHashSize, -1);
    auto hash4 = [&](std::size_t i) {
        std::uint32_t v = 0;
        std::memcpy(&v, src.data() + i, 4);
        return static_cast<std::size_t>((v * 2654435761u) >> (32 - kHashBits));
    };

    const std::size_t matchLimit = (n >= 13) ? (n - 5) : 0;
    const std::size_t searchEnd = (n >= 13) ? (n - 12) : 0;

    std::size_t anchor = 0;
    std::size_t i = 0;
    auto emitLen = [&](std::size_t extra) {
        while (extra >= 255) {
            dst.push_back(static_cast<char>(255));
            extra -= 255;
        }
        dst.push_back(static_cast<char>(extra));
    };

    while (i < searchEnd) {
        const std::size_t h = hash4(i);
        const std::int32_t cand = table[h];
        table[h] = static_cast<std::int32_t>(i);

        bool matched = false;
        std::size_t mlen = 0;
        std::size_t offset = 0;
        if (cand >= 0) {
            const std::size_t c = static_cast<std::size_t>(cand);
            // `c < i` 而不是 `c != i`：同一位置重复命中会让 offset=0，那在
            // LZ4 里是非法的（解压器会当场判坏包）。
            if (c < i && (i - c) <= 65535 &&
                std::memcmp(src.data() + c, src.data() + i, 4) == 0) {
                std::size_t len = 4;
                while (i + len < matchLimit && src[c + len] == src[i + len]) ++len;
                mlen = len;
                offset = i - c;
                matched = true;
            }
        }
        if (!matched) {
            ++i;
            continue;
        }

        const std::size_t litLen = i - anchor;
        const std::size_t matchCode = mlen - 4;
        const std::uint8_t token = static_cast<std::uint8_t>(
            (std::min<std::size_t>(litLen, 15) << 4) |
            std::min<std::size_t>(matchCode, 15));
        dst.push_back(static_cast<char>(token));
        if (litLen >= 15) emitLen(litLen - 15);
        dst.append(src, anchor, litLen);
        dst.push_back(static_cast<char>(offset & 0xFF));
        dst.push_back(static_cast<char>((offset >> 8) & 0xFF));
        if (matchCode >= 15) emitLen(matchCode - 15);

        const std::size_t next = i + mlen;
        // 在 `next-2` 补一个哈希：匹配跳过的区域里往往还有一个可用的起点，
        // 补这一个能让"重复段落"（缩进、相同的键）多命中一次。
        if (next >= 2) table[hash4(next - 2)] = static_cast<std::int32_t>(next - 2);
        i = next;
        anchor = next;
    }

    // 尾段：只有字面量（约束 ②）
    const std::size_t litLen = n - anchor;
    if (litLen > 0) {
        dst.push_back(static_cast<char>(
            std::min<std::size_t>(litLen, 15) << 4));
        if (litLen >= 15) emitLen(litLen - 15);
        dst.append(src, anchor, litLen);
    }
    // 压不小就当没压成（调用方存原文）—— 对已经压缩过的格式（PNG）必然如此。
    return dst.size() < n;
}

bool lz4DecompressBlock(const char* src, std::size_t srcSize,
                        std::size_t rawSize, std::string& dst) {
    dst.clear();
    if (!src && srcSize) return false;
    dst.reserve(rawSize);
    std::size_t i = 0;
    while (i < srcSize) {
        const std::uint8_t token = static_cast<std::uint8_t>(src[i++]);
        std::size_t litLen = token >> 4;
        if (litLen == 15) {
            for (;;) {
                if (i >= srcSize) return false;
                const std::uint8_t b = static_cast<std::uint8_t>(src[i++]);
                litLen += b;
                if (b != 255) break;
            }
        }
        if (i + litLen > srcSize) return false;
        if (dst.size() + litLen > rawSize) return false;
        dst.append(src + i, litLen);
        i += litLen;
        if (i == srcSize) break;   // 最后一段（没有 offset）—— 正常收尾
        if (i + 2 > srcSize) return false;
        const std::size_t offset =
            static_cast<std::size_t>(static_cast<std::uint8_t>(src[i])) |
            (static_cast<std::size_t>(static_cast<std::uint8_t>(src[i + 1])) << 8);
        i += 2;
        if (offset == 0 || offset > dst.size()) return false;
        std::size_t matchLen = token & 0x0F;
        if (matchLen == 15) {
            for (;;) {
                if (i >= srcSize) return false;
                const std::uint8_t b = static_cast<std::uint8_t>(src[i++]);
                matchLen += b;
                if (b != 255) break;
            }
        }
        matchLen += 4;
        if (dst.size() + matchLen > rawSize) return false;
        const std::size_t start = dst.size() - offset;
        // 逐字节复制，**允许重叠**（LZ4 靠重叠表示"一直重复同一个字符"）。
        // 取成局部变量再 push_back：直接把 `dst[start+k]` 传进 push_back 时，
        // 如果内部扩容，引用会悬垂（char 是按值传的，但显式拷贝更不容易被改坏）。
        for (std::size_t k = 0; k < matchLen; ++k) {
            const char ch = dst[start + k];
            dst.push_back(ch);
        }
    }
    return dst.size() == rawSize;
}

// ============================================================
// 读
// ============================================================
namespace {

constexpr char kMagic[4] = {'V', 'P', 'A', 'K'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderSize = 32;

std::uint32_t rdU32(const char* p) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3])) << 24);
}
std::uint64_t rdU64(const char* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint8_t>(p[i]);
    }
    return v;
}
void wrU32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void wrU64(std::string& s, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 路径规范化：反斜杠 → 正斜杠；去掉前导 "./"；拒绝绝对路径与 `..`。
bool normalizeRel(const std::string& raw, std::string& out) {
    out.clear();
    if (raw.empty()) return false;
    std::string s = raw;
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    if (s.empty()) return false;
    // 绝对路径（`C:/`）与上跳（`../`）都不该出现在包内
    if (s.size() >= 2 && s[1] == ':') return false;
    if (s == ".." || s.rfind("../", 0) == 0 ||
        s.find("/../") != std::string::npos) {
        return false;
    }
    out = s;
    return true;
}

} // namespace

Pak::~Pak() { close(); }

void Pak::close() {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_in.is_open()) m_in.close();
    m_index.clear();
    m_entries.clear();
    m_rawBytes = 0;
    m_packedBytes = 0;
    m_path.clear();
    m_open = false;
}

bool Pak::open(const std::string& absPath, std::string& err) {
    close();
    err.clear();

    std::ifstream in(absPath, std::ios::binary);
    if (!in) {
        err = "cannot open " + absPath;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff fileSize = in.tellg();
    in.seekg(0, std::ios::beg);
    if (fileSize < static_cast<std::streamoff>(kHeaderSize)) {
        err = "file too small to be a pak (or empty)";
        return false;
    }

    char hdr[kHeaderSize] = {};
    in.read(hdr, static_cast<std::streamsize>(kHeaderSize));
    if (in.gcount() != static_cast<std::streamsize>(kHeaderSize)) {
        err = "short read on header";
        return false;
    }
    if (std::memcmp(hdr, kMagic, 4) != 0) {
        err = "not a pak file (bad magic)";
        return false;
    }
    const std::uint32_t ver = rdU32(hdr + 4);
    if (ver != kVersion) {
        err = "unsupported pak version " + std::to_string(ver) +
              " (this build reads " + std::to_string(kVersion) + ")";
        return false;
    }
    const std::uint32_t flags = rdU32(hdr + 8);
    if (flags != 0) {
        err = "pak header has unknown flags";
        return false;
    }
    const std::uint32_t count = rdU32(hdr + 12);
    const std::uint64_t indexOff = rdU64(hdr + 16);
    const std::uint64_t indexSize = rdU64(hdr + 24);

    // ---- 与真实文件尺寸对账（读侧永不信任文件）----
    const std::uint64_t fsz = static_cast<std::uint64_t>(fileSize);
    if (indexOff < kHeaderSize || indexOff > fsz ||
        indexSize == 0 || indexOff + indexSize > fsz) {
        err = "index range out of file bounds";
        return false;
    }

    std::string idxText(indexSize, '\0');
    in.seekg(static_cast<std::streamoff>(indexOff), std::ios::beg);
    in.read(&idxText[0], static_cast<std::streamsize>(indexSize));
    if (in.gcount() != static_cast<std::streamsize>(indexSize)) {
        err = "short read on index";
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(idxText);
    } catch (const std::exception& e) {
        err = std::string("index is not valid JSON: ") + e.what();
        return false;
    }
    if (!j.is_object() || !j.contains("entries") || !j["entries"].is_array()) {
        err = "index has no entries array";
        return false;
    }

    std::vector<PakEntry> entries;
    entries.reserve(j["entries"].size());
    std::unordered_map<std::string, std::size_t> index;
    for (const auto& e : j["entries"]) {
        if (!e.is_object()) {
            err = "index entry is not an object";
            return false;
        }
        PakEntry pe;
        pe.path = e.value("p", std::string());
        pe.offset = e.value("o", static_cast<std::uint64_t>(0));
        pe.packedSize = e.value("cs", static_cast<std::uint64_t>(0));
        pe.rawSize = e.value("us", static_cast<std::uint64_t>(0));
        pe.hash = e.value("h", std::string());
        const std::string codec = e.value("c", std::string("store"));
        pe.codec = (codec == "lz4") ? PakCodec::Lz4 : PakCodec::Store;
        if (pe.path.empty()) {
            err = "index entry with empty path";
            return false;
        }
        // 数据必须整个落在"头部之后、索引之前"—— 这条不成立就说明索引被改过
        if (pe.offset < kHeaderSize || pe.offset + pe.packedSize > indexOff) {
            err = "entry data out of bounds: " + pe.path;
            return false;
        }
        // store 的 packedSize 必须等于 rawSize（不然读出来必然长度不符，
        // 早报比晚报好：晚报的表现是"某个贴图加载失败"，查起来慢得多）
        if (pe.codec == PakCodec::Store && pe.packedSize != pe.rawSize) {
            err = "store entry has mismatched sizes: " + pe.path;
            return false;
        }
        const std::string key = lowerAscii(pe.path);
        if (index.find(key) != index.end()) {
            err = "duplicate entry in index: " + pe.path;
            return false;
        }
        index.emplace(key, entries.size());
        entries.push_back(std::move(pe));
    }
    if (static_cast<std::uint32_t>(entries.size()) != count) {
        // 头部与索引对不上：说明这个包被截断过 / 被拼过
        err = "entry count mismatch (header " + std::to_string(count) +
              ", index " + std::to_string(entries.size()) + ")";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_in = std::move(in);
        m_entries = std::move(entries);
        m_index = std::move(index);
        m_path = absPath;
        m_rawBytes = 0;
        m_packedBytes = 0;
        for (const PakEntry& pe : m_entries) {
            m_rawBytes += pe.rawSize;
            m_packedBytes += pe.packedSize;
        }
        m_open = true;
    }
    return true;
}

bool Pak::has(const std::string& rel) const {
    if (!m_open) return false;
    std::string norm;
    if (!normalizeRel(rel, norm)) return false;
    return m_index.find(lowerAscii(norm)) != m_index.end();
}

bool Pak::read(const std::string& rel, std::string& out, std::string& err) {
    out.clear();
    err.clear();
    if (!m_open) {
        err = "pak not open";
        return false;
    }
    std::string norm;
    if (!normalizeRel(rel, norm)) {
        err = "bad relative path: " + rel;
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mu);
    const auto it = m_index.find(lowerAscii(norm));
    if (it == m_index.end()) {
        err = "not in pak: " + norm;
        return false;
    }
    const PakEntry& pe = m_entries[it->second];

    std::string packed(static_cast<std::size_t>(pe.packedSize), '\0');
    m_in.clear();
    m_in.seekg(static_cast<std::streamoff>(pe.offset), std::ios::beg);
    if (pe.packedSize) {
        m_in.read(&packed[0], static_cast<std::streamsize>(pe.packedSize));
        if (m_in.gcount() != static_cast<std::streamsize>(pe.packedSize)) {
            err = "short read on data: " + pe.path;
            return false;
        }
    }

    if (pe.codec == PakCodec::Lz4) {
        if (!lz4DecompressBlock(packed.data(), packed.size(),
                                static_cast<std::size_t>(pe.rawSize), out)) {
            err = "lz4 decode failed: " + pe.path;
            return false;
        }
    } else {
        out = std::move(packed);
    }

    if (out.size() != static_cast<std::size_t>(pe.rawSize)) {
        err = "size mismatch after decode: " + pe.path;
        return false;
    }
    // 哈希校验：少一个字节 / 解错一个字节都要在这里被抓住（静默返回半截数据
    // 的表现是"贴图花屏"，而花屏在日志里没有任何痕迹）。
    if (!pe.hash.empty()) {
        const std::string got = hex64(fnv1a64(out.data(), out.size()));
        if (got != pe.hash) {
            err = "hash mismatch: " + pe.path;
            return false;
        }
    }
    return true;
}

// ============================================================
// 写
// ============================================================
bool writePak(const std::string& absPath, std::vector<PakInput>& files,
              PakWriteStats& stats, std::string& err, bool compress) {
    stats = PakWriteStats{};
    err.clear();

    // ---- 规范化 + 排序 ----
    // 排序不只是"好看"：同样的输入必须产出**同样的字节**，否则
    // "两次构建出的包是不是同一份"就没法用文件哈希回答（CI / 发布对账要用）。
    for (PakInput& f : files) {
        std::string norm;
        if (!normalizeRel(f.path, norm)) {
            err = "bad entry path: " + f.path;
            return false;
        }
        f.path = norm;
    }
    std::sort(files.begin(), files.end(),
              [](const PakInput& a, const PakInput& b) { return a.path < b.path; });
    for (std::size_t i = 1; i < files.size(); ++i) {
        if (lowerAscii(files[i - 1].path) == lowerAscii(files[i].path)) {
            // 大小写不同但归一化后同名：Windows 上解出来会互相覆盖
            err = "duplicate pak path (case-insensitive): " + files[i].path;
            return false;
        }
    }

    const std::string tmp = absPath + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot create " + tmp;
        return false;
    }

    // 先占位 32 字节头（最后回填：条目数 / 索引位置要等写完才知道）
    const std::string zeroHeader(kHeaderSize, '\0');
    out.write(zeroHeader.data(), static_cast<std::streamsize>(kHeaderSize));
    std::uint64_t cursor = kHeaderSize;

    nlohmann::json entries = nlohmann::json::array();
    std::string blob;
    for (const PakInput& f : files) {
        PakCodec codec = PakCodec::Store;
        std::string packed;
        if (compress && f.bytes.size() >= 64) {
            if (lz4CompressBlock(f.bytes, packed)) codec = PakCodec::Lz4;
        }
        const std::string& payload = (codec == PakCodec::Lz4) ? packed : f.bytes;

        if (!payload.empty()) {
            out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            if (!out) {
                err = "write failed at " + f.path;
                out.close();
                std::remove(tmp.c_str());
                return false;
            }
        }
        nlohmann::json e;
        e["p"] = f.path;
        e["o"] = cursor;
        e["cs"] = static_cast<std::uint64_t>(payload.size());
        e["us"] = static_cast<std::uint64_t>(f.bytes.size());
        e["c"] = pakCodecName(codec);
        e["h"] = hex64(fnv1a64(f.bytes.data(), f.bytes.size()));
        entries.push_back(std::move(e));

        cursor += static_cast<std::uint64_t>(payload.size());
        ++stats.files;
        if (codec == PakCodec::Lz4) ++stats.lz4; else ++stats.stored;
        stats.rawBytes += static_cast<std::uint64_t>(f.bytes.size());
        stats.packedBytes += static_cast<std::uint64_t>(payload.size());
        if (static_cast<std::uint64_t>(f.bytes.size()) > stats.largestRaw) {
            stats.largestRaw = static_cast<std::uint64_t>(f.bytes.size());
            stats.largestPath = f.path;
        }
    }

    nlohmann::json idx;
    idx["format"] = "venn.pak";
    idx["version"] = static_cast<int>(kVersion);
    idx["entries"] = std::move(entries);
    std::string idxText = idx.dump();
    const std::uint64_t indexOff = cursor;
    const std::uint64_t indexSize = static_cast<std::uint64_t>(idxText.size());
    out.write(idxText.data(), static_cast<std::streamsize>(idxText.size()));
    if (!out) {
        err = "write failed on index";
        out.close();
        std::remove(tmp.c_str());
        return false;
    }

    // ---- 回填头部 ----
    std::string hdr;
    hdr.append(kMagic, 4);
    wrU32(hdr, kVersion);
    wrU32(hdr, 0);
    wrU32(hdr, static_cast<std::uint32_t>(stats.files));
    wrU64(hdr, indexOff);
    wrU64(hdr, indexSize);
    out.seekp(0, std::ios::beg);
    out.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    out.flush();
    if (!out) {
        err = "write failed on header";
        out.close();
        std::remove(tmp.c_str());
        return false;
    }
    out.close();

    // tmp → 正式名（与出包同一条纪律：半成品比没有更难查）
    std::remove(absPath.c_str());
    if (std::rename(tmp.c_str(), absPath.c_str()) != 0) {
        err = "cannot rename " + tmp + " -> " + absPath;
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace assets
