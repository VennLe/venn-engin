# 旗标表 + 脚本断点调试（A4 / C8，2026-11-02）

> 两件事合在一篇：它们是同一条调试链的两端 ——
> **旗标**回答"这一局游戏记住了什么"，**断点**回答"它现在为什么这么走"。
> 只补其中一个，剧情调试仍然要靠 `print`。

- 验收：`tools/verify_flags.py`（A~E，23 条内核 + 5 组界面）
  与 `tools/verify_script_debug.py`（A~G）
- 面板：`Debug ▸ Flags`（`editor/FlagTablePanel.*`）、
  `Debug ▸ Script debug`（`editor/ScriptDebugPanel.*`）
- 命令：`flags.*` 7 条 + `script.bp` / `script.at` / `script.cont` /
  `script.step` / `script.select`

---

## 一、A4 补的是什么

### 1.1 真正的问题不在面板

`grep -rn "setFlag\|getFlag" editor/` 在 2026-11-02 之前是 **0 命中**。
写"缺一块面板"是对的，但补的时候发现更严重的一层：

> **编辑器 Play 态根本没接 `svc.act.setFlag`。**

`grep -on "svc.act.*" editor/EditorApp.cpp` 只有 `axis / held / joints /
player / pressed / released`。于是 Play 里 `setFlag("x",1)` 报的是

```
setFlag() is not available in this host
```

**不崩、不中断，剧情分支静默全走 else。** 表现成"剧本写错了"，
而作者会去翻剧本 —— 那份剧本一点问题都没有。

所以 A4 的交付顺序是：**先接 host，再补面板**。面板只解决"看得见"，
不解决"调得动"。验收 C 组因此有一条**反向判据**：

```
[C] 没有 'not available in this host'
```

这条比任何"读到了值"的正向断言都值钱 —— 正向断言在**别的路径**写进去时也会绿。

### 1.2 `GameFlags` 为什么从 `runtime/` 搬到 `engine/scene/`

依赖图铁律：`runtime → engine ← editor`，**`editor/` 不许 include `runtime/`**。

旗标真身原来在 `runtime/GameFlags.h`。而编辑器 Play 也是宿主 —— 不搬的话
"编辑器里能调剧本分支"这件事在**类型层面**就不可能成立，面板再好也只是看一张
永远空的表。

现在：

- 真身 → `engine/scene/GameFlags.h`（`namespace scene`）
- `runtime/GameFlags.h` 降级成一行垫片，旧调用点零改动：
  ```cpp
  #pragma once
  #include "scene/GameFlags.h"
  namespace runtime { using GameFlags = scene::GameFlags; }
  ```

### 1.3 每个键记一个 `writer`

`FlagEntry { double value; std::string writer; }`。

调试一张几十项的旗标表时，**"这个值是谁写的"是最常问的问题**，而它只能在
写入那一刻知道 —— 事后推断不出来（同一帧里脚本、UI、控制台都可能写）。

| writer 取值 | 来源 |
|---|---|
| `script:scripts/ch02.vks` | 脚本 `setFlag`（取 `ScriptRuntime::currentScriptPath()`） |
| `ui` | UI 动作 `Flag` / `Toggle`（`dispatchUiAction`） |
| `editor` | 面板里改值 |
| `console` | `flags.set` 命令 |
| `host` | 其它宿主（Game.exe 的 `RuntimeApp`） |

`writer` **不参与存档** —— 它是"这一次运行"的调试信息，存进存档会让下次读档
显示一个上一局的写入者。

### 1.4 "类型"是看出来的，表里没有类型位

与 `project::DataTable` 没有类型行同一条理由：类型本来就是从值推出来的。

```cpp
inline const char* flagKindName(double v) {
    if (v == 0.0 || v == 1.0) return "bool";
    if (v == static_cast<double>(static_cast<long long>(v))) return "int";
    return "real";
}
```

只用于显示。`0` / `1` 一律显示 `bool` —— 旗标里最常见的用法就是开关，
把它显示成 `int` 只会让人多问一句"这两个是不是同一个东西"。

### 1.5 语义细节（都是踩过才知道的）

| 行为 | 为什么 |
|---|---|
| `getFlag` 未设过 = **0** | 脚本里 `if (getFlag("x"))` 的默认语义。所以面板/命令必须**同时**报 `set=0/1` 才能区分"设成 0"与"从没设过" |
| 快照 = **整表替换** | "回到某章开头"的语义：那一章之后才出现的旗标**不该还在**。恢复时新增的键必须消失（G6d） |
| 进入 Play **清空**旗标 | = 新开一局。不清的话"上一局见过的人"会带进下一局，而且**不报错** |
| Stop **保留**旗标 | "刚才那一局到底设了哪些旗标"是调试时最想事后翻的，一按 Stop 就清等于销毁证物 |
| 同名 `set` **覆盖**不累加 | `setFlag` 是赋值不是自增；要自增就写 `setFlag("x", getFlag("x")+1)` |

