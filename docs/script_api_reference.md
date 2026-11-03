[INFO ] === Venn Engine - Runtime (Game.exe) ===
[INFO ] GAME-LOG path=C:\Users\wumen\AppData\Local\Venn\venn\logs\runtime-20261103-215722.log
[INFO ] GAME-BUILD venn 0.2.0 dev git=5761469-dirty
[INFO ] GAME-CONFIG loaded D:\A-VENN\code\cpp\venn-engin\build\bin\assets\game.json
[INFO ] AUDIO-CONFIG enabled=0 device=default null=0 master=1.00 music=0.70 sfx=1.00 voice=1.00 mute=0 duck=0.75 maxvoices=64 retry=0
[INFO ] GAME-CONFIG scene='scenes/sandbox.json' window=1280x720 fullscreen=0 maxFps=60 unfocusedFps=15 timeScale=1.00 language=en quality=high startState=playing selftest='api' verbose=0 uiDoc='' source='D:\A-VENN\code\cpp\venn-engin\build\bin\assets\game.json'
# Lua 游戏逻辑 API · 完整参考

> 本文件由 `Game.exe --selftest api` **自动生成**，源数据是 `engine/script/ApiTable.cpp`。
> **不要手改这一份** —— 改表后重跑，否则下次生成会被覆盖。

共 **106** 个函数，分布在 **12** 个命名空间。概念与用法见 `docs/script_api.md`。

| 命名空间 | 函数数 | 作用 |
|---|---|---|
| `engine` | 13 | 自身 / 值 / 时间 / 日志 |
| `world` | 18 | 场景查询与变换 |
| `actor` | 10 | 实体行为（可见 / 刚体 / 材质） |
| `input` | 4 | 动作输入 |
| `anim` | 10 | 动画 |
| `nav` | 4 | 导航 |
| `fx` | 5 | 特效与镜头 |
| `audio` | 3 | 音频 |
| `light` | 5 | 灯光 |
| `ui` | 6 | HUD / 对话 / 过场 / 界面 |
| `camera` | 3 | 相机（FOV / 臂长 / 机位开关） |
| `game` | 25 | 玩法数据（属性 / 状态 / 配表 / 卡牌 / 旗标 / 关节） |

## `engine` · 自身 / 值 / 时间 / 日志（13）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `engine.self(-> entity)` | `` | 当前脚本所属的实体 |  |
| `engine.valid(entity)` | `boolean` | 句柄是否还活着（实体没被销毁） |  |
| `engine.exists(entity)` | `boolean` | `valid` 的语义化别名 |  |
| `engine.name(entity)` | `string` | 实体名 |  |
| `engine.vec3(number, number, number)` | `vec3` | 构造向量（vec3 是 `{x=,y=,z=}` 的表） |  |
| `engine.dist(vec3, vec3)` | `number` | 两点距离（米） |  |
| `engine.clamp(number, number, number)` | `number` | 夹取到 [lo, hi] |  |
| `engine.lerp(number, number, number)` | `number` | 线性插值 a→b，t=0..1 |  |
| `engine.log(any...)` | `void` | 写一行 INFO 日志（参数用空格拼接） |  |
| `engine.warn(string)` | `void` | 写一行 WARN 日志（面板标黄） |  |
| `engine.wait(number)` | `number` | 挂起 n 秒；恢复时返回**实际等待时长** | 只能在方法体顶层或协程里调；不要在 `pcall` 包裹的代码里用 |
| `engine.waitFrames(integer)` | `void` | 挂起 n 帧（与帧率无关） |  |
| `engine.waitUntil(function)` | `void` | 每帧调一次条件，成立才继续 | 条件函数里**不能**调 `wait`；内部每帧求值，别写重计算 |

