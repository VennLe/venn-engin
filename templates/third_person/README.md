# __PROJECT_NAME__

由 **Venn 启动器** 从 `third_person`（Third person）模板创建的工程 —— 相机在角色斜后方，鼠标环绕 + 角色转身

## 怎么玩

在启动器里打开这个工程（或直接打开 `venn.project.json`），按 Play：

1. `WASD` 移动，`Shift` 疾跑，`Space` 跳跃（这是默认绑定）
2. 出生点在**街心**：引擎按场景里的 `GameStart` 生成玩家，Console 打印
   `GAME-READY` / `GAME-MODE`
3. 这一版**没有**摆玩法道具 —— 起始场景是一条街 + 12 栋房子 + 树。
   想加东西直接拖进视口；`scripts/pickup.vks` / `scripts/portal.vks` 是
   现成可抄的示例（收集物 / 终点），挂到新实体上就会跑
4. **你能看见自己**：这个工程带了一个角色模型 + 走路状态机
   （见下面的「你的角色」一节）
5. `V` 键切到第一人称再切回来（`ToggleView`，手柄是 Y）：切过去之后
   身体**看不见了但影子还在**（`bOwnerNoSee` + `bCastHiddenShadow`），
   眼位挂在骨架的 `head` 骨上。五个视角参数都在 `GameStart` 的
   Inspector 段（Eye bone / Eye offset / First person FOV /
   Hide own body / View blend），改完要重新 Play 才生效。
   切过去之后**身体不再跟着移动方向转**（`bUseControllerRotationYaw`
   接管）：侧步时人还是朝前，否则头骨会绕中线横移几厘米，配 80° 视场
   就是画面平移一下

## 目录结构

| 路径 | 作用 |
|---|---|
| `venn.project.json` | 工程身份（名字 / 类型 / 起始场景 / 代码模式），启动器写，别手改 |
| `assets/scenes/main.json` | **初始场景**：二次元小镇（街道 / 12 栋房子 / 树 / 灌木）+ `GameStart` 出生点 |
| `assets/scenes/terrain.json` | 地形样例：一块 257×257 的土黄地形 + 出生点，想看"地形怎么用"就打开它 |
| `assets/scripts/main.vks` | **游戏逻辑主脚本**（挂在 `GameStart` 上，管流程与世界状态） |
| `assets/scripts/pickup.vks` | 收集物逻辑（挂到任意实体上即生效，可多份实例各自持状态） |
| `assets/scripts/portal.vks` | 终点逻辑（收齐后点亮自己，演示两个脚本靠世界黑板通信） |
| `assets/game.json` | 运行时出厂配置（窗口尺寸、限帧、音频、**玩家外观**） |
| `assets/models/characters/default_character.vmesh` | 玩家角色（片段名 Idle / Walk / Run / Jump） |
| `assets/animators/humanoid.anim.json` | 走路状态机（地面 Idle↔Walk↔Run **混合**、空中 Jump/Fall/Land） |
| `config/render.json` | 画质开关（创建向导写入；删成 `{}` = 全部默认） |

## 你的角色

出生时引擎会按 `GameStart` 生成玩家，并把 `assets/game.json` 的
`player` 块挂上去：模型 `default_character.vmesh` + 状态机
`humanoid.anim.json`。**这两项在界面上就能换**：
`Edit ▸ Project Settings...` → **Player** 段 → 点 `Pick...` 挑
`assets/models/` 下的任意 `.vmesh`、`assets/animators/` 下的任意
`.anim.json`。清空模型 = 回到胶囊占位体。

⚠ 换模型之后如果走路变成滑行，先看新模型的片段名是不是
`Idle` / `Walk` / `Run` / `Jump` —— 状态机里写的就是这四个名字。

## 动画怎么动（对标 UE5 源码）

地面和空中是两套逻辑，别用一套思路去调。

**地面 `Idle ↔ Walk ↔ Run` 是混合的，不是切换的。** 引擎里没有"Walk 播完播 Run"
这回事，等价于 UE 的 `UBlendSpace1D`（`BS_Locomotion`）：横轴是**移动速度**，
按速度落在**相邻两段**之间做线性加权。差别不在"有没有过渡"，而在过渡期腿在
干什么 —— 硬切是两个完整姿势整身平均（两条腿各走各的相位，看起来像腿打结），
混合空间是"3/4 步 + 1/4 步"，仍然是一个连续步态。

