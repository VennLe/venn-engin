# 面板自由拖拽 / 合并 —— E29 路线与进度（2026-10-27 起）

> 用户需求原话：「各个功能面板应该是灵活可变的，且支持自由拖拽和合并区域的，
> 就和 blender 中的差不多」。这是六条 UI 改造需求里**唯一没在视觉改造那一轮
> 做完**的一条，因为它不是配色/圆角，而是**布局模型**的替换。

现状一句话（2026-10-27 收工）：分栏能拖**分隔条**（改比例 / 双击折叠 / 右键
预设 / Shift+Space 最大化）；**面板能拖到别格** —— 拖到**中部**并成一个标签组，
拖到**边缘**整格交换；标签组里点标签切换、双击标签送回原位。三刀全部落地。

---

## 0. 为什么不能直接上 ImGui docking

两条路线对比（当时也是这么决定的）：

| | 做法 | 代价 | 结论 |
|---|---|---|---|
| A | 换 `third_party/imgui` 为 docking 分支 | 20+ 面板的 `Begin/SetNextWindowPos` 全部改成 `DockSpace` 语义；锚点锚在旧矩形上 → 一批像素用例要重写；**本机无外网，分支拉不下来** | 否 |
| **B** | `SplitLayout` 从「固定 3 节点树」升级成「**可变树 + 槽位→面板映射**」 | 不动第三方代码、锚点体系保留、**可以一刀一刀落地** | **是** |

## 1. 三刀拆分

| 刀 | 内容 | 状态 |
|---|---|---|
| **E29-1** | 槽位 → 面板的映射**运行时可写** + 落盘 | ✅ 2026-10-27 |
| **E29-2** | 面板标题栏是抓手，拖到别格松手即**交换** | ✅ 2026-10-27 |
| **E29-3** | 一格容纳多个面板 + 标签条（点切换 / 双击拆回），中部落点 = 合并 | ✅ 2026-10-27 |
| **E29-5** | **角落抓手**：分隔条**交点**上拖 = 合并/换位（光标变加号），其余线段仍调大小 | ✅ 2026-10-28 |

⚠ 每一刀都必须补 `--selftest`（本轮是 `MYVK_LAYOUT_TEST=1`）+ `verify_*`。
理由：本仓反复踩过「**布局变了但用例仍然绿**」—— 像素用例按 rect 点控件，
布局一漂就静默点偏。

---

## 2. E29-1 做了什么（✅）

### 2.1 数据结构

`SplitNode` 的叶子从**一个 int** 变成**面板组**：

```cpp
std::vector<int> groupA, groupB;   // 存槽位值（kSlot*）
int activeA = 0, activeB = 0;      // 组内当前显示的下标（E29-3 的标签页选中项）
int panelA() const;                // 派生的"活动面板"，空组 → kSlotNone
int panelB() const;
```

`resolve()` / `syncPanelVisibility()` / tooltip / 最大化全部改走 `panelA()`，
所以「组里装几个」对它们透明。

### 2.2 公开 API（E29-2 用的就是这几个）

```cpp
bool isLeaf(node, side);
int  groupSize(node, side);   int groupPanel(node, side, idx);   int activeIndex(node, side);
bool setGroup(node, side, panels, active);       // 整格替换（会跨位置去重）
bool activateInGroup(node, side, idx);
bool locatePanel(panel, &node, &side, &idx);
bool movePanel(panel, node, side);               // 搬家：源摘掉 → 追加到目标并激活
bool swapActive(nA, sA, nB, sB);                 // 交换两侧的活动面板
void restoreGroups(const std::vector<int> (&g)[kNodeCount][2]);  // 读盘：整份替换
```

### 2.3 落盘

`LayoutState` 加 `slotGroups[kLayoutSplitNodes][2]`（**存面板 id 字符串，不存下标**）。

