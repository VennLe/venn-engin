# 游戏逻辑 API · 开发者指南

> 目标读者：**写玩法的人**（不是改引擎的人）。
> 这份文件讲**怎么用、为什么这么设计、哪里会咬人**；
> 106 个函数的完整签名/返回值速查见 **`docs/script_api_reference.md`**（自动生成）。
>
> 设计取舍见 `docs/script_framework.md`，C++/Lua 分工见 `docs/lua_cpp_split.md`。

---

## 1 · 三十秒上手

在实体上挂 `ScriptComponent`，`path` 指向一个 `.lua`，内容长这样：

```lua
-- assets/scripts/patrol.lua
local M = {}
local speed = 2.0          -- 每个实例各自一份（local 是 chunk 级，见 §3）

function M:BeginPlay()
    self.t = 0.0           -- self 就是"这个实体的脚本实例"
    self.dir = 1
end

function M:Tick(dt)
    self.t = self.t + dt
    local p = world.pos(self.entity)
    if p.x > 4.0 then self.dir = -1 end
    if p.x < -4.0 then self.dir = 1 end
    world.setPos(self.entity, engine.vec3(p.x + self.dir * speed * dt, p.y, p.z))
end

return M
```

三条硬规则：

| 规则 | 为什么 |
|---|---|
| **必须 `return` 一张表** | 那张表就是这个脚本的方法集；不返回 = 编译失败，报 `script must return a table` |
| **方法用 `:` 定义（`function M:Tick(dt)`）** | `:` 才有隐式 `self`。写成 `function M.Tick(dt)` 会把 `dt` 当 `self`，症状是"位置乱飞" |
| **改状态写 `self.x`，不要写裸变量** | 见 §3，裸变量是"这份脚本共享一份"，不是每个实例一份 |

---

## 2 · 生命周期与钩子

| 钩子 | 何时调 | 参数 |
|---|---|---|
| `BeginPlay()` | 实体进入运行态时**一次** | 无 |
| `Tick(dt)` | 每个固定步一次 | `dt` 秒 |
| `OnCollide(other)` | 发生硬碰撞 | `other` 实体句柄 |
| `OnTrigger(other)` | 进入触发体积 | `other` |
| `OnExit(other)` | 离开触发体积 | `other` |
| `EndPlay()` | 实体销毁 / 退出运行态 | 无 |

- 钩子**可以不存在**，缺哪个就跳过哪个（不报错）。
- ⚠ **编辑态不跑脚本**。只在 Play / `Game.exe` 里跑 —— 预览态的时间轴是另一条路。
- ⚠ `Tick` 一帧内**不会打断重入**：脚本没有多线程，`world.*` 系列都是同步的。

---

## 3 · 状态放在哪（最容易搞错的一节）

一张对照表，**必须记住**：

| 写法 | 作用域 | 存盘 | 用途 |
|---|---|---|---|
| `self.hp` | **每个实体实例一份** | 不（重进 Play 归零） | 血、计时器、AI 阶段 —— 99% 的玩法状态 |
| `local speed`（chunk 顶层） | **同一份脚本共享**（所有实例） | 不 | 脚本级常量 |
| `局部变量`（函数体内） | **一次调用内**，`Tick` 结束就没了 | 不 | 临时中间量 |
| 裸赋值 `hp = 10`（不在 `let` 里） | **同一份脚本共享**（相当于 C# 的 `static`） | 不 | 极少用；见下面的 ⚠ |
| `game.attr(e, k, v)` / `game.setAttr` | 挂在**实体**上（`ScriptVarsComponent`） | 进场景文件 | 要跨 Play、要让别的脚本读到 |
| `game.worldAttr(k, v)` | **场景级**，所有脚本共享 | 进场景文件 | 波次号、全局计数器 |

```lua
function M:Tick(dt)
    self.t = (self.t or 0) + dt    -- ✅ 每个实体各算各的
    -- hp = 10                     -- ⚠ 所有实例共享一份，两个敌人会互相踩
end
```

