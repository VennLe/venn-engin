#pragma once
// ============================================================
// engine/audio/WasapiBackend —— Windows 设备层（WASAPI shared mode）
//
// 只做四件事：枚举 / 打开默认（或指定名字的）渲染设备 / 按设备的
// **mix format** 协商 / 在独立线程里按事件回调混音。
//
// ⚠⚠ 三条从微软文档里踩出来的硬约束（docs/audio_roadmap.md §G14-3）
//   ① shared mode 下**不要**试图强行设 44.1 kHz。设备只接受它的
//     mix format（通常 float32 / 48 kHz / 立体声）。设失败就走
//     `GetMixFormat`，重采样交给 `Mixer`（§3.9）。
//   ② 事件驱动（`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`）比轮询省 CPU，
//     但事件句柄必须在 `Start()` **之前** `SetEventHandle`，否则
//     `Initialize` 会失败且错误码很难懂。
//   ③ `GetBuffer` / `ReleaseBuffer` 返回 `AUDCLNT_E_DEVICE_INVALIDATED`
//     就是"设备没了"（拔耳机 / 切输出）→ 置 lost 标志，**别在回调里
//     重连**（重连要分配内存，实时线程上不允许）。
// ============================================================

#include "audio/AudioBackend.h"

#if defined(_WIN32)

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace audio {

class WasapiBackend final : public AudioBackend {
public:
    WasapiBackend() = default;
    ~WasapiBackend() override;

    const char* name() const override { return "wasapi"; }

    bool open(const OpenParams& params, MixFn fn, void* user,
              std::string& err) override;
    bool start() override;
    void stop() override;
    void close() override;
    bool isOpen() const override { return m_open; }

    AudioFormat format() const override { return m_format; }
    const std::string& deviceName() const override { return m_deviceName; }
    int enumerate(std::vector<DeviceInfo>& out) const override;
    bool deviceLost() const override { return m_lost.load(); }
    double blockDurationSeconds() const override;

    // 打开时是否回落到了默认设备（"配了名字但没匹配上"的守卫，R9）
    bool fellBack() const { return m_fellBack; }
    // 实际周期（帧数）：用于 AUDIO-STATS 的 cpu 占比
    std::uint32_t periodFrames() const { return m_periodFrames; }

private:
    void threadLoop();
    void releaseAll();

    struct Impl;              // COM 接口藏在 .cpp 里（头文件不 include windows.h）
    Impl* m_impl = nullptr;

    bool m_open = false;
    bool m_fellBack = false;
    AudioFormat m_format;
    std::string m_deviceName = "(none)";
    MixFn m_fn = nullptr;
    void* m_user = nullptr;
    std::uint32_t m_periodFrames = 480;
    std::uint32_t m_bufferFrames = 0;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_lost{false};
    void* m_event = nullptr;      // HANDLE
    std::vector<float> m_convert; // PCM16 → float 的中间缓冲
};

} // namespace audio

#endif // _WIN32
