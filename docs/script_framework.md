# 游戏逻辑框架：C++ 定契约，Lua 写行为

> 定稿 2026-11-03。这是**要照着写代码的那份**——不是评估，是契约。
>
> 前置：`code_env_2026-11-03.md`（为什么换）· `lua_cpp_split.md`（谁管哪一半）。
> 本文定的是：**Lua 侧看到什么样的 API、怎么被调用、出错怎么办、怎么扩**。

---

## 0 · 一句话

> **C++ 定义「能做什么」，Lua 决定「什么时候做」。**
>
> 和 UE5 的 `UCLASS/UPROPERTY/UFUNCTION` + 蓝图关系**一模一样**：
> C++ 是契约，Lua 是编排。区别只是本引擎把它做小、做薄、做成一张表。

---

## 1 · 三层结构（UE5 对照）

```
┌──────────────────────────────────────────────────────────────────────┐
│ L3  Lua 脚本（用户写）                                                │
│     enemy_patrol.lua —— 一张表 + 几个方法，return 出去                │
└──────────────────────────┬───────────────────────────────────────────┘
                           │  只能看见 12 个命名空间里的函数
┌──────────────────────────▼───────────────────────────────────────────┐
│ L2  API 契约表（C++ 里唯一定义，engine/script/ApiTable.cpp）           │
│     { "world.pos", 1, 1, "entity -> vec3", "返回实体世界坐标" }        │
│     ↓ 自动生成两样东西：                                               │
│       · 命名空间表 + Lua 绑定 + 参数个数校验                           │
│       · docs/script_api_reference.md（`--selftest api` 原样打印）      │
│     ⚠ `.vks` 的 `kBuiltins` **不是**从这张表生成的 —— 它按位置对齐     │
│       `enum class Builtin`，是另一份事实源（阶段 D 之后才会退场）      │
└──────────────────────────┬───────────────────────────────────────────┘
                           │  每个函数转发到 std::function
┌──────────────────────────▼───────────────────────────────────────────┐
│ L1  宿主能力（ScriptHost / ScriptServices / ScriptWorldActions）       │
│     全都可能是空的 —— 空 = 该动作静默无效，不是崩溃                    │
└──────────────────────────────────────────────────────────────────────┘
```

| 层 | UE5 的对应物 | 本引擎的差异 |
|---|---|---|
| L3 | 蓝图图表 | **写文本，不做节点图**（成本差一个数量级） |
| L2 | `UHT` 生成的反射表 + `BlueprintCallable` 标记 | **一张手写表**，因为本引擎没有反射 |
| L1 | `UObject` 虚函数 / `UFUNCTION(BlueprintImplementableEvent)` | `std::function` 窄动作 |

⚠ **L2 是这套设计的全部价值所在**。现在加一个内建要改 5 处（enum / `kBuiltins` /
`call` 分支 / 头注释 / 文档），而且 `docs/script_api.md` 是手抄的、会和实现漂移。
改成一张表之后：**加一行 = 加一个 API**，另外三处自动派生。

---

## 2 · Lua 侧看到什么：12 个命名空间

不搞"103 个全局函数"。按**域**分表，理由是补全与查文档时能按域找。

**实际交付：106 个函数 / 12 个命名空间**（下表是落地后的真实数字，
权威来源是 `engine/script/ApiTable.cpp`；文档 `docs/script_api_reference.md`
由 `Game.exe --selftest api` 从那张表生成；逐组件覆盖见 §2.2）。

| 命名空间 | 管什么 | 函数数 | 典型 |
|---|---|---|---|
| `engine` | 自身、值、时间、日志 | 13 | `engine.self()` `engine.vec3(1,0,0)` `engine.dt` |
| `world` | 场景查询、变换、**层级** | 18 | `world.pos(e)` `world.setPos(e,v)` `world.setParent(e,p)` `world.near(p,5)` |
| `actor` | 可见 / 材质 / 刚体 | 10 | `actor.setVisible(e,false)` `actor.setColor(e,r,g,b)` `actor.impulse(e,v)` |
| `input` | 动作输入（不是键码） | 4 | `input.pressed("Jump")` `input.axis("MoveX")` |
| `anim` | 播放 / 控制 / 形态键 / 表情 | 10 | `anim.play(e,"Run")` `anim.speed(e,0.3)` `anim.morph(e,"smile",1)` |
| `nav` | 导航 | 4 | `nav.moveTo(e, x,y,z)` `nav.arrived(e)` `nav.failed(e)` |
| `fx` | 特效与镜头 | 5 | `fx.play("hit")` `fx.shake(0.3)` `fx.hitstop(0.05)` |
| `audio` | 音频 | 3 | `audio.play("click.wav")` `audio.bus("music", 0.4)` |
| `light` | 灯光 | 5 | `light.color(e,r,g,b)` `light.intensity(e,2)` `light.enabled(e,false)` |
| `camera` | 相机（FOV / 臂长 / 机位） | 3 | `camera.fov(cam,70)` `camera.enabled(cam,false)` |
| `ui` | HUD / 对话 / 过场 | 6 | `ui.health(80)` `ui.popup(e,10)` `ui.dialogue("ch01")` |
| `game` | 玩法数据（属性/状态/配表/卡牌/旗标/关节） | 25 | `game.attr(e,"hp")` `game.tableNum("cards.a.cost")` `game.flag("win", true)` |

⚠ **`engine.dt` / `engine.time` / `engine.frame` 是字段不是函数**（每帧读，函数调用不值）。

**为什么不是 `UObject` 式的方法调用**（`e:GetTransform():SetLocation()`）：

UE 之所以那样，是因为它有反射和对象系统；本引擎的 `entity` 只是 `{id, generation}`
四个字节的句柄，**没有对象**。做成 `world.pos(e)` 这种"函数 + 句柄"的形式，有两个好处：

