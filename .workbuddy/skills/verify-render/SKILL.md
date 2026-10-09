---
name: verify-render
description: 验证 venn 引擎的渲染 / 编辑器改动是否真的正确——在不依赖人眼看图的前提下，用"冒烟测试 + 截图 + 像素对比 + 交互自动化"四层证据链确认。当用户改了渲染、着色器、几何、材质、场景序列化，或者改了编辑器面板 / 手柄 / 拖拽交互，需要确认"没画错、没崩、验证层干净、交互真的通"时使用。触发词：验证渲染、渲染对不对、截图看看、有没有画错、跑一下冒烟、渲染回归、验证编辑器、拖拽对不对、verify render、regression。
agent_created: true
---

# 渲染 / 编辑器改动验证（venn）

## 为什么需要这套流程

两个约束决定了不能"改完就说好了"：

1. **"看起来对"不算数** —— 必须把"画面是否正确"转成**可读的数字**（直方图 / 像素差）。
   Read 工具能直接看图（PNG 可以读，用来做**布景/取景**这类审美判断很合适），
   但它在**数值判等**上不可靠：人眼分辨不出 1 LSB 的差异，也分不清"少了 3% 的光"
   和"少了 30% 的光"。凡是能用数字回答的问题，就别用眼睛回答。
   （实践分工：Read 看图 → 发现"柱子挡住了画面""物体叠在一起"这类问题；
   compare_png → 证明"剔除开关不改变任何像素"。）
2. **"编译通过 + 日志无错" ≠ 功能可用** —— 界面 / 交互类改动（手柄、拖拽、
   右键菜单）编译过了也照样可能是空转。这类必须**真的点一下**。

**铁律：所有命令结果都写文件，再用 Read/Grep 工具去看。**
bash 的 coreutils 在部分场景不可靠，PowerShell 的 stdout 在本环境**完全不回显** ——
所以重定向到文件再 Read 永远是最稳的。写文件优先用
`[System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))`；
**不要**用 PowerShell 的 `*>` 重定向（它是 UTF-16，Read 会当成二进制拒绝）。

## 四层证据链

### 第 1 层：构建 + 冒烟（必须干净）

```bash
cd /e/code/cpp/venn-engin
cmake --build build -j 8 2>&1 | grep -vE "third_party|tiny_obj" | tail -20
```

`third_party` 里有几条自带的 `defined but not used` 告警，过滤掉之后应当**零告警**。
（`tiny_obj_loader.h` 那 3 条是第三方自带的，与本项目改动无关。）

冒烟要跑**两趟**：编辑态一趟、Play 态一趟（Play 链路会走"复制运行态 → 编译脚本 →
每帧执行脚本 → 渲染到视口"这一整条，只跑编辑态覆盖不到）：

```bash
cd /e/code/cpp/venn-engin/build/bin
MYVK_FRAMES=180 ./Editor.exe                    > smoke_a.txt 2>&1; echo "exitA=$?"
MYVK_FRAMES=180 MYVK_EDITOR_PLAY=1 ./Editor.exe > smoke_b.txt 2>&1; echo "exitB=$?"
grep -iE "error|warn|Validation|VUID" smoke_a.txt smoke_b.txt
```

合格标准：两个 `exit=0`、grep 无输出（`Runtime scripts: 0 executed, 0 failed` 那行
里带 "failed" 字样不算）、以 `Application shutdown. Total frames: 180` 收尾。

### 第 2 层：截图（可带环境变量）

```bash
PY="C:/Users/v_lweiili/.workbuddy/binaries/python/envs/default/Scripts/python.exe"
cd /e/code/cpp/venn-engin
"$PY" tools/capture_window.py --size 1600x900 --wait 6 --out shot.png
```

`capture_window.py` 的默认目标现在是 **`build/bin/Editor.exe` / 标题 "Venn Editor"**
（Sandbox 目标已随内置 demo 一起删除）。它用 `PrintWindow(PW_RENDERFULLCONTENT)`
抓窗口，**不受窗口遮挡影响**；参数：`--no-resize` 跳过缩放压测、`--size WxH`
摆成固定尺寸、`--env K=V` 可重复注入环境变量、`--args` 追加命令行参数。
窗口查找是 `EnumWindows` + 标题**前缀**匹配。

