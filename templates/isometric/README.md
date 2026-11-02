# __PROJECT_NAME__（等距俯视模板）

相机从斜上方俯视一块 8×8 棋盘，**没有玩家角色** —— 这是回合制策略 / 战棋的起点。

## 先看三个地方

1. `assets/scenes/main.json` 的 `camera`：`pitch` 越大越接近正上方俯视
   （1.05 rad ≈ 60°），`distance` 是相机到目标的距离。
2. `entities/Ally` 与 `entities/Enemy`：带 `gameplay` 组件（hp / attack / armor），
   是"行动单位"的最小形态。
3. `assets/scripts/main.vks`：按一次 **Interact** 推进一回合
   （`tickRound()` 结算持续伤害与状态到期）。

## 加一个单位

复制 `Ally` 实体，改 `name` 与 `transform.position`。想给它一副牌，
就加 `cards` 组件（Debug ▸ Cards 面板可以挂牌组、造牌、走一遍流程）。

## 为什么没有玩家控制器

回合制不需要"用鼠标转视角"。出生点里 `spawnOnRun = false` 表示
"这个场景不要玩家"，相机停在 `camera` 那组参数上。
