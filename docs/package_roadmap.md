# 工程的「建」与「打包」—— 现状体检 + E28 出包路线图

> 2026-10-28。回答一个问题：**"我的引擎在项目构建/打包上还是空白"这个判断，
> 哪些成立、哪些其实早就做完了。**
>
> 结论先放这里：**"新建独立游戏工程"早就有了**（模板 + 启动器向导 + 工程文件），
> 真正空白的是**出包**（把工程变成能发出去的目录）。本轮的 **E28-1** 把这一刀补上了。

---

## 1 · 体检表（对着代码核过，不是对着文档猜）

| 能力 | 状态 | 证据（文件 / 命令） |
|---|---|---|
| 模板包（第一/第三人称） | ✅ | `templates/first_person`、`templates/third_person`（各 13 个文件：场景/脚本/地形/材质/贴图/配置） |
| 新建独立工程（复制模板 + 占位符 + 身份文件） | ✅ | `engine/project/ProjectCreate.{h,cpp}`；`--selftest project` 自检 43+ 条 |
| 工程文件 | ✅ | `venn.project.json`（`engine/project/ProjectFile.{h,cpp}`） |
| 启动器（新建向导 / 最近列表 / 拉起编辑器与游戏） | ✅ | `launcher/`（`LauncherApp` + `LauncherSpawn`，每工程一份 CWD / 资产根） |
| 编辑器认工程（标题栏挂工程名、切 CWD） | ✅ | `EditorApp.cpp` 的 "D7" 段（`MYVK_EDITOR_PROJECT`） |
| 运行时读工程（入口 `assets/game.json`） | ✅ | `runtime/RuntimeConfig.cpp` |
| **出包（一键变可分发目录）** | **✅ E28-1（本轮）** | `engine/project/ProjectBuild.{h,cpp}`、`Game.exe --package`、`File ▸ Package project...` |
| 引用闭包（只打被引用的资产） | ❌ | 见 §5（G29） |
| `.pak` 单文件 + 压缩 | ❌ | 见 §5（G29） |
| Shipping 剔除（无编辑器/无调试层） | ❌ | 见 §5（G30） |
| 崩溃回收（minidump） | ❌ | `ship_roadmap` G31 |

⚠ 易混：**"新建工程"和"出包"是两件事**。

- `ProjectCreate` ≈ UE 的 **New Project Wizard** → 产出一个**可编辑**的工程。
- `ProjectBuild` ≈ UE 的 **Project ▸ Package Project** → 产出一个**可分发**的目录。

---

## 2 · 一个工程长什么样（与 UE5 / Unity 对照）

```
MyGame/                       ← 独立工程根（自己一份 CWD、自己一份资产根）
  venn.project.json           ← 身份：名字 / 模板 / 起始场景 / 代码方案 / 引擎配置
  assets/                     ← 工程自己的资产（"内容"）
    game.json                 ←   运行时入口（窗口尺寸、起始场景、音频…）
    scenes/main.json          ←   场景
    scripts/*.vks             ←   脚本（也可以外置成目录联接）
    terrain/ materials/ textures/ animators/ sequences/ dialogues/
  config/render.json          ← 画面设置
  dist/<Name>/                ← 出包的产物（本轮的 E28-1）
```

| | venn | UE5 | Unity |
|---|---|---|---|
| 工程标识 | `venn.project.json` | `*.uproject` | `ProjectSettings/ProjectVersion.txt` |
| 内容根 | `assets/` | `Content/` | `Assets/` |
| 运行时入口 | `assets/game.json` | `Config/DefaultGame.ini` + GameMode | `ProjectSettings/` |
| 新建 | 启动器向导（模板包） | New Project Wizard | Hub |
| 出包 | `File ▸ Package project...` | Platforms ▸ Windows ▸ Package | Build Settings ▸ Build |

---

## 3 · E28-1：出包做了什么

### 3.1 产物结构