```json
"slots": [{"a": ["modes"], "b": []},
          {"a": ["viewport"], "b": []},
          {"a": ["inspector"], "b": ["hierarchy"]}]
```

* **只在"与默认树不同"时才写** —— 没动过布局的用户，文件与改造前逐字节相同。
* 老文件没有这一段 → 保持默认树（**不覆盖**）。
* ⚠ `kSlotModes` 是**合成槽位**（模式栏），没有同名 `PanelDesc`，而且八个模式
  参数面板共用 `layoutSlot == kSlotModes`。所以 id 反查会命中 `landscape`。
  落盘用专用 id `moderail`，读盘**兼容别名 `modes`**。

---

## 3. 三个踩过的坑（都是"看起来对"的那种）

### 3.1 逐格 `setGroup` 互换 = 互相摘掉对方

第一版读盘是「对每个 (node, side) 调一次 setGroup」。互换场景下：

```
setGroup(2,A,[inspector]) → 清空 A（原 hier），加 inspector 时发现它在 (2,B) → 跳过 → A 空
setGroup(2,B,[hierarchy]) → 清空 B（原 insp），加 hierarchy → 成功
结果：右栏只剩一格
```

而当时那条断言 `insp.y < hier.y` **还是绿的**（insp 是 0 矩形，0 < 394）。
是旁边那条「两格同宽同列」把它抓出来的。
→ 读盘必须 `restoreGroups()` 一次性替换整棵树。

### 3.2 `apply()` 必须先把树内槽位清 0

`LayoutRects` 是**逐帧复用**的。面板被搬走/合并后，那个槽位在树里不存在了，
不清零就会留着上一帧的矩形 → 同一个面板在旧位置和新位置**各画一遍**。

### 3.3 断言写错两次，实现是对的

* `rect_vanished_slot_zeroed` 断言 `inspector.w == 0`，实际 330 —— 搬走的是
  Hierarchy，消失的是 **Viewport**（它和 Hierarchy 同组，活动项变成后者）。
* `LAYOUT-SLOTS applied=2` —— `applied` 数的是**文件里指定并成功解析的条目数**
  （4 个），不是"换了几个"。

两次都印证本仓纪律：**FAIL 先怀疑「期望」再怀疑「实现」**。

---

## 4. 怎么验

```bash
# 纯数值（不开窗口、不要 GPU）：33 条
MYVK_LAYOUT_TEST=1 build/bin/Editor.exe

# 端到端：A 自检 + B 落盘生效 + C 对照组
python tools/verify_layout_slots.py

# 端到端：A 自检 F 组 + B **真鼠标拖** + C 对照组（只点不拖）
python tools/verify_layout_drag.py
```

`verify_layout_slots.py` 的 C 组是**对照区**：同一份文件删掉 `slots` 段 →
`hierarchy` 必须回到上面、且日志里没有 `LAYOUT-SLOTS`。没有这条就分不清
「是 slots 生效了」还是「本来就那样」。

配套回归：`verify_layout_v4.py`（默认布局的像素/anchor 断言）未受影响。

---

## 5. E29-2 做了什么（✅）

### 5.1 为什么**不用** ImGui 的 `BeginDragDropSource`

那是**挂在 item 上**的 API（要求 `g.LastItemData` 是刚提交的 item），而本工程的
面板是普通顶层窗口，标题栏是 ImGui 在 `Begin()` 内部画的，不是 item。想用它只有
两条路：

| 做法 | 代价 |
|---|---|
| 在窗口内容区塞一个覆盖标题栏的 `InvisibleButton` | 抢标题栏原有命中（折叠 / × / 双击），还要掰 `ClipRect` |
| 铺一层全屏 overlay 当 source/target | 抢整个编辑器的输入 |

两条都是「为了用框架的 API 去改框架的布局」。所以改成**只读鼠标状态**
（`IsMouseClicked / IsMouseDown / MousePos`）自己维护一个状态机 —— 不提交任何
item，标题栏原有交互一个都不动。树内面板本来就是 `NoMove`，所以 ImGui 那边也
不会同时拖窗口，没有"两个拖拽打架"的问题。

