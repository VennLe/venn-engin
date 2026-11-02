// ============================================================
// gameplay/CardZones.cpp
//
// 实现上只有三处值得注意（都是"看起来等价、结果不同"的地方）：
//   1) 同区重排（`move(uid, Hand, pos)`）在 `from == to && pos > idx` 时，
//      插入位置必须 **-1** —— 元素已经被摘走了，不补偿就是差一位。
//   2) `play()` / `resolve()` 必须**先拷值再 erase**：`m_hand[idx]` 是引用，
//      erase 之后它悬空，而悬空的 `CardInstance` 里的 `std::string` 会在
//      下一次读到时炸成随机内容（不一定崩，但 `defId` 会变成乱码）。
//   3) `deserialize()` 要重算 `m_nextUid`（取最大 uid + 1），否则读档之后
//      造的新牌会与老牌**撞 uid** —— 而撞 uid 的症状是"打出这张，那张消失了"。
// ============================================================

#include "gameplay/CardZones.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>   // std::size（C++17 在 <iterator>，不在 <array>）
#include <sstream>

namespace gameplay {

namespace {

const char* const kKindNames[] = {"attack", "skill", "power", "status", "curse"};
const char* const kTargetNames[] = {"enemy",     "allEnemies", "self",
                                    "allAllies", "random",     "none"};
const char* const kEffectKindNames[] = {"damage", "heal", "status",
                                        "shield", "draw", "energy"};
const char* const kPileNames[] = {"draw", "hand", "discard", "exhaust", "limbo"};

template <typename E, std::size_t N>
bool parseEnumImpl(const std::string& s, const char* const (&names)[N], E& out) {
    for (std::size_t i = 0; i < N; ++i) {
        if (s == names[i]) {
            out = static_cast<E>(i);
            return true;
        }
    }
    return false;
}

}  // namespace

const char* cardKindName(CardKind k) {
    const int i = static_cast<int>(k);
    return (i >= 0 && i < static_cast<int>(std::size(kKindNames))) ? kKindNames[i] : "attack";
}
const char* cardTargetName(CardTarget t) {
    const int i = static_cast<int>(t);
    return (i >= 0 && i < static_cast<int>(std::size(kTargetNames))) ? kTargetNames[i] : "enemy";
}
const char* cardEffectKindName(CardEffect::Kind k) {
    const int i = static_cast<int>(k);
    return (i >= 0 && i < static_cast<int>(std::size(kEffectKindNames))) ? kEffectKindNames[i]
                                                                        : "damage";
}
const char* cardPileName(CardPile p) {
    const int i = static_cast<int>(p);
    return (i >= 0 && i < kCardPileCount) ? kPileNames[i] : "draw";
}

bool parseCardKind(const std::string& s, CardKind& out) {
    return parseEnumImpl<CardKind>(s, kKindNames, out);
}
bool parseCardTarget(const std::string& s, CardTarget& out) {
    return parseEnumImpl<CardTarget>(s, kTargetNames, out);
}
bool parseCardEffectKind(const std::string& s, CardEffect::Kind& out) {
    return parseEnumImpl<CardEffect::Kind>(s, kEffectKindNames, out);
}
bool parseCardPile(const std::string& s, CardPile& out) {
    return parseEnumImpl<CardPile>(s, kPileNames, out);
}

// ---------------------------------------------------------------- 定义表

void CardZones::setDefs(std::vector<CardDef> defs) {
    m_defs = std::move(defs);
}

const CardDef* CardZones::def(const std::string& id) const {
    for (const CardDef& d : m_defs) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

int CardZones::costOf(const CardInstance& c) const {
    const CardDef* d = def(c.defId);
    if (!d) return 0;
    if (c.upgrade > 0 && d->upgradedCost >= 0) return d->upgradedCost;
    return d->cost;
}

// ---------------------------------------------------------------- 区访问

std::vector<CardInstance>& CardZones::pileMut(CardPile p) {
    switch (p) {
    case CardPile::Hand: return m_hand;
    case CardPile::Discard: return m_discard;
    case CardPile::Exhaust: return m_exhaust;
    case CardPile::Limbo: return m_limbo;
    case CardPile::Draw:
    default: return m_draw;
    }
}

const std::vector<CardInstance>& CardZones::pile(CardPile p) const {
    // 独立写一份 switch 而不是 `const_cast` 转调 pileMut：后者在"const 对象
    // 被真的改了"时是未定义行为，而那种写法一旦被复制粘贴到别处就会成为
    // 一个只在 Release 里发作的 bug。
    switch (p) {
    case CardPile::Hand: return m_hand;
    case CardPile::Discard: return m_discard;
    case CardPile::Exhaust: return m_exhaust;
    case CardPile::Limbo: return m_limbo;
    case CardPile::Draw:
    default: return m_draw;
    }
}

std::size_t CardZones::count(CardPile p) const {
    return pile(p).size();
}

std::size_t CardZones::total() const {
    return m_draw.size() + m_hand.size() + m_discard.size() + m_exhaust.size() +
           m_limbo.size();
}

int CardZones::indexOf(CardPile p, uint32_t uid) const {
    const std::vector<CardInstance>& v = pile(p);
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].uid == uid) return static_cast<int>(i);
    }
    return -1;
}

