# venn —— 项目笔记

> 只放 **纪律 + 速查 + 指针**：细则 → `*_details_archive.md`（三册）/ 流水 → `YYYY-MM-DD.md` /
> 数字 → `roadmaps.md`。⚠ **14.5 KB 以上注入会被截尾** → "做完的体系"判据已拆到
> **`FEATURES.md`**（碰某体系前先读那一节）。仓库 `D:\A-VENN\code\cpp\venn-engin`（CMake `Venn`）。C++17 手写 Vulkan，**右手系 Z-up**；
> `core→rhi→render→scene→assets`（另有 `ui`/`i18n`/`ecs`/`physics`/`script`/`audio`/`project`；
> 应用层 `launcher`/`editor`/`runtime`，**engine/ 不认识后三个**）。改名后删 `build/` 重 configure。

## 已完成体系的判据 → `FEATURES.md`（11-01 拆出，必读）
- 着色 A1~A6 + 动画 B1~B3 · 过场时间轴 C1 · 相机运镜 CINE + 多机位 · 序列化/存档 G16·G17 ·
  关节 J5 · 对话 C2 · 步态 A4·A4b · **表情 B6** · 脚本/编辑器 · 二次元 UI 主题 · 界面语言默认。
- ⚠ 拆分原因：超 14.5 KB 会**截尾注入**，"做完的"挪出去。

## ⚠⚠ 第 0 条：动手前重读磁盘版
快照**可能截断/陈旧** → 跑回归/改契约前先读磁盘；`docs/` 历史数字**要标口径**。⚠⚠ **先 stat 二进制 vs
源码 mtime**（10-23：`build/` 没跟 → 跑着旧 exe，把已实现的需求原样重提）。

## ⚠ 回归纪律（用户明确要求）
**不自动跑全量**：只有用户明确下达才 `--all`，日常只跑受影响用例（`TRIGGERS`）。
⚠ `--explain` 无基准快照时**一律报全量**。⚠ GUI 用例一次一个。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry`（`bindParam` 在成员默认初始化**之后**、`loadEnvOverrides()`**之前**）
③ **UI 面** ④ 序列化 ⑤ 用例 + `TRIGGERS`。**界面点不到 = 没交付**。

## ⚠ 本机环境
**GPU = Intel Iris Xe**（`maxBoundDescriptorSets=32`；低端只保 8）→ 加纹理槽走
**bindless**（`render/Bindless.h`，默认关）。Python 用 envs\default\Scripts\python.exe；
⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（细则 → 归档十一 A）
`./make.cmd build`（`TOOLCHAIN=auto` → **MSVC**；`Debug` → **性能数字是 Debug 的**）。
⚠⚠ **前向声明必须与定义同 struct/class**（TU 首次声明 → `LNK2019`；**C4099 是前兆**）。
⚠⚠ **新加资产/新目录不会自动进 `build/bin/`** →「加了完全没效果」先查这条（含 CMake 拷贝档）。
⚠⚠ **编译代价看扇出**：`Components.h`（75 TU）**67.8 s** vs `ScriptEngine.cpp`（3888 行/6 TU）
**15.4 s** → 玩法代码写进 `engine/` = 「改一行泡杯茶」。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP`，一律 `MYVK_EDITOR_` 前缀。
⚠ **落盘读/写不对称**：自动跑默认不读、`MYVK_EDITOR_LAYOUT` 照读、**写一律拒**。
⚠⚠ **改"全局默认"要 grep 依赖它的测试**（钉输入/升 fixture）。

## 契约速查（原文 → 归档十一 B）
- **Pass 序**：`Shadow → LocalShadow → DepthPrePass → SSAO → ClusterBuild → VolumetricFog(×2) →
  Forward → PostProcess → 交换链`。**compute 不能在 RP 内执行**；栅格夹在不透明与透明之间、
  **绝不能**做成 ImGui 叠加层。
- **改一处同步改 N 处**：`FrameUBO` 768B/推常量 128B/set0 25 binding/分簇编址/旁路管线 range 与主管线
  **逐字段相同**（VUID-08600）。
