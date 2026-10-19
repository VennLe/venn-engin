# 引擎启动器路线图 —— 从「双击就进编辑器」到「先选工程再进编辑器」

> **一句话**：现在 `Editor.exe` 只有一个隐含的"工程"——`build/bin/assets`。
> 这份文档要把它变成**启动器 + 工程目录**的形态：启动器里新建/打开游戏工程，
> 选类型（第一/第三人称）、分辨率、画质、工程路径、代码路径，**创建成功后**
> 才拉起编辑器，并让编辑器按工程类型初始化场景配置。
>
> 定稿 2026-10-19。**执行序以 §7 为准。**

---

## 0 · 这条路线图在解决什么

用户原话（三段，按提出顺序）：

> 做一个引擎启动器窗口，在启动器创建游戏成功后，才打开游戏引擎编辑器窗口，
> 并根据游戏类型等，初始化游戏的场景配置，比如第一人称游戏和第三人称游戏时的
> 游戏初始配置是不太一样的。

> 1. 启动器中可以创建新游戏，打开最近游戏，当选择创建游戏时，则会让选择游戏类型。
> 2. 启动器中可以设置游戏的类型，第一还是第三人称游戏。
> 3. 可以设置当前游戏工程项目的存储位置，也可以设置游戏工程项目中关于游戏代码
>    工程项目的存储位置。这个游戏代码的方面请多参照之前游戏中的关于代码方面的处理方案。

拆成**四件事**，但它们的依赖**不是并列的**：

```
   ① 工程数据模型（目录 + venn.project.json）
        │
        ├──► ② 模板包（第一/第三人称各自的初始场景）
        │        │
        │        └──► ③ 启动器 UI（新建向导 / 最近列表）
        │                 │
        │                 └──► ④ 拉起编辑器（env + CWD + 生命周期）
        │
        └──► ⑤ 代码解决方案（.vks 代码根，可选外置 / C++ 模块）
```

**① 是地基，不是第四个功能。** 没有工程数据模型，"最近游戏"存什么、
"工程路径"写到哪、"代码路径"怎么被编辑器看见，全部悬空。
②③④ 都只是①的消费者。

**为什么顺序不能反过来**：先做启动器 UI 会立刻卡在"创建出来的东西编辑器打不开"
——那时你会以为是 UI 的问题，其实是资产根/CWD 的契约没定。所以 §7 的 M0
是"用命令行 + 手写模板，不写一行 UI，先证明工程能被打开"。

---

## 1 · 现状盘点（实测，2026-10-19）

### 1.1 已经有的（不要重做）

| 能力 | 位置 | 状态 |
|---|---|---|
| 两个可执行 | `CMakeLists.txt:200/230` → `Editor.exe` / `Game.exe` | ✅ 都继承 `core::Application` |
| 资产根候选链 | `engine/assets/AssetPath.cpp:22` | ✅ `MYVK_ASSET_DIR` → exe/assets → cwd/assets → cwd，**第一个存在的赢** |
| 场景存读 | `SceneSerializer.cpp` | ✅ 含 `player.mode`、`playerLimb`、`physics` 块 |
| 玩家两种模式 | `ecs::PlayerComponent`（`Components.h:449`） | ✅ `mode` / `eyeHeight` / `lookSpeed` / `camDistance` / `turnSpeed` |
| 玩家的创建 | `editor/EditorScene.cpp:196 createPlayer()` | ✅ **已经按 FP/TP 分别设初值** |
| 画质预设 | `editor::applyQualityPreset()`（`FeatureUIGen.cpp:300`） | ✅ `Low…Cinematic` 几档，翻特性开关 |
| 设置覆盖链 | `render/RenderSettingsIO.h` | ✅ `EditorPrefs < Project < Scene < Object < Camera` |
| 运行时配置 | `runtime/RuntimeConfig` | ✅ CLI > `MYVK_RUNTIME_*` > `assets/game.json` > 默认 |
| 游戏脚本 | `engine/script/`（S0~S6 刚收口） | ✅ 热重载、不改 C++、不重编译 |
| 每工程布局隔离 | `EditorLayoutIO` / `imgui.ini` / `editor_window.ini` | ✅ **本来就相对 CWD**（只是没人换过 CWD） |

### 1.2 缺的（逐条对应需求）

| # | 缺什么 | 具体表现 |
|---|---|---|
| 1 | **工程实体** | 全仓没有 `project` 概念；`assets/` 就是唯一的工程，路径写死在 exe 旁边 |
| 2 | **多工程隔离** | 换工程 = 换 CWD，但没人做；一旦两个工程共用 CWD，布局/预置互相覆盖 |
| 3 | **启动器** | 不存在；只有 `Editor.exe` / `Game.exe` |
| 4 | **模板** | 没有 `templates/` 目录；新场景只能靠编辑器里手搭 |
| 5 | **最近列表** | 不存在；`%LOCALAPPDATA%/Venn/` 下只有运行时日志 |
| 6 | **画质接线** | `RuntimeConfig::quality` 是**空挂字段**（见 P9） |

### 1.3 ⚠ 六个「隐形问题」—— 不写启动器也已经在限制你

**P1 · `Editor.exe` 丢弃 argv。** `editor/main.cpp:54` 明写 `(void)argc; (void)argv;`。
所以 `Editor.exe --project xxx` **不会有任何提示地什么都不做**。启动器只能走
"环境变量 + 工作目录"。已实测踩过同类：给 `Editor.exe` 传 `--frames`，它会照常
渲染到超时（`Game.exe` 认 argv，`Editor.exe` 不认）。

**P2 · 工作目录是隐式契约，但没人声明。** `config/editor_layout.json`、
`config/editor_prefs.json`、`imgui.ini`、`editor_window.ini` **全部相对 CWD**
（仓库根目录里就躺着一个 `imgui.ini` 和一个 `editor_window.ini`，是证据）。
这意味着"工程"其实一直存在，只是它叫"当前工作目录"。

**P3 · 资产根的候选链是"第一个存在的赢"，不是"合并"。**
`MYVK_ASSET_DIR` 只是插到最前面；工程里没有的文件会自动回退到 `exe/assets`。
这**既是好事**（工程不用拷贝 shader/图标/默认材质），**也是坑**（你以为工程是
自包含的，其实它偷偷依赖 `build/bin/assets`）。

