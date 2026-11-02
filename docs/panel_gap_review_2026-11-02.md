# 开发者视角的「面板 + 操作体系」缺口复查（2026-11-02）

> 上一轮同类审计是 `panel_gap_review_2026-10-27.md`（面板视角）与
> `developer_workflow_gaps.md`（流程视角）。**两份都已过期** —— 它们把打包、存档、
> GUID、Prefab、多选、对话编辑器、UI 编辑器、时间轴编辑器、表情预设全部记成"断"或
> "无"，而这些在 10-27 ~ 11-01 之间逐条落地了。
>
> 本文是按 **2026-11-02 磁盘实际状态**重做的复查，口径只有一条：
> **开发者做某个具体动作时，面板上有没有那条路**。
> 所有数字与结论都来自 `grep` / 读码，可逐条复核。

清点基准（**2026-11-03 P3-3 收尾时重数过**）：
**46 张面板**（`grep -c '&EditorApp::panelDraw' editor/EditorApp.cpp`）、
**14 个 Inspector 页签**（`kTabs[]`，含 `All`）、**46 个 ECS 组件**（`Components.h`）、
**118 个渲染参数**（`FeatureRegistry.cpp` 的 `P*()`）、
**46 个特性**（`check_feature_surface.py` 的 `dict(id=...)`）。

> ⚠ 本文初稿写的是 35 张面板 / 43 个组件 —— 那个数在一整天的收口里被
> 自己写旧了：PostVolume / 音频混音台 / Project Settings / Data tables /
> Localization / Flags / **Effects** / Script debug / Levels 九张面板是
> 同一批补上的（见各小节 ✅ 与 §5）。组件 +1 是 `FxHostComponent`。
> **教训**：这份文档的每个数字都是"读磁盘读出来的"，重数一次只要两秒。
>
> ⚠ **2026-11-03 再涨一格**：P1 的 `Effects`（FxPanel）+ P2 的
> `Debug ▸ Gameplay`（GameplayPanel）→ 面板 43 → **45**；
> Inspector 第 13 个页签 `Gameplay` → 页签 13 → **14**；
> `GameplayComponent` → 组件 44 → **45**。
> 两条都不是"多一个面板"，而是补上了一类**能力**：
> Effects = "打击链能被作者调出来"，Gameplay = "数值与回合能被作者改"。
>
> ⚠ **2026-11-03 第三格**：P3-3 的牌堆 → 面板 45 → **46**（`Debug ▸ Cards`）、
> `CardDeckComponent` → 组件 45 → **46**；Inspector 页签数**不变**（牌堆是
> Gameplay 页签里的第 ⑤ 段，不是新页签 —— 两者是同一类数据，切页签找它
> 反而更慢）。能力上补的是"**多张有身份的实例**能被作者造出来、洗、抽、
> 打、弃、存档"：P2 的 `GameplayComponent` 管的是一个单位的数值，
> 这套管的是**一副牌的顺序**。

---

## 0 · 一句话

**"摆东西"和"做表现"这两段已经是专业级的；缺的全部集中在三层：
配置层（配表 / 本地化 / 发布配置）、调试层（CPU / 脚本 / 物理可视化）、
规模层（流送 / 寻路 / 实体数量）。**

而且里面最短的几条不是"要开发新系统"，是 **能力早就在、面板零入口** ——
后处理体积、音频混音台、导入设置、剧情旗标四条都属于这一类，加起来不到三天。

---

## 1 · A 类：引擎有能力，编辑器中零入口（**最亏**）

判据：`engine/` 里实现完整、甚至已经落盘、已经被渲染/运行期消费，
但 `editor/` 里 **grep 命中 0**。

### A1 后处理体积（Post-Process Volume）✅ **2026-11-02 已补**

> 落地：`editor/PostVolumePanel.{h,cpp}` + 面板表项 `postvol` + 四条命令
> `pv.list/eval/add/remove` + `docs/post_volume.md` + `verify_post_volume`（43 条）。
> 另修了一个真 bug：`cgLutBlend` / `autoExposureBias` 没进 `toJson/fromJson`，
> 存盘即丢、无报错。**仍未做**：视口线框（另一刀，要走调试线通道）。

