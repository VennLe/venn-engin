#include "animation/BlendSpace.h"

#include <algorithm>
#include <cmath>

namespace animation {

void BlendSpace1D::setSamples(std::vector<BlendSample> samples) {
    // 去掉空 clip 的槽：一个"位置有、动画没有"的样本会把权重分走一块，
    // 结果是一段**不存在**的动画参与混合 —— 表现为主段只出 60% 幅度，
    // 而日志里状态名还是对的（最难查的一类）。
    samples.erase(std::remove_if(samples.begin(), samples.end(),
                                 [](const BlendSample& s) {
                                     return s.clip.empty();
                                 }),
                  samples.end());

    // 稳定排序：`at` 相同的两个样本，先来的留在前面。UE 的编辑器不允许
    // 两点重合；这里不报错也不合并，只保证顺序确定（否则混合结果会随
    // 调用方的插入顺序变，而那是不可复现的）。
    std::stable_sort(samples.begin(), samples.end(),
                     [](const BlendSample& a, const BlendSample& b) {
                         return a.at < b.at;
                     });
    m_samples = std::move(samples);
}

BlendResult BlendSpace1D::evaluate(float x) const {
    BlendResult r;
    r.x = x;
    if (m_samples.empty()) {
        r.lo = r.hi = 0;
        r.wLo = 1.0f;
        return r;
    }
    if (m_samples.size() == 1) {
        r.lo = r.hi = 0;
        r.wLo = 1.0f;
        r.clamped = true;
        return r;
    }

    // 两端：钳制（不外插，理由见头文件）。
    if (x <= m_samples.front().at) {
        r.lo = r.hi = 0;
        r.wLo = 1.0f;
        r.x = m_samples.front().at;
        r.clamped = true;
        return r;
    }
    const int last = static_cast<int>(m_samples.size()) - 1;
    if (x >= m_samples[last].at) {
        r.lo = r.hi = last;
        r.wLo = 1.0f;
        r.x = m_samples[last].at;
        r.clamped = true;
        return r;
    }

    // 找 x 落在哪一段。样本数是个位数（3 段），线性扫比二分更短也更好读。
    for (int i = 0; i < last; ++i) {
        const float a = m_samples[static_cast<size_t>(i)].at;
        const float b = m_samples[static_cast<size_t>(i) + 1].at;
        if (x >= a && x <= b) {
            r.lo = i;
            r.hi = i + 1;
            const float span = b - a;
            // 两点重合（导入的数值恰好相等）：不能除零，全给上面那一段。
            r.wLo = span > 1.0e-6f ? std::clamp((b - x) / span, 0.0f, 1.0f)
                                   : 0.0f;
            return r;
        }
    }
    // 到不了这里（上面两个边界已经覆盖），保险给最后一个样本。
    r.lo = r.hi = last;
    r.wLo = 1.0f;
    return r;
}

int BlendSpace1D::dominantSlot(const BlendResult& r) const {
    if (m_samples.empty()) return kSlotIdle;
    const int i = (r.wLo >= 0.5f) ? r.lo : r.hi;
    const BlendSample& s = m_samples[static_cast<size_t>(i)];
    return s.slot;
}

BlendSpace1D makeGroundBlendSpace(const std::string& idleClip,
                                  const std::string& walkClip,
                                  const std::string& runClip, float walkAt,
                                  float runAt) {
    // `walkAt` 必须 < `runAt`，否则两点重合 —— 混合空间会在一个速度点上
    // 从"纯走路"瞬间变成"纯跑步"，也就是**退回硬切**，但日志里一切正常
    // （state 会照常报 walk→run）。这里夹一个最小间距，宁可让跑步段
    // 稍微宽一点。
    float w = walkAt;
    float r = runAt;
    if (!(w > 0.0f)) w = 1.0f;
    if (!(r > w)) r = w * 1.5f;
    if (!(r > w)) r = w + 1.0f;

    BlendSpace1D bs;
    std::vector<BlendSample> samples;
    samples.push_back({idleClip, 0.0f, kSlotIdle});
    samples.push_back({walkClip, w, kSlotWalk});
    samples.push_back({runClip, r, kSlotRun});
    bs.setSamples(std::move(samples));
    return bs;
}

} // namespace animation
