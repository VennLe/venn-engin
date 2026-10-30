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
