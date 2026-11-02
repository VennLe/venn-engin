# 关卡流送（G19，2026-11-02）

> 2026-11-02 之前：`grep -rn "LevelStreaming\|LevelManifest"` 全仓 **0 命中**。
> 编辑器只认"当前打开的那一个场景文件"，游戏只能一次加载整个世界。
> 后果不是"慢"，是**做不了**大世界：几千个实体全在内存里、全进 BVH、
> 全参与剔除，而且没有"走近才加载"这件最基础的事。

- 内核：`engine/scene/LevelManifest.{h,cpp}` / `engine/scene/LevelStreamer.{h,cpp}`
- 面板：`Authoring ▸ Levels`（`editor/LevelPanel.*`）
- 清单资产：`assets/levels/*.vlevels`
- 验收：`tools/verify_levels.py`（A~G，31 条内核 + 6 组界面）

---

## 一、两块拼图

| | 回答什么 | 住在哪 |
|---|---|---|
| **清单** `LevelManifest` | 这局游戏由哪些关卡组成、哪张常驻、哪张按位置流送 | 独立资产 `.vlevels` |
| **流送器** `LevelStreamer` | 现在该在内存里的关卡是哪些，谁要加载、谁要卸载 | `EditorApp::m_levels`（按值成员） |

⚠ 清单**不塞进场景文件**：它是"整个世界"的视图，而场景文件是"世界的一部分"。
塞进场景的话，加载 A 场景才知道 B 场景的存在 —— 而 A 恰恰是还没加载的那一半。

---

## 二、叠加加载（`loadSceneAdditive`）

### 2.1 只给 `instantiateDocument` 加了一个 `bool additive`

`SceneObject.h` 的三段式（G16-5）早就是为这件事准备的：

```
① parseSceneDocument      纯 CPU
② instantiateDocument     纯 CPU（建实体）
③ resolveDocumentAssets    唯一要设备的一段
```

原注释里就写着"G19 流送要后台解析 / 主线程上传"。所以这次**没有新写一套加载**，
只给 ② 加了 `bool additive = false`：

```cpp
if (!additive) {
    scene.clear();
    ... 相机 / 光照 / 物理 / 环境 / scriptVars / postVolumes ...
}
... 建实体 + 父子关系（两条分支共用）...
```

⚠ **globals 必须整块跳过，不能"只跳过相机"**。副关卡如果把自己的
`light` / `environment` / `postVolumes` 应用上去，**主关卡刚调好的光照与天空
会被静默覆盖** —— 看起来像"流送之后画面变了"，一个很难归因的 bug。
副关卡的 globals 在流送语义里**没有意义**：它们是"这一份世界"的全局设置，
而流送进来的是世界的一部分。

⚠ `scriptVars` 同理：它是主关卡的黑板，副关卡往里写等于偷改剧情状态。

### 2.2 卸载的依据是"本次加载建出来的实体"

`loadSceneAdditive(..., std::vector<ecs::Entity>* createdOut)` 把 ② 建出来的
实体（只含有效的）交出去。卸载就是逐个 `world().destroy(e)`。

⚠⚠ **绝不能按"名字前缀"或"包围盒里的所有实体"反推**：玩家可以把一个箱子
拖进那个盒子里 —— 它不属于这张关卡，按包围盒卸就会把它一起删掉。

⚠ 卸载前必须 `world().alive(e)`：编辑器里删过的实体句柄已经过期，
在 swap-and-pop 的池子里 destroy 一个过期句柄是未定义行为。

### 2.3 已知边界

**跨文件的 `parentGuid` 引用不生效** —— `entityApplyHierarchy` 只在本次
`created` 里查 guid。要跨关卡挂父子得在加载完之后手动 `setParent`。

---

## 三、流送判定（三条不变量）

### 3.1 常驻关卡要**主动加载**，不只是"不卸载"

