# 特效系统（P1）—— `.veffect` / 宿主池 / 动画事件路由 / 打击感

> 权威文档。评估与路线图在 `docs/gameplay_fx_readiness.md`（§四 方案 / §五 路线图）。
> 本文只写**怎么用 + 为什么这么做 + 判据在哪**。
> 做完日期：2026-11-02。判据：`tools/verify_fx.py` A~G 共 **52 条**。
> 引擎侧与运行时侧**各有一份分发**（编辑器 Play / `Game.exe`），行为逐条对齐。

---

## 零、一句话

一个特效 = **一个 `.veffect` 文件**（粒子 + 音效 + 震屏 + 闪白 + 顿帧）。
三个入口（动画事件 / 过场事件 / 脚本动词）都调**同一个** `EffectService::play`，
它从一个**固定大小的宿主池**里借槽位，播放由**引擎自己回收**。

作者写一次 `assets/fx/slash_a.veffect`，角色动画的事件表里填
`kind=Hit  payload=fx/slash_a.veffect` —— 出刀就有刀光 + 音效 + 震屏 +
顿帧，**不写一行代码**。这就是"打击感不需要写代码"的分界线。

---

## 一、四层结构

```
① EffectAsset (.veffect)   一个特效 = 粒子 + 音效 + 震屏 + 闪白 + 顿帧 + 时长
② 三条入口                  AnimEventRouter / SeqEvent / 脚本动词
③ EffectService            池化 / 上限 / 抢占确定性 / 回收
④ 五执行者                  粒子 / 音效 / 震屏 / 闪白 / 伤害数字
```

| 层 | 文件 | 职责 | 判据 |
|---|---|---|---|
| ① 资产 | `engine/fx/EffectAsset.*` | 解析 `.veffect`、算 `effectiveDuration()` | `FX-ASSET` **18 条** |
| ③ 池 | `engine/fx/EffectService.*` | 借槽 / 回收 / 丢弃计数 / 抢占规则 | `FX-POOL` **12 条** |
| ② 路由 | `engine/fx/AnimEventRouter.*` | `(kind, payload)` → 动作 | `FX-EVENT` **11 条** |
| ④ 接线（编辑器） | `editor/EditorApp.*` / `editor/FxPanel.*` | 四个 sink / 顿帧 / 闪白 / 面板 | `verify_fx` B~F |
| ④ 接线（出包） | `runtime/RuntimeApp.*` | 同四个 sink + 闪白画在 UI canvas | `verify_fx` G |

**分层硬约束**：`fx` **不直接依赖** `audio` / `CameraSystem` / `EditorApp`。
四条对外动作走 `EffectSinks` 的四个 `std::function`（`shake` / `sound` /
`flash` / `hitstop`）。好处有两层：自检可以塞空实现（不需要声卡、不需要
相机）；Runtime 与编辑器各自接自己的实例，`fx` 一行不改。

---

## 二、`.veffect` 怎么写

```jsonc
{
  "name": "hit_spark",
  "duration": 0.12,        // 本体时长（秒）
  "hitstop": 0.05,         // 顿帧秒数（0 = 不顿）
  "shake": 0.18,           // 震屏 trauma（0..1，走距离衰减）
  "shakeRadius": 12.0,     // 衰减半径（米）
  "flash": 0.12,           // 闪白强度（0..1）
  "flashDuration": 0.05,   // 闪白秒数。⚠ 只给 flash 不给它会静默不闪（见下）
  "flashColor": [1, 1, 1],
  "sound": "sfx/hit.wav",  // 空 = 不响
  "volume": 1.0,
  "pitchVariance": 0.08,   // 音高抖动（走特效独立随机流）
  "particles": [ /* 1~3 个发射器，字段与 ParticleEmitterComponent 完全相同 */ ]
}
```

### ⚠ `flash` 给了但 `flashDuration` 漏了 = **一点都不闪**

这是"配了没效果"里最难查的一类：没有错误、没有警告、画面就是没闪。
解析期补一个默认值（`min(duration, 0.25s)`）并记一条 warning（`FX-ASSET`），
判据是自检 **E10**。
上限 0.25 秒的理由：闪白超过 1/4 秒开始"糊住整个画面"，比不闪更糟。


