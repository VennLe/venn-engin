# 牌堆（P3-3）—— 牌库 / 手牌 / 弃牌堆 / 消耗区 / 结算区

> 代码：`engine/gameplay/CardZones.{h,cpp}`（纯数据 + 纯函数，无窗口）
> 组件：`ecs::CardDeckComponent`（`Components.h` 末尾）
> 桥：`scene::GameplayRuntime` 的 `card*` 一族（带随机流与日志）
> 面板：`Debug ▸ Cards`（`editor/CardsPanel.*`）+ `Inspector ▸ Gameplay ▸ Cards`
> 判据：`tools/verify_cards.py`（A~E，33 条）+ `--selftest gameplay` 的 **M 组 80 条**

---

## 零、一句话

**牌的身份是 `uid`，不是下标；打出的牌在结算期间进第五个区 `Limbo`，不进弃牌堆。**

这两句话是这一层存在的原因。其余（洗回、手牌上限、存档）都是围绕它们
必然会问出来的问题。

---

## 一、五个区

```
                 create()                     play()              resolve()
    ┌──────────┐  ────────►  ┌──────┐  ──────►  ┌───────┐  ──────►  ┌─────────┐
    │   Draw   │             │ Hand │           │ Limbo │           │ Discard │
    │  抽牌堆  │  ◄────────  └──────┘           └───────┘           └─────────┘
    └──────────┘   reshuffle      │                                     │
          ▲        (弃牌洗回)      │ discardHand()                       │
          └────────────────────────┴─────────────────────────────────────┘

                                    resolve() 时 def.exhaust == true
                                              │
                                              ▼
                                        ┌─────────┐
                                        │ Exhaust │  消耗区（本场不再回来）
                                        └─────────┘
```

`enum class CardPile { Draw, Hand, Discard, Exhaust, Limbo }`
—— **顺序是存档格式的一部分，只能在末尾追加**。

### ⚠⚠ 为什么必须有 `Limbo`（结算区）

"打出的牌立刻进弃牌堆"是卡牌项目里最常见的一种实现，它的坏处不是
"看起来不对"，而是**在别的系统里显形**：

```cpp
// 一张"弃牌堆每有 3 张，伤害 +1"的牌
int bonus = zones.count(CardPile::Discard) / 3;
```

如果打出的牌立刻进弃牌堆，那么在它自己的结算里读 `count(Discard)` 时
**它自己已经在里面了** → 每打一张这类牌都多算一次加成。

症状是"这张牌的伤害总比预期高一点"，而且**只在弃牌堆数量跨过 3 的倍数时
跳变**——调它是地狱。

`Limbo` 是这个 bug 的哨兵：**`Limbo` 非空 = 有一张牌正在结算**。
判据 M6 直接断言 `play()` 之后 `count(Discard) == 0`。

---

## 二、身份：`uid` 不是下标

```cpp
struct CardInstance {
    uint32_t uid = 0;      // 全局唯一，create() 分配，**永不复用**
    std::string defId;     // → CardDef::id
    int upgrade = 0;
};
```

三道相关的纪律：

| 纪律 | 违反后的症状 | 判据 |
|---|---|---|
| **永不复用**（erase 之后 `create` 给新号） | 存档里"那张牌"读到另一张 | M24 |
| **读档重算 `nextUid`**（取最大 uid + 1） | 新造的牌与老牌撞号 → "打出这张，那张消失了" | M14 |
| **`move()` 的 `pos` 是目标位置** | 手牌拖动差一位 → "拖动有点飘" | M10 |

`move()` 的契约一句话：

```
move(uid, to, pos) 之后  uidAt(to, pos) == uid
```

⚠ 特别记一下**不需要**补偿：`from == to && pos > idx` 时，`erase` 已经让
后面的元素前移一格，正好抵消。我第一版写了 `insertAt = pos - 1`，
那么 `move(1, Hand, 3)` 在 `[1,2,3,4]` 上会得到 `[2,3,1,4]` 而不是
`[2,3,4,1]` —— 差一位。

