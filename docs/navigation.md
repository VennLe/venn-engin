# 导航与 AI（C8，2026-11-04 / 脚本动词 11-05）

> 在此之前：`grep -rn "NavMesh\|navmesh\|pathfind"` 全仓 **0 命中**。
> 引擎能做"玩家自己走"（CharacterController + PlayerController），
> 但**没有任何方式让一个 NPC 自己走到某处** —— 街上走动的村民、巡逻兵、
> 追击的敌人、棋盘上挪动的单位，一个都做不了。
> 这一份补的就是这条：**格子导航网格 + A\* + 代理跟随 + 网格资产 + 脚本驱动**。
>
> 对应 `docs/remaining_work_2026-10-27.md` §2.3 的 **C8**（NPC / 寻路 / AI）。
> ⚠ 注意 `docs/panel_gap_review_2026-11-02.md` 里的 "C8" 是**脚本断点调试**，
> 两个编号撞了名，但它们不是一件事。

- 内核：`engine/nav/NavGrid.{h,cpp}`（纯 CPU）、`engine/nav/NavBake.{h,cpp}`（要射线）
- 代理：`engine/nav/NavSystem.{h,cpp}` + `ecs::NavAgentComponent`
- 存盘：`engine/nav/NavSerialize.{h,cpp}`（组件块 + `.vnav.json` 资产）
- 面板：`Authoring ▸ Navigation`（`editor/NavPanel.*`）+ `nav.*` 命令
- 脚本：`navGoto` / `navArrived` / `navFailed` / `navStop`（§6.1）
- 运行时：`RuntimeApp::setupNav / tickNav`
- 验收：`Game.exe --selftest nav` **94 条** + `tools/verify_nav.py` **66 条**（A~F）
  + `--selftest script` **C22 组 5 条**

---

## 一、为什么是"格子"而不是三角网 navmesh

三角网 navmesh（Recast 那一路）要先 **体素化 → 区域划分 → 轮廓提取 →
多边形三角化**，四步每一步都是几千行，而且每一步的中间结果都要配一套
自己的可视化工具才敢改。这个引擎的物理世界本来就有"向下打一条射线，拿到
地面高度与法线"（`PhysicsWorld::raycast`），而关卡是**编辑器里手搭的、
格子友好的**（大世界 / 室内 / 棋盘）。

所以第一版取格子法：**一个格子 = 一个可行走采样点**。代价是斜向贴墙有
一格左右的抖动；换来的是：

| 好处 | 具体是什么 |
|---|---|
| 可整份存档 | `std::vector<uint8_t>` + `std::vector<float>`，直接写 JSON |
| 可无设备自检 | A\* / 视线 / 吸附全是**纯函数** → `--selftest nav` 不要 GPU、不要窗口，0.8 秒跑完 94 条 |
| 可人眼排查 | `nav.ascii` 打一张 ASCII 图，一眼看出"这堵墙到底挡没挡住" |

坐标约定：世界是**右手系 Z-up**；格子铺在 XY 平面，每格存一个地面高度
（2.5D）。`origin` 是格子 (0,0) 的**角点**，格心 = `origin + ((c+0.5)·cell, (r+0.5)·cell, 0)`。

---

## 二、烘焙：一格是怎么被判定的（`bakeFromPhysics`）

对每个格子中心，从采样盒上沿打一条**向下**的射线（`rays == cells`）：

1. **没命中** → 这一格没有地面（坑 / 悬空）→ `noGround++`，写 NaN。
2. **法线太陡**（`n.z < cos(maxSlopeDeg)`）→ 站不住 → `tooSteep++`。
   判据与 `CharacterController` **完全一致**（都是法线 z 分量），否则会出现
   "导航说能走、角色站不住"的缝隙。
3. **代理体积被挡** → 在命中点上方 `max(radius+0.05, height/2)` 处做一次
   `overlapSphere(radius)`，有任何刚体 → `tooTight++`。
   这是**近似**（球而不是胶囊扫掠），方向是保守的：离墙 0.35 m 内的格子直接判死。
