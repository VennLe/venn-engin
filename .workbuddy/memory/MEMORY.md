# venn —— 项目笔记

> 只放 **纪律 + 速查 + 指针**。细则全文 → `editor_details_archive.md` §十一 A~E（构建/契约/断言/
> 资产链/路线图**原文**）、`render_details_archive.md`（A1~A6 + B1~B3 全文）；
> 流水 → `YYYY-MM-DD.md`；数字 → `roadmaps.md`。
> ⚠ **体积纪律**：**14.5 KB 以上注入会被截尾**（实测丢掉了末尾两节）。10-26 蒸馏过两轮
> （18.2 → 14.6 → 现在 ~14.5 KB），四个大节改成"原文 → 归档"指针，A/B/C1 三节的原文搬去
> **归档十二**。**新细则往归档写，别往这里加**；章节按"越靠后越不怕被截"排。
> 仓库 `D:\A-VENN\code\cpp\venn-engin`（CMake 名 `Venn`）。C++17 手写 Vulkan，**右手系 Z-up**；
> `core→rhi→render→scene→assets`（另有 `ui`/`ui2`/`i18n`/`ecs`/`physics`/`script`/`audio`/`project`；
> 应用层 `launcher`/`editor`/`runtime`，**engine/ 不认识后三个**）。改名后删 `build/` 重 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
快照**可能被截断/陈旧** → 跑回归 / 批量改文件 / 改契约前先读磁盘。`docs/` 历史数字**要标"哪一轮的
口径"**（不标就会被当基准）。
⚠⚠ **动手先 stat 二进制 vs 源码 mtime**（10-23 实测：目录 mtime 被批量还原、build/ 没跟 → 用户跑着
10-19 的旧 exe，把已实现的 P6 三条需求原样重提）。

## ⚠ 回归纪律（用户明确要求）
**不自动跑全量**：只有用户明确下达才 `--all`，日常只跑受影响用例（按 `TRIGGERS`）。
⚠ `--explain` 无基准快照时**一律报全量**。⚠ GUI 用例**同时只能有一个**。

## ⚠ 特性准入（R5/R6/R7）= 五件套
① 实现 ② `FeatureRegistry`（`bindParam` 在成员默认初始化**之后**、`loadEnvOverrides()`**之前**）
③ **UI 面** ④ 序列化 ⑤ 用例 + `TRIGGERS`。**只有 `MYVK_*`、界面点不到的不算交付**。
把关 `check_feature_surface.py`（C/D 清零）。