---

## 三、抽牌：队首出、自动洗回、手牌满就停

```cpp
int draw(int n, Random& rng);   // 返回**实际进手牌**的张数
```

三件事写在一次循环里，每一件都有一个"不做会怎样"：

1. **从队首取**（`m_draw.front()` + `erase(begin)`）。
   换成 `back()` 不是错，但"下一张要抽的牌"就不能用 `uidAt(Draw, 0)` 读了
   —— 而预读、UI、判据都要读它。

2. **牌库空 → 把弃牌堆洗回**（reshuffle）。
   ```cpp
   m_draw.swap(m_discard); m_discard.clear();
   rng.shuffle(m_draw);
   ++m_reshuffles; m_lastReshuffle = true;
   ```
   ⚠ **必须计数**（`reshuffles()` + `lastDrawReshuffled()`）：
   不计数的话，"洗回没发生"的唯一表现是"中盘之后抽牌效果全部哑火"，
   而 `draw()` 返回 0 与"游戏在正常跑"长得完全一样。
   UI 也要用它播"弃牌堆洗回"的动画。

3. **手牌满就停止抽**，剩下的**留在牌库**。
   ```cpp
   if (m_handLimit > 0 && hand.size() >= (size_t)m_handLimit) break;
   ```
   ⚠ 经典实现是"抽了就丢"，那等于**烧掉玩家的牌**。
   判据 M5 断言 `draw(5)` 在上限 2 时 `got=2` 且 `Draw` 还剩 3 张。

`draw()` 是唯一会检查手牌上限的地方。`move()` **不检查** ——
否则"读档恢复 15 张手牌"会因为上限 10 而失败。

---

## 四、随机流：`draw()` 自己不开流

```cpp
int draw(int n, Random& rng);
void shuffleDraw(Random& rng);
```

⚠ 刻意**不**在 `CardZones` 里放一个 `Random` 成员：

- 那样"谁在用这条流"就查不清了（三流分离的意义就是能追）；
- 牌堆的存档会把 rand 状态一起带走，与引擎的种子管理打架。

调用方一律传 `streams.gameplay`（`GameplayRuntime` 里就是 `m_streams.gameplay`）。
**表现层的 `fx` 流绝对不许碰牌序** —— 否则"多放一个粒子就改变下一张抽到的牌"。

---

## 五、存档：只存 `defId`，不存定义

| 存 | 不存 |
|---|---|
| 每张牌的 `uid` / `defId` / `upgrade`，以及它**在哪个区、第几位** | 卡牌的数值（费用/效果/描述） |
| `handLimit` / `nextUid` / `reshuffles` / `plays` | `m_defs`（内容，由配表补） |

⚠⚠ 这条与 P2 的"状态存完整定义"**正好相反**，而相反是有意的：

- 状态是"这一局正在发生的效果"。冻住数值才对 —— 否则作者把毒从 2 改成 3
  之后，玩家战斗中途读档会发现毒忽然变疼。
- 卡牌是**内容**。玩家期望"我把这张卡改强了，读档之后它就该更强"。

两处必须都写下来，否则后来的人一定会把其中一处"统一"成另一处。

### JSON 形状（`SceneSerializer.cpp` 的 `cardsToJson`）

```json
"cards": {
  "label": "Probe deck",
  "handLimit": 10, "nextUid": 14, "reshuffles": 2, "plays": 7,
  "piles": {
    "draw": [{"u": 1, "d": "strike"}],
    "hand": [{"u": 12, "d": "strike", "up": 1}]
  }
}
```

- **只写非空的区**（一副 20 张牌写 20 行，但空区不该占位）。
- 未升级不写 `up`。
- ⚠ **`limbo` 也要写**：存到一半的牌读不回来 = "打出一张牌的中途存档，
  那张牌凭空消失"。判据 D 组专门验这一条。
