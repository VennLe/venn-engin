# __PROJECT_NAME__

由 **Venn 启动器** 从 `first_person`（First person）模板创建的工程 —— 相机在眼睛位置，鼠标直接转视角

## 怎么玩

在启动器里打开这个工程（或直接打开 `venn.project.json`），按 Play：

1. `WASD` 移动，`Shift` 疾跑，`Space` 跳跃（这是默认绑定）
2. 出生点在**街心**：引擎按场景里的 `GameStart` 生成玩家，Console 打印
   `GAME-READY` / `GAME-MODE`
3. 这一版**没有**摆玩法道具 —— 起始场景是一条街 + 12 栋房子 + 树。
   想加东西直接拖进视口；`scripts/pickup.vks` / `scripts/portal.vks` 是
   现成可抄的示例（收集物 / 终点），挂到新实体上就会跑
4. `V` 键切到第三人称再切回来（`ToggleView`，手柄是 Y），镜头是一段
   **过渡**不是硬切 —— 见下面「视角与眼位」

## 视角与眼位（对标 UE5 的 FirstPerson 模板）

这一版的第一人称不是"相机贴脸"，四条口径都对着 UE 的源码走：

| UE 里的东西 | 这里对应 |
|---|---|
| `ACharacter::GetActorEyesViewPoint` 用 `GetMesh()->GetSocketLocation("head")` | 眼位 = 骨架 **head 骨**在世界里的位置，不是"脚 + 一个常数" |
| socket 的**相对变换**（美术在骨架资产里摆的那 4~5 cm） | Inspector ▸ `GameStart` ▸ **Eye offset**（米，玩家局部轴） |
| `bUsePawnControlRotation` | 相机朝向**永远来自输入**，只借骨骼的位置。头骨怎么转都不甩镜头 |
| `bOwnerNoSee` + `bCastHiddenShadow` | 第一人称看不见自己的身体，**但影子还在** —— 低头能看见自己的影子 |
| `SetViewTargetWithBlend(..., BlendTime)` | 换视角是一段平滑过渡（`View blend`，0 = 硬切） |

五个参数都在场景里 `GameStart` 的 Inspector 段：

- **Eye bone** —— 眼位骨名（默认 `head`；匹配不分大小写、允许 `mixamorig:` 前缀，会跳过 `Head_end`/`*_tip` 这类末端辅助骨）
- **Eye offset** —— 相对头骨的偏移。头骨在**颅底**，眼睛还在它上方几厘米，这段差要作者给（UE 里就是 socket）
- **First person FOV** —— 默认 80（UE 模板用 90；80 边缘拉伸小一点）
- **Hide own body in first person** —— 就是 `bOwnerNoSee`
- **View blend** —— 换视角过渡时长，默认 0.22 s

⚠ 这些都是**出生配置**：改完要重新 Play 才生效。第一人称的镜头**每帧**
跟着头骨走，但过了一道阻尼（`0.045 s`）—— 呼吸和迈步带来的那点抖动
不会 1:1 放大到全屏，否则看久了会晕。


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

| `config/render.json` | 画质开关（创建向导写入；删成 `{}` = 全部默认） |

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
