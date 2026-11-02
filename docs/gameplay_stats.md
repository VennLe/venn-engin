# 玩法状态层（P2）—— 属性 / 状态 / 伤害明细 / 行动条 / 存档

> 权威文档。判据在 `tools/verify_gameplay.py`（A~F 六组）与
> `Game.exe --selftest gameplay`（G~L 六组 **111 条** + P3-3 的 M 组 80 条 = **191 条**）。
> 数字口径：2026-11-03，本机 Intel Iris Xe / Debug 构建。
> 上游评估 → `docs/gameplay_fx_readiness.md`；P1 特效 → `docs/fx_system.md`。

---

## 零、一句话

**组件存"白值 + 状态列表"，其余全是算出来的。**
`AttributeSet` 是每次读取时现算的**派生视图**，不是第二份真值。

这一条是整个 P2 的地基。违反它的症状非常具体："面板显示攻击力 94，
实际扣血按 100 算" —— 因为一份数据被写在了两个地方，而两份都在被改。

---

## 一、四层结构

```
engine/gameplay/             ← 纯数据 + 纯算法，只依赖 std（--selftest 不需要窗口）
    GameRandom.{h,cpp}       PCG32 + 三条流（gameplay / loot / fx）+ 可存档
    Stats.{h,cpp}            Attribute / AttributeSet / StatusSet / resolveDamage
    GameplayTypes.{h,cpp}    AttrState / StatusState（**存盘形状**）+ 预设状态
    TurnQueue.{h,cpp}        行动条（CTB）：advance / preview / timeline / 存档
    GameplaySelfTest.{h,cpp} 191 条断言（G 属性 / H 状态 / I 伤害 / J 随机 /
                              K 回合 / L 持久化 / M 牌堆）

engine/scene/
    GameplayRuntime.{h,cpp}  世界 ↔ 数值的桥；持有 RandomStreams + TurnQueue

engine/ecs/Components.h
    GameplayComponent        **权威数据**：attributes[] / statuses[] / speed / gauge / team

engine/scene/SceneSerializer.cpp
    gameplayToJson / gameplayFromJson      组件 ↔ 场景文件

editor/InspectorPanel.cpp    Gameplay 页签（第 13 个）
editor/GameplayPanel.{h,cpp} Debug ▸ Gameplay（11 条 console 命令的落点）
engine/script/ScriptEngine   getAttr / addStatus / damage / tickRound 四个动词
```

依赖方向单向：`gameplay` 不认识 `ecs`，`ecs` 不认识 `gameplay`（只 include
`GameplayTypes.h` 里的 POD 形状），`scene::GameplayRuntime` 是唯一同时认识两边的
地方。这条分层不是洁癖 —— `--selftest gameplay` 能在**没有窗口、没有 Vulkan
设备**的情况下跑完 191 条断言，靠的就是它。

---

## 二、属性：求值顺序固定五步

```
Override → Flat → AddPct → MulPct → clamp(min, max)
```

**`clamp` 只在最后做一次。** 这是《属性求值顺序》里最容易写错的地方：
每一步都夹一次的话，"先把上限调高再乘"和"先乘再调上限"会得到两个数。

`Attribute::value()`：

```cpp
float value() const {
    bool hasOverride = false;
    float ov = 0.0f, flat = 0.0f, addPct = 0.0f, mul = 1.0f;
    for (const StatModifier& m : mods) {
        switch (m.op) {
        case ModOp::Override: hasOverride = true; ov = m.value; break;
        case ModOp::Flat:     flat += m.value; break;
        case ModOp::AddPct:   addPct += m.value; break;
        case ModOp::MulPct:   mul *= m.value; break;
        }
    }
    if (hasOverride) return clamp(ov, minValue, maxValue);   // base 不参与
    const float v = (base + flat) * (1.0f + addPct * 0.01f) * mul;
    return clamp(v, minValue, maxValue);                     // 只夹一次
}
```

### ⚠ `AddPct` 是**相加**再乘一次，`MulPct` 是**连乘**

三条 +10% 的 `AddPct` = +30%（不是 1.1³ = 1.331）；三条 ×0.9 的 `MulPct`
= 0.729（不是 0.7）。这两者的区别在"叠加几层之后就爆炸"的设计里是决定性的，
自检 G5/G6 就是钉这个的：`addpct-sum` 期望 **300**（100 × (1+2.0)），
`mulpct-mul` 期望 **400**（100 × 4）。

