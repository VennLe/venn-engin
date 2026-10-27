# 对话 / 字幕（C2）· 一份 JSON 演一段台词

> 2026-10-26 · C 组第 2 刀（`anime_gap_review_2026-10-26.md` 的 C2 行）
> 代码：`engine/dialogue/DialogueAsset.{h,cpp}`（解析 + 加载时校验）、
> `engine/dialogue/DialogueRunner.{h,cpp}`（播放头 / 选项，纯数值）、
> `engine/dialogue/DialoguePlayer.{h,cpp}`（资产所有权 + 打字机 + 两段式推进）、
> `engine/dialogue/DialogueSystem.{h,cpp}`（调度层：谁在播 / cue 优先）、
> `engine/dialogue/DialogueSelfTest.cpp`（97 条数值断言）、
> `engine/ecs/Components.h` 的 `DialogueComponent`、
> `engine/scene/SceneSerializer.cpp`（读写 `dialogue` 块）、
> `runtime/ui/Subtitle.{h,cpp}`（字幕框，ui2 + SDF 字体）、
> `runtime/RuntimeApp.cpp`（输入 / 绘制 / 自动推进）、
> `runtime/PlayerController.{h,cpp}`（对话期间挡跳跃）
> 面板：`editor/DialogueEditorWindow.{h,cpp}`（剧本编辑器，§10）+
> `editor/InspectorPanel.cpp` 的 `drawTabDialogue`（组件配置台）
> 验收：`Game.exe --selftest dialogue`（105 条）+ `tools/verify_dialogue.py`（11 组端到端）
> + `tools/verify_dialogue_editor.py`（Inspector 页签）+ `tools/verify_dialogue_script.py`（编辑器 7 组）
> 示例：`assets/dialogues/ch01_open_01.dlg.json`

---

## 0 · 一句话

**在场景里挂一份 `.dlg.json`（或让过场时间轴 `kind=Line` 点它的名），进 Play
底部就出字幕框：逐字出、空格翻页、有分支就上下选。**

---

## 1 · 30 秒上手

| 步 | 做什么 | 在哪 |
|---|---|---|
| ① | 打开 `assets/scenes/cutscene_demo.json` | Content 面板双击 |
| ② | 选中 `Director`（空物体） | Hierarchy |
| ③ | 直接按 **Play**，等 4 秒 | 时间轴在 t=4.0 发 `Line` 事件 |
| ④ | 底部出字幕，**空格** 翻页 | 打完字再按一下就翻 |
| ⑤ | 出选项时 **↑/↓** 选、**空格** 确认 | 走不同分支 |

想不看时间轴、直接试对话：把 `Director` 上 `dialogue` 块的
`autoStart` 改成 `true`，进 Play 第一帧就开播。

从零自己配：Inspector → **Tool** 页 → **Add Component** → **Dialogue** →
`path` 填 `dialogues/ch01_open_01.dlg.json`（**相对 `assets/`**）。

**没有现成剧本？** 点同一个页签里的 **Edit script…** 打开剧本编辑器（§10），
按 **New script** 就有两行的骨架，写好按 **Save** —— 不必先手写一个空 JSON。

> ⚠ `autoStart=false` 时**进 Play 什么都不会发生** —— 它在等时间轴或脚本
> 点它的名。这不是接线断了，是默认值。排查对话时第一个要排掉的可能。

---

## 2 · 资产格式（`.dlg.json`）

放 `assets/dialogues/`。**文件名就是台词 id** —— 时间轴里
`{"kind":"Line","payload":"ch01_open_01"}` 直接指到这里。