### 5.2 关键语义：**槽位矩形 = 面板矩形**

`LayoutRects` 的字段是**按面板槽位**写的（`SplitLayout::writePanel`），不是按
"格子"。所以面板被搬到别的格子后，它的矩形自动跟着走。于是：

* 抓手 / 落点的命中直接用 `L.hierarchy / L.inspector / ...`，**不需要知道树结构**；
* 树只在**落下那一刻**用一次（`locatePanel` 找源与目标的位置）。

### 5.3 抓手、落点、阈值

| | 判定 | 为什么 |
|---|---|---|
| 抓手 | 面板矩形**顶部 `titleH` 像素**（`FontSize + FramePadding.y*2`） | 不去取 `ImGuiWindow::TitleBarRect()`：那要先按名字找到每个面板窗口，而名字各不相同（翻译后的标题 / `###mode_rail` / 视口动态标题）。槽位矩形本身就是窗口矩形，顶部必然是标题栏 |
| 落点 | 面板**整个矩形** | 拖到哪一格就在哪一格松手（Blender 手感） |
| 阈值 | 位移 > **6px** 才算拖动 | 与 `io.MouseDragThreshold` 默认值一致 |
| 取消 | **Esc** | 少了它，中途按 Esc 再松开会在松手处照样交换 |
| 落下 | `swapActive`（**交换**，不是搬走） | E29-3 之后"搬走"会让源格变空、越拖越空；交换则格子数恒定 |

⚠ **`kSlotModes` 只能当落点，不能当抓手**：它是合成槽位（8 个模式参数面板共用
一块），"把模式栏拖走"没有语义；而且它顶部是 9 行模式单选，按下去首先得保证
*点击切模式* 不被拖拽吞掉。

### 5.4 落点高亮

画在 `ForegroundDrawList`（和分隔条同一条理由：要压过所有面板窗口）：

* 源格：`accent` 6% 填充 + 30% 描边（"这一格的东西正在被搬走"）；
* 目标格：16% 填充 + 95% 描边 2.5px（"松手就换到这里"）；
* 圆角取 `ui::metrics().windowRounding`，跟随主题。

### 5.5 实测（`verify_layout_drag.py`）

```
拖前  modes=(0,70,454,716)     hier=(1253,70,325,301)  insp=(1253,371,325,415)
拖后  modes=(1253,371,325,415) hier=(1253,70,325,301)  insp=(0,70,454,716)
LAYOUT-DRAG from=inspector to=moderail
```

两格矩形**逐位互换**（`modes` 拖后 = `insp` 拖前），Hierarchy 一位没动 ——
「换的是面板，不是格子」。

---

## 6. E29-3 做了什么（✅）

### 6.1 ⚠⚠ 决定：标签条**画在标题行里**，面板矩形一个像素都没动

原计划是"格顶画 22px 标签条、面板矩形下移 22px"。真动手时改成**重叠绘制**：

| | 下移 22px（原计划） | 画在标题行里（实际做法） |
|---|---|---|
| 面板高度 | 每个面板矮 22px | 不变 |
| 像素用例 | 所有"按矩形点控件"的用例集体漂移 | **零漂移**（`verify_layout_v4` 全 PASS 就是证据） |
| 视觉 | 标签条 + 窗口自己的标题栏 = 46px **双层 chrome** | 一层：标签条盖住标题文字，就是 Blender 的样子 |
| 代价 | — | 标题栏原有交互（折叠箭头 / ×）被标签挡住；但树内面板本来就是 `NoCollapse`，× 在右侧留了 26px 没盖 |

实现上就是 `ForegroundDrawList` 在这一格顶部 `titleH` 里画几个圆角 pill。
**能不能点中**由 `tabAtPoint` 判（纯函数，自检里跑）。

