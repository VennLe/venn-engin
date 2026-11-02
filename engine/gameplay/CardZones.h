#pragma once
// ============================================================
// gameplay/CardZones —— 牌库 / 手牌 / 弃牌堆 / 消耗区 / 结算区
//
// ------------------------------------------------------------
// 为什么卡牌"数据层"值得单独一个文件
// ------------------------------------------------------------
//   看起来牌堆就是个 `vector<Card>`，加几个 `shuffle` 就完了。实际把它
//   做错的方式有五种，而且五种都是**在别的系统里显形**：
//
//   1) **用下标当身份**。手牌一重排（拖拽 / 按费用排序 / 使用后移除），
//      下标全变。存档里存"第 3 张牌"→ 读档之后第 3 张是另一张牌。
//      本题解：每张牌有 `uid`（造牌时分配，全局唯一、永不复用）。
//
//   2) **打出的牌立刻进弃牌堆**。于是"这张牌结算到一半"的中间态不存在，
//      任何在结算里读 `discard.size()` 的效果（"弃牌越多伤害越高"）都会
//      在结算**之前**就看到它自己 → 数值多算一张。
//      本题解：第四区 `Limbo`（结算区）。打出 = Hand → Limbo，
//      结算完 = Limbo → Discard/Exhaust。**Limbo 非空就是这个 bug 的哨兵**。
//
//   3) **抽牌与表现共用一条随机流**。多放一个粒子就改变下一张抽到的牌。
//      本题解：`draw()` / `shuffleDraw()` **只接受调用方传进来的 `Random&`**，
//      自己不开流。传 `streams.gameplay` 就对了（见 GameRandom.h 的三流分离）。
//      ⚠ 故意不存一个成员 `Random`：那样"谁在用这条流"就查不清了，而且
//        牌堆存档会把 rand 状态一起带走，与引擎的种子管理打架。
//
//   4) **牌库空了就抽不到**。真实卡牌规则是"弃牌堆洗回牌库"（reshuffle）。
//      忘了它 → 一局打到中盘手牌抽干、"抽牌"类效果全部哑火，而报错也没有
//      （`draw()` 返回 0 与"游戏还在正常跑"长得一样）。
//      本题解：`draw()` 自动洗回，并把**洗回次数**记在 `reshuffles()` 上 ——
//      它是可统计的玩法数据，也是判据能验"确实洗了而不是凭空生牌"的唯一途径。
//
//   5) **手牌溢出把牌烧掉**。抽 5 张但上限只剩 2 → 经典实现是"抽了就丢"。
//      本题解：抽到手牌满就**停止抽**，剩下的留在牌库（不浪费玩家的牌）。
//
// ------------------------------------------------------------
// 分层
// ------------------------------------------------------------
//   只依赖 GameRandom（PCG32）。不碰 ECS / 序列化 / Vulkan —— 所以
//   `--selftest gameplay` 的 M 组不需要窗口，也能把上面五条全钉住。
//   场景存档在 `SceneSerializer` 里转 JSON（这边只给文本行 `serialize()`，
//   与 `TurnQueue` 同一套路：引擎层不引 nlohmann）。
// ============================================================

#include "gameplay/GameRandom.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gameplay {

// ---------------------------------------------------------------- 卡牌定义

enum class CardKind { Attack, Skill, Power, Status, Curse };
enum class CardTarget { Enemy, AllEnemies, Self, AllAllies, Random, None };

const char* cardKindName(CardKind k);      // "attack" / "skill" / ...
const char* cardTargetName(CardTarget t);  // "enemy" / "allEnemies" / ...
// 配表容错：认不出的字符串**保持 out 不变**并返回 false（调用方打 WARN）。
// ⚠ 不要"猜一个最像的"：把 "skil" 猜成 Skill 会让一张写错的表静默生效。
bool parseCardKind(const std::string& s, CardKind& out);
bool parseCardTarget(const std::string& s, CardTarget& out);

// 一条效果。**数据，不是代码** —— 结算在 `CardResolver`（P3 后续）里做。
// 这里只负责"能完整存下来、能从配表读出来、能被人读懂"。
struct CardEffect {
    enum class Kind { Damage, Heal, Status, Shield, Draw, Energy };
    Kind kind = Kind::Damage;
    // 留空 = 用卡自己的 `target`。留空是最常见的情况，所以默认空。
    std::string target;
    // Damage / Heal / Shield 的数值；Status 的**层数**（不是数值）；
    // Draw 的**张数**；Energy 的点数。同一个字段四种含义是有意的：
    // 配表里一列 `amount` 比六列 `damageValue / healValue / ...` 好维护。
    float amount = 0.0f;
    // Damage 的元素（"physical" / "fire" ...）；Status 的状态 id；
    // Shield 留空。
    std::string type;
    // 重复次数（"造成 3 次 2 点伤害" = amount 2 / count 3）。
    int count = 1;
};

