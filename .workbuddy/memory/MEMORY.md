# venn —— 项目笔记

> 只放 **纪律 + 速查 + 指针**：细则 → `*_details_archive.md`（editor / render / verify 三册），
> 流水 → `YYYY-MM-DD.md`，数字 → `roadmaps.md`。
> ⚠ **体积纪律**：14.5 KB 以上注入会被截尾 → 细则进归档，本页只留判据。
> 仓库 `D:\A-VENN\code\cpp\venn-engin`（CMake `Venn`）。C++17 手写 Vulkan，**右手系 Z-up**；
> `core→rhi→render→scene→assets`（另有 `ui`/`i18n`/`ecs`/`physics`/`script`/`audio`/`project`；
> 应用层 `launcher`/`editor`/`runtime`，**engine/ 不认识后三个**）。改名后删 `build/` 重 configure。

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
**bindless**（`render/Bindless.h`，默认关）。
Python 用 envs\default\Scripts\python.exe；⚠ **裸 `python` 不带 PIL** → 回归整批 FAIL。

## 构建（细则 → 归档十一 A）
`./make.cmd build`（`TOOLCHAIN=auto` → **MSVC**；`Debug` → **性能数字是 Debug 的**）。
⚠⚠ **前向声明必须与定义同 struct/class**（按 TU 内首次声明 → `LNK2019`；**C4099 是前兆**）。
⚠⚠ **新加资产不会自动进 `build/bin/`** → 「加了完全没效果」先查这条。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP`，一律 `MYVK_EDITOR_` 前缀。
⚠ **落盘读/写不对称**：自动下默认不读、`MYVK_EDITOR_LAYOUT` 照读、**写一律拒**。
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

## 着色 A1~A6 ＋ 动画 B1~B3（10-26 ✅；权威 → `docs/anime_gap_review_2026-10-26.md`；
使用 → `docs/character_animation_guide.md`）
- ⚠⚠ **A 类槽（ramp/matcap/SDF/detail/aniso/eye）+ 角色光只在 `VENN_BINDLESS` 单元里**，`MYVK_BINDLESS`
  **默认关** → 静默全失效。色带 `u=0` 被 REPEAT 回绕 → 夹首末纹素。⚠ **"不吃 X"要对照材质自证**；
  **先确认被测物在画面里再谈它动没动**；**"暗档 ≠ 更暗"**。
- ⚠ B1 遮罩=**根骨+父链子树**；B2 区间**半开**、换 clip **归零 `eventPrev`**；**B3 剥离=钳回基准**。
- ⚠⚠ **glTF `joints` 是 Y-up、FBX 已 Z-up** → 写 `TransformComponent` 前必须过 `Skeleton::skeletonToEngine`，
  否则角色**朝天上飘**；yaw 用 **swing-twist**。⚠ **手写 `skinned` 实体一像素不画** → 先 `IMPORT` + `scene.save` 烤模板。

## 过场时间轴 C1（10-26 ✅；权威 → `docs/cutscene_sequencer.md`）
- ⚠⚠ 播放器只产出 `pose()`、**不持有 `CameraSystem*`**（传指针 = 视口变自由相机）；`tick` 在 `update` **之前**；
  播完 `hasPose()==false`；`dt==0` 不推进。

## 相机运镜 CINE + 多机位（10-30 ✅；权威 → `docs/camera_move.md`；判据 `camera_move` 23 / `multi_cam` 44 / 自检 81·97）
- **四层分离**：姿态(+roll/shift)/镜头/rig(look·path·handheld)/效果 —— 混一起 = "改了没反应"。
  ⚠ **动词是算子不是状态**（`+15°` 在当前朝向上再摇）→ 设值写法单次看不出、**连推两次立刻红**。
- ⚠⚠ "一顿一顿"根因是**没做弧长参数化**（`kArcSub=64`，10.3%→0.48%）；判据用**直线 + 速度差 30 倍**
  手调手柄（折线测的是"弯"）。⚠ `cameraEulerFor` 基**只能** `(f,−r,u)`（`(f,r,u)` 行列式 −1 → 镜像）。
- ⚠⚠ **叠加序**：焦距→路径→注视→roll/shift→手持→震动（手持在注视前被抹掉；路径在后 →
  "沿路径+盯主体"退化成"沿切线看"）。阻尼 `1−exp(−dt/τ)`；快门存**角度**不存帧数。
- ⚠ 镜头→渲染只能宿主做（`applyCameraLensEffects`，**只在配了时写**否则 `MYVK_DOF` 失效）；`clipNear=0` =
  **不设**；新字段**只在非默认时写 JSON**（`sceneHash` 不能变）；`fov` 是**残留值** → 判 `fovEff`。
- **多机位**：`.seq.json` 的 `shots[]`（区间半开；`camera:0` = 回落关键帧轨；**早于首个 marker 返回 −1
  不是 0**）。⚠⚠ 切镜前必须 `clearDesiredPose()`（否则机位**完全不动**而 `CAM-CUT` 照打）；目标被删要
  **回落 priority + 清 guid**（否则**永久冻住**）；绑 guid 走 `parseGuidHex`（`atoll` 读成十进制）。
  ⚠ 老资产**不许长出** `"shots": []`；marker 轨画**区间**。

## 序列化 / 存档 G16·G17-1·G17-2（10-27 ✅；**逐条判据 → `persistence_roadmap.md` §12.9**）
- ⚠⚠ `setGuid` **先摘旧键**、`destroy` **先 erase 再清零**（写反 = 旧 guid 复活）；hex 补满 16 位；版本校验在
  `scene.clear()` **之前**。⚠ **空实体不进场景文件**，但**存档必须全量**（`includeEmpty=true`）。
- ⚠⚠ 父子写 `parentGuid`**不写下标**（下标会让"销毁一个实体"带翻别人的哈希）；**「已销毁」是粘性**的
  （`kDeadHash`，别 erase → 打死的怪读档复活）。应用序 **销毁 → 覆盖 → 悬挂父（无条件跑）**。

## 关节 J5（10-27 ✅；判据 → `verify_joints` A~H）
- 拾取：self 过**本实体**逆、target 过 **target** 逆（写反 = 一 Play 拽飞）；点空退相机平面；
  **Esc 只退拾取不清选中**。改关节 = 组件 + `rebuildJoints`，**绝不** `buildSceneWorld`。
- ⚠⚠ **`kBuiltins[]` 的 arity 就是公开签名**（写成标量 → 按 vec3 写的脚本**编译期** not runnable）；位移判据
  用**末态−起点**。⚠ 可视化用 **verts 精确值**（竖直连杆 4／漏换向 3）；退化基必须 `return`。
- ⚠⚠ `NOTIF-PUSH` 会**镜像每条 WARN** → 日志断言必须筛，别用裸 WARN 计数。

## 对话 C2（10-26 ✅；权威 → `docs/dialogue.md`）
- ⚠⚠ **`cue()` 绝不给场景加组件**（会被 `scene.save` 写盘）；**cue 优先于 `autoStart`**；起点**永远**用资产
  `start`。语义：`goto/next` 悬空 = **整份加载失败**｜**有 choices 时 `advance()` 不走**｜`cps<=0` = **立刻全出**。
  ⚠ **`Press::NeedChoice` 运行时不可达** → 别断言。`DLG-END` 打**两条**。

## 步态 / 步幅匹配 A4·A4b（10-30 ✅；权威 → `docs/gait_cycle.md` §五~七）
- ⚠⚠ 承重判据 = **`dFwd<0`（方向，无阈值）+ 最低点 +12cm 绝对门**；比例带宽被根运动带跑（Walk 髋起伏
  0.805m）—— 八轮改阈值全败，`duty` 0.13~0.61 乱跳。走/跑只认**三条相对关系**；教科书 duty **不可用**。
- ⚠⚠ 相位曲线推进**必须积分**（4~32 子步；逐帧查表 = 量化到帧率 → "60fps 不滑、30fps 滑"）；只推**循环**
  clip；**缓存键带毫秒时长**。⚠ 斜坡 `rate` 分子永远**水平投影**，`slopeDeg` 只算 `verticalResidual`。

## 脚本/编辑器（细则 → 归档；权威 → `docs/script_roadmap.md`；**能力评估** → `docs/script_language_assessment.md`）
- ⚠⚠ **能力实测（10-30）：6861 行 / 84 内建 / 93 自检绿，但组件覆盖 6/42=14%**（相机 29 字段全不碰）→
  **白名单字段投影层** `prop/setProp`（字段名编译期解析），不换 Lua。⚠ `setIntensity` 对无灯实体
  **静默返回 0**；**无断点/单步/内联编辑器**。
- **`ScriptRuntime.{h,cpp}` = 唯一 tick 内核**。相位 **`声明区 → start → 接触事件 → 顶层 → update`**；热重载用
  `ScriptAsset::version()`。⚠⚠ **`ScriptProgram`/`HookBlock` 构造析构必须 out-of-line**；前向声明类型的定义
  **必须在 `namespace vks` 里**（C2872）。⚠⚠ **编辑态脚本不 tick** → 测热重载必须进 Play。
- **加面板 = 表里加一行**（`PanelRegistry` → 菜单/快捷键/图标/落盘四路派生）；面板间**只准依赖
  `EditorContext`**，动引擎走 `EditorHost`。⚠ 快捷键排他；⚠⚠ 粒子分叉/辉光**多发射器**；
  ⚠ 循环里 `PushID` **绝不能用名字**（重名 → ImGui 弹**模态**告警窗，之后点击全被吞）。
- ⚠ **Inspector 13 页签**：加页签 = `kTabs[]` 一行 + 两处 `static_assert`；**落盘写 id 不写下标**；
  `All` **不是融合视图** → 新页签锚点也要进 `drawTabAll`；布局一变，按 rect 点控件的用例会漂。

## 二次元UI主题（10-27✅；→ `docs/anime_ui_restyle.md`）
- ⚠⚠ **`c^(1/2.2)`**：交换链把 ImGui 浮点色当线性值**又编码一次**；动漫档 `darkenThemeColors(1.45)` 只对
  深色，**老 5 套系数一个都不许动**。⚠ 圆角进 `ThemeMetrics` 后 padding 一动**面板矩形全变**；**主题下标只能追加**。

## 界面语言默认 = 系统语言（10-29 ✅；→ `docs/system_language_2026-10-29.md`；判据 `verify_system_lang`）
- 出厂 = **系统语言**（`g_lang` 用 `-1` 哨兵惰性求值）；env > 存档 > 系统。⚠ **自动化跑（`MYVK_FRAMES` /
  `MYVK_NO_WINDOW_SAVE`）默认英文**；prefs **不写**与出厂相同的 `language`。
- ⚠⚠ `it.id == "language"` 比的是**指针**（`const char*`）→ 必须 `strcmp`（10-29 静默踩过）。

## 断言纪律/工具（细则 → `verify_details_archive.md`）
- ⚠⚠ 结尾 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号）；落点 = "两种实现会不一样的那个量"；新断言
  必做**否定测试** + **对照区**；**FAIL 先怀疑期望**；否定测试**没红也是结论**。
- ⚠⚠ 像素判据用**显著变化像素占比**（非整图平均差）；**先确认被测物在采样区**；测形状关 CSM+TAA。
- ⚠⚠ **合成鼠标先预热**（两次 MOVE + 0.35s，否则 `MousePos=-FLT_MAX`）；**点菜单用 `mouse_event`**；
  lParam **带扫描码** `(_sc(vk)<<16)|1`；**键位会变** → 从日志读；**运行时先点视口才进操作态**。
- ⚠⚠ `logRect` **不判可见性** → 先滚再点；锚点**无条件**且放在"选中之后"；裁切不含边界；
  **/RTC1 越界 = `#32770` 断言框**；日志计数**带后缀**。
