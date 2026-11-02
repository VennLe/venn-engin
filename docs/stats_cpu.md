# Stats 的 CPU 分段与进程内存（B2 / C9）

> 2026-11-02。对应审计文档 `panel_gap_review_2026-11-02.md` 的 B2 / C9 条。
> 判据：`tools/verify_stats_cpu.py`（A / A2 / B+C）。
> 引擎：`engine/core/CpuProfiler.{h,cpp}`（12 段计时）、
> `engine/core/ProcStats.{h,cpp}`（进程内存）；
> 打点：`engine/core/Application.cpp`（帧分区）+ `engine/render/Renderer.cpp`
> （drawFrame 细分）+ `runtime/RuntimeApp.cpp`（GameLoop 的两个回调）；
> 面板：`editor/EditorApp.cpp` 的 `drawStatsPanel`。

## 1. 它补的是什么洞

GPU 侧从 R0-1 起就有 `GpuTimer` 的 11 段时戳，E15 那轮又把它画成了堆叠条。
CPU 侧只有一个 `io.Framerate`。

于是最基本的一个问题在界面上无法回答：

> 这帧 12ms —— 是卡在**等 GPU**，还是卡在自己**录命令**，还是卡在**场景同步**？

三者的处理方式完全相反：等 GPU 要去砍 GPU 负载（分辨率 / pass / 着色），
录命令太重要去减 draw call 或合批，场景同步慢要去查视锥剔除 / 蒙皮 / 上传。
一个总数把它们糊在一起，等于没有信息。

顺带补上进程内存：一次性的巨大分配（重建交换链 / 载入大场景）在瞬时值上
抓不到，只有峰值能。

## 2. 12 段

### 2.1 帧分区（五段互不重叠，加起来 ≈ `frame`）

| 段 | 覆盖什么 | 大了说明 |
|---|---|---|
| `events` | `pollEvents` + 时间步进 | 消息泵有问题（拖窗口 / 大量输入） |
| `scene` | `activeScene().update` + LOD 惰性生成 | 场景实体太多，或每帧都在建 LOD 链 |
| `logic` | `onUpdate`（脚本 / 物理 / 玩法 / 状态栈） | 脚本或物理太重 |
| `ui` | `ImGui::NewFrame` + 面板构建 | 某个面板在每帧重建（表格 / 树） |
| `assets` | 纹理流驱动 | 正在补 mip 层（载入大贴图时的正常现象） |
| `render` | `drawFrame` 整体 | 见下一张表 |

### 2.2 `render` 的细分（五段互不重叠，加起来 ≤ `render`）

| 段 | 覆盖什么 | 大了说明 |
|---|---|---|
| `wait` | `vkWaitForFences` | **CPU 在等 GPU**，不是自己在干活 |
| `sync` | `uploadSceneState`（视锥剔除 / Hi-Z / 蒙皮 / FrameUBO） | 随**物体数**增长的那部分 |
| `record` | `recordFrame`（命令录制 + 各 pass 的 CPU 侧准备） | 随 **draw call 数**增长的那部分 |
| `submit` | `vkQueueSubmit` | 正常很小；大 = 驱动在等 |
| `present` | `vkQueuePresentKHR` | vsync 打开时它会节流整帧 |

`render − Σ细分` 本身也是有价值的读数：那是 `drawFrame` 里头尾的检查
（尺寸重建 / 描述符重写 / 探针 / 调试日志）。**这块一旦变大，通常意味着
某条"每帧只该比对开关位"的路径在反复重建资源。**

## 3. 怎么用

### 3.1 在界面上

Stats 面板顶部（帧时间曲线下面）两条通栏堆叠条：

- 上条 = 帧分区（6 段）；
- 下条 = `render` 细分（5 段）。

悬停读精确毫秒与占比（与右边 GPU 条同一套"按累计占比反查"的算法）。

下面一行 `Memory: working set / peak / private`。

再下面 `CPU breakdown (last / avg / max ms)` 可展开（**默认收起** —— 12 行
展开会把浮动面板撑到和上面的 Scripts 面板重叠）。

**`max` 是那张表里最有用的一列**：`avg` 正常而 `max` 有尖峰 = 某条路径在
**偶发**重建资源。每帧重建的路径 `avg` 也会大，两者不是一件事。

### 3.2 命令行

```
MYVK_CPU_STATS=1 Game.exe --frames 120
```

每 30 帧一条（与 `GPU-TIMING` / `CULL-STATS` 同频、同为 `key=value`）：

```
CPU-STATS frame=16.71 events=0.02 scene=0.01 logic=0.15 ui=0.03 assets=0.00
          render=2.11 wait=0.16 sync=0.33 record=1.40 submit=0.10 present=0.02
```

**不开就一行都不打**（`verify_stats_cpu` 的 A2 组守这条）。日志默认零噪音
这条纪律在很多老用例里被当作前提（按行数 / 正则断言）。

## 4. 语义细节（踩出来的）

### 4.1 `beginFrame()` 推进的是**上一帧**

`CpuProfiler::beginFrame()` 在被调用的一瞬间把上一帧的累积推进历史环、再清零。
好处是**不需要配对调 `endFrame()`** —— 某一帧 `continue` / `break` 掉不会污染
历史，也不需要每个调用点都记得收尾。

