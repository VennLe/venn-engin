// ============================================================
// gameplay/GameplaySelfTest.cpp —— P2 玩法状态层验收
//                              （`Game.exe --selftest gameplay`）
//
// 六组断言，每一组对应"这套东西会怎么算错"：
//
//   G 属性    —— 求值顺序 / Override / clamp 位置 / 两种百分比 / 三种摘除。
//                这一组全是"两种实现都看起来很合理"的地方，所以每条都
//                故意构造了**结果不同**的输入（值不同才是判据）。
//   H 状态    —— 叠加规则三条 / 重建而不是增量 / 装备加成不被洗掉 / dot。
//   I 伤害    —— 五步明细 / 曲线 / 穿透 / 易伤 / 护盾 / 真伤 / 过量 / 暴击。
//                ⚠ 判据读 `steps`，不读最终血量：只读血量的话"护甲没生效"
//                  和"易伤没生效"可以给出完全一样的数。
//   J 随机    —— 可复现 / 三流不串 / 存档往返 / 边界不消耗。
//   K 回合    —— 行动条频率 / tie-break / 插队 / 取消 / 阵亡 / 预读一致。
//
// ⚠ detail 里**不能有空格**：验收脚本用 `detail=(\S*)` 解析。
// ============================================================

#include "gameplay/GameplaySelfTest.h"

#include "core/Logger.h"
#include "gameplay/CardZones.h"
#include "gameplay/CardTable.h"
#include "gameplay/CardFx.h"          // P 组（P3-4 卡牌表现模板）
#include "gameplay/DamageNumbers.h"   // O 组（P3-4 伤害数字层）
#include "gameplay/GameRandom.h"
#include "gameplay/GameplayTypes.h"
#include "gameplay/Stats.h"
#include "gameplay/TurnQueue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace gameplay {

namespace {

int g_pass = 0;
int g_fail = 0;

void report(const char* name, double actual, double expected, double tol = 0.0) {
    const bool ok = (actual - expected) <= tol && (expected - actual) <= tol;
    VK_LOG_INFO("GAMEPLAY-TEST %s actual=%.6f expected=%.6f tol=%.6f result=%s", name, actual,
                expected, tol, ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

void reportFlag(const char* name, bool ok, const std::string& detail = {}) {
    VK_LOG_INFO("GAMEPLAY-TEST %s actual=%d expected=1 tol=0 detail=%s result=%s", name, ok ? 1 : 0,
                detail.c_str(), ok ? "PASS" : "FAIL");
    if (ok) ++g_pass;
    else ++g_fail;
}

std::size_t modCount(const AttributeSet& s, const std::string& name) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s.nameAt(i) == name) return s.at(i).mods.size();
    }
    return 0;
}

StatModifier mod(ModOp op, float v, const std::string& src = "test") {
    StatModifier m;
    m.op = op;
    m.value = v;
    m.source = src;
    return m;
}

// 目标：血 1000、护甲可调。
AttributeSet makeTarget(float hp, float armor) {
    AttributeSet t;
    t.setBase("hp", hp);
    t.setRange("hp", 0.0f, 1.0e9f);
    if (armor > 0.0f) t.setBase("armor", armor);
    return t;
}

// ---------------------------------------------------------------- G 属性

void groupAttribute() {
    // G1：顺序契约 (10+5)*(1+100%)*1.2 = 36。
    //     换个顺序（先乘百分比再加 flat）得 10*1.2*2+5 = 29 —— 两边都
    //     "看起来对"，所以必须钉住一个具体的数。
    {
        AttributeSet s;
        s.setBase("attack", 10.0f);
        s.addMod("attack", mod(ModOp::Flat, 5.0f));
        s.addMod("attack", mod(ModOp::AddPct, 100.0f));
        s.addMod("attack", mod(ModOp::MulPct, 1.2f));
        report("G1.eval-order", s.get("attack"), 36.0, 1e-4);
    }
    // G2：Override 后加的赢（两个变身效果叠在一起，用最后上的那个）。
    {
        AttributeSet s;
        s.setBase("attack", 10.0f);
        s.addMod("attack", mod(ModOp::Override, 5.0f));
        s.addMod("attack", mod(ModOp::Override, 7.0f));
        report("G2.override-last", s.get("attack"), 7.0, 1e-4);
        // 顶替之后 flat 仍然加在顶替值上（10 -> 7 -> 7+3 = 10）
        s.addMod("attack", mod(ModOp::Flat, 3.0f));
        report("G2.override-then-flat", s.get("attack"), 10.0, 1e-4);
    }
    // G3：clamp 只做一次、且作用在**最终值**上。
    //     base 1000 超过上限 100，一个 ×0.5 的修正：
    //       · 正确（最后夹）  1000*0.5 = 500 -> 夹到 100
    //       · 错误（先夹 base）100*0.5  = 50
    //     50 与 100 差一倍，而且"先夹 base"听起来非常直观。
    {
        AttributeSet s;
        s.setBase("attack", 1000.0f);
        s.setRange("attack", 0.0f, 100.0f);
        s.addMod("attack", mod(ModOp::MulPct, 0.5f));
        report("G3.clamp-at-end", s.get("attack"), 100.0, 1e-4);
    }
    // G4：AddPct 求和 vs MulPct 连乘。base 100，两条 +100% / 两条 ×2：
    //     300 vs 400。这是配表时最容易配错的一处。
    {
        AttributeSet a;
        a.setBase("v", 100.0f);
        a.addMod("v", mod(ModOp::AddPct, 100.0f));
        a.addMod("v", mod(ModOp::AddPct, 100.0f));
        report("G4.addpct-sum", a.get("v"), 300.0, 1e-4);
        AttributeSet m;
        m.setBase("v", 100.0f);
        m.addMod("v", mod(ModOp::MulPct, 2.0f));
        m.addMod("v", mod(ModOp::MulPct, 2.0f));
        report("G4.mulpct-mul", m.get("v"), 400.0, 1e-4);
    }
    // G5：按 source 摘除（装备脱下）。
    {
        AttributeSet s;
        s.setBase("attack", 100.0f);
        s.addMod("attack", mod(ModOp::Flat, 10.0f, "equip:sword"));
        const double withEq = s.get("attack");
        const int removed = s.removeModsBySource("equip:sword");
        report("G5.source-remove-count", removed, 1.0);
        report("G5.source-remove-value", withEq, 110.0, 1e-4);
        report("G5.source-remove-after", s.get("attack"), 100.0, 1e-4);
    }
    // G6：空 source 被改写成 anon，仍然摘得掉（否则"摘不掉"是静默的）。
    {
        AttributeSet s;
        s.setBase("attack", 100.0f);
        s.addMod("attack", mod(ModOp::Flat, 10.0f, ""));
        const int removed = s.removeModsBySource("anon");
        report("G6.anon-removable", removed, 1.0);
        report("G6.anon-value", s.get("attack"), 100.0, 1e-4);
    }
    // G7：前缀摘除。独立叠加的状态 id 带 `#2` 后缀，按精确 source 摘会漏。
    {
        AttributeSet s;
        s.setBase("attack", 100.0f);
        s.addMod("attack", mod(ModOp::Flat, 5.0f, "status:poison"));
        s.addMod("attack", mod(ModOp::Flat, 5.0f, "status:poison#2"));
        s.addMod("attack", mod(ModOp::Flat, 7.0f, "equip:ring"));
        const int removed = s.removeModsBySourcePrefix("status:");
        report("G7.prefix-remove-count", removed, 2.0);
        // 装备那条必须还在（100 + 7 = 107）——"摘干净"不能摘到别人头上
        report("G7.prefix-keeps-equip", s.get("attack"), 107.0, 1e-4);
    }
}

// ---------------------------------------------------------------- H 状态

StatusDef poisonDef(int turns, int maxStacks) {
    StatusDef d;
    d.id = "poison";
    d.name = "中毒";
    d.turns = turns;
    d.maxStacks = maxStacks;
    d.rule = StackRule::Stack;
    d.dotPerTurn = 2.0f;
    d.dotAttribute = "hp";
    NamedMod nm;
    nm.attribute = "attack";
    nm.mod = mod(ModOp::Flat, -2.0f);
    d.mods.push_back(nm);
    return d;
}