```
dist/MyGame/
  MyGame.exe        ← 引擎运行时（改名；**不是** Editor.exe）
  shaders/          ← 引擎着色器（263 个 .spv）
  assets/           ← 工程资产 + 引擎内置资产（见 §3.3）
  config/           ← 画面/输入配置
  build.json        ← 清单：每个文件的 size + 哈希，加打包元信息
  README.txt
```

实测（`templates/third_person` 的模板工程）：**321 个文件 / 38.6 MB**，
其中 **307 条是引擎侧资产**（shaders 263 + 字体/色带/图标/LUT）。

### 3.2 三个入口

```
# 命令行（默认输出到 <工程根>/dist/<Name>）
Game.exe --package <工程根>
Game.exe --package <工程根> --out <目录> --release-only
Game.exe --selftest package           # 18 条无窗口自检

# 编辑器
File ▸ Package project...             # 输出到 <工程根>/dist/<Name>，结果走通知条
```

⚠ **出包用的是 `Game.exe` 自己**（`GetModuleFileNameA`），不是另开一个工具：
"打包出来的 exe"与"正在跑的 exe"必须是同一份字节，否则会出现"编辑器里好好的、
发出去的包跑的是另一个版本"。

### 3.3 `build.json`（清单）

```jsonc
{
  "format": 1, "engine": "venn", "name": "MyGame", "exe": "MyGame.exe",
  "startScene": "scenes/main.json",
  "builtAt": "2026-10-28T03:50:12Z",
  "builtFrom": "D:/games/MyGame",
  "hashKind": "fnv1a64",        // ⚠ 明写算法名：换算法时读包的人不用猜
  "totalFiles": 321, "totalBytes": 38593297,
  "engineFiles": 307,           // 引擎侧资产条数（与项目资产分开计数）
  "warnings": ["engine exe looks like a Debug build ..."],
  "files": [ {"path":"assets/game.json","size":143,"hash":"c1a2…"} ]
}
```

---

## 4 · ⚠ 第一版真踩到的两个坑（都只在打包版出现）

开发机 `build/bin/` 里**什么都有**，所以"引擎资产不在工程里"这件事完全隐形。
把 exe 拷进一个只有项目资产的目录，症状是这两条 —— **都不指向资产管理**：

| 缺什么 | 症状 | 第一现场 |
|---|---|---|
| `<exeDir>/shaders/` | 管线建不出来 → 退出时 **24 条** `VUID-vkDestroyDevice-device-05137`（image / device memory / framebuffer 未销毁），**rc=1** | 日志尾 |
| `assets/fonts`、`assets/ui/fonts` | ImGui `Assertion failed: Fonts.Size > 0` → **弹断言框、进程卡死、日志停在上一行** | `UI2-FONT failed path=...` + `ImGui font not found` |

根因：`resolveAssetPath` / `resolveShaderPath` 是**逐候选根回退**的
（`MYVK_ASSET_DIR` → `exeDir/assets` → `cwd/assets`）。开发时 exeDir 恰好是
`build/bin`，回退永远命中；打包后 exeDir 是 dist，里面只有项目资产。

**修法**：出包时按**白名单目录**补引擎侧资产（`ProjectBuild.cpp` 的
`kEngineAssetDirs`），并且**项目优先**（项目里有同名文件就不覆盖）。

```
shaders/  assets/fonts  assets/ui  assets/ramps  assets/icons
assets/materials  assets/textures
```

⚠⚠ **新增"引擎内置资产"（新 LUT / 字体 / 默认材质）必须往这张表加一行**，
否则**只有打包版**缺它 —— 开发机永远复现不了。

另一条同源的坑：`build/bin/Game.exe` 是 **Debug**（默认 `CMAKE_BUILD_TYPE=Debug`），
依赖 `MSVCP140D / VCRUNTIME140D / VCRUNTIME140_1D` —— 这三个 DLL **不允许再分发**。
出包会检测出来并写进 `build.json.warnings`（`--release-only` 时直接拒绝）。

