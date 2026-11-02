#include "scene/GameplayRuntime.h"

#include "core/Logger.h"
#include "ecs/Components.h"
#include "ecs/World.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace scene {

namespace {

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

// StatusSet → 组件里的快照列表。
// ⚠ 这一步不能省：状态到期之后**必须**从组件里消失，否则读档会把它复活。
void toStateList(const gameplay::StatusSet& set, std::vector<gameplay::StatusState>& out) {
    out.clear();
    out.reserve(set.size());
    for (std::size_t i = 0; i < set.size(); ++i) {
        const gameplay::StatusInstance& inst = set.at(i);
        out.push_back(gameplay::toStatusState(inst.def, inst.turnsLeft, inst.stacks));
    }
}

// dot 的元素字符串 → 伤害类型。空 = 魔法（与 StatusDef::dotElement 的约定一致）。
gameplay::DamageType elementToType(const std::string& s) {
    gameplay::DamageType t = gameplay::DamageType::Magic;
    if (s.empty()) return t;
    gameplay::parseDamageType(s, t);
    return t;
}

} // namespace

// ---------------------------------------------------------------- 基础

ecs::GameplayComponent* GameplayRuntime::comp(ecs::Entity e) {
    if (!m_scene || !e.valid()) return nullptr;
    return m_scene->world().get<ecs::GameplayComponent>(e);
}

const ecs::GameplayComponent* GameplayRuntime::comp(ecs::Entity e) const {
    if (!m_scene || !e.valid()) return nullptr;
    return m_scene->world().get<ecs::GameplayComponent>(e);
}

void GameplayRuntime::setSeed(uint64_t seed) {
    m_seed = seed;
    m_streams.seedAll(seed);
    VK_LOG_INFO("%sSEED seed=%llu", kLogPrefix,
                static_cast<unsigned long long>(seed));
}

void GameplayRuntime::load(ecs::Entity e, gameplay::AttributeSet& attrs,
                           gameplay::StatusSet& statuses) const {
    attrs.clear();
    statuses.clear();
    const ecs::GameplayComponent* c = comp(e);
    if (!c) return;
    gameplay::buildAttributes(c->attributes, c->statuses, attrs);
    for (const gameplay::StatusState& s : c->statuses) {
        statuses.restore(gameplay::toStatusDef(s), s.turnsLeft, s.stacks);
    }
}

void GameplayRuntime::store(ecs::Entity e, const gameplay::AttributeSet& attrs,
                            const gameplay::StatusSet& statuses) {
    ecs::GameplayComponent* c = comp(e);
    if (!c) return;
    gameplay::storeBases(attrs, c->attributes);
    toStateList(statuses, c->statuses);
}

float GameplayRuntime::attr(ecs::Entity e, const std::string& name, float fallback) const {
    const ecs::GameplayComponent* c = comp(e);
    if (!c) return fallback;
    gameplay::AttributeSet attrs;
    gameplay::buildAttributes(c->attributes, c->statuses, attrs);
    return attrs.get(name, fallback);
}

std::string GameplayRuntime::attrLine(ecs::Entity e) const {
    const ecs::GameplayComponent* c = comp(e);
    if (!c) return std::string("(no gameplay component)");
    gameplay::AttributeSet attrs;
    gameplay::buildAttributes(c->attributes, c->statuses, attrs);
    // `hp` 放在最前面：它是唯一一个"必须一眼看到"的数。
    std::string s;
    if (attrs.has("hp")) s = fmt("hp=%.2f", static_cast<double>(attrs.get("hp")));
    const std::string rest = attrs.dump();
    return s.empty() ? rest : (s + " " + rest);
}

std::string GameplayRuntime::explainLine(ecs::Entity e) const {
    const ecs::GameplayComponent* c = comp(e);
    if (!c) return std::string("(no gameplay component)");
    gameplay::AttributeSet attrs;
    gameplay::buildAttributes(c->attributes, c->statuses, attrs);
    std::string s;
    for (std::size_t i = 0; i < attrs.size(); ++i) {
        s += attrs.nameAt(i);
        s += ": ";
        s += attrs.at(i).explain();
        s += "\n";
    }
    return s;
}

std::string GameplayRuntime::statusLine(ecs::Entity e) const {
    const ecs::GameplayComponent* c = comp(e);
    if (!c) return std::string("(no gameplay component)");
    if (c->statuses.empty()) return std::string("(none)");
    std::string s;
    for (const gameplay::StatusState& st : c->statuses) {
        if (!s.empty()) s += " ";
        s += fmt("%s x%d (%d)", st.id.c_str(), st.stacks, st.turnsLeft);
    }
    return s;
}

// ---------------------------------------------------------------- 状态

std::string GameplayRuntime::addStatus(ecs::Entity e, const gameplay::StatusDef& def) {
    if (!comp(e)) return std::string("FAIL: no gameplay component");
    gameplay::AttributeSet attrs;
    gameplay::StatusSet statuses;
    load(e, attrs, statuses);

    const gameplay::StatusAddResult r = statuses.add(def);
    // ⚠ 即使 `added == false`（层数满）也要 store：`add()` 可能刷新了回合数。
    store(e, attrs, statuses);
    // ⚠ 状态变了必须**重建**属性（不是增量）—— 见 Stats.h 文件头第 3 条。
    load(e, attrs, statuses);

    if (!r.added) {
        return fmt("skipped status=%s reason=%s stacks=%d", def.id.c_str(),
                   r.reason.c_str(), r.stacks);
    }
    return fmt("added status=%s stacks=%d turns=%d refreshed=%d", def.id.c_str(),
               statuses.stacks(def.id), statuses.turnsLeft(def.id), r.refreshed ? 1 : 0);
}

