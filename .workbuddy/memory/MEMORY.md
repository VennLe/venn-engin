# venn —— 项目笔记

> 只放 **纪律 + 速查 + 指针**。细则 → `editor_details_archive.md` / `render_details_archive.md`；
> 全文 → `memory_full_2026-10-19.md`；数字 → `roadmaps.md`；下一步 → `docs/*_roadmap.md`。
> ⚠ **上限 ~13 KB（超了会被整段截断注入）** → 只增纪律，涨了先删"已完成"细节。
> 仓库 `D:\A-VENN\code\cpp\venn-engin`（CMake 名 `Venn`）。C++17 手写 Vulkan，**右手系 Z-up**；
> `core→rhi→render→scene→assets`（另有 `ui`/`ui2`/`i18n`/`ecs`/`physics`/`script`/`audio`/`project`；
> 应用层 `launcher`/`editor`/`runtime`，**engine/ 不认识后三个**）。改名后删 `build/` 重 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
快照**可能被截断/陈旧** → 跑回归 / 批量改文件 / 改契约前先读磁盘。
⚠ `docs/` 里的历史数字**要标"哪一轮的口径"**（不删，但不标就会被当基准）。

## ⚠ 回归纪律（用户明确要求）
**不自动跑全量**：只有用户明确下达才 `--all`，日常只跑受影响用例（按 `TRIGGERS`）。
⚠ `--explain` 无基准快照时**一律报全量**。⚠ GUI 用例**同时只能有一个**。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry`（`bindParam` 在成员默认初始化**之后**、`loadEnvOverrides()`**之前**）
③ **UI 面** ④ 序列化 ⑤ 用例 + `TRIGGERS`。**只有 `MYVK_*`、界面点不到的不算交付**。把关 `check_feature_surface.py`（C/D 清零）。

## ⚠ 本机环境
**GPU = Intel Iris Xe**：`maxBoundDescriptorSets=32` / `maxPerStageDescSamplers=64` / **descriptor
indexing 可用** —— 但**低端设备只保证 8** → 加纹理槽走 **bindless**（`render/Bindless.h`，默认关），
不是"加 set"。Python 用 `...\envs\default\Scripts\python.exe`；⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（细则 → 归档五）
入口 **`./make.cmd build`**（`TOOLCHAIN=auto` → **MSVC**，`Debug` → **性能数字是 Debug 的**）。
⚠ **四坑**：`https_proxy`+`HTTPS_PROXY` 并存 → 假报 `MSB6001`；UTF-8 无 BOM + 中文 → `C2001`
（要 `/utf-8`）；`.cmd`/`.bat` **纯 ASCII**；Make 用 sh 跑配方 → `echo` 里不能有 `(` `)` `|`。
⚠ 资产拷贝必须 `add_custom_target(... ALL)`（`POST_BUILD` **只在重链后**跑），**多个 target 拷同一
目录会并发冲突** → 全局单例 target。增量构建报 `LNK1163` → 删那个 `.obj`，不是代码错。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP="near,far"`（默认 `0.1/5000`）；
编辑器侧一律 `MYVK_EDITOR_` 前缀（白名单按前缀放行）。⚠ **落盘读/写不对称**：自动化下默认不读、
显式 `MYVK_EDITOR_LAYOUT` 照读、**写一律拒绝**。⚠⚠ **改"全局默认"要 grep 依赖它的测试**
（远平面 200→5000 让基线整体漂）。**修法是钉输入 / 升 fixture，不是改期望值。**

## 契约速查
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
## 脚本（S0~S6 ✅；权威 → `docs/script_roadmap.md` §11）
- **`engine/script/ScriptRuntime.{h,cpp}` = 唯一 tick 内核**。`let` **幂等声明**；内建名**编译期**
  解析；相位 **`声明区 → start → 接触事件 → 顶层 → update`**。
- ⚠⚠ **`ScriptProgram`/`HookBlock` 构造析构必须 out-of-line**（头里 `= default` → 别的 TU 报
  `can't delete an incomplete type`）；头里前向声明的类型**定义必须在 `namespace vks` 里**（C2872）。
- ⚠ 热重载用 `ScriptAsset::version()` **不用指针**；`on start` **不重跑**；自检里"第二步重新求值"
  必须用普通赋值（`let` 幂等 → 整句跳过）。**S4 协程**：先 `++ip` 再压子帧 / `Frame&` 在
  `push_back` 后悬垂 / `simFrame` 是 **1-based**。
- ⚠⚠ **编辑态脚本不 tick** → `progVersion` 恒 0 → `VKS-RELOAD-*` 一条都不打。
  **测热重载必须先按 Space 进 Play**。遗留：**编辑器 Play 没接 `ContactTracker`**（只有 `Game.exe` 派发）。