### ⚠⚠ `particles[]` 的字段清单**必须**复用 `ecs::ParticleEmitterComponent`

这就是 `engine/scene/ParticleJson.*` 存在的原因：50+ 个字段，另写一份清单
必然漂移，而漂移**不报错**（缺字段按默认值走）—— 症状是"场景里调好的发射器
做成特效就少了一半参数"，只有逐字段对比才能发现。
所以 `SceneSerializer` 与 `EffectAsset` 共用**同一份** `particleEmitterTo/FromJson`。

### ⚠ `effectiveDuration()` 是**回收判据**，不是 `duration`

粒子寿命常比特效本体长：火花 0.5 秒喷完、飞 1.8 秒。共享池按遍历顺序分配
槽位，提前回收 = 那批粒子不再被模拟 → **定格在空中**。
`effectiveDuration()` 取"本体时长"与"最后一颗粒子死掉"的**大**：

```
循环发射器无终点      → 用本体时长兜底（作者得自己给个时长）
非循环                → startDelay + duration
burst                 → 可能落在尾巴上，所以取 max(burstTime)
最后出生的粒子        → 还要活完 lifetime × (1 + variance)
```

上限 `kMaxEffectDuration = 10.0f`（超过一律夹住 —— 一个忘改的 `duration`
会让整池被一个特效占满）。

### 三处"看着能省"但一定炸的

1. **`kMaxEffectLayers = 3`**：一个 `.veffect` 最多 3 个发射器 = 3 个宿主槽。
   超过就**截断 + warning**（`FX-ASSET` E5），不是静默丢。
2. **`particles` 不是数组 → 整个资产拒收**（致命，E3）；空数组合法（E4）。
3. **`duration <= 0` 被夹到 0.01**（E6），不是拒收 —— 作者漏填时应当还能出东西。

---

## 三、宿主池：为什么空闲不能用 `enabled = false`

这是整个 P1 唯一**只能靠读渲染器代码才能发现**的坑。

`Renderer::collectParticleEmitters` 按下标顺序收集：

```cpp
for (每个实体) { if (scratch.size() >= maxEmitters) return; scratch.push_back(...); }
```

**发射器下标 = 本帧收集顺序**，不是实体身份。于是：

| 表示"空闲"的方式 | 后果 |
|---|---|
| `enabled = false` | 空闲宿主被排除 → "在用的那个"下标**前移** → 粒子槽位前移 → 上一批还在飞的粒子换成别人的参数（**火花突然变烟雾**） |
| **`rate = 0` + 所有 `bursts[].count = 0`** ✅ | 收集顺序恒定；已飞的粒子自然活完（**不定格**） |

所以 `EffectService::silenceHost()` **只关"发新粒子"**，永远不碰 `enabled`。

### 池的四条确定性规则

1. **按槽号升序取**（`acquire` 顺序扫）：同一串请求的结果可复现，截图判据依赖它。
2. **不够就整笔拒绝**（`freeCount < n` → 全体丢弃，`dropped++`）。半套是最坏的：
   一个特效只出了一半的层，看起来像"参数没生效"。
3. **池满时新的被丢弃**，不挤掉正在播的。同帧多次请求结果可复现。
4. **`clearActive()` 保留 id 计数**：`stopAll()` 后把 `m_nextId` 归零的话，
   脚本里还攥着的旧 id 一调 `stopEffect` 就误杀刚播的那个。

### ⚠⚠ 特效随机必须与玩法随机**分两条流**

`EffectService.cpp` 自己一个 `xorshift32`（种子固定）。共用一条流的话
"多播一个粒子就改变下一张抽到的牌" —— 症状是"关掉特效之后抽牌顺序变了"，
几乎不可能靠读代码发现。P2 的 `Random` 是另一条。

---

## 四、动画事件路由：`kind` 与文档口径

作者在 Inspector 的 `Animation Events` 段填一行：