std::string GameplayRuntime::removeStatus(ecs::Entity e, const std::string& id) {
    if (!comp(e)) return std::string("FAIL: no gameplay component");
    gameplay::AttributeSet attrs;
    gameplay::StatusSet statuses;
    load(e, attrs, statuses);
    const bool ok = statuses.remove(id);
    store(e, attrs, statuses);
    if (!ok) return fmt("FAIL: status not found id=%s", id.c_str());
    return fmt("removed status=%s", id.c_str());
}

std::string GameplayRuntime::heal(ecs::Entity e, float amount) {
    if (!comp(e)) return std::string("FAIL: no gameplay component");
    gameplay::AttributeSet attrs;
    gameplay::StatusSet statuses;
    load(e, attrs, statuses);
    const float before = attrs.get("hp", 0.0f);
    // 治疗走"加 base 再夹上限"，不是 `consume(-x)`：`consume` 只负责扣。
    const float maxHp = attrs.maxValue("hp");
    if (maxHp > 0.0f) {
        const float target = std::min(before + std::max(amount, 0.0f), maxHp);
        attrs.setBase("hp", attrs.base("hp") + (target - before));
    }
    store(e, attrs, statuses);
    return fmt("heal hp %.1f -> %.1f", static_cast<double>(before),
               static_cast<double>(attrs.get("hp", 0.0f)));
}

// ---------------------------------------------------------------- 伤害

std::string GameplayRuntime::damage(ecs::Entity from, ecs::Entity to,
                                    const gameplay::DamageSpec& spec) {
    if (!comp(to)) return std::string("FAIL: target has no gameplay component");

    gameplay::AttributeSet attacker;
    gameplay::StatusSet attackerStatuses;
    if (comp(from)) load(from, attacker, attackerStatuses);

    gameplay::AttributeSet target;
    gameplay::StatusSet targetStatuses;
    load(to, target, targetStatuses);

    const gameplay::DamageResult r =
        gameplay::resolveDamage(attacker, target, spec, m_streams.gameplay);
    m_lastDamage = r;

    // ⚠ 只回写目标的 base（hp / 护盾）。攻击方的属性没被改（`resolveDamage`
    //   把它当 const），但 `load()` 出来的那份是临时视图，丢掉即可。
    store(to, target, targetStatuses);

    // 明细要打出来：**只打总伤害的话，"护甲没生效"和"易伤没生效"看起来一样**。
    // 前缀 + 一行 summary，steps 逐条跟在后面（判据按 label 找）。
    VK_LOG_INFO("%sDAMAGE %s", kLogPrefix, r.summary().c_str());
    for (const gameplay::DamageStep& s : r.steps) {
        VK_LOG_INFO("%sDAMAGE-STEP %s %.2f->%.2f", kLogPrefix, s.label.c_str(),
                    static_cast<double>(s.before), static_cast<double>(s.after));
    }
    return r.summary();
}

// ---------------------------------------------------------------- 回合末

std::string GameplayRuntime::tickTurn() {
    if (!m_scene) return std::string("FAIL: not attached");

    // 收集一遍再改：`each()` 的回调里改组件是安全的，但**删实体**不安全，
    // 而这里不删实体（状态到期只改组件），所以一次遍历就够。
    int units = 0;
    int dots = 0;
    float totalDot = 0.0f;
    std::string detail;

    m_scene->world().each<ecs::GameplayComponent>(
        [&](ecs::Entity e, ecs::GameplayComponent& c) {
            ++units;
            gameplay::AttributeSet attrs;
            gameplay::StatusSet statuses;
            gameplay::buildAttributes(c.attributes, c.statuses, attrs);
            for (const gameplay::StatusState& s : c.statuses) {
                statuses.restore(gameplay::toStatusDef(s), s.turnsLeft, s.stacks);
            }

            // ① 先收集 dot（**用回合末之前的属性结算**：本回合到期的增益仍然
            //    生效）。收集与推进寿命都在 `tickTurn()` 里，返回的是推进前的值。
            const std::vector<gameplay::StatusSet::DotTick> list = statuses.tickTurn();

            // ② 逐条结算。走 `resolveDamage` 而不是直接减 hp ——
            //    "毒伤吃不吃护盾 / 抗性"是伤害系统的规则，不能在状态里另写一套。
            for (const gameplay::StatusSet::DotTick& d : list) {
                gameplay::DamageSpec spec;
                spec.amount = d.amount;
                spec.type = elementToType(d.element);
                spec.canCrit = false;      // dot 不暴击（否则一回合的毒能秒人）
                spec.source = "status:" + d.statusId;
                const gameplay::DamageResult r =
                    gameplay::resolveDamage(attrs, attrs, spec, m_streams.gameplay);
                ++dots;
                totalDot += r.dealt;
                detail += fmt(" %s=%.1f", d.statusId.c_str(),
                              static_cast<double>(r.dealt));
            }

            // ③ 写回：hp 可能变了、状态可能到期了。
            gameplay::storeBases(attrs, c.attributes);
            toStateList(statuses, c.statuses);
        });

    // 行动条也往前走一回合（一个"回合"= 所有人各行动一次，这里不建模，
    // 只把 gauge 交给 `advance()` 管）。
    m_lastDots = dots;
    const std::string line = fmt("tick units=%d dots=%d dotDamage=%.1f%s%s", units, dots,
                                 static_cast<double>(totalDot),
                                 detail.empty() ? "" : " detail=", detail.c_str());
    VK_LOG_INFO("%sTICK %s", kLogPrefix, line.c_str());
    return line;
}