4. 都通过 → `walk`，高度 = 命中点 z。

⚠⚠ **只有 `RigidBodyComponent` 才会建物理体**。只挂 `CollisionComponent`
（"形状从哪来"）而不挂刚体时，`buildSceneWorld` 的遍历根本不会走到它 ——
症状是 `bodies=1`（只有无限地面）、`walk=9216`（96×96 全可走），而烘焙
**报成功**，因为确实采到了地面。踩过一次，`verify_nav` 现在有一条断言
专门钉 `walk < cells`。

⚠ 场景关掉物理（`physics.enabled=false`）时烘焙出一张全 Blocked 的空网，
`stats.walk == 0`。调用方必须**判这个数**并给提示，否则症状是
"烘焙成功但所有代理一动不动"，日志一行错都没有。

---

## 三、A\*（`findPath`）的六条规矩

每一条都对应一种"看起来能用但很怪"的路径：

| # | 规矩 | 不这么做会怎样 |
|---|---|---|
| ① | **不许斜向切角**：斜迈一步时两个正交邻格也要可走 | 路径从两堵墙的对角缝里"穿"过去，走起来是撞墙角 |
| ② | **高差 > `maxStep` 的边不通**（格本身仍可走） | "每个台阶都变成一堵墙"或"能走上垂直墙面" |
| ③ | 队列用 `(f, index)` **全序**，不是只比 f | 同 f 节点按堆内部顺序展开 → 同一张图两次跑出两条不同路径，用例偶发红 |
| ④ | 起点/终点先**吸附**到最近可行走格（半径 `2·cell`） | 点到墙上就"走不了"，而玩家/脚本给的点经常差半格 |
| ⑤ | 终点不可达时**走到离终点最近的那个已到达点**，`reached=false` | 直接返回空路径 → 调用方分不清"没路"和"没网格" |
| ⑥ | 首尾两点尽量用**调用方给的精确坐标** | 玩家看到角色停在离目标 25 cm 的地方 |

`length` 是折线长度（含 z 差）。**它是"绕过去了"的唯一判据** ——
"直接穿墙"和"绕过去"都能返回"一条路径"。

### 视线与平滑

`lineWalkable` 沿线段按 `cell/2` 采样，每格必须可走、相邻采样高差 ≤ `maxStep`。
它不是碰撞（没有胶囊扫掠），只回答"这条线会不会跨过墙/坑"。

`smoothPath` 是贪心视线拉直：从最远往回找第一个能直连的点，折成几段长直线。
**平滑只可能让路径变短**（`C5.not_longer` 那条断言）—— 变长说明视线判定比
A\* 还严，那种情况下平滑出来的路径比不平滑更难看。

---

## 四、代理怎么动（`NavSystem::tick`）

`ecs::NavAgentComponent` 刻意做得**极窄**：它只负责"朝目标走"。

```cpp
struct NavAgentComponent {
    float speed, turnSpeed, arriveDist, repathInterval;   // 运动
    bool enabled, faceMoveDir, smooth, autoStart;         // 行为
    glm::vec3 goal; bool hasGoal;                         // 数据（进场景文件）
    // 运行态（**不**序列化）
    std::vector<glm::vec3> path; std::size_t next;
    float repathTimer; bool reached, failed; int lastExpanded;
};
```

两条移动路：

| 实体有 | 位移由谁做 | 引擎写什么 |
|---|---|---|
| `CharacterControllerComponent` | 物理那套（台阶 / 斜坡 / 贴地） | 只写**期望速度** `cc.velocity.xy` |
| 没有 | 自己 | 直接写 `TransformComponent.position`，z 贴网格 |

⚠ **`tick` 必须排在物理之前**（`RuntimeApp` 里紧跟在 `tickTransformClips` 之后）：

- 无胶囊的代理写 Transform → 物理的 `syncKinematicFromScene` 要读它；
- 有胶囊的代理写期望速度 → 物理这一步才积分。

排在物理之后 = 代理永远晚一帧，追赶玩家时能看到"跟着自己的影子走"。

