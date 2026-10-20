# __PROJECT_NAME__

由 **Venn 启动器** 从 `third_person`（Third person）模板创建的工程 —— 相机在角色斜后方，鼠标环绕 + 角色转身

## 怎么玩

在启动器里打开这个工程（或直接打开 `venn.project.json`），按 Play：

1. `WASD` 移动，`Shift` 疾跑，`Space` 跳跃，`E` 交互（这是默认绑定）
2. 场上 5 个金色光球，走近 1.6 m 自动收集
3. 收齐 5 个 → Console 打印 `GAME-COMPLETE`，场地一端的传送门同时由暗青变亮
   青绿并加速自转（这一下是 `portal.vks` 干的，用来演示"两个脚本靠世界黑板
   通信"）
4. 120 秒内没收齐 → `GAME-TIMEOUT`（把 `main.vks` 的 `TIME_LIMIT` 改 0 可取消）

## 目录结构

| 路径 | 作用 |
|---|---|
| `venn.project.json` | 工程身份（名字 / 类型 / 起始场景 / 代码模式），启动器写，别手改 |
| `assets/scenes/main.json` | 起始场景：地面 / 玩家 / 5 个光球 / 传送门 |
| `assets/scripts/main.vks` | **游戏逻辑主脚本**（挂在 Player 上，管流程与胜负） |
| `assets/scripts/pickup.vks` | 光球逻辑（挂在每个 Pickup_* 上，5 个实例各一份状态） |
| `assets/scripts/portal.vks` | 终点逻辑（挂在 Portal 上，收齐后点亮自己） |
| `assets/game.json` | 运行时出厂配置（窗口尺寸、限帧、音频） |
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

- **加一个可拾取物**：场景里复制任意 `Pickup_*`，它的 `script.path` 已经是
  `scripts/pickup.vks`，复制即生效；再把 `main.vks` 的 `TOTAL` 改成新总数。
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