// ---------------------------------------------------------------- 队列

void GameplayRuntime::rebuildQueue() {
    m_queue.clear();
    if (!m_scene) return;
    // ⚠ 排序的实体 id 决定 `order`（tie-break 依据）。不排序的话"谁先动"
    //   会随组件池的内存布局变化 —— 同一份存档两次加载可能给出不同顺序。
    // ⚠ 这里**拷值不拷指针**：回调里拿到的 `GameplayComponent&` 在池扩容后
    //   就悬空了，而 `add()` 之后还要用这些值。
    struct Row {
        ecs::EntityId id;
        std::string name;
        float speed;
        float gauge;
        int team;
    };
    std::vector<Row> list;
    m_scene->world().each<ecs::GameplayComponent>(
        [&](ecs::Entity e, ecs::GameplayComponent& c) {
            if (!c.inTurnQueue) return;
            Row r;
            r.id = e.id;
            const std::string* n = m_scene->world().name(e);
            r.name = (n && !n->empty()) ? *n : (std::string("E") + std::to_string(e.id));
            r.speed = c.speed;
            r.gauge = c.gauge;
            r.team = c.team;
            list.push_back(std::move(r));
        });
    std::sort(list.begin(), list.end(),
              [](const Row& a, const Row& b) { return a.id < b.id; });

    for (const Row& r : list) {
        gameplay::TurnActor a;
        a.id = r.id;
        a.name = r.name;
        a.speed = r.speed;
        a.gauge = r.gauge;
        a.team = r.team;
        a.alive = true;
        m_queue.add(a);
    }
    VK_LOG_INFO("%sQUEUE %s", kLogPrefix, m_queue.dump().c_str());
}

void GameplayRuntime::storeQueue() {
    if (!m_scene) return;
    for (const gameplay::TurnActor& a : m_queue.actors()) {
        ecs::Entity e = m_scene->world().handle(static_cast<ecs::EntityId>(a.id));
        if (ecs::GameplayComponent* c = comp(e)) {
            c->gauge = a.gauge;
        }
    }
}

std::string GameplayRuntime::advance(float dt) {
    if (m_queue.size() == 0) rebuildQueue();
    const uint32_t id = m_queue.advance(dt);
    const std::string line =
        id == 0 ? fmt("advance dt=%.3f actor=- %s", static_cast<double>(dt),
                      m_queue.dump().c_str())
                : fmt("advance dt=%.3f actor=%u %s", static_cast<double>(dt),
                      static_cast<unsigned>(id), m_queue.dump().c_str());
    VK_LOG_INFO("%sADVANCE %s", kLogPrefix, line.c_str());
    return line;
}

std::string GameplayRuntime::endTurn() {
    const bool ok = m_queue.endTurn();
    if (ok) storeQueue();
    return fmt("endturn ok=%d %s", ok ? 1 : 0, m_queue.dump().c_str());
}

std::string GameplayRuntime::orderLine(int n) const {
    return m_queue.orderPreview(n);
}

int GameplayRuntime::addPresetStatus(ecs::Entity e, const std::string& which) {
    if (!comp(e)) return -1;
    gameplay::StatusDef def;
    // 定义表**只有一份**（`statusDefById`）。这里自己再写一遍 if-chain 的话，
    // 面板按钮加的"中毒"与卡牌结算挂的"中毒"有一天会不一样，而那种不一致
    // 几乎没人会去比对（两处都"看起来对"）。
    if (!gameplay::statusDefById(which, def)) return -1;
    const std::string line = addStatus(e, def);
    if (line.rfind("FAIL", 0) == 0) return -1;
    return stacksOf(e, def.id);
}

int GameplayRuntime::stacksOf(ecs::Entity e, const std::string& id) const {
    const ecs::GameplayComponent* c = comp(e);
    if (!c) return 0;
    int n = 0;
    for (const gameplay::StatusState& s : c->statuses) {
        // 独立叠加的实例 id 带 `#N` 后缀 —— 与 `StatusSet::stacks()` 同一条规则，
        // 两处不一致的话"脚本读到的层数"和"面板显示的层数"会不一样。
        if (s.id == id || s.id.rfind(id + "#", 0) == 0) n += s.stacks;
    }
    return n;
}

std::string GameplayRuntime::stateLine() const {
    int units = 0;
    if (m_scene) {
        m_scene->world().each<ecs::GameplayComponent>(
            [&](ecs::Entity, ecs::GameplayComponent&) { ++units; });
    }
    return fmt("units=%d queued=%d seed=%llu turn=%d", units,
               static_cast<int>(m_queue.size()),
               static_cast<unsigned long long>(m_seed), m_queue.turnCount());
}

// ---------------------------------------------------------------- 牌堆（P3-3）

