#include "gameplay/GameRandom.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gameplay {

// PCG32（XSH-RR）。参考实现 pcg-c-basic；这里的 noexcept 语义与它一致：
// 同一个 (state, inc) 永远给出同一个序列，跨平台、跨编译器。
void Random::seed(uint64_t initState, uint64_t seq) {
    m_state = 0u;
    // inc 必须是奇数（PCG 的要求）；`seq << 1 | 1` 是最常见的写法。
    m_inc = (seq << 1u) | 1u;
    nextU32();
    m_state += initState;
    nextU32();
}

uint32_t Random::nextU32() {
    const uint64_t old = m_state;
    m_state = old * 6364136223846793005ULL + m_inc;
    const uint32_t xorshifted =
        static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
    const uint32_t rot = static_cast<uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
}

float Random::nextFloat() {
    // 24 位而不是 32 位：float 的尾数只有 24 位，取 32 位再除会有
    // "1.0 永远取不到但 0.99999994 出现两次"的不均匀。
    return static_cast<float>(nextU32() >> 8) * (1.0f / 16777216.0f);
}

float Random::range(float lo, float hi) { return lo + (hi - lo) * nextFloat(); }

int Random::intRange(int lo, int hi) {
    if (hi <= lo) return lo;
    const uint32_t span = static_cast<uint32_t>(hi - lo) + 1u;
    // 用乘法缩放而不是 `% span`：取模在 span 不整除 2^32 时有轻微偏置，
    // 抽卡这种"每张牌概率必须精确"的场合会被玩家抓出来。
    const uint32_t r = static_cast<uint32_t>(
        (static_cast<uint64_t>(nextU32()) * span) >> 32);
    return lo + static_cast<int>(r);
}

bool Random::chance(float percent) {
    // 见头文件：0 与 100 **不消耗**序列。
    if (percent <= 0.0f) return false;
    if (percent >= 100.0f) return true;
    return nextFloat() * 100.0f < percent;
}

int Random::pickIndex(std::size_t n) {
    if (n == 0) return -1;
    return intRange(0, static_cast<int>(n) - 1);
}

bool Random::setState(uint64_t s, uint64_t inc) {
    if (s == 0u) return false;          // 0 状态是退化流
    m_state = s;
    m_inc = inc | 1u;                   // inc 必须奇数
    return true;
}

std::string Random::dump() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "state=%llu inc=%llu",
                  static_cast<unsigned long long>(m_state),
                  static_cast<unsigned long long>(m_inc));
    return std::string(buf);
}

bool Random::parse(const std::string& dump) {
    unsigned long long s = 0, i = 1;
    if (std::sscanf(dump.c_str(), "state=%llu inc=%llu", &s, &i) != 2) return false;
    return setState(static_cast<uint64_t>(s), static_cast<uint64_t>(i));
}

void RandomStreams::seedAll(uint64_t base) {
    // 三条流的 seq 不同 → 同一 seed 下互不相关。
    // ⚠ 不用 `base+1 / base+2` 作 initState：那样三条流的**前几个输出**
    //   会高度相关（PCG 对相邻种子有一定局部性），而"抽牌序列和掉落序列
    //   同步抖动"是最难查的一类。
    gameplay.seed(base, 0x67616d65u);   // "game"
    loot.seed(base, 0x6c6f6f74u);       // "loot"
    fx.seed(base, 0x5f78785fu);         // "_xx_"
}

const char* randomStreamName(int stream) {
    switch (stream) {
    case 0: return "gameplay";
    case 1: return "loot";
    case 2: return "fx";
    default: return "?";
    }
}

} // namespace gameplay
