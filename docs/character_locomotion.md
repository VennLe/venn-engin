# 角色默认模型 · 移动动画 · 按键映射

> 2026-10-22 · 对应 `docs/character_pipeline_roadmap.md` 的 **A2a**（硬切版状态机）
> **P5 补充**（同日晚些时候）：crossfade、模型朝向补偿、脚底贴合、鼠标两态
> —— 见 `docs/player_feel.md`。本文关于"硬切"的描述保留在原处，它是 A2a 的
> 历史状态；当前实现已经带 0.2 s 交叉淡入。
> **P6 补充**：出厂键位定案（WASD / **鼠标右键跑** / **空格跳**，都**不需要
> 绑键**）、Jump 动画重写成"双脚离地"、真实跳跃参数与脚踩地形的接触窗口
> —— 见 `docs/player_controls.md`。本文 §4 讲的 `AnimKeyMapComponent` 仍然
> 有效，但它管的是"给某段动画额外绑个键"，与 Sprint / Jump 这类动作绑定无关。
> 代码：`engine/animation/Locomotion.*`（两个程序共用）、`editor/CharacterWindow.*`、
> `engine/ui/KeyCodes.*`、`engine/scene/SceneSerializer.cpp`
> 验收：`tools/verify_char_locomotion.py`（15 条，C0~C10）

---

## 0 · 一句话

新建一个第三人称角色，**它自带模型、骨骼和四段动画**；按 WASD 就走、停下就待机、
站够 8 秒待机动画重放一次；任意一段动画都能在角色面板里绑个键直接触发。

---

## 1 · 怎么用（五个动作）

| 想做什么 | 怎么做 |
|---------|-------|
| 建一个自带角色的第三人称玩家 | `Shift+A` ▸ Player ▸ **Third-Person Player**。不用再手动挑模型 |
| 走起来 | 进 Play（`Space`），按 **W/A/S/D**。走 → 播 `Walk`，松手 → 回 `Idle` |
| 站着不动看 8 秒重放 | 进 Play 后什么都不按。第 8 / 16 / 24 秒 `Idle` 各从头重放一次 |
| 给某段动画绑键 | 视口标签栏点 **Character** ▸ `Animation` 页 ▸ 点某行行尾的键槽 ▸ 按一个键 |
| 触发动效 | 还在 Play 里，按你刚绑的键。播完自动交还给移动状态机 |

绑错了怎么改：右键那个键槽 = 清除；再点一次 = 重绑。
同一个键被两段动画占用时，旧的那段会被**自动回收**（不会出现"按一下两个都播"）。

---

## 2 · 默认角色资产

`assets/models/characters/default_character.vmesh`

| 项 | 值 |
|---|---|
| 来源 | Mixamo `samba_dancing.fbx` → `AssetC --strip-skin --autorig --procanim` |
| 骨骼 | 23 根（`Hips` 为根，含脊椎 / 四肢 / 头颈） |
| 动画 | **Idle 2.40s / Walk 1.00s / Run 0.70s / Jump 0.90s**，全部循环可用 |
| 几何 | 35440 顶点、2 个 submesh（头身 + 眼），身高 **1.805 m** |

重新烤一遍（源 FBX 在 `assets/models/characters/` 下）：

```bash
AssetC.exe samba_dancing.fbx --strip-skin --autorig --procanim \
  -o assets/models/characters/default_character.vmesh
```

### 为什么身高要写进日志

装配时 `PLAYER-DEFAULT` 的那一行会打 `height=1.805 capsule=0.29/0.61`。
人体三处比例是**同源**的，改身高必须一起去：

```
胶囊半径  = clamp(0.16 h, 0.15, 0.60)
胶囊半高  = max(0.10, 0.5 h - 半径)
相机距离  = clamp(2.0 h, 2.5, 6.5)
```

写死过 0.30 那种常量的话，换一个 1.9 m 的模型就会"脚陷进地里"或"胶囊悬空"。
玩家实体原点是**脚底**，而模型 pivot 在几何中心 —— 装配时要按 `-min.z` 抬一次，
否则人整体沉下去半个身位（这个坑在 A0 踩过）。

---

## 3 · 状态机（`engine/animation/Locomotion`）

### 3.1 为什么抽成独立文件

这套"按速度选 clip"原本只长在 `runtime/RuntimeApp.cpp` 里。于是同一个角色在
**两只程序里表现不同**：Game.exe 走起来会迈步，编辑器 Play 里腿一动不动
（clip 永远停在实例化时挑的第一段）。抽出来之后谁改都只改一处。

