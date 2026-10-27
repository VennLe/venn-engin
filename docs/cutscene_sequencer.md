# 过场时间轴（C1）· 一段运镜 + 一串事件

> 2026-10-26 · C 组第 1 刀（`anime_gap_review_2026-10-26.md` 的 C1 行）
> 代码：`engine/sequence/SequenceAsset.{h,cpp}`（解析 / 采样）、
> `engine/sequence/SequencerSystem.{h,cpp}`（播放器）、
> `engine/sequence/SequenceSelfTest.cpp`（48 条数值断言）、
> `engine/ecs/Components.h` 的 `SequencerComponent`、
> `engine/scene/SceneSerializer.cpp`（读写）、
> `runtime/RuntimeApp.cpp` + `editor/EditorApp.cpp`（接线两行）、
> `editor/InspectorPanel.cpp`（`sequence` 页签）
> 面板：`editor/TimelineEditorWindow.{h,cpp}`（时间轴轨道编辑器，§9）+
> `editor/InspectorPanel.cpp` 的 `drawTabSequence`（组件配置台）
> 验收：`Game.exe --selftest sequence`（67 条）+ `tools/verify_sequencer.py`（8 组端到端）
> + `tools/verify_timeline_editor.py`（6 组 / 34 条）
> 演示：`tools/make_sequencer_demo.py` → `assets/scenes/cutscene_demo.json`

---

## 0 · 一句话

**在场景里放一个"导演"空物体，填一行资产路径，进 Play 相机就按关键帧飞、到点发事件。**
镜头怎么走、什么时候切、放到第几秒出脚步声 —— 全在那份 `.seq.json` 里，
改完直接重进 Play 看效果，不用重新编 C++。

---

## 1 · 30 秒上手

| 步 | 做什么 | 在哪 |
|---|---|---|
| ① | **生成演示场景**（第一次） | `python tools/make_sequencer_demo.py` |
| ② | 打开 `assets/scenes/cutscene_demo.json` | Content 面板双击，或 `MYVK_EDITOR_SCENE=assets/scenes/cutscene_demo.json` |
| ③ | 选中 `Director`（空物体） | Hierarchy |
| ④ | Inspector → **Sequence** 页签 | 组件默认就在（演示场景已经配好） |
| ⑤ | 拖页签里的**时间滑条**，看视口相机跟着动 | 拖动即预览（不推进时间、不发事件） |
| ⑥ | 按 **Play** | 相机接管，8 秒后自动撒手 |

从零自己配：Hierarchy 选中任一物体 → Inspector 的 **Tool** 页 → **Add Component** →
**Cutscene Sequence** → 在 `Sequence asset` 里填 `sequences/opening.seq.json`
（**相对 `assets/`**，不要写绝对路径、也不要带 `assets/` 前缀）。

**不想手写 JSON？** 点同一个页签里的 **Edit timeline…** 打开时间轴编辑器（§9），
按 **New sequence** 就有一帧骨架，摆好机位按 **Capture** 就在播放头处打一个
关键帧 —— 整段运镜都在轨道上排，不必自己算 yaw 的弧度。

> ⚠ 相机被谁的 `CameraComponent` 接管，运镜就落在谁身上。**场景里没有
> `CameraComponent` 实体时，运镜会被静默丢弃** —— 事件照发、资产照加载、
> 画面纹丝不动。这是排查时间轴时第一个要排掉的可能。

---

## 2 · 资产格式（`.seq.json`）

放 `assets/sequences/`（CMake 会整目录拷到 `build/bin/assets/sequences/`，
`VENN_ALWAYS_ASSET_DIRS` 里有它，改完**不用重新 configure** 也能生效）。

```json
{
  "version": 1,
  "name": "opening",
  "loop": false,
  "duration": 8.0,
  "camera": [
    { "t": 0.0, "pos": [5.5, -1.2, 1.90], "yaw": 0.20, "pitch": -0.12, "fov": 45.0, "ease": "smooth" },
    { "t": 2.5, "pos": [3.0, -0.7, 1.55], "yaw": 0.22, "pitch": -0.09, "fov": 42.0, "ease": "step"   },
    { "t": 4.0, "pos": [1.6,  1.4, 1.35], "yaw": -0.72,"pitch": -0.06, "fov": 38.0, "ease": "smooth" },
    { "t": 8.0, "pos": [6.5,  0.0, 2.40], "yaw": 0.00, "pitch": -0.16, "fov": 45.0 }
  ],
  "events": [
    { "t": 0.0, "kind": "Fade",     "payload": "in"           },
    { "t": 1.2, "kind": "Footstep", "payload": "left"         },
    { "t": 4.0, "kind": "Line",     "payload": "ch01_open_01" },
    { "t": 6.5, "kind": "Music",    "payload": "calm_loop"    }
  ]
}
```

