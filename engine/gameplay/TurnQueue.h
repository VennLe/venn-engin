#pragma once
// ============================================================
// gameplay/TurnQueue —— 回合调度（行动条 / CTB）
//
// ------------------------------------------------------------
// 为什么用"行动条"而不是"按速度排个序轮流"
// ------------------------------------------------------------
//   按速度排序轮流（speed 100 先动、speed 90 后动）有个致命问题：
//   **速度差 10 和速度差 90 的收益完全一样**。玩家堆速度堆到某个值之后就
//   再也没有收益（反正都在第 1 位），堆不够就永远在第 2 位。这是回合制
//   数值设计里最常见的塌方。
//
//   行动条（charge time battle）把"速度"变成**频率**：
//       每个 tick  `gauge += speed * dt`，谁的 gauge 先满谁先动，动完清零。
//   于是 speed 200 的单位在同样时间里动 2 次。慢速单位的收益是连续的，
//   不存在"卡在阈值上"。
//
// ------------------------------------------------------------
// 三条必须钉住的语义
// ------------------------------------------------------------
// 1) **同 gauge 的先后必须确定**：按 `order`（加入序号）升序，不是"上次谁
//    先动"。用浮点累加就一定会有恰好相等的一天，那时若用"上一次的顺序"
//   或者指针顺序，同一份存档在两次运行里会走出不同的出手顺序。
//
// 2) **`advance(dt)` 一次只出一个人**。它可能"越过"很多个满槽（dt 很大时），
//    剩下的留到下次 —— 因为每个行动都可能改变世界（有人被击杀 → 从队列
//    移除），一次吐出多个就会对"已经不存在的实体"做行动。
//
// 3) **插队（`insertNow`）不改 speed**，只把 gauge 直接顶满。改 speed 的做法
//    会让"额外行动一次"变成"此后永久更快"，两者是完全不同的设计。
//    ⚠ 插队者与已有满槽者的先后仍按 `order` 定 —— 但插队者期望是"下一个
//      一定是我"，所以插队时要给一个**负的 order**（越小越优先），
//      否则速度 999 的 Boss 会把它压回队尾。
//
// 分层：只依赖 `<string>` / `<vector>` / `<cstdint>`，与 Stats / Random 同一层
// （都不碰 ECS / Vulkan），所以 `--selftest gameplay` 不需要窗口。
// ============================================================

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gameplay {

// 队列里的一个行动者。
struct TurnActor {
    // 稳定的外部 id（实体 id / 卡牌实例 id / 敌人编号）。
    // ⚠ 对外**只用这个 id**，返回指针会让调用方在下一次 add/remove 之后
    //   拿到悬空引用（vector 会搬家）。
    uint32_t id = 0;
    std::string name;
    float speed = 100.0f;
    float gauge = 0.0f;      // 0..gaugeMax
    int team = 0;            // 0 = 己方，1 = 敌方（只用于显示分组）
    bool alive = true;
    // 内部：加入顺序，用来给"gauge 相等"定先后。**不参与外部语义**。
    int order = 0;
};

class TurnQueue {
public:
    static constexpr float kDefaultGaugeMax = 1000.0f;

    void setGaugeMax(float v);      // <= 0 会被忽略（钳到默认）
    float gaugeMax() const { return m_gaugeMax; }

    // 加入。`id` 重复 → 返回 false（不覆盖，避免"两个同 id 各自行动"）。
    // `speed <= 0` 会被钳到 1（速度 0 = 永不行动，那是"冻结"，应该用
    // `alive = false` 表达，不要用速度 0 表达）。
    bool add(const TurnActor& a);
    bool remove(uint32_t id);
    bool has(uint32_t id) const;
    // 只读查询。⚠ 返回指针在下一次 add/remove 后失效。
    const TurnActor* find(uint32_t id) const;
    // 标记阵亡：alive = false 且 gauge 清零。**不从列表移除**（战斗结算
    // 还要读它）。返回是否找到。
    bool kill(uint32_t id);
    std::size_t size() const { return m_actors.size(); }
    std::size_t aliveCount() const;
    const std::vector<TurnActor>& actors() const { return m_actors; }
    void clear();

    // 推进 `dt` 秒，返回**本次**轮到的行动者 id（0 = 没人能行动）。
    // 一次只出一个人（见文件头第 2 条）。
    uint32_t advance(float dt);

    // 预读：不改状态，返回"接下来谁先动"（0 = 没人）。
    uint32_t nextActor() const;
    // 距离"下一次有人满槽"还有多少秒。0 = 已经有人满了；< 0 = 没人能行动。
    // ⚠ 它和 `preview()` 必须用同一套 tie-break，否则"预读条显示 A 先动"
    //   而实际 B 先动 —— 判据 K7 就是钉这件事的。
    float timeToNext() const;
    // 预读：`n` 个未来的出手顺序（用剩余时间模拟，不改状态）。
    // 判据 / UI 的"行动顺序条"读它。
    std::vector<uint32_t> preview(int n) const;
    // 预读的"带时刻"版本：`(id, 从此刻起多少秒之后出手)`。时刻**单调不减**
    // （两个人在同一瞬间满槽时，两人时刻相同、按 `order` 排序 —— 这是
    //  "同一 tick 里先后手"，不是 bug）。
    // ⚠ 顺序条**必须**用它而不是 `preview()` + `find(id)->remaining()`：
    //   后者拿的是**原始队列**的行动条，同一个人出现三次就印三次同一个
    //   时刻（面板显示 `A(5.00) > B(10.00) > A(5.00)`，读起来像时间倒流）。
    std::vector<std::pair<uint32_t, float>> timeline(int n) const;

    // 当前正在行动的人（不推进时它就是上一次 `advance` 的结果）。
    uint32_t acting() const { return m_acting; }
    // 已经出手的次数。判据读它（"一次 advance 只出一个人"靠它验，
    // 比数"谁的行动条满了"可靠 —— 满槽的人本来就不止一个）。
    int turnCount() const { return m_turn; }
    uint32_t beginTurn(uint32_t id);   // 显式指定行动者（调试 / 强制）
    // 结束当前行动：行动者 gauge 归零、清 acting。返回它是否确实在行动。
    bool endTurn();

    // 插队：把 `id` 的 gauge 顶满，且排在所有满槽者之前（见文件头第 3 条）。
    bool insertNow(uint32_t id);
    // 取消某人的**下一次**行动（gauge 归零）。若他正在行动，同时清 acting。
    bool cancelAction(uint32_t id);

    // `A(5.00) > B(10.00) > A(11.67)` —— 面板与日志用。括号里是**从现在
    // 起多少秒后出手**（递增），不是剩余槽量。
    std::string orderPreview(int n) const;
    // `acting=B t=3 n=4 alive=3` —— 一行状态。
    std::string dump() const;

    // 存档：完整 actor 表（含 gauge / order / acting）。
    std::string serialize() const;
    bool deserialize(const std::string& text, std::string* err = nullptr);

private:
    TurnActor* findMut(uint32_t id);
    // "谁下一秒会先满"的剩余时间。<= 0 表示已经满了。
    float remaining(const TurnActor& a) const;

    std::vector<TurnActor> m_actors;
    float m_gaugeMax = kDefaultGaugeMax;
    uint32_t m_acting = 0;
    int m_nextOrder = 1;      // 正数递增
    int m_nextOrderFront = 0; // 插队用，递减（负数）
    int m_turn = 0;           // 已经出手的次数（判据 / 存档）
};

} // namespace gameplay