- ⚠⚠ 三处"看着能省"的雷：**默认槽不能空**（DBuffer 必须 `m_defaultTransparent`；探针 binding 23 绑 1×1
  全黑 cube array）；**材质按名共享缓存**（父子的材质覆盖存盘即丢）。
- ⚠⚠ **水体没有网格** → 不在绘制列表/BVH（`WaterPass` 只看 `WaterBody`）。⚠ **出生点 = `GameStartComponent`**
  （不挂网格）→ 点选/落地要单独开口。

## 渲染资源生命周期（权威 → `docs/render_resource_lifetime.md`）
**铁律：一条 CB 在 recording 时引用过的对象都不能销毁** —— 违反 = `invalid state` 刷屏 → submit -4；
**只读第一条**。`syncTerrains/syncWaters` **只对主场景**（`offscreenSlot<0`，**别用 `resetSkinning`**）。
另一 -4 来路：**逐帧重建 → 单次提交超 TDR 2 秒**。

## 断言纪律/工具（细则 → `verify_details_archive.md` §一~§十）
- ⚠⚠ 结尾 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号）；落点 = "两种实现会不一样的那个量"；新断言必做
  **否定测试** + **对照区**；**FAIL 先怀疑期望**；否定测试**没红也是结论**（先查日志是否与执行同源）。
- ⚠⚠ 像素判据用**显著变化像素占比**（非整图平均差）；**先确认被测物在采样区**；测形状关 CSM+TAA。
- ⚠⚠ 合成输入一律**先预热**（两次 MOVE + 0.35s，否则 `MousePos=-FLT_MAX`）：点菜单用 `mouse_event`、
  P8 接管要 `force_foreground`、lParam 带**扫描码**、**键位从日志读**、**先点视口才进操作态**。
- ⚠⚠ `logRect` **不判可见性** → 一律 `kit.click_in_panel`（先滚再点）；锚点**无条件**且放"选中之后"；
  裁切**不含边界**；**/RTC1 越界 = `#32770` 断言框**；日志计数**带后缀**；新 Inspector 段先问
  "**哪个页签、要不要 `MYVK_EDITOR_SELECT`**"（`All` 不画 animation 段）。⚠ **锚点中间插字段 = 静默失配**
  （新字段用**非捕获组**，编号不许动）。
- **`run_regression.py`**：新 `verify_*.py` 进 `DEFAULT`+`TRIGGERS`，⚠⚠ **改完必跑 `check_regression_map.py`**；
  **没登记 = 全量 + 点名**；横切文件**别写进 `TRIGGERS`**；帧数是 `--frames`；⚠⚠ **没打 `结果:` 行 = 全绿判红**。
- **`verify_i18n_coverage.py`**：⚠⚠ 缺译文**不报错**只静静少翻；特性名走 `FeatureDesc.name`；格式串包 `TR()`；
  契约行进 `FMT_KEEP`。⚠⚠ **D 组判据 = 剥掉 `%d/%s` 后还剩 ≥2 个字母** → **"自拼字符串避词表"绕不过它**
  （`"F %d  %.2fs"` 剩 F+s 就红）：屏幕上看得见就得真翻；只有**日志锚点**（脚本按它解析）能标 `// i18n-keep`。
  ⚠ `capture_client` 返 `(Image, ok)`；`--scene` 打不开**不报错** → 夹具落 `bin/`。
- ⚠⚠ **判据重标定**：先分**预热**（起手 100 ms = `maxFrameTime` 钳值）；用 **p90** 不用 p95；
  "玩法卡顿 vs 环境"用**静止 vs 移动**对照。**细则 → 归档十**。

## 资产链 + 外部模型 → 角色（原文 → 归档十一 D；权威 → docs/fbx_asset_pipeline.md 等）
- ⚠⚠ **新增顶层目录必须进 `WATCH_ROOTS`**；⚠⚠ **"写了容错分支" ≠ "容错生效"** —— 上游会 `throw` 的分支
  跑不到 → 贴图缺失必须走 `tryLoadTexture()`；造"缺贴图"现场要改路径下标 + **重算 MAT CRC32**。
