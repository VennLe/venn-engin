# 操作面板缺口审视（2026-10-27）

> 目的：找出**"引擎已经能做到、但用户在面板上摸不到"**的地方。
> 方法见 §1；结论见 §2；每条都带 `文件:行` 证据，可逐条复核。

---

## 1 · 怎么审的

**不按代码行数排**（行数只说明写得长，不说明用户摸得到）。做的是两件事：

1. **能力 → 入口 的反向核对**：拿 `engine/` 里已成型的子系统（物理关节、音频混音、
   存档、UI 文档…），逐条去 `editor/` 找"用户从哪儿点"。找不到入口 = 缺口。
2. **扫"灰项"和"等某个版本"的提示**：这类菜单项最会骗人 —— 功能可能早就做完了，
   菜单还停在当时的注释上。用户看到灰项就以为引擎没有这个能力。

清点范围：**26 张面板**（`EditorApp::buildPanelTable()`）+ **13 个 Inspector 页签**
（`kTabs[]`）+ **7 个主菜单**（`Toolbar::drawMenuBar`）+ **38 个 ECS 组件**。

---

## 2 · 结论：三档 + 一条"零成本"档

| 档 | 含义 | 条数 | 谁来买单 |
|---|---|---|---|
| **P0** | 引擎有能力，面板里**根本没有入口** → 用户彻底用不了 | 3 | 要写新面板 |
| **P1** | 面板在，但**链路是断的**（点了没用 / 只能看不能改） | 4 | 补面板里的细节 |
| **P2** | 菜单/控件**自相矛盾**（灰着的东西其实已经能用了） | 5 | **只接线，不加功能** |

P2 五条加起来不到半天，但收益最大：它们现在是"用户以为引擎没有"的直接来源。

---

## 3 · P0：引擎有能力，面板零入口

### 3.1 关节 / 约束 —— 最贵的一条 ✅ **已完成 10-27 夜**

