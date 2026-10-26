# 角色动画实操手册

> **这份文档给谁看**：拿到了一个带骨骼动画的模型（FBX / glTF），想让它在
> Venn 引擎里"跑起来、做得像样"的人。
>
> **不写给谁看**：想改渲染器 / 加新着色模型的人。那些在
> `docs/anime_gap_review_2026-10-26.md`、`docs/character_pipeline_roadmap.md`。
>
> **读完能做到**：一个角色，下半身走、上半身做另一个动作，每踩一步发一条
> 可以接音效的事件，并且知道"位移到底该由动画给还是由代码给"。
>
> 全部截图与日志都来自本仓库的 `assets/models/anime_girl.fbx` 与
> `assets/scenes/anime_demo.json`，可以复现。

---

## 0. 五分钟：先看到东西，再问为什么

```bash
# 1) 生成演示场景（约 20 秒；会顺带导入一次 anime_girl.fbx）
python tools/make_anim_demo.py

# 2) 打开它
build/bin/Editor.exe
#    启动器里选 assets/scenes/anime_demo.json
#    或者直接：
#    MYVK_EDITOR_SCENE="assets/scenes/anime_demo.json" build/bin/Editor.exe
```

进去之后：

- 视口里有一个约 1.56 m 高的角色，**下半身在走路**；
- `Hierarchy` 里 `anime_girl` 下面是 7 个 `AnimeGirl_*_inst` 子实体；
- 底部 `Output Log` 里每秒钟滚过这几行：

```
ANIM-LAYER configured=1 applied=1
ANIM-MASK mode=Allow roots=1 joints=21 included=11
ANIM-EVENT entity=2 clip=Walk layer=[] kind=Footstep t=0.1500 payload=[left]
ANIM-ROOTMOTION cfg=1 n=0 bone=hips dist=0.0000 yaw=0.00deg
```

这四行就是引擎动画系统的全部"心跳"。**每加一个功能，先看日志有没有对应
那一行** —— 比盯着视口猜快十倍。

> ⚠ 顶部 `PLAYING t=3.35s` 表示视图已经在 Play 状态。编辑器里非 Play
> 时动画时间不走（`dt = 0`），这是设计，不是 bug。

---

## 1. 你的资产要长什么样

### 1.1 硬要求

| 项 | 要求 | 怎么验 |
|---|---|---|
| 格式 | `.fbx` 或 `.gltf/.glb` | — |
| 蒙皮 | **必须有 skin**（骨骼蒙皮），不能是静态网格 | 导入后 `Hierarchy` 里出现"根 + N 个子网格" |
| 动画 | clip 至少一段 | Inspector → Animation → clip 下拉里有名字 |
| 单位 | 米。人形约 1.5~1.9 m | 视口里跟 1 m 栅格比一下 |
| 轴向 | 引擎是 **Z-up 右手系**；FBX 进来自动转，glTF 也会转 | 角色应该是**站着**的，不是躺着 |

### 1.2 两条实在的经验

**① FBX 优先，glb 慎用。**
同一个角色，Blender 直接导出的 `anime_girl.glb` 进引擎只有 **0.37 m 高**
（Y-up 的 1.556 躺在水平轴上了）；走 FBX 那条路（`ufbx` 的
`right_handed_z_up` 映射）进来就是正确的 1.556 m 人形。
本仓库两份都有，`verify_*.py` 一律用 `.fbx`。

**② "原地动画"还是"带位移的动画"，先搞清楚。**
关系到第 6 节能不能用根运动。判断方法有两个：

- 在 DCC（Blender / Maya）里看：**髋部（hips）这一根骨**在这段动画里
  有没有真的往前走；
- 或者进引擎跑一遍，看日志 `ANIM-ROOTMOTION ... dist=` —— 一直是
  `0.0000` 就是原地动画。

本仓库的 `anime_girl.fbx` 四段（`Walk` / `Idle` / `Run` / `RigWalk`）
**全是原地动画**。这是游戏资产的常态（移动交给代码 / 物理），不是缺陷。