1. **不会让人以为持有对象** —— 句柄失效（实体被删）时 `engine.valid(e)` 立刻为假，
   `world.pos(e)` 会**报错**（`luaL_error` → 实例熔断）而不是悄悄返回一个假位置；
2. **批量化容易** —— `world.near()` 一次拿一批，比 `for each actor` 快一个数量级。

### 2.1 实体的 Lua 表示

**64 位整数打包**：`((generation << 32) | id) + 1`，**空句柄 = `0`**。理由：

- Lua 5.4 的 integer 是 64 位，**不用造 userdata**；
- 可比较、可作表键、可存数组；
- `generation` 那一半**不能丢**——丢了就是"实体被删后句柄复用，脚本静默改到别人身上"
  （`.vks` 的 `Value::K::Entity` 注释里踩过这个坑）；取参数时也**必须按 64 位取**
  （`luaL_checkinteger` → `uint64_t`），走 `int` 等于把 generation 截掉；
- **为什么要 +1**：不加的话 `id==0 && gen==0` 也是 0，"0 号实体"和"没有实体"
  撞在同一个数字上 —— 文档里那句 `setParent(kid, 0) -- 拔到根` 会静默把子节点
  挂到 0 号实体（而 0 号常常就是玩家）身上。加一之后 0 只表示空，
  `world.find` 没找到 / `world.parent(根)` / `game.jointTarget` 没有另一端，
  一律返回 `0`。

```lua
local e = engine.self()      -- 整数
if engine.valid(e) then      -- 句柄还活着吗
    world.setPos(e, engine.vec3(1,0,0))
end
```

⚠ `engine.valid()` 判空是**必须**的：`world.*` / `actor.*` 系列拿到失效句柄
**一律报错**（`"entity #N is not alive"`）并让实例熔断，**不静默返回 nil** ——
静默是这个引擎吃过最多亏的地方（"东西被搬到原点"会离现场很远）。

---

### 2.2 组件覆盖矩阵（47 个组件逐条核对）

这张表回答的是**"引擎里有的东西，脚本够不够得着"**。判据不是"有没有同名函数"，
而是**玩法作者能不能用它写出想写的东西**。

四类：

| 记号 | 含义 |
|---|---|
| ✅ | 玩法需要，且读/写都能用（写进去下一帧生效） |
| ◐ | 有 API，但受引擎既有语义限制（共享资产、只有播放没有暂停、逐层不可控 …） |
| ✕ | **有意不开放** —— 编辑器/美术/序列化内部，或真相源不由脚本决定 |
| 🚧 | **该开放但开不了** —— 需要先改引擎；这类才叫真缺口 |

| # | 组件 | 脚本入口 | 状态 |
|---|---|---|---|
| 1 | `NameComponent` | `engine.name` / `world.setName` / `world.find` | ✅ |
| 2 | `TransformComponent` | `world.pos/setPos/rot/setRot/rotY/scale/setScale/moveBy` | ✅ |
| 3 | `HierarchyComponent` | `world.setParent/parent/children` **（本轮新增）** | ✅ |
| 4 | `MeshComponent` | 只有 `world.spawn`（换整个实体） | ◐ 换网格**资产**没有 API；要做需宿主加载 + 守渲染资源生命周期 |
| 5 | `MaterialComponent` | `actor.color/setColor/setEmissive` | ◐ 材质**按名共享**，逐实体副本没有 API |
| 6 | `SkinnedMeshComponent` | `anim.pause/resume/stop/speed/time/clip` 的权威字段 | ✅ |
| 7 | `MorphComponent` | `anim.morph/morphWeight` **（本轮新增）** | ✅ |
| 8 | `ExpressionComponent` | `anim.expression` **（本轮新增）** | ✅ |
| 9 | `AnimatorComponent` | `anim.play/pause/resume/stop/speed/time/clip` | ◐ 逐层/NLA、`rootMotion` 模式、状态机参数不在脚本内 |
| 10 | `SequencerComponent` | `ui.sequence` | ◐ 只能"起播"，不能暂停/跳转/改循环 |
| 11 | `DialogueComponent` | `ui.dialogue` | ◐ 只能"起播"，不能推进/选项 |
| 12 | `AnimKeyMapComponent` | — | ✕ 编辑器映射表 |
| 13 | `AnimationClipComponent` | — | ✕ 旧数据通道 |
| 14 | `CollisionComponent` | — | 🚧 `shape/solid` 在建物理世界时**一次性烘焙** |
| 15 | `RigidBodyComponent` | `actor.vel/setVel/impulse/grounded` | ◐ `bodyType/useGravity/mass` 改字段**不生效**（烘焙） |
| 16 | `JointComponent` | `game.joint*`（九件） | ✅ |
| 17 | `CharacterControllerComponent` | 走 `nav.*`（NPC） | ✕ 玩家位移的真相源是输入 + 控制器，不由脚本决定 |
| 18 | `NavAgentComponent` | `nav.moveTo/arrived/failed/stop` | ✅ |
| 19 | `TriggerComponent` | — | 🚧 `enabled` 烘焙成 sensor 标志，运行期改无效 |
| 20 | `PhysicsMaterialComponent` | — | 🚧 构建期烘焙 |
| 21 | `CameraComponent` | `camera.fov/distance/enabled` + `fx.shake` | ◐ `priority`、DOF、手持、快门角不在脚本内 |
| 22 | `CameraPathComponent` | 由 `ui.sequence` 间接驱动 | ✕ 演出资产 |
| 23 | `VisibilityComponent` | `actor.visible/setVisible/setShadow` | ✅ |
| 24 | `DecalComponent` | — | ✕ 美术资产（要淡出贴花得另开一个 `actor.decalOpacity`） |
| 25 | `PlayerComponent` | — | ✕ 移动归 `PlayerSystem` + 输入；要"让玩家走过去"请用演出 |
| 26 | `PlayerLimbComponent` | — | ✕ 程序化四肢，渲染细节 |
| 27 | `LockedComponent` | — | ✕ 编辑器锁 |
| 28 | `FxHostComponent` | `fx.play/fx.stop`（走 EffectService 池） | ✅ |
| 29 | `DirectionalLightComponent` | `light.color/intensity/direction` | ◐ **没有** `enabled`（要关太阳用 `intensity = 0`） |
| 30 | `PointLightComponent` | `light.color/intensity/range/enabled` | ✅ |
| 31 | `SpotLightComponent` | 以上 + `light.direction` | ✅ |
| 32 | `ScriptComponent` | — | ✕ 运行期挂/换脚本要等阶段 D 的 `ScriptRuntime` 分派 |
| 33 | `AudioSourceComponent` | `audio.play/stopAll/bus`（全局一次性音效） | ◐ 3D 音源的 play/stop/volume 没有入口（宿主没开 `AudioSystem` 句柄） |
| 34 | `AudioListenerComponent` | — | ✕ 单例 |
| 35 | `ReflectionProbeComponent` | — | ✕ 美术资产 |
| 36 | `ParticleEmitterComponent` | `fx.play`（资产级） | ◐ emitter 的 `rate/enabled` 没有 API |
| 37 | `ScriptVarsComponent` | `game.attr/setAttr` | ✅ |
| 38 | `TerrainComponent` | — | ✕ 刷地形是编辑器的事 |
| 39 | `WaterComponent` | — | ✕ 同上 |
| 40 | `GameStartComponent` | — | ✕ 出生点是场景配置 |
| 41 | `SpringBoneComponent` | — | ✕ 运行期物理，参数由美术调 |
| 42 | `LookAtComponent` | — | ✕ 约束参数（组件没有"目标实体"字段，换目标无从谈起） |
| 43 | `TwoBoneIKComponent` | — | ✕ 同上 |
| 44 | `BoneAttachmentComponent` | — | 🚧 骨级挂点；`world.setParent` 只能挂**实体** |
| 45 | `PrefabLinkComponent` | — | ✕ 序列化内部（prefab 实例身份） |
| 46 | `GameplayComponent` | `game.stat/hasStat/damage/status/round` | ✅ |
| 47 | `CardDeckComponent` | `game.castCard/drawCards` | ✅ |