### 3.2 三个设计决策

**① 用物理真值，不用输入轴。**
状态只看两个东西：**水平速度**、**是否贴地**。用输入轴判的话，按着 W 撞墙时
输入满档但速度是 0 —— 人在原地蹬腿，而日志里一切正常。这是"手感不对"
最典型的来源。

**② 空中优先。**
判定顺序是硬的：`!onGround → Jump`，然后才看速度。起跳瞬间速度可能很小、
助跑跳可能很大，按速度先判会落进 idle/walk，于是"跳到一半突然迈步"。

**③ `runSpeed <= 0` 表示自动。**
自动值 = 控制器最大速度 × 1.15。写死一个数的话，普通走路就已经超过阈值 ——
永远在播跑步。而"走路在跑"这种错最难被当成 bug 看出来：腿在动、位置在变，
只是不像走路。

### 3.3 静止 8 秒的准确语义

需求原话："停在原地如果不动长达 8 秒以上就触发 idle，此后如果一直没移动，
则每隔 8 秒继续触发。"

实现是**重放**，不是"才开始播"：

- 停下来的**那一刻**照常进入 `Idle` 循环 —— 否则人会僵在上一个姿势里干等 8 秒；
- 之后每满 `idleTriggerSec`（默认 8.0s）把 `Idle` 从 `time = 0` **重头播一次**；
- 节奏实测：`retrigger=1 idle=8.00s` → `retrigger=2 idle=16.01s`；
- 一旦移动，两个计时器都清零，下次停下重新起算；
- `idleTriggerSec = 0` 关掉这条（此时行为与之前逐位一致）。

### 3.4 写 clip 必须写两份

**权威关系**（`Components.h` 里写死）：没有 `AnimatorComponent` 时，
`SkinnedMeshComponent` 的 `time / speed / playing / loop` 是权威；有它时，
**它的**那四项才是权威，`AnimatorSystem` 每帧把它们镜像回 `sk`。

所以状态机里改 clip 时两边都写。只写一边的表现是"改了但没生效"，而且
编辑器 Play 与 Game.exe 的失败方式还不一样（一个有 Animator 一个没有）。

---

## 4 · 按键映射（`AnimKeyMapComponent`）

### 4.1 它是场景数据，不是全局键位

`ecs::AnimKeyMapComponent` 挂在角色实体上，跟着角色**存盘**：

```json
"animKeys": { "holdSeconds": 0, "entries": [ { "clip": "Jump", "key": 75 } ] }
```

`key` 是 **GLFW 键码**（75 = `K`）。和全局 `InputMap` 的分工：`InputMap` 管
"前进/跳跃"这种**玩法语义**，`animKeys` 管"这段动画想看就按这个键"这种
**调试/演出**语义。两者互不干扰。

### 4.2 键码翻译表（`engine/ui/KeyCodes.*`）

一张表搞定三件事：GLFW 码 ↔ `ImGuiKey` ↔ 给人看的标签（`K` / `F5` / `Num1`）。
收字母、数字、F1~F12、小键盘、方向键、标点；**故意不收**修饰键与 `Esc`
（修饰键要组合、`Esc` 是"取消绑键"）。

### 4.3 强播的规矩

按下后**忽略状态机**强播这一段：`loop = false`，播完（或到 `holdSeconds`）
自动交还给移动状态机，并还原强播前 `loop` 的值。这条路上不做混合 ——
引擎还没有 crossfade（见路线图 A2b）。

`holdSeconds = 0`（默认）表示用 clip 自己时长。

---

## 5 · 面板位置

角色面板就在**视口标签栏**上（`Viewport` / `Character` 并排），点标签切换。

`Animation` 页每条 clip 一行：名字 · 时长 · 行尾键槽。键槽三态：

| 样式 | 含义 | 左键 | 右键 |
|---|---|---|---|
| `+` | 没绑 | 进入"等按键" | — |
| `K` | 已绑到 K | 重绑 | 清除 |
| `...` | 正在等按键 | — | `Esc` 取消 |

打开面板时它会**自动认领**场上玩家已经装好的模型
（`CharacterWindow: adopted bound model '...' (joints=23 clips=4)`），
不用再手动拖一次。

---

## 6 · 六个坑（都在这轮踩过）

**① `core::Input::keyPressed` 在编辑器里恒为 false。**
它被 `g_uiKeyboard` 门控，而那一份是编辑器每帧按 `io.WantCaptureKeyboard` 设的。
Play 时只要有 ImGui 窗口拿着键盘焦点，门就是关的 —— 表现是"按了没反应、
日志一行没有"。**编辑器里必须用 `ImGui::IsKeyPressed`**（`ui_keys::pressed`）。
这就是 `pollKeyTrigger` 把判定函数做成参数的原因。

