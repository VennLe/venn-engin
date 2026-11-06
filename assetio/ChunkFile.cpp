#include "assetio/ChunkFile.h"

#include <cstdio>
#include <cstring>

namespace assetio {

void writeMat4(Writer& w, const glm::mat4& m) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) w.f32(m[c][r]);
}

glm::mat4 readMat4(Reader& r) {
    glm::mat4 m(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) m[c][row] = r.f32();
    return m;
}

std::string resolveString(const std::vector<std::string>& strs, std::uint32_t idx) {
    if (idx == kNoString || idx >= strs.size()) return std::string();
    return strs[idx];
}

std::vector<std::uint8_t> writeChunkFile(const char magic[8],
                                         std::uint32_t fileVersion,
                                         std::uint32_t flags,
                                         std::uint64_t sourceHash,
                                         std::vector<Chunk> chunks) {
    Writer w;
    w.bytes(magic, 8);
    w.u32(fileVersion);
    w.u32(flags);
    w.u64(sourceHash);
    w.u32(static_cast<std::uint32_t>(chunks.size()));
    w.u32(0);   // reserved（将来可能放"编译器版本"）

    const std::size_t tableStart = w.size();
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        w.u32(chunks[i].id);
        w.u32(chunks[i].version);   // 区块版本（调用方给，容器不解释）
        w.u32(0);                   // crc（回填）
        w.u32(0);                   // reserved
        w.u64(0);                   // offset（回填）
        w.u64(0);                   // size（回填）
    }

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        w.align(8);
        const std::uint64_t off = w.size();
        w.bytes(chunks[i].payload.data(), chunks[i].payload.size());
        const std::uint64_t sz = w.size() - off;
        const std::uint32_t crc =
            crc32(chunks[i].payload.data(), chunks[i].payload.size());
        const std::size_t e = tableStart + i * kChunkHeaderSize;
        w.patchU32(e + 8, crc);
        w.patchU64(e + 16, off);
        w.patchU64(e + 24, sz);
    }
    return w.take();
}

bool readChunkTable(const void* data, std::size_t size, const char magic[8],
                    const char* kind, std::uint32_t expectVersion,
                    ChunkFileHeader& hdr, std::vector<ChunkEntry>& out,
                    const char* (*nameOf)(std::uint32_t), std::string* err) {
    out.clear();
    hdr = ChunkFileHeader{};

    if (size < kChunkFileHeaderSize) {
        if (err) *err = "file too small for header";
        return false;
    }
    Reader r(data, size);
    char got[8] = {};
    if (!r.bytes(got, 8)) {
        if (err) *err = "truncated magic";
        return false;
    }
    if (std::memcmp(got, magic, 8) != 0) {
        if (err) *err = std::string("bad magic (not a ") + kind + " file)";
        return false;
    }
    hdr.version = r.u32();
    hdr.flags = r.u32();
    hdr.sourceHash = r.u64();
    hdr.chunkCount = r.u32();
    r.u32();   // reserved
    hdr.bytes = size;

    if (hdr.version != expectVersion) {
        if (err) {
            *err = "unsupported container version " +
                   std::to_string(hdr.version) + " (expect " +
                   std::to_string(expectVersion) + ")";
        }
        return false;
    }
    // 区块数上限：32 字节一个区块头，再多的文件一定是坏的（或恶意）。
    if (hdr.chunkCount == 0 || hdr.chunkCount > 64) {
        if (err) *err = "bad section count";
        return false;
    }
    const std::size_t need =
        kChunkFileHeaderSize +
        static_cast<std::size_t>(hdr.chunkCount) * kChunkHeaderSize;
    if (need > size) {
        if (err) *err = "truncated section table";
        return false;
    }

    out.reserve(hdr.chunkCount);
    for (std::uint32_t i = 0; i < hdr.chunkCount; ++i) {
        ChunkEntry e;
        e.id = r.u32();
        e.version = r.u32();
        e.crc = r.u32();
        r.u32();   // reserved
        e.offset = r.u64();
        e.size = r.u64();
        if (e.offset > size || e.size > size - e.offset) {
            if (err) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "section %s out of bounds",
                              nameOf ? nameOf(e.id) : "?");
                *err = buf;
            }
            return false;
        }
        out.push_back(e);
    }
    return true;
}

bool verifyChunkCrc(const void* data, std::size_t size,
                    const std::vector<ChunkEntry>& entries,
                    const char* (*nameOf)(std::uint32_t), std::string* err) {
    (void)size;
    for (const ChunkEntry& e : entries) {
        const std::uint32_t crc =
            crc32(static_cast<const std::uint8_t*>(data) + e.offset,
                  static_cast<std::size_t>(e.size));
        if (crc != e.crc) {
            if (err) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "section %s crc mismatch (file corrupt)",
                              nameOf ? nameOf(e.id) : "?");
                *err = buf;
            }
            return false;
        }
    }
    return true;
}

} // namespace assetio