## ⚠ 本机环境
**GPU = Intel Iris Xe**：`maxBoundDescriptorSets=32` / `maxPerStageDescSamplers=64` / descriptor indexing
可用 —— 但**低端设备只保证 8** → 加纹理槽走 **bindless**（`render/Bindless.h`，默认关），不是"加 set"。
Python 用 `...\envs\default\Scripts\python.exe`；⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（原文 → 归档十一 A）
`./make.cmd build`（`TOOLCHAIN=auto` → **MSVC**；`Debug` → **性能数字是 Debug 的**）。
⚠⚠ **前向声明必须与定义同 struct/class**（`AEBU`/`AEBV` 按 TU 内**首次**声明 → `LNK2019`；**C4099
是前兆**，增量构建掩护、全量重编才炸；加前向声明前先 grep 定义）。⚠⚠ **新加资产不会自动进
`build/bin/`** → 首次跑用例必 `*-MISSING`，「加了完全没效果」先查这条。其余（代理变量并存 →
`MSB6001`、UTF-8 无 BOM → `C2001`、`.cmd` 纯 ASCII、`add_custom_target(... ALL)`、`LNK1163`）→ 归档。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP="near,far"`（默认 `0.1/5000`）；
编辑器侧一律 `MYVK_EDITOR_` 前缀（白名单按前缀放行）。⚠ **落盘读/写不对称**：自动化下默认不读、
显式 `MYVK_EDITOR_LAYOUT` 照读、**写一律拒绝**。⚠⚠ **改"全局默认"要 grep 依赖它的测试**（远平面
200→5000 让基线整体漂）。**修法是钉输入 / 升 fixture，不是改期望值。**

## 契约速查（原文 → 归档十一 B）
- **Pass 序**：`Shadow → LocalShadow → DepthPrePass → SSAO → ClusterBuild → VolumetricFog(×2) →
  Forward → PostProcess → 交换链`。**compute 不能在 RP 内执行**；栅格夹在不透明与透明之间、
  **绝不能**做成 ImGui 叠加层。**"改一处要同步改 N 处"是常态**：`FrameUBO`(768B)/推常量(128B 全满)/
  set0(25 binding)/分簇编址/旁路管线 range 须与主管线**逐字段相同**（否则 VUID-08600）。
- ⚠⚠ 三处"看着能省"的雷：两处**默认槽**（DBuffer 必须 `m_defaultTransparent`；探针 binding 23 必须
  绑 1×1 全黑 cube array，**不能"不绑"**）；**材质是共享资产、按名缓存**（父子实体的材质覆盖存盘即丢）。
- ⚠⚠ **水体没有网格** → 不在绘制列表/BVH；`WaterPass` 只看 `WaterBody` 不看 Transform → W/E/R 靠
  "Transform 当手柄+每帧成对镜像"；**Water 不能算进 `brushMode`**。⚠ **出生点 = `GameStartComponent`**
  （不挂网格）→ 点选/落地要单独开口。

## 渲染资源生命周期（权威 → `docs/render_resource_lifetime.md`）
**铁律：一条 CB 在 recording 状态时，它引用过的对象都不能销毁** —— 违反 = `invalid state` 刷屏 →
submit -4 (DEVICE_LOST)。**读日志只读第一条 `invalid state`**（后面的 TEX-FAIL 是余波）。
`uploadSceneState()` 里 `syncTerrains`/`syncWaters` **只对主场景做**（判据 `offscreenSlot < 0`，
**别用 `resetSkinning`**，已被统计口径占用）。另一 -4 来路：**逐帧重建 → 单次提交超 TDR 2 秒**。

## 着色 A1/A3/A4/A5/A6 ＋ 动画 B1/B2/B3（10-26 ✅；原文 → 归档十二）
权威 → `docs/anime_gap_review_2026-10-26.md`；使用者文档 → `docs/character_animation_guide.md`。
- ⚠⚠ **A 类槽（ramp/matcap/SDF/detail/aniso/eye）+ 角色光只在 `VENN_BINDLESS` 单元里**，而
  `MYVK_BINDLESS` **默认关** → 不开 = 全部静默失效、零报错。色带 `u=0` 被 REPEAT 回绕 → 夹到首末
  纹素中心；"白 ramp = 恒等"在 A6 后不成立（落点 `params.x = -1`）；材质页 112 B / 7 块。
- ⚠⚠ **"某特性不吃 X"要用对照材质自证响应度**；**先确认被测物在画面里再谈它动没动**
  （相机 `yaw = 180° − φ`）；A4 `shiftTangent` = `normalize(T + N*shift)`；**"暗档 ≠ 更暗"**。
- ⚠ 硬约束：B1 遮罩 = **根骨+父链子树**、**0 命中 = 不生效**（不是全部），顺序 底座→附加层→
  `applyPerformance`→`poseToJoints`，`dt==0` 视为淡完；B2 区间**半开**、`t == duration` **永不
  触发**、换 clip **归零 `eventPrev`**；**B3 剥离 = 钳回基准**（减增量 = 双倍位移且每帧日志都对）、
  跨圈那一帧与 `dt==0` 都不提、`n` 不能逐位比较。
- ⚠⚠ **glTF 的 `joints` 是 Y-up、FBX 已 Z-up** → 位移写 `TransformComponent` 前必须过
  `Skeleton::skeletonToEngine`（各 loader 自填），否则角色**朝天上飘**；yaw 用 **swing-twist**。
  ⚠ **手写的 `skinned` 实体一像素都不画**（渲染的是子实体）→ 先 `MYVK_EDITOR_IMPORT` + `scene.save`
  烤模板；`anime_girl` 的 `Idle` 是坏的、四段**全在原地** → B3 端到端只能验接线。

## 过场时间轴 C1（10-26 ✅；原文 → 归档十二；权威 → `docs/cutscene_sequencer.md`）
- ⚠⚠ **播放器不持有 `CameraSystem*`，只产出 `pose()`** —— 传指针的话运行时正常、**编辑器完全
  不能用**（视口是自由相机）。⚠ 顺序：`tick` 在 `CameraSystem::update` **之前**、玩家控制**之后**。
- ⚠⚠ **播完必须 `hasPose()==false`**；**`Ease::Step` 是"选择"不是"lerp"**；**yaw 走最短弧**；
  **关键帧按 `t` 排序**；**事件半开 + 跨圈扫两段**；**首帧 `prev = playhead - eps`**；
  `startTime>0` **不补发**；每帧夹 `0.10 s`。⚠ 编辑态 `dt==0` **要应用姿态、不推进不发事件**
  （与 B3「`dt==0` 不提取根运动」**正好相反**）。
- ⚠⚠ 验证坑：**没有 `CameraComponent` 实体时运镜被静默丢弃**；**`CAM-POSE` 只在姿态变化时打印**
  → "撒手"只能靠数值断言；硬切判**倍率**（2.1 m vs 0.32 m/帧）。端到端还要 `MYVK_LOG_RECTS=1`
  + `MYVK_EDITOR_PANELS=inspector`，否则锚点一个都不出现。

## 脚本 / 编辑器（细则 → 归档一/二/三/九；权威 → `docs/script_roadmap.md` §11）
- **`ScriptRuntime.{h,cpp}` = 唯一 tick 内核**。`let` 幂等声明；相位 **`声明区 → start → 接触事件 →
  顶层 → update`**；热重载用 `ScriptAsset::version()`。⚠⚠ **`ScriptProgram`/`HookBlock` 构造析构
  必须 out-of-line**；头里前向声明的类型**定义必须在 `namespace vks` 里**（C2872）。**S4 协程**：
  先 `++ip` 再压子帧 / `Frame&` 在 `push_back` 后悬垂 / `simFrame` **1-based**。
  ⚠⚠ **编辑态脚本不 tick** → 测热重载必须先进 Play（遗留：Play 没接 `ContactTracker`）。
- **加面板 = 表里加一行**（`PanelRegistry` → 菜单/快捷键/图标/落盘四路派生）；面板间**只准依赖
  `EditorContext`**，动引擎走 `EditorHost`。⚠ **默认一律"全关"**；⚠ **快捷键排他**；⚠⚠ **粒子**分叉/
  辉光必须**多发射器**。
- ⚠ **2026-10-21 起已引入面板 docking** —— 旧"别引入 docking"约定**作废**。⚠ 布局一变，"按 rect
  点控件"的用例会**静默漂移**（修布局不改期望）。⚠ Content 是底栏**抽屉**；"最小化" =
  `panelTrayHiddenRef(i)=1`，自己无条件绘制的窗口也得认。
- ⚠ **Inspector 12 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**落盘写 id 不写下标**。
  ⚠ **`All` 不是融合视图** → 自动化 `All`、交互 `Object`；⚠ 新页签的锚点必须也进 `drawTabAll` 那条链。

## 断言纪律（全文 → 归档十一 C）
- ⚠⚠ 结尾必须是 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号）—— 已踩 **五次**。
- ⚠⚠ 落点必须是"两种实现会不一样的那个量"；新断言必须做**否定测试** + 带**对照区**；
  **FAIL 先怀疑「期望」再怀疑「实现」**。
- ⚠⚠ 像素判据：不用整图平均差（用**显著变化像素占比**）；**不用「最亮 N%」当高光**；**先确认被测物
  完整落在采样区**；测形状关 CSM+TAA。⚠ `has_line` 证明不了"点了有反应" → `wait_count(before+1)`；
  `Popen(stdout=PIPE)` 必须有人排空；删文件一律走 `tools/safe_fs.py`。
- ⚠ 其余（`NO_HUD`+`GRID=0`、面板收托盘、NDC 的 y 向下、`argtypes`、`KF_EXTENDED`、锚点 tag 含
  数字、harness 抖动、临时诊断代码要清、删功能要连锚点一起删）→ 归档四 / 十一 C。

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新 `verify_*.py` 必须进 `DEFAULT`+`TRIGGERS`，
  ⚠⚠ **改完必跑 `check_regression_map.py`**（改表 + 同步自检 fixture 是原子操作）。铁律：**没登记 =
  全量 + 点名**；**横切文件别写进 `TRIGGERS`**（名单只以 `GLOBAL_GLOBS` 为准）。
- **`verify_i18n_coverage.py`**：⚠⚠ **缺译文不报错不崩，只会静静少翻一处**；⚠ 特性名走
  `FeatureDesc.name`，**不在参数 label/tooltip 的视野里**；⚠ **格式串（`"%.2f s"`）也要包 `TR()`**
  （E 组会抓，词典里已有条目）。
- 设施：`capture_window.py`（⚠ `capture_client` 返回 `(PIL.Image, ok)` 元组）；截图目录
  `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**；键盘必须 `PostMessage(WM_CHAR)`。
  ⚠⚠ 加词条后重跑 `gen_sdf_atlas.py --atlas 2048 --atlas-h 3072`（SDF 图集是静态产物）。