⚠ `dt <= 0` 直接返回 0：暂停时不该有人偷偷走动。

⚠ `repathInterval`（默认 0.5 s）**不是性能优化，是防抖**：目标每帧微动
（追玩家）时不设下限会每帧跑一次 A\*，几十个代理就是每帧几毫秒的纯浪费。

### 序列化

`navAgentToJson` 只写**非默认**字段；`path` / `next` / `repathTimer` /
`reached` / `failed` / `lastExpanded` **一律不写** —— 存下来会让"读档后沿着
上一次的路径走"，而那时世界已经变了。`hasGoal` 由 `goal` 是否存在推出，
不单独存一个容易对不上的 bool。

读端会把负数夹回 0（`speed = -2` 的后果是"永远到不了点"，而日志里一切正常）。

---

## 五、网格资产 `.vnav.json`

**为什么是资产而不是每次进场景现场烘**：

- 烘焙要射线，射线要物理世界，而物理的开关 / 地面参数都是场景设置 ——
  现场烘的话"作者机器上能过去的路"在玩家机器上可能不通，那种 bug 没法复现；
- 200×200 格的现场烘是几十毫秒，放在"换关卡 / 读档"那一下就是卡顿尖刺。

格式（`version` 1）：位图用**字符串**（`'0'` / `'1'`），高度用数组（3 位小数）。

```json
{"version":1,"origin":[-24,-24,-60],"cell":0.5,"cols":96,"rows":96,"maxStep":0.4,
 "cells":"1111…0…","height":[0.0,0.0,…]}
```

⚠ 位图**不压 base64**：这个文件要能用文本编辑器打开、`git diff` 能看出哪一片
格子变了。

⚠ 读端只认 `'1'` 为可走。写坏了（`'2'` / `' '` / `'\0'`）一律 Blocked ——
反过来（"非 '0' 即可走"）会把"位图被截断"变成"整张图乱走"。
⚠ 位图长度必须等于 `cols*rows`，**不匹配就拒绝整份资产**（否则"第 i 格"不再
有意义，网格看起来正常、路径却乱穿墙）。

### 运行时的优先级（`RuntimeApp::setupNav`）

```
assets/nav/<场景名>.vnav.json  →  读到了就用（GAME-NAV source=asset）
        ↓ 没有 / 坏了（WARN）
现场烘（以玩家为中心 48×48 m，MYVK_NAV_CELL 可调格子大小）
        ↓ 一格都站不住
GAME-NAV source=none → 代理全体待机（**不是错误**，但必须说出来）
```

⚠ 命名约定与场景文件同名：`scenes/main.json` → `nav/main.vnav.json`。
⚠ 资产坏了会**回落到现场烘**，而不是留一张空网 —— 坏文件不该让整关 NPC
集体静止。

---

## 六、面板与命令

`Authoring ▸ Navigation`（浮动窗口，默认关，无快捷键）。

| 段 | 控件 | 锚点 |
|---|---|---|
| 网格参数 | 采样范围 min/max、格子尺寸、代理半径/高度、最大坡度、最大台阶 | `NAV-BOUNDS-MIN` … `NAV-STEP` |
| 烘焙 | `Bake` / `Clear` / `Print grid` | `NAV-BAKE` `NAV-CLEAR` `NAV-ASCII` |
| 资产 | 路径输入 + `Save grid` / `Load grid` | `NAV-GRID-PATH` `NAV-GRID-SAVE` `NAV-GRID-LOAD` |
| 路径测试 | from / to + `Find path` / `From selection` | `NAV-FROM` `NAV-TO` `NAV-PATH-TEST` `NAV-USE-SELECTION` |
| 代理（选中项） | `Attach agent` / `Stop` + speed / arrive / repath / 三个勾 | `NAV-AGENT-*` |

命令（与按钮走**同一个**函数 —— 分叉过就会变成"能点不能脚本化"）：