| | |
|---|---|
| 已有 | `engine/scene/PostVolume.{h,cpp}`：AABB + 优先级 + 过渡带权重 + `kNoOverride` 哨兵 + `PostVolumeSet::evaluate()` |
| 已落盘 | `SceneSerializer.cpp:1118` `postVolumeToJson` / `:4209` 写 / `:4426` 读，且进了 `kSceneKeys` 白名单 |
| 已消费 | `Renderer.cpp:2327` 每帧按相机位置求值，混进 `CameraEffects` |
| 缺 | `grep -rln PostVolume editor/` → **0 命中**。它不是实体，Hierarchy 里也没有 |

**开发者遇到什么**：想让"进洞穴变暗""角色特写加景深""回忆段落去饱和"——
只能改 `post.*` 的 118 个**全局**参数，一改全场。UE/Unity 里这是三步操作
（放个 Box → 勾几项 → 设过渡带），这里一步都没有。

**补什么**：一张 `PostVolume` 面板（场景内体积列表 + 新建/删除 + AABB 拖拽/尺寸
+ 覆盖项勾选表 + 过渡带 + 优先级 + **"这一项没覆盖"要显式画出来**）。
另外给它一个视口内的线框（现在没有任何可视化，放了也看不见）。

### A2 音频混音台 ✅ **2026-11-02 已补**

| | |
|---|---|
| 已有 | `Mixer.h`：声部池 / 总线 / 重采样 / 3D 衰减 / pan / **ducking**；`AudioSettings{master/music/sfx, duckDepth, device}`；11-01 又加了 `busRms[kBusCount]` |
| 缺（补之前） | 面板表里没有 audio；`grep -rn "masterGain\|duckDepth\|busVolume" editor/` → **0**；编辑器起音频是硬编码 `as.music = 0.7f` |

**开发者遇到什么**：BGM 太吵 → 改 C++ 重编译。想关 ducking、想知道"现在 32 个声部
占了几个"、想揪出"谁在循环播那条音效"→ 面板上没有任何地方。

**补了什么**：`editor/AudioMixerPanel.{h,cpp}` + 面板表 id `audio` + 命令
`audio.state` / `audio.gain` / `audio.mute` / `audio.sources`。
四根推子 + 静音 + duck 深度 + 停止全部 + 三条总线的实时声部数与 RMS 电平表 +
语音总线电平（B6 口型的唯一输入）+ Play 态声源清单。
判据 `tools/verify_audio_mixer.py` 27 条；细则 `docs/audio_mixer.md`。

**顺带修的两个真 bug**：
① `AudioSystem` 没有增益读访问器 —— 面板读 `settings()` 画推子，而那是
`init()` 的**出厂值**，`setBusGain` 不回写 → 推子拖完弹回原位。加了
`busGain(BusId)`。
② `setBusGain(BusId::Master)` 只写 `m_busGain[0]`、不写 `m_master`，
两个真值各对一半（命令改了、读回没变）。现在 Master 转发 `setMasterGain`。

**还没做**：总线 solo、运行期改 `maxVoices`、频谱/波形、把音量持久化到
`game.json`（那属于 D 类 Project Settings）。

### A3 资产导入设置（`.vmeta`）

| | |
|---|---|
| 已有 | `.vmeta` 侧车文件（导入参数：缩放 / 轴向 / 材质映射…）；`tryLoadTexture` 容错链 |
| 缺 | `grep -rn "vmeta" editor/*.cpp` → **0**；`ContentBrowser::importFiles()` 全程只有 `fs::copy_file`（`:1372`） |

**开发者遇到什么**：导入是"复制文件"，不是"导入资产"。轴朝向、单位、动画段、
材质映射一个参数都没有；`.vmeta` 只能手写。坏文件也照样报 `Imported 1 file(s)`。

**补什么**：导入向导一屏（选骨架 → 根骨 → 动画段 → 预览 → 生成 `.vmeta`），
以及 Content 里右键一个 "Import Settings…"。这一条还是 `panel_gap_review`
P1-1 的原话，两个月没动。

### A4 剧情旗标 / 全局状态　✅ **2026-11-02 已补**

