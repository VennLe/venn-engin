# 脚本玩法路线图 —— 让 `.vks` 从「属性动画」变成「能写玩法」

> **一句话**：现在做新玩法 = 改 C++ = 重编译重启。这份文档把 `.vks` 补到
> **不改一行 C++ 就能写出巡逻兵、开门、计时挑战**的程度，并且**不需要换掉自研 VM**。
>
> 定稿 2026-10-19。替代 `ship_roadmap.md` §9 的 `G26 / G27 / G28`（那三条是同一件事的
> 粗粒版本，粒度和顺序都不够）。**执行序以本文档 §7 为准。**

---

## 0 · 这条路线图在解决什么

原始问题（用户原话）：

> vks 有完整语法、热重载、ScriptHost 接缝，但内置绑定只有数学 + print + transform
> （move/moveBy…）。没有实体查询、没有组件读写、没有事件/输入、没有跨帧状态与协程。
> 结果就是：做新玩法 = 改 C++ = 重编译重启。这一条不补，后面所有内容的迭代速度都被它按住。

拆成**五件事**，但它们的**依赖关系不是并列的**：

```
   ① 跨帧状态 ──┬──► ② 实体查询 ──► ③ 组件读写
   （实例）      │                        │
                 └──► ④ 事件 / 输入 ──────┴──► ⑤ 协程
```

**① 是地基，不是第五项。** 现在 `Evaluator` 的 `m_vars` 每次 `run()` 都新构造
（`ScriptEngine.cpp:619-625`），脚本**天生失忆**：

- 没有 ①，`hp = hp - 10` 每一帧都在对 0 做减法；
- 没有 ①，`find()` 找到了实体也存不住（下一帧就没了）；
- 没有 ①，`on collide` 里想"记下撞过谁"无从谈起；
- 没有 ①，协程连"挂起位置"都没地方放。

所以**顺序硬约束**：先有实例，再谈其余四件事。这条写进 §7 的 S0。

---

## 1 · 现状盘点（实测，2026-10-19）

### 1.1 已经有的（不要重做）

| 能力 | 位置 | 状态 |
|---|---|---|
| 词法 / 语法 / AST / 求值器 | `engine/script/ScriptEngine.cpp`（1217 行） | ✅ 支持 `if / else / while / 赋值 / 表达式语句`，运算符 `+ - * / % < <= > >= == != && \|\| !` |
| 资产编译缓存 + 热重载 | `ScriptAsset`（时间戳轮询 0.5 s） | ✅ 改动**成功才替换**，语法错时保留上一版继续跑 |
| 接缝 `ScriptHost` | 7 个 `std::function` 窄动作 | ✅ 设计是对的（HUD / 音频），**但只覆盖了两种宿主** |
| 36 个内建函数 | `Evaluator::call`（36 处 `if (name == "…")`） | ✅ 数学 17 + print 1 + 变换 6 + HUD 3 + 音频 3 + 灯光 4 + 可见/材质 2 |
| 接触事件 | `physics/ContactTracker`（G08） | ✅ Enter / Stay / Exit 三态差分**已跑通**，只是**没有派发给脚本** |
| 动作输入 | `core/InputMap`（G06） | ✅ 上下文栈 / 轴合成 / 死区 / 重绑定全有，**脚本侧无入口** |
| 空间索引 | `scene::Scene::spatialIndex()`（BVH） | ✅ `query(volume, out)`，但**只索引「可见 + 有网格」的实体** |

### 1.2 缺的（逐条对应原始问题）

| # | 缺什么 | 具体表现 |
|---|---|---|
| 1 | **跨帧状态** | 变量每帧归零；`frame` 是渲染帧号而不是固定步号 |
| 2 | **实体查询** | 只有隐式 `self`；`find("Boss")` 无处可写 |
| 3 | **组件读写** | 变换只有 `move(x,y,z)` 绝对设值；**没有 getter**（`pos()` 都没有）；刚体速度/冲量完全摸不到 |
| 4 | **事件 / 输入** | `ContactTracker` 采到了事件、只打了日志（`RuntimeApp.cpp:702` 注释白纸黑字写着"具体语言绑定在 G27"）；`InputMap` 完全没接线 |
| 5 | **协程** | 没有 `wait`；"等 2 秒再开门"只能用 `frame` 手算，一改帧率就废 |

### 1.3 ⚠ 七个「隐形问题」—— 不加功能也已经在伤害你

这七条不是"缺功能"，是**现有代码里会跟新能力打架的东西**。S0 必须一起清掉，
否则后面每加一个功能都要在坑上盖一层。

**P1 · 每次 `run()` 新建 `m_vars`** — 见 §0。这是"没有实例"的直接原因。

**P2 · 两份 tick 循环已经分叉。**
`EditorApp::runScriptsOn`（`editor/EditorApp.cpp:1428`）与 `RuntimeApp::runScripts`
（`runtime/RuntimeApp.cpp:611`）是**复制粘贴后各自演化**的两份：

| | EditorApp 版 | RuntimeApp 版 |
|---|---|---|
| `host.setHealth / damage / popup` | ❌ 没接 | ✅ 接了 HUD |
| `host.playSound / stopAllSounds / setBusVolume` | ❌ 没接 | ✅ 接了音频 |
| 时间源 | `m_ctx->playTime()` / `animTime()` | `m_simFrames * dt` |
| 错误表 | 成员 `errors` | `m_scriptErrors` |

后果：**同一个脚本在编辑器里 Play 和在 `Game.exe` 里跑，能力不一样**（编辑器里
`playSound` 静默不响）。这条现在已经能咬人，加事件/输入之后会变成"两份实现各自
漏一半"。**必须先抽成一个内核。**

**P3 · `time = t * timeScale` 会在改倍速时跳变。**
`prog->run(host, t * ts, dtd * ts, …)`，其中 `t` 是全局仿真时间。`ts` 从 1.0 改到 0.5
的瞬间，脚本看到的 `time` **倒退一半**。正确做法是每个实例自己累加
`localTime += dt * ts`。子弹时间（M3）一上来就会踩到。

**P4 · 运行期错误不熔断 —— 每帧重试。**
`run()` 返回 false 之后，下一帧**还会再跑一遍**。`m_scriptErrors` 只去重了日志。
一个死循环脚本每帧烧掉 `kMaxSteps = 200000` 步，**帧时间直接崩**，而且日志只打一次
—— 看起来"没报错"，实际每帧都在空转。

**P5 · 内建函数名是运行期字符串比较。**
36 个 `if (name == "…")` 顺序比较，每次调用平均 18 次 `std::string` 比较。更糟的是
**拼错函数名只在运行期报**（`unknown function 'moveby'`），每帧报一次，而热重载是
"编译通过才替换"—— 意味着拼错时**旧版本还在跑**，你以为改了其实没改。

**P6 · 所有绑定都是"对 self 隐式生效"。**
`setHealth` 甚至接的是 **HUD**，不是实体血量 —— 也就是说**引擎里根本没有"实体血量"
这个东西**。`setVisible(a[0].truthy())` 只能改自己。想表达"关掉那扇门"，
现有语言**没有任何句式**能指向"另一扇门"。

**P7 · `frame` 是渲染帧号，脚本按固定步跑。**
一个渲染帧跑两个定步时，两次 `run` 拿到**同一个** `frame`。
`hud_damage.vks` 的注释里已经记着这个坑（`frame == 60` 会扣两次血）。
解法不是让用户自己写 `>= 60 && < 62`，而是给一个**固定步号** `simFrame`。

---

## 2 · 为什么仍然自研，不换 Lua（以及什么条件下该换）

`ScriptEngine.h:15-27` 已经论证过一遍，这里只补**新的理由**和**退出条件**。

**继续自研的三条新理由**

1. **"脚本能碰什么"是类型系统级的封闭**。`ScriptHost` 里没有 `void*`、没有反射表，
   脚本拿不到 `AudioSystem` 门面就永远 `setMasterGain(0)` 不了。换成 Lua 之后，
   "只暴露窄接口"要靠在注册表里逐条小心，**漏一个就是后门**。
