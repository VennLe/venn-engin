# venn —— 项目笔记

> 只放 **跨体系的纪律 + 速查 + 指针**：细则 → `*_details_archive.md`（editor / render / verify 三册），
> 流水 → `YYYY-MM-DD.md`，数字 → `roadmaps.md`。
> ⚠ **体积纪律**：14.5 KB 以上注入会被**截尾** → 11-01 把"已完成体系"的判据整体拆到 **`FEATURES.md`**
> （本页 8.5 KB / 续册 8.3 KB，两边都不截尾）。**碰某个已完成的体系之前，先读 `FEATURES.md` 那一节。**
> 仓库 `D:\A-VENN\code\cpp\venn-engin`（CMake `Venn`）。C++17 手写 Vulkan，**右手系 Z-up**；
> `core→rhi→render→scene→assets`（另有 `ui`/`i18n`/`ecs`/`physics`/`script`/`audio`/`project`；
> 应用层 `launcher`/`editor`/`runtime`，**engine/ 不认识后三个**）。改名后删 `build/` 重 configure。

## 已完成体系的判据 → `FEATURES.md`（11-01 拆出，**碰之前必读**）
- 着色 A1~A6 + 动画 B1~B3 · 过场时间轴 C1 · 相机运镜 CINE + 多机位 · 序列化/存档 G16·G17 ·
  关节 J5 · 对话 C2 · 步态 A4·A4b · **表情 B6** · 脚本/编辑器 · 二次元 UI 主题 · 界面语言默认。
- ⚠ 为什么拆：本页超 14.5 KB 会被**截尾注入**，尾部「路线图」最先丢 → 把"做完的东西"挪出去。

## ⚠⚠ 第 0 条：动手前重读磁盘版
快照**可能截断/陈旧** → 跑回归/改契约前先读磁盘；`docs/` 历史数字**要标口径**。⚠⚠ **先 stat 二进制 vs
源码 mtime**（10-23：`build/` 没跟 → 用户跑着旧 exe，把已实现的需求原样重提）。