> **落地情况**（判据 → `tools/verify_joints.py`，A/B/C/D/E/F/G 七组）
>
> 1. `engine/ecs/Components.h`：`JointKind`（Fixed/Ball/Hinge/Slider/Distance）
>    + `JointComponent`（两锚点 / 轴 / distance / 限位 / 马达 / enabled）。
>    **不直接复用 `physics::Joint`** —— 它按刚体槽位下标寻址、且一半字段是
>    求解器的草稿纸（`rows[]` / 累积冲量），存进场景文件等于存随机数。
> 2. `SceneSerializer`：`jointToJson` / `jointFromJson`，`target` 存 **16 位 hex
>    字符串**（不存下标、不存 JSON number）；两处 `hasNonMeshContent` 都加了
>    `joint`（写端读端各一份，漏一处 = `skipped` 数骗人）。
> 3. `ScenePhysics::buildSceneWorld`：**所有刚体建完之后**翻译，三种坏法
>    各打一条明确的 WARN（no-target / target-missing / no-rigid-body），
>    `enabled=false` 静默跳过且**不算** skip。
> 4. Inspector 的 Physics 页签末尾一整段 `Joint`（`INS-JOINT-*` 七个锚点）；
>    "另一端"用**下拉选名字**而不是填 guid。
> 5. `PhysicsPanel` 的关节总览：按场景全局列一遍、三种坏法标红、点一行就
>    选中、右键跳到另一端 / 删除。**这是单端视角看不出来的那部分**。
>
> ✅ **10-27 深夜把这四条补齐了**（判据 → `tools/verify_joints.py` 的 E / F 两组）：
>
> - **视口里点两下拾取锚点**：Inspector 的 `On this object` / `On the other
>   object` 两个按钮（锚点 `INS-JOINT-PICK-SELF` / `INS-JOINT-PICK-TARGET`）把
>   状态写进 `EditorContext`（发起者在 Inspector、消费者在 Viewport，面板之间
>   **只准通过它说话**），`ViewportPanel::handleAnchorPick` 负责换算。三条硬约束：
>   ① 命中点是**世界**坐标，self 端过**本实体**世界矩阵的逆、target 端过
>   **target 实体**的逆（写反 = 锚点看着对、一按 Play 物体被拽到十万八千里）；
>   ② 点空也要能用（锚点常在空气中 —— 摆锤铰点、绳子端点）→ 退到"过该实体
>   原点、正对相机的平面"，日志用 `source=surface|camera-plane` 区分；
>   ③ **Esc 只退拾取、不清选中**（清了 Inspector 变空，用户找不到自己在配哪个
>   对象）。另有一条顺序：**拾取必须排在点选之前**，否则"在别的物体表面上取点"
>   一点就换选中项、Inspector 跟着跳走。
> - **脚本 API 九件**：`joint` / `jointAnchors` / `jointAxis` / `jointLimit` /
>   `jointMotor` / `jointOn` / `jointRemove` / `jointKind` / `jointTarget`，
>   **运行时（`GAME-JOINT`）与编辑器 Play（`EDITOR-JOINT`）都接**。每一条都要
>   **同时**改组件与求解器（`physics::rebuildJoints`，两处重载走同一个
>   `buildJoints`）；**绝不** `buildSceneWorld` 重建（它连刚体一起重来，全场的
>   位置与速度都回到初始值）。
>   ⚠ 内建表的 **arity 就是公开签名**：`jointAnchors` / `jointAxis` 一度声明成
>   7 个 / 4 个参数（六个标量拆开写），按 `vec3` 写的脚本在**编译期**就 not
>   runnable —— 一行运行日志都没有，看着像"脚本根本没跑"。现在两处都收成
>   `vec3`（与 `setPos` / `near` 的位置类参数一致）。
> - **3D 调试绘制**：`RENDER joint-gizmos n=… verts=…`；蓝=自己、橙=另一端、
>   红=没选目标、黄=有一端没刚体、绿=铰链/滑动轴。走**普通 mesh**（吃深度测试，
>   夹在不透明与透明之间），不做 ImGui 叠加层 —— 穿墙可见会让用户误判"锚点在
>   墙外"。默认关（`MYVK_JOINT_GIZMOS` / Appearance 的 `rp.jointGizmos`）。
>   ⚠ **H 组的判据是 `verts` 的精确值，不是"有日志"**：夹具让两端锚点差成
>   **纯 +Z**，故意踩那条"参考轴不能与线段平行"的分支 —— 竖直连杆（摆锤的绳、
>   吊钩的缆）方向就是世界 Z，参考轴仍取 Z 时叉乘是零向量。为了让这个错法
>   **可见**，退化基现在直接 `return`（不提交零尺寸立方体），于是漏掉换向分支
>   就是 `verts=3` 而不是 4。否定测试：把 `fabs(z.z) > 0.99` 恒判 false 重编，
>   H3 当场变红（`verts=3`）—— 这条能红才算数。
> - **四种类型的运动学判据**：E 组 19 条，每种都配**同几何、同初值的无约束
>   对照**。Ball 的"朝向自由"没法直接看（`PHYS-BODY` 只有 pos/vel）→ 把锚点
>   挪出质心 1 m，用"摆到铰点正下方"当**可观测替身**，对照取 **Fixed**（同样锁
>   位置、但朝向也锁 → 摆不下来）；Slider 必须给**横向初速度**再断言"x 不动"，
>   否则自由落体也满足这条，判据是空转的；Distance 用"绳长恒定 + 真的摆下来"
>   两条，对照物直线下落时同一个量从 2 m 涨到 5.8 m。
> - **G 组（视口拾取的真鼠标回归）**：点两下按钮 → 点两下视口，从 `JOINT-PICK
>   apply=` 里取 `world` / `local`，用夹具里**已知**的变换独立算一遍
>   `world = pos + scale·local`。只做平移不做缩放、或者拿错实体矩阵的实现都会
>   在这里红 —— 这是"两种实现会不一样的那个量"。
>   ⚠ 两个坑写进脚本注释了：① `MYVK_EDITOR_PANELS=physics` 会把中间格子换掉，
>   视口从第 3 帧起不再 `draw()`，`VP-RECT` 变成**陈旧值**；② `scroll_tag_into_view`
>   的固定落点 `(客户区宽-100, 高×0.5)` 在本机压在 Inspector 头部的**死区**上，
>   滚 44 次一格不动（目标矩形始终 y=2144、客户区高只有 844）→ 改用**面板自身
>   矩形**的中轴、离底边 60px，每格 90px。③ `logRect` 按矩形**去重**，所以
>   "Esc 之后还画着关节字段"不能靠"再看一眼日志"，必须先滚一下逼它重打。

