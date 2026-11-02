# UI 事件回调（UI actions，B1）

> 2026-11-02。对应审计文档 `panel_gap_review_2026-11-02.md` 的 B1 条。
> 判据：`tools/verify_ui_actions.py`（五组）。
> 引擎：`engine/ui2/UiAction.*`（解析）+ `engine/ui2/UiDocInput.*`（命中/派发）；
> 宿主：`runtime/RuntimeApp.cpp`（执行）；编辑器：`editor/UiEditorWindow.cpp`。

## 1. 它补的是什么洞

`UiDocument`（#312）从 10-27 起就能让作者在 `.ui.json` 里摆控件、能画、
能按绑定值显示血量。但**按钮是纯装饰**：

```
grep -rn "action" engine/ui2/     →  0 命中（2026-11-02 之前）
```

`UiWidget` 里没有 `action`，也没有任何命中测试。于是"摆一个能点的按钮"
这件事只能写 C++ —— 而唯一能点的 `UiButton` + `UiFocus`（U2/U6）服务的是
**暂停菜单那条硬编码路径**，跟文档层完全不通。

这是典型的"能力在、入口零"，但比 PostVolume / 混音台更贵：那两条是"能力
在、面板没有"，这条是**能力根本不在 UI 层**。背包 / 卡池 / 商店 / 主菜单
—— 二次元游戏 UI 的大头 —— 全部卡在这里。

## 2. 怎么用

### 2.1 在文档里写

```json
{
  "id": "btn_start",
  "type": "button",
  "rect": [100, 100, 240, 64],
  "text": "Start",
  "action": "flag:menuStart=1",
  "enabled": true,
  "visibleBind": "hasSave"
}
```

三个新字段：

| 字段 | 含义 |
|---|---|
| `action` | 点一下做什么。留空 = 纯装饰（但**仍是可交互的**，见 §3.6） |
| `enabled` | `false` = 照样画（变暗），但点击被忽略 |
| `visibleBind` | 填了就只有那个**旗标非零**时才显示；留空 = 一直显示 |

### 2.2 四个动词

| 写法 | 做什么 | 谁执行 |
|---|---|---|
| `flag:hp=10` | 旗标 `hp` 置 10（省略 `=10` 就是 1） | `GameFlags::set` |
| `toggle:musicOn` | 0↔1 翻转（`cur != 0 ? 0 : 1`） | `GameFlags::set` |
| `sound:audio/click.wav` | 在 SFX 总线播一个一次性音效（参数是**资产路径**） | `AudioSystem::playOneShot` |
| `quit` | 退出游戏（**唯一不带冒号**的动词） | `svc.quitRequested` |

`flag:` 的值走 `strtod` 且**必须整串吃掉** —— `flag:hp=10abc` 用 `atof`
会静默得到 10，于是"写错了"表现成"值对了语义不对"，最难查。

### 2.3 在编辑器里写

`Window ▸ UI editor` → 选中控件 → **Interaction** 段：

- `Action`：边打字边校验，**写错当场标红拒绝**（与加载路径不同，见 §3.6）
- `Enabled`：勾选框
- `Visible when flag`：填旗标名（留空 = 一直显示）

### 2.4 自动化（`MYVK_RUNTIME_UICLICK`）

```
MYVK_RUNTIME_UI_DOC=ui/docs/action_demo.ui.json \
MYVK_RUNTIME_UICLICK="220,132;220,232" \
Game.exe --frames 340 --start-state playing
```

坐标是**参考像素**（1920×1080 画布，与编辑器的设计视图同一套坐标），
不是屏幕像素。注入的点击**排队**、在第 20 帧起每 30 帧发一次 —— 首帧之前
既没有输出尺寸也没算过布局，当场做命中测试必然 miss，而 miss 会被误读成
"按钮没画上"。

## 3. 语义细节（容易踩的）

### 3.1 命中测试必须**倒序**

`placeDocument` 输出的是 DFS 序（父在前、子在后），后画的盖在上面。正序扫
会让"被面板盖住的按钮"抢走点击 —— 而玩家看到的是面板。

### 3.2 按下与松开必须在**同一个**控件上

按下后拖出去再松开**不算点击**（所有桌面 UI 都这样）。`core::Input` 只给
`mouseDown` / `mousePressed`，**没有 released**，所以 `m_uiDocMouseDownPrev`
自己合成松开沿。

⚠ 顺序是"先处理按下、再处理松开"：注入的点击在同一帧里 press+release 都
置位，反过来处理会读到已经清空的 `m_pressId` → **点了没反应，而真实鼠标
一切正常**。这种"只出现在自动化里"的 bug 最难认。

### 3.3 为什么**没有** `emit:` 动词

评估过"把事件交给脚本"，最后砍掉。原因不是懒，是时序：

`GameServices::uiAction` 这一格由 `tickUi` **每帧 `clear()`**，而文档层的
派发发生在**渲染阶段**（`after onUpdate`）—— 往里写的事件，下一帧在状态栈
读到之前就被清掉了。加一个中转变量能救，但那是给一个**本就不需要**的动词
加一层状态：这一层的脚本本来就是"每帧轮询"模型（同 `AudioSystem::voicePlaying()`
的头注释），`flag:menuStart=1` + 脚本轮询 `getFlag("menuStart")` 语义一样，
而且旗标还能进存档。真想做成事件，应该连 `uiAction` 的清理时机一起改，
那是另一件事。

### 3.4 `visible == false` 与 `enabled == false` 都不命中

前者由 `placeDocument` **直接跳过**（连 `UIDOC-BOUNDS` 记录都不产出），
后者在命中测试里查。禁用还能命中是最糟的一种 —— "点了没反应"会被误判成
"按钮坏了"。

