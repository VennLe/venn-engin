#include "audio/AudioClip.h"

#include "assets/AssetPath.h"
#include "core/Logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

// stb_vorbis 的惯用法：`STB_VORBIS_HEADER_ONLY` 只拿声明，实现在
// `third_party/stb/stb_vorbis.c`（G14-1 起由 CMake 单独编成 C 静态库）。
// 文件自带 `extern "C"` 保护，所以 C++ 侧直接 include 没问题。
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace audio {

// ============================================================
// 小工具
// ============================================================

namespace {

std::uint16_t rdU16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t rdU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string lowerExt(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = path.substr(dot);
    for (char& c : e) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return e;
}

// ------------------------------------------------------------
// WAV（RIFF/WAVE）
// ------------------------------------------------------------
// 支持 PCM 8/16/24/32-bit、IEEE float32、WAVE_FORMAT_EXTENSIBLE。
//
// ⚠ 三个必须写进代码的坑（§G14-1）：
//   ① 24-bit 是**打包的 3 字节**（blockAlign = ch*3）。按 4 字节读会整体
//      错位，听起来是"噪音 + 变调"，但帧数是对的 —— 最难查的那类。
//   ② `data` 的长度可能是 0xFFFFFFFF（流式录制的文件）→ 按"文件剩余字节"
//      兜底，否则会试图读 4 GB。
//   ③ `fmt ` 的 cbSize / EXTENSIBLE 的 subFormat 里前 2 字节才是真正的
//      格式码（1 = PCM，3 = float）。只看 wFormatTag == 0xFFFE 会当成未知。
class WavDecoder final : public ClipDecoder {
public:
    ~WavDecoder() override {
        if (m_f) std::fclose(m_f);
    }

    bool open(const std::string& resolvedPath, std::string& err) override {
        m_f = std::fopen(resolvedPath.c_str(), "rb");
        if (!m_f) {
            err = "open-failed";
            return false;
        }

        unsigned char hdr[12] = {};
        if (std::fread(hdr, 1, 12, m_f) != 12 ||
            std::memcmp(hdr, "RIFF", 4) != 0 ||
            std::memcmp(hdr + 8, "WAVE", 4) != 0) {
            err = "bad-magic";
            return false;
        }

        bool haveFmt = false;
        std::uint64_t dataOffset = 0;
        std::uint64_t dataBytes = 0;
        bool haveData = false;

        // 遍历 chunk。⚠ 长度是奇数时按 RIFF 规范要跳过 1 字节对齐。
        for (;;) {
            unsigned char ch[8] = {};
            if (std::fread(ch, 1, 8, m_f) != 8) break;
            const std::uint32_t size = rdU32(ch + 4);
            const long bodyPos = std::ftell(m_f);
            if (std::memcmp(ch, "fmt ", 4) == 0) {
                if (size < 16) {
                    err = "bad-fmt";
                    return false;
                }
                unsigned char fmt[40] = {};
                const std::uint32_t want = size < sizeof(fmt)
                                               ? size
                                               : static_cast<std::uint32_t>(sizeof(fmt));
                if (std::fread(fmt, 1, want, m_f) != want) {
                    err = "truncated-fmt";
                    return false;
                }
                std::uint16_t tag = rdU16(fmt + 0);
                m_channels = rdU16(fmt + 2);
                m_sampleRate = rdU32(fmt + 4);
                m_bits = rdU16(fmt + 14);
                if (tag == 0xFFFE) {
                    // WAVE_FORMAT_EXTENSIBLE：subFormat GUID 的前两字节是格式码
                    if (size < 40) {
                        err = "bad-extensible";
                        return false;
                    }
                    tag = rdU16(fmt + 24);
                }
                m_isFloat = (tag == 3);
                if (tag != 1 && tag != 3) {
                    err = "unsupported-codec";
                    return false;
                }
                if (m_channels == 0 || m_sampleRate == 0) {
                    err = "bad-fmt-values";
                    return false;
                }
                if (m_bits != 8 && m_bits != 16 && m_bits != 24 && m_bits != 32) {
                    err = "unsupported-bits";
                    return false;
                }
                m_blockAlign = static_cast<std::uint32_t>(m_channels) *
                               ((m_bits + 7) / 8);
                haveFmt = true;
            } else if (std::memcmp(ch, "data", 4) == 0) {
                dataOffset = static_cast<std::uint64_t>(bodyPos);
                if (size == 0xFFFFFFFFu || size == 0) {
                    // ② 0xFFFFFFFF = "一直到文件尾"（流式录制）
                    if (std::fseek(m_f, 0, SEEK_END) != 0) {
                        err = "seek-failed";
                        return false;
                    }
                    const long end = std::ftell(m_f);
                    dataBytes = (end > bodyPos)
                                    ? static_cast<std::uint64_t>(end - bodyPos)
                                    : 0;
                } else {
                    dataBytes = size;
                }
                haveData = true;
                // data 之后通常没有需要的 chunk 了 → 可以停
                if (haveFmt) break;
            }
            // 跳到下一个 chunk（偶数字节对齐）
            const long next = bodyPos + static_cast<long>(size) +
                              (size & 1u);
            if (std::fseek(m_f, next, SEEK_SET) != 0) break;
        }

        if (!haveFmt) {
            err = "no-fmt";
            return false;
        }
        if (!haveData) {
            err = "no-data";
            return false;
        }

        const std::uint32_t bytesPerFrame = m_blockAlign;
        m_dataOffset = dataOffset;
        m_totalFrames = bytesPerFrame ? (dataBytes / bytesPerFrame) : 0;
        m_dataBytes = dataBytes;

        m_format.sampleRate = m_sampleRate;
        m_format.channels = m_channels;
        m_format.frames = m_totalFrames;

        if (!rewind()) {
            err = "rewind-failed";
            return false;
        }
        return true;
    }

    AudioFormat format() const override { return m_format; }

    bool rewind() override {
        if (!m_f) return false;
        return std::fseek(m_f, static_cast<long>(m_dataOffset), SEEK_SET) == 0;
    }

    bool seek(std::uint64_t frame) override {
        if (!m_f) return false;
        if (frame > m_totalFrames) frame = m_totalFrames;
        const std::uint64_t off =
            m_dataOffset + frame * static_cast<std::uint64_t>(m_blockAlign);
        return std::fseek(m_f, static_cast<long>(off), SEEK_SET) == 0;
    }

    std::uint32_t read(float* dst, std::uint32_t frames) override {
        if (!m_f || !dst || frames == 0) return 0;
        if (frames > kMaxBlockFrames) frames = kMaxBlockFrames;

        const std::uint32_t bytes = frames * m_blockAlign;
        m_bytes.resize(bytes);
        const std::size_t got = std::fread(m_bytes.data(), 1, bytes, m_f);
        const std::uint32_t gotFrames =
            m_blockAlign ? static_cast<std::uint32_t>(got / m_blockAlign) : 0;

        const unsigned char* p = m_bytes.data();
        const std::uint32_t n = gotFrames * m_channels;
        if (m_isFloat && m_bits == 32) {
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint32_t u = rdU32(p + i * 4);
                float v;
                std::memcpy(&v, &u, 4);
                dst[i] = v;
            }
        } else if (m_bits == 8) {
            // 8-bit PCM 是**无符号**（128 = 零点），这是唯一一个无符号的。
            for (std::uint32_t i = 0; i < n; ++i)
                dst[i] = (static_cast<float>(p[i]) - 128.0f) / 128.0f;
        } else if (m_bits == 16) {
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto v = static_cast<std::int16_t>(rdU16(p + i * 2));
                dst[i] = static_cast<float>(v) / 32768.0f;
            }
        } else if (m_bits == 24) {
            // ① 3 字节小端打包，符号扩展靠把最高字节当作有符号 char。
            for (std::uint32_t i = 0; i < n; ++i) {
                const unsigned char* q = p + i * 3;
                std::int32_t v = static_cast<std::int32_t>(q[0]) |
                                 (static_cast<std::int32_t>(q[1]) << 8) |
                                 (static_cast<std::int32_t>(
                                      static_cast<std::int8_t>(q[2]))
                                  << 16);
                dst[i] = static_cast<float>(v) / 8388608.0f;
            }
        } else {   // 32-bit int
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto v = static_cast<std::int32_t>(rdU32(p + i * 4));
                dst[i] = static_cast<float>(
                             static_cast<double>(v) / 2147483648.0);
            }
        }
        return gotFrames;
    }

    // 自检用：解码器层面的元信息
    std::uint64_t dataBytes() const { return m_dataBytes; }
    std::uint16_t bits() const { return m_bits; }