**P4 · `RuntimeConfig::quality` 是空挂字段。** `grep quality` 的结果是：
读进来（`RuntimeConfig.cpp:101`）→ 环境变量覆盖（`:152`）→ **打印到日志**
（`:353`）。**没有任何消费者**。启动器如果老老实实写这个字段，用户会得到
"选了 Cinematic，画面一点没变，也没有任何提示"。

**P5 · 分辨率是唯一真正生效的画面字段。** `cfg.width/height/fullscreen` 直接进
`core::Application(title, w, h)`（`RuntimeApp.cpp:25`），`run()` 里窗口就是这么建的。
所以"分辨率"能立刻交付，"画质"必须先接线（见 P4）。

**P6 · 第一/第三人称的差异**不是**一个 `mode` 字段**。`mode` 只决定相机怎么摆；
真正能"看出来"的差异还得有可见身体（`PlayerLimbComponent` 六个部件才能摆腿），
而程序化小人已在 2026-10-11 删掉、模型要走角色详情窗口绑定。
**启动器不能承诺"创建完就能看到一个第三人称角色"。**

---

## 2 · 核心设计决策（D1~D8）

### D1 · 启动器是独立进程 `Launcher.exe`，不是编辑器的一个模式

- 新建 CMake target `VennLauncher` → `Launcher.exe`。
- **理由**：① 启动器必须在"工程还不存在"时就能跑；② 它 `CreateProcess` 拉起
  编辑器后**自己退出**，两个进程生命周期互不纠缠；③ 编辑器已经是 1600×900
  满屏面板，塞一个全屏向导进去是另一种产品。
- **被否**：编辑器内模态向导 —— 第一次启动时编辑器自己还没工程可开，逻辑倒挂。

### D2 · 启动器复用 `core::Application`（ImGui + Vulkan），不另起炉灶

- 代价：一次 Vulkan 初始化 + 空场景跑一遍 pass 链。
- 收益：主题、字体（含中文图集）、i18n、图标、窗口几何、DPI **一次拿全**。
  从零写一套 ImGui 窗口是几天的活，而且会和编辑器两套主题漂移。
- **必须打的两个补丁**
  1. `Application` 加 `virtual double startupShowDelay() const { return 2.0; }`
     —— 现在 `run()` 里写死"等 2 秒再显示窗口"`kStartupShowDelay`
     （`Application.cpp:78`）。启动器**必须**返回 `0`，否则"点了图标两秒后才出窗"。
  2. 启动器**不建场景**（空的 `m_scene` 就够），所以 `onInit` 里什么都不做。
- **已知遗留**（不阻塞，M6 之后可选）：`Renderer` 加 UI-only 模式跳过 3D pass，
  目标"启动到可交互 < 1.0 s"。先用日志里的真实数字定基线，不猜。

### D3 · "工程" = 一个目录 + `venn.project.json`，**配置单源**

- 目录布局见 §3。
- ⚠⚠ **启动器不存第二份配置**。分辨率/画质读写的就是 `assets/game.json` 与
  `config/render.json`（它们是既有权威）；`venn.project.json` 只存**身份与元数据**
  （名字、类型、起始场景、代码根、创建时间、格式版本）。
- **理由**：这个项目里最贵的 bug 全是"两份数据漂移"——材质同名静默合并、
  `kPrefsVersion` 升版后 fixture 还写旧值、`SceneSerializer` 的 Model 分支只写
  两个键。启动器一旦成为"第二个存储"，立刻多出一整类
  "启动器显示 1080p、游戏跑 720p"的 bug，而且**两边都不报错**。
- **被否**：把分辨率也写进 `venn.project.json`（"启动器自己的配置"）—— 上面那条。

### D4 · 工程资产与引擎资产分层，靠既有候选链

- 启动器给子进程设 `MYVK_ASSET_DIR=<工程>/assets`。
- 工程里没有的 shader / 图标 / 引擎默认材质**自动回退**到 `exe/assets`（P3）。
- **纪律写进文档**：*工程只放自己的资产；shader 不进工程*。
  哪天要"每工程改 shader"，那是**另一个功能**（`MYVK_SHADER_DIR`），别顺手做。

### D5 · **CWD = 工程根**（最关键的一条）

- 启动器用 `CreateProcessW` 的 `lpCurrentDirectory` 指定，**不用
  `SetCurrentDirectory`**（污染父进程、并发不安全、失败后难复位）。
- 由此**白拿**：每工程一份 `editor_layout.json` / `editor_prefs.json` /
  `imgui.ini` / `editor_window.ini`（P2 那个"隐形工程"现在变成真的）。
- ⚠ 副作用要接受：工程根目录会多出 `imgui.ini`、`editor_window.ini` 两个文件。
  M2 里可以把 `io.IniFilename` 指到 `config/imgui.ini`（一行），**但别在 M0 做**
  —— 那会动到编辑器的既有落盘路径，得连带改回归断言。

### D6 · 游戏类型 = **模板包**，不是代码里的 if 分支

```
templates/
  first_person/
    template.json          ← 声明：id / 显示名 / gameType / 起始场景 / 占位符白名单
    assets/scenes/main.json
    assets/game.json
    assets/input_map.json
    assets/scripts/main.vks
    config/render.json
  third_person/
    （同上）
```

- 启动器创建工程 = **复制一个模板目录 + 替换白名单里的占位符**。
- 加一种游戏类型（俯视角 / 横版 / 载具）= **加一个目录**，不改一行 C++。
  与"加面板 = 表里加一行"是同一条哲学。
- ⚠ 模板**不允许**引用 `build/bin/assets` 里的模型/材质（P3 的隐式依赖）——
  自检会扫模板里所有 `"path"` 字段，出现在工程外的就 FAIL。

### D7 · 拉起编辑器只走 env + CWD；`Editor.exe` 的 argv **先不动**

- M0/M3 零改动编辑器就能跑（`MYVK_ASSET_DIR` + `MYVK_EDITOR_SCENE` + CWD 够了）。
- M1 才加一个 `MYVK_EDITOR_PROJECT`：解决"用户直接双击 `Editor.exe`"的场景
  （那时它自己 `current_path(projectRoot)` + 标题栏显示工程名）。