void groupStatus() {
    // H1：Refresh —— 回合数刷新到 max(当前, 新)，层数不变（"眩晕"）。
    {
        StatusSet set;
        StatusDef d;
        d.id = "stun";
        d.turns = 3;
        d.maxStacks = 1;
        d.rule = StackRule::Refresh;
        set.add(d);
        set.add(d);
        report("H1.refresh-count", static_cast<double>(set.size()), 1.0);
        report("H1.refresh-stacks", set.stacks("stun"), 1.0);
        report("H1.refresh-turns", set.turnsLeft("stun"), 3.0);
    }
    // H2：Stack —— 层数 +1，到上限之后只刷新回合且 added=false。
    {
        StatusSet set;
        const StatusDef d = poisonDef(3, 3);
        set.add(d);
        set.add(d);
        set.add(d);
        report("H2.stack-3", set.stacks("poison"), 3.0);
        const StatusAddResult r = set.add(d);
        report("H2.stack-capped-stacks", set.stacks("poison"), 3.0);
        reportFlag("H2.stack-capped-flag", !r.added, "max");
        report("H2.stack-capped-turns", set.turnsLeft("poison"), 3.0);
    }
    // H3：Independent —— 真的多一条实例（"两个护盾"必须挡两次）。
    {
        StatusSet set;
        StatusDef d;
        d.id = "shield";
        d.turns = 2;
        d.maxStacks = 9;
        d.rule = StackRule::Independent;
        set.add(d);
        set.add(d);
        report("H3.indep-instances", static_cast<double>(set.size()), 2.0);
        report("H3.indep-stacks", set.stacks("shield"), 2.0);
    }
    // H4：重建而不是增量。3 层毒 → attack 上恰好 3 条 modifier；
    //     再重建一次仍然是 3 条（增量法会变 6 条）。
    {
        AttributeSet attrs;
        attrs.setBase("attack", 100.0f);
        StatusSet set;
        const StatusDef d = poisonDef(3, 3);
        set.add(d);
        set.add(d);
        set.add(d);
        rebuildStatusMods(set, attrs);
        report("H4.rebuild-3mods", static_cast<double>(modCount(attrs, "attack")), 3.0);
        report("H4.rebuild-value", attrs.get("attack"), 94.0, 1e-4);
        rebuildStatusMods(set, attrs);
        report("H4.rebuild-idempotent", static_cast<double>(modCount(attrs, "attack")), 3.0);
        report("H4.rebuild-idempotent-value", attrs.get("attack"), 94.0, 1e-4);
    }
    // H5：装备加成不被重建洗掉（"挂个 buff 装备加成没了"）。
    //     100(白值) + 10(装备) - 2×3(毒 3 层) = 104。
    //     洗多了 → 94，增量了 → 110，两种错值都离 104 很远。
    {
        AttributeSet attrs;
        attrs.setBase("attack", 100.0f);
        attrs.addMod("attack", mod(ModOp::Flat, 10.0f, "equip:sword"));
        StatusSet set;
        const StatusDef d = poisonDef(3, 3);
        set.add(d);
        set.add(d);
        set.add(d);
        rebuildStatusMods(set, attrs);
        rebuildStatusMods(set, attrs);
        report("H5.equip-survives", attrs.get("attack"), 104.0, 1e-4);
    }
    // H6：每回合 dot = 层数 × 单层量；且顺序 = 加入顺序。
    {
        StatusSet set;
        const StatusDef d = poisonDef(3, 3);
        set.add(d);
        set.add(d);
        set.add(d);
        const std::vector<StatusSet::DotTick> dots = set.tickTurn();
        report("H6.dot-count", static_cast<double>(dots.size()), 1.0);
        report("H6.dot-amount", dots.empty() ? 0.0 : dots[0].amount, 6.0, 1e-4);
        // 回合数 3 → 4 → 减 1 之后还剩 2
        report("H6.dot-turns-left", set.turnsLeft("poison"), 2.0);
    }
    // H7：turns = 1 的状态在回合末消失。
    {
        StatusSet set;
        StatusDef d;
        d.id = "haste";
        d.turns = 1;
        d.rule = StackRule::Refresh;
        set.add(d);
        set.tickTurn();
        report("H7.expire-size", static_cast<double>(set.size()), 0.0);
    }
    // H8：非法定义被拒（turns <= 0 永不 tick，加了就是个静默黑洞）。
    {
        StatusSet set;
        StatusDef d;
        d.id = "bad";
        d.turns = 0;
        const StatusAddResult r = set.add(d);
        reportFlag("H8.reject-turns0", !r.added && set.size() == 0, "turns");
    }
}

// ---------------------------------------------------------------- I 伤害

void groupDamage() {
    // I1：每条伤害都必须有**五步**明细。步数固定，判据才能按下标读。
    {
        AttributeSet atk;
        atk.setBase("attack", 100.0f);
        AttributeSet tgt = makeTarget(1000.0f, 0.0f);
        Random rng;
        rng.seed(1u);
        DamageSpec spec;
        spec.amount = 100.0f;
        spec.type = DamageType::Physical;
        const DamageResult r = resolveDamage(atk, tgt, spec, rng);
        report("I1.step-count", static_cast<double>(r.steps.size()), 5.0);
        reportFlag("I1.summary-nonempty", !r.summary().empty(), "summary");
    }
    // I2：护甲曲线。armor = 100 = k → 减伤 50%。
    {
        AttributeSet atk;
        AttributeSet tgt = makeTarget(1000.0f, 100.0f);
        Random rng;
        rng.seed(2u);
        DamageSpec spec;
        spec.amount = 100.0f;
        const DamageResult r = resolveDamage(atk, tgt, spec, rng);
        report("I2.armor-100", r.afterArmor, 50.0, 1e-3);
        report("I2.armor-dealt", r.dealt, 50.0, 1e-3);
        report("I2.armor-hp", r.hpAfter, 950.0, 1e-3);
    }
    // I3：穿透先削护甲再走曲线。armor 100、穿透 50% → 50 → 50/150 = 1/3。
    {
        AttributeSet atk;
        AttributeSet tgt = makeTarget(1000.0f, 100.0f);
        Random rng;
        rng.seed(3u);
        DamageSpec spec;
        spec.amount = 100.0f;
        spec.armorPen = 0.5f;
        const DamageResult r = resolveDamage(atk, tgt, spec, rng);
        report("I3.pen-half", r.afterArmor, 66.6667, 1e-2);
    }
    // I4：易伤是**目标身上**的百分比。
    {
        AttributeSet atk;
        AttributeSet tgt = makeTarget(1000.0f, 0.0f);
        tgt.setBase("vulnerable", 50.0f);
        Random rng;
        rng.seed(4u);
        DamageSpec spec;
        spec.amount = 100.0f;
        const DamageResult r = resolveDamage(atk, tgt, spec, rng);
        report("I4.vuln-50", r.afterVuln, 150.0, 1e-3);
        report("I4.vuln-dealt", r.dealt, 150.0, 1e-3);
    }
    // I5：护盾先吃，剩下的才扣血。
    {
        AttributeSet atk;
        AttributeSet tgt = makeTarget(1000.0f, 0.0f);
        tgt.setBase("shield", 30.0f);
        tgt.setRange("shield", 0.0f, 1.0e9f);
        Random rng;
        rng.seed(5u);
        DamageSpec spec;
        spec.amount = 50.0f;
        const DamageResult r = resolveDamage(atk, tgt, spec, rng);
        report("I5.absorbed", r.absorbed, 30.0, 1e-3);
        report("I5.dealt", r.dealt, 20.0, 1e-3);
        report("I5.shield-left", tgt.get("shield"), 0.0, 1e-3);
        report("I5.hp", r.hpAfter, 980.0, 1e-3);
    }
    // I6：真伤无视护甲与抗性（只被护盾吃）。
    {
        AttributeSet atk;
        AttributeSet tgt = makeTarget(1000.0f, 1000.0f);
        Random rng;
        rng.seed(6u);
        DamageSpec spec;
        spec.amount = 100.0f;
        spec.type = DamageType::True;
        const DamageResult r = resolveDamage(atk, tgt, spec, rng);
        report("I6.true-ignores-armor", r.afterArmor, 100.0, 1e-3);
    }
    // I7：过量伤害 —— dealt 是**真正扣掉的**，不是进公式的量。
    {
        AttributeSet atk;
        AttributeSet tgt = makeTarget(5.0f, 0.0f);
        Random rng;
        rng.seed(7u);
        DamageSpec spec;
        spec.amount = 100.0f;
        const DamageResult r = resolveDamage(atk, tgt, spec, rng);
        report("I7.overkill-dealt", r.dealt, 5.0, 1e-3);
        report("I7.overkill-hp", r.hpAfter, 0.0, 1e-3);
        reportFlag("I7.overkill-lethal", r.lethal, "lethal");
    }
    // I8：暴击消耗随机的条件。0% 一次都不耗、50% 恰好耗一次。
    //     ⚠ 判据用 `state()` 变没变，不用"暴没暴击"——后者在 50% 下本身就随机。
    {
        AttributeSet atk;
        AttributeSet tgt = makeTarget(100000.0f, 0.0f);
        Random rng;
        rng.seed(8u);
        const uint64_t s0 = rng.state();
        DamageSpec spec;
        spec.amount = 1.0f;
        spec.critChance = 0.0f;
        resolveDamage(atk, tgt, spec, rng);
        reportFlag("I8.crit0-no-consume", rng.state() == s0, "state");
        const uint64_t s1 = rng.state();
        spec.critChance = 50.0f;
        resolveDamage(atk, tgt, spec, rng);
        reportFlag("I8.crit50-consumes", rng.state() != s1, "state");
    }
}