## 编辑器架构（细则 → 归档一/二/三/九）
- **加面板 = 表里加一行**（`panelTableVec()` + `PanelRegistry` → 菜单/快捷键/图标/落盘四路派生）；
  面板间**只准依赖 `EditorContext`**，动引擎走 `EditorHost`。开关权威是 `PanelRegistry.cpp` 的
  `g_open[kMaxPanels]`（**固定数组 + 引用**，vector 扩容 → `bool&` 悬垂）。
- ⚠ **默认一律"全关"**（`config/render.json` 缺失 → **逐像素等于今天**）。⚠ **快捷键排他**（面板
  `Alt+1..8` / 工作区 `Ctrl+Alt+1..6` / 视图键 `Ctrl+1..8`）；**别引入 ImGui docking**（自研 `SplitLayout`）。
- ⚠ **Inspector 11 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**枚举值 = 图标栏顺序**；
  **落盘写 id 不写下标**。⚠ **`All` 不是融合视图**（9 个脚本靠 `INS-*`）→ 自动化 `All`、交互 `Object`。
- ⚠⚠ **布局/遮挡/托盘**：Content 是底栏**抽屉**；分隔条靠 `m_occluders`（**必须排除落在
  `LayoutRects` 上的布局面板**）；"最小化" = `panelTrayHiddenRef(i)=1`，**自己无条件绘制的窗口也得认**；
  **标题栏 × = 收进底栏**。⚠ 树里"随选中出现/消失的一行"会让整棵树跳行。⚠⚠ **粒子**：单发射器
  只能画直线/矩形；分叉/辉光必须**多发射器**。

## 断言纪律（细则 → 归档四/九/十）
- ⚠ 必须 `MYVK_EDITOR_NO_HUD=1` + `MYVK_GRID=0`；**采样区落在被测物体上**；**面板会盖住被测物**
  → 先收进托盘 / 先点 `BAR-BTN-content`（被裁控件矩形**照样有值**，但点下去点到抽屉）。
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取最后一条）—— 已踩 **五次**。
- ⚠ **"状态一致"与"像素一致"是两个断言**；**新断言必须做否定测试**且**带"对照区"**；
  ⚠⚠ **落点必须是"两种实现会不一样的那个量"** —— 断言打在"对方也懒得写"的字段上，
  两个世界都绿（实测 A1 `animator_paused` 打在 `anim.time`，系统无视 playing 也照样过）；
  **临时诊断代码用完必须清**。**FAIL 先怀疑「期望」再怀疑「实现」**；**兜底条件必须
  与放行条件同源**。⚠ 锚点拼错 = **断言永远为假**（静默 None）；`wait_rect` 读"最后一条" → 窗口收起
  后读到**旧值** → 假通过。- ⚠⚠ **`has_line` 证明不了"点了有反应"** → `wait_count(needle, before+1)`，**计数在点击之前取**。
  ⚠ **节流日志**不能 `sleep` 后读一把，要轮询。
- ⚠⚠ **像素断言别用"整图平均差"**：角色占屏 4% → "站着 vs 跪着"只有 **0.41/255**。用**显著变化像素
  占比**（裁到 `VP-RECT`，阈值 8）：> 0.5% **且 > 10× 基线**。⚠ 同 clip 钉两个时间不够 → **换 clip**。
  ⚠ 同配置跑两次的**控制上限**写 0.1~0.3%（TAA 抖动），**不能写 0**。
  ⚠⚠ **钉"主色是谁"而不是"某色占多少"**：差分包围盒会随布局缩放翻倍（实测 95x25→197x52），
  占比型阈值当场红而画面完全正确；**"某状态色占 N%"等于把"控件此刻是否被悬停"判成 bug**。
  ⚠⚠ **代理量会饱和**：平均差在位移超纹理格宽后撞顶（pom 45° 比 80° 还大）→ 换直接量或只断言单调。
  ⚠⚠ **绝对容差不能用在计数型瞬时量上**（particles `alive` 跨运行差 1/5/10 随基数漂）→ `max(绝对值, 5%)`。
- ⚠⚠ **要截图的用例别用 `MYVK_FRAMES`**；**`Game.exe` 认 argv `--frames`，`Editor.exe` 只认
  `MYVK_FRAMES`**（`main.cpp` 丢 argv）。传错**无任何提示**，渲染到超时；叠加 Python **块缓冲** → 外面零日志。
- ⚠⚠ **`PostMessage` 方向键必须带 `KF_EXTENDED`**（不带 → GLFW 翻成小键盘 2）；⚠ **`ctypes` 必须给
  `PostMessageW`/`GetWindowTextW`/`OpenProcess` 声明 `argtypes`**（64 位 HWND 截断 → 静默失败）。
  "输入没反应"要**同时**断言"目标没动"和"输入确实被吃到了"。
