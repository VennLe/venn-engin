# venn —— 项目笔记

> 只放 **纪律 + 速查 + 指针**。细则 → `editor_details_archive.md` / `render_details_archive.md`；
> 已完成数字与逐条实测 → `roadmaps.md`；下一步 → `docs/*_roadmap.md`；理由 → `docs/editor_architecture.md`。
> ⚠ **本文件上限 ~16 KB（超过会被整段截断注入）** → 涨了先压「路线图」段、再压细节括号，别新增小节。
> 仓库 `D:\A-VENN\code\cpp\venn-engin`（CMake 名 `Venn`）。C++17 手写 Vulkan，**右手系 Z-up**；
> `core→rhi→render→scene→assets`（另有 `ui`/`ui2`/`i18n`/`ecs`/`physics`/`script`/`audio`；应用层
> `editor`/`runtime`，**engine/ 不认识后两者**）。改名后删 `build/` 重 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
快照**可能被截断/陈旧** → 跑回归 / 批量改文件 / 改契约前先读磁盘。（已因缺条目误启过全量回归。）

## ⚠ 回归纪律（用户明确要求）
**不自动跑全量**：只有用户明确下达才 `--all`，日常只跑受影响用例（按 `TRIGGERS`）。⚠ `--explain`
无基准快照时**一律报全量**（保守兜底）。⚠ GUI 用例**同时只能有一个**（`kill_stale_editors` 无差别
杀 `Editor.exe`/`Game.exe`）。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry`（`bindParam` 在成员默认初始化**之后**、`loadEnvOverrides()`**之前**）
③ **UI 面** ④ 序列化 ⑤ 用例 + `TRIGGERS`。**只有 `MYVK_*`、界面点不到的不算交付**。把关
`check_feature_surface.py`（C/D 清零）。⚠ `FeatureRegistry.*` 在 `GLOBAL_GLOBS`。

## ⚠ 本机环境
**GPU = Intel Iris Xe**，实测 `maxBoundDescriptorSets=32` / `maxPerStageDescSamplers=64` /
`descriptor indexing available`——⚠ **"=8 卡在线上"是错的**，但**低端设备只保证 8** → 加纹理槽仍要
走 **bindless**（`render/Bindless.h`，默认关），不是"加 set"。3D 主通道绑 8 set、UI 独立管线 1 set。
Python 用 `...\envs\default\Scripts\python.exe`；⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（细则 → 归档五）
入口 **`./make.cmd build`**（`TOOLCHAIN=auto` → **MSVC**），`CMAKE_BUILD_TYPE=Debug`（`/Od /Ob0`）
——**性能数字是 Debug 的，别当 Release 读**。⚠ **四坑**：`https_proxy`+`HTTPS_PROXY` 并存 → 假报
`MSB6001`；UTF-8 无 BOM + 中文 → `C2001`（要 `/utf-8`）；`.cmd`/`.bat` **纯 ASCII**；Make 用 sh 跑
配方 → `echo` 里不能有 `(` `)` `|`。⚠ **资产拷贝必须 `add_custom_target(... ALL)`**（`POST_BUILD`
**只在重链后**跑 → "只改资产没改 C++"时静默拿旧文件）。⚠ `strings` 抓不到 ASCII 常量 → 只能靠
**行为**验代码进没进二进制。⚠ 改结构体布局后增量构建报 `LNK1163` → 删那个 `.obj` 重编，不是代码错。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP="near,far"`（默认 `0.1/5000`）；
编辑器侧一律 `MYVK_EDITOR_` 前缀（白名单按前缀放行）。⚠ **落盘读/写不对称**：自动化下默认不读、
显式 `MYVK_EDITOR_LAYOUT` 照读、**写一律拒绝**。⚠⚠ **改"全局默认"要 grep 依赖它的测试**（远平面
200→5000 让基线漂；`kPrefsVersion` 升 2 后 fixture 还写 1 → 存档被丢弃）。**修法是钉输入 / 升
fixture，不是改期望值。**

## 契约速查
- **Pass 序**：`Shadow → LocalShadow → DepthPrePass → SSAO → ClusterBuild → VolumetricFog(×2) →
  Forward → PostProcess → 交换链`。**compute 不能在 RP 内执行**；**栅格夹在不透明与透明之间**、
  **绝不能**做成 ImGui 叠加层。
- **"改一处要同步改 N 处"是常态**：`FrameUBO`(768B)/推常量(128B 全满)/set0(25 binding)/变体位序/
  分簇编址/材质贴图 set(7)/旁路管线 range 须与主管线**逐字段相同**（否则 VUID-08600）。