private:
    std::FILE* m_f = nullptr;
    AudioFormat m_format;
    std::uint32_t m_channels = 0;
    std::uint32_t m_sampleRate = 0;
    std::uint32_t m_blockAlign = 0;
    std::uint16_t m_bits = 0;
    bool m_isFloat = false;
    std::uint64_t m_dataOffset = 0;
    std::uint64_t m_dataBytes = 0;
    std::uint64_t m_totalFrames = 0;
    std::vector<unsigned char> m_bytes;
};

// ------------------------------------------------------------
// OGG（stb_vorbis）
// ------------------------------------------------------------
class OggDecoder final : public ClipDecoder {
public:
    ~OggDecoder() override {
        if (m_v) stb_vorbis_close(m_v);
    }

    bool open(const std::string& resolvedPath, std::string& err) override {
        int e = 0;
        m_v = stb_vorbis_open_filename(resolvedPath.c_str(), &e, nullptr);
        if (!m_v) {
            err = "vorbis-open-failed";
            return false;
        }
        const stb_vorbis_info info = stb_vorbis_get_info(m_v);
        m_format.sampleRate = info.sample_rate;
        m_format.channels = info.channels;
        // stb_vorbis 的 stream_length_in_samples 在某些文件里是 0（未知）
        const unsigned int len = stb_vorbis_stream_length_in_samples(m_v);
        m_format.frames = len;
        m_knownLength = len > 0;
        return m_format.sampleRate > 0 && m_format.channels > 0;
    }

