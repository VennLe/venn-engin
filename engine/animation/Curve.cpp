#include "animation/Curve.h"

#include <algorithm>
#include <cmath>

namespace animation {

namespace {

constexpr float kPi = 3.14159265358979323846f;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// 标准 bounce-out（四段抛物线）。**"In" 由它镜像出来**，见 inShape()。
float bounceOut(float x) {
    constexpr float n1 = 7.5625f;
    constexpr float d1 = 2.75f;
    if (x < 1.0f / d1) return n1 * x * x;
    if (x < 2.0f / d1) {
        x -= 1.5f / d1;
        return n1 * x * x + 0.75f;
    }
    if (x < 2.5f / d1) {
        x -= 2.25f / d1;
        return n1 * x * x + 0.9375f;
    }
    x -= 2.625f / d1;
    return n1 * x * x + 0.984375f;
}

// 所有形状统一成**"进入"形**（In）。Out/InOut 由它镜像/拼接，
// 这样 7 种形状只需要 7 个公式，而且永远不会出现"In 和 Out 是两套无关实现"。
float inShape(EaseShape s, float x) {
    x = clamp01(x);
    switch (s) {
    case EaseShape::Linear:
        return x;
    case EaseShape::Sine:
        return 1.0f - std::cos(x * kPi * 0.5f);
    case EaseShape::Quad:
        return x * x;
    case EaseShape::Cubic:
        return x * x * x;
    case EaseShape::Back: {
        constexpr float c1 = 1.70158f;
        return (c1 + 1.0f) * x * x * x - c1 * x * x;
    }
    case EaseShape::Bounce:
        // bounce 的"自然"形状是 Out，In 取它的镜像。
        return 1.0f - bounceOut(1.0f - x);
    case EaseShape::Elastic: {
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return 1.0f;
        constexpr float c4 = 2.0f * kPi / 3.0f;
        return -std::pow(2.0f, 10.0f * x - 10.0f) *
               std::sin((10.0f * x - 10.75f) * c4);
    }
    case EaseShape::Smooth:
        // Hermite smoothstep。**InOut 会特判成它本身**（见 easeFn），
        // 这里只是 In 的半段形状。
        return x * x * (3.0f - 2.0f * x);
    }
    return x;
}

// 一段（相邻两帧之间）的全部求值参数。手柄是**相对**偏移：
// outH 相对 (t0,v0)，inH 相对 (t1,v1)。
struct Seg {
    float t0 = 0.0f, v0 = 0.0f, t1 = 1.0f, v1 = 1.0f;
    Interp interp = Interp::Linear;
    EaseMode ease = EaseMode::None;
    EaseShape shape = EaseShape::Linear;
    glm::vec2 outH{1.0f / 3.0f, 0.0f};
    glm::vec2 inH{-1.0f / 3.0f, 0.0f};
};

float evalSegment(const Seg& s, float t) {
    const float span = s.t1 - s.t0;
    if (span <= 1e-9f) return s.v1;   // 重复时刻 → 取右帧（等价硬切）

    const float u = clamp01((t - s.t0) / span);

    // ⚠⚠ Constant **不受 ease 影响**：它的语义是"这一整段都等于左端"，
    //   再乘一个 0..1 的 w 就成了"从左边慢慢挪到右边"，那是 linear。
    if (s.interp == Interp::Constant) return s.v0;

    if (s.interp == Interp::Bezier) {
        // 归一化到 [0,1]：x 上解 u，y 上取三次贝塞尔。
        // 手柄 x 必须钳进 [0,1] —— 否则 x(u) 非单调，解出来的 u 不唯一，
        // 表现是"某一帧曲线突然跳到别处"。
        const float x1 = clamp01(s.outH.x / span);
        const float x2 = clamp01(1.0f + s.inH.x / span);
        const float uu = bezierSolveU(x1, x2, u);
        return bezier1D(s.v0, s.v0 + s.outH.y, s.v1 + s.inH.y, s.v1, uu);
    }

    // Linear + 缓动 = 时间重映射（贝塞尔之外的模式才用这个，见文件头 ⚠⚠）
    const float w = easeFn(s.shape, s.ease, u);
    return s.v0 + (s.v1 - s.v0) * w;
}

// 端点斜率（给 Linear 外推用）。退化时返回 0 —— 宁可"不继续"也不要 NaN。
float endSlope(const Curve& c, bool atEnd) {
    const int n = static_cast<int>(c.keys.size());
    if (n < 2) return 0.0f;
    const int i = atEnd ? n - 2 : 0;
    const float dt = c.keys[static_cast<std::size_t>(i) + 1].time -
                     c.keys[static_cast<std::size_t>(i)].time;
    if (dt <= 1e-9f) return 0.0f;
    return (c.keys[static_cast<std::size_t>(i) + 1].value -
            c.keys[static_cast<std::size_t>(i)].value) /
           dt;
}

float cyclePeriodOf(const Curve& c) {
    if (c.cyclePeriod > 1e-6f) return c.cyclePeriod;
    const float span = c.keys.back().time - c.keys.front().time;
    return span > 1e-6f ? span : 0.0f;
}

float evalInRange(const Curve& c, float t) {
    const int n = static_cast<int>(c.keys.size());
    const int i = segmentIndexAt(c, t);
    if (i < 0) return c.keys.front().value;
    if (i >= n - 1) return c.keys.back().value;

    const CurveKey& a = c.keys[static_cast<std::size_t>(i)];
    const CurveKey& b = c.keys[static_cast<std::size_t>(i) + 1];
    Seg s;
    s.t0 = a.time;
    s.v0 = a.value;
    s.t1 = b.time;
    s.v1 = b.value;
    s.interp = a.interp;   // 插值方式存在**左端**（"从这一帧到下一帧怎么走"）
    s.ease = a.ease;
    s.shape = a.shape;
    s.outH = resolveOutHandle(c, i);
    s.inH = resolveInHandle(c, i + 1);
    return evalSegment(s, t);
}

} // namespace

// ---------------------------------------------------------------- 名字表

const char* interpName(Interp v) {
    switch (v) {
    case Interp::Constant: return "constant";
    case Interp::Linear: return "linear";
    case Interp::Bezier: return "bezier";
    }
    return "linear";
}

const char* easeModeName(EaseMode v) {
    switch (v) {
    case EaseMode::None: return "none";
    case EaseMode::In: return "in";
    case EaseMode::Out: return "out";
    case EaseMode::InOut: return "inout";
    }
    return "none";
}

const char* easeShapeName(EaseShape v) {
    switch (v) {
    case EaseShape::Linear: return "linear";
    case EaseShape::Sine: return "sine";
    case EaseShape::Quad: return "quad";
    case EaseShape::Cubic: return "cubic";
    case EaseShape::Back: return "back";
    case EaseShape::Bounce: return "bounce";
    case EaseShape::Elastic: return "elastic";
    case EaseShape::Smooth: return "smooth";
    }
    return "linear";
}

const char* handleTypeName(HandleType v) {
    switch (v) {
    case HandleType::Auto: return "auto";
    case HandleType::AutoClamped: return "auto_clamped";
    case HandleType::Vector: return "vector";
    case HandleType::Aligned: return "aligned";
    case HandleType::Free: return "free";
    }
    return "auto_clamped";
}

const char* extrapolateName(Extrapolate v) {
    switch (v) {
    case Extrapolate::Hold: return "hold";
    case Extrapolate::Linear: return "linear";
    case Extrapolate::Cycle: return "cycle";
    case Extrapolate::CycleOffset: return "cycle_offset";
    case Extrapolate::CycleFlipped: return "cycle_flipped";
    }
    return "hold";
}

// ⚠ 与 `sequence::parseEase` 同一态度：**不做大小写兼容**。
//   资产里的拼写错误要在加载期报出来，而不是被"智能匹配"吃掉。
bool parseInterp(const std::string& s, Interp& out) {
    if (s == "constant") { out = Interp::Constant; return true; }
    if (s == "linear") { out = Interp::Linear; return true; }
    if (s == "bezier") { out = Interp::Bezier; return true; }
    return false;
}

bool parseEaseMode(const std::string& s, EaseMode& out) {
    if (s == "none") { out = EaseMode::None; return true; }
    if (s == "in") { out = EaseMode::In; return true; }
    if (s == "out") { out = EaseMode::Out; return true; }
    if (s == "inout") { out = EaseMode::InOut; return true; }
    return false;
}

bool parseEaseShape(const std::string& s, EaseShape& out) {
    if (s == "linear") { out = EaseShape::Linear; return true; }
    if (s == "sine") { out = EaseShape::Sine; return true; }
    if (s == "quad") { out = EaseShape::Quad; return true; }
    if (s == "cubic") { out = EaseShape::Cubic; return true; }
    if (s == "back") { out = EaseShape::Back; return true; }
    if (s == "bounce") { out = EaseShape::Bounce; return true; }
    if (s == "elastic") { out = EaseShape::Elastic; return true; }
    if (s == "smooth") { out = EaseShape::Smooth; return true; }
    return false;
}

bool parseHandleType(const std::string& s, HandleType& out) {
    if (s == "auto") { out = HandleType::Auto; return true; }
    if (s == "auto_clamped") { out = HandleType::AutoClamped; return true; }
    if (s == "vector") { out = HandleType::Vector; return true; }
    if (s == "aligned") { out = HandleType::Aligned; return true; }
    if (s == "free") { out = HandleType::Free; return true; }
    return false;
}

bool parseExtrapolate(const std::string& s, Extrapolate& out) {
    if (s == "hold") { out = Extrapolate::Hold; return true; }
    if (s == "linear") { out = Extrapolate::Linear; return true; }
    if (s == "cycle") { out = Extrapolate::Cycle; return true; }
    if (s == "cycle_offset") { out = Extrapolate::CycleOffset; return true; }
    if (s == "cycle_flipped") { out = Extrapolate::CycleFlipped; return true; }
    return false;
}

// ---------------------------------------------------------------- 标量

float bezier1D(float p0, float p1, float p2, float p3, float u) {
    const float v = 1.0f - u;
    return v * v * v * p0 + 3.0f * v * v * u * p1 + 3.0f * v * u * u * p2 +
           u * u * u * p3;
}

float bezierSolveU(float x1, float x2, float x) {
    // Newton 先跑：曲线光滑时 3~4 次就收敛到 1e-7。
    float u = x;
    for (int i = 0; i < 8; ++i) {
        const float xx = bezier1D(0.0f, x1, x2, 1.0f, u);
        const float d = xx - x;
        if (std::fabs(d) < 1e-7f) return clamp01(u);
        // dx/du（对 u 求导）
        const float v = 1.0f - u;
        const float dx = 3.0f * v * v * x1 + 6.0f * v * u * (x2 - x1) +
                         3.0f * u * u * (1.0f - x2);
        if (std::fabs(dx) < 1e-6f) break;   // 切线太平，交给二分
        u = clamp01(u - d / dx);
    }
    // 二分兜底（20 次 → 1e-6）。**不能省**：Newton 在两端手柄几乎竖直时
    // 会来回震荡，而这条路径是每帧都跑的。
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 20; ++i) {
        const float mid = 0.5f * (lo + hi);
        const float xx = bezier1D(0.0f, x1, x2, 1.0f, mid);
        if (xx < x) lo = mid; else hi = mid;
    }
    return 0.5f * (lo + hi);
}

