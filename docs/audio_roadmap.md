# 游戏音频现状分析 + 改造路线图

> 2026-10-18 · 对齐 `docs/ship_roadmap.md` 的 **G14**
> 这是"补齐一条 3D 游戏链路"系列的**第三篇**（第一篇：`character_pipeline_roadmap.md`；
> 第二篇：`ui_hud_roadmap.md`；后续：存档与 GUID、打包）。
> 所有结论都对着磁盘代码核过，行号是 2026-10-18 的版本。

---

## 0 · 一页结论

**现状不是"少一套音效"，是"一个采样都没有"。**

整个工程里与音频有关的代码 = **2 行配置读取 + 1 行菜单表项**：

| 位置 | 内容 | 有没有人用 |
|---|---|---|
| `runtime/RuntimeConfig.h:57` | `std::string audioDevice = "default";` | ⚠ 零消费者 |
| `runtime/RuntimeConfig.cpp:102` | `getStr("audioDevice", cfg.audioDevice);` | ⚠ 读进来就丢了 |
| `editor/ContentBrowser.cpp:974` | `{"audio", "Audio", "vaudio"}`（Add ▸ 菜单第 10 项） | ⚠ 建完是空文件 |
| `assets/game.json:13` | `"audioDevice": "default"` | ⚠ 空挂字段 |

也就是说：**配置层有一条完整的"读到就丢"链路，内容层有一个能点但什么都不做的菜单项，
渲染/运行时/物理/脚本四层各零行音频代码**。这不是"缺一个模块"，是缺**六层地基**：

| 层 | 现在有什么 | 还差什么 | 断点 |
|---|---|---|---|
| 设备 | 零 | 打开声卡、时钟、设备枚举、失效恢复 | **D0** |
| 解码 | 零（`stb_vorbis.c` 在仓库里但**没进编译**） | WAV/OGG → PCM | **D1** |
| 混音 | 零 | 声部池、总线、重采样、增益、削波保护 | **D2** |
| 空间 | 零 | listener、3D 衰减、panning、多普勒 | **D3** |
| 资产 | 零（`AssetManager` 全是 GPU 资源） | CPU 侧 clip 缓存 + 流式 | **D4** |
| 组件 | 零 | `AudioSourceComponent` + 序列化 + Inspector | **D5** |
| 内容 | 零 | Content 试听、脚本 `playSound`、测试素材 | **D6** |

**一句话判断**：六层里少任何一层，都只能做出"能播一个 wav、但换个采样率就变调 / 换个设备就崩 /
暂停之后再也停不下来 / 编辑器里根本挂不上"的东西。这个项目已经在同类问题上返工过两次
（Inspector 的 `All` 页签、粒子面板的逐发射器编辑），所以这次先把地基按层切开。

**总规模：`L`**（≈ `ui_hud_roadmap` 的 0.8 倍；6 个断点，但每层的验收都能脱离声卡跑）。
**建议拆成 4 个里程碑**：

- **M0 骨架**（G14-0 ~ G14-2）：**不开设备也能跑完混音**，离线渲染出波形；
- **M1 出声**（G14-3 ~ G14-4）：真设备打开、3D 定位、场上有声音；
- **M2 能控**（G14-5 ~ G14-6）：总线 / 音量 / ducking / 流式音乐；
- **M3 能编**（G14-7 ~ G14-9）：组件、序列化、Inspector、Content 试听、脚本、回归。

**三个最重要的判断**（后面 §3 有完整论证）：

1. ⚠⚠ **不要用 miniaudio 的高层 `ma_engine` / `ma_sound`**。它们把混音状态关在库里，
   无法离线渲染 → **无窗口测不了** → 又回到"只能在人耳朵里验证"的老路。要用的是
   "设备层（低层 `ma_device` 或自研 WASAPI）+ **自研 Mixer**"。这与项目既有铁律
   "采样不许留在 Renderer 里"是同一条：**能被断言的东西必须在我们自己的数据结构里**。
2. ⚠⚠ **音频的时基来自设备，不是 `GameLoop`**。`timeScale = 0` 冻的是模拟，不是声音；
   反过来，音频**只读世界，永不写世界** —— 否则 `verify_hud` 那条"暂停时位置逐位不变"
   会被一条无关的改动打红。
3. ⚠⚠ **"默认关"要改成"自动化默认关"**。`ship_roadmap` 原文写"默认关 = 基线不变"，
   但产品上游戏默认没声音是不能交付的。正确做法：**产品默认开**（设备缺失时静默降级），
   `--selftest` / `--frames` / 回归脚本**默认注入 `MYVK_RUNTIME_AUDIO=0`**，基线才逐位不变。

---

## 1 · 现状盘点（逐项对着代码）

### 1.1 `audioDevice` 是一条完整的"读到就丢"链路

```
assets/game.json:13          "audioDevice": "default"
        ↓
RuntimeConfig.cpp:102        getStr("audioDevice", cfg.audioDevice);   ← 只做字符串读取
        ↓
RuntimeConfig.h:57           std::string audioDevice = "default";
        ↓
        （没有任何 .cpp 再读过这个字段）
```

`grep -rn "audioDevice"` 全仓只有上面两处。**这就是"空挂字段"的教科书形态**：
它在配置里、它能被覆盖、它连日志都不打，所以既不会崩也不会提示 —— 你在 `game.json`
里写 `"audioDevice": "我的耳机"` 和写 `"小黄鸭"` 的行为**完全相同**（都是什么都不做）。

⚠ 这条正是本次改造要**优先接线**的东西，因为它是"用户以为自己能设置"的入口。
接线的判据不能是"读到了"，必须是**`AUDIO-INIT` 日志里打出了实际设备名**。

### 1.2 ContentBrowser 已经预留了 audio 类型，但没有试听

`editor/ContentBrowser.cpp:960-975` 的 `addKinds()` 是**有序**表（注释写明"只能往后加、
不能重排"），第 10 项就是音频：

```cpp
{"audio",     "Audio",          "vaudio"},
```

但双击的分发 `openEntry()`（`:1554`）只处理三种：`isScene` / `isModel` / `isScript`。
其余一律**静默无反应** —— 包括音频、纹理、水、地图。

于是现状是：**Add ▸ Audio 能建出一个 `.vaudio` 空文件，双击它什么都不会发生**。
这和 `audioDevice` 是同一类问题：菜单上看得见，点下去什么都没有。

⚠ 顺带一个陷阱：`.vaudio` 是**自定义描述文件**（没有内容格式定义），而真实音频是
`.wav` / `.ogg`。如果 G14 只把 `.vaudio` 做成"资产壳"，就会造出**第二个空挂**。
本路线图的处置是：**先让 `.wav/.ogg` 双击就能试听**（真实文件、零格式设计），
`.vaudio` 的语义（总线 / 衰减预设）放到 G14-X 可选，且必须"引用一个真实 clip"才算数。

### 1.3 Inspector 没有 Audio 段，Add Component 也没有

`editor/InspectorPanel.cpp:259` 的 `kTabs[]` 是 11 项：

```
object / tool / render / output / scene / world / particles / physics / material / animation / all
```

没有 audio。而 `sectionKeywords()`（`:99`）的搜索关键词表里也没有 Audio ——
这两张表是**必须同步**的（用户搜 "volume" 要能命中）。

"Add component"菜单在 `:5442` 一带（`ImGui::MenuItem(TR("Script"), ...)`），
也没有音频项。所以**今天的编辑器里没有任何路径能挂上音频组件** —— 即使引擎里有实现。

### 1.4 ECS / 序列化 / Play 副本：组件层是空的

`engine/ecs/Components.h` 有 26 个组件（Transform / Mesh / Material / Collision / RigidBody /
CharacterController / Script / Player / 各种 Light / ParticleEmitter …），**没有音频**。

序列化侧（`engine/scene/SceneSerializer.cpp`）是**扁平 JSON**：每个组件一个顶层键
（`"script"` / `"player"` / `"particle"` …），写端在 `:1509` 一带、读端在 `:1736` 一带。

⚠ 这里有一条**结构性红利 + 一条结构性陷阱**：

- **红利**：编辑器的 Play 是**把场景序列化成 JSON 再读回来**作为运行态副本
  （`EditorContext.cpp:110-135`，`sceneToJson` → `sceneFromJson`）。所以
  **只要写了序列化，Play 副本自动带上音频组件**，不需要写任何 clone 代码。
- **陷阱**：反过来，**只加组件不写序列化 → Play 里音频组件凭空消失**，而且不报错。
  这是本项目"五件套"里第 ④ 条为什么是硬性的最好例子。

### 1.5 脚本引擎能扣血，不能发声

`engine/script/ScriptEngine.h` 的 `ScriptHost` 目前有三个回调（U8 加的）：

```cpp
std::function<void(float)> setHealth;
std::function<void(float)> damage;
std::function<void(float,float,float,float)> popup;
```

求值器里对应 `setHealth` / `damage` / `popup` 三个内置函数。**没有 `playSound`**。
所以"击杀木桶 → 有声音"这条最简单的游戏反馈，今天写不出来。

⚠ 注意方向：`engine/script` **不认识 runtime**，所以音频回调必须和 `setHealth` 一样
走 `std::function` 注入，不能 include runtime 的头。

### 1.6 第三方：`stb_vorbis.c` 在仓库里，但**没进编译**

`CMakeLists.txt:102` 是：

```cmake
add_library(stb INTERFACE)   # 纯头文件
```

`third_party/stb/stb_vorbis.c` 虽然存在，**没有任何 target 编译它** → 今天想解 OGG 是
"文件在、符号没有"。要启用必须照 `ufbx` 的做法单独建一个 C 库
（`CMakeLists.txt:112` 的注释已经把原因写清楚了：**纯 C 源不能混进 C++ 的 GLOB**）。

**miniaudio 不在仓库里**，而且本机取不到：

```
curl --proxy 127.0.0.1:31180 https://raw.githubusercontent.com/...  → 连接失败
curl --proxy 127.0.0.1:31181 https://raw.githubusercontent.com/...  → 连接失败
git config: http.proxy=31180 / https.proxy=31181（两端口不一致，项目笔记里记的老坑）
```