| 写法 | 结果 |
|---|---|
| `kind=Hit` + `payload=fx/slash_a.veffect` | 出特效（**文档口径**） |
| `payload="Hit\|fx/slash_a"`（kind 留空） | 同上（兜底写法） |
| `kind=Fx` / `Effect` / `Impact` / `Attack` / `HitFx` | 同上（别名） |
| `kind=Sound` + `payload=sfx/swing.wav` | 放音效 |
| `kind=Shake` + `payload=0.35` | 震屏（夹到 0..1） |
| `kind=Hitstop` + `payload=0.08` | 顿帧（夹到 0..0.5） |
| `kind=Footstep` | **忽略**（不是错误，留给脚本） |
| `kind=Shake` + `payload=abc` | 失败（参数不是数字） |

- **kind 大小写不敏感**（作者写小写是最常见的手滑）。
- **路径可以不带扩展名**：`fx/sword_hit` → `fx/sword_hit.veffect`。
  判据是"最后一个 `/` 之后还有点吗" —— 盲目 `substr(0, rfind('.'))`
  会把 `fx/v1.2_hit` 截成 `fx/v1`。
- 同一个错误**每种只报一次**（一段动画每帧都发会刷爆日志）。

### ⚠⚠ `FiredAnimEvent::entity` **只有 id、没有 generation**

自己拼 `Entity{id, 0}` 会永远 generation 不匹配（`alive()` 恒假）→
特效全部落在**世界原点**。必须走 `World::handle(id)` 让它把当前代际号填回来。

### ⚠⚠ 全仓只有**一处**读 `eventsThisFrame()`

编辑器：`EditorApp::dispatchAnimEventsThisFrame()`。
出包：`RuntimeApp::dispatchAnimEventsThisFrame()`。

**同一个进程里只有其中一方在跑**（编辑器 = Play 态，运行时 = `Game.exe`），
所以不会"双重曝光"。⚠ 真正危险的是**在同一个 App 里再开一处** ——
那样每帧两条一模一样的分派，特效叠两层而两边都觉得自己对。

### 编辑器 / 运行时两份必须逐条对齐

| 项 | 编辑器 | 出包 |
|---|---|---|
| 顿帧系数 | `fx::kHitstopScale` | `fx::kHitstopScale`（**同一个常量**，定义在 `EffectAsset.h`） |
| 顿帧计时器 | 真实 dt | 固定步 `rawDt`（不含被压低的倍率） |
| 特效池推进 | `tickFx(dt)` 每帧 | `cb.variableUpdate` 里的 `tickFx(vdt)` |
| 闪白 | ImGui 背景层 | `ui2::UiCanvas` 最后一层矩形 |
| 日志前缀 | `FX-HITSTOP` / `FX-FLASH` / `FX-DISPATCH` | `FX-RT-HITSTOP` / `FX-RT-FLASH` / `FX-RT-DISPATCH` |

⚠ 前缀分开是**故意的**：两边的判据完全不同（编辑器验"面板 + 命令"，
运行时验"出包后还有没有"），混在一个前缀里会让一边的日志兜住另一边的红。

---

## 五、打击感五个旋钮

| 旋钮 | 字段 | 典型值 | 失败形态 |
|---|---|---|---|
| 顿帧 | `hitstop` | 0.04 ~ 0.09 | 太长 = "卡"；累加 = 5 连击 0.45 秒 |
| 震屏 | `shake` | 0.12 ~ 0.30 | 无衰减 = 远处爆炸和近处一样晃 |
| 闪白 | `flash` | 0.10 ~ 0.25 | 太长 = 满屏白，看不出动作 |
| 音高抖动 | `pitchVariance` | 0.05 ~ 0.12 | 0 = 连击听起来像复读 |
| 粒子 | `particles[]` | 2 层（光带 + 火星） | 层数 > 3 = 占满池，第二个敌人没火花 |

### ⚠⚠ 顿帧计时器必须用**真实 dt**

用 `dt * timeScale` 会形成**正反馈**：倍速被自己压低 → 计时器变慢 →
0.09 秒的停顿拖到 1.8 秒。写成 `timeScale = 0` 是**死锁**（永远走不完，
游戏卡死但渲染正常）。