### 1.3 这一节最常见的三个坑

| 现象 | 原因 | 怎么办 |
|---|---|---|
| 导入后只有一个空实体，没有子网格 | 源文件里 mesh 没挂 skin，被当成静态网格了 | 回 DCC 检查蒙皮 |
| 角色躺在地上 | 轴向没转 | FBX 用 `ufbx` 那条路；glTF 检查导出时 up 轴 |
| 角色只有 0.3 m 高 | 单位是 cm 但没被识别 | 侧车文件里设 `import.scale`（见 `docs/fbx_asset_pipeline.md`） |

---

## 2. 导入与检查

### 2.1 导入

**方式 A（界面）**：`Content` 抽屉（底部）→ 找到 `.fbx` → 拖进视口。

**方式 B（命令行，自动化用）**：

```bash
MYVK_EDITOR_IMPORT="assets/models/anime_girl.fbx" build/bin/Editor.exe
```

### 2.2 导入成功的标志

`Hierarchy` 里应该是这个形状：

```
anime_girl                     ← 根实体（挂 SkinnedMesh + Transform）
├── assets/models/anime_girl.fbx/anime_girl/AnimeGirl_0_inst
├── ... _1_inst ... _6_inst    ← 7 个 submesh 子实体
Ground
DirectionalLight
```

> ⚠⚠ **如果你只有一个 `anime_girl` 而没有那 7 个子实体，视口里会什么都
> 看不到。** `skinned` 组件只提供"骨架在哪、关节矩阵给谁"，真正被渲染的
> 是每个 submesh 一个子实体（Mesh + Material）。手写 JSON 只填
> `"skinned": {...}` 是画不出东西的 —— 日志里
> `SCENE-SKINNED ok=1 joints=21`、`ANIM-SKIN ok=1` 全绿，画面全空。
> 这是新人（和本引擎的开发者也踩过）最容易掉进去的坑：**判据全 0 时
> 先问"被测物到底画出来没有"**。

### 2.3 检查骨架

选中根实体 → Inspector 右边切到 **Animation** 页签：

- **`Model`**：指向那个 `.fbx`；
- **`Clip`** 下拉：列出所有动画段（`Idle` / `Walk` / `Run` / `RigWalk`）；
- **`Time` / `Speed` / `Playing` / `Loop`**：单片段播放的四个旋钮。

拿这个页签就能验资产：点开下拉，名字都在、点一段进视口能动 = 资产没问题。

---

## 3. 播动画（先不加 Animator）

只想要"它动起来"的话，这一页就够了：选 clip、勾 playing、拖时间轴。

**帧率无关**：动画时间按 `time += dt × speed` 推进，所以 30 fps 和
144 fps 下同一段动画的**时长一样**。不用担心"高刷屏上角色变快"。

**控制台（自动化 / 调参用）**：

```
anim.clip Walk            # 切片段
anim.time 0.75            # 钉在 0.75 秒（会暂停推进）
```

---

## 4. 加 Animator：后面所有功能的地基

同一页往下滚，点 **`Add Animator`**。

### 为什么非加不可

不加也能播单段动画。但**分层 / 事件 / 根运动三样全挂在 `AnimatorComponent`
上**，没有它就没有这三样。原因很实际：`AnimationClip` 是只读资产（每次
重导入都重新生成），往里面写事件表活不过一次重新烤模型；而组件跟场景一起
落盘，是"作者的工作"该待的地方。

### 加了之后字段归谁管

一条硬规则，记一下能省很多困惑：

> **有 Animator 时，`Time` / `Speed` / `Playing` / `Loop` 以 Animator 上
> 那一份为准；Animation 页签上半部分（Skinned Mesh 那块）的同名四个字段
> 只是镜像。**

界面上看不太出来（两个地方都能改），但改错地方的症状是"改了又弹回去"。

### 状态机（可选，但迟早要用）

