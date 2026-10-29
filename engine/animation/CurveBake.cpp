// ============================================================
// animation/CurveBake.cpp —— 骨骼轨道 ⇄ 曲线
//
// 头文件里四条约定（逐位不变 / 旋转两条路 / 插值属于键 / 写回不增删帧）
// 的实现都在这里。下面只记实现层面必须知道的几件事。
//
// ⚠ `evaluateCurve` 的 Linear 分支是 `v0 + (v1-v0)*w`，而
//   `assets::sampleTrack` 是 `a + (b-a)*u` —— **同一个式子、同一份 u**
//   （两边的 u 都是 `(t - t0)/(t1 - t0)`，且 `easeFn(None)` 逐位返回 u）。
//   所以"接了曲线但没设缓动"的平移/缩放轨是**逐位**相同的，不是"差不多"。
//   这条等式很脆：谁要是把 `evalSegment` 改成 `glm::mix(v0,v1,w)`
//   （`a*(1-w)+b*w`），它就断了，而且没有任何画面变化能提示你 ——
//   只有 `curvebake_default_bitwise` 会红。
// ============================================================

#include "animation/CurveBake.h"

#include "core/Logger.h"

#include <algorithm>
#include <cmath>

namespace animation {

namespace {

// 写回后再烘一遍做自检时用的采样点数（含两端）。
constexpr int kVerifySamples = 33;

// `EditTrack` → `assets::AnimTrack` 的只读视图。**故意返回一份拷贝**：
// 两者字段不同（joint 是名字 vs 下标），拿引用就得改类型。
assets::AnimTrack runtimeViewOf(const EditTrack& tr) {
    assets::AnimTrack t;
    t.joint = -1;          // 名字在下游解析，这里不关心
    t.path = tr.path;
    t.times = tr.times;
    t.values = tr.values;
    t.step = tr.step;
    return t;
}

// FNV-1a 64 —— 摘要用（跨进程比对的唯一依据）。
inline void feed(unsigned long long& h, const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
}
inline void feedF(unsigned long long& h, float v) { feed(h, &v, sizeof(v)); }
inline void feedI(unsigned long long& h, int v) { feed(h, &v, sizeof(v)); }

} // namespace

// ---------------------------------------------------------------- 分量

int pathComponentCount(assets::AnimTrack::Path p) {
    return p == assets::AnimTrack::Rotation ? 4 : 3;
}

const char* pathComponentName(assets::AnimTrack::Path p, int comp) {
    if (p == assets::AnimTrack::Rotation) {
        switch (comp) {
            case 0: return "x";
            case 1: return "y";
            case 2: return "z";
            case 3: return "w";
            default: return "?";
        }
    }
    switch (comp) {
        case 0: return "x";
        case 1: return "y";
        case 2: return "z";
        default: return "?";
    }
}

const char* pathName(assets::AnimTrack::Path p) {
    switch (p) {
        case assets::AnimTrack::Translation: return "translation";
        case assets::AnimTrack::Rotation:    return "rotation";
        case assets::AnimTrack::Scale:       return "scale";
    }
    return "rotation";
}

// ---------------------------------------------------------------- 缓动表

void resolveEaseTable(const EditTrack& tr, std::vector<KeyEase>& out) {
    const int n = tr.keyCount();
    out.assign(static_cast<std::size_t>(std::max(n, 0)), KeyEase{});
    for (int i = 0; i < n; ++i) {
        out[static_cast<std::size_t>(i)] = tr.easeAt(i);
        // step 轨道的兜底：**只在"这一键没被人动过"时**才强制常量。
        // 动过（比如设成贝塞尔）的键按作者写的来 —— 那也是
        // `applyCurveToTrack` 会顺手把 `tr.step` 清掉的情形，两处一致。
        if (tr.step && out[static_cast<std::size_t>(i)].isDefault())
            out[static_cast<std::size_t>(i)].interp = Interp::Constant;
    }
}

// ---------------------------------------------------------------- 烘焙

Curve bakeTrackCurve(const EditTrack& tr, int comp) {
    Curve c;
    c.before = Extrapolate::Hold;    // = `sampleTrack` 的端点语义
    c.after = Extrapolate::Hold;
    c.cyclePeriod = 0.0f;            // 0 → 用首末帧推算（这里不循环）

    const int comps = pathComponentCount(tr.path);
    if (comp < 0 || comp >= comps) return c;

    std::vector<KeyEase> ease;
    resolveEaseTable(tr, ease);

    const int n = tr.keyCount();
    c.keys.resize(static_cast<std::size_t>(std::max(n, 0)));
    for (int i = 0; i < n; ++i) {
        CurveKey& k = c.keys[static_cast<std::size_t>(i)];
        k.time = tr.times[static_cast<std::size_t>(i)];
        k.value = tr.values[static_cast<std::size_t>(i)]
                           [static_cast<glm::length_t>(comp)];
        const KeyEase& e = ease[static_cast<std::size_t>(i)];
        k.interp = e.interp;
        k.ease = e.ease;
        k.shape = e.shape;
        k.handle = e.handle;
        k.outHandle = e.outHandle;
        k.inHandle = e.inHandle;
    }
    // `EditTrack::times` 本来就升序（`setKey` 用 upper_bound 插），写回之后
    // 可能出现"拖动留下的乱序"。这里**不排序**：写回是按下标一一对应的
    // （约定 ④），排序会让下标与轨道对不上。乱序是暂时的，
    // `CurveEditorPanel` 松手时 `sortAndRemap()` 一次性收拾。
    return c;
}

void bakeResolvedTrack(const EditTrack& src, ResolvedTrack& rt) {
    rt.comps = pathComponentCount(rt.track.path);
    resolveEaseTable(src, rt.ease);

    bool any = false;
    for (const KeyEase& e : rt.ease) {
        if (!e.isDefault()) { any = true; break; }
    }
    // ⚠ `rt.track.step` 也算"要走曲线"：step 的常量语义已经由
    //   `resolveEaseTable` 落进 interp=Constant，那之后就是曲线的活了。
    //   不算进来的话，"step 轨道上作者又给某一键设了贝塞尔"会静默失效。
    rt.hasEase = any || rt.track.step;

    rt.curves.clear();
    if (!rt.hasEase || rt.track.times.empty()) return;

    // 用**已经解析好的**缓动表烘，避免 `bakeTrackCurve` 再解析一遍时
    // 对 step 兜底产生分歧。
    EditTrack view;
    view.path = rt.track.path;
    view.times = rt.track.times;
    view.values = rt.track.values;
    view.step = false;            // 已折进 ease，别再让兜底插一手
    view.ease = rt.ease;
    rt.curves.resize(static_cast<std::size_t>(rt.comps));
    for (int c = 0; c < rt.comps; ++c)
        rt.curves[static_cast<std::size_t>(c)] = bakeTrackCurve(view, c);
}

// ---------------------------------------------------------------- 采样

namespace {

// 旋转 + 缓动。约定 ②：Linear 段走 slerp（缓动恒等时**转发** sampleTrack），
// Bezier 段逐分量解再归一。
glm::vec4 sampleRotationEased(const ResolvedTrack& rt, float time) {
    const std::vector<float>& T = rt.track.times;
    const int n = static_cast<int>(T.size());
    if (n == 0) return glm::vec4(0.0f);
    if (time <= T.front()) return rt.track.values.front();
    if (time >= T.back()) return rt.track.values.back();

    const auto it = std::upper_bound(T.begin(), T.end(), time);
    const int i1 = static_cast<int>(it - T.begin());
    const int i0 = i1 - 1;
    if (i0 < 0 || i1 >= n) return assets::sampleTrack(rt.track, time);

    const KeyEase& e = rt.ease[static_cast<std::size_t>(i0)];
    if (e.interp == Interp::Constant)
        return rt.track.values[static_cast<std::size_t>(i0)];

    if (e.interp == Interp::Bezier) {
        if (rt.curves.size() < 4) return assets::sampleTrack(rt.track, time);
        glm::vec4 v(0.0f);
        for (int c = 0; c < 4; ++c)
            v[static_cast<glm::length_t>(c)] =
                evaluateCurve(rt.curves[static_cast<std::size_t>(c)], time);
        glm::quat q(v.w, v.x, v.y, v.z);
        const float len = glm::length(q);
        if (len > 1.0e-8f)
            return glm::vec4(q.x / len, q.y / len, q.z / len, q.w / len);
        return rt.track.values[static_cast<std::size_t>(i0)];
    }

    const float span = T[static_cast<std::size_t>(i1)] -
                       T[static_cast<std::size_t>(i0)];
    const float u =
        span > 1.0e-9f ? (time - T[static_cast<std::size_t>(i0)]) / span : 0.0f;
    const float w = easeFn(e.shape, e.ease, u);
    // ⚠ 恒等缓动 → 走原路。**这一句就是"逐位不变"的全部依据**：
    //   不是"算出来恰好一样"，而是"根本没另算"。
    if (w == u) return assets::sampleTrack(rt.track, time);

    const glm::vec4& a = rt.track.values[static_cast<std::size_t>(i0)];
    const glm::vec4& b = rt.track.values[static_cast<std::size_t>(i1)];
    const glm::quat qa(a.w, a.x, a.y, a.z);
    const glm::quat qb(b.w, b.x, b.y, b.z);
    const glm::quat qr = glm::slerp(qa, qb, w);
    return glm::vec4(qr.x, qr.y, qr.z, qr.w);
}

} // namespace

glm::vec4 sampleResolvedTrack(const ResolvedTrack& rt, float time) {
    if (rt.track.times.empty()) return glm::vec4(0.0f);
    // 约定 ①：没缓动 → 一个浮点运算都不多做。
    if (!rt.hasEase || rt.curves.empty()) return assets::sampleTrack(rt.track, time);
    if (rt.track.path == assets::AnimTrack::Rotation)
        return sampleRotationEased(rt, time);

    glm::vec4 out(0.0f);
    const int comps = std::min(rt.comps, static_cast<int>(rt.curves.size()));
    for (int c = 0; c < comps; ++c)
        out[static_cast<glm::length_t>(c)] =
            evaluateCurve(rt.curves[static_cast<std::size_t>(c)], time);
    return out;
}

glm::vec4 sampleEditTrack(const EditTrack& tr, float time) {
    // 约定 ① 的第一道闸门：**表里没有非默认项就直接转发**。
    // 注意 `tr.step` 的情形也走这里 —— `sampleTrack` 本来就懂 step。
    if (!tr.hasEasing()) return assets::sampleTrack(runtimeViewOf(tr), time);
    ResolvedTrack rt;
    rt.track = runtimeViewOf(tr);
    bakeResolvedTrack(tr, rt);
    return sampleResolvedTrack(rt, time);
}

// ---------------------------------------------------------------- 写回

CurveWriteStats applyCurveToTrack(EditTrack& tr, int comp, const Curve& c) {
    CurveWriteStats st;
    const int comps = pathComponentCount(tr.path);
    const int n = tr.keyCount();
    st.keys = static_cast<int>(c.keys.size());

    if (comp < 0 || comp >= comps) {
        ++st.skipped;
        return st;
    }
    // 约定 ④：不增删帧。
    if (n <= 0 || static_cast<int>(c.keys.size()) != n) {
        ++st.skipped;
        VK_LOG_WARN("CURVEBAKE-WRITE-SKIP path=%s comp=%d track=%d curve=%d"
                    "（关键帧数不等 —— 曲线编辑器只改形状，不改帧数）",
                    pathName(tr.path), comp, n,
                    static_cast<int>(c.keys.size()));
        return st;
    }

    // ---- 先把原始数据快照下来，再重建 ----
    //
    // ⚠ 必须快照：下面会 clear 三个数组，而"其它分量保持原值"要从原表读。
    //   第一版没快照，边清边读，写回去的 y/z 全是 0 —— 而那在单分量断言
    //   里完全看不出来。
    const std::vector<glm::vec4> origValues = tr.values;
    const std::vector<unsigned char> origTypes = tr.types;
    const std::vector<float> origTimes = tr.times;

    struct Item {
        float t = 0.0f;
        float v = 0.0f;
        glm::vec4 value{0.0f};
        unsigned char type = 0;
        KeyEase ease;
        int src = -1;
    };
    std::vector<Item> items;
    items.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const CurveKey& ck = c.keys[static_cast<std::size_t>(i)];
        Item it;
        it.t = ck.time;
        it.v = ck.value;
        it.value = origValues[static_cast<std::size_t>(i)];
        it.value[static_cast<glm::length_t>(comp)] = ck.value;   // 只改本分量
        it.type = i < static_cast<int>(origTypes.size())
                      ? origTypes[static_cast<std::size_t>(i)]
                      : static_cast<unsigned char>(KeyType::Key);
        it.ease = tr.easeAt(i);          // 保留表里别的分量设过的东西
        it.ease.interp = ck.interp;
        it.ease.ease = ck.ease;
        it.ease.shape = ck.shape;
        it.ease.handle = ck.handle;
        it.ease.outHandle = ck.outHandle;
        it.ease.inHandle = ck.inHandle;
        it.src = i;
        items.push_back(it);
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.t < b.t; });

    tr.times.clear();
    tr.values.clear();
    tr.types.clear();
    tr.ease.clear();
    tr.times.reserve(items.size());
    tr.values.reserve(items.size());

    for (const Item& it : items) {
        if (!tr.times.empty() &&
            std::fabs(it.t - tr.times.back()) <= kKeyTimeEpsilon) {
            ++st.merged;
            // 与 `rebuildTrack` 同口径：**后来者赢**（作者刚把它拖到这里）。
            tr.values.back() = it.value;
            tr.types.back() = it.type;
            tr.ease.back() = it.ease;
            continue;
        }
        tr.times.push_back(it.t);
        tr.values.push_back(it.value);
        tr.types.push_back(it.type);
        tr.ease.push_back(it.ease);
        ++st.written;
    }

    // ---- 变了多少 ----
    //
    // 按**源下标**回查（不是按位置）：排序 + 合并之后位置全变了，按下标比
    // 会把"整体挪了一格"误报成"每个键都变了"。被合并掉的键也算变（它没了）。
    {
        std::vector<const Item*> bySrc(static_cast<std::size_t>(n), nullptr);
        for (const Item& it : items) {
            if (it.src >= 0 && it.src < n)
                bySrc[static_cast<std::size_t>(it.src)] = &it;
        }
        for (int i = 0; i < n; ++i) {
            const Item* it = bySrc[static_cast<std::size_t>(i)];
            if (!it) { ++st.changed; continue; }
            const glm::vec4& before = origValues[static_cast<std::size_t>(i)];
            if (it->t != origTimes[static_cast<std::size_t>(i)] ||
                it->v != before[static_cast<glm::length_t>(comp)])
                ++st.changed;
        }
    }

    // ---- step 与缓动的互斥 ----
    // 作者把某一键设成贝塞尔/线性，就是"这条轨道不再是阶跃"。
    // 不清 `step` 的话 `resolveEaseTable` 会把**其它**默认键仍当常量，
    // 于是同一条轨道上出现"一键贝塞尔 + 其余瞬跳"的怪异混合。
    if (tr.step) {
        for (const KeyEase& e : tr.ease) {
            if (e.interp != Interp::Constant) {
                tr.step = false;
                VK_LOG_INFO("CURVEBAKE-STEP-OFF path=%s（有键改成了非阶跃插值）",
                            pathName(tr.path));
                break;
            }
        }
    }
    tr.pruneDefaultEase();

    // ---- 写回后再烘一遍：与传入曲线必须逐位相同 ----
    //
    // 这是"写回完整"的判据：漏了手柄、漏了 interp、漏了 ease 三者之一，
    // 这条偏差就不再是 0，而画面上可能一点都看不出来（AutoClamped 手柄
    // 会自己长回来）。所以宁可在这里多算 33 次。
    {
        const Curve back = bakeTrackCurve(tr, comp);
        if (!back.keys.empty()) {
            const float t0 = back.keys.front().time;
            const float t1 = back.keys.back().time;
            for (int s = 0; s < kVerifySamples; ++s) {
                const float w = static_cast<float>(s) /
                                static_cast<float>(kVerifySamples - 1);
                const float t = t0 + (t1 - t0) * w;
                const float d = std::fabs(evaluateCurve(back, t) -
                                          evaluateCurve(c, t));
                if (d > st.worstDeviation) st.worstDeviation = d;
            }
        }
        st.bitIdentical = (st.worstDeviation == 0.0f) && st.skipped == 0;
    }
    return st;
}

