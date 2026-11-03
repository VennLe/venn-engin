# 15 分钟：让一个角色走起来

> 这份文档是**唯一**一份按"你要做什么"写的。其余 58 份 `docs/` 是按"哪一块
> 是怎么实现的"写的 —— 它们是查询手册，不是入门路径。
>
> 全程**只用鼠标 + W/A/S/D**，不需要环境变量、不需要控制台、不需要改 JSON。
> 如果某一步没做到，说明引擎有 bug，请照最后一节的「卡住时看哪里」报。
>
> 目标：**双击 `Launcher.exe` → 15 分钟后，你自己的角色在街心走路。**

---

## 0 · 三个 exe 分别是什么（30 秒）

`build/bin/` 里有四个可执行文件。**只记住前三个**：

| exe | 干什么 | 什么时候点 |
|---|---|---|
| `Launcher.exe` | 工程管理：新建 / 打开 / 打包 | **从这里开始**，永远 |
| `Editor.exe` | 编辑器（摆场景、调参数、按 Play） | 由启动器拉起，别直接双击 |
| `Game.exe` | 纯运行时（玩家拿到的东西） | 由「打包」产出，或按 `Play` 时内部用 |
| `AssetC.exe` | 资产烘焙（模型 / 贴图预处理） | 导入模型时自动调，暂时不用管 |

---

## 1 · 新建工程（2 分钟）

1. 双击 **`Launcher.exe`**
2. `New Project` → 名字随便写（例如 `my_first_game`）→ 选一个空目录
3. 类型选 **`Third person`**
4. 创建

建完你会得到一个**已经站着一个人的小镇**：街道 + 12 栋房子 + 树 + 一个角色。

> ⚠ 这一步之所以值得单列：在 2026-11-03 之前，四个模板里**一个可见角色都没有**。
> 新工程打开是空气中的胶囊体，而"让它变成一个人"要求手改 `assets/game.json`
> 的 `player` 块 —— 那条路上没有鼠标。

---

## 2 · 打开编辑器，认四个面板（2 分钟）

在启动器里点 `Open in Editor`。你会看到：

| 在哪 | 叫什么 | 干什么 |
|---|---|---|
| 左侧 | **Hierarchy** | 场景里所有东西的列表（按名字找东西） |
| 右侧 | **Inspector** | 选中东西之后，在这里改它的属性 |
| 中间 | **Viewport** | 3D 视口。按住 **右键** 转视角，`W/A/S/D` 飞行 |
| 底部 | **Content Browser** | 资产列表（模型 / 贴图 / 脚本 / 场景） |

**只有这四个是默认打开的。** 其余 43 个面板要用 `View ▸ Panel` 或
`Ctrl+Shift+P`（命令面板，模糊搜索）打开 —— 本教程只用到其中一两个。

视口操作（记这三条就够）：

- 按住 **右键** 拖 = 转视角；右键按住时 **W/A/S/D** = 前后左右飞
- 按住 **中键** 拖 = 平移
- 滚轮 = 推近拉远；选中物体后按 **F** = 聚焦到它

---

## 3 · 按 Run Game，看它走路（2 分钟）

视口正下方的控制条上，点 **`Run Game`**（或按 `Space`）。进游戏后：

- `W/A/S/D` 走，`Shift` 跑，`Space` 跳
- 鼠标移动 = 环绕角色转视角
- 再点一次（这时按钮写着 `Exit Game`）退出

**现在你已经有一个能走能跳的第三人称角色了。**

> ⚠ 控制条上的按钮有两组，别混：
>
> | 按钮 | 真正发生什么 | 退出时 |
> |---|---|---|
> | **Run Game** | 把当前场景**复制一份**到运行时场景跑 | 你的改动**丢弃**（编辑场景没被碰过） |
> | **Preview**（旁边的青按钮） | 在**编辑场景本身上**跑脚本 | 用快照**回滚**，看起来像"没保存" |
> | 打包出来的 `Game.exe` | 跑 `dist/<名字>/` 里的成品 | 与编辑器无关 |
>
> 症状对照：**"我在运行中摆的东西，退出之后没了"** = 正常（副本）；
> **"我拖了滑条，退出之后弹回去了"** = 你按的是 Preview。
> 三个按钮都带 tooltip，悬停就能看到这段说明。

---

## 4 · 换成你自己的角色（5 分钟）

这是本教程唯一要打开"第五个面板"的地方。

### 4.1 先把模型放进工程

把 `.vmesh`（或 `.fbx` / `.glb`）拖进 **Content Browser** 的 `assets/models` 下。
拖进去就等于导入 —— 导入结果落在同一目录。