| 命令 | 作用 |
|---|---|
| `nav.bake` | 用面板参数烘一张网格 |
| `nav.state` | `grid=0/1 cols=… rows=… walk=… agents=…` |
| `nav.ascii` | 打 ASCII 图（Y 向上） |
| `nav.clear` | 丢掉网格 |
| `nav.path <x1> <y1> <x2> <y2>` | 求一条路径（不需要代理） |
| `nav.agent [x y z]` | 给选中实体挂代理并设目标 |
| `nav.stop` | 清选中实体的目标 |
| `nav.tick [n]` | **编辑态手动推进**代理 n 次（每次 0.05 s） |
| `nav.save <rel>` / `nav.load <rel>` | 网格资产存 / 取（相对 `assets/`） |

⚠ `nav.tick` 存在的理由：编辑器不跑 Play 就没有主循环，代理永远不会动 ——
而"挂上代理之后它到底会不会走过去"是这一步唯一的验收方式。

⚠ 编辑器烘网格用的是**当前编辑场景临时建的物理世界**（`EditorApp::bakeNav`
里 `buildSceneWorld(..., enabledOverride=1)`），不是 `m_physics`：后者只在 Play
里存在，而"烘一张网格看看形状"恰恰是编辑时最需要的事。强制开物理是刻意的 ——
作者常常还没勾"启用物理"，而烘焙只读几何、不推进模拟。

### 6.1 脚本动词（C8-2，2026-11-05）

上一段那些命令都是**手动验证**用的。让 NPC 真的按剧情走，靠的是四个脚本
动词（`.vks`，全部作用于 **self**）：

| 动词 | 返回 | 语义 |
|---|---|---|
| `navGoto(x, y, z)` | `1` = 受理 | 记下目标并把 `repathTimer` 归零（下一帧强制求路） |
| `navArrived()` | `1` = 到点 | `NavAgentComponent::reached` |
| `navFailed()` | `1` = 走不通 | `NavAgentComponent::failed`（不可达 / 没网格） |
| `navStop()` | 恒 `1` | 清目标与路径（有没有代理组件都算成功） |

一段能跑的巡逻（`tools/verify_nav.py` F 组用的就是这一份）：

```vks
let done = 0

on start {
    print("NAVPROBE goto", navGoto(6.0, 0.0, 0.0))
}

on update {
    if (done == 0) {
        if (navArrived()) {
            done = 1
            print("NAVPROBE arrived", pos(self()))
        }
        if (navFailed()) {
            done = 1
            print("NAVPROBE failed")
        }
    }
}
```

四条纪律：

1. **`navGoto` 返回的是"受理"，不是"走到了"**。求路发生在下一帧的
   `NavSystem::tick` 里（`repathInterval` 那条防抖链），当场返回"到了"
   是不可能的。
2. **`navArrived()` 必须和 `navFailed()` 成对用**。目标在墙外 / 网格外时
   `arrived` 永远是 0，`while (!navArrived()) wait(0.1)` 会**永远转**
   —— 而且不报错（配额只管"每帧调用次数"，不管"永远不满足"）。
3. **`navGoto` 返回 0 不报错**。0 的含义是"受理失败（没有网格）"，那是
   **运行时状态**，脚本要用 `if (navGoto(...))` 自己决定；报错会让整份脚本
   not runnable，而"这一关没烘网格"不是脚本的错。
4. **只有 self**。"一个导演实体指挥一堆 NPC"现在做不到 —— `kBuiltins[]`
   的 arity 表达不了"第 0 个参数换类型"，而做一个"传错了不报错"的重载
   比不做更糟。当前的解法是把脚本挂在 NPC 自己身上。

⚠ 三个宿主差别：`Game.exe` 与编辑器 Play 都接了这四个（各自的
`installScriptServices`），**编辑器 Play 还会每帧 `m_nav.tick()`**（排在脚本
之后、物理之前）。编辑态（非 Play）不 tick —— 推进代理会改编辑场景的
`Transform` 并进 undo 栈，作者只想看网格时不该被巡逻兵挪场景。编辑态要验
走路请用 `nav.tick` 命令。

---