---

## 5 · 还没做（按建议顺序）

| 下一步 | 内容 | 为什么排在这里 |
|---|---|---|
| ~~**G29-a 引用闭包**~~ | ✅ **2026-10-28 已做**（见 §7）：只打被引用的资产，悬空引用 = 打包失败 | — |
| **G29-b `.pak`** | 单文件 + 索引 + 压缩（zstd），`MYVK_ASSET_PAK=0/1` 切换 | 依赖 G29-a 的文件清单 |
| ~~**G30 Shipping**~~ | ✅ **2026-10-28 已做**：`make ship` → 只有 Game.exe / 校验层强制关 / 开发命令编译期剔除 / pdb 归档 | — （见 §9） |
| 平台 | 目前只有 Windows（MSVC）。跨平台要先有各自的构建与资产目录规则 | — |
| 签名 / 安装包 / 增量更新 | — | 等真的有人要发 |

---

## 6 · 判据

`tools/verify_package.py`（**39 条**，40 秒跑完；只有 G 组要开窗口）：

| 组 | 内容 |
|---|---|
| A（7） | `--selftest package` 18 条全绿（正常打包 / 清单对账 / 缺入口必须失败 / 目标非空必须拒绝 / Debug 必须带警告 / 引擎资产缺失必须点名） |
| B（12） | 真打一次模板工程：产物有 exe/assets/config/build.json/README；**没有** Editor.exe、**没有** venn.project.json；带了 263 个 shaders、字体、SDF 图集 |
| C（6） | 清单与磁盘**逐条**对账（条数 + 每个 size + 覆盖差集为空 + engineFiles>0） |
| D（4） | **干净目录测试**：把包挪到工程外、删掉源工程 → 跑 20 帧 → `rc=0`、**0 条** Vulkan 校验错误、无"资源找不到"警告、日志里确有物理推进 |
| E（3） | 场景缺失 → `rc=2` + `PACKAGE-FAIL` + 报错点名缺哪个文件 + 不产出 dist |
| F（2） | dist 已存在且非空 → 拒绝，且原内容一个字节不动 |
| G（4） | **编辑器入口**：`File` 菜单在 → 有 `Package project...` → 点它真出包 → 落在 `<工程根>/dist/<Name>` |

⚠ D 组是这组用例的价值所在：**没有它，"能跑"永远只证明"源工程还在的时候能跑"**。

---

## 7 · G29-a 引用闭包（✅ 2026-10-28）

### 7.1 目标

从 `assets/game.json` + 起始场景出发，只把**真的被用到**的资产打进包；
遇到"写了资产名但文件不在"的情况 —— **打包失败**（`ship_roadmap` G29 验收第 2 条）。

实测（`templates/third_person`，11 个项目资产）：闭包 **9 个**，剔掉 2 个
（`scripts/pickup.vks` / `scripts/portal.vks` —— 只在 `main.vks` 的注释里被提到）。
整包 **317 个文件 / 38.6 MB**（含 307 条引擎侧资产）；干净目录跑 20 帧 **rc=0、0 条校验错误**。

### 7.2 为什么不做"按字段名解析"

场景 JSON 里装资产路径的字段有几十个（`script.path` / `mesh.modelKey` /
`terrain.source` / `terrain.layers[]` / 材质十几种贴图槽 / 动画的 `skeleton.path` /
粒子贴图 / 贴花……），**每加一个特性就多一个**。按字段名写的扫描器一定会落后于
格式，而漏掉的那一个不会报错 —— 只会在玩家机器上黑屏。

改成：**任何 JSON 字符串值都是候选**，用一份**资产索引**判定它是不是真文件。
加新字段不需要改扫描器。

索引（建一次，之后全是哈希查表，不碰磁盘）：

```
exact[rel]              = rel          // "textures/a.png"
noExt[去掉扩展名的 rel]  = [rel...]     // "materials/ground"  → ground.vmat
byBase[只剩 basename]    = [rel...]     // "intro"             → sequences/intro.seq.json
```