- **被否**：给 `Editor.exe` 加 argv 解析 —— argv 与 env 两套入口语义必然漂移，
  而且 `check_feature_surface.py` 的白名单是按 `MYVK_*` 前缀放行的。

### D8 · 代码解决方案三档，**默认 T1**，T2 有前置条件

| 档 | 内容 | 状态 |
|---|---|---|
| **T1 · `.vks` 脚本**（默认） | `<工程>/assets/scripts/*.vks`，编辑器里改、热重载 | ✅ 引擎既有方案（S0~S6） |
| **T2 · C++ 游戏模块** | 生成 `code/CMakeLists.txt` + VS2022 `.sln` | ⚠ **前置：`engine/` 必须先变成可链接的静态库** |
| **T3 · 发行包** | 打包成能发给别人的目录 | 不在本文范围（→ `ship_roadmap`） |

**为什么默认 T1**：这正是用户说的"多参照之前游戏中的关于代码方面的处理方案"
—— 这个引擎里"游戏代码"的既有方案就是 `.vks`：改脚本不重编译、热重载、
编辑器里直接看实例状态。S0~S6 刚刚全线收口，**现在就能交付**。

**为什么 T2 不能现在做**：`CMakeLists.txt:200/230` 是把 `engine/**` 的
**源码列表**分别编进两个 exe（`${EDITOR_SOURCES}` / `${RUNTIME_SOURCES}`），
仓库里**没有 `venn_engine` 这个可链接产物**。所以"生成一个 sln"现在只能生成
一个**编不过**的 sln。要做得先抽出静态库（改 `CMakeLists` = 横切 = 全量回归），
所以 T2 单列成 M5 的可选阶段，**且必须先做库化那一半并跑通全量**。

---

## 3 · 数据契约

### 3.1 目录布局

```
<ProjectRoot>/                     ← 启动器创建的就是这个目录
  venn.project.json                ← 身份与元数据（唯一新增的"格式"）
  assets/                          ← 资产根（MYVK_ASSET_DIR 指向这里）
    game.json                      ← 运行时出厂配置（分辨率/标题/起始场景/语言）
    scenes/main.json               ← 起始场景（模板产物）
    scripts/*.vks                  ← 游戏代码（T1 默认位置）
    materials/ models/ textures/ audio/ ui/   （按需）
  config/
    render.json                    ← 工程级画质（SettingScope::Project）
    editor_layout.json             ← 本工程的编辑器布局（首次打开时生成）
    editor_prefs.json              ← 本工程的编辑器偏好
```

### 3.2 `venn.project.json`

```json
{
  "format": 1,
  "name": "MyGame",
  "template": "third_person",
  "gameType": "third_person",
  "startScene": "scenes/main.json",
  "code": {
    "mode": "vks",
    "root": "scripts",
    "linked": false
  },
  "createdAt": "2026-10-19T14:00:00Z",
  "engine": { "name": "venn", "config": "Debug" }
}
```

字段规则：

| 键 | 规则 |
|---|---|
| `format` | 整数。**只增不改**；读不懂的高版本 → 拒绝打开并给一句人话 |
| `template` | 创建时用的模板 id（用于"重新应用模板"） |
| `gameType` | `first_person` / `third_person`。**展示用**，权威是场景里的 `player.mode` |
| `startScene` | 相对 `assets/` 的正斜杠路径 |
| `code.root` | 相对 `assets/` 的路径，**或绝对路径**（外置时） |
| `code.linked` | `true` = `assets/scripts` 是指向 `code.root` 的 junction |
| `engine.*` | **只记录不校验**（启动器不做引擎版本管理，§9） |

⚠ **不写**：分辨率、画质、窗口模式、语言 —— 它们在 `assets/game.json` 里（D3）。

### 3.3 最近列表（用户级，不属于任何工程）

`%LOCALAPPDATA%/Venn/launcher/recent.json`（与运行时日志同根，`RuntimeLog.cpp:25`
已有取法）：

```json
{ "format": 1, "items": [
  { "name": "MyGame", "path": "D:/Games/MyGame", "gameType": "third_person",
    "lastOpenedAt": "2026-10-19T14:02:11Z" }
] }
```

- 上限 10 条，按 `lastOpenedAt` 倒序。
- **打开失败**（目录没了 / 没有 `venn.project.json`）→ 从列表移除 + 一条提示，
  **不崩、不弹模态**（P11）。

---

## 4 · 第一人称 vs 第三人称：差异清单（不是只改一个字段）

依据：`ecs::PlayerComponent`（`Components.h:449`）、`createPlayer`（`EditorScene.cpp:196`）、
`runPlayers`（`EditorApp.cpp`）。

| 项 | 第一人称模板 | 第三人称模板 | 依据 |
|---|---|---|---|
| `player.mode` | `"first"` | `"third"` | `SceneSerializer.cpp:1933` |
| `player.pitch`（视线仰角，正=抬头） | `0.0` | `-0.35`（略低头 → 相机在角色斜上方俯视） | `createPlayer` |
| `player.camDistance` | `0.0`（不参与） | `4.5`（默认 3.5 看不到全身，模板给宽一点） | `Components.h:465` |
| `player.eyeHeight` | `1.7` | 不参与 | `Components.h:452` |
| `player.lookSpeed` | `0.0025` | `0.0025` | 同上 |
| `player.turnSpeed` | 不参与 | `12.0`（角色朝移动方向平滑转身） | `Components.h:466` |
| 可见身体 | **不建**（有头就会挡视线/穿模） | 需要模型才能"看见"，见下 ⚠ | `EditorApp.cpp:1707` |
| 摆腿动画 | 无 | 需要 6 个 `playerLimb`（Body/Head/ArmL/ArmR/LegL/LegR） | `Components.h:476` |
| 场景相机 | 眼位（运行时由 `PlayerController` 驱动） | 角色后方，`distance` 来自 `camDistance` | `SpringArm` |
| 输入映射 | 鼠标视角 + WASD | 鼠标环绕 + WASD + 转身插值 | `input_map.json` |
| 起始脚本样板 | `main.vks`：准星/交互提示占位 | `main.vks`：跟随/朝向占位 | 模板自带 |

