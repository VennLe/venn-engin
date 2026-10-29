// ============================================================
// animation/NlaTimeline.cpp —— NLA 求值器
//
// 语义与约定全部写在头文件里。这个 .cpp 只做三件事：
//   ① 把"时间轴时刻"映射成"源 clip 时刻"（`NlaStrip::sourceTime`）；
//   ② 把"区间内 / 淡入淡出 / 权重"折成一个数（`envelope × weight`）；
//   ③ 用已有的两个原语把它盖到姿势上 —— Replace 走 `blendPoseMasked`
//      （三个逐位短路都是白送的），Add 走 rest 相对增量。
//
// 为什么第 ③ 步**不另写一份混合**：`blendPoseMasked` 的三条短路（权重 0
// 逐位不动 / 权重 1 逐位等于来源 / 遮罩空逐位不动）是 B1 花了一轮回归
// 换来的，抄一份就等于抄了三份"某天有人只改了其中一份"的机会。
// ============================================================

#include "animation/NlaTimeline.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace animation {
namespace {

// 摘要用：浮点按位吃进去。`+0.0f` 与 `-0.0f` 数值相等但位不同 ——
// 摘要就是要区分它们（"存了一趟读回来符号变了"这种事只有位级比较看得见）。
void fnvBytes(uint64_t& h, const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(b[i]);
        h *= 1099511628211ULL;
    }
}

void fnvFloat(uint64_t& h, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    fnvBytes(h, &bits, sizeof(bits));
}

void fnvInt(uint64_t& h, int v) {
    int32_t x = static_cast<int32_t>(v);
    fnvBytes(h, &x, sizeof(x));
}

void fnvStr(uint64_t& h, const std::string& s) {
    const uint32_t n = static_cast<uint32_t>(s.size());
    fnvBytes(h, &n, sizeof(n));   // 长度也吃：否则 "ab"+"c" 与 "a"+"bc" 撞
    fnvBytes(h, s.data(), s.size());
}

// 遮罩解算缓存键：**必须带骨架身份**（同一份 spec 落到关节数不同的骨架上，
// 解出来的 weights 长度都不同）。
std::string scratchMaskKey(const assets::Skeleton& skel,
                           const BoneMaskSpec& spec) {
    std::string k = boneMaskKey(spec);
    k += '\x1e';
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llx",
                  static_cast<unsigned long long>(
                      reinterpret_cast<uintptr_t>(&skel)));
    k += buf;
    return k;
}

const BoneMask& maskForScratch(const assets::Skeleton& skel,
                               const BoneMaskSpec& spec, NlaScratch& sc,
                               NlaEvalStats& st) {
    std::string key = scratchMaskKey(skel, spec);
    auto it = sc.masks.find(key);
    if (it != sc.masks.end()) return it->second;

    BoneMask m = buildBoneMask(skel, spec);
    // 只计数不打日志：NLA 每帧都在跑，逐条 warning 会把日志打爆。
    // 面板与 `ANIM-NLA` 收尾行会把 `masksUnresolved` 报出来。
    if (m.empty()) ++st.masksUnresolved;
    if (m.empty() && st.firstProblem.empty()) st.firstProblem = "mask:empty";
    return sc.masks.emplace(std::move(key), std::move(m)).first->second;
}

void noteProblem(NlaEvalStats& st, const std::string& what) {
    if (st.firstProblem.empty()) st.firstProblem = what;
}

} // namespace

const char* nlaBlendName(NlaBlend b) {
    switch (b) {
    case NlaBlend::Replace: return "replace";
    case NlaBlend::Add: return "add";
    }
    return "replace";
}

bool nlaBlendFromInt(int v, NlaBlend* out) {
    if (!out) return false;
    if (v < 0 || v > 1) return false;
    *out = static_cast<NlaBlend>(v);
    return true;
}

// ------------------------------------------------------------ NlaStrip

float NlaStrip::span() const {
    // 下限 1e-6：`end == start`（新建但还没拖过的 strip）是常见的中间状态，
    // 让它除出 inf 会把 `sourceTime` 变成 NaN，然后 NaN 顺着姿势传遍全身
    // —— 症状是"角色消失"，而且在检查 strip 之前根本看不出根因。
    return std::max(end - start, 1.0e-6f);
}

bool NlaStrip::activeAt(float t) const {
    // 闭区间：接缝处**两条都算**（约定 ③ 说的是"区间外不存在"，
    // 没说"接缝要留一个 t 的空隙"）。于是首尾相接的两条 strip 在接缝那
    // 一帧会各贡献一半 —— 这正是作者想要的连续过渡，而不是一帧的静止姿势。
    return t >= start && t <= end;
}

