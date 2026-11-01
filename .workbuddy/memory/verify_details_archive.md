# 测试 / 断言 / 回归设施 —— 细则归档

> 从 `MEMORY.md`「断言纪律」「工具」两段迁出（体积纪律：本页只留判据）。
> 判据速查仍在 `MEMORY.md`；这里是**为什么**和**具体怎么绕**。

## 一、断言落点

- ⚠⚠ 结尾必须是 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号）。`RESULT:` 是 `verify_runtime_m2` 的历史遗留，
  只有它的解析器认，新用例一律用 `结果:`。
- ⚠⚠ 落点必须是"两种实现会不一样的那个量"。反例：断言"有矩形"（两种实现都有）、断言"日志有输出"
  （崩了之前也会打）。
- 新断言必做**否定测试**（把实现改回错的，必须变红）+ **对照区**（同一张图里放一个"应该不变"的参照物）。
- ⚠ **FAIL 先怀疑期望再怀疑实现**。10-30 运镜三轮红都是期望算错：A2f yaw=90° 时"右"是 +X 不是 −X；
  D1 `cameraEulerFor` 基写错（`(f,r,u)` 行列式 −1）。
- ⚠ 否定测试**没红也是结论** —— 说明这条路径不可达或已有别的机制兜住，写进文档别删。

## 二、像素判据

- 不用整图平均差（用**显著变化像素占比**）：平均差会被"大面积没变"稀释到看不出。
- **先确认被测物完整落在采样区**再谈它动没动。10-26 踩过：以为"角色没反应"，实际它整个在采样框外。
- 测形状/轮廓时关 CSM + TAA（两者都会让边缘抖动）。
- 不用「最亮 N%」当高光（N 一晃，结论就翻）。

## 三、进程与日志

- ⚠ `has_line` 证明不了"点了有反应"（锚点/启动行一直有）→ 用 `wait_count(+1)`。
- ⚠ `Popen(PIPE)` **必须排空**，否则管道满 → 子进程卡死 → 超时。
- 删文件走 `safe_fs.py`（受保护路径会拒，避免误删仓库）。
- ⚠⚠ `--scene` 打不开**不报错**（静默跑默认场景）→ 夹具落 `build/bin/`、用**相对名**。
  11-30 踩过：`MYVK_EDITOR_SCENE=_cine_fixture.json` → `WARN auto-open failed … assets\_cine_fixture.json`
  —— `resolveAssetPath` 是相对 `assets/` 根，正确的是 `scenes/_cine_fixture.json`。
- ⚠⚠ 日志计数**要带后缀**：`CINE-VERB` 与锚点行 `CINE-VERB-PAN=(…)` 同前缀，`count_line` 会串 →
  改成 `count_line("CINE-VERB action=")`。

## 四、合成输入

- ⚠⚠ **鼠标"起手"要预热**：两次 MOVE（间隔 0.2s）+ 0.35s。上一动作常把光标甩出客户区 →
  `MousePos=-FLT_MAX` → 分组轮流红，而且**看起来像功能坏了**。
- ⚠⚠ **点菜单必须 `mouse_event`**（系统级）。`PostMessage` 连跑 0/3 成功（菜单窗口不认合成 WM_LBUTTON*）。
- ⚠⚠ `PostMessage` 的 lParam **必须带扫描码** `(_sc(vk)<<16)|1`：GLFW 从 `lParam>>16` 取 scancode，
  传 0 → `KEY_UNKNOWN` → 回调根本不进引擎。
- ⚠⚠ **`PostMessage` 的 Shift 会被 GLFW 撤销** → 修饰键必须 `keybd_event` + 先 `SetForegroundWindow`。
- ⚠ **键位会变**（Sprint 10-22 Shift→右键）→ 用例**从日志读绑定**，别写死。
- ⚠⚠ **运行时玩家要先左键点视口才进"操作态"**：不点则键**照样**进引擎、人一毫米不动、**日志零错**
  （状态机停在 `state=idle speed=0.00`）—— 和"键没送到"长得一模一样。

## 五、UI 锚点（logRect）

- ⚠⚠ `logRect` **不判可见性**（只按坐标去重）。滚出可视区的控件"有矩形却点不到"。
  → `click_tag` 前**先滚**：本机 Inspector 的固定落点是**死区**，滚 `INS-PANEL` 中轴、离底 60px，90px/格。
  通用解法是 `scroll_into_view(hwnd, tap, tag, win_tag)`（10-30 为 `CINE-BAKE` 新增）。
