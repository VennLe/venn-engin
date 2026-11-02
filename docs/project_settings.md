# Project Settings 真面板（B3）

> 2026-11-02。对应审计文档 `panel_gap_review_2026-11-02.md` 的 B3 条。
> 判据：`tools/verify_project_settings.py`（六组，46 条 PASS）。
> 引擎：`engine/project/GameFile.{h,cpp}`（读/写/夹取 + 自检 29 条）；
> 编辑器：`editor/ProjectSettingsPanel.{h,cpp}`（面板 + 命令）。

## 1. 它补的是什么洞

`Edit ▸ Project Settings...` 打开的是**画质档位**那张面板：

```cpp
// editor/Toolbar.cpp:567（改之前）
panelOpenRef("quality") = true;
```

而一个工程的真正出厂配置住在 `assets/game.json`：
标题 / 启动场景 / 分辨率 / 全屏 / `maxFps` / `timeScale` / `language` /
`quality` / `uiDoc` / `startState` / 整个 `audio` 块 —— **全部只能手改 JSON**。
`grep -rn "game.json" editor/` 在改之前只命中一句 tooltip。

具体贵在哪：

- 一个策划要把游戏标题改掉，得先知道这个文件在哪、键名是什么大小写；
- 改 `width` / `height` 时没有夹取，写成 `40` 会被原样接受，
  下次启动窗口宽 40 px，而**界面上没有任何提示**；
- `language` 有 `""` / `"auto"` / `"system"` 三种等价写法，手写时不知道写哪个"对"。

## 2. 怎么用

### 2.1 打开

三种路都通到同一张面板（`panelOpenRef("projectsettings")`）：

- 菜单 `Edit ▸ Project Settings...`
- 面板菜单 / 快捷键（`PanelRegistry` 表里的一行）
- 命令 `project.settings`（打进控制台，每字段一行，可复制）

### 2.2 22 个字段

键名就是 `game.json` 里的**真实键名**（`audio.` 前缀表示子对象）。这张表是
唯一真源 —— 面板每一行、命令每一次 `set`、判据每一条断言都从
`kFieldNames[]` 取：

| 段 | 键 |
|---|---|
| General | `title` `scene` `uiDoc` `startState` `width` `height` `fullscreen` |
| Loop | `maxFps` `unfocusedFps` `timeScale` |
| Localization | `language` |
| Render defaults | `quality` |
| Audio（出厂值） | `audio.enabled` `audio.device` `audio.master` `audio.music` `audio.sfx` `audio.voice` `audio.mute` `audio.muteOnFocusLost` `audio.duckDepth` `audio.maxVoices` |

界面上的文字全部走 `TR()`，锚点（`##PROJ-*`）不随语言变。

### 2.3 命令

```
project.settings                 # 打印全部 22 个字段（PROJ-SETTINGS key=… value=…）
project.set <key> <value>        # 改一个字段（值可带空格，例如标题）
project.save                     # 落盘
project.reload                   # 从磁盘重读（丢弃未保存改动）
```

`project.set` 的值从第 3 个参数**整段拼回**，所以 `project.set title My Game`
不需要引号。键名匹配大小写不敏感（`uiDoc` / `uidoc` 都行）。

### 2.4 自动化锚点

`PROJ-RECT` `PROJ-PATH` `PROJ-DIRTY` `PROJ-SAVE` `PROJ-RELOAD`
`PROJ-OPEN-QUALITY` `PROJ-NOTE`；每字段一个（`PROJ-TITLE` `PROJ-WIDTH` …）；
数值行的微调按钮 `PROJ-DEC-*` / `PROJ-INC-*`（`PROJ-DEC-WIDTH` 等）。

## 3. 语义细节（这几条都是踩出来的）

### 3.1 不存第二份配置

`ProjectFile.h` 早就写死了这条纪律：**工程只允许一份真值**。所以面板不是
"读进内存 → 自己维护 → 退出时刷盘"，而是**按需读盘 → 改 → 写回同一份文件**。
好处是"落盘 = 真值"：面板上看到的就是下一次启动会用的东西，中间没有一层
可能漂移的缓存。

