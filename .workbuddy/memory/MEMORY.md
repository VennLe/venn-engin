# venn —— 项目笔记

> 只放 **纪律 + 速查 + 指针**。细则 → `editor_details_archive.md`（§十一）/
> `render_details_archive.md`；流水 → `YYYY-MM-DD.md`；数字 → `roadmaps.md`。
> ⚠ **体积纪律**：14.5 KB 以上注入会被截尾 → 细则写归档，本页只留判据。
> 仓库 `D:\A-VENN\code\cpp\venn-engin`（CMake `Venn`）。C++17 手写 Vulkan，**右手系 Z-up**；
> `core→rhi→render→scene→assets`（另有 `ui`/`i18n`/`ecs`/`physics`/`script`/`audio`/`project`；
> 应用层 `launcher`/`editor`/`runtime`，**engine/ 不认识后三个**）。改名后删 `build/` 重 configure。

## ⚠⚠ 第 0 条：动手前重读磁盘版
快照**可能截断/陈旧** → 跑回归/批量改文件/改契约前先读磁盘。`docs/` 历史数字**要标口径**（不标
就被当基准）。
⚠⚠ **动手先 stat 二进制 vs 源码 mtime**（10-23 实测：目录 mtime 被还原、build/ 没跟 → 用户跑着旧 exe，
把已实现的需求原样重提）。

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

## 构建（原文 → 归档十一 A）
`./make.cmd build`（`TOOLCHAIN=auto` → **MSVC**；`Debug` → **性能数字是 Debug 的**）。
⚠⚠ **前向声明必须与定义同 struct/class**（按 TU 内首次声明 → `LNK2019`；**C4099 是前兆**）。
⚠⚠ **新加资产不会自动进 `build/bin/`** → 「加了完全没效果」先查这条。

## 环境变量
⚠ 顺序固定：`MYVK_EDITOR_SCENE` → `EDITOR_CAM` → `MYVK_EDITOR_CLIP`（默认 `0.1/5000`），一律
`MYVK_EDITOR_` 前缀。⚠ **落盘读/写不对称**：自动下默认不读、`MYVK_EDITOR_LAYOUT` 照读、**写一律拒**。
⚠⚠ **改"全局默认"要 grep 依赖它的测试**（钉输入/升 fixture）。

## 契约速查（原文 → 归档十一 B）
- **Pass 序**：`Shadow → LocalShadow → DepthPrePass → SSAO → ClusterBuild → VolumetricFog(×2) →
  Forward → PostProcess → 交换链`。**compute 不能在 RP 内执行**；栅格夹在不透明与透明之间、
  **绝不能**做成 ImGui 叠加层。**改一处同步改 N 处**：`FrameUBO` 768B/推常量 128B/set0 25 binding/
  分簇编址/旁路管线 range 与主管线**逐字段相同**（VUID-08600）。
- ⚠⚠ 三处"看着能省"的雷：**默认槽不能空**（DBuffer 必须 `m_defaultTransparent`；探针 binding 23
  绑 1×1 全黑 cube array）；**材质按名共享缓存**（父子的材质覆盖存盘即丢）。
- ⚠⚠ **水体没有网格** → 不在绘制列表/BVH；`WaterPass` 只看 `WaterBody` 不看 Transform。⚠ **出生点 =
  `GameStartComponent`**（不挂网格）→ 点选/落地要单独开口。

## 渲染资源生命周期（权威 → `docs/render_resource_lifetime.md`）
**铁律：一条 CB 在 recording 状态时，它引用过的对象都不能销毁** —— 违反 = `invalid state` 刷屏 →
submit -4；**只读第一条**。`syncTerrains/syncWaters` **只对主场景**（`offscreenSlot<0`，**别用
`resetSkinning`**）。另一 -4 来路：**逐帧重建 → 单次提交超 TDR 2 秒**。

## 着色 A1/A3/A4/A5/A6 ＋ 动画 B1/B2/B3（10-26 ✅；权威 → `docs/anime_gap_review_2026-10-26.md`；
使用 → `docs/character_animation_guide.md`）
- ⚠⚠ **A 类槽（ramp/matcap/SDF/detail/aniso/eye）+ 角色光只在 `VENN_BINDLESS` 单元里**，
  `MYVK_BINDLESS` **默认关** → 静默全失效。色带 `u=0` 被 REPEAT 回绕 → 夹首末纹素。
- ⚠⚠ **"不吃 X"要用对照材质自证**；**先确认被测物在画面里再谈它动没动**（`yaw = 180° − φ`）；
  **"暗档 ≠ 更暗"**。
