#pragma once
// ============================================================
// Time.h —— 帧计时（header-only）
// deltaTime：上一帧耗时（秒）
// elapsed：引擎启动至今总时间（秒）
// fps：每 0.5 秒刷新一次的帧率
// ============================================================

#include <chrono>

namespace core {

class Time {
public:
    void update() {
        auto now = std::chrono::high_resolution_clock::now();
        if (m_hasLast) {
            m_deltaTime = std::chrono::duration<float>(now - m_last).count();
        }
        m_last = now;
        m_hasLast = true;
        m_elapsed += m_deltaTime;
        ++m_frameCount;

        m_accumTime += m_deltaTime;
        ++m_accumFrames;
        if (m_accumTime >= 0.5f) {
            m_fps = static_cast<float>(m_accumFrames) / m_accumTime;
            m_accumTime = 0.0f;
            m_accumFrames = 0;
        }
    }

    float deltaTime() const { return m_deltaTime; }
    float elapsed() const { return m_elapsed; }
    uint64_t frameCount() const { return m_frameCount; }
    float fps() const { return m_fps; }

private:
    std::chrono::high_resolution_clock::time_point m_last{};
    bool m_hasLast = false;
    float m_deltaTime = 0.0f;
    float m_elapsed = 0.0f;
    uint64_t m_frameCount = 0;
    float m_fps = 0.0f;
    float m_accumTime = 0.0f;
    uint32_t m_accumFrames = 0;
};

} // namespace core