### 1.6 面板与命令

`FLAG-*` 锚点 11 个：`RECT / FILTER / NONZERO / COUNT / SNAP / RESTORE /
CLEAR / EMPTY / VALUE / LIST / STATUS`。

命令**全部走面板的方法**（`m_flagPanel->set(...)`），与按钮同一条路 ——
"能点不能存"是这类面板最常见的缺陷，分开写两套的话判据只覆盖其中一条。

```
flags.count
flags.list [filter]
flags.get <name>
flags.set <name> <value>
flags.erase <name>
flags.clear
flags.snap
flags.restore
```

`setFlag` 的写日志受 `MYVK_LOG_FLAGS=1` 控制 —— 每帧写同一个键会把日志刷爆。

---

## 二、C8 补的是什么

`grep -rn breakpoint` 全仓 **0 命中**。`.vks` 有实例表（Scripts 面板能看 vars），
但"停在第几行、那一刻是什么状态"只能靠 `print` —— 而 `print` 恰恰看不见
**没走到的那一行**。

### 2.1 断点 = (脚本路径, 行号)

按**路径**存，不按 `ScriptProgram*`：热重载会重建 program（`clear()` 后地址
可能复用），而"第 12 行"在作者眼里从来是相对文件的。

`BreakpointSet` 内部是 `std::map<std::string, std::set<int>>`。

### 2.2 行号必须命中一条真语句

`executableLines()` 顺手在 `indexAst()` 里收集（它已经在遍历全部语句）。

在空行 / 注释行 / 纯括号行上点断点**永远不会命中**，而"点了没反应"是最难自证
的失败 —— 症状是"断点功能坏了"，日志里一条错都没有。

所以 `addBreakpoint` 校验行号，不命中就**拒收并报原因**：

```
SCR-BP-ADD-FAIL path=scripts/x.vks line=1 err=line 1 has no statement in 'scripts/x.vks'
```

面板里那些不可打断的行画成灰色、**不给点** —— 比"给点但不生效"诚实。

### 2.3 三个必须写对的时序

#### ① 断点检查必须在**执行之前、ip 未动之前**

任何"先执行再停"都会让被停住的语句执行两次：
`setFlag("x", getFlag("x")+1)` 变成 +2，而且看起来只是"数字不对"。

#### ② `drive()` 里必须是 if / else-if 链

```cpp
if (m_coro.pauseSkipLine == st0.line) {
    m_coro.pauseSkipLine = 0;   // 放行这一次
} else if (m_coro.stepPause) { ... return Step::Paused; }
else if (m_bps && !m_bps->empty() && m_bps->has(m_bpPath, st0.line)) {
    ... return Step::Paused;
}
```

写成两段独立 `if` 的话，"继续"的放行标记会被消费掉、紧接着断点检查又命中原行
→ 表现成**"点继续原地不动"**，且不报错。

#### ③ 暂停时**不能**置 `inst.started = true`

```cpp
if (!ctx.paused) inst.started = true;
```

断点若落在 `on start` 里，置位后 `in.runStart` 变假，恢复时 `case 1` 整段跳过
—— "停在 start 第 3 行"变成"start 剩几行再也不跑"，一条错都不报。

### 2.4 `Paused` 与 `Suspend`（wait）分开报

wait 是"脚本自己要等"，paused 是"调试器摁住了"。两者对 `executed` / `started`
的处理不同，混在一起会让"被摁住"被算成"执行了"。

### 2.5 日志去重

不暂停的实例下一帧在 `runFrame` 开头直接早退，但**仍把 `ctx.paused` 置真**
→ 不按"上一帧是否已停着"去重的话，每帧刷一条 `SCR-PAUSE`
（而这条日志正是自动化唯一能读到行号的地方）。

```cpp
const bool wasPaused = inst.paused;
...
if (!wasPaused) VK_LOG_INFO("SCR-PAUSE entity=%u path=%s line=%d steps=%llu", ...);
```

### 2.6 放行只能走自由函数

`ScriptCoroutine` 的布局只在 `ScriptEngine.cpp` 里完整 → `ScriptRuntime` 摸不到
它的字段。所以：

```cpp
bool resumeCoroutine(ScriptCoroutine& coro);   // 清 pauseSkipLine，放行一次
bool stepCoroutine(ScriptCoroutine& coro);     // 置 stepPause，走一步再停
```

与 `ScriptEngine::makeCoroutine()` 同一条理由。

### 2.7 面板与命令

`SCRDBG-*` 锚点 12 个。源码视图按行切（**吃掉 `\r`**），
`>` 标暂停行、`*` 标断点行，只有 `executableLines()` 里的行可点。