`Controller Path` 填一个 `assets/animators/*.anim.json`，就是一套数据驱动
的状态机。仓库里有一份现成的 `humanoid.anim.json`：

```json
{
  "defaultState": "Idle",
  "parameters": [
    { "name": "Speed",    "type": "float", "default": 0.0 },
    { "name": "Grounded", "type": "bool",  "default": true },
    { "name": "Jump",     "type": "trigger" }
  ],
  "states": [
    { "name": "Idle", "clip": "Idle", "speed": 1.0, "loop": true  },
    { "name": "Walk", "clip": "Walk", "speed": 1.0, "loop": true  },
    { "name": "Run",  "clip": "Run",  "speed": 1.0, "loop": true  },
    { "name": "Jump", "clip": "Jump", "speed": 1.0, "loop": false }
  ],
  "transitions": [
    { "from": "Idle", "to": "Walk", "duration": 0.20,
      "conditions": [ { "param": "Speed", "op": ">=", "value": 0.15 } ] }
  ]
}
```

⚠ **`Speed` / `Grounded` / `Jump` 这三个参数名是引擎侧的约定名**：编辑器
Play 和运行时每帧会往这三个名字里喂真值。改成别的名字不会报错，但那三行
真值就喂不进来 —— 表现是"角色永远停在 Idle"，而日志里状态机一切正常。

Controller 加载失败（JSON 语法错、转移引用了不存在的状态、`from == to`）
不会崩：打一条 `ANIM-CTL-BAD`，然后**整个状态机不接管**，回落到
`defaultClip` 单片段直放。

---

## 5. 分层：上半身一个动作、下半身另一个

### 5.1 想做什么

> "角色一边走，一边挥手 / 一边走，一边端着枪瞄准 / 一边走，一边说话打手势。"

这是玩法动画的**本体**。做不了它，所有"边 A 边 B"都做不了。

### 5.2 步骤

Animation 页签 → `Animation Layers  (upper / lower body)` → **`Add Layer`**：

| 控件 | 填什么（演示场景里的值） | 说明 |
|---|---|---|
| 层名 | `Upper body` | 只给人看，随便写 |
| `Clip` | `Idle` | ⚠ **不要和底座的 clip 相同**，见下面的坑 |
| `Weight` | `0.75` | 0 = 这一层关闭；1 = 完全接管遮罩内的骨 |
| `Playing` / `Loop` | 勾上 | |
| `Mask` | `Allow roots` | `Allow` = 只接管这些根的子树；`Deny` = 除了这些都接管 |
| `Mask roots` | `chest` | 逗号分隔，可写多根（如 `chest,neck`） |

填完之后控件下方会出现一行只读回显：

```
mask: 11 / 21 bones
```

**这就是判据**：`11 / 21` 说明 `chest` 子树里有 11 根骨被接管了。如果显示
`mask: NO BONE MATCHED (roots not found)`，就是骨名写错了。

最终形态（`assets/scenes/anime_demo.json` 里真实的那一段）：

```json
"layers": [{
  "name": "Upper body",
  "clip": "Idle",
  "weight": 0.75,
  "speed": 1.0,
  "loop": true,
  "playing": true,
  "fadeDur": 0.15,
  "mask": { "mode": 1, "roots": ["chest"] }
}]
```

### 5.3 遮罩为什么用"根骨"而不是"骨名单"

因为它**换资产还有效**。写 `spine,chest,neck,head,shoulder_l,shoulder_r,...`
的话，换一个角色、换一套命名（`mixamorig:` 前缀、`Bip01`、`J_Bip_C_Spine`）
就静默失效 —— 而"静默失效"在画面上跟"没配"完全一样。

只写一个 `chest`，引擎会沿着父链把它的整棵子树都收进来。名字匹配是
**不分大小写 + 子串 + 收全部命中**，所以 `chest`、`Chest`、`mixamorig:Spine1`
这种都能对上。