bool CardZones::locate(uint32_t uid, CardPile* where, int* pos) const {
    if (uid == 0) return false;
    for (int i = 0; i < kCardPileCount; ++i) {
        const CardPile p = static_cast<CardPile>(i);
        const int idx = indexOf(p, uid);
        if (idx >= 0) {
            if (where) *where = p;
            if (pos) *pos = idx;
            return true;
        }
    }
    return false;
}

bool CardZones::contains(uint32_t uid) const {
    return locate(uid, nullptr, nullptr);
}

const CardInstance* CardZones::find(uint32_t uid, CardPile* where) const {
    CardPile p = CardPile::Draw;
    int idx = 0;
    if (!locate(uid, &p, &idx)) return nullptr;
    if (where) *where = p;
    return &pile(p)[static_cast<std::size_t>(idx)];
}

uint32_t CardZones::uidAt(CardPile p, int index) const {
    const std::vector<CardInstance>& v = pile(p);
    if (index < 0 || index >= static_cast<int>(v.size())) return 0;
    return v[static_cast<std::size_t>(index)].uid;
}

// ---------------------------------------------------------------- 造牌

uint32_t CardZones::create(const std::string& defId, int count) {
    if (count <= 0) return 0;
    uint32_t first = 0;
    for (int i = 0; i < count; ++i) {
        CardInstance c;
        c.uid = m_nextUid++;
        c.defId = defId;
        c.upgrade = 0;
        if (i == 0) first = c.uid;
        m_draw.push_back(std::move(c));
    }
    return first;
}

bool CardZones::erase(uint32_t uid) {
    for (int i = 0; i < kCardPileCount; ++i) {
        const CardPile p = static_cast<CardPile>(i);
        std::vector<CardInstance>& v = pileMut(p);
        for (std::size_t k = 0; k < v.size(); ++k) {
            if (v[k].uid == uid) {
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(k));
                return true;
            }
        }
    }
    return false;
}

bool CardZones::put(CardPile p, const CardInstance& c, int pos) {
    if (c.uid == 0 || c.defId.empty()) return false;
    if (contains(c.uid)) return false;
    std::vector<CardInstance>& dst = pileMut(p);
    if (pos < 0 || pos >= static_cast<int>(dst.size())) {
        dst.push_back(c);
    } else {
        dst.insert(dst.begin() + pos, c);
    }
    if (c.uid >= m_nextUid) m_nextUid = c.uid + 1;
    return true;
}

void CardZones::reserveUid(uint32_t v) {
    // 只往上抬：存档里的 `nextUid` 可能比实际最大 uid 小（旧版本漏写），
    // 信它就会造出撞号的牌。
    if (v > m_nextUid) m_nextUid = v;
}

