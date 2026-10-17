# venn —— 项目长期笔记（规则索引）

> **必须整份进注入窗口**，超了**尾部会被截断**。只放 **纪律 + 速查 + 指针**：
> 细则/坑点 → `render_details_archive.md`；**下一步** → `docs/{editor,ship,render}_roadmap.md`；
> **设计理由** → `docs/editor_architecture.md`。
> 手写 Vulkan 引擎（C++17，学习向），仓库 **`D:\A-VENN\code\cpp\venn-engin`**（CMake 名 `Venn`，刻意
> 不一致）。**右手系 Z-up**。现只有 `Editor.exe`（**`Game.exe` = G 系列待做**）。分层
> `core→rhi→render→scene→assets`（另有 `ui`/`ecs`/`physics`；应用层 `editor`，**待加 `runtime`**）；
> **engine/ 不认识 editor/、更不认识 runtime/**。改目录名后必须删 `build/` 重新 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
上下文里的快照**可能被截断/陈旧** → 任何"重"动作（跑回归/批量改文件/改契约）前先读磁盘版，
否则可能正在违反一条**已生效**的纪律（已发生过：因快照缺条目误启动全量回归）。

## ⚠ 回归执行纪律（2026-10-15，用户明确要求）
**不要自动跑全量回归。** 只有用户**明确下达"开始全量回归"**才跑 `run_regression.py --all`。日常只跑
**受影响用例**（按 `TRIGGERS`）；收尾**不要**顺便来一轮全量。⚠ `--explain` 在「没有基准快照」时**一律
返回"全量"**（保守兜底）—— 不代表"必须跑全量"，点名即可。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry` 登记（`bindParam` 必须在**成员默认初始化之后、`loadEnvOverrides()` 之前**）
③ **UI 面**（编辑器点得到）④ 序列化 ⑤ 回归用例 + TRIGGERS。**只有 `MYVK_*`、界面点不到的不算交付**。
把关 `tools/check_feature_surface.py`（**C 无 UI 面 / D 裸 env 漏网 必须清零**）。⚠ `FeatureRegistry.*`
在 `GLOBAL_GLOBS` → 动它触发全量。

## ⚠ 本机环境（归档里"另一台机器"的记录别照抄）
- 仓库 **`D:\A-VENN\...`**（旧笔记的 `E:\...` **不存在**）。**GPU = Intel Iris Xe**（不是 RTX 3060）
  → **8 个 set**，Intel 恰好 `maxBoundDescriptorSets = 8` **卡在线上** → 再加材质贴图槽得改架构。
- Python：`...\envs\default\Scripts\python.exe`（numpy+pillow）。⚠ **裸 `python` 不带 PIL/numpy** →
  回归整批 FAIL 报 `No module named 'PIL'`。

## 构建
入口 **`make build`**；`TOOLCHAIN=auto` → **本机走 MSVC**（MinGW 缺失）。⚠ **MSVC 四坑**：① 代理变量
`https_proxy`+`HTTPS_PROXY` 并存 → MSBuild 假报 `MSB6001: CL.exe 开关无效`；② UTF-8 无 BOM + 中文注释 →
`C2001`（对策 `/utf-8`）；③ `.cmd`/`.bat` **必须纯 ASCII**；④ Make 可能用 sh 跑配方 → `echo` 里不能有
`(` `)` `|`。
- ⚠ **资产拷贝必须 `add_custom_target(... ALL)`**，禁用 `add_custom_command(TARGET ... POST_BUILD)`（后者
  只在**重新链接**时跑 → 改 shader/贴图**不生效**）。三目标拆分后（10-17）改成**全局一份**
  `VennShadersCopy`/`VennAssetsCopy`（`if(NOT TARGET)` 守卫），否则两个 exe 各拷一遍。
- ⚠ `LNK1168` = 残留 `Editor.exe` → `taskkill //F //IM Editor.exe`。⚠ 日志 UTF-16 混合 → `grep -a`。
  ⚠ `strings Editor.exe` **抓不到** ASCII 常量 → 验代码是否进二进制只能靠**行为**。新增 `.cpp` 自动进构建。
  ⚠ `build/bin/config/*.json`（layout/prefs/**render**）**未进版本控制**且运行会被写 → 像素断言必须用
  `MYVK_EDITOR_LAYOUT`/`MYVK_EDITOR_PREFS_PATH`/`MYVK_RENDER_SETTINGS` 钉死。
- ⚠ **三目标拆分（10-17）**：`add_executable` 一改名（`Editor`→`VennEditor`），`add_dependencies` /
  `target_link_options` / 资产拷贝调用**全要同步**，产物名靠 `OUTPUT_NAME` 保住。
- ⚠ `glm::decompose` 在 **gtx**（`matrix_decompose.hpp`，不是 gtc）+ `GLM_ENABLE_EXPERIMENTAL` 必须写在
  **任何 glm 头之前**（含**本文件自己的头**）。GLFW **没有 `glfwGetWindowFocus`** → `glfwGetWindowAttrib(w, GLFW_FOCUSED)`。
- ⚠ 前置声明 `vks`（原 `editor` 脚本引擎）**必须写在 `namespace editor` 外面** → 否则成嵌套 `editor::vks`
  把外层 `::vks` 遮住（症状像"少 include"）。与 `physics` 同源。

## 环境变量（全表 → 归档）
- ⚠ `MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP="near,far"`（钉视口裁剪面，默认
  `0.1/5000`；**顺序不能反**）；编辑器侧注入一律 `MYVK_EDITOR_` 前缀（白名单
  **按前缀放行**，不带前缀 = 裸 env 漏网）。⚠ **落盘读/写不对称**：自动化下**默认不读**、显式
  `MYVK_EDITOR_LAYOUT` 照读、**写一律拒绝**。
- ⚠⚠ **改任何"全局默认"都要回头 grep 依赖它的测试**（2026-10-17 全量回归：视口远平面
  200→5000 让 `verify_frustum_cull`/`verify_bvh` 的 132/4 基线整体漂到 94/42；偏好
  `kPrefsVersion` 升 2 后 `verify_viewport` 的 `write_prefs` 还写 1 → 整份存档被丢弃）。
  **正确修法是钉输入（`MYVK_EDITOR_CLIP`）/ 升 fixture，不是改期望值。**

## 契约速查
- **Pass 序**：`Shadow(4 级联) → LocalShadow(12 层数组) → DepthPrePass → SSAO → ClusterBuild →
  VolumetricFog(×2) → Forward(天空盒→不透明→栅格→透明) → PostProcess → 交换链`。**compute 不能在 RP 内
  执行**（深度预通道独立成 RP 且**无条件执行**）；**栅格夹在不透明与透明之间**、**绝不能**做成 ImGui 叠加层。
- **"改一处就得同步改 N 处"是常态**：`FrameUBO`(768B)/推常量(128B 全满)/set0(25 binding)/变体位序/
  分簇编址/材质贴图 set(7)/旁路管线 range 须与主管线**逐字段相同**（否则 VUID-08600）。
- ⚠ 两处**默认槽**是雷：DBuffer 缺省必须 `m_defaultTransparent`（`m_defaultBlack` 整屏变黑）；探针
  binding 23 缺省必须绑 1×1 全黑 cube array，**不能"不绑"**。
- **颜色空间是契约**：albedo/emissive `srgb=true`；normal/ORM/高度图 `false`；着色器内**不做手动 gamma**。

## 编辑器架构（全文 → 归档「布局范式 · 核心面板」）
- **加面板 = 表里加一行**：`panelTableVec()` + `PanelRegistry`（View 菜单/快捷键/图标/落盘**四路自动派生**）；
  **面板间只准依赖 `EditorContext`**，动引擎走 `EditorHost` 纯虚接口。**`PanelVisibility`** 权威在
  `PanelRegistry.cpp` 的 `bool g_open[kMaxPanels]`（**必须固定数组 + 引用**，vector 扩容 → `bool&` 悬垂）。
  覆盖链：偏好→项目→场景→对象→相机。
- ⚠ **默认一律"全关"**：`config/render.json` 缺失时行为**逐像素等于今天**（硬约束）；新效果一律
  "关闭 = 基线逐像素不变"。
- ⚠ **快捷键有排他**（面板 `Alt+1..8` / 工作区 `Ctrl+Alt+1..6` / 视图键占 `Ctrl+1..8` /
  `Shift+Space` 须排在 `Space` 前）。**别引入 ImGui docking**（用自研 `SplitLayout`）。
- ⚠ **Inspector 左侧 11 个页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**枚举值 = 图标栏顺序**；
  **落盘写 id 字符串不写下标**。⚠ **窄栏属性行的槽位宽度按该列"最长标签"反推**（`kPhysParamW`/`kFxParamW`）
  —— 拍脑袋的宽度会在**滚动条出现之后**才吃掉标签尾巴。
- ⚠ **`All` 不是"融合视图"，是旧堆叠绘制顺序一字不改**（9 个旧脚本靠 `INS-*` 锚点断言分区存在）→
  自动化默认页签 = `All`，交互默认 `Object`。**别"优化"它。**
- ⚠⚠ **锚点语义**：`logRect` 记在"提前返回"**之前**的锚点只能证明**函数被调用**，不能证明**分区可见**
  → **负向断言只能用记在返回之后的锚点。** ⚠ 图标**改完要真的放大截图肉眼看**。
- **主题** = `ui::applyTheme(int)`（`engine/ui/ImGuiTheme.*`，5 套）：度量/颜色解耦，**VennDark 照抄 → 默认
  逐像素不变**；⚠ 浅色主题**逐个**收掉硬编码深色"孤岛"（顶栏/工具条 → `Token.barBg`，叠加片推浅 `Text`）。
- **语言** = `tr()`/`trId()`（`editor/Localization.*`）：**代码只写英文**（硬约束），中文只在词典；
  ⚠ **窗口名兼作 ID** → 改名丢布局 → 一律 `trId(name,id)`。
- ⚠ **`TreeNode` 行上的图标按钮必须加 `ImGuiTreeNodeFlags_AllowOverlap`**，否则 `ItemHoverable`
  （imgui.cpp:5121）因**节点先提交**锁死 `HoveredId` → **图标收不到 hover/点击**。
- ⚠ **偏好 `kPrefsVersion` ≠ 布局 `kFormatVersion`**：偏好**逐键覆盖** → **老存档会把新默认顶回** → 升版本
  让老档作废。
- ⚠⚠ **布局范式（2026-10-17 改）**：**Content 不再是常驻分栏**，改成"从底栏往上弹的抽屉"。
  左栏下半收成 0 → **Hierarchy 吃满整条左栏**；`syncPanelVisibility()` 里 **`kSlotContent` 恒 `false`**
  （**保留 case 保持类型显式**，别删——删了会掉进 `default: return true`）。
  底栏 `LayoutRects.bottom` = 状态栏上方 32px 通栏：`[📁]Content [📄]Output Log [⌨]CMD` + 右侧
  `N entries | assets`；**播放条 UI 已整体移除**（`transport` 字段保留但恒 0×0，旧脚本还在读）。
  抽屉默认**不 dock**（浮层，点外部即关）；`Dock in layout` 勾上后**真的占布局**（`bottomInset` 加高）。
  **抽屉内容 = 三栏**：左目录树（根 `assets/`，点节点切目录）/ 中资源类型单选列 / 右"搜索+排序+网格"
  （排序菜单是**勾选框语义但二选一**；目录永远排文件前；搜索历史只在回车时记）。
  四个坑：① **`SplitLayout::setBottomInset(h)` 是"底栏 + 停靠抽屉"的总让位高度，≠ 底栏高度**
  （拿它当底栏高度 → 底栏被撑成 352px）；`apply()` 不写 `out.bottom`，由 `computeLayout()` 显式写。
  ② **`IsWindowHovered()` 默认不把"有 active item"算悬停** → 点按钮的松开帧返回 false →
  判定成"点外部"、同帧把刚开的抽屉关掉（表现：按钮闪一下抽屉永远不出来）→ 必须加
  `ImGuiHoveredFlags_AllowWhenBlockedByActiveItem`。③ **工作区预设会强制覆盖面板开关**
  （`Workspace.cpp` 的 `kLevelOpen`/`kCharOpen`）→ 只改 `defaultOpen` 不够，**预设里也要删**。
  ④⚠ **ImGui 绘制顺序 = `g.Windows` 顺序，且点中某窗口会把它挪到末尾** → "后画的在上层"
  只在同帧成立（点视口 / 重开抽屉时抽屉会被盖）→ 每帧 `BringWindowToDisplayFront(抽屉)`
  （imgui_internal，**有 popup 时不能提**）。**分隔条画在 ForegroundDrawList** → 必须用
  `SplitLayout::setOccluder(抽屉矩形)` 把线切成 0~2 段，否则竖线直接画进抽屉里。
  ⑤⚠ **popup 开着时 `IsWindowHovered(抽屉)` 是 false** → "点抽屉里别处"会连带把抽屉关掉；
  判定要用**上一帧快照**（`m_contentPopupWasOpen`）—— ImGui 在 NewFrame 就关了弹层，
  帧末查 `IsPopupOpen` 已经 false，抓不到。
- ⚠⚠ **面板里"随选中状态出现/消失的一行"会让整棵树上下跳一行**（点行 → 选中 → 那行出现 →
  树往下跳 → 鼠标已不在该行 → 再点变取消 → 又跳回），表面症状是"行尾图标点了没反应"
  （`verify_hierarchy_status` A2/A3 的 `hover=0 active=0`）。`Path: (no selection)` 面包屑
  整行删掉就是为这个 —— **别用 `Dummy` 占位糊过去**，那只是把抖动换成一条空白。
- ⚠ **Hierarchy 列头必须手画，不能用 `Table`/`Columns`**：`Columns` 会在列结束前固定
  `GetContentRegionAvail().x`，而这宽度还要给搜索框 `SetNextItemWidth(-1)` →
  `m_typeColX` 每帧算一次存成员，给树内所有行共用（行在 `BeginChild` 内、列头在外，
  各算各的**必差 8px 内边距、有滚动条差 14px**）。名字**宽度不够先丢 `[hidden]/[locked]`
  后缀保名字**；截断走 `drawClippedText` 在 **UTF-8 字符边界**二分（`(b & 0xC0) != 0x80`
  才是字符起点），按字节截会给中文留半个字形。
- ⚠ **视口左上角模式条已删（2026-10-17 用户要求）**：Lit/Unl/Nrm… 与右上 Lighting 下拉
  完全重复。视图模式只剩两个入口（Lighting 下拉 + `Shift+1..8`，日志 `VP-MODEBAR key=N`
  仍在）。`VP-OVERLAY` 从此 **3 字段** `master/stats/navGizmo`；左上角为空 → 统计 HUD 的
  `statsY` 不再需要往下避让。偏好键 `vp.overlay.modeBar` 一并删除（老存档残留无害：
  `applyPrefs` 逐项按**本表**查键，表里没有的键根本不读）。

## 断言纪律（全文 → 归档）
- ⚠ **必须 `MYVK_EDITOR_NO_HUD=1`** + `MYVK_GRID=0`；**采样区必须落在被测物体上**（俯角 8° 时"视口正中"
  只采到天空 → 误判"没生效"）。**抓图一律走 `capture_nonempty`**。
- ⚠ **"状态一致"与"像素一致"是两个断言**（前者可精确、后者只能**有界**）；**新断言必须做"否定测试"**
  （退化回 bug 版本 → 必须变红）；**临时诊断代码用完必须清**。
- ⚠ **FAIL 先怀疑「期望」再怀疑「实现」**；**兜底条件必须与放行条件同源**；⚠ **flaky 先查度量口径**。
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取**最后一条**；自由格式
  `PASS —— …` 会 rc=0 却判 FAIL）—— **已踩第三次** → **新脚本先照抄已通过脚本的结尾块。**
- ⚠ 要截图的用例**别用 `MYVK_FRAMES` 控帧** → **等条件出现 + 主动关窗**。
- ⚠⚠ **量测三坑**（细节 → 归档）：① **扫描区间夹在"被测控件自己的矩形"里**；② 比"两来源的数字"先
  **对齐采样窗口**；③ **"点了没反应"先怀疑送达**。
- ⚠⚠ **GUI harness 两类抖动**：① **首帧布局竞态**（分栏根 `ratio=0`）→ 连起重试到 `VP-RECT` 收敛；
  ② **Win32 窗口级联**（不给位置每次右移 38px）→ **显式摆 (0,0) + 光标挪屏幕右下**。
- ⚠ **sRGB 交换链二次编码** → 面板底**上屏实测**亮度 深色 85~132 / 浅色 225~245 → **像素判据阈值按实测定**。
- ⚠⚠ **删功能 = 同时删掉它的锚点和"等它的等待"**，否则留下两类隐性垃圾：
  ① **永真的空断言**（"某矩形不出现"在矩形永远不出现之后白送 PASS）→ 改成查**还活着**的
  同义锚点（如总闸自己的 `VP-OVERLAY master=0`），并把"这东西不存在"归到一个**独占的**
  反向用例里；② **永不到来的 `ready_needle` / `wait_count`** → 每个会话**白等满 timeout**
  （`VP-RECT modebar=` 删模式条后让 `verify_viewport` 每次空转 25 秒）。删断言时**项数会变**
  （本轮 `verify_viewport` 42 → 41），是正常的，不是漏跑。

## 工具（细节 → 归档「编辑器验证工具链」）
- **`run_regression.py`**：读末尾 `结果: PASS`；**增量**。⚠ **新增 `verify_*.py` 必须进 `DEFAULT` + 补
  `TRIGGERS`**；⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`**（很可能先红）→ **改 `TRIGGERS` +
  同步 fixture 是一个原子操作**。铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**（`--narrow`
  下变假跳过）；横切名单**只以 `GLOBAL_GLOBS` 为准**（**没有 `editor/**`**）。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 **`(PIL.Image, ok)` 元组**）。⚠ `kill_stale_editors`
  **无差别**杀 `Editor.exe` → **同时只能有一个 GUI 测试在跑**；截图目录 `<repo>/../_venn_verify`；
  ⚠ **禁止硬编码绝对盘符**。

## 路线图与未解问题（**明细 → 归档**；只留结论 + 下一项）
**渲染 ✅**：R0~R4 · R5-1 反射探针 · R6-1 GPU 粒子（缺 ribbon/mesh/碰撞/预览）。**未做 R5-2/R5-3/R6-2~R6-5**
—— 后 5 项是**游戏运行时 G21~G25 的硬前置**。**基准 = "以 unity 或 blender 为准"。**

**编辑器 ✅**：B（E04~E08）· **C 全部（E09~E17）** · E11.1 图标条 · D（E18/E19/E21）· E25 · 五点需求 +
Inspector Render/Particles/Physics 页（10-16）· **打磨七连（10-17，见路线图 §13）** +
**底栏/Content 抽屉改造（10-17，`editor_roadmap.md` E06 段）**。
⚠ **栅格俯视抖动已修（10-17，`grid.frag`，记在 `editor_roadmap.md` E14 段）**：根因不是采样相位，
是 **1 m 格在 120 m 高空只占 ~4.3 px、线宽 1.4 px = 周期 4px 的高频图案**（量级：机位移 0.3m →
`mean=24.49`、`n>18=221k`）；**TAA 无效**（平均成灰噪点）。修法 = LOD 阈值 `0.9/2.0 → 5.0/12.0`
+ 补 **100 m 超级格**（否则高空整族被收掉 → 地面全白）。复测 24.49 → **7.14**。
⚠ 定抖动/噪声的**唯一可靠对照是"同机位两次启动"**（= 0.000000 才能排除确定性因素）。
⚠⚠ **栅格"虚化/一片看不见"（10-17 晚）= 共面深度冲突，不是 LOD**：判据是"**地板盖住的像素没线、
出了地板边界立刻有线**"。根因：只在**顶点**里做 `clip.z -= 1e-6*clip.w`，而四边形横跨 ±2000 m，
4 km 跨度上的插值误差与 1e-6 同量级。修法：`grid.frag` 里用 `vWorld` 重算 `gl_FragDepth` 再减 **1e-5**，
`grid.vert` 的偏移删掉。⚠ 别用"放大到 1e-3"敷衍（20 m 处等于抬高 4 m，物体穿模）。
**E03 设置层级 ✅（10-17）**：`engine/render/RenderSettingsIO.*` = 5 层覆盖链
（prefs→project→scene→object→camera，读时从最具体回退）+ `config/render.json` 落盘 + 无窗口自检
（`MYVK_RENDER_SETTINGS_TEST=1`）。⚠ 两个坑：① **文件层与 env 层各要一个 applied 标记**，每阶段顺序
`applyToRegistry() → loadEnvOverrides()`（env 必须赢；只用一个标记会把阶段二绑定时的 env 值盖回去）；
② **别按 UI 的 lo/hi 夹文件值** —— `lshadow.bias` 默认 z=1.2 而 UI hi=1.0，一夹"导出→重载"就漂。
⚠ 新增 `engine/render/*` 文件必须进 `RENDER_MODULES`（`check_regression_map` F 组会抓）。
⚠ **下一项 = `E20` 覆盖指示器**（scene/object/camera 三层只通了数据通路，schema 改动属 E20）或 `E26` 画质档位。

**游戏运行时（G 系列，`docs/ship_roadmap.md`，G01~G32 / 7 里程碑）✅ M1 + M2 已交付（10-17）**：
三目标 `VennEngine`(静态库) + `VennEditor`→`Editor.exe` + `VennRuntime`→`Game.exe`；依赖单向
`runtime→engine←editor`；`engine/script/`（namespace **`vks`**）；`runtime/` = GameLoop /
GameState(+Stack) / RuntimeConfig / RuntimeLog / RuntimeApp；**`engine/physics/ScenePhysics.*`
= 场景⇄物理的唯一实现**（编辑器与运行时共用，防手感漂移）。**M2 手感地基 ✅（G06~G10）**：
`core/InputMap` + `physics/CharacterController` + `ContactEvents` + `scene/CameraSystem` +
`runtime/PlayerController|SpringArm`；四个新组件（character/trigger/physicsMaterial/camera）**已序列化**；
**G06 键位重绑定 = Preferences ▸ Bindings 页**（`editor/KeyBindingsPage.*` + `InputMapIO.*`，
落盘 `assets/input_map.json`，运行时可撤销地读同一份）。**下一段 = M3 表现层 `G11 动画 I`。**
⚠ M2 的三个"想当然"坑：① `sweepCapsule` 是**推出式**的（撞斜棱会边滑边退，找落点必须二分）；
② 手柄轴**正负两半**不算冲突（只比 code 会报 8 对假冲突）；③ `core::Input::keyPressed` 在
`WantCaptureKeyboard` 时恒 false → 键位捕获必须直接 `glfwGetKey`。
P0 六项 = 运行时框架/游戏 UI/音频/动画运行时/输入映射/存档（**输入映射已 ✅**）。⚠ **最硬的墙 = Iris Xe `maxBoundDescriptorSets=8`
已用满** → G15 运行时 UI **不能**再要独立 set（动手前先量）；⚠ **蒙皮已在 GPU**（`Renderer.h:52` `binding 6`
jointMatrices SSBO）—— **别再规划"转 GPU 蒙皮"**。

**物理（E-PHYS）✅ v1（10-16）**：`engine/physics`（PGS+热启动+split impulse/SAP/GJK-EPA+SAT/关节 5 种/
岛级休眠）+ `RigidBodyComponent`/`PhysicsSettings` + 序列化；**仅 Play 态**；UI 在 Inspector→Physics；
自检 `MYVK_PHYS_TEST=1`；回归 `verify_physics.py`。

**编辑器 UI 收官（10-17 晚）**：视口工具条重做成**一行 18 控件 + 9 个下拉**
（工具/坐标/四个吸附/视口看法，`ViewportPanel::drawToolbar`），新增 `GizmoMode::Select`；
底栏 CMD 右侧 `+` **面板托盘**（`EditorApp::drawPanelPicker/trayButton`，直接遍历
`panelTable()`，勾选→生带 logo 的按钮、叉号移除、右键最小化）。⚠ 三个容易再踩的：
① `panelOpen`=勾选（落盘）与 `panelTrayHiddenRef`=窗口临时收起（`PanelRegistry`）**必须分开**，
否则"点按钮"会把按钮自己弄没；② 浮动面板要每帧 `BringWindowToDisplayFront`，**否则被视口盖住**
（视口一旦被点中就被 ImGui 提到 `g.Windows` 末尾）；③ ImGui 的 `OpenPopup` 无定位时弹层开在
**鼠标位置**，会被 `SetNextWindowPos(..., Appearing)` 修正。新面板 `PhysicsPanel` / `ParticlesPanel`。
回归 `verify_viewport_bar.py`（57 项）。⚠ **自动化里键盘输入必须 `PostMessage(WM_CHAR)`**，
`keybd_event` 编辑器收不到（扫描码 0 → KEY_UNKNOWN）。

**自检现状**：特性 **34/34 有 UI 面**、裸 env 漏网 **0**；映射表 **58/66**、`DEFAULT` **66 例**。
⚠ 裸 env 白名单：`MYVK_PHYS`/`MYVK_PHYS_TEST`/`MYVK_STARTUP_DELAY`/`MYVK_RENDER_SETTINGS(_TEST)`/
`MYVK_RENDER_PREFS`（同 `MYVK_FRAMES`）、`ALLOW_SUFFIX` 加 `_DUMP`（同 `_DBG`）；
`MYVK_RUNTIME`、`MYVK_FIXED_DT` 在 `ALLOW_EXACT`，`MYVK_RUNTIME_*` 按前缀放行。

### ⚠ 未解 / 已收尾（**明细 → `editor_roadmap.md` §12/§13**）
- ✅ 10-16（46/50 → 4 项）+ 10-17（3 项）全部定案，**都是测试侧改动、产品码基本未动**。留下两条要记的：
  ⚠ `verify_bvh`/`csm`/`ibl` 全量红 = harness flaky（单跑全 PASS）→ **别自动补跑"凑干净"**；
  ⚠ `FX-FREEZE` 日志 "bit-for-bit" 是**过度承诺** → 下次动 `engine/**` 顺手改。