- **`run_regression.py`**：新 `verify_*.py` 必须进 `DEFAULT`+`TRIGGERS`，⚠⚠ **改完必跑
  `check_regression_map.py`**；**没登记 = 全量 + 点名**；横切文件**别写进 `TRIGGERS`**；帧数是 `--frames`。
- **`verify_i18n_coverage.py`**：⚠⚠ 缺译文**不报错**只静静少翻；特性名走 `FeatureDesc.name`；格式串包 `TR()`；
  契约行进 `FMT_KEEP`。⚠ `capture_client` 返 `(Image, ok)`；`--scene` 打不开**不报错** → 夹具落 `bin/`。

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

## 路线图（剩什么 → `docs/remaining_work_2026-10-27.md`；权威 → `docs/*_roadmap.md`）
- 做完的：R0~R7·W0~W4·S0~S6·A1~A6·B1~B3·C1/C2/C6（除 `openUI`）·G16~G17-2·G29/G30·E28/E29·J5·P2/P6·
  Prefab·多选·UI 编辑器·植被两轮·步态 A4/A4b·运镜 CINE + 多机位（判据 → 归档十三~十七 + `verify_*`）。
- **R7 ✅**（10-28）：`engine/animation/ClipEdit.*` = 可写回的 `EditClip`（`.animclip.json`）。
- **两大缺口：C3 存档（G17-3）· D1 语音 + B6 表情**。
- ⚠ 材质位掩码到 **bit26**，只剩 5 位 → 加旋钮要换两个 u32。
- ⚠ flaky：`bvh`/`csm`/`ibl`/`particles` 偶发全红 → 别补跑"凑干净"；"闸门红"先分 真漏/记账/夹具陈旧。
