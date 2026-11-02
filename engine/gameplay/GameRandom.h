#pragma once
// ============================================================
// gameplay/GameRandom —— 确定性随机（PCG32）+ 三流分离
//
// ------------------------------------------------------------
// 为什么不能用 `rand()` / `std::mt19937` + 时间种子
// ------------------------------------------------------------
//   回合制卡牌的核心诉求是**可复现**：抽牌顺序、暴击判定、掉落，一旦
//   "同一份操作两次跑出不同结果"，撤销、回放、联机、自动化判据全都做不了。
//   `rand()` 的序列是实现定义的（换个 CRT 就变），`mt19937` 状态 2.5 KB
//   且跨平台输出虽然有定义但体积大。PCG32 是 64 位状态 + 32 位输出，
//   序列有定义、体积小、可存档、质量够。
//
// ------------------------------------------------------------
// 为什么必须有"多条流"
// ------------------------------------------------------------
//   如果表现（火花角度、音高抖动）和玩法（抽牌、暴击）共用一条流：
//   **多播一个粒子就会改变下一张抽到的牌**。
//   症状是"关掉特效之后抽牌顺序变了"——这种 bug 几乎不可能靠读代码发现，
//   只能靠"从一开始就分两条流"。
//
//   本文件提供三条：`gameplay`（抽牌/判定/伤害）、`loot`（掉落/奖励）、
//   `fx`（表现抖动）。⚠ `fx` 与 `engine/fx/EffectService.cpp` 里那条
//   独立 xorshift **不是**同一个东西：EffectService 那条跑在渲染帧上，
//   这里这条跑在逻辑帧上，两者的消费次数天差地别，合并会让"逻辑帧率变化
//   改变表现随机序列"。
//
// ------------------------------------------------------------
// 存档
// ------------------------------------------------------------
//   `state()` / `setState()` 是**完整**状态（不像 mt19937 要存 624 个字）。
//   存档里存 `state()` + `inc()` 就能精确恢复"接下来会抽到什么"。
//   ⚠ 只存 state 不存 inc 是错的：inc 决定序列步长，恢复之后是另一条流。
// ============================================================

#include <cstdint>
#include <string>
#include <vector>

namespace gameplay {

class Random {
public:
    Random() { seed(0x853c49e6748fea9bULL, 54u); }

    // 种子。`seq` 决定"这是第几条平行流"——同一个 seed + 不同 seq 得到
    // 互不相关的序列，这是三流分离的实现方式。
    void seed(uint64_t initState, uint64_t seq = 54u);

    uint32_t nextU32();
    // [0, 1)
    float nextFloat();
    // [lo, hi)
    float range(float lo, float hi);
    // [lo, hi] 闭区间
    int intRange(int lo, int hi);
    // 百分比判定：`percent` 是 0..100，返回 true 的概率是 percent/100。
    // ⚠ **总是**消耗一次随机数（即使 percent <= 0 或 >= 100）？不 ——
    //   这里刻意反过来：0 与 100 都**不消耗**。理由是"必定/绝不"的判定
    //   不该占用序列，否则给一件装备加 100% 暴击会改变后面的抽牌。
    bool chance(float percent);
    // 从数组里挑一个（空数组返回 -1）
    int pickIndex(std::size_t n);
    template <typename T>
    const T* pick(const std::vector<T>& v) {
        const int i = pickIndex(v.size());
        return i < 0 ? nullptr : &v[static_cast<std::size_t>(i)];
    }
    // 洗牌（Fisher-Yates，确定性的）
    template <typename T>
    void shuffle(std::vector<T>& v) {
        for (std::size_t i = v.size(); i > 1; --i) {
            const std::size_t j =
                static_cast<std::size_t>(intRange(0, static_cast<int>(i) - 1));
            std::swap(v[i - 1], v[j]);
        }
    }

    uint64_t state() const { return m_state; }
    uint64_t inc() const { return m_inc; }
    // 恢复。`s == 0` 会被拒绝（PCG 的 0 状态是退化的死流）。
    bool setState(uint64_t s, uint64_t inc);

    // 存档用：`state=... inc=...`
    std::string dump() const;
    bool parse(const std::string& dump);

private:
    uint64_t m_state = 0;
    uint64_t m_inc = 1;
};

// 三条独立的流。**玩法侧不许直接用 `std::rand`**（没有可复现性）。
struct RandomStreams {
    Random gameplay;   // 抽牌 / 暴击 / 命中
    Random loot;       // 掉落 / 奖励 / 商店
    Random fx;         // 表现抖动（伤害数字偏移、粒子角度）
    // `base` 相同 → 三条流都相同；三条流之间互不影响。
    void seedAll(uint64_t base);
};

const char* randomStreamName(int stream);
constexpr int kRandomStreamCount = 3;

} // namespace gameplay
