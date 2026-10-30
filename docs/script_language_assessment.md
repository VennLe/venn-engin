# 脚本能力评估 —— 「引擎的脚本是不是一片空白」

> 定稿 2026-10-30。回答的是**能力边界**，不是"语言好不好看"。
> 实测口径写在每张表下面；数字全部来自 `--selftest script` 与源码 grep，
> 不是估计值。**改动前请重跑，别拿这份文档当基准**（数字会漂）。

---

## 0 · 结论先行

**不是一片空白。但也不是"能用"。**

| 问题 | 实测答案 |
|---|---|
| 有没有脚本语言 | **有**。`.vks`，自研树遍历 VM，`engine/script/` 共 **6861 行** |
| 做完到什么程度 | S0~S6 六个阶段全部落地，语言层有 `let` / `if` / `while` / `foreach` / `on` 钩子 / 协程 / 六种值类型 |
| 内建函数 | **84 个**（`kBuiltins[]` 表长，含数学 17） |
| 判据 | `--selftest script` **93 / 0** |
| 示例脚本 | 4 个（巡逻兵 / 定时门 / 拾取物 / HUD 计数） |
| **能碰到几个组件** | **6 / 42**（直连可写）＝ **14 %** |
| 有没有调试器 | **没有**。无断点、无单步、无调用栈 |

一句话：**语言是够的，手不够长。**

作为对比，业界三层的分工是这样的：

| 引擎 | 内核语言 | 脚本层 | 可视化 |
|---|---|---|---|
| UE5 | C++ | 蓝图（本质是 C++ 的另一种写法） | 蓝图图表 |
| Unity | C++（不开放） | C# | 无（靠第三方） |
| **本引擎** | **C++（全可达）** | **`.vks`（14 % 可达）** | **无** |

所以用户问的"要能灵活控制组件和参数"这件事，**C++ 那一层已经做到了**
—— 42 个组件全都能改。缺的是**中间那一层的手**：`.vks` 只伸得到 6 个。
而中间这层的全部价值就是"改一行不用重编译重启"。

---

## 1 · 三层编程接口，各自现在能干什么

```
  L0  C++（engine/ 全部）          42/42 组件 · 全字段 · 改一行 = 重编译 58 s
       │
  L1  .vks（engine/script/）        6/42 组件 · 84 个动作 · 改一行 = 0.5 s 热重载
       │
  L2  可视化（蓝图 / 节点图）        ❌ 不存在
```

**L1 已经拿到的**（这部分不要重做，`script_roadmap.md` 的 S0~S6 是真的做完了）：

| 维度 | 状态 |
|---|---|
| 跨帧状态 | ✅ `ScriptInstance::vars` 按实体独立，`let` 幂等声明 |
| 值类型 | ✅ `num` / `bool` / `str` / `vec3` / `entity` / `list` |
| 控制流 | ✅ `if` / `else` / `while` / `foreach` |
| 钩子 | ✅ `on start / update / collide / trigger / exit` |
| 协程 | ✅ `wait` / `waitFrames` / `waitUntil`（帧栈挂起，不阻塞帧、不引线程） |
| 热重载 | ✅ 0.5 s 轮询，**成功才替换**；结构变化时按 `Stmt::id` 对账，对不上整体重启 |
| 熔断 | ✅ 死循环 3 帧内停，后续帧不再执行（P4） |
| 编辑器可见 | ✅ Scripts 面板 + Instances 页签（状态 / 变量数 / Reset） |
| 确定性 | ✅ 逐位可复现（树遍历序 = AST 序，无哈希表迭代） |
| 双宿主一致 | ✅ 编辑器 Play 与 `Game.exe` 共用 `ScriptRuntime` 一个内核（P2 根治） |

---

## 2 · 真实缺口：三层，严重程度递减

### 缺口 1（**致命**）· 手短 —— 组件覆盖 14 %

`.vks` 能**直接**读写的组件只有这 6 个：

| 组件 | 能做到 | 做不到 |
|---|---|---|
| `TransformComponent` | `pos/setPos` `rot/setRot/rotY` `scale/scaleOf` `move/moveBy` `rotate/rotateBy` `scale/scaleBy` | — |
| `VisibilityComponent` | `visible/setVisible` | — |
| `PointLightComponent` | `setColor/setIntensity/setRange` | — |
| `SpotLightComponent` | 上面三个 + `setDirection` | — |
| `MaterialComponent` | `setEmissive`（**只有自发光**） | 其余全部 PBR 因子、贴图槽 |
| `ScriptVarsComponent` | `attr/setAttr`（脚本私有黑板，**不是组件参数**） | — |
| `NameComponent` / `HierarchyComponent` | 只读（`name()` / 隐式 self） | 写 |