- ⚠⚠ **`.vmesh` 传给 `loadFBX` → 第二次起永远失败**（`.vmeta` 判过期后回退解析 .vmesh 抛异常）。
- ⚠⚠ **`PostMessage` 的 Shift 会被 GLFW 撤销** → 修饰键必须 `keybd_event` + 先 `SetForegroundWindow`；
  **`desc.maxSpeed` 截断冲刺** → 上限留余量。世界 = 米 / Z-up，玩家位置语义是**脚** → 挂模型 `lift = -boundsMin.z`；
  玩家实体必须挂 `CameraComponent`；`InputMap` **存档优先** → 老工程要"恢复默认"。
- P6 跳跃 v₀ 3.70 / `airControl 0.45`，**Play 手写同一条抛物线 → 改一处改两处**。

## 出包 G29/G30（→ `docs/package_roadmap.md`；判据 `verify_package` 76 + `verify_shipping` 39）
- `--package` / `File ▸ Package` → `dist/<Name>/`；`--pak` = `game.pak`（assets+shaders，自带 LZ4）。
- ⚠⚠ **引擎侧资产（shaders/fonts/ui/LUT）在 exeDir、不在工程里** → 打包必带：漏了 = 24 条 VUID /
  字体断言卡死，**开发机复现不了**。
- **G30 ✅**：`make ship` → `build-ship/bin/Game.exe`（开发命令**编译期**剔除）。

## 性能基线 P1（11-01 ✅；表 → `docs/perf_baseline.md`；采集 → `tools/bench_baseline.py`）
- `make perf-build` → **`build-rel/`**，与回归的 Debug `build/` **分开**（否则"断言"与"帧率"互相污染）。
- ⚠⚠ 量法：丢预热 · **中位数** · 取 `GPU-TIMING` 不用 `1/fps` · `MAXFPS=0` · `--frames 900`。
  ⚠⚠ `forward` 占 **50%** 且随像素线性涨（填充率）；**Debug↔Rel 只差 8%** → 别拿 Debug 判退步。

## 选中描边 + 底部动画坞（11-05 ✅；权威 → `docs/anim_dock_and_selection_outline_2026-11-05.md`）
- 选中高光 = **几何缓冲 a 取负**（`depth_only.frag` 读 `push.pbr.x`）+ 后处理 8 邻域扩张。
  ⚠⚠ **反向外壳不成立**：硬边网格法线逐面分裂 → 描边裂成虚线；薄片网格没有背面 → 一条边都没有。
  ⚠ `geoEdge` 必须 `abs(a)`（不取 = 选中物被当"深度为负"→ 整片黑）；深度预通道推常量阶段位扩到
  `VERTEX|FRAGMENT` 后，**每个** `vkCmdPushConstants` 的 stageFlags 都得跟。只在 Pick 模式喂 id。
- 动画坞 = 六张面板（dope/curveed/bonecurv/nla/timelineed/cine）的**壳**：内容靠 `pushDockHint`，
  面板代码不动（NLA 补了 applyDockHint）。⚠⚠ 坞和 Content 抽屉的高度必须并进**同一次** `setBottomInset`
  （分两次 = 后一次覆盖前一次，坞压在主体上）；⚠ 六张必须从 `isTabHosted` 摘掉，否则点开就顶掉视口；
  ⚠ `drawPanels` 要 `continue` 掉（cine 有真 draw 指针，不跳会画两遍）。
  ⚠⚠ **面板搬进矮条 = 所有"至少这么高"的硬下限都变溢出**（dope `max(...,200)`、曲线
  `avail-5.2行` 重复预留 + 180/160 下限）：先保"不出窗"再谈舒适值。⚠ 条高/宽**必须跟字号算**
  （写死 30/22 在本机字号 22 下把标签文字齐刷刷切一半）。⚠ **日志 dedup 键要含 px/py**
  （锚点被当坐标用 → 不带坐标就永远读到上一帧的矩形，看着像"画歪了"）。

## 路线图（剩什么 → `docs/remaining_work_2026-10-27.md`；权威 → `docs/*_roadmap.md`）
- 已做完的**全清单**只维护在 `remaining_work` §1（本页不重复）；近期的：出包 G29/G30·主题 E28/E29·
  J5 关节·Prefab/多选/UI 编辑器·植被两轮·步态 A4·运镜 CINE+R7·B6 表情+P1 基线·Lua 后端+M5-c·P3-3。
