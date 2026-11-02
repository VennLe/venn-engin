#pragma once

#include <cstdint>

namespace core {

// CPU 侧分段计时 —— 与 `render/GpuTimer` 的 GPU 时戳**一一对称**。
//
// 为什么要它：GPU 侧从 R0-1 起就有 11 段时戳 + 堆叠条，CPU 侧只有
// `io.Framerate` 一个总数。于是"这帧 12ms —— 是卡在等 GPU、还是卡在自己
// 录命令、还是卡在场景同步"在界面上无法回答。P1 基线（forward 占 50%、
// 是填充率问题）能测出来靠的是外部工具，无法被"每一次改动"复用。
//
// 设计约束（每一条都决定实现）：
//   · **零分配**：段数据是定长数组，`Scope` 是枚举下标，不做字符串 → 哈希。
//   · **不加锁**：只在主线程（Application::run 的循环体 + drawFrame）打点。
//   · **可缺席**：任何一段没被 begin 就一直是 0，面板照常显示 0.000 ——
//     不需要"这段有没有接上"的分支（少一处判断 = 少一处漂移）。
//   · **不自己结束帧**：`beginFrame()` 在被调用的瞬间把**上一帧**的累积推进
//     历史环，再清零。这样"某一帧中途 break / continue 掉"不会污染历史，
//     也不需要每个调用点记得配对调 `endFrame()`。
class CpuProfiler {
public:
    enum Scope : uint32_t {
        // ---- 顶层的帧分区（五段互不重叠，加起来≈kFrame）----
        kFrame = 0,   // 主循环体（pollEvents 之后到 Input::endFrame 之前）
        kEvents,      // pollEvents + 时间步进
        kScene,       // activeScene().update + LOD 惰性生成
        kLogic,       // onUpdate（脚本 / 物理 / 玩法）
        kUi,          // ImGui 新帧 + 面板构建
        kAssets,      // 纹理流驱动
        kRender,      // drawFrame 整体
        // ---- drawFrame 的细分（互不重叠，加起来≤kRender）----
        // ⚠ kRender 与这五段的差（render − Σdetail）本身就是有价值的读数：
        //   它 = drawFrame 里头尾的检查（尺寸重建 / 描述符重写 / 探针 /
        //   调试日志）—— 这一块一旦变大，通常意味着某条"每帧本该只比对
        //   开关位"的路径在反复重建资源。
        kRenderWait,  // vkWaitForFences：**CPU 在等 GPU**，不是自己在干活
        kSceneSync,   // uploadSceneState + buildDrawLists
        kRecord,      // recordFrame（命令录制 + 各 pass 的 CPU 侧准备）
        kSubmit,      // vkQueueSubmit
        kPresent,     // vkQueuePresentKHR
        kCount
    };

    static constexpr uint32_t kHist = 120;  // 与 Stats 面板的曲线同长

    static CpuProfiler& get();

    // 把上一帧推进历史环并清零（见头注释最后一条）。每帧调一次。
    void beginFrame();

    float lastMs(Scope s) const;
    float avgMs(Scope s) const;
    float maxMs(Scope s) const;
    uint32_t samples() const { return m_samples; }

    static const char* scopeName(Scope s);
    // `kRenderWait` 起是 kRender 的细分（面板据此分成两条）。
    static bool isRenderDetail(Scope s) { return s >= kRenderWait && s < kCount; }

    // 只给 CpuScopeTimer 用。
    void addNs(Scope s, double ns) { m_cur[s] += ns; }

private:
    CpuProfiler() = default;

    double m_cur[kCount] = {};
    float m_hist[kCount][kHist] = {};
    uint32_t m_head = 0;      // 下一条写入位置
    uint32_t m_samples = 0;   // 已推进历史的帧数（≤kHist）
    bool m_hasFrame = false;  // 上一帧有没有真的开始过（首帧前不推进）
};

// RAII 打点。用宏 `VK_CPU_SCOPE(core::CpuProfiler::kRecord)` 保证作用域名唯一。
class CpuScopeTimer {
public:
    explicit CpuScopeTimer(CpuProfiler::Scope s);
    ~CpuScopeTimer();
    CpuScopeTimer(const CpuScopeTimer&) = delete;
    CpuScopeTimer& operator=(const CpuScopeTimer&) = delete;

private:
    CpuProfiler::Scope m_scope;
    long long m_startNs;
};

} // namespace core

#define VK_CPU_SCOPE_CAT_(a, b) a##b
#define VK_CPU_SCOPE_CAT(a, b) VK_CPU_SCOPE_CAT_(a, b)
#define VK_CPU_SCOPE(scope) \
    ::core::CpuScopeTimer VK_CPU_SCOPE_CAT(_vkCpuScope_, __LINE__)(scope)
