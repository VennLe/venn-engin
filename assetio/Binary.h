#pragma once
// ============================================================
// assetio/Binary —— 最小二进制读写 + 校验和
//
// 只有三个原则，但每一条都是为了"出错时能定位"而不是"先跑起来再说"：
//
//   1. **小端固定**。不写 `if (big endian)` 分支 —— 这套格式只在
//      x64 / arm64 的 Windows/Linux 上用，两端都是小端。真要跨端，
//      在读的时候一次性字节交换，比在几百个读写点上各判一次可靠。
//   2. **Reader 不抛异常**。越界就把 `ok` 置 false 并返回 0，调用方
//      在结尾查一次。理由：读盘代码在"加载一个坏文件"时会被触发，
//      那是**正常路径**（用户可能拷了个半截文件），不该用异常做控制流。
//   3. **每个区块带 CRC32**。`crc32` 是硬件加速之外最便宜的自检手段：
//      能区分"文件被截断"和"内容合法但版本不对"，这两种错误的处理
//      方式完全不同（前者报损坏，后者报版本）。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace assetio {

// 标准 CRC-32（IEEE 802.3，多项式 0xEDB88320，与 zlib 一致）
std::uint32_t crc32(const void* data, std::size_t n, std::uint32_t seed = 0);

// FNV-1a 64 位。与 `assets::fnv1a32` 同一族但位宽不同：这里要撑住
// "源文件改了"的判定，32 位在几十万个文件规模下会撞。
std::uint64_t fnv1a64(const void* data, std::size_t n,
                      std::uint64_t seed = 0xcbf29ce484222325ull);

// ------------------------------------------------------------
// Writer —— 追加到一个内存缓冲
// ------------------------------------------------------------
class Writer {
public:
    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void i32(std::int32_t v);
    void u64(std::uint64_t v);
    void f32(float v);
    void bytes(const void* p, std::size_t n);

    // 长度前缀字符串（u32 长度 + UTF-8 字节，**不带**结尾 0）。
    // 不用 0 结尾是为了让任意字符串（含 `\0`）都能往返。
    void str(const std::string& s);

    // 写到 8 字节边界。区块起始对齐到 8，是为了将来用 memcpy 或
    // 直接 mmap 起 `Vertex[]` 时不会因为对齐问题降级（x64 上不致命，
    // 但 Vulkan 的 buffer offset 有 4 字节对齐要求，留余量总是便宜）。
    void align(std::size_t n, std::uint8_t pad = 0);

    // 回填（写区块头时"先占位、后填偏移"用）
    void patchU32(std::size_t at, std::uint32_t v);
    void patchU64(std::size_t at, std::uint64_t v);

    std::size_t size() const { return m_buf.size(); }
    const std::vector<std::uint8_t>& data() const { return m_buf; }
    std::vector<std::uint8_t> take() { return std::move(m_buf); }

private:
    std::vector<std::uint8_t> m_buf;
};

// ------------------------------------------------------------
// Reader —— 越界不崩，只置 ok=false
// ------------------------------------------------------------
class Reader {
public:
    Reader(const void* data, std::size_t n)
        : m_p(static_cast<const std::uint8_t*>(data)), m_n(n), m_i(0) {}

    bool ok() const { return m_ok; }
    bool eof() const { return m_i >= m_n; }
    std::size_t pos() const { return m_i; }
    std::size_t size() const { return m_n; }

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::int32_t i32();
    std::uint64_t u64();
    float f32();
    std::string str();

    // 批量拷贝：失败时返回 false 且不改动 out 的大小（调用方自己决定
    // 要不要保留部分数据）。n 是从未信任的文件里读出来的，必须先跟
    // 剩余字节数比一遍 —— 否则一个 `0xFFFFFFFF` 长度就是一次 OOM。
    bool bytes(void* out, std::size_t n);
    bool skip(std::size_t n);

private:
    const std::uint8_t* m_p;
    std::size_t m_n;
    std::size_t m_i;
    bool m_ok = true;
    bool need(std::size_t n);
};

} // namespace assetio