⚠ 锚点打**两套**：

| 锚点 | 含义 |
|---|---|
| `SCRDBG-LINE` | 只有**第一条**可打断行（`logRect` 按 tag 去重） |
| `SCRDBG-LINE-<行号>` | 每一行一条 |

第二套是必需的：只有一套时自动化只能点到第一可打断行，而那一行往往是
`let x = 0`（顶层相位，只跑一次）—— 想在 Play 里验证"这个断点真的会停"
就必须点得到 `on update` 里的那一行。

```
script.bp <add|remove|list|clear> [path] [line]
script.at
script.cont
script.step
script.select <path>
```

---

## 三、判据表（哪些断言是真判据）

| 组 | 断言 | 为什么它不可替代 |
|---|---|---|
| A4-A | 23 条内核 | 未设过=0 / 覆盖不累加 / writer 记最后一次 / 快照整表替换 |
| A4-B | 计数序列 **`[2,3,1,0]`** | 分开断言"某处出现过"会被后面的值碰巧满足 |
| A4-B | 未设过 → `set=0` | 只比 `value` 的话，"把值清零但键留着"照样绿 |
| A4-C | **没有 `not available in this host`** | 反向判据；正向断言在别的路径写进去时也绿 |
| A4-C | `ticks >= 20` | 证明**每帧**在写，不是"进 Play 时写了一次"（实测 119） |
| A4-D | 进 Play 后 `keeper` 是 `set=0` | 同上，必须是 `set` 而不是 `value` |
| A4-E | 点 `FLAG-CLEAR` → `EDITOR-FLAG clear removed=1` | "界面点不到 = 没交付" |
| C8-B | 注释行 **被拒收** | 静默收下永不命中的断点 = "功能坏了"且无错 |
| C8-D | 继续后 **`ticks` 变大**（39→40） | 唯一能区分"执行一次"与"只清了暂停位/执行两次"的量 |
| C8-E | 单步后 **行号必须变**（14→15） | 不变 = "单步等于继续"，两种写法都不报错 |
| C8-F | 不加断点 → **0 条 `SCR-PAUSE`** | 阴性对照；没有它，C/D/E 的暂停可能来自熔断 |
| C8-G | 鼠标点行 → 加断点 → 进 Play **真停在那儿** | 完整闭环；只断言多加了一行日志的话，"从不查断点表"的实现也能绿 |

---

## 四、已知边界

1. **Play 态下这两张面板不画。**
   `EditorApp::drawPanels()` 里 `hosted = isEditing() ? viewportTabDrawnPanel() : -1`
   —— Play 期间内容区永远是 3D 视口（P8），所有 `kSlotNone` 标签页面板整块
   不派发。所以"在 Play 里点源码行加断点"做不到：断点要在编辑态加。
   验收 G 组因此是**编辑态点击 → 点 Play → 看它停住**。

2. **断点表跨 Play 存活，但不落盘。** 关掉编辑器就没了。
   （落盘要面对"脚本改名/删掉之后那条断点归谁"的问题，现在没有这个价值。）

3. **旗标不落盘。** A4 只管"这一局"；存档里的旗标是 C3/G17-3 的事。

4. **`executableLines()` 只按 `Stmt::line` 收集。**
   一条语句写在两行（跨行的函数调用）时，断点落在**起始行**。

5. **面板改值不校验类型。** `InputDouble` 写进去什么就是什么；
   `flagKindName` 会跟着变（写 2.5 就从 `bool` 变 `real`）。

---

## 五、自动化里的坑（写给下一个改验收的人）

| 坑 | 症状 | 正解 |
|---|---|---|
| Play 态面板不画 | 8 个锚点全无，看着像"面板坏了" | 编辑态跑，或直接断言"面板不画"是设计 |
| 编辑态启动不传 `scene` | 点 Play 跑默认场景 → 脚本不 tick | `scene=PROBE_SCENE_REL` |
| `logRect` 不判可见性 | 行有矩形但点在子窗口外 → 什么也没发生 | 先滚（`scroll_into_box`），再断言矩形落在 `SCRDBG-SRC` 内 |
| 判据被命令自己满足 | 点击完全没生效也 PASS | 命令碰**另一个行号** |
| 点击次数为偶数 | 断点加了又去掉，进 Play 不停 | 先点一次看效果，没中才再点 |
| 编辑器没有"进 Play"日志 | FAIL 分不清"断点不生效"与"Play 没点中" | 先 `flags.set` 塞一个键，用 `EDITOR-FLAG new-session` 当指示灯 |
| 等面板锚点而不是等命令 | 命令一条都还没跑 | `wait_for` 等**最后一条命令自己的日志行** |
| 写死夹具行号 | `line 13 has no statement` | 从 `PROBE_SRC.splitlines().index(...)` 推导 |