### ⚠⚠ `consume()` 不能"探斜率"

扣血/扣护盾要走 `Attribute::consume(amount)`，它的正确写法是**按定义反解 base**：

```cpp
const float k = (1.0f + addPct * 0.01f) * mul;   // 斜率
base = (v0 - take) / k - flat;
```

**不能**写成"探一下斜率"（`value(base + 1) - v0`）。原因：满血单位的 `base`
正卡在 `maxValue` 上，往上探出来的斜率是 **0** → 判定"扣不动" → 返回 0。
而**满血是最常见的情况**，症状是"打人完全没伤害"，且只测"扣一次半血"
的用例看不出来（半血时 `base` 没顶在上限，斜率正常）。

自检 **L1.consume-taken** 就是这条的回归钉：100 血 `consume(30)` 必须返回
**30**（不是 0），且 `storeBases` → 重新 build 之后 hp = **70**。

### 三种摘除，一个都不能少

| API | 用途 |
|---|---|
| `removeMod(name, source)` | 摘一条（`source` 是唯一键） |
| `removeAnonymous(name)` | 摘所有 `source` 为空的（调试/临时） |
| `removeModsBySourcePrefix(prefix)` | 按来源前缀批量摘 —— **状态重建走这条** |

`removeModsBySourcePrefix("status:")` 必须能命中 `status:poison` 和
`status:poison#2` 两种（独立叠加的实例带 `#N` 后缀）。前缀匹配写成
`source == prefix + id` 的话，"第二个护盾"永远摘不掉。

---

## 三、状态：三条叠加规则 + 重建而不是增量

```cpp
enum class StackRule {
    Refresh,        // 刷新寿命与强度，层数不涨（"眩晕"）
    Stack,          // 层数上涨，寿命重置，强度 × 层数（"中毒"）
    Independent,    // 每次施加是**独立实例**（`id#2`），各算各的（"护盾"）
};
```

| 规则 | `stacks()` 返回 | 实例数 | 典型 |
|---|---|---|---|
| `Refresh` | 1 | 1 | 眩晕 / 沉默 |
| `Stack` | 累计层数（≤ `maxStacks`） | 1 | 中毒 / 易伤 |
| `Independent` | **所有同名实例之和** | N | 护盾 / 回春 |

### ⚠⚠ `stacks()` 必须**累加同名实例**

第一版写的是 `if (find(id)) return s->stacks;` —— 命中 `shield` 就返回它的
层数，从不看 `shield#2`。症状是"两个护盾只挡了一次"。
自检 **H3.indep-stacks** 期望 **2**，第一版给 1。

### ⚠⚠ 状态对属性的贡献走**重建**，不是增量

```cpp
void rebuildStatusMods(const StatusSet& set, AttributeSet& attrs) {
    removeStatusMods(attrs);              // ① 先摘干净
    for (每个实例) for (每层) attrs.addMod(...);   // ② 再把当前的全挂上
}
```

增量法的错法是"加状态时 `addMod`、掉状态时 `removeMod`" —— 只要有一条
路径漏了（层数变化、寿命到期、读档覆盖），错误就**永久留在**属性上，
而且再也不会被修正（因为没有"重算"这回事）。

重建法每次都是全量，代价是个位数的乘法。`H4.rebuild-3mods` 验的是
**幂等**：连调两次重建，结果完全一样。

### ⚠⚠ 存档恢复必须走 `restore()` 而不是 `add()`

`add()` 会按叠加规则重算（`Refresh` 刷寿命、`Stack` 涨层数）。
读档时对一份"3 层毒"的快照调 `add()` → 变成 1 层。
`restore(def, turnsLeft, stacks)` 直接照抄，不看规则。
自检 **L3/L4**：快照读 3 层、恢复 3 层；而同一份快照走 `add()` 只给 **1** 层
（这是**对照判据** —— 没有它，"restore 生效了"和"add 恰好也是 3"分不出来）。

### ⚠⚠ 状态存**完整定义**，不只存 id

只存 `id` 的话，作者今天把毒从 5 点/回合调到 8 点，所有老存档的毒就
悄悄变味了。`StatusState` 存 `mods[]` / `dotPerTurn` / `dotAttribute` /
`dotElement` / `rule` / `maxStacks` —— 存档自洽，不依赖当时的配表。