- ⚠ 写端与 `hasNonMeshContent` 的两处名单**必须同时加 `cards`**，
  否则会出现"写了但读端不认"或者"空实体被塞进场景文件"。

### `clear()` 不清定义表

```cpp
z.clear();              // 清五个区 + 计数器，**保留 defs**
z.setDefs(defs);        // 内容
z.deserialize(blob);    // 进度
```

读档顺序是这三步。如果 `clear()` 把 `defs` 也清了，中间那步就变成"必须"，
漏了就**整副牌变未知卡**——而未知卡不报错，只是打出来没有任何效果。
判据 M18 钉这件事。

---

## 六、定义表（P3-1 之前的临时来源）

```cpp
std::vector<CardDef> presetCardDefs();   // strike / defend / bash / slimed
```

与 `presetPoison()` 同一套路：**唯一一份定义**，Inspector 按钮、Cards 面板、
自检、判据全调它。

⚠ 它是**冒烟工具，不是游戏设计**。P3-1（配表进运行时）落地之后，正式内容
从 `assets/tables/cards.csv` 读，这份表只留作"自检与夹具的默认世界"。
所以数值故意很小、很容易口算：判据要能一眼看出"这一下打了多少"。

| id | 费用 | 效果 | 用来验 |
|---|---|---|---|
| `strike` | 1 | 6 物理 | 抽→打→弃的直线路径 |
| `defend` | 1 | 5 护盾 | 自目标 |
| `bash` | 2（升级 1） | 8 物理 + 2 易伤，**消耗** | `exhaust` 进消耗区 |
| `slimed` | 1 | 无，**不可打出** | `play()` 拒绝且牌留在手牌 |

### ⚠ 配表枚举"认不出就不猜"

```cpp
bool parseCardKind(const std::string& s, CardKind& out);  // 认不出 → 返回 false，out 不变
```

不要"猜一个最像的"（`"skil"` → `Skill`）：那会让一张写错的表**静默生效**，
而症状是"这张卡的分类不对"，跟拼写毫无关系。判据 M21。

---

## 七、面板与命令

### `Debug ▸ Cards`（第 46 张面板）

流程的完整入口：挂牌组 → 载入定义 → 造测试牌组 → 抽 → 打 → 结算 →
弃手 → 洗 → 收拾 Limbo → 往返 → 清空。

锚点：`CP-RECT / CP-STATE / CP-ENSURE / CP-LOADDEFS / CP-TESTDECK /
CP-DEFCOUNT / CP-ADDID / CP-ADDN / CP-ADD / CP-DRAWN / CP-DRAW /
CP-DISCARDHAND / CP-SHUFFLE / CP-RESOLVE / CP-SWEEP / CP-ROUNDTRIP /
CP-RESET / CP-STATUS / CP-LIST / CP-LIST-END / CP-HAND0 / CP-NOSEL / CP-NORT`

### `Inspector ▸ Gameplay ▸ Cards`（第 ⑤ 段，不新开页签）

**只做不需要随机的事**（看 / 挂牌组 / 载入定义 / 弃手）。
抽牌与洗牌要随机流，它们只在 Cards 面板 —— 那边走 `GameplayRuntime` 的
gameplay 流，与运行时是同一条。

⚠ 在页签里再开一个 `Random` 会让"面板抽过的牌"与"游戏里抽到的牌"分叉，
而分叉的表现只是"抽牌顺序不一样"——没人会想到是面板动了序列。

锚点：`INS-CARD-ADD / INS-CARD-STATS / INS-CARD-HAND / INS-CARD-DISCARD /
INS-CARD-DEFS`

⚠ **字段名必须与 `GameplayRuntime::cardLine()` 逐字相同**
（`discard=` / `exhaust=`，不是 `disc=` / `exh=`）。两处写两套形状的后果是
判据要记两份正则，而记错的那份会让"这一行断了"看起来像"数值错了"。
（这是实测踩到的：Inspector 第一版用了短名，C 组立刻红。）