## `world` · 场景查询与变换（18）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `world.pos(entity)` | `vec3` | 世界坐标（父节点已折算） |  |
| `world.setPos(entity, vec3)` | `void` | 设置世界坐标（有父节点时反解局部矩阵） |  |
| `world.rot(entity)` | `vec3` | 欧拉角（**弧度**） |  |
| `world.setRot(entity, vec3)` | `void` | 设置欧拉角（**弧度**） |  |
| `world.rotY(entity)` | `number` | 只取朝向角（弧度），等价 `world.rot(e).z` |  |
| `world.scale(entity)` | `vec3` | 缩放 |  |
| `world.setScale(entity, vec3)` | `void` | 设置缩放 |  |
| `world.moveBy(entity, vec3)` | `void` | 相对位移（不覆盖现有坐标） |  |
| `world.near(vec3, number [, integer])` | `entity[]` | 半径内的实体（按距离升序，可限个数） | ⚠ 全扫带 Transform 的实体再按 (距离, id) 排序 —— **不是** BVH 快查，循环里每帧调会退化成 O(n²)，热路径请自己缓存结果 |
| `world.find(string)` | `entity` | 按名字找实体；找不到返回**空句柄**（`engine.valid()` 为假，不报错） | 空句柄就是 `0`（唯一表示），`if e == 0` / `engine.valid(e)` 都判得出 |
| `world.count(-> integer)` | `` | 场景里的实体总数 | 数的是**带 Transform 的**实体（出生点 / 水体这类没有变换的对象不算） |
| `world.spawn(string, vec3)` | `entity` | 在指定位置实例化模型资产，返回新实体 | 新实体**不进物理世界**（场景物理只在启动/换场景时构建一次） |
| `world.despawn(entity)` | `void` | 销毁实体（含子节点） |  |
| `world.loadScene(string)` | `boolean` | 请求换场景：true = 已受理，**下一帧**才真正切换 |  |
| `world.setName(entity, string)` | `void` | 改实体名（改完 `world.find` / `engine.name` 立刻按新名生效） | 重名不报错：`find` 返回先遍历到的那个 |
| `world.parent(entity)` | `entity` | 父实体；根节点返回**空句柄**（不报错） | 空句柄 = `0`，可以原样传回 `setParent` 表示「拔到根」 |
| `world.children(entity)` | `entity[]` | 直接子实体（**不含**孙辈） | 顺序 = 挂接顺序，可当确定性依据用 |
| `world.setParent(entity, entity)` | `void` | 挂到新父节点下；第二个参数传 `nil` 或 `0` = 拔到根 | ⚠ 只改局部变换：世界位置会跟着父节点跳。要「原地换父」先记下 `world.pos` 再 `setPos` 补回来。⚠ 成环（父是自己的后代）或句柄已销毁 → 报错 |

## `actor` · 实体行为（可见 / 刚体 / 材质）（10）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `actor.visible(entity)` | `boolean` | 是否可见 |  |
| `actor.setVisible(entity [, boolean])` | `void` | 设置可见（省略布尔 = 对 self 设为 true） |  |
| `actor.setShadow(entity, boolean)` | `void` | 是否参与阴影投射 |  |
| `actor.vel(entity)` | `vec3` | 刚体线速度 | 实体没有刚体时**报错**，不是返回 0 |
| `actor.setVel(entity, vec3)` | `void` | 设置刚体线速度 | 同上：没有刚体 → 报错 |
| `actor.impulse(entity, vec3)` | `void` | 施加冲量（质量已含在刚体里） |  |
| `actor.grounded(entity)` | `boolean` | 是否站在地面上 |  |
| `actor.setEmissive(entity, number, number, number)` | `void` | 设置材质自发光 RGB | 材质是**按名共享的资产**：改它会影响所有用同一材质的实体 |
| `actor.color(entity)` | `vec3` | 材质底色（`baseColorFactor.rgb`） | 读到的值可能是**别的实体**写进去的（材质共享） |
| `actor.setColor(entity, number, number, number)` | `void` | 改材质底色 RGB（受击闪白 / 中毒变绿 / 无敌闪烁） | ⚠ 材质共享：会影响所有用同一材质的实体。要独立配色，得先在工程里给该实体换成材质副本 |

## `input` · 动作输入（4）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `input.pressed(string)` | `boolean` | 本帧是否按下（动作名，不是键码） |  |
| `input.held(string)` | `boolean` | 是否持续按住 |  |
| `input.released(string)` | `boolean` | 本帧是否松开 |  |
| `input.axis(string)` | `number` | 轴的值（-1..1） |  |