### 三个预设（唯一一份定义）

`presetPoison()` / `presetShield()` / `presetVulnerable()` 在
`GameplayTypes.cpp`，Inspector 的三个按钮、脚本的 `addStatus("poison")`、
自检的夹具**全都读这里**。抄成三份的后果是"面板加的毒和脚本加的毒不一样"。

```
poison       Stack        3 回合  最多 5 层  每回合 5 点毒  −3 攻击/层
shield       Independent  2 回合             每份 +30 护盾
vulnerable   Stack        2 回合  最多 2 层  +25% 受伤/层
```

---

## 四、伤害：**必须**回传明细

```cpp
struct DamageResult {
    float incoming, afterCrit, afterArmor, afterVuln, absorbed, dealt;
    bool crit;
    std::vector<DamageStep> steps;   // 固定五步，**每步都 push**（含"没发生"的）
};
```

固定五步：**① 暴击 → ② 护甲/抗性 → ③ 易伤 → ④ 护盾 → ⑤ 扣血**。

### ⚠⚠ 为什么必须记"没发生"的步

如果只在"真的减伤了"时才 push，那么 A 用例加了暴击之后，所有后续步的
**下标会整体错位**。判据按下标读（`steps[1]` 是护甲），错位就等于判据失效，
而且是静默的。

### 护甲曲线

```
减伤率 = armor / (armor + 100)      // kArmorK = 100
穿透先削 armor，再套公式
```

armor 100 → 50%；armor 100 + 50% 穿透 → armor 50 → 33.3%（自检 I2 期望 66.67 剩余）。

### ⚠ `dealt` 是**真正扣掉的血**，不是 `amount`

`hp` 只剩 5，打 1000 → `dealt = 5`。脚本判死亡必须读 `dealt`
（`if (damage(1000) >= hp)`），拿 `amount` 判会在过量伤害时判错。

### ⚠ 真伤（`DamageType::True`）只跳护甲，**不跳护盾**

真伤的设计意图是"无视防御"，不是"无视一切"。护盾是**血量之上的一层资源**，
真伤照样吃（自检 I6/I5）。

---

## 五、随机：PCG32 + 三条流

```cpp
struct RandomStreams {
    Random gameplay, loot, fx;      // ⚠ 三流**必须分开**
    void seedAll(uint64_t base);
};
```

特效随机和玩法随机用同一条流的话，"玩家打出一发暴击"会改变"下一次爆炸的
粒子形状"—— 那不只是难看，而是**同一份存档两次运行的表现不一致**，录像回放
和自动化都对不上。`J6.streams-independent` 钉这件事。

### 边界不消耗序列

`chance(0.0f)` 恒 false、`chance(100.0f)` 恒 true，**两者都不消耗**
随机数。不然"暴击率 0% 的角色"和"暴击率 100% 的角色"会让后续随机序列
分叉（自检 I14/I15 + J5）。

### 可存档

`dump()` → `"state=... inc=..."`，`parse()` 反向。`setState(0, ...)` **拒绝**
（PCG32 的 state 是加法常数，0 会让输出恒为 0）。

---

## 六、行动条（CTB）

```
每个 tick：gauge += speed * dt；谁的 gauge 先满谁先动；动完清零。
```

**不是**"按速度排个序轮流"。排序轮流的话，"速度差 10"和"速度差 90"的收益
完全一样 —— 玩家堆速度堆到某个值之后再也没有收益。行动条把速度变成**频率**：
speed 200 的人在同样时间里动 2 次。

### ⚠⚠ 同槽先后用 `order`（加入序号），不用 gauge 大小

`gauge` 经过不同的累加路径会差出 1e-4。用"gauge 最大"做 tie-break，出手顺序
会在两次运行之间翻转。`order` 是整数，不会因为浮点抖动而变。
`preview()` 与 `advance()` **必须用同一套 tie-break**，否则顺序条在骗人（K7）。

### ⚠ `advance(dt)` 一次只出**一个**人

dt 很大时可能越过多个满槽，剩下的留到下次 `advance(0)`。一次吐出多个会
对"已经被击杀、已经从队列移除"的实体做行动（K9）。

### ⚠⚠ 同一个人连着占两个位置是**正确行为**