三种写法因此都能命中：

| 写法 | 出现在哪 | 靠哪张表 |
|---|---|---|
| `terrain/ground/terrain.vterrain` | 场景 `terrain.source` | `exact` |
| `materials/template_ground`（**无扩展名**） | 地形分层图 `layers[]` | `noExt` |
| `playSequence("intro")`（**纯 id**） | `.vks` 脚本 | `byBase` |
| `height.r32`（**相对引用者自己**） | `.vterrain` / `.gltf` 的 uri | 先按 `dirname(引用者)` 探 |

⚠ 第 4 条是踩出来的：地形资产里 `"height": "height.r32"` 是**相对 vterrain 自己**的，
不是相对 assets 根。第一版没探这一下，地形永远"少两个文件"。

### 7.3 什么算"未解析"（判据就在这两条）

只有两种字符串会被当成引用：

1. 以**已知资产扩展名**结尾（`.json/.vmat/.vterrain/.png/.glb/...`，见 `kExts[]`）
2. 以**已知资产目录**开头（`materials/` `textures/` `sequences/` …，见 `kIdDirs[]`）

两条都不满足 → 安静跳过（实体名、剧情文本、日志格式串都在这一类）。
满足但索引里找不到 → **unresolved → 打包失败**，报错格式 `引用者 -> 引用内容`。

### 7.4 三个必须记住的坑

①⚠⚠ **`_comment` 不是引用**。模板的 `game.json` 有一条 `_comment` 写着
"见 `config/render.json`" —— 它以 `.json` 结尾 → 会被当成引用 → 因为
"assets 下没有 config/" → 把打包判死。于是 JSON 遍历时**跳过 `_` 开头的键**。

② **"绝对路径"只认盘符和 UNC**。第一版是"含冒号就算绝对路径"，结果中文散文里的
`优先级: 命令行` 被报成绝对路径警告（模板 `_comment` 里就有这一句）。
现在判据：`X:/` 或 `//server`。

③⚠⚠ **`continue` 不等于剪枝**。`recursive_directory_iterator` 跳过名字之后
**递归仍会进去**：`assets/ramps/.trash/` 里的 `diag_rb.vramp.105210`、
`shaders/pbr.k0.frag.spv.hotreload-bak.trash-192907` 都混进了包。
必须 `disable_recursion_pending()`，并且 `.trash-<数字>` 这种残片要单独拉黑
（它既不是 `.tmp` 也不是 `.bak`）。

### 7.5 两条设计取舍

- **`.vks` 只认引号里的字符串**：注释里的路径不算引用。模板 `main.vks` 的注释里
  就有 `assets/scripts/pickup.vks` 这种说明性路径 —— 跟着它走会把没人用的脚本打进包。
- **`ui/` 与 `fonts/` 无条件带上**（`alwaysInclude`）：它们是"运行时按固定名字打开"
  的目录，**没有引用者**，靠闭包永远扫不到；少了就是字体断言框（§4 坑二）。
- **`config/` 永远整棵带**：同理，运行时按固定名字读 `render.json`。

### 7.6 判据

| 用例 | 条数 | 内容 |
|---|---|---|
| `--selftest scan`（S 组） | **16** | 闭包与预期**逐条相等** / 孤儿不进 / 材质 id / 脚本 id / 注释不触发 / 缺材质与缺贴图各自点名 / 未解析时已解析部分照收 / 环引用不死循环 / 坏 JSON 只 warning / 入口缺失必须失败 |
| `--selftest package`（P 组） | **23** | 原有 18 条 + 闭包默认开（孤儿不进）/ `build.json` 的 `closure`+`pruned` / `--no-closure` 对照 / 悬空引用失败且点名 + 不产出 dist |
| `--selftest scan` 的"精确集合"那条 | 1 | S2 用 `files == want` 逐条比：多一个少一个都红（比"包含某些文件"强得多） |
| `verify_package.py` | **60** | 含 **B8~B13**（闭包剔孤儿、被引用的一个不能少）、**H 组**（`--no-closure` 对照）、**I 组**（悬空引用 → rc=2 + 点名；同工程 `--no-closure` 能打出来）、**D 组**（干净目录跑：闭包没漏东西的最强证据） |