ecs::CardDeckComponent* GameplayRuntime::deck(ecs::Entity e) {
    if (!m_scene || !e.valid()) return nullptr;
    return m_scene->world().get<ecs::CardDeckComponent>(e);
}

const ecs::CardDeckComponent* GameplayRuntime::deck(ecs::Entity e) const {
    if (!m_scene || !e.valid()) return nullptr;
    return m_scene->world().get<ecs::CardDeckComponent>(e);
}

std::string GameplayRuntime::ensureDeck(ecs::Entity e) {
    if (!m_scene || !e.valid()) return std::string("FAIL: no scene or invalid entity");
    if (ecs::CardDeckComponent* d = deck(e)) {
        VK_LOG_INFO("%sCARDS-ENSURE entity=%u existed=1 %s", kLogPrefix,
                    static_cast<unsigned>(e.id), d->zones.dump().c_str());
        return fmt("CARDS-ENSURE entity=%u existed=1 %s",
                   static_cast<unsigned>(e.id), d->zones.dump().c_str());
    }
    ecs::CardDeckComponent fresh;
    m_scene->world().add<ecs::CardDeckComponent>(e, std::move(fresh));
    VK_LOG_INFO("%sCARDS-ENSURE entity=%u existed=0 %s", kLogPrefix,
                static_cast<unsigned>(e.id),
                m_scene->world().get<ecs::CardDeckComponent>(e)->zones.dump().c_str());
    return fmt("CARDS-ENSURE entity=%u existed=0", static_cast<unsigned>(e.id));
}

int GameplayRuntime::cardCount(ecs::Entity e, gameplay::CardPile p) const {
    const ecs::CardDeckComponent* d = deck(e);
    return d ? static_cast<int>(d->zones.count(p)) : 0;
}

std::string GameplayRuntime::cardLine(ecs::Entity e) const {
    const ecs::CardDeckComponent* d = deck(e);
    if (!d) {
        VK_LOG_INFO("%sCARDS entity=%u state=none", kLogPrefix, static_cast<unsigned>(e.id));
        return fmt("CARDS entity=%u state=none", static_cast<unsigned>(e.id));
    }
    const gameplay::CardZones& z = d->zones;
    const std::string line =
        fmt("CARDS entity=%u draw=%d hand=%d discard=%d exhaust=%d limbo=%d uid=%u "
            "reshuffle=%d plays=%d",
            static_cast<unsigned>(e.id),
            static_cast<int>(z.count(gameplay::CardPile::Draw)),
            static_cast<int>(z.count(gameplay::CardPile::Hand)),
            static_cast<int>(z.count(gameplay::CardPile::Discard)),
            static_cast<int>(z.count(gameplay::CardPile::Exhaust)),
            static_cast<int>(z.count(gameplay::CardPile::Limbo)), z.nextUid(),
            z.reshuffles(), z.plays());
    VK_LOG_INFO("%s%s", kLogPrefix, line.c_str());
    return line;
}

std::string GameplayRuntime::cardDetail(ecs::Entity e) const {
    const ecs::CardDeckComponent* d = deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");
    const gameplay::CardZones& z = d->zones;
    for (int i = 0; i < gameplay::kCardPileCount; ++i) {
        const gameplay::CardPile p = static_cast<gameplay::CardPile>(i);
        const std::vector<gameplay::CardInstance>& v = z.pile(p);
        for (std::size_t k = 0; k < v.size(); ++k) {
            // ⚠ 打印 `def` 是否在定义表里（`known=0`）。读档之后定义表由配表
            //   补，没补之前每张牌都 `known=0` —— 那是正常的，但"配表没加载"
            //   和"这张卡的 id 拼错了"在日志里长得一样，所以必须印出来。
            VK_LOG_INFO("%sCARDS-PILE entity=%u pile=%s idx=%d uid=%u def=%s up=%d known=%d",
                        kLogPrefix, static_cast<unsigned>(e.id),
                        gameplay::cardPileName(p), static_cast<int>(k), v[k].uid,
                        v[k].defId.c_str(), v[k].upgrade,
                        z.def(v[k].defId) ? 1 : 0);
        }
    }
    return cardLine(e);
}

std::string GameplayRuntime::cardDraw(ecs::Entity e, int n) {
    ecs::CardDeckComponent* d = deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");
    if (n <= 0) return std::string("FAIL: draw count must be > 0");
    const int got = d->zones.draw(n, m_streams.gameplay);
    const std::string line =
        fmt("CARD-DRAW entity=%u want=%d got=%d reshuffled=%d draw=%d hand=%d",
            static_cast<unsigned>(e.id), n, got,
            d->zones.lastDrawReshuffled() ? 1 : 0,
            static_cast<int>(d->zones.count(gameplay::CardPile::Draw)),
            static_cast<int>(d->zones.count(gameplay::CardPile::Hand)));
    VK_LOG_INFO("%s%s", kLogPrefix, line.c_str());
    return line;
}

// ---- P3-4 卡牌表现事件 ----
//
// 只记账，不播（见头注释）。⚠ 满了**丢最老的**而不是丢新的：屏幕上正在播
// 的那一批已经过去了，最新一条才是"刚刚发生的"。队列整体搬走的实现
// （`takeCardFxEvents` 直接 move）保证宿主每帧只清一次，不会漏。
void GameplayRuntime::pushCardFx(gameplay::CardFxKind kind, ecs::Entity target,
                                 ecs::Entity owner, float amount) {
    if (m_cardFx.size() >= kMaxCardFxEvents) {
        m_cardFx.erase(m_cardFx.begin());
    }
    CardFxEvent ev;
    ev.kind = kind;
    ev.target = target;
    ev.owner = owner;
    ev.amount = amount;
    m_cardFx.push_back(ev);
}