### 3.2 写回是**合并**，不是整份重写

`saveGameFile()` 先读现有文档，再覆盖我们管的键：

```cpp
json j = json::object();
f >> j;                       // ① 读现有
j["title"] = s.title;         // ② 只盖自己管的
j.dump(2);                    // ③ 原子写回
```

`game.json` 里我们**不管**的东西至少有三类：`_comment`（作者手写的注释）、
`player`（25 个字段的角色模型配置）、废弃别名 `audioDevice`。整份重写会把
它们全静默清掉 —— **而且不报错**，界面上一切正常，直到下次 Play 发现角色
变回默认模型。

判据 C 组因此必须先往文件里**注入** `player` 与 `zzUnknownKey`：
出厂 `game.json` 里本来没有这两个键，"未知键保留"在它上面恒真是假绿。

### 3.3 坏 JSON 时**拒绝写**

解析失败时不"从空对象重写"，而是原样退回并说明原因：

```
PROJ-GAME-SAVE path=… ok=0 err=existing file is not valid JSON, refusing to overwrite: …
```

判据 F 组验的是"文件一字未变"，不是"返回了 false"。

### 3.4 `language` 空串要**写出来**

`""` / `"auto"` / `"system"` 三种写法在运行时等价（都是跟随系统），面板统一
落盘成 `""`。但**必须写出来而不是省略键**：省略之后，"我把语言改回 auto 了"
与"这个工程从没配过语言"在磁盘上无法区分，排查时看着像没保存（自检 `G2d`）。

### 3.5 `uiDoc` 空则**删键**

唯一一个反例。`uiDoc` 是 #312 才加的字段，老工程里都没有它；每存一次就多
一行 `"uiDoc": ""` 会让 diff 变吵。语义上"键不存在"与空串完全等价（都不画
UI 层），所以删键更干净（自检 `G3` 段）。

### 3.6 夹取而不是拒绝

写盘前统一走 `clampGameSettings()`：

| 字段 | 区间 |
|---|---|
| `width` / `height` | 320 ~ 7680 |
| `maxFps` / `unfocusedFps` | 0 ~ 1000（0 = 不限帧） |
| `timeScale` | 0.01 ~ 10 |
| `audio.master/music/sfx/voice/duckDepth` | 0 ~ 1 |
| `audio.maxVoices` | 4 ~ 256 |

夹了就打 `PROJ-GAME-CLAMP`（面板上也会显示一行提示）。**为什么夹而不拒**：
拒绝保存会让作者卡在"不知道哪一格坏了"，而 20 多个输入框里找一个越界值
是纯折磨。夹取是安全的 —— 它只可能把值变得**更合法**。

读盘时也夹（自检 `G7`）：手写 `{"width": 40}` 的老文件读进来直接是 320，
面板不会把 40 摆出来让人以为能用。

### 3.7 "未保存"靠**快照比较**算出

`m_saved` 是最近一次成功读/写的快照，`m_dirty = !sameSettings(m_st, m_saved)`。
不用"每个控件改动时置脏位"：控件 20 多个，漏一个的后果是"改了却显示已保存"
→ 用户直接关窗口 → 改动丢了。比较法对"哪个控件改的"一无所知，只看值变了没有。

浮点用 `1e-4` 容差比较（`sameSettings` 里的 `fe()`）。用 `==` 的话滑块走一位
（`0.7000001` vs `0.7`）就判成"改了"，面板会永远显示"未保存"，而用户一个键
都没碰过。

### 3.8 `quality` 只是**标签**

真正的渲染参数在 `config/render.json`（`QualitySettingsPanel` 管）。本面板的
`quality` 字段只被记进日志、不被应用 —— 所以面板上**明写这一点**，并给一个
`Quality settings...` 按钮直达那张真面板。藏着不说 = 作者改完标签发现画面
没变，然后开始怀疑引擎。

### 3.9 `audio` 块是**出厂值**，不是当前增益