### 14 条 console 命令

```
cards.state        五个区的张数 + uid + reshuffle/plays
cards.detail       逐张列出（GP-CARDS-PILE 一条一张，含 known=0/1）
cards.ensure       给选中实体挂一个空牌组
cards.defs         载入 presetCardDefs()
cards.add <id> [n] 造牌进抽牌堆
cards.deck         造固定测试牌组（5/4/1/1，**不洗**）
cards.draw <n>     抽牌（gameplay 流）
cards.play <idx>   按**手牌下标**打出
cards.resolve      结算最后打出的那张（Limbo → 弃牌/消耗）
cards.discardhand  弃掉整手
cards.shuffle      洗抽牌堆
cards.sweep        收拾 Limbo（异常恢复）
cards.roundtrip    文本行往返（ok/same）
cards.reset        清空所有区（保留组件与定义表）
```

全部回显成 `GP-CARDS-CMD name=<动词> <明细>` —— 判据用一个正则就能捞任意一条。

⚠ `cards.deck` **不洗**：判据要能算准"抽 5 张必然是 5 张打击"。
洗牌是单独的动词；把两件事合起来会让"抽到了预期之外的一张"
既可能是洗牌、也可能是排序。

---

## 八、判据表（`tools/verify_cards.py`）

| 组 | 验什么 | 条数 | 关键判据 |
|---|---|---|---|
| **A** | 内核自检 M 组 | 15 | `pass=191 fail=0` + 11 条 M 关键项单点 + 三组分布 |
| **B** | Cards 面板 + 命令流程 | 15 | play 后 `limbo=1 discard=0`；resolve 后 `discard=1` |
| **C** | Inspector ▸ Cards | 6 | `INS-CARD-STATS draw=11 hand=2 discard=0 uid=14` |
| **D** | 存档往返 | 10 | 写端 `piles.limbo` 长度 **1**（结算中的牌也存） |
| **E** | 上限 / 弃手 / 往返 | 8 | `draw(100)` → `got=10 draw=1`（不烧牌）+ 自动洗回 |

### 自检 M 组（80 条）分布

```
M1  uid 身份        4      M13 存档往返（含 Limbo）      6
M2  队首抽牌        3      M14 nextUid 重算             2
M3  洗回            6      M15 坏档拒绝                 3
M4  抽干            3      M16-17 洗牌确定性 + 集合不变   3
M5  手牌上限不烧牌  4      M18 clear 不清 defs          3
M6-7 play/resolve   6      M19 erase                    4
M8  exhaust         4      M20 升级费用                 3
M9  unplayable      3      M21 枚举容错                 4
M10 同区重排        6      M22 sweepLimbo               4
M11 跨区插位        3      M23 总数守恒                 1
M12 弃手            2      M24 uid 不复用               1
```

### 为什么 B 组要断言 `discard=0`

只验"手上少了一张"的话，**两种实现都是绿的**。而 `discard=0` 只有
"打出的牌进了 Limbo"这一种实现能满足。这是整个 P3-3 最贵的一条判据。

### 为什么 C 组不能先 `play`

console 命令在**同一个 tick** 里全跑完，页签第一帧绘制时数值已经变过 ——
那时"读进来的 `draw=11 hand=2`"这条判据就永远测不到。
（P2 的 C 组踩过一模一样的坑，这里预先绕开。）

---

## 九、已知边界