float easeFn(EaseShape shape, EaseMode mode, float u) {
    // 端点短路 + Linear/None 走"原样"：这两条都必须是**逐位恒等**，
    // 否则所有"没配缓动"的段会凭空多出 1e-7 的抖动。
    if (mode == EaseMode::None || shape == EaseShape::Linear) return u;
    u = clamp01(u);
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;

    // ⚠⚠ 迁移锚：`Smooth + InOut` **特判成 Hermite smoothstep 本尊**，
    //    逐位等于 R4 时期 `sequence::smoothstep01()`。
    //    不走下面那个通用 In/Out 拼接，是因为拼出来的形状（半段 ×0.5）
    //    在 u=0.25 处是 0.25 而不是 0.15625 —— 老资产的运镜会整体变形，
    //    而这一层的目的从来不是"改观感"。
    if (shape == EaseShape::Smooth && mode == EaseMode::InOut)
        return u * u * (3.0f - 2.0f * u);

    switch (mode) {
    case EaseMode::In:
        return inShape(shape, u);
    case EaseMode::Out:
        return 1.0f - inShape(shape, 1.0f - u);
    case EaseMode::InOut:
        return u < 0.5f ? 0.5f * inShape(shape, 2.0f * u)
                        : 1.0f - 0.5f * inShape(shape, 2.0f - 2.0f * u);
    default:
        return u;
    }
}