| | |
|---|---|
| 已有 | 脚本 `setFlag` / `getFlag`（`ScriptEngine.cpp:332`），世界黑板持久化 |
| 缺 | `grep -rn "setFlag\|getFlag" editor/` → **0 命中** |

**开发者遇到什么**：写了 20 个旗标之后，**没有任何地方能看到"现在有哪些旗标、
值是多少"**。剧情调试全靠 `print`。也没有"重置到某章开头"的操作。

**补什么**：Debug 面板里一张旗标表（名字 / 值 / 类型 / 谁写的），
加上 Play 中的实时刷新 —— 这张表比断点更常用。

→ **补的过程中发现更严重的东西**：编辑器 Play 态**根本没接** `svc.act.setFlag`，
Play 里调它报的是 `setFlag() is not available in this host` —— 剧本分支在编辑器里
**等于不存在**，只能出包才能试。所以这一条真正的修复是"接上 host"，面板只是第二半
（能写但看不见也调不了）。另外 `GameFlags` 原来住在 `runtime/`，编辑器按分层规则
不许 include，必须先搬到 `engine/scene/`。

→ 细节 / 判据 / 已知边界见 `docs/flags_and_breakpoints.md`；
验收 `tools/verify_flags.py`（A~E 全 PASS，23 条内核 + 面板/命令/Play/新一局/鼠标）。

---

## 2 · B 类：面板在，但链路不成立

### B1 UI 编辑器：做不出"能点的界面"　✅ **2026-11-02 已补（行为那一半）**

- `ui2/UiDocument.h` 的 `UiWidgetType` **6 种**（Panel / Nine / Image / Text /
  Button / Progress），`UiWidget` 里**没有 `action` / `onClick`**（grep 0 命中）。
- 后果：按钮画得出、能高亮、能悬停，**点下去什么都不会发生**。
  背包 / 卡池 / 商店 / 主菜单 —— 二次元游戏 UI 的大头，全部还得写 C++。
- 另外缺 **list / scroll / slider / toggle**，几十条目的列表排不下。

→ 这一条是"UI 段从半通到通"的唯一门槛，也是 6 个控件里唯一有**行为**缺口的一条。

**补了什么**（`docs/ui_actions.md`）：

- `UiWidget` 加三个字段：`action`（字符串动词）/ `enabled` / `visibleBind`；
  编辑器属性面板多一段 `Interaction`（`UIED-ACTION` / `UIED-ENABLED` /
  `UIED-VISBIND`），**写错当场拒**。
- 动词四个：`flag:key=1` / `toggle:key` / `sound:path` / `quit`（**没有 `emit:`**，
  理由见文档 §2 —— 文档层的派发发生在渲染阶段、而 `svc.uiAction` 每帧被
  `clear()`，两者时序不通；脚本是"每帧轮询"模型，`flag:` 语义一样且能进存档）。
- 命中测试 `ui2/UiDocInput`：倒序（最上面的赢）、按下/松开同一控件才算点击、
  `visible=false` 与 `enabled=false` 都不命中、注入点击排队。
- 顺带修的真 bug：`tickUi` 的"点击视口 → 进入操作态"原来**无条件捕获**鼠标，
  Playing 下真鼠标点文档按钮**永远点不到**（而注入点击照样能点）。现在先问
  文档层（`GAME-MOUSE captured=0 reason=ui-doc`）。
- 判据：`tools/verify_ui_actions.py` 五组（自检 23 条 / 注入点击 / 条件可见 /
  quit / **真鼠标**）。

**还没做**：`list` / `scroll` / `slider` / `toggle` 四种控件仍然不存在 ——
几十条目的列表、设置里的滑条还是得写 C++。这是**控件种类**缺口，与上面这条
（行为缺口）不是一件事。


### B2 Stats 面板：只有 7 行 — ✅ **已补（2026-11-02）**

现有全部内容（`EditorApp.cpp:6651~6676`）：FPS / 帧时间 / 场景名 / 实体数 /
本地点光聚光数 / RT 尺寸 / MSAA / 深度预通道开关。

**没有**：draw call 数、三角形数、实例数、各 pass 的 CPU 时间、内存占用、
资产数量、物理体数（在 Physics 面板）、脚本耗时。