- ⚠ B1 遮罩=**根骨+父链子树**、**0 命中=不生效**；B2 区间**半开**、换 clip **归零 `eventPrev`**；
  **B3 剥离=钳回基准**（减增量=双倍位移）。`dt==0`：B1 视为淡完、B3 不提取。
- ⚠⚠ **glTF 的 `joints` 是 Y-up、FBX 已 Z-up** → 位移写 `TransformComponent` 前必须过
  `Skeleton::skeletonToEngine`，否则角色**朝天上飘**；yaw 用 **swing-twist**。⚠ **手写的 `skinned`
  实体一像素都不画** → 先 `MYVK_EDITOR_IMPORT` + `scene.save` 烤模板。

## 过场时间轴 C1（10-26 ✅；权威 → `docs/cutscene_sequencer.md`）
- ⚠⚠ 播放器只产出 `pose()`、**不持有 `CameraSystem*`**（传指针 = 视口变自由相机）；`tick` 在
  `CameraSystem::update` 之前；播完 `hasPose()==false`；`dt==0` 不推进；无 `CameraComponent` 时静默丢。

## 序列化 / 存档 G16·G17-1·G17-2（10-27 ✅；原文 → 归档十三~十六；权威 → `persistence_roadmap.md` §12）
- ⚠⚠ `setGuid` **先摘旧键**、`destroy` **先 erase 再清零**（写反 = 旧 guid 复活）；hex **补满 16 位**；
  版本校验在 `scene.clear()` **之前**。⚠ **空实体不进场景文件**，但**存档必须全量**（`includeEmpty=true`）。
- ⚠⚠ 三段 ①② **签名不许有 `AssetManager`**（`verify_sceneio.py` D 组静态盯）。存档原子写
  tmp→fsync→bak→`MoveFileExW`（**顺序不能换**）；`sceneHash` = guid **排序后顺序**喂 FNV。
- ⚠⚠ 父子写 `parentGuid`**不写下标**（下标会让"销毁一个实体"把别人的哈希带翻）；`World` 池子
  **swap-and-pop** → 夹具不"把父排最后"时这 bug **测不出来**。
- ⚠⚠ **「已销毁」粘性**：基线保留 guid 记 `kDeadHash`，**别 erase**（→ 打死的怪读档复活）。应用序
  **销毁 → 覆盖 → 悬挂父（无条件跑）**；应用**复用** `entityApply*`（白名单=第二套解析，漏改不报错）。

## 关节 J5（10-27 ✅；判据 → `verify_joints.py` A~H）
- 拾取：self 过**本实体**逆、target 过 **target** 逆（写反=一 Play 拽飞）；点空退相机平面；**Esc 只退
  拾取不清选中**。改关节 = 组件 + `rebuildJoints`，**绝不** `buildSceneWorld`。
- ⚠⚠ **`kBuiltins[]` 的 arity 就是公开签名**（写成标量 → 按 vec3 写的脚本**编译期** not runnable）；
  位移判据用**末态−起点**。⚠ 可视化判据用 **verts 精确值**（竖直连杆 4／漏换向 3）：零尺寸立方体照样
  计数 → 退化基必须 `return`。

## 对话 C2（10-26 ✅；权威 → `docs/dialogue.md`）
- ⚠⚠ **`cue()` 绝不给场景加组件**（会被 `scene.save` 写盘）；**cue 优先于 `autoStart`**；起点
  **永远**用资产 `start`。语义：节点按 **id**｜`goto/next` 悬空 = **整份加载失败**｜
  **有 choices 时 `advance()` 不走**｜`cps<=0` = **立刻全出**。
- ⚠⚠ **`Press::NeedChoice` 运行时不可达** → 别断言。`DLG-END` 打**两条**（只 system 带 `asset/src`）。

## 脚本 / 编辑器（细则 → 归档一/二/三/九；权威 → `docs/script_roadmap.md` §11）
- **`ScriptRuntime.{h,cpp}` = 唯一 tick 内核**。相位 **`声明区 → start → 接触事件 → 顶层 → update`**；
  热重载用 `ScriptAsset::version()`。⚠⚠ **`ScriptProgram`/`HookBlock` 构造析构必须 out-of-line**；前向
  声明类型的定义**必须在 `namespace vks` 里**（C2872）。**S4 协程**：先 `++ip` 再压子帧。
  ⚠⚠ **编辑态脚本不 tick** → 测热重载必须先进 Play（遗留：Play 没接 `ContactTracker`）。
