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
Inspector Render/Particles/Physics 页（10-16）· **打磨七连（10-17，见路线图 §13）**。
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

**自检现状**：特性 **34/34 有 UI 面**、裸 env 漏网 **0**；映射表 **54/62**、`DEFAULT` **62 例**。
⚠ 裸 env 白名单：`MYVK_PHYS`/`MYVK_PHYS_TEST`/`MYVK_STARTUP_DELAY`/`MYVK_RENDER_SETTINGS(_TEST)`/
`MYVK_RENDER_PREFS`（同 `MYVK_FRAMES`）、`ALLOW_SUFFIX` 加 `_DUMP`（同 `_DBG`）；
`MYVK_RUNTIME`、`MYVK_FIXED_DT` 在 `ALLOW_EXACT`，`MYVK_RUNTIME_*` 按前缀放行。

### ⚠ 未解 / 已收尾（**明细 → `editor_roadmap.md` §12/§13**）
- ✅ 10-16（46/50 → 4 项）+ 10-17（3 项）全部定案，**都是测试侧改动、产品码基本未动**。留下两条要记的：
  ⚠ `verify_bvh`/`csm`/`ibl` 全量红 = harness flaky（单跑全 PASS）→ **别自动补跑"凑干净"**；
  ⚠ `FX-FREEZE` 日志 "bit-for-bit" 是**过度承诺** → 下次动 `engine/**` 顺手改。
