#pragma once
// ============================================================
// assets/Pak —— 资产包容器（G29-b）
//
// ------------------------------------------------------------
// 为什么需要它：出包现在是 300+ 个散文件
// ------------------------------------------------------------
// 散文件目录在开发机上看不出问题，但发出去就是三种麻烦：
//   · 拷贝 / 上传慢（300 个小文件的元数据开销远大于 1 个文件）；
//   · 杀毒软件逐个扫描，首次启动要等好几秒；
//   · 谁都可能改 / 删其中一个 —— 缺一个就是黑屏，而且从文件列表上看不出来。
//
// 打成单个 `.pak` 之后：文件数 3 个（exe + pak + build.json），
// 索引与数据在同一份字节里，**清单即真相**（build.json 的哈希可以逐条对账）。
//
// ------------------------------------------------------------
// 格式 v1（全部小端）
// ------------------------------------------------------------
//   ┌ 文件头 32 字节 ───────────────────────────────────────┐
//   │  0..3   'V' 'P' 'A' 'K'                               │
//   │  4..7   u32 version   （= 1）                          │
//   │  8..11  u32 flags     （保留，必须为 0）                │
//   │ 12..15  u32 entryCount                                 │
//   │ 16..23  u64 indexOffset                                │
//   │ 24..31  u64 indexSize                                  │
//   └───────────────────────────────────────────────────────┘
//   [条目数据 0] [条目数据 1] … [索引 JSON]
//
// 索引是 **JSON**，不是二进制表。理由：
//   · 30 万字节的索引对 38 MB 的包可以忽略；
//   · 出问题时 `python -c` 就能读出来（二进制表要另写解析器）；
//   · 加减字段不需要升版本（读侧只要求认识的字段在）。
//     但**格式语义**变了（比如路径含义）必须升 version。
//   条目形状：{"p":"textures/a.png","o":32,"cs":123,"us":456,
//              "c":"lz4","h":"<fnv1a64 hex>"}
//     p  = pak 根下的相对路径（正斜杠，大小写按原样保存）
//     o  = 数据起始偏移      cs = 压缩后字节    us = 原始字节
//     c  = 编解码（"store" / "lz4"）
//     h  = 原始内容的 FNV-1a 64（**解压后**算的，用来对账"解出来的对不对"）
//
// ------------------------------------------------------------
// 三条纪律
// ------------------------------------------------------------
// ① **读侧永不信任文件**：所有长度都要和真实文件尺寸对账，越界即拒绝。
//    资产包是"别人给的字节"，坏包必须**报错**而不是崩在解压里。
// ② **解压后校验哈希**：少一个字节都要报错。静默返回半截数据 = 贴图花屏，
//    而花屏在日志里没有任何痕迹。
// ③ **压不动就存原文**（`store`）：PNG/JPG 这类已经压过的东西再压只会变大。
// ============================================================

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace assets {

// ---------------------------------------------------------------- 编解码
// 暴露出来是为了能**单独测**：容器的 bug 和编解码的 bug 症状完全不同
// （前者是"文件找不到"，后者是"解出来是乱码"），混在一起查会很慢。
enum class PakCodec : std::uint8_t {
    Store = 0,   // 原样
    Lz4 = 1,     // LZ4 block（本文件自带，不引外部依赖）
};
const char* pakCodecName(PakCodec c);

// LZ4 block 压缩。压完不比原文小就返回 false（调用方改存原文）。
// ⚠ LZ4 的块格式要求"最后 5 字节必须是字面量"，所以尾部永远不参与匹配。
bool lz4CompressBlock(const std::string& src, std::string& dst);

// LZ4 block 解压。`rawSize` 必须精确等于原始长度（我们自己的容器里记着它），
// 解出的字节数不符、或任何越界读写 → 返回 false。
bool lz4DecompressBlock(const char* src, std::size_t srcSize,
                        std::size_t rawSize, std::string& dst);

// FNV-1a 64（与 build.json 的 `hashKind` 同一算法）。
std::uint64_t fnv1a64(const void* data, std::size_t size);
std::string hex64(std::uint64_t v);

// ---------------------------------------------------------------- 条目
struct PakEntry {
    std::string path;          // pak 根下相对路径（正斜杠）
    std::uint64_t offset = 0;  // 数据在 pak 里的起始偏移
    std::uint64_t packedSize = 0;
    std::uint64_t rawSize = 0;
    PakCodec codec = PakCodec::Store;
    std::string hash;          // 原始内容的 FNV-1a 64（hex）
};

// ---------------------------------------------------------------- 读
class Pak {
public:
    Pak() = default;
    ~Pak();
    Pak(const Pak&) = delete;
    Pak& operator=(const Pak&) = delete;

    // 打开并校验。失败时 `err` 是人话（"不是 pak 文件" / "索引越界" …）。
    bool open(const std::string& absPath, std::string& err);
    void close();
    bool isOpen() const { return m_open; }
    const std::string& path() const { return m_path; }

    bool has(const std::string& rel) const;
    // 读出**原始**内容（内部解压 + 哈希校验）。失败返回 false 并写 err。
    bool read(const std::string& rel, std::string& out, std::string& err);

    const std::vector<PakEntry>& entries() const { return m_entries; }
    int fileCount() const { return static_cast<int>(m_entries.size()); }
    std::uint64_t rawBytes() const { return m_rawBytes; }
    std::uint64_t packedBytes() const { return m_packedBytes; }

private:
    // 大小写不敏感查表：Windows 上资产文件名的大小写经常对不上
    // （`.PNG` vs `.png`）。**打包时保存原始大小写**，查找时归一化。
    std::unordered_map<std::string, std::size_t> m_index;
    std::vector<PakEntry> m_entries;
    mutable std::ifstream m_in;
    mutable std::mutex m_mu;   // 资源加载将来可能并行；一次 seek+read 必须原子
    std::string m_path;
    std::uint64_t m_rawBytes = 0;
    std::uint64_t m_packedBytes = 0;
    bool m_open = false;
};

// ---------------------------------------------------------------- 写
struct PakInput {
    std::string path;    // pak 根下相对路径（正斜杠）
    std::string bytes;   // 原始内容
};
struct PakWriteStats {
    int files = 0;
    int stored = 0;
    int lz4 = 0;
    std::uint64_t rawBytes = 0;
    std::uint64_t packedBytes = 0;
    // 最大的单个条目（用来解释"为什么这个包还是很大"）
    std::string largestPath;
    std::uint64_t largestRaw = 0;
};

// 写一个 pak。`compress=false` 时全部 store（排查"是不是压缩坏了"用）。
// ⚠ 先写 `<out>.tmp` 再 rename —— 与出包同一条纪律：半成品比没有更难查。
bool writePak(const std::string& absPath, std::vector<PakInput>& files,
              PakWriteStats& stats, std::string& err, bool compress = true);

// 自检（`Game.exe --selftest pak`）：编解码往返 / 容器往返 / 坏包拒绝。
// 放在这里而不是 project/，因为它测的是**容器本身**，与出包流程无关；
// 出包那边只用它，不该反过来被它约束。
int runPakSelfTest();

} // namespace assets