const char* cardEffectKindName(CardEffect::Kind k);
bool parseCardEffectKind(const std::string& s, CardEffect::Kind& out);

struct CardDef {
    // 配表 key。⚠ **不能含空格**：文本存档按空格切字段。
    std::string id;
    std::string name;
    std::string text;         // 描述（可含空格与换行，只进 UI）
    CardKind kind = CardKind::Attack;
    CardTarget target = CardTarget::Enemy;
    int cost = 1;
    // 打出后进**消耗区**而不是弃牌堆（"这把武器用一次就没了"）。
    bool exhaust = false;
    // 诅咒 / 状态牌：占手牌、**打不出去**。`play()` 会拒绝它。
    bool unplayable = false;
    // 升级后的费用。-1 = 升级不改费用。
    // ⚠ 升级**不改名字/描述**：那两样由内容侧另存一份，不在这里拼字符串
    //   （拼出来的 "打击+" 在本地化之后是错的，中文该是"打击+"，日文是
    //   "ストライク+"，拼 `name + "+"` 恰好都还行，但"升级后描述"不能拼）。
    int upgradedCost = -1;
    std::vector<CardEffect> effects;
};

// 牌堆里的一张牌 = 定义 id + 实例状态。
struct CardInstance {
    uint32_t uid = 0;      // 全局唯一，由 CardZones 分配，**永不复用**
    std::string defId;     // 指向 CardDef::id
    int upgrade = 0;       // 升级次数（0 = 未升级）
};

// ---------------------------------------------------------------- 五个区
//
// ⚠ 顺序是**存档格式的一部分**（序列化按这个顺序遍历）：只能在末尾追加。
enum class CardPile { Draw, Hand, Discard, Exhaust, Limbo };
constexpr int kCardPileCount = 5;

const char* cardPileName(CardPile p);   // 小写，序列化用
bool parseCardPile(const std::string& s, CardPile& out);

// ---------------------------------------------------------------- 示例定义表
//
// 与 `presetPoison()` / `presetShield()` 同一套路：**唯一一份定义**，
// Inspector 按钮、Cards 面板、自检、判据全调这里。
//
// ⚠ 它是**冒烟工具，不是游戏设计**。P3-1（配表进运行时）落地之后，
//   正式内容从 `assets/data/cards.csv` 读（见 `gameplay/CardTable.h`），
//   这份表只留作"--selftest 与夹具的默认世界"。所以数值**故意很小、
//   很容易口算**：判据要能一眼看出"这一下打了多少"，而不是去猜设计意图。
std::vector<CardDef> presetCardDefs();

// ---------------------------------------------------------------- 牌堆

class CardZones {
public:
    // ---- 定义表 ----
    // 读档 / 进战斗之前**必须**先 `setDefs()`。理由见 `clear()` 的注释。
    void setDefs(std::vector<CardDef> defs);
    const std::vector<CardDef>& defs() const { return m_defs; }
    const CardDef* def(const std::string& id) const;
    // 某张**实例**当前该用的费用（考虑升级）。
    int costOf(const CardInstance& c) const;

    // ---- 造牌 / 删牌 ----
    // 往牌库（Draw）加 `count` 张，返回第一张的 uid（0 = 一张都没造）。
    // `defId` 不在定义表里也能造（存档里可能有旧卡），但会少一套数值 ——
    // 这是有意的：读档不能因为"这张卡的表被删了"整个失败。
    uint32_t create(const std::string& defId, int count = 1);
    // 从所在区彻底删掉（"这张牌被移除了"）。返回是否找到。
    bool erase(uint32_t uid);

    // 直接塞一张牌。**只给读档用**（场景 JSON 是结构化存区，不走文本行
    // `deserialize`）：它不分配 uid，传进来的 uid 原样使用。
    // `uid` 已存在 → 返回 false（挡住"手改存档写重复了"）。
    bool put(CardPile p, const CardInstance& c, int pos = -1);
    // 恢复 uid 分配器的下界。`v <= 现有最大 uid` 时**忽略**（只往上抬）。
    void reserveUid(uint32_t v);
    void setCounters(int reshuffles, int plays);
    uint32_t nextUid() const { return m_nextUid; }