这条**是硬阻断**：依赖不可获取 → 不能把 G14 的地基压在"必须先下到一个 4.2 MB 单头文件"上。
§3.4 给两个方案的决策表。

### 1.7 运行时循环：没有音频相位

`runtime/GameLoop.h` 的相位顺序是钉死的四段：

```
① pollInput  ② fixedUpdate  ③ variableUpdate  ④ render
```

没有音频。⚠ 但这**不是缺陷，是正确设计**：音频不能挂在 `fixedUpdate`（会被
`timeScale` 缩放）也不能挂在 `variableUpdate`（会被掉帧拉长）。音频的推进由**声卡时钟**
驱动，主线程每帧只做"同步世界 → 发命令"。

### 1.8 测试设施：零

- `tools/` 下 102 个脚本，**没有一个与音频有关**；
- `assets/` 下 **没有任何音频文件**（`models` 2.1 MB / `textures` 576 KB / `ui` 612 KB，
  音频 0）；
- 没有生成音频素材的脚本（对比：`gen_sdf_atlas.py` / `make_bc_dds.py` / `make_sss_lut.py`
  都是"离线生成 + 提交产物"的先例）；
- `Game.exe --selftest` 有 7 组（states / determinism / input / character / contact /
  camera / player / hud），没有 audio。

### 1.9 与"游戏的定义"差多远

`docs/ship_roadmap.md:51` 定义的总验收里有一条：

> 冷启动 → 主菜单 → 选/加载关卡 → 操控角色 → 与场景/敌人交互
> （物理 + 脚本 + **音效** + 动画反馈）

`:62` 的判据是 **"有声音（至少：环境音 + 脚步 + UI 音）；音频设备被打开，波形非零"**。

所以音频是 **P0 六项之一**（`:37`），不是"锦上添花"。而 `:94` 的现状表里，
音频那一格写的就是 **零**。

---

## 2 · 七个断点（D0~D6）

| 断点 | 层 | 今天的事实 | 补齐后能做什么 | 验收形态 |
|---|---|---|---|---|
| **D0** | 设备 | 没有任何设备代码；`audioDevice` 空挂 | 打开声卡、拿到真实采样率/声道、设备消失能降级 | `AUDIO-INIT backend=... device=... sr=48000 ch=2` |
| **D1** | 解码 | WAV/OGG 都不能读；`stb_vorbis.c` 未编译 | 文件 → 交错 float PCM（含 24-bit） | `AUDIO-CLIP frames=22050 sr=44100 ch=2` |
| **D2** | 混音 | 没有任何混音 | 64 声部、4 总线、重采样、削波保护 | 离线渲染出的 float 波形（可 `memcmp`） |
| **D3** | 空间 | 没有 listener / 距离 | 距离衰减、左右定位、跟随相机 | 距离序列增益**严格递减**；左声道能量 > 右 |
| **D4** | 资产 | `AssetManager` 全是 GPU 资源 | CPU 侧 clip 缓存、按路径复用、流式 | 同一路径加载两次 → 同一指针；堆不涨 |
| **D5** | 组件 | 无组件、无序列化、无 UI | 实体挂音源、存档带上、Play 副本自动生效 | JSON 往返逐字段相等 + `INS-*` 锚点 |
| **D6** | 内容 | Content 双击无反应、脚本不能发声 | 双击试听、`playSound()`、测试素材 | `AUDIO-PREVIEW` / `AUDIO-PLAY` 锚点 |

**依赖关系**：`D0 ← D2 ← D3`，`D1 ← D4 ← D6`，`D2 ← D5`（组件只是 D2/D3 的输入源）。
⚠ 注意 **D0 不是 D2 的前置**：混音器可以完全脱离设备先写完并验收（这正是 M0 的意义）。

---

## 3 · 关键技术判断（含对 `ship_roadmap` G14 的四条纠正）

### 3.1 ⚠⚠ 纠正一：不要用 `ma_engine` / `ma_sound`，用"设备 + 自研 Mixer"

`ship_roadmap` 的 G14 原文写"声部池（一次性音效复用 `ma_sound`）"，又写流式走
"`ma_sound` 的 streaming 模式"。**这条要改**，原因是可测性：

- `ma_engine` 的混音发生在它自己的线程里，中间缓冲区**不可访问** →
  "3D 衰减对不对""ducking 降到多少 dB""有没有爆音"全部只能靠耳朵；
- `ma_sound` 的状态机（started / at-end / looping）**不可离线推进** →
  无窗口自检里没法"跑 100 个 block 然后断言"，只能 sleep 等真实时间；
- 一旦把混音逻辑寄存在第三方结构里，后面想加"混响区""遮挡""音频事件"都要绕它的模型走。

**正确切法**（也是本路线图的骨架）：

```
  ┌─────────────── 主线程 ───────────────┐        ┌──── 音频线程 ────┐
  │ AudioSystem::update(dt)              │        │ 每 block 调用    │
  │   · 扫场景里的 AudioSourceComponent  │        │ mixer->mixBlock()│
  │   · 更新 listener / 位置 / 参数      │ ─────► │   · 读命令队列   │
  │   · push 命令到 SPSC 队列            │  队列  │   · 混 64 声部   │
  └──────────────────────────────────────┘        │   · 写 float 输出│
                                                  └──────────────────┘
  ┌────────── 自检 / 离线渲染（同一条代码路径）──────────┐
  │ mixer->mixBlock(out, 512)  连调 200 次 → 逐位可复现  │
  └──────────────────────────────────────────────────────┘
```

**miniaudio 的价值收缩为"开设备"**（枚举 / 格式协商 / 热插拔），混音、总线、3D、
流式缓冲全在我们自己的 POD 里。这样 `--selftest audio` 可以在**没有任何声卡**的机器上
跑完整断言 —— 和 `verify_hud` 用 `--selftest hud` 靠纯数据层验收是同一个套路。

### 3.2 ⚠⚠ 纠正二："默认关"应改成"自动化默认关"

`ship_roadmap` 的 G14 原文写：

> 关闭音频（`MYVK_AUDIO=0`）→ 其余系统行为**逐位不变**（默认关 = 基线不变）

"默认关"在**产品**上是不能交付的（玩家开游戏没声音）。而且 `MYVK_AUDIO` 不合
`MYVK_RUNTIME_*` 前缀，会被 `check_feature_surface.py` 的 D 组判为"裸 env 漏网"。

改成两条：

1. **产品默认 `audio.enabled = true`**；设备打不开 → `AUDIO-DEVFAIL` + 静默降级，不崩；
2. **自动化默认关**：`--selftest` / `--frames N`（无窗口或有限帧）以及所有回归脚本
   默认注入 `MYVK_RUNTIME_AUDIO=0`。于是既有 30+ 个用例的基线**逐位不变**，
   而真实玩家默认有声。

⚠ 反过来也要有一条断言：**打开音频时，物理/输入/UI 的行为必须与关闭时逐位相同**
（音频只读世界）。这条比"默认关"更强，且能抓住"音频顺手改了什么"。

### 3.3 ⚠ 纠正三：音频**不进** `AssetManager`

G14 原文写"`engine/assets/AssetManager`（加音频加载路径）"。**不建议**：

`engine/assets/AssetManager.h` 的公开接口全是 GPU 资源（`loadTexture` / `loadModel` /
`registerMesh` / `registerTexture`），构造时拿的是 `VkDevice` + `VkCommandPool`。
把 CPU 侧的 PCM 塞进去会造成两个后果：

- 音频（尤其流式音乐）会被 GPU 资源的缓存策略（LRU / 常驻）管理，语义错配；
- 想跑 `--selftest audio` 就得先有 Vulkan 设备 → **无窗口自检直接不可能**。

**正确位置**：`engine/audio/AudioClipCache`（按路径 key 的 `unordered_map`，所有权在
`AudioSystem`），跟 `ThumbnailCache` 是"另一个维度的缓存"同一个道理。

### 3.4 后端选型：A（miniaudio）vs B（自研 WASAPI）

| | **A · miniaudio 低层 `ma_device`** | **B · 自研 WASAPI 后端** |
|---|---|---|
| 获取 | 需外网下载 4.2 MB 单头文件（**本机实测不通**） | 零依赖，系统 SDK 自带 |
| 平台 | Windows / Linux / macOS 全支持 | 仅 Windows（Linux 要另写 ALSA） |
| 解码 | 内置 wav/mp3/flac/vorbis（省掉 D1 的大半） | WAV 自写（~120 行）+ OGG 用 `stb_vorbis`（已在仓库） |
| 编译影响 | 一个大 TU，约 +20~30 s；要单独 target 关警告 | 约 +500 行，只链 `ole32`/`avrt`/`ksuser` |
| 设备枚举 | 现成（`ma_context_get_devices`） | `IMMDeviceEnumerator`（~60 行） |
| 热插拔 | 内置 `ma_device_notification` | 自己挂 `IMMNotificationClient`（~80 行）或"失败即重开" |
| 与项目气质 | 引第三方大件 | 与"手写 Vulkan / 手写物理"一致 |
| 切换成本 | — | 设备层是接口，后补 `MiniaudioBackend` 约 200 行 |

**推荐 B**，理由是**当前环境的硬约束**（取不到外网）+ **混音本来就必须自研**
（§3.1）+ 只 Windows 就够（现状 `_WIN32` 分支只出现在 7 个文件里，构建入口是
`make.cmd` + MSVC/MinGW）。

⚠ 但**设备层必须是接口**（`engine/audio/AudioBackend.h`），把"开设备"隔离成一件事：
将来要跨平台或想要 miniaudio 的解码器，只是多实现一个类，`Mixer` 一行不动。
如果用户手上有网、且希望以后上 Linux，选 A 也完全可行 —— 差异只在 G14-0/G14-1，
G14-2 之后两条路完全重合（见 §5 的"选 A 时的差异点"）。

### 3.5 线程模型：为什么必须是 SPSC 命令队列 + 槽位/代际

音频回调运行在**声卡线程**（WASAPI 事件线程）。三条铁律：

