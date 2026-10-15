# venn —— 项目长期笔记（规则索引）

> 改动前必读的**规则**。实现细节 / 全量环境变量 / set0 描述符表 / 构建路径表 / 全量坑点表 →
> `render_details_archive.md`；叙事 → 各日 `YYYY-MM-DD.md`。
> 引擎：手写 Vulkan（学习向 C++17），`E:\code\cpp\venn-engin`（CMake 工程名 `Venn`，刻意不一致）。
> 世界坐标 **右手系 Z-up**。唯一可执行文件 `Editor.exe`，启动 `resetToEmptyScene`。
> 分层 `core→rhi→render→scene→assets`（另有 `ui`/`ecs`/`physics`；应用层只有 `editor`）；依赖单向，
> **engine/ 不认识 editor/**。**改目录名后必须删 `build/` 重新 configure**。

## ⚠ 回归执行纪律（用户明确要求，2026-10-15）
**不要自动跑全量回归。** 只有用户**明确下达"开始全量回归"的指令**时才跑
`tools/run_regression.py --all`。日常改动的正确做法是：
- 只跑**受影响的用例**（`run_regression.py <名字…>`，可省 `verify_` 前缀；它按 `TRIGGERS`
  的源码 glob 判断"要不要跑"，比全量快一个数量级）；
- 收尾交付时**不要**顺便来一轮全量 —— 等指令。
- 单跑某个失败用例做**复筛**不属于"全量回归"，允许。

## ⚠ 特性准入（新功能定义，与构建同级）
**新特性（R5-x / R6-x / R7-x）交付 = 五件套，缺一不可**：
① 实现 ② `FeatureRegistry` 登记 ③ **UI 面**（编辑器里点得到）④ 序列化 ⑤ 回归用例 + TRIGGERS。
**只有 `MYVK_*` 环境变量、编辑器点不到的功能，不算交付**（美术用不上）。
把关：`python tools/check_feature_surface.py`（秒级、不跑 GUI，可进 CI）。
架构全文 → `docs/editor_architecture.md`；**要动手做什么 → `docs/editor_roadmap.md`（E01~E29）**。

## 特性注册表（E01 ✅ 2026-10-14，改动前必读）
`engine/render/FeatureRegistry.{h,cpp}` = 可调项的**唯一事实源**。现状 **28 组 / 103 参数 / 0 未接线**。
- **加新参数 = 两步**：① 在 `kFeatures[]` 的对应 `kParams*[]` 加一行 `ParamDesc`；
  ② 在 `Renderer::bindFeatureParams()`（或 `bindPostFeatureParams()`）加一行 `bindParam("id", &成员, kind)`。
  漏了②→ 启动时 `FEATURES-UNBOUND` WARN 点名；漏了①→ 参数根本没地方写。
- **两条不能破的约束**：① 权威存储仍是原来那 4 个设置结构体 + Renderer 成员，注册表**只有地址**；
  ② `bindParam` 必须发生在**成员默认初始化之后、`loadEnvOverrides()` 之前**
  —— 绑定那一刻的值会被记成默认值（供 `resetToDefaults()`），顺序反了 reset 就失效。
- **`loadEnvOverrides()` 可多次调用**，只处理"尚未应用过"的绑定（`applied` 标记）。
  所以构造函数 + `createMainTarget()` 各调一次是设计如此，**不是 bug**；
  也正因如此，`init()` 里被设备能力修正过的开关（如 `m_bindlessEnabled` 被强制关）
  不会被第二次重读打回去。
- **`EnvMode` 11 种必须逐条对应原解析函数**，不许"顺手统一"（边界行为会变）。
  最容易错的：`PFloatClamp`（`atof` 直取，**不拒绝负数** → 夹到下界）vs
  `PFloatClampNN`（`envFloat` 语义，**拒绝负数** → 保持默认）。`MYVK_RENDER_SCALE` 属于后者。
- **不进注册表的**（有理由）：`MYVK_IES_FILE`（字符串清单）、`MYVK_VIGNETTE_OFF`（反向开关）、
  `MYVK_VIEW_CAM`（用哪台相机）、`MYVK_AE_SIM_LUM`（调 setter）、全部 `*_DBG`（诊断闸门）。
- 新参数加完后跑 `python tools/check_feature_surface.py -v`：H 组会打印"注册表接管度"，
  未接管的 24 条（值在函数局部变量里）是 **E10** 的输入。
- ⚠ **`FeatureRegistry.*` 在 `run_regression.py` 的 `GLOBAL_GLOBS` 里** —— 动它就全量重跑全量回归（现 **47 例**）。

## 布局范式（E04~E08 ✅/🟡 2026-10-14，改动前必读）
清单全文与逐条记账 → `docs/editor_roadmap.md` §3「落地记录」。**几条不能破的约束**：
- **面板表 = `EditorApp` 的静态成员 `panelTableVec()`**（不是自由函数 + `friend`：那要求两处声明
  逐字一致，漂移编译期查不出来）。加面板 = 在 `buildPanelTable()` 加一行 + 写一个静态薄壳
  `panelDrawXxx(EditorApp&)`（表项要访问私有成员，薄壳里 `a.m_xxx->draw()`）。
- **`PanelVisibility` 不是存储，是「命名引用视图」**：权威存储在 `PanelRegistry.cpp` 的
  `bool g_open[kMaxPanels]`。**必须是固定数组，不能用 vector**（持一堆 `bool&`，扩容就悬垂）。
  **必须是引用不是每帧拷贝**：`MenuItem(..., &p.stats)` 直接写指针，副本改了不回写。
- **面板快捷键一律 `Alt+1..8`**。`Ctrl+1/3/7` 与 `Ctrl+2/4/6/8` 被 `handleViewShortcuts()` 的
  视图方向键占了；工作区用 `Ctrl+Alt+1..6`。三条顺序硬约束：`Ctrl+Alt` 分支排
  `handleViewShortcuts()` **之前**；按住 Alt 时不调它；`Shift+Space`（最大化）排
  `Space`（播放/暂停）**之前**且 Space 分支排除 `io.KeyShift`。
- **`autoRatio` 逐节点**（不是一个全局 `m_userAdjusted`——那样拖根节点会冻住右区的自适应）。
  **面板关掉时那一侧收成 0**，且不画那条没意义的分隔条。
- **折叠 `collapsed` 与 `ratio` 的耦合**：折叠靠 `ratio = 0/1` 实现，而 `0` 又是"自适应"哨兵 →
  落盘时折叠中的节点必须**写真实比例**、读盘时走独立分支（`setRatio` 顺带 `autoRatio = false`）。
  否则折叠一读回来就丢（而 `collapsed` 还是 true，自相矛盾）。
- **`m_maximized` 只有一份权威状态**（在 `SplitLayout`），其余三个面板写 0×0 +
  `EditorApp::drawPanels()` 用 `showsInLayout(slot)` 跳。`computeLayout()` 只在
  `L.viewport.w/h > 1` 时才从视口底部扣播放条（否则最大化 Hierarchy 后底部凭空冒出一条）。
- **多视口 = 挂在 `m_targets` 尾部的 RenderTarget**。**必须进 `m_targets`**：相机 UBO 槽按
  `m_targets` 下标分配，同一命令缓冲录多台相机每台要自己的槽。`SubviewSlot::target` 是
  **非拥有裸指针**（写 `unique_ptr` = 双拥有 = 运行时双重释放）。缩容 `erase` 后必须
  `reindexTargets()`。`setSubviewCount(0)` 是默认值 → 单视口下逐像素不变。
- **`forceShaderReloadScan()` 不直接 poll**：把 `m_shaderReloadTick` 顶到 15 让**下一帧**越过节流；
  直接 `poll()` 会把"变了"消费掉 → `drawFrame` 那轮看不到 → 管线永不重建。
- ⚠ **`TR-BAR` 那一行只能是灰阶**：`verify_add_menu.py` 断言 2 取该行"高饱和"（`max-min > 45`）
  像素的质心、断言落在水平正中。任何彩色控件都会把它拽偏 → **误报 FAIL**。Transport 加的
  时间轴区块全部 `PushStyleColor` 成灰阶，且只吃左侧（`leftBudget = centerX - 24`）。
- ⚠ **两条分隔条会叠在同一像素**（某节点折到 0/1 时压在父节点线上）→ 必须有"悬停独占"
  （子树先于父节点，先到先得），否则一次双击"折一条 + 展一条"同时发生。

## 构建
- 入口 **`make build`**（`Makefile` + `make.cmd`）；`TOOLCHAIN=auto` → **本机走 MSVC**（MinGW 整条缺失）。
- **MSVC 四坑**：① `https_proxy` 与 `HTTPS_PROXY` 并存 → MSBuild 假报 `MSB6001: CL.exe 开关无效`；
  ② UTF-8 无 BOM + 中文注释 → `C2001`，对策 `/utf-8`；③ `.cmd`/`.bat` **必须纯 ASCII**；
  ④ Make 可能用 sh 跑配方 → `echo` 里不能有 `(` `)` `|`，`$(wildcard)` 探含空格路径必须转义。
- ⚠ **资产拷贝必须挂 `add_custom_target(... ALL)`，禁用 `add_custom_command(TARGET ... POST_BUILD)`**
  —— 后者只在**重新链接**时跑 → 改 shader/贴图**不生效**，会连续多轮测到旧资产。
  输出用 `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}`（用 `$<TARGET_FILE_DIR:Editor>` 会循环依赖报错）；
  `assets/scenes|scripts` **保持 POST_BUILD**（编辑器运行时也会写）。其余构建约定见归档。

## 环境变量（全表见归档）
- 断言必备 **`MYVK_EDITOR_NO_HUD=1`**（关 FPS/帧时间等叠加层）+ **`MYVK_GRID=0`**。
- 编辑器 `MYVK_EDITOR_SCENE` → `EDITOR_CAM`（**顺序不能反**）；`MYVK_FRAMES` / `LOG_RECTS`。
- **布局范式（E04~E08）新增**：`MYVK_EDITOR_WORKSPACE=<level|rendering|character|animation|
  cinematic|debug>`（启动切工作区）、`MYVK_EDITOR_WORKSPACE_SEQ="30:debug 60:level"`
  （运行中**按帧**切 —— E05 验收要"切过去再切回来"，一次会话得切两次）、
  `MYVK_EDITOR_VP_LAYOUT=<single|2v|2h|quad>`、`MYVK_EDITOR_LAYOUT=<布局文件路径>`。
  ⚠ **落盘的读/写不对称**：自动化运行（`MYVK_FRAMES`/`MYVK_NO_WINDOW_SAVE`）下**默认路径不读**
  （残留布局破坏像素级对照）**但显式 `MYVK_EDITOR_LAYOUT` 照读**（脚本自己要求的文件，
  否则落盘没法自动化验）；**写则一律拒绝**（别把测试的布局存成"用户的"）。
- **`MYVK_SSS=f`**：皮肤曲率缩放（默认 8）；`=0` ⇒ 退化成 Lambert 的对照组；**`≥24` 已饱和**。

## 渲染管线（改动前必读）
Pass 序：`Shadow(4 级级联) → LocalShadow(12 层数组) → DepthPrePass → SSAO(compute) →
ClusterBuild(compute) → VolumetricFog(compute×2) → Forward(天空盒→不透明→栅格→透明) →
PostProcess(Bloom+Tonemap) → 交换链`。
- **compute 不能在 RP 内执行** → 深度预通道独立成 RP 且**无条件执行**（只把"画几何"包开关）。
- **栅格必须夹在不透明与透明之间**（吃深度测试 + 不写深度），且**绝不能**做成 ImGui 叠加层。
- **阴影**（细则见归档，最易忘三条）：矩阵必须 **Zero-to-One**（`glm::orthoRH_ZO`）；CSM 每级范围 =
  切片包围球、球心在**光轴基**量化到纹素网格（**不能量 `view*mid`**）；局部光 near =
  `clamp(range/20, 0.1, 2)`、深度偏移以**米**给再按参考深度换回 NDC。`ShadowPass` 在自己 RP 内绑 set0
  （**别搬进前向 RP**）。
- **分簇**：簇编址 C++ / `cluster_build.comp` / `pbr.frag` **三处逐字一致**；`maxLightsPerCluster`
  唯一来源是 C++（`ubo.lightParams.w`），**禁硬编码**；固定槽位 → 结果可复现。
- **FrameGraph**：新效果登记依赖、**关时不得录 pass**；读 MSAA 深度要另编 `*.msaa.*` 并进 CMake 列表；
  `recreateScreenSpace()` 早于 `postInputViews()`。
- ⚠ **`FrameUBO` = 768B**（720 → 736 `pomParams` → 752 `decalParams` → **768 `probeParams`**，
  末尾 offset **752**；`RenderTypes.h` 有 `static_assert(sizeof(FrameUBO) == 768)`）。改它必须同步**所有**
  声明它的着色器（`pbr.vert/frag`、`sky.vert`、`cluster_build.comp`、`shadow.vert`、`fog_inject.comp`、
  **`decal.vert/frag`**）；新字段**只能加末尾**。
- ⚠ 推常量 **128B 全满**（`skin.w` = alpha cutoff，符号位 = 抖动开关；**`skin.y` = 逐材质 POM 深度**）。
  **旁路管线的 range 必须与主管线
  逐字段相同**（Vulkan 把 range 与阶段算进"布局兼容"）→ 否则 VUID-08600，整个 Pass 的 set0 一起挂。
- **set0 = 25 binding**（0 FrameUBO … 18 lightTex / **19 sssLut** / **20/21/22 = DBuffer 三张**
  （反照率/世界法线/粗糙度-金属度）/ **23 = `samplerCubeArray probeMap`** / **24 = `ProbeBuffer` SSBO**，
  全表见归档）。新增要同步
  `kGlobalSetBindings` + `writeGlobalSet` + 着色器 layout **三处**。
  ⚠ DBuffer 缺省槽位必须绑 **`m_defaultTransparent`**（RGBA 0,0,0,0）—— **不能**用
  `m_defaultBlack`（alpha=255 → 整屏被当成 coverage=1 的黑色贴花）。
  ⚠ 探针的 binding 23 缺省槽必须绑那张 **1×1 全黑 cube array（6 层）兜底图** ——
  **不能"不绑"**：pbr.frag 声明了它，只要 layout 有该资源，创建任何用 pbr.frag 的管线都必须有
  有效描述符，否则 `VUID-VkGraphicsPipelineCreateInfo-layout-07988`（与 SSAO 缺省白图同辙）。
- **材质贴图 set = 7 个**（set1 albedo … set6 normal / **set7 heightMap**）。Vulkan 规范只保证 4 个
  绑定集，本引擎实际要 **8 个**（RTX 3060 报 `maxBoundDescriptorSets=32`，AMD/Intel/Adreno/Mali
  普遍报 8，正好卡在线上）→ **再加一张贴图槽就得改架构**（并进 set0 数组或改 bindless 专用）。
- 全 SRGB，**着色器内不做手动 gamma**；`addFace(n,u,v,half)` 中 **u×v 必须 = n**。

## 变体与关键字
`ShaderKeywords.h` 唯一事实来源；**位序三处逐字对齐**：`kKeywords[]` / `CMakeLists` 的
`SHADER_KEYWORDS` + `VARIANT_DEFINES_ALL` / `pbr.frag` 的 `#if`。
bit0 `DOUBLE_SIDED` / bit1 `ALPHA_BLEND` / bit2 `NORMAL_MAP` / bit3 `ALPHA_MASK` /
**bit4 `SHADING_SKIN`** / **bit5 `PARALLAX`** → **64 变体**（**变体 ID = 掩码本身**）。
- 变体表由 CMake 循环生成（**不手写**）。⚠ 空串会被 `list(APPEND)` **丢弃 → 位序整体错位**：
  空项存哨兵 `"_"`、`list(JOIN DEFS " " DEFS_STR)` 拍平、末尾 `list(TRANSFORM ... REPLACE "^_$" "")`；
  每套必须 `separate_arguments(DEFS NATIVE_COMMAND ...)` 再喂 glslc。
- 硬约定：绑定缓存哨兵**不能是 0**（0 是合法掩码）→ `kBindsNothing = 0xFFFFFFFFu`，栅格后重置；
  `SHADER-VARIANTS` 日志必须打在前向 Pass **之后**；变体**共用同一 `VkPipelineLayout`**。
- `#extension` 必须在所有非预处理语句之前。全局开关（bindless / instancing / MDI）走独立 .spv。

## 材质（Alpha 模式 + 着色模型 + POM）
- `AlphaMode`：Opaque / **Blend**（不写深度、透明队列降序、**跳深度预通道**）/
  **Mask**（`alpha < cutoff` → `discard`、写深度、走**不透明队列**、**跳深度预通道**）。
  **队列分类唯一判据是关键字位** `hasKeyword(kw, kKeywordAlphaBlend)`。
- **cutoff 载体 = `push.skin.w`**；**符号位 = 抖动开关**（负 → Bayer 4×4）。唯一出口
  `Renderer::alphaCutoffLane(mat)`（非 Mask 恒推 +0.5）。MSAA 下开 a2c + 预映射
  `clamp((alpha-cutoff)*2+0.5)`；⚠ **a2c 只允许 `samples != 1`**（1x 下开会让创建管线报错）。
- **`ShadingModel{Default,Skin}`**：Skin 把漫反射乘子从 `NdotL` 换成预积分 LUT 查表
  `texture(sssLut, (N·L*0.5+0.5, curv))`；**方向光 / `shadeLocalLight` / `shadeLocalLightMod` 三处都要分支**；
  曲率 `clamp(length(fwidth(几何法线)) * sssParams.x, 0, 1)` —— **必须用扰动之前**的 N；
  采样用 `textureLod(...,0)`（隐式 LOD 会糊掉响应）。
- ⚠ **查表坐标必须钉到纹素中心** `(x*(N-1)+0.5)/N`：采样器是 **REPEAT**，而端点 `u=1` / `v=0`
  （平面 `fwidth(几何法线)=0` → 曲率恒 0）**正好落在纹理边界** → 双线性掺进"绕到另一侧"的纹素
  → **平坦皮肤无端泛红**（R4-2 实测 0.20 → 5.61）。**这条适用于所有查表类贴图。**
- 序列化：`alphaMode`（字符串，**保留旧 `alphaBlend` 布尔兼容**）+ `alphaCutoff` + `ditherAlpha` +
  `shadingModel` + `heightMap` / `heightScale`。**色彩空间是契约**：albedo/emissive `srgb=true`；
  **normal/ORM/高度图等数据表必须 false**。
- **POM（R4-3）**：`Material::heightMap` + `heightScale`（UV 单位）→ 关键字 **bit5 `PARALLAX`**。
  全部贴图采样改用 `parallaxUv()` 的结果 `matUV`，但 **TBN 必须仍用未位移的 `vUV`**（位移后的 UV
  在淡出/早退的**分支边界**不连续 → `dFdx` 拿到错的切线基）。几何法线要在 POM 里**重算一份**
  （含双面翻转），不能借用随后被法线贴图扰动过的 `N`。`scale <= 0` 第一步就 `return uv`
  —— 这就是"heightScale=0 逐像素不变"的依据（全局 `MYVK_POM=0` 同路短路）。
  ⚠ 高度图必须 **`srgb=false` + REPEAT**：sRGB 解码会把 0.5 压成 0.216（起伏形状被扭过）；
  视差一旦生效 UV 必然被推出 [0,1]，CLAMP 会在边界拉出硬缝。

## 延迟贴花（R4-4 DBuffer，改动前必读）
- 一条贴花 = 一个**盒体**（渲染器自建单位立方体，**画正面 / cull back**）。片元**不采盒体表面**，
  而是从深度图 `texelFetch` 重建场景表面世界坐标 P → `worldToLocal` → `|local|/halfSize` 最大值
  `m > 1` 就 `discard`。**这条盒内测试同时替代了深度测试** → pass 里**没有深度附件**
  （场景深度是 MSAA 4x，DBuffer 是单采样，采样数不同不能共用同一 RP 的深度附件）。
- ⚠⚠ **深度附件里存的是 NDC z 本身，不是 `(ndc+1)/2`**：投影是 `glm::perspective` 且**没有**
  `GLM_FORCE_DEPTH_ZERO_TO_ONE` → NDC z ∈ [-1,1]；Vulkan 深度范围**恒为 [0,1]** → `z_f = ndc_z`。
  所以重建**不能**照抄 `space.glsl::vennViewPosFromDepth` 的 `depth*2-1`（那是二次映射 → 重建点
  被拉近 ~1.7 倍 → 贴花全不可见）。`worldPosFromDepth` 直接用 `vec4(uv*2-1, depth, 1.0)`。
- ⚠ `worldToLocal` = **`glm::inverse(base)`（不含 size 缩放）**，**不是** `inverse(it.world)` ——
  否则 `local` 被反成单位立方体坐标，而盒内测试与 uv 投影（`local.xy/size.xy`）都按**米**写。
- 法线局部→世界用 **`transpose(mat3(rec.worldToLocal))`**（刚体逆的转置 = 旋转本身），
  **不能**用 `mat3(rec.world)`（含非均匀 size 缩放 → 法线扭歪）。
- ⚠ **两处 `dFdx/dFdy`（重建法线的 `dFdx(P)`、贴图 uv 的 `dFdx(duv)`）必须在所有 `discard` 之前
  无条件算完** → 贴图采样走 `textureGrad`，**不是** `texture()`。被丢弃的辅助调用导数是垃圾。
- `DecalRecord`（std430 **192B**：world/worldToLocal/tintOpacity/sizeEdge/params/fade）走 **SSBO**
  而非 push（两个 mat4 = 128B 就吃满 push 下限）；push 只剩 4B 下标。
  `DecalBuffer` 自有 set0 = `{FrameUBO, 深度图, 记录 SSBO}`，set1 = 逐贴花 `{albedo, normal}`。
- DBuffer = 3 × `R8G8B8A8_UNORM`（**必须线性**，法线/粗糙度做 sRGB 解码会整体扳偏）。
  混合语义（`PipelineDesc` 只有一套）：`rgb = src·src.a + dst·(1-src.a)`、`alpha = src + dst·(1-src.a)`
  → 清屏 `alpha=0` ⇒ 前向跳过整段（这就是"关 = 基线逐像素不变"的实现依据）。
- 全局开关走 **`ubo.decalParams.x`**（**不是**关键字位 —— 贴花开关可随时切，关键字位留给材质属性）。
  `MYVK_DECALS` 默认**关**；`kMaxDecals = 64`/帧，超出丢弃并 WARN。
  GPU 计时 `kSectionCount` 9 → **10**（新 `kDecals`），`CULL-STATS` 尾部 ` decal=%u`。
- ⚠ **析构必须真的销毁资源，不能只 `vector.clear()`**：一个 `DecalFrame` 带
  image + view + memory + framebuffer 四个**裸句柄**，`clear()` 只析构元素、不调 `vkDestroy*`
  → 3 张图 × 2 帧 = **20 个对象泄漏**。`DecalBuffer` 是**无条件构造**的（与 `MYVK_DECALS`
  无关），所以泄漏每次运行都在 `vkDestroyDevice()` 被点名。
  **凡是持有裸 Vk* 句柄的帧结构，析构都要走 `destroyFrames()`**（对照 `VolumetricFog` /
  `SSAOPass` 的写法）。

## 反射探针（R5-1 ✅ 2026-10-15，改动前必读）
逐项实现 + 21 项验收 + 坑点全表 → `2026-10-15.md` §12。这里只留**以后还会踩到的规则**：

- ⚠⚠ **UBO 是"记录期写、执行期读"的内存 —— 同一份 UBO 在同一个提交里给多个视角/目标复用，
  必然全部用最后一份的值。** R5-1 的头号 bug：6 个立方体面共用一个相机 UBO 槽 → 6 面全按第 5 面的
  相机渲染；而绘制列表是**记录期**按各面相机剔除的 → 只有最后一面自洽，表现成
  **"只有最后一个面有内容"**（极易误诊成拷贝/面序问题）。
  **修法 = 每面独占一个 UBO 槽**（`Renderer::kProbeFaceUboSlotBase = 64`，base+0..5）
  **+ 一个探针一次独立提交**。离屏目标（`FrameResources::OffscreenUbo`）踩过同一个坑 —— 这是第二次。
  **排查手法**：切分实验（让 6 面全用同一个面的相机 → 若"六面都有内容"就证明问题在渲染不在拷贝）。
  排除过两条错误假设：① "cube-compatible 图像拷贝必须 6 层一组"（**证伪**，逐层 region 是标准做法）；
  ② "缺内存屏障"（加全量 `ALL_COMMANDS` 屏障结果**零变化**）。
- ⚠ **一个探针 = 一次独立提交**：`rhi::CommandPool` 的 cmd 缓冲**不能录两次/提交两次**，
  多探针必须各取各的 `beginOneTime()`。
- ⚠ **mip 链长必须与分辨率自洽**：N×N 的图最多 `1+log2(N)` 级（8×8 只支持 4 级）。
  超了是**规范违规**，图像创建失败、**整条捕获链作废**（表现"开了探针但什么都没发生"，不崩）。
  入口已把 `prefilterMips` 夹紧到 `1+log2(prefilterSize)`。
- ⚠ **探针混合在 IBL 总开关内部**（`pbr.frag` 的 `if (ubo.envParams.x > 0.5)`）：
  探针替换的就是 IBL 的镜面预过滤项。**`MYVK_PROBES=1, MYVK_IBL=0` 会静默失效**
  （已在面板 hint 写明，但**没有**把混合移出 IBL 分支 —— 有意识的取舍）。
- ⚠ **`ambientScale` 会把探针一起乘没**（`ambient = (diff+spec) * ao * ambientScale * ssao`）
  → 验收场地必须 `ambientScale = 1.0`。
- ⚠ **GLSL 里 `struct Probe` 必须定义在 interface block 之外**（结构体不能定义在块里）。
- ⚠ **探针分区默认折叠**（`sectionHeader(..., defaultOpen=false, ...)`）→ 参数行在
  `if (!open) return;` **之后**，headless 点不到 → 断言只能做"节标题在不在"（用 `INS-PROBE-HEADER`）。
- 回读自检报**三个交叉验证指标**才可信：`PROBE-DUMP`（逐面加权平均亮度，会被"大面积暗+一小块亮"
  压扁）/ `PROBE-MID`（逐面中心纹素，单点说不清整面）/ **`PROBE-NZ`（逐面非黑计数，最不容易自欺）**。
- **探针判定的三条或关系**：① 显式请求；② 数据变了 或 `captures == 0`（首帧必烘）；
  ③ `autoCaptureFrames > 0` 且过期。**`autoCaptureFrames` 默认 0（关）** —— 默认是"变了才烘"。
- **组件**：`ecs::ReflectionProbeComponent`（`extents` 盒体半轴 / `intensity` / `boxProjection` /
  `blendDistance` / `enabled`），位置 = 实体 Transform；序列化字段 `probe`；
  ⚠ `SceneSerializer` 的存活条件是 `mesh|mat|decal|probe` **四者取或** —— 漏一个探针实体就被整个吃掉。

## GPU 粒子系统（R6-1 ✅ 2026-10-15，改动前必读）
逐项实现 + 34 项验收 + 实测表 → `2026-10-15.md` §13~§18。这里只留**以后还会踩到的规则**：

- ⚠⚠ **push constant 的"范围"必须与着色器里的 `layout(push_constant)` 块逐字节一致**，
  **每个** compute 管线都要写（`ParticleSystem::createPipelines` 里那条计数管线漏了）：
  `VkPipelineLayoutCreateInfo` 里不声明 → 验证层刷 `VUID-...-layout-07987`，
  `vkCmdPushConstants` 落点**未定义**，而且 `cfg.y`（总槽数）取到 **0**
  → `instanceCount = 0` → **一颗粒子都不画**，日志里只有那几行 VUID。
- ⚠⚠ **返回类型推导的 lambda：任何一个 `return 值;` 都会把返回类型定死**，要提前退出
  必须用**裸 `return;`**。混进 `return false;` 而末尾无 return 是 **UB**，
  MSVC Debug 会在末尾塞 trap 指令 → 整进程 `0xC000001D`，**一行日志都不给**。
  （本轮 `InspectorPanel` 的 `module()` 就是这么死的；另一个化身见 §14.2。）
- ⚠ **冻结帧（`MYVK_FX_FREEZE=N`）保证的是「状态」逐位可复现，不是「像素」逐像素一致。**
  两条固有不确实性：① **池按 frames-in-flight 分槽**，每槽只在自己被派发时推进，
  冻结后各槽停在**各自的最后一步**（差 1~2 步）→ 边界处那颗粒子生/死不同 →
  一条**集合**差异（所以**不随速度放大**，10 倍初速反而更小）；
  ② **紧凑表顺序来自 `atomicAdd` 完成顺序**，加色混合数学上无序但 **8 位 UNORM**
  下每片元 `+=` 都要量化 → 差值随**重叠的部分覆盖率像素**增长（软粒子开 1.59~2.13、
  关 0.058，27 倍）。⇒ 断言口径：**状态逐位一致（精确）+ 像素差有界（≤4.0）**。
- ⚠ **验证"序列化新旧等价"不要用像素** —— 用 `MYVK_FX_DBG=2` 的 **`FX-EMITTER`**
  （把交给 GPU 的发射器数据用 `%.9g` 逐值打出）。等价性退化成**字符串比较**。
  ⚠ 必须 `%.9g`（float32 十进制往返精度）；`%g` 默认 6 位会把**不同的值打成一样**。
- ⚠ **12 个模块的折叠键必须带 `fx/` 前缀**（`m_sectionOpen` 是全局表，`INSP-SECTIONS`
  日志会把它打出来 → 裸名 `playback`/`shape` 会污染 `verify_inspector` 的断言）。
- ⚠ **一键展开/折叠必须 `ImGuiCond_Always` 强推一帧**：`SetNextItemOpen(Appearing)`
  对**已存在**的 ImGui 节点不再生效，光改 `m_sectionOpen` 是点不动的。
- ⚠ **`CollapsingHeader` 的 ID 取 `"##"` 之后的部分** —— 12 个模块不能都叫 `"X##fx"`。
- ⚠ **`FX-PARAMS` 的 `modules=` 只登记「画出来的、且展开的」模块**；一个都没有时写
  **字面量 `(none)`** 而不是空串（验证脚本必须归一化）。所以测"搜索过滤"必须**先把
  12 个模块全展开**再搜，否则被命中的模块默认折叠 → 永远是 `(none)`。
- ⚠ **搜索的兜底判据必须与逐项放行判据同源**：只按模块**名**算兜底、却按名字**+关键词**
  放行，搜只在关键词表里的词（`billboard`）会得到"全部显示"，过滤等于没生效。
- **组件是纯 POD**（`colorRamp.stops[8]` / `bursts[4]` 全是数值，无 `std::string`/`vector`）
  —— 这是"整块进撤销栈"与 Copy/Paste Properties（`PK_ParticleEmitter`）的前提。
- **序列化两头都写**：`bursts` 数组 + `burst = bursts[0].count`；
  `colorRamp` 数组 + `colorStart/Mid/End = ramp.eval(0/0.5/1)`；读则反之回落。
- **色带撤销记整条带子**（拖完会 `sort()` 重排，按单个停靠点记会让撤销写回**别的**点）。
- **`Add stop` 插在最大空隙中间并取该处 `eval()` 颜色** ⇒ "加点从不改变外观"。

## 编辑器 / UI 架构（改动前必读）
架构全文 → `docs/editor_architecture.md`（六层：L0 注册表 → L5 自动化契约）。
**可执行任务清单 → `docs/editor_roadmap.md`（E01~E29，按阶段 A 地基 / B 布局 / C 面板 /
D 交互 / E 生产力，每项含改动点 + 涉及文件 + 机器可验证的验收 + 规模 + 依赖）。**
**开工前先看清单的 §1 依赖总览 —— 一次只做一件时的推荐直线序在那一节。**

**实测欠账（2026-10-14）**：32 个特性里 **8 个在编辑器里点不到** ——
SSAO / SSGI / SSR / God rays / SSS / 阴影级联硬化 / 解析式高度雾 / Instancing。
157 个 `MYVK_*` 里 119 已登记、38 白名单、0 裸 env。
**根因**：env（构造时读，给测试）与 UI（运行时改，给人）是**两套平行世界**，
都能写同一个设置结构体，但**谁都没有义务覆盖全部字段** → 字段加了、面板忘了。
**✅ E01 补上"事实源"、E02 补上"控件"、E09/E10 把 8 组欠账清零** ——
2026-10-15 实测 `check_feature_surface.py`：**33 个特性全部有 UI 面、裸 env 漏网 0**
（R5-1 反射探针是清零后新增的第一个特性，走完五件套、**没有**再产生"只有 env"的欠账）。

**四条关键约定**：
- **SSOT**：每个可调项只有一份权威存储。现有 4 个设置结构体就是存储
  （`PostSettings` / `CameraEffectSettings` / `GridSettings` / `ScreenSpaceSettings`），
  注册表只加"元数据"、**不动布局** —— ⚠ 实际落地时**没有用 `offsetof`**，
  而是运行时 `bindParam(id, &成员, kind)` 把地址交给注册表（`Renderer.cpp` 有私有访问权，
  所以连成员重命名都不需要）。见上文「特性注册表」一节的"与原方案的差异"表。
- **五路自动派生**：登记一次 → env 读取 / UI 控件 / 序列化 / 工具提示 / 回归触发。
  手写量从"每参数一次"降到"每特性一次"。
- **分级暴露**：每参数标 `Basic/Advanced/Debug`，默认只显示 Basic（158 个旋钮平铺 = 没人能用）。
- **覆盖链**：编辑器偏好 → 项目 → 场景 → 对象 → 相机。UI 上每控件带覆盖指示器
  （`●` 已覆盖 / `○` 继承 / `◇` 默认）。
- **默认值一律保持"全关"**：`config/render.json` 缺失时行为**逐像素等于今天**，
  **不得破坏现有 39 例回归基线**（硬约束）。

**改造顺序**（详细任务见 `docs/editor_roadmap.md`）：
阶段 A 地基（E01 注册表 + E02 生成层 + E03 设置层级，**先做，否则后面全是手写**）→
阶段 B 布局范式（E04 面板注册表 / E05 工作区 / E06 分栏增强 / E07 多视口 / E08 菜单工具栏）→
阶段 C 核心面板（E09 Engine 重构 / E10 补齐无 UI 特性 / E11 Inspector / E12 Hierarchy /
E13 ContentBrowser / E14 Viewport / E15 Stats / E16 Console / E17 Preferences）→
阶段 D 交互理念（E18 命令面板 / E19 设置可撤销 / E20 覆盖指示器 / E21 通知进度 /
E22 Gizmo选择 / E23 快捷键表）→
阶段 E 生产力（E24 资产编辑器 / E25 GPU 时间轴 / E26 画质档位 / E27 RenderGraph /
E28 打包发布 / E29 烘焙流水线）。
**里程碑 M1 = E01+E02+E09+E10**（做完 `check_feature_surface.py` 退出码 0 = 23 组特性全有 UI 面）。

**已有承接物（复用，别重建）**：`SplitLayout`（手工分栏树，**不引入 ImGui docking**）·
`CommandStack`（`LambdaCommand` 可包一切设置改动）· `GizmoController` · `PickingSystem` ·
`ContentBrowser` · `ThumbnailCache` · `EditorDragDrop` · `LogTap`/`MYVK_LOG_RECTS`/`NO_HUD`。

## 核心面板（E09~E17 ✅ 2026-10-15，改动前必读）
一句话：**"每个面板都能搜、能过滤、能拖、能落盘，且每项都有自己的 `verify_*.py` 且 PASS"**。
逐项实现 + 取证 + 后延记账 → `docs/editor_roadmap.md` 的「阶段 C 落地记录」；
过程坑点全表 → `2026-10-15.md`。这里只留**以后还会踩到的规则**：

- **编辑器侧的注入 env 一律 `MYVK_EDITOR_` 前缀**（`MYVK_EDITOR_CB_TYPE` /
  `_INSP_SEARCH` / `_HIER_FILTER` …）。`check_feature_surface.py` 的白名单**按前缀放行**，
  不带前缀的自定义注入点会被判成"裸 env 漏网"（E13 的 `MYVK_CB_*` 就这么翻车的）。
  这也让"渲染特性 env"与"编辑器注入 env"一眼可分。
- **新面板 = `PanelRegistry` 加一行**（View 菜单 / 快捷键 / 图标 / 落盘四路自动派生）；
  面板之间**只准依赖 `EditorContext`**，要动引擎就走 `EditorHost` 纯虚接口。
- **Inspector**：分区折叠状态按**分区名**存（`m_sectionOpen`，换实体不重置）；
  Copy/Paste/Reset 是 **POD 字节快照**（`sectionPodOf` 返回 `{ptr,size,def}`），
  写入必须经 `RawBytesEditCommand`（可撤销）。`insp.selftest` 是机器可验证的自检命令。
- **Hierarchy**：过滤要**保留祖先链**（`subtreeMatches` 递归查后代，未命中的祖先灰显）；
  增删实体**不在遍历 ECS 的过程中做** —— 走 pending 队列，树画完 `flushPending`
  统一经 `structuralEdit`（带撤销快照）；
  ⚠ **状态列的 `InvisibleButton` 必须画在右键菜单之后**（`BeginPopupContextItem`
  绑的是"最后一个 item"，抢在它前面会让右键菜单失效）；拖放落点 `Dummy` 的**宽度必须铺满**
  （写 0 = 无可命中矩形，真鼠标拖不到）。
- **ContentBrowser**：面板默认只有约 290px 宽 —— 工具栏必须**单行**，
  面包屑走**三级降级**（全显示 → `...` + 末尾若干段 → 只留当前目录名）；
  `ThumbnailCache` 是 LRU，**GPU 上传必须在渲染线程**（真正的后台解码另外记账）。
- **E14 视口浮层**：必须是**子窗口**（独立顶层窗口会被焦点窗口盖住）；
  两层显隐 = 总闸 `m_showOverlay`（`MYVK_EDITOR_NO_HUD` 的落点）× 四块各自开关
  （`EditorContext::ViewportOverlay`，右键空白处 `Viewport Display` 菜单 + 偏好落盘）。
  **总闸一关四块全灭 —— 这是 NO_HUD 逐像素回归的前提。**
  ⚠ **四角布局 = 左上模式条 / 右上统计 / 左下相机信息 / 右下导航球**（E14 把导航球
  从**左下搬到了右下**）。浮层既然是子窗口，**就会先吃掉落在自己身上的点击** ——
  于是"点视口某角落当空地"的用例全废了（`verify_bvh` 就栽在这：它点右下角 94%,94%
  当空地，E14 之后那里是导航球 → `miss 日志 0 -> 0`）。
  **凡是靠"点视口空白处"做断言的用例，一律以 `MYVK_EDITOR_NO_HUD=1` 启动。**
  ⚠ 浮层子窗口的高度必须用 `GetTextLineHeightWithSpacing()` / `GetFrameHeightWithSpacing()`
  （不带 Spacing 的版本少算几像素 → 冒滚动条、最后两行被滚出可视区）；
  子窗口一律加 `NoScrollbar | NoScrollWithMouse`。
- **数字键已被两套快捷键占满**（`1/3/7(+Ctrl)` 视图方向、`Alt+数字` 面板显隐）
  → E14 的三件事全挂 Shift 层。⚠ **`Shift+N` 切的是模式索引 `N-1`**：
  `Shift+2 → unlit`、`Shift+5 → metallic`、`Shift+6 → wireframe`。
  书签跳转**只恢复 yaw/pitch、保留当前注视点**（比连位置一起还原好用）。
- ⚠ **`logRect` 不是逐帧刷的**：同 tag 只在矩形变化 >4px 时打一条。
  ① 想断言"某块还在不在画" → 用只在**状态变化**时打的 `VP-OVERLAY master=.. modeBar=.. stats=.. camera=.. navGizmo=..`；
  ② **不要拿 `VP-RECT` 去裁图**：视口首帧布局用的是"启动那一刻的窗口尺寸"，
     resize 与首帧的先后不同会留下不同首值（同一份二进制实测两种都出现过）。
     像素比对用**整窗**（含面板边框，最强），或视口内部稳定锚点 `VP_INNER=(330,200,1200,750)`。

## 交互理念（E18/E19/E21 ✅ 2026-10-15，改动前必读）
逐项实现 + 取证 + 两条设计取舍 → `docs/editor_roadmap.md` 的「阶段 D 落地记录」。
**E20 待 E03、E23 待做**。这里只留以后还会踩到的规则：

- **命令注册表（E18）= 编辑器里所有"动作"的 SSOT**：面板表 / 视图模式 / 工作区 /
  控制台命令 **全部从既有注册表派生**（`render::kFeatures[]`、`PanelRegistry`、
  `ConsolePanel::commands()`…），**不许手抄第二份清单**。加一条命令 = `reg.add` 一行。
  实测 `CMD-REGISTRY count=71`。命令面板、Help 菜单、快捷键表共用这一张表。
- **命令面板开着时 `handleShortcuts()` 必须停**（`if (!m_showCommandPalette)`）——
  否则用户在输入框里敲 "s" 会顺手触发吸附、敲数字会切视角。
  且 `drawCommandPalette(...)` **不能**放进 `if (m_showCommandPalette)`：env 钩子
  （`MYVK_EDITOR_CMDPAL`）需要"面板还关着"的那一帧来把它打开。
- **E19 参数撤销的钩子必须在 `SetTooltip` 之前取 item 状态**：`SetTooltip` 会提交
  一个新 item，之后 `IsItemActivated/IsItemDeactivatedAfterEdit` 问的就是 tooltip 了。
  `ParamEditCommand::name()` 返回 `const char*` → 名字**预拼**成成员（现拼要用静态缓冲，
  多条历史会互相覆盖）。合并是**三重条件**：同一命令对象 + 同 paramId + 500ms 窗口
  （外加"栈顶就是它"）。
- **E21 通知中心不复用 Console 的 sink**：`core::Logger` 有两条**正交**通道 ——
  `setLogSink`（显示端，Console 面板独占）与 `setLogHook`（通知端）。
  混用会让 Console 自己的回显行（`> cmd`）也变成 toast，且"有没有 Console"决定
  "有没有通知"。**只有 `Warn/Error` 自动升级**，`Info` 只能显式 `push`
  （否则启动几百条 Info 糊满屏）；同文本在 toast 存活期内**去重**（`repeat` 计数）。
- **toast / 任务条 / 历史窗口必须最后画**（ImGui 后画的窗口在上层），且游戏全屏
  提前 return 那条路上也要画 —— 全屏跑游戏时恰恰最需要"刚才出错了"。
- **点 toast 的落地在宿主**：`NotificationCenter` 只 `takeConsoleFocusRequest()`；
  `EditorApp` 负责 `panels().console = true` + `ConsolePanel::requestScrollToText()`
  （只记请求，真正的 `SetScrollY` 发生在它下一次 draw，帧内不做跨面板迭代）。
  UI 点击与自动化 `notif.click` 走**同一条** `clickToast()`。
- **进度取消的契约**：`requestCancel()` 只置位；`setTaskProgress()` 返 **false**
  才表示"取消已被消费" → **由宿主** `endTask()` 收尾。取消的任务**不发**完成通知。
- **`NOTIF-STATUSBAR` 是边沿触发**：只在（errors / warns / tasks / dirty）变化时打，
  **tool 提示不进判等**（它每帧都变，进来就退化成每帧一条的噪声）。
- 通知历史落盘 `config/editor_notifications.json`，守卫与布局/偏好**同一条规则**
  （自动化下拒写；显式 `MYVK_EDITOR_NOTIFICATIONS_PATH` 时读放行、写仍禁止）。
- ⚠ **整窗像素基线会因状态栏变化而红**：`verify_viewport.py` 的 V3 是"与 E14
  改造前的整窗参考图一致"。E21 给状态栏加了四个常驻项 → 实测差 4692 px、
  bbox 只在 y[825,840]（客户端底部 `kStatusH=26`）。**修法是拆断言、不重采基线**
  （重采 = 把当前二进制像素盖成"参考"，V3 退化成自己跟自己比）：V3 = 状态栏以上
  整窗 0 差；V3b = 差异全落在那一只条内且非零。**以后动状态栏/工具栏那两条带，
  先想到 V1b/V3 会不会红。**
- ⚠ NO_HUD 会话里**出现 WARN/ERROR 会让右上角多一条 toast**，同样会破坏
  V1b/V3 的整窗逐像素比对 —— 那说明渲染/脚本真报了警告，让它先红，别改阈值。

## 断言纪律（血的教训）
- ⚠ **采样区必须落在被测物体上**（俯角 8° 时视口**上半是天空盒**，"视口正中"只采到天空 → 误判"没生效"）。
  先拿哨兵确认可见性；用**面板区**或"有物体 − 无物体"现算掩膜再腐蚀几像素。
- ⚠ **必须 `MYVK_EDITOR_NO_HUD=1`**（`drawOverlay` 会画 FPS/帧时间）。
- 色调映射压平高光 → **别用固定倍数阈值**，用"逐级落差 > ε"或单调性。**"全有/全无"的开关测不出"分级"**
  → 取**敏感区间内**的中间刻度。
- **亚像素效果不能靠截图验证**；**"输入驱动"的行为别只靠像素差分**。
- **新写的断言必须做"否定测试"**（故意把实现退化回 bug 版本 → 断言必须变红）。**临时诊断代码用完必须清**。
- **"同一份产物、失败项每次不同" = flaky**，先别怀疑功能，检查断言的**度量口径**：
  ① 是不是拿"整幅图统计量"比较**样本数不同**的两个画面（用差异带/面板区）；
  ② 同一行里两个阈值是否自洽（`n` 统计 `>18` 却说 `mx > 8` 就算显著 → mx=17 时自相矛盾）；
  ③ 是否依赖**注入时序**（点击/焦点）。三类本项目各踩过一次。
- ⚠ **脚本结尾必须打 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号，且是行内可被 `OK_RE` 命中的写法）**。
  `run_regression.py` 只认 `RESULT: PASS` / `全部断言 PASS` / `^全部 PASS\b` / `结果: PASS` 四种，
  取**最后一条**匹配 → **自由格式（如 `PASS —— ...`）会让 rc=0 但判 FAIL**。
  ⚠⚠ **这条已经被踩第三次了**（R5-1 一次、R6-1 一次，写的是 `PASS: 全部通过`）。
  规则写在文档里显然不够 → **新脚本的第一件事就是照抄一个已通过脚本的结尾块**，别自己发挥。
- ⚠ **"抓图偶发偏一点"不要靠放宽阈值解决**：同一场景连续抓 9 次全 0.000000，整套脚本跑
  十几次里出现过一次 0.261（窗口抢占 / 捕获落在尺寸未落定的一帧）。处置 = **失败时重抓 N 次
  取最小差**，判据一个字不放宽。**放宽门槛 = 把断言变成永远绿。**
- ⚠ **FAIL 了先怀疑「期望」再怀疑「实现」**：本轮 5 项 FAIL 里 3 项是**期望写错**
  （注入是空操作 / 没先展开被搜的模块 / 拿模块名的子串当"组件级关键词"），
  1 项是**新断言的口径本身不成立**（像素逐位一致），只有 1 项是真代码 bug。
- ⚠ **兜底条件必须与放行条件同源**：逐项放行的判据包含 A、兜底判据只看 B ⇒ 两者必然打架
  （实例：搜索过滤按"模块名+关键词"放行、兜底只按"模块名"→ 搜只在关键词表里的词会全放行）。
- ⚠ **"状态一致"与"像素一致"是两个断言，别混**：凡有**原子顺序 / 多槽分帧 / 8 位量化**
  的地方，前者可以做精确断言，后者只能做**有界**断言。先**实测**再定阈值，不要猜。

## 工具
- **`tools/check_feature_surface.py`**：特性↔UI 覆盖自检（**秒级、不跑 GUI、可进 CI**）。
  维护"特性登记表"（`FeatureDesc` 的离线投影），grep `editor/` 校验每个特性的 **UI 入口真的存在**。
  查 A 登记完整 / B 文档 / **C 无 UI 面★** / D 裸 env 漏网 / E 回归保护 / F GPU 段名 /
  G 陈旧登记。**C、D 组必须清零**。⚠ 加白名单 = 显式断言"用户不需要在界面摸到它"，加前三思。
  ⚠ 它**排除自己**再扫仓库，否则清单里的 env 名会让"每个 env 都存在"，G 组永远无法报红。
- `tools/check_regression_map.py`：映射表 + 影响分析的自检（同样秒级不跑 GUI）。
- `tools/run_regression.py`：读末尾 `结果: PASS`；**增量**；**新增 `verify_*.py` 必须加进
  `DEFAULT`**，并且顺手补一条 `TRIGGERS`。`run_regression.py` 自己**不参与任何用例的指纹**
  （它只是"跑法"）。`--explain` = 只打印影响分析不跑，**改完先看一眼会跑哪些**。
- **跳过 = 两道闸都放行**：
  ① **用例脚本**：该脚本 + 它递归 `import` 的本地 tools 模块没变（修一个脚本只重跑它自己）；
  ② **变更影响分析**：把构建输入（`WATCH_ROOTS`）拍快照存缓存，下次 diff 出"改过哪些文件"，
     按 `GLOBAL_GLOBS` / `TRIGGERS` / `IGNORE_GLOBS` 三张表映射成**关联用例集**，只有落在
     这个集合里的才重跑。实测：只改 `decal.frag` → 39 例里只跑 1 例（原来 30+ 分钟 → ~100 秒）。
     快照与上次完全一致 → **一例都不跑**（连"没登记"的也不跑，因为"没变 ⇒ 结果不变"）。
- ⚠ **映射表三条铁律**：
  1. **没登记 = 全量 + 点名**（保守兜底），**绝不静默跳过** —— 表可以增量补，补漏不会漏测；
  2. **不要把横切文件写进 `TRIGGERS`**（`GLOBAL_GLOBS` = `CMakeLists.txt` / `engine/rhi|core|**`
     / `engine/assets/**` / `Renderer.cpp` / `RenderTypes.h` / `ShaderKeywords.*` / `FrameGraph.*`
     / `Scene.h` / `common.glsl` / `pbr.frag` …）：命中横切永远**先**按全量处理，写进去是死代码，
     而且 `--narrow` 下会变成"看着有归属、其实没覆盖"的**假跳过**；
  3. **判定顺序 = 先横切 → 再忽略 → 最后映射**。`IGNORE_SUFFIXES` 里的 `.txt` 曾把
     `CMakeLists.txt` 当文档吞掉（改了构建配置却什么都不跑）。
- **横切文件是"文件粒度"的天花板**：`Renderer.cpp` 里改的是哪一段看不出来 → 只能全跑。
  确认真只动了一个特性时用 **`--narrow`**（作者显式断言其它特性不受影响）跳过这层，
  再把该特性用例**显式点名**（点名的永远跑）。
- **资源泄漏门**：全套里**只有 `verify_collision_play` 会让编辑器优雅退出**，因而只有它的
  日志里会出现 `vkDestroyDevice(): ... leaked objects`（其余脚本都是强杀进程，泄漏报告根本
  不产生）。所以它被挂上**整个渲染层**（`RENDER_MODULES`），回归脚本另有一道**搭车式兜底**：
  扫本轮跑过的每个用例日志，命中泄漏行就报 `== 横切检查 ==` 并把退出码置 1。
  ⚠ 这条门是"只跑关联用例"的**必要配套** —— 跳过的用例不再提供覆盖率，横切不变量必须有专责。
- ⚠ **不要用 `engine/render/**` 这种整片 glob 当触发**：它会把"新增的、还没人认识的 render
  文件"也判成**已登记**，从而绕过"未登记 → 全量"的兜底。要用 `RENDER_MODULES`（逐个模块列），
  `check_regression_map.py` 的 [F] 项会核对"engine/render 下每个文件要么在 GLOBAL、要么在
  RENDER_MODULES"，漏列会被抓出来。
- 快照**不含运行时数据**（`assets/scenes`、`assets/scripts` 是编辑器运行时会写的目录），
  也不含 `tools/`、`docs/`、`.workbuddy/`。
- `tools/check_regression_map.py`：**映射表自检**（秒级、不跑 GUI）。检查影响分析的 13 个行为
  分支、快照覆盖范围、映射覆盖率、**死映射**（写了不存在的路径）、**全横切登记**（假跳过风险）。
  改完映射表就跑它。
- 共用设施 `capture_window.py` / `verify_add_menu.py`（`LogTap` / `kill_stale_editors` / `wait_rect`）。
  ⚠ 抓图**一律用 `capture_nonempty`**，别直接 `capture_client(...)[0].save(...)`。
- **阶段 C 的面板验收**：`verify_inspector.py`（E11）/ `verify_content_browser.py`（E13）/
  `verify_viewport.py`（E14，40 项断言）。**全量参考图** `tools/vp_nohud_baseline.png`
  （1578×844 整窗，NO_HUD，E14 改造前）。
- **阶段 D 的验收**：`verify_command_palette.py`（E18）/ `verify_undo_settings.py`（E19）/
  `verify_notifications.py`（E21，5 组）。**渲染侧 R5-1**：`verify_reflection_probe.py`（21 项，
  八联对比图 `_venn_verify/probe_compare.png` 由脚本自动重生成）。
  `DEFAULT` 现 **47 例**。
  这一批的注入钩子：`MYVK_EDITOR_CMDPAL[_EXEC]`（命令面板）、`MYVK_EDITOR_CONSOLE_CMD`
  （控制台命令，`undo.selftest` / `notif.*`）、`MYVK_EDITOR_TOAST[_CLICK]`、
  `MYVK_EDITOR_ASYNC_TASK[_CANCEL]`、`MYVK_EDITOR_NOTIF_HISTORY` —— 全部 `MYVK_EDITOR_` 前缀。

## 生产力（E24~E29 🟡E25✅ 2026-10-15，改动前必读）
逐项实现 + 取证 + 三条设计取舍 → `docs/editor_roadmap.md` §6 末尾的「阶段 E 落地记录」。
**E24 / E28 依赖已就绪（可立刻开工）；E26 卡 E03；E27 卡渲染侧 R7-1；
E29 的 R5-1 前置已满足（2026-10-15 ✅）→ 探针那一路可开工，**只剩 R5-2 lightmap 未落地**。**

- **GPU 时间轴（E25）= `editor/GpuTimelinePanel.h/.cpp`**，与 Stats 面板（E15）**分工不重复**：
  Stats 画"本帧 10 段按占比铺一条"（看谁最贵），时间轴画"最近 60 帧纵向堆叠"（看抖不抖）。
  **历史由面板自己攒** —— `GpuTimer` 只持"最近一帧"（加历史就要跟 in-flight 对齐，是它不该管的事），
  面板每帧 `sample()` 写进 60 槽环形缓冲，采样是**纯读**。⚠ 采样在 `onUpdate` 里无条件做
  （面板关着也在积累，打开即见完整 60 帧），所以面板**默认必须 `defaultOpen=false`** —— 它盖在视口上。
- **"时间轴总长"的诚实定义**：`GpuTimer::lastTotalMs()` 是"首 begin → 末 end 的跨度"，
  而甘特图只能画 10 段的**各自时长** → `lastTotal = seg_sum + gap`。验收（误差 < 5%）
  落地成三条断言：`seg_sum ≈ last`、`gap ≥ 0 且 gap/last < 5%`、面板 `last` 中位 vs
  `GPU-TIMING` 的 `frame` 中位同量级。**别改成"让总长恒等于 total"** —— 那会丢掉
  "各段确实覆盖了整帧"这条证据。
- **锚点**：`GPU-TIMELINE frames=.. last=.. seg_sum=.. gap=.. avg=.. max=.. hot=<段>:<ms> sel=<段|->`
  （`MYVK_EDITOR_GPU_TIMELINE=1` 时每 30 帧一条）、`GPU-TIMELINE-CLICK seg=.. index=.. ms=..`、
  `CONSOLE-FILTER text=..`（`ConsolePanel::setSearchFilter`）。点段联动复用**现成的 Console 搜索框**，
  不发明新过滤机制。
- ⚠ **给面板补"启动即改状态"的 env 钩子时，要自己打一次 `logPanelStates()`** ——
  `onInit` 里没有任何 `logPanelStates()` 调用点，不打就只剩 `loadEditorLayoutFromDisk`
  的 `closed`，"钩子生效了"没证据。

### ⚠ 断言纪律补充：`MYVK_FRAMES=N` 会让进程"几秒跑完"（2026-10-15 踩）
- **症状**：`capture_stable()` 恒返回 `None`；`GetClientRect` 返回 **`0×0`**、`PrintWindow` 返回 `False`。
- **根因**：`MYVK_FRAMES=900` 实测进程只活 **2.07 秒** —— 无 vsync 时帧率 ~450fps。
  "启动 → 等日志 → 截图"拿到的是**已退出的窗口**（枚举顶层窗口时 `Venn Editor` 已不在）。
- **纪律**：**需要截图的用例不要用 `MYVK_FRAMES` 控帧**，改成"等条件出现 + 主动关窗"。
  `MYVK_NO_WINDOW_SAVE=1` 已足够让 `automationRun()` 为真（写盘照样禁止），不必给 `MYVK_FRAMES`。
- **键鼠注入的可靠组合**（`verify_viewport.py::press_combo` 是范式）：
  **修饰键必须用真实 `keybd_event`** —— GLFW 的 `getMods()` 走 `GetKeyState`，
  PostMessage 出来的修饰键在那里看不见；**主键用 `PostMessage` 直投窗口**（不依赖前台焦点，
  但**必须带扫描码**，lParam 里扫描码为 0 会被映射成 `GLFW_KEY_UNKNOWN`）；
  **任何点击/右键前先 `force_foreground`** —— `mouse_event` 只进前台窗口队列，
  不在前台时**不报错**，只表现为"菜单没弹 / 点了没反应"（`verify_add_menu` 的
  attach-thread-input + ALT 绕法）。注入是**固有 flaky** 的 → 断言写"按到生效为止"
  （`press_until(needle)` 重试并记录次数），**不要**断言"这一次 PostMessage 没被系统吃掉"。
- 截图目录 `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**。
- Python：**必须显式指定** `C:\Users\v_lweiili\AppData\Local\Programs\Python\Python312\python.exe`
  （3.12，装了 numpy + pillow）。
  ⚠ **裸 `python` 会解析到 `~/.workbuddy/binaries/python/3.13.12`，它没有 PIL/numpy**
  → 回归会 39 例全 FAIL，且错误是 `ModuleNotFoundError: No module named 'PIL'`
  （看着像代码炸了，其实是环境）。跑回归固定写成：
  `"C:/Users/v_lweiili/AppData/Local/Programs/Python/Python312/python.exe" tools/run_regression.py`
- 离线烘焙：`make_bc_dds.py`（BC 编码器，可复用）、`make_sss_lut.py`（R4-2 LUT）。日志标签表见归档。

## 高频坑点（全表 66 条见归档 `#已知坑点全表`）
1. **同一文件的多处修改必须串行 Edit**（并行第二个会静默回滚第一个）；不同文件可并行。
2. `Window.h` 里 **vulkan.h 必须在 glfw3.h 之前**；`std::filesystem::path` 不能直接赋 `std::string`。
3. **改某条管线的 push constant range = 可能打断整个 Pass 的 set0 绑定**（VUID-08600）。
4. 深度图当纹理采样用 `DEPTH_STENCIL_READ_ONLY_OPTIMAL`；**全局描述符引用的图重建后必须
   `writeGlobalDescriptors`**；blit 生成 mip 的初始 barrier 必须全层 UNDEFINED→TRANSFER_DST。
5. **视口 HUD 必须是 viewport 的 child window**（ImGui 每帧把聚焦窗口提到最前）；popup 必须每帧提交。
6. **自动化**：跑前关 `imgui.ini` 与窗口几何存档；`SetForegroundWindow` 会静默失败；
   **"注入的第一次左键会被 Windows 拿去做窗口激活"**；进程 stdout 是块缓冲。
7. **改 GLSL/贴图后先 `cmp` 确认 `build/bin/` 里的副本真的变了**。**没排除构建陈旧之前，别去怀疑算法。**
8. **REPEAT 采样器的 0/1 边界**是陷阱（见 `材质`）。
9. **抓图必须走 `capture_stable`（`capture_nonempty` 已转发到它）**，它挡两种 flaky：
   ① **client 0×0**（窗口还没铺开）—— 直接 `save` 会抛 `ValueError: cannot write
   empty image` **把整个用例崩掉**（不是判 FAIL，是崩）；
   ② **尺寸正常但整幅纯白**（`PrintWindow(PW_RENDERFULLCONTENT)` 在 Vulkan 交换链上
   偶发返回未合成帧；实测 1280×800 只压出 **4460 字节**）。第二种最阴险：
   空图 vs 正常图做差 = "差异铺满整窗 / 采样区全 255"，**看起来像渲染回归**。
   判空规则：整窗缩到 32×20 后 min/max 灰度差 < 3（编辑器整窗恒有面板文字，
   整窗单色必然不是真画面）。⚠ 凡是自己写 `capture_client` 的用例都补上这层
   （`verify_csm` / `verify_ibl` 就因为这个红过一整轮）。
10. **注入的点击会被吞 → "展开类"断言要做成幂等 + 重试**：折叠段先点标题栏再读日志，
    偶尔第一次点击无效（Windows 拿去激活窗口 / 布局未稳）→ 读到"没有 XXX 日志"的**假失败**。
    先查日志判断当前是否已展开，未展开才点，点完校验、失败再点（≤3 次）。
11. **顶点属性的 numeric type 必须与 `format` 一致**：`Vertex` 的 joints 是 `R8G8B8A8_UINT`
    → GLSL 必须 `uvec4`（**写 `ivec4` 触发 VUID-Input-08733**，管线创建失败、画面全无效果，
    但 `CULL-STATS` 照样报"画了 1 个" → 极具误导）。
12. **`re.Match.group(-1)` 抛 `IndexError`**（Python ≥3.6 负数下标不再表示"最后一组"）。
13. **深度图读出来的深度约定必须实测确认，别照抄**（见"延迟贴花"块 —— 本引擎存的是 NDC z）。
14. **调试量编码成颜色再读数不可靠**：输出经 exposure × ~2.3 + tone-map(`x/(x+1)`) 会**饱和**，
    读数恒贴上界。**改用饱和原色做分类编码**（黑/蓝/黄/品红/绿）+ 直接看截图。
    想核实数值约定则用**窄带身份测试**（拿已知几何算出唯一可能的区间，再看落在哪个约定里）。
15. **凸体要画正面**（cull back）：凸体正面投影 == 整个盒体投影，盒内像素必被覆盖；
    代价是相机进入盒体内部时片元全为背面 → 一个都不产生（已知边界情形）。
16. **递归 lambda 不能按名字自引用** → 把"自己"作为第一个参数传（`[&](auto&& self, ...)`），
    比 `std::function` 少一个 include、少一层类型擦除。
17. **`unique_ptr` 成员与"统一由容器持有"不能并存**：目标若必须挂在 `m_targets` 里（为了让下标
    == 槽位），槽位结构体里就只能是**非拥有裸指针**；写 `unique_ptr` 编译器可能侥幸放过，
    运行期一定双重释放。
18. **C++ 成员/内联实现里写错成员名只在实例化时报**（`subviewCount()` 里的
    `m_subviewTargets` vs `m_subviews`）—— 报错行指向 `.h`，但根因是名字对不上，先怀疑拼写。
19. **优雅关闭类自动化测试：窗口在 `onInit` 之前就建好了**。"找到窗口"≠"跑了几帧" ——
    立刻发 WM_CLOSE 会拿到 `Total frames: 1` 的无意义会话；必须再等几秒。

## 路线图
✅ R0-1 / R0-2 / R1-1~R1-4 / R2-1~R2-3 / R3-1~R3-3 / R4-1 `AlphaMode::Mask` / R4-2 预积分 SSS /
R4-3 POM 视差遮挡贴图 / R4-4 DBuffer 延迟贴花 / R5-1 反射探针（2026-10-15）/
**R6-1 GPU 粒子系统（2026-10-15，⭐ 最大缺失，`tools/verify_particles.py` 34/34 PASS）**。
⚠ R5-1 **遗留**：SSR↔探针权重混合 / 相邻探针边界混合（`blendDistance` 已进结构未实现）/
  探针放置 gizmo · 预览球 · 独立烘焙流程（归 E29 + E14）。R5-2 lightmap / R5-3 DDGI 仍未做。
⚠ R6-1 **遗留**：ribbon / mesh 渲染模式 / **粒子碰撞** / 视口内粒子预览窗口 / 帧图事件 /
  曲线仍是 4 点滑条组（roadmap 允许）；**`duration=0` 时 Burst 完全不发射**（待确认的产品语义，
  Unity/Blender 此时应发一次）。**基准**：用户两次强调"以 unity 或 blender 为基准"→
  面板 = Unity Shuriken 的 12 个模块，颜色 = Blender Color Ramp 形态的带停靠点色带。
✅ **E01 特性注册表**（编辑器阶段 A 的第 1 项，2026-10-14）—— "特性交付五件套"里的第②件从
"每参数手写"变成"在 `kFeatures[]` 加一行"，"只有 env、没有 UI"这条结构性欠账的口子堵住了。
✅ **阶段 B 布局范式 E04~E08 主体完成**（2026-10-14）—— 面板表驱动四路派生 / 六套工作区 /
分栏增强（可视化·双击折叠·最大化·比例预设·最小宽度）/ 多视口（Quad 等四路 + 独立相机）/
菜单栏六项补全 + 可折叠工具组 + Transport 扩展。E07 留 4 项、E08 留 2 项按计划后延
（都要等 **E14 视口面板重构** 或 **E03 落盘**），逐条记账见 `docs/editor_roadmap.md` §3。
✅ **阶段 C 核心面板 E09~E17 全部完成**（2026-10-15）—— Engine 面板注册表驱动 /
无 UI 特性清零 / Inspector（折叠·搜索·Copy-Paste-Reset）/ Hierarchy（过滤·图标·拖拽·状态列）/
ContentBrowser（过滤·视图·四档缩略图·拖放全链路）/ Viewport（四角浮层·9 书签·偏好落盘）/
Stats / Console（新建）/ Preferences。**里程碑 M3 达成：每个面板都有自己的 `verify_*.py` 且 PASS。**
⚠ **E02 已做**（`editor/FeatureUIGen.*` 存在）；**E03 设置层级仍未做** ——
`engine/render/RenderSettingsIO.*` **不存在**，5 层覆盖链没落（E17 只落了"扁平偏好表 + JSON"一层）。
**下一项 = E03**（或在阶段 D/E 里挑，见清单 §1）。
其后：~~R6-1 GPU 粒子（⭐ 最大缺失）~~ ✅ 2026-10-15 / R6-2 水体 / R6-3 植被风 / R6-4 地形 /
R6-5 动态天空 / R7 架构演进。
并行编辑器轨：**A 地基 🟡E01✅ E02✅ E03缺 → B 布局 ✅E04~E08 主体（2026-10-14）
→ C 核心面板 ✅E09~E17（2026-10-15）→ D 交互 🟡E18✅ E19✅ E21✅ / E20 缺E03 / E22 L / E23 待做
→ E 生产力 🟡E25✅ / E24·E28 可开工 / E26 缺E03 / E27 缺R7-1 / E29 🟡（R5-1 已就绪，只缺 R5-2）（2026-10-15）**
（可执行清单 `docs/editor_roadmap.md` 的 E01~E29；架构理由见 `docs/editor_architecture.md`）。
⚠ E07 后延的四项（子视口性能降级 / 独立视图模式与 gizmo / 点击激活 / 视口矩形数组）
**本轮仍未做** —— 已落地的是"能同时出多个视角"那一半（Single/2V/2H/Quad + 随工作区落盘）。
当前 `check_feature_surface.py`：**特性 33/33 有 UI 面（欠账 0）**、
裸 env 漏网 **0**、注册表接管 env 99/127（78%）。
`check_regression_map.py`：41 例已登记 / 共 47 例（6 例按设计无条件重跑）。
**阶段 C 回归结论 = 有效 42/42**：全量跑出 39 PASS + 3 红
（`verify_bvh`/`verify_csm`/`verify_ibl`，全部定性为 harness flaky ——
CSM/IBL 是 `PrintWindow` 白帧，BVH 是导航球吃了右下角点击），修复验证侧后
3 例单跑全 PASS。⚠ **不要再为"凑干净的全量"自动补跑** —— 见顶部「回归执行纪律」。