> ⚠ **裸赋值写的是"这份脚本的表"，不是全局表。**
> 引擎给每个脚本文件建了一张 `{__index = _G}` 的表当它的 `_ENV`：
> 读 `engine` / `world` 照样穿透到全局，写只落在自己的表上。
> 这样即使脚本写 `world = nil` 也**抹不掉** API 本身，其它脚本不受影响。
> 代价是：同一份脚本的多个实例共享这张表 —— 所以裸赋值 ≈ static。

---

## 4 · 两种值：实体句柄与 vec3

**实体（entity）= 一个 64 位整数**，不是对象。

```lua
local e = engine.self()          -- 整数
if engine.valid(e) then          -- 句柄还活着？
    world.setPos(e, engine.vec3(1, 0, 0))
end
```

- 打包的是 `(generation << 32) | id`，**再 +1**。`generation` 那一半
  **不能丢** —— 丢了就是"实体被删后句柄复用，脚本静默改到别人身上"。
- **空句柄就是 `0`**，而且只有它一个：`world.find` 没找到、`world.parent`
  碰到根节点、`game.jointTarget` 没有另一端，一律返回 `0`。
  所以 `if e == 0 then ... end` 成立，`setParent(child, 0)` = 拔到根。
  （`0` 之所以能当空，是因为打包时加了 1 —— 否则"0 号实体"会和"空"撞车，
  而 0 号实体几乎总是存在，`setParent(child, 0)` 就会静默挂到它身上。）
- 句柄**失效时不会崩**：`engine.valid(e)` 返回 `false`；
  但 `world.pos(e)` 这种**会报错并让实例熔断**（见 §6）——
  静默返回 0 会让"东西被搬到原点"离现场太远。
- 句柄只在**本次运行**里有意义：它不进存档、不进场景文件，别往 `game.attr`
  里存了下次读出来用。

**vec3 = 一张表 `{x=, y=, z=}`**，不是三元组：

```lua
local p = world.pos(e)
world.setPos(e, engine.vec3(p.x, p.y + 1.0, p.z))   -- 构造走 engine.vec3
local d = engine.dist(p, engine.vec3(0, 0, 0))       -- 距离
```

- 用表而不是三返回值，是因为**读 `p.x` 顺手、写回去也不用重新拼**。
- 热路径想省一次建表，就尽量少中间 vec3（见 §11）。

⚠ **角度一律弧度**。`world.rot` / `world.setRot` / `world.rotY` 都是弧度。
`.vks` 的 `rotate(...)` 是**角度制**，迁移时这是最常漏的一处（见 §12）。

---

## 5 · 时间与协程

```lua
function M:Tick(dt)
    engine.dt      -- 本步的秒数（字段，不是函数）
    engine.time    -- 这个实例从 BeginPlay 起累计的秒数（自己累加，不受倍速跳变影响）
    engine.frame   -- 这个实例被 tick 过多少步（整数，严格 +1）
end
```

**等待**（会让 `Tick` 中途挂起，下一帧从挂起处继续）：

```lua
function M:Tick(dt)
    ui.popup(10)
    engine.wait(0.5)          -- 半秒后再往下走
    ui.popup(20)              -- 这行在 0.5 秒后才执行
end
```

| 函数 | 语义 |
|---|---|
| `engine.wait(sec)` | 等 `sec` 秒。恢复时返回**剩余时间**（走向这里就说明等完了，恒 0） |
| `engine.waitFrames(n)` | 等 `n` 帧（与帧率无关） |
| `engine.waitUntil(fn)` | 每帧调一次 `fn()`，返回真才继续 |

⚠ 三条边界：

1. **`wait` 只能在钩子体的顶层**（或者它调用的函数里）。写在
   `pcall` 包裹的代码里会报 `attempt to yield across a C-call boundary`。