1. **回调里不分配内存、不加锁、不打日志、不碰 `World`**。任何一条破掉都会在低负载下
   看起来正常、在掉帧/换设备时爆成卡顿或崩溃（实时安全的经典形态）。
2. **主线程 → 音频线程只走一个无锁 SPSC 环形队列**，元素是**定长 POD**
   （`PlayCmd{ clipId, slot, gen, vol, pitch, bus, spatial, pos }`）。
   不能用 `std::function`（会堆分配）。
3. **声音句柄 = `(slot, generation)`**。槽位会复用，只有槽号会导致 ABA
   （"停掉的那一声"停到了"刚抢进来的新一声"）。`generation` 每次分配 +1，
   命令里带旧代际 → 音频线程直接丢弃。

反向（音频线程 → 主线程）只有两样东西：**日志环形缓冲**（固定 256 条 × 128 B，
主线程每帧 drain 到 `VK_LOG`）和**统计快照**（`std::atomic` 的 RMS / voice 数）。

### 3.6 时钟：音频不随 `timeScale` 停；暂停 ≠ 静音

| 事件 | 物理 | 音频 |
|---|---|---|
| `timeScale = 0`（子弹时间） | 停 | **照常**（音乐不该变慢） |
| 暂停菜单打开 | 停 | BGM 继续（或 duck 到 30%），SFX 全部淡出 |
| 窗口失焦 | 降帧 | 照常（除非显式 `muteOnFocusLost`） |

实现上：`Mixer` 的时间推进量来自**设备每 block 的帧数**，和 `dt` / `timeScale` 无关。
`GameLoop` 只负责调用 `AudioSystem::update(dt)`（用 `dt` 做**包络插值**，不用它推进播放位置）。

⚠ 反过来的硬约束：**音频永不写 `Transform` / `RigidBody` / 组件状态**。
`verify_hud` 的"暂停时位置逐位不变"是拿 `GAME-PLAYER-POS` 一条不新增来判的，
只要音频碰了世界，那条断言会莫名其妙变红。

### 3.7 3D 衰减与 panning：两个必须写死的细节

**衰减**（`minDist` / `maxDist` / `rolloff`）：

```
d <= minDist              → gain = 1
minDist < d < maxDist     → gain = inv(d) * fadeOut(d)
d >= maxDist              → gain = 0

inv(d)    = minDist / (minDist + rolloff * (d - minDist))
fadeOut(d)= 1                                   d <= 0.9 * maxDist
            (maxDist - d) / (0.1 * maxDist)     0.9*maxDist < d < maxDist
```

⚠⚠ **`inv(d)` 在 `maxDist` 处并不为 0**（它只是很小）。直接"超过 maxDist 就静音"会在
跨越边界那一帧产生一个**阶跃**，听感是"啪"的一声。所以必须乘一段收尾斜坡
（推荐最后 10% 线性到 0）。这条不写进代码注释，下一个人一定会踩。

**panning**：把"源相对 listener 的右向量投影"归一化到 `[-1, 1]` 得到 `x`，用**等功率**：

```
L = cos((x + 1) * π / 4)     R = sin((x + 1) * π / 4)
```

⚠ 不要用线性 `L = (1-x)/2`：中间位置会 **-3 dB 塌陷**（单声道源听起来"凹进去"）。
等功率在中间是 `0.707 / 0.707`，总功率恒定。

### 3.8 流式：环形缓冲 + 主线程预读，不做"全解码"

一首 4 分钟 44.1 kHz 立体声 16-bit 的曲子 = **42 MB**。三首背景乐就 126 MB 常驻，
这在"要出包"的项目里不可接受，而且加载时会卡一次几百毫秒。

做法：`AudioClip` 有两种模式：

| 模式 | 内存 | 适用 | 加载 |
|---|---|---|---|
| **InMemory** | 全量 PCM | 音效（< 2 s，几百 KB） | 一次性解码 |
| **Streaming** | 3 s 环形缓冲（~1.1 MB） | 音乐 / 长环境音 | 边播边读 |

⚠ 流式**不能在音频回调里读文件**（IO 会阻塞回调 → 断音）。做法是**主线程每帧预读**：
`AudioSystem::update()` 里把每个流式 clip 的环形缓冲填到 3 s。60 fps 下每帧填 16 ms 的量，
余量 180 倍 → 主线程卡 2 秒都不会断（3 s 缓冲兜底）。

⚠ 代价：`timeScale = 0` 时主线程仍在跑 `update`，所以**暂停时音乐照样能流**——
这正是我们要的（暂停菜单要有 BGM）。

### 3.9 重采样：为什么必须有，最低质量要求

设备几乎总是 **48 kHz**，而素材常见 **44.1 kHz**。不重采样 = 变调 8.8%（`44100/48000`），
"叮"会变成"咚"。

最低要求：**线性插值**（`s = s0 + (s1-s0)*frac`），步进 `= clipSr / deviceSr * pitch`。
这会有混叠，但对本项目（音效 + 环境音）完全够用；不要为了"专业级"过早引入
sinc/多相滤波器（成本高、收益低）。

⚠ 两个细节：
1. `pitch` 变化必须**同时**改步进（否则变调不变速）；
2. `loop` 的环绕点要按**源采样**算，不能在重采样之后算 —— 否则每次绕回都有一个
   `frac` 的相位跳变（听感是轻微的"哒"）。

### 3.10 为什么不给每个实体一个 `ma_sound` / 一个独立 voice

"每个 `AudioSourceComponent` 一个常驻 voice"在 200 个实体时就是 200 路混音，
而同一时刻真正在响的通常 < 10 路。做法：

- **组件是配置**（clip / loop / volume / 3D 参数 / bus），不是"一个正在播放的声音"；
- `playOnAwake` 的组件在实体激活时**申请**一个池槽，播完**释放**；
- 池满（64）→ **抢最旧**（起始时间最早的），并给被抢的那个 10 ms 淡出，
  避免"啪"（断言上体现为相邻样本差 < 0.5）。

### 3.11 `.vaudio` 的风险：不要再造一个空挂

`Add ▸ Audio` 已经存在（`:974`），建出来的 `.vaudio` 没有任何格式定义。
如果这次只做"能建文件"，就是**第四个空挂**。

处置顺序：

1. **先做真实文件的试听**（`.wav` / `.ogg` 双击）—— 零格式设计、立刻可用；
2. `.vaudio` 的语义（引用 clip + 总线 + 衰减预设）**留到 G14-X**，且一旦做就必须
   "双击 = 试听它引用的 clip"，不能是空壳；
3. 若最终决定不做预设，就把 `addKinds()` 里那一项**删掉**（并同步
   `CB-ADDITEM` 的验证脚本）—— 宁可没有，也不要一个点了没反应的菜单项。

---

## 4 · 目标架构

### 4.1 模块表

| 文件 | 职责 | 依赖 | 可无窗口测 |
|---|---|---|---|
| `engine/audio/AudioFormat.h` | `AudioFormat{sampleRate, channels, frames}`、`BusId` 枚举 | — | ✅ |
| `engine/audio/AudioClip.{h,cpp}` | WAV/OGG 解码 + 两种模式（内存/流式）+ 元信息 | `stb_vorbis` | ✅ |
| `engine/audio/AudioClipCache.{h,cpp}` | 按路径缓存 `AudioClip`，引用计数 | 上面 | ✅ |
| `engine/audio/Mixer.{h,cpp}` | **纯数据**：voice 池 / 总线 / 3D 衰减 / panning / ducking / 重采样 | `AudioClip` | ✅ ★核心 |
| `engine/audio/AudioBackend.h` | 设备抽象：`open / close / enumerate / sampleRate / name` | — | 接口 |
| `engine/audio/NullBackend.{h,cpp}` | 确定性后端（按 `steady_clock` 拉 block，或手动 pump） | 上面 | ✅ |
| `engine/audio/WasapiBackend.{h,cpp}` | Windows 实现（事件驱动 + shared mode） | `ole32` / `avrt` | ❌（只能冒烟） |
| `engine/audio/MiniaudioBackend.{h,cpp}` | 选 A 时的实现（**可选**） | miniaudio | ❌ |
| `engine/audio/AudioSystem.{h,cpp}` | 门面：读场景 → 更新 listener/位置 → 发命令；总线增益；统计 | 以上全部 | ✅ |
| `engine/audio/AudioSelfTest.{h,cpp}` | `--selftest audio` 的全部断言 | 以上 | ✅ |
| `runtime/ui/AudioPreview.{h,cpp}` | Content 试听（编辑器用，播放/停止/进度） | `AudioSystem` | 半（要 ImGui） |

⚠ `engine/audio/` 会被 `CMakeLists.txt:143` 的 `GLOB_RECURSE engine/*.cpp` **自动收进来**
—— 不需要改 target。但 `stb_vorbis.c`（纯 C）和可选的 `miniaudio.c` **必须**单独
`add_library`（照 `ufbx` 的做法，`:112`），不能塞进 C++ GLOB。

### 4.2 所有权与生命周期

```
RuntimeApp / EditorApp
   └── AudioSystem（每 App 一份，栈上成员）
         ├── AudioClipCache（CPU 侧）
         ├── Mixer（POD 数组，无堆分配 after init）
         └── std::unique_ptr<AudioBackend>（进程内**唯一**打开的设备）
```

- **不做全局单例**：项目里没有"引擎级单例"的先例（`Renderer` / `PhysicsWorld` 都是
  成员），而且单例会让"编辑器 Play 与运行时同时存在"的情况变成设备争用。
- **设备打开失败不是错误**：`AudioSystem` 进入 `Silent` 模式，所有 API 变成空操作，
  `AUDIO-DEVFAIL` 打一次 WARN，之后**不再重试**（避免每帧刷日志）。
  `MYVK_RUNTIME_AUDIO_RETRY=1` 可开"每 2 s 重试一次"（用于拔插耳机）。

### 4.3 一帧内的时间线（主线程）

```
pollInput        （UI tick / 输入）
fixedUpdate      （物理 / 玩法；**音频不在这里**）
variableUpdate   AudioSystem::update(dt):
                  1) drain 音频线程的日志环形缓冲 → VK_LOG
                  2) 扫场景：AudioSourceComponent 的位置 / 参数 → 写进命令
                  3) listener = 相机（或 AudioListenerComponent）
                  4) 流式 clip 预读填到 3 s
                  5) 每 60 帧打 AUDIO-STATS
render           （与音频无关）
```

