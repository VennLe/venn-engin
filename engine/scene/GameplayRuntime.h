#pragma once
// ============================================================
// scene/GameplayRuntime —— 世界 ↔ 玩法数值的桥（P2）
//
// 它只做三件事，一件都不多：
//   ① 把 `ecs::GameplayComponent`（权威数据）**现算**成 `AttributeSet`
//      （派生视图），改完再写回；
//   ② 把 `gameplay::` 里那几个纯函数（伤害 / 状态 / 回合）串到实体上；
//   ③ 维护一份 `TurnQueue`，并用组件里的 `gauge` 让"读档后出手顺序不变"。
//
// 为什么住在 `scene/` 而不是 `gameplay/`：它要遍历 `ecs::World`，
// 而 `gameplay/` 那一层是**纯数据 + 纯函数**（连 `<vector>` 之外都不碰），
// 所以 `--selftest gameplay` 才能在没有任何场景/设备时跑完。分层上
// `scene → gameplay` 单向，`gameplay → ecs` 不存在。
//
// ⚠⚠ **不要在这里缓存算好的数值**。每次 `attr()` 都从组件现算：
//   属性数量是个位数，"每帧算几十次"的代价可以忽略，而缓存的失效漏一次
//   的症状是"面板显示 94、实际扣血按 100 算"。
// ============================================================

#include "ecs/Entity.h"
#include "gameplay/CardFx.h"
#include "gameplay/CardZones.h"
#include "gameplay/DamageNumbers.h"
#include "gameplay/GameRandom.h"
#include "gameplay/GameplayTypes.h"
#include "gameplay/Stats.h"
#include "gameplay/TurnQueue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ecs {
// 只前向声明（下面两个私有方法的参数）：把 `ecs/Components.h` 引进这个头会
// 让每个 include 它的编辑器面板都吃下 2000 行组件定义。
struct GameplayComponent;
struct CardDeckComponent;
} // namespace ecs

namespace scene {

// 一条"卡牌动作"的表现事件（P3-4）。
//
// 为什么在 `scene/` 而不在 `gameplay/`：它带着 `ecs::Entity`，而
// `gameplay/` 那一层**不认识 ecs**（`scene → gameplay` 单向）。模板表
// （纯数据）留在 `gameplay/CardFx.h`，事件（带实体）住这里。
//
// 为什么是"排队给宿主"而不是在 `castCard` 里直接播特效：
// `GameplayRuntime` 是**编辑器与运行时共用**的（面板上的 `cards.cast`
// 也走它），而两边的摄像头、音频、粒子宿主都不是同一个。当场播 = 编辑器
// 里点一下命令就在场景里喷一把火，而且还关不掉。
struct CardFxEvent {
    gameplay::CardFxKind kind = gameplay::CardFxKind::Play;
    ecs::Entity target{};      // 播在谁身上（`atTarget == false` 时宿主换成持牌者）
    ecs::Entity owner{};
    float amount = 0.0f;
};

class Scene;

class GameplayRuntime {
public:
    // 日志前缀。编辑态用 `GP-`、运行时用 `GP-RT-` —— 分开的理由与
    // `FX-` / `FX-RT-` 一样：混在一起时，一边的日志会兜住另一边的红。
    static constexpr const char* kLogPrefix = "GP-";

    void attach(Scene* s) { m_scene = s; }
    bool ready() const { return m_scene != nullptr; }
    Scene* scene() { return m_scene; }

    // 随机种子。**同一 seed 重开 → 同样的抽牌顺序**，这是"可复现"的入口。
    void setSeed(uint64_t seed);
    uint64_t seed() const { return m_seed; }
    gameplay::RandomStreams& streams() { return m_streams; }
    const gameplay::RandomStreams& streams() const { return m_streams; }

    // ---- 读 ----
    // 某条属性的当前值（含状态加成）。没有组件 / 没有该属性 → fallback。
    float attr(ecs::Entity e, const std::string& name, float fallback = 0.0f) const;
    std::string attrLine(ecs::Entity e) const;    // `hp=100.00 attack=20.00`
    std::string statusLine(ecs::Entity e) const;  // `poison x3 (2) shield#2 x1 (1)`
    std::string explainLine(ecs::Entity e) const; // 每条属性的 explain，多行