**合计：✅ 15 · ◐ 10 · ✕ 18 · 🚧 4**。

#### 四个真缺口，各自卡在哪

1. **物理运行期开关**（#14 / #15 / #19 / #20）
   症状：想"开门时让墙可穿过""把尸体冻住""触发过的陷阱不再触发"，一个都写不出来。
   卡点：`ScenePhysics::buildSceneWorld` 把 `bodyType / useGravity / solid / enabled`
   一次性写进刚体，之后**没有任何代码再读组件**。所以现在加
   `actor.setKinematic / setGravity / trigger` 只会是"调了没反应"。
   要开需要三处一起改：`PhysicsWorld` 加 `setMotionType / setGravityFactor / setSensor`
   → `ScenePhysics` 加按实体转发 → `ScriptWorldActions` 加 3 个动作（运行时与编辑器两个宿主都要接）。
   ⚠ **不是加 3 个绑定就完事**，所以这一轮没做。

2. **骨级挂点**（#44）
   症状：武器要跟着手骨动。现在只能用 `world.setParent(武器, 角色实体)`，跟的是**实体**不是骨。
   卡点：`BoneAttachmentComponent::hostName` 是**字符串**，由系统在构建期按名字解析成骨索引。
   运行期换宿主需要该系统开放"重建绑定"入口。

3. **运行期换网格资产**（#4）
   症状：同一个实体换模型（断肢、换装、开关门）。
   卡点：`MeshComponent::mesh` 是 `assets::Mesh*`，换它要经 `AssetManager` 拿新指针，
   并且旧网格不能在本帧 recording 中被引用时销毁（见 `docs/render_resource_lifetime.md` 铁律）。
   脚本层不该直接碰资产指针 —— 需要宿主开一个 `act.setMesh(e, path)`。

4. **运行期挂/换脚本**（#32）
   卡死点：`ScriptRuntime` 目前只认 `.vks`（阶段 D）。这条不是"要不要"，是"还没轮到"。

#### 为什么 ✕ 这么多（18 个）

因为它们**不是玩法需要的东西**：
- **美术资产**（贴花、反射探针、地形、水面、弹簧骨）：改了也不是玩法；
- **编辑器/序列化内部**（`AnimKeyMap`、`AnimationClip` 旧通道、`PrefabLink`、`Locked`）；
- **单例/渲染细节**（`AudioListener`、`PlayerLimb`）；
- **真相源不在脚本**（`PlayerComponent` 的移动由输入决定、`GameStart` 的出生点由场景决定、
  `CameraPath` 由 Sequencer 决定）。给它们开 API 只会让"谁最后写谁赢"变成偶发 bug。

⚠ 判据不是"组件里有字段就开一个函数"。**能改 ≠ 该改**：
`CameraComponent.fov` 该开（视角是表现力），`PlayerComponent.moveSpeed` 不该开
（那是策划在 Inspector 里调的数值，脚本每帧改它会和角色控制器打架）。

## 3 · 生命周期：一张表就是一个脚本实例

### 3.1 脚本形态