**其余 27 个组件，`.vks` 一行代码都没有。** 其中最扎眼的是：

- **`CameraComponent` —— 29 个字段，可达 0 个。**
  刚刚做完的运镜体系（焦距 / 快门 / 景深 / 路径 / 手持）**一条脚本都改不了**。
  想写"被击中时镜头抖一下、焦距推到 85 mm"，只能回 C++。
- **`ParticleEmitterComponent` —— 0 个。** 脚本点不着特效。
- **`PhysicsMaterialComponent` / `CollisionComponent` —— 0 个。** 改不了摩擦力、开不了碰撞。
- **`DirectionalLightComponent` —— 0 个。** `setColor` 只认 point/spot，主光（太阳）改不了。
- **`TriggerComponent` —— 0 个。** 能收 `on trigger` 事件，但改不了触发体积的大小。
- **`TerrainComponent` / `WaterComponent` / `DecalComponent` / `ReflectionProbeComponent` —— 0 个。**

⚠ 这里还藏着一个**会咬人的静默失败**：`setIntensity` 对没有灯的实体**返回 0、不报错**。
实体上只有 `DirectionalLightComponent` 时，脚本写 `setIntensity(2.0)` 会"成功"但一点反应
都没有 —— 和"脚本没跑"长得一模一样。这是缺口 1 的次生伤害。

### 缺口 2（**高**）· 手不顺 —— 写脚本要离开引擎

| 缺什么 | 现状 | 代价 |
|---|---|---|
| 内联编辑器 | **没有**。Scripts 面板只是一个"已加载列表 + reload 按钮" | 改一行要切到 VSCode，再切回来按 reload |
| 语法高亮 / 补全 | 没有 | 84 个函数名靠背；拼错只在**编译期**报（`unknown function`），但热重载是"成功才替换"→ 旧版本还在跑，你以为改了 |
| 断点 / 单步 / 调用栈 | **完全没有**（grep `breakpoint` 零命中） | 唯一手段是 `print` + Instances 面板看状态 |
| 变量检视 | Instances 页签只显示**变量个数**，不显示内容 | 想知道 `hp` 现在多少，得 `print` 出来 |
| 面板内 API 参考 | 没有。全靠手抄 `docs/script_api.md` | 84 个函数的签名在外面，写的时候要开第二个窗口 |

### 缺口 3（中）· 找不到手 —— 可发现性

84 个内建函数**在引擎里没有任何地方列出来**。Scripts 面板顶多一行
`Language: .vks (see engine/script/ScriptEngine.h)` —— 让用户去看头文件。

---

## 3 · 四条路线，逐条算账

### 路线 A · 继续扩 `.vks` 的绑定（**推荐**）

不换语言，只把"手"接到剩下的 36 个组件上。

| 项 | 内容 |
|---|---|
| 成本 | 中。核心是加一层**字段投影表**，不是加 84×N 个函数 |
| 收益 | 直接。这是唯一一条**不需要重编译就能改全部参数**的路 |
| 风险 | 低。`ScriptHost` 窄接口的结构性安全**继续保留**（见 §4 为什么） |
| 破坏性 | 零。老脚本逐位不变（新函数只增不改） |

### 路线 B · 换 Lua / Wren

`script_roadmap.md` §2 已经论证过一次，这里只补**现在**的判断。

**继续自研的三条理由仍然成立**：
1. 类型系统级的封闭 —— `ScriptHost` 里没有 `void*`、没有反射表，脚本拿不到门面就永远改不了
   引擎全局状态。换 Lua 之后这条要靠注册表逐条小心，**漏一个就是后门**。
2. 确定性免费 —— 现在是逐位可复现的，回归能逐字符比日志。Lua 的表遍历顺序不保证。
3. 零第三方源码 —— 离线构建，`third_party` 里没有 VM。

**退出条件（原文照抄，不重复讨论）**：需要脚本里定义函数/结构体且已写满 3 个关卡、
需要真正的调试器、需要第三方生态 —— **三条里出现两条就换**。

**现在的判断：一条都没满足**（4 个示例脚本、0 个关卡、没有生态诉求）。
⚠ 但**缺口 2 的"要调试器"这一条正在靠近** —— 如果将来为了调试器而换 Lua，
那说明的其实是"自研 VM 上做调试器不划算"，而不是"语言不够用"。