### 6.2 手势：中部 = 并入，边缘 = 交换（Blender 语义）

| 落点 | 动作 | 实现 |
|---|---|---|
| 目标格**中部**（四周各留 25% 边） | 并入目标格，成为标签 | `SplitLayout::movePanel`（源格被摘空 → 自动收成 0 宽） |
| 目标格**边缘** | 整格交换（E29-2 的老行为） | `SplitLayout::swapActive` |

⚠ 边距不是死板的 25%：夹在 `[min(8px, 40%), 40%]` 之间。窄栏（右栏只有 330px、
模式栏 454px）按 25% 算中部还有 80px，够用；但**极窄的格子**（拖窄到 100px）
纯按比例算中部落点只剩 2px，用户根本对不准。

⚠⚠ **中部落在"不能当标签"的格子上 → 退回整格交换**，不是"什么都不做"。
否则用户分不清是被拒绝还是没接住。视口 / 模式栏就属于这类（见 6.3）。

### 6.3 两条硬约束：视口与模式栏**不能当标签成员**

`canBeTabMember(slot)` 只放行 `hierarchy / inspector`：

* **视口**：非活动标签**不绘制** —— 视口一旦被并进去，用户看到的就是
  "3D 视口消失了"，只会以为引擎坏了。它仍然**能拖**（整格交换），只是不能被合并。
  ⚠ `canDragSlot` 与 `canBeTabMember` **刻意是两个函数**："能拖 ≠ 能并"。
* **模式栏**：合成槽位（8 个模式参数面板共用一块），整块就是"当前模式 + 参数"，
  拖走它没有语义。

### 6.4 ⚠⚠ 退路：双击标签 = 送回老家（否则是单向门）

面板并进别的格子后，**它原来那一格被收成 0 宽**（E06-5 的既有机制）——
鼠标再也点不到那里。没有退路的话，用户并一次就少一格，只能 Reset Layout。

所以 `homeCellOf(slot)` 给四个树内槽位各定了一个"老家"（与 `buildDefault`
的四个叶子一一对应，**改那边必须改这边**），双击标签 → `movePanel` 回家。
一一对应是这条路能成立的唯一理由：四个槽位恰好四个叶子，送回去永远有地方放。

### 6.5 ⚠⚠ `trId()` 返回的是 `"译文###stableId"`

第一版直接拿 `trId("Hierarchy", "panel_hierarchy")` 当标签文字 → 画面上是
`Hierarchy###panel_hierarchy`，而且 `CalcTextSize` **按整串算宽**（两倍多）→
第二枚标签被"放不下就截断"的规则挤掉，**标签组里只剩第一个面板的标签**。
症状极具误导性（数据层完全正常，只是"少画了一个"）。
修法：`tabVisibleLabel()` 在 `"##"` 处截断。

### 6.6 落盘：活动标签靠**顺序**记住

`restoreGroups` 读盘时一律把 `activeIndex` 归 0（它要"顺序无关"，见 §3.1），
所以"哪个标签是当前显示的"只能靠**组成员顺序**：写盘时把活动面板写在**最前面**。
不这么写的话，重开编辑器永远显示第一个成员。

### 6.7 实测（`verify_layout_drag.py`，22 条）

```
A 自检 97 条全绿（原 58 + G 组 39）
D 真拖合并：
  D2 LAYOUT-MERGE from=hierarchy to=inspector size=2
  D3 hier=(1270,70,330,772)  ← 原 insp=(1270,394,330,448)，合并后整格给它
  D4 insp=(0,0,0,0)          ← 非活动标签不占矩形
  D5/D6 两个标签锚点都在：hier x=1369..1459 / insp x=1276..1365
  D7 点 Inspector 标签 → LAYOUT-TABCLICK panel=inspector idx=0
  D8 insp=(1270,70,330,772) hier.w=0   ← 换人显示
  D9 双击 Hierarchy 标签 → LAYOUT-TABHOME panel=hierarchy -> 2/0
  D10 hier=(1270,70,330,324) insp=(1270,394,330,448)  ← 拆回上下两格
B 交换（E29-2 的老行为，落到模式栏中部 → 因为它不能当标签成员，退回交换）
verify_layout_v4.py   全部 PASS（默认布局零像素漂移）
verify_layout_slots.py 全部 PASS
```