| 项 | 现状 | 归属 |
|---|---|---|
| **卡牌效果结算**（`CardEffect` → `resolveDamage` / `addStatus`） | 数据齐了，**没有结算器** | P3-1 / P3-2 |
| **配表加载**（`assets/data/cards.csv`） | ✅ P3-1（`CardTable` → `cardsFromCsv()`） | 已完成 |
| **费用 / 能量** | `CardDef::cost` 有了，`play()` **不检查** | 战斗规则层 |
| **目标选择** | `CardTarget` 有了；UI 侧靠 `dropAccept` 拖放挑目标 | P3-2 已给拖放，选敌仍是"第一个别人" |
| **`ui2` 拖拽 + 合法目标高亮** | ✅ P3-2（`dragTag` / `dropAccept`） | 已完成 |
| **伤害数字层** | ✅ P3-4（`gameplay/DamageNumberLayer` + 卡牌表现模板） | 已完成 |
| 组件移除 | `World` 没有移除组件的 API → `cards.reset` 只清区 | 引擎级 |

⚠ `play()` **不检查费用**是有意的：那是战斗规则，牌堆只负责"这张牌能不能
离开手牌"。在这里顺便判费用的话，会出现"测试时能打出、实战打不出"两套判定
——而两套判定从不同时出现，所以永远对不上。

---

## 十、下一步（P3-1 配表进运行时）

1. **`assets/tables/cards.csv`** + `CardTable`（`core/Csv` → `CardDef`）。
   效果列用紧凑格式：`damage:target:6:physical|status:target:2:vulnerable`。
2. **`CardResolver`**：把 `CardEffect` 接到 `resolveDamage` / `addStatus` 上。
   ⚠ 它必须复用 `GameplayRuntime`（随机流 + 属性求值），不能自己算。
3. **`svc.table.get`** 脚本动词（G5 的原始条款）。
4. 打包时把 `assets/tables/` 带进闭包（`AssetScan` 已经会扫 JSON，
   CSV 要单独加一条 —— 否则出包之后卡牌全变未知卡）。

P3-2（`ui2` 拖拽）与 P3-4（伤害数字层）在 P3-1 之后落地，见下一节。

---

## 十一、P3-4 表现层：伤害数字 + 卡牌表现模板（2026-11-04 ✅）

**这一层不碰牌堆数据**，只回答"打出一张牌时画面上该发生什么"。

### 11.1 伤害数字是**独立一层**，不在 `UiDocument` 里

`ui2` 的文档布局是"锚点 + 树 + 每帧重算"，而飘字要的是反过来的语义：
位置每帧变、来自世界坐标、**不能**被父节点的 `pushClip` 裁掉。
所以它走画布直绘（U8 的 game-UI builder 本来就是一块裸 `UiCanvas`）。

| 文件 | 干什么 |
|---|---|
| `engine/gameplay/DamageNumbers.h/.cpp` | 池 + 合并 + 五种样式 + 确定性漂移 + `layout()` 出屏幕坐标 |
| `runtime/ui/HudModel.h` | 把老 `std::vector<DamagePopup>` 换成这个层（`popups` 成员名不变） |
| `runtime/ui/Hud.cpp` | 只负责"照着 `PopupLayout` 画"，曲线一律不算 |

- **合并（默认开）**：同点（≤1 米）同类、且年龄差在 `kMergeWindow`(0.35s) 内
  → 数值相加、`combo++`，显示 `37 x3`。⚠ 合并**不重置 `t`**：重置会让
  "每帧都在挨打的目标"留下一条永生的数字。
- **漂移确定**：`driftX/Y` 由 `serial` 整数散列得出，不用 `rand()` ——
  否则"连拍两帧比像素"的判据永远 flaky。
- **`t` 是归一化年龄**（0..1）：寿命只影响 `tick` 的步长，绘制曲线不必再除。
- 五种样式（`PopupKind`）：`Damage / Crit / Heal / Shield / Buff / Miss`。
  `Miss` 是唯一显示固定文本（`MISS`）的；`Shield` 用 `[8]` 包起来，不依赖图标。
- **`0.4` 显示成 `1`**：`lround(0.4)=0` 会让屏幕上出现一个 "0"，看起来像伤害被吞了。

### 11.2 卡牌表现模板：一张默认表 + 三个 `.veffect`