代价是：日志里读到的 `frame` 永远是**上一帧**的完整值。这正是我们要的
（本帧的 `kFrame` 作用域要到循环体末尾才析构）。

### 4.2 `beginFrame()` 的位置在 `pollEvents` **之前**

放在之后的话，"等 vsync 醒过来"那段会被记进 `events` —— 而那恰恰是
"CPU 空闲"最该被单独看见的一段。

### 4.3 ⚠⚠ `kLogic` 不能包住整个 `onUpdate`（运行时的那个坑）

编辑器的 `onUpdate` 是纯逻辑；**运行时的 `onUpdate` 是整个
`GameLoop::tick`**（固定步 + 变量步 + 渲染）。在 `Application::run` 里对
`onUpdate` 整体打一个 `kLogic`，`render` 就会被算进 `logic`：

```
frame=30.59  logic=18.84  render=1.88     ← 错（logic 含 render）
frame=30.59  logic=0.15   render=2.11     ← 对
```

这种错的可怕之处是**每个单独的数字看起来都合理**，只有把五段加起来才会
发现 20.72 > 30.59 的一半、堆叠条冲出 100%。所以判据里必须有一条
`顶层五段之和 <= frame`。

现在的做法：`Application` 只在 `!ownsFrameRendering()` 时套 `kLogic`；
运行时由 `GameLoop` 的 `cb.fixedUpdate` / `cb.variableUpdate` 自己打
（见 `RuntimeApp::onUpdate`）。

### 4.4 累加而不是赋值

同一个 `Scope` 允许被多次打点（`ui` 就在 `ImGui::beginFrame` 和 `onImGui`
各打一次；`logic` 在固定步与变量步各打一次），`addNs` 是**累加**。
这样"一段散落在多处"不需要额外的合并逻辑 —— 少一处判断就少一处漂移。

### 4.5 不用 `high_resolution_clock`

它在不同标准库上可能是 `system_clock`，而 `system_clock` 会被对时服务
往回拨 —— 那会让某一段读出负的毫秒数。用 `steady_clock`（MSVC 上是 QPC）。

析构里还有一道兜底：`d > 0` 才累加。负值会让 `max` 永远算对、`avg` 被
悄悄拉低，属于最难查的那类。

### 4.6 进程内存三个数各自回答一个问题

| 字段 | 回答什么 |
|---|---|
| `workingSet` | 任务管理器里那一列。涨了要说清"涨在哪" |
| `peak` | 一次性的巨大分配在瞬时值上抓不到，只有峰值能 |
| `private` | 我们真的占了这么多。工作集可被系统随时回收，掉下去不代表释放了 |

`PROCESS_MEMORY_COUNTERS_EX` 的 `cb` 必须给 `sizeof(EX)` —— 给基础版的
大小会被按基础版截断，`PrivateUsage` 恒 0。

## 5. 判据表

`tools/verify_stats_cpu.py`：

| 组 | 验什么 | 关键断言 |
|---|---|---|
| A | `Game.exe --frames 48` + `MYVK_CPU_STATS=1` | 12 键全在；**`render >= wait+sync+record+submit+present`**（嵌套关系）；**`顶层五段之和 <= frame`**（不重复计数）；`record>0` / `sync>0` / `submit>0` / `present>0` |
| A2 | 阴性对照 | **不开 env 时零条 `CPU-STATS`** |
| B | 面板锚点 | `STATS-CPU-BAR` / `-DETAIL` / `-TREE` / `STATS-MEM`；帧条在明细条**之上**；两条都有宽度 |
| C | 展开分段表 | 点 `STATS-CPU-TREE` → `STATS-MEM` 下移 **> 80px**（实测 Δ=396） |

**两条不能省的**：

- **嵌套关系**（A 组）：唯一能证明五段确实是 `drawFrame` 的**细分**、
  而不是五个各自独立的计数器的判据。接错位置（比如把 `record` 套在
  `uploadSceneState` 外面）会让这条红。
- **C 组量的是位移，不是"点了不崩"**：`logRect` 只给锚点、表格行没有锚点，
  所以用"表下面的那一行被推下去多少"当判据。不展开的实现、或者展开了但
  表是空壳的实现，都不会下移。

## 6. 已知边界

- **只测主线程**。`CpuProfiler` 不加锁，也没有线程 ID 校验 —— 从工作线程
  打点会撕裂（表现为读数随机偏小）。目前所有打点都在主线程。
- **`wait` 与 `present` 的边界取决于驱动**：有些驱动在 `vkQueuePresentKHR`
  里就完成节流，有些在下一帧的 `vkWaitForFences`。两者都是"CPU 空闲"，
  看和的时候不要纠结分界。
- **没有火焰图 / 调用树**。这是"分段"不是"profiler"：12 段是**手工选定**的，
  想看更细要加 `Scope`（改一个枚举 + 一个名字，没有别的注册表）。
- **`kStatsH = 300`** 是浮动面板码放的估算值（Stats 在 Scripts 上方）。
  面板内容再长下去要同步调它，否则两块会重叠。