> ⚠ **匹配 0 根 = 这一层不生效**，不是"整具身体"。这条是刻意设计的：
> 配错名字时你会看到"没变化"，而不是"角色上半身被一段不相干的动画覆盖"。

### 5.4 三种"层没生效"的排查

看日志 `ANIM-LAYER configured=N applied=M`：

| 日志 | 含义 | 查哪里 |
|---|---|---|
| 没有这一行 | 一个层都没配 | 层是不是被删了 / `Add Animator` 没点 |
| `configured=1 applied=0` | **配了但没生效** | ① clip 名字空或不存在（看 `ANIM-LAYER-CLIP-MISSING`）② Weight 是 0 ③ 遮罩一根都没命中 |
| `configured=1 applied=1` | 生效了 | 如果画面看不出，是 **weight 太小**或**两个相位恰好相同** |

### 5.5 一个真实的坑：两层播同一段 clip → 事件发两遍

事件表是**按 clip 名**找的（`events` 数组里每项带一个 `clip` 字段）。所以
底座和附加层播**同一段** `Walk` 时，那一张表会被用两次 —— 同一个
`Footstep` 每帧发两条：

```
ANIM-EVENT entity=2 clip=Walk layer=[] kind=Footstep t=0.1500 payload=[left]
ANIM-EVENT entity=2 clip=Walk layer=[Upper body] kind=Footstep t=0.1500 payload=[left]
```

`make_anim_demo.py` 的第一版就是这样（层也用 `Walk`），一圈吐 4 条而不是 2 条。
**层的 clip 用另一段就没有这个问题**，因为那张表只属于底座那段。
如果你确实想让两层同步，那就得接受"事件带 `layer=` 字段、消费方自己按层过滤"。

---

## 6. 事件：脚步 / 命中 / 语音

### 6.1 想做什么

> "脚落地那一下放脚步声 / 挥到最高点生成刀光 / 播到 3.4 秒说一句台词。"

### 6.2 步骤

Animation 页签 → `Animation Events  (footstep / hit / sound)` → **`Add Event Track`**
→ 选中要挂事件的 clip → 每行一个事件：

| 控件 | 填什么 | 说明 |
|---|---|---|
| `Time` | `0.15` | **秒**，相对这段 clip 的开头 |
| `Kind` | `Footstep` | 类别名。消费方按它分支（`Footstep` / `Hit` / `Voice` / 随便） |
| `Payload` | `left` | 附加字符串（哪只脚 / 特效名 / 音效文件名） |

演示场景里的表：

```json
"events": [{
  "clip": "Walk",
  "events": [ { "t": 0.15, "kind": "Footstep", "payload": "left" },
              { "t": 0.65, "kind": "Footstep", "payload": "right" } ]
}]
```

### 6.3 语义：只有"跨过边界"才发

这一条决定了事件能不能用：

> 播放头从 `0.14` 走到 `0.16`，跨过了 `0.15` → **发一次**。
> 播放头**停在** `0.15`（暂停 / `dt = 0`）→ **一次都不发**。

为什么不是"落在区间里就发"：那样暂停时每帧发一次，一步的脚步声会变成
60 次/秒的机关枪。实现上是半开区间 `[上一帧, 这一帧)`。

推论（都是实测过的行为）：

- **`t` 正好等于 clip 时长 → 永远不发。** 播放头最多走到 `duration` 就被
  回绕了，而半开区间排除右端点。面板上这种情况会给提示。想"在结尾触发"
  就写 `duration - 0.02`。
- **正落在 `t` 上也只推到下一帧，不会丢。**
- **次数不随帧率变。** 60 fps 和 240 fps 跑同样长的仿真时间，事件数一样。
- **换 clip 那一帧不发。** 上一段的播放头不能当新段的起点，否则新片段里
  靠后的事件会立刻发一次。

### 6.4 谁来消费这些事件

⚠ **现在是 C++ 消费，脚本还读不到。** 这是当前的能力边界，别指望在
`.vks` 里写 `on footstep`。