void CardZones::setCounters(int reshuffles, int plays) {
    m_reshuffles = reshuffles > 0 ? reshuffles : 0;
    m_plays = plays > 0 ? plays : 0;
}

// ---------------------------------------------------------------- 移动

bool CardZones::move(uint32_t uid, CardPile to, int pos) {
    CardPile from = CardPile::Draw;
    int idx = 0;
    if (!locate(uid, &from, &idx)) return false;
    // 原地不动：同区且没给位置（或者给的正是它自己的位置）→ 成功但什么都不做。
    if (from == to && (pos < 0 || pos == idx)) return true;

    // ⚠ 先拷值再 erase（见文件头第 2 条）。
    const CardInstance c = pileMut(from)[static_cast<std::size_t>(idx)];
    pileMut(from).erase(pileMut(from).begin() + idx);

    // `pos` 是**目标位置**（0-based），不是"插到原数组第 pos 个之前"。
    // 所以**不需要**对 `pos > idx` 做 -1 补偿：erase 已经让后面的元素前移
    // 一格，正好抵消。契约因此可以写成一句话：
    //     move(uid, to, pos) 之后  uidAt(to, pos) == uid
    // 这比"补偿一格"好判据得多 —— 补偿写错一格时，症状是手牌拖动**差一位**，
    // 而差一位在 UI 上看起来像"拖动有点飘"，几乎不会被当成 bug 报。
    std::vector<CardInstance>& dst = pileMut(to);
    if (pos < 0 || pos >= static_cast<int>(dst.size())) {
        dst.push_back(c);
    } else {
        dst.insert(dst.begin() + pos, c);
    }
    return true;
}

void CardZones::setHandLimit(int v) {
    // <= 0 = 不限（用 0 表达"不限"是刻意的：手牌上限为 0 的规则没有意义，
    // 而"0 = 不限"比"负数 = 不限"更不容易被误写成 `if (limit)` 而把 0 当禁用）。
    m_handLimit = v > 0 ? v : 0;
}

// ---------------------------------------------------------------- 流程

int CardZones::draw(int n, Random& rng) {
    m_lastReshuffle = false;
    int got = 0;
    for (int i = 0; i < n; ++i) {
        if (m_handLimit > 0 && static_cast<int>(m_hand.size()) >= m_handLimit) break;
        if (m_draw.empty()) {
            if (m_discard.empty()) break;   // 抽干了：牌库与弃牌堆都空
            m_draw.swap(m_discard);
            m_discard.clear();
            rng.shuffle(m_draw);
            ++m_reshuffles;
            m_lastReshuffle = true;
        }
        // 从**队首**取：`draw` 的第 0 张就是"下一张要抽的牌"，判据与 UI
        // 都能直接读它。一副牌撑死几百张，`erase(begin())` 的搬移无所谓。
        m_hand.push_back(m_draw.front());
        m_draw.erase(m_draw.begin());
        ++got;
    }
    return got;
}

int CardZones::discardHand() {
    if (m_hand.empty()) return 0;
    const int n = static_cast<int>(m_hand.size());
    for (const CardInstance& c : m_hand) m_discard.push_back(c);
    m_hand.clear();
    return n;
}

bool CardZones::play(uint32_t uid) {
    const int idx = indexOf(CardPile::Hand, uid);
    if (idx < 0) return false;
    const CardInstance c = m_hand[static_cast<std::size_t>(idx)];
    if (const CardDef* d = def(c.defId)) {
        if (d->unplayable) return false;
    }
    m_limbo.push_back(c);
    m_hand.erase(m_hand.begin() + idx);
    ++m_plays;
    return true;
}