| 字段 | 含义 | 缺省 |
|---|---|---|
| `duration` | 总长（秒）。不写就取"最后一个关键帧 / 最后一条事件"的时刻 | 自动推导 |
| `loop` | 资产自己是循环的吗。**与组件上的 Loop 是"或"关系**，不是覆盖 | `false` |
| `camera[]` | 相机关键帧。`pos`（也接受 `position`）/ `yaw` / `pitch` / `fov` / `ease` | — |
| `events[]` | 事件表。`t` / `kind` / `payload` | — |

**`yaw` / `pitch` 单位是弧度**（与场景 JSON 的 `camera` 块同一口径，不是度）。

### `ease` 三档

| 值 | 语义 |
|---|---|
| `smooth`（默认） | 平滑加减速，曲线是 `u²(3−2u)`（与 `CameraSystem::blendTo` 同一条） |
| `linear` | 匀速 |
| `step` | **不是插值，是"选择"**：这一段一直保持本帧，直到抵达下一帧的那一刻**硬切**过去 |

> `step` 描述的是"从这一帧到下一帧"，所以**切点落在下一个关键帧上**。
> 想在第 4 秒切镜头，就把 `step` 写在 2.5 秒那一帧上（示例就是这么干的）。

### 三条"看起来能省、省了就错"的语义

1. **`yaw` 走最短弧**：170° → −170° 是 **+20°**，不是 −340°。直接
   `a + (b−a)·u` 会让镜头原地抡一大圈，而日志里"转了多少度"看不出来。
2. **关键帧按 `t` 排序**（`stable_sort`，同一 `t` 保留作者顺序）。插值依赖
   "相邻两帧"，JSON 写乱顺序会在段与段之间反复横跳 —— 画面只是"有点跳"。
3. **事件区间是半开 `[from, to)`**。所以
   - `t` 正好等于 0 的事件**会**发（首帧区间左端点从 `playhead − eps` 起算）；
   - 跨圈要扫两段 `[prev, dur)` + `[0, next)` —— 只用 `[prev, next)` 在回绕时
     `from > to`，**一条都不发**（循环运镜里每圈少一个音效，只在跨圈那一刻少）。

---

## 3 · 事件：现在能看见，将来能消费

播放器**只负责"到点喊一声"**，日志长这样：

```
SEQ-EVENT seq=opening kind=Footstep t=1.2000 payload=[left]
```

`kind` 是**你定的名字**，引擎不解释它（`Fade` / `Footstep` / `Line` / `Music`
只是演示约定）。消费端是后续工作的落点：

| 后续 | 会怎么接 |
|---|---|
| C2 对话 | `kind=Line` 的 `payload` 当台词 id，暂停播放头等人按键 |
| 音频 | `kind=Music` → `audio.play(payload)` |
| 画面 | `kind=Fade` → 后期参数打一条曲线 |
| C6 脚本导演 API | `SequencerSystem::fire()` 就是挂点（代码里已标注） |

> ⚠ `startTime > 0` 时**不补发**之前的事件。从第 5 秒开始播，0~5 秒的音效
> 不该一次性糊出来。

---

## 4 · 组件字段

| 字段 | 作用 | 备注 |
|---|---|---|
| `sequencePath` | 资产路径（相对 `assets/`） | 清空 = 组件什么都不做 |
| `enabled` | 总开关 | 关掉等于"这一帧不参与调度" |
| `playing` | 播 / 停 | 停住时保留姿态（不会撒手） |
| `loop` | 循环 | **与资产的 `loop` 是"或"** |
| `startTime` | 从第几秒起播 | 首帧落在它上面，且不补发之前的事件 |
| `speed` | 倍速 | 0 = 冻住；负数不推荐（会立刻完成） |
| `priority` | 同场景多条同时跑时谁赢 | **与 `CameraComponent::priority` 无关** |