```json
{
  "version": 1,
  "name": "ch01_open_01",
  "start": "l1",
  "lines": [
    { "id": "l1", "speaker": "千夏", "text": "……又是这个梦。", "next": "l2" },
    { "id": "l2", "speaker": "千夏",
      "text": "每次都在同一个地方醒过来。\n今天的风，好像有点不一样。",
      "next": "q1" },
    { "id": "q1", "speaker": "千夏", "text": "先做什么？",
      "choices": [
        { "text": "去天台看看",     "goto": "roof"  },
        { "text": "回教室再睡一会", "goto": "sleep" }
      ] },
    { "id": "roof",      "speaker": "千夏", "text": "……果然，今天的天空不正常。", "next": "roof_end" },
    { "id": "roof_end",  "speaker": "我",   "text": "（这一段的结局还没写。）" },
    { "id": "sleep",     "speaker": "千夏", "text": "……五分钟。就五分钟。", "next": "sleep_end" },
    { "id": "sleep_end", "speaker": "我",   "text": "（结果睡过头了。）" }
  ]
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `id` | ✅ | **全局唯一**。`goto` / `next` 都靠它寻址 |
| `speaker` | ⬜ | 显示名（不是 id）。同一人多处写同一个字符串 |
| `text` | ✅ | 台词本体，可以含 `\n` 换行（字幕框按行数长高） |
| `next` | ⬜ | 说完去哪。**空 = 这一段结束** |
| `choices` | ⬜ | 有它就是分支节点；**此时不能再写 `next`**（加载判错） |

### 四条"看起来能省、省了就错"的语义

| # | 语义 | 省了会怎样 |
|---|---|---|
| ① | 节点按 **`id`** 寻址，不按下标 | 改剧本（中间插一句）后所有 `goto` 整体错位，而且**新剧本能跑**，只是跳错地方 |
| ② | `goto` / `next` 指向不存在的 id = **整份加载失败** | 留到运行期查的症状是"前 20 句都好，第 21 句突然没了" —— 作者得重播整段才复现 |
| ③ | **有选项时 `advance()` 一步都不走** | 写成"可以继续"的话，玩家看到的是"选项闪现一下然后自动选了第一个" |
| ④ | 打字机 `cps <= 0` = **立刻全出**，不是"永远不显示" | `floor(elapsed × 0)` 恒为 0 → 字幕永远是空的，而日志里一切正常 |

---

## 3 · 组件字段

Inspector → Tool → **Dialogue**，或直接写场景 JSON 的 `dialogue` 块：

```json
"dialogue": {
  "path": "dialogues/ch01_open_01.dlg.json",
  "enabled": true,
  "autoStart": false,
  "cps": 24.0,
  "startNode": "",
  "priority": 10
}
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `path` | `""` | 相对 `assets/`。**空 = 组件等于没配，读档时不建**（免得存档多出一段空块） |
| `enabled` | `true` | 关了就不参与"谁在播"的竞选 |
| `autoStart` | `false` | 进 Play / 换到这个场景就播 |
| `cps` | `30` | 每秒出多少字。`<= 0` = 立刻全出 |
| `startNode` | `""` | 空 = 用资产自己的 `start` |
| `priority` | `0` | 多个组件同时在播时选最大的 |

> `player` / `started` / `loadFailedLogged` 这些是**运行时字段，不落盘** ——
> 落盘会让"下次读档"接着上次的播放头，那是存档（C3）的活。

---

## 4 · 谁在播：cue > 组件

两个来源，优先级从高到低：

1. **cue（时间轴 `kind=Line` / 脚本点名）** —— 过场里"该说话了"是导演命令，
   普通 NPC 的自动台词必须让位。
2. **组件 `autoStart`** —— 多个组件按 `priority` 选一个。

`cue()` **绝不给场景加组件**：它的状态存在系统自己的槽位里。给场景动态加
组件看着方便，但那份组件会被 `scene.save` 写进磁盘 —— 于是"播过一次过场"
就永久改变了场景文件，而作者从没配过那一段。

`cue()` 只从宿主组件借 **`cps`** 一个字段；起点**永远**用资产的 `start`
（过场点名的是"这段台词"，从哪句开始是资产自己的事）。

---

## 5 · 键盘

| 键 | 作用 |
|---|---|
| **空格** | 正文没出完 → 立刻全出；出完了 → 翻下一句；在选项上 → 确认当前项 |
| **↑ / ↓** | 选项上移 / 下移（**刻意不用 W/S**：那是移动键，按一下会连人一起后退） |

> ⚠ 空格在输入表里是 `Jump`。`InputMap::wasPressed` 是**边沿查询、不是消费**
> —— 同一帧对话推进完，`PlayerController::update` 照样能读到那一次按下，
> 角色会原地跳一下。所以运行时会调 `setJumpBlocked(true)`：**只挡跳跃，
> 不挡移动**（过场里允许走位）。

---

## 6 · 字幕框几何

参考 1920×1080 设计，按 `客户区宽 / 1920` 等比缩放：