⚠ **对照组的必要性**：只验"包里没有孤儿"证明不了"是闭包干的"（也可能是别的原因
漏了整目录）。H 组用 `--no-closure` 把同样的工程整棵拷一遍，孤儿必须**在**包里。

### 7.7 还没做

- ~~`.pak` 单文件 + 压缩~~ → **G29-b 已做，见 §8**。
- 引擎侧资产的白名单还是"整目录"（`assets/materials` 会把开发用的 15 个材质也带上）。
  它们的体积很小（≈2 MB），等有性能诉求时一起按引用收敛。

---

## 8 · G29-b/c/d 资产包 game.pak（✅ 2026-10-28）

### 8.1 目标与结果

| | 树模式（G29-a） | 包模式（G29-b/c） |
|---|---|---|
| 分发**文件数** | **317** | **5** |
| 资产 + shader | 52 + 262 个散文件 | **1 个 `game.pak`** |
| 包内原始字节 | 21,127,921 | 21,127,921（同上，一个字节不差） |
| 包字节（压缩后） | 21,127,921 | **14,599,294**（69.1%） |
| 整包字节 | 38,765,592 | **32,270,677**（-16.8%） |

顶层长这样（对比 UE：`.pak` 之于 `Content/`）：

```
GcPak/
  GcPak.exe      game.pak      config/      build.json   README.txt
```

开关两个入口：

```
Game.exe --package <根> --pak
编辑器  File ▸ Package project as .pak...
```

`File ▸ Package project...`（不带 pak）仍然产出散文件目录 —— **默认不变**。
理由：刚打完包最常见的问题是"包里到底有什么"，散文件能直接翻；pak 是**发版**用的。
等 §8.9 的"直读"做完、pak 变成没有可观察代价的形态，再把默认翻过去。

### 8.2 容器格式（v1，全小端）

```
┌ 头 32 字节 ───────────────────────────────────┐
│  0..3   'V''P''A''K'                           │
│  4..7   u32 version = 1                        │
│  8..11  u32 flags   = 0（未知 flags 必须拒绝） │
│ 12..15  u32 entryCount                         │
│ 16..23  u64 indexOffset                        │
│ 24..31  u64 indexSize                          │
└────────────────────────────────────────────────┘
[条目数据…] [索引 JSON]
```

索引是 **JSON** 不是二进制表，理由都是"排查成本"：

- 30 KB 的索引对 38 MB 的包可以忽略；
- 坏了能 `python -c` 直接读（二进制表要另写解析器 —— 而解析器本身会出错）；
- 加字段不用升版本，只有**语义**变了才升。

条目：`{"p":"assets/textures/a.png","o":32,"cs":123,"us":456,"c":"lz4","h":"<fnv1a64>"}`
（路径 / 偏移 / 压缩后 / 原始 / 编解码 / **原始内容**的哈希）。

⚠ **`p` 是相对 dist 根的路径**，所以一个包能装多个根：`assets/…` 与
`shaders/…`。运行时查询时按根前缀拼 key（`resolveAssetPath` 拼 `assets/`，
`resolveShaderPath` 拼 `shaders/`）。将来把 `config/` 也收进来只是多一个前缀，
**不需要**第二条挂载链。

### 8.3 编解码：自己写 LZ4 block，不引依赖

仓库里没有 zlib / zstd（`third_party/` 里只有 glfw / glm / imgui / nlohmann /
stb / tinygltf / tinyobjloader / ufbx / VMA）。为一次出包引一个压缩库，
要连带"它的构建系统、它的许可证、它的 Windows 二进制"三件事 —— 不值。