**结论：现在不换。** 换了不解决缺口 1（组件覆盖），只解决缺口 2 的一部分，
却要重写值/语句/求值三层。

### 路线 C · 可视化脚本（蓝图式节点图）

| 项 | 内容 |
|---|---|
| 成本 | **最高**。节点图 = 序列化格式 + 画布 + 连线命中 + 类型推导 + 编译到 VM，比 S0~S6 全部加起来还贵 |
| 收益 | 对**非程序员**高，对本项目（用户自己是唯一开发者）低 |
| 风险 | 高。节点图一旦成为主流入口，`.vks` 会变成"底层实现"被雪藏 |
| 冲突 | 与确定性、离线、零第三方三条纪律都不冲突，但与"一个人维护"冲突 |

**结论：现在不做，但不封死。** 若将来 `.vks` 的字段投影层做完了，节点图可以**编译到同一份
AST**（即节点图只是另一套前端），成本就从"重写一遍"降到"多一个前端"。这是它唯一合理的
插入时机 —— **在投影层之后，不在之前**。

### 路线 D · C++ 热重载（脚本 = 编译成 DLL）

| 项 | 内容 |
|---|---|
| 成本 | 中（跨平台/ABI 是坑，Windows-only 还行） |
| 收益 | 能写任意逻辑、性能最好 |
| 风险 | **极高**。崩溃 = 整个编辑器挂掉，状态全丢；32/64 位、MSVC 版本、增量链接全是雷 |
| 冲突 | 与"编辑态不跑脚本、坏了不能冻住游戏"这条铁律正面冲突 |

**结论：不做。** `.vks` 熔断只停一个实例，DLL 崩了要重启编辑器 —— 这个差距不是性能能换的。

---

## 4 · 推荐方案：**白名单字段投影层**（路线 A 的具体形态）

### 4.1 为什么不能直接做反射（回应 `script_roadmap.md` 的 S-D5）

S-D5 写着「世界访问 = 窄接口 + 黑板，**不做**反射式 `getComponent(e, "Health")`」，
理由是：引擎没有类型注册表，且会破坏安全边界。

**这条纪律是对的，不要推翻。** 但它有一个容易误读的地方：它反对的是
**"任何组件任何字段"的无白名单反射**，而不是"用表来描述字段"。

区别在这张图上：

```
  ❌ 反射（S-D5 反对的）              ✅ 投影表（本方案）
  脚本 ──► "给我 Health 组件"          脚本 ──► "camera.focal = 35"
         ──► 遍历类型注册表                   ──► 查 kProjections 白名单
         ──► 拿到 void*                        ──► 命中 → 写那个字段
         ──► 偏移 + sizeof 硬算                ──► 未命中 → **编译期报错**
         ──► 任何字段都能碰（含内部指针）
```

**投影表保留了"结构上封闭"这个优点**：表里没有的字段，脚本**根本写不出来**
——不是运行期拒绝，是**编译期就找不到这个名字**。同时它把"每加一个字段要写
5 处"（enum / kBuiltins / 求值分支 / 头注释 / 文档）降成"加一行"。

### 4.2 脚本侧的形态：`prop(e, "组件.字段", 值)`

```vks
# 读：两种字段都给（省一次乘法/比较）
let f = prop(cam, "camera.focal")          # 35

# 写
setProp(cam, "camera.focal", 85)           # 推焦
setProp(cam, "camera.shutterAngle", 90)    # 快门
setProp(cam, "light.directional.intensity", 2.0)   # 主光（现在完全够不着）
setProp(self(), "collision.layer", 4)
```

**为什么是字符串而不是 `cameraFocal(e, v)` 这种逐个函数**：
- 84 个组件的字段加起来是**几百个**，逐个函数会把 84 涨到 500+，`kBuiltins` 表失去可读性；
- 字符串在**编译期**被解析成 `(组分, 字段分)` 枚举对（沿用 S-D6 的"内建函数名编译期解析"），
  所以拼错仍然是**编译期**错误，不是运行期；
- 与已有的 `attr/setAttr`（黑板）在**语法上分得开**：带点的是引擎字段，不带点是脚本私有。

⚠ **硬约束**：`prop` 的字段名**必须**在编译期解析（走 `findBuiltin` 同一条路径）。
写成运行期字符串比较，就等于把 P5（拼错只在运行期报）从函数名扩散到字段名 —— 那是退步。

