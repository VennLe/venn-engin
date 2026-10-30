#include "scene/CameraPath.h"

#include <algorithm>
#include <cmath>

namespace scene {

namespace {

glm::vec3 bezier(const glm::vec3& p0, const glm::vec3& c0, const glm::vec3& c1,
                 const glm::vec3& p1, float t) {
    const float mt = 1.0f - t;
    const float a = mt * mt * mt;
    const float b = 3.0f * mt * mt * t;
    const float c = 3.0f * mt * t * t;
    const float d = t * t * t;
    return p0 * a + c0 * b + c1 * c + p1 * d;
}

glm::vec3 bezierTangent(const glm::vec3& p0, const glm::vec3& c0,
                        const glm::vec3& c1, const glm::vec3& p1, float t) {
    const float mt = 1.0f - t;
    return (c0 - p0) * (3.0f * mt * mt) + (c1 - c0) * (6.0f * mt * t) +
           (p1 - c1) * (3.0f * t * t);
}

} // namespace

int CameraPath::segmentCount() const {
    const int n = static_cast<int>(points.size());
    if (n < 2) return 0;
    return closed ? n : n - 1;
}

void CameraPath::smoothHandles() {
    const int n = static_cast<int>(points.size());
    if (n < 2) return;

    auto at = [&](int i) -> int {
        // 闭合时环绕；开放时两端夹住（一端差分 = 单向差分）。
        if (closed) return ((i % n) + n) % n;
        return std::clamp(i, 0, n - 1);
    };

    std::vector<CameraPathPoint> out = points;
    for (int i = 0; i < n; ++i) {
        const glm::vec3 prev = points[static_cast<std::size_t>(at(i - 1))].position;
        const glm::vec3 next = points[static_cast<std::size_t>(at(i + 1))].position;
        // Catmull-Rom：切线 = (next − prev) / 2，手柄取切线的 1/3
        // （三次贝塞尔在 t=0 的导数是 3·(c0−p0)，所以 c0 = p0 + 切线/3）。
        const glm::vec3 tan = (next - prev) * 0.5f;
        out[static_cast<std::size_t>(i)].outHandle = tan / 3.0f;
        out[static_cast<std::size_t>(i)].inHandle = -tan / 3.0f;
    }
    points.swap(out);
    markDirty();
}

void CameraPath::rebuild() const {
    m_segs.clear();
    m_cum.clear();
    m_len = 0.0f;

    const int segs = segmentCount();
    if (segs <= 0) return;

    const int n = static_cast<int>(points.size());
    m_segs.reserve(static_cast<std::size_t>(segs));
    for (int i = 0; i < segs; ++i) {
        const CameraPathPoint& a = points[static_cast<std::size_t>(i)];
        const CameraPathPoint& b =
            points[static_cast<std::size_t>((i + 1) % n)];
        Segment s;
        s.p0 = a.position;
        s.c0 = a.position + a.outHandle;
        s.c1 = b.position + b.inHandle;
        s.p1 = b.position;
        m_segs.push_back(s);
    }

    // 弧长表：逐段 16 步，累计**弦长**（不是解析长度 —— 解析长度对这种
    // 手调手柄的曲线没有闭式解，弦长累加在 16 步下误差已经 < 1 cm）。
    m_cum.reserve(static_cast<std::size_t>(segs * kArcSub + 1));
    glm::vec3 prev = m_segs.front().p0;
    m_cum.push_back(0.0f);
    for (int i = 0; i < segs; ++i) {
        const Segment& s = m_segs[static_cast<std::size_t>(i)];
        for (int k = 1; k <= kArcSub; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(kArcSub);
            const glm::vec3 p = bezier(s.p0, s.c0, s.c1, s.p1, t);
            m_len += glm::length(p - prev);
            prev = p;
            m_cum.push_back(m_len);
        }
    }
}

float CameraPath::length() const {
    if (m_len < 0.0f) rebuild();
    return m_len;
}

int CameraPath::segmentAt(float u) const {
    if (m_len < 0.0f) rebuild();
    if (m_segs.empty()) return 0;

    u = std::clamp(u, 0.0f, 1.0f);
    const float target = u * m_len;
    // 找到 m_cum[i] <= target 的最大 i（线性扫也行，段数是个位数；
    // 用 lower_bound 是为了以后路径变长也不会变成 O(n) 每帧）。
    const auto it = std::upper_bound(m_cum.begin(), m_cum.end(), target);
    int idx = static_cast<int>(it - m_cum.begin()) - 1;
    idx = std::clamp(idx, 0, static_cast<int>(m_cum.size()) - 2);
    return std::clamp(idx / kArcSub, 0, static_cast<int>(m_segs.size()) - 1);
}

glm::vec3 CameraPath::sample(float u, glm::vec3* tangent) const {
    if (m_len < 0.0f) rebuild();
    if (m_segs.empty()) {
        if (tangent) *tangent = glm::vec3(1.0f, 0.0f, 0.0f);
        return points.empty() ? glm::vec3(0.0f) : points.front().position;
    }
    if (m_len <= 1.0e-6f) {
        const Segment& s = m_segs.front();
        if (tangent) *tangent = s.c0 - s.p0;
        return s.p0;
    }

    // 闭合：u=1 与 u=0 是同一个点（否则循环播放时最后一帧会"跳一格"）。
    if (closed) {
        u = u - std::floor(u);
    } else {
        u = std::clamp(u, 0.0f, 1.0f);
    }

    const float target = u * m_len;
    const auto it = std::upper_bound(m_cum.begin(), m_cum.end(), target);
    int idx = static_cast<int>(it - m_cum.begin()) - 1;
    idx = std::clamp(idx, 0, static_cast<int>(m_cum.size()) - 2);

    const int segIdx = std::clamp(idx / kArcSub, 0,
                                  static_cast<int>(m_segs.size()) - 1);
    const int sub = idx - segIdx * kArcSub;
    const float s0 = m_cum[static_cast<std::size_t>(idx)];
    const float s1 = m_cum[static_cast<std::size_t>(idx) + 1];
    const float frac = (s1 > s0) ? (target - s0) / (s1 - s0) : 0.0f;
    const float t = (static_cast<float>(sub) + frac) / static_cast<float>(kArcSub);

    const Segment& s = m_segs[static_cast<std::size_t>(segIdx)];
    if (tangent) *tangent = bezierTangent(s.p0, s.c0, s.c1, s.p1, t);
    return bezier(s.p0, s.c0, s.c1, s.p1, t);
}

} // namespace scene