运行时字段（`playhead` / `started` / `finished` / `assetLoaded` / `loadFailedLogged`）
**一律不落盘** —— 那是存档系统（C3）的活。`scene.save` 只写 `path / enabled /
playing / loop / startTime / speed / priority`，`verify_sequencer.py` 的 ⑧ 组盯着这条。

---

## 5 · 接线：为什么播放器不直接推相机

`SequencerSystem::tick(scene, dt)` **只产出意图**，不碰任何相机：

```cpp
seq.tick(scene, dt);
if (seq.hasPose())
    scene::activeCameraSystem().setDesiredPose(seq.pose());   // 运行时
scene::activeCameraSystem().update(scene, dt);
```

```cpp
seq.tick(scene, simDt);
if (seq.hasPose())
    sequence::applyPoseToSceneCamera(scene, seq.pose());      // 编辑器 Play
```

两个理由，都是踩过才知道的：

- **编辑器视口是自由相机，压根不经过 `CameraSystem`**。第一版把
  `CameraSystem*` 当参数传进 `tick`，结果"编辑器里完全不能用"。
- **运行时走 `setDesiredPose` 而不是直接写相机**：这样爆炸震动能叠在运镜上
  （两者都在"期望姿态"这一层汇合）。

排序是**硬约束**：`tick` 必须排在 `CameraSystem::update` **之前**
（排后面 = 这一帧的运镜丢掉），但又必须排在玩家控制器**之后**
（排前面 = 每帧被玩家覆盖）。

`hasPose()` 在播完那一刻变 `false` —— **必须撒手**。留着的话相机会被冻在
结尾机位，玩家再也拿不回镜头，症状是"过场结束后人物在动、画面不动"。

---

## 6 · 排查速查

| 症状 | 大概率原因 |
|---|---|
| 日志只有 `SEQ-ASSET`，一条 `SEQ-EVENT` 都没有 | 组件的 `playing` 为假 / 路径填错 / `startTime` 超出总长 |
| `SEQ-EVENT` 在发，但**画面不动** | 场景里没有 `CameraComponent` 实体（`CameraSystem::update` 直接 return） |
| 运镜在动，但**过场结束后回不来** | 检查 `hasPose()` 分支 —— 应该是 `if (seq.hasPose())`，不是无条件 |
| 一进 Play 画面就"跳到底" | 上一次运行的 `playhead` 被当真值用了（它不该落盘） |
| 循环运镜每圈少一个音效 | 跨圈只扫了一段（必须 `[prev,dur)` + `[0,next)` 两段） |
| Inspector 的 Sequence 页签**一片空白** | 选中的实体没挂组件（页面会提示去 Add Component） |
| 页签里出现橙色 `N event(s) at or past the end will never fire.` | 有事件的 `t ≥ duration` —— 它永远不会发，改 `duration` 或事件时刻 |
| 关键帧改了、画面没变 | 资产没进 `build/bin/assets/sequences/`；或页签路径框里填的不是那份 |

打开 `--trace-states` 能看到逐帧相机位姿：

```
CAM-POSE pos=(5.500,-1.200,1.900) yaw=0.200 pitch=-0.120 fov=45.0 ...
```

> ⚠ `CAM-POSE` **只在姿态变化时打印**（`CameraSystem` 里有去重）。
> "停住不动" = 没有新行，而不是"位移为 0 的行"。

---

## 7 · 验收怎么跑

```bash
# 数值层：48 条，几毫秒（解析 / 采样 / yaw 最短弧 / step / 半开区间 / 跨圈 /
#         首帧 / 夹步长 / 循环 / 编辑态 / 优先级接管）
build/bin/Game.exe --selftest sequence        # 末行应为 SEQ-TOTAL pass=48 fail=0

# 端到端：8 组（真加载场景 + 真跑帧 + 真开编辑器点页签）
python tools/verify_sequencer.py
```

两条用例是**互补**的，别只跑一条：

- 数值侧帧长是钉死的（`dt = 0.1`），所以 `--selftest` 证明不了"真实帧率下
  半开区间也对"；