2. **确定性是免费的**。树遍历的顺序 = AST 顺序，ECS 遍历顺序 = dense 数组顺序，
   接触事件顺序 = `ContactTracker` 的 `(a,b)` 排序。整套是**逐位可复现**的，
   回归脚本能逐字符比日志。Lua 的表遍历顺序不保证。
3. **零第三方源码**。离线构建（`third_party` 里没有 VM），引入 Lua = 一份要长期
   跟着维护的 C 源码 + 一个要自己包的 C API 层。

**什么时候该认输换 Lua（写下来是为了将来不纠结）**

- 需要**在脚本里定义可复用的函数 / 结构体**，而且已经写满 3 个以上关卡；
- 需要一个**真正的调试器**（断点 / 单步 / 调用栈）—— 树遍历 VM 上做这个不划算；
- 需要第三方生态（比如现成的对话树 / 行为树库）。

三条里出现**两条**，就换。换的时候 `ScriptHost` 的窄接口原样保留，只重写
"值 + 语句 + 求值"这三层（`ScriptRuntime` 以上一行不动）。

---

## 3 · 目标 / 非目标

**目标（做到这些就算交付）**

1. 写一个**巡逻兵**：来回走、撞到玩家扣血、被打死消失 —— **纯脚本，零 C++ 改动**；
2. 写一个**定时门**：玩家靠近 3 m → 开 → 等 2 s → 关 —— 用 `wait`，不手算帧；
3. 写一个**拾取物**：碰到 → 计数 +1 → 消失 → 世界黑板记下"已拾取"；
4. 上面三件事**热重载有效**：改脚本不进 Play 重进、状态不丢；
5. **一个坏脚本不能冻住游戏**：死循环 3 帧内被熔断，帧时间不受影响；
6. 同一份脚本在**编辑器 Play** 与 **`Game.exe`** 里能力**完全一致**（P2 根治）。

**非目标（明确不做，避免无限扩张）**

- ❌ 不做 GC（`Value` 只持有 `string / vector<Value>`，无环，析构即回收）；
- ❌ 不做线程（协程 = 帧驱动，绝不引线程）；
- ❌ 不做异常 / try-catch / 模块 import；
- ❌ 不做 `class / struct / 函数定义`（用 `on` 钩子 + 黑板替代，见 S-D4）；
- ❌ 不做脚本里创建/销毁实体（那是 C++ 或预制体的活；`spawn` 留到 W5 之后再议）；
- ❌ **不改「编辑态不跑脚本」这条铁律**（`ecs::ScriptComponent` 的注释里写着：
  脚本只在运行态求值，Stop 之后编辑态场景永远是你离开时的样子）。

---

## 4 · 总体架构

### 4.1 一张图

```
  ┌──────────────────────────────────────────────────────────────────┐
  │ 宿主（二选一，能力必须一致）                                       │
  │   runtime/RuntimeApp.cpp     editor/EditorApp.cpp                │
  │        │                          │                              │
  │        └────────► ScriptServices ◄┘   ← 唯一差异点在"谁填这个结构" │
  │                    （HUD / 音频 / 输入 / 玩家 / 日志 / trace）      │
  └────────────────────────────┬─────────────────────────────────────┘
                               │  tick(scene, simTime, dt)
  ┌────────────────────────────▼─────────────────────────────────────┐
  │  vks::ScriptRuntime        ← **新增**：实例表，唯一的 tick 内核    │
  │    · 每实体一个 ScriptInstance（vars / pc 栈 / 本地时间 / 状态）    │
  │    · 事件队列（collide / trigger / exit）按 ContactTracker 顺序派发│
  │    · 预算与熔断（per-instance steps + per-frame total）            │
  └────────────┬─────────────────────────────────┬───────────────────┘
               │ 取编译产物（按路径共享）          │ 填 ScriptHost（窄接口）
  ┌────────────▼──────────────┐      ┌───────────▼───────────────────┐
  │ vks::ScriptEngine（已有）  │      │ vks::ScriptHost（扩）          │
  │   ScriptAsset → Program    │      │   scene / entity               │
  │   热重载 0.5 s 轮询         │      │   + 输入 4 个 / 查询 3 个       │
  └───────────────────────────┘      │   + 事件回调 3 个 / 黑板        │
                                     └───────────────────────────────┘
```

**一句话记住分层**：`编译产物按路径共享，运行状态按实体独立`。
`ScriptEngine` 管前者（已经有了，几乎不动），`ScriptRuntime` 管后者（新增）。

### 4.2 四个角色各自只干一件事

| 角色 | 职责 | 不知道什么 |
|---|---|---|
| `ScriptAsset` / `ScriptProgram` | 读盘、编译、热重载、AST | 不知道有几个实体在用它 |
| `ScriptRuntime` | 实体 ↔ 实例、tick 顺序、预算、事件派发 | 不知道 HUD/音频/输入**怎么实现** |
| `ScriptHost` | 脚本这一帧能看到的世界的**快照式接口** | 不知道 `ScriptRuntime` 的存在 |
| `ScriptServices` | 宿主填的一次性回调（HUD/音频/输入/玩家） | 不知道脚本语言长什么样 |

### 4.3 为什么必须先把「实例」做出来

`ScriptRuntime` 里的实例长这样（S0 就要定下来，后面五个阶段全挂在它上面）：

```cpp
struct ScriptInstance {
    ecs::Entity entity{};        // 谁挂的（含 generation，防悬空）
    std::string path;            // 用哪个脚本
    ScriptProgram* prog = nullptr;  // 指向共享的编译产物

    std::unordered_map<std::string, Value> vars;  // ★ 跨帧状态（P1）

    // ---- 执行状态（S4 用；S0 先占位）----
    struct Frame { const std::vector<StmtPtr>* body; size_t pc; bool loopBody; };
    std::vector<Frame> stack;
    enum class State { Idle, Running, Waiting, Failed };
    State state = State::Idle;
    double wakeAt = 0.0;         // 协程唤醒时刻（本地时间）
    double localTime = 0.0;      // ★ 修 P3：自己累加，不用全局 t * ts
    uint64_t simFrames = 0;      // ★ 修 P7：固定步号

    bool started = false;        // on start 跑过没
    std::string lastError;       // 熔断原因（P4）
    uint64_t stepsThisFrame = 0; // 预算
};
```

**这个结构一旦存在，五件事里有四件变成"往里面填"，而不是"重新设计"。**

---

## 5 · 语言设计（只加「看得见」的）

### 5.1 能力矩阵

| 维度 | 现在 | 目标 | 加什么 |
|---|---|---|---|
| 值 | `num` / `bool` / `str` | + `vec3` / `entity` / `list` | §5.2 |
| 状态 | 无 | 实例级 vars + `let` | S-D2 |
| 控制流 | `if / else / while` | + `foreach` | §5.3 |
| 钩子 | 无（顶层语句就是全部） | `on start / update / collide / trigger / exit` | S-D4 |
| 时间 | `time / dt / frame` | + `simFrame` / `wait` / `waitFrames` / `waitUntil` | S-D9 |
| 世界 | `self` 隐式 | `self()` / `find` / `near` / `player` / 黑板 | S-D5 |

### 5.2 值类型：只加三种

**`vec3`** —— 不加就永远在拆分量。
加 `vec3(x,y,z)` 构造 + `+ - *`（vec3×vec3 逐分量、vec3×num 缩放）+ `.x/.y/.z`
**属性访问**（这是语言里唯一的成员语法，值得为它开一次口子）。
代价：所有 `asNum()` 遇到 vec3 要报清晰错误（`"expected number, got vec3"`），
不能静默取 `.x`（静默取分量是最难查的那类 bug）。

**`entity`** —— 与 `num` 分开，不塞进 double。
`ecs::Entity` 是 `{id, generation}`，generation 就是防悬空的那一位；
塞进 double 会丢 generation，于是"物体被删了、句柄被复用"时脚本会**静默改到别人身上**。
`toStr()` → `Enemy#12`（日志里人看得懂）。无效实体 = `id == kInvalidEntity`。

