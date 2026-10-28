# 编辑器 / 验证 细则归档（MEMORY.md 的展开版，2026-10-18 从 MEMORY.md 分出）

## 一、Content 抽屉六个坑（10-17 改造）
1. `SplitLayout::setBottomInset(h)` 是"底栏 + 停靠抽屉"的**总让位高度，≠ 底栏高度**。拿它当底栏
   高度去画 → 底栏被撑成 352px。`apply()` 不写 `out.bottom`，由 `computeLayout()` 显式写。
2. `IsWindowHovered()` 默认**不把"有 active item"算悬停** → 点按钮的**松开帧**返回 false → 被判定成
   "点外部"、同帧把刚开的抽屉关掉（表现：按钮闪一下，抽屉永远不出来）。必须加
   `ImGuiHoveredFlags_AllowWhenBlockedByActiveItem`。
3. **工作区预设会强制覆盖面板开关**（`Workspace.cpp` 的 `kLevelOpen` / `kCharOpen`）→ 只改
   `defaultOpen` 不够，**预设里也要删**。
4. ImGui 绘制顺序 = `g.Windows` 顺序，且**点中某窗口会把它挪到末尾** → "后画的在上层"只在同帧成立
   （点视口 / 重开抽屉时抽屉会被盖）→ 每帧 `BringWindowToDisplayFront(抽屉)`（imgui_internal，
   **有 popup 时不能提**）。
5. **popup 开着时 `IsWindowHovered(抽屉)` 是 false** → "点抽屉里别处"会连带把抽屉关掉；判定要用
   **上一帧快照**（`m_contentPopupWasOpen`）—— ImGui 在 NewFrame 就**已经关掉**弹层，帧末查
   `IsPopupOpen` 是 false，抓不到。
6. **分隔条画在 `ForegroundDrawList`** → 见第二节。

## 二、分隔条遮挡（2026-10-18 通用化）
- 历史：分隔条画在 `ImGui::GetForegroundDrawList()`，压在所有窗口之上 → 任何浮层（`+` 勾选菜单、
  右键菜单、下拉、浮动面板、角色窗口、tooltip）都会被竖线/横线穿过。
- 原机制只支持**一个**矩形（`setOccluder`，为 Content 抽屉特设）。
- 现机制：`SplitLayout::m_occluders` = **列表**，`clearOccluders()` / `addOccluder(x0,y0,x1,y1)`
  （空矩形自动忽略、坐标自动排序）。绘制时对每条分隔线的**可行区间**（沿线的轴向 t0..t1）做
  `sort → merge → 相减`，得到若干可见 `spans`；**锚点 / 抓手命中 / tooltip 都记在最长的那一段上**
  （否则日志里矩形会飘、脚本拿不到稳定锚点）。
- 数据来源：`EditorApp::collectSplitOccluders()`，**必须排在所有窗口都画完之后、`handleSplitters()`
  之前**。内容 = ① 浮层形态的 Content 抽屉（`L.content.w > 0 && !m_contentDock`）；② `g.Windows`
  里所有 `WasActive && !Hidden && 非 ChildWindow && Size >= 2px` 的顶层窗口。
- ⚠⚠ **必须排除落在 `LayoutRects` 上的布局面板**：分隔线正好压在两个面板的公共边界
  （`A.x1 == B.x0`）上，拿面板自己的矩形去裁 → 整条线被裁成 0 长，**等于把全部分隔条弄没**。
  排除用"四边都在 kEps(2px) 内等于某个 LayoutRect"判定。
- ⚠ 用 `WasActive` 而不是 `Active`：否则菜单刚弹出那一帧 `Active` 还没置位，会漏切一帧 → 竖线闪。

## 三、其他编辑器细则
- ⚠ **面板里"随选中状态出现/消失的一行"会让整棵树上下跳一行**：点行 → 选中 → 那行出现 → 树往下
  跳 → 鼠标已不在该行 → 再点变取消 → 又跳回。表面症状是"行尾图标点了没反应"
  （`verify_hierarchy_status` A2/A3 的 `hover=0 active=0`）。`Path: (no selection)` 面包屑整行删掉
  就是为这个 —— **别用 `Dummy` 占位糊过去**，那只是把抖动换成一条空白。
- ⚠ **Hierarchy 列头必须手画，不能用 `Table`/`Columns`**：`Columns` 会在列结束前固定
  `GetContentRegionAvail().x`，而这宽度还要给搜索框 `SetNextItemWidth(-1)` → `m_typeColX` 每帧算
  一次存成员，给树内所有行共用（行在 `BeginChild` 内、列头在外，各算各的**必差 8px 内边距、
  有滚动条差 14px**）。名字宽度不够**先丢 `[hidden]/[locked]` 后缀保名字**；截断走
  `drawClippedText` 在 **UTF-8 字符边界**二分（`(b & 0xC0) != 0x80` 才是字符起点），按字节截会给
  中文留半个字形。
- ⚠ **视口左上角模式条已删（10-17 用户要求）**：Lit/Unl/Nrm… 与右上 Lighting 下拉完全重复。视图
  模式只剩两个入口（Lighting 下拉 + `Shift+1..8`，日志 `VP-MODEBAR key=N` 仍在）。`VP-OVERLAY`
  从此 **3 字段** `master/stats/navGizmo`；左上角为空 → 统计 HUD 的 `statsY` 不再需要往下避让。
  偏好键 `vp.overlay.modeBar` 一并删除（老存档残留无害：`applyPrefs` 逐项按**本表**查键）。
- ⚠⚠ **浮动面板"最小化" = 把 `panelTrayHiddenRef(i)` 置 1**（收进底栏托盘），靠 `drawPanels()` 里
  `if (panelTrayHiddenRef(i)) continue;` 生效。**凡是自己无条件绘制的窗口必须自己认这个位** ——
  `CharacterWindow` 就踩了：`drawCharacterWindow()` 无条件画，于是"按钮因为 hidden 被跳过而消失、
  窗口照画"（10-18 用户报的 bug）。`setPanelOpen(idx, true)` 会顺手清掉 tray hidden 位。托盘按钮
  锚点 = `BAR-TRAY-<panelId>`（`trayButton()` 里 `strncmp(id,"tray_",5)` 剥前缀）。
- **主题** = `ui::applyTheme(int)`（`engine/ui/ImGuiTheme.*`，5 套）：度量/颜色解耦，VennDark 照抄 →
  默认逐像素不变；⚠ 浅色主题**逐个**收掉硬编码深色"孤岛"（顶栏/工具条 → `Token.barBg`）。
- **语言** = `tr()`/`trId()`（`editor/Localization.*`）：**代码只写英文**（硬约束），中文只在词典；
  ⚠ **窗口名兼作 ID** → 改名丢布局 → 一律 `trId(name,id)`。
- ⚠ **`TreeNode` 行上的图标按钮必须加 `ImGuiTreeNodeFlags_AllowOverlap`**，否则 `ItemHoverable`
  （imgui.cpp:5121）因**节点先提交**锁死 `HoveredId` → 图标收不到 hover/点击。
- ⚠ **偏好 `kPrefsVersion` ≠ 布局 `kFormatVersion`**：偏好**逐键覆盖** → 老存档会把新默认顶回 →
  升版本让老档作废。

## 四、断言纪律的展开
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取**最后一条**；自由格式
  `PASS —— …` 会 rc=0 却判 FAIL）—— **已踩第三次** → 新脚本先照抄已通过脚本的结尾块。
- ⚠⚠ **量测三坑**：① 扫描区间夹在"被测控件自己的矩形"里；② 比"两来源的数字"先**对齐采样窗口**；
  ③ "点了没反应"先怀疑**送达**。
- ⚠⚠ **删功能 = 同时删掉它的锚点和"等它的等待"**，否则留下两类隐性垃圾：
  ① **永真的空断言**（"某矩形不出现"在矩形永远不出现之后白送 PASS）→ 改成查**还活着**的同义锚点
  （如总闸自己的 `VP-OVERLAY master=0`），并把"这东西不存在"归到一个**独占的**反向用例里；
  ② **永不到来的 `ready_needle` / `wait_count`** → 每个会话**白等满 timeout**（`VP-RECT modebar=`
  删模式条后让 `verify_viewport` 每次空转 25 秒）。删断言时**项数会变**（10-17 那轮
  `verify_viewport` 42 → 41），是正常的，不是漏跑。
- ⚠ **sRGB 交换链二次编码** → 面板底**上屏实测**亮度 深色 85~132 / 浅色 225~245 → 像素判据阈值按
  实测定。
- ⚠⚠ **GUI harness 两类抖动**：① **首帧布局竞态**（分栏根 `ratio=0`）→ 连起重试到 `VP-RECT` 收敛；
  ② **Win32 窗口级联**（不给位置每次右移 38px）→ **显式摆 (0,0) + 光标挪屏幕右下**。
- ⚠ **必须 `MYVK_EDITOR_NO_HUD=1`** + `MYVK_GRID=0`；**采样区必须落在被测物体上**（俯角 8° 时"视口
  正中"只采到天空 → 误判"没生效"）。**抓图一律走 `capture_nonempty`**。⚠ 面板会盖住被测物 → 先
  `PANEL-MIN <panel>` 把面板收进底栏托盘再采样。
- ⚠ **"状态一致"与"像素一致"是两个断言**（前者可精确、后者只能**有界**）；**新断言必须做"否定
  测试"**（退化回 bug 版本 → 必须变红）；**临时诊断代码用完必须清**。
- ⚠ **FAIL 先怀疑「期望」再怀疑「实现」**；**兜底条件必须与放行条件同源**；⚠ **flaky 先查度量口径**。

## 五、构建与工具细节
- ⚠ **资产拷贝必须 `add_custom_target(... ALL)`**，禁用 `add_custom_command(TARGET ... POST_BUILD)`
  （后者只在**重新链接**时跑 → 改 shader/贴图**不生效**）。三目标拆分后（10-17）改成**全局一份**
  `VennShadersCopy` / `VennAssetsCopy`（`if(NOT TARGET)` 守卫），否则两个 exe 各拷一遍。
- ⚠ **三目标拆分**：`add_executable` 一改名（`Editor`→`VennEditor`），`add_dependencies` /
  `target_link_options` / 资产拷贝调用**全要同步**，产物名靠 `OUTPUT_NAME` 保住。
- ⚠ `LNK1168` = 残留 `Editor.exe` → `taskkill //F //IM Editor.exe`。
- ⚠ 日志 UTF-16 混合 → `grep -a`。⚠ `strings Editor.exe` **抓不到** ASCII 常量 → 验代码是否进二进制
  只能靠**行为**。新增 `.cpp` 自动进构建。