| 常量 | 值 | 说明 |
|---|---|---|
| `kBoxW` | 1240 | 框宽（水平居中） |
| `kBoxBottom` | 96 | 框底边距屏幕底 |
| `kPadX / kPadY` | 30 / 20 | 内边距 |
| `kTextLineH` | 36 | 行高（**框向上长高**，底边不动） |
| `kChoiceH / kChoiceGap` | 36 / 6 | 选项行高 / 间距 |

三条不变量（`verify_dialogue.py` ⑤ 就照这个判）：**等比缩放**、
**水平居中**、**所有框底边一致**。

---

## 7 · 排查速查

| 症状 | 先查 |
|---|---|
| 进 Play 什么都没有 | `autoStart` 是不是 `false`？有没有时间轴 `kind=Line`？ |
| 有 `DLG-START` 但画面没框 | 是不是被暂停菜单盖住了？`SUB-BOX` 有没有打？ |
| 字幕永远空白 | `cps` 是不是 0 或负数？—— 那是"立刻全出"，反过来"永远不出"说明 `elapsed` 没在涨 |
| 中文变成一串 `?` | 打字机按**字节**截断了。必须按码点（`utf8Prefix`） |
| 按空格角色跳一下 | `setJumpBlocked` 没接上 |
| 选项一闪就自动选了 | 某个调用方在 `choices` 非空时直接跳了 `choices[0]` |
| 改剧本后跳错地方 | 用下标而不是 `id` 存的目标 |
| `DLG-LOAD-FAIL` | 资产里有 `goto` / `next` 指向不存在的 id（加载时校验会点名） |
| 播过一次，场景文件多了一段 `dialogue` | 有代码给场景动态加了组件（`cue()` 不允许） |

### 日志锚点

```
DLG-ASSET  name= lines= start= warns=
DLG-START  asset= path= node= cps= host= src=cue|component
DLG-LINE   id= speaker= chars= shown= choices= asset= src=
DLG-PRESS  reveal id= chars=shown/total src=        ← 打字中按了一下，补完了
DLG-PRESS  need-choice id= n= src=                  ← 见下方说明，运行时不可达
DLG-CHOICE i= id= asset=
DLG-END    asset= steps= last= src=
SUB-BOX    x= y= w= h= lines= choices= speaker=     ← 只在布局变化时打
SUB-CHOICE i= focus= rect= text=[..]
SUB-TYPE   id= shown= total=                        ← 每 30 帧；两个数都是**码点**
```

> **`DLG-END` 会打两条，不是重复**：一条来自 `DialogueRunner::advance()`
> （只有 `steps/last`，在"判定说完"那一刻打），另一条来自
> `DialogueSystem::noteLine()`（下一帧补上 `asset/src`）。要判"这段是时间轴
> 起的还是组件起的"，只能看带 `src=` 的那条。
>
> **`need-choice` 在运行时不可达**：输入分发是
> `atChoice ? choose() : pressAdvance()`，而 `pressAdvance()` 只在 `!atChoice`
> 时被调 —— 那时要么不在选项节点，要么正文没出完，两条路都只可能返回
> `Revealed` / `Moved`。这个返回值留给外部直接调 API 的调用方，数值侧
> `player_q1_needchoice` 覆盖它。

---

## 8 · 验收怎么跑

```bash
# 数值层：97 条，几毫秒（解析 / 校验 / 打字机 / 播放头 / 选项 / 调度优先级）
./build/bin/Game.exe --selftest dialogue

# 端到端：11 组（真加载场景 + 真跑帧 + 真往窗口里按键）
python tools/verify_dialogue.py
python tools/verify_dialogue.py --skip-keys   # 只跑自动推进那一组
python tools/verify_dialogue.py --skip-auto   # 只跑按键那一组
```

端到端分两个用例：

- **A 自动推进**（`MYVK_DLG_AUTO=1.5`）：时间轴真的 cue、五句顺序与码点字数、
  打字机在涨、字幕框几何、选项等正文出完才出现、说完 `DLG-END src=cue`。
- **B 按键推进**（`MYVK_RUNTIME_TIMESCALE=0.5` 拉宽打字窗口 + PostMessage 真
  按键）：打字中按 = 补完、补完不会顺手选掉选项、↓ 之后选中的是 1 号、换分支。

---

## 9 · 现在**还**不能做的（别照着试）

