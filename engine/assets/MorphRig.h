#pragma once
// ============================================================
// assets/MorphRig —— "滑条 → 每子网格 target 权重"的求解器（§15）
//
// 为什么需要这一层，而不是让调用方直接写 `weights[target]`：
//
//   1. **滑条与 target 不是 1:1**。Blender 的 in-between shape key 让一个
//      滑条在中途经过若干形状，落盘后就是"一个 channel 挂多个 target"。
//      直接写 target 权重等于要求每个调用方自己实现分段线性插值。
//
//   2. **同一个滑条跨子网格**。FBX 的 blend deformer 是**按 mesh** 挂的，
//      而一张脸会被材质切成十几个桶（皮肤 / 眉毛 / 睫毛 / 眼白…）。
//      于是"眨眼"在皮肤桶里是 target_3、在睫毛桶里是 target_1 ——
//      名字相同、下标不同。按名字驱动是唯一对用户友好的接口。
//
//   3. **哨兵断点**。某个桶里没有某个 key 的偏移时，那条 key 仍然要参与
//      插值（否则中途形状会被"跳过"），所以它保留在表里但指向
//      `kNoMorphTarget` —— 求值时只当断点、不写权重。
//
// 求值只做加法与线性插值，**每帧每角色一次**，代价与 target 数同阶
// （alicia：88 个 target）。这不是热点。
// ============================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace assets {

// "这个 key 在本子网格里没有对应 target"。必须与 `assetio::kNoMorphTarget`
// 同值（VMeshLoader 里有 static_assert）。
constexpr std::uint32_t kNoMorphTarget = 0xFFFFFFFFu;

struct MorphChannelKey {
    float targetWeight = 1.0f;
    std::uint32_t target = kNoMorphTarget;
};

// 一个子网格自己的滑条表（磁盘上的原样）
struct MorphChannel {
    std::string name;
    std::vector<MorphChannelKey> keys;
};

// 模型级的滑条：同名 channel 跨子网格合并成一条
struct MorphRigChannel {
    std::string name;
    // 按 weight 升序。`target == kNoMorphTarget` = 只当断点。
    struct Point {
        float weight = 1.0f;
        std::uint32_t subMesh = 0;
        std::uint32_t target = kNoMorphTarget;
    };
    std::vector<Point> points;
    // 至少有一个 point 真的写权重（否则整条滑条是空转，Inspector 该隐藏它）
    bool drives = false;
};

class MorphRig {
public:
    // `perSub[i]` = 子网格 i 的滑条表；`targetCounts[i]` = 子网格 i 的
    // target 数。返回后 `channelIndex()` 立即可用。
    void build(const std::vector<std::vector<MorphChannel>>& perSub,
               const std::vector<std::uint32_t>& targetCounts);

    bool empty() const { return m_channels.empty(); }
    std::size_t channelCount() const { return m_channels.size(); }
    const std::vector<MorphRigChannel>& channels() const { return m_channels; }
    const std::string& channelName(std::size_t i) const {
        return m_channels[i].name;
    }
    // 名字 → 通道下标；-1 = 没有这个滑条。
    // 空名字返回 -1（FBX 里存在无名 channel，那类只能靠下标驱动）。
    int channelIndex(const std::string& name) const;

    std::size_t subMeshCount() const { return m_targetCounts.size(); }
    std::uint32_t targetCount(std::uint32_t subMesh) const;

    // 求值：`channelWeights[c]` → `out[sub][target]`。
    // `out` 会被 resize 成 `subMeshCount()` 行、每行 `targetCount(sub)` 个
    // float（开销只在第一次，之后复用同一份）。
    //
    // ⚠ 权重会被 clamp 到 [0,1]：负数或超过 1 的值在分段插值里没有定义，
    //   静默钳位比让形状飞出去好 —— 但**不静默**：调用方拿到的就是钳位后的
    //   结果，调试时能看出来。
    void evaluate(const std::vector<float>& channelWeights,
                  std::vector<std::vector<float>>* out) const;

private:
    std::vector<MorphRigChannel> m_channels;
    std::unordered_map<std::string, int> m_byName;
    std::vector<std::uint32_t> m_targetCounts;
};

// ---- 按名字驱动（唯一推荐的入口）----
//
// 返回值 = 找到并写入。名字不存在时**不改动** `weights`，返回 false ——
// 静默忽略会让"拼错一个名字"表现为"滑条没反应"，比直接报错难查得多。
// `weights` 会按需 resize 到通道数（只增不减）。
bool setMorphWeight(const MorphRig& rig, std::vector<float>& weights,
                    const std::string& channel, float w);
// 读回一个通道的权重（不存在 → 0；越界 → 0）。
float morphWeight(const MorphRig& rig, const std::vector<float>& weights,
                  const std::string& channel);

} // namespace assets