- ⚠⚠ 锚点 logRect 必须**无条件**。反例：写在 `if (!clicked) return;` 之后 → 只有点中那一下才出现锚点，
  于是"锚点检查"永远红。改成 `const bool clicked = Button(...); logRect(...); if (!clicked) return;`。
- ⚠⚠ 锚点检查的**位置**也讲究：放在"选中相机"之前，而镜头/路径/定稿段都在 `cur() != nullptr` 分支里 →
  检查时它们根本没建出来。检查必须在选中之后。
- ⚠⚠ 分隔条遮挡裁切**不能含边界**：右边界压在线上 → 整条线被裁光（`spans=0`），可锚点还在 → 假绿。
- ⚠ 面板上加一行，下面的按钮网格会**整排掉出面板**。

## 六、崩溃形态

- ⚠⚠ **Debug /RTC1 越界 = 断言框**（`#32770` 对话框）：进程**活着**、日志停在上行、看起来像卡死。
  → 找 `#32770` 窗口读原文，别猜。
- ⚠ 面板内容比停靠后的窗口高 → 后半截控件滚出可视区（见 §五）。

## 七、回归登记

- 新 `verify_*.py` 必须同时进 `DEFAULT` 与 `TRIGGERS`；⚠⚠ **改完必跑 `check_regression_map.py`**（否则静默不跑）。
- 铁律：**没登记 = 全量 + 点名**。
- **横切文件别写进 `TRIGGERS`**（以 `GLOBAL_GLOBS` 为准）：`Components.h` 这类一改全挂，写进去等于每次全量。
- ⚠ `run_regression.py` 的帧数参数是 **`--frames`**（`--max-frames` 会被静默忽略 → 跑满超时被 kill）。
- ⚠ `--explain` 在无基准快照时**一律报全量**，别拿它当"只影响这几个"的证据。
- ⚠ flaky：`bvh`/`csm`/`ibl`/`particles` 偶发全红 → 别补跑"凑干净"。

## 八、i18n 覆盖

- ⚠⚠ **缺译文不报错不崩，只会静静少翻一处**。
- ⚠ 特性名走 `FeatureDesc.name`，**不在参数 label/tooltip 的视野里**（要单独查）。
- ⚠ **格式串（`"%.2f s"`）也要包 `TR()`**。
- ⚠ 机器解析的契约行（如 `CINE-STATE`）进 `FMT_KEEP` —— 翻了就断，测试解析不到。
- 加词条后重跑 `gen_sdf_atlas.py`。

## 九、其它设施

- ⚠ `capture_client` 返 **`(PIL.Image, ok)` 元组**（不是裸 Image）。
- 截图落 `<repo>/../_venn_verify`；**禁硬编码盘符**。
- 键盘文本走 `PostMessage(WM_CHAR)`，键盘按键走扫描码（见 §四）。

## 十、2026-11-01 收口复跑查出的四类坑（细则）

### 10.1 引擎给锚点加字段 → 夹具静默失配（最隐蔽）

`PLAYER-STATE` 在 2026-10-24 插了 `arm=` / `cam=`（"贴墙镜头钻墙"的证据），位置
**在 `pitch` 和 `miv` 之间**。而 `verify_player.STATE_RE` 是

    yaw=(..) pitch=(..)(?: miv=(\d))?(?: cap=(\d))?

中间多了两段 → `cap` 那个可选组**一条都匹配不上**，`group(9)` 恒 `None`。

连锁：`capture_ready()` 8 秒拿不到 `cap=1` → `enter_play()` 返 `vp=None` →
`verify_frame_pacing` 报 `[FAIL] 进 Play + 点视口接管` / `rc=3`；`verify_player`
报"探针 cap=1 拿不到"。**看着像 P8 的鼠标接管坏了，产品一行没错。**

修法：**非捕获组** `(?: arm=[\d.]+)?(?: cam=\([^)]*\))?` —— 分组编号 1~9 一位
不动，几十处 `group(2/3/4/8/9)` 一个都不用改。正反两向都验（带 `arm/cam` 的
与老 `miv/cap` 行都能解析出 `cap=1`）。

**通用教训**：给日志中间插字段时，任何"可选组链"都会**静默断掉后面的组**。
要么插到行尾，要么整条正则改成非捕获 + 具名组。

### 10.2 P8 接管必须用真实输入

`capture_ready()` 原来 `click_client()`（`PostMessage` 合成 `WM_LBUTTONDOWN`）。
P8 的接管是 **GLFW** 在看真实按键/光标状态，合成消息它不认。症状：**连跑 5 次
全过、紧接着连跑 3 次全挂**（窗口是否在前台）。

