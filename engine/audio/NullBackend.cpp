#include "audio/NullBackend.h"

#include <algorithm>
#include <chrono>

namespace audio {

NullBackend::~NullBackend() { close(); }

bool NullBackend::open(const OpenParams& params, MixFn fn, void* user,
                       std::string& err) {
    if (m_open) close();
    if (!fn) {
        err = "no mix callback";
        return false;
    }
    m_fn = fn;
    m_user = user;

    std::uint32_t sr = params.preferSampleRate ? params.preferSampleRate : 48000;
    std::uint32_t ch = params.preferChannels ? params.preferChannels : 2;
    m_format.sampleRate = sr;
    m_format.channels = ch;
    m_format.frames = 0;

    m_blockFrames = params.blockFrames ? params.blockFrames : 480;
    if (m_blockFrames > static_cast<std::uint32_t>(kMaxBlockFrames))
        m_blockFrames = static_cast<std::uint32_t>(kMaxBlockFrames);
    const std::size_t need =
        static_cast<std::size_t>(m_blockFrames) * m_format.channels;
    m_scratch.assign(need, 0.0f);
    m_threadBuf.assign(need, 0.0f);
    m_blocks.store(0);
    m_lost.store(false);
    m_open = true;
    err.clear();
    return true;
}

void NullBackend::close() {
    stopThread();
    m_open = false;
    m_fn = nullptr;
    m_user = nullptr;
}

int NullBackend::enumerate(std::vector<DeviceInfo>& out) const {
    out.clear();
    DeviceInfo d;
    d.name = "Null Output";
    d.id = "null:0";
    d.isDefault = true;
    out.push_back(d);
    return 1;
}

double NullBackend::blockDurationSeconds() const {
    if (!m_format.sampleRate) return 0.0;
    return static_cast<double>(m_blockFrames) /
           static_cast<double>(m_format.sampleRate);
}

const float* NullBackend::pump(std::uint32_t frames) {
    if (!m_open || !m_fn || m_scratch.empty()) return nullptr;
    if (frames > m_blockFrames) frames = m_blockFrames;
    // ⚠ 每次都清零：契约要求后端不假设回调会自己清。这也让"忘了清"这种
    //   经典 bug 在自检里立刻表现为"上一块的声音漏过来"。
    std::fill(m_scratch.begin(), m_scratch.end(), 0.0f);
    m_fn(m_user, m_scratch.data(), frames, m_format.channels);
    m_blocks.fetch_add(1);
    return m_scratch.data();
}

bool NullBackend::start() {
    if (!m_open) return false;
    startThread();
    return true;
}

void NullBackend::stop() { stopThread(); }

void NullBackend::startThread() {
    if (!m_open || m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&NullBackend::threadLoop, this);
}

void NullBackend::stopThread() {
    if (!m_running.load()) {
        if (m_thread.joinable()) m_thread.join();
        return;
    }
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

void NullBackend::threadLoop() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(
        static_cast<long long>(blockDurationSeconds() * 1e9));
    auto next = clock::now();
    while (m_running.load()) {
        std::fill(m_threadBuf.begin(), m_threadBuf.end(), 0.0f);
        if (m_fn) m_fn(m_user, m_threadBuf.data(), m_blockFrames,
                       m_format.channels);
        m_blocks.fetch_add(1);
        next += period;
        const auto now = clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else next = now;   // 落后了就重新对齐，别积累债务
    }
}

} // namespace audio