- 端到端侧证明不了 `Ease::Step` 在 `t` 恰好等于关键帧边界时的分支，也
  证明不了"播完撒手"（`CAM-POSE` 去重，见上）。

端到端已登记进 `tools/run_regression.py`（`DEFAULT` + `TRIGGERS`）：
改 `engine/sequence/**`、`engine/scene/CameraSystem.*`、
`engine/scene/SceneSerializer.*`、`editor/InspectorPanel.*`、`assets/sequences/**`
任意一处都会带出它。改完那张表**必须**跑一次 `tools/check_regression_map.py`。

---

## 8 · 现在**还**不能做的（别照着试）

| 想做的事 | 现状 |
|---|---|
| 在**轨道上**排关键帧 / 加删事件 | ✅ **2026-10-27 已做**（#311，§9）—— 拖、改、存盘都在面板里 |
| 在**视口里**拖关键帧、看空间曲线 | ❌ 轨道是一维的（只有时间轴）。要做需要一条"视口 gizmo → 时间轴"的反向通道（见 §9.8） |
| 事件真的触发对话 / 音效 / 转场 | ❌ 现在只打日志（`SEQ-EVENT`），消费端属于 C2 / 音频 / 后期 |
| 过场结束把进度存下来 | ❌ 运行时字段不落盘，属于 **C3 存档** |
| 多条时间轴做"接力" | ⚠ 同时跑会按 `priority` 选一条；接力需要等 C2 的调度 |
| 关键帧带动画曲线（缓入缓出用贝塞尔） | ❌ 只有三档 `ease` |

排期在 `docs/anime_gap_review_2026-10-26.md` 的「B/C/D」三刀里：C1（本文）→
C2 对话 → C3 存档 → 之后是 D 组（打包 / 本地化 / 性能门槛）。

---

## 9 · 时间轴轨道编辑器（把运镜排出来，不写 JSON）

> #311 · `editor/TimelineEditorWindow.{h,cpp}`（约 1000 行）+
> `sequence::toJson` / `saveSequence`（写回）
> 验收：`tools/verify_timeline_editor.py`（6 组 / 34 条）
> + `--selftest sequence` 的 F 组（`rt_*` 九条往返断言）

### 9.1 三个入口

| 从哪进 | 带什么 |
|---|---|
| Inspector → Tool → **Cutscene Sequence** 页签 → **Edit timeline…** | 组件上配的那份路径 |
| **View / Window 菜单**（或底栏 `+` 托盘）→ **Timeline editor** | 空手开窗，只剩"新建" |
| `MYVK_EDITOR_TLED=<路径>`（自动化） | 指定路径 |

三个入口汇到同一个 `open(path)`，走的是 `EditorContext` 里的一次性请求 ——
所以自动化那句和手点按钮是**同一条路**，不是给测试开的旁路。

### 9.2 界面

```
┌ 时间轴编辑器 — opening *─────────────────────────────────────────────────┐
│ 资产路径 [sequences/opening.seq.json        ] [重新加载] [保存]           │
│ 时长 [8.00 秒] 循环[ ] 播放头 [0.000 秒]                                  │
│ [抓取机位] [+ 关键帧] [+ 事件] [删除] [新建过场]                          │
├────────┬─────────────────────────────────────────────────────────────────┤
│        │ 0s    1s    2s    3s    4s    5s    6s    7s    8s              │
│ 相机 5 │ ◆──────◆┈┈┈┈┈┈◆───────────◆──────────────◆                     │
│ 事件 4 │   ●Fade    ●Footstep      ●Line              ●Music              │
├────────┴─────────────────────────────────────────────────────────────────┤
│ 0.000 秒 t  [5.500][-1.200][1.900] pos  11.5° yaw  -6.9° pitch  45.0 fov │
│ smooth ease      第 1 / 5 个 | 弧度 yaw=0.2000 pitch=-0.1200             │
├──────────────────────────────────────────────────────────────────────────┤
│ 加载校验：通过   5 个关键帧，4 个事件，8.00 秒                            │
└──────────────────────────────────────────────────────────────────────────┘
```

轨道图上的三件事一眼可读：