→ P1 基线已经证明 **`forward` 占 50% 且是填充率问题**（`docs/perf_baseline.md`），
但"这一帧到底提交了多少个 DrawItem"在面板上看不到 —— 优化只能靠外部工具。
**GPU 侧有 Timeline 面板，CPU 侧是空的。**

**⚠ 这条写下时是照旧行号读的，有一半已经过时**：E15 那轮（`docs/` 里的
帧时间曲线）已经把面板扩成 CPU/GPU 双栏，draw call / 三角形 / BVH / LOD /
CSM / 局部阴影 / shader 变体 / 环境贴图**全都在**。真正还缺的只剩两样：

- **各 pass 的 CPU 时间**（C9：无 `ScopedTimer`、无 `cpuMs` 采样）—— GPU 有
  11 段时戳 + 堆叠条，CPU 只有一个 `io.Framerate` 总数。
- **内存占用**。

**补了什么**：`engine/core/CpuProfiler.{h,cpp}`（12 段，RAII 打点，定长环、
零分配、不加锁）+ `engine/core/ProcStats.{h,cpp}`（工作集 / 峰值 / 私有）。
Stats 面板加两张通栏堆叠条（帧分区 + 渲染细分）、一张可展开的分段表
（last / avg / max）、一行内存读数。`MYVK_CPU_STATS=1` 每 30 帧打一条
`CPU-STATS`（与 `GPU-TIMING` 同格式）。判据：`tools/verify_stats_cpu.py`
（A / A2 / B+C）。细则 → `docs/stats_cpu.md`。

**没做**（有意）：资产数量（Content Browser 已有计数）、物理体数（Physics
面板）、脚本耗时（`ScriptRuntime` 有自计时，但塞进这一屏会把 12 段表撑爆 ——
它是独立面板的活）。

### B3 Project Settings 菜单 → 只开 Quality 面板 — ✅ **已补（2026-11-02）**

`Toolbar.cpp:567`：点 `Project Settings...` 只是 `panelOpenRef("quality") = true`。

而真正的项目/发布配置住在 `assets/game.json`：标题 / 分辨率 / 全屏 / `maxFps` /
`quality` / `language` / `uiDoc` / `scenePath`（`RuntimeConfig.h:37~`）——
**全部只能手改 JSON**。一个策划要改游戏标题，得知道这个文件在哪。

→ 缺一张真正的 Project Settings：通用 / 渲染默认 / 输入 / 音频 / 物理 / 本地化 /
发布（窗口、图标、默认场景、打包白名单）。

**补了什么**：`editor/ProjectSettingsPanel.{h,cpp}`（22 个字段、五段折叠、
`PROJ-*` 锚点）+ `engine/project/GameFile.{h,cpp}`（读/写/夹取 + 自检 29 条）。
`Toolbar.cpp` 改成 `panelOpenRef("projectsettings")`。命令 `project.settings` /
`project.set <key> <value>` / `project.save` / `project.reload`。
判据：`tools/verify_project_settings.py`（六组 46 条 PASS）。细则 →
`docs/project_settings.md`。

三条最贵的纪律：**写回是合并不是整份重写**（否则静默清掉 `player` 的 25 个字段）/
**坏 JSON 拒绝写**（不"从空对象重写"）/ **"未保存"靠快照比较算出**（21 个控件
逐个置脏位，漏一个就丢改动）。

### B4 Console 无级别过滤

`setSearchFilter()` 存在但只在代码里用；搜索框 / 清除 / 复制都有，
**没有 Info/Warn/Error 分段开关**。日志一多就只能靠文本搜。

### B5 覆盖指示器（E20）

渲染参数有 5 层覆盖链（默认 → 画质档 → 项目 → 场景 → 相机），
`FeatureUIGen.cpp` 里 `Overrides` / `inherited` **0 命中**。
→ 调参数时永远在自问"这个值是谁设的、我改了会不会被谁盖掉"。

---

## 3 · C 类：整块缺失（面板 + 系统都没有）