// ---------------------------------------------------------------- J 随机

void groupRandom() {
    // J1：同种子同序列。
    {
        Random a;
        Random b;
        a.seed(1234u);
        b.seed(1234u);
        bool same = true;
        for (int i = 0; i < 16; ++i) {
            if (a.nextU32() != b.nextU32()) same = false;
        }
        reportFlag("J1.same-seed", same, "16draws");
    }
    // J2：同 initState、不同 seq → 必须是两条不相关的流。
    {
        Random a;
        Random b;
        a.seed(1234u, 1u);
        b.seed(1234u, 2u);
        int diff = 0;
        for (int i = 0; i < 16; ++i) {
            if (a.nextU32() != b.nextU32()) ++diff;
        }
        report("J2.seq-separates", diff, 16.0);
    }
    // J3：存档往返 —— dump 之后接着抽的顺序必须完全一致。
    {
        Random a;
        a.seed(77u);
        for (int i = 0; i < 3; ++i) a.nextU32();
        const std::string d = a.dump();
        Random b;
        const bool parsed = b.parse(d);
        bool same = parsed;
        for (int i = 0; i < 8; ++i) {
            if (a.nextU32() != b.nextU32()) same = false;
        }
        reportFlag("J3.dump-parse", same, parsed ? "ok" : "parsefail");
    }
    // J4：0 状态是退化流，必须拒绝（否则之后每个数都是同一个）。
    {
        Random r;
        reportFlag("J4.reject-zero-state", !r.setState(0u, 1u), "zero");
    }
    // J5：0% 与 100% 不消耗序列 —— 给一件装备加 100% 暴击不该改变抽牌。
    {
        Random r;
        r.seed(11u);
        const uint64_t s0 = r.state();
        r.chance(0.0f);
        r.chance(100.0f);
        const bool noConsume = (r.state() == s0);
        r.chance(50.0f);
        reportFlag("J5.extremes-no-consume", noConsume, "state");
        reportFlag("J5.mid-consumes", r.state() != s0, "state");
    }
    // J6：三条流互不相关 —— 把玩法流抽 100 次，掉落流的下一个数不能变。
    {
        RandomStreams a;
        RandomStreams b;
        a.seedAll(999u);
        b.seedAll(999u);
        for (int i = 0; i < 100; ++i) a.gameplay.nextU32();
        reportFlag("J6.streams-independent", a.loot.nextU32() == b.loot.nextU32(), "loot");
    }
    // J7：洗牌确定性 + 内容不丢。
    {
        std::vector<int> v1;
        std::vector<int> v2;
        for (int i = 0; i < 10; ++i) {
            v1.push_back(i);
            v2.push_back(i);
        }
        Random a;
        Random b;
        a.seed(5u);
        b.seed(5u);
        a.shuffle(v1);
        b.shuffle(v2);
        const bool same = (v1 == v2);
        std::vector<int> s1 = v1;
        std::sort(s1.begin(), s1.end());
        bool ordered = true;
        for (int i = 0; i < 10; ++i) {
            if (s1[static_cast<std::size_t>(i)] != i) ordered = false;
        }
        reportFlag("J7.shuffle-deterministic", same, "same");
        reportFlag("J7.shuffle-permutation", ordered, "perm");
    }
    // J8：intRange 的边界（闭区间，不能越界）。
    {
        Random r;
        r.seed(21u);
        bool inRange = true;
        int lo = 99;
        int hi = -99;
        for (int i = 0; i < 2000; ++i) {
            const int v = r.intRange(3, 7);
            if (v < 3 || v > 7) inRange = false;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        reportFlag("J8.intrange-bounds", inRange, "3-7");
        // 两端都要真的出现过（只出 3 和 4 是"乘法缩放写错了"的典型症状）
        report("J8.intrange-touches-lo", lo, 3.0);
        report("J8.intrange-touches-hi", hi, 7.0);
    }
}

// ---------------------------------------------------------------- K 回合

TurnActor actor(uint32_t id, const char* name, float speed) {
    TurnActor a;
    a.id = id;
    a.name = name;
    a.speed = speed;
    return a;
}

void groupTurn() {
    // K1：第一次出手的时间 = gaugeMax / speed（200 速 5 秒、100 速 10 秒）。
    {
        TurnQueue q;
        q.add(actor(1, "A", 200.0f));
        q.add(actor(2, "B", 100.0f));
        report("K1.first-before", static_cast<double>(q.advance(4.9f)), 0.0);
        report("K1.first-at-5s", static_cast<double>(q.advance(0.2f)), 1.0);
    }
    // K2：速度是**频率**。31 秒里 200 速出手 6 次、100 速 3 次。
    //     ⚠ 这条能抓住"按速度排个序轮流"的实现：那种实现是 2 : 2。
    {
        TurnQueue q;
        q.add(actor(1, "A", 200.0f));
        q.add(actor(2, "B", 100.0f));
        int ca = 0;
        int cb = 0;
        for (int i = 0; i < 310; ++i) {
            const uint32_t id = q.advance(0.1f);
            if (id == 0) continue;
            if (id == 1) ++ca;
            else if (id == 2) ++cb;
            q.endTurn();
        }
        report("K2.fast-count", ca, 6.0);
        report("K2.slow-count", cb, 3.0);
    }
    // K3：同速时按**加入顺序**（不是上次谁先动、也不是指针顺序）。
    {
        TurnQueue q;
        q.add(actor(1, "A", 100.0f));
        q.add(actor(2, "B", 100.0f));
        report("K3.tie-first-added", static_cast<double>(q.advance(10.0f)), 1.0);
        q.endTurn();
        report("K3.tie-second", static_cast<double>(q.advance(0.0f)), 2.0);
    }
    // K4：插队 —— gauge 顶满、立刻下一个，且**不改速度**（改速度等于永久加速）。
    {
        TurnQueue q;
        q.add(actor(1, "A", 200.0f));
        q.add(actor(2, "B", 100.0f));
        q.add(actor(3, "C", 50.0f));
        q.advance(5.0f);   // A 出手
        q.endTurn();
        const bool inserted = q.insertNow(3);
        reportFlag("K4.insert-ok", inserted, "insert");
        report("K4.insert-next", static_cast<double>(q.advance(0.0f)), 3.0);
        q.endTurn();
        const TurnActor* c = q.find(3);
        report("K4.insert-keeps-speed", c ? c->speed : -1.0, 50.0, 1e-4);
    }
    // K5：取消下一次行动（"打断"）—— 行动条归零，不是"延迟一点"。
    {
        TurnQueue q;
        q.add(actor(1, "A", 200.0f));
        q.add(actor(2, "B", 100.0f));
        q.advance(5.0f);
        q.endTurn();
        q.cancelAction(2);
        const TurnActor* b = q.find(2);
        report("K5.cancel-gauge", b ? b->gauge : -1.0, 0.0, 1e-4);
        report("K5.cancel-next-is-a", static_cast<double>(q.timeToNext() > 0.0f ? 1.0 : 0.0), 1.0);
    }
    // K6：阵亡的人不再出手，且**不从列表移除**（战斗结算还要读它）。
    {
        TurnQueue q;
        q.add(actor(1, "A", 200.0f));
        q.add(actor(2, "B", 100.0f));
        q.kill(2);
        report("K6.alive-count", static_cast<double>(q.aliveCount()), 1.0);
        report("K6.size-kept", static_cast<double>(q.size()), 2.0);
        bool bActs = false;
        for (int i = 0; i < 300; ++i) {
            const uint32_t id = q.advance(0.1f);
            if (id == 2) bActs = true;
            if (id != 0) q.endTurn();
        }
        reportFlag("K6.dead-never-acts", !bActs, "B");
    }
    // K7：预读必须与实际一致。两处 tie-break 写得不一致时，预读条会
    //     "显示 A 先动"而实际 B 先动 —— 玩家看到的就是顺序条在骗人。
    {
        TurnQueue q;
        q.add(actor(1, "A", 200.0f));
        q.add(actor(2, "B", 100.0f));
        q.add(actor(3, "C", 50.0f));
        const std::vector<uint32_t> pred = q.preview(4);
        std::vector<uint32_t> actual;
        for (int i = 0; i < 4; ++i) {
            const float t = q.timeToNext();
            if (t < 0.0f) break;
            const uint32_t id = q.advance(t);
            if (id == 0) break;
            actual.push_back(id);
            q.endTurn();
        }
        reportFlag("K7.preview-matches", pred == actual, "4turns");
        report("K7.preview-len", static_cast<double>(pred.size()), 4.0);
    }
    // K8：存档往返 —— 读档后出手顺序不能变（order 是内部字段，最容易漏存）。
    {
        TurnQueue q;
        q.add(actor(1, "A", 200.0f));
        q.add(actor(2, "B", 100.0f));
        q.add(actor(3, "C", 50.0f));
        q.advance(7.3f);   // 攒一点不均匀的 gauge
        q.endTurn();
        const std::vector<uint32_t> before = q.preview(5);
        const std::string text = q.serialize();
        TurnQueue r;
        std::string err;
        const bool ok = r.deserialize(text, &err);
        reportFlag("K8.parse-ok", ok, ok ? "ok" : "parsefail");
        reportFlag("K8.order-preserved", r.preview(5) == before, "5turns");
        report("K8.acting-preserved", static_cast<double>(r.acting()),
               static_cast<double>(q.acting()));
    }
    // K9：一次 advance 只出一个人（dt 再大也一样）。
    //     "一次吐出多个"会对已经被击杀的人做行动。
    {
        TurnQueue q;
        q.add(actor(1, "A", 200.0f));
        q.add(actor(2, "B", 100.0f));
        q.add(actor(3, "C", 50.0f));
        const uint32_t id = q.advance(100.0f);
        reportFlag("K9.only-one", id != 0 && q.turnCount() == 1, "turn1");
    }
    // K11：**带初始 gauge 不均匀**的预读一致性。
    //      ⚠ 这一条是 P2-6 上真出过的 bug：面板的行动顺序条把同一个人
    //      连打两次（`A > B > A > A`）。K7 用全 0 起始 gauge 测不出来 ——
    //      "有人带着半槽进来"（存档 / 半路加入）才会走到那条路径。
    {
        TurnQueue q;
        TurnActor fast = actor(1, "A", 150.0f);
        fast.gauge = 250.0f;          // 半槽
        TurnActor slow = actor(2, "B", 100.0f);
        q.add(fast);
        q.add(slow);
        const std::vector<uint32_t> pred = q.preview(4);
        std::vector<uint32_t> actual;
        for (int i = 0; i < 4; ++i) {
            const float t = q.timeToNext();
            if (t < 0.0f) break;
            const uint32_t id = q.advance(t);
            if (id == 0) break;
            actual.push_back(id);
            q.endTurn();
        }
        reportFlag("K11.preview-with-gauge", pred == actual, "4turns");
        // 顺序本身也要对：A(剩 750/150=5s) → B(10s) → A(6.67s vs 5s)…
        report("K11.first", pred.empty() ? 0.0 : static_cast<double>(pred[0]), 1.0);
        report("K11.second", pred.size() < 2 ? 0.0 : static_cast<double>(pred[1]), 2.0);
        report("K11.no-repeat", pred.size() >= 3 && pred[2] != pred[1] ? 1.0 : 0.0, 1.0);
    }

    // K12：`timeline()` 的**时刻**。面板顺序条读的就是这一份数据。
    //      ⚠ 钉的是 P2-6 上真出过的**显示**缺陷：`orderPreview` 原来把括号里
    //      的数取成 `find(id)->remaining()`（那是**原始队列**的剩余槽量），
    //      于是同一个人预读里出现三次就印三次 `5.00`，面板读起来像时间倒流。
    //      判据必须落在"时刻"上：只断言"顺序对"的话，括号里的数怎么错都绿。
    {
        TurnQueue q;
        TurnActor fast = actor(1, "A", 150.0f);
        fast.gauge = 250.0f;
        TurnActor slow = actor(2, "B", 100.0f);
        q.add(fast);
        q.add(slow);
        const std::vector<std::pair<uint32_t, float>> tl = q.timeline(4);
        reportFlag("K12.size", tl.size() == 4, "4rows");
        bool mono = true;
        for (std::size_t i = 1; i < tl.size(); ++i) {
            if (tl[i].second < tl[i - 1].second - 1.0e-4f) mono = false;
        }
        reportFlag("K12.monotonic", mono, "nondecreasing");
        const std::vector<uint32_t> pv = q.preview(4);
        bool same = pv.size() == tl.size();
        if (same) {
            for (std::size_t i = 0; i < tl.size(); ++i) {
                if (tl[i].first != pv[i]) same = false;
            }
        }
        reportFlag("K12.matches-preview", same, "same-ids");
        // 手算：A 剩 (1000-250)/150 = 5.00；B 1000/100 = 10.00；
        //       第三位 A 再 250/150 = 1.67 → 11.67；第四位 A 再 1000/150 → 18.33。
        report("K12.t1", tl.size() > 0 ? static_cast<double>(tl[0].second) : -1.0, 5.0, 0.02);
        report("K12.t2", tl.size() > 1 ? static_cast<double>(tl[1].second) : -1.0, 10.0, 0.02);
        report("K12.t3", tl.size() > 2 ? static_cast<double>(tl[2].second) : -1.0, 11.67, 0.02);
        report("K12.t4", tl.size() > 3 ? static_cast<double>(tl[3].second) : -1.0, 18.33, 0.02);
    }

    // K13：**同一个人连着占两个位置是正确行为**，不是"预读重复"。
    //      150 速对 100 速（A 另带 250 初始槽）：8 次出手里 A 出 5 次、B 出 3 次
    //      （5:3，逼近 1.5 的速度比），且第 3、4 位都是 A。
    //      ⚠ 这条是写给"看着像 bug 就改掉"的人的：把连出"修"掉等于把 1.5 倍
    //      速抹平（序列会退回 A B A B…，比 1:1），与速度完全不符。
    //      手算时刻 5.0 / 10.0 / 11.67 / 18.33 / 20.0 / 25.0 / 30.0 / 31.67。
    {
        TurnQueue q;
        TurnActor fast = actor(1, "A", 150.0f);
        fast.gauge = 250.0f;
        TurnActor slow = actor(2, "B", 100.0f);
        q.add(fast);
        q.add(slow);
        const std::vector<uint32_t> pv = q.preview(8);
        int na = 0, nb = 0;
        for (std::size_t i = 0; i < pv.size(); ++i) {
            if (pv[i] == 1) ++na;
            else if (pv[i] == 2) ++nb;
        }
        report("K13.count-A", static_cast<double>(na), 5.0);
        report("K13.count-B", static_cast<double>(nb), 3.0);
        reportFlag("K13.consecutive-is-correct", pv.size() >= 4 && pv[2] == 1 && pv[3] == 1,
                   "pos3n4=A");
        const std::vector<std::pair<uint32_t, float>> tl8 = q.timeline(8);
        report("K13.t8", tl8.size() == 8 ? static_cast<double>(tl8[7].second) : -1.0, 31.67, 0.05);
    }

    // K10：空队列 / 全灭 —— 不崩、返回"没人"。
    {
        TurnQueue q;
        report("K10.empty-advance", static_cast<double>(q.advance(1.0f)), 0.0);
        report("K10.empty-time", static_cast<double>(q.timeToNext()), -1.0);
        q.add(actor(1, "A", 100.0f));
        q.kill(1);
        report("K10.all-dead-advance", static_cast<double>(q.advance(1.0f)), 0.0);
    }
}

// ---------------------------------------------------------------- L 持久化

// 组件的形状（`ecs::GameplayComponent` 的数据部分）。这里刻意**不复用**
// 那个类型：`engine/gameplay` 不依赖 ecs（那是 `--selftest gameplay` 能
// 在没有场景的情况下跑完的前提）。判据测的是**等价的不变量**：
// "白值 + 状态列表 → 视图 → 改 → 写回" 这条往返不丢东西。
void groupPersist() {
    std::vector<AttrState> attrs = defaultAttributes(100.0f, 20.0f, 50.0f);
    std::vector<StatusState> statuses;

    // L1：伤害扣血 → storeBases → 重新 build，hp 必须真的变成 70。
    //     漏了 storeBases 的症状是"扣了血，下一帧又满血"。
    {
        AttributeSet view;
        buildAttributes(attrs, statuses, view);
        const float taken = view.consume("hp", 30.0f);
        report("L1.consume-taken", taken, 30.0, 1e-4);
        storeBases(view, attrs);
        buildAttributes(attrs, statuses, view);
        report("L1.hp-after-roundtrip", view.get("hp"), 70.0, 1e-4);
    }
    // L2：状态快照往返。3 层毒经 `toStatusState → toStatusDef → restore`
    //     之后仍然是 3 层（走 `add()` 会变成 1 层 —— 这就是 L2 的判据）。
    {
        StatusDef d;
        d.id = "poison";
        d.turns = 3;
        d.maxStacks = 5;
        d.rule = StackRule::Stack;
        d.dotPerTurn = 4.0f;
        NamedMod nm;
        nm.attribute = "attack";
        nm.mod = mod(ModOp::Flat, -3.0f);
        d.mods.push_back(nm);

        StatusSet live;
        live.add(d);
        live.add(d);
        live.add(d);
        const StatusState snap = toStatusState(d, live.turnsLeft("poison"), live.stacks("poison"));
        report("L2.snapshot-stacks", static_cast<double>(snap.stacks), 3.0);

        StatusSet restored;
        restored.restore(toStatusDef(snap), snap.turnsLeft, snap.stacks);
        report("L2.restore-stacks", static_cast<double>(restored.stacks("poison")), 3.0);
        report("L2.restore-turns", static_cast<double>(restored.turnsLeft("poison")), 3.0);
        // 对照：同一份快照走 `add()` 只剩 1 层 —— 证明 L2 不是"两条路都对"。
        StatusSet viaAdd;
        viaAdd.add(toStatusDef(snap));
        report("L2.add-gives-one-stack", static_cast<double>(viaAdd.stacks("poison")), 1.0);
    }
    // L3：坏档里的 turns <= 0 状态必须被丢（否则是个永不 tick 的幽灵）。
    {
        StatusState bad;
        bad.id = "bad";
        bad.turnsLeft = 0;
        StatusSet set;
        const bool ok = set.restore(toStatusDef(bad), 0, 1);
        reportFlag("L3.reject-zero-turns", !ok && set.size() == 0, "turns");
    }
    // L4：状态挂上来的修正器经过一次完整往返仍然生效，且**不被烘进 base**。
    //     100(白值) - 2×3(毒 3 层) = 94。
    {
        std::vector<AttrState> a2 = defaultAttributes(100.0f, 100.0f, 0.0f);
        std::vector<StatusState> s2;
        StatusState st;
        st.id = "poison";
        st.turnsLeft = 3;
        st.stacks = 3;
        st.maxStacks = 5;
        st.rule = StackRule::Stack;
        st.dotPerTurn = 0.0f;   // 这个用例只测属性
        StatusModState m;
        m.attribute = "attack";
        m.op = ModOp::Flat;
        m.value = -2.0f;
        st.mods.push_back(m);
        s2.push_back(st);

        AttributeSet view;
        buildAttributes(a2, s2, view);
        report("L4.status-mod-applied", view.get("attack"), 94.0, 1e-4);
        // 往返一次（写回 base 后再 build）—— 状态加成**不能**被写成 base，
        // 否则第二次 build 会把它算两遍（94 → 88）。
        storeBases(view, a2);
        buildAttributes(a2, s2, view);
        report("L4.status-mod-not-baked", view.get("attack"), 94.0, 1e-4);
    }
    // L5：空名字的属性被忽略（序列化里漏了 `n` 的条目不能建出一个无名属性）。
    {
        std::vector<AttrState> a3;
        a3.push_back(AttrState{"", 999.0f, 0.0f, 1.0e9f});
        a3.push_back(AttrState{"hp", 10.0f, 0.0f, 10.0f});
        std::vector<StatusState> s3;
        AttributeSet view;
        buildAttributes(a3, s3, view);
        report("L5.empty-name-skipped", static_cast<double>(view.size()), 1.0);
        report("L5.hp-kept", view.get("hp"), 10.0, 1e-4);
    }
}

// ---------------------------------------------------------------- M 牌堆
//
// P3-3。这一组钉的是"牌堆的五种做错方式"（见 CardZones.h 文件头）：
//   M1  身份用 uid 不用下标（同 defId 两张牌 uid 不同）
//   M2  抽牌**队首**出牌（顺序契约；换成 back() 会让"下一张"不可预读）
//   M3  牌库空 → 弃牌洗回（reshuffle 计数 + 那一张真的抽到了）
//   M4  抽干不崩、返回 0
//   M5  手牌满就**停止抽**（剩下的留在牌库，**不烧牌**）
//   M6  打出的牌进 Limbo 而**不是**弃牌堆（结算中读 discard 不能看到它）
//   M7  resolve 之后才进弃牌堆
//   M8  exhaust 卡进消耗区
//   M9  unplayable（诅咒）打不出去且留在手牌
//   M10 同区重排的"差一位"（move 之后 uidAt(to,pos) == uid）
//   M11 跨区插到指定位置
//   M12 discardHand
//   M13 存档往返（含 Limbo 里的牌与顺序）
//   M14 读档重算 nextUid（否则新造的牌与老牌撞 uid）
//   M15 坏档拒绝（缺 header / 未知区名 / uid=0）
//   M16-17 洗牌确定性 + 只改顺序不改集合
//   M18 clear 不清定义表
//   M19 erase 真的删掉
//   M20 升级改费用
//   M21 配表枚举"认不出就不猜"
//   M22 sweepLimbo 按 def 分派
//   M23 移动不改变总张数
//   M24 uid 永不复用
void groupCards() {
    // 一张最小的定义表。判据只关心"区转移"，不关心效果数值。
    const auto defs = []() {
        std::vector<CardDef> v;
        CardDef strike;
        strike.id = "strike";
        strike.name = "Strike";
        strike.cost = 1;
        v.push_back(strike);

        CardDef bash;
        bash.id = "bash";
        bash.name = "Bash";
        bash.cost = 2;
        bash.upgradedCost = 1;
        bash.exhaust = true;
        v.push_back(bash);

        CardDef curse;
        curse.id = "curse";
        curse.kind = CardKind::Curse;
        curse.unplayable = true;
        curse.cost = 0;
        v.push_back(curse);
        return v;
    };

    // M1：身份是 uid 不是下标。
    {
        CardZones z;
        z.setDefs(defs());
        const uint32_t a = z.create("strike", 1);
        const uint32_t b = z.create("strike", 1);
        reportFlag("M1.distinct-uids", a != 0 && b != 0 && a != b,
                   "a=" + std::to_string(a) + ",b=" + std::to_string(b));
        report("M1.first-uid", static_cast<double>(a), 1.0);
        report("M1.second-uid", static_cast<double>(b), 2.0);
        report("M1.draw-count", static_cast<double>(z.count(CardPile::Draw)), 2.0);
    }

    // M2：抽牌从队首出，顺序可预读。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 1);
        z.create("bash", 1);
        z.create("curse", 1);
        const uint32_t head = z.uidAt(CardPile::Draw, 0);
        Random rng;
        const int got = z.draw(2, rng);
        report("M2.drawn", static_cast<double>(got), 2.0);
        report("M2.hand", static_cast<double>(z.count(CardPile::Hand)), 2.0);
        reportFlag("M2.fifo", z.uidAt(CardPile::Hand, 0) == head,
                   "head=" + std::to_string(head));
    }

    // M3/M4：洗回 + 抽干。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 3);
        Random rng;
        z.draw(3, rng);
        z.discardHand();
        report("M3.reshuffle-before", static_cast<double>(z.reshuffles()), 0.0);
        const int got = z.draw(1, rng);
        report("M3.drawn", static_cast<double>(got), 1.0);
        report("M3.reshuffled", static_cast<double>(z.reshuffles()), 1.0);
        reportFlag("M3.flag", z.lastDrawReshuffled());
        report("M3.discard-emptied", static_cast<double>(z.count(CardPile::Discard)), 0.0);
        report("M3.draw-left", static_cast<double>(z.count(CardPile::Draw)), 2.0);

        const int rest = z.draw(99, rng);
        report("M4.drawn-rest", static_cast<double>(rest), 2.0);
        report("M4.emptied", static_cast<double>(z.draw(1, rng)), 0.0);
        reportFlag("M4.no-reshuffle-flag", !z.lastDrawReshuffled());
    }

    // M5：手牌满 → 停止抽，剩下的**留在牌库**（不烧牌）。
    {
        CardZones z;
        z.setDefs(defs());
        z.setHandLimit(2);
        z.create("strike", 5);
        Random rng;
        const int got = z.draw(5, rng);
        report("M5.got", static_cast<double>(got), 2.0);
        report("M5.hand", static_cast<double>(z.count(CardPile::Hand)), 2.0);
        report("M5.left-in-draw", static_cast<double>(z.count(CardPile::Draw)), 3.0);
        report("M5.nothing-burned", static_cast<double>(z.count(CardPile::Discard)), 0.0);
    }

    // M6/M7/M8：打出去进 Limbo，结算完才落地。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 1);
        z.create("bash", 1);
        Random rng;
        z.draw(2, rng);
        const uint32_t strikeUid = z.uidAt(CardPile::Hand, 0);
        const uint32_t bashUid = z.uidAt(CardPile::Hand, 1);

        reportFlag("M6.play-ok", z.play(strikeUid));
        report("M6.hand-after", static_cast<double>(z.count(CardPile::Hand)), 1.0);
        report("M6.limbo", static_cast<double>(z.count(CardPile::Limbo)), 1.0);
        // ⚠ 整组的核心：打出的牌**还没**进弃牌堆。
        report("M6.discard-not-yet", static_cast<double>(z.count(CardPile::Discard)), 0.0);
        report("M6.plays", static_cast<double>(z.plays()), 1.0);

        reportFlag("M7.resolve-ok", z.resolve(strikeUid));
        report("M7.limbo-after", static_cast<double>(z.count(CardPile::Limbo)), 0.0);
        report("M7.discard", static_cast<double>(z.count(CardPile::Discard)), 1.0);

        reportFlag("M8.play-bash", z.play(bashUid));
        reportFlag("M8.resolve-bash", z.resolve(bashUid));
        report("M8.exhaust", static_cast<double>(z.count(CardPile::Exhaust)), 1.0);
        report("M8.discard-unchanged", static_cast<double>(z.count(CardPile::Discard)), 1.0);
    }

    // M9：诅咒打不出去，且**留在手牌**（不能"打失败但牌没了"）。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("curse", 1);
        Random rng;
        z.draw(1, rng);
        const uint32_t c = z.uidAt(CardPile::Hand, 0);
        reportFlag("M9.play-rejected", !z.play(c));
        report("M9.stay-hand", static_cast<double>(z.count(CardPile::Hand)), 1.0);
        report("M9.plays-unchanged", static_cast<double>(z.plays()), 0.0);
    }

    // M10：同区重排的"差一位"。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 4);          // uid 1,2,3,4
        Random rng;
        z.draw(4, rng);                 // hand = [1,2,3,4]
        reportFlag("M10.move-tail", z.move(1, CardPile::Hand, 3));
        report("M10.at0", static_cast<double>(z.uidAt(CardPile::Hand, 0)), 2.0);
        report("M10.at3", static_cast<double>(z.uidAt(CardPile::Hand, 3)), 1.0);
        report("M10.size", static_cast<double>(z.count(CardPile::Hand)), 4.0);
        // 往回挪：把第 3 位挪到第 0 位。
        reportFlag("M10.move-head", z.move(1, CardPile::Hand, 0));
        report("M10.back-at0", static_cast<double>(z.uidAt(CardPile::Hand, 0)), 1.0);
    }

    // M11：跨区插到指定位置。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 3);
        Random rng;
        z.draw(3, rng);
        const uint32_t last = z.uidAt(CardPile::Hand, 2);
        reportFlag("M11.ok", z.move(last, CardPile::Discard, 0));
        report("M11.discard-at0", static_cast<double>(z.uidAt(CardPile::Discard, 0)),
               static_cast<double>(last));
        report("M11.hand-left", static_cast<double>(z.count(CardPile::Hand)), 2.0);
    }

    // M12：弃手牌。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 3);
        Random rng;
        z.draw(3, rng);
        report("M12.discarded", static_cast<double>(z.discardHand()), 3.0);
        report("M12.hand-empty", static_cast<double>(z.count(CardPile::Hand)), 0.0);
    }

    // M13：存档往返（含 Limbo）+ 顺序。
    {
        CardZones a;
        a.setDefs(defs());
        a.create("strike", 2);
        a.create("bash", 1);
        Random rng;
        a.draw(3, rng);
        const uint32_t played = a.uidAt(CardPile::Hand, 0);
        a.play(played);                 // 留一张在 Limbo

        const std::string blob = a.serialize();
        CardZones b;
        b.setDefs(defs());
        std::string err;
        reportFlag("M13.load-ok", b.deserialize(blob, &err), err);
        report("M13.hand", static_cast<double>(b.count(CardPile::Hand)),
               static_cast<double>(a.count(CardPile::Hand)));
        report("M13.limbo", static_cast<double>(b.count(CardPile::Limbo)), 1.0);
        reportFlag("M13.limbo-uid", b.uidAt(CardPile::Limbo, 0) == played,
                   "uid=" + std::to_string(played));
        report("M13.order", static_cast<double>(b.uidAt(CardPile::Hand, 0)),
               static_cast<double>(a.uidAt(CardPile::Hand, 0)));
        report("M13.plays", static_cast<double>(b.plays()), 1.0);
    }

    // M14：读档重算 nextUid —— 否则新造的牌与老牌撞号（症状是"打出这张、
    // 那张消失了"）。
    {
        CardZones z;
        z.setDefs(defs());
        const std::string stale =
            "handLimit=10 nextUid=1 reshuffles=0 plays=0\nhand strike 7 0\n";
        reportFlag("M14.load-ok", z.deserialize(stale));
        const uint32_t n = z.create("strike");
        report("M14.no-collision", static_cast<double>(n), 8.0);
    }

    // M15：坏档拒绝。
    {
        CardZones z;
        z.setDefs(defs());
        reportFlag("M15.no-header", !z.deserialize("hand strike 1 0\n"));
        reportFlag("M15.bad-pile",
                   !z.deserialize("handLimit=10 nextUid=1 reshuffles=0 plays=0\ndeck strike 1 0\n"));
        reportFlag("M15.uid-zero",
                   !z.deserialize("handLimit=10 nextUid=1 reshuffles=0 plays=0\nhand strike 0 0\n"));
    }

    // M16/M17：洗牌确定性 + 只改顺序不改集合。
    {
        CardZones x;
        CardZones y;
        x.setDefs(defs());
        y.setDefs(defs());
        x.create("strike", 10);
        y.create("strike", 10);
        Random r1;
        Random r2;
        r1.seed(4242u, 7u);
        r2.seed(4242u, 7u);
        x.shuffleDraw(r1);
        y.shuffleDraw(r2);
        int same = 1;
        for (int i = 0; i < 10; ++i) {
            if (x.uidAt(CardPile::Draw, i) != y.uidAt(CardPile::Draw, i)) same = 0;
        }
        report("M16.deterministic", static_cast<double>(same), 1.0);

        // 集合不变：uid 1..10 各出现一次（按位图统计）。
        int seen = 0;
        for (int i = 0; i < 10; ++i) {
            const uint32_t u = x.uidAt(CardPile::Draw, i);
            if (u >= 1 && u <= 10) seen |= (1 << (u - 1));
        }
        report("M17.permutation", static_cast<double>(seen), 1023.0);
        report("M17.total", static_cast<double>(x.total()), 10.0);
    }

    // M18：clear 不清定义表（读档 = clear → setDefs → deserialize，
    // 漏了中间一步整副牌就变未知卡，而未知卡**不报错**）。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 2);
        z.clear();
        reportFlag("M18.defs-kept", z.def("strike") != nullptr);
        report("M18.cards-cleared", static_cast<double>(z.total()), 0.0);
        report("M18.nextuid-reset", static_cast<double>(z.create("strike")), 1.0);
    }

    // M19：erase。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 3);
        const uint32_t u = z.uidAt(CardPile::Draw, 1);
        reportFlag("M19.erase-ok", z.erase(u));
        report("M19.count", static_cast<double>(z.count(CardPile::Draw)), 2.0);
        reportFlag("M19.gone", !z.contains(u));
        reportFlag("M19.erase-missing", !z.erase(9999u));
    }

    // M20：升级改费用。
    {
        CardZones z;
        z.setDefs(defs());
        CardInstance c;
        c.defId = "bash";
        c.upgrade = 0;
        report("M20.base-cost", static_cast<double>(z.costOf(c)), 2.0);
        c.upgrade = 1;
        report("M20.upgraded-cost", static_cast<double>(z.costOf(c)), 1.0);
        CardInstance s;
        s.defId = "strike";
        s.upgrade = 1;
        report("M20.no-upgrade-cost", static_cast<double>(z.costOf(s)), 1.0);
    }

    // M21：配表枚举"认不出就不猜"（猜成最像的会让一张写错的表静默生效）。
    {
        CardKind k = CardKind::Curse;
        reportFlag("M21.reject-typo", !parseCardKind("skil", k));
        report("M21.unchanged", static_cast<double>(static_cast<int>(k)),
               static_cast<double>(static_cast<int>(CardKind::Curse)));
        reportFlag("M21.accept", parseCardKind("skill", k) && k == CardKind::Skill);
        CardPile p = CardPile::Hand;
        reportFlag("M21.pile-reject", !parseCardPile("deack", p) && p == CardPile::Hand);
    }

    // M22：sweepLimbo 按 def 分派（异常恢复不能把消耗卡塞进弃牌堆）。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 2);
        z.create("bash", 1);
        Random rng;
        z.draw(3, rng);
        z.play(z.uidAt(CardPile::Hand, 0));
        z.play(z.uidAt(CardPile::Hand, 1));
        report("M22.swept", static_cast<double>(z.sweepLimbo()), 2.0);
        report("M22.limbo", static_cast<double>(z.count(CardPile::Limbo)), 0.0);
        report("M22.discard", static_cast<double>(z.count(CardPile::Discard)), 1.0);
        report("M22.exhaust", static_cast<double>(z.count(CardPile::Exhaust)), 1.0);
    }

    // M23：移动不改变总张数（"牌凭空多出来/少掉"是这套东西最贵的 bug）。
    {
        CardZones z;
        z.setDefs(defs());
        z.create("strike", 4);
        Random rng;
        z.draw(2, rng);
        const double before = static_cast<double>(z.total());
        z.move(z.uidAt(CardPile::Hand, 0), CardPile::Discard, 0);
        z.play(z.uidAt(CardPile::Hand, 0));
        z.resolve(z.uidAt(CardPile::Limbo, 0));
        report("M23.total-stable", static_cast<double>(z.total()), before);
    }

    // M24：uid 永不复用（复用会让"存档里的那张牌"读到另一张牌）。
    {
        CardZones z;
        z.setDefs(defs());
        const uint32_t a = z.create("strike");
        z.erase(a);
        const uint32_t b = z.create("strike");
        reportFlag("M24.no-reuse", b != a && b == 2u,
                   "a=" + std::to_string(a) + ",b=" + std::to_string(b));
    }
}

}  // namespace