- ⚠ `build/bin/config/*.json`（layout/prefs/**render**）**未进版本控制**且运行会被写 → 像素断言必须用
  `MYVK_EDITOR_LAYOUT` / `MYVK_EDITOR_PREFS_PATH` / `MYVK_RENDER_SETTINGS` 钉死。
- ⚠ `glm::decompose` 在 **gtx**（`matrix_decompose.hpp`，不是 gtc）+ `GLM_ENABLE_EXPERIMENTAL` 必须
  写在**任何 glm 头之前**（含**本文件自己的头**）。GLFW **没有 `glfwGetWindowFocus`** →
  `glfwGetWindowAttrib(w, GLFW_FOCUSED)`。
- ⚠ 前置声明 `vks` / `physics` **必须写在 `namespace editor` 外面** → 否则成嵌套名把外层遮住
  （症状像"少 include"）。
- `capture_window.py`：⚠ `capture_client` 返回 **`(PIL.Image, ok)` 元组**。⚠ `kill_stale_editors`
  **无差别**杀 `Editor.exe`/`Game.exe` → **同时只能有一个 GUI 测试在跑**。截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**。自动化里键盘输入必须 `PostMessage(WM_CHAR)`
  （`keybd_event` 编辑器收不到，扫描码 0 → KEY_UNKNOWN）。
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新增 `verify_*.py` 必须进 `DEFAULT` + 补
  `TRIGGERS`；⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`**（很可能先红）→ **改 `TRIGGERS` +
  同步 fixture 是一个原子操作**。铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**
  （`--narrow` 下变假跳过）；横切名单**只以 `GLOBAL_GLOBS` 为准**（**没有 `editor/**`**）。

## 六、路线图明细结论
- **E03 设置层级**：`engine/render/RenderSettingsIO.*` = 5 层覆盖链（prefs→project→scene→object→
  camera，读时从最具体回退）+ `config/render.json` 落盘 + 无窗口自检（`MYVK_RENDER_SETTINGS_TEST=1`）。
  ⚠ 两坑：① **文件层与 env 层各要一个 applied 标记**，每阶段顺序 `applyToRegistry() →
  loadEnvOverrides()`（env 必须赢；只用一个标记会把阶段二绑定时的 env 值盖回去）；② **别按 UI 的
  lo/hi 夹文件值** —— `lshadow.bias` 默认 z=1.2 而 UI hi=1.0，一夹"导出→重载"就漂。
  ⚠ 新增 `engine/render/*` 必须进 `RENDER_MODULES`（`check_regression_map` F 组会抓）。
- **栅格抖动**（10-17 修，`grid.frag`）：根因不是采样相位，是 **1 m 格在 120 m 高空只占 ~4.3 px、
  线宽 1.4 px = 周期 4px 的高频图案**（量级：机位移 0.3 m → `mean=24.49`、`n>18=221k`）；**TAA 无效**
  （平均成灰噪点）。修法 = LOD 阈值 `0.9/2.0 → 5.0/12.0` + 补 **100 m 超级格**（否则高空整族被收掉
  → 地面全白）。复测 24.49 → **7.14**。
- **栅格"虚化/一片看不见"** = **共面深度冲突，不是 LOD**：判据是"**地板盖住的像素没线、出了地板
  边界立刻有线**"。根因：只在**顶点**里做 `clip.z -= 1e-6*clip.w`，而四边形横跨 ±2000 m，4 km 跨度
  上的插值误差与 1e-6 同量级。修法：`grid.frag` 里用 `vWorld` 重算 `gl_FragDepth` 再减 **1e-5**，
  `grid.vert` 的偏移删掉。⚠ 别用"放大到 1e-3"敷衍（20 m 处等于抬高 4 m，物体穿模）。
- ⚠ 定抖动/噪声的**唯一可靠对照是"同机位两次启动"**（= 0.000000 才能排除确定性因素）。
- **M2 三个"想当然"坑**：① `sweepCapsule` 是**推出式**的（撞斜棱边滑边退，找落点必须二分）；
  ② 手柄轴**正负两半**不算冲突（只比 code 会报 8 对假冲突）；③ `core::Input::keyPressed` 在
  `WantCaptureKeyboard` 时恒 false → 键位捕获必须直接 `glfwGetKey`。
- **运行时 P0 六项** = 运行时框架 / 游戏 UI / 音频 / 动画运行时 / 输入映射 / 存档（输入映射已 ✅）。
  ⚠ **最硬的墙 = Iris Xe `maxBoundDescriptorSets=8` 已用满** → G15 运行时 UI **不能**再要独立 set
  （动手前先量）；⚠ **蒙皮已在 GPU**（`Renderer.h:52` binding 6 jointMatrices SSBO）—— **别再规划
  "转 GPU 蒙皮"**。
- **视口工具条（10-17 晚重做）** = 一行 18 控件 + 9 个下拉（工具/坐标/四个吸附/视口看法，
  `ViewportPanel::drawToolbar`），新增 `GizmoMode::Select`；底栏 CMD 右侧 `+` **面板托盘**
  （`EditorApp::drawPanelPicker/trayButton`）。⚠ 三坑：① `panelOpen`=勾选（落盘）与
  `panelTrayHiddenRef`=窗口临时收起**必须分开**，否则"点按钮"会把按钮自己弄没；② 浮动面板要每帧
  `BringWindowToDisplayFront`，**否则被视口盖住**；③ `OpenPopup` 无定位时弹层开在**鼠标位置**，
  会被 `SetNextWindowPos(..., Appearing)` 修正。
- **物理 E-PHYS v1（10-16）**：`engine/physics`（PGS + 热启动 + split impulse / SAP / GJK-EPA + SAT /
  关节 5 种 / 岛级休眠）+ `RigidBodyComponent`/`PhysicsSettings` + 序列化；**仅 Play 态**；UI 在
  Inspector→Physics；自检 `MYVK_PHYS_TEST=1`；回归 `verify_physics.py`。

## 七、附：MEMORY.md 第二次压缩前原文（2026-10-18）

> 压缩版已取代它，这里留全文只为"细节别丢"。查具体某条背景时来这里翻。

# venn —— 项目笔记（规则索引 / 请勿超过 ~11 KB，超了尾部被截断）

> 只放 **纪律 + 速查 + 指针**，细则一律外置：
> 编辑器/验证细则 → `.workbuddy/memory/editor_details_archive.md`；渲染细则 →
> `render_details_archive.md`；下一步 → `docs/{render,editor,ship}_roadmap.md`；设计理由 →
> `docs/editor_architecture.md`。
> 手写 Vulkan 引擎（C++17，学习向），仓库 **`D:\A-VENN\code\cpp\venn-engin`**（CMake 名 `Venn`，
> 刻意不一致）。**右手系 Z-up**。产物 `Editor.exe`/`Game.exe`。分层
> `core→rhi→render→scene→assets`（另有 `ui`/`ecs`/`physics`/`script`；应用层 `editor`/`runtime`）；
> **engine/ 不认识 editor/ 与 runtime/**。改目录名后必须删 `build/` 重新 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
上下文快照**可能被截断/陈旧** → 跑回归 / 批量改文件 / 改契约前先读磁盘版，否则可能正在违反一条
**已生效**的纪律（已发生过：因快照缺条目误启动全量回归）。

## ⚠ 回归纪律（2026-10-15 用户明确要求）
**不要自动跑全量回归。** 只有用户**明确下达"开始全量回归"**才跑 `run_regression.py --all`；日常只跑
**受影响用例**（按 `TRIGGERS`）；收尾**不要**顺便来一轮全量。⚠ `--explain` 在没有基准快照时**一律
返回"全量"**（保守兜底），不代表"必须跑"。⚠ GUI 用例**同时只能有一个**（`kill_stale_editors` 无差别
杀 `Editor.exe`/`Game.exe`）。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry` 登记（`bindParam` 必须在成员默认初始化**之后**、`loadEnvOverrides()`
**之前**）③ **UI 面**（点得到）④ 序列化 ⑤ 回归用例 + TRIGGERS。**只有 `MYVK_*`、界面点不到的不算
交付**。把关 `tools/check_feature_surface.py`（**C 无 UI 面 / D 裸 env 漏网 必须清零**）。
⚠ `FeatureRegistry.*` 在 `GLOBAL_GLOBS` → 动它触发全量。

## ⚠ 本机环境（归档里"另一台机器"的记录别照抄）
- 仓库 **`D:\A-VENN\...`**（旧笔记的 `E:\...` 不存在）。**GPU = Intel Iris Xe**（不是 RTX 3060）→
  **8 个 set**，恰好 `maxBoundDescriptorSets = 8` **卡在线上** → 再加材质贴图槽得改架构。
- Python：`...\envs\default\Scripts\python.exe`（numpy+pillow）。⚠ **裸 `python` 不带 PIL/numpy** →
  回归整批 FAIL 报 `No module named 'PIL'`。

## 构建（细则 → 归档五）
入口 **`./make.cmd build`**；`TOOLCHAIN=auto` → 本机 **MSVC**（MinGW 缺失）。⚠ **MSVC 四坑**：
① `https_proxy`+`HTTPS_PROXY` 并存 → 假报 `MSB6001: CL.exe 开关无效`；② UTF-8 无 BOM + 中文注释 →
`C2001`（对策 `/utf-8`）；③ `.cmd`/`.bat` **必须纯 ASCII**；④ Make 可能用 sh 跑配方 → `echo` 里不能有
`(` `)` `|`。⚠ 资产拷贝必须 `add_custom_target(... ALL)`（`POST_BUILD` 只在重链时跑 → 改 shader 不
生效）。⚠ `.cpp` 新增自动进构建；`strings Editor.exe` **抓不到 ASCII 常量** → 验是否进二进制只能靠
**行为**。

## 环境变量（全表 → 归档）
- ⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP="near,far"`（默认
  `0.1/5000`，**顺序不能反**）；编辑器侧一律 `MYVK_EDITOR_` 前缀（白名单**按前缀放行**，不带前缀 =
  裸 env 漏网）。⚠ **落盘读/写不对称**：自动化下**默认不读**、显式 `MYVK_EDITOR_LAYOUT` 照读、
  **写一律拒绝**。
- ⚠⚠ **改任何"全局默认"都要回头 grep 依赖它的测试**（10-17：远平面 200→5000 让
  `verify_frustum_cull`/`verify_bvh` 的 132/4 基线漂到 94/42；`kPrefsVersion` 升 2 后
  `verify_viewport` 的 `write_prefs` 还写 1 → 整个档案被丢弃）。**正确修法是钉输入 / 升 fixture，
  不是改期望值。**

## 契约速查
- **Pass 序**：`Shadow(4 级联) → LocalShadow(12 层) → DepthPrePass → SSAO → ClusterBuild →
  VolumetricFog(×2) → Forward(天空盒→不透明→栅格→透明) → PostProcess → 交换链`。**compute 不能在 RP
  内执行**（深度预通道独立成 RP 且**无条件执行**）；**栅格夹在不透明与透明之间**、**绝不能**做成
  ImGui 叠加层。
- **"改一处就得同步改 N 处"是常态**：`FrameUBO`(768B)/推常量(128B 全满)/set0(25 binding)/变体位序/
  分簇编址/材质贴图 set(7)/旁路管线 range 须与主管线**逐字段相同**（否则 VUID-08600）。
- ⚠ 两处**默认槽**是雷：DBuffer 缺省必须 `m_defaultTransparent`（`m_defaultBlack` 整屏变黑）；探针
  binding 23 缺省必须绑 1×1 全黑 cube array，**不能"不绑"**。
- **颜色空间是契约**：albedo/emissive `srgb=true`；normal/ORM/高度图 `false`；着色器内**不做手动
  gamma**。

## 编辑器架构（细则 → 归档一/二/三）
- **加面板 = 表里加一行**：`panelTableVec()` + `PanelRegistry`（View 菜单/快捷键/图标/落盘四路自动
  派生）；**面板间只准依赖 `EditorContext`**，动引擎走 `EditorHost` 纯虚接口。面板开关权威在
  `PanelRegistry.cpp` 的 `g_open[kMaxPanels]`（**必须固定数组 + 引用**，vector 扩容 → `bool&` 悬垂）。
- ⚠ **默认一律"全关"**：`config/render.json` 缺失时行为**逐像素等于今天**（硬约束）。
- ⚠ **快捷键有排他**（面板 `Alt+1..8` / 工作区 `Ctrl+Alt+1..6` / 视图键 `Ctrl+1..8` /
  `Shift+Space` 须排在 `Space` 前）。**别引入 ImGui docking**（用自研 `SplitLayout`）。
- ⚠ **Inspector 左侧 11 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**枚举值 = 图标栏
  顺序**；**落盘写 id 不写下标**。⚠ 窄栏属性行槽位宽度按该列**最长标签**反推（`kPhysParamW`/
  `kFxParamW`）—— 拍脑袋的宽度会在滚动条出现后才吃掉标签尾巴。
- ⚠ **`All` 不是"融合视图"，是旧堆叠绘制顺序一字不改**（9 个旧脚本靠 `INS-*` 锚点断言分区存在）→
  自动化默认 `All`、交互默认 `Object`。**别"优化"它。**
- ⚠⚠ **锚点语义**：`logRect` 记在"提前返回"**之前**的锚点只能证明**函数被调用**，不能证明**分区
  可见** → **负向断言只能用记在返回之后的锚点。** ⚠ 图标改完真的放大截图肉眼看。
- ⚠⚠ **布局范式（10-17 改）**：**Content 不再是常驻分栏**，改成从底栏往上弹的**抽屉**；左栏下半收成
  0 → Hierarchy 吃满整条左栏；`syncPanelVisibility()` 里 **`kSlotContent` 恒 `false`**（**保留 case
  保持类型显式**，删了会掉进 `default: return true`）。`LayoutRects.bottom` = 状态栏上方 32px 通栏
  `[📁]Content [📄]Output Log [⌨]CMD`；**播放条已移除**（`transport` 恒 0×0，旧脚本还在读）。抽屉
  默认**不 dock**（浮层，点外部即关），勾 `Dock in layout` 才占布局。**六个坑 → 归档一**。
- ⚠⚠ **分隔条遮挡（10-18 通用化）**：`SplitLayout` 从单个 occluder 改成 **`m_occluders` 列表**
  （空矩形忽略、重叠合并、区间相减求可见 spans，锚点记在**最长那段**）；每帧由
  `EditorApp::collectSplitOccluders()` 喂数据（**必须排在所有窗口画完、handleSplitters 之前**）。
  ⚠⚠ **必须排除落在 `LayoutRects` 上的布局面板**（分隔线压在公共边界 `A.x1 == B.x0` 上，拿它自己
  裁 → 整条线被裁成 0，等于把全部分隔条弄没）。用 `WasActive` 不用 `Active`。**详情 → 归档二**。
- ⚠⚠ **浮动面板"最小化" = 把 `panelTrayHiddenRef(i)` 置 1**（收进底栏托盘），靠 `drawPanels()` 里
  `if (panelTrayHiddenRef(i)) continue;` 生效。**凡是自己无条件绘制的窗口（如 `CharacterWindow`）
  必须自己认这个位**，否则"按钮消失、窗口照画"（10-18 用户报的 bug）。托盘按钮锚点
  `BAR-TRAY-<panelId>`。
- ⚠ 树里"随选中状态出现/消失的一行"会让整棵树跳行（症状：行尾图标点了没反应）→ **别用 `Dummy`
  占位糊**。⚠ Hierarchy 列头必须手画（`Columns` 会固定 `GetContentRegionAvail().x`）；截断在
  **UTF-8 字符边界**二分。**详情 → 归档三**。
- ⚠⚠ **粒子预设的形态上限（10-18 闪电）**：出生体是**轴对齐长方体**，速度只有"锥 + 恒定加速度"，
  两者都**没有"x 与 z 相关"的自由度** → **单个发射器只能画直线或矩形**（窄 = 光柱、宽 = 光幕）。
  要分叉 / 辉光必须**多用几个发射器**（`FxPreset::forks` + `FxFork`，共用同一张 burst 表 +
  duration → 同时亮）。⚠ **倾斜只能用 `tc->rotation`**；`orbital` 是 `v += ω×(p-origin)`，
  得到的是"上下拧"的扇面，**不是斜线**。⚠ 倾斜只对 `flipDown` 预设生效，其余保持 `(0,0,0)`。

## 断言纪律（细则 → 归档四）
- ⚠ 必须 `MYVK_EDITOR_NO_HUD=1` + `MYVK_GRID=0`；**采样区必须落在被测物体上**；**抓图走
  `capture_nonempty`**；⚠ **面板会盖住被测物** → 先 `PANEL-MIN <panel>` 收进托盘再采样。
- ⚠ **"状态一致"与"像素一致"是两个断言**；**新断言必须做"否定测试"**（退化回 bug 版本必须变红）；
  **临时诊断代码用完必须清**。
- ⚠ **FAIL 先怀疑「期望」再怀疑「实现」**；**兜底条件必须与放行条件同源**；⚠ flaky 先查度量口径。
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取**最后一条**）—— 已踩
  **四次**；第四次是 `verify_viewport_bar` 只打 `ALL PASS`（`OK_RE` 不认）→ 60 项全 PASS 也判
  FAIL，rc=0、日志里**一条 FAIL 都没有**。**接手/新写的脚本先 `grep "结果:"`。**
- ⚠ 要截图的用例**别用 `MYVK_FRAMES` 控帧** → 等条件出现 + 主动关窗。
- ⚠⚠ **量测三坑**：① 扫描区间夹在"被测控件自己的矩形"里；② 比两来源数字先**对齐采样窗口**；
  ③ "点了没反应"先怀疑**送达**。
- ⚠⚠ **GUI harness 两类抖动**：① 首帧布局竞态 → 连起重试到 `VP-RECT` 收敛；② Win32 窗口级联 →
  **显式摆 (0,0) + 光标挪屏幕右下**。
- ⚠⚠ **删功能 = 同样删掉它的锚点和"等它的等待"**（否则留下永真空断言与永不到来的 `ready_needle`）。

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新增 `verify_*.py` 必须进 `DEFAULT` + 补
  `TRIGGERS`；⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`** → **改 TRIGGERS + 同步 fixture
  是原子操作**。铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**；横切名单**只以
  `GLOBAL_GLOBS` 为准**（**没有 `editor/**`**）。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 **`(PIL.Image, ok)` 元组**）；截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**；自动化键盘输入必须 `PostMessage(WM_CHAR)`。

## 路线图（明细 → 归档六 / `docs/*_roadmap.md`，这里只留结论 + 下一项）
**渲染 ✅** R0~R4 · R5-1 · R6-1 GPU 粒子（缺 ribbon/mesh/碰撞/预览）；**未做** R5-2/R5-3/R6-2~R6-5
（后 5 项是运行时 G21~G25 的硬前置）。**基准 = "以 unity 或 blender 为准"。**
**编辑器 ✅** B·C 全·E11.1·D·E25·五点需求 + Render/Particles/Physics 页·打磨七连·底栏/Content 抽屉·
分隔条通用遮挡 + 角色窗口最小化/叉号。**下一项 `E20` 覆盖指示器** 或 `E26` 画质档位。
**运行时 ✅ M1+M2**（`docs/ship_roadmap.md` G01~G32）：三目标 `VennEngine`/`VennEditor`/`VennRuntime`，
依赖单向 `runtime→engine←editor`；`engine/script/`（namespace **`vks`**）；`ScenePhysics.*` = 场景⇄
物理唯一实现（编辑器与运行时共用，防手感漂移）；G06 键位重绑定 = Preferences ▸ Bindings（落盘
`assets/input_map.json`）。**下一段 M3 `G11 动画 I`。**
**物理 ✅ v1**：PGS+热启动+split impulse / SAP / GJK-EPA+SAT / 关节 5 种 / 岛级休眠；**仅 Play 态**；
UI 在 Inspector→Physics；自检 `MYVK_PHYS_TEST=1`。
**自检现状**：特性 **34/34 有 UI 面**、裸 env 漏网 **0**；映射表 **54/62**、`DEFAULT` **62 例**。
⚠ 裸 env 白名单：`MYVK_PHYS`/`MYVK_PHYS_TEST`/`MYVK_STARTUP_DELAY`/`MYVK_RENDER_SETTINGS(_TEST)`/
`MYVK_RENDER_PREFS`（同 `MYVK_FRAMES`）、`ALLOW_SUFFIX` 加 `_DUMP`（同 `_DBG`）；`MYVK_RUNTIME`、
`MYVK_FIXED_DT` 在 `ALLOW_EXACT`，`MYVK_RUNTIME_*` 按前缀放行。
⚠ `verify_bvh`/`csm`/`ibl`/`particles` 偶发全量红 = harness flaky（单跑全 PASS）→ **别自动补跑
"凑干净"**；⚠ `FX-FREEZE` 日志 "bit-for-bit" 是**过度承诺** → 下次动 `engine/**` 顺手改。

## 八、附：MEMORY.md 第三次压缩前原文（2026-10-18 · 角色链分析同日）

> 压缩版已取代它，全文留此备查。

# venn —— 项目笔记（规则索引 / **别超过 ~9 KB**，超了尾部被截断）

> 只放 **纪律 + 速查 + 指针**：编辑器/验证细则 → `editor_details_archive.md`；渲染 →
> `render_details_archive.md`；下一步 → `docs/{render,editor,ship}_roadmap.md`；设计理由 →
> `docs/editor_architecture.md`。
> 仓库 **`D:\A-VENN\code\cpp\venn-engin`**（CMake 名 `Venn`，刻意不一致）。C++17 手写 Vulkan，
> **右手系 Z-up**。分层 `core→rhi→render→scene→assets`（另有 `ui`/`ecs`/`physics`/`script`；
> 应用层 `editor`/`runtime`）；**engine/ 不认识 editor/ 与 runtime/**。改名后删 `build/` 重 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
上下文快照**可能被截断/陈旧** → 跑回归 / 批量改文件 / 改契约前先读磁盘版。（已发生过：因快照缺
条目误启动全量回归。）

## ⚠ 回归纪律（用户明确要求）
**不自动跑全量。** 只有用户明确下达"开始全量回归"才 `run_regression.py --all`；日常只跑**受影响
用例**（按 `TRIGGERS`）。⚠ `--explain` 无基准快照时**一律报全量**（保守兜底），不代表必须跑。
⚠ GUI 用例**同时只能有一个**（`kill_stale_editors` 无差别杀 `Editor.exe`/`Game.exe`）。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry` 登记（`bindParam` 在成员默认初始化**之后**、`loadEnvOverrides()`**之前**）
③ **UI 面**（点得到）④ 序列化 ⑤ 回归用例 + `TRIGGERS`。**只有 `MYVK_*`、界面点不到的不算交付**。
把关 `tools/check_feature_surface.py`（C/D 类清零）。⚠ `FeatureRegistry.*` 在 `GLOBAL_GLOBS`。

## ⚠ 本机环境
仓库在 **`D:\A-VENN\...`**（旧笔记 `E:\...` 不存在）。**GPU = Intel Iris Xe** → **8 个 set**，恰好
`maxBoundDescriptorSets = 8` **卡在线上** → 再加贴图槽得改架构。Python 用
`...\envs\default\Scripts\python.exe`（numpy+pillow）；⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（细则 → 归档五）
入口 **`./make.cmd build`**（`TOOLCHAIN=auto` → 本机 **MSVC**）。⚠ **MSVC 四坑**：代理变量
`https_proxy`+`HTTPS_PROXY` 并存 → 假报 `MSB6001`；UTF-8 无 BOM + 中文注释 → `C2001`（要 `/utf-8`）；
`.cmd`/`.bat` **纯 ASCII**；Make 用 sh 跑配方 → `echo` 里不能有 `(` `)` `|`。
⚠ 资产拷贝必须 `add_custom_target(... ALL)`（`POST_BUILD` 只在重链时跑 → 改 shader 不生效）。
⚠ `strings` 抓不到 ASCII 常量 → 验代码是否进二进制只能靠**行为**。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP="near,far"`（默认 `0.1/5000`）；
编辑器侧一律 `MYVK_EDITOR_` 前缀（白名单按前缀放行）。⚠ **落盘读/写不对称**：自动化下默认不读、
显式 `MYVK_EDITOR_LAYOUT` 照读、**写一律拒绝**。
⚠⚠ **改"全局默认"要 grep 依赖它的测试**（10-17：远平面 200→5000 让两条基线整体漂；`kPrefsVersion`
升 2 后 fixture 还写 1 → 存档被丢弃）。**修法是钉输入 / 升 fixture，不是改期望值。**

## 契约速查
- **Pass 序**：`Shadow → LocalShadow → DepthPrePass → SSAO → ClusterBuild → VolumetricFog(×2) →
  Forward → PostProcess → 交换链`。**compute 不能在 RP 内执行**；**栅格夹在不透明与透明之间**、
  **绝不能**做成 ImGui 叠加层。
- **"改一处要同步改 N 处"是常态**：`FrameUBO`(768B)/推常量(128B 全满)/set0(25 binding)/变体位序/
  分簇编址/材质贴图 set(7)/旁路管线 range 须与主管线**逐字段相同**（否则 VUID-08600）。
- ⚠ 两处**默认槽**是雷：DBuffer 缺省必须 `m_defaultTransparent`；探针 binding 23 缺省必须绑 1×1
  全黑 cube array，**不能"不绑"**。
- **颜色空间是契约**：albedo/emissive `srgb=true`；normal/ORM/高度图 `false`；着色器内不做手动 gamma。

## 编辑器架构（细则 → 归档一/二/三）
- **加面板 = 表里加一行**（`panelTableVec()` + `PanelRegistry` → 菜单/快捷键/图标/落盘四路自动派生）；
  面板间**只准依赖 `EditorContext`**，动引擎走 `EditorHost` 纯虚接口。开关权威在 `PanelRegistry.cpp`
  的 `g_open[kMaxPanels]`（**固定数组 + 引用**，vector 扩容 → `bool&` 悬垂）。
- ⚠ **默认一律"全关"**：`config/render.json` 缺失时**逐像素等于今天**（硬约束）。⚠ **快捷键有排他**
  （面板 `Alt+1..8` / 工作区 `Ctrl+Alt+1..6` / 视图键 `Ctrl+1..8` / `Shift+Space` 排在 `Space` 前）；
  **别引入 ImGui docking**（用自研 `SplitLayout`）。
- ⚠ **Inspector 11 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**枚举值 = 图标栏顺序**；
  **落盘写 id 不写下标**；槽位宽度按该列**最长标签**反推。⚠ **`All` 不是融合视图**，是旧堆叠顺序
  一字不改（9 个旧脚本靠 `INS-*` 锚点）→ 自动化 `All`、交互 `Object`。**别"优化"它。**
- ⚠⚠ **锚点语义**：记在"提前返回"**之前**的锚点只证明**函数被调用**，不证明**分区可见** →
  **负向断言只能用返回之后的锚点**。⚠ 图标改完要放大截图肉眼看。
- ⚠⚠ **布局范式（10-17）**：Content 不再是常驻分栏 → 从底栏弹出的**抽屉**；左栏下半收 0 →
  Hierarchy 吃满；`syncPanelVisibility()` 里 **`kSlotContent` 恒 `false`**（**保留 case**，删了会掉进
  `default: return true`）；`LayoutRects.bottom` = 状态栏上方 32px 通栏；**播放条已移除**（`transport`
  恒 0×0，旧脚本还在读）。抽屉默认不 dock。**六坑 → 归档一**。
- ⚠⚠ **分隔条遮挡**：`SplitLayout` 用 `m_occluders` 列表（空矩形忽略、重叠合并、区间相减，锚点记在
  最长段）；每帧 `collectSplitOccluders()`（**排在所有窗口画完、handleSplitters 之前**）；⚠ **必须
  排除落在 `LayoutRects` 上的布局面板**，否则整条线被裁成 0（= 分隔条全消失）。用 `WasActive`。
- ⚠⚠ **浮动面板"最小化" = `panelTrayHiddenRef(i)=1`**，靠 `drawPanels()` 的
  `if (panelTrayHiddenRef(i)) continue;` 生效。**自己无条件绘制的窗口（如 `CharacterWindow`）必须
  自己认这个位**，否则"按钮消失、窗口照画"。托盘按钮锚点 **`BAR-TRAY-<panelId>`**（`tray_` 前缀会
  被剥掉）。**标题栏 × = 收进底栏**（勾选留着、按钮变暗），**不是取消勾选**。
- ⚠ 树里"随选中状态出现/消失的一行"会让整棵树跳行（症状：行尾图标点了没反应）→ 别用 `Dummy`
  占位糊。Hierarchy 列头**必须手画**（`Columns` 会固定 `GetContentRegionAvail().x`）；截断在
  **UTF-8 字符边界**二分。
- ⚠⚠ **粒子形态上限**：出生体是轴对齐长方体 + 速度只有"锥 + 恒定加速度" → **单发射器只能画直线或
  矩形**；分叉/辉光必须**多发射器**（`FxPreset::forks`，共用 burst 表 + duration 才同时亮）；**倾斜
  只能用 `tc->rotation`**（`orbital` 得到"上下拧"扇面，不是斜线）。

## 断言纪律（细则 → 归档四）
- ⚠ 必须 `MYVK_EDITOR_NO_HUD=1` + `MYVK_GRID=0`；**采样区落在被测物体上**；**抓图走
  `capture_nonempty`**；⚠ **面板会盖住被测物** → 先收进托盘再采样。
- ⚠ **"状态一致"与"像素一致"是两个断言**；**新断言必须做否定测试**（退化回 bug 版本要变红）；
  **临时诊断代码用完必须清**。⚠ **FAIL 先怀疑「期望」再怀疑「实现」**；**兜底条件必须与放行条件
  同源**；flaky 先查度量口径；**峰值类指标的容差不能比瞬时值更紧**（10-18：`maxAlive` 容差 2 <
  `alive` 的 8 → 随机红两次）。
- ⚠⚠ **锚点拼错 = 断言永远为假**（`BAR-TRAY-tray_particles` 查不到 → 静默 None、连点都没点），
  和"永真空断言"一样废 → **改完锚点必须跑一遍看到它变绿**。⚠ `wait_rect` 读"最后一条"：窗口收起后
  日志不再刷新 → 读到**旧值** → 会**假通过**。
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取最后一条）—— 已踩 **五次**
  （最近 `verify_editor_round4` 只打「全部通过。」→ 全 PASS 也判 FAIL、报 `0/1 通过`）。
- ⚠ 要截图的用例**别用 `MYVK_FRAMES` 控帧** → 等条件出现 + 主动关窗。
- ⚠⚠ **量测三坑**：① 扫描区间夹在"被测控件自己的矩形"里；② 比两来源数字先**对齐采样窗口**；
  ③ "点了没反应"先怀疑**送达**。⚠⚠ **GUI harness 两类抖动**：① 首帧布局竞态 → 连起重试到
  `VP-RECT` 收敛；② Win32 窗口级联 → **显式摆 (0,0) + 光标挪屏幕右下**。
- ⚠⚠ **删功能 = 同样删掉它的锚点和"等它的等待"**（否则留下永真空断言与永不到来的 `ready_needle`）。

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新增 `verify_*.py` 必须进 `DEFAULT` + 补
  `TRIGGERS`；⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`**（改 TRIGGERS + 同步 fixture 是
  原子操作）。铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**；横切名单**只以
  `GLOBAL_GLOBS` 为准**（**没有 `editor/**`**）。⚠ **新断言长在哪个文件上，那个文件就得进本用例的
  `TRIGGERS`**，否则改它时断言被静默跳过。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 **`(PIL.Image, ok)` 元组**）；截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**；自动化键盘输入必须 `PostMessage(WM_CHAR)`。

## 路线图（明细 → 归档六 / `docs/*_roadmap.md`）
**渲染 ✅** R0~R4 · R5-1 · R6-1 GPU 粒子（缺 ribbon/mesh/碰撞/预览）；**未做** R5-2/R5-3/R6-2~R6-5
（后 5 项是运行时 G21~G25 的硬前置）。**基准 = "以 unity 或 blender 为准"。**
**编辑器 ✅** B·C 全·E11.1·D·E25·五点需求 + Render/Particles/Physics 页·打磨七连·底栏/Content
抽屉·分隔条通用遮挡·角色窗口最小化/叉号·**粒子面板逐发射器编辑 + 层级右键打开（10-18）**。
**下一项** `E20` 覆盖指示器 或 `E26` 画质档位。⚠ **自定义 PNG 粒子贴图未接线**：粒子 draw set 只有
6 个 binding、没有贴图槽，且 8 个 set 已满 → 要做得先加贴图数组 + 资源管线。
**运行时 ✅ M1+M2**（`docs/ship_roadmap.md` G01~G32）：三目标 `VennEngine`/`VennEditor`/`VennRuntime`，
依赖单向 `runtime→engine←editor`；`engine/script/`（namespace **`vks`**）；`ScenePhysics.*` = 场景⇄
物理唯一实现；G06 键位重绑定 = Preferences ▸ Bindings（`assets/input_map.json`）。**下一段 M3 G11。**
**物理 ✅ v1**：PGS+热启动+split impulse / SAP / GJK-EPA+SAT / 关节 5 种 / 岛级休眠；**仅 Play 态**；
UI 在 Inspector→Physics；自检 `MYVK_PHYS_TEST=1`。
**自检现状**：特性 **34/34 有 UI 面**、裸 env 漏网 **0**；映射表 **58/66**。裸 env 白名单：
`MYVK_PHYS`/`MYVK_PHYS_TEST`/`MYVK_STARTUP_DELAY`/`MYVK_RENDER_SETTINGS(_TEST)`/`MYVK_RENDER_PREFS`
（同 `MYVK_FRAMES`），`ALLOW_SUFFIX` 加 `_DUMP`（同 `_DBG`）；`MYVK_RUNTIME`/`MYVK_FIXED_DT` 在
`ALLOW_EXACT`，`MYVK_RUNTIME_*` 按前缀放行。
⚠ `verify_bvh`/`csm`/`ibl`/`particles` 偶发全红 = harness flaky（单跑全 PASS）→ **别自动补跑"凑
干净"**；⚠ `FX-FREEZE` 日志 "bit-for-bit" 是**过度承诺** → 下次动 `engine/**` 顺手改。

## 九 · 2026-10-18 压缩前全文（MEMORY.md，11.8 KB → 目标 9 KB）

# venn —— 项目笔记（**别超过 ~9 KB**，超了尾部被截断）

> 只放 **纪律 + 速查 + 指针**。细则 → `editor_details_archive.md`（**每次压缩前全文都追加在这，
> 细节不丢**）/ `render_details_archive.md`；下一步 → `docs/{render,editor,ship,character_pipeline}
> _roadmap.md`；设计理由 → `docs/editor_architecture.md`。
> 仓库 **`D:\A-VENN\code\cpp\venn-engin`**（CMake 名 `Venn`，刻意不一致）。C++17 手写 Vulkan，
> **右手系 Z-up**。分层 `core→rhi→render→scene→assets`（另有 `ui`/`ecs`/`physics`/`script`；应用层
> `editor`/`runtime`）；**engine/ 不认识 editor/ 与 runtime/**。改名后删 `build/` 重 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
上下文快照**可能被截断/陈旧** → 跑回归 / 批量改文件 / 改契约前先读磁盘版。（已发生过：因快照缺条目
误启动全量回归。）

## ⚠ 回归纪律（用户明确要求）
**不自动跑全量。** 只有用户明确下达"开始全量回归"才 `run_regression.py --all`；日常只跑**受影响
用例**（按 `TRIGGERS`）。⚠ `--explain` 无基准快照时**一律报全量**（保守兜底），不代表必须跑。
⚠ GUI 用例**同时只能有一个**（`kill_stale_editors` 无差别杀 `Editor.exe`/`Game.exe`）。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry` 登记（`bindParam` 在成员默认初始化**之后**、`loadEnvOverrides()`**之前**）
③ **UI 面**（点得到）④ 序列化 ⑤ 回归用例 + `TRIGGERS`。**只有 `MYVK_*`、界面点不到的不算交付**。
把关 `check_feature_surface.py`（C/D 类清零）。⚠ `FeatureRegistry.*` 在 `GLOBAL_GLOBS`。

## ⚠ 本机环境
仓库在 **`D:\A-VENN\...`**（旧笔记 `E:\...` 不存在）。**GPU = Intel Iris Xe** → **8 个 set**，恰好
`maxBoundDescriptorSets = 8` **卡在线上** → 再加贴图槽得改架构。Python 用
`...\envs\default\Scripts\python.exe`（numpy+pillow）；⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（细则 → 归档五）
入口 **`./make.cmd build`**（`TOOLCHAIN=auto` → 本机 **MSVC**）。⚠ **MSVC 四坑**：代理变量
`https_proxy`+`HTTPS_PROXY` 并存 → 假报 `MSB6001`；UTF-8 无 BOM + 中文注释 → `C2001`（要 `/utf-8`）；
`.cmd`/`.bat` **纯 ASCII**；Make 用 sh 跑配方 → `echo` 里不能有 `(` `)` `|`。
⚠ 资产拷贝必须 `add_custom_target(... ALL)`（`POST_BUILD` 只在重链时跑 → 改 shader 不生效）。
⚠ `strings` 抓不到 ASCII 常量 → 验证代码进没进二进制只能靠**行为**。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP="near,far"`（默认 `0.1/5000`）；
编辑器侧一律 `MYVK_EDITOR_` 前缀（白名单按前缀放行）。⚠ **落盘读/写不对称**：自动化下默认不读、
显式 `MYVK_EDITOR_LAYOUT` 照读、**写一律拒绝**。
⚠⚠ **改"全局默认"要 grep 依赖它的测试**（10-17：远平面 200→5000 让两条基线整体漂；`kPrefsVersion`
升 2 后 fixture 还写 1 → 存档被丢弃）。**修法是钉输入 / 升 fixture，不是改期望值。**

## 契约速查
- **Pass 序**：`Shadow → LocalShadow → DepthPrePass → SSAO → ClusterBuild → VolumetricFog(×2) →
  Forward → PostProcess → 交换链`。**compute 不能在 RP 内执行**；**栅格夹在不透明与透明之间**、
  **绝不能**做成 ImGui 叠加层。
- **"改一处要同步改 N 处"是常态**：`FrameUBO`(768B)/推常量(128B 全满)/set0(25 binding)/变体位序/
  分簇编址/材质贴图 set(7)/旁路管线 range 须与主管线**逐字段相同**（否则 VUID-08600）。
- ⚠ 两处**默认槽**是雷：DBuffer 缺省必须 `m_defaultTransparent`；探针 binding 23 缺省必须绑 1×1
  全黑 cube array，**不能"不绑"**。
- **颜色空间**：albedo/emissive `srgb=true`；normal/ORM/高度图 `false`；着色器内不做手动 gamma。
- **动画/蒙皮**：`jointBase` 三路来源（MDI `aux.x` / 合批与基线 `push.skin.x`）；`invMeshNode` 与
  `push.model` 里的 meshNodeWorld **约掉** → 静态与蒙皮共用同一条 draw。⚠ **采样不许留在 Renderer
  里**（否则无窗口测不了），见 `docs/character_pipeline_roadmap.md` §3.3。

## 编辑器架构（细则 → 归档一/二/三）
- **加面板 = 表里加一行**（`panelTableVec()` + `PanelRegistry` → 菜单/快捷键/图标/落盘四路派生）；
  面板间**只准依赖 `EditorContext`**，动引擎走 `EditorHost` 纯虚接口。开关权威在 `PanelRegistry.cpp`
  的 `g_open[kMaxPanels]`（**固定数组 + 引用**，vector 扩容 → `bool&` 悬垂）。
- ⚠ **默认一律"全关"**：`config/render.json` 缺失时**逐像素等于今天**（硬约束）。⚠ **快捷键排他**
  （面板 `Alt+1..8` / 工作区 `Ctrl+Alt+1..6` / 视图键 `Ctrl+1..8` / `Shift+Space` 排在 `Space` 前）；
  **别引入 ImGui docking**（用自研 `SplitLayout`）。
- ⚠ **Inspector 11 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**枚举值 = 图标栏顺序**；
  **落盘写 id 不写下标**；槽宽按该列**最长标签**反推。⚠ **`All` 不是融合视图**，是旧堆叠顺序一字
  不改（9 个旧脚本靠 `INS-*`）→ 自动化 `All`、交互 `Object`。**别"优化"它。**
- ⚠⚠ **锚点语义**：记在"提前返回"**之前**的锚点只证明**函数被调用**，不证明**分区可见** →
  **负向断言只能用返回之后的锚点**。⚠ 图标改完要放大截图肉眼看。
- ⚠⚠ **布局 / 遮挡 / 托盘**（细节 → 归档一）：Content 是从底栏弹出的**抽屉**（`kSlotContent` 恒
  `false`，**保留 case**，删了会掉进 `default: return true`）；分隔条靠 `m_occluders` + 每帧
  `collectSplitOccluders()`（**排在所有窗口画完、`handleSplitters` 之前**；**必须排除落在
  `LayoutRects` 上的布局面板**，否则整条被裁成 0）；"最小化" = `panelTrayHiddenRef(i)=1`，
  **自己无条件绘制的窗口（如 `CharacterWindow`）必须自己认这个位**；托盘锚点
  **`BAR-TRAY-<panelId>`**（`tray_` 会被剥掉）；**标题栏 × = 收进底栏**（勾选留着、按钮变暗）。
- ⚠ 树里"随选中状态出现/消失的一行"会让整棵树跳行 → 别用 `Dummy` 占位糊。Hierarchy 列头**必须
  手画**（`Columns` 会固定 `GetContentRegionAvail().x`）；截断在 **UTF-8 字符边界**二分。
- ⚠⚠ **粒子形态上限**：出生体是轴对齐长方体 + 速度只有"锥 + 恒定加速度" → **单发射器只能画直线或
  矩形**；分叉/辉光必须**多发射器**（`FxPreset::forks`，共用 burst 表 + duration 才同时亮）；**倾斜
  只能用 `tc->rotation`**（`orbital` 得到"上下拧"扇面，不是斜线）。

## 断言纪律（细则 → 归档四）
- ⚠ 必须 `MYVK_EDITOR_NO_HUD=1` + `MYVK_GRID=0`；**采样区落在被测物体上**；**抓图走
  `capture_nonempty`**；⚠ **面板会盖住被测物** → 先收进托盘再采样。
- ⚠ **"状态一致"与"像素一致"是两个断言**；**新断言必须做否定测试**（退化回 bug 版本要变红）；
  **临时诊断代码用完必须清**。⚠ **FAIL 先怀疑「期望」再怀疑「实现」**；**兜底条件必须与放行条件
  同源**；**峰值类指标的容差不能比瞬时值更紧**。
- ⚠⚠ **锚点拼错 = 断言永远为假**（查不到 → 静默 None、**连点都没点**）→ **改完锚点必须跑一遍看到
  它变绿**。⚠ `wait_rect` 读"最后一条"：窗口收起后日志不再刷新 → 读到**旧值** → **假通过**。
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取最后一条）—— 已踩 **五次**
  （`verify_editor_round4` 只打「全部通过。」→ 全 PASS 也判 FAIL、报 `0/1 通过`）。
- ⚠ 要截图的用例**别用 `MYVK_FRAMES` 控帧** → 等条件出现 + 主动关窗。⚠ **量测三坑**：扫描区间
  夹在"被测控件自己的矩形"里 / 比两来源先**对齐采样窗口** / "点了没反应"先怀疑**送达**。
  ⚠⚠ **harness 两类抖动**：首帧布局竞态 → 连起重试到 `VP-RECT` 收敛；Win32 窗口级联 → **显式摆
  (0,0) + 光标挪屏幕右下**。
- ⚠⚠ **像素断言别用"整图平均差"**：主角占屏 4% → Idle 前后 0.02/255、Walk 的"站着 vs 跪着"
  也只有 **0.41/255**（阈值 0.5 直接假红）。用 **显著变化像素占比**（`ImageChops.difference` →
  直方图，裁到 `VP-RECT`，阈值 8）：同样两对是 0.1% / **2.48%**，同配置两跑基线 **0.000%**。
  判据 = 占比 > 0.5% **且 > 10× 基线**（自校准，换模型/分辨率不红）。⚠ 同 clip 钉两个时间也
  不够（待机动画全程幅度都小）→ **换 clip**（Idle → Walk）。
- ⚠⚠ **删功能 = 同样删掉它的锚点和"等它的等待"**（否则留下永真空断言与永不到来的 `ready_needle`）。

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新增 `verify_*.py` 必须进 `DEFAULT` + 补
  `TRIGGERS`；⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`**（改表 + 同步 fixture 是原子
  操作）。铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**；横切名单**只以
  `GLOBAL_GLOBS` 为准**（**没有 `editor/**`**）。⚠ **新断言长在哪个文件上，那个文件就得进本用例的
  `TRIGGERS`**，否则改它时断言被静默跳过。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 **`(PIL.Image, ok)` 元组**）；截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**；自动化键盘输入必须 `PostMessage(WM_CHAR)`。
  ⚠ `MYVK_EDITOR_IMPORT` 与 `MYVK_EDITOR_CONSOLE_CMD` 都支持 **`;` 多值**（多实例：第 i 个沿 +X
  排 1.5m、名字自动 `_1/_2`）；控制台 `anim.time <秒> [实体名]` / `anim.clip <名字> [实体名]`
  （不给名字 = 当前选中）。⚠⚠ **自检日志的 `detail=` 里不能有空格**（脚本按 `\S*` 解析，
  一个空格 = 整行失配 = 断言在脚本眼里"不存在"，引擎 PASS 却报"缺少断言"）。

## 路线图（权威 → `docs/*_roadmap.md`；已完成清单与自检数字 → 归档六）
- **渲染** R0~R4·R5-1·R6-1 ✅；**未做** R5-2/R5-3/R6-2~R6-5（= 运行时 G21~G25 硬前置）。
  基准 = "以 unity 或 blender 为准"。⚠ 粒子自定义 PNG 未接线：draw set 只 6 个 binding、8 个 set 满。
- **编辑器** B·C·E11.1·D·E25 ✅（10-18 收尾：粒子面板逐发射器 + 层级右键）。**下一项** `E20`/`E26`。
- **运行时** M1+M2 ✅（G01~G32；依赖单向 `runtime→engine←editor`；`engine/script/` 用 `vks`）。
- ⚠⚠ **角色表现链** → `docs/character_pipeline_roadmap.md`（A1 Animator → A2 状态机 → A3 层 →
  A4 根运动 → A5 事件 → A6 IK → A7 面板）。**A0 五项 + 用例 ✅（10-18）**：唯一装配入口
  `engine/scene/ModelInstance`（D0 已修）· `skinned` 存档块 · `engine/animation/{Pose,AnimatorSystem}`
  （`Skeleton::sample` 保留兼容壳）· `DrawItem::jointBase` 逐绘制项（多实例实测 0/21/42）·
  Inspector 时间轴。⚠ **`Layer` 是布局概念，别的名字别撞**；A0 案例见 `verify_character_import.py`。
- **物理** v1 ✅（PGS+热启动+split impulse / SAP / GJK-EPA+SAT / 关节 5 种 / 岛级休眠；**仅 Play 态**）。
- ⚠ flaky：`verify_bvh`/`csm`/`ibl`/`particles` 偶发全红（单跑全 PASS）→ **别补跑"凑干净"**。
- ⚠ `FX-FREEZE` 日志 "bit-for-bit" 是**过度承诺** → 下次动 `engine/**` 顺手改。

---

## 2026-10-18 第二轮：U6/U7/U8（从 MEMORY.md 压缩下来的全文）

### Vulkan NDC 的 y 向下
UI 像素 y→NDC 必须写 `y/H*2-1`。3D 那边的 `proj[1][1] *= -1` 已经把 OpenGL 约定翻成 Vulkan，
UI 这里就只剩**零次**。写反（`1 - 2y/H`）= 整层 UI **上下镜像**（血条跑到顶部、`100 / 100`
倒着写），而**日志全是对的** —— 只有截图能看出来。这是本轮最贵的一个 bug。

### UI 的其它三条
- 命中框的输出尺寸必须与 PostProcess **同源**：`Renderer::uiOutputExtent()`（= `post->outputExtent()`），
  用窗口尺寸或 `renderExtent()` 会整体偏一小段比例 → "光标在按钮上却点不中"。
- 焦点导航自测**必须用数组顺序故意打乱的布局**（左右两列）。垂直单列里"几何最近"与
  "数组下一个"恰好一致，是最容易假绿的地方。
- 验收别用 `VP-RECT` 算坐标（离屏图被 ImGui 等比缩放）→ 用差分包围盒。

### i18n 搬家（U7）
- 权威 `engine/i18n/ZhTable.cpp`（1559 条）；`editor::tr` 只转发，792 处 `TR()` 零改动；
  英文模式 `tr()` 在查表**前** return → 表不构造、零开销（`I18N-KEY lang=en count=0`）。
- ⚠⚠ **搬词典必须同步所有 `DICT_SRC`**：`verify_ui2.py` 与 `gen_sdf_atlas.py` 都曾写着
  `editor/Localization.cpp`。漏改 → "词典全覆盖"这条硬断言仍在跑，但取值域变成 101 行转发壳
  （956 → 211 个字），红得"变小"，最难发现。
- ⚠ **图集必须重生成**：旧图集缺 279 个汉字 → 界面上是方块，而"有字了"的画面上看不出来。
  重生成命令 `gen_sdf_atlas.py --atlas 2048 --atlas-h 3072` → 1059 字形 / 0.54 MB / 缺 0。
  顺带加了 `--atlas-h`（2048² 只差 2.4% 装不下，4096² 只用 28%）。图集尺寸由 JSON 的
  `atlas.w/h` 驱动，运行时**不假设 2 的幂**。
- `MYVK_LANG` 是路线图点名的例外名字（不合 `MYVK_RUNTIME_*` 前缀），已进 `ALLOW_EXACT`。

### 暂停 / 焦点（U6）
- 暂停的权威是**状态栈顶名**（`topName()=="Paused"`），不是 bool 成员。
- "暂停时按 W 不动"靠 `pushContext("Menu")` 让 Gameplay 整组**结构性**失效，不是靠 if。
- 状态迁移只在 `onUpdate` **末尾**做（`tickUi` / `Paused::onUpdate` 只置意图位），否则 pop 自己
  会让遍历中的 vector 失效。
- `tickUi` 挂 `cb.pollInput`：每帧无条件调用、不受 `blocksUpdate`/`timeScale` 影响、**早于
  fixedUpdate** → `route()` 天然早于 `PlayerController::update()`。
- `logFocusIfChanged()` 是私有的、由 `setFocus/move/cycle/focusFirst` 内部调用 → 打开菜单时
  不调 `focusFirst()` 就一条 `UI2-FOCUS` 都不打，脚本分不清"菜单没开"和"开了没焦点"。
- GLFW 从 `CURSOR_DISABLED` 切回 `NORMAL` 会把光标**还原到中心附近** → 正好压在菜单按钮上 →
  `hoverAt` 抢走焦点。这是**正确行为**，所以验收断言"在按钮环上"而不是钉死 `resume`。

### 伤害数字夹具（U8）
`frame` 是**渲染帧号**（`time().frameCount()`），脚本却按**定步**求值 —— 一帧两个定步时
`frame == 60` 会执行两次（实测 `damage(30)` 扣成 40 血、弹两个数字）。夹具因此写成幂等：
`damage(30)` 走一遍真实接口 + `setHealth(70)` 绝对值钉死。

---

## 2026-10-18 MEMORY.md 压缩前全文（13.8 KB 版）

```
# venn —— 项目笔记（**软目标 ~11 KB**；实测 11.8 KB 仍被完整注入、未见截断。为留余量按 11 KB 控）

> 只放 **纪律 + 速查 + 指针**。细则 → `editor_details_archive.md`（**压缩前全文都追加在这**）/
> `render_details_archive.md`；下一步 → `docs/*_roadmap.md`；设计理由 → `docs/editor_architecture.md`。
> 仓库 **`D:\A-VENN\code\cpp\venn-engin`**（CMake 名 `Venn`，刻意不一致）。C++17 手写 Vulkan，
> **右手系 Z-up**；`core→rhi→render→scene→assets`（另有 `ui`/`ecs`/`physics`/`script`；应用层
> `editor`/`runtime`，**engine/ 不认识后两者**）。改名后删 `build/` 重 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
快照**可能被截断/陈旧** → 跑回归 / 批量改文件 / 改契约前先读磁盘。（已因缺条目误启动过全量回归。）

## ⚠ 回归纪律（用户明确要求）
**不自动跑全量**：只有用户明确下达才 `run_regression.py --all`，日常只跑**受影响用例**（按
`TRIGGERS`）。⚠ `--explain` 无基准快照时**一律报全量**（保守兜底），不代表必须跑。
⚠ GUI 用例**同时只能有一个**（`kill_stale_editors` 无差别杀 `Editor.exe`/`Game.exe`）。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry`（`bindParam` 在成员默认初始化**之后**、`loadEnvOverrides()`**之前**）
③ **UI 面** ④ 序列化 ⑤ 用例 + `TRIGGERS`。**只有 `MYVK_*`、界面点不到的不算交付**。把关
`check_feature_surface.py`（C/D 清零）。⚠ `FeatureRegistry.*` 在 `GLOBAL_GLOBS`。

## ⚠ 本机环境
**GPU = Intel Iris Xe** → 3D 主通道同时绑 **8 个 set**，恰好 `maxBoundDescriptorSets=8` 卡在线上
（再加贴图槽得改架构；⚠ 但 UI 是独立管线只绑 1 个，**不受此限**）。Python 用
`...\envs\default\Scripts\python.exe`（numpy+pillow）；⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（细则 → 归档五）
入口 **`./make.cmd build`**（`TOOLCHAIN=auto` → **MSVC**）。⚠ **四坑**：`https_proxy`+`HTTPS_PROXY`
并存 → 假报 `MSB6001`；UTF-8 无 BOM + 中文 → `C2001`（要 `/utf-8`）；`.cmd`/`.bat` **纯 ASCII**；
Make 用 sh 跑配方 → `echo` 里不能有 `(` `)` `|`。⚠ 资产拷贝必须 `add_custom_target(... ALL)`
（`POST_BUILD` 只在重链时跑 → 改 shader 不生效）。⚠ `strings` 抓不到 ASCII 常量 → 验代码进没进
二进制只能靠**行为**。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP="near,far"`（默认 `0.1/5000`）；
编辑器侧一律 `MYVK_EDITOR_` 前缀（白名单按前缀放行）。⚠ **落盘读/写不对称**：自动化下默认不读、
显式 `MYVK_EDITOR_LAYOUT` 照读、**写一律拒绝**。⚠⚠ **改"全局默认"要 grep 依赖它的测试**（远平面
200→5000 让基线整体漂；`kPrefsVersion` 升 2 后 fixture 还写 1 → 存档被丢弃）。**修法是钉输入 /
升 fixture，不是改期望值。**

## 契约速查
- **Pass 序**：`Shadow → LocalShadow → DepthPrePass → SSAO → ClusterBuild → VolumetricFog(×2) →
  Forward → PostProcess → 交换链`。**compute 不能在 RP 内执行**；**栅格夹在不透明与透明之间**、
  **绝不能**做成 ImGui 叠加层。
- **"改一处要同步改 N 处"是常态**：`FrameUBO`(768B)/推常量(128B 全满)/set0(25 binding)/变体位序/
  分簇编址/材质贴图 set(7)/旁路管线 range 须与主管线**逐字段相同**（否则 VUID-08600）。
- ⚠ 两处**默认槽**是雷：DBuffer 缺省必须 `m_defaultTransparent`；探针 binding 23 缺省必须绑 1×1
  全黑 cube array，**不能"不绑"**。**颜色空间**：albedo/emissive `srgb=true`，normal/ORM/高度图
  `false`，着色器内不做手动 gamma。
- **动画/蒙皮**：`jointBase` 三路来源（MDI `aux.x` / 合批与基线 `push.skin.x`）；`invMeshNode` 与
  `push.model` 里的 meshNodeWorld **约掉** → 静态与蒙皮共用同一条 draw。⚠ **采样不许留在 Renderer
  里**（否则无窗口测不了），见 `docs/character_pipeline_roadmap.md` §3.3。

## 编辑器架构（细则 → 归档一/二/三，全文 → 归档九）
- **加面板 = 表里加一行**（`panelTableVec()` + `PanelRegistry` → 菜单/快捷键/图标/落盘四路派生）；
  面板间**只准依赖 `EditorContext`**，动引擎走 `EditorHost` 纯虚接口。开关权威在 `PanelRegistry.cpp`
  的 `g_open[kMaxPanels]`（**固定数组 + 引用**，vector 扩容 → `bool&` 悬垂）。
- ⚠ **默认一律"全关"**（`config/render.json` 缺失 → **逐像素等于今天**）。⚠ **快捷键排他**（面板
  `Alt+1..8` / 工作区 `Ctrl+Alt+1..6` / 视图键 `Ctrl+1..8` / `Shift+Space` 在 `Space` 前）；
  **别引入 ImGui docking**（用自研 `SplitLayout`）。
- ⚠ **Inspector 11 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**枚举值 = 图标栏顺序**；
  **落盘写 id 不写下标**。⚠ **`All` 不是融合视图**，是旧堆叠顺序一字不改（9 个脚本靠 `INS-*`）→
  自动化 `All`、交互 `Object`，**别"优化"它**。⚠⚠ **记在"提前返回"之前的锚点只证明函数被调用**，
  **负向断言只能用返回之后的锚点**；图标改完要放大截图肉眼看。
- ⚠⚠ **布局 / 遮挡 / 托盘**：Content 是底栏弹出的**抽屉**（`kSlotContent` 恒 `false`，保留 case）；
  分隔条靠 `m_occluders` + `collectSplitOccluders()`（排在所有窗口画完之后；**必须排除落在
  `LayoutRects` 上的布局面板**，否则整条被裁成 0）；"最小化" = `panelTrayHiddenRef(i)=1`，**自己
  无条件绘制的窗口（如 `CharacterWindow`）也必须认这个位**；托盘锚点 `BAR-TRAY-<panelId>`；
  **标题栏 × = 收进底栏**。⚠ 树里"随选中出现/消失的一行"会让整棵树跳行；Hierarchy 列头必须手画，
  截断在 **UTF-8 字符边界**二分。⚠⚠ **粒子**：单发射器只能画直线或矩形（出生体轴对齐长方体 +
  速度只有锥+恒定加速度）；分叉/辉光必须**多发射器**；倾斜**只能用 `tc->rotation`**。

## 断言纪律（细则 → 归档四/九）
- ⚠ 必须 `MYVK_EDITOR_NO_HUD=1` + `MYVK_GRID=0`；**采样区落在被测物体上**；抓图走 `capture_nonempty`；
  **面板会盖住被测物** → 先收进托盘再采样。
- ⚠ **"状态一致"与"像素一致"是两个断言**；**新断言必须做否定测试**；**临时诊断代码用完必须清**。
  ⚠ **FAIL 先怀疑「期望」再怀疑「实现」**；**兜底条件必须与放行条件同源**；峰值类指标的容差不能比
  瞬时值更紧。⚠ 锚点拼错 = **断言永远为假**（静默 None）→ 改完必须跑一遍看它变绿；`wait_rect` 读
  "最后一条"，窗口收起后读到**旧值** → 假通过。
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取最后一条）—— 已踩 **五次**。
- ⚠ 要截图的用例**别用 `MYVK_FRAMES` 控帧** → 等条件出现 + 主动关窗。⚠ **量测三坑**：扫描区间夹在
  被测控件自己的矩形里 / 比两来源先**对齐采样窗口** / "点了没反应"先怀疑**送达**。⚠⚠ **harness 两类
  抖动**：首帧布局竞态 → 连起重试到 `VP-RECT` 收敛；Win32 窗口级联 → **显式摆 (0,0) + 光标挪右下**。
- ⚠⚠ **像素断言别用"整图平均差"**：角色占屏 4% → Walk 的"站着 vs 跪着"也只有 **0.41/255**（阈值
  0.5 直接假红）。用 **显著变化像素占比**（`ImageChops.difference`→直方图，裁到 `VP-RECT`，阈值 8）：
  同样两对 0.1% / **2.48%**，基线 **0.000%**；判据 = 占比 > 0.5% **且 > 10× 基线**（自校准）。
  ⚠ 同 clip 钉两个时间不够（待机幅度小）→ **换 clip**。
- ⚠⚠ **Vulkan NDC 的 y 向下**（+1 在屏幕**底部**）：任何 2D/UI 像素 → NDC 都写 `y/H*2-1`
  （3D 的 `proj[1][1]*=-1` 已翻一次，2D 只剩**零次**）。写反 = 整层 UI **上下镜像**，
  而**日志全对**、只有截图看得出。⚠ 命中框输出尺寸必须与 PostProcess **同源**
  （`Renderer::uiOutputExtent()`）；焦点自测**必须用打乱的数组顺序**。
- ⚠⚠ **像素否定测试必须带"对照区"**：两次启动窗口偏 1~2px → 目标区差异冲到 **100%**，断言照样
  "通过"却只证明"两张图不一样"。判据 = 对照区 < 2% **且** 目标区 ≥ 10× 对照（2.219% vs 0.000%）。
- ⚠⚠ **`PostMessage` 方向键必须带 `KF_EXTENDED`（lParam bit24）**：不带 → GLFW 把 `0x50` 翻成
  **小键盘 2**，界面不动而日志**毫无痕迹**。⚠ "输入没反应"要**同时**断言"目标没动"和"输入确实
  被吃到了"（暂停按 W：位置逐位不变 **且** `UI2-FOCUS` 变了），否则"吞掉全部输入"也能通过。
- ⚠⚠ **删功能 = 同样删掉它的锚点和"等它的等待"**（否则留下永真空断言与永不到来的 `ready_needle`）。

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新增 `verify_*.py` 必须进 `DEFAULT` + 补
  `TRIGGERS`；⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`**（改表 + **同步自测 fixture**
  是原子操作）。铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**，横切名单**只以
  `GLOBAL_GLOBS` 为准**（**没有 `editor/**`**）。⚠ **新断言长在哪个文件上，那个文件就得进本用例的
  `TRIGGERS`**，否则改它时断言被静默跳过。
- **`verify_i18n_coverage.py`**（八组：A 参数 label/tooltip/特性名/Combo、B 未包 TR、B2 缺词条、
  C 孤儿+重复键、D snprintf 格式串、E **控件显示格式串**）：⚠⚠ **缺译文不报错不崩，只会静静少翻
  一处** → 三门盲区都实测栽过（裸 `snprintf("tasks %d")`、`SliderFloat(…,"%.1f deg")`、把 `TR` 漏在
  辅助函数外面）。⚠ **豁免白名单必须整串精确匹配**（用子串匹配过一次：`"%s"`/`"GB"` 把 42 条藏成
  11 条 = 假进度）。⚠ `kZh[]` 是 `emplace` = **先到先得**，重复键后者**永不生效** → 改词条先查重。
  ⚠ 一词两义只能靠**让 UI 原文更具体**（`Solid` → `Solid color`）。⚠ 状态串要在**显示时** TR
  （语言在构造**之后**才从存档/环境变量应用）。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 **`(PIL.Image, ok)` 元组**）；截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**；键盘输入必须 `PostMessage(WM_CHAR)`。
  ⚠ `MYVK_EDITOR_IMPORT` / `MYVK_EDITOR_CONSOLE_CMD` 都支持 **`;` 多值**（多实例沿 +X 排、名字自动
  `_1/_2`）；控制台 `anim.time <秒> [实体名]` / `anim.clip <名字> [实体名]`（缺省 = 当前选中）。
  ⚠⚠ **自检日志 `detail=` 里不能有空格**（脚本按 `\S*` 解析 → 整行失配 = 断言在脚本眼里"不存在"）。

## 路线图（权威 → `docs/*_roadmap.md`；已完成清单与自检数字 → 归档六）
- **渲染** R0~R4·R5-1·R6-1 ✅；**未做** R5-2/R5-3/R6-2~R6-5（= 运行时 G21~G25 硬前置）。⚠ 粒子
  自定义 PNG 未接线（draw set 只 6 个 binding、8 个 set 满）。
- **编辑器** B·C·E11.1·D·E25 ✅。**下一项** `E20`/`E26`。**运行时** M1+M2 ✅（依赖单向
  `runtime→engine←editor`；`engine/script/` 用 `vks`）。**物理** v1 ✅（仅 Play 态）。
- ⚠⚠ **角色链** `docs/character_pipeline_roadmap.md`：**A0 ✅（10-18，细则→日志）**，
  **下一项 A1 Animator**（→A2 状态机→A3 层→A4 根运动→A5 事件→A6 IK→A7 面板）。要点：唯一装配
  入口 `scene/ModelInstance` · `animation/{Pose,AnimatorSystem}` · `DrawItem::jointBase` 逐绘制项
  （多实例 0/21/42）· `skinned` 存档块。
- ⚠⚠ **游戏 UI/HUD** `docs/ui_hud_roadmap.md`：**U0~U8 全 ✅（10-18；`engine/ui2/` +
  `runtime/ui/`）**，下一项仅 U9（可选）。新用例 **`verify_hud.py`**（已进 DEFAULT/TRIGGERS）。
  三条实测铁律：①`maxBoundDescriptorSets` 是"同时绑定数"→ UI 独立 1 set 合法（`ship_roadmap`
  G15-5 错）；②⚠⚠ **subpass dependency 参与 pass 兼容性判定** → "一份 pipeline 两模式通用"**错**
  （2028 条 VUID-02684）→ `pipelineFor(VkRenderPass)` 按 pass 缓存、gameUi 回调带 pass；
  ③⚠⚠ 顶点 RGBA8 写 `R|G<<8|B<<16|A<<24`（小端反直觉）。UI 只插 **Post 输出 pass 内、ImGui
  之前**；sRGB → shader `srgbToLinear`（#808080→128，漏了 188）；`bear.x` 必须 `-pad`；
  PNG/JSON dims 不符**拒载**。⚠ 验收别用 `VP-RECT` 算坐标 → 用**差分包围盒**。
- ⚠⚠ **i18n 权威 `engine/i18n/ZhTable.cpp`**（1559 条）；`editor::tr` 只转发（792 处零改动）；
  英文模式 `tr()` 在查表**前** return → 零开销（`count=0`）。⚠ 搬词典必须**同步所有 `DICT_SRC`**
  + **重生成图集**，否则"词典全覆盖"静默失效。⚠ `MYVK_LANG` 是 `ALLOW_EXACT` 里的例外。
- **暂停**：权威是栈顶名（`topName()=="Paused"`，非 bool）；"按 W 不动"靠 `pushContext("Menu")`
  让 Gameplay **结构性**失效而非 if；迁移只在 `onUpdate` **末尾**；`tickUi` 挂 `cb.pollInput`
  （早于 fixedUpdate、不受 timeScale 影响）。细则（U6/U7/U8 全文）→ 归档十。
- ⚠ flaky：`verify_bvh`/`csm`/`ibl`/`particles` 偶发全红（单跑全 PASS）→ **别补跑"凑干净"**。
  ⚠ `FX-FREEZE` 日志 "bit-for-bit" 是**过度承诺** → 下次动 `engine/**` 顺手改。
```

---

## 2026-10-18 第三轮 MEMORY.md 压缩前全文（13.3 KB 版，加音频条目之前）

```
# venn —— 项目笔记（**软目标 ~12 KB**；上一轮 11.8 KB 实测完整注入。若发现**尾部缺失**，
先压「路线图」那段 —— 纪律在中间、路线图在最后）

> 只放 **纪律 + 速查 + 指针**。细则 → `editor_details_archive.md`（**压缩前全文都追加在这**）/
> `render_details_archive.md`；下一步 → `docs/*_roadmap.md`；设计理由 → `docs/editor_architecture.md`。
> 仓库 **`D:\A-VENN\code\cpp\venn-engin`**（CMake 名 `Venn`，刻意不一致）。C++17 手写 Vulkan，
> **右手系 Z-up**；`core→rhi→render→scene→assets`（另有 `ui`/`ecs`/`physics`/`script`；应用层
> `editor`/`runtime`，**engine/ 不认识后两者**）。改名后删 `build/` 重 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
快照**可能被截断/陈旧** → 跑回归 / 批量改文件 / 改契约前先读磁盘。（已因缺条目误启动过全量回归。）

## ⚠ 回归纪律（用户明确要求）
**不自动跑全量**：只有用户明确下达才 `run_regression.py --all`，日常只跑**受影响用例**（按
`TRIGGERS`）。⚠ `--explain` 无基准快照时**一律报全量**（保守兜底），不代表必须跑。
⚠ GUI 用例**同时只能有一个**（`kill_stale_editors` 无差别杀 `Editor.exe`/`Game.exe`）。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry`（`bindParam` 在成员默认初始化**之后**、`loadEnvOverrides()`**之前**）
③ **UI 面** ④ 序列化 ⑤ 用例 + `TRIGGERS`。**只有 `MYVK_*`、界面点不到的不算交付**。把关
`check_feature_surface.py`（C/D 清零）。⚠ `FeatureRegistry.*` 在 `GLOBAL_GLOBS`。

## ⚠ 本机环境
**GPU = Intel Iris Xe** → 3D 主通道同时绑 **8 个 set**，恰好 `maxBoundDescriptorSets=8` 卡在线上
（再加贴图槽得改架构；⚠ 但 UI 是独立管线只绑 1 个，**不受此限**）。Python 用
`...\envs\default\Scripts\python.exe`（numpy+pillow）；⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（细则 → 归档五）
入口 **`./make.cmd build`**（`TOOLCHAIN=auto` → **MSVC**）。⚠ **四坑**：`https_proxy`+`HTTPS_PROXY`
并存 → 假报 `MSB6001`；UTF-8 无 BOM + 中文 → `C2001`（要 `/utf-8`）；`.cmd`/`.bat` **纯 ASCII**；
Make 用 sh 跑配方 → `echo` 里不能有 `(` `)` `|`。⚠ 资产拷贝必须 `add_custom_target(... ALL)`
（`POST_BUILD` 只在重链时跑）。⚠ `strings` 抓不到 ASCII 常量 → 只能靠**行为**验代码进没进二进制。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP="near,far"`（默认 `0.1/5000`）；
编辑器侧一律 `MYVK_EDITOR_` 前缀（白名单按前缀放行）。⚠ **落盘读/写不对称**：自动化下默认不读、
显式 `MYVK_EDITOR_LAYOUT` 照读、**写一律拒绝**。⚠⚠ **改"全局默认"要 grep 依赖它的测试**（远平面
200→5000 让基线整体漂；`kPrefsVersion` 升 2 后 fixture 还写 1 → 存档被丢弃）。**修法是钉输入 /
升 fixture，不是改期望值。**

## 契约速查
- **Pass 序**：`Shadow → LocalShadow → DepthPrePass → SSAO → ClusterBuild → VolumetricFog(×2) →
  Forward → PostProcess → 交换链`。**compute 不能在 RP 内执行**；**栅格夹在不透明与透明之间**、
  **绝不能**做成 ImGui 叠加层。
- **"改一处要同步改 N 处"是常态**：`FrameUBO`(768B)/推常量(128B 全满)/set0(25 binding)/变体位序/
  分簇编址/材质贴图 set(7)/旁路管线 range 须与主管线**逐字段相同**（否则 VUID-08600）。
- ⚠ 两处**默认槽**是雷：DBuffer 缺省必须 `m_defaultTransparent`；探针 binding 23 缺省必须绑 1×1
  全黑 cube array，**不能"不绑"**。**颜色空间**：albedo/emissive `srgb=true`，normal/ORM/高度图
  `false`，着色器内不做手动 gamma。
- **动画/蒙皮**：`jointBase` 三路来源（MDI `aux.x` / 合批与基线 `push.skin.x`）；`invMeshNode` 与
  `push.model` 里的 meshNodeWorld **约掉** → 静态与蒙皮共用同一条 draw。⚠ **采样不许留在 Renderer
  里**（否则无窗口测不了）→ `docs/character_pipeline_roadmap.md` §3.3。

## 编辑器架构（细则 → 归档一/二/三，全文 → 归档九）
- **加面板 = 表里加一行**（`panelTableVec()` + `PanelRegistry` → 菜单/快捷键/图标/落盘四路派生）；
  面板间**只准依赖 `EditorContext`**，动引擎走 `EditorHost` 纯虚接口。开关权威在 `PanelRegistry.cpp`
  的 `g_open[kMaxPanels]`（**固定数组 + 引用**，vector 扩容 → `bool&` 悬垂）。
- ⚠ **默认一律"全关"**（`config/render.json` 缺失 → **逐像素等于今天**）。⚠ **快捷键排他**（面板
  `Alt+1..8` / 工作区 `Ctrl+Alt+1..6` / 视图键 `Ctrl+1..8` / `Shift+Space` 在 `Space` 前）；
  **别引入 ImGui docking**（用自研 `SplitLayout`）。
- ⚠ **Inspector 11 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**枚举值 = 图标栏顺序**；
  **落盘写 id 不写下标**。⚠ **`All` 不是融合视图**，是旧堆叠顺序一字不改（9 个脚本靠 `INS-*`）→
  自动化 `All`、交互 `Object`，**别"优化"它**。⚠⚠ **记在"提前返回"之前的锚点只证明函数被调用**，
  **负向断言只能用返回之后的锚点**；图标改完要放大截图肉眼看。
- ⚠⚠ **布局 / 遮挡 / 托盘**：Content 是底栏弹出的**抽屉**（`kSlotContent` 恒 `false`，保留 case）；
  分隔条靠 `m_occluders` + `collectSplitOccluders()`（排在所有窗口画完之后；**必须排除落在
  `LayoutRects` 上的布局面板**，否则整条被裁成 0）；"最小化" = `panelTrayHiddenRef(i)=1`，**自己
  无条件绘制的窗口（如 `CharacterWindow`）也必须认这个位**；托盘锚点 `BAR-TRAY-<panelId>`；
  **标题栏 × = 收进底栏**。⚠ 树里"随选中出现/消失的一行"会让整棵树跳行；Hierarchy 列头必须手画，
  截断在 **UTF-8 字符边界**二分。⚠⚠ **粒子**：单发射器只能画直线/矩形（出生体轴对齐长方体 +
  速度只有锥+恒定加速度）；分叉/辉光必须**多发射器**；倾斜**只能用 `tc->rotation`**。

## 断言纪律（细则 → 归档四/九/十）
- ⚠ 必须 `MYVK_EDITOR_NO_HUD=1` + `MYVK_GRID=0`；**采样区落在被测物体上**；抓图走 `capture_nonempty`；
  **面板会盖住被测物** → 先收进托盘再采样。
- ⚠ **"状态一致"与"像素一致"是两个断言**；**新断言必须做否定测试**；**临时诊断代码用完必须清**。
  ⚠ **FAIL 先怀疑「期望」再怀疑「实现」**；**兜底条件必须与放行条件同源**；峰值类指标的容差不能比
  瞬时值更紧。⚠ 锚点拼错 = **断言永远为假**（静默 None）→ 改完必须跑一遍看它变绿；`wait_rect` 读
  "最后一条"，窗口收起后读到**旧值** → 假通过。
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取最后一条）—— 已踩 **五次**。
- ⚠ 要截图的用例**别用 `MYVK_FRAMES` 控帧** → 等条件出现 + 主动关窗。⚠ **量测三坑**：扫描区间夹在
  被测控件自己的矩形里 / 比两来源先**对齐采样窗口** / "点了没反应"先怀疑**送达**。⚠⚠ **harness 两类
  抖动**：首帧布局竞态 → 连起重试到 `VP-RECT` 收敛；Win32 窗口级联 → **显式摆 (0,0) + 光标挪右下**。
- ⚠⚠ **像素断言别用"整图平均差"**：角色占屏 4% → Walk 的"站着 vs 跪着"也只有 **0.41/255**（阈值
  0.5 直接假红）。用 **显著变化像素占比**（`ImageChops.difference`→直方图，裁到 `VP-RECT`，阈值 8）：
  判据 = 占比 > 0.5% **且 > 10× 基线**（自校准）。⚠ 同 clip 钉两个时间不够 → **换 clip**。
  ⚠⚠ **否定测试必须带"对照区"**：两次启动窗口偏 1~2px → 目标区差异冲到 **100%**，断言照样"通过"
  却只证明"两张图不一样"（实测 2.219% vs 对照 0.000%）。
- ⚠⚠ **Vulkan NDC 的 y 向下**（+1 在屏幕**底部**）：任何 2D/UI 像素 → NDC 都写 `y/H*2-1`
  （3D 的 `proj[1][1]*=-1` 已翻一次，2D 只剩**零次**）。写反 = 整层 UI **上下镜像**，而**日志全对**、
  只有截图看得出。⚠ 命中框输出尺寸必须与 PostProcess **同源**（`Renderer::uiOutputExtent()`）；
  焦点自测**必须用打乱的数组顺序**（单列里"几何最近"≡"数组下一个" = 最易假绿）。
- ⚠⚠ **`PostMessage` 方向键必须带 `KF_EXTENDED`（lParam bit24）**：不带 → GLFW 把 `0x50` 翻成
  **小键盘 2**，界面不动而日志**毫无痕迹**。⚠ "输入没反应"要**同时**断言"目标没动"和"输入确实
  被吃到了"（暂停按 W：位置逐位不变 **且** `UI2-FOCUS` 变了），否则"吞掉全部输入"也能通过。
- ⚠⚠ **删功能 = 同样删掉它的锚点和"等它的等待"**（否则留下永真空断言与永不到来的 `ready_needle`）。

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新增 `verify_*.py` 必须进 `DEFAULT` + 补
  `TRIGGERS`；⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`**（改表 + **同步自测 fixture**
  是原子操作）。铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**，横切名单**只以
  `GLOBAL_GLOBS` 为准**（**没有 `editor/**`**）。⚠ **新断言长在哪个文件上，那个文件就得进本用例的
  `TRIGGERS`**，否则改它时断言被静默跳过。
- **`verify_i18n_coverage.py`**（八组：A 参数 label/tooltip/特性名/Combo、B 未包 TR、B2 缺词条、
  C 孤儿+重复键、D snprintf 格式串、E **控件显示格式串**）：⚠⚠ **缺译文不报错不崩，只会静静少翻
  一处**（裸 `snprintf("tasks %d")`、`SliderFloat(…,"%.1f deg")`、`TR` 漏在辅助函数外，三门盲区都
  实测栽过）。⚠ **豁免白名单必须整串精确匹配**（子串匹配过一次：`"%s"`/`"GB"` 把 42 条藏成 11 条）。
  ⚠ `kZh[]` 是 `emplace` = **先到先得**，重复键后者**永不生效** → 改词条先查重。⚠ 一词两义只能靠
  **让 UI 原文更具体**（`Solid` → `Solid color`）。⚠ 状态串要在**显示时** TR（语言在构造**之后**才
  应用）。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 **`(PIL.Image, ok)` 元组**）；截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**；键盘输入必须 `PostMessage(WM_CHAR)`。
  ⚠ `MYVK_EDITOR_IMPORT`/`MYVK_EDITOR_CONSOLE_CMD` 支持 **`;` 多值**；控制台 `anim.time <秒> [实体]`
  / `anim.clip <名> [实体]`（缺省 = 当前选中）。⚠⚠ **自检日志 `detail=` 里不能有空格**。

## 路线图（权威 → `docs/*_roadmap.md`；已完成清单与自检数字 → 归档六）
- **渲染** R0~R4·R5-1·R6-1 ✅；**未做** R5-2/R5-3/R6-2~R6-5（= 运行时 G21~G25 硬前置）。⚠ 粒子
  自定义 PNG 未接线（draw set 只 6 个 binding）。
- **编辑器** B·C·E11.1·D·E25 ✅。**下一项** `E20`/`E26`。**运行时** M1+M2 ✅（依赖单向
  `runtime→engine←editor`；`engine/script/` 用 `vks`）。**物理** v1 ✅（仅 Play 态）。
- ⚠⚠ **角色链** `docs/character_pipeline_roadmap.md`：**A0 ✅**，**下一项 A1 Animator**（→A2 状态机
  →A3 层→A4 根运动→A5 事件→A6 IK→A7 面板）。要点：唯一装配入口 `scene/ModelInstance` ·
  `animation/{Pose,AnimatorSystem}` · `DrawItem::jointBase` 逐绘制项（多实例 0/21/42）· `skinned` 存档块。
- ⚠⚠ **游戏 UI/HUD** `docs/ui_hud_roadmap.md`：**U0~U8 全 ✅（10-18；`engine/ui2/` + `runtime/ui/`）**，
  仅剩可选 U9。新用例 **`verify_hud.py`**（已进 DEFAULT/TRIGGERS）。三条铁律：①`maxBoundDescriptorSets`
  是"同时绑定数"→ UI 独立 1 set 合法（`ship_roadmap` G15-5 错）；②⚠⚠ **subpass dependency 参与 pass
  兼容性判定** → "一份 pipeline 两模式通用"**错**（2028 条 VUID-02684）→ `pipelineFor(RenderPass)`
  按 pass 缓存；③⚠⚠ 顶点 RGBA8 写 `R|G<<8|B<<16|A<<24`（小端反直觉）。UI 只插 **Post 输出 pass 内、
  ImGui 之前**；sRGB → `srgbToLinear`；`bear.x` 必须 `-pad`。
- ⚠⚠ **i18n 权威 `engine/i18n/ZhTable.cpp`**（1559 条）；`editor::tr` 只转发（792 处零改动）；
  英文模式 `tr()` 在查表**前** return → 零开销（`I18N-KEY count=0`）。⚠ 搬词典必须**同步所有
  `DICT_SRC`** + **重生成图集**，否则"词典全覆盖"静默失效。⚠ `MYVK_LANG` 是 `ALLOW_EXACT` 里的例外。
- **暂停**：权威是栈顶名（`topName()=="Paused"`，非 bool）；"按 W 不动"靠 `pushContext("Menu")` 让
  Gameplay **结构性**失效而非 if；迁移只在 `onUpdate` **末尾**；`tickUi` 挂 `cb.pollInput`（早于
  fixedUpdate、不受 timeScale 影响）。细则（U6/U7/U8 全文）→ 归档十。
- ⚠ flaky：`verify_bvh`/`csm`/`ibl`/`particles` 偶发全红（单跑全 PASS）→ **别补跑"凑干净"**。
  ⚠ `FX-FREEZE` 日志 "bit-for-bit" 是**过度承诺** → 下次动 `engine/**` 顺手改。
```

## 十一、2026-10-26 蒸馏：MEMORY.md 压缩下来的原文

> MEMORY.md 到 18.2 KB（约定 ~13 KB，超了会被整段截断注入）→ 把「构建」「契约速查」
> 「断言纪律」「资产链」「路线图」里已经稳定成习惯的细则压成判据留在 MEMORY.md，
> 全文留在这里。查具体背景时来这里翻。

### A. 构建（原文）
入口 **`./make.cmd build`**（`TOOLCHAIN=auto` → **MSVC**，`Debug` → **性能数字是 Debug 的**）。
⚠ **五坑**：`https_proxy`+`HTTPS_PROXY` 并存 → 假报 `MSB6001`；UTF-8 无 BOM + 中文 → `C2001`
（要 `/utf-8`）；`.cmd`/`.bat` **纯 ASCII**；Make 用 sh 跑配方 → `echo` 里不能有 `(` `)` `|`；
**struct/class 前向声明必须与定义一致**（MSVC 修饰名 `AEBU`/`AEBV` 按 TU 内**首次**声明决定
→ 两 TU 各看一个就 `LNK2019`；**C4099 是前兆**，增量构建会掩护、全量重编才炸；
链接错先 `dumpbin //symbols`）。
⚠ 资产拷贝必须 `add_custom_target(... ALL)`（`POST_BUILD` **只在重链后**跑），**多个 target 拷同一
目录会并发冲突** → 全局单例 target。增量构建报 `LNK1163` → 删那个 `.obj`，不是代码错。
⚠ **新加资产文件不会自动进 `build/bin/`** → 首次跑用例必 `RAMP-MISSING`/`X-MISSING`，
「新增资产却"完全没效果"」先查这个（10-26 实测浪费一轮：诊断色带没拷过去，白测两次）。

**10-26 实锤补记：C4099 又中一次** —— `engine/scene/SceneSerializer.h` 写 `class Material;`，
而 `assets/Material.h` 是 `struct Material` → 已改 `struct`。**加任何前向声明前先 grep 定义**；
`grep -rn "^class \|^struct " | grep <TypeName>` 一行就能对出来。

### B. 契约速查（原文）
- **Pass 序**：`Shadow → LocalShadow → DepthPrePass → SSAO → ClusterBuild → VolumetricFog(×2) →
  Forward → PostProcess → 交换链`。**compute 不能在 RP 内执行**；**栅格夹在不透明与透明之间**、
  **绝不能**做成 ImGui 叠加层。
- **"改一处要同步改 N 处"是常态**：`FrameUBO`(768B)/推常量(128B 全满)/set0(25 binding)/分簇编址/
  旁路管线 range 须与主管线**逐字段相同**（否则 VUID-08600）。
- ⚠ 两处**默认槽**是雷：DBuffer 缺省必须 `m_defaultTransparent`；探针 binding 23 缺省必须绑 1×1
  全黑 cube array，**不能"不绑"**。**颜色空间**：albedo/emissive `srgb=true`，normal/ORM/高度图
  `false`，着色器内不做手动 gamma。
- ⚠⚠ **材质是共享资产，不是逐物体**（按名缓存）。"逐物体"是**假象**，靠"每实体唯一材质名"维持。
  三个实测 bug：①**同名静默合并**；②**身份是数组下标**（重导出即错位）；③**导入模型的材质覆盖存盘即丢**。
- **动画/蒙皮**：`jointBase` 三路（MDI `aux.x` / 合批与基线 `push.skin.x`）；`invMeshNode` 与
  `push.model` 约掉 → 静态与蒙皮同一条 draw。⚠ **采样不许留在 Renderer 里**。
- ⚠⚠ **水体没有网格** → 不在绘制列表/BVH；`WaterPass` 只看 `WaterBody` 不看实体 Transform →
  W/E/R 靠"Transform 当手柄+每帧成对镜像"；**Water 不能算进 `brushMode`**（否则手柄整段关掉）。
- **出生点 = `GameStartComponent`**（仿 PlayerStart）：位置走实体 transform，**不给实体挂网格**
  （叠加层画 Q 版小人，锚点 `GAMESTART-MARK`）；模板场景 = **地形 + GameStart**，地形资产与
  材质**随模板自带**。⚠ 无网格 → 点选/落地单独开口；层贴图路径必须过 `resolveAssetPath`。
  地面"糊/重影"先查**材质有没有贴图**，不是 TAA。

### C. 断言纪律（原文全文）
- ⚠ 必须 `MYVK_EDITOR_NO_HUD=1` + `MYVK_GRID=0`；**采样区落在被测物体上**；面板会盖住被测物
  → 先收进托盘（被裁控件矩形照样有值，但点下去点到抽屉）。
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号）—— 已踩 **五次**。
- ⚠ "状态一致"≠"像素一致"；**新断言必须做否定测试**且**带"对照区"**；
  ⚠⚠ **落点必须是"两种实现会不一样的那个量"**（A1 `animator_paused` 打在镜像值 `comp(f).time`
  才有效）；**临时诊断代码用完必须清**；**FAIL 先怀疑「期望」再怀疑「实现」**。
  ⚠ 锚点拼错 = 断言永远为假；`wait_rect` 读"最后一条"→ 窗口收起读到旧值 → 假通过。
- ⚠⚠ **`has_line` 证明不了"点了有反应"** → `wait_count(needle, before+1)`，**计数在点击之前取**；
  节流日志要轮询。
- ⚠⚠ **像素断言别用"整图平均差"** → 用**显著变化像素占比**（裁 `VP-RECT`、阈值 8）：
  > 0.5% 且 > 10× 基线；控制上限 0.1~0.3%（TAA 抖动）不能写 0。
  四陷阱：①钉"主色是谁"不是"某色占多少"；②**代理量会饱和**（pom 平均差 45° 比 80° 大）；
  ③**绝对容差不能用在计数型瞬时量** → `max(绝对值, 5%)`；④"某状态色占 N%"会把悬停判成 bug。
- ⚠⚠ **要截图的用例别用 `MYVK_FRAMES`**；`Game.exe` 认 argv `--frames`、`Editor.exe` 只认
  `MYVK_FRAMES`，传错无提示渲染到超时。
- ⚠⚠ `PostMessage` 方向键必须带 `KF_EXTENDED`；`ctypes` 必须给 `PostMessageW` 等**声明
  `argtypes`**（64 位 HWND 截断 → 静默失败）。
- ⚠⚠ **Vulkan NDC 的 y 向下**：2D/UI 像素 → NDC 写 `y/H*2-1`（3D 已翻过一次）。写反 =
  整层 UI 上下镜像、日志全对、只有截图看得出。
- ⚠⚠ 矩形锚点 tag 字符类必须含数字；**删功能 = 同时删它的锚点和"等它的等待"**。
- ⚠⚠ harness 抖动：首帧布局竞态 → 连起重试到 `VP-RECT` 收敛；**真实点击要求窗口在最前**
  （`click()` 前 `focus_window()`）；`logRect` 是客户区坐标、`SetCursorPos` 要屏幕坐标 →
  走 `verify_asset_drag.move_to(hwnd,…)`；`MYVK_EDITOR_NO_HUD` **会连带关掉变换手柄**
  → 拖手柄的用例 no_hud=False。其余 → 归档四/九。
- ⚠⚠ **`Popen(stdout=PIPE)` 必须有人排空**（64 KB 满即死锁 → "窗口不出现"）。
- ⚠⚠ **`os.remove`/`shutil.rmtree` 会被宿主按回合累计硬杀**（阈值 50 文件、进程层终止）→
  一律用 **`tools/safe_fs.py`**；让位必须进**同级 `.trash/` 子目录**不能原地改名。
  `fresh_dir()` 删不掉**绝不降级复用**。

### D. 资产链 + 外部模型 → 角色（原文）
- ⚠⚠ **新增顶层目录必须进 `WATCH_ROOTS`**；⚠⚠ **"写了容错分支" ≠ "容错生效"**（上游会
  `throw` 的分支跑不到 → 贴图缺失必须走 `tryLoadTexture()`）；造"缺贴图"现场要改路径下标 +
  **重算 MAT CRC32**。
- 一条命令：`AssetC --strip-skin --autorig --procanim in.vmesh -o out.vmesh`；运行时用
  `assets/game.json` 的 `player` 块或 `MYVK_RUNTIME_PLAYER_MODEL/_CLIP`。
- **世界 = 米 / Z-up**，模型 pivot 在**几何中心**、玩家位置语义是**脚** → 挂模型必须
  `lift = -boundsMin.z`。玩家实体必须挂 **`CameraComponent`**（否则镜头钉住）。
- `ProcAnim`：关键姿势表 + smoothstep（**不要正弦叠加** = 驴踢）；判"倒着走"用**摆动相平均前向速度**。
- ⚠⚠ **`desc.maxSpeed` 会截断冲刺** → 物理层上限留冲刺余量（`physicsMaxSpeed()`）。
- ⚠⚠ **`PostMessage` 合成的 Shift 会被 GLFW 当场撤销** → **修饰键必须 `keybd_event` +
  先 `SetForegroundWindow`**；测试开头清修饰键残留。
- ⚠⚠ **`.vmesh` 传给 `loadFBX` → 第二次起永远失败**（`.vmeta` 判过期回退解析 .vmesh 抛异常；
  "第一次正常，删 .vmeta 又好一次"）。`MeshLoader` 已加"情况 0"短路；重烤后查 `.vmeta` 残留。
- **P6 键位/跳跃/贴地（10-22，细则 → `docs/player_controls.md`）**：出厂 WASD/右键跑/空格跳
  （`InputMap`，**存档优先** → 老工程要点"恢复默认"）；跳跃 v₀ 3.70/0.70 m/0.754 s、`airControl 0.45`
  （编辑器 Play 手写同一条抛物线，**改一处改两处**）；贴地用**扫掠窗口** + `GAME-PLAYER-RESCUE` 兜底。

### E. 路线图（原文）
- 完成：启动器 M0~M6(缺 M5-c)·世界 W0~W4·渲染 R0~R4/R5-1/R6-1·编辑器 B/C/D/E11.1/E25/E26·
  运行时 M1+M2·物理 v1·音频·UI/HUD U0~U8·脚本 S0~S6·资产链六/七·**外部模型→角色
  （1:1/AutoRig/ProcAnim，10-22）·A1 Animator·P4~P6 角色操控全套（10-22）·
  二次元着色 A1/A4/A5/A6（10-26）**。
- 下一项：**着色 A3 眼睛**（A2 法线平面化与 A1 二选一，A1 已落地 → 降级；A7/A8 打磨留到
  有真实角色资产之后）·**A2/A3 状态机与控制器资产**（这是 `character_pipeline_roadmap`
  的 A2/A3，**与着色 A2/A3 同名不同事**）·世界 **W5 植被**·编辑器 E20/E22·**G16-1**。
  ⚠ **anim/sk 双份字段的权威规则**见 `roadmaps.md`。
- ⚠ flaky：`bvh`/`csm`/`ibl` 偶发全红（单跑 PASS）→ **别补跑"凑干净"**。

## 十二、2026-10-26（C1 落地那轮）：A/B/C1 三节的原文

> 为了把 MEMORY.md 压回 ~13 KB，把「着色 A1/A3/A4/A5/A6 + 动画 B1/B2/B3」
> 与「过场时间轴 C1」两节的**完整表述**搬到这里，MEMORY.md 只留判据。

## 着色 A1/A3/A4/A5/A6 ＋ 动画 B1/B2/B3（10-26 ✅）
权威 → `docs/anime_gap_review_2026-10-26.md`；使用者文档 → `docs/character_animation_guide.md`。
本页只留**下轮立刻会再踩**的：
- ⚠⚠ **A 类槽（ramp/matcap/SDF/detail/aniso/eye）+ 角色光只在 `VENN_BINDLESS` 编译单元里存在**，
  而 `MYVK_BINDLESS` **默认关** → 不开就是"全部静默失效、零报错"。**A 类槽验收必须带它**。⚠ 色带
  `u=0` 被 REPEAT 回绕 → 采样前夹到首末纹素中心；**"白 ramp = 恒等"在 A6 后不成立**（落点
  `params.x = -1`）；材质页 **112 B / 7 块**。
- ⚠⚠ **"某特性不吃 X"必须用对照材质自证响应度**；**诊断顺序：先确认被测物在画面里，再谈它动没动**
  （相机 **`yaw = 180° − φ`**；A3 写反 → 根本没拍到目标，"0 px"看着像没接线）。⚠ A4 `shiftTangent`
  必须是 `normalize(T + N*shift)`（先投影 = 死参数且画面逐像素相同）；⚠ **"暗档 ≠ 更暗"**。
- ⚠ 硬约束：B1 遮罩 = **根骨+父链子树**、**0 命中 = 该层不生效**（不是全部），顺序 底座 → 附加层 →
  `applyPerformance` → `poseToJoints`，`dt==0` 视为淡完；B2 区间 **`[prev,new)` 半开**、`t == duration`
  **永不触发**、换 clip **必须**归零 `eventPrev`、表**按 clip 名查找**（两层同 clip 发两遍）。
- ⚠⚠ **B3 剥离 = "钳回基准"，不是"减本帧增量"**：每帧重新采样，减增量只让根**滞后一帧**（模型走
  0.96 + 实体走 0.98 = 双倍位移），而**每帧日志都对**。否证：base 跟着走 → `rm_in_place` 报
  `p1=0.0167 p60=0.9666`。
- ⚠⚠ **B3 两个"不要提取"的时刻**：① **跨圈那一帧**（根骨瞬回原点，增量是负的 → 不丢就每步被拉回
  原地）② **`dt==0`**（拖时间轴 = 任意跳帧）。第一帧只记基准不提（骨盆离地 1 m，否则一进 Play
  窜天）。⚠ `n` **不能用逐位比较**（原地动画 dist 有 1e-6 抖动 → `n=1 dist=0.0000`）。
- ⚠⚠ **glTF 与 FBX 的 `joints` 不在同一坐标系**（glTF = Y-up 藏在 `invMeshNode`；FBX 经 ufbx 已
  Z-up）→ 位移写 `TransformComponent` 前必须过 `Skeleton::skeletonToEngine`（各 loader 自填）。不转
  = 角色**朝天上飘**且俯视不动。⚠ yaw 用 **swing-twist**，不用 `atan2(right.y, right.x)`。
- ⚠⚠ **手写的 `skinned` 实体在视口里一个像素都不画**（渲染的是每个 submesh 的子实体）→ 验收必须先
  `MYVK_EDITOR_IMPORT` + `scene.save` 烤模板。⚠ `anime_girl.fbx` 的 **`Idle` 段数据是坏的** → 用
  `Walk` 靠相位区分；四段**全是原地动画** → B3 端到端只能验接线。

## 过场时间轴 C1（10-26 ✅；权威 → `docs/cutscene_sequencer.md`）
- `sequence/SequenceAsset.*`（**纯函数**解析/采样/收集）+ `SequencerSystem.*`（播放器）+
  `SequencerComponent` + Inspector **第 12 页签 `sequence`**；`assets/sequences/*.seq.json`。
  数值 48 条（`--selftest sequence`）+ 端到端 8 组（`verify_sequencer.py`），已进 `DEFAULT`。
- ⚠⚠ **播放器不持有 `CameraSystem*`，只产出 `pose()`** —— 第一版传指针，运行时正常、**编辑器
  完全不能用**（视口是自由相机、不经过 `CameraSystem`）。运行时 `setDesiredPose`、编辑器
  `applyPoseToSceneCamera`。⚠ 顺序：`tick` 在 `CameraSystem::update` **之前**、玩家控制**之后**。
- ⚠⚠ **播完必须 `hasPose()==false`**（否则相机冻在结尾，症状="人物在动、画面不动"）；
  **`Ease::Step` 是"选择"不是"lerp"**（段内保持前帧）；**yaw 走最短弧**（否则原地抡一圈）；
  **关键帧按 `t` 排序**；**事件半开且跨圈扫两段**（只用 `[prev,next)` 在回绕时 from>to → 一条不发）；
  **首帧 `prev = playhead - eps`** 否则 t=0 的事件永不发；`startTime>0` **不补发**；每帧夹 `0.10 s`。
- ⚠ 编辑态 `dt==0` **要应用姿态、不推进不发事件**（与 B3「`dt==0` 不提取根运动」**正好相反**）。

---

## 十三、2026-10-27（G16 稳定身份 1~4 落地）：判据原文

> MEMORY.md 只留三条，这里是**完整版**（含每条断言的落点与否定测试结果）。
> 权威文档：`docs/persistence_roadmap.md` §3（设计）、§12.1（落地记录）。

### 13.1 数据模型（已落地）

```cpp
// ecs/Entity.h
using EntityGuid = std::uint64_t;
constexpr EntityGuid kInvalidGuid = 0;   // 0 = 无身份，不参与存档
std::string  guidToHex(EntityGuid);      // 16 位小写，**必须补前导零**
EntityGuid   parseGuidHex(const std::string&);  // 严格 16 位；不抛、不吃 0x、不吃尾随垃圾

// ecs/World.h
struct Record { uint32_t generation; bool alive; std::string name; EntityGuid guid; };
std::unordered_map<EntityGuid, EntityId> m_byGuid;   // 反查表，只增删不重建
EntityGuid guid(Entity) const;  Entity find(EntityGuid) const;
bool setGuid(Entity, EntityGuid);  EntityGuid allocateGuid();
```

**分配**：进程级 `std::mt19937_64`（`seed_seq{rd(),rd(),rd(),rd(),steady_clock}`）。
⚠ 不能放进 `World`：各持一份会让"清空再加载"重放同一串 → 场景合并交集非空。

**生命周期三处**（顺序都是硬依赖）：
| 位置 | 动作 | 写反的后果 |
|---|---|---|
| `create` | 在 `emplace_back` **之后**赋 `r.guid = allocateGuid()`，再 `m_byGuid[g]=id` | 读到默认 0 |
| `destroy` | **先** `m_byGuid.erase(r.guid)`，**再** `r.guid = 0` | 拿 0 去 erase → 旧键留表里 → 槽位复用后**旧 guid 复活** |
| `clear` | `m_byGuid.clear()` | 跨场景串号 |

### 13.2 场景 JSON（v1 → v2）

- 写端：`guid` 紧跟 `name`（**别插在 `parent` 后面** —— 读文件的人第一眼该看到"这是谁"）；
  `parentGuid` **只在父不在本次导出集合里**时写（Prefab 子树根 / G19 分块边界）。
- 读端：第一遍建实体时就 `setGuid` 覆盖（`create` 已给随机值）；
  `want==0` → `assigned++`；`setGuid` 失败 → `dup++` + `SCENE-GUID-DUP`。
  父解析：有 `parentGuid` 走 `find(guid)`（失败只 warn，不 fail 整份 —— 父可能在还没流送的块里）；
  否则走 `parent` 下标，且**必须 `is_number_integer()` 先判**（`value("parent",-1)` 遇字符串抛 302）。
- 版本：`kFormatVersion = 2`；读端只拒绝 **> 2**，**1 照读**（否则仓库里所有既有场景打不开）。
  ⚠ 校验必须在 `scene.clear()` **之前**。
- 锚点：一次加载一条 `SCENE-GUID assigned=%d dup=%d total=%d path=%s`（不是每实体一条）。

### 13.3 选择接回（G16-4）

逻辑抽成 `Scene::reconnectByGuidOrName(guid, name)`（**guid 优先、名字兜底**），
`EditorContext::validateSelection` 调它。`m_selectionGuid` 与 `m_selectionName` 同时抓、同时刷新。
⚠ 集合成员（多选）**仍是名字兜底**，没改成 guid —— 这是已知的剩余缺口。

### 13.4 验收 26 条（`Game.exe --selftest guid`）

| 组 | 断言 |
|---|---|
| 分配 | `guid_unique_100k` · `guid_never_zero` |
| 反查 | `guid_find_roundtrip`（1000）· `guid_find_zero_empty` · `guid_invalid_entity_zero` |
| 冲突 | `guid_set_duplicate_rejected` · `guid_set_self_ok`（**自己占自己的键要成功**，钉 `it->second != e.id`） |
| 幽灵 | `guid_no_ghost_after_set` · `guid_set_chain_no_ghost`（**期望 1**，漏 erase 是 100） |
| 生命周期 | `guid_destroy_unmaps`（含 `old=REVIVED` 判据）· `guid_reuse_not_recycled` · `guid_clear_unmaps` |
| 跨 World | `guid_two_worlds_disjoint` |
| hex | `guid_hex_leading_zeros` · `guid_hex_case_insensitive` · `guid_hex_rejects_garbage` |
| 场景 | `scene_guid_roundtrip` · `scene_guid_stable_twice`（逐字节）· `scene_guid_assigned` · `scene_guid_dup_reassign` · `scene_merge_no_collision` · `scene_version_too_new`（含 `kept=1`）· `scene_parent_guid_miss_safe` |
| 选择 | `selection_reconnect_by_guid` · `selection_fixture_has_ambiguity`（**对照区**：证明夹具真造出了歧义）· `selection_reconnect_no_hint_empty` |

端到端 `tools/verify_guid.py` 8 组：A 自检全绿 · B v1 补发 dup=0 · C v2 `assigned=0`
（**唯一能证明"读端真读了文件里的值"的一条**）· C2 无 skipped · C3 大写 hex · D dup=1 + 锚点。

### 13.5 否定测试（4 次，都实测到红 → 还原 → 绿）

1. 去 `setGuid` 的 erase → `old=ghost` + `hits=100`。
2. `destroy` 顺序写反 → `old=REVIVED`。
3. 写端不写 guid → `roundtrip same=0` 红；**`stable_twice` 仍绿**（两边都不写，长度一样）
   → 说明它抓的是**读端重算**。
4. 读端不 `setGuid` → 两条都红，且 `len=5082/5082` **相同**、内容不同
   → **只判长度会漏，逐字节判据是必要的**。

### 13.6 夹具的坑（最容易再踩）

**只挂 Transform/Name 的实体不进场景文件**（`hasNonMeshContent` 名单不含它们）→
第一次跑 `scene_guid_roundtrip` 拿到 `n=500` 字节、`same=0`：10 个实体全被当空实体跳过，
"往返"比的是**两份空文件**，而 `stable_twice` 还 PASS。修法是给夹具加 `folder` 标记
（`h->group = true`，最轻、不引资产）。修好后 `n=5082`。

---

## 十四、2026-10-27（G16-5 序列化器三段拆分）：判据原文

### 14.1 三段的**签名**就是判据

```cpp
// engine/scene/SceneDocument.h
bool parseSceneDocument(const nlohmann::json& root, SceneDocument& out, std::string& err);
void instantiateDocument(Scene&, const SceneDocument&,
                         std::vector<ecs::Entity>& created,
                         std::unordered_map<ecs::EntityGuid, ecs::Entity>& guidMap,
                         DocumentStats& stats);
void resolveDocumentAssets(Scene&, assets::AssetManager&, const SceneDocument&,
                           const std::vector<ecs::Entity>& created, DocumentStats& stats);
```

* ①② **不许**出现 `AssetManager` —— 出现就等于把"后台线程解析"这条路焊死了。
  `tools/verify_save.py` D 组用**纯静态**文本检查盯这一条（不用编译），否定测试实测能抓到。
* 实现留在 `SceneSerializer.cpp`（不是 `.cpp` 分开）：`applyCamera` / `physicsFromJson` /
  `entityApplyComponentsImpl` 全在匿名 namespace 里，另起 TU 只能复制一份。
* 定义必须在匿名 namespace **外**（头文件声明是外部链接，里面定义就是内部链接 → 链接期未定义引用）。

### 14.2 三处"看着更好其实更糟"的设计（都是刻意不做）

| 放弃了 | 改成 | 理由 |
|---|---|---|
| `DocumentEntity` 每组件一个 `optional` | 只留 `raw`（原始 JSON 块） | 搬组件就要搬每个 `xxxFromJson`，其中十几个要解析资产 → ③ 段变成第二份实现，加组件必漏改 |
| 单独 `AssetRequest` 表 | ③ 段直接遍历实体 | 拆它等于把 `entityApplyComponents` 按组件再拆一遍；等 G19 真要"资产预取清单"再生成 |
| `documentToJson` 反向 | 未做 | 目前没消费者（G18 Apply 还没到）。**空接口比错接口便宜** |

### 14.3 落地结构（改动点清单）

* `entityFromJsonImpl` 原地劈成 `entityApplyHierarchyImpl`（纯 CPU：父子 + folder）+
  `entityApplyComponentsImpl`（要资产：mesh/material/skinned/…全部）。
* 保留一个**只有两行**的 `entityFromJsonImpl` 当唯一串联点 —— 这样"拆完之后 Prefab
  与场景加载仍然走同一条路"在代码上无法被悄悄改坏。
* 公开壳 `entityApplyHierarchy` / `entityApplyComponents` 进 `SceneEntityIO.h`。
* `loadSceneFromJson` = 三段组合 + 一行注释"想后台解析就把 ①② 搬走、只留 ③"。
* `kFormatVersion` 提到头文件 `kSceneFormatVersion`，源文件留别名（两份字面量会漂）。
* `SceneDocument.globals` 是**白名单**（`camera/cameras/light/physics/scriptVars/postVolumes`），
  不是"排除 entities"。新增全局块要同时改白名单 + ② 段的 `has()` 分支。

### 14.4 验收数字

```
--selftest save                → SAVE-TOTAL pass=39 fail=0
tools/verify_save.py           → 14/14 PASS（7.4 s）
tools/verify_runtime_m2.py     → RESULT: PASS（C 组 12 项逐字段）
--selftest guid / verify_guid  → 26/26 · 8/8（拆分无回归）
```

`verify_save.py` 四组：A 自检 + **分组存在性**（只数总数的话"整组被删"也能凑够）·
B 真实场景 total 对得上 · C 写出→再读 `assigned=0`（**写端真写了 guid 的唯一运行时证据**）·
D 签名护栏。

### 14.5 否定测试（5 次，都实测到红 → 还原 → 绿）

1. 父子第二遍注释掉 → `instantiate_parent_by_index` / `.._by_guid` / `.._folder` /
   `equiv_parent_shape` **4 条红**（最后一条证明"等价性断言"能抓到"父子只在一处做"）。
2. `raw` 改成 `json::object()` → **6 条红**（`parse_keeps_raw` + transform + 两条父引用 + folder + equiv）。
3. `scene.clear()` 挪到 parse **之前** → `parse_fail_keeps_scene` 红且 `n=0`。
4. 给 `instantiateDocument` 加 `assets::AssetManager&` 参数 → `verify_save.py` D3 红（`sig=dirty`）。
5. 写端不写 `guid` → `verify_save.py` C3 红（`got=(11,0,11)` 全部补发）。

### 14.6 这轮踩的两个新坑（都属工具/参数层）

* **`--max-frames` 不是有效参数，正解是 `--frames`**。写错 = 进程不退出 = 脚本靠
  45 s 超时 kill 收场。`verify_guid.py`（上一轮写的）也中了这一枪，每跑一次白等 4 分钟，
  本轮一并修掉。**全仓 `tools/*.py` 现已无 `--max-frames`**。
* **`--scene <绝对路径>` 打不开却不报错**：运行时把场景名当"资产根下的相对路径"，
  绝对路径失败就退回默认流程继续跑 —— 症状是"脚本 90 秒无输出"而不是报错。
  夹具产物一律落 `build/bin/` 下、用**相对文件名**再读。
* `Scene::light()` 在找不到方向光组件时**自己造一个实体** → `entityCount()` 恒比"文件实体数"多 1。
  断言"清空旧场景"要按**名字**判旧实体是否还在，别写死总数。

---

## 十五、2026-10-27（G17-1 存档文件层）：判据原文

### 15.1 分层边界（这条先看，否则会重复劳动）

`SaveGame.{h,cpp}` 只管 **格式 / 原子写 / 迁移 / 校验**。
「算 diff」「应用到已有世界」「悬挂父修正」= G17-2。混在一起的话，
"格式对不对"和"应用对不对"的失败长得一模一样（都表现为"读档后箱子回到原位"）。

`kSaveVersion = 2`。**v2 的唯一理由**是让迁移机制被真的走过一遍 ——
空跑的迁移表比没有更糟（给人虚假安全感，等真需要迁移时才发现机制是坏的）。
`migrateV1ToV2` 只补 `globals.playerStamina`，**不重建整份 globals**。

### 15.2 原子写的顺序（不能换）

```
write tmp → fflush → _commit(fileno) → 备份旧档(失败不阻断) → MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)
```

* 先 rename 再 fsync：崩在中间会留下"改好名但内容是空的"档，而它看起来完全合法。
* Windows 上**不能**用 `std::rename` 做替换的**理由**不是"它会失败"（MSVC 的
  `rename` 其实能覆盖，实测否定测试没红），而是它不保证 write-through。
  代码注释已按这个真实理由写，别再传"rename 会失败"的说法。
* `.bak` 备份失败**不许**阻断替换：备份失败不能变成"存不了档"。
* 读档：主档解析失败 → 退 `.bak`（只退一次，避免坏 `.bak` 时死循环）。

### 15.3 结构指纹（`sceneStructureHash`）

guid 集合**排序后**顺序喂 FNV-1a。两个坑：

1. **必须排序**：`World` 池子按分配顺序遍历，同内容不同顺序 → 不排序的哈希会
   "无缘无故地变"，于是每次进游戏都报"关卡漂移"，而所有人都会去查 JSON。
2. **必须顺序喂字节**：对每个 guid 单独调一次 `fnv1a64` 再"混合"，等于每次都从
   offset 重新开始 → 只在算最后一个 guid，加/删实体哈希都不变。

### 15.4 `entityToJson` 的 `includeEmpty`（存档要全量）

默认 `false` = 场景文件语义"不写空壳"（只挂 Transform 的实体返回 false）。
**存档必须传 `true`** —— 否则玩家 / 出生点 / 纯分组节点一条都进不了档，
读档后它们"消失"或被随机补发新 guid，而画面上看起来像"读档没生效"。

### 15.5 验收数字

```
--selftest save 33/33 · verify_persistence.py 8/8 · sceneio 39/39 · guid 26/26
verify_runtime_m2 PASS（C 组 12 项逐字段）· check_regression_map PASS
```

四组：A 结构指纹（5）· B 格式往返（7 + capture 4）· C 迁移（6）· D 原子写/读（11）。
`verify_persistence.py` 的 B 组看**磁盘**：A 组可以在"写进了别处 / 只写进内存"时全绿。

### 15.6 否定测试（3 次红 + 1 次"没红"）

1. 不排序 → `hash_order_independent` 红（`fwd=847875,rev=96d7a8`）。
2. 迁移重建 globals → `migrate_v1_keeps_old` 红（`actual=4`，4 个老字段全丢）。
3. `captureSaveGame` 用默认 `includeEmpty=false` → 3 条 `capture_*` 红（`actual=0`）。
4. `replaceFile` 换 `std::rename` → **没红**。记下来：`file_write_twice_replaces` 抓的是
   "写第二遍这个场景"，不是 rename 语义。**"否定测试没红"也是结论，要写进文档**，
   否则下一轮会以为这条断言能防住 rename。

### 15.7 两个新坑

* **自检故意造坏数据必须收尾**：为测 `.bak` 兜底把主档写成 `{ this is not json`，
  不收尾就在磁盘上留一份坏档 → 脚本 B 组红，而人会去查"写端是不是坏了"。
  加 `file_restore_valid_after_negative`。
* **`Scene` 不可拷贝/不可移动**（有 BVH 与池子）→ 不能写"返回 Scene 的 lambda"（C2280）。

### 15.8 遗留

* `--selftest save-crash`（写 tmp 后 `_exit(3)`，父进程断言旧档可读）—— 需要专用
  CLI 子模式 + 脚本起子进程；纯 C++ 自检做不到"自我杀进程后再断言"。
* 槽位目录 / CLI / 控制台 `save.*` = G17-3。本层路径由调用方给（engine 不拼目录）。

---

## 十六、2026-10-27（G17-2 存档应用层）：判据原文

### 16.1 分层边界

`SaveGame.{h,cpp}`（文件层：格式 / 原子写 / 迁移 / 校验）vs
`SaveGameState.{h,cpp}`（应用层：基线 / diff / apply / 悬挂父）。
混在一起的失败长得一模一样（都表现成"读档后箱子回到原位"），排查无下手处。

### 16.2 基线的三个状态

| 基线里有 | 存活 | 含义 | diff |
|---|---|---|---|
| 否 | 是 | 运行时新生成 | 写整条 |
| 是(h) | 是 | 关卡自带 | `h` 同 → 跳过；不同 → 整条 |
| 是(h) | 否 | 已销毁 | 写 `_destroyed`（**每次**） |
| 否 | 否 | 关卡没有 / 生成后又被销毁 | 不写 |

⚠⚠ 「已销毁」**粘性**：`noteApplied` 里对 destroyed 条目写 `kDeadHash`（0），**不要 erase**。
erase 之后下一次存档不再写 `_destroyed` → 它被读进**还带着那个实体**的关卡 → 怪复活。

### 16.3 存档的"一条实体 JSON"（`saveEntityJson`）

= `entityToJson(w, e, /*空下标表*/, je, /*includeEmpty=*/true)` + `je.erase("parent")`。
给空下标表是为了让 `entityToJson` 自己走"父不在导出集合里"那条分支写 `parentGuid`。

**三条理由（缺一条都不该改回下标）**：① 应用不读层级；② 增量档只写一部分实体，下标指错人；
③⚠⚠ 下标让"销毁一个实体"把别人哈希带翻。

⚠⚠ **`World` 的池子是 swap-and-pop**：销毁下标 i 时**只有最后一个元素**被搬到 i，其余纹丝不动。
所以"销毁第一个"的夹具**测不出**下标耦合（父的下标不变）——夹具必须让**父排在最后**。
这条是"断言看似覆盖、其实假绿"的教科书案例：第一版夹具写错了，否定测试时那条是绿的。

### 16.4 应用顺序（`applySaveGame`）

① 漂移统计（`missing / total > kMaxMissingRatio(0.5)` → **拒读，一个实体都不许动**）
② `_destroyed` 先销毁 → ③ 逐条覆盖（`entityApplyShell` + `entityApplyComponents`）
④ `fixOrphanParents`（**无条件**跑，手改的档也会造出悬挂）
⑤ `globals.vars` **合并**进 `scene.scriptVars()`（不清空：脚本 `let` 的关卡常量不在存档里）

⚠ ②③ 的顺序**观测不到**（两种顺序的终态一样），别为它编断言；写在前面的理由是"白写一遍 + 短暂悬空"。
⚠ 覆盖**复用**组件解析，不写"值白名单" —— 白名单 = 第二套字段解析，加组件必漏改且不报错。

### 16.5 无设备自检怎么跑应用层

`assets::AssetManager am{assets::TextureContext{}}`（`device = nullptr`）。夹具**不许**有
`mesh` / `material` / `skinned` 等块：`resolveMesh(am, json(nullptr))` 直接 `return nullptr`
（`!jm.is_object()` 那条短路），一次 GPU 调用都不会发生。加了资产键 = 自检进程直接崩，不是 FAIL。

### 16.6 验收数字 / 断言分组

```
--selftest save         SAVE-TOTAL pass=77 fail=0（A 5 / B 11 / C 6 / D 13 / E 12 / F 25 / G 5）
verify_persistence.py   20/20（A1~A7 + B1~B7）
sceneio 39 · guid 26 · dialogue 105 · sequence 67 · verify_runtime_m2 PASS · regression_map PASS
```

`verify_persistence.py` 新增四条**锚点**断言（A4~A7）：`SAVE-APPLY` / `SAVE-ORPHAN` /
`SAVE-SCENE-DRIFT` / `SAVE-DRIFT-REJECT` —— 只看数值断言数不够，自检可以"全绿"而应用路径一次没走。

### 16.7 否定测试（6 次，全部实测到红 → 还原 → 绿）

1. `captureDiff` 不传基线 → **18 条红**（diff 全组 + 销毁/悬挂父全组）。
2. `saveEntityJson` 改回下标 → 首轮只有 2 条红（`json_has_parent_guid` / `.._value`），
   `diff_no_index_coupling` **假绿** → 改夹具（父排最后）重做 → **4 条红**。
3. `noteApplied` 用 `erase` 代替 `kDeadHash` → `diff_dead_sticky` 红（`d1=0,d2=0`）。
4. `kMaxMissingRatio = 1.0` → `apply_drift_reject` + `.._untouched` 红。
5. 不调 `fixOrphanParents` → `apply_orphan_parent_fixed` / `.._count` 红。
6. `hashEntityJson` 只哈希 `transform` → `diff_detects_visible` + `hash_is_dump_fnv` +
   `apply_orphan_fixture_total` 红。

### 16.8 已知边界

* `HierarchyComponent::children` 存 `EntityId`（无 generation）→ 槽位复用后 `alive()` 会误判，
  悬挂父清扫**可能漏判**。彻底修要存 `{id, generation}`，属 G16 延伸。
* 父被销毁后子实体的 `parentGuid` 写不出来（`w.guid(handle(dead))` = 0）→ 销毁一个父会让所有子
  都被判"改过"（**正确**：持久化的父引用确实变了）。写死"只 1 条"的期望会把这个行为当 bug 抹掉。

---

## 十七、二次元 UI 主题改造（10-27，第 1/2/3/5部分/6 条）

**权威 → `docs/anime_ui_restyle.md`**（需求状态表 / 压暗原理 / 两条路线 / 实测数字）。

### 17.1 本轮最反直觉的一条：`c^(1/2.2)`

本工程交换链把 ImGui 浮点色**当线性值再编码一次**（坑点 #34）→ 设 `0.106` 上屏是
**RGB 92（中灰）**。所以「深色主题从来不真黑」不是色号选错，是**整条链路多了一次 gamma**。

修法：`darkenThemeColors(Theme&, float k)` 遍历 **60 个 `ImVec4*`** 做 `pow(c, k)`。
动漫档 `k = 1.45` → 保 0 与 1、保色相、只压暗部。
实测：窗口底 `rgb(95,90,99)` → **`rgb(73,69,77)`**，luma 71.1。

⚠ 只对**深色**动漫档压（`if (!t.tok.isLight)`）；浅色档压了会变脏。
⚠ **老 5 套（0~4）一个系数都不许动** —— `verify_theme_language` 的亮度分区是护栏，
改完必须逐位复测（本轮 102.4/85.2/122.5/109.9/245.3 全部不变）。

### 17.2 圆角必须从「全局 Style」里拆出来

`WindowPadding / ItemSpacing / FramePadding.x` 一动，**所有面板矩形全变**
→ 按 rect 点控件的用例静默漂移。圆角只动四角几十个像素。

所以新增 `ThemeMetrics`（圆角 7 项 + 边框 4 项 + `framePadY`），
`applyBaseMetrics()` 仍全局共用一份 padding/spacing。`applyTheme` 末尾同时更新
`g_tokens / g_metrics`，`metrics()` 供自绘件读。

动漫档度量：`window/child 12 · frame/grab/tab 9 · popup 14 · scrollbar 10`，
**边框全 0**（`popupBorder = 1` 例外，弹窗没边会飘）；工程档 8/6 + 保留 1px。

### 17.3 主题下标**只能追加不能插队**

偏好落盘存的是**整数下标**。往 `ThemeId` 中间插一套 → 老配置静默换主题。
新增 5 套一律追加在 5~9：`SakuraNight / MatchaMint / SunsetPop / CyberNeon / SakuraDay`；
`kDefaultThemeIndex = SakuraNight(5)`。
`--selftest theme` **A 组**就是这条护栏：`venn_window_bg` / `venn_accent_r/g/b` /
`daylight_is_light` 四条硬编码旧值。

### 17.4 `applyTheme` 必须能在**没有 ImGui 上下文**时跑

`--selftest theme` 是纯数值、不建窗口。`StyleColors*` / `GetStyle()` 在
`GImGui == nullptr` 时直接断言崩：

```cpp
if (ImGui::GetCurrentContext() == nullptr) { g_tokens = t.tok; g_metrics = t.met; return; }
```

没有这行，整个自检只能靠开窗口跑（慢、要 GPU、GUI 用例还不能并行）。

### 17.5 三个「看着更好其实更糟」的决定

1. **`ImGui::AddRectFilledMultiColor` 不支持圆角** → `gradientRectV` 的绕法：
   先铺一整块 bottom 色**圆角**矩形 → `PushClipRect` 把渐变限制在上半段 →
   接缝处补一条同色细线。直接拿 MultiColor 画会露出**方角**。
2. **`pillButton` 用 `InvisibleButton` 而不是 `Button`** —— 保留 ImGui 的命中 /
   Hover / 键盘导航 / Item 注册，只接管绘制。尺寸由调用方给，语义等同 `ImGui::Button`
   → 「按钮换形状」不会让锚点矩形漂移（三个 CTA 尺寸与原来逐像素一致）。
3. **只自绘按钮一类纯点击控件**。`Checkbox / Slider` 内部还有键盘导航、拖拽、
   tooltip，重画必丢。勾选框的"圆润"靠 `FrameRounding = 9` 拿，不重画。

### 17.6 混色系数：量级差会把它变成「粉条」

动漫档派生完 `header = mix(bg, accent, k)`，`k` 从 0.28 一路收到 **0.14 / 0.20 / 0.28**
（normal/hover/active）；`headerBand` 的 `mix(barBg, accent, ×)` 收到 **0.05**、
底边线 0.55 → **0.40**。

原因：压暗后 `barBg ≈ 0.04` 而 `accent ≈ 0.97`，**量级差 20 倍** → 混 0.24 得到的是
一整条高饱和粉（实测 `rgb(138,85,113)`），不是"淡淡的顶栏"。

### 17.7 `BeginChild(..., true)` 用的是 `ImGuiCol_ChildBg`

只改 `tok.cardBg` 不够 —— 两栏与窗口底只差 3 个色阶（看起来像没分层）。
动漫档必须补一句 `t.childBg = t.tok.cardBg;`。

### 17.8 色卡：`themeSwatch(i, bg, accent)`

纯查询接口，**不改当前主题**。Preferences 的 `id == "theme"` 行特判画 10 枚
（上 = 强调色、下 = 窗口底的竖向渐变 + 7px 圆角 + 选中描边），
锚点 `PREFS-THEMECHIP-%d`，点击 `VK_LOG_INFO("EDITOR-THEME index=%d name=%s")`。

⚠⚠ **色卡只在 Appearance 页画，Preferences 默认停在 Interface 页** →
`verify_theme_anime.py` 的 C 组**顺序不能反**：先 `PREFS-TAB-Appearance` 点击切页，
再数色卡。反了就是白等 10×12 s 全红，而 C2（点击后变绿）却是绿的 —— 这种
"部分绿"的形态最容易骗过自己。

### 17.9 播放条颜色改 token 后要复跑 `verify_add_menu`

五个写死常量换成 `tk.ok / tk.error / tk.info / tk.warn` 后，`TR-BAR` 区间的
高饱和中点断言仍 PASS（底栏/菜单锚点无漂移）。**换颜色 = 换像素 = 必须复跑像素用例**。

### 17.10 本轮实测

```
--selftest theme              22/22
--selftest save               77/77（存档无回归）
verify_theme_anime.py          9/9   A3 + B3 + C1/C2/C2b
verify_theme_language.py      12/12  （主题 0~4 逐位不变）
verify_add_menu.py            全 PASS
check_regression_map.py       PASS
```

`TRIGGERS["verify_theme_anime"] = ["launcher/LauncherApp.*", "runtime/main.cpp"]`；
`engine/ui/**` 在 `GLOBAL_GLOBS` 里 → **刻意不登记**，否则 `--narrow` 下会假跳过。

### 17.11 遗留

第 4 条（Blender 式自由拖拽 / 合并**区域**）**本轮没做**，两条路线对比见
`docs/anime_ui_restyle.md` §5。推荐 **B：把 `SplitLayout` 升级成可变树**，
分三刀 ① 槽位映射运行时可写 → ② 拖拽换位 → ③ 合并成标签组。
A（换 ImGui docking 分支）改动量小但等于换掉整个上游分支，编译期风险不可控。

---

## 十八、E29-1 面板槽位映射运行时可写（10-27 ✅）

**权威 → `docs/editor_docking_roadmap.md`**（三刀拆分 / 坑 / 下一刀落点）。
用户需求：「各个功能面板应该是灵活可变的，且支持自由拖拽和合并区域的，就和
blender 中的差不多」。路线定了 **B：`SplitLayout` 升可变树**（A = 换 ImGui
docking 分支，本机无外网拉不下来，且 20+ 面板的定位语义全要重写）。

### 18.1 数据结构：叶子从 int 变成「面板组 + 活动下标」

```cpp
std::vector<int> groupA, groupB;   // 槽位值（kSlot*）
int activeA = 0, activeB = 0;      // E29-3 的标签页选中项
int panelA() const;                // 派生值，空组 → kSlotNone
```

`resolve` / `syncPanelVisibility` / tooltip / 最大化全部改走 `panelA()` ——
**"组里装几个"对它们透明**，所以 E29-3 加标签条只是画的问题。

⚠ `panelA()` 是**函数不是字段**：`组 + 活动下标`是组合状态，就地派生才不会出现
"组换了、缓存没跟上"。

### 18.2 ⚠⚠ 读盘必须 `restoreGroups` 整份替换，不能逐格 setGroup

第一版逐格调：
```
setGroup(2,A,[inspector]) → 清 A（原 hier）；加 inspector 时它在 (2,B) → 跨位置去重跳过 → A 空
setGroup(2,B,[hierarchy]) → 清 B（原 insp）；hierarchy 在 A 已空 → 成功
结果：右栏只剩一格
```
**而 `insp.y < hier.y` 这条断言还是绿的**（insp 是 0 矩形，0 < 394）—— 假绿。
是旁边"两格同宽同列"（insp=(0,0) vs hier=(1137,463)）把它抓出来的。
→ `restoreGroups(groups[3][2])` 一次性清空再灌，顺序无关。

### 18.3 ⚠⚠ `apply()` 必须先把树内四个槽位清 0

`LayoutRects` **逐帧复用**。面板被搬走 / 合并后那个槽位在树里不存在了，不清零
就留着上一帧矩形 → 同一面板在旧位置和新位置**各画一遍**（"面板被劈成两块"）。
`out.content` **不清**：它由底栏抽屉直接写，不经过分栏树。

### 18.4 ⚠⚠ `kSlotModes` 是**合成槽位**

模式栏（模式列表 + 当前模式参数）由 `ToolkitEditors` 直接钉进 `modes` 矩形，
**表里没有 id=="modes" 的 PanelDesc**，而**八个模式参数面板共用**
`layoutSlot == kSlotModes`。于是：
* `panelIndexOf("modes")` 查不到 → 落盘会静默丢掉这一格（实测 `applied=3` 而不是 4）
* 靠 `layoutSlot` 反查会命中 **`landscape`**（第一个共用它的面板）
→ 落盘 id 用 **`moderail`**，读盘**兼容别名 `modes`**（日志锚点/文档都叫它 modes）。
这一格的丢失还很隐蔽：左栏宽度会从 460 塌成 0，而右栏变宽 —— 只有"同宽同列"那类
断言能看出来。

### 18.5 落盘：`"slots"` 段

```json
"slots": [{"a": ["modes"], "b": []}, {"a": ["viewport"], "b": []},
          {"a": ["inspector"], "b": ["hierarchy"]}]
```
* **只在"与默认树不同"时写** → 没动过布局的用户，文件与改造前逐字节相同。
* 老文件没有这一段 → 保持默认树（**不覆盖**）。
* 空数组在这一阶段无法与"没写"区分 → 一律按"保持默认"（`kDefLeaf` 先填进 groups）。
* ⚠ **不升 `kFormatVersion`**：加字段是向后兼容的，升版本会把所有老布局当"没有"，
  白丢用户的窗口尺寸/比例。

### 18.6 自检（`MYVK_LAYOUT_TEST=1`，33 条）

走 **env 不是 argv** —— `Editor.exe` 丢弃命令行（本仓 P1 坑），且自检必须在
Vulkan / 窗口之前 return。分组：A 默认树（6）· B 定位（4）· C 搬家（11）·
D 交换（5）· E 矩形（4）。

⚠ **E 组才是关键**：只断言 `groupPanel()` 变了证明不了"接上了 LayoutRects"
（可能根本没写矩形）。`rect_follows_swap` / `rect_vanished_viewport_zeroed`
两条是唯一能证明端到端的。

### 18.7 两次"期望写错、实现是对的"

1. `rect_vanished_slot_zeroed` 断言 `inspector.w == 0`，实际 330 —— 搬走的是
   **Hierarchy**，消失的是 **Viewport**（它与 Hierarchy 同组，活动项变成后者）。
2. `LAYOUT-SLOTS applied=2` —— `applied` 数的是**文件里指定并成功解析的条目数**
   （4 个），不是"换了几个"。

### 18.8 实测

```
MYVK_LAYOUT_TEST=1 Editor.exe   LAYOUT-TOTAL pass=33 fail=0
verify_layout_slots.py          9/9   A1~A2 + B1~B3 + C1~C2
verify_layout_v4.py             全部 PASS（默认布局零漂移：模式栏 x=0 w=460、
                                右栏 x=1270 w=330、hier 底 394 贴 insp 顶 394）
check_regression_map.py         PASS
```

`TRIGGERS["verify_layout_slots"] = ["editor/SplitLayout.*",
"editor/EditorLayoutIO.*", "editor/LayoutSelfTest.*", "editor/main.cpp"]`
（`EditorApp.*` **故意不登记** → 改它走全量）。

### 18.9 下一刀（E29-2）

标题栏 `BeginDragDropSource`（payload = 面板 id）→ 四格 `BeginDragDropTarget`
→ 落下 `locatePanel` + `swapActive`；落点高亮画在 `ForegroundDrawList` 并
`addOccluder()` 挡住分隔条。未决：视口能不能被搬走（建议允许换位、不允许并入
别人的标签组）。

---

## 十九、E29-2 面板拖拽换位（10-27 ✅）

**权威 → `docs/editor_docking_roadmap.md` §5**。用户需求：「各个功能面板应该是
灵活可变的，且支持自由拖拽和合并区域的，就和 blender 中的差不多」。

### 19.1 ⚠⚠ 为什么**不用** `ImGui::BeginDragDropSource`

它是**挂在 item 上**的（要求 `g.LastItemData` 是刚提交的 item）。本工程的面板是
普通顶层窗口，**标题栏是 ImGui 在 `Begin()` 内部画的，不是 item**。绕过去只有：
① 在内容区塞一个盖住标题栏的 `InvisibleButton`（抢折叠/×/双击，还要掰 ClipRect）；
② 铺一层全屏 overlay（抢整个编辑器输入）。两条都得不偿失。

→ 改成**只读鼠标状态**（`IsMouseClicked / IsMouseDown / MousePos`）自建状态机，不提交
任何 item。树内面板本来就 `NoMove`，所以 ImGui 那边不会同时拖窗口，没有"两个拖拽打架"。

### 19.2 语义前提：**槽位矩形 = 面板矩形**（E29-1 送的）

`LayoutRects` 的字段是**按面板槽位**写的（`SplitLayout::writePanel`），不是按格子。
面板搬到别的格子后矩形自动跟着走 → **抓手/落点的命中直接用 `L.hierarchy / L.inspector / ...`，
不需要知道树结构**；树只在**落下那一刻**用一次（`locatePanel`）。
这一条想反了就会去写"格子矩形"那一套，然后发现和 LayoutRects 对不上。

### 19.3 抓手高度：算，不要找窗口

`titleH = FontSize + FramePadding.y * 2`。
⚠ **不要**去取 `ImGuiWindow::TitleBarRect()`：那得先按名字找到每个面板窗口，而名字
各不相同 —— Hierarchy/Inspector 是 `trId()` 的翻译结果、模式栏是 `###mode_rail`、
视口是动态标题。槽位矩形本身就是窗口矩形，顶部这一段必然是标题栏。

### 19.4 状态机三段（每段都有非它不可的理由）

| 段 | 判定 | 少了它会怎样 |
|---|---|---|
| `armed` | 按下点落在标题栏 → 只记候选 | 一按下就进拖拽态 → 点标题栏（切模式 / 给焦点）变成拖拽 |
| `active` | 位移 **> 6px** | 手抖 1px 也算拖 → 点击几乎不可用 |
| 落下 | 松开时鼠标所在**整格** | 只认标题栏的话，"拖到目标格中间"落不下去（Blender 手感是整格） |

其它两条：
* **Esc 取消** —— 少了它，中途按 Esc 再松开会在松手处**照样**交换。
* 落 = `swapActive`（**交换**，不是搬走）—— E29-3 之后"搬走"会让源格变空、
  越拖越空；交换则格子数恒定。
* `armed` 只在 `clicked` 那一帧判定：之后鼠标再移进标题栏**不再 arm**，
  否则"从视口划出来经过标题栏"会凭空开始一次拖拽。

### 19.5 `kSlotModes` 只能当落点，不能当抓手

合成槽位（8 个模式参数面板共用一块）+ 顶部是 9 行模式单选。拖走"模式栏"没有语义，
而误吞点击会让人以为"点模式没反应"。头文件里 `canDragSlot()` 就这一条例外。

### 19.6 高亮：`ForegroundDrawList` + 主题圆角

源格 accent 6% 填充 / 30% 描边；目标格 16% / 95% 2.5px；圆角 `ui::metrics().windowRounding`。
必须画在前景层（和分隔条同一条理由：面板都是顶层窗口，普通 DrawList 压不住）。

### 19.7 验收（`verify_layout_drag.py`，9 条）

```
A 自检 F 组 25 条（`MYVK_LAYOUT_TEST=1` 共 58 条）：命中面 / 阈值 / Esc / 落下 / 原地与空白
B 真鼠标拖（PostMessage WM_LBUTTONDOWN → 14 步 WM_MOUSEMOVE → WM_LBUTTONUP）
  modes=(0,70,454,716) hier=(1253,70,325,301) insp=(1253,371,325,415)
→ modes=(1253,371,325,415) hier=(1253,70,325,301) insp=(0,70,454,716)
  LAYOUT-DRAG from=inspector to=moderail     ← 两格**逐位互换**
C 对照：同一位置按下再原地点开 → 无 LAYOUT-DRAG、矩形一位不变
```

⚠ **B 组必须存在**：F 组只能证明状态机对，证明不了"鼠标事件真的喂进去了"
（`IsMouseClicked` 没接上 / 标题栏高度算错 / 坐标不是客户区坐标，都只在真窗口暴露）。
⚠ B/C 的判据用**矩形互换**而不是"日志里出现 LAYOUT-DRAG"：后者在"换了又换回来"时也是绿的。

### 19.8 本轮的诚实记录

**一次通过，没有踩坑** —— 因为前面 19.1~19.5 这五条都是"先想反了会怎样"再写。
唯一改动过的是把 `drag_hit_outside` 的坐标从面板内挪到菜单栏（`5,5`）：
原写法在"点落在 hierarchy 矩形内"时永远命中，测不出边界。

### 19.9 下一刀（E29-3 合并成标签组）

见 `docs/editor_docking_roadmap.md` §6：格顶 22px 标签条（锚点 `LAYOUT-TAB-<slot>`）、
面板矩形下移 22px（**最易让像素用例漂移的一步**）、拖到格中部 = `movePanel` 追加、
拖到边缘 = `swapActive`（已实现）。未决：**视口不许并入标签组**（非活动标签不画 →
3D 视口会消失），建议 E29-3 明确禁止。

---

## 二十、E29-3 一格多面板（标签组，10-27 ✅）

**权威 → `docs/editor_docking_roadmap.md` §6**。需求：「支持自由拖拽和合并区域的，
就和 blender 中的差不多」——这是最后一条没做的 UI 需求。

### 20.1 ⚠⚠ 最反直觉的决定：标签条**画在标题行里**，矩形一个像素都不动

原计划（写在路线文档里）是"格顶画 22px 标签条、面板矩形下移 22px"。真动手时否掉：

* 下移 22px ⇒ **所有按 rect 点控件的用例集体漂移**（本仓最贵的一类回归），
  而且标签条 + 窗口自己的标题栏 = **46px 双层 chrome**，比不合并更丑；
* 标签条和标题栏本来就是**同一行东西**（都写在这格顶上）→ 直接
  `ForegroundDrawList` 画几个圆角 pill 盖住标题文字，就是 Blender 的样子。

⇒ 零漂移（`verify_layout_v4` 全 PASS 就是证据）。代价是标题栏原有的折叠/×
被盖住 —— 树内面板本来 `NoCollapse`，× 在右侧留 26px 没盖。

### 20.2 手势：中部 = 并入，边缘 = 交换

| 落点 | 动作 |
|---|---|
| 目标格中部（四周各留 25% 边） | `movePanel` 追加成标签（源格被摘空 → 收成 0 宽） |
| 目标格边缘 | `swapActive` 整格交换（E29-2 老行为） |

⚠ 边距夹在 `[min(8px,40%), 40%]`：窄栏按纯 25% 算中部落点只剩 2px，对不准。
⚠⚠ **中部落在"不能当标签"的格子上要退回交换**，不能静默无反应 —— 否则用户
分不清"被拒绝"和"没接住"。E29-2 的老用例因此从"拖中部"改成"拖边缘"，
**语义没变、机器一位没动**。

### 20.3 ⚠⚠ 视口 / 模式栏**不能当标签成员**（`canBeTabMember`）

非活动标签**不绘制** → 视口一旦被并进去，用户看到"3D 视口消失"，只会以为坏了。
`canDragSlot` 与 `canBeTabMember` **刻意是两个函数**：**能拖 ≠ 能并**。
视口仍可整格交换（拖到边缘），只是不能被合并。

### 20.4 ⚠⚠ 单向门：双击标签 = 送回老家

并入后**源格被收成 0 宽**（E06-5 既有机制）→ 鼠标再也点不到它。没有退路的话
用户并一次就少一格，只能 Reset Layout。
`homeCellOf(slot)` 给四个树内槽位各定一个"老家"，与 `buildDefault` 的四个叶子
**一一对应**（改那边必须改这边）—— 一一对应是"送回去永远有地方放"的唯一理由。
双击判 `ImGui::IsMouseDoubleClicked`，**两次 `WM_LBUTTONDOWN` 间隔必须 < 0.30s**，
所以 `verify_layout_drag.py` 里双击单独一个函数（`drag()` 那些 0.15s sleep
永远判不出双击），并且**重试 3 次**。

### 20.5 ⚠⚠ `trId()` 返回 `"译文###stableId"`

第一版直接把它当标签文字 → 画面上是 `Hierarchy###panel_hierarchy`，而
**`CalcTextSize` 也按整串算宽**（两倍多）→ 第二枚标签被"放不下就截断"挤掉，
**标签组里只剩第一个面板的标签**。数据层完全正常，只是"少画了一个"，
极具误导性。修法：`tabVisibleLabel()` 在 `"##"` 处截断。`trLabel()` 也不适用
（它保留 `##id` 后缀）。

### 20.6 落盘：活动标签靠**顺序**记住

`restoreGroups` 读盘一律 `activeIndex = 0`（要"顺序无关"，见 §18.2），所以
"当前显示哪个标签"只能靠组成员顺序 → **写盘把活动面板写最前面**。
不写的话重开编辑器永远显示第一个成员，用户以为"我选的标签没保存"。

### 20.7 验收（`verify_layout_drag.py` 22 条 / 自检 97 条）

```
A 自检（阈值 >=90）：97 条全绿，其中 G 组 39 条
D 真拖合并：
  LAYOUT-MERGE from=hierarchy to=inspector size=2
  hier=(1270,70,330,772)（整格） / insp=(0,0,0,0)（非活动不占矩形）
  标签锚点 hier x=1369..1459 / insp x=1276..1365
  LAYOUT-TABCLICK panel=inspector idx=0 → insp 可见、hier.w=0
  LAYOUT-TABHOME panel=hierarchy -> 2/0 → 拆回 hier=(1270,70,330,324)
                                          insp=(1270,394,330,448)
B 交换（旧行为）：落模式栏中部 → 因为它不能当标签成员，退回交换
```

⚠ 自检 G 组里唯一"能证明端到端"的仍是**矩形**：`merge_inactive_rect_zero` /
`merge_active_rect_is_whole_cell` / `detach_rects_both_visible`。只看
`groupSize == 2` 证明不了"画面上真的并了"。

### 20.8 本轮诚实记录

一次通过（97/97 + 22/22）。唯一被打回来的是 **20.5 的 `###id`**，而且它是
**D5 断言抓出来的**（"两个标签锚点都在" → `hier=False`）—— 当时第一反应是
"logRect 去抖把第二枚吞了"，实际是宽度把第二枚挤掉了。
教训：**"少画了一个"这类缺陷要靠"数量断言"抓，不要靠"有没有"断言**。

## 二十一、2026-10-27 晚：清掉遗留红（5 条）＋一条产品行为变更

起因：用户问"有没有遗留的未解决的 bug"。清单上的"N 条已知红"里，
**只有 1 条是真 bug**，其余是记账债与夹具陈旧 —— 而这个区分本身才是重点。

### 21.1 真 bug：浮层 Content 抽屉遮住右栏 → 材质拖放不可用

现象：`verify_material_chips` / `verify_material_roundtrip` 三槽全不落地，
日志里连一行 DROP 都没有，retry 三次仍 `(empty)`。

取证（截图，`_venn_verify/probe_drag/1_armed.png`）：
拖拽源**是好的**（预览窗 "drop on a Material texture slot to bind" 跟着
鼠标走），但整块材质区在抽屉**后面**。几何：抽屉 `(0,786)-(1578,1106)`
横跨整窗，材质槽 `metallic=(1473,1074)-(1501,1102)` 完全落在里面。
→ 鼠标落点被抽屉吃掉，接收端 `IsItemHovered()` 永远 false。

修法（`computeLayout` 的 `L.content`）：**浮层形态让出右栏**，右缘取
`L.inspector.x`（Hierarchy 并进标签组时取 `L.hierarchy.x`）；右栏 < 160px
时视同没有右栏、退回通栏。停靠形态不变（它是布局的一部分，谁也不遮）。

⚠ 这条会连带影响三处期望，改的时候一起改（否则"修一处红三处"）：
- `verify_bottom_bar` B2d 原来断言"抽屉横跨整窗" → 改成"右缘止于右栏左缘
  （与视口右缘差 0~20px，中间是 12px 分隔条）"；新增 D2b 守"停靠后仍横跨
  整窗"（**两个形态各守一条，别只守一个**）。
- `verify_bottom_bar` D1 的基准从 `W-220` 改成 `抽屉右缘-220`。
- `verify_content_three_pane` G2/G3（分隔线下沿被裁）自动跟随（它读的正是
  `L.content`），实测仍绿。

### 21.2 用例坑：`logRect` 是**逻辑坐标**，滚出可视区的控件仍报矩形

材质槽 `roughness y=1111..1139` / `emissive y=1148..1176`，而 Inspector 底边
= 底栏上沿 1106（窗口客户区 1164）→ 两行在面板**外面**（被 ImGui 裁掉），
但 `logRect` 照样打它们的布局坐标。于是用例"有坐标 → 拖 → 没反应"，连
"矩形缺失"这种明确错误都报不出来。

修法：拖之前用 `wheel_at` 把目标滚进可视区（`verify_material_chips` 的
`scroll_slot_into_view`；`verify_material_roundtrip` 有一份等价实现，因为它
用的是 `verify_mesh_params.wait_rect` 而不是自带 LogTap 的接口），并加一条
"滚不进来就明确 FAIL"的断言。

⚠⚠ **同一个坑在目录树上又踩了一次**：`verify_content_three_pane` B2 与
`verify_content_action_row` F0 都点 `CB-TREE-NODE scenes` —— 那是树的最后
一个节点，1600x900 时压在最底下，点击等于点在底栏上。两条用例因此长期红。
修法同上（`verify_asset_drag.wheel_at` + 各自滚到自己那块容器里）。

### 21.3 夹具陈旧：`verify_viewport` 的 V1d / V3（#317 关掉）

三处都钉在"某个历史时刻的像素坐标"上，布局一改就全错位，而且**报的话与
真因无关**（"VP_INNER 不在视口里"，真因是左栏 300→460）：
- `CONVERGED_VP` 还是 `(305,145,1241,778)` → 永远不收敛（每次启动 WARN）。
  实测新值 **`(463,139,1241,778)`**。
- `VP_INNER=(330,200,1200,750)` 绝对坐标 → 改成 `vp_inner(VP-RECT)` 相对
  推导（内缩 25/55/41/28）。顺带把 `V1d` 拆成两条真断言：视口宽≥600 高≥400
  ＋左边界压在屏幕上一根真实分隔线上（±8px）——后者能抓"改分栏没同步日志"。
- `tools/vp_nohud_baseline.png` 是 **E14 浅色主题**截图 → 按当前默认布局＋
  默认主题**重抓**；`BASE_VP_TOP` 184 → **139**。
  ⚠ 旧骨架第三条 `1574` **不是面板分隔线**，是浅色主题下才看得见的窗口右
  边框；深色档下检测不到，所以"少一条"不是布局回退。
结果：42 项全 PASS。

### 21.4 记账债：i18n 102 条 / 特性面 21 条

i18n（`verify_i18n_coverage`）：A1=5 A2=0 B=1 B2=95 D=2 E=1。全是 10-26/27
新面板（Prefab / 弹簧骨 / 双骨 IK / 注视 / 挂点 / 对话 / 多选 / 卡通材质
tooltip）—— `TR()` 包了但词典没条目，中文界面静默显示英文。补 102 条
（20 条多行 tooltip，占位符逐个对过），词典 2229 → **2331**。
⚠ 两条**不该翻**（进 `FMT_KEEP`）：`LAYOUT-TAB-%s`（E29-3 的 logRect 锚点）、
`PREFS-THEMECHIP-%d`（色卡按钮的 widget ID）—— 翻了脚本抓不到、按钮状态丢。
`pos 0.00` / `pos %.2f` 是字段名，包 `TR()` 但译文与英文**逐字相同**。

特性面（`check_feature_surface`）：21 条裸 env = 14 条测试/自检/日志探针
（进 `ALLOW_EXACT`，逐条写理由）+ 6 条描边参数（新增 `id="outline"` 特性项，
ui token 用参数 id 前缀 `outline.` —— `_collect_editor_sources()` 把
FeatureRegistry.cpp 也拼进去了）+ 1 条 **`MYVK_X`**：它在 `RunMode.h` 的
**注释**里当例子，扫描是纯正则、不认注释 → 改写成 `MYVK_<NAME>`。
结果：裸 env 0 / 特性 37 个全有 UI 面。剩 25 条 `*_DBG` 只是 WARN。

### 21.5 判据（写进 MEMORY 的就是这三条）

1. **"闸门红了" ≠ "真的漏"**：先数条目属于哪类（真漏 / 记账没记 / 夹具
   陈旧）。三类的修法完全不同，混在一起会按最贵的那类排期。
2. **"期望值过时"和"实现回退"在日志里长得一样**：都要先问"这个量靠什么
   成立"。凡是钉在绝对像素坐标上的断言，布局一改就该跟着改夹具。
3. **拖放类用例的两个隐形前提**：目标必须在可视区内（`logRect` 证明不了
   这点），且源与目标之间不能被更高层的窗口盖住（浮层通栏尤其危险）。

### 21.6 本轮实测（可复现）

```
check_feature_surface.py      结果: PASS（37 特性 / 裸 env 0）        原 FAIL 21
verify_i18n_coverage.py       结果: PASS（0 项缺中文，词典 2331）     原 FAIL 104
verify_viewport.py            结果: PASS（42 项）                     原 FAIL 2
verify_material_chips.py      RESULT: PASS（三槽 + 着色器生效）        原 FAIL 10
verify_material_roundtrip.py  RESULT: PASS（存盘/读盘闭环）            原 FAIL 14
verify_bottom_bar.py          结果: PASS（B2d/D1/D2b 已按新语义）      原 PASS→改动后回归
verify_content_three_pane.py  结果: PASS（树节点先滚进可视区）          原 FAIL 2
verify_content_action_row.py  全部 PASS                               原 FAIL 4
verify_terrain_editor.py      （见今日流水）
```
