#pragma once
// ============================================================
// gameplay/Stats —— 属性 / 状态 / 伤害（P2 玩法状态层的地基）
//
// 三层东西放在一个编译单元里，因为它们**是同一条计算链**：
//
//     状态(StatusSet) ──改写──> 属性(AttributeSet) ──喂给──> 伤害(Damage)
//
// 分开成三个文件会让"状态改了属性、属性影响伤害"这条因果在三处各写一遍
// 注释，而真正的坑全在**交界处**（见下面四条）。所以头文件只切了语义，
// 实现同源。
//
// ------------------------------------------------------------
// 四条不能想当然的语义
// ------------------------------------------------------------
// 1) **属性的求值顺序是固定的：Override → Flat → AddPct → MulPct → clamp**。
//    换顺序结果就不同（`(10+5)*1.5` ≠ `10*1.5+5`），而且两边都"看起来很
//    合理"。顺序必须写死在一处、被用例钉住，作者只能按这个顺序理解。
//    · Override：**最后加的那条赢**（后加的 buff 覆盖先前的变身）
//    · Flat    ：求和
//    · AddPct  ：求和后 `×(1 + sum/100)`（100 = +100%）
//    · MulPct  ：连乘（1.2 = ×1.2，两个 1.2 = ×1.44 —— 与 AddPct 的
//                100+100=+200% 完全不同，这是最容易配错的一处）
//
// 2) **clamp 只在最后做一次**。中途夹会丢掉"负值 buff 之后又被拉回来"的
//    情况（`HP = 1` 时挂一个 -50 的 debuff 再挂 +200 的 buff，中途夹到 1
//    会变成 201 而不是 51）。下限默认 0 由调用方按属性给（HP 有下限，
//    攻击力没有）。
//
// 3) **状态对属性的贡献走"重建"而不是"增量"**。每次状态变化（加/叠/到期）
//    都把 `source` 以 `status:` 开头的 modifier 全摘掉再重加。
//    增量法的问题是**叠层加/减算错一次就永远错**（摘的时候找不到该摘几条），
//    而重建每次都是从当前状态列表算出来的，天然自洽。代价是状态变化时
//    O(状态数×层数)，这个量级（几十）完全无所谓。
//
// 4) **伤害必须回传明细**（`DamageResult::steps`）。回合制游戏的伤害数字
//    是玩家唯一能看到的"为什么这一下这么疼"——"护甲减了 40%、易伤 +50%、
//    暴击 ×1.5、护盾吃掉 12" 这些如果只在公式里，调平衡时只能靠猜。
//    判据也必须读明细（只读最终血量的话，"护甲没生效"和"易伤没生效"
//    在数值上可以完全一样）。
//
// 分层：只依赖 `<string>` / `<vector>` / `gameplay/GameRandom.h`，
// 不碰 ECS / asset / Vulkan —— 所以 `--selftest gameplay` 不需要窗口。
// ============================================================

#include "gameplay/GameRandom.h"

#include <string>
#include <vector>

namespace gameplay {

// ---------------------------------------------------------------- 属性

// 修饰器的作用方式。数值含义见文件头第 1 条。
enum class ModOp : int {
    Flat = 0,      // 加起来
    AddPct = 1,    // 百分比求和（100 = +100%）
    MulPct = 2,    // 连乘（1.2 = ×1.2）
    Override = 3,  // 直接顶替 base（最后加的赢）
};

const char* modOpName(ModOp op);
bool parseModOp(const std::string& s, ModOp& out);   // "flat"/"add"/"mul"/"set"

struct StatModifier {
    ModOp op = ModOp::Flat;
    float value = 0.0f;
    // 谁挂的。状态的来源统一写成 `status:<状态id>`，装备写 `equip:<名字>`，
    // 脚本写 `script:<路径>`。⚠ 它是**摘除的唯一依据** —— 写错就摘不掉，
    // 表现是"状态到期了属性还没回去"。
    std::string source;
    // 剩余回合。-1 = 永久（装备/被动）；>0 = 每回合末减 1，到 0 移除。
    int turnsLeft = -1;
};

// 一条属性。`base` 是"白值"（等级/装备算完之后的基础），mods 是临时修正。
struct Attribute {
    float base = 0.0f;
    float minValue = -1.0e30f;
    float maxValue = 1.0e30f;
    std::vector<StatModifier> mods;