修法三步：`force_foreground()`（`verify_asset_drag.py` 里那套 attach-thread-input
+ ALT 兜底）→ 真实 `mouse_event` → 光标预热两次 MOVE + 静置。改完连跑 3 次全过。

### 10.3 报告契约守卫（`check_regression_map` [G] 段）

`run_regression` 的 `mark = PASS if (rc==0 and ok) else FAIL`，`ok` 只由 `OK_RE`
扫到收尾行为真 → **没打 `结果:` 行 = 全绿判红**。

2026-11-01 实测 `DEFAULT` 里 2 个脚本缺：`verify_editor_play_terrain`（打 `N/M PASS`）、
`verify_frame_pacing`（打 `==== N passed, M failed ====`，**而且把 `rc` 丢了** ——
`main()` 提前 `return 1..7` 时 `g_fail` 还是 0 → 一行没测被判成功）。

守卫用**字面量包含**判（`结果` / `RESULT` / `全部断言 PASS` / `全部 PASS` / `kit.finish(`），
**不用正则**：本机 `bash heredoc` 会把 Python 正则里的反斜杠吃掉（`\s` → `/s`），
第一版扫描因此报了 39~45 个假的"缺收尾"。修前点名 2 个、修后 0 个 = 否定测试。

### 10.4 判据重标定（含实测数字，下次别再重标一遍）

`verify_frame_pacing` 修好夹具后露出 4 条红，逐条量过：**没有一条是产品回归**。

| 断言 | 老口径 | 实测 | 定性 |
|---|---|---|---|
| A3 | `sd/mean < 8%` | 44% | 8% 来自"空场景 MAILBOX 470 fps → FIFO"基线 |
| A4 | 尖刺 0 个 | 5/64 | **每 15 帧一次 55~73 ms** |
| B2 | `ΔanimT/dt ≈ 1` | 1.54~1.76 | 前提被两个设计推翻 |
| C1 | 不锁 >200 fps | 33~59 | 本机 surface 只有 `[IMMEDIATE, FIFO]` |

**决定性实验**：进 Play 后**先静止 2 s** 再走。静止组尖刺 `idx 5,20,35,50,65`，
移动组 `idx 37,52,67,82` —— **间隔都是 15 帧、毫秒数一样**，锁定/不锁定也一样。
→ 停顿既不来自玩法，也不来自呈现模式（机器空闲时同一二进制只掉 1/125 帧，
它随**系统负载**变化）。

**B2 的两个设计**：① `Locomotion` 步幅匹配 `playbackRate`（走 3.0 m/s → 1.5~1.6）；
② `AnimatorSystem` A4b 相位速率曲线（`time = pc->advance(phase, dt*speed/dur)*dur`）。
要求 ≈1 等于要求"把滑步改回来"。

**新口径**：A3 = 掐掉起手 0.5 s 后 **p90/中位 ≤ 1.8**（用 p90 不用 p95：周期停顿
密度正好 6.7%，p95 恰好压在停顿上恒 2.3~2.5，p90 落在正常帧上 1.10~1.23；而
20% 塌陷会把 p90 顶到 3.51）；A4 = 尖刺 < 10% 并打出**中位间隔**；**A5（新增）**
= 静止 vs 移动的尖刺数差 ≤ 2；B2a = 负跳只允许是回绕（幅度 ≥ 0.3 s）；B2b = 中位
倍率 ∈ [0.2, 4.0]。

阈值形式自检 → `tools/check_pacing_judge.py`（喂合成数据，不启动编辑器）：
A3 1.11 绿 / 3.51 红；A4 6.2% 绿 / 20.3% 红；A5 持平绿 / 4-vs-12 红。

### 10.5 `MYVK_VSYNC=0` 在本机是死开关（已改为诚实报告）

`Swapchain.cpp` 原来只试 `MAILBOX`，没有就退 `FIFO` → 本机没有 MAILBOX，于是
`vsync=0` 打出 `present mode = FIFO (vsync=0)`：**开关看着生效，帧率还是锁的**。
补了 `IMMEDIATE` 兜底；日志改成**真实模式名 + 可用列表**（原来的二选一写法一旦
选中 IMMEDIATE 就会谎报成 MAILBOX，而这一行是判据唯一数据源）。

⚠ 但实测 **IMMEDIATE 在本机也不更快**（34 vs 34 fps，DWM 会压回来）。所以 C 组
三档：真跑满 → 通过；拿到 MAILBOX 却没跑满 → **红（真回归）**；只有
`IMMEDIATE/FIFO` 且跑不满 → **SKIP** 并念出可用列表。**测不了就说测不了。**