⚠⚠ **必须对用户说清楚的一条**：**今天的第三人称模板创建出来"看不到角色"**。
程序化小人已在 2026-10-11 按用户要求删除，模型要走角色详情窗口绑定
（`editor/CharacterWindow`）。所以 TP 模板 v1 的效果是"相机在胶囊体斜后方"，
和现在编辑器里手动 `Add ▸ Player` 完全一致 —— **不是模板做坏了，是角色链还没到**。
等 `character_pipeline` 的 A1（Animator）落地，**只换模板文件**就能升级
（模板是数据，这正是 D6 的价值）。

---

## 5 · 启动器 UI

### 5.1 窗口

- 默认 1100 × 680，居中，可缩放，最小 900 × 560。
- **不显示 3D 场景**（空场景；M6 之后可选做背景）。
- 顶部：Logo + 引擎版本 + 语言切换（复用 `editor::tr` 的词条表）。
- 底部状态条：GPU 名（`render::Renderer` 现成）、CMake 配置（Debug/Release）。

### 5.2 主界面（`Idle` 态）

```
┌────────────────────────────────────────────────────────────┐
│  Venn Engine 0.9                                   [中文 ▾] │
├──────────────────────────┬─────────────────────────────────┤
│  最近工程                │   MyGame                         │
│  ▸ MyGame  第三  2 分钟前│   路径  D:/Games/MyGame           │
│  ▸ FpsDemo 第一  昨天     │   类型  第三人称                  │
│  ▸（空列表时的引导文案）  │   场景  scenes/main.json          │
│                          │   代码  scripts (vks)             │
│  [ 打开已有工程… ]        │                                  │
│                          │   [打开编辑器]  [运行游戏]        │
│                          │   [在资源管理器中显示] [移出列表] │
├──────────────────────────┴─────────────────────────────────┤
│  [ ＋ 新建游戏 ]                                            │
└────────────────────────────────────────────────────────────┘
```

- 左列 = `recent.json`；选中项决定右栏内容。
- **空列表**时右栏画一段引导（"点左下角新建游戏"），不留白板。

### 5.3 新建向导（三步，同一个窗口内换页）

**Step 1 · 类型**

两张卡片，各带一句差异说明（§4 的前三行，不是全表）：

```
┌────────────────────┐  ┌────────────────────┐
│   第一人称          │  │   第三人称          │
│   相机在眼睛位置     │  │   相机在角色斜后方   │
│   鼠标直接转视角     │  │   鼠标环绕 + 角色转身│
└────────────────────┘  └────────────────────┘
```

**Step 2 · 位置**

- 工程路径（可"浏览…"，文件夹选择器用 `IFD::SelectFolderDialog`）
- 工程名（与目录名联动：改名字 → 路径末尾跟着变，用户手改过路径后联动断开）
- 代码根：默认 `scripts`（相对 `assets/`），勾"放到工程外…"才允许绝对路径
- ⚠ 目标目录**必须不存在或为空**；非空 → 就地红字提示，**不弹模态**

**Step 3 · 画面**

- 分辨率预设：`1280×720` / `1920×1080` / `2560×1440` / `自定义`
- 窗口模式：窗口化 / 全屏
- 画质：`Low / Medium / High / Cinematic`（名字**从 `editor::qualityPresetName()`
  取**，不硬编码 —— 那边加一档，这里自动出现）
- 右下角实时显示"**将写入**"清单：
  `assets/game.json ← width/height/fullscreen`、`config/render.json ← 12 个键`

**Step 4 · 创建**

- 进度条（4 步：校验 → 复制模板 → 替换占位符 → 写 recents）
- 失败 → 停在这一页，显示**可读的原因**（目录被占用 / 磁盘满 / 模板缺文件）
- 成功 → `[打开编辑器]` / `[运行游戏]` / `[完成]`

### 5.4 状态机

```
Idle ──新建──► Wizard(1→2→3) ──确认──► Creating ──ok──► Done ──► (拉起编辑器) → 退出
  ▲                    │                    │
  └──────取消──────────┘                    └─fail─► Wizard(3) + 错误行
Idle ──选中+打开──► Spawning ──ok──► 退出（编辑器接管）
                        └─fail─► Idle + toast
```

---

## 6 · 代码解决方案（"之前游戏中的处理方案"是什么）

### 6.1 T1 · `.vks` 脚本（默认，现在就能交付）

- 位置：`<工程>/assets/scripts/*.vks`；`venn.project.json.code.root` 记录。
- 编辑器里：Content 面板双击编辑 + 保存即热重载（S0~S6 已实现）。
- **模板自带 `main.vks` 样板**：
  ```vks
  // <工程名> · 由模板 <templateId> 生成
  let t = 0
  on start {
      print("HELLO", "<工程名>")
  }
  on update {
      t = t + 1
  }
  ```
  （`let` 是幂等声明；`on start` 不重跑 —— 见 `script_roadmap.md` 的 S1/S4。）
- **代码根外置**：勾"放到工程外"→ `code.root` 存绝对路径，并创建
  **目录联接** `assets/scripts → <外部目录>`（Windows：`mklink /J`，**不需要管理员**）。
  - 为什么用 junction 而不是改引擎：脚本路径是**资产相对路径**，走
    `resolveAssetPath`（`ScriptComponent::path`）。junction 让引擎**零改动**，
    也让用户在外部目录里用任何编辑器打开同一个代码根。
  - ⚠ 删除工程时**只删 junction**（`rmdir` 不跟随目标），外部代码根必须保留 ——
    M2 的删除路径要专门测这条。

### 6.2 T2 · C++ 游戏模块（M5 可选，有前置）

**前置（必须先做完并跑通全量）**：把 `engine/**` 抽成 `venn_engine` STATIC 库，
两个 exe 只保留 `main.cpp` + 应用层。现在没有它，生成出来的 sln 是编不过的（D8）。

生成物：

```
<工程>/code/
  CMakeLists.txt          ← find_package / add_subdirectory(venn_engine)
  src/game_module.cpp     ← 继承 core::Application 的最小骨架
  venn_game.sln           ← cmake -S . -B build -G "Visual Studio 17 2022" 生成
```