### 4.3 C++ 侧：一张表描述一个组件的可写字段

```cpp
// engine/script/PropProjection.h（新增）
struct PropField {
    const char* name;          // "focal"
    enum class T { F32, I32, Bool, Vec3, Str } type;
    std::size_t offset;        // offsetof 结果
    float lo, hi, def;         // 范围与默认（UI 滑条与"0 = 不设"的判据）
};
struct PropGroup {
    const char* name;          // "camera"
    // 该组的实体必须真有这个组件才可写 —— 没有就**报错**，不静默
    bool (*hasGroup)(ecs::World&, ecs::Entity);
};
```

**为什么用 `offsetof` 而不是 getter/setter 函数指针**：字段数量是几百，
getter/setter 要写几百个小函数；`offsetof` 只写一行数据。而真正的类型安全由
**`PropField::T` 在编译期核对**（`setProp` 的第二个参数是 double，投影层按
`T` 做范围夹取与类型转换，不匹配报错）。

⚠ 这条设计的代价要写下来：`offsetof` 对**非标准布局类型**是未定义行为。
所以 `Components.h` 里所有被投影的字段必须满足 POD/标准布局 —— 现在它们是
（全是 `float`/`bool`/`int`/`glm::vec3`），将来若某个组件加了 `std::string` 成员
就必须改用 getter。**`static_assert(std::is_standard_layout_v<T>)` 兜住。**

### 4.4 覆盖面目标：从 14 % 到多少

不建议一次铺满 42 个组件。按"玩法需要什么"排：

| 期 | 组件 | 为什么排这个顺序 |
|---|---|---|
| **P1** | `Camera`(29) + `DirectionalLight` + `Decal`(4) | 运镜刚做完、主光改不了最荒谬、贴花是最便宜的视觉反馈 |
| **P2** | `ParticleEmitter`(16) + `PhysicsMaterial`(5) + `Collision`(6) | 特效与手感，是"看起来像个游戏"的分界线 |
| **P3** | `Terrain`(8) + `Water`(10) + `ReflectionProbe`(6) | 环境调整，脚本驱动的少，优先级低 |
| **不投影** | `PrefabLink` / `GameStart` / `Locked` / `PlayerLimb` / `ScriptVars` | 编辑器内部状态或脚本自己的东西，暴露出去只会被误用 |

做完 P1 是 **10 / 42**，做完 P2 是 **16 / 42（38 %）**，而**玩法相关的组件基本齐了**。

---

## 5 · 分阶段执行序（不可跳）

### P1 · 投影层内核（无新组件，先证明机制本身）

1. `engine/script/PropProjection.h/.cpp`：表结构 + `findField` 编译期解析 + 范围夹取；
2. `kBuiltins` 加 `prop` / `setProp`（arity 2 / 3）；
3. **只接 `TransformComponent` 一个组的 6 个字段** —— 它已经有 `pos/setPos` 等专用函数，
   所以可以**逐位对照**：`setProp(e,"transform.pos",v)` 与 `setPos(e,v)` 必须完全等价。
   这条对照是"投影层没有引入语义漂移"的唯一硬证据。
4. 判据：`--selftest script` 新增一组，含"未命中字段必须编译期报错"的**否定测试**。

### P2 · 相机 + 主光 + 贴花（把最荒谬的缺口补上）

5. `Camera` 组 29 个字段全接（`focal` / `shutterAngle` / `dofEnabled` / `fStop` /
   `clipNear` / `clipFar` / `handheld*` / `lookDamping` …）；
6. `DirectionalLight`（`color` / `intensity` / `direction` / `shadows`）——
   顺带修 §2 里那个**静默失败**：`setIntensity` 打空时要报错，不能返回 0；
7. `Decal`（`color` / `opacity` / `size` / `rotation`）；
8. 判据：脚本改焦距 → `CINE-STATE` 的 `fovEff` 跟着变（跨系统联动的证据，
   光断言"字段变了"证明不了渲染真的用了它）。

### P3 · 特效与手感

9. `ParticleEmitter`（`rate` / `life` / `size` / `color` / `gravity` / 发射器开关）；
10. `PhysicsMaterial`（`friction` / `restitution`）+ `Collision`（`layer` / `mask` / `isTrigger`）；
11. ⚠ 这两组会牵动物理世界的**在线重建**（当前 `SceneBinding` 是整体重建）——
    改摩擦系数要不要重建？这是 P3 最大的设计问题，不要顺手做。