引擎每帧把事件攒在 `AnimatorSystem::eventsThisFrame()` 里，**下一帧就清空**
（事件是"这一帧发生的事"，攒着会让音效晚一帧、伤害判定在角色死了之后才
结算）。典型接法：

```cpp
// runtime/RuntimeApp.cpp 之类，每帧 AnimatorSystem::tick 之后
for (const animation::FiredAnimEvent& e : m_anim.eventsThisFrame()) {
    if (e.kind == "Footstep") {
        // e.entity = 谁发的；e.layer 空 = 底座，非空 = 附加层；
        // e.payload = "left" / "right"；e.time = 事件自身的时间
        m_audio.playOneShot(e.payload == "left" ? "sfx/step_l.wav"
                                                : "sfx/step_r.wav");
    } else if (e.kind == "Hit") {
        spawnDamageBox(e.entity, e.payload);
    }
}
```

> 引擎**故意不内置**"自动播音效"这类默认实现：一个"看起来接好了"的系统
> 比"明显没接"更难查。

---

## 7. 根运动：位移到底由谁给

### 7.1 先判断你的动画是哪种

| 类型 | 表现 | 该用哪个模式 |
|---|---|---|
| **原地动画**（游戏资产常态） | 髋部原地踏步，位移交给代码 / 物理 | `Off` |
| **带位移的动画**（过场、精确对齐） | 髋部真的往前走 | `From clip` |
| 想让物理算出精确速度 | — | `Delta only` |

判断方法见 §1.2：看日志 `dist=` 是不是 `0.0000`。

### 7.2 三档到底差在哪

`Animation` 页签的 **`Root motion`** 下拉，三档：

| 档位 | 引擎做什么 | 你该做什么 |
|---|---|---|
| `Off` | 什么都不做（**与没有这个功能逐位相同**） | 自己推 Transform / 交给物理 |
| `From clip` | 把动画给根骨的每帧位移**写进实体的 Transform**，同时把根骨钳回原处 | ⚠ **不要同时开物理驱动** —— 两个写入点每帧打架，症状是抖动 / 走不动 |
| `Delta only` | 只把每帧位移算出来放进 `rootDelta`，**不碰 Transform** | 自己读 `rootDelta` 去喂 CharacterController |

`From clip` 是"引擎替你把位移搬过去"，`Delta only` 是"引擎只告诉你搬多少"。

### 7.3 根运动的三条底层语义（做对了才不飘）

这三条引擎已经做掉了，但你得知道，否则排查时会怀疑错地方：

1. **钳回基准，不是减增量。**
   每帧模型会重新采样，根骨本来就在"动画当前时刻的位置"。如果只是减掉
   这一帧的增量，它**只滞后一帧**、并没有回到原点 —— 结果实体走了 1 m、
   模型也自己走了 1 m，**双倍位移**。正确做法是把根钉在"第一次提取时的
   位置"。这个 bug 的隐蔽之处是**每帧日志都对**，只有拿模型世界位置去
   比才看得见。

2. **跨圈那一帧的增量必须丢掉。**
   走路循环接回首帧时，根骨从"前方 1.5 m"瞬回原点 —— 那一帧的增量是
   `-1.5 m`。不丢的话角色每走一步被拉回原地（原地抽搐）。
   验收判据：跑两圈，累计位移应该是 **≈ 2 × 一圈的前进量**，不是 0。

3. **坐标系要显式转一次。**
   同一个 `Skeleton`，glTF 那条路里的关节是**文件原始空间（Y-up）**，
   FBX 那条路是 **Z-up（ufbx 加载时就转了）**。平时渲染看不出来（`invMeshNode`
   和 `push.model` 恰好抵消），但位移要写进 `TransformComponent`（引擎
   Z-up）—— 不转的话 glTF 角色走路时**朝天上飘**，而且俯视看完全不动。

### 7.4 骨名填错会怎样