**已经明说的代价**：这条路一开，"改玩法不重编译"的优势就没了。
所以它**不是默认值**，而是给"要接第三方 C++ 库"的工程用。

### 6.3 T3 · 发行包

不在本文范围。指向 `ship_roadmap`。启动器只留一个"导出…"的位置，M6 之后再说。

---

## 7 · 分阶段执行序

> 铁律：**每个阶段结束时都能手工验收，且不依赖后一个阶段。**
>
> **状态（2026-10-19）：M0 ~ M5-b 全部 ✅ 落地，M5-c（T2 C++ 模块）未做，
> M6 收口 ✅。** 逐项实测数字与踩到的坑 → §11 台账；仍未做的两项是
> M5-c（§6.2 的前置条件还没到）与 §9 里明确不做的 7 条。

### M0 · 地基：工程能被"手动"打开（规模 S，编辑器**零改动**）✅

**目标**：不写 UI、不改 `Editor.exe`，只靠"目录 + env + CWD"证明工程概念成立。

改动点：

1. **新建 `engine/project/ProjectFile.{h,cpp}`** —— 纯数据：读写 `venn.project.json`、
   路径规则（`projectRoot()` / `assetRoot()` / `startScene()` / `codeRoot()`）、
   逐条校验（`format` / 必有键 / 路径逃逸检查）。**不 include 任何 render/scene**。
2. **新建 `tools/gen_templates.py`** → 生成 `templates/{first_person,third_person}/`
   （确定性、可重复生成；场景结构抄 `assets/scenes/levels/script_demo.json`）。
3. **`templates/*/template.json`**：`id / label / gameType / startScene /
   placeholders[] / files[]`（`files[]` 给自检对账用）。
4. `Editor.exe` 零改动。

**验收（手工，三条）**：

```cmd
cd D:\tmp\MyGame
set MYVK_ASSET_DIR=D:\tmp\MyGame\assets
set MYVK_EDITOR_SCENE=scenes/main.json
"D:\...\build\bin\Editor.exe"
```

- ① 场景里有 Ground + Player，Hierarchy 能看到两个物体；
- ② 视口里相机位置符合类型（FP 在眼位 / TP 在斜后方）；
- ③ 关掉后 `config/editor_layout.json` 出现在**工程目录**下（不是仓库根）——
  这一条同时验证了 D5 与 P2。

**止损条件**：如果 ③ 不成立（布局仍写到别处），说明 CWD 契约有更深的绑定，
**先停下来改本文档**，别继续做 UI。

### M1 · 启动器窗口骨架（规模 M，只读）✅

- 新 target `VennLauncher`（`launcher/main.cpp` + `launcher/LauncherApp.{h,cpp}`）。
- `Application::startupShowDelay()` 虚函数（D2 的补丁 1）。
- 主体：§5.2 的布局；最近列表读写（§3.3）；"打开已有工程"（M3 的 `spawnEditor`
  先写个最简版，够本阶段验收）。
- `MYVK_EDITOR_PROJECT` 支持（D7）：`onInit` 最前面 `current_path()` + 标题栏。
- i18n：新词条进 `engine/i18n/ZhTable.cpp`。

**验收**：`verify_launcher_ui.py` —— 锚点 `LAUNCH-RECENT-*` / `LAUNCH-DETAIL` /
`LAUNCH-BTN-new`；截图人工看一眼排版（图标/对齐只能靠眼睛，见记忆里的教训）。

### M2 · 工程创建引擎（规模 M）✅

- `project::createFromTemplate(spec)`：
  1. 目标目录不存在或为空（非空 → 拒绝，带原因）；
  2. **先写到 `<name>.tmp` 再 rename**（写一半断电不留半成品）；
  3. 复制模板 → 替换白名单占位符（`__PROJECT_NAME__` / `__TEMPLATE_ID__`）；
  4. 写 `venn.project.json`；
  5. 追加 recents（失败不回滚工程，只提示）。
- 失败回滚：任何一步失败 → 删掉本次创建的整个目录（**只删自己建的**）。
- 向导 UI 接上（§5.3 四步）。
- `--selftest project`：纯数值（路径规则 / 校验 / 占位符替换 / 冲突检测），
  **不开窗口**，与 `MYVK_PHYS_TEST` 同一条"Vulkan 之前返回"的路。

**验收**：`verify_launcher_create.py` —— 新建后目录结构对账（用 `template.json`
的 `files[]`）、`game.json` 的分辨率/标题、重复创建被拒、失败无残留。

### M3 · 拉起与生命周期（规模 M）✅

- `spawnEditor(projectRoot)` / `spawnGame(projectRoot)`：
  - `CreateProcessW` + **自建 env 块**（`CREATE_UNICODE_ENVIRONMENT`）：
    `MYVK_ASSET_DIR` / `MYVK_EDITOR_SCENE` / `MYVK_EDITOR_PROJECT` /
    `MYVK_LAUNCHER_CHILD=1`；
  - `lpCurrentDirectory = projectRoot`（D5）；**宽字符路径**（P8）；
  - 启动器随后退出（默认）。勾"保持启动器打开"则隐藏到托盘 → 不实现，只做退出。
- 错误可读化：Editor.exe 不存在 / 路径含非 ASCII / 权限不足 / 工程被占用。

**验收**：`verify_launcher_open.py` —— 创建 → 自动拉起 → 等到 `Venn Editor` 窗口 →
断言标题含工程名、`EDITOR-READY` 行里的实体数 = 模板实体数。

### M4 · 画面设置接线（规模 S，但**必须**先解掉 P4）✅

- 分辨率/全屏 → `assets/game.json`（真实生效，P5）。
- 画质 → `config/render.json`：把 `editor::applyQualityPreset` 那几档的
  **开关集合**导出成 `<feature>.<param>` 平铺键写进去（格式见 `render.example.json`）。
  - ⚠ `editor::` 的函数在应用层，启动器能调；但**更干净**是把预设表下沉到
    `engine/render/`（`kPresets` 现在在 `FeatureUIGen.cpp` 里）。下沉属于重构，
    单独一刀，别混在启动器里。