150 速（带 250 初始槽）对 100 速，8 次出手是：

```
A(5.00) > B(10.00) > A(11.67) > A(18.33) > B(20.00) > A(25.00) > B(30.00) > A(31.67)
```

第 3、4 位都是 A，**不是 bug**。1.5 倍速在长期里出 5:3，必然在某些位置连出。
把它"修"成交替（1:1）等于把 1.5 倍速抹平。自检 **K13** 就是写给"看着像
bug 就改掉"的人的。

### ⚠⚠ 顺序条印的必须是**时刻**，不是剩余槽量

`orderPreview()` 第一版对每个 id 调 `find(id)->remaining()` —— 那是
**原始队列**的槽量，不是"模拟到那一步"的槽量。同一个人出现三次就印三次
`5.00`，面板上变成 `A(5.00) > B(10.00) > A(5.00)`，读起来像时间倒流。

修法：新增 `timeline(n)` 返回 `(id, 从此刻起多少秒)`，`preview()` 与
`orderPreview()` 都基于它。时刻**单调不减**（两人同一瞬间满槽时相等，
按 `order` 排序，那是"同一 tick 里的先后手"）。
自检 **K12**：5.00 / 10.00 / 11.67 / 18.33。

### 插队不改 speed

`insertNow(id)` 只把 gauge 顶满 + 给一个**负数 order**（越小越优先）。
改 speed 的话，"额外行动一次"会变成"此后永久更快"，那是完全不同的设计。

---

## 七、存档形状

组件在场景文件里的样子（`"gameplay"` 块）：

```json
"gameplay": {
  "speed": 150.0, "gauge": 250.0, "team": 1,
  "inQueue": false,                       // 非默认值才写
  "attrs":    [{"n":"hp","b":85.0,"min":0.0,"max":100.0}, ...],
  "statuses": [{"id":"poison","name":"Poison","turns":2,"stacks":2,"max":5,
                "rule":"stack","dot":5.0,"dotAttr":"hp","dotEl":"poison",
                "mods":[{"a":"attack","op":"flat","v":-3.0}]}]
}
```

### ⚠⚠ 组件只要在就写，哪怕全是默认值

**空实体不进场景文件**那条（G16）在这里不适用：`gameplay` 块是"这个实体参与
数值体系"的标记。不写的话，读回来 `hasNonMeshContent` 判 false → 实体被当成
空壳丢掉。两处 `hasNonMeshContent` 名单也都补了 `je.contains("gameplay")`。

### ⚠⚠ `min` / `max` 必须和 `base` 一起存

只存 `base` 的话，读档后所有属性的上下限回默认（0..1e9 之类），
症状是"闪避率变 350%"。

### ⚠ `gauge` 必须存

不存的话读档后所有人行动条清零 → 敌人总是先手。

### ⚠ `hp` 的白值就是当前血量

`b = 85` 是"还剩 85 血"，不是"85 是基础值、实算 85+修正"。
面板/脚本读到的 `hp` 都走 `buildAttributes()` 现算，看到的是**当前值**。

---

## 八、脚本动词（6 个）

| 动词 | arity | 返回 |
|---|---|---|
| `getAttr("attack")` | 1 | 当前值（含状态加成）；属性不存在也返回 0 |
| `addStatus("poison")` | 1 | **总层数**；`-1` = 未知 id 或没组件 |
| `damage(n)` / `damage(n, "fire")` | 1..2 | **真正扣掉的血**（过量不虚高） |
| `tickRound()` | 0 | 结算的 dot **条数** |
| `castCard(idx)` | 1 | **实际造成的伤害**（P3-4）；没牌组/越界 → 0 |
| `drawCards(n)` | 1 | **实际抽到几张**（P3-4）；手牌满抽不进 → 0 |

> `castCard` / `drawCards` 的权威说明在 `docs/cards.md` §11.4：
> 目标自动选"场上第一个别人"，表现（粒子 + 飘字）由运行时自动播。
> ⚠ 它们**不报错只返回 0**（"手牌空了还按了一下"是正常用法），
> 诊断看 `GP-CARD-CAST ... ok=0 reason=...`。

### ⚠⚠ `damage` 与 U8 的 HUD 扣血是**同一个动词**

`damage` 在 `kBuiltins[]` 里曾经登记了**两条**（HUD 组 `{1,1}` + P2 组 `{1,2}`），
查表先命中老的那条 → `damage(100, "physical")` 在**编译期**报