---

## 7. 还没做的

1. **切分新格**（拖到边缘 = 新建一栏）。现在边缘是"交换"，真正的切分要先让
   `SplitNode` 能动态增删（树是定长 3 节点的，`kNodeCount` 参与落盘数组长度）。
2. **标签拖出**：把标签从组里拖到别的格子，现在要先双击回家再拖。
3. **工作区切换**时槽位映射保持不动（只改比例/显隐）——要不要"切工作区 = 恢复
   该工作区的映射"，等真正有人抱怨再说。
4. `m_tabBarRect`（视口上方的 ViewportTabs 窗口）与新的标签条是两套东西，
   将来可以合并成一套（都有"点标签切换"的语义）。

---

## 8. E29-5 角落抓手（✅ 2026-10-28）

### 8.1 需求原文

> 「关于引擎编辑器中拖拽面板更换面板显示位置这块，当鼠标放在两个面板的角的
>   交界时往左或往右鼠标拖拽时显示成一个加号，这个时候往左拖拽意味着左边
>   区域面板被合并到左边，反之亦然。而在除了角落的其他交接处位置，则是正常
>   地调整面板区域的显示面积大小。」

一句话：**把一条分隔条切成两种手势** —— 交点是"合并"，其余是"调大小"。

### 8.2 为什么这不是"加个判断"就完了

改动前实测（`GetCursorInfo` 逐点读）：

| 位置 | 光标 |
|---|---|
| SPLIT-0 中段 | `SIZEWE` |
| SPLIT-1 中段 | `SIZEWE` |
| SPLIT-2 中段 | `SIZENS` |
| **T 交点 (1270,394)** | **`SIZENS`** ← 谁先处理谁赢 |

交点处两条线**都能命中**，而 `processSplitters` 是"子树先于父节点"递归，
所以 T 交点永远被横线吃掉（`m_splitHoverTaken`）。这让"角"根本不是一个
可寻址的位置 —— 想在那儿挂第二个手势，必须先**把命中权抢下来**。

### 8.3 做法

1. **先算线，再算交，最后才画线**（`handleSplitters` 里的顺序）
   - `collectCornerLines()` 只收集线段（竖线 `{x, y0, y1}`、横线 `{y, x0, x1}`），
     公式与 `resolve` / `processSplitters` 逐字一致；
   - 横 × 竖求交，交点必须**落在两条线的跨度内**（容差 1.5px，见 8.5 坑①）；
   - 被浮层（Content 抽屉 / `+` 菜单 / 浮动面板）盖住的交点直接剔除 ——
     否则会有一个加号浮在菜单上面。
2. **命中排在 processSplitters 之前**：命中就把 `m_splitHoverTaken = true`，
   于是所有节点的 `hovered` 都是 false（`hovered` 本来就要 `!m_splitHoverTaken`）。
   **一行都不用改线的判定逻辑**，也不存在"两条线同时响应"。
3. **光标**：`ImGuiMouseCursor_ResizeAll`（Windows 的 `IDC_SIZEALL`，四向箭头，
   看着就是一个加号）+ 自绘的加号（静息 3px 臂、悬停带圆角底块）。
   光标管"鼠标底下那点像素"，自绘管"不在鼠标底下也能看见这儿有个抓手"。