**证据（改动前）**
- `engine/physics/Joints.h:54,66`：完整的 `JointRow` / `Joint`（锚点、限位、马达）。
- `engine/physics/Joints.cpp`：球窝 / 铰链 / 距离 / 弹簧都实现了，`World::addJoint(j)` 可建。
- 但：`engine/ecs/Components.h` 的 **38 个 `*Component` 里没有 `JointComponent`**；
  `editor/InspectorPanel.cpp` 里 `"Joint"` 出现 **0 次**；脚本 API 里 `joint` **0 命中**。

**用户遇到什么**
门、摆锤、秋千、链条、布娃娃、起重机 —— 游戏里最常见的物理玩法，**一个都做不了**。
引擎侧全部就绪，但没有任何一条路能从游戏工程里**描述**出一个关节：
不能存盘、不能在面板里建、脚本也调不到。等于"只在自检里存在"。

**补什么（建议做成一个闭环，别只做面板）**
1. `JointComponent{ type, targetGuid, anchorA, anchorB, axis, limits, motor }`
2. `SceneSerializer` 读写（父-子用 **guid**，不用下标 —— 见 G16 的教训）
3. Inspector 一段：类型下拉 + 两个锚点（视口里点两下拾取）+ 限位/马达
4. `PhysicsPanel` 加一个"本场景所有关节"列表（能点选、能删、能定位）
5. `verify_joints.py` + `TRIGGERS`

### 3.2 音频总线 / 音量

**证据**
- `engine/audio/Mixer.h`：声部池、总线、重采样、3D 衰减、pan、ducking 全在。
- `engine/audio/AudioSystem.h:34`：`AudioSettings{ master/music/sfx, duckDepth, device… }`。
- 但面板表里**没有 audio 面板**；`duckDepth` / `masterGain` / `busVolume` 在 `editor/` 下
  **零引用**；编辑器起音频时是硬编码 `as.music = 0.7f`（`EditorApp.cpp:1626` 一带）。

**用户遇到什么**
BGM 太吵 → 只能改 C++ 重编译。想关 ducking、想看"现在几个声部在响"、想定位某条
音效谁在循环播放 → **面板上没有任何地方**。音效本身能挂（`AudioSourceComponent` 有
Inspector 段），但**混音台整层是空的**。

**补什么**
Audio 面板：Master / Music / SFX 三根推子 + duck 深度 + 声部池占用（x/32）+ 正在播放
的声部列表（名字、总线、音量、是否 3D）+ 每行一个"停"。运行期改的走 `AudioSystem` 的
setter（它已经备好了，只是没人调）。

### 3.3 打包 / 发布（E28）

**证据**：`Toolbar.cpp:468-471`，`Package... (E28)` 和 `Bake Lighting... (E29)` 包在
`BeginDisabled(true)` 里。

**用户遇到什么**：做完发不出去。这是 E28 整件未做，不只是面板缺 —— 但它在面板上的
表现就是"Build 菜单里两个灰项"，所以必须列在这里。

---

## 4 · P1：面板存在，但链路是断的

### 4.1 Content ▸ Import 只是"把文件拷进来"

**证据**：`ContentBrowser::openImportDialog()`（约 1300-1400 行）全程只有
`fs::copy_file`。**不解析、不校验、不生成 `.vmeta`、没有任何选项**（缩放/朝向/材质映射），
失败也只打一行 WARN。

**用户遇到什么**：拖一个坏 FBX 进来，界面显示"Imported 1 file(s)"——一切正常。
直到把它拖进视口才炸，而那时候已经不知道是文件坏了还是流程错了。

**补什么**：导入后立刻跑一遍解析自检，坏文件当场标红 + 通知里带原因；`.vmeta` 预生成
（省掉"第一次拖进去等 20 秒"）；再给一个可跳过的选项面板（默认值走全自动）。

### 4.2 UI 编辑器：做不出"能点的界面"

**证据**：`engine/ui2/UiDocument.h:69-77` 的 6 种控件（Panel / Nine / Image / Text /
Button / Progress）**没有任何事件字段**；全库搜 `onClick` / `action` → 0。