2. `wait(0)` 合法：让出这一帧，下帧继续 —— 用来把一个长循环摊到多帧。
3. `waitUntil` 的条件函数**不能**再调 `wait`；它在 C++ 侧每帧被 `pcall` 一次。

```lua
-- 正确的"等敌人靠近再开火"
function M:Tick(dt)
    engine.waitUntil(function()
        local p = world.player()
        return engine.valid(p) and
               engine.dist(world.pos(p), world.pos(self.entity)) < 6.0
    end)
    audio.play("audio/shoot.wav")
end
```

---

## 6 · 错误、熔断、确定性

**三类失败**，面板上语义不同：

| 类别 | 什么时候 | 后果 |
|---|---|---|
| 编译错 | `return` 的不是表 / 语法错 / **顶层死循环** | 实例不创建，面板显示原文（带 `文件:行`） |
| 运行期错 | 绑定的 `luaL_error`（如实体已死、没有刚体、宿主动作没装、**参数个数不对**） | 该实例 `Failed`，**后续帧不再执行**，直到热重载成功 |
| 死循环 | 单次 resume（或 `waitUntil` 条件函数、顶层 chunk）超过指令预算 | 同"运行期错"，报 `budget exceeded (20000 steps)` |

- 预算默认 **20000 条 VM 指令 / 每次 resume**，环境变量 `MYVK_LUA_BUDGET` 可调。
- **参数个数是硬校验**：每个函数都按 API 表里写的 `min..max` 核一遍，
  多给少给都当场报错（`world.setPos: expects 2 argument(s), got 1`），
  **不会**静默忽略多余参数 —— 少给的那个参数在实现里只会变成 `nil/0`，
  症状离现场太远。
- ⚠ **熔断是"停一个实例"，不是停全场** —— 这是刻意保住的纪律。
- ⚠ **报错就停**，没有"忽略错误继续跑"。所以**别把可能失败的东西放在关键路径开头**：

```lua
-- ✅ 先判后调
local e = world.find("Boss")
if engine.exists(e) then world.setPos(e, engine.vec3(0, 0, 0)) end
```

**确定性**：`math.random` / `math.randomseed` **已被摘掉**（`os` / `io` / `debug` /
`package` 整库不开）。要随机请走宿主侧的玩法系统（`game.*`），否则回放会漂。

---

## 7 · 热重载

保存 `.lua` 后引擎重新编译，并**换掉实例的 `__index` 指向**：

- ✅ 方法体立刻是新的；
- ✅ `self.hp` 这些字段**一个不动**；
- ❌ 正在 `wait` 里的那个协程**仍然跑旧代码**（Lua 的 pc 指向旧函数体，跨 chunk 迁不了），
  要等它自己结束或 `Reset`。

改**结构**（增删字段、改钩子名）请 `Reset` 实例，别指望热重载接住。

---

## 8 · 十二个命名空间导读

完整签名见 `script_api_reference.md`。这里只讲**语义上容易踩的地方**。

### `engine` — 自身、值、时间、日志
`self()` / `valid()` / `name()` / `vec3()` / `dist()` / `clamp()` / `lerp()` /
`log()` / `warn()` / `wait*()` + 字段 `dt` / `time` / `frame`。

### `world` — 场景查询与变换
- `pos/setPos` 是**世界坐标**（父节点已折算，写回时内部反解局部矩阵）。
- `rot/setRot/rotY` 是**弧度**。
- `near(center, r [, limit])` 返回**按距离升序**的实体数组（1 基下标）。
  ⚠ 它是全场景线性扫描，**不要每帧对上千实体反复调**；常用的写法是
  "谁近就打谁"这种一帧一次。
- `find(name)` 找不到返回**无效句柄**（不报错）—— 配 `engine.exists` 用。
- `spawn(path, pos)` 返回**真句柄**，当场就能摆弄。
  ⚠ 刷出来的实体**不进物理世界**（场景物理只在启动/换场景时构建一次）。