```lua
-- assets/scripts/enemy_patrol.lua
local Enemy = {}

Enemy.hp       = 100        -- 字段 = 实例状态（不用 let，不用黑板）
Enemy.home     = nil        -- 运行时才知道，BeginPlay 里填

function Enemy:BeginPlay()
    self.home = world.pos(self.entity)
end

function Enemy:Tick(dt)
    local p = world.pos(self.entity)
    if p.x > self.home.x + 6 then self.dir = -1 end
    if p.x < self.home.x - 6 then self.dir =  1 end
    world.setPos(self.entity, engine.vec3(p.x + (self.dir or 1) * 2.5 * dt, p.y, p.z))

    if self.hp <= 0 then return end
    if world.dist(p, world.pos(world.player())) < 1.6 then
        self.hp = self.hp - 10
        ui.health(self.hp)
        ui.popup(self.entity, 10)
        engine.log("PATROL-HIT", self.hp)
    end
end

function Enemy:OnCollide(other) end
function Enemy:OnTrigger(other) end
function Enemy:EndPlay() end

return Enemy
```

四条规则，背下来就够：

| # | 规则 | 为什么 |
|---|---|---|
| 1 | **`return` 一张表** | 这张表就是实例状态；热重载时**整表保留**（见 §5） |
| 2 | **方法用 `:`**（第一个参数是 `self`） | `self` 是那张表；`self.entity` 是本实例的实体句柄（引擎注入） |
| 3 | **没写的方法 = 不调用** | `Tick` 不写就不 tick（等价 UE 的 `BlueprintImplementableEvent` 没实现） |
| 4 | **表里不要放函数以外的东西**（不要嵌套大表） | 热重载会保留整表，嵌套表里的旧闭包会悬空 |

### 3.2 钩子对照（`.vks` → Lua）

| `.vks` | Lua | 何时调用 |
|---|---|---|
| `on start { }` | `function M:BeginPlay()` | 实例第一次 tick 之前，**只一次** |
| `on update { }` | `function M:Tick(dt)` | 每个固定步 |
| `on collide(other) { }` | `function M:OnCollide(other)` | 接触 Enter |
| `on trigger(other) { }` | `function M:OnTrigger(other)` | 进入触发区 |
| `on exit(other) { }` | `function M:OnExit(other)` | 离开触发区 |
| （无） | `function M:EndPlay()` | 实体销毁 / Stop 之前 |
| 顶层语句（每帧跑） | ❌ **不支持** | 顶层只允许声明；每帧逻辑必须写在 `Tick` 里 |

⚠ 最后一行是**唯一的不兼容点**：`audio_play.vks` / `hud_damage.vks` 那种"顶层 = 每帧"的
写法在 Lua 里要包进 `Tick`。迁移表见 §7——这两个脚本各 3 行，改起来 30 秒。

### 3.3 场景级脚本（无实体）

管波次、胜负、关卡流程的脚本**不该挂在一个假实体上**：

```lua
-- assets/scripts/level_01.lua
local Level = { wave = 0 }

function Level:BeginPlay()
    self.timer = 0
end

function Level:Tick(dt)
    self.timer = self.timer + dt
    if self.timer > 30 and self.wave < 3 then
        self.wave = self.wave + 1
        spawnWave(self.wave)          -- 自己的函数，Lua 天然支持
    end
    if game.flag("boss_dead") then
        ui.dialogue("ch01_clear")
    end
end

return Level
```

挂载方式：场景文件里的 `sceneScripts: ["scripts/level_01.lua"]`。
这类实例 `self.entity == 0`（无效句柄），任何 `world.pos(self.entity)` 都会被拒并 WARN。

---

## 4 · 协程：`wait` 是函数，不是语句

`.vks` 的 `wait` 是**语句**（`Stmt::Kind::Wait`，要推进 ip 再挂起）。
Lua 里没有这个机制——`wait` 必须是**协程里的 yield**：

```lua
function Door:Open()
    actor.setVisible(self.entity, true)
    engine.wait(2.0)                     -- ← 挂起 2 秒，不阻塞帧、不阻塞别的脚本
    actor.setVisible(self.entity, false)
end
```

### 实现方式（Unity / UE Latent Action 同款）

```
引擎调 M:Tick(dt)
   └─ 若 M:Tick 是"普通函数" → lua_pcall，跑完就返回
   └─ 若它 yield 了（或它在协程里被调）→ 存下协程，每帧 lua_resume(co, dt) 继续
```

具体规则：

| 规则 | 说明 |
|---|---|
| **任何脚本方法都可以 `wait`** | 引擎用 `lua_resume` 调方法（先包成协程），yield 了就记账 |
| **`engine.wait(sec)` 返回**当前剩余时间（被 resume 时传入） | `local left = engine.wait(2.0)` |
| **`engine.waitFrames(n)`** | 同上，按固定步计数 |
| **`engine.waitUntil(fn)`** | 每帧调 `fn()`，返回真才继续 |
| **实例销毁时协程直接丢** | 不 resume、不报错（等价 `EndPlay` 后的清理） |
| ⚠ **`Tick` 里 `wait` 会中断本帧剩余逻辑** | 这是对的（和 `.vks` 一致），但要写清楚 |

⚠ **协程不是线程**。`wait` 期间实例的 `Tick` 也不会被再次调用——
一个 `Tick` 挂起后，下一帧 resume 的还是那个 `Tick`。这是 Unity 的语义，别和"每帧 Tick + 定时器"混。

---

## 5 · 热重载：变量保留、函数替换、协程重启

| 保留什么 | 怎么做 |
|---|---|
| **实例字段**（`self.hp` / `self.home`） | 就是那张表 → **整表不换**，只换方法 |
| **方法** | 重新 `load` chunk → 新表的方法覆盖旧的 |
| **协程** | 旧协程的 pc 指向旧函数 → **只能重启那个协程**（不是整个实例） |
| **表里嵌套的表** | 不保留（里面可能挂着旧闭包）→ 文档里写"不要嵌套" |

