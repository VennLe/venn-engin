#pragma once
// ============================================================
// Time.h —— 帧计时（header-only）
// deltaTime：上一帧耗时（秒）
// elapsed：引擎启动至今总时间（秒）
// fps：每 0.5 秒刷新一次的帧率
// ============================================================

#include <chrono>
#include <cstdlib>

namespace core {

class Time {
public:
    Time() {
        // MYVK_FIXED_DT=<秒>：把每帧的 deltaTime 钉成常量。
        //
        // 为什么需要它：**确定性验证**。物理是固定步长 + 累加器的，只要
        // 喂进去的 dt 序列一样，结果就逐位一样。而真实墙钟的 dt 每帧都在
        // 抖 —— 于是"编辑器 Play 与 Game.exe 跑完同一段之后位置相同"这件
        // 事，用真实 dt 根本无法比较。钉成常量之后两边步进序列完全一致，
        // 逐位比较才有意义（见 tools/verify_runtime_determinism.py）。
        //
        // ⚠ 正常使用**不设**即为真实墙钟；它不是给玩家用的旋钮。
        if (const char* s = std::getenv("MYVK_FIXED_DT")) {
            const float v = static_cast<float>(std::atof(s));
            if (v > 0.0f) m_fixedDelta = v;
        }
    }

    void update() {
        auto now = std::chrono::high_resolution_clock::now();
        if (m_fixedDelta > 0.0f) {
            m_deltaTime = m_fixedDelta;
        } else if (m_hasLast) {
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
    // > 0 → 每帧 deltaTime 固定为它（MYVK_FIXED_DT，见构造函数注释）
    float m_fixedDelta = -1.0f;
    float m_elapsed = 0.0f;
    uint64_t m_frameCount = 0;
    float m_fps = 0.0f;
    float m_accumTime = 0.0f;
    uint32_t m_accumFrames = 0;
};

} // namespace core