**`list`** —— 只为 `near()` / `all()` 服务。
`count(l)` / `at(l, i)` / `foreach`。**不做 `push` / 切片 / 排序**（要做就回 C++ 加窄函数）。

### 5.3 语句：只加两条

```
on start { … }              # 进入 Play 后跑一次
on update { … }             # 每个固定步跑一次
on collide(other) { … }     # 撞上（Enter）
on trigger(other) { … }     # 进入触发区（Enter）
on exit(other) { … }        # 离开（Exit）

foreach (e in near(pos(self()), 20.0)) { … }
```

`on` 块在**编译期**被收集成一张表（`Program::hooks`），求值器按钩子名调度。
**没有 `on` 块的脚本仍然完全合法** —— 顶层语句 = 每帧执行，与今天逐位相同
（`audio_play.vks` / `hud_damage.vks` 一字不改继续跑）。这条兼容性是硬要求。

### 5.4 内建函数：36 → 约 62，按「族」加

| 族 | 新增（★ = 玩法必需） | 说明 |
|---|---|---|
| 值构造 | `vec3` `count` `at` | `vec3` 已有 3 个 num 的重载版本要一并支持 |
| 自我 | ★`self()` `name(e)` `exists(e)` | `self()` 与隐式 self 等价，给"传参"用 |
| 查询 | ★`find(name)` `near(pos,r[,limit])` `player()` `dist(a,b)` | `near` 走 BVH（⚠ 见 §6 S-D5 的限界说明） |
| 变换 | ★`pos(e)` ★`setPos(e,v)` `rot(e)` `setRot(e,v)` `rotY(e)` `scaleOf(e)` | 老 `move(x,y,z)` 保留 = `setPos(self, vec3(x,y,z))` |
| 刚体 | ★`vel(e)` ★`setVel(e,v)` `impulse(e,v)` `grounded(e)` | 物理 v1 已有 `RigidBody`；`grounded` 读 `CharacterController` |
| 黑板 | ★`attr(e,key[,def])` ★`setAttr(e,key,v)` `worldAttr(key[,def])` `setWorldAttr(key,v)` | 见 S-D5 |
| 可见/材质 | `visible(e)` `setVisible(e,b)`（老签名保留） `setEmissive(e,r,g,b)` | |
| 事件 | —（事件是钩子不是函数） | |
| 输入 | ★`pressed(a)` ★`held(a)` `released(a)` ★`axis(a)` | 动作名走 `InputMap`，**不是键码** |
| 时间 | `simFrame`（内置变量） | 修 P7 |
| 协程 | ★`wait(sec)` `waitFrames(n)` `waitUntil(expr)` | 见 S-D9 |

⚠ **全部在编译期解析成 enum**（S-D6）。拼错函数名 = 编译失败 = 面板红字 +
旧版本继续跑，而不是每帧刷一条运行期警告。

### 5.5 目标脚本（这段是验收基准，S6 会真的放进 `assets/scripts/`）

```vks
# enemy_patrol.vks —— 巡逻兵：来回走，撞到玩家扣血，血尽消失
let hp = 100
let dir = 1
let home = vec3(0, 0, 0)

on start {
    home = pos(self())          # ★ 记下出生点（跨帧状态，S0 之前做不到）
}

on update {
    p = pos(self())
    if (p.x > home.x + 8.0) { dir = -1 }
    if (p.x < home.x - 8.0) { dir = 1 }
    setPos(self(), p + vec3(dir * 2.5 * dt, 0, 0))
    setAttr(self(), "hp", hp)   # ★ 给 Inspector 看（黑板可读）
}

on collide(other) {
    if (name(other) == "Player") {
        hp = hp - 10
        popup(10)
        playSound("audio/hit.wav")
        if (hp <= 0) { setVisible(self(), false) }
    }
}
```

```vks
# door_timer.vks —— 玩家靠近 3 m，开 2 秒，关
let busy = 0

on update {
    if (busy == 0 && dist(pos(self()), pos(player())) < 3.0) {
        busy = 1
        setRot(self(), vec3(0, 0, -90))   # 开
        wait(2.0)                          # ★ 协程（S4）
        setRot(self(), vec3(0, 0, 0))      # 关
        busy = 0
    }
}
```

**这两段能跑 = 这条路线图完成。** 注意它们**没有一行 C++**，也没有任何
"改一下要重新编译"的地方。

---

## 6 · 十一条决策

> 格式：**决策 / 理由 / 代价**。代价是硬的，不写"没什么代价"。

### S-D1 · 实例是第一公民

- **决策**：新增 `ScriptInstance`，`vars` 跟着实体存活；`ScriptProgram`（AST）仍然按
  路径共享，一个实体一个实例。
- **理由**：见 §0。没有实例，后面四件事全是沙上建塔。
- **代价**：`Evaluator` 不再自己 new `m_vars`，改成引用外部 `vars`。求值器的构造签名
  变了，但语义不变（`time/dt/frame/index` 仍然每帧覆盖写）。

### S-D2 · `let` = **幂等**声明

- **决策**：`let hp = 100` 的语义是「**如果实例里没有 `hp`，就设成 100**」；已有值一律不动。
  裸赋值 `hp = 50` 仍是普通赋值。
- **理由**：这是**为热重载量身定做**的一条。热重载保留状态之后，新代码里新增的
  变量没有值 —— 如果读未定义变量就报错，那么"热重载时加一个计数器"必然红。
  `let` 的幂等语义恰好补上，而且它同时是正常代码的默认值声明。**一条语法解决两个问题。**
- **代价**：用户可能误以为 `let` 每次进 `on start` 都重置 —— 文档要写清
  "要重置用 `hp = 100`，不要用 `let hp = 100`"。且 `let` 只在**顶层**合法（`on` 块内
  写 `let` 报编译错，避免"每次 update 都静默跳过"的困惑）。

### S-D3 · 语句层显式栈，表达式层保持递归

- **决策**：`exec()` 从递归改成**显式帧栈**（`vector<Frame>`）；`eval()` 一行不改。
- **理由**：`wait` 要挂起，递归实现**没有任何地方**能存"我执行到哪了"。但**表达式
  求值永远不需要挂起**（`wait` 是语句，不是表达式）—— 所以只改语句层就够了，约
  200 行。顺带白拿两个好处：**嵌套深度不再吃 C++ 栈**（现在 200 层 `if` 能爆）、
  **错误定位**能精确到每一帧的语句行号。
- **代价**：`while` 的语义要在栈帧里表达成「这是 loop body，跑完回 cond 重判」，
  比递归绕。**必须配一个"改写前后行为逐位相同"的回归**（S0 就加，
  用 `--selftest script` 跑同一批脚本比 `prints` 与变量终值）。

### S-D4 · 钩子用 `on` 块，**不加**函数定义

- **决策**：只支持 5 个固定钩子名；不支持用户定义函数、不支持递归调用、不支持传参
  给自定义函数。
- **理由**：一旦开了"用户函数"，就要有**调用栈、作用域、返回语句、参数绑定**四件事，
  还要写错时的报错体验。这四项加起来比整个 S0+S1 都贵。而 95% 的玩法逻辑
  用「每帧更新 + 事件钩子 + 状态变量」写得出来。
- **代价**：重复逻辑要复制，或者抽成 `wait` 序列。**认了**。真不够时按 §2 的判据换 Lua。

### S-D5 · 世界访问 = 窄接口 + 黑板，**不做**反射式 `getComponent(e, "Health")`

- **决策**：
  1. 每个组件族给**具名函数**（`pos` / `vel` / `visible` / `attr`），不做通用反射；
  2. 玩法状态（血量 / 弹药 / 计数 / 状态机相位）走**黑板**：
     实体级 `attr(e, key)` + 世界级 `worldAttr(key)`，值只有 `double`；
  3. 黑板**分两种生命周期**：`attr` 在实例内存（**不存盘**）；
     `worldAttr` 挂在 `Scene` 上（**存盘**，用于"已拾取 / 已开门"这类关卡进度）。