**用户遇到什么**：按钮画得出来、能高亮、能悬停，**点下去什么都不会发生**。
文本绑定 `{key}` 和 Progress 值绑定有了，所以只能做"显示型"界面；主菜单、背包、
对话框的"继续"全都要写 C++。
（另：无列表/滚动/翻页控件，几十条目的背包即使有事件也排不下。）

**补什么**：控件加 `action` 字段（脚本函数名）+ 编辑器里一个下拉从当前脚本的 `fn` 列表里选
+ 运行期派发。列表/滚动是第二刀。

### 4.3 存档没有槽位

**证据**：G17-1/G17-2 落了存档的序列化与应用层（`docs/persistence_roadmap.md` §12），
但 `editor/` 下**没有槽位概念**（无 `SaveSlot` / 无 Save/Load 按钮 / 无 CLI）。

**用户遇到什么**：存档系统只有引擎作者能用。做游戏的人没法在面板上"存到槽位 1、
读槽位 1"，也没法验证自己配的存档规则。

**补什么**（G17-3）：槽位下拉 + Save/Load/Delete 三个按钮 + `--save-slot N` CLI
（CLI 是为了能用无头脚本断言）。

### 4.4 PhysicsPanel 只有全局旋钮

**证据**：`editor/PhysicsPanel.cpp` 共 245 行，内容 = `PhysicsSettings` 十个旋钮 +
Play 中的计数表（bodies/manifolds/contacts/broadphase pairs/joints…）。

**用户遇到什么**：面板能告诉你"有 37 个接触点"，但**看不到它们在哪儿**。
`DebugRects` 只是 `logRect` 的转发壳（`editor/DebugRects.h`），不是物理调试画。
接触点/流形/关节的 3D 调试绘制没有 → 调物理全靠猜数字。

**补什么**：Play 中按层开关的调试绘制（接触点 + 法线 + AABB）+ 选中物体高亮它的
碰撞体与它参与的关节；配合 3.1 一起做。

---

## 5 · P2：陈旧 / 自相矛盾的菜单（**只接线，不加功能**）

这五条全部躺在 `editor/Toolbar.cpp` 里，修的是"面板在骗人"这件事。

| # | 位置 | 现在长什么样 | 实际情况 |
|---|---|---|---|
| 1 | `Toolbar.cpp:347-355` | `Select All` / `Invert Selection` **灰的**，tooltip 写"multi-selection lands with E11 / E22" | 多选 10-27 就做完了。`EditorContext::selectAll()` 存在，但**全仓只有自检在调**（`MultiSelectSelfTest.cpp:130`）→ Ctrl+A 也没绑 |
| 2 | `Toolbar.cpp:337-340` | `Duplicate` 点下去只弹一句"Use Ctrl+D / the Hierarchy context menu" | 多选复制已实现（`SceneHierarchy.cpp:1415` 的 `selectMany(dups)`）。菜单应该直接做 |
| 3 | `Toolbar.cpp:358-361` | `Project Settings...` 只弹"lands with E26 (quality tiers)" | E26 画质档位面板早就在了（`quality` 面板 + `verify_quality_presets.py`） |
| 4 | `Toolbar.cpp:275-282` | `File ▸ Import ▸ Native file dialog (E28)` **灰的**，注释说"等平台层有原生对话框" | Content 里 `OPENFILENAMEW` 已经跑了很久（`ContentBrowser` 的 `Import...`）。同一个能力，一个入口能用、一个入口灰着 |
| 5 | `Toolbar.cpp:265-267` | `Save As` 写死路径 `"assets/scenes/scene.json"` | **这是个真 bug**：打开 `level2.json` 后按 Save As，会覆盖 `scene.json`，而用户以为"另存了" |

第 5 条不是陈旧，是行为错误 —— 建议优先修（要么接原生另存为对话框，要么至少改成
`<当前名>.copy.json` 并明确提示）。

---

## 6 · P3：观察，先记着（不建议现在动）

- **GameObject 菜单只有 7 个基元 + 2 种灯**，而 `Add Component` 能加 **16** 个组件，
  ECS 里一共 **38** 个。其余组件靠专用流程（导入 / 笔刷 / 角色窗口）。
  新用户想"加个音频源"，路径是：先建 Empty → 切 Inspector 的 Tool 页签 → Add Component。
  **能不能在 GameObject 菜单下面挂一个 `Add Component ▸` 就解决一半**，值得考虑。