## 资产链 + 外部模型 → 角色（原文 → 归档十一 D；权威 → docs/fbx_asset_pipeline.md、
external_model_pipeline.md、player_controls.md）
- ⚠⚠ **新增顶层目录必须进 `WATCH_ROOTS`**；⚠⚠ **"写了容错分支" ≠ "容错生效"** —— 上游会 `throw` 的
  分支跑不到 → 贴图缺失必须走 `tryLoadTexture()`；造"缺贴图"现场要改路径下标 + **重算 MAT CRC32**。
- ⚠⚠ **`.vmesh` 传给 `loadFBX` → 第二次起永远失败**（`.vmeta` 判过期回退解析 .vmesh 抛异常；
  「第一次正常，删 .vmeta 又好一次」）。`MeshLoader` 有"情况 0"短路；重烤后查 `.vmeta` 残留。
- ⚠⚠ **`PostMessage` 合成的 Shift 会被 GLFW 当场撤销** → 修饰键必须 `keybd_event` + 先
  `SetForegroundWindow`。⚠⚠ **`desc.maxSpeed` 会截断冲刺** → 上限留余量（`physicsMaxSpeed()`）。
- 世界 = 米 / Z-up；玩家位置语义是**脚** → 挂模型 `lift = -boundsMin.z`；玩家实体必须挂
  `CameraComponent`；`InputMap` **存档优先** → 老工程要"恢复默认"。其余（`AssetC` 一条命令 /
  `ProcAnim` 关键姿势表 + smoothstep、**不要正弦叠加** / P6 跳跃 v₀ 3.70、0.70 m、`airControl 0.45`、
  **编辑器 Play 手写同一条抛物线 → 改一处改两处**）→ `player_controls.md`。