### 4.4 一声"命中"的完整路径

```
脚本 damage(30)
  → ScriptHost::playSound("audio/hit.wav")            [engine/script]
    → AudioSystem::playOneShot(clipPath, bus=SFX, pos=实体头顶)   [engine/audio]
      → AudioClipCache::get(path)  ── 未加载则同步解码（< 2 s 的音效）
      → Mixer::allocVoice()       ── 池满 → 抢最旧 + 10 ms 淡出
      → 命令入队 { slot, gen, clipId, vol, bus, pos }
                                  ──── SPSC ────►  音频线程
                                                  每个 block:
                                                    mixer.mixBlock(out, frames)
                                                      读命令 / 算 3D 增益 / 等功率 pan
                                                      重采样 / 累加到总线 / 削波保护
                                                      → 写 float 输出 → WASAPI
```

---

## 5 · 路线图 G14-0 ~ G14-9

> 编号沿用 `ship_roadmap` 的 **G14-x**（与 `R5-1` 的风格一致），不另立前缀。

### G14-0 · 选型落地与骨架（规模 **S**）

- **目标**：把"能不能引第三方"这件事一次性定死，并让 `engine/audio/` 能编进两个 exe。
- **改动点**
  1. 拍板 A（miniaudio）或 B（自研 WASAPI）。默认 **B**（理由见 §3.4）；
  2. 建 `engine/audio/` 目录 + `AudioBackend.h` 接口 + `NullBackend`（先空实现）；
  3. `CMakeLists.txt`：若选 A，`add_library(miniaudio STATIC third_party/miniaudio/miniaudio.c)`
     + `target_compile_options(miniaudio PRIVATE /W0)`（第三方头在 `/W3` 下会刷几百条）；
     若选 B，给 `VennEngine` 链 `ole32 avrt`（`#if WIN32`）；
  4. `assets/audio/` 目录 + 在 `VENN_ALWAYS_ASSET_DIRS`（`CMakeLists.txt:537`）加一行
     `"${CMAKE_CURRENT_SOURCE_DIR}/assets/audio|assets/audio"`。
     ⚠ 必须进"每次都拷"那一档：`POST_BUILD` 只在重链时跑，改音频资产会不生效（老坑）。
- **验收**
  - 两个 exe 都能链接通过，`Game.exe --selftest audio` 返回 `AUDIO-TOTAL pass=0 fail=0`；
  - `grep -c "audio" build/bin/` 之类的"行为验证"不成立 —— 用 `AUDIO-INIT` 锚点证明代码进二进制。
- **选 A 时的差异点**：多一个下载步骤 + 校验（见 §7 风险 R1），`AudioClip` 的解码可以直接
  用 `ma_decoder`，D1 的工作量减半。

### G14-1 · 解码：`AudioClip` + 素材生成器（规模 **M**）

- **目标**：文件 → 交错 float PCM，且**每一段都能断言**。
- **改动点**
  1. `AudioClip`：WAV 解析（RIFF/WAVE，`fmt ` + `data`）。支持
     PCM 8/16/24/32-bit + IEEE float32 + `WAVE_FORMAT_EXTENSIBLE`；
     ⚠ **24-bit 是打包的 3 字节**（最常见的手写 bug：按 4 字节读会整体错位）；
     ⚠ `data` 长度可以是 `0xFFFFFFFF`（流式录制文件）→ 按"文件剩余字节"兜底；
     统一解成 **float32 交错**（后续所有环节只认这一种格式，省掉一堆分支）；
  2. OGG：`stb_vorbis_open_memory` + `stb_vorbis_get_samples_float_interleaved` 增量读。
     ⚠ 需要新建 C 库（`third_party/stb/stb_vorbis.c` 今天**没被编译**）。⚠ 本机无 ffmpeg
     → **生成不出 ogg 测试素材**，所以 OGG 只做"有文件就能读"，不进自动化验收（G14-X）；
  3. `tools/gen_audio_assets.py`（用标准库 `wave`，离线、可复现）生成：
     `tone_440_stereo.wav`(1 s) / `tone_1k_mono.wav`(0.5 s) / `silence.wav`(0.25 s) /
     `click.wav`(20 ms 带包络) / `loop_2s.wav`(无缝)。
     ⚠ **10 分钟长文件不提交**（105 MB），由脚本按需生成到 `../_venn_verify/`；
  4. `AudioSelfTest`：`AUDIO-CLIP path=... frames=... sr=... ch=... bytes=... stream=0`。
- **验收**（全部可自动化）
  - 16-bit 立体声 1 s / 44.1 kHz → `frames=44100 ch=2 sr=44100`；
  - 解析出的样本与生成时的**第一个正弦周期**逐点比对（`|Δ| < 1e-4`）；
  - 24-bit 文件解析正确（对照组：同一信号导出 16-bit 与 24-bit，归一化后 RMS 差 < 0.1%）；
  - `silence.wav` → 全部样本 = 0（不是 "≈0"）；
  - 文件不存在 / 魔数错 → 返回空 clip + `AUDIO-CLIP-FAIL`，**不崩、不刷屏**。

### G14-2 · Mixer：声部池 / 总线 / 重采样 / 离线渲染（规模 **L**）

- **目标**：**不开设备**就能渲染出确定的波形。这是整条路线图的地基。
- **改动点**
  1. `Mixer` 只依赖 `AudioClip`，不 include 任何 OS 头；
  2. 声部池 64 槽（`kMaxVoices`），每槽 `{ clipId, gen, cursor(源采样位置, float), step,
     vol, pan, bus, loop, startFrame }`；
  3. 总线 `Master / Music / SFX / Voice`，增益**乘算**（不是覆盖）；`mute` 单独一位；
  4. `mixBlock(float* out, uint32_t frames)`：每样本累加 4 条总线 → 主总线 →
     **软削波**（`tanh` 或 `x/(1+|x|)`）而不是硬夹（硬夹会爆）；
  5. 命令队列（SPSC，定长 POD）+ 槽位代际；
  6. `renderOffline(float* out, uint32_t frames)`：自检专用，直接循环调 `mixBlock`。
- **验收**
  - **确定性**：同参数跑两次 `renderOffline`，**`memcmp` 逐位相同**；
  - 单声部 440 Hz 正弦 → 过零率 ≈ `440 * 2 / (sr / frames)`（容差 1%）；
  - 音量 0.5 的输出 RMS = 音量 1.0 的 0.5 倍（容差 1%）；
  - `SFX` 总线设 0 → SFX 声部贡献 = 0，而 `Music` 声部 RMS 不变（对照断言）；
  - 64 声部 + 第 65 个 → 最旧槽被替换（`AUDIO-STATS` 或自检日志里的 voice id），
    且相邻样本差 **< 0.5**（无爆音）；
  - **削波保护**：256 个满幅声部同时响 → 输出 `|x| <= 1.0` 全样本成立；
  - 重采样：44.1 kHz clip 在 48 kHz 设备上播放 1 s → 输出正好 `48000` 帧消耗完
    clip（`cursor` 到位，误差 < 1 帧）。

### G14-3 · 设备后端 + 实时回调 + 失效恢复（规模 **L**）

- **目标**：真的从扬声器出声，且在"没声卡 / 被独占 / 拔掉"时不崩。
- **改动点**
  1. `WasapiBackend`：`CoInitializeEx(MULTITHREADED)` → `IMMDeviceEnumerator` →
     `GetDefaultAudioEndpoint(eRender, eConsole)`（或按 `audioDevice` 名字枚举匹配）→
     `IAudioClient::Initialize(SHARED, EVENTCALLBACK)` → 独立线程 `WaitForSingleObject` →
     `GetCurrentPadding` / `GetBuffer` / 写 float / `ReleaseBuffer`；
  2. ⚠ **shared mode 必须接受设备的 mix format**（通常 float32 / 48 kHz / 2 ch）。
     不要试图强行设 44.1 kHz —— `IsFormatSupported` 失败时要用 `GetMixFormat` 的结果，
     重采样由 `Mixer` 负责（§3.9）；
  3. ⚠ 设备名匹配失败 → `AUDIO-DEVFAIL reason=device-not-found` + **回落到默认设备**，
     并打一行 `AUDIO-INIT fallback=1`（否则又变成"配了不生效"）；
  4. `NullBackend`：按 `steady_clock` 每 10 ms pump 一个 block（用于"设备回调路径"的冒烟，
     不参与确定性断言）；
  5. `AudioSystem` 的设备状态机：`Closed → Running → Failed`（`Failed` 下所有 API 空操作）。
- **验收**
  - 有设备：`AUDIO-INIT backend=wasapi device=<真名> sr=48000 ch=2`，播 2 s 后
    `AUDIO-STATS voices>=1 rms>0`；
  - `MYVK_RUNTIME_AUDIO_DEVICE=__none__` → `AUDIO-DEVFAIL` + `muted=1`，**进程正常退出**；
  - 关音频（`MYVK_RUNTIME_AUDIO=0`）→ `AUDIO-INIT enabled=0`，一个 block 都不混；
  - ⚠ **音频回调不阻塞**：`AUDIO-STATS cpu=<1%`（连续 600 帧 p99 < 5%）；
  - 拔设备模拟（脚本 kill 设备线程）→ 不崩，日志一条 `AUDIO-DEVFAIL reason=device-lost`。

### G14-4 · 3D 空间化 + 场景同步（规模 **M**）

- **目标**：声音"从哪来"能被听出来，也能被量出来。
- **改动点**
  1. `AudioListenerComponent`（挂相机；**没有就用 `activeScene().camera()`** ——
     运行时与编辑器 Play 都成立）。多个 listener → WARN + 取第一个；
  2. `AudioSystem` 每帧：遍历 `AudioSourceComponent` → 算方位/距离 →
     写进 voice 的 `pos/pan/gain`（⚠ **写进自己的数组，不写组件**）；
  3. 衰减 + 等功率 pan（§3.7 的公式，含 10% 收尾斜坡）；
  4. `AudioSourceComponent{ clip, loop, volume, pitch, spatial, minDist, maxDist, rolloff,
     bus, playOnAwake, streaming }` + `AudioListenerComponent{}` 加进 `Components.h`。