float bezierWeight(float u, glm::vec2 outH, glm::vec2 inH) {
    const float x1 = clamp01(outH.x);
    const float x2 = clamp01(1.0f + inH.x);
    const float uu = bezierSolveU(x1, x2, clamp01(u));
    return bezier1D(0.0f, outH.y, 1.0f + inH.y, 1.0f, uu);
}

float evaluateSegmentEase(Interp interp, EaseMode ease, EaseShape shape,
                          glm::vec2 outH, glm::vec2 inH, float u) {
    Seg s;
    s.t0 = 0.0f;
    s.v0 = 0.0f;
    s.t1 = 1.0f;
    s.v1 = 1.0f;
    s.interp = interp;
    s.ease = ease;
    s.shape = shape;
    s.outH = outH;
    s.inH = inH;
    return evalSegment(s, clamp01(u));
}

// ---------------------------------------------------------------- 曲线

void sortCurve(Curve& c) {
    std::stable_sort(c.keys.begin(), c.keys.end(),
                     [](const CurveKey& a, const CurveKey& b) {
                         return a.time < b.time;
                     });
}

glm::vec2 resolveOutHandle(const Curve& c, int index) {
    const int n = static_cast<int>(c.keys.size());
    if (index < 0 || index >= n) return glm::vec2(0.0f);
    const CurveKey& k = c.keys[static_cast<std::size_t>(index)];

    // Free / Aligned：作者写的（Aligned 的"共线"在求值时由 in 侧对齐到 out）
    if (k.handle == HandleType::Free || k.handle == HandleType::Aligned)
        return k.outHandle;

    const bool hasNext = index + 1 < n;
    const bool hasPrev = index > 0;
    const float dtNext = hasNext
                             ? c.keys[static_cast<std::size_t>(index) + 1].time - k.time
                             : 0.0f;
    const float dtPrev =
        hasPrev ? k.time - c.keys[static_cast<std::size_t>(index) - 1].time : 0.0f;

    if (k.handle == HandleType::Vector) {
        // 指向下一个关键帧 → 这一段退化成直线
        if (hasNext)
            return glm::vec2(dtNext, c.keys[static_cast<std::size_t>(index) + 1].value -
                                         k.value);
        return glm::vec2(-dtPrev, c.keys[static_cast<std::size_t>(index) - 1].value -
                                      k.value);
    }

    // Auto / AutoClamped：中心差分
    float slope = 0.0f;
    if (!hasPrev) {
        slope = hasNext ? (c.keys[1].value - k.value) / std::max(dtNext, 1e-9f) : 0.0f;
    } else if (!hasNext) {
        slope = (k.value - c.keys[static_cast<std::size_t>(index) - 1].value) /
                std::max(dtPrev, 1e-9f);
    } else {
        const float sPrev =
            (k.value - c.keys[static_cast<std::size_t>(index) - 1].value) /
            std::max(dtPrev, 1e-9f);
        const float sNext =
            (c.keys[static_cast<std::size_t>(index) + 1].value - k.value) /
            std::max(dtNext, 1e-9f);
        if (k.handle == HandleType::AutoClamped && sPrev * sNext <= 0.0f)
            slope = 0.0f;   // 局部极值 → 压平，**不会过冲**
        else
            slope = 0.5f * (sPrev + sNext);
    }

    const float dt = hasNext ? dtNext : dtPrev;
    return glm::vec2(dt / 3.0f, slope * dt / 3.0f);
}