| 形状 | 含义 |
|---|---|
| 蓝色**菱形** | 相机关键帧（选中的是黄色，超出时长的是红色） |
| 实线 | 这一段是插值过去的（`linear` / `smooth`） |
| **虚线** | 这一段是 `ease=step` —— 保持前一帧，到下一帧那一刻**硬切** |
| 彩色**圆点** | 事件。颜色由 `kind` 哈希决定（同一个 kind 永远同一个色），右边的字是 kind |
| 竖线 + 顶部三角 | 播放头 |

### 9.3 六条"省了就错"的约束

| # | 约束 | 省了会怎样 |
|---|---|---|
| ① | 拖关键帧时**松手才重排**，拖动中不排 | 拖到一半，鼠标下面那一帧在数组里跳到别处 → 选中项换成别人 |
| ② | UI 用**度**，资产用**弧度** | 作者不会用弧度思考（180° 写成 3.1416 没人算得出）。反过来，转换写错后 `yaw=0.2` 显示成 11.5° 看着"挺合理"，等镜头发疯才发现 |
| ③ | 拖播放头走 `tick(sc, 0.0f)`（只采样，不推进、不发事件） | 拖一下就变成"播"，事件也跟着发 —— 那是在编辑态跑运行时 |
| ④ | 校验**不在面板里实现**（序列化 → 丢回 `parseSequence`） | 两套规则。症状是"编辑器说没问题，进 Play 镜头不动" |
| ⑤ | 读盘失败 → **只读** | `parseSequence` 边解析边填，存盘会把 40 帧的过场截成 3 帧 |
| ⑥ | 存盘后把场景里引用同一份资产的组件**标记重读** | 窗口里是新运镜、Save 成功、进 Play 还是旧的 |

### 9.4 写回（`sequence::toJson` / `saveSequence`）

从 #311 起 `SequenceAsset` 能写。四样东西刻意**不写**：

| 不写 | 为什么 |
|---|---|
| `valid` / `error` / `warnings` | 加载期诊断，不是内容。写进去下次读盘会把上次的警告当成"这次也这样" |
| `sourcePath` | 它是"从哪读来的"，不是资产属性。换个目录打开就指回老路径 |
| 空的 `payload` | 整个字段省略。写 `"payload": ""` 与不写是同一件事，diff 里却多一行噪声 |
| `\uXXXX` 转义 | `ensure_ascii=false`。转义之后 diff 是一屏 `\u5343\u590f`，作者没法用 git 看自己改了什么 |

反过来，**有两样东西必须写**：

| 必写 | 为什么 |
|---|---|
| `duration` | `parseSequence` 的口径是"`duration <= 0` → 按内容推算"。编辑器把它变成可拖的字段之后，不写显式值的话，"我把最后一帧从 8s 拖到 9s"会看起来什么都没发生 |
| `ease` | 它是"这两帧之间怎么走"的唯一表述。省略就得去翻代码里的默认值 —— 而 `step` 和 `smooth` 的画面差一个数量级 |

再加一条**不丢东西**的：

> ⚠⚠ `"//"` 键是作者写的说明（JSON 没有注释语法，约定用它）。
> `opening.seq.json` 里那段"yaw 是弧度、step 的切点落在下一帧"是这份格式
> 唯一一份触手可及的文档 —— 编辑器**必须原样带回去**，否则作者第一次按
> Save 就无声地失去了它。`--selftest sequence` 的 `rt_comment_roundtrip`
> 钉的就是这条。

`saveSequence` 是**写临时文件再改名**，不是直接覆盖：写一半断电的后果是
"文件打不开"，而存档点就在一个快捷键之外。

### 9.5 存盘之后：场景里的组件会被刷新

作者按 Save 后，场景里**引用同一份资产**的 `SequencerComponent` 会被标记
`assetLoaded = false`，下一帧 `SequencerSystem::tick` 就会重读。

不做这件事的话症状很怪：窗口里是新运镜、按 Save 成功、进 Play 还是旧的 ——
因为 `assetLoaded` 的语义就是"读过一次就别再读了"。

**正在播的那一段不打断**（`started && !finished && playing` → 记
`TLED-SCENE-SKIP`，等它下一次起播）。中途换运镜会让画面在一帧之内跳到
一个完全不同的机位，比"这次预览是旧的"更糟。

### 9.6 自动化