std::vector<CardFxEvent> GameplayRuntime::takeCardFxEvents() {
    std::vector<CardFxEvent> out;
    out.swap(m_cardFx);
    return out;
}

std::string GameplayRuntime::cardPlay(ecs::Entity e, int handIndex) {
    ecs::CardDeckComponent* d = deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");
    const uint32_t uid = d->zones.uidAt(gameplay::CardPile::Hand, handIndex);
    if (uid == 0) {
        const std::string line =
            fmt("CARD-PLAY entity=%u idx=%d ok=0 reason=no-such-hand-index hand=%d",
                static_cast<unsigned>(e.id), handIndex,
                static_cast<int>(d->zones.count(gameplay::CardPile::Hand)));
        VK_LOG_INFO("%sWARN %s", kLogPrefix, line.c_str());
        return line;
    }
    const gameplay::CardInstance* inst = d->zones.find(uid);
    const std::string defId = inst ? inst->defId : std::string("?");
    if (!d->zones.play(uid)) {
        const std::string line =
            fmt("CARD-PLAY entity=%u idx=%d uid=%u def=%s ok=0 reason=unplayable",
                static_cast<unsigned>(e.id), handIndex, uid, defId.c_str());
        VK_LOG_INFO("%sWARN %s", kLogPrefix, line.c_str());
        return line;
    }
    m_lastCard = uid;
    const std::string line =
        fmt("CARD-PLAY entity=%u idx=%d uid=%u def=%s ok=1 hand=%d limbo=%d discard=%d",
            static_cast<unsigned>(e.id), handIndex, uid, defId.c_str(),
            static_cast<int>(d->zones.count(gameplay::CardPile::Hand)),
            static_cast<int>(d->zones.count(gameplay::CardPile::Limbo)),
            static_cast<int>(d->zones.count(gameplay::CardPile::Discard)));
    VK_LOG_INFO("%s%s", kLogPrefix, line.c_str());
    return line;
}

std::string GameplayRuntime::cardResolveLast(ecs::Entity e) {
    ecs::CardDeckComponent* d = deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");
    if (m_lastCard == 0) return std::string("FAIL: no card has been played");
    const gameplay::CardInstance* inst = d->zones.find(m_lastCard);
    const std::string defId = inst ? inst->defId : std::string("?");
    const uint32_t uid = m_lastCard;
    if (!d->zones.resolve(uid)) {
        const std::string line =
            fmt("CARD-RESOLVE entity=%u uid=%u def=%s ok=0 reason=not-in-limbo",
                static_cast<unsigned>(e.id), uid, defId.c_str());
        VK_LOG_INFO("%sWARN %s", kLogPrefix, line.c_str());
        return line;
    }
    const std::string line =
        fmt("CARD-RESOLVE entity=%u uid=%u def=%s ok=1 discard=%d exhaust=%d limbo=%d",
            static_cast<unsigned>(e.id), uid, defId.c_str(),
            static_cast<int>(d->zones.count(gameplay::CardPile::Discard)),
            static_cast<int>(d->zones.count(gameplay::CardPile::Exhaust)),
            static_cast<int>(d->zones.count(gameplay::CardPile::Limbo)));
    VK_LOG_INFO("%s%s", kLogPrefix, line.c_str());
    return line;
}

std::string GameplayRuntime::cardDiscardHand(ecs::Entity e) {
    ecs::CardDeckComponent* d = deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");
    const int n = d->zones.discardHand();
    const std::string line =
        fmt("CARD-DISCARD-HAND entity=%u n=%d hand=%d discard=%d",
            static_cast<unsigned>(e.id), n,
            static_cast<int>(d->zones.count(gameplay::CardPile::Hand)),
            static_cast<int>(d->zones.count(gameplay::CardPile::Discard)));
    VK_LOG_INFO("%s%s", kLogPrefix, line.c_str());
    return line;
}

std::string GameplayRuntime::cardShuffle(ecs::Entity e) {
    ecs::CardDeckComponent* d = deck(e);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");
    d->zones.shuffleDraw(m_streams.gameplay);
    const std::string line =
        fmt("CARD-SHUFFLE entity=%u draw=%d", static_cast<unsigned>(e.id),
            static_cast<int>(d->zones.count(gameplay::CardPile::Draw)));
    VK_LOG_INFO("%s%s", kLogPrefix, line.c_str());
    return line;
}

// ---------------------------------------------------------------- 配表（P3-1）

int GameplayRuntime::setCardDefs(const std::vector<gameplay::CardDef>& defs) {
    if (!m_scene) return 0;
    int decks = 0;
    // 定义表是**内容**，所有牌堆共用同一份（拷贝进去即可，`setDefs` 会
    // 整体替换）—— 不让每个牌堆各持一份"将来会被改"的定义。
    m_scene->world().each<ecs::CardDeckComponent>(
        [&](ecs::Entity, ecs::CardDeckComponent& c) {
            c.zones.setDefs(defs);
            ++decks;
        });
    VK_LOG_INFO("%sCARD-DEFS decks=%d defs=%zu", kLogPrefix, decks, defs.size());
    return decks;
}