- **验收**
  - 距离序列 `{minDist, 2, 4, 8, maxDist}` 的增益**严格递减**，且 `maxDist` 处 **= 0**；
  - `d = 2*maxDist` → 增益 0（不是"很小"）；
  - 源在 listener **左侧** → `L_rms / R_rms > 1.5`；正前方 → `|L-R| / max(L,R) < 0.05`；
  - 近/远**交叉点无阶跃**：扫描距离时增益曲线相邻采样差 < 0.02（抓"啪"）；
  - `spatial=false` → 增益恒为 `volume`，与距离无关（对照断言）。

### G14-5 · 总线 / ducking / 音量 / `game.json` 接线（规模 **M**）

- **目标**：让"音量"真的能调，让 `audioDevice` 不再是空挂。
- **改动点**
  1. `game.json` 加块（**只读**出厂默认，运行时不写它）：
     ```json
     "audio": {
       "enabled": true,
       "device": "default",
       "master": 1.0, "music": 0.7, "sfx": 1.0, "voice": 1.0,
       "mute": false,
       "muteOnFocusLost": false,
       "maxVoices": 64
     }
     ```
     ⚠ 保留旧的顶层 `audioDevice` 作为**兼容别名**（写进文档 + 读到就打一行 DEPRECATED），
     不要直接删 —— 用户机器上的 `game.json` 可能已经被改过；
  2. `RuntimeConfig` 加 `AudioSettings` 子结构 + `MYVK_RUNTIME_AUDIO*` 全套 env；
  3. **ducking**：`Voice` 总线有活跃声部时，`Music` 增益按包络下降
     （attack 50 ms / depth 可配 / release 400 ms）；
  4. 音量变化的**平滑**：目标值变化时 50 ms 线性过渡（直接改增益会有"拉链噪声"）。
- **验收**
  - `MYVK_RUNTIME_AUDIO_MASTER=0.5` → 输出 RMS 是 1.0 时的 0.5 倍（1%）；
  - `SFX=0` 且 `MUSIC=1` → 音效静音、音乐不变（**两条断言**，不能只测一条）；
  - ducking：触发 voice 后 100 ms，Music 增益 ≤ `-6 dB`；释放后 500 ms 内回到 ±0.5 dB；
  - `audioDevice` 填真名 → `AUDIO-INIT device=<真名> fallback=0`；填假名 →
    `AUDIO-DEVFAIL reason=device-not-found` + `fallback=1`；
  - `mute=true` → 输出全 0，但 `voices` 仍 > 0（**静音不等于停播**，恢复到有声要能接上）。

### G14-6 · 流式音乐（规模 **M**）

- **目标**：一首 4 分钟的歌只占 1.1 MB 常驻。
- **改动点**
  1. `AudioClip{ mode = InMemory | Streaming }`；流式带一个 3 s 环形缓冲；
  2. `AudioSystem::update()` 每帧预读（§3.8）；⚠ **音频回调里绝不读文件**；
  3. `loop` / `seek`（`seek` 要清空环形缓冲 + 重置 `cursor`，并且**淡出 5 ms**）；
  4. 流式 clip **不进** `AudioClipCache` 的强引用（播完就释放解码器）。
- **验收**
  - 10 分钟 wav（脚本生成到 `_venn_verify/`）播放 30 s：`AUDIO-STATS mem` 增长 **< 2 MB**；
  - 同一文件用 `InMemory` 模式 → 内存占用 > 100 MB（对照组，证明流式真的在流）；
  - 循环 3 圈后 `cursor` 回绕点无样本跳变（`|Δ| < 0.05`）；
  - 主线程人为卡 1 s（`Sleep`）→ 音乐**不断**（缓冲 3 s 兜底），日志 `AUDIO-STREAM underrun=0`。

### G14-7 · `AudioSourceComponent` + 序列化 + Inspector（规模 **M**）

- **目标**：编辑器里能挂上、能存档、Play 之后还在。
- **改动点**
  1. `Components.h` 加两个组件（G14-4 的结构定义，这里补"能用"）；
  2. `SceneSerializer`：写端 `je["audio"] = {...}`（照 `:1509` 的 script 写法）、
     读端 `:1736` 一带（照 script 的读法，`value("...", 默认)`）；
     ⚠ **只写非默认值**？不 —— 音频字段少，全写（读端有默认，旧场景不受影响）；
  3. `InspectorPanel`：
     - `kTabs[]` **不动**（不加页签，避免改枚举 + 图标 + 落盘 id + 两处 `static_assert`），
       把音频段放 **Tool** 页签（"Script behavior / add & remove components" 的语义位）；
     - `sectionKeywords()`（`:99`）加 `{"Audio Source", "audio sound clip volume pitch
       loop spatial distance rolloff attenuation bus"}` —— ⚠ 每个字段都要列，
       否则搜 "rolloff" 会看到 "no component matches"（粒子那次的原话）；
     - Add Component 菜单（`:5442` 一带）加 "Audio Source"；
     - 用 `sectionHeader("Audio Source", true, e, "Audio")` + `structuralEdit` 保证可撤销；
     - 文件名输入框旁加一个 **▶ 试听** 按钮（直接复用 `AudioSystem::playOneShot`）。
- **验收**
  - JSON 往返：写 → 读 → **逐字段相等**（含 `bus` 枚举、浮点精确相等）；
  - 旧场景（无 `"audio"` 键）→ 加载后没有该组件，且不报错；
  - 编辑器：`INS-AUDIO-HEADER` 矩形存在；改音量 → `Ctrl+Z` 能撤销（`verify_undo_settings` 风格）；
  - **Play 副本**：挂组件 → Play → 脚本读 `audio` 组件仍在（证明序列化接上了，§1.4 的陷阱）。

### G14-8 · Content 试听 + 脚本内置函数（规模 **M**）

- **目标**：从"能挂"到"能听见"闭环。
- **改动点**
  1. `ContentBrowser::openEntry()`（`:1554`）加分支：
     `.wav/.ogg` → `AudioPreview::toggle(path)` → 锚点 `AUDIO-PREVIEW path=... state=playing|stopped`；
     再点一次停（同一时刻只试听一个）；
  2. 试听中的行画一个小喇叭角标（可选，但要有 `AUDIO-PREVIEW-ROW` 锚点供脚本找矩形）；
  3. 脚本内置函数（照 `setHealth` 的注入方式，`engine/script` **不** include runtime）：
     `playSound("audio/click.wav")` / `stopAllSounds()` /
     `setBusVolume("music", 0.3)`；
  4. `ScriptHost` 加 `std::function<void(const std::string&)> playSound` 等（⚠ 别忘
     `#include <functional>` —— U8 踩过一次 C2039）。
- **验收**
  - 双击 wav → `AUDIO-PREVIEW state=playing` + `AUDIO-STATS voices>=1`；再双击 → `state=stopped`；
  - 脚本 `playSound` → `AUDIO-PLAY clip=audio/click.wav bus=sfx`；
  - 试听一个**不存在**的文件 → 通知条报错、不崩、不刷屏；
  - ⚠ `.vaudio` 双击：**要么**进"未实现"通知，**要么**（若做了预设）真能播；
    不允许"静默无反应"（这是 §3.11 的判据）。

### G14-9 · 验收收口 + 回归 + 文档（规模 **S**）

- **目标**：这条链路从此有机器守着。
- **改动点**
  1. `tools/verify_audio.py`（四组，见 §8.1）；
  2. `run_regression.py`：`DEFAULT` 加 `verify_audio`；`TRIGGERS` 加
     `engine/audio/**`、`runtime/RuntimeConfig.*`、`runtime/RuntimeApp.*`、
     `assets/audio/**`（⚠ 不进快照 → 改素材要手动跑）、`engine/script/ScriptEngine.*`；
  3. `check_regression_map.py` 的 fixture **同步改**（改表 + 改 fixture 是原子操作）；
  4. `docs/ship_roadmap.md` 的 G14 段：加"详见 `docs/audio_roadmap.md`" + 四条勘误指向；
  5. `MEMORY.md` / 当日日志记"音频七条纪律"（§12）。
- **验收**：`check_regression_map.py` PASS；`verify_audio.py` 末行 `结果: PASS`。

### G14-X（可选，不在本轮）

| 项 | 价值 | 代价 |
|---|---|---|
| `.vaudio` 预设资产（总线/衰减模板） | 美术不用手调每个声源 | 要定义格式 + 解析 + Content 双击 |
| 混响区（`AudioReverbZone`） | 室内/洞穴的氛围差别巨大 | 需要一个简易 IR 或 Schroeder 混响（~300 行） |
| 遮挡（射线检测 → 低通） | 隔墙的声音"闷" | 每帧 N 条射线 + 一个单极点低通 |
| 多普勒 | 快速掠过时有速度感 | 位置差分 + 音高偏移（易出怪声，默认关） |
| miniaudio 后端（若选 B） | 跨平台 + 解码器 | 一个类（~200 行） |
| 音频事件轨道（配合 G11 动画事件） | 脚步/打击音与动画对齐 | 依赖 G12 的动画事件落地 |

---

## 6 · 锚点总表（自动化用）

⚠ 项目铁律：`detail=` 里**不能有空格**（脚本按 `\S*` 解析，含空格会整行失配 →
断言在脚本眼里"不存在"）。所以下面全部用 `k=v` + 逗号分隔。

