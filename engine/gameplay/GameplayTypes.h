#pragma once
// ============================================================
// gameplay/GameplayTypes —— 玩法状态的**持久化形状**，以及与运行时的互转
//
// ------------------------------------------------------------
// 为什么要有这一层（而不是把 AttributeSet / StatusSet 直接放进组件）
// ------------------------------------------------------------
// 两套东西的寿命不一样：
//   · `AttributeSet` / `StatusSet` 是**运行时视图**：里面有 `source` 字符串、
//     临时修正器、`turnsLeft` 的中间值。它们每帧都在被重建。
//   · 组件是**权威数据**：白值（hp 现在多少、攻击力多少）+ 状态列表。
//
// 如果把 `AttributeSet` 直接塞进组件，就会出现"两份真值"：
// 面板读组件、伤害读 AttributeSet，两边一旦不同步，症状是
// **"面板显示 94，实际扣血按 100 算"** —— 而这种不一致没人会怀疑到
// 序列化身上，只会觉得"面板的显示有 bug"。
//
// 所以约定只有一条：
//     组件 = 真值； AttributeSet = 由组件**现算**出来的视图。
// 任何修改都是 "build → 改 → store" 三步，`GameplayRuntime` 负责这件事。
//
// ------------------------------------------------------------
// 为什么状态要存**完整定义**（而不是只存 id）
// ------------------------------------------------------------
// 只存 id 的话，读档要依赖"配表还是那一版"。作者调完平衡（毒从 2 改成 3）
// 之后，老存档里的毒会**悄悄变成新数值**，而玩家看到的是"这场战斗的毒
// 比上一场疼"——这种漂移既不能复现也不能解释。
// 存完整定义的代价是场景文件大一点，换来"存档里的世界与存的那一刻一致"。
// ============================================================

#include "gameplay/Stats.h"

#include <string>
#include <vector>

namespace gameplay {

// 一条属性的白值 + 上下限。
struct AttrState {
    std::string name;
    float base = 0.0f;
    // ⚠ 上下限**必须一起存**：hp 的 min 是 0（扣不成负的），攻击力的 min
    //   是 0 也无所谓，但"闪避率"的 max 是 100 —— 只存 base 会让读档之后
    //   闪避率变成 350%。
    float minValue = 0.0f;
    float maxValue = 1.0e9f;
};

// 状态要挂的修正器（属性名必须一起存，见 Stats.h 的 NamedMod 注释）。
struct StatusModState {
    std::string attribute;
    ModOp op = ModOp::Flat;
    float value = 0.0f;
};

// 一个状态实例的完整快照。
struct StatusState {
    // ⚠ 独立叠加的实例 id 带 `#N` 后缀（`shield#2`），这里**原样存** ——
    //   去掉后缀会让读档之后的两个护盾合成一个。
    std::string id;
    std::string name;
    int turnsLeft = 1;
    int stacks = 1;
    int maxStacks = 1;
    StackRule rule = StackRule::Refresh;
    std::vector<StatusModState> mods;
    float dotPerTurn = 0.0f;
    std::string dotAttribute = "hp";
    std::string dotElement;
};

// 快照 → 定义（`StatusSet::restore()` 用它，**不走 `add()`**）。
StatusDef toStatusDef(const StatusState& s);
// 定义 + 当前层数/回合 → 快照。
StatusState toStatusState(const StatusDef& d, int turnsLeft, int stacks);

// 由"白值 + 状态列表"算出完整的 `AttributeSet`（含状态挂上来的修正器）。
// ⚠ 这是**唯一**的求值入口。任何地方自己 `setBase` 之后直接读值，
//   都会漏掉状态加成 —— 而漏掉的表现是"中毒了但攻击力没降"。
void buildAttributes(const std::vector<AttrState>& attrs,
                     const std::vector<StatusState>& statuses,
                     AttributeSet& out);

// 把 `AttributeSet` 里被 `consume()` 改过的 base 写回快照。
// 只回写**已存在的名字**（`buildAttributes` 会创建缺的名字吗？不会 ——
// 所以这里找不到就是调用方的问题，直接跳过即可，不创建新条目）。
void storeBases(const AttributeSet& from, std::vector<AttrState>& attrs);

// 便捷：在快照里找一条属性，没有就建（`minValue = 0`）。
AttrState& ensureAttr(std::vector<AttrState>& attrs, const std::string& name);
const AttrState* findAttr(const std::vector<AttrState>& attrs, const std::string& name);

// 默认属性表（新挂 `GameplayComponent` 时的初始值）。
// ⚠ 名字是**约定**（`hp` / `attack` / `armor` / `vulnerable` / `shield`）：
//   `resolveDamage` 按这几个名字取值，改成别的名字不会报错，只会
//   "护甲永远 0"（`get()` 缺属性返回 fallback）。
std::vector<AttrState> defaultAttributes(float hp = 100.0f, float attack = 20.0f,
                                         float armor = 0.0f);

// ---------------------------------------------------------------- 预设状态
//
// 三条叠加规则各给一个。**唯一一份定义**：Inspector 的按钮、脚本动词、
// 自检全都调这里。各处手搓一份的症状是"面板加的中毒扣 5 点、脚本加的扣 3 点"
// —— 而这种不一致几乎没人会去比对。
//
// ⚠ 它们是**示例 + 冒烟工具**，不是"游戏设计"。做正式内容时把数值写进配表
//   （`assets/data/*.csv`），别改这里。
StatusDef presetPoison();      // Stack：3 回合、最多 5 层、每回合按层数掉血、-3 攻击
StatusDef presetShield();      // Independent：2 回合、每份 +30 护盾（两份就是两道盾）
StatusDef presetVulnerable();  // Stack：2 回合、最多 2 层、每层 +25% 受伤

// 按 id 取上面三条内置预设（也认 `vuln` 这个短名）。
//
// ⚠ 认不出 → **false 且 `out` 不变**（不猜、不返回一个空定义）。返回空定义
//   的话，`addStatus()` 会加一个"id 为空、无修正器"的状态：面板上多一条
//   看不见的垃圾，而且它会占着叠加位。
bool statusDefById(const std::string& id, StatusDef& out);

} // namespace gameplay
