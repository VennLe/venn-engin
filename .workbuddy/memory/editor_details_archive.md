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