- **理由**：
  - 引擎**没有**组件类型注册表（`unordered_map<type_index, pool>`），运行时按字符串
    查类型要新建一套反射 —— 而反射一旦存在，"脚本能改任何组件"的安全边界就没了；
  - 每加一个玩法变量就加一个 C++ 组件，正是要消灭的那个循环；
  - 具名函数能在**编译期**检查参数个数与名字，反射不能。
- **代价**：引擎加新组件时，要手写一对 `attr` 式访问器（约 3 行/组件）。**这是故意的**
  —— 每暴露一个能力都要过一次"这该不该让脚本碰"的判断。
- ⚠ **`near()` 的限界**：`Scene::spatialIndex()` 只索引**可见 + 有网格**的实体，
  隐藏的敌人 / 纯逻辑实体查不到。所以 `near()` 的实现是"BVH 快查 + 补扫
  `Transform` 池"，且**结果按 `(dist, id)` 排序**保证确定性。

### S-D6 · 内建函数名在**编译期**解析

- **决策**：`Expr::Call` 里存 `BuiltinId` enum（编译时查表），运行时 `switch`；
  未知名字 / 参数个数不对 = **编译错误**（不是运行期错误）。
- **理由**：修 P5。36 次字符串比较 → 一次 switch；拼错立刻红。
- **代价**：必须在"新增内建函数"时**同时**改三处（enum / 名字表 / switch），
  漏一处会编译不过 —— 这反而是好的（编译期强制同步）。

### S-D7 · 热重载语义：**保状态、不重跑 start、位置失效则整体重启**

- **决策**（三句话讲完，写死在文档里）：
  1. 实例的 `vars` **保留**（配合 S-D2 的 `let` 补默认值）；
  2. `on start` **不重跑**（否则热重载会把"出生点"重置成 "重载时所在的位置"）；
  3. 如果实例正挂在 `wait` 上（S4），且新的 AST 在**同一个栈位置**对不上
     （语句数变少 / 类型变了）→ **整个实例重启**（清 vars、重跑 start），
     并打一行 `VKS-RELOAD-RESTART entity=… reason=pc_mismatch`。
- **理由**：热重载最常见的用法是"调一个数字 / 改一行逻辑"，此时状态必须留；
  而结构大改时保留一个无意义的 pc 只会产出玄学行为。**规则简单 > 行为正确**。
- **代价**：S4 的实现要能在 reload 时做"栈位置对账"，约 40 行。

### S-D8 · 错误即熔断（fail-stop）

- **决策**：`run()` 失败 → 实例 `state = Failed`，**后续帧不再执行**，直到
  ①热重载成功 ②面板点 Reset ③实体换脚本。日志只打一次（带行号 + 错误）。
- **理由**：修 P4。死循环脚本每帧 20 万步会直接毁掉帧时间，而"只在第一次报错"
  让你以为它停了 —— **静默的 CPU 黑洞**。
- **代价**：一个"偶然失败一次"的脚本会被永久停掉（比如某帧数据异常导致除零）。
  这是**故意选的方向**：宁可停掉并大声报错，不要每帧半死不活。

### S-D9 · 单实例单协程；`wait` 不阻塞帧、不引线程

- **决策**：
  - 一个实例同时只有**一个**执行位置（在 `on update` 里 `wait` 之后，`on update`
    不再被调用，直到唤醒）；**不支持 `spawn` 多协程**；
  - 事件钩子（`collide`）在实例处于 `Waiting` 时**仍然会跑**（事件不该被吞），
    但**不能**在事件钩子里 `wait`（编译期禁止：钩子体内的 `wait` 只在 `update` 内合法）。
    —— 这条限制换来"语义能用一句话说清"。
  - `wait(n)` 的 `n` 是**本地时间**（受 `timeScale` 影响，子弹时间里慢动作一致）。
- **理由**：多协程要引入句柄、生命周期、"谁在等谁"，而它的收益是 5% 的脚本。
  单协程 + 状态变量能覆盖"分镜式逻辑"。
- **代价**：`update` 里 `wait` 之后**这一帧剩余的语句不执行**（挂起）。
  写 `wait` 在 `while` 循环里的脚本要自己保证不会每帧重入（`busy` 标志，见 §5.5 的门）。

### S-D10 · 「编辑态不跑脚本」这条铁律不变

- **决策**：`on start` / `on update` / 事件 **全部只在运行态**（编辑器 Play 的
  runtime 副本 / `Game.exe`）。编辑态的脚本面板只做**预编译 + 语法报错**。
- **理由**：这是"编辑态 / 运行态场景分离"的基石（`EditorContext`），一旦破了，
  Stop 之后场景永远回不到你离开的样子。
- **代价**：编辑态看不到脚本效果，得按 Play。**认了**（这也是引擎界通行做法）。

### S-D11 · 确定性是硬约束

- **决策**：①实体遍历顺序 = `ecs::World` 的 dense 顺序；②`near()` 结果按 `(dist, id)`
  排序；③接触事件按 `ContactTracker` 的 `(a,b)` 排序派发；④脚本里**没有** `random()`
  （要随机就用 `MYVK_VKS_SEED` + 内置 `rand()`，种子固定时序列固定）。
- **理由**：回归脚本靠逐行比日志（`verify_runtime_determinism.py` 已经在做这件事）。
  一条不确定性会毁掉整个自动化体系。
- **代价**：`rand()` 的种子必须由宿主给，不能用 `std::random_device`。

---

## 7 · 分阶段（执行序，不可跳）

> 每阶段末尾都有「验收」，全部是**可自动化的**（`--selftest script` / `verify_*.py`）。
> 规模：S ≈ 半天，M ≈ 1~2 天，L ≈ 3~5 天。

### S0 · 抽内核 + 实例化（**无新语法，行为逐位不变**）　规模 M　依赖 —

**目标**：把两份 tick 循环合成一份，并让变量活过一帧。

**改动点**
1. 新建 `engine/script/ScriptRuntime.{h,cpp}`：
   ```cpp
   struct ScriptServices {          // 宿主填的一次性回调
       std::function<void(float)> setHealth, damage;
       std::function<void(float,float,float,float)> popup;
       std::function<void(const std::string&)> playSound;
       std::function<void()> stopAllSounds;
       std::function<void(const std::string&,float)> setBusVolume;
       // S2/S3 会往这里加：输入 4 个、player()、trace 开关
   };
   class ScriptRuntime {
   public:
       void setServices(ScriptServices);
       void reset(scene::Scene&);                    // 进 Play / 换场景
       void tick(scene::Scene&, double simTime, float dt);  // 唯一的 tick 内核
       ScriptInstance* find(ecs::Entity);
       void resetInstance(ecs::Entity);
       const std::vector<std::unique_ptr<ScriptInstance>>& instances() const;
       struct Stats { uint32_t running, waiting, failed; uint64_t steps; };
       const Stats& lastStats() const;
       vks::ScriptEngine& engine();                  // 面板要用（资产列表 / 热重载）
   };
   ```
2. `Evaluator` 接受外部 `vars` 引用；`ScriptProgram::run` 增加
   `run(host, vars, instanceState, err)` 的重载（老的 `run` 保留给自检）。
3. **熔断**（修 P4）、**本地时间**（修 P3）、**`simFrame` 内置变量**（修 P7）。
4. `EditorApp::runScriptsOn` 与 `RuntimeApp::runScripts` 都改成调
   `m_scriptRuntime->tick(...)`，各自只填 `ScriptServices`。→ **编辑器 Play 与
   `Game.exe` 从此能力一致**（修 P2）。

**验收**（`--selftest script`，≥8 条）
- A1 `n = n + 1` 跑 10 步 → `n == 10`（**这条在今天必然失败**，是 S0 的靶心）
- A2 两个实体挂同一脚本 → 各自独立（`n` 分别 3 和 5）
- A3 熔断：`while (1) {}` → 第 1 步后 `state=Failed`；**第 2 步不再执行**
      （断言 `executedCount == 1`），且日志只出 1 条
