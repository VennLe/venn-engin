# __PROJECT_NAME__（卡牌对战模板）

一张牌桌、一副 10 张的牌库、一个 45 血的敌人。**没有玩家角色**——
卡牌游戏的相机就是牌桌，操作走 UI（那是你的内容）。

## 先看四个地方

1. `assets/data/cards.csv` —— 卡牌定义（费用 / 效果 / 是否消耗 / 升级后费用）。
   改数值只要改这个文件，**不用重编译引擎**；效果串格式见 `docs/cards.md`。
2. `assets/scenes/main.json` → `entities/Battle` 的 `cards` 块 ——
   牌库里的 10 张牌（每张有 `u`=uid、`d`=defId）。
   ⚠ 牌的身份是 **uid**，不是下标：手牌一重排，下标全变。
3. `entities/Enemy` —— 带 `gameplay` 组件，是打击的目标。
4. `assets/scripts/battle.vks` —— 开局读配表、按 Interact 过一回合。

## 在编辑器里走一遍

1. 打开 **Debug ▸ Cards**，选中 `Battle`。
2. `Defs from table` 把配表载进牌库（`defs=7`）。
   > 编辑器换场景时会自动灌一次定义（P3-1），所以面板一打开通常就是
   > 已载入状态；`Defs from table` 是手动重灌（改完 CSV 不想重开编辑器时用）。
3. `Draw 5` → `Cast #0`：打击打向 `Enemy`，日志里出现
   `CARD-CAST ... def=strike ... dmg=6.00 discard=1 limbo=0`。
4. 存档 → 重开 → 牌库还在（牌库进度进存档，卡牌定义不进）。

## 出包后按什么

| 键 | 动作 |
|---|---|
| `Interact`（E） | 过一回合：`tickRound()`，结算持续伤害与状态到期 |
| `Sprint`（右键）/ 手柄 LB | **打出第一张手牌 + 补抽一张**（`castCard(0)` / `drawCards(1)`） |

打出时会**自动播粒子 + 飘伤害数字**（P3-4）：
* 同一点、同类、0.35 秒内连着打 → 数值累加并显示 `37 x3`（合并）；
* 数字取的是**实际造成/治疗量**（打在护甲上、满血治疗都比"请求量"小）；
* 这一层是独立的（`gameplay::DamageNumberLayer`），**不参与 `ui2` 的布局树**，
  所以不会被父节点的裁剪区切掉，也不会跟着面板锚点跑。

编辑器内 Play 也能出牌（脚本动词一样可用），但**没有 HUD 层**——
事件会被丢弃并打一行 `ED-CARD-FX events=N (editor play has no HUD layer…)`。
看数字请出包 / 跑 `Game.exe`。

## 下一步做什么

* 手牌 UI：`ui2` 的文档层已经能放按钮、能做变换 / Scroll / Grid / 拖放
  （P3-2），把 `cards.play` 接到每张牌上、摆成扇形即可。
* 更多卡：往 `cards.csv` 加行，`id` 不能有空格。
* 表现分档：`engine/gameplay/CardFx.cpp` 的 `kPresets[]` 是"事件 → 粒子资产 +
  飘字样式"的唯一一张表，加一档 = 加一行（**下标即枚举值，只能追加**）。
* 阵营：现在"敌方" = "除自己以外第一个带玩法组件的实体"，见
  `GameplayRuntime::firstOtherGameplayEntity`。