- `RuntimeConfig::quality`：**要么接线（映射到上面的 render.json）、要么删掉**。
  不许留着"读进来打日志"（P4）。

**验收**：`verify_launcher_display.py` —— 改分辨率后 `Game.exe` 窗口客户区尺寸；
改画质后渲染截图**显著变化像素占比** > 0.5% 且 > 10× 基线（判据见记忆"断言纪律"）。

### M5 · 代码解决方案（规模 L，可选，先决条件在 §6.2）· M5-b ✅ / M5-c 未做

- **M5-a（必须先做）**：`engine/**` → `venn_engine` STATIC 库；两个 exe 只留
  `main.cpp` + 应用层。⚠ 改 `CMakeLists.txt` = 横切 = **全量回归 + 用户明确下令**。
- **M5-b**：T1 外置代码根（junction）+ 编辑器侧一致性校验（`code.linked`
  与真实 junction 指向不符 → 打 WARN 并降级为"只读提示"）。
- **M5-c**：T2 生成 `code/`（CMake + 骨架 + 可选 `cmake -G` 出 sln）。

**验收**：外置后改一行 `.vks` → 编辑器热重载生效（`VKS-RELOAD-*`）；
T2 生成物 `cmake --build` 产出 exe 并 `--selftest` 通过。

### M6 · 收口（规模 S）✅

- i18n 全量词条（`verify_i18n_coverage.py` 六组都要绿）。
- 错误/成功提示走 `NotificationCenter`？**不** —— 启动器是独立进程，用自己的轻量 toast。
- 回归：`run_regression.py` 的 `DEFAULT` 加三个 `verify_launcher_*.py`，
  `TRIGGERS` 补 `launcher/**` / `engine/project/**` / `templates/**` / `tools/gen_templates.py`；
  ⚠ 改完 `TRIGGERS` 必跑 `check_regression_map.py`。
- 台账（§11）+ 记忆（`MEMORY.md` 加一段，**注意 16 KB 上限**）。

---

## 8 · 总验收（一条判据）

> **一台干净的机器、仓库零改动**：双击 `Launcher.exe` → 新建第三人称工程
> （改过路径、分辨率 1920×1080、画质 High）→ 编辑器**自动**打开、标题是工程名、
> 场景里是第三人称初值 → 播放，WASD 能走、鼠标能环绕；
> 回到启动器再新建一个第一人称工程 → 相机在眼位、鼠标直接转视角；
> **两个工程互不影响**（布局、最近列表、画质各自独立）；
> 全程**不改一行 `.cpp`、不重编译**。

---

## 9 · 明确不做（写下来是为了不反复讨论）

1. **不做引擎版本管理**（`venn.project.json.engine` 只记录不校验）。
2. **不做从 Git 克隆工程**。
3. **不做插件 / 模板市场**。
4. **不在启动器里预览 3D 场景**（M6 之后可选，不进主线）。
5. **不做自动更新**。
6. **不给 `Editor.exe` 加 argv 解析**（D7）。
7. **不做"工程内独立 shader"**（D4，那是 `MYVK_SHADER_DIR` 的事）。

---

## 10 · 与其它路线图的耦合

| 路线图 | 耦合点 |
|---|---|
| `script_roadmap.md` | T1 的全部能力都来自 S0~S6（已收口）；模板的 `main.vks` 直接用它 |
| `character_pipeline_roadmap.md` | A1（Animator）落地后，TP 模板才能"看见角色"（§4 的 ⚠） |
| `persistence_roadmap.md` | G16 起要有 `World::Record.guid`；`venn.project.json` 与 Prefab 共用 json 读取层 |
| `editor_roadmap.md` | **E26 画质档位已于 2026-10-19 落地**（与启动器共用 `QualityPresets` 与 `fillLayerForPreset`）；E20 覆盖指示器仍未做，启动器的"打开工程"不依赖它 |
| `ship_roadmap.md` | T3 发行包；启动器只留位置 |

---

## 11 · 台账（实测数字与坑，逐项追加）

### 11.1 交付物清单（新建 / 改动）

**新建**

| 文件 | 内容 |
|---|---|
| `engine/project/ProjectFile.{h,cpp}` | `venn.project.json` 读写 + `validate` + `isSafeRelative`；自检 43 条 |
| `engine/project/RecentList.{h,cpp}` | `%LOCALAPPDATA%/Venn/launcher/recent.json`，上限 10、`pruneMissing`、坏文件不崩；自检 14 条 |
| `engine/project/ProjectDisplay.{h,cpp}` | 分辨率写 `assets/game.json`、画质写 `config/render.json`（**22 个 `*.enabled` 全量平铺**）；自检 23 条 |
| `engine/project/ProjectCreate.{h,cpp}` | `createFromTemplate`（`.tmp` → rename + 失败删目录）+ `createDirectoryJunction`/`isDirectoryLink`；自检 32 条 |
| `engine/render/QualityPresets.{h,cpp}` | 五档预设表（从 `editor/FeatureUIGen.cpp` **下沉**，两个应用层共用） |
| `engine/core/Version.h` | `kVennVersion`（原先编辑器里私有一份） |
| `engine/ui/DebugRects.h` | `logRect` 下沉（启动器也要打锚点）；`editor/DebugRects.h` 变转发壳 |
| `launcher/main.cpp` | `--selftest project` / `MYVK_PROJECT_TEST=1` |
| `launcher/LauncherApp.{h,cpp}` | 约 900 行：`Idle/Type/Location/Display/Done` 五态 + 四步向导 + 锚点 `LAUNCH-*` |
| `launcher/LauncherSpawn.{h,cpp}` | `CreateProcessW` + **自建 env 块** + `DuplicateHandle` 传 std 句柄 |
| `launcher/LauncherDialog.{h,cpp}` | `IFileOpenDialog` + `FOS_PICKFOLDERS`（`CoInitializeEx` 成对） |
| `templates/{first_person,third_person}/` | 模板包：`venn.project.json` 占位 + `scenes/main.json` + `assets/scripts/main.vks`，材质全 procedural |
| `tools/gen_templates.py` | 生成上面两个模板（含给 Player 挂 `scripts/main.vks`） |
| `tools/launcher_kit.py` | 六个验收脚本共用：`LogTap` / `RECT_RE` / `click_anchor` / `press_space` / `wait_title` / `wait_pid_exit` / `fresh_dir` / `finish` |
| `tools/verify_launcher_{project,ui,create,open,display,code}.py` | 六个验收脚本（`python` / `ui` / `create` / `open` / `display` / `code` 六层） |