    // 按文件头第 1 条的固定顺序求值。
    float value() const;

    // 从当前值里扣掉 `amount`（护盾 / 血量 / 耐久都用它），返回**真正扣掉
    // 的量**（不够时扣到 floor 为止）。
    //
    // ⚠ 不能用 `addMod({Flat, -x})` 实现：那样会在属性上留一条永久修正器，
    //   扣完之后每次 `rebuildStatusMods` 都会再扣一次 —— 表现是"护盾自己
    //   慢慢掉光"，而且只测一次扣减的用例看不出来。
    // ⚠ floor = `max(minValue, 0)`：血不会被扣成负数（"过量伤害"要看
    //   `DamageResult::dealt` 与进公式的量之差，不是让 hp 变负）。
    // ⚠ 被 `Override` 顶住的属性**扣不动**（base 已不参与求值）→ 返回 0。
    //   护盾不要配 Override。
    float consume(float amount);

    // 明细（给面板 / 调试用）：逐条列出生效的修正器最终贡献了多少。
    // ⚠ 只在被调用时算，不缓存 —— 属性一帧可能被问几十次（UI + AI），
    //   缓存就得管失效，而失效漏一次的症状是"面板显示旧值"。
    std::string explain() const;
};

// 一组命名属性。
//
// ⚠ 用 **vector + 线性查**而不是 `unordered_map`：属性数是个位数到几十，
//   线性查完全够；而 `unordered_map` 的**遍历顺序不保证跨实现一致**，
//   一旦有人拿它去做"按顺序结算"（比如每回合的属性 dot），
//   同一份存档在两次运行里会走出不同的结果。
class AttributeSet {
public:
    void setBase(const std::string& name, float v);
    bool has(const std::string& name) const;
    float base(const std::string& name) const;
    // 缺属性返回 `fallback`（默认 0）。⚠ 与"属性存在但值为 0"不可区分 ——
    // 需要区分时先 `has()`。
    float get(const std::string& name, float fallback = 0.0f) const;
    float maxValue(const std::string& name) const;
    // 设上下限。hp / 护盾用 `min = 0`（这样 `consume()` 不会扣成负数）。
    // ⚠ 下限是**求值最后**才用的（文件头第 2 条），不是对 base 生效。
    void setRange(const std::string& name, float minV, float maxV);
    // 便捷：扣某条属性的值。属性不存在返回 0（不创建）。
    float consume(const std::string& name, float amount);

    // 加一条修正器。⚠ `source` 空串会被改写成 `"anon"`：否则"摘不掉"。
    void addMod(const std::string& name, const StatModifier& m);
    // 摘掉某条属性上 `source` 匹配的全部修正器，返回摘了几条。
    int removeModsBySource(const std::string& name, const std::string& source);
    // 摘掉**所有属性**上该 source 的修正器（状态到期用这个）。
    int removeModsBySource(const std::string& source);
    // 摘掉所有属性上 `source` 以 `prefix` 开头的修正器，返回条数。
    // ⚠ 状态重建靠它：状态的 id 会带后缀（`poison#2`），按精确 source 摘
    //   会漏，于是"层数变了但属性没变"。
    int removeModsBySourcePrefix(const std::string& prefix);
    // 回合末：`turnsLeft > 0` 的全部 -1，减到 0 的移除。返回移除条数。
    // ⚠ `turnsLeft < 0` 的**不动**（永久），`== 0` 的不会存在（加的时候
    //   就当作永久了，见 addMod 的夹取）。
    int tickTurn();

