#pragma once
// ============================================================
// engine/audio/AudioFormat —— 音频侧的基础 POD
//
// 这一层**故意**什么依赖都没有（不 include glm、不 include OS 头、不
// include 日志），因为 `Mixer` 要能在无窗口、无声卡、无 GPU 的自检里
// 被完整驱动。能被断言的东西必须住在我们自己的 POD 里 —— 这是
// `docs/audio_roadmap.md` §3.1 定下的第一条纪律（与"采样不许留在
// Renderer 里"同源）。
// ============================================================

#include <cstdint>
#include <cstring>

namespace audio {

// 一路 PCM。**统一是 float32 交错**（解码层解完就转成这个，之后所有
// 环节只认它）—— 省掉"这里 16-bit、那里 float"的一堆分支。
struct AudioFormat {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;   // 1 = mono，2 = stereo
    std::uint64_t frames = 0;     // 每声道采样数

    double durationSeconds() const {
        return sampleRate ? static_cast<double>(frames) /
                                static_cast<double>(sampleRate)
                          : 0.0;
    }
    std::uint64_t totalSamples() const { return frames * channels; }
};

// ------------------------------------------------------------
// 总线
// ------------------------------------------------------------
// 四条：Master（总） / Music（音乐） / SFX（音效） / Voice（人声 / 对话）。
//
// ⚠ `Master` 不作为"声部可挂的 bus"—— 它是汇总级。声部只能挂后三条，
//   但 `AudioSystem` 允许把任意一条的音量设成 0（验收里"SFX=0 而
//   Music 不变"就是靠这个）。
enum class BusId : int {
    Master = 0,
    Music = 1,
    SFX = 2,
    Voice = 3,
    Count = 4,
};

constexpr int kBusCount = static_cast<int>(BusId::Count);
constexpr int kSourceBusCount = kBusCount - 1;   // 声部可挂的条数

inline const char* busName(BusId b) {
    switch (b) {
        case BusId::Master: return "master";
        case BusId::Music:  return "music";
        case BusId::SFX:    return "sfx";
        case BusId::Voice:  return "voice";
        default:            return "?";
    }
}

// 认不出 → 返回 -1。**不静默回落到 SFX**：配置文件里把 bus 拼错
// （"sfxx"）时，我们希望日志里能看到"未知总线"，而不是悄悄变成音效。
inline int busIndexFromName(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < kBusCount; ++i) {
        if (std::strcmp(name, busName(static_cast<BusId>(i))) == 0) return i;
    }
    return -1;
}

inline BusId busFromName(const char* name, BusId fallback = BusId::SFX) {
    const int i = busIndexFromName(name);
    return i < 0 ? fallback : static_cast<BusId>(i);
}

// 声部池上限（G14-2）。64 是 §3.10 定的：同屏真正在响的通常 < 10 路，
// 64 够用且 64 路混音的 CPU 在 Iris Xe 这种机器上可以忽略。
constexpr int kMaxVoices = 64;

// 输出块的最大帧数（预分配用的上界）。WASAPI shared mode 的 period
// 通常是 480 帧（10 ms @48k），这里留 2048 的余量。
constexpr int kMaxBlockFrames = 2048;

} // namespace audio
