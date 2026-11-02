# Venn 引擎 · 剩余任务全量评估（2026-10-27）

> **口径**：本文只回答一个问题 —— **"现在还有哪些事没做"**。
> 每一条都给三样东西：**现状**（代码里有没有）、**依据**（文件/行号/工具输出）、
> **不做的后果**。没有依据的一律标 `[待核]`，不凭印象写。
>
> **2026-11-01 更新**：第一档"收口闭环"三件（C3 存档 / E28 打包 / B6 表情）**已全部完成**。
> 下方 §0 与 §2 的相关行已就地改成现状，~~删除线~~ 表示该条已结。
>
> **2026-11-03 更新**：另开了一条**不在本文 40 项里**的线 ——
> 「玩法类型 × 打击特效」的能力评估（`docs/gameplay_fx_readiness.md`）分了三期：
> - **P1 打击链闭环 ✅** —— `.veffect` 资产 / 宿主池 / 动画事件路由 / 打击感五旋钮 /
>   6 个脚本动词 / `Effects` 面板。判据 `verify_fx` **52 条**（权威 → `docs/fx_system.md`）。
> - **P2 玩法数值与回合调度 ✅** —— 属性求值 / 状态三规则 / 伤害五步明细 / PCG32 三流 /
>   行动条（CTB）/ 存档往返 / 4 个脚本动词 / Inspector 第 14 页签 + `Debug ▸ Gameplay`。
>   判据 `--selftest gameplay` **191 条** + `verify_gameplay` **A~F**（权威 → `docs/gameplay_stats.md`）。
> - **P3-3 牌堆 ✅** —— 五区（抽牌/手牌/弃牌/消耗/**结算区 Limbo**）/ uid 身份 /
>   自动洗回 + 计数 / 手牌上限不烧牌 / 存档含 Limbo / `Debug ▸ Cards` 面板 +
>   14 条 `cards.*` 命令 + Inspector 第 ⑤ 段。判据 `verify_cards` **A~E 33 条** +
>   自检 M 组 **80 条**（权威 → `docs/cards.md`）。
> - **P3-1 配表进运行时 ✅**（2026-11-04）—— `assets/data/cards.csv` →
>   `core::CsvTable` → `gameplay::cardsFromCsv()` → `GameplayRuntime::setCardDefs()`
>   → 场景里每个 `CardDeckComponent`。脚本四动词 `tableNum/tableStr/tableHas/tableRows`
>   （键 `"表.行.列"`）。出包两道闸门：`sopt.alwaysInclude` 加 `"data/"`、
>   `kEngineAssetDirs[]` 加 `"assets/data"`。判据 `--selftest gameplay` N 组 **19 条**
>   + `verify_cards` **F 组 17 条**（权威 → `docs/cards.md` §十）。
>   ⚠ **定义是"内容"、不进存档**：读档/换场景必须重新灌定义，漏了每张牌 `known=0`
>   ——打出去什么都不发生，日志一行错都没有。
> - **游戏类型 4 种 ✅**（2026-11-04）—— 用户："游戏类型的设置应该是在启动器创建游戏
>   的时候去设置的"。启动器向导第 1 步扩到 `first_person / third_person / isometric /
>   card_battler` 2×2 四张卡，配两个新模板（8×8 棋盘 / 牌桌）。判据 `--selftest project`
>   K33 16 条 + `verify_launcher_create` **四种类型各跑一整轮**（权威 →
>   `docs/launcher_roadmap.md` §13）。
> - **P3-2 `ui2` 变换 / Scroll / Grid / 拖放 ✅**（2026-11-04）—— `Ui2Prim::RoundBoxRotated`
>   + `Ui2Xform` 三条状态栈（变换/透明度/裁剪走 scissor run）、`Scroll`/`Grid` 两个容器、
>   `dragTag`/`dropAccept` + 合法目标高亮（判据只有 `dropAccepts()` 一处）。
>   判据 UI2 自检 **132 条**（+23）+ `verify_ui_editor --only i` **10 条**（全量 56）
>   （权威 → `docs/ui_hud_roadmap.md` §17）。
> - **P3-4 伤害数字层 + 卡牌表现模板 ✅**（2026-11-04）—— `gameplay/DamageNumberLayer`
>   （池 + 合并 `37 x3` + 五种样式 + 确定性漂移，独立于 `UiDocument`）+ `gameplay/CardFx`
>   默认表 + 三个 `.veffect` + 两个新脚本动词 `castCard/drawCards`。判据
>   `--selftest gameplay` **243 条**（O 26 / P 7）、`--selftest script` 99、
>   `verify_cards` **G 组**（权威 → `docs/cards.md` §十一）。
> - **P3 全清 🎉** —— 卡牌专供四项（P3-1 配表 / P3-2 变换拖放 / P3-3 牌堆 /
>   P3-4 表现层）全部落地。**下一个大缺口 = §2 那两条硬缺口（流送 / 导航 AI）**
>   —— 注意 E30（Shipping 剔除）/ D1（语音）**早已结清**（见 §2 对应行），
>   别把它们当待办再排一遍。
>
> **2026-11-05 更新：§0 说的"2 项硬缺口"已经全部结清，本文现在没有 XL 级欠账。**
> - **G19 关卡流送 ✅**（2026-11-02，早于本日）—— `scene/LevelManifest.*` +
>   `scene/LevelStreamer.*`（`setManifest` / `attach` / `detach` / 距离驱动 /
>   卸载预算）+ 场景 `levels` 块 + `Streaming` 面板。权威 → `docs/level_streaming.md`。
> - **C8 导航 / 寻路 / AI ✅**（2026-11-05，本轮）—— `engine/nav/` 六个文件
>   （`NavGrid` 格子网 + `NavBake` 射线烘焙 + `NavSystem` 代理调度 + `NavSerialize`
>   `.vnav.json` + `NavSelfTest`）+ `NavAgentComponent` + `editor/NavPanel` +
>   九条 `nav.*` 命令 + 运行时 `setupNav/tickNav`。
>   判据 `--selftest nav` **94 条** + `tools/verify_nav.py` **66 条**
>   + `--selftest script` **C22 组 5 条**（脚本四动词）
>   （权威 → `docs/navigation.md`）。
> - 因此 §0 的"2 项硬缺口"、§2.3 的 C8 行、§2.6 的 G19 行、§6.1 的 #21、
>   §6.3 的 G19 依赖锁、附录 grep 表都已就地改成现状。**剩下的是打磨型能力
>   （A7/A8/B4/B5/B7/B8/B9/C2 残留/C5/C7/D2/D3/E27/W5/W7）。**
>
> **核对方式**：全仓 grep + 三段工具实跑（`check_feature_surface` /
> `verify_i18n_coverage` / `verify_viewport`）+ 逐份路线图状态表。**未跑全量回归**
> （项目纪律：只有明确下令才 `--all`）。
>
> 与其它文档的分工：
> - `anime_gap_review_2026-10-26.md` = 能力视角（**已过期**，A/B/C 行状态停在 10-26）
> - `developer_workflow_gaps.md` = 流程视角（**部分过期**，第 6 段"没 UI 编辑器"已被 #312 推翻）
> - `docs/*_roadmap.md` = 单个体系的任务台账（权威，但散）
> - **本文 = 跨体系的"还剩多少"总账 + 优先级**

---

## 0 · 一句话

**"存得住、发得出、跑得起来一点了"——"跑得动"（性能）还欠着。**

具体到数字（下面每条都有出处）：**约 40 项**待办，其中
**0 项是硬缺口**（~~流送~~、~~导航 AI~~、~~存档~~、~~打包~~、~~GUID~~、~~Release 基线~~
已于 10-27 ~ 11-05 结清 —— 流送 11-02、导航 11-05），
**9 项是性能与工程健康**，**7 项是验收与文档债**，
其余 21 项是"打磨型"能力（表情残留 D2 音素级、脚 IK、光照图、RenderGraph…）。

---

## 1 · 已经做完的（别重做）

按体系列一遍，避免把已完成项当欠账重新排期：

| 体系 | 状态 |
|---|---|
| 渲染底座 | GPU-driven（Hi-Z + MDI + GPU 剔除）· bindless · TAA/FXAA/DRS · 后处理链（Bloom/Tonemap/LUT/DOF/GodRays/SSR/SSGI/体积雾）· CSM · 分簇光源 · 反射探针 · 延迟贴花 · POM · SSS |
| NPR 角色 | Toon · 描边 · 面部 SDF 阴影（A1）· 头发各向异性（A4）· 眼睛 Unlit+视差+眼神光（A3）· 角色光（A5）· ramp 资产（A6） |
| 动画 | 状态机+控制器资产（A2）· 层/遮罩（B1）· 事件（B2）· Root Motion（B3）· **表情预设 + 眨眼/口型（B6，11-01）** · 两骨 IK · 弹簧骨 · LookAt · morph 全链 · 挂点 |
| 内容/叙事 | 过场时间轴 C1（+轨道编辑器 #311）· 对话 C2（+剧本编辑器 #310）· 时间轴 `Line` → 对话已接线（`SequencerSystem.cpp:348`）|
| 工具 | Prefab（#315）· 多选批量编辑（#316）· UI 编辑器最小版（#312）· 脚本导演 API 7 项（#307/#309 + `playSequence`/`showDialogue`/`playAnim`）|
| 物理玩法 | **关节 J5（10-27 夜）**：`JointComponent`（5 种类型 + 限位 + 马达）· 场景 JSON 往返（`target` 用 guid）· `buildSceneWorld` 翻译成 `physics::Joint`（三种坏法各一条 WARN）· Inspector 的 Joint 段 · Physics 面板的关节总览 · `verify_joints.py` 38 条断言 |
| 其它 | 地形/水体/粒子 · 物理 v1 · 音频 4 总线+ducking+流式 · ui2（SDF 字体）· i18n（2366 条）· 启动器 M0~M6 · 脚本 S0~S6 |

数量锚点（**11-01** 实测，命令附在括号里）：**43 个 ECS 组件**（`grep -c '^struct .*Component'
engine/ecs/Components.h`）、**37 个已注册特性**（`check_feature_surface.py` 自己枚举的数；10-27 记的
"42" 是含未注册分组的旧口径，**别拿它当基准**）、**187 个 `verify_*.py`**
（`ls tools/verify_*.py | wc -l`）、**2792 行词条**（`grep -c '^\s*{"' engine/i18n/ZhTable.cpp`）。

---

## 2 · 剩余任务总表

### 2.1 脸与材质（A 组，4 项）

| # | 项 | 现状 / 依据 | 后果 |
|---|---|---|---|
| A2 | 面部法线平面化 | 0（`pbr.frag` 无中线插值分支） | 与 A1 二选一已满足，**优先级最低**；换真实角色资产后才谈得上调 |
| A7 | 描边逐物体开关 / 遮罩 | 只有全局开关（`MYVK_OUTLINE*`） | 内眼角、鼻翼、头发内侧的线断不掉 → 近景特写立刻"游戏感"掉档 |
| A8 | NPR × CSM 的 PCF 台阶 | 未修（审计 P1-13；实测脸上 ~8 px 锯齿） | **A 组里唯一"已知缺陷"**，且现有夹具能复现 —— 想再挖一刀就挖它 |
| A9 | Posterize / 色调分离 | 0 | 廉价整体赛璐璐杠杆，可后置 |

> ⚠ A 组剩下的共同前置：**真实角色资产**（有内眼角、有刘海）。用程序化球调遮罩没有意义。

### 2.2 动画表演（B 组，剩 6 项，B1~B3 + B6 已完成）

| # | 项 | 现状 / 依据 | 后果 |
|---|---|---|---|
| B4 | 脚部贴地 IK | 0（`TwoBoneIK.h:52` 只是一句注释提到 foot lock） | 斜坡/台阶上脚悬空或插地。**胶囊级贴地已有，骨骼级没有** |
| B5 | 弹簧骨碰撞 | 0（`SpringBone.h` 无碰撞体/半径字段） | 头发穿肩、裙摆穿胸；近景必穿帮 |
| ~~B6~~ | ~~表情预设 + 眨眼/口型驱动~~ | ✅ **已做（2026-11-01）**：`anim::ExpressionPreset`（`.expr.json` 资产层）+ `ExpressionComponent` + `anim::tickExpressions`（眨眼 LCG / 口型指数包络）+ `Mixer::busLevel(Voice)` + Inspector Expression 段 + 四条控制台命令。判据 `tools/verify_expression.py` 41 条 + `--selftest expression` 49 条 | 已可"按预设名做一次表情"并跨角色复用；D2 音素级口型仍未做（见 `docs/expression_preset.md` §7） |
| B7 | 混合树进阶（1D/2D/additive） | 只有 crossfade（`ClipSwitch`） | 起步/急停/斜向走的过渡会"滑步"；待机微动（呼吸）做不了 |
| B8 | 蒙皮网格 LOD | `LOD-SKIP-SKIN`：蒙皮永远 1 级全精度 | 同屏 10+ 角色（二次元常态）性能撑不住 |
| B9 | 换装系统 | 挂点有了（`BoneAttachment`），无"部件按挂点挂/卸 + 材质继承"语义 | 服装/发型是二次元内容主体之一 |

### 2.3 内容与叙事（C 组，5 个主项）

| # | 项 | 现状 / 依据 | 后果 |
|---|---|---|---|
| ~~**C3**~~ | ~~**存档系统**~~ | ✅ **已做（2026-10-31 ~ 11-01，G17-1~3）**：`engine/scene/SaveGame.*` + `SaveSlots` 槽位面板 + H2 自动保存 + 场景 `version` 校验（在读入**之前**）。判据 `--selftest save` **101 条** + `verify_save_slots` 31 条 | 已可存读；**G17-4 用例补组仍在**（见 §2.6） |
| C5 | 实体关键帧在 Game.exe 里"活起来" | `autoplay` 字段有并落盘（`Components.h:586` / `SceneSerializer.cpp:2157`），但**运行时无消费者** | 门 / 电梯 / 机关在游戏里全是死的 |
| C7 | 剧情旗标 → 任务 | `setFlag/getFlag` 内建已有（#309，`verify_world_api` ⑤ 证明跨场景保留），但**无编辑器面板、无任务系统、对话选项改不了 flag** | 分支剧情只能"演一次"，玩家选的不算数 |
| ~~C8~~ | ~~NPC / 寻路 / AI~~ | ✅ **已做（2026-11-05）**：`engine/nav/` 五件（`NavGrid` 格子网 + `NavBake` 射线烘焙 + `NavSystem` 代理调度 + `NavSerialize` `.vnav.json` + `NavSelfTest`）+ `NavAgentComponent` + `nav/Navigation` 面板 + 九条 `nav.*` 命令 + 运行时 `setupNav/tickNav` + **脚本四动词**（`navGoto/navArrived/navFailed/navStop`）。判据 `--selftest nav` **94 条** + `tools/verify_nav.py` **66 条** + `--selftest script` C22 5 条（权威 → `docs/navigation.md`） | 街上走动的 NPC / 巡逻 / 追玩家**已可**（局部避障与动态障碍仍未做，见 `navigation.md` §十） |
| C2 残留 | 对话的六件 | 见 `docs/dialogue.md` §9：立绘 / 表情 / 语音 / 条件分支 / 历史回看 / 编辑器内预览 全 ✗ | 现在能做"有字幕的线性对话"，做不了 AVG 的"立绘 + 表情 + 语音"三件套 |

> C1 的残留较小：实体/特效轨道、视口内拖关键帧、贝塞尔曲线（`cutscene_sequencer.md` §8）。
> 事件 → 对话的接线**已经通了**（那条"❌ 只打日志"的旧描述已过期，本文顺手改了）。

### 2.4 音频（D 组，3 项）

| # | 项 | 现状 / 依据 | 后果 |
|---|---|---|---|
| D1 | 语音通道 + **播放结束回调** | ✅ **已做（2026-10-31）**：`MixStats::busVoices[]` + `Mixer::activeVoicesOnBus()` → `AudioSystem::playVoice / voicePlaying / voiceSerial`；对话侧 `DlgNode::voice` + `VoiceHooks` + `Press::WaitingVoice`（超时兜底 `max(3, 字数*0.35)`）。判据 `tools/verify_dialogue_voice.py` 30 条 + `--selftest dialogue` 语音组 38 条 | 已支持"语音播完再进下一句"；D2 口型的**前置条件已具备** |
| D2 | 口型同步（音素级） | 包络级 ✅ **已做（2026-11-01，随 B6 落地）**：`Mixer::busRms[Voice]` → `AudioSystem::voiceLevel()` → `tickExpressions` 口型通道（攻击 0.02 s / 释放 0.09 s 指数平滑）。音素级（viseme 表 / 音轨对齐）仍是 0 | 现在能"跟着音量张嘴"，做不了"a/i/u/e/o 口型区分" |
| D3 | 混响 / EQ / 音乐过渡 | 0（`engine/audio` 全仓 `reverb` 0 命中） | 室内/水下空间感、战斗音乐切换没有 |

### 2.5 渲染与世界（9 项）

| # | 项 | 现状 / 依据 | 后果 |
|---|---|---|---|
| R5-2 | lightmap / 光照探针烘焙 | 0（`engine/render` 无 `lightmap`） | 静态室内光照只能靠实时 light，性能与观感都吃亏；**E29 被它阻塞** |
| R5-3 | DDGI 动态 GI | 0 | 次要（实时 GI 已有 SSGI 顶一半） |
| W5 / R6-3 | 植被系统（独立顶点布局 + 风场 + 撒点） | 现为"每株一个实体"的临时方案（`FoliageEditor.h` 头注释自认：**几千株没问题，上万株要上真系统**） | 大草原 / 花海做不了；CPU DrawItem 随株数线性涨 |
| W6 | 室内光照（探针当辐照度源） | 0 | 室内场景"灰一片" |
| W7 / R6-5 | 动态天空（Hillaire 大气 + 昼夜） | 0 | 没有时间变化 → 二次元的"黄昏/清晨"氛围镜头做不出来 |
| W9 | GI 烘焙 | 明确后置 | — |
| R7-1 | 声明式 RenderGraph | 0（只有"前向之后 5 个 pass"的 `FrameGraph`） | 手写屏障、无 pass 裁剪、无资源别名；**E27 被它阻塞** |
| R7-2 | 细粒度同步 + 异步 compute | 重建路径 `waitIdle` 全局停机 | 卡顿尖刺来源之一 |
| R7-3 | Ray tracing 后端 | 0 | 远期 |

### 2.6 持久化（3 项，全部未开工）

| # | 项 | 现状 / 依据 | 后果 |
|---|---|---|---|
| **G16** | 稳定 GUID | **✅ 已完成 1~5（10-27）**：`World::Record.guid` + `m_byGuid` + 场景 `guid`/`parentGuid`/`v2` + `Scene::reconnectByGuidOrName` + **G16-5 三段拆分**（`SceneDocument` / `parseSceneDocument` / `instantiateDocument` / `resolveDocumentAssets`）。自检 26 条 + 39 条 + 端到端 8 组 + 14 组全绿 | **G17/G18/G19 的共同前置** —— 已解锁 |
| **G17** | 存档（增量 + 槽位 + 迁移 + 原子写） | **◑ G17-1~3 完成（10-27 / 10-31）**：文件层（格式 / 原子写 / v1→v2 迁移）＋**应用层**（基线哈希 / 增量 diff / `applySaveGame` / 悬挂父修正 / 漂移拒读）。自检 **101 条** + 端到端 **31 项**全绿。G17-3（槽位 + CLI + `captureBaseline` + F5/F9 + **存档槽位面板**）**10-31 全部落**；剩 G17-4（用例补组） | 与 C3 同一件事。**G17-3 是下一个能直接开工的点** —— `--save/--load/--slot` 一接上，C3 就从「库」变成「玩家能按的功能」 |
| ~~G19~~ | ~~关卡流送~~ | ✅ **已做（2026-11-02）**：`scene/LevelManifest.*` + `scene/LevelStreamer.*`（距离驱动挂载/卸载 + 每帧卸载预算）+ 场景 `levels` 块 + `Streaming` 面板。判据 `tools/verify_levels.py`（31 条内核 + 6 组界面）（权威 → `docs/level_streaming.md`） | 大关卡可分段加载；**跨段导航网格尚未拼接**（每段各烘各的，见 `navigation.md` §十） |

### 2.7 编辑器与工具链（6 项）

| # | 项 | 现状 / 依据 | 后果 |
|---|---|---|---|
| E20 | 覆盖指示器 + Reset to inherited | 0（`FeatureUIGen.cpp` 无 `Overrides`/`inherited`） | 有 5 层覆盖链却看不出"这个值从哪来" —— 调渲染参数时反复自问 |
| E22 残留 | 对齐/分布 · 循环选择 · 吸附可视化 | 多选/框选/批量变换已做（#316），**对齐分布、循环选择、吸附网格线没有** | 关卡搭建"能批量搬"但"不能对齐"，摆一排灯还得手填数字 |
| E23 残留 | 快捷键总览 + 可重绑定 | `KeyBindingsPage`（G06 的 UI 面）已有；**跨命令的统一快捷键表 / 冲突检测 / 总览窗口**未做 | 键位散在 `handleShortcuts()` 里，加一个命令要改两处 |
| E24 | 资产编辑器框架（材质节点图 / 曲线编辑器） | 0 | **明确不建议现在做**（二次元材质是参数化的，见 §7） |
| E27 | RenderGraph 可视化 | 0，**被 R7-1 阻塞** | 依赖链上最末端 |
| **E28** | **打包 / 发布 UI + Shipping 剔除** | ✅ **2026-10-28 全通**：E28-1 出包 + G29-a 闭包 + G29-b/c/d `game.pak` + **G30 `make ship`**（去编辑器/去校验层/开发命令编译期剔除/pdb 归档） | 从"整目录拷给别人 + 让对方装 VS"进步到"**一个 exe + 一个 pak**"，且出货二进制里没有任何调试设施（判据 `verify_shipping` 39 条） |
| E29 | 烘焙流水线 UI | 反射探针那半可做（R5-1 已落地），**光照图那半被 R5-2 阻塞** | 半可用 |

### 2.8 UI（5 项）

| # | 项 | 现状 / 依据 | 后果 |
|---|---|---|---|
| 立绘系统 | 0 | `SubtitleBox` 只有"名字 + 正文 + 选项" | AVG 骨架缺一半（背景 + 立绘 + 表情切换 + 位移） |
| 控件类型 | 6 种（panel/nine/image/text/button/progress） | `ui2/UiDocument.h` | 没有 list / grid / scroll / slider / toggle → 背包、编队、设置页做不了 |
| UI 动画 | 0 | 无 tween / 无状态 | 弹出、滑动、淡入全没有 |
| 数据绑定 | 只有 `UiBindings`（血量等少数量化量） | `UiDocument.h` | 列表项、循环绑定、条件显隐没有 |
| 运行时预览 | 0 | `UiEditorWindow.h` 头注释第 6 条明说"不做边编边跑" | 改控件要来回切窗口 |

### 2.9 脚本（1 项小残留）

- ~~**`openUI`** 缺席~~ → **✅ 2026-10-31 落地**（C6 第七件）。`kBuiltins[]`
  加 `{"openUI", 1, 1}`、`ScriptWorldActions::openUI`、`RuntimeApp::openUiDocument`
  三处同步；语义四条：路径**相对 `assets/`**（内部走 `resolveAssetPath`）、
  **空串 = 关掉当前那块**、失败**保住当前那块**并返回 false、报错要分清
  "宿主没装 UI 层"（`not available in this host`）与"资产加载不了"
  （`UIDOC-OPEN ... ok=0`）。判据：自检 `C20a~d`（4 条）+ 端到端
  `verify_director_api.py ⑦~⑩`（4 条）。
- **无断点调试**：只有 `print` → Console，协程里的状态看不见。
  （⚠ 顺带修掉一个真 bug：同一帧里 `print` 之后紧跟一个运行期错误时，
  报错那条 `continue` 会**跳过 print 的排空**，那行 print 直接消失 ——
  而它恰恰是排查现场最想要的一行。见 `ScriptRuntime.cpp` 的 `flushPrints`。）

---

## 3 · 性能优化专项（"跑得动"这一条）

这一节单独拎出来，因为**目前所有"性能数字"都是 Debug 的**，谈不上预算。

| # | 问题 | 证据 | 量它的办法 | 改法 |
|---|---|---|---|---|
| ~~P1~~ | ~~**没有 Release 基线**~~ → **✅ 11-01** | `make perf-build` → `build-rel/`（RelWithDebInfo，与回归用的 Debug `build/` 分开）；`tools/bench_baseline.py` 采集 | 2 场景 × 3 分辨率 × 两种负载（Game.exe / 编辑器） | ✅ 表在 `docs/perf_baseline.md`。**实测结论：帧时间 50% 在 `forward`、随分辨率线性涨 = 填充率；Debug↔Rel 只差 8% → 这两份场景不是 CPU 受限** |
| P2 | **每帧每骨 `glm::decompose(restLocal)`** | `engine/animation/Pose.cpp:62` | `ANIM-COST` 锚点（A8 已设计未落地） | rest 分解是常量 → 预计算进 `Skeleton`，只算一次 |
| P3 | ~~**多角色 buffer 扩容首帧卡顿**~~ → **✅ 10-31** | `updateSkinning` 两遍 `each`：先量总量再扩一次（余量 25%），循环内只做"溢就跳过" | `verify_skin_capacity.py` 5/5（16 角色 / `need=1040` / `ANIM-SKIN-CAP` 每槽一次） | 一进场景一次到位，不再"边填边扩" |
| P4 | **蒙皮没有 LOD**（B8） | 见 §2.2 | 10 个角色同屏的 GPU 时间 | 按 Model 整体选层 + 专用管线 |
| P5 | **植被是"一堆实体"** | `FoliageEditor.h` 头注释自认上限"几千株" | 撒 5k / 20k 株测 CPU 提交时间 | W5：独立顶点布局 + 实例缓冲 + 视锥剔除 + 密度衰减 |
| P6 | **资产永不淘汰** | 全仓 `evict` 0 命中，4 张资产表只增不减 | 连续编辑 30 分钟看显存曲线 | 引用计数 + LRU + 一帧末回收（注意 CB 录制期不能销毁，见 `render_resource_lifetime.md`） |
| P7 | **Undo = 整场景 JSON 快照** | `EditorContext.h:516` `structuralEdit` 抓前后两份完整 JSON | 1000 实体场景单步撤销耗时 | 改增量命令（组件级 patch） |
| P8 | **重建路径 `waitIdle` 全局停机**（R7-2） | 无异步 compute / 无 timeline semaphore | 切分辨率、改画质时的长帧 | R7-1 之后再做 |
| P9 | **无 device-lost 恢复** | `VK_ERROR_DEVICE_LOST` 只有注释，0 处恢复逻辑 | 拔驱动 / TDR 复现 | 检测 → 重建全部 GPU 资源（大工程，可后置） |
| P10 | **进 Play = 整场景 JSON 往返** | `EditorContext`/`EditorApp` 的 Play 路径 | 大场景按 Play 的等待时间 | 改成"世界快照 + 差异回滚" |

> 排在前面的三项（P1/P2/P5）共同点：**改起来小、收益能立刻量到**。
> P6/P9/P10 都是"改一次动一大片"，必须先有 P1 的基线，否则改完不知道好坏。

---

## 4 · 工程健康（不挡功能，但拖慢一切）

| # | 问题 | 证据 |
|---|---|---|
| H1 | **上帝类** | `Renderer.cpp` 7259 行 · `InspectorPanel.cpp` 8699 · `EditorApp.cpp` 8321 · `SceneSerializer.cpp` 3911 |
| H2 | ~~**崩溃即丢档**~~ → **✅ 10-31**：`tickAutoSave` 写同目录 `<场景>.autosave.json`（**不覆盖**正式档）+ 启动 `EDIT-AUTOSAVE-STALE` 提示（**不自动加载**）；`verify_autosave.py` 9/9 | 10-23 真实发生过一次 |
| H3 | **两套 FBX 管线不一致**（运行时权重截断 vs 离线归一化） | `anime_gap_review` §2.E.5，未动 |
| H4 | **零单元测试** | 153 个用例全是 GUI 端到端，一轮 30 分钟+ |
| H5 | ~~回归有 2 条已知红~~ → **已清零（10-27 晚）** | 见 §5 / §5.1：四条红全部修完（其中 `verify_material_*` 是真 bug，`verify_content_three_pane` 是"节点被滚出可视区"的夹具问题） |
| H6 | **flaky 4 条** | `bvh` / `csm` / `ibl` / `particles` 偶发全红，单跑 PASS（**别补跑凑干净**） |
| H7 | **无 CI** | 全本地跑；改 `engine/**` 忘了重编 Editor.exe 是常见事故（#315 踩过） |
| H8 | **Play 没有 `ContactTracker`** | 编辑器里做碰撞玩法只能用距离判定（`script_roadmap` 唯一遗留） |

---

## 5 · 验收与文档债（本轮实测，数字是真的）

> **2026-10-27 晚收口**：下面 V1~V4 四条**已经全部修完并复跑验证**，
> 根因与修法见 §5.1。这四条原来一直被当成"已知红"记着，实际只有 V4 是
> 真 bug（产品功能不可用），V1/V2 是记账债，V3 是**夹具陈旧**。

| # | 项 | 本轮实测（10-27 白天） | 现状（10-27 晚） |
|---|---|---|---|
| V1 | `check_feature_surface.py` | **FAIL：45 条**（21 裸 env + 24 诊断项） | ✅ **PASS**：裸 env 0 条；特性 37/37 有 UI 面（25 条 `*_DBG` 仍只是 WARN） |
| V2 | `verify_i18n_coverage.py` | **FAIL：102 项缺中文**（A1=5/B=1/B2=95/E=1）；孤儿 280 / 重复键 24 | ✅ **PASS**：0 项缺中文；词典 2229 → **2331 条**；孤儿 285 / 重复 24（仅提示） |
| V3 | `verify_viewport.py` | **FAIL（2 项）**：`V1d` / `V3` | ✅ **PASS（42 项）**：`CONVERGED_VP` 与基线图按当前布局重抓 |
| V4 | `verify_material_roundtrip` | 三槽拖放全没落下 | ✅ **PASS**：三槽全部绑上、落盘、读回来 |
| V5 | `verify_character_import` G1 | 今日已修（窗口 1440×1200） | ✅ **PASS**（10-27 晚复跑：G3 时间轴存盘、像素差 7.572%、零 VUID） |
| V6 | 文档失真 | `cutscene_sequencer.md` §8 说"事件只打日志"（已接对话）；`developer_workflow_gaps.md` 第 6 段说"没 UI 编辑器"（#312 已做） | ✅ **两处都已修**（后者按 10-27 现状重写 §6 + 三张表） |
| V7 | 任务台账陈旧 | TaskList 里 32 条 pending 中混着已完成项（A0/U6~U8/G14 等） | 🔶 **部分清**：按硬证据标完 12 条（pending 32 → **21**）—— A0-2~A0-6、A4 验收、B1 四刀、A1/A3/A5/A6 收口、主题系统、U7。**没敢动的**：U8（`config/ui.json` 全仓不存在，Hud/PauseMenu 仍硬编码）、G14-4~9、物理 #27/#28、编辑器杂项 #19~#22/#30~#36 |

### 5.1 这四条到底红在哪（根因，别再踩）

**V1 特性面 21 条"裸 env"** —— 闸门红着 ≠ 有一堆新特性没 UI。21 条里
**14 条是测试/自检/日志探针**（`MYVK_ASSET_TEST`、`MYVK_LAYOUT_TEST`、
`MYVK_LOD_OFF`、`MYVK_LOG_DT`…），**6 条是描边参数**（真特性，只是从没
在离线表里登记过），**1 条是 `MYVK_X`** —— 它在 `RunMode.h` 的**注释**里
当例子写，扫描是纯正则、不认注释。修法分别是：白名单（带理由）、新增
`id="outline"` 特性项、把注释里的例子改成 `MYVK_<NAME>`。
> 教训：**"闸门红了"要先分清"真的漏"还是"记账没记"**。全都是记账时，
> 补完一次要顺手加注释说明为什么它不需要 UI —— 否则下一轮又会被当成欠账。

**V2 i18n 102 条** —— 全是 10-26/10-27 新加面板的文案（Prefab、弹簧骨、
双骨 IK、注视、挂点、对话、多选、卡通材质 tooltip），`TR()` 包了但词典没
条目 → 中文界面下静默显示英文。补 102 条（含 20 条多行 tooltip）。
另外两条 **不是**漏翻、是**不该翻**：`LAYOUT-TAB-%s`（E29-3 的 logRect
锚点）与 `PREFS-THEMECHIP-%d`（色卡按钮的 widget ID）→ 进 `FMT_KEEP`。

**V3 视口 2 项** —— 夹具陈旧，不是回退：
- `CONVERGED_VP` 还写着左栏 300px 时代的 `(305,145,1241,778)` → 左栏下限
  抬到 460 后**永远不收敛**（每次启动都 WARN），
- `VP_INNER=(330,200,1200,750)` 是绝对坐标 → 视口左边界挪到 463 后它整块
  掉到视口外面，于是 `V1d` 长期红着、报的话却与真因无关，
- 基线图 `tools/vp_nohud_baseline.png` 是 **E14 时期（浅色主题）**的截图，
  默认主题换成二次元深色档之后它守的是"两年前那张图"。骨架里第三条
  `1574` 根本不是面板分隔线 —— 是**浅色主题下才看得见的窗口右边框**。
修法：`CONVERGED_VP` 按实测更新；`VP_INNER` 改成 `vp_inner(VP-RECT)` 相对
推导；`V1d` 拆成两条真断言（视口够大 + 左边界压在真实分隔线上）；基线按
"当前默认布局 + 默认主题"重抓。
> 教训：**"期望值过时"和"实现回退"在日志里长得一样**，都要先看**这个量
> 靠什么成立**。这里三个量全都钉在"某个历史时刻的像素坐标"上。

**V4 材质三槽拖放（真 bug）** —— 浮层 Content 抽屉**横跨整窗**，而右栏
Inspector 的七个材质纹理槽在抽屉**后面**：拖拽源是好的（预览窗跟着鼠标
走），但落点被抽屉吃掉，接收端 `IsItemHovered()` 永远 false。见
`_venn_verify/probe_drag/1_armed.png`（抽屉盖住整块材质区）。
修法：**浮层抽屉让出右栏**（`L.content` 右缘 = 右栏左缘），停靠形态不变。
> 这是本轮唯一的产品行为变更，也是唯一"用户真能感觉到"的一条。

### 5.2 2026-10-31 全量回归：167 例 42 红，**产品 bug 只占 1 条**

这一轮的教训值得单列，因为它决定了"看到一片红该怎么下手"。

| 类别 | 条数 | 根因 | 修法 |
|---|---|---|---|
| **宿主 safe-delete 配额** | 9 | 脚本 `finally: os.remove()` 被宿主硬杀（按回合累计 50 个），断言早已全过却没有 `结果:` 行 | 全域改 `safe_fs`：34 个脚本 / 63 处调用点，`verify_*.py` 现在 **0 处裸删除** |
| **报告契约不完整** | 3 | 只打 `=== N/M PASS ===`，闸门只认 `结果:` | 补一行；并在 `run_regression` 里打印 `← rc=0 但没有 结果: 行` |
| **外部资产缺失** | 5 | `animate_girl.fbx/.vmesh` 的贴图是**绝对路径** `E:/dev/UE-project/…`，目录已不在 → 引擎必打 `[ERROR]` → "零 ERROR"必红 | 新增 `tools/prereq.py`（扫模型里的绝对路径，不写死盘符）+ 闸门的 `SKIP` 档 |
| **夹具没跟上 v1→v2** | 2 | "读→写往返逐字段相等"报 374 处不同、**全是 `/guid`**：源场景 v1 无 guid，dump 走 v2 写端会补 | 显式断言"每个实体都补了 guid + version 升到 2"，再剔掉 guid 逐字段比（复跑 **12/12**、**18/18** PASS） |
| **词典涨了、SDF 图集装不下** | 1(3 项) | `ZhTable` 1289 字，图集在 2048² 处 97.6% 溢出；`gen_sdf_atlas.py` 默认正方形 | 高度**自动翻倍重排** + 重生成；`verify_ui2` PASS |
| **真红** | **1** | `verify_dialogue` ⑨。第一版以为是"q1 的 reveal 窗口只有 0.42 s、机器忙就踏空"，把慢放从 0.5 调到 0.25（窗口 0.84 s）之后**照样红**。真正原因是**合成按键会静默丢失**：两对 down/up 挤进同一轮 `glfwPollEvents`，帧边界上净变化为 0，`wasPressed` 一次不为真。证据：`DLG-LINE id=q1` 之后 30 帧里**一条 `DLG-PRESS` 都没有**，而 Python 侧 `taps` 确实从 4 涨到 6（发了两下空格） | 判据不再依赖"第 N 下按在哪"：①在打字窗口里多发几发（0.20 s 节拍、两发之间留 0.14 s 静默）②一看见 `DLG-PRESS reveal` 或选项面板就撒手 ③用新加的 `DLG-KEY id=q1 atChoice=0` 证明"至少一发真送到了"。复跑 **11/11 PASS**（`keys_q1=2(atChoice=0:1) reveal_q1@470 pick@509`） |

> 顺带修掉一条**永远为真**的告警：`stale_build_warning` 把 `runtime/*.cpp` 拿去和
> `Editor.exe` 比 mtime，而它只链接进 `Game.exe` → 每次改运行时代码都弹"大概率
> 没重新构建"。改成按归属产物比（`runtime/` → `Game.exe`、`assets/shaders/` →
> `.spv`、其余 → `Editor.exe`）。一条永远为真的告警等于没有告警。

> **三条纪律**（都是从这轮抽出来的，不是从文档抄的）：
> 1. **"闸门红"先分类**：真漏 / 记账 / 夹具陈旧 / 环境。这一轮环境占了 14 条，
>    夹具 2 条，记账 3 条 —— 按"42 条红"去排期会去修一堆不存在的问题。
> 2. **"日志里全是 OK 但闸门报 FAIL" 是体系问题，不是偶发**：把闸门的判定
>    契约（`结果:` 行）和被删配额一起当成基础设施来维护。
> 3. **判据的可靠性不能取决于机器忙不忙**。一条"20%~40% 概率踏空"的断言
>    等于没有断言 —— 它每轮都会红一次，而每次红的都是同一件事。
>    进一步的推论（⑨ 最终查出来的）：**别拿"我发了 N 次输入"当证据**。
>    合成输入没有送达保证，能当证据的只有**引擎侧打出来的锚点**。所以这一刀
>    加的不是"再多等一会儿"，而是一条 `DLG-KEY` —— 它把"按键根本没送到"和
>    "送到了但状态不对"这两种**在日志上一模一样**的情况分开了。

> **2026-11-01 结账**：42 条红**全部清零**（含环境 SKIP 档）。其中 41 条在
> 10-31 当天修完，最后一条 `verify_dialogue ⑨` 与随后浮出来的
> `verify_frame_pacing`（④⑤ 见 §5.4）在 11-01 收口。六个"改过但没复跑"的
> 用例（`hierarchy_status` / `world_api` / `landscape_ground` /
> `editor_play_terrain` / `player_feel` / `player_jump`）复跑全绿。

### 5.3 同一轮的另外两条：滚出视野的按钮、以及"日志在撒谎"

**① `verify_camera_move` 的 B6/B7 报"点了没反应"，不是产品 bug**

`B6` 报 `dyaw=0.00`、`B7` 报 `n=0`，看起来像"增量算错了 / Bake 没接线"。
实测矩形（1600×1000、`CINE-WIN` 底边 y=886）：

| 控件 | y 范围 | 结论 |
|---|---|---|
| `CINE-VERB-PAN` | 844 … 874 | 在窗口内 → **能点** |
| `CINE-VERB-PAN-NEG` | 881 … 911 | 出界 5 px → 点不动 |
| `CINE-BAKE` | 1037 … 1067 | 出界 150 px → 点不动 |

滚进视野之后两个都能点（`verb 0→1`、`bake lines=1`）。**`logRect` 不判可见性**
（只按坐标去重），所以"有矩形却点不到"是本项目的结构性坑，不是这次的偶然。
修法不是给这两个按钮各加一次滚动，而是把它做成设施：
`launcher_kit.scroll_into_view()` / `click_in_panel()`，面板类用例一律走后者；
`verify_camera_move` 另加一条 `B6a`，把"点不到"和"点了没效果"**分成两句报**。

**② 否定测试没变红，也是结论 —— 它暴露了判据的洞**

混合曲线这一刀写了 `CAM-CUT-RT … curve=` 并断言 `C5d`。否定测试把
`blendCut(b, cv)` 改回 `blendCut(b)` 之后 **57 条全绿** —— 因为日志打的是
调用方手上的 `cv`，而不是相机真正用的那个。这是"日志与执行不同源"。
改成从 `CameraSystem::beganCurve()` 取之后，同一处否定测试**只红 C5d 一条**。

第二处否定（执行侧恒走 `Smooth`：曲线存了、日志也对、画面不对）**G5 照样绿**，
只有 `G7b/G7c/G7d` 红 —— `smooth` 在中点的值恰好等于 `linear`，所以"钉一个
中点"永远分不出这两种实现。**新断言必须先想清楚"两种实现会在哪里不一样"**。

**③ "锚点存在"不等于"控件在可视区内"**

属性栏加了一张 56 px 的曲线预览图，`propH` 跟着从 148 抬到 214。只断
"`TLED-SHOT-CURVE-PLOT` 在日志里"是不够的 —— `logRect` 不判可见性，被 Child
裁掉的控件照样有矩形（`engine/ui/DebugRects.h` 的文件头就写着这条）。所以
`B8b` 比的是**矩形包含关系**：预览图与 `Use keys` 的下沿都必须 ≤ 属性栏下沿。
⚠ 第一版 `RECT_RE` 按 `(x0, y0, x1, y1)` 写，而真实格式是
`TAG=(x0,y0)-(x1,y1) center=(cx,cy)`（`-(` 分隔、无空格）→ 三个值全 None，
**而 B8 那条存在性断言照样绿**。这是同一个病的两种形态：*断言能过 ≠ 它测到了东西*。

### 5.4 2026-11-01 收口复跑：6 个待复跑用例全绿，但顺手挖出 4 个真缺口

复跑 `hierarchy_status` / `world_api` / `landscape_ground` / `editor_play_terrain` /
`player_feel` / `player_jump` —— **全部 PASS**。但 `editor_play_terrain` 的那一行
收尾把一整类问题暴露了出来，顺着查下去牵出四条，每条都不是"断言红了"那么简单。

**① 报告契约：`DEFAULT` 里 2 个脚本全绿也判红**

`run_regression` 的 `mark = PASS if (rc==0 and ok) else FAIL`，其中 `ok` 只由
`OK_RE` 扫到收尾行才为真。而：

| 脚本 | 收尾行 | 后果 |
|---|---|---|
| `verify_editor_play_terrain` | `9/9 PASS` | `OK_RE` 不认 → **全绿判红** |
| `verify_frame_pacing` | `==== N passed, M failed ====` | 同上，**而且把 rc 丢了** |

`frame_pacing` 还多一个毛病：`rc = main()` 拿了却没用（`sys.exit(0 if g_fail == 0 else 1)`），
而 `main()` 会在"起不来 / 拿不到帧"时 `return 1..7` 提前退出，此时 `g_fail` 还是 0
→ **一行都没测的运行被判成功**。两个都补了 `结果:` 行，`frame_pacing` 改成先看 rc。

**守卫**：`check_regression_map.py` 新增 **[G] 报告契约** 段，扫 `DEFAULT` 里每个
脚本的源码，缺收尾行的点名报红。修之前它点名了上面 2 个，修之后 0 个 —— 这
就是这条守卫的否定测试。⚠ 判据故意用**字面量包含**而不是正则：本机
`bash heredoc` 会把正则里的反斜杠吃掉（`\s` → `/s`），第一版扫描因此报了
39~45 个假的"缺收尾"。

**② `PLAYER-STATE` 加了字段，夹具没跟上 → 两条用例从来跑不到断言**

引擎在 2026-10-24 给 `PLAYER-STATE` 插了 `arm=` / `cam=`（"贴墙时镜头钻进
墙体"的证据），位置**在 `pitch` 和 `miv` 之间**；而 `verify_player.STATE_RE`
是 `pitch=..(?: miv=(\d))?(?: cap=(\d))?` —— 中间多了两段，`cap` 那个可选组
**一条都匹配不上**，`group(9)` 恒为 `None`。

连锁后果：
- `capture_ready()` 等 8 秒拿不到 `cap=1` → `enter_play()` 返回 `vp=None`
- `verify_frame_pacing` 报 `[FAIL] 进 Play + 点视口接管` → `rc=3` 提前退出
- `verify_player` 报"探针 cap=1 拿不到"

从外面看像 **P8 的鼠标接管坏了**，实际是夹具陈旧，产品一行没错。

修法用**非捕获组**（`(?: arm=[\d.]+)?` / `(?: cam=\([^)]*\))?`）：分组编号
1~9 一位不动，几十处 `group(2/3/4/8/9)` 一个都不用改。正反两向都验过：带
`arm/cam` 的与老的 `miv/cap` 行都能解析出 `cap=1`。

**③ `capture_ready()` 用 PostMessage 合成点击 → 改成真实 `mouse_event`**

修完 ② 之后它开始"连跑 5 次全过、紧接着连跑 3 次全挂"。根因是项目里早就
记过的一条：**P8 的接管是 GLFW 在看真实按键/光标状态，合成消息它不认**。
改成 `force_foreground()`（`verify_asset_drag.py` 里那套 attach-thread-input
+ ALT 兜底）→ 真实 `mouse_event` → 光标预热两次 MOVE。之后连续 3 次全过。

**④ `MYVK_VSYNC=0` 在这台机器上是死开关（已修到"能诚实地说不行"）**

`Swapchain.cpp` 原来只试 `MAILBOX`，没有就一路退到 `FIFO`。本机 surface
实测只给 **`[IMMEDIATE, FIFO]`** —— 没有 MAILBOX，于是 `vsync=0` 打出
`present mode = FIFO (vsync=0)`：**开关看着生效了，帧率还是锁着的**。

补了 `IMMEDIATE` 兜底，并把日志从"FIFO/MAILBOX 二选一"改成**真实模式名 +
可用列表**（原来的写法一旦选中 IMMEDIATE 就会谎报成 MAILBOX，而这一行正是
判据唯一的数据源）；解不了锁时额外 WARN 一句点名可用列表。

⚠ 但**实测 IMMEDIATE 在本机并不更快**（34 fps vs FIFO 34 fps，Windows 的
DWM 会把它压回来）。所以 `verify_frame_pacing` 的 C 组改成三档：
真跑满 >200 fps → 通过；拿到 MAILBOX 却没跑满 → **红（真回归）**；只有
`IMMEDIATE/FIFO` 且跑不满 → **SKIP** 并把可用列表念出来。**测不了就说测不了**。

**⑤ 判据重标定：三条红的量根本不是"能不能测到"**

`frame_pacing` 修好 ②③ 之后露出 4 条红，逐条量过之后结论是——**没有一条是产品回归**：

| 断言 | 老口径 | 实测 | 结论 |
|---|---|---|---|
| A3 | `sd/mean < 8%` | 44% | 8% 来自"空场景 MAILBOX 470 fps → FIFO"的基线；现在带 SSAO/体积雾/分簇/CSM/TAA 只有 31~59 fps，且**两次会话之间差 60%** |
| A4 | 尖刺必须 0 个 | 5/64 | 停顿是**每 15 帧一次 55~73 ms** |
| B2 | `ΔanimT/dt ≈ 1` | 1.54~1.76 | 前提被两个**设计**推翻（见下） |
| C1 | 不锁 >200 fps | 33~55 | 本机解不了锁（见 ④） |

**A4 的决定性实验**：进 Play 后**先静止 2 秒**再走 —— 静止组的尖刺位置是
`idx 5,20,35,50,65`，移动组是 `idx 37,52,67,82`，**间隔都是 15 帧，毫秒数
一模一样**，锁定/不锁定两组也一模一样。也就是说：这些停顿既不来自玩法逻辑，
也不来自呈现模式。（机器空闲时同一份二进制只掉 1/121 帧 —— 它随**系统负载**
变化，是环境噪声。）

**B2 的两个"设计"**：① `Locomotion` 的**步幅匹配** `playbackRate`（走 3.0 m/s
对应 1.5~1.6，防滑步）；② `AnimatorSystem` 的 **A4b 相位速率曲线**
（`time = pc->advance(phase, dt*speed/dur)*dur`，一个步态周期内重新分配速度）。
继续要求 ≈1 等于要求"把滑步改回来"。

重标定后的判据（阈值都从实测数据里取，两边留余量）：
- **A3** → **p90 / 中位 ≤ 1.8**，并掐掉起手 0.5 s 预热。用 p90 不用 p95：
  周期性停顿的密度正好是 **6.7%**，p95 恰好落在停顿上（恒 2.3~2.5），
  p90 落在正常帧上（1.10~1.23）；而"20% 的帧塌陷到 100 ms"会把 p90 顶到
  **3.51** —— 两边都留足。
- **A4** → 尖刺 < 10% 的帧，并且把**中位间隔**打出来（"每 15 帧一次"和
  "散落各处"是两种完全不同的病）。
- **A5（新增）** → **静止 2 s 与移动 2.6 s 的尖刺数相差 ≤ 2**。这是本轮
  唯一净增的判据，也是把"环境噪声"和"玩法把帧拖垮"**永久分开**的那一刀。
- **B2a** → 负跳只允许是循环回绕（幅度 ≥ 0.3 s）；**B2b** → 中位倍率落在
  步幅匹配的合理区间（0.2~4.0，抓"动画冻结"和"倍率爆炸"）。

**判据形式自检**：`tools/check_pacing_judge.py`（不启动编辑器，喂合成帧序列）
证明三条新阈值**都能真的红**：A3 本机噪声 1.11 绿 / 20% 塌陷 3.51 红；
A4 6.2% 绿 / 20.3% 红；A5 持平绿 / 4-vs-12 红。
⚠ 写它的理由：阈值是从实测数据上定的，**如果它们其实怎么都能过，那和没写一样**。

---

## 6 · 建议优先级

排序原则：**先解开下游多的，再解开"看得见"的**；同一档内按"改动能被量到"排。

### 第一档 · 收口闭环（做完这三件，引擎才"能用"）

> ✅ **三件全部完成**（2026-11-01 收口）：#1 C3+G16 → `--selftest save` 101 条 / `verify_save_slots` 31 条；
> #2 E28 打包 → `verify_package` 76 条；#3 B6 表情 → `--selftest expression` 49 条 / `verify_expression` 41 条。
> 下一档从「第二档 · 性能与工程」开始 —— **P1 Release 基线也已结**（11-01，`docs/perf_baseline.md`）。

| 序 | 做什么 | 解开 | 理由 |
|---|---|---|---|
| 1 | **C3 存档 + G16 GUID**（合并一轮：G16-1 先落 `World::Record.guid`，再 G17-1~4） | C3 / G17 / 部分 G18·G19 # 见 §2.6 | 现在做的东西**存不下来**。且 G16 是流送/引用的共同前置，晚做返工越大 |
| 2 | **E28 打包 / 发布**（含 `VENN_SHIPPING` 剔除 + 资源裁剪） | 第 8 段流程 | "最后 1 公里"。Console/GPU Timeline 原样进发行包不可接受 |
| 3 | ~~**B6 表情预设**~~（D1 语音回调 ✅ 2026-10-31） | 二次元脸的表演 | ✅ **已完成 2026-11-01**：`.expr.json` 资产层 + `ExpressionComponent` + 眨眼/口型驱动 + Inspector 段 + 四条命令。权威 → `docs/expression_preset.md` |

### 第二档 · 性能与工程（让上面三件不返工）

4. ~~**P1 Release 基线**~~ ✅ **已完成 2026-11-01**（`make perf-build` + `tools/bench_baseline.py` + `docs/perf_baseline.md`）
5. **P2 rest 分解预计算 + P3 buffer 预留**（A8 的原计划，小改大收益）
6. **V1/V2 两条红**（特性面 45 条 + i18n 102 条）：都是"记账"，但它们是**准入闸门**，红着就意味着"新特性有没有 UI 面"这件事没人查
7. **H1 拆 `Renderer`**（只在动渲染时顺手切，不要专门开一轮）

### 第三档 · 能力打磨（有真实资产之后再谈）

8. A8（NPR×CSM 台阶）→ A7（描边遮罩）→ B4（脚 IK）→ B5（弹簧骨碰撞）
9. C8（NavMesh + 巡逻）· W5（植被）· W7（动态天空）  ← **C8 已于 11-05 结清（格子导航）**
10. E22 残留（对齐/分布）· 立绘系统 · UI 控件扩充

### 6.1 可执行排名（1~21）

三个过滤器**按顺序**用，不能跳步：

1. **硬阻塞** —— 它不做，下游一律做不了 → 自动进前十（拓扑优先于性价比）；
2. **返工斜率** —— 越晚做要迁移的东西越多（GUID 引用、`VENN_SHIPPING` 宏、存档格式版本、
   表情通道、材质位掩码）→ 这类"今天省一天、明天还一周"的提前；
3. **性价比** —— `(解锁下游数 × 2 + 玩家/开发者可感知度) / 工作量`。

> 量级：**S** < 0.5 天 · **M** 1~2 天 · **L** 3~5 天 · **XL** 1 周+
> （粗估，只用于排相对顺序，不当作承诺工期）

| 序 | 项 | 量级 | 为什么在这个位置 | 前置 → 解锁 |
|---|---|---|---|---|
| 1 | **G16-1 稳定 GUID**（`World::Record.guid`） | S~M | ✅ **已完成 10-27**（含 G16-2/3/4）：自检 26 条 + `verify_guid.py` 8/8 | — → C3 / G19 / 跨场景引用 |
| 2 | ~~**C3 + G17-1~4 存档**~~ | L | ✅ **G17-1~3 全部落地**（含 10-31 的槽位面板 + H2 自动保存）：`--selftest save` 101 条 + `verify_save_slots` 31 条 + `verify_save_panel` 8/8；G17-4 用例补组仍在 | #1 → C7 / G19 / 一切玩法 |
| 3 | ~~**P1 Release 基线**~~（`RelWithDebInfo` + 基准表） | S | ✅ **已完成 2026-11-01**：`build-rel/` 与 Debug 的 `build/` 分开；实测 **Debug↔Rel 只差 8%、`forward` 占 50% 且随像素线性涨** → 这两份场景不是 CPU 受限（表 → `docs/perf_baseline.md`） | — → P2~P10 的可判断性（**且已知：只动 CPU 的项不会体现**） |
| 4 | ~~D1 语音结束回调~~ | S | ✅ **已完成 2026-10-31**（`voicePlaying()` / `busVoices[]` / `Press::WaitingVoice`，30+38 条判据） | — → C2 语音 ✅ / D2 口型 |
| 5 | ~~**E28 打包 + `VENN_SHIPPING` 剔除**~~ | L | ✅ **已完成**（G29-a~d：闭包扫描 + `game.pak` + `pak:` 虚拟路径 + `dist/<Name>/`；`verify_package` 76 条） | — → 外部测试 / 发布 |
| 6 | ~~**B6 表情预设 + 眨眼/口型驱动**~~ | L | ✅ **已完成 2026-11-01**（自检 49 + 用例 41；驱动只写自有通道、归零只做一帧、LCG 确定性抖动、Voice 总线 RMS 指数包络）：`--selftest expression` / `verify_expression` | — → C2 立绘表情 / D2 音素级口型 |
| 7 | ~~**C5 `autoplay` 消费者**~~ | S~M | ✅ **已完成 2026-10-31**（`TransformClipComponent` 消费：自检 35 条 + `verify_transform_clip` 18 条） | — → 关卡互动 |
| 8 | **C2 立绘系统**（背景 + 立绘 + 表情切换 + 位移） | M~L | AVG 骨架缺的另一半；排在 #4/#6 之后才能一次做对 | #4 #6 → 真正的 AVG 对话 |
| 9 | ~~**H2 崩溃自动保存**~~ | S | ✅ **已完成 2026-10-31**：`EDIT-AUTOSAVE` / `EDIT-AUTOSAVE-STALE`，两条负判据（dirty 门 + 正式档逐字节不变）+ 否定测试 | — → 编辑器安全感 |
| 10 | **V1 特性面 45 条 + V2 i18n 102 条** | M | 准入闸门红着 = "新特性有没有 UI 面"这件事**没人查**，等于 #6 那类问题会反复出现 | — → 后续特性的质量下限 |
| 11 | ~~**P2 rest 分解预计算**~~ | S | ✅ **已完成 2026-10-31**：`Joint::restT/restR/restS` 懒填充（`Pose::setRest`），自检 `rest_cache_*` 4 条 + 否定测试 | #3 → 动画 CPU 时间 |
| 12 | ~~**P3 多角色 buffer 预留**~~ | S~M | ✅ **已完成 2026-10-31**：两遍 `each` 量总量 → 一次扩到位（`ANIM-SKIN-CAP` 每帧资源槽一次，16 角色 5/5 绿） | #3 → 多角色流畅度 |
| 13 | **B8 / P4 蒙皮 LOD** | M~L | 同屏 10+ 角色是**二次元常态**，而蒙皮永远 1 级全精度 | #3 → 群像场景可行 |
| 14 | **C7 旗标 → 任务系统** | L | 现在玩家选的**不算数**（flag 能存，但选项改不了 flag、没有任务视图） | #2 → 分支剧情 |
| 15 | **R5-2 lightmap** | XL | 静态室内的观感与性能双收益，且**解锁 E29** 那半 | #3 → E29 / 室内场景 |
| 16 | **W5 植被真系统** | XL | 现在是"一株一个实体"，头注释自认上限几千株；CPU 提交随株数线性涨 | #3 → 大草原 / 花海 |
| 17 | **B4 脚部贴地 IK** | M | 斜坡/台阶脚悬空。**胶囊级贴地已有、骨骼级没有** | 真实角色资产 → 走位正确 |
| 18 | **B5 弹簧骨碰撞** | M | 头发穿肩、裙摆穿胸，**近景必穿帮** | 真实角色资产 → 特写可用 |
| 19 | **W7 动态天空（Hillaire + 昼夜）** | L | 二次元的"黄昏/清晨氛围镜头"全靠它 | — → 氛围演出 |
| 20 | **A8 NPR×CSM 的 PCF 台阶** | M | A 组**唯一已知缺陷**，脸上 ~8 px 锯齿，现有夹具能复现 —— "想再挖一刀就挖它" | 真实角色资产 → 脸部干净 |
| ~~21~~ | ~~**C8 NavMesh + 巡逻**~~ | ~~XL~~ | ✅ **2026-11-05 结清**（格子导航，不是 NavMesh）。成本确实最高，但收益兑现了：`engine/nav/` 五件 + 面板 + 九条命令 + 脚本四动词 + 94/66/5 条判据 | 已完成 → 后续只挂"局部避障 / 动态障碍"（`navigation.md` §十） |

### 6.2 波次建议（三波 + 两条并行轨）

- **波次一（#1~#10）= "能用"**：主线 `#1 → #2` 串行；`#3 #4 #9` 是可插空的**并行轨**（都是 S 级）；
  `#5 #6` 谁先都行，但**别都拖到波次二**（返工斜率会咬人）。
- **波次二（#11~#16）= "跑得动"**：全部以 #3 的基线为尺子。`#11 #12` 一天能出结论，
  先用它们建立"改动 → 数字"的肌肉记忆，再碰 `#15 #16` 这种 XL。
- **波次三（#17~#21）= "好看、活起来"**：共同前置是**真实角色资产**（有内眼角、有刘海、有裙摆）
  与**群像场景**。用程序化球体调 #17/#18/#20 没有意义。
- **横切不进排名**：H4（零单元测试）不单独开轮，**每轮顺手补一条**；H1（拆 `Renderer`）只在动
  渲染时顺手切；H7（无 CI）等有第二台机器再谈；E22（对齐/分布）挂在波次二任一项后顺手做。

### 6.3 排后面但必须排在某个东西前面（依赖锁死，别搞反）

| 项 | 被谁锁住 | 说明 |
|---|---|---|
| **E27 RenderGraph 可视化** | R7-1 | 依赖链最末端；**R7-1 本身也先不做**（工程性收益、不可感知） |
| **E29 烘焙 UI** | R5-2（光照图那半） | 反射探针那半已可用 |
| ~~**G19 关卡流送**~~ | ~~#2（存档格式定稿）~~ | ✅ **2026-11-02 结清** —— 存档格式 11-01 定稿后按期解锁 |
| **R7-2 细粒度同步 / 异步 compute** | R7-1 | — |
| **D2 口型同步（音素级）** | 音轨对齐 / viseme 表 | 包络级已随 B6 落地（`AudioSystem::voiceLevel()` → `tickExpressions`） |
| **B7 混合树进阶** | 真实角色资产 + 动作素材 | 线性 Demo 只有 crossfade 就够 |
| **B9 换装** | #2（部件引用要存下来） | 挂点（`BoneAttachment`）已有 |

### 不建议现在做

- **E24 材质节点图**（二次元材质是参数化的，ramp/SDF/各向异性参数面板够用）
- **R7-3 Ray tracing**、**R5-3 DDGI**、**W9 GI 烘焙**
- **移动端导出**（先把 Windows 闭环走完）

---

## 7 · 附：本轮的实测证据（可复现）

```
check_feature_surface.py     → 结果: FAIL（45 条 D 组）
verify_i18n_coverage.py      → 结果: FAIL（102 项缺中文；孤儿 280 / 重复键 24）
verify_viewport.py           → RESULT: FAIL（2 项）V1d / V3

⚠ 2026-10-31 复查：`verify_i18n_coverage.py` 已经是 **结果: PASS**
（A1~F 全 0；B2 的最后一条缺口 `Ctrl+. or Home snaps…` 也补上了）。
上面这行是 10-27 的快照，留着是为了对齐"当时剩多少"，**不要当现状**。

2026-11-01 收口时四条闸门（全绿）：
--selftest expression        → TOTAL pass=49 fail=0
verify_expression.py         → 41 通过 / 0 失败 → 结果: PASS（连跑 3 次一致，非 flaky）
check_feature_surface.py     → 结果: PASS（37 个特性全部有 UI 面；无裸 env 漏网）
verify_i18n_coverage.py      → 结果: PASS
check_regression_map.py      → 结果: PASS
```

代码侧核对（grep 命中数，0 = 未实现）：

| 关键词 | 命中 | 含义 |
|---|---|---|
| `NavMesh` / `DataTable` / `.csv` | 0 / ~~0~~ / ~~0~~ | 无三角网 NavMesh（**已用格子法替代**，见 `navigation.md`）；配表 ✅ 11-04 落地 |
| `SaveGame` / `EntityGuid` | ~~0~~ / ~~0~~ | ✅ 两者均已落地（`scene/SaveGame.*` 10-31；`World::Record.guid` 10-27） |
| `lightmap` / `Reverb` | 0 / 0 | 无光照图、无混响 |
| `ExpressionPreset` | ~~0~~ → **有** | ✅ 2026-11-01 落地（`engine/animation/ExpressionPreset.*` + `ExpressionDriver.*`），判据 `verify_expression` 41 条 |
| `BlendTree` / `footIk` | 0 / 0 | 无混合树、无脚 IK |
| `LevelStreaming` / `LevelManifest` | ~~0~~ → **30+** | ✅ 2026-11-02 落地（`scene/LevelStreamer.*`），判据 `verify_levels` 31 条 |
| `openUI` | ~~0~~ → **5** | ~~7 个导演 API 里唯一缺的~~ → **✅ 2026-10-31 落地**，7/7 齐 |
| `playSequence` / `showDialogue` / `playAnim` | 6~7 | 已实现（#307/#309） |

---

*本文把"还剩什么"和"为什么"分开写：§2 是清单，§3/§4 是**怎么量**，§6 是**先做哪个**。
下次评估从 §2 逐行勾掉即可。*