`Root bone` 输入框留空 = 自动（骨架里第一根没有父的骨，人形角色通常就是
`hips`）。填了名字却找不到：

```
ANIM-ROOTMOTION-BONE-MISS rootMotionBone 填了名字但骨架里找不到 →
该实体的根运动整条关闭（保持原样，不会退化成"用根骨"）
ANIM-ROOTMOTION cfg=1 n=0 bone=(none) dist=0.0000 yaw=0.00deg
```

**不会静默退化成"用根骨"**：那样填错名字的人会以为填对了。

> 为什么要手填这一项：Blender 导出的骨架常多一层静止的 `Root`（它不动），
> 自动选骨会选到它 → 增量恒为 0 → 表现成"开了根运动但一步都不走"。
> 日志的 `bone=` 会告诉你引擎实际用了哪根骨，跟美术报的名字一对比就知道。

### 7.5 本仓库的现状（诚实说明）

`anime_girl.fbx` 的四段动画**都是原地做的**，所以演示场景里 `From clip` /
`Delta only` 打开之后：

```
ANIM-ROOTMOTION cfg=1 n=0 bone=hips dist=0.0000 yaw=0.00deg
```

`n=0` 就是"这一步真的没走"。这不是 bug，是资产事实。想要根运动，得让美术
在 DCC 里把位移**烘到髋部**（不是烘到整个 armature 上），然后重导出。

---

## 8. 排错速查表

### 8.1 按现象查

| 现象 | 先看这条日志 | 说明 |
|---|---|---|
| 视口里空空的，什么也没有 | 有没有 7 个 `*_inst` 子实体 | 手写实体只有 `skinned` 时**不渲染** |
| 角色在动，但很奇怪 / 腿是尖刺 | `SCENE-SKINNED ok=1` | 资产本身数据坏了。`anime_girl` 的 `Idle` 段就是（腿被压成尖刺） |
| 角色是 T-pose，不动 | `ANIM-CLIP-MISSING` | `clipName` 填的字在骨架里找不到（逐字比较，大小写敏感） |
| 不动，也没日志 | `ANIM-TICK entities=` | 0 = 场景里没有蒙皮实体 |
| 层配了没反应 | `ANIM-LAYER configured=1 applied=0` | clip 空 / weight 0 / 遮罩没命中 |
| 遮罩命中不了 | `mask: NO BONE MATCHED` | 骨名写错，或者那根骨不在 `joints` 里 |
| 事件不响 | 没有 `ANIM-EVENT` | ① 没 `Add Animator` ② `t` 超过 clip 时长 ③ 时间没在推进（`playing` 没勾 / 非 Play） |
| 事件发两遍 | `layer=[]` 与 `layer=[X]` 成对出现 | 两层播了同一段 clip（§5.5） |
| 根运动开了但不走 | `ANIM-ROOTMOTION cfg=1 n=0` | 骨选错 / 动画是原地做的 |
| 根运动让角色飞起来 | — | 骨架空间没转（引擎已处理；自定义 loader 才会遇到） |
| 角色抖 / 走不动 | — | `From clip` 和物理同时在写 Transform |
| 改了 Inspector 又弹回去 | — | 有 Animator 时四个播放字段以它为准（§4） |

### 8.2 所有动画相关日志锚点

| 锚点 | 频率 | 用途 |
|---|---|---|
| `ANIM-TICK` | 每 60 帧 | 系统活着 / 处理了几个实体 |
| `ANIM-ANIMATOR` | 每 60 帧 | 权威播放字段（clip / time / speed） |
| `ANIM-CLIP-MISSING` | 每 60 帧（有才打） | clip 名找不到 |
| `ANIM-WRAP` | 每次回绕 | `fmod` 正确性 |
| `ANIM-FADE` | 过渡第一帧 | 交叉淡入开始 |
| `ANIM-PERF` / `ANIM-IK` / `ANIM-SPRING` | 每 60 帧（有才打） | 表演层 |
| `ANIM-CTL` / `ANIM-STATE` | 每 60 帧 / 每次切换 | 状态机 |
| `ANIM-LAYER` | 每 60 帧（有才打） | `configured` vs `applied` |
| `ANIM-MASK` / `ANIM-MASK-UNRESOLVED` | 缓存未命中时一次 | 遮罩解算结果 |
| `ANIM-EVENT` | 每发一条 | 事件自身的时间（可比对） |
| `ANIM-ROOTMOTION` | 每 60 帧（`cfg>0` 就打） | `n` = 真的走了的实体数 |
| `ANIM-ROOTMOTION-BONE-MISS` | 每 60 帧（有才打） | 骨名找不到 |