- **加面板 = 表里加一行**（`PanelRegistry` → 菜单/快捷键/图标/落盘四路派生）；面板间**只准依赖
  `EditorContext`**，动引擎走 `EditorHost`。⚠ 默认**全关**；⚠ 快捷键排他；⚠⚠ 粒子分叉/辉光**多发射器**。
- ⚠ 布局一变，"按 rect 点控件"的用例会**静默漂移**（修布局≠改期望）。
- ⚠ **Inspector 12 页签**：加页签 = `kTabs[]` 加一行 + 两处 `static_assert`；**落盘写 id 不写下标**。
  ⚠ **`All` 不是融合视图** → 自动化 `All`、交互 `Object`；新页签锚点也要进 `drawTabAll`。

## 二次元UI主题（10-27✅；→ `docs/anime_ui_restyle.md`）
- ⚠⚠ **`c^(1/2.2)`**：交换链把 ImGui 浮点色当线性值**又编码一次**；动漫档 `darkenThemeColors(1.45)`
  只对深色，**老 5 套系数一个都不许动**。⚠ 圆角进 `ThemeMetrics` 后 padding 一动**面板矩形全变**；
  **主题下标只能追加**。E29-1~3 ✅；E29-5 角落抓手 ✅（角=合并/边=调大小）。

## 断言纪律
- ⚠⚠ 结尾必须是 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号）。
- ⚠⚠ 落点必须是"两种实现会不一样的那个量"；新断言必做**否定测试** + **对照区**；
  **FAIL 先怀疑期望再怀疑实现**。⚠ 否定测试**没红也是结论**（写进文档）。
- ⚠⚠ 像素判据：不用整图平均差（用**显著变化像素占比**）；不用「最亮 N%」当高光；**先确认被测物完整
  落在采样区**；测形状关 CSM+TAA。⚠ `has_line` 证明不了"点了有反应" → `wait_count(+1)`；`Popen(PIPE)`
  必须排空；删文件走 `safe_fs.py`。
- ⚠⚠ **合成鼠标"起手"要预热**（两次 MOVE + 0.35s）：上一动作常把光标甩出客户区 → `MousePos=-FLT_MAX`
  → 分组轮流红。**点菜单必须 `mouse_event`**（`PostMessage` 连跑 0/3 成功）。

## 工具
- **`run_regression.py`**：读末尾 `结果: PASS`；增量。⚠ 新 `verify_*.py` 必须进 `DEFAULT`+`TRIGGERS`，
  ⚠⚠ **改完必跑 `check_regression_map.py`**。铁律：**没登记 = 全量 + 点名**；**横切文件别写进
  `TRIGGERS`**（以 `GLOBAL_GLOBS` 为准）。⚠ **帧数参数是 `--frames`**（`--max-frames` 静默忽略→
  超时 kill）；⚠ `--scene` 打不开**不报错** → 夹具落 `build/bin/`、用**相对名**。
- **`verify_i18n_coverage.py`**：⚠⚠ **缺译文不报错不崩，只会静静少翻一处**；⚠ 特性名走
  `FeatureDesc.name`，**不在参数 label/tooltip 的视野里**；⚠ **格式串（`"%.2f s"`）也要包 `TR()`**。
- 设施：⚠ `capture_client` 返 **`(PIL.Image, ok)` 元组**；截图到 `<repo>/../_venn_verify`；禁硬编码盘符；
  键盘走 `PostMessage(WM_CHAR)`；加词条后重跑 `gen_sdf_atlas.py`。
- ⚠⚠ `logRect` **不判可见性**（只去重）→ 滚出可视区的控件"有矩形却点不到"：`click_tag` 前**先滚**
  （本机 Inspector 的固定落点是**死区** → 滚 `INS-PANEL` 中轴、离底 60px，90px/格）。
- ⚠⚠ 分隔条遮挡裁切**不能含边界**：右边界压在线上 → 整线被裁光（`spans=0`），锚点却还在。
- ⚠⚠ 面板往上加一行，下面的按钮网格会整排掉出面板；**Debug /RTC1 越界 = 断言框**
  （进程活着、日志停在上行）→ 找 `#32770` 读原文。

## 资产链 + 外部模型 → 角色（原文 → 归档十一 D；权威 → docs/fbx_asset_pipeline.md 等）
- ⚠⚠ **新增顶层目录必须进 `WATCH_ROOTS`**；⚠⚠ **"写了容错分支" ≠ "容错生效"** —— 上游会 `throw` 的
  分支跑不到 → 贴图缺失必须走 `tryLoadTexture()`；造"缺贴图"现场要改路径下标 + **重算 MAT CRC32**。