- `despawn(e)` 会**连子节点一起销毁**，并且先把自己从父节点的 `children`
  里摘掉（否则那个 id 被复用后，别人的层级登记会跟着错）。
- `loadScene(path)` 的语义是"**已受理**，下一帧才切"：`true` 只代表路径存在。

**命名与层级**（`world.setName` / `parent` / `children` / `setParent`）：

```lua
local sword = world.spawn("props/sword.vmodel", world.pos(hand))
world.setParent(sword, hand)      -- 之后 sword 跟着 hand 走
for i, c in ipairs(world.children(hand)) do
    engine.log(engine.name(c))    -- 取名字是 engine.name
end
```

- ⚠ `setParent` **只改局部变换**：世界位置会跟着父节点跳。要"原地换父"：
  先 `local p = world.pos(e)`，`setParent` 完再 `world.setPos(e, p)`。
- ⚠ 传**空句柄**（`nil` 或 `0`）= 拔到根；成环（把自己的父设成自己的子）→ **报错**；
  传一个**已销毁**的句柄 → 也报错（不是静默当空：那会把"怪物被删了还去追它"
  变成"追到空气"）。
- ⚠ `world.setName` 改的是**两处**（`World` 记录 + `NameComponent`），
  走 `world.setName` 这个名字才会立刻被 `world.find` / `engine.name` 看到。
  直接改别的路径不在脚本能力内 —— 这不是限制，是提示你别去找"另一条"。

### `actor` — 可见 / 刚体 / 材质
- `vel` / `setVel` 对**没有刚体**的实体**报错**（不返回 0）。
- `setVisible(e [, b])` 两种写法都行：`setVisible(e, false)` 和 `setVisible(false)`（对 self）。
- `setEmissive` 改的是**按名共享的材质资产** → 会影响所有用同一材质的实体。
- `color(e)` / `setColor(e, r, g, b)` 同理（材质底色 = `baseColorFactor.rgb`）。
  **受击闪白**这种效果必须先确认该实体用的是**独占材质**，否则全场一起白：
  ```lua
  local c = actor.color(me)                 -- 记下原色
  actor.setColor(me, 1, 1, 1)               -- 闪白
  engine.wait(0.06)
  actor.setColor(me, c.x, c.y, c.z)         -- 回来
  ```
- ⚠ alpha 不在这个 API 里（只承诺 RGB）：`Opaque` 材质下改 alpha 是看不见的，
  要么在材质面板把 `alphaMode` 改掉，要么就别指望它。

### `input` — 动作名，不是键码
`pressed/held/released/axis` 收的是 `InputMap` 里的**动作名**（`"Jump"`）。
⚠ 写 `pressed(32)` 是错误的抽象层级 —— 换键位就全废。

### `anim` — 播放、控制、形态键、表情

**`play(e, "Run")`**：骨架里真有这段才返回 `true`，**别当它一定成功**。

**播放控制**（`pause` / `resume` / `stop` / `speed` / `time` / `clip`）：

```lua
anim.play(me, "Attack")
anim.speed(me, 0.35)          -- 慢放（子弹时间）
engine.waitFrames(3)
anim.pause(me)                -- 定在受击帧
engine.wait(0.4)
anim.resume(me)
anim.time(me, 0.8)            -- 跳到第 0.8 秒（对齐演出）
```

- ⚠ 权威字段跟 `AnimatorSystem` 同一条规则：**有 `AnimatorComponent` 用它，
  否则用 `SkinnedMeshComponent`**。所以简单角色（只挂蒙皮）也能 `pause`。
- ⚠ `speed` 为负 = **倒放**（引擎的循环折回两个方向都处理了）。
- ⚠ `time` 跳帧会**同步事件游标**，否则中间那些动画事件会在下一帧一次性全炸
  （"跳帧跳过一段剧情，结果所有音效同时响"）。
- `stop` = 停 + 回 0 秒，**不是**"播完最后一段"。
- ⚠ 分层/NLA 的**逐层**控制、根运动模式、状态机参数（float/bool/trigger）
  **不在脚本 API 内** —— 见 §14。