- ⚠ 两处**默认槽**是雷：DBuffer 缺省必须 `m_defaultTransparent`；探针 binding 23 缺省必须绑 1×1
  全黑 cube array，**不能"不绑"**。**颜色空间**：albedo/emissive `srgb=true`，normal/ORM/高度图
  `false`，着色器内不做手动 gamma。
- ⚠⚠ **音频 ✅**（自研 WASAPI 零依赖）→ `audio_roadmap.md` §13（`--selftest audio` 118 条，无声卡
  也全绿）。要点：**别用 `ma_engine`**（混音锁在库里 → 无窗口测不了）；产品开/自动化关
  （`MYVK_RUNTIME_AUDIO`）；**不进 `AssetManager`**（`AudioClipCache`）；时基来自设备 block 不是
  `dt`；音频线程不分配/不加锁/不打日志。
- **动画/蒙皮**：`jointBase` 三路来源（MDI `aux.x` / 合批与基线 `push.skin.x`）；`invMeshNode` 与
  `push.model` 的 meshNodeWorld **约掉** → 静态与蒙皮同一条 draw。⚠ **采样不许留在 Renderer 里**
  （否则无窗口测不了）→ `character_pipeline_roadmap.md` §3.3。
- ⚠⚠ **材质是共享资产，不是逐物体参数**（`Material*` → `AssetManager` 按名缓存）。"逐物体"是
  **假象**，靠"每实体起唯一材质名"维持。三个实测 bug：①**同名静默合并**（`AssetManager.h:476`
  直接 return 旧对象，后者参数全丢）；②**身份是数组下标**（`…glb/mat0`，重导出即错位）；③**导入
  模型的材质覆盖存盘即丢**（`SceneSerializer.cpp:162-166` 只写 name+modelKey，resolve 失败静默
  null）。参数走 push 常量，纹理走 set 1..7。

## 脚本（**S0~S6 全部 ✅ 收口**；权威 → `docs/script_roadmap.md` §11 台账）
- **`engine/script/ScriptRuntime.{h,cpp}` = 唯一 tick 内核**。`let` **幂等声明**；五钩子编译期收进
  `Program::hooks()`；内建名**编译期**解析成 `enum class Builtin`（拼错 = 编译失败）。相位顺序
  **`声明区 → start → 接触事件 → 顶层 → update`**。
- ⚠⚠ **`ScriptProgram`/`HookBlock` 构造析构必须 out-of-line**（`m_ast` 是 `unique_ptr<Stmt>`，头里
  `= default` → 别的 TU 报 `can't delete an incomplete type`）；头里前向声明的类型**定义必须在
  `namespace vks` 里、匿名命名空间之外**（C2872，踩两次）。
- ⚠ 热重载用 `ScriptAsset::version()` **不用指针**；`on start` **不重跑**。`attr`（逐实体、**不存盘**）
  vs `worldAttr`（`Scene::scriptVars`、**进存档**）分开；配额 `MYVK_VKS_QUOTA`（默认 20000）。
  ⚠⚠ **自检里"第二步重新求值"必须用普通赋值**（`let` 幂等 → 整句跳过，踩两次）。
- **S4 协程**：显式帧栈 + `Stmt::id` + `reattach`。三坑：**先 `++ip` 再压子帧**（反了 = wait 后整条
  if 重跑）；**`Frame&` 在 `push_back` 后悬垂**（字段先拷局部）；`simFrame` 是 **1-based**。
- **S5 编辑器面**：`Instances` 页签（四态四色）+ Inspector   Script 段 + `script.reset/vars/trace`。
  四坑：①`syncInstances` 只在 `tick()` 里调 → **编辑态表永远空**（需在非运行态每帧调 `sync()`）；
  ②`reset()` 每帧调 → **暂停 = 静默重启**（按 `usingRuntime()` + `m_scriptTableDropped` 判）；
  ③**状态日志块在循环末尾 → `continue` 之前的状态变更永远不打**（`state=Failed` 查无此行 → 抽
  `noteState()` 四路共用）；④**一行塞绝对路径把按钮推出窗口**，被裁控件 `GetItemRect*` 照样有值。
- **S6 示例关卡**：`assets/scripts/*.vks` 四个 + `tools/gen_script_demo.py` + `--selftest scriptbench`。
  三坑：①**`on collide` 做不出玩法** —— 编辑器 Play 无 `ContactTracker`，且角色代理是**运动学**体、
  `collectContacts` 跳过"两边都不动" → 用**距离判定**；②**`Player` 朝 +X、弹簧臂在 -X 侧** → 摆 -X
  的东西全在相机背后（日志全对，只有截图看得出）；③`HUD-STATS` 是**节流**日志 → 轮询而非 `sleep`。
- 唯一遗留：**编辑器 Play 还没接 `ContactTracker`**（`on collide/trigger/exit` 只在 `Game.exe` 派发）。