    std::size_t size() const { return m_attrs.size(); }
    const std::string& nameAt(std::size_t i) const { return m_attrs[i].first; }
    const Attribute& at(std::size_t i) const { return m_attrs[i].second; }
    void clear();

    // 一次性 dump（面板 / 存档 / 日志）。格式固定：`name=value(base)`
    std::string dump() const;

private:
    Attribute* find(const std::string& name);
    const Attribute* find(const std::string& name) const;
    Attribute& ensure(const std::string& name);

    std::vector<std::pair<std::string, Attribute>> m_attrs;
};

// ---------------------------------------------------------------- 状态

// 叠加规则。这三条覆盖了绝大多数回合制游戏的 buff 设计。
enum class StackRule : int {
    Refresh = 0,      // 已有 → 刷新剩余回合，层数不变（"眩晕"）
    Stack = 1,        // 已有 → 层数 +1（到上限再刷新回合）（"中毒"）
    Independent = 2,  // 每条独立存在、独立到期（"两个不同来源的护盾"）
};

const char* stackRuleName(StackRule r);
bool parseStackRule(const std::string& s, StackRule& out);

// 状态要往**某条**属性上挂修正器，所以一对（属性名, 修正器）。
// ⚠ 不能只写 `StatModifier`：`AttributeSet` 是按名字索引的，缺名字就不知道
//   往哪挂 —— 第一版就是那么写的，编译期能过、运行时无处可挂。
struct NamedMod {
    std::string attribute;     // "attack" / "defense" / "hp" ...
    StatModifier mod;
};

// 状态定义（模板）。作者在配表 / 脚本里写这个，运行时按它实例化。
struct StatusDef {
    std::string id;                     // "poison"
    std::string name;                   // 显示名（可空 → 用 id）
    int turns = 1;                      // 初始回合数
    int maxStacks = 1;
    StackRule rule = StackRule::Refresh;
    // 挂上去时要写进 AttributeSet 的修正器（source 会被强制改写为
    // `status:<id>`，作者不用自己填）。
    std::vector<NamedMod> mods;
    // 每回合结束时的持续伤害（0 = 不结算）。
    // ⚠ 走 `DamageSpec` 而不是直接减 HP：护盾该不该吸收毒伤、抗性算不算，
    //   是**伤害系统的规则**，不能在状态里另写一套。
    float dotPerTurn = 0.0f;
    std::string dotAttribute = "hp";     // 扣哪条属性
    std::string dotElement;              // 空 = 魔法伤害
};

// 一个活着的状态实例。
struct StatusInstance {
    StatusDef def;
    int turnsLeft = 0;
    int stacks = 1;
};

struct StatusAddResult {
    bool added = false;       // false = 被规则忽略了（比如层数满 + Refresh）
    bool refreshed = false;   // 走的是 Refresh 分支
    int stacks = 0;
    std::string reason;       // added=false 时的原因（判据读它）
};

class StatusSet {
public:
    // 加一个状态。规则见 `StackRule` 的注释。
    StatusAddResult add(const StatusDef& def);
    // 直接放回一条实例（**读档专用**）：不套叠加规则、不改层数与回合。
    // ⚠ 用 `add()` 恢复存档是错的：它会按规则重算，于是"读档时 3 层毒变
    //   1 层"、"两个护盾合成一个"。不读档的路径（施法时 add）看起来完全
    //   正常，所以这个坑只有在跑完整存档往返时才会露出来。
    bool restore(const StatusDef& def, int turnsLeft, int stacks);
    bool remove(const std::string& id);
    void clear();
    bool has(const std::string& id) const;
    // **总层数**：同名独立实例（`shield` / `shield#2`）的层数相加。
    // ⚠ 只报第一条实例的层数会让"两个护盾"看起来只有一个。
    int stacks(const std::string& id) const;
    // 第一条同名实例的剩余回合（独立叠加的各自到期，这条只用于显示）。
    int turnsLeft(const std::string& id) const;
    std::size_t size() const { return m_list.size(); }
    const StatusInstance& at(std::size_t i) const { return m_list[i]; }