## 七、第一版**没做**的事（记账，别当成 bug）

| 没做 | 后果 | 为什么先不做 |
|---|---|---|
| 多层 / 桥下 / 楼上同一 XY | 走不了两层 | 2.5D 的代价；真要做得换体素或分层网格 |
| 局部避障（绕开别的角色） | NPC 之间会互相穿过去 | 与全局寻路是两件事，且需要 steering |
| 动态障碍（门 / 可推动的箱子） | 门关了它照样穿 | 需要"运行时改格子"的通道，先有门再说 |
| 跳跃 / 落差 > `maxStep` 的连接 | 台阶太高就绕远路 | 要 edge link（跳点），格子法表达不了 |
| 烘焙分块（大世界流式） | 整张图一次烘完 | 依赖 G19 流送的分块语义，先把单图做对 |
| 曲线 / 样条路径 | 折线走起来有棱角 | 交给转向插值（`turnSpeed`）掩盖，够用 |

> ~~脚本动词~~ **已做（C8-2，2026-11-05）** → 见 §6.1。原计划把它和"能走"
> 分开交付，理由仍然成立：那一版不需要脚本就能验完全部内核。

---

## 八、判据

### `Game.exe --selftest nav`（94 条，纯 CPU，0.8 s）

| 组 | 覆盖 |
|---|---|
| A 网格 | 尺寸 / 原点 / `cellOf(centerOf)` 往返 / 越界 / NaN 与"低于底面"的哨兵 / 尺寸不符拒绝 |
| B A\* | 直线长度 = 距离 / 绕墙变长 / 密封房不可达（`reached=0` 但有路径）/ 不切角 / 高台阻断 / 两次逐点相同 / 终点吸附 / 无网格拒绝 |
| C 视线 | 穿墙为假 / 空地真 / 平滑后点更少 / 首尾不丢 / **长度不变长** |
| D 吸附 | 墙里吸到最近格（`x=4`，同距离时取字典序小者）/ 半径不够返回 false |
| E 代理 | 无网格 → `failed>0` 且不动 / `dt=0` 不动 / 到点 / 贴网格高度 / 朝向走 **90° 而不是 270°** / 停 / 有胶囊时只写速度、位置不动 |
| F 存盘 | 默认不写键 / 全字段往返 / **运行态读回来是空的** / 负数夹回 / 非 object 拒绝 |
| G ASCII | 行数 = rows、`.` 与 `#` 都在、缺口在最后一行 |

### `tools/verify_nav.py`（66 条）

| 组 | 断言什么 |
|---|---|
| A | 自检 94 条 + 点名 8 条（`B4.no_cut` / `B5.detoured` / `B6.same_path_diff` / …） |
| B | 命令链：`nav.state` 四次（烘前 grid=0 / 烘后 grid=1 / 清空后 grid=0）、`nav.bake` 的 `grid=96x96` 与 **`walk < cells`**（墙真的挡住了）、`nav.path` 的 `len > 12`（绕墙）、网格外 `ok=0`、`nav.save` 落盘、`nav.load` 成功与失败各一次、`nav.agent` / `nav.tick moving>0` / `nav.stop` |
| C | 13 个面板锚点都有矩形 + 网格参数在代理段**上面**（顺序一变，按 rect 点控件的用例会静默漂移） |
| D | **真跑 `Game.exe`**：`GAME-NAV source=asset`、`GAME-NAV tick agents=1 moving>0`、零 `[ERROR]` |
| E | 资产 `cols/rows/cell`、位图与高度长度都 = `cols*rows`、位图只含 0/1、两种格都有；**故意把位图截短一格 → 必须被拒且说清原因**；读失败后**旧网格还在** |
| F | **真跑 `Game.exe` 跑一份 `.vks`**：`source=bake`（没有资产时现场烘）· 脚本没有 `not runnable` · `NAVPROBE goto 1`（受理）· 没有 `is not available` · `NAVPROBE arrived` 且**到达点 x > 3**（真的过墙）· 没有 `failed` · 零 `[ERROR]` |

