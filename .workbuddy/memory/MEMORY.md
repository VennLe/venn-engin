# venn —— 项目长期笔记（精编版）

> 本文是「改动前必读」的浓缩约定集，**只保留仍然有效的规则**。详细叙事见各日
> `YYYY-MM-DD.md` 日志；本文每条都指向一个明确的代码位置或一个明确的判断。
>
> **项目**：从零手写的 Vulkan 引擎（学习向，C++17），仓库 `E:\code\cpp\venn-engin`
> （CMake 工程名 `Venn`，刻意不一致）。世界坐标系 = **右手系 Z-up**（2026-10-10 起）。
> 只有一个可执行文件 `Editor.exe`；Sandbox/demo 已全部删除，启动 `resetToEmptyScene`。
> **改目录名后必须删 `build/` 重新 configure**（CMakeCache 全是绝对路径）。

## 项目分层（新增代码必须放对层）
`engine/core`(Application/Window/Input/Logger/Time/WindowGeometry) → `engine/rhi`(Vulkan 封装) →
`engine/render`(Renderer/HdrTarget/ShadowPass/PostProcess/FrameResources/Environment) →
`engine/scene`(Scene/Camera/Light/Bvh + SceneSerializer) → `engine/assets`(Mesh/Texture/Material/加载器/Lod)；
另有 `engine/ui`(ImGuiManager)、`engine/ecs`(World/Entity/Components)、`engine/physics`(碰撞)。
应用层只有 `editor`(EditorApp + 面板 + 自研脚本 `*.vks` + 编辑器态场景)。
依赖 `editor → render/ui → scene/assets → rhi → core`；**engine/ 不认识 editor/**，
唯一接缝 `Application::activeScene()` / `Renderer::setViewportSize` / `viewportTextureId()`；
一次性收尾走 `Application::onShutdown()`。CMake 目标：静态库 `venn` + 可执行 `Editor`。

## 构建与运行（Windows + MinGW）
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=C:/Program\ Files/CodeBlocks/MinGW/bin/gcc.exe \
  -DCMAKE_CXX_COMPILER=C:/Program\ Files/CodeBlocks/MinGW/bin/g++.exe \
  "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"   # 必须（CMake4 拒绝 GLFW 的 3.4）
cmake --build build -j 8
```
- **工具链路径含空格** → `-DCMAKE_CXX_COMPILER=` 这类参数**必须整体加引号**，否则按空格拆开。
- 默认走 `./make build`（`Makefile` + `make.cmd`，驱动 `mingw32-make`）。
  常用目标 `build / run / clean / rebuild / distclean / package / smoke / info`。
  **`make run` = 打开编辑器**（唯一入口，无 sandbox/play 目标）。
- Makefile 配方须**同时兼容 cmd.exe 与 sh** → 只用 `cmake -E`、正斜杠、`cmake -E env`；
  `echo` 与 `.cmd/.bat` **整文件**必须纯 ASCII（cmd 用 GBK，乱码会被当命令 → 死循环）。
- `build/bin/imgui.ini` **会在下次启动覆盖默认面板布局**（自动化跑必须 `io.IniFilename=nullptr`）。
- 第三方库版本锁：**tinygltf 固定 v2.9.4**（master 是 v3 API，不兼容）；imgui 用 master；
  glfw 3.4；glm 1.0.1；stb / tinyobjloader / VulkanMemoryAllocator 用 master。

## 环境变量（自动化钩子，只在设了时生效）
- 基础：`MYVK_FRAMES=N`(跑 N 帧退出+抑制窗口存档) / `MYVK_VALIDATION=0` /
  `MYVK_SHADER_DIR` / `MYVK_ASSET_DIR` / `MYVK_HIDE_UI=1` / `MYVK_NO_WINDOW_SAVE=1` /
  `MYVK_CONTENT_DIR` / `MYVK_FREEZE_ANIM=1` / `MYVK_SCENE_TEST=1`。
- **视口像素断言必备**：`MYVK_EDITOR_NO_HUD=1`(关掉视口全部 2D 叠加层)。
  里面的 FPS/帧时间每次运行都不同，不关掉"同配置两次逐像素相同"永远失败。
  另 `MYVK_GRID=0`(关地平面栅格，栅格是屏幕空间固定图案，给对比添噪声)。
- 编辑器自动化：`MYVK_EDITOR_SCENE=<路径>` / `MYVK_EDITOR_SELECT=<名字>` /
  `MYVK_EDITOR_GIZMO=move|rotate|scale` / `MYVK_EDITOR_PREFS=1` /
  `MYVK_EDITOR_CAM="x,y,z,yawDeg,pitchDeg"`（**必须排在 MYVK_EDITOR_SCENE 之后**，见坑 #53）/
  `MYVK_EDITOR_SPACE=world|local` / `MYVK_EDITOR_COLLIDER=hull|capsule` /
  `MYVK_CHARACTER_MODEL=<路径>`。
- 渲染开关：`MYVK_MSAA / LIGHTS / CLUSTER_CULL=0 / DEPTH_PREPASS=0 / LIGHT_GIZMOS=0 /
  TILE_SIZE / CLUSTER_SLICES / MAX_LIGHTS_PER_CLUSTER / CLUSTER_STATS=1` / `MYVK_FRUSTUM_CULL=0` /
  `MYVK_LOD / MYVK_LOD_BIAS / MYVK_LOD_FORCE / MYVK_LOD_DBG=1`。
- 阴影（详见渲染管线节）：`MYVK_SHADOW_DBG=1`(打 `SHADOW-DBG`/`SHADOW-STATS`) /
  `MYVK_SHADOW=0` / `MYVK_SHADOW_CASCADES=1..4` / `MYVK_SHADOW_DIST=米` / `MYVK_SHADOW_LAMBDA=0..1` /
  `MYVK_SHADOW_RES=像素` / `MYVK_SHADOW_SNAP=0` / `MYVK_SHADOW_BLEND=0..0.5` / `MYVK_SHADOW_HARDEN=0..1` /
  `MYVK_LOCAL_SHADOW=0`(与 `MYVK_LOCAL_SHADOW_LIGHTS=0` 逐像素等价) / `MYVK_LOCAL_SHADOW_LIGHTS=n` /
  `MYVK_LOCAL_SHADOW_RES=像素` / `MYVK_LOCAL_SHADOW_BIAS=a,b,c`(米/米/纹素，默认 `0.030,0.150,1.2`)。
- 环境/IBL：`MYVK_SKY / IBL / SKY_ZENITH / SKY_GROUND / SKY_RES / ENV_DBG=1`。
- Froxel 体积雾（R3-2）：`MYVK_VFOG=1` / `MYVK_VFOG_DENSITY` / `MYVK_VFOG_ALBEDO` /
  `MYVK_VFOG_FALLOFF` / `MYVK_VFOG_BLOB` / `MYVK_VFOG_PHASE_G` / `MYVK_VFOG_DBG=1`(打 `VFOG-DBG`)。
  ⚠ **`MYVK_FOG*` 是另一条路径**（解析式**高度雾**，只在 pbr.frag 里按世界坐标求值）——
  两条独立，别混。froxel 雾默认关，关时 set0/binding16 绑 1×1×1 透明图 = 基线逐位不变。
- `MYVK_LOG_RECTS=1`：打印关键控件矩形（自动化定位）；各子系统落盘日志见"工具"节。

## 渲染管线约定（改动前必读）
Pass 顺序：`ShadowPass(4 级级联，每级独立 depth-only RP) → LocalShadowPass(点光/聚光，12 层 2D
array，每层独立 RP；未用的层也只清) → DepthPrePass(独立 RP) → SSAO(compute) → ClusterBuild(compute) →
VolumetricFog(compute×2，R3-2，关时不录) →
Forward(天空盒 → 不透明 → 地平面栅格 → 透明，MSAA resolve，深度 LOAD) → PostProcess(Bloom+Tonemap)
→ 交换链`。**compute 不能在 RP 内执行** → 深度预通道必须独立成 RP，且**无条件执行**（只把"画几何"
包进开关），否则前向 RP 的 `LOAD` 深度未定义。
- **Forward 内部子顺序刻意如此**：栅格夹在不透明与透明之间 —— 吃深度测试（墙能挡它）+
  不写深度（玻璃能叠） + 排在透明前。它**绝不能**做成 ImGui 叠加层（ImGui 永远压 3D 之上）。

### 阴影 · 方向光 CSM（`ShadowPass`）
1. **投影矩阵必须是 Zero-to-One**：`glm::orthoRH_ZO`（不是 `glm::ortho`）。Vulkan clip z∈[0,1]，
   GL 的 [-1,1] 会让深度图与 `pbr.frag` 参考深度不在同一空间 → 覆盖区一片黑。
   `pbr.frag::shadowFactor` **只对 xy** 做 `*0.5+0.5`，z 直接用。
2. **每级正交范围 = "该级视锥切片的包围球"**（`invViewProj` 反投影 8 角点求球），**不是物体 AABB**。
   用球（半径与光方向无关，盒会让光一转阴影边缘"呼吸"）。球心在**光轴坐标系里量化到纹素网格**
   （`MYVK_SHADOW_SNAP=0` 可关，默认开）。⚠ **量化必须作用在球心的世界位置**：用只含旋转的
   光轴基 `(lx,ly,lz)` 把 `mid` 投影后取整，再 `center = mid + lx*(sx-ax) + ly*(sy-ay)` 重建
   lookAt。**不能对 `view*mid` 量化**（`mid` 是 lookAt target，`view*mid≡(0,0,-dist)`，量化 = 空操作
   —— 这个 bug 曾潜伏到 R3-1 才被 `SHADOW-MATRIX` 日志揪出）。
   正交**近平面取 0**，靠 `kDepthPadding = 25m` 外扩。退化时 `radius` 下限 0.05。
3. **只接收不投射的物体必须 `VisibilityComponent::castShadow=false`**（自带地面就是），
   否则大地板会把自己投进每一级 + 自投影 acne。
4. **级联分割 PSSM practical scheme**：`λ·near·(D/near)^(i/N) + (1-λ)·(near+(D-near)·i/N)`，
   `D = min(相机远平面, 距离上限)`、λ=0.6。**near/far 必须从 `camera().nearPlane()/farPlane()` 读**，
   不能写死（级联分割、切片角点 NDC 换算、分簇对数切片三处假设同一组值）。
   D=90/near=0.1 → `9.36 / 19.82 / 36.87 / 90.00`（`tools/verify_csm.py` 逐项对公式）。
   级联盒**边缘不淡出**（只在"视深度 > 最大距离"处 `smoothstep`），否则交界处假暗带。
5. **R3-1 阴影硬化三件套**（`MYVK_SHADOW_BLEND` / `MYVK_SHADOW_HARDEN` / `MYVK_SHADOW_SNAP`）：
   `FrameUBO.shadowFilter = (blendBand, harden, 0, 0)`。
   - **级联间混合带**：接收点落在"本级远端最后 band 比例"内时，同时采样下一级并按深度插值
     （`pbr.frag::shadowFactor` / `sampleCascade`）。0 = 关 = 基线。
   - **接触硬化**：按接收点在级联内的相对视深度缩放 PCF 核半径（近端收、远端放）。0 = 关 = 基线。
   - **texel snapping**：见上第 2 条，默认开（= 原行为）。
   每件都遵循"默认关 = 逐像素等于基线"，每件都要 A/B 对照。
   ⚠ **snap 的效果无法用截图验证**（亚像素级）→ 断言 `MYVK_SHADOW_MATRIX_DBG=1` 打的
   `SHADOW-MATRIX c0 t=(..)`（第 0 级矩阵平移列 ×1e6）：量化开 = 分段常数 + 一次整格跳；
   量化关 = 每步都漂移。见 `tools/verify_shadow_hardening.py`（4 组断言）。

### 阴影 · 局部光源（点光/聚光，`LocalShadowPass`）
1. **近平面按 range 取 `clamp(range/20, 0.1, 2.0)`**，不能拍小常数。透视 NDC 深度是**双曲**的：
   range=32 用 near=0.05 时，"离灯 15m 遮挡体"与"20m 地面" NDC 只差 7.5e-4 → 阴影整个消失；
   改 range/20 后涨到 2.8e-2。
2. **深度偏移以「米」给，片元按参考深度换算回 NDC**（`LightShadowRecord.params=(near,far,灯下标,有效位)`；
   `pbr.frag::ndcBiasFromWorld`：`dz/dd = (f − z·(f−n))² / (n·f·(f−n))`）。
   照抄 CSM 的 NDC 常数 `0.0016` 在 range=32 下等于 10 米世界深度 → 阴影一片不剩。
   **别给双曲深度配 NDC 常数偏移**。
3. **未使用的层每帧也要走一次空 depth-only pass（只清深度）**：整张 array 被采样器一起绑定，
   留一层停 UNDEFINED → 验证层报 IMAGE_LAYOUT。
4. **层分配：按到相机的距离平方由近到远贪心，预算不够就整体停在那一盏之前**（不跳过它去找下一盏，
   那样"哪盏灯有影子"会随相机乱跳）。点光 6 层/聚光 1 层，`kMaxLocalShadowLayers=12`；
   结果写回 `LightInstance::cosOuterType.z`（基准层号），点光实际层号 = 基准 + `vennCubeFaceIndex(dir)`。
   光栅化 `depthBias` 对浮点深度几乎为 0，真正偏移在片元侧。

### 阴影 · 共用
- `ShadowPass`/`LocalShadowPass` 共用 `shadow.vert`，靠 `ShadowPushConstants.which.y` 选矩阵来源
  （0→`FrameUBO.lightSpace[x]`，1→set0/binding11 `lightShadow.records[x].matrix`）。**矩阵只有一份**。
- `ShadowCull.h::visibleInLightClip` 是两套阴影共用的局部 AABB 剔除（正交/透通用，`w≤0` 放弃剔除）。
- `ShadowPass` 推常量是 `ShadowPushConstants`（96B，非主前向 128B）—— 因为它在**自己的 RP 内自行绑
  set 0**。**绝不要把该 Pass 绘制搬进前向 RP**，否则 VUID-08600。
- **array 深度图渲染**：framebuffer/`imageStore` 不能挂 array 视图做单层渲染（那是 layered
  rendering）。每层单独建 `VIEW_TYPE_2D` 视图 + framebuffer；采样侧才用 `sampler2DArrayShadow`。

### 描述符 / FrameUBO / 推常量
- **set 0（17 个 binding）**：0 FrameUBO / 1 shadowMap(`sampler2DArrayShadow`，第 i 层=第 i 级) /
  2 lights / 3 clusters / 4 lightIndices / 5 clusterStats / 6 jointMatrices / 7 irradianceMap /
  8 prefilterMap / 9 brdfLut / 10 skyMap / 11 lightShadows(`LightShadowRecord[]` SSBO，VERTEX|FRAGMENT) /
  12 localShadowMap(`sampler2DArrayShadow`) / 13 instanceModels(vertex, instancing) /
  14 ssaoTex(R8，关时绑白图) / 15 mdiInstances(vertex, MDI) /
  16 vfogTex(`sampler3D`，R3-2，关时绑 1×1×1 透明图)。`kGlobalSetBindings = 17`。
  （灯/环境/雾资源全挤 set 0：Vulkan 只保证 4 个 set 同时绑，set 1..6 已被六张材质贴图占满。）
- **`FrameUBO` 704 字节**（std140，有 `static_assert`）。历史：IBL `envParams`(384) → CSM
  `lightSpace[4]`+`shadowParams`(576)/`cascadeSplits`(592)/`shadowBias`(608) → `localShadowParams`(624)
  → 高度雾 `fogParams`(640) → `viewModeParams`(656) → R3-1 `shadowFilter`(672) → R3-2 `vfogParams`(688)
  → 704。
  ⚠ 改它必须同步**所有**声明它的着色器：`pbr.vert`、`pbr.frag`、`sky.vert`、`cluster_build.comp`、
  `shadow.vert`、`fog_inject.comp`（后几个只声明前缀，无害但必须一致）。新字段**只能加在末尾**。
- 推常量 **112B**：`mat4 model + vec4 baseColor + vec4 pbr + vec4 emissive`（`render::PushConstants`）；
  栅格另有 48B `GridPushConstants`；顶点 `pos3+normal3+uv2` stride 32。
- ⚠️ **旁路管线的 push constant range 必须与主管线逐字段相同**：set 0 是整个 Pass 一次性绑定的，
  Vulkan 判"管线布局兼容"时**把 push constant range 的范围与阶段也算进去**。栅格管线因此**故意**
  声明与主前向一样的 112B（实际只推 48B）。否则立刻 VUID-08600，**后续所有 draw 全报错**。
  也别想"换个 layout 重绑 set 0"——那会打断既有绑定，更糟。
- 交换链/纹理均 SRGB，**着色器内不做手动 gamma**；视口/剪刀动态，resize 不必重建管线。
- 绕序：`addFace(n,u,v,half)` 中 **u×v 必须 = n**，正面 `COUNTER_CLOCKWISE`；
  `offsetAlongNormal`：立方体面 true，单面（地面）必须 false。

## 分簇光照（Clustered Forward）
- **簇编址三处必须一致**（C++/`cluster_build.comp`/`pbr.frag`）：`idx=(slice*gridY+tileY)*gridX+tileX`，
  切片 `d(k)=clusterNear·(zf/clusterNear)^(k/slices)`；切片起点用 `clusterNear` 而非 `zNear`。
- **固定槽位** `lightIndices[ci*cap+i]`（非原子追加）→ 结果**可复现**，截图对比才有意义。
- `maxLightsPerCluster` **唯一数据源是 C++**（经 `ubo.lightParams.w` 传片元），**禁在 pbr.frag 硬编码**。
  只花显存不花时间 → 默认 128。
- 射灯只按 range 球剔除；衰减用窗口化平方反比 `win²/d²`，range 处严格归零。
- 透明（Blend）不写深度、按相机距离**降序**排序、**不参与深度预通道**。
- 遥测 `stats` SSBO：0 单簇最大/1 溢出簇数/2 配对数/3 空簇数（`beginFrame` 回读并清零）。

## 渲染性能三件套
- **视锥剔除**（`render/Frustum.h/.cpp`，Gribb–Hartmann；本工程 NDC z∈[-1,1]，近平面 `r3+r2`）：
  `uploadSceneState` 按**本场景**相机重建 `m_sceneFrustum`。开关 `frustumCullingEnabled()`/
  `MYVK_FRUSTUM_CULL=0`（关掉=全量提交，是"开关两次逐像素一致"的对照组）。
- **BVH**（`scene/Bvh.h/.cpp`）：`spatialIndex()` 每帧一次，实体集合（含**顺序**）变了→`build`，
  否则 `refit`。渲染剔除与编辑器射线拾取**共用同一棵**。
  ⚠ `query` 叶子必须**逐元素复测**（叶子盒是成员并集，并集相交 ≠ 每个成员相交）；
  `refit` 倒序遍历依赖"父下标恒小于子下标"；`buildRange` **只能重排 order 下标表，不能动 items**。
  自检 `MYVK_BVH_DBG=1` 打 `BVH-DBG brute= bvh= missing= extra=`，`missing>0` = 会漏画的硬错误。
- **LOD**（`assets/Lod.h/.cpp` + `AssetManager::ensureLod`）：`switchRatio[i]` = 进入第 i 级的上界
  （默认 {1.0,0.25,0.10,0.04}，迟滞 1.15）；比值 = `AABB半对角线/(距离×tan(fovY/2))`。
  `selectLevel(ratio,bias,current)` 里 **bias 是除**（>1 → 更快落低模）；`current<0` 时**从高模往下退**。
  程序化图元按细分减半重生成；导入网格走顶点聚类（格分辨率二分搜索 [2,256]）。
  **蒙皮网格不生成**（jointBase 按 `Mesh*` 记）。`MYVK_LOD_DBG=1` → `LOD-BUILD/STATS/SWITCH`。
- **Hi-Z / 遮挡剔除**：候选 AABB 表必须收**所有**视锥内物体；两帧延迟回读有时序不变量。
- **MDI（间接绘制）**：`vkCmdDrawIndexedIndirect` 不带 VB/IB → **必须自己绑**；
  提交组键必须含"网格"；"Pass 2.5" 要放 `recordFrame` 不是 `recordSceneInto`；
  `pbr.mdi.vert` 读的 binding 15 必须在 set 0 layout 里声明。

## 天空盒 + 真 IBL（split-sum）
模块 `render/Environment.h/.cpp`（4 张图 + 采样器 + 描述符池 + 4 条 compute）+ `common.glsl` /
`sky_cube.comp` / `ibl_irradiance.comp` / `ibl_prefilter.comp` / `ibl_brdf.comp` / `sky.vert` / `sky.frag`。
- **天空模型唯一来源 `vennSkyColor(dir, zenith, ground)`**（`common.glsl`）。梯度**必须用 `dir.z`**
  （Z-up）—— 用 `dir.y` 会出现一条笔直的明暗带。
- **烘焙用 compute 而非 6 次 RP**：`vennCubeDir(face,uv)` 是纯函数，dispatch z 维=6 就写满。
- ⚠ **`imageStore` 不能指定 mip** → 预过滤每 mip 各建一个 `VIEW_TYPE_2D_ARRAY`（`baseMipLevel=m`）。
- 天空立方体 `RGBA32F`、BRDF LUT `RG32F`（为 CPU 回读免半精度转换）。irradiance/prefilter `RGBA16F`。
- **天空 pass 排在前向 Pass 最前**（depthTest/Write 全关）→ 排最后会盖掉地平面栅格。
- split-sum（Karis）：漫反射 = `albedo × irradiance(N)`（卷积乘 π，与 1/π 抵消）；
  镜面 = `prefilter(R, rough×maxLod) × (F0×A + B)`。
- **BRDF LUT 有解析解**：`(N·V=1, roughness=0)` 处**精确 A=1, B=0** —— 唯一能硬断言的检查点。
- 保留旧程序化近似路径，用 `FrameUBO.envParams.x` 切换（"两条路径给出不同画面"的对照实验手段）。

## Shader 变体 / 关键字系统
着色器是**离线 glslc 预编译的固定管线**。关键字 = 位集合（掩码 0..7），
`render/ShaderKeywords.h` 唯一事实来源；位序在三处逐字对齐：`ShaderKeywords.cpp::kKeywords[]` /
`CMakeLists.txt` 的 `SHADER_KEYWORDS` + `VARIANT_DEFINES_0..7` / `pbr.frag` 的 `#if`。
- bit0 `VENN_DOUBLE_SIDED`(改代码+改 cullMode) / bit1 `VENN_ALPHA_BLEND`(改代码+改混合/不写深度) /
  bit2 `VENN_NORMAL_MAP`(只改代码,**不改状态**)。
- 变体文件名 `pbr.k<n>.frag.spv`（k0..k7）。掩码同时推导"编哪份 .spv"与"管线固定状态"
  （`VariantPipelines::get` + `applyKeywordState`）。队列分类也跟着掩码。
- **两条硬约定**：① 绑定缓存的"什么都没绑"哨兵**不能用 0**（0 是合法掩码）→
  `kBindsNothing = 0xFFFFFFFFu`，栅格管线之后必须重置。② `SHADER-VARIANTS` 自检日志
  **必须打在前向 Pass 之后**（`variantSwitches` 在 Pass 循环里累加）。
- 变体**共用同一 VkPipelineLayout**（正确性要求，防 VUID-08600）；`rhi::PipelineDesc.sharedLayout`
  （非空则用、不拥有不销毁）。
- `fillForwardPipelineDesc` 里 `desc.fragSpv = readFile("pbr.frag.spv")` 必须删（单体已不生成）。
- **glslang：采样器数组非常量下标要显式声明扩展，且 `#extension` 必须在所有非预处理语句之前**。
- **默认定义为 0 的编译开关不能用 `#ifdef` 判断**。

## 视口像素断言的纪律（血的教训）
- ⚠ `ViewportPanel::drawOverlay` 会画 FPS/帧时间（每次运行都不同）→ 断言前**必须
  `MYVK_EDITOR_NO_HUD=1`**。
- `region_diff` 按 stride 2 抽样：全区域均值 1.11 的区可能算出 4.48。归因前先打印
  "全区域均值 + 差异像素清单"，别只看一个数。
- 色调映射会压平高光：线性 2.1× 的比值到画面只剩 1.24×。**别用固定倍数阈值**，
  用"逐级落差 > ε"或"单调性"判据。
- **把日志行当读数表时，先打印一次 `m.groups()` 再写断言** —— 多分量日志手数分组号必错一次。
- **"全有/全无"的开关测不出"分级"**：要证明"距离分级"得取一个**落在淡出区间内**的中间阈值，
  再配两端，三点连起来才是单调信号。
- **"整体亮度偏移"这类伪信号要用"变化定位"排除**：断言差异**只**落在预期区域、别的区域 ≈ 0。
- **验证"输入驱动"的行为别只靠像素差分**（编辑器场景是大片平滑渐变，轻微视角变化就差分很大）。
- **验证脚本结尾必须打 `结果: PASS`/`结果: FAIL: ...` 汇总行**（run_regression.py 依赖它）。
- **回归可增量跑**：`run_regression.py` 会跳过"上次 PASS 且产物未变"的。

## editor/ 编辑器层约定
三条铁律：**① 编辑器模式不跑游戏逻辑 ② 渲染目标是 ImGui 子窗口而非全屏 ③ 编辑态与运行态分离**。
- **运行态 = 同进程的内存副本**（`m_runtimeScene`）：`EditorContext::activeScene()` 是唯一分派点
  （`usingRuntime() ? m_runtimeScene : m_editorScene`）。Play = 编辑态 `sceneToJson` →
  `sceneFromJson(m_runtimeScene)`（**内存拷贝，不落盘不起进程**）→ 视口自己变成游戏画面。
  **不做跨进程**（`GameProcess`/`PlayerApp` 已整体删除）。运行态相机 `play()` 里对副本调
  `setUseGlobalInput(false)`+`setFlyMode(true)`，此后编辑器不再驱动它。
  `pause()` 只翻状态；`stop()` 丢弃 runtime scene、复位 `m_gameFullscreen`、`validateSelection()`。
- **Play 期间编辑器只读**（gizmo/Inspector/层级增删被 `!isEditing()` 挡住）。
- **命令模式**：值编辑 `RawBytesEditCommand`，结构编辑 `SceneSnapshotCommand`（整场景 JSON）；
  统一入口 `structuralEdit(name,fn)`。破坏性操作**两段式**（ECS 遍历期间不能增删 → 攒 pending
  队列，树画完 `flushPending()`）。
- **视口解耦**：`setViewportSize` 让 3D 输出写离屏图，`viewportTextureId()` 给 `ImGui::Image`。
  纹理稳定性：① recreate 保留场景输出图 ② `createViewportTextures` 比较未变则不重注册
  ③ ViewportPanel 尺寸刚变那帧不画图（`m_resizePending`）。
- **布局**：SplitLayout 二叉分栏树；主体面板必须 **Always 落位 + NoMove/NoResize**；
  标题带状态须加稳定 ID（`###viewport`）。
- **自研脚本 .vks**（`editor/script/ScriptEngine.{h,cpp}`）：`ScriptHost{scene,entity}` 是唯一接缝；
  热重载 mtime 轮询 0.5s，**编译失败保留上一版**；数值用 double，`rotate` 单位是**度**，
  步数上限防死循环。只在 Play 态跑且只作用于 runtime scene。
- **全局观感走 `ImGuiManager::applyVennStyle()`**：accent `(0.16,0.53,0.94)`、
  `WindowBg(0.106,0.110,0.118)`、`MenuBarBg == TitleBg(0.113,0.117,0.125)`（工具条与菜单栏
  **连成一条顶栏**，UE 观感关键）。
- **面板宽度自适应**：`kLeftFrac 0.175/min300/max520`、`kRightFrac 0.205/min330/max560`；
  用户拖过分隔条后 `m_userAdjusted` 不再自动重算。
- **快捷键**：`F2` 重命名 / `Ctrl+D` 复制 / `Ctrl+N` 新建场景 / `Delete` 删除 / `Backspace` 上一级。
- **Content 右键菜单是"显式单 popup"**：只写 `m_ctxRequest/m_ctxEntry`，循环外 `OpenPopup` +
  `drawEntryContextMenu`；`BeginPopup` 返回 false 就自动清状态。

### 关键子系统
- **Shift+A 添加菜单 / 播放条 / Preferences**：菜单视口 hovered 才触发，popup 每帧无条件提交；
  落点 = 鼠标射线∩z=0。**Sunlight = 场景级太阳**（唯一、有阴影、不进实体序列化）；
  **Directional Light = 实体级补光**（`Type::Directional=2`，`positionRange.w=1e5` 影响球 trick）。
  播放条 `LayoutRects.transport`，视口底部扣 `kTransportH=34px`。Preferences 窗口 ID 是 `###prefs`
  （**别沿用旧名**，imgui.ini 残留会让它飞角落）。
- **编辑器锁定**：`ecs::LockedComponent`（纯标记，看得见动不了）。`PickingSystem::pick` 跳过它、
  `GizmoController::update` 直接 return。序列化字段 `je["locked"]`（**读写都要补**）。
  自带 `Ground` 默认带它（它是参照系）。数字键 0 = 落地（只改 z、世界包围盒底部贴 z=0，
  只认"在 3D 视口里左键点中"的选中）。
- **碰撞体系统**（`engine/physics/` 四文件 + `editor/ColliderView`）：QuickHull 必须配冲突表；
  GJK+EPA 不依赖绕序。`CollisionComponent` 与 `TransformComponent` 字段名/类型/欧拉序完全一致 →
  手柄只换指针。序列化只存 7 参数，凸包顶点读盘后按 mesh 重建。
  线框走 ImGui draw list（免描述符同步 + 天然 X-ray）。**编辑态碰撞不阻止变换，只在运行态分离**。
  `MYVK_EDITOR_COLLIDER=hull|capsule` 自动化钩子。
- **Player 系统**（第一/第三人称 + 程序化卡通角色）：实体 = 玩家"脚"（贴地 root），
  `PlayerComponent{mode, eyeHeight=1.7, moveSpeed=3.0, lookSpeed=0.0025, yaw, pitch, camDistance=3.5,
  turnSpeed=12, walkPhase, bodyYaw, started}`。第一人称 yaw/pitch = 视线；第三人称 = 轨道方位/俯仰，
  角色朝移动方向转身（最短角差 wrap 插值，`rotation.z = -bodyYaw`，Z-up）。
  ⚠ **Play 态 W/E/R 手柄快捷键必须 `isEditing() && !flying` 守卫**。
- **FBX 导入**（ufbx 双文件库）：`ufbx_matrix` 是 3x4（12 float），memcpy 到 mat4 会越界 → 按列构造。
  **蒙皮核心约定**：蒙皮顶点加载时**烘到静止世界空间**，`invBind(j)=inv(W_rest(j))`、`invMeshNode=I`。
  模型归位：世界 AABB 中心平移回原点（UE 惯例）。**资产端坑**：Blender 导 FBX 声明 Z-up 但数据沿
  +Y → 导出前把 armature 转 90°X；`bake_space_transform` 别用。改 assets/models/*.fbx 后必须手动 cp
  到 build/bin/assets/models（重链才触发拷贝）。CharacterWindow 的 `recordPreview` 必须在 **ImGui 之前**。
- **Hierarchy 文件夹节点**：显式标记 `HierarchyComponent::group`（**不要**靠"没有网格组件"去猜）。
  树的图标不能塞进 `TreeNodeEx` label → label 传空串，图标+名字用 DrawList 自己画
  （`行左边缘 = GetCursorScreenPos().x` **必须在 TreeNodeEx 之前取**）。

## 视口相机与导航（先分清"谁的相机"）
两者的存储模型相同（`scene::Camera` 自由飞行），但归属完全不同：
- **编辑态 = 编辑器视口导航相机**，编辑器完全驱动。
- **Play 态 = 游戏相机**，编辑器**不碰**（视口在 Play 期间不响应任何键鼠导航）。

编辑态导航（按 UE）：右键拖=自由旋转 / 中键拖=平移 / 单独滚轮=推拉相机（`kNavDollyPerNotch=0.6`）/
右键+滚轮=灵敏度 / 右键+WASD=飞行（**只在按住右键时**）/ 右键+Q/E=升降（Shift ×3）/
左键=点选+gizmo / 左下导航球 / 数字键 1/3/7=Front/Right/Top（Ctrl 反向）。
- **滚轮两条路靠"是否按住右键"分流**（复用 `lookDown`，无第三种语义）。
- **"飞行只在按住右键时生效"是必须的**：`W/E/R` 同时是手柄快捷键，`handleShortcuts` 里用
  `const bool flying = IsMouseDown(Right)` 门控。
- `applyEditorCameraMode()` = `setUseGlobalInput(false)`+`setFlyMode(true)`。**每次场景重建必须重设**
  （新建/打开/重建 demo → `onSceneReplaced()`；undo/redo → 各自入口显式调用）。
  **飞行模式不在序列化范围内**，漏掉表现为"导航突然变回老式轨道相机"。
- **导航球**（`ViewportPanel::drawNavGizmo`，Blender 风格）：整块方形是 `InvisibleButton`，
  四角也吞鼠标。切视图/拖拽都**绕当前注视点**转（先存 target，改完 setTarget 钉回去）。
  Z-up 映射：Front=-Y(yaw=+π/2)/Back=+Y/Right=+X(yaw=0)/Top=+Z(pitch=+kPitchLimit)。
  数字键接入点必须在 `io.KeyCtrl` 分支**之前**。主键盘数字键是 `ImGuiKey_1`。
  `Camera::kPitchLimit = 1.5533f`（89°）。
- **Camera 飞行模式不变式**：`position()` 与 `m_target` 通过 `target = position - angleDir()*distance`
  **互为逆运算**。**任何动 target/distance/yaw/pitch 的接口都必须同步另一侧**，否则"拖了没反应/
  画面静止/绕空点打转"。相关入口：`syncFlyTarget()` / `setTarget` / `setDistance` / `setYaw` /
  `setPitch` / `orbit` / `zoomBy` / `panWorld` / `dolly` / `update()`。新增接口先问
  "飞行模式下这条还成立吗"。
- **灵敏度浮层**（`m_navSensitivity`，默认 1，clamp 0.05..20）只作用于编辑态。浮层只保留
  标题 + Sensitivity 输入 + `Ground grid` 勾选框。**浮层必须是 viewport 的 child window**；
  **浮层尺寸必须按字体度量算**（写死像素放大字号后会被裁）。

## Gizmo 拖拽与吸附
**两个曾经的致命缺陷（用户报"拖拽方向反 + 越拖越快"），已修，改这里前必读：**
- **① 轴线/射线最近点的参数符号**（`PickingSystem::closestOnAxis`，Ericson 5.1.9）：
  参数用 `(c*d - b*e) / (b*b - a*c)`，分母 `b*b-a*c` 只用于判平行。**改符号前先想清楚分子分母
  是不是同时翻了。**
- **② 拖拽参考线必须"冻结"**（正反馈环）：`beginDrag` 快照 `m_dragOrigin/m_dragAxis/m_dragWorldLen`，
  `applyDrag` 全程只用快照。用每帧当前位置会指数发散。
- **三套独立吸附**（量化语义各不相同，别统一）：平移=**位移增量**（默认 0.1m）/ 旋转=**角度增量**
  （10°）/ 缩放=**倍率 factor**（0.1，不是缩放绝对值）。UI：视口工具栏第二行 `drawSnapRow()`，
  **G 键**切换（`!flying` 内）。旋转**始终在"拖拽起始姿态"上叠加**（`R=D*R0` 或 `R0*D`）。
- **手柄 7 种 `Handle`**：`AxisX/Y/Z`、`PlaneYZ/XZ/XY`（**枚举名 = 该平面法线**，颜色按法线取）、
  `Screen`。平移/缩放=单轴+平面双轴+中心 Screen；旋转=三轴环+外圈 arcball。
  平面方片命中用 `pointInQuad()`；命中顺序旋转先判三轴环再判外圈。平面拖拽**必须兜底**
  （`kMaxDragUnits=2000`）。
- **本引擎欧拉序 `R = Rx·Ry·Rz`**，而 `glm::eulerAngles` 用 YXZ，**不能直接用** → 自己写
  `eulerFromRotation()`（`y=asin(m[2][0])`、`x=atan2(-m[2][1],m[2][2])`、`z=atan2(-m[1][0],m[0][0])`，
  `|sy|>0.99999` 走万向锁分支）。
- **缩放：拖的那根轴 ≠ 改的那个分量**（`scale` 是局部的，手柄拖的是世界方向）。修法：把拖拽方向
  **投影到局部轴**再分配 `sc[i] = m_startScale[i]*(1+(factor-1)*w[i])`，`w[i]=|dot(axis,localAxisWorld[i])|`。
  **不变量**：不是"拖 Y 改 scale.y"，而是"物体在世界里长大的方向 ≈ 被拖手柄的世界方向"。

## 地平面参考栅格（`grid.{vert,frag}` + `Renderer::GridSettings`）
- **解析式、零顶点缓冲**：VS 用 `gl_VertexIndex` 拼 4 顶点铺在 z=`planeOffset`；FS 按
  `fract(p/step+0.5)-0.5` 求到最近线的距离，除 `fwidth(p)` 转**像素距离** → 1px 抗锯齿，不依赖 MSAA。
- **三种图层**：最小格(1m 暗) → 主格(10m 亮，线宽×1.4) → 世界轴（**X 红 / Y 绿**）。默认 extent60/1/10。
- **自带地面**（`resetToEmptyScene` 建 `Ground`，`castShadow=false`，顶面 z=0 与栅格面重合）。
  改地面尺寸/位置时**必须同步 `GridSettings::extent`**。地面是"上锁"的。
- **淡化是"距栅格中心"不是"距相机"**（用切比雪夫距离 `max(|p.x|,|p.y|)`），样子不随相机移动变化。
- **线宽必须按占空比封顶**（`kMaxLineDuty=0.22`）：`w = min(baseWpx, kMaxLineDuty*cellPx)`；
  亚像素淡出阈值 `smoothstep(0.7,1.6,cellPx)`（旧值 2.5~8.0 太狠 → "贴近地面看栅格会消失"）。
- 与地面共面压 z-fighting：VS 里 `clip.z -= 1e-6*clip.w`。
- 管线：TRIANGLE_STRIP/无顶点布局/depthTest=true/depthWrite=false/blendEnable=true/cullMode=NONE。
- **开关**：`gridEnabled()` 由 `EditorApp::onUpdate` 每帧写 `= isEditing() && ctx.showGrid()`
  → **Play 期间强制关**。UI 三处等价：视口 HUD 勾选框 / View 菜单 / `MYVK_GRID=0`。

## 全屏与窗口几何
- **Play 态全屏** = 窗口真全屏 + 视口独占整屏：`m_gameFullscreen` 只存逻辑状态，
  `EditorApp::syncWindowFullscreen()` → `Window::setFullscreen(true)` → `glfwSetWindowMonitor`。
  `F` 切换 / `Esc` 退出（都只在 Play 态）。**F11 = 只切窗口全屏**（任何模式可用）。
  切换**必须做边沿检测**（`m_appliedWindowFullscreen`），否则每帧无条件 set 会立刻关掉 F11 全屏。
- **窗口几何**（`core/WindowGeometry.{h,cpp}`）：无存档 → 工作区居中、宽高=工作区×0.70；
  有存档 → 用存档（含 maximized），**必须先做可见性校验**（换显示器后存档可能落屏幕外）。
  `onInit()` 第一件事落位、`onShutdown()` 保存。自动化跑不写存档
  （`MYVK_FRAMES`/`MYVK_NO_WINDOW_SAVE` 存在时跳过）。

## venn 命名与图标
窗口标题 `Venn Editor`，日志头 `=== Venn Engine - Editor ===`。图标是**滑稽可爱豚鼠**。
三层独立缺一不可：① exe 资源（`venn.rc` + `enable_language(RC OPTIONAL)`，`.ico` 七档）②
窗口图标（`GLFW_VISIBLE=FALSE` 建窗 → `applyIconFromResource()` → 再 show；三处设
`WM_SETICON` + `SetClassLongPtrW`）③ AUMID（`SetCurrentProcessExplicitAppUserModelID`，
必须在任何窗口创建之前）。
> ⚠ **不要**退回"单张 512 PNG + `glfwSetWindowIcon`"（GLFW 只发 `WM_SETICON`、不设窗口类图标，
> 且默认 `GLFW_VISIBLE=true` 会在设图标前就 ShowWindow 抢跑）。

## 资产溯源与序列化
- Mesh/Material/Texture 各带"来源描述"，序列化只写描述、加载时由 AssetManager 重建；
  路径入 JSON 前过 `makeAssetRelative()`；层级父子用**数组下标**；相机与方向光也在同一 JSON。
- **`Mesh` 带局部 AABB 缓存**（`hasBounds()/boundsMin()/boundsMax()`，`upload()` 时算一次）：
  阴影正交范围、编辑器拾取/贴地都要"物体实际尺寸"，别退回实体原点。
- **`Texture` 色彩空间是契约**：`fromPixels(...,srgb)` / `makeSolid(...,srgb)` —— albedo/emissive
  用 true，**normal/ORM 必须 false**。
- **材质 6 个纹理槽**：albedo/normal/orm/**roughnessMap/metallicMap/emissiveMap**（后三个独立单因子，
  刻意不复用 orm）。roughness/metallic **相乘**（默认纯白=恒等）；emissive **相加**（默认**纯黑**=恒等，
  "加白"是错的）。描述符 `kTextureSetCount 3→6`，setLayouts 4→7。
  落盘的 `name` 就是纹理**缓存键**，数据贴图的 `#linear` 后缀**不能剥**。
- **场景 JSON 的 mesh `size` 只吃标量**（数组会让加载失败）。

---

## 已知坑点（血泪清单 · 只留仍有效的规则）

### 构建 / 环境
1. PowerShell 里 `-DXXX=3.5` 会被拆开 → **必须加引号**（含空格的路径同理）。
2. `Window.h` 里 **vulkan.h 必须在 glfw3.h 之前**（`glfwCreateWindowSurface` 被 `#if VK_VERSION_1_0` 包）。
3. 本环境 PowerShell 输出不回显、bash coreutils 不可靠 → 结果写文件再 Read/Grep。
4. mingw 的 `-Wformat` 不认 `%zu` → 报"参数过多/类型不符"。
5. MinGW 的 `os_defines.h` **已定义 `NOMINMAX`** → 自己再 `#define` 会告警重定义。
6. **改了 GLSL 后 `make build` 不一定把 .spv 拷到 `build/bin/shaders/`**（拷贝是 Editor 目标的
   依赖，改动只有 .spv 时要触发拷贝）。
7. **`GLOB mismatch!` 是 `CONFIGURE_DEPENDS` 的正常提示**，不是错误。

### 视口像素断言（续）
64. **texel snapping 不能对 `view * mid` 量化**：`mid` 是 lookAt 的 target，
    lookAt 后必然落在视轴原点 → `view*mid == (0,0,-dist)` **恒成立**，量化它是
    空操作（开关、日志都"正常"，阴影边缘却照样连续滑动）。必须用"只含旋转"的
    光轴基把球心的**世界位置**投影后量化，再重建 lookAt。见 R3-1。
65. **亚像素级的效果不能靠截图差分验证**：`MYVK_SHADOW_RES=2048` 时一纹素
    ≈0.05 屏幕像素，snap 开/关的截图差分在小数点后 5 位完全相同（降到 256 也
    一样）——因为相机一移动，"整幅画面的视差位移"就把它淹没了。**测理论亚像素
    的现象要断言画面之外的量**（如光空间矩阵的平移列，见 `MYVK_SHADOW_MATRIX_DBG`）。
    硬凑一个像素断言只会做出恒真的假 PASS。

### C++ / 类型
8. `enum class` 底层类型必须是整型；嵌套类型当类型名用要写全限定名。
9. **头文件前向声明的类型，定义不能放匿名命名空间**（否则是另外两个类型）。
10. `std::filesystem::path` **不能**直接赋 `std::string` → 用 `.generic_string()`。
11. 引号 include 先查**当前文件所在目录**。
12. **同一文件的多处修改必须串行下发**（并行 Edit 第二个会静默回滚第一个）。不同文件可并行。
13. `World::clear()` 归零 generation → 旧句柄可能"碰巧" alive；`Scene::clear()` 必须重置 `m_lightEntity`。

### Vulkan
14. 每张交换链图像需**独立** render-finished 信号量；析构时 Swapchain 先于 Surface 销毁。
15. **深度图当纹理采样**用 `DEPTH_STENCIL_READ_ONLY_OPTIMAL`（非 SHADER_READ_ONLY）。
16. **改某条管线的 push constant range = 可能打断整个 Pass 的 set 0 绑定**（VUID-08600）。
17. **DescriptorAllocator 的 0 计数类型不能进 poolSize**（VUID-00302）。
18. **ping-pong 屏障的 oldLayout 要用 UNDEFINED**（单 pass 链里 k=1 工作图）。
19. **全局描述符引用的图（如 SSAOPass）重建后必须 `writeGlobalDescriptors`**。
20. `vkGetPhysicalDeviceFeatures2` 在 apiVersion=1.0 的实例上是无效调用。
21. **blit 链生成 mip 时初始 barrier 必须全层 UNDEFINED→TRANSFER_DST**；流式纹理的未传层也要转
    SHADER_READ（验证层按描述符视图**全部 subresource** 校验）。
22. **`glm::ortho` ≠ Vulkan 深度**：用 `glm::orthoRH_ZO`（或定义 `GLM_FORCE_DEPTH_ZERO_TO_ONE`）。

### GPU 时戳 / 查询
23. **被 reset 却没写入的 query 会永远 unavailable，拖累整批读回**。
24. **fence 初始是 signaled 的，头几帧读 query 违反 VUID-09401**。
25. **`timestampPeriod`/位数是队列族属性不是设备属性**。

### ImGui / 编辑器自动化
26. `ImGui::BeginDragDropSource()` 默认要求上一个 item 有非零 ID → 无 ID 的 item 必须传
    `SourceAllowNullID`（否则 `IM_ASSERT(0)` 崩，退程码 `0xc0000409`）。
27. **`ImGui::Dummy()` 天然就是拖放落点**（ItemAdd 传的 ID 是 0）。
28. **ImGui 每帧把聚焦窗口提到最前** → 视口上的 HUD/覆盖层**必须是 viewport 的 child window**。
29. **ImGui 的 popup 必须"每帧都被提交"**，否则帧末会被直接关掉。
30. **`ImGui::BeginMenu` 只有子菜单展开才返回 true** → 配的 logRect 必须写在子菜单内部。
31. **`ImGui::EndGroup()` 会 `ItemAdd`**（带自动 ID），`BeginDragDropTarget()` 在其后仍可用。
32. `ImGui::SameLine(offset)` 的 offset 是**相对窗口内容起点**，不是相对光标。
33. **固定像素尺寸的浮层会被字号缩放裁掉** → 用 `CalcTextSize` 等字体度量算尺寸。
34. **视口工具栏已经很长**：加按钮会被挤出右边缘裁掉 → 先想"能不能挪到浮层/主工具栏"。
35. `discreteEdit` 的契约是"`*target` 已持有新值，第 4 参 = 旧值"，它只压撤销栈、不赋值。
36. **序列化 `applyCamera` 的 setter 顺序：target 必须最后设**（fly 模式下）。
37. **`logRect` 是按矩形去抖的 → 弹出物的 tag 必须带"第几次打开"的序号**；
    正则要允许小写 tag。
38. imgui master(≥1.93) InitInfo 变了；无 docking 分支 → `ImGuiWindowFlags_NoDocking` 未定义。
39. **自动化跑必须把 ImGui 的 `imgui.ini` 一起关掉**（`io.IniFilename = nullptr`）。
40. **自动化启动别读窗口几何存档**（存档可能是上次手动会话留下的全屏态）。
41. **解析自家日志的正则别用行首锚定**（日志有 `[INFO ] ` 前缀）。
42. **进程 stdout 是块缓冲**：日志只有前 24 行 ≠ 进程卡死。
43. **自动化 `SetForegroundWindow` 会静默失败**（窗口不在前台时 ImGui 收不到输入）→
    要加 attach-thread-input 兜底；`SetWindowPos` 后光标会被外部挪走。
44. **自动化注入扩展键（Delete/方向键等）用 `keybd_event` 必须同时给扫描码**；
    GLFW 键值按 scancode 查表 + 修饰键走 `GetKeyState`。
45. **验证脚本里"注入的第一次左键点击会被 Windows 拿去做窗口激活"**，ImGui 收不到。
46. **自动化注入 Shift+A 的唯一稳法**：ALT-trick 前台确认 + 真实扫描码。
47. **验证层静默是一等验收项**：`grep -c "VUID\|ERROR"` 必须为 0。

### 颜色 / 贴图
48. **数据贴图必须 UNORM**：normal/ORM 用 `srgb=false`，颜色贴图才 `srgb=true`。
49. **ImGui 颜色经 SRGB 交换链后会明显变亮**（`ImVec4(0.62,0.24,0.22)` 屏幕上约是 0.34 亮度）。
50. **同一张图在不同色彩空间必须是两份 GPU 纹理 → 缓存键要编进 sRGB**。
51. **"因子 × 贴图"的默认值必须是数学中性值**（roughness/metallic 未绑定时绑纯白；
    emissive 绑纯黑）。**别用"加白"**。
52. **缺省贴图**：`createDefaultTextures()` 的白色(ORM)与平坦法线**必须 `srgb=false`**，
    否则平坦法线 (128,128,255) 被硬件 gamma 解码 → 被扳约 39° → 大面片整块变色。

### 命名 / 图标 / 其它
53. **环境的钩子要注意初始化顺序**：`MYVK_EDITOR_CAM` 必须放在 `MYVK_EDITOR_SCENE` **之后**。
54. **"可被选中/可被变换"要显式声明**（用标记组件，别靠约定）。
55. **`MYVK_EDITOR_CAM` 的 pitch 是俯角（正=向下）**，且 setTarget 轨道重摆后要重设。
56. **脚本的 `readJoint` 等辅助函数未使用会有告警**；tinyobjloader 头里的 unused 函数告警无害。
57. **改项目根目录名会被"以它为 cwd 的进程"挡住**（`mv` 报 busy）→ 先关进程。
58. **`ContentBrowser` 面板会扫描进程 CWD 的所有图片做缩略图** → 别在项目根目录乱丢验证截图。
59. **自动控制器不要给用户给的"目标值"做无理由的下限夹紧**。
60. **动态分辨率必须把"输出分辨率"与"场景渲染分辨率"彻底分开，且两处一致**；
    **离屏输出图（ImGui 采样那张）是全分辨率的，判"能否复用"要用它**。
61. **"关闭 = 基线逐像素不变"在着色器里也要落实到位**（用 0 判断而不是宏）。
62. **glslang：`#extension` 必须在所有非预处理语句之前**。
63. **bindless 纹理数组的容量必须按设备收窄，不能硬写**；材质页表必须**按帧槽分离**，
    否则会撕裂在途读取。
64. **`#ifdef` 判不出"默认定义为 0 的编译开关"** → 用 `#if` 或 `constexpr`。

## 工具
- `tools/run_regression.py`：总回归，逐脚本跑、读末尾 `结果: PASS`；**增量**（跳过"上次 PASS
  且产物未变"的）。新增验证脚本必须加进它的 `DEFAULT` 列表。
- 各类 `tools/verify_*.py`：CSM / local shadow / shadow(hardening) / nav_gizmo / gizmo_scale /
  collider_menu / collider_gizmo / collision_play / character_window / add_menu / csm / lod /
  occlusion / mdi … 每个都以 `结果: PASS` 结尾。
- 共用设施：`capture_window.py`（`capture_client`/`find_window`/`user32`，默认注入
  `MYVK_NO_WINDOW_SAVE=1`）、`verify_add_menu.py`（`LogTap`/`kill_stale_editors`/`wait_rect`）。
- 截图输出目录：`<repo>/../_venn_verify`（本机 `G:\GameEngine\_venn_verify`）。
- Python 解释器：`D:\env\python\python.exe`（3.12），已装 Pillow（走 `-i https://pypi.org/simple`）。
- `make smoke` = 跑 `MYVK_FRAMES=N` 冒烟；`make package` = `cmake -E tar` 打包。
- 子系统日志：`SHADOW-DBG`/`SHADOW-STATS`/`LOCAL-SHADOW-*` / `ENV-DBG`/`ENV-READBACK` /
  `BVH-DBG` / `LOD-BUILD`/`LOD-STATS`/`LOD-SWITCH` / `CLUSTER-*` / `SHADER-VARIANTS` /
  `RT-TARGETS` / `PLAYER-STATE` / `COLLIDER-SIG` / `VP-RECT` / `NAV-CAM`。
- `MYVK_LOG_RECTS=1`：打印关键控件矩形（给自动化定位）。