bool NlaStrip::valid() const {
    return !muted && !clip.empty() && weight > 0.0f &&
           end > start + 1.0e-6f;
}

float NlaStrip::envelope(float t) const {
    // 常走的那条路：没有淡入淡出 → 恰好 1.0f（约定 ①，逐位不变靠它）
    if (blendIn <= 0.0f && blendOut <= 0.0f) return 1.0f;
    float e = 1.0f;
    if (blendIn > 0.0f) e *= smoothFade((t - start) / blendIn);
    if (blendOut > 0.0f) e *= smoothFade((end - t) / blendOut);
    // 两者重叠时乘积仍然 <= 1；这里再夹一次只是因为将来加第三种淡入淡出
    // （比如淡入也走 Add）时不该出现 >1 的权重。
    return std::clamp(e, 0.0f, 1.0f);
}

float NlaStrip::sourceTime(float t, float clipDuration) const {
    // u = 在 strip 里的归一化位置
    float u = (t - start) / span();
    u *= speed;
    if (loop) {
        // fract。用 `u - floor(u)` 而不是 `fmod(u,1)`：负 speed 时 fmod
        // 会给出负数，而负数 u 映射到源区间之外 = 采样到夹紧的端点，
        // 表现成"倒放的动作一进去就定住"。
        u -= std::floor(u);
    } else {
        u = std::clamp(u, 0.0f, 1.0f);
    }

    const float a = clipStart;
    // `clipEnd <= clipStart`（含新 strip 的 0/0）→ 取到 clip 末尾
    const float b = (clipEnd > clipStart + 1.0e-6f)
                        ? clipEnd
                        : std::max(clipDuration, clipStart + 1.0e-6f);
    return a + u * (b - a);
}

// ------------------------------------------------------------ 求值

NlaEvalStats applyNlaTimeline(const assets::Skeleton& skel,
                              const std::vector<NlaTrack>& tracks, float time,
                              Pose& pose, NlaScratch& sc) {
    NlaEvalStats st;
    st.tracks = static_cast<int>(tracks.size());
    // 约定 ①：没有轨 → pose 一个分量都不碰
    if (tracks.empty()) return st;

    // ---- ① 先决定哪些轨参与（muted / solo）----
    bool anySolo = false;
    for (const NlaTrack& T : tracks) {
        if (T.solo) {
            anySolo = true;
            break;
        }
    }
    sc.order.clear();
    for (size_t i = 0; i < tracks.size(); ++i) {
        const NlaTrack& T = tracks[i];
        st.strips += static_cast<int>(T.strips.size());
        if (T.muted) continue;
        if (anySolo && !T.solo) continue;
        sc.order.push_back(static_cast<int>(i));
    }
    if (sc.order.empty()) return st;   // 全静音 / solo 过滤后一条不剩

    const int n = pose.count();
    if (n <= 0) return st;

    for (int ti : sc.order) {
        const NlaTrack& T = tracks[static_cast<size_t>(ti)];
        bool used = false;

        // 轨内按**列表顺序**求值（头文件里的约定）
        for (const NlaStrip& s : T.strips) {
            if (!s.activeAt(time)) continue;   // 约定 ③：区间外它不存在
            ++st.active;

            if (!s.valid()) {
                ++st.skipped;
                if (s.muted) {
                    // 静音的 strip 在区间内也不该算"问题"
                } else if (s.clip.empty()) {
                    noteProblem(st, "clip:empty");
                } else if (s.weight <= 0.0f) {
                    // 权重 0 = 用户故意的，不算问题
                } else {
                    noteProblem(st, "span:zero");
                }
                continue;
            }

            const assets::AnimationClip* c = skel.findClip(s.clip);
            if (!c) {
                ++st.skipped;
                noteProblem(st, "clip:" + s.clip);
                continue;
            }

            const float w = s.weight * s.envelope(time);
            if (w <= 0.0f) {
                // 包络正好在端点上（淡入 0 秒处 / 淡出末点）= 这一帧不贡献。
                // 不算"跳过"（不是配置问题），但也不许往下走 —— 否则会多
                // 做一次采样 + 一次混合，而结果被 weight=0 抹掉。
                continue;
            }

            const BoneMask& mask = maskForScratch(skel, s.mask, sc, st);
            if (mask.empty()) {
                // 遮罩一根骨都没命中 = 这条 strip 静默失效（与 AnimLayer
                // 同一个判据：配错名字必须表现为"没生效"，不是"全身覆盖"）
                ++st.skipped;
                continue;
            }

            samplePose(skel, c, s.sourceTime(time, c->duration), sc.stripPose);

            if (s.blend == NlaBlend::Add) {
                // ---- 约定 ④：rest 相对增量 ----
                if (!sc.restValid) {
                    sc.restPose.setRest(skel);
                    sc.restValid = true;
                }
                const Pose& R = sc.restPose;
                const int m = std::min(
                    n, std::min(sc.stripPose.count(), mask.count()));
                const glm::quat kIdentity(1.0f, 0.0f, 0.0f, 0.0f);
                for (int j = 0; j < m; ++j) {
                    const size_t jj = static_cast<size_t>(j);
                    const float mw = mask.weights[jj] * w;
                    if (mw <= 0.0f) continue;
                    // 增量在**本骨局部空间**：`rest⁻¹ * strip`
                    const glm::quat dq = glm::inverse(R.rotation[jj]) *
                                         sc.stripPose.rotation[jj];
                    // mw == 1 时不必 slerp（slerp(id, dq, 1) 在浮点下
                    // 不保证逐位等于 dq，虽然差得极少 —— 没必要冒这个险）
                    const glm::quat scaled =
                        (mw >= 1.0f) ? dq : glm::slerp(kIdentity, dq, mw);
                    // ⚠ **不 normalize**。乘单位四元数在浮点下是精确的
                    //   （乘 1、加 0），所以"这条 strip 没碰这根骨"时结果与
                    //   输入**逐位相同** —— `nla_add_untouched_bone_bitwise`
                    //   断的就是这个。加一次 normalize 就会引入 1 ulp 的
                    //   偏差，而且它只出现在**没被碰过的骨**上（被碰过的骨
                    //   反正已经变了），是最难查的那种"看起来全对"的错。
                    //   漂移风险也不存在：`pose` 每帧都由 `samplePose` 从
                    //   资产重建，不是跨帧累乘。
                    pose.rotation[jj] = pose.rotation[jj] * scaled;
                    pose.translation[jj] +=
                        (sc.stripPose.translation[jj] - R.translation[jj]) * mw;
                    // 缩放也按差值叠（相对倍率在 rest 缩放大范围时更"对"，
                    // 但 rest 缩放可能是 0，除法会把整根骨炸成 NaN —— 差值
                    // 在导演上够用，且永远不会除零）
                    pose.scale[jj] +=
                        (sc.stripPose.scale[jj] - R.scale[jj]) * mw;
                }
            } else {
                // Replace：直接复用 B1 的遮罩混合（三个逐位短路白送）。
                // `out == base` 同址安全（blendPoseMasked 先整体拷贝）。
                blendPoseMasked(pose, sc.stripPose, mask, w, pose);
            }

            ++st.applied;
            used = true;
        }

        if (used) ++st.tracksActive;
    }
    return st;
}