## `anim` · 动画（10）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `anim.play(entity, string)` | `boolean` | 播放动画片段（要求有 AnimatorComponent 且骨架里真有这段） | 返回 false = 没播；不要当它一定成功 |
| `anim.pause(entity)` | `void` | 暂停播放（播放头停在原地） |  |
| `anim.resume(entity)` | `void` | 从暂停处继续 |  |
| `anim.stop(entity)` | `void` | 停止并回到 0 秒（**不是**「播完最后一段」） |  |
| `anim.speed(entity [, number])` | `number` | 读/写播放倍率（1 = 正常，负数 = 倒放） | 权威字段与 `AnimatorSystem` 一致：有 `AnimatorComponent` 用它，否则用 `SkinnedMeshComponent` |
| `anim.time(entity [, number])` | `number` | 读/写播放头（秒）；写 = 直接跳帧 | 跳帧会把 `eventPrev` 一起挪过去 —— 否则中间那些动画事件会在下一帧**一次性全炸** |
| `anim.clip(entity)` | `string` | 当前正在放的片段名；没有则空串 |  |
| `anim.morph(entity, string, number)` | `void` | 设置形态键（BlendShape）通道权重，钳到 0..1 | 通道名来自模型资产的 `MorphRig`；**拼错报错**，不静默。权重会被钳位，负数等于 0 |
| `anim.morphWeight(entity, string)` | `number` | 读形态键通道的当前权重 |  |
| `anim.expression(entity, string)` | `boolean` | 切表情预设（资产里的 preset 名）；空串 = 回中性 | 要求实体挂 `ExpressionComponent` 且预设表加载成功。返回 false 时打一行 WARN，不静默 |

## `nav` · 导航（4）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `nav.moveTo(entity, number, number, number)` | `boolean` | 请求走到某点：true = 已受理（求路在下一帧） | ⚠ 不叫 `goto`：`goto` 是 Lua 5.4 的**保留字**，`nav.goto(...)` 连语法都过不了 |
| `nav.arrived(entity)` | `boolean` | 是否到达目标点 | **必须**和 `nav.failed` 成对轮询，否则会永远等 |
| `nav.failed(entity)` | `boolean` | 是否走不过去（目标在网格外/墙里） |  |
| `nav.stop(entity)` | `boolean` | 停止移动 |  |

## `fx` · 特效与镜头（5）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `fx.play(string [, number, number, number])` | `integer` | 在 self 处（或指定点）播放特效，返回实例 id（0 = 没播） |  |
| `fx.stop(integer)` | `boolean` | 停止一个特效实例 |  |
| `fx.shake(number)` | `void` | 镜头抖动，trauma 0..1（按与镜头的距离衰减） |  |
| `fx.hitstop(number)` | `void` | 顿帧 n 秒（按真实时间递减） |  |
| `fx.timeScale(number)` | `void` | 全局倍速（子弹时间）；<0 拒收 |  |

## `audio` · 音频（3）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `audio.play(string)` | `void` | 播放一次性音效（路径相对 assets/） |  |
| `audio.stopAll(-> void)` | `` | 停掉全部一次性音效 |  |
| `audio.bus(string, number)` | `void` | 设置总线音量（"music" / "sfx" / "voice"，0..1） |  |

## `light` · 灯光（5）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `light.color(entity, number, number, number)` | `void` | 灯光颜色 RGB | 实体既没有点光也没有聚光时**报错** |
| `light.intensity(entity, number)` | `void` | 灯光强度 |  |
| `light.range(entity, number)` | `void` | 影响半径（米） |  |
| `light.direction(entity, number, number, number)` | `void` | 灯光传播方向（自动归一化） | 聚光灯与平行光都改；点光没有方向，调用是空操作 |
| `light.enabled(entity [, boolean])` | `boolean` | 读/写点光 / 聚光是否亮着 | ⚠ 平行光**没有**这个开关 → 报错；要关太阳请用 `light.intensity(e, 0)` |

## `ui` · HUD / 对话 / 过场 / 界面（6）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `ui.health(number)` | `void` | 设置 HUD 血量（绝对值） |  |
| `ui.damage(number [, string])` | `void` | 扣血：给了伤害类型就走玩法属性（护甲/易伤/护盾） |  |
| `ui.popup(number)` | `void` | 在 self 头顶弹一条伤害数字 |  |
| `ui.dialogue(string)` | `boolean` | 播放对话（资产 id） |  |
| `ui.sequence(string)` | `boolean` | 播放过场时间轴（资产路径） |  |
| `ui.open(string)` | `boolean` | 切换到某份 UI 文档；**空串 = 关掉**当前这块 |  |