**改动**

- `engine/core/Application.{h,cpp}`：新增 `virtual double startupShowDelay()`（默认 2.0，
  启动器重写 0.0）——**P10 的正解**，不写就是"双击图标两秒没反应"。
- `engine/core/Window.{h,cpp}`：新增 `setTitle()`（编辑器标题改成 `Venn Editor - <工程名>`）。
- `editor/EditorApp.cpp`：①`onInit()` 开头打 `EDITOR-CWD path=`；②`MYVK_EDITOR_PROJECT`
  分支（载工程 → `current_path(root)` → 改标题 → 记 `m_projectScene` → `PROJ-CODE-LINK` 校验）；
  ③场景钩子改成"显式 env > 工程 startScene"。
- `editor/FeatureUIGen.cpp`：`kPresets`/`isEnableParam` 删掉，改调 `render::`。
- `CMakeLists.txt`：新 target `VennLauncher`（`OUTPUT_NAME Launcher`）；全局单例
  `VennScenesCopy`/`VennScriptsCopy`/`VennTemplatesCopy`（原先两个 `copy_scenes` **并发往同一个
  `build/bin` 拷**）；`templates/` → `<exe>/templates`。
- `engine/i18n/ZhTable.cpp`：启动器约 45 条 + 脚本面板 9 条；删 4 条**重复键**（`Close`/`Scene`/
  `Resolution`/`Folder` —— `emplace` 先到先得，后写那条永远读不到）。现 **1682 条**、重复键 0。
- `tools/run_regression.py`：`DEFAULT` +6、`TRIGGERS` +6、`WATCH_ROOTS` 加 `launcher`/`templates`、
  `RENDER_MODULES` 加 `QualityPresets.*`。

### 11.2 验收数字（全部实测）

| 项 | 结果 |
|---|---|
| `Launcher.exe --selftest project` | PROJ 43 + RECENT 14 + DISPLAY 23 + CREATE 32 = **112 条**，`PROJECT-ALL-TOTAL failures=0` |
| `verify_launcher_project`（M0） | FP / TP 各三条 PASS：`entities=3`、`camera.distance` 8.0 / 4.5、`config/editor_layout.json` 落在**工程目录**（不是仓库根） |
| `verify_launcher_ui`（M1） | PASS：`header=(10,8)-(1090,54)`、`left.right=335 < right.left=537`、`list.bottom=160 < footer.y=640`、死路径被剔除后剩 2 条 |
| `verify_launcher_create`（M2） | PASS：C1~C16 + F5~F16；`render.json keys=22`、`player.mode=third/first`、重名被拒 **且** 无 `.tmp` 残留 **且** 原工程 mtime 不变 |
| `verify_launcher_open`（M3） | PASS：`LAUNCHER-SPAWN ok pid=…`、启动器自己退出 rc=0、标题 `Venn Editor - OpenGame`、`EDITOR-CWD`=工程根、`entities=3`、两个进程日志在**同一流**里、编辑器 rc=0 |
| `verify_launcher_display`（M4） | PASS：1920×1080 / 1280×720 客户区、`RENDER-SET chain=project=22 total=22`、baseline 0.0929%、high-vs-low **5.1212% = 55.1×** |
| `verify_launcher_code`（M5-b） | PASS D1~D9：junction 真存在、`PROJ-CODE-LINK ok`、`precompiled 1 script (via junction)`、`VKS-RELOAD-KEEP entity=2 path=scripts/main.vks vars=6`、工程侧文件跟着变、rc=0 |
| `verify_i18n_coverage`（M6） | **PASS**：A1/A2/A3/A4/B/B2/C/D/E/F 全绿（词典 1682 条、重复键 0、占位符漂移 0） |
| `check_regression_map`（M6） | **PASS**（A/B/C/D/E/F 六项；`engine/render` 无未覆盖文件） |
| 六个脚本**同一个回合**连跑 | **6/6 通过**（7.5 / 7.6 / 27.0 / 11.5 / 69.6 / 12.5 s）—— 这条本身就是回归：一回合跑满就撞 P20 与 P21，两条都修完才可能全绿 |

### 11.3 这一轮新踩的坑（P13~P21，前 12 条见 §12）

**P13 · 编辑态脚本**不 tick，热重载在编辑态**不可观测**。`EditorApp::update` 里是
`if (isEditing() && animPlaying()) runAnimScripts(..)`，默认不播 → 实例表里
`inst.progVersion` 恒为 0 → `ScriptRuntime::tick` 的 `if (inst.progVersion != 0)` 那条分支
根本进不去，`VKS-RELOAD-*` 一条都不打。测外置代码根的热重载**必须先进 Play**
（判据用 `Runtime scripts:` 汇总行，别只看"我按了 Space"）。

**P14 · `ImGui::BeginChild` 是独立窗口** —— 盖在它上面的 `InvisibleButton` 的 hover
被父窗口挡掉：卡片画出来了、锚点有坐标、**点下去日志一个字都没有**。向导的类型卡片
最终改成"一个多行 `ImGui::Button` + 三个 `PushStyleColor`"，不用子窗口。

**P15 · 创建完工程，最近列表被写回旧值** —— `createFromTemplate` 自己会往
`recent.json` 追加，而启动器内存里是**启动时**读的旧列表，`onShutdown` 的 `save()`
把它覆盖掉（新工程凭空消失）。正解：`commitWizard()` 成功后先 `m_recent.load(nullptr)`
再 `refreshRecent(false)`；`runPendingSpawn()` 里 touch 之前也一样。

**P16 · TAA 让"同配置两次运行"也差 0.1%** —— `verify_launcher_display` 的 R7 对照组
（同配置跑两次）实测 0.1576%，原先按 0.05% 写就假红。控制上限改 0.3%（**必须明显
小于 R5 的 0.5%，也不能写 0**），主判据仍是 **10 倍**（R6：5.12% / 0.093% = 55×）。