// ---------------------------------------------------------------- 通道表

std::vector<BoneChannel> listBoneChannels(const EditClip& clip) {
    std::vector<BoneChannel> out;
    for (std::size_t ti = 0; ti < clip.tracks.size(); ++ti) {
        const EditTrack& tr = clip.tracks[ti];
        if (tr.keyCount() <= 0) continue;
        const int comps = pathComponentCount(tr.path);
        for (int c = 0; c < comps; ++c)
            out.push_back(BoneChannel{static_cast<int>(ti), c});
    }
    return out;
}

std::string boneChannelId(const EditClip& clip, const BoneChannel& ch) {
    if (ch.track < 0 || ch.track >= static_cast<int>(clip.tracks.size()))
        return "(none)";
    const EditTrack& tr = clip.tracks[static_cast<std::size_t>(ch.track)];
    std::string s = tr.joint;
    s += '/';
    s += pathName(tr.path);
    s += '.';
    s += pathComponentName(tr.path, ch.comp);
    return s;
}

// ---------------------------------------------------------------- 摘要

unsigned long long curveDigest(const Curve& c) {
    unsigned long long h = 14695981039346656037ull;
    feedI(h, static_cast<int>(c.keys.size()));
    feedI(h, static_cast<int>(c.before));
    feedI(h, static_cast<int>(c.after));
    feedF(h, c.cyclePeriod);
    for (const CurveKey& k : c.keys) {
        feedF(h, k.time);
        feedF(h, k.value);
        feedI(h, static_cast<int>(k.interp));
        feedI(h, static_cast<int>(k.ease));
        feedI(h, static_cast<int>(k.shape));
        feedI(h, static_cast<int>(k.handle));
        feedF(h, k.outHandle.x);
        feedF(h, k.outHandle.y);
        feedF(h, k.inHandle.x);
        feedF(h, k.inHandle.y);
    }
    return h;
}

} // namespace animation
