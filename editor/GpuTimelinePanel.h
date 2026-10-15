#pragma once
// ============================================================
// editor/GpuTimelinePanel —— E25：GPU 时戳时间轴（对标 UE GPU Profiler）
//
// 与 Stats 面板（E15）的关系 —— 两者**不重复**：
//   · Stats 的堆叠条是"**本帧** 10 段按占比铺一条"，看的是"谁最贵"；
//   · 本面板是"**最近 60 帧**的甘特堆叠"，看的是"**抖不抖**"。
//     单帧的占比再准，也回答不了"这一帧的 forward 突然涨了 3ms 是
//     偶发还是常态" —— 那需要纵向时间轴。
//
// ------------------------------------------------------------
// 数据从哪来（R0-1 的 GpuTimer 早就够了）
// ------------------------------------------------------------
// GpuTimer 只保留 `lastMs[]`（最近一帧）—— 它自己**不存历史**（存了就
// 要跟 in-flight 对齐，是它不该管的事）。所以历史由本面板自己按帧采样：
// 每帧一次 `sample()`，写进 60 槽环形缓冲。这样：
//   · 面板关着也在积累（EditorApp 无条件采样），打开即见完整历史；
//   · 采样是**纯读**，不碰 GpuTimer，也不影响画面（与 R0-1 的
//     "时戳只写 query pool" 同一条纪律）。
//
// ------------------------------------------------------------
// "时间轴总长"到底等于什么（验收条款的落点）
// ------------------------------------------------------------
// GpuTimer 的 `lastTotalMs()` 是**首 begin → 末 end 的跨度**；而甘特图上
// 能画的只有 10 段的**各自时长**（绝对起点没暴露）。于是：
//
//     lastTotal = seg_sum + gap        （gap ≥ 0 = 段与段之间的空隙）
//
// 验收要求"时间轴总长与 GPU-TIMING 日志的帧总耗时一致（误差 < 5%）"。
// 本面板把两件事都报出来（`GPU-TIMELINE` 日志里的 `seg_sum` 与 `last`），
// 断言 `0 ≤ gap/last < 5%` —— 这比"让总长恒等于 total"有信息量：
// 它同时证明了**各段确实覆盖了整帧**，没有大片没被计时的黑洞。
// gap 本身也画成时间轴右端的一条灰色"间隙"。
//
// ------------------------------------------------------------
// 点击某段 → 联动 Console（路线图的第 3 条改动点）
// ------------------------------------------------------------
// 段名恰好是**日志里会出现的词**（`forward` / `shadows` / `local_sh`…）——
// 所以"联动"就是给 Console 的过滤框填上段名。不发明新协议，用现成的
// 那一个搜索框（`ConsolePanel::setSearchFilter`）。
// ============================================================

#include <cstdint>

namespace render {
class GpuTimer;
}

namespace editor {

class ConsolePanel;

// 一帧的时戳快照（拷贝 —— 不持有 GpuTimer 的引用，因为它每帧被重写）。
struct TimelineSample {
    float total = 0.0f;   // 首 begin → 末 end（= GPU-TIMING 的 frame 值）
    float segSum = 0.0f;  // 11 段时长之和（= 甘特图可画部分）
    float ms[11] = {};    // 逐段毫秒
};

class GpuTimelinePanel {
public:
    static constexpr int kMaxHistory = 60;  // 路线图原文："最近 60 帧"
    static constexpr int kSections = 11;    // GpuTimer::kSectionCount

    GpuTimelinePanel() = default;

    // 每帧一次。gt == nullptr / !valid() / 首帧无数据 → 什么都不做
    // （历史里不会插一条全 0 的假帧）。
    void sample(const render::GpuTimer* gt);

    // 面板绘制。console 非空时，"点击某段"会去设置它的日志过滤。
    // 传引用而不是在内部找 ConsolePanel —— 面板之间不互相引用是硬约束，
    // 所以联动由宿主（EditorApp）在**两边的入口**上接一根线。
    void draw(bool* open, ConsolePanel* console);

    // ---------------- 自动化 / 断言接口 ----------------
    int   historyCount() const { return m_count; }
    float lastTotal() const;      // 最近一帧 total
    float lastSegSum() const;     // 最近一帧 segSum
    float lastGap() const;        // total - segSum
    float averageTotal() const;   // 历史平均帧耗时
    float maxTotal() const;       // 历史最慢帧
    int   hottestSection() const; // 最近一帧最贵的段；无数据 -1
    int   selectedSection() const { return m_selected; }
    const char* selectedName() const;  // 未选中返回 "-"

    // 选中一段（click 为真时打 GPU-TIMELINE-CLICK，并记下"待联动的过滤词"）
    void selectSection(int s, bool click);

    // 取走待联动的过滤词（取一次清一次）。宿主在 Console 就绪时调用。
    // 返回 nullptr = 没有待处理请求。
    const char* consumeFilterRequest();

    // MYVK_EDITOR_GPU_TIMELINE=1 时每 30 帧一行 key=value 快照（脚本正则截取）。
    void logSnapshot() const;

    // 自动化钩子：MYVK_EDITOR_GPUTIMELINE=1（开面板）、
    // MYVK_EDITOR_GPUTIMELINE_CLICK=<段名>（第 50 帧点它）。
    void driveHooks(int editorFrame);

    // 第 i 个历史样本（0 = 最旧）。越界返回 nullptr。
    const TimelineSample* historyAt(int i) const;

private:
    TimelineSample m_hist[kMaxHistory];
    int m_count = 0;   // 已填样本数（≤ kMaxHistory）
    int m_head = 0;    // 下一个写入槽（环形）
    int m_selected = -1;
    // 待联动的过滤词（selectSection(click=true) 时写，宿主取走即清）
    const char* m_filterReq = nullptr;
    int m_clickHookDone = -1;  // MYVK_EDITOR_GPUTIMELINE_CLICK 的一次性哨兵
    bool m_autoOpenDone = false;
    uint64_t m_snapTick = 0;
};

// ConsolePanel 的前置声明够了（draw 只调一个成员函数）；
// 这里把"联动"的签名固定下来，避免 EditorApp 到处写字符串。
void applyTimelineFilter(editor::ConsolePanel* console, const char* text);

}  // namespace editor