bool CardZones::resolve(uint32_t uid) {
    const int idx = indexOf(CardPile::Limbo, uid);
    if (idx < 0) return false;
    const CardInstance c = m_limbo[static_cast<std::size_t>(idx)];
    m_limbo.erase(m_limbo.begin() + idx);
    const CardDef* d = def(c.defId);
    // 定义查不到时按**弃牌**处理：消耗会导致这张牌永久消失，而"表丢了"
    // 不该让玩家的牌蒸发。
    if (d && d->exhaust) m_exhaust.push_back(c);
    else m_discard.push_back(c);
    return true;
}

int CardZones::sweepLimbo() {
    if (m_limbo.empty()) return 0;
    const int n = static_cast<int>(m_limbo.size());
    // 拷一份再逐张 resolve（resolve 会改 m_limbo）。
    std::vector<CardInstance> pending = m_limbo;
    m_limbo.clear();
    for (const CardInstance& c : pending) {
        const CardDef* d = def(c.defId);
        if (d && d->exhaust) m_exhaust.push_back(c);
        else m_discard.push_back(c);
    }
    return n;
}

void CardZones::shuffleDraw(Random& rng) {
    rng.shuffle(m_draw);
}

// ---------------------------------------------------------------- 清空 / 输出

void CardZones::clear() {
    m_draw.clear();
    m_hand.clear();
    m_discard.clear();
    m_exhaust.clear();
    m_limbo.clear();
    m_handLimit = 10;
    m_nextUid = 1;
    m_reshuffles = 0;
    m_plays = 0;
    m_lastReshuffle = false;
    // ⚠ m_defs **故意不清**（见头文件）。
}

std::string CardZones::dump() const {
    char buf[192];
    // i18n-keep —— 内部日志格式，不是给人读的文案
    std::snprintf(buf, sizeof(buf),
                  "draw=%zu hand=%zu discard=%zu exhaust=%zu limbo=%zu uid=%u "
                  "reshuffle=%d plays=%d",
                  m_draw.size(), m_hand.size(), m_discard.size(), m_exhaust.size(),
                  m_limbo.size(), m_nextUid, m_reshuffles, m_plays);
    return std::string(buf);
}

std::string CardZones::serialize() const {
    std::string s;
    char buf[160];
    // i18n-keep —— 存档格式
    std::snprintf(buf, sizeof(buf), "handLimit=%d nextUid=%u reshuffles=%d plays=%d\n",
                  m_handLimit, m_nextUid, m_reshuffles, m_plays);
    s += buf;
    for (int i = 0; i < kCardPileCount; ++i) {
        const CardPile p = static_cast<CardPile>(i);
        for (const CardInstance& c : pile(p)) {
            // i18n-keep —— 存档格式
            std::snprintf(buf, sizeof(buf), "%s %s %u %d\n", cardPileName(p),
                          c.defId.c_str(), c.uid, c.upgrade);
            s += buf;
        }
    }
    return s;
}

bool CardZones::deserialize(const std::string& text, std::string* err) {
    clear();
    std::istringstream ss(text);
    std::string line;
    bool sawHeader = false;
    uint32_t maxUid = 0;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (!sawHeader) {
            // 首行：`handLimit=N nextUid=N reshuffles=N plays=N`
            if (line.rfind("handLimit=", 0) != 0) {
                if (err) *err = "missing header (expected `handLimit=` first)";
                return false;
            }
            std::istringstream hs(line);
            std::string tok;
            while (hs >> tok) {
                const std::size_t eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = tok.substr(0, eq);
                const long v = std::strtol(tok.c_str() + eq + 1, nullptr, 10);
                if (k == "handLimit") m_handLimit = static_cast<int>(v) > 0
                                                       ? static_cast<int>(v)
                                                       : 0;
                else if (k == "nextUid") m_nextUid = static_cast<uint32_t>(v);
                else if (k == "reshuffles") m_reshuffles = static_cast<int>(v);
                else if (k == "plays") m_plays = static_cast<int>(v);
            }
            sawHeader = true;
            continue;
        }

        std::istringstream ls(line);
        std::string pileName, defId;
        unsigned long uid = 0;
        int upgrade = 0;
        if (!(ls >> pileName >> defId >> uid >> upgrade)) {
            if (err) *err = "bad card row: " + line;
            return false;
        }
        CardPile p = CardPile::Draw;
        if (!parseCardPile(pileName, p)) {
            if (err) *err = "unknown pile: " + pileName;
            return false;
        }
        CardInstance c;
        c.uid = static_cast<uint32_t>(uid);
        c.defId = defId;
        c.upgrade = upgrade;
        if (c.uid == 0) {
            if (err) *err = "card uid must be > 0";
            return false;
        }
        if (c.uid > maxUid) maxUid = c.uid;
        pileMut(p).push_back(std::move(c));
    }

    if (!sawHeader) {
        if (err) *err = "empty card data";
        return false;
    }
    // ⚠ 重算 nextUid（见文件头第 3 条）：存档里的数**可能比实际最大 uid 小**
    //   （旧版本漏写、手改成 0），信它就会造出撞号的牌。
    if (m_nextUid <= maxUid) m_nextUid = maxUid + 1;
    return true;
}

