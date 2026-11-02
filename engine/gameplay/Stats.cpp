#include "gameplay/Stats.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gameplay {

// ================================================================ 名字表

const char* modOpName(ModOp op) {
    switch (op) {
    case ModOp::Flat: return "flat";
    case ModOp::AddPct: return "add";
    case ModOp::MulPct: return "mul";
    case ModOp::Override: return "set";
    }
    return "?";
}

bool parseModOp(const std::string& s, ModOp& out) {
    if (s == "flat" || s == "+") { out = ModOp::Flat; return true; }
    if (s == "add" || s == "addpct" || s == "pct") { out = ModOp::AddPct; return true; }
    if (s == "mul" || s == "mulpct" || s == "x") { out = ModOp::MulPct; return true; }
    if (s == "set" || s == "override") { out = ModOp::Override; return true; }
    return false;
}

const char* stackRuleName(StackRule r) {
    switch (r) {
    case StackRule::Refresh: return "refresh";
    case StackRule::Stack: return "stack";
    case StackRule::Independent: return "independent";
    }
    return "?";
}

bool parseStackRule(const std::string& s, StackRule& out) {
    if (s == "refresh") { out = StackRule::Refresh; return true; }
    if (s == "stack") { out = StackRule::Stack; return true; }
    if (s == "independent" || s == "indep") { out = StackRule::Independent; return true; }
    return false;
}

const char* damageTypeName(DamageType t) {
    switch (t) {
    case DamageType::Physical: return "physical";
    case DamageType::Magic: return "magic";
    case DamageType::Fire: return "fire";
    case DamageType::Ice: return "ice";
    case DamageType::Poison: return "poison";
    case DamageType::True: return "true";
    }
    return "?";
}

bool parseDamageType(const std::string& s, DamageType& out) {
    if (s == "physical" || s == "phys") { out = DamageType::Physical; return true; }
    if (s == "magic" || s == "arcane") { out = DamageType::Magic; return true; }
    if (s == "fire") { out = DamageType::Fire; return true; }
    if (s == "ice" || s == "cold") { out = DamageType::Ice; return true; }
    if (s == "poison") { out = DamageType::Poison; return true; }
    if (s == "true" || s == "pure") { out = DamageType::True; return true; }
    return false;
}

// ================================================================ 属性

float Attribute::value() const {
    // ⚠ 顺序就是契约（文件头第 1 条）。改动会改变**每一个**配方的数值，
    //   而且改动之后"看起来都很合理"，所以必须被用例钉住。
    bool hasOverride = false;
    float overrideV = 0.0f;
    float flat = 0.0f;
    float addPct = 0.0f;
    float mul = 1.0f;

    for (const StatModifier& m : mods) {
        switch (m.op) {
        case ModOp::Override:
            // 后加的赢：按插入顺序扫，最后一个覆盖前面的。
            hasOverride = true;
            overrideV = m.value;
            break;
        case ModOp::Flat: flat += m.value; break;
        case ModOp::AddPct: addPct += m.value; break;
        case ModOp::MulPct: mul *= m.value; break;
        }
    }

    float v = (hasOverride ? overrideV : base) + flat;
    v *= (1.0f + addPct * 0.01f);
    v *= mul;
    // clamp 只做一次（文件头第 2 条）。
    if (v < minValue) v = minValue;
    if (v > maxValue) v = maxValue;
    return v;
}

std::string Attribute::explain() const {
    char buf[256];
    bool hasOverride = false;
    float overrideV = 0.0f, flat = 0.0f, addPct = 0.0f, mul = 1.0f;
    int n = 0;
    for (const StatModifier& m : mods) {
        ++n;
        switch (m.op) {
        case ModOp::Override: hasOverride = true; overrideV = m.value; break;
        case ModOp::Flat: flat += m.value; break;
        case ModOp::AddPct: addPct += m.value; break;
        case ModOp::MulPct: mul *= m.value; break;
        }
    }
    std::snprintf(buf, sizeof(buf),
                  "base=%.2f%s%.2f flat=%+.2f add=%+.0f%% mul=x%.3f mods=%d -> %.2f",
                  static_cast<double>(base),
                  hasOverride ? " set=" : "",
                  static_cast<double>(hasOverride ? overrideV : 0.0f),
                  static_cast<double>(flat), static_cast<double>(addPct),
                  static_cast<double>(mul), n, static_cast<double>(value()));
    return std::string(buf);
}