**形态键**（`morph` / `morphWeight`）：通道名来自模型资产的 `MorphRig`，
拼错**报错**（不静默），权重钳到 0..1。
⚠ 被表情驱动（`ExpressionComponent`）接管的通道会被每帧覆盖 —— 那几条别用 `morph` 抢。

**表情**（`expression(e, "smile")`）：走的是编辑器面板同一个函数
（`anim::setExpressionPreset`），空串 = 回中性；失败打一行 WARN 并返回 `false`。

### `nav` — 导航
- `nav.moveTo(e, x, y, z)` → `true` = **已受理**（求路在下一帧），不是"走到了"。
- ⚠ **`nav.arrived` 必须和 `nav.failed` 成对轮询**，否则目标在墙外时永远是 0，
  `while not nav.arrived(e) do engine.wait(0.1) end` 会永远转。
- ⚠ 名字是 `moveTo` 不是 `goto` —— `goto` 是 Lua 5.4 的**保留字**，
  `nav.goto(...)` 连语法都过不了。

### `fx` — 特效与镜头
- `fx.play(path)` 在 **self 的位置**播；`fx.play(path, x, y, z)` 在指定点播。
- 返回实例 id（`0` = 没播：资产缺失或池满），可交给 `fx.stop(id)`。
- `fx.shake(amount)` trauma 0..1，按与镜头的距离自动衰减。
- `fx.timeScale(0.3)` = 子弹时间；`<0` 拒收。

### `audio` — `play` / `stopAll` / `bus`
路径相对 `assets/`。总线名是 `"music"` / `"sfx"` / `"voice"`。

### `light` — 颜色 / 强度 / 范围 / 方向 / 开关
`color` / `intensity` 三种灯都支持（**平行光也算**）；`range` 只对点光/聚光；
`direction` 对聚光与平行光有效（点光调用是空操作）。
实体没有灯 → **报错**（不静默）。

`enabled(e [, b])` 只有**点光/聚光**有这个开关；平行光没有 → 报错，
要关太阳请用 `light.intensity(e, 0)`（不替它假装有开关：
写一个不存在的字段等于让你以为关掉了，而画面还亮着）。

### `camera` — FOV / 臂长 / 机位开关
- `fov(e [, deg])`：**单位是度**（相机面板本来就用度），只接受 `(1, 179)`。
  ⚠ 相机要是装了**定焦镜头**（`CameraComponent::focal > 0`），视场角由焦距
  算出来、会**覆盖** `fov`。所以这里**写 fov 会把 focal 清零**（否则画面
  一动不动还不报错），**读**回来的永远是**实际生效**的视场角。
- `distance(e [, m])`：第三人称臂长。
- `enabled(e [, b])`：多机位切换用。渲染时 `enabled` 是硬过滤、
  `priority` 定胜负（同分取实体 id 小的，确定性）。

```lua
-- 冲刺拉 FOV
local base = camera.fov(cam)
camera.fov(cam, base + 12)
engine.wait(0.25)
camera.fov(cam, base)
```

### `ui` — HUD / 对话 / 过场 / 界面
- `health(v)` 绝对值；`damage(v)` 扣血。
- `damage(v, "fire")` 走**玩法属性**（护甲 / 易伤 / 护盾），**返回真正扣掉的血**；
  宿主没接玩法系统时自动回退到 HUD 扣血。
- `popup(n)` 在 **self 头顶**弹伤害数字。
- `dialogue(id)` / `sequence(path)` / `open(path)` 都返回 `bool`，**失败要当真**。
  `open("")` = 关掉当前那块界面。

### `game` — 玩法数据
这是最容易混的一组，**四个"看起来像"的东西**：