实现：新 chunk 跑完后拿到 `newTable`，对每个 key **只覆盖函数**，非函数字段**不动**；
`entity` 由引擎重新注入。

⚠ **比 `.vks` 弱一档**：`.vks` 靠 `Stmt::id` 能"改字面量不变、从原位置继续"，
Lua 做不到语句级续跑。**这是换 Lua 的既定代价**，写在这里免得以后当 bug 查。

---

## 6 · 错误、熔断、确定性

### 6.1 错误分三类（面板要分颜色）

| 类型 | 什么时候 | 处理 |
|---|---|---|
| **编译错**（语法/加载） | 加载或热重载时 | 保留上一版继续跑 + 面板红字 + **行号**（`lua_tostring` 带 `file:line`） |
| **运行期错**（`lua_pcall` 非 0） | 执行中 | 打日志 + 本实例 `failed`，**后续帧不再执行**（等价 `.vks` 的 P4） |
| **预算超限**（count hook） | 死循环 | 同上，且错误信息里带 `budget` 字样 |

### 6.2 熔断必须靠 count hook

```cpp
// 每 1000 条 VM 指令查一次
lua_sethook(L, budgetHook, LUA_MASKCOUNT, 1000);
// 超了 → luaL_error("budget exceeded") → lua_pcall 返回非 0 → 本实例 failed
```

⚠ **不做这一步，一个 `while true do end` 会冻住整个游戏**——现在 `.vks` 的
"死循环 3 帧内停、只停一个实例"这条纪律会直接丢掉。这是换 Lua 最实在的代价。

### 6.3 两个必须守的边界

| 边界 | 规则 |
|---|---|
| **C++ 异常不能穿过 Lua** | 绑定层每个 C++ 调用都 `try { ... } catch(...) { return luaL_error(L, "%s", e.what()); }`。`lua_error` 是 `longjmp`，穿过有析构函数的 C++ 栈帧 = UB |
| **Lua 错误不能穿过 C++** | 一律 `lua_pcall` / `lua_resume`，绝不裸调 `lua_call`（崩了没有保护） |

### 6.4 确定性

| 项 | 规则 |
|---|---|
| 随机数 | `game.random()` → `GameRandom`（C++，有种子）。**不注册 `math.random`** |
| 时间 | `engine.dt` / `engine.time`，**不注册 `os` 库** |
| 遍历 | **不注册 `pairs`**（或注册成"报错版本"）；数组用 `ipairs`，集合用 `world.near()` 的有序结果 |
| 输出 | `engine.log()` 走 C++ 日志，不在 Lua 里拼时间戳 |

白名单：`luaL_openlibs` **不调**，只开 `base`（去掉 `dofile`/`loadfile`/`collectgarbage`）、
`coroutine`、`table`、`string`、`math`（去掉 `random`/`randomseed`）、`utf8`。
**`io` / `os` / `debug` / `package` 一律不开。**

---

## 7 · `.vks` → Lua 迁移对照

### 7.1 语法级

| `.vks` | Lua |
|---|---|
| `let hp = 100` | `M.hp = 100` |
| `hp = hp - 10` | `self.hp = self.hp - 10` |
| `if (a && b) { }` | `if a and b then end` |
| `while (c) { }` | `while c do end` |
| `foreach (e in near(p, 5)) { }` | `for _, e in ipairs(world.near(p, 5)) do end` |
| `pos(self())` | `world.pos(self.entity)` |
| `setPos(e, vec3(x,y,z))` | `world.setPos(e, engine.vec3(x,y,z))` |
| `attr(self(), "hp", 0)` | `game.attr(self.entity, "hp", 0)` |
| `print("x", v)` | `engine.log("x", v)` 或 `print("x", v)`（保留 `print`） |
| `wait(2.0)`（语句） | `engine.wait(2.0)`（协程里） |
| `damage(10)` | `ui.health_relative(-10)` 或 `ui.damage(10)`（语义见下） |

⚠ **`damage` 的歧义**：`.vks` 里 `damage(n)` 是 **HUD 扣血**（U8），
而 P2-6 的"玩法伤害"是 `addStatus` 那条线。Lua 里**拆成两个名字**：
`ui.damage(n)`（HUD）与 `game.damage(e, n, "physical")`（玩法）。这个歧义在
`kBuiltins` 里靠 arity 分流，很难读——**Lua 侧正好借机修掉**。

### 7.2 资产级

| 文件 | 迁移 |
|---|---|
| `assets/scripts/enemy_patrol.vks` | ✅ 值得迁（40 行，有 `hp`/`home` 跨帧状态） |
| `assets/scripts/door_timer.vks` | ✅ 值得迁（`wait` 的正面案例） |
| `assets/scripts/pickup.vks` | ✅ 值得迁 |
| `assets/scripts/hud_counter.vks` | ✅ 值得迁 |
| `assets/scenes/runtime_test/audio_play.vks` | ⚠ 顶层每帧 → 包进 `Tick` |
| `assets/scenes/runtime_test/hud_damage.vks` | ⚠ 同上 |

---

## 8 · 扩展：怎么加一个新的 API

**改两处，派生三处。**（这条流程本身跑过 19 次，见 §9.3）

```cpp
// ① engine/script/ApiTable.cpp —— 唯一事实源。加一行：
{"camera", "fov", 1, 2, "entity [, number] -> number",
 "读/写视野角（**度**）",
 "⚠ 单位是度，和 world.rot 的弧度不一样"},

// ② engine/script/LuaBackend.cpp —— 把 (ns, name) 接到实现上：
{"camera", "fov", l_camera_fov},        // kImpls[]，按 ns/name 字典序插
int l_camera_fov(lua_State* L) { ... lua_pushnumber(L, ...); return 1; }
```

