#include "GameLoop.h"

#include "core/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace runtime {

namespace {

using Clock = std::chrono::steady_clock;

double secondsNow() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

} // namespace

void GameLoop::resetStats() {
    m_stats = LoopStats{};
    m_accumulator = 0.0f;
    m_framesSinceLog = 0;
    m_hasPace = false;
}

// ---------------------------------------------------------------- 帧率限制
//
// 放在 tick() 的**开头**：先按上一帧结束到现在的时间差补睡，再干活。
// 这样它等效于"等一个 vblank"，而不是"干完活再睡" —— 后者会让每帧的
// 输入延迟多出一个 sleep 的量。
//
// ⚠ 第一次调用只记时间点、不睡（否则启动第一帧会平白多等一帧）。
void GameLoop::applyFrameLimit(bool focused) {
    const int limit = focused ? m_cfg.maxFps : m_cfg.unfocusedFps;
    if (limit <= 0) {
        m_hasPace = false;
        return;
    }
    const double budget = 1.0 / static_cast<double>(limit);
    const double now = secondsNow();
    if (!m_hasPace) {
        m_hasPace = true;
        m_paceTime = now;
        return;
    }
    const double elapsed = now - m_paceTime;
    if (elapsed < budget) {
        const double sleepFor = budget - elapsed;
        // sleep_until 精度依赖调度器，睡够就好；极短时用 yield 兜住。
        if (sleepFor > 0.001)
            std::this_thread::sleep_for(std::chrono::duration<double>(sleepFor));
        else
            std::this_thread::yield();
        m_paceTime = secondsNow();
    } else {
        m_paceTime = now;
    }
}

// ---------------------------------------------------------------- 一帧

void GameLoop::tick(float rawDt, bool windowFocused, const LoopCallbacks& cb) {
    applyFrameLimit(windowFocused);

    ++m_stats.ticks;

    // ---- ① 事件泵 / 输入采样 ----
    if (cb.pollInput) cb.pollInput();

    // ---- ② 固定步 ----
    // 负数 / NaN 的 rawDt 一律当 0（首帧、调试器断点之后都会遇到）。
    float dt = rawDt;
    if (!(dt > 0.0f)) dt = 0.0f;
    dt = std::min(dt, m_cfg.maxFrameTime);

    m_cfg.timeScale = std::max(0.0f, m_cfg.timeScale);
    m_stats.lastScaledDt = dt * m_cfg.timeScale;

    const float step = (m_cfg.fixedTimeStep > 0.0f) ? m_cfg.fixedTimeStep
                                                    : (1.0f / 60.0f);
    const int maxSteps = std::max(1, m_cfg.maxSubSteps);

    m_accumulator += m_stats.lastScaledDt;

    int steps = 0;
    while (m_accumulator >= step && steps < maxSteps) {
        if (cb.fixedUpdate) cb.fixedUpdate(step);
        m_accumulator -= step;
        ++steps;
        ++m_stats.fixedSteps;
        m_stats.simTime += step;
    }
    // 到上限还有剩余 → 丢掉。物理会"变慢"，但不会把整机拖死（防死亡螺旋）。
    if (m_accumulator >= step) {
        const std::uint64_t dropped =
            static_cast<std::uint64_t>(std::floor(m_accumulator / step));
        m_accumulator = std::fmod(m_accumulator, step);
        m_stats.droppedSteps += dropped;
    }
    // 时间缩放为 0 时不该留下余量（否则恢复时会突然补跑一大堆步）
    if (m_cfg.timeScale <= 0.0f) m_accumulator = 0.0f;

    m_stats.accumulator = m_accumulator;
    m_stats.alpha = step > 0.0f ? (m_accumulator / step) : 0.0f;

    // ---- ③ 变量更新 ----
    if (cb.variableUpdate) cb.variableUpdate(m_stats.lastScaledDt);
    ++m_stats.variableSteps;

    // ---- ④ 渲染 ----
    if (cb.render) cb.render();
    ++m_stats.renders;

    // ---- 定期日志 ----
    if (m_logInterval > 0 && ++m_framesSinceLog >= m_logInterval) {
        m_framesSinceLog = 0;
        logNow();
    }
}

void GameLoop::logNow() const {
    // 格式被 tools/verify_runtime_loop.py 解析（tick/fixed/var/render 四个
    // 字段是**验收条款**，改名字要同步改脚本）。
    VK_LOG_INFO("GAME-LOOP tick=%llu fixed=%llu var=%llu render=%llu "
                "acc=%.4f alpha=%.3f sim=%.3fs scale=%.2f dropped=%llu",
                static_cast<unsigned long long>(m_stats.ticks),
                static_cast<unsigned long long>(m_stats.fixedSteps),
                static_cast<unsigned long long>(m_stats.variableSteps),
                static_cast<unsigned long long>(m_stats.renders),
                static_cast<double>(m_stats.accumulator),
                static_cast<double>(m_stats.alpha),
                static_cast<double>(m_stats.simTime),
                static_cast<double>(m_cfg.timeScale),
                static_cast<unsigned long long>(m_stats.droppedSteps));
}

} // namespace runtime