## 路线图（原文 → 归档十一 E；权威 → `docs/*_roadmap.md`；清单/数字 → `roadmaps.md`）
- 当前：编辑器 B~E26（缺 E20/E22）·渲染 R0~R4/R5-1/R6-1·世界 W0~W4·运行时 M1+M2·脚本 S0~S6·
  **着色 A1/A3/A4/A5/A6 + 动画 B1/B2/B3（B 组 3/9）+ 时间轴 C1（C 组 1/8）（10-26）**。
- 下一项：**C2 对话/字幕**（顺带逼出 D1 语音回调）—— `kind=Line` 的 payload 当台词 id、时间轴
  暂停等人按键，挂点已在 `SequencerSystem::fire()`。再往后 **C3 存档**（要同时记住时间轴与对话
  进度）→ D 组。**B4~B9 全要求"有东西可演"的舞台**。另：**A2/A3 状态机与控制器资产**
  （`character_pipeline_roadmap`，**与着色 A2/A3 同名不同事**）·世界 **W5 植被**·E20/E22·**G16-1**；
  A 组剩的全是"打磨"，若还挖一刀 **A8 NPR×CSM 的 PCF 台阶**最值。
- ⚠ 材质位掩码已用到 **bit26**，`kOvScalarMask=0x7FFFFFF`，**只剩 5 位** → 再加旋钮就要换成两个
  u32（升 `kMaterialVersion` + 迁移）。
- ⚠ flaky：`bvh`/`csm`/`ibl` 偶发全红（单跑 PASS）→ **别补跑"凑干净"**。
- ⚠ 已知红（未修）：`verify_character_import` 的「点完之后 Animator 段出现」（疑似按钮落在裁剪区外，
  留到 A2 轮查）；`verify_material_roundtrip` 三槽拖放全没落下；`check_feature_surface` D 组 19 条
  （**全是既有欠账**）；`verify_i18n_coverage` 缺 60 条既有译文。