**编辑器侧的自动化钩子**（都只在设了环境变量时生效，正常使用完全不受影响）：

| 变量 | 作用 |
|---|---|
| `MYVK_EDITOR_SCENE=<路径>` | 启动即打开指定场景，而不是空场景 |
| `MYVK_EDITOR_SELECT=<名字>` | 启动后按名字选中一个实体（让手柄 / 检查器有内容） |
| `MYVK_EDITOR_GIZMO=move\|rotate\|scale` | 启动就把手柄切到某个模式 |
| `MYVK_CONTENT_DIR=<相对路径>` | Content 浏览器的初始落点（默认 `assets`） |
| `MYVK_GRID=0` | 关掉地平面栅格（像素对比时用） |
| `MYVK_LOG_RECTS=1` | 把控件真实矩形打进日志，给自动化脚本点控件用 |

有它们才能把"层级图标 / 手柄三种粒度 / 拖拽落点"这类界面做成可脚本化的验证，
否则只能靠人手点。示例：验证手柄三种粒度

```bash
for m in move rotate scale; do
  "$PY" tools/capture_window.py --exe build/bin/Editor.exe --title "Venn Editor" \
    --size 1600x900 --wait 6 --out "gizmo_$m.png" \
    --env "MYVK_EDITOR_SCENE=/abs/path/scene.json" \
    --env "MYVK_EDITOR_SELECT=Crate" --env "MYVK_EDITOR_GIZMO=$m"
done
```

**做像素对比必须同时加 `MYVK_HIDE_UI=1` 和 `MYVK_FREEZE_ANIM=1`** ——
少任何一个，diff 都永远不是 0，于是"0 差异"这个最强的证据就用不上了。

### 第 3 层：像素级对比（决定性证据）

改渲染前后各截一张，然后 `"$PY" tools/compare_png.py before.png after.png`。
输出 mean/max diff、>16 差异像素占比、**差异包围盒**、8×6 网格分布。

**必须跑对照组**：同一份代码 + 同一套环境变量连跑两次做 diff。合格基线是
**0 差异**（`MYVK_FREEZE_ANIM=1` + `MYVK_HIDE_UI=1` 之后没有任何非确定性来源）。

### 第 4 层：交互自动化（界面 / 拖拽类改动必须做）

编译通过不代表交互能用。`tools/verify_asset_drag.py` 是"真的用鼠标拖一次"的端到端
验证，也是**新写这类脚本的模板**：

1. 带 `MYVK_LOG_RECTS=1` 启动编辑器 —— `DebugRects.h` 会把控件矩形按
   `TAG=(x0,y0)-(x1,y1) center=(cx,cy)` 打一行日志。**坐标就是客户区物理像素，
   和 PrintWindow 截出来的图 1:1**，所以脚本可以直接用它们去点控件，
   不用在截图里"猜"按钮在哪（猜坐标踩过坑：DPI 缩放 + 窗口移动会让写死的坐标
   整体偏掉，而且偏了不报错，只是点不中）。
   现有 tag：`CB-CELL <文件名>`、`CB-RECT up|reload|cell0`、`VP-RECT`、`HIER-RECT row0`。
2. 先 `SetWindowPos` 把窗口摆成固定尺寸，**再**读矩形 —— 缩放会让 ImGui 重排，
   旧坐标就失效了。`logRect` 只在矩形变化时重打，所以"读最后一条"天然拿到新值。
3. `SetCursorPos` + `mouse_event` 做真实拖拽：按下 → 移动过 ImGui 的拖拽阈值
   （默认 6px）→ 移进目标 → 松开。
4. 截图存档 + 从日志抓数值断言（例：`alignImportToGround: ... bottom=0.0000`）。

```bash
"$PY" tools/verify_asset_drag.py --asset box01.glb \
    --out drag.png --debug-shot drag_mid.png --env MYVK_CONTENT_DIR=assets/models
```

