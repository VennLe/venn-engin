#pragma once
// ============================================================
// engine/audio/AudioClip —— 文件 → 交错 float32 PCM
//
// 两种模式（docs/audio_roadmap.md §3.8）：
//
//   InMemory   一次性全解码。适用于音效（< 2 s，几百 KB）。
//   Streaming  保留解码器 + 顺序读。适用于音乐 / 长环境音
//              （4 分钟立体声 16-bit = 42 MB，三首就 126 MB 常驻）。
//
// ⚠ **解码永远在主线程**（加载时、或 AudioSystem::update 的预读里）。
//   音频回调里绝不读文件、绝不解码 —— IO 会阻塞回调 → 断音（§3.8）。
//
// ⚠ 这个类**不继承 / 不依赖** `assets::AssetManager`：那里面全是 GPU
//   资源（构造要 VkDevice），塞进来会让 `--selftest audio` 变成"必须先
//   有 Vulkan 设备"（§3.3）。它只是一块 CPU 内存 + 一个解码器。
// ============================================================

#include "audio/AudioFormat.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audio {

// 解码器抽象。WAV 手写（~150 行）、OGG 走 stb_vorbis。
// ⚠ 只在主线程使用；`read()` 允许返回少于请求的帧数（文件尾）。
class ClipDecoder {
public:
    virtual ~ClipDecoder() = default;
    virtual bool open(const std::string& resolvedPath, std::string& err) = 0;
    virtual AudioFormat format() const = 0;
    virtual bool rewind() = 0;
    // 跳到第 frame 帧（流式的 seek）。默认实现 = rewind + 丢弃。
    virtual bool seek(std::uint64_t frame);
    // 读 frames 帧到 dst（交错 float，容量 frames*channels）。
    virtual std::uint32_t read(float* dst, std::uint32_t frames) = 0;

protected:
    std::uint64_t m_skipped = 0;   // 已经"读过但丢弃"的帧（seek 的默认实现用）
};

// 一个已加载的 clip。
class AudioClip {
public:
    enum class Mode { InMemory, Streaming };

    AudioClip() = default;
    ~AudioClip();

    // 按扩展名选解码器。失败返回 false 并填 err（同时打 AUDIO-CLIP-FAIL）。
    bool load(const std::string& relativeOrAbsolutePath, Mode mode,
              std::string& err);

    const AudioFormat& format() const { return m_format; }
    Mode mode() const { return m_mode; }
    const std::string& path() const { return m_path; }
    const std::string& resolvedPath() const { return m_resolvedPath; }

    // InMemory：交错 float PCM（长度 = frames * channels）。
    const std::vector<float>& samples() const { return m_samples; }
    const float* data() const { return m_samples.empty() ? nullptr : m_samples.data(); }

    // Streaming：顺序读。`read` 返回实际帧数（0 = 文件尾 / 未打开）。
    // ⚠ 只允许主线程调用（音频线程读的是环形缓冲，不是这里）。
    std::uint32_t readStreaming(float* dst, std::uint32_t frames);
    bool rewindStreaming();
    bool seekStreaming(std::uint64_t frame);

    // 常驻字节数（InMemory = 样本字节；Streaming = 解码器 + 零常驻 PCM）。
    std::uint64_t residentBytes() const;

    // 是否是无缝循环素材（元信息，仅日志用；循环判定在 voice 上）
    bool isLoopable() const { return m_loopable; }
    void setLoopable(bool v) { m_loopable = v; }

private:
    std::string m_path;
    std::string m_resolvedPath;
    Mode m_mode = Mode::InMemory;
    AudioFormat m_format;
    std::vector<float> m_samples;            // InMemory
    std::unique_ptr<ClipDecoder> m_decoder;  // Streaming
    bool m_loopable = false;
};

// ------------------------------------------------------------
// 解码器工厂（供自检直接测某一种格式，绕开扩展名推断）
// ------------------------------------------------------------
std::unique_ptr<ClipDecoder> makeWavDecoder();
std::unique_ptr<ClipDecoder> makeOggDecoder();

} // namespace audio