- A4 本地时间：`ts` 1.0 → 0.5 后 `time` **单调不减**（今天会倒退）
- A5 `simFrame` 在固定步下严格 +1（不是渲染帧号）
- A6 兼容：`hud_damage.vks` 的 `health == 70`、`audio_play.vks` 的 4 条日志**逐字不变**
- A7 `reset()` 之后实例全部重建、`vars` 清空
- A8 面板/日志零回归：`GAME-SCRIPT precompiled N reference(s)` 数字不变

### S1 · 语言层：值类型 + 编译期检查　规模 L　依赖 S0

**目标**：`vec3` / `entity` / `list` 三种值；内建函数名编译期解析；`let`。

**改动点**
1. `Value` 加 `K::Vec3 / K::Entity / K::List`（`list` 用 `std::vector<Value>`）；
   `asNum()` 遇到非数值**必须报类型错误**（不许静默取分量）。
2. 运算符：`+ - *` 支持 vec3（逐分量 / 标量缩放）；`==` 支持 entity（id+gen）；
   `.x/.y/.z` 是**唯一的成员访问**（`Expr::Kind::Member`）。
3. `BuiltinId` enum + 名字表 + `switch` 派发（修 P5）。
4. 编译期检查：未知函数名、参数个数（新加 `BuiltinArity`）、`let` 只能在顶层。
5. `foreach (e in expr) { }`。

**验收**（`--selftest script`，≥20 条）
- B1 `vec3` 算术逐分量（含 `v * 2.0`）
- B2 `vec3 + num` → **编译或运行期报清晰错误**（不是静默变 NaN）
- B3 `entity` 往返：`find("X")` → `name(e) == "X"`；`find("Nope")` → `exists(e) == false`
- B4 悬空：`destroy` 后 `exists(oldHandle) == false`（generation 生效）
- B5 **负向测试**：`moveby(1,2,3)`（拼错大小写）→ **编译失败**，错误信息含行号
- B6 负向测试：`sin()` 参数个数错 → **编译失败**
- B7 `let x = 1` 在 `on` 块内 → 编译失败（带"let 只能在顶层"）
- B8 `foreach` 遍历 5 个实体，顺序 = dense 顺序（连跑两次逐位相同）

### S2 · 世界访问：查询 + 组件 + 黑板　规模 L　依赖 S1

**目标**：`find / near / player / pos / setPos / vel / attr` 全部可用。

**改动点**
1. `ScriptInstance` 之外新增 `ecs::ScriptVarsComponent`
   （`std::unordered_map<std::string,double>`）+ 序列化**勾选**：
   ⚠ 这个组件**不进存档**（运行时状态），但 `worldAttr` 的世界黑板
   （`Scene::scriptVars`）**进存档** —— 两者必须分开，别混。
2. 组件访问器族（§5.4 表），每个都是 3 行 + 编译期 arity。
3. `near()`：BVH 快查 + 补扫 Transform 池（S-D5 的限界），结果 `(dist,id)` 排序。
4. `player()`：从 `ScriptServices` 注入（runtime 知道玩家是谁，engine 不知道）。
5. 配额：每帧每实例内建调用数上限（默认 20000，`MYVK_VKS_QUOTA` 可调）。

**验收**（≥14 条）
- C1 `setPos` 后**同一帧** `Renderer` 读到的世界矩阵已变（脏标记生效）
- C2 `setPos` 后**同一帧**物理 `RigidBody` 位置已变（不是下一帧）
- C3 `near(vec3(0,0,0), 20)` 命中数 == 手工数出来的数；结果按距离升序
- C4 `near` 能查到**隐藏**实体（BVH 查不到 → 补扫兜住）
- C5 `attr` 默认值：`attr(e,"hp",100)` 在没有时返回 100 且**写回**
- C6 两个实体同名 `attr "hp"` 互不影响
- C7 存档往返：`worldAttr` 保住，`attr` **不出现**在 JSON 里
- C8 配额：一个循环里调 30000 次 `pos()` → 报错 + 熔断，**帧时间不超预算**

### S3 · 事件与输入　规模 M　依赖 S2

**目标**：脚本能"响应"。

**改动点**
1. `Program::hooks`：编译期收集 `on start / update / collide / trigger / exit`。
2. `ScriptRuntime::tick` 的相位顺序（**写死，别含糊**）：
   ```
   ① 对刚创建的实例跑 on start（组件已全部构造完）
   ② 派发本步接触事件（collide / trigger / exit）
   ③ 跑 on update（顶层语句 = 隐式 on update，两者按"顶层先、on update 后"）
   ④ 打 GAME-SCRIPT 统计行（只在 trace 下）
   ```
   `pumpContactEvents()` 在 `onFixedUpdate` 里比脚本**早**一步，保证"撞上这一帧
   就能反应"。
3. 输入四件套：`pressed / held / released / axis`，动作名走 `InputMap`
   （`ScriptServices` 注入 4 个 `std::function`，engine 不认 `InputMap` 实例）。
   ⚠ 只在 Play 态有值；编辑态/自检里宿主给 nullptr → **恒 false**（不是崩）。

**验收**（≥12 条）
- D1 注入 `Jump` 边沿 → 脚本 `setPos` 抬升，位移**符合预期**
- D2 `held` 在连按 3 帧里为 true，第 4 帧松开后 false
- D3 `on collide(other)` 里 `name(other)` 与"实际被撞的实体"一致
- D4 接触序列：Enter×1 → Stay×N → Exit×1（与 `ContactTracker` 自检同口径）
- D5 `on start` 里创建的实体，**同一帧**能被 `find` 找到（明确语义，别玄学）
- D6 静态断言：`on collide` 里写 `wait` → **编译失败**（S-D9）
- D7 宿主没填输入 → `pressed("Jump")` 恒 false，脚本不报错

### S4 · 协程与序列　规模 M　依赖 S3

**目标**：`wait` / `waitFrames` / `waitUntil`。

**改动点**
1. `exec()` 改显式帧栈（S-D3）。**这是全路线图风险最高的一步** →
   必须先在 S0 加好"改写前后逐位相同"的回归（同一批脚本比 `prints` + 变量终值）。
2. `wait(sec)`：`state = Waiting; wakeAt = localTime + sec;` 挂起；唤醒时从栈顶恢复。
3. `waitUntil(expr)`：每步重估 expr（受配额约束）。
4. 热重载对账（S-D7 第 3 条）。

**验收**（≥10 条）
- E1 `wait(1.0)` → 恢复时刻误差 **< 1 个固定步**
- E2 `wait` 在 `if` 内 / `while` 内都合法，且挂起位置精确（嵌套 3 层）
- E3 `while` 循环体里 `wait(0.1)` 跑 10 次 → 总时长 ≈ 1.0 s（不重入、不加速）
- E4 1000 个实例同时 `wait` → 帧时间与"0 个 wait"同量级（O(活跃) 而非 O(总数)）
- E5 热重载：`wait` 挂起中改脚本（只改一个数字）→ **仍从原位置恢复**
- E6 热重载：把 `wait` 那行删掉 → `VKS-RELOAD-RESTART` 日志出现，实例重启
- E7 改写前后逐位相同：S0 那批脚本的 `prints` 与变量终值**一字不差**

### S5 · 编辑器可见（五件套里的「UI 面」）　规模 M　依赖 S4

**改动点**
1. Scripts 面板加 **Instances** 页签：每行 = 实体名 / 脚本 / 状态
   (`Idle|Running|Waiting|Failed`) / 本地时间 / 变量数 / `Reset` 按钮 / 错误行。
   锚点 `SCR-INST-<index>`、`SCR-INST-RESET`。
2. 变量查看器（只读，展开看 `vars`；`Failed` 实例显示 `lastError`）。
3. Inspector 的 Script 段：显示该实体实例状态 + `Reset`（锚点 `INS-SCRIPT-STATE`）。
4. 控制台命令：`script.reset <实体名>` / `script.vars <实体名>` / `script.trace on|off`
   （`MYVK_EDITOR_CONSOLE_CMD` 支持 `;` 多值，自动化可直接下发）。