- **出生点 `GameStartComponent`** 只能由角色窗口生成，GameObject 菜单里没有"放置出生点"。
- **材质两条路摆在一个菜单里**：`+ ▸ Material (inline in scene)` 是灰的，真材质要走
  `.vmat`（在类型列表里，能用）。挨着放容易误点。
- **Console 没有"按级别过滤"的 UI**：`setSearchFilter()` 存在但只在代码里用；搜索框、
  清除、复制都有。
- **Hierarchy 的过滤支持 `type:` 语法**，但界面上没提示 —— 能力藏起来了。

---

## 7 · 建议顺序

| 顺序 | 做什么 | 为什么排这儿 |
|---|---|---|
| 1 | **P2 五条** | 半天工作量，纯接线。修完"面板不再骗人"，用户的信任成本立刻降 —— ✅ **已完成**（`verify_menu_wiring.py` 14 条） |
| 2 | **P0-1 关节**（组件 + 序列化 + Inspector + 面板列表 + 用例） | 用户能玩的物理玩法从"零"变"全套"，收益最大 —— ✅ **已完成 10-27 夜**（`verify_joints.py` 40 条；剩视口拾取锚点 / 脚本 API） |
| 3 | **P1-1 导入自检 + 选项** | 资产管线的第一道门，坏资产现在是静默通过 |
| 4 | **P0-2 Audio 面板** | 混音台整层是空的，补起来面窄、不用碰渲染 |
| 5 | **P1-2 UI 事件** / **P1-3 存档槽位** | 都依赖已有内核，属于"接线 + 小控件" |
| 6 | **P0-3 打包（E28）** | 独立大件，不和上面抢上下文 |

---

## 附 · 26 张面板清点

| 面板 | 快捷键 | 现状 | 本轮列出的缺口 |
|---|---|---|---|
| Hierarchy | Alt+1 | 树 / 过滤（含 `type:`）/ 拖拽换父 / 锁定+眼睛 / Prefab 右键全套 | — |
| Inspector | Alt+2 | 13 页签 / 8.7k 行 / 属性编辑进撤销栈（`ParamEditCommand`） | ~~无 Joint 段~~✅ 已补（Physics 页签末尾，`INS-JOINT-*`） |
| Content Browser | Alt+3 | 树 + 缩略图 + 13 类型 + 原生导入 + Show References | 导入无自检/无选项（→4.1） |
| Viewport | Alt+4 | 多视口 / 视图模式 / 吸附预设 / 浮层工具栏 | — |
| Stats | Alt+5 | 性能统计 | — |
| Scripts | Alt+6 | 实例与变量查看 | — |
| Engine | Alt+7 | 特性参数（`FeatureUIGen` 从登记表自动生成） | — |
| Console | Alt+8 | 搜索 / 清除 / 复制 | 无级别过滤 UI（→P3） |
| GPU Timeline | Alt+9 | GPU 时戳 | — |
| Character | Alt+0 | 骨架 / 蒙皮 / 动画预览 / 角色装配 | — |
| **Physics** | — | 全局设置 10 旋钮 + 运行计数 + **关节总览** | 调试绘制（→4.4） |
| Particles | — | 1.3k 行，发射器全参数 | — |
| Quality | — | 画质档位 | — |
| Landscape | — | 2.7k 行，笔刷/地形 | — |
| Foliage | — | 植被笔刷 | — |
| Mesh Paint | — | 顶点色笔刷 | — |
| Modeling | — | 挤出/内插/倒角/环切/镜像/模态变换 | — |
| Fracture | — | 破碎 | — |
| Brush Editing | — | 笔刷体编辑 | — |
| Animation | — | 动画编辑模式（层级/事件/根运动在 Inspector） | — |
| PCG | — | 程序化散布 | — |
| Water | — | 水体编辑 | — |
| Material library | — | 材质货架（贴图版） | — |
| Script editor | — | 剧本编辑器 | — |
| Timeline editor | — | 过场时间轴 | — |
| **UI editor** | — | 6 种控件 + 锚点 + 文本/数值绑定 | **无事件回调（→4.2）· 无列表/滚动** |

**没有对应面板的引擎子系统**：~~物理关节（3.1）~~（✅ 10-27 夜已补）、音频混音（3.2）、
存档槽位（4.3）、打包（3.3）、NavMesh/寻路（未登记）。