// ---- P3-4：伤害数字层（O 组）----
//
// 这一组守的是"数字会怎么看起来不对"：
//   · 合并窗口写错 → 三条 "37" 互相盖住（或者一个 "37 x99" 永生）；
//   · `t` 不是归一化 → 数字活着不动 / 瞬间闪没；
//   · 漂移不确定 → 像素判据永远 flaky；
//   · 背后/屏幕外没丢 → 屏幕边上永远贴着一个数字。
void groupDamageNumbers() {
    using namespace gameplay;

    // O1：同点同类在同一瞬间 → 合并成一条，数值相加、连击数记住。
    {
        DamageNumberLayer L;
        const bool a = L.spawn(PopupSpawn{glm::vec3(0.0f), 12.0f,
                                          PopupKind::Damage, true});
        const bool b = L.spawn(PopupSpawn{glm::vec3(0.3f, 0.0f, 0.0f), 12.0f,
                                          PopupKind::Damage, true});
        reportFlag("O1.merge-one", a && !b && L.size() == 1,
                   "a=" + std::to_string(a) + ",b=" + std::to_string(b) +
                       ",n=" + std::to_string(L.size()));
        report("O1.amount", static_cast<double>(L.items()[0].amount), 24.0);
        report("O1.combo", static_cast<double>(L.items()[0].combo), 2.0);
        report("O1.merged-count", static_cast<double>(L.mergedCount()), 1.0);
    }

    // O2：同类但**超出合并半径** → 两条（两个敌人站得远，各自弹自己的）。
    {
        DamageNumberLayer L;
        L.spawn(PopupSpawn{glm::vec3(0.0f), 12.0f, PopupKind::Damage, true});
        L.spawn(PopupSpawn{glm::vec3(5.0f, 0.0f, 0.0f), 12.0f,
                           PopupKind::Damage, true});
        report("O2.far-no-merge", static_cast<double>(L.size()), 2.0);
    }

    // O3：**不同种类不合并**（治疗和伤害在同一格上必须分开读）。
    {
        DamageNumberLayer L;
        L.spawn(PopupSpawn{glm::vec3(0.0f), 12.0f, PopupKind::Damage, true});
        L.spawn(PopupSpawn{glm::vec3(0.0f), 4.0f, PopupKind::Heal, true});
        report("O3.kind-split", static_cast<double>(L.size()), 2.0);
    }

    // O4：合并窗口之外 → 新起一条（"隔了一秒又挨一下"不该算连击）。
    {
        DamageNumberLayer L;
        L.spawn(PopupSpawn{glm::vec3(0.0f), 12.0f, PopupKind::Damage, true});
        L.tick(DamageNumberLayer::kMergeWindow + 0.05f);
        L.spawn(PopupSpawn{glm::vec3(0.0f), 12.0f, PopupKind::Damage, true});
        report("O4.window-out", static_cast<double>(L.size()), 2.0);
    }

    // O5：寿命到了会被清掉；`dt <= 0` **不推进**（暂停时数字不该偷偷变老）。
    {
        DamageNumberLayer L;
        L.spawn(PopupSpawn{glm::vec3(0.0f), 12.0f, PopupKind::Damage, false});
        const float life = popupStyle(PopupKind::Damage).life;
        L.tick(0.0f);
        const std::size_t frozen = L.size();
        L.tick(life * 0.5f);
        const std::size_t mid = L.size();
        L.tick(life * 0.6f);
        report("O5.dt0-frozen", static_cast<double>(frozen), 1.0);
        report("O5.alive-mid", static_cast<double>(mid), 1.0);
        report("O5.expired", static_cast<double>(L.size()), 0.0);
    }

    // O6：池满替换**最老的**（最新那条必须在）。
    {
        DamageNumberLayer L;
        for (int i = 1; i <= 40; ++i)
            L.spawn(PopupSpawn{glm::vec3(static_cast<float>(i) * 10.0f, 0.0f,
                                         0.0f),
                               static_cast<float>(i), PopupKind::Damage, false});
        bool newest = false;
        for (const DamageNumber& n : L.items())
            if (std::fabs(n.amount - 40.0f) < 1e-3f) newest = true;
        report("O6.capped",
               static_cast<double>(L.size()),
               static_cast<double>(DamageNumberLayer::kMax));
        reportFlag("O6.keeps-newest", newest, "");
        report("O6.dropped", static_cast<double>(L.droppedCount()), 8.0);
    }

    // O7：文本格式（判据看**文本**，不是"有个字符串"）。
    {
        DamageNumber n;
        n.amount = 37.0f;
        n.kind = PopupKind::Damage;
        n.combo = 3;
        const std::string dmg = DamageNumberLayer::formatText(n);

        DamageNumber h;
        h.amount = 45.0f;
        h.kind = PopupKind::Heal;
        const std::string heal = DamageNumberLayer::formatText(h);

        DamageNumber s2;
        s2.amount = 8.0f;
        s2.kind = PopupKind::Shield;
        const std::string sh = DamageNumberLayer::formatText(s2);

        DamageNumber miss;
        miss.kind = PopupKind::Miss;
        const std::string ms = DamageNumberLayer::formatText(miss);

        // 0.4 → "1"（显示成 "0" 会让人以为伤害被吞了）
        DamageNumber tiny;
        tiny.amount = 0.4f;
        tiny.kind = PopupKind::Damage;
        const std::string tn = DamageNumberLayer::formatText(tiny);

        reportFlag("O7.text-damage", dmg == "37 x3", "text=" + dmg);
        reportFlag("O7.text-heal", heal == "+45", "text=" + heal);
        reportFlag("O7.text-shield", sh == "[8]", "text=" + sh);
        reportFlag("O7.text-miss", ms == "MISS", "text=" + ms);
        reportFlag("O7.text-round-up", tn == "1", "text=" + tn);
    }

    // O8：漂移确定（同一序列跑两遍，逐字段相同）。
    {
        const auto run = []() {
            DamageNumberLayer L;
            for (int i = 0; i < 5; ++i)
                L.spawn(PopupSpawn{glm::vec3(static_cast<float>(i) * 10.0f,
                                             0.0f, 0.0f),
                                   10.0f, PopupKind::Damage, false});
            std::vector<double> v;
            for (const DamageNumber& n : L.items()) v.push_back(n.driftX);
            return v;
        };
        const std::vector<double> a = run();
        const std::vector<double> b = run();
        reportFlag("O8.drift-deterministic", a == b && a.size() == 5,
                   "n=" + std::to_string(a.size()));
    }

    // O9：投影 —— 背后 / 屏幕外**丢**，屏幕内的点落在正确象限。
    {
        const glm::mat4 I(1.0f);
        glm::vec2 p;
        const bool center = projectToScreen(I, glm::vec3(0.0f), 800, 600, &p);
        // "背后"的样子是 `clip.w <= 0`（透视投影里相机后面的点 w 为负）。
        // ⚠ 不能用单位阵测它：单位阵的 w 恒为 1，任何点都"在相机前面"。
        //   零矩阵的 w 恒为 0，正是这一支。
        const bool behind = projectToScreen(glm::mat4(0.0f), glm::vec3(0.0f), 800,
                                            600, &p);
        const bool outside = projectToScreen(I, glm::vec3(3.0f), 800, 600, &p);
        reportFlag("O9.center", center && std::fabs(p.x - 400.0f) < 0.01f &&
                                    std::fabs(p.y - 300.0f) < 0.01f,
                   "x=" + std::to_string(p.x) + ",y=" + std::to_string(p.y));
        reportFlag("O9.behind-dropped", !behind, "");
        reportFlag("O9.outside-dropped", !outside, "");
    }

    // O10：布局曲线的两个端点 + 屏幕外不产出 layout。
    {
        DamageNumberLayer L;
        L.spawn(PopupSpawn{glm::vec3(0.0f), 12.0f, PopupKind::Damage, false});
        std::vector<PopupLayout> out;
        const glm::mat4 I(1.0f);

        L.layout(I, 800, 600, 1.0f, out);
        // 出生：不透明、在上浮起点附近。容差是**漂移的幅度**
        // （`driftRef * 0.4`，最大 ±5.6 参考像素），不是随手给的 1.0 ——
        // 给 1.0 会让"漂移正常生效"看起来像失败。
        const bool born = out.size() == 1 && out[0].alpha > 0.99f &&
                          std::fabs(out[0].baselineY - 300.0f) < 8.0f;
        const float bornSize = out.empty() ? 0.0f : out[0].sizePx;

        // 走到 80% 寿命：已经淡出到 20% 上下，而且浮上去了
        L.tick(popupStyle(PopupKind::Damage).life * 0.85f);
        L.layout(I, 800, 600, 1.0f, out);
        const bool faded = out.size() == 1 && out[0].alpha < 0.55f &&
                           out[0].baselineY < 300.0f;
        // 出生时比稳态**大**（"弹一下"），稳态回到 style.size
        const bool popped = bornSize > popupStyle(PopupKind::Damage).size;

        reportFlag("O10.born", born, "");
        reportFlag("O10.faded-and-rose", faded, "");
        reportFlag("O10.pop-scale", popped, "born=" + std::to_string(bornSize));

        // 屏幕外：同一个数字挪到相机背后 → 一条 layout 都不产出
        DamageNumberLayer L2;
        // ⚠ 往 **x** 方向挪出去（ndc.x = 9 → 屏幕外）。挪 z 是没用的：
        //   单位阵下 z 不影响屏幕坐标，数字照样在画面正中。
        L2.spawn(PopupSpawn{glm::vec3(9.0f, 0.0f, 0.0f), 12.0f,
                            PopupKind::Damage, false});
        L2.layout(I, 800, 600, 1.0f, out);
        report("O10.off-screen-none", static_cast<double>(out.size()), 0.0);
    }
}

