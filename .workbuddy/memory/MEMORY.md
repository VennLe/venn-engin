# venn —— 项目笔记

> 只放 **纪律 + 速查 + 指针**。细则 → `editor_details_archive.md` / `render_details_archive.md`；
> 逐日流水 → `YYYY-MM-DD.md`；数字 → `roadmaps.md`；下一步 → `docs/*_roadmap.md`。
> ⚠ **上限 ~13 KB（超了会被整段截断注入）** → 只增纪律，涨了先删"已完成"细节。
> 仓库 `D:\A-VENN\code\cpp\venn-engin`（CMake 名 `Venn`）。C++17 手写 Vulkan，**右手系 Z-up**；
> `core→rhi→render→scene→assets`（另有 `ui`/`ui2`/`i18n`/`ecs`/`physics`/`script`/`audio`/`project`；
> 应用层 `launcher`/`editor`/`runtime`，**engine/ 不认识后三个**）。改名后删 `build/` 重 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
快照**可能被截断/陈旧** → 跑回归 / 批量改文件 / 改契约前先读磁盘。
⚠ `docs/` 里的历史数字**要标"哪一轮的口径"**（不删，但不标就会被当基准）。
⚠⚠ **动手先 stat 二进制 vs 源码 mtime**（10-23 实测：整个目录 mtime 被批量还原、
build/ 没跟 → 用户跑着 10-19 的旧 exe，把已实现的 P6 三条需求原样重提）。

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
⚠ **五坑**：`https_proxy`+`HTTPS_PROXY` 并存 → 假报 `MSB6001`；UTF-8 无 BOM + 中文 → `C2001`
（要 `/utf-8`）；`.cmd`/`.bat` **纯 ASCII**；Make 用 sh 跑配方 → `echo` 里不能有 `(` `)` `|`；
**struct/class 前向声明必须与定义一致**（MSVC 修饰名不同：`AEBU`=struct / `AEBV`=class，
按 **TU 内第一次见到的声明**修饰 → 两 TU 各看一个就 `LNK2019`；**C4099 是前兆**，
增量构建会掩护、全量重编才炸。10-23 实锤：Skeleton/Material/Model 都是 struct，
6 处 `class X;` 声明已改。链接错先 `dumpbin //symbols` 对修饰名）。
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
- ⚠⚠ **水体没有网格** → 不在绘制列表/BVH；`WaterPass` 只看 `WaterBody` 不看实体 Transform →
  W/E/R 靠"Transform 当手柄+每帧成对镜像"；**Water 不能算进 `brushMode`**（否则手柄整段关掉）。
  细则 → 归档。
- **出生点 = `GameStartComponent`**（仿 PlayerStart）：位置走实体 transform，**不给实体挂网格**
  （叠加层画 Q 版小人，锚点 `GAMESTART-MARK`）；模板场景 = **地形 + GameStart**，地形资产与
  材质**随模板自带**。⚠ 无网格 → 点选/落地单独开口；层贴图路径必须过 `resolveAssetPath`。
  地面"糊/重影"先查**材质有没有贴图**，不是 TAA。

## 脚本（S0~S6 ✅；权威 → `docs/script_roadmap.md` §11）
- **`ScriptRuntime.{h,cpp}` = 唯一 tick 内核**。`let` **幂等声明**；内建名**编译期**解析；
  相位 **`声明区 → start → 接触事件 → 顶层 → update`**。
- ⚠⚠ **`ScriptProgram`/`HookBlock` 构造析构必须 out-of-line**；头里前向声明的类型**定义必须在
  `namespace vks` 里**（C2872）。热重载用 `ScriptAsset::version()` 不用指针；`on start` 不重跑；
  **S4 协程**：先 `++ip` 再压子帧 / `Frame&` 在 `push_back` 后悬垂 / `simFrame` **1-based**。
- ⚠⚠ **编辑态脚本不 tick** → 测热重载必须先进 Play。遗留：**编辑器 Play 没接 `ContactTracker`**。

## 编辑器架构（细则 → 归档一/二/三/九）
- **加面板 = 表里加一行**（`PanelRegistry` → 菜单/快捷键/图标/落盘四路派生）；面板间**只准依赖
  `EditorContext`**，动引擎走 `EditorHost`。开关权威 `g_open[kMaxPanels]`（固定数组+引用）。
- ⚠ **默认一律"全关"**（缺 `config/render.json` → 逐像素等于今天）。⚠ **快捷键排他**。
- ⚠ **2026-10-21 起已引入面板 docking**（`ModeRail`/`PanelDock`/`ViewportTabs`，imgui 随之改动）
  —— 旧"别引入 docking"约定作废。⚠ 布局一变，"按 rect 点控件"的用例会**静默漂移**（修布局不改期望）。
