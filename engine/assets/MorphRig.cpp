#include "assets/MorphRig.h"

#include <algorithm>
#include <cmath>

namespace assets {

void MorphRig::build(const std::vector<std::vector<MorphChannel>>& perSub,
                     const std::vector<std::uint32_t>& targetCounts) {
    m_channels.clear();
    m_byName.clear();
    m_targetCounts = targetCounts;

    // 按 (子网格, channel) 的顺序展开。**同名合并**：名字是跨子网格唯一的
    // 语义键（见文件头第 2 条）。
    std::unordered_map<std::string, std::size_t> idxOf;
    for (std::uint32_t sub = 0; sub < perSub.size(); ++sub) {
        for (const MorphChannel& ch : perSub[sub]) {
            if (ch.name.empty() || ch.keys.empty()) continue;
            std::size_t ci;
            auto it = idxOf.find(ch.name);
            if (it == idxOf.end()) {
                ci = m_channels.size();
                idxOf.emplace(ch.name, ci);
                m_channels.push_back(MorphRigChannel{});
                m_channels.back().name = ch.name;
            } else {
                ci = it->second;
            }
            MorphRigChannel& rc = m_channels[ci];
            for (const MorphChannelKey& k : ch.keys) {
                MorphRigChannel::Point p;
                p.weight = k.targetWeight;
                p.subMesh = sub;
                p.target = k.target;
                if (k.target != kNoMorphTarget) rc.drives = true;
                rc.points.push_back(p);
            }
        }
    }

    for (MorphRigChannel& rc : m_channels) {
        // points 必须**整体**按 weight 升序（跨子网格一起排），否则分段
        // 查找会在子网格边界上切错段。stable：同 weight 的相对顺序不动，
        // 保证逐帧结果可复现。
        std::stable_sort(rc.points.begin(), rc.points.end(),
                         [](const MorphRigChannel::Point& a,
                            const MorphRigChannel::Point& b) {
                             return a.weight < b.weight;
                         });
    }

    m_byName.clear();
    for (std::size_t i = 0; i < m_channels.size(); ++i)
        m_byName.emplace(m_channels[i].name, static_cast<int>(i));
}

int MorphRig::channelIndex(const std::string& name) const {
    if (name.empty()) return -1;
    auto it = m_byName.find(name);
    return it == m_byName.end() ? -1 : it->second;
}

std::uint32_t MorphRig::targetCount(std::uint32_t subMesh) const {
    return subMesh < m_targetCounts.size() ? m_targetCounts[subMesh] : 0u;
}

void MorphRig::evaluate(const std::vector<float>& channelWeights,
                        std::vector<std::vector<float>>* out) const {
    if (!out) return;
    out->resize(m_targetCounts.size());
    for (std::size_t s = 0; s < m_targetCounts.size(); ++s)
        (*out)[s].assign(m_targetCounts[s], 0.0f);

    // 写权重前的边界检查：文件读端已经校验过 target 下标，但这一层的
    // 代价是两次比较，而越界写是**堆破坏**（症状随机、极难定位）。
    const auto add = [out](std::uint32_t sub, std::uint32_t tgt, float v) {
        if (sub >= out->size()) return;
        if (tgt >= (*out)[sub].size()) return;
        (*out)[sub][tgt] += v;
    };

    const std::size_t n = std::min(channelWeights.size(), m_channels.size());
    for (std::size_t c = 0; c < n; ++c) {
        const MorphRigChannel& rc = m_channels[c];
        if (rc.points.empty() || !rc.drives) continue;
        float w = channelWeights[c];
        if (!(w > 0.0f)) continue;         // 0 / NaN / 负 → 什么都不做
        if (w > 1.0f) w = 1.0f;            // 滑条上限（见文件头）

        // ------------------------------------------------------------
        // 关键：**先按 targetWeight 去重成"级"，再在级之间插值**
        // ------------------------------------------------------------
        // `points` 里同一个 weight 上可能挂着**多个桶**的点 —— 它们是
        // 同一个形状在皮肤 / 睫毛 / 眼白几个材质桶里的副本，全都必须拿到
        // **同一个**标量。
        //
        // 上一版直接把 points 当成"关键帧链"做分段插值，于是
        // `w <= points.front().weight`（所有 key 的 targetWeight 都是 1.0
        // 时恒成立）只让 `points.front()` 拿到 1.0，其余全部为 0 ——
        // **一个滑条只驱动一个材质桶**。实测 alicia 的 `target_0` 分布在
        // 4 个子网格里，只有 1 个桶真的动了（`MORPH-FRAME` 里 slots 只有
        // 一条 w0=1.00），而日志、数据、校验全绿。
        //
        // 级数很少（in-between 一般 2~4 级），线性扫一遍就够。
        constexpr std::size_t kMaxLevels = 64;
        float lv[kMaxLevels];
        std::size_t nl = 0;
        for (const MorphRigChannel::Point& p : rc.points) {
            // 1e-6 的容差：同一级的 weight 来自同一个 FbxImport 采集分支，
            // 本来就逐位相同；留一点余量只是防手工构造的资产。
            if (nl > 0 && p.weight <= lv[nl - 1] + 1.0e-6f) continue;
            if (nl == kMaxLevels) break;
            lv[nl++] = p.weight;
        }
        if (nl == 0) continue;

        // 求值后只有相邻两级有非零标量（斜坡本来就是两点的凸组合）
        float scW[2] = {-1.0f, -1.0f};
        float scV[2] = {0.0f, 0.0f};
        if (w <= lv[0]) {
            // 第一级的 targetWeight 通常就是 1.0；不是 1 时按比例提前到位
            // （FBX 里 target_weight 就是这个意思）。
            scW[0] = lv[0];
            scV[0] = (lv[0] > 1.0e-6f) ? std::min(w / lv[0], 1.0f) : 1.0f;
        } else if (w >= lv[nl - 1]) {
            scW[0] = lv[nl - 1];
            scV[0] = 1.0f;
        } else {
            std::size_t j = 0;
            while (j + 1 < nl && lv[j + 1] < w) ++j;
            const float span = lv[j + 1] - lv[j];
            // 相邻两级 weight 相同是导出器的退化数据 → 直接取后一级，
            // 除以 0 会让整张脸变 NaN（NaN 进 GPU 后是"消失的顶点"）。
            const float t = (span > 1.0e-6f) ? (w - lv[j]) / span : 1.0f;
            scW[0] = lv[j];
            scV[0] = 1.0f - t;
            scW[1] = lv[j + 1];
            scV[1] = t;
        }

        for (const MorphRigChannel::Point& p : rc.points) {
            if (p.target == kNoMorphTarget) continue;   // 只当断点
            float s = 0.0f;
            if (scW[0] >= 0.0f && std::abs(p.weight - scW[0]) <= 1.0e-6f)
                s = scV[0];
            else if (scW[1] >= 0.0f && std::abs(p.weight - scW[1]) <= 1.0e-6f)
                s = scV[1];
            if (s > 0.0f) add(p.subMesh, p.target, s);
        }
    }
}

bool setMorphWeight(const MorphRig& rig, std::vector<float>& weights,
                    const std::string& channel, float w) {
    const int ci = rig.channelIndex(channel);
    if (ci < 0) return false;
    if (weights.size() < rig.channelCount()) weights.resize(rig.channelCount(), 0.0f);
    weights[static_cast<std::size_t>(ci)] = w;
    return true;
}

float morphWeight(const MorphRig& rig, const std::vector<float>& weights,
                  const std::string& channel) {
    const int ci = rig.channelIndex(channel);
    if (ci < 0 || static_cast<std::size_t>(ci) >= weights.size()) return 0.0f;
    return weights[static_cast<std::size_t>(ci)];
}

} // namespace assets
