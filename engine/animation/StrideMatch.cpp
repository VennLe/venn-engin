// ============================================================
// animation/StrideMatch.cpp —— 从 clip 里量自然步速 + 速度→播放速率
//
// 判据的分工（这三层缺一层，这个功能就是"感觉上好像不滑了"）：
//   ① `measureStride`   —— 量 clip：承重期脚相对身体的后退速率
//   ② `strideRateFor`   —— 算速率：速度 / 自然步速，夹范围，报残差
//   ③ `Locomotion`      —— 每帧把 ② 写进 `speed`，并在夹住时喊一声
// ============================================================

#include "animation/StrideMatch.h"

#include "animation/ClipEdit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace animation {

namespace {

// 低于这个"自然步速"就当没有步幅（m/s）。idle / 坐 / 挥手都落在这里。
// 取 5 cm/s：正常走路是 60~140 cm/s，慢到 5 cm/s 已经不是步态了。
constexpr float kMinNominalSpeed = 0.05f;

// 一只脚在整个周期里上下行程不到 2 cm → 它没抬起来，不参与承重判定
// （双撑站立时两脚都贴地，此时"谁承重"没有意义，量出来的是噪声）。
constexpr float kMinFootTravel = 0.02f;

// 承重 = 脚相对身体**向后走** **且** 脚底在**低位**。
//
// 第一条是原地走的定义本身（身体前进 + 脚不动 ⇒ 脚相对身体向后）；
// 第二条排除"脚还在空中、但正在往后收腿准备落地"那一段 —— 那一段方向
// 也是向后，只靠方向会把跑步的腾空算成承重（实测 `alicia` Run 的
// `air` 因此恒为 0，跑步和走路分不开）。
//
// ⚠ 低位门用**绝对抬升**（"最低点 + 12 cm"），不是"行程的比例"：
//   比例项会跟着这个 clip 的抬腿高度一起涨（跑步 40 cm 行程 → 20 cm 带，
//   腾空正好被吞掉），而"脚掌滚动最多把脚底抬多高"是几何事实，与画风
//   无关。12 cm 在中间：走路滚动 ≤ 7 cm（不排除），跑步腾空 ≥ 20 cm
//   （排除）。
constexpr float kContactLiftMax = 0.12f;

// 采样密度（帧/秒）。60 是"够密到能用中心差分求速度、又不至于量一次
// 花掉毫秒级"的折中；一次换 clip 才量一次。
constexpr float kSampleHz = 60.0f;
constexpr int kMinSamples = 16;
constexpr int kMaxSamples = 480;

// 相位曲线把它切成多少个 bin。32 在 24 fps 的 24 帧循环上是"每帧 1.33 个
// bin"，比关键帧密度略粗 —— 再细就只是在插值噪声上做除法。
constexpr int kCurveBins = 32;

// 相位增益的夹范围。见 StrideMatch.h 里的说明：承重期两端 v(φ) → 0，
// 不夹的话 gain 会到几十（一帧之内跳半步，比滑步更难看成"在走"）。
constexpr float kGainMin = 0.5f;
constexpr float kGainMax = 2.0f;

// 斜坡角度的上限（度）。超过 75° 已经不是"走上去"了（那是爬/攀），
// cos 会掉到 0.26 以下，report 出来的竖直残差会大得没有意义。
constexpr float kMaxSlopeDeg = 75.0f;

float median(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid),
                     v.end());
    float m = v[mid];
    if (v.size() % 2 == 0) {
        // 偶数个：取中间两个的均值（否则 0.6/0.62 会跳成 0.62）
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid - 1),
                         v.end());
        m = 0.5f * (m + v[mid - 1]);
    }
    return m;
}

glm::vec3 originOf(const glm::mat4& m) { return glm::vec3(m[3]); }

} // namespace