⚠ **最大的坑：窗口必须在真正的前台**。`SetForegroundWindow` 有条硬规则 ——
只有当前前台进程（或被它启动的进程）才允许改前台窗口，从后台脚本里直接调多半
**静默失败**；而窗口一旦不是活动窗口，ImGui 的拖拽就完全收不到输入（鼠标消息
还是会来，但就是"点了没反应"）。绕法：先按下再松开一次 ALT，系统就认为
"用户在操作"，随后的 `SetForegroundWindow` 才会被放行。见
`verify_asset_drag.py::force_foreground`。**没做这一步时脚本会静默地什么都不发生**，
症状是"坐标明明对、但拖拽中间态截图里连个跟随提示都没有"。

### 5. 对照实验的两种范式（分簇剔光就是这么验证的）

| 范式 | 期望 | 用途 |
|---|---|---|
| **优化开关**：剔除 / 深度预通道 ON vs OFF | **必须 0 差异** | 证明优化没有改变画面（保守剔除不漏光） |
| **功能开关**：MSAA / 灯数 ON vs OFF | **必须明显不同** | 证明功能真的在生效（否则可能是死代码） |

两种都要做 —— 只做前者，"功能其实是空转"会漏过；只做后者，"优化把画面改坏了"会漏过。

**对照实验的最大陷阱：参考图自己可能是坏的。** 例：
"关剔除"分支按每簇 `cap` 个槽位无脑塞灯，若 `cap < 灯数`，参考图自己就被截断了
（实测 200 盏灯 + cap=128 → 与"正确"的剔除图有 32.8% 的假差异）。
**先确认参考图的有效性，再解读差异。**

### 6. 遥测 > 想象（量化"到底省了多少"）

只证明"画面一致"还不够，还要证明"优化真的省了东西"。渲染代码里留一条纯日志通道
（面板会被 `MYVK_HIDE_UI` 关掉，所以要能走日志），例如 `MYVK_CLUSTER_STATS=1` 定期打印：
```
[cluster-stats] pass=culling lights=200 clusters=14720 maxPerCluster=79 avgPerCluster=28.47 overflow=0
```
`avgPerCluster=28.47` 直接说明"200 盏灯时每像素只循环 ~28 盏"（≈7× 削减）。
`overflow` 是**正确性告警**：非 0 就说明有簇的灯表被截断、可能漏光，要调大槽位数。

## 场景序列化专项

`MYVK_SCENE_TEST=1` 会在 `onInit` 末尾做一次 scene JSON 的 **save → clear → load** 往返自检，
断言实体数一致、所有 renderable 的 mesh/material 非空、名字能找回，并打印：
```
[scene-test] PASS  entities 11 -> 11, renderables=10, missingMesh=0, missingMat=0
```
改动了 `SceneSerializer` / 资产溯源（`MeshSource`/`MaterialSource`/`TextureSource`）后必跑。

产物 `build/bin/scene_roundtrip.json` 值得 Read 一眼：检查路径是否是**相对资产目录**
（`models/Cube/Cube.gltf` 而不是 `E:\...\build\bin\assets\...`），否则换机器就失效。

## 常见误判

- **不要**因为 diff 不等于 0 就判定失败 —— 先跑对照组；对照组本身非 0 就说明
  "冻结动画 / 隐藏 UI" 没生效，或者有别的非确定性来源（先解决它）。
- **不要**关掉验证层来"让日志干净" —— 那等于把唯一能发现 Vulkan 误用的手段扔掉。
  经验：本项目里所有真实的 Vulkan 误用（漏绑描述符集、深度附件未清…）
  都**只在验证层可见**，运行时表现正常或诡异但无报错。
- 对照实验前先确认**参考图本身有效**（见上"最大陷阱"）。
- 审美/布景问题（柱子挡视线、物体互相叠住）用 Read 看图判断，别指望直方图。
- 环境读不了图 ≠ 用户读不了图。把关键截图路径在回复里给出，让用户自己看。
- **`SetCursorPos` 返回 1 不代表坐标对**：非 DPI 感知的进程拿到的是"虚拟化"坐标，
  本机 150% 缩放下 2560≠3840。脚本必须先 `SetProcessDpiAwareness(2)`
  （`capture_window.py` 导入时就做了，所以 `from capture_window import ...` 是必须的）。
- **残留的 `Editor.exe` 进程会占住 `build/bin/Editor.exe`**，导致链接阶段只报
  一行 `collect2.exe: error: ld returned 1 exit status` 而**不打印具体符号**。
  先 `taskkill //F //IM Editor.exe` 再重编。