- ⚠ **Inspector 11 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**落盘写 id 不写下标**。
  ⚠ **`All` 不是融合视图** → 自动化 `All`、交互 `Object`。
- ⚠⚠ Content 是底栏**抽屉**；分隔条靠 `m_occluders`；"最小化" = `panelTrayHiddenRef(i)=1`，
  自己无条件绘制的窗口也得认。⚠⚠ **粒子**：分叉/辉光必须**多发射器**。

## 断言纪律（细则 → 归档四/九/十）
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

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新 `verify_*.py` 必须进 `DEFAULT`+`TRIGGERS`；
  ⚠⚠ **改完 `TRIGGERS` 必跑 `check_regression_map.py`**（改表+同步自检 fixture 是原子操作）。
  铁律：**没登记 = 全量 + 点名**；**横切文件别写进 `TRIGGERS`**（名单只以 `GLOBAL_GLOBS` 为准）。
- **`verify_i18n_coverage.py`**：⚠⚠ **缺译文不报错不崩，只会静静少翻一处**（三门盲区都栽过）；
  豁免白名单整串精确匹配；`##` id 后缀译文里必须原样保留。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 `(PIL.Image, ok)` 元组）；截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**；键盘必须 `PostMessage(WM_CHAR)`；
  自检日志 `detail=` 里不能有空格。
- ⚠⚠ **词典会长、SDF 图集是静态产物** → 加词条后重跑 `gen_sdf_atlas.py --atlas 2048 --atlas-h 3072`。

## 路线图（权威 → `docs/*_roadmap.md`；已完成数字 → `roadmaps.md`）
- 完成：启动器 M0~M6(缺 M5-c)·世界 W0~W4·渲染 R0~R4/R5-1/R6-1·编辑器 B/C/D/E11.1/E25/E26·
  运行时 M1+M2·物理 v1·音频·UI/HUD U0~U8·脚本 S0~S6·资产链六/七·**外部模型→角色
  （1:1/AutoRig/ProcAnim，10-22）·A1 Animator·P4~P6 角色操控全套（10-22）**。
- 下一项：**A2/A3 状态机与控制器资产** ·世界 **W5 植被**·编辑器 E20/E22·**G16-1**。
  ⚠ **anim/sk 双份字段的权威规则**见 `roadmaps.md`。
- ⚠ flaky：`bvh`/`csm`/`ibl` 偶发全红（单跑 PASS）→ **别补跑"凑干净"**。
  `primitive/material_roundtrip` 因 docking 漂移 → 修布局，别改期望。
- ⚠ **已知红（未修）**：`verify_character_import` 的「点完之后 Animator 段出现」——
  疑似 Add Animator 按钮落在裁剪区外，留到 A2 轮查。

## 资产链 + 外部模型 → 角色（权威 → docs/fbx_asset_pipeline.md、external_model_pipeline.md、player_controls.md）
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
- **P6 键位/跳跃/贴地（10-22）**：出厂 WASD/右键跑/空格跳（`InputMap`，**存档优先**——老工程
  `input.json` 存过 `Sprint=LShift` 要点"恢复默认"）；跳跃 v₀ 3.70 m/s / 0.70 m / 0.754 s /
  `airControl 0.45`（编辑器 Play 手写同一条抛物线，**改一处改两处**）；`poseJump` 已重写为
  双脚离地（`PROCANIM-JUMP jumpFootLift` 可断言）；贴地用**扫掠窗口**
  `sweep = |vz|·dt + hStep·tan(slopeLimit) + 0.02` + `GAME-PLAYER-RESCUE` 兜底。

## 渲染资源生命周期（权威 → docs/render_resource_lifetime.md）
**铁律：一条 CB 在 recording 状态时，它引用过的对象都不能销毁** —— 违反 =
`invalid state ... was destroyed` 刷屏 → submit -4 (DEVICE_LOST) → Fatal。
- **读日志只读第一条 `invalid state`**，后面的 TEX-FAIL 是余波，**别从后往前读**。
- `uploadSceneState()` 里 `syncTerrains`/`syncWaters` **只对主场景做**（判据 `offscreenSlot < 0`；
  **别用 `resetSkinning`**，已被统计口径占用）。角色预览+地形必崩已修（验收 `verify_preview_terrain.py`，
  **必须带地形**，空场景恒真抓不到）。
- 另一 -4 来路：**逐帧重建 → 单次提交超 TDR 2 秒** → "每帧比对不一致就重建"的判据要先问会不会恒不一致。