| 想干什么 | 用哪个 |
|---|---|
| 存一个跨 Play 的实体数值 | `game.attr(e, k [, default])` / `game.setAttr(e, k, v)` |
| 读**玩法属性**（含状态加成） | `game.stat(e, name)`，先 `game.hasStat` 判存在 |
| 存一个场景级共享值 | `game.worldAttr(k [, default])` / `game.setWorldAttr(k, v)` |
| 记剧情旗标（进存档） | `game.flag(name [, value])` |

```lua
-- ⚠ `game.attr` 是**脚本变量**，不是玩法属性；两者千万别混
local hp   = game.attr(e, "hp", 100)   -- 第一次调用会写回 100
local atk  = game.stat(e, "atk")       -- 玩法属性（护甲/状态已算进去）
if not game.hasStat(e, "atk") then ... end   -- stat 返回 0 有歧义，用这个分清
```

- `game.damage(e, amount, type)` **对别人**造成伤害，返回真正扣掉的血。
  ⚠ 别和 `ui.damage` 混：`ui.damage` 打的是 **self**（HUD 血条），
  `game.damage` 打的是**你指定的实体**。前者没有目标参数，后者必须给。
- `game.round()` 回合末结算（dot），返回结算条数。
- `game.castCard(e, idx)` / `game.drawCards(e, n)`：手牌空 / 越界返回 **0，不报错**
  （"空手牌还按了一下"是正常用法）。
- `game.tableNum("cards.strike.cost")`：键是 `表.行.列`。
  ⚠ `tableNum` 返回 0 有三种含义（表没加载 / 行没有 / 值就是 0）→ 配 `tableHas`。
- 关节九件（`joint*`）全部**显式传 self**：脚本没有"选中"这个概念。

---

## 9 · 常用配方

**① 巡逻（到点折返）**

```lua
local M = {}
local A, B, SPEED = -5.0, 5.0, 2.0
function M:BeginPlay() self.target = B end
function M:Tick(dt)
    local me, p = self.entity, world.pos(self.entity)
    local d = self.target - p.x
    if math.abs(d) < 0.1 then self.target = (self.target == A) and B or A; return end
    world.setPos(me, engine.vec3(p.x + (d > 0 and 1 or -1) * SPEED * dt, p.y, p.z))
end
return M
```

**② 玩家靠近才动（省 CPU）**

```lua
function M:Tick(dt)
    local pl = world.player()
    if not engine.valid(pl) then return end
    if engine.dist(world.pos(pl), world.pos(self.entity)) > 20.0 then return end
    -- 近处才跑 AI
end
```

**③ 打到最近的敌人一次**

```lua
function M:Tick(dt)
    if not input.pressed("Fire") then return end
    local list = world.near(world.pos(self.entity), 8.0, 1)  -- 只要最近的
    if #list == 0 then return end
    local victim = list[1]
    local real = game.damage(victim, game.stat(self.entity, "atk"), "physical")
    ui.popup(real)
    fx.play("fx/hit", world.pos(victim).x, world.pos(victim).y, world.pos(victim).z)
end
```

> ⚠ 下面这条依赖**场景级脚本**（`self.entity` 为无效句柄）。
> `LuaBackend` 已经支持，但把"哪个实体挂哪份脚本"接起来的是
> `ScriptRuntime`，它目前只认 `.vks`（阶段 D 才按扩展名分派）。
> 现在写出来是为了对齐语义，**暂时还跑不起来**。

**④ 波次：场景级脚本（`self.entity == 0`）**

```lua
local M = {}
function M:BeginPlay() game.worldAttr("wave", 1) end
function M:Tick(dt)
    if world.count() > 1 then return end      -- 场上还有活口就等
    local w = game.worldAttr("wave") + 1
    game.setWorldAttr("wave", w)
    for i = 1, w do
        world.spawn("models/enemy.vmodel", engine.vec3(i * 2.0, 0, 0))
    end
end
return M
```

**⑤ 一段演出**

```lua
function M:BeginPlay()
    ui.sequence("sequences/opening.seq.json")
    engine.wait(2.0)
    ui.dialogue("ch01_open_01")
    engine.waitUntil(function() return ui_state_done end)  -- 用你自己的标志
end
```