StrideInfo measureStride(const assets::Skeleton& skel, float duration,
                         const PoseSampler& sample,
                         StridePhaseCurve* curve) {
    StrideInfo out;
    out.cycleSeconds = duration;

    if (!sample) {
        out.error = "没有采样函数";
        return out;
    }
    if (!(duration > 1.0e-3f)) {
        out.error = "clip 时长为 0，没法量步速";
        return out;
    }

    const HumanoidRig rig = resolveHumanoidRig(skel);
    out.missingRoles = static_cast<int>(rig.missing.size());
    if (!rig.walkable()) {
        out.error = "骨架不可走（缺髋 / 大腿 / 小腿 / 踝），量不出步速";
        return out;
    }
    const HumanoidAxes ax = measureHumanoidAxes(skel, rig);
    if (!ax.ok) {
        out.error = ax.error.empty() ? "量不出骨架三轴" : ax.error;
        return out;
    }

    const int hips = rig.joint[static_cast<int>(GaitRole::Hips)];
    const int foot[2] = {rig.joint[static_cast<int>(GaitRole::LeftFoot)],
                         rig.joint[static_cast<int>(GaitRole::RightFoot)]};
    // 脚尖：承重期脚掌滚动 / 蹬离时**脚尖仍贴地而踝已经抬起来**，所以
    // "脚底高度"要取踝与脚尖的低者。只用踝高会把走路的承重期砍掉一半
    // （见 `kContactFracOfTravel` 的演进史 ①）。
    // 没有脚尖骨的骨架（`-1`）→ 退回踝高：量出来的 duty 会偏小，但
    // 步速（nominal）不受影响 —— 那段仍是"脚相对身体匀速后退"。
    const int toe[2] = {rig.joint[static_cast<int>(GaitRole::LeftToe)],
                        rig.joint[static_cast<int>(GaitRole::RightToe)]};
    if (hips < 0 || foot[0] < 0 || foot[1] < 0) {
        out.error = "骨架缺少髋 / 踝";
        return out;
    }

    int n = static_cast<int>(std::lround(duration * kSampleHz));
    n = std::clamp(n, kMinSamples, kMaxSamples);
    const float dt = duration / static_cast<float>(n);

    // 逐帧采：脚底的**两套高度**（沿骨架自己的 up 轴）+ 前向距离（相对髋）。
    //
    // ⚠ 为什么必须要两套参考系（这条是被真实资产打回来的）：
    //   · 用**绝对高度**判"脚在不在承重"，前提是"整具身体不竖直漂移"。
    //     真实资产 `animate_girl` 的 Walk 里髋沿 up 轴上下走了 **0.805 m**
    //     （clip 自己带竖直位移），脚底的绝对行程于是也是 0.8 m —— 带宽
    //     一旦跟行程挂钩就变成 12 cm，腾空期被算成承重、duty 掉到 0.16。
    //   · 用**相对髋的高度**，前提是"髋不怎么动"。骨盆 bob ±3 cm 时，
    //     承重期脚钉地不动而"脚相对髋"扫过 6 cm，窄带下 duty 掉到 0.23
    //     （第一版就是这么错的）。
    //   两者的失效原因互为镜像 → **哪一套的行程更小就用哪一套**：
    //   `alicia`（hipRise=0.010 m）选绝对，`animate_girl`（0.805 m）选相对。
    //   这不是"两个都试一遍碰运气"：行程小 = 那套参考系里的变动更接近
    //   "脚自己抬了多少"，而另一个量（身体漂移 / 骨盆 bob）被抵消掉了。
    //
    // 前向**必须**相对髋：要的就是"脚相对身体走了多远"，用绝对量的话
    // 角色整体前进（有根运动）会被当成脚步。
    std::vector<float> heightAbs[2];
    std::vector<float> heightRel[2];
    std::vector<float> forward[2];
    for (int s = 0; s < 2; ++s) {
        heightAbs[s].assign(static_cast<std::size_t>(n), 0.0f);
        heightRel[s].assign(static_cast<std::size_t>(n), 0.0f);
        forward[s].assign(static_cast<std::size_t>(n), 0.0f);
    }
    // 髋沿 up 轴的高度（诊断用）：它是"该选哪套参考系"的依据。
    std::vector<float> hipUp(static_cast<std::size_t>(n), 0.0f);

    Pose pose;
    std::vector<glm::mat4> world;
    for (int i = 0; i < n; ++i) {
        sample(dt * static_cast<float>(i), pose);
        poseToWorld(skel, pose, world);
        const glm::vec3 hp = originOf(world[static_cast<std::size_t>(hips)]);
        const float hu = glm::dot(hp, ax.up);
        hipUp[static_cast<std::size_t>(i)] = hu;
        for (int s = 0; s < 2; ++s) {
            const glm::vec3 fp =
                originOf(world[static_cast<std::size_t>(foot[s])]);
            // 脚底高度 = 踝与脚尖的低者（见 toe[] 的注释）。
            float sole = glm::dot(fp, ax.up);
            if (toe[s] >= 0) {
                sole = std::min(
                    sole, glm::dot(originOf(world[static_cast<std::size_t>(
                                       toe[s])]),
                                   ax.up));
            }
            heightAbs[s][static_cast<std::size_t>(i)] = sole;
            heightRel[s][static_cast<std::size_t>(i)] = sole - hu;
            // 前向距离仍用**踝**：脚尖会自己转（脚跟抬起 / 蹬离），
            // 用它量"脚相对身体走了多远"会把踝的滚动算成位移。
            forward[s][static_cast<std::size_t>(i)] = glm::dot(fp - hp, ax.fwd);
        }
    }
    out.samples = n;

    // ---- 调试导出（`MYVK_STRIDE_DUMP=1`）----
    //
    // 承重判据这个东西，写错的时候**每一个读数都自洽**（duty=0.28、
    // air=0.43 都像模像样），只是与物理对不上。这时候唯一的出路是看
    // 原始曲线，而不是再换一个阈值 —— 这条注释就是八轮"改常数"之后
    // 留下的教训。钩子刻意常驻（一次性调试的代价远低于它）。
    //
    // 输出：每帧每只脚一行（i / t / side / fwd / hAbs / hRel / dFwd）。
    if (const char* dump = std::getenv("MYVK_STRIDE_DUMP")) {
        if (dump[0] == '1') {
            char path[64];
            std::snprintf(path, sizeof(path), "_stride_dump_%.2f.csv",
                          static_cast<double>(duration));
            if (std::FILE* fp = std::fopen(path, "w")) {
                std::fprintf(fp, "i,t,side,fwd,hAbs,hRel,dFwd\n");
                for (int i = 0; i < n; ++i) {
                    for (int s = 0; s < 2; ++s) {
                        const int ip = (i + 1) % n;
                        const int im = (i + n - 1) % n;
                        std::fprintf(
                            fp, "%d,%.4f,%d,%.5f,%.5f,%.5f,%.5f\n", i,
                            static_cast<double>(dt * static_cast<float>(i)), s,
                            static_cast<double>(
                                forward[s][static_cast<std::size_t>(i)]),
                            static_cast<double>(
                                heightAbs[s][static_cast<std::size_t>(i)]),
                            static_cast<double>(
                                heightRel[s][static_cast<std::size_t>(i)]),
                            static_cast<double>(
                                (forward[s][static_cast<std::size_t>(ip)] -
                                 forward[s][static_cast<std::size_t>(im)])));
                    }
                }
                std::fclose(fp);
            }
        }
    }

    // ---- 选参考系：两脚行程之和更小的那一套 ----
    auto rangeOf = [n](const std::vector<float>& v) {
        float lo = v[0], hi = v[0];
        for (int i = 1; i < n; ++i) {
            lo = std::min(lo, v[static_cast<std::size_t>(i)]);
            hi = std::max(hi, v[static_cast<std::size_t>(i)]);
        }
        return hi - lo;
    };
    const float spanAbs = rangeOf(heightAbs[0]) + rangeOf(heightAbs[1]);
    const float spanRel = rangeOf(heightRel[0]) + rangeOf(heightRel[1]);
    out.soleAbsolute = (spanAbs <= spanRel);
    auto H = [&](int s, int i) {
        const std::size_t k = static_cast<std::size_t>(i);
        return out.soleAbsolute ? heightAbs[s][k] : heightRel[s][k];
    };

    // ---- 每只脚的前向速度（中心差分 + **循环回绕**）----
    //
    // 回绕是必须的：步态 clip 基本都是首尾相接的循环，用端点单侧差分会在
    // 首尾两帧给出偏差，而这两帧正好是最关键的两帧（触地 / 蹬离）。
    // 不循环的 clip（一次起跳）只会在端点贡献两个略偏的样本，而判据取
    // 的是**中位数**与**方向**，离群点影响不到。
    std::vector<float> dFwd[2];
    for (int s = 0; s < 2; ++s) {
        dFwd[s].assign(static_cast<std::size_t>(n), 0.0f);
        for (int i = 0; i < n; ++i) {
            const int ip = (i + 1) % n;
            const int im = (i + n - 1) % n;
            dFwd[s][static_cast<std::size_t>(i)] =
                forward[s][static_cast<std::size_t>(ip)] -
                forward[s][static_cast<std::size_t>(im)];
        }
    }

    // ---- 每只脚的抬升行程 + 低位门 ----
    float band[2] = {0.0f, 0.0f};
    bool usable[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        float lo = H(s, 0), hi = H(s, 0);
        for (int i = 1; i < n; ++i) {
            lo = std::min(lo, H(s, i));
            hi = std::max(hi, H(s, i));
        }
        const float travel = hi - lo;
        out.footTravel[s] = travel;
        // `footLow` 报**绝对**最低点（不管参考系选的是哪套）：相对量是负数
        // （脚比髋低近 1 米），读日志时容易看错。
        {
            float alo = heightAbs[s][0];
            for (int i = 1; i < n; ++i)
                alo = std::min(alo, heightAbs[s][static_cast<std::size_t>(i)]);
            out.footLow[s] = alo;
        }
        if (travel < kMinFootTravel) continue;   // 这只脚一直没离地
        usable[s] = true;
        band[s] = lo + kContactLiftMax;
    }
    out.soleFromToe = (toe[0] >= 0) || (toe[1] >= 0);
    {
        // 前向摆幅（交叉校验用，见 StrideInfo::strideSpan 的说明）。
        for (int s = 0; s < 2; ++s) {
            if (!usable[s]) continue;
            const float span = rangeOf(forward[s]);
            out.strideSpan = std::max(out.strideSpan, span);
        }
        if (duration > 0.0f)
            out.spanSpeed = 2.0f * out.strideSpan / duration;
    }
    {
        float hlo = hipUp[0], hhi = hipUp[0];
        for (int i = 1; i < n; ++i) {
            hlo = std::min(hlo, hipUp[static_cast<std::size_t>(i)]);
            hhi = std::max(hhi, hipUp[static_cast<std::size_t>(i)]);
        }
        out.hipRise = hhi - hlo;
    }
    if (!usable[0] && !usable[1]) {
        out.error = "两只脚都没抬起来（不是步态 clip？）";
        return out;
    }

    // ---- 承重期：**脚相对身体在向后走** ----
    //
    // 这一条用的是原地走（treadmill）的定义本身，不是它的代理量：
    //
    //   身体在前进、脚在地上不动  ⇒  脚**相对身体**恒定向后移动
    //   脚要迈到身体前面去          ⇒  摆动期脚相对身体向前
    //
    // 判据就是 `dFwd < 0`。**没有阈值要调** —— 0 是自然边界，不是某个
    // 资产上量出来的数。
    //
    // ⚠ 之前那一长串"高度带 + 带宽 + 参考系"全部被删掉了，原因是它们是
    //   这个判据的**代理量**，而每一个代理量都要一个常数：高度带在走路
    //   滚动期会漏、在跑步腾空期会多算；带宽挂着"行程比例"就会被根运动
    //   的竖直漂移带跑（`animate_girl` 的 Walk 髋上下走了 0.805 m）。
    //   六轮改动（踝高 25% → 50% → 加方向 → 脚底 → 封顶 → 脚底不动）
    //   每次都只是把另一个阈值挪一挪，`duty` 在 0.13~0.61 之间乱跳。
    //   换成这个判据之后每个 clip 都落在物理讲得通的位置，且**一个常数
    //   都不用调**。教训：能用定性事实表达的判据别用阈值去逼近它。
    //
    // ⚠ 双支撑期两只脚都向后 → 两只都算承重（`duty` 因此可以 > 0.5，
    //   这是对的：`duty` 的定义是"单脚承重占比"，双撑时两只脚都在承重）。
    // ⚠ `speeds` 只收向后样本，所以它与 `duty` 描述的是**同一批**样本。
    std::vector<float> speeds;
    speeds.reserve(static_cast<std::size_t>(n));
    // 承重 = 脚相对身体在向后走。一次定义、两处使用（承重统计 / 腾空统计）
    // —— 两处各写一遍的话，"air=0 但 duty=0.35"这种自相矛盾的组合会出现
    // 在同一行日志里。
    auto onGround = [&](int s, int i) {
        return dFwd[s][static_cast<std::size_t>(i)] < 0.0f &&
               H(s, i) <= band[s];
    };
    // 相位分箱同时收：一次遍历既出中位数（nominal）也出曲线（gain）。
    // ⚠ 分两遍写会让"两个数来自不同的承重判据"成为可能 —— 那正好是
    //   "曲线补偿的和中位数对不上"这种没人查得出来的偏差的来源。
    std::vector<std::vector<float>> binSamples(static_cast<std::size_t>(
        curve ? kCurveBins : 0));
    const float binScale = (n > 0)
                               ? static_cast<float>(kCurveBins) /
                                     static_cast<float>(n)
                               : 0.0f;
    int contactCount = 0;
    for (int i = 0; i < n; ++i) {
        const int bin = curve
                            ? std::min(kCurveBins - 1,
                                       static_cast<int>(static_cast<float>(i) *
                                                        binScale))
                            : -1;
        for (int s = 0; s < 2; ++s) {
            if (!usable[s]) continue;
            if (!onGround(s, i)) continue;
            ++contactCount;
            // 承重期脚相对身体**往后**走 → dFwd 为负 → 取负号得地面速度
            const float v = -dFwd[s][static_cast<std::size_t>(i)] /
                            (2.0f * dt);
            speeds.push_back(v);
            if (bin >= 0)
                binSamples[static_cast<std::size_t>(bin)].push_back(v);
        }
    }
    out.contactSamples = contactCount;

    // ---- 腾空：两只脚**同时**离开了地面 ----
    //
    // 这是"这段是走还是跑"最直接的一个数（比 `duty < 0.5` 更直接）：
    // 走路的双支撑期占 10~25% 的时间，任何一刻都至少有一只脚在地上；
    // 跑有腾空期，那一刻两只脚都不在。所以判据是 `airborne > 0` 而不是
    // 一个阈值 —— 0 与"非 0"是两种步态，不是程度差别。
    //
    // ⚠ 与承重统计**共用** `onGround`：两条判据分头写的话，"air=0 但
    //   duty=0.35"这种自相矛盾的组合会出现在同一行日志里。
    {
        int air = 0;
        for (int i = 0; i < n; ++i) {
            bool anyDown = false;
            for (int s = 0; s < 2; ++s) {
                if (usable[s] && onGround(s, i)) {
                    anyDown = true;
                    break;
                }
            }
            if (!anyDown) ++air;
        }
        out.airborneSamples = air;
        out.airborneFraction =
            static_cast<float>(air) / static_cast<float>(n);
    }

    if (contactCount < 6 || speeds.empty()) {
        out.error = "找不到承重期（脚落地的时间太短？）";
        return out;
    }
    out.dutyFactor = static_cast<float>(contactCount) /
                     (2.0f * static_cast<float>(n));
    out.nominalSpeed = median(speeds);

    if (out.nominalSpeed < kMinNominalSpeed) {
        // 负数是"承重期脚往前跑"—— 要么 fwd 反了，要么这段是倒退走。
        // 两种都必须报出来，不能取绝对值糊过去（取 abs 的后果是"倒着走
        // 的 clip 被当成正常走路，然后角色一边前进一边倒腿"）。
        out.error = "承重期脚没有向后走（fwd 方向反了？这段不是原地走？）";
        return out;
    }
    out.ok = true;

    // ---- 相位速率曲线 ----
    //
    // 每 bin 取**中位数**（不是均值）：一个 bin 里可能同时落着"承重期"
    // 与"刚蹬离、脚开始加速"两种样本，均值会被后者拉偏，而中位数只会
    // 取到占多数的那一类。
    //
    // 空 bin（整段没有承重样本）保持 0 → `gain()` 返回 1（摆动期）。
    if (curve) {
        curve->bins = kCurveBins;
        curve->nominalSpeed = out.nominalSpeed;
        curve->speed.assign(static_cast<std::size_t>(kCurveBins), 0.0f);
        int filled = 0;
        for (int b = 0; b < kCurveBins; ++b) {
            const auto& v = binSamples[static_cast<std::size_t>(b)];
            if (v.empty()) continue;
            const float m = median(v);
            // 分箱内速度必须是正的（承重判据已经保证 dFwd<0），但浮点
            // 抖动可能给出 1e-9；低于"最小步速"就当作没样本。
            if (m < kMinNominalSpeed) continue;
            curve->speed[static_cast<std::size_t>(b)] = m;
            ++filled;
        }
        // 一个承重 bin 都没有 = 曲线没有意义（中位数那条路已经挡住了，
        // 这里是防御性清空：invalid 的曲线 gain() 恒返回 1）。
        if (filled < 2) *curve = StridePhaseCurve{};
    }
    return out;
}