| 锚点 | 何时打 | 字段 | 用途 |
|---|---|---|---|
| `AUDIO-CONFIG` | `--selftest audio-config` 解析完（一次） | `enabled= device= null= master= music= sfx= voice= mute= duck= maxvoices= retry=` | **"配了但没生效"是音频最常见的失败模式**，这行是唯一证据 |
| `AUDIO-INIT` | 设备打开/关闭（一次） | `enabled= backend= device= sr= ch= fallback=` | 证明设备真的开了、`audioDevice` 真的接上了 |
| `AUDIO-DEVFAIL` | 设备失败（一次，不重试） | `backend= reason= fallback=` | 无声卡不崩 |
| `AUDIO-DEVWARN` | 指定设备名找不到 → 回落默认 | `reason= want=` | R9 的守卫（不许静默回落） |
| `AUDIO-CLIP` | 每次成功解码 | `path= frames= sr= ch= bytes= stream=` | 解码正确性 |
| `AUDIO-CLIP-FAIL` | 解码失败 | `path= reason=` | 坏文件不崩 |
| `AUDIO-PLAY` | 每次 `playOneShot` / 组件唤醒 | `id=slot,gen= clip= bus= loop= vol= stream=` | 播放路径通了（`id` 带代际，防"上一具尸体的音效"） |
| `AUDIO-LISTENER` | 首次取到 `AudioListenerComponent` | `id= pos=` | 3D 声场真的有人听 |
| `AUDIO-DUCK` | ducking 包络跨过阈值 | `music= reason=` | 只在变化时打（别每帧打） |
| `AUDIO-STATS` | 每 60 帧 | `voices= rms= peak= cpu= fill= sr= ch= muted=` | 实时健康度 |
| `AUDIO-STREAM` | 每 60 帧 | `active= cur= loops= ring= resident= underrun= fill=` | 流式健康度（`resident` 不随文件变长是判据） |
| `AUDIO-STREAM-SEEK` | seek 成功 | `to= ring=` | 切歌/跳转 |
| `AUDIO-SUMMARY` | 运行时关闭（一次） | `backend= device= running= plays= devfail=` | 一次性总账 |
| `AUDIO-PREVIEW` | 控制台/Inspector 试听 | `clip= vol= ok=` | 试听链走通 |
| `AUDIO-PREVIEW`（Content） | 双击试听四态 | `path= state=playing\|stopped\|failed\|unsupported` | §3.11 的判据：不许静默无反应 |
| `AUDIO-PREVIEW-ROW` | 试听中那一行的矩形 | `=(x0,y0)-(x1,y1)` | "角标真的画在那一行上" |
| `AUDIO-CMD` | 控制台命令执行 | `preview clip= / bus=unknown name=` | 命令认没认出来 |
| `AUDIO-TOTAL` | 自检结束 | `pass=N fail=0` | 回归读它 |
| `EDITOR-AUDIO` | 编辑器音频初始化 | `enabled= backend= device= sr= ch=` | 编辑器侧独立开关 |
| `INS-AUDIO-HEADER` / `INS-AUDIO-PREVIEW` | 每帧（`MYVK_LOG_RECTS=1`） | `=(x0,y0)-(x1,y1)` | "界面点得到" |

⚠ `AUDIO-STATS` 里的 `cpu` 用"混一个 block 的耗时 / block 时长"，**不是**全局 CPU 占用
（后者在 Windows 上要 `GetProcessTimes`，噪声大、不稳定）。


---

## 7 · 风险与坑（预判）

| # | 风险 | 现象 | 处置 |
|---|---|---|---|
| **R1** | miniaudio 取不到（本机实测两个代理端口都不通） | G14-0 卡死 | 选 B（零依赖）；或手工拷 `miniaudio.h` 并校验版本号 |
| **R2** | 开发机可能**没有声卡**（虚拟机/远程桌面） | `IAudioClient::Initialize` 失败 | 一律降级到 `Silent`，自检走 `--audio-null`；`AUDIO-DEVFAIL` 只打一次 |
| **R3** | WASAPI shared mode 的格式**不是**你想要的 | 设 44.1 kHz 失败 / 声音变调 | 永远用 `GetMixFormat`，重采样交给 `Mixer` |
| **R4** | 音频回调里读文件 / 加锁 / 打日志 | 低负载正常，掉帧时卡顿爆音 | §3.5 三条铁律写进 `Mixer.h` 头注释；代码评审逐条对 |
| **R5** | 槽位复用导致 ABA | "停"停到了新的一声 / 旧声杀不掉 | `(slot, generation)`；断言里专门测"抢最旧后 old gen 命令被丢弃" |
| **R6** | 跨 maxDist 的阶跃 | 走近/走远时"啪"一声 | §3.7 的 10% 收尾斜坡 + "相邻采样差 < 0.02"的断言 |
| **R7** | 线性 pan 的中间塌陷 | 正前方的声音听起来"远" | 等功率 pan + 断言 |
| **R8** | 音量直改产生拉链噪声 | 拖动滑杆时有"沙沙" | 50 ms 平滑（§G14-5） |
| **R9** | `audioDevice` 接了但匹配不上时静默回落 | 用户以为设置生效了 | 必须打 `fallback=1`（这条是 §1.1 的直接教训） |
| **R10** | `engine/ecs/**` 是**横切**文件（`run_regression.py:335`） | 加组件那一步会让**全量回归**被点名 | 按纪律：**只有用户明确下令才跑全量**；日常只跑 `run_regression.py audio` |
| **R11** | 只加组件不写序列化 | Play 里音频组件凭空消失 | G14-7 的验收第 4 条专门测它 |
| **R12** | 每实体一个常驻 voice | 200 实体 → 200 路混音，CPU 爆 | §3.10 的池 + `playOnAwake` 申请/释放 |
| **R13** | 流式在主线程卡顿时断音 | 加载大模型时音乐"跳"一下 | 3 s 环形缓冲 + 主线程每帧预读 |
| **R14** | 无声卡机器上 GUI 用例永远 FAIL | 回归在别人机器上红 | B 组**可跳过但必须留痕**（`SKIP` 写进输出，不能静默） |
| **R15** | 把 `AUDIO-STATS` 每帧打 | 日志被淹没、断言 `tail -1` 读到中间态 | 每 60 帧；`AUDIO-DUCK` 只在变化时 |
| **R16** | 停止/切歌时的爆音 | 每次切换"哒" | 所有"停/抢/切"都走 5~10 ms 淡出 |

---

## 8 · 回归与测试策略

### 8.1 六组用例（`tools/verify_audio.py`）

立项时写的是四组；落地时按"缺一组就有洞"拆成了六组（B 与 F 是后加的：
配置解析的失败模式和脚本注入链各占一面，混在 A 里就测不细）。

| 组 | 内容 | 需要窗口 | 需要声卡 | 实测时长 |
|---|---|---|---|---|
| **A** | `Game.exe --selftest audio`：解码 / 混音 / 总线 / ducking / 3D / 流式 / 确定性（**118 条断言**）+ 10 分钟长文件的流式↔全解码对照 | ❌ | ❌ | ~3 s |
| **B** | `--selftest audio-config`：env > `game.json` > 默认，含越界夹取与 `_NULL` 隐含语义 | ❌ | ❌ | ~2 s |
| **C** | 设备后端四态：null / wasapi / 假名（`fallback=1` + `AUDIO-DEVWARN`）/ `__none__`（`DEVFAIL` 且不回落） | 部分（40 帧） | C2/C3 需要 | ~6 s |
| **D** | 场景往返：`audio` 11 字段逐字段相等（含 float32）+ `audioListener` + 老场景对照 | ❌ | ❌ | ~2 s |
| **E** | 编辑器：`INS-AUDIO-HEADER` / `INS-AUDIO-PREVIEW` 矩形 / `audio.preview` 试听链 / **Play 副本被消费** | ✅ | ❌ | ~18 s |
| **F** | 脚本内置 `playSound` / `setBusVolume` / `stopAllSounds`（含拼错总线名必须留痕） | ❌ | ❌ | ~3 s |

整轮 **33 秒**（含 3 次编辑器启动）。

四条纪律：

1. ⚠ **GUI 用例同时只能有一个**（E 组三次启动串行；不能与其它 GUI 用例并行）；
2. ⚠ **结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取最后一条）——
   项目里已经踩过五次；
3. ⚠ 无声卡时 C2/C3 **SKIP 必须写在输出里**（`SKIP  C2  (no-sound-card)`），
   不许静默当成绿；
4. ⚠⚠ **两个 exe 的帧控方式不同**：`Game.exe` 认 argv `--frames N`，
   `Editor.exe` 只认环境变量 `MYVK_FRAMES`（`editor/main.cpp` 里
   `(void)argc; (void)argv;` 把 argv 整个丢弃）。给编辑器传 `--frames`
   **不会有任何提示**，它会一直渲染到超时 —— 而且因为 Python 输出重定向时
   是块缓冲，外面连一行日志都看不到。见 §13.1 第 1 条。


### 8.2 确定性判据（本路线图最重要的一条）

`Mixer::renderOffline()` 用固定参数连续渲染 200 个 block（512 帧），两次结果
**`memcmp` 必须为 0**。这条能一次性抓住：

- 用了全局 `rand()`（pitch 抖动、抢最旧的随机）→ 两次不同；
- 用了 `float` 累加顺序不稳定 / 多线程写同一缓冲 → 两次不同；
- 用了系统时间做包络 → 两次不同。

⚠ 所以：**随机数必须每个 voice 自带确定性 PRNG**（`xorshift32`，种子 = voice id），
**绝对不能用 `rand()` / `std::random_device`**。

### 8.3 `TRIGGERS` 登记 + 横切警告

落地后的实际登记（`run_regression.py`，11 条）：

```python
"verify_audio": ["engine/audio/**",              # 整块新子系统（不在 GLOBAL_GLOBS）
                 "runtime/RuntimeConfig.*",      # audio 块 / MYVK_RUNTIME_AUDIO_* 解析
                 "runtime/RuntimeApp.*",         # syncAudio / 暂停时 SFX 静音
                 "runtime/main.cpp",             # --selftest audio / audio-config 注册
                 "engine/scene/SceneSerializer.*",  # audio / audioListener 块读写
                 "engine/script/ScriptEngine.*",    # playSound / stopAllSounds / setBusVolume
                 "editor/InspectorPanel.*",      # Tool 页签的 Audio Source 段
                 "editor/EditorApp.*",           # 编辑器音频门面 + audio.* 控制台命令
                 "editor/EditorContext.*",       # requestAudioPreview / takeAudioPreview
                 "editor/ContentBrowser.*",      # 双击试听四态
                 "assets/game.json"],            # B2 组读它的 music=0.70（出厂配置）
```