| 要做的事 | 谁管 |
|---|---|
| 名字 / 参数个数 / 签名 / 文档 / 注意事项 | ✅ `ApiTable.cpp`（**不认识 Lua**，只有 `const char*` 和整数） |
| 实现函数 + `kImpls[]` 一行 | ❌ 手写（`LuaBackend.cpp`） |
| 命名空间表（`camera` / `world` …） | ✅ 从表里出现过的 ns 生成，不用手建 |
| 参数个数校验（`min/max`） | ✅ 注册时按表校验 |
| `docs/script_api_reference.md` | ✅ `Game.exe --selftest api` 从表原样打印 |
| `docs/script_api.md` 的域语义 | ❌ 手写（生成不出来的是"为什么"） |
| G 组覆盖（106 个全调一遍） | ❌ 手加一行调用 |
| **新开命名空间时 `kNsInfo[]` 的标题** | ❌ 手加 —— 漏了只会让文档里那一段标题变空，**不报错** |

**缺实现的后果是响的**：`installBindings()` 逐条查表，缺一条就打
`LUA-API missing implementation: ns.name` 并计数，最后打
`LUA-API N of M entries have no implementation`。
所以不可能出现"文档里有、脚本里调不到"。

⚠⚠ **三条硬规矩**（都是踩过的）：

1. **名字不能撞 Lua 保留字**。`nav.goto` 语法上不存在（`goto` 是 5.4 保留字），
   表里有、文档有、一编译就 red。已改 `nav.moveTo`。
2. **错误信息里不能用 `%u`**。`luaL_error` 走 `lua_pushvfstring`，只认
   `%d %f %p %s %c %U %I %L` —— 写 `%u` 会让**报错本身报错**。
3. **参数个数用 `min,max` 表达，不要用重载**。`kImpls` 按 (ns,name) 查表，
   同名只能有一条（`damage` 曾经登记两条，`damage(100,"fire")` 在**编译期**
   报 arity 不符，而错误行完全不像 arity 的问题）。要"可选参数"就把
   `max` 放大，在实现里用 `lua_gettop` 判。

---

## 9 · 分阶段（每阶段都有判据）

| 阶段 | 做什么 | 判据 |
|---|---|---|
| **A** | vendor Lua + CMake；`ScriptBackend.h` 接口 | ✅ `lua_static.lib` 产出，`make build` EXIT=0 |
| **B** | `LuaBackend` 最小闭环（加载/实例/Tick/协程/熔断/热重载） | ✅ `--selftest lua` **22 / 22 PASS**（A5+A+B+C+D+E+F） |
| **C** | 补齐 12 个命名空间 | ✅ **106 个函数**；`--selftest lua` **35 / 35 PASS**，G 组把 106 个全调一遍 |
| **C+** | **组件覆盖核对**（本文 §2.2，47 个组件逐条） | ✅ ✅15 · ◐10 · ✕18 · 🚧4；I 组从 C++ 侧读组件比对 |
| **D** | `ScriptRuntime` 按扩展名分派 | 老 `.vks` 4 个脚本行为逐位不变（`sceneHash` 不变） |
| **E** | 迁移 6 个示例脚本 + 模板 | 同一玩法 `.vks`/`.lua` 日志逐行一致 |
| **F** | 熔断 + 热重载 + 面板（API 参考 / 变量检视） | 死循环 3 帧停；改一行 0.5 s 生效且变量不丢 |
| **G** | `verify_lua.py` + `TRIGGERS` | 注册进 `DEFAULT`，`check_regression_map.py` PASS |

⚠ **A / B / C / C+ 已完成**（2026-11-03）。B 那一轮踩了一个极贵的坑，单列在下面。

### 9.1 ⚠⚠ B 阶段的坑：`lua_resume` 的第 4 个参数不能传 `nullptr`

Lua 5.4 把 `lua_resume` 从三个参数改成四个（多了 `int* nresults`）。
网上讲迁移的笔记几乎都只写"多一个参数"，**没有一句说它必须指向有效内存** ——
而 `ldo.c:863` 是**无条件**解引用：

```c
*nresults = (status == LUA_YIELD) ? L->ci->u2.nyield
                                  : cast_int(L->top.p - (L->ci->func.p + 1));
```

传 `nullptr` = 往地址 0 写一个 `int` → `0xC0000005`。特征很容易把人带偏：

| 现象 | 容易得出的错误结论 |
|---|---|
| 崩在 `lua_resume` 内部，调用栈完整、参数栈正常 | "库编坏了" |
| `lua_pcall` 一模一样地跑毫无问题 | "协程机制有问题" |
| 独立小程序跑同一段代码正常 | "引擎环境/链接有问题" |
| `/RTC1` `longjmp` 这类说法听上去很对口 | "关掉 `/RTC-` `/GS-` 就好"（**而 `/RTC-` 根本不是合法选项**，cl 只报 `D9002` 然后忽略） |

真正能一刀切开的是"**同库不同写法**"：把最小协程写成独立 C 程序时，
如果那里写的是 `int nres; lua_resume(co, L, 1, &nres);` —— 它一定正常，
因为**错误就在传 `nullptr` 那一行，不在这段代码是否被引擎链接**。

判据（写进回归）：A5 组必须存在，且**只允许用 `&nres` 形式**。

### 9.2 C 阶段（补齐 API）踩到的三件事

**① `nav.goto` 是语法错误，不是实现错误。**
`goto` 是 Lua 5.4 的**保留字**，`nav.goto(...)` 里的 `.goto` 连词法都过不了 ——
表里有、文档有、一编译就 red。已改名 `nav.moveTo`。
**规律**：给 API 取名前先过一遍 Lua 保留字表
（`goto end function nil repeat then until while in local not and or if else elseif for do break return true false`）。
这条不靠 code review 抓得住 —— 现在由 G 组（把 106 个函数全调一遍）兜住。