float Attribute::consume(float amount) {
    if (!(amount > 0.0f)) return 0.0f;
    const float v0 = value();
    // floor = max(minValue, 0)：血不会被扣成负数。
    const float floorV = std::max(minValue, 0.0f);
    const float room = v0 - floorV;
    if (!(room > 0.0f)) return 0.0f;

    // 反解 base：`value = clamp((base + flat) * k)`。直接 `base -= take` 是
    // **错的** —— 有 +100% 加成时 `base -= take` 只让值掉了 `2*take`。
    //
    // ⚠⚠ 这里**不能用"探一下斜率"的写法**（`value(base+1) - v0`）：满血的
    //   单位 base 正卡在 `maxValue` 上，往上探出来的斜率是 **0**，于是
    //   "扣不动"—— 而满血是最常见的情况，症状是"打人完全没伤害"。
    //   所以要按定义把 k 算出来，而不是测出来。
    bool hasOverride = false;
    float flat = 0.0f;
    float addPct = 0.0f;
    float mul = 1.0f;
    for (const StatModifier& m : mods) {
        switch (m.op) {
        case ModOp::Override: hasOverride = true; break;
        case ModOp::Flat: flat += m.value; break;
        case ModOp::AddPct: addPct += m.value; break;
        case ModOp::MulPct: mul *= m.value; break;
        }
    }
    // Override 顶住 → base 完全不参与求值，扣不动（见头文件注释）。
    if (hasOverride) return 0.0f;
    const float k = (1.0f + addPct * 0.01f) * mul;
    // 负系数 / 0 系数：值不随 base 单调，扣减没有唯一解 —— 拒绝对自己撒谎。
    if (!(k > 1.0e-6f)) return 0.0f;

    const float take = std::min(amount, room);
    const float target = v0 - take;   // >= floorV >= minValue，不会触发夹取
    base = target / k - flat;
    return take;
}

Attribute* AttributeSet::find(const std::string& name) {
    for (auto& kv : m_attrs) {
        if (kv.first == name) return &kv.second;
    }
    return nullptr;
}

const Attribute* AttributeSet::find(const std::string& name) const {
    for (const auto& kv : m_attrs) {
        if (kv.first == name) return &kv.second;
    }
    return nullptr;
}

Attribute& AttributeSet::ensure(const std::string& name) {
    if (Attribute* a = find(name)) return *a;
    m_attrs.emplace_back(name, Attribute{});
    return m_attrs.back().second;
}

void AttributeSet::setBase(const std::string& name, float v) {
    ensure(name).base = v;
}

bool AttributeSet::has(const std::string& name) const { return find(name) != nullptr; }

float AttributeSet::base(const std::string& name) const {
    const Attribute* a = find(name);
    return a ? a->base : 0.0f;
}

float AttributeSet::get(const std::string& name, float fallback) const {
    const Attribute* a = find(name);
    return a ? a->value() : fallback;
}

float AttributeSet::maxValue(const std::string& name) const {
    const Attribute* a = find(name);
    return a ? a->maxValue : 0.0f;
}

float AttributeSet::consume(const std::string& name, float amount) {
    Attribute* a = find(name);
    return a ? a->consume(amount) : 0.0f;
}

void AttributeSet::setRange(const std::string& name, float minV, float maxV) {
    Attribute& a = ensure(name);
    a.minValue = minV;
    a.maxValue = maxV;
    // 上下限写反（min > max）会让 `value()` 永远返回 min（先把 v 抬到 min
    // 再压到 max 又低于 min…实际是压到 max）。当成作者笔误，交换。
    if (a.minValue > a.maxValue) std::swap(a.minValue, a.maxValue);
}

void AttributeSet::addMod(const std::string& name, const StatModifier& m) {
    Attribute& a = ensure(name);
    StatModifier copy = m;
    // ⚠ 空 source 会被改写成 "anon"：否则"摘不掉"，症状是状态到期了
    //   属性还没回去，而摘的时候按 `""` 匹配会把**所有**匿名修正器一起摘掉。
    if (copy.source.empty()) copy.source = "anon";
    // `turnsLeft == 0` 在作者看来常常是"一次性"，按永久处理更符合直觉
    // （一次性效果应该走"加完就摘"，而不是等回合末）。
    if (copy.turnsLeft == 0) copy.turnsLeft = -1;
    a.mods.push_back(std::move(copy));
}