⚠ 布局与绘制**必须用同一个判据**（`widgetVisible`）。两边各写一份的后果
不是"少画一个"，而是**整棵树错位一格**：`drawRec` 靠"记录序号"与树同步
推进，漏一条之后所有控件都画到邻居的位置上。

### 3.5 设计画布**忽略** `visibleBind`

`placeDocumentAt(..., honorVisibleBind = false)`。空绑定表查不到键 → 判为
不可见 → 作者一配"按旗标隐藏"，控件就在设计视图里消失，**连选都选不到**
（框不到它，属性面板也进不去）。设计视图要的是"摆得中"，不是"和运行时逐像素
相同"。

### 3.6 写错的动作：加载**只警告**，编辑**当场拒**

- 加载：一条 `action` 拼错不该把"某个按钮点不动"升级成"进游戏一片空白"
  → 降级成 `warnings`，控件变回纯装饰。
- 编辑器：作者正在打字，立刻标红才知道自己写错了。

### 3.7 没配 action 的 Button **仍然可交互**

这样点上去能打一条 `UIDOC-NOACTION`（每个 id **只打一次**），而不是让一个
看起来能按的按钮静默吃掉点击。`UIDOC-CLICK` 的 `action=(none)` 是同一件事
的另一个视角。

### 3.8 点空白**也要**打日志

`UIDOC-CLICK hit=0 x= y=` 必须打 —— 否则"点歪了"和"按钮没画"这两件事在
日志里一模一样。

### 3.9 ⚠⚠ 真鼠标与注入点击走的是**两条路**

- **注入**：`requestClickRef` 排队 → 在 `update()` 里直接覆盖 px/py，
  **不看 `m_mouseCaptured`**。
- **真鼠标**：先过 `tickUi` 的"点击视口 → 进入操作态"（①b）。

2026-11-02 加这条功能时踩到的真 bug：①b 原来是**无条件**捕获
（`m_mouseCaptured = true` + 藏光标），于是 Playing 下点文档按钮：

```
第 N 帧   tickUi ①b：mousePressed → m_mouseCaptured = true（顺手藏光标）
第 N 帧   渲染回调：tickUiDocInput → 看到 m_mouseCaptured → 整段跳过真鼠标
```

**注入点击照样能点，鼠标点不动。** 现在 ①b 先问文档层（同一个
`uiDocHitAt`），命中可交互控件就不捕获，打
`GAME-MOUSE captured=0 reason=ui-doc id=<id>`；点在空白处仍然照旧进入操作态
（`captured=1 reason=click`）。这对锚点就是 `verify_ui_actions.py` E 组的判据。

## 4. 判据（`tools/verify_ui_actions.py`，五组）

| 组 | 量什么 | 关键判据 |
|---|---|---|
| A | 无窗口自检 | `UI2-TOTAL pass>=109 fail=0` + 23 条 `uiint_*` 全 PASS + 17 个关键用例名都在 |
| B | 注入点击（一次启动 9 下） | `flag:` 的值 = 1.0000；`toggle` 连点两次 = **1 → 0**；`sound` 到达分支；`enabled=false` → `hit=0` 且动作一次都不出现；点空白 → `hit=0` 且坐标 = 参考像素×scale；`NOACTION` **只有一条** |
| C | 条件可见 | 置旗标**之前** `panel_secret` 不在 `UIDOC-BOUNDS` 里，之后出现；`widgets 7→9`、`skipped 1→0` |
| D | `quit` | `UIDOC-ACTION kind=quit` → `GAME-STATE ... shutting down`；**对照组**（没点 quit 的那次）必须没有这一条 |
| E | **真鼠标** | 点按钮 → `hit=1` + `GAME-MOUSE captured=0 reason=ui-doc`；点空白 → `captured=1 reason=click` |

⚠ 几条"看起来能省"的断言：

- **`toggle` 必须点两次**。只断言"打出了 `kind=toggle`"的话，"每次翻成 1"
  的实现也绿。
- **C 组不能只断言"最后有 panel_secret"**。那样"永远显示"的实现同样通过。
- **D 组必须有对照组**。否则那条断言只是在量"每次跑完都会打一行"。
- **E 组不能省**。它守的是与注入完全不同的输入路径，也是 3.9 那条 bug 的
  唯一判据。合成鼠标要预热（两次 MOVE + 每步 0.18 s）且必须用
  `mouse_event`（`PostMessage` 的左键在 GLFW 下实测 0/3 命中）。

## 5. 已知边界（**没有**做）

- **没有** `list` / `scroll` / `slider` / `toggle` 控件。几十条目的列表、
  设置里的滑条仍然得写 C++。这是"控件种类"缺口，与本文补的"行为"缺口不是
  一件事（审计 B1 原文里那两条并列写着）。
- **没有**悬停的光标变化（手型）、没有按下音效、没有点击动画 —— 只有
  `hovered / pressed / disabled` 三档调色（1.22 / 0.72 / 0.55）。
- **没有**键盘 / 手柄导航（`UiFocus` 只服务暂停菜单那条路径，文档层没接）。
- **没有**点击穿透控制：点中了 UI 之后，那一下**同时**还是玩法输入
  （射击等）。真正的做法是给 `GameServices` 加一个 `uiConsumed` 位，
  现在没加 —— 所以 HUD 上摆可点按钮时要自己确认不会误开火。
- `sound:` 的参数是**资产路径**，不是音频库里的短名。写错只会得到
  `ok=0`（与"没有声卡"同一个表现），所以最好先在混音台的场景声源段里
  确认那个 clip 能跑。
- `quit` 只是把 `quitRequested` 置位（与暂停菜单的 quit 同一条约定：
  不在回调里关窗口），所以**没有任何"确认退出"的语义**。