两个数决定它怎么分：

- `blendWalkAt`（默认 1.2 m/s）—— 走满权重对应的速度
- `blendRunAt`（默认 5.0 m/s）—— 跑满权重对应的速度

> 注意这两个数按**移动速度**放，不是按动画的自然步速放。UE 里 Walk@200 是因为
> 它 `MaxWalkSpeed=500`；这里的走路满速是 4.5，所以 `blendWalkAt` 得落在
> 走路区间里，否则一按 `W` 就直接整段落到 Run 上（"人一按 W 就在跑"）。
> 步幅不匹配另外由**步幅匹配**（`strideMatch`）负责，两件事分工，别混。

**空中分三段，都不循环。** 对应 UE 动画蓝图里的三态：

| 段 | 判据 | 循环 |
|---|---|---|
| `Jump` | 离地且竖直速度 **> 0**（还在上升） | 否，只播一遍 |
| `Fall` | 离地且竖直速度 **≤ 0**（开始下落） | 否 |
| `Land` | 落地后 `landRecoverSec`（默认 0.12 s）内 | 否 |

判据只能用竖直速度的**方向**，不能用大小 —— 起跳那一瞬间速度最大但在上升，
抛物线顶点速度为零却已经在下降。

**转身由"移动方向"决定，不是由"相机朝向"（`bOrientRotationToMovement`）。**
按 `W` 往背后跑，角色会以 `rotationRateDeg`（默认 500°/s）匀速转过去；
转的是角色的 yaw，相机不跟着转。这和第一人称是两条互斥的路线 —— 切到第一
人称时引擎会自动把 `orientToMovement` 关掉（详见下面第 5 条）。

想让动画退回老的三态硬切做对照，把 `assets/game.json` 的 `player` 块里
`blendSpace` 改成 `false` 即可（`fadeSeconds` 会重新接管过渡）。

## 游戏逻辑写在哪

**写在 `.vks` 脚本里，不写 C++。** 引擎的 C++ 提供能力（渲染 / 物理 /
角色控制器 / 输入采集），脚本提供玩法。好处是：改完保存就生效，不用编译。

改 `assets/scripts/main.vks` → 保存 → 编辑器 Play 态**自动热重载**
（Console 出现 `VKS-RELOAD`）。手感参数都集中在文件开头：

    let TOTAL = 5              # 光球总数（= 场景里 Pickup_* 的个数）
    let TIME_LIMIT = 120.0     # 限时（秒），写 0 = 不限时

脚本语言的完整说明见 `assets/scripts/main.vks` 开头的「VKS 速查」，
以及引擎的 `docs/script_roadmap.md`。

## 常见的下一步

- **加一个可拾取物**：随便放一个实体（Cube/球都行），把 Inspector ▸ Script
  指向 `scripts/pickup.vks`；再把 `main.vks` 的 `TOTAL` 改成你要的总数。
- **搬房子 / 加一栋**：`assets/scenes/main.json` 里的房子是**内联图元**
  （`kind: builtin` 的 cube / plane + 内联材质），在视口里复制一栋再摆位置
  就行，不依赖任何外部模型文件。
- **加自己的逻辑**：新建 `assets/scripts/my_game.vks`，在实体的
  Inspector ▸ Script 里指向它。一个实体一份实例，变量互不干扰。
- **读输入**：`pressed("Jump")` / `held("Sprint")` / `axis("MoveForward")`，
  参数是 Preferences ▸ Bindings 里的**动作名**，不是键码。
- **存分数**：`setWorldAttr("score", 10)` 写世界黑板（进存档）；
  `setAttr(self(), "hp", 5)` 写逐实体变量（不进存档）。

## 三个别做的事

1. **不要在脚本里 `setPos(player())`**：移动归角色控制器 / 物理管，
   脚本改位置会被下一步物理拉回去（表现为"走两步被拽回原地"，无报错）。
2. **不要用 `on collide` 做靠近判定**：编辑器的 Play 态没有接触事件，
   同一份脚本在编辑器里会一动不动。用 `dist()`。
3. **不要手改 `venn.project.json` 的 `code` 段**，除非你确认
   `assets/scripts` 是不是指向外部目录的联接。