int AttributeSet::removeModsBySource(const std::string& name, const std::string& source) {
    Attribute* a = find(name);
    if (!a) return 0;
    const std::size_t before = a->mods.size();
    a->mods.erase(std::remove_if(a->mods.begin(), a->mods.end(),
                                 [&](const StatModifier& m) {
                                     return m.source == source;
                                 }),
                  a->mods.end());
    return static_cast<int>(before - a->mods.size());
}

int AttributeSet::removeModsBySource(const std::string& source) {
    int n = 0;
    for (auto& kv : m_attrs) {
        Attribute& a = kv.second;
        const std::size_t before = a.mods.size();
        a.mods.erase(std::remove_if(a.mods.begin(), a.mods.end(),
                                    [&](const StatModifier& m) {
                                        return m.source == source;
                                    }),
                     a.mods.end());
        n += static_cast<int>(before - a.mods.size());
    }
    return n;
}

int AttributeSet::removeModsBySourcePrefix(const std::string& prefix) {
    int n = 0;
    // ⚠ 用 `rfind(prefix, 0) == 0` 而不是 `compare(0, prefix.size(), prefix)`：
    //   两者等价，但 `compare` 在 `prefix.size() > s.size()` 时会抛
    //   `out_of_range`。source 常常比 id 短不了多少，"poison" vs "pois" 这种
    //   边界在测试里随手就会撞到。
    for (auto& kv : m_attrs) {
        Attribute& a = kv.second;
        const std::size_t before = a.mods.size();
        a.mods.erase(std::remove_if(a.mods.begin(), a.mods.end(),
                                    [&](const StatModifier& m) {
                                        return m.source.rfind(prefix, 0) == 0;
                                    }),
                     a.mods.end());
        n += static_cast<int>(before - a.mods.size());
    }
    return n;
}

int AttributeSet::tickTurn() {
    int removed = 0;
    for (auto& kv : m_attrs) {
        Attribute& a = kv.second;
        for (StatModifier& m : a.mods) {
            if (m.turnsLeft > 0) --m.turnsLeft;   // <0 永久，不动
        }
        const std::size_t before = a.mods.size();
        a.mods.erase(std::remove_if(a.mods.begin(), a.mods.end(),
                                    [](const StatModifier& m) {
                                        return m.turnsLeft == 0;
                                    }),
                     a.mods.end());
        removed += static_cast<int>(before - a.mods.size());
    }
    return removed;
}

void AttributeSet::clear() { m_attrs.clear(); }

std::string AttributeSet::dump() const {
    std::string s;
    char buf[192];
    for (const auto& kv : m_attrs) {
        std::snprintf(buf, sizeof(buf), "%s%s=%.2f",
                      s.empty() ? "" : " ", kv.first.c_str(),
                      static_cast<double>(kv.second.value()));
        s += buf;
    }
    return s;
}

// ================================================================ 状态

StatusInstance* StatusSet::find(const std::string& id) {
    for (StatusInstance& s : m_list) {
        if (s.def.id == id) return &s;
    }
    return nullptr;
}

const StatusInstance* StatusSet::find(const std::string& id) const {
    for (const StatusInstance& s : m_list) {
        if (s.def.id == id) return &s;
    }
    return nullptr;
}