| 想做的事 | 现状 |
|---|---|
| 文本里插变量 `{player}` | ✗ 需要黑板 / 存档，`DialogueAsset` 明确不做 |
| 条件分支（按 flag 走线） | ✗ 同上，要等任务标记 |
| 立绘 / 表情 / 语音 | ✗ 资产里没这些字段（加了没人消费 = 永远为空的必填项） |
| 对话历史回看 | ✗ 只保留"当前这一句" |
| 多人同时说话 / 旁白分栏 | ✗ 只有一个字幕框 |
| 跳过整段（Fast-forward） | ✗ 只能一句一句按 |
| 在编辑器视口里预览对话 | ✗ 只有运行时画字幕框 |

---

## 10 · 剧本编辑器（面板上写台词，不用手写 JSON）

> #310 · `editor/DialogueEditorWindow.{h,cpp}`（约 700 行）
> 验收：`tools/verify_dialogue_script.py`（7 组 / 28 条断言）
> + `--selftest dialogue` 的 G 组（`rt_*` 八条往返断言）

### 10.1 三个入口

| 从哪进 | 带什么 |
|---|---|
| Inspector → Tool → **Dialogue** 页签 → **Edit script…** | 组件上配的那份路径 |
| **View / Window 菜单**（或底栏 `+` 托盘）→ **Script editor** | 空手开窗，只剩"新建" |
| `MYVK_EDITOR_DLGED=<路径>`（自动化） | 指定路径 |

三个入口汇到同一个 `open(path)`，走的是 `EditorContext` 里的一次性请求 ——
所以自动化那句和手点按钮是**同一条路**，不是给测试开的旁路。

### 10.2 界面

```
┌ 剧本编辑器 — ch01_open_01 *──────────────────────────────────────────┐
│ 资产路径 [dialogues/ch01_open_01.dlg.json ] [Reload][Save][New script]│
│ 有未保存的改动。                                                       │
├──────────────┬───────────────────────────────────────────────────────┤
│ 共 7 行      │ id [l1]  说话人 [千夏]  [设为起点] （起点）             │
│ [+台词][复制]│ ┌───────────────────────────────────────────────────┐ │
│ [删除][上移] │ │ ……又是这个梦。                                     │ │
│ [下移]       │ └───────────────────────────────────────────────────┘ │
│ > l1 千夏 …  │ 5 字 | 18 字节                                        │
│   l2 千夏 …  │ ── 走向 ──  下一句 [l2 ▾]                             │
│   q1 千夏 …  │ ── 选项 ──  [去天台看看] [roof ▾] [x]                 │
│   roof  …    │             [+ 选项]                                   │
├──────────────┴───────────────────────────────────────────────────────┤
│ 加载校验：通过  共 7 行，起点 l1                                       │
└──────────────────────────────────────────────────────────────────────┘
```

左栏行首的标记：`>` = 起点，`.` = 从起点走不到（建议，不是错误），空 = 普通。

### 10.3 五条"省了就错"的约束

| # | 约束 | 省了会怎样 |
|---|---|---|
| ① | 校验**不在面板里实现**：每次改动都是"序列化 → 丢回 `parseDialogue`" | 两套规则。症状是"编辑器说没问题，进 Play 却 `DLG-LOAD-FAIL`"，而且两边都"有理" |
| ② | 改 `id` 时把全表 `next` / `goto` / `start` 一起打补丁 | 校验器只会说 `next "old" is not a line id`，**不会说在哪几行** —— 作者得一行行找回去 |
| ③ | 读盘失败时**禁止存盘**（只读态） | `parseDialogue` 是边解析边填，半路出错时 `nodes` 是前几行的残片。此时按 Save 会把 300 行的剧本截成 119 行 |
| ④ | 存盘前必须校验通过，除非显式按 **Save anyway** | 把问题推给运行时，而作者以为"我存过了" |
| ⑤ | **不做预览播放**按钮 | `DialogueSystem` 只在运行时 tick。按下去什么都不动的按钮比没有更糟 |

### 10.4 写回（`dialogue::toJson` / `saveDialogue`）

`DialogueAsset` 从 #310 起能写。四样东西刻意**不写**：

| 不写 | 为什么 |
|---|---|
| `index` | 它是 `parseDialogue` 重建的**缓存**。落盘等于把身份写死成下标（与 §2 的 ① 同源） |
| `error` / `warnings` | 加载期诊断，不是内容。写进去下次读盘会把上次的警告当成"这次也这样" |
| 空的 `next` / `choices` | 整个字段省略。写 `"next": ""` 会让校验器以为作者显式指定了"跳到空 id" |
| `\uXXXX` 转义 | `ensure_ascii=false`。转义之后 diff 是一屏 `\u5343\u590f`，作者没法用 git 看自己改了什么 |