```
'damage' expects 1 argument(s), got 2
```

整份脚本 not runnable，而错误行长得完全不像 arity 的问题。

现在合成一条 `{1,2}`，靠**返回值**分流：
- 宿主挂了 `GameplayComponent` → 走五步结算，返回 `dealt`
- 宿主返回 `-1` → 目标是空壳，回退到老的 HUD 扣血（返回 `amount`）

`-1` 这个哨兵不能用 `0` 代替：`0` 是合法的"打不动"（护盾吃满、真伤为 0）。

### ⚠⚠ `kBuiltins[]` 是**按位置**对应枚举，删条目必须原位改

这次踩的第二个坑：删了 HUD 组那条 `{"damage",1,1}`、在末尾补了 `{1,2}`，
于是从这一位起**所有内建整体错位一格** —— `getAttr` 跑进了 `setTimeScale`
的分支，报 `setTimeScale(): scale must be > 0`。

`static_assert` 只比**长度**，拦不住错位。所以：改内建动词一律**原位改**，
加新动词前 `grep '"名字",'` 一次。

### ⚠ `tickRound()` 返回的是**实例数**，不是层数

2 层毒是 1 个实例 → 返回 **1**，伤害 5×2 = 10。写成"两层 → 2 条"就把判据
绑到了错误实现上。

### ⚠ `getAttr` 与 `gameplayHasAttr` 必须成对

`getAttr` 返回 0 有两种含义（属性不存在 / 值就是 0）。不区分的话
`getAttr("atk")` 拼错会静默返回 0，战斗里表现为"攻击力永远是 0"，
而日志一行错都没有。

---

## 九、面板与命令

### Inspector ▸ Gameplay（第 13 个页签）

`kTabs[]` 加一行 `{"gameplay", "Gameplay", "...", iconGameplay}`，四段：

| 段 | 锚点 | 内容 |
|---|---|---|
| 总览 | `INS-GP-STATS` | `hp=100.0/100 atk=97.0 armor=100.0`（**带数值**，改值会重打） |
| ① 白值 | `INS-GP-ATTRS` | 可增删属性（`hp` 拒删）、`base/min/max` 三个拖动 |
| ② 状态 | `INS-GP-STATUSES` | 三个预设按钮 + 每条的层数/寿命 |
| ③ 回合 | — | `speed` / `gauge` / `team` / `inQueue` |
| ④ 伤害预览 | `INS-GP-DMGPREV` | 用**副本** + 固定种子 + `canCrit=false` 算五步，只算不改 |

⚠ ④ 必须**默认展开**。做成 `defaultOpen=false` 过一次 —— 伤害五步明细是 P2 的
核心产物，折叠起来等于用户永远看不到"护甲到底减了多少"。

⚠ `INS-GP-STATS` 的 `hp` 是**当前值**。C 组验证时**不能**先 `gameplay.hit`：
console 命令在同一个 tick 里全部跑完，等页签第一帧绘制时 hp 已经扣过了，
"hp=100/100（读进来的白值没被算错）"这条判据就永远测不到。

### Debug ▸ Gameplay

锚点：`GP-RECT / GP-STATE / GP-ORDER / GP-DT / GP-ADVANCE / GP-ENDTURN /
GP-REBUILD / GP-TICK / GP-ROUNDTRIP / GP-SEEDIN / GP-SEEDAPPLY /
GP-SEL-ATTRS / GP-SEL-STATUS / GP-ADD-POISON / GP-ADD-SHIELD / GP-ADD-VULN /
GP-HITAMT / GP-HITTYPE / GP-HIT / GP-STATUS / GP-NORT`

### 12 条 console 命令

```
gameplay.state / order <n> / advance <dt> / endturn / tick / rebuild
gameplay.seed <n> / roundtrip / addstatus <which> / hit <amount> [type] / reset
entity.select <name>            ← 凡是"要先选中才能验"的用例都要它
```

`entity.select` 存在的理由：本仓在 Hierarchy/视口点击的锚点上漂移过多轮，
所有"选中 → 读面板"的判据都需要一个**不依赖像素**的入口。

---

## 十、判据表（`tools/verify_gameplay.py`）