    // 回合末：所有 turnsLeft -1，到 0 的移除；返回**本回合要结算的
    // 持续伤害列表**（每条 = 多少点、扣哪条属性、什么元素）。
    //
    // ⚠ 顺序固定（按加入顺序），所以"两条毒同时结算"是可复现的。
    struct DotTick {
        std::string statusId;
        std::string attribute;
        std::string element;
        float amount = 0.0f;
    };
    std::vector<DotTick> tickTurn();

    const std::vector<StatusInstance>& list() const { return m_list; }

private:
    StatusInstance* find(const std::string& id);
    const StatusInstance* find(const std::string& id) const;
    std::vector<StatusInstance> m_list;
};

// 把 `set` 里所有状态的 modifier **重建**到 `attrs` 上（文件头第 3 条）。
// 调用时机：状态集合发生变化之后（加 / 摘 / 到期）。
// ⚠ 它只动 `source` 以 `status:` 开头的修正器，装备/脚本挂的毫发无伤。
void rebuildStatusMods(const StatusSet& set, AttributeSet& attrs);

// 只摘 `status:` 前缀的修正器（`rebuildStatusMods` 的第一步，也单独有用）。
int removeStatusMods(AttributeSet& attrs);

// ---------------------------------------------------------------- 伤害

enum class DamageType : int {
    Physical = 0,
    Magic = 1,
    Fire = 2,
    Ice = 3,
    Poison = 4,
    True = 5,   // 真伤：无视护甲与抗性（只被护盾吸收）
};

const char* damageTypeName(DamageType t);
bool parseDamageType(const std::string& s, DamageType& out);

struct DamageSpec {
    float amount = 0.0f;
    DamageType type = DamageType::Physical;
    float armorPen = 0.0f;    // 0..1，护甲穿透比例
    float critChance = 0.0f;  // 0..100（来自攻击方属性）
    float critMul = 1.5f;
    bool crit = false;        // 已经判过暴击（比如技能固定暴击）
    bool canCrit = true;
    std::string source;       // "skill:fireball" / "status:poison"
};

// 一步明细。`before`/`after` 是**这一步前后**的伤害量，`label` 是这一步
// 做了什么（"armor 40 -> -28.6%"）。判据读它，玩家看到的伤害数字也可以
// 拆着往上飘。
struct DamageStep {
    std::string label;
    float before = 0.0f;
    float after = 0.0f;
};

struct DamageResult {
    float incoming = 0.0f;       // 进公式的量（暴击后）
    float afterArmor = 0.0f;
    float afterVuln = 0.0f;
    float absorbed = 0.0f;       // 被护盾吃掉
    float dealt = 0.0f;          // 真正扣掉的血
    bool crit = false;
    bool lethal = false;         // 目标被打到 0
    float hpBefore = 0.0f;
    float hpAfter = 0.0f;
    std::vector<DamageStep> steps;
    std::string summary() const;  // 一行：`120 -> armor 86 -> vuln 129 -> shield 0 -> hp 129`
};

// 结算一次伤害。
//
// ⚠ `target` 是**可写**的（会扣 hp 与护盾），`attacker` 只读。
// ⚠ `rng` 只在 `canCrit && critChance > 0 && !spec.crit` 时被消耗一次 ——
//   "这一下到底耗了几次随机"必须是确定的，否则同一份录像回放会对不上。
DamageResult resolveDamage(const AttributeSet& attacker, AttributeSet& target,
                           const DamageSpec& spec, Random& rng);

// 护甲减伤曲线：`reduction = armor / (armor + kArmorK)`。
// kArmorK = 100 → 100 护甲吃 50% 减伤，200 护甲 66.7%，永不免疫。
// ⚠ 不用"护甲直接减固定值"：那样要么前期无敌要么后期无关，是回合制
//   数值设计里最常见的一个坑。
constexpr float kArmorK = 100.0f;

} // namespace gameplay