`saveDialogue` 是**写临时文件再改名**，不是直接覆盖：写一半断电的后果是"文件打不开"，
而存档点就在一个快捷键之外。

### 10.5 存盘之后：场景里的组件会被刷新

作者按 Save 后，场景里**引用同一份剧本**的 `DialogueComponent` 会重新读一遍。

不做这件事的话症状很怪：窗口里是新台词、按 Save 成功、进 Play 还是旧台词 ——
因为 `DialoguePlayer::load` **按路径缓存，同路径重复调用不重读**。
所以刷新时是把 `player` 整个复位再 `load()`。

**正在播的那一份不打断**（`player.running()` 为真 → 记 `DLGED-SCENE-SKIP`，
等它下一次起播），换台词比"过场中途换词"更不该发生。

### 10.6 自动化

```bash
python tools/verify_dialogue_script.py            # 7 组 / 28 条
python tools/verify_dialogue_script.py --only c   # 只跑某个用例
python tools/verify_dialogue_script.py --keep     # 留临时目录
```

两个注入钩子（与 `MYVK_EDITOR_REBIND` 同一条纪律：**只跑一次**）：

```
MYVK_EDITOR_DLGED=<路径>                 启动即打开这份剧本
MYVK_EDITOR_DLGED_SCRIPT="sel:2;del;save"  按顺序执行，分号分隔、冒号分隔参数
MYVK_EDITOR_DLGED_DUMP=<路径>            把当前资产落成 JSON（不写盘也能看）
```

命令表：`sel:<i>` `new:<id>` `dup` `del` `up` `down` `setid:` `setspeaker:`
`settext:` `setnext:<id|->` `start` `addchoice:<goto>` `delchoice:<i>` `newscript`
`save` `saveforce` `saveas:<路径>` `reload` `check` `dump`。

> ⚠ 注入的文本**用 ASCII**：`std::getenv` 在 Windows 上按 ANSI 代码页转换
> 环境块，中文进去会变成 GBK 字节 —— 那验的是"编码没配好"，不是编辑器对不对。
> 中文是否原样，用更硬的判据：把原文件里的中文往返一遍逐字节比（B5）。

### 10.7 日志锚点

```
DLGED-OPEN            path= ok= lines= start= choices= readOnly=
DLGED-CHECK           ok= lines= start= warns= err=            ← 只在结论变化时打
DLGED-SEL             i= id=
DLGED-NEW             id= from= total=                        ← from=- 表示没自动接线
DLGED-DUP / DLGED-DEL id= refs= total=
DLGED-MOVE            id= from= to=
DLGED-RENAME          old= new= patched=                      ← patched 是补的引用条数
DLGED-NEXT / DLGED-START / DLGED-ADDCHOICE / DLGED-DELCHOICE / DLGED-CHOICEGOTO
DLGED-UNLINK          id= old_next= reason=choices            ← 加选项时清掉的 next
DLGED-REJECT          op= reason=                             ← 每个被拒的操作都有
DLGED-SAVE            path= lines= force=
DLGED-SAVE-BLOCKED    path= reason=read-only|no-path|check
DLGED-SAVE-FAIL       path= err=
DLGED-SCENE-RELOAD    path= reloaded= skipped=
DLGED-CLOSE-DIRTY     path=                                   ← 带着未存改动关窗
DLG-SAVE              path= full= lines= start= bytes=        ← 由 DialogueAsset 打
```

### 10.8 现在**还**不能做的

| 想做的事 | 现状 |
|---|---|
| 面板里直接看字幕框长什么样 | ✗ 要预览得进 Play（`DialogueSystem` 只在运行时 tick） |
| 变量替换 / 按旗标分支 | ✗ 资产格式里没有，与 §9 同步（旗标已在脚本里能用，但对话资产不读） |
| 立绘 / 表情 / 语音绑定 | ✗ 同上，`DlgNode` 没这些字段 |
| 撤销（Ctrl+Z） | ✗ 面板的改动不进 `EditorContext` 的 undo 栈 —— 资产不在场景里。**存盘**就是检查点，Reload 就是整体回滚 |
| 剧情流程图（节点连线图） | ✗ 只有列表 + `next` 下拉 |
| 多语言剧本（同一份文案多语言） | ✗ 与 §9 的"对话文本不在 i18n 体系里"同一条 |