```cpp
if (s.entry.alwaysLoaded) {
    if (!s.loaded) loadLevel(...);   // ← 第一次进 Play 没人会手动 load
    continue;
}
```

只写"不卸载"的话，常驻关卡永远是未加载状态，而**不报错**。

### 3.2 加载盒 ≠ 卸载盒（滞回）

| | 判据 | 默认 |
|---|---|---|
| 加载 | viewer 在 `halfExtent` 盒内 | — |
| 卸载 | viewer 出了 `halfExtent * unloadScale` 盒 | `unloadScale = 1.25` |

⚠⚠ 两个盒写成同一个的后果：玩家在边界上左右晃就是"加载 → 卸载 → 加载"，
每次都重新解析一遍场景文件 —— 表现成**"走到某条线上游戏就卡"**，
而日志里是一串 `SCENE-STREAM`，看不出是抖动。

`unloadScale < 1` 会被钳回 1（卸载盒比加载盒还小 = 永远卸不掉）。

### 3.3 距离是"到盒面"，不是"到中心"

```cpp
const glm::vec3 d = glm::abs(viewer - e.center) - e.halfExtent;
return glm::length(glm::max(d, glm::vec3(0.0f)));   // 盒内 = 0
```

用中心距的话，一张 128m 宽的关卡里玩家站在最边上时"距离"是 64 ——
按半径判定就会把它误判成"太远"。

### 3.4 `update()` 是两趟，不是一趟

先算"该加载"（会改变 `loaded`），再算"该卸载"。合成一趟时，一张刚被加载的
关卡会在同一趟里又被判定"要不要卸"，顺序一乱就变成"加载完立刻卸"。

---

## 四、清单格式

```json
{
  "name": "world",
  "levels": [
    { "name": "main", "path": "scenes/levels/first_level.json",
      "alwaysLoaded": true },
    { "name": "script_demo", "path": "scenes/levels/script_demo.json",
      "center": [40, 0, 0], "halfExtent": [32, 32, 24] }
  ]
}
```

| 字段 | 说明 |
|---|---|
| `name` | 清单内**唯一**的 id（命令与面板都用它）。空 = 整份失败 |
| `path` | 相对**资产根**的场景文件 |
| `alwaysLoaded` | 常驻：`update()` 保证它一直加载着 |
| `enabled` | 默认 true；false = 流送不看它（手动 load 仍可以） |
| `center` / `halfExtent` | 触发盒（世界坐标，米，Z-up）。**两个都要有或都没有** |

### 校验是严格的

| 情形 | 结果 | 为什么 |
|---|---|---|
| 缺 `name` / 缺 `path` | **整份失败** | 缺名字 = 面板点不到它；缺路径 = 直到加载才发现 |
| 重名 | **整份失败** | 收下一半会让其中一个静默不加载，现场像"数据丢了" |
| 只有 `center` 没有 `halfExtent` | **整份失败** | 手写清单最常见的错，而它的表现是"这张关卡永远不流送" |
| 负的 `halfExtent` | 取绝对值 | 手写 `-32` 是笔误，abs 一下比让玩家永远进不去盒子好 |
| 没有 `levels` 键 | 合法（空清单） | 纯程序生成的世界没有关卡文件 |

---

## 五、面板与命令

`LVL-*` 锚点：`RECT / COUNT / VIEWER / RELOAD / LOAD / UNLOAD / UNLOADALL /
LIST / ROW-<name> / SRC / STATUS`。

⚠ 行锚点打**两套**（与 `SCRDBG-LINE` 同一理由）：`LVL-LIST` 被 `logRect`
去重成一条，留哪一条取决于首帧布局是否已稳定（实测留的是**最后一行**）——
点它等于随机选一张关卡。要指定那一张就用 `LVL-ROW-<name>`。

```
level.manifest <path>
level.list
level.count
level.state
level.load <name>
level.unload <name>
level.unloadall
level.stream <x> <y> <z>
```