StatusAddResult StatusSet::add(const StatusDef& def) {
    StatusAddResult r;
    if (def.id.empty()) {
        r.reason = "empty status id";
        return r;
    }
    if (def.turns <= 0) {
        r.reason = "turns <= 0 (would never tick)";
        return r;
    }

    StatusInstance* live = find(def.id);
    if (!live) {
        StatusInstance inst;
        inst.def = def;
        inst.def.maxStacks = std::max(def.maxStacks, 1);
        inst.turnsLeft = def.turns;
        inst.stacks = 1;
        m_list.push_back(std::move(inst));
        r.added = true;
        r.stacks = 1;
        return r;
    }

    // 已有同 id。
    switch (def.rule) {
    case StackRule::Refresh:
        // 回合数刷新到 max(当前, 新)，层数不变。
        live->turnsLeft = std::max(live->turnsLeft, def.turns);
        live->def = def;            // 数值可能被新版本覆盖（配表热更）
        live->def.maxStacks = std::max(def.maxStacks, 1);
        r.added = true;
        r.refreshed = true;
        r.stacks = live->stacks;
        return r;

    case StackRule::Stack:
        if (live->stacks >= live->def.maxStacks) {
            // 层满：只刷新回合（"中毒"层满之后继续中毒会延长，而不是浪费）
            live->turnsLeft = std::max(live->turnsLeft, def.turns);
            r.added = false;
            r.refreshed = true;
            r.stacks = live->stacks;
            r.reason = "max stacks reached (turns refreshed)";
            return r;
        }
        ++live->stacks;
        live->turnsLeft = std::max(live->turnsLeft, def.turns);
        r.added = true;
        r.stacks = live->stacks;
        return r;

    case StackRule::Independent: {
        // ⚠ 独立叠加要**真的多一条实例**，不能复用同 id 那条 —— 复用的
        //   表现是"两个护盾只挡了一次"，而作者以为自己配了两层。
        //   为了让 `has()/stacks()` 还能用，id 上打后缀 `#2`。
        StatusInstance inst;
        inst.def = def;
        inst.def.maxStacks = std::max(def.maxStacks, 1);
        inst.turnsLeft = def.turns;
        inst.stacks = 1;
        inst.def.id = def.id + "#" + std::to_string(m_list.size() + 1);
        m_list.push_back(std::move(inst));
        r.added = true;
        r.stacks = 1;
        return r;
    }
    }
    r.reason = "unknown stack rule";
    return r;
}

bool StatusSet::restore(const StatusDef& def, int turnsLeft, int stacks) {
    if (def.id.empty()) return false;
    // ⚠ 这里**刻意不检查 turnsLeft <= 0**：存档里可能出现"这一回合正是
    //   最后回合"的状态（turnsLeft == 1 存盘、读档后 tick 一次才该消失）。
    //   真的 <= 0 的是坏档，直接丢掉比让它变成一个永不 tick 的幽灵好。
    if (turnsLeft <= 0) return false;

    StatusInstance inst;
    inst.def = def;
    inst.def.maxStacks = std::max(def.maxStacks, 1);
    inst.turnsLeft = turnsLeft;
    inst.stacks = std::max(stacks, 1);
    m_list.push_back(std::move(inst));
    return true;
}