**② `engine.dt/time/frame` 曾经是三个死字段。**
注册时写成 0，之后**没有任何地方刷新** —— 文档写了，永远返回 0。
这类"API 存在但没用"比崩溃更难发现（不报错、不崩、只是数值不对）。
已修（每次进脚本前刷一次）+ 加了断言 `H1_clock_fields_live`。

**③ 顺带修掉的健壮性洞：chunk 的 `_ENV` 直接用了 `_G`。**
后果是脚本里任何裸赋值都写进真正的全局 → 所有实例共享；更糟的是
脚本写 `world = nil` 能把 **API 表本身**抹掉，之后整个 VM 全部失效。
现在每个脚本文件有自己的 `{__index = _G}` 表当 `_ENV`：读穿透、写隔离。

**方法论结论**：出现"同一个库、同一批开关，A 程序崩 / B 程序不崩"时，
先怀疑**两份调用代码本身有差异**，再怀疑环境。当时先怀疑环境（开关、
语言、链接），绕了整整一轮全量构建才回到调用点。

---

### 9.3 C+ 阶段（组件覆盖核对）踩到的四件事

**① `luaL_error` 不支持 `%u`。**
`lua_pushvfstring` 只认 `%d %f %p %s %c %U %I %L`，`%u` 会抛
`invalid option '%u' to 'lua_pushfstring'` —— 也就是**错误信息本身报错**：
脚本触发任何一条"实体没有某组件"的检查，拿到的都不是你以为的那句话。
⚠ 这个坑从第一版就在（`setEmissive` 那条一直是坏的），只是没人踩到它的路径。
已全量把 `luaL_error` 里的 `%u` 换成 `%d`（19 处）。

**② 实体名有两处存储，只写组件不生效。**
`World::Record::name`（`World::name()` / `world.find` 读它）和 `NameComponent::name`
（序列化与 Inspector 读它）是**同一事实的两份拷贝**。
`world.setName` 一开始只写组件 → 脚本里改完名字，`engine.name` 还是旧值，**而且不报错**。
修法：走 `World::setName`（它同时写两处）。
**规律**：凡是"看到两个地方都有这个字段"，先找那个唯一的写入口，别自己拼。

**③ 动画的权威字段不是"只有 AnimatorComponent 才有"。**
`AnimatorSystem` 自己的规则是 `an ? an->time : sk.time` ——
简单角色只挂 `SkinnedMeshComponent` 时，`time/speed/playing` 住在蒙皮组件上。
只认 Animator 的话会得到"能 `anim.play` 却 `anim.pause` 报错"，
而这两句在作者眼里是同一件事。现在 `anim.*` 走 `AnimFields`（两个都认），
与引擎同一条规则。

**④ 脚本一个钩子都没写时，引擎什么都不说。**
`function M:beginPlay()`（小写 b）是完全合法的 Lua —— 引擎永远不会调用它，
表现是"这份脚本一行动静都没有"，日志也一行没有。
现在 `createInstance` 会检查表里有没有 `BeginPlay/Tick/EndPlay/OnCollide/OnTrigger/OnExit`，
一个都没有就打一行 `LUA-NOHOOK`。
⚠ 这条不是可选的礼貌：**它正是我自己在写 I 组测试时踩的**（写了 `beginPlay`，
结果 9 条断言全红，而 `beginPlay=ok`）。

**判据设计的收获**：I 组（`I1`~`I9`）不读脚本变量，而是**从 C++ 侧读组件**比对。
理由：`setColor` 写错字段、`fov` 单位写错、`time` 忘了同步 `eventPrev` ——
这三种都**不报错不崩溃**，只有读数能抓住。其中 `eventPrev` 那条只有
"跳帧后断言它等于 0.5"能测出来。

### 9.4 收尾复查（C++ 阶段留下的六处静默失效）

C+ 收完、接口"全绿"之后又做了一轮**逐函数复查**（不是再补 API，而是问
"这套东西在真项目里跑三个月会怎么坏"）。查出六处，全部**不报错、不崩溃、
只在特定时序下错**，所以 106 条全覆盖的 G 组一条都没抓到：

**① 实体句柄被 `int` 截断（最严重）。**
句柄是 `(gen<<32)|id` 的 64 位整数，但 `engine.valid` / `world.setParent` /
`game.joint` 三处用 `aInt()`（`int`）取参数 → **generation 整段被截掉**。
只有"销毁过一个实体、槽位被复用"之后才错：

| 调用 | 截断后 | 正确 |
|---|---|---|
| `engine.valid(新句柄)` | `false`（明明活着） | `true` |
| `world.setParent(c, p)` | 报 "parent handle is not alive" | 挂上 |
| `game.joint(a, b, …)` | 挂到 generation 0 的旧句柄 | 挂到 b |

新建场景里 gen 全是 0，**一次都复现不出来** —— 这是典型的上线才炸。
修法：`aHandle()`（`luaL_checkinteger` → `uint64_t`），三处全部换掉。

**② 空句柄和"0 号实体"共用一个数字。**
打包 `(gen<<32)|id` 在 `id=0, gen=0` 时正好是 `0`，而文档一路写的都是
"空句柄 = 0"（`find` 没找到、`parent` 到根、`setParent(kid, 0)` 拔到根）。
实际行为：`setParent(kid, 0)` **静默把子节点挂到 0 号实体**上 ——
而 0 号通常活着、而且往往就是玩家，症状是"物体莫名飞到玩家身上"。
修法：打包改成 `((gen<<32)|id) + 1`，`0` 成为**唯一**的空；`unpack` 侧
`0` → `Entity{}`。句柄是不透明的，没有存档/文件依赖这个数值。
⚠ 这条是 **I1 断言先红**才发现的（`world.setParent(kid, 0)`
把 I 组的 `kids` 从 1 变成 0）—— 又一次证明"夹具里写文档里的用法"能抓到东西。