glm::vec2 resolveInHandle(const Curve& c, int index) {
    const int n = static_cast<int>(c.keys.size());
    if (index < 0 || index >= n) return glm::vec2(0.0f);
    const CurveKey& k = c.keys[static_cast<std::size_t>(index)];

    if (k.handle == HandleType::Free) return k.inHandle;

    if (k.handle == HandleType::Aligned) {
        // 与 out 共线：方向抄 out，长度用自己的
        const glm::vec2 o = resolveOutHandle(c, index);
        const float len = glm::length(k.inHandle);
        const float olen = glm::length(o);
        if (olen < 1e-6f || len < 1e-6f) return k.inHandle;
        return -glm::normalize(o) * len;
    }

    // Auto/AutoClamped/Vector：把 out 那套斜率镜像到左边
    const glm::vec2 o = resolveOutHandle(c, index);
    const bool hasPrev = index > 0;
    const float dtPrev =
        hasPrev ? k.time - c.keys[static_cast<std::size_t>(index) - 1].time : 0.0f;
    const float dt = hasPrev ? dtPrev : std::fabs(o.x);
    const float slope = std::fabs(o.x) > 1e-9f ? o.y / o.x : 0.0f;
    return glm::vec2(-dt / 3.0f, -slope * dt / 3.0f);
}

