#pragma once
// ============================================================
// runtime/GameLoop —— 固定步长 + 相位顺序的游戏循环**调度器**
//
// ------------------------------------------------------------
// 为什么不自己写 `while (!quit)`
// ------------------------------------------------------------
// 宿主循环（窗口事件泵 / 交换链呈现 / 交换链重建 / 启动缓冲消闪白）已经在
// `core::Application::run` 里写好了，而且那些都是**踩过坑才写对的**：
//   · 窗口隐藏创建 + 首帧呈现后才 show（消"启动闪白"）
//   · 最小化时 waitEvents 而不是空转
//   · resize → 交换链重建标志
// 再写一个平行的 while 循环，等于把上面每一条都要重新对一遍。
//
// 所以 GameLoop 的定位是**调度器**：Application 每帧调一次 `tick(rawDt)`，
// 由 GameLoop 决定"这一帧跑几步固定更新、变量更新拿多少 dt、什么时候渲染"。
// 相位顺序被钉死在这里 —— 这是"编辑器里调好了，出包一样"的结构保证。
//
// ------------------------------------------------------------
// 相位顺序（与编辑器 Play 态一致，见 EditorApp::onUpdate）
// ------------------------------------------------------------
//   ① pollInput        事件泵 / 采样输入（每帧一次）
//   ② fixedUpdate      物理 + 玩法逻辑，dt 恒为 fixedTimeStep
//                       —— 编辑器 Play 的顺序是 脚本 → 玩家 → 物理 → 碰撞，
//                          本调度器不掺和其中的**内容**，只保证"每帧该跑够
//                          几步就跑够几步"
//   ③ variableUpdate   相机 / 动画插值 / UI，dt = 帧剩余时间
//   ④ render           只调 Renderer，不碰任何面板
//
// ------------------------------------------------------------
// 累加器（与 physics/PhysicsWorld 同一套约定）
// ------------------------------------------------------------
// 把真实经过的时间攒起来，每攒够一个 fixedTimeStep 跑一步，最多
// maxSubSteps 步（**防死亡螺旋**：帧率掉下去 → 需要更多子步 → 更慢 →
// 掉得更狠。超出上限直接丢掉剩余时间，物理"变慢"但不会把整机拖死）。
// 这套约定与 `PhysicsWorld::step` 完全一致 —— 运行时不另立一套。
//
// ⚠ 时间缩放（timeScale）**只**作用在固定步的推进量上：
//   timeScale = 0 → 一点都不推进（子弹时间 / 暂停），但渲染照常。
// ============================================================

#include <cstdint>
#include <functional>

namespace runtime {

struct LoopConfig {
    // ---- 固定步 ----
    float fixedTimeStep = 1.0f / 60.0f;
    int maxSubSteps = 4;
    // 单帧最多推进多少"缩放后"的时间（秒）。长卡顿之后不该让物理追赶
    // 到几秒钟之后的状态。
    float maxFrameTime = 0.25f;

    // ---- 时间缩放（0 = 停住，1 = 正常，2 = 双倍速）----
    float timeScale = 1.0f;

    // ---- 帧率 ----
    // 0 = 不限（交给交换链的呈现模式）。
    // ⚠ 引擎的呈现模式是 FIFO / MAILBOX 自动选（见 rhi/Swapchain.cpp），
    //   MAILBOX 下 present 并**不**阻塞 —— 所以"别空转"这件事由这里的
    //   帧率上限兜底，不能指望 vsync。
    int maxFps = 60;
    // 窗口失焦时的帧率上限（省电、少发热）。0 = 不降频。
    int unfocusedFps = 15;
};

// 宿主提供的四段回调。GameLoop 只负责"什么时候调"。
struct LoopCallbacks {
    std::function<void()> pollInput;
    std::function<void(float)> fixedUpdate;      // dt == fixedTimeStep
    std::function<void(float)> variableUpdate;   // dt == 帧剩余时间
    std::function<void()> render;
};

struct LoopStats {
    std::uint64_t ticks = 0;          // tick() 调用次数 = 渲染帧数
    std::uint64_t fixedSteps = 0;     // 累计固定步数
    std::uint64_t variableSteps = 0;  // 累计变量更新次数
    std::uint64_t renders = 0;        // 累计渲染次数
    std::uint64_t droppedSteps = 0;   // 因 maxSubSteps 上限被丢弃的步数
    float accumulator = 0.0f;         // 当前累加器余量
    float alpha = 0.0f;               // accumulator / fixedTimeStep（渲染插值用）
    float simTime = 0.0f;             // 累计仿真时间（= fixedSteps × dt）
    float lastScaledDt = 0.0f;        // 上一次 tick 的缩放后 dt
};

class GameLoop {
public:
    GameLoop() = default;
    explicit GameLoop(const LoopConfig& cfg) : m_cfg(cfg) {}

    LoopConfig& config() { return m_cfg; }
    const LoopConfig& config() const { return m_cfg; }

    // 跑一帧。`windowFocused` 只影响帧率上限（后台降频）。
    void tick(float rawDt, bool windowFocused, const LoopCallbacks& cb);

    const LoopStats& stats() const { return m_stats; }
    void resetStats();

    // 每 N 帧打一行 `GAME-LOOP`（0 = 不打）。默认 60。
    void setLogInterval(std::uint64_t frames) { m_logInterval = frames; }

    // 立刻按当前统计打一行（收尾时用）
    void logNow() const;

private:
    void applyFrameLimit(bool focused);

    LoopConfig m_cfg;
    LoopStats m_stats;
    float m_accumulator = 0.0f;
    std::uint64_t m_logInterval = 60;
    std::uint64_t m_framesSinceLog = 0;
    bool m_hasPace = false;
    double m_paceTime = 0.0;   // steady_clock 的秒数
};

} // namespace runtime
