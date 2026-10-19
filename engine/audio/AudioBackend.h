#pragma once
// ============================================================
// engine/audio/AudioBackend —— 设备层抽象（**唯一**允许碰 OS 音频 API 的接缝）
//
// 为什么要有这层接口（docs/audio_roadmap.md §3.4）：
// 本机取不到外网 → miniaudio 下不来 → 选"自研 WASAPI"。但设备层必须是
// 可替换的：将来要跨平台 / 想用 miniaudio 的解码器，只多实现一个类，
// `Mixer` 一行不动。
//
// ------------------------------------------------------------
// 契约（三条，破了就会在低负载下看起来正常、掉帧时爆）
// ------------------------------------------------------------
//  1. `open()` 之后，`MixFn` 会被**设备线程**周期调用。回调里**不许**
//     分配内存、不许加锁、不许打日志、不许碰 World（§3.5 铁律）。
//  2. 回调必须把 `frames * channels` 个 float **全部写满**（后端不预清零）。
//  3. `close()` 返回之后保证不会再有一次回调进来（内部要 join 线程）。
// ============================================================

#include "audio/AudioFormat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace audio {

// 设备混音回调：用户数据 + 输出缓冲 + 帧数 + 输出声道数。
// ⚠ 用裸函数指针而不是 std::function：std::function 的调用在部分实现里
//   会多一层间接 + 可能捕获堆对象，实时线程上不想要这个不确定性。
using MixFn = void (*)(void* user, float* out, std::uint32_t frames,
                       std::uint32_t outChannels);

struct DeviceInfo {
    std::string name;        // 设备友好名（"扬声器 (Realtek(R) Audio)"）
    std::string id;          // 后端内部 id（WASAPI 是 device id 串）
    bool isDefault = false;
};

struct OpenParams {
    // 期望值。⚠ 只是"期望"：shared mode 下设备只接受它的 mix format，
    // 后端必须用实际协商到的格式回报（§3.9 强调过），重采样由 Mixer 负责。
    std::uint32_t preferSampleRate = 48000;
    std::uint32_t preferChannels = 2;
    // 空 / "default" = 默认设备；否则按友好名或 id **子串**匹配。
    std::string deviceName = "default";
    std::uint32_t blockFrames = 480;   // NullBackend 的 pump 粒度（WASAPI 忽略）
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    // 后端名（"wasapi" / "null"），进 `AUDIO-INIT backend=`。
    virtual const char* name() const = 0;

    // 打开设备。失败返回 false 并填 err（不抛异常 —— 没声卡是正常工况）。
    // ⚠ 失败**不是错误**：调用方进入 Silent 模式继续跑（§4.2）。
    virtual bool open(const OpenParams& params, MixFn fn, void* user,
                      std::string& err) = 0;

    // 真正开始/停止回调。
    // ⚠ 为什么 open 和 start 要分开：`AudioSystem` 必须**先**拿到设备的
    //   实际格式（shared mode 下通常是 48 kHz float32，不是我们想要的），
    //   再按那个格式 init `Mixer`。如果 open 里就直接起线程，第一块会拿
    //   着一个还没 init 的 Mixer 去混 —— 这是"偶尔开头有一声爆音"的来源。
    virtual bool start() { return true; }
    virtual void stop() {}

    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // 实际协商到的格式（frames 字段无意义，填 0）。
    virtual AudioFormat format() const = 0;

    // 实际打开的设备的友好名（进 `AUDIO-INIT device=`）。
    virtual const std::string& deviceName() const = 0;

    // 枚举渲染设备（默认项 isDefault=true）。失败返回 0 条。
    virtual int enumerate(std::vector<DeviceInfo>& out) const = 0;

    // 是否检测到设备已失效（拔耳机 / 被独占）。轮询式，不阻塞。
    // 返回 true 之后调用方应 close() 并尝试重开（§4.2 的 RETRY）。
    virtual bool deviceLost() const { return false; }

    // 一个 block 的时长（秒），用于 AUDIO-STATS 的 cpu 占比。
    virtual double blockDurationSeconds() const = 0;
};

} // namespace audio