// ---------------------------------------------------------------- 示例定义表

std::vector<CardDef> presetCardDefs() {
    std::vector<CardDef> v;

    // 打击：1 费 6 点物理。判据用它验"抽到 → 打出 → 弃牌"的直线路径。
    CardDef strike;
    strike.id = "strike";
    strike.name = "Strike";
    strike.text = "Deal 6 damage.";
    strike.kind = CardKind::Attack;
    strike.target = CardTarget::Enemy;
    strike.cost = 1;
    {
        CardEffect e;
        e.kind = CardEffect::Kind::Damage;
        e.amount = 6.0f;
        e.type = "physical";
        strike.effects.push_back(e);
    }
    v.push_back(strike);

    // 防御：1 费 5 点护盾。护盾走 `addStatus(presetShield())` —— 见 P3 的
    // 结算器（本文件不含结算，只给数据）。
    CardDef defend;
    defend.id = "defend";
    defend.name = "Defend";
    defend.text = "Gain 5 block.";
    defend.kind = CardKind::Skill;
    defend.target = CardTarget::Self;
    defend.cost = 1;
    {
        CardEffect e;
        e.kind = CardEffect::Kind::Shield;
        e.amount = 5.0f;
        defend.effects.push_back(e);
    }
    v.push_back(defend);

    // 重击：2 费 8 点 + 2 层易伤，**消耗**（打出后进消耗区，不再回牌库）。
    // 判据 N4 用它验"exhaust 卡不进弃牌堆"。
    CardDef bash;
    bash.id = "bash";
    bash.name = "Bash";
    bash.text = "Deal 8 damage. Apply 2 Vulnerable. Exhaust.";
    bash.kind = CardKind::Attack;
    bash.target = CardTarget::Enemy;
    bash.cost = 2;
    bash.upgradedCost = 1;
    bash.exhaust = true;
    {
        CardEffect e;
        e.kind = CardEffect::Kind::Damage;
        e.amount = 8.0f;
        e.type = "physical";
        bash.effects.push_back(e);
    }
    {
        CardEffect e;
        e.kind = CardEffect::Kind::Status;
        e.amount = 2.0f;      // Status 的 amount 是**层数**
        e.type = "vulnerable";
        bash.effects.push_back(e);
    }
    v.push_back(bash);

    // 黏液：诅咒。占手牌、打不出去、**endTurn 也不会自己消失** ——
    // 判据 N6 用它验"play 失败时牌必须还在手牌里"。
    CardDef slimed;
    slimed.id = "slimed";
    slimed.name = "Slimed";
    slimed.text = "Unplayable.";
    slimed.kind = CardKind::Curse;
    slimed.target = CardTarget::None;
    slimed.cost = 1;
    slimed.unplayable = true;
    v.push_back(slimed);

    return v;
}

}  // namespace gameplay