    AudioFormat format() const override { return m_format; }

    bool rewind() override {
        if (!m_v) return false;
        return stb_vorbis_seek_start(m_v) != 0;
    }

    bool seek(std::uint64_t frame) override {
        if (!m_v) return false;
        return stb_vorbis_seek_frame(m_v, static_cast<unsigned int>(frame)) != 0;
    }

    std::uint32_t read(float* dst, std::uint32_t frames) override {
        if (!m_v || !dst || frames == 0) return 0;
        // interleaved float：frames * channels 个 float
        const int got = stb_vorbis_get_samples_float_interleaved(
            m_v, static_cast<int>(m_format.channels), dst,
            static_cast<int>(frames * m_format.channels));
        return got < 0 ? 0 : static_cast<std::uint32_t>(got);
    }

    bool knownLength() const { return m_knownLength; }

private:
    stb_vorbis* m_v = nullptr;
    AudioFormat m_format;
    bool m_knownLength = false;
};

} // namespace

bool ClipDecoder::seek(std::uint64_t frame) {
    if (!rewind()) return false;
    m_skipped = frame;
    // 默认实现：rewind + 丢弃。仅作兜底 —— WAV / OGG 都覆盖了它。
    std::vector<float> scratch(static_cast<std::size_t>(kMaxBlockFrames) *
                               (format().channels ? format().channels : 1));
    std::uint64_t left = frame;
    while (left > 0) {
        const std::uint32_t want = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(left, kMaxBlockFrames));
        const std::uint32_t got = read(scratch.data(), want);
        if (got == 0) return false;
        left -= got;
    }
    return true;
}

