#include "gameplay/GameplayTypes.h"

#include <algorithm>

namespace gameplay {

StatusDef toStatusDef(const StatusState& s) {
    StatusDef d;
    d.id = s.id;
    d.name = s.name;
    // `turns` 用 turnsLeft：`restore()` 不重算回合，这里只是让"如果之后有人
    // 拿这个 def 去 `add()`"时行为不离谱（那就是一次全新的施加）。
    d.turns = s.turnsLeft > 0 ? s.turnsLeft : 1;
    d.maxStacks = s.maxStacks > 0 ? s.maxStacks : 1;
    d.rule = s.rule;
    d.mods.reserve(s.mods.size());
    for (const StatusModState& m : s.mods) {
        NamedMod nm;
        nm.attribute = m.attribute;
        nm.mod.op = m.op;
        nm.mod.value = m.value;
        // source 由 `rebuildStatusMods` 统一改写成 `status:<id>`，
        // 这里留空也无所谓 —— 关键是**不要**让作者自己填。
        nm.mod.source = "status:" + s.id;
        nm.mod.turnsLeft = -1;
        d.mods.push_back(std::move(nm));
    }
    d.dotPerTurn = s.dotPerTurn;
    d.dotAttribute = s.dotAttribute.empty() ? "hp" : s.dotAttribute;
    d.dotElement = s.dotElement;
    return d;
}

StatusState toStatusState(const StatusDef& d, int turnsLeft, int stacks) {
    StatusState s;
    s.id = d.id;
    s.name = d.name.empty() ? d.id : d.name;
    s.turnsLeft = turnsLeft;
    s.stacks = stacks > 0 ? stacks : 1;
    s.maxStacks = d.maxStacks > 0 ? d.maxStacks : 1;
    s.rule = d.rule;
    s.mods.reserve(d.mods.size());
    for (const NamedMod& nm : d.mods) {
        StatusModState m;
        m.attribute = nm.attribute;
        m.op = nm.mod.op;
        m.value = nm.mod.value;
        s.mods.push_back(std::move(m));
    }
    s.dotPerTurn = d.dotPerTurn;
    s.dotAttribute = d.dotAttribute.empty() ? "hp" : d.dotAttribute;
    s.dotElement = d.dotElement;
    return s;
}

void buildAttributes(const std::vector<AttrState>& attrs,
                     const std::vector<StatusState>& statuses, AttributeSet& out) {
    out.clear();
    for (const AttrState& a : attrs) {
        if (a.name.empty()) continue;
        out.setBase(a.name, a.base);
        out.setRange(a.name, a.minValue, a.maxValue);
    }

    if (statuses.empty()) return;

    // ⚠ 用 `restore()` 而不是 `add()`：`add()` 会按叠加规则**重算**层数与
    //   回合，于是"读档时 3 层毒变 1 层"。这是最容易在自检里被漏掉的一处，
    //   因为不读档的那条路径（施法时 add）看起来完全正常。
    StatusSet set;
    for (const StatusState& s : statuses) {
        set.restore(toStatusDef(s), s.turnsLeft, s.stacks);
    }
    rebuildStatusMods(set, out);
}

void storeBases(const AttributeSet& from, std::vector<AttrState>& attrs) {
    for (AttrState& a : attrs) {
        if (a.name.empty()) continue;
        if (!from.has(a.name)) continue;
        a.base = from.base(a.name);
    }
}

AttrState& ensureAttr(std::vector<AttrState>& attrs, const std::string& name) {
    for (AttrState& a : attrs) {
        if (a.name == name) return a;
    }
    AttrState a;
    a.name = name;
    a.base = 0.0f;
    a.minValue = 0.0f;
    a.maxValue = 1.0e9f;
    attrs.push_back(std::move(a));
    return attrs.back();
}

const AttrState* findAttr(const std::vector<AttrState>& attrs, const std::string& name) {
    for (const AttrState& a : attrs) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

std::vector<AttrState> defaultAttributes(float hp, float attack, float armor) {
    std::vector<AttrState> v;
    v.push_back(AttrState{"hp", hp, 0.0f, hp});
    v.push_back(AttrState{"attack", attack, 0.0f, 1.0e9f});
    v.push_back(AttrState{"armor", armor, 0.0f, 1.0e9f});
    // 下面两条给 0：没有护盾 / 没有易伤。**必须建出来**，否则
    // `resolveDamage` 读 `vulnerable` 走 fallback 0（结果一样），但面板
    // 里看不到它们，作者就不会想到"易伤是可以加的"。
    v.push_back(AttrState{"shield", 0.0f, 0.0f, 1.0e9f});
    v.push_back(AttrState{"vulnerable", 0.0f, -1000.0f, 1000.0f});
    return v;
}

// ---------------------------------------------------------------- 预设状态

StatusDef presetPoison() {
    StatusDef d;
    d.id = "poison";
    d.name = "Poison";
    d.turns = 3;
    d.maxStacks = 5;
    d.rule = StackRule::Stack;
    d.dotPerTurn = 5.0f;
    d.dotAttribute = "hp";
    d.dotElement = "poison";
    NamedMod nm;
    nm.attribute = "attack";
    nm.mod.op = ModOp::Flat;
    nm.mod.value = -3.0f;
    d.mods.push_back(nm);
    return d;
}

StatusDef presetShield() {
    StatusDef d;
    d.id = "shield";
    d.name = "Shield";
    d.turns = 2;
    d.maxStacks = 3;
    // ⚠ Independent：两份护盾必须**各自**到期、各自被打破。用 Stack 的话
    //   一份盾挨打会连累另一份（层数是同一条实例的），作者会以为配了两层。
    d.rule = StackRule::Independent;
    NamedMod nm;
    nm.attribute = "shield";
    nm.mod.op = ModOp::Flat;
    nm.mod.value = 30.0f;
    d.mods.push_back(nm);
    return d;
}

StatusDef presetVulnerable() {
    StatusDef d;
    d.id = "vulnerable";
    d.name = "Vulnerable";
    d.turns = 2;
    d.maxStacks = 2;
    d.rule = StackRule::Stack;
    NamedMod nm;
    nm.attribute = "vulnerable";
    nm.mod.op = ModOp::Flat;
    nm.mod.value = 25.0f;   // 每层 +25% 受伤
    d.mods.push_back(nm);
    return d;
}

bool statusDefById(const std::string& id, StatusDef& out) {
    if (id == "poison") {
        out = presetPoison();
        return true;
    }
    if (id == "shield") {
        out = presetShield();
        return true;
    }
    if (id == "vulnerable" || id == "vuln") {
        out = presetVulnerable();
        return true;
    }
    return false;
}

} // namespace gameplay
