#include "assetio/Binary.h"

#include <cstring>

namespace assetio {

namespace {

// CRC32 查表（首次调用时生成一次）。生成而非硬编码 256 行表，
// 是为了让这个文件保持"能一眼读完"——表内容是可推导的，
// 抄错一位就会变成一个只在某些字节组合下才暴露的怪 bug。
struct Crc32Table {
    std::uint32_t t[256];
    Crc32Table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};

const Crc32Table& table() {
    static const Crc32Table k;
    return k;
}

} // namespace

std::uint32_t crc32(const void* data, std::size_t n, std::uint32_t seed) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    const Crc32Table& T = table();
    std::uint32_t c = seed ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = T.t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint64_t fnv1a64(const void* data, std::size_t n, std::uint64_t seed) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 0x100000001b3ull;
    }
    return h;
}

// ------------------------------------------------------------ Writer

void Writer::u8(std::uint8_t v) { m_buf.push_back(v); }

void Writer::u16(std::uint16_t v) {
    m_buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    m_buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void Writer::u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        m_buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void Writer::i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }

void Writer::u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        m_buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void Writer::f32(float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    u32(bits);
}

void Writer::bytes(const void* p, std::size_t n) {
    if (!n) return;
    const auto* b = static_cast<const std::uint8_t*>(p);
    m_buf.insert(m_buf.end(), b, b + n);
}

void Writer::str(const std::string& s) {
    u32(static_cast<std::uint32_t>(s.size()));
    bytes(s.data(), s.size());
}

void Writer::align(std::size_t n, std::uint8_t pad) {
    if (n <= 1) return;
    while (m_buf.size() % n) m_buf.push_back(pad);
}

void Writer::patchU32(std::size_t at, std::uint32_t v) {
    if (at + 4 > m_buf.size()) return;
    for (int i = 0; i < 4; ++i)
        m_buf[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

void Writer::patchU64(std::size_t at, std::uint64_t v) {
    if (at + 8 > m_buf.size()) return;
    for (int i = 0; i < 8; ++i)
        m_buf[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

// ------------------------------------------------------------ Reader

bool Reader::need(std::size_t n) {
    if (!m_ok) return false;
    if (n > m_n - m_i) {   // 减法先做，避免 m_i + n 溢出
        m_ok = false;
        return false;
    }
    return true;
}

std::uint8_t Reader::u8() {
    if (!need(1)) return 0;
    return m_p[m_i++];
}

std::uint16_t Reader::u16() {
    if (!need(2)) return 0;
    const std::uint16_t v = static_cast<std::uint16_t>(
        m_p[m_i] | (static_cast<std::uint16_t>(m_p[m_i + 1]) << 8));
    m_i += 2;
    return v;
}

std::uint32_t Reader::u32() {
    if (!need(4)) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(m_p[m_i + i]) << (8 * i);
    m_i += 4;
    return v;
}

std::int32_t Reader::i32() {
    const std::uint32_t u = u32();
    std::int32_t v = 0;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

std::uint64_t Reader::u64() {
    if (!need(8)) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(m_p[m_i + i]) << (8 * i);
    m_i += 8;
    return v;
}

float Reader::f32() {
    const std::uint32_t u = u32();
    float v = 0.0f;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

std::string Reader::str() {
    const std::uint32_t n = u32();
    if (!m_ok || n == 0) return std::string();
    if (!need(n)) return std::string();
    std::string s(reinterpret_cast<const char*>(m_p + m_i), n);
    m_i += n;
    return s;
}

bool Reader::bytes(void* out, std::size_t n) {
    if (!need(n)) return false;
    if (n) std::memcpy(out, m_p + m_i, n);
    m_i += n;
    return true;
}

bool Reader::skip(std::size_t n) {
    if (!need(n)) return false;
    m_i += n;
    return true;
}

} // namespace assetio