## 编辑器架构（细则 → 归档一/二/三/九）
- **加面板 = 表里加一行**（`panelTableVec()` + `PanelRegistry` → 菜单/快捷键/图标/落盘四路派生）；
  面板间**只准依赖 `EditorContext`**，动引擎走 `EditorHost` 纯虚接口。开关权威在 `PanelRegistry.cpp`
  的 `g_open[kMaxPanels]`（**固定数组 + 引用**，vector 扩容 → `bool&` 悬垂）。
- ⚠ **默认一律"全关"**（`config/render.json` 缺失 → **逐像素等于今天**）。⚠ **快捷键排他**（面板
  `Alt+1..8` / 工作区 `Ctrl+Alt+1..6` / 视图键 `Ctrl+1..8` / `Shift+Space` 在 `Space` 前）；
  **别引入 ImGui docking**（用自研 `SplitLayout`）。
- ⚠ **Inspector 11 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**枚举值 = 图标栏
  顺序**；**落盘写 id 不写下标**。⚠ **`All` 不是融合视图**（旧堆叠顺序一字不改，9 个脚本靠
  `INS-*`）→ 自动化 `All`、交互 `Object`，**别"优化"它**。⚠⚠ **记在"提前返回"之前的锚点只证明
  函数被调用**，**负向断言只能用返回之后的锚点**；图标改完要放大截图肉眼看。
- ⚠⚠ **布局 / 遮挡 / 托盘**：Content 是底栏**抽屉**（`kSlotContent` 恒 `false`）；分隔条靠
  `m_occluders`（**必须排除落在 `LayoutRects` 上的布局面板**，否则整条裁成 0）；"最小化" =
  `panelTrayHiddenRef(i)=1`，**自己无条件绘制的窗口也必须认这个位**；锚点 `BAR-TRAY-<panelId>`；
  **标题栏 × = 收进底栏**。⚠ 树里"随选中出现/消失的一行"会让整棵树跳行；列头必须手画、截断在
  **UTF-8 边界**二分。⚠⚠ **粒子**：单发射器只能画直线/矩形；分叉/辉光必须**多发射器**；倾斜
  **只能用 `tc->rotation`**。

## 断言纪律（细则 → 归档四/九/十）
- ⚠ 必须 `MYVK_EDITOR_NO_HUD=1` + `MYVK_GRID=0`；**采样区落在被测物体上**；抓图走 `capture_nonempty`。
- ⚠ **"状态一致"与"像素一致"是两个断言**；**新断言必须做否定测试**；**临时诊断代码用完必须清**。
  ⚠ **FAIL 先怀疑「期望」再怀疑「实现」**；**兜底条件必须与放行条件同源**；峰值类容差不能比瞬时值
  更紧。⚠ `wait_rect` 读"最后一条"，窗口收起后读到**旧值** → 假通过。
- ⚠⚠ **脚本结尾必须是 `结果: PASS` / `结果: FAIL: ...`**（ASCII 冒号，取最后一条）—— 已踩 **五次**。
- ⚠ 要截图的用例**别用 `MYVK_FRAMES` 控帧** → 等条件出现 + 主动关窗。⚠⚠ **两个 exe 的帧控不一样**：
  `Game.exe` 认 argv `--frames`，**`Editor.exe` 只认 `MYVK_FRAMES`**（`main.cpp` 丢弃 argv）。传
  `--frames` **无提示**，渲染到超时；再叠加 Python **块缓冲** → 外面零日志。⚠ **量测三坑**：扫描
  区间夹在被测控件自己的矩形里 / 比两来源先**对齐采样窗口** / "点了没反应"先怀疑**送达**。
  ⚠⚠ **harness 两类抖动**：首帧布局竞态 → 连起重试到 `VP-RECT` 收敛；Win32 窗口级联 → 显式摆
  (0,0) + 光标挪右下。
- ⚠⚠ **像素断言别用"整图平均差"**：角色占屏 4% → "站着 vs 跪着"也只有 **0.41/255**（阈值 0.5 直接
  假红）。用**显著变化像素占比**（`ImageChops.difference`→直方图，裁到 `VP-RECT`，阈值 8）：占比
  > 0.5% **且 > 10× 基线**。⚠ 同 clip 钉两个时间不够 → **换 clip**。⚠⚠ **否定测试必须带"对照区"**：
  窗口偏 1~2px → 目标区差异冲到 **100%**，断言"通过"却只证明"两张图不一样"。
- ⚠⚠ **Vulkan NDC 的 y 向下**（+1 在屏幕**底部**）：2D/UI 像素 → NDC 写 `y/H*2-1`（3D 的
  `proj[1][1]*=-1` 已翻一次，2D 只剩**零次**）。写反 = 整层 UI **上下镜像**，**日志全对**、只有截图
  看得出。⚠ 命中框输出尺寸必须与 PostProcess **同源**；焦点自测**必须用打乱顺序**。