    // ---- 写（全部返回一行明细；失败时以 `FAIL: ` 开头）----
    std::string addStatus(ecs::Entity e, const gameplay::StatusDef& def);
    std::string removeStatus(ecs::Entity e, const std::string& id);
    std::string heal(ecs::Entity e, float amount);
    // 结算一次伤害。目标 hp 会被扣，护盾会被消耗。
    std::string damage(ecs::Entity from, ecs::Entity to,
                       const gameplay::DamageSpec& spec);
    // 整个场景跑一次"回合末"：所有带组件的单位结算 dot 并推进状态寿命。
    // ⚠ dot 用**回合末之前**的属性结算（本回合到期的增益仍然生效）。
    std::string tickTurn();

    // ---- 回合队列 ----
    // 从组件重建（**含 gauge 回填**）。实体增删之后必须调一次。
    void rebuildQueue();
    // 把队列里的 gauge 写回组件（推进之后调；存档前也调）。
    void storeQueue();
    gameplay::TurnQueue& queue() { return m_queue; }
    const gameplay::TurnQueue& queue() const { return m_queue; }
    std::string advance(float dt);          // 推进并返回一行明细
    std::string orderLine(int n) const;     // `A(2.50) > B(5.00) > C(10.00)`
    // 一个人跑完自己的回合（行动条清零 + gauge 写回组件）。
    std::string endTurn();

    std::string stateLine() const;          // `units=3 queued=3 seed=... turn=...`

    // ---- 牌堆（P3-3）----
    //
    // 组件是权威数据（`ecs::CardDeckComponent`），这里只提供"带随机流与日志"
    // 的动作。抽牌一律走 `streams().gameplay` —— 表现层的随机绝对不能碰牌序
    // （见 `gameplay/GameRandom.h` 的三流分离）。
    //
    // ⚠ 这几个方法**不会替实体挂组件**。要不要挂是调用方的决定（面板上要
    //   "加牌组"按钮，脚本里"没牌组的单位抽牌"应该是一个明确的失败而不是
    //   静默新建一副空牌）。
    ecs::CardDeckComponent* deck(ecs::Entity e);
    const ecs::CardDeckComponent* deck(ecs::Entity e) const;
    // 没有组件就挂一个空的（面板"加牌组"按钮 / 夹具用）。
    std::string ensureDeck(ecs::Entity e);

    std::string cardLine(ecs::Entity e) const;
    // `draw=5 hand=4 ...` + 每个区的内容（多行，`GP-CARDS-PILE` 每条一行）。
    std::string cardDetail(ecs::Entity e) const;
    std::string cardDraw(ecs::Entity e, int n);
    // 按**手牌下标**打出。为什么用下标而不是 uid：面板/命令/脚本说的都是
    // "第几张牌"，让他们自己从 uid 反查下标等于多一份出错的地方。
    // 打出的牌 uid 记在 `lastCard()`（手牌下标在打出后已经变了）。
    std::string cardPlay(ecs::Entity e, int handIndex);
    // 结算 `lastCard()` 那一张（Limbo → 弃牌/消耗）。
    std::string cardResolveLast(ecs::Entity e);
    std::string cardDiscardHand(ecs::Entity e);
    // 洗牌库（用 gameplay 流）。
    std::string cardShuffle(ecs::Entity e);

    int cardCount(ecs::Entity e, gameplay::CardPile p) const;
    uint32_t lastCard() const { return m_lastCard; }

    // ---- 卡牌表现事件（P3-4）----
    //
    // `castCard` 只**记账**，不播。宿主（runtime / 编辑器 Play）每帧取走并
    // 清空；不取也不会堆积 —— 队列有上限，满了丢最老的（一场战斗里
    // 没被消费的事件本来就是"没人看的表现"）。
    const std::vector<CardFxEvent>& cardFxEvents() const { return m_cardFx; }
    std::vector<CardFxEvent> takeCardFxEvents();
    void clearCardFxEvents() { m_cardFx.clear(); }
    // 队列上限。一轮 AoE 卡最多十几条，64 足够；有上限是为了让"宿主忘了取"
    // 变成一个确定的小泄漏而不是无限增长。
    static constexpr std::size_t kMaxCardFxEvents = 64;