**验收**（`verify_script_editor.py`，≥12 条）
- F1 面板出现 Instances 页签且列出 3 个实例（锚点全在）
- F2 状态列在 Play 后从 `Idle` 变 `Running`（**像素/文本双证**）
- F3 改脚本 → `#reload` 计数 +1，变量**不清零**（面板数字证明）
- F4 点 Reset → 变量数回到初始（`n=0`）
- F5 熔断实例显示红字 + 状态 `Failed`；点 Reset 后恢复
- F6 控制台 `script.vars Boss` 打印 3 行键值（日志断言）

### S6 · 示例关卡 + 基准 + 收口　规模 M　依赖 S5

**改动点**
1. `assets/scripts/` 落地 4 个脚本：`enemy_patrol.vks` / `door_timer.vks` /
   `pickup.vks` / `hud_counter.vks`。
2. `assets/scenes/levels/script_demo.json`：1 个地形 + 3 个巡逻兵 + 2 扇门 +
   5 个拾取物 + 1 个计时挑战（**全部逻辑在脚本里**）。
3. `--selftest scriptbench`：200 实例 × 100 语句/帧，打
   `VKS-BENCH instances=200 stmts=20000 ms=…`，钉住预算。
4. 台账（§11）+ 回归登记（`run_regression.py` 的 `DEFAULT` / `TRIGGERS`）。

**验收**（`verify_script.py` 端到端，≥14 条）
- G1 「不改一行 C++」：`script_demo` 加载 → 3 个巡逻兵在动（像素差 > 阈值）
- G2 玩家撞巡逻兵 → HUD 血量下降（日志 `HUD-STATS`）
- G3 门：靠近 → 2 s 后关上（按时刻断言，不按帧）
- G4 拾取 5 个 → `worldAttr("picked") == 5`
- G5 存档 → 读回 → `worldAttr` 保住，巡逻兵**位置不保**（运行时状态不存盘）
- G6 热重载：改 `enemy_patrol.vks` 的速度 → **不重启**，位置连续（无跳变）
- G7 基准：`scriptbench` 帧时间 < 预算（写死在用例里）

---

## 8 · 总验收（一条判据）

> **拿一个新关卡，从"设计玩法"到"能在编辑器里玩"，全程不改任何 `.cpp`、
> 不重编译引擎。**

满足它就交付；不满足就还有 C++ 要改。这是**唯一**能防止"补了一堆绑定、
结果写玩法还是要改 C++"的判据。

---

## 9 · 明确不做（写下来是为了不反复讨论）

| 不做 | 原因 |
|---|---|
| 用户自定义函数 / 递归 | 要调用栈 + 作用域 + return，比 S0+S1 都贵（S-D4） |
| 多协程 `spawn` | 要句柄 + 生命周期，"单协程 + 状态变量"够 95%（S-D9） |
| `class / struct / 模块 import` | 一个 `.vks` 就是一个实体的一份逻辑，模块化靠黑板 |
| GC / 引用计数 | `Value` 无环，析构即回收 |
| 线程 / 异步 IO | 语言里根本没有 IO |
| 脚本创建 / 销毁实体 | 涉及生命周期与存档一致性，等 W5 之后再议 |
| 反射式 `getComponent(e,"X")` | 引擎没有类型注册表，且会破坏安全边界（S-D5） |
| 编辑器里跑脚本 | 破坏编辑/运行分离（S-D10） |

---

## 10 · 与其它路线图的耦合

| 路线图 | 耦合点 | 何时接 |
|---|---|---|
| `character_pipeline_roadmap` **A5 动画事件** | `on animEvent(name)` —— 攻击判定窗口 / 脚步声 | A5 完成后加第 6 个钩子（`Program::hooks` 表加一行） |
| `ship_roadmap` **G17 存档** | `worldAttr` 是关卡进度的载体（"已拾取 / 已开门"） | S2 就把 `Scene::scriptVars` 的序列化做好 |
| `world_scale_roadmap` **W4 地形** | `heightAt(x, y)` 查询（脚本里撒点/贴地） | 随时可加（`physics::Heightfield` 已就绪，加一个内建函数） |
| `audio_roadmap` §13 | `playSound` 已经是**窄动作**了，S0 只是搬进 `ScriptServices` | S0 |
| `ship_roadmap` **G30 shipping** | 脚本是资产，**要一起打进 pak**（G29 的引用闭包别忘了 `.vks`） | G29 时 |
| `ship_roadmap` **G31 崩溃** | 脚本错误要把 `path:line` 打进日志（S-D8 已经做了） | 已满足 |

---

## 11 · 台账（实测数字与坑，逐项追加）

| 日期 | 项 | 结果 |
|---|---|---|
| 2026-10-19 | 路线图定稿 | 盘点 36 个内建函数 / 1217 行求值器 / 两份 tick 循环；发现 7 个隐形问题（P1~P7）。**S0 是唯一的硬依赖。** |
| 2026-10-19 | **S0 抽内核 + 实例化 ✅** | 新建 `engine/script/ScriptRuntime.{h,cpp}` = **唯一 tick 内核**。`--selftest script` **18 条全绿**：A1 `n==10`（P1 靶心）· A2 两实例 3/5 且 `scripts=1 instances=2`（按路径共享/按实体独立）· A3 死循环 `steps=200001` 后熔断、第 2 步 `steps=0`、错误**只报 1 次** · A4 倍速 1.0→0.5 后 `time` 0.1333→0.2000（老实现会给 0.0667）· A5 `simFrame=5` 而 `frame` 仍是 60 · A6 `hud_damage` health=70 / `audio_play` 四动作各一次 · A7~A10 reset/换脚本/disabled/回收。**零回归**：`verify_hud` PASS（health 70.0）、`verify_audio` PASS（plays=4）、`GAME-SCRIPT precompiled 1 reference(s)` 不变。 |

| 2026-10-19 | **S1 语言层 ✅** | `--selftest script` **36 条全绿**（A 18 + B 18）。`Value` 扩到六种 `K`；内建名**编译期**解析成 `enum class Builtin`（45 项表 + `static_assert` 兜数量）。B1 vec3 五种运算 `sum=21 dsum=9 msum=nsum=12 wsum=32 ax=1` · B2 `vec3+num` → `cannot use '+' between vec3 and number (use vec3(...) or a component like .x)` · B3 `find/name/exists/self` 往返 · B4 删实体后旧句柄 `exists` 变 false（generation 生效）· B5 拼错 `moveby` 编译失败且 `line=2` · B6 `sin()` → `'sin' expects 1 argument(s), got 0` · B7 块内 `let` → `let is only allowed at the top level` · B8 `foreach` 5 个按距离排序 `T0,T1,T2,T3,T4`、连跑两次逐位相同 · B9 `let` 幂等 `n=4` · B10 循环变量遮蔽后恢复。**零回归**：`verify_hud` / `verify_audio` PASS。 |

| 2026-10-19 | **S2 世界访问 ✅** | `--selftest script` **53 条全绿**（A18 + B18 + C17）。新增 15 个内建（`player/pos/setPos/rot/setRot/rotY/scaleOf/vel/setVel/impulse/grounded/attr/setAttr/worldAttr/setWorldAttr/visible`，共 61 个）；`ecs::ScriptVarsComponent`（逐实体、**不存盘**）+ `Scene::scriptVars`（世界、**进存档**）分开落地；`near` 换成 **BVH 快查 + 补扫 Transform 池**。实测：C1 `setPos` 同帧 `world=(3,4,5)` 且 `pos()` 读回 12 · C2 同帧写进刚体 `calls=1` · C3 `n=6 asc=1 acc='T0..T5'` · C4 **BVH 叶子 0 个仍查到 `Solid,Ghost`**（补扫的唯一硬证据）· C5 `attr` 默认值 `100/100`（写回生效）· C7 `quest_stage` 进 JSON、`zz_local_only` **不入 JSON**，读回 `stage=3` · C8 20000 次配额熔断、`148 ms` · C9~C12 `player/vel/grounded/visible` 与"已删实体报错"。 |