- **P3 卡牌专供**：P3-3 ✅；剩 **P3-1 配表进运行时（cards.csv → `CardDef` + `CardResolver`）**、
  P3-2 `ui2` 拖拽/Grid、P3-4 伤害数字层。⚠ 卡牌**只存 defId 不存定义**（与状态相反），别"统一"成一处。
- **剩余硬缺口：流送 / 导航 AI**；打磨型仍有 立绘 C2 / 脚 IK B4 / 弹簧骨 B5 / lightmap R5-2 /
  植被 W5 / 音素级口型 D2。
- **面板/操作缺口 → `docs/panel_gap_review_2026-11-02.md`**（10-27 两份已过期）。⚠ 先答"能力→
  入口的差集"再答"真没有"（多数是"实现了但只对引擎作者可见"，editor 0 命中）；最贵四条：
  PostVolume 面板 · 音频混音台 · UI 事件回调 · Project Settings —— 都不动渲染管线。
- ⚠ 材质位掩码到 **bit26**，只剩 5 位 → 加旋钮要换两个 u32。
- ⚠ flaky：`bvh`/`csm`/`ibl`/`particles` 偶发全红 → 别补跑"凑干净"；"闸门红"先分 真漏/记账/夹具陈旧。
- ⚠⚠ `verify_outline` 的 **determinism / strength_zero_is_baseline 已长期红**（11-05 用 `git stash -u`
  在 HEAD 上复现：max=89）。根因是运行时**角色姿势 + 相机弹簧的落点**逐次不同（差值恰好只在角色那一块，
  天空行全 0），与描边实现无关 —— 别把它当本仓描边的回归。

## ⚠⚠ 可上手性（`docs/usability_2026-11-03.md`）—— 别再当"缺功能"报
- 判据：374 env > 137 命令 > 47 面板（默认开 3）> 4 模板；验收 **41%**。能力顺序与人能用的
  **正相反** —— **症结不在能力，在"谁被当成用户"** → **别再提议加面板**。
- 最便宜三补丁：**任务坐标文档 + 首次运行向导 + 面板按任务分组**；硬伤：**`Play` 一词三语义**。

## 代码环境 / 语言选型（→ `docs/code_env_2026-11-03.md`）
- **已选 C++ + Lua**：判据「**Lua 做决策 / C++ 做执行**」（→ `docs/lua_cpp_split.md`）。
  ⚠ 旧 `.vks` 天花板是**结构性**的（无 Function/Return）；**换语言涨不了覆盖率**（绑定表人写），
  C++ 缺的是**分层**（→ `game/GameModule`，**不做 DLL 热重载**：崩溃带走编辑器 + 毁确定性）。