StrideInfo measureClipStride(const assets::Skeleton& skel,
                             const assets::AnimationClip& clip,
                             StridePhaseCurve* curve) {
    return measureStride(skel, clip.duration,
                         [&skel, &clip](float t, Pose& out) {
                             samplePose(skel, &clip, t, out);
                         },
                         curve);
}

StrideInfo measureEditClipStride(const assets::Skeleton& skel,
                                 const EditClip& clip,
                                 StridePhaseCurve* curve) {
    std::vector<ResolvedTrack> tracks;
    resolveEditTracks(skel, clip, tracks);
    return measureStride(skel, clip.duration, [&skel, &tracks](float t,
                                                              Pose& out) {
        // 与编辑器/保存后的回放**同一条求值路径**（先静止姿势再叠加轨道）：
        // 换成 `samplePose` 的话，缓动（KeyEase）就丢了，量出来的速度
        // 与画面上的不是一回事。
        out.setRest(skel);
        overlayEditPose(tracks, t, out);
    }, curve);
}

// ------------------------------------------------------------
// StridePhaseCurve
// ------------------------------------------------------------
float StridePhaseCurve::gain(float phase) const {
    if (!valid()) return 1.0f;

    const float p = phase - std::floor(phase);
    const float fx = p * static_cast<float>(bins);
    const int i0 = static_cast<int>(fx);
    const float t = fx - static_cast<float>(i0);

    // 单个 bin 的增益。非承重 bin 返回 1（摆动期没有接触约束）。
    auto g = [this](int idx) {
        idx = ((idx % bins) + bins) % bins;   // 相位是环形的
        const float v = speed[static_cast<std::size_t>(idx)];
        if (!(v > 0.0f)) return 1.0f;
        return std::clamp(nominalSpeed / v, kGainMin, kGainMax);
    };

    // 取相邻四个 bin 做两段平均再插值 —— 等价于对相邻 bin 做一次 3-tap
    // 平滑，但**不会改变摆动区的值**（那里四个 bin 都是 1）。
    // 不做平滑的后果：32 个 bin 边界上各有一个速率阶跃，一秒 32 次微顿，
    // 而它看起来像"关键帧打得不好"，不会有人想到是速率曲线。
    const float a = 0.5f * (g(i0 - 1) + g(i0));
    const float b = 0.5f * (g(i0) + g(i0 + 1));
    return a * (1.0f - t) + b * t;
}