**P17 · `shutil.rmtree(ignore_errors=True)` 静默失败 = 全绿但一个字没验** ——
删不掉时脚本照跑，读的是**上一轮**的旧文件。`launcher_kit.fresh_dir()` 原来的做法是
"删不掉就抛"，于是又撞上 P20。

**P18 · `Popen(stdout=PIPE)` 必须有人排空** —— 不排空 → 日志写满 64 KB 管道 →
子进程阻塞在 `write()` → "窗口一直不出现"且进程还活着，与"启动崩了"长得一模一样。

**P19 · `ctypes` 不声明 `argtypes` 会截断 64 位 HWND** —— `PostMessageW` **静默返回 0**，
现象是"发了关窗消息，进程照跑"。`post_message`/`get_window_text`/`open_process` 全部
显式声明。

**P20 · 宿主的批量删除保护会硬杀整个脚本** —— 一回合累计删超过 50 个文件后，任何
`rmtree` 都会让进程直接退出（日志里只有一行
`[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED]`，**没有 traceback**；
`ignore_errors=True` 接不住 —— 它不在 Python 层）。一个工程目录 15~30 个文件，所以
一个回合连跑三个 GUI 用例必撞（实测 `count:50 threshold:50` 与 `count:69`）。
正解：**用例脚本一个文件都不删** —— `fresh_dir()` 改成"改名让位"
（`launcher_code` → `launcher_code.old-HHMMSS` + 新建空目录；`rename` 不计数），
`verify_launcher_project` 的收尾清理与 `make_project` 的覆盖逻辑一起改掉。
代价是 `_venn_verify` 会留旧目录，人工清即可；**正确性上更安全**（永不读到旧状态）。

**P21 · 真实鼠标点击要求窗口在最前，否则第一下只是"激活窗口"、进不了 ImGui** ——
`click()` 走 `SetCursorPos` + `mouse_event`，打到的是**光标底下最上层的窗口**，不是我们
指定的 hwnd。窗口不在最前时的现象是"点了没反应、日志一个字都没有"，而且**完全看时序**
（同一脚本连跑三次，第二次偶发红在向导 Step1→Step2；实测一次
`建 HiRes 失败: 没有进入 Step2`）。正解：`launcher_kit.focus_window()`
（`ShowWindow(SW_RESTORE)` → 置顶 → 立刻取消置顶 → `SetForegroundWindow`），
`launch()` 拿到 hwnd 后调一次、**每次 `click()` 前再调一次**。
⚠ 别用 `AttachThreadInput`：它要挂到当前前台线程上，写错会把整个桌面输入搞乱。

### 11.4 刻意没做的（写下理由，免得下次重启讨论）

1. **创建工程不开线程**：四步同帧跑完，进度条只会闪一下；做成线程池只会引入
   "UI 读半成品"。真要进度就得先把模板做大到秒级。
2. **`MYVK_LAUNCHER_TEST_*` 只填初值**：四步向导仍要一步不少地真点 —— 否则等于
   拿脚本代替被测对象。
3. **外置代码根只填路径、不代勾复选框**：勾选本身是被测行为。
4. **`--selftest` 走 argv 不走 `MYVK_FRAMES`**：`renderSettingsAutomationRun()` 见到
   `MYVK_FRAMES` 会**跳过**项目层 `render.json` 的读写，自检就白测了。
5. **M5-c（T2 C++ 游戏模块）没做**：§6.2 的前置条件（工程内独立 CMake 与
   `Runtime.exe` 的启动契约）还没到；T1（`.vks` + junction）已经能覆盖"改代码
   → 热重载"这条主线。

---

## 12 · 坑（执行时会撞上的）

**P1 · `Editor.exe` 丢弃 argv** —— 传参数**没有任何提示**地无效，且会渲染到超时。
启动器只能走 env + CWD。要传帧数用 `MYVK_FRAMES`（不是 `--frames`）。

**P2 · CWD 是隐式契约** —— `config/*`、`imgui.ini`、`editor_window.ini` 全相对 CWD。
启动器不设 `lpCurrentDirectory` 的后果是：两个工程的编辑器布局**互相覆盖**，
而且没有任何日志。

**P3 · 资产候选链是"第一个存在的赢"** —— 工程偷偷依赖 `build/bin/assets`
（shader/图标/默认材质）。测试"工程自包含"的做法：把 `build/bin/assets` 改个名再打开，
**缺的东西会立刻现形**（这招很好用，加进 M2 的自检）。

**P4 · `quality` 是空挂字段** —— 写它 = 什么都没发生。M4 必须先接线或删除。

**P5 · 只有分辨率真的生效** —— 别在 UI 上把两者画成一样的可信度。

**P6 · 第三人称"看不见角色"** —— 不是 bug，是角色链没到（§4 ⚠）。文案要提前写。

**P7 · 中文路径** —— `CreateProcessW` 必须宽字符；env 块自己拼，别用
`SetEnvironmentVariable` 污染父进程（父进程还要继续跑 UI）。

**P8 · 半成品目录** —— 创建失败不清理 → 下次创建被"目录非空"拒，用户以为坏了。
修法：写 `.tmp` 再 rename + 失败删目录。**只在"本次创建的目录"上做**。

**P9 · 模板替换只走白名单** —— 全目录字符串替换会改坏材质名/资产哈希。
`template.json.placeholders[]` 是白名单，别扩大。

**P10 · 启动器也吃 2 秒启动延迟** —— `Application::run` 里写死
`kStartupShowDelay = 2.0`。不覆盖 `startupShowDelay()`，用户会看到"点了图标没反应"。

**P11 · 最近列表里的死路径** —— 工程被移动/删除后，点开必须降级为
"从列表移除 + 提示"，不能崩、不能弹模态。

**P12 · 启动器是新的 GUI 用例** —— 与既有纪律同一条：**GUI 用例同时只能跑一个**
（`kill_stale_editors` 无差别杀 `Editor.exe`/`Game.exe`，将来也得杀 `Launcher.exe`）；
锚点统一 `LAUNCH-*` 前缀；断言前先确认窗口已显示（P10 修好之前它两秒才出现）。