---

## 10 · 调试

| 想干什么 | 怎么做 |
|---|---|
| 打一行日志 | `engine.log("hp", self.hp)` → `LUA-PRINT ...`；`engine.warn(...)` → 黄色 |
| 看变量 | 面板 / 控制台的 `script.vars`（读的就是 `self.*`） |
| 看实例状态 | `Idle / Running / Waiting / Failed` |
| 查 API 有没有接上 | `Game.exe --selftest lua` → 必须 `LUA-TOTAL pass=25 fail=0` |
| 查函数清单 | `Game.exe --selftest api` → 打整份参考文档 |
| 崩溃了 | 自带符号化：直接打出 `函数名+偏移 (文件:行)` + 20 帧调用栈 |
| 跑太快看不清 | `fx.timeScale(0.2)` 或 `fx.hitstop(0.2)` |

⚠ 自检是**无窗口、不需要 GPU** 的，改完绑定先跑它再开编辑器。

---

## 11 · 性能纪律

一次 Lua→C++ 调用约 **100~250 ns**。预算按"每帧 1 万次调用 ≈ 1~2 ms"估。

**真正的坑不是"Lua 慢"，是在 Lua 里写 N²**：

```lua
-- ❌ 每帧 O(N²)：一千个实体就是一百万次跨语言调用
for _, e in ipairs(all_entities()) do
    if engine.dist(world.pos(e), world.pos(self.entity)) < 5.0 then ... end
end

-- ✅ 一次调用，C++ 侧扫完
for _, e in ipairs(world.near(world.pos(self.entity), 5.0)) do ... end
```

其它三条：

1. **别在 `Tick` 里 `world.find("常驻对象")`** —— 名字查找是字符串比较，`BeginPlay` 里缓存句柄。
2. **`engine.dt` / `time` / `frame` 是字段**，读它们不产生调用；写成 `engine.dt()` 会报错。
3. `engine.wait(0)` 能把长循环摊到多帧，但**别用它拼一个每帧都重建的表**。

---

## 12 · 从 `.vks` 迁移

| `.vks` | Lua | 注意 |
|---|---|---|
| `move(x,y,z)` | `world.setPos(e, engine.vec3(x,y,z))` | `.vks` 的 `move` 是**对 self 直接写 position**（不反解父节点）；`setPos` 是世界坐标 |
| `rotate(a,b,c)` | `world.setRot(e, engine.vec3(r,r,r))` | ⚠ **角度 → 弧度**（`math.rad`） |
| `scaleBy(...)` | `world.setScale(e, ...)` | 没有"乘"的版本，自己算 |
| `pressed("Jump")` | `input.pressed("Jump")` | 语义相同 |
| `wait(1.0)` | `engine.wait(1.0)` | `.vks` 是语句，Lua 是函数（要 `()`） |
| `setVisible(false)` | `actor.setVisible(false)` | 对 self 的短签名保留 |
| `attr(e,"hp",100)` | `game.attr(e, "hp", 100)` | 写回默认值的行为一致 |
| `getAttr("atk")` | `game.stat(e, "atk")` | ⚠ 名字变了：`attr` 是脚本变量，`stat` 是玩法属性 |
| `navGoto(x,y,z)` | `nav.moveTo(e, x, y, z)` | ⚠ 要显式传实体 |
| `print(...)` | `engine.log(...)` | 日志前缀不同 |
| `count(list)` | `#list` | Lua 原生 |
| `at(list, i)` | `list[i + 1]` | ⚠ Lua 是 **1 基** |
| `damage(10,"fire")` | `ui.damage(10, "fire")` | 返回实际扣血 |

⚠ 三条最容易漏：**角度制 vs 弧度**、**0 基 vs 1 基下标**、**隐式 self vs 显式实体**。

---

## 13 · 加一条新 API（给改引擎的人）

**两处，不是四处。**