⚠ 立项时写的 `runtime/ui/AudioPreview.*` **没有落地**（试听链最后是
`EditorContext` 请求位 → `EditorApp::syncEditorAudio` → `AudioSystem::playOneShot`），
所以那条登记换成了真实存在的三个文件。

⚠ **`engine/ecs/**` 在 `GLOBAL_GLOBS` 里**（`run_regression.py:335`）
→ **不要**把 `Components.h` 写进 `TRIGGERS`（写了会在 `--narrow` 下假跳过）。
加 `AudioSourceComponent` 那一步是**横切改动**，按项目纪律：
**只有用户明确下令才跑全量回归**。

⚠ `assets/audio/**` 不在快照里（`WATCH_ROOTS` 没有它）→ 不登记，也不该登记
（登记了永远不触发 = 假映射）→ 改音频素材时**手动** `run_regression.py audio`。

⚠ 这条 fixture 当场抓到了一处**真实遗漏**：`editor/InspectorPanel.cpp` 的
fixture `want` 集合里没有 `verify_audio`，音频段加进去后它立刻 FAIL。
这正是"改表 + 同步改 fixture 是原子操作"的意义。


### 8.4 否定测试清单

立项时列了 8 条"应该变红"。G14-9 实际跑了前 3 条（最核心的三条判据），
**每一条都真的改坏 → 重建 → 看到红 → 还原 → 再看到绿**，实测如下：

| 故意改坏 | 实测结果 | 结论 |
|---|---|---|
| 衰减公式去掉 `fadeOut`（`return inv`） | `spatial_no_step_at_maxdist g=2.000e-02`（正常 `3.998e-06`）/ `spatial_fade_engaged_after_90pct ratio=1.0000`（正常 `0.5000`）/ `spatial_sweep_no_jump maxjump=0.020001` → **fail=3** | ✅ 三条一起抓住。⚠ 但 `spatial_sweep_no_jump` 的阈值是 0.02、实测 0.020001，**余量只有 1e-6** —— 靠它单独守不住，真正决断的是前两条 |
| pan 改成线性（`L=0.5(1-x)`、`R=0.5(1+x)`） | `pan_center_is_half_power l=0.5 r=0.5` / `pan_center_power_unity pow=0.500000` / `pan_constant_power_sweep worst=5.000e-01` → **fail=3**；而 `pan_left_full l=1 r=0`、`pan_right_full l=0 r=1` **仍然 PASS** | ✅ 正好印证了立项时的判断：端点断言对线性 pan **无效**，只有"正前方等功率"那条能抓。**这条断言不能删** |
| 24-bit 按大端拼（等价于"3 字节解错"） | `wav_24_vs_16_rms rel=2.2531`（正常 `0.0000`）→ **fail=1**，且 `wav_24_frames_match frames=44100` **仍绿** | ✅ 定位精准：帧数对、波形错 —— 只有 RMS 对照能抓 |

剩下 5 条（总线增益写成 `max` / 抢最旧改成拒绝新声 / 流式改成 `InMemory` /
序列化删字段 / 删 `fallback` 日志）留待后续改动时顺手做 —— 它们对应的断言
都已经在 A/C/D 组里跑着，不是"没有守卫"，只是这一轮没逐条手动改坏一次。

⚠ 否定测试做完**必须把改动还原并重跑一次看到绿**（本次已确认
`AUDIO-TOTAL pass=118 fail=0`，且全仓 `grep NEGTEST` 为空）。


---

## 9 · 与其它路线图的接口

| 上游 | 接口 | 说明 |
|---|---|---|
| **G11/G12 动画** | 动画事件 → `AudioSystem::playOneShot` | 脚步声与"命中判定窗口"对齐（`ship_roadmap` 的 G12 已点名） |
| **G15 游戏 UI** | 按钮音 / `U8` 的"可选按钮音效" | `ui_hud_roadmap:824` 已预留一行 |
| **G20 运行时设置** | `AudioSettings`（master/music/sfx/voice/mute） | 设置面板读写它；**持久化归 G20**，G14 只提供内存态 + `game.json` 只读默认 |
| **G26 ~ G28 脚本** | `playSound` / `setBusVolume` | 本路线图先给三个内置函数，组件级访问留给 G26 |
| **G29 打包** | 流式音频**不打进 pak**（保持文件级随机读） | `ship_roadmap` 的 G29 已写明 |
| **G32 性能 HUD** | `audio voices` / `audio ms` | `AUDIO-STATS` 的 `cpu=` 就是它的数据源 |
| **物理 / 输入 / UI** | **单向**：音频只读世界 | §3.6 的硬约束 |

⚠ 反向依赖禁令：`engine/audio/**` **不许** include `runtime/` 或 `editor/`。
脚本侧的音频能力通过 `ScriptHost` 的 `std::function` 注入（与 `setHealth` 同款）。

---

## 10 · 验收总表（一句话场景）

| # | 场景 | 判据 |
|---|---|---|
| 1 | 解码一个 1 s 44.1k 立体声 wav | `frames=44100 sr=44100 ch=2`，首周期逐点误差 < 1e-4 |
| 2 | 不开设备渲染 10 s 440 Hz | 音量与 RMS 成线性；两次渲染 `memcmp` 相同 |
| 3 | 把声源从 `minDist` 移到 `2×maxDist` | 增益严格递减，`maxDist` 处 = 0，无阶跃 |
| 4 | 声源在左耳侧 | `L/R > 1.5`；正前方 `|L-R| < 5%` |
| 5 | `SFX` 音量拖到 0 | 音效消失、音乐不变（两条都测） |
| 6 | 触发 voice 对话 | 音乐 100 ms 内降 ≥ 6 dB，500 ms 内恢复 ±0.5 dB |
| 7 | 并发 65 个音效 | 不爆音（相邻样本差 < 0.5），最旧被替换 |
| 8 | 播 10 分钟音乐 | 常驻增长 < 2 MB，主线程卡 1 s 不断音 → **实测流式常驻 4 096 B（恒定）vs 全解码 211 680 000 B** |
| 9 | 场景里挂音源 → 存 → 读 | 逐字段相等；Play 副本仍在 |
| 10 | 编辑器双击 wav | 听见声音 + `AUDIO-PREVIEW state=playing` |
| 11 | 设备名填假名 | `AUDIO-DEVWARN` + 回落默认设备 + `fallback=1`（⚠ 不是 `DEVFAIL`：假名是"可以救"的，`DEVFAIL` 只给 `__none__` 这种"明确不要设备"或真失败） |
| 12 | 无声卡机器启动游戏 | 不崩、一条 WARN、其余系统逐位不变 |
| 13 | 关掉音频跑 `verify_hud` / `verify_player` | 结果与今天**逐位相同** |

---

## 11 · 规模与执行顺序

| 任务 | 规模 | 依赖 | 一句话 |
|---|---|---|---|
| G14-0 | S | — | 选型 + 目录 + CMake 接线 |
| G14-1 | M | G14-0 | 能读 wav（+ 素材生成器） |
| G14-2 | **L** | G14-1 | 能离线混出波形（**地基**） |
| G14-3 | **L** | G14-2 | 能真的出声 |
| G14-4 | M | G14-2 | 能听出方位 |
| G14-5 | M | G14-3 | 能调音量 / ducking |
| G14-6 | M | G14-2 | 能流式放歌 |
| G14-7 | M | G14-4 | 能在编辑器里挂 |
| G14-8 | M | G14-7 | 能试听 / 脚本发声 |
| G14-9 | S | 全部 | 回归收口 |

**推荐顺序**：`0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9`。
⚠ G14-3 与 G14-6 之间没有强依赖，但在 4 之后做设备相关的事更省事（能听到效果）。
⚠ **G14-2 之前不要碰编辑器**（没有可听的东西，UI 只能"点一下没反应"）。

**里程碑判定信号**

| 里程碑 | 判定 |
|---|---|
| M0（0~2） | `--selftest audio` 在**无声卡机器**上全绿，且波形 `memcmp` 可复现 |
| M1（3~4） | 真实设备上"从左边走到右边，声音跟着走" |
| M2（5~6） | 音量滑杆有效；一首 4 分钟的歌只占 1 MB |
| M3（7~9） | 编辑器里双击 wav 能听、挂到实体上 Play 有声、存盘再开还在 |

---

## 12 · 一页速查

```
核心：设备层可替换 / 混音自己写 / 音频只读世界 / 自动化默认关
公式：gain = minDist/(minDist+rolloff*(d-minDist)) × fadeOut(d)   （fadeOut 最后 10% 到 0）
       L = cos((x+1)π/4)   R = sin((x+1)π/4)                      （等功率，别用线性）
线程：回调里不分配、不加锁、不打日志、不碰 World；主↔音 只走 SPSC 定长 POD
句柄：voice = (slot, generation)；所有停止/抢断都 5~10 ms 淡出
时基：来自设备 block，不是 dt / timeScale；暂停 ≠ 静音
资产：AudioClip 不进 AssetManager；>2 s 走 Streaming（3 s 环形缓冲 + 主线程预读）
序列化：加了组件不写 JSON → Play 副本里凭空消失（Play 靠 sceneToJson→sceneFromJson）
默认：产品开 / 回归注入 MYVK_RUNTIME_AUDIO=0 → 既有基线逐位不变
锚点：AUDIO-CONFIG / AUDIO-INIT / AUDIO-CLIP / AUDIO-PLAY / AUDIO-DEVFAIL / AUDIO-DEVWARN
      / AUDIO-DUCK / AUDIO-STATS / AUDIO-STREAM / AUDIO-SUMMARY / AUDIO-PREVIEW
      detail 里不能有空格；AUDIO-DUCK 只在变化时打；AUDIO-STATS/STREAM 每 60 帧
坑：24-bit 是 3 字节打包 / shared mode 用 GetMixFormat / data 长度可能是 0xFFFFFFFF
    maxDist 硬截断会"啪" / 线性 pan 中间塌陷 / 槽位复用要代际
    "Volume"/"Pitch" 已被粒子/相机占用 → 改叫 "Clip Volume"/"Clip Pitch"
    kZh 长条目必须写一行（解析器不处理 "a" "b" 相邻拼接）
回归：engine/ecs/** 是横切（加组件 = 全量被点名，需明确指令）
      assets/audio/** 不在快照 → 改素材手动跑 audio 用例
      ⚠⚠ Editor.exe **只认 MYVK_FRAMES**（argv 被丢弃），传 --frames 会静默挂死
      ⚠ 脚本输出重定向时是块缓冲 → 卡住时一行都看不到，要 -u 或强制行缓冲
```