std::unique_ptr<ClipDecoder> makeWavDecoder() {
    return std::make_unique<WavDecoder>();
}

std::unique_ptr<ClipDecoder> makeOggDecoder() {
    return std::make_unique<OggDecoder>();
}

// ============================================================
// AudioClip
// ============================================================

AudioClip::~AudioClip() = default;

bool AudioClip::load(const std::string& relativeOrAbsolutePath, Mode mode,
                     std::string& err) {
    m_path = relativeOrAbsolutePath;
    m_mode = mode;
    m_samples.clear();
    m_decoder.reset();
    err.clear();

    m_resolvedPath = assets::resolveAssetPath(relativeOrAbsolutePath);
    const std::string ext = lowerExt(relativeOrAbsolutePath);
    if (ext == ".ogg" || ext == ".oga") {
        m_decoder = makeOggDecoder();
    } else if (ext == ".wav") {
        m_decoder = makeWavDecoder();
    } else {
        // 不认识的扩展名：不猜。`.vaudio` 这类"资产壳"要显式指定解码器，
        // 否则会变成第二个空挂（§3.11）。
        err = "unknown-extension";
        VK_LOG_WARN("AUDIO-CLIP-FAIL path=%s reason=%s",
                    relativeOrAbsolutePath.c_str(), err.c_str());
        return false;
    }

    if (!m_decoder->open(m_resolvedPath, err)) {
        m_decoder.reset();
        VK_LOG_WARN("AUDIO-CLIP-FAIL path=%s reason=%s",
                    relativeOrAbsolutePath.c_str(), err.c_str());
        return false;
    }
    m_format = m_decoder->format();

    if (mode == Mode::InMemory) {
        // 一次性全解码到交错 float。
        // ⚠ 流式那边**不能**这么做：4 分钟立体声 16-bit = 42 MB。
        const std::uint32_t ch = m_format.channels ? m_format.channels : 1;
        std::vector<float> chunk(static_cast<std::size_t>(kMaxBlockFrames) * ch);
        m_samples.reserve(static_cast<std::size_t>(m_format.frames) * ch);
        for (;;) {
            const std::uint32_t got = m_decoder->read(chunk.data(), kMaxBlockFrames);
            if (got == 0) break;
            m_samples.insert(m_samples.end(), chunk.begin(),
                             chunk.begin() + static_cast<std::ptrdiff_t>(got) * ch);
        }
        // 帧数以**实际解出来的**为准：某些 ogg 的 length 是估计值。
        m_format.frames = m_samples.size() / ch;
        m_decoder->rewind();
    }

    VK_LOG_INFO("AUDIO-CLIP path=%s frames=%llu sr=%u ch=%u bytes=%llu stream=%d",
                m_path.c_str(),
                static_cast<unsigned long long>(m_format.frames),
                m_format.sampleRate, m_format.channels,
                static_cast<unsigned long long>(residentBytes()),
                mode == Mode::Streaming ? 1 : 0);
    return true;
}

std::uint32_t AudioClip::readStreaming(float* dst, std::uint32_t frames) {
    if (!m_decoder || m_mode != Mode::Streaming) return 0;
    return m_decoder->read(dst, frames);
}

bool AudioClip::rewindStreaming() {
    if (!m_decoder || m_mode != Mode::Streaming) return false;
    return m_decoder->rewind();
}

bool AudioClip::seekStreaming(std::uint64_t frame) {
    if (!m_decoder || m_mode != Mode::Streaming) return false;
    return m_decoder->seek(frame);
}

std::uint64_t AudioClip::residentBytes() const {
    if (m_mode == Mode::InMemory) {
        return static_cast<std::uint64_t>(m_samples.size()) * sizeof(float);
    }
    // 流式：常驻只有解码器状态（几 KB）。真正占内存的是 AudioSystem 的
    // 3 s 环形缓冲，那笔账记在 stream 上，不记在 clip 上。
    return 4096;
}

} // namespace audio
