#pragma once
// ============================================================
// scene/CameraPath —— 运镜路径（Follow Path）
//
// 一段三次贝塞尔路径 + **弧长参数化**。求值全是纯函数：`sample(u)` 给定
// 同一个 u 永远返回同一个点，不持有播放头（播放头属于持有它的组件 / 时间轴）。
//
// ------------------------------------------------------------
// ⚠⚠ 为什么一定要弧长参数化（这是本文件唯一值得记住的事）
// ------------------------------------------------------------
// 贝塞尔曲线**不是**匀速的：控制点一密，曲线在这一段的"参数速度"就快。
// 直接 `sample(u = t)` 让 u 线性增长，摄影机在拐弯处会突然窜一下、在长段
// 上又会爬 —— 这正是"路径运镜一顿一顿"最常见的原因，而且**看不出是数学
// 问题**（以为是关键帧太少）。
//
// 所以这里先把曲线按 16 段/段 采样成弧长表，`sample(u)` 里 u 走的是**走过的
// 距离占全长的比例**。改手柄不会改速度曲线（只改形状），这才是运镜要的。
// 判据 `camerapath_arclen_uniform`（自检 E3）用"同一段弧长步长下的实际
// 位移离散度"钉它：均匀参数化时人造对照能到 20%+，弧长参数化 < 1%。
//
// ------------------------------------------------------------
// 手柄约定（与 Blender 的 Bezier 同一口径）
// ------------------------------------------------------------
//   · `outHandle` 相对**本点**，`inHandle` 相对**本点**（都是绝对米数，
//     不是"相对下一个点"—— 相对下一个点的写法在增删点时会整段跳）。
//   · 进/出控制点：`c0 = p[i] + outHandle`，`c1 = p[i+1] + inHandle`。
//   · `smoothHandles()` 用 Catmull-Rom 自动生成手柄（1/6 张力），
//     也就是 Blender 里 "Auto Clamped" 的近似 —— 折线点一下就能变成
//     顺滑路径，作者不用手调。
// ============================================================

#include <glm/glm.hpp>

#include <vector>

namespace scene {

struct CameraPathPoint {
    glm::vec3 position{0.0f};
    glm::vec3 inHandle{0.0f};    // 相对本点，进这段用的控制点偏移
    glm::vec3 outHandle{0.0f};   // 相对本点，出这段用的控制点偏移
};

class CameraPath {
public:
    std::vector<CameraPathPoint> points;
    bool closed = false;

    bool empty() const { return points.size() < 2; }

    // 段数：闭合路径等于点数（最后一段回到起点），开放路径 = 点数 − 1。
    int segmentCount() const;

    // 按 Catmull-Rom 重算所有手柄（清掉手调的值）。
    void smoothHandles();

    // **改了 points / closed 之后必须调**。不调的话弧长表还是旧的
    // （采样位置看着"没跟上"，但拖动点位又确实变了 —— 极难查）。
    void markDirty() { m_len = -1.0f; }

    // 路径总长（米）。首次调用会建弧长表。
    float length() const;

    // u ∈ [0,1] 是**弧长比例**。`tangent` 非空时写入该处的切向（未归一化，
    // 长度为导数模长）。闭合路径 u=1 等价 u=0；开放路径两端夹住。
    glm::vec3 sample(float u, glm::vec3* tangent = nullptr) const;

    // 采样点所在段下标（日志 / 面板高亮用）。
    int segmentAt(float u) const;

private:
    struct Segment {
        glm::vec3 p0, c0, c1, p1;
    };

    void rebuild() const;

    mutable std::vector<Segment> m_segs;
    mutable std::vector<float> m_cum;   // 弧长表：每 kArcSub 步一个累计长度
    mutable float m_len = -1.0f;
};

// 每段在弧长表里的细分步数。
//
// ⚠ 这个数直接决定"匀速"的精度，而且**对速度差大的曲线很敏感**：手柄
//   一短一长时（自检 E1 那条：0.1 m vs 3 m，速度差 30 倍），16 步的
//   表在快端每步跨 0.6 m，步内按直线插值 t 会把速度锯出 10% 的抖动
//   —— 也就是"弧长了，但还是有点一顿一顿"。64 步把它压到 1% 以内。
//   代价只是建表时多算 4 倍（只在路径改动时发生一次），采样仍是二分。
// ⚠ 步数一旦改，`segmentAt` / `sample` 里 `/ kArcSub` 的下标计算自动跟上
//   （都从这个常量推），但**弧长表的长度**会变，别在别处缓存过下标。
constexpr int kArcSub = 64;

} // namespace scene