## 十一、2026-11-01 B6 表情落地查出的四类坑（细则）

### 11.1 `A || ImGui::TreeNode(...)` = ID 栈杀手

第一版把"眨眼通道名"藏进一个条件折叠节点：

```cpp
if (xc->blinkChannel != "blink" || ImGui::TreeNode("##blinkadv")) {
    ImGui::InputText(...);
    if (xc->blinkChannel != "blink") ImGui::TreePop();
}
```

`blinkChannel == "blink"`（默认值）时**短路**：`TreeNode` 没被调用，而 `TreePop`
在块尾照样执行 → `Calling PopID() too many times!` → **同一帧之内之后所有控件的
ID 全部错位**。

症状极具误导性：**整段 Inspector 锚点集体消失**（`INS-MESH-*` 之类全都找不到），
日志里**只有一条** `[imgui-error]`，看起来像"UI 没画出来 / 布局漂移 / 窗口太小"。
第一版排查方向是"面板高度不够"，浪费了一轮。

修法：不做折叠，平铺成
`InputText(TR("Blink channel"), ...) + SameLine() + TextDisabled(TR("slider name in the rig"))`。

**判据**：日志里出现 `PopID() too many times` / `PushID` 相关 imgui-error，一律先查
同一个 `if` 里有没有 `TreeNode/TreePop` 配对，再怀疑布局。

### 11.2 UI 用例"锚点全红"的两个前提（漏一个就一条都不画）

`verify_expression` B 组第一版 8 个 `INS-EXPR-*` 全红，**两条原因都不是产品 bug**：

1. **Inspector 只画选中的实体** —— 不选中时所有 `INS-*` 锚点一条都不出现，而日志里
   **没有任何"你没选中"的提示**。夹具必须 `env["MYVK_EDITOR_SELECT"] = ENTITY`。
2. **`drawTabAll` 不是融合视图** —— 它只调 transform / visibility / mesh / lights /
   script / sequence；`drawTabAnimation` 里的段落在 All 页签下**根本不画**。
   夹具必须 `env["MYVK_EDITOR_INSP_TAB"] = "animation"`。

⚠ 这条对**所有新加 Inspector 段落**通用：写用例时先问"它在哪个页签、要不要选中"。

### 11.3 新资产目录必须进 CMake 的"每次都拷"档

`EXPR-LOAD-FAIL ... cannot read .../build/bin/assets/expressions/alicia_demo.expr.json`

新目录没进 `VENN_ALWAYS_ASSET_DIRS` → 运行目录里根本没有这个文件。
**报错给的是 `build/bin` 下的绝对路径**，所以第一反应是"资产格式写错了 / 名字拼错了"，
实际是"文件没被拷过去"。

**判据**：构建日志必须有 `Copying readonly assets to binary dir`，并
`ls build/bin/assets/<新目录>` 有东西。与"新加资产不会自动进 `build/bin/`"是同一条铁律。

### 11.4 驱动器的两个"只做一次"

- **归零只做一帧**：`ownedChannels` 必须只存**本帧活跃**的通道，**不含**为了清旧表情
  而补 0 的那些。第一版把补 0 的也塞进 owned → 清空预设后每帧都往那根通道写 0 →
  用户再推滑条立刻被抹掉（断言 `E5b` 报 `owned=1`、`E5c` 报"手动推的 0.4 没了"）。
- **首帧只抽一次相位**：`blinkTimer < 0` 表示未初始化，首帧按实体 id 抽
  `interval*(0.25+0.75*jitter)`，避免全场景角色同时眨眼；之后每次眨完用
  `max(0.05, interval*(0.85+0.30*jitter))`，抖动来自**确定性 LCG**（种子 `e.id`），
  这样自检才能钉"第 N 次间隔落在 [0.85,1.15]×interval"。

### 11.5 口型的输入源为什么必须是 Voice 总线

- **全局 RMS**：一句轻声台词配响 BGM → 嘴一直张着。
- **`voicePlaying()`**：带一个 block 的宽限期 → 嘴先动、声后到（穿帮）。
- **`Mixer::busRms[Voice]`** ✅：只含语音总线；`frames==0` 时**不更新**（否则除 0 →
  inf → NaN 的脸）；累加器用 **double**（float 会丢尾音）。
- 平滑必须**指数**（`1 - exp(-dt/tau)`，攻 0.02 / 放 0.09）：线性逼近会让 60 fps 与
  144 fps 的**韵律**不同。