### P4 · 缺口 2：写起来顺手（可与 P2/P3 并行）

12. **面板内 API 参考**：Scripts 面板加一页 `Reference`，**从 `kBuiltins` 表直接生成**
    （不是手写文档）—— 84 个函数名/arity/一句话说明，点一下插到剪贴板；
13. **变量检视**：Instances 页签展开时列出 `vars` 的键值（现在是只数个数量）；
14. **内联编辑器**：Scripts 面板右侧加 `InputTextMultiline` 源码编辑 + 保存 + 自动 reload。
    ⚠ 不做语法高亮也能用；但**必须**有"保存失败 / 编译失败"的行号提示。

### P5 · 调试（最后做，因为最贵）

15. **断点**：`ScriptInstance` 加 `std::set<uint32_t> breakLines`（按 `Stmt::line`），
    `runFrame` 每步检查 → 命中置 `Waiting` 并记 `pendingBreak`；
16. **单步**：`stepOver` 标志，执行一条语句后再次挂起；
17. **调用栈 / 变量**：协程帧栈里已有 `Stmt::id`，能把 `id` 翻回行号。

⚠ P5 的价值要打折：这套 VM 是**每实体单协程 + 无函数调用**（S-D4/S-D9），
所以"调用栈"永远只有一层 —— 断点能停、单步能走，但**没有栈可看**。
真正需要的其实是 **P4 的变量检视**，不是 P5。

---

## 6 · 判据（做完怎么证明）

| # | 判据 | 落点 |
|---|---|---|
| 1 | `--selftest script` 从 93 涨到 ≥ 130，**0 fail** | 数字 |
| 2 | `setProp(e,"transform.pos",v)` 与 `setPos(e,v)` **逐位相等** | 语义无漂移 |
| 3 | 写一个**不存在的字段名** → **编译期**失败（否定测试） | 投影表真的在拦 |
| 4 | 脚本改 `camera.focal` → 运行时 `CINE-STATE` 的 `fovEff` 跟着变 | 跨系统联动 |
| 5 | 脚本改 `directional.intensity` → 画面亮度**显著变化像素占比** > 阈值 | 渲染真的用了它 |
| 6 | 对没有该组件的实体 `setProp` → **报错**（不是返回 0） | 修掉静默失败 |
| 7 | 老脚本（4 个 `.vks`）逐位不变，`sceneHash` 不变 | 不破坏 |
| 8 | `verify_i18n_coverage` PASS（新面板文字要翻译） | 五件套 |
| 9 | 投影表里的组件全部满足 `is_standard_layout` | `offsetof` 的前提 |

---

## 7 · 明确不做（写下来为了不反复讨论）

| 不做 | 原因 |
|---|---|
| 换 Lua / Wren | 三条退出条件一条都没满足；换了也不解决组件覆盖 |
| 反射式 `getComponent(e,"X")` | 破坏结构性封闭（S-D5）；投影表是它的**有白名单替代** |
| 用户自定义函数 / 递归 | 要调用栈 + 作用域 + return（S-D4） |
| 可视化节点图 | 成本高于 S0~S6 总和；**只能在投影层之后**作为第二套前端 |
| C++ 热重载 DLL | 崩溃即编辑器全挂，与熔断纪律冲突 |
| 脚本创建 / 销毁实体 | 生命周期与存档一致性（`spawn` 已有，销毁另议） |
| 编辑器编辑态跑脚本 | 破坏编辑/运行分离（S-D10） |

---

## 8 · 一页纸回答用户的问题

- **"是不是一片空白"** → 不是。`.vks` 有 6861 行、84 个内建、93 条自检全绿，
  能写巡逻兵/定时门/拾取物，热重载 0.5 s。
- **"要能灵活控制组件和参数"** → 这正是**缺的那一半**。42 个组件只够得着 6 个（14 %）。
  相机 29 个字段一个都碰不到 —— 运镜体系刚做完，脚本却改不了焦距。
- **"UE5 是 C++、Unity 是 C#"** → 本引擎的 C++ **就是**那一层，且全可达；
  `.vks` 相当于"轻量脚本层"，定位更接近 UE 的蓝图但不做节点图。
- **"整体方案"** → 不换语言，做**白名单字段投影层**（`prop/setProp` + 一张表），
  把手从 6 个组件伸到 16 个（38 %），顺带补面板内 API 参考与变量检视。
  **先 P1 用 Transform 做逐位对照证明机制**，再接相机。