**③ `ApiTable` 的 `minArgs/maxArgs` 从来没人查。**
§8 那张表里写着"参数个数校验 ✅ 注册时按表校验"——**是假的**：注册只是把
函数挂上去。于是 `world.pos(e, 1, 2)` 静默忽略多余参数、`world.setPos(e)`
要到实现里 `luaL_checknumber(L,2)` 才报一句 Lua 味的话。
修法：所有绑定统一挂 `dispatch`（upvalue 带 `Impl*` + `ApiEntry*`），
先按表校验实参个数再进实现，报错形如
`world.setPos: expects 2 argument(s), got 1`。`.vks` 那边（`kBuiltins`）一直是查的，
两个后端现在同一套语义。

**④ 顶层代码与 `waitUntil` 条件函数不受熔断保护。**
预算钩子只装在 `doResume()` 里，而 `compile()` 的 `lua_pcall`（顶层 chunk）
和 `evalUntil()` 的 `lua_pcall`（条件函数）同样跑的是作者代码 ——
一句 `while true do end` 写在顶层就能冻住整个游戏，只有任务管理器能救。
修法：钩子提到文件级 `luaBudgetHook` + `budgetAttach/Detach`，
三处都装。⚠ **不能用 RAII**：`luaL_error` 是 longjmp，守卫的析构跑不到，
钩子会永远留在 state 上打断后面所有代码。

**⑤ `camera.fov` 在定焦镜头下静默无效。**
`CameraComponent::focal > 0` 时视场角由焦距算出并**覆盖** `fov`
（`poseFromEntity` / `apply` / `applyCameraEntityTo` 三处都这样）。
脚本写 `camera.fov(e, 70)` 画面一动不动、日志一行没有；读回来的还是那个
被覆盖的旧值。修法：**写 fov 就把 `focal` 清零**（两者是同一个量的两种表示，
脚本显式要视角就是"改用 fov 说话"，并打一行 `LUA-CAMERA`），
**读返回实际生效的视场角**（`focalToFovY`）。

**⑥ `World::setName` 在实体没有 `NameComponent` 时不补组件。**
`Scene::find` / 层级树 / 序列化只读组件，`Record::name` 只有 `World::name()` 读。
缺组件时改名 → `engine.name` 变了、`world.find` 还是找不到（同 §9.3 ② 的另一半）。
修法：`setName` 缺组件就补（**只在显式改名时补**，`create()` 里不补，
否则每个实体都多一个组件、存档与 `sceneHash` 跟着变）。
顺带：`world.despawn` 现在会**先把自己从父节点的 `children` 里摘掉**再销毁 ——
否则那个 id 被 `m_free` 复用后，`Scene::setParent` 摘链时会删掉**别人**的登记项。

**判据**：新增 **J 组（J0~J8）**，九条全部可以在旧实现下变红（③④⑥ 需要
先造"槽位复用"现场才有意义，所以 `J1` 是**夹具前提**断言：销毁后重建必须
复用同一 id 且 gen+1；不复用就说明这条测不到东西）。
`--selftest lua` 从 35 条变成 **44 条**。

**J7 / J8 是"空句柄"承诺的两半，落点不同，别合并**（合并了就分不清是哪一半坏了）：

| 断言 | 守的是什么 | 怎么让它红（否定测试实测） |
|---|---|---|
| `J7_zero_is_null` | `unpack` 里 `if (v == 0)` 空分支 + `pack` 里 `if (!valid) return 0` | 删掉 `packEntity` 的 `!valid` 短路 → `find(missing)` 返回 `4294967296`（`0x100000000`），J7 红、J8 绿 |
| `J8_zero_entity_handle_nonzero` | `pack` 的 `+1`（**0 号实体的句柄不许是 0**） | 撤掉 `+1` → `self()~=0 -> false`，J8 红、J7 因脚本先崩只能读到空值 |

⚠ `J8` 的取值**必须放在脚本最前面**：撤掉 `+1` 后 0 号实体的句柄变成 0，
脚本会在中途 `world.children` 那行先报 `entity #-1 is not alive`，
放在后面就只能读到空值 —— 看着红了，其实不是它自己抓到的。
（第一次就是放后面，测出来 18 条红、J8 的失败信息是空的，才改成前置。）

⚠ 顺带发现一个**既有** flaky：`.vks` 的 `C8b 熔断后帧时间仍在预算内`
（`< 500 ms`）在机器负载高时会红（实测 508 ms / 空闲 421 ms）。
与本次改动无关（那条走的是 `.vks` 内核），跑回归时别把这种红当漏。

## 10 · 与 UE5 的取舍（明确不做）

| UE5 有 | 本引擎 | 为什么 |
|---|---|---|
| `UObject` + 反射 + `UPROPERTY` | 一张手写 API 表 + `entity` 句柄 | 反射要一整套类型注册表，与本引擎"结构封闭"的纪律冲突 |
| 蓝图节点图 | 不做，只写文本 | 成本高于 S0~S6 总和；节点图可以将来**编译到同一份 API 表** |
| `GAS`（Ability / Attribute / Effect / Cue） | `game.*` 的 `attr/status/tickRound`（已有 5410 行 `gameplay/`） | 够用，不另起一套 |
| `UWorld` / `Actor` 生命周期 | 场景 + `entity` + 本文件的钩子 | 本引擎没有 Actor 对象，只有句柄 |
| 多播委托 / 事件 | 钩子（碰撞/触发）+ `engine.on()`（后续） | 先做够用的 |
| Live Coding（C++ 热重载） | **不做** | `FreeLibrary` 脆 + 崩溃带走编辑器 + 毁确定性 |