⚠ D 组必须真跑游戏：`--selftest nav` 用的是内存里造的网格，证明不了
"运行时把资产 / 烘焙接上了"—— 而"接没接上"恰恰是这类功能最常见的缺口。

⚠ F 组断言的是**到达点位置**，不只是那行日志：`navArrived()` 恒返回 1 的
实现也能让 `NAVPROBE arrived` 出现（在第 0 帧）。位置才是"真走过去了"。

### `--selftest script` 的 C22 组（5 条）

求值器侧的那一半：`navGoto` 三个坐标原样到宿主、walking 时 `arrived` 与
`failed` **都是 0**、`arrived` / `failed` 能各自翻上去（两个标志不是同一个量）、
**没有网格时返回 0 且不报错**（目标仍记账）、宿主没接时**必须报错**说清是
宿主缺能力。

⚠ C22 与 F 组不能互相替代：C22 的宿主是 MockHost（记录器），它证明"求值器 →
宿主动作"接线对；F 组证明"运行时把动词接到 `m_nav`、网格烘出来了、人真走到了"。

---

## 九、踩过的坑（都付过一次代价）

1. **只挂 `CollisionComponent` 不会建物理体**。物理世界按 `RigidBodyComponent`
   遍历，碰撞组件只是"形状从哪来"。症状：`bodies=1`、`walk=9216`、烘焙报成功。
2. **`walk == cells` 不是"烘焙成功"，是"墙没进来"**。所以判据要断言
   `walk < cells`，不能只断言 `walk > 0`。
3. **`nlohmann::json` 没有 `reserve()`**（只有底层 `array_t` 有）。写进
   `NavSerialize.cpp` 是 C2039，而同一个调用在别的 TU 里编译得过。
4. **`ofstream` 不会替你建目录**。第一次 `nav.save` 到 `assets/nav/` 报
   `save=0 reason=open-failed`，看着像权限问题 —— 其实目录不存在。
5. **多行日志会把"等某一行"的等待条件骗过去**。`nav.ascii` 一次打 96 行；
   自动化要等**最后一条**命令的锚点再快照，不能只等第一个 `nav.state`。
6. **`setupNav()` 必须排在玩家实体建好之后、`PlayerController::attach` 之前**
   —— attach 要读物理世界的 heightfield，顺序不能倒；而兜底范围要以玩家为中心，
   所以只能夹在中间。
7. **判据读"长度"而不是"有没有路径"**。绕过去与直接穿墙都会返回一条路径，
   只有长度能分开它们。
8. **编辑器 Play 以前根本没 tick 导航**（C8-2 才发现）。四个动词接得再对，
   没人调 `NavSystem::tick` 也是一动不动 —— 而症状是"`navGoto` 返回 1、
   日志一切正常、NPC 站着"。现在编辑器的 Play 循环里排了
   `m_nav.tick()`（脚本之后、物理之前）。

---

## 十、下一步（推荐顺序）

1. ~~**脚本动词** `navGoto` / `navArrived` / `navStop`~~ ✅ **2026-11-05 已做**
   （多了一个 `navFailed`，见 §6.1）。原话是"三件套照旧：`kBuiltins[]` +
   `ScriptServices` + 宿主两侧接线"—— 实际做下来只有一处意外：
   **编辑器 Play 以前根本不 tick 导航**，动词接上了人也不动。
2. **局部避障**（半径排斥 + 前方射线）—— 现在 NPC 会互相穿过。
3. **动态障碍**：`NavSystem::setCellBlocked(世界点, bool)`，供门 / 可推箱子调用。
4. **烘焙分块**：与 G19 流送对齐，大世界按 level 各存一张 `.vnav.json`。
5. **`navGoto(e, x, y, z)` 的显式实体重载** —— 一个导演实体指挥一堆 NPC。
   卡在 `kBuiltins[]` 的 arity 表达不了"第 0 个参数换类型"，要先想清楚
   怎么让"传错了"报错（比如另起一个名字 `navGotoTo(e, x, y, z)`）。