LZ4 block 的**解压**只要四十行，**压缩**用贪心哈希表也四十行，而且是公开的
稳定格式（将来想换 zstd 只需在 `PakCodec` 里加一个枚举值，容器不用动）。

格式约束（写错一条，标准解压器就拒绝）：

1. `offset ∈ [1, 65535]`（窗口 64 KB）—— 所以 `c < i` 而不是 `c != i`
   （同一位置命中会算出 offset=0，那是非法值）；
2. 最后一段**只有字面量**，结尾没有 offset；
3. 最后 5 字节必须是字面量、最后 12 字节里不能再起新匹配。

压不动就 `store`（PNG/JPG 必然压不动）——`build.json` 里能看到 `stored` / `lz4`
各多少条。

> 实测 52 条里 **40 条 lz4、12 条 store**；最大的单条是 1.3 MB 的
> `.jpg`（存原文），所以整体只压到 87%。真正吃压缩的是 JSON / vmat / vks。

### 8.4 ⚠ 运行时怎么读：**只拦截一个函数**

三十来处资产读取点写法各不相同（`ifstream` / `stbi_load` / `ufbx_load_file` /
`tinyobj::LoadObj`），逐一改是**三十次回归风险**。所以拦截点选在它们共同的上游：

```
resolveAssetPath(rel)
  ① 磁盘候选根逐个试  → 命中就返回（开发态 / 编辑器 / 热重载全靠这条）
  ② 都没有 → 幂等探测 <exeDir>/assets.pak → 有就把该条目落到缓存并返回缓存路径
  ③ 都没有 → 返回 candidates.front()（交给读取处报"打不开"）
```

这样 `stbi_load("…")` 这种只吃路径的库也照样能用，且**一行都不用改**。

缓存是**内容寻址**的：`%LOCALAPPDATA%\Venn\assetcache\<fnv>\<basename>`
（`MYVK_ASSET_CACHE` 可覆盖）。目录名就是内容哈希 → 换了内容自然换目录，
**不存在读到旧字节**的可能；也不再需要"包版本号"这套东西。

`makeAssetRelative` 配了一张反查表：从缓存落出来的文件被写回场景时，
存的是原始相对路径（否则会存进 `C:/Users/.../assetcache/<hash>/a.png`，
换台机器就打不开）。

### 8.5 代价（说清楚，别假装没有）

**资产**走"按需解包"：首次访问会在缓存里落一份字节。实测（模板工程，冷缓存，
20 帧）**12 个文件 / 1.5 MB** —— 只有真的被读到的那些。
`makeAssetRelative` 有反查表，所以缓存路径不会被写进场景。

**shader 走直读，一个字节都不落盘**（§8.6）。第一版是"也落到缓存"，实测
**210 个 .spv / 13 MB**（启动时几乎要建全部管线变体，"按需"等于"全解包"）——
正是这个数字促成了直读：`resolveShaderPath` 命中包时返回虚拟路径
`pak:shaders/x.spv`，`vkutil::readFile` 认这个前缀，从包内存直接返回。

所以现在的全貌是：冷启动写 **1.5 MB**（资产），shader 走内存。pak 的成本只剩
"资产那一小块按需解包"。

收益仍然是实的：分发是**一个文件**、完整性可校验（打开包就校验结构，读每条就
核对哈希）、玩家不会遇到"少了一个 png 所以黑屏"（少文件在打开包时就报错）。

零解包直读（内存映射 + 每次解到内存）要改前面那三十处调用点；等有性能诉求时
**按格式分批**迁移（贴图 / 场景优先），而不是一次性铺开。

### 8.6 ✅ shader 也进了包（G29-c）

`shaders/` 有 262 个 `.spv`，是文件数的绝对大头（317 里占 263）。这一轮把它收进
**同一个** `game.pak`：