## ⚠ 回归纪律（用户明确要求）
**不自动跑全量**：只有用户明确下达才 `--all`，日常只跑受影响用例（`TRIGGERS`）。
⚠ `--explain` 无基准快照时**一律报全量**。⚠ GUI 用例一次一个。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry`（`bindParam` 在成员默认初始化**之后**、`loadEnvOverrides()`**之前**）
③ **UI 面** ④ 序列化 ⑤ 用例 + `TRIGGERS`。**界面点不到 = 没交付**。

## ⚠ 本机环境
**GPU = Intel Iris Xe**（`maxBoundDescriptorSets=32`；低端只保证 **8**）→ 加纹理槽走
**bindless**（`render/Bindless.h`，默认关）。Python 用 envs\default\Scripts\python.exe；
⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（细则 → 归档十一 A）
`./make.cmd build`（`TOOLCHAIN=auto` → **MSVC**；`Debug` → **性能数字是 Debug 的**）。
⚠⚠ **前向声明必须与定义同 struct/class**（TU 内首次声明 → `LNK2019`；**C4099 是前兆**）。
⚠⚠ **新加资产/新目录不会自动进 `build/bin/`** →「加了完全没效果」先查这条（含 CMake 拷贝档）。

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
  契约行进 `FMT_KEEP`。⚠ `capture_client` 返 `(Image, ok)`；`--scene` 打不开**不报错** → 夹具落 `bin/`。
- ⚠⚠ **判据重标定**：先分**预热**（起手 100 ms = `maxFrameTime` 钳值）；用 **p90** 不用 p95；"玩法卡顿 vs
  环境"用**静止 vs 移动**对照。**细则 → 归档十**。

## 资产链 + 外部模型 → 角色（原文 → 归档十一 D；权威 → docs/fbx_asset_pipeline.md 等）
- ⚠⚠ **新增顶层目录必须进 `WATCH_ROOTS`**；⚠⚠ **"写了容错分支" ≠ "容错生效"** —— 上游会 `throw` 的分支
  跑不到 → 贴图缺失必须走 `tryLoadTexture()`；造"缺贴图"现场要改路径下标 + **重算 MAT CRC32**。
- ⚠⚠ **`.vmesh` 传给 `loadFBX` → 第二次起永远失败**（`.vmeta` 判过期后回退解析 .vmesh 抛异常）。
- ⚠⚠ **`PostMessage` 的 Shift 会被 GLFW 撤销** → 修饰键必须 `keybd_event` + 先 `SetForegroundWindow`；
  **`desc.maxSpeed` 截断冲刺** → 上限留余量。世界 = 米 / Z-up，玩家位置语义是**脚** → 挂模型 `lift = -boundsMin.z`；
  玩家实体必须挂 `CameraComponent`；`InputMap` **存档优先** → 老工程要"恢复默认"。
- P6 跳跃 v₀ 3.70 / `airControl 0.45`，**Play 手写同一条抛物线 → 改一处改两处**。

## 出包 E28/G29/G30（→ `docs/package_roadmap.md`；判据 `verify_package` 76 + `verify_shipping` 39）
- `Game.exe --package` / `File ▸ Package` → `dist/<Name>/`；`--pak` = `game.pak`（assets+shaders，自带 LZ4）。
- ⚠⚠ **引擎侧资产（shaders/fonts/ui/LUT）在 exeDir、不在工程里** → 打包必带（`kEngineAssetDirs`）：漏了 =
  退出 24 条 VUID / 字体断言卡死，**开发机复现不了**。
- **G30 ✅**：`make ship` → `build-ship/bin/Game.exe`（开发命令**编译期**剔除）。

## 性能基线 P1（11-01 ✅；表 → `docs/perf_baseline.md`；采集 → `tools/bench_baseline.py`）
- `make perf-build` → **`build-rel/`**，与回归用的 Debug `build/` **分开**（换掉 `build/` = 同时改掉
  "断言在不在"与"帧率是多少"，回归和基线互相污染）。
- ⚠⚠ 量法：丢预热帧 · **中位数不用均值** · GPU 取 `GPU-TIMING` 不用 `1/fps` ·
  `MYVK_RUNTIME_MAXFPS=0`（`game.json` 有 `maxFps=60`，否则量的是上限）· `--frames 900`（每 30 帧一条）。
- ⚠⚠ **实测三条**：① `forward` 占 **50%** 且随像素**线性**涨（= 填充率，改 CPU 无用）；② `shadows`
  **不随分辨率动**（跟光源数/级联图走）；③ **Debug↔Rel 只差 8%** → 这两份场景**不是 CPU 受限**，
  只动 CPU 的项（P2/P3）不会体现在帧率上，**别拿 Debug 数字判性能退步**。

## 路线图（剩什么 → `docs/remaining_work_2026-10-27.md`；权威 → `docs/*_roadmap.md`）
- 已做完的**全清单**只维护在 `remaining_work` §1（本页不重复）；近期：G29/G30 出包·E28/E29 主题·J5 关节·
  P2/P6·Prefab·多选·UI 编辑器·植被两轮·步态 A4/A4b·运镜 CINE + 多机位·R7 ClipEdit·10-31 红清零·**11-01 B6 表情 + P1 基线**。
- **剩余硬缺口：流送 / 导航 AI**（C3 存档 · E28 打包 · B6 表情 · P1 基线已结）；打磨型仍有
  立绘 C2 / 脚 IK B4 / 弹簧骨 B5 / lightmap R5-2 / 植被 W5 / 音素级口型 D2。
- ⚠ 材质位掩码到 **bit26**，只剩 5 位 → 加旋钮要换两个 u32。
- ⚠ flaky：`bvh`/`csm`/`ibl`/`particles` 偶发全红 → 别补跑"凑干净"；"闸门红"先分 真漏/记账/夹具陈旧。