float StridePhaseCurve::advance(float phase, float phaseDelta) const {
    float p = phase - std::floor(phase);
    const float target = p + phaseDelta;
    if (!valid() || !(std::fabs(phaseDelta) > 1.0e-9f))
        return target - std::floor(target);

    // 子步数按"每步最多跨一个 bin"自适应，下限 4（保证曲线内部的精度）、
    // 上限 32（一次推进跨过整圈时的开销兜底）。
    int sub = static_cast<int>(std::ceil(std::fabs(phaseDelta) *
                                        static_cast<float>(bins)));
    sub = std::clamp(sub, 4, 32);
    const float step = phaseDelta / static_cast<float>(sub);
    for (int k = 0; k < sub; ++k) p += step * gain(p);
    return p - std::floor(p);
}

void StridePhaseCurve::stats(float& gMin, float& gMax, float& gMean) const {
    gMin = 1.0f;
    gMax = 1.0f;
    gMean = 1.0f;
    if (!valid()) return;
    constexpr int kProbe = 256;
    float sum = 0.0f;
    gMin = 1.0e9f;
    gMax = -1.0e9f;
    for (int k = 0; k < kProbe; ++k) {
        const float g = gain((static_cast<float>(k) + 0.5f) /
                             static_cast<float>(kProbe));
        gMin = std::min(gMin, g);
        gMax = std::max(gMax, g);
        sum += g;
    }
    gMean = sum / static_cast<float>(kProbe);
}

