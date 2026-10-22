#include "assetio/AnimOps.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

namespace assetio {

float quatAngleDeg(const glm::vec4& a, const glm::vec4& b) {
    // q 与 -q 是**同一个旋转**。不取绝对值的话，"翻转符号"的相邻
    // 关键帧会被算成 180° 误差，于是抽帧器认为"这里变化剧烈"、
    // 一帧都不敢删 —— 日志上表现为"压缩率 0%"，很难看出原因。
    glm::quat qa(a.w, a.x, a.y, a.z);
    glm::quat qb(b.w, b.x, b.y, b.z);
    qa = glm::normalize(qa);
    qb = glm::normalize(qb);
    float d = std::fabs(glm::dot(qa, qb));
    d = std::min(1.0f, std::max(0.0f, d));
    return glm::degrees(2.0f * std::acos(d));
}

namespace {

// 轨道在某时刻的插值值（与 assets::sampleTrack 同语义：
// 旋转用 slerp，其余线性）。抽帧必须用**运行时同一个插值**来评估误差，
// 否则"离线认为误差 0.05°、运行时插出来 2°"。
glm::vec4 lerpValue(const SourceTrack& t, const glm::vec4& a,
                    const glm::vec4& b, float u) {
    if (t.path == 1) {   // Rotation
        const glm::quat qa(a.w, a.x, a.y, a.z);
        const glm::quat qb(b.w, b.x, b.y, b.z);
        const glm::quat q = glm::slerp(qa, qb, u);
        return glm::vec4(q.x, q.y, q.z, q.w);
    }
    return a + (b - a) * u;
}

// 用 i..j 两点插值去近似中间的每一帧，返回最大误差
float maxErrorInSpan(const SourceTrack& t, std::size_t i, std::size_t j,
                     float rotTol, float posTol, float scaleTol,
                     float* worstRotDeg, float* worstPos, float* worstScale) {
    const float t0 = t.times[i];
    const float t1 = t.times[j];
    const float span = t1 - t0;
    float worst = 0.0f;

    for (std::size_t k = i + 1; k < j; ++k) {
        const float u = span > 1.0e-9f ? (t.times[k] - t0) / span : 0.0f;
        const glm::vec4 approx = lerpValue(t, t.values[i], t.values[j], u);
        float err = 0.0f;
        if (t.path == 1) {
            const float deg = quatAngleDeg(t.values[k], approx);
            if (worstRotDeg && deg > *worstRotDeg) *worstRotDeg = deg;
            err = rotTol > 0.0f ? deg / rotTol : (deg > 0.0f ? 1.0e9f : 0.0f);
        } else if (t.path == 2) {   // Scale
            const glm::vec3 a(t.values[k].x, t.values[k].y, t.values[k].z);
            const glm::vec3 b(approx.x, approx.y, approx.z);
            const float d = glm::length(a - b);
            if (worstScale && d > *worstScale) *worstScale = d;
            err = scaleTol > 0.0f ? d / scaleTol : (d > 0.0f ? 1.0e9f : 0.0f);
        } else {                    // Translation
            const glm::vec3 a(t.values[k].x, t.values[k].y, t.values[k].z);
            const glm::vec3 b(approx.x, approx.y, approx.z);
            const float d = glm::length(a - b);
            if (worstPos && d > *worstPos) *worstPos = d;
            err = posTol > 0.0f ? d / posTol : (d > 0.0f ? 1.0e9f : 0.0f);
        }
        if (err > worst) worst = err;
        if (worst > 1.0f) return worst;   // 早退：已经超容差了
    }
    return worst;
}

} // namespace

std::size_t reduceTrack(SourceTrack& t, float rotationToleranceDeg,
                        float positionTolerance, float scaleTolerance,
                        ReduceStats* stats) {
    const std::size_t n = std::min(t.times.size(), t.values.size());
    if (n <= 2) {
        if (stats) stats->keysIn += n, stats->keysOut += n;
        return n;
    }

    std::vector<float> times;
    std::vector<glm::vec4> values;
    times.reserve(n);
    values.reserve(n);

    times.push_back(t.times[0]);
    values.push_back(t.values[0]);

    std::size_t i = 0;
    while (i + 1 < n) {
        // 从 i 出发，找**最远**的 j：i..j 之间所有帧用 i、j 插值都还在
        // 容差内。找不到（j=i+1 也不行）就退化成保留 i+1。
        //
        // 这是 O(n²) 的朴素做法。为什么够用：单条轨道的关键帧数在
        // 30fps × 几秒的量级（几十~几百），而且一旦超容差就早退；
        // 换成"二分 + 单调性"要依赖误差的单调性假设，而 slerp 误差
        // 在有反向旋转的段落上并不严格单调，宁可慢一点也不能删错帧。
        std::size_t best = i + 1;
        float worstRot = 0.0f, worstPos = 0.0f, worstScale = 0.0f;
        for (std::size_t j = i + 2; j < n; ++j) {
            float r = 0.0f, p = 0.0f, s = 0.0f;
            const float err = maxErrorInSpan(t, i, j, rotationToleranceDeg,
                                             positionTolerance, scaleTolerance,
                                             &r, &p, &s);
            if (err > 1.0f) break;   // 再远只会更糟（同一段内误差随 j 增长）
            best = j;
            worstRot = r;
            worstPos = p;
            worstScale = s;
        }

        if (stats) {
            stats->maxRotErrDeg = std::max(stats->maxRotErrDeg, worstRot);
            stats->maxPosErr = std::max(stats->maxPosErr, worstPos);
            stats->maxScaleErr = std::max(stats->maxScaleErr, worstScale);
        }

        // best 的端点必须保留；中间那些被它近似掉的帧就是省下的
        times.push_back(t.times[best]);
        values.push_back(t.values[best]);
        i = best;
    }

    // 末帧必须**逐位**保留：它是"动画结束时的姿势"，被插值近似掉的话
    // 循环点会出现一帧的跳变。
    if (times.back() != t.times[n - 1]) {
        times.push_back(t.times[n - 1]);
        values.push_back(t.values[n - 1]);
    }

    if (stats) {
        stats->keysIn += n;
        stats->keysOut += times.size();
    }
    t.times.swap(times);
    t.values.swap(values);
    return t.times.size();
}

ReduceStats reduceClip(SourceClip& clip, float rotationToleranceDeg,
                       float positionTolerance, float scaleTolerance) {
    ReduceStats st;
    st.tracksIn = clip.tracks.size();
    for (SourceTrack& t : clip.tracks) {
        if (t.times.size() != t.values.size()) {
            // 坏轨道：以较短的为准截齐，别让它带着错位数据落盘
            const std::size_t n = std::min(t.times.size(), t.values.size());
            t.times.resize(n);
            t.values.resize(n);
        }
        reduceTrack(t, rotationToleranceDeg, positionTolerance, scaleTolerance,
                    &st);
    }
    st.tracksOut = clip.tracks.size();
    return st;
}

} // namespace assetio