⚠ 全部走 `EditorApp::applyLevelAction()`（与面板按钮同一条路）。

`level.stream` 走的是 `LevelStreamer::update()` **本体**，不是"按坐标直接 load"
—— 后者能过判据却证明不了距离/滞回那段逻辑。

---

## 六、Play 里的驱动位置

```cpp
runRuntimeScripts(simDt);
updateLevelStreaming();     // ← 这里
runPlayers(simDt);
```

| 位置 | 为什么 |
|---|---|
| 在脚本**之后** | 脚本可能瞬移玩家（过场 / 传送门），流送要按**瞬移后**的位置判定，否则玩家会先在空世界里站一帧 |
| 在玩家**之前** | 新进来的地面必须先存在，玩家这一帧的重力/碰撞才有东西可站（否则会掉穿一帧再被弹回来） |

⚠⚠ **只在 Play 里调**。编辑态流送会把副关卡实体写进**编辑场景**，
下一次 `scene.save` 就把它们固化进主场景文件了 —— 副关卡从此粘在主场景里，
"怎么删都删不掉"。

### viewer 取谁

三级回退，每一级失败都退到下一级：

1. 运行态里带 `CameraComponent` 的实体（玩家实体必须挂它 —— 这是 P6 的规矩，最准）；
2. 出生点实体（`GameStartComponent`，它不挂网格，所以 ① 找不到时它最像玩家）；
3. 场景相机。

⚠ 三级都退不到才用原点 —— 用原点会让原点附近那张关卡被无故加载，
表现成"一进 Play 就多了一堆实体"。

---

## 七、判据表

| 组 | 断言 | 为什么它不可替代 |
|---|---|---|
| A | 31 条内核 | 解析 / 校验（4 种拒收）/ 往返 / 距离 / 滞回 |
| B | 8 个锚点 + `LEVEL-COUNT total=3` | 面板存在且清单读进来了 |
| C | **`world=` 加载前后只增不减**（0→3→5） | 唯一能抓"误用整份替换"的判据；"加载成功了"对那个错误完全无效 |
| C | 加载后每张的 `entities=` 等于它场景文件里的实体数 | 指错文件（把 arena 的路径写在 hub 上）加载也会**成功** |
| C | 没有 `Scene loaded: *_lvl_*` | ⚠ 不能写成"整份日志没有 Scene loaded"——编辑器**启动**时会用它打开主场景 |
| D | `loaded` 序列 **2→2→1→2→1** | 按顺序比；同盒实现中间那步会变 1 |
| E | 跑到 9999 米后 hub 仍 `loaded=1` | 常驻永不自动卸载 |
| F | 11 米（缓冲带内）**不许**卸 | 滞回；缺了它就是"走到某条线上就卡" |
| G | 鼠标点 `LVL-ROW-hub` → `LVL-LOAD` → `LEVELS-CMD load name=hub ok=1` | "界面点不到 = 没交付" |

---

## 八、自动化里的坑

| 坑 | 症状 | 正解 |
|---|---|---|
| `LVL-LIST` 去重 | 点它加载的是**最后一行**（far）不是 hub | 用 `LVL-ROW-<name>` |
| `entities=` 的语义 | 未加载时是 0，不是"场景文件里的数量" | "指对文件"的断言放**加载之后** |
| `LEVEL-STATE` 正则漏段 | 写成 `levels=3 loaded=1 entities=3` 永远匹配不上 | 它是 `levels= loaded= wanted= entities= failed= viewer= world=` 一整串 |
| 判据范围过宽 | 把编辑器**启动**加载主场景的 `Scene loaded` 算成流送的错 | 只看 `_lvl_` 那几张 |
| 夹具 guid 撞车 | 三张关卡共用同一批 guid → `SCENE-GUID-DUP`（不中断但变浑） | 派生时清掉 `guid` / `parentGuid` |