| # | 缺什么 | 判据 | 开发者卡在哪 |
|---|---|---|---|
| C1 | **配表 / 数据表** ✅ | ~~`grep -rli "datatable\|\.csv"` → 0~~ → `core/Csv` + `project/DataTable` + Data tables 面板 | 角色属性、技能、掉落、文本表只能塞脚本常量；数值一改就要重编 |
| C2 | **本地化编辑** ✅ | ~~译文只有 `ZhTable.cpp` 一张编译进去的静态表~~ → `i18n/LocaleFile` + `assets/i18n/zh.csv` 覆盖层 + Localization 面板（**台词仍不走 i18n**，见下） | 策划改一句翻译 = 改 C++ 重编译 |
| C3 | **关卡流送 / 关卡清单**（G19）✅ | ~~`LevelStreaming` / `LevelManifest` 0 命中~~ → `scene/LevelManifest` + `scene/LevelStreamer` + `loadSceneAdditive` + Levels 面板 | 只能"一次加载整个世界"；大关卡内存与加载时间线性涨，也没有"章节列表" |
| C4 | **NavMesh / 寻路** | 全仓 0 命中（连组件都没有） | NPC 不会走。剧情向游戏可以先用路径点顶，但"敌人追击/村民走动"做不了 |
| C5 | **立绘系统**（C2） | `DialogueAsset.h:33` 明写"不做立绘/表情/语音字段" | AVG 骨架缺一半：背景 + 立绘 + 表情切换 + 左右位移 |
| C6 | **任务 / 剧情状态机**（C7） | 0 | 选项改不了任何持久状态 → 分支剧情只能靠旗标硬编码（见 A4） |
| C7 | **光照烘焙 UI**（E29） | `Toolbar.cpp:687` 灰着 | 室内静态光只能靠实时灯；被 R5-2 lightmap 阻塞 |
| C8 | **脚本断点调试** ✅ | ~~`breakpoint` 全仓 0 命中~~ → `script/BreakpointSet` + `ScriptProgram::executableLines()` + Script debug 面板 | 只有 `print` → Console；协程里的状态看不见 |
| C9 | **CPU Profiler / 内存面板** ✅ | ~~无 `ScopedTimer` / 无 cpuMs 采样~~ → `core/CpuProfiler` 12 段 + `core/ProcStats` | 见 B2 |
| C10 | **材质节点图**（E24） | 0 | **明确不建议现在做**（二次元材质是参数化的，118 个参数面板够用） |
| C11 | **特效调度 / 打击感**（P1）✅ | ~~`EffectAsset` / `EffectService` / `AnimEventRouter` 全仓 0 命中；`.veffect` 不存在；`FiredAnimEvent` 没有消费者~~ → `.veffect` 打包 + 宿主池 + 动画事件路由 + 6 个脚本动词 + 顿帧/闪白/震屏 + **Effects 面板**；权威文档 `docs/fx_system.md` | 打击特效只能靠脚本掐秒表；改一次 `timeScale` 就全部错位。**仍未接**：Runtime 侧分发（出包后打怪没刀光） |

---

## 4 · D 类：横向操作体系（比单个面板更拖慢）

这四条不属于任何一段流程，但**每一分钟都在拖慢所有流程**。

### D1 Undo = 整场景 JSON 前后快照

`EditorContext.h:595`：

```cpp
void structuralEdit(const std::string& name, Fn&& fn) {
    const std::string before = snapshot();
    fn();
    const std::string after = snapshot();
    if (before == after) return;
    ...SceneSnapshotCommand(name, before, after)
}
```

- 大场景**每一步**都存两份完整 JSON（内存与耗时随实体数线性涨）。
- 属性面板拖滑条默认不进历史 —— 只有包了 `structuralEdit` 的调用点才进
  （现 133 处）。于是"我改坏了想撤回"**经常撤不回来**，且用户无法预知哪次能撤。

→ 需要命令化（按字段记 delta），或至少"属性拖拽结束时自动入栈"。

### D2 对齐 / 分布 / 循环选择 / 吸附可视化（E22 残留）

多选、框选、批量变换都有了（10-27），但**对齐、等距分布、循环选择、
吸附网格线**没有 → "能批量搬，不能对齐"，摆一排灯还要手填数字。

### D3 快捷键总览与冲突检测（E23 残留）