与 Audio 混音台（A2）不是一件事：

| | 写谁 | 什么时候生效 |
|---|---|---|
| 本面板 | `game.json` 的 `audio` 块 | 下次启动 |
| 混音台 | 运行时的总线增益 | 立刻，**从不落盘** |

两条都动 `master`。不说清楚的话，用户会以为在混音台拖推子改的是工程配置，
或者以为在这里改音量能当场听到变化。面板上直接写了这段说明。

### 3.10 按钮与命令走**同一条路**

`PROJ-SAVE` 按钮调 `save()`，命令 `project.save` 也调 `save()`；`setField()`
同理。两套实现的话，结果一定是"能点不能存"或者"命令改了界面不认"。

### 3.11 `markDirty()` 不能绑

`game.json` 写完就是写完了，不属于"场景有未保存改动"。绑进场景脏标记会让
"关编辑器时提示保存场景"莫名弹出来。

### 3.12 `idLabel` 的 tag 必须**自带 `##`**

`idLabel(TR("Save"), "##PROJ-SAVE")`，而 `idLabel` 内部是 `shown + tag`。
不写成 `"##" + tag` 的原因是 `verify_i18n_coverage.py` 会把 ImGui 实参里所有
字符串字面量当界面文字 —— `"PROJ-SAVE"`（大写开头、无空格）会被判"没包 TR"，
而 `"##…"` 命中它的 `SKIP_PREFIX` 白名单。全仓其它面板本来就这么写
（`"##AM-FADER-MASTER"`）。

⚠ 但 `tagItem("PROJ-SAVE")` 的 tag **不能**带 `##` —— 它收的是 `logRect` 的
锚点名，带上之后 `PROJ-SAVE` 这个锚点会从日志里消失，而判据 B 组正好断言它。

## 4. 判据表

`tools/verify_project_settings.py`，六组：

| 组 | 验什么 | 关键断言 |
|---|---|---|
| A | 引擎自检 | `pass>=29 fail=0` + `PROJECT-ALL-TOTAL failures=0` + 15 个用例名（`G1`~`G7`） |
| B | 面板接线 | 15 个锚点都在 + `project.settings` 读全 22 字段 |
| C | 合并语义 | `set → save → reload → settings` 值来自磁盘；**前置注入的** `player` / `zzUnknownKey` 原样保留；数值类型仍是 int/float |
| D | 按钮真的落盘 | `wait_rect("PROJ-SAVE")` → 点 → 磁盘上 `title=BtnSaved` |
| E | 端到端 | 存 `width=1600 height=900` 后 `Game.exe --frames 20` 读到 `window=1600x900`，且**不是默认 1280x720** |
| F | 坏 JSON | `PROJ-GAME-SAVE ok=0` + 文件一字未变 |

全程用 `shutil.copy2` 快照 `game.json`，`finally` 还原。

**几条不能省的**：

- **C 组的前置注入**：不注入未知键，"未知键保留"是假绿（出厂文件里没有）。
- **D 组必须"点按钮"**：只测命令的话，"按钮接了但没接对"这条永远不会红。
- **E 组必须换非默认值**：用 1280×720 测的话，读没读盘都一样。
- **F 组查"文件未变"**：查返回值只能证明函数返回了 false。

## 5. 已知边界

- **只能改 `game.json` 里已有的这些键**。`player` 块（角色模型 / 挂点 / 碰撞
  半径等 25 个字段）仍只能手改 —— 那是运行时唯一配置读路径，动它的收益是 0
  （面板不编辑它），风险却不小。防漂移靠的是 E 组端到端判据。
- **没有字段级的"恢复出厂"**。要恢复就把整个文件删掉，下次启动会以
  `created=1` 重建（自检 `G1`）。
- **`dist` 目录是只读摘要**，不能改。出包是 `File ▸ Package`（E28/G29）。
- **没有工程级的多环境**（dev / prod 两套 game.json）。要的话目前靠
  `MYVK_` 环境变量覆盖，见 `docs/` 里关于环境变量顺序的那一节。