bool StatusSet::remove(const std::string& id) {    for (std::size_t i = 0; i < m_list.size(); ++i) {
        if (m_list[i].def.id == id) {
            m_list.erase(m_list.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

void StatusSet::clear() { m_list.clear(); }

bool StatusSet::has(const std::string& id) const { return find(id) != nullptr; }

int StatusSet::stacks(const std::string& id) const {
    // ⚠ 不能 `if (find(id)) return s->stacks;` 就完事：独立叠加的状态
    //   id 是 `shield` / `shield#2` / `shield#3`，第一版那样写会**只报 1 层**
    //   （find 命中了第一条就返回），于是"两个护盾只挡了一次"在数值上
    //   完全看不出来 —— 判据 H3 就是被这条打红的。
    //   语义：`stacks()` 报**总层数**（同名独立实例的层数相加）。
    int n = 0;
    for (const StatusInstance& inst : m_list) {
        if (inst.def.id == id) {
            n += inst.stacks;
        } else if (inst.def.id.rfind(id + "#", 0) == 0) {
            n += inst.stacks;
        }
    }
    return n;
}

int StatusSet::turnsLeft(const std::string& id) const {
    const StatusInstance* s = find(id);
    return s ? s->turnsLeft : 0;
}

std::vector<StatusSet::DotTick> StatusSet::tickTurn() {
    std::vector<DotTick> dots;
    // ① 先按**当前**层数收集 dot（顺序 = 加入顺序，可复现）
    for (const StatusInstance& s : m_list) {
        if (s.def.dotPerTurn > 0.0f) {
            DotTick d;
            d.statusId = s.def.id;
            d.attribute = s.def.dotAttribute.empty() ? "hp" : s.def.dotAttribute;
            d.element = s.def.dotElement;
            // 层数直接乘：中毒 3 层每回合 3× 伤害。这是最常见的约定。
            d.amount = s.def.dotPerTurn * static_cast<float>(s.stacks);
            dots.push_back(std::move(d));
        }
    }
    // ② 再推进寿命并从**后往前**删（删前面的会让下标错位）
    for (std::size_t i = m_list.size(); i > 0; --i) {
        StatusInstance& s = m_list[i - 1];
        if (s.turnsLeft > 0) --s.turnsLeft;
        if (s.turnsLeft <= 0) {
            m_list.erase(m_list.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }
    }
    return dots;
}

// ================================================================ 状态 → 属性

namespace {

// 状态挂上来的修正器统一用这个前缀。改它 = 改"谁摘谁"的边界。
constexpr const char* kStatusPrefix = "status:";

} // namespace

int removeStatusMods(AttributeSet& attrs) {
    return attrs.removeModsBySourcePrefix(kStatusPrefix);
}

void rebuildStatusMods(const StatusSet& set, AttributeSet& attrs) {
    // 文件头第 3 条：**先摘干净再重加**。
    // ⚠ 只摘 `status:` 前缀 —— 装备/脚本挂的必须毫发无伤，否则"挂个 buff
    //   装备加成没了"，而且这个 bug 在只测属性的用例里看不出来。
    removeStatusMods(attrs);

    for (std::size_t k = 0; k < set.size(); ++k) {
        const StatusInstance& inst = set.at(k);
        // 独立叠加的状态 id 带 `#N` 后缀，这里原样带上 —— 于是"两个护盾"
        // 是两个不同 source，各自能被单独摘掉。
        const std::string source = std::string(kStatusPrefix) + inst.def.id;
        const int stacks = std::max(inst.stacks, 1);
        for (const NamedMod& nm : inst.def.mods) {
            // 没写属性名 = 无处可挂。静默跳过会让作者以为配了没生效，
            // 所以这里在 dump / explain 里查得到（属性列表里不会出现它）。
            if (nm.attribute.empty()) continue;
            for (int s = 0; s < stacks; ++s) {
                StatModifier c = nm.mod;
                c.source = source;
                // 寿命由 StatusSet 管，属性侧不重复计时（否则两边各减一次，
                // 3 回合的 buff 只撑 1.5 回合）。
                c.turnsLeft = -1;
                attrs.addMod(nm.attribute, c);
            }
        }
    }
}

// ================================================================ 伤害

namespace {

// 约定的属性名（做成常量，避免各处手写字符串拼错 —— 拼错的表现是
// "护甲永远是 0"，因为 `get()` 缺属性直接返回 fallback 不报错）。
constexpr const char* kHpAttr = "hp";
constexpr const char* kShieldAttr = "shield";
constexpr const char* kVulnAttr = "vulnerable";

// 物理走护甲，其余走对应抗性，真伤两样都不走。
float defenseOf(const AttributeSet& target, DamageType type, std::string& label) {
    switch (type) {
    case DamageType::Physical: {
        if (target.has("armor")) { label = "armor"; return target.get("armor"); }
        if (target.has("defense")) { label = "defense"; return target.get("defense"); }
        label = "armor(none)";
        return 0.0f;
    }
    case DamageType::True:
        label = "true";
        return 0.0f;
    default: {
        label = std::string("resist_") + damageTypeName(type);
        return target.get(label);
    }
    }
}

} // namespace

std::string DamageResult::summary() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%.1f%s -> armor %.1f -> vuln %.1f -> shield %.1f -> hp %.1f",
                  static_cast<double>(incoming), crit ? "(crit)" : "",
                  static_cast<double>(afterArmor), static_cast<double>(afterVuln),
                  static_cast<double>(absorbed), static_cast<double>(dealt));
    return std::string(buf);
}

DamageResult resolveDamage(const AttributeSet& attacker, AttributeSet& target,
                           const DamageSpec& spec, Random& rng) {
    DamageResult r;
    // 每步都记，包括"没发生"的步 —— 步数固定，判据才能按下标读。
    // 只记"发生了的"会让 A 用例的下标在加了暴击之后整体错位。
    const auto step = [&r](const std::string& label, float before, float after) {
        DamageStep s;
        s.label = label;
        s.before = before;
        s.after = after;
        r.steps.push_back(std::move(s));
    };

    // ---------- ① 暴击
    // 有效暴击率：spec 给了就用 spec 的，没给就从攻击方属性取。
    // ⚠ 消耗随机的条件必须**只看这里算出来的数**，与实现细节无关 ——
    //   否则同一份录像回放会因为"改了个属性名"而错位。
    float critChance = spec.critChance;
    if (!(critChance > 0.0f) && attacker.has("critChance")) {
        critChance = attacker.get("critChance");
    }
    bool crit = spec.crit;
    if (!crit && spec.canCrit && critChance > 0.0f) {
        crit = rng.chance(critChance);   // 恰好消耗一次
    }
    const float amount = spec.amount > 0.0f ? spec.amount : 0.0f;
    const float incoming = crit ? amount * spec.critMul : amount;
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), crit ? "crit x%.2f (%.1f%%)" : "no-crit (%.1f%%)",
                      static_cast<double>(spec.critMul), static_cast<double>(critChance));
        step(buf, amount, incoming);
    }
    r.crit = crit;
    r.incoming = incoming;

    // ---------- ② 护甲 / 抗性（穿透后再套曲线）
    std::string armorLabel;
    const float armorRaw = defenseOf(target, spec.type, armorLabel);
    float armorAfterPen = 0.0f;
    float reduction = 0.0f;
    if (spec.type != DamageType::True && armorRaw > 0.0f) {
        const float pen = std::min(std::max(spec.armorPen, 0.0f), 1.0f);
        armorAfterPen = armorRaw * (1.0f - pen);
        // reduction = armor / (armor + k)：永不免疫（见头文件 kArmorK 注释）
        reduction = armorAfterPen / (armorAfterPen + kArmorK);
    }
    const float afterArmor = incoming * (1.0f - reduction);
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s=%.1f pen=%.0f%% red=%.1f%%",
                      armorLabel.c_str(), static_cast<double>(armorRaw),
                      static_cast<double>(std::min(std::max(spec.armorPen, 0.0f), 1.0f) * 100.0f),
                      static_cast<double>(reduction * 100.0f));
        step(buf, incoming, afterArmor);
    }
    r.afterArmor = afterArmor;

    // ---------- ③ 易伤（目标身上的 percent）
    const float vuln = target.get(kVulnAttr, 0.0f);
    const float afterVuln = afterArmor * (1.0f + vuln * 0.01f);
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "vuln=%+.1f%%", static_cast<double>(vuln));
        step(buf, afterArmor, afterVuln);
    }
    r.afterVuln = afterVuln;

    // ---------- ④ 护盾（真伤**也**被护盾吃，这就是真伤与"直接改 hp"的区别）
    float remain = afterVuln;
    if (target.has(kShieldAttr)) {
        // ⚠ 用 `consume()` 而不是 `addMod(-x)`：后者会在属性上留一条永久
        //   修正器，"护盾回到 0"之后每次 rebuild 都会再扣一次。
        const float taken = target.consume(kShieldAttr, remain);
        r.absorbed = taken;
        remain -= taken;
    }
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "shield absorbed %.1f",
                      static_cast<double>(r.absorbed));
        step(buf, afterVuln, remain);
    }

    // ---------- ⑤ 扣血
    r.hpBefore = target.get(kHpAttr, 0.0f);
    // `dealt` 是**真正扣掉的**：血量只有 5 时来了 100 点伤害，dealt = 5。
    // 用 afterVuln 当 dealt 会让伤害统计虚高，也让"过量伤害"没法单独算。
    const float hpTaken = target.consume(kHpAttr, remain);
    r.dealt = hpTaken;
    r.hpAfter = target.get(kHpAttr, 0.0f);
    r.lethal = (r.hpBefore > 0.0f && r.hpAfter <= 0.0f);
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "hp %.1f -> %.1f%s",
                      static_cast<double>(r.hpBefore),
                      static_cast<double>(r.hpAfter),
                      r.lethal ? " (lethal)" : "");
        step(buf, r.hpBefore, r.hpAfter);
    }

    return r;
}

} // namespace gameplay