`engine/gameplay/CardFx.h` 只有一张表：`CardFxKind → { .veffect, 飘字种类, 落点, 高度 }`。

| 动作 | 特效 | 飘字 | 落点 |
|---|---|---|---|
| `Play` | `fx/card_play.veffect` | 不弹 | 持牌者 |
| `Hit` | `fx/card_hit.veffect` | `Damage` | 目标 |
| `Crit` | `fx/card_hit.veffect` | `Crit` | 目标 |
| `Heal` | `fx/heal_glow.veffect` | `Heal` | 目标 |
| `Shield` / `Buff` | `fx/card_buff.veffect` | `Shield` / `Buff` | 目标 |
| `Draw` / `Discard` / `Exhaust` | **空** | 不弹 | — |

- 抽/弃/消耗**刻意什么都不出**：牌堆变化是 UI 的事，往世界里喷火花会让
  玩家以为"场上出了什么事"。
- 改手感 = 改 `.veffect`（粒子/音效/震屏都在里面），**不改代码**。

### 11.3 事件怎么走（`castCard` 只记账，宿主播）

```
castCard() ──► GameplayRuntime 逐条 pushCardFx(...)  ──► CardFxEvent 队列（上限 64）
                                                              │
        RuntimeApp::dispatchCardFx()（每帧，排在 runScripts 之后）
                                                              ├─► m_fx.play(预设.effect)
                                                              └─► m_hudModel.addPopup(...)
```
- **为什么不在 `castCard` 里直接播**：`GameplayRuntime` 编辑器与运行时共用，
  在编辑器里点一下 `cards.cast` 就往场景里喷火是错的（关不掉、也没有 HUD）。
- **飘字用"实际数值"**：伤害取 `DamageResult::dealt`（打护甲上差很多），
  治疗取 `after - before`（满血时治 30 只回 4）。⚠ 日志里的 `heal=` 仍然是
  **请求量**，口径不变。
- 编辑器 Play 也接了这个动词（否则脚本报 "not available"），但编辑器
  **没有 HUD 层**，事件取出来直接丢，日志留一行 `ED-CARD-FX events=N`。

### 11.4 两个新脚本动词（P3-4）

| 动词 | 返回 | 说明 |
|---|---|---|
| `castCard(idx)` | **实际造成的伤害** | 打出手牌第 idx 张并结算；目标自动选"场上第一个别人" |
| `drawCards(n)` | **实际抽到几张** | 手牌上限满了抽不进去时返回 0 |

⚠ 没有牌组 / 下标越界返回 **0** 而不是报错（"手牌空了还按了一下"是正常用法）；
诊断看 `GP-CARD-CAST ... ok=0 reason=...`。
⚠ `drawCards` 不解析 `cardDraw()` 返回的那行字符串，而是数手牌差分 ——
解析给人看的字符串是本仓明确反对的做法。

`templates/card_battler/assets/scripts/battle.vks` 已经接上：**右键 / LB = 打第一张牌**。

### 11.5 判据

| 层 | 判据 |
|---|---|
| 内核 | `--selftest gameplay` **243 条**（O 组 26 条伤害数字 + P 组 7 条模板表） |
| 脚本 | `--selftest script` **99 条**（C21 正向 + C21b 负向"宿主没接必须报错"） |
| HUD | `--selftest hud` 24 条（`hud_popup_merge` / `_no_merge` / `_pool_capped`） |
| 资产 | `verify_cards.py` **G 组**（模板表引用的 `.veffect` 都在、JSON 合法、真会喷东西、**`build/bin` 副本比源新**、模板脚本真的调了这两个动词） |

⚠ 最后那条"副本新鲜度"是这一轮踩出来的：`card_buff.veffect` 的 `_comment`
里有一对未转义的 ASCII 双引号 → JSON 非法 → 运行时 `FX-PLAY failed`，
而仓库里那份看起来完全正常（编辑器读的是 `build/bin/assets/` 的副本）。