- ⚠⚠ **Vulkan NDC 的 y 向下**：2D/UI 像素 → NDC 写 `y/H*2-1`（3D 已由 `proj[1][1]*=-1` 翻过一次，
  2D 只剩**零次**）。写反 = 整层 UI **上下镜像**，**日志全对**、只有截图看得出。
- ⚠⚠ **矩形锚点 tag 的字符类必须含数字**（`[A-Za-z0-9_-]+`），否则 `INS-…-LAYER0` 解析不出坐标。
- ⚠⚠ **删功能 = 同时删掉它的锚点和"等它的等待"**（否则留下永真空断言）。
- ⚠⚠ **harness 抖动三类**：首帧布局竞态 → 连起重试到 `VP-RECT` 收敛；Win32 窗口级联 → **显式
  摆 (0,0) + 光标挪右下**；⚠⚠ **真实点击要求窗口在最前**（不在最前第一下只"激活窗口"、进不了
  ImGui，日志一个字都没有）→ `click()` 前 `focus_window()`。⚠ **量测四坑**：扫描区间夹在被测
  控件矩形里 / 比两来源先**对齐采样窗口** / "点了没反应"先怀疑**送达** / 一行排 N 个按钮会超宽。
- ⚠⚠ **`Popen(stdout=PIPE)` 必须有人排空**（64 KB 管道满即死锁 → "窗口不出现"）。
- ⚠⚠ **`shutil.rmtree` 静默失败 = 全绿但一个字没验**（读到上一轮旧文件）→ `fresh_dir()` 删不掉**绝不
  降级复用**。⚠ 宿主有**批量删除保护**（按回合累计，阈值 50 文件）→ 一回合连跑三个 GUI 用例必撞。

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新 `verify_*.py` 必须进 `DEFAULT` + `TRIGGERS`；
  ⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`**（改表 + **同步自检 fixture** 是原子操作）。
  铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**（名单只以 `GLOBAL_GLOBS` 为准）。
  ⚠ **新断言长在哪个文件上，那个文件就得进本用例的 `TRIGGERS`**。
- **`verify_i18n_coverage.py`**（A1~A4/B/B2/C/D/E/F）：⚠⚠ **缺译文不报错不崩，只会静静少翻一处**
  （裸 `snprintf`、`SliderFloat(…,"%.1f deg")`、`TR` 漏在辅助函数外，三门盲区都实测栽过）。
  ⚠ **豁免白名单必须整串精确匹配**。⚠ `kZh[]` 是 `emplace` = **先到先得**。⚠ 带 `##` 的 ImGui id
  后缀在译文里必须原样保留。⚠ 一词两义靠**让 UI 原文更具体**。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 **`(PIL.Image, ok)` 元组**）；截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**；键盘必须 `PostMessage(WM_CHAR)`。
  ⚠⚠ **自检日志 `detail=` 里不能有空格**。
- ⚠⚠ **`os.remove`/`shutil.rmtree` 会被宿主按回合累计硬杀**（阈值 50 文件，**进程层终止、
  `try/except OSError` 接不住**，日志停在删除那行、上面全 `[ OK ]`）→ 一律用 **`tools/safe_fs.py`**
  的 `safe_remove`/`safe_rmtree`（rename 不计数）。⚠ 让位必须进**同级 `.trash/` 子目录**，
  **不能原地改名**（`render.json.trash-123` 会撞 `verify_settings_layers` 的"安装目录无 render* 残留"）。
- ⚠⚠ **词典会长、SDF 图集是静态产物** → 加词条后 `verify_ui2` 的"词典字符都在图集里"会红，
  重跑 `gen_sdf_atlas.py --atlas 2048 --atlas-h 3072`（**默认 2048² 装不下**）。

## 路线图（权威 → `docs/*_roadmap.md`；已完成数字 → `roadmaps.md`）
- 完成：启动器 M0~M6(缺 M5-c)·世界 W0~W4·渲染 R0~R4/R5-1/R6-1·编辑器 B/C/D/E11.1/E25/E26·
  运行时 M1+M2·物理 v1(仅 Play)·音频·UI/HUD U0~U8·脚本 S0~S6。
- 下一项：**A2 状态机**（A1 ✅）·世界 **W5 植被**·编辑器 E20/E22·渲染 R6-2 水体·**G16-1**。
  ⚠ **anim/sk 双份字段的权威规则**见 `roadmaps.md`。
- ⚠ flaky：`bvh`/`csm`/`ibl` 偶发全红（单跑 PASS）→ **别补跑"凑干净"**。
  **10-19 的 5 个红已全修**（camfx/taa/pom = 期望过期；particles = 交互+容差；
  notifications = **Console 环形缓冲真缺陷**）→ 判据见 `ship_roadmap.md`「全量回归基线」。