- ⚠⚠ **`PostMessage` 方向键必须带 `KF_EXTENDED`（lParam bit24）**：不带 → GLFW 把 `0x50` 翻成
  **小键盘 2**，界面不动而日志**毫无痕迹**。"输入没反应"要**同时**断言"目标没动"和"输入确实被吃到"。
- ⚠⚠ **删功能 = 同样删掉它的锚点和"等它的等待"**（否则留下永真空断言与永不到来的 `ready_needle`）。
- ⚠⚠ **矩形锚点的 tag 字符类必须含数字**：`[A-Za-z-]+` 让 `INS-TERRAIN-LAYER0` 解析不出坐标、
  `last_rect` 静默 None。已放宽到 `[A-Za-z0-9_-]+`。
- ⚠⚠ **`has_line` 断言"点了有没有反应"= 永远通过**：同一按钮前面已点过，那条日志早就在。要断言
  "又多了一条" → `wait_count(needle, before+1)`，**计数在点击之前取**。⚠ 同理**节流日志**不能
  `sleep` 后读一把，要轮询"直到看见目标值"。
- ⚠⚠ **底栏抽屉压住 Inspector 下半截**：被裁控件 `GetItemRectMin/Max` **照样有值**，但 `logRect`
  不记、鼠标点在抽屉上（"关可见"像素差 **0.00%**，收起后 **76.29%**）→ 采样前先点 `BAR-BTN-content`。

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新增 `verify_*.py` 必须进 `DEFAULT` + 补
  `TRIGGERS`；⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`**（改表 + **同步自测 fixture** 是
  原子操作）。铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**，横切名单**只以
  `GLOBAL_GLOBS` 为准**（**没有 `editor/**`**）。⚠ **新断言长在哪个文件上，那个文件就得进本用例的
  `TRIGGERS`**，否则改它时断言被静默跳过。
- **`verify_i18n_coverage.py`**（八组）：⚠⚠ **缺译文不报错不崩，只会静静少翻一处**（裸 `snprintf`、
  `SliderFloat(…,"%.1f deg")`、`TR` 漏在辅助函数外）。⚠ **豁免白名单必须整串精确匹配**。⚠ `kZh[]`
  是 `emplace` = **先到先得**（重复键后者永不生效）。⚠ 一词两义靠**让 UI 原文更具体**；状态串要在
  **显示时** TR。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 **`(PIL.Image, ok)` 元组**）；截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**；键盘输入必须 `PostMessage(WM_CHAR)`。
  ⚠ `MYVK_EDITOR_IMPORT`/`MYVK_EDITOR_CONSOLE_CMD` 支持 **`;` 多值**；控制台 `anim.time <秒> [实体]`
  / `anim.clip <名> [实体]`（缺省 = 当前选中）。⚠⚠ **自检日志 `detail=` 里不能有空格**。

## 路线图（权威 → `docs/*_roadmap.md`；**已完成清单与自检数字 → `roadmaps.md`**）
- ⚠ **启动器 → `launcher_roadmap.md`**（定稿未开工；四条前提见 `roadmaps.md`）。
- **脚本 S0~S6 ✅ 全部收口**（`--selftest script` 85 + `verify_script_editor` 21 + `verify_script` 23）。
  **唯一遗留 = 编辑器 Play 接 `ContactTracker`**。
- **世界规模** W0~W4 ✅ / **下一项 W5 植被**（独立顶点布局 + `foliage.vert` 风场 + `FoliageVolume`
  撒点）。⚠ **物体数不是性能瓶颈**（500 盒子 60 帧 2.60 s，GPU 4.7 ms）。⚠ **地形高度场没接物理**
  （`runtime/`、`editor/` 搜不到 `Heightfield`）→ 别把角色放到 `terrain/hills`。
- **渲染** R0~R4·R5-1·R6-1 ✅（未做 → W5~W9）· **编辑器** B·C·E11.1·D·E25 ✅ / 下一项 E20/E26 ·
  **运行时** M1+M2 ✅ · **物理** v1 ✅（仅 Play 态）· **音频** ✅ · **UI/HUD** U0~U8 ✅。
- **角色链** A0 ✅ / 下一项 **A1 Animator**（→A2 状态机→A3 层→A4 根运动→A5 事件→A6 IK→A7 面板；
  唯一装配入口 `scene/ModelInstance`）。
- **存档/Prefab** G16~G19 未开工 / 下一项 G16-1（`World::Record.guid`；前置 G16-5 拆 `sceneFromJson`）。
- ⚠ flaky：`verify_bvh`/`csm`/`ibl`/`particles` 偶发全红（单跑全 PASS）→ **别补跑"凑干净"**。