// ---- P3-4：卡牌表现模板（P 组）----
void groupCardFx() {
    using namespace gameplay;

    // P1：表的下标就是枚举值（错位会让"出牌"播成"消耗"的表现）。
    {
        bool ok = true;
        for (int i = 0; i < static_cast<int>(CardFxKind::Count); ++i) {
            const CardFxPreset& p = cardFxPreset(static_cast<CardFxKind>(i));
            if (p.height <= 0.0f && i <= static_cast<int>(CardFxKind::Buff))
                ok = false;
        }
        reportFlag("P1.heights", ok, "");

        // 越界 → 回落到 Play（不抛、不留空）
        const CardFxPreset& bad = cardFxPreset(static_cast<CardFxKind>(99));
        reportFlag("P1.oob-fallback", bad.effect && bad.effect[0] != '\0', "");
    }

    // P2：抽/弃/消耗**不在世界里出东西**（牌堆变化是 UI 的事）。
    {
        bool clean = true;
        for (int i = static_cast<int>(CardFxKind::Draw);
             i < static_cast<int>(CardFxKind::Count); ++i) {
            const CardFxPreset& p = cardFxPreset(static_cast<CardFxKind>(i));
            if (p.effect[0] != '\0' || p.popup != PopupKind::Count) clean = false;
        }
        reportFlag("P2.pile-moves-silent", clean, "");
    }

    // P3：出牌落在**持牌者**身上；伤害落在**目标**身上。
    {
        const CardFxPreset& play = cardFxPreset(CardFxKind::Play);
        const CardFxPreset& hit = cardFxPreset(CardFxKind::Hit);
        const CardFxPreset& crit = cardFxPreset(CardFxKind::Crit);
        reportFlag("P3.play-on-owner", !play.atTarget, "");
        reportFlag("P3.hit-on-target", hit.atTarget, "");
        // 暴击的飘字必须换种类（同样式的话"暴击"在画面上看不出来）
        reportFlag("P3.crit-style",
                   crit.popup == PopupKind::Crit && hit.popup == PopupKind::Damage,
                   "");
    }

    // P4：每一种"会弹字"的动作，飘字种类都必须有**非 Count** 的样式，
    //     且样式表的每一项都真的有寿命（0 寿命 = 弹出来立刻消失）。
    {
        bool alive = true;
        for (int i = 0; i < static_cast<int>(PopupKind::Count); ++i) {
            const PopupStyle& s = popupStyle(static_cast<PopupKind>(i));
            if (!(s.life > 0.0f) || !(s.size > 0.0f)) alive = false;
        }
        reportFlag("P4.styles-alive", alive,
                   "kinds=" + std::to_string(static_cast<int>(PopupKind::Count)));
    }
}

int runGameplaySelfTest() {
    g_pass = 0;
    g_fail = 0;

    VK_LOG_INFO("== selftest: gameplay (P2 attribute / status / damage / random / "
                "turn / P3 cards / P3 table) ==");
    groupAttribute();
    groupStatus();
    groupDamage();
    groupRandom();
    groupTurn();
    groupPersist();
    groupCards();
    groupDamageNumbers();   // P3-4：O 组
    groupCardFx();          // P3-4：P 组
    // N 组住在 `CardTable.cpp`（配表解析是纯函数，不碰场景，与 M 组分开
    // 才不会让"M 组一红"同时指着"牌堆错了"和"CSV 解析错了"两件事）。
    // ⚠ 通过条数也要并回来 —— 总数与 `GAMEPLAY-TOTAL pass=` 必须一致。
    int nPass = 0;
    g_fail += runCardTableSelfTest(&nPass);
    g_pass += nPass;

    VK_LOG_INFO("GAMEPLAY-TOTAL pass=%d fail=%d", g_pass, g_fail);
    return g_fail;
}

}  // namespace gameplay