## `camera` · 相机（FOV / 臂长 / 机位开关）（3）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `camera.fov(entity [, number])` | `number` | 读/写视野角（**度**） | ⚠ 单位是**度**，和 `world.rot` 的弧度不一样 —— 相机面板本来就用度，跟着它走。⚠ 相机装了定焦镜头（`focal > 0`）时视场角由焦距算出来、会覆盖 fov：这里**写 fov 会把 focal 清零**（否则画面一动不动），**读回来的是实际生效的视场角** |
| `camera.distance(entity [, number])` | `number` | 读/写第三人称臂长（米） |  |
| `camera.enabled(entity [, boolean])` | `boolean` | 读/写这台相机是否参与渲染（多机位切换） | 渲染时 `enabled` 是硬过滤、`priority` 定胜负（同分取实体 id 小的）；`priority` 本轮不开放 —— 要换机位就切 `enabled` |

## `game` · 玩法数据（属性 / 状态 / 配表 / 卡牌 / 旗标 / 关节）（25）

| 函数 | 返回 | 说明 | 注意 |
|---|---|---|---|
| `game.attr(entity, string [, number])` | `number` | 读脚本变量；给了默认值则**写回**并返回它 | ⚠ 这是**脚本变量**（`.vks` 的 `attr`）。读玩法属性请用 `game.stat` |
| `game.setAttr(entity, string, number)` | `void` | 写脚本变量 |  |
| `game.stat(entity, string)` | `number` | 读玩法属性（含状态加成） | 属性不存在时返回 0 —— 想区分请先用 `game.hasStat` |
| `game.hasStat(entity, string)` | `boolean` | 玩法属性是否存在 | `game.stat` 返回 0 有歧义（不存在 / 就是 0），这个函数用来分清 |
| `game.damage(entity, number, string)` | `number` | 对**指定实体**造成伤害，返回真正扣掉的血 | target 没有玩法组件 → 报错（`ui.damage` 才是「打自己」那个） |
| `game.worldAttr(string [, number])` | `number` | 读场景级共享变量；给了默认值则写回 | 不带默认值读取未设过的键 → **报错**（避免「永远 0」） |
| `game.setWorldAttr(string, number)` | `void` | 写场景级共享变量 |  |
| `game.status(entity, string)` | `integer` | 加一层状态，返回加完之后的层数（-1 = 失败） |  |
| `game.round(-> integer)` | `` | 回合末结算（dot 等），返回结算的条数 |  |
| `game.flag(string [, number])` | `number` | 读剧情旗标；给了第二个参数则**写**它 | 未设过的旗标返回 0，不报错（「第 1 章没遇见的人好感度就是 0」） |
| `game.castCard(entity, integer)` | `number` | 打出手牌第 idx 张并结算，返回实际伤害 | 没有牌组 / 下标越界返回 0（**不报错**）：手牌空还按一下是正常用法 |
| `game.drawCards(entity, integer)` | `integer` | 抽 n 张，返回实际抽到的张数 |  |
| `game.tableNum(string)` | `number` | 读配表数字（键是 `表.行.列`） | 表没加载也返回 0 → 配合 `game.tableHas` 判 |
| `game.tableStr(string)` | `string` | 读配表字符串 |  |
| `game.tableHas(string)` | `boolean` | 这一格是否存在 |  |
| `game.tableRows(string)` | `integer` | 行数（表不存在 → **-1**） |  |
| `game.joint(entity, entity, string)` | `boolean` | 建立关节（"hinge" / "ball" / "slider" / "distance"） |  |
| `game.jointAnchors(entity, vec3, vec3)` | `boolean` | 设置两端锚点（各自的**局部**坐标） |  |
| `game.jointAxis(entity, vec3)` | `boolean` | 设置关节轴（self 的局部坐标） |  |
| `game.jointLimit(entity, boolean, number, number)` | `boolean` | 设置限位（铰链是弧度、滑动是米） |  |
| `game.jointMotor(entity, number, number)` | `boolean` | 设置马达（force <= 0 等于关掉） |  |
| `game.jointOn(entity, boolean)` | `boolean` | 启用 / 禁用关节 |  |
| `game.jointRemove(entity)` | `boolean` | 删除关节；false = 本来就没有 |  |
| `game.jointKind(entity)` | `string` | 关节类型；没有关节返回 nil |  |
| `game.jointTarget(entity)` | `entity` | 关节的另一端；没有则返回空句柄（`engine.valid()` 为假） |  |

