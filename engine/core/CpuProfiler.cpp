#include "CpuProfiler.h"

#include <chrono>

namespace core {

namespace {

// steady_clock 在 MSVC 上是 QPC（~20 ns 一对），够用且不需要平台头。
// 刻意**不用** `high_resolution_clock`：它在不同标准库上可能是 system_clock，
// 而 system_clock 会被对时服务往回拨 —— 那会让某一段读出负的毫秒数。
using Clock = std::chrono::steady_clock;

long long nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

const char* const kNames[CpuProfiler::kCount] = {
    "frame",   "events",     "scene",  "logic",  "ui",      "assets",
    "render",  "renderWait", "sync",   "record", "submit",  "present",
};

} // namespace

CpuProfiler& CpuProfiler::get() {
    // 函数内 static：首次调用时才构造。引擎里没有"谁先初始化"的保证
    // （Application 的静态初始化顺序不确定），放在这里最省心。
    static CpuProfiler s;
    return s;
}

void CpuProfiler::beginFrame() {
    if (m_hasFrame) {
        for (uint32_t s = 0; s < kCount; ++s) {
            m_hist[s][m_head] = static_cast<float>(m_cur[s] / 1.0e6);
        }
        m_head = (m_head + 1) % kHist;
        if (m_samples < kHist) ++m_samples;
    }
    for (uint32_t s = 0; s < kCount; ++s) m_cur[s] = 0.0;
    m_hasFrame = true;
}

float CpuProfiler::lastMs(Scope s) const {
    if (s >= kCount || m_samples == 0) return 0.0f;
    const uint32_t i = (m_head + kHist - 1) % kHist;
    return m_hist[s][i];
}

float CpuProfiler::avgMs(Scope s) const {
    if (s >= kCount || m_samples == 0) return 0.0f;
    double sum = 0.0;
    for (uint32_t i = 0; i < m_samples; ++i) sum += m_hist[s][i];
    return static_cast<float>(sum / m_samples);
}

float CpuProfiler::maxMs(Scope s) const {
    if (s >= kCount || m_samples == 0) return 0.0f;
    float m = 0.0f;
    for (uint32_t i = 0; i < m_samples; ++i) {
        if (m_hist[s][i] > m) m = m_hist[s][i];
    }
    return m;
}

const char* CpuProfiler::scopeName(Scope s) {
    return s < kCount ? kNames[s] : "?";
}

CpuScopeTimer::CpuScopeTimer(CpuProfiler::Scope s)
    : m_scope(s), m_startNs(nowNs()) {}

CpuScopeTimer::~CpuScopeTimer() {
    const long long d = nowNs() - m_startNs;
    // 理论上不会负（steady_clock 单调），但真出现也不要把负数喂进累积 ——
    // 一个负值会让 max 永远算对、avg 被悄悄拉低，属于最难查的那种。
    if (d > 0) CpuProfiler::get().addNs(m_scope, static_cast<double>(d));
}

} // namespace core