实现取 `kHitstopScale = 0.06f` 而不是 0（0 会让动画/物理完全冻住，
而"顿帧"要的是"几乎冻住"）。**取 max 不累加**。

### 顿帧的三个数：`total` / `elapsed` / `frames`

```
FX-HITSTOP begin=0.150 reqs=3 left=0.150
FX-HITSTOP end total=0.150 elapsed=0.159 frames=7 reqs=3 minScale=0.060
```

| 数 | 抓什么 |
|---|---|
| `total` | **累加**（3×0.15 若变 0.450 立刻红） |
| `elapsed` | **scaled dt**（0.15 拖成 2.5 秒立刻红） |
| `frames` | **`timeScale = 0` 的死锁**（永远不打 end 行） |

这三个数缺一不可：顿帧的失败形态在画面上都长得像"有点卡"。

### ⚠ 闪白不新开 pass

走 `ImGui::GetBackgroundDrawList()`（3D 之上、所有窗口之下）：不盖面板文字、
不动 `PostProcess` 的 pass 结构（改一处要同步改 N 处是老坑）。
强度**线性衰减**（不做 ease-out），让"闪了多久"与 `flashDuration` 严格对应 ——
判据好写。

---

## 六、脚本动词（6 个）

```js
let id = playEffect("fx/hit_spark.veffect")      // self 位置；返回 id，0 = 失败
let id2 = playEffect("fx/hit_spark", x, y, z)    // 世界坐标
stopEffect(id)                                   // 按 id 停
shake(0.4)                                       // 震屏（夹 0..1）
hitstop(0.08)                                    // 顿帧（夹 0..0.5）
setTimeScale(0.5)                                // 夹 [0.05, 4]，**0 会被拒**
```

- `playEffect` **只有 1 和 4 个参数合法**；2/3 个**当场报错**（静默当成 self
  位置会让"特效出现在脚下"变成玄学）。
- 返回 `id = 0` 时**报错但不中止脚本** —— 一个火花放不出来不该让整段剧情停下。
- `setTimeScale(0)` 是作者最常写的死锁，直接拒并给出区间。

> `playSound` 早就存在（`ScriptHost::playSound`），不在这一批新增里 ——
> 当初以为缺，是因为只 `grep` 了 `svc.act.` 前缀。

---

## 七、三种驱动源必须分清

| 类型 | 例子 | 驱动方式 | 现在的支持 |
|---|---|---|---|
| **状态型** | 附魔光、燃烧、中毒 | 存在即播，跟着实体走 | Inspector 挂发射器（`ParticleEmitterComponent`），不用 `.veffect` |
| **事件型** | 刀光、命中、爆炸 | 瞬间事件，一次性 | `.veffect` + 三条入口 |
| **环境型** | 雨雪、飘尘、雾 | 区域长驻 | 场景里的发射器（`rateOverDistance` / 大 `shape`） |

混在一起设计必然退化成"每次手写脚本"。

---

## 八、判据表（`tools/verify_fx.py`）

| 组 | 验什么 | 条数 | 关键判据 |
|---|---|---|---|
| A | 三组内核自检 | 10 | `FX-ASSET-TOTAL pass=18` / `FX-POOL pass=12` / `FX-EVENT pass=11` + `PROJECT-ALL-TOTAL failures=0` |
| B | 面板锚点 + 三条命令 | 11 | `FX-PLAY ... id=1 active=1`（**`id>0` 才算真占到槽**） |
| C | 池满确定性丢弃 | 4 | 同帧 11 连发 → 8 成功 3 丢弃 + `dropped=3` |
| D | 脚本动词接通 | 5 | 没有 `playEffect() is not available` + `src=service` + `id>0` |
| E | 动画事件 → 特效（编辑器） | 4 | `ANIM-EVENT ... kind=Hit` **恰好一次** + `FX-DISPATCH total=1 fx=1` **恰好一次** + `FX-PLAY id>0` |
| F | 顿帧 / 闪白否定测试 | 11 | `total=0.150`（不是 0.450）/ `elapsed<0.60` / `2<=frames<=90` / `FX-FLASH end` 出现 |
| G | **出包运行时**侧同一条链 | 7 | `FX-RT-DISPATCH total=1 fx=1` + `FX-PLAY id>0` + `FX-RT-HITSTOP end` + `FX-RT-FLASH end` |