std::string GameplayRuntime::addAttr(ecs::Entity e, const std::string& name,
                                     float delta) {
    if (name.empty()) return std::string("FAIL: empty attribute name");
    if (!comp(e)) return std::string("FAIL: no gameplay component");
    gameplay::AttributeSet attrs;
    gameplay::StatusSet statuses;
    load(e, attrs, statuses);
    if (!attrs.has(name)) {
        // 不在 `defaultAttributes()` 里的计数器（能量 / 手牌数加成）由内容
        // 决定，第一次写就建出来 + 下限 0（扣不成负的）。
        attrs.setBase(name, 0.0f);
        attrs.setRange(name, 0.0f, 1.0e9f);
    }
    const float before = attrs.base(name);
    attrs.setBase(name, before + delta);
    const float after = attrs.base(name);
    store(e, attrs, statuses);
    const std::string line = fmt("attr %s %.2f -> %.2f", name.c_str(),
                                 static_cast<double>(before),
                                 static_cast<double>(after));
    VK_LOG_INFO("%sATTR-ADD entity=%u %s", kLogPrefix,
                static_cast<unsigned>(e.id), line.c_str());
    return line;
}

ecs::Entity GameplayRuntime::firstOtherGameplayEntity(ecs::Entity exclude) const {
    ecs::Entity found;
    if (!m_scene) return found;
    m_scene->world().each<ecs::GameplayComponent>(
        [&](ecs::Entity e, ecs::GameplayComponent&) {
            // ⚠ "已经有候选了"必须用 `valid()` 判，**不能**写 `found.id != 0`：
            //   `ecs::Entity` 的默认 id 是 `kInvalidEntity`（0xFFFFFFFF），不是
            //   0 —— 写成 `!= 0` 会让**一进门就 return**，函数永远返回无效实体。
            //   症状是"单体攻击牌打出去 0 伤害、missed=1"，而牌正常进弃牌堆。
            if (found.valid()) return;
            if (e.id == exclude.id) return;
            found = e;
        });
    return found;
}