| 组 | 验什么 | 关键判据 |
|---|---|---|
| **A** | 内核自检 `--selftest gameplay` | `pass=191 fail=0`（含 P3-3 的 M 组 80 条）+ 六组都出现 + L1/L2 反向探针 |
| **B** | 面板锚点 + 六条只读命令 | 顺序条 **`P,S,P,P`** 且时刻 **5.00/10.00/11.67/18.33** 递增 |
| **C** | Inspector ▸ Gameplay | `hp=100.0/100`、`atk=97.0`（毒挂上了）、`steps=5` |
| **D** | 伤害五步明细 | 五条 `GP-DAMAGE-STEP` 逐条 label + `dealt=50` |
| **E** | 存档往返 | 读端 `ok=1`、写端 `hp.b=85`（护甲真生效）+ `max` + `mods` + `gauge` |
| **F** | 脚本动词（Play） | `97 / 2 / 94 / 50 / 1 / 40` + 无 `not available` |

> ⚠ 这里**不写"每组几条"**：加一条断言就要同步改三个地方（用例 /
> 本文档 / `--selftest` 的 `pass=N`），而漏改的那个数**不会报错**，只会
> 悄悄变成假基准。条数看 `verify_gameplay.py` 的收尾行（`pass=/fail=`）
> 与 `GAMEPLAY-TOTAL` 那行，那是唯一权威。

### 为什么写 85 而不是 70

E 组 `hit 30` 打 armor=100 的目标 → 减伤 50% → 真的只掉 **15** → hp = **85**。
第一版期望写的 70（= 100 − 30）恰好等于"护甲**没**生效"的结果 ——
拿它当判据，"护甲生效"和"护甲漏了"永远分不出来。

### F 组为什么要开 `MYVK_LOG_SCRIPT_PRINT=1`

脚本的 `print` 默认只进编辑器面板（用户脚本可能每帧打印，落日志会淹掉）。
要落日志必须显式开这个开关，而且只能通过 `run_editor(env_extra=...)` 传 ——
`base_env()` 会**清掉所有 `MYVK_*`**，直接改 `os.environ` 等于没设。

落出来的行是 `VKS-PRINT <path>: <value>`，判据按 `": "` 切最后一段再
`fullmatch` 数字。不能全日志扫数字（坐标/耗时也会命中），也不能子串匹配
（`97` 会被 `197` 满足）。

---

## 十一、已知边界

| 项 | 状态 | 说明 |
|---|---|---|
| 属性**依赖**（`attack = str*2`） | 没做 | `AttrState` 只有字面 `base`，没有公式字段 |
| 状态**免疫 / 驱散** | 没做 | 可以由脚本 `removeStatus` 拼，没有内建规则 |
| **多段伤害 / 连击** | 没做 | `DamageSpec` 是单段；多段 = 调多次（`steps` 会各记一份） |
| **伤害上限 / 下限** | 没做 | `dealt` 没有 `minDamage` 之类的钳 |
| 行动条**读档后 `acting`** | 部分 | `serialize` 存了 `acting`，但读回来不会**接着**那次行动 —— 会重新 `advance` |
| `tickTurn` **团队/阵营** | 部分 | `team` 只用于显示分组，没有"只结算敌方"的过滤 |
| 暴击**倍率** | 写死 | `critMultiplier` 默认 1.5，没做成属性 |
| **卡牌专供**（P3） | 待做 | 配表进运行时 / `ui2` 拖拽 / 牌堆数据 |

---

## 十二、下一步（P3 卡牌专供）

1. **配表进运行时** —— `.csv` 走 `core/Csv.*`（`verify_data_tables` 已铺好），
   卡牌 = 一行 → `CardDef{id, cost, DamageSpec, StatusDef[], targeting}`。
2. **手牌 / 牌堆 / 弃牌堆** —— 复用 `TurnQueue`（每张牌是一张"行动"，
   `insertNow` 做"额外打一张"），随机抽牌走 `loot` 流（不能和 `gameplay` 串）。
3. **`ui2` 拖拽出牌** —— 现有 `UiDocument` 有拖拽，缺的是"拖到战场目标上"
   的命中测试（要射线 → 走 `CameraSystem` 的屏幕射线）。
4. **判据** —— 一条 `verify_card.py`：抽牌确定性 / 费用不足拒收 /
   `insertNow` 插队后**下一位仍是原顺序** / 拖拽落点。