**判据口径的四条纪律**：

- **`id > 0` 才是证据**。"命令没报错"完全不能证明播了（资产缺失时也返回一个错误串）。
- **池满必须断言 `dropped`**：画面里**永远**不会出现"这里本该有一个特效"的证据。
- **反向判据优先**：`playEffect() is not available in this host` 不许出现 ——
  没接时脚本不崩、静默走 else（A4 的 `setFlag` 同一个教训）。
- **E 组只放一条事件且 `loop=False`**：循环时"多一帧 / 少一次 / 每帧都发"
  这三种错误在序列里长得几乎一样。

---

## 九、已知边界（没做 / 做不了）

1. **`EffectPool` 是全局池，不是每角色池**。强 boss 一套 3 层特效连发会把
   小怪的火花挤掉 —— 这是**有意的**（优先级是下一步的事，`EffectSlotState`
   已经留了 `priority` 字段，`acquire` 也收它，但抢占策略还没实现）。
2. **伤害数字还没接**。`.veffect` 没有 `damageNumber` 字段 —— 它需要
   文字渲染 + 世界→屏幕投影，属于 A9 反馈层，等 P2 的攻击结算出来再接。
3. **`SeqEvent` 入口没接**。过场时间轴的事件表（`SequenceAsset`）目前只有
   marker，没有"到这一刻播特效"。三条入口里这条是空的（运行时有
   `svc.act.playSequence`，但序列自己的事件不驱动特效）。
4. **`fx.play` 面板不扫目录**：三个预设是硬编码的。打包后资产是 `pak:` 虚拟
   路径，扫不了 —— 想要"列出所有特效"得走 `AssetManager` 的清单。
5. **`.veffect` 不进资产闭包扫描**（G29-a 的 `AssetScan`）。它只在
   `.json` 字符串里被引用时才被扫到；如果特效路径是**脚本拼出来**的
   （`"fx/" + name`），打包会漏。写死路径即可（与材质同一个规矩）。
6. **运行时的闪白只有一层纯色**（不叠 bloom / 不按命中点偏色）。够用，
   但不是"影视级"的冲击帧。
7. **`EffectSinks::flash` 在 Runtime 里画在 UI canvas 上**，分辨率跟随输出
   尺寸；编辑器那条走 ImGui 背景层。两边的**视觉**会有极轻微差别
   （alpha 混合精度），`begin=/end=` 的数值语义完全一致。
8. **宿主实体在 Hierarchy 里可见**（名字 `FX_HOST_0..7`）。它们不是场景内容、
   不会落盘（`entityToJsonImpl` 开头就跳过），但作者在**编辑态**点过
   `fx.play` / 面板预设之后，那 8 个实体会出现在层级树与"Emitters in this
   scene"列表里。Play 态不受影响（池建在运行态副本上）。
   下一步：给 `FxHostComponent` 加一条"层级树不列"的过滤（与热重载用的
   临时实体同一条路）。

---

## 十、下一步

- **优先级抢占**：池满时高 `priority` 的请求可以挤掉低优先级的（现在是一律
  丢弃）。⚠ 必须**确定性**：同帧多次请求按 `(priority, 到达顺序)` 排序，
  否则"同一串操作两次跑出不同结果"。
- **伤害数字**（P2 攻击结算出来后）：`EffectSinks` 加第五个 sink，
  世界坐标 → 屏幕坐标走 `runtime/ui/Hud` 已有的那条投影（⚠ Y 只翻一次）。
- **`SeqEvent` → 特效**：过场时间轴的 marker 加一个 `action` 字段，
  解析复用 `parseAnimEventAction`（同一份 kind 表，别写第二份）。
- **每角色特效预算**：`EffectRequest` 加 `owner` 字段，池按 owner 分组限额
  （"小怪的火花不会被 boss 挤光"）。