---

## 9. 现在做不到什么（别浪费时间试）

诚实列一下，这些是"看起来该有、其实没有"的：

| 想要 | 现状 | 替代 |
|---|---|---|
| 在 `.vks` 脚本里收动画事件 | ❌ 脚本内建函数表里没有动画项 | C++ 消费 `eventsThisFrame()` |
| 两段动画**相加**混合（比如"呼吸"叠在"走"上做加法） | ❌ 层是**按骨替换**，不是相加 | 把呼吸做进基础 clip |
| 一层里再分左右（每只手不同动作） | ❌ 一层一个遮罩 | 用两层（`arm_l` / `arm_r`） |
| 动画驱动的 Morph / 表情 | ⚠ 有 `morph.set` 控制台命令，但没接到事件上 | 手动 `morph.set` |
| 在 Inspector 里预览脚步事件发出的声音 | ❌ 引擎不内置音效消费 | 运行时自己接 |
| Root motion + 物理自动协作 | ❌ 得自己写（`Delta only` + CharacterController） | 关掉物理或关掉 `From clip` |
| 编辑器非 Play 时看到根运动位移 | ❌ 刻意不提（拖时间轴 = 任意跳帧，每一跳都会变成位移） | 进 Play 看 |

---

## 10. 一页速查

```bash
# 生成演示场景
python tools/make_anim_demo.py

# 跑动画数值自检（181 条，不开窗口，秒级）
MYVK_ANIM_TEST=1 build/bin/Editor.exe

# 跑端到端（会开编辑器窗口，几分钟）
python tools/verify_anim_runtime.py     # 数值包装
python tools/verify_anim_layers.py      # 分层像素级
python tools/verify_anim_events.py      # 事件端到端
python tools/verify_root_motion.py      # 根运动接线
```

```
# 控制台
anim.clip Walk       # 切片段
anim.time 0.75       # 钉住时间
scene.save           # 存回原路径
```

```
# 环境变量（自动化用）
MYVK_EDITOR_SCENE=<path>       # 打开指定场景
MYVK_EDITOR_PLAY=1             # 进去就 Play
MYVK_FIXED_DT=0.0166667        # 固定步长（帧数可数）
MYVK_EDITOR_IMPORT=<fbx>       # 导入模型
MYVK_EDITOR_INSP_TAB=animation # Inspector 默认页签
MYVK_EDITOR_SELECT=<名字>       # 预选实体
```

---

## 附：相关文档

| 想知道 | 看哪里 |
|---|---|
| 为什么要做这些功能 / 还缺什么 | `docs/anime_gap_review_2026-10-26.md` |
| 角色链整体路线（A0~A9 / B1~B9） | `docs/character_pipeline_roadmap.md` |
| 状态机资产的完整 schema | `engine/animation/AnimController.h` 文件头 |
| 根运动的实现与三条否证 | `engine/animation/RootMotion.h` 文件头 |
| 分层 / 遮罩的实现与端点纪律 | `engine/animation/BoneMask.h` 文件头 |
| 事件的半开区间语义 | `engine/animation/AnimEvent.h` 文件头 |
| FBX 导入链（单位 / 轴向 / 侧车） | `docs/fbx_asset_pipeline.md` |
| 玩家操控（移动 / 冲刺 / 跳跃） | `docs/player_controls.md` |