`KeyBindingsPage` 只管**输入映射**（WASD 那套），**编辑器命令**的快捷键散在
`handleShortcuts()` 里，加一个命令要改两处，也没有冲突检测/总览窗口。

### D4 面板之间没有交叉引用

"给这个物体加碰撞体"要去 Physics 面板、"加 IK"要去 Character 窗口 ——
Inspector 里没有任何"这个组件归谁管、点这里跳过去"。新用户学不到归属关系。

---

## 5 · 已经做完的（**不要再报**）

写在这里是为了下一轮审计不重复劳动：

打包/发布（E28 + G30 `make ship`）、存档槽位（G17-3 + 面板）、稳定 GUID（G16）、
Prefab / 多选 / 批量变换、关节（J5 全套 + 视口拾取 + 3D gizmo）、对话系统 +
剧本编辑器、过场时间轴 + 轨道编辑器、UI 编辑器（最小版）、
曲线编辑器 / 摄影表 / 骨骼曲线 / NLA、动画状态机图编辑器、角色制作流程壳、
天空与大气（Environment）、二次元 UI 主题、系统语言默认、表情预设 + 眨眼/口型（B6）、
P1 Release 性能基线、多选与批量编辑、布局自由拖拽/合并（E29-1~5）、
后处理体积（PostVolume 面板）、音频混音台、Project Settings、Data tables、
Localization、剧情旗标、**特效调度 + 打击感（Effects 面板 / `docs/fx_system.md`）**、
脚本断点调试、关卡流送（Levels 面板）、
**玩法数值与回合调度（Inspector 第 13 页签 Gameplay + Debug ▸ Gameplay /
`docs/gameplay_stats.md`）**。

> ⚠ 最后两行是**同一类东西**，别拆开看：
> 它们补的都不是"某个面板缺了"，而是"引擎有能力、编辑器里零入口"这个
> A 类缺口 —— 特效系统做完了但作者只能改 JSON，等于没做完；
> 属性/状态/伤害做完了但作者只能改场景文件，同样等于没做完。

---

## 6 · 建议顺序（按"解开多少下游"排，不按难度）

| 序 | 做什么 | 成本 | 解开什么 |
|---|---|---|---|
| 1 | **PostVolume 面板**（A1）✅ | 0.5 天 | 镜头语言。渲染侧全在，只缺一屏 UI + 一个线框 |
| 2 | **Audio 混音台**（A2）✅ | 0.5 天 | 混音整层从"改 C++"变成"拖推子"；顺带让 B6 口型的输入可见 |
| 3 | **UI 事件回调**（B1）✅ | 1~2 天 | 背包/菜单/卡池从"做不了"变成"能搭"；**二次元 UI 的大头** |
| 4 | **Project Settings 真面板**（B3）✅ | 1 天 | 把 `game.json` 从手改 JSON 变成 UI；发布前的必经之路 |
| 5 | **Stats 补齐 + CPU 采样**（B2/C9）✅ | 1~2 天 | 让 P1 基线可被"每一次改动"复用，而不是一次性 |
| 6 | **配表 + 本地化编辑**（C1/C2）✅ | 3~5 天 | 内容规模化；策划能独立干活的分界线 |
| 7 | **旗标表 + 断点调试**（A4/C8）✅ | 2~3 天 | 剧情调试从"print 大法"变成可观察 |
| 8 | **关卡流送**（C3）✅ | L | 大世界的前置；也是最后一项硬缺口 |

⚠ 1~4 全部**不需要动渲染管线**，且都有现成的内核在等 —— 属于"接线 + 一屏 UI"。

---

## 7 · 与既有文档的关系

| 文档 | 状态 |
|---|---|
| `panel_gap_review_2026-10-27.md` | **10-27 快照**。A 类三条（关节/音频/打包）里关节与打包已完成，只剩音频。P2 五条已修 |
| `developer_workflow_gaps.md` | **10-27 快照**。八段里第 7（存档）、第 8（打包）已完成，"五件"全部落地 |
| `remaining_work_2026-10-27.md` | 仍在维护，是**任务账本**；本文是**能力→入口**的判据，两者互补 |
| **本文** | 2026-11-02 现状。下一轮审计以本文为基线 |