- 拦截点还是"一个函数"：`resolveShaderPath` 在四个磁盘候选都失败后，查
  `shaders/<文件名>`。所以 `vkutil::readFile(resolveShaderPath(x))` 那六十来个
  调用点**一行没改**。
- 顺带解决了一件事：包里同时有 `assets/` 与 `shaders/` 之后，**dist 顶层只剩 5 项**。
- ⚠ `watchShaderFiles()` 故意把**全部关键字变体**都注册进热重载（"多听几个空路径
  比漏听安全"），所以 `resolveShaderPath` 会有大量**未命中**。这条分支必须便宜：
  `Pak::has` 只做一次哈希查表，不碰磁盘。
- 数值：整包 38.8 MB → **32.3 MB**；pak 21.1 MB → 14.6 MB（**69%**，SPIR-V 很好压）。

**直读（G29-d）**：`resolveShaderPath` 命中包时返回 **`pak:shaders/x.spv`**（虚拟
路径，不落盘），`vkutil::readFile` 见前缀就调 `assets` 注册进来的读包钩子，从内存
返回字节。两处改动、五十来个调用点一行没改。

- `rhi` 是最底层、不能 include `assets` → 用一个 `inline` 函数指针
  （`vkutil::g_pakRead`，C++17 inline variable）做依赖倒置：`assets` 挂载成功后填，
  卸载时清空。默认 `nullptr`，所以**开发态完全不受影响**。
- 插件的失效行为要写明：出包后 `ShaderHotReload` 拿到的 `resolved` 是 `pak:` 路径，
  `last_write_time` 必然失败 → 永远不触发。这是**对的**（包里的 .spv 改不了），
  但必须在代码里写清楚，否则以后看起来像 bug。
- 判据就是那个"两实现不一样"的数：**冷启动后缓存里的 `.spv` 数量必须是 0**
  （落盘版是 210）。J14 就是它。

### 8.7 判据

| 用例 | 条数 | 关键内容 |
|---|---|---|
| `--selftest pak`（K 组） | **27** | 编解码往返（重复文本 / 随机 / 1 字节 / 超长匹配走 255 扩展）/ 坏流拒绝 / 容器往返（store + lz4 两种）/ 大小写与反斜杠查找 / **同样内容产出逐字节相同的包** / magic 被改、文件截断、数据被改一字节都**必须拒绝** / 写侧拒绝 `../` 与大小写重名 |
| `--selftest package`（P 组） | **32** | P24~P31 + P30b：pak 产物结构（`game.pak`、`assets/` 与 `shaders/` 都没了）、`config/` 仍是散文件、清单里没有 `assets/**` `shaders/**`、`pak.entries` 与结果一致、**从包里读回的场景与源文件逐字节相同**、**包里真有 `shaders/*.spv`**、对照（非 pak 模式必须是两棵目录树） |
| `verify_package.py`（J 组） | **15** | 端到端：`--pak` 出包 → 拷到别处 + **删源工程** + **空缓存** → 跑 20 帧 rc=0 / 0 条校验错误 / `ASSET-PAK mounted` / 缓存落盘 / 加载路径确实是我们的缓存根 / **包内条目 >250（含 shader）** / **缓存里真有 .spv** / **dist 顶层只剩 5 项** |

⚠ **J 组的四条要一起看**：包文件存在、体积变小，都不能证明"运行时真的从包里读"——
包可能压根没挂上，游戏靠磁盘上残留的 `assets/` 目录跑得很开心。所以必须
「干净目录 + 空缓存 + mounted 日志 + 缓存落盘 + 0 校验错误」同时成立。
另外 J10 用的是 Vulkan 校验层的**错误计数**，不是 `rc`：VUID 是 ERROR 级日志，
但只要不致命 `rc` 仍是 0（第一版缺 shader 时就是 `rc=0` + 24 条 VUID）。

### 8.8 三个坑