void autoHandles(Curve& c) {
    for (int i = 0; i < static_cast<int>(c.keys.size()); ++i) {
        CurveKey& k = c.keys[static_cast<std::size_t>(i)];
        if (k.handle == HandleType::Free) continue;   // 作者写的就是答案
        k.outHandle = resolveOutHandle(c, i);
        if (k.handle != HandleType::Aligned) k.inHandle = resolveInHandle(c, i);
    }
}

int segmentIndexAt(const Curve& c, float t) {
    const int n = static_cast<int>(c.keys.size());
    if (n <= 0) return -1;
    if (t < c.keys.front().time) return -1;
    if (t >= c.keys.back().time) return n - 1;
    // keys 是升序的，二分找最后一个 time <= t
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (c.keys[static_cast<std::size_t>(mid)].time <= t) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

float evaluateCurve(const Curve& c, float t) {
    const int n = static_cast<int>(c.keys.size());
    if (n <= 0) return 0.0f;
    if (n == 1) return c.keys[0].value;

    const float t0 = c.keys.front().time;
    const float t1 = c.keys.back().time;
    if (t >= t0 && t <= t1) return evalInRange(c, t);

    const bool before = t < t0;
    switch (before ? c.before : c.after) {
    case Extrapolate::Hold:
        return before ? c.keys.front().value : c.keys.back().value;

    case Extrapolate::Linear: {
        const float v = before ? c.keys.front().value : c.keys.back().value;
        const float s = endSlope(c, !before);
        return v + s * (before ? (t - t0) : (t - t1));
    }

    case Extrapolate::Cycle:
    case Extrapolate::CycleOffset:
    case Extrapolate::CycleFlipped: {
        const float period = cyclePeriodOf(c);
        if (period <= 1e-6f) return before ? c.keys.front().value : c.keys.back().value;
        float local = std::fmod(t - t0, period);
        if (local < 0.0f) local += period;
        const float cycles = std::floor((t - t0) / period);
        const float u = std::clamp(t0 + local, t0, t1);

        float v = evalInRange(c, u);
        if ((before ? c.before : c.after) == Extrapolate::CycleOffset) {
            v += cycles * (c.keys.back().value - c.keys.front().value);
        } else if ((before ? c.before : c.after) == Extrapolate::CycleFlipped) {
            // 奇数圈绕"两端中点"镜像 —— 呼吸/心跳那种来回
            const float mid = 0.5f * (c.keys.front().value + c.keys.back().value);
            const long long ci = static_cast<long long>(cycles);
            if ((ci % 2) != 0) v = 2.0f * mid - v;
        }
        return v;
    }
    }
    return before ? c.keys.front().value : c.keys.back().value;
}

} // namespace animation