### S1 踩到的三个坑

1. **`ScriptProgram` 的默认构造不能 `= default` 写在头文件里**。`m_ast` 是
   `std::vector<unique_ptr<Stmt>>`，而 `Stmt` 只在 `.cpp` 里完整。默认构造一旦
   inline 生成，编译器为了异常展开要能析构"已构造的成员" → 任何在**别的 TU**
   里写 `ScriptProgram p;` 的地方都报 `can't delete an incomplete type`
   （实测落在 `ScriptSelfTest.cpp:143`）。构造与析构都必须 out-of-line。
2. **`let` 的幂等性会吃掉"再问一次"的测试**。B4 一开始写
   `let aliveBefore = exists(h)` + `let aliveAfter = exists(h)`，第二步整句被跳过，
   于是"删了实体"和"变量没变"两件事混在一起。**凡是要验"某一刻重新求值"的
   用例，第二句必须是普通赋值**，否则测的是声明而不是求值。
3. **语言里有 `Bool` 值却没有 `true`/`false` 字面量**。比较运算与 `exists()` 都
   产出 Bool，但写不出 `let flag = false` —— 它会被当成"未知变量 false"。
   S1 顺手补上（`Expr::Kind::Bool` + 标识符分支先行拦截），否则每个脚本都得
   写成 `let flag = 0` 再靠 `truthy()` 兜。

| 2026-10-19 | **S3 事件与输入 ✅** | `--selftest script` **68 条全绿**（A18+B18+C17+D15）。`on start/update/collide/trigger/exit` 五钩子在**编译期**收进 `Program::hooks()`；相位顺序写死在 `ScriptProgram::runFrame`：**声明区 → start → 接触事件 → 顶层 → update**；输入四件套走 `ScriptServices`（61 → 65 个内建）；`ScriptRuntime::postContact` + `Physics ContactEvent → ScriptContact` 翻译在 `RuntimeApp::pumpContactEvents`（用 `World::handle(id)` 补 generation）。实测：D0 `hooks=2 start,update` · D0b `n=101` · D1 `y=2.500` · D2 `held=3 after=3` · D3 `hitName='Wall'` · D4 `hits=3 exits=1` · D5 `seen='ready'` · D6 `order=12` · D7 `p=false ax=0.0` 且不报错 · D8 `starts=1` · D9/D9b/D9c 三种写错全编译失败 · D12 双方互为 `other`。**零回归**：`verify_hud` / `verify_audio` PASS。 |

| 2026-10-19 | **S5 编辑器可见 ✅** | `verify_script_editor.py` **21 条全绿**（F1~F6）。Scripts 面板加 Instances 页签（实体名 / 脚本 / 状态 / 本地时间 / 变量数 / Reset / 错误行），变量查看器（按名字排序），Inspector 的 Script 段加实例状态 + Reset，三条控制台命令（`script.reset` / `script.vars` / `script.trace`）。四个新环境钩子：`MYVK_EDITOR_PANELS`（按 id 开面板，绕开 Alt+6 的键盘状态问题）/ `MYVK_EDITOR_SCRIPT_TAB=instances` / `MYVK_EDITOR_CONSOLE_CMD_FRAME` / 保留 `MYVK_EDITOR_CONSOLE_CMD` 的 `;` 多值。实测：F1 三个锚点 `x=1238` 三行对齐 · F2 **Idle→Running 文本 + 像素双证 `Δ=36.42%`** · F3 `before=8 after=8`、无 RESTART · F4 `vars_after=0` · F5 `state=Failed` + 红字 `17.81%` + Reset 后 `reset_lines 1→2`、`Failed 1→2` · F6 `script.vars Boss` 打 8 行键值。**零回归**：`--selftest script` 85 条仍全绿。 |

### S5 踩到的五个坑

1. **编辑态实例表是空的 —— 面板一帧都画不出来**。`syncInstances()` 只在 `tick()`
   里调，而编辑态不执行脚本（S-D10），所以 Instances 页签永远是
   "(none)"。修法：`ScriptRuntime::sync()`（本来就写着"用途②：S5 的 Instances
   面板在编辑态枚举实例"，只是**从来没人调**）在 `EditorApp::onUpdate` 的
   非运行态分支里调一次。**"写了但没人调"和"没写"在症状上一模一样。**
2. **`reset()` 每帧调一次**。原代码在 `isPlaying()` 的 `else` 里无条件
   `m_scriptRuntime->reset()`，后果两个：① Pause 也算"非 Play" → 一暂停表就被
   丢，恢复时所有脚本从头再来（`wait` 挂在哪全丢，且**静默**）；② 编辑态表
   永远空。改成按 `usingRuntime()` 判（与丢弃物理世界同一个判据）+ 一个
   `m_scriptTableDropped` 位。
3. **`state=Failed` 那一行永远不会打印**。日志块写在循环**末尾**，而两条失败
   路径都在它之前 `continue`（熔断跳过 / 本帧执行失败置熔断）。抽成
   `noteState()`，四条路径共用。**"日志锚点写对了、但走不到"是日志类断言的
   头号假绿来源。**
4. **行太宽把 Reset 按钮推出窗口**。一行里塞了"实体名 + **完整绝对路径** +
   状态 + 时间 + 变量数 + 按钮"，面板只有 360 px：按钮被裁到 x=2021（窗口
   右边缘是 1588）。被 ImGui 裁掉的控件 `GetItemRect*` **照样返回真实坐标**，
   于是脚本拿着一个屏幕外的坐标去点，命中 0 次、日志 0 行，而面板看起来
   "按钮明明在"。改成两行（第一行只放名字/状态/按钮，第二行放文件名+时间+变量数）。
5. **`has_line` 做"点了有没有反应"的断言 = 永远通过**。F4 已经点过一次
   Reset，日志里那条早就在，F5c 的 `wait_line("SCR-RESET ok=1 …")` 立刻为真。
   改成 `wait_count(needle, before+1)` —— 要的是"**又多了一条**"。
6. （附）**`F2` 的像素双证要求四种状态四种颜色**。`Idle` 与 `Running` 都绿
   的话截图一样，像素断言只能靠"时间数字在跳"勉强过 —— 那是假证据。

| 2026-10-19 | **S6 示例关卡 + 基准 + 收口 ✅** | `verify_script.py` **23 条全绿**（G1~G7）。落地四个脚本（`assets/scripts/enemy_patrol|door_timer|pickup|hud_counter.vks`）+ 关卡生成器 `tools/gen_script_demo.py` → `assets/scenes/levels/script_demo.json`（13 实体：地面 / 玩家 / 3 巡逻兵 / 2 门 / 5 拾取物 / Director），**玩法逻辑一行 C++ 都没有**。`--selftest scriptbench` 新增：200 实例 × 100 语句/帧。实测：G1 巡逻区 `Δ=1.99%`、对照天空 `0.000%` · G2 `HUD-STATS health 100→90` · G3 两扇门 `Δt=2.0000s`（按时刻不是按帧）· G4 `picked 1..5` + `HUD-COUNT 5` · G5 退出存盘 `scriptVars={'picked':5.0}`、`scriptVars` 全篇只出现 1 次、读回后 `PICKUP … 6` 与 `… 10` · G6 `VKS-RELOAD-KEEP`、时间单调、`max_jump=1.250 m` · G7 `ms=94.3`（预算 200，Debug /Od）。**零回归**：`--selftest script` 85 条仍全绿。 |

### S6 踩到的五个坑（含三处「与原文的差异」）

1. **`add_custom_command(TARGET … POST_BUILD)` 拷资产 = 改了资产不生效**。
   POST_BUILD 只在**重链**后跑，而"改关卡 JSON / 改 .vks"恰恰是不重链的那
   一类改动 —— 直接跑测试会拿到上一份，**且没有任何提示**。改成
   `add_custom_target(... ALL)` + `add_dependencies`。W3/W4 一直侥幸没踩到，
   只因为那时总在重链。