std::string GameplayRuntime::castCard(ecs::Entity owner, int handIndex,
                                      ecs::Entity target) {
    ecs::CardDeckComponent* d = deck(owner);
    if (!d) return std::string("FAIL: entity has no CardDeckComponent");

    const uint32_t uid = d->zones.uidAt(gameplay::CardPile::Hand, handIndex);
    if (uid == 0) {
        const std::string line =
            fmt("CARD-CAST entity=%u idx=%d ok=0 reason=no-such-hand-index hand=%d",
                static_cast<unsigned>(owner.id), handIndex,
                static_cast<int>(d->zones.count(gameplay::CardPile::Hand)));
        VK_LOG_INFO("%sWARN %s", kLogPrefix, line.c_str());
        return line;
    }
    const gameplay::CardInstance* inst = d->zones.find(uid);
    const std::string defId = inst ? inst->defId : std::string("?");
    const gameplay::CardDef* def = d->zones.def(defId);
    if (!def) {
        // ⚠ 不搬牌。搬了再静默丢弃 = "打出去什么都没发生"，那与"配表没
        //   生效"完全一样（P3-1 最贵的一类 bug）。
        // ⚠ 打出 `hand`/`limbo`：这两个数才是"牌没动"的证据。只打
        //   `reason` 的话，判据只能验错误码，验不到"手牌真的还在"。
        const std::string line =
            fmt("CARD-CAST entity=%u idx=%d uid=%u def=%s ok=0 "
                "reason=unknown-def defs=%d hand=%d limbo=%d",
                static_cast<unsigned>(owner.id), handIndex, uid, defId.c_str(),
                static_cast<int>(d->zones.defs().size()),
                static_cast<int>(d->zones.count(gameplay::CardPile::Hand)),
                static_cast<int>(d->zones.count(gameplay::CardPile::Limbo)));
        // ⚠ 形状统一用 `%s%s`（`GP-` + 行本体）：先前写成 `%sWARN %s` 会
        //   产出 `GP-WARN CARD-CAST ...`，判据按 `GP-CARD-CAST` 抓就抓不到
        //   —— 而抓不到的后果是"这条断言凭空消失"，不是"这条断言红了"。
        VK_LOG_WARN("%s%s", kLogPrefix, line.c_str());
        return line;
    }
    if (!d->zones.play(uid)) {
        const std::string line =
            fmt("CARD-CAST entity=%u idx=%d uid=%u def=%s ok=0 reason=unplayable "
                "hand=%d limbo=%d",
                static_cast<unsigned>(owner.id), handIndex, uid, defId.c_str(),
                static_cast<int>(d->zones.count(gameplay::CardPile::Hand)),
                static_cast<int>(d->zones.count(gameplay::CardPile::Limbo)));
        VK_LOG_WARN("%s%s", kLogPrefix, line.c_str());
        return line;
    }
    m_lastCard = uid;
    // 出牌：表现落在**持牌者**身上（牌是从手里打出去的，不在敌人身上）。
    pushCardFx(gameplay::CardFxKind::Play, owner, owner, 0.0f);

    // 目标池一次收齐：`damage()` / `addAttr()` 内部要重新取组件，
    // 在 `each()` 回调里再调它们等于"边遍历边改组件"——这套 API 允许，但
    // 一旦将来有人在这里加了 `world().add<>()` 就会变成遍历中增删。
    // 收成 vector 之后这个问题不存在。
    std::vector<ecs::Entity> pool;
    m_scene->world().each<ecs::GameplayComponent>(
        [&](ecs::Entity e, ecs::GameplayComponent&) {
            if (e.id != owner.id) pool.push_back(e);
        });

    int fx = 0;
    int missed = 0;
    float dmgTotal = 0.0f;
    float healTotal = 0.0f;
    int statusN = 0;
    int drawN = 0;
    int energyN = 0;
    int shieldN = 0;

    for (const gameplay::CardEffect& e : def->effects) {
        // ---- 两条"不需要目标"的效果先处理 ----
        //
        // ⚠⚠ 它们必须在**目标解析之前**：`draw::2` 的意思是"给持牌者抽 2 张"，
        //   与这张牌打谁毫无关系。放进下面的 switch 里的话，一张
        //   `target=none` 的抽牌卡会因为"没目标"而整条被算成 missed ——
        //   症状是"抽牌卡打出去什么也没抽"，而日志里只有一个 `missed=1`
        //   （它同时也可能是"目标死了"、"敌人列表为空"，看不出是哪一种）。
        if (e.kind == gameplay::CardEffect::Kind::Draw ||
            e.kind == gameplay::CardEffect::Kind::Energy) {
            ++fx;
            const int reps0 = (e.count < 1) ? 1 : e.count;
            for (int r = 0; r < reps0; ++r) {
                if (e.kind == gameplay::CardEffect::Kind::Draw) {
                    const int n = static_cast<int>(e.amount) < 1
                                      ? 1
                                      : static_cast<int>(e.amount);
                    const int got = d->zones.draw(n, m_streams.gameplay);
                    drawN += got;
                    // 牌库空了抽不到：`got == 0` 时**不弹字**（弹个 "0" 只会
                    // 让人以为抽牌坏了）。模板里 Draw 本来也不弹字，这里
                    // 只把"真的抽到了"这件事记下来，宿主想弹就自己改表。
                    if (got > 0)
                        pushCardFx(gameplay::CardFxKind::Draw, owner, owner,
                                   static_cast<float>(got));
                } else {
                    const std::string line = addAttr(owner, "energy", e.amount);
                    if (line.rfind("FAIL", 0) == 0) ++missed;
                    else {
                        energyN += static_cast<int>(e.amount);
                        pushCardFx(gameplay::CardFxKind::Buff, owner, owner,
                                   e.amount);
                    }
                }
            }
            continue;
        }

        // 效果自己的 `target` 优先；写了但认不出 → 回落到卡自己的（不静默
        // 变成"没目标"，那会让这张牌少打一下而看不出原因）。
        gameplay::CardTarget tgt = def->target;
        if (!e.target.empty()) {
            gameplay::CardTarget tmp = tgt;
            if (gameplay::parseCardTarget(e.target, tmp)) tgt = tmp;
        }

        std::vector<ecs::Entity> targets;
        switch (tgt) {
        case gameplay::CardTarget::Enemy:
            if (target.valid() && comp(target) && target.id != owner.id) {
                targets.push_back(target);
            }
            break;
        case gameplay::CardTarget::AllEnemies:
            targets = pool;
            break;
        case gameplay::CardTarget::Random: {
            const int i = m_streams.gameplay.pickIndex(pool.size());
            if (i >= 0) targets.push_back(pool[static_cast<std::size_t>(i)]);
            break;
        }
        case gameplay::CardTarget::Self:
        case gameplay::CardTarget::AllAllies:
            // 本引擎还没有阵营概念 → "我方"就是自己一个人。
            // ⚠ 这条假设写在 docs/cards.md 的"已知边界"里：加阵营时**这里**
            //   是唯一要改的地方（卡牌效果不各自判目标）。
            targets.push_back(owner);
            break;
        case gameplay::CardTarget::None:
        default:
            break;
        }

        if (targets.empty()) {
            ++missed;
            continue;
        }
        ++fx;
        const int reps = (e.count < 1) ? 1 : e.count;
        for (int r = 0; r < reps; ++r) {
            // `Draw` / `Energy` 已经在上面处理过（它们不需要目标），
            // 所以这里的 `targets` 一定非空 —— 只按目标遍历就够了。
            for (int ti = 0; ti < static_cast<int>(targets.size()); ++ti) {
                const ecs::Entity t = targets[static_cast<std::size_t>(ti)];
                switch (e.kind) {
                case gameplay::CardEffect::Kind::Damage: {
                    gameplay::DamageSpec spec;
                    spec.amount = e.amount;
                    spec.type = gameplay::DamageType::Physical;
                    if (!e.type.empty()) {
                        gameplay::parseDamageType(e.type, spec.type);
                    }
                    spec.source = "card:" + def->id;
                    const std::string line = damage(owner, t, spec);
                    if (line.rfind("FAIL", 0) != 0) {
                        dmgTotal += m_lastDamage.dealt;
                        // 飘字用**实际造成的伤害**（`dealt`），不是 `e.amount`：
                        // 打在护甲上时两者差很多，写 `e.amount` 就是骗人。
                        // 暴击另走 Crit 样式（更大更亮）—— 是不是暴击由
                        // `damage()` 的随机流决定，这里只读结果。
                        pushCardFx(m_lastDamage.crit ? gameplay::CardFxKind::Crit
                                                     : gameplay::CardFxKind::Hit,
                                   t, owner, m_lastDamage.dealt);
                    }
                    break;
                }
                case gameplay::CardEffect::Kind::Heal: {
                    // 飘字用"实际回的血"（`after - before`）而不是 `e.amount`：
                    // 满血时治疗 30 实际只回 4，弹 "30" 就是假消息。
                    const float hpBefore = attr(t, "hp", 0.0f);
                    const std::string line = heal(t, e.amount);
                    if (line.rfind("FAIL", 0) == 0) {
                        ++missed;
                    } else {
                        // ⚠ `healTotal` 仍然累加**请求量**（日志 `heal=` 的口径
                        //   不能悄悄改：判据与文档都在读它）。"实际回了多少"
                        //   只用于飘字 —— 满血时治疗 30 实际只回 4，弹 "30"
                        //   就是假消息，而日志要的是"这张牌写了多少"。
                        healTotal += e.amount;
                        const float got = attr(t, "hp", 0.0f) - hpBefore;
                        if (got > 0.0f)
                            pushCardFx(gameplay::CardFxKind::Heal, t, owner, got);
                    }
                    break;
                }
                case gameplay::CardEffect::Kind::Status: {
                    gameplay::StatusDef sd;
                    if (!gameplay::statusDefById(e.type, sd)) {
                        // 认不出的状态 id：计数 + 落 WARN。**不猜**（猜成
                        // 中毒会让一个拼错的 id 静默生效）。
                        ++missed;
                        VK_LOG_WARN("%sCARD-CAST unknown-status def=%s id=%s",
                                    kLogPrefix, def->id.c_str(), e.type.c_str());
                        break;
                    }
                    // `amount` 在这一支里是**层数**（见 CardEffect 的注释）。
                    const int layers = static_cast<int>(e.amount) < 1
                                           ? 1
                                           : static_cast<int>(e.amount);
                    const int statusBefore = statusN;
                    for (int k = 0; k < layers; ++k) {
                        const std::string line = addStatus(t, sd);
                        if (line.rfind("FAIL", 0) == 0) ++missed;
                        else ++statusN;
                    }
                    // ⚠ 用**本条的增量**（`statusN - statusBefore`）而不是
                    //   `statusN > 0`：后者会让"第二个状态全失败"也弹字，
                    //   因为你读的是整张牌累计的计数。
                    if (statusN > statusBefore)
                        pushCardFx(gameplay::CardFxKind::Buff, t, owner,
                                   static_cast<float>(statusN - statusBefore));
                    break;
                }
                case gameplay::CardEffect::Kind::Shield: {
                    // ⚠ 直接加 `shield` 属性的白值，不是挂 `presetShield()` 状态：
                    //   状态版是"回合到期消失的盾"，而"这张牌给 5 点盾"是
                    //   一次性的数值。两件事在配表里必须能分开表达
                    //   （状态版写 `status::1:shield`）。
                    const std::string line = addAttr(t, "shield", e.amount);
                    if (line.rfind("FAIL", 0) == 0) {
                        ++missed;
                    } else {
                        shieldN += static_cast<int>(e.amount);
                        pushCardFx(gameplay::CardFxKind::Shield, t, owner,
                                   e.amount);
                    }
                    break;
                }
                case gameplay::CardEffect::Kind::Draw:
                case gameplay::CardEffect::Kind::Energy:
                    // 不可达：这两条在循环开头就 `continue` 了（它们不需要
                    // 目标）。留着 `default` 式的空分支而不是删掉，是为了让
                    // "有人把它们挪回来"时编译器还能提醒 switch 没漏枚举。
                    break;
                }
            }
        }
    }

    // 结算完落地：Limbo → 弃牌堆 / 消耗区。
    d->zones.resolve(uid);
    // 落点决定是"弃"还是"消耗"：`exhaust` 在定义里，所以只有 `resolve`
    // 之后才知道（`resolve` 内部就是按 `d->exhaust` 分的，这里读同一个
    // 判据 —— 不重算一遍规则，否则"表里写了消耗、日志说弃牌"会分叉）。
    pushCardFx((def->exhaust) ? gameplay::CardFxKind::Exhaust
                              : gameplay::CardFxKind::Discard,
               owner, owner, 0.0f);

    const std::string line =
        fmt("CARD-CAST entity=%u idx=%d uid=%u def=%s target=%u ok=1 fx=%d "
            "missed=%d dmg=%.2f heal=%.2f status=%d shield=%d draw=%d energy=%d "
            "discard=%d exhaust=%d limbo=%d",
            static_cast<unsigned>(owner.id), handIndex, uid, def->id.c_str(),
            static_cast<unsigned>(target.valid() ? target.id : 0u), fx, missed,
            static_cast<double>(dmgTotal), static_cast<double>(healTotal), statusN,
            shieldN, drawN, energyN,
            static_cast<int>(d->zones.count(gameplay::CardPile::Discard)),
            static_cast<int>(d->zones.count(gameplay::CardPile::Exhaust)),
            static_cast<int>(d->zones.count(gameplay::CardPile::Limbo)));
    VK_LOG_INFO("%s%s", kLogPrefix, line.c_str());
    return line;
}

} // namespace scene