    // ---- 查询 ----
    const std::vector<CardInstance>& pile(CardPile p) const;
    std::size_t count(CardPile p) const;
    std::size_t total() const;
    bool contains(uint32_t uid) const;
    // 这张牌在哪一区（以及区内下标）。找不到返回 false，出参不动。
    bool locate(uint32_t uid, CardPile* where = nullptr, int* pos = nullptr) const;
    const CardInstance* find(uint32_t uid, CardPile* where = nullptr) const;
    uint32_t uidAt(CardPile p, int index) const;   // 越界返回 0

    // ---- 移动 ----
    // **唯一的区转移入口**。`pos < 0` = 追加到末尾；否则 `pos` 是**目标位置**
    // （0-based，越界当末尾）。契约一句话：
    //     move(uid, to, pos) 之后  uidAt(to, pos) == uid
    // 同区（`from == to`）且给 `pos` = 区内重排（手牌拖动排序用它）。
    // ⚠ 它**不检查手牌上限**：上限是 `draw()` 的规则，`move()` 是底层动作。
    //   在这里也拦一次的话，"读档恢复 15 张手牌"会因为上限 10 而失败。
    bool move(uint32_t uid, CardPile to, int pos = -1);

    // ---- 手牌上限 ----
    void setHandLimit(int v);   // <= 0 = 不限
    int handLimit() const { return m_handLimit; }

    // ---- 流程 ----
    // 抽 `n` 张，返回**实际进手牌的张数**。抽干（牌库+弃牌都空）或手牌满
    // 就提前停。牌库空时自动把弃牌堆洗回（见文件头第 4 条）。
    int draw(int n, Random& rng);
    // 手牌全部进弃牌堆（回合结束）。返回弃了几张。
    int discardHand();
    // 打出：Hand → Limbo。`unplayable` 的牌返回 false。
    // ⚠ **不检查费用/能量**：那是战斗规则，牌堆只负责"这张牌能不能离开手牌"。
    //   在这里顺便判费用的话，会出现"测试时能打出、实战打不出"两套判定。
    bool play(uint32_t uid);
    // 结算完：Limbo → Discard（或按 `def.exhaust` 进 Exhaust）。
    bool resolve(uint32_t uid);
    // 收拾残局：所有 Limbo 按 `resolve()` 的规则落地。返回张数。
    // 卡牌里"结算到一半被中断"（读档 / 退出战斗）必须调它，否则这些牌会在
    // 存档里留在一个永远不会被读到的区。
    int sweepLimbo();
    void shuffleDraw(Random& rng);

    // ---- 统计 ----
    int reshuffles() const { return m_reshuffles; }
    // 上一次 `draw()` 里是否发生了洗回（UI 要播"弃牌堆洗回"的动画）。
    bool lastDrawReshuffled() const { return m_lastReshuffle; }
    int plays() const { return m_plays; }

    // 清空所有区与计数器。
    // ⚠⚠ **不清定义表**：读档的顺序是 `clear() → setDefs() → deserialize()`，
    //   如果这里把 defs 也清了，中间那步就变成"必须"，漏了就整副牌变未知卡
    //   （而未知卡不报错，只是打出来没有任何效果）。
    //   反过来，defs 也**不属于存档**：它是内容（配表），不是这一局的进度。
    void clear();

    // `draw=5 hand=4 discard=2 exhaust=1 limbo=0 uid=13 reshuffle=0` —— 一行。
    std::string dump() const;

    // 文本存档。每行一张牌：`<pile> <defId> <uid> <upgrade>`；
    // 首行是 `handLimit=N nextUid=N reshuffles=N plays=N`。
    // ⚠ `Limbo` 也会被写出来 —— 存到一半的牌必须能读回来（读档时会
    //   `sweepLimbo()`，但"读到 Limbo"这件事本身是判据 N7 的落点）。
    std::string serialize() const;
    bool deserialize(const std::string& text, std::string* err = nullptr);

private:
    std::vector<CardInstance>& pileMut(CardPile p);
    int indexOf(CardPile p, uint32_t uid) const;

    std::vector<CardDef> m_defs;

    std::vector<CardInstance> m_draw;
    std::vector<CardInstance> m_hand;
    std::vector<CardInstance> m_discard;
    std::vector<CardInstance> m_exhaust;
    std::vector<CardInstance> m_limbo;

    int m_handLimit = 10;
    uint32_t m_nextUid = 1;
    int m_reshuffles = 0;
    int m_plays = 0;
    bool m_lastReshuffle = false;
};

} // namespace gameplay