4. **方向 → 源/目标**（`cornerDragStep`）
   - 主轴 = |dx| 与 |dy| 大的那个；拖向左 = 目标在左、源在右；
   - 源/目标必须是**贴着这条线**的格子（`cellBesideCorner`）；
   - T 形交点同侧有两个候选时，取**格心离鼠标更近**的那个 —— 手偏上就是
     想动上面那格，比按面积/固定顺序猜都更符合直觉。
5. **落点动作复用标题栏拖拽那一套**：双方都能当标签成员 → `Merge`
   （`movePanel`），否则 → `Swap`（`swapActive`）。见 8.4。

### 8.4 ⚠ 默认布局下"往左拖"是**交换**，不是合并（这不是偷懒）

默认树里唯一的交点是 `Viewport | Hierarchy(上) / Inspector(下)`。往左拖 →
目标格是**视口**，而视口不能当标签成员：`movePanel` 会**激活**被搬进来的
面板，视口一旦变成非活动标签，3D 画面就整块消失（E29-3 定下的硬约束）。
所以横向拖一律退回"整格交换"（`LAYOUT-CORNER-DRAG`），纵向拖才是真合并
（`LAYOUT-CORNER-MERGE`）。

要"往左也合并"就得先放开视口这一条，并且得同时改 `movePanel` 的激活性
（否则并进去 = 视口消失）。这条**没有**在这次动 —— 它是个产品决定。

### 8.5 三个坑

① **容差 1.5px**：T 交点的横线端点在数学上正好等于竖线坐标，但比例是浮点
   算出来的（`bx = x + w * ratio`）。不留容差，窗口一缩放就"有的尺寸下有角、
   有的没有"。

② **交点在拖拽中会消失**：合并之后那一侧收成 0，横线没了，角也就没了。
   所以 `CornerDragState` 里把按下时的 `cornerX/cornerY` **存下来**，拖拽
   全程用存下来的值 —— 每帧重读 `corners()[i]` 会把源判到别处（更糟的是
   越界）。

③ **`kCornerHit = 12px`**：太小（6~8）用户怼不准 4px 的交点；太大（16+）
   会把分隔条两端各吃掉一截，"在边上调大小"反而变难。

### 8.6 判据

`tools/verify_layout_drag.py` **F 组 9 条**（真窗口）：

| 断言 | 判据 |
|---|---|
| F1 | `SPLIT-CORNER-0` 的中心 == (SPLIT-1.x, SPLIT-2.y)，差 < 3px |
| F2 | 悬停角上 → `GetCursorInfo` = `SIZEALL`（用户要的"加号"） |
| F3 | 悬停线上（离角 60px+）→ `SIZENS`（**对照**：边没被角抢走） |
| F4 | 线上往下拖 80px → 横线 y 394 → 474（真的在调大小） |
| F4b | 同一时刻角落日志 0 → 0（边 ≠ 角） |
| F5 | 角上往下拖 → `LAYOUT-CORNER-MERGE from=hierarchy to=inspector size=2` |
| F6 | 右栏收成一格：`hier.w=330 / insp.w=0` |
| F7 | 角上往左拖 → `LAYOUT-CORNER-DRAG from=hierarchy to=viewport`（视口格整格交换） |

`MYVK_LAYOUT_TEST=1` 自检**H 组 15 条**（不碰 ImGui，守映射）：

```
corner_armed_on_click / corner_drag_active_after_move
corner_down_target=inspector / corner_down_source=hierarchy
corner_down_is_merge / corner_down_applied / corner_merge_group_size=2
corner_left_target=viewport / corner_left_source=hierarchy
corner_left_falls_back_to_swap / corner_swap_applied
corner_click_is_not_a_drag / corner_click_kept_tree
corner_diagonal_uses_dominant_axis   ← 斜拖 dx=-30/dy=+70 必须按纵向合并
```

⚠ `verify_layout_drag` 的 F 组**换了一次会话**：F5 把右栏合成一格之后横线
就没了，角点自然消失（这是正确行为）—— 拿同一个会话去测"往左拖"会读到
一个不存在的角。