2. **巡逻兵摆在 -X 侧 = 全部在相机背后**。`Player` 朝向 +X，弹簧臂挂在身后，
   于是 home=(-6/‑14) 的三个兵一个都看不见。日志里一条错都没有，
   **只有截图看得出**。G1 现在先断言"画面里确实有东西在动"，再谈动得对不对。
3. **`on collide` 做不出玩法**（这是**差异**，不是坑）：① 编辑器的 Play 态没有
   `ContactTracker`（S3 台账第 4 条，一直没补）；② 玩家的角色代理是
   **运动学**体，而 `PhysicsWorld::collectContacts` 会把"两边都不会动"的接触对
   整对跳过 —— 运动学 vs 运动学撞不出事件。改用**距离判定**后同一份脚本在
   `Game.exe` 与编辑器里行为逐位相同，正好是 §8 那条总验收要的东西。
   contact 派发本身留给"编辑器 Play 接 ContactTracker"那一项。
4. **「1 个地形」换成了平地**（差异）：`engine/terrain` 的高度场**没有接进物理**
   （`runtime/` 与 `editor/` 里搜不到 `Heightfield`），角色胶囊会直接掉到
   z=0，而 `terrain/hills` 在原点附近高 11~17 m —— 整关的玩法判定会全部失真。
   等 W5 之后地形物理接通，把 `Ground` 换成地形实体即可，脚本一行都不用改。
5. **「巡逻兵位置不保」实测不成立**（差异，判据已改）：Transform 是普通场景
   数据，脚本每帧写的就是它，所以位置**会**存；真正保不住的是**脚本实例状态**
   （`taken` / `hp` / 巡逻进度）。G5e 因此改成同一件事的可观测形式：读回后
   5 个拾取物**各自又被捡了一次**（`picked 5→10`），而世界黑板从 5 接着走。
6. （附）**`HUD-STATS` 是每 60 帧一行的节流日志** —— 扣血那一刻可能正好落在
   两次采样之间。第一版用 `sleep(1)` 后读一把，偶发红；改成轮询"直到看见
   `health<=90`"。

### S4 踩到的五个坑

1. **`ScriptCoroutine` 一开始写进了匿名命名空间** → 与头文件的 `vks::ScriptCoroutine`
   变成两个类型（C2872）。和 `Expr`/`Stmt` 同一个坑，第二次踩：**只要头文件里有
   前向声明，定义就必须在 `namespace vks` 里、匿名命名空间之外**。
2. **`resetInstance` 没清协程**（真 bug，E12 抓到的）：Reset 之后实例仍挂在
   旧的 `waitAt=100s` 上，界面"重置成功"、脚本一动不动。Reset = 换一个空协程。
3. **控制流语句必须先 `++ip` 再压子帧**。顺序反了，挂起时父帧 ip 还指着那条
   if/while，恢复后整条控制流重跑一遍 —— 表现是"wait 之后无限循环"。
4. **`f` 引用在 `push_back` 后会悬垂**（vector 重分配）。`stepWhile`/`stepForeach`
   里所有要用的字段必须先拷成局部变量再用。这条错误在 Debug 下不崩、在 Release
   下随机错，最难查。
5. **`simFrame` 是 1-based**：tick 里先 `++inst.simFrames` 再填 ctx，所以首帧看到
   的是 1。`waitFrames(5)` 从 1 等 5 步 → 在 6 恢复（E8 一开始按 5 写，红了一次）。

### S3 的四处「按实测调整」（与 §7 原文的差异，必须记账）

1. **多了「声明区」这一相位**：原顺序是 ① start → ③ 顶层，而 `let hp = 100`
   属于顶层 —— 照字面跑，`on start` 里读 `hp` 就是"未定义变量"（D0b/D8 两次
   实测红）。于是编译期把**文件开头连续的 `let`** 切出来（`m_declCount`），
   在 start **之前**先落地。只切开头那一段，中间夹了别的语句之后的 `let`
   留在原位 —— 这样"先赋值后 let"也不会因分区改语义。
2. **`on start` 里"创建实体"没做**（D5 的原始判据）。刷怪需要 `spawn()` /
   预制体，而 §5.4 的函数表里没有它；S3 改成验"start 写的状态同帧 update
   可见"（同一件事：相位顺序）。真要刷怪，S6 做示例关卡时再定。
3. **`on collide` 里写 `wait` 的静态断言推迟到 S4**（D6）。`wait` 本身在 S4
   才存在，S3 阶段写它只会得到"unknown function 'wait'"—— 那不算验证 S-D9。
4. **编辑器 Play 态暂不派发接触事件**：`EditorApp` 里根本没有
   `ContactTracker`（那套只在 runtime 的 `onFixedUpdate` 里）。编辑器的
   玩家/输入已接（用"第一个带 `CharacterControllerComponent` 的实体"当玩家），
   collide/trigger/exit 只在 `Game.exe` 里生效 —— 这一条进 S5 的待办。

### S2 踩到的四个坑

1. **`let` 是声明、不是赋值 —— 自检里"第二步重新求值"必须用普通赋值**。
   （B4 与 C 组反复踩：`let aliveAfter = exists(h)` 第二步整句被跳过，测出来
   的是"变量没变"而不是"实体没了"。）同一条纪律在 S1 已记一次，S2 又踩一次。
2. **`ScriptProgram` 的默认构造不能 `= default` 写在头文件**（S1 的坑 1 的
   另一半）：`m_ast` 是 `unique_ptr<Stmt>`，`Stmt` 只在 `.cpp` 里完整，任何一个
   新 TU 里写 `ScriptProgram p;` 都报 `can't delete an incomplete type`。
   构造与析构都要 out-of-line。
3. **增量构建会留下 `LNK1163: COMDAT 选择无效`**。改过带模板/内联函数的结构体
   （`ScriptServices` 变过布局）之后，旧 `.obj` 与新 `.obj` 混链会报这个 ——
   删掉报错的那个 `.obj` 重编即可，**不是代码错**。
4. **`near` 用 BVH 时"一次 run 最多 refit 一次"是硬要求**。`spatialIndex()`
   每次调用都要把全场景世界矩阵 × 8 角点算一遍；放进 `near` 里直接调用，
   "循环里查 near"就变成"循环里 refit 全场景"，比朴素全扫还慢。用
   `m_indexFresh` 挡，任何 `setPos/setRot` 把它置回 false。

### S0 踩到的三个坑

1. **`Value` 必须搬到头文件**。它原来住在 `ScriptEngine.cpp` 的**匿名命名空间**
   里 —— 而 `ScriptInstance::vars` 要跨编译单元持有它。搬的时候顺手踩了第二个坑：
   `std::string Value::toStr()` 的实现如果还留在 `namespace {` 里，编译器会认成
   "另一个类的成员"，报 **C2888**（`cannot define member of vks::Value in
   anonymous-namespace`）。定义必须挪到匿名命名空间**外面**。
2. **不能用指针比较检测热重载**。`ScriptEngine::clear()` 会连 `ScriptProgram`
   一起释放，重编译时 allocator **完全可能把新对象放在同一个地址** → 指针相等
   → 误判"没变" → 熔断的实例永远不解除，而日志上一切正常。改用
   `ScriptAsset::version()`（成功编译才前进的全局单调序号）。
   **"改了但没编译过"时版本号不动**，正好对上 S-D8 的"热重载成功才解熔断"。
3. **`prints` 挂在 `ScriptProgram` 上是错的**。Program 按路径共享，两个实体跑
   同一脚本时，第二个的 `run()` 会 `clear()` 掉第一个的输出。两份宿主循环
   各自"跑完立刻读"把这个 bug 盖了一年。S0 把它挪进 `ScriptRunContext`。

> 每完成一项，在此追加「实测数字 + 踩到的坑」——与 `audio_roadmap.md` §13 /
> `world_scale_roadmap.md` §19 同款。