```bash
python tools/verify_timeline_editor.py            # 6 组 / 34 条
python tools/verify_timeline_editor.py --only c   # 只跑某个用例
python tools/verify_timeline_editor.py --keep     # 留临时目录
```

两个注入钩子（与 `MYVK_EDITOR_REBIND` 同一条纪律：**只跑一次**）：

```
MYVK_EDITOR_TLED=<路径>                  启动即打开这份资产
MYVK_EDITOR_TLED_SCRIPT="sel:camera:1;settime:3.0;save"   按顺序执行
MYVK_EDITOR_TLED_DUMP=<路径>             把当前资产落成 JSON（不写盘也能看）
```

命令表：`sel:<camera|events>:<i>` `addkey` `capture` `addevent[:kind]` `del`
`settime:<秒>` `setpos:<x,y,z>` `setyaw:<度>` `setpitch:<度>` `setfov:<f>`
`setease:<linear|smooth|step>` `setkind:<k>` `setpayload:<s>` `setdur:<秒>`
`head:<秒>` `loop:<0|1>` `new` `save` `saveforce` `saveas:<路径>` `reload`
`check` `dump`。

> ⚠ 注入的文本**用 ASCII**：`std::getenv` 在 Windows 上按 ANSI 代码页转换
> 环境块，中文进去会变成 GBK 字节 —— 那验的是"编码没配好"，不是编辑器对不对。

> ⚠ `setyaw` / `setpitch` 收的是**度**（与界面一致），写进文件的是弧度。
> 验收里那条 `setyaw:90 → 1.5708` 就是钉这个换算的 —— 写成 90 或者
> 5156 都是错的，而两种错都"看着像个数"。

### 9.7 日志锚点

```
TLED-OPEN             path= ok= keys= events= dur= readOnly=
TLED-CHECK            ok= keys= events= dur= warns= err=     ← 只在结论变化时打
TLED-SEL              track= i= t=
TLED-ADDKEY           t= keys= sel=
TLED-CAPTURE          mode=insert|overwrite t= sel=
TLED-ADDEVENT         t= kind= events= sel=
TLED-DEL              track= ... 
TLED-SETTIME          track= t= sel=
TLED-SETEASE          t= ease=
TLED-SETDUR           dur=
TLED-REORDER          track= from= to= t=                    ← 拖拽与输入框共用
TLED-DRAG-END         track= from= to= t=                    ← 只在松手时
TLED-REJECT           op= reason=                            ← 每个被拒的操作都有
TLED-SAVE             path= keys= events= force=
TLED-SAVE-BLOCKED     path= reason=read-only|no-path|check
TLED-SAVE-FAIL        path= err=
TLED-SCENE-RELOAD     path= reloaded= skipped=
TLED-CLOSE-DIRTY      path=                                  ← 带着未存改动关窗
SEQ-SAVE              path= full= keys= events= dur= bytes=   ← 由 SequenceAsset 打
```

`TLED-REORDER` 单独存在是有意的：它是**唯一**会改变"下标 ↔ 身份"对应关系
的动作，而拖拽和输入框改 t 都走它。验收里 C 组断言的就是这条 —— 不管从
哪条路改的 t，数组都必须重排、且被改的仍然是同一帧。

### 9.8 这一版**不做**的

| 想做的事 | 现状 |
|---|---|
| 在视口里拖关键帧 / 看空间曲线 | ✗ 轨道是一维的。要做需要"视口 gizmo → 时间轴"的反向通道，而那要穿过 EditorScene / ViewportPanel / Gizmo 三层（编辑器视口是自由相机，不经过 `CameraSystem`） |
| 撤销（Ctrl+Z） | ✗ 资产不在场景里，改动不进 `EditorContext` 的 undo 栈。**存盘**就是检查点，Reload 就是整体回滚 |
| 多选 / 框选关键帧 | ✗ 一次只选一个。批量挪动是下一个自然的扩展 |
| 复制粘贴关键帧 | ✗ 同上 |
| 贝塞尔曲线缓动 | ✗ 只有 `linear` / `smooth` / `step` 三档（与资产格式一致） |
| 把"当前视口机位"实时跟随成轨道预览 | ✗ 预览是"拖播放头 → 写场景相机"，反过来（拖相机 → 关键帧跟着走）需要上面那条通道 |