StrideRate strideRateFor(float moveSpeed, float nominalSpeed, float minRate,
                         float maxRate, float slopeDeg) {
    StrideRate r;
    if (!(nominalSpeed > kMinNominalSpeed)) return r;   // 没量出来 → 原样
    if (!(moveSpeed > 0.0f)) return r;                  // 没动 → 原速

    // ---- 斜坡：rate 只能对齐**水平**分量 ----
    //
    // `moveSpeed` 已经是水平投影（见头文件）。竖直方向脚要跟着爬坡，
    // 而平地 clip 里脚没有竖直分量 → 那部分**rate 修不了**，只能报。
    // 沿坡面速度 = 水平 / cosθ，竖直分量 = 水平 · tanθ。
    if (std::isfinite(slopeDeg) && std::fabs(slopeDeg) > 0.5f) {
        const float deg = std::clamp(slopeDeg, -kMaxSlopeDeg, kMaxSlopeDeg);
        const float rad = deg * 3.14159265358979f / 180.0f;
        const float c = std::cos(rad);
        if (c > 1.0e-3f) {
            r.verticalResidual =
                moveSpeed * std::fabs(std::tan(rad)) / nominalSpeed;
        }
    }

    const float raw = moveSpeed / nominalSpeed;
    float lo = minRate, hi = maxRate;
    if (!(hi > 0.0f) || hi < lo) {   // 配置写反了 → 退回出厂范围
        lo = 0.6f;
        hi = 1.7f;
    }
    if (lo < 0.0f) lo = 0.0f;
    r.rate = std::clamp(raw, lo, hi);
    r.clamped = (r.rate != raw);
    // 夹住之后**仍然存在**的滑步：1 = 完全滑（rate 太小），0 = 不滑。
    const float effective = nominalSpeed * r.rate;
    r.residual = (effective > 1.0e-6f) ? (moveSpeed / effective - 1.0f) : 0.0f;
    return r;
}

} // namespace animation