float nlaTimelineDuration(const std::vector<NlaTrack>& tracks) {
    float d = 0.0f;
    for (const NlaTrack& T : tracks) {
        for (const NlaStrip& s : T.strips) {
            if (s.end > d) d = s.end;
        }
    }
    return d;
}

int nlaStripCount(const std::vector<NlaTrack>& tracks) {
    int n = 0;
    for (const NlaTrack& T : tracks) n += static_cast<int>(T.strips.size());
    return n;
}

uint64_t nlaDigest(const std::vector<NlaTrack>& tracks) {
    uint64_t h = 1469598103934665603ULL;
    fnvInt(h, static_cast<int>(tracks.size()));
    for (const NlaTrack& T : tracks) {
        fnvStr(h, T.name);
        fnvInt(h, T.muted ? 1 : 0);
        fnvInt(h, T.solo ? 1 : 0);
        fnvInt(h, static_cast<int>(T.strips.size()));
        for (const NlaStrip& s : T.strips) {
            fnvStr(h, s.name);
            fnvStr(h, s.clip);
            fnvFloat(h, s.start);
            fnvFloat(h, s.end);
            fnvFloat(h, s.clipStart);
            fnvFloat(h, s.clipEnd);
            fnvFloat(h, s.speed);
            fnvInt(h, s.loop ? 1 : 0);
            fnvFloat(h, s.weight);
            fnvFloat(h, s.blendIn);
            fnvFloat(h, s.blendOut);
            fnvInt(h, static_cast<int>(s.blend));
            fnvInt(h, static_cast<int>(s.mask.mode));
            fnvInt(h, static_cast<int>(s.mask.roots.size()));
            for (const std::string& r : s.mask.roots) fnvStr(h, r);
            fnvInt(h, s.muted ? 1 : 0);
        }
    }
    return h;
}

} // namespace animation