- ⚠⚠ **`.vmesh` 传给 `loadFBX` → 第二次起永远失败**（`.vmeta` 判过期后回退解析 .vmesh 抛异常；
  「第一次正常，删 `.vmeta` 又好一次」）。重烤后查 `.vmeta` 残留。
- ⚠⚠ **`PostMessage` 合成的 Shift 会被 GLFW 撤销** → 修饰键必须 `keybd_event` + 先
  `SetForegroundWindow`。⚠⚠ **`desc.maxSpeed` 截断冲刺** → 上限留余量。
- 世界 = 米 / Z-up；玩家位置语义是**脚** → 挂模型 `lift = -boundsMin.z`；玩家实体必须挂
  `CameraComponent`；`InputMap` **存档优先** → 老工程要"恢复默认"。P6 跳跃 v₀ 3.70 / `airControl
  0.45`，**Play 手写同一条抛物线 → 改一处改两处**。

## 出包 E28/G29/G30（→ `docs/package_roadmap.md`；判据 `verify_package` 76 + `verify_shipping` 40）
- 出包 = `Game.exe --package` / `File ▸ Package（as .pak）` → `<根>/dist/<Name>/`。
- ⚠⚠ **引擎侧资产（shaders/fonts/ui/LUT）在 exeDir、不在工程里** → 打包必带（`kEngineAssetDirs`）。
  漏了 = 「退出 24 条 VUID」或「字体断言卡死」，开发机复现不了。
- **G29-a 闭包 ✅**：**任何 JSON 字符串**都是候选 + 三索引。⚠ 地形 `height/splat` 相对 **vterrain 自己**；
  `_comment` 跳过；`continue` ≠ 剪枝（`.trash/` 照样递归）。
- **G29-b/c/d ✅**（`Pak.*`+`AssetSource.*`）：`game.pak` 装 `assets/`+`shaders/` + 自带 LZ4；只拦
  `resolveAssetPath`/`resolveShaderPath`，shader 走 `pak:` **直读内存**（`inline` 函数指针倒置）。⚠ 资产仍"落缓存"。
- **G30 shipping ✅**：`make ship` → `build-ship/bin/Game.exe`；编辑器/启动器/AssetC **不声明**，开发命令
  **编译期**剔除（判据按字节：出货 0 次 / dev >0）；出包用 dev 当工具 + `--engine` 指出货 exe。

## 路线图（剩什么 → `docs/remaining_work_2026-10-27.md`；权威 → `docs/*_roadmap.md`）
- 做完的：R0~R4·R5-1·R6-1·W0~W4·S0~S6·A1~A6·B1~B3·C1/C2·C6（除 `openUI`）·Prefab·多选·UI 编辑器·
  **G16~G17-2 · 二次元 UI 主题 · P2 菜单接线 · J5 关节 · 植被第二轮 · G29/G30 出包 · R7 骨骼打帧+Pose Mode**
  （判据 → 归档十三~十七）。
- **R7 ✅**（10-28）：`engine/animation/ClipEdit.{h,cpp}` = 可写回的 `EditClip`（`.animclip.json`），
  `Skeleton` 仍只读 → **没动 `engine/assets/**`、不用跑全量**。采样三段式（基础片段→编辑层→瞬态层），
  无编辑数据时逐位一致。⚠ **循环里 `PushID` 绝不能用名字**：`anime_girl.fbx` 归一化后是
  `[Idle,Walk,Idle,Walk]`，同帧同 ID → ImGui 弹**模态**告警窗，之后所有点击全被吞（界面看着正常、
  只是点不动）；一律用下标。⚠ `OK_RE/BAD_RE` 现认 `结果: N 通过 / M 失败`（此前这种收尾一律误判 FAIL）。
- **植被第二轮 ✅**（10-28）：类型可绑自有模型 + 可设偏移/旋转/缩放区间，三路共用 `samplePlacement`
  （→ `foliage_mesh_2026-10-28.md` / `verify_foliage_mesh`）。**J5** ⚠⚠ `NOTIF-PUSH` 镜像每条 WARN →
  日志断言必须筛；`INS-JOINT-HEADER` 无条件画 → 看 `-KIND`（→ `verify_joints` A~H）。
- **三大缺口：C3 存档（G17-3）· E30 Shipping 剔除 · D1 语音 + B6 表情**。
- ⚠ 材质位掩码到 **bit26**，只剩 5 位 → 加旋钮要换两个 u32。
- ⚠ flaky：`bvh`/`csm`/`ibl`/`particles` 偶发全红 → 别补跑"凑干净"。
- ⚠ "闸门红"先分 真漏/记账/夹具陈旧。