## Lua 后端（11-03 ✅ B/C/C+；权威 → `docs/script_framework.md`）
- 判据：`--selftest lua` **44/44**（A5/A 白名单/B 编译/C 执行/D 协程/E 熔断/F 热重载/
  G **106 全调**/H 时钟/I 组件写入/**J 句柄·实参**）。`lua_static` **跟随工程配置，不调开关**。
- **106 函数 / 12 命名空间**。表驱动：改 `ApiTable.cpp` + `LuaBackend.cpp` 的 `kImpls[]` **两处**；
  缺实现启动即报 `LUA-API missing`。文档 `script_api.md` + `script_api_reference.md`（api 自检生成）。
  ⚠ `.vks` 的 `kBuiltins` **不是**从表生成（按位置对齐 `Builtin`）。
- **组件矩阵 → `script_framework.md` §2.2**（47 组件：✅15 ◐10 ✕18 🚧4）。
- ⚠⚠⚠ **`lua_resume` 第 4 参数不能传 `nullptr`**（`ldo.c:863` 无条件写 → `0xC0000005`；
  特征："`pcall` 正常、一 resume 就崩"）→ 细则 §9.1。⚠ `/RTC-` **不是合法选项**（`D9002`）。
- ⚠⚠ **`luaL_error` 不支持 `%u`**（报错本身报错）；已换 `%d`。
- ⚠⚠ **实体名两处存储**（`Record::name` 给 `find`；`NameComponent` 给序列化）→
  只写组件**不生效也不报错**，必须走 `World::setName`。
- ⚠⚠ **动画权威字段双组件**：`AnimatorSystem` 是 `an ? an->time : sk.time` →
  `anim.*` 必须两个都认（`AnimFields`），只认 Animator 会"能 play 却 pause 报错"。
  `anim.time` 跳帧**必须同步 `eventPrev`**（事件区间半开 `(prev,time]`，否则一次性全炸）。
- ⚠⚠ **API 名别撞 Lua 保留字**：`nav.goto` 语法错 → `nav.moveTo`。
- ⚠⚠ **`engine.dt/time/frame` 曾是死字段** → 已修 + `H1`。**"API 存在但没用"不报错不崩，
  比崩溃难发现**。同理**钩子名大小写**（`beginPlay` 合法但永不调用）→ 现打 `LUA-NOHOOK`。
- ⚠⚠ **chunk 的 `_ENV` 不能直接用 `_G`**（裸赋值全局共享 + `world=nil` 抹掉 API 表）
  → 每脚本一张 `{__index=_G}`。⚠ 自检读变量写 `self.x`（`snapshotVars` 只读实例 env）。
- ⚠⚠ **物理是构建期一次性烘焙**（`bodyType/useGravity/solid/Trigger.enabled` 写进刚体后
  无人再读组件）→ **故意不开放** `actor.setKinematic/setGravity/trigger`；要开先给
  `PhysicsWorld` 加 `setMotionType/setGravityFactor/setSensor`。
- ⚠⚠ **句柄 = `((gen<<32)|id)+1`，`0` 是唯一空句柄**（编码统一在 `ecs/EntityHandle.h`，
  Lua 与 C++ 模块共用）。⚠ **取句柄一律 `aHandle()` 不用 `aInt()`**（`int` 截掉
  generation → **只在槽位复用后才错**）。实参个数由 `dispatch` **按表核**；熔断钩子提到
  文件级（`luaL_error` 是 longjmp，**不能 RAII**）。`camera.fov` 遇 `focal>0` → 写清 focal。
- ⚠ 判据：**I 组从 C++ 读组件**、**J 组造"槽位复用"现场**（写错字段/单位/句柄截断都不报错）；
  空句柄两半**分开断言**（J7/J8），J8 取值**前置**。

## 游戏 C++ 模块 / VS 工程（11-03 ✅ M5-c；权威 → `docs/game_code_project.md`）
- 建工程勾一项 → `code/` 一个独立 VS 工程（`.sln`+`.vcxproj` 直写，**不用 cmake**），自动开 VS。
  判据：`--selftest project` GC 48 + CREATE 54；`--selftest game` 12（需 `MYVK_GAME_DLL`）。
- ⚠⚠ **DLL 绝不链接引擎**（STATIC → 全局对象两份）；`OutDir` 必须 `$(ProjectDir)bin\`（否则落 `bin/Debug/`
  → 编成功、游戏里没反应）。⚠ **`CreateProcessW` 传环境块必带 `CREATE_UNICODE_ENVIRONMENT`**（不带就 87）；
  **MSBuild 环境变量大小写重复 → 报成 `MSB6001` 开关无效**。

## 视口选择方式（11-04 ✅ → `docs/viewport_selection.md`）
- 入口 = **工具条左上箭头**（Selection Mode 四行 + 变换工具）；Tab 圆盘第二入口；左栏只留说明卡
  —— **别再往左栏加第二份单选**（两处不同步 = "没切过去"）。
- ⚠⚠ 只选"物体"：`isSelectableObject()`（有 Transform 且非 Locked）。天空/云/星空**不是实体**；
  ⚠⚠ 模板 Ground 曾漏 `locked`（只有空场景加）→ 框选带进地面，已补数据。
- ⚠⚠ 结算**别跳过已选中**（`selectMany`=替换）：连框两次 → found 空 → 清空。
- ⚠ 右组放不下第 7 颗时 `VP-SPEED` 会**留旧矩形** → 点中别人 → V3a→V1 连锁红：已总打
  `VP-BAR-FIT taken=n/7` 供用例 SKIP（`logRect` 不判可见性最贵的一次）。