> 模型必须是**带骨骼**的（有 skeleton），否则它只是一个雕塑，不会动。
> 引擎自带的三个可动角色在 `assets/models/characters/`：
> `default_character` / `alicia` / `animate_girl`。

### 4.2 在 Project Settings 里换掉它

`Edit ▸ Project Settings...` → 展开 **Player** 段：

| 控件 | 填什么 |
|---|---|
| **Character model** | 点 `Pick...` 从下拉里挑你的模型；清空 = 回到胶囊占位体 |
| **Animator** | 点 `Pick...` 挑 `animators/humanoid.anim.json`（走路状态机） |
| **Fit body** | 保持勾上（胶囊按模型高度反推，脚才不会陷进地里） |
| Idle / Walk / Run / Jump clips | **有动画器时自动忽略**（状态机里写了哪段归哪个状态） |

按 **Save**（面板顶部）。然后按 **Play**。

> ⚠ **换了模型但走路变成"滑行"**（脚不动、人平移）：说明新模型的动画片段名
> 不是 `Idle` / `Walk` / `Run` / `Jump`。两条路：
> ① 打开 `assets/animators/humanoid.anim.json`，把 `states[].clip` 改成你的片段名；
> ② 把 **Animator** 清空，改成用上面那四个 clip 名（四态硬编码）。
>
> ⚠ **人朝天上飘**：模型的骨骼是 Y-up（glTF 常见），引擎是 Z-up。
> 走 `Character` 面板的绑定向导会做这一步转换；直接手写配置不会。

---

## 5 · 出包，发给别人（3 分钟）

`File ▸ Package...` → 产物在工程的 `dist/<游戏名>/`。

把**整个 `dist/<游戏名>/` 目录**拷给别人，双击里面的 `Game.exe`。

> ⚠ 只拷 `Game.exe` 是不够的：`assets.pak`（你的资产）和 exe 旁边的
> 引擎资产（shaders / fonts / ui）都必须在。整个目录一起走。

---

## 6 · 卡住时看哪里

引擎的每一条错误都在 **Console** 面板（`View ▸ Panel ▸ Console`，或 `Alt+8`）。
按下面的关键字找，比读源码快：

| 症状 | 在 Console 搜 | 含义 |
|---|---|---|
| 角色是胶囊体，不是模型 | `GAME-PLAYER visual=` | 后面写 `capsule` = 模型没配上；写 `model` = 配上了 |
| 角色不动 / 姿势僵住 | `ANIM-CTL-INIT` / `GAME-ANIM-MISSING` | 状态机没起来 / 片段名对不上 |
| 角色脚陷进地面 | `GAME-PLAYER model lift=` | 抬脚量算错了，或 `Fit body` 关了 |
| 场景一片黑 | `PIPELINE` / `VULKAN` | 渲染管线初始化失败 |
| 一进 Play 东西就少 | `SCENE-LOST` | **组件忘了序列化**（2026-11-03 起会显式报警） |
| 按 Play 卡一下 | `GAME-NAV` | 现场烘导航网格（换成资产就不卡了） |

Console 里 `[WARN]` / `[ERROR]` 找不到时，`Help ▸ Show Notification History`
会保留最近 4 秒里飘过去的提示。

---

## 7 · 接下来想做什么，去哪查

按**任务**索引（不是按模块）：

| 我想… | 打开什么面板 / 看哪份文档 |
|---|---|
| 摆房子、摆道具 | `Hierarchy` 右键 → `Create`；或直接把模型拖进视口 |
| 让角色播自己的动作 | `State machine` 面板（`docs/character_animation_guide.md`） |
| 做一段过场动画 | `Timeline editor`（`docs/cutscene_sequencer.md`） |
| 写一段对话 | `Script editor`（`docs/dialogue.md`） |
| 做 HUD / 菜单 | `UI editor`（`docs/ui_hud_roadmap.md`） |
| 写玩法逻辑 | 选中实体 → `Inspector ▸ Tool ▸ Add Component ▸ Script`（`docs/script_api.md`） |
| 让 NPC 自己走 | `Navigation` 面板（`docs/navigation.md`） |
| 换画质 / 关阴影 | `Edit ▸ Project Settings...` → 点 `Quality settings...` 跳过去 |
| 打包 / 发布 | `File ▸ Package`（`docs/package_roadmap.md`） |

---

## 8 · 这份文档的验收口径

它是"可上手性"的第一条判据，量法很硬：

> **一个没用过这台引擎的人，只允许鼠标 + WASD，从双击 `Launcher.exe` 到
> "角色在场景里走动" ≤ 15 分钟，且全程不吃任何 `MYVK_*` 环境变量。**

做不到时，**先怀疑这份文档**（步骤写漏了 / 按钮改名了），再怀疑引擎。
两份都改完才算修好。