    // ---- 配表定义（P3-1）----
    //
    // 把配表读出来的定义灌进场景里**所有**牌堆，返回被灌的牌堆数。
    //
    // ⚠⚠ **读档之后必须调**：`CardZones::m_defs` 是**内容**（配表），不进
    //   场景文件；没灌之前每张牌都 `known=0`，打出来什么都不发生，而且
    //   一行错都没有（`cardDetail()` 的 `known=` 是唯一的诊断入口）。
    //   编辑器与运行时两端都在场景加载之后立刻调它。
    int setCardDefs(const std::vector<gameplay::CardDef>& defs);

    // 直接调整一条属性的**白值**（`Energy` 效果 / 调试用）。
    // 走 load → 改 base → store 三步，与 `heal()` 同一条路。
    // path 上没这条属性时**建出来**（min 0），因为"能量"这类计数器由内容
    // 决定，不在 `defaultAttributes()` 里。
    std::string addAttr(ecs::Entity e, const std::string& name, float delta);

    // 打出**并结算**一张牌：Hand → Limbo → 逐条效果 → Discard/Exhaust。
    //
    // 与 `cardPlay` + `cardResolveLast` 的区别：那两个是"搬牌"，
    // 这个会真的改数值（伤害 / 治疗 / 状态 / 护盾 / 抽牌 / 能量）。
    //
    // ⚠ 定义查不到时 **不搬牌**，直接返回 `ok=0 reason=unknown-def`：
    //   搬了再静默丢弃＝"这张牌打出去什么也没发生"，而它与"配表没生效"
    //   完全一样 —— 那正是 P3-1 最贵的一类 bug。
    // `target` 只对 `Enemy` 类生效；`AllEnemies` = 场上除自己以外的全部
    // 带玩法组件的单位（本引擎还没有阵营概念，Documented 在 docs/cards.md）。
    std::string castCard(ecs::Entity owner, int handIndex, ecs::Entity target);

    // "敌方"目标 = 除 `exclude` 以外**第一个**带玩法组件的实体。没有 → 无效实体。
    //
    // ⚠ 本引擎还没有阵营概念，所以"敌方"就是"别人"。面板/调试用它把
    //   `Enemy` 类的牌打在**看得见的那个单位**身上（打自己会让
    //   `Enemy` 目标解析落空，症状是"这张牌打出去一点伤害都没有"）。
    //   加阵营时的唯一改动点：把这里换成按 team 过滤。
    ecs::Entity firstOtherGameplayEntity(ecs::Entity exclude) const;

    // ---- 窄接口（给脚本动词用）----
    //
    // 上面那几个方法返回的是**一行给人看的明细**；脚本要的是数值。
    // 让脚本去解析那行字符串是本仓明确反对的做法（格式一改就静默错）。
    //
    // `addPresetStatus` 返回加完之后的**总层数**（-1 = 未知预设 / 没组件）。
    int addPresetStatus(ecs::Entity e, const std::string& which);
    // 某个状态的总层数（同名独立实例相加）。
    int stacksOf(ecs::Entity e, const std::string& id) const;
    // 上一次 `damage()` 的完整结果（脚本读 `.dealt`，面板读明细）。
    const gameplay::DamageResult& lastDamage() const { return m_lastDamage; }
    // 上一次 `tickTurn()` 结算了几条 dot。
    int lastDotCount() const { return m_lastDots; }

private:
    ecs::GameplayComponent* comp(ecs::Entity e);
    const ecs::GameplayComponent* comp(ecs::Entity e) const;
    // build → 交给 fn 改 → store。三处（attr/status/damage）共用。
    void load(ecs::Entity e, gameplay::AttributeSet& attrs,
              gameplay::StatusSet& statuses) const;
    void store(ecs::Entity e, const gameplay::AttributeSet& attrs,
               const gameplay::StatusSet& statuses);

    Scene* m_scene = nullptr;
    uint64_t m_seed = 20261103u;
    gameplay::RandomStreams m_streams;
    gameplay::TurnQueue m_queue;
    // 观测值（不参与计算，只给外部读 —— 脚本 / 面板 / 判据）。
    gameplay::DamageResult m_lastDamage;
    int m_lastDots = 0;
    uint32_t m_lastCard = 0;
    std::vector<CardFxEvent> m_cardFx;
    // 只给 `castCard` 用的记账口（满了丢最老的）。
    void pushCardFx(gameplay::CardFxKind kind, ecs::Entity target,
                    ecs::Entity owner, float amount);
};

} // namespace scene