**② 把 `.vmesh` 当源传给 `loadFBX`，第二次起永远失败。**
`.vmesh` 的 `vmeshPathFor(p) == p`，而 `.vmeta` 是**加载成功之后**才写的，
mtime 永远比 `.vmesh` 新 → 新鲜度判断说过期 → 回退去按 FBX 解析 `.vmesh` → 抛异常。
**第一次一切正常、第二次起必崩**，删掉 `.vmeta` 又好一次 —— 极具迷惑性。
修法：`MeshLoader.cpp` 加"情况 0"短路（源本身就是 `.vmesh` → 直接 `loadVMeshModel`）。

**③ 别让 `.vmeta` 混进资产目录。**
烤资产时误生成的 `default_character.vmesh.vmeta` 会触发坑 ②。烤完检查一下目录。

**④ 阻塞式 `hold_key` 拍出来的"走路截图"里人其实站着。**
它返回时键已经松了，人已回 `idle`。必须自己 `key_down` / `key_up`，
在**按住 W 的当中**截图。这个坑让第一版验收"绿着但没证明任何事"。

**⑤ 日志并发追加会让"第 N 个匹配"错位。**
两次 grep 之间行数会变，按下标取会取错行 → 假红 / 假绿。要按**行号偏移**
单调向后扫（`wait_regex_after` / `wait_anim_state`）。

**⑥ 编辑器 Play 没有重力，`Jump` 段走不到。**
编辑器里 `onGround` 恒为 true，所以 `EDIT-ANIM state=jump` 永远不会出现。
`Jump` 要靠**按键强播**验证（C9），不能靠状态机。运行时（Game.exe）两者都有。

---

## 7 · 验收

```bash
python tools/verify_char_locomotion.py     # 15 条，约 35 秒
```

| # | 断言 |
|---|---|
| C0 | 默认资产存在：23 骨 + 四段 clip 齐 |
| C1a | 菜单能创建第三人称角色 |
| C1b | 创建即装配默认角色（`PLAYER-DEFAULT bound`，height/capsule 一起打出来） |
| C1c | 装的是**蒙皮**模型且 clip 数 = 4（不是退化成静态网格） |
| C2 | 角色窗口打开时自动认领场上模型 |
| C3 | 角色面板在视口标签栏，矩形未退化 |
| C4a/C4b | Animation 页可达 + 每条 clip 有按键槽 |
| C5 | 点键槽 + 按 `K` → `Jump` 绑到 K（GLFW 75） |
| C6 | 按 W → `EDIT-ANIM state=walk clip=Walk` **且截图里真的在迈步** |
| C6b | 确实进过 Play（否则 C6 是空跑） |
| C7 | 松开 → 回 `Idle` |
| C8 | 静止满 8 秒重放 idle（`retrigger=1 idle=8.00s`） |
| C9 | Play 里按 K 触发 `Jump`（顺带证明 `animKeys` 存读闭环） |
| C10 | 0 条 `VK_CHECK` / 0 条 `invalid state` / 进程活着 |

**C6 是唯一不靠日志的判据。** 日志说 `state=walk` 只证明"状态机认为该走"，
不证明屏幕上那个人在迈步 —— 第一版就是日志全绿、截图里站着不动。

**C9 是存读闭环的证据。** Play 副本是 `sceneToJson` → `sceneFromJson` 出来的，
按键映射能生效就说明 `animKeys` 既写进了 JSON、又读回来了。

---

## 8 · 还没做

| 项 | 现状 | 归属 |
|---|---|---|
| crossfade | **已做（P5，2026-10-22）**：0.2 s smoothstep 双采样混合，见 `docs/player_feel.md` §4 | A2a |
| 控制器资产 `.anim.json` | 配置是 C++ 结构体，不能可视化搭状态机 | A2b |
| 根运动 | 位移由碰撞驱动，步子与位移可能轻微不同步（滑步） | A4 |
| IK | 无。斜坡上脚会穿地 | A6 |
| 手指骨 | 无。手是一整块 | — |
| 非人形绑骨 | `--autorig` 只认人形比例 | — |

`Run` 段目前只在速度确实超过 `runSpeed`（自动 = `maxSpeed × 1.15`）时才会播。
默认 `moveSpeed` 下正常走路走不到，属于预期行为。