---

## 13 · 执行进度

| 日期 | 任务 | 结果 |
|---|---|---|
| 2026-10-18 | **立项分析**（本文档） | 三个硬事实均已实测：`audioDevice` 零消费者（全仓 2 处读写）/ `stb_vorbis.c` 未进编译（`stb` 是 INTERFACE）/ 外网不通（两个代理端口都连不上）。`ship_roadmap` 的 G14 已加指针 + 四条勘误。**尚未写任何音频代码。** |
| 2026-10-18 | **G14-0 骨架** | `engine/audio/` 八个文件（`AudioFormat.h` / `AudioBackend.h` / `NullBackend` / `AudioClip` / `AudioClipCache` / `Mixer` / `AudioSystem` / `AudioSelfTest`）+ CMake（`stb_vorbis` 静态库、`ole32 avrt ksuser winmm`、`assets/audio` 进"每次都拷"清单）。`Game.exe --selftest audio` 跑通，**不初始化 Vulkan、不开窗**。✅ |
| 2026-10-18 | **G14-1 解码** | `tools/gen_audio_assets.py` 生成 7 个夹具（合计 **1 080 128 B**）；`--long` 另生成 10 分钟对照（**105 840 044 B**，不入库）。WAV 8/16/24/32/float32/EXTENSIBLE + OGG 全部解出。`wav_24_vs_16_rms rel=0.0000` 守住"24-bit 是 3 字节打包"（按 4 字节读会差 >50%）。✅ |
| 2026-10-18 | **G14-2 Mixer** | 64 声部池 / 四总线 / 线性重采样 / 软削波（`[-0.8,0.8]` 严格线性）/ SPSC 定长 POD 命令队列 / `(slot, generation)` 句柄。`mixer_deterministic_memcmp` 两次离线渲染**逐位相同** —— 时基确实来自 block 帧数而不是 `dt`。`pool_steal_no_click` 证明抢断先淡出。✅ |
| 2026-10-18 | **G14-3 设备后端** | `WasapiBackend`（`GetMixFormat` → 再 `Mixer::init`，open 与 start 分开）+ `AudioSystem` 状态机 + `__none__` 哨兵。实测真机：`AUDIO-INIT enabled=1 backend=wasapi device=扬声器_(适用于_I2S_音频的英特尔®_智音技术) sr=48000 ch=2 fallback=0`。✅ |
| 2026-10-18 | **G14-4 3D 空间化** | 23 条断言全绿。`spatial_no_step_at_maxdist g=3.998e-06`（收尾斜坡生效，硬截断会"啪"）/ `pan_constant_power_sweep worst=1.192e-07`（等功率）/ `spatial_sweep_no_jump maxjump=0.0099`。⚠ 3D 断言**不能只测纯函数**：新增 `renderOneSpatial()` 真的渲染 0.2 s 再判。✅ |
| 2026-10-18 | **G14-5 总线/ducking/配置** | `game.json` 加 `audio` 块（10 个字段）+ 全套 `MYVK_RUNTIME_AUDIO_*` + `AUDIO-CONFIG` 一行锚点。ducking 实测 **-12.041 dB**（`duck_active_at_least_6db`）；50 ms 音量平滑 `vol_smoothing_no_step maxadj=0.000208`（硬跳是 0.5，差 2400 倍）。假设备名 → `fallback=1` + `AUDIO-DEVWARN` 留痕。✅ |
| 2026-10-18 | **G14-6 流式** | 3.000 s 环形缓冲（`stream_ring_is_three_seconds`），`absWritten` 单调递增 → 第 a 帧 = `a % totalFrames`（音频线程**不减** `pos`）。回绕点 `stream_wrap_no_jump maxjump=0.010475`；欠载 `stream_underrun_silent_not_wrapped leak=0.0`（**行为级**守卫：欠载只能补静音，不能把旧数据当新数据）。对照实测：流式常驻 **4 096 B** vs 全解码 **211 680 000 B**（`ratio=51680`），且 `stream_resident_constant` 证明不随文件变长。✅ |
| 2026-10-18 | **G14-7 组件/序列化/Inspector** | `AudioSourceComponent`（11 字段）+ `AudioListenerComponent`；`saveScene`/`loadScene` 双端都写。往返实测逐字段相等（11/11，含 float32：`volume=0.3499999940395355`）+ `audioListener: true`。Inspector Tool 页签新增 Audio Source 段：`INS-AUDIO-HEADER=(1337,182)-(1590,210)`、`INS-AUDIO-PREVIEW=(1503,217)-(1576,245)`；Add Component 加两项。✅ |
| 2026-10-18 | **G14-8 试听/脚本** | Content 双击试听四态（playing / stopped / failed / unsupported）+ 行角标 + `AUDIO-PREVIEW-ROW`；编辑器控制台 `audio.preview|stop|bus|stats`。脚本内置 `playSound` / `stopAllSounds` / `setBusVolume`。实测 `AUDIO-PLAY id=0,1 clip=audio/click.wav bus=3 loop=0 vol=0.35`（组件路径）与 `AUDIO-PLAY ... clip=audio/tone_1k_mono.wav bus=2`（脚本路径）并存；故意拼错的 `musik` 打 `GAME-SCRIPT setBusVolume: 未知总线 'musik'`（**不许静默忽略**）。✅ |
| 2026-10-18 | **G14-9 收口** | `tools/verify_audio.py`（A~F 六组、**33 条 check**）末行 `结果: PASS`，整轮 **33 秒**；`--selftest audio` 累计 **`AUDIO-TOTAL pass=118 fail=0`**。`run_regression.py` 的 `DEFAULT` 加入本用例，`TRIGGERS` 登记 11 条（`engine/audio/**` + 各层接线点）；`check_regression_map.py` 的 fixture 同步改（`InspectorPanel.cpp` 的 want 集合 + `assets/game.json` 改成双 owner）→ PASS。`verify_i18n_coverage.py` PASS（词典 **1 582 条**、A1~F 八组零缺）；`check_feature_surface.py` PASS（35 个特性全有 UI 面）。✅ |

（每完成一项，在此追加"实测数字 + 踩到的坑"，与 `ui_hud_roadmap` §13/§14 同款。）

### 13.1 落地时踩到的坑（**都是"看起来对、其实在骗你"那一类**）

| # | 坑 | 现象 | 修法 |
|---|---|---|---|
| 1 | ⚠⚠ **`Editor.exe` 不认 `--frames`** | 脚本给编辑器传 `--frames 70`，它照常开窗、照常渲染、**永不退出** → 三次启动各挂到 240 s 超时。加上输出被 Python 块缓冲吞掉，**外面一个字都看不到**，只能靠猜 | `editor/main.cpp` 里 `(void)argc; (void)argv;` —— argv **整个丢弃**，编辑器只认环境变量 `MYVK_FRAMES`。`verify_audio.py::run()` 现在按 exe 名分派（editor → env，game → argv），并强制行缓冲、超时保留部分输出、E 组补 `rc == 0` |
| 2 | **3D 方向断言的符号约定写反了** | `spatial_move_flips_balance` / `spatial_stale_update_dropped` 双红 | listener 在原点、`forward=-Z`、`up=+Y` → 右向量 = `forward × up` = **+X**；声源在 -X（左）→ **L 更响**（`rmsL-rmsR > 0`）。结论直接写进注释，别再靠脑补 |
| 3 | **拿错了对照物** | `stream_cheaper_than_inmem` 红：stream=1 058 400 vs inmem=352 800 | 3 s 环形缓冲比 1 s 音效大是**正确行为**。有意义的判据是 `stream_resident_constant`（跑 40 帧字节数一个不涨）而不是"谁更小" |
| 4 | **fade 区里用绝对阈值** | `spatial_fade_engaged_after_90pct` 红（g=0.0105 期望 < 0.01） | fade 区里 `1/(...)` 本身已经很小，绝对阈值估不准 → 改成**跟纯倒数比比值**（d=47.5 落在 [45,50] 正中 → 0.5000） |
| 5 | **`_NULL=1` 单独用静默无效** | 只设 `MYVK_RUNTIME_AUDIO_NULL=1` 时音频根本没起来 | `enabled` 归 `MYVK_RUNTIME_AUDIO` 管、`_NULL` 只管后端；自动化默认关又把它关掉 → `_NULL=1` 且未显式给 `MYVK_RUNTIME_AUDIO` 时**隐含** `enabled=1`，且默认关的判断要排除 `_NULL` 存在的情况 |
| 6 | **i18n 解析器不处理相邻字面量拼接** | 6 条长 tooltip 明明写进 `kZh[]`，`--dump-keys` 也一致，B2 组仍报"缺译文" | 解析器用 `finditer(r'"..."\s*,\s*"')`，遇到 `"a" "b"` 只取第一段 → **长条目必须写成一行** |
| 7 | **`--dump-scene` 不走 `resolveAssetPath`** | 相对路径写不进去：`cannot open for write` | dump 路径按**当前工作目录**（`build/bin`）解析 → 脚本一律传绝对路径 |
| 8 | **`game.json` 去掉顶层 `audioDevice` 会静默毁掉旧配置** | 老用户的设备选择突然失效 | 读端保留兼容别名 + `GAME-CONFIG-DEPRECATED` 警告，本次仍按旧字段生效 |
| 9 | **i18n 一词两义** | `"Volume"` 已被粒子体积占用、`"Pitch"` 已被相机俯仰占用 | UI 原文改成更具体的 `"Clip Volume"` / `"Clip Pitch"`（路线图 §3.12 早就点了这条，落地时确实撞上） |

