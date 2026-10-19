#pragma once
// ============================================================
// engine/audio/NullBackend —— 确定性"设备"
//
// 两种用法，**用途完全不同**：
//
//  1. `pump()`（手动）：自检 / 离线验收用。调用一次 = 混一个 block，
//     纯单线程、无时钟、无线程调度 → **逐位可复现**。
//  2. `open()` + 线程模式：冒烟"设备回调路径"用（每 blockFrames 拉一次），
//     ⚠ 因为有时钟参与，**不参与确定性断言**（§G14-2 的验收只认 pump）。
//
// 这个类存在的意义：让"没有声卡的机器"（虚拟机 / 远程桌面 / 回归机）
// 也能把整条链路跑到"输出 float 缓冲"这一步，只差最后写进声卡。
// ============================================================

#include "audio/AudioBackend.h"

#include <atomic>
#include <thread>
#include <vector>

namespace audio {

class NullBackend final : public AudioBackend {
public:
    NullBackend() = default;
    ~NullBackend() override;

    const char* name() const override { return "null"; }

    bool open(const OpenParams& params, MixFn fn, void* user,
              std::string& err) override;
    // 线程模式才是"设备回调路径"的冒烟；手动 pump 不走这里。
    bool start() override;
    void stop() override;
    void close() override;
    bool isOpen() const override { return m_open; }

    AudioFormat format() const override { return m_format; }
    const std::string& deviceName() const override { return m_deviceName; }
    int enumerate(std::vector<DeviceInfo>& out) const override;
    double blockDurationSeconds() const override;

    // ---- 自检专用 ----
    // 混一个 block 到内部缓冲（不启动线程也合法）。返回缓冲首地址。
    // 调用方拿到的指针在下一次 pump 前有效。
    const float* pump(std::uint32_t frames);

    // 线程模式下已经 pump 过的 block 数（冒烟用）。
    std::uint64_t blocksPumped() const { return m_blocks.load(); }

    // 让"线程模式"用：open() 之后默认**不开线程**，显式调用才开。
    // （默认不活跃是刻意的：自检若不小心走了线程，确定性断言会变成 flaky。）
    void startThread();
    void stopThread();

    // 人为把 deviceLost() 置真（模拟拔耳机）。验收里用来打 AUDIO-DEVFAIL 分支。
    void simulateDeviceLost() { m_lost.store(true); }
    bool deviceLost() const override { return m_lost.load(); }

private:
    void threadLoop();

    bool m_open = false;
    AudioFormat m_format;
    std::string m_deviceName = "Null Output";
    MixFn m_fn = nullptr;
    void* m_user = nullptr;
    std::uint32_t m_blockFrames = 480;

    std::vector<float> m_scratch;   // blockFrames * channels
    std::vector<float> m_threadBuf;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_blocks{0};
    std::atomic<bool> m_lost{false};
};

} // namespace audio