① ⚠ **解压时 `dst.push_back(dst[start+k])` 不能直接传引用**：`push_back` 内部
扩容会让引用悬垂。虽然 `char` 是按值传的（临时上没问题），但显式取一个局部
变量更不容易被后人改坏 —— 这类"重叠复制"的代码一旦被改成 `append(&dst[x], n)`
就是内存错误。容器里所有长度都先和 `rawSize` 对账，任何越界直接拒绝。

② ⚠⚠ **匿名命名空间少一个 `}` → 公开函数变成 `project::(anon)::X` → 每个调用点
C2668 二义**。这一轮改 `ProjectBuild.cpp`（把 README 正文改成函数）时，我的文本
锚点吃掉了 `} // namespace`，于是 `buildStepLabel` / `buildProject` 落进了匿名
命名空间，报错是"对重载函数的调用不明确"而不是"缺括号" —— 症状离病因很远。
（同族坑：前向声明与定义的 struct/class 不一致 → LNK2019。这个仓对"文本级编辑
把结构改坏"特别敏感：改完先编译一次再往下走，别连着改三处。）

③ ⚠ **手工造测试工程必须替换 `__PROJECT_NAME__`**：模板 `game.json` 里
`"title": "__PROJECT_NAME__"` 是占位符，正规路径由 `ProjectCreate` 替换。
我一开始用 `shutil.copytree` 直接造工程，窗口标题就成了字面量
`__PROJECT_NAME__` —— 结果 `find_window("PakGame")` 找不到窗口，
表现为"进程活着、截图 0 张"。教训：**造工程要走和用户一样的路径**（或至少
补上它做的每一步）。

### 8.9 下一步（G29-d）

- ~~shader 直读~~ ✅ 已做（§8.6）。
- **资产也直读**：贴图/场景现在是"落到缓存再读"。要做成直读得改
  `stbi_load(path)` / `ufbx_load_file(path)` 这类**只吃路径**的调用（换成
  `*_from_memory`），大约十来处，风险比 shader 那条大。收益是冷启动 0 写入。
- **默认值翻成 pak**：现在还得显式选。翻之前要先把"资产也直读"或至少
  "缓存可清理"做完 —— 不然玩家机器上会长期留 1.5 MB×N 个工程的缓存。
- `config/` 是否也进包（现在故意留散文件：玩家要能改画质）。

---

## 9 · G30 Shipping 剔除（✅ 2026-10-28）

> 与 G29 是两件事：G29 管"包里装什么"，G30 管"那份 exe 是什么"。
> 详细设计 / 踩坑 → `docs/ship_roadmap.md` §10 的 G30 一节。

- `make ship` → `build-ship/bin/Game.exe`（`RelWithDebInfo` + `VENN_SHIPPING=ON`）。
  Editor / Launcher 的 target 在 shipping 构建里**根本不声明**。
- 开发钩子（`--selftest` / `--package` / `MYVK_RUNTIME_SELFTEST` / `MYVK_FIXED_DT`
  / `MYVK_PHYS_DUMP`）是**编译期**剔除的：出货 exe 里这些字符串各 **0** 次
  （dev exe 里各 > 0，作对照区）。
- 校验层在 shipping 下强制关（`INSTANCE build=shipping … validation=off`）。
- git 号编进二进制：`--version` / 日志 `GAME-BUILD` / shipping 标题栏。
- **出包时用 dev 版当工具、指定出货 exe 当内容**：
  `Game.exe --package <工程> --engine build-ship/bin/Game.exe [--pak] [--pdb|--no-pdb]`
  → `build.json` 里 `flavor=shipping`、`git=…`、`pdb=debug/Game.pdb`；
  pdb 归档进 `dist/<Name>/debug/`（**不在 exe 旁边**），默认跟风味走。
- 判据：`tools/verify_shipping.py`（A~D 共 39 条）。

### 9.1 还没做

- **崩溃转储**（G31）：现在 shipping 只是"不弹断言框"，真正的
  `MiniDumpWriteDump` + 日志轮转还没写。
- **资产直读 / pak 当默认**（§8.9 那三条，原样保留）。
