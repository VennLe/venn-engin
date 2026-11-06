#pragma once
// ============================================================
// assetio/ChunkFile —— `.vmesh` / `.vskel` 共用的**容器层**
//
// 为什么抽出来（2026-11-06，UE5 骨架资产化这一刀）：
//   第二个自研格式（`.vskel`）如果自己再写一遍"文件头 + 区块表 + CRC +
//   字符串表"，就有了两份容器实现。这两份的差别会从"排版不一样"长成
//   "一个能跳过未知区块、另一个不能"，而症状是**老版本读到新文件时
//   从中间开始错位** —— 最贵的那种 bug。容器是纯机械的，只该有一个实现。
//
// ------------------------------------------------------------
// 容器结构（两个格式完全一致）
// ------------------------------------------------------------
//   [ 32B 文件头 ]
//   [ N × 32B 区块表 ]
//   [ 区块 0 ][ 区块 1 ] ...     每个 8 字节对齐
//
// 文件头布局（32 B）：
//   magic[8] | version u32 | flags u32 | sourceHash u64 | count u32 | reserved u32
// 区块表项（32 B）：
//   id u32 | version u32 | crc u32 | reserved u32 | offset u64 | size u64
//
// 为什么是"区块表 + 偏移"而不是"顺序拼接"：
//   顺序拼接一旦某个区块的语义扩展了（比如 MESH 里加个字段），
//   老版本读新文件就会从中间开始错位，只能整体报"文件损坏"。
//   有了区块表，读端可以**只取自己认识的区块**，其余跳过 ——
//   这正是"版本不对"和"文件截断"能分开报的前提。
//
// 每个区块带 CRC32：坏一个字节就能立刻定位到**哪个区块**坏了，
// 而不是等到渲染出一堆 NaN 三角形再去猜。
//
// 版本策略：
//   文件头 version  = 容器版本（区块表布局）。变了 = 老读端直接拒。
//   区块头 version  = 该区块自身的布局版本。变了 = 只有用它的人受影响。
// 两者分开，是为了避免"材质加了一个字段 → 所有模型都得重烤"。
// ============================================================

#include "assetio/Binary.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace assetio {

constexpr std::size_t kChunkFileHeaderSize = 32;
constexpr std::size_t kChunkHeaderSize = 32;

// 字符串表里"没有这个串"的哨兵。**不能**用 0：0 是合法的第一个串。
constexpr std::uint32_t kNoString = 0xFFFFFFFFu;

// ------------------------------------------------------------ 字符串表
//
// 为什么要单独一个区块而不是"每个名字就地写一遍"：
//   骨骼名在 joint 里出现 1 次、在动画轨道里却要按 joint 引用；
//   材质名在 submesh 里出现。重复写不但浪费，更要命的是**没法按名字
//   对齐**（比如 `.vmeta` 的材质映射表要按名字查）。
// 去重之后，同名一定是同一个下标，比较成本从 strcmp 降到比整数。
class StringTable {
public:
    std::uint32_t add(const std::string& s) {
        const auto it = m_map.find(s);
        if (it != m_map.end()) return it->second;
        const std::uint32_t idx = static_cast<std::uint32_t>(m_strs.size());
        m_strs.push_back(s);
        m_map.emplace(s, idx);
        return idx;
    }
    const std::vector<std::string>& all() const { return m_strs; }
    std::size_t size() const { return m_strs.size(); }

private:
    std::vector<std::string> m_strs;
    std::unordered_map<std::string, std::uint32_t> m_map;
};

// glm::mat4 是**列主序**，直接按 [col][row] 顺序写 16 个 float。
// 读端按同样顺序填回 —— 中间不做转置，否则往返一次模型就镜像了。
void writeMat4(Writer& w, const glm::mat4& m);
glm::mat4 readMat4(Reader& r);

// 下标 → 字符串。越界或哨兵都返回**空串**（不是抛、不是返回最后一个）：
// 读一个被改坏的文件时"名字是空的"能让人看出是索引坏了，
// 而"返回上一个名字"会让两根源骨莫名其妙同名。
std::string resolveString(const std::vector<std::string>& strs, std::uint32_t idx);

// ------------------------------------------------------------ 区块
struct Chunk {
    std::uint32_t id = 0;
    // 该区块**自身**的布局版本。容器里照抄，不做解释 —— 只有用这个
    // 区块的人知道自己要不要按版本做兼容分支。
    std::uint32_t version = 1;
    std::vector<std::uint8_t> payload;
};

struct ChunkEntry {
    std::uint32_t id = 0;
    std::uint32_t version = 0;
    std::uint32_t crc = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

struct ChunkFileHeader {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint64_t sourceHash = 0;
    std::uint32_t chunkCount = 0;
    std::uint64_t bytes = 0;
};

// 组装容器。区块按传入顺序落盘，每条 8 字节对齐，CRC 在这里算。
std::vector<std::uint8_t> writeChunkFile(const char magic[8],
                                         std::uint32_t fileVersion,
                                         std::uint32_t flags,
                                         std::uint64_t sourceHash,
                                         std::vector<Chunk> chunks);

// 只读头 + 区块表（不解任何区块内容）。
//
// `kind` / `nameOf` 只用来拼错误信息（`bad magic (not a .vskel file)`、
// `section BONES out of bounds`）。`nameOf` 传 nullptr 就退化成打十六进制 id。
// **不设默认值是刻意的**：调用方必须显式说"这是哪种格式"，否则错误信息
// 分不清 `.vmesh` 和 `.vskel`。
bool readChunkTable(const void* data, std::size_t size, const char magic[8],
                    const char* kind, std::uint32_t expectVersion,
                    ChunkFileHeader& hdr, std::vector<ChunkEntry>& out,
                    const char* (*nameOf)(std::uint32_t), std::string* err);

// 逐区块比 CRC。
//
// ⚠ 只校验 CRC 不算完：CRC 只能说明"字节没坏"，不能说明"内容合法"
//   （有人拿 v1 的写端配 v2 的读端，CRC 一样是对的）。所以调用方在
//   解析每个区块时仍然要逐字段做范围检查。
bool verifyChunkCrc(const void* data, std::size_t size,
                    const std::vector<ChunkEntry>& entries,
                    const char* (*nameOf)(std::uint32_t), std::string* err);

} // namespace assetio