```cpp
// ① engine/script/ApiTable.cpp —— 加一行
{"world", "setScale", 2, 2, "entity, vec3 -> void",
 "设置缩放", ""},

// ② engine/script/LuaBackend.cpp —— kImpls 里加一行
{"world", "setScale", l_world_setScale},
```

漏了 ② → 下次启动日志里直接出现
`LUA-API missing implementation: world.setScale`，
不会变成"文档里有、脚本里调不到"。文档重跑
`Game.exe --selftest api > docs/script_api_reference.md` 即可。

⚠ **实参个数由表里那两个数字说了算**：注册时每条绑定都会套一层
`dispatch`，调用时按 `minArgs..maxArgs` 核一遍再进实现。
所以表里的数字写错 = 调用报"expects N argument(s)"，实现里读不到的参数
**不会**再变成 `nil/0`。改实现签名时，这两个数字**必须**一起改。

⚠ **取实体句柄一律用 `aHandle()`，不要用 `aInt()`**：句柄是 64 位
（`((gen<<32)|id)+1`），`int` 会把 generation 截掉 —— 而截断后的症状
（`engine.valid` 恒假、`setParent` 报 not alive）只有"槽位被复用"之后才出现。

⚠ **命名红线**：别用 Lua 保留字（`goto` / `end` / `function` / `nil` / `repeat` /
`then` / `until` / `while` / `in` / `local` / `not` / `and` / `or` / `if` / `else` /
`elseif` / `for` / `do` / `break` / `return` / `true` / `false`）。
`nav.goto` 就是这么翻车的：表里有、文档有、编译期直接语法错。

⚠ **错误串里别写 `%u`**：`luaL_error` 走的是 `lua_pushvfstring`，只认
`%d %f %p %s %c`，写 `%u` 会让**报错本身报错**（`invalid option '%u'`）。

⚠ **新开一个命名空间**：还要在 `LuaSelfTest.cpp` 的 `kNsInfo[]` 里补一行标题，
否则生成的参考文档里那一段标题是空的（不报错，只是难看）。

---

## 14 · 已知边界

| 边界 | 说明 |
|---|---|
| 没有 `pairs` 遍历实体 | 只有 `world.near` / `world.find` / `world.count`。要遍历请先在 C++ 侧开口 |
| 没有 userdata | 实体是整数、vec3 是表 —— 也没有"对象方法"那种写法 |
| 没有调试器 | 无断点/单步（`.vks` 有，Lua 侧还没接 `ldb`） |
| 不存盘 `self.*` | 要跨 Play 的数值写 `game.attr` |
| 输出 `.vks` 与 `.lua` 并存期间 | `ScriptRuntime` 尚未按扩展名分派（阶段 D），当前 Lua 只经 `LuaBackend` 直连自检/内部调用 |
| 热重载不接住"正在 wait 的协程" | 见 §7 |
| 物理无运行期开关 | `RigidBodyComponent` 的 `bodyType` / `useGravity` / `mass` 与 `CollisionComponent.solid`、`TriggerComponent.enabled` 都是**建物理世界时一次性烘焙**进刚体的。改组件字段**不会**生效，所以**故意没有** `actor.setKinematic` / `actor.setGravity` / `actor.trigger` —— 开放它们前必须先给 `ScenePhysics` 加运行时 setter，否则就是"调了没反应" |
| 不能逐层控动画 | 分层/NLA 的 `layers[i].speed/time/weight`、根运动模式、状态机参数都不在脚本 API 内。要"上半身挥手 + 下半身跑"，现在只能在动画控制器里做 |
| 没有骨级挂点 | `BoneAttachmentComponent` 没有 API。挂武器请用 `world.setParent(武器, 手)`（挂到**实体**，不是骨骼）；骨级挂点在场景里预先配好 |
| 音源组件不开放 | `audio.*` 只有一次性音效与总线。3D 音源的 `play/stop/volume` 要拿到 `AudioSystem` 的句柄，宿主没开 |
